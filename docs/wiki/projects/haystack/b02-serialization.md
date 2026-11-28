---
project: haystack
feature: serialization
title: 组件序列化
page_type: feature
schema_version: 1
refs:
  - path: haystack/core/serialization.py
    symbols: [component_to_dict, component_from_dict, default_to_dict, default_from_dict]
  - path: haystack/core/serialization.py
    symbols: [generate_qualified_class_name, import_class_by_name]
---

# 组件序列化

## 一句话职责

把组件实例变成 `{"type": 全限定类名, "init_parameters": {...}}` 这一层信封，并能从信封重建实例；四个入口分工是「组件级转发」与「默认实现」，而类型名是跨进程可解析的唯一凭据。

## 机制是什么

这套序列化要解决的核心问题是：**一个组件的状态用什么表达**。

它不是通用的对象序列化（`pickle` 那类），而是**声明式的构造式序列化**——只记录「这个对象是怎么被构造出来的」，即类名 + 构造参数。重建时重新执行构造函数。

这个选择带来三个直接后果：

1. **对象必须可重构造**。构造函数若有副作用（读环境变量、建连接），重建时会再执行一遍。
2. **状态必须来自构造参数**。运行期才产生的状态（缓存、连接池）不参与序列化——这是对的，因为它们本来就该在重建后为空。
3. **对外只有一层信封**，且这个信封被明确声明为**与另一套信封不通用**（见下）。

第二个核心问题是**类型名怎么表达**。用 `__module__ + "." + __name__` 拼出全限定名（如 `haystack.components.builders.answer_builder.AnswerBuilder`），反序列化时按这个名字导入。这是「字符串即身份」的做法，安全边界因此全在导入那一步（b03 专讲）。

## 怎么实现的

**`component_to_dict` 是「有自定义就走自定义，没有就反射」的二选一。**

```python
def component_to_dict(obj: Any, name: str) -> dict[str, Any]:
    if hasattr(obj, "to_dict"):
        data = obj.to_dict()
    else:
        init_parameters = {}
        for param_name, param in inspect.signature(obj.__init__).parameters.items():
            if param_name in ("args", "kwargs"):
                continue
            try:
                param_value = getattr(obj, param_name)
            except AttributeError as e:
                if param.default is param.empty:
                    raise SerializationError(
                        f"Cannot determine the value of the init parameter '{param_name}' "
                        f"for the class {obj.__class__.__name__}."
                        f"You can fix this error by assigning 'self.{param_name} = {param_name}' or adding a "
                        f"custom serialization method 'to_dict' to the class."
                    ) from e
                param_value = param.default
            init_parameters[param_name] = param_value
        data = default_to_dict(obj, **init_parameters)

    _validate_component_to_dict_output(obj, name, data)
    return data
```

五处：

1. **`hasattr(obj, "to_dict")` 而不是检查某个基类**。所以任何有 `to_dict` 方法的对象都自动被支持——这是**结构化类型**（duck typing）的用法，与组件基类无关。

2. **`param_name in ("args", "kwargs")` 被跳过**。注释说「used by the default constructor」——因为很多组件用 `**kwargs` 收参数，那个 `kwargs` 参数本身不是一个有意义的构造参数。

3. **`getattr(obj, param_name)` 依赖「构造参数被赋值成同名实例属性」**。注释写得很直白：「This only works if the Component constructor assigns the init parameter to an instance variable or property with the same name」。所以这套反射是有前提的约定，不是通用的。

4. **取不到属性时分两种情况**：该参数**无默认值** → 抛 `SerializationError`，且错误消息直接给出两种修法（`self.x = x` 或加 `to_dict`）；**有默认值** → 用默认值填。这个「用默认值兜底」意味着**一个没保存的参数会被静默地按默认值恢复**——如果实际运行时的值不是默认值（例如构造后被改过），重建出的对象就与原件不同。

5. **`_validate_component_to_dict_output` 在最后统一校验**。它不管数据从哪来（自定义 `to_dict` 还是反射），一律检查形状。

**类型校验的允许类型集是一个显式白名单。**

