---
project: langchain
feature: output-parsing
title: 输出解析
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/output_parsers/base.py
    symbols: [BaseOutputParser, BaseLLMOutputParser, BaseGenerationOutputParser]
  - path: libs/core/langchain_core/output_parsers/pydantic.py
    symbols: [PydanticOutputParser]
  - path: libs/core/langchain_core/output_parsers/json.py
    symbols: [JsonOutputParser]
---

# 输出解析

## 一句话职责

把模型返回的文本（或消息）变成结构化对象，并且**在流式场景下能在「JSON 还没写完」时也给出一个可用的中间结果**——失败时把原始输出与错误一起包成异常，供调用方决定重试还是放弃。

## 机制是什么

这套解析器要同时处理三个互相牵制的要求：

| 要求 | 后果 |
|---|---|
| 一次调用可能返回多个候选生成 | 接口必须接受 `list[Generation]` 而不仅是一段文本 |
| 流式输出逐块到达，JSON 长期不合法 | 需要 `partial=True` 的宽松解析路径 |
| 解析失败必须可被「重试钩子」利用 | 异常里要带原始输出 |

前两条决定了类的层次：三个基类分别对应「只关心文本」「关心整条生成」「关心流」三种粒度。第三条决定异常的形状。

而「重试钩子」这一点在源码里体现为一个很窄但关键的接口：`parse_with_prompt(completion, prompt)`。它的文档直说了用途：

> The prompt is largely provided in the event the `OutputParser` wants to retry or fix the output in some way, and needs information from the prompt to do so.

即**解析器可以拿到输入提示**，从而在失败时构造「修正请求」。默认实现忽略 prompt 直接调 `parse`——所以它是一个可选的扩展点。

## 怎么实现的

**三层基类，泛型参数 `T` 贯穿。**

```
BaseLLMOutputParser(ABC, Generic[T])
  ├─ BaseGenerationOutputParser  ← 绑定 Runnable[LanguageModelOutput, T]
  └─ BaseOutputParser            ← 绑定 Runnable，同时提供 parse()
```

三者关系：

- **`BaseLLMOutputParser`** 只有一个抽象方法 `parse_result(result, *, partial=False)`，以及一个用 `run_in_executor` 包装它的异步版。它不管 `parse(str)`，因为它面对的是「生成列表」而非「一段文本」。
- **`BaseGenerationOutputParser`** 直接继承 `RunnableSerializable[LanguageModelOutput, T]`，所以它**有 `invoke`**。它没有 `parse(str)`——因为有些解析器（如工具调用解析器）需要的是完整消息而非文本。
- **`BaseOutputParser`** 也继承 `Runnable`，但**多提供了 `parse_result` 的默认实现**（取第一个候选的文本、转调 `parse`）与抽象 `parse(text)`。

所以「是否需要 `parse(str)`」这条分界线决定了继承哪一个：解析纯文本用 `BaseOutputParser`，解析完整生成（含工具调用、元数据）用 `BaseGenerationOutputParser`。

**`invoke` 的两个分支把「输入是消息还是字符串」统一到 `Generation`。**

```python
def invoke(self, input, config=None, **kwargs):
    if isinstance(input, BaseMessage):
        return self._call_with_config(
            lambda inner_input: self.parse_result([ChatGeneration(message=inner_input)],
            input, config, run_type="parser",
        )
    return self._call_with_config(
        lambda inner_input: self.parse_result([Generation(text=inner_input)], input, config, run_type="parser")
    )
```

关键在于**两种输入都被包成对应的 Generation 变体**：消息包成 `ChatGeneration`，字符串包成 `Generation(text=...)`。于是 `parse_result` 只需处理 `Generation`，不必关心来源。

`run_type="parser"` 是给回调系统用的标签——让追踪记录知道这一段是解析器。

**`parse_result` 的默认实现只有一个决定：取第一个候选。**

```python
def parse_result(self, result: list[Generation], *, partial: bool = False) -> T:
    """The return value is parsed from only the first `Generation` in the result, which
    is assumed to be the highest-likelihood `Generation`."""
    return self.parse(result[0].text)
```

