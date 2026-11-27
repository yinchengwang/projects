---
project: openhands
feature: conversation-service
title: 会话服务与响应归一化
page_type: feature
schema_version: 1
refs:
  - path: src/api/conversation-service/agent-server-conversation-service.api.ts
    symbols: [normalizeWorkspace, normalizeRuntimeInfo, normalizeAgent]
  - path: src/api/conversation-service/agent-server-conversation-service.api.ts
    symbols: [normalizeTokenUsage, requirePathInsideDirectory]
---

# 会话服务与响应归一化

## 一句话职责

`AgentServerConversationService` 是所有会话相关 HTTP 调用的唯一入口，而它真正的份量在一个不起眼的地方：**后端返回的 JSON 一律当 `unknown` 处理**，由一批 `normalize*` 函数逐字段归一化后才进前端类型系统——前端在这里把自己当成信任边界，而不是 `as Conversation` 一铸了之。

## 机制是什么

这是一个浏览器前端在跟两种后端（本地 agent-server 与云端 app-server）对话，而两种后端的字段形态并不一致：

| 差异 | 表现 |
|---|---|
| 命名风格 | 同一份数据可能回 `created_at` 也可能回 `createdAt` |
| 字段有无 | 旧版本服务端缺字段：`tags`、`sub_conversation_ids`、`current_model_id`、`runtime_info` |
| 字段演进 | 同一字段在不同版本里类型不同（`stats` 只做浅检查、`tags` 的值可能不是字符串） |
| 值的合法性 | 枚举值可能是前端没见过的（例如新的 `runtime_status`） |

如果按常规做法写 `const data = (await resp.json()) as AppConversation`，上面每一条都会变成运行期崩溃或渲染出错（例如对一个 `null` 调 `.map`）。所以这里的策略是分层的：

```
HTTP 响应 (unknown)
  └─ require* / read*       ← 结构不合格就整体抛错（"data this UI does not understand"）
       └─ normalize*        ← 单字段不合格就降级（给 null、给 0、给空数组、给默认时间）
            └─ AppConversation / DirectConversationInfo  ← 前端类型系统接受的形状
```

两层职责的区别是关键：**结构性问题抛错，字段性问题降级**。

- 顶层缺 `id` 或响应根本不是对象/数组 → 抛 `invalidConversationResponse()`。因为此时这份数据无法对应到任何会话，继续渲染只会显示一片空白加无解的错误。
- 单个字段类型不对 → 降级为 `null` / `0` / `[]` / 默认时间。因为会话本身是有效的，只是某个属性缺失。

这个分界在 `readConversationSearchPage` 里最清楚：`items` 不是数组就抛；`items` 里缺少 `id` 的条目用 `.filter(hasConversationId)` **静默丢掉**，其余照常归一化。

## 怎么实现的

**一组三行的原语。** 整个归一化体系建在四个微函数上：

```ts
function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
function numberOrNull(value: unknown): number | null { ... }
function numberOrZero(value: unknown): number { ... }
function stringOrNull(value: unknown): string | null { ... }
```

`isRecord` 显式排除数组，这一条很关键——JS 里 `typeof [] === "object"`，不排除的话数组会被当成对象走进字段读取，然后所有字段都是 `undefined`，最终产出一个「字段全空但结构合法」的对象，比直接降级成 `null` 更难诊断。

**`normalize*` 家族的逐字段策略。** 每个函数做同样的三件事：不是对象就返回 `null`、逐字段用 `*OrNull` / `*OrZero` 收口、嵌套结构递归。

一个典型样本是 `normalizeRuntimeInfo`，它比别处多一步**枚举校验**：

```ts
if (runtimeStatus !== "available" && runtimeStatus !== "starting" &&
    runtimeStatus !== "missing" && runtimeStatus !== "ownership_lost" && runtimeStatus !== "error") {
  return null;
}
return { runtime_status: runtimeStatus, can_resume: value.can_resume === true };
```

即 `runtime_status` 不是五个已知值之一就整个丢弃。理由很直接：这个字段驱动「沙箱可用性」的判断，前端若把它当字符串透传，遇到 `"degraded"` 这种新值时所有 `switch` 都会落到 `default`，而 `default` 分支往往是最危险的那个（例如「可以继续」）。宁可当作「没有 runtime 信息」，让上层退回最保守的分支。

