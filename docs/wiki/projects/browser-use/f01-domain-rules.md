---
project: browser-use
feature: domain-rules
title: 域名规则与守卫进程
page_type: feature
schema_version: 1
refs:
  - path: browser_use/domains.py
    symbols: [DomainRules, of, blocked]
  - path: browser_use/domains.py
    symbols: [Guard, pattern_matches, sweep]
---

# 域名规则与守卫进程

## 一句话职责

用一套 0.x 兼容的模式语法声明「浏览器能去哪」，并用三道强度递减的关卡执行它：REPL 辅助函数拒绝不合规 URL、`Guard` 在独立 CDP 连接上直接拦掉主框架文档请求、`sweep()` 在每个单元格之后把仍停在违规页的标签页拨回 `about:blank`。

## 机制是什么

三个类，一条链路：

| 组件 | 位置 | 作用 |
|---|---|---|
| `DomainRules` | 纯数据 + 判定 | 持有 `allowed` / `prohibited` 模式与 `block_ips`，回答「这个 URL 能不能开」 |
| `pattern_matches` | 纯函数 | 一条模式对一个已解析 URL 的匹配 |
| `Guard` | 后台线程 + 自有 CDP 连接 | 浏览器级 `Fetch.enable`，失败主框架文档请求 |
| `sweep` / `disallowed_tabs` | 事后巡检 | 单元格结束后重置仍停在违规页的标签页 |

模式语法沿用 0.x 并按 1.0 收紧，模块 docstring 用一张表列全了六种形态（裸域名、带子域、通配符、带 scheme、glob、带端口）。`about:blank`、新标签页、`data:`、`blob:` 永远放行；没有 host 的 URL 与所有 `file:` URL 一律不放行。

## 怎么实现的

**URL 必须按浏览器的方式解析，否则规则就是在防一个不存在的字符串。** `parse_url` 的 docstring 给了具体理由：按 WHATWG 语义，**去掉首尾的 C0 控制符与空格、删除制表符与换行、把 http(s) URL 里的反斜杠当斜杠**。模块里有一行注释直接给出了攻击样例：`'\x01https://evil\\@ok/'` 会让 `urlsplit` 读出的 host 是 `ok`——也就是说如果不做浏览器式预处理，规则看到的 host 与浏览器真正访问的 host 会不同。

**`canonical_host` 把 host 规范化成「浏览器发出去的那个形态」。** 它依次做：percent-decode、去空白与方括号、小写、对非 ASCII 用 IDNA UTS46 映射、去尾点、能当 IP 就规范化成标准写法、最后一段是数字或 `0x` 前缀时按 WHATWG 当 IPv4（`0x7f.1` → `127.0.0.1`）、仍非 ASCII 则 punycode 编码。docstring 点明这是「像 Chrome」：`faß.de` → `xn--fa-hia.de`。没有这一步，一条 `example.com` 规则挡不住 `examp1e.com` 的同形字（punycode）写法。

**路径也要按浏览器的方式规范化，这是防绕过最细的一处。** `_dot_segments` 把 `.` 与 `..` 段（含 `%2e` 的各种大小写拼写）解析掉。注释给了真实漏法：**路径前缀模式 `https://host/docs` 会匹配 `https://host/docs/../admin`，而浏览器实际加载的是 `/admin`**。`DOUBLE_DOT` 把 `..`、`.%2e`、`%2e.`、`%2e%2e` 四种拼写都算作上跳。尾段的 `..` 会留下一个空串（保证 `/a/..` 规范化成 `/`），这是保留末尾斜杠语义的细节。

**`pattern_matches` 从「字符串前缀」改成「解析后的字段比较」。** docstring 说得很直接：**`'https://example.com'` 不再匹配 `https://example.com.evil.com` 或 `https://example.com@evil.com`**——这正是 0.x 用字符串前缀时的两类经典漏法。带 `://` 的模式按 scheme / host / port / path 四段分别比；不带 `://` 的模式只对 `http`/`https` 生效（0.x 行为），且要单独处理端口。

