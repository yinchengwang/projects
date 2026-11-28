---
project: haystack
feature: type-compatibility
title: 类型兼容与自动转换
page_type: feature
schema_version: 1
refs:
  - path: haystack/core/type_utils.py
    symbols: [ConversionStrategy, _types_are_compatible, _strict_types_are_compatible]
  - path: haystack/core/type_utils.py
    symbols: [_get_conversion_strategy, _convert_value, _chat_message_to_str]
---

# 类型兼容与自动转换

## 一句话职责

决定两个 socket 的声明类型能不能接上——不只是「类型相等」，还包括「能否在运行时自动转换」；如果只能靠转换，就把「该怎么转」算出来并随连接一起存下来，供执行期照做。

## 机制是什么

大多数数据流框架在这一层的规则只有一条：`sender` 类型必须是 `receiver` 类型的子类型（或相等），否则连接报错。haystack 多了一条路：**如果类型不严格兼容，就看存不存在一个已知的转换策略**。存在则连接成立，执行期按策略转换值。

这带来两个必须分开的概念：

| 概念 | 含义 | 入口 |
|---|---|---|
| 严格兼容 | `sender` 就是 `receiver`（或是其子类型） | `_strict_types_are_compatible` |
| 可兼容 | 严格兼容，**或**存在转换策略 | `_types_are_compatible` |

`_types_are_compatible` 返回的是一个**二元组** `(is_compatible, conversion_strategy)`，而 `conversion_strategy` 为 `None` 本身有两种含义：严格兼容（无需转换）或不兼容。要区分必须看第一个元素。

策略被设计成一个**封闭枚举**而不是任意函数：

```python
class ConversionStrategy(Enum):
    CHAT_MESSAGE_TO_STR = "chat_message_to_str"
    STR_TO_CHAT_MESSAGE = "str_to_chat_message"
    WRAP = "wrap"
    WRAP_CHAT_MESSAGE_TO_STR = "wrap_chat_message_to_str"
    WRAP_STR_TO_CHAT_MESSAGE = "wrap_str_to_chat_message"
    UNWRAP = "unwrap"
    UNWRAP_STR_TO_CHAT_MESSAGE = "unwrap_str_to_chat_message"
    UNWRAP_CHAT_MESSAGE_TO_STR = "unwrap_chat_message_to_str"
```

八个值可以分解成两组正交的维度：

- **形状维度**：`T → list[T]`（`WRAP`）与 `list[T] → T`（`UNWRAP`）
- **元素维度**：`ChatMessage ↔ str` 的双向转换

组合起来得到 2（形状）× 1（无元素转换）+ 2（元素双向）× 2（带/不带形状）= 八个。所以这不是八个特例，而是 **2 × 4 的笛卡尔积**——枚举把组合显式列了出来，没写成两个独立字段。

## 怎么实现的

**`_strict_types_are_compatible` 是一个九分支的递归判定，而每个分支都在处理 Python 类型系统的一处特殊。**

按源码顺序：

1. **`sender == receiver or receiver is Any` → 真**。`Any` 作为接收方接受一切。
2. **`sender is Any → 假`**。反过来不成立：声明「我产出 `Any`」的 socket 不知道实际会产出什么，所以不能承诺接到具体的接收方。这是一个**方向不对称**的选择。
3. **`issubclass(sender, receiver)` 包 `try/except TypeError`**。`typing` 构造（如 `list[int]`）不能用于 `issubclass`，会抛 `TypeError`，于是落到后面的泛型处理。
4. **裸 `Union` 一律拒绝**：`(sender_origin is Union and not sender_args) or (receiver_origin is Union and not receiver_args)` → 假。裸 `Union`（`typing.Union` 不带参数）在语义上是「任意 union」，信息不足，直接判不兼容。
5. **`Literal` 发送方按「值的类型」展开**：`Literal["a", "b"]` 要接到某个 receiver，则每个**值的类型**（这里是 `str`）都要兼容 receiver；若 receiver 是 `Union`，则每个值要能作为 `Literal[value]` 匹配到 union 的某个成员。这一条让 `Literal["a"] → str` 成立、`Literal["a"] → int` 不成立。
6. **`Literal` 接收方按「成员资格」匹配**：`sender_origin is receiver_origin is Literal` 时用 `all(value in receiver_args for value in sender_args)`。所以 `Literal["a","b"] → Literal["a","b","c"]` 成立（子集），反向不成立。**位置无关**，因为 `Literal` 的参数是无序集合。
7. **`Union` 接收方按「任一成员」匹配**：`any(_strict_types_are_compatible(sender, union_arg) for union_arg in receiver_args)`。
8. **`Union` 发送方按「全部成员」匹配**：`all(_strict_types_are_compatible(union_arg, receiver) for union_arg in sender_args)`。方向与上一条相反——发送方的 union 每个成员都必须能接上。
9. **`list[T] → Iterable[T]` 专门放行**：裸 `Iterable` 接受任意 `list`；`Iterable[T]` 要求 `list[T]` 且 `T` 兼容。这是一个**协变**关系的显式补丁（`list` 在类型理论上不是 `Iterable` 的子类型，因为 `Iterable` 是协变而 `list` 是 invariant）。

