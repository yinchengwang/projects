---
project: ragas
feature: metric-decorator
title: 装饰器式自定义指标
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/decorator.py
    symbols: [create_metric_decorator, NumericMetricProtocol, DiscreteMetricProtocol]
  - path: src/ragas/metrics/numeric.py
    symbols: [NumericMetric, numeric_metric]
  - path: src/ragas/metrics/discrete.py
    symbols: [DiscreteMetric, discrete_metric]
  - path: src/ragas/metrics/ranking.py
    symbols: [RankingMetric, ranking_metric]
---

# 装饰器式自定义指标

## 一句话职责

把一个普通函数（同步或异步）装饰成指标对象：函数体就是打分逻辑，`allowed_values` 声明输出取值域，装饰器据此自动选配校验器、自动生成输入校验模型，并让结果统一包成 `MetricResult`。

## 机制是什么

自定义指标有三条路，装饰器路线是其中最轻的一条：

| 路线 | 如何定义 | 输出校验 | 输入校验 |
|---|---|---|---|
| 旧式子类 | 继承 `SingleTurnMetric` + 实现私有异步方法 | 无统一校验 | 靠 `_required_columns` 裁剪 |
| 新式组件 | 继承 `BaseMetric` + 实现 `ascore` | `NumericValidator` 区间 | 各指标自己写 |
| 装饰器 | `@numeric_metric(...)` 套在函数上 | 按 `allowed_values` 自动选配 | 按函数签名自动生成 pydantic 模型 |

装饰器路线的骨架是一句话：**`allowed_values` 的类型决定一切**。传 `["pass","fail"]` 得到离散指标，传 `(0.0, 1.0)` 得到数值指标，传整数 `3` 得到排序指标。这个判定在 `get_validator_for_allowed_values` 里就是三个 `isinstance` 分支。

三个具体的装饰器入口只是薄壳，各自把默认值与类型标注钉死：

- `numeric_metric(name=None, allowed_values=(0.0, 1.0), **metric_params)`
- `discrete_metric(name=None, allowed_values=["pass", "fail"], **metric_params)`
- `ranking_metric(name=None, allowed_values=2, **metric_params)`

三者末尾都做同一件事：`create_metric_decorator()(name=..., allowed_values=..., ...)`。真正的实现在 `decorator.py` 的闭包里。

## 怎么实现的

**`create_metric_decorator` 是「装饰器工厂的工厂」。** 外层函数不接收任何参数，内层是 `decorator_factory(name=None, **metric_params)`，再内层才是接收被装饰函数的 `decorator(func)`。三层嵌套的理由是：`allowed_values` 决定要构造哪种指标类型，而它只在调用装饰器时才可知，所以整个 `CustomMetric` 类必须在 `decorator(func)` 的执行期**动态创建**——每次装饰一个函数就新建一个类。

**类的动态构造。** `CustomMetric` 用 `@dataclass(repr=False)` 定义在闭包内，继承 `SimpleBaseMetric` 与**动态选出的校验器**（`class CustomMetric(SimpleBaseMetric, validator_class)`）。因为 `validator_class` 是运行期变量，这个类的父类组合是每次装饰时算出来的。类里还声明了两个 `init=False` 的字段 `_func` 与 `_metric_params`，用于装原始函数与装饰参数。

**默认值的三处一致化。** `allowed_values` 若未提供，装饰器工厂里先补成 `["pass","fail"]`。而 `numeric_metric` / `discrete_metric` / `ranking_metric` 各在自己的签名里给了默认值，于是**从具体装饰器进入时不会走到工厂的兜底**；只有直接调 `create_metric_decorator()` 才用工厂默认。两条路径的默认值恰好一致，但来源不同。

**输入校验靠「从签名生成 pydantic 模型」。** `_create_pydantic_model` 遍历 `sig.parameters`，对每个参数：

1. 取类型注解（先在 `get_type_hints(func)` 里找，失败退到 `param.annotation`）。
2. 注解为空（`inspect.Parameter.empty`）时：有默认值就用 `type(默认值)` 作为注解，否则用 `typing.Any`。这条兜底让「没写注解但有默认值」的函数也能校验类型。
3. 取默认值：有显式默认值就用它；否则检查是不是 `Optional`（`get_origin` 是 `Union` 且 `NoneType` 在参数里），是则默认 `None`，否则标成必需字段 `...`。
4. 用 `create_model(f"{name}_ValidationModel", __config__=ConfigDict(arbitrary_types_allowed=True), **field_definitions)` 造模型。

