---
project: haystack
feature: tools
title: 工具定义与工具集
page_type: feature
schema_version: 1
refs:
  - path: haystack/tools/tool.py
    symbols: [Tool]
  - path: haystack/tools/from_function.py
    symbols: [create_tool_from_function]
  - path: haystack/tools/toolset.py
    symbols: [Toolset]
---

# 工具定义与工具集

## 一句话职责

把「一个可被模型调用的能力」定义成一个 `@dataclass`，里面既有给模型看的三件（`name` / `description` / `parameters` 的 JSON Schema），也有模型看不到的四个行为配置（从哪读状态、往哪写状态、怎么转成字符串、异步版本）；再从函数签名与 docstring 自动生成前者，并允许把多个工具组成一个可继承、可序列化、可按运行隔离的工具集。

## 机制是什么

`Tool` 是 `@dataclass`，八个字段分两类：

**给模型的三件**——`name`、`description`、`parameters`（一个 JSON Schema）。这三者构成 `tool_spec` 属性，也正是 chat generator 要传给 provider 的东西。类 docstring 有一句关键提示：「`name` 与 `description` 这类文本属性的**准确**定义对模型正确准备调用很重要」。

**给运行时的四项**——`function` / `async_function`（至少一个）、`outputs_to_string`（怎么变成给模型看的文本）、`inputs_from_state`（哪些参数从 State 取）、`outputs_to_state`（返回值写回 State 的哪些键）。

所以这个数据类的形状本身就说明了 haystack 对「工具」的理解：**工具不只是「给模型一个函数」，它是一个同时向模型侧与状态侧双向读写的东西**——模型侧拿参数、看文本结果；状态侧提供隐藏参数、接收结构化产出。

`create_tool_from_function(function, ...)` 负责从函数签名生成 `parameters`；`tool` 装饰器只是它的一个薄包装（`@overload` 两种调用形态都收敛到它）。`Toolset` 是一个 `@dataclass` 的容器，实现 `__iter__` / `__contains__` / `__len__` / `__getitem__`，所以能当列表用。

## 怎么实现的

**`Tool.__post_init__` 是全文件最长的一段，而它做的是一件值得称赞的事：把几乎所有配置错误挡在构造期。** 逐组看：

**第一组：函数形态三道检查。**

```python
if self.function is None and self.async_function is None:
    raise ValueError(...requires at least one of `function` or `async_function`...)
if self.function is not None and inspect.iscoroutinefunction(self.function):
    raise ValueError(...`function` must be a synchronous function... Pass it as `async_function` instead.)
if self.async_function is not None and not inspect.iscoroutinefunction(self.async_function):
    raise ValueError(...`async_function` must be a coroutine function defined with `async def`...)
```

三条分别是「至少要有一个」「`function` 不能是协程」「`async_function` 必须是协程」。第 2 条的错误消息直接给出修法（「把它作为 `async_function` 传」）。

为什么要把 `function` 与 `async_function` 分开而不是「一个字段两种类型」？因为同步与异步两条调用路径的语义不同：`invoke` 只能跑同步函数，`invoke_async` 在有原生异步时优先用它。若混在一个字段里，`invoke` 就得在运行时判断能不能跑——而失败点会推迟到调用时。分成两个字段让「这个工具支不支持同步调用」在构造期就确定。

**第二组：`parameters` 必须是合法的 JSON Schema。**

```python
Draft202012Validator.check_schema(self.parameters)
```

用的是 JSON Schema 2020-12 草案的校验器——**不是「看起来像」，而是真的按规范校验**。所以一个拼错的 schema（例如 `"type": "strng"`）在这里就报错，而不是等 provider 拒绝或模型给出乱参数。

**第三组：`outputs_to_state` 的每个配置。**

```python
for key, config in self.outputs_to_state.items():
    if not isinstance(config, dict): raise TypeError(...)
    if "source" in config and not isinstance(config["source"], str): raise ValueError(...)
    if "handler" in config and not callable(config["handler"]): raise ValueError(...)
```

然后还有一层**跨字段校验**：

