---
project: vllm
feature: tool-parsers
title: 工具调用解析
page_type: feature
schema_version: 1
refs:
  - path: vllm/tool_parsers/__init__.py
    symbols: [ToolParser, ToolParserManager, register_lazy_tool_parsers]
  - path: vllm/tool_parsers/abstract_tool_parser.py
    symbols: [ToolParser, ToolParserManager]
---

# 工具调用解析

## 一句话职责

把模型吐出来的**自由文本**里藏着的工具调用抽成结构化对象——静态响应抽一次，流式响应边收边抽；而 `vllm/tool_parsers/__init__.py` 是一张 **61 条**的「模型名 → (文件, 类名)」懒加载表。

## 机制是什么

工具调用这件事有一个绕不开的矛盾：**模型输出的是文本，客户端要的是 JSON**。而每个模型家族的输出格式都不同——有的用 `<tool_call>` 标签包 JSON，有的用 XML，有的用 Python 函数调用语法，有的用自研的特殊 token。所以「解析」就必然是**一族一实现**。

于是这里出现了三件东西：

1. **一个接口**：`ToolParser`，两个核心方法 + 一个请求改写钩子。
2. **一张注册表**：`_TOOL_PARSERS_TO_REGISTER`，61 条映射，但**一条都不 import**。
3. **一个懒加载管理器**：`ToolParserManager`，维护「名字 → 模块路径 + 类名」的映射，第一次查时才 import。

**懒加载是这张表存在的技术前提。** 若 61 条都在 import 时就加载，那所有工具解析器的依赖（各种正则、语法解析库、tokenizer 交互）都会在任何一次启动里被拉起来——哪怕这次请求根本不用工具。而现在：`_TOOL_PARSERS_TO_REGISTER` 只是一个字典字面量，模块底部的 `register_lazy_tool_parsers()` 只把 61 个三元组塞进 `lazy_parsers`，**零 import**。

**这里是 vllm 与同类项目的分野**：`ToolParserManager` 是「字符串 → 类」的注册器（名字空间可枚举、可插件注入），而不是一个模块级的常量枚举。前者能容纳外部插件（`import_tool_parser`），后者只能容纳内置实现。

## 怎么实现的

**注册表本身值得先看清楚形状。** 每一条是 `"名字": ("文件名", "类名")`，而模块路径由 `register_lazy_tool_parsers` 拼成 `f"vllm.tool_parsers.{file_name}"`。

```python
def register_lazy_tool_parsers():
    for name, (file_name, class_name) in _TOOL_PARSERS_TO_REGISTER.items():
        module_path = f"vllm.tool_parsers.{file_name}"
        ToolParserManager.register_lazy_module(name, module_path, class_name)

register_lazy_tool_parsers()
```

**模块顶部同时 import 了 `ToolParser` 与 `ToolParserManager` 并写进 `__all__`**——所以 `from vllm.tool_parsers import ToolParserManager` 可用。这是一种「门面再导出」：真正定义在 `abstract_tool_parser.py`，但对外只有一个包级入口。

**表里有几类条目需要单独认出来，因为它们各自代表一种现实。**

**一对多（同一文件、不同类名）几乎没有，但一处多对一很多。** 看这几组：

- `glm45` 与 `glm47` → 同一个文件 `glm47_moe_tool_parser`、同一个类 `Glm47MoeModelToolParser`；
- `muse` 与 `musespark` → 同一个类，注释写 `# Alias of "muse".`；
- `zaya` 与 `zaya_xml` → 同一个类，注释写 `# Alias of "zaya", the name Zyphra's model cards use.`；
- `llama3_json` 与 `llama4_json` → 同一个类；
- `qwen3_coder` 与 `qwen3_xml` → 同一个类；
- `cohere_command3` 与 `cohere_command4` → 同一个文件，两个类。

**别名不是冗余，是兼容性负担。** `musespark` 与 `zaya_xml` 的注释都点明「另一个名字来自模型卡」，也就是说**用户会按模型卡上的名字来填这个参数**，而模型卡与实现者的命名习惯并不一致。保留两个键让两种写法都能用，代价是这张表不再是「一个模型一个条目」的清晰映射。

**有一条带 TODO 的条目，是全表信息量最高的地方。**

