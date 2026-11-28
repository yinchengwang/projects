---
project: ragas
feature: prompt-base
title: 提示契约
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/prompt/base.py
    symbols: [BasePrompt, StringPrompt, StringIO, BoolIO]
---

# 提示契约

## 一句话职责

`BasePrompt` 定下「提示对象必须能生成一次、能生成多次、能存取到磁盘」这三条边，`StringPrompt` 是最小的纯文本实现，而 `StringIO` / `BoolIO` 是给结构化提示复用的两种单字段输入/输出形态。

## 机制是什么

ragas 的提示层不是「一段字符串 + format」，而是**「一种输入模型 → 一种输出模型」的可调用对象**。`BasePrompt` 只约束三件事：

- `generate(llm, data, ...) -> 单个输出`；
- `generate_multiple(llm, data, n, ...) -> n 个输出`；
- `save` / `load` 把提示的状态落成 JSON。

它自己不带任何具体的输入/输出类型——`data` 与返回值都是 `t.Any`。这正是 E 组第二页（`PydanticPrompt`）存在的理由：用泛型把 `Any` 换成两个具体的 pydantic 模型。

`BasePrompt` 还带三个与语言相关的字段：`name`（缺省由类名转 snake_case）、`language`（缺省 `english`）、`original_hash`（用于判断这份提示是否被改写语言）。

`StringIO` 与 `BoolIO` 是两个「最简单形态」的 IO 实现：前者包一个 `text: str`，后者包一个 `value: bool`。它们不是给 `StringPrompt` 用的，而是给**任何只需要一段文本或一个布尔值作为输入/输出的结构化提示**复用——例如判定某条陈述是否有上下文支持，输入是文本、输出是布尔。

两个 IO 都自定义了 `__hash__`（分别哈希 `text` 与 `value`）。这一条是为缓存服务的：提示对象整体要能哈希（`PydanticPrompt.__hash__` 会把 examples 序列化后拼进 SHA-256），而 IO 实例是 examples 的一部分。

## 怎么实现的

**`StringPrompt` 完全不做格式化。** `generate` 把 `data` 直接包成 `StringPromptValue(text=data)` 交给 `llm.agenerate_text`，再从 `llm_result.generations[0][0].text` 取文本。`generate_multiple` 是同一个调用，只是传 `n`，然后把 `generations[0]` 整批展平成列表。

也就是说，`data` 就是最终提示正文，调用方要自己把变量填好。类 docstring 里写着「instruction 字段可被 f-string 格式化」，但类体里并没有 `instruction` 字段——这句 docstring 与实现不符，看代码比看文档可靠。

`generate_multiple` 的 docstring 补了一条实现细节之外的事实：开启缓存时每个输出会被唯一缓存，历史上曾因缓存导致返回重复输出，现已修正。这条注释解释了为什么「n 次采样」在缓存开启后仍能得到不同结果。

**`save` / `load` 是基类的，且只存元数据。** `BasePrompt.save` 写入三个键：`ragas_version`、`language`、`original_hash`，若目标文件已存在则抛 `FileExistsError`。`load` 读回这三个键，跨版本时只 `logger.warning` 不报错，然后用 `cls(language=..., original_hash=...)` 重建。

由此有一个直接后果：`StringPrompt` 没有覆写 `save`/`load`，所以它的存取几乎是无内容的——提示正文本来就不在对象里（正文来自调用时传入的 `data`），`save` 存下来的只有语言标签。真正需要持久化提示内容的是 `PydanticPrompt`，它把 instruction 与 examples 一起写盘（见下一节）。

**`BasePrompt.__init__` 有一处赋值遗漏。** 只有 `name is None` 分支才写 `self.name`；调用方显式传入 `name` 时 `self.name` 从未被赋值，而 `__repr__` 会读它。这是当前实现里的真实缺陷，不是设计取舍。

**语言适配的位置。** `language` 与 `original_hash` 两个字段是为「把提示翻译成别的语言」这条路预留的：适配后的提示与原提示的差别记在 `original_hash` 上，`language` 记目标语言。基类只负责存取这两个字段，真正做翻译的是 `PydanticPrompt.adapt`。

