---
project: haystack
feature: sockets
title: 输入输出 socket
page_type: feature
schema_version: 1
refs:
  - path: haystack/core/component/sockets.py
    symbols: [Sockets]
  - path: haystack/core/component/types.py
    symbols: [InputSocket, OutputSocket, InputSocketTypeDescriptor]
---

# 输入输出 socket

## 一句话职责

`InputSocket` / `OutputSocket` 是组件端口的**值类型**（名字、类型、默认值、可变性、连接方），`Sockets` 是它们的容器并同时充当**属性访问代理**（`component.__haystack_input__.question` 能直接拿到 socket）；三个字段——`default_value`（决定必填）、`is_lazy_variadic` / `is_greedy`（决定可变语义）、`type`（决定接线时的类型兼容）——承载了整套数据流契约的全部可变面。

## 机制是什么

socket 这东西看起来只是「一个带名字的端口」，但它在这里承担了三件互不相干的事：

1. **必填性**。`default_value` 是否等于 `_empty` 哨兵，决定了这个输入能不能省略。**必填性不是独立字段，是从「有没有默认值」推出来的**——一个字段承载两个语义。
2. **可变性（arity）**。一个 socket 能接几个上游？普通 socket 只接一个；`Variadic[T]` 接多个，且分成**懒惰**（等所有上游都产出）与**贪婪**（来一个就跑）两种。这个区别不是配置项，而是**类型注解的一部分**（用 `Annotated` 的 metadata 标记）。
3. **类型**。用于接线时的兼容检查（b01 的主题），以及 `__repr__` 的可读展示。

`Sockets` 这个容器本身几乎没有逻辑，它的巧妙之处在 `__init__` 的两次赋值：

```
self._sockets_dict = sockets_dict
self.__dict__.update(sockets_dict)
```

**把字典同时登记为「内部有序字典」与「实例属性」**。于是 `sockets.question` 与 `sockets["question"]`（通过 `__setitem__`）都可用，而 `_sockets_dict` 保证了遍历顺序稳定（Python 3.7+ 的 dict 有序）。

这个双写设计带来一个必须维护的一致性要求：`__setitem__` 必须同时写两处。

```
def __setitem__(self, key, socket):
    self._sockets_dict[key] = socket
    self.__dict__[key] = socket
```

注释说明「这应该在 `component` 装饰器里发生」——也就是说运行期不应再增删 socket，`__setitem__` 只是构造期的一个便利。

## 怎么实现的

**`_empty` 是一个空类，专门用来区分「没设默认值」与「默认值是 None」。**

```
class _empty:
    """Custom object for marking InputSocket.default_value as not set."""
```

**为什么不能直接用 `None`？** 因为「默认值是 `None`」是一个合法且常见的声明（例如 `def run(self, query: str | None = None)`）。那意味着这个输入**不是必填的**、缺省时传 `None`。若用 `None` 表示「未设置」，这个声明就无法表达。

而 `is_mandatory` 的写法揭示了一个更细的点：

```
@property
def is_mandatory(self) -> bool:
    """Check if the input is mandatory."""
    # Identity, so a default with a custom `__eq__`, such as a DataFrame, is never compared to the sentinel.
    return self.default_value is _empty
```

**用 `is` 而不是 `==`，注释给了具体理由**：默认值可能是 `DataFrame` 这类自定义 `__eq__` 的对象，而它的 `__eq__` 返回的不是布尔（返回逐元素比较的 DataFrame）。`self.default_value == _empty` 会调用 `DataFrame.__eq__(_empty)`，得到一个 DataFrame，再被 `if` 求值时会抛「truth value is ambiguous」。**这是一个在实践中会真实爆炸的陷阱**，与 a03 里 `_NoOutputProduced` 必须用 `isinstance` 判是同一条纪律。

**可变性编码在 `Annotated` 的 metadata 里，靠两个字符串常量识别。**

