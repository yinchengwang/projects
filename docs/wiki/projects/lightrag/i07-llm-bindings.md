---
project: lightrag
feature: llm-bindings
title: 绑定的选项参数模型
page_type: feature
schema_version: 1
refs:
  - path: lightrag/llm/binding_options.py
    symbols: [BindingOptions, OpenAILLMOptions, OllamaLLMOptions]
  - path: lightrag/llm/binding_options.py
    symbols: [_parse_role_json, _make_bool_or_literal_parser, _resolve_optional_type]
---

# 绑定的选项参数模型

## 一句话职责

把「每个 LLM provider 有哪些参数、默认值是什么、环境变量叫什么、命令行怎么拼」从一个手写的清单变成**一次声明**：在 dataclass 上写一个字段，`BindingOptions` 就自动派生出 CLI 参数、环境变量名、`.env` 样例、以及按角色覆盖的解析规则——而所有解析器必须在「遇到看不懂的值」时做出**明确选择**（报错还是回退），因为这个选择决定了错的配置会在启动时暴露还是在请求时爆炸。

## 机制是什么

`BindingOptions` 的类注释列出了四件事，而它们全部由**类自省**驱动：

1. **配置管理**：每个 provider 的参数结构、默认值、类型。
2. **环境变量集成**：从参数名自动生成环境变量名（`_binding_name` 大写 + 字段名大写）。
3. **命令行接口**：为所有注册的绑定动态生成 `--<binding>-<field>` 参数。
4. **可扩展性**：用类自省自动发现所有子类，「加一个 provider 只需定义它自己的参数与帮助文本，基类处理其余一切」。

所以这一层的核心机制是**声明式**：一个字段声明同时产生四个消费面。而 `args_env_name_type_value` 是那个唯一的派生点：

```python
argdef = {
    "argname": f"{args_prefix}-{field.name}",     # openai-llm-temperature
    "env_name": f"{env_var_prefix}{field.name.upper()}",  # OPENAI_LLM_TEMPERATURE
    "type": _resolve_optional_type(field.type),
    "default": default_value,
    "help": f"{cls._binding_name} -- " + help.get(field.name, ""),
}
```

四种消费面都从这个 `argdef` 读，所以它们不可能对「这个字段叫什么」有分歧。

## 怎么实现的

**`dataclasses.fields` 而不是 `__dict__` 扫描，而两个分支的理由写在注释里。**

```python
if dataclasses.is_dataclass(cls):
    for field in dataclasses.fields(cls):
        if field.name.startswith("_"): continue
        if field.default is not dataclasses.MISSING:
            default_value = field.default
        elif field.default_factory is not dataclasses.MISSING:
            default_value = field.default_factory()
        else:
            default_value = None
```

三点：

1. **`is_dataclass` 分支优先** ——因为 dataclass 提供 `field.default` 与 `field.default_factory` 的**区分**，而 `__dict__` 扫描看不到「这个默认值是一个工厂」。
2. **`default_factory()` 被调用** ——所以 `stop: List[str] = field(default_factory=list)` 派生出 `[]` 而不是 `list` 类型对象。若不调，`.env` 样例里会写出 `<class 'list'>`。
3. **私有字段跳过** ——`_help` / `_binding_name` 这类 `ClassVar` 不是配置项。

而 `else` 分支（非 dataclass）保留「老方法」：从 MRO 收集类变量，用 `__annotations__` 拿类型。这个分支注释没说什么，但它的存在说明这个模块正在从「类变量 + 类型注解」迁移到 dataclass，而旧形态仍被支持。

**类型解析 `_resolve_optional_type` 做的是「剥掉 Optional 拿内层类型」。**

```python
def _resolve_optional_type(field_type):
    origin = get_origin(field_type)
    if origin in (list, dict, tuple): return field_type
    args = get_args(field_type)
    if args:
        non_none_args = [arg for arg in args if arg is not type(None)]
        if len(non_none_args) == 1: return non_none_args[0]
    return field_type
```

