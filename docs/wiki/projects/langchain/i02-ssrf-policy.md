---
project: langchain
feature: ssrf-policy
title: 出站请求的 SSRF 防护
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/_security/_policy.py
    symbols: [SSRFPolicy]
  - path: libs/core/langchain_core/_security/_ssrf_protection.py
    symbols: [validate_safe_url, is_safe_url]
  - path: libs/core/langchain_core/_security/_transport.py
    symbols: [SSRFSafeTransport, SSRFSafeSyncTransport]
---

# 出站请求的 SSRF 防护

## 一句话职责

把「服务器被诱导去请求自己内网」这件事拦在两处：一处是**校验函数**（看 URL、看域名、看 DNS 解析出的每个 IP），另一处是**传输层**（校验完之后把请求**钉在已校验的那个 IP 上**——因为校验与连接之间 DNS 可以变。

## 机制是什么

三层职责分明：

**策略层**（`SSRFPolicy`）——一个 `@dataclasses.dataclass(frozen=True)`，七个字段：

```python
allowed_schemes: frozenset[str] = frozenset({"http", "https"})
block_private_ips: bool = True
block_localhost: bool = True
block_cloud_metadata: bool = True
block_k8s_internal: bool = True
allowed_hosts: frozenset[str] = frozenset()
additional_blocked_cidrs: tuple[IPv4Network | IPv6Network, ...] = ()
```

前四个是开着的否决位，第六个是「例外名单」（放行），第七个是「额外黑名单」。`DEFAULT_SSRF_POLICY = SSRFPolicy()` —— 默认全防。

**校验层**（`validate_url_sync` / `validate_url` / `validate_resolved_ip` / `validate_hostname`）——纯函数式，输入一个字符串与一份策略，违规就抛 `SSRFBlockedError`。

**传输层**（`SSRFSafeTransport` / `SSRFSafeSyncTransport`）——`httpx` 的 transport 实现，每次请求都跑一遍校验，然后改写目标地址。

`_ssrf_protection.py` 则是**面向 Pydantic 与普通调用方的薄包装**：模块 doc 只有一句「SSRF 防护——为内部调用方抛 `ValueError` 的薄包装。**把所有校验委托给 `_policy`**」。所以它不重复实现，只翻译异常类型（`SSRFBlockedError` → `ValueError`）。

## 怎么实现的

**黑名单是按 RFC 注释逐条枚举的，而不是「私网」三个字。** `_BLOCKED_IPV4_NETWORKS` 十五条，每条都有 RFC 出处：

`10.0.0.0/8`、`172.16.0.0/12`、`192.168.0.0/16`（RFC 1918 三个私网段）、`127.0.0.0/8`（RFC 1122 回环）、`169.254.0.0/16`（RFC 3927 链路本地）、`0.0.0.0/8`（RFC 1122「本网络」）、`100.64.0.0/10`（RFC 6598 共享/CGN 地址空间）、`192.0.0.0/24`（RFC 6890 IETF 协议分配）、`192.0.2.0/24` 与 `198.51.100.0/24` 与 `203.0.113.0/24`（RFC 5737 TEST-NET 1/2/3，文档用）、`198.18.0.0/15`（RFC 2544 基准测试）、`224.0.0.0/4`（RFC 5771 组播）、`240.0.0.0/4`（RFC 1112 保留）、`255.255.255.255/32`（RFC 919 受限广播）。

`_BLOCKED_IPV6_NETWORKS` 八条：`::1/128`、`fc00::/7`（ULA）、`fe80::/10`、`ff00::/8`、`::ffff:0:0/96`（IPv4 映射）、`::0.0.0.0/96`（IPv4 兼容，已废弃）、`64:ff9b::/96`（RFC 6052 NAT64 已知前缀）、`64:ff9b:1::/48`（RFC 8215 NAT64 发现前缀）。

这张表的信息量在于「哪些段看起来不像内网但其实通向内网」：

- **TEST-NET 与基准测试段**（`192.0.2.x` / `198.51.100.x` / `203.0.113.x` / `198.18.x`）本不该有真实服务，挡住它们防的是「内网里有人真的在这些段上跑了服务」。
- **`100.64.0.0/10`** 是运营商级 NAT 段，在容器网络里常被用。
- **组播与保留段**看起来无害，但一次「请求」打到组播地址会被多个主机收到，属于信息泄露面。

