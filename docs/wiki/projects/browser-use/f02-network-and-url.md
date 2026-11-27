---
project: browser-use
feature: network-and-url
title: URL 规范化与 TLS 请求
page_type: feature
schema_version: 1
refs:
  - path: browser_use/domains.py
    symbols: [Url, canonical_host, parse_url]
  - path: browser_use/utils.py
    symbols: [tls_context, install_tls_opener, urlopen_keyed]
---

# URL 规范化与 TLS 请求

## 一句话职责

给「解析一个 URL」和「发一个 HTTPS 请求」这两件散落在各处的小事各一个统一的实现：URL 侧按浏览器语义（WHATWG host 解析、UTS46、点段解析）产出可比较的字段元组，请求侧给包内自发的 HTTPS 一个「信任 OS 所信任」的 TLS 上下文，并让带密钥的请求永不跟随重定向。

## 机制是什么

两块内容，边界清楚：

- **URL 规范化**在 `domains.py`：`Url` 是一个四字段 `NamedTuple`（`scheme`、`host`、`port`、`path`），`parse_url` 与 `canonical_host` 负责从任意用户/页面字符串产出它。
- **TLS 与 opener** 在 `utils.py`：`new_tls_context`（每线程一个）、`tls_context`（线程本地缓存）、`TLSHandler`（urllib 的 HTTPS handler）、`install_tls_opener`（把 `urlopen` 全局改道）、`urlopen_keyed`（不跟随重定向的专用 opener）。

`Url` 的 `port` 字段刻意是**字符串**而非整数，且空串表示「URL 里没写端口」——这个区分是匹配逻辑需要的，见下文。

## 怎么实现的

**`Url` 用四字段而不是「一个 host 字符串」。** 匹配一条域名规则要看 scheme、host、显式端口、路径四段，把它们先切出来，`pattern_matches` 才能逐段比较而不是做字符串前缀判断。`path` 字段存的是 `path + '?query'`，所以路径模式天然也能约束查询串。

**`port` 为空串与「默认端口」是两件不同的事。** 这是匹配语义的关键：`u.port` 空表示 URL 里没写端口，`DEFAULT_PORT` 才给出该 scheme 的默认值。`pattern_matches` 里两处用法不同——带端口的模式用 `u.port or DEFAULT_PORT.get(u.scheme)` 比较（所以 `host:443` 能匹配无端口的 https URL），而「禁用模式无端口则封所有端口」那条用的是裸 `u.port`（所以 `http://evil` 能封住 `http://evil:8080`，却不会因为默认端口而误封 `http://evil:80` 这条明确写法之外的任何东西）。

**`parse_url` 的三步预处理都在复刻浏览器行为。** 先 `strip(C0_AND_SPACE)` 去掉首尾 C0 控制符与空格，再 `re.sub(r'[\t\n\r]', '', ...)` 删掉制表符与换行，然后**仅对 http(s) URL 把反斜杠替换成斜杠**。第二步的理由写在模块注释里：`\x01https://evil\@ok/` 这种带前导控制符与反斜杠的串，`urlsplit` 会把它读成 host `ok`，而浏览器会去 `evil`。第三步限定在 http(s) 是因为反斜杠在别的 scheme 里不是分隔符。

**`port` 的解析要兜住 `ValueError`。** `urlsplit` 遇到非法端口（`host:abc`）会在访问 `.port` 时抛错，所以 `parse_url` 在外面包了一层 try 并返回 `None`。`None` 的语义统一为「这个 URL 没有可用 host」，与 `urlsplit` 没解出 hostname 的情形合并处理。

**`canonical_host` 的五步顺序是有依赖的。** 先 percent-decode（因为 host 里的 `%2e` 也是点）、去空白与方括号、小写；再对非 ASCII 做 UTS46 映射（`std3_rules=False`、`transitional=False`，与 Chrome 一致的 IDNA2008）；去尾点；然后**先试 `ipaddress`，再试 WHATWG 的数字形态 IPv4**，最后才是 punycode 编码。

