---
project: openhands
feature: conversation-types
title: 会话类型与启动任务
page_type: feature
schema_version: 1
refs:
  - path: src/api/conversation-service/agent-server-conversation-service.types.ts
    symbols: [SandboxStatus, AppConversationStartTaskStatus, AppConversationStartRequest]
  - path: src/api/conversation-service/agent-server-conversation-service.types.ts
    symbols: [MessageContent, TokenUsage, MetricsSnapshot]
---

# 会话类型与启动任务

## 一句话职责

这份类型文件是「前端能说、能听」的全部词汇：一个会话长什么样（`AppConversation`）、怎么请求创建它（`AppConversationStartRequest`）、创建过程中它处于哪个阶段（`AppConversationStartTaskStatus`）、以及沙箱与用量的状态模型。

## 机制是什么

这些类型不是照着后端 schema 抄一遍，而是在几个互相牵制的约束下**协商出来的产物**。把文件里那些长注释串起来读，能看出四组反复出现的张力：

| 张力 | 表现 | 处理方式 |
|---|---|---|
| 两个后端（本地 / 云端） | 有些字段只有云端有（沙箱、git provider id），有些只有本地有 | 字段声明为可选 + 注释标「Cloud only」 |
| 版本差异 | 旧服务端缺字段（`tags`、`sub_conversation_ids`、`current_model_*`） | 全部可选，读取方自行回退 |
| 前后端语义不同 | 同一个 `tags` 在两边的 PATCH 语义不同（替换 vs 合并） | 把差异写进类型注释，作为契约 |
| 客户端专有状态 | `selected_workspace`、`active_profile` 只在前端存在 | 显式注明「never round-tripped」 |

所以这份文件同时承载了三件事：类型定义、**契约文档**、以及**跨版本兼容的承诺**。这里的注释不是解释代码，而是在陈述「这个字段在什么情况下会缺席」。

## 怎么实现的

**两个状态类型，两套取值风格。** 这是文件里最直观的一处设计差异：

```ts
export type SandboxStatus = "PAUSED" | "RUNNING" | "STARTING" | "MISSING" | "ERROR";

export type AppConversationStartTaskStatus =
  | "WORKING" | "WAITING_FOR_SANDBOX" | "PREPARING_REPOSITORY"
  | "RUNNING_SETUP_SCRIPT" | "SETTING_UP_GIT_HOOKS" | "SETTING_UP_SKILLS"
  | "STARTING_CONVERSATION" | "READY" | "ERROR";
```

`SandboxStatus` 是**大写枚举**，注释写「Mirrors OpenHands' V1SandboxStatus」——即它的取值必须与后端常量逐字一致，因为它是后端的状态机，前端只读。

`AppConversationStartTaskStatus` 则是**启动任务的九个阶段**，前七个是「正在做」的不同步骤，`READY` 是终态成功，`ERROR` 是终态失败。这个序列本身就在描述一次启动会经过哪些环节：等沙箱 → 准备仓库 → 跑安装脚本 → 装 git hooks → 装 skills → 启动会话。

值得注意它**没有** `CANCELLED`：因为这个对象描述的是后端的任务记录，用户取消只是前端不再轮询，任务记录本身不会因此变形。缺少这个状态反而让前端更简单——只需判「有没有进 READY 或 ERROR」。

**`AppConversationStartTask` 把「请求」原样留着。** 它含 `request: AppConversationStartRequest`：

```ts
export interface AppConversationStartTask {
  title_generation?: ConversationTitleGeneration | null;
  id: string;
  created_by_user_id: string | null;
  status: AppConversationStartTaskStatus;
  detail: string | null;
  app_conversation_id: string | null;
  agent_server_url: string | null;
  request: AppConversationStartRequest;
  created_at: string;
  updated_at: string;
}
```

保留完整请求有两个用途：前端在任务还处于等待阶段时可以据此渲染「将要创建什么」（不必另存一份本地意图）；以及 `app_conversation_id` 为 `null` 时仍能通过 `request.conversation_id` 判断这是「新建」还是「唤醒已有会话」。

`detail: string | null` 是给人看的阶段说明，与枚举的机器可读状态配对——注释没明说，但从 `status` 与 `detail` 并存可读出「状态驱动逻辑、detail 驱动展示」的分工。

**`AppConversationStartRequest` 是这份文件信息密度最高的部分。** 约二十个字段，几乎每个都带一段注释，其中四处揭示了真实的历史包袱：

