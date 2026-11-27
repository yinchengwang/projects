---
project: openhands
feature: agent-state
title: 智能体执行状态的派生
page_type: feature
schema_version: 1
refs:
  - path: src/hooks/use-agent-state.ts
    symbols: [mapExecutionStatusToAgentState, useAgentState]
  - path: src/stores/agent-store.ts
    symbols: [AgentStateData, useAgentStore]
---

# 智能体执行状态的派生

## 一句话职责

把服务端的**七值执行状态**（`ExecutionStatus`）映射成界面用的**十三值智能体状态**（`AgentState`），并且用两级数据源拼出一个**按会话 id 分键**的当前状态：先看 WebSocket 推来的实时值，没有就回退到 REST 拉到的那个。

## 机制是什么

三层：

| 层 | 内容 |
|---|---|
| 源枚举 | `ExecutionStatus`（`idle` / `running` / `paused` / `waiting_for_confirmation` / `finished` / `error` / `stuck`） |
| 目标枚举 | `AgentState`（十三个：`loading`、`init`、`running`、`awaiting_user_input`、`paused`、`stopped`、`finished`、`rejected`、`error`、`rate_limited`、`awaiting_user_confirmation`、`user_confirmed`、`user_rejected`） |
| 派生 hook | `mapExecutionStatusToAgentState`（纯函数）+ `useAgentState`（两级源 + 记忆化）|

`usePlanningAgentState` 是同一套机制的第二个消费者：规划助手是一个**独立的会话**，所以它借 `useAgentState(localPlanningConversationId)` 读自己的状态。

## 怎么实现的

**映射是不满射的，而且多出来的那些目标值各有来历。** 七个源值映射到六个目标值（`STUCK` 与 `ERROR` 都落到 `AgentState.ERROR`），而 `AgentState` 有十三个。差集说明了这个枚举的两层历史：

- `init` / `loading`：运行时的启动期。`RUNTIME_STARTING_STATES = [INIT, LOADING]` 这个常量就在 `agent-state.tsx` 里，供「运行时是否已起来」判断用。
- `stopped` / `rejected` / `rate_limited` / `user_confirmed` / `user_rejected`：更像是**本地动作的结果**或**更早期协议的取值**，不由执行状态映射产生。
- `awaiting_user_input` / `awaiting_user_confirmation`：这两个是映射产生的，区别在于「等你输入」与「等你批准某个动作」。

也就是说 `AgentState` 是一个**界面词汇表**（供多个来源写），而 `ExecutionStatus` 是一个**单一来源的权威枚举**。`mapExecutionStatusToAgentState` 只是向这个词汇表贡献一部分值。

**`STUCK → ERROR` 的注释只有四个字：`// Map STUCK to ERROR for now`。** 这句是全篇最诚实的一行。`STUCK` 在服务端是一个独立概念（agent 卡住了），而在界面里目前按错误处理。把它写成一个带 `for now` 的显式分支，比偷偷合进来要好——它标出了一个待定决策。

**`!status` 与 `default` 两个分支都返回 `LOADING`，但它们不是同一件事。** 前者是「还不知道」（`status` 为 `null`/`undefined`），后者是「知道但不认识」（协议加了新值）。两者都落到 `LOADING` 是一个**保守方向**：宁可显示「加载中」也不显示「空闲」或「失败」。

这与 `h01` 里 `getSessionPhase` 对未知状态的处理是同一条原则：**不认识的值要落到过渡态，而不是终态**。因为终态会改变界面可用性（例如禁用输入），而过渡态只是暂时显示。

**两级数据源的优先级写在 `??` 上：`liveExecutionStatus ?? fallbackExecutionStatus`。** 前者来自 `useConversationStateStore`（WebSocket 推来，按会话 id 分键），后者来自 `useActiveConversation().data?.execution_status`（REST 查询）。

而后者的注释解释了它的**作用域限制**：

> REST 回退**只描述路由自己的那个会话**，所以它只在没有显式请求别的会话时才适用。

代码里对应 `const fallbackExecutionStatus = isRouteConversation ? routeConversationExecutionStatus : null;`，而 `isRouteConversation` 的定义是 `!conversationId || conversationId === routeConversationId`。

**这三行是本页最精细的一处逻辑，值得完整推一遍。** 为什么必须限制？因为 `useActiveConversation()` 无论传什么参数都**只查当前路由的会话**（它是「active conversation」查询）。若调用方传了另一个会话 id（例如规划助手），却让 REST 回退生效，就会把主会话的状态当成规划助手的状态。

