---
project: langchain
feature: structured-tool
title: 结构化工具与结果
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/tools/structured.py
    symbols: [StructuredTool, _filter_schema_args]
  - path: libs/core/langchain_core/tools/simple.py
    symbols: [Tool]
---

# 结构化工具与结果

## 一句话职责

`StructuredTool` 是「多参数工具」的实现，`Tool` 是「单字符串输入工具」的实现。二者的差别不只是参数个数，而在于**输入从哪来**：前者靠 args_schema 校验一个命名字典，后者靠一个隐式的 `{"tool_input": {"type": "string"}}` 约定。

## 机制是什么

这个模块要处理的是**同一个基类下的两种输入模型**，而它们的存在理由是历史与兼容：

| | `StructuredTool` | `Tool` |
|---|---|---|
| 输入 | 命名字典（按 args_schema 校验） | 单个字符串（或单个位置参数） |
| 参数推断 | 从函数签名生成 schema | 不生成，假定单输入 |
| 模型可见的参数名 | 函数参数名 | 固定的 `tool_input` |
| 主要来源 | `@tool` 装饰器 | 旧式 `Tool.from_function` |

`Tool` 的注释把它的定位说得很明确：「For backwards compatibility, if the function signature is ambiguous, assume it takes a single string input.」

所以这不是「简化版」，而是**为了不破坏既有的单输入工具而保留的一条路径**。理解了这一点，才能读懂 `StructuredTool` 里那些看起来过度设计的地方——它们全都在处理「多参数 + 注入 + 序列化」这一组真实需求。

## 怎么实现的

**`Tool` 的 `args` 属性里有一个硬编码的兜底。**

```python
@property
def args(self) -> dict[str, Any]:
    if self.args_schema is not None:
        return super().args
    # For backwards compatibility, if the function signature is ambiguous,
    # assume it takes a single string input.
    return {"tool_input": {"type": "string"}}
```

即**没有 schema 时，对外声明「我有一个叫 `tool_input` 的字符串参数」**。这个参数名不在任何函数签名里——它是凭空造的。所以模型会看到 `tool_input` 这个名字，而实际的函数接收的是位置参数。

这解释了 `Tool._to_args_and_kwargs` 里那段看起来奇怪的逻辑：

```python
args, kwargs = super()._to_args_and_kwargs(tool_input, tool_call_id)
# For backwards compatibility. The tool must be run with a single input
all_args = list(args) + list(kwargs.values())
if len(all_args) != 1:
    msg = f"""Too many arguments to single-input tool {self.name}.
            Consider using StructuredTool instead.""" f" Args: {all_args}"
    raise ToolException(msg)
return tuple(all_args), {}
```

三处：

1. **`list(args) + list(kwargs.values())`**：父类把输入拆成位置参数与关键字参数两部分，这里合并起来检查总数。单输入工具只允许恰好一个。
2. **返回 `tuple(all_args), {}`**——把那个唯一的值转成**位置参数**、清空关键字参数。因为原始函数可能是 `def f(x)`（位置）而不是 `def f(tool_input=...)`（关键字），所以必须用位置传。
3. **错误类型是 `ToolException` 而不是 `ValueError`**。这是一个重要的选择：`ToolException` 按 d01 讲的语义是「不终止 agent 的错误信号」。所以模型传多参数时，这次调用变成一个观察结果回给模型，而不是让整个循环崩掉。而错误文案给出「Consider using StructuredTool instead」——把修法写进错误。

**`StructuredTool` 的四个关键字段各自对应一处序列化难题。**

```python
class StructuredTool(BaseTool):
    description: str = ""
    args_schema: Annotated[ArgsSchema, SkipValidation(), _JsonSchemaFallback] = Field(...)
    func: Annotated[Callable[..., Any] | None, _JsonCallableFallback] = None
    coroutine: Annotated[Callable[..., Awaitable[Any]] | None, _JsonCallableFallback] = None
```

问题在于：**`args_schema` 是类、`func` 是函数，两者都没有 JSON 形式**。而工具需要被序列化（追踪、缓存、导出），默认的 pydantic 序列化器遇到不可序列化的对象会抛 `PydanticSerializationError`。

解法是两个 `PlainSerializer`：

```python
_JsonSchemaFallback = PlainSerializer(_serialize_args_schema, when_used="json-unless-none")
_JsonCallableFallback = PlainSerializer(_serialize_as_str, when_used="json-unless-none")
```

而**为什么用 `Annotated` 而不是 `@field_serializer`**，注释给了精确的理由：

