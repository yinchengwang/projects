---
project: haystack
feature: marshal
title: YAML 编组与可调用对象序列化
page_type: feature
schema_version: 1
refs:
  - path: haystack/marshal/protocol.py
    symbols: [Marshaller]
  - path: haystack/marshal/yaml.py
    symbols: [YamlMarshaller, YamlDumper, YamlLoader]
  - path: haystack/utils/callable_serialization.py
    symbols: [serialize_callable, deserialize_callable]
---

# YAML 编组与可调用对象序列化

## 一句话职责

`Marshaller` 是「字典 ↔ 字符串」的一层极薄协议（两个方法），`YamlMarshaller` 是它唯一的实现（`SafeDumper`/`SafeLoader` 加一个 tuple 扩展）；`serialize_callable`/`deserialize_callable` 则把函数压成一个点号路径字符串，并在这条路径上复用 b03 的全部安全闸门。

## 机制是什么

这一页要回答两个独立的问题，它们被放在一起是因为**可调用对象的序列化结果最终要进 YAML**：

**问题一：字典怎么变成字符串？** 这层的设计选择是**把协议压到最小**——两方法，一个 `marshal(dict) -> str`，一个 `unmarshal(str|bytes|bytearray) -> dict`。协议没有任何错误处理的约定、没有版本号、没有流式接口。

**问题二：函数怎么序列化？** 函数是代码而不是数据，所以只能序列化成「引用」——一个能在重建时重新解析出同一个函数的字符串。haystack 的选择是 `module.__qualname__`，即点号路径。

第二个问题的关键约束是：**序列化容易，反序列化危险**。`"os.system"` 是个无害的字符串，解析它就等于拿到一个能执行任意命令的函数。所以 `deserialize_callable` 是整个框架里检查最密的一段代码——它在遍历路径的**每一跳**都做一次模块检查。

## 怎么实现的

**`Marshaller` 是一个 `Protocol`，只有两个方法，且文档字符串极短。**

```python
class Marshaller(Protocol):
    def marshal(self, dict_: dict[str, Any]) -> str:
        "Convert a dictionary to its string representation"
        ...

    def unmarshal(self, data_: str | bytes | bytearray) -> dict[str, Any]:
        """Convert a marshalled object to its dictionary representation"""
        ...
```

三处：

1. **`Protocol` 而非 ABC**。所以任何有这两个方法的对象都能当 Marshaller 用——不需要继承。这是结构化类型的用法，与 b02 里 `hasattr(obj, "to_dict")` 是同一思路。

2. **`unmarshal` 接受三种输入类型**（`str`/`bytes`/`bytearray`）。所以调用方不必先解码。`Pipeline.loads` 的签名也是这三者的联合。

3. **`marshal` 的返回类型固定 `str`**，而 `unmarshal` 接受三种。这个不对称是对的：产出规范化成文本，输入容忍字节。

注意第一个方法的文档字符串是**裸字符串字面量**而不是 `"""..."""`——它是「表达式语句」，在 `Protocol` 里不起 docstring 作用（`__doc__` 会是 `None`）。这是一个小的不一致（第二个方法用了三引号）。对 `Protocol` 来说不影响功能，但 `help()` 看到的是空的。

**`YamlMarshaller` 的两个方法各自把一种 YAML 异常翻译成 `TypeError`。**

```python
class YamlMarshaller:
    def marshal(self, dict_: dict[str, Any]) -> str:
        try:
            return yaml.dump(dict_, Dumper=YamlDumper)
        except yaml.representer.RepresenterError as e:
            raise TypeError(
                "Error dumping pipeline to YAML - Ensure that all pipeline components only serialize basic Python types"
            ) from e

    def unmarshal(self, data_: str | bytes | bytearray) -> dict[str, Any]:
        try:
            return yaml.load(data_, Loader=YamlLoader)
        except yaml.constructor.ConstructorError as e:
            raise TypeError(
                "Error loading pipeline from YAML - Ensure that all pipeline "
                "components only serialize basic Python types"
            ) from e
```

四处：