所以这张表不是「私有地址列表」，而是「**所有不该被服务器主动请求的地址段**」。

**IPv6 的漏洞通过「提取内嵌 IPv4」补掉。** `_extract_embedded_ipv4` 三支：

```python
if addr.ipv4_mapped is not None: return addr.ipv4_mapped
if addr in _NAT64_PREFIX or addr in _NAT64_DISCOVERY_PREFIX:
    raw = addr.packed
    return ipaddress.IPv4Address(raw[-4:])
if addr in _IPV4_COMPATIBLE_PREFIX and int(addr) > 1:
    return ipaddress.IPv4Address(int(addr))
return None
```

三支对应三种「IPv6 里藏 IPv4」的编码：`::ffff:x.x.x.x`（映射，`ipv4_mapped` 直接给）、NAT64 前缀（**取打包后的最后四个字节**）、`::x.x.x.x`（兼容格式，`int(addr)` 还原）。

而 `validate_resolved_ip` 里的用法：

```python
if isinstance(addr, ipaddress.IPv6Address):
    addr = ipaddress.IPv6Address(int(addr))
    inner = _extract_embedded_ipv4(addr)
    if inner is not None:
        addr = inner
```

第一行那个 `IPv6Address(int(addr))` 看起来只是重建，实际是一次**规范化**——`int()` 抹掉原字符串的任何变体写法（大小写、全零段压缩方式），再由 `int` 重建成标准形式。所以在提取之前先把地址归一到「纯数值表示」，避免「不同写法被不同判据漏掉」。

这个漏洞的现实性值得说清：一份 URL 里写 `http://[::ffff:127.0.0.1]/`，若只按 IPv6 判就会命中 `::ffff:0:0/96`（那是被挡的），但若一个**更细**的规则检查「`::1` 才叫回环」，那这个地址就漏了。所以「提取内嵌 IPv4 再判」是唯一可靠的顺序。

**`_ip_in_blocked_networks` 的检查顺序暴露了「哪条不能依赖别的开关」。** 四组检查，注释都标了独立性：

1. `block_private_ips` 时查两个黑名单（v4 与 v6 各一遍）。
2. `additional_blocked_cidrs` 按地址族分别查。
3. `block_localhost` 时查回环——注释：「回环检查**独立于 `block_private_ips`**，这样即使允许私网，`block_localhost=True` 仍能抓到 `127.x.x.x` / `::1`」。
4. `block_cloud_metadata` 时查云元数据——注释：「独立于 `block_private_ips`，这样 `allow_private=True` 仍会挡住云元数据端点」。且是两套判据：`str(addr) in _CLOUD_METADATA_IPS`（精确 IP 集合）与 `addr in net`（网段 `169.254.0.0/16`）。

第 3 组里有一个细节：IPv4 的回环判定是 `addr in _LOOPBACK_IPV4 or addr in ipaddress.IPv4Network("0.0.0.0/8")` ——除了 `127.0.0.0/8` 还包含 `0.0.0.0/8`。后者不是回环，但把它归到「localhost 地址」这个 reason 里，是因为它同样不该被主动请求（`0.x.x.x` 在语义上是「本网络」）。

而第 4 组那个 `_CLOUD_METADATA_NETWORKS` 与 `_BLOCKED_IPV4_NETWORKS` 里的 `169.254.0.0/16` 是**重复的**——注释解释了为什么接受这个重复：「当 `block_cloud_metadata=True` 时总是被挡的网络段，独立于 `block_private_ips`。整个链路本地段被各 provider 的云元数据服务使用」。

所以两处都列不是冗余低效，而是**让两条开关各自完备**：只开 `block_cloud_metadata` 时靠第 4 组挡住整个链路本地段；只开 `block_private_ips` 时靠第 1 组挡住。任一条单独生效都安全。

`_CLOUD_METADATA_IPS` 七个精确地址，注释逐个标明归属：`169.254.169.254`（AWS/GCP/Azure/DigitalOcean/Oracle）、`169.254.170.2`（AWS ECS）、`169.254.170.23`（AWS EKS Pod Identity Agent）、`100.100.100.200`（阿里云）、`fd00:ec2::254`（AWS EC2 IMDSv2 over IPv6）、`fd00:ec2::23`（AWS EKS IPv6）、`fe80::a9fe:a9fe`（OpenStack Nova IPv6）。

