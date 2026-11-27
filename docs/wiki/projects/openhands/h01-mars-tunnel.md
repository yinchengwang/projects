---
project: openhands
feature: mars-tunnel
title: Mars 隧道与沙箱代理
page_type: feature
schema_version: 1
refs:
  - path: src/api/mars/mars-tunnel-backend.ts
    symbols: [MarsConnection, MarsAuthState, MarsSession]
  - path: src/api/mars/mars-tunnel-backend.ts
    symbols: [MarsTransport, isValidAgentName, MarsAgentConfig]
---

# Mars 隧道与沙箱代理

## 一句话职责

把 DigitalOcean Managed Agents（MARS）的一个**远端沙箱**变成前端认识的一个普通 `kind: "local"` 后端：凭据留在可信进程、隧道由服务器侧的桥拨通、页面只拿到一个 `http://127.0.0.1:<port>` 的回环地址——于是会话、文件、终端整套功能不必知道「这其实在云上」。

## 机制是什么

这个文件是**渲染侧的视图 + 若干纯函数**，本身不拨隧道。真实的分工写在模块 docstring 里：

> MARS 桥由 `electron/preload-main.cjs` 暴露。一个会话通过**回环上的端口转发隧道**访问。远端是一个普通 agent-server，所以这个后端是 `kind: "local"`。
>
> `window.marsBridge` 在桌面构建里来自 Electron 的 preload，在 Web 构建里来自 Agent Canvas 服务器（在服务器托管它时、首次渲染前安装）。**harness-api 不发 CORS 头，而令牌必须住在一个可信进程里**，所以两者都没有的页面根本无法提供 MARS。调用方应该对 `getMarsBridge()` 做门禁，而不是靠在每次调用上捕获失败。

于是几个类型各自表达一个概念：

| 类型 | 表达什么 |
|---|---|
| `MarsConnection` | 一条**已存储**凭据的非秘密描述（`kind`、`label`、`teamName`、`email`、`expiresAt`、`isExpired`、`isSaved`） |
| `MarsAuthState` | 凭据集合 + 当前活跃 + 是否持久化 + 存储位置 + 是否可用 OAuth |
| `MarsSession` | 一个**会话**（microVM），`status` 是 `SESSION_STATUS_*` 字符串 |
| `MarsAgentConfig` | 一个**智能体定义**（不可变的配置），一个 config 对应多个 session |
| `MarsTransport` | 目前只有 `'tunnel'` |
| `MarsTunnelStatus` | 隧道连接状态：`host`、`remotePort`、`localPort`、`upstreamFailure` |

## 怎么实现的

**凭据类型的第一条约束是「绝不携带令牌」。** `MarsConnection` 的注释只有一句：**已存储凭据的非秘密描述。绝不携带令牌**。字段都是可以安全地显示在界面上的：`id`、`kind`（`oauth` / `pat`）、`label`、`teamName`、可选的 `email`、`expiresAt`、`isExpired`、`isSaved`。

而 `expiresAt` 的注释带了一条硬事实：**ISO 时间戳；OAuth 授权 30 天后过期且无法刷新**。这条事实解释了为什么必须支持 PAT（`MarsCredentialKind = 'oauth' | 'pat'`）：一种会过期的凭据不能是唯一选项。

**`isSaved` 的注释揭示了「保存可能失败」这个第三态。** 原文：**存储拒绝这个令牌时为 `False`；它只在此次运行内有效**。也就是说凭据有三个状态：持久保存、只在本进程有效、以及不存在。前端因此可以告诉用户「重启后可能就要重新登录」——`MarsStorage` 类型正是为这句话存在的。

**`MarsStorage` 把「存在哪里」变成可显示的字段。** 三个 `kind`（`keychain` / `encrypted-file` / `memory`）、一个 `label`（**存储自己的名字，例如 "macOS Keychain"**）、一个 `host`（**持有令牌的机器：运行 Canvas 的那台**）。