末尾两级：

```python
if not (sender_origin and receiver_origin and sender_origin == receiver_origin):
    return False
if sender_origin == receiver_origin == Callable:
    return _check_callable_compatibility(sender_args, receiver_args)
if not sender_args and sender_origin:
    sender_args = (Any,)
if not receiver_args and receiver_origin:
    receiver_args = (Any,) * (len(sender_args) if sender_args else 1)
return not (len(sender_args) > len(receiver_args)) and all(
    _strict_types_are_compatible(*args) for args in zip(sender_args, receiver_args, strict=False)
)
```

四处：

- **裸类型参数补成 `Any`**：`list` 与 `list[int]` 比较时，前者的参数被视作 `(Any,)`。于是 `list[int] → list` 成立（接收方参数是 `Any`），`list → list[int]` 也成立（发送方参数是 `Any`，而第 1 条说 `receiver is Any` 为真……不对，这里是 `_strict_types_are_compatible(Any, int)`，按第 2 条 `sender is Any → 假`）。所以 **`list → list[int]` 被判不兼容**。这是第 2 条的连带后果。
- **接收方参数按发送方参数个数展开**：`receiver_args = (Any,) * len(sender_args)`。所以 `tuple[int] → tuple` 会把接收方补成 `(Any,)`。
- **`len(sender_args) > len(receiver_args)` 直接拒**：发送方参数更多意味着它要求更具体，不能接到更宽的位置。例如 `tuple[int, str] → tuple[int]` 被拒，反向也因长度不同而被 `zip` 截断（`zip(..., strict=False)` 只比较前几项）——实际上反向 `tuple[int] → tuple[int, str]` 会通过（长度 1 ≤ 2，只比第一项）。
- **`zip(..., strict=False)`** 显式声明不回滚多余项。所以这个比较是**前缀式**的，不是严格的逐项相等。

**`_get_conversion_strategy` 的处理顺序决定了 Union 的语义方向。**

```python
if _safe_get_origin(sender) is Union:
    strategies = {_get_conversion_strategy(arg, receiver) for arg in get_args(sender)}
    if len(strategies) == 1:
        return strategies.pop()
    return None
```

**发送方是 Union 时，所有成员必须得到同一个策略**（`len(strategies) == 1`）。注意若所有成员都严格兼容，集合是 `{None}`，`len == 1`，返回 `None`——即「无需转换」。而若成员给出 `{None, WRAP}` 两个不同结果，**整体返回 `None`**，而 `None` 在调用方（`_types_are_compatible`）意味着「没有转换策略」→ 判不兼容。所以「混合情况一律拒绝」是正确的。

```python
if _safe_get_origin(receiver) is Union:
    strategies = {_get_conversion_strategy(sender, arg) for arg in get_args(receiver)}
    for preferred in _STRATEGY_PRIORITY:
        if preferred in strategies:
            return preferred
    return None
```

接收方是 Union 时用**优先级表**挑选。而优先级表是：

```python
_STRATEGY_PRIORITY = (
    ConversionStrategy.WRAP,
    ConversionStrategy.UNWRAP,
    ConversionStrategy.CHAT_MESSAGE_TO_STR,
    ConversionStrategy.STR_TO_CHAT_MESSAGE,
    ConversionStrategy.WRAP_CHAT_MESSAGE_TO_STR,
    ConversionStrategy.WRAP_STR_TO_CHAT_MESSAGE,
    ConversionStrategy.UNWRAP_CHAT_MESSAGE_TO_STR,
    ConversionStrategy.UNWRAP_STR_TO_CHAT_MESSAGE,
)
```