`_CLOUD_METADATA_HOSTNAMES` 四个域名：`metadata.google.internal`、`metadata.amazonaws.com`、`metadata`、`instance-data`。后两个是**裸名**——这是必需的，因为云内 DNS 常把 `metadata` 解析成元数据服务，而「裸名」在 URL 里也合法。

**`validate_hostname` 是三道按名字的检查，第三道最容易被忽略。** 实现：小写化后依次查 `_LOCALHOST_NAMES`（`localhost` / `localhost.localdomain` / `host.docker.internal`）、`_CLOUD_METADATA_HOSTNAMES`、以及 `block_k8s_internal` 时的 `.svc.cluster.local` **后缀**。

第三道用的是 `lower.endswith(_K8S_SUFFIX)` 而不是精确匹配——因为 K8s 内部 DNS 的名字是 `<service>.<namespace>.svc.cluster.local`，前缀无限多，只能用后缀。

而 `host.docker.internal` 在 localhost 名单里，说明 Docker 的宿主别名也被当成「本地」——它确实指向宿主。

**`validate_url_sync` 的顺序是「先粗后细」，而且有一段容易看错的 try/except/else。** 实现顺序：解析 → scheme 在白名单内 → hostname 非空 → 命中 `allowed_hosts` 就放行 → 然后：

```python
try:
    ipaddress.ip_address(hostname)
    validate_resolved_ip(hostname, policy)
except SSRFBlockedError:
    raise
except ValueError:
    pass
else:
    return
validate_hostname(hostname, policy)
```

`try` 里两行：第一行「这个 hostname 本身就是个 IP 吗」，不是就抛 `ValueError`；是就继续验 IP。

`except SSRFBlockedError: raise` ——IP 被挡，直接上抛。
`except ValueError: pass` ——**不是 IP**（域名），落到外面继续做域名检查。
`else: return` ——是 IP且验过了，直接返回（不再做域名检查）。

所以这段代码用异常作控制流，把「是 IP」与「是域名」分成两条路。这个写法的可读性有争议，但它的语义是精确的：`ipaddress.ip_address` 对域名抛 `ValueError`，而这个 `ValueError` 与「IP 被挡」是**不同类型的异常**，所以能分开处理。

`allowed_hosts` 的放行位置在 IP 判定之前 ——所以一份显式放行的域名会跳过所有后续检查。这是「例外名单必须显式设置」的设计：默认是空集。

**`_effective_allowed_hosts` 是一个环境变量驱动的自动放行，且只在「local 前缀」时生效。**

```python
if os.environ.get("LANGCHAIN_ENV", "").startswith("local"):
    extra.update({"localhost", "testserver"})
if extra: return policy.allowed_hosts | frozenset(extra)
return policy.allowed_hosts
```

用 `startswith("local")` 而不是 `== "local"`，所以 `local`、`local_test`、`local_dev` 都会命中。这比 `_ssrf_protection.py` 里那个 bypass 更宽（那里要求精确 `== "local_test"`）。

这两个「本地环境自动放行」的机制是这一层最该被审视的地方：它们把「是否处于本地开发环境」的判断交给一个环境变量，而这个变量在生产环境里若意外地被设成以 `local` 开头的值，防护就被削弱了。当然对 `localhost` / `testserver` 这两个具体名字的放行危害有限（它们本就指本机）。

**`validate_url`（异步）在 `validate_url_sync` 之上加 DNS。** 实现顺序：

1. 解析、取 hostname。
2. `validate_url_sync(url, policy)` ——先做全部无网络的检查。
3. `allowed` 的域名就直接返回（**跳过 DNS**）。
4. `port = parsed.port or (443 if scheme == "https" else 80)` ——默认端口按 scheme 定。
5. `await asyncio.to_thread(socket.getaddrinfo, hostname, port, type=socket.SOCK_STREAM)` ——用 `to_thread` 是因为 `getaddrinfo` 是阻塞的。
6. `except socket.gaierror` → `SSRFBlockedError("DNS resolution failed")`。**解析失败也算阻塞**，这是 fail-closed 的方向：宁可拒绝也不要放行一个无法验证的目标。
7. 遍历 `addrinfo`，对 `sockaddr[0]` 逐个 `validate_resolved_ip`。