```python
# TODO: Qwen2.5/QwQ/Qwen3 JSON tool calls. Aliased to `hermes` for now;
# a dedicated parser should treat the template's newline before and
# between `<tool_call>` blocks as framing, as the Rust frontend does.
"qwen3_json": ("hermes_tool_parser", "Hermes2ProToolParser"),
```

**注释里出现了两个关键信息**：一是「暂时借用 hermes 的实现」（因为 Qwen 的 JSON 工具调用与 Hermes 格式接近）；二是「Rust 前端已经正确处理了 `<tool_call>` 块前后的换行，Python 侧应该对齐」。这条注释把**跨语言实现的一致性债务**明确记在代码里，而不是留在 issue 里。

**文件名的命名并不统一，注释也承认了。**

```python
"deepseek_v32": ("deepseekv32_engine_tool_parser", "DeepSeekV32EngineToolParser"),
"cohere_command3": ("cohere_command_tool_parser", "CohereCommand3ToolParser"),
```

有的文件名带 `_engine_` 后缀有的不带，有的用下划线分隔版本号有的不用。`vllm/tool_parsers/AGENTS.md` 里有一句直接承认：「the `_engine_` suffix is not applied consistently」。**这类不一致只能靠这张表作为唯一权威**——所以表本身就是文档。

还有条目把 `-` 与 `_` 混用：`granite-20b-fc` 用连字符，`granite_toolcall_parser` 用下划线。**因为键是用户命令行输入，两种写法都得接受**，所以不能统一。

**`ToolParserManager` 维护两张字典，而不是一张。**

```python
tool_parsers: dict[str, type[ToolParser]] = {}
lazy_parsers: dict[str, tuple[str, str]] = {}  # name -> (module_path, class_name)
```

**区分「已加载」与「待加载」是必要的**：`get_tool_parser` 先查前者，命中直接返回；再查后者，命中则加载并**把结果塞回前者缓存**；两者都没有才抛 `KeyError`。

```python
if name in cls.tool_parsers:
    return cls.tool_parsers[name]
if name in cls.lazy_parsers:
    return cls._load_lazy_parser(name)
raise KeyError(f"Tool parser '{name}' not found.")
```

`_load_lazy_parser` 除了 import 还做一次**类型校验**：`issubclass(parser_cls, ToolParser)` 不成立就抛 `TypeError`。也就是说「名字指向的类必须是这个基类的子类」是运行期强制的——因为懒加载绕过了任何静态检查。

失败时先 `logger.exception` 再 `raise`——**记日志是为了让人看到完整 traceback，重抛是为了让调用方拿到原始异常类型**。两者不能只取一个。

**`register_module` 同时支持「立即注册」与「装饰器注册」，而且装饰器那条路是「注册成懒的」。**

```python
if module is not None:
    cls._register_module(module=module, module_name=name, force=force)
    return module

def _decorator(obj):
    module_path = obj.__module__
    class_name = obj.__name__
    ...
    for n in names:
        cls.lazy_parsers[n] = (module_path, class_name)   # Lazy mapping only: do not import now
    return obj
```

注释写明「Lazy mapping only: do not import now」。**装饰器路径复用对象已有的 `__module__` 与 `__name__`**，所以不需要调用方再写一遍模块路径与类名——这比 `register_lazy_module` 省事，但要求被装饰的类已经被 import（否则装饰器根本不会执行）。**这是懒加载的一个内在限制：被装饰器注册的模块必须已经加载过。**

`_register_module` 支持 `module_name` 传 `list[str]`——**一个类可以挂多个名字**，这就是别名机制在即时注册路径上的对应实现。`force=False` 时若名字已被占用则抛 `KeyError` 并指出占用者的 `__module__`，这条错误消息对调试冲突很有用。

**`ToolParser` 基类上有四个类属性，它们的语义构成了整个解析层的「能力声明」。**

| 属性 | 默认 | 含义 |
|---|---|---|
| `supports_required_and_named` | `True` | 是否能用标准 JSON 引导处理 `tool_choice="required"` |
| `structural_tag_model` | `None` | 对应的 xgrammar 内置结构化标签模型键 |
| `default_tool_strict_level` | `AUTO` | 运维未指定时的严格级别 |
| `engine_based_streaming` | `False` | 是否走新版引擎流式解析 |

`supports_required_and_named` 的注释给了一段清晰的判据：

