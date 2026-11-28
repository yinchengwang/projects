---
project: haystack
feature: hooks
title: 钩子
page_type: feature
schema_version: 1
refs:
  - path: haystack/hooks/protocol.py
    symbols: [Hook]
  - path: haystack/hooks/from_function.py
    symbols: [hook, FunctionHook]
  - path: haystack/hooks/invocation.py
    symbols: [_run_hooks, _create_hook_span]
  - path: haystack/hooks/utils.py
    symbols: [warm_up_hooks, close_hooks]
---

# 钩子

## 一句话职责

给 `Agent` 的运行循环插入六个可干预点，而干预方式只有一种——**就地修改传入的 `State`**；因为钩子可能持有连接池这类资源，它还有独立的生命周期（预热 / 关闭），且同一个钩子可挂在多个点上也只会被预热与关闭一次。

## 机制是什么

四层：

**协议层**（`Hook`）——一个 `Protocol`，三个方法：`run(state)` 必须、`to_dict` / `from_dict` 用于序列化。docstring 把干预机制说得很绝对：「一个钩子**只通过就地修改 `State`** 来影响运行」。

**装饰器层**（`hook` + `FunctionHook`）——把普通函数包成满足协议的类，且负责序列化（函数引用要能被写下来再读回来）。

**调用层**（`_run_hooks` / `_run_hooks_async` / `_create_hook_span`）——按钩子点取列表、按顺序跑、每个钩子一个追踪 span。

**生命周期层**（`warm_up_hooks` / `close_hooks`，各带异步版）——按**对象身份去重**后预热与关闭。

六个钩子点是 `HookPoint` 这个 `Literal` 的六个取值：`before_run` / `before_llm` / `before_tool` / `after_tool` / `on_exit` / `after_run`。而 `VALID_HOOK_POINTS = get_args(HookPoint)` ——**从类型标注里反推出运行时元组**，所以「有哪些点」只有一个定义处，不会与校验逻辑不同步。

## 怎么实现的

**`Hook` 协议有意不声明三个方法，理由都写在了 docstring 里。** 协议只要求 `run`、`to_dict`、`from_dict`，而三处「故意不放进协议」的内容是：

1. **`run_async`**：「一个钩子可以额外定义 `async def run_async(self, state)` 来实现真正的异步行为；**不存在时 Agent 在异步运行中调用 `run`**。**它被故意留在这个协议之外，这样只支持同步的钩子不必实现它**」。

这句话解释了为什么不把它做成 `Protocol` 的成员：`Protocol` 的成员是结构性要求，放进去就等于「所有钩子都必须有 `run_async`」——而一个纯同步的钩子不该被迫写一个 `async def` 转发。

2. **`warm_up` / `warm_up_async` 与 `close` / `close_async`**：「一个钩子也可以实现可选的生命周期方法……Agent 从它自己的 `warm_up` / `warm_up_async` 与 `close` / `close_async` 里调用它们，这样钩子可以把打开客户端或读取凭据推迟到预热时，并在关闭时释放。**因为预热在每次 Agent 运行前都会跑，钩子应避免重复昂贵的初始化，例如在客户端已初始化时提前返回**」。

所以生命周期方法是鸭子类型的——靠 `hasattr` 探测（`warm_up_hooks` 里就是 `if hasattr(h, "warm_up")`）。

3. **`allowed_hook_points`**（在 h01 里被 `_validate_hooks` 使用）：一个可选的属性，声明这个钩子只在哪些点有效。

三处「不进协议」的共同原因是同一个：**`Protocol` 表达「必须有」，而这些能力都是「可以有」**。把它们放进去会让所有实现都背上空方法。

**`FunctionHook` 的四道校验与 `Tool` 的几乎逐字相同，只多一道。** 前三条：

```python
if function is None and async_function is None: raise ValueError(...)
if function is not None and inspect.iscoroutinefunction(function): raise ValueError(...)
if async_function is not None and not inspect.iscoroutinefunction(async_function): raise ValueError(...)
```

第四条是钩子独有的：

```python
for func in (function, async_function):
    if func is not None and not _takes_single_state_argument(func):
        raise ValueError(f"Hook function '{func.__name__}' must take a single parameter annotated with `State` (e.g. `def my_hook(state: State) -> None`).")
```

而 `_takes_single_state_argument` 的判定是：

```python
param_types = list(_resolve_parameter_types(function).values())
return len(param_types) == 1 and param_types[0] is State
```

**两个条件都是 `is` 级别的严格**：必须**恰好一个**参数，且它的注解**就是 `State`**（`is` 比较，不是子类判断）。