```python
valid_outputs = self._get_valid_outputs()
if valid_outputs is not None:
    for state_key, config in self.outputs_to_state.items():
        source = config.get("source")
        if source is not None and source not in valid_outputs:
            raise ValueError(f"outputs_to_state: '{self.name}' maps state key '{state_key}' to unknown output '{source}'Valid outputs are: {valid_outputs}.")
```

注意那句错误消息里 `'{source}'` 与 `Valid outputs are` 之间**少了一个空格**（两个 f-string 相邻拼接，前者以 `'` 结尾后者以 `V` 开头）——一个排版瑕疵，但内容是完整的。

这层校验的价值在于 `_get_valid_outputs()` 的默认实现返回 `None`（跳过校验）：

```python
def _get_valid_outputs(self) -> set[str] | None:
    return None
```

docstring 解释：「默认返回 `None` 因为普通基于函数的工具没有形式化的输出 schema。返回 `None` 时输出校验被跳过。`ComponentTool` 这类子类覆盖它来返回组件输出 socket 名，从而为输出已知的工具启用校验」。

所以这是「能力按实现提供」的经典形态：能校验的就校验，不能的就不假装能。而 `_get_valid_inputs()` 相反——它**默认能校验**：

```python
introspection_target = self.function if self.function is not None else self.async_function
try:
    sig = inspect.signature(introspection_target)
except (ValueError, TypeError):
    return None
return set(sig.parameters) | set(self.parameters.get("properties", {}))
```

两个来源的并集：函数签名 + schema 里的 `properties`。docstring 解释了为什么需要并集：「签名里还包含从 State 提供的参数，它们可能被从暴露给模型的 schema 里省略」。

这是这一层最重要的一个细节：**`inputs_from_state` 注入的参数不在 schema 里**（模型不该看到它），但它是合法参数。所以校验「`inputs_from_state` 指向的参数存在」时必须查签名而不是查 schema——若只查 schema，所有状态注入的配置都会被误判为错误。

**第四组：`outputs_to_string` 的两套格式，各有各的严格校验。** 单输出格式：

```python
if "source" in ... and not isinstance(..., str): raise
if "handler" in ... and not callable(...): raise
if "raw_result" in ... and not isinstance(..., bool): raise
for key in self.outputs_to_string:
    if key not in {"source", "handler", "raw_result"}:
        raise ValueError("Invalid outputs_to_string config. When using 'source', 'handler' or 'raw_result' at the root level, no other keys are allowed. Use individual output configs instead.")
```

最后那条「根级出现这三种键时不允许其他键」是关键：它消除了「这到底是单输出还是多输出」的歧义。若允许混合，`{"source": "docs", "summary": {...}}` 就有两种解读。

多输出格式的三条：

```python
if not isinstance(config, dict): raise TypeError
if "raw_result" in config: raise ValueError(...'raw_result' is not supported in the multiple output format.)
if "source" not in config: raise ValueError(...each output must have a 'source' defined.)
```

第二条「多输出不支持 `raw_result`」在 `_build_tool_result_message` 里还有对应的运行时强制（`{**cfg, "raw_result": False}`），所以构造期报错只是提前了一点。第三条「每个输出必须有 `source`」是必需——多输出格式下没有 `source` 就不知道从结果的哪个键取值。

**第五组：`inputs_from_state` 的值必须是字符串且指向存在的参数。**

```python
if not isinstance(param_name, str): raise TypeError(...)
if valid_inputs is not None and param_name not in valid_inputs: raise ValueError(...Valid parameters are: {valid_inputs}.)
```

注意方向：`inputs_from_state` 是 `{state_key: param_name}` ——**键是状态键、值是参数名**。所以校验查的是 `param_name`（值）在不在合法参数里，而不是 `state_key`。这符合语义：状态里有什么键是运行时的事，但工具必须真的有那个参数。

**`create_tool_from_function` 里有一段七步的过滤，每一步都在处理一种「不该出现在 schema 里」的参数。** 循环体内按顺序：

1. `param.kind in (VAR_POSITIONAL, VAR_KEYWORD)` → `continue`。注释：「跳过 `*args` 和 `**kwargs`，因为 LLM 按名字传参数」。这条很实在：变参没法用 JSON Schema 表达。