这两条 IP 处理各有用途：`ipaddress.ip_address` 覆盖标准写法（含 IPv6），而「最后一段是纯数字或 `0x` 开头」那条覆盖 WHATWG 的 legacy 解析（`0x7f.1` → `127.0.0.1`），这是浏览器真的会连过去的写法，也是 `block_ip_addresses` 必须能识别的形态。

**所有 IDNA 相关操作都 `suppress` 了异常而不是抛出。** 三个 `contextlib.suppress(idna.IDNAError, UnicodeError)` 意味着一个无法规范化的 host 会以**当前形态**返回。这是一个正确的选择：匹配失败会拒绝访问（fail closed），而抛异常会让「判断一个 URL 是否合规」这个操作本身失败，从而在更高层变成崩溃或跳过检查。

**`_glob` 让通配符与 IDNA 共存。** 它把模式按标签切开，**含 `*` 的标签只做小写、不含的标签走 `canonical_host`**。所以 `*.bücher.de` 能正确变成 `*.xn--bcher-kva.de`，而 `*` 本身不会被送去 punycode 编码。

**TLS 上下文的存在理由是「Windows 上的一类具体失败」。** `new_tls_context` 的 docstring 把事故写全了：Python 的默认上下文**只读 Windows 证书库里已有的根**，而一台全新的 Windows 主机只有在 OS 验证器主动索取时才会去抓缺失的根（`api.browser-use.com` 背后的 Starfield 根就是这种）。于是 urllib 报 `CERTIFICATE_VERIFY_FAILED`，而 Edge、`curl.exe`、以及走 truststore 的模型调用都正常工作。

解法是优先用 `truststore` 走 OS 验证器（Windows CryptoAPI / macOS Security / Linux 的 OpenSSL system store），同时仍然尊重 `SSL_CERT_FILE` 与 `SSL_CERT_DIR`（代理的 CA 要当作额外根加载）。truststore 不可用时退回 Python 默认上下文 + certifi 的根。这套「优先 OS、退回 certifi」的顺序说明了一件事：**证书信任的正确答案取决于部署环境，硬编码一份根列表在受管企业网里必然出错**。

**`tls_context` 是线程本地的，理由是一个并发 bug。** docstring 写得很具体：**truststore 在整个握手期间持有上下文的锁，所以若所有线程共用一个上下文，一次卡住的握手（例如向被防火墙拦掉的 PostHog 发遥测）会拖住其它线程的 HTTPS，连网关检查都一起等，直到它超时**。线程本地化把这个全局串行点拆掉了。代价是每个线程各建一份上下文（加载耗时几十毫秒），所以有下面的延迟构造。

**加载 TLS 上下文的时间被显式当作设计约束。** `TLSHandler` 的 docstring：**上下文在第一次 HTTPS 请求时才建，因为加载要几十毫秒，对每次 CLI 调用来说太长**。`_TLSOpener` 更是把「什么时候构建 opener」也当成语义问题：**在第一次 `urlopen` 时才 `build_opener`**，因为 urllib 会在那时读代理环境变量——而 `.env` 是在 browser-harness 被 import 时才读的，**在安装时就构建 opener 会导致 `.env` 里的 `HTTPS_PROXY` 被忽略**。

**`install_tls_opener` 有一条明确的「库不该改变宿主」纪律。** docstring：只在**本包自己运行的进程**里调用（CLI、MCP 服务器、REPL 工作进程），**绝不在 import 时调用——一个库不该为一个 import 它的程序改掉 urllib**。这条把「全局副作用」限制在「我们拥有这个进程」的前提下。