**1.0 相对 0.x 有三处刻意的收紧，每处都写在注释里。** 其一，`prohibited_domains` 里的裸域名现在**也匹配所有子域**（0.x 会让 `en.example.com` 过）；其二，带 scheme 的禁用模式**没有端口就封所有端口**（0.x 里 `http://evil` 会放过 `http://evil:8080`）；其三，`localhost:8080` 这类带端口的主机**现在能匹配**（0.x 从不匹配）。三条都是「0.x 的规则太松」的具体形态，而不是口味问题。

**`allowed` 与 `prohibited` 同时给时，后者被忽略并告警。** `DomainRules.of` 里直接 `logger.warning`，且 docstring 注明「与 0.x 一致」。这是一条**保留历史语义**而非「更合理」的选择——同时给清单在实践中多半是配置错误，但改成「两者取交」会静默改变老用户的运行结果。

**`block_ips` 在域名判定之前生效。** `blocked()` 的顺序是：空规则/白名单 URL/no-host scheme → 解析 → 无 host 或 `file:` → IP 检查 → `allowed` → `prohibited`。`file:` 的拒绝理由写得特别具体：**Chrome 会从本地磁盘打开 `file://localhost/etc/passwd`（任意 host）**。所以这里不是「file 协议不支持」，而是「file 协议会绕过域名字面意义的整个概念」。

**拒绝理由的文案是给模型看的，不是给日志看的。** `blocked()` 返回单行原因，包含具体 host 与规则清单（超过 10 条就截断成 `... (N patterns)`）。`_scope()` 会根据当前配置生成三种不同的「范围描述」——只在 `allowed` 时说 `allowed_domains (...)`，只在 `prohibited` 时说 `sites outside prohibited_domains (...)`，只有 `block_ips` 时说 `hosts that are not IP addresses`。这些文案会进模型上下文，所以措辞必须准确。

**`prompt()` 的措辞按「浏览器到底拦不拦」分叉，这是一个防止过度承诺的设计。** 参数 `browser_blocks` 为假时（例如 `Browser('chrome')` 没有 Guard），文案只能说「每个步骤后标签页会被拨回 `about:blank`」，不能说「浏览器会拦住」。docstring 给了理由：**这条话不能声称浏览器拦住了只有事后重置才能兜住的东西**。代码里还有一处更细的分叉：设了 `allowed` 时说「其他页面被重置」，没设 `allowed` 时说「打开其中一个的标签页被重置」——注释记录了修改动机：早先那句 `Other navigation is blocked` 会被模型读成「所有导航都被拦」，而实际上只有清单里的域名（或 IP URL）是禁区。

**`Guard` 是这三道里最强的一道，因为它拦在请求层。** 它在**自有**的 CDP 连接上做两件事：`Target.setDiscoverTargets` 订阅标签页生命周期，以及 `Fetch.enable` 只针对 `resourceType: 'Document'` 且 `requestStage: 'Request'` 的请求。于是它覆盖 docstring 列出的一整串绕过路径：链接点击、重定向、`target=_blank`、`window.open`、meta refresh、JS 改 location、`Target.createTarget(url=...)`。被拦下的请求根本不出浏览器（`net::ERR_BLOCKED_BY_CLIENT`）。

**「自有连接」本身是安全属性。** docstring 明确：**因为连的是它自己的 CDP 连接，agent 通过 daemon 发的 `cdp('Fetch.disable')` 关不掉它**。也就是说这道关卡的关闭权不在被约束方手里。

**Guard 承认两类它看不见的路径，并各自给了兜底。** 一是**不经过它所见请求就 commit 的页面**（推测规则预取、预渲染——Chrome 在 Fetch 不拦截的上下文里抓；以及往返缓存）。二是**连接断开期间**（Chrome 停止拦截直到重连）。兜底都是同一个动作：一旦发现某个标签页的 URL 变成违规的，**立刻把它导航到 `about:blank`**；断连则由每单元格后的 `sweep()` 兜。

**`_handle` 的四类消息各对应一个判定点。** `Target.targetCreated` / `targetInfoChanged` 更新 `pages`（target id → 最近显示的 URL），并判断「是否真的变了」：新建标签页、或 URL 变化、或**这是连接时就已经开着的标签页且已完成 discover**（`changed = url != before if before is not None else self._discovered`）。`Fetch.requestPaused` 里有一行关键判断：**`frame in self.pages` 才检查，于是 iframe 全部放行**。`Target.targetDestroyed` 清掉记录。

