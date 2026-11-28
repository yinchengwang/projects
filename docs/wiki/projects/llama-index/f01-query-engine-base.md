---
project: llama-index
feature: query-engine-base
title: 查询引擎契约
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/base/base_query_engine.py
    symbols: [BaseQueryEngine]
  - path: llama-index-core/llama_index/core/query_engine/retriever_query_engine.py
    symbols: [RetrieverQueryEngine]
---

# 查询引擎契约

## 一句话职责

定义一个查询引擎必须提供什么（`query` / `aquery` 两个抽象入口）、可以额外提供什么（`retrieve` / `synthesize` 两个**默认抛 `NotImplementedError`** 的可选能力），以及 `RetrieverQueryEngine` 如何把「检索 → 后处理 → 合成」这条标准三段串成默认实现。

## 机制是什么

`BaseQueryEngine` 只有 93 行，它把接口分成三层：

| 层 | 方法 | 谁必须实现 |
|---|---|---|
| 唯一必需 | `_query` / `_aquery` | **子类必须实现（`abstractmethod`）** |
| 门面 | `query` / `aquery` | 基类实现：包 trace、把 `str` 转 `QueryBundle`、发仪表事件 |
| 可选分解 | `retrieve` / `synthesize` / `asynthesize` | **基类实现为「抛 `NotImplementedError`」** |

`RetrieverQueryEngine` 是那个「把可选分解全部实现」的标准子类——它把 `_query` 写成 `retrieve()` + `synthesize()`。

## 怎么实现的

### 一、门面层：三件事是固定顺序的

**`query` 的实现只有五句，但每句都有理由。**

```
@dispatcher.span
def query(self, str_or_query_bundle: QueryType) -> RESPONSE_TYPE:
    dispatcher.event(QueryStartEvent(query=str_or_query_bundle))
    with self.callback_manager.as_trace("query"):
        if isinstance(str_or_query_bundle, str):
            str_or_query_bundle = QueryBundle(str_or_query_bundle)
        query_result = self._query(str_or_query_bundle)
    dispatcher.event(QueryEndEvent(query=str_or_query_bundle, response=query_result))
    return query_result
```

1. **`@dispatcher.span` 与两个 `dispatcher.event` 是两套不同的仪表机制**。`span` 是分布式的 span 树（用于追踪耗时层级），`event` 是离散事件（start/end 各一条）。**同一个方法同时上两种**，说明这两套机制服务不同消费者：span 树给性能剖析、事件给日志/回调。
2. **`QueryStartEvent` 在 `as_trace` 之前发出，`QueryEndEvent` 在之后**。也就是说这两个事件**不在 trace 里**——它们标记的是「整个查询」这个边界，而不是 trace 内的一步。
3. **`str` → `QueryBundle` 的转换在 trace 内部完成**，但 `QueryEndEvent` 用的是转换后的 bundle。这也是为什么 `QueryStartEvent` 用的是**转换前**的原始参数——它忠实记录了调用方传了什么。
4. **`async` 版是完整的镜像**，只差一个 `await`。两份代码逐句相同——这是全篇最明显的重复，而它换来的是「同步调用者不必写 async、异步调用者不必写同步桩」。

**`as_trace("query")` 是「把这个块包成一个可命名的回调区间」。** 于是所有内部事件的回调都归属于名为 `query` 的 trace，而不是散在顶层。

### 二、可选分解：默认就是「不支持」

**三个方法（`retrieve` / `synthesize` / `asynthesize`）在基类里的实现全是抛错。**

```
def retrieve(self, query_bundle: QueryBundle) -> List[NodeWithScore]:
    raise NotImplementedError("This query engine does not support retrieve, use query directly")
```

三条错误消息的措辞值得注意——它们都**给出了替代方案**：

| 方法 | 消息里的指引 |
|---|---|
| `retrieve` | `use query directly` |
| `synthesize` | `use query directly` |
| `asynthesize` | `use aquery directly` |

这是一个「不支持的接口要告诉调用方怎么绕」的正确做法。对比一个只说 `NotImplementedError()` 的实现：调用方得去读源码才知道该用什么。

