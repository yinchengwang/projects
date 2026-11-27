---
project: openhands
feature: conversation-store
title: 会话状态容器
page_type: feature
schema_version: 1
refs:
  - path: src/stores/conversation-store.ts
    symbols: [ConversationTab, ConversationState, ConversationActions]
  - path: src/stores/conversation-store.ts
    symbols: [getConversationIdFromLocation, getInitialConversationMode]
---

# 会话状态容器

## 一句话职责

一个 zustand store 装着**一个正在看的会话的界面状态**（右侧抽屉开没开、选了哪个标签、附件、待发消息、会话模式、规划助手的会话 id），并且用两条不同的持久化策略处理两类字段：**布局类留在内存、模式类落到 localStorage**。

## 机制是什么

文件 413 行，结构是经典的三段式：

| 类型 | 作用 |
|---|---|
| `ConversationTab` | 六个标签的字面量联合：`'files'`、`'commits'`、`'browser'`、`'terminal'`、`'planner'`、`'tasklist'` |
| `ConversationState` | 全部状态字段（面板、标签、`images`/`files` 附件、`messageToSend`、`conversationMode`、`subConversationTaskId`、`localPlanningConversationId`…） |
| `ConversationActions` | 全部 setter（含 `resetConversationState`） |
| `getConversationIdFromLocation` / `getInitialConversationMode` | 两个在**模块加载期**就执行的辅助函数 |

`ConversationStore = ConversationState & ConversationActions`，由 `create<ConversationStore>()(devtools((set) => ({...}))` 包一层 `devtools` 中间件，所以每个 `set` 都带第三个参数——**动作名**（`"setIsRightPanelShown"` 等），用于 Redux DevTools 里区分。

## 怎么实现的

**初始状态上方那 14 行注释是这一页最重要的东西：它解释了一条「持久化策略」而不是一个字段。** 原文：

> 右侧抽屉（`isRightPanelShown` / `hasRightPanelToggled`）**有意是仅会话内的状态**：app 加载时（或重启后打开一个新的/已存在的会话时）它总是从关闭开始，但它在应用内导航时存活，因为 Zustand store 跨 React Router 转换保活。**把开/关状态持久化进 localStorage 让面板以一种用户不预期的方式「粘住」了**——他们回到应用时想要一个干净、专注的聊天视图，只在本次会话里自己打开过时才希望面板回来。

这段评论解决的是一个具体的设计问题：**「跨路由保活」与「跨启动保活」是两件事，而只做前者才是对的行为**。用 zustand 天然得到第一个（store 是模块级单例，React Router 的转换不会销毁它），而第二个需要显式持久化——这里刻意**不**做。

值得记的是它给出的**用户理由**而不是技术理由：面板「粘住」违反了「回到应用时想要干净视图」的预期。这类注释的价值在于，后来的人若想「顺手加个持久化」，会先看到这段。

**初始化里唯一会碰持久化的字段只有 `conversationMode`。** `conversationMode: getInitialConversationMode()`。其余全是硬编码初值。

`getInitialConversationMode` 的逻辑是：无 `window`（SSR / 测试）→ `'code'`；从当前 URL 取不到会话 id → `'code'`；否则读 `getConversationState(conversationId)` 并取它的 `conversationMode`。

也就是说**这个字段是「按会话持久化」的，而不是「按应用」的**。这与面板字段正好相反：模式（`code` / 某个规划模式）是会话的属性，用户切换会话时期望看到各自的模式；而面板开合是当下的注意力状态。

`getConversationState` / `setConversationState` 来自 `utils/conversation-local-storage`——也就是说持久化被委托给一个独立的按会话 id 分键的工具，而 store 只负责在初始化时读一次。

**`getConversationIdFromLocation` 的解析方式是正则而不是路由参数。** `window.location.pathname.match(/\/conversations\/([^/]+)/)`，取第一个捕获组。

