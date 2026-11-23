---
project: postgres
feature: tls-transport
page_type: feature
schema_version: 1
refs:
  - path: src/backend/libpq/be-secure-openssl.c
    symbols: [be_tls_init, be_tls_open_server, be_tls_read]
  - path: src/backend/libpq/be-secure.c
    symbols: [secure_open_server, secure_read]
---

# TLS 传输安全

## 一句话职责

服务端在启动或配置重载时构建 TLS 上下文，并在每个连接上执行握手，为后续报文提供加密读写通道。

## 机制是什么

TLS 在此是传输层包装而非认证方法：连接最初以明文开始，客户端发送 SSLRequest 后，服务端用 `SSL_accept` 完成握手；握手期间可协商 ALPN 协议标识、索取并校验客户端证书，并把得到的对端证书主体信息写回 `Port`，供认证阶段消费。

本机制负责上下文构建、握手驱动、对端证书信息提取以及加密后的字节读写。它不负责决定“是否必须使用 TLS”——那是 `pg_hba.conf` 中 `hostssl`/`hostnossl` 与 `ssl` 相关配置的职责；也不负责根据证书主体做授权决策，`clientcert` 语义与 `CheckCertAuth` 属于认证路径。GSS 加密是并行的另一套实现，本文不覆盖。

## 怎么实现的

1. `be_tls_init` 从零构建上下文：先清掉 `ssl_loaded_verify_locations`，因为服务端在收到 ClientHello 之前并不知道会用到哪个主机配置。随后在独立的内存上下文里分配候选 `struct hosts`，并注册 `MemoryContextCallback`（回调 `host_context_cleanup_cb`），使旧配置被删除时能一并释放 OpenSSL 管理的对象。
2. 若启用 `ssl_sni`，`be_tls_init` 通过 `load_hosts` 读取主机配置文件；加载失败即按启动期/重载期分别以 FATAL 或 LOG 报错，而不是静默回落到 `postgresql.conf`。解析出的条目按主机名去重，并区分 default 主机、`/no_sni/` 主机与具名主机；随后逐条 `init_host_context` 建立各自的 `SSL_CTX`。若没有任何可用配置则整体报错。
3. 未启用 SNI、或主机配置文件缺失/为空时，`be_tls_init` 把 GUC 中的证书、私钥、CA 与 passphrase 命令复制成一条 `HostsLine`，把它作为 default 主机配置。这种复制是刻意的：重载失败时 GUC 值可能已不完整，副本保证已有配置仍可用。
4. 上下文主体（协议版本上下界、密文套件、是否由服务端偏好顺序等）在这里统一设置。成功时才替换全局 `SSL_context` 与 `SSL_hosts`，并删除旧的主机内存上下文；失败路径释放新建的 `SSL_CTX`、删除临时上下文，并在 `ssl_sni` 的取值与已装载配置不一致时发一条 WARNING，说明“旧配置与旧 SNI 状态继续生效”。
5. `secure_open_server` 是通用入口：它先把尚未加密的缓冲数据（`pq_buffer_remaining_data`）搬进 `port->raw_buf`，避免客户端在握手请求之后紧接着发送的字节丢失；调用 `be_tls_open_server` 握手；若握手结束后仍残留未消费的原始字节，说明客户端在建链前就发了加密数据，记为异常并返回错误。
6. `be_tls_open_server` 先确认 `SSL_context` 存在，再注册 info 回调、ALPN 选择回调，用 `SSL_new` 建连接对象并绑定 socket BIO，置 `ssl_in_use`。支持 client hello 回调的构建里注册 `sni_clienthello_cb`；不支持的构建（如 LibreSSL）则在有 CA 时直接 `SSL_set_verify` 并置 `ssl_loaded_verify_locations`。
7. 握手循环里每次调用前清空 OpenSSL 错误队列（`ERR_clear_error`）再 `SSL_accept`，因为 `SSL_get_error` 依赖每线程错误队列为空。`WANT_READ`/`WANT_WRITE` 用 `WaitLatchOrSocket` 等待可读/可写后重试；`SYSCALL`、`SSL`、`ZERO_RETURN` 分别转成带原因的 COMMERROR 并返回失败，其中协议版本类错误会追加一条提示客户端可能不支持当前协议区间的 hint。
8. 握手成功后读取 ALPN 选择结果：只有与 `PG_ALPN_PROTOCOL` 完全一致才置 `alpn_used`，否则报“unexpected ALPN protocol”。随后取对端证书，提取 Common Name 与 RFC2253 格式的 DN，并拒绝含内嵌 NUL 的值，最后置 `peer_cert_valid`。
9. 数据阶段 `secure_read` 按可用通道分派：`ssl_in_use` 走 `be_tls_read`，GSS 加密走 `be_gssapi_read`，否则走 `secure_raw_read`；非阻塞读返回 `EWOULDBLOCK`/`EAGAIN` 且处于阻塞模式时，它会按通道给出的 `waitfor` 修改 `FeBeWaitSet` 并等待，同时处理 postmaster 死亡与 latch 中断，然后重试。
10. `be_tls_read` 把 OpenSSL 返回码映射成 POSIX 风格结果：`WANT_READ`/`WANT_WRITE` 通过 `*waitfor` 回传期望方向并置 `EWOULDBLOCK`，`SYSCALL` 在没有 errno 时归一成 `ECONNRESET`，`SSL_ERROR_SSL` 记错误后同样归一，`ZERO_RETURN` 视为对端正常关闭并返回 0。