**把这三个做成「默认抛错」而不是「抽象方法」是一个明确的取舍。** 若它们是 `abstractmethod`，**每一个查询引擎都必须实现它们**——但很多引擎（比如一个纯模板填充引擎、或一个路由引擎）本质上没有「检索」与「合成」这两个可分开的阶段。让它们必须写三个 `raise NotImplementedError` 是纯粹的负担。

而代价是：**调用方无法从类型上判断一个引擎是否支持分解**。若代码想「先 retrieve 再合成自定义的回答」，必须 try/except 或事先知道具体类型。这是一个「用运行期错误换取子类负担降低」的标准交换。

### 三、`RetrieverQueryEngine`：标准三段

**`_query` 的实现是这一页的核心，而它的结构很简单。**

```
@dispatcher.span
def _query(self, query_bundle):
    with self.callback_manager.event(CBEventType.QUERY, payload={EventPayload.QUERY_STR: query_bundle.query_str}) as query_event:
        nodes = self.retrieve(query_bundle)
        response = self._response_synthesizer.synthesize(query=query_bundle, nodes=nodes)
        query_event.on_end(payload={EventPayload.RESPONSE: response})
    return response
```

注意三件事：

1. **它调的是 `self.retrieve(...)` 而不是 `self._retriever.retrieve(...)`。** 这一点是关键：`retrieve` 里包含了后处理链，所以**后处理器是「检索」这个概念的一部分**，而不是一个额外的阶段。于是 `synthesize` 收到的永远是处理完的节点。
2. **它在 `CBEventType.QUERY` 的事件里做完全部工作**，而 `on_end` 只带 `RESPONSE`。也就是说从回调的角度看，「查询」是一个**不透明的黑盒**——内部细节通过 `retrieve` 与 `synthesize` 各自的嵌套事件展开。
3. **`_query` 上面又有一个 `@dispatcher.span`**，而基类的 `query` 上也有一个。于是 span 树里会有 `query → _query` 两层——一个略微多余但无害的嵌套。

**`retrieve()` 的实现是「检索 + 后处理链」。**

```
def retrieve(self, query_bundle):
    nodes = self._retriever.retrieve(query_bundle)
    return self._apply_node_postprocessors(nodes, query_bundle=query_bundle)

def _apply_node_postprocessors(self, nodes, query_bundle):
    for node_postprocessor in self._node_postprocessors:
        nodes = node_postprocessor.postprocess_nodes(nodes, query_bundle=query_bundle)
    return nodes
```

**后处理是「逐个串行、每步的输出喂给下一步」。** 于是链的顺序有实际后果——必须由使用者负责排对（例如 `LongContextReorder` 必须在过滤器之后）。

**`aretrieve` 走的是 `apostprocess_nodes`（异步版）。** 而 `BaseNodePostprocessor` 的默认 `apostprocess_nodes` 大概是「在线程里跑同步版」——也就是说异步路径在默认情况下并不会真的并行。这是这条链的一个隐含限制。

**`__init__` 里有一段容易被忽略的接线工作。**

```
self._retriever = retriever
self._response_synthesizer = response_synthesizer or get_response_synthesizer(llm=Settings.llm, callback_manager=callback_manager or Settings.callback_manager)
self._node_postprocessors = node_postprocessors or []
callback_manager = callback_manager or self._response_synthesizer.callback_manager
for node_postprocessor in self._node_postprocessors:
    node_postprocessor.callback_manager = callback_manager
super().__init__(callback_manager=callback_manager)
```

`callback_manager` 的优先级链是：显式传入 → 合成器自带 → `Settings.callback_manager`。然后**它把这个 manager 逐一写进每个后处理器**——因为后处理器是独立构造的对象，它们的 `callback_manager` 字段默认是一个空的 `CallbackManager([])`。

**这段赋值是必需的**：不这么做，后处理器内部的 `CBEventType.RERANKING` 之类事件就不属于这个查询的 trace，也不会出现在日志里。这是一个「用赋值把松散的部件接进同一条 trace」的做法——而不是依赖构造参数传递。

**`from_args` 是一个十几参数的类方法，它把「合成器如何配置」的所有旋钮都抬到了引擎这一层。**

签名里有 `response_mode=ResponseMode.COMPACT`（默认）、`text_qa_template`、`refine_template`、`summary_template`、`simple_template`、两个 chat content 模板、`output_cls`、`use_async`、`streaming`、`verbose`、`multimodal`。

