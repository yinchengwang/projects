---
project: redis
feature: tls-transport
page_type: feature
schema_version: 1
refs:
  - path: src/tls.c
    symbols: [tlsInit]
---

# TLS 传输

## 一句话职责

`tlsInit` 把 OpenSSL 的进程级状态初始化好，`tlsConfigure` 把 `redisTLSContextConfig` 变成三份可复用的 `SSL_CTX`（服务端 / 集群总线 / 客户端），`CT_TLS` 这张 `ConnectionType` 表把这些上下文装配成一种真正的连接类型——此后所有加密读写、握手推进、事件注册与对端身份校验都走同一套连接抽象。

## 机制是什么

TLS 在 Redis 里不是一个独立子系统，而是**连接类型**的一种实现。`src/connection.h` 定义了 `ConnectionType` 结构：一张函数指针表，提供 `conn_create` / `conn_create_accepted` / `accept` / `connect` / `read` / `write` / `close` / `ae_handler` 等能力；`src/tls.c` 末尾的 `CT_TLS` 就是「TLS 那一张」。三种连接类型按名字注册进 `connection.c` 里的 `connTypes[]`（容量常量 `CONN_TYPE_MAX` 为 8，注释写的是「8 is enough to be extendable」），名字常量分别是 `CONN_TYPE_SOCKET`（`"tcp"`）、`CONN_TYPE_UNIX`（`"unix"`）、`CONN_TYPE_TLS`（`"tls"`）。

因此本机制分三层：

1. **进程层**：`tlsInit` 做 OpenSSL 全局初始化与 RNG 播种。
2. **配置层**：`tlsConfigure` 依据 `redisTLSContextConfig`（`src/server.h` 中的 `server.tls_ctx_config`）构建 `SSL_CTX`；`createSSLContext` 建基座，`configureServerSSLContext` 补服务端专属项，`tlsSetSessionIdContext` 给每条连接打会话命名空间标签。
3. **连接层**：`connCreateTLS` / `connCreateAcceptedTLS` 拿某个 `SSL_CTX` 调 `SSL_new`，`tlsHandleEvent` 按连接状态推进 `SSL_connect` / `SSL_accept` / 读写；事件注册与增删由 `updateSSLEvent` / `registerSSLEvent` 负责。

三个全局 `SSL_CTX` 指针定义了全部身份：`redis_tls_ctx`（服务端主上下文）、`redis_tls_client_ctx`（显式配了客户端证书时才创建）、`redis_tls_cluster_ctx`（`server.tls_cluster` 为真时才创建）。前两个不是 `static`，因为 `src/sentinel.c` 用 `extern` 直接取用它们建 hiredis 之外的 `SSL` 对象。

本页只讲 TLS 上下文的初始化与连接装配，不含 ACL 权限判定（k01）。

## 怎么实现的

**注册与初始化入口。** `connTypeInitialize` 先 `serverAssert(RedisRegisterConnectionTypeSocket() == C_OK)`，再调 `RedisRegisterConnectionTypeTLS()`（注释写明「may fail if without BUILD_TLS=yes」，所以不 assert），最后注册 Unix 类型（注释解释：Unix 放最后，是为了让监听器在 TCP/TLS 之后初始化）。`connTypeRegister` 用 `ct->get_type(NULL)` 取类型名，忽略大小写查重，找到空槽后存表，随后调用 `if (ct->init) ct->init()` —— 对 `CT_TLS` 而言这个 `init` 就是 `tlsInit`。未编译 TLS 时（`USE_OPENSSL` 分支不成立），`RedisRegisterConnectionTypeTLS` 只打一条 `LL_VERBOSE` 日志「Connection type %s not builtin」并返回 `C_ERR`；`connectionTypeTls()` 用一个独立的 `cached` 静态标志把「查过、结果是 NULL」也缓存住，注释点明 TCP/Unix 类型不会缺失，所以只有它需要区分「没查」与「查到 NULL」。

**`tlsInit` 的顺序。** 全部动作都围绕三件事：加载 OpenSSL 配置、按版本装锁、播种随机数。版本分支是这样切的：

```c
#if OPENSSL_VERSION_NUMBER < 0x10100000L
OPENSSL_config(NULL);
SSL_load_error_strings();
SSL_library_init();
#elif OPENSSL_VERSION_NUMBER < 0x10101000L
OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CONFIG, NULL);
#else
OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CONFIG|OPENSSL_INIT_ATFORK, NULL);
#endif
```

注释说明为什么要先做这一步：`OPENSSL_config()` / `OPENSSL_init_crypto()` 应当是调用 OpenSSL 库的第一个函数，这样 `openssl.cnf` 里的标准配置才生效。只有 `OPENSSL_VERSION_NUMBER < 0x10100000L` 时才定义 `USE_CRYPTO_LOCKS`，进而调 `initCryptoLocks`：它先查 `CRYPTO_get_locking_callback() != NULL`，为真就直接返回（注释：别人已经装过回调，别去破坏它），否则按 `CRYPTO_num_locks()` 分配互斥锁数组并逐个 `pthread_mutex_init`，最后 `CRYPTO_set_locking_callback(sslLockingCallback)`。收尾是 `if (!RAND_poll())` 时打一条 `LL_WARNING`「OpenSSL: Failed to seed random number generator.」——失败只记录，不 abort。

**`tlsConfigure`：三份上下文、原子替换。** 它是 `CT_TLS.configure`，由 `connTypeConfigure(ct, priv, reconfigure)` 间接调用，`priv` 就是 `&server.tls_ctx_config`。函数头的短路是「非重配且已有服务端上下文」就直接 `C_OK` 返回：

```c
if (!reconfigure && redis_tls_ctx) {
    return C_OK;
}
```

