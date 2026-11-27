---
project: pydantic-ai
feature: agent-spec
title: Agent 规格与可序列化定义
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/agent/spec.py
    symbols: [AgentSpec, CapabilitySpecContext]
  - path: pydantic_ai_slim/pydantic_ai/_spec.py
    symbols: [NamedSpec, CapabilitySpec, build_registry]
---

# Agent 规格与可序列化定义

## 一句话职责

`AgentSpec` 把 Agent 的配置从 Python 构造参数外置成一份 YAML/JSON 可表达的数据契约；`NamedSpec` 是这份契约里「如何用名字加参数指向一个类」的通用编码，`build_registry` 则把「名字 → 类」的可信集合建出来，供反序列化时查找。

## 机制是什么

这一层的价值不只是「能用 YAML 写 Agent」，而是**把配置变成可以被 schema 约束的数据**：`AgentSpec.to_file` 顺手写出 `./{stem}_schema.json`，并在 YAML 顶部写入 `# yaml-language-server: $schema=...`，于是编辑器能在写配置时就校验字段与能力类型。

三个角色分工明确：

- `AgentSpec`：Agent 的**标量字段清单**（模型名、名字、描述、指令、输出 schema、模型设置、重试、结束策略、工具超时、元数据）+ 一个能力数组。
- `CapabilitySpec`（`NamedSpec` 的子类）：**能力条目的编码形式**，把「一个能力」写成 `'Name'`、`{'Name': arg}` 或 `{'Name': {k: v}}` 三种短形式之一。
- `build_registry`：名字与类的**白名单**，决定哪些类可以被配置引用。

关键设计是「**短形式的三态**」：无参数写成一个字符串；一个参数写成单键映射；多个参数也写成单键映射但值是对象。反序列化时必须能区分「单键映射的值是 kwargs」还是「一个位置参数恰好是个 dict」——这就是 `NamedSpec.args` / `kwargs` 那对属性需要小心处理的原因。

## 怎么实现的

**`AgentSpec` 的字段与别名。** 第一个字段是 `json_schema_path: str | None = Field(default=None, alias='$schema')`——用别名把 `$schema` 这个 JSON 约定键收进模型里，注释说明了意图：**避免 `$schema` 键导致校验失败**。其余字段类型刻意宽松：`deps_schema` / `output_schema` / `model_settings` / `metadata` 都是 `dict[str, Any] | None`，因为它们是「要送给别处去构造」的描述而不是本层要解释的结构；`instructions` 允许 `TemplateStr | str | list[...]`，因为模板字符串在 YAML 里就是普通字符串。

`end_strategy: EndStrategy = 'graceful'` 有默认值，`capabilities: list[CapabilitySpec] = []` 也有——因此 `exclude_defaults=True` 时这两项只有在被显式改动后才会写出。

**读写链路的四种入口。** `from_file` 推断格式后 `read_text`；这里有一处真实坑的注释：**不用 `encoding='utf-8-sig'`**，因为 `read_text` 遇到孤立的 `\xef` 或 `\xef\xbb` 会返回空串而不是报错，所以改成先读 `utf-8` 再 `removeprefix('\ufeff')`。`from_text` 按 `fmt` 分派 `from_json` / `yaml.safe_load`，PyYAML 缺失时抛 `ImportError` 并提示 `pip install "pydantic-ai-slim[spec]"`；解析结果必须 `is_str_dict`，否则 `UserError`。`from_dict` 就是 `model_validate`。

`to_file` 更复杂，因为它要同时产出配置与 schema：

- `schema_path` 可以是字符串模板，`{stem}` 会被替换成配置文件名的主干。
- schema 路径的引用形式分三种：相对路径直接用；绝对路径但落在配置目录内则转成相对；落在目录外则保留绝对。
- YAML 分支：`model_dump(mode='json', by_alias=True, context={'use_short_form': True}, exclude_defaults=True)` → `yaml.dump(sort_keys=False, allow_unicode=True)`，若有 schema 引用就在**第一行**插入 `# yaml-language-server: $schema=...`。
- JSON 分支：把 schema 引用塞进 context 的 `'$schema'` 键，交给 `_add_json_schema` 这个 `wrap` 序列化器在最外层拼上 `$schema`。
- 两个分支都带 `exclude_defaults=True`，所以导出的文件是**最小形式**。

`_save_schema` 还做了一次内容比对：文件存在且内容与生成结果完全一致就**不写**，避免每次导出都改 mtime。

**schema 生成为什么要单独一个模型。** `model_json_schema_with_capabilities` 里定义了一个局部的 `_AgentSpecSchema(BaseModel, extra='forbid')`，注释明确列出三点差异：`TemplateStr` 换成普通 `str`（模板在 YAML 里就是字符串）、`capabilities` 用解析后的**能力类型 Union** 而不是 `CapabilitySpec`、`extra='forbid'` 让生成的 schema 严格。它同时留下一条维护提醒：**改 `AgentSpec` 字段必须同步改这个类**——这是真实的重复，且被注释承认。