注释说明了「为什么是第 0 个」——**约定它是最可能的候选**。这个约定由产生 `Generation` 的一侧负责（通常是 `n=1` 或按概率排序）。解析器不参与排序，也不尝试「多个候选都解析、挑最好的」——那会把解析变成搜索。

`partial` 参数在这里**被接收但被忽略**（传了没用）。这是合理的：`parse(str)` 的签名没有 `partial`，所以默认实现无法传播它。只有覆写了 `parse_result` 的解析器（如 `JsonOutputParser`）才能用上它。

**异步默认实现用 `run_in_executor`，而文档明确指出了它的一个后果。**

```python
async def aparse_result(self, result, *, partial=False) -> T:
    """This method is used by `ainvoke` and `abatch`. The default implementation
    runs `parse_result` in an executor; it does not call `aparse`. Override this
    method to provide native async parsing, for example when using async I/O or
    clients bound to the caller's event loop."""
    return await run_in_executor(None, self.parse_result, result, partial=partial)

async def aparse(self, text):
    """The default implementation runs `parse` in an executor. To use native
    async parsing through `ainvoke` or `abatch`, override `aparse_result`
    instead; those entry points do not call this method by default."""
    return await run_in_executor(None, self.parse, text)
```

两段文档是这一页最值得记的「坑说明」：

- **`ainvoke` 与 `abatch` 走的是 `aparse_result`，不是 `aparse`**。所以覆写 `aparse` 对异步入口毫无影响。
- 要实现真正的原生异步解析，必须覆写 **`aparse_result`**。源码把这条写进了两个方法的文档里——因为这是很容易搞错的一处（覆写了 `aparse` 却发现还是跑在线程池里）。

这也解释了两层方法的存在理由：`aparse` 只服务于「显式只要文本的调用方」，而框架入口用的是 `aparse_result`。

**`OutputType` 的推导用了 pydantic 的泛型元数据。**

```python
@property
def OutputType(self) -> type[T]:
    for base in self.__class__.mro():
        if hasattr(base, "__pydantic_generic_metadata__"):
            metadata = base.__pydantic_generic_metadata__
            if "args" in metadata and len(metadata["args"]) > 0:
                return cast("type[T]", metadata["args"][0])
    msg = f"Runnable {self.__class__.__name__} doesn't have an inferable OutputType. Override the OutputType property to specify the output type."
    raise TypeError(msg)
```

即**沿 MRO 找第一个带泛型实参的基类，取其第一个类型参数**。所以 `BooleanOutputParser(BaseOutputParser[bool])` 的 `OutputType` 自动是 `bool`。

这个推导的价值在 schema 生成：`Runnable` 用它构建输入/输出 schema，而结构化输出解析器必须能声明「我输出什么类型」。若推导不出，报 `TypeError` 并提示覆写。

注意它遍历的是 `mro()` 而非 `__orig_bases__`——因为走 MRO 能找到多层继承链上最近一次泛型化。这样 `class MyParser(SomeSpecificParser[Foo])` 也能推导对。

`BaseGenerationOutputParser` 的 `OutputType` 实现则不同：`return cast("type[T]", T)`，带一句注释「even though mypy complains this isn't valid, it is good enough for pydantic to build the schema from」。即它直接把类型变量 `T` 交出去——因为在「解析完整生成」这类解析器里没有 `pydantic_generic_metadata` 可查（它不一定是 pydantic 泛型）。这是一个「类型层不严格但运行可用」的务实选择。

**`JsonOutputParser` 的 `partial` 分支与完整分支的处理**是这一页的核心机制。

```python
def parse_result(self, result, *, partial=False):
    if isinstance(result[0], ChatGeneration):
        raise_if_refused(result[0].message)
    text = result[0].text
    text = text.strip()
    if partial:
        try:
            return parse_json_markdown(text)
        except JSONDecodeError:
            return None
    else:
        try:
            return parse_json_markdown(text)
        except JSONDecodeError as e:
            msg = f"Invalid json output: {text}"
            raise OutputParserException(msg, llm_output=text) from e
```

四处：

