---
project: smolagents
feature: safe-serialization
title: 安全序列化
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/serialization.py
    symbols: [SafeSerializer, SerializationError]
  - path: src/smolagents/utils.py
    symbols: [make_json_serializable]
---

# 安全序列化

## 一句话职责

用一个**白名单 + 类型标记**的 JSON 编码方案替代 pickle：能编码的类型有明确清单，超出的直接抛 `SerializationError`；解码只走 `__type__` 标记表，不做任意类实例化。

## 机制是什么

`SafeSerializer` 有两个对称方法：`to_json_safe(obj)` 把 Python 对象转成「可 JSON 序列化、带类型标记」的结构；`from_json_safe(obj)` 反向还原。

编码采用的形状是 `{"__type__": "<类型名>", "data": ...}`。基础类型（`str`/`int`/`float`/`bool`/`None`/`list`/`dict`）直接原样或递归处理，不带标记；扩展类型（`tuple`/`set`/`frozenset`/`bytes`/`complex`/时间类型/`Decimal`/`Path`/`PIL.Image`/`ndarray`/dataclass）带标记。

序列化产物是带前缀的字符串：`"safe:" + json.dumps(...)`。`allow_pickle=True` 时才可能出现 `"pickle:" + base64(...)` 作为降级。

## 怎么实现的

**拒绝什么——这是这一层的核心。** `to_json_safe` 的末尾是一句无条件的 `raise SerializationError(f"Cannot safely serialize object of type {type_name}")`。

所以**任何不在下列清单里的对象都无法编码**：

- 基础：`str`、`int`、`float`、`bool`、`None`、`list`、`dict`（键须为字符串，否则走「复杂键」分支）
- 容器：`tuple`、`set`、`frozenset`
- 标量扩展：`bytes`、`complex`、`datetime`、`date`、`time`、`timedelta`、`Decimal`
- 路径：`pathlib` 下名字含 `Path` 的类型
- 可选：`PIL.Image.Image`、`numpy.ndarray`（以及 `numpy.integer`/`numpy.floating` 转成 Python 标量）
- dataclass 实例

被拒的包括且不限于：**任意用户类实例**、函数与 lambda、模块、类对象本身、生成器、`io` 对象、`sqlite3.Connection` 之类的资源句柄、`Enum` 成员、`NamedTuple`（除非恰好是 dataclass）、`type` 对象。

这里最关键的一条：**普通类实例一律被拒**，而 `make_json_serializable`（另一个函数）对这类对象是**接受**的——它会把 `obj.__dict__` 摊开并加 `_type` 标记。两个函数的策略正好相反，后面会对比。

**解码只认标记表。** `from_json_safe` 里有一个 `if "__type__" in obj:` 的分支链，逐个匹配固定字符串：`bytes`、`PIL.Image`、`set`、`tuple`、`complex`、`frozenset`、`dict_with_complex_keys`、`datetime`、`date`、`time`、`timedelta`、`Decimal`、`Path`、`ndarray`、`dataclass`。

每个分支都是**显式构造已知类型**，没有一处 `importlib` 或 `globals()[...]` 动态取类。所以解码不可能因为数据里的字符串而实例化任意类——这是与 pickle 最本质的区别。

不认识 `__type__` 时落进 `return {k: from_json_safe(v) for k, v in obj.items()}`——即当作普通字典递归处理，保留那个 `__type__` 键。所以伪造的标记不会导致崩溃或代码执行，只会变成一个多了一个键的字典。

**dataclass 的可逆性缺口。** `to_json_safe` 对 dataclass 记录 `class_name`、`module` 与字段数据；但 `from_json_safe` 明确注释「无法在没有类的访问权限的情况下重建真实类」，所以返回一个**字典**：`{"__dataclass__": class_name, "__module__": module, ...fields}`。

所以 dataclass 是「能出去、回不来」。这不是 bug 而是有意的降级：数据保住了，类型没了。若需要真正还原，调用方得自己拿 `__dataclass__` 与 `__module__` 去构造。

**复数键字典的特殊处理。** 若 dict 的键不全是字符串，编码成 `{"__type__": "dict_with_complex_keys", "data": [[k, v], ...]}`——一个二元组列表。这是必要的，因为 JSON 的键只能是字符串，直接 `json.dumps({1: "a"})` 会把键静默转成 `"1"` 丢失类型。