> Subclasses set False when the standard parsing does not work for their model's output format (e.g. GLM models that use XML). When False, the serving layer falls back to the tool_parser's extract_tool_calls / extract_tool_calls_streaming methods for required/named tool_choice, treating them the same as "auto".

**这段注释解释的是「强制工具调用」这条路径的两种实现**：一种靠「用 JSON schema 引导解码」（适用于输出本就是 JSON 的模型），一种靠「照常解码再用本解析器抽」（适用于输出是 XML 等格式的模型）。选错会导致「模型被强制输出 JSON，而解析器只认 XML」——于是抽不到调用，整段输出被当成普通内容返回。

**`__init_subclass__` 把这个开关自动化了一半。**

```python
def __init_subclass__(cls, **kwargs):
    super().__init_subclass__(**kwargs)
    if cls.structural_tag_model is not None and envs.VLLM_ENFORCE_STRICT_TOOL_CALLING:
        cls.supports_required_and_named = False
```

**配了 `structural_tag_model` 且开了严格模式，就自动关掉通用 JSON 处理。** 因为此时约束由结构化标签接管，再套一层 JSON schema 会冲突。这是「类属性在子类定义时被环境变量改写」——**同一个类在不同环境变量下行为不同**，调试时要记得这一点。

`get_structural_tag` 则做了三重前置判断：`structural_tag_model is None` → 返回 `None`；`VLLM_ENFORCE_STRICT_TOOL_CALLING` 为假 → 返回 `None`；否则才去 `structural_tag_registry` 查表。所以「声明了但没启用」是一个合法的空操作。

**`adjust_request` 是唯一改写请求的方法，它有三道早退。**

1. 请求没有 `tools` → 原样返回。
2. 请求已经带了 `structural_tag` → 原样返回（说明统一解析器已经处理过，别再覆盖）。
3. `supports_required_and_named` 为假 → 原样返回，注释解释了原因：「they would never find a call in the JSON output and would return it as content instead」。

只有三道都没拦住，才用 `get_json_schema_from_tools(tool_choice, tools, parallel_tool_calls)` 造 schema，然后按请求类型分别写：

**`ChatCompletionRequest` 的写法很直接**——把 `structured_outputs` 整个替换成 `StructuredOutputsParams(json=schema)`，并把 `response_format` 置 `None`。注释写「will override structured output json settings to make tool calling work correctly」——**强制工具调用优先于用户指定的响应格式**。

**`ResponsesRequest` 的写法旁边有一整段注释解释两个坑。**

```python
request.text = ResponseTextConfig(
    format=ResponseFormatTextJSONSchemaConfig(type="json_schema", name="tool_calling_response", schema=..., strict=True)
)
```

注释说：「Single-shot construction so Pydantic v2 tracks `format` in `__fields_set__` — assigning to `.format` after the bare `ResponseTextConfig()` constructor does not, which can drop the nested config from `model_dump`. Also drop the `description` kwarg: it is not a field on `ResponseFormatTextJSONSchemaConfig` and was being silently passed through as extra.」

**两个都是真实的坑，而且都很难自己发现**：一是 pydantic v2 的 `__fields_set__` 只记录构造时传入的字段，事后赋值会让嵌套配置在 `model_dump` 时被丢掉（于是序列化出去给前端的请求里没有 schema）；二是多传了一个不存在的字段，pydantic 默认忽略而不是报错，于是它静默消失在 extra 里。**注释在这里的密度是必要的——这类行为只有踩过才知道。**

**`extract_tool_calls` 与 `extract_tool_calls_streaming` 是一对刻意不对称的接口。**

- 前者是「静态方法」语义：docstring 写「Static because it's stateless」——完整响应一次性给出，不需要记住任何东西。
- 后者必须是实例方法：docstring 写「Has to be an instance method because it requires state - the current tokens/diffs, but also the information about what has previously been parsed and extracted (see constructor)」。

**构造器里那四个状态字段就是流式解析的全部记忆**：

```python
self.prev_tool_call_arr: list[dict] = []
self.current_tool_id: int = -1
self.current_tool_name_sent: bool = False
self.streamed_args_for_tool: list[str] = []
```

`current_tool_id` 初值是 `-1` 而不是 0——**表示「还没开始任何工具调用」**，与「第 0 个工具调用进行中」区分开。

