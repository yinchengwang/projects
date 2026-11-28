---
project: llama-index
feature: llm-and-embedding-base
title: 模型与嵌入的抽象
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/base/llms/base.py
    symbols: [BaseLLM, ChatResponse, CompletionResponse]
  - path: llama-index-core/llama_index/core/base/llms/generic_utils.py
    symbols: [messages_to_prompt, prompt_to_messages, chat_response_to_completion_response]
  - path: llama-index-core/llama_index/core/base/embeddings/base.py
    symbols: [BaseEmbedding, similarity, SimilarityMode]
---

# 模型与嵌入的抽象

## 一句话职责

把「模型」抽象成**八个必选方法**（聊天/补全 × 同步/异步 × 一次/流式），然后提供一整组装饰器——让只实现了其中一两种能力的后端通过「消息转提示词、聊天响应转补全响应」被包装成全部八种；嵌入侧则把「一个文本要嵌几次」的语义拆成 `_` 前缀的必选与可覆盖的批量/异步/缓存层。

## 机制是什么

`BaseLLM(BaseComponent, DispatcherSpanMixin)` 的两个字段都是 `exclude=True`：`callback_manager` 与 `rate_limiter`。后者浓墨重彩地标了「期望类型：`BaseRateLimiter`（来自 `rate_limiter` 模块），用 `Any` 以避免导入环」。

八个 `@abstractmethod`：`chat` / `complete` / `stream_chat` / `stream_complete`（同步），`achat` / `acomplete` / `astream_chat` / `astream_complete`（异步）。外加一个必选的 `metadata` property 返回 `LLMMetadata`。

而 `generic_utils.py` 提供那一整组转换：`messages_to_prompt` / `prompt_to_messages` 是**输入侧**，`completion_response_to_chat_response` / `chat_response_to_completion_response`（含流式与异步流式版本）是**输出侧**，六个 decorator 把它们组合成「补全函数变聊天函数」与「聊天函数变补全函数」。

嵌入侧`BaseEmbedding(TransformComponent, DispatcherSpanMixin)` 的必选只有两个：`_get_query_embedding` 与 `_get_text_embedding`。其余全是可覆盖的默认实现。

`similarity(embedding1, embedding2, mode)` 是一个**模块级函数**，`SimilarityMode` 三值：`DEFAULT`（值就是 `"cosine"`）、`DOT_PRODUCT`、`EUCLIDEAN`。

## 怎么实现的

**八个必选方法意味着「一个只支持聊天、不支流式的模型」也必须写八个方法。** 这是这一层最直白的事实。但 `generic_utils` 的六个装饰器让这件事可行：

```python
@completion_to_chat_decorator
def complete(prompt, **kwargs) -> CompletionResponse: ...

# 于是得到一个 chat 方法
chat = completion_to_chat_decorator(complete)
```

所以「实现一个 `complete`，再套一层装饰器」就得到了 `chat`。而装饰器内部做两件事，都有明确的 `# normalize input` / `# normalize output` 注释：

```python
prompt = messages_to_prompt(messages)      # 输入归一
completion_response = func(prompt, **kwargs)
return completion_response_to_chat_response(completion_response)  # 输出归一
```

**`messages_to_prompt` 的格式极其朴素，而它的朴素是有意的。** 实现每条形如 `f"{role.value}: {content}"`，若有 `additional_kwargs` 就追加一行，最后再补一行 `f"{MessageRole.ASSISTANT.value}: "`（注意末尾冒号与空格）——**给模型留出续写的位置**。

所以「聊天 → 提示词」这个降维动作在这里就是「拼成 `user: xxx\nassistant: ` 这样一段文本」。它没有用任何模型的真实聊天模板（例如 ChatML 或某种 token 前缀）——这是**故意的通用降级**：既然目标模型是「只会补全的老式模型」，那它能看到的就只有一段文本，用最朴素的分角色标记已经够用。