三处细节：

1. **`origin in (list, dict, tuple)` 先短路** ——因为 `List[str]` 的 `get_origin` 是 `list`，而 `get_args` 会返回 `(str,)`。若不加这个短路，`List[str]` 会被「剥」成 `str`，于是 CLI 参数变成单值字符串而非 JSON 数组。
2. **只剥掉 `NoneType` 后剩一个** ——所以 `int | None` → `int`，而 `str | int` 保持原样（有歧义的联合不猜）。
3. **`is type(None)`** ——用身份比较而不是 `== NoneType`。

第 1 点是这个函数最容易写错的地方，而它的注释（docstring）只说「返回 Optional/Union 注解的具体类型」，没有解释为什么要短路容器类型——这一点只能从代码看出来。

**`add_args` 按类型分了五个分支，而每个分支的错误策略不同。**

| 分支 | 环境变量解析失败时 | 命令行解析失败时 |
|---|---|---|
| `List[str]` | 静默回退到 `SUPPRESS` | 抛 `ArgumentTypeError` |
| `dict` | 静默回退到 `SUPPRESS` | 抛 `ArgumentTypeError` |
| `bool \| Literal[...]` | **抛**（propagate 出 `parse_args()`） | 抛 |
| `bool` | `get_env_value` 转换，失败回退 | 抛 |
| 其他 | — | — |

前两个分支的代码很一致：

```python
env_value = get_env_value(f"{arg_item['env_name']}", argparse.SUPPRESS)
if env_value is not argparse.SUPPRESS:
    try: env_value = json_list_parser(env_value)
    except argparse.ArgumentTypeError: env_value = argparse.SUPPRESS
```

所以环境变量里的坏 JSON **不阻止启动**——它退化成「这个参数没设」，于是默认值生效。而命令行里的坏 JSON 会抛，因为命令行是显式输入，报错是应该的。

这个「环境变量宽容、命令行严格」的分工很实用：环境变量常常由外部系统注入（容器编排、CI），一个拼错不该让服务起不来；而命令行是人手打的，报错能立刻纠正。

而 bool-or-literal 分支**故意反过来**，理由写在 `_make_bool_or_literal_parser` 的 docstring 里：

> 不认识的值**抛而不是回退**。一个纯 bool 选项可以承受「不是真就是假」，但这里一个拼写错误（`hgih` 而不是 `high`）否则会被静默降级为**关**——把一个用户想调高的特性变成他关掉的特性，而日志里什么都没有。这也**故意不同于 `add_args` 的 list/dict 分支**，那些把不可解析的环境值当作未设：抛出让错误在服务器还在启动时暴露。

这段是本页最有价值的判断。它把「宽容 vs 严格」的判据说清了：

- **`List` / `dict` 的坏值** → 退化成「未设」→ 默认值 → 行为是「这个选项没生效」。**可接受的降级**。
- **`bool | Literal` 的坏值** → 若退化成「未设」→ 而它可能是 `think` 这类 `False` 是默认值的选项 → 行为是「**特性被关掉了**」。**不可接受的降级**，因为用户想开的特性变成了关的。

差别在于「退化到默认值」这个动作的方向：对前者，默认值是「无操作」；对后者，默认值可能是「关闭」。

**`_bool_or_literal_choices` 里有一段关于 `typing.Union` 的坑，值得单独记。**

```python
if get_origin(field_type) is not Union: return None
args = get_args(field_type)
literals = [arg for arg in args if get_origin(arg) is Literal]
if len(args) != 2 or len(literals) != 1 or bool not in args: return None
```

docstring：

> 用 `typing.Union` 是**故意的**：`bool | Literal[...]` 构造的是一个 `types.UnionType`，它的 `get_origin` **不是** `Union`，而这个检测会静默漏掉它。

