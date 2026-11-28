---
project: haystack
feature: component-contract
title: 组件契约与元类校验
page_type: feature
schema_version: 1
refs:
  - path: haystack/core/component/component.py
    symbols: [Component, ComponentMeta, _Component]
  - path: haystack/core/component/component.py
    symbols: [_compare_run_methods_signatures, _hook_component_init]
---

# 组件契约与元类校验

## 一句话职责

用「一个装饰器 + 一个元类」把组件的契约从文档变成运行期强制：`@component` 把用户类**重建**成带 `ComponentMeta` 元类的新类，元类在**实例化时**从 `run` / `run_async` 的签名反推出输入 socket、从 `@component.output_types` 的缓存反推出输出 socket，并把「`run` 与 `run_async` 签名必须逐参数一致」「`run_async` 必须是协程函数」等规则变成 `ComponentError`。

## 机制是什么

这份契约最反常的地方是：**它不写在基类里，而写在元类的 `__call__` 里。**

通常的写法是让组件继承一个 `BaseComponent`，在 `__init__` 或 `__init_subclass__` 里做校验。haystack 没这么做，原因很实在——**它不想让用户类带上任何基类**（组件是普通类，不继承任何东西，于是可以自由地多继承、可以包住任意第三方对象）。于是它只能在「类被创建」和「实例被创建」两个时刻插手，而干预这两个时刻的唯一手段就是装饰器（改类）+ 元类（改实例化）。

完整链路有四步，每一步解决一个具体问题：

| 时刻 | 手段 | 做什么 |
|---|---|---|
| 类创建 | `_Component._component` | 检查有 `run`；用 `new_class` 重建类并换成 `ComponentMeta`；登记到 registry |
| 方法定义 | `@component.output_types` | 把输出规格暂存成方法属性 `_output_types_cache`（此时还没有类） |
| 实例创建 | `ComponentMeta.__call__` | 调 pre-init 钩子、调真正的 `__init__`、建输入/输出 socket、设 `__haystack_added_to_pipeline__ = None` |
| 实例创建中 | `_parse_and_set_input_sockets` | 从 `run` 签名反推输入 socket，并校验 `run_async` 与之完全一致 |

这个链路之所以这么绕，是因为**socket 必须在实例上而不是类上**（同一个组件的两个实例可以有不同的 socket，因为 `set_input_types` 是在 `__init__` 里调的），而**输出规格必须在类创建时就已收集**（装饰器先于元类运行）。

## 怎么实现的

**`Component` 不是基类，是一个 `Protocol`。**

```
@runtime_checkable
class Component(Protocol):
    def run(self, *args: Any, **kwargs: Any) -> Mapping[str, Any]: ...
```

它只用于类型检查，且注释坦白了它的宽松程度：「`run` 的签名和返回值都不会被检查」——写成 `run(self, param: str) -> dict[str, Any]` 与 `run(self, **kwargs)` 都算符合协议。真正的一致性由元类在实例化时保证。

类里那段注释解释了为什么不用 `run: Callable[..., dict[str, Any]]`：那样协议会要求 `run` 是**可赋值的属性**，而实际实现是只读方法，于是 mypy 会报 「Protocol member Component.run expected settable variable, got read-only attribute」。用一个 `def` 声明方法就把「只读」这个性质表达对了。

`runtime_checkable` 让 `isinstance(MyComponent, Component)` 可调用——这也是为什么它能当 `add_component` 的参数类型而不会在运行期挡住鸭子类型对象。

**`@component` 装饰器做的事不是「标记」，而是「重建类」。**

```
def copy_class_namespace(namespace):
    for key, val in dict(cls.__dict__).items():
        if key in ("__dict__", "__weakref__"):
            continue
        namespace[key] = val

new_cls = new_class(cls.__name__, cls.__bases__, {"metaclass": ComponentMeta}, copy_class_namespace)
```

**`typing.new_class` 按给定元类造一个新类，`exec_body` 回调负责把原类的命名空间搬过去。** 两个细节：

