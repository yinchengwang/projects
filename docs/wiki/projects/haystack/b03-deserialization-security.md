---
project: haystack
feature: deserialization-security
title: 反序列化的模块白名单
page_type: feature
schema_version: 1
refs:
  - path: haystack/core/serialization_security.py
    symbols: [allow_deserialization_module, _is_module_allowed, _check_resolved_module_allowed]
  - path: haystack/core/serialization_security.py
    symbols: [_is_unsafe_deserialization, _check_traversable_attribute, _is_deserialization_internal]
---

# 反序列化的模块白名单

## 一句话职责

反序列化要按字符串里的全限定名 import 类，而「按名字 import」等价于「执行任意代码」。这一层用六道闸门把这件事收窄成一个可控的集合：模块白名单、解析后模块复查、属性穿越检查、内建黑名单、导入原语黑名单、以及反序列化机制自身的自我封印。

## 机制是什么

b02 讲过，序列化信封里的 `type` 字段就是一个字符串（`haystack.components.builders.answer_builder.AnswerBuilder`）。反序列化时这个字符串被拆成「模块路径 + 属性名」，然后 import 模块、`getattr` 取属性。

这条路径的危险在于：**`getattr(module, name)` 里如果 `name` 可以是任意内容，那就有了一整类沙箱逃逸工具**。`func.__globals__` 给出函数定义模块的活命名空间，从那里可以改白名单状态或拿到 `__builtins__`；`obj.__class__.__subclasses__()` 能遍历所有已加载的类。所以「按字符串解析」这个动作本身必须被逐段限制。

haystack 的答案是**白名单而非黑名单**：

```python
DEFAULT_ALLOWED_MODULES: tuple[str, ...] = (
    "haystack",
    "haystack_integrations",
    "haystack_experimental",
    "builtins",
    "typing",
    "collections",
)
```

六项。前三个是自己的包（组件都在里面），后三个是被序列化类型注解频繁引用的标准库（`typing.List[str]`、`collections.deque`）。注释明确写了理由：「Importing these modules has no meaningful side effects on its own」。

但白名单只有**模块级**粒度，而危险的不是模块而是模块里的成员——`builtins` 必须放行（否则连 `builtins.str` 都解析不了），而 `builtins.eval` 绝不能放行。所以这套机制在模块级白名单之上，又叠了**按上下文区分的成员级闸门**：

| 上下文 | 例子 | 判据 |
|---|---|---|
| 类型/类 | `deserialize_type`、`import_class_by_name` | 解析出的 `builtins` 成员必须是 `type` |
| 可调用对象 | `deserialize_callable` | 解析出的对象不得是黑名单里的函数 |

这个二分解决了「模块粒度太粗」的问题：**类型上下文要求是 `type`（放行所有内建类型，拒绝所有内建函数，不需要维护黑名单）；可调用上下文真的会返回函数，所以只能列黑名单。**

## 怎么实现的

**白名单的扩展有四个来源，而它们是叠加的。**

```python
def _is_module_allowed(module_name: str) -> bool:
    ctx = _get_context()
    if _is_unsafe_deserialization():
        return True
    patterns: list[str] = []
    patterns.extend(DEFAULT_ALLOWED_MODULES)
    patterns.extend(_extra_allowed_modules)
    patterns.extend(_patterns_from_env())
    patterns.extend(ctx.extra_allowed)
    return any(_module_matches(module_name, p) for p in patterns)
```

四处来源：默认集、进程级程序化注册（`allow_deserialization_module`）、环境变量（`HAYSTACK_DESERIALIZATION_ALLOWLIST`，逗号分隔）、以及**本次调用**的（`Pipeline.load(..., allowed_modules=[...])`）。

所以是「四层并联、任一命中即放行」。没有任何一层能**收窄**权限——只能加宽。这个方向是安全的：无法通过传入一个空列表来收紧限制。

**匹配规则混合了前缀语义与 `fnmatch` 语义。**

```python
def _module_matches(module_name: str, pattern: str) -> bool:
    if pattern.endswith(".*") and not any(c in pattern[:-2] for c in "*?["):
        prefix = pattern[:-2]
        return module_name == prefix or module_name.startswith(prefix + ".")
    if any(c in pattern for c in "*?["):
        return fnmatch.fnmatchcase(module_name, pattern)
    return module_name == pattern or module_name.startswith(pattern + ".")
```

三支：