```python
def is_allowed_type(obj: Any) -> bool:
    return isinstance(obj, (str, int, float, bool, list, dict, set, tuple, type(None)))
```

九种：`str`/`int`/`float`/`bool`/`list`/`dict`/`set`/`tuple`/`NoneType`。递归检查容器内部：

```python
def check_iterable(iterable):
    for v in iterable:
        if not is_allowed_type(v):
            raise SerializationError(...)
        if isinstance(v, (list, set, tuple)):
            check_iterable(v)
        elif isinstance(v, dict):
            check_dict(v)
```

三处：

- **`bool` 在 `isinstance` 检查里是多余的**——`bool` 是 `int` 的子类，`isinstance(True, int)` 已为真。但保留它让白名单读起来更完整。
- **`set` 被允许**，而它是无序的。所以一个含 `set` 的序列化结果**在 YAML/JSON 里没有稳定表示**（`json.dumps` 直接报错）。这是一个隐含的限制：这个校验只看「Python 类型安全」，不看「能否落盘」。落盘那一步（b04）自己处理。

- **递归是手写的两层**（`check_iterable` ↔ `check_dict`），互相调用。所以嵌套任意深度的容器都能检查到。

`check_dict` 还额外要求**键必须是字符串**：

```python
if any(not isinstance(k, str) for k in d):
    raise SerializationError(... has a non-string key ...)
```

这是落盘的必要条件（YAML/JSON 的键必须是标量，通常要求字符串）。所以这个校验实际上混合了两类要求：**类型可序列化**（白名单）与**结构可落盘**（键为字符串）。

注意**这个校验递归但会无限递归于自引用结构**——一个 `dict` 包含自己会导致 `RecursionError`。没有环检测。

**`generate_qualified_class_name` 是最简单的函数，但它是整套机制的锚点。**

```python
def generate_qualified_class_name(cls: type[object]) -> str:
    return f"{cls.__module__}.{cls.__name__}"
```

两处必知的性质：

1. **用 `__name__` 而不是 `__qualname__`**。所以嵌套类（`Outer.Inner`）会得到 `module.Inner`——**丢掉外层名字**。反序列化时 `_import_class_by_name` 用 `rsplit(".", 1)` 拆成 `module` 与 `Inner`，然后 `getattr(module, "Inner")` 会失败（因为 `Inner` 不是模块的属性）。所以**嵌套类无法序列化往返**。

2. **`__module__` 是定义处的模块**，而不是「导入路径」。所以若一个类被重新导出（`from a import B as B` 在 `c` 里），它的 `__module__` 仍是 `a`。这保证了往返正确（回到定义处），但也意味着**重命名模块会破坏所有旧存档**。

**`default_to_dict` 的自动化只有一条规则：带 `to_dict` 的值自动递归。**

```python
serialized_params = {}
for key, value in init_parameters.items():
    if value is not None and hasattr(value, "to_dict") and callable(value.to_dict):
        serialized_params[key] = value.to_dict()
    else:
        serialized_params[key] = value

return {"type": generate_qualified_class_name(type(obj)), "init_parameters": serialized_params}
```

三处：

1. **`value is not None` 前置检查**——因为 `None` 会通过 `hasattr` 吗？不会（`None` 没有 `to_dict`）。所以这个检查是冗余的，但它让意图更明确：`None` 一律原样保留。

2. **只处理一层**。`init_parameters` 里的 dict 不会被递归进去找 `to_dict` 对象。所以「`{"a": {"b": SomeComponent()}}`」里的 `SomeComponent` **不会被自动序列化**——它会原样留在字典里，然后被 `_validate_component_to_dict_output` 拒绝（不是允许类型）。这是一个真实的行为边界。

3. **`type(obj)` 而不是 `obj.__class__`**——同一个东西，但用 `type()` 更常见于运行时。

**`default_from_dict` 的自动反序列化有**三类**特例，顺序很重要。**

