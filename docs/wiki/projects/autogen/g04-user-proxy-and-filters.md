---
project: autogen
feature: user-proxy-and-filters
title: 用户代理与消息过滤智能体
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/agents/_user_proxy_agent.py
    symbols: [UserProxyAgent, on_messages]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/agents/_message_filter_agent.py
    symbols: [MessageFilterAgent, _apply_filter]
---

# 用户代理与消息过滤智能体

## 一句话职责

两个都实现 `ChatAgent` 却几乎不做推理的智能体：`UserProxyAgent` 把「人」伪装成一个可被团队调度的参与者，`MessageFilterAgent` 把「上游历史」裁剪成下游智能体该看到的那一部分。

## 机制是什么

`UserProxyAgent` 的唯一实质行为是「请求用户输入并把输入包成消息」。它的输入源是可注入的 `input_func`，签名为 `Callable[[str], str]` 或 `Callable[[str, CancellationToken | None], Awaitable[str]]`，默认是 `cancellable_input`——把内建的 `input` 放到线程里跑，并把 asyncio 任务挂到取消令牌上。

`MessageFilterAgent` 是一个装饰器式的包装器：持有 `wrapped_agent` 与一份 `MessageFilterConfig`，每次收到消息先按配置过滤，再把过滤后的序列交给被包装的智能体。它的 `produced_message_types` 直接委托给内层智能体，`on_reset` 也直接转发，因此对外看它就是内层智能体。

## 怎么实现的

**用户代理的手交接缝。** `_get_latest_handoff` 只看**最后一条**消息：如果它既不是 `HandoffMessage` 就返回 `None`；如果是但没有指向本代理（`target != self.name`），直接 `RuntimeError`。这个严格性是有意的——交接消息是「把控制权交给某个参与者」的承诺，落到错误的人手里说明上游选错了目标，不能静默忽略。

**提示语随情境变化。** 有交接时提示语是 `Handoff received from {source}. Enter your response: `，否则是 `Enter your response: `。回复类型也跟着变：交接场景回 `HandoffMessage(content=user_input, target=handoff.source)`（把控制权**退回**原发送者），普通场景回 `TextMessage`。这让「人」可以临时接管再交还，而不需要上层再判断。

**输入请求的广播与上下文。** 在真正阻塞等待输入之前，先产出一个 `UserInputRequestedEvent(request_id=..., source=self.name)`，`request_id` 是新建的 UUID；随后用 `InputRequestContext.populate_context(request_id)` 把该 id 绑到 `ContextVar` 上。UI 层可以在输入回调内部通过 `InputRequestContext.request_id()` 取到 id，从而把「用户提交的输入」与「之前的那个请求」对上号——这是跨异步边界传递关联信息的标准做法（`ContextVar` 而非参数传递）。取不到时抛 `RuntimeError`，明确限定只能在回调内调用。

**同步与异步输入的分派。** 构造期用 `iscoroutinefunction` 判定一次，之后 `_get_input` 按标志分两支：异步回调直接 `await`；同步回调走 `loop.run_in_executor(None, sync_func, prompt)`，即放到默认线程池里——因为同步的 `input()` 会阻塞事件循环。异常处理很克制：`CancelledError` 原样重抛（取消必须能被上游看见），其它异常统一包成 `RuntimeError("Failed to get user input: ...")`。

**过滤的两层结构。** `PerSourceFilter` 有三个字段：`source`（按来源筛选）、`position`（`"first"` 或 `"last"`，可为 `None`）、`count`（数量，可为 `None`）。`_apply_filter` 的算法极简：对每个 `PerSourceFilter`，先按 `source` 精确匹配取出该来源的全部消息；若 `position == "first"` 且有 `count` 就取前 N 条；`position == "last"` 且有 `count` 就取后 N 条；否则全取；把结果**追加**进结果列表。最后按配置顺序拼成一个新序列。

**过滤带来的两个副作用。** 其一是顺序：输出顺序由**配置顺序**决定，而不是原消息序列中的时间顺序，所以配置写反了会把「后半段对话」排在「前半段」前面。其二是丢弃：没有被任何 `PerSourceFilter` 覆盖的来源，其消息会被完全丢掉。