1. **跳过 `__dict__` 与 `__weakref__`** ——注释说明它们是「类绑定的」，必须让 Python 自己重建。若不跳过，新类会持有一个指向旧类的描述符，行为不可预测。
2. **`cls.__bases__` 原样传递** ——用户的继承关系被完整保留，只是元类换了。于是 `class MyThing(SomeMixin): ...` 加上 `@component` 仍然是个 `SomeMixin`。

装饰器里唯一的前置校验是：

```
if not hasattr(cls, "run"):
    raise ComponentError(f"{cls.__name__} must have a 'run()' method. See the docs for more information.")
```

**「尽早失败」在这里的含义是「类定义时就报错」**，而不是等到实例化或加进管道。这一点很重要：一个漏写 `run` 的组件会在 `import` 阶段就炸，而不是在 CI 跑管道时。

装饰器同时把新类登记进 registry：

```
class_path = f"{new_cls.__module__}.{new_cls.__name__}"
if class_path in self.registry:
    logger.debug("Component {component} is already registered. Previous imported from '{module_name}', new imported from '{new_module_name}'", ...)
self.registry[class_path] = new_cls
```

**登记键是 `模块名.类名` 字符串**——这正是反序列化时用来定位类的键。重复登记只打 debug 日志、不报错，注释说明理由是「notebook 里重跑 cell」这种常见情形。这是一个把「常见的开发期不便」放在「严格性」之前的选择。

最后覆盖 `__repr__`：

```
new_cls.__repr__ = _component_repr
```

`_component_repr` 打印对象地址、所属管道里的组件名、以及输入/输出 socket 的完整表述。于是 `print(component)` 就能看到这个组件的全部接线面——一个**为调试而设的默认实现**。

**元类的 `__call__` 是实例生命的入口，也是全部校验的落点。**

```
def __call__(cls, *args, **kwargs):
    pre_init_hook = _COMPONENT_PRE_INIT_HOOK.get()
    if pre_init_hook is None or pre_init_hook.in_progress:
        instance = super().__call__(*args, **kwargs)
    else:
        try:
            pre_init_hook.in_progress = True
            named_positional_args = ComponentMeta._positional_to_kwargs(cls, args)
            assert set(named_positional_args.keys()).intersection(kwargs.keys()) == set(), ("positional and keyword arguments overlap")
            kwargs.update(named_positional_args)
            pre_init_hook.callback(cls, kwargs)
            instance = super().__call__(**kwargs)
        finally:
            pre_init_hook.in_progress = False

    has_async_run = hasattr(instance, "run_async")
    if has_async_run and not inspect.iscoroutinefunction(instance.run_async):
        raise ComponentError(f"Method 'run_async' of component '{cls.__name__}' must be a coroutine")
    instance.__haystack_supports_async__ = has_async_run

    ComponentMeta._parse_and_set_input_sockets(cls, instance)
    ComponentMeta._parse_and_set_output_sockets(instance)
    instance.__haystack_added_to_pipeline__ = None
    return instance
```

四段各自的意图：

**第一段：pre-init 钩子。** 它存在的用途是「在构造器运行之前改写 init 参数」——典型场景是管道加载时把配置注入组件、或者测试里替换依赖。三个设计点：

- **钩子存在 `ContextVar` 里**（`_COMPONENT_PRE_INIT_HOOK`），于是它随上下文传播、在线程/异步任务间不串味。`_hook_component_init` 是一个 `contextmanager`，进入时 set、退出时 reset，用 `try/finally` 保证还原。
- **`in_progress` 标志防递归**：组件构造器里可能再构造另一个组件，若无此标志，内层组件的构造会再次触发同一个钩子，而钩子可能假定自己只被调一次（例如改写参数后期待它生效一次）。注释把理由写在 `PreInitHookPayload` 的字段说明里。
- **位置参数先转成关键字参数**：`_positional_to_kwargs` 用 `inspect.signature(cls.__init__)` 把 `args` 映射到参数名，因为钩子的回调签名是「类 + 关键字参数字典」，它只能就地改字典。转换时若遇到 `*args`（`VAR_POSITIONAL`）直接抛 `ComponentError`——**因为变长位置参数无法映射回名字**，这时「就地改写」这件事根本无从谈起。