所以一个 `def my_hook(state: State, extra: int = 0)` 会被拒绝，一个 `def my_hook(state: Optional[State])` 也会被拒绝。这个严格度是合理的：钩子的调用方式是 `h.run(state)`，多一个参数就必然缺值；而 `Optional[State]` 在运行时拿到的永远是真实 State，写成 Optional 只会误导。

错误消息里给了示例签名（`def my_hook(state: State) -> None`），这是「错误消息要能自救」的又一例。

**`FunctionHook.run` 与 `run_async` 的不对称是有意的。**

```python
def run(self, state: State) -> None:
    if self.function is None:
        raise RuntimeError("This FunctionHook only has an `async_function` and cannot run in a synchronous Agent run. Use the Agent's async run methods, or provide a synchronous `function`.")
    self.function(state)

async def run_async(self, state: State) -> None:
    if self.async_function is not None:
        await self.async_function(state)
    else:
        self.function(state)  # type: ignore[misc]  # guaranteed non-None: at least one is always set
```

所以**同步路径不会回退到异步**（做不到——不能在一个同步函数里跑协程），而**异步路径会回退到同步**。这个方向与 h01 里 `Tool.invoke` / `invoke_async` 的关系完全一致：异步能从同步来，同步不能从异步来。

那句 `# type: ignore[misc]` 后面跟的注释「保证非 None：至少有一个总是被设置」是把不变量就地写明——构造期已经校验过，所以这里的 `self.function` 一定非空。

**`hook` 装饰器只做一件事：按是不是协程函数决定放进哪个槽。**

```python
if inspect.iscoroutinefunction(function):
    return FunctionHook(async_function=cast("Callable[[State], Awaitable[None]]", function))
return FunctionHook(function=cast("Callable[[State], None]", function))
```

注释说明了那两个 `cast` 的作用：「`iscoroutinefunction` 缩窄了可调用对象属于哪个槽；用 cast 满足类型化的槽」。

所以「一个函数只占一个槽」——这带来一个限制，docstring 也写明了：「要给单个钩子同时提供两条路径，直接用 `function` 与 `async_function` 构造一个 `FunctionHook`」。装饰器只能表达「一条路径」。

**`_run_hooks` 的顺序与 span 的父级处理值得逐行看。**

```python
hooks_to_run = hooks.get(hook_point, [])
if not hooks_to_run:
    return
parent_span = tracing.tracer.current_span()
for h in hooks_to_run:
    with _create_hook_span(hook=h, hook_point=hook_point, parent_span=parent_span):
        h.run(state)
```

三点：

1. **`if not hooks_to_run: return` 的早退很重要** ——Agent 每步都会调六次 `_run_hooks`，而多数时候某些点没有钩子。早退让「没配钩子」的路径零开销（连 `current_span()` 都不取）。

2. **`parent_span` 在循环外取一次** ——所以同一个点上的多个钩子 span 是**兄弟**而不是链式。这与 h02 里 `_create_tool_span` 的处理完全同源：并发或同级的多个操作，父级必须显式传。

3. **钩子按列表顺序串行执行**，且**前一个的修改对后一个可见**（因为传的是同一个 `state` 对象）。所以钩子的顺序有语义——`hooks={"before_llm": [a, b]}` 里 `a` 改的东西 `b` 能看到。

**异步版换用一个统一的分派器。** `_run_hooks_async` 的循环体是：

```python
await _execute_component_async(h, state=state)
```

而不是自己判断有没有 `run_async`。注释说明了这个选择：「优先 `run_async`，把只有同步 `run` 的钩子卸载到别处」。所以「有没有异步版本」的判断逻辑被集中在一个工具函数里，钩子层不重复实现。

**`_create_hook_span` 的 span 是「内容无关」的，只有标签。** docstring 一句话：「为一个钩子调用创建一个**无内容**的追踪 span」。

三个标签：

```python
"haystack.agent.hook.point": hook_point,
"haystack.agent.hook.name": _hook_name(hook=hook),
"haystack.agent.hook.type": generate_qualified_class_name(cls=type(hook)),
```

而 span 名是 `"haystack.agent.hook"`。**没有 set_content_tag 调用** ——所以钩子看到的输入输出不进追踪。理由是显然的：钩子拿到的是完整的 State（含全部对话），把它写进追踪会泄露内容且体积巨大。

**`_hook_name` 是这一层最精细的一个小函数，它要处理三种「没有正常名字」的可调用对象。**

