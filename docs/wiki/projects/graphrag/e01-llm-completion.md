---
project: graphrag
feature: llm-completion
title: 补全抽象与工厂
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag-llm/graphrag_llm/completion/completion.py
    symbols: [LLMCompletion]
  - path: packages/graphrag-llm/graphrag_llm/completion/completion_factory.py
    symbols: [CompletionFactory, register_completion, create_completion]
  - path: packages/graphrag-llm/graphrag_llm/embedding/embedding.py
    symbols: [LLMEmbedding]
  - path: packages/graphrag-llm/graphrag_llm/utils/completion_messages_builder.py
    symbols: [CompletionMessagesBuilder]
---

# 补全抽象与工厂

## 一句话职责

把「调一次模型」抽象成一个**不带任何横切关注点**的纯函数（`completion` / `completion_async`），把限流、重试、缓存、指标、日志全部推到外层中间件，再让工厂按 `ModelConfig.type` 装配成一个可直接使用的实例。

## 机制是什么

graphrag 的 LLM 层有三个正交的切分，混在一起就理解不了：

| 切分 | 两侧 | 判据 |
|---|---|---|
| 能力 | `LLMCompletion` 与 `LLMEmbedding` | 产出是文本还是向量 |
| 实现 | 抽象基类与 `LiteLLMCompletion` / `MockLLMCompletion` | 真的调 provider 还是返回固定值 |
| 关注点 | 核心方法与中间件 | 「调模型」与「管调用」 |

**第二与第三条切分的组合方式是这一页的核心**：抽象基类只声明 `completion` 与 `completion_async` 两个**裸方法**，而所有「一次调用之外的事」都由 `with_middleware_pipeline` 包在外面（见 e02 那页）。于是子类的实现里看不到任何缓存或限流代码。

**`LLMCompletion` 与 `LLMEmbedding` 是两个平行的抽象但不是两套代码。** 它们的构造参数逐字相同（九个关键字参数），都提供 `*_thread_pool` 上下文管理器与 `*_batch` 批量方法，都暴露 `metrics_store` / `tokenizer` 两个抽象属性。**差别只在核心方法的签名与返回类型。**

## 怎么实现的

### 一、抽象基类：九个参数里有五个是横切的

**构造签名（`__init__`）声明了九个关键字参数，而它们全部是 `@abstractmethod`。**

```
def __init__(
    self,
    *,
    model_id: str,
    model_config: "ModelConfig",
    tokenizer: "Tokenizer",
    metrics_store: "MetricsStore",
    metrics_processor: "MetricsProcessor | None" = None,
    rate_limiter: "RateLimiter | None" = None,
    retrier: "Retry | None" = None,
    cache: "Cache | None" = None,
    cache_key_creator: "CacheKeyCreator",
    **kwargs: Any,
):
    raise NotImplementedError
```

**为什么构造也要是抽象方法？** 因为它不是「给子类一个默认实现」的钩子，而是**一份必须被遵守的契约声明**：任何子类都必须接受这九个参数，因为工厂会按这个名字逐个传（见下）。

**九个参数里只有三个是「这个模型是什么」**（`model_id`、`model_config`、`tokenizer`），其余六个都是横切设施：指标存储、指标处理器、限流器、重试器、缓存、缓存键生成器。**而它们全部可为 `None`** —— 这是「关闭某个中间件」的表达方式，且关闭是在**装配环节**决定的，不在核心方法里判断。

**核心方法只有两个，且签名完全相同。**

```
def completion(self, /, **kwargs: Unpack["LLMCompletionArgs[ResponseFormat]"]) -> "LLMCompletionResponse[ResponseFormat] | Iterator[LLMCompletionChunk]"
async def completion_async(self, /, **kwargs: Unpack["LLMCompletionArgs[ResponseFormat]"]) -> "LLMCompletionResponse[ResponseFormat] | AsyncIterator[LLMCompletionChunk]"
```

**三个细节：**