而 `liveExecutionStatus` 不受此限：`useConversationStateStore` 是按 id 分键的字典，`state.executionStatusByConversation[targetConversationId] ?? null` 能取到任意会话的实时值。

于是两种源的**作用域不同**：一个是 per-conversation 的，一个是 per-route 的。用 `isRouteConversation` 把它们对齐，是这个 hook 存在的核心理由。

**`conversation-state-store.ts` 的注释把「为什么按 id 分键」讲得更透。** 它说这些值由**主会话与规划会话两个 WebSocket 处理器**喂入，而按会话 id 分键使**规划助手会话自己的 run/idle 转换永远不能覆盖主会话的状态（或反之）**。

也就是说这是一个**曾经出过问题**的地方：两个会话各自有 WebSocket，如果不分键，后到的消息会覆盖另一个的状态。而分键是最直接的修法——它把「谁的状态」变成数据的键，而不是靠调用顺序约定。

**`useAgentState` 的 `useMemo` 依赖只有 `executionStatus`。** 于是状态映射只在源值变化时重算，而不是每次渲染。这点在 React 里是标准做法，但值得一提的是**依赖项选得对**：`curAgentState` 只由 `executionStatus` 决定，与 `conversationId` 无关。

**`useAgentState` 的返回值带 `executionStatus` 本身，不只是派生的 `curAgentState`。** 让调用方能拿到源值，是因为有些判断需要区分「`STUCK` 与 `ERROR`」（它们映射到同一个 `AgentState`）。这是一个正确的接口设计：**派生值加源值一起返回，避免调用方为了区分而再查一遍数据源**。

**`usePlanningAgentState` 的 `isPlanningAgentRunning` 把 `LOADING` 也算「正在跑」。** 定义是：

```
!!localPlanningConversationId && (curPlanningAgentState === RUNNING || curPlanningAgentState === LOADING)
```

字段的注释说明了这个取向：**Running or loading。在没有 planner 时是 `false`（而不是「未知」）**。

也就是说这个布尔刻意把过渡态归入「忙」：如果界面显示一个 spinner 依赖它，那么「还没起来」也该显示 spinner。而「没有规划助手」时给 `false` 而不是「未知」，是因为**没有就是没有**，不该让调用方额外处理三态。

**`usePlanningAgentState` 的组合方式说明了「一等会话」的概念。** 它从 `useConversationStore` 读 `localPlanningConversationId`，再把它（`?? undefined` 处理成可选参数）传给 `useAgentState`。于是规划助手与会话状态机制的关系是**「另一个会话」而不是「另一种状态」**——完全复用同一套两级源逻辑。

**`agent-store.ts` 是这组机制里的第三种东西，且它很小。** 21 行：一个 `AgentStateData`（只有 `curAgentState`）、一个 `AgentStore`（含 `setCurrentAgentState` 与 `reset`）、常量 `initialState = { curAgentState: AgentState.LOADING }`。

`AgentStateData` 这个类型名与 `AgentState` 枚举容易混淆，但它们的关系是「容器与值」：`data` 是装 `curAgentState` 的形状。

**这个 store 的用途在 `routes/conversation.tsx` 里才看得清：它是「界面当前该显示哪个状态」的落点。** 那处从 store 取出 `setCurrentAgentState`。也就是说存在一条**从派生值到可写状态**的流动：`useAgentState` 算出 `curAgentState`，然后被写进 `useAgentStore`，界面组件读 store。

为什么需要这一步？因为它提供了**覆盖点**：某些情形下状态不是根据执行状态派生的，而是由本地事件设定的（例如 `RATE_LIMITED`、`USER_CONFIRMED`）。而 store 的初值 `LOADING` 与映射的兜底一致，所以「还没算出来」与「写进去之前」显示同一个东西。

`reset: () => set(initialState)` 能安全地简写，正是因为 `initialState` 是**常量**——这与 `i01` 里 `resetConversationState` 必须重算初值（因为它依赖当前 URL）形成对照。

## 关键取舍

**源码事实：** 七值源映射到六值目标（`STUCK` 与 `ERROR` 合并），而目标枚举有十三值——`AgentState` 是**多来源的界面词汇表**，不是执行状态的镜像。

**源码事实：** `STUCK → ERROR` 带着 `// for now` 的注释，把待定决策标出来而不是藏起来。

