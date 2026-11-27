---
project: openhands
feature: bash-service
title: Bash 输出分页与检索
page_type: feature
schema_version: 1
refs:
  - path: src/api/bash-service/bash-service.api.ts
    symbols: [isBashOutput, MAX_OUTPUT_PAGES, BashService]
  - path: src/api/bash-service/bash-service.api.ts
    symbols: [SearchOptions]
---

# Bash 输出分页与检索

## 一句话职责

把一个 bash 命令的全部 `BashOutput` 事件**按时间顺序拼成一份完整输出**给界面看：逐页检索、翻到最后一页为止、按命令顺序累加——并且在云/本地两种后端下打**不同的查询作用域**，因为本地只有一个 agent-server 而云端没有共享的 bash 事件宿主。

## 机制是什么

106 行，四个部件：

| 部件 | 作用 |
|---|---|
| `SearchOptions` | 检索参数：`kind__eq`、`command_id__eq`、`sort_order`、`page_id`、`limit` |
| `MAX_OUTPUT_PAGES = 20` | 分页上限，注释说明「超过 2000 个输出事件不太可能」 |
| `isBashOutput(event)` | 类型守卫：`event.kind === 'BashOutput'` |
| `BashService.listOutputs` / `searchEvents` | 公开入口（全量拼接）与私有检索（分后端作用域） |

`listOutputs` 的 docstring 一句定了语义：**取一个 bash 命令的全部 `BashOutput` 事件，分页并按时间戳排序。按命令发出的顺序返回事件，于是调用方可以直接拼接 `stdout` / `stderr` 的值。**

## 怎么实现的

**分页循环的形状是「不确定页数、读一页拼一页」。** `listOutputs` 用 `for (let i = 0; i < MAX_OUTPUT_PAGES; i += 1)` 逐页取，每页把 `page.items` 里属于 `BashOutput` 的收进结果，然后 `if (!page.next_page_id) break;` 否则 `pageId = page.next_page_id`。

有两个细节值得记：

1. **终止条件是「没有下一页」而不是「页数到上限」。** 上限只是个保险，正常路径靠 `next_page_id` 为假退出——也就是说它是一个**游标式分页**，而不是「按偏移量算总页数」。
2. **`page_id` 只在非空时加入查询**（`...(pageId ? { page_id: pageId } : {})`）。首轮请求不带 `page_id`，这与 `g01`/`f01` 里的「缺省即省略」是同一手法。

**`MAX_OUTPUT_PAGES` 的注释是本页最有信息量的一行：`safety cap; >2000 output events is unlikely.`** 它把「上限怎么定的」讲清了——不是拍一个数字，而是「一个命令的输出事件超过 2000 个不太可能」。也就是说这个上限的作用是**防无限循环**（一个总是返回 `next_page_id` 的坏服务会把 `for` 变成无限），而不是业务约束。

而它被截断时的行为是**静默返回前 20 页**，不报错也不标记。这是一个显式的取舍：输出可能不完整，但界面不会因为一个异常长的命令而崩。代价是「被截断」这件事对调用方不可见。

**两套作用域差异全部集中在 `searchEvents` 的一段注释里，值得逐句读。** 原文大意：

> Bash 事件住在拥有这个会话的 agent-server 运行时上。在**本地**模式，我们直接跟活跃后端的 agent-server 说话、在服务器层用 SDK 的 `BashClient`，**忽略任何 per-conversation URL**：一个单独的本地 agent-server 承载所有会话，而自动化的命令是在它上面、**在任何会话之外**跑的，所以一次会话作用域的检索（`/api/conversations/<id>/bash/...`）永远找不到它的输出。

> 在**云**模式，我们从浏览器直接调用同一个 per-conversation 运行时宿主：运行时的 CORS 允许列表（`OH_ALLOW_CORS_ORIGINS`，在 saas-deploy 里设成 Canvas 的 origin）允许这次跨源请求，而运行时端点用会话的 `X-Session-API-Key` 认证。

这段话解释了一个**非对称**：本地模式下**必须忽略** `conversationUrl`（不是可选、是必须），因为自动化命令不属于任何会话；而云模式下**必须提供**它，因为没有别的宿主。

**代码把这条非对称做成了「只在云下传 conversationUrl」。** 实现是：

```ts
getAgentServerClientOptions({
  ...(isCloud && conversationUrl ? { conversationUrl } : {}),
  sessionApiKey,
})
```