2. `if inputs_from_state and param_name in inputs_from_state.values()` → `continue`。**从状态注入的参数不暴露给模型**。

3. `if _is_state_annotation(annotation)` → `continue`。注释：「跳过 `State` 类型的参数（包括 `Optional[State]`）——Agent 的工具执行在运行时注入它们」。

4. `if annotation is param.empty` → `raise ValueError(f"Function '{...}': parameter '{...}' does not have a type hint.")`。**没类型标注的参数报错**而不是猜一个类型。这是必要的：没有类型就没有 JSON Schema。

5. `if _contains_callable_type(annotation)` → `continue`。注释：「跳过 `Callable` 类型，因为 Pydantic 无法为它们生成 JSON schema」。

6. 下划线开头且**没有默认值** → 报错：

```python
if param_name.startswith("_") and param.default is param.empty:
    raise ValueError(f"...required parameter '{param_name}' starts with an underscore and can't be part of the tool schema. Provide it through `inputs_from_state` or give it a default.")
```

注释解释了原因：「Pydantic 把下划线开头的名字当作私有属性并从 schema 里排除」。所以「下划线 + 无默认值」的参数会既不在 schema 里、又在调用时缺值——必须在构造期挡住。而错误消息给了两条修法：走 `inputs_from_state`，或给默认值。

注意这里**有默认值的下划线参数被放过**——它会留在函数签名里、不在 schema 里、调用时用默认值。这正好是「内部常量参数」的用法。

7. 默认值与描述：

```python
default = param.default if param.default is not param.empty else ...
fields[param_name] = (annotation, default)
if hasattr(annotation, "__metadata__"):
    descriptions[param_name] = annotation.__metadata__[0]
```

`...`（Ellipsis）是 Pydantic 表达「必填」的方式，注释写明了。而 `__metadata__` 是 `Annotated[...]` 的元数据——取第一个作为参数描述。

**`_remove_title_from_schema` 是这一层最精细的一个函数。** 它要解决的现实问题是：Pydantic 生成的 JSON Schema 里到处是 `title` 关键字，而这些对模型是无用的冗余信息（每个属性一个 `title`，值与属性名相同）。注释引了 Pydantic 的讨论链接说「没有编程方式阻止 Pydantic 添加它们，所以之后删掉」。

但删 `title` 不能简单递归删除——因为 **`title` 也可能是用户自己起的名字**。所以函数维护了两张关键字表：

```python
_NAME_KEYED_SCHEMA_MAPS = frozenset({"properties", "patternProperties", "$defs", "definitions", "dependentSchemas", "dependentRequired"})
_DATA_SCHEMA_KEYWORDS = frozenset({"default", "const", "enum", "examples", "example"})
```

第一张表是「**键由用户命名的映射**」。注释解释得很到位：「`properties` 这类映射的键是属性名或定义名，不是 JSON Schema 关键字。所以即使它们拼写为 `title`，也必须保留——递归只进值，不能删键。在这里删一个键会丢掉一个已声明的属性，或留下一个悬空的 `$ref`」。

所以一个名为 `title` 的参数：`properties["title"]` 这个**键**必须保留，只递归进它的**值**删里面的 `title` 关键字。

第二张表是「**值是实例数据而非子 schema**」。注释：「`default` 里的 `title` 键是那个值的一部分，删掉它会改变工具的契约」。所以 `{"default": {"title": "x"}}` 里的 `title` 不动。

函数体的分支顺序：

```python
if key in _NAME_KEYED_SCHEMA_MAPS and isinstance(value, dict):
    for sub_schema in value.values():
        if isinstance(sub_schema, dict): _remove_title_from_schema(sub_schema)
elif key in _DATA_SCHEMA_KEYWORDS:
    continue
elif key == "title":
    del schema[key]
elif isinstance(value, dict):
    _remove_title_from_schema(value)
elif isinstance(value, list):
    for item in value:
        if isinstance(item, dict): _remove_title_from_schema(item)
```

五支。第 2 支 `continue` 对整棵子树都不递归——所以 `default: {"title": ...}` 被完整保留，连它内部更深层的 schema 也不动。这是一个保守的选择（宁可漏删也不误删）。

