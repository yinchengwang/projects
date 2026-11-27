---
project: smolagents
feature: callbacks
title: 步骤回调注册
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/memory.py
    symbols: [CallbackRegistry, register, callback]
  - path: src/smolagents/agents.py
    symbols: [_setup_step_callbacks]
---

# 步骤回调注册

## 一句话职责

按「步骤类型」注册回调，在步骤产生时按类型分派调用；并且用一次签名探测同时兼容「只收步骤」的老回调和「收步骤加额外参数」的新回调，让监控指标这类内部消费者与用户回调共用一个注册表。

## 机制是什么

`CallbackRegistry` 只持有一个字典：`dict[Type[MemoryStep], list[Callable]]`——步骤类到回调列表的映射。

两个方法：

- `register(step_cls, callback)`：把回调追加到该类对应的列表（列表不存在就新建）。同名类可以注册多个回调，按注册顺序保存。
- `callback(memory_step, **kwargs)`：按步骤实例的**类型层次**分派调用。

分派方式是这一页的核心：遍历 `memory_step.__class__.__mro__`，对每一层类取注册列表并逐个调用。所以注册在 `MemoryStep` 基类上的回调，会对所有步骤生效；注册在 `ActionStep` 上的只对动作步骤生效。

## 怎么实现的

**MRO 遍历带来的「继承自动生效」。** 用 `type(memory_step).__mro__` 而不是直接查 `type(memory_step)`，意味着**父类注册的回调会被子类实例触发**。这不是副作用而是设计目的：`MemoryStep` 是所有步骤的基类，所以注册在它上面的回调是「全局钩子」；注册在 `ActionStep` 上的是「动作专用钩子」。

同时这也带来一个必然结果：若同一个可调用对象同时注册在父类与子类上，它会被调用**两次**。代码里没有去重，因为「按类型层次各调一次」的语义是明确的——每个注册点代表一次独立订阅。

**签名探测决定调用形状。** 每个回调被调用时的形式由一句三元表达式决定：

```python
cb(memory_step) if len(inspect.signature(cb).parameters) == 1 else cb(memory_step, **kwargs)
```

参数个数恰好为 1 时只传步骤；否则连同 `**kwargs` 一起传。docstring 解释了这是「为了向后兼容，单参数签名的回调只收 memory_step，多参数签名的回调收步骤加额外 kwargs」。

这个兼容策略的关键在「恰好等于 1」而不是「大于 1」：单参数是旧形态，多参数是新形态。所以一个 `lambda step: ...` 与一个 `def f(step, agent=None): ...` 能共存于同一注册表。

`kwargs` 的典型内容是 `agent`——从 `_setup_step_callbacks` 的上下文看，主循环在产生步骤时会把当前 agent 传进去，让回调可以访问 agent 状态。

**agent 侧的装配。** `_setup_step_callbacks(step_callbacks)` 是新 agent 构造时调用的方法，逻辑分三段：

1. 先建一个空 `CallbackRegistry`。
2. 按传入的 `step_callbacks` 形态分派注册。若是 **list**，则把每个回调都注册给 `ActionStep`——注释写明「仅为 `ActionStep` 注册，保持向后兼容」，即旧 API 只有「每步回调」这个概念，那时唯一的「步」就是动作步骤。若是 **dict**，则键是步骤类、值是回调（或回调列表，代码里做了 `if not isinstance(callbacks, list): callbacks = [callbacks]` 的归一）。两者都不是则 `ValueError`。
3. **无条件**把 `self.monitor.update_metrics` 注册给 `ActionStep` 与 `PlanningStep`。

第 3 步的注释解释了为什么规划步骤也要注册监控：「让 monitor 的合计包含规划 token，与 `RunResult.token_usage` 保持一致」。

**监控复用回调机制。** 这一处设计值得单独看：`Monitor.update_metrics(step_log)` 的签名是单参数，所以它走的是「旧回调」路径，只收到步骤对象。它做的事是从 `step_log.timing.duration` 取耗时、从 `step_log.token_usage` 累加 token（若有），然后打一行 `[Step N: Duration X.XX seconds| Input tokens: ... | Output tokens: ...]` 的 dim 样式文本。

所以「监控」不是一个独立的观测通道，而是**注册在回调表里的一个普通消费者**。这让「步骤产生」只有一条分发路径——所有关心步骤的东西（用户回调、监控）都从同一点拿到通知。

**步骤号来自监控而非步骤本身。** 注意 `update_metrics` 里打印的步骤号是 `len(self.step_durations)`（即列表长度加一之后的数），而不是 `step_log.step_number`。这是一个有意思的选择：监控维护自己的计数，不依赖步骤对象的字段。副作用是若某类步骤不触发这个回调（例如 `PlanningStep` 在早期版本未注册），计数就会与实际步号错位。