```python
init_params = dict(data.get("init_parameters", {}))
if "type" not in data:
    raise DeserializationError("Missing 'type' in serialization data")
if data["type"] != generate_qualified_class_name(cls):
    raise DeserializationError(f"Class '{data['type']}' can't be deserialized as '{cls.__name__}'")

valid_init_param_names = _init_parameter_names(cls)

for key, value in init_params.items():
    if isinstance(value, dict) and "type" in value:
        type_value = value.get("type")
        if type_value == "env_var":
            init_params[key] = Secret.from_dict(value)
        elif _is_serialized_component_device(value):
            init_params[key] = ComponentDevice.from_dict(value)
        elif isinstance(type_value, str) and "." in type_value:
            ...
```

四处：

1. **`init_params = dict(data.get("init_parameters", {}))` 复制一份**。注释解释了为什么：「Without this, a second deserialization of the same dict would receive already-parsed objects instead of their serialized form」。即**这个函数会就地替换值**，若不复制就会污染调用方的数据，导致同一个 dict 只能反序列化一次。这是一个真实的陷阱，而修法（浅复制）足够——因为只替换顶层值。

2. **`type` 字段双重校验：必须存在，且必须等于 `cls` 的全限定名**。所以你**不能用 `default_from_dict` 反序列化成别的类**——即使那个类兼容。这个严格性是安全边界的一部分：`type` 决定了构造哪个类，若允许覆盖就等于允许任意类实例化。

3. **三个特殊分支按 `type` 值分派**：`"env_var"` → `Secret`；`"single"`/`"multiple"` + 特定结构 → `ComponentDevice`；含 `.` 的字符串 → 尝试当全限定类名导入。

   前两个是**硬编码的魔术字符串**。为什么不用 `.` 检查统一处理？因为 `Secret.to_dict()` 产出的 `type` 是 `"env_var"`（不含 `.`），所以会落到 `.` 检查之外。若统一用 `.` 判定，`Secret` 就永远无法自动反序列化。

   所以这三个分支的存在说明：**`type` 字段承载了两种语义**——「信封类型标签」（`env_var`/`single`/`multiple`）与「类名」（含 `.`）。代码靠「有没有点」区分。这是一个脆弱但有效的约定。

4. **未知参数的拒绝发生在 import 之前**：

```python
if valid_init_param_names is not None and key not in valid_init_param_names:
    known_params = (...)
    raise DeserializationError(
        f"Refusing to deserialize unknown parameter '{key}' for '{cls.__name__}'. {known_params} "
        f"Correct the parameter name or remove it from the serialized data."
    )
```

   注释：「blocks YAML that smuggles untrusted classes into unused parameter slots」。即**先看这个参数名这个类收不收，不收就完全不 import**。这是一个很重要的顺序：若先 import 再检查，恶意 YAML 里的类已经被导入了（模块顶层代码已执行）。所以这里的顺序是安全边界的一部分。

**`_init_parameter_names` 的返回值是 `set | None`，而 `None` 有特定含义。**

```python
def _init_parameter_names(cls: type[object]) -> set[str] | None:
    try:
        signature = inspect.signature(cls.__init__)
    except (TypeError, ValueError):
        return None
    names: set[str] = set()
    for name, param in signature.parameters.items():
        if name == "self":
            continue
        if param.kind is inspect.Parameter.VAR_KEYWORD:
            # Constructor accepts **kwargs; we cannot tell whether `key` is a real parameter.
            return None
        if param.kind is inspect.Parameter.VAR_POSITIONAL:
            continue
        names.add(name)
    return names
```

三处：

1. **`**kwargs` → 返回 `None`（放弃校验）**。因为收 `**kwargs` 的类对任何参数名都不报错，无法判断额外参数是不是「未使用的位置」。于是那条防走私检查在这种情况下**不生效**——这是一个真实的安全缺口，但它被记在注释里（「we cannot tell whether `key` is a real parameter」）。

2. **`*args`（`VAR_POSITIONAL`）被跳过**——位置参数没有名字，无法作为关键字传。

3. **`inspect.signature` 的 `TypeError`/`ValueError` → `None`**。所以拿不到签名的类（某些 C 扩展）也放弃校验。

**三个 `DeserializationCallbacks` 的存在说明反序列化需要外部介入点。**

```python
@dataclass(frozen=True)
class DeserializationCallbacks:
    component_pre_init: Callable | None = None
```