**性能上的快路径。** `to_json_safe` 开头用 `type(obj) is str` 这类**精确类型比较**而不是 `isinstance`。注释说明理由：基础类型是最常见的情况。精确比较比继承检查快，且对基础类型语义等价。

但注意这也意味着**子类不命中快路径**：一个 `class MyStr(str)` 实例不会被当作 `str`，会落到后面的分支并最终被拒。这是「用性能换了一点兼容性」的取舍。

类型分派还用了 `type(obj).__module__` 与 `__name__` 做字符串比较（而不是导入类型再 `isinstance`），比如 `if type_module == "datetime"`、`if type_module.startswith("pathlib")`。这让 datetime 与 pathlib 的处理**不需要 import**，只在需要时（如 `PIL`、`numpy`）才通过 `_get_optional_type` 惰性导入并缓存。

**`_get_optional_type` 的缓存。** 它用 `cls._optional_types_cache` 这个类级字典记录「模块.属性 → 类或 None」，避免重复 import 尝试。`None` 也被缓存，所以「没装 numpy」这个事实只被探测一次。

**两个序列化器的对比。** `make_json_serializable`（在 `utils.py`）做的是另一件事：

- 对 `str` 且形如 `{...}` 或 `[...]` 的，**尝试 `json.loads` 并递归**——即「字符串里的 JSON 会被展开成结构」。
- 对 `dict`，键一律 `str(k)`。
- 对**任何有 `__dict__` 的对象**，返回 `{"_type": 类名, **__dict__ 摊开}`。
- 兜底 `str(obj)`。

所以它是**尽力而为的「变成能被 json.dumps 接受的东西」**，不保证可逆、不拒绝任何输入。而 `SafeSerializer` 是**白名单 + 可逆 + 明确拒绝**。

两者用途不同：`make_json_serializable` 用于「记录」——记忆步骤的 `dict()`、工具调用的参数、`action_output`，都只需要「能存下来看看」；`SafeSerializer` 用于「传输」——变量与最终答案要跨进程序列化再反序列化，必须可逆且不能被构造攻击。

**前缀协议的严格度在调用点不同。** `SafeSerializer.loads` 对**无前缀**的输入，在 `allow_pickle=True` 时按 legacy pickle 反序列化（发警告）；`allow_pickle=False` 时抛错。

但远程执行器的 `_deserialize_final_answer`（见 g01）对无前缀输入**一律拒绝**，不论 `allow_pickle`。同一份代码里两条路径的严格度不同，原因是信任级别不同：最终答案来自沙箱内部（不完全可信），而 `SafeSerializer.loads` 的输入来自调用方自己的代码。

**生成远端代码的能力。** 除了运行时编解码，`SafeSerializer` 还提供两个「把实现导出成源码」的方法：`get_safe_serializer_code()` 与 `get_deserializer_code(allow_pickle)`。它们都用 `inspect.getsource` 取真实实现，再做文本改名，所以**远端的编解码逻辑与本地是同一份代码**，不会漂移。

## 关键取舍

**源码事实：** 普通类实例在 `SafeSerializer` 里被**拒绝**（抛 `SerializationError`），而在 `make_json_serializable` 里被**接受**（摊开 `__dict__` 加 `_type` 标记）。两个函数的策略相反，服务不同用途。

**源码事实：** dataclass 编码后无法还原成原类型，只能得到带 `__dataclass__`/`__module__` 键的字典。这是有意的降级。

**源码事实：** 解码不存在任何动态类查找，因此不可能被构造出代码执行。伪造 `__type__` 只会得到一个普通字典。

**源码事实：** `to_json_safe` 用精确类型比较做快路径，所以 `str`/`int` 等的子类会落到后面的分支并被拒（除非是 dataclass）。

**源码事实：** `allow_pickle=True` 的降级路径会发 `FutureWarning` 声明「未来版本会移除」，但当前仍可用。

**架构推论：** 这套设计的核心不是「更紧凑的格式」，而是「消除反序列化的攻击面」。pickle 的危险在于 `__reduce__` 能让数据指定「构造哪个类、调什么函数」，于是反序列化等于执行代码。白名单 + 显式标记的解码把「数据能表达什么」限制在一个固定集合里，攻击面从「整个 Python」缩到「那十几个构造器」。