1. **`/` 之前没有参数** —— 位置参数被全部封禁，于是所有调用必须是关键字形式。这是必需的，因为中间件是**层层透传 `**kwargs`** 的包装，位置参数无法穿过这种包装。
2. **`Unpack[TypedDict]`** —— 参数集合被声明成一个 `TypedDict` 并整体展开。于是「有哪些参数」是**一个有名字的类型**，而不是散落在各处的 `**kwargs: Any`。
3. **返回类型是联合**：流式返回迭代器、非流式返回完整响应。**调用方必须自己判断拿到的哪种** —— 这是一个刻意的取舍（见下面的「关键取舍」）。

**`completion` 与 `completion_async` 是并列关系，不是「一个包装另一个」。** 两者都 `raise NotImplementedError`，各自独立实现。理由在中间件那层看得出来：中间件为同步与异步**各包一份**，所以底层也必须是两份同构实现，不能异步转发同步。

### 二、基类里唯一有实现的方法：线程池与批量

**`completion_thread_pool` 是一个 `@contextmanager`，而它把 `self.completion` 本身作为参数传给了 runner。**

```
@contextmanager
def completion_thread_pool(self, *, response_handler, concurrency, queue_limit=0):
    with completion_thread_runner(
        completion=self.completion,
        response_handler=response_handler,
        concurrency=concurrency,
        queue_limit=queue_limit,
        metrics_store=self.metrics_store,
    ) as completion:
        yield completion
```

**注意传的是 `self.completion`——绑定了实例的方法，也就是「已包过中间件的那个」**（若是子类实例，见 `LiteLLMCompletion` 的构造）。于是线程池里的每次调用都经过完整的中间件链：限流器在多线程下正确工作（它内部有锁，见 e03），缓存命中在多线程下也生效。

**而 `metrics_store` 被单独传进去** —— 它不属于「一次补全」，而属于「这一整批的运行时长」。

**`completion_batch` 用「预分配数组 + 按 request_id 回填」把异步回调收成同步返回值。**

```
responses: list[...] = [None] * len(completion_requests)

def handle_response(request_id: str, resp: ...):
    responses[int(request_id)] = resp

with self.completion_thread_pool(response_handler=handle_response, concurrency=..., queue_limit=...) as threaded_completion:
    for idx, request in enumerate(completion_requests):
        threaded_completion(request_id=str(idx), **request)

return responses
```

**`request_id` 就是索引的字符串形式** —— 这是「把 `request_id` 定义为调用方给的任意字符串」这个设计的直接利用：批量方法把它当数组下标用，于是**返回顺序与请求顺序严格对应**，不必排序也不必等到全部完成再匹配。

**而返回值里允许出现 `Exception`** —— 类型是 `list[Response | Iterator | Exception]`。**单个请求失败不会让整批失败**，调用方逐项检查。这与 `derive_from_rows`（g02 那页）相反：那里任何一行失败都会让整体抛 `ParallelizationError`。

**两者的差别有理由**：批量补全的每个请求是**独立的模型调用**（成功一个算一个，自然适合「部分成功」）；而工作流里的一行失败意味着**这一步的产物不完整**，继续下去只会产出一个缺行的索引。

**返回值是**列表而不是生成器**，意味着调用必须等全部完成。** 若并发度是 4 而请求有 100 个，`completion_batch` 会一直阻塞到最后一批。而 `completion_thread_pool` 那个上下文管理器形式允许边提交边在回调里处理 —— 这是「批量」与「流式消费」的两种用法。

### 三、工厂：按 `type` 字符串懒注册

**`CompletionFactory` 是空的，只继承 `Factory`。**

```
class CompletionFactory(Factory["LLMCompletion"]):
    """Factory for creating Completion instances."""

completion_factory = CompletionFactory()
```

**这行「空类」不是占位** —— 它给泛型参数一个具体类型，让静态检查知道 `completion_factory.create(...)` 返回 `LLMCompletion` 而不是 `Any`。而实际逻辑全在 `graphrag_common.factory.Factory`（含 `register` / `create` / `__contains__` / `keys`）。

**`create_completion` 的第一步是「没注册就先注册」。**