也就是说**构造引擎时并不直接构造合成器，而是把合成器的参数全部前移**，由 `from_args` 内部调 `get_response_synthesizer` 组装。这个设计让「最常见的用法」只需一个调用，代价是：

- 这些参数与合成器的参数**必须手工保持同步**——合成器加一个模式就得在这里加一个参数；
- 大多数调用的绝大多数参数是 `None`。

**`multimodal=True` 那对参数是一个具体的例子。** 它有两个专用模板（`chat_content_qa_template` / `chat_content_refine_template`），只在多模态时用。这是**参数空间随能力增长而膨胀**的典型：每加一种能力，就多几个「只在某个 flag 为真时才有意义」的参数。

**`with_retriever` 是「换掉检索器、保留其余一切」。**

```
return RetrieverQueryEngine(retriever=retriever, response_synthesizer=self._response_synthesizer, callback_manager=self.callback_manager, node_postprocessors=self._node_postprocessors)
```

四个字段全部原样传递。这个方法存在是因为一个常见用法：**同一个合成配置要配多个检索器**（例如「同索引换 top_k」或「换不同索引」）。没有它就只能自己记着全部参数重造。

注意它**共享同一个 `response_synthesizer` 实例**——不是复制。于是两个引擎共用一份合成器状态（包括它的 callback manager 与可能的内部缓存）。这对无状态合成器无碍，对有状态的则是共享风险。

### 四、`PromptMixin` 与 `DispatcherSpanMixin` 的双继承

**`BaseQueryEngine(PromptMixin, DispatcherSpanMixin)`——两个 mixin 各自带来一组能力。**

- `PromptMixin` 要求实现 `_get_prompts` / `_update_prompts`，基类给的空实现（`return {}` 与 pass）。于是「一个查询引擎可以暴露它的提示以便统一改写」这件事成为**可选能力**。
- `DispatcherSpanMixin` 提供的应该是「让实例可被 span 追踪」的基础设施——`@dispatcher.span` 装饰器要能挂上它。

**`_get_prompts` 在 `RetrieverQueryEngine` 里被覆盖成一件有价值的事。**

```
def _get_prompt_modules(self) -> PromptMixinType:
    return {"response_synthesizer": self._response_synthesizer}
```

`_get_prompt_modules`（模块级）而不是 `_get_prompts`（直接返回提示字典）——也就是说引擎**不复制**合成器的提示，而是**把合成器本身作为一个子模块暴露**。于是上层可以递归地收集整棵组件树的提示，而不必在每个层级手工同步。

这是一个正确的结构：**提示的归属跟着它的拥有者走**，中间层只负责传递。

## 关键取舍

**源码事实：** `BaseQueryEngine` 只有 `_query` / `_aquery` 是抽象方法；`retrieve` / `synthesize` / `asynthesize` **默认抛 `NotImplementedError`**，且消息里给出替代方法。

**源码事实：** `query` 同时挂 `@dispatcher.span` 与两个 `dispatcher.event`（start/end），两套仪表机制并存。

**源码事实：** `QueryStartEvent` 用转换前的原始参数、`QueryEndEvent` 用转换后的 `QueryBundle`；且两个事件都在 `as_trace` 块之外。

**源码事实：** `_query` 调 `self.retrieve()` 而非 `self._retriever.retrieve()`，因此**后处理属于「检索」概念的一部分**，合成器只见到处理完的节点。

**源码事实：** 后处理链是「逐个串行、上一步输出喂下一步」，顺序有实际后果。

**源码事实：** `__init__` 会把最终确定的 `callback_manager` **逐一写进每个后处理器**，否则它们内部的事件不属于这条 trace。

**源码事实：** `callback_manager` 的优先级是「显式 → 合成器自带 → 全局 Settings」。

**源码事实：** `from_args` 把合成器的十几个参数全部前移到引擎层，必须手工保持同步；每加一种能力（如 `multimodal`）就多几个只在特定 flag 下有效的参数。

**源码事实：** `with_retriever` **共享同一个合成器实例**（不复制）。

**源码事实：** `_get_prompt_modules` 返回合成器本身而不是它的提示字典，让提示归属跟随拥有者、上层递归收集。

