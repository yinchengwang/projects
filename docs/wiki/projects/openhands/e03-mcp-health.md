---
project: openhands
feature: mcp-health
title: MCP 健康探测与重授权
page_type: feature
schema_version: 1
refs:
  - path: src/api/mcp-health/probe-mcp-server-health.ts
    symbols: [probeMcpServerHealth, interpretMcpTestResponse, reauthorizeMcpServerHealth]
  - path: src/api/mcp-health/mcp-health-store.ts
    symbols: [getMcpHealthSnapshot, beginMcpHealthCheck, resolveMcpHealthCheck]
---

# MCP 健康探测与重授权

## 一句话职责

维护每张 MCP 服务器卡片的「健康状态」：怎么探测、怎么把探测结果判定成 `healthy` / `failed`（以及健康到什么程度）、以及**怎么保证一个慢的旧探测不会覆盖掉一个新探测的结果**。

## 机制是什么

健康状态本身很简单——`checking` / `healthy` / `failed` 三态。真正的问题是**并发**：用户可能连点两次「测试」，或者探测进行中改了配置、删了服务器、又从别的流程种进了一个状态。如果不管这些，就会出现「UI 显示失败但其实第二次探测成功了」或者「探测结果落在了一条已经被改过的服务器上」。

所以这个模块的重心是两件与状态机有关的事：

| 问题 | 手段 |
|---|---|
| 旧探测覆盖新探测 | 单调递增的 `checkId` + 结果提交时校验 |
| 状态过期 | 存储**刻意不持久化**，刷新页面即全部回到 `unchecked` |

再加上一件与判定有关的事：**`healthy` 要区分「只是连上了」与「真的验证过了」**。

## 怎么实现的

**存储是内存态的，而「不持久化」是一条明确的设计决定。** `mcp-health-store.ts` 的模块注释：

> In-memory MCP connection-health store, keyed by `getMcpServerHealthKey(server)`. Mirrors the module-store shape of `#/api/backend-registry/health-store` but is deliberately NOT persisted: a health verdict is only as fresh as its probe, so a page reload resets every server to "unchecked" instead of resurrecting a possibly-stale "healthy".

这条与 `backend-registry/health-store` 形成了有意的对照：那个后端健康存储是持久化的，因为「后端可达性」是个相对稳定的属性；而 MCP 服务器健康是**一次性探测的结论**，持久化它等于把某个瞬间的快照当成当前事实。刷新后回到 `unchecked` 是**更诚实**的表达。

实现是三个模块级变量：

```ts
let healthMap: McpHealthMap = {};
const listeners = new Set<Listener>();
let nextCheckId = 1;
```

`healthMap` 用整体替换而不是原地修改（`commit(next)` 里 `healthMap = next`），这是为配合 `useSyncExternalStore` 这类订阅式读取：只有引用变了，订阅方才认为状态变了。`getMcpHealthSnapshot` 直接返回当前引用，不做拷贝。

**`beginMcpHealthCheck` 同时做两件事：写 `checking` 状态、发一个 id。**

```ts
export function beginMcpHealthCheck(key: string): number {
  const checkId = nextCheckId;
  nextCheckId += 1;
  commit({ ...healthMap, [key]: { status: "checking", checkId } });
  return checkId;
}
```

`nextCheckId` 是模块级的单调计数器，**永不重置**（只有测试专用的 `__resetMcpHealthStoreForTests` 会重置）。这一点很重要：如果 id 会回绕（例如按 key 归零），两次连续探测就可能拿到同一个 id，校验就失效了。

**`resolveMcpHealthCheck` 是整套并发安全的落点。**

```ts
export function resolveMcpHealthCheck(key: string, checkId: number, health: McpServerHealth): void {
  const current = healthMap[key];
  if (current?.status !== "checking" || current.checkId !== checkId) {
    return;
  }
  commit({ ...healthMap, [key]: health });
}
```

两个条件缺一不可，而它们各挡一类：