注意条件里同时有 `isCloud` 与 `conversationUrl`：**本地模式下即使调用方传了 `conversationUrl` 也不会被使用**。这不是防御性写法，而是那个语义的直接表达——本地模式下它是无关参数，传了也没意义。

而云模式下的缺失被显式拦住：

```ts
if (!conversationUrl) {
  throw new Error("BashService.listOutputs requires a conversation URL on cloud backends");
}
```

错误消息指出了方法名与后端类型，并且旁边有一句注释：**调用方必须等会话完成 hydration 后再在云后端上调用这个方法**。也就是说这是一个时序契约：`conversationUrl` 来自会话对象，而会话要等到运行时起来才有它。

**`SearchOptions` 里的字段名是一段「API 表面考古」。** 类注释的最后一段专门解释了它：

> 关于检索过滤器的名字：agent-server 的 API 用 `command_id__eq`（**不是** `bash_command_id__eq`）——那是 `BashService.search_bash_events` 的 Python 实现声明的参数，也是 typescript-client 的 `BashEventSearchOptions` 暴露的东西。

这段注释的存在说明曾经有人在 `bash_command_id__eq` 上踩过坑。而注释给出的两个依据（Python 实现的声明、TS 客户端的暴露）说明这个命名不是任意的，而是从服务端契约一路传下来的——**前端没有权力「改得更自然」**。

**`kind__eq` 的类型是 `'BashCommand' | 'BashOutput'`，而检索只要 `BashOutput`。** `listOutputs` 把它固定成 `'BashOutput'`。但类型里保留 `BashCommand` 说明同一个端点也能列命令事件——`SearchOptions` 是一个**共享的检索形状**，不是为 `listOutputs` 定制的。

**`isBashOutput` 这个守卫在循环里逐条调用，而不是信任 `kind__eq` 过滤。** 也就是说即便查询已经要了 `kind__eq: 'BashOutput'`，代码仍然在客户端再筛一遍。

这一条值得注意：它意味着**「查询条件」与「结果内容」被当成两件事**。如果服务端在 `kind__eq` 上实现有偏差（或返回了混合页），客户端仍然只收 `BashOutput`。代价是多一层判断，收益是「拼接 `stdout`/`stderr`」这个下游动作不会拿到异类事件而产出乱码。

**`sort_order: 'TIMESTAMP'`（升序）是让「直接拼接」成立的前提。** docstring 说「按命令发出的顺序返回事件，于是调用方可以直接拼接」。而排序是**由服务端做的**（作为查询参数），不是客户端排序。这解释了为什么 `SearchOptions` 要暴露 `sort_order` 且有两个值（`'TIMESTAMP'` 与 `'TIMESTAMP_DESC'`）：不同的调用方需要不同方向，而 `listOutputs` 只需要升序。

**`BashService` 的两个方法一个是 `static async` 公开、一个是 `private static async`。** `searchEvents` 是私有的，只被 `listOutputs` 调用。这个形状说明 `searchEvents` 不是给外部用的通用检索——外部只需要「一个命令的完整输出」这个语义化结果。

**每次检索都新建 `BashClient`。** 与 `f02` 的 `ToolClient`、`g01` 的 `GitClient` 完全一致：`new BashClient(getAgentServerClientOptions(...))`。没有单例、没有缓存，因为客户端选项依赖**当前活跃后端**与**这次调用的会话 URL**，两者都可能在运行中变化。

**没有对「响应不是页形状」做校验，与 `g02` 形成对比。** `g02` 里有两处 `if (!Array.isArray(changes)) throw 'Invalid response from runtime...'`；这里直接 `page.items.forEach(...)`。也就是说如果服务端返回了一个没有 `items` 的对象，这里会抛一个 TypeError，而不是一条有意义的错误消息。

这个不一致（同一批代码里两种风格）说明这类防御是**按踩过的坑逐个加的**，而不是统一的架构约定。`g02` 加是因为运行时的 502 会变成一个空对象；这里没加可能是因为 bash 检索的失败总是以异常形式出现（而不是形状变化）。

**`listOutputs` 的参数里 `sessionApiKey` 是 `string | null | undefined`，而 `conversationUrl` 是 `string | null`。** 前者允许 `undefined`（调用方可能根本没持有它），后者只允许 `null`。这个类型差异反映了两者的来源：会话密钥可以是「还没取到」，而会话 URL 一旦 hydration 完成就一定是字符串或 `null`。