1. **第一件事是检查拒答**（`raise_if_refused`）。注意它只在输入是 `ChatGeneration` 时做——因为拒答是消息级信息（`RefusalContentBlock` 或 `stop_reason`），纯文本 `Generation` 里没有。这一步**在解析之前**，因为拒答的输出通常不是可解析的 JSON，而「被拒答」这个事实比「JSON 无效」更有诊断价值。
2. **两条分支用的是同一个 `parse_json_markdown`**——注意不是 `parse_partial_json`。所以 `partial=True` 的「宽容」来自两点：失败时返回 `None` 而不是抛错；以及上一层的累积逻辑会让文本持续增长，直到某个时刻 JSON 恰好合法。
3. **`partial=True` 失败返回 `None`，而不是空字典**。这一点很关键：`None` 可被上游判为「还没到可解析的时刻」，而 `{}` 会被当成「解析出了一个空对象」。`BaseCumulativeTransformOutputParser` 的 `_transform` 里那句 `if parsed is not None and parsed != prev_parsed` 正是依赖这一点——`None` 不产出任何块。
4. **`OutputParserException` 带 `llm_output=text`**。这就是「重试钩子」的数据基础：异常里既有错误消息，也有**原始输出**，所以调用方可以把它拼进修正请求。

`parse(text)` 的实现只有一行：`return self.parse_result([Generation(text=text)])`。即**它把字符串包成伪装成「只有文本的生成」，然后走同一条路径**。所以 JSON 解析器的 `partial` 能力在 `parse` 上不可用——这与上面「默认 `parse_result` 忽略 `partial`」是同一类限制。

**流式累积才是 `partial` 真正的工作场景，而实现里有一个「只在变化时产出」的守卫。**

```python
def _transform(self, input: Iterator[str | BaseMessage]) -> Iterator[Any]:
    prev_parsed = None
    acc_gen: GenerationChunk | ChatGenerationChunk | None = None
    for chunk in input:
        chunk_gen = ...  # 按类型包装
        acc_gen = chunk_gen if acc_gen is None else acc_gen + chunk_gen
        parsed = self.parse_result([acc_gen], partial=True)
        if parsed is not None and parsed != prev_parsed:
            if self.diff:
                yield self._diff(prev_parsed, parsed)
            else:
                yield parsed
            prev_parsed = parsed
```

五处：

1. **`acc_gen` 是累积的**：每来一个块就 `acc_gen + chunk_gen`。所以 `parse_result` 每次都拿到「到目前为止的全部输出」，而不是单个块。
2. **解析失败给 `None`，不产出**。所以流式期间「JSON 还没写完」的那些时刻被静默跳过。
3. **`parsed != prev_parsed` 才产出**。这个守卫避免了「每个 token 都产出一次几乎相同的结果」。它依赖解析结果的 `__eq__`——对 JSON 来说比较的是字典相等。
4. **`diff` 开关**：为 `True` 时产出 `_diff(prev, next)`（默认实现抛 `NotImplementedError`，由子类定义差分格式）。`JsonOutputParser` 的文档说 `diff=True` 时「yields `JSONPatch` operations describing the difference between the previous and the current object」——即用 `jsonpatch` 库算差集。
5. **`BaseMessage` 会被转成 `BaseMessageChunk`**：`BaseMessageChunk(**chunk.model_dump())`。这一步是必需的——因为 `acc_gen + chunk_gen` 要求两侧都是 chunk 类型（b01 讲过 `BaseMessageChunk.__add__` 是拼接，而 `BaseMessage.__add__` 是构造模板）。

**三个 chunk 类型的选择顺序是有讲究的。**

```python
if isinstance(chunk, BaseMessageChunk):
    chunk_gen = ChatGenerationChunk(message=chunk)
elif isinstance(chunk, BaseMessage):
    chunk_gen = ChatGenerationChunk(message=BaseMessageChunk(**chunk.model_dump()))
else:
    chunk_gen = GenerationChunk(text=chunk)
```