1. **`pkg.*`（且 `.*` 前无通配符）→ 前缀匹配**，且**同时匹配 `pkg` 本身**。注释解释了为什么不用 `fnmatch`：「`pkg.*` requires a literal `.` to follow」，真 `fnmatch` 不会匹配裸的 `pkg`。所以这个特例是必要的修正。

2. **含通配符 → `fnmatch.fnmatchcase`**。用 `fnmatchcase` 而非 `fnmatch`——后者在 Windows 上会做大小写归一，而模块名大小写敏感。

3. **无通配符 → 也按前缀匹配**（`module_name == pattern or startswith(pattern + ".")`）。所以 `allow_deserialization_module("mypkg")` 就等于放行整个子包。这个「裸包名自动含子模块」的语义与 `pkg.*` 一致，文档也明说了：「`"mypkg"` and `"mypkg.*"` behave identically」。

注意第 3 支的 `startswith(pattern + ".")` 是**必要的边界处理**：若只写 `startswith(pattern)`，那 `allow("os")` 会意外放行 `osquery` 这样的模块。加一个 `.` 就排除了。

**`_check_resolved_module_allowed` 是第二道闸，而它检查的是「解析出来的对象实际属于哪个模块」。**

```python
name = getattr(resolved, "__name__", None)
if isinstance(name, str) and getattr(builtins, name, None) is resolved:
    return
module_name = resolved.__name__ if isinstance(resolved, ModuleType) else getattr(resolved, "__module__", None)
if not (isinstance(module_name, str) and module_name):
    return
if _is_module_allowed(module_name):
    return
if (
    declared_module is not None
    and module_name.lstrip("_") == declared_module
    and _is_module_allowed(declared_module)
):
    return
_check_module_allowed(module_name)
```

这段的文档字符串把要防的两类逃逸写得很清楚：

- **模块**：`haystack.utils.auth.os` 会解析到标准库的 `os` 模块，因为 `haystack.utils.auth` 在模块级做了 `import os`。句柄前缀是白名单里的 `haystack`，但解析出的对象属于未放行的 `os`。
- **类/函数**：`haystack.<mod>.<Name>` 可能解析到一个从非白名单模块 import 进来的类（如被重新导出的 `subprocess.Popen`）。

四个分支各自的理由：

1. **内建豁免**：`getattr(builtins, name, None) is resolved` 的对象直接放行。因为内建由另外两道闸负责（可调用路径的 `_check_not_denied_builtin`、类型路径的 `_check_builtin_is_type`），而内建可能报告一个私有 `__module__`（`open.__module__` 是 `"_io"`）。若不豁免，所有内建都会在这里被模块检查拒掉，反而绕过了那两个更精确的闸。

   注释强调「This exemption cannot widen access」——豁免不会放宽，因为危险的内建在下游仍被拦。这个论证是对的：豁免的是**模块检查这一道**，不是全部检查。

2. **`isinstance(resolved, ModuleType)` 时用 `__name__`**：模块的身份在 `__name__` 里，不在 `__module__` 里（模块没有 `__module__`）。这是一个必须区分的细节，否则 `haystack.utils.auth.os` 这种情况查不到任何东西。

3. **`module_name` 不可得时放行**（`functools.partial` 实例这类）。注释：「they are not reachable as gadgets through an attribute walk that stays entirely inside allowlisted modules」。这是「无法判定则交给别的闸」的取舍。

4. **私有加速模块豁免**：`module_name.lstrip("_") == declared_module` 且 `declared_module` 在白名单里。所以 `operator.add`（实际 `__module__` 是 `_operator`）通过，`io.StringIO`（`_io`）通过。

   这个条件的**严格性**值得细看：它要求「去下划线后的名字**等于**声明的模块名」，而不是「在声明模块的白名单里」。所以 `_operator` 只对 `declared_module == "operator"` 成立；若句柄写成 `haystack.foo.operator.add`，`declared_module` 是 `haystack.foo`，`"_operator".lstrip("_") == "haystack.foo"` 为假 → 拒绝。这保证了加速模块豁免**不能横向扩大**到别的模块。

   但注意这个分支还额外要求 `_is_module_allowed(declared_module)` —— 双重确认。虽然调用点已经在白名单里了，但这里显式再查一次，让这个分支可以独立成立。

**属性穿越检查禁止点号走到对象内部。**

