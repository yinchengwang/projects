---
project: pydantic-ai
feature: ssrf-guard
title: 网络抓取的地址校验
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_ssrf.py
    symbols: [ResolvedUrl, DownloadTooLargeError]
  - path: pydantic_ai_slim/pydantic_ai/common_tools/web_fetch.py
    symbols: [WebFetchLocalTool, WebFetchResult]
---

# 网络抓取的地址校验

## 一句话职责

模型给出一个 URL 之后，在**发出请求之前**把它解析成 IP 地址并逐个校验：内网与云元数据一律按黑名单拒绝，同一条链上的每一跳都要重查，响应体还有体积上限——这是服务端请求伪造（SSRF）的唯一防线。

## 机制是什么

`safe_download(url, allow_local=False, max_redirects=10, timeout=30, headers=None, allowed_domains=None, blocked_domains=None, max_bytes=None)` 是这套防线的唯一入口（`__all__` 里只导它一个）。它文档化了自己做的八件事：

1. 校验协议（只允许 http/https）。
2. 校验主机名是否在允许/禁止域名表内。
3. 把主机名解析成 IP。
4. 校验没有解析出内网 IP（除非 `allow_local=True`）。
5. **总是**拦云元数据端点。
6. 用解析出的 IP 发请求，`Host` 头设为原主机名。
7. 手动跟随重定向，**每一跳都校验**。
8. 服务端设置的回环到 cookie 时，按原主机名而非解析 IP 隔离。

`ResolvedUrl` 是第 3 步的产物：`resolved_ip`、`hostname`、`port`、`is_https`、`path`。它把「连到哪个 IP」与「对外声称是谁」分成两个字段——这是 HTTPS 场景下能同时做到「连 IP」与「用正确 SNI/证书校验」的前提。

`DownloadTooLargeError` 是响应体超限的错误，`WebFetchLocalTool` 是上层工具，`WebFetchResult` 是它的文本结果。

## 怎么实现的

**域名检查在最前面，且每一跳都做。** 循环体第一步是 `_check_domain(hostname, allowed_domains=..., blocked_domains=...)`，注释给出理由：**先按域名判，让一个被拒的主机无论是哪一跳都被这个理由拒掉**。比较用 `_domain_key` 归一化（忽略大小写、末尾点、IDNA 拼写），因此 `Example.COM.` 与 `example.com` 是同一个键。域名表是精确匹配，不是通配。

**协议校验发生在取出主机名之前。** `extract_host_and_port` 第一行就是 `validate_url_protocol`。这个顺序很重要：畸形 URL 在解构之前就该被拒，否则会把解析失败伪装成别的问题。

**解析出的所有 IP 都要过，不是只看第一个。** `resolve_hostname` 用 `socket.getaddrinfo`（在线程池里跑，避免阻塞事件循环），按 DNS 顺序去重。`validate_and_resolve_url` 对结果里**每一个** IP 做两条检查：云元数据必然拒；内网在 `allow_local` 为假时拒。只有当所有 IP 都通过，才用第一个发请求。这一步挡住的是「DNS 返回多条地址，其中一条是内网」这种绕过。

**云元数据黑名单是硬编码的具体地址。** IPv4 侧包含 `169.254.169.254`（AWS IMDS / GCP / Azure / OCI 等）、`169.254.170.2`（AWS ECS 任务凭据）、`169.254.170.23`（AWS EKS Pod Identity）、`168.63.129.16`（Azure WireServer）、`100.100.100.200`（阿里云）、`192.0.0.192`（Oracle Cloud）、`169.254.42.42`（Scaleway）、`169.254.0.23`（腾讯云）；IPv6 侧有 `fd00:ec2::254`、`fd00:ec2::23`、`fd20:ce::254`。这类地址无法靠网段规则全部覆盖（有些是公网 IP），所以必须逐个列举。

**内网网段表覆盖了 IPv4 的特殊用途段与 IPv6 的过渡形式。** 除常规私有段（`10/8`、`172.16/12`、`192.168/16`、`127/8`、`169.254/16`、`100.64/10` CGNAT）外，还包括测试网段（`192.0.2/24`、`198.51.100/24`、`203.0.113/24`）、基准测试段（`198.18/15`）、组播与保留段，以及 IPv6 的 `::/128`、`::1/128`、`fe80::/10`、`fc00::/7`、`fec0::/10` 等。

**IPv6 过渡形式是最容易被绕过的地方，代码专门处理。** `_embedded_ipv4s` 从 IPv6 地址里解出它可能路由到的 IPv4：IPv4-mapped（`::ffff:a.b.c.d`）、IPv4-compatible（已废弃的 `::a.b.c.d`）、6to4（`2002::/16`）、NAT64（`64:ff9b::/96` 与 `64:ff9b:1::/48`）、ISATAP、Teredo（其客户端 IPv4 是低 32 位取反）。函数有两个模式：常规模式只解码「公认的过渡上下文」，避免把一个字节恰好落在私有段的真实公网 IPv6 误判；穷尽模式对每种标准嵌入位置都解码，只用于云元数据检查（目标集合小，巧合概率可忽略，且能覆盖运维自定义的 NAT64 前缀）。