注意 `can_resume: value.can_resume === true` 这种写法：任何非字面 `true`（包括 `"true"`、`1`、`{}`）都归为 `false`。布尔字段用 `=== true` 而不是 `Boolean(...)`，是为了避免 `"false"` 被当成真值——后者是前端从字符串化布尔踩坑的常见来源。

**`normalizeTokenUsage` 与 `normalizeMetrics` 的两级嵌套。** token 用量是六个计数字段，全部用 `numberOrZero`：

```ts
return {
  prompt_tokens: numberOrZero(value.prompt_tokens),
  completion_tokens: numberOrZero(value.completion_tokens),
  cache_read_tokens: numberOrZero(value.cache_read_tokens),
  cache_write_tokens: numberOrZero(value.cache_write_tokens),
  context_window: numberOrZero(value.context_window),
  per_turn_token: numberOrZero(value.per_turn_token),
};
```

用 `0` 而不是 `null` 是因为这些值会直接参与累加与百分比计算——`null + 1` 在 JS 里等于 `1`，但 `null / total` 就是 `NaN`，而 `NaN` 会一路污染到 UI 上的每一个数字。所以在「计数」这类字段上，缺省值必须是数字。

外层 `normalizeMetrics` 则对 `accumulated_cost` 与 `max_budget_per_task` 用 `numberOrNull`——这两个是「可能真的没有」的量（预算可以未设定、成本可以未知），`null` 在 UI 上有明确含义（显示「未设定」而不是「0 美元」）。

**这两处的不对称是这个模块最有信息量的设计点：同一个响应里，`token_tokens` 用 `numberOrZero`、`accumulated_cost` 用 `numberOrNull`，判断依据不是字段类型而是「缺省值在业务上意味着什么」。**

**`normalizeAgent` 保留三个看似冗余的字段。** 它输出 `kind` / `acp_server` / `acp_model` / `llm.model`：

```ts
const llm = isRecord(value.llm) ? { model: stringOrNull(value.llm.model) } : null;
return {
  kind: stringOrNull(value.kind),
  acp_server: stringOrNull(value.acp_server),
  acp_model: stringOrNull(value.acp_model),
  llm,
};
```

源码注释把每个字段的用途都写清了：`kind` 是 SDK 的 pydantic 判别器（`"Agent"` 与 `"ACPAgent"`）；`acp_server` 是 ACP 供应商身份，必须在 `acpserver` 标签缺失时仍能拿到（注释点名「a profile launch doesn't stamp it, #1571」）；`acp_model` 是 Canvas 侧配置的模型，用于 SDK 运行期字段为空时回退。

最后一句话值得单记：「Preserving these makes the wire path agree with the unit-test path that builds `DirectConversationInfo` directly」。即**线上路径与单元测试路径必须产出同一个形状**，否则测试里绿、线上崩。这是归一化函数存在的第二重理由——它不只防脏数据，还保证「同一个类型有两个构造点」时不漂移。

**`normalizeTags` 的 docstring 是一份完整的信任边界声明。** 它说服务端校验器保证键形如 `^[a-z0-9]+$`、值都是字符串，但仍要防御：

> a non-conforming response（older server, raw API write, future schema drift）must never crash the parser

实现只做一件事：值不是字符串就丢掉该条目；整个字段不是对象就返回 `null`（让消费方统一用 `info.tags?.[KEY] ?? null`）。注意它**不校验键名**——因为未知键要保留（自动化与 API 客户端会写自己的标签），消费方靠具名常量取值。

**`normalizeSubConversationIds` 对「字段不存在」与「格式错误」给同一个结果。** 两者都返回 `null`，注释说明这是刻意的：字段在 1.37.1 之前的服务端上不存在，「parsed defensively so a non-conforming payload degrades to "no children" instead of crashing the list」。即「不知道」与「确实没有」在这里被合并成同一件事，因为下游只关心「有没有子会话」。

**`readTimestamp` 处理的是双命名风格**，这是全模块唯一一处「同一语义两个键名」：

