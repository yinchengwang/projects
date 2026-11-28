---
project: langchain
feature: tool-conversion
title: 函数转工具与 schema 生成
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/tools/convert.py
    symbols: [tool, convert_runnable_to_tool]
  - path: libs/core/langchain_core/utils/function_calling.py
    symbols: [FunctionDescription, ToolDescription]
---

# 函数转工具与 schema 生成

## 一句话职责

`tool` 是一个能同时当「装饰器」与「构造函数」用的多态入口，它把普通函数或 `Runnable` 变成 `BaseTool`；而 `FunctionDescription` / `ToolDescription` 是它的下游产物——工具被序列化成模型的函数调用 schema 时的两层结构。

## 机制是什么

这里要解决两个互不相同的问题。

**第一个是入口的形态问题。** 用户会这样写：

```python
@tool
def f(x: int) -> str: ...

@tool("renamed")
def f(x: int) -> str: ...

@tool(parse_docstring=True)
def f(x: int) -> str: ...

t = tool("name", some_runnable)
```

四种写法，而 Python 的装饰器语法要求**同一个函数对象**能同时支持「被直接调用」与「返回装饰器」。`tool` 用了 `@overload` 声明四种签名，实现里靠类型分派。

**第二个是 schema 从哪来的问题。** 函数的参数类型要到 JSON Schema 是**跨表示**的转换：Python 注解 → pydantic 模型 → JSON Schema。而 `Runnable` 的输入可能根本不是一个有名字的模型（可以是 `str`、`TypedDict`、甚至解析不出结构的泛型）。

## 怎么实现的

**`tool` 的四个重载对应四条用法，而实现里用「参数的实际形态」分派。**

```python
def tool(
    name_or_callable: str | Callable[..., Any] | None = None,
    runnable: Runnable[Any, Any] | None = None,
    *args: Any,
    description: str | None = None,
    ...
) -> BaseTool | Callable[[Callable[..., Any] | Runnable[Any, Any]], BaseTool]:
```

分派顺序（在 `_create_tool_factory` 定义之后）：

```python
if len(args) != 0:
    raise ValueError("Too many arguments for tool decorator. A decorator ")
if runnable is not None:
    # tool("name", runnable) —— 当构造函数用
    ...
    return _create_tool_factory(name_or_callable)(runnable)
if name_or_callable is not None:
    if callable(name_or_callable) and hasattr(name_or_callable, "__name__"):
        # @tool —— 无参装饰器
        return _create_tool_factory(name_or_callable.__name__)(name_or_callable)
    if isinstance(name_or_callable, str):
        # @tool("name") —— 带名字的装饰器
        return _create_tool_factory(name_or_callable)
    raise ValueError(...)
# 其余：@tool(parse_docstring=True) —— 带参数的装饰器
def _partial(func): ...
return _partial
```

四处：

1. **`*args` 只用来报错**。「Too many arguments」的注释举了 `@tool("name", runnable, "extra_arg")` 这个例子——第三个位置参数不存在，所以任何多出来的位置参数都是误用。用一个永远为空的 `*args` 拦下这个错误，比让 Python 抛「unexpected keyword」更清楚。
2. **`callable(...) and hasattr(..., "__name__")`**：这是「无参装饰器」的判据。为什么还要 `hasattr`？因为 callable 不等于有名字——`functools.partial` 可能没有 `__name__`。这个双重判据让「传了一个可调用对象」与「传了一个名字字符串」能被分开。
3. **`@tool("name")` 返回的是 `_create_tool_factory(name)` 的结果，而它本身是个装饰器**。所以 `tool("search")` 返回 `Callable[[func], BaseTool]`。
4. **最后的 `_partial` 分支处理「带其他参数但第一个位置参数为空」的情形**。它从 func 推断名字：

```python
name_ = func.get_name() if isinstance(func, Runnable) else func.__name__
```

   注意 `Runnable` 走 `get_name()`（它有自己的命名机制），函数走 `__name__`。两条路径都在这里归一。

**`@overload` 声明四种返回类型，而它们的差别不只是「返回什么」，还有「第一个参数是什么」。**

```python
@overload
def tool(name_or_callable: str, ...) -> Callable[..., BaseTool]: ...
@overload
def tool(name_or_callable: Callable[..., Any], ...) -> BaseTool: ...
@overload
def tool(name_or_callable: str, runnable: Runnable, ...) -> BaseTool: ...
@overload
def tool(*, ...) -> Callable[..., BaseTool]: ...
```