这个函数是这一页最值得学的实现：**「删除某个键」这件事在递归结构里必须区分「键是名字」与「键是关键字」**。同一份 JSON 里，`title` 在 `{"title": "Person"}` 里是关键字，在 `{"properties": {"title": {...}}}` 里是属性名——纯语法层面的递归无法区分它们。

**`create_tool_from_function` 的最后二选一决定了工具走哪条调用路径。**

```python
is_async = inspect.iscoroutinefunction(function)
return Tool(
    name=name or function.__name__,
    description=tool_description,
    parameters=schema,
    function=None if is_async else function,
    async_function=function if is_async else None,
    ...
)
```

所以只有 `function` 或只有 `async_function` 被填充，两者不共存。于是 `Tool.invoke` 在纯异步工具上会抛 `ToolInvocationError`（消息明确指引「用 `Agent.run_async`」），而 `Tool.invoke_async` 在纯同步工具上会 `await asyncio.to_thread(self.invoke, **kwargs)`。

`tool_description` 的取值有一条容易忽略的语义：

```python
tool_description = description if description is not None else (function.__doc__ or "")
```

用的是 `if description is not None` 而不是真值判断——**所以传空字符串会得到空描述，而不回退到 docstring**。docstring 里也明说了：「要故意让描述为空，传一个空字符串」。这是一个「显式覆盖默认」的正确写法，值得与「用 `or` 导致空串被当没传」的错误做法对照。

**`Tool.to_dict` / `from_dict` 要处理三处不可直接序列化的东西。** 实现：

```python
data = asdict(self)
data["function"] = serialize_callable(self.function) if self.function is not None else None
data["async_function"] = serialize_callable(self.async_function) if self.async_function is not None else None
if self.outputs_to_state is not None:
    data["outputs_to_state"] = _serialize_outputs_to_state(self.outputs_to_state)
if self.outputs_to_string is not None:
    data["outputs_to_string"] = _serialize_outputs_to_string(self.outputs_to_string)
return {"type": generate_qualified_class_name(type(self)), "data": data}
```

三处：两个函数用 `serialize_callable`；两个配置字典里可能藏 `handler`（也是一个 callable），用 `_convert_handler_in_configs` 逐个转换；最外层包 `{"type": ..., "data": ...}`。

反序列化是严格镜像，且有一处**不对称**：

```python
if "outputs_to_state" in init_parameters and init_parameters["outputs_to_state"]:
    init_parameters["outputs_to_state"] = _deserialize_outputs_to_state(...)
if init_parameters.get("outputs_to_string") is not None:
    init_parameters["outputs_to_string"] = _deserialize_outputs_to_string(...)
```

第一处用 `in ... and ...` 的双重判定（既要求键存在又要求值为真），第二处只用 `get(...) is not None`。所以「`outputs_to_state` 是空字典」时会被跳过转换，而「`outputs_to_string` 是空字典」时会被转换（转换空字典也是空字典，无害）。这是一个轻微的写法不一致，不影响正确性。

而 `_is_single_output_config` 在序列化路径上被复用——它决定「handler 在根级还是在每个子配置里」。所以多输出格式序列化时，`_convert_handler_in_configs` 会遍历每个输出；单输出格式时，`_convert_handler` 只看根级。

**`Toolset` 的三件事：当列表用、能继承做动态加载、能按运行隔离。** 三件事各有一个实现点：

**当列表用**——四个魔法方法：

```python
def __iter__(self): return iter(self.tools)
def __contains__(self, item): ...
def __len__(self): return len(self.tools)
def __getitem__(self, index): return self.tools[index]
```

`__contains__` 支持两种查法：`isinstance(item, str)` 时按名字匹配，`isinstance(item, Tool)` 时用 `tool is item or tool == item`。所以「按名字查」与「按对象查」都成，且对象查优先用身份（`is`）再退回相等（`==`）——避免为每个元素算一次 `__eq__`。

`__post_init__` 有一条专门的防御：

```python
if isinstance(self.tools, Tool):
    raise TypeError("A single Tool cannot be directly passed to Toolset. Please use a list: Toolset([tool])")
```

