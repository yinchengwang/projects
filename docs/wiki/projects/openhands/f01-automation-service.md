---
project: openhands
feature: automation-service
title: 自动化与自定义 Webhook
page_type: feature
schema_version: 1
refs:
  - path: src/api/automation-service/automation-service.api.ts
    symbols: [AUTOMATION_BASE_PATH, AUTOMATION_KV_PATH, automationCreateEndpointForTarget]
  - path: src/api/automation-service/automation-service.api.ts
    symbols: [CustomWebhook, AutomationHealthResponse, normalizeAutomationSdkVersion]
---

# 自动化与自定义 Webhook

## 一句话职责

前端对「自动化服务」的唯一客户端：把同一组功能（列出/创建/启停自动化、草稿、工厂、KV 存储、自定义 Webhook、健康检查）在**云后端**与**本地 agent-server + automation 侧车**两套部署下分派出去，并且把「这个部署到底支持哪些功能」这件事交给一个**能力声明**而不是靠版本号猜。

## 机制是什么

这个文件有 1677 行，是这一批里最大的。它的结构可以归成四层：

| 层 | 内容 |
|---|---|
| 路径常量 | `AUTOMATION_BASE_PATH = '/api/automation'`、`AUTOMATION_KV_PATH = AUTOMATION_BASE_PATH + '/v1/kv'` |
| 端点解析 | `automationCreateEndpointForTarget` 把「创建目标」映射到具体端点；通用端点走 `getAutomationEndpoint(...)` |
| 请求分派 | 每个静态方法都是「读 `getActiveBackend().backend` → 云走 `callCloudProxy`，本地走 `localAutomationAxios`」 |
| 归一化 | `normalizeAutomationSdkVersion`、`normalizeDraftResponse`、`normalizeValidationErrors`、`normalizeRepositoryPage` 类的形状收敛 |

而 `CustomWebhook` / `AutomationHealthResponse` 这两个类型是本页抽出来看的具体机制：一个涉及**只在创建时出现一次的密钥**，一个涉及**把失败当成一种正常状态**。

## 怎么实现的

**两套部署走两套 HTTP 通道，但共用同一套请求头。** `localAutomationAxios` 上方的注释说明了本地那侧的物理形态：**本地自动化调用打到 `scripts/dev-with-automation.mjs` 挂在本地 agent-server 后面的 automation 侧车**；而注释紧接着说：**两个后端用同一个会话 API 密钥与同一个 `X-Session-API-Key` 头，为了一致性**。

也就是说「本地」其实也是一个独立的 HTTP 服务，只是它由开发脚本挂在本地 server 后面。这使得分派逻辑可以对称：两边都是 HTTP，只是传输封装不同（`callCloudProxy` 走 App API 代理，`localAutomationAxios` 直连）。

**请求头构造是异步的，因为它要取遥测身份。** `buildAutomationRequestHeaders` 是 `async` 并且先 `await getTelemetryDistinctId()`，然后把 `AGENT_CANVAS_CLIENT_HEADERS`、可选的遥测 distinct id 头、以及调用方给的额外头合并起来。三个 `...` 的顺序决定了优先级：**额外头最后展开，所以能覆盖前面**。

这个设计意味着**每个请求都要等一次遥测身份的异步取用**。它值得记一笔：这是一个把「客户端身份」与「服务端请求」绑在一起的常见做法，代价是热路径上多一个 await 点。

**`automationCreateEndpointForTarget` 是唯一显式抛错的端点解析。** 它按目标分四支：

- `plugin` → `getAutomationEndpoint('createPlugin')`
- `custom` → `getAutomationEndpoint('createBundle')`，**取不到就抛** `This deployment does not support creating custom automation bundles.`
- 无目标或 `prompt` → `automationCreateEndpoint()`
- 其余（SetupEntry）→ `automationCreateEndpoint(target, selectedAction)`

第三支的那个抛出是本页第一条值得记的判断：**「这个部署没有这个端点」被表达成一条明确的、面向用户的错误**，而不是静默降级到另一个端点。因为创建自定义 bundle 与创建普通自动化在后端是两个完全不同的动作，降级会造出错误的东西。

