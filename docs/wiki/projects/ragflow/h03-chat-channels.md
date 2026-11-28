---
project: ragflow
feature: chat-channels
title: 渠道接入与出站
page_type: feature
schema_version: 1
refs:
  - path: internal/channels/bootstrap.go
    symbols: [Runtime, NewRuntime]
  - path: internal/channels/feishu.go
    symbols: [SetMessageHandler, Start, Stop]
  - path: internal/channels/egress.go
    symbols: [RoundTrip]
---

# 渠道接入与出站

## 一句话职责

把「数据库里配了哪些聊天渠道账号」与「进程里实际跑着哪些长连接适配器」这两份状态持续对齐（`Runtime.Reconcile`），并保证每条出站消息只发往经策略校验过的目的地（`channelHTTPTransport.RoundTrip`）。

## 机制是什么

聊天渠道对接（飞书、Discord、Telegram、企业微信、钉钉、LINE、QQ、WhatsApp）有两个各自独立的问题：

**一，生命周期的收敛。** 运维在数据库里改一行 `chat_channel` 配置：启用、禁用、换 app_secret、换域名。进程里跑着的 WebSocket 长连接必须相应启动、停止或重建。这不是一次性启动，而是**持续对账**——`Runtime` 每 10 秒拉一次数据库，把「期望集合」与「运行集合」求差，该停的停、该起的起、指纹变了的重启。

**二，出站的可控性。** 渠道适配器要往各种外部域名发 HTTP/WebSocket。若配置里能填任意 URL，就存在把服务端当作请求跳板的可能（访问内网元数据接口、探测内网端口）。所以出站请求不直接用 `http.Client`，而是走一层自定义 `RoundTripper`：每次请求先解析目标主机、按策略判定是否允许、把 DNS 结果**钉死**成具体 IP 再拨号。

两件事的交汇点是 `SetMessageHandler`——适配器收到入站消息后回调，回调里经过「REST 客户端 → RAG 回答 → 出站发送」的完整链路，而出站发送就走那层受控传输。

## 怎么实现的

**对账的核心是「指纹」而不是「是否启用」。** `desiredChannels` 从数据库取出启用的行，把每行压成 `desiredChannel{channel, credential, fingerprint}`。指纹由 `service.ChatChannelRuntimeFingerprint(channel, credential)` 算出——**它把渠道类型与凭据内容一起哈希**。于是「换了 secret」与「换了渠道类型」都会被识别为变化，从而触发「停旧的、起新的」；只要指纹不变就什么都不做。这比「比较整个配置结构」简单，也比「只看启用位」正确。

**`Reconcile` 的三段结构。**

1. **算差集并清理。** 在锁内遍历 `running`：期望集合里没有它、或指纹不同，就删出 `running`、加进 `toStop`、记入 `removedIDs`。同时遍历 `failed`，条件不同（不存在或指纹变化）就删除。**锁外**才真正停连接与删运行时快照——停连接是慢操作，不能占锁。
2. **同步 WhatsApp 网关。** 这是一个特例：WhatsApp 需要一个独立的网关进程/连接，所以先扫一遍期望集合看有没有 whatsapp 渠道，再调 `syncWhatsAppGateway(ctx, activeWhatsApp)`。失败时按 `channelRetryDelay(attempts)` 排下次重试时间（`gatewayNextRetryAt`），成功则清零。重试窗口内跳过同步，避免每 10 秒都去撞一次失败。
3. **启动缺的。** 遍历期望集合，若已在跑就跳过；若处于失败重试窗口内（同指纹且 `now` 未到 `nextRetryAt`）也跳过；否则 `startChannel`。失败则 `recordStartFailure`（attempts 递增、算出下次重试时间），成功则 `clearStartFailure`。

注意第三段里「在跑」的判断与「失败重试」的判断**各自在锁内取样**，然后才在锁外启动。这允许与其它 goroutine 的注销并发，但启动本身是幂等的（适配器的 `Start` 有守卫）。