**带密钥的请求永不跟随重定向，理由是 header 会被复制。** `urlopen_keyed` 用 `_keyed_opener`（装了 `_NoRedirect` 的 opener），docstring 说明了攻击面：**urllib 会把每个 header 复制到新 URL，所以一次重定向到另一个源就会把密钥交出去**。`_NoRedirect` 的实现就是让 `redirect_request` 返回 `None`，于是 urllib 把 3xx 当成 `HTTPError` 抛出——docstring 特意指出这与 httpx 对模型调用的行为一致，所以调用方的错误处理逻辑可以统一。

**`raise_fd_limit` 的存在理由是一组连锁症状。** docstring：macOS 起始软限制是 256，而 100 个并发 Agent 在一个进程里约用 300 个 fd；于是 `connect` 与 `bind` 报 `EMFILE`、DNS 也失败（一次模型调用最终显示为 `Connection error.`）。它只做一件事：把软限制抬到硬限制与 `FD_LIMIT`（10240）的较小值，**从不降低**，在 Windows 上是 no-op（`import resource` 失败即返回）。注释里那句「约 3 fd / Agent（模型连接、REPL socket、worker 管道）」把这笔账算清了。

**`file_names` 与 `name_download` 解决的是同一个问题的两面：文件名冲突。** `file_names` 是一个生成器，按 `report.csv`、`report (1).csv` 的顺序产出候选，且**长名字会被缩短而不是丢弃**（按 255 字节上限截断 stem，后缀超 32 字节则判定它「不是扩展名」而并入 stem）。

`name_download` 的注释记录了两次真实事故：一是 `Browser.setDownloadBehavior(allow)` 下 Chrome 会把第二个 `invoice.txt` 直接覆盖第一个；二是**仅仅用 `exists()` 检查会让两个下载同时占一个名字，第二个覆盖第一个**。所以实现改成两级原子占位：先试 `os.link`（目标存在就失败，天然不覆盖），在不支持硬链接的文件系统上退回 `O_EXCL` 建空文件占位再 `os.replace`。这两条路径都保证「绝不覆盖」。

**`utils.py` 末尾那段是兼容层的一个接入点。** 它在非类型检查时把 `__getattr__` 设成 `removed_names(__name__)`，于是 0.x 时代从这个模块 import 的已移除名字会抛出指向 1.0 答案的错误。这与 `legacy.py` 是同一套机制，这里只是把它挂到 `utils` 这个模块上。

## 关键取舍

**源码事实：** `Url.port` 是字符串且空串表示「没写端口」，`DEFAULT_PORT` 单独提供默认值。匹配逻辑依赖这个区分来表达「带端口的模式」与「无端口即封全端口」两种语义。

**源码事实：** `parse_url` 先做三项浏览器式预处理（C0/空白剥离、删制表符换行、http(s) 的反斜杠归一），再做 `urlsplit`。每一项都对应一个让规则看到的 host 与浏览器真实访问的 host 不同的绕过。

**源码事实：** `canonical_host` 先试标准 IP，再试 WHATWG 的数字形态 IPv4，最后才 punycode。IDNA 失败被静默抑制，返回当前形态（fail closed）。

**源码事实：** TLS 上下文优先 `truststore`（OS 验证器），并额外加载 `SSL_CERT_FILE`/`SSL_CERT_DIR`；不可用时退回默认 + certifi。原因是一台干净的 Windows 主机拿不到缺失的根。

**源码事实：** TLS 上下文是**线程本地**的，因为 truststore 在握手期间持锁，共用会让一次卡住的握手阻塞全部 HTTPS。

**源码事实：** 上下文的构造被推迟到第一次 HTTPS 请求；opener 的构造被推迟到第一次 `urlopen`，以便读到那时才从 `.env` 载入的代理变量。

**源码事实：** `install_tls_opener` 只在拥有进程的场景（CLI / MCP / worker）调用，绝不在 import 时调用。

**源码事实：** 带 API key 的请求走不跟随重定向的 opener，因为 urllib 会把 header 复制到重定向目标。