```python
_UNSAFE_TRAVERSAL_ATTRS: frozenset[str] = frozenset({
    "gi_frame", "gi_code", "gi_yieldfrom",
    "cr_frame", "cr_code", "cr_await",
    "ag_frame", "ag_code",
    "f_globals", "f_builtins", "f_locals", "f_back", "f_code",
    "func_globals", "func_code", "func_closure", "func_dict", "func_defaults",
})

def _check_traversable_attribute(name: str, handle: str) -> None:
    if _is_unsafe_deserialization():
        return
    if name.startswith("__") or name in _UNSAFE_TRAVERSAL_ATTRS:
        raise DeserializationError(...)
```

两处：

1. **`name.startswith("__")` 一条规则覆盖全部 dunder**。所以 `__globals__`、`__dict__`、`__class__`、`__builtins__`、`__subclasses__` 都被一条前缀规则拦住。这是**用规则的形状换维护成本**——不必枚举 dunder。

2. **非 dunder 的框架逃逸访问器必须显式列出**。`gi_frame`（生成器的帧）、`cr_frame`（协程的帧）、`f_globals`（帧的全局命名空间）、`func_globals` 这些都没有 `__` 前缀，所以命中不了那条前缀规则。注释把这一条需求写得很清楚：「These have no such prefix and must be listed explicitly」。

   列出的 18 个名字覆盖了四类对象：生成器（`gi_*`）、协程（`cr_*`）、异步生成器（`ag_*`）、帧对象（`f_*`）、函数（`func_*`——Python 2 风格的旧名字，仍被保留）。

注意**这个名字列表是「历史上出现过的逃逸工具」的快照**，不是穷举。新的 Python 版本若引入新的帧访问路径，这个集合不会自动跟上。这是一个**黑名单的固有弱点**，而它出现在一个以白名单为主的机制里——说明白名单在「属性名」这个维度上不可行（属性名与正常用法无法用前缀区分）。

**反序列化机制自己的封印是三段判定。**

```python
def _is_deserialization_internal(resolved: object) -> bool:
    if getattr(resolved, _DESERIALIZATION_INTERNAL_ATTR, False):
        return True
    if getattr(resolved, "__module__", None) == __name__:
        return True
    state = (_extra_allowed_modules, _current_context)
    bound_to = getattr(resolved, "__self__", None)
    return any(resolved is s or bound_to is s for s in state)
```

三种方式：

1. **带标记**（`mark_deserialization_internal` 打的属性）。覆盖那些**住在别的模块里**的解析辅助函数：`deserialize_callable`、`deserialize_type`、`import_class_by_name`。

2. **定义在本模块**（`__module__ == "haystack.core.serialization_security"`）。覆盖 `allow_deserialization_module` 与全部 `_check_*` 私有函数。

3. **绑定到模块级可变状态**：`resolved is s or bound_to is s`。即「对象本身就是那个 list/contextvar」或「它是那个对象上的绑定方法」。所以 `_extra_allowed_modules.append`（`__self__` 是那个 list）与 `_current_context.set` 都被拦。

第三个分支的必要性在文档里解释得很具体：

> Left reachable, they let serialized data append `"*"` to the allowlist or install an `unsafe` context — operating the control from within the very data it exists to distrust, which persists process-wide and enables a staged RCE on a later load.

即这两个对象的 `__module__` 分别是 `builtins`（list 的方法）与 `None`（ContextVar 的方法），所以前两个分支都命中不了，必须靠「绑定的宿主对象」来识别。

而 `mark_deserialization_internal` 的文档给出了完整的攻击链，这是本页最有信息量的一段：

> A hostile pipeline can register `allow_deserialization_module` as a Jinja custom filter, call it with `"*"` to disarm the allowlist process-wide, then use the equally-resolvable `deserialize_callable` to resolve and invoke `os.system`.

三步：注册成 Jinja 过滤器 → 传 `"*"` 解除白名单 → 用另一个可解析的辅助函数调 `os.system`。这个链条的每一步都依赖「`haystack` 命名空间整体在白名单里」，所以组件能序列化就必须让整个包可解析——而包的**安全机制本身也在包里**。这就是「自我封印」要解决的根本矛盾。

**`unsafe` 开关是一条贯穿所有检查的旁路，而它被冻结。**