- **`current?.status !== "checking"`**：说明这个条目在探测期间被别的东西覆盖了（`setMcpServerHealth` 种入、或被 `clearMcpServerHealth` 删除）。此时无论 id 是否匹配都不该落结果——因为当前状态已经不是「这次探测在等」的那个状态。
- **`current.checkId !== checkId`**：说明有条更晚的探测已经开始（它把状态重新置为 `checking` 并换了新 id）。旧探测必须安静退出。

注释把这层意思写全了：

> A slow older probe can never overwrite a newer probe's result, and a probe whose server was edited, deleted, or reseeded mid-flight（entry cleared or replaced）is silently dropped — a stale check can never land a false verdict.

注意「**silently**」。这里刻意不报错、不打日志：探测被作废是正常流程的一部分（用户改配置就会触发），把它当异常只会污染日志。

**`setMcpServerHealth` 是唯一的无条件写入**，注释写明用途：「used to seed health from a pre-save test result」。它与 `resolveMcpHealthCheck` 的差别就是这个模块里「受保护的写入」与「权威写入」的分界：前者要证明自己仍是最新的，后者本身就是最新的事实来源。

`clearMcpServerHealth` 有个提前返回：`if (!(key in healthMap)) return;`——避免为一个不存在的条目触发一次无意义的通知（那会让所有订阅方白白重渲染一次）。

`getMcpHealthSnapshot` 返回的是**订阅式存储的惯用形状**：读快照 + 订阅变更。`subscribeMcpHealth` 返回取消函数，与 `useSyncExternalStore` 的契约一致。

**判定逻辑 `interpretMcpTestResponse` 分两段，第一个分支处理失败。**

```ts
if (!response.ok) {
  const kind =
    (response.error_kind === "connection" || response.error_kind === "unknown") &&
    AUTH_FAILURE_TEXT.test(response.error)
      ? "credentials"
      : response.error_kind;
  return { status: "failed", kind, error: response.error, checkedAt };
}
```

这段是「保守的认证失败嗅探」，模块注释解释了动机：

> Conservative auth-failure sniffing for hosted servers that reject a bad token at the HTTP handshake: the backend can only classify those as `connection`/`unknown`（the MCP connection itself failed）, but the guidance the user needs is the credentials one.

即后端只有 `connection` / `unknown` 这类粗粒度分类，因为对一个托管服务器来说，「401 拒绝」在协议层面就是「连接建立失败」。但用户需要的是「你的令牌不对」这条指引。所以前端在**只有粗分类时**用文本嗅探补一层细分。

正则本身：`/\b(401|403)\b|unauthorized|forbidden|invalid[ _-]?(token|credentials?|api[ _-]?key)/i`。三处设计：

- **`\b(401|403)\b`** 用词边界，避免把 `1401` 或 `4037` 里的数字当成状态码。
- **`invalid[ _-]?(token|…)`** 允许空格、下划线、连字符三种分隔，因为不同服务端的措辞不同（`invalid_token`、`invalid-token`、`invalid token`）。
- **只在 `error_kind` 是 `connection` 或 `unknown` 时才嗅探**——若后端已经给了更精确的 kind（例如 `credentials`），就不该用文本覆盖它。这是一条「不降级已有精度」的守卫。

代价也很清楚：一个返回「403 Forbidden」的**非认证**失败（例如地域封锁）会被误判成凭据问题。注释里的「Conservative」正是对这里保留的余地——它宁可偏向给凭据指引，因为那种情况下用户至少会去检查令牌，而「连接失败」的指引往往让人无从下手。

`checkedAt: Date.now()` 在函数开头取一次，**不在两个分支各取一次**——这样同一个响应的判定只有一个时间戳，与「这次探测发生在何时」对应，而不是「判定完成于何时」。

**第二个分支处理成功，而它的核心是 `verified` 与 `connectivity-only` 的区分。**

```ts
const verified =
  !!validation &&
  response.tools.includes(validation.toolCall.name) &&
  !!response.tool_result &&
  !response.tool_result.is_error;
```

四个条件必须全真：