**`Run` 的等待时长是动态的。** 每轮 `Reconcile` 之后不固定睡 10 秒，而先取一个基准 `reconcileInterval`，然后在锁内检查 `gatewayNextRetryAt` 与每个 `failed` 条目的 `nextRetryAt`——**任何比基准更早到期的重试时间都会把等待缩短**。于是「退避期间」也能按时醒来重试，而不是等满一个对账周期。退出时 `defer` 里用 `context.WithoutCancel` 加 30 秒超时跑 `stopAll`：取消已经发生，但清理必须还能进行。

**`startChannel` 装配回调是整页最密的一段。** 顺序是：`buildChannel`（按渠道类型分派到具体构造函数）→ `SetMessageHandler` → `ch.Start(ctx)` → 登记进 `running`。

回调内部做六件事：

1. 锁内检查 `stopping`（停机中直接返回 `context.Canceled`），锁内 `messages.Add(1)`，`defer messages.Done()`。**这里用 `WaitGroup` 而不是单纯依赖 ctx**，因为 ctx 取消不会等正在跑的处理完成，而 `stopAll` 需要等。
2. 判断该渠道是否支持流式：`ch.(core.StreamingChannel)` 类型断言且 `StreamEnabled()`。支持就建两个缓冲 1 的 channel（`text` 累积文本、`sent` 结果），起一个 goroutine 跑 `SendStream`，并造一个 `onDelta`。
3. `onDelta` 的丢弃策略值得单独看：先非阻塞试写；写不进就**先读走旧值再写新值**（两段非阻塞）。注释写明了理由——只保留最新的累积快照，渠道不应在平台渲染旧文本时积压。因为传的是累积文本而不是增量，丢中间态是安全的。
4. 调 REST 客户端 `HandleIncomingMessage(..., onDelta)` 拿完整答案。
5. 流式收尾：若 `answer` 非空就再 `onDelta(answer)`（保证最终快照不因前面被合并而丢失），`close(text)`，等 `<-sent` 拿流式错误。流成功就直接返回；流失败但回答非空则记 warning 并**落到单条发送**；流失败且答案为空则返回 nil（模型什么都没产出，没东西可发）。
6. 非流式或流式失败时：用 `context.WithoutCancel(ctx)` 加 30 秒超时构造 `sendCtx` 再 `ch.Send`。注释解释了理由——本轮 ctx 可能已过期，而兜底发送不该被它取消。

**适配器的 `Set/Start/Stop` 是一个统一的生命周期契约。** 以飞书为例：`SetMessageHandler` 在锁内替换 `handler` 字段；`Start` 在锁内检查 `cancel != nil`（**已在跑就直接返回 nil，天然幂等**），否则建 `runCtx` 存下 `cancel` 后起 `go c.run(runCtx)`；`Stop` 在锁内清 `cancel`、清去重表 `seen`、清 worker 表，再在锁外调 `cancel()`。

「锁内置空句柄、锁外执行取消」这个顺序是有意的：`Stop` 必须先把状态改成「没在跑」，避免并发的 `Start` 看到旧的 `cancel` 而误判为已启动。同一模式在各渠道实现里重复出现，是这一层的公共约定。

**出站策略是「解析 → 判定 → 钉 IP」三段。** `channelHTTPTransport.RoundTrip`：

1. `resolveChannelDestination(req.URL, policy, false)` 返回 `(host, resolvedIP, port, err)`。
2. 用 `host + "\x00" + resolvedIP` 作缓存键取 `http.RoundTripper`（首次调用时由 `utility.PinnedHTTPClient(host, resolvedIP, timeout)` 创建），缓存进 `transports` map。
3. 用这个「已钉住 IP」的 transport 发请求。

**钉 IP 是这套防护的核心**：策略判定基于解析出的 IP，而实际拨号用的是同一个 IP，中间不存在「判定时解析到公网、拨号时解析到内网」的时间窗（DNS rebinding）。`PinnedHTTPClient` 承担后者。

**WebSocket 走 `dialSafeChannelWebSocket`，同一套判定但多一道自检。** 它先解析出允许的 `(host, resolvedIP, port)`，然后重写 `websocket.Dialer`：清掉 `Proxy`、`NetDial`、`NetDialTLSContext`（避免默认拨号器绕开策略），在 `NetDialContext` 里**先校验传入的 address 与之前判定的 host/port 一致**，不一致直接报 `unvalidated channel WebSocket destination`，一致才用 `resolvedIP` 拨。这是「即使调用链某处改了 URL，也无法把连接导向别的目的地」的兜底。

