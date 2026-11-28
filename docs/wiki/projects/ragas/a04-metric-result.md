---
project: ragas
feature: metric-result
title: 结果结构与输出校验
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/result.py
    symbols: [MetricResult]
  - path: src/ragas/metrics/validators.py
    symbols: [BaseValidator, get_validator_for_allowed_values, get_metric_type_name]
  - path: src/ragas/metrics/validators.py
    symbols: [NumericValidator]
---

# 结果结构与输出校验

## 一句话职责

`MetricResult` 是把「一个分数」升级成「分数 + 理由 + 轨迹」的容器，同时伪装成原始值以兼容算术与比较；三个校验器负责在打分之后检查这个值是否落在指标声明的取值域内，取值域的类型本身就是判别指标种类的唯一依据。

## 机制是什么

两件事被放在一起讲，因为它们通过 `allowed_values` 这个字段咬合：

**结果容器 `MetricResult`。** 它有三个字段：`value`（原始结果，可以是浮点、字符串或列表）、`reason`（可选的自然语言理由）、`traces`（可选的输入输出轨迹，**只允许 `input` 与 `output` 两个键**，其他键在构造时直接抛 `ValueError`）。

它的设计目标很特别：**既要能当对象用，又要能当值用**。所以它不是 `BaseModel` 而是纯 Python 类，靠大量魔法方法来转发行为——`float()` / `int()`、四则运算、六个比较运算、`len()` / 下标 / 迭代、以及 `__getattr__` 转发到内部值。这样老代码写 `score > 0.5` 或 `sum(scores) / len(scores)` 时不必改。

**取值域与校验器。** `allowed_values` 的类型决定指标种类：

| `allowed_values` 类型 | 指标种类 | 校验器 |
|---|---|---|
| `list`（字符串列表） | 离散 | `DiscreteValidator` |
| `tuple` / `range` | 数值 | `NumericValidator` |
| `int` | 排序（期望列表长度） | `RankingValidator` |
| 其他 | 回退成离散 | `DiscreteValidator` |

`get_validator_for_allowed_values` 与 `get_metric_type_name` 是同一套判定的两个出口：前者返回校验器**类**（用于动态继承），后者返回类型**名**（用于 `__repr__` 打印）。

三个校验器统一接口：`validate_result_value(value) -> Optional[str]`——返回错误消息字符串表示失败，返回 `None` 表示通过。**没有异常，没有布尔值**，调用方拿到字符串就知道该怎么组装错误信息。

## 怎么实现的

**`MetricResult` 的构造期唯一校验是 `traces` 的键。** `__init__` 里先检查 `traces` 的键是否都在 `{"input", "output"}` 里，有非法键就抛 `ValueError` 并列出它们。`value` 本身**不做任何校验**——校验发生在更上层（装饰器或 `BaseMetric` 的子类）。

**算术与比较的失败方式是抛错，不是降级。** 每个数值方法都先判 `isinstance(self._value, (int, float))`，不是就 `raise TypeError`。所以对离散结果做 `+` 会得到 `"Cannot add str objects"`，而不是静默的字符串拼接。这是刻意的：让误用立刻暴露。

`__add__` 等双向方法都支持「`MetricResult` 与 `MetricResult`」以及「`MetricResult` 与裸值」两种组合，分别取各自的 `_value`。反向方法（`__radd__`、`__rsub__`、`__rmul__`、`__rtruediv__`）只处理裸值在左的情形。

**比较运算没有类型守卫。** `__lt__` 等六个方法直接把 `self._value` 与对方比较，没有 `isinstance` 检查。所以拿字符串与浮点比较会得到 Python 原生的 `TypeError`，消息不带指标上下文——与算术方法的处理不一致。

**`__getattr__` 是转发层，且会「重新包装」返回值。** 当访问的属性在内部值上存在时：可调用的属性被包成一个 wrapper，调用后若返回值类型与内部值同类型，就重新包成新的 `MetricResult`（并沿用原 `reason`）；不可调用的属性直接返回。因此 `MetricResult("positive").upper()` 返回的是 `MetricResult("POSITIVE")` 而不是裸字符串——**链式字符串操作能保持容器语义**。

`__getattr__` 只在常规查找失败时触发，而 `value`、`reason`、`traces` 都是实例属性，所以不会互相遮蔽。

