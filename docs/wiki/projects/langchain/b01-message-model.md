---
project: langchain
feature: message-model
title: 消息模型与角色
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/messages/base.py
    symbols: [BaseMessage, BaseMessageChunk, TextAccessor]
  - path: libs/core/langchain_core/messages/ai.py
    symbols: [AIMessage, AIMessageChunk]
  - path: libs/core/langchain_core/messages/human.py
    symbols: [HumanMessage]
---

# 消息模型与角色

## 一句话职责

`BaseMessage` 是「对话里的一条消息」的统一表示：它把「这段话是什么角色」「内容是什么形态」「额外要透传给供应商什么」三件事压进一个类型，让上游的模型、提示模板、解析器都不必各自猜测消息的形状。

## 机制是什么

这里有一个必须解决的张力：**同一份消息定义要同时服务三类使用者**。

| 使用者 | 需要什么 |
|---|---|
| 模型实现 | 原始字段（`additional_kwargs`、`response_metadata`）原样透传给供应商 |
| 框架内部（模板、解析器） | 稳定的标准字段（`tool_calls`、`usage_metadata`） |
| 用户代码 | 简单读取（`message.text`） |

`additional_kwargs` 与 `response_metadata` 这两个字典就是为第一类留的逃生舱：**框架不认识但供应商需要的东西放这里，原样透传**。注释把它们分工说得很清楚——`additional_kwargs` 举例是「由模型供应商编码的工具调用」，`response_metadata` 举例是「响应头、logprobs、token 数、模型名」。

这条分工决定了整个消息体系的一个特征：**标准字段少而稳，原始字段多而不稳定**。凡是还没被标准化的东西，都先进两个字典；一旦被标准化（如 `tool_calls`、`usage_metadata`），就从字典里搬出来做成正式字段，并在 `lc_attributes` 里声明。

## 怎么实现的

**五个核心字段，其中一个用了「允许额外字段」的配置。**

```python
class BaseMessage(Serializable):
    content: str | list[str | dict[Any, Any]]
    additional_kwargs: dict[Any, Any] = Field(default_factory=dict)
    response_metadata: dict[Any, Any] = Field(default_factory=dict)
    type: str
    name: str | None = None
    id: str | None = Field(default=None, coerce_numbers_to_str=True)

    model_config = ConfigDict(extra="allow")
```

四处值得注意：

- **`content` 的类型是 `str | list[str | dict]`**，而不是更精确的联合。这是一个刻意的宽口径：内容可以是纯字符串，也可以是内容块列表，而块的具体形状由 b02 那套 TypedDict 表达。基类选择不收紧，因为不同供应商返回的形状确实不同，且收紧会破坏对旧数据的兼容。
- **`type: str` 而不是 `Literal[...]`**。注释说「Must be a string that is unique to the message type」，用途是「反序列化时识别类型」。不做成 `Literal` 的理由是**子类可扩展**——用户可以定义自己的消息类型（如 `ChatMessage` 带任意 `role`），基类不能把取值锁死。
- **`id` 带 `coerce_numbers_to_str=True`**。有些供应商返回数字 id，而字段声明是 `str | None`。用 pydantic 的强制转换而不是拒绝，是为了「不要因为 id 是数字就丢掉整条消息」。
- **`extra="allow"`**。这条最重要：子类可以加自己的字段（`AIMessage` 加 `tool_calls`），用户也可以往消息上挂任意属性。代价是**拼错字段名不会报错**（会被当成额外字段）。这与 `additional_kwargs` 的存在形成对照——前者是「框架允许你挂」，后者是「框架会把它们透传给供应商」。

**`TextAccessor` 是一次「不破坏旧代码」的类型技巧。**

```python
class TextAccessor(str):
    __slots__ = ()

    def __new__(cls, value: str) -> Self:
        return str.__new__(cls, value)

    def __call__(self) -> str:
        warn_deprecated(since="1.0.0", message="Calling .text() as a method is deprecated. Use .text as a property instead (e.g., message.text).", removal="2.0.0")
        return str(self)
```

做法是**继承 `str` 并实现 `__call__`**。于是：

- 属性访问 `message.text` 得到一个 `str` 子类，行为与字符串一致（可拼接、可比较、可索引）。
- 方法调用 `message.text()` 也合法——因为 `__call__` 让它可调用，返回 `str(self)` 并发出弃用警告。