**「静默重置」区分了错误页与真实违规页。** `_reset` 里的 `silent` 参数在两种情况为真：标签页**创建时就带着违规 URL**（此时它还没加载任何东西，它的请求若有也会被拦并上报），以及**这是本 Guard 自己失败某个请求后留下的错误页**（`_failed` 里记了 `(frame, url)`）。后者避免「拦一次上报两次」——错误页的 URL 也是那个违规 URL，但不是一次新的违规。

**上报量的控制也是设计的一部分。** `take()` 取走自上次调用以来的通知与拦截计数，**去重后最多保留 5 条**（`NOTICES_PER_CELL`），多出的压成一条 `[... N more blocked navigations]`。这让模型不会被一页几十个重定向刷屏，同时仍知道「还有更多」。`_failed` 字典本身也有 100 条上限，超了就从最早开始删。

**启动失败必须「什么都不留」。** `start()` 等一个 `_ready` 事件最多 15 秒（`GUARD_START_S`，注释说明这段覆盖 connect + `setDiscoverTargets` + `Fetch.enable`），超时或出错就 `close()` 再抛 `RuntimeError`，消息包含原因并声明 `nothing was left running`。这是一个正确的资源语义：要么 Guard 真的在工作，要么规则根本没被启用——不允许「以为有保护其实没有」的中间态。

**重连是自动的，且有指数退避。** `_run` 的循环在连接丢失后按 0.5s → 1s → 2s → 4s → 5s 上限重试。注释说明首次失败由 `start()` 上报、后续失败只 debug 日志。重连后 `_discovered = False` 直到 `setDiscoverTargets` 应答——因为**它上报的那些 target 是已经开着的**，不能当成新变化处理（那会误判为违规跳转）。

**`_connect` 自己处理握手期消息。** 它等两个命令的应答，期间收到的非应答消息直接交给 `_handle`。这个写法保证「在 `Fetch.enable` 生效之前到达的事件不会被丢掉」。`wss:` 才传 `ssl` 上下文，注释说明理由是 `ws://` URL 会拒绝 `ssl` 参数。

**`sweep` 的两条豁免都需要解释。** `keep` 是「浏览器刚启动时就显示的那个违规 URL 的标签页→URL 映射」，只有当它**仍然精确显示那个 URL** 时才放过——注释说明场景：`Browser('chrome')` 接用户自己的浏览器，用户本来就有违规页开着，不该被拨走，但一旦它导航到别处就失去豁免。`only` 是「agent 自己的标签页集合」，注释说明 `Browser('chrome')` 下用户自己开的标签页永不被碰，且会**传递地扩展**到这些标签页打开的弹窗（`openerId in only`）。

**`sweep` 的失败不是忽略而是上报。** 重置失败时它产生一条不同的通知：`Tab ... shows it and could not be reset (...); close it`。这条文案设计成**给模型一个可行动的指令**，因为此时唯一正确的补救是关掉那个标签页。

## 关键取舍

**源码事实：** 模块 docstring 自称 **「a guard for an aligned model, not a sandbox」**，并逐条列出边界：子资源（fetch、图片、iframe）不过滤，REPL 自己的 Python 网络访问不是浏览器。这把保护范围钉死在「主框架文档导航」。

**源码事实：** URL 与 host 的解析按 WHATWG/UTS46 复刻浏览器行为，包含 C0/空白剥离、反斜杠归一、`.%2e` 点段解析、punycode、数字形态 IPv4。这些都不是学究气的细节，每条都对应一个实际绕过。

**源码事实：** 1.0 相对 0.x 的三处收紧（禁用清单裸域名管子域、无端口禁用模式封全端口、带端口主机可匹配）都是改「太松」而不是改风格。

**源码事实：** `allowed` 与 `prohibited` 同时给时忽略后者并告警，是保留 0.x 语义的刻意选择。

**源码事实：** Guard 用自有 CDP 连接，所以 agent 自己发的 `Fetch.disable` 关不掉它。关闭权不在被约束方手里。