排序的含义：**纯形状转换优先于元素转换**，而「元素转换 + 形状」的组合垫底。理由是形状转换不丢信息（包一层/解一层），而 `ChatMessage → str` 会**丢掉角色、工具调用等所有非文本内容**。所以当一个值有多条路可走时，优先选信息损失最小的。

注意优先级表里 `WRAP` 排在 `UNWRAP` 前——但两者不会同时适用（一个要求接收方是 `list`，一个要求发送方是 `list`）。除非双方都是 `list`，那时 `WRAP` 分支会被 `_safe_get_origin(receiver) is list` 接管，而 `UNWRAP` 分支里有一句 `_safe_get_origin(receiver) is not list` 的显式互斥。

**四个转换分支各有一条精度限制。**

```python
# Wrap: T -> List[T]
if _safe_get_origin(receiver) is list and (args := get_args(receiver)):
    inner = args[0]
    if _strict_types_are_compatible(sender, inner):
        return ConversionStrategy.WRAP
    if _contains_type(sender, ChatMessage) and _contains_type(inner, str):
        return ConversionStrategy.WRAP_CHAT_MESSAGE_TO_STR
    if _contains_type(sender, str) and _contains_type(inner, ChatMessage):
        return ConversionStrategy.WRAP_STR_TO_CHAT_MESSAGE
```

三处：

1. **`(args := get_args(receiver))` 的海象赋值同时兼作真值检查**。裸 `list` 的 `get_args` 是空元组，所以「接收方是裸 `list`」不会进入 wrap 分支——因为不知道 `T` 是什么，包成 `list[??]` 无从判断。于是 **`T → list`（裸）不成立**。

2. **`_contains_type` 而不是 `is`**：它检查的可能是 union。所以 `ChatMessage | str → list[str]` 这类情形能被识别。

3. **`_contains_type` 的实现是浅层的**：

```python
def _contains_type(container: Any, target: Any) -> bool:
    if container == target:
        return True
    return _safe_get_origin(container) is Union and target in get_args(container)
```

   只查一层 union 成员，不做递归。所以 `ChatMessage | int → list[str]` 里 `_contains_type(ChatMessage | int, str)` 为假、`_contains_type(Union, ChatMessage)` 为真，于是走 `WRAP_CHAT_MESSAGE_TO_STR`——但这个策略在执行期要求值真的是 `ChatMessage`，而它可能是 `int`。**这是一个真实的运行期崩溃点**：连接成立但转换失败。

```python
# Unwrap: list[T] -> T，注释明确限制在 str / ChatMessage
if _safe_get_origin(sender) is list and (args := get_args(sender)):
    inner = args[0]
    if (
        _safe_get_origin(receiver) is not list
        and inner in (str, ChatMessage)
        and _strict_types_are_compatible(inner, receiver)
    ):
        return ConversionStrategy.UNWRAP
```

**`inner in (str, ChatMessage)` 把 unwrap 限制在两种元素类型**，注释把理由写清楚了：「restricted to str / ChatMessage to avoid silent drop of `list[1:]`」。即 `list[int] → int` 不允许，因为一个多元素列表被 unwrap 成单个值时就丢了其余元素。

这个限制是**保守但正确**的。但它留下一个不对称：`T → list[T]` 对任意 `T` 都允许（wrapping 不丢信息），而 `list[T] → T` 只对两种 `T` 允许。所以**同一对类型两个方向的可转换性不同**。

**`_convert_value` 用 `match` 语句穷举八个策略，而三处 helper 抛错是有意的。**

```python
def _get_first_item(value: list[Any]) -> Any:
    if not value:
        raise ValueError("Cannot get first item of an empty list. ")
    if len(value) > 1:
        raise ValueError(
            f"Cannot unwrap a list of {len(value)} items to a single value: "
            "a list-to-scalar connection only accepts one-element lists; otherwise items would be silently dropped."
        )
    return value[0]
```

两处：

1. **空列表与多元素列表都抛错**——即 unwrap 要求**恰好一个元素**。这是必要的：连接期类型检查通过只说明「形状对」，值的内容要多长只有运行期知道。