> Attached to the fields via `Annotated` rather than declared with `@field_serializer`, which would take the field's one serializer slot and make any subclass that declares its own serializer for the same field fail at class creation with `PydanticUserError: Multiple field serializer functions ...`.

即 `@field_serializer` 是**每个字段独占一个槽位**的。若基类占了 `args_schema` 的槽，任何想给同一字段加自定义序列化的子类都会在**类创建时**就失败。用 `Annotated` 则把序列化器附在类型上，不占槽位——子类可以自由地再声明一个。这是一个「为可扩展性让路」的选择。

`when_used="json-unless-none"` 也有讲究：只在 JSON 序列化时用（`model_dump()` 保留原对象），且值为 `None` 时跳过。

**`_serialize_args_schema` 走的是「类 → JSON Schema」的降级链。**

```python
def _serialize_args_schema(args_schema: ArgsSchema) -> Any:
    if isinstance(args_schema, dict):
        return args_schema
    return _model_json_schema(args_schema, getattr(args_schema, "__pydantic_validator__", None))
```

即字典原样返回，类则取它的 JSON Schema。注释说明了为什么：pydantic 模型类没有 JSON 形式，但**它的 JSON Schema 恰好就是字典 schema 已经拥有的那个形状**——所以降级到 schema 是无损的（对「描述形状」这个目的而言）。

`_model_json_schema` 的实现有两层：

```python
@functools.lru_cache(maxsize=256)
def _model_json_schema(args_schema: TypeBaseModel, _rebuild_token: object) -> Any:
    try:
        return model_json_schema(args_schema)
    except Exception:  # a schema holding an arbitrary type has no JSON schema
        return str(args_schema)
```

四处值得读：

1. **`lru_cache(maxsize=256)` + 明确的性能理由**：注释给了具体数字——「1.10 ms against 0.02 ms for an eight-tool payload」，即**慢 55 倍**，且「tracing pays on every run」。所以这个缓存不是为了理论优雅，而是被实际的追踪开销逼出来的。
2. **`_rebuild_token` 是缓存键的一部分**，取值是 `__pydantic_validator__`。注释解释了为什么需要它：「`model_rebuild()` replaces」这个 validator；「Keying on it retires the entry for a rebuilt schema, and for one whose forward reference was still unresolved when it was first dumped」。
   即**同一个类在 `model_rebuild()` 之后可能产出不同的 schema**（前向引用解析了），所以缓存键必须包含「这是哪一代的 validator」。这是一处很精细的处理：单纯用类做键会返回过期结果。
3. **注释末尾还承认了它的局限**：「Mutating a class in place without rebuilding it is not detected」。即直接改类而不 rebuild 不会被发现——这是缓存不可覆盖的盲区。
4. **`except Exception` 兜底成 `str(args_schema)`**，注释说「a schema holding an arbitrary type has no JSON schema」。所以降级链是三级：JSON Schema → 类的字符串表示。这与 d01 里「解析失败就让模型可见」是同一种「失败在近处」的取向。

**`_serialize_as_str` 是一个「不用内建 `str`」的小函数，理由写在文档里。**

```python
def _serialize_as_str(value: Any) -> str:
    """Stringify a value that has no JSON form.

    A plain function rather than the `str` builtin: Pydantic < 2.9 inspects the
    serializer's signature, and `inspect.signature` raises for C builtins.
    """
    return str(value)
```

即 pydantic 会 `inspect.signature` 检查序列化器的签名，而 `inspect.signature(str)` 会抛异常（`str` 是 C 内建类型，没有 Python 签名）。所以要包一层。这是一个只有踩过才知道的坑，而注释把它记录了。

**`_run` / `_arun` 是注入链路的最终落点，而它们的判据都是「被调用方是否声明了这个参数」。**

```python
def _run(self, *args, config: RunnableConfig, run_manager=None, **kwargs):
    if self.func:
        if run_manager and _signature_parameters(self.func).get("callbacks"):
            kwargs["callbacks"] = run_manager.get_child()
        if config_param := _get_runnable_config_param(self.func):
            kwargs[config_param] = config
        return self.func(*args, **kwargs)
    msg = "StructuredTool does not support sync invocation."
    raise NotImplementedError(msg)
```

三处：

1. **`if run_manager and _signature_parameters(self.func).get("callbacks")`**：只有**函数签名里真的有 `callbacks` 参数**时，才把 run manager 的子管理器传进去。这一步是必需的——否则会给一个不接受 `callbacks` 的函数传多余的关键字参数，导致 `TypeError`。

   这条恰好揭示了 d02 那个「Reserved argument names」警告的另一面：`callbacks` 被从 **schema** 里剔除（模型看不见），但如果函数签名里有它，**执行时会真的填上**。所以「保留名」的语义是「模型看不见但框架会填」——而不是「完全不可用」。