那个 `assert ... == set(), "positional and keyword arguments overlap"` 在 `-O` 下会被去掉，但正常运行时它抓的是「调用方同时用位置和关键字传了同一个参数」这种 Python 本身就会报错的情形——所以它更像是一条防御性断言而非真正的校验。

**第二段：`run_async` 必须是协程函数。**

```
if has_async_run and not inspect.iscoroutinefunction(instance.run_async):
    raise ComponentError(f"Method 'run_async' of component '{cls.__name__}' must be a coroutine")
```

为什么必须检查？因为管道会 `await component.run_async(...)`。若 `run_async` 是普通函数，`await` 一个非 awaitable 会抛 `TypeError`——而那个错误发生在**运行期、在管道调度循环里**，堆栈指向管道而非组件。在实例化时检查，错误就落到了写组件的人头上。

同时设 `instance.__haystack_supports_async__` 这个标记，让管道不必反复 `hasattr`。

**第三段与第四段：建 socket、置管道归属标记。**

`__haystack_added_to_pipeline__ = None` 的注释说明用途：「一个组件不能同时属于多个管道」，所以加进管道时检查这个字段。**它是运行期状态被塞进组件实例**——组件不是纯函数对象，它知道自己被谁持有。

**输入 socket 的推导是「从签名反推 + 与显式声明对账」。**

```
def inner(method, sockets):
    run_signature = inspect.signature(method)
    param_types = _resolve_parameter_types(method)

    for param_name, param_info in run_signature.parameters.items():
        if param_name == "self" or param_info.kind in (Parameter.VAR_POSITIONAL, Parameter.VAR_KEYWORD):
            continue
        socket_kwargs = {"name": param_name, "type": param_types[param_name]}
        if param_info.default != Parameter.empty:
            socket_kwargs["default_value"] = param_info.default
        new_socket = InputSocket(**socket_kwargs)

        existing_socket = sockets.get(param_name)
        if existing_socket is not None and existing_socket != new_socket:
            raise ComponentError("set_input_types()/set_input_type() cannot override the parameters of the 'run' method")
        sockets[param_name] = new_socket
    return run_signature
```

四点：

1. **跳过 `self` 与 `*args` / `**kwargs`** ——变长参数没有确定的名字，无法成为 socket。这也解释了 `set_input_types` 为什么要求 `run` 必须有 `**kwargs`：**只有存在 `**kwargs` 时，才可能在签名之外声明额外 socket**。
2. **类型用 `_resolve_parameter_types(method)` 解析**，而不是直接读 `param_info.annotation`。因为写了 `from __future__ import annotations` 的模块里所有注解都是**字符串**，直接读会得到 `"Document"` 这样的字符串而不是 `Document` 类——socket 类型就成了无意义的字符串。`_resolve_parameter_types` 只在检测到字符串注解时才调 `get_type_hints`，并在失败时（`TypeError` / `NameError`）退回未解析的注解。
3. **默认值决定必填性**：有 `default` 就写进 `default_value`，没有就是必填。**必填性不是显式声明的，而是从 Python 的默认参数语法推出来的**——这是这套契约里最自然的映射。
4. **显式声明与签名冲突时报错**：若 `set_input_types` 已经设了同名 socket 且与签名推出的不等，抛 `ComponentError`。注意判据是**不相等**（`!=`）而非「存在即冲突」——所以「显式声明的类型与签名一致」是允许的，只是冗余。

**`run` 与 `run_async` 的一致性检查是双重比对。**

```
async_run = getattr(component_cls, "run_async", None)
if async_run is not None:
    run_sockets = Sockets(instance, {}, InputSocket)
    async_run_sockets = Sockets(instance, {}, InputSocket)
    run_sig = inner(getattr(component_cls, "run"), run_sockets)
    async_run_sig = inner(async_run, async_run_sockets)

    if async_run_sockets != run_sockets or run_sig != async_run_sig:
        sig_diff = _compare_run_methods_signatures(run_sig, async_run_sig)
        raise ComponentError(f"Parameters of 'run' and 'run_async' methods must be the same.\nDifferences found:\n{sig_diff}")
```