`arbitrary_types_allowed=True` 是必需的：函数的参数类型可能是任意对象（例如一段消息列表），pydantic 默认会拒绝。

**校验失败给人看的消息是自己拼的，不是 pydantic 原样抛的。** 三条错误路径：

- **位置参数**：`_create_positional_error` 拼一段带 emoji 的说明，把用户给的位置参数逐个映射回形参名（`param_name=repr(value)`），并给出「正确写法」。这是刻意的设计——位置参数会绕过 pydantic 的字段名匹配，所以直接拒绝而不是尝试兼容。
- **未知关键字参数**：`warnings.warn` 提示哪些参数不认识、哪些是合法的，但**继续执行**（不抛错）。
- **类型/缺字段**：`_format_pydantic_errors` 把 `ValidationError.errors()` 里的 `loc`、`msg`、`input` 逐条拼成清单后抛 `TypeError`。

**校验结果被写回实例再被读走。** `_validate_inputs` 成功后 `self._validated_data = validated_data.model_dump()`；`ascore` 里再 `func_kwargs = getattr(self, "_validated_data", {})` 去调用原函数。也就是说**原函数拿到的是经过 pydantic 转换后的值**（例如字符串 `"3"` 被注解成 `int` 时会被转成 `3`）。这是一个隐蔽但重要的行为：校验不只是检查，还带类型转换。

`_validated_data` 不是 dataclass 字段（没在类体里声明），靠 `getattr` 默认值容错。

**结果归一与两层兜底。** `ascore` 执行完原函数后：

1. 若结果不是 `MetricResult` 就 `MetricResult(value=result, reason=None)` 包一层——所以函数可以只 `return 0.8`。
2. `self._validate_result_value(result.value)` 做输出校验；有错误消息时**返回** `MetricResult(value=None, reason=校验消息)`，而不是抛错。越界值因此表现为「值为 `None`、理由说明越界」，而不是异常。
3. 任何异常都被 `except Exception` 捕获，转成 `MetricResult(value=None, reason=f"Error executing metric ...")`。

第二、三步把「失败」也变成一种结果值。这是这套设计最鲜明的取向：**指标函数不抛出，永远返回结果对象**。

**同步壳双路径。** `score` 里先试 `asyncio.get_running_loop()`：成功了说明在事件循环里，用 `ragas.async_utils.run` 跑；抛 `RuntimeError` 说明没有循环，用 `asyncio.run`。注意这里与 `BaseMetric.score` 的做法不同——后者在事件循环里直接报错，这里则试图用 nest_asyncio 兼容。两代实现在同一个问题上给了相反答案。

**`__call__` 绕过全部指标机制。** 它若 `_func` 已设置就直接调用原函数（异步函数返回协程），**完全不做输入校验、不做结果包装、不做输出校验**。这是一个逃生口：既能当指标用（`score`/`ascore`），也能当普通函数用（直接调用），两条路径的行为不同。

**`__repr__` 自描述。** 它用 `get_metric_type_name(allowed_values)` 得到类型名，输出形如 `my_metric(user_input, response) -> NumericMetric[(0.0, 1.0)]`——把形参名与取值域都打出来，便于在 notebook 里确认装饰是否正确。

**三个子类额外提供「类式」用法。** `NumericMetric` / `DiscreteMetric` / `RankingMetric` 都是 `SimpleLLMMetric` + 对应校验器的 dataclass，用于「用提示词 + LLM 而不是自定义函数」的场合。它们的 `__post_init__` 都调 `create_auto_response_model` 造结构化输出模型，且各自把 `reason` 与 `value` 的类型钉死：

- 数值：`reason: str`、`value: float`
- 离散：`value` 用 `t.Literal[tuple(allowed_values)]`——取值域被写进类型里，模型给别的值就过不了 pydantic 校验
- 排序：`value: List[str]`（列表长度由校验器事后查，不在类型里）

三者的 `get_correlation` 各用不同统计量：数值用 `scipy.pearsonr`，离散与排序用 `sklearn.cohen_kappa_score`（排序版还带 `weights="quadratic"` 并对每个标签对求平均）。也就是说**相关性度量与指标类型绑定**，这是「输出形态决定比较方法」的直接体现。

## 关键取舍

**源码事实：** 「哪种指标」由 `allowed_values` 的 Python 类型决定，不靠显式参数。传错类型（例如数值指标传了 `[0.0, 1.0]` 列表）会静默变成离散指标，且 `0.0 not in [...]` 之类的比较会失败。