这是一个真实的 Python 3.10+ 陷阱：`X | Y` 语法产生的对象与 `Union[X, Y]` 是不同类型，而 `get_origin` 分别返回 `types.UnionType` 与 `typing.Union`。所以若注解写成新语法，这个检测会返回 `None`（不识别），于是分支落到「plain bool」路径，`Literal` 的值域完全丢失。

而 `len(args) != 2` 这个检查保证了「恰好 `bool` 加一个 `Literal`」这个形状，不多不少。

**`_make_bool_or_literal_parser` 里的优先级是「布尔词 → 字面量词 → 抛」。**

```python
def parse(value):
    if isinstance(value, bool): return value
    text = str(value).strip().lower()
    if text in _BOOL_TRUE_WORDS: return True
    if text in _BOOL_FALSE_WORDS: return False
    if text in choices: return text
    raise argparse.ArgumentTypeError(f"{env_name}={value!r} is not valid: expected a boolean (...) or one of {', '.join(choices)}")
```

四个顺序要点：

1. **`isinstance(value, bool)` 最先** ——所以已经是布尔的值原样返回（argparse 可能传布尔默认值）。
2. **布尔词先于字面量** ——所以 `true`/`false` 不会被误判为字面量（除非字面量里正好有 `"true"`，而那时布尔语义更合理）。
3. **`.strip().lower()`** ——所以 `"High"`、`" high "` 都能匹配。
4. **错误消息列出全部合法值** ——所以用户看到错误就知道该写什么。

而布尔词表来自共享常量：

```python
_BOOL_TRUE_WORDS = ENV_BOOL_TRUE_WORDS
_BOOL_FALSE_WORDS = ENV_BOOL_FALSE_WORDS
```

注释：「与 `get_env_value` 共享，所以环境变量解析器、CLI 解析器与 bool-or-literal 解析器**不可能漂移**」。

这与 h03 里「占位符预留与展开共用同一个正则」是同一手法：**两个必须一致的东西共用一个常量**，而不是各写一份然后靠测试对齐。

**`_parse_role_json` 与 `_UNSET` 一起构成了「角色覆盖」的解析骨架。**

```python
def _parse_role_json(env_name, raw, field_type):
    expected = dict if field_type is dict else list
    try: parsed = json.loads(raw)
    except ValueError: parsed = _UNSET
    if isinstance(parsed, expected): return parsed
    logger.warning("Environment variable %s=%r is not a valid JSON %s, using default %r", env_name, raw, "object" if expected is dict else "array", _UNSET)
    return _UNSET
```

而 `_UNSET` 的 `__repr__` 返回 `"<not set: base option or provider default>"`：

```python
class _Unset:
    """Marks "the env value was unusable" for paths that must leave a field unset.
    Passed to get_env_value as the default, so its repr is what the operator reads in the "using default ..." warning."""
    def __repr__(self): return "<not set: base option or provider default>"
```

这个设计的巧妙之处在于：**「未设」这个状态需要一个可打印的代言人**。用 `None` 不行（`None` 是合法值），用字符串也不行（会与真实值混淆）。所以用一个单例对象，而它的 `__repr__` 就是给运维看的那句话。

而 `get_env_value(role_env, _UNSET, field_type)` 之后用 `if parsed is not _UNSET:` 判断——**身份比较而不是相等比较**，因为单例。

**`options_dict_for_role` 是这一页最复杂的方法，而它的两个分支对应两种部署形态。**

```python
if is_cross_provider:
    base: dict[str, Any] = {}
else:
    base = cls.options_dict(args)
```

docstring：

```
同 provider：
- 从已解析的 args 继承基础绑定的选项
- 用任何角色特定的环境变量覆盖
跨 provider：
- 从空的 provider 选项开始
- 用任何角色特定的环境变量覆盖
```

这个区分解决一个具体问题：若 `query` 角色用 OpenAI 而基础绑定是 Ollama，那 `ollama_llm` 的选项（`num_ctx`、`top_k`）**不能**被传给 OpenAI —— 它们对 OpenAI 无意义或会被拒。所以跨 provider 时基座必须是空的。