## 关键取舍

**源码事实：** `BasePrompt` 的 `generate` / `generate_multiple` 都用 `t.Any` 形容输入与输出，没有泛型。类型信息完全交给子类自行声明，基类只管「有这两个方法」。

**源码事实：** `generate` 与 `generate_multiple` 是两个独立抽象方法，基类不提供「`generate` 等于 `generate_multiple(n=1)`」的默认实现。这个默认实现被下沉到了 `PydanticPrompt`（它确实这么做了），所以两种提示在这条语义上可以有不同的实现。

**源码事实：** `BasePrompt.save` 用 `FileExistsError` 拒绝覆盖已有文件，而不是静默覆盖。这条约束让「保存提示」变成一次性动作，避免误改已归档的提示版本。

**源码事实：** `StringIO` / `BoolIO` 自定义 `__hash__` 但没自定义 `__eq__`（pydantic 模型自带按字段比较的 `__eq__`）。两者配合起来才使「内容相同的两个 IO 实例」在哈希容器里等价。

**架构推论：** 把「输入/输出」表达成两个 pydantic 模型而不是函数签名，带来一个额外收益：**提示可以自动生成格式说明**。`PydanticPrompt` 直接拿 `output_model.model_json_schema()` 拼进提示，让「模型该怎么输出」由类型定义驱动，而不是手写格式描述——手写描述与解析器漂移是这类系统最常见的故障源。

**架构推论：** 语言字段放在基类而不是子类，说明「多语言提示」被当作横切能力而非某个提示的特性。代价是基类为一个多数子类不用的能力付了字段成本，收益是 `adapt` 这类操作有统一落点。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/prompt/base.py` | `BasePrompt` | 提示契约：单次/多次生成、存取、语言字段 |
| `src/ragas/prompt/base.py` | `BasePrompt.save` | 存元数据，拒绝覆盖已有文件 |
| `src/ragas/prompt/base.py` | `BasePrompt.load` | 读元数据，跨版本只告警 |
| `src/ragas/prompt/base.py` | `StringPrompt` | 纯文本实现，data 即正文，不做格式化 |
| `src/ragas/prompt/base.py` | `StringIO` | 单字段文本输入/输出形态，自定义哈希 |
| `src/ragas/prompt/base.py` | `BoolIO` | 单字段布尔输入/输出形态，自定义哈希 |
| `src/ragas/prompt/pydantic_prompt.py` | `PydanticPrompt` | 把 Any 换成输入/输出模型的子类（下一节） |

## 参考

- [ragas 源码仓库](https://github.com/explodinggradients/ragas)
- [ragas 源码：prompt/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/prompt/base.py)
- [ragas 文档：提示与输出解析](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/)
- [Pydantic 文档：模型与 JSON Schema](https://docs.pydantic.dev/latest/concepts/models/)

## 与主核 / AI 的关系

「提示 = 输入模型 → 输出模型的可调用对象」与主核里「算子 = 输入批次 → 输出批次」是同一种抽象层级：都把一个易变的东西（LLM 的自由文本、数据行的物理形态）压成一个有明确边界的函数。差别在于主核算子的边界由 schema 静态确定，而这里的输入/输出侧常常是 `Any`——**类型约束的强弱，决定了管道能自动到什么程度**。

对 agent 工程最实用的两条。其一：`StringIO` / `BoolIO` 这种「单字段 IO 形态」是好设计，它让「判定某句话是否被支持」这类二元问题不必为输入输出各造一个类，同时仍保留结构化解析的全部好处。其二：`StringPrompt` 这种「data 即正文、不做格式化」的最简实现要慎用——它把格式化责任推给调用方，一旦调用方漏填变量，提示里就会原样出现花括号占位符，而系统不会报错，只会安静地得到质量很差的输出。

另外值得记住的是 `save`/`load` 的分层：基类只存元数据，子类才存内容。看到 `BasePrompt.save` 里只有三个键，不必怀疑实现漏了——这是有意的分工。