```python
_unsafe_env_snapshot: bool | None = None

def _unsafe_env_enabled() -> bool:
    global _unsafe_env_snapshot
    snapshot = _unsafe_env_snapshot
    if snapshot is None:
        snapshot = os.environ.get(UNSAFE_DESERIALIZATION_ENV_VAR, "").strip().lower() in ("1", "true")
        _unsafe_env_snapshot = snapshot
        if snapshot:
            logger.warning(...)
    return snapshot

def _is_unsafe_deserialization() -> bool:
    return _unsafe_env_enabled() or _get_context().unsafe
```

四处：

1. **`_unsafe_env_snapshot` 是「读一次就冻结」**。注释把理由写成安全性质而非优化：「the first read happens before any deserialized data can run, so a hostile pipeline cannot turn the safety checks off while it is being loaded」。

   冻结的方向是双向的：`snapshot is None` 才读，之后无论环境变量怎么改都不影响。所以**攻击者无法在加载中途打开它，也无法关闭它**。

   而注释还给了一个具体的攻击路径来说明为什么必须冻结：一个白名单模块在模块作用域绑定了 `os.environ`，而它的 `.update` 方法解析为 `collections.abc.MutableMapping.update`（因为 `collections` 在白名单里）。于是「反序列化数据 → 改环境变量 → 打开 unsafe」这条路是通的。这是一个很有说服力的论证——它说明为什么「每次读环境变量」不安全。

2. **延迟到第一次检查时才读，而不是 import 时**。注释：「a `load_dotenv()` that runs before the first load is still honored」。所以先 `load_dotenv()` 再 load 管道是可行的。

3. **`_is_unsafe_deserialization` 里 `_unsafe_env_enabled()` 放在前面**，注释解释了顺序：「so the env snapshot is always taken on the earliest check in the process, even when that check happens inside an `unsafe=True` load」。即若写成 `_get_context().unsafe or _unsafe_env_enabled()`，短路会延迟快照的时机，于是快照可能在一个更晚（更危险）的时间点被取到。

4. **`("1", "true")` 是白名单而不是真值判断**。所以 `HAYSTACK_UNSAFE_DESERIALIZATION=yes` 或 `=0` 都是关闭。用 `.strip().lower()` 归一大小写与空白。

**每次调用的上下文用 `ContextVar` 承载，而合并规则是「只增不减」。**

```python
@dataclass(frozen=True)
class _DeserializationContext:
    extra_allowed: tuple[str, ...] = field(default_factory=tuple)
    unsafe: bool = False

@contextmanager
def _deserialization_context(allowed_modules=None, unsafe=False):
    parent = _get_context()
    extra = parent.extra_allowed + (tuple(allowed_modules) if allowed_modules else ())
    merged_unsafe = parent.unsafe or unsafe
    token = _current_context.set(_DeserializationContext(extra_allowed=extra, unsafe=merged_unsafe))
    try:
        yield
    finally:
        _current_context.reset(token)
```

四处：

1. **`frozen=True`** ——上下文本身不可变，每次进入造一个新的。
2. **`extra = parent.extra_allowed + (...)`** ——**父上下文的模式被继承并追加**，不是替换。所以嵌套的 `Pipeline.load` 调用会累积白名单。
3. **`merged_unsafe = parent.unsafe or unsafe`** ——OR 而非赋值。所以外层的 `unsafe=True` 无法被内层「关掉」。
4. **`ContextVar` + `finally: reset(token)`** ——上下文是**线程/协程局部**的，并发反序列化互不可见。这对服务端是必需的：两个请求同时 load 不同管道，一个传了 `allowed_modules`，另一个不该看到。

`_DeserializationContext` 的文档字符串把合并规则总结成一句：「this never narrows the active permissions」。这是设计上的硬约束——**权限只能单调增加**。

**六道闸的执行顺序，以及每道闸的名字。**

以 `_import_class_by_name` 为例：

```python
module_path, attr_name = fully_qualified_name.rsplit(".", 1)
_check_module_allowed(module_path)              # 闸 1：声明的模块路径
_check_traversable_attribute(attr_name, fully_qualified_name)   # 闸 2：属性不是内部访问器
module = thread_safe_import(module_path)        # ← 副作用点
resolved = getattr(module, attr_name)
_check_resolved_module_allowed(resolved, declared_module=module_path)   # 闸 3：解析后真实模块
_check_not_deserialization_internal(resolved, fully_qualified_name)     # 闸 4：不是控制面
if module_path == "builtins":
    _check_builtin_is_type(resolved, fully_qualified_name)              # 闸 5：内建必须是类型
```