## 关键取舍

- 源码事实：`be_tls_init` 在失败分支保留旧的 `SSL_context`/`SSL_hosts`，仅在 `ssl_sni` 设置与旧状态不一致时警告。放弃“重新加载失败就清空 TLS 配置”换取已有连接与后续连接继续可用，代价是运维看到的配置可能与文件内容不一致，只能靠这条 WARNING 察觉。
- 源码事实：SNI 配置存在但解析失败时直接报错而不回落 `postgresql.conf`，注释明确写“避免静默回落到非期望的配置”。放弃“总能启动”的容错换取配置意图不被悄悄改变，代价是配置文件里的一个错误会在启动或重载期直接阻断 TLS 初始化。
- 源码事实：`secure_open_server` 在握手前把明文缓冲搬进 `raw_buf`，握手后又检查是否仍有残留。放弃“握手后直接丢弃旧缓冲”换来不丢失请求切换前已到达的字节；代价是多一条“残留即异常”的判定路径，需要区分合法缓冲与建链前越界发送。
- 源码事实：`sni_clienthello_cb` 在未启用 SNI 时仍被安装，只是固定装载默认配置。源码注释说明这是为了在不同构建下保持 OpenSSL 上下文的处理一致；代价是关闭 SNI 时仍走一次回调，而不是在 `be_tls_open_server` 里直接分支。
- 源码事实：`be_tls_open_server` 在握手等待阶段依赖 `StartupPacketTimeoutHandler` 直接退出进程，注释说明此处无需自行处理超时与中断。放弃在握手内部再实现一套超时逻辑换取实现简单，代价是握手期的超时行为与其它阶段不一致，只能由外层启动包超时兜底。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/libpq/be-secure-openssl.c` | `be_tls_init` | 解析 SNI 或 GUC 主机配置、逐条建立上下文并原子替换全局 TLS 状态 |
| `src/backend/libpq/be-secure-openssl.c` | `be_tls_open_server` | 建立连接对象、驱动握手、协商 ALPN 并提取对端证书主体信息 |
| `src/backend/libpq/be-secure-openssl.c` | `be_tls_read` | 把 OpenSSL 读结果翻译为可等待的 POSIX 返回值与等待方向 |
| `src/backend/libpq/be-secure.c` | `secure_open_server` | 保留未加密的残余字节并调用 TLS 握手，检查握手后是否仍有残留 |
| `src/backend/libpq/be-secure.c` | `secure_read` | 在 TLS、GSS 与明文三种通道间分派读写并在阻塞模式下等待事件 |

## 参考

- [PostgreSQL 文档：SSL/TLS 配置](https://www.postgresql.org/docs/current/ssl-tcp.html) —— 支撑证书、私钥、CA 与 ssl_sni 等配置项语义。
- [PostgreSQL 文档：pg_hba.conf 中的 hostssl](https://www.postgresql.org/docs/current/auth-pg-hba-conf.html) —— 支撑“是否要求 TLS”由 HBA 而非传输层决定的边界。
- [RFC 7301：TLS ALPN](https://datatracker.ietf.org/doc/html/rfc7301) —— 支撑 ALPN 协商失败时以 no_application_protocol 告警拒绝连接。
- [PostgreSQL 官方源码：OpenSSL 后端实现](https://github.com/postgres/postgres/blob/master/src/backend/libpq/be-secure-openssl.c) —— 支撑上下文构建、握手循环与证书提取细节。
- [PostgreSQL 官方源码：传输层包装](https://github.com/postgres/postgres/blob/master/src/backend/libpq/be-secure.c) —— 支撑通道分派与阻塞等待逻辑。

## 与主核 / AI 的关系

暂无直接对应。主核若引入 TLS，可借鉴“配置原子替换失败即回退旧状态”“握手前保留已到字节”“把库返回码归一成可等待结果”三点；但 SNI 多主机配置与证书主体到授权的映射需按主核自身连接模型定义。