因为 `Tool` 是可迭代的 dataclass 吗？不是——但 `Toolset([tool])` 与 `Toolset(tool)` 在用户看来很像，而后者会让 `self.tools` 变成一个 Tool 对象，随后所有列表操作都错。这条检查把这种误用变成一条带修法的错误。

**能继承做动态加载**——类 docstring 第二个用途里给了完整的示例约定：

- 在 `warm_up()` 里加载工具并赋给 `self.tools`，「因为 `warm_up()` 可能在每次运行前被调用，用你自己的状态做守卫让它幂等（例如 `if self._client is not None: return`）」。
- 覆盖 `to_dict()` / `from_dict()` 来序列化**端点描述符**（URL、服务器信息）而不是动态加载出来的 Tool 实例。

第二条是关键的设计指引：动态加载的工具不可能序列化（它们来自远端，可能有无数个），所以序列化的对象应该是「怎么连到那个远端」。这让「配置」与「加载结果」分离。

**能按运行隔离**——`spawn`：

```python
def spawn(self, selected_tool_names: set[str] | None = None) -> "Toolset":  # noqa: ARG002
    return self
```

默认实现返回 `self`，`selected_tool_names` 参数被 `# noqa: ARG002` 标注为未使用。docstring 解释了为什么可以这样：「一个普通 `Toolset` 没有运行期作用域的状态，所以默认实现返回 `self` 并忽略选择（由 Agent 物化它）。有运行期作用域状态的子类（例如 `SearchableToolset`）覆盖这个方法来返回一份携带选择的副本，这样并发运行共享同一个配置好的 `Toolset` 时不会互相破坏」。

所以这是一个**默认零成本、子类按需付费**的契约。而 `get_selectable_tools` 是配套的：

```python
def get_selectable_tools(self) -> list[Tool]:
    return list(self.tools)
```

docstring：「返回可用于按名字选择的工具（例如通过 `Agent.run(tools=["tool_name"])`）。在按名字选择前用 `warm_up_tools` 或 `warm_up_tools_async` 预备惰性工具集。**迭代时不会浮现所有可选择的工具的子类（例如 `SearchableToolset`）覆盖这个来返回全集**」。

所以「能迭代出来的」与「能按名字选择的」是两个概念。一个搜索型工具集可能只让你迭代出一部分（当前可见的），但按名字可以选到更多。把两者分开，让「给模型看的」与「用户能指定的」不被绑死。

**`Tool` 的 `warm_up` / `close` 是「约定式接口」而不是抽象方法。** 类 docstring 写：

「对于资源密集的操作（例如建立到远端服务的连接或加载模型），实现一个可选的 `warm_up()` 方法。它在调用之前以及管道/agent 装配期间被调用，**必须幂等**。原生异步工具可以改为实现 `warm_up_async()`。实现 `close()` 和/或 `close_async()` 来释放持有的资源；**这些方法必须能在 warm-up 之前调用，且可被多次调用**」。

而 `invoke` 里的实现是 `if hasattr(self, "warm_up"): self.warm_up()` ——用 `hasattr` 而不是基类定义空方法。所以 `Tool` 不声明这些接口，只在运行时探测。这是一个「鸭子类型的可选钩子」，代价是「有没有实现」只能靠 `hasattr` 判断，没有类型检查保证。

同一个 `invoke` 还把所有异常包成 `ToolInvocationError`：

```python
try:
    if hasattr(self, "warm_up"): self.warm_up()
    result = self.function(**kwargs)
except Exception as e:
    raise ToolInvocationError(f"Failed to invoke Tool `{self.name}` with parameters {kwargs}. Error: {e}", tool_name=self.name) from e
```

所以 `warm_up` 的失败与业务逻辑的失败被统一成同一种错误类型。这让 h02 的「按 `raise_on_failure` 决定抛还是转成错误消息」只需要处理一种异常。

## 关键取舍

**源码事实：** `Tool` 的八个字段分两类：给模型的三件（`name`/`description`/`parameters`）与运行时的四项行为配置。

**源码事实：** `parameters` 用 `Draft202012Validator.check_schema` 做真正的规范校验，不是「看起来像」。