而 `getAutomationEndpoint` 来自 `constants/automation-form`（或相邻模块）。它返回的是**相对路径片段**——所有调用点都写成 `` `${AUTOMATION_BASE_PATH}${getAutomationEndpoint(...)}` ``。这种「基础路径 + 端点表」的拼法让「端点表随部署变化」与「基础路径固定」两件事独立。

**能力声明是一个字符串列表，不是一个版本号。** `supportsAutomationDrafts(capabilities)` 的注释与实现都指向同一个模式：**这个部署是否宣称支持服务端自动化草稿（`automationDrafts` 这个 feature，来自 `OpenHands/automation` PR #417）；持久化草稿的表单流程据此决定**，于是**旧部署继续用纯客户端的草稿存储**。

而 `constants/automation-form.ts` 里另有两个同类开关，注释都写明了各自对应的能力：

- `SERVICE_ACCOUNT_RUN_AS_FEATURE = 'serviceAccountRunAs'`：能作为服务账号运行（`run_as_kind: 'service'`、创建时的 `run_as_user_id`、`POST /v1/{id}/run-as`）
- `FACTORIES_FEATURE = 'factories'`：能提供工厂（`/v1/factories`、自动化上的 `factory_id`）

加上 `automation-kv.ts` 里的 `AUTOMATION_KV_FEATURE = 'kvStore'`，一共四个能力位。**四个位各自对应一组端点**，于是「这个部署支不支持某功能」变成一个查询而不是一次试错。

这是本页最值得学的模式：**用能力声明而不是版本号比较**。版本号比较需要知道「哪个版本加了什么」，而这个知识会过期；能力声明是部署自己说的，加功能时只需多声明一个字符串。

**`AutomationHealthResponse` 的 `status` 只有两个值，而健康检查把异常吞成了 `error`。** 类型是 `{ status: 'ok' | 'error'; message?: string }`，而 `checkHealth` 的实现是一个 `try/catch`：云路径 `callCloudProxy(..., timeoutSeconds: 5)`，本地路径 `localAutomationAxios.get(path, { timeout: 5000 })`，**catch 一律返回 `{ status: 'error' }`**。

两处都设了 5 秒超时，且云路径的注释说明了对称性：**快速失败，与下面本地分支的 5 秒超时一致**。而整个函数**从不抛错**——它把「服务不可用」与「服务回了 error」合并成同一个返回值。

这个选择与 `g01` 的「空页而不是异常」是同一种取向：**健康检查的调用方只想知道「能不能用」，而不关心为什么不能用**。代价是诊断信息丢失（`message` 只在服务真的回了一个 body 时才存在）。

**`CustomWebhook` 与 `CustomWebhookCreateResponse` 的差异正是「密钥只出现一次」。** 列表/详情类型 `CustomWebhook` 有 `webhook_url`、`event_key_expr`、`signature_header`、`signature_scheme`、`enabled`、时间戳，**但没有 secret**。而创建响应 `CustomWebhookCreateResponse extends CustomWebhook` 加了一个可选 `webhook_secret?: string | null`。

类型层面把这个区分做出来，等于**在编译器上禁止了「从列表里读 secret」这件事**。创建一个 webhook 时服务端生成签名密钥并只返回一次，之后任何查询都拿不到——这是签名密钥类数据的标准做法（与 `e02` 里 MCP 凭据的「展示脱敏 + 使用回填」是同一族问题，但这里的解法更强：**不展示**）。

而 `CustomWebhookCreateRequest` 里 `webhook_secret?: string` 是**可选入参**：用户可以自带密钥（例如与已有系统共享），不给就由服务端生成并返回一次。

**三个签名相关字段构成一个可配置的签名约定。** `signature_header`（放在哪个头里）、`signature_scheme`（例如 HMAC 算法）、`event_key_expr`（从载荷里取事件键的表达式）。把它们都做成字段而不是常量，说明这套 webhook 要对多种外部系统兼容——每个系统的签名约定都不同。