1. **`conversation_id` 的双重用途**：注释说它表示「Re-provision an EXISTING conversation（waking a recycled sandbox）instead of minting a new one」，且后端会用这个 id 重建，ACP 场景下还会「resumes it from the durable event store with a bootstrap prompt」。也就是说同一个字段既是「新建」的否定，也是「恢复」的凭据——前端只需传 id，后端负责判断走哪条路。

2. **`agent_profile_id` 的两条路**：注释明确写「Accepted by the cloud app-server（OpenHands #15060）on `POST /api/v1/app-conversations`; the local path uses the encrypted `agent_settings` builder instead, which threads its own `agentProfileId`」。即同一个概念在两条后端路径上有两种传法——这是「两个后端」张力最直接的一处证据。

3. **`agent_launch_additions` 的嵌套单字段**：它的唯一内容是 `{system_message_suffix_append?: string | null}`，注释说是用来「route the first message through the Model Router when the user enables "Run at conversation start"」。用嵌套对象而不是平铺一个字段，看不出必要性——更像是复用 SDK 的 `AgentLaunchAdditions` 结构以保持线上形状一致。

4. **`processors?: unknown[]`**：这是唯一一个**主动放弃类型**的字段，注释只写「EventCallbackProcessor - keeping as unknown for now」。这是一个诚实的标记：后端有定义、前端暂时不用，所以不假装有类型。

还有一处**显式的默认语义声明**：`plugins`、`client_tools`、`tags` 都写成 `| null` 而不是仅可选。`tags` 的注释说「Conversation tags to persist at start」，而 `AppConversation.tags` 的注释则给出了一条重要约束——

> PATCH semantics differ by backend: the agent-server replaces the **complete** `tags` map on every PATCH — it does not merge individual keys, so a writer adding one tag must read the current map, merge, and send the full desired result, or it will silently drop every other tag（including `acpserver`）. The Cloud app-server instead applies a merge patch（a `null` value deletes a key）and rejects server-managed keys.

这段话是整份文件最重要的工程信息。它说明**同一个字段在不同后端上的更新语义不兼容**：本地是全量替换、云端是合并。任何写 `tags` 的代码都必须知道自己在哪个后端上，否则一次「只加一个标签」的操作会把包括 `acpserver` 在内的所有标签抹掉——而 `acpserver` 一旦丢掉，会话的 ACP 供应商身份就从卡片上消失了。

**`agent_kind` 的存在解释了为什么需要两个「模型」字段。** 注释：

> `"openhands"` for an LLM-driven Agent, `"acp"` for an ACPAgent that delegates to an external ACP CLI subprocess. Consumers can use this to gate UI affordances that only make sense for one kind（e.g. the LLM-profile switcher in the chat header is a no-op for ACP conversations even though `llm_model` may carry the ACP subprocess model for display）.

即 `llm_model` 这个字段在 ACP 会话里装的是「ACP 子进程的模型名，仅供展示」。如果只看 `llm_model`，前端会以为切换到另一个 LLM profile 有意义，而实际对 ACP 会话无效。所以需要 `agent_kind` 这个判别位来门控功能。这是「一个字段承担两种语义」的典型后遗症，解法是**另加一个判别位**而不是拆字段——因为拆字段会破坏对旧数据的兼容。

`acp_server` 的注释同理，而且给出了更细的背景：它从 `info.tags.acpserver` 来，但因为 `acpserver` 是「snake_case-incompatible」的形式（含连字符/大写不匹配），所以不能直接用 snake_case 别名映射，得靠具名常量 `ACP_SERVER_TAG_KEY` 显式读取。

**`selected_workspace` 与 `workspace.working_dir` 的区别被写得很清楚**，这是另一处容易混淆的语义：

> The local workspace the user explicitly attached when creating this conversation. Client-side only — never round-tripped to the agent-server or cloud. Null/undefined for conversations created via "No workspace". Distinct from `workspace.working_dir`, which is the runtime path and may either match this folder directly or point at a per-conversation worktree.

即「用户选的那个文件夹」与「运行时实际用的路径」是两件事——后者可能是每个会话单独开的工作树。若把它们合成一个字段，前端就无法正确地在设置里重新展示「用户当初选的目录」。

`active_profile` 的注释给出同类理由：它来自客户端元数据，优先于「拿 `llm_model` 去匹配 profile 列表」，因为「several profiles share a model」时匹配是歧义的。这也是一次「不要用推断，用记录」的选择。