**源码事实：** `_get_valid_outputs()` 默认返回 `None`（跳过输出名校验），`_get_valid_inputs()` 默认能校验且取「函数签名 ∪ schema properties」的并集——因为状态注入的参数不在 schema 里。

**源码事实：** `create_tool_from_function` 七步过滤掉变参、状态注入参数、`State` 注解参数、`Callable` 类型；无类型标注与「下划线开头且无默认值」则报错。

**源码事实：** `_remove_title_from_schema` 用两张关键字表区分「键是名字」与「键是实例数据」，所以名为 `title` 的参数与 `default` 里的 `title` 都不会被删。

**源码事实：** `description if description is not None` 允许用空字符串显式清空描述，不回退到 docstring。

**源码事实：** `create_tool_from_function` 只在 `function` 与 `async_function` 之一上放函数，两者不共存。

**源码事实：** `Tool.to_dict` 需要转换三处不可序列化内容（两个 callable 与配置字典里的 `handler`）；`from_dict` 的两处判断写法不一致但无害。

**源码事实：** `Toolset.spawn` 默认返回 `self` 并忽略选择，有运行期状态的子类必须覆盖它实现隔离。

**源码事实：** `get_selectable_tools` 与 `__iter__` 是两件事，「能按名字选择的」可以多于「能迭代出来的」。

**源码事实：** `warm_up` / `close` 是通过 `hasattr` 探测的鸭子类型可选钩子，不是基类声明的抽象方法；`invoke` 把它们的失败与业务失败统一包成 `ToolInvocationError`。

**架构推论：** 「给模型的三件」与「运行时的四项」分开，是这一层最重要的结构判断。模型只能理解 `name`/`description`/`parameters`；而「参数从状态来」「结果写回状态」「结果怎么转字符串」全都是**宿主系统的概念**，模型既不需要也无法理解。把它们放在同一个数据类里、但清楚分成两组，让「一个工具定义」同时服务两个消费者而不混淆。

**架构推论：** `_remove_title_from_schema` 那两张表说明「在递归结构里删键」需要**语义知识而不只是语法递归**。JSON Schema 本身没有区分「这个键是关键字还是名字」的语法标记，只能靠维护「哪些关键字的值是映射、哪些是数据」的清单。任何对通用格式做变换的工具都需要同类知识——否则要么漏删（冗余留在产物里）要么误删（破坏语义）。