**`normalizeAutomationSdkVersion` 处理两种线形态，且空串归一成 `null`。** `AutomationSdkVersionResponse` 是 `string | { sdk_version?, version? }` 的联合。`getAutomationSdkVersionFromResponse` 先试 `sdk_version`、再退 `version`，两个都过 `normalizeAutomationSdkVersion`（`typeof !== 'string'` 返回 `null`；`trim()` 后为空也返回 `null`）。

注意回退顺序与 `??` 的用法：`normalize(...) ?? normalize(...)` 意味着**字段存在但值为空串时也会退到下一个**，因为归一化已经把它变成 `null` 了。这是一个正确的细节——否则 `sdk_version: ""` 会遮住一个有效的 `version`。

**`buildListQuery` 的注释把「省略 undefined」当成了它的契约。** docstring：**分页列表请求的查询串。`extra` 加上过滤之类的参数；值为 undefined 的键被略去**。

实现是 `URLSearchParams` + 循环里 `if (value !== undefined)`。而 `limit`/`offset` 是无条件 `set` 的（先转成 `String`）。这与 `g01` 里 `query || undefined` 的手法一致——**「没有这个条件」必须表达成参数缺失，而不是参数为空**。

**`DRAFT_ENDPOINTS` 是一个显式的白名单，`isDraftEndpoint` 用它做类型守卫。** 三个端点是 `'/v1'`、`'/v1/preset/prompt'`、`'/v1/preset/plugin'`。守卫的实现是 `DRAFT_ENDPOINTS.includes(value as AutomationDraftEndpoint)`——一个数组查找，而不是正则或字符串前缀判断。

**把端点路径当数据（而不是散落在各方法里）**，让「哪些端点属于草稿族」可以一次看见，也让类型守卫有一个真值源。

**`syncTelemetryConsent` 只在本地后端做，且带一个「撤销时要清理待发队列」的分支。** 前半句是 `if (active.backend.kind !== 'local') return;`——云端的遥测同意由云侧管，前端不推。后半句：`if (consent !== 'granted' && frontendDistinctId) clearPendingLocalTelemetryRevocation(frontendDistinctId)`。

这条分支的理由在于**撤销同意本身需要一次网络请求**：如果这次请求失败，就有「已经声称撤销、但服务端还在收」的风险。所以本地维护一个「待撤销」队列，让同意撤销可以被重试。这是一个把「隐私操作必须最终生效」当成功效要求的设计。

**`buildPinnedLocalConfig` / `buildPinnedCloudHeaders` 是「把当前后端固化成一组请求配置」的两个函数。** 本地版给 `baseURL: backend.host` 加 `X-Session-API-Key`（有 apiKey 才加）；云端版给 `X-Org-Id`（有 orgId 才加）。

它们的存在说明有一批请求**需要绕过「读当前活跃后端」这个动态分派**——例如一个后台任务在切换后端之后仍要继续打原来那个。所以「pin 住」是一等概念，而不是临时 hack。

## 关键取舍

**源码事实：** 「这个部署支不支持某功能」用**能力声明的字符串列表**判定（`automationDrafts` / `serviceAccountRunAs` / `factories` / `kvStore`），不用版本号比较。旧部署因此自动回退到纯客户端行为。

**源码事实：** 「创建自定义 bundle 的端点不存在」被显式抛错，不降级到普通创建端点——两者在语义上不同。

**源码事实：** `checkHealth` 从不抛错，5 秒超时（云与本地对称），异常与 `status: 'error'` 的响应合并成同一个返回值。代价是诊断信息丢失。

**源码事实：** `CustomWebhook` 没有 secret 字段，只有 `CustomWebhookCreateResponse` 有（可选）。类型层面禁止从列表读密钥，密钥只在创建时出现一次。

**源码事实：** 签名约定被拆成三个字段（头名、方案、事件键表达式），为了兼容不同外部系统的约定。

**源码事实：** `normalizeAutomationSdkVersion` 把空串归一成 `null`，于是 `sdk_version: ""` 不会遮住有效的 `version`。

**源码事实：** 查询串里「没有条件」表达成参数缺失而不是空值；`buildListQuery` 的 docstring 把这条写成了契约。

**源码事实：** 遥测同意撤销带「待撤销」队列——撤销操作必须最终生效，不能因为一次请求失败而静默失效。