## 关键取舍

**源码事实：** 分派走完整 MRO，所以父类注册的回调会被所有子类实例触发，且同一回调注册两次会被调用两次（没有去重）。

**源码事实：** 调用形状由 `len(signature.parameters) == 1` 决定。这意味着一个「只定义一个位置参数但带默认值的第二参数」的回调（`def f(step, agent=None)`）会被当成多参数回调，收到 `agent` kwarg；而一个 `*args` 签名的回调参数个数在 `signature` 里是 1（只有一个 VAR_POSITIONAL 项），会被误判成单参数回调。

**源码事实：** list 形态的 `step_callbacks` 一律注册给 `ActionStep`，不能覆盖其他步骤类型。要按类型注册必须用 dict 形态。

**源码事实：** `Monitor.update_metrics` 被无条件注册，用户无法通过 `step_callbacks` 参数关掉它。所以「只要用了 MemoryStep 体系就一定有监控输出」，除非把 logger 级别调到 OFF。

**架构推论：** 「按类型层次分派」比「精确类型匹配」更适合这个场景，因为调用方关心的是「这类步骤发生时通知我」，而不是「只有恰好这个类型才通知」。代价是层次关系的意外影响——例如把回调注册到 `MemoryStep` 基类上会连带 `SystemPromptStep` 与 `FinalAnswerStep` 也触发（若它们进过 `callback()`），这未必是注册者预期的。

**架构推论：** 用 `inspect.signature` 做能力探测是 Python 里处理「新旧两种回调签名」的标准手法，但它依赖「参数个数」这个脆弱的代理指标。更稳的做法是显式声明版本（例如 `callback.__accepts_context__ = True`）或统一成 `**kwargs` 签名。这里选签名探测，是因为不想让用户为了兼容而改写回调。

**架构推论：** 把监控做成回调的消费者，让「步骤产生」只有一个出口。若监控走独立路径（比如在主循环里直接调 `monitor.update_metrics`），那么「用户回调」与「监控」的执行顺序、异常隔离、以及「是否注册了」都会变成两套逻辑。统一成一个注册表，代价是所有消费者共享同一条执行序列与同一个异常行为。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/memory.py` | `CallbackRegistry` | 按步骤类注册与分派的回调表 |
| `src/smolagents/memory.py` | `CallbackRegistry.register` | 追加回调到某步骤类的列表 |
| `src/smolagents/memory.py` | `CallbackRegistry.callback` | 按 MRO 分派并做签名兼容 |
| `src/smolagents/agents.py` | `_setup_step_callbacks` | 按 list/dict 形态注册并内置监控回调 |
| `src/smolagents/monitoring.py` | `Monitor.update_metrics` | 单参数回调：累计耗时与 token 并打印 |
| `src/smolagents/monitoring.py` | `Monitor.get_total_token_counts` | 汇总用量为 `TokenUsage` |

## 参考

- [smolagents 源码：memory.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/memory.py)
- [smolagents 源码：agents.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/agents.py)
- [smolagents 文档：Callbacks](https://huggingface.co/docs/smolagents/main/en/reference/agents)

## 与主核 / AI 的关系

主核里最接近的是「事件触发器 + 监听器」：触发器按事件类型注册，事件发生时按类型分派。差别在于主核通常区分「精确匹配」与「继承匹配」两种触发粒度，并允许显式选择；这里只提供继承匹配（MRO），粒度靠「注册在哪个类上」隐式决定。

与 CrewAI 的事件总线相比，这层极简得多：没有依赖排序、没有同步/异步分流、没有重放、没有异常隔离。`CallbackRegistry.callback` 里回调抛出的异常会**直接冒出去**——一个写错用户回调会让主循环崩掉。这是一个明确的取舍：回调被视为「用户自己的代码」，其错误归用户负责；而 CrewAI 的处理器被视为「观测设施」，其错误必须被吞掉。两种定位导致两种异常策略。

对 AI 工程最实用的一条是「监控复用回调机制」。它避免了「核心逻辑里到处调埋点函数」这种耦合。任何要往主循环里加观测的地方，都可以先问：能不能做成一个注册在现有分发点上的消费者？如果能，核心逻辑就不用改，观测逻辑也可以独立开关。

另一条是签名探测的脆弱性。`len(signature.parameters) == 1` 这条判据在遇到 `*args`、`**kwargs`、以及带默认值参数的函数时都会给出与直觉不同的结果。做同类兼容时，更可靠的方式是「先试新签名，捕获 TypeError 再退回旧签名」或者显式标记。把「参数个数」当作能力探测的代理，只在回调形态被严格约束时才是安全的——这里恰好是这样（要么一个参数，要么两个且第二个是命名参数），但它对用户并不显式。