而「角色特定的环境变量」的命名规则被写成了一个例子：

```python
role_env = f"{role_upper}_{original_env}"   # EXTRACT_OPENAI_LLM_TEMPERATURE
```

所以 `{ROLE}_{BINDING_PREFIX}_{FIELD}` 三层拼接，而 `original_env` 已经是 `OPENAI_LLM_TEMPERATURE`。

注意它还从 `original_env` 里剥掉 binding 前缀得到 `field_name`：

```python
field_name = original_env[len(env_prefix) :].lower()   # temperature
```

所以最终 `base["temperature"] = ...` ——provider 收到的 kwargs 用裸字段名，与 dataclass 字段名一致。

**这个循环里那段注释说明了「为什么不同的类型用不同的失败策略」。**

```python
# A typed value that does not parse must never reach the provider payload as its raw string, where it
# would fail at request time far from the config. A bool-or-literal option raises (as its CLI flag
# does); every other typed option warns and stays unset, so the base option or the provider default applies.
```

两个要点：

1. **共同前提**：坏值**绝不能**原样当字符串传给 provider。否则它会在请求时失败，而那时离配置已经很远——错误消息会是 `invalid literal for int()` 出现在某次 LLM 调用的深处。
2. **分歧点**：`bool | Literal` 抛，其他警告并保持未设。

而实现上 `bool` / `int` / `float` 走 `get_env_value(role_env, _UNSET, field_type)` ——把 `_UNSET` 当默认值传进去，于是「解析失败」与「没设」在返回值上不可区分，但 `get_env_value` 会自己发警告。而 `list` / `dict` 走 `_parse_role_json`，它自己发警告。

所以三个分支都保证了「坏值不会被写进 payload」：

```python
if parsed is not _UNSET:
    base[field_name] = parsed
```

**`OpenAIResponsesLLMOptions` 被刻意做成独立命名空间，而注释说明了为什么。**

```python
# Deliberately a separate namespace from openai_llm: the Responses API spells several parameters
# differently (max_output_tokens rather than max_completion_tokens, a nested reasoning object rather
# than a flat reasoning_effort) and does not accept others at all. Sharing the OPENAI_LLM_* namespace
# would let Chat Completions settings leak into the Responses driver, where they are either wrong or
# rejected.
```

两条理由：

1. **参数名不同** ——`max_output_tokens` vs `max_completion_tokens`、嵌套 `reasoning` 对象 vs 扁平 `reasoning_effort`。
2. **有些参数完全不被接受** ——所以共享命名空间会让 Chat Completions 的设置「泄漏」进 Responses 驱动。

这是「**名字空间即隔离边界**」的具体应用：两个 API 的参数集不兼容，所以它们的配置命名空间必须分开。若共用一个，`OPENAI_LLM_MAX_COMPLETION_TOKENS` 会被两个驱动都读到，而其中一个必然误用。

**`OllamaLLMOptions` 与 `OllamaEmbeddingOptions` 共用 `_OllamaOptionsMixin`，而它带来的问题被 `_help` 里的一条说明解决了。**

```python
@dataclass
class OllamaEmbeddingOptions(_OllamaOptionsMixin, BindingOptions):
    _binding_name: ClassVar[str] = "ollama_embedding"
```

`_OllamaOptionsMixin` 有 30 多个字段（`num_ctx`、`top_k`、`mirostat`、`num_gpu`……），全部从 mixin 继承。所以 embedding 与 LLM 两个绑定都暴露同一组参数——即使其中大部分对 embedding 无意义。

而 `_help` 里 `think` 那条说明了这个问题的处理方式：

```python
"think": ("Enable the model's extended-thinking/reasoning trace (LLM only, ignored for embeddings). true/false or a reasoning level (low/medium/high, needs an Ollama server that supports levels). Leave unset to follow the model's own default -- setting true for a model without thinking support makes Ollama reject the request")
```