不用路由库的参数是因为**这个函数要在 store 创建时（模块加载期）执行**，而那时 React Router 还没挂载、路由参数不可用。这是一个「初始化顺序」逼出来的实现选择。而 `typeof window === 'undefined'` 的守卫说明这个模块会被 SSR 或测试环境导入。

**注意它匹配的是 `pathname` 而非整条 URL**——于是查询串（包括 `i02` 里那两个 `backend=` / `org=` 参数）不会干扰解析。这看起来是碰巧，但在有 URL 参数的系统里是必要的。

**`selectedTab` 的类型是 `ConversationTab | null`，但初值是 `'files'`。** 这个不一致是有信息的：`null` 表示「没有选中任何标签」（例如右侧面板整体关闭时），而默认给 `'files'` 是因为打开面板时总要有个落点。

**`setIsOverviewPanelShown` 同时改两个字段。** 实现是 `set({ isOverviewPanelShown, isOverviewPanelPeeked: false }, ...)`。也就是说**展开概览面板会清掉「peek」状态**——两个字段表达的是同一块屏幕空间的三种占用方式（隐藏 / 预览 / 展开），所以进入一种就要清掉另一种的残留。这是一个把「互斥状态」写成一次赋值的做法，比分别调用两个 setter 安全。

**每个 `set` 都传了名字，这让整个 store 在 DevTools 里是可读的。** 这不只是舒适度问题：一个有 30 多个 action 的 store，如果不命名，时间线上全是 `set`，无法判断是谁改的。

**`localPlanningConversationId` 与 `subConversationTaskId` 这两个 id 字段说明这个 store 也在承担「子会话」的书签职责。** 它们的语义在 `use-agent-state.ts` 里被揭示：规划助手是一个**独立的会话**，有自己的 run/idle 转换，所以主会话的状态不能用来判断它是否在跑。

**`resetConversationState` 是一个显式的整体重置动作。** 它在路由里被取出使用（`routes/conversation.tsx` 里 `const { resetConversationState } = useConversationStore()`），说明「离开一个会话」需要一个明确的重置点——否则上一个会话的附件、待发消息、面板状态会漏到下一个。

这里有一个值得注意的取舍：`resetConversationState` 与 zustand 的「初值」是两份需要同步的东西。而 store 里同时存在 `conversationMode: getInitialConversationMode()` 这种**依赖当前 URL 的初值**——所以「重置」不能简单地 `set(initialState)`，它必须重新计算模式。这正是为什么它是一个独立的 action 而不是复用 `agent-store.ts` 里那种 `reset: () => set(initialState)`。

（对照：`agent-store.ts` 只有 21 行，`initialState` 是常量 `{ curAgentState: AgentState.LOADING }`，所以它的 `reset: () => set(initialState)` 是安全的。这个对比很能说明「初值是否为常量」决定了重置能不能被简化。）

## 关键取舍

**源码事实：** 右侧面板的开合**有意不持久化**：它总是从关闭开始，但在应用内导航时存活（zustand store 跨路由保活）。理由写在注释里——持久化会让面板「粘住」，违反「回到应用想要干净视图」的预期。

**源码事实：** 唯一在初始化时读持久化的字段是 `conversationMode`，且它是**按会话 id 分键**的（`getConversationState(conversationId)`），与会话属性一致；面板字段则与应用级注意力一致。

**源码事实：** 会话 id 从 `window.location.pathname` 用正则取出，因为这段代码在模块加载期就要执行，路由参数尚不可用；只匹配 pathname 所以查询参数不干扰。

**源码事实：** `selectedTab` 类型允许 `null` 但初值是 `'files'`——`null` 表达「没有选中」（面板关闭），初值是「打开时的落点」。

**源码事实：** `setIsOverviewPanelShown` 一次赋值同时清掉 `isOverviewPanelPeeked`，因为两个字段是同一块屏幕空间的互斥占用方式。