```
HAYSTACK_VARIADIC_ANNOTATION = "__haystack__variadic_t"
HAYSTACK_GREEDY_VARIADIC_ANNOTATION = "__haystack__greedy_variadic_t"

Variadic: TypeAlias = Annotated[Iterable[T], HAYSTACK_VARIADIC_ANNOTATION]
GreedyVariadic: TypeAlias = Annotated[Iterable[T], HAYSTACK_GREEDY_VARIADIC_ANNOTATION]
```

注释说得很直白：「这个类型除了标记包含的类型之外什么都不做」。于是 `Variadic[int]` 在运行期就是 `Annotated[Iterable[int], "__haystack__variadic_t"]`，`__post_init__` 通过读 `__metadata__` 把它认出来：

```
try:
    self.is_lazy_variadic = (
        hasattr(self.type, "__metadata__") and self.type.__metadata__[0] == HAYSTACK_VARIADIC_ANNOTATION
    )
    self.is_greedy = (
        hasattr(self.type, "__metadata__") and self.type.__metadata__[0] == HAYSTACK_GREEDY_VARIADIC_ANNOTATION
    )
except AttributeError:
    self.is_lazy_variadic = False
    self.is_greedy = False
```

四点：

1. **`self.type.__metadata__[0]` 取第一个元素**，也就是只认一个标记。若用户写 `Annotated[int, "a", "b"]`，只看 `"a"`。
2. **用字符串而不是类作标记**，于是这个标记可以被序列化（写进 YAML 再读回来仍然相等）。若用类对象作标记，跨进程序列化就会失效。
3. **整个 try/except 包住两个赋值**：任何一步 AttributeError（`type` 不是 `Annotated`、`__metadata__` 为空元组导致索引越界不会触发 AttributeError 但 `hasattr` 为假）都会把两个标志都置假。**这个兜底是有意的**：「认不出来就当普通 socket」，而不是让组件因为一个注解格式问题无法构造。
4. **懒惰与贪婪互斥**（两个标记不同），但代码没有显式检查「不能同时为真」——因为同一个 `__metadata__[0]` 不可能等于两个不同的字符串。

**`__post_init__` 还会把类型「拆」一层，这段注释写得极详细。**

```
if self.is_lazy_variadic or self.is_greedy:
    outer_args = get_args(self.type)
    inner_type = outer_args[0]
    inner_args = get_args(inner_type)
    if not inner_args:
        raise ComponentError(
            f"Variadic input '{self.name}' must have a type argument, e.g. Variadic[int]. "
            f"Got bare {inner_type!r} without a type argument."
        )
    self.type = inner_args[0]
```

两段注释解释了为什么要拆两次：

> **Note1**: Variadic 是单一类型的注解，所以 `get_args` 的返回值永远是长度为 1 的 tuple。
>
> **Note2**: 管道在组件输入声明为 `Variadic` 时总是传一个列表，所以这个类型本身永远包着一个 iterable。例如 `Variadic[int]` 最终是 `Iterable[int]` 的别名。因为我们关心的是内层类型 `int`，所以调两次 `get_args`：第一次从 `Variadic` 拿到 `list[int]`，第二次从 `list[int]` 拿到 `int`。

两处细节：

- **`self.type` 被就地改写**成内层类型（`int`），于是后续的类型兼容检查（b01）比较的是「单元素类型」而不是「iterable 类型」。这是必要的——管道写进 socket 时是逐值写，比较时也应当逐值比。
- **`if not inner_args: raise ComponentError`** 覆盖「裸 `Variadic`」（不带类型参数）的情况。注意报错里用 `inner_type!r` 而不是 `self.type!r`，因为此时 `self.type` 已经被上层拿到的是 outer args。消息给出正确写法 `Variadic[int]`，这是「错误消息要给出修法」的又一例（与 a01 的签名差异报告同源）。

**`InputSocketTypeDescriptor` 是一个只有两个键的 `TypedDict`。**