`get_remaining_unstreamed_args` 是一段精巧的差量计算：取最后一个已解析调用的 `arguments`（可能是 dict 也可能是字符串，前者要 `json.dumps`），与已流出的字符串比较：

```python
if expected.startswith(actual):
    return expected[len(actual):]
return ""
```

**它假设「已流出的内容一定是目标内容的前缀」**——这个假设在参数按顺序增量输出时成立。若 `actual` 不是前缀（例如模型输出了回退），它返回空串而不是报错。**这是「差量推送」的标准容错：前缀不成立时宁可这一拍不推，也不重复推。**

`vocab` 是 `cached_property`，注释写「Only TokenizersBackend is guaranteed to have `.vocab` whereas all tokenizers have `.get_vocab()`」——所以用 `get_vocab()` 保证跨实现可用，用 `cached_property` 避免重复取整份词表。

**`ToolParser.__init__` 对 tools 参数做了一次过滤。** 它只留 `ChatCompletionToolsParam` 或 `FunctionTool` 实例，其余静默丢弃。**静默是这里的问题**——传错类型不会报错，只是工具列表变短，而模型看到的可用工具也就少了。

## 关键取舍

**源码事实：** `_TOOL_PARSERS_TO_REGISTER` 有 61 条映射，格式是 `名字: (文件名, 类名)`；模块底部的 `register_lazy_tool_parsers()` 只写入懒映射，**不 import 任何解析器模块**。

**源码事实：** 表里有多组别名（`musespark`/`muse`、`zaya_xml`/`zaya`、`llama4_json`/`llama3_json`、`qwen3_xml`/`qwen3_coder`），注释明说别名来自模型卡的命名。

**源码事实：** `qwen3_json` 暂时借用 `Hermes2ProToolParser`，附 TODO 说明 Rust 前端已正确处理 `<tool_call>` 块前后的换行，Python 侧应对齐。

**源码事实：** 表里的键混用连字符与下划线（`granite-20b-fc` vs `granite_toolcall_parser`），文件名 `_engine_` 后缀也不统一。

**源码事实：** `ToolParserManager` 维护 `tool_parsers`（已加载）与 `lazy_parsers`（待加载）两张字典，加载成功后回填前者缓存；加载时校验必须是 `ToolParser` 子类。

**源码事实：** 装饰器式注册只写懒映射（注释「do not import now」），因此被装饰的类必须已被加载。

**源码事实：** `__init_subclass__` 在「配了 `structural_tag_model` 且 `VLLM_ENFORCE_STRICT_TOOL_CALLING` 为真」时把 `supports_required_and_named` 置 `False`。

**源码事实：** `adjust_request` 三道早退（无 tools / 已有 structural_tag / 不支持 required_and_named）后才改写；`ChatCompletionRequest` 走替换 `structured_outputs` + 清空 `response_format`，`ResponsesRequest` 走一次性构造 `ResponseTextConfig`。

**源码事实：** 构造器四个状态字段（`prev_tool_call_arr`、`current_tool_id`、`current_tool_name_sent`、`streamed_args_for_tool`）支撑流式解析；`current_tool_id` 初值 `-1`。

**源码事实：** `get_remaining_unstreamed_args` 依赖「已流出内容是目标内容的前缀」这个假设，不成立时返回空串。

**架构推论：** 「61 条映射但零 import」是懒加载最有说服力的场景。**映射表是纯数据，加载是副作用**，把两者分开后，启动成本从「61 个模块的 import」降到「一个字典字面量」。代价是错误被推迟：某个解析器模块写错，要到第一次有人用那个名字时才会发现——**这正是懒加载的通用交易**。

**架构推论：** 别名条目是「用户输入空间」与「实现空间」不一致的产物。理想情况下一一对应，但模型卡的名字不受实现者控制。**处理这类不一致的唯一正确做法是在唯一入口做映射，而不是在每个消费点做兼容判断**——这张表就是那个唯一入口。

**架构推论：** 用类属性（`supports_required_and_named` 等）声明能力，再由 `__init_subclass__` 依据环境变量自动调整，把「每个子类都要记得改」变成了「机制替你改」。但它同时让**类的行为依赖于环境变量**，而这在静态阅读时完全不可见。**这类「声明式 + 自动派生」的设计必须配一条日志或在文档里显式写明**，否则排障时无从下手。