**关键细节：这里重新建了两个全新的空 `Sockets` 来比对**，而不是复用上面已经填好的 `instance.__haystack_input__`。注释写明理由：「不能用上面的 sockets，因为它们可能含有 `set_input_types()` 设的值」。也就是说这里比的是**纯签名**，不含任何显式声明——因为显式声明是实例级的，两边共享，比它没有意义。

两个比对条件各有含义：`run_sockets != run_sockets` 比 socket 集合（名字、类型、默认值），`run_sig != async_run_sig` 比完整签名（含返回注解、参数种类）。**为什么两个都要？** 因为 socket 只承载了「名字/类型/默认值」三项，而签名还包含「参数顺序」「参数种类」「返回注解」——两个方法的参数可以 socket 完全相同但顺序不同，那是必须禁止的（管道按名字传值，但 `**kwargs` 组件按位置取值的实现可能错位）。

**`_compare_run_methods_signatures` 是专门为这条错误消息而存在的函数。**

```
if len(run_params) != len(async_params):
    differences.append(f"Different number of parameters: run has {len(run_params)}, run_async has {len(async_params)}")

for (run_name, run_param), (async_name, async_param) in zip(run_params, async_params, strict=False):
    if run_name != async_name: differences.append(f"Parameter name mismatch: {run_name} vs {async_name}")
    if run_param.annotation != async_param.annotation: differences.append(f"Parameter '{run_name}' type mismatch: ...")
    if run_param.default != async_param.default: differences.append(f"Parameter '{run_name}' default value mismatch: ...")
    if run_param.kind != async_param.kind: differences.append(f"Parameter '{run_name}' kind (POSITIONAL, KEYWORD, etc.) mismatch: ...")
return "\n".join(differences)
```

**它把「哪些参数不一样、差在名字还是类型还是默认值还是种类」四项逐一列出，拼成多行消息。** 这个函数的存在本身就是一条经验：`inspect.Signature` 的 `!=` 只会告诉你「不相等」，而用户需要知道「哪一项不等」。在一个「两个方法必须手写对齐」的场景里（`run` 与 `run_async` 是两份代码），**诊断质量直接决定这个约束是「麻烦」还是「不可忍受」**。

注意它用 `zip(..., strict=False)` 而不是 `strict=True`：因为长度不等已经单独报了一条，再让 `zip` 抛 `ValueError` 就没必要了。这是「已经有一个更具体的错误」时主动放宽后续检查的写法。

**输出 socket 走完全不同的路径：它来自缓存的装饰器结果。**

```
run_output_types = getattr(instance.run, "_output_types_cache", {})
async_run_output_types = getattr(instance.run_async, "_output_types_cache", {}) if has_async_run else {}

if has_async_run and run_output_types != async_run_output_types:
    raise ComponentError("Output type specifications of 'run' and 'run_async' methods must be the same")
instance.__haystack_output__ = Sockets(instance, deepcopy(output_types_cache), OutputSocket)
```

三处：

1. **`_output_types_cache` 是方法属性**，由 `@component.output_types` 在**方法定义时**写入（那时类还不存在，所以只能挂在方法上）。装饰器只允许用在 `run` / `run_async` 上（`if method_name not in ("run", "run_async")` 就抛错），否则这份缓存永远不会被读，等于静默失效。
2. **两个方法的输出规格必须完全相同**，否则抛错——与输入侧的一致。
3. **`deepcopy` 的理由写在注释里**：「把缓存内容的属主从类方法转移到实际实例，使同一个类的不同实例不共享这份数据」。若不 deepcopy，`__haystack_output__` 里的 `OutputSocket` 对象会被所有实例共享，而管道在接线时可能会往 socket 上挂运行期状态（例如连接信息）——那样一个实例的接线会串到另一个实例上。

**这条路径还有一个「优先权」细节**：整段逻辑被 `if not hasattr(instance, "__haystack_output__")` 包着——若构造函数里调过 `set_output_types()`，那个属性已存在，这里的推导**整体跳过**。同理 `set_output_types` 也会检查是否已有装饰器（`has_run_decorator or has_run_async_decorator` 就抛错）。于是两条声明方式**互斥且都有明确的冲突报错**，不会有「谁覆盖谁」的歧义。