即 `tool("name")` 返回装饰器、`tool(func)` 返回工具、`tool("name", runnable)` 返回工具、`tool(parse_docstring=True)` 返回装饰器。**同名的字符串参数在有无 `runnable` 时返回不同东西**——这是重载存在的理由，因为 Python 的类型系统无法从「`str` 且后面可能跟一个 `Runnable`」推断返回类型。

**描述优先级是一条三级瀑布，而顺序明确写在文档里。**

```
1. description 参数          （即使有 docstring 或 args_schema 也用它）
2. 函数 docstring            （即使有 args_schema 也用它）
3. args_schema 的 description（只在上面两者都没有时才用）
```

这个顺序有实际后果：**显式 `description` 永远赢**，所以它是最可靠的方式。而「docstring 赢过 args_schema」这条说明框架认为「人的说明文字」比「结构化 schema 的描述」更权威——后者往往是 pydantic 生成的模板化文本。

**`Runnable` 分支里有三处必要的处理。**

```python
if isinstance(dec_func, Runnable):
    runnable = dec_func
    if runnable.get_input_jsonschema().get("type") != "object":
        msg = "Runnable must have an object schema."
        raise ValueError(msg)

    async def ainvoke_wrapper(callbacks: Callbacks | None = None, **kwargs: Any) -> Any:
        return await runnable.ainvoke(kwargs, {"callbacks": callbacks})
    def invoke_wrapper(callbacks: Callbacks | None = None, **kwargs: Any) -> Any:
        return runnable.invoke(kwargs, {"callbacks": callbacks})

    coroutine = ainvoke_wrapper
    func = invoke_wrapper
    schema: ArgsSchema | None = runnable.input_schema
    tool_description = description or repr(runnable)
```

三处：

1. **`get_input_jsonschema().get("type") != "object"` 直接拒绝**。理由很明确：工具的输入必须是「一个带命名参数的对象」，否则模型的函数调用无法表达它（函数调用参数天然是一个对象）。所以一个输入是 `str` 的 `Runnable` 不能直接当工具——必须先包成对象。
2. **包装函数把 `callbacks` 显式传下去**：

   ```python
   def invoke_wrapper(callbacks: Callbacks | None = None, **kwargs: Any) -> Any:
       return runnable.invoke(kwargs, {"callbacks": callbacks})
   ```

   这里的 `callbacks` 就是 d01 里 `FILTERED_ARGS = ("run_manager", "callbacks")` 提到的那个被过滤掉的参数之一。**框架注入 `callbacks`，包装函数把它塞进 RunnableConfig**。所以注入链路是完整的：schema 剔除 → 输入过滤 → 这里接收 → 传给下游。

3. **`tool_description = description or repr(runnable)`**。`repr()` 是一个兜底——`Runnable` 通常没有 `__doc__`，所以 `repr` 给出一个能看的标识。注意它**用的是 `repr` 而不是 `_get_description_from_runnable`**（后者生成 `"Takes {input_schema}."`）。这个不一致意味着 `tool()` 路径与 `convert_runnable_to_tool` 路径的默认描述不同。

**`infer_schema` 的两种结果走两条完全不同的构造路径。**

```python
if infer_schema or args_schema is not None:
    return StructuredTool.from_function(
        func, coroutine,
        name=tool_name, description=tool_description, return_direct=return_direct,
        args_schema=schema, infer_schema=infer_schema, response_format=response_format,
        parse_docstring=parse_docstring, error_on_invalid_docstring=error_on_invalid_docstring,
        extras=extras,
    )
# If someone doesn't want a schema applied, we must treat it as
# a simple string->string function
tool_description = tool_description or dec_func.__doc__
if tool_description is None:
    raise ValueError("Function must have either a docstring or description when infer_schema is False.")
return Tool(
    name=tool_name, func=func, description=tool_description, return_direct=return_direct,
    coroutine=coroutine, response_format=response_format, extras=extras,
)
```

两条路径的语义差别值得说清：

- **有 schema** → `StructuredTool`，输入是一个按 schema 校验的字典。
- **无 schema**（`infer_schema=False` 且没给 `args_schema`）→ `Tool`，注释直说「treat it as a simple string->string function」。即输入就是一段字符串，不做结构化。