`messages_to_history_str` 与它几乎一样，唯一的差别是**不补那行 `assistant:`**。所以两个函数只差在这个后缀，而它们的存在理由是两种用途：`messages_to_history_str` 用于「把历史压成一段交给某个组件」，`messages_to_prompt` 用于「让模型接着写」。这个差别不该被忽略——补不补 `assistant:` 决定了模型是在「读一段对话」还是「轮到我说话了」。

**`prompt_to_messages` 单行实现，配方固定。** `return [ChatMessage(role=MessageRole.USER, content=prompt)]` ——一个提示词就是一个 user 消息。所以「补全 → 聊天」的输入侧实际上是「把一段文本包成一条 user 消息」，信息量没有增加。

于是「只实现 chat 的模型被包装成 complete」时，一来一回都发生在**降维**上：输入被包成一条 user 消息（没有丢失，因为本来就只有一段文本），输出被取 `message.content` 当一个字符串。后者就是 `chat_response_to_completion_response`：

```python
additional_kwargs = chat_response.message.additional_kwargs
additional_kwwards.update(chat_response.additional_kwargs)
return CompletionResponse(text=chat_response.message.content or "", additional_kwargs=additional_kwargs, raw=chat_response.raw)
```

两处值得记：

1. `additional_kwargs` 是**两级合并**：先取消息自己的，再把响应级的 update 进去（响应级优先）。所以「响应级 kwargs 覆盖消息级」。
2. `chat_response.message.content or ""` —— `content` 为 `None` 时给空串。

而这个 update 是**在消息对象上就地改**的（`additional_kwargs = chat_response.message.additional_kwargs` 拿到的是引用，`update` 直接改它）。所以转换之后，原来那个 `ChatResponse.message.additional_kwargs` 也被改了。这是一个副作用——反向的 `completion_response_to_chat_response` 不做双向合并（只把 completion 的 kwargs 塞进消息），所以两个方向不对称。

**反向转换丢东西。** `completion_response_to_chat_response` 只搬 `text` 与 `additional_kwargs`，**不搬 `delta`**：

```python
return ChatResponse(message=ChatMessage(role=ASSISTANT, content=completion_response.text, additional_kwargs=...), raw=completion_response.raw)
```

而流式版本搬了 `delta`：

```python
yield ChatResponse(message=ChatMessage(...), delta=response.delta, raw=response.raw)
```

所以非流式转换丢 `delta`（那本身在非流式响应里也没意义），流式转换保留。这是一个「按场景取所需字段」的合理差异，但它在签名上不可见——`ChatResponse` 的 `delta` 是 `Optional`，所以两种转换产出的对象类型相同、语义不同。

**流式单响应与异步流式的转换函数各有一份，且有重复。** 文件里有 `stream_completion_response_to_chat_response` 与 `astream_completion_response_to_chat_response`，两者函数体几乎逐行相同（后者 `async for` 而非 `for`）。类似地 `stream_chat_response_to_completion_response` 与 `astream_chat_response_to_completion_response` 也是两份。

而注释里还出现了 `async_stream_completion_response_to_chat_response` 与 `astream_completion_response_to_chat_response` 这两个**函数名不同但实现完全相同**的版本（后者是前者的另一份）。所以这一层里有一段真实的重复代码：同一个异步转换被写了两次，名字不同。

这一处值得记下来，因为它说明「一个抽象要求八个方法」的成本会渗到工具层：为了让八种组合都有对应的转换，这一层不得不维护「同步/异步 × 四种方向」的转换矩阵。

**`get_from_param_or_env` 是这一层里最实用的一个小函数。** 三级回退：显式参数 → 环境变量（且值非空）→ 默认值，全都没有就报错，且错误消息里同时给出「环境变量名」与「参数名」：

```
f"Did not find {key}, please add an environment variable `{env_key}` which contains it, or pass `{key}` as a named parameter."
```