**源码事实：** Guard 承认两类盲区（不经过它所见请求的 commit、连接断开期），兜底都是「发现违规 URL 就立刻拨到 `about:blank`」，后者再由每单元格的 `sweep` 兜一层。

**源码事实：** 启动要么成功要么不留任何东西；重连后 `_discovered = False`，避免把已存在的标签页误判成新跳转。

**源码事实：** 每次 `take()` 的通知去重且截断到 5 条，多余的压成一行计数。上报量本身是被管理的。

**架构推论：** 三道关卡的分工是**按「能多早知道」排的**：REPL 辅助函数在动作发出前就知道（最便宜、最可解释）；Guard 在请求层知道（最强、能覆盖点击与重定向，但看不见预渲染）；`sweep` 在事后知道（兜底，只能补救）。任何单一关卡都不够——只有辅助函数挡不住模型直接发 CDP；只有 Guard 挡不住预渲染与断连期；只有 sweep 会先让违规页真的加载一次。而三者都不试图覆盖子资源，因为那会把一个「导航边界」变成「网络防火墙」，代价与误报都不可接受。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/domains.py` | `DomainRules` | 规则数据与 `blocked()` 判定，拒绝理由文案给模型 |
| `browser_use/domains.py` | `DomainRules.of` | 从 `Browser()` 选项校验构造，两清单冲突时告警 |
| `browser_use/domains.py` | `DomainRules.prompt` | 按「浏览器是否真的拦」分叉的系统提示行 |
| `browser_use/domains.py` | `pattern_matches` | 解析后字段比较的模式匹配，含 1.0 的三处收紧 |
| `browser_use/domains.py` | `parse_url` / `canonical_host` | WHATWG/UTS46 复刻的 URL 与 host 规范化 |
| `browser_use/domains.py` | `_dot_segments` | `.`/`..`/`%2e` 点段解析，防前缀模式被绕 |
| `browser_use/domains.py` | `Guard` | 自有 CDP 连接上的 `Fetch.enable`，拦主框架文档请求 |
| `browser_use/domains.py` | `Guard._handle` | 四类消息的判定点，含「iframe 放行」 |
| `browser_use/domains.py` | `Guard._reset` | 拨到 `about:blank`，区分静默与真实违规 |
| `browser_use/domains.py` | `sweep` / `disallowed_tabs` | 单元格后巡检重置，含 `keep` 与 `only` 两条豁免 |
| `browser_use/domains.py` | `ALWAYS_ALLOWED` / `NO_HOST_ALLOWED` | 永远放行的新标签页与 `data:`/`blob:` |

## 参考

- [browser-use 源码：domains.py](https://github.com/browser-use/browser-use/blob/main/browser_use/domains.py)
- [browser-use 文档：域名限制](https://docs.browser-use.com/customize/browser/all-parameters)
- [WHATWG URL 标准：host 解析](https://url.spec.whatwg.org/#host-parsing)
- [Chrome DevTools Protocol：Fetch 域](https://chromedevtools.github.io/devtools-protocol/tot/Fetch/)

## 与主核 / AI 的关系

这一层与主核里的「连接级访问控制」同类：都在外部资源访问发生**之前**做判断，而不是事后审计。主核要限制外部表能读哪些路径、网络复制能连哪些主机；这里要限制浏览器能导航到哪些域。共同点是**判定必须用与执行方相同的规范化规则**——如果主核按字符串比路径而文件系统做符号链接解析，检查就会被绕过；这里同理，如果按原始字符串比 host 而浏览器做 punycode 与数字 IPv4 解析，规则会被同形字与 `0x7f.1` 绕过。

对 agent 工程的直接价值有三条。其一，**导航限制要在请求层做，不能只在工具层做**。工具层的检查可以被「模型直接发 CDP」绕过，也可以被重定向、`window.open`、meta refresh 绕过。请求层的 `Fetch.enable` 是唯一能覆盖全部这些路径的位置。其二，**要如实声明覆盖范围**。「不限制子资源」是一个真实且合理的选择，但必须在文档与提示词里说清——否则使用者会以为这是网络隔离。其三，**「保护不存在」比「保护较弱」更危险**。`start()` 超时就抛错并声明什么都没留下，而不是降级成「无 Guard 运行」，这个选择避免了最糟的状态：用户以为有边界，实际没有。
