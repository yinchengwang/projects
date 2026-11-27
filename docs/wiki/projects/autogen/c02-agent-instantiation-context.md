---
project: autogen
feature: agent-instantiation-context
title: 实例化上下文与订阅延迟绑定
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_agent_instantiation.py
    symbols: [AgentInstantiationContext]
  - path: python/packages/autogen-core/src/autogen_core/_subscription_context.py
    symbols: [SubscriptionInstantiationContext]
---

# 实例化上下文与订阅延迟绑定

## 一句话职责

用两个相互独立的 `ContextVar` 把「此刻正在被谁创建」与「此刻正在为哪个类型建订阅」注入当前协程，让工厂和订阅回调不必接收参数就能知道运行时、`AgentId` 与 `AgentType`。

## 机制是什么

两个上下文类形状几乎一致，都只能通过类方法访问（构造函数直接抛错）：

| | `AgentInstantiationContext` | `SubscriptionInstantiationContext` |
|---|---|---|
| 载体 | `ContextVar[tuple[AgentRuntime, AgentId]]` | `ContextVar[AgentType]` |
| 写入方 | 运行时在调工厂时 `populate_context((self, agent_id))` | 注册路径在求值订阅工厂时 `populate_context(agent_type)` |
| 读取方 | `BaseAgent.__init__`、`ClosureAgent.__init__`、工厂函数 | `DefaultSubscription.__init__` |
| 缺失时 | `RuntimeError`（提示不要直接实例化） | `RuntimeError`，`DefaultSubscription` 再包成 `CantHandleException` |

`populate_context` 是 `@contextmanager`：set 返回 token，`finally` 里 `reset(token)`。因此上下文是**严格嵌套**的——异常、提前 return、`await` 都不会让它泄漏到外层，也不会在并发任务之间串味（`ContextVar` 天然按任务隔离）。

`AgentInstantiationContext` 多一个 `is_in_factory_call()`，用 `get(None)` 判断是否处于上下文里。这一个方法把「构造期可选绑定」变成可能：`BaseAgent.__init__` 先问一句「现在在工厂里吗」，在就直接取 runtime 与 id，不在就什么都不做，等注册时 `bind_id_and_runtime` 来补。

## 怎么实现的

**写入点在运行时的工厂调用路径上。** `SingleThreadedAgentRuntime._invoke_agent_factory` 用 `with AgentInstantiationContext.populate_context((self, agent_id)):` 包住整段工厂执行，然后按工厂签名分派：0 参数直接调用，2 参数走已弃用路径并 `warnings.warn`，其它数量抛 `ValueError("Agent factory must take 0 or 2 arguments.")`。若工厂返回可等待对象再 `await` 一次。异常路径会记 `AgentConstructionExceptionEvent` 与 error 日志后重抛。

关键一点：上下文覆盖了整个工厂调用（含 `await`），所以异步工厂里的任意深度都能读到同一个 runtime 与 id。

**两个参数的旧工厂已被降级。** 2 参数形式 `(runtime, agent_id)` 仍可用但会告警，文案写明「将来会移除」。新的推荐形式是 0 参数 + 上下文取值。这解释了为什么上下文必需：它是把「显式传参」换成「隐式注入」的那一步。

**订阅上下文只带类型，不带运行时。** `SubscriptionInstantiationContext.agent_type()` 返回 `AgentType`，没有 runtime。原因是订阅工厂唯一需要的信息就是「这些订阅属于哪个 agent type」——`DefaultSubscription` 要把 `agent_type` 填进 `TypeSubscription`，而运行时由调用方（`BaseAgent.register`）自己拿着。

**`populate_context` 的键是类型本身。** `ClassVar[ContextVar[AgentType]]`，注意泛型参数是 `AgentType` 而不是 `str`。`DefaultSubscription` 里写 `SubscriptionInstantiationContext.agent_type().type`，说明取出来的是包装对象。

**多处写入点共用同一个箭头。** 除了 `register`，`register_instance`、`ClosureAgent.register_closure` 也各自 `populate_context`。因此「订阅求值必须在上下文里」这条约定在任何注册路径上都被满足，`DefaultSubscription()` 无需显式传 `agent_type`。

**上下文与 `MessageHandlerContext` 是并行的三套。** 除上面两个，还有 `MessageHandlerContext`（`ContextVar[AgentId]`）在投递时写入，供 `DefaultTopicId()` 在被处理器调用时推断 source。三套上下文互不复用，各管一个阶段：实例化、订阅构造、消息处理。