先判 `BaseMessageChunk`（它本身就是 chunk，直接包）；再判 `BaseMessage`（已是成品消息，需要降级成 chunk）；最后是字符串。**顺序不能颠倒**——`BaseMessageChunk` 是 `BaseMessage` 的子类，若先判父类会走进「降级」分支，把一个 chunk 又重建一遍。

**`PydanticOutputParser` 在 `JsonOutputParser` 之上只加一层「校验」。**

```python
class PydanticOutputParser(JsonOutputParser, Generic[TBaseModel]):
    pydantic_object: Annotated[type[TBaseModel], SkipValidation()]

    def _parse_obj(self, obj: Any) -> TBaseModel:
        try:
            if issubclass(self.pydantic_object, pydantic.BaseModel):
                return self.pydantic_object.model_validate(obj)
            if issubclass(self.pydantic_object, pydantic.v1.BaseModel):
                return self.pydantic_object.parse_obj(obj)
            msg = "Unsupported model version for PydanticOutputParser: ..."
            raise OutputParserException(msg)
        except (pydantic.ValidationError, pydantic.v1.ValidationError) as e:
            raise self._parser_exception(e, obj) from e
```

五处：

1. **`SkipValidation()` 注解**：pydantic 不把 `pydantic_object` 当成模型字段来校验/序列化（它是一个**类**而不是实例，pydantic 默认处理不了）。
2. **同时支持 pydantic v1 与 v2**。这是必要的兼容层——`issubclass` 两支分别走 `model_validate` 与 `parse_obj`。
3. **两个分支都被 `except` 覆盖**：`(pydantic.ValidationError, pydantic.v1.ValidationError)` 一起捕获——因为同一段代码可能遇到任一版本的错误。
4. **`_parser_exception` 把 JSON 对象重新序列化成字符串**（`ensure_ascii=False`），再把它拼进错误消息并放入 `llm_output`。注意它丢掉了原始文本里的格式（空白、键序），只保留语义。理由大概是「重试提示里要给出规范化的 JSON 更容易被模型修正」，但代价是丢了原始缩进等线索。
5. **`parse_result` 的 `partial` 特殊处理**：

```python
try:
    json_object = super().parse_result(result, partial=partial)
    return self._parse_obj(json_object)
except OutputParserException:
    if partial:
        return None
    raise
```

即**流式期间连「校验失败」也要静默**。这条比 JSON 层面更宽松：一个只写了 `{"name": "Al"}` 的对象可能在 Pydantic 模型上缺了必填字段，而此时不该抛错——因为后续块还会补上。所以 `partial=True` 时把 `OutputParserException`（含校验失败）转成 `None`。

注意这里捕获的是 `OutputParserException` 而不是 `ValidationError`——因为 `_parse_obj` 已经把校验错误包成了 `OutputParserException`。用同一类型统一处理，让「部分模式下所有失败都返回 None」这条规则只需一个 `except`。

`@overload` 两个签名让类型检查器知道 `partial=False` 时返回 `TBaseModel`（不是 `Optional`），而 `partial=True` 时是 `TBaseModel | None`。这是**用重载表达「参数值决定返回是否可空」**的做法——Python 类型系统本身无法表达这种依赖，所以只能靠重载。

**`get_format_instructions` 会主动删掉 schema 里的两个键。**

```python
schema = dict(self._get_schema(self.pydantic_object).items())
reduced_schema = schema
if "title" in reduced_schema:
    del reduced_schema["title"]
if "type" in reduced_schema:
    del reduced_schema["type"]
schema_str = json.dumps(reduced_schema, ensure_ascii=False)
```

删 `title` 与顶层 `type` 的理由可以从结果推断：`title` 会变成类的名字（对模型无用且可能误导），顶层 `type` 恒为 `"object"`（多余）。而注释写的是「Remove extraneous fields」——即去噪。

注意 `dict(self._get_schema(...).items())` 这层复制，注释说是「Copy schema to avoid altering original Pydantic schema」。真的必要吗？`del` 只作用于顶层字典，而 `dict(...)` 是浅复制——所以确实需要，否则会改掉 `_get_schema()` 返回的原对象。但浅复制意味着嵌套的 `properties` 仍共享，只是这里不删嵌套，所以安全。