第二条路径还多了一个前置条件：**必须有描述**（`tool_description or dec_func.__doc__`，两者都空则报错）。为什么只在无 schema 时强制？因为无 schema 时模型看不到参数结构，唯一的引导就是描述——没有描述就完全无从调用。有 schema 时参数名与类型本身已经提供了足够指引。

**`convert_runnable_to_tool` 是「给已存在的 Runnable 加工具外观」的独立入口，它做了两处 `tool()` 没做的事。**

```python
if args_schema:
    runnable = runnable.with_types(input_type=args_schema)
description = description or _get_description_from_runnable(runnable)
name = name or runnable.get_name()

schema = runnable.get_input_jsonschema()
if schema.get("type") == "string":
    return Tool(name=name, func=runnable.invoke, coroutine=runnable.ainvoke, description=description)
```

第一处：**`with_types(input_type=args_schema)`**——用给定的 schema **改写** Runnable 的输入类型，而不是丢弃 Runnable 自己的。这让调用方能强制一个特定的参数形状。

第二处也是最有意思的一处：**输入是字符串时走 `Tool` 而不是报错**。

这与 `tool()` 里「Runnable 必须 object schema 否则报错」**正好相反**。为什么？因为两个入口的目标不同：

- `tool()` 是给**模型**创建工具，模型只能填对象参数，所以字符串输入不可用。
- `convert_runnable_to_tool` 更通用，它可以产出一个「输入是字符串」的 `Tool`（`Tool` 本来就支持 `str` 输入，见 d03）。这种工具适合「被别的地方调用」而不一定被模型调用。

这个不对称是同一份 `Runnable` 在两个入口下得到不同结果的原因，值得注意。

而默认描述也不同：这里用 `_get_description_from_runnable`，产出 `"Takes {input_schema}."`——把输入 schema 直接写进描述。这对模型来说信息量很足（它能从描述里看到参数结构），但也很啰嗦。

**`_get_schema_from_runnable_and_arg_types` 是「schema 推断不出来时的最后手段」。**

```python
def _get_schema_from_runnable_and_arg_types(runnable, name, arg_types=None):
    if arg_types is None:
        try:
            arg_types = get_type_hints(runnable.InputType)
        except TypeError as e:
            msg = ("Tool input must be str or dict. If dict, dict arguments must be typed. "
                   "Either annotate types (e.g., with TypedDict) or pass arg_types into `.as_tool` to specify. " f"{e}")
            raise TypeError(msg) from e
    fields = {key: (key_type, Field(...)) for key, key_type in arg_types.items()}
    return cast("type[BaseModel]", create_model(name, **fields))
```

三处：

1. **从 `runnable.InputType` 上取 `get_type_hints`**。即把 Runnable 声明的输入类型（通常是一个 `TypedDict`）的注解读出来。
2. **每个字段都用 `Field(...)`**——即**全部必填**。这是一个明确的取舍：从 `TypedDict` 推断时无法可靠区分 `NotRequired` 与必填（取决于 `__required_keys__` 的实现细节），所以统一当必填。代价是「可选参数字段」在这里会丢失可选性。
3. **失败信息给出了两条可操作的出路**：给 `TypedDict` 加注解，或者显式传 `arg_types`。这与 d01 里 `InjectedToolCallId` 的错误文案是同一种做法——**把修法写进错误里**。

`convert_runnable_to_tool` 何时走这条兜底路径，判据写得很具体：

```python
is_root_model = isinstance(input_schema_cls, type) and issubclass(input_schema_cls, RootModel)
if (
    arg_types is None
    and schema.get("type") == "object"
    and schema.get("properties")
    and not is_root_model
):
    args_schema = input_schema_cls
else:
    args_schema = _get_schema_from_runnable_and_arg_types(runnable, name, arg_types=arg_types)
```

即**四个条件全都满足才直接用 Runnable 自带的 schema**：

- 没显式给 `arg_types`（给了就以它为准）。
- 是 object 类型（字符串分支已经提前返回了，这里防的是别的类型）。
- **`schema.get("properties")` 非空**——这条防的是「object 但没有属性」，那种 schema 无法作为工具参数。
- **不是 `RootModel` 子类**。注释解释了理由：「Detect `RootModel` input schemas so we don't use them directly as the tool args schema」。`RootModel` 是「只有一个根值」的模型（如 `RootModel[list[str]]`），它的 JSON Schema 会是数组而不是对象——直接当参数 schema 用会让工具变成一个没有命名字段的东西。

**`FunctionDescription` 与 `ToolDescription` 是「工具长什么样」的两层。**