```python
def component_pre_init_callback(component_cls: type, init_params: dict[str, Any]) -> None:
    assert callbacks is not None
    assert callbacks.component_pre_init is not None
    callbacks.component_pre_init(name, component_cls, init_params)
```

四处：

1. **`frozen=True`**——回调配置不可变。

2. **回调可以修改 `init_params` 字典**（文档明确说了）。所以这是一个**注入点**：可以在组件构造前替换参数值（例如从密钥管理器取真密钥替换占位符）。

3. **两个 `assert`**——它们的作用是给类型检查器看（在 `component_pre_init_callback` 内部，`callbacks` 一定非 `None` 且 `component_pre_init` 一定非 `None`，因为只有那种情况下才注册这个回调）。用 `assert` 而非 `if` 是合理的；但 `assert` 在 `-O` 下会被去掉，此时若前提不成立会变成 `AttributeError`。

4. **`with _hook_component_init(component_pre_init_callback):`** ——用 `ContextVar` 传递回调（`_COMPONENT_PRE_INIT_HOOK.set(...)` + `finally: reset(token)`）。所以回调是**线程/协程局部**的，不是全局状态。这是必要的：并发的反序列化不能互相看到对方的回调。

   这也解释了为什么回调要绕一圈：组件构造函数里没有回调参数，所以只能靠上下文变量「进去」。而 `_hook_component_init` 必须在调用组件构造函数**之前**进入——而实际执行构造函数的是 `cls.from_dict(data)`，所以这个 `with` 必须包住整个 `do_from_dict()`。

**`import_class_by_name` 是转发，而转发的原因是安全标记。**

```python
@mark_deserialization_internal
def import_class_by_name(fully_qualified_name: str) -> type[object]:
    return _import_class_by_name(fully_qualified_name)
```

实际实现在 `haystack/utils/type_serialization.py`，做四件事（b03 详述）：查模块白名单 → 查属性是否可穿越 → import → **再查一次解析后的模块** → 查是否是反序列化内部机制 → 若是 `builtins` 还查是否是类型。

而 `serialization.py` 顶部的注释解释了那个奇怪的 `as`：

```python
from haystack.core.serialization_security import allow_deserialization_module as allow_deserialization_module
```

「The redundant `as` alias marks it as an intentional re-export so ruff does not flag it (F401)」。所以这是**给 lint 工具看的标记**——一种用语言特性表达意图的技巧。

`@mark_deserialization_internal` 是另一个机制：给函数打标记，让反序列化时**拒绝把这类函数当作 class 解析**。所以恶意 YAML 不能写 `type: haystack.core.serialization_security.allow_deserialization_module` 来调用白名单函数。这是一个**自我保护的闭环**——防止安全机制自己被当作入口。

**`default_to_dict` 的文档里有一段很硬的警告，关于两套信封不能混用。**

> This is the format used for saved pipeline files (`Pipeline.dump`/`Pipeline.load`). Don't merge it with `base_serialization._serialize_value_with_schema` — that one uses a different envelope for a different job (arbitrary runtime values, not Components) and changing either would break saved files.

所以项目里存在**两套并存的序列化信封**：

| | 本页这套 | `_serialize_value_with_schema` |
|---|---|---|
| 用途 | 组件（构造式） | 任意运行时值（带 schema） |
| 信封 | `{"type", "init_parameters"}` | 另一套 |
| 稳定性要求 | 已存盘，不能改 | 同 |

注释明确说「changing either would break saved files」。这是把「兼容性约束」写进文档的做法——因为代码本身无法表达「这个格式已经被写进磁盘了」。

## 关键取舍

**源码事实：** 序列化是**构造式**的：只记 `{"type": 全限定类名, "init_parameters": {...}}`，重建靠重新执行构造函数。运行期才产生的状态不参与。

**源码事实：** `component_to_dict` 用 `hasattr(obj, "to_dict")` 做结构化判定；无自定义时反射 `__init__` 签名并对每个参数 `getattr(obj, param_name)`——依赖「构造参数赋成同名属性」这个约定。取不到时：无默认值抛 `SerializationError`（错误消息给两种修法），有默认值则**静默用默认值**。