1. 这个服务器在目录里有凭据验证配方；
2. 那个只读探测工具**出现在服务端广告的工具列表里**；
3. 探测真的产生了 `tool_result`（即它确实被调用了）；
4. 这次调用的结果不是错误。

不够四个就降为 `connectivity-only`。docstring 把原则说得很直白：

> `verified` requires the catalog entry's read-only probe tool to have been advertised AND invoked without error; everything else that connected is `connectivity-only` — truthfully labeled, never silently upgraded.

最后半句「never silently upgraded」是这一页最值得记的一句话。很多实现会在这种地方「乐观地」判成健康——毕竟连接是通的、没有报错。但那样做就把「能连上」与「能用」混为一谈，而这两件事在 MCP 场景下确实可能分开：服务器可以握手成功、列出工具、但在真正调用时因为凭据或权限失败。

这里选择**如实标注为较弱的一档**，让 UI 有机会展示不同的措辞（例如「已连接（未验证凭据）」而不是「正常」）。

顺带注意 `response.tools.length` 被带进结果（`toolCount`），即使只是 `connectivity-only` 也记了工具数量——这让「连上了但有零个工具」这种可疑情形在界面上可见。

**`probeMcpServerHealth` 是最朴素的编排：拿 id、跑探测、落结果。**

```ts
const key = getMcpServerHealthKey(server);
const checkId = beginMcpHealthCheck(key);
let health: McpServerHealth;
try {
  const response = await McpService.testServer(server);
  health = interpretMcpTestResponse(server, response);
} catch (error) {
  health = failedHealth(server, error);
}
resolveMcpHealthCheck(key, checkId, health);
```

注意 `resolveMcpHealthCheck` 在 `try` **之外**——这样无论探测成功、失败还是被降级成 `failedHealth`，结算都走同一行。而 `failedHealth` 兜的是「探测本身抛异常」（网络层失败、客户端建不起来），与「探测返回了 `ok: false`」是两条不同的路径，但最后都产出 `status: "failed"`。

`failedHealth` 里的脱敏值得一提：

```ts
error: redactMcpSecrets(message, server),
```

即使这是一个前端自己捕获的异常，消息里也可能带凭据（例如 HTTP 客户端的错误里含 URL 与查询串）。所以与 e02/e01 一致，**所有回流到界面的文本都要过一遍脱敏**。这构成一条完整的闭环：入站回填（e02）、出口脱敏（e01）、异常文本脱敏（这里）。

它的 `kind` 固定为 `"unknown"`——因为这里没有足够信息做分类，硬猜不如诚实。

**`seedMcpServerHealth` 解决的是「刚保存的服务器不该显示未检查」。** 安装/编辑流程在保存**之前**已经跑过一次测试，那份结果与保存后的配置「same config, moments earlier」。所以直接把那份结果种进卡片，省掉第二次探测：

```ts
export function seedMcpServerHealth(server, response, otherServers) {
  const key = getMcpServerHealthKey(server);
  if (otherServers.some((other) => getMcpServerHealthKey(other) === key)) {
    return;
  }
  setMcpServerHealth(key, interpretMcpTestResponse(server, response));
}
```

那段 `otherServers` 的守卫是本函数最有意思的地方。注释：

> `otherServers` are the servers whose health must NOT be overwritten — callers pass the pre-save list, excluding the server being saved. When a same-shape duplicate exists（health keys exclude secret values, so two installs of one catalog entry can collide until the save suffixes the name）, the seed is skipped and the new card simply starts "unchecked".

拆开来看：健康键 `getMcpServerHealthKey` **刻意不包含密钥值**（否则每次改密码都会换一个卡片身份）。于是两个「同一个目录条目装了两次」的服务器在名字被加后缀之前会撞在同一个键上。若此时无条件种入，第二次安装的健康结果会覆盖第一次的——而第一次那台服务器可能根本没被这次测试覆盖。

所以选择「撞了就跳过种子」，让新卡片停在 `unchecked`。这是**宁可少显示一个状态，也不显示一个错配的状态**。

**`reauthorizeMcpServerHealth` 与探测的差别只有一处，但很关键：它把原始响应返回出去。**

