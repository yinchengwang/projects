---
project: postgres
feature: auth-methods
page_type: feature
schema_version: 1
refs:
  - path: src/backend/libpq/auth.c
    symbols: [ClientAuthentication, sendAuthRequest, CheckPasswordAuth]
  - path: src/backend/libpq/auth-scram.c
    symbols: [scram_exchange]
---

# 认证方法

## 一句话职责

服务端按 pg_hba.conf 选定的方法向客户端发送认证请求，并用口令散列、SASL 交换或外部系统校验连接身份。

## 机制是什么

认证发生在连接建立、数据库与用户已知之后：`ClientAuthentication` 从 `Port` 取已匹配的 HBA 记录，按其中的 `auth_method` 分派到具体校验例程。方法覆盖口令类（明文口令、MD5 挑战、SCRAM-SHA-256）、外部系统类（PAM、BSD auth、LDAP）、基于传输层的证书校验，以及 trust/reject 这类不做凭据校验的条目。

本机制只负责“这个连接声明的用户是否通过身份校验”，不负责 HBA 规则的解析与匹配顺序（那是 `hba.c` 的职责）、不负责角色权限与 ACL 判定、也不负责会话建立后的对象访问控制。证书链本身的校验发生在 TLS 握手阶段；本机制只消费其结论并把 HBA 的 `clientcert` 选项语义落实为通过或致命错误。

另一条边界是“凭据存储”：用户口令散列的生成与 `pg_authid` 中的格式由口令管理路径维护，认证侧只做读取与比对。也正因如此，认证失败的具体原因不会原样回传给客户端，源码把这一约束贯穿到各方法内部以避免泄露角色是否存在。

## 怎么实现的

1. `ClientAuthentication` 先调 `hba_getauthmethod(port)` 取得本连接匹配的 HBA 记录（文件已在别处解析过），随后处理 `clientcert` 前置条件：若要求校验客户端证书但未加载根证书库，直接 `FATAL`；若已加载但 `peer_cert_valid` 为假，也 `FATAL`，因为握手阶段本该已中止该连接。
2. 按 `auth_method` 分派。`uaReject` 与 `uaImplicitReject` 直接以 FATAL 报错，并带上加密状态描述（GSS 加密、SSL 加密或无加密）；`uaTrust` 与 `uaCert` 走 `STATUS_OK`；`uaPeer`/`uaIdent`/`uaPAM`/`uaBSD`/`uaLDAP` 各有独立实现；`uaMD5` 与 `uaSCRAM` 共用 `CheckPWChallengeAuth` 按用户实际口令类型再分流；`uaPassword` 走 `CheckPasswordAuth`。
3. `sendAuthRequest` 是所有方法的报文出口：组装 `PqMsg_AuthenticationRequest`、写入 4 字节方法号与可选附加数据，然后 `pq_flush`。例外是 `AUTH_REQ_OK` 与 `AUTH_REQ_SASL_FIN`——这两个不立即 flush，源码注释说明它们要等到服务端准备接受查询时才一起发出。
4. 明文口令路径 `CheckPasswordAuth` 先发 `AUTH_REQ_PASSWORD`，用 `recv_password_packet` 读应答。该函数校验报文类型为 `PqMsg_PasswordMessage`、检查包长度与字符串长度一致、拒绝空口令，并且不对口令做任何字符集转换，也不把口令写入日志。
5. `CheckPasswordAuth` 用 `get_role_password` 取散列，交给 `plain_crypt_verify` 比对；若散列是 MD5 格式则额外排一条弃用告警，成功后调用 `set_authn_id` 记录认证身份。注意这个方法本身接受明文传输，安全性取决于传输层是否加密。
6. MD5/SCRAM 路径由 `CheckPWChallengeAuth` 统一入口：先取散列并据其类型（或用户不存在时退化为 `Password_encryption` 配置）决定实际执行哪种挑战。只有在 HBA 允许 md5 且用户确实持有 MD5 散列时才用 `CheckMD5Auth`（随机 4 字节 salt 随请求下发），其余一律走 SCRAM。用户不存在或口令过期时仍完整走一遍流程，只为不让客户端区分“用户不存在”和“口令错误”。
7. SCRAM 的交互核心是 `scram_exchange`，由 SASL 机制框架按消息驱动：客户端未在 `SASLInitialResponse` 里附带初始响应时，服务端先回一个空挑战；随后 `SCRAM_AUTH_INIT` 解析 client-first 并构造 server-first（含 salt、迭代数与 nonce），状态推进到 `SCRAM_AUTH_SALT_SENT`；下一轮解析 client-final、校验 nonce 与 client proof，成功则构造 server-final 并置 `SCRAM_AUTH_FINISHED`。
8. 空消息、长度与串长不一致、nonce 不匹配都按协议违规抛错。源码注释强调 client proof 的计算即使在“注定失败”的 mock 认证中也要照做，用于削弱通过耗时判断角色是否存在的时序攻击。成功时把 `ClientKey`/`ServerKey` 抄入 `MyProcPort` 并置 `has_scram_keys`，供后续通道绑定使用。
9. 分派结束后还有两处统一收尾：若状态成功且 HBA 要求 `clientcert=verify-full`（或方法本身是 cert），再调 `CheckCertAuth` 做主体名匹配；随后按 `log_connections` 记录认证方法与 HBA 来源行，最后成功发 `AUTH_REQ_OK`，失败走 `auth_failed`。SASL 的 abandoned 状态用 `FATAL_CLIENT_ONLY`，以便不产生服务端日志。