`MarsAuthState.storage` 是可选字段，注释说明了兼容性：**较旧的桥不提供它，只报 `isPersistent`**。这是一个典型的向前兼容手法：新字段可选，旧实现至少还有一个布尔能表达「持久还是不持久」。

**`MarsAgentConfig` 与 `MarsSession.agent_kind` 的区别被专门写了一段注释。** 原文：**持久化的智能体定义——用户口中的「一个 agent」。一个 config 有很多 session。不要与 `MarsSession.agent_kind` 混淆，后者只是一个命名沙箱里那个编码 agent 的枚举**。

这条注释的价值在于它纠正了一个极易犯的误读：两个字段都叫 agent，但一个是**用户视角的实体**，一个是**沙箱内部的实现种类**。同一个沙箱可以跑不同的 agent，而 agent 定义在云端是一个不可变对象。

**`agent` 字段的注释说明了「是谁填的」。** **manifest 的 `agent`（例如 `openhands`），已小写。由主进程填入；manifest 读不出来时为 `null`**。

于是 `isOpenHandsConfig` 的判定是 `config.agent === 'openhands'`，并且注释说明了为什么要严格：**没有它的 manifest 是更老的规格、已经启动不了，所以未知的 agent 永远不算**。也就是说这里的选择是**白名单**（只认明确声明的），而不是黑名单（排除已知的其他类型）——因为「读不出来」比「是别的类型」更常见。

**`isValidAgentName` 是 harness-api 的命名规则的本地镜像。** 正则 `^[A-Za-z0-9]([A-Za-z0-9._-]{0,62}[A-Za-z0-9])?$`——首尾必须是字母数字，中间允许 `.`/`_`/`-`，总长 1–64。注释只有一句：**harness-api 的 Agent Config 命名规则**。

在前端做这个校验的理由是**让错误在提交之前出现**，而不是让一次网络往返告诉你名字不合法。而它是「镜像」——规则的真值源在服务端，这里只是提前判定。

**`MarsTransport` 只有一个成员，且注释说了「只有这个」。** `type MarsTransport = 'tunnel';` 上方注释：**一个已连上的会话怎么被访问。今天只有回环隧道**。

把只有一个成员的联合类型留在这里，是为了给将来的第二种传输（例如 AGENTS.md 里记的「公共 ingress 直连，等平台修好后回来」）留位置，而不必改所有读到 `transport` 的地方。

**`MarsSessionPhase` 是一个「把服务端枚举压成界面状态」的映射，且对未知值有明确的兜底。** `getSessionPhase` 把五个 `SESSION_STATUS_*` 收成五个 phase：`ready` / `paused` / `failed` / `ended` / `starting`。而兜底规则写在注释里：

> 状态是服务端加的，不需要客户端发版，所以任何不认识且不是终态的状态都读作 `starting`（过渡态）。

这是一个非常正确的取舍：**服务端会加新状态**，而前端不能因为遇到不认识的状态就显示成错误或结束。默认成「正在启动」既保守（不会误判为可用），又不会让用户看到刺眼的失败。

而 `TERMINAL_SESSION_STATUSES`（`DESTROYING`/`DESTROYED`/`FAILED`）是「回不来」的集合，`getSessionPhase` 在三个具名判断之后用它兜「结束」——注意 `FAILED` 被单独判成 `'failed'` 而**不在** `'ended'` 里，但它在终态集合里。这个重叠是有意的：**它是终态，但界面要单独显示它**。

**`waitForMarsAgentServer` 是这一页最重的函数，它解决的是「刚唤醒的沙箱还没起来」。** 它的 docstring 与三组常量构成一条完整的判定链：

