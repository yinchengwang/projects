---
project: openhands
feature: agent-profiles
title: 智能体档案与当前档案
page_type: feature
schema_version: 1
refs:
  - path: src/hooks/use-active-agent-profile.ts
    symbols: [useActiveAgentProfile, useActiveAgentKind]
  - path: src/api/conversation-service/agent-server-conversation-service.api.ts
    symbols: [normalizeAcpModels, normalizeAcpEfforts]
---

# 智能体档案与当前档案

## 一句话职责

「现在跑的是哪种 agent」由**当前激活的 agent profile** 决定，而不是全局的 `settings.agent_settings`——因为激活只是一个指针、从不回写全局设置；`normalizeAcpModels` / `normalizeAcpEfforts` 则把会话返回的 ACP 模型与推理档位列表校验成前端可用的结构。

## 机制是什么

这一页把两件事放在一起，因为它们都围绕「当前 agent 是什么」的**读取侧**：

1. **谁是当前 agent**：`useActiveAgentProfile` 从「档案列表 + 激活指针」推导出一个 `AgentKind`（`openhands` / `acp`）。
2. **这个 agent 支持什么**：会话接口返回的 `available_models` / `available_efforts` 需要被归一化，才能驱动界面上的模型与推理档位选择。

第一件里有一条容易搞反的关系，源码 docstring 直接写了出来：

> 用户已激活的 agent profile——**在 Settings → Agent 就是档案库之后（#1571），它是权威的「当前 agent」**。`activate` 只是指针式的，从不写 `settings.agent_settings`，所以决定生效的是哪种 agent（OpenHands vs ACP）的是活动档案，而**不是**全局 agent 设置。

也就是说同一份「当前 agent」有两个可能来源，而设计特意让它们分工：

- **档案列表 + `active_agent_profile_id`** 是权威来源；
- **`settings.agent_settings.agent_kind`** 是**回退**来源，只在两种情况下用：列表还在加载，或者后端是 Cloud（可以不激活档案就直接启动）。

原文的措辞很精确：「没有活动档案时，`agentKind` 只在列表加载期间、以及 Cloud 上（那里可以不带档案启动）来自全局 agent 设置。**本地服务器总是保持有一个档案处于活动状态。**」

最后那句是一条重要的事实：本地 agent-server 会在第一次 GET 空的档案库时懒种一个默认档案（另一处注释说「于是对一个正在升级的用户，列表永远不会是空的」），所以本地场景下「没有活动档案」只是一个瞬态的加载状态，不是稳定状态。Cloud 则不同，它可以不激活就启动，所以那里的回退是长期的。

## 怎么实现的

**`useActiveAgentProfile` 的主题结构只有四步。**

```
const { data, isLoading, isError: queryError } = useAgentProfiles();
const { data: settings } = useSettings();
const { backend } = useActiveBackend();
const activeId = data?.active_agent_profile_id ?? null;
const activeProfile = data?.profiles?.find((p) => p.id != null && p.id === activeId) ?? null;
const isError = queryError && !data;
const settingsKind = settings?.agent_settings?.agent_kind;
let agentKind: AgentKind | undefined;
if (activeProfile) agentKind = activeProfile.agent_kind;
else if (!isError && (isLoading || backend.kind === "cloud") && (settingsKind === "acp" || settingsKind === "openhands"))
  agentKind = settingsKind;
return { activeProfile, agentKind, isLoading, isError };
```

三处判断都值得逐条读。

**`p.id != null && p.id === activeId`** —— 为什么先判 `p.id != null`？因为 `AgentProfileSummary.id` 是可选（`string | null`）。若不判，`activeId` 为 `null` 时，第一个 `id` 也是 `null` 的档案就会被误判成活动档案。这是个典型的「两个 null 相等」陷阱，被显式挡掉了。

**`isError = queryError && !data`** —— docstring 说明了这个组合的含义：「缓存里的档案数据能在一次失败的重取后存活；只有一次失败的取数**什么都不剩**时 `isError` 才置位，那时 `agentKind` 是 `undefined`」。也就是说「曾经取到过、这次刷新失败」不算错误——旧数据仍然可用。这个判断是 React Query 的 `isError` 与 `data` 的联合，用来区分「从未成功」与「刷新失败」。

**回退分支的四个合取项** —— `!isError && (isLoading || cloud) && (settingsKind === "acp" || settingsKind === "openhands")`：