2. **多元素时的错误消息解释了为什么不做「取第一个」**：「otherwise items would be silently dropped」。这是把设计意图写进错误，属于「把修法写进错误」的同类做法——但这里写的是「为什么不能修」。

```python
def _chat_message_to_str(value: Any) -> str:
    if value.text is None:
        raise ValueError("Cannot convert `ChatMessage` to `str` because it has no text. ")
    return value.text
```

**`value.text is None` 判空**，注意是 `is None` 而不是真值判断——所以**空字符串 `""` 的文本可以通过**（返回 `""`）。这个区分很重要：一个只有工具调用、没有文本的 assistant 消息，`text` 是 `None`，转换会报错；而一条内容为空字符串的消息可以正常转成 `""`。

**`ChatMessage → str` 是有损的，而代码不掩饰这一点。** `_chat_message_to_str` 只取 `value.text`，所以角色、工具调用、图像、推理内容全部丢弃。而 `STR_TO_CHAT_MESSAGE` 用 `ChatMessage.from_user(value)`——**总是构造 user 角色**。所以 `ChatMessage → str → ChatMessage` 往返会把角色变成 `user`。

这两条合起来说明：**这套转换是「让管道能接上」的便利机制，不是无损编解码**。所以它只在「两边本意就是同一份文本」时成立，而框架无法检查这个前提。

**`_STRATEGY_PRIORITY` 之外还有一处「严格优先」的裁决，在管道连接逻辑里。**

```python
if len(possible_connections) > 1 and self._connection_type_validation:
    strict_matches = [
        (out_sock, in_sock, None)
        for out_sock, in_sock, conversion_strategy in possible_connections
        if conversion_strategy is None
    ]
    if strict_matches:
        possible_connections[:] = strict_matches
```

当一对组件之间存在多个可能的 socket 配对时，**严格兼容的配对优先于需转换的配对**。注释给了理由：「previously, pipelines did not allow type conversion」——所以这是**向后兼容的保证**：一个在旧版本里能正确连线的管道，引入转换能力后不会因为多出来的候选而连错。

这是一个很好的模式：**新增能力时不改变旧输入的解析结果**。实现方式是「新能力只在旧规则无解时启用」，而不是修改旧规则的优先级。

**`connection_type_validation` 这个总开关让整套机制可以关掉。**

```python
def __init__(self, ..., connection_type_validation: bool = True) -> None:
    self._connection_type_validation = connection_type_validation
```

而 `_types_are_compatible` 的第一句就是：

```python
if not type_validation:
    return True, None
```

所以关掉之后**所有连接都成立且不做转换**——注意返回的 `conversion_strategy` 是 `None` 而不是某个策略。于是后面的 `_convert_value` 不会被调用。所以这个开关的语义是「不做类型检查也不做转换」，而不是「不检查但仍转换」。

这是一个合理的降级：用户关掉验证通常是因为类型标注不准确，此时按标注做的转换同样不可信。

**执行期的转换失败被包装成带组件名的错误。**

```python
if not isinstance(value, _NoOutputProduced) and conversion_strategy:
    try:
        value = _convert_value(value=value, conversion_strategy=conversion_strategy)
    except Exception as e:
        sender_node = self.graph.nodes.get(component_name)
        sender_instance = sender_node.get("instance") if sender_node else None
        sender_type_name = type(sender_instance).__name__ if sender_instance else "unknown"
        receiver_node = self.graph.nodes.get(receiver_name)
        receiver_instance = receiver_node.get("instance") if receiver_node else None
        receiver_type_name = type(receiver_instance).__name__ if receiver_instance else "unknown"
        msg = "Failed to perform conversion between components:\n..."
```

三处：

1. **`not isinstance(value, _NoOutputProduced)` 先判哨兵**——若上游没产出这个 socket，不做转换（a03 讲过哨兵的用途）。
2. **`except Exception` 全捕**。所以转换里的任何错误（`ValueError`、`AttributeError` 等）都被包装。合理，因为转换失败的形态有多种。
3. **错误消息里带两个组件的「实例类型名」**。不是 socket 名而是组件的类名——因为用户更容易认出「`OpenAIGenerator` 的输出接到了 `DocumentJoiner`」而不是 socket 名。它也解释了 `sender_instance`/`receiver_instance` 可能为 `None`（拿不到实例，比如反序列化后的管道），此时名字是 `"unknown"`。