所以「仅限 LLM」这个事实被写在帮助文本里，而不是靠字段不出现在 embedding 绑定里。这是一个取舍：共用 mixin 换来「加一个参数只改一处」，代价是帮助文本必须标注适用范围。

而 `think` 这条帮助的最后一句是一个具体的坑：「给一个不支持思考的模型设 `true` 会让 Ollama 拒绝请求」——所以默认是「留空以跟随模型自己的默认」。

而 `think` 正是 `bool | Literal["low","medium","high"]` 那个特殊注解的实例。所以「CLI 分支顺序里 bool-or-literal 必须在 plain bool 之前」这条实现细节，就是为了这一个字段服务的：

```python
# Handle booleans that also accept a few named levels (Ollama's think). Checked before the plain-bool
# branch: the annotation is a Union, so `is bool` would miss it and argparse would be handed the Union
# itself as a value converter.
```

「`is bool` 会漏掉它」——因为注释是 `bool | Literal[...]`，`field_type is bool` 为假。若不提前检查，它会落到 else 分支，而那里 `resolved_type` 是那个 Union 对象，被当作 argparse 的 converter —— 于是 `--ollama-llm-think high` 会尝试 `Union[...]("high")` 并报一个完全无关的错误。

**`generate_dot_env_sample` 遍历的是 `cls.__subclasses__()`，所以它是「所有已 import 子类」的样例。**

```python
for klass in cls.__subclasses__():
    for arg_item in klass.args_env_name_type_value():
        if arg_item["help"]: sample_stream.write(f"# {arg_item['help']}\n")
        if arg_item["type"] is List[str] or arg_item["type"] is dict:
            default_value = json.dumps(arg_item["default"])
        else:
            default_value = arg_item["default"]
        sample_stream.write(f"# {arg_item['env_name']}={default_value}\n\n")
```

三点：

1. **每一行都被注释掉（`# ` 前缀）** ——所以把样例贴进 `.env` 不会意外设任何值。这是一个必要的细节：一个「可用样例」若每行都生效，会让所有参数等于默认值，而那与不设不同（`max_request_body_bytes_explicit` 那类区分会受影响）。
2. **`List` / `dict` 的默认值走 `json.dumps`** ——所以 `stop` 显示成 `[]` 而不是 `list()`。
3. **`__subclasses__()` 的边界** ——它只包含**已被 import** 的子类。所以样例的完整性取决于导入图，而 `python -m lightrag.llm.binding_options` 这个入口（样例顶部注释里写的）必然 import 全部。

第 1 点值得单独强调：`# ` 前缀让样例既「可以复制」又「零副作用」。这与 h03 里「未声明的 token 原样保留」是同一类思维——**让默认行为是「什么都不改」**。

**`options_dict` 用「剥前缀」而不是「按已知字段名过滤」。**

```python
prefix = cls._binding_name + "_"
skipchars = len(prefix)
options = {key[skipchars:]: value for key, value in vars(args).items() if key.startswith(prefix)}
```

所以它靠**命名约定**而不是**字段清单**。这带来一个细节：`vars(args)` 里任何以 `openai_llm_` 开头的键都会被收进来——包括 argparse 为这个 group 生成的全部参数。所以它等价于「这个 binding 的所有参数」这个集合。

而 docstring 给了一个例子说明意图：「如果 args 含 `{'ollama_num_ctx': 512, 'other_option': 'value'}` 而这是对 `OllamaOptions` 调用，它返回 `{'num_ctx': 512}`」。

**`_help` 是 `ClassVar[dict[str, str]]`，而它把「帮助文本」与「字段」分开维护。**

这个分离的代价是「加一个字段要改两处」（字段 + help 条目），而收益是字段声明保持紧凑。而代码对缺失的处理是宽容的：

```python
"help": f"{cls._binding_name} -- " + help.get(field.name, "")
```

`help.get(field.name, "")` 意味着**没有帮助文本的字段不会报错**，只是说明里少一句。所以这是一个「可以逐步补齐」的约定，而不是硬要求。