- `AGENT_SERVER_WAIT_MS = 60_000`、`AGENT_SERVER_PROBE_TIMEOUT_MS = 5_000`、`AGENT_SERVER_PROBE_INTERVAL_MS = 2_000`：轮询一个刚打开的隧道，直到后面的 agent-server 应答。
- 注释说明前提：**一个空闲暂停的会话在首次流量上冷唤醒，而沙箱内的 agent-server 还要再多一点时间才应答，所以早期的探测合法地失败**。
- `GUEST_PORT_CLOSED_GRACE_MS = 20_000`：注释同样具体——**一个刚唤醒的沙箱在它的 agent-server 启动期间也关 4002，所以只有一个超过这个窗口的 4002 才意味着永远不会有东西服务那个端口**。

**失败被分成两个具名原因，而不是一个超时。** `MarsAttachFailureReason` 是 `'not-openhands' | 'refused'`，配一个 `MarsAttachError` 类。注释：**一个界面能精确解释的失败，而不是一个笼统的超时**。

三类判据：

| 判据 | 判成什么 |
|---|---|
| close code `4001`（`CLOSE_SESSION_UNAVAILABLE`），或 HTTP `401`/`403` | `refused` —— **DO 为这个令牌或会话拒绝了隧道** |
| close code `4002`（`CLOSE_GUEST_DIAL_FAILED`）持续超过 20 秒宽限 | `not-openhands` —— **沙箱应答了，但 `:8000` 上不是 OpenHands agent-server** |
| 探测本身是「决定性」失败（密钥被拒、版本过旧） | 立即抛出，不等 |

第三条的判据来自 `isDefinitiveLocalBackendError`，注释写明理由：**一个被拒的密钥或太老的服务器不会因为等待而改变**。这条把「等 60 秒」与「立刻失败」分开，是可用性上的必要区分——否则一个配错的密钥要等满一分钟才看到错误。

**4002 的宽限窗口是一个会被重置的时间戳。** 代码里 `guestPortClosedSince ??= now`（首次置位），然后比较 `now - guestPortClosedSince >= GUEST_PORT_CLOSED_GRACE_MS`；而 `else { guestPortClosedSince = null }` 在任何一次「不是 4002」的失败上把它清空。

这个重置是必要的：如果中间出现过别的失败（例如一次网络抖动），就说明「端口一直关着」这个前提不成立，宽限窗口应该重新开始。这是一个容易被漏掉的细节——只置位不复位会让窗口从第一次 4002 起一直累加，从而把「间歇性异常」误判成「永远起不来」。

**`readUpstreamFailure` 是「问桥要失败原因」的胶水，且它自己绝不抛。** 它先 `getMarsBridge()`，没有 sessionId 或没有桥就返回 `null`；然后 `try { return (await bridge.getTunnel(sessionId))?.upstreamFailure ?? null } catch { return null }`。

也就是说**取失败原因这件事本身失败时，不让它影响主判定**——主判定会退回「继续轮询或超时」。这个层次关系是对的：诊断信息是加分项，不能成为新的失败点。

**`getMarsConnectionHost` 处理了「旧桥没有 host 字段」的兼容。** 它先试 `status.host`，没有就用 `buildMarsBackendHost(status.localPort)` 拼 `http://127.0.0.1:<port>`。注释：**一个来自早于 `host` 的桥的状态，仍通过它的 `localPort` 被尊重**。

这是与 `MarsAuthState.storage` 可选同源的兼容策略：**新字段可缺，但旧字段必须足以恢复语义**。

**`MARS_GUEST_API_KEY = ''` 那个空字符串带着一段关键的安全说明。** 注释：

> 沙箱内 agent-server 的会话密钥由沙箱模板设置，不由我们设置。**DigitalOcean 令牌才是授权边界**：拨通隧道需要它。

也就是说这条链上的安全性**不来自沙箱内的密钥**（那个是空串，等于没有）。真正的门在隧道拨号那一步，而那一步发生在持有 DO 令牌的服务器进程里。这解释了为什么「令牌不能到页面」是硬要求：如果它能到页面，这条链就只剩沙箱内那个空密钥了。