```
class InputSocketTypeDescriptor(TypedDict):
    type: type | UnionType
    is_mandatory: bool
```

它的用途是把「接线时需要知道的全部信息」压缩成两个字段。**注意它故意不含 `name`、`default_value`、`is_lazy_variadic`** —— 也就是说这个描述符的消费者（类型兼容检查）只关心「什么类型」与「必填吗」。这个裁减是有意的：**描述符的字段越少，能被复用的场景越多**，且不会有人从描述符里读到不该依赖的东西。

`is_mandatory` 出现在描述符里很关键：类型兼容不仅看类型，还要看**可选性是否兼容**——一个「可选输出」不能喂给「必填输入」，否则输入缺失就成了必然。

**`OutputSocket` 比 `InputSocket` 简单一个量级。**

```
@dataclass
class OutputSocket:
    name: str
    type: type
    receivers: list[str] = field(default_factory=list)
```

三个字段：名字、类型、接收方列表。**没有任何可变性标记**——因为「一个输出能发给几个接收方」这件事在 haystack 里是自由的（一个输出可以扇出到多个下游），不需要声明。也没有默认值概念（输出不存在「缺省」）。

`type` 的注解是 `type` 而不是 `type | UnionType`，比 `InputSocket` 的窄——但这是注解层面的近似，实际 `OutputSocket` 也可以持有 `UnionType`（例如 `@component.output_types(out=int | str)`）。**这个不一致是注解与实现之间的真实缝隙，非致命但会在类型检查器严格时暴露。**

`Sockets` 的 `__eq__` 说明了两侧 socket 如何比较：

```
def __eq__(self, value):
    if not isinstance(value, Sockets):
        return False
    return (
        self._sockets_io_type == value._sockets_io_type
        and self._component == value._component
        and self._sockets_dict == value._sockets_dict
    )
```

三个条件：io 类型相同（不能拿输入 socket 容器和输出 socket 容器比）、**组件相同**、字典相同。

**「组件相同」这一条很关键，也是一个隐蔽的陷阱。** 它意味着**两个不同组件实例的 `Sockets` 永不相等，即使 socket 定义完全一样**。a01 里 `_parse_and_set_input_sockets` 用 `async_run_sockets != run_sockets` 判断 `run` 与 `run_async` 是否一致，而那里两个 `Sockets` 都是 `Sockets(instance, {}, InputSocket)` 造的——**同一个 instance**，所以比较才有意义。若某处不小心用不同 instance 造了两个 `Sockets` 来比，就会永远不等。

`_component` 的比较用的是 `==`，而 `Sockets.__eq__` 又比较 `_component` ——若 `Component` 类的 `__eq__` 恰好也依赖 socket 比较，就会无限递归。`PipelineBase` 的 `__eq__` 确实比较 graph，而 graph 里存着组件实例；`component.py` 没有为组件定义 `__eq__`，于是走默认的身份比较。**这条链条能成立，靠的是「组件不重写 `__eq__`」这个未成文的约定。**

**`_sockets_io_type` 只影响 `__repr__`，构造函数注释把这一点说清了。**

```
def __init__(self, component, sockets_dict, sockets_io_type):
    """
    We don't do any enforcement on the types of the sockets here, the `sockets_type` is only used for
    the `__repr__` method.
    We could do without it and use the type of a random value in the `sockets` dict, but that wouldn't
    work for components that have no sockets at all. Either input or output.
    """
```

**「没有 socket 的组件」是这里必须有一个显式类型参数的原因**：若靠「取字典里第一个值的类型」推断这是输入容器还是输出容器，一个零 socket 的组件就无从判断。这是一个很小的设计点，但它解释了为什么一个「只用于 repr」的参数必须存在。

**`__repr__` 的实现有一个必须绕开的递归。**