- `!isError`：取数彻底失败时不猜；
- `isLoading || cloud`：只在「列表还没回来」或「云后端可能没有活动档案」时才用全局设置；
- 最后一项是对 `settingsKind` 的**窄化**：它把 `string | undefined` 收成 `AgentKind | undefined`。这一步是类型需要，也是语义需要——一个未知的 `agent_kind` 字符串不该被当成有效种类。

于是 `agentKind` 有三种结果：来自活动档案（权威）、来自全局设置的窄化值（回退）、`undefined`（不知道）。**`undefined` 是刻意保留的输出**，不是遗漏——调用方需要能表达「此刻不知道」。

**`useActiveAgentKind` 是同一逻辑的窄化入口。**

```
export function useActiveAgentKind(): AgentKind | undefined {
  return useActiveAgentProfile().agentKind;
}
```

它不返回 `profile`，只返回种类。这让「只关心是不是 ACP」的调用方（例如 `use-acp-model-context`）不必拿到整个档案对象，也就不必为它建立对档案列表的依赖语义。代价是**它仍然会触发 `useAgentProfiles` 与 `useSettings` 两个查询**——Hook 的组合是静态的，窄化只发生在返回值上。

**数据来源本身是「按后端隔离」的。**

`useAgentProfiles` 的 queryKey 是 `[...AGENT_PROFILES_QUERY_KEYS.all, backend.id, orgId]`，注释写着：「后端身份隔离跨后端/组织切换的缓存」。所以 `useActiveAgentProfile` 间接拿到了「这个后端上的活动档案」——切后端时档案列表与活动指针都跟着换。

而 `AgentProfilesService` 的 `activateProfile` 注释重申了那个要点：「按档案的稳定 UUID `id` 激活（**指针式；从不写 `agent_settings`**）」。这解释了为什么必须读活动档案而不能读全局设置：全局设置在这种情况下根本不会被更新，读它就会读到旧值。

**`normalizeAcpModels` 与 `normalizeAcpEfforts` 是两个「逐条丢弃」的归一化函数。**

```
function normalizeAcpModels(value: unknown): ACPModelInfo[] {
  if (!Array.isArray(value)) return [];
  return value.flatMap((entry) => {
    if (!isRecord(entry) || typeof entry.model_id !== "string") return [];
    return [{ model_id: entry.model_id, name: stringOrNull(entry.name), description: stringOrNull(entry.description) }];
  });
}

function normalizeAcpEfforts(value: unknown): ACPEffortOption[] {
  if (!Array.isArray(value)) return [];
  return value.flatMap((entry) =>
    isRecord(entry) && typeof entry.value === "string"
      ? [{ value: entry.value, name: stringOrNull(entry.name) }]
      : [],
  );
}
```

两者的形状完全对称，各有两条规则：

- **不是数组 ⇒ 空数组**（不抛错）。上游可能给 `null`、`undefined`、一个对象，都退化成「没有模型」。
- **每条必须有一个关键字段**（`model_id` / `value`）且它是字符串；不满足就**丢弃这一条**，不是整份作废。其余字段走 `stringOrNull`（非字符串一律归 `null`）。

用 `flatMap` 而不是 `filter` + `map` 是个小技巧：返回 `[]` 表示丢弃、返回 `[obj]` 表示保留，一趟走完。这比「先 filter 再 map」少一次遍历，也让「丢弃」与「构造」写在同一处。

为什么对关键字段这么严而对次要字段这么松？因为 `model_id` 是标识符——没有它就无法发起请求；而 `name` / `description` 只是展示文案，缺失时退回 `null`（UI 会用 `model_id` 兜底）。**标识缺失即该条无用，展示缺失只影响美观。**

**它们的消费点在一段会话归一化里。**

```
available_models: normalizeAcpModels(item.available_models),
available_efforts: normalizeAcpEfforts(item.available_efforts),
```

旁边的注释交代了这两个字段的来源与兼容姿态：

> SDK 运行时的 ACP 模型字段（当 agent-server 支持 `ConversationInfo.current_model_*` 时填充）。由会话适配器消费，驱动每张卡片的 chip 的模型文案。**较旧的 agent-server 会省略这些——适配器优雅地处理 `undefined` / `null`。**

注意这里的一致性：归一化函数对「不是数组」返回空数组，而调用方永远得到一个数组——于是「旧服务端不返回这个字段」与「返回了但内容畸形」在类型上不可区分，都被处理成「没有模型可选」。这对 UI 是正确行为（没有可选项就不显示选择器），但也意味着**无法从数据层区分「这个 agent 不支持模型选择」与「服务端太旧」**。这个区分只能靠别的手段（例如看其他字段是否存在）。