## 关键取舍

**源码事实：** `UserProxyAgent` 会**阻塞整个团队**直到用户回应。源码文档明确写出这个后果，并建议配合 `HandoffTermination` 或 `SourceMatchTermination` 先把团队停掉、把状态存下来、在用户回应后用新任务再跑一次。也就是说，「人在回路」在这个框架里有两种实现路径：阻塞式（本智能体）与中断式（终止条件 + 状态恢复）。

**源码事实：** `save_state` / `load_state` 未在 `UserProxyAgent` 中实现，它继承 `BaseChatAgent` 的默认实现，即返回空的 `BaseState`。`on_reset` 也是空操作，注释直说「重置智能体状态」但什么都不做——它确实没有可变状态。`_to_config` 里 `input_func=None`，注释留了 TODO：回调不可序列化，从配置恢复得到的是「用默认 `input()`」的代理。

**源码事实：** `MessageFilterAgent` 的描述是拼接出来的：`f"{wrapped_agent.description} (with message filtering)"`。这让上层选人时能看到包装关系的痕迹，但也意味着名字与描述都可由包装者自定义，与内层智能体的身份解耦。

**源码事实：** `MessageFilterAgent` 的 `on_messages_stream` 是纯转发生成器，不插入任何自己的事件。它不改写 `Response`，也不调整 `inner_messages`——过滤只作用于输入侧，输出原样透出。

**架构推论：** 这两个智能体把「非推理职责」从团队编排里剥离出去。人在回路与会话可见性都是编排问题，但若让团队管理器去做，就得给管理器加特例分支；做成 `ChatAgent` 之后，群聊管理器与容器完全不需要知道「这个参与者是人」或「这个参与者只看到部分历史」——这正是契约统一的收益。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_user_proxy_agent.py` | `UserProxyAgent` | 人类代理本体，产出 `TextMessage` 或 `HandoffMessage` |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_user_proxy_agent.py` | `on_messages_stream` | 交接检测 → 请求事件 → 阻塞取输入 → 回应 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_user_proxy_agent.py` | `_get_latest_handoff` | 只认最后一条消息，目标不符即报错 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_user_proxy_agent.py` | `cancellable_input` | 默认输入源：线程中跑 `input` 并挂取消令牌 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_user_proxy_agent.py` | `InputRequestContext` | 用 `ContextVar` 在回调内取当前请求 id |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_message_filter_agent.py` | `MessageFilterAgent` | 包装器：先过滤再委托 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_message_filter_agent.py` | `_apply_filter` | 按来源分组、按 first/last + count 截取的拼装算法 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_message_filter_agent.py` | `PerSourceFilter` | 过滤的最小配置单元：来源、位置、数量 |

## 参考

- [AutoGen 源码：_user_proxy_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/agents/_user_proxy_agent.py)
- [AutoGen 源码：_message_filter_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/agents/_message_filter_agent.py)
- [AutoGen 文档：人在回路](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/human-in-the-loop.html)

## 与主核 / AI 的关系

`MessageFilterAgent` 解决的问题与主核的「视图裁剪」同源：同一份底层数据，不同读取者只应看到各自的投影。这里投影的维度是「来源 + 位置 + 数量」，比主核的谓词过滤简陋得多，但足以解决上下文膨胀与信息污染两类问题。

对 AI 工程最实用的推论：**上下文不是越多越好，而是「该看的人看该看的部分」**。在有环的图式编排里（例如 A → B → A → B），如果每个节点看到全部历史，上下文会随环数线性增长且充满自我重复；用过滤器给每个节点固定「首条任务 + 上一轮对方消息 + 自己的最近若干条」，就能把上下文规模压成常数。代价是过滤器完全基于消息来源字符串，来源命名一旦不一致（例如人也叫 `user`、代理也叫 `user_proxy`），过滤就会静默失效——没有校验，只有结果为空或过满。

至于 `UserProxyAgent`，它暴露的是一个更普遍的工程事实：把「等待人类」放进同步的编排流里，会让整个团队的推进被一个不可控延迟卡住。可恢复的中断（终止条件 + 状态保存）通常比阻塞等待更适合真实部署。