```python
if isinstance(hook, FunctionHook):
    function = hook.function or hook.async_function
    if function is not None:
        try:
            try:
                return serialize_callable(callable_handle=function)
            except SerializationError:
                return f"{function.__module__}.{function.__qualname__}"
        except Exception:
            pass
return type(hook).__name__
```

三层降级：

1. **`serialize_callable`** ——能序列化就用它的结果（通常是 `模块.限定名`）。
2. **`SerializationError`** → 用 `f"{function.__module__}.{function.__qualname__}"`。注释解释了为什么这两种失败要分开处理：「嵌套函数与 lambda 不被序列化支持，但它们的限定名仍然有用」。
3. **其他任何异常** → `pass`，落到 `type(hook).__name__`。注释：「`functools.partial` 与可调用对象实例可能不暴露 `__module__`、`__qualname__` 或 `__name__`」。

而最外层对 `FunctionHook` 的特别处理也带注释：「`FunctionHook` 是一个通用包装器，所以用它的**被包装可调用对象**来标识它」——否则所有用 `@hook` 装饰的钩子在追踪里都叫 `FunctionHook`，完全无法区分。

这个三层降级是「在有损的环境里尽力给出有用信息」的范例：能精确就精确，不能就退到限定名，再不能就退到类名。每一层都比「什么都给不出」好。

**`warm_up_hooks` 与 `close_hooks` 的核心是 `_unique_hooks` 的身份去重。**

```python
def _unique_hooks(hooks: dict[HookPoint, list[Hook]]) -> list[Hook]:
    unique: list[Hook] = []
    seen: set[int] = set()
    for hook_list in hooks.values():
        for h in hook_list:
            if id(h) not in seen:
                seen.add(id(h))
                unique.append(h)
    return unique
```

docstring 说明了必要性：「一个钩子可能注册在多个钩子点下；**按身份去重确保生命周期方法（预热 / 关闭）每个钩子对象只跑一次**」。

这里用 `id(h)` 而不是 `h` 作为集合元素，是一个刻意的选择：钩子是用户对象，未必可哈希（若实现了 `__eq__` 而没实现 `__hash__`，或者实现了允许变异的哈希）。用 `id` 回避了这个问题，代价是「两个逻辑相等但不同实例的钩子会被各预热一次」——而这恰恰是想要的（它们确实是两个对象，各自持有资源）。

**而 `_unique_hooks` 的一个副作用值得注意**：它遍历 `hooks.values()`，而 dict 的插入顺序就是用户传 `hooks={...}` 的顺序。所以预热顺序是「按钩子点首次出现、点内按列表顺序」。若某个钩子的 `warm_up` 依赖另一个先预热，这个顺序就不受控——而框架没有提供声明依赖的方式。

**四个生命周期函数的结构完全同构，异步版多一层回退。**

```python
def warm_up_hooks(hooks):
    for h in _unique_hooks(hooks):
        if hasattr(h, "warm_up"):
            h.warm_up()

async def warm_up_hooks_async(hooks):
    for h in _unique_hooks(hooks):
        warm_up_async = getattr(h, "warm_up_async", None)
        if warm_up_async is not None:
            await warm_up_async()
        elif hasattr(h, "warm_up"):
            h.warm_up()
```

同步版只用 `hasattr`；异步版用 `getattr(..., None)` 取方法，有 `warm_up_async` 就 await，否则回退到同步 `warm_up`。

注意异步版回退时**直接调同步 `warm_up`**（不是 `to_thread`）——所以一个慢的同步预热会阻塞事件循环。这是一个已知的简化：预热通常只跑一次，阻塞可接受。

`close_hooks` 与 `close_hooks_async` 是对称的镜像。

**为什么要专门做预热/关闭，答案在 `Hook` 协议的 docstring 里，但更直接的理由可以从「去重」反推**：如果钩子只被调用、没有独立生命周期，那么「打开客户端」就只能放在 `__init__` 里或第一次 `run` 里。前者让「构造配置对象」产生副作用（违反直觉）；后者让「第一次调用变慢」且需要自己写幂等守卫。给出显式的生命周期方法后，「构造」「预热」「调用」「关闭」四个阶段各归其位。

**序列化走的是组件的那套工具，而注释解释了为什么可以这样用。**

```python
# Hooks are (de)serialized with `component_to_dict` / `deserialize_component_inplace` even though they aren't
# Components. Despite the name, those helpers aren't component-specific: they just produce/consume the standard
# `{"type", "init_parameters"}` dict, and deserialization enforces the import allowlist (so only trusted modules are
# loaded).
```