```
if strategy not in completion_factory:
    match strategy:
        case LLMProviderType.LiteLLM:
            from graphrag_llm.completion.lite_llm_completion import LiteLLMCompletion
            register_completion(completion_type=LLMProviderType.LiteLLM, completion_initializer=LiteLLMCompletion, scope="singleton")
        case LLMProviderType.MockLLM:
            from graphrag_llm.completion.mock_llm_completion import MockLLMCompletion
            register_completion(completion_type=LLMProviderType.MockLLM, completion_initializer=MockLLMCompletion)
        case _:
            msg = f"ModelConfig.type '{strategy}' is not registered in the CompletionFactory. Registered strategies: {', '.join(completion_factory.keys())}"
            raise ValueError(msg)
```

**四个要点：**

1. **`import` 在 `case` 分支内部** —— 内置实现是**懒加载**的。理由很实在：`LiteLLMCompletion` 依赖 `litellm`（一个很大的库），而若只是用 `MockLLMCompletion` 做测试，不该为它付导入代价。
2. **`scope="singleton"` 只给 LiteLLM** —— 见下。
3. **兜底分支的消息里列出已注册的 strategy** —— 这让「拼错类型名」的排查一眼可见，而不是只报「不支持」。
4. **`register_completion` 是公开函数**，第三方可以注册自己的实现。**而这就要求「必须能按 `type` 找回来」** —— 于是类型名是一个**开放的字符串空间**，不是封闭的枚举。

**`scope` 的差别值得单独说。** `Factory.create` 的 `scope` 决定实例是否被缓存复用：`singleton` 全局一个、`transient` 每次新建。而 **LiteLLM 是单例、Mock 是 transients**：

- 真实 provider 的连接、HTTP 客户端池、限流器的滑动窗口队列都**必须跨调用共享**——否则每次新建补全实例都会得到一个新的限流器，限流就形同虚设。
- 而 Mock 是需要**每次干净**的（测试之间不该共享状态）。

**这个区分说明了一件事：`scope` 不是性能选项，而是正确性选项。** 若把 LiteLLM 改成 `transient`，限流器与缓存的语义都会被破坏。

**工厂的其余职责是「把配置翻译成实例」。**

```
cache_key_creator = cache_key_creator or create_cache_key
model_id = f"{model_config.model_provider}/{model_config.model}"
strategy = model_config.type
extra: dict[str, Any] = model_config.model_extra or {}
...
tokenizer = tokenizer or create_tokenizer(TokenizerConfig(model_id=model_id))
```

**`model_id` 是「provider/model」拼出来的** —— 这个字符串会进指标（区分不同模型的花费）、进缓存键（见 e04）、进容器名（向量存储那边）。

**四个可选组件按「配置里有没有」决定建不建：**

```
rate_limiter = create_rate_limiter(rate_limit_config=model_config.rate_limit) if model_config.rate_limit else None
retrier = create_retry(retry_config=model_config.retry) if model_config.retry else None
metrics_store = NoopMetricsStore()
if model_config.metrics:
    metrics_store = create_metrics_store(config=model_config.metrics, id=model_id)
    metrics_processor = create_metrics_processor(model_config.metrics)
```

**注意 `metrics_store` 的默认值是 `NoopMetricsStore()` 而不是 `None`** —— 因为抽象基类声明它是**非可选的抽象属性**（`def metrics_store(self) -> "MetricsStore"`）。若允许 `None`，每个消费者都要判空。**用 `NoopMetricsStore` 当默认，就把「没配指标」变成了「有一个什么都不做的指标存储」**，于是调用点的代码只有一种形态。

**这与 `NoopCache` 是同一个手法**（见 f01 那页），而它与 `metrics_processor` 的默认 `None` 形成对照：**存储必须有值（因为它是可写的汇点）、处理器可以为空（因为它是可选的增强）**。

**最后一步把所有东西塞进 `init_args`。**

```
return completion_factory.create(
    strategy=strategy,
    init_args={
        **extra,
        "model_id": model_id, "model_config": model_config, "tokenizer": tokenizer,
        "metrics_store": metrics_store, "metrics_processor": metrics_processor,
        "rate_limiter": rate_limiter, "retrier": retrier,
        "cache": cache, "cache_key_creator": cache_key_creator,
    },
)
```