生成之后还有三处补丁：把 `title` 设成 `AgentSpec`；手动加 `$schema` 属性；从 `ModelSettings` 的 `$defs` 里**删掉 `additionalProperties`**，因为 `extra='forbid'` 会传播到嵌套类型，而模型设置必须允许供应商自定义字段。

**`NamedSpec` 的三态与「往返陷阱」。** `NamedSpec` 有 `name` 与 `arguments: None | tuple[Any] | dict[str, Any]`。反序列化用 `model_validator(mode='wrap')` 实现：先让标准校验试一次，失败再退到 `_SerializedNamedSpec`（一个 `RootModel[str | dict[str, Any]]`）解析短形式再转成 `NamedSpec`。

`_SerializedNamedSpec._args` 是这套编码的核心歧义裁决点：值是 dict 且**所有键都是 str** → 当 kwargs；否则 → 当单个位置参数（包成单元素 tuple）。`enforce_one_key` 校验器禁止映射里有多个键。

序列化侧 `serialize` 在 `use_short_form` 上下文里反向编码，并处理一个**往返对称性问题**：若单个位置参数本身会序列化成「字符串键的 dict」（例如 `ModelSettings`），按 `{Name: value}` 写出会在读取时被误判成 kwargs，所以这种情况**退回长形式**。判断函数 `serializes_as_string_keyed_dict` 用 `to_jsonable_python(value, serialize_unknown=True)` 先看可 JSON 化的形态。

**`build_registry` 的不对称规则。** 它接收 `custom_types` 与 `defaults` 两组，行为有意不同：

- 自定义类型：逐个 `validate`（若给了回调），取名字，名字为 `None` 视为「该类主动退出序列化」并抛错，名字重复也抛错。
- 默认类型：同样取名字，但用 `registry.setdefault(name, cls)` ——**允许自定义类型覆盖同名的默认类型，且不报错**。

这就是「内置能力可以被用户同名实现替换掉」的实现位置。`get_capability_registry` 在这一层上再套两个约束：自定义类必须是 `AbstractCapability` 子类，且必须带 `@dataclass`（检查 `'__dataclass_fields__' not in cls.__dict__`）。

**`CapabilitySpecContext` 的存在意义。** 它是一个 `__slots__` 小对象，装 `registry` 与 `instantiate` 两个东西，通过模块级 ContextVar `capability_spec_context` 传递。`load_capability_from_nested_spec` 靠它实现「嵌套能力复用外层上下文」：在 `Agent.from_spec` / `_resolve_spec` 内部调用时，用同一份注册表（含自定义类型）与同一个实例化回调；在上下文之外则回落到默认注册表与 `cls.from_spec(*args, **kwargs)`。

这个设计解的是**包装型能力**的问题：像 `PrefixTools(capability=...)` 这种能力内部还有一个内嵌能力，内嵌那个也必须能解析自定义类型；如果没有这层上下文，内层解析会看不到外层的自定义注册表。

**schema 里的能力 Union 怎么生成。** `build_schema_types` 遍历注册表，为每项用 `_target_schema_types` 产出若干「能加载该类的 spec 形态」类型：

- `Literal[name]`——当目标零参数或有默认值时存在（最短形式）；
- `{'Name': 值}`——短形式，要求目标接受一个位置参数；
- `{'Name': {...}}`——关键字形式，在参数多于一个、存在 keyword-only 参数、或唯一参数接受对象时存在。

`_spec_param_type_hints` 负责从签名与解析后的类型注解里剔除 `self`/`cls`、`*args`/`**kwargs`，把有默认值的参数标成 `NotRequired`；并且**若某个必填参数的类型无法用 JSON 表达**（`filter_serializable_type` 把它消掉了，例如 TypeVar 或 Callable），整个类就不产出任何 schema 形态——因为此时没有任何配置能加载它。

`_object_rejecting_type` 是个精细补丁：短形式的值若本身能接受 object（如 `ModelSettings`），读取时会被当 kwargs，所以给该类型加一条 `allOf` + `not: {type: object, propertyNames: {type: string}}` 的 JSON schema 约束，把「字符串键对象」排除出短形式。`_json_schema_accepts_object` 甚至要递归跟随 `$defs` 里的 `$ref`（带环检测），才能判断一个类型到底能不能接受对象。

## 关键取舍

**源码事实：** 短形式有三种，且「单键映射的值是 kwargs 还是位置参数」只能靠「键是否全为 str」推断。这带来了 `serializes_as_string_keyed_dict` 这个补偿：可 JSON 化成字符串键 dict 的单位置参数必须退回长形式。

**源码事实：** `build_registry` 对自定义类型与默认类型的重复处理不对称——自定义类型重名报错，默认类型可被自定义覆盖。这既是「允许替换内置实现」的入口，也是「两个自定义之间不许歧义」的边界。