第 7 步是「**全部都要过**」而不是「有一个过就行」：注释在 transport 里写得更明确（下面会看到）。理由很清楚：DNS 可能返回多个 A 记录，其中一个是内网地址；若只验第一个，攻击者只需让 DNS 返回两个记录、把内网那个放在后面。

**传输层是这一层的核心，因为只有它能解决「校验与连接之间的时间差」。** `SSRFSafeTransport.handle_async_request` 七步：

1. `validate_url_sync(str(request.url), self._policy)` ——scheme 与 hostname 检查。
2. 命中 `_effective_allowed_hosts` 就直接转给内层（跳过 DNS）。
3. `port = request.url.port or (443 if scheme == "https" else 80)`。
4. `await asyncio.to_thread(socket.getaddrinfo, hostname, port, type=SOCK_STREAM)`，`gaierror` → `SSRFBlockedError`，空结果也 → `SSRFBlockedError("DNS resolution returned no results")`。
5. 遍历 `addrinfo` 逐个 `validate_resolved_ip` ——「**任何一个被挡就拒绝**」。
6. `pinned_ip = addrinfo[0][4][0]` ——取第一个解析结果。
7. **改写 URL 的 host 为那个 IP**，同时：

```python
extensions = dict(request.extensions)
if scheme == "https":
    extensions["sni_hostname"] = hostname.encode("ascii")
pinned_request = httpx.Request(method=..., url=pinned_url, headers=request.headers, content=request.content, extensions=extensions)
```

第 6 与第 7 步合起来就是**IP 钉住（IP pinning）**，也是整个模块 docstring 的第一句话：「带 DNS 解析与 IP 钉住的 SSRF 安全 httpx 传输」。

为什么必须钉住？因为「校验」与「真正连接」之间有一个窗口。若不钉住，流程是「校验 DNS 结果 → 让内层 transport 自己再解析一次 DNS → 连接」。攻击者控制的 DNS 可以**第一次返回公网地址（通过校验）、第二次返回内网地址（被连接）**——这就是经典的 DNS rebinding。钉住之后，连接直接指向已校验的那个 IP，第二次解析根本不发生。

而 `sni_hostname` 那一步解决的是钉住的副作用：把 host 改成 IP 之后，TLS 握手会用 IP 做 SNI 并校验 IP 的证书——那必然失败（证书是签给域名的）。所以显式把原始 hostname 塞进 `extensions["sni_hostname"]`，让 httpx 用它做 SNI 与证书校验。而 `Host` 请求头因为直接复用 `request.headers`（注释：「Host 头已经设成原始值」），所以 HTTP 层看到的仍是原域名。

所以钉住这一个动作需要**三处配合**：URL 的 host（连接目标）、`sni_hostname`（TLS 身份）、`Host` 头（应用层身份）。少任何一处都会破坏正常请求或留下漏洞。

**重定向被重新校验，靠的是客户端配置而不是传输层逻辑。** 类 docstring 的末句：「重定向在每一跳都被重新校验，因为 `follow_redirects` 是设在**客户端**上的，这导致 `handle_async_request` 对每个重定向目标被再次调用」。

而两个工厂函数里正是这么设的：

```python
client_kwargs.setdefault("follow_redirects", True)
client_kwargs.setdefault("max_redirects", 10)
```

`setdefault` 意味着调用方可以覆盖。所以「每一跳都校验」这个保证是否成立，取决于「客户端是否自己处理重定向」。若调用方传 `follow_redirects=False` 并自己跟随，那传输层只会看到第一个 URL——**防护被绕过**。

这个依赖关系值得记下来：**安全属性依赖一个可被覆盖的配置项**。`max_redirects=10` 是同一件事的另一半（限制跳数，防止重定向环）。

**工厂函数的 kwargs 分流是「哪些属于传输、哪些属于客户端」。** `_TRANSPORT_KWARGS` 七个：`verify`、`cert`、`trust_env`、`http1`、`http2`、`limits`、`retries`。循环把命中的塞进 `transport_kwargs`、其余进 `client_kwargs`。

所以 `ssrf_safe_async_client` 的 docstring 说它是「`httpx.AsyncClient(...)` 的即插即用替代——调用方只需换掉构造函数调用」。这个分流表是实现「即插即用」的必要条件：若不分流，一个 `verify=False` 会被传给 `AsyncClient`（那里不接受）而报错。