这条短路就是真实的降级口子：`applyTLSPort`（`CONFIG SET tls-port` 的 apply 回调）传 `reconfigure = 0`，注释写「Configure TLS in case it wasn't enabled」，所以它只在首次启用 TLS 时真正建上下文；而 `applyTlsCfg`（`tls-cert-file`、`tls-ciphers` 等一大批选项的 apply）与 `initListeners`（启动时）都传 `reconfigure = 1`，走完整重建。

重建前有四道校验，任一失败即 `goto error`：

- `!ctx_config->cert_file` → 「No tls-cert-file configured!」。
- `!ctx_config->key_file` → 「No tls-key-file configured!」。
- `((server.tls_auth_clients != TLS_CLIENT_AUTH_NO) || server.tls_cluster || server.tls_replication) && !ca_cert_file && !ca_cert_dir` → 「Either tls-ca-cert-file or tls-ca-cert-dir must be specified when tls-cluster, tls-replication or tls-auth-clients are enabled!」。`tls-auth-clients` 的枚举默认值是 `TLS_CLIENT_AUTH_YES`（`src/config.c` 的 `createEnumConfig`），枚举三值是 `no` / `yes` / `optional`。
- `parseProtocolsConfig(ctx_config->protocols) == -1`。该函数对 `NULL` 返回 `REDIS_TLS_PROTO_DEFAULT`；宏定义是「有 `TLS1_3_VERSION` 时取 `REDIS_TLS_PROTO_TLSv1_2|REDIS_TLS_PROTO_TLSv1_3`，否则只取 TLSv1.2」，注释写「Use safe defaults」。字符串按 `" "` 切成 token 逐个 `strcasecmp`，只认 `tlsv1` / `tlsv1.1` / `tlsv1.2` / `tlsv1.3` 四种；不认识的 token 打「Invalid tls-protocols specified.」并置 `-1`；`tlsv1.3` 在缺 `TLS1_3_VERSION` 的构建里也置 `-1`。

校验通过后按顺序建上下文：`ctx = createSSLContext(ctx_config, protocols, 0)` → `configureServerSSLContext(ctx, ctx_config, "redis", sizeof("redis") - 1)`；若 `server.tls_cluster` 再建一份 `cluster_ctx` 并用会话命名空间 `"redis-cluster"`；若 `client_cert_file && client_key_file` 都配了才建 `client_ctx = createSSLContext(..., 1)`。注释点明「Create server-side contexts with identical TLS settings but distinct session namespaces」——两份服务端上下文的 TLS 参数完全一样，只有会话命名空间不同。

成功路径才释放旧上下文，随后一次性换指针：

```c
SSL_CTX_free(redis_tls_ctx);
SSL_CTX_free(redis_tls_client_ctx);
SSL_CTX_free(redis_tls_cluster_ctx);
redis_tls_ctx = ctx;
redis_tls_client_ctx = client_ctx;
redis_tls_cluster_ctx = cluster_ctx;
```

`error` 分支只释放本次新建的 `ctx` / `client_ctx` / `cluster_ctx` 并返回 `C_ERR`——旧上下文原封不动，这正是函数头注释「This operation is atomic and will leave the SSL_CTX unchanged if fails」的含义。调用方看到 `C_ERR` 后的动作是点名可查的：`initListeners` 打「Failed to configure TLS. Check logs for more info.」后 `exit(1)`，`applyTlsCfg` 写入错误串「Unable to update TLS configuration. Check server logs.」并拒绝这次配置。

**`createSSLContext` 的固定参数。** 客户端与服务端的差异只在三个文件名：`cert_file` / `key_file` / `key_file_pass` 各有一个 `client_` 前缀的对应字段，由 `client` 参数选择。其余一律共用：

- `SSL_CTX_new(SSLv23_method())`，再 `SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3)`。
- 有 `SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS` 定义时也打开它。
- 协议开关是「不想要谁就禁谁」：`protocols` 里没有 `REDIS_TLS_PROTO_TLSv1` 就 `SSL_OP_NO_TLSv1`，依次类推到 TLSv1.3（1.2/1.3 的选项各自包在 `#ifdef` 里）。
- 有 `SSL_OP_NO_COMPRESSION` 则关闭压缩；`SSL_CTX_set_mode` 打开 `SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`。
- `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL)` —— 这是**上下文级**默认，`connCreateAcceptedTLS` 会按 `tls-auth-clients` 逐连接改写它。
- 口令回调 `SSL_CTX_set_default_passwd_cb(ctx, tlsPasswordCallback)`，用户数据是 `key_file_pass`。`tlsPasswordCallback` 在 `pass` 为 NULL 或 `strlen(pass) > size` 时返回 `-1`，否则 `memcpy` 并返回长度。
- 依次 `SSL_CTX_use_certificate_chain_file`、`SSL_CTX_use_PrivateKey_file(..., SSL_FILETYPE_PEM)`，每步失败都把 `ERR_error_string_n` 灌进栈上 `errbuf`（大小 256）再 `goto error`。
- `ca_cert_file || ca_cert_dir` 存在时才 `SSL_CTX_load_verify_locations`。
- `ciphers` 非空则 `SSL_CTX_set_cipher_list`；有 `TLS1_3_VERSION` 且 `ciphersuites` 非空则 `SSL_CTX_set_ciphersuites`。
- `groups` 非空时走 `redisTlsCtxSetGroupsList`，它在 `CONN_TLS_SUPPORTS_GROUPS` 为 1 时映射到 `SSL_CTX_set1_groups_list`，若头文件只提供旧名字则映射到 `SSL_CTX_set1_curves_list`；两者都没有的构建在编译期 `#error`。编译期定义 `TLS_NO_GROUPS` 则 `CONN_TLS_SUPPORTS_GROUPS` 为 0，此时只要配了 `tls-groups` 就走「Failed to configure TLS groups: not supported by this build」并失败——注释说明这是有意的：不静默忽略这项策略。