```
def _component_name(self) -> str:
    if pipeline := self._component.__haystack_added_to_pipeline__:
        return pipeline.get_component_name(self._component)
    # This Component has not been added to a Pipeline yet, so we can't know its name.
    # Let's use default __repr__. We don't call repr() directly as Components have a custom
    # __repr__ method and that would lead to infinite recursion since we call Sockets.__repr__ in it.
    return object.__repr__(self._component)
```

**注释明确说明：不能调 `repr(self._component)`，因为组件的 `__repr__`（a01 的 `_component_repr`）会打印 socket，而打印 socket 又会打印组件名——无限递归。** 于是这里显式调 `object.__repr__` 绕过自定义的 `__repr__`。

这是一条「两个类的 `__repr__` 互相引用」时唯一正确的解法：**在其中一侧显式绕到你想要的基类实现**。若没有这条注释，下一个人很可能会「优化」成 `repr(...)` 并在打印一个未入管道的新组件时踩进 `RecursionError`。

`__repr__` 本身是逐行列出 socket 名与类型：

```
result = ""
if self._sockets_io_type == InputSocket:
    result = "Inputs:\n"
elif self._sockets_io_type == OutputSocket:
    result = "Outputs:\n"
return result + "\n".join([f"  - {n}: {_type_name(s.type)}" for n, s in self._sockets_dict.items()])
```

**类型名走 `_type_name` 而不是 `str(type)`**，因为后者会给出 `typing.Union[int, str]` 这类带 `typing.` 前缀且冗长的形式。`_type_name` 专门处理了 `UnionType`（用 ` | ` 连接）、`Literal`（把字符串参数加引号）、`NoneType`（显示成 `None`），并剥掉 `typing.` 前缀与泛型参数。**这个函数存在的唯一目的就是让人读到能看懂的类型名**——一个纯粹为可观测性服务的工具。

**`is_variadic` 是派生属性，不是存储字段。**

```
@property
def is_variadic(self) -> bool:
    return self.is_greedy or self.is_lazy_variadic
```

于是「是否可变」永远与两个具体标志一致，不可能出现「`is_variadic=True` 但两个标志都假」的非法状态。**用派生属性代替冗余字段是这类「有主从关系」的标志位的标准做法**：单一真相来源（两个具体标志），派生值只读。

而 `is_lazy_variadic` 与 `is_greedy` 用 `field(init=False)` 声明：

```
is_lazy_variadic: bool = field(init=False)
is_greedy: bool = field(init=False)
```

`init=False` 意味着它们**不在 `__init__` 参数里**，只能在 `__post_init__` 里被赋值。这样就不可能有人构造出一个「标着 lazy 但类型是 `int`」的 socket。**这是一条用 dataclass 机制强制的约束，比文档约定可靠。**

**`senders` / `wrap_input_in_list` / `receivers` 三个字段是「连接期状态」，不是「声明期状态」。**

```
senders: list[str] = field(default_factory=list)        # InputSocket
wrap_input_in_list: bool = True                          # InputSocket
receivers: list[str] = field(default_factory=list)       # OutputSocket
```

它们由 `Pipeline.connect()` 填充。`wrap_input_in_list` 值得单独说：它只在 `is_lazy_variadic` 为真时有意义（字段注释也这么写），决定管道把多个上游的值交给组件时**是否再包一层 list**。

`_make_socket_auto_variadic` 揭示了它的一处自动翻转：

> When auto-variadicity is applied, `wrap_input_in_list` is also set to `False` so that sender output types match the receiver socket's declared list type directly.

也就是说，当一个声明为 `list[int]` 的普通 socket 被接了第二个上游时，它**被自动升级成懒惰可变**，同时 `wrap_input_in_list` 置假——因为声明类型本身已经是 `list`，每个上游各给一个 `list`，应当把多个 list **摊平合并**而不是「装进去一个 list 的 list」。

对应地 `_consume_component_inputs` 里有两支不同的取值方式：