**`**extra` 在最前面** —— 于是 `model_extra` 里的未知键会被传下去，但**被九个具名参数覆盖**（Python 的字典字面量后写的键赢）。这保证了「配置里的杂项字段可以透传给自定义实现，但不能劫持标准参数」。

### 四、`LLMEmbedding`：逐字对称的第二套抽象

**对比两个基类的构造签名，会发现它们完全相同。**

两者的九个参数、`*_thread_pool` 的结构、`*_batch` 的实现、两个抽象属性——全部对齐，只有核心方法名不同（`completion` / `embedding`）与返回类型不同。

**这种「复制而非抽取公共基类」是一个值得记的取舍。** 若抽一个 `LLMCall` 基类共享构造与批量逻辑，就要处理「核心方法叫什么」的分派——而那个分派会让类型检查失效（返回类型取决于子类）。

**代价是双份代码要同步维护**；收益是**两套类型各自的签名精确**（`LLMCompletionArgs` / `LLMEmbeddingArgs` 是不同 `TypedDict`，返回类型也不同）。

**而 `embedding_batch` 的返回类型是 `list[LLMEmbeddingResponse | Exception]`** —— 比补全版少了「迭代器」那一支，因为嵌入没有流式。

**`embedding_thread_pool` 传的是 `self.embedding`** —— 同样已包过中间件。于是**限流器对嵌入与补全共享同一套机制**（若它们配了同一个限流器实例）。

### 五、`CompletionMessagesBuilder`：把「消息数组」变成可校验的构造过程

**它的接口是流式构建器：每个 `add_*` 返回 `self`。**

```
def add_system_message(self, content, name=None) -> "CompletionMessagesBuilder":
    if name:
        self._messages.append(ChatCompletionSystemMessageParam(role="system", content=content, name=name))
    else:
        self._messages.append(ChatCompletionSystemMessageParam(role="system", content=content))
    return self
```

**六种消息类型各有专用方法**：`system` / `developer` / `tool`（需 `tool_call_id`）/ `function`（需 `function_name`）/ `user` / `assistant`。

**这不是「一个通用 `add(role, content)`」能替代的** —— 因为每种消息的**必填字段不同**：`tool` 消息必须有 `tool_call_id`，`function` 消息必须有 `name`，而 `system` 的 `name` 是可选的。**把这些差异编码进方法签名，就让「编排一条 tool 消息却忘了 `tool_call_id`」变成参数缺失而不是运行期报错。**

**`add_assistant_message` 是唯一接受「一条已有模型响应」的方法。**

```
args = {
    "role": "assistant",
    "content": message if isinstance(message, str) else message.content,
    "refusal": None if isinstance(message, str) else message.refusal,
}
if name: args["name"] = name
if not isinstance(message, str):
    if message.function_call: args["function_call"] = message.function_call
    if message.tool_calls: args["tool_calls"] = message.tool_calls
    if message.audio: args["audio"] = message.audio
self._messages.append(ChatCompletionAssistantMessageParam(**args))
```

**三个「只在非字符串时才设」的条件。** 这是为了支持「多轮对话把上一轮响应直接接进来」：`ChatCompletionMessage` 带 `function_call` / `tool_calls` / `audio`，若丢了这些，下一轮就会因为「tool_calls 没有对应响应」而被 provider 拒绝。

**而 `refusal` 无条件设置**（字符串时设 `None`）—— 说明它是一个**必须存在**的字段（而非可选），即使值为空。

**`build()` 返回内部列表本身而不是副本。**

```
def build(self) -> "LLMCompletionMessagesParam":
    return self._messages
```

**调用方拿到的是引用** —— 于是之后继续 `add_*` 会改变已 `build()` 的结果。这是一个**有意省掉的防御性拷贝**（组合根上构建一次就发出去，不需要快照语义），但也是一个容易踩的点。