`ensure_ascii=False` 是另一个细节：它让中文等非 ASCII 字符原样出现在提示里而不是 `\uXXXX` 转义。对模型理解 schema 里的中文字段描述很关键。

**两个解析器都做了「拒答优先于解析」的检查，而 `raise_if_refused` 是共用的小模块。**

`JsonOutputParser.parse_result` 的第一句就是 `if isinstance(result[0], ChatGeneration): raise_if_refused(result[0].message)`。而 `PydanticOutputParser` 通过 `super()` 继承了这个行为。

这个顺序的含义：**当模型拒答时，报「模型拒绝」比报「JSON 无效」更好**。因为拒答是一个明确的语义事件（用户应该看到「模型拒绝回答」而不是一个解析错误），而拒答的文本往往恰好不是合法 JSON。

## 关键取舍

**源码事实：** 三层基类按「是否需要 `parse(str)`」分界：`BaseOutputParser` 有 `parse`（面对纯文本），`BaseGenerationOutputParser` 无（面对完整生成）。

**源码事实：** `invoke` 把消息包成 `ChatGeneration`、把字符串包成 `Generation`，所以 `parse_result` 只需处理 `Generation` 一种输入。

**源码事实：** 默认 `parse_result` 只取第一个候选并假定它是最高概率项，且忽略 `partial` 参数（因为 `parse(str)` 签名里没有它）。

**源码事实：** `ainvoke` / `abatch` 走 `aparse_result` 而非 `aparse`；要实现原生异步解析必须覆写 `aparse_result`。

**源码事实：** `OutputType` 从 MRO 上的 `__pydantic_generic_metadata__` 取第一个类型参数；`BaseGenerationOutputParser` 则直接把类型变量交出去并注明「pydantic 能用就够了」。

**源码事实：** `JsonOutputParser.parse_result` 在解析之前先 `raise_if_refused`（仅 `ChatGeneration`），`partial=True` 失败返回 `None`、`partial=False` 失败抛 `OutputParserException` 且带 `llm_output`。

**源码事实：** 流式累积每次把全部块相加后以 `partial=True` 解析，失败（`None`）不产出，且只在解析结果变化时才产出；`diff=True` 时产出子类定义的差分。

**源码事实：** chunk 类型判断顺序为 `BaseMessageChunk` → `BaseMessage`（降级为 chunk）→ 字符串，顺序不能颠倒。

**源码事实：** `PydanticOutputParser` 在 `partial=True` 时把 `OutputParserException`（含 Pydantic 校验失败）也转成 `None`，并用 `@overload` 表达「`partial=False` 时返回非可空」。

**源码事实：** `get_format_instructions` 从 schema 里删掉 `title` 与顶层 `type`，用 `ensure_ascii=False` 序列化，并在复制后删除以免改动原 schema。

**架构推论：** 「`partial` 失败返回 `None` 而不是空值或抛出」这个三元选择是整个流式解析的核心。它让上游能区分三种状态：**还没到可解析的时刻（`None`）**、**解析出了但内容变了（产出）**、**解析出了且没变（不产出）**。若把「不可解析」也表达成 `{}`，那么「部分 JSON」与「真的空对象」就无法区分，流式 UI 会在开头闪一个空对象。

**架构推论：** 「重试钩子靠异常携带原始输出」这个设计比「返回 `(结果, 错误)` 元组」更贴合 Python 生态（异常可以被专门的 retry 包装捕获），代价是**调用方必须知道去哪取原文**（`llm_output` 属性）。而 `parse_with_prompt` 提供了一个额外入口：若解析器想主动修正，可以拿到输入提示。

**架构推论：** 三个基类按「输入粒度」而非「功能」分层，是这一页最值得带走的结构性观察。分层的判据不是「哪个更通用」，而是**「这个解析器需要看到什么」**：只看到文本的用 `BaseOutputParser`，需要看到完整消息（工具调用、拒答、元数据）的用 `BaseGenerationOutputParser`。选错基类会导致「拿不到需要的信息」或者「被迫实现用不到的 `parse`」。