1. **`yaml.dump` 与 `yaml.load` 带显式的 `Dumper`/`Loader`**。这是安全关键：默认的 `yaml.load` 不做 `Loader` 参数会用一个**不安全的全功能 loader**，而这里显式传 `YamlLoader`（继承 `SafeLoader`）。所以这个文件的安全性依赖于「调用方没有绕过 `YamlMarshaller`」。

2. **捕获的异常很窄**：`RepresenterError` 与 `ConstructorError`。其他 YAML 异常（如 `ScannerError`——语法错误）会**原样传播**。而 `Pipeline.loads` 里包了一层宽捕获并统一转成 `DeserializationError`：

```python
try:
    deserialized_data = marshaller.unmarshal(data)
except Exception as e:
    raise DeserializationError(
        "Error while unmarshalling serialized pipeline data. This is usually "
        "caused by malformed or invalid syntax in the serialized representation."
    ) from e
```

   所以分层是：`YamlMarshaller` 把两类语义明确的错误转成 `TypeError`（提示「组件只能序列化基础 Python 类型」），`Pipeline.loads` 把其余一切（含语法错误）转成 `DeserializationError`。这个分工让 `TypeError` 那条消息能针对「组件写坏了」，而 `DeserializationError` 针对「文件坏了」。

3. **错误消息里「only serialize basic Python types」这个说法**，与 b02 的 `_validate_component_to_dict_output` 的白名单对应。所以一个组件若在 `to_dict` 里放了 `datetime`，会在两处之一被拦住：b02 的形状校验（更早、消息更精确）或这里的 `RepresenterError`。

4. **`raise ... from e`** 保留异常链。所以 Python traceback 里能看到原始 `RepresenterError`——这对排查「到底哪个值不能序列化」是必要的，因为 `TypeError` 的消息本身不含值信息。

**`YamlLoader`/`YamlDumper` 各自只加一条规则：tuple。**

```python
class YamlLoader(yaml.SafeLoader):
    def construct_python_tuple(self, node: yaml.SequenceNode) -> tuple:
        return tuple(self.construct_sequence(node))

class YamlDumper(yaml.SafeDumper):
    def represent_tuple(self, data: tuple) -> yaml.SequenceNode:
        return self.represent_sequence("tag:yaml.org,2002:python/tuple", data)

YamlDumper.add_representer(tuple, YamlDumper.represent_tuple)
YamlLoader.add_constructor("tag:yaml.org,2002:python/tuple", YamlLoader.construct_python_tuple)
```

四处：

1. **用 `SafeLoader`/`SafeDumper` 作为基类**。所以除 tuple 之外，一切「能构造任意 Python 对象」的 YAML 标签都被禁掉。这很关键：`yaml.Loader`（完整版）支持 `!!python/object:...` 这类标签，直接构造任意类——那等于把反序列化攻击面从「组件 type 字段」扩大到「YAML 本身」。

2. **tuple 是被单独放行的唯一类型**。理由能推出来：b02 的白名单里允许 `tuple`，所以组件可能把它写进 `init_parameters`；而 YAML 没有原生 tuple（只有 list），`SafeDumper` 遇到 tuple 会抛 `RepresenterError`。所以这个扩展是**为了让 b02 的允许类型真的能落盘**。

   注意用的是标准 YAML 标签 `tag:yaml.org,2002:python/tuple`，而不是自定义标签。这个标签名属于 YAML 的「python」扩展标签族——`SafeLoader` 默认拒绝整个族，这里只放行其中一个。

3. **`add_representer`/`add_constructor` 是类方法调用**，所以注册到子类上而不影响 `yaml.SafeDumper`。这是必要的：若注册到基类，整个进程里所有 `yaml.safe_dump` 都会受影响。

4. **loader 侧用 `construct_sequence(node)` 再转 `tuple`**——所以 YAML 里的序列先按通常规则构造（元素也按 `SafeLoader` 的规则），再整体转成 tuple。这意味着嵌套结构里 tuple 会正确递归。

**安全边界：`SafeLoader` 挡住了 YAML 本身的危险标签，但挡不住「合法 YAML 里的危险字符串」。**