```python
class FunctionDescription(TypedDict):
    name: str
    description: str
    parameters: dict[str, Any]

class ToolDescription(TypedDict):
    type: Literal["function"]
    function: FunctionDescription
```

`parameters` 的类型是 `dict[str, Any]` 而**不是**一个专门的 schema 类型。这是一个刻意的宽松：参数部分是 JSON Schema，而 JSON Schema 本身是开放的，用任意字典承载比造一个精确类型更实用。

`ToolDescription` 只是一层包装（`type: "function"` + 内嵌 `FunctionDescription`）。这个嵌套不是冗余——它是 OpenAI 工具定义的实际形状：

```json
{"type": "function", "function": {"name": ..., "description": ..., "parameters": {...}}}
```

所以这两层直接对应线上的 JSON 结构，**不是框架自己的抽象**。这也是为什么 `type` 是 `Literal["function"]`：目前只有这一种工具类型（未来可能有 `"code_interpreter"` 之类）。

`FunctionDescription` 的文档说它是「Representation of a callable function to send to an LLM」，而 `ToolDescription` 说「to the OpenAI API」——**两者的目标读者不同**。前者是通用的「函数描述」，后者特指某个 API 的形状。这个措辞差异暗示 `FunctionDescription` 可能被其他协议复用，而 `ToolDescription` 是绑定到具体 API 的。

**这两层结构与 `tool()` 的输出之间还有一次转换。** `tool()` 产出 `BaseTool`，而 `BaseTool` 到 `ToolDescription` 的转换在 `convert_to_openai_tool`（h03 那篇的范围）。所以本篇只负责「函数 → 工具」，向模型协议的那一步是另一层。

## 关键取舍

**源码事实：** `tool` 用四个 `@overload` 对应四种用法，实现里按「第一个位置参数是 callable 还是 str、有没有 `runnable`、是否全为空」分派；`*args` 仅用于拦截多余位置参数。

**源码事实：** 描述优先级是 `description` 参数 > 函数 docstring > `args_schema` 描述，且显式 `description` 在任何情况下都赢。

**源码事实：** `tool()` 对 `Runnable` 要求输入 schema 是 `object` 类型否则报错；且默认描述用 `repr(runnable)`，与 `convert_runnable_to_tool` 用 `"Takes {schema}."` 不一致。

**源码事实：** `infer_schema=False` 且无 `args_schema` 时产出 `Tool`（简单字符串输入），且此时**强制要求有描述**，否则报错。

**源码事实：** `convert_runnable_to_tool` 在输入 schema 是 `string` 时产出 `Tool` 而非报错——与 `tool()` 相反；`args_schema` 会通过 `with_types` 改写 Runnable 的输入类型。

**源码事实：** 只有当「无 `arg_types`、是 object 类型、`properties` 非空、不是 `RootModel`」四条全满足时才直接用 Runnable 自带 schema，否则退回 `create_model` 推断。

**源码事实：** 推断路径把每个字段都设成 `Field(...)`（全部必填），失败信息给出「加 `TypedDict` 注解」或「传 `arg_types`」两条出路。

**源码事实：** `FunctionDescription` 是「给 LLM 的函数描述」，`ToolDescription` 是它的 OpenAI 形状包装（`type: "function"` + 嵌套 `function`），`parameters` 用 `dict[str, Any]` 承载开放 JSON Schema。

**架构推论：** `tool` 同时做装饰器与构造函数，是靠**参数形态分派**而不是两套 API。代价是重载签名复杂、且「`tool("name")` 与 `tool("name", runnable)` 返回不同类型」这条需要靠类型重载传达。收益是一个入口能覆盖全部写法——对这类高频使用的 API，降低「该用哪个」的认知负担比简化实现更重要。

**架构推论：** 「`Runnable` 必须 object schema」这条限制在 `tool()` 里是硬性拒绝，而在 `convert_runnable_to_tool` 里被绕过。这个不一致的根源是**目标不同**：前者面向模型调用（模型只能填对象），后者面向通用封装。这类「同一份输入在不同入口下得到不同结果」的情况需要在文档上到位，否则用户会困惑为什么一个 `str` 输入的 Runnable 在 `tool("x", r)` 上失败而在 `.as_tool()` 上成功。

