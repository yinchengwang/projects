---
project: smolagents
feature: error-taxonomy
title: 错误分类与重试
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/utils.py
    symbols: [AgentError, AgentParsingError, AgentMaxStepsError]
  - path: src/smolagents/utils.py
    symbols: [RateLimiter, Retrying]
---

# 错误分类与重试

## 一句话职责

把 agent 的失败分成一棵七类的小继承树，每类都自带「构造时就把错误写进日志」的副作用；同时提供两个与错误无关但常与错误一起出现的控制器——限流器与重试器。

## 机制是什么

错误基类是 `AgentError(Exception)`，构造签名是 `(message, logger)`——**必须传 logger**。构造函数体里除了调用 `super().__init__(message)` 与存 `self.message`，还多一句 `logger.log_error(message)`。

所以「造一个 AgentError」这个动作本身就往控制台写了一条红色错误。这是一个很特别的副作用：错误对象不能在不产生输出的情况下被构造。

`dict()` 方法返回 `{"type": 类名, "message": str(message)}`——用于把错误存进记忆步骤（`ActionStep.error`）。

七个子类：

- `AgentParsingError`：解析失败
- `AgentExecutionError`：执行失败
- `AgentMaxStepsError`：步数超限
- `AgentToolCallError`（继承 `AgentExecutionError`）：传参错误
- `AgentToolExecutionError`（继承 `AgentExecutionError`）：工具执行错误
- `AgentGenerationError`：生成失败

加上基类共七个，构成两层继承：`ExecutionError` 有两个子类，其余四个是 `AgentError` 的直接子类。

## 怎么实现的

**分类的语义边界。** 这套分类对应的其实是「失败发生在哪个阶段」：

- 解析（模型输出看不懂）→ `AgentParsingError`
- 生成（模型调用本身失败）→ `AgentGenerationError`
- 执行（代码或工具跑挂了）→ `AgentExecutionError`，细分成「参数不对」与「执行出错」
- 步数（跑太久）→ `AgentMaxStepsError`

关键在于：**只有 `AgentParsingError` 与 `AgentExecutionError` 通常可恢复**。解析失败可以重试（让模型重新输出），执行失败可以把错误反馈给模型让它换方法；而 `AgentMaxStepsError` 是终止条件、`AgentGenerationError` 是外部故障。

这个区分在上层被用来决定「继续循环还是终止」。而错误消息进 `ActionStep.error` 后会被 `to_messages` 渲染成带指导语的 TOOL_RESPONSE（见 f01），于是「重试」这件事变成模型看到的提示而不是控制流的决定。

**`logger` 必须在构造时传。** 这带来一个实际的类型问题：任何要抛 `AgentError` 的地方都必须持有 logger。所以 `raise AgentError(msg, self.logger)` 是标准形态——错误类型与日志设施的耦合被写进了构造函数签名。

代价是「先造错误对象晚点再抛」这种写法无法使用（除非手里有 logger），且测试时要构造 `AgentLogger` 而无法省掉。

**`RateLimiter` 的最小实现。** 构造时 `_enabled = requests_per_minute is not None`，`_interval = 60.0 / requests_per_minute if _enabled else 0.0`，`_last_call = 0.0`。

`throttle()` 的逻辑是：未启用直接返回；否则取当前时间，若距上次调用的间隔小于 `_interval` 就 `time.sleep` 补齐，然后更新 `_last_call`。

注意 `_last_call = 0.0` 这个初值：`time.time()` 的返回值远大于 60，所以首次调用时 `elapsed` 极大，不会 sleep。这让「第一个请求不等」成为默认行为。

它不处理并发：`_last_call` 的读改写没有锁。多线程共用同一个实例时，两个线程可能同时读到旧值、同时决定不 sleep，于是实际速率略高于设定。对「限流是为了别被服务端打死」这个目的，这点宽松是可接受的。

**`Retrying` 的结构。** 构造参数七个：`max_attempts`、`wait_seconds`、`exponential_base`、`jitter`、`retry_predicate`、`reraise`、以及两个日志钩子 `before_sleep_logger` 与 `after_logger`（各是 `(Logger, level)` 元组）。

`__call__(fn, *args, **kwargs)` 把可调用体包起来重试。这决定了它的调用形状：`retryer(self.client.completion, **kwargs)` 而不是当装饰器用。

循环 `for attempt_number in range(1, max_attempts + 1)`：

- 成功：若 `attempt_number > 1` 且有 after_logger，记一条「总耗时 + 第几次尝试」；然后返回结果。
- 失败：算 `should_retry = self.retry_predicate(e) if self.retry_predicate else False`；若不该重试或已到上限，`raise`；否则记 after 日志、算退避、记 before sleep 日志、`time.sleep(delay)`。

**退避的乘法结构。** `delay *= self.exponential_base * (1 + self.jitter * random.random())`。

`delay` 初值是 `wait_seconds`，每次失败后**累乘**。所以第 n 次重试前的等待是 `wait_seconds * ∏(base * (1 + jitter*rand))`。抖动的因子范围是 `1` 到 `1 + jitter`（默认 `jitter=True` 即布尔真，数值上等于 1，所以因子在 1 到 2 之间）。

注意 `jitter` 的类型标注是 `bool` 但参与乘法——`True` 会被当作 `1`。所以「关闭抖动」得到因子恒为 1，「开启抖动」得到 1 到 2 之间的随机。**抖动只向上抖动**，实际等待总是 ≥ 名义延迟。

**`reraise` 是个无效参数。** 失败分支的两条路都是 `raise`：