所以使用者拿到错误就知道两条补救路径。这是「错误消息要能自救」的又一例（与之前几页里 llama-index 的其他实现同源）。

**`BaseLLM.to_payload` 是一条安全契约，不是一个普通方法。** docstring 写得很硬：「这个 LLM 的**非敏感**表示，用于可观测性。通过 instrumentation 事件与 callback payload 发出，所以它**绝不能**包含凭据（例如 `api_key`）或认证头。默认返回模型的 metadata；子类可以覆盖以加入安全的细节」。

实现是 `{"class_name": self.class_name(), **self.metadata.model_dump()}` ——把 metadata 整份 dump 出来。所以这个默认值的安全性依赖「`LLMMetadata` 里不含密钥」这个前提。

`BaseEmbedding.to_payload` 走得更保守：只输出三个字段（`class_name` / `model_name` / `embed_batch_size`），而不是把整个模型配置 dump 出来。两份实现的保守程度不同，前者靠 metadata 的类型保证，后者白名单列举。

**`convert_chat_messages` 把「内容可能是块列表」压成字符串。** 实现逐个消息判断 `content` 的类型：`str` 直接过；`List` 时遍历块，`TextBlock` 就把 `text` 拼起来，**其他块类型直接 `raise ValueError("LLM only supports text inputs")`**；其余类型报错。

而它在拼完之后**就地改 `message.content = content_string`** ——修改了传入的消息对象。所以「转换」这个方法实际上有副作用：调用之后，原来那些 `ChatMessage` 的 `content` 就变成字符串了。

这是这一层的一个真实隐患：一个「看起来是纯转换」的方法改了输入。如果同一个消息列表还要给别的地方用（例如给一个支持多模态的模型），它已经被压平了。

**嵌入侧的必选是「两个 `_` 前缀方法」，其余都是可覆盖的默认层。** 这个命名约定本身是这一层的核心：

- `_get_query_embedding` / `_get_text_embedding` —— `@abstractmethod`，必须实现。
- `_aget_text_embedding` —— 默认 `return self._get_text_embedding(text)` （**同步阻塞在异步方法里**）。
- `_get_text_embeddings` —— 默认 `[self._get_text_embedding(text) for text in texts]`（**逐个循环，不是真批量**）。
- `_aget_text_embeddings` —— 默认 `asyncio.gather(*[self._aget_text_embedding(text) ...])`（**并发但不批量**）。
- `_aget_text_embeddings_rate_limited` —— 先 `await self.rate_limiter.async_acquire()` 再走上面那条。

所以「批量嵌入」在默认实现下是「N 次单条调用」。docstring 说得清楚：「子类若支持批量查询可以实现这个方法」。而 rate-limited 那层是**在批量外面套一次 acquire**（不是每条一次），意味着「一批算一个配额」。这个语义选择很重要——若每条都 acquire，那 20 条一批就要等 20 次；一次 acquire 则承认「批量调用是对后端的一次压力」。

**`BaseEmbedding.__call__` 是 TransformComponent 的接口，做的事很具体。** 它把节点内容按 `MetadataMode.EMBED` 取出、批量嵌入、然后**逐节点写回 `node.embedding`**：

```python
embeddings = self.get_text_embedding_batch([node.get_content(metadata_mode=MetadataMode.EMBED) for node in nodes], **kwargs)
for node, embedding in zip(nodes, embeddings):
    node.embedding = embedding
return nodes
```

两点值得记：`MetadataMode.EMBED` 说明「嵌入时看到的文本与生成时看到的不同」（元数据会以某种形式并进文本，因为嵌入要能捕捉元数据语义）；而 `zip(nodes, embeddings)` **不检查长度**——若嵌入返回的列表比节点少，多出来的节点就保持原 embedding（可能为 `None`），不报错。

**`similarity` 的三种模式，只有一种归到了 `[0,1]`。** 实现：