**架构推论：** 推断路径把字段全部设为必填，是一个「宁可要求严格也不误判可选」的选择。若把 `NotRequired` 误判成必填，模型会为每个字段都产生值（可能给出无意义的占位）；若把必填误判成可选，模型可能漏掉关键参数。前者错得明显、后者错得隐蔽，所以选前者。

**架构推论：** `tool()` 的 docstring 里有一段专门的「Reserved argument names」警告：

> Avoid naming tool arguments `config`, `run_manager`, or `callbacks`. These collide with values LangChain injects into tools at call time. A `config` argument is shadowed by the injected `RunnableConfig`, so the tool fails at call time with `TypeError: ... missing 1 required positional argument: 'config'`, while `run_manager` and `callbacks` are silently dropped from the generated schema. To access runtime information (state, context, store, config), add a `ToolRuntime` parameter instead.

这段是整个工具契约里最值得记住的「坑」。它揭示了三件事：注入通道会**静默覆盖**同名参数（`config` 表现为运行期 `TypeError`）；`run_manager`/`callbacks` 被静默从 schema 剔除（即模型看不到但也不报错）；而正确的替代方案是显式加 `ToolRuntime`。这个警告之所以必须写在文档里，正是因为两种失败形态都很隐蔽——一个在运行期才炸，一个根本不炸。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/tools/convert.py` | `tool` | 四态装饰器/构造函数入口，按参数形态分派 |
| `libs/core/langchain_core/tools/convert.py` | `tool._create_tool_factory` | 内层工厂：区分 Runnable / 协程函数 / 普通函数，选 `StructuredTool` 或 `Tool` |
| `libs/core/langchain_core/tools/convert.py` | `tool._partial` | 带参数装饰器的收尾，从 func 推断名字 |
| `libs/core/langchain_core/tools/convert.py` | `convert_runnable_to_tool` | 独立入口：给 Runnable 加工具外观，字符串输入走 `Tool` |
| `libs/core/langchain_core/tools/convert.py` | `_get_description_from_runnable` | 兜底描述：`"Takes {input_schema}."` |
| `libs/core/langchain_core/tools/convert.py` | `_get_schema_from_runnable_and_arg_types` | 最后手段：从 `InputType` 注解 + `create_model` 造 schema |
| `libs/core/langchain_core/utils/function_calling.py` | `FunctionDescription` | 给 LLM 的函数描述：名字、描述、参数 JSON Schema |
| `libs/core/langchain_core/utils/function_calling.py` | `ToolDescription` | OpenAI 工具形状：`type: "function"` + 嵌套 `function` |

## 参考

- [langchain 源码：tools/convert.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/tools/convert.py)
- [langchain 文档：工具](https://docs.langchain.com/oss/python/langchain/tools)
- [OpenAI 文档：函数调用工具格式](https://platform.openai.com/docs/guides/function-calling)
- [Pydantic 文档：create_model 动态建模](https://docs.pydantic.dev/latest/api/base_model/#pydantic.create_model)

## 与主核 / AI 的关系

「同一入口用参数形态分派」与主核里**重载函数的解析**同源。主核会根据参数个数与类型选择匹配的重载（例如 `substr(text, int)` 与 `substr(text, int, int)`），而本页的 `tool` 靠「第一个参数是不是 callable」分派。两者的共同代价是**错误必须自己构造**——Python 抛出的通用错误对用户没用，所以两处都手写了明确的消息。

「schema 推断不出来才退回 `create_model`」与主核里**类型推断的多级降级**一致。主核从常量字面量能推出精确类型，从列定义能推出声明类型，最后兜底是 `unknown`。本页同样分三级：直接用 Runnable 的 schema、从 `TypedDict` 注解造、报错并给出修法。

「Reserved argument names」那段警告对应主核里**保留字**的存在。SQL 里 `user`、`current_date` 这类名字有特殊含义，用作列名需要加引号。判据相同——**框架需要占据一部分命名空间，用户必须知道边界在哪**。而本页的问题更难，因为它不报错：`run_manager` 与 `callbacks` 被静默剔除，用户只会发现「模型看不到这个参数」。

对 AI 工程的实际意义在于：从 Python 函数到模型可调用的工具，这条转换链的每一步都有信息损失（`NotRequired` 变必填、docstring 变成描述、注解变成 JSON Schema）。这份实现给出的三条经验可直接用：**显式声明的描述优先级最高**（因为它是唯一不受推断影响的信息）、**保留名必须写进文档而不是靠报错**（静默行为无法被用户发现）、以及**推断失败要给两条出路而不是只报错**。