```ts
const value = item[snakeKey] ?? item[camelKey];
return typeof value === "string" && value.trim() ? value : DEFAULT_CONVERSATION_TIMESTAMP;
```

`??` 而不是 `||` 是有意的：`||` 会把空字符串当成 falsy 转而用 camelKey，而 `??` 只在 `null`/`undefined` 时才回退——不过紧接着 `value.trim()` 的检查又把空串挡掉了，所以两者最后等效。真正的重点是默认值 `DEFAULT_CONVERSATION_TIMESTAMP = "1970-01-01T00:00:00.000Z"`。

**这个 1970 默认值是一个刻意的「明显的假值」而非「当前时间」。** 因为时间戳会被用来排序，若填当前时间，一条时间戳损坏的会话会突然排到列表最前面；填 1970 则它沉到最底，且一眼能看出异常。用「epoch」当哨兵比用 `null` 更好——排序代码不必处理 `null`。

**`requirePathInsideDirectory` 与 `normalizeAbsolutePath` 是唯一做安全校验的一对。** 它们服务于 `readConversationFile`：从会话沙箱里读 `PLAN.md`。这里有一个真实的攻击面——`filePath` 若可以任意指定，前端就变成了「让浏览器代读后端任意文件」的通道。

`normalizeAbsolutePath` 的做法是**自己解析而不是调库**：

```ts
const normalizedPath = path.replace(/\\/g, "/");
if (/^[A-Za-z]:\//.test(normalizedPath)) { root = normalizedPath.slice(0, 3); remainder = normalizedPath.slice(3); }
else if (normalizedPath.startsWith("/")) { root = "/"; remainder = normalizedPath.slice(1); }
else { return null; }
```

然后是分段处理：跳过空段与 `.`；遇到 `..` 就 `segments.pop()`，但**若 `segments` 已空则直接返回 `null`**——这一条是全函数的安全核心：`/../etc/passwd` 不会「穿过根」变成 `/etc/passwd`，而是直接判为非法。Windows 盘符被当作根，`C:/../x` 同理被拒。

`requirePathInsideDirectory` 再比对：

```ts
if (!normalizedPath || !normalizedDirectory ||
    (normalizedPath !== normalizedDirectory && !normalizedPath.startsWith(`${normalizedDirectory}/`))) {
  throw new Error("Conversation file path must stay inside the workspace");
}
```

三点值得记：

- **两个条件必须同时满足**：要么完全等于目录本身，要么以 `目录 + "/"` 开头。只判 `startsWith(目录)` 会漏掉 `/workspace/project2` 这种前缀相同但不在同一个目录里的路径——这是这类校验最经典的 off-by-separator 错误。
- **校验的是归一化后的路径**，所以 `工作区/../etc` 会先变成「非法」再被拒，而不是先被放行再归一化。
- 路径归一化在**两侧都做**，因为 `workingDir` 本身也可能带 `./` 或反斜杠。

调用点上还看得出这是「服务端也在校验」的双保险：云端分支把默认路径与固定目录 `/workspace/project` 一起传，本地分支则先用 `resolveConversationWorkingDir` 查出真实工作目录再校验——因为本地的工作目录可能是每个会话单独的工作树（worktree），不能写死。

**`requireDirectConversationInfo` 是唯一一个「全字段必过」的入口。** 它先 `hasConversationId(item)`（`id` 是字符串且 `trim()` 后非空），然后逐字段调 `normalize*`，最后返回一个**字段集合完全固定**的对象。这个固定性很重要：下游拿到的一定是这些键，不会出现「某些会话有 `tags` 某些没有」导致的不一致。

同一文件里还有两个批次读取器，它们的差异值得记：