## 关键取舍

**源码事实：** 活动档案是权威的「当前 agent」来源；`activate` 是指针式的，**从不写 `agent_settings`**，所以读全局设置会得到陈旧值。

**源码事实：** `agentKind` 的回退分支要求四个条件：非彻底失败、且（列表加载中或 cloud 后端）、且全局设置里的 `agent_kind` 是已知的两个值之一。本地服务器总是保持有一个档案处于活动状态。

**源码事实：** `isError` 只在「查询失败且没有任何数据」时为真，所以刷新失败时旧档案数据仍然可用。

**源码事实：** 活动档案的匹配先判 `p.id != null`，避免「两个 `null` 相等」把无 id 的档案误判成活动档案。

**源码事实：** 两个归一化函数对「不是数组」返回空数组，对每条「关键字段不是字符串」丢弃该条，其余字段走 `stringOrNull`。

**架构推论：** 「指针」与「配置副本」的分离带来一个必然的读取复杂度：既然 `activate` 不回写全局设置，那任何需要知道「当前 agent」的地方都必须读「指针 + 档案列表」而不是读配置。这条规则被写进 docstring 而不是靠调用方自觉——因为违反它的代码不会报错，只会返回一个看起来合理但过期的值。

**架构推论：** 保留 `undefined` 作为 `agentKind` 的第三种结果，是把「不知道」与「不是 ACP」分开。若把 `undefined` 折成 `openhands`，那么列表加载期间会短暂地把 ACP 会话当成 OpenHands，触发与它无关的凭据检查（`use-llm-configured` 里就靠 `isAcpAgent` 决定要不要查 API key）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/hooks/use-active-agent-profile.ts` | `useActiveAgentProfile` | 从活动指针 + 档案列表推导当前档案与种类，带回退 |
| `src/hooks/use-active-agent-profile.ts` | `useActiveAgentKind` | 只取种类的窄化入口 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeAcpModels` | 校验并归一 ACP 模型列表，逐条丢弃 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeAcpEfforts` | 校验并归一推理档位列表，逐条丢弃 |
| `src/hooks/query/use-active-acp-profile-detail.ts` | `useActiveAcpProfileDetail` | 首页模型选择器读活动 ACP 档案自身的字段 |
| `src/hooks/query/use-agent-profiles.ts` | `useAgentProfiles` | 按后端/组织隔离的档案列表查询 |

## 参考

- [OpenHands 源码：use-active-agent-profile.ts](https://github.com/OpenHands/OpenHands/blob/main/src/hooks/use-active-agent-profile.ts)
- [OpenHands 源码：agent-server-conversation-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/conversation-service/agent-server-conversation-service.api.ts)
- [Agent Client Protocol 规范](https://agentclientprotocol.com/)
- [TanStack Query：缓存与后端隔离](https://tanstack.com/query/latest/docs/framework/react/guides/query-keys)

## 与主核 / AI 的关系

「权威事实是一个指针，而不是一份配置副本」在主核里对应的是「系统目录是唯一真相，缓存只是加速」。主核里如果某处代码改成读一个缓存的表定义副本，那么 `ALTER TABLE` 之后那段代码就会用旧 schema 解析——错误静默、且只在特定时序下出现。这里的同构是：`activate` 只动指针，所以任何读 `agent_settings` 的地方都会拿到旧值。

对 AI 工程的实际意义有三条。第一，**「不知道」要能表达**。`agentKind` 的三态（两个值 + `undefined`）让调用方能区分「不是 ACP」与「还没查清」。把 `undefined` 折成默认值是一类常见但危险的优化，因为它把「不确定」变成了一个确定的错误答案。

第二，**回退必须带条件**。这里读全局设置需要同时满足「列表还在加载」或「是 cloud 后端」，而不是无条件回退。无条件回退会在正常路径下引入一条永远存在的错误来源——「档案列表已加载但没有活动档案」时，读全局设置就会给出与实际情况不同的答案。

第三，**归一化对关键字段严、对展示字段松**。`model_id` / `value` 缺失就丢那一条，`name` / `description` 缺失归 `null`。这个分野的判据是「这个字段缺失后该条目还有用吗」——有则降级，无则丢弃。逐条丢弃（而不是整份作废）也是同一个判据在集合层面的应用。
