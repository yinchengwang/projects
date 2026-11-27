---
project: openhands
feature: event-service
title: 事件检索与确认响应
page_type: feature
schema_version: 1
refs:
  - path: src/api/event-service/event-service.api.ts
    symbols: [EventService]
  - path: src/api/event-service/event-service.types.ts
    symbols: [EventSortOrder, EventSearchOptions, ConfirmationResponseRequest]
---

# 事件检索与确认响应

## 一句话职责

`EventService` 只做三件事：应答确认请求、查事件总数、按页搜索事件历史。它的复杂度不在这些调用本身，而在**同一个逻辑能力在云端被拆到两个不同的上游主机上**，且其中一个必须由浏览器直连。

## 机制是什么

会话的事件流在两种后端里是两套完全不同的拓扑：

| 能力 | 本地（agent-server） | 云端 |
|---|---|---|
| 事件历史（`search`） | 会话所在主机，`RemoteEventsList` | **App API**（云端后端持久化，能活过沙箱） |
| 事件计数（`getEventCount`） | 会话所在主机 | **运行时沙箱**（从 `conversation_url` 提取） |
| 确认应答（`respondToConfirmation`） | 会话所在主机 | **运行时沙箱** |

云端的拆分理由写在文件顶部的注释里：事件历史由云端后端持久化，「survives the runtime sandbox」；而计数与确认应答是**运行时状态**，只存在于沙箱里。于是同一个会话的两类数据来自两个地方。

更复杂的一层是认证方式与网络路径也不一样：

- App API 走 `callCloudProxy`，用 bearer；
- 运行时沙箱从浏览器**直接**跨域访问，用 `X-Session-API-Key`。

注释明确了这条要成立的两个前提：运行时的 CORS 允许列表（`OH_ALLOW_CORS_ORIGINS`，在部署里被设成 Canvas 的源）接受这个跨域请求，且 `X-Session-API-Key` 的预检能过。

本地模式则「targets the conversation's host directly via typed client classes — the same path cloud mode now uses」——即两条路径最终都落到同一批类型化客户端，只是云端的那批要额外处理「历史在别处」这件事。

## 怎么实现的

**三个方法，两种构造客户端的姿势。** `respondToConfirmation` 与 `getEventCount` 长得几乎一样：

```ts
return new ConversationClient(
  getAgentServerClientOptions({ conversationUrl, sessionApiKey }),
).respondToConfirmation<ConfirmationResponseResponse>(conversationId, request);
```

它们都**显式传入 `conversationUrl` 与 `sessionApiKey`**，而其余会话 API 大多用无参的 `getAgentServerClientOptions()`。这个差异就是「运行时直连」的编码：客户端被指向每个会话自己的运行时主机，并带上该会话的会话密钥。

这一步的意义可以和 `searchEvents` 的云端分支对照：`searchEvents` **不接受** `conversationUrl` 作为云端历史请求的目标，它在云端走 `callCloudProxy`。也就是说同一个 `EventService` 里，方法之间对「目标主机」的假设不同，而这正是拓扑差异在代码上的落点。

**`searchEvents` 的参数收口。** 它接受 `EventSearchOptions`，默认 `limit = 100`（云端再 `Math.min(limit, 100)`，因为服务端上限就是 100）。选项逐个映射到线上参数名：

```ts
const params = new URLSearchParams();
params.set("limit", String(cloudLimit));
if (options.sortOrder) params.set("sort_order", options.sortOrder);
if (options.pageId) params.set("page_id", options.pageId);
if (options.timestampGte) params.set("timestamp__gte", options.timestampGte);
if (options.timestampLt) params.set("timestamp__lt", options.timestampLt);
if (options.excludeFullState) params.set("exclude_full_state", "true");
```

注意 `timestamp__gte` 是**双下划线**，而 `sort_order` / `page_id` 是单下划线。这种不一致不是笔误——它是把后端的字段过滤语法（`__gte` 是 Django 风格的范围查询后缀）直接暴露到了线上，而排序与分页是普通查询参数。前端在这里只是照抄，没有做统一化。