**zone identifier 被主动丢掉。** `_parse_ip` 的注释说：IPv6 字面量可以带 zone（如 `fd00:ec2::254%251`），Python 会把它揉进地址相等性与哈希；而 zone 只对链路本地目标有意义（内核对其余目标会忽略它、照样投递），所以它**绝不能改变一个检查怎么分类这个地址**。丢掉一次，所有检查就都只在地址本身比较。

**非法 IP 视为危险。** `is_private_ip` 在 `_parse_ip` 返回 `None` 时直接返回 `True`——解析不出来就按内网处理。这是一条明确的「失败即拒绝」选择。

**重定向手动跟随，每跳重查，且凭据按来源裁剪。** `safe_download` 的循环里，每次跟随都用 `_redirect_target` 算下一跳并递增计数（超过 `max_redirects` 抛错），然后回到循环开头重新做域名检查与 IP 检查。同时若这一跳跨了源，则用 `_without_sensitive_headers` 去掉 `Authorization`、`Cookie`、`Proxy-Authorization`。`_keeps_credentials` 的规则写得很具体：同源（scheme + host + port 全同）保留；例外是**同主机 http:80 → https:443 的升级**保留；其余（含端口变化、https→http 降级、跨主机）一律剥掉。这套规则文档说是照搬 httpx 对 `Authorization` 的源规则，并把它扩到所有敏感头。

**HTTPS 靠 `sni_hostname` 扩展。** 因为请求打到的是解析出的 IP，若不特殊处理，TLS 会按 IP 做 SNI 与证书校验。代码在 `is_https` 时把 `extensions['sni_hostname']` 设为原主机名，于是连的是 IP、验的是主机。

**cookie 按原主机名隔离。** 每次请求前构造一个「原主机名版本的 URL」作为 cookie 作用域，用它建或取该主机的 jar；发请求用 IP URL；响应回来后用原主机名那个 jar 存 cookie，并立刻 `client.cookies.clear()` 清掉 HTTPX 自动按解析 IP 存下的那份不安全副本。这段逻辑的注释解释了为什么必须清：IP 版 cookie 会把另一个站点在同一 IP 上的 cookie 混进来。

**体积上限在两处生效，且分编码方式。** `max_bytes` 给定时：请求头 `Accept-Encoding` 被强制为 `identity, gzip`（除非调用方自己设了），注释说明 Brotli/Zstandard 能在单次解码步把几字节膨胀成多 MiB，而 `deflate` 必须先整段缓冲才能分辨 zlib 包裹与裸帧，所以三者都被排除出可接受编码，收到也会拒绝。读取路径按编码分派：无编码走 `_read_capped_identity`（逐块累计，超限即抛），gzip 走 `_read_gzip_body`（用 zlib 的 `max_length` 在解码途中限流，成员一个一个解）。两者都用 `aiter_raw` 读**编码后**的流，所以超大的线流量在到达时就被拒，而不是先解压再检查。

**重建响应时剥掉编码相关头。** `_response_with_decoded_content` 去掉 `content-encoding` 与 `content-length`，否则 HTTPX 会再解一次码、且旧的 `content-length` 已经不再适用。这是一个纯粹的正确性细节，但漏了会立刻出错。

**`_aiter_raw` 把读失败包在生成器里而不是循环外。** 注释说明：这样循环体自己抛的错误（体积超限、gzip 畸形）能保留自己的类型，不被请求错误的包装覆盖。

**`WebFetchLocalTool` 把每种失败都变成 `ModelRetry`。** 它捕获 `ValueError`（含 SSRF 校验失败与体积超限）、`httpx2.InvalidURL`、`HTTPStatusError`、`RequestError`，统一抛 `ModelRetry`。注释给出两个具体理由：模型选 URL，所以每种坏法都必须变成它可行动的信息；`InvalidURL`（例如 IDNA 主机名被 httpx2 拒绝）既不是 `ValueError` 也不是 `HTTPError`，所以必须单独列出。

**上层工具的默认值全都是保守的。** `allow_local_urls=False`、`max_download_bytes` 有默认上限、`allowed_domains` / `blocked_domains` 为 `None`（不额外限制）。文档还提醒：因为模型控制 URL，若自定义 `headers` 里含凭据，应当同时用 `allowed_domains`。

**HTML 转换有并发闸与递归兜底。** 转换是 CPU 密集且随（服务器控制的）正文长度增长，所以在线程池跑；并发用进程内 `anyio.Semaphore`（`_MAX_CONCURRENT_HTML_CONVERSIONS`）限流；`markdownify` 递归下降所以过深的页面会抛 `RecursionError`，被转成 `ModelRetry` 让模型换目标。解码同样走线程池，理由是「服务器选字符集，解码耗时与正文成正比，某些编解码器更差」。

