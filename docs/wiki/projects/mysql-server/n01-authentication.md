---
project: mysql-server
feature: authentication
page_type: feature
schema_version: 1
refs:
  - path: sql/auth/sql_authentication.cc
    symbols: [acl_authenticate]
  - path: sql/sql_connect.cc
    symbols: [acl_authenticate]
---

# 认证插件

## 一句话职责

在连接建立阶段（`COM_CONNECT`）或连接中途切换用户时（`COM_CHANGE_USER`），驱动认证插件完成「用户名 + 口令」的校验流程，并把校验结果写回线程的安全上下文。

## 机制是什么

认证是一台**插件化的口令校验流水线**。服务端不直接比较口令，而是把「怎么比」这件事交给认证插件（`st_mysql_auth` 结构里的 `authenticate_user` 函数指针），自己只负责编排：选插件、握手、必要时换插件、补多因子、记账、落错误。

骨架由 `acl_authenticate(THD *thd, enum_server_command command)` 承担，它在 `sql/auth/sql_authentication.cc` 中定义，入口断言 `assert(command == COM_CONNECT || command == COM_CHANGE_USER);`——只有这两种命令会走认证。

流程可以拆成四段：

1. **准备。** `server_mpvio_initialize()` 把通信层状态清零（`memset(mpvio, 0, sizeof(MPVIO_EXT))`），装上 `server_mpvio_read_packet` / `server_mpvio_write_packet` / `server_mpvio_info` 三个回调，并把 `mpvio->status` 置为 `MPVIO_EXT::FAILURE`、`mpvio->auth_info.password_used` 置为 `PASSWORD_USED_NO`。同时 `thd->reset_db(NULL_CSTR)` 清掉当前库（源码注释说明原因：连接失败时不能误释放一个悬垂指针）。

2. **握手与取用户名。** `COM_CONNECT` 走 `do_auth_once(thd, auth_plugin_name, &mpvio)`，`auth_plugin_name` 初值是 `default_auth_plugin_name`；`COM_CHANGE_USER` 走 `parse_com_change_user_packet()` 解析包体。

3. **换插件（RESTART）。** 若 `mpvio.status == MPVIO_EXT::RESTART`，说明拿到用户名后发现该账号用的不是默认插件，于是把 `auth_plugin_name` 换成 `mpvio.acl_user->plugin`，**再调一次 `do_auth_once()`**。这是「服务端知道该账号用什么插件，但客户端未必知道」时的第二次握手。

4. **多因子与收尾。** `res == CR_OK` 时调 `do_multi_factor_auth(thd, &mpvio)` 做第二、第三因子的校验，随后 `server_mpvio_update_thd()` 把结果写回 THD，`check_and_update_password_lock_state()` 记账并处理口令锁定。

**边界（与相邻机制的分界）：**

- 不负责权限判定——`sctx->set_master_access(acl_user->access, ...)` 只是把权限位搬进安全上下文，真正「这次操作是否被授权」是 `sql/auth/sql_authorization.cc` 的 `check_access`（见 `n02-privilege-check` 页）。
- 不负责连接建立本身——TCP/Unix socket 建立、主机名解析、IP 黑名单检查属于 `sql/sql_connect.cc` 的 `check_connection()`（见 `a01-connection-dispatch` 页）。
- 不负责口令的**存储**格式——只有 `mysql_native_password` 与 `sha256_password` 这两个内置插件带 `AUTH_FLAG_USES_INTERNAL_STORAGE` 标志，存储格式由各插件自己的 `generate_*` / `validate_*` 函数决定。

## 怎么实现的

### 入口：`sql/sql_connect.cc` 的调用点

`sql/sql_connect.cc` 中 `acl_authenticate` 出现在 `check_connection()` 内的一处调用：

```c
auth_rc = acl_authenticate(thd, COM_CONNECT);
```

它被夹在两个审计事件之间——调用前是 `mysql_audit_notify(thd, AUDIT_EVENT(MYSQL_AUDIT_CONNECTION_PRE_AUTHENTICATE))`，调用后是 `mysql_audit_notify(thd, AUDIT_EVENT(MYSQL_AUDIT_CONNECTION_CONNECT))`。这两个事件任一返回非零，`check_connection()` 立刻 `return 1`。

调用前后还有三段固定动作：

- 之前：`thd->get_protocol_classic()->get_output_packet()->alloc(thd->variables.net_buffer_length)` 分配输出缓冲；分配失败时递增 `connection_errors_internal` 并返回 1（源码注释说明这里按全局计数而非按 IP 计数，因为 `net_buffer_length` 在连接阶段取的是 GLOBAL 值、不是 per-client 值）。
- 之后：`if (auth_rc == 0 && connect_errors != 0)` 则调 `reset_host_connect_errors(thd->m_main_security_ctx.ip().str)`——本次连接成功就把该 IP 之前的失败计数清零。
- 最后：`thd->set_ssl(net->vio);`，源码注释说明此时 SSL 信息才可用，才能对外公布 SSL 状态变量。

`check_connection()` 由 `login_connection()` 调用，后者先 `thd->get_protocol_classic()->set_read_timeout(connect_timeout, true)` 与 `set_write_timeout(connect_timeout)`——**认证阶段用的是 `connect_timeout` 而不是 `net_read_timeout`**；认证结束后 `login_connection()` 才把超时换回 `thd->variables.net_read_timeout` 与 `thd->variables.net_write_timeout`。

`COM_CHANGE_USER` 这条路径的调用方在 `sql/sql_parse.cc` 的 `dispatch_command()` 里（`a01-connection-dispatch` 页记录了该分支的失败处理：报 `ER_ACCESS_DENIED_CHANGE_USER_ERROR` 并置 `thd->killed = THD::KILL_CONNECTION`）。