**`configureServerSSLContext` 的服务端专属项。** 会话缓存两分支：`session_caching` 为真时 `SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER)`、`SSL_CTX_sess_set_cache_size(ctx, ctx_config->session_cache_size)`、`SSL_CTX_set_timeout(ctx, ctx_config->session_cache_timeout)`，再 `SSL_CTX_set_session_id_context(ctx, session_id_context, len)`（失败打「Failed to set TLS session context」并返回 `C_ERR`）；为假时直接 `SSL_SESS_CACHE_OFF`。三个配置项的默认值都能在 `src/config.c` 查到：`tls-session-caching` 默认 1、`tls-session-cache-size` 默认 `20*1024`、`tls-session-cache-timeout` 默认 300。紧接着按定义开关：`SSL_OP_NO_CLIENT_RENEGOTIATION`、`prefer_server_ciphers` 时的 `SSL_OP_CIPHER_SERVER_PREFERENCE`、`SSL_OP_SINGLE_DH_USE`，以及 OpenSSL 3.0 以前可用的 `SSL_CTX_set_ecdh_auto(ctx, 1)`。

DH 参数分两条路：配了 `dh_params_file` 时先 `fopen` 读文件（打不开就「Failed to load ...」返回 `C_ERR`）；OpenSSL 3.0 及以上用 `OSSL_DECODER_CTX_new_for_pkey(..., "PEM", NULL, "DH", OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS, ...)` + `OSSL_DECODER_from_fp` + `SSL_CTX_set0_tmp_dh_pkey`，注释点明此后 `pkey` 归 OpenSSL 所有所以不再 `free`；3.0 以前用 `PEM_read_DHparams` + `SSL_CTX_set_tmp_dh` + 显式 `DH_free`。没配 DH 参数文件时，3.0 及以上调 `SSL_CTX_set_dh_auto(ctx, 1)`。

**会话命名空间 `tlsSetSessionIdContext`。** 它接收 `SSL *` 与可选的 `names`，把 `SSL_get_verify_mode(ssl)` 和 `names` 拼成 `"redis:accept:%d:%s"`，做 `SHA256`，把摘要当作 `SSL_set_session_id_context` 的值。函数头有一大段理由，核心是：OpenSSL 只在完整握手时验证证书，会话恢复时直接复用当初存下的验证结果；而 `tls-auth-clients` 与 `tls-expected-peer-name` 都能在运行时改、又不重建上下文，所以弱校验下建立的会话会在强校验生效后被恢复，从而跳过校验。把「这条连接按什么方式校验」写进 session-ID context，OpenSSL 就只在命名空间相同者之间恢复会话。代码里有 `_Static_assert(SHA256_DIGEST_LENGTH <= SSL_MAX_SID_CTX_LENGTH, ...)` 与之配套，注释解释哈希的目的是把不设上限的名字列表压到 `SSL_MAX_SID_CTX_LENGTH` 允许的 32 字节，同时避免截断造成的策略前缀碰撞。`SHA256` 或 `SSL_set_session_id_context` 任一失败都按「fail closed」打 `LL_WARNING` 并返回 `C_ERR`（注释：调用方只打 verbose 级日志）。

**接受连接：`tlsAcceptHandler` 与 `connCreateAcceptedTLS`。** `tlsAcceptHandler` 是监听 fd 上的可读回调，每次最多收 `server.max_new_tls_conns_per_cycle` 条——该配置项在 `src/config.c` 用 `createUIntConfig` 注册，范围为 1 到 1000，默认 1。它构造 `TLSAcceptConfig accept_config = { .require_auth = server.tls_auth_clients, .is_cluster_bus = 0 }`（该结构定义在 `src/connection.h`，只有这两个字段），然后循环 `anetTcpAccept`：返回 `ANET_ERR` 时，若 `anetAcceptFailureNeedsRetry(errno)` 为真就 `continue` 再试一轮，否则只在 `errno != EWOULDBLOCK` 时打「Accepting client connection: %s」，然后 `return`。每条成功接受的 fd 交给 `acceptCommonHandler(connCreateAcceptedTLS(el, cfd, &accept_config), 0, cip)`。

集群总线是同一函数的另一个入口：`src/cluster_legacy.c` 的 `clusterAcceptHandler` 用 `MAX_CLUSTER_ACCEPTS_PER_CALL`（1000）作为本轮上限，构造的 `TLSAcceptConfig` 是 `.require_auth = TLS_CLIENT_AUTH_YES, .is_cluster_bus = 1`，再 `connCreateAccepted(server.el, connTypeOfCluster(), cfd, &accept_config)`。也就是说两条链路的「每轮接受上限」来自两个不同的常量/配置，只有客户端口那一侧受 `max-new-tls-connections-per-cycle` 控制。

`connCreateAcceptedTLS` 先断言 `!accept_config->is_cluster_bus || redis_tls_cluster_ctx`，再按 `is_cluster_bus` 二选一拿上下文（集群总线取 `redis_tls_cluster_ctx`，其余取 `redis_tls_ctx`），转交 `createTLSConnection(el, 0, ctx)`；后者只在 `accept_ctx` 为空、`client_side` 为真且 `redis_tls_client_ctx` 存在时才改用客户端上下文，并把 `conn->c.iovcnt` 设为 `IOV_MAX`、`conn->ssl` 设为 `SSL_new(ctx)`。随后把 fd、`c.state = CONN_STATE_ACCEPTING` 写上；`!conn->ssl` 时 `updateTLSError` 并置 `CONN_STATE_ERROR` 直接返回。

接下来按 `require_auth` 三值改写上下文级 verify 模式：