`excludeFullState` 转成字符串 `"true"` 而不是布尔，因为 `URLSearchParams` 的值只能是字符串。它对应 `EventSearchOptions` 里那句「Omit full state snapshots from Cloud display history when supported」——完整的 state 快照很大，展示历史时不需要。

**云端分页的降级策略是这一页最有意思的一段。** 注释先说清背景：

> Full pagination params（sort_order, page_id, timestamp filters）require the server-side fix from OpenHands/OpenHands#14399. If the cloud backend hasn't been updated yet, the timestamp filters trigger a 500（str-vs-datetime comparison）.

于是代码的做法是「先试完整请求、失败再降级」：

```ts
try {
  const data = await doCloudSearch(params);
  return { items: data?.items ?? [], next_page_id: data?.next_page_id ?? null };
} catch (err) {
  if (!hasFilterParams) throw err;
  if (options.strictPagination) throw err;
  console.warn("[EventService] Cloud backend doesn't support pagination filters. ...");
  return { items: [], next_page_id: null };
}
```

**降级的结果是空页，而不是「只用 limit 再查一次」。** 这个选择被注释明确解释过：

> A limit-only fallback would return the same most-recent events already in the store, which get deduped but keep hasMore=true.

展开来说：如果降级成「只带 `limit`」，服务端会返回最前面那一页——而那页的事件前端**已经有了**。去重后新增为零，但 `next_page_id` 仍然非空，于是「还有更多」永远为真，调用方会不停请求下一页却永远拿不到新内容。这是一个典型的**无限重试**陷阱：结果集去重后为空，但分页游标不变。

返回空页则不同：空页 + `next_page_id: null` 明确表达「没有更多了」，分页循环自然终止。代价是用户只能看到首屏事件——但这比一个不停转圈的分页器好。

**`strictPagination` 是给需要完整结果的调用方的逃生门。** `EventSearchOptions` 里它的注释写得比代码更重要：

> Surface unsupported cloud pagination instead of degrading to an empty page. Callers that require a complete result（such as transcript export）must set this so a partial history is never mistaken for exhaustion.

即同一个降级行为对两类调用方是相反的：

- **浏览界面**：降级成空页是对的——总比无限重试好。
- **导出转录**：降级成空页是灾难——导出的文件会**静默地少掉后续所有事件**，而调用方以为已经到底了。

所以这个选项不是性能开关，而是**语义声明**：「我需要完整，宁可失败也不要部分」。这类「同一降级策略对不同消费者有相反后果」的情况，在分页接口上非常常见，而把决定权交给调用方是最省事也最准确的做法。

三个守卫的顺序也值得看：`if (!hasFilterParams) throw err` 在前——若这次请求本来就没带过滤参数，失败就不是「后端不支持过滤」引起的，直接抛出；`strictPagination` 的检查在后。这个顺序保证降级只在「确实是因为过滤参数失败」时发生。

**`hasFilterParams` 的构成**是四个参数的或：

```ts
const hasFilterParams = !!(options.sortOrder || options.pageId || options.timestampGte || options.timestampLt);
```

注意 `excludeFullState` **不在其中**——说明这个参数不依赖 #14399 的修复，可以单独使用。

**本地分支走 `RemoteEventsList`，且参数名与云端一致。** 本地分支：

```ts
const page = await new RemoteEventsList(
  getAgentServerHttpClientOptions({ conversationUrl, sessionApiKey }),
  conversationId,
).search({
  limit,
  ...(options.pageId ? { page_id: options.pageId } : {}),
  ...(options.sortOrder ? { sort_order: options.sortOrder } : {}),
  ...(options.timestampGte ? { timestamp__gte: options.timestampGte } : {}),
  ...(options.timestampLt ? { timestamp__lt: options.timestampLt } : {}),
});
```

注意两件事：一是**没有 `strictPagination`**（本地不需要降级，参数一定支持）；二是**没有 `excludeFullState`**——它在本地分支被静默忽略。也就是说这个选项只在云端生效，本地调用方传了也不会报错，只是没效果。这是一处不对称，而 docstring 里用的措辞「when supported」正是为此留的余地。

还注意 `getAgentServerHttpClientOptions` 而不是 `...ClientOptions`：本地搜索用的是 **HTTP** 客户端选项（因为是 REST 分页查询），而前面两个方法用的是 `ConversationClient`。两种客户端类对应两类传输。