**架构推论：** `adjust_request` 的三道早退顺序是有意的：先看「有没有工具」（无工具则什么都不做）、再看「是否已被统一层处理过」（避免覆盖）、最后看「本解析器能否吃标准 JSON」（不能则别乱改）。**顺序错了会覆盖上游的正确决定**——把第三道放前面，就会让已带 structural_tag 的请求被 JSON 引导覆盖掉。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/tool_parsers/__init__.py` | `_TOOL_PARSERS_TO_REGISTER` | 61 条「名字 → (文件, 类名)」的纯数据映射 |
| `vllm/tool_parsers/__init__.py` | `register_lazy_tool_parsers` | 把映射表灌进管理器的懒映射（不 import） |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParser` | 解析器接口与四个能力声明类属性 |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParser.__init_subclass__` | 按环境变量自动关闭通用 required/named 处理 |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParser.adjust_request` | 三道早退后为强制工具调用注入 JSON schema |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParser.get_structural_tag` | 结构化标签的三重前置判断 |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParser.extract_tool_calls` | 静态解析（完整响应） |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParser.extract_tool_calls_streaming` | 流式解析（依赖实例状态） |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParser.get_remaining_unstreamed_args` | 按前缀差量算出未推送的参数尾部 |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParserManager` | 双字典注册表（已加载 / 待加载） |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParserManager.get_tool_parser` | 先查已加载、再懒加载、否则 `KeyError` |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParserManager._load_lazy_parser` | import + 子类校验 + 回填缓存 + 失败记日志重抛 |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParserManager.register_module` | 即时注册或装饰器注册（后者只写懒映射） |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParserManager.list_registered` | 合并两张字典的名字并排序 |
| `vllm/tool_parsers/abstract_tool_parser.py` | `ToolParserManager.import_tool_parser` | 外部插件的导入入口 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/tool_parsers/__init__.py](https://github.com/vllm-project/vllm/blob/main/vllm/tool_parsers/__init__.py)
- [vllm 源码：vllm/tool_parsers/abstract_tool_parser.py](https://github.com/vllm-project/vllm/blob/main/vllm/tool_parsers/abstract_tool_parser.py)
- [vllm 文档：工具调用与解析器选择](https://docs.vllm.ai/en/latest/features/tool_calling.html)

## 与主核 / AI 的关系

「名字 → 实现」的懒加载表与主核里「函数名 → 实现」的目录（`pg_proc`）是同一结构，但两边的加载时机相反：主核在建库时就把函数注册进系统表（持久化、可查询），vllm 在第一次用到时才 import。**持久化注册可审计、可跨版本对比；懒加载零启动成本、但对错误的暴露最晚。**

**别名机制 vs 主核的类型别名。** 数据库允许同一类型有多个名字（`int4` / `integer` / `int`），全部指向同一个 OID；vllm 让 `musespark` 与 `muse` 指向同一个类。**两者的动机相同：外部世界的命名不可控，必须在入口收敛。** 区别在数据库能查询「这个别名指向谁」，而 vllm 只能读代码或调 `list_registered` 看名字，看不到指向。

**`adjust_request` 的三道早退 vs 主核的规则重写系统。** 数据库在解析查询前会做规则重写（视图展开、行级安全），也会判断「这条规则是否已经有更早的规则处理过」以避免重复展开。**「已被上游处理过就跳过」是重写系统的基本纪律**——否则同一层被叠加两次，语义就变了。

**流式解析的状态机 vs 主核的游标。** 主核的游标记住「读到哪了」，流式解析记住「已推到哪了、当前第几个调用」。**两者都是「用最少的持久状态支撑增量处理」**，而 `get_remaining_unstreamed_args` 那个「前缀假设」就是游标位置与目标内容的一致性检查——不成立时游标已经偏了，此时返回空是最保守的处理。

对 agent 工程最直接的价值有四条。其一：**多格式解析器要用懒注册表而不是启动时全量 import**，否则一个用不到的分支会拖慢所有启动。其二：**别名必须在唯一入口做映射**，让每个消费点自己兼容是最容易失控的做法。其三：**「强制工具调用」有两条实现路径**（schema 引导 / 原生格式抽取），选错会让解析器完全抽不到调用——用一个类属性显式声明自己属于哪一类。其四：**pydantic v2 里嵌套配置必须一次性构造**，事后赋值会让字段不在 `__fields_set__` 里、序列化时被静默丢掉。