```c
switch (accept_config->require_auth) {
    case TLS_CLIENT_AUTH_NO:
        SSL_set_verify(conn->ssl, SSL_VERIFY_NONE, NULL);
        break;
    case TLS_CLIENT_AUTH_OPTIONAL:
        SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, NULL);
        break;
    default: /* TLS_CLIENT_AUTH_YES, also fall-secure */
        SSL_set_verify(conn->ssl, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        break;
}
```

`default` 收 `TLS_CLIENT_AUTH_YES` 之外的任何值，注释自称 fall-secure：不认识的值按最严处理。紧接 `tlsSetSessionIdContext(conn->ssl, NULL)`（此时还没有 expected names），失败同样落到 `CONN_STATE_ERROR`。最后 `SSL_set_fd` 与 `SSL_set_accept_state`。函数头注释强调此时 socket 还不能做 I/O，要等 `connAccept()` 走到 accept 回调。

**握手推进 `tlsHandleEvent`。** 事件统一入口是 `tlsEventHandler`（`CT_TLS.ae_handler`），它把 `clientData` 当 `tls_connection *` 转给 `tlsHandleEvent(conn, mask)`，按 `c.state` 分派：

- `CONN_STATE_CONNECTING`：先 `ERR_clear_error()`，再 `anetGetError(conn->c.fd)`；非零即 `last_errno = conn_error` 与 `CONN_STATE_ERROR`。否则若 `TLS_CONN_FLAG_FD_SET` 没置就 `SSL_set_fd` 并置位（只设一次），调 `SSL_connect`；返回值 `<= 0` 时进 `handleSSLReturnCode`，它返回 0（表示只是 WANT_READ/WANT_WRITE）就 `registerSSLEvent` 后**直接 return**——注释写明要绕开 `updateSSLEvent`，因为后者只看读写回调、不知道 `SSL_connect` 想要哪个方向的 I/O；否则置 `CONN_STATE_ERROR`。成功则置 `CONN_STATE_CONNECTED`。
- `CONN_STATE_ACCEPTING`：结构与上一支相同，只是 `ERR_clear_error` 之后调 `SSL_accept`。
- `CONNECTED`：先算两个布尔：

  ```c
  int call_read = ((mask & AE_READABLE) && conn->c.read_handler) ||
      ((mask & AE_WRITABLE) && (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE));
  int call_write = ((mask & AE_WRITABLE) && conn->c.write_handler) ||
      ((mask & AE_READABLE) && (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ));
  ```

  这两个表达式就是「逻辑读卡在想写、逻辑写卡在想读」的复原逻辑。`invert = conn->c.flags & CONN_FLAG_WRITE_BARRIER` 决定调用顺序：默认先读后写，barrier 置位时把读推迟到写之后（注释给的用例是 `beforeSleep` 里先 fsync 再回复）。每次调 handler 前先清掉对应的「想要反方向」标志。最后一段处理 SSL 内部已读未交付的数据：`mask & AE_READABLE` 且 `SSL_pending(conn->ssl) > 0` 就 `tlsPendingAdd(conn)`，否则若已在待处理链表就 `tlsPendingRemove(conn)`。

分支结束后还有一个收尾：`if (conn->c.el) updateSSLEvent(conn)`，注释写「The event loop may have been unbound during the event processing above」。

**待处理数据链。** 链表挂在事件循环的 `privdata[1]` 上（`tlsPendingAdd` 里 `if (!conn->c.el->privdata[1]) conn->c.el->privdata[1] = listCreate()`），每连接记录 `pending_list_node` 以免重复入表。`tlsHasPendingData` 只判断该链表长度是否大于零；`tlsProcessPendingData` 先记下 `listLength`，再循环「取首个节点 → `tlsPendingRemove` → `tlsHandleEvent(conn, AE_READABLE)`」，即把「socket 已读进 SSL 缓冲但还没交给上层」的连接当读事件重放一遍。这两个函数是 `CT_TLS.has_pending_data` / `process_pending_data`，被 `connTypeHasPendingData` / `connTypeProcessPendingData` 遍历调用；主循环 `beforeSleep` 先 `connTypeProcessPendingData(server.el)` 再算 `dont_sleep = connTypeHasPendingData(server.el) || ...`（注释：「Handle pending data(typical TLS). (must be done before flushAppendOnlyFile)」），I/O 线程的 `IOThreadBeforeSleep` 用自己那个 `el` 做同样两步。`processEventsWhileBlocked` 的捷径分支里也会调一次 `connTypeProcessPendingData` 并把返回值累加进 `processed`。

**读写路径。** `connTLSRead` / `connTLSWrite` 都要求 `c.state == CONN_STATE_CONNECTED`，先 `ERR_clear_error()`，再做 `SSL_read` / `SSL_write`，结果交给 `updateStateAfterSSLIO(conn, ret, 1)`。该函数先看 `errno == EINTR`：命中就 `last_errno = EINTR` 并返回 `-1`，注释解释此时不必走完整 OpenSSL 错误处理，交给调用方重试。否则 `ret <= 0` 时调 `handleSSLReturnCode` 拿 `ssl_err`：返回 0（WANT 类）就按方向置 `TLS_CONN_FLAG_WRITE_WANT_READ` 或 `TLS_CONN_FLAG_READ_WANT_WRITE`，必要时 `updateSSLEvent`，并把 `errno` 写成 `EAGAIN` 后返回 `-1`；`SSL_ERROR_ZERO_RETURN` 或 `(SSL_ERROR_SYSCALL && !errno)` 置 `CONN_STATE_CLOSED`，其余置 `CONN_STATE_ERROR`，两者都返回 `-1`。`handleSSLReturnCode` 内部对 `SSL_ERROR_SYSCALL` 记 `last_errno = errno` 并把 `strerror(errno)` 存进 `ssl_error`；对 `SSL_ERROR_ZERO_RETURN` 明确**不格式化错误串**（注释：此时 OpenSSL 错误队列为空，格式化只会得到无意义的 `error:00000000:lib(0)::reason(0)`）；其余情况走 `updateTLSError`，它 `zmalloc(512)` 一块缓冲给 `ERR_error_string_n`。