**返回值归一化只有一行，但有一处类型断言。**

```ts
return {
  items: (page?.items ?? []) as OpenHandsEvent[],
  next_page_id: page?.next_page_id ?? null,
};
```

`as OpenHandsEvent[]` 是这个文件里唯一的类型断言，而 d01 那套 `normalize*` 的做法在这里没有出现。理由可以从类型上看出来：`RemoteEventsList.search` 返回的是类型化客户端已经解析过的形状，`page?.items` 的类型是客户端自己的事件类型，而前端要用的是 `OpenHandsEvent`。这两个类型来自不同的包（`@openhands/typescript-client` 与 `#/types/agent-server/core`），前端选择信任客户端的解析结果而不是再归一化一遍。

云端分支则**完全没有断言**：`data?.items ?? []` 直接进入 `EventSearchPage<OpenHandsEvent>`，靠 `callCloudProxy` 的泛型参数声明的类型。也就是说云端这条路径在这里**没有运行期校验**——`callCloudProxy<EventSearchPage<OpenHandsEvent>>` 只是编译期的假设。这与 d01 里「每个字段都归一化」形成了明显对比：会话列表那类数据被当成不可信输入，而事件流这里被直接信任。差别大概源自两处：事件流的结构极简（只有 `items` 与 `next_page_id`），以及坏了之后的后果不同（事件渲染失败只影响一段历史，会话列表失败会让整个界面不可用）。

**`EventSearchOptions` 与 `EventSearchPage` 是轻量类型。**

```ts
export type EventSortOrder = "TIMESTAMP" | "TIMESTAMP_DESC";
export interface EventSearchPage<TEvent> { items: TEvent[]; next_page_id?: string | null; }
```

`EventSearchPage` 是泛型的，本地与云端都复用它，只是 `TEvent` 都是 `OpenHandsEvent`。`next_page_id` 既是可选又是 `| null`，与实现里 `?? null` 的写法对应——两种「没有下一页」的表达（缺席与显式 null）被归一到同一个值。

`ConfirmationResponseRequest` / `ConfirmationResponseResponse` 是确认流程的最小契约：`{ accept: boolean; reason?: string }` 与 `{ success: boolean }`。`reason` 可选意味着「拒绝时可以给理由」，而 `accept` 必填意味着必须明确表态——没有「默认接受」这种模糊态。

## 关键取舍

**源码事实：** 云端把事件历史放在 App API（`/api/v1/conversation/{id}/events/search`，bearer 认证，走 `callCloudProxy`），把计数与确认应答放在运行时沙箱（`/api/conversations/{id}/events/...`，`X-Session-API-Key` 认证，浏览器跨域直连）。

**源码事实：** 运行时直连依赖两个外部前提：部署时设定的 CORS 允许列表 `OH_ALLOW_CORS_ORIGINS` 与 `X-Session-API-Key` 的预检通过。

**源码事实：** 云端分页在不支持过滤参数时**返回空页**而不是「只用 limit 再查」，因为后者会返回已在本地存储中的事件、去重后新增为零但 `next_page_id` 仍非空，导致无限重试。

**源码事实：** `strictPagination` 让需要完整结果的调用方（如转录导出）把「不支持」升级为错误，避免把「降级」误认为「到底了」。

**源码事实：** `excludeFullState` 只在云端生效，本地分支静默忽略；它也不计入 `hasFilterParams`，即不依赖 #14399 的修复。

**源码事实：** 线上参数名不统一：范围过滤用双下划线（`timestamp__gte` / `timestamp__lt`），排序与分页用单下划线（`sort_order` / `page_id`），是照抄后端的混合语法。

**源码事实：** 本地分支用 `RemoteEventsList` + HTTP 客户端选项，云端历史走 `callCloudProxy` 泛型，两个运行时方法用 `ConversationClient`；只有本地分支有一处 `as OpenHandsEvent[]` 断言，云端分支无运行期校验。