一个 `!!python/object/apply:os.system` 标签会被 `SafeLoader` 拒绝（`ConstructorError`）。但 `type: os.system` 这个**普通字符串**不会——它需要 b03 的白名单来拦。所以 YAML 层与类型解析层是两道独立的闸：

| 层 | 拦什么 | 拦不住什么 |
|---|---|---|
| `SafeLoader` | YAML 的 `!!python/*` 标签 | 普通字符串里的危险类名 |
| b03 白名单 | `type` 字段里的类名 | 已经通过 YAML 构造的对象 |

这个分工在源码里没有明写，但它是成立的——两层都需要。

**`serialize_callable` 拒绝三类函数，理由各不相同。**

```python
def serialize_callable(callable_handle: Callable) -> str:
    try:
        full_arg_spec = inspect.getfullargspec(callable_handle)
        is_instance_method = bool(full_arg_spec.args and full_arg_spec.args[0] == "self")
    except TypeError:
        is_instance_method = False
    if is_instance_method:
        raise SerializationError("Serialization of instance methods is not supported.")

    qualname = getattr(callable_handle, "__qualname__", "")
    if "<lambda>" in qualname:
        raise SerializationError("Serialization of lambdas is not supported.")
    if "<locals>" in qualname:
        raise SerializationError("Serialization of nested functions is not supported.")
```

四处：

1. **实例方法**：判据是「第一个位置参数叫 `self`」。这是一个**启发式**而不是语义判定——一个普通函数的第一个参数恰好叫 `self` 会被误判为实例方法。但这个误判的代价是「拒绝序列化」而非「序列化错」，方向安全。

   为什么实例方法不行？因为它绑定到某个实例，而实例本身没有被序列化——重建时拿不到那个实例。所以只能拒绝。

2. **lambda**：`__qualname__` 里含 `<lambda>`。lambda 没有名字，无法被路径引用。

3. **嵌套函数**：`__qualname__` 里含 `<locals>`。嵌套函数的 `__qualname__` 形如 `outer.<locals>.inner`——它**不是模块属性**，所以路径解析不到。这与 b02 里「嵌套类因用 `__name__` 而丢失外层名」是同一类问题，但这里的处理更好：**显式检测并拒绝**，而不是产出错的名字。

   注意 lambda 也被 `<locals>` 检查覆盖了（模块级 lambda 的 qualname 是 `"<lambda>"`，函数内的是 `"outer.<locals>.<lambda>"`），但先检查 `<lambda>` 让错误消息更准确。

4. **`try/except TypeError` 包住 `getfullargspec`**：某些内建函数没有 `__argspec__`，会抛 `TypeError`。此时 `is_instance_method = False`，继续走后面的路径——因为内建函数是可序列化的（`builtins.print`）。

**路径的构造用 `__qualname__` 而不是 `__name__`。**

```python
name = qualname or callable_handle.__name__
module = inspect.getmodule(callable_handle)
if module is not None:
    full_path = f"{module.__name__}.{name}"
else:
    full_path = name
```

三处：

1. **`__qualname__` 包含类名**：`ClassName.method`。注释说明了理由：「`__qualname__` contains the fully qualified path we need for classmethods and staticmethods」。所以类方法/静态方法能序列化成 `module.Class.method`，反序列化时逐段 `getattr` 走通。

   这与 b02 的 `generate_qualified_class_name` 恰好相反——那里用 `__name__` 所以**嵌套类会丢**，这里用 `__qualname__` 所以**类方法能走**。两处选不同字段，因为目标不同：类要能被 `getattr(module, name)` 取到（`__name__` 即可，嵌套类则不行）；方法要能被逐段走过（需要 `QualName`）。

   所以 **b02 的嵌套类问题是真实的**，只是那里没被检测。

2. **`module is None` 时 `full_path = name`**（裸名）。然后反序列化时按裸名走——`_is_module_allowed("main")` 之类大概会失败。所以这是一个**会产出不可反序列化字符串**的分支。