**判定规则有四档，优先级从高到低。** `resolveChannelDestination` 里：

1. URL 形状校验：必须绝对、有 host、无 userinfo、无 fragment。
2. scheme 校验：WebSocket 只收 `ws`/`wss`，HTTP 只收 `http`/`https`。
3. **本地回环特例**：`resolveAllowedLocalChannelDestination` 允许「主机是回环地址、路径前缀精确匹配、scheme 与端口都与配置的本地基址一致」的目标。这里还额外处理了 `host.docker.internal` 指向 `127.0.0.1:3005` 的容器场景，并要求解析结果全是私有地址。
4. **运维白名单**：`RAGFLOW_CHANNEL_ALLOWED_HOSTS` 环境变量，逗号分隔、**不接受通配符**，端口可选（不写端口时只有标准端口才放行）。命中的主机要求「解析结果要么是私有地址，要么 scheme 是 TLS」——也就是说**明文 HTTP 只能连内网**。
5. **内置信任域**：`policy.exactHosts` 与 `policy.hostSuffixes`（后者匹配「等于」或「以 `.后缀` 结尾」），且必须 TLS。之后还要过 `common.AssertURLSafe`（另一层内网地址拦截）。

**非标端口单独设卡。** 即使主机被白名单允许，端口若不是 80/443（对应 scheme）也不放行——除非运维在白名单里显式写了该端口。这条把「允许这个域名」与「允许这个域名的任意端口」区分开。

**主机名归一有两处细节。** `normalizeChannelHost` 先小写、去首尾空白、**去掉结尾的点**（`example.com.` 与 `example.com` 等价），若是 IP 就规范化成 `ip.String()`，否则走 IDNA 的 `ToASCII`——所以 Unicode 域名会先转 punycode 再比较，避免同形字绕过白名单。