2. **`if config_param := _get_runnable_config_param(self.func)`**：同理，只有当签名里有一个 `RunnableConfig` 类型的参数时才注入 config。注意**用的是参数名而不是固定名 `config`**——所以函数可以把那个参数叫任何名字，只要类型是 `RunnableConfig`。

3. **`func` 为 `None` 时抛 `NotImplementedError`** 而不是静默成功。因为一个只有 coroutine 的工具无法同步执行，静默返回 `None` 会让调用方以为工具跑过了。

`_arun` 是对称的，但**多了一个 fallback 分支**：

```python
if self.coroutine:
    ...  # 直接调用
# If self.coroutine is None, then this will delegate to the default
# implementation which is expected to delegate to _run on a separate thread.
return await super()._arun(*args, config=config, run_manager=run_manager, **kwargs)
```

即**异步入口在只有同步实现时会退到线程池**。注释直说了期望：「expected to delegate to `_run` on a separate thread」。所以「纯同步工具」也能被 `await` 调用，只是跑在 executor 里。

`ainvoke` 的重写就是为了配合这一点：

```python
async def ainvoke(self, input, config=None, **kwargs):
    if not self.coroutine:
        # If the tool does not implement async, fall back to default implementation
        return await run_in_executor(config, self.invoke, input, config, **kwargs)
    return await super().ainvoke(input, config, **kwargs)
```

即**没有 coroutine 时，整个 `invoke` 被丢进线程池**（不是只把 `_run` 丢进去）。这个差别有意义：`invoke` 包含完整的 Runnable 生命周期（回调、追踪），整个丢进线程池能保证这些同步回调也在工作线程里跑。而 `Tool.ainvoke` 有一模一样的实现——说明这是两个工具类的共同需求。

**`from_function` 是构造主体，而它的描述推导链有一段特别的脏数据处理。**

```python
description_ = description
if description is None and not parse_docstring:
    description_ = source_function.__doc__ or None
if description_ is None and args_schema:
    if isinstance(args_schema, type) and is_basemodel_subclass(args_schema):
        description_ = args_schema.__doc__
        if description_ and "A base class for creating Pydantic models" in description_:
            description_ = ""
        elif not description_:
            description_ = None
    elif isinstance(args_schema, dict):
        description_ = args_schema.get("description")
    else:
        raise TypeError("Invalid args_schema: expected BaseModel or dict, ...")
```

四处：

1. **`not parse_docstring` 才用 `__doc__`**。因为若开了 docstring 解析，docstring 的用途是「解析参数描述」而不是「当整体描述」——两者不该同时用它。
2. **从 `args_schema.__doc__` 取描述时，要检查它是不是 pydantic 基类的默认 docstring**：

   ```python
   if description_ and "A base class for creating Pydantic models" in description_:
       description_ = ""
   ```

   即**用户定义的模型若忘了写 docstring，会继承 `BaseModel` 的默认文档**，而那段文字（「A base class for creating Pydantic models」）对模型毫无意义。用字符串包含检查把它过滤成空。这是一个务实的脏数据处理——用内容匹配而不是类型判断，因为无法区分「继承来的」与「用户真这么写的」。

3. **`elif not description_: description_ = None`**：把空字符串归成 `None`，让它继续往下一级降级（这里已经是最后一级，所以结果是报错）。
4. **非法 `args_schema` 报 `TypeError`**，位置在这一段而非更早——因为它直到此刻才需要判定类型。

之后还有一条 dedent：

```python
if description is None:
    # Only apply if using the function's docstring
    description_ = textwrap.dedent(description_).strip()
```

**只在使用 docstring 时 dedent**。因为 docstring 通常因缩进而带前导空格（函数体缩进），而显式传入的 `description` 是用户写好的字符串，不该被改写。`dedent` 去掉公共前导空白，`strip` 去掉首尾。

**`infer_schema` 与 `args_schema` 的组合决定是否生成 schema。**

```python
if args_schema is None and infer_schema:
    args_schema = create_schema_from_function(
        name, source_function,
        parse_docstring=parse_docstring,
        error_on_invalid_docstring=error_on_invalid_docstring,
        filter_args=_filter_schema_args(source_function),
    )
```

注意 `filter_args` 的来源，它是这一页最简洁的一处机制：