3. **`inspect.getmodule` 用 `__module__` 反查**。所以若一个函数被动态赋了 `__module__`，路径会跟着变。

**`serialize_callable` 对危险内建只警告不拒绝。**

```python
if _is_denied_builtin(callable_handle):
    logger.warning(
        "Serialized callable '{full_path}' is a builtin that is blocked during deserialization; "
        "the resulting pipeline will only be loadable with unsafe=True.",
        full_path=full_path,
    )
```

三处：

1. **只 `warning` 不 `raise`**。所以 `serialize_callable(eval)` 会成功产出一个字符串，但那个管道**永远无法在安全模式下加载**。这是一个有意的选择：序列化侧不阻止（用户可能确实要这么做），但把后果写清楚。

2. **警告消息直接说出后果**：「the resulting pipeline will only be loadable with `unsafe=True`」。这比抛错信息量大——用户可能就是想生成一个「必须 unsafe 才能加载」的文件。

3. **`_is_denied_builtin` 从安全模块 import 过来复用**。所以「什么算危险内建」只有一份定义，序列化侧与反序列化侧共享。这个复用是必要的——否则两处名单会漂移。

**`deserialize_callable` 的解析策略是「从最长的前缀往下试」。**

```python
parts = callable_handle.split(".")
for i in range(len(parts), 0, -1):
    module_name = ".".join(parts[:i])
    if not _is_module_allowed(module_name):
        continue
    try:
        mod: Any = thread_safe_import(module_name)
    except Exception:
        continue
    ...
```

三处：

1. **`range(len(parts), 0, -1)` 从最长前缀开始**。`"json.dumps"` 先试 `json.dumps`（作为模块，失败）再试 `json`（成功）。所以它能自动区分「`json.dumps` 里 `json` 是模块、`dumps` 是属性」与「`a.b.c` 里 `a.b` 是模块、`c` 是属性」。

2. **`_is_module_allowed` 在 import 之前检查**。注释说明了理由：「Gating the import (rather than a mere string prefix of the handle) means a disallowed module is never imported for its side effects, and the resolver can only ever start from a trusted module」。

   注意这个 `continue` 而不是抛错——因为一个长前缀可能不在白名单里，但短前缀在。所以「跳过不可信前缀」是策略的一部分。

3. **`except Exception: continue`** 捕获 import 的一切失败。所以「模块不存在」与「模块存在但 import 时抛错」被同样处理——后者会导致**该模块的顶层代码执行了一半**。这是一个真实的风险点：`import` 是有副作用的，而 `continue` 掩盖了「副作用已经发生」。

**遍历每一跳都做一次模块检查，这是本页最值得记的一处。**

```python
attr_value = mod
for part in parts[i:]:
    _check_traversable_attribute(part, callable_handle)
    try:
        attr_value = getattr(attr_value, part)
    except AttributeError as e:
        container = getattr(attr_value, "__name__", type(attr_value).__name__)
        raise DeserializationError(f"Could not find attribute '{part}' in {container}") from e
    _check_resolved_module_allowed(attr_value, declared_module=module_name)
```

四处：

1. **`_check_traversable_attribute` 在 `getattr` 之前**——所以 `__globals__` 这类名字根本没被取到。这是「校验先于副作用」的又一处。

2. **`_check_resolved_module_allowed` 在 `getattr` 之后、且在循环内**——所以**每一跳的对象都被检查**。

   注释给出了具体攻击：一个白名单的 haystack 模块重新导出了 `rich.console.Console`，句柄走 `Console._environ.update`，终点是 `collections.abc.MutableMapping.update`（`collections` 在白名单里），于是**终点检查会通过**，而中间那一跳 `rich.console.Console` 属于未白名单的 `rich`。

   所以「只检查终点」不够——中间跳本身就是逃逸。这与 b03 里 `_check_resolved_module_allowed` 的设计意图一致，但**逐跳检查**是这里独有的（b03 的 `_import_class_by_name` 只检查终点，因为它只有一段属性名）。

