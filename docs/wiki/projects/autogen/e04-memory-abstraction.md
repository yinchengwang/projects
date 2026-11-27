---
project: autogen
feature: memory-abstraction
title: 记忆抽象与列表实现
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/memory/_base_memory.py
    symbols: [Memory, MemoryContent, update_context]
  - path: python/packages/autogen-core/src/autogen_core/memory/_list_memory.py
    symbols: [ListMemory, query, add]
---

# 记忆抽象与列表实现

## 一句话职责

把「外部知识」与「对话历史」分开建模：记忆负责长期内容的存取与注入，并提供一个反向操作——`update_context` 直接把召回结果写进模型上下文，由记忆实现决定注入什么、怎么排版。

## 机制是什么

模型上下文管「这一次对话说了什么」，记忆管「此前积累了什么」。两者的接口形态不同：上下文只有 `add`/`get`/`clear`，记忆则有五个抽象方法，且多出一个关键的 `update_context(model_context)`。

`Memory` 的五个抽象方法是 `update_context`、`query`、`add`、`clear`、`close`。前四个都好理解，只有 `update_context` 值得单独说：它的入参是一个 `ChatCompletionContext`，返回值是 `UpdateContextResult`（内含本次注入的 `MemoryQueryResult`）。**记忆实现直接拥有改写模型上下文的权力**——它可以只看当前上下文决定召回什么，然后把结果**写进**上下文。这把「检索 + 注入」两步合成一步，交给实现方编排。

数据模型三层：`MemoryContent` 是单条（`content` + `mime_type` + `metadata`），`MemoryQueryResult` 是一批（`results` 列表），`UpdateContextResult` 包一层 `memories`。`content` 的类型是 `Union[str, bytes, Dict[str, Any], Image]`，靠 `arbitrary_types_allowed=True` 让 `Image` 这类非 pydantic 原生类型通过校验。`mime_type` 可以取 `MemoryMimeType` 枚举（`text/plain`、`application/json`、`text/markdown`、`image/*`、`application/octet-stream`）或任意字符串，并配了 `field_serializer` 把枚举序列化成它的 `value`——所以状态里存的是 `"text/plain"` 而非 `"MemoryMimeType.TEXT"`。

`ListMemory` 是最小实现：内部一个 `List[MemoryContent]`，按时间顺序追加。

## 怎么实现的

**`ListMemory.update_context` 的三段逻辑。** 若内容为空，直接返回空 `UpdateContextResult` 且**不动上下文**。否则把每条内容格式化成 `f"{i}. {str(memory.content)}"`（序号从 1 开始），拼成一段文本：前缀 `"\nRelevant memory content (in chronological order):\n"`，中间换行连接，尾部再补一个换行。然后把这段文本包成 `SystemMessage` 调 `model_context.add_message`。最后返回全部内容作为「本次注入的记忆」。

注意它**完全没有查询**：注入的是全部记忆，不按当前对话相关性筛选。方法名里的 "Relevant" 与实现不符，是实现留下的空位。

**`ListMemory.query` 是个直通。** 实现只有一行 `_ = query, cancellation_token, kwargs` 再 `return MemoryQueryResult(results=self._contents)`——参数被显式丢弃（用 `_` 接收以通过 linter），返回全部。所以 `ListMemory` 上「查询」等价于「列出全部」。

**`add` / `clear` / `close`。** `add` 是 `append`；`clear` 是 `self._contents = []`（换对象）；`close` 是 `pass`——没有资源需要释放。

**属性访问。** `name` 是只读 property，构造时 `name or "default_list_memory"`；`content` 有 getter 和 setter，setter 允许外部整体替换内容列表。这让 `ListMemory` 在测试与脚本里能直接改内容，不必走 `add`。

**组件化。** `ListMemory` 是 `Component[ListMemoryConfig]`，`component_provider_override = "autogen_core.memory.ListMemory"`。`ListMemoryConfig` 有两个字段：`name`（可空）与 `memory_contents`（默认空列表）。因为 `memory_contents` 是配置的一部分，**记忆内容是可序列化的**——这与缓存的 `InMemoryStoreConfig` 刻意留空形成对照：记忆内容就是记忆的语义，必须能 dump 出去。

**`update_context` 的错误面。** 抽象方法没有声明异常，`ListMemory` 也不捕。所以 `model_context.add_message` 若抛（例如自定义上下文的 `add_message` 校验失败），异常直接冒到调用方——注入失败与查询失败没有被区分。

## 关键取舍

**源码事实：** `update_context` 会**修改传入的上下文**（docstring 明确写 "Will be mutated if memories exist"）。这让记忆成为上下文的一个「写者」，且是隐式的——调用方必须先拿到上下文实例、再传给记忆，才能得到注入效果。没有返回值里的「新上下文」可供替换。

**源码事实：** `ListMemory.update_context` 每次调用都追加一条 `SystemMessage`，不检查上下文里是否已有同类消息。重复调用会累积多条记忆提示，且每条都列全量内容——重复注入不幂等。

**源码事实：** `query` 在 `ListMemory` 里退化为「返回全部」。这意味着任何依赖「按相关性召回」的上层逻辑，在换成 `ListMemory` 后会退化成「全量召回」，不会报错，只会让 prompt 变长。这是接口宽泛、实现缩水的典型形态。