## 关键取舍

**源码事实：** 校验发生在**请求发出之前**（先解析、再检查、最后拿 IP 发请求），并且**每一跳重定向都重做**。这是这套防线最核心的一点：只在入口校验一次的实现会被一次重定向绕过。

**源码事实：** 所有解析出的 IP 都要过检查，不是只看第一个。这挡住「DNS 多返回、其中一条内网」。

**源码事实：** 云元数据与内网是两套判定：元数据**永远**拒（即使 `allow_local=True`），内网只在 `allow_local` 为假时拒。因此 `allow_local=True` 不等于「放开一切」。

**源码事实：** IPv6 过渡形式被专门解码后检查，且两个检查用不同的穷尽程度：内网检查用常规模式避免误判，元数据检查用穷尽模式追求覆盖。非法 IP 直接判为内网。

**源码事实：** 凭据只在同源（或同主机 http→https 升级）时跨跳保留，其余一律剥掉。

**源码事实：** 体积上限只支持 `identity` 与 `gzip`，并按编码在**流式解码途中**限流。不支持流式限流的编码既不被邀请（改 `Accept-Encoding`），收到也拒绝。

**源码事实：** cookie 的存储作用域是原主机名，且 HTTPX 自动按 IP 存下的副本会被主动清掉。

**源码事实：** 上层 `WebFetchLocalTool` 把校验失败也转成 `ModelRetry`。这意味着对模型而言「这个 URL 不允许抓」与「这个 URL 404」是同一种可重试的失败，模型会换个 URL 再试——这是刻意的产品取向（让模型能自我纠正），代价是安全拒绝不会作为硬异常暴露给应用。

**架构推论：** 这套机制防的是**「让服务端去访问本该由外部访问的地址」**这一类攻击：内网服务、云凭据端点、以及通过 DNS 或 IPv6 过渡形式伪装的目标。它**不防**「抓回来的是恶意内容」——那是内容层的事（框架只管转 markdown，注入防护靠 framing 与护栏）。把 SSRF 校验当成「抓网页就安全了」是典型的层次混淆。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `safe_download` | 唯一入口：协议、域名、解析、IP、重定向、体积的全流程 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `ResolvedUrl` | 「连哪个 IP」与「声称是谁」分离，供 SNI/Host 使用 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `validate_and_resolve_url` | 解析并逐个校验 IP，元数据必拒、内网按开关 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `is_cloud_metadata_ip` | 硬编码的云元数据地址表，含 IPv6 穷尽解码 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `is_private_ip` | 内网与特殊用途网段，非法 IP 视为内网 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `_embedded_ipv4s` | IPv6 过渡形式里解出可能路由到的 IPv4 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `_parse_ip` | 丢掉 zone identifier，保证分类不受其影响 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `_check_domain` | 允许/禁止域名表，跳前先判，归一化比较 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `_keeps_credentials` | 跨跳凭据规则：同源或同主机 http→https 升级 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `_read_capped_body` | 按编码分派的有界读取，编码途中限流 |
| `pydantic_ai_slim/pydantic_ai/_ssrf.py` | `DownloadTooLargeError` | 响应体超过 `max_bytes` |
| `pydantic_ai_slim/pydantic_ai/common_tools/web_fetch.py` | `WebFetchLocalTool` | 上层工具：把所有失败转成 `ModelRetry` |
| `pydantic_ai_slim/pydantic_ai/common_tools/web_fetch.py` | `WebFetchResult` | 抓取结果：URL、标题、清洗后的正文 |

## 参考

- [pydantic-ai 源码：_ssrf.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_ssrf.py)
- [pydantic-ai 源码：common_tools/web_fetch.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/common_tools/web_fetch.py)
- [Pydantic AI 文档：Web Fetch 能力](https://ai.pydantic.dev/capabilities/web-fetch/)
- [OWASP：服务端请求伪造](https://owasp.org/www-community/attacks/Server_Side_Request_Forgery)

## 与主核 / AI 的关系

SSRF 校验的位置选择与主核里「权限检查必须放在打开文件之前」同构：检查必须在**产生副作用的那一步之前**，而不是之后。这里的副作用是外发网络请求；如果先发再检查返回内容，攻击已经完成。

对 agent 工程的直接价值有三条。其一，**只要模型能选 URL，就必须有这层校验**，而且是每跳都校验。把它写在「入口校验一次」是最常见的实现错误。其二，**`allow_local` 不等于全放开**：云元数据仍然拒。这是设计里最有价值的一条细节，因为元数据端点恰恰是「内网可访问但看起来像公网」的那一类。其三，**体积上限要能流式生效**。先下载再检查大小等于没有上限——一个 4GB 的响应在检查前就已经把进程拖垮了。