**架构推论：** 这页的核心不是「怎么查事件」，而是「同一个能力在两种部署下被物理拆开」时前端要付的代价。云端之所以要拆，是因为历史必须活在能活过沙箱的地方（否则沙箱回收就丢历史），而计数与确认应答必须活在沙箱里（否则就失去「当前运行时」的语义）。前端的应对是把「目标主机」从一个隐含的全局概念细化成每个方法各自的决定。

**架构推论：** 「降级成空页」这个决定值得单独记住。多数分页实现在遇到不支持时会选择「尽量返回一些」，而这里选择了「明确返回没有更多」。判据是**哪一种错误更容易被发现**：返回部分数据会让调用方在导出时静默丢内容，而返回空页会让界面立刻显示「历史到此为止」——后者是可见的、可被用户报告的错误。

**架构推论：** `strictPagination` 体现了「把降级决定权交给调用方」这一模式。同一条降级策略（返回空页）在两种消费者上是相反的好坏，代码无法替它们决定，所以必须由调用方显式声明自己的容错期望。这与 d01 里「结构错抛、字段错降级」的分层判断同源：**错误的严重性取决于使用场景，而不只是错误本身**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/event-service/event-service.api.ts` | `EventService` | 事件三能力：确认应答、计数、分页搜索 |
| `src/api/event-service/event-service.api.ts` | `EventService.respondToConfirmation` | 运行时直连，带 `conversationUrl` 与 `sessionApiKey` |
| `src/api/event-service/event-service.api.ts` | `EventService.getEventCount` | 同上；云端指向运行时沙箱 |
| `src/api/event-service/event-service.api.ts` | `EventService.searchEvents` | 云端走 App API 并降级、本地走 `RemoteEventsList` |
| `src/api/event-service/event-service.types.ts` | `EventSearchOptions` | 分页/排序/时间范围/省略快照/严格分页 |
| `src/api/event-service/event-service.types.ts` | `EventSearchPage` | 泛型分页形状，`items` + `next_page_id` |
| `src/api/event-service/event-service.types.ts` | `EventSortOrder` | `TIMESTAMP` / `TIMESTAMP_DESC` 两值 |
| `src/api/event-service/event-service.types.ts` | `ConfirmationResponseRequest` | `accept` 必填、`reason` 可选的确认契约 |

## 参考

- [OpenHands 源码：event-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/event-service/event-service.api.ts)
- [OpenHands 仓库：frontend-api-contracts 技能](https://github.com/OpenHands/OpenHands/blob/main/.agents/skills/frontend-api-contracts/SKILL.md)
- [software-agent-sdk 仓库：事件与 REST/WebSocket 契约](https://github.com/OpenHands/software-agent-sdk)
- [MDN：跨源资源共享（CORS）预检](https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS)

## 与主核 / AI 的关系

「同一个逻辑能力被拆到不同上游」与主核里的**读写分离**是同一种结构：主核的读可能走副本、写必须走主库，两个目标的主机、认证、一致性语义都不同。这里也一样——历史读走持久化后端，运行时状态读走当时的执行体。两边的共同教训是：一旦拆分发生，调用方就不能再假设「同一个对象的所有方法指向同一个主机」。

「降级成空页而不是部分数据」这条与主核里**快照隔离下的可见性**有一个相同的判断原则：宁可让读者看到「没有更多」，也不要让他看到一份**看起来完整但实际缺了尾部**的数据。主核在备份与导出上尤其如此——一个截断的备份比一个失败的备份更危险，因为它会被当成好的。

`strictPagination` 这类「由调用方声明容错期望」的选项，在主核里对应的是 `ON_ERROR_STOP` 之类的开关：批处理脚本需要「遇错即停」，而交互式会话需要「跳过错误继续」。同一个错误，两种消费者的正确答案相反，所以开关必须暴露给最了解场景的那一层。

对 AI 工程的实际意义在于：agent 的事件流是长跑会话的主线数据，它的分页可靠性直接决定「历史能不能完整回看」。这份实现给出的两条经验可直接用：**降级要能被看见**（返回空页 + 明确终止游标，而不是悄悄少几条）、以及**把「我必须完整」做成显式选项**（否则导出这类调用方会在无人察觉的情况下产出残缺结果）。第三点也可借鉴：把「目标主机」写进方法签名（`conversationUrl`）而不是藏在全局配置里，让拓扑差异在类型层面可见。