这个技巧的价值在于**同一处代码同时支持两种语法**，且不需要给消息类加一个「已废弃的方法」。旧代码不报错、只警告；类型检查器看到的是 `str` 子类，所以 `message.text` 与 `message.text()` 都能通过。

注意 `__slots__ = ()`：不新增实例属性，所以 `TextAccessor` 的内存布局与 `str` 相同。这对一个每次访问都新建的对象很重要——若加了 `__dict__`，每次 `message.text` 都会多分配一个字典。

**`text` 属性的实现体现了「内容可能是列表」这件事的代价。**

```python
@property
def text(self) -> TextAccessor:
    if isinstance(self.content, str):
        text_value = self.content
    else:
        blocks = [
            block
            for block in self.content
            if isinstance(block, str)
            or (block.get("type") == "text" and isinstance(block.get("text"), str))
        ]
        text_value = "".join(
            block if isinstance(block, str) else block["text"] for block in blocks
        )
    return TextAccessor(text_value)
```

三处：

- **两种形态两支**：字符串直接返回；列表则**只挑 `type == "text"` 的块**，其余（图片、工具调用、推理）全部忽略。这是「纯文本视图」的定义——它不试图把多模态内容压平成文本。
- **同时接受块是裸字符串**（`isinstance(block, str)`）。因为 `content` 的列表元素类型是 `str | dict`，历史上允许直接放字符串。
- **用 `"".join` 而不是 `"\n".join`**：多个文本块之间不加分隔符。这是一个选择——块之间的语义边界本来就不明确（可能是同一段被打散，也可能是两段），加换行会凭空插入结构。

**`__add__` 是一个类型上的「提前返回」技巧。** 在 `BaseMessage` 里定义 `__add__` 返回 `ChatPromptTemplate`：

```python
def __add__(self, other: Any) -> ChatPromptTemplate:
```

即 `message + message` 不再拼接内容，而是**构造一个提示模板**。而子类 `BaseMessageChunk.__add__` 则重写为返回 `BaseMessageChunk`（拼接内容）。

这件事的含义：**「消息相加」在两条语义上都合理，而基类与 chunk 基类选择了不同的一个**。基类消息相加构造模板（因为「这两条消息构成一段提示」是最常见的意图），而块相加是拼接（因为流式增量必须能相加）。两个不同的返回类型由不同的类承担，避免了用一个 `Union` 返回值让调用方自己判断。

**`base.py` 里有一个私有函数，揭示了「旧字段迁移」的形态。**

```python
def _extract_reasoning_from_additional_kwargs(message: BaseMessage) -> ...:
    ...
    if reasoning_content is not None and isinstance(reasoning_content, str):
        return {"type": "reasoning", "reasoning": reasoning_content}
    return None
```

即**从 `additional_kwargs` 里把供应商的推理内容搬到标准内容块**。这正呼应前面那条分工：标准化的过程就是「从字典里搬到正式字段」，而这里是搬到内容块（`type: "reasoning"`），因为推理内容是内容的一部分而不是元数据。

注意它对类型做了检查（`isinstance(..., str)`），不满足就返回 `None`——即搬不动就放弃，不报错。这个方向与 b02 会讲的整体取向一致：未知形状降级，不让它中断流程。

**`AIMessage` 加了三个字段，每一个都是「从字典里搬出来」的结果。**

```python
class AIMessage(BaseMessage):
    tool_calls: list[ToolCall] = Field(default_factory=list)
    invalid_tool_calls: list[InvalidToolCall] = Field(default_factory=list)
    usage_metadata: UsageMetadata | None = None
    type: Literal["ai"] = "ai"
```

对照组很明显：`tool_calls` 的文档在 `additional_kwargs` 里是「由供应商编码的工具调用」，而这里变成了标准化的 `list[ToolCall]`。同一份信息有两个位置——原始的在字典里，标准化的是字段。

三个字段各自的取舍：

- **`invalid_tool_calls` 是独立字段而不是 `tool_calls` 里的一档**。理由是「模型给错了参数不该导致整个调用列表不可用」：合法的进 `tool_calls`，畸形的进 `invalid_tool_calls` 并带 `error` 字符串。这是「错误也是数据」的做法——把模型的错误保留下来让上层决定怎么处理（重试、忽略、喂回模型）。
- **`usage_metadata: UsageMetadata | None`** 而不是空字典。`None` 表示「这个供应商没报用量」，与「用量是 0」区分开。这个区分对成本统计很关键（见 b03）。
- **`type: Literal["ai"] = "ai"`** 在此处收紧了基类的 `str`。子类可以收紧——`AIMessage` 的类型永远是 `"ai"`。