`connTLSRead` 比写入多一步收尾：`updateStateAfterSSLIO` 返回 `-1` 且 `c.state == CONN_STATE_CLOSED` 时返回 0，注释说明这是把对端 `close_notify` 的平滑关闭折算成「零长度读」，与普通 socket `read()` 返回 0 等价，让上层按干净 EOF 处理而不是按错误断开。

`connTLSWritev` 有一个明确的分叉点：它先累加各 `iov` 长度，累加时一旦超过 `NET_MAX_WRITES_PER_EVENT`（`src/server.h` 定义 `1024*64`）就 `break` 退出累加。累计值超阈值时逐个 `connTLSWrite`，注释说此时「不值得为了少几次系统调用做这么多内存拷贝」；不超过阈值才用 VLA `char buf[iov_bytes_len]` 拼成连续内存再一次写出。`iovcnt == 1` 直接退化成 `connTLSWrite`。

**身份校验。** `src/tlshelpers.h` 里的 `tlsVerifyParamAddIdentity` 是唯一落点：先 `X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS)`，若名字是 IP 字面量（`tlsIsIPAddressLiteral` 用 `inet_pton` 对 `AF_INET` / `AF_INET6` 各试一次）就 `X509_VERIFY_PARAM_set1_ip_asc`，否则按 `replace` 选 `set1_host`（替换）或 `add1_host`（追加，任一命中即可）。函数头注释记录了四条 OpenSSL 语义细节：IP 只对 IP SAN 生效、host 匹配器不查 IP SAN；OpenSSL 只保留**一个**期望 IP 且与 host 名同时设置时要求两者都匹配，所以接受列表的调用方不能混用两类名字；长度必须显式传（OpenSSL 把 0 当 `strlen`，BoringSSL 直接拒绝 0）；证书里的部分标签通配（`f*.example.com`）被拒、整标签通配（`*.example.com`）仍可匹配。

`tls.c` 这一侧有三个使用者：

- `tlsSetVerifyName(SSL *, const char *names)`：按空格切分 `names`，第一个 token 走 `replace`、后续走追加，空 token 跳过；全部成功且至少应用过一个才返回 `C_OK`，否则 `C_ERR`（注释：`first` 仍为真说明「全是空白」，同样 fail closed）。
- `tlsSetVerifyIdentity(SSL *, const char *name)`：单个名字，空名字直接 `C_ERR`，否则以 `replace` 应用一次。Sentinel 的 `instanceLinkNegotiateTLS` 用它校验每个被监视实例的地址。
- `connTLSSetVerifyName`（静态，`CT_TLS.set_verify_name`）：给**已创建**的接受侧连接设名字，设完还必须用同一个 `name` 重新推一次 `tlsSetSessionIdContext` —— 注释解释名字属于「这条连接怎么校验」的一部分，旧会话的命名空间对不上，就不能被恢复。

出站方向由 `tlsApplyExpectedPeerName` 统一处理：取 `server.tls_ctx_config.expected_peer_name`，为空即 `C_OK`，否则 `tlsSetVerifyName`，失败时打「Failed to set expected TLS peer name for outbound connection to %s」、置 `CONN_STATE_ERROR` 并返回 `C_ERR`。它的两个调用方是 `connTLSConnect`（非阻塞）与 `connTLSBlockingConnect`（阻塞，注释点明这条路径供 MIGRATE 使用），保证「复制、集群总线、MIGRATE」三类出站连接都按同一份配置校验身份。

编译开关层面的降级路径是明确的：`CONN_TLS_SUPPORTS_VERIFY_NAME` 在 `TLS_NO_PEER_NAME_VERIFICATION` 下为 0，或 OpenSSL 版本低于 1.0.2 且未定义该开关时直接 `#error`。为 0 时 `tlsSetVerifyName` 与 `tlsSetVerifyIdentity` 都改为「打 `LL_WARNING` 说明本构建不强制该项、返回 `C_OK`」——于是 `connTLSSetVerifyName`（集群总线接受侧）与 `tlsApplyExpectedPeerName`（出站侧）都会放行连接，剩下只有 CA 链校验；`sentinel.c` 里对应分支的注释也写明「tlsSetVerifyName()/tlsSetVerifyIdentity() have warned and set no expectation」。这是「告警后继续」而非「拒绝」。

反向的一条校验更早：`src/config.c` 的 `isValidTlsExpectedPeerName` 在配置期就拒掉不合法的值——空串放行（表示关闭），含空格以外的空白字符报错，全是空格报「contains no usable name」，以及任何 IP 字面量都报「tls-expected-peer-name accepts DNS names only」（理由见函数注释：IP 校验是 Sentinel 的 `tls-verify-hostnames` 与工具默认身份检查的职责）。另有 `applyTlsExpectedPeerName` 只调 `tlsWarnExpectedPeerNameScope` 打一条「有 N 个条目以点开头」的告警，注释说明以点开头的名字在 OpenSSL 里是父域模式（`.example.com` 也接受 `a.b.example.com`、但不匹配域名本身），且该选项**不进** `SSL_CTX`，是逐连接生效的，所以这个回调刻意不触发重配。