**架构推论：** 这个契约的形状说明了一件工程上的事：**「必须做什么」与「可以做什么」应当用不同的机制表达**。`abstractmethod` 表达前者（编译器强制），而「默认抛 `NotImplementedError`」表达后者（运行期可选）。

若把 `retrieve` / `synthesize` 也设成抽象方法，每个子类都要写三个无意义的抛错；若把它们做成「返回空列表」之类的默认实现，调用方就分不清「不支持」与「支持但没结果」。**抛错是这两种错之间唯一清晰的表达**——它让「不支持」成为一个可被捕获的明确信号。

代价是调用方无法静态判断。这是一个可以接受的代价，因为真正需要分解的调用方（想让检索与合成分开控制的人）通常自己就持有具体类型的引用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/base/base_query_engine.py` | `BaseQueryEngine` | 查询引擎契约：必需入口 + 可选分解 |
| `llama-index-core/llama_index/core/base/base_query_engine.py` | `BaseQueryEngine.query` / `aquery` | 门面：span + 两个事件 + `str`→`QueryBundle` + trace |
| `llama-index-core/llama_index/core/base/base_query_engine.py` | `BaseQueryEngine.retrieve` / `synthesize` | 默认抛 `NotImplementedError` 并给出替代指引 |
| `llama-index-core/llama_index/core/query_engine/retriever_query_engine.py` | `RetrieverQueryEngine` | 标准三段：检索 → 后处理 → 合成 |
| `llama-index-core/llama_index/core/query_engine/retriever_query_engine.py` | `RetrieverQueryEngine.retrieve` | 检索 + 串行后处理链 |
| `llama-index-core/llama_index/core/query_engine/retriever_query_engine.py` | `_apply_node_postprocessors` | 逐个串行，上一步输出喂下一步 |
| `llama-index-core/llama_index/core/query_engine/retriever_query_engine.py` | `RetrieverQueryEngine.__init__` | callback_manager 优先级链 + 写进每个后处理器 |
| `llama-index-core/llama_index/core/query_engine/retriever_query_engine.py` | `RetrieverQueryEngine.from_args` | 把合成器参数前移到引擎层 |
| `llama-index-core/llama_index/core/query_engine/retriever_query_engine.py` | `with_retriever` | 换检索器、共享同一个合成器 |
| `llama-index-core/llama_index/core/query_engine/retriever_query_engine.py` | `_get_prompt_modules` | 暴露合成器作为子模块，让提示归属跟随拥有者 |

## 参考

- [llama-index 源码：base/base_query_engine.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/base/base_query_engine.py)
- [llama-index 源码：query_engine/retriever_query_engine.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/query_engine/retriever_query_engine.py)
- [llama-index 文档：查询引擎](https://docs.llamaindex.ai/en/stable/module_guides/deploying/query_engine/)
- [Python 文档：abc.abstractmethod](https://docs.python.org/3/library/abc.html#abc.abstractmethod)

## 与主核 / AI 的关系

这个「必需 + 可选」双层契约与主核里的**存储引擎接口**同构：数据库的表访问方法也区分「必须实现的」与「可以实现但允许不支持的」（例如某些引擎不支持 `savepoint` 或 `DELETE WHERE`，它们的实现返回 `SQLITE_ERROR` / `NotSupported` 而不是编译期报错）。

主核的做法类似——用明确的错误码表达「这个能力不可用」，而不是让每个实现都写空桩。而它的调用方也得靠能力探测（`sqlite3_table_column_metadata`、版本号）来预判。

对 agent 工程的直接价值有三条。其一，**「不支持」要给出替代方案**。三条错误消息都写明「改用 query」，这比一句干巴巴的 `NotImplementedError` 有用得多——尤其在这种「用户可能只是想拆开控制」的场景里。其二，**接线工作要显式做**。后处理器的 `callback_manager` 必须被父组件赋值，否则它们的事件不属于同一条 trace；这类「部件自带一个默认但必须被替换的字段」是所有分层 tracer 系统的共同坑。其三，**参数前移要付同步成本**。`from_args` 把合成器的十几个参数搬上来，让便利性提升而维护成本转到「必须手工同步」——当上游加一个模式，这里必须加一个参数，否则用户拿不到。