3. **`except AttributeError` 时给出容器名**：`getattr(attr_value, "__name__", type(attr_value).__name__)`。所以错误消息形如「Could not find attribute 'foo' in json」——定位到具体那一跳。

4. **`declared_module=module_name` 传的是「整个起点的模块」**，而不是当前跳的模块。所以 b03 里那个「私有加速模块豁免」在这个路径上要求 `module_name.lstrip("_") == 起点的模块`——对一个多跳句柄来说，这个豁免基本不会命中。这是一个保守的结果（更容易拒绝）。

**`deserialize_callable` 在终点做五道检查，顺序有讲究。**

```python
if isinstance(attr_value, (classmethod, staticmethod)):
    attr_value = attr_value.__func__
if isinstance(attr_value, Tool):
    attr_value = attr_value.function or attr_value.async_function
if isinstance(attr_value, FunctionHook):
    attr_value = attr_value.function or attr_value.async_function
if not callable(attr_value):
    raise DeserializationError(f"The final attribute is not callable: {attr_value}")

_check_resolved_module_allowed(attr_value, declared_module=module_name)
_check_not_denied_builtin(attr_value, callable_handle)
_check_not_denied_callable(attr_value, callable_handle)
_check_not_deserialization_internal(attr_value, callable_handle)
```

四处：

1. **三段「解包」先做**：`classmethod`/`staticmethod` → `__func__`；`Tool` → `function` 或 `async_function`；`FunctionHook` → 同理。

   这三段解决的是「装饰器把函数换成了对象」的问题。`@tool` 与 `@hook` 装饰器返回的不是函数，而是一个对象；若不解包，`callable(attr_value)` 可能是真（`Tool` 有 `__call__`？），但拿回来的是对象而不是用户写的函数。

   注意 **`Tool`/`FunctionHook` 的 import 在函数内部**（`from haystack.tools.tool import Tool`），注释说「Import here to avoid circular imports」。这是 Python 里处理循环依赖的常规手法，代价是每次调用都要走一次 import 查找（虽然有 `sys.modules` 缓存）。

2. **解包在前、检查在后**。所以检查的是**解包后的函数**而不是 `Tool` 对象本身。这是必要的：`_check_resolved_module_allowed` 要看的是那个函数的真实模块。

   但这也留下一个缝隙：**`Tool` 对象的来源没有被检查**。若一个白名单模块导出了一个从非白名单模块来的 `Tool`，解包后检查的是用户函数的模块——而那个 `Tool` 对象的模块从未被检查。这是个真实的边界。

3. **`not callable(attr_value)` 才抛错** —— 所以「走到了一个不是可调用对象的东西」是明确的错误。而 `deserialize_callable` 的契约就是「返回一个 callable」。

4. **四道检查的顺序**：模块 → 内建黑名单 → 导入原语黑名单 → 控制面。

   为什么这个顺序？因为检查的「精密度」递增：模块检查最粗（会误拒私有加速模块，所以有豁免），内建黑名单最具体。若顺序反过来（先查黑名单），一个危险的内建会先被拦，但那不影响结果。所以**顺序在功能上不敏感**——但注释里每道检查都解释了「为什么前面那道不够」，这说明顺序是按「哪道能拦」组织的。

   而 `_check_resolved_module_allowed` 出现了两次（循环内每跳一次 + 终点一次）。终点那次是必要的，因为注释说「This catches a dangerous callable bound as a plain (non-module) attribute of an allowlisted object, which the module-walk check above would not see」。即：最后一跳可能是上一跳对象上的一个属性，而那个属性**没有经过循环内的检查**（循环内检查的是 `attr_value`，而最后一跳之后才检查它本身——实际上循环内已经检查了，见下）。

   仔细看：循环 `for part in parts[i:]` 里，每次 `getattr` 之后都调 `_check_resolved_module_allowed(attr_value, ...)`，所以**最后一跳的对象已经在循环内被检查过**。那么循环外的这道检查是**冗余的**。

   这大概是从「只检查终点」的旧版本演化到「逐跳检查」后留下的残留。不影响正确性，但是一处重复。

**失败路径的兜底也值得看。**