- `EUCLIDEAN` → `-float(np.linalg.norm(a - b))` ——**取负的欧氏距离**。注释解释了：「用负欧氏距离作相似度以获得相同的排序次序」。
- `DOT_PRODUCT` → `np.dot(a, b)` ——原始内积，无界。
- `DEFAULT`（余弦）→ `dot / (norm(a) * norm(b))`。

所以三种模式的输出**尺度完全不同**：余弦在 `[-1, 1]`，内积在 `(-∞, +∞)`，负欧氏距离在 `(-∞, 0]`。而它们的共同点是「越大越相似」——所以**可以混用于排序，但不能混用于阈值**。

这一点在 `SimilarityMode` 命名上体现得很微妙：`DEFAULT` 的值是 `"cosine"` 而不是 `"default"`。所以序列化时写出的是 `"cosine"` ——枚举成员的**名字**与它表示的**度量**分离了。这让人在日志里看到 `"cosine"` 而不知道它是默认值。

`BaseEmbedding` 上另有一个**同名实例方法** `similarity`，实现是调模块级函数。所以「`from ... import similarity` 与 `emb.similarity(...)` 两种用法都成立」——一个是纯函数、一个是转发。转发那层存在的意义大概是「让调用方不必知道该用哪个」。

**`mean_agg` 是 `get_agg_embedding_from_queries` 的默认聚合器。** 实现 `np.array(embeddings).mean(axis=0).tolist()`，且空输入 `raise ValueError("No embeddings to aggregate")`。

而 `get_agg_embedding_from_queries` 的签名允许传 `agg_fn`，所以「多个查询合成一个向量」的聚合方式是可替换的。这是个有用的设计：多查询检索时，除了均值还可以用「取第一条」「加权」等策略。

**`BaseEmbedding` 的 `embeddings_cache` 强制执行类型。** 那个 `model_validator` 里：

```python
if self.embeddings_cache is not None and not isinstance(self.embeddings_cache, BaseKVStore):
    raise TypeError("embeddings_cache must be of type BaseKVStore")
```

所以「嵌入缓存」必须是 h01 那套 `BaseKVStore` 的实现。字段本身标注成 `Optional[Any]`（注释「用 `Any` 以避免导入环」），但校验时在函数内导入真类型并检查。这是一个「类型标注为了解耦而放宽、校验为了正确而收紧」的组合。

而 `embed_batch_size` 带 `gt=0, le=2048` 的数值约束——所以「批量大小」有一个硬上界 2048，且不能为 0。

## 关键取舍

**源码事实：** `BaseLLM` 有八个 `@abstractmethod`（聊天/补全 × 同步/异步 × 一次/流式）加一个 `metadata` property。

**源码事实：** `messages_to_prompt` 与 `messages_to_history_str` 的实现只差「末尾是否补 `assistant: `」这一行。

**源码事实：** `prompt_to_messages` 固定产出单条 `role=USER` 的消息——补全转聊天的输入侧没有信息增益。

**源码事实：** `chat_response_to_completion_response` 在消息对象上就地合并 `additional_kwargs`，且两个方向的合并不对称。

**源码事实：** 非流式转换不搬 `delta`，流式转换搬——两者签名相同、语义不同。

**源码事实：** 异步流式转换在同一文件里存在两个函数名不同、实现相同的版本。

**源码事实：** `convert_chat_messages` 就地修改传入消息的 `content`，且遇到非 `TextBlock` 直接报错。

**源码事实：** `BaseLLM.to_payload` 的 docstring 明确禁止包含凭据；`BaseEmbedding.to_payload` 用白名单三字段，比前者更保守。

**源码事实：** 嵌入的必选只有两个 `_` 前缀方法；批量默认是逐条循环、异步批量默认是 `gather`。

**源码事实：** `_aget_text_embeddings_rate_limited` 一批只 acquire 一次配额。

**源码事实：** `similarity` 三种模式的输出尺度完全不同（余弦 `[-1,1]`、内积无界、负欧氏距离 `≤0`），共同点是「越大越相似」。