**源码事实：** 序列化结果统一过一遍 `_validate_component_to_dict_output`：允许 `str`/`int`/`float`/`bool`/`list`/`dict`/`set`/`tuple`/`None`，递归检查容器内部，且字典**键必须是字符串**。无环检测（自引用会 `RecursionError`）。

**源码事实：** `generate_qualified_class_name` 用 `__module__ + "." + __name__`（**不是 `__qualname__`**），所以嵌套类会丢掉外层名，无法往返。

**源码事实：** `default_to_dict` 只把**顶层**带 `to_dict` 的值递归序列化，不钻进 `dict`/`list` 内部——嵌套对象会留在原处并被形状校验拒绝。

**源码事实：** `default_from_dict` 先复制 `init_parameters`（避免就地替换污染调用方数据，使同一 dict 可反序列化多次），再校验 `type` 存在且**必须等于 `cls` 的全限定名**（不允许反序列化成别的类）。

**源码事实：** `type` 字段承载两种语义：信封标签（`"env_var"` / `"single"` / `"multiple"`）与类名（含 `.`），代码靠「有没有点」区分；这就是 `Secret`/`ComponentDevice` 需要硬编码分支的原因。

**源码事实：** 对「含 `.` 的未知参数」，**先按构造签名拒绝不认识的参数名，再 import**（注释：blocks YAML that smuggles untrusted classes into unused parameter slots）——顺序是安全边界的一部分。

**源码事实：** `_init_parameter_names` 在构造函数收 `**kwargs`、或 `inspect.signature` 抛 `TypeError`/`ValueError` 时返回 `None`，此时上述「未知参数拒绝」**不生效**。

**源码事实：** `DeserializationCallbacks.component_pre_init` 通过 `_hook_component_init`（`ContextVar` + `finally: reset`）在组件构造前注入，可修改 `init_params`；它是上下文局部的，并发反序列化互不可见。

**源码事实：** `import_class_by_name` 只是转发到 `utils/type_serialization._import_class_by_name`，自身带 `@mark_deserialization_internal` 标记——防止反序列化机制自己成为可被 YAML 引用的入口。

**源码事实：** `allow_deserialization_module` 在 `serialization.py` 里以 `as` 自我别名再导出，注释说明这是给 ruff 看的「有意重导出」标记（否则报 F401）。

**源码事实：** `default_to_dict` 的文档明确声明它的信封与 `base_serialization._serialize_value_with_schema` 的信封**不通用**，两者都已被写进存档文件，改动任一都会破坏旧存档。

**架构推论：** 「构造式序列化」把**可重建性**当成了序列化的前提。代价是：构造函数有副作用时会重复执行；构造后被改过的属性不会出现在存档里（`getattr` 拿到的是当前值，这反而是对的）；而「有默认值的参数取不到就填默认」这一条会把「存档里没这个参数」与「这个参数就是默认值」混为一谈。这最后一条是本页最实际的缺陷——它让「存档不完整」变成一个静默的成功。

**架构推论：** 用字符串当类型身份（`module.Class`）意味着**类名与模块路径成为公开契约**。一旦有人保存了管道文件，这个类的模块路径、类名都不能改。而 `__name__` 而不是 `__qualname__` 的选择排除了嵌套类——这大概是可接受的（组件都是模块级类），但它是一个没被写在文档里的限制。

**架构推论：** 「先校验参数名再 import」这个顺序，是「安全校验必须在副作用之前」的实例。若顺序颠倒（先 import 再检查参数），则恶意 YAML 里的类在拒绝之前就已经执行了模块顶层代码。这类「检查顺序即安全边界」的问题在反序列化代码里普遍存在，而它只能靠代码审查发现——类型系统无法表达「这一步必须在 import 之前」。