**闸 1 与闸 2 在 import 之前**——这是安全边界的关键（b02 也提到同一个原则的另一处）。闸 3/4/5 只能在 import 之后，因为它们需要 `resolved` 对象。

而闸 2 有个容易被忽略的作用：它拦的是**属性名**，而属性名可以带点（`a.b.C` 里的 `b.C` 部分）。实际上 `rsplit(".", 1)` 只拆最后一段，所以 `attr_name` 永远是单段——那么 `_check_traversable_attribute` 检查的就是这一段的 dunder 前缀与黑名单。

`thread_safe_import` 用一把全局锁包住 `importlib.import_module`：

```python
_import_lock = Lock()

def thread_safe_import(module_name: str) -> ModuleType:
    with _import_lock:
        return importlib.import_module(module_name)
```

四处：

1. **`importlib.import_module` 自身是线程安全的**（CPython 用 per-module 锁），所以这把额外的全局锁把**所有** import 串行化了。文档只说了「Importing modules in a multi-threaded environment can lead to race conditions」——它指的是「模块顶层代码有竞态」这类问题，而不是 import 机制本身。

2. **这把锁也成了 `thread_safe_import` 必须被禁止的原因之一**：它是个「按名字加载任意模块」的原语，与 `__import__` 等价。所以 `_DENIED_CALLABLE_QUALNAMES` 里专门列了它：

```python
_DENIED_CALLABLE_QUALNAMES: frozenset[tuple[str, str]] = frozenset(
    {("haystack.utils.type_serialization", "thread_safe_import")}
)
```

   注释解释了为什么用 `(module, qualname)` 而不用对象身份：「so this module need not import `haystack.utils.type_serialization` (which itself imports from this module, so an import here would be circular)」。这是**用字符串匹配绕开循环 import**——代价是「改名就失效」。

3. **`_DENIED_CALLABLE_OBJECTS = {importlib.import_module, importlib.reload}`** 用身份匹配。所以 `import_module` 的别名（`from importlib import import_module as im`）也会被拦住——因为对象是同一个。

4. **这个黑名单只覆盖「按名字加载模块」的原语**，不覆盖「已经拿到的模块对象上的方法」。所以一个白名单模块若在模块级绑定了 `os`，`_check_resolved_module_allowed` 才是拦它的那道。

**`_check_builtin_is_type` 是「用判据代替黑名单」的实例。**

```python
if not isinstance(resolved, type):
    raise DeserializationError(
        f"Refusing to deserialize '{handle}': it resolves to a builtin that is not a type..."
    )
```

注释给出了理由，值得逐字引：「That lets every builtin type through (`str`, `memoryview`) while rejecting every builtin *function* (`eval`, `exec`, `getattr`), **with no denylist to maintain**」。

这是一个**结构性判据**而不是枚举：内建类型全部是 `type` 的实例，内建函数全部不是。所以一条 `isinstance` 就等价于一份「所有内建类型」的白名单，且**新版本 Python 加的内建函数自动被拒**。

对比 `_DENIED_BUILTIN_NAMES` 那份 14 个名字的黑名单（`eval`/`exec`/`compile`/`__import__`/`open`/`getattr`/`setattr`/`delattr`/`globals`/`locals`/`vars`/`breakpoint`/`__build_class__`/`type`）——它必须枚举，因为可调用路径**要返回函数**，无法用类型判据。

所以同一份代码里两种手法并存，而它们的适用条件很清楚：

| 判据形式 | 适用 | 弱点 |
|---|---|---|
| 结构性（`isinstance(x, type)`） | 目标集合有共同类型特征 | 目标集合必须真的同质 |
| 枚举黑名单 | 目标集合无共同特征 | 新成员不会被自动覆盖 |

`_DENIED_BUILTIN_NAMES` 里每一项都带了注释说明危险类型：

```python
"eval",            # arbitrary code execution
"exec",            # arbitrary code execution
"compile",         # arbitrary code compilation
"__import__",      # dynamic import of any module (gateway to os/subprocess/...)
"open",            # filesystem read/write
"getattr",         # attribute-traversal gadget (classic sandbox escape)
"setattr",         # arbitrary attribute mutation
"delattr",         # arbitrary attribute deletion
"globals",         # access to module namespaces
"locals",          # access to local namespaces
"vars",            # access to object/module namespaces
"breakpoint",      # runs the PYTHONBREAKPOINT hook
"__build_class__", # dynamic class creation
"type",            # dynamic class creation via type(name, bases, dict)
```