**`to_dict` 与 `__json__` 是两份略有差异的序列化。** `to_dict()` 返回 `{"result": value, "reason": reason}`（键名是 `result`），`__json__()` 返回 `{"value": value, "reason": reason}`（键名是 `value`）。同一对象两种键名，是一个实际存在的陷阱：读 `to_dict()` 的下游拿不到 `traces`，且键名与 `__json__` 不同。

**pydantic 集成靠自定义 core schema。** `__get_pydantic_core_schema__` 返回一个 union schema：命中 `is_instance_schema(MetricResult)` 就直接保留；否则走 chain schema 把任意值转成 `MetricResult(value=value)`。序列化器按 `info.mode` 分流——JSON 模式用 `__json__`（保证 `model_dump_json` 可序列化），Python 模式返回实例本身（保证 `model_dump()` 后仍是 `MetricResult`，能继续做算术）。

这个双向行为对应两个真实需求：存盘要能 JSON 化，内存里要能运算。**用同一个 schema 同时满足两者，是这里最精巧的一处。**

**`validate` 与 `__get_pydantic_core_schema__` 并存。** 前者是旧版 pydantic 的兼容入口（`classmethod validate(value, info)`），后者是新版 v2 的机制。两代都保留，说明这个类要同时活在新旧 pydantic 环境里。

**`NumericValidator` 有两条判定路径，都先做类型门控。** 第一句 `if not isinstance(self.allowed_values, (tuple, range)): return None`——如果当前指标的取值域不是数值型，这个校验器直接放行（对应「被错误地混入数值校验器的离散指标」这种情形，静默通过而不是报错）。然后：

- 值本身不是 `int` / `float` → 返回「expected a numeric value」。
- `allowed_values` 是二元组 → 判 `min <= value <= max`，越界返回带区间的消息。
- `allowed_values` 是 `range` → 判 `value in range`。

注意**闭区间**：等于边界算通过。而 `bool` 是 `int` 的子类，因此 `True` 会被当成数值 `1` 通过校验——一个容易被忽略的边界。

**`DiscreteValidator` 同样先判 `isinstance(self.allowed_values, list)`。** 不是列表就放行；是列表则要求 `value` 是列表中的某个**元素**（用 `in` 比较，不做类型规约）。消息用 `repr` 打印实际值——所以字符串值会带引号。

**`RankingValidator` 判的是长度。** 先判 `allowed_values` 是 `int`，再要求结果是 `list`，再要求 `len(result) == allowed_values`。

**`get_validator_for_allowed_values` 的兜底是离散。** 类型不认识时返回 `DiscreteValidator` 而不是报错——与两个具体校验器「类型不匹配就放行」的取向一致：**校验链整体倾向于「不认识就不拦」**。

**`get_metric_type_name` 的兜底是 `"CustomMetric"`。** 四条分支返回 `DiscreteMetric` / `NumericMetric` / `RankingMetric`，其余返回 `CustomMetric`。这个名字只用于 `__repr__`，不影响行为。

**校验在读侧被谁调用。** 全仓只有两处：`BaseMetric`（新式基类）通过 `NumericValidator` 混入，但**`BaseMetric.ascore` 本身并不调用 `validate_result_value`**——校验实际生效的路径在装饰器里：`CustomMetric.ascore` 显式调 `self._validate_result_value(result.value)`，失败时返回 `MetricResult(value=None, reason=<错误消息>)`。

## 关键取舍

**源码事实：** `MetricResult` 强调「像原始值」。算术、比较、迭代、下标、`float()`、`int()`、长度、属性转发全部支持，`__eq__` 直接比内部值。所以它几乎可以透明替换 `float`。

**源码事实：** 算术方法有类型守卫会抛 `TypeError`，比较方法没有——同一类误用在两处得到不同质量的错误信息。

**源码事实：** `traces` 只接受 `input` / `output` 两个键，非法键在构造时抛 `ValueError`。而 `value` 在构造期不做任何校验。

**源码事实：** `to_dict()` 与 `__json__()` 使用不同的键名（`result` 与 `value`）。这是同一份数据的两个不一致投影。

**源码事实：** pydantic 序列化按 mode 分流：JSON 模式输出纯字典，Python 模式输出实例本身。这让「存盘」与「继续运算」都能成立。