**源码事实：** 存在「固化为当前后端」的请求配置构造函数（本地紧固 `baseURL` + 会话密钥，云端紧固 org id），说明有一批请求必须绕过动态分派。

**架构推论：** 这个 1677 行的文件里几乎没有「业务逻辑」——它几乎全是**适配**。两套部署 × 四个能力位 × 十几个端点，让每个方法都变成三行同构的「读后端 → 分派 → 归一化」。这种代码的维护成本不在写，而在**一致性**：新增一个端点必须同时加进端点表、加进云代理路径、并考虑能力位。而能力声明的存在正是把这部分成本压下去的手段——它让「旧部署怎么办」有一个不依赖版本比较的答案。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/automation-service/automation-service.api.ts` | `AUTOMATION_BASE_PATH` / `AUTOMATION_KV_PATH` | 基础路径与 KV 子路径 |
| `src/api/automation-service/automation-service.api.ts` | `automationCreateEndpointForTarget` | 按创建目标解析端点，缺端点时显式抛错 |
| `src/api/automation-service/automation-service.api.ts` | `AutomationService.supportsAutomationDrafts` | 能力位判定，旧部署回退纯客户端草稿 |
| `src/api/automation-service/automation-service.api.ts` | `normalizeAutomationSdkVersion` | 版本归一，空串变 `null` 以便回退 |
| `src/api/automation-service/automation-service.api.ts` | `buildAutomationRequestHeaders` | 异步请求头，含遥测身份与额外头覆盖 |
| `src/api/automation-service/automation-service.api.ts` | `buildListQuery` | 分页查询串，未定义的条件被略去 |
| `src/api/automation-service/automation-service.api.ts` | `DRAFT_ENDPOINTS` / `isDraftEndpoint` | 草稿端点白名单与类型守卫 |
| `src/api/automation-service/automation-service.api.ts` | `AutomationHealthResponse` | 两值状态，`message` 仅在服务回了 body 时存在 |
| `src/api/automation-service/automation-service.api.ts` | `checkHealth` | 5 秒超时对称，异常合并为 `error` 且从不抛 |
| `src/api/automation-service/automation-service.api.ts` | `CustomWebhook` / `CustomWebhookCreateResponse` | 密钥只在创建响应里存在的类型分界 |
| `src/api/automation-service/automation-service.api.ts` | `CustomWebhookCreateRequest` | 三字段签名约定 + 可选自带密钥 |
| `src/api/automation-service/automation-service.api.ts` | `syncTelemetryConsent` | 仅本地推送，撤销带待发队列 |
| `src/api/automation-service/automation-service.api.ts` | `buildPinnedLocalConfig` / `buildPinnedCloudHeaders` | 把当前后端固化成请求配置 |

## 参考

- [OpenHands 源码：automation-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/automation-service/automation-service.api.ts)
- [OpenHands 文档：自动化](https://docs.openhands.dev/openhands/usage/automations)
- [MDN：Webhook 与签名校验](https://developer.mozilla.org/en-US/docs/Web/API/Webhook)
- [GitHub 文档：webhook 载荷签名](https://docs.github.com/en/webhooks/using-webhooks/validating-webhook-deliveries)

## 与主核 / AI 的关系

这层与主核里「服务端能力协商」是同构的：客户端与服务端都要先确定对方支持什么，再决定用哪套协议。数据库用客户端/服务端版本协商 + 能力查询（`pg_settings`、`SHOW`、扩展列表）；这里用能力字符串列表。两条路都指向同一个结论：**能力声明比版本号比较更耐用**，因为功能可以 backport，而版本号不会。

对 agent 工程的直接价值有三条。其一，**「这个功能有没有」要用能力位，不要用版本号**。版本号需要一张永不更新的对照表，而能力位是部署自己说的。其二，**创建类响应与查询类响应的类型要分开**。webhook 密钥只在创建时返回一次，用两个类型把这件事写进编译器，比写在文档里有效得多。其三，**健康检查要能合并「不可用」与「回了错误」**。调用方通常只需要一个布尔，而把两者区分开会迫使每个调用点重复同一段判断——代价是排查时拿不到原因，所以 `message` 字段的存在（在能拿到时）就成了一种补偿。