- `requireDirectConversationBatch`：agent-server 对不存在的 id 回 `null`，所以 `null` 条目被**保留为 null**（注释「a null entry means "not found"（the caller decides how to report it）, not an incompatible response」）。即「查无此会话」是正常结果，不是错误。
- `readConversationSearchPage`：兼容「直接回数组」与「回分页对象」两种形态（`Array.isArray(response) ? { items: response } : response`），然后处理 `broken_items`：只接受 `reason` 为 `"metadata"` 或 `"state"` 的条目，且跳过已被健康条目占用 id 的、以及带 `LOCAL_PLANNER_PARENT_TAG_KEY` 的（计划器的父会话由正常路径处理）。这类条目会被补上 `broken_reason` 并把 `conversation_url` / `session_api_key` / `execution_status` / `llm_model` 全部置 `null`——因为元数据或状态损坏的会话**无法运行**，但用户仍需在列表里看到它并删掉它。

`broken_items` 的处理还有一处排序细节：只有当坏条目被追加过（`result.items.length > healthyCount`）才重新按 `updated_at` 排序，注释没说原因，但从代码可读出意图：没有坏条目时不打乱服务端给的顺序。

**`toRuntimeStatus` 是枚举收口的第二处。** `RUNTIME_STATUSES` 是七个值的集合（`idle`/`running`/`paused`/`waiting_for_confirmation`/`finished`/`error`/`stuck`），不在集合里就用 `idle`：

```ts
const nextStatus = status ?? "idle";
return (RUNTIME_STATUSES.has(nextStatus) ? nextStatus : "idle") as RuntimeConversationInfo["status"];
```

`idle` 作为兜底而不是 `error`，是一个「默认不吓人」的选择：未知状态多半是服务端加了新状态，此时显示「空闲」比显示「错误」更接近事实。

## 关键取舍

**源码事实：** 后端响应一律以 `unknown` 进入，由 `require*` / `read*` 做结构校验、`normalize*` 做字段降级；结构性问题抛 `invalidConversationResponse()`，字段性问题静默降级。

**源码事实：** `isRecord` 显式排除数组，因为 `typeof [] === "object"` 会让数组走进对象分支并产出「结构合法但字段全空」的对象。

**源码事实：** 计数类字段（token 用量）缺省为 `0`，可能真的不存在的量（成本、预算）缺省为 `null`；判据是「缺省值在业务上意味着什么」，不是字段类型。

**源码事实：** `normalizeRuntimeInfo` 对 `runtime_status` 做枚举校验，不在五个已知值内就整个丢弃，避免新状态落到 UI 的 `default` 分支。

**源码事实：** 布尔字段一律用 `=== true`，非字面 `true` 归为 `false`，避免字符串化布尔被当成真值。

**源码事实：** 缺时间戳时用 `1970-01-01T00:00:00.000Z` 而非当前时间，让损坏数据在排序里沉底且一眼可辨。

**源码事实：** `requirePathInsideDirectory` 要求路径**等于**目标目录或以「目标目录 + 分隔符」开头，避免 `/workspace/project2` 这类前缀相同的越界；且 `normalizeAbsolutePath` 在 `..` 越过根时直接判非法而不是穿越。

**源码事实：** `requireDirectConversationBatch` 把 `null` 条目保留为 `null`（表示「查无此会话」），而 `readConversationSearchPage` 只部分保留 `broken_items`（`reason` 必须是 `metadata` 或 `state`，且不覆盖健康的同 id 条目）。

**架构推论：** 把「结构错」与「字段错」分开处理，是前端作为信任边界时最实用的分界线。前者意味着「我不认识这份数据，继续下去没有意义」；后者意味着「这份数据基本可用，只是某个属性缺失」。混在一起处理会导致两种极端：全部抛错（一个可选字段缺失就让整个列表失败）或全部降级（收到完全不相干的响应也当空数据处理，用户永远看不到真正的错误）。

**架构推论：** `1970` 哨兵揭示了一个通用技巧：**默认值应当携带「我是默认值」这一信息**。用当前时间填默认值会主动制造错误排序，用 `null` 会强迫排序代码写特例分支，用 epoch 则既不改顺序又能被察觉。

**架构推论：** `normalizeAbsolutePath` 手写解析而不调库，与 browser-use 里 `_strip_userinfo` 手写解析而同类的理由不同：这里的目的是**拒绝**而不是保真。库函数（如 `path.normalize`）会给出一个归一化结果，但不会告诉你「这个路径试图穿越根」；手写才能在那个时刻返回 `null`。安全校验需要的是「拒绝的时机」，而不是「归一化的结果」。