**`buildMarsBackendInput` 是「云 → 本地」的最后一跳。** 它返回 `kind: 'local'`、`authMode: 'api-key'`、`apiKey = MARS_GUEST_API_KEY`，外加 `marsSessionId` 与可选的 `marsConfigId`。

两个 `mars*` 字段的存在说明这个后端的「云身份」没有丢——它被记在后端记录里，于是「这个 local 后端其实是一个云会话」这件事可被查询（例如恢复隧道时用它重新拨号）。

**`buildNewSessionName` 与 `getMarsBackendLocalPort` 是两个小工具，各解决一个具体问题。** 前者把 agent 名改成 slug + 6 位随机后缀（注释：**收窄到 MARS 自己生成名字所用的字符集**）；后者从后端 `host` 里解析出回环端口，注释：**为了在恢复时能再要求同一个本地端口**。

后者的判定写得很紧：`url.hostname !== '127.0.0.1'` 就返回 `undefined`（只认这一种回环写法，不认 `localhost`），端口必须 `Number.isInteger(port) && port > 0`，整段包在 `try/catch` 里（一个非 URL 的 `host` 返回 `undefined`）。

**`getMarsErrorMessage` 剥掉 Electron 的 IPC 包装。** 正则 `^Error invoking remote method '[^']+': (?:\w*Error: )?` 把「Error invoking remote method 'mars:openTunnel': Error: 真正的消息」剥成「真正的消息」。注释：**失败桥调用的消息，不含 Electron 在主线进程抛出的错误外面套的那层包装**。

这是一个「错误消息会被展示给用户」的直接后果：不剥掉，用户会看到一串内部方法名。

## 关键取舍

**源码事实：** 远端是普通 agent-server，所以云会话被注册成 `kind: "local"` 后端；页面拿到的只是一个回环端口地址。

**源码事实：** 凭据类型绝不携带令牌；`MarsConnection` 只放可安全展示的描述。`isSaved: false` 表示「只在此次运行内有效」，于是凭据有「持久/会话内/不存在」三态。

**源码事实：** `agent` 字段由主进程填入，判定用白名单（只认 `openhands`），因为「读不出来」比「是别的类型」更常见，而老 manifest 已经启动不了。

**源码事实：** 未知的会话状态一律读作 `starting`（过渡态），因为状态是服务端加的、不需要客户端发版。`FAILED` 是终态但单独显示。

**源码事实：** 失败被分成 `refused`（4001 / 401 / 403）与 `not-openhands`（4002 超过 20 秒宽限）两个具名原因，配一个专用错误类型；「决定性」失败立即抛而不等满 60 秒。

**源码事实：** 4002 宽限窗口在遇到「不是 4002」的失败时重置，避免间歇性异常被误判成「永远起不来」。

**源码事实：** 取上游失败原因是 best-effort，取不到就 `null`，不影响主判定。

**源码事实：** 沙箱内的会话密钥是空串，真正的授权边界是拨隧道所需的 DigitalOcean 令牌——因此令牌绝不能进页面。

**源码事实：** `marsSessionId` / `marsConfigId` 被记在本地后端记录里，让「这个 local 后端其实是云会话」可被查询，用于恢复隧道。

**架构推论：** 这层的核心是把**一个远端资源伪装成本地资源**，而且伪装得很彻底：它不改变调用方的任何行为，只改一个 `host`。好处是会话/文件/终端这些功能一行都不用改（因为它们本来就只认 `host` + `apiKey`）；代价是所有「云与本地不同」的知识都被压在隧道那一侧，而前端只能靠几个不变量（回环 host、空密钥、`mars*` 字段）来推断。