**架构推论：** `spawn` 那个「默认返回 self」的契约，是把并发隔离的成本精确落到需要它的实现上。这对库设计很重要：若默认实现就拷贝，所有用户都为「可能不需要的隔离」付出代价；若默认不拷贝且不提供 `spawn` 钩子，有状态的子类就没有正确的做法。提供一个可覆盖的 `spawn` 让两者都成立。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/tools/tool.py` | `Tool` | 八字段的 dataclass：三件给模型 + 四项运行时配置 |
| `haystack/tools/tool.py` | `Tool.__post_init__` | 五组构造期校验，覆盖函数形态到配置结构 |
| `haystack/tools/tool.py` | `Tool._get_valid_inputs` | 签名 ∪ schema properties，用于校验状态映射 |
| `haystack/tools/tool.py` | `Tool._get_valid_outputs` | 默认 `None`（跳过），子类可提供输出名集合 |
| `haystack/tools/tool.py` | `Tool.tool_spec` | 给模型的三件组成的规范 |
| `haystack/tools/tool.py` | `Tool.invoke` | 同步调用，`hasattr` 探测 `warm_up`，统一包错 |
| `haystack/tools/tool.py` | `Tool.invoke_async` | 有原生异步则用它，否则 `asyncio.to_thread` |
| `haystack/tools/tool.py` | `Tool.to_dict` | 三处 callable 的序列化 |
| `haystack/tools/tool.py` | `_serialize_outputs_to_state` | 逐个配置序列化 `handler` |
| `haystack/tools/tool.py` | `_deserialize_outputs_to_string` | 按单/多输出格式分别反序列化 |
| `haystack/tools/tool.py` | `_is_single_output_config` | 靠「值是不是字典」判别两种格式 |
| `haystack/tools/tool.py` | `_check_duplicate_tool_names` | 重名检查，报出全部重复名 |
| `haystack/tools/from_function.py` | `create_tool_from_function` | 从签名+docstring 生成 schema 与 Tool |
| `haystack/tools/from_function.py` | `tool` | 装饰器包装，两种调用形态 |
| `haystack/tools/from_function.py` | `_remove_title_from_schema` | 删 `title` 但保留名为 `title` 的键与数据 |
| `haystack/tools/from_function.py` | `_NAME_KEYED_SCHEMA_MAPS` | 「键由用户命名」的五类映射关键字 |
| `haystack/tools/from_function.py` | `_DATA_SCHEMA_KEYWORDS` | 「值是实例数据」的五个关键字 |
| `haystack/tools/toolset.py` | `Toolset` | 容器、动态加载基类、运行隔离契约 |
| `haystack/tools/toolset.py` | `Toolset.spawn` | 默认返回 `self`，有状态子类负责拷贝 |
| `haystack/tools/toolset.py` | `Toolset.get_selectable_tools` | 「能按名字选」与「能迭代出」分开 |
| `haystack/tools/toolset.py` | `Toolset.__post_init__` | 拒绝 `Toolset(tool)` 这种误用 |

## 参考

- [haystack 源码：tools/tool.py](https://github.com/deepset-ai/haystack/blob/main/haystack/tools/tool.py)
- [haystack 源码：tools/from_function.py](https://github.com/deepset-ai/haystack/blob/main/haystack/tools/from_function.py)
- [haystack 文档：工具](https://docs.haystack.deepset.ai/docs/tools)

## 与主核 / AI 的关系

主核里最接近的是「用户自定义函数（UDF）的注册契约」：一个函数要能被引擎调用，必须声明入参类型、返回类型，以及一些执行相关的元信息（是否确定、是否允许副作用、能否并行）。`Tool` 就是这套契约在模型工具上的对应物，连「入参类型从签名自动推断」这个便利都一样。

差别在于**两套输入输出**。主核的 UDF 只有一组参数（调用方给的）；`Tool` 有两套——模型可见的 `parameters` 与状态注入的隐藏参数，结果也分两路（给模型看的字符串、写回状态的结构化值）。这是因为工具同时服务于「模型决策」与「宿主状态机」两个消费者，而 UDF 只服务于查询执行。

与本组 h02（工具调用执行）的关系最紧密：`Tool` 的三个行为字段（`inputs_from_state` / `outputs_to_state` / `outputs_to_string`）全部由 h02 消费。所以这一页定义契约，h02 实现它的语义。而 h02 的调度器需要 `_state_io_for_call` 去**重新推导**这一层的配置含义（哪些参数从状态来、哪些键会写）——两者必须保持同步，否则调度会漏依赖。

与 h01（智能体组件与状态）的连接：`State` 注解的参数在这一层被从 schema 里剔除（第 3 步过滤），在 h01/h02 里被无条件注入。所以「参数在 schema 里出现与否」这件事，正是「模型能不能看见它」的开关。

对 AI 工程最实用的一条是「状态注入的参数不要暴露给模型」。一个工具可能需要「当前用户 id」「仓库路径」这类上下文，而模型完全不该（也不能）提供它们。做法是在函数签名里声明这个参数、用 `inputs_from_state` 映射、然后在生成 schema 时把它过滤掉。若不过滤，模型会尝试编造这些值——而编造出来的值往往格式正确、内容错误，排查起来很费劲。

另一条是 `_remove_title_from_schema` 那个教训。给模型生成 schema 时删修剪冗余字段是常规操作，但「删字段」在递归结构里必须区分「这个键是结构关键字还是用户数据」。做得不对的两种后果：删得太少（冗余标题占 token），删得太多（用户名为 `title` 的属性消失，或默认值被改写）。判据是：**要删的键名在目标格式里是否也可能作为名字出现？** 是的话就必须维护「哪些关键字的值是映射/数据」的知识。

最后一条是「工具描述的显式覆盖」。`description if description is not None else docstring` 允许用空字符串清空——这与常见的 `description or docstring` 相比只差一个字符，但语义完全不同：后者让「故意不写描述」变得不可能。凡是「参数覆盖默认值」的地方，都该用 `is None` 判断而不是真值判断。