所以 `_serialize_hooks_dictionary` 里每个钩子都过 `component_to_dict(obj=h, name="hook")`，而反序列化包一层 `{"hook": serialized_hook}` 再 `deserialize_component_inplace(wrapper, key="hook")`。

那句注释里的重点在后半句：**反序列化会强制导入允许清单**。所以「钩子能被反序列化」这件事的安全性由那套机制保证——一个流水线 YAML 里写了 `{type: "evil.Module"}` 不会被实例化。这是把「序列化」与「安全检查」绑在同一处的好处：只要走这条路径，检查就不会漏。

`FunctionHook.to_dict` 存的是 `serialize_callable(self.function)` ——**函数引用而不是函数体**。所以一个用 `@hook` 装饰的钩子被序列化成「去哪个模块找哪个函数」，反序列化时重新 import。这让配置文件简短，但要求那个函数在反序列化环境里可导入。

## 关键取舍

**源码事实：** `Hook` 协议只要求 `run` / `to_dict` / `from_dict`；`run_async`、`warm_up*`、`close*`、`allowed_hook_points` 全部**故意不放进协议**，让纯同步钩子不必实现空方法。

**源码事实：** `VALID_HOOK_POINTS = get_args(HookPoint)` ——运行时元组从类型标注反推，定义只有一处。

**源码事实：** `FunctionHook` 要求函数**恰好一个**参数且注解 `is State`（`Optional[State]` 与带额外默认参数的签名都被拒绝）。

**源码事实：** `FunctionHook.run` 不会回退到异步（做不到），而 `run_async` 会回退到同步——方向与 `Tool` 一致。

**源码事实：** `hook` 装饰器把协程函数放进 `async_function`、普通函数放进 `function`，一个函数只占一个槽；要两条路径必须直接构造 `FunctionHook`。

**源码事实：** `_run_hooks` 在循环**外**取一次 `parent_span`，所以同点多个钩子的 span 是兄弟而非链式；没钩子时早退不取 span。

**源码事实：** 钩子按列表顺序串行执行，传同一个 `state`，所以前一个的修改对后一个可见。

**源码事实：** `_create_hook_span` 只设三个标签，**不设内容标签**，因为 State 含完整对话。

**源码事实：** `_hook_name` 三层降级：可序列化引用 → `模块.限定名` → 类名；对 `FunctionHook` 特判取被包装的函数名，否则全部同名。

**源码事实：** `_unique_hooks` 用 `id(h)` 去重，所以同一对象挂在多个点只预热/关闭一次，而两个相等但不同的实例各算一次。

**源码事实：** 异步预热在缺少 `warm_up_async` 时直接调同步 `warm_up`（不做 `to_thread`），可能阻塞事件循环。

**源码事实：** 钩子序列化复用 `component_to_dict` / `deserialize_component_inplace`，后者的导入允许清单顺带提供了安全检查。

**架构推论：** 「干预只能靠改 State」这个约束看似限制，实际是让钩子系统保持简单的关键。若钩子能返回值、能抛异常改控制流、能直接操作 Agent 的字段，那它们的组合行为就无法预期。收敛到「改一个共享数据结构」之后，钩子之间只有**顺序**这一种关系，而顺序由用户传的列表决定——完全可推理。

**架构推论：** 「能力不进协议」这个选择，是 Python `Protocol` 的正确用法。`Protocol` 的语义是「结构上必须具备」，所以它只该放**所有实现都必须有的**成员。把可选能力塞进去，实际是在用类型系统表达「这个类可能没有某个方法」，而 `Protocol` 无法表达「可能没有」。正确做法就是像这样：协议只写必需项，可选项靠 `hasattr` 探测并写进文档。