```ts
const response = await McpService.authorizeOAuth(server);
resolveMcpHealthCheck(key, checkId, interpretMcpTestResponse(server, response));
return response;
```

docstring 说明了返回的用途：「Returns the raw response（null on transport error）so the caller can persist a refreshed `oauth_state`」。即 OAuth 授权成功后会带一个更新过的 `oauth_state`，调用方需要把它写回设置——这是一条**副作用**，不能只留一个健康状态。

这个返回值也是它与 `probeMcpServerHealth`（返回 `Promise<void>`）唯一的签名差异。两者共用同一套 `beginMcpHealthCheck` / `interpret` / `resolve`，所以并发保护对授权流程自动同样生效——这是把「状态机」与「探测手段」分开的好处：授权是另一种手段，但状态结算完全一致。

`reauthorizeMcpServerHealth` 在 catch 分支同时做了两件事：结算成 `failedHealth` **并**返回 `null`。调用方拿到 `null` 就知道「这次没拿到可持久化的状态」，不应写回。

## 关键取舍

**源码事实：** 健康存储刻意不持久化，页面刷新后所有服务器回到「未检查」，因为健康结论只与其那次探测同样新鲜。

**源码事实：** `checkId` 是永不重置的单调计数器；`resolveMcpHealthCheck` 只在条目仍是同一次 `checking` 时才落结果，因此慢的旧探测无法覆盖新探测，也无法落在被编辑/删除/重新种入过的条目上，且这种作废是静默的。

**源码事实：** `setMcpServerHealth` 是无条件写入，专门用于从保存前的测试结果种入健康状态；`begin` / `resolve` 则是有保护的配对。

**源码事实：** 认证失败嗅探只在 `error_kind` 为 `connection` 或 `unknown` 时才做文本匹配，不覆盖后端已给出的更精确分类。

**源码事实：** `verified` 要求四个条件全真（有配方、探测工具被广告、有 `tool_result`、结果非错误），否则如实标为 `connectivity-only`，「never silently upgraded」。

**源码事实：** `seedMcpServerHealth` 在已存在同键条目时跳过种入，因为健康键不含密钥值，两次安装同一条目会撞键。

**源码事实：** `reauthorizeMcpServerHealth` 额外返回原始响应，以便调用方持久化刷新后的 `oauth_state`。

**架构推论：** 这个模块把「状态机」与「探测手段」分成了两层，而并发正确性完全落在状态机那一层。于是「连接探测」与「OAuth 重授权」这两种完全不同的手段自动共享同一套防覆盖保护。这是很值得抄的结构：**当多个手段都要写入同一个状态格位时，把格位的写入协议做成唯一入口**。

**架构推论：** 「不持久化」是一个容易被当成偷懒的决定，而这里它是有理由的。判断一个状态该不该持久化，判据不是「它会变」，而是「它变的速度与它的意义是否匹配」：后端可达性几小时内基本稳定，MCP 服务器健康则取决于一次具体探测。持久化后者等于把一个瞬间的快照当作长期事实，用户会看到一个绿色的卡片而实际早就失效了。

**架构推论：** `verified` 与 `connectivity-only` 的两档标注，本质上是给「健康」这个概念加了**精度维度**。二值的 healthy/failed 无法表达「连上了但没验证凭据」，而这两种状态对用户的下一步动作有不同含义（前者可以直接用，后者应该先去查凭据）。加一档比加一个状态更轻——`status` 仍是 `healthy`，只是带上了 `verification` 的强弱。