**源码事实：** 「还不知道」与「不认识的值」都落到 `LOADING`，与 `h01` 对未知会话状态的处理同一方向：**未知落过渡态，不落终态**。

**源码事实：** 两级源的 `??` 优先级配合 `isRouteConversation` 的限制：WebSocket 值是 per-conversation，REST 值是 per-route，不对齐就会把主会话的状态当成别的会话的。

**源码事实：** `executionStatusByConversation` 按 id 分键，因为主会话与规划会话各有一个 WebSocket 处理器，不分键就会互相覆盖。

**源码事实：** hook 同时返回派生值与源值，因为有些判断（例如区分 `STUCK`）需要源值而派生值已丢失该信息。

**源码事实：** `isPlanningAgentRunning` 把 `LOADING` 也算忙，且无规划助手时给 `false` 而不是「未知」。

**源码事实：** 规划助手通过「另一个会话 id」复用同一套机制，不是另一条代码路径。

**架构推论：** 这一页真正在做的事是**把「状态的权威」与「状态的显示」分开**。权威只有一处（服务端 `ExecutionStatus`），而显示需要更多值（`AgentState` 十三个），其中一部分由派生得出（六个）、一部分由本地动作写入（其余七个）。而两级数据源、按 id 分键、per-route 回退这三条约束，都是为了让「哪个会话的哪个状态」这件事在任何时刻只有一种解释。

这类「枚举对不齐」的问题在 agent 界面里特别常见：服务端的心智模型是「这次运行在什么阶段」，而界面的心智模型是「用户此刻能做什么」。两个集合必然有交集也有差集，而正确的做法是**明确哪一侧是权威、差集里的值由谁产生**——而不是强行让两边一样长。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/hooks/use-agent-state.ts` | `mapExecutionStatusToAgentState` | 七值执行状态 → 界面状态的纯函数映射 |
| `src/hooks/use-agent-state.ts` | `useAgentState` | 两级源 + per-route 回退 + 记忆化 |
| `src/hooks/use-agent-state.ts` | `useAgentState` 里的 `isRouteConversation` | 限制 REST 回退的作用域 |
| `src/hooks/use-agent-state.ts` | `usePlanningAgentState` | 用「另一个会话 id」复用同一机制 |
| `src/hooks/use-agent-state.ts` | `isPlanningAgentRunning` | 把 `LOADING` 也算忙；无 planner 时给 `false` |
| `src/stores/agent-store.ts` | `AgentStateData` / `useAgentStore` | 可写的当前界面状态，提供本地覆盖点 |
| `src/stores/conversation-state-store.ts` | `executionStatusByConversation` | 按会话 id 分键的实时执行状态 |
| `src/types/agent-state.tsx` | `AgentState` / `RUNTIME_STARTING_STATES` | 十三值界面词汇表与「启动期」集合 |
| `src/types/agent-server/core/base/common.ts` | `ExecutionStatus` | 七值权威枚举 |

## 参考

- [OpenHands 源码：src/hooks/use-agent-state.ts](https://github.com/OpenHands/OpenHands/blob/main/src/hooks/use-agent-state.ts)
- [OpenHands 源码：src/stores/agent-store.ts](https://github.com/OpenHands/OpenHands/blob/main/src/stores/agent-store.ts)
- [OpenHands 文档：会话与运行时](https://docs.openhands.dev/openhands/usage/interface)
- [React 文档：useMemo 与依赖项](https://react.dev/reference/react/useMemo)

## 与主核 / AI 的关系

「权威枚举与显示词汇表对不齐」在数据库里同样普遍：事务状态有 `idle` / `active` / `idle in transaction` / `aborted`，而监控面板要显示「正常/可疑/等待输入/已回滚」。主核的做法通常是把服务端的原始状态原样暴露，让展示层自己映射；这里则把映射集中在一个纯函数里，并额外保留源值。

对 agent 工程的直接价值有三条。其一，**未知状态要落到过渡态**。协议会加值，而落到终态会误禁用界面；落到「加载中/启动中」只是短暂的保守。其二，**两级数据源的作用域必须对齐**。WebSocket 是 per-conversation、REST 查询是 per-route，混用会让 `A` 会话的界面显示 `B` 会话的状态——而这类错误看起来像「状态乱跳」，极难定位。其三，**派生值要连同源值一起返回**。一旦合并了两个源值（如 `STUCK` 与 `ERROR`），信息就永久丢失，调用方只能重新读数据源。