**关闭与向 TCP 的回退。** `connTLSShutdown` 与 `connTLSClose` 都先处置 SSL 对象：`c.state == CONN_STATE_CONNECTED` 时 `SSL_shutdown(conn->ssl)`，然后 `SSL_free` 并把指针清 NULL；`connTLSClose` 另外释放 `ssl_error`、从待处理链表摘除，最后一步是 `connectionTypeTcp()->close(conn_)`（`connTLSShutdown` 对应 `connectionTypeTcp()->shutdown(conn_)`）。也就是说 TLS 连接在关 fd 这一环上不自己实现，直接回退到 TCP 类型的方法。同样是回退而非自研的还有三处：`connTLSAddr` 走 `anetFdToString`、`connTLSIsLocal` 走 `connectionTypeTcp()->is_local(conn)`、`connTLSListen` 走 `listenToPort(listener)`。

`tlsCleanup` 是 `CT_TLS.cleanup`，由 `connTypeCleanupAll` 遍历调用（`sentinel.c` 在 fork 出的脚本子进程里调它，注释位于 `sentinel.c` 的子进程分支）。它按顺序 `SSL_CTX_free` 三个全局上下文并逐一清 NULL，最后在 OpenSSL 1.1.0 及以上且非 LibreSSL 的构建里调 `OPENSSL_cleanup()`（注释：「unavailable on LibreSSL」）。

**配置重写的入口。** `tlsConfigure` 的三个真实调用点都在 `connTypeConfigure` 上：`initListeners`（启动、`reconfigure = 1`，条件是 `server.tls_port || server.tls_replication || server.tls_cluster`）、`applyTlsCfg`（`reconfigure = 1`，同一触发条件）、`applyTLSPort`（`reconfigure = 0`）。`applyTLSPort` 建完上下文后还要 `listenerByType(CONN_TYPE_TLS)`、写 `port` 与 `ct`、`clusterUpdateMyselfAnnouncedPorts()`，再 `changeListener(listener)`，失败回「Unable to listen on this port. Check server logs.」。`tls-cluster` 的 apply 回调 `applyTlsCluster` 先 `clusterBusPortProtectionUnmet()`（判据是 `server.cluster_enabled && server.cluster_bus_port_protected_mode && !server.tls_cluster`），不满足就拒；若 `server.tls_cluster == prev_tls_cluster` 则提前返回成功，不重建上下文也不通知模块。`applyTlsCfg` 自己的触发条件是 `server.tls_port || server.tls_replication || server.tls_cluster`，都不满足时**不碰 TLS**，直接返回成功。

**模块形态。** 在 `BUILD_TLS_MODULE == 2` 的构建里 `tls.c` 还实现 `RedisModule_OnLoad`：先 `strcmp(REDIS_BUILD_ID_RAW, redisBuildIdRaw())`，不一致就报「Connection type %s was not built together with the redis-server used.」并返回 `REDISMODULE_ERR`；再 `RedisModule_Init(ctx,"tls",1,REDISMODULE_APIVER_1)`；再检查 `RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_SERVER_STARTUP`，不在启动期就报「Connection type %s can be loaded only during bootup」；`RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD)`；最后 `connTypeRegister(&CT_TLS)`。`RedisModule_OnUnload` 恒返回 `REDISMODULE_ERR` 并打「Connection type %s can not be unloaded」。

**身份到用户名。** `tlsGetPeerUsername` 先 `SSL_is_init_finished(conn->ssl)` 才继续；仅当 `server.tls_ctx_config.client_auth_user == TLS_CLIENT_FIELD_CN` 时取字段名 `"CN"`（枚举另一值是 `TLS_CLIENT_FIELD_OFF`，其它值直接返回 NULL）；随后 `SSL_get_peer_certificate`，失败返回 NULL；`getCertFieldByName` 只映射 `CN` → `NID_commonName`、`O` → `NID_organizationName`，用 `X509_NAME_get_index_by_NID` 取首个条目、`ASN1_STRING_to_UTF8` 解码后 `sdstrynewlen` 造出二进制安全的 sds（注释：名字里含内嵌 NUL，所以用返回长度而非 `strlen`）。该返回值唯一的消费点是 `src/networking.c` 的 `clientAcceptHandler`：拿到名字就 `ACLGetUserByName`，用户存在且未禁用在位就 `c->authenticated = 1` 并 `clientSetUser`、`moduleNotifyUserChanged`，否则 `addACLLogEntry(c, ACL_INVALID_TLS_CERT_AUTH, ...)`。权限规则本身归 k01。

## 关键取舍

**源码事实：** `tlsConfigure` 的三份上下文先全建好再一次性换指针，失败时旧上下文不动——`CONFIG SET` 被拒不会让正在服务的连接失去上下文。**源码事实：** 每个 `SSL_CTX` 的 verify 默认是 `SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT`，真正按 `tls-auth-clients` 放宽到 `NO` 或 `OPTIONAL` 发生在 `connCreateAcceptedTLS` 里，逐连接 `SSL_set_verify` 改写。**源码事实：** 会话命名空间把 verify 模式与 expected names 的 SHA256 编进去，所以策略变更后旧会话无法恢复。**源码事实：** `!reconfigure && redis_tls_ctx` 直接返回成功，这是 `applyTLSPort` 与 `applyTlsCfg` 行为不同的根源。**源码事实：** `tls-groups` 在无对应 OpenSSL API 的构建里让配置失败（`#error` 或运行期报错），不静默忽略。**源码事实：** `max-new-tls-connections-per-cycle` 默认 1（可配到 1000），集群总线侧用编译常量 `MAX_CLUSTER_ACCEPTS_PER_CALL`（1000）。**源码事实：** 编译掉身份校验的构建只告警不拒绝，`tlsSetVerifyName` / `tlsSetVerifyIdentity` 都返回 `C_OK`。**源码事实：** 平滑关闭（`SSL_ERROR_ZERO_RETURN`）在 `connTLSRead` 里折算成返回值 0。**源码事实：** 关 fd 与地址/本地判断都回退到 TCP 类型的方法。