**消息内容是判别联合而不是通用结构。**

```ts
export interface MessageTextContent { type: "text"; text: string; }
export interface MessageImageContent { type: "image"; image_urls: string[]; }
export type MessageContent = MessageTextContent | MessageImageContent;
```

注释「These types match the SDK's TextContent and ImageContent formats」——即形状不能自己发挥，必须与 SDK 一致。`type` 作为判别位，让 `switch` 能收窄。注意图片是 **`image_urls: string[]`**（复数），所以一条消息可以带多张图，这与「一条消息一个图」的常见简化不同。

`MessageRole` 是 `"user" | "system" | "assistant" | "tool"` 四值，且**不导出**（`type MessageRole` 无 `export`）——只作为 `SendMessageRequest` 的内部构成。

**用量模型分成「摘要」与「明细」两层。**

```ts
export interface TokenUsage { prompt_tokens; completion_tokens; cache_read_tokens; cache_write_tokens; context_window; per_turn_token; }
export interface MetricsSnapshot { accumulated_cost: number | null; max_budget_per_task: number | null; accumulated_token_usage: TokenUsage | null; usage_breakdown?: UsageBreakdownEntry[] | null; }
```

`MetricsSnapshot` 的三个主字段全是可空：成本可未知、预算可未设、token 用量可无。而 `TokenUsage` 的六个字段全是必填数字——这与 d01 里 `normalizeTokenUsage` 用 `numberOrZero` 的做法严格对应：**外层可空、内层不可空**。

`usage_breakdown` 是唯一可选又带 `| null` 的字段，它按模型拆分成本（`UsageBreakdownEntry` 的注释「Cost and tokens of one model, summed over the usage ids that used it」）。它缺席时前端退回「不展示拆分」，而不是假装只有一个模型。

**运行期明细 `RuntimeMetrics` 是另一套结构**，比摘要厚得多：`model_name`、`accumulated_cost`、`costs`（逐次成本）、`response_latencies`（逐次延迟）、`token_usages`（逐次用量）。它被包在 `RuntimeConversationStats` 的 `usage_to_metrics: Record<string, RuntimeMetrics>` 里——即按 `usage id` 分组。这个结构的存在解释了 d01 里 `normalizeStats` 只做浅检查的那句注释：这些逐次条目由 `combineUsageMetrics` 消费，那里已经容忍缺失与畸形，所以不必在解析处再校验一遍。

`RuntimeConversationInfo` 是「运行期视角的会话」：比 `AppConversation` 少了仓库、标签、URL、密钥这些管理信息，多了 `status: ExecutionStatus`（非空）与 `stats`。即它假定「这个会话正在运行」，所以状态必有。

**只有一处 `public?: boolean`** 没有任何注释。它表示会话是否公开（可分享），而它的缺席是整份文件里唯一的沉默——其余每个非显然字段都有解释。

## 关键取舍

**源码事实：** `SandboxStatus` 用大写枚举并与后端 `V1SandboxStatus` 逐字一致，因为是后端状态机；`AppConversationStartTaskStatus` 用九个大写阶段值，没有「已取消」，因为取消是前端行为而非后端记录。

**源码事实：** `tags` 的 PATCH 语义在两个后端上不兼容：本地全量替换、云端合并（`null` 删键并拒绝服务端管理键）。类型注释明确要求写入方先读后合并。

**源码事实：** `agent_kind` 存在的理由是 `llm_model` 在 ACP 会话里只是展示值，切换 LLM profile 对 ACP 无效，所以需要独立判别位门控 UI。

**源码事实：** `selected_workspace`（用户选的目录）与 `workspace.working_dir`（运行时路径，可能是 per-conversation worktree）是两个字段；前者只在前端存在，不回传后端。

**源码事实：** `active_profile` 来自客户端元数据，优先于「用 `llm_model` 匹配 profile 列表」，因为多个 profile 可能共用同一模型导致歧义。

**源码事实：** `AppConversationStartRequest.conversation_id` 兼作「唤醒已有会话」的凭据，ACP 场景下后端会从持久事件库恢复并注入 bootstrap prompt。

**源码事实：** `MessageContent` 是 `"text" | "image"` 判别联合，图片字段是复数 `image_urls`，形状必须与 SDK 的 `TextContent` / `ImageContent` 一致。

**源码事实：** `MetricsSnapshot` 三个主字段全可空，`TokenUsage` 六个字段全必填，与归一化时「外层可空、内层补零」的策略严格对应。