```python
if not should_retry or attempt_number >= self.max_attempts:
    if self.reraise:
        raise
    raise
```

所以无论 `reraise` 是真是假，行为相同。这是一个「保留签名但未实现分支」的痕迹——大概原本打算让 `reraise=False` 时返回 `None` 或包装异常。

**日志的两个时机。** after_logger 在两处调用：成功后（已重试过）与失败且将重试时。before_sleep_logger 只在失败将重试时。两者都用 `fn_name = getattr(fn, "__name__", repr(fn))` 取名字——传的是绑定方法，`__name__` 通常存在，取不到就退回 `repr` 避免日志崩掉。

## 关键取舍

**源码事实：** `AgentError.__init__` 带 `logger` 参数并立即调 `logger.log_error(message)`。所以构造错误 = 打印错误，无法分离。

**源码事实：** 七个错误类中只有 `AgentToolCallError` 与 `AgentToolExecutionError` 有继承关系（都继承 `AgentExecutionError`）。其余四个直接继承基类，彼此平行。

**源码事实：** `RateLimiter` 的 `_last_call` 初值是 `0.0`，保证第一个请求不等待。

**源码事实：** `Retrying` 的 `reraise` 参数当前不产生行为差异——两条分支都 `raise`。

**源码事实：** 退避抖动只向上（因子 1 到 2），所以实际等待不小于名义值。

**源码事实：** `jitter` 声明为 `bool` 但参与数值乘法，依赖 `True == 1` 这个语言特性。

**架构推论：** 「构造即日志」的设计把「错误被记录」从「调用方记得记录」变成了「类型保证」。任何被抛出的 `AgentError` 都必然已经在日志里出现过一次，不会出现「异常被上层捕获但从未记录」的黑洞。代价是错误对象不可复制、不可测试性地构造，且在需要「先攒着最后统一报」的场景下无法使用。

**架构推论：** 错误分类的层级其实是「可恢复性」的层级。`AgentExecutionError` 有子类是因为上层对它的处理方式相同（都是把消息喂回模型重试），但需要区分给日志看。而 `AgentMaxStepsError` 与 `AgentGenerationError` 直接挂基类，因为它们走完全不同的终止路径。这个形状不是分类学上的整洁，而是消费侧需求的投影。

**架构推论：** `RateLimiter` 与 `Retrying` 放在 `utils.py` 而不是 `models.py`，说明它们被视为通用工具而非模型专属。但只有 `ApiModel` 用它们——本地推理不需要。这个摆放位置与实际用途之间的偏差不多，属于「按机制归类」而非「按使用方归类」的组织方式。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/utils.py` | `AgentError` | 错误基类，构造时即写日志 |
| `src/smolagents/utils.py` | `AgentParsingError` | 模型输出无法解析 |
| `src/smolagents/utils.py` | `AgentExecutionError` | 代码或工具执行失败 |
| `src/smolagents/utils.py` | `AgentMaxStepsError` | 步数超限的终止错误 |
| `src/smolagents/utils.py` | `AgentToolCallError` | 工具参数错误，继承执行错误 |
| `src/smolagents/utils.py` | `AgentToolExecutionError` | 工具执行错误，继承执行错误 |
| `src/smolagents/utils.py` | `AgentGenerationError` | 模型调用失败 |
| `src/smolagents/utils.py` | `RateLimiter` | 最小间隔限流，`None` 即关闭 |
| `src/smolagents/utils.py` | `Retrying` | 指数退避加无抖动的重试器 |

## 参考

- [smolagents 源码：utils.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/utils.py)
- [smolagents 文档：Agents](https://huggingface.co/docs/smolagents/main/en/reference/agents)
- [tenacity 文档：重试策略](https://tenacity.readthedocs.io/en/latest/)

## 与主核 / AI 的关系

主核里最接近的是错误码体系：SQLSTATE 把「语法错误」「约束冲突」「死锁」「连接断开」分门别类，上层据此决定重试、回滚还是报给用户。这里的七类承担同一职能，但覆盖面不同——主核的错误码描述「引擎内部发生了什么」，这里的类别还包含「模型输出不符合预期」这类**调用方（模型）用错了**的情况。

与 CrewAI 的 `ToolFailureReason` 相比（crewai 的 f05 页），两者的分类动机相近但结构不同。CrewAI 用一个枚举表达七类理由，配套三档策略（忽略/警告/抛出）与四级作用域（工具/任务/agent/团队）回退。smolagents 用继承层次表达分类，没有策略层——错误抛出后由 agent 主循环统一处理。前者适合「细粒度控制单个工具的失败行为」，后者适合「统一的错误恢复循环」。这个差异来自两种 agent 范式的不同：CrewAI 是任务编排（每个工具失败要独立决策），smolagents 是单循环（所有失败都喂回模型）。

对 AI 工程最实用的一条是「构造即日志」。它保证了可观测性不会因为某个 `except` 分支忘了记录而丢失。任何在深层被抛出、又在浅层被捕获的异常，最大的风险是「捕获后静默处理」——日志里什么都没有，问题无从追查。把这个动作放进构造函数，等于给错误类型加了一条不可绕过的约束。代价是灵活性下降，所以更适合「错误最终都会被记录」的架构（如这里的 agent 循环）。

另一条是 `RateLimiter` 的初值选择。`_last_call = 0.0` 让第一个请求不等待，这个细节很容易做错——若初值设成 `time.time()`，首个请求就会被无谓地延迟一个间隔。凡是「基于上次时间戳做节流」的实现，都要想清楚「首次调用应该等多久」，答案几乎总是「不等」。