而 `_help` 里 `stop` 与 `extra_body` 的文本里带了转义示例：

```python
"stop": 'Stop sequences (JSON array of strings, e.g., \'["</s>", "\\n\\n"]\')',
"extra_body": 'Extra body parameters for OpenRouter of vLLM (JSON dict, e.g., \'"reasoning": {"reasoning": {"enabled": false}}\')',
```

所以帮助文本本身就是「这种类型的值该怎么写」的文档，而这对 JSON 类型的参数尤其必要——否则用户不知道该写 `["a","b"]` 还是 `a,b`。

## 关键取舍

**源码事实：** `BindingOptions` 的全部机制来自类自省：一个字段声明同时派生 CLI 参数名（`--<binding>-<field>`）、环境变量名（`<BINDING>_<FIELD>` 大写）、`.env` 样例、按角色覆盖的解析。

**源码事实：** `args_env_name_type_value` 优先走 `dataclasses.fields` 分支，因为它能区分 `field.default` 与 `field.default_factory`；`default_factory()` 被**调用**（所以 `stop` 派生 `[]` 而非 `list`）；私有字段跳过；非 dataclass 时保留旧的类变量 + `__annotations__` 路径。

**源码事实：** `_resolve_optional_type` 先短路 `list` / `dict` / `tuple`，否则 `List[str]` 会被剥成 `str`（`get_origin` 是 `list`，`get_args` 返回 `(str,)`），CLI 参数就从 JSON 数组退化成单值。

**源码事实：** 环境变量解析失败时，`List` / `dict` **静默回退到 `SUPPRESS`**（这一项等于没设），而命令行解析失败**抛 `ArgumentTypeError`**——环境变量常由外部注入，拼错不该让服务起不来；命令行是人手打的，报错能立刻纠正。

**源码事实：** `bool | Literal[...]` 分支**故意反过来**：环境变量里的坏值也**抛**，因为「一个拼写错误（`hgih`）否则会被静默降级为关——把用户想调高的特性变成他关掉的特性，而日志里什么都没有」。这与 list/dict 的宽容「是故意的不同」。

**源码事实：** `_bool_or_literal_choices` 必须用 `typing.Union` 判断——`bool | Literal[...]` 构造的是 `types.UnionType`，其 `get_origin` **不是** `Union`，用新语法写的注解会**静默漏掉**这个检测。

**源码事实：** CLI 分支里 bool-or-literal 必须在 plain bool 之前，否则 `field_type is bool` 为假，那个 Union 对象会被当成 argparse 的 converter。

**源码事实：** `_BOOL_TRUE_WORDS` / `_BOOL_FALSE_WORDS` 与 `get_env_value` **共享同一常量**，所以环境变量、CLI、bool-or-literal 三个解析器「不可能漂移」。

**源码事实：** `_UNSET` 单例存在的理由是「『未设』这个状态需要一个**可打印的代言人**」——`None` 是合法值不能用，字符串会混淆；它的 `__repr__` 就是运维在警告里读到的那句话。判定用 `is not _UNSET`（身份比较）。

**源码事实：** `options_dict_for_role` 的两个分支：同 provider 从 `options_dict(args)` 继承基座，**跨 provider 从空 dict 开始**——因为 Ollama 的 `num_ctx` / `top_k` 传给 OpenAI「要么错要么被拒」。

**源码事实：** 角色环境变量命名是 `{ROLE}_{BINDING_PREFIX}_{FIELD}`（如 `EXTRACT_OPENAI_LLM_TEMPERATURE`），而写进 provider payload 的键是**剥掉 binding 前缀的小写裸字段名**。

**源码事实：** 角色覆盖的共同前提是「坏值绝不能原样当字符串传给 provider，否则会在请求时失败而那时离配置已经很远」；分歧是 bool-or-literal 抛、其他类型警告并保持未设（于是基座选项或 provider 默认生效）。