```python
# 循环走完还没 return
_check_module_allowed(callable_handle)
raise DeserializationError(f"Could not import '{callable_handle}' as a module or callable.")
```

两处：

1. **`_check_module_allowed(callable_handle)`** 用**整个句柄**当模块名检查。这几乎必然失败（`"json.dumps"` 不是模块），于是会抛出带「三条扩展途径」的标准白名单错误。

   注释说明了意图：「Surface the standard allowlist error when the top-level module is untrusted; otherwise report a plain resolution failure」。但代码里并没有区分——它无条件调 `_check_module_allowed`，所以：
   - 若整个句柄恰好是白名单里的模块 → 通过，继续抛下面的 `DeserializationError`
   - 否则 → 抛白名单错误

   所以形状上像是「顶层模块不可信时给白名单错误」，但实际上判据是「整个句柄是不是一个被允许的模块名」。

2. **`raise DeserializationError(f"Could not import '{callable_handle}' as a module or callable.")`** —— 只有当上面那句通过时才到达（即句柄本身是个允许的模块名，例如 `serialize_callable` 产出了 `"haystack"` 这样的裸模块名，或 `module is None` 的兜底分支产出了裸名）。

**`marshal/__init__.py` 用 `LazyImporter` 把 import 推迟。**

```python
_import_structure = {"protocol": ["Marshaller"], "yaml": ["YamlMarshaller"]}

if TYPE_CHECKING:
    from .protocol import Marshaller as Marshaller
    from .yaml import YamlMarshaller as YamlMarshaller
else:
    sys.modules[__name__] = LazyImporter(name=__name__, module_file=__file__, import_structure=_import_structure)
```

三处：

1. **`TYPE_CHECKING` 分支给类型检查器看真实 import**，运行期走 `LazyImporter`。所以静态检查与运行期看到的是不同的东西——这是 `lazy_imports` 这个包的标准用法。

2. **推迟的理由是 `yaml` 是可选依赖**。`haystack/marshal/yaml.py` 顶层 `import yaml`，若 `yaml` 没装，任何 `from haystack.marshal import Marshaller` 都会失败——即使调用方只用 `Marshaller` 协议。`LazyImporter` 让「只用协议」的路径不必装 yaml。

3. **`as Marshaller` 的自我别名**（与 b02 里 `allow_deserialization_module as allow_deserialization_module` 同一手法）标记「有意重导出」，避免 lint 报 F401。

## 关键取舍

**源码事实：** `Marshaller` 是 `Protocol`（非 ABC），只有 `marshal(dict) -> str` 与 `unmarshal(str|bytes|bytearray) -> dict` 两个方法；第一个方法的文档字符串是裸字符串字面量（对 `Protocol` 不起 `__doc__` 作用），第二个用了三引号——不一致但不影响功能。

**源码事实：** `YamlMarshaller` 只捕获 `RepresenterError`（dump）与 `ConstructorError`（load）转成 `TypeError`，其余 YAML 异常（如语法 `ScannerError`）由 `Pipeline.loads` 的宽捕获统一转成 `DeserializationError`；错误消息都提示「组件只能序列化基础 Python 类型」，与 b02 的允许类型白名单对应。

**源码事实：** `YamlLoader`/`YamlDumper` 继承 `SafeLoader`/`SafeDumper`（拒绝一切 `!!python/*` 标签），只为 tuple 注册标准标签 `tag:yaml.org,2002:python/tuple`。tuple 被单独放行的原因是 b02 把 `tuple` 列为允许类型，而 YAML 无原生 tuple。

**源码事实：** `add_representer`/`add_constructor` 注册在子类上，所以不影响进程内其他 `yaml.safe_dump` 调用。

**源码事实：** `serialize_callable` 拒绝三类：实例方法（判据是「第一个位置参数名为 `self`」，是启发式）、lambda（`<lambda>` 在 `__qualname__`）、嵌套函数（`<locals>` 在 `__qualname__`）；`getfullargspec` 抛 `TypeError` 时按「非实例方法」继续（内建函数没有 `__argspec__`）。