**架构推论：** 三处「放弃校验返回 `None`」（`**kwargs`、签名不可得）是**安全机制里的显式缺口**。它们存在的原因是「无法判断」，而不是「不想检查」。这类缺口应该被记录下来（注释确实写了），但它带来的实际风险是：一个用 `**kwargs` 收参数的组件无法阻止任意 `type` 值的注入。所以「组件怎么写构造函数」变成了一个安全相关的决定——这个耦合在文档里没有写。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/core/serialization.py` | `component_to_dict` | 有 `to_dict` 则转发，否则反射 `__init__` 拼参数 |
| `haystack/core/serialization.py` | `_validate_component_to_dict_output` | 九种允许类型的递归白名单 + 键必须是字符串 |
| `haystack/core/serialization.py` | `component_from_dict` | 有 `from_dict` 则转发，并在有回调时用 `_hook_component_init` 包住 |
| `haystack/core/serialization.py` | `default_to_dict` | 生成信封，顶层带 `to_dict` 的值自动递归 |
| `haystack/core/serialization.py` | `default_from_dict` | 复制参数、校验 `type`、三类特例自动反序列化、未知参数先拒再 import |
| `haystack/core/serialization.py` | `_init_parameter_names` | 反射构造签名；`**kwargs` 或签名不可得时返回 `None`（放弃校验） |
| `haystack/core/serialization.py` | `generate_qualified_class_name` | `__module__ + "." + __name__` |
| `haystack/core/serialization.py` | `import_class_by_name` | 转发到 type_serialization，自身带内部标记 |
| `haystack/core/serialization.py` | `_is_serialized_component_device` | 按 `type` + 特定键的形状识别序列化设备 |
| `haystack/core/serialization.py` | `DeserializationCallbacks` | `component_pre_init` 注入点（可改 `init_params`） |
| `haystack/core/component/component.py` | `_hook_component_init` | `ContextVar` 传回调，`finally` 重置 |
| `haystack/utils/type_serialization.py` | `_import_class_by_name` | 白名单 → 属性穿越检查 → import → 二次模块检查 → 内部机制检查 → builtin 类型检查 |

## 参考

- [haystack 源码：serialization.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/serialization.py)
- [haystack 文档：序列化管道](https://docs.haystack.deepset.ai/docs/serialization)
- [haystack 文档：自定义组件](https://docs.haystack.deepset.ai/docs/custom-components)
- [Python 文档：inspect.signature](https://docs.python.org/3/library/inspect.html#inspect.signature)

## 与主核 / AI 的关系

构造式序列化与主核（多模态数据库内核）的**WAL 记录**是两条相反的思路。

主核的 WAL 记录的是**变更**（新元组的字节、被改动的页区间），目的是「重放后得到相同状态」。它不需要知道「这个元组是怎么构造的」，只要字节一致。haystack 记的是**构造参数**，目的是「重新构造一个相同的对象」。差别在于可重放性：WAL 记录是幂等的（重放两次结果相同），而重构造不是（构造函数有副作用时会执行两次）。

主核里选择「记变更」是因为**写入路径必须极快且必须精确**；haystack 选「记构造」是因为**组件的构造参数才是它的语义状态**——缓存、连接这些都该丢弃。两种都对，取决于「什么算对象的状态」。

第二处对照是 **`type` 字段的双重语义**，与主核的 `pg_class.relkind`（关系种类）类似：一个字段既标「这是什么种类的东西」又参与后续分派。主核用单字符编码（`r`=表、`i`=索引）并集中在一处判断；haystack 用字符串 + 「有没有点」的隐式判定，分散在三个分支里。前者显式，后者脆弱但省了一个字段。

第三处是最有启发的一处：**「先校验参数名再 import」**。主核在恢复 WAL 时也有一条类似的纪律——**先校验记录头的校验和与长度，再解析内容**。若先解析后校验，恶意或损坏的记录可能在拒绝之前就触发了越界读取。判据是一样的：**任何有副作用的步骤（解析、import、分配）都要排在校验之后。**

对 AI 工程的实际意义在于：组件的可序列化性是「管道能不能存盘复用」的前提，而它的坑几乎全在「反射」上。这份实现给出的三条经验可直接用：**反射要给出明确的修法**（`self.x = x` 或加 `to_dict`，错误消息里都写了）、**取不到值要区分「无默认」与「有默认」**（否则存档不完整会静默变成默认值）、以及**安全校验要排在 import 之前**（顺序本身就是边界）。