三处：

- **`getattr` 在名单里**——而反序列化本身就在用 `getattr`。所以这是个「工具与武器同形」的例子：框架自己可以用，但从序列化数据里解析出来就危险。
- **`type` 在名单里**——`type(name, bases, dict)` 能动态造类，是沙箱逃逸的经典手法。但 `type` 也是内建**类型**，所以类型上下文（`_check_builtin_is_type`）会放行它。两处的判定方向恰好相反且都是对的。
- **`breakpoint` 在名单里**——它执行 `PYTHONBREAKPOINT` 环境变量指定的钩子，是一个间接执行通道。这一项说明这份名单考虑得比较细（不是只列了最出名的几个）。

`_DENIED_BUILTIN_OBJECTS` 用身份：

```python
_DENIED_BUILTIN_OBJECTS: frozenset = frozenset([getattr(builtins, name) for name in _DENIED_BUILTIN_NAMES])
```

注释解释了为什么用身份而非名字：「Resolve names to objects once so callers can match by identity, which also catches aliases that reach the same builtin via a different import path (e.g. `io.open is builtins.open`)」。

**这是一个真实且重要的补丁**：`io.open` 就是 `builtins.open`，句柄写 `io.open` 时名字匹配会漏掉，身份匹配不会。而 `io` 不在白名单里，所以这道闸其实轮不到——但如果将来 `io` 被加进白名单（`_io.StringIO` 的加速模块豁免已经暗示了这个方向），身份匹配就是唯一可靠的判定。

`_is_denied_builtin` 用 `any(resolved is denied ...)` 而不是 `resolved in frozenset`，注释说「so an unhashable resolved object never raises」。即 `frozenset.__contains__` 会对不可哈希对象抛 `TypeError`，而线性 `is` 比较不会。

## 关键取舍

**源码事实：** 默认白名单六项（`haystack`/`haystack_integrations`/`haystack_experimental`/`builtins`/`typing`/`collections`），选标准库三项的理由是「import 它们本身没有副作用」。

**源码事实：** 白名单有四个叠加来源（默认 / 进程级注册 / 环境变量 / 本次调用），任一命中即放行，**没有任何一层能收窄权限**；`_deserialization_context` 也只追加（`parent.extra_allowed + new`）并 OR 合并 `unsafe`。

**源码事实：** 匹配规则三分支：`pkg.*`（`.*` 前无通配符）走前缀并**同时匹配 `pkg` 本身**（真 `fnmatch` 不会）；含通配符走 `fnmatchcase`（不是 `fnmatch`，避免 Windows 大小写归一）；无通配符也按前缀，且用 `startswith(pattern + ".")` 而非 `startswith(pattern)`（否则 `allow("os")` 会放行 `osquery`）。

**源码事实：** `_check_resolved_module_allowed` 复查对象**实际所属**模块，防两类逃逸：白名单模块里 `import os` 后暴露 `os` 模块、以及被重新导出的类（如 `subprocess.Popen`）。

**源码事实：** 该复查有四个例外分支：**真内建豁免**（`getattr(builtins, name) is resolved`，因为它们由更精确的闸负责且可能报告私有 `__module__` 如 `open` → `_io`）；**模块用 `__name__`**（模块没有 `__module__`）；**`module_name` 不可得则放行**（`functools.partial` 这类）；**私有加速模块豁免**（`module_name.lstrip("_") == declared_module` 且声明模块在白名单里，所以 `_operator`→`operator`、`_io`→`io` 通过，且不能横向扩大）。

**源码事实：** `_check_traversable_attribute` 用一条 `name.startswith("__")` 覆盖全部 dunder，另需显式列出 18 个非 dunder 的框架访问器（`gi_frame`/`cr_frame`/`f_globals`/`func_globals` 等），因为它们没有 `__` 前缀。

**源码事实：** `_is_deserialization_internal` 三种命中方式：带 `mark_deserialization_internal` 标记（覆盖别处的解析辅助函数）、`__module__` 等于本模块、**或绑定到模块级可变状态**（`_extra_allowed_modules` / `_current_context`）——第三支必要是因为 `list.append` 与 `ContextVar.set` 的 `__module__` 分别是 `builtins` 与 `None`。