**`ComponentMeta._positional_to_kwargs` 里的断言透露了 pre-init 钩子的边界。**

```
assert info.kind in (inspect.Parameter.POSITIONAL_OR_KEYWORD, inspect.Parameter.POSITIONAL_ONLY)
```

位置参数只能是这两种，否则（`*args` / `**kwargs` / 仅关键字）就无法映射。但真正会触发的只有 `VAR_POSITIONAL`，因为 `zip(args, init_params.items())` 是按顺序配对，遇到 `**kwargs` 这种「不是在 args 里出现的参数」也只是配对错位而非报错。**这条断言是最后一道防线，而不是主要检查**——主要检查是前面那个 `VAR_POSITIONAL` 的显式 `raise`。

## 关键取舍

**源码事实：** `@component` 不做标记，而是用 `typing.new_class` **重建**一个带 `ComponentMeta` 元类的新类，命名空间靠 `exec_body` 回调逐项搬运并跳过 `__dict__` / `__weakref__`；基类原样保留。

**源码事实：** `Component` 是 `Protocol` 而非基类（组件不必继承任何东西），且 `run` 用 `def` 声明而非 `Callable` 属性——注释说明后者会让 mypy 报「只读属性不满足可赋值成员」。

**源码事实：** 唯一的类创建期校验是「有 `run` 方法」；其余校验都推迟到实例化，因为 socket 是实例级状态（`set_input_types` 在 `__init__` 里调）。

**源码事实：** 输入 socket 从 `run` 签名反推：跳过 `self` 与变长参数、用 `_resolve_parameter_types` 解析字符串注解（应对 `from __future__ import annotations`）、有默认值即非必填；与显式声明**不相等**时才报错。

**源码事实：** `run` 与 `run_async` 的一致性用**两个全新空 `Sockets`** 比对（注释说明不能用实例上已填的那份，因它可能含 `set_input_types` 的值），且同时比对 socket 集合与完整签名两样。

**源码事实：** `_compare_run_methods_signatures` 只为生成可读错误消息而存在，逐项列出「参数个数 / 名字 / 类型 / 默认值 / 种类」的差异；内部 `zip(..., strict=False)` 是刻意的，因为长度不等已单独报过。

**源码事实：** 输出 socket 来自方法属性 `_output_types_cache`（由装饰器在类创建前挂上），构造实例时 `deepcopy` 以免同类多实例共享；若构造函数已调 `set_output_types` 则整段推导跳过，两条路径互斥且各自有冲突报错。

**源码事实：** `run_async` 存在但不是 `iscoroutinefunction` 时在实例化期抛 `ComponentError`；同时设 `__haystack_supports_async__` 供管道免查。

**源码事实：** pre-init 钩子存在 `ContextVar` 里、带 `in_progress` 标志防递归调用、用 `try/finally` 重置；位置参数先经 `_positional_to_kwargs` 转成关键字才能交给回调，遇 `*args` 直接 `ComponentError`。

**源码事实：** 组件实例被塞进两个运行期字段：`__haystack_supports_async__`（能力标记）与 `__haystack_added_to_pipeline__`（归属标记，用于「一个组件不能同时属于多个管道」的检查）。

**架构推论：** 「用元类而不是基类强制契约」这个选择的收益是**组件可以是任何类的子类**（不占用唯一的继承位），代价是**必须重建类**（`new_class`），而重建会带来 `__dict__`/`__weakref__` 的处理、类型检查器的辨认、registry 的登记这一整套附带成本。这是一个「库作者承担复杂度、用户获得自由度」的分配。

**架构推论：** 把校验放在实例化期（而非类创建期）是被「socket 是实例级状态」这条需求逼出来的。于是 `@component` 上的类定义只保证「有 `run`」，真正的契约检查要等你写 `MyComponent()` 才发生——这让一个「签名与 `run_async` 不一致」的组件能通过 `import`，只在实例化时炸。这是灵活性与早失败之间的必然取舍。