## 关键取舍

**源码事实：** 分页是游标式的（靠 `next_page_id`），上限 20 页只是防无限循环的保险；被截断时静默返回前 20 页，调用方无从知道不完整。

**源码事实：** 本地模式**必须忽略** `conversationUrl`（自动化命令在任何会话之外跑，会话作用域检索找不到它）；云模式**必须提供**它（没有共享的 bash 事件宿主），缺失即抛错并指出时序契约。

**源码事实：** 「是否传 `conversationUrl`」的条件同时包含 `isCloud` 与值存在，于是本地模式下传了也不用——语义的直接表达而非防御。

**源码事实：** 过滤器名 `command_id__eq` 是从服务端 Python 实现与 TS 客户端一路传下来的契约，注释专门记录「不是 `bash_command_id__eq`」。

**源码事实：** 客户端在 `kind__eq` 过滤之外仍用 `isBashOutput` 逐条筛，把「查询条件」与「结果内容」当两件事。

**源码事实：** 升序由服务端 `sort_order` 保证，这是「调用方可以直接拼接输出」这个语义的前提。

**源码事实：** 每次检索都新建客户端，因为选项依赖活跃后端与本次的会话 URL。

**架构推论：** 这一层最值得学的是**「作用域必须跟着语义走」**。同一个 bash 命令的事件，在本地属于「那台 agent-server」，在云上属于「那个会话的运行时」——因为本地只有一台服务器而云上没有共享宿主。而如果按「总是用会话作用域」或「总是用服务器作用域」来统一，就会有一半场景查不到东西，且症状是「空输出」而不是报错。

第二点值得记的是**游标分页 + 保险上限**这个组合的诚实程度：上限的注释写明了它为什么是 2000 而不是别的（「超过 2000 个输出事件不太可能」），也就是承认这是一个基于经验的界限。而它选择静默截断而非报错，说明「拿到大部分输出」比「知道输出被截断」对界面更重要——后者可以通过别的路径（例如事件总数的提示）补，而一个因超长命令而报错的终端是不可用的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/bash-service/bash-service.api.ts` | `BashService.listOutputs` | 一个命令的全量输出：逐页检索并拼接 |
| `src/api/bash-service/bash-service.api.ts` | `BashService.searchEvents` | 私有检索，按云/本地切换查询作用域 |
| `src/api/bash-service/bash-service.api.ts` | `SearchOptions` | 检索形状：kind、command_id、排序、游标、limit |
| `src/api/bash-service/bash-service.api.ts` | `MAX_OUTPUT_PAGES` | 20 页保险上限与「2000 个事件不太可能」的依据 |
| `src/api/bash-service/bash-service.api.ts` | `isBashOutput` | 类型守卫，在查询过滤之外再筛一遍 |
| `src/api/bash-service/bash-service.api.ts` | 类注释的三段说明 | 本地/云作用域的差异与过滤器命名的来历 |

## 参考

- [OpenHands 源码：bash-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/bash-service/bash-service.api.ts)
- [OpenHands 源码：agent-server-git-service.api.ts（同批的云/本地区分）](https://github.com/OpenHands/OpenHands/blob/main/src/api/git-service/agent-server-git-service.api.ts)
- [MDN：CORS 与允许来源](https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS)
- [OpenHands 文档：终端与命令](https://docs.openhands.dev/openhands/usage/interface)

## 与主核 / AI 的关系

游标分页 + 保险上限这套做法与主核里「分批扫描游标」同构：数据库的 `cursor` 也是一页一页取、靠一个不透明的 token 继续，而「最多取 N 批」的保险同样存在（防止一次查询无限拉取）。共同点是**上限的意义是「防死循环」而不是「业务限制」**，因此越界时的行为通常是截断而不是报错。

对 agent 工程的直接价值有三条。其一，**查询作用域要按语义选，不能统一**。同一个资源在两种部署下属于不同层级（服务器 vs 会话），硬统一会有一半场景「查得到但为空」。其二，**下游动作依赖的顺序要由服务端保证**。「支持直接拼接」不是一个实现细节，而是一个接口承诺——它必须由 `sort_order` 这类显式参数实现对，而不是靠客户端事后排序。其三，**过滤器名是契约，不是风格**。`command_id__eq` 与 `bash_command_id__eq` 的差别之所以值得写进注释，是因为它没有可推断性——下一个人只会按「更自然的命名」去猜。
