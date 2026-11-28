---
project: langchain
feature: tool-contract
title: 工具契约与参数注入
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/tools/base.py
    symbols: [BaseTool, BaseToolkit, ToolException]
  - path: libs/core/langchain_core/tools/base.py
    symbols: [InjectedToolArg, InjectedToolCallId, SchemaAnnotationError]
---

# 工具契约与参数注入

## 一句话职责

`BaseTool` 定义「一个工具对模型呈现什么、被调用时收到什么、出错时怎么表现」。其中最特殊的一处机制是**参数注入**：带 `InjectedToolArg` 注解的参数对模型不可见、由框架在调用时填入——这让工具能拿到 `tool_call_id` 这类**模型不可能提供**的信息。

## 机制是什么

工具在 agent 系统里有两种身份，而它们的信息需求是矛盾的：

| 身份 | 需要什么 |
|---|---|
| 给模型看的「能力声明」 | 参数 schema、描述、名字——都必须是模型能理解和填写的 |
| 被框架执行的「函数」 | 上下文：回调管理器、配置、本次调用的 id、运行时状态 |

矛盾在于后者**模型填不了**。`tool_call_id` 是模型提出调用时由框架分配的；`run_manager` 是回调系统的对象；`ToolRuntime` 装着会话状态。这些参数必须能被函数签名接收到，却绝不能出现在 schema 里——否则模型会尝试编造它们。

所以这套机制的核心是**一个双向的标记**：

```
参数带 InjectedToolArg 注解  →  从 schema 里剔除（模型看不到）
                             →  从工具输入里过滤（校验时不以模型提供为准）
                             →  执行时由框架填入（真的给到函数）
```

三条都要做对，缺一条就出问题：只从 schema 剔除而不过滤输入，模型仍能伪造；只过滤而不注入，函数拿到 `None`；只注入而不剔除，模型会编。

## 怎么实现的

**`InjectedToolArg` 是一个空的标记类，而它的新式用法是「注解元数据」。**

```python
class InjectedToolArg:
    """Annotation for tool arguments that are injected at runtime.

    Tool arguments annotated with this class are not included in the tool
    schema sent to language models and are instead injected during execution.
    """
```

它**没有父类、没有方法、没有字段**——纯粹是类型层面的标记。之所以做成类而不是哨兵值或字符串，是为了能作为 `Annotated[...]` 的元数据参与类型标注：

```python
def foo(x: int, tool_call_id: Annotated[str, InjectedToolCallId]) -> ToolMessage
```

即「类型是 `str`，并带一个 `InjectedToolCallId` 标记」。用 `Annotated` 的好处是**保留原有的类型信息**——`str` 仍是 `str`，校验、序列化都照常；标记只是附加元数据。

**`InjectedToolCallId` 是它的子类，而子类这个设计支撑了「按具体类型判定」。**

```python
class InjectedToolCallId(InjectedToolArg):
    """Annotation for injecting the tool call ID."""
```

继承关系让判定函数可以两种粒度工作：

```python
def _is_injected_arg_type(type_: type | TypeVar, injected_type: type[InjectedToolArg] | None = None) -> bool:
    if injected_type is None:
        if _is_directly_injected_arg_type(type_):
            return True
        injected_type = InjectedToolArg

    return any(
        isinstance(arg, injected_type)
        or (isinstance(arg, type) and issubclass(arg, injected_type))
        for arg in get_args(type_)[1:]
    )
```

四处：

1. **`get_args(type_)[1:]`**：`Annotated[T, meta1, meta2]` 的 `get_args` 返回 `(T, meta1, meta2)`，所以 `[1:]` 取全部元数据。即**一个参数可以带多个标记**，只要有一个匹配就算。
2. **`isinstance(arg, injected_type) or (isinstance(arg, type) and issubclass(arg, injected_type))`**：同时接受「标记的实例」与「标记的类」。为什么两者都要？因为 `Annotated[str, InjectedToolCallId]` 里放的是**类本身**，而某些用法可能放实例（例如带参数的标记）。两种都判是兼容性考虑。
3. **两个粒度**：不传 `injected_type` 时判「是否任何注入类型」（包括 `_DirectlyInjectedToolArg` 那条直接注解路径）；传了 `InjectedToolCallId` 时只认这一个。后者用于「必须注入 call id」的强校验点。
4. **`_DirectlyInjectedToolArg` 那条分支** 让 `ToolRuntime` 可以**直接写类型注解而不必包 `Annotated`**：

   ```python
   @tool
   def foo(x: int, runtime: ToolRuntime) -> str:
   ```

   文档直接对比了两种写法并说 `annotated metadata` 是 `verbose alternative`。这是一个「让常见用法更短」的取舍——代价是多了一条判定路径。