**源码事实：** `OpenAIResponsesLLMOptions` **刻意独立命名空间**，因为 Responses API 的参数名不同（`max_output_tokens` vs `max_completion_tokens`、嵌套 `reasoning` vs 扁平 `reasoning_effort`）且有些参数完全不被接受——共享 `OPENAI_LLM_*` 会让 Chat Completions 的设置「泄漏」进 Responses 驱动。

**源码事实：** `OllamaEmbeddingOptions` 与 `OllamaLLMOptions` 共用 `_OllamaOptionsMixin` 的 30+ 字段，所以 embedding 绑定也暴露 LLM 参数；「仅限 LLM」写进 `_help["think"]` 的文本里而不是靠字段缺席。该条还警告「给不支持思考的模型设 `true` 会让 Ollama 拒绝请求」。

**源码事实：** `.env` 样例里**每一行都被 `#` 注释掉**，所以贴进 `.env` 零副作用；`List` / `dict` 的默认值走 `json.dumps` 所以显示成 `[]` 而非 `list()`。

**源码事实：** `options_dict` 按**命名前缀**剥取而不是按字段清单过滤，所以它等价于「这个 binding 的全部参数」这个集合。

**源码事实：** `_help` 缺条目时用 `help.get(field.name, "")` 宽容处理，所以它是「可以逐步补齐」的约定而不是硬要求。

**架构推论：** 这一层的价值是**把「一个参数」的四处表示收敛成一个声明**。任何带外部配置的系统都有这个问题：字段名（内部）、环境变量名（运维）、命令行参数名（CLI 用户）、帮助文本（文档）——四处如果各写一份，它们必然在某一版之后不一致，而不一致的症状是「文档里有的参数不生效」。判据是：**这个概念是否在 N 个地方各有一个标识符？** 是的话，让其中一个成为唯一来源。

**架构推论：** 「坏值该抛还是该回退」这个问题没有统一答案，而这一层的判据可以推广：**回退到默认值之后，系统行为是「少了个优化」还是「少了用户要的功能」？** 前者宽容，后者严格。`List`/`dict` 的坏值属于前者（少一个结构化参数），`bool | Literal` 的坏值属于后者（特性被关）。写错方向的后果很不对称：宽容错了会「静默失去功能」，严格错了会「启动失败」——后者吵但可修，前者安静且可能几周后才发现。