**本地路径校验防的是路径穿越。** `isSafeLocalChannelPath` 拒绝反斜杠、拒绝编码后的 `/` 与 `\`（`%2f` / `%5c`，大小写都查）、再 `PathUnescape` 后逐段检查 `.` 与 `..`。前缀以 `/` 结尾时按前缀匹配，否则要求完全相等。

## 关键取舍

**源码事实：** 对账用「渠道类型 + 凭据」的指纹判断是否需要重建，而不是逐字段比较或只看启用位。

**源码事实：** `Run` 的等待时长会被最近的网关/渠道重试时间缩短，退避期间不必等满 10 秒。

**源码事实：** 消息回调用 `WaitGroup` 单独计数在途消息，`stopAll` 在 30 秒超时内等它归零，超时只记 warning。

**源码事实：** 流式输出传的是**累积文本**而非增量，`onDelta` 因此可以安全丢弃中间态，只保留最新快照。

**源码事实：** 兜底发送与退出清理都用 `context.WithoutCancel` 加独立超时，避免被已经取消的父 context 波及。

**源码事实：** 出站请求先判定 DNS 结果、再把该 IP 钉进 transport/拨号器；WebSocket 拨号前还自检 address 与判定结果一致。

**源码事实：** 明文 HTTP 只允许私有地址；公网目标必须 TLS；非标端口需在运维白名单里显式声明。

**架构推论：** 「持续对账」而不是「启动时读一次配置」，让运维改数据库即生效，不需要重启进程。代价是每秒都在读表（10 秒一次 `ListActive`），且**配置错误会在运行期反复重试而不是启动失败**——一条凭据写错的渠道只会不断在日志里报错，服务本身照常运行。这是可用性优先的选择。

**架构推论：** 指纹里含凭据意味着「换 secret 会重建连接」，这是必须的（旧连接用旧凭据）；但也意味着**指纹算法变了会让所有渠道重建一遍**。指纹一旦成为持久化的比较基准，它就成了隐式契约。

**架构推论：** 钉 IP 的手法把「策略判定」与「实际拨号」绑成一个不可分割的动作，这是唯一能真正防住 DNS rebinding 的位置。放在应用层的 URL 检查（`AssertURLSafe`）做不到这件事——它检查完 URL 就返回了，真正的解析发生在更下层。

**架构推论：** 渠道适配器的 `Start` 幂等（已有 `cancel` 就返回 nil）与 `Stop` 的「锁内先清句柄」组合，使 `Reconcile` 可以在任意时刻重复调用而不产生重叠连接。这套不变式完全靠实现约定维持，接口签名里没有任何体现——**这是适配器作者必须遵守的隐式契约**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/channels/bootstrap.go` | `Runtime` | 对账器：运行集合、失败集合、在途消息计数 |
| `internal/channels/bootstrap.go` | `NewRuntime` | 空状态构造 |
| `internal/channels/bootstrap.go` | `Runtime.Run` | 周期性对账，等待时长可被重试时间缩短 |
| `internal/channels/bootstrap.go` | `Runtime.Reconcile` | 三段：算差集清理、同步网关、启动缺的 |
| `internal/channels/bootstrap.go` | `recordStartFailure` | 递增尝试次数并算出下次重试时间 |
| `internal/channels/bootstrap.go` | `startChannel` | 装配回调：流式分支、兜底发送、在途计数 |
| `internal/channels/bootstrap.go` | `desiredChannels` | 读启用行并算凭据指纹 |
| `internal/channels/bootstrap.go` | `Runtime.stopAll` | 停机：清快照、停全部、等消息排空 |
| `internal/channels/feishu.go` | `feishuChannel.SetMessageHandler` | 锁内替换入站回调 |
| `internal/channels/feishu.go` | `feishuChannel.Start` | 幂等启动，起长连接 goroutine |
| `internal/channels/feishu.go` | `feishuChannel.Stop` | 锁内清句柄与瞬态表，锁外取消 |
| `internal/channels/egress.go` | `channelHTTPTransport.RoundTrip` | 出站：判定目的地后钉 IP 转发 |
| `internal/channels/egress.go` | `resolveChannelDestination` | 四档目的地判定 |
| `internal/channels/egress.go` | `dialSafeChannelWebSocket` | WebSocket 拨号并自检 address 一致 |
| `internal/channels/egress.go` | `operatorAllowsChannelDestination` | 环境变量白名单（含端口）判定 |
| `internal/channels/egress.go` | `isSafeLocalChannelPath` | 本地路径前缀与穿越检查 |
| `internal/channels/core/channel.go` | `Channel` | 适配器五方法契约 |
| `internal/channels/core/channel.go` | `StreamingChannel` | 流式能力：累积文本入、关闭即结束 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：channels/bootstrap.go](https://github.com/infiniflow/ragflow/blob/main/internal/channels/bootstrap.go)
- [ragflow 源码：channels/egress.go](https://github.com/infiniflow/ragflow/blob/main/internal/channels/egress.go)
- [OWASP：服务端请求伪造（SSRF）防护](https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html)

## 与主核 / AI 的关系

出站策略这一层与主核里「外部表 / 联邦查询只允许连白名单端点」是同一类问题：系统要主动访问外部资源，就必须在发起连接的那一刻做准入，而且**准入判定必须与实际连接绑在一起**。主核在连接器层面做同样的事——若只在配置解析时校验 URL，之后 DNS 变化就能绕过；所以必须钉 IP 或做连接后校验。

「持续对账」与主核的崩溃恢复也对得上：主核在启动时扫表空间把「状态标记为活跃但实际不存在」的残留复位；这里的 `Reconcile` 每 10 秒做一次小规模的同种对账。差别在于主核只在对账失败时拒绝启动，这里对账失败只记日志继续跑——**在线系统的对账是持续修复，离线系统的对账是启动门禁。**

对 agent / RAG 工程的实际含义有四条。其一，运维改渠道配置不需要重启，但要等一个对账周期（最长 10 秒，失败重试时会更快）。其二，凭据写错的渠道会进入退避重试，表现为「日志反复报错但服务正常」——排查渠道不工作时先看日志再看数据库。其三，流式回答在渠道侧失败会自动降级成单条发送，所以用户看到的效果是「没有打字机效果但仍收到完整答案」；只有模型输出为空时才什么都不发。其四，`RAGFLOW_CHANNEL_ALLOWED_HOSTS` 是接入自建渠道端点的唯一开关，且不支持通配符——要接一个自建域名，必须把主机与端口（若非 80/443）都写进去。