**`CompletionContentPartBuilder` 是同构的多模态版本。** `add_text_part` / `add_image_part(url, detail)` / `add_audio_part(data, format)` —— 对应 OpenAI 的三种 content part 类型。而 `detail` 参数是 `Literal["auto", "low", "high"]`，没有默认值：**清晰度必须被显式选择**，因为它直接影响 token 消耗与费用。

**两个构建器的用例配合是固定的**：外层 `add_user_message(completion_content_part_builder.build())`，内层逐段添加。于是「一条消息里有图文混排」被表达成「内层构建器的产物作为外层的 `content`」。

## 关键取舍

**源码事实：** `LLMCompletion` / `LLMEmbedding` 的构造签名完全相同、九个关键字参数、全部为 `@abstractmethod`；核心方法用 `Unpack[TypedDict]` 声明参数并禁止位置参数（`/`）。

**源码事实：** 同步与异步核心方法并列实现，互不转发——因为中间件为两者各包一份，底层也必须有两份同构实现。

**源码事实：** 构造的九个参数里六个是横切设施（指标存储/处理器、限流器、重试器、缓存、缓存键生成器），全部可为 `None`；「关闭中间件」的决定发生在装配环节而非核心方法内部。

**源码事实：** `metrics_store` 默认 `NoopMetricsStore()` 而非 `None`，因为抽象基类把它声明为非可选属性；而 `metrics_processor` 默认 `None`。存储是必有的汇点，处理器是可选的增强。

**源码事实：** `completion_thread_pool` 传给 runner 的是 `self.completion`（已包中间件的绑定方法），于是线程池里每次调用都经过完整中间件链；`metrics_store` 单独传入，因为它量化的是整批的运行时长。

**源码事实：** `completion_batch` 用「预分配数组 + `request_id` 即索引」实现顺序对应，且返回值允许含 `Exception`——单请求失败不影响整批。这与 `derive_from_rows` 的「任一失败即整体抛错」相反，因为模型调用是独立的、而工作流的一行失败意味着这一步产物不完整。

**源码事实：** 内置实现按 `case` 懒加载（`import` 在分支内），因为 `LiteLLMCompletion` 会拖入庞大的 `litellm` 依赖。

**源码事实：** `scope` 对 LiteLLM 是 `singleton`、对 Mock 是 `transient`。这不是性能选项而是正确性选项：限流器的滑动窗口与缓存必须跨调用共享，否则每次新建实例都会得到新的限流器。

**源码事实：** 工厂里 `**extra` 排在九个具名参数之前，于是 `model_extra` 的未知键会透传但不能覆盖标准参数。

**源码事实：** 未注册类型的错误消息列出所有已注册 strategy，让拼写错误一眼可见。

**源码事实：** `CompletionMessagesBuilder` 为六种消息类型各有专用方法（`tool` 必须给 `tool_call_id`、`function` 必须给 `function_name`），而 `build()` 返回内部列表的引用而非副本。

**源码事实：** `add_assistant_message` 只在非字符串输入时才设 `function_call` / `tool_calls` / `audio`，因为多轮对话若不回传这些字段，provider 会因「tool_calls 没有对应响应」拒绝；而 `refusal` 无条件设置（可为 `None`）。

**源码事实：** `add_image_part` 的 `detail` 没有默认值，必须显式选择 `auto` / `low` / `high`。

**架构推论：** 「核心方法只做一件事」这个分立方式让**每个横切关注点都可以独立开关、独立替换、独立组合**。代价是核心方法的 `**kwargs` 里混着「给模型的」与「给中间件的」两类参数（`metrics` 参数就是后者，见 e02 那页的 `kwargs.pop("metrics")`）。**当一个函数既是业务接口又是中间件的通信通道时，它的签名就必然混合两种词汇** —— 这是这种架构不可避免的成本。