**架构推论：** `_hook_name` 那三层降级揭示了「可观测性命名」的普遍困难：`serialize_callable` 能给出稳定的模块路径，但它对 lambda 与嵌套函数不成立；`__qualname__` 能覆盖那些，但对 `partial` 与可调用对象实例不成立；最后只能退到类名——而此时「所有用同一个装饰器包装的钩子都同名」这件事就损失了区分度。任何「给运行时对象生成稳定标识」的机制都会撞上同样的墙，因为 Python 的对象模型里没有统一的「名字」概念。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/hooks/protocol.py` | `Hook` | 三方法协议，只靠改 State 影响运行 |
| `haystack/hooks/protocol.py` | `HookPoint` | 六个钩子点的 `Literal` 类型 |
| `haystack/hooks/protocol.py` | `VALID_HOOK_POINTS` | 从 `Literal` 反推出的运行时元组 |
| `haystack/hooks/from_function.py` | `hook` | 装饰器，按是否协程决定放哪个槽 |
| `haystack/hooks/from_function.py` | `FunctionHook` | 可序列化的钩子包装器 |
| `haystack/hooks/from_function.py` | `FunctionHook.run` | 无同步函数时报错，不回退 |
| `haystack/hooks/from_function.py` | `FunctionHook.run_async` | 有异步用异步，否则回退同步 |
| `haystack/hooks/from_function.py` | `_takes_single_state_argument` | 「恰好一个参数且注解是 `State`」的严格判定 |
| `haystack/hooks/invocation.py` | `_run_hooks` | 按点取列表，串行跑，span 父级提前取 |
| `haystack/hooks/invocation.py` | `_run_hooks_async` | 统一走 `_execute_component_async` 分派 |
| `haystack/hooks/invocation.py` | `_create_hook_span` | 无内容 span，只带三个标签 |
| `haystack/hooks/invocation.py` | `_hook_name` | 三层降级的可观测命名 |
| `haystack/hooks/utils.py` | `_unique_hooks` | 按 `id` 去重，保住生命周期只跑一次 |
| `haystack/hooks/utils.py` | `warm_up_hooks` | `hasattr` 探测后预热 |
| `haystack/hooks/utils.py` | `close_hooks` | 对称的关闭 |
| `haystack/hooks/utils.py` | `_serialize_hooks_dictionary` | 逐钩子走组件序列化（含导入允许清单） |
| `haystack/hooks/utils.py` | `_is_system_reminder` | 靠 meta 键识别「由钩子插入的用户消息」 |

## 参考

- [haystack 源码：hooks/protocol.py](https://github.com/deepset-ai/haystack/blob/main/haystack/hooks/protocol.py)
- [haystack 源码：hooks/utils.py](https://github.com/deepset-ai/haystack/blob/main/haystack/hooks/utils.py)
- [haystack 文档：Agent 钩子](https://docs.haystack.deepset.ai/docs/hooks)

## 与主核 / AI 的关系

主核里最接近的是「扩展点 / 回调钩子」：例如存储引擎在 WAL 写入前后、页刷盘前后提供的回调，让扩展能插入自定义逻辑。那一层的回调同样只能通过操作上下文（而不是返回值）影响主流程。

差别在于**主核的回调通常是编译期注册、受信任的扩展**；这里的钩子是**用户配置里写进来的对象**，可能来自反序列化的 YAML（这正是为什么要复用带导入允许清单的序列化工具）。所以同一类机制在两种信任前提下需要不同的安全措施。

与本组 h01（智能体组件与状态）的连接最紧：`Agent` 里六处 `_run_hooks` 调用就是这一页的消费者，而「钩子能改 State」这个能力之所以有意义，完全依赖 h01 里那些「重读点」——例如 `_pending_tool_call_messages_from_state` 在 `BEFORE_TOOL` 之后重读状态，才让钩子的修改真的生效。**扩展点与重读点必须成对存在**，只有扩展点而没有重读，钩子改了也没人看。

与 h02（工具调用执行）的对照：那边每个工具调用一个 span 且**有内容标签**（输入参数、原始输出、给模型看的字符串），这边每个钩子一个 span 但**没有内容标签**。差别在于「工具的参数是必要诊断信息，而钩子看到的是全部对话」。

对 AI 工程最实用的一条是「扩展点的能力要收敛到改一个共享数据结构」。一旦允许扩展点返回值、抛异常、或直接调度，它们的组合行为就变成不可分析的。收敛到 State 之后，「多个钩子按顺序执行、前一个的修改对后一个可见」成为一个简单且可推理的语义。

另一条是「生命周期去重要按身份而不是按相等」。钩子可能注册在多个点上，若不按 `id` 去重，`warm_up` 会跑多次——而一个开了两次连接池的钩子会泄漏一半资源。反过来，用 `__eq__` 去重会把「两个配置相同的钩子实例」合成一个，而它们各自持有资源、必须各自预热。判据是：**这个对象的资源是绑在实例上的吗？** 是的话只能按身份去重。

最后一条是「可观测命名要有降级链」。给运行时对象起名字这件事，在 Python 里没有一种手段能覆盖所有情况（lambda、嵌套函数、`partial`、可调用对象实例）。写三层降级（精确引用 → 限定名 → 类名）能让绝大多数情况都有可用标识，而最后一层虽然粗糙，至少不会让追踪里出现空值。