而 `retries` 在传输层意味着**重试也走同一份钉住逻辑**——所以重试不会引入新的 DNS 解析。

**`_ssrf_protection.py` 的 `_policy_for` 把旧的两布尔接口映射成策略，且有两个位不受旧接口影响。** 实现：

```python
schemes = frozenset({"http", "https"}) if allow_http else frozenset({"https"})
return SSRFPolicy(allowed_schemes=schemes, block_private_ips=not allow_private, block_localhost=not allow_private, block_cloud_metadata=True, block_k8s_internal=True)
```

`block_cloud_metadata=True` 与 `block_k8s_internal=True` 是**硬编码**的——所以 `allow_private=True` 只放开私网与 localhost，云元数据与 K8s 内部 DNS 永远挡着。

而 `validate_safe_url` 的 docstring 把这件说明写了：「`allow_private`：若为 `True`，允许私网 IP 与 localhost（供开发用）。**云元数据端点永远被挡**」。

这是这一层最重要的一条策略表述：`allow_private` 是「开发便利」，不是「全放」。云元数据是个特别危险的例外——一次成功的 SSRF 打到元数据服务，往往能拿到实例凭据（进而拿到整个云账号的权限）。所以它不能被「为了本地调试方便」的开关关掉。

**`validate_safe_url` 里有一个测试环境的直接短路，位置在任何检查之前。**

```python
if (os.environ.get("LANGCHAIN_ENV") == "local_test" and hostname.startswith("test") and "server" in hostname):
    return url_str
```

注释：「测试环境旁路（从原始实现保留下来的）」。三个条件的与：环境精确为 `local_test`、hostname 以 `test` 开头、hostname 含 `server`。所以它放行 `testserver` 之类的名字。

这个短路的存在是**历史妥协**：测试里要用 `testserver` 这个假域名，而它既不是 localhost 也不在 `allowed_hosts` 里，于是加了这个特判。但它的条件组合足够具体（环境精确匹配 + 两个子串），误命中的面很窄。

与 `_effective_allowed_hosts` 里那个 `startswith("local")` 的比对很有意思：两处都靠 `LANGCHAIN_ENV` 判断测试环境，一处精确匹配 `local_test`、一处前缀匹配 `local`。同一个环境变量在两处被以不同严格度使用，是一个轻微的不一致。

**Pydantic 集成靠四个 `Annotated` 类型。** 文件末尾：

```python
SSRFProtectedUrl = Annotated[HttpUrl, BeforeValidator(_validate_url_ssrf_strict)]
SSRFProtectedUrlRelaxed = Annotated[HttpUrl, BeforeValidator(_validate_url_ssrf_relaxed)]
SSRFProtectedHttpsUrl = Annotated[HttpUrl, BeforeValidator(_validate_url_ssrf_https_only)]
SSRFProtectedHttpsUrlStr = Annotated[str, ...]
```

`BeforeValidator` 在类型转换**之前**跑，所以它看到的是原始输入（可能是任意字符串），可以据此拒绝。`_validate_url_ssrf_strict` / `_https_only` / `_relaxed` 三个函数对应三种严格度。所以「模型配置里的 `base_url` 字段」可以直接标成 `SSRFProtectedUrl`，把校验挂在 Pydantic 字段声明上——这比在每个调用点手动调 `validate_safe_url` 可靠得多。

注意这四个类型只做**同步**校验（`_validate_url_sync`），不做 DNS。理由是 Pydantic 验证器是同步的，不能 await。所以「Pydantic 层挡住明显的坏 URL，DNS 层挡住解析到内网的域名」是两道分开的防线——这也解释了 `validate_url_sync` 的 docstring 为什么写「适用于 Pydantic 验证器与其他同步上下文。**只检查 scheme 与 hostname 模式——完整 DNS 感知检查请用 `validate_url`**」。

**这个不对称是这一层最需要使用者理解的限制**：一个配置字段被 Pydantic 校验通过，**不等于**它真的安全。若那个 URL 后面被真正请求，必须走传输层或 `validate_url`（异步）才会做 DNS 检查。而很多「配置时校验、稍后使用」的场景恰好落在这个缝里。