### 主体：`acl_authenticate()` 的骨架

`acl_authenticate()` 的实际顺序如下：

1. `LEX_CSTRING auth_plugin_name = default_auth_plugin_name;`——默认插件名来自 `set_default_auth_plugin()` 设置的全局量。该设置函数只接受三个内置插件名：`PLUGIN_SHA256_PASSWORD`、`PLUGIN_MYSQL_NATIVE_PASSWORD`、`PLUGIN_CACHING_SHA2_PASSWORD`，其它名字返回 1（拒绝）。若默认值取的是 `PLUGIN_CACHING_SHA2_PASSWORD`，`set_default_auth_plugin()` 会打一条 `ER_DEPRECATE_MSG_WITH_REPLACEMENT` 的警告，提示改用 `authentication_policy`。
2. `static_assert(MYSQL_USERNAME_LENGTH == USERNAME_LENGTH, "");`——用户名长度上限在两套常量间保持一致。
3. `server_mpvio_initialize(thd, &mpvio, &charset_adapter);` 初始化通信层。
4. `thd->reset_db(NULL_CSTR);` 清当前库。
5. `thd->get_protocol_classic()->get_net()->read_pos = thd->get_protocol_classic()->get_raw_packet();`——源码注释写明「`acl_authenticate()` takes the data from net->read_pos」，认证读的是当前原始包。
6. **分叉：**

   - `COM_CHANGE_USER`：`mpvio.packets_written++`（假装已发过握手包）、`mpvio.packets_read++`（把 COM_CHANGE_USER 包计入），`thd->set_user_connect(nullptr)` 清掉已分配的用户连接变量，然后调 `parse_com_change_user_packet(thd, &mpvio, mpvio.protocol->get_packet_length())`。该函数返回 true（解析失败）时，调 `login_failed_error(thd, &mpvio, mpvio.auth_info.password_used)`、`server_mpvio_update_thd(thd, &mpvio)`，然后 `goto end`。
   - `COM_CONNECT`：`mpvio.scramble[SCRAMBLE_LENGTH] = 1;` 标记「尚未生成 scramble」，然后调 `do_auth_once(thd, auth_plugin_name, &mpvio)`。

7. `if (mpvio.status == MPVIO_EXT::RESTART)`：`auth_plugin_name = mpvio.acl_user->plugin;` 然后**再调一次** `do_auth_once(thd, auth_plugin_name, &mpvio)`。此处有断言 `assert(mpvio.acl_user);`，以及一条只在 `COM_CHANGE_USER` 下才成立的断言：`assert(command == COM_CHANGE_USER || my_strcasecmp(system_charset_info, auth_plugin_name.str, mpvio.acl_user->plugin.str));`。
8. `if (res == CR_OK) res = do_multi_factor_auth(thd, &mpvio);`
9. `server_mpvio_update_thd(thd, &mpvio);` 把用户名、账号锁定状态写回安全上下文。
10. `check_and_update_password_lock_state(mpvio, thd, res);` 记账。
11. `PSI_THREAD_CALL(set_connection_type)(thd->get_vio_type());` 记录连接类型。

之后进入以 `Security_context *sctx = thd->security_context();` 开头的一大段，这段的顺序是：

- `bool proxy_check = check_proxy_users && !*mpvio.auth_info.authenticated_as;`——只有开启了代理用户检查、且本次不是以代理身份认证时才做代理匹配。
- `thd->password = mpvio.auth_info.password_used;` 记住「本次是否用了口令」，供后续错误信息使用。
- `if ((!check_proxy_users) && acl_user && !*mpvio.auth_info.authenticated_as)` 则把 `authenticated_as` 置成 `acl_user->user`（源码注释：服务器未启用代理映射时，把这个标志重置成实际用户名）。
- `if (mpvio.auth_info.user_name && !proxy_check)` 调 `acl_log_connect()` 写通用日志。
- `if (res == CR_OK && (!mpvio.can_authenticate() || thd->is_error())) res = CR_ERROR;`——`can_authenticate()` 的实现是 `return (acl_user && acl_user->can_authenticate);`。
- `if (mpvio.can_authenticate()) assign_priv_user_host(sctx, const_cast<ACL_USER *>(acl_user));`
- 失败记账：`if (res > CR_OK && mpvio.status != MPVIO_EXT::SUCCESS)` 时按错误码分类累加 `Host_errors`：`CR_AUTH_PLUGIN_ERROR` → `errors.m_auth_plugin`，`CR_AUTH_HANDSHAKE` → `errors.m_handshake`，`CR_AUTH_USER_CREDENTIALS` → `errors.m_authentication`，`CR_ERROR` 及默认分支 → `errors.m_auth_plugin`。然后 `inc_host_errors(mpvio.ip, &errors)`。

### 找账号：`find_mpvio_user()`

`find_mpvio_user(THD *thd, MPVIO_EXT *mpvio)` 在 `sql/auth/sql_authentication.cc` 中定义，被 `parse_com_change_user_packet()` 和握手包解析路径各调一次。

它的流程是：加 `Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE)` 读锁；若 `acl_users` 非空则 `cached_acl_users_for_name(mpvio->auth_info.user_name)` 取出同名候选列表，逐个比对 `acl_user_tmp->host.compare_hostname(mpvio->host, mpvio->ip)`，命中就 `mpvio->acl_user = acl_user_tmp->copy(mpvio->mem_root)`，并 `*(mpvio->restrictions) = acl_restrictions->find_restrictions(mpvio->acl_user)`。