**架构推论：** 一个值得注意的边界：`normalizeAbsolutePath` 把反斜杠统一转成斜杠，于是 Windows 的 UNC 路径（`\\server\share`）会被解析成 posix 风格的 `/server/share`，并且只要目标目录也是 posix 形态就能通过包含检查。这在「只读沙箱内一个固定文件」的当前用途下不构成问题（工作区路径本身来自服务端），但若将来把这个函数复用到「校验用户提供的任意路径」，UNC 这一支需要单独处理。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeWorkspace` | 工作目录的 `{working_dir}` 归一，非对象即 `null` |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeRuntimeInfo` | 沙箱运行态，含 `runtime_status` 枚举校验与 `can_resume` 严格布尔 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeAgent` | 保留 `kind` / `acp_server` / `acp_model` / `llm.model` 四路身份 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeTokenUsage` | 六个计数字段全部 `numberOrZero` |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeMetrics` | 成本与预算用 `numberOrNull`，内嵌 token 用量 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `requirePathInsideDirectory` | 仅允许目标目录内（或目录本身）的绝对路径 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeAbsolutePath` | 手写解析绝对路径，越根即返回 `null` |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `readTimestamp` | 兼容 `created_at` / `createdAt`，缺失回落 epoch |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeTags` | 只收字符串值，键名不限，非对象返回 `null` |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `requireDirectConversationInfo` | 全字段归一化的唯一入口，输出字段集合固定 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `readConversationSearchPage` | 兼容数组/分页两形态，`broken_items` 的选择性并入与重排 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `toRuntimeStatus` | 七值枚举收口，未知归 `idle` |

## 参考

- [OpenHands 源码：agent-server-conversation-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/conversation-service/agent-server-conversation-service.api.ts)
- [OpenHands 仓库：frontend-api-contracts 技能](https://github.com/OpenHands/OpenHands/blob/main/.agents/skills/frontend-api-contracts/SKILL.md)
- [TypeScript 文档：unknown 与类型收窄](https://www.typescriptlang.org/docs/handbook/2/narrowing.html)
- [OWASP：路径穿越](https://owasp.org/www-community/attacks/Path_Traversal)

## 与主核 / AI 的关系

「把不可信的输入逐字段归一化」与主核里的**协议解析**是同一种工程。主核在每个报文进来时都必须回答同一组问题：这个字段在吗？类型对吗？枚举值我认识吗？缺失时该用什么默认？差别只在失败策略——主核多半**拒绝整条报文**（因为它要保证的是数据一致性），而前端只能**降级单字段**（因为它要保证的是「界面还能用」）。

两者在最关键的一点上一致：**默认值不是随便挑的**。主核给缺失的统计字段填 0 会让查询优化器误判成本，所以宁可标记「未知」；这里给缺失的成本填 0 会让用户以为免费，所以用 `null`；给缺失的时间戳填当前时间会打乱排序，所以用 epoch。默认值的选择本身就是一次业务语义判断。

`requirePathInsideDirectory` 那段与主核里的**路径校验**几乎可以逐条对照：两边都要处理相对路径、`..` 穿越、盘符/根前缀、以及「前缀相同但不同目录」这个经典陷阱（`/workspace/project` 与 `/workspace/project2`）。主核的解法通常是先把路径解析成绝对形态、再逐段比对规范化结果；这里也是同一条路，只是拒绝得更早（越根直接 null 而不是穿越回根）。

对 AI 工程的实际意义在于：agent 类应用的前端实际上是一个**协议翻译层**——它要把「后端可能来自不同版本、不同部署形态」的数据翻译成一套稳定的 UI 类型。这份实现给出的三条经验可以直接用：**结构与字段分开处理**（否则要么太脆要么太瞎）、**枚举要收口**（未知状态别让它流到 `switch` 的 `default`）、以及**同一个类型不要有两个构造点**（注释里那句「wire path agree with the unit-test path」正是为此）。最后一条在 agent 框架里尤其重要，因为会话对象常常既由 HTTP 构造、也由测试夹具直接构造，一旦两者漂移，测试就失去了对线上的保护力。