**架构推论：** `PydanticOutputParser` 在 `partial` 时连校验失败也静默，隐含一个前提：**流式的中间状态本来就必然不合法**。半截 JSON 必然缺字段，所以对一个增量解析器来说，「校验失败」不是错误信号而是正常状态。这条与本篇开头说的「JSON 长期不合法」是同一件事的另一个面。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/output_parsers/base.py` | `BaseLLMOutputParser` | 最抽象的层：只有 `parse_result` 与其异步版 |
| `libs/core/langchain_core/output_parsers/base.py` | `BaseGenerationOutputParser` | 面向完整生成的解析器，绑定 `Runnable[LanguageModelOutput, T]` |
| `libs/core/langchain_core/output_parsers/base.py` | `BaseOutputParser` | 面向纯文本，提供 `parse` 与默认 `parse_result` |
| `libs/core/langchain_core/output_parsers/base.py` | `BaseOutputParser.invoke` | 把消息/字符串统一包成 Generation 变体 |
| `libs/core/langchain_core/output_parsers/base.py` | `BaseOutputParser.aparse_result` | 异步入口的默认实现，文档指出覆写 `aparse` 无效 |
| `libs/core/langchain_core/output_parsers/base.py` | `BaseOutputParser.OutputType` | 从 MRO 的 pydantic 泛型元数据推导输出类型 |
| `libs/core/langchain_core/output_parsers/base.py` | `BaseOutputParser.parse_with_prompt` | 重试钩子：解析器可拿到输入提示 |
| `libs/core/langchain_core/output_parsers/json.py` | `JsonOutputParser` | JSON 解析：拒答优先、`partial` 返回 `None` |
| `libs/core/langchain_core/output_parsers/json.py` | `JsonOutputParser.get_format_instructions` | 删 `title`/`type` 去噪后的 schema 提示 |
| `libs/core/langchain_core/output_parsers/pydantic.py` | `PydanticOutputParser` | 在 JSON 之上做模型校验，`partial` 时静默失败 |
| `libs/core/langchain_core/output_parsers/pydantic.py` | `PydanticOutputParser._parser_exception` | 把校验错误包成带 `llm_output` 的异常 |
| `libs/core/langchain_core/output_parsers/transform.py` | `BaseCumulativeTransformOutputParser._transform` | 流式累积、失败不产出、仅在变化时产出、支持 `diff` |

## 参考

- [langchain 源码：output_parsers/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/output_parsers/base.py)
- [langchain 源码：output_parsers/json.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/output_parsers/json.py)
- [langchain 文档：结构化输出](https://docs.langchain.com/oss/python/langchain/structured-output)
- [RFC 6902：JSON Patch](https://www.rfc-editor.org/rfc/rfc6902)

## 与主核 / AI 的关系

「部分结果用 `None` 而非默认值来表达」与主核里的**三值逻辑**同源。主核的 `NULL` 与 `0` 绝不能混（聚合时 NULL 参与方式完全不同），本页的 `None` 与 `{}` 同理：前者是「还没有结果」，后者是「结果是空」。两者都属于「状态的存在性与值本身必须分开」。

「解析失败携带原始输出」与主核里的**带上下文错误**一致：主核在解析一条 SQL 失败时会给出出错位置与附近文本，而不只是「语法错误」。理由是同一件事——**错误信息要能被用来修正**。这里的 `llm_output` 就是「附近文本」的对应物。

「确定性证据不能被概率信号稀释」这一条在两边表现不同但结构相似：主核用硬约束覆盖成本估算，本页用**拒答优先于 JSON 无效**覆盖解析结果。判据相同——**哪一个事实更确定、更能解释当前结局，就先报它**。

对 AI 工程的实际意义在于：结构化输出是 agent 系统里最常见也最容易出错的一环，因为模型无法保证输出合法。这份实现给出的三条经验可直接用：**流式与完整解析必须走不同的失败策略**（前者返回「还没有」，后者抛错）、**解析器要能拿到输入提示以便自我修正**、以及**用重载表达「参数值决定可空性」**（`partial=False` 时结果一定非空，这对调用方的类型安全很重要）。