```
if socket.is_lazy_variadic:
    if socket.wrap_input_in_list:
        # 保持包在 list 里
        consumed_inputs[socket_name] = socket_inputs_values
    else:
        # 摊平一层 list，让传入形状匹配 socket 声明的类型
        consumed_inputs[socket_name] = list(itertools.chain.from_iterable(socket_inputs_values))
```

**这两个字段的组合把一个「语义歧义」变成了显式标志**：`Variadic[list[int]]`（每个上游给一个 list，我要 list 的 list）与 `list[int]`（每个上游给一个 list，我要把它们拼起来）在运行期都需要「攒多个」，但攒完的形状不同。用 `wrap_input_in_list` 表达这个差异，比让组件自己判断要清楚得多。

## 关键取舍

**源码事实：** `default_value` 默认值是 `_empty` 哨兵类实例；`is_mandatory` 用 `is` 判等，注释给出理由是「像 DataFrame 这样自定义 `__eq__` 的默认值不能与哨兵做 `==` 比较」。

**源码事实：** 可变性编码在 `Annotated[Iterable[T], "<字符串标记>"]` 的 metadata 里，标记是两个模块级字符串常量；`__post_init__` 读 `__metadata__[0]` 识别，任何 `AttributeError` 都退回「普通 socket」。

**源码事实：** `__post_init__` 会把可变 socket 的 `type` **就地改写**成内层类型（`Variadic[int]` → `int`），并调两次 `get_args` 完成两层解包；裸 `Variadic`（无类型参数）抛 `ComponentError` 且消息给出正确写法。

**源码事实：** `is_lazy_variadic` 与 `is_greedy` 用 `field(init=False)` 声明，只能在 `__post_init__` 中被赋值；`is_variadic` 是二者的派生属性。

**源码事实：** `Sockets.__init__` 同时写 `_sockets_dict` 与 `__dict__`，于是 socket 可同时用属性访问与 `__setitem__` 访问；`__setitem__` 也双写。

**源码事实：** `Sockets.__eq__` 比较三项：io 类型、`_component`、字典；因为连组件也比，**不同实例的两个 `Sockets` 永不相等**（a01 里 `run` / `run_async` 的比对因此必须共用同一 instance）。

**源码事实：** `sockets_io_type` 参数不做任何强制，只用于 `__repr__`；构造函数注释给出理由——若靠「取字典第一个值的类型」推断，零 socket 的组件无法判断。

**源码事实：** `Sockets._component_name` 显式调 `object.__repr__` 而非 `repr()`，注释说明是为了避免与组件的自定义 `__repr__` 互相递归。

**源码事实：** `InputSocketTypeDescriptor` 只有 `type` 与 `is_mandatory` 两个键，故意不含名字、默认值、可变性——它只服务类型兼容检查。

**源码事实：** `OutputSocket` 只有 `name` / `type` / `receivers`，无默认值、无可变性概念；其 `type` 注解窄于 `InputSocket`（只有 `type`），但运行期可为 `UnionType`。

**源码事实：** `wrap_input_in_list` 只在懒惰可变时有意义，决定多个上游的值是「保持包在 list 里」还是「摊平一层」；自动升级成可变 socket 时它被置 `False`。

**架构推论：** 「必填性由默认值推出」与「可变性由类型注解推出」是同一种设计哲学的两个实例：**不引入独立的布尔字段，而是让已有的语法结构承载语义**。好处是不可能出现「必填但给了默认值」这种自相矛盾的状态；代价是「怎么判断必填」这件事必须写成文档，否则读代码的人会以为 `default_value` 只是个便利字段。

**架构推论：** 把可变性放在 `Annotated` 的 metadata 里，而不是放在 socket 的字段声明里，意味着**组件的 `run` 签名是唯一真相来源**——写 `def run(self, docs: Variadic[Document])` 就够了，不需要再在别处声明「这个参数接多个」。这与 a01 的「socket 从签名反推」是同一个方向。