## 关键取舍

**源码事实：** 兼容性分两层：`_strict_types_are_compatible`（九分支递归）判「能不能直接接」，`_types_are_compatible` 在严格不成立时再试转换策略，返回 `(is_compatible, conversion_strategy)` 二元组。

**源码事实：** `sender is Any → 假`、`receiver is Any → 真`，方向不对称；裸 `Union`（无参数）一律拒绝；`Literal` 发送方按其值的类型展开、`Literal` 接收方按成员资格匹配；`list[T] → Iterable[T]` 有专门协变放行。

**源码事实：** 泛型参数比较是**前缀式**的（`zip(..., strict=False)` 且先判 `len(sender_args) > len(receiver_args)`），裸类型参数补 `Any`；因此 `list → list[int]` 因 `sender is Any` 判假而不成立，而 `list[int] → list` 成立。

**源码事实：** 发送方是 `Union` 时要求**所有成员给出同一策略**，混合则整体返回 `None`（判不兼容）；接收方是 `Union` 时按 `_STRATEGY_PRIORITY` 挑选，顺序是「纯形状转换 → 元素转换 → 形状+元素组合」，依据是信息损失从小到大。

**源码事实：** 四个 shape 分支都有精度限制：`T → list[T]` 对任意 `T` 成立（wrapping 不丢信息），`list[T] → T` 只在 `T ∈ {str, ChatMessage}` 时成立（注释说明：否则会静默丢掉 `list[1:]`）；裸 `list` 作为接收方不触发 wrap（拿不到 `T`）。

**源码事实：** `_contains_type` 只查一层 union 成员不递归，所以 `ChatMessage | int → list[str]` 会被判成 `WRAP_CHAT_MESSAGE_TO_STR`，但值可能是 `int`——连接成立、运行期转换失败。

**源码事实：** `_get_first_item` 要求恰好一个元素（空列表与多元素都抛错），多元素时的错误消息明确写出「otherwise items would be silently dropped」。

**源码事实：** `_chat_message_to_str` 用 `value.text is None` 判空（空串可通过），只取文本；`STR_TO_CHAT_MESSAGE` 总用 `ChatMessage.from_user`。所以 `ChatMessage → str → ChatMessage` 往返会把角色变成 `user`，且丢弃工具调用/图像/推理内容。

**源码事实：** 管道连接时若存在多个候选配对，**严格兼容的优先于需转换的**（注释理由：旧版本不允许类型转换，这是向后兼容保证）。

**源码事实：** `connection_type_validation=False` 时 `_types_are_compatible` 直接返回 `(True, None)`——**不检查也不转换**，而不是「不检查但仍转换」；执行期转换失败被 `except Exception` 全捕并包装成带两个组件实例类型名的错误。

**架构推论：** 「类型不匹配时先试转换」这个机制的本质是**把一部分类型错误从「编译期」推到「运行期」**。严格检查是静态的（`typecheck()` 阶段就能拒绝），而转换是动态的（形状对了但值可能不对，如 `_get_first_item` 的多元素、`_contains_type` 的浅层检查）。这套设计用「连线的成功率」换了「运行期失败的确定性」——判据是**这类转换在实践中足够常见**（`ChatMessage` 与 `str` 之间的接错、单值接列表），值得为它开一条路。

**架构推论：** 封闭枚举 + 优先级表这个组合很值得注意。若策略是可注册的函数，就无法预先定义优先级（任意函数之间没有可比性）。正因为策略集合是**封闭且可枚举**的，才能写出一张「信息损失从小到大」的优先级表。所以「限制扩展性」在这里换来了「决定论」——同一个 Union 接收方的选择结果不依赖注册顺序。