**源码事实：** 路径用 `__qualname__` 而非 `__name__`，注释理由是需要支持 classmethod/staticmethod——与 b02 的 `generate_qualified_class_name` 恰好相反（那里用 `__name__`，故嵌套类无法往返）。

**源码事实：** `serialize_callable` 对危险内建**只 warning 不 raise**，警告里写清后果（「the resulting pipeline will only be loadable with `unsafe=True`」），并复用安全模块的 `_is_denied_builtin` 保证名单唯一。

**源码事实：** `deserialize_callable` 从**最长前缀**往下试模块名（`range(len(parts), 0, -1)`），先在 import 之前过白名单、失败则 `continue` 试更短的前缀；`except Exception: continue` 会掩盖「模块 import 时抛错导致的半执行副作用」。

**源码事实：** 遍历路径的**每一跳**都在 `getattr` 前做 `_check_traversable_attribute`、之后做 `_check_resolved_module_allowed`——注释给出的理由是「只查终点」会漏掉「中途经过未白名单模块再落到白名单模块」的句柄。

**源码事实：** 终点先做三段解包（`classmethod`/`staticmethod` → `__func__`，`Tool`/`FunctionHook` → `function`/`async_function`），再依次检查模块、内建黑名单、导入原语黑名单、控制面；`Tool`/`FunctionHook` 用函数内 import 避免循环依赖。循环内已检查过终点对象，循环外那道模块检查是重复的。

**源码事实：** 失败兜底无条件 `_check_module_allowed(callable_handle)`（整个句柄当模块名），因此形状上像「顶层模块不可信时给标准白名单错误」，实际判据是「整个句柄是不是一个被允许的模块名」。

**源码事实：** `marshal/__init__.py` 用 `LazyImporter` 推迟 `yaml` 的 import（`yaml` 是可选依赖），`TYPE_CHECKING` 分支给静态检查器真实 import，`as` 自我别名标记有意重导出。

**架构推论：** `Marshaller` 协议被压到两方法这个选择，让「换格式」变得极便宜（实现两个方法即可），代价是**协议放弃了所有质量保证**：没有错误类型约定、没有版本、没有「保留注释/顺序」这类格式特有需求的位置。对一个「管道存盘」的场景这够用，因为存盘是一次性全量写入（不需要增量、不需要保留人写的内容）。

**架构推论：** `SafeLoader` 与 b03 的白名单是**两道独立且互补的闸**：前者拦 YAML 标签（`!!python/object/apply:os.system`），后者拦普通字符串里的类名（`type: os.system`）。这两层的必要性可以从两侧各自的盲区看出来——若只有白名单，一个 YAML 标签就能在**反序列化开始前**构造出任意对象；若只有 `SafeLoader`，`type` 字段里的字符串完全不受约束。所以「安全需要多层」在这件事上有具体的形状：每层守一类语法位置。

**架构推论：** `deserialize_callable` 逐跳检查是本页最重的一处设计。它承认了「只检查终点不够」这个事实，而理由很具体：`getattr` 的中间结果本身可能是逃逸工具（一个重新导出的类），终点合法不代表路径合法。这与主核里「视图展开的每一层都要重新做权限检查」是同一个道理——**权限检查必须跟着解析过程走，不能只看结果**。