## 关键取舍

**源码事实：** 上下文用 `ContextVar` 而不是模块级全局变量。这让同一进程内多个运行时、多个并发实例化可以共存而不互相覆盖——异步并发下，全局变量做不到这一点。

**源码事实：** `populate_context` 带 `reset(token)`。因此嵌套调用（例如工厂里又触发另一实例化）能正确恢复外层值，而不是「内层覆盖后不回退」。

**源码事实：** 两种缺失上下文的报错都指向同一个因果：直接 `new` 一个智能体。文案明确写「多半是因为直接实例化了智能体，而不是让运行时来实例化」。

**源码事实：** `is_in_factory_call()` 用 `get(None)` 而不是 try/except，是唯一一个「缺失即返回假」的读取点。它让 `BaseAgent.__init__` 能在两种路径下都安全执行，不必包 try。

**源码事实：** 运行时对工厂签名的处理是「0 或 2 参数，其它一律报错」，并对 2 参数告警。这说明上下文机制是在 2 参数方案之上加的替换层，且替换尚未完成——两条路径并存。

**架构推论：** 用上下文而不是把 runtime/id 塞进工厂参数，使工厂可以保持「零依赖」的函数形态（`lambda: MyAgent(...)`）。构造参数与运行时注入被彻底分开：用户在工厂里传业务参数，框架在上下文里传身份。

**架构推论：** 订阅构造用独立上下文而不是复用实例化上下文，避免了「订阅需要在实例化期间构造」这条伪依赖。实际上两者时序不同：实例化发生在投递时（懒加载），订阅发生在注册时。共用一个 `ContextVar` 会让「注册时求值订阅」与「投递时创建实例」两个阶段互相污染。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_agent_instantiation.py` | `AgentInstantiationContext` | 实例化上下文：runtime 与 AgentId |
| `python/packages/autogen-core/src/autogen_core/_agent_instantiation.py` | `AgentInstantiationContext.populate_context` | `@contextmanager` 写入并按 token 复位 |
| `python/packages/autogen-core/src/autogen_core/_agent_instantiation.py` | `AgentInstantiationContext.current_runtime` | 取当前运行时，缺失抛 `RuntimeError` |
| `python/packages/autogen-core/src/autogen_core/_agent_instantiation.py` | `AgentInstantiationContext.current_agent_id` | 取当前要创建的 AgentId |
| `python/packages/autogen-core/src/autogen_core/_agent_instantiation.py` | `AgentInstantiationContext.is_in_factory_call` | 唯一「缺失即返回假」的探测点 |
| `python/packages/autogen-core/src/autogen_core/_subscription_context.py` | `SubscriptionInstantiationContext` | 订阅构造上下文，只带 `AgentType` |
| `python/packages/autogen-core/src/autogen_core/_subscription_context.py` | `SubscriptionInstantiationContext.populate_context` | 同样的 set/reset 模式 |
| `python/packages/autogen-core/src/autogen_core/_subscription_context.py` | `SubscriptionInstantiationContext.agent_type` | 取当前注册类型，缺失抛 `RuntimeError` |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `_invoke_agent_factory` | 唯一写入实例化上下文的运行时实现 |

## 参考

- [AutoGen 源码：_agent_instantiation.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_agent_instantiation.py)
- [AutoGen 源码：_subscription_context.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_subscription_context.py)
- [Python 文档：contextvars](https://docs.python.org/3/library/contextvars.html)
- [AutoGen 文档：Agent 标识与生命周期](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/agent-identity-and-lifecycle.html)

## 与主核 / AI 的关系

这套上下文注入与主核里「当前事务 xid 随上下文传递」是同一类设计：把横切的执行环境挂在隐式上下文上，而不是逐层显式传参。主核用显式传参的地方，都是希望调用方能看见依赖的地方；这里选择隐式，是因为工厂签名要保持干净。

对 AI 工程有三条具体结论。第一，`ContextVar` 的选择直接决定了「同一进程能不能同时跑多个运行时」。用全局变量收集当前运行时的框架，在并发下会互相串扰；这是判断框架成熟度的一条硬指标。第二，订阅上下文独立于实例化上下文，意味着订阅规则在注册期就定型、与实例何时被创建无关；因此「改了订阅却没生效」的现象应当去查注册路径，而不是等第一条消息投递。第三，0 参数工厂加隐式注入是推荐路径，2 参数旧路径虽仍可用但已标弃用；新代码若要兼容多版本，应只依赖 `AgentInstantiationContext` 而不依赖工厂参数。