**判定要处理 Python 注解的三种形态，而其中一种会静默失败。**

```python
def _get_injected_args_keys_from_signature(func: Callable[..., Any]) -> frozenset[str]:
    params = _signature_parameters(func)
    hint_source = _get_type_hints_source(func)
    hints = _get_type_hints(hint_source, include_extras=True) if hint_source is not None else None) or {}
    ...
    for name, param in params.items():
        annotation = hints.get(name, param.annotation)
        if isinstance(annotation, str):
            ...  # 尝试单独解析
        if _is_injected_arg_type(annotation):
            keys.add(name)
```

关键点：

- **先试 `get_type_hints(..., include_extras=True)` 批量解析**。`include_extras=True` 是必需的——不加的话 `Annotated` 的元数据会被剥掉，所有注入参数都认不出来。这是一个很容易踩的坑：`get_type_hints` 默认 `include_extras=False`。
- **批量失败则逐个重试**。文档说得精确：「`get_type_hints` resolves every annotation at once and raises on the first failure, so any parameter it leaves uncovered is retried on its own: an unrelated unresolvable forward reference must not disable injection for parameters whose annotations do resolve」。即**一个无法解析的前向引用不该拖累其他参数**。
- **每个不同的字符串注解在一次调用内最多解析一次，失败也记下来**（`resolved_annotations` 缓存）。避免同一份失败被反复重试。
- **解析不出来的后果是「当作未注入」**，且打 debug 日志。文档把这条明确写出：「Such a parameter keeps its raw string annotation, which `_is_injected_arg_type` never classifies as injected, so it stays visible to the model rather than being injected at call time」。

  这是一个**方向明确的失败选择**：认不出来就让模型看见它。理由是安全——若错判成注入，模型看不到它、框架也没注入（因为解析失败），函数会拿到 `None` 或抛错；若当作普通参数，模型至少能看到并尝试填一个值。

`_resolve_forward_ref` 的实现有一处技巧：

```python
def _annotation_holder() -> None:
    pass
_annotation_holder.__annotations__ = {"value": annotation}
return get_type_hints(_annotation_holder, globalns=globalns, include_extras=True)["value"]
```

即**造一个临时函数来复用 `get_type_hints` 的单注解解析能力**。`get_type_hints` 没有「解析单个字符串」的公开 API，所以用一个空函数挂上注解再调它。`except Exception` 兜住所有失败返回 `None`——这里捕获宽泛是合理的，因为解析失败的类型五花八门。

**注入参数在三个地方被处理，缺一处就出漏洞。**

第一处：**schema 生成时剔除**（`create_schema_from_function` 里）：

```python
if not include_injected:
    for existing_param in sig.parameters:
        if (_is_injected_arg_type(sig.parameters[existing_param].annotation)
            and existing_param not in filter_args_):
            filter_args_.append(existing_param)
```

注释说明「Exclude injected args from the schema regardless of whether `filter_args` was provided」。所以即使调用方显式传了 `filter_args`，注入参数也会被**额外**剔除。

第二处：**`_injected_args_keys` 从函数签名直接推导**：

```python
@functools.cached_property
def _injected_args_keys(self) -> frozenset[str]:
    # Inspect the tool's `_run` (falling back to `_arun` for async-only
    # subclasses) for directly injected args like `ToolRuntime` or args
    # annotated with `InjectedToolArg`. ... `StructuredTool` overrides this to
    # inspect its wrapped `func`/`coroutine` instead.
    for method in (self._run, self._arun):
        keys = _get_injected_args_keys_from_signature(method)
        if keys:
            return keys
    return _EMPTY_SET
```

三处：

- **`_run` 优先、`_arun` 作为回退**。因为「只用异步的子类」的 `_run` 可能不存在或是空壳。
- **`if keys: return keys`**——即「找到非空集合就返回」。所以 `_run` 有签名但无注入参数时，会继续看 `_arun`。这个顺序让「同步与异步签名不一致」的工具也能被覆盖。
- **`StructuredTool` 覆写它**，因为它的 `_run` 只是转发，真正的签名在被包装的 `func` 上。这是一处必要的特化：注入参数的判定必须顺着包装链走。

第三处：**执行前从输入里过滤**（`_filter_injected_args`）：