**源码事实：** 校验器宁可放行也不误拦：`allowed_values` 类型不匹配时一律 `return None`，未知类型兜底成离散。校验失败**不抛异常**，只返回消息字符串；在装饰器路线里，失败被转成 `MetricResult(value=None, reason=消息)`。

**架构推论：** 把「结果值、理由、轨迹」合成一个容器，且容器可当值用，解决了「升级返回类型但不破坏下游」这个老问题。老代码不变，新代码能取理由——这是接口演进的常见解法，代价是容器自身要背一堆魔法方法，任何新运算都得补一遍（例如缺少 `//`、`**`、`&` 等）。

**架构推论：** 校验失败变成「值为 `None` 的结果对象」，把错误编码进数据而不是控制流。好处是批量评分不会因单条越界而中断；代价是「值为 `None`」成了隐式的失败信号，任何聚合前都必须判空，且 `None` 参与算术会抛 `TypeError`（因为守卫只放行 `int` / `float`）。

**架构推论：** `allowed_values` 一字段三义是这套设计的关键简化——它既是取值域声明、又是指标分类依据、又是校验器选择依据。省掉了一个显式的 `metric_type` 参数，代价是**类型误用会静默变成另一种指标**（数值区间写成列表就成了离散指标）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/result.py` | `MetricResult` | 值 + 理由 + 轨迹的容器 |
| `src/ragas/metrics/result.py` | `MetricResult.__init__` | `traces` 键白名单校验，值不校验 |
| `src/ragas/metrics/result.py` | `MetricResult.__getattr__` | 属性转发，同类型返回值重新包装 |
| `src/ragas/metrics/result.py` | `MetricResult.to_dict` | 键名为 `result` 的字典投影 |
| `src/ragas/metrics/result.py` | `MetricResult.__json__` | 键名为 `value` 的 JSON 投影 |
| `src/ragas/metrics/result.py` | `MetricResult.__get_pydantic_core_schema__` | 按序列化模式分流的自定义 schema |
| `src/ragas/metrics/validators.py` | `BaseValidator` | 校验器接口：返回消息或 None |
| `src/ragas/metrics/validators.py` | `NumericValidator` | 区间 / range 判定，闭区间，布尔被当数值 |
| `src/ragas/metrics/validators.py` | `get_validator_for_allowed_values` | 按取值域类型选校验器，兜底离散 |
| `src/ragas/metrics/validators.py` | `get_metric_type_name` | 按取值域类型取类型名，兜底 CustomMetric |
| `src/ragas/metrics/validators.py` | `AllowedValuesType` | 列表、二元组、range、整数的联合类型 |

## 参考

- [ragas 源码：metrics/result.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/result.py)
- [ragas 源码：metrics/validators.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/validators.py)
- [Pydantic 文档：自定义类型与 core schema](https://docs.pydantic.dev/latest/concepts/types/)
- [ragas 文档：自定义指标](https://docs.ragas.io/en/stable/concepts/metrics/custom_metrics/)

## 与主核 / AI 的关系

`MetricResult` 的处境与主核里「游标/行句柄」很像：对外要能被当成数据本身使用（拿到行就能读字段），对内要携带额外上下文（定位、版本、缓冲状态）。两者都用「伪装成值」来降低迁移阻力，也都因此在实现里堆了大量转发逻辑。差别在于主核的句柄生命周期由存储引擎管，`MetricResult` 是纯值对象，没有资源释放问题。

`traces` 只允许 `input` / `output` 两个键，与主核的日志记录只保留少量受控字段是同一种克制：**结构里只开确定会被消费的字段，避免它变成谁都想往里塞的杂物箱**。反过来，`value` 完全不校验，把校验交给上层的取值域——这与主核「行的字段校验由表定义负责、不由行句柄负责」的分工一致。

对 agent 工程最要紧的三条：其一，`value=None` 是失败信号，聚合前必须判空，否则 `None + float` 会抛 `TypeError`。其二，序列化有两条不一致的路径（`to_dict` 用 `result`、`__json__` 用 `value`），落盘与展示不要混用。其三，`allowed_values` 的类型决定指标种类，写成 `[0.0, 1.0]`（列表）会被当成离散指标的取值域，而 `(0.0, 1.0)`（元组）才是数值区间——这是最容易写错的一处。