**`AIMessageChunk` 三处新增，每一处都服务于流式。**

```python
class AIMessageChunk(AIMessage, BaseMessageChunk):
    type: Literal["AIMessageChunk"] = "AIMessageChunk"
    tool_call_chunks: list[ToolCallChunk] = Field(default_factory=list)
    chunk_position: Literal["last"] | None = None
```

- **`type` 用 `"AIMessageChunk"` 而不是 `"ai"`**。这里有一个微妙的后果：它是**大写的、与父类的判别位不同**。所以序列化/反序列化能区分「完整消息」与「增量块」。注意这与其他消息类型的风格不一致（其余都是小写 `"human"`/`"ai"`/`"system"`），但判别位只需要唯一，不需要风格统一。
- **`tool_call_chunks` 与 `tool_calls` 并存**。前者是**未拼装的碎片**（`args` 是 JSON 子串），后者是拼装后的完整调用。流式过程中只能得到碎片，拼装完成后才填 `tool_calls`。这个并存是流式架构的必然结果——两个字段代表同一份数据的两个阶段。
- **`chunk_position: Literal["last"] | None`** 是一个「终止信号」。流式的最后一块带 `"last"`，调用方据此知道流结束。用可选的单值字面量而不是 `bool`，是因为将来可能加其他位置标记（如 `"first"`）。

**`AIMessageChunk.content_blocks` 的重写值得单看**（虽然实现细节属于 b02 的范畴，这里只看它的存在理由）。基类的 `content_blocks` 从 `content` 解析块；chunk 需要**额外把 `tool_call_chunks` 也投影成内容块**，让流式期间的 UI 也能看到「正在成形的工具调用」。这是一次「同一份语义、两种来源」的合并。

**`HumanMessage` 是最简单的子类，而它的简单本身是一份信息。**

```python
class HumanMessage(BaseMessage):
    type: Literal["human"] = "human"
```

它没有加任何字段。与 `AIMessage` 的三个新字段对比，说明**复杂度的分布不是均匀的**：AI 消息承载了工具调用与用量，而人类消息只是文本加可选多模态内容。框架把复杂度集中在真正需要的地方，而不是给每个角色都配一套对称的字段。

（源码里还有 `example` 字段的说明与 `ai`/`human` 之外的 `system`、`tool`、`function`、`chat`、`notification` 等角色类，各自也只加必需的最小字段。）

## 关键取舍

**源码事实：** `BaseMessage` 只有五个核心字段（`content`、`additional_kwargs`、`response_metadata`、`type`、`name`、`id`），并设 `extra="allow"`，让子类与用户都能挂额外字段。

**源码事实：** `additional_kwargs` 与 `response_metadata` 是「框架不认识但供应商需要」的透传通道，二者分别面向「请求侧附加负载」与「响应侧元信息」。

**源码事实：** `content` 声明为 `str | list[str | dict]`，基类刻意不收紧为精确的内容块联合。

**源码事实：** `type` 在基类是 `str`（便于子类扩展），`AIMessage` 收紧为 `Literal["ai"]`，`AIMessageChunk` 用 `"AIMessageChunk"` 而非 `"ai"` 以区分增量块。

**源码事实：** `id` 用 `coerce_numbers_to_str=True`，容忍供应商返回数字 id。

**源码事实：** `TextAccessor` 继承 `str` 并实现 `__call__`，使 `message.text` 与 `message.text()` 都可用；方法调用发弃用警告，`__slots__` 为空以避免额外分配。

**源码事实：** `text` 属性只收集 `type == "text"` 的块并用空串拼接，不插入分隔符；非文本块全部忽略。

**源码事实：** `BaseMessage.__add__` 返回 `ChatPromptTemplate`（构造提示），而 `BaseMessageChunk.__add__` 返回 `BaseMessageChunk`（拼接内容）。

**源码事实：** `AIMessage` 把工具调用与用量从 `additional_kwargs` 提为正式字段，并把畸形调用单独放 `invalid_tool_calls`（带 `error`）而不是混进 `tool_calls`。