**没找到用户时不报错，而是造一个「诱饵用户」。** `decoy_user(usr, hst, mpvio->mem_root, mpvio->rand, initialized)` 分配一个 `ACL_USER`，设 `user->can_authenticate = !initialized;`，口令槽位全部清零（`memset(user->credentials[i].m_salt, 0, SCRAMBLE_LENGTH + 1)`、`m_salt_len = 0`、`m_auth_string = EMPTY_CSTR`）。源码注释写明意图：「Pretend the user exists; let the plugin decide how to handle bad credentials.」诱饵用户的插件名是随机挑的：`const int DECIMAL_SHIFT = 1000;`、`random_number = static_cast<int>(my_rnd(rand) * DECIMAL_SHIFT)`、`plugin_num = (uint)(random_number % ((uint)PLUGIN_LAST))`，从 `Cached_authentication_plugins::cached_plugins_names` 里取。选中的编号记在 `unknown_accounts` 表里，插入失败时回退查表、再失败则用 `default_auth_plugin_name`。表超过 `MAX_UNKNOWN_ACCOUNTS` 时由 `unknown_accounts->clear_if_greater(MAX_UNKNOWN_ACCOUNTS)` 清空。

`find_mpvio_user()` 还做了一次「客户端太旧」的判定：若账号的插件不是 `PLUGIN_MYSQL_NATIVE_PASSWORD`，而客户端又没有 `CLIENT_PLUGIN_AUTH` 能力位，报 `ER_NOT_SUPPORTED_AUTH_MODE` 并写通用日志，返回 true（失败）。

最后它填充认证素材：`multi_factor_auth_info[0].auth_string` 取 `acl_user->credentials[PRIMARY_CRED].m_auth_string.str`，`additional_auth_string` 取 `credentials[SECOND_CRED].m_auth_string`（第二凭据为空时置 `nullptr`），并 `strmake(mpvio->auth_info.authenticated_as, mpvio->acl_user->user ...)`。若账号配了多因子（`mpvio->acl_user->m_mfa`），则把第二、第三因子的 `auth_string`、`auth_string_length`、`is_registration_required` 拷进 `multi_factor_auth_info[f]`（`f` 从 1 开始）。

### 换插件：`do_auth_once()`

`do_auth_once(THD *thd, const LEX_CSTRING &auth_plugin_name, MPVIO_EXT *mpvio)` 的核心是取插件、调用、返回：

- 取插件：`g_cached_authentication_plugins->get_cached_plugin_ref(&auth_plugin_name)`；取不到则 `my_plugin_lock_by_name(thd, auth_plugin_name, MYSQL_AUTHENTICATION_PLUGIN)`，成功则置 `unlock_plugin = true`。
- 调用：`st_mysql_auth *auth = (st_mysql_auth *)plugin_decl(plugin)->info; res = auth->authenticate_user(mpvio, &mpvio->auth_info);` —— **这就是插件的接入点**。传进去的 `mpvio` 同时充当 `MYSQL_PLUGIN_VIO`（插件通过 `mpvio->read_packet` / `mpvio->write_packet` 与客户端对话）。
- 插件加载不到：置 `errors.m_no_auth_plugin = 1`，`inc_host_errors(mpvio->ip, &errors)`，报 `ER_PLUGIN_IS_NOT_LOADED`（参数是插件名），返回 `CR_ERROR`。
- 一个状态修正：若调用前 `old_status == MPVIO_EXT::RESTART` 而调用后仍为 `MPVIO_EXT::RESTART`，则把状态重置为 `MPVIO_EXT::FAILURE`。源码注释解释：任何 `write_packet()` 或 `read_packet()` 都会重置状态，但如果插件一个包都没读写，状态就会原样留着。

### 多因子：`do_multi_factor_auth()`

`do_multi_factor_auth(THD *thd, MPVIO_EXT *mpvio)` 的流程：

- `if (!mpvio->acl_user->m_mfa) return res;`——账号没配多因子就直接返回 `CR_OK`。
- `if (!mpvio->protocol->has_client_capability(MULTI_FACTOR_AUTHENTICATION)) return CR_AUTH_USER_CREDENTIALS;`——**老客户端连多因子账号直接失败**。
- 遍历 `mpvio->acl_user->m_mfa->get_multi_factor_auth_list()->get_mfa_list()`，对每个因子：
  - 按 `af->get_factor()` 设置 `mpvio->auth_info.current_auth_factor`：`nthfactor::SECOND_FACTOR` 置 1，`nthfactor::THIRD_FACTOR` 置 2。`get_factor()` 的实现是看 `m_multi_factor_auth->nth_factor`：等于 2 返回 `SECOND_FACTOR`，等于 3 返回 `THIRD_FACTOR`，否则返回 `nthfactor::NONE`。
  - `mpvio->cached_client_reply.pkt = nullptr;`——重置缓存的客户端应答。
  - `my_plugin_lock_by_name(thd, af->plugin_name(), MYSQL_AUTHENTICATION_PLUGIN)` 取该因子的插件；取不到则置 `errors.m_no_auth_plugin = 1`、报 `ER_PLUGIN_IS_NOT_LOADED`（参数是 `af->get_plugin_str()`），返回 `CR_ERROR`。
  - 把 `auth_info.auth_string` / `auth_string_length` 切换到 `multi_factor_auth_info[current_auth_factor]` 对应项，置 `mpvio->status = MPVIO_EXT::START_MFA`，再调 `auth->authenticate_user(mpvio, &mpvio->auth_info)`。
  - 若返回 `CR_OK_AUTH_IN_SANDBOX_MODE` 且 `af->get_requires_registration()`，则 `thd->security_context()->set_registration_sandbox_mode(true)`，解锁插件并返回 `CR_OK`。
  - 其它非 `CR_OK` 返回值：`mpvio->status = MPVIO_EXT::FAILURE` 并 `break`；成功则 `mpvio->status = MPVIO_EXT::SUCCESS`。

### 插件实现之一：`mysql_native_password`