## 关键取舍

- 源码事实：`sendAuthRequest` 对 `AUTH_REQ_OK` 与 `AUTH_REQ_SASL_FIN` 不 `pq_flush`，其它方法号立即 flush。放弃“一律立即 flush”换取把认证成功消息与后续就绪消息合并，减少一次网络往返；代价是这两个方法号的消息可能延迟到服务端准备就绪时才可见。
- 源码事实：`CheckPWChallengeAuth` 在 `get_role_password` 返回空时仍按 `Password_encryption` 虚构一种口令类型并完整执行认证流程，SCRAM 侧则通过 `doomed` 标记与照常计算 proof 来收尾。放弃“用户不存在就立刻失败”换取客户端无法从行为差异推断角色存在性，代价是这类连接会多消耗一次完整的散列与交换开销。
- 源码事实：`ClientAuthentication` 把 `uaCert` 与 `uaTrust` 归到同一个成功分支，再在收尾处按 `clientcert == clientCertFull` 或 `auth_method == uaCert` 触发 `CheckCertAuth`。放弃在分派处直接返回证书校验结果，换取“证书校验只在需要时发生”的集中判断；代价是证书方法的语义分散在两处，阅读时必须看完整函数才知全貌。
- 源码事实：`recv_password_packet` 拒绝空口令（`buf.len == 1`）并在包长与串长不一致时报协议违规。源码注释说明 MD5/SCRAM 本来也依赖建角色时禁止空口令，但仍保留此处检查，是为了覆盖 PAM、LDAP 这类把口令交给外部系统的方法；放弃“只信任存储侧约束”换取外部认证方法也有一致的最低门槛。
- 架构推论：认证方法的分派集中在一个 switch 上，使新增方法必须同时处理“报文如何发、凭据如何收、失败如何记”三件事。若改为每个方法独立注册表，仍需保证 abandoned/EOF 这类跨方法状态不被漏处理，因此当前形态更利于统一收尾；这一点在源码中没有显式论证。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/libpq/auth.c` | `ClientAuthentication` | 取 HBA 方法、前置校验客户端证书、按方法分派并统一收尾认证结果 |
| `src/backend/libpq/auth.c` | `sendAuthRequest` | 构造并发送认证请求报文，按方法号决定是否立即 flush |
| `src/backend/libpq/auth.c` | `CheckPasswordAuth` | 明文口令方法的请求、接收、散列比对与成功后记录认证身份 |
| `src/backend/libpq/auth-scram.c` | `scram_exchange` | 按状态推进 SCRAM 的 client-first/client-final 交换并校验 proof |

## 参考

- [PostgreSQL 文档：pg_hba.conf 与认证方法](https://www.postgresql.org/docs/current/auth-pg-hba-conf.html) —— 支撑 HBA 方法分派与 clientcert 选项语义。
- [PostgreSQL 文档：口令认证与 SCRAM](https://www.postgresql.org/docs/current/auth-password.html) —— 支撑 MD5/SCRAM 口令类型与挑战流程背景。
- [RFC 5802：SCRAM-SHA-256](https://datatracker.ietf.org/doc/html/rfc5802) —— 支撑 nonce、salt、迭代数与 client proof 的协议含义。
- [PostgreSQL 官方源码：服务端认证](https://github.com/postgres/postgres/blob/master/src/backend/libpq/auth.c) —— 支撑方法分派、报文发送与口令校验细节。
- [PostgreSQL 官方源码：SCRAM 实现](https://github.com/postgres/postgres/blob/master/src/backend/libpq/auth-scram.c) —— 支撑交换状态机与 proof 校验顺序。

## 与主核 / AI 的关系

暂无直接对应。主核若要接入认证，可借鉴“传输层结论与凭据校验分离”“失败原因统一收敛”“不因用户不存在而改变行为”的原则；但具体 HBA 匹配、SASL 机制接口与 key 存储位置需按主核的连接建立流程自行设计。