## 关键取舍

**源码事实：** 黑名单是逐条按 RFC 枚举的，包含 TEST-NET、基准测试段、CGN 段、组播、保留段，而不只是三个私网段。

**源码事实：** `_extract_embedded_ipv4` 处理三种 IPv6 藏 IPv4 的编码（映射、NAT64、兼容），且提取前先用 `IPv6Address(int(addr))` 规范化。

**源码事实：** 回环与云元数据的检查标记为「独立于 `block_private_ips`」，所以 `allow_private=True` 仍挡它们。

**源码事实：** `169.254.0.0/16` 在两个黑名单里重复，注释说明是为了让两条开关各自完备。

**源码事实：** `_effective_allowed_hosts` 在 `LANGCHAIN_ENV` 以 `local` 开头时自动加入 `localhost` 与 `testserver`。

**源码事实：** DNS 解析失败与空结果都抛 `SSRFBlockedError`（fail-closed）。

**源码事实：** 传输层校验**所有**解析结果，然后**钉住第一个 IP**，并同时设置 `sni_hostname` 与保留原 `Host` 头。

**源码事实：** 「重定向逐跳校验」依赖客户端设置了 `follow_redirects`，而该值可被调用方覆盖。

**源码事实：** `_policy_for` 把 `block_cloud_metadata` 与 `block_k8s_internal` 硬编码为 `True`，`allow_private` 不影响它们。

**源码事实：** `validate_safe_url` 有一个 `LANGCHAIN_ENV == "local_test"` 且 hostname 含 `test` + `server` 的直接短路，位于所有检查之前。

**源码事实：** Pydantic 的 `SSRFProtectedUrl` 系列只跑同步校验，不做 DNS。

**架构推论：** 「校验 + 钉住」这个组合是 SSRF 防护的正确形态，因为**单靠校验必然有 TOCTOU 窗口**。任何「先检查目标、再让底层库自己连接」的设计都无法阻止 DNS rebinding：两次 DNS 解析之间可以有任意长的时间，而攻击者控制 DNS。钉住 IP 把「检查的对象」与「连接的对象」变成同一个值，从而关闭这个窗口。这条规则的普遍形式是：**校验过的值必须就是使用的值**，中间不能有第二次解析或转换。

**架构推论：** 「云元数据永远挡住」是这套策略里唯一没有开关的东西。理由是威胁模型的不对称：私网访问可能只是调试需要，而元数据服务的价值是「实例凭据」——一次成功就能横向扩到整个账号。当两个风险的性质差异这么大时，「一个开关控制两者」就是错的，必须把危险的那个单独钉死。