`native_password_authenticate(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)` 的流程：

- `native_password_authentication_deprecation_warning();`——发废弃警告。
- `if (mpvio->scramble[SCRAMBLE_LENGTH]) generate_user_salt(mpvio->scramble, SCRAMBLE_LENGTH + 1);`——只有还没生成过 scramble 才生成（`acl_authenticate()` 在 `COM_CONNECT` 路径上把它置为 1）。
- `mpvio->write_packet(mpvio, (uchar *)mpvio->scramble, SCRAMBLE_LENGTH + 1)` 把 scramble 发给客户端；写失败返回 `CR_AUTH_HANDSHAKE`。
- `pkt_len = mpvio->read_packet(mpvio, &pkt)` 读客户端应答；读失败（返回负值）同样返回 `CR_AUTH_HANDSHAKE`。
- `if (mysql_native_password_proxy_users) *info->authenticated_as = PROXY_FLAG;`——开启该开关时把 `authenticated_as` 标成代理标志。
- **空口令分支：** `if (pkt_len == 0)` 则 `info->password_used = PASSWORD_USED_NO`，并按 `mpvio->acl_user->credentials[PRIMARY_CRED].m_salt_len != 0` 决定返回 `CR_AUTH_USER_CREDENTIALS` 还是 `CR_OK`。
- **正常分支：** `pkt_len == SCRAMBLE_LENGTH` 时做两级校验：先 `check_scramble(pkt, mpvio->scramble, mpvio->acl_user->credentials[PRIMARY_CRED].m_salt)`；若主凭据不匹配，再试 `credentials[SECOND_CRED].m_salt`。命中第二凭据时打一条 `ER_MYSQL_NATIVE_PASSWORD_SECOND_PASSWORD_USED_INFORMATION` 的日志并返回 `CR_OK`。
- 长度既非 0 也非 `SCRAMBLE_LENGTH` 时，报 `ER_HANDSHAKE_ERROR` 并返回 `CR_AUTH_HANDSHAKE`。

校验算法本体在 `sql/auth/password.cc`：`check_scramble()` 转调 `check_scramble_sha1()`，后者做三步——用 `compute_sha1_hash_multi(buf, message, SCRAMBLE_LENGTH, hash_stage2, SHA1_HASH_SIZE)` 算出 key，用 `my_crypt()` 把客户端应答异或掉得到 `hash_stage1`，再 `compute_sha1_hash(hash_stage2_reassured, buf, SHA1_HASH_SIZE)` 得二级哈希，最后 `memcmp(hash_stage2, hash_stage2_reassured, SHA1_HASH_SIZE) != 0` 即为「口令不对」。文件头注释给出了完整公式：客户端做 `reply = xor(hash_stage1, sha1(public_seed, hash_stage2))`，服务端做 `hash_stage1 = xor(reply, sha1(public_seed, hash_stage2))`、`candidate_hash2 = sha1(hash_stage1)`、再比 `candidate_hash2 == hash_stage2`。

### 插件实现之二：`sha256_password`

`sha256_password_authenticate(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)`：

- 先打一条 `ER_SERVER_WARN_DEPRECATED` 警告（提示改用 `caching_sha2_password`），再 `generate_user_salt(scramble, SCRAMBLE_LENGTH + 1)` 生成 nonce，把 `SCRAMBLE_LENGTH + 1` 字节发给客户端（源码注释说明：nonce 拆成 8 + 12 字节，与 `Protocol::HandshakeV10` 一致）。
- `auth_save_scramble(vio, scramble);` 保存 scramble，供认证需要重启时复用。
- 读客户端包失败返回 `CR_ERROR`。
- **空口令分支：** `if ((pkt_len == 0 || pkt_len == 1) && *pkt == 0)` 则 `info->password_used = PASSWORD_USED_NO`；若 `info->auth_string_length == 0` 返回 `CR_OK`（并在 `sha256_password_proxy_users` 打开时置 `*info->authenticated_as = PROXY_FLAG`），否则返回 `CR_ERROR`。
- **非加密信道分支：** `if (!my_vio_is_encrypted(vio))` 则必须用 RSA 加密。取 `g_sha256_rsa_keys->get_private_key()` 与 `get_public_key()`，任一为 `nullptr` 则报 `ER_SHA_PWD_AUTH_REQUIRES_RSA_OR_SSL` 并返回 `CR_ERROR`；`g_sha256_rsa_keys->get_cipher_length() > MAX_CIPHER_LENGTH` 则报 `ER_SHA_PWD_RSA_KEY_TOO_LONG` 并返回 `CR_ERROR`。
- 客户端请求公钥：`if (pkt_len == 1 && *pkt == 1)` 则把 `get_public_key_as_pem()` 发过去，再读一次包。
- `if (pkt_len != cipher_length) return CR_ERROR;`——密文长度必须精确等于期望的 `cipher_length`。
- `decrypt_RSA_private_key(pkt, cipher_length, plain_text, sizeof(plain_text) - 1, private_key)` 解密，然后 `xor_string((char *)plain_text, cipher_length, (char *)scramble, SCRAMBLE_LENGTH);` 用 scramble 异或掉。

### 插件实现之三：`caching_sha2_password`

`caching_sha2_password_authenticate(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)` 定义在 `sql/auth/sha2_password.cc`：