```python
def _filter_injected_args(self, tool_input: dict[str, Any]) -> dict[str, Any]:
    filtered_keys = set[str](FILTERED_ARGS)
    # Add injected args from function signature (e.g., ToolRuntime parameters)
    filtered_keys.update(self._injected_args_keys)
    # If we have an args_schema, use it to identify injected args
    if self.args_schema ...:
        for ...:
            if _is_injected_arg_type(field_type):
                ...
    # Filter out the injected keys from tool_input
```

三层来源都收：固定清单 `FILTERED_ARGS = ("run_manager", "callbacks")`、签名推导的注入键、以及 `args_schema` 里标注的注入字段。**三层必须都覆盖**，因为注入参数可能出现在这三处的任一处（例如用户自定义 `args_schema` 时把某个字段标成注入）。

这个方法的调用点有两处，注释都写「Filter out injected arguments from callback inputs」（第 1058 行与 1186 行，分别对应同步与异步路径）。

**`InjectedToolCallId` 有一处「必须给」的强校验，且错误信息给出了完整的调用形状。**

```python
if _is_injected_arg_type(v, injected_type=InjectedToolCallId):
    if tool_call_id is None:
        msg = ("When tool includes an InjectedToolCallId "
               "argument, tool must always be invoked with a full "
               "model ToolCall of the form: {'args': {...}, "
               "'name': '...', 'type': 'tool_call', "
               "'tool_call_id': '...'}")
        raise ValueError(msg)
    tool_input[k] = tool_call_id
```

三处：

1. **用 `injected_type=InjectedToolCallId` 做精确判定**——不是「任何注入类型」都要求 call id。所以 `ToolRuntime` 参数不会触发这条错误。
2. **`tool_call_id is None` 时报错而不是填 `None`**。这是必要的：工具若用 `tool_call_id` 构造 `ToolMessage`（文档示例正是如此），`None` 会导致下游无法配对。宁可报错。
3. **错误信息把正确的调用形状写全了**（五个键）。这是给调用方（往往是 agent 循环的实现者）看的可操作提示——与 open-interpreter d06 里 `ImplicitInvocation` 那条错误是同一种做法：**把正确形态写进错误里**。

而 `tool_input[k] = tool_call_id` 这一行就是**注入的实际动作**：把框架拿到的 id 塞进参数字典，之后 pydantic 校验会看到它。

**`ToolException` 是一个「不终止 agent」的异常，而这一点写在它的文档里。**

```python
class ToolException(Exception):  # noqa: N818
    """Exception thrown when a tool execution error occurs.

    This exception allows tools to signal errors without stopping the agent.

    The error is handled according to the tool's `handle_tool_error` setting, and the
    result is returned as an observation to the agent.
    """
```

三处：

- **`# noqa: N818`**：这个 lint 规则要求异常类名以 `Error` 结尾。这里刻意不遵守，因为 `ToolException` 已经是既成 API——改名会破兼容。这条 `noqa` 就是「知道规则但不遵守，理由明确」的标记。
- **「signal errors without stopping the agent」**：这是与普通异常的根本区别。抛 `ValueError` 会让整个 agent 停；抛 `ToolException` 会让**这次工具调用**变成一个「观察结果」回给模型。
- **「handled according to the tool's `handle_tool_error` setting」**：所以异常只是一半，处理策略在工具上配置。这是一个**错误处理的二分**：异常表达「发生了什么」，设置表达「怎么处置」。

**`handle_tool_error` 可以返回结构化的内容，而类型定义说明了它的用途。**

```python
ToolExceptionHandlerOutput = str | Sequence[MessageContentBlock]
"""Content returned by a `handle_tool_error` callable.

Error handlers may return plain text or a sequence of structured message
content blocks. When the original tool call includes a `tool_call_id`, this
content is normalized to the content of a `ToolMessage` with `status="error"`.
"""
```

即错误处理器的产出会被规范化成一条 `ToolMessage(status="error")`。所以「工具失败」在消息层面与「工具成功」是同一种形状（都是 `ToolMessage`），只是 status 不同。这解释了为什么 `InjectedToolCallId` 那么重要——构造这条错误消息需要 `tool_call_id`。

**`BaseTool.__init_subclass__` 在类定义时就拦一个常见误写。**