**架构推论：** 这份文件的注释承担了**契约文档**的职责，而不是类型注释的常态（解释一个字段叫什么）。原因在于它描述的信息类型系统表达不了：版本兼容边界（「1.37.1 之前没有」）、后端间语义差异（PATCH 替换 vs 合并）、以及「这个字段缺席时消费方该怎么做」。TypeScript 只能表达「可选」，不能表达「可选是因为旧服务端没有，且你应该回退到 X」。

**架构推论：** 「另加判别位而不是拆字段」是这里反复出现的手法（`agent_kind` 之于 `llm_model`，`acp_server` 之于 `tags`，`selected_workspace` 之于 `working_dir`）。它的好处是旧数据仍然可读；代价是每个新增的判别位都意味着某处存在过语义重载，而未来的读者必须同时理解两套。

**架构推论：** `processors?: unknown[]` 是这份文件里最诚实的一处。给一个不用的后端字段编一个前端类型，会让后来的人误以为前端真的理解它；写 `unknown` 则明确「我们不知道也不假装知道」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `SandboxStatus` | 云端沙箱五态，与后端常量逐字一致 |
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `AppConversationStartTaskStatus` | 启动任务的九个阶段（七步进行 + 成功 + 失败） |
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `AppConversationStartRequest` | 创建/唤醒请求，含云端与本地两条 profile 传法 |
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `AppConversation` | 一个会话的完整前端形状，含两个后端的差异字段 |
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `MessageContent` | `text` / `image` 判别联合，图片为复数 URL |
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `TokenUsage` | 六个必填计数，内层不可空 |
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `MetricsSnapshot` | 外层三个可空字段 + 可选按模型拆分 |
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `RuntimeConversationStats` / `RuntimeMetrics` | 按 usage id 分组的运行期明细 |
| `src/api/conversation-service/agent-server-conversation-service.types.ts` | `AppConversationPage` | `items` + `next_page_id` 的分页形状 |

## 参考

- [OpenHands 源码：agent-server-conversation-service.types.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/conversation-service/agent-server-conversation-service.types.ts)
- [OpenHands 仓库：frontend-api-contracts 技能](https://github.com/OpenHands/OpenHands/blob/main/.agents/skills/frontend-api-contracts/SKILL.md)
- [software-agent-sdk 仓库：Agent Server 契约](https://github.com/OpenHands/software-agent-sdk)
- [TypeScript 文档：判别联合](https://www.typescriptlang.org/docs/handbook/2/narrowing.html#discriminated-unions)

## 与主核 / AI 的关系

「多个来源的同一概念要靠判别位分开」这件事，主核里对应的是**协议版本与类型标签**：同一个页结构在不同表空间可能来自不同的访问方法，为了区分，元组上必须带一层类型标识。主核不能「换个字段名」来解决——那会破坏所有读取方；只能像这里一样加一个判别位。

`tags` 的 PATCH 语义差异是这份文件里最有普适价值的一条教训。主核的类比是 `COMMENT` 与 `reloptions`：有的后端接口是「整体替换」语义，有的是「按键合并」语义。当同一份配置可以从两条路写入且语义不同时，调用方稍不留神就会静默丢数据——丢掉的还恰好是身份字段（`acpserver`）。这条与 e02 的「稀疏补丁」是同一族问题：**更新的语义粒度必须与字段的独立性匹配**。稀疏补丁之所以安全，是因为它约定「没出现在补丁里的键一律不动」；而全量替换的接口则要求写入方先读。

`selected_workspace` 与 `working_dir` 并存，也对应主核里一个熟知的区分：**用户声明的意图**与**系统解析出的结果**。前者是「我选了这条路径」，后者是「实际用到的路径」。把两者合成一个字段，就再也无法区隔「用户本来就想要这个」与「系统碰巧解析到了这个」——这在审计与重试时都是致命的。

对 AI 工程的实际意义在于：agent 前端的类型层比一般应用更早暴露「协议漂移」。后端会不断加字段（ACP 支持、plugins、client tools、agent profile），而前端要能同时兼容几个版本。这份实现给出的做法值得抄：**所有跨版本字段一律可选并由消费方回退**、**用判别位而非拆字段处理语义重载**、**把后端间差异写进类型注释而不是散落在各个调用点**。最后一条尤其重要——差异一旦散落，就没人知道全局有多少处需要跟着改。