- `generate_user_salt(scramble, SCRAMBLE_LENGTH + 1)` 后 `vio->write_packet(vio, (unsigned char *)scramble, SCRAMBLE_LENGTH + 1)`，写失败返回 `CR_AUTH_HANDSHAKE`。
- `auth_save_scramble(vio, scramble);`。
- 空口令分支 `if (!pkt_len || (pkt_len == 1 && *pkt == 0))`：`info->password_used = PASSWORD_USED_NO`，`info->auth_string_length == 0` 返回 `CR_OK`，否则返回 `CR_AUTH_USER_CREDENTIALS`。
- `make_hash_key(info->authenticated_as, hostname, authorization_id);` 构造缓存键。
- `if (pkt_len != sha2_password::CACHING_SHA2_DIGEST_LENGTH) return CR_ERROR;`——快速认证路径要求应答长度精确等于摘要长度。
- `g_caching_sha2_password->fast_authenticate(authorization_id, scramble, SCRAMBLE_LENGTH, pkt, info->additional_auth_string_length ? true : false)` 返回一个 `std::pair<bool, bool>`。**第一个分量是「是否还需要走完整认证」**：
  - 为真（失败或缓存未命中）：`vio->write_packet(vio, (uchar *)&perform_full_authentication, 1)` 通知客户端转完整认证。
  - 为假：`vio->write_packet(vio, (uchar *)&fast_auth_success, 1)` 发快速认证成功包，返回 `CR_OK`；第二个分量为真时额外打一条 `ER_CACHING_SHA2_PASSWORD_SECOND_PASSWORD_USED_INFORMATION` 日志。
- 完整认证路径读客户端包：`if ((pkt_len = vio->read_packet(vio, &pkt)) <= 0) return CR_AUTH_HANDSHAKE;`。
- `if (!my_vio_is_secure(vio))` 则走 RSA 分支，取 `g_caching_sha2_rsa_keys` 的公私钥；缺钥报 `ER_SHA_PWD_AUTH_REQUIRES_RSA_OR_SSL`，密文超长报 `ER_SHA_PWD_RSA_KEY_TOO_LONG`。
- 客户端请求公钥的条件是 `pkt_len == 1 && *pkt == request_public_key`。
- `if (pkt_len != cipher_length) return CR_ERROR;`，然后 `decrypt_RSA_private_key(...)` 与 `xor_string((char *)plain_text, cipher_length, (char *)scramble, SCRAMBLE_LENGTH);`。

### 失败记账与口令锁定

`check_and_update_password_lock_state(MPVIO_EXT &mpvio, THD *thd, int &res)` 在 `sql/auth/sql_authentication.cc` 中定义：

- 前提条件是 `mpvio.acl_user && initialized && mpvio.acl_user->password_locked_state.is_active()`。
- 在 `Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE)` 下 `find_acl_user(acl_user->host.get_host(), acl_user->user, true)` 取到可写的账号对象。
- `acl_user_ptr->password_locked_state.update(thd, res == CR_OK, &days_remaining)` 的返回值表示「本次结果导致账号被锁定」。
- 锁定时，报 `ER_USER_ACCESS_DENIED_FOR_USER_ACCOUNT_BLOCKED_BY_PASSWORD_LOCK`，日志打 `ER_ACCESS_DENIED_FOR_USER_ACCOUNT_BLOCKED_BY_PASSWORD_LOCK`，并置 `res = CR_ERROR`。错误信息里带的三个数分别来自 `get_failed_login_attempts()`、`get_password_lock_time_days()` 和 `days_remaining`；后两者为 0 时字符串写成 `"unlimited"`。

`login_failed_error(THD *thd, MPVIO_EXT *mpvio, int passwd_used)` 统一负责报「拒绝访问」：

- 若 `thd->is_error()` 为真（前面已设过错误），只打一条 `ER_ABORTING_USER_CONNECTION` 的日志，不再改错误码。
- 否则若 `passwd_used == 2`，报 `ER_ACCESS_DENIED_NO_PASSWORD_ERROR`（无口令场景），写通用日志，并打 `ER_ACCESS_DENIED_ERROR_WITHOUT_PASSWORD` 日志。
- 其余情况报 `ER_ACCESS_DENIED_ERROR`，第三个参数是「是否使用了口令」（`ER_THD(thd, ER_YES)` / `ER_THD(thd, ER_NO)`），并打 `ER_ACCESS_DENIED_ERROR_WITH_PASSWORD` 日志。

`acl_log_connect()` 写通用日志：若 `strcmp(auth_as, user)` 且 `*auth_as != PROXY_FLAG`，格式是 `"%s@%s as %s on %s using %s"`，否则是 `"%s@%s on %s using %s"`。

### 认证通过后的收尾

认证成功（`initialized` 为真）时，`acl_authenticate()` 依次做：