**架构推论：** 「能出去回不来」的 dataclass 与「一律拒绝」的普通类实例，共同构成一条分界：**结构化数据可传，行为与状态对象不可传**。这不是能力不足，而是刻意的边界——一旦允许传自定义类，就需要在远端能导入同一个类，那就退回到了「远端必须装同样代码」的耦合。

**架构推论：** 用 `type().__module__` 与 `__name__` 做字符串判别，避免了在 import 期引入 datetime/pathlib/numpy/PIL 这些模块。对一个「几乎所有程序都会 import」的序列化模块来说，避免强制依赖是重要的——尤其 PIL 与 numpy 是可选依赖。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/serialization.py` | `SafeSerializer` | 白名单式 JSON 序列化器 |
| `src/smolagents/serialization.py` | `SerializationError` | 类型不在白名单时的错误 |
| `src/smolagents/serialization.py` | `SafeSerializer.to_json_safe` | 编码，未命中白名单即抛错 |
| `src/smolagents/serialization.py` | `SafeSerializer.from_json_safe` | 按 `__type__` 标记显式构造已知类型 |
| `src/smolagents/serialization.py` | `SafeSerializer.dumps` | 产出带 `safe:`/`pickle:` 前缀的字符串 |
| `src/smolagents/serialization.py` | `SafeSerializer.loads` | 按前缀分派，pickle 需显式开启 |
| `src/smolagents/serialization.py` | `SafeSerializer._get_optional_type` | 可选类型惰性导入并缓存探测结果 |
| `src/smolagents/serialization.py` | `SafeSerializer.get_deserializer_code` | 从真实实现导出远端可用的解码器源码 |
| `src/smolagents/utils.py` | `make_json_serializable` | 尽力而为的 JSON 化，接受任意对象 |

## 参考

- [smolagents 源码：serialization.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/serialization.py)
- [Python 文档：pickle 的安全警告](https://docs.python.org/3/library/pickle.html#module-pickle)
- [smolagents 文档：安全执行代码](https://huggingface.co/docs/smolagents/tutorials/secure_code_execution)

## 与主核 / AI 的关系

主核里最接近的是「WAL 记录与元组序列化」：同一批数据既要在本地快速编解码，又要在崩溃恢复时可靠还原，所以格式必须自描述且不能依赖「读的时候有同样的代码」。这里 `SafeSerializer` 面对同样的问题，但约束更硬——它跨的是一条**信任边界**，而不只是进程或时间。

与 CrewAI 的工具结果序列化相比（crewai 的 f02 页），CrewAI 用 pydantic 的 `model_validate` + `model_dump_json` 做结果规范化，安全性依赖「输出 schema 里的类型是可信代码定义的」。smolagents 的 `SafeSerializer` 反过来：它不知道接收方有什么类型，所以只能传「带标记的原始数据」。前者要求两端共享类型定义，后者不需要——这是「同进程/同代码库」与「跨沙箱」的差别。

与 `make_json_serializable` 的对照最能说明问题：同一个仓库里两套序列化，一套宽松（为了记录），一套严格（为了传输）。这个共存本身是合理的设计——因为需求确实不同。但它也埋了一个陷阱：开发者可能随手用错。代码里没有任何机制阻止把 `make_json_serializable` 的输出交给 `SafeSerializer.from_json_safe`——那样会得到一个「看起来像 dict 的失败还原」，而不会报错。

对 AI 工程最实用的一条是「白名单 + 显式标记」这个组合的两个必要条件：**编码侧必须拒绝**（而不是降级成 `str()`），**解码侧必须只构造已知类型**（而不是反射）。只做后者会导致「类型信息在编码时就丢了」，只做前者会导致「能编出来但解不回去」。两者缺一，白名单就失效。

另一条是 dataclass 的「能出去回不来」。做跨进程数据传递时，最容易犯的错是假定了可逆性而没有测试。凡是「编码后类型信息只记录在字符串里、解码时不去 import 原类」的方案，都必然丢失类型——这让「传自定义结构」看起来可行（编码成功）而实际不可行（解码得到字典）。若需要真正跨进程传结构，正确做法是把结构定义（schema 或源码）与数据一起传过去。