**源码事实：** 文件去重靠 `os.link` 或 `O_EXCL` 做原子占位，因为「先检查再创建」会让两个并发下载互相覆盖。

**架构推论：** 这一层看起来在写工具函数，实际在处理**三件与环境耦合的判断题**：浏览器的 URL 语义是什么、这台机器的证书信任从哪来、这个进程该不该改全局状态。三者的共同点是「没有一个跨平台跨部署的正确答案」，所以代码里充满了条件分支与注释解释的失败史——Windows 的根证书、macOS 的 fd 限制、`.env` 的加载时机、不支持硬链接的文件系统。这类代码的价值不在算法，而在**把踩过的坑固化成注释**，否则下一个维护者会以同样的理由把它简化掉。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/domains.py` | `Url` | 四字段 URL 表示：scheme、host、显式端口、path+query |
| `browser_use/domains.py` | `parse_url` | 浏览器语义预处理后解析，无 host 或非法端口返回 `None` |
| `browser_use/domains.py` | `canonical_host` | UTS46/punycode/IP 规范化，IDNA 失败静默保留原形态 |
| `browser_use/domains.py` | `_dot_segments` | 点段解析，防路径前缀模式被 `..` 绕过 |
| `browser_use/utils.py` | `new_tls_context` | 优先 OS 验证器的 TLS 上下文，含代理 CA 加载 |
| `browser_use/utils.py` | `tls_context` | 线程本地缓存，拆掉 truststore 的握手锁串行点 |
| `browser_use/utils.py` | `TLSHandler` | 首次 HTTPS 请求时才建上下文的 urllib handler |
| `browser_use/utils.py` | `install_tls_opener` | 把 `urlopen` 改道，只在拥有进程的场景调用 |
| `browser_use/utils.py` | `_TLSOpener` | 首次 `urlopen` 时才建 opener，以读到那时的代理变量 |
| `browser_use/utils.py` | `urlopen_keyed` | 带密钥请求的不跟随重定向 opener |
| `browser_use/utils.py` | `raise_fd_limit` | 抬升 fd 软限制，macOS 上避免 EMFILE 连锁失败 |
| `browser_use/utils.py` | `file_names` / `name_download` | 文件名去重，用 `os.link`/`O_EXCL` 原子占位避免覆盖 |
| `browser_use/utils.py` | `removed_names(__name__)` | 把 0.x 兼容层挂到本模块 |

## 参考

- [browser-use 源码：utils.py](https://github.com/browser-use/browser-use/blob/main/browser_use/utils.py)
- [browser-use 源码：domains.py](https://github.com/browser-use/browser-use/blob/main/browser_use/domains.py)
- [WHATWG URL 标准](https://url.spec.whatwg.org/)
- [truststore 文档](https://truststore.readthedocs.io/)
- [Python 文档：ssl 模块](https://docs.python.org/3/library/ssl.html)

## 与主核 / AI 的关系

这一层与主核里「实现自己的传输层」时的处境一致：协议栈底层的行为差异（证书信任、代理变量、重定向是否保留凭据、fd 上限）会以「某台机器上偶发失败」的形式暴露，而正确的修法通常是按环境分支而不是统一成一个「更简单」的实现。

对 agent 工程的直接价值有三条。其一，**带凭据的请求不该跟随重定向**。这不是风格问题：urllib、部分 HTTP 客户端默认会把 header 复制到新 origin，一次 302 就把密钥交给了别人。正确做法是禁止重定向或使用会剥离凭据的客户端。其二，**别给同一个进程的所有线程共用一个会持锁的 TLS 上下文**。这类问题只在遥测/日志这类「可能卡住」的请求上暴露，症状是「主链路莫名变慢」。其三，**全局副作用要有边界**。`install_tls_opener` 只在「本包拥有这个进程」时调用，这条纪律让一个库不会被自己的方便变成宿主程序的问题。