最值得记的一条是**「空密钥是安全的，因为门在别处」**。这是一个反常识但在本架构下正确的判断：把授权边界放在「拨通隧道」这个动作上，比放在每一次请求上更集中、也更难绕过。而它的前提是「令牌在可信进程里」——一旦这个前提破了，整条链就没有边界了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/mars/mars-tunnel-backend.ts` | `MarsConnection` | 凭据的非秘密描述，绝不携带令牌 |
| `src/api/mars/mars-tunnel-backend.ts` | `MarsAuthState` | 凭据集合 + 活跃项 + 持久化 + 存储位置 + OAuth 可用性 |
| `src/api/mars/mars-tunnel-backend.ts` | `MarsSession` / `MarsAgentConfig` | 会话与不可变的智能体定义，两个概念不可混用 |
| `src/api/mars/mars-tunnel-backend.ts` | `isOpenHandsConfig` | 白名单判定，只认 manifest 明确声明的 `openhands` |
| `src/api/mars/mars-tunnel-backend.ts` | `isValidAgentName` | harness-api 命名规则的本地镜像，让错误在提交前出现 |
| `src/api/mars/mars-tunnel-backend.ts` | `MarsTransport` | 今天只有 `'tunnel'`，为将来的第二种传输留位 |
| `src/api/mars/mars-tunnel-backend.ts` | `getSessionPhase` | 服务端枚举 → 界面状态，未知值兜底成 `starting` |
| `src/api/mars/mars-tunnel-backend.ts` | `waitForMarsAgentServer` | 60 秒轮询冷唤醒沙箱，含三档失败判定 |
| `src/api/mars/mars-tunnel-backend.ts` | `MarsAttachError` / `MarsAttachFailureReason` | 把失败分成 `refused` 与 `not-openhands` |
| `src/api/mars/mars-tunnel-backend.ts` | `readUpstreamFailure` | 取上游失败原因，自身绝不抛 |
| `src/api/mars/mars-tunnel-backend.ts` | `getMarsConnectionHost` | 新字段缺失时用 `localPort` 重建 host |
| `src/api/mars/mars-tunnel-backend.ts` | `getMarsBackendLocalPort` | 从后端 host 解析回环端口，供恢复时复用 |
| `src/api/mars/mars-tunnel-backend.ts` | `MARS_GUEST_API_KEY` | 空串密钥 + 「授权边界在隧道拨号」的说明 |
| `src/api/mars/mars-tunnel-backend.ts` | `buildMarsBackendInput` | 云会话 → `kind: "local"` 后端记录 |
| `src/api/mars/mars-tunnel-backend.ts` | `getMarsErrorMessage` | 剥掉 Electron IPC 的错误包装 |

## 参考

- [OpenHands 源码：src/api/mars/mars-tunnel-backend.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/mars/mars-tunnel-backend.ts)
- [DigitalOcean 文档：Managed Agents（原 App Platform Agents）](https://docs.digitalocean.com/products/)
- [OpenHands 文档：DigitalOcean 托管智能体](https://docs.openhands.dev/openhands/usage/cloud/digitalocean)
- [RFC 6455：WebSocket 关闭码](https://datatracker.ietf.org/doc/html/rfc6455#section-7.4)

## 与主核 / AI 的关系

这层与主核里「把远端表空间挂载成本地表」是同构的：外部数据包装器（fdw）也是让调用方以为数据就在本地，而真实位置在一个远端进程。两者的共同点是把差异压缩到**一个连接字符串**（这里是一个回环 URL，主核是一个 fdw 的 server 定义），差异具体在哪里由包装器负责。

对 agent 工程的直接价值有三条。其一，**「冷启动期失败」是一种合法状态，必须与真失败区分**。空转的沙箱会先拒绝连接再起来，把这当成错误会让用户以为会话坏了；而按时间窗口 + 具名原因分开判，才是可用的体验。其二，**授权边界要放在最集中的那个动作上**。这里它放在「拨隧道」，使得后续每一次请求都不需要密钥——这比在每条请求上校验更省、也更难漏。其三，**未知的服务端状态要有过渡态兜底**。服务端会加状态且不需要客户端发版；把不认识的值读成「启动中」，比读成「失败」或「已结束」都安全。