- 代理用户匹配：`acl_find_proxy_user(auth_user, sctx->host().str, sctx->ip().str, mpvio.auth_info.authenticated_as, &is_proxy_user)`。`is_proxy_user` 为真但没找到代理定义时置 `errors.m_proxy_user = 1` 并 `login_failed_error()`；找到代理定义后 `find_acl_user()` 取出被代理账号，找不到则置 `errors.m_proxy_user_acl = 1` 并失败；成功则 `acl_user = acl_proxy_user->copy(thd->mem_root)`，并 `*(mpvio.restrictions) = acl_restrictions->find_restrictions(acl_user)`。
- `sctx->set_master_access(acl_user->access, *(mpvio.restrictions));` 与 `assign_priv_user_host(sctx, ...)`。
- 角色激活：若 `opt_always_activate_granted_roles` 为真则调 `activate_all_granted_and_mandatory_roles(acl_user, sctx)`；否则按源码注释「The server policy is to only activate default roles」只调 `get_default_roles()` 取默认角色再逐个 `sctx->activate_role()`。
- `sctx->checkout_access_maps();`
- 离线模式检查：非 `SUPER_ACL` 且无 `CONNECTION_ADMIN` 全局授权时，若 `mysqld_offline_mode()` 则报 `ER_SERVER_OFFLINE_MODE`。
- SSL 校验：`acl_check_ssl(thd, acl_user)` 返回真则置 `errors.m_ssl = 1` 并 `login_failed_error()`。源码注释说明这一步历史上排在口令之后，是「额外的层」而不是替代口令。
- 账号锁定：`if (unlikely(mpvio.acl_user->account_locked))` 则 `locked_account_connection_count++`，报 `ER_ACCOUNT_HAS_BEEN_LOCKED`，打 `ER_ACCESS_DENIED_FOR_USER_ACCOUNT_LOCKED` 日志。
- 安全传输要求：`if (opt_require_secure_transport && thd->get_net()->vio != nullptr && !is_secure_transport(thd->get_net()->vio->type))` 则报 `ER_SECURE_TRANSPORT_REQUIRED`。
- 口令过期：`check_password_lifetime(thd, mpvio.acl_user)` 判断是否过期。该函数只有在 `!acl_user->password_expired`、`password_last_changed.time_type != MYSQL_TIMESTAMP_ERROR`、`auth_plugin_is_built_in(acl_user->plugin.str)` 且（用默认期限或 `acl_user->password_lifetime` 非零）时才计算；期限取 `acl_user->password_lifetime` 或全局 `default_password_lifetime`（后者在 `MUTEX_LOCK(lock, &LOCK_default_password_lifetime)` 保护下读）。若过期且客户端没有 `CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS` 能力位且 `disconnect_on_expired_password`（默认值为 `true`），报 `ER_MUST_CHANGE_PASSWORD_LOGIN`、写通用日志、打 `ER_ACCOUNT_WITH_EXPIRED_PASSWORD` 日志、置 `errors.m_authentication = 1` 并失败。
- 用户资源限额：`get_or_create_user_conn(thd, ..., &acl_user->user_resource)`，条件是 `user_resource.questions` / `updates` / `conn_per_hour` / `user_conn` 任一非零，或 `global_system_variables.max_user_connections` 非零。
- `if (command == COM_CONNECT && check_restrictions_for_com_connect_command(thd))` 则 `release_user_connection(thd)` 并失败。该检查含两条：管理连接（`thd->is_admin_connection()`）缺 `SERVICE_CONNECTION_ADMIN` 时报 `ER_SPECIFIC_ACCESS_DENIED_ERROR`；用户既无 `SUPER_ACL`、也无 `CONNECTION_ADMIN`、也无 `SERVICE_CONNECTION_ADMIN` 时，若 `Connection_handler_manager::get_instance()->valid_connection_count()` 为假则报 `ER_CON_COUNT_ERROR`。
- 默认库切换：`if (mpvio.db.length)` 则调 `mysql_change_db(thd, to_lex_cstring(mpvio.db), false)`；失败则 `release_user_connection(thd)`、置 `errors.m_default_database = 1`、`login_failed_error()`。
- 成功应答：`if (res == CR_OK_HANDSHAKE_COMPLETE) thd->get_stmt_da()->disable_status(); else my_ok(thd);`——**注意这里的区分**：处于「握手已完成」状态时不再额外发 OK 包。

`initialized` 为假（即 `--skip-grant-tables`）时走 `else` 分支：`sctx->skip_grants();` 并 `thd->get_stmt_da()->reset_diagnostics_area();` 清掉诊断区里的错误。源码注释说明原因：此时 `parse_com_change_user_packet()` 已经把 MPVIO 置为 `SUCCESS`，所以诊断区里的错误不打算采纳。

## 关键取舍

**用插件接口而不是内建分支表。** 认证的「怎么比口令」全部收敛到 `st_mysql_auth::authenticate_user` 这一个函数指针上，`do_auth_once()` 只负责取插件和调它。`mysql_declare_plugin(mysql_password)` 声明了两个内置插件：`native_password_handler`（`mysql_native_password`）与 `sha256_password_handler`（`sha256_password`），版本号都是 `0x0101`，许可都是 `PLUGIN_LICENSE_GPL`。代价是插件调用方要自己处理「插件加载不到」「插件没读写任何包导致状态未变」这类边缘情况——`do_auth_once()` 里那段 `old_status == MPVIO_EXT::RESTART && mpvio->status == MPVIO_EXT::RESTART` 的修正就是为此。

**默认插件只允许三种内置名。** `set_default_auth_plugin()` 用 `Cached_authentication_plugins::compare_plugin()` 逐一比对 `PLUGIN_SHA256_PASSWORD`、`PLUGIN_MYSQL_NATIVE_PASSWORD`、`PLUGIN_CACHING_SHA2_PASSWORD`，三者都不匹配就返回 1。代价是外部认证插件无法被设为全局默认插件。

**找不到账号时造诱饵用户，而不是直接报错。** `decoy_user()` 让不存在的账号也走完整个插件认证流程，由插件自己决定怎么处理「坏凭据」。源码注释明确写了这个意图。代价是这条路径要真的跑一遍密码学运算（对攻击者而言是无意的计算开销，对正常用户是无谓的延迟）。诱饵用户的插件名是随机选的，且随机结果记在 `unknown_accounts` 表里以保持稳定——代价是这张表有 `MAX_UNKNOWN_ACCOUNTS` 的上限，超出即被 `clear_if_greater()` 清空。

**RESTART 机制让服务端能纠正客户端的插件选择。** 握手先走默认插件，拿到用户名后若发现账号用的是别的插件，`mpvio.status` 被置为 `MPVIO_EXT::RESTART`，`acl_authenticate()` 据此用账号的插件重跑一次 `do_auth_once()`。这让老客户端（没有 `CLIENT_PLUGIN_AUTH` 能力位）也能连非 `mysql_native_password` 账号——但 `find_mpvio_user()` 里那条「客户端太旧就报 `ER_NOT_SUPPORTED_AUTH_MODE`」的检查又限制了这一点。代价是同一连接上可能发生两次握手。