**源码事实：** `MemoryContent.content` 允许 `bytes` 与 `Image`，但 `ListMemory` 注入时用 `str(memory.content)`——对 `bytes` 得到的是 `b'...'` 的字面表示，对 `Image` 得到的是对象 repr（含内存地址）。**结构化内容在列表实现里会退化成不可用的字符串**，而代码不做任何类型判断。

**架构推论：** `Memory` 继承 `ComponentBase[BaseModel]` 且 `component_type = "memory"`，所以记忆是可配置组件，能随智能体定义一起序列化。但 `ListMemoryConfig` 只覆盖「内容 + 名字」，任何真正的检索型实现（向量库、数据库）的配置只能是连接参数——**记忆的「能力」不在配置里**，配置只描述「连到哪儿」。

**架构推论：** 五个抽象方法里 `close` 的存在说明记忆被预期会持有外部资源（连接、句柄）。基类不提供上下文管理器协议（不像 `CodeExecutor` 与 `Workbench` 有 `__aenter__`/`__aexit__`），所以资源释放必须由调用方显式调 `close`，没有 with 语法糖可用。

**架构推论：** 记忆与上下文的分工不是「历史 vs 知识」而是「读时机不同」：上下文的消息每轮都被完整送给模型，记忆的内容只在 `update_context` 被调用的那一刻以一条 `SystemMessage` 的形式注入。所以记忆的注入时机由调用方决定，而 `Memory` 接口本身不规定「每轮都该调」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/memory/_base_memory.py` | `Memory` | 记忆抽象，五个方法含 `update_context` |
| `python/packages/autogen-core/src/autogen_core/memory/_base_memory.py` | `Memory.update_context` | 由记忆实现改写模型上下文并返回注入内容 |
| `python/packages/autogen-core/src/autogen_core/memory/_base_memory.py` | `MemoryContent` | 单条记忆：内容、MIME、元数据 |
| `python/packages/autogen-core/src/autogen_core/memory/_base_memory.py` | `MemoryMimeType` | MIME 枚举，序列化为字符串值 |
| `python/packages/autogen-core/src/autogen_core/memory/_base_memory.py` | `MemoryContent.serialize_mime_type` | 把枚举序列化成 `value` |
| `python/packages/autogen-core/src/autogen_core/memory/_base_memory.py` | `MemoryQueryResult` | 一批记忆结果 |
| `python/packages/autogen-core/src/autogen_core/memory/_base_memory.py` | `UpdateContextResult` | 注入结果，内存放 `MemoryQueryResult` |
| `python/packages/autogen-core/src/autogen_core/memory/_list_memory.py` | `ListMemory` | 按时间顺序追加的列表实现 |
| `python/packages/autogen-core/src/autogen_core/memory/_list_memory.py` | `ListMemory.query` | 直通返回全部内容，忽略查询参数 |
| `python/packages/autogen-core/src/autogen_core/memory/_list_memory.py` | `ListMemory.add` | 追加一条内容 |
| `python/packages/autogen-core/src/autogen_core/memory/_list_memory.py` | `ListMemory.update_context` | 全量格式化后注入一条 `SystemMessage` |
| `python/packages/autogen-core/src/autogen_core/memory/_list_memory.py` | `ListMemoryConfig` | 名字与内容列表，记忆内容可序列化 |

## 参考

- [AutoGen 源码：_base_memory.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/memory/_base_memory.py)
- [AutoGen 源码：_list_memory.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/memory/_list_memory.py)
- [AutoGen 文档：Model Context](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/model-context.html)

## 与主核 / AI 的关系

这份抽象最像主核里的「外部表 / 扩展」：内容不在主存储里，取用要经过连接，且平台的代价由实现承担。但记忆比那更弱——它连「一致性」都没有定义：`query` 与 `update_context` 之间没有任何隔离保证，两次调用之间内容可以变，也没有版本或快照概念。对 RAG 这类只追加、不修改的用法来说这足够；对会删改的记忆，接口缺一个「读到的是哪个时刻」的表达。

对 AI 工程最要紧的是 `update_context` 的这个设计选择：**它让记忆拥有了改写 prompt 的权力**。好处是注入格式由最懂数据的一方决定（列表用编号、向量库可以带相似度、结构化记忆可以带 schema）；坏处是调试成本——一次请求的最终 prompt 由上下文内容 + 记忆注入共同决定，而注入发生在 `get_messages` 之外的时刻，不读记忆实现就无法复原最终 prompt。若要做到可复现，必须把 `update_context` 的产物也纳入日志。

另一条实践提醒：`MemoryContent` 支持 `Image`，但默认实现用 `str()` 注入。要用多模态记忆，注入逻辑必须自己处理 `Image`（例如调 `to_openai_format()` 或 `data_uri`），否则视觉信息在注入那一步就没了。这条边界同样适用于 `bytes`——序列化后的二进制不能靠 `str()` 变回可读内容。

最后，`ListMemory` 的「无查询」其实是个有价值的基准：它把记忆退化成「一份固定前言」。很多场景（用户偏好、品牌语调、领域约束）确实只需要固定前言，不需要检索。先用它跑通，再换检索型实现，是这套抽象最合理的用法。