```python
args_schema_type = cls.__annotations__.get("args_schema", None)
if args_schema_type is not None and args_schema_type == BaseModel:
    typehint_mandate = """
class ChildTool(BaseTool):
    ...
    args_schema: Type[BaseModel] = SchemaClass
    ..."""
    raise SchemaAnnotationError(f"Tool definition for {name} must include valid type annotations for argument 'args_schema' ... Expected annotation of 'Type[BaseModel]' but got '{args_schema_type}' ...")
```

即**只判 `args_schema: BaseModel` 这一种误写**——用户写了「基类本身」而不是「子类类型」。注释里 `TODO: Use get_args / get_origin and fully specify valid annotations` 承认了这只是覆盖最常见的错法，而非完整校验。

`__init_subclass__` 的时机很关键：**在类定义时就报错，而不是等到实例化或运行时**。这让误写在校验环节就暴露，而错误信息里直接给出正确写法（`args_schema: Type[BaseModel] = SchemaClass`）。

`args_schema` 字段本身用 `SkipValidation()`：

```python
args_schema: Annotated[ArgsSchema | None, SkipValidation()] = Field(default=None, description="The tool schema.")
```

因为它的值是**类**（`type[BaseModel]`）或**字典**，pydantic 若尝试校验/序列化会失败。`ArgsSchema = TypeBaseModel | dict[str, Any]` 这个联合正好覆盖两种合法形态，文档的说明也列了三条（v2 BaseModel 子类、v1 BaseModel 子类、JSON schema dict）。

**`BaseToolkit` 是工具的分组容器，而它的存在说明「工具集」是独立的概念。**

```python
class BaseToolkit(BaseModel, ABC):
```

一个 toolkit 持有若干工具并给它们共同的命名前缀。它与 `BaseTool` 的关系是**组合而非继承**：toolkit 提供工具，工具自己不关心属于哪个 toolkit。这个分工让「同名工具来自不同 toolkit」能被前缀区分，而这正是 d03 里代码模式命名要处理的情形。

## 关键取舍

**源码事实：** `InjectedToolArg` 是空的标记类，通过 `Annotated[T, InjectedToolArg]` 的元数据位置携带；`InjectedToolCallId` 是它的子类，使判定可以按具体类型收窄。

**源码事实：** 注入判定同时接受「标记的实例」与「标记的类」，并遍历 `get_args(type)[1:]` 的全部元数据（一个参数可带多个标记）。

**源码事实：** `_DirectlyInjectedToolArg` 让 `ToolRuntime` 可以**直接写类型注解**而不必包 `Annotated`，文档称之为比 `Annotated` 更简洁的替代。

**源码事实：** 注解解析用 `get_type_hints(..., include_extras=True)`（不加 `include_extras` 会剥掉 `Annotated` 元数据，导致全部判不出）；批量失败时逐个重试，避免一个坏的前向引用拖累其他参数；同一个字符串注解每次调用最多解析一次（成功与失败都缓存）。

**源码事实：** 字符串注解解析失败时该参数**不被当作注入**（因此模型看见它），并打 debug 日志——失败方向是「让模型可见」。

**源码事实：** 注入参数在三处被处理：schema 生成时强制剔除（即使显式给了 `filter_args`）、`_injected_args_keys` 从 `_run`/`_arun` 签名推导（`StructuredTool` 覆写以顺着包装链）、执行前 `_filter_injected_args` 从输入剔除（来源含 `FILTERED_ARGS` 常量、签名、`args_schema`）。

**源码事实：** `InjectedToolCallId` 有一处强校验：缺 `tool_call_id` 时报 `ValueError`，错误信息里给出完整 `ToolCall` 的五个键；有此参数时把 id 写入参数字典完成注入。

**源码事实：** `ToolException` 刻意不取名 `ToolError`（带 `# noqa: N818`），因为改名会破兼容；它的定位是「不终止 agent 的错误信号」，处置由工具的 `handle_tool_error` 决定。

**源码事实：** `__init_subclass__` 在类定义时拦截 `args_schema: BaseModel`（写成基类而非子类类型）这一种误写并给出正确模板，注释承认这是覆盖最常见错法而非完整校验。

**架构推论：** 「注入参数」这套机制的本质是**给函数签名增加一个「非模型通道」**。它让同一个函数既能接收模型提供的参数，又能接收框架提供的上下文，而对外呈现的 schema 只包含前者。这个设计的价值在 `tool_call_id` 上最明显——那条 `ToolMessage(status="error")` 必须带 id 才能与调用配对，而 id 是模型在发出调用时由框架分配的，模型自己不可能知道。