**架构推论：** 「fail-closed on 解析失败」这个方向值得单独点出。DNS 失败时不放行，意味着网络抖动会让合法请求失败——这是一个**可用性代价**。但它换来的是「无法验证的一律不信任」。在这个方向上出错（拒绝合法请求）是可恢复的、可见的；反方向出错（放行未验证目标）是静默的、且往往不可挽回。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/_security/_policy.py` | `SSRFPolicy` | 冻结的七字段策略，默认全防 |
| `libs/core/langchain_core/_security/_policy.py` | `_BLOCKED_IPV4_NETWORKS` | 十五条按 RFC 注释的禁用 IPv4 段 |
| `libs/core/langchain_core/_security/_policy.py` | `_BLOCKED_IPV6_NETWORKS` | 八条 IPv6 段，含映射与 NAT64 |
| `libs/core/langchain_core/_security/_policy.py` | `_CLOUD_METADATA_IPS` | 七个各云厂商元数据端点精确地址 |
| `libs/core/langchain_core/_security/_policy.py` | `_CLOUD_METADATA_HOSTNAMES` | 四个元数据域名，含两个裸名 |
| `libs/core/langchain_core/_security/_policy.py` | `_extract_embedded_ipv4` | 三种 IPv6 内嵌 IPv4 编码的提取 |
| `libs/core/langchain_core/_security/_policy.py` | `_ip_in_blocked_networks` | 四组检查，回环与元数据独立于私网开关 |
| `libs/core/langchain_core/_security/_policy.py` | `validate_resolved_ip` | 先规范化再提取内嵌 IPv4 再判 |
| `libs/core/langchain_core/_security/_policy.py` | `validate_hostname` | 三道按名检查，含 K8s 后缀 |
| `libs/core/langchain_core/_security/_policy.py` | `_effective_allowed_hosts` | 本地环境下自动放行两个名字 |
| `libs/core/langchain_core/_security/_policy.py` | `validate_url_sync` | 无 DNS 的同步校验，异常作控制流分流 |
| `libs/core/langchain_core/_security/_policy.py` | `validate_url` | 异步校验，解析 DNS 并验每个 IP |
| `libs/core/langchain_core/_security/_ssrf_protection.py` | `validate_safe_url` | 薄包装，翻译异常并含测试短路 |
| `libs/core/langchain_core/_security/_ssrf_protection.py` | `is_safe_url` | 不抛异常的布尔版本 |
| `libs/core/langchain_core/_security/_ssrf_protection.py` | `_policy_for` | 旧双布尔接口到策略的映射 |
| `libs/core/langchain_core/_security/_ssrf_protection.py` | `SSRFProtectedUrl` | Pydantic 前置校验的类型别名 |
| `libs/core/langchain_core/_security/_transport.py` | `SSRFSafeTransport` | 异步传输，校验全部 IP 后钉住第一个 |
| `libs/core/langchain_core/_security/_transport.py` | `SSRFSafeSyncTransport` | 同步镜像实现 |
| `libs/core/langchain_core/_security/_transport.py` | `ssrf_safe_client` | 同步客户端工厂，按传输/客户端分流 kwargs |
| `libs/core/langchain_core/_security/_transport.py` | `ssrf_safe_async_client` | 异步客户端工厂，设 `follow_redirects` 默认 |

## 参考

- [langchain 源码：_security/_policy.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/_security/_policy.py)
- [langchain 源码：_security/_transport.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/_security/_transport.py)
- [OWASP：服务端请求伪造（SSRF）防护](https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html)

## 与主核 / AI 的关系

主核里最接近的是「外部表 / FDW 的连接白名单」：数据库允许连到外部数据源，但必须限制「能连哪些地址」，否则一条 SQL 就能让服务器去探测内网。这一页的 `SSRFPolicy` 与那些连接白名单是同一职责，连「必须按解析后的实际地址判而不是按名字判」这个原则都一样。

差别在于**判据的层次更多**。主核的连接目标通常是一个固定的 `host:port` 配置；这里的 URL 可能来自模型输出、文档内容、或用户上传的 manifest——也就是**不受控的输入**。所以这里要处理 DNS rebinding、IPv6 内嵌、重定向、以及「配置时校验通过但使用时才连接」这一整条链。i01 那页的 doc 里那句「一份不可信 manifest 可以把 `base_url` 指向攻击者主机——这是一个 SSRF 向量」，正是这两页的交汇点：序列化能重建配置，而这个配置里的 URL 必须由这一层把关。

与本组 g04（内存存储）的 `load(..., allowed_objects=[Document])` 对照：两处都在做「限制外部输入的影响面」，但一个是限制**类型**，一个是限制**目标地址**。判据的形态一致——都是「白名单 + 默认拒绝」，只是作用维度不同。

对 AI 工程最实用的一条是「校验过的值必须就是使用的值」。任何「先验后连」的设计都必须回答：验完之后，底层还会不会再解析一次？如果会，那个窗口就是漏洞。做法是显式钉住——把校验得到的 IP 直接塞进连接目标，同时把「原域名」通过 SNI 与 `Host` 头带过去。三处都要改，少一处要么破坏 TLS 校验、要么破坏应用层路由。

另一条是「危险程度差异极大的风险不能共用一个开关」。`allow_private` 放开私网是合理的开发便利，但如果它也顺带放开了云元数据端点，那一次误配置就能让开发机上的凭据被拿走。把最危险的那条单独硬编码成永远开，是唯一稳妥的做法。

最后一条是 Pydantic 校验与 DNS 校验的分层。同步的字段校验能挡住明显的坏形状（非法 scheme、裸 IP、localhost 名），但它无法判断「这个域名会解析到内网」——那需要 DNS，而 DNS 是异步/阻塞的。所以要清楚这条缝的存在：**字段校验通过 ≠ 目标安全**。任何「配置时校验、稍后使用」的 URL，都必须在使用前再走一次 DNS 感知的检查（或直接用带钉住的客户端）。