**架构推论：** `Sockets.__eq__` 把 `_component` 纳入比较，是一个「便于调试但容易误用」的选择。它让 `print` 出来的差异能被 `==` 感知，但也让 `==` 无法用于「比较两组 socket 定义是否相同」这个更常见的需求。a01 里之所以能绕过去，是因为两边共用同一个 instance——**这个巧合一旦被打破就会变成静默的错误判断**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/core/component/types.py` | `InputSocket` | 输入端口值类型：名字、类型、默认值、可变标志、发送方 |
| `haystack/core/component/types.py` | `InputSocket.is_mandatory` / `is_variadic` | 派生属性：前者靠 `is _empty`，后者靠两个可变标志 |
| `haystack/core/component/types.py` | `InputSocket.__post_init__` | 从 `Annotated` 元数据识别可变性并解包内层类型 |
| `haystack/core/component/types.py` | `OutputSocket` | 输出端口值类型：名字、类型、接收方 |
| `haystack/core/component/types.py` | `InputSocketTypeDescriptor` | 供类型兼容检查使用的两键描述符 |
| `haystack/core/component/types.py` | `Variadic` / `GreedyVariadic` / `HAYSTACK_VARIADIC_ANNOTATION` | 用 `Annotated` 字符串标记表达「懒惰可变」与「贪婪可变」 |
| `haystack/core/component/types.py` | `_empty` | 「未设默认值」哨兵类 |
| `haystack/core/component/sockets.py` | `Sockets` | 端口容器：双写字典与 `__dict__`、`__contains__` / `get` / `__eq__` / `__repr__` |
| `haystack/core/component/sockets.py` | `Sockets._component_name` | 绕开组件自定义 `__repr__` 以避免无限递归 |
| `haystack/core/type_utils.py` | `_type_name` | 把类型渲染成人可读形式（处理 UnionType / Literal / NoneType） |

## 参考

- [haystack 源码：core/component/types.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/component/types.py)
- [haystack 源码：core/component/sockets.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/component/sockets.py)
- [haystack 文档：组件](https://docs.haystack.deepset.ai/docs/components)
- [Python 文档：`typing.Annotated`](https://docs.python.org/3/library/typing.html#typing.Annotated)

## 与主核 / AI 的关系

socket 是「模块之间的数据契约」，主核里对应的是**算子输出 schema 与列定义**。这里有四条经验。

**第一，用哨兵而不是 `None` 表达「未设置」。** `_empty` 与 a03 的 `_NoOutputProduced` 是同一手法。主核里做「列默认值」时同样会遇到：`DEFAULT NULL`（默认值是 NULL）与「没有默认值」是两件事，用 `None` 混着表达必然错。而 `is_mandatory` 用 `is` 而非 `==` 这一条更值得抄——**任何可能与用户数据比较的哨兵，都必须用身份比较**，否则一个自定义 `__eq__` 的类型就会让判定炸掉（DataFrame 的「truth value is ambiguous」是最典型的症状）。

**第二，把语义编码进已有的语法结构，而不是加字段。** 「必填性来自默认值」「可变性来自类型注解」都是这个思路。主核的 schema 定义里若出现「`nullable` 标志 + 默认值」两套并存，就很容易出现自相矛盾的行；让其中一个从另一个推出，非法状态就不可表达。

**第三，派生属性优于冗余字段。** `is_variadic` 只读地由两个具体标志合成，于是不可能出现不一致。主核里凡是「`has_x` 与 `x_count > 0` 并存」的地方都应当只留一个。

**第四，`__repr__` 互引必须显式绕开。** `Sockets._component_name` 调 `object.__repr__` 的做法，是「两个对象的 repr 互相包含」时唯一可靠的解法。主核里若做「计划节点打印包含输入输出 schema、而 schema 打印又引用节点」，会遇到同一个 `RecursionError`。判断准则：**repr 链里只要有一处可能指回自己，就要在那里断开**。