**源码事实：** `mark_deserialization_internal` 的文档给出了完整攻击链：把 `allow_deserialization_module` 注册成 Jinja 过滤器 → 传 `"*"` 解除白名单 → 用同样可解析的 `deserialize_callable` 调 `os.system`。

**源码事实：** `unsafe` 环境变量（`HAYSTACK_UNSAFE_DESERIALIZATION`）在**第一次检查时**读取并**冻结**，双向生效；冻结是安全性质而非优化（否则反序列化数据可在加载中途打开它），且延迟读取让先前的 `load_dotenv()` 仍生效。判定用 `("1","true")` 白名单而非真值。

**源码事实：** `_is_unsafe_deserialization` 把 `_unsafe_env_enabled()` 放在 `or` 之前，确保快照总在最早期检查时取；`_DeserializationContext` 是 `frozen=True` 且存在 `ContextVar` 里，`finally: reset(token)`。

**源码事实：** 校验顺序：模块白名单与属性穿越检查在 **import 之前**，解析后模块复查/控制面检查/内建类型检查只能在 import 之后。

**源码事实：** 可调用路径的内建黑名单 14 项（含 `getattr`/`setattr`/`globals`/`locals`/`vars`/`breakpoint`/`__build_class__`/`type`），按**对象身份**匹配（以捕获 `io.open is builtins.open` 这类别名），并用 `is` 线性比较而非 `in`（避免不可哈希对象抛 `TypeError`）。

**源码事实：** 导入原语黑名单用两种匹配：`importlib.import_module`/`reload` 按身份；`thread_safe_import` 按 `(__module__, __qualname__)` 字符串对，理由是避免与 `type_serialization` 循环 import——代价是改名即失效。

**源码事实：** 类型上下文用结构性判据 `isinstance(resolved, type)` 而非黑名单，注释明确说这样「no denylist to maintain」——新版本 Python 新增的内建函数自动被拒。

**架构推论：** 这套机制的核心结构是「**白名单管模块，判据管成员，封印管自己**」。三层各自解决不同粒度的问题：模块级只能粗粒度放行（`builtins` 必须放行），成员级必须按上下文选判据（类型上下文能用结构性判据，可调用上下文只能枚举），而「框架自己的安全接口也在被放行的命名空间里」这个矛盾只能用自我封印解决。第三条是最容易被忽略的——**任何「放行自己以支持自反」的设计都会遇到同一问题**。

**架构推论：** 「权限只能单调增加」这条约束（四层叠加、上下文只追加、`unsafe` 只 OR）是这套设计的可靠性基础。若允许某一层收紧，就出现「这次 load 比上次更严格」的可能，而严格性的不一致会让「这个管道为什么昨天能 load 今天不行」变成不可推理解的问题。代价是**无法在单次调用里收紧**——要收紧只能改代码或进程级配置。

**架构推论：** 环境变量快照的「读一次冻结」是本页最精妙的一处。它解决的不是性能而是**时序安全问题**：安全配置必须在任何不可信代码运行之前确定，否则「运行 → 改配置 → 绕过」这条链就成立了。这与主核里「`postgresql.conf` 在启动时读取」是同一个道理——配置的一次性读取本身就是一种安全边界。