**架构推论：** 「标准字段 + 原始字典」的双轨结构是这类跨界抽象的标准解法。它承认一个事实：**框架不可能立刻为标准之外的一切建模**，所以必须留一个不解释的通道。代价是「同一份信息可能同时在两处」（例如工具调用既在 `additional_kwargs` 也在 `tool_calls`），需要迁移逻辑（`_extract_reasoning_from_additional_kwargs` 就是这种迁移的一支）。

**架构推论：** `invalid_tool_calls` 独立成字段，体现了「把模型的错误当数据而不是异常」的取向。这一点与 c02 的输出解析会形成呼应：解析失败不该让整条消息丢失，因为它对「让模型自我修正」这件事有信息价值。

**架构推论：** `TextAccessor` 这种「继承内置类型 + 实现 `__call__`」的技巧，本质上是**用类型系统做向后兼容**。它比「加一个已废弃的方法」更彻底（旧代码连属性名都不必改），比「同时提供两个名字」更省（不需要 `text` 与 `text_v2` 这种成对命名）。代价是可发现性差——`message.text()` 看起来该报错却能用，新读者会困惑。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/messages/base.py` | `BaseMessage` | 五个核心字段 + `extra="allow"` 的消息基类 |
| `libs/core/langchain_core/messages/base.py` | `BaseMessageChunk` | 增量块基类，`__add__` 重写为内容拼接 |
| `libs/core/langchain_core/messages/base.py` | `TextAccessor` | 继承 `str` 且可调用，兼容 `.text` 与 `.text()` |
| `libs/core/langchain_core/messages/base.py` | `BaseMessage.text` | 只收集文本块的空串拼接视图 |
| `libs/core/langchain_core/messages/base.py` | `BaseMessage.content_blocks` | 内容投影入口，含五家供应商的 block translator |
| `libs/core/langchain_core/messages/base.py` | `_extract_reasoning_from_additional_kwargs` | 把供应商推理内容从字典搬成标准块 |
| `libs/core/langchain_core/messages/ai.py` | `AIMessage` | 提为正式字段的 `tool_calls` / `invalid_tool_calls` / `usage_metadata` |
| `libs/core/langchain_core/messages/ai.py` | `AIMessageChunk` | 增量：`tool_call_chunks` 与 `chunk_position` |
| `libs/core/langchain_core/messages/human.py` | `HumanMessage` | 只声明 `type` 的最简角色类 |

## 参考

- [langchain 源码：messages/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/messages/base.py)
- [langchain 文档：消息类型](https://docs.langchain.com/oss/python/langchain/messages)
- [Pydantic 文档：模型配置与额外字段](https://docs.pydantic.dev/latest/concepts/models/#extra-fields)
- [Python 文档：`__slots__`](https://docs.python.org/3/reference/datamodel.html#slots)

## 与主核 / AI 的关系

「标准字段 + 原始字典双轨」与主核里的**元组格式与扩展属性**是同一类结构。主核的元组有固定的系统列（`xmin`、`ctid`、表 OID），而用户自定义的列值放在用户区；此外还有「扩展字段」（`attoptions`、`reloptions`）用来存引擎不解释但插件需要的键值。两者的共同点是**保留一个不解释的通道**，代价都是「同一份信息可能在两处」。

「把错误当数据」这条与主核里的**软错误**一致：主核在约束冲突时会带回是哪一行哪一列冲突，而不是只抛一个异常——因为这个信息对构建更好的错误消息、以及支持 `ON CONFLICT` 这类语义是必需的。这里 `invalid_tool_calls` 带 `error` 字符串做的是同一件事：让上层的「重试」或「喂回模型」有依据。

`TextAccessor` 那种向后兼容手法，在主核里对应的是**同一语义的两套语法（旧写法与标准写法）**，例如 `SERIAL` 与 `IDENTITY`。主核同时接受两者并在旧写法上给提示，本页的表现是同一种策略：**让旧代码继续工作，但每次使用都发声**。判据是「破坏旧代码的代价 vs 提示噪音的成本」。

对 AI 工程的实际意义在于：消息模型是 agent 框架的地基，它的字段划分决定了后续多少能力可以无损表达。这份实现给出的三条经验可直接用：**给未知留一个不解释的透传通道**（否则每接一个供应商就要改基类）、**把畸形输出独立成带错误信息的字段**（而不是丢弃或混入正常结果）、以及**用类型技巧做兼容而不是引入成对命名**。