**源码事实：** 每个 `set` 都带动作名（`"setIsRightPanelShown"` 等），使 30 多个 action 的 store 在 DevTools 时间线里可读。

**源码事实：** `getInitialConversationMode()` 依赖当前 URL，所以重置不能复用 `set(initialState)`，必须走独立的 `resetConversationState`——与 `agent-store.ts` 里那个常量初值的 `reset` 形成对照。

**架构推论：** 这个 store 的复杂度全在**「什么该记住多久」**这一个问题上，而不是在功能上。它有四种时间尺度：

| 尺度 | 字段 | 载体 |
|---|---|---|
| 一次会话内 | 面板开合、选中标签 | 模块级单例（跨路由存活） |
| 每会话 | `conversationMode` | 按会话 id 的 localStorage |
| 每次启动 | 硬编码初值 | 代码 |
| 子会话 | `localPlanningConversationId`、`subConversationTaskId` | 内存（随主会话） |

而区分这些尺度的依据**不是技术便利，是用户预期**。那段 14 行注释就是证据：技术上用 localStorage 只需两行，但它会让面板粘住。

这个判断在 agent 工程里尤其重要，因为 agent 界面常常同时有「长期偏好」（模型选择、主题）与「当下注意力」（面板、滚动位置）两类状态，而把它们都塞进持久层会让界面显得不受控。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/stores/conversation-store.ts` | `ConversationTab` | 六个面板标签的字面量联合 |
| `src/stores/conversation-store.ts` | `ConversationState` | 界面状态字段全集，含子会话 id |
| `src/stores/conversation-store.ts` | `ConversationActions` | setter 全集，含 `resetConversationState` |
| `src/stores/conversation-store.ts` | `getConversationIdFromLocation` | 模块加载期从 pathname 取会话 id |
| `src/stores/conversation-store.ts` | `getInitialConversationMode` | 按会话 id 读会话模式初值 |
| `src/stores/conversation-store.ts` | `useConversationStore` | 带 `devtools` 的 zustand store 与命名动作 |
| `src/stores/conversation-store.ts` | 初始状态上方的注释 | 「面板故意不持久化」的用户理由 |
| `src/stores/conversation-state-store.ts` | `executionStatusByConversation` | 按会话 id 分键的执行状态，供 `i03` 读取 |
| `src/stores/agent-store.ts` | `useAgentStore` | 常量初值的小 store，重置可直接复用初值 |

## 参考

- [OpenHands 源码：src/stores/conversation-store.ts](https://github.com/OpenHands/OpenHands/blob/main/src/stores/conversation-store.ts)
- [zustand 文档：devtools 中间件与动作名](https://zustand.docs.pmnd.rs/middlewares/devtools)
- [MDN：localStorage 与 sessionStorage](https://developer.mozilla.org/en-US/docs/Web/API/Web_Storage_API)
- [OpenHands 文档：界面与会话](https://docs.openhands.dev/openhands/usage/interface)

## 与主核 / AI 的关系

这个 store 与主核里「会话级 vs 系统级参数」的分野完全同构：数据库的 `search_path`、`statement_timeout` 是会话级的（断开即失）；`shared_buffers`、`max_connections` 是系统级的。而把某个参数放到哪一层，依据同样是**语义归属**，不是实现便利。

对 agent 工程的直接价值有三条。其一，**「跨路由保活」不等于「跨启动保活」**。zustand 单例天然给出前者，而后者必须显式持久化；把两者混同会让界面在重启后带上不该有的状态。其二，**持久化的键要按语义分组**。面板开合是「当下注意力」、会话模式是「会话属性」，用一个 localStorage 键装两者会让其中一个错。其三，**依赖当前 URL 的初值不能做成常量重置**。`agent-store.ts` 能 `set(initialState)` 是因为初值是常量；`conversation-store.ts` 不能，因为它要按当前会话重算——这类差异如果不写在代码里，重置会静默地给出错的模式。