**源码事实：** `to_file` 会顺带写 schema 文件，并且内容相同就不写。这让配置文件与编辑器提示保持同步，同时避免无意义写入。

**源码事实：** schema 生成用的是**另建一个局部模型**而不是继承 `AgentSpec`，注释承认这是一处必须手动同步的重复。理由是两者的类型意图不同（模板要退化成字符串、能力要展开成 Union、要严格禁止额外字段）。

**源码事实：** `ModelSettings` 的 `$defs` 必须手动删掉 `additionalProperties`，否则 `extra='forbid'` 会禁止供应商自定义设置——一个典型的「严格化副作用」。

**源码事实：** 必填参数的类型无法用 JSON 表达时，该类**不产出任何 schema 形态**。这意味着「schema 里没有这个能力」等价于「这个能力不能用配置写出来」。

**架构推论：** 把配置变成数据契约的收益是双向的：人可以用 YAML 写，工具可以用 schema 校验，程序可以用 dict 传递。代价是**每一层扩展都要在三处同时成立**——Python 构造参数、`AgentSpec` 字段、以及 schema 生成模型；漏一处的表现是「运行时能用但 YAML 里写不出来」。

**架构推论：** 用 `CapabilitySpecContext` 这个 ContextVar 传递注册表，使得嵌套能力的解析与最外层一致，而不需要把注册表沿着每个 `from_spec` 的参数列表传下去。这是「隐式环境 > 显式参数」的一个合理例外，因为它是**纯配置期**的作用域，不会跨越运行时边界。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/agent/spec.py` | `AgentSpec` | Agent 的声明式字段清单与 `$schema` 别名 |
| `pydantic_ai_slim/pydantic_ai/agent/spec.py` | `AgentSpec.from_file` | 读文件并去掉 BOM（不用 `utf-8-sig` 的原因） |
| `pydantic_ai_slim/pydantic_ai/agent/spec.py` | `AgentSpec.to_file` | 导出配置与 schema，含三种 schema 引用形式 |
| `pydantic_ai_slim/pydantic_ai/agent/spec.py` | `AgentSpec.model_json_schema_with_capabilities` | 局部严格模型的 schema 生成与三处补丁 |
| `pydantic_ai_slim/pydantic_ai/agent/spec.py` | `CapabilitySpecContext` | 经 ContextVar 传递的注册表与实例化回调 |
| `pydantic_ai_slim/pydantic_ai/agent/spec.py` | `load_capability_from_nested_spec` | 嵌套能力复用外层注册表，否则回落默认 |
| `pydantic_ai_slim/pydantic_ai/_spec.py` | `NamedSpec` | 三态短形式的编码与对称性补偿 |
| `pydantic_ai_slim/pydantic_ai/_spec.py` | `CapabilitySpec` | 专用于 schema 展开的 NamedSpec 子类 |
| `pydantic_ai_slim/pydantic_ai/_spec.py` | `build_registry` | 名字白名单，自定义可覆盖默认、自定义间不许重名 |
| `pydantic_ai_slim/pydantic_ai/_spec.py` | `build_schema_types` | 按签名推导每种能力可用的 spec 形态 |

## 参考

- [pydantic-ai 源码：agent/spec.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/agent/spec.py)
- [pydantic-ai 源码：\_spec.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_spec.py)
- [Pydantic AI 文档：Agent 规格（YAML/JSON）](https://ai.pydantic.dev/agent/#agent-specs)
- [Pydantic AI 文档：能力概览](https://ai.pydantic.dev/capabilities/)

## 与主核 / AI 的关系

「配置外置成数据 + 用 schema 约束」是主核里「参数表 + 校验 + 预设文件」的同一路线。主核做这件事的方式通常是写一个严格的解析器并逐项检查；这里走的是 **Pydantic 模型即契约**：模型定义既生成运行时校验，又生成 JSON schema，还顺手给编辑器用。收益是三者天然一致，代价是遇到 Pydantic 表达不了的东西（模板字符串、可调用、带前缀的名字）就得靠 `Annotated` 和自定义 schema 打补丁——本页最后那三段（`NotRequired` 的剥离、拒绝对象的 `allOf`、递归跟随 `$ref`）全是这类补丁。

对 AI 工程的实际意义在于：**给配置写 schema 不只是为了校验，而是为了让人和模型都能写对**。YAML 顶部那行 `$schema` 注释，本质上是把「这个文件允许哪些能力、每个能力要什么参数」的知识推到了编辑器里。反过来，`build_registry` 对「必填参数类型不可 JSON 化就不产出 schema」这一条，说明这套设计有意让**不可配置的能力在配置层面不可见**——这比允许写出来再在运行时报错要干净得多，也更符合「配置的可用空间应当可枚举」这一原则。