**架构推论：** 三处处理（schema 剔除 / 输入过滤 / 执行注入）必须全做，这是**纵深防御**的形态：只剔除 schema 时模型仍可伪造；只过滤输入时函数拿不到值；只注入时模型会编。任何一处漏掉，漏洞的形态不同但都真实存在。

**架构推论：** 注解解析失败时选择「让模型可见」这个方向值得单记。反向选择（默认当作注入）会导致「模型看不到、框架也没给」的静默空洞——而那种空洞在运行期表现为函数拿到 `None`，报错点距离成因很远。选「可见」则最坏情况是模型多看到一个参数并尝试填写，问题会以「参数校验失败」的形式出现在近处。

**架构推论：** `ToolException` 与 `handle_tool_error` 构成的二分，把「错误信号」与「错误处置」分开。这与 b01 里 `invalid_tool_calls` 独立成字段是同一条思路的不同层次：那里是「把畸形调用当数据」，这里是「把工具失败当观察结果」。两者都服务于同一个目标——**让 agent 循环能继续跑**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/tools/base.py` | `BaseTool` | 工具基类：名字、描述、`args_schema`、执行入口 |
| `libs/core/langchain_core/tools/base.py` | `BaseTool.__init_subclass__` | 类定义时拦截 `args_schema: BaseModel` 误写并给出模板 |
| `libs/core/langchain_core/tools/base.py` | `BaseToolkit` | 工具分组容器，提供共同命名前缀 |
| `libs/core/langchain_core/tools/base.py` | `ToolException` | 不终止 agent 的错误信号，处置由 `handle_tool_error` 决定 |
| `libs/core/langchain_core/tools/base.py` | `InjectedToolArg` | 空的标记类：不进 schema、执行时注入 |
| `libs/core/langchain_core/tools/base.py` | `InjectedToolCallId` | `InjectedToolArg` 子类，专门注入本次调用的 id |
| `libs/core/langchain_core/tools/base.py` | `_is_injected_arg_type` | 按 `Annotated` 元数据判定，支持实例/类两种形态 |
| `libs/core/langchain_core/tools/base.py` | `_get_injected_args_keys_from_signature` | 注解解析（`include_extras`、逐个重试、失败即「可见」） |
| `libs/core/langchain_core/tools/base.py` | `BaseTool._injected_args_keys` | 从 `_run`/`_arun` 签名推导，`StructuredTool` 覆写 |
| `libs/core/langchain_core/tools/base.py` | `BaseTool._filter_injected_args` | 从输入剔除注入键（三处来源并集） |
| `libs/core/langchain_core/tools/base.py` | `SchemaAnnotationError` | `args_schema` 注解错误专用异常（`TypeError` 子类） |

## 参考

- [langchain 源码：tools/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/tools/base.py)
- [langchain 文档：工具与参数注入](https://docs.langchain.com/oss/python/langchain/tools)
- [Python 文档：typing.Annotated](https://docs.python.org/3/library/typing.html#typing.Annotated)
- [Pydantic 文档：SkipValidation 注解](https://docs.pydantic.dev/latest/api/functional_validators/#pydantic.functional_validators.SkipValidation)

## 与主核 / AI 的关系

「给函数签名开一个非模型通道」与主核里的**系统列注入**是同一种结构。主核的插件函数可以声明它需要 `xmin`、`ctid` 这类系统列，而用户看不见也不需要填这些值——它们由执行器提供。本页的 `InjectedToolArg` 做的就是同一件事：**声明一个由框架提供的参数，且不进入对外 schema**。

「三处处理必须一致」这条与主核里**权限检查的多层覆盖**同理。主核在语句解析、计划生成、执行三个阶段都会检查权限，任何一处的绕过都会造成漏洞。判据相同——**一个安全属性不能只在一个地方实现**，因为它可能被另一个入口绕过。

「解析失败时选择让模型可见」这个方向选择，与主核里**未知对象的处理**有相同的判据。主核遇到无法解析的对象名时会报错而不是猜测它是一个函数调用——因为猜错的后果（调用一个不存在的东西）比报错更糟。本页的判据相同：**不确定时，选择那个「失败在近处」的方向**。

对 AI 工程的实际意义在于：工具是 agent 的手脚，而工具的参数契约决定了模型能做什么、不能做什么。这份实现给出的三条经验可直接用：**注入通道必须三处一致**（schema、输入、执行）、**注解解析不能因为一个坏引用就整体失败**、以及**把不终止 agent 的错误单独建类**（这样「失败」与「崩溃」在语义上分开，前者仍能回到模型那里成为信息）。