**源码事实：** 输入校验严格拒绝位置参数，未知关键字只警告不报错，类型错误抛 `TypeError`。三种处理方式的差异来自「绕过校验」「可能的拼写错误」「真正的类型违约」三类问题的不同后果。

**源码事实：** 校验带类型转换（`model_dump` 后的结果被喂给原函数）。这意味着 `def f(x: int)` 被 `f(x="3")` 调用时拿到的是 `3` 而不是 `"3"`。

**源码事实：** 输出越界与执行异常都返回 `MetricResult(value=None, ...)`。因此「值为 `None`」是这套路线的统一失败标志，下游必须先判空再算术。

**源码事实：** `__call__` 直接透传到原函数，与 `score` 走完全不同的路径。

**架构推论：** 动态建类让每个被装饰函数拥有独立的类，于是校验器、pydantic 模型、错误消息都能按函数定制。代价是每次装饰都做一次 `create_model`，且这些类无法被静态检查工具分析——类型信息只在运行期存在。

**架构推论：** 把失败编码成 `MetricResult(value=None)` 而不是异常，使批量评分可以「部分失败而整体继续」。这正是 `batch_score` 的实现前提：它是一句列表推导 `[self.score(**d) for d in inputs]`，没有任何 try——能这样写，是因为单次调用自身不会抛（除了 pydantic 模型创建失败的极端情形，那时会 warn 并跳过校验）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/decorator.py` | `create_metric_decorator` | 三层闭包：按 `allowed_values` 动态建指标类 |
| `src/ragas/metrics/decorator.py` | `NumericMetricProtocol` | 数值指标的协议：`allowed_values` 为浮点区间 |
| `src/ragas/metrics/decorator.py` | `DiscreteMetricProtocol` | 离散指标的协议：`allowed_values` 为字符串列表 |
| `src/ragas/metrics/decorator.py` | `RankingMetricProtocol` | 排序指标的协议：`allowed_values` 为期望长度 |
| `src/ragas/metrics/decorator.py` | `CustomMetric._create_pydantic_model` | 由函数签名生成输入校验模型 |
| `src/ragas/metrics/decorator.py` | `CustomMetric._validate_inputs` | 拒位置参数、警告未知参数、写回校验结果 |
| `src/ragas/metrics/decorator.py` | `CustomMetric.ascore` | 归一为 `MetricResult`，两层失败兜底 |
| `src/ragas/metrics/numeric.py` | `NumericMetric` | 类式数值指标，输出模型为 `float` |
| `src/ragas/metrics/numeric.py` | `numeric_metric` | 装饰器入口，默认区间 (0.0, 1.0) |
| `src/ragas/metrics/discrete.py` | `DiscreteMetric` | 类式离散指标，取值域写进 `Literal` |
| `src/ragas/metrics/discrete.py` | `discrete_metric` | 装饰器入口，默认 pass/fail |
| `src/ragas/metrics/ranking.py` | `RankingMetric` | 类式排序指标，输出为字符串列表 |
| `src/ragas/metrics/ranking.py` | `ranking_metric` | 装饰器入口，默认为长度 2 |

## 参考

- [ragas 源码：metrics/decorator.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/decorator.py)
- [ragas 源码：metrics/numeric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/numeric.py)
- [ragas 文档：自定义指标](https://docs.ragas.io/en/stable/concepts/metrics/custom_metrics/)
- [Pydantic 文档：create_model 动态建模](https://docs.pydantic.dev/latest/concepts/models/#dynamic-model-creation)

## 与主核 / AI 的关系

「从函数签名自动生成校验模型」与主核里「按表定义生成行编解码」是同一种**声明驱动**：主核不手写每张表的读写函数，而是从 DDL 生成；装饰器不手写每个指标的入参校验，而是从签名生成。两者的收益相同——一致性与零重复；代价也相同——生成的实体（行布局 / 动态类）在静态分析里不可见，问题只能在运行期暴露。

差别在于严格度。主核的 DDL 一旦落盘就不能随意改（改要迁移）；这里的签名可以随手改，校验模型随之变化，历史调用的语义因此会漂移。**同一套「声明驱动」在数据库里是稳定性的来源，在评测脚本里是灵活性的来源。**

对 agent 工程最实用的三条：其一，用装饰器写自定义指标时，**形参名就是对外契约**——`score(user_input=..., response=...)` 的名字必须与函数签名一致，且必须用关键字调用。其二，返回值可以直接 `return 0.8`，但一旦越界会得到 `value=None` 而不是异常，聚合前必须判空。其三，`__call__` 与 `score` 行为不同，若同一对象在两条路径上被使用，会得到不一致的结果（一处被校验被包装，一处没有）。