```python
def _filter_schema_args(func: Callable[..., Any]) -> list[str]:
    filter_args = list(FILTERED_ARGS)
    if config_param := _get_runnable_config_param(func):
        filter_args.append(config_param)
    # filter_args.extend(_get_non_model_params(type_hints))
    return filter_args
```

即**剔除两类参数**：固定的 `FILTERED_ARGS`（`run_manager`、`callbacks`）与「签名里那个 `RunnableConfig` 类型的参数」（不管它叫什么名字）。

注意那行被注释掉的 `# filter_args.extend(_get_non_model_params(type_hints))`——这是一个**曾经尝试过但被放弃的做法**（把「不是模型字段的参数」也剔除）。保留注释而不删除，暗示这里有过一次权衡，但源码没写理由。

而 `create_schema_from_function` 自己还会**额外剔除注入参数**（d01 讲过 `include_injected`），所以最终的 schema 是「函数参数 - 保留名 - config 参数 - 注入参数」的差集。

**`_injected_args_keys` 在 `StructuredTool` 上被覆写，理由就在 d01 那段的注释里。**

```python
@functools.cached_property
def _injected_args_keys(self) -> frozenset[str]:
    fn = self.func or self.coroutine
    if fn is None:
        return _EMPTY_SET
    return _get_injected_args_keys_from_signature(fn)
```

即**从被包装的 `func` / `coroutine` 上看签名**，而不是从 `_run` / `_arun`。这正是 d01 里那句「`StructuredTool` overrides this to inspect its wrapped `func`/`coroutine` instead」的实现——因为 `StructuredTool` 的 `_run` 签名里只有 `*args, **kwargs`，真正的参数在被包装的函数上。

`self.func or self.coroutine` 的短路求值顺带处理了「只有异步实现」的工具。

## 关键取舍

**源码事实：** 无 `args_schema` 时 `Tool.args` 返回硬编码的 `{"tool_input": {"type": "string"}}`，而实际函数收到的是位置参数——参数名是凭空造的，只为给模型一个可填的位置。

**源码事实：** `Tool._to_args_and_kwargs` 合并位置参数与关键字参数后要求恰好一个，否则抛 `ToolException`（不终止 agent）并建议改用 `StructuredTool`；成功时把唯一值转成位置参数。

**源码事实：** `args_schema` / `func` / `coroutine` 用 `Annotated` + `PlainSerializer` 而非 `@field_serializer`，因为后者占住字段的序列化槽，会让子类在类创建时抛 `PydanticUserError`。

**源码事实：** `_model_json_schema` 用 `lru_cache(256)` 并以 `__pydantic_validator__` 作为缓存键的一部分，因为 `model_rebuild()` 会换掉 validator 从而可能产出不同 schema；注释承认「原地修改类而不 rebuild 不会被检测到」。

**源码事实：** schema 序列化是三级降级：字典原样 → JSON Schema → `str(类)`；`_serialize_as_str` 存在是因为 pydantic < 2.9 会对 `inspect.signature(str)` 抛错。

**源码事实：** `_run` / `_arun` 只在**被调用方签名里确实有该参数**时才注入 `callbacks` 与 config；config 参数按 `RunnableConfig` 类型识别（名字任意），`callbacks` 按名字识别。

**源码事实：** `_arun` 在没有 coroutine 时委托父类走线程池；`ainvoke` 在没有 coroutine 时把整个 `invoke` 丢进 `run_in_executor`（而非只丢 `_run`）。

**源码事实：** `_filter_schema_args` 剔除固定保留名加上「签名里 `RunnableConfig` 类型的参数」，并留着一行被注释掉的「剔除所有非模型参数」的旧尝试。

**源码事实：** `from_function` 的描述推导在从 `args_schema.__doc__` 取值时，用**字符串包含** `"A base class for creating Pydantic models"` 来识别并清空 pydantic 基类的默认文档；`dedent` 只在使用 docstring 时执行。

**架构推论：** `Tool` 与 `StructuredTool` 的差别本质上是**「参数契约来自哪里」**。前者用一条硬编码约定（一个叫 `tool_input` 的字符串），后者用函数签名生成的 schema。前者的好处是零成本（不生成 schema、不校验），坏处是模型只能填一个字符串；后者正好相反。保留两条路径而不是统一，是「不破坏既有调用方式」的代价——而代价具体表现为 `Tool` 里那些「为兼容而存在」的分支。

**架构推论：** `Annotated` 而非 `@field_serializer` 这个选择，揭示了一个常被忽略的约束：**pydantic 的字段级钩子是独占的**。当框架要给某个字段加行为时，用「占槽位」的方式会剥夺子类的扩展能力。改用类型注解（`Annotated`）则把行为挂在类型上，天然可叠加。这是一条可迁移的设计原则：**框架加的东西不该占用用户的扩展点**。