**架构推论：** `_UNSET` 那个单例说明「三态」需要一个能被打印的代表。任何「未设 / 设成空值 / 设成具体值」的区分都需要一个这样的哨兵，而它的 `__repr__` 应该在警告里读起来像一句话——因为运维看到的不是代码，是那句话。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/llm/binding_options.py` | `BindingOptions` | 自省派生的基类，四个消费面 |
| `lightrag/llm/binding_options.py` | `BindingOptions.args_env_name_type_value` | 唯一的派生点，dataclass 与旧路径两分支 |
| `lightrag/llm/binding_options.py` | `BindingOptions.add_args` | 五分支的 argparse 接线，错值策略不同 |
| `lightrag/llm/binding_options.py` | `BindingOptions.options_dict` | 按前缀剥取，等价于「本 binding 的全部参数」 |
| `lightrag/llm/binding_options.py` | `BindingOptions.options_dict_for_role` | 同 provider 继承 / 跨 provider 空基座 |
| `lightrag/llm/binding_options.py` | `BindingOptions.generate_dot_env_sample` | 全注释的 `.env` 样例，零副作用 |
| `lightrag/llm/binding_options.py` | `_resolve_optional_type` | 剥 Optional，但先短路容器类型 |
| `lightrag/llm/binding_options.py` | `_parse_role_json` | 角色 JSON 覆盖，坏值退到 `_UNSET` 并警告 |
| `lightrag/llm/binding_options.py` | `_bool_or_literal_choices` | 识别 `Union[bool, Literal[...]]`，须用 `typing.Union` |
| `lightrag/llm/binding_options.py` | `_make_bool_or_literal_parser` | 坏值抛而非降级为「关」 |
| `lightrag/llm/binding_options.py` | `_Unset` | 「未设」的可打印代言人 |
| `lightrag/llm/binding_options.py` | `_OllamaOptionsMixin` | 30+ 字段共用，适用范围写进帮助文本 |
| `lightrag/llm/binding_options.py` | `OllamaLLMOptions` | Ollama 语言模型绑定，`think` 是 bool-or-literal |
| `lightrag/llm/binding_options.py` | `OpenAILLMOptions` | Chat Completions 参数，`max_tokens` 标为废弃 |
| `lightrag/llm/binding_options.py` | `OpenAIResponsesLLMOptions` | 独立命名空间，防跨 API 参数泄漏 |
| `lightrag/llm/binding_options.py` | `_BOOL_TRUE_WORDS` | 与 `get_env_value` 共享，防三处漂移 |

## 参考

- [lightrag 源码：llm/binding_options.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/llm/binding_options.py)
- [Python 文档：dataclasses](https://docs.python.org/3/library/dataclasses.html)
- [Python 文档：argparse](https://docs.python.org/3/library/argparse.html)
- [PEP 604：联合类型的新语法](https://peps.python.org/pep-0604/)

## 与主核 / AI 的关系

主核里最接近的是**配置参数表（GUC）**：每个参数有名字、类型、默认值、取值范围、以及 `postgresql.conf` / `ALTER SYSTEM` / 环境变量多个入口。而 `pg_settings` 视图把「这个参数是什么」变成可以 `SELECT` 的数据——这正是 `args_env_name_type_value` 在做的事：**把参数的元数据本身变成数据**。

**差别在于「类型构造」的方式。** 主核的参数表是手写的静态表（因为 C 没有自省）；这里是 `dataclasses.fields` 自动派生。所以这里的「加一个参数」成本极低，而主核那边每加一个参数要改表、改解析、改文档三处。代价是这里更容易漏掉帮助文本（而 `help.get(..., "")` 恰好容忍了这个疏漏）。

与 i06（异常与角色）的连接很直接：`RoleLLMConfig.kwargs` 与 `provider_options` 的内容就是这些 `BindingOptions` 的 `asdict()` 结果，而 `options_dict_for_role` 产出的就是角色覆盖。所以那一页定义「角色是什么」，这一页定义「角色的参数怎么来」。

与 i01（配置存储）的关系是分层的：`BindingOptions` 处理「进程启动时从环境变量/命令行来的配置」，`config_store` 处理「运行时持久化的配置」。前者是引导期，后者是会话期。

对 AI 工程最实用的一条是**「环境变量宽容、命令行严格」这个不对称**。它看起来不一致，但两边面对的用户不同：环境变量由部署系统注入（拼错不该让服务起不来），命令行由人输入（拼错应该立刻知道）。若反过来（环境变量严格、命令行宽容），后果是「容器反复重启而日志里只有一行 argparse 错误」——最难查的一类部署问题。

另一条是 `_resolve_optional_type` 那个「容器类型先短路」。`Optional[List[str]]` 的 `get_args` 会返回 `(List[str], NoneType)`，剥掉 `NoneType` 得到 `List[str]` ——看起来对，但若先对 `List[str]` 本身做同样的剥，就会得到 `str`。这类「递归解包」在类型处理里非常常见，而漏掉容器短路会让一个数组参数静默变成标量参数。判据是：**这个类型的 `get_args` 返回的是「组成部分」还是「元素类型」？** 前者可以剥，后者不能。

最后一条是 `_UNSET` 那个「可打印的单例」。任何有「未设」状态的配置系统都需要它，而多数实现用 `None`，于是「显式设为 None」与「没设」不可区分——这正是 i05 里 `max_request_body_bytes_explicit` 要解决的问题的另一种形态。用一个带 `__repr__` 的哨兵单例同时解决「可区分」与「可读」。
