---
project: autogen
feature: agent-proxy
title: 代理与跨智能体调用
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_agent_proxy.py
    symbols: [AgentProxy, send_message, save_state]
---

# 代理与跨智能体调用

## 一句话职责

把一个 `AgentId` 加一层薄壳，使它用起来像一个智能体：可以 `send_message`、可以存取状态，调用面与实体一致，但真正的解析工作全部交给运行时。

## 机制是什么

`AgentProxy` 只有两个字段：目标 `AgentId`（`_agent`）与运行时引用（`_runtime`）。它对外提供：

| 成员 | 形态 | 实现 |
|---|---|---|
| `id` | 属性 | 直接返回目标 `AgentId` |
| `metadata` | 属性，返回 `Awaitable[AgentMetadata]` | 转给 `runtime.agent_metadata(_agent)` |
| `send_message` | 协程 | 转给 `runtime.send_message(..., recipient=_agent, sender=...)` |
| `save_state` | 协程 | 转给 `runtime.agent_save_state(_agent)` |
| `load_state` | 协程 | 转给 `runtime.agent_load_state(_agent, state)` |

它是纯转发：没有缓存、没有重试、没有本地实例引用。所有动作都穿过运行时接口，因此在分布式实现里这些调用会变成 RPC，而调用方代码不变。

## 怎么实现的

**`send_message` 的 `sender` 是必填关键字参数。** 签名是 `async def send_message(self, message, *, sender: AgentId, ...)`——`sender` 没有默认值，必须显式给。这与 `AgentRuntime.send_message` 允许 `sender=None` 不同：代理是给「智能体之间互相调用」用的，所以强制要求调用方自报身份；无名调用只属于运行时外部注入这一种场景。

**`metadata` 是 `Awaitable` 而不是 `async def`。** 声明成 property 并返回 `self._runtime.agent_metadata(...)` 的调用结果，于是用法变成 `await proxy.metadata`。这是一个形态上的选择：与 `BaseAgent.metadata`（同步属性、返回 `AgentMetadata`）名字相同但用法不同，代码从实体切到代理时，`await` 必须补上。

**不需要状态的两处：** 代理没有 `load_state` 之外的写操作，也没有 `close`。`close` 的缺席很关键——关闭一个代理绝不能理解为「关掉目标智能体」，因为目标的生命周期由运行时管理。省略这个方法，等于在接口层拒绝了这个语义。

**这几个转发在两种实现下并不等价。** 单机实现的三个方法都会先 `_get_agent(agent_id)`，也就是**会拉起实例**；而 autogen-ext 的 grpc worker 运行时里，`agent_metadata`、`agent_save_state`、`agent_load_state` 三个方法都直接 `raise NotImplementedError`。也就是说：`AgentProxy.send_message` 跨两种部署形态都能用，`metadata`/`save_state`/`load_state` 目前只在单机实现下真正可用。判断语义必须看实现，不能只看代理自己的文档。

**`__init__` 不校验目标存在。** 构造 `AgentProxy(AgentId(...), runtime)` 时不检查该 id 是否已注册，错误一律推迟到第一次真正调用。这让「先造代理、后注册目标」的装配顺序可行。

**与运行时的 `send_message` 完全同形。** 逐参数比对可以看出代理只是把 `recipient=_agent` 固定住，其余 `cancellation_token`、`message_id` 原样透传。所以取消与幂等语义没有在代理层被削弱或增强。

## 关键取舍

**源码事实：** 代理本身零依赖边界。它只持有 `AgentId` 与运行时引用，所以同一个 `AgentProxy` 表达式在单机与分布式两种部署下都能通过构造与调用；但 `metadata`/`save_state`/`load_state` 这三个转发在 grpc worker 运行时上是未实现的（直接 `NotImplementedError`），只有 `send_message` 跨形态可用。

**源码事实：** `metadata` 做成 property 返回 `Awaitable`，而不是 `async def metadata(self)`。这使「取 metadata」在语法上像读属性、在使用上要 `await`，是最容易被写错的一处——忘写 `await` 会拿到一个协程对象而不报错（如果是 `async def`，忘 `await` 同样拿到协程，但至少类型检查器更容易发现）。

**源码事实：** `send_message` 强制 `sender`，不接受 `None`。这把「代理代表谁说话」变成必须显式声明的事实，避免了「同一个代理被不同调用方使用却都记成同一个发送者」的溯源错误。

**源码事实：** 代理没有 `close`，也没有 `publish_message`。后者尤其值得注意：广播的语义是「发给一批未知的收件人」，而代理绑定的恰恰是一个已知地址，两者在概念上冲突，因此不在代理上暴露。

**架构推论：** 代理把「地址」提升成「可调用的对端」，使上层代码不必到处写 `runtime.send_message(msg, recipient=id, sender=self.id)`。收益是可读性与统一性；代价是多了一层透明转发，调试栈会多一帧。

**架构推论：** 因为代理不缓存任何东西，多次 `await proxy.metadata` 会重复走运行时、单机实现下重复 `_get_agent`。它不是「远程引用」那种带缓存的桩对象，而是「每次现查的绑定」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_agent_proxy.py` | `AgentProxy` | 绑定目标地址的薄壳 |
| `python/packages/autogen-core/src/autogen_core/_agent_proxy.py` | `AgentProxy.__init__` | 只存 `AgentId` 与 runtime，不做存在校验 |
| `python/packages/autogen-core/src/autogen_core/_agent_proxy.py` | `AgentProxy.id` | 返回目标地址 |
| `python/packages/autogen-core/src/autogen_core/_agent_proxy.py` | `AgentProxy.metadata` | property 返回 `Awaitable`，转 `agent_metadata` |
| `python/packages/autogen-core/src/autogen_core/_agent_proxy.py` | `AgentProxy.send_message` | 强制 `sender`，其余透传运行时 |
| `python/packages/autogen-core/src/autogen_core/_agent_proxy.py` | `AgentProxy.save_state` | 转 `agent_save_state`，仅单机实现可用 |
| `python/packages/autogen-core/src/autogen_core/_agent_proxy.py` | `AgentProxy.load_state` | 转 `agent_load_state`，仅单机实现可用 |

## 参考

- [AutoGen 源码：_agent_proxy.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_agent_proxy.py)
- [AutoGen 源码：_agent_runtime.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_agent_runtime.py)
- [AutoGen 文档：分布式 Agent Runtime](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/distributed-agent-runtime.html)

## 与主核 / AI 的关系

代理与主核里「跨实例的连接句柄」是同一个概念：本地只持有一个标识与一条通往远端的通道，真实对象在哪、活着没活着由对方决定。主核的连接句柄要做序列化与超时管理，这里的代理把这些全交给运行时，自己只留一个转发面。

对 AI 工程有两条实践判断。第一，代理只在**跨边界**时提供真正的价值；单机部署里它只是语法糖，而且 `metadata`/`save_state` 仍会拉起本地实例，性能上不会更省。第二，`send_message` 强制 `sender` 是好事：多智能体系统的可观测性几乎完全依赖「谁发给谁」，把发送者变成必填参数，比事后补字段可靠得多。若要把一个代理复用给多个逻辑调用方，必须为每个调用方准备不同的 `sender`，否则链路追踪里所有调用会被归到同一个发送者。

还有一条隐含边界：代理不隔离生命周期。删掉代理对象不会影响目标实例，`close` 的缺席正是这条约定的体现。反过来说，想「停掉某个智能体」也不能通过代理做到——那属于运行时的能力，而 `AgentRuntime` 协议里并没有单智能体销毁接口。