**源码事实：** `embeddings_cache` 标注为 `Optional[Any]` 但校验时必须 `isinstance(BaseKVStore)`；`embed_batch_size` 有 `gt=0, le=2048`。

**架构推论：** 「八个必选方法 + 一整组转换装饰器」这个组合，是把「接口完备性」与「实现成本」这对矛盾分开处理的结果。接口要求八种能力（因为上层代码会调用任意一种），但实现方只需写一两种，剩下的由装饰器合成。而代价是**转换过程必然有损**：聊天转补全丢掉了 role 结构，补全转聊天丢掉了 delta。所以「被包装出来的能力」与「原生的能力」不等价，而这一点在类型上完全看不出来——两者都返回 `ChatResponse`。

**架构推论：** `messages_to_prompt` 那两行的差别（补不补 `assistant:`）揭示了一个重要的语义分野。「历史字符串」是给组件读的，不该有「该你了」的暗示；「提示词」是给模型接续的，必须有。任何把对话压成文本的地方都要先问：这段文本是给谁看的？给模型续写就要留出位置，给人或给另一个 LLM 当上下文就不该留。

**架构推论：** 「必选用 `_` 前缀、公开方法带完整包装」这个命名约定，是嵌入侧的关键设计。它让「可覆盖的层」与「不可覆盖的层」在名字上就分开了：`_get_text_embedding` 是实现方的契约点，`get_text_embedding` 是调用方的入口（还带 `@dispatcher.span` 做追踪）。任何「一个操作有多层可控性」的抽象都该这么分——否则调用方无法判断「我改这个方法安全吗」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/base/llms/base.py` | `BaseLLM` | 八个必选方法加 metadata 的模型抽象 |
| `llama-index-core/llama_index/core/base/llms/base.py` | `BaseLLM.to_payload` | 非敏感可观测表示，明令禁止凭据 |
| `llama-index-core/llama_index/core/base/llms/base.py` | `BaseLLM.convert_chat_messages` | 块列表压平为字符串，就地修改输入 |
| `llama-index-core/llama_index/core/base/llms/base.py` | `BaseLLM.rate_limiter` | 用 `Any` 避免导入环的限流器槽位 |
| `llama-index-core/llama_index/core/base/llms/generic_utils.py` | `messages_to_prompt` | 对话拼成提示词，末尾补 `assistant: ` |
| `llama-index-core/llama_index/core/base/llms/generic_utils.py` | `prompt_to_messages` | 单条 user 消息，补全转聊天的输入侧 |
| `llama-index-core/llama_index/core/base/llms/generic_utils.py` | `chat_response_to_completion_response` | 输出侧降维，就地合并 kwargs |
| `llama-index-core/llama_index/core/base/llms/generic_utils.py` | `completion_to_chat_decorator` | 用补全函数合成聊天函数 |
| `llama-index-core/llama_index/core/base/llms/generic_utils.py` | `chat_to_completion_decorator` | 反向合成 |
| `llama-index-core/llama_index/core/base/llms/generic_utils.py` | `get_from_param_or_env` | 参数与环境变量三级回退，错误消息给双路径 |
| `llama-index-core/llama_index/core/base/llms/generic_utils.py` | `image_node_to_image_block` | 图片节点的三种来源转换与 base64 校验 |
| `llama-index-core/llama_index/core/base/embeddings/base.py` | `BaseEmbedding` | 必选两个 `_` 方法，其余可覆盖 |
| `llama-index-core/llama_index/core/base/embeddings/base.py` | `BaseEmbedding._get_text_embeddings` | 默认逐条循环，非真批量 |
| `llama-index-core/llama_index/core/base/embeddings/base.py` | `_aget_text_embeddings_rate_limited` | 一批只 acquire 一次配额 |
| `llama-index-core/llama_index/core/base/embeddings/base.py` | `BaseEmbedding.__call__` | 按 EMBED 模式取文本、批量嵌入、写回节点 |
| `llama-index-core/llama_index/core/base/embeddings/base.py` | `BaseEmbedding.check_base_embeddings_class` | 缓存必须是 `BaseKVStore` |
| `llama-index-core/llama_index/core/base/embeddings/base.py` | `similarity` | 三种度量的尺度各不同，共同点是越大越相似 |
| `llama-index-core/llama_index/core/base/embeddings/base.py` | `SimilarityMode` | 枚举值写 `"cosine"` 而非 `"default"` |
| `llama-index-core/llama_index/core/base/embeddings/base.py` | `mean_agg` | 多查询聚合的默认均值，空输入报错 |

## 参考

- [llama-index 源码：base/llms/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/base/llms/base.py)
- [llama-index 源码：base/llms/generic_utils.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/base/llms/generic_utils.py)
- [llama-index 文档：自定义 LLM](https://docs.llamaindex.ai/en/stable/module_guides/models/llms/usage_custom/)

## 与主核 / AI 的关系

主核里最接近的是「存储引擎的能力矩阵 + 适配层」：一个引擎可能只支持全表扫、另一个只支持索引查，上层通过适配层把它们统一成一套接口；而那些「这个引擎没有的能力」由适配层用降级实现补上（例如索引查降级成全表扫）。

这一页的转换装饰器就是那个降级层，且它把降级做得**显式**：不是「没有 `chat` 就报错」，而是「用 `messages_to_prompt` 把对话拼成提示词喂给 `complete`」。这与主核里「没有索引就扫全表」是同一类做法——**用有损但正确的方式补齐能力**。

差别在于**有损的程度与可见性**。主核的降级只影响性能（扫全表更慢，结果一样）；这里的降级影响语义——聊天转补全之后，模型看不到 role 结构（system prompt 也被拼成一行文本）。所以同一份代码在原生聊天模型与包装过的补全模型上跑，行为可能不同，而类型检查看不出来。

与 i04（限流与异步工具）的连接：`BaseLLM` 与 `BaseEmbedding` 都有 `rate_limiter: Optional[Any]` 字段（都用 `Any` 规避导入环），而嵌入侧那层 `_aget_text_embeddings_rate_limited` 就是实际的接入点。所以这一页是「接口」，i04 是「那个接口的实现与用法」。

与 i02（全局设置）的连接：`Settings.llm` 与 `Settings.embed_model` 的类型就是这一页的两个基类，所以默认行为由这里的默认实现决定。

对 AI 工程最实用的一条是「区分『给模型续写的文本』与『给组件读的文本』」。同一个「把对话拼成字符串」的函数，末尾补不补 `assistant:` 决定了模型是在读历史还是该发言了。这类细微差别在生产里经常表现为「模型莫名其妙地继续扮演用户」或「输出带上了 `assistant:` 前缀」——原因往往就是这段拼接少了或多了那一个标记。

另一条是「转换有损时要显式说明」。`chat_to_completion_decorator` 这种「把一个只支持补全的模型包装成聊天模型」的能力很方便，但用户需要知道 role 结构丢失了。做法是把它写在装饰器的 doc 里，或者在包装时发出一次警告——而不是让 `ChatResponse` 的类型让人误以为「能力等价」。

最后一条是 `similarity` 那三种尺度。多后端混用时最危险的错误是「拿余弦相似度算出的 0.8 与内积算出的 0.8 当同一标准」。这里的实现让三个函数共用同一个入口（`mode` 参数），但输出尺度不同——所以**阈值必须与度量绑定**，任何「统一设个 0.7 阈值」的做法在切换度量后都会失效。相比之下 langchain 那边（`vectorstore-contract` 页）的做法是强制子类声明一个归一函数，把尺度问题提前暴露。两种做法各有代价：这里简单但容易误用，那边严格但适配成本高。