**架构推论：** 一处值得指出的残余风险：认证失败的文本嗅探会误判「因非认证原因返回 403」的情形（例如地域封锁、配额耗尽），把它们都归入 `credentials`。注释用「Conservative」承认了这个偏差方向——它偏向「让用户去检查凭据」而不是「让用户面对无解」。这是一个可以接受的偏差，但它依赖 `error_kind` 的精度：若将来后端能把 401 与 403 分得更细，这个嗅探就该整段删掉。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/mcp-health/probe-mcp-server-health.ts` | `interpretMcpTestResponse` | 探测结果 → 卡片健康状态，含失败分类与 `verified` / `connectivity-only` |
| `src/api/mcp-health/probe-mcp-server-health.ts` | `probeMcpServerHealth` | 非变更式连接探测的编排 |
| `src/api/mcp-health/probe-mcp-server-health.ts` | `reauthorizeMcpServerHealth` | OAuth 重授权，额外返回可持久化的原始响应 |
| `src/api/mcp-health/probe-mcp-server-health.ts` | `seedMcpServerHealth` | 用保存前的结果种入，撞键则跳过 |
| `src/api/mcp-health/probe-mcp-server-health.ts` | `failedHealth` | 探测抛异常时的兜底状态，错误文本脱敏、`kind` 为 `unknown` |
| `src/api/mcp-health/probe-mcp-server-health.ts` | `AUTH_FAILURE_TEXT` | 401/403 与常见认证措辞的保守嗅探正则 |
| `src/api/mcp-health/mcp-health-store.ts` | `getMcpHealthSnapshot` / `subscribeMcpHealth` | 订阅式快照读取 |
| `src/api/mcp-health/mcp-health-store.ts` | `beginMcpHealthCheck` | 置 `checking` 并发放单调 `checkId` |
| `src/api/mcp-health/mcp-health-store.ts` | `resolveMcpHealthCheck` | 带双条件校验的结果提交，挡住过期探测 |
| `src/api/mcp-health/mcp-health-store.ts` | `setMcpServerHealth` | 唯一的无条件写入，用于种子 |
| `src/api/mcp-health/mcp-health-store.ts` | `clearMcpServerHealth` | 回到「未检查」，不存在时不触发通知 |

## 参考

- [OpenHands 源码：probe-mcp-server-health.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/mcp-health/probe-mcp-server-health.ts)
- [OpenHands 源码：mcp-health-store.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/mcp-health/mcp-health-store.ts)
- [React 文档：useSyncExternalStore](https://react.dev/reference/react/useSyncExternalStore)
- [MCP 规范：服务端能力协商](https://modelcontextprotocol.io/specification/2025-06-18/server)
- [OpenHands 文档：MCP](https://docs.openhands.dev/openhands/usage/mcp)

## 与主核 / AI 的关系

「一个状态格位只接受来自当前主人的写入」与主核里的**租约与代次编号**是同一套机制。主核在处理后台任务、checkpointer 或连接池槽位时，都会给一次尝试分配一个代次号（epoch），只有代次号仍然匹配才允许写回结果——否则说明槽位已经被回收或换人。这里 `checkId` 的设计完全同构，连「作废是静默的」这条都一样：过期写入不是错误，是正常流程的必然产物。

「健康状态不持久化」这条与主核里**缓存不落盘**的判据一致：缓存的价值来自「与当前数据一致」，一旦跨越进程生命周期就不再可信。主核宁可重启后冷启动也不读一个可能过期的缓存，这里宁可刷新后全部变「未检查」也不复活一个绿色的「健康」。

「加精度维度而不是加状态」这个手法，在主核里对应的是**指标的分级**（例如复制延迟既报数值也报「未知」）。二值状态无法表达「部分已知」，硬塞一个新状态会破坏所有消费者已有的 `switch`；而在既有状态上挂一个置信/验证字段，消费者可以选择忽略它，也可以按它改写措辞。这是向后兼容地提升表达力的通用做法。

对 AI 工程的实际意义在于：agent 依赖的外部能力（MCP 服务器）大多是**远端的、不稳定的、需要凭据的**，所以「健康」几乎从来不是一个布尔值。这份实现给出的三条经验可以直接用：**健康结论不要持久化**（它只与那次探测同样新鲜）、**并发写入要有代次保护**（否则用户连点两下就会看到错乱状态）、以及**把「连上了」与「验证过了」分开标注**（不要为了好看把弱证据升格成强结论）。最后一条尤其重要——agent 的可靠性很大程度上取决于它对自身依赖的诚实程度。