**架构推论：** 返回类型用联合（响应 | 迭代器）而不是分成 `completion` 与 `completion_stream` 两个方法，意味着**「是否流式」是运行期参数而不是类型区分**。好处是调用方能统一处理「配置里说流式就流式」；坏处是每个调用点都要 `isinstance` 判断，而**类型系统帮不上忙**。这是把灵活性优先于类型安全的选择。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag-llm/graphrag_llm/completion/completion.py` | `LLMCompletion` | 补全抽象：两个核心方法 + 线程池 + 批量 |
| `packages/graphrag-llm/graphrag_llm/completion/completion.py` | `completion_thread_pool` | 上下文管理器形式的线程池（可边提交边消费） |
| `packages/graphrag-llm/graphrag_llm/completion/completion.py` | `completion_batch` | 批量补全，`request_id` 即索引 |
| `packages/graphrag-llm/graphrag_llm/completion/completion_factory.py` | `CompletionFactory` | 泛型绑定的工厂（逻辑在 `graphrag_common`） |
| `packages/graphrag-llm/graphrag_llm/completion/completion_factory.py` | `register_completion` / `create_completion` | 公开注册入口与按配置装配 |
| `packages/graphrag-llm/graphrag_llm/completion/lite_llm_completion.py` | `LiteLLMCompletion` | 真实 provider 实现（单例 scope） |
| `packages/graphrag-llm/graphrag_llm/completion/mock_llm_completion.py` | `MockLLMCompletion` | 测试用实现（transient scope） |
| `packages/graphrag-llm/graphrag_llm/embedding/embedding.py` | `LLMEmbedding` | 与补全逐字对称的嵌入抽象 |
| `packages/graphrag-llm/graphrag_llm/utils/completion_messages_builder.py` | `CompletionMessagesBuilder` | 六种消息类型的专用构造方法 |
| `packages/graphrag-llm/graphrag_llm/utils/completion_messages_builder.py` | `CompletionContentPartBuilder` | 多模态内容段（文本/图像/音频） |
| `packages/graphrag-llm/graphrag_llm/threading/completion_thread_runner.py` | `completion_thread_runner` | 线程池实现（e05 详述） |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：LLM 配置](https://microsoft.github.io/graphrag/config/yaml/)
- [LiteLLM 文档](https://docs.litellm.ai/)
- [graphrag 源码：completion.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag-llm/graphrag_llm/completion/completion.py)

## 与主核 / AI 的关系

这套抽象与主核的**可插拔存储引擎接口**在结构上完全同构：一个薄的核心契约（`completion` / `embedding`）+ 一个按字符串类型名解析的工厂 + 一堆通过配置注入的横切设施。

**差别在「横切的处理位置」。** 数据库的存储引擎接口把事务、锁、缓冲管理**留给引擎自己实现**——因为它们是正确性的一部分，不能由外部包裹。而这里把限流、重试、缓存全部推到外层，因为它们是**可以逐层叠加的增强**，且不改变语义（缓存命中与真实调用应当返回等价的结果）。

**「什么样的关注点可以中间件化」这个问题有明确的判据**：能被中间件包裹的操作必须满足**「包装后语义不变」**。缓存命中返回旧结果、重试返回第一次成功的结果、限流只是延迟——这些都保持「一次调用的语义」。而事务不行：你不能在存储引擎外面包一层事务，因为事务要求引擎内部配合（回滚要真的回滚）。

**`scope="singleton"` 与数据库的连接池是同一个考虑。** 连接的复用不是性能优化而是语义要求（同一个事务必须在同一连接上）。限流器的共享同理：**「租约」这个状态必须只有一个持有者**，否则配额被乘以实例数。

**`NoopMetricsStore` 与主核的「无操作统计收集器」**同构：把「没启用」表示成一个真实但空实现的对象，能让所有调用点只有一种代码路径。这比在每个调用点判空更不容易出错——**判空遗忘的后果是崩溃，而空实现遗忘的后果是不统计**。

对 agent 工程的直接价值有三条。其一，**核心方法里不该有任何横切代码**。一旦在 `completion` 里写了「先查缓存」，缓存就无法被单独关闭、无法被替换、无法被测试——而推到外层之后，`MockLLMCompletion` 可以完全不做缓存相关的事。其二，**实例作用域是正确性问题而非性能问题**。限流器、缓存、连接池必须单例，否则每个新实例都会重置状态。其三，**用空实现代替 `None`**。默认值一个 `NoopX` 能让下游少写一半的判空，而它什么都不做的事实不影响任何语义。