**架构推论：** `_UNSAFE_TRAVERSAL_ATTRS` 这 18 个名字是黑名单，而它出现在一个以白名单为主的机制里。这暴露了一个本质限制：**属性名这个维度无法用白名单表达**，因为正常用法（`module.ClassName.method`）与逃逸用法（`func.__globals__`）在「是不是合法标识符」上没有区别。所以这一层只能靠「枚举已知的坏名字 + 一条 dunder 前缀规则」——而后者能覆盖大部分，是因为 Python 的 dunder 命名约定恰好把危险访问器集中在 `__` 前缀下（除了帧访问器）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/core/serialization_security.py` | `DEFAULT_ALLOWED_MODULES` | 六项默认白名单 |
| `haystack/core/serialization_security.py` | `allow_deserialization_module` | 进程级追加白名单模式（自身被封印） |
| `haystack/core/serialization_security.py` | `_module_matches` | 前缀 / `fnmatchcase` 三分支匹配 |
| `haystack/core/serialization_security.py` | `_is_module_allowed` | 四层来源叠加判定 |
| `haystack/core/serialization_security.py` | `_check_module_allowed` | 失败时给出三条扩展途径 |
| `haystack/core/serialization_security.py` | `_check_resolved_module_allowed` | 复查对象真实模块 + 四个例外分支 |
| `haystack/core/serialization_security.py` | `_UNSAFE_TRAVERSAL_ATTRS` | 18 个非 dunder 的框架逃逸访问器 |
| `haystack/core/serialization_security.py` | `_check_traversable_attribute` | `__` 前缀 + 显式名单 |
| `haystack/core/serialization_security.py` | `mark_deserialization_internal` | 给别处的解析辅助函数打封印标记 |
| `haystack/core/serialization_security.py` | `_is_deserialization_internal` | 三种命中方式（含绑定到可变状态） |
| `haystack/core/serialization_security.py` | `_check_not_deserialization_internal` | 拒绝控制面对象并给出 `unsafe=True` 提示 |
| `haystack/core/serialization_security.py` | `_DENIED_BUILTIN_NAMES` / `_DENIED_BUILTIN_OBJECTS` | 14 项内建黑名单（名字 + 身份） |
| `haystack/core/serialization_security.py` | `_DENIED_CALLABLE_QUALNAMES` | `thread_safe_import` 的字符串对匹配（避循环 import） |
| `haystack/core/serialization_security.py` | `_check_builtin_is_type` | 类型上下文的结构性判据 |
| `haystack/core/serialization_security.py` | `_check_not_denied_builtin` / `_check_not_denied_callable` | 可调用路径的内建与导入原语黑名单 |
| `haystack/core/serialization_security.py` | `_unsafe_env_enabled` / `_is_unsafe_deserialization` | 读一次冻结的环境快照 + 上下文 OR |
| `haystack/core/serialization_security.py` | `_deserialization_context` | `ContextVar` 上下文，只追加不收窄 |
| `haystack/utils/type_serialization.py` | `_import_class_by_name` | 六道闸的实际执行顺序 |
| `haystack/utils/type_serialization.py` | `thread_safe_import` | 全局锁下的 import，本身也被列为导入原语 |

## 参考

- [haystack 源码：serialization_security.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/serialization_security.py)
- [haystack 文档：序列化与安全](https://docs.haystack.deepset.ai/docs/serialization)
- [Python 文档：pickle 的安全限制](https://docs.python.org/3/library/pickle.html#warning)
- [OWASP：反序列化漏洞](https://owasp.org/www-community/vulnerabilities/Deserialization_of_untrusted_data)

## 与主核 / AI 的关系

这套机制与主核（多模态数据库内核）的**扩展加载**是同一类问题的解法。主核加载共享库扩展时也只能按名字（`LOAD 'path/to/ext.so'`），而「按名字加载可执行代码」与「按名字 import 模块」风险相同。主核的处理是**要求超级用户权限 + 限定库目录**；haystack 的处理是**白名单 + 六道闸**。差别在于威胁模型：主核的扩展加载是 DBA 行为（可信但需授权），haystack 的反序列化面对的是**上传的 YAML 文件**（不可信且频繁）。

第二处对照是 **「权限只增不减」与主核的 `SECURITY DEFINER`**。主核里函数的权限是「调用者的权限」或「定义者的权限」二选一，而 `SECURITY DEFINER` 函数在执行期间**提升**权限。haystack 的嵌套上下文同理只追加不放宽——但它是**单调**而非**提升**，因为追加的模式在整个调用链上都有效，不像 `SECURITY DEFINER` 只在函数体内有效。这个差别让 haystack 的语义更容易推理：不需要知道「当前在谁的上下文里」。

第三处是最值得记的：**「读一次冻结」与主核的 `postgresql.conf` 启动读取**。两者都把「安全相关的配置必须在一个确定的时间点定下来」当作原则。主核之所以不能在运行期随意改 `shared_buffers`，部分原因是「改了会让行为不可预测」；haystack 之所以冻结 `unsafe` 快照，是因为「不冻结就存在运行期绕过」。**同一个手法，一个服务于可预测性，一个服务于安全性**——而两者都需要「一次性确定」这个性质。

对 AI 工程的实际意义在于：任何「按名字加载代码」的能力都是 RCE 的入口，而 RAG 框架恰恰必须有这个能力（组件要能序列化）。这份实现给出的三条经验可直接用：**白名单管模块、判据管成员、封印管自己**（三层缺一层就有一条逃逸路径）、**安全校验要排在 import 之前**（顺序即边界）、以及**安全配置必须在不可信代码运行前确定**（快照冻结）。