**架构推论：** 严格优先于转换这条兼容规则，是「新增能力不改变旧行为」的通用手法。它比「版本开关」更好：不需要用户做任何选择，也不需要维护两套代码路径。代价是当用户**想要**转换时，若同时存在严格配对，转换配对会被静默丢弃——所以「为什么我的 socket 没被转换」这个问题在源码里无迹可循。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/core/type_utils.py` | `ConversionStrategy` | 八个策略的封闭枚举（2 形状 × 4 元素组合） |
| `haystack/core/type_utils.py` | `_STRATEGY_PRIORITY` | Union 接收方的挑选顺序，按信息损失从小到大 |
| `haystack/core/type_utils.py` | `_resolve_parameter_types` | 解析延迟注解（`from __future__ import annotations`），保留 `Annotated` 元数据 |
| `haystack/core/type_utils.py` | `_type_name` | 报错用的可读类型名（处理 Optional/Literal/Callable） |
| `haystack/core/type_utils.py` | `_safe_get_origin` | `get_origin` 的补强，把 `UnionType` 归一成 `Union` |
| `haystack/core/type_utils.py` | `_contains_type` | 浅层 union 成员检查（不递归） |
| `haystack/core/type_utils.py` | `_strict_types_are_compatible` | 九分支严格兼容判定 |
| `haystack/core/type_utils.py` | `_check_callable_compatibility` | `Callable` 的返回值与参数逐位比较 |
| `haystack/core/type_utils.py` | `_get_conversion_strategy` | 发送方 Union「全部一致」/ 接收方 Union「按优先级」 |
| `haystack/core/type_utils.py` | `_types_are_compatible` | 两层判定的组合入口 |
| `haystack/core/type_utils.py` | `_chat_message_to_str` | 取 `text`，`is None` 时报错 |
| `haystack/core/type_utils.py` | `_get_first_item` | unwrap 的单元素约束 |
| `haystack/core/type_utils.py` | `_convert_value` | `match` 穷举八个策略 |
| `haystack/core/pipeline/base.py` | 连接裁决段 | 多候选时严格配对优先（向后兼容） |
| `haystack/core/pipeline/base.py` | 执行期转换段 | 跳过哨兵、包装错误、带组件实例类型名 |

## 参考

- [haystack 源码：type_utils.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/type_utils.py)
- [haystack 文档：管道连接与类型](https://docs.haystack.deepset.ai/docs/pipelines)
- [Python 文档：typing.get_origin 与 get_args](https://docs.python.org/3/library/typing.html#typing.get_origin)
- [PEP 484：类型提示规范](https://peps.python.org/pep-0484/)

## 与主核 / AI 的关系

「类型不匹配就尝试转换」这一层，在主核（多模态数据库内核）里对应的是**隐式类型转换（implicit cast）**。SQL 里 `int4 = int8` 会自动提升，而 `text = int4` 会报错。主核的做法是把转换规则写进**类型系统的转换矩阵**（谁能转谁、转的时候有没有精度损失），并且明确规定「隐式转换只在安全方向发生」（`int4 → int8` 可以，反向需要显式 `CAST`）。

haystack 的选择恰好相反：它允许了**不安全方向**（`ChatMessage → str` 丢内容、`list[T] → T` 要求单元素），因为它的目标不是「保证语义等价」而是「让管道能接上」。这个差别来自错误代价：主核的一次错误隐式转换会让查询结果静默错误（无法察觉），而管道里的一次有损转换通常只是「这轮输出丢了一个字段」，能通过输出观察到。

第二处对照是 **`Any` 的方向不对称**。主核里对应的是 `unknown` 类型（PostgreSQL 的 `unknown` 伪类型只用于字面量，进入表达式前必须解析成具体类型）。haystack 允许 `receiver is Any` 接受一切而不允许 `sender is Any`——理由是「产出 `Any` 的人不知道会产出什么，不能承诺」。这与主核对 `unknown` 的处理方向一致：**不知道的东西只能被接受，不能被承诺**。

第三处是 **`_STRATEGY_PRIORITY` 的信息损失排序**，与主核的 `EXPLAIN` 里「哪种计划更便宜」的代价排序同构。两者都是「在多个可行方案里按一个预先定义的偏序挑选」，而偏序的判据都是「代价」——一个是信息损失，一个是执行代价。

对 AI 工程的实际意义在于：管道里的类型错误是最难排查的一类（错误发生的位置与显现的位置相隔几个组件）。这份实现给出的三条经验可直接用：**新增转换能力时保护旧的严格匹配**（否则老管道可能连错）、**转换策略要封闭可枚举**（否则无法定义优先级，结果不确定）、以及**有损转换要在错误消息里写清损失**（`_get_first_item` 的多元素报错就是这么做的）。