**架构推论：** `serialize_callable` 对危险内建「只警告」是个有意思的选择。它区分了「能不能表达」与「能不能安全加载」两件事：`serialize_callable(eval)` 是一种**忠实的表达**（这个管道确实用了 `eval`），拒绝它会让用户无法表达真实状态；而加载时的拒绝已经足够。把「这文件只能 unsafe 加载」写进警告，属于「让用户知道后果」而不是「替用户决定」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/marshal/protocol.py` | `Marshaller` | 两方法的 `Protocol`（`marshal` / `unmarshal`） |
| `haystack/marshal/yaml.py` | `YamlMarshaller` | 唯一实现，两类 YAML 异常转 `TypeError` |
| `haystack/marshal/yaml.py` | `YamlLoader` | 继承 `SafeLoader`，只为 tuple 加一条 constructor |
| `haystack/marshal/yaml.py` | `YamlDumper` | 继承 `SafeDumper`，tuple 用标准 python/tuple 标签 |
| `haystack/marshal/__init__.py` | `_import_structure` / `LazyImporter` | 推迟 `yaml` 的 import（可选依赖） |
| `haystack/core/pipeline/base.py` | `DEFAULT_MARSHALLER` | 模块级单例 `YamlMarshaller()` |
| `haystack/core/pipeline/base.py` | `Pipeline.dumps` / `dump` / `loads` / `load` | 四个编组入口，`loads` 把剩余异常转 `DeserializationError` |
| `haystack/utils/callable_serialization.py` | `serialize_callable` | 拒绝实例方法/lambda/嵌套函数；危险内建只警告 |
| `haystack/utils/callable_serialization.py` | `deserialize_callable` | 最长前缀试模块 + 逐跳检查 + 终点四道闸 |
| `haystack/utils/callable_serialization.py` | 三段解包段 | `classmethod`/`staticmethod`/`Tool`/`FunctionHook` 解到函数 |
| `haystack/core/serialization_security.py` | `_is_denied_builtin` | 被序列化侧复用，保证危险内建名单唯一 |

## 参考

- [haystack 源码：marshal/yaml.py](https://github.com/deepset-ai/haystack/blob/main/haystack/marshal/yaml.py)
- [haystack 源码：utils/callable_serialization.py](https://github.com/deepset-ai/haystack/blob/main/haystack/utils/callable_serialization.py)
- [PyYAML 文档：SafeLoader 与标签](https://pyyaml.org/wiki/PyYAMLDocumentation)
- [Python 文档：inspect.getfullargspec](https://docs.python.org/3/library/inspect.html#inspect.getfullargspec)

## 与主核 / AI 的关系

「字典 ↔ 字符串」这一层与主核（多模态数据库内核）的**输出格式**（`COPY ... WITH (FORMAT csv)` 那类）同构：都是把一个结构化结果渲染成文本，且需要保证能读回来。主核的 CSV 输出要处理「字段含分隔符」「NULL 与空串的区分」这些具体问题；YAML 用引号与标签把这些都标准化了，所以 haystack 这一层的代码量极小（45 行）。

真正的对照价值在**安全边界的形状**。主核的 `COPY` 输入不做「按名字构造任意类」这种事——它只解析数据，不解析代码引用。而 haystack 的这条路必须解析代码引用，于是把安全问题从「数据注入」升级成了「代码注入」。这个差别的根源是**持久化对象里要不要含代码引用**：主核的元组只存数据类型（`int4`、`text`），是封闭集合；组件的 `init_parameters` 里可以放任意 callable，是开放集合。

第二处对照是 **`SafeLoader` 与主核的类型检查**。两者都是「在解析阶段就拒绝不合规输入」，而不是「先接受再验证」。主核读 CSV 时会先做类型校验再入堆（脏数据不进表）；YAML 的 `SafeLoader` 在构造阶段就拒绝未注册的标签。判据相同：**越早拒绝，攻击面越小**。

第三处是 **逐跳检查与主核的权限检查**。主核里视图（view）展开时每一层都要检查权限（`SELECT` 一个视图不绕过底层表的权限）。这与 `deserialize_callable` 逐跳调用 `_check_resolved_module_allowed` 是同一个理由：**中间结果不能比终点更宽松**。

对 AI 工程的实际意义在于：可调用对象序列化让「配置里的回调」（`streaming_callback`、`embedding_fn`、Jinja 过滤器）能存进 YAML，这是很实用的能力，但它把 YAML 变成了代码入口。这份实现给出的三条经验可直接用：**编组层用 `SafeLoader` 而不是 `Loader`**（一个字的差别决定攻击面）、**解析路径时每一跳都要检查**（只查终点会漏「中途经过」的逃逸）、以及**无法表达的东西要明确拒绝并说明原因**（lambda 与嵌套函数因为不是模块属性而无法被路径引用）。