**多因子靠「客户端能力位」而不是协商。** `do_multi_factor_auth()` 只检查 `mpvio->protocol->has_client_capability(MULTI_FACTOR_AUTHENTICATION)`：客户端没有这个能力位就直接返回 `CR_AUTH_USER_CREDENTIALS`，不做任何降级协商。`parse_com_change_user_packet()` 里读客户端插件名的分支也类似——没有 `CLIENT_PLUGIN_AUTH` 能力位就固定用 `PLUGIN_MYSQL_NATIVE_PASSWORD`。

**SSL 校验排在口令之后。** `acl_check_ssl()` 在口令认证成功之后才跑，源码注释解释：「Historically it was checked after the password, as an additional layer, not instead of the password (in which case it would've been a plugin too).」代价是错误口令的用户会先付出一次密码学运算的成本。

**口令过期按客户端能力位分流。** 客户端有 `CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS` 能力位时，过期不拦连接，而是通过 `sctx->set_password_expired(...)` 标记，后续在 `dispatch_command()` 侧拦（见 `a01-connection-dispatch` 页的 `ER_MUST_CHANGE_PASSWORD`）。没有该能力位且 `disconnect_on_expired_password` 为真（默认值 `true`）时，直接报 `ER_MUST_CHANGE_PASSWORD_LOGIN` 断开。

**架构推论：** `acl_authenticate()` 把「找账号」「校验凭据」「检查账号状态」「记账」四件事放在同一个函数里顺序执行，其中前两件事的结果（`mpvio.acl_user`、`res`）同时被后面两件事消费。这种「单函数线性流水线」的好处是没有中间状态分散在多个对象里，代价是函数体很长、且账号状态的多种失败分支（代理用户、SSL、账号锁定、安全传输、口令过期、资源限额）都通过 `goto end` 汇到同一个出口。从 `a01-connection-dispatch` 页可以看到 `COM_CHANGE_USER` 走的是同一个 `acl_authenticate()`，因此这两条路径共享上面全部的检查项——只是 `COM_CHANGE_USER` 不做 `check_restrictions_for_com_connect_command()`（该检查有 `command == COM_CONNECT` 的前置判断）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/auth/sql_authentication.cc` | `acl_authenticate` | 认证主编排：初始化 MPVIO、`COM_CONNECT` / `COM_CHANGE_USER` 分叉、RESTART 换插件、多因子、代理用户匹配、账号状态检查、错误分类记账 |
| `sql/auth/sql_authentication.cc` | `do_auth_once` | 取认证插件（`get_cached_plugin_ref` → `my_plugin_lock_by_name`）、调 `auth->authenticate_user()`、插件缺失时报 `ER_PLUGIN_IS_NOT_LOADED`、修正 RESTART 状态残留 |
| `sql/auth/sql_authentication.cc` | `do_multi_factor_auth` | 第二、第三因子的认证：按 `nthfactor::SECOND_FACTOR` / `THIRD_FACTOR` 切换 `current_auth_factor`，逐个调插件的 `authenticate_user()` |
| `sql/auth/sql_authentication.cc` | `find_mpvio_user` | 按用户名 + 主机匹配账号，命中则拷贝到 MPVIO；未命中则造诱饵用户；老客户端遇非 `mysql_native_password` 账号时报 `ER_NOT_SUPPORTED_AUTH_MODE` |
| `sql/auth/sql_authentication.cc` | `decoy_user` | 为不存在的账号构造 `ACL_USER`：凭据槽位清零、插件名随机选取并记入 `unknown_accounts` 表 |
| `sql/auth/sql_authentication.cc` | `parse_com_change_user_packet` | 解析 `COM_CHANGE_USER` 包体：用户名、口令长度、默认库、字符集、客户端插件名、连接属性；解析失败报 `ER_UNKNOWN_COM_ERROR` |
| `sql/auth/sql_authentication.cc` | `server_mpvio_initialize` | 初始化 `MPVIO_EXT`：装读写回调、置 `status` 为 `FAILURE`、`password_used` 为 `PASSWORD_USED_NO`、分配 `auth_factor_desc` 数组 |
| `sql/auth/sql_authentication.cc` | `server_mpvio_update_thd` | 把认证结果写回 THD：用户名、账号锁定标记、`CLIENT_IGNORE_SPACE` 能力位转 `MODE_IGNORE_SPACE` |
| `sql/auth/sql_authentication.cc` | `login_failed_error` | 统一报访问拒绝：`ER_ACCESS_DENIED_NO_PASSWORD_ERROR`（`passwd_used == 2`）或 `ER_ACCESS_DENIED_ERROR`，并写通用日志与错误日志 |
| `sql/auth/sql_authentication.cc` | `acl_log_connect` | 把登录事件写进通用日志，含代理用户时的 `"user@host as user on db using vio"` 格式 |
| `sql/auth/sql_authentication.cc` | `check_and_update_password_lock_state` | 失败次数记账与账号锁定判定，锁定时报 `ER_USER_ACCESS_DENIED_FOR_USER_ACCOUNT_BLOCKED_BY_PASSWORD_LOCK` |
| `sql/auth/sql_authentication.cc` | `check_password_lifetime` | 口令有效期判定：期限取账号的 `password_lifetime` 或全局 `default_password_lifetime` |
| `sql/auth/sql_authentication.cc` | `check_restrictions_for_com_connect_command` | `COM_CONNECT` 专属：管理连接需 `SERVICE_CONNECTION_ADMIN`；否则查 `valid_connection_count()` 是否超限 |
| `sql/auth/sql_authentication.cc` | `native_password_authenticate` | `mysql_native_password` 插件的认证实现：发 scramble、读应答、两级凭据（`PRIMARY_CRED` / `SECOND_CRED`）校验 |
| `sql/auth/sql_authentication.cc` | `sha256_password_authenticate` | `sha256_password` 插件的认证实现：非加密信道下走 RSA 公钥协商与解密，再用 scramble 异或 |
| `sql/auth/sql_authentication.cc` | `set_default_auth_plugin` | 设置全局默认插件名，只接受 `sha256_password` / `mysql_native_password` / `caching_sha2_password` 三者 |
| `sql/auth/sha2_password.cc` | `caching_sha2_password_authenticate` | `caching_sha2_password` 插件的认证实现：快速认证命中缓存直接回 `fast_auth_success`，否则转完整认证走 RSA |
| `sql/auth/password.cc` | `check_scramble` | `mysql_native_password` 的校验算法（转调 `check_scramble_sha1`）：异或还原 `hash_stage1`、重算二级 SHA1 并 `memcmp` 比较 |
| `sql/auth/sql_mfa.cc` | `Multi_factor_auth_info::get_factor` | 由 `m_multi_factor_auth->nth_factor` 判定因子序号：2 对应 `SECOND_FACTOR`，3 对应 `THIRD_FACTOR`，其余为 `nthfactor::NONE` |
| `sql/sql_connect.cc` | `acl_authenticate` | 连接建立路径上的调用点：夹在 `MYSQL_AUDIT_CONNECTION_PRE_AUTHENTICATE` 与 `MYSQL_AUDIT_CONNECTION_CONNECT` 两个审计事件之间，成功后清 IP 失败计数并 `set_ssl` |

## 与主核 / AI 的关系

主核（`engine/db`）走的是嵌入式 SPI，不存在「连接」「握手」「用户名字符串」这些概念，因此没有与本页对应的认证机制。主核里与认证最接近的是账户与口令的存储与校验，但那是接口契约层的事，不涉及协议编排。

**架构推论：** 主核若要接入远程协议访问，MySQL 的认证流水线里有两点值得借鉴。其一是**「插件接口 + 线性编排」的分工**：`acl_authenticate()` 负责编排，`st_mysql_auth::authenticate_user` 负责具体的凭据校验，两者之间只通过 `MPVIO_EXT` 这一个结构通信（插件通过 `mpvio->read_packet` / `mpvio->write_packet` 与对端对话）。主核的嵌入式 SPI 若要支持多种凭据类型，可以采用同样的「编排层 + 可插拔校验器」结构，避免在核心路径里堆叠 if-else。其二是**「账号不存在」与「口令错误」走同一条代码路径**：`decoy_user()` 让两者最终都落到插件的凭据比较上。这对 AI 代理接入的场景有直接价值——代理在尝试认证时，若服务端返回「用户不存在」与「口令错误」两种可区分的错误，攻击者就能用错误类型做用户名枚举；MySQL 用诱饵用户抹平了这一差异。

对 AI 场景而言，认证机制与 AI 代理的关系主要体现在三点：

- **能力位协商是插件能用的前提。** `find_mpvio_user()` 里「非 `mysql_native_password` 账号 + 客户端无 `CLIENT_PLUGIN_AUTH` 能力位 → 报 `ER_NOT_SUPPORTED_AUTH_MODE`」这条检查，以及 `do_multi_factor_auth()` 里「客户端无 `MULTI_FACTOR_AUTHENTICATION` 能力位 → 直接 `CR_AUTH_USER_CREDENTIALS`」，说明服务端不与客户端协商降级，而是按能力位硬判。AI 代理作为自研客户端接入时，必须主动声明正确的能力位，否则会在认证阶段被服务端拒绝——这类失败发生在命令解析之前，从错误信息看是「认证失败」而非「命令不支持」。
- **认证失败会污染主机的错误计数。** `acl_authenticate()` 里的 `inc_host_errors(mpvio.ip, &errors)` 按错误类型分类累加（`m_auth_plugin` / `m_handshake` / `m_authentication` / `m_no_auth_plugin` / `m_proxy_user` / `m_proxy_user_acl` / `m_ssl` / `m_default_database`），而 `sql_connect.cc` 侧在连接成功后调 `reset_host_connect_errors()` 清零。也就是说「一次成功即可洗掉此前失败」的判定发生在 `check_connection()` 里，`acl_authenticate()` 只负责累加。AI 代理若在连接池中反复重试认证，主机的失败计数会持续增长。
- **口令锁定策略作用于账号而非连接。** `check_and_update_password_lock_state()` 通过 `password_locked_state.update(thd, res == CR_OK, &days_remaining)` 记账，锁定后整个账号被拒（`ER_USER_ACCESS_DENIED_FOR_USER_ACCOUNT_BLOCKED_BY_PASSWORD_LOCK`），不只是这一次连接失败。这与 `decoy_user()` 的方向相反——诱饵用户是为了**不**区分，不存在的账号不会因为反复试错而被锁定。

## 参考

- [MySQL 源码：`sql/auth/sql_authentication.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sql_authentication.cc)
- [MySQL 源码：`sql/sql_connect.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_connect.cc)
- [MySQL 源码：`sql/auth/password.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/password.cc)
- [MySQL 源码：`sql/auth/sha2_password.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sha2_password.cc)
- [MySQL 参考手册：认证插件（Authentication Plugins）](https://dev.mysql.com/doc/refman/8.0/en/authentication-plugins.html)
- [MySQL 参考手册：客户端与服务端通信协议（连接阶段与认证交换）](https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase_packets.html)
- [MySQL 参考手册：`caching_sha2_password` 认证插件](https://dev.mysql.com/doc/refman/8.0/en/caching-sha2-password.html)
- [MySQL 参考手册：口令过期策略（Password Expiration Policy）](https://dev.mysql.com/doc/refman/8.0/en/password-expiration-policy.html)