**架构推论：** 把 TLS 做成 `ConnectionType` 的一张表，等于让事件循环、客户端生命周期、阻塞客户端、复制等上层代码完全不知道加密存在——它们只调 `connRead`/`connWrite`/`connAccept`。代价是任何「SSL 想要的 I/O 方向」与「上层逻辑想要的 I/O 方向」不一致的情况都得在这一层显式编码，`TLS_CONN_FLAG_READ_WANT_WRITE` / `TLS_CONN_FLAG_WRITE_WANT_READ` 两个标志就是这笔账的记账凭证；换成让上层感知 TLS，这层状态会散落到每个调用点。**架构推论：** `privdata[1]` 挂待处理链表，是把「TLS 缓冲里还有已解密数据」这一 SSL 私有状态借事件循环的数据槽中转，避免了在连接对象上再加一个全局索引；副作用是这链表按事件循环实例分片——主线程与每个 I/O 线程各自一份，正好与各线程互不共享连接的事实对齐。**架构推论：** 身份校验放在「配置期拒非法值 + 连接期逐条绑定 + 会话命名空间隔离」三处，而不是只做握手时的一次性检查，说明设计上把「策略能在运行时改」当成硬约束；如果策略只在启动时生效，会话恢复的绕过问题根本不存在，也不需要 SHA256 那一步。**架构推论：** `tlsPasswordCallback` 只做长度检查与 memcpy，说明私钥口令的保密性完全交给进程内存与 `SENSITIVE_CONFIG` 标记（`src/config.c` 里 `tls-key-file-pass` 带该标记），TLS 层不额外抹零——这与「口令来自配置文件、生命周期等于进程」的假设一致。**架构推论：** `default` 分支收掉 `TLS_CLIENT_AUTH_YES` 之外的任何值并施加最严校验，是典型的「枚举扩展时安全默认」写法：将来加新枚举值忘了改 switch，后果是校验变严而不是变松。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/tls.c` | `tlsInit` | OpenSSL 全局初始化：按版本三分支 `OPENSSL_config` / `OPENSSL_init_crypto`，`USE_CRYPTO_LOCKS` 下的 `initCryptoLocks`，`RAND_poll` 失败告警 |
| `src/tls.c` | `tlsConfigure` | 配置入口与原子替换：四道校验、`parseProtocolsConfig`、建三份上下文、成功后换指针 |
| `src/tls.c` | `createSSLContext` | 建基座 `SSL_CTX`：协议开关、verify 默认、口令回调、证书/私钥/CA/密码套件/`tls-groups` 加载 |
| `src/tls.c` | `configureServerSSLContext` | 服务端专属项：会话缓存三参数与 session-ID context、renegotiation、server cipher preference、DH 参数两条版本路径 |
| `src/tls.c` | `parseProtocolsConfig` | `tls-protocols` 字符串解析与 `REDIS_TLS_PROTO_DEFAULT` 回退 |
| `src/tls.c` | `tlsSetSessionIdContext` | 把 verify 模式与 expected names 的 SHA256 写进会话命名空间，`SSL_MAX_SID_CTX_LENGTH` 静态断言 |
| `src/tls.c` | `tlsAcceptHandler` | 客户端口接受循环：`server.max_new_tls_conns_per_cycle` 上限、`anetAcceptFailureNeedsRetry` 重试、`acceptCommonHandler` 转交 |
| `src/tls.c` | `connCreateAcceptedTLS` | 按 `is_cluster_bus` 选上下文、按 `require_auth` 三值改写 verify、设 session-ID context 与 accept 状态 |
| `src/tls.c` | `createTLSConnection` / `connCreateTLS` | `SSL_new` 建连接对象，出站侧优先用 `redis_tls_client_ctx` |
| `src/tls.c` | `tlsHandleEvent` / `tlsEventHandler` | 状态机：`CONNECTING` / `ACCEPTING` / `CONNECTED` 三支，WANT 标志复原、write barrier、`SSL_pending` 入表 |
| `src/tls.c` | `handleSSLReturnCode` / `updateStateAfterSSLIO` | OpenSSL 返回码到连接状态的映射：WANT 类、SYSCALL、ZERO_RETURN、EINTR |
| `src/tls.c` | `updateSSLEvent` / `registerSSLEvent` | 按读回调/写回调与 WANT 标志增删 `AE_READABLE` / `AE_WRITABLE` |
| `src/tls.c` | `tlsPendingAdd` / `tlsPendingRemove` / `tlsHasPendingData` / `tlsProcessPendingData` | `el->privdata[1]` 上的待处理连接链表 |
| `src/tls.c` | `connTLSRead` / `connTLSWrite` / `connTLSWritev` | 读写封装；`NET_MAX_WRITES_PER_EVENT` 阈值的 writev 分叉；graceful close 折算成 0 |
| `src/tls.c` | `connTLSConnect` / `connTLSBlockingConnect` | 出站握手：SNI（非 IP 字面量才设）、身份绑定、TCP connect；非阻塞路径在建 SSL 前绑名字，阻塞路径在 TCP 连上后绑名字并加 send/recv 超时 |
| `src/tls.c` | `tlsSetVerifyName` / `tlsSetVerifyIdentity` / `connTLSSetVerifyName` / `tlsApplyExpectedPeerName` | 对端身份绑定与降级告警路径 |
| `src/tls.c` | `tlsGetPeerUsername` / `getCertFieldByName` | 从证书取 `CN` / `O` 造二进制安全 sds |
| `src/tls.c` | `connTLSClose` / `connTLSShutdown` / `tlsCleanup` | SSL 对象释放后回退 `connectionTypeTcp()` 的 close/shutdown；`tlsCleanup` 释放三个全局上下文 |
| `src/tls.c` | `CT_TLS` / `RedisRegisterConnectionTypeTLS` / `RedisModule_OnLoad` | 连接类型函数表、注册入口、模块构建下的加载校验 |
| `src/tlshelpers.h` | `tlsVerifyParamAddIdentity` / `tlsIsIPAddressLiteral` / `CONN_TLS_SUPPORTS_VERIFY_NAME` | 共用身份校验：IP 与 DNS 两条 matcher、通配符 flags、编译开关 |
| `src/connection.h` | `ConnectionType` / `TLSAcceptConfig` / `connTypeConfigure` / `connSetVerifyName` / `CONN_TYPE_TLS` / `CONN_TYPE_MAX` | 连接抽象、接受配置结构、配置与校验名字的转发 |
| `src/connection.c` | `connTypeRegister` / `connTypeInitialize` / `connectionTypeTls` / `connTypeProcessPendingData` / `connTypeHasPendingData` | 类型注册与 `init` 调用、NULL 缓存、pending 链的遍历 |
| `src/connhelpers.h` | `callHandler` | 引用计数保护下调用回调，`CONN_FLAG_CLOSE_SCHEDULED` 时延迟 close |
| `src/server.h` | `redisTLSContextConfig` / `TLS_CLIENT_AUTH_NO` / `TLS_CLIENT_AUTH_YES` / `TLS_CLIENT_AUTH_OPTIONAL` / `TLS_CLIENT_FIELD_CN` / `NET_MAX_WRITES_PER_EVENT` | 配置结构、认证模式与证书字段常量、单次写出上限 `1024*64` |
| `src/config.c` | `applyTlsCfg` / `applyTLSPort` / `applyTlsCluster` / `isValidTlsExpectedPeerName` / `applyTlsExpectedPeerName` / `tlsWarnExpectedPeerNameScope` / `clusterBusPortProtectionUnmet` | 各 TLS 配置项的 apply 回调、expected name 校验与父域告警 |
| `src/server.c` | `initListeners` / `beforeSleep` | 启动期 `connTypeConfigure(..., 1)` 与失败 `exit(1)`；睡前 pending 处理与 `dont_sleep` |
| `src/cluster_legacy.c` | `clusterAcceptHandler` | 集群总线接受：`require_auth = TLS_CLIENT_AUTH_YES`、`is_cluster_bus = 1`、`connSetVerifyName` |
| `src/networking.c` | `acceptCommonHandler` / `clientAcceptHandler` | 接受后进行准入与客户端创建；证书用户名到 ACL 用户的自动认证 |
| `src/sentinel.c` | `instanceLinkNegotiateTLS` / `connTypeCleanupAll` 调用点 | Sentinel 复用 `redis_tls_ctx` 建 `SSL` 并做身份校验；脚本子进程清理连接类型 |

## 与主核 / AI 的关系

**架构推论：** 这份实现最值得主核（`engine/db/`）借鉴的是「加密作为连接类型表的一行」这个分层：上层只认 read/write/accept，加密、握手、身份校验全部留在类型内部。主核若将来加传输加密，照这个形状做能避免把 TLS 状态渗进会话管理与执行路径；但需要比 Redis 多做一件事——Redis 的 `tlsInit` / `tlsCleanup` 是进程级单例，主核是多写者引擎，`SSL_CTX` 的构建与替换需要与连接创建同步，`tlsConfigure` 那种「先全建后换指针」的原子替换正好是可复用的模式，但换指针那一步必须加同步。

**架构推论：** 「会话命名空间 = 校验策略的哈希」解决的是策略热变更与会话恢复复用旧验证结果的冲突。这对 AI 场景有直接映射：推理会话若允许跨策略复用（例如从「只校验调用方身份」放宽或收紧到「校验模型版本 + 租户」），同样会出现旧结果被复用的问题；把策略摘要编进会话键是通用解法，而不是 TLS 专属技巧。

**架构推论：** 三个明确可搬的细节：其一，`SSL_error` 与 `ZERO_RETURN` 的区分（把平滑关闭折算成零长度读）——AI 流式输出的收尾同样需要「正常结束」与「传输失败」两种终止语义，不能都当成错误断开；其二，`max-new-tls-connections-per-cycle` 默认 1 的节奏控制——每轮事件循环只接受一条新连接，把 TLS 握手（最贵的 CPU 操作）摊到多轮，避免一轮事件循环被握手打满，这对并发推理网关的准入节奏是同类约束；其三，编译期把身份校验裁掉时选择「告警后继续」而不是拒绝，体现了「宁可降级也不整体不可用」的取向——但 AI 场景不应照抄：身份校验缺失在有租户隔离要求时是致命降级，这时应该反过来按 fail-closed 处理。

## 参考

- [Redis 文档：TLS](https://redis.io/docs/latest/operate/oss_and_stack/management/security/encryption/) —— `BUILD_TLS=yes` 编译、证书配置、`tls-port` / `tls-auth-clients` / `tls-replication` / `tls-cluster` 的官方口径。
- [Redis 源码：src/tls.c](https://github.com/redis/redis/blob/unstable/src/tls.c) —— `tlsInit`、`tlsConfigure`、`CT_TLS` 与全部连接级函数的真实定义。
- [Redis 源码：src/connhelpers.h](https://github.com/redis/redis/blob/unstable/src/connhelpers.h) —— `callHandler` 的引用计数与延迟关闭语义，`tlsHandleEvent` 调用上层回调时的保护基础。
- [Redis 源码：src/connection.h](https://github.com/redis/redis/blob/unstable/src/connection.h) —— `ConnectionType` 表、`TLSAcceptConfig` 与 `connTypeConfigure` / `connSetVerifyName` 的转发约定。
- [Redis 源码：src/tlshelpers.h](https://github.com/redis/redis/blob/unstable/src/tlshelpers.h) —— `tlsVerifyParamAddIdentity` 与 `CONN_TLS_SUPPORTS_VERIFY_NAME` 编译开关。