**架构推论：** `_compare_run_methods_signatures` 这种「只为错误消息而存在的函数」值得单独记。规则一旦要求两份手写代码保持一致（`run` / `run_async`），诊断质量就是这个规则能否被接受的决定因素——只说「不一致」的检查，在实际项目里会被人绕开。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/core/component/component.py` | `Component` | `runtime_checkable` 的 `Protocol`，只约束存在 `run`，不约束签名 |
| `haystack/core/component/component.py` | `ComponentMeta` | 元类：`__call__` 在实例化时建 socket、校验 `run_async`、设能力与归属标记 |
| `haystack/core/component/component.py` | `_Component` | 装饰器运行时：`_component` 重建类并登记 registry，`set_input_types` / `set_output_types` / `output_types` 声明接口 |
| `haystack/core/component/component.py` | `_compare_run_methods_signatures` | 逐项列出 `run` 与 `run_async` 的签名差异，仅供错误消息使用 |
| `haystack/core/component/component.py` | `_hook_component_init` | pre-init 钩子的 `contextmanager`，用 `ContextVar` + `in_progress` 防递归 |
| `haystack/core/component/component.py` | `_COMPONENT_PRE_INIT_HOOK` / `PreInitHookPayload` | 钩子的上下文载体与「执行中」标志 |
| `haystack/core/component/component.py` | `_component_repr` | 覆盖 `__repr__`，打印实例、管道内名字与两侧 socket |
| `haystack/core/component/component.py` | `_component_run_has_kwargs` | `set_input_types` 的前置条件：`run` 必须有 `**kwargs` |
| `haystack/core/type_utils.py` | `_resolve_parameter_types` | 解析被推迟求值的字符串注解，失败时退回原始注解 |

## 参考

- [haystack 源码：core/component/component.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/component/component.py)
- [haystack 文档：组件](https://docs.haystack.deepset.ai/docs/components)
- [Python 文档：`typing.new_class`](https://docs.python.org/3/library/types.html#types.new_class)
- [Python 文档：`typing.Protocol`](https://docs.python.org/3/library/typing.html#typing.Protocol)

## 与主核 / AI 的关系

「用元类在类创建/实例化期强制契约」在主核里对应的是**插件注册与 ABI 校验**：外部扩展必须满足一套接口，而框架要在加载期而非运行期发现不合规。四条经验。

**第一，元类方案换来的是「不占用继承位」。** haystack 的组件可以是任意类的子类，因为契约不在基类里。主核若允许扩展实现自己的算子/存储引擎，同样值得考虑「不强制单一基类」——代价是必须有个地方集中做校验（这里是元类的 `__call__`）。反面是：不占继承位意味着**无法用基类的默认方法**，所有公共逻辑都得靠装饰器或工具函数注入。

**第二，「尽早失败」的边界由状态决定。** 这里「有 `run`」在类创建时检查，「签名一致性」在实例化时检查，理由是 socket 属于实例。主核里同理：能在加载期检查的规则（接口存在、版本匹配）就该在加载期；必须等实例状态才能检查的（配置组合是否有效）只能推迟。要避免的是「能早查却推迟」——那会让错误堆栈离病因很远。

**第三，为一个约束配上「有理有据的错误消息」是必要成本。** `run` 与 `run_async` 必须一致，这是个人为约束（两份手写代码）；若只报「不一致」，写组件的人每次都要自己去 diff。`_compare_run_methods_signatures` 逐项列出差异，把这个约束从「烦人」变成「可接受」。主核里凡是要求「两处声明保持一致」的地方（如 schema 与结构体定义），都应当自带这种差异报告。

**第四，`deepcopy` 在这种「把类级缓存搬到实例」的场景里是必需的，不是保险。** 输出 socket 若不 deepcopy，同类多实例会共享 `OutputSocket` 对象，而运行期会往上写状态。主核里凡是「类级常量被实例拿去当可变状态用」的地方都有同一个 bug 形态，而症状通常是「第二个实例的行为被第一个实例影响」——排序靠后、极难复现。