**架构推论：** `_model_json_schema` 的缓存键设计（类 + validator）是一个「缓存失效要绑在真正会变的东西上」的范例。用类做键是直觉做法但会错——因为同一个类可以因 `model_rebuild()` 而产出不同 schema。绑在 validator 上则精确对应「类被重建过」这个事件。而注释主动承认盲区（原地改类检测不到），这比声称完美更可信。

**架构推论：** 注入判据用「签名里有没有这个参数」而不是「工具是不是某个类型」，使 `callbacks` 与 config 的注入成为**可选能力**而不是强制行为。这个方向是对的——若强制注入，所有工具的签名都必须接受这两个参数。代价是注入行为依赖签名检查（每次调用都做一次 `_signature_parameters`），不过这是廉价的反射操作。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/tools/structured.py` | `StructuredTool` | 多参数工具：schema 校验 + 注入 + 序列化回退 |
| `libs/core/langchain_core/tools/structured.py` | `StructuredTool._run` / `_arun` | 按签名条件注入 `callbacks` 与 config，异步缺失时退线程池 |
| `libs/core/langchain_core/tools/structured.py` | `StructuredTool.from_function` | 构造主体：schema 生成、描述三级推导、dedent |
| `libs/core/langchain_core/tools/structured.py` | `StructuredTool._injected_args_keys` | 从被包装的 `func`/`coroutine` 推导注入参数 |
| `libs/core/langchain_core/tools/structured.py` | `_filter_schema_args` | 剔除保留名与 config 参数后再生成 schema |
| `libs/core/langchain_core/tools/structured.py` | `_serialize_args_schema` / `_model_json_schema` | 类 → JSON Schema 的降级与带 validator 键的缓存 |
| `libs/core/langchain_core/tools/structured.py` | `_serialize_as_str` | 规避 pydantic 对 `inspect.signature(str)` 抛错的包装 |
| `libs/core/langchain_core/tools/simple.py` | `Tool` | 单字符串输入工具，为兼容保留 |
| `libs/core/langchain_core/tools/simple.py` | `Tool.args` | 无 schema 时硬编码 `tool_input` 字符串参数 |
| `libs/core/langchain_core/tools/simple.py` | `Tool._to_args_and_kwargs` | 要求恰好一个输入，否则抛 `ToolException` 并建议改 `StructuredTool` |

## 参考

- [langchain 源码：tools/structured.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/tools/structured.py)
- [langchain 源码：tools/simple.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/tools/simple.py)
- [langchain 文档：工具](https://docs.langchain.com/oss/python/langchain/tools)
- [Pydantic 文档：PlainSerializer 与 Annotated](https://docs.pydantic.dev/latest/api/functional_serializers/#pydantic.functional_serializers.PlainSerializer)

## 与主核 / AI 的关系

「两条输入路径并存而不统一」与主核里**旧协议与新协议共存**是同一种维护现实。主核保留旧的客户端协议版本（因为存在大量已部署的客户端），同时推进新版本。代价是一段时间内两套代码路径并行，且新功能必须在两处都实现（或明确只在新版实现）。本页的 `Tool` 与 `StructuredTool` 就是这种状态，且 `Tool` 的错误文案直接建议用户迁到 `StructuredTool`——这就是「保留但不推荐」的标准做法。

「缓存键要绑在真正会变的东西上」与主核里**计划缓存的失效键**完全一致。主核缓存执行计划时会绑定「相关表的 DDL 版本」而不是表名——因为同名表可以被 `ALTER` 改掉。本页把缓存键绑在 `__pydantic_validator__` 而非类本身，是同一条原则在 Python 对象层面的应用。

「框架加的行为不该占用用户的扩展点」这条在主核里对应的是**扩展点与内置行为的分离**：主核的钩子（如 `ExecutorEnd`）允许多个实现共存，而不是「一个槽位只能插一个」。判据相同——**可叠加性比简洁性更重要**，因为不可叠加的行为最终会逼用户绕过框架。

对 AI 工程的实际意义在于：工具的参数契约是模型与外部世界的接口，而「参数从哪来」决定了这个接口有多灵活。这份实现给出的三条经验可直接用：**给「只有一个输入」的场景留一条廉价路径**（不必为它生成 schema）、**框架给字段加行为时用类型注解而非占位钩子**、以及**缓存失效要绑在有版本语义的字段上**（并且把无法覆盖的盲区写进注释）。
