---
project: redis
feature: acl
page_type: feature
schema_version: 1
refs:
  - path: src/acl.c
    symbols: [ACLInit, ACLSetUser, ACLCheckAllUserCommandPerm, ACLAuthenticateUser]
---

# 访问控制列表

## 一句话职责

ACL 把「哪个连接以哪个用户身份、能跑哪些命令、能碰哪些键与哪些 Pub/Sub 通道」写成一组可热改的规则，并在每条命令执行前用一次位图判断加模式匹配给出放行或拒绝。

## 机制是什么

数据结构是两层：一个 `user` 挂着若干 `aclSelector`，每个选择器自己持三份白名单——命令位图、键模式列表、通道模式列表。`user` 定义在 `src/server.h`，字段是 `name`、原子 `flags`（`USER_FLAG_*`）、`passwords` 链表、`selectors` 链表（长度至少为 1）、以及缓存用的 `acl_string` 与 `source`。`aclSelector` 是 `src/acl.c` 内部类型、不对外暴露，字段是 `flags`（`SELECTOR_FLAG_*`）、`allowed_commands`（长度 `USER_COMMAND_BITS_COUNT/64` 的 `uint64_t` 数组，即 16 个字，覆盖 1024 个命令位）、`allowed_firstargs`、`patterns`、`channels`、`command_rules`（用于反写规则字符串的有序文本）。

选择器不是可选装饰：`ACLCreateUser` 建用户时就 `listAddNodeHead` 塞入一个带 `SELECTOR_FLAG_ROOT` 的根选择器，`ACLUserGetRootSelector` 靠 `listFirst` 取它并 `serverAssert` 校验根标志，因此所有「旧式」规则（`+get`、`~foo:*`）都落在根选择器上；`(...)` 语法加的是额外选择器。

判定语义是「任一选择器放行即放行」：`ACLCheckAllUserCommandPerm` 与 `ACLUserCheckKeyPerm`、`ACLUserCheckChannelPerm` 都是遍历 `u->selectors`，命中一个 `ACL_OK` 就返回。

本页只讲用户规则的维护与一次命令的权限判定；认证密码学细节与 TLS 通道加密在相邻页面。

## 怎么实现的

**初始化顺序。** `ACLInit` 建 `Users`（用户名到 `user` 的 radix 树）、`UsersToLoad`（待加载用户链表，match 方法设为 `ACLListMatchLoadedUser`，按首条规则即用户名比对）、`ACLLog` 三条全局结构，再调 `ACLInitCommandCategories`，最后 `ACLCreateDefaultUser()`。后者先以 `"default"` 建用户，然后连施五条规则：`+@all`、`~*`、`&*`、`on`、`nopass`。`ACLInit` 在 `src/server.c` 的启动流程里紧跟 `initServerConfig()` 被调用，源码注释写明网络层与客户端创建依赖它，必须尽早初始化。

**分类表上限。** `ACLInitCommandCategories` 一次 `zcalloc` 出 `ACL_MAX_CATEGORIES + 1` 个条目（`ACL_MAX_CATEGORIES` 为 64），再逐条登记 `ACLDefaultCommandCategories`——该表到 `{NULL,0}` 终止符为止共 22 项（keyspace、read、write、set、sortedset、list、hash、string、array、bitmap、hyperloglog、geo、stream、pubsub、admin、fast、slow、blocking、dangerous、connection、transaction、scripting，`ENABLE_GCRA` 下多一项 ratelimit）。`ACLAddCommandCategory` 在 `nextCommandCategory >= ACL_MAX_CATEGORIES` 时返回 0，即模块运行期注册新分类只能用到 64 个名额为止。

**新建用户是「几乎全禁」。** `ACLCreateUser` 先 `raxFind` 查重，重名返回 NULL；初始 `flags` 置为 `USER_FLAG_DISABLED | USER_FLAG_SANITIZE_PAYLOAD`，口令链表为空，于是新用户既不能登录也不能执行任何命令。`ACLCreateSelector` 把入参 flags 与 `server.acl_pubsub_default` 或起来，键位图清零——新选择器没有任何键权限。

**用户级规则解析。** `ACLSetUser` 一进来先失效缓存：`u->acl_string` 若在就 `decrRefCount` 并置空。`oplen == -1` 时按 `strlen` 取长度（允许二进制口令由调用方显式给长度），长度为 0 直接返回 `C_OK`。随后按首字符分派：`on`/`off` 原子改 `USER_FLAG_ENABLED` / `USER_FLAG_DISABLED`；`skip-sanitize-payload` 与 `sanitize-payload` 互斥地改 `USER_FLAG_SANITIZE_PAYLOAD_SKIP` / `USER_FLAG_SANITIZE_PAYLOAD`；`nopass` 置位并清空 `passwords`；`resetpass` 清位并清空；`>` 前缀把口令用 `ACLHashPassword` 转成 SHA-256 十六进制后入表，`#` 前缀先 `ACLCheckPasswordHash` 校验，不合法置 `errno = EBADMSG` 返回 `C_ERR`；两者都通过 `listSearchKey` 去重、并清掉 `USER_FLAG_NOPASS`。`<`/`!` 前缀是删口令，`listSearchKey` 找不到就置 `errno = ENODEV` 返回 `C_ERR`。`( ... )` 交给 `aclCreateSelectorFromOpSet` 建新选择器并挂尾。`clearselectors` 用 `listRewind` 遍历并删掉除根以外的选择器（先 `serverAssert(listNext(&li))` 跳过根）。`reset` 不自己实现，而是依次递归调用 `ACLSetUser` 应用 `resetpass`、`resetkeys`、`resetchannels`、（仅当 `server.acl_pubsub_default & SELECTOR_FLAG_ALLCHANNELS` 时）`allchannels`、`off`、`sanitize-payload`、`clearselectors`、`-@all`，每条都用 `serverAssert(... == C_OK)` 断言不会失败。其余规则全部转给根选择器：`ACLUserGetRootSelector(u)` 后 `ACLSetSelector`。

**选择器级规则解析。** `ACLSetSelector` 是规则语法的真正归属地：`allkeys` / `~*` 置 `SELECTOR_FLAG_ALLKEYS` 并 `listEmpty(patterns)`；`resetkeys` 反向；`allchannels` / `&*` 与 `resetchannels` 同理操作 `SELECTOR_FLAG_ALLCHANNELS`；`allcommands` / `+@all` 把 16 个命令字 `memset` 成全 1、置 `SELECTOR_FLAG_ALLCOMMANDS`、清 `command_rules`、`ACLResetFirstArgs`；`nocommands` / `-@all` 归零并清标志。键模式 `~` / `%` 分支：已置 ALLKEYS 时再加模式置 `errno = EEXIST` 返回错误；`%` 需逐个字母解析 `R` / `W`（大小写不敏感），遇到 `~` 停止并后移，出现别的字符则 `perm_ok = 0`，只要没解析出权限位或出现非法字符就置 `errno = EINVAL`；`~` 的权限位是 `ACL_ALL_PERMISSION`。模式串经 `ACLStringHasSpaces` 检查（含空白或 NUL 即 `EINVAL`），再用 `listSearchKey` 去重——重复时是把旧模式的 `flags` 或起来而非再插一份，最后清掉 ALLKEYS。通道 `&` 分支在 ALLCHANNELS 已置时给出 `errno = EISDIR`，同样查空格、去重、清 ALLCHANNELS 标志。命令 `+cmd` 分支走 `ACLLookupCommand`，它只查 `server.orig_commands`（注释说明这样 ACL 不受命令重命名影响），查不到置 `ENOENT`；命中后 `ACLChangeSelectorPerm` 置位并清该命令的 first-args，若命令有 `subcommands_dict` 则连同每个子命令一起置位，同时 `ACLUpdateCommandRules` 维护有序规则文本。`+cmd|sub` 分支有三种拒绝：命令本身不存在是 `ENOENT`；`cmd->parent` 非空（即目标已是子命令）置 `errno = ECHILD`；`|` 后为空置 `errno = EINVAL`。命令确实有子命令字典时按全名再查一次并只置子命令位；没有子命令字典时走 `ACLAddAllowedFirstArg`，并 `serverLog(LL_WARNING, ...)` 打一条「允许被禁命令的首参数属于 ACL 误用、未来可能被禁用」的弃用警告。`-cmd` 分支同理但只做置零。`+@cat` / `-@cat` 交给 `ACLSetSelectorCategory`，它先用 `ACLGetCommandCategoryFlagByName` 拿分类位，拿不到返回 `C_ERR`，调用方据此置 `ENOENT`。任何其他形态一律 `errno = EINVAL`。

**按分类批量改位图。** `ACLSetSelectorCategory` 落位时调 `ACLSetSelectorCommandBitsForCategory`，它遍历命令字典并对 `cmd->acl_categories & cflag` 命中的命令调 `ACLChangeSelectorPerm`，同时递归进 `cmd->subcommands_dict`，因此子命令也跟随分类。

**命令 ID 与位图寻址。** 每个命令要有一个位下标：`ACLGetCommandID` 把命令名转小写后在 `commandId` radix 树里查，命中就复用旧 ID（注释说明这样模块卸载再装载后 ID 不变），否则取 `nextid` 并自增。它特意跳过 `USER_COMMAND_BITS_COUNT-1`（即第 1024 位）当普通 ID，因为这一位被借作「+@all 标记」：`ACLSelectorCanExecuteFutureCommands` 就是去读这一位，用来区分「以 +@all 起步、再减」与「从零加起」，`ACLDescribeSelectorCommandRules` 与 `ACLRecomputeCommandBitsFromCommandRulesAllUsers` 反写规则时依赖它决定先写 `+@all` 还是 `-@all`。寻址在 `ACLGetCommandBitCoordinates`：`id >= USER_COMMAND_BITS_COUNT` 返回 `C_ERR`，字下标是 `id / sizeof(uint64_t) / 8`，位是 `1ULL << (id % (sizeof(uint64_t) * 8))`；越界时 `ACLGetSelectorCommandBit` 直接返回 0（源码注释：这种边界情况按「禁止执行」处理），`ACLSetSelectorCommandBit` 直接返回不做操作。置零时额外清 `SELECTOR_FLAG_ALLCOMMANDS`，因为一旦有位为 0 就不能再靠标志跳过逐位检查。

**一次命令的判定顺序。** 入口是 `src/server.c` 的 `processCommand`：先查 `authRequired(c)`（未认证且命令无 `CMD_NO_AUTH` 就回 `-NOAUTH`），再 `ACLCheckAllPerm(c, c->current_pending_cmd, &acl_errpos)`。失败时 `addACLLogEntry`（`CLIENT_MULTI` 下记 `ACL_LOG_CTX_MULTI`，否则 `ACL_LOG_CTX_TOPLEVEL`），再 `getAclErrorMessage(..., verbose = 0)` 拼成 `-NOPERM ...` 用 `rejectCommandFormat` 拒绝——`verbose = 0` 使键/通道两种原因只回「无权限访问某个键 / 某个通道」而不回具体名字。`ACLCheckAllPerm` 只做两件事：`serverAssert(!pcmd || pcmd->cmd == c->cmd)` 保证缓存键结果与当前命令对得上，然后把 `getClientCachedKeyResult(pcmd)` 当作 `key_result` 传给 `ACLCheckAllUserCommandPerm`——缓存里存的是键在 `pcmd->argv` 中的位置，用错命令就会得到无关下标。

`ACLCheckAllUserCommandPerm` 自身按这个顺序走：`u == NULL`（无用户连接，如内部/主从连接的某个阶段）直接 `ACL_OK`；随后一个快速路径，取 `listFirst(u->selectors)` 的根选择器，若 `ALLCOMMANDS|ALLKEYS|ALLCHANNELS` 三个标志全在就直接 `ACL_OK`；否则逐个选择器调 `ACLSelectorCheckCmd`，任一返回 `ACL_OK` 即放行，全部失败时按「错误码更大者优先、同码取更大下标」聚合出 `relevant_error` 与 `last_idx` 写回 `*idxptr`——因为 `ACL_DENIED_CHANNEL` 是 4、`ACL_DENIED_AUTH` 是 3、`ACL_DENIED_KEY` 是 2、`ACL_DENIED_CMD` 是 1，键/通道的错误天然压过命令错误。

`ACLSelectorCheckCmd` 分三段：命令段在 `SELECTOR_FLAG_ALLCOMMANDS` 缺席且命令不带 `CMD_NO_AUTH` 时逐位查；位为 0 还要看首参数白名单——`argc < 2`、`allowed_firstargs == NULL` 或该命令槽为空都直接 `ACL_DENIED_CMD`，否则按 `cmd->parent ? 2 : 1` 取下标与白名单逐项 `strcasecmp`，走完仍未命中则 `ACL_DENIED_CMD`。键段在 `SELECTOR_FLAG_ALLKEYS` 缺席且 `doesCommandHaveKeys(cmd)` 为真时执行，键表由 `getKeysFromCommandWithSpecs` 一次算出并放进 `aclKeyResultCache` 复用（`initACLKeyResultCache` / `cleanupACLKeyResultCache` 配对管理 `getKeysFreeResult`）；逐个键调 `ACLSelectorCheckKey`，失败就把该键下标写进 `keyidxptr` 并原样返回。通道段同理，只处理带 `CMD_CHANNEL_PUBLISH | CMD_CHANNEL_SUBSCRIBE` 标志的通道参数，逐条调 `ACLCheckChannelAgainstList`。

**键与通道匹配。** `ACLSelectorCheckKey` 先把 keyspec 标志翻译成权限位：`CMD_KEY_ACCESS` 要读权限，`CMD_KEY_INSERT`、`CMD_KEY_DELETE`、`CMD_KEY_UPDATE` 要写权限；再遍历 `patterns`，跳过权限位不满足所需位的模式，`CMD_KEY_PREFIX` 时用 `prefixmatch`（键本身是一批键的前缀），否则 `stringmatchlen` 做 glob 匹配。`ACLCheckChannelAgainstList` 区分两种语义：`is_pattern` 为真（`PSUBSCRIBE` 的通道模式）时用 `strcmp` 字面比较，为假（`PUBLISH` / `SUBSCRIBE` 的普通通道名）时才做 `stringmatchlen` glob 匹配。

**认证。** `ACLAuthenticateUser` 先让模块层试：`checkModuleAuthentication` 在没有任何模块认证回调时直接返回 `AUTH_NOT_HANDLED`；结果确为 `AUTH_NOT_HANDLED` 时才回退到 `checkPasswordBasedAuth`——这就是源码里点名的回退路径，不是隐含分支。`checkPasswordBasedAuth` 调 `ACLCheckUserCredentials`，成功后置 `c->authenticated = 1`、`clientSetUser(c, ACLGetUserByName(...), 1)`（`auth_changed = 1` 会在换身份前给还挂在当前用户的 Pub/Sub 订阅打上归属标记）并 `moduleNotifyUserChanged`；失败则 `addACLLogEntry(..., ACL_DENIED_AUTH, ...)` 返回 `AUTH_ERR`。`ACLCheckUserCredentials` 的次序是：查不到用户置 `ENOENT`；`USER_FLAG_DISABLED` 置 `EINVAL`（停用用户不能登录）；`USER_FLAG_NOPASS` 直接 `C_OK`（任何口令都算通过）；否则把入参口令 `ACLHashPassword` 成哈希，逐个 `passwords` 用 `time_independent_strcmp` 比较 `HASH_PASSWORD_LEN` 字节，全不中置 `EINVAL`。`ACLHashPassword` 用 `sha256_init` / `sha256_update` / `sha256_final` 得到 32 字节摘要（`SHA256_BLOCK_SIZE` 在 `src/sha256.h` 定义为 32），再按 `0123456789abcdef` 表转成十六进制，长度 `HASH_PASSWORD_LEN = SHA256_BLOCK_SIZE*2`，即 64 个字符；`ACLCheckPasswordHash` 就是按「长度必须等于 64」加「只允许 `a`–`f` 与 `0`–`9`」两条来验。`time_independent_strcmp` 逐字节 `diff |= (a[j] ^ b[j])`，不做提前返回，注释写明目的是不让攻击者从耗时推断字符串。`AUTH` 命令的两个形态由 `authCommand` 分派：参数多于 3 个直接语法错误；两参数形态在 `DefaultUser` 带 `NOPASS` 时先报错「没配密码」再拒绝；用户名是 `internal connection` 时转 `internalAuth`（要求集群模式、并与 `clusterGetSecret` 取到的内部秘钥做等长比对），其余一律走 `ACLAuthenticateUser`。`HELLO ... AUTH <user> <pass>` 由 `helloCommand` 解析后同样调 `ACLAuthenticateUser`。

**在事务与脚本里的二次判定。** 同一次判定不覆盖所有执行路径：`src/multi.c` 的 `execCommand` 在真正执行每条队列命令前再调一次 `ACLCheckAllPerm`，失败时按错误码翻译原因并回 `-NOPERM ACLs rules changed between the moment the transaction was accumulated and the EXEC call`，同时以 `ACL_LOG_CTX_MULTI` 记日志；`src/script.c` 的 `scriptVerifyACL` 对脚本的内部客户端调 `ACLCheckAllPerm` 并以 `ACL_LOG_CTX_LUA` 记日志；`src/module.c` 用 `ACLCheckAllUserCommandPerm` 判模块命令、用 `ACLUserCheckChannelPerm` 判模块通道权限；`src/sort.c` 用 `ACLUserCheckCmdWithUnrestrictedKeyAccess(..., CMD_KEY_ACCESS)` 判断是否可跳过逐键检查。

**加载与保存。** 启动阶段的 `ACLLoadUsersAtStartup` 把两种配置来源对立起来：`server.acl_filename` 非空且 `UsersToLoad` 非空就直接 `exit(1)`；先 `ACLLoadConfiguredUsers`，失败同样 `exit(1)`；再 `ACLLoadFromFile` 加载 ACL 文件，有错误字符串就 `exit(1)`。`ACLLoadConfiguredUsers` 逐条应用 `UsersToLoad` 里的规则，用户名含空格直接返回 `C_ERR`，重名只允许 `default` 并先对它 `reset`，规则应用失败返回 `C_ERR`；配置里写了 `off` 用户只打 `LL_NOTICE` 提示而不算错误。`ACLLoadFromFile` 的做法是先在新建的 `Users` radix 树上全集加载：跳过空行与 `#` 注释，每行必须 `user` 开头，用户名含空格报错，重名报错，单行内先 `ACLMergeSelectorArguments` 合并跨参数的选择器括号（括号不配对时 `invalid_idx` 指向开括号位置），再逐条 `ACLSetUser`；全部无错才生效——把 `DefaultUser` 原地拷贝成新配置（旧配置先存进 `old_users` 的 `"default"` 快照），逐客户端用 `pubsubACLLoadReconcileClient` 校验并改写 Pub/Sub 归属戳，再用 `pubsubACLLoadResolveOwner` 重新解析当前身份，`ACL_LOAD_OWNER_GONE` 与校验失败都调 `deauthenticateAndCloseClient` 踢掉连接；有错则释放新树、把 `Users` 指回 `old_users`，并把「未做任何改动」的警告拼进返回串。`ACLSaveToFile` 反向：遍历 `Users` 生成 `user <name> <ACLDescribeUser>` 行，写到 `<filename>.tmp-<pid>-<mtime>`，`write` 循环处理 `EINTR`，`redis_fsync` 后 `rename` 覆盖，再 `fsyncFileDir` 落目录项。

**反写规则。** `ACLDescribeUser` 命中 `u->acl_string` 缓存就直接加引用返回，否则按固定次序拼：`ACLUserFlags` 表里的标志名、每个口令写成 `#<hash>`、每个选择器的 `ACLDescribeSelector`（根选择器不加括号，其余包一层括号）。`ACLDescribeSelector` 拼键模式（全键权限直接写 `~*`，否则按 `keyPattern->flags` 写 `~` / `%R~` / `%W~` 前缀）、通道（全通道写 `&*`，否则先写 `resetchannels` 再逐条 `&pattern`）、最后命令规则。`ACLDescribeSelectorCommandRules` 除了拼字符串还做一次自检：用同样的规则集在一份假选择器上重放，再 `memcmp` 两份 `allowed_commands` 位图，不一致就 `serverPanic`——注释给的理由是「在这条路径上崩溃也比留下安全风险好」。

**ACL 日志。** `addACLLogEntry` 先按原因 `ACLUpdateInfoMetrics` 打点（认证失败、命令/键/通道拒绝各记一个计数，未知原因 `serverPanic`）；`server.acllog_max_len` 为 0 时只做 `trimACLLogEntriesToMaxLen` 就返回，不建条目（打点照样生效）。建条目后最多扫前 10 条旧记录（源码写死 `long toscan = 10`）寻找「同原因、同上下文、`ctime` 差值不超过 `ACL_LOG_GROUPING_MAX_TIME_DELTA`（60000 毫秒）、同 object、同 username」的条目，命中则把它移到表头并 `count++`，否则插表头再按 `server.acllog_max_len` 从头裁掉尾部。上限与默认值来自 `src/config.c`：`createULongConfig("acllog-max-len", ...)` 默认 128；`ACL LOG` 不带参数时默认输出 10 条（源码局部常量 `long count = 10`），带 `RESET` 则临时装上 `ACLFreeLogEntry` 后 `listEmpty` 再摘掉 free 方法。

## 关键取舍

**源码事实：** 权限判定是位图加模式匹配，不做命令名的字符串比较——`ACLGetSelectorCommandBit` 只读一个位，命令粒度代价固定；命令名比较只出现在 `ACLLookupCommand` 这类改规则的路径上。**源码事实：** 位图上限是 `USER_COMMAND_BITS_COUNT`（1024），越界 ID 在 `ACLGetCommandBitCoordinates` 被判 `C_ERR`，`ACLGetSelectorCommandBit` 对越界返回 0，即「越界即拒绝」而非「越界即放行」。**源码事实：** 第 1024 位被借作「+@all 标记」，`ACLGetCommandID` 专门跳过它，所以「以 +@all 起步」的用户能自动获得后续模块加载的新命令，而「逐条加命令」的用户不会——这是 `ACLDescribeSelectorCommandRules` 必须区分 `+@all` / `-@all` 起手的原因。**源码事实：** 权限判定放在 `processCommand`，于是 `MULTI` 队列内的命令要等 `execCommand` 时再判一次，规则在排队期间被改掉就以执行时的规则为准。**源码事实：** ACL 文件加载是「整棵树重建 + 全部成功才切换」，任一规则报错就整体回滚到 `old_users`，并把已有连接按新权限重新校验。**源码事实：** 口令只存 SHA-256 十六进制，比较走 `time_independent_strcmp`，而 `#<hash>` 形态同样只接受 64 位小写十六进制，因此 ACL 文件可以不存明文。

**架构推论：** 把「用户」与「选择器」拆成两层，是为了让多条互不相同的键/通道/命令白名单能挂在同一个用户名下而不必做交集——判定是「任一选择器放行」，所以选择器之间的语义是并集而非交集；这解释了 `ACLCheckAllUserCommandPerm` 在聚合错误时只能挑一个「最相关」的错误上报，而不是列出所有失败原因。**架构推论：** 键段与通道段只有在命令真带键/通道时才执行，且命令段先做，意味着「命令被禁」时不会去做键的键提取；这是为了让最常见的拒绝（整条命令无权限）走最短路径。**架构推论：** `ACLLoadFromFile` 原地改写 `DefaultUser` 而不是替换指针，是因为 `c->user == DefaultUser` 的指针比较被 `authRequired` 等多处依赖，换指针会让所有存量连接的身份引用失效。**架构推论：** 日志条目按「原因 + 上下文 + 时间窗 + object + username」聚合而不去重全表，是把无界增长的拒绝事件压成有界条目（默认 128 条）的手段；代价是聚合表扫描有 10 条上限，历史深处的同因事件会另起一条。**架构推论：** 位图宽度固定为 1024，等价于「一个进程最多能寻址 1024 个命令位」的硬上限，命令表再大也只能靠分类位与命令名区分——这是用固定内存换判定常数时间的选择。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/acl.c` | `ACLInit` | 建 `Users` / `UsersToLoad` / `ACLLog`，初始化分类表与 `default` 用户 |
| `src/acl.c` | `ACLCreateDefaultUser` | 按 `+@all`、`~*`、`&*`、`on`、`nopass` 造内置 default 用户 |
| `src/acl.c` | `ACLCreateUser` | 建用户并挂根选择器；重名返回 NULL，初始标志为禁用并做 payload 清洗 |
| `src/acl.c` | `ACLSetUser` | 解析用户级规则（on/off、口令、reset、选择器、其余转根选择器） |
| `src/acl.c` | `ACLSetSelector` | 解析选择器级规则（键模式、通道模式、命令/分类、首参数白名单） |
| `src/acl.c` | `ACLChangeSelectorPerm` | 置/清某命令位，命令有子命令字典时连子命令一起改 |
| `src/acl.c` | `ACLSetSelectorCommandBitsForCategory` | 按分类位批量改位图，递归进子命令字典 |
| `src/acl.c` | `ACLGetCommandID` | 分配顺序命令 ID，复用同名 ID，跳过保留的第 1024 位 |
| `src/acl.c` | `ACLGetCommandBitCoordinates` | 把命令 ID 折算成字下标与位掩码，越界返回 `C_ERR` |
| `src/acl.c` | `ACLSelectorCheckCmd` | 单选择器三段判定：命令位（含首参数）→ 键 → 通道 |
| `src/acl.c` | `ACLCheckAllUserCommandPerm` | 遍历选择器聚合结果，任一放行即 `ACL_OK`，否则给出错误码与下标 |
| `src/acl.c` | `ACLCheckAllPerm` | 被 `processCommand` 调用的高层入口，复用 `pendingCommand` 的键结果缓存 |
| `src/acl.c` | `ACLAuthenticateUser` | 先模块认证，`AUTH_NOT_HANDLED` 时回退 `checkPasswordBasedAuth` |
| `src/acl.c` | `ACLCheckUserCredentials` | 查用户、查禁用、查 nopass，最后逐个哈希比对 |
| `src/acl.c` | `ACLHashPassword` | SHA-256 摘要转 64 字符小写十六进制 |
| `src/acl.c` | `ACLLoadFromFile` | 整树重建式加载 ACL 文件，全成功才切换并重校验存量连接 |
| `src/acl.c` | `ACLSaveToFile` | 生成 `user ...` 行，写临时文件后 `rename` 覆盖并 fsync 目录 |
| `src/acl.c` | `ACLDescribeUser` | 反写用户规则文本（带 `acl_string` 缓存） |
| `src/acl.c` | `addACLLogEntry` | 记 ACL 日志，10 条扫描窗口内聚合，按 `acllog-max-len` 裁剪 |
| `src/server.h` | `USER_COMMAND_BITS_COUNT` | 命令位图宽度上限 1024，末位保留给「可执行未来命令」标记 |

## 与主核 / AI 的关系

**架构推论：** 主核若要做行级或列级的权限控制，ACL 的「用户 + 多选择器 + 逐维度白名单」结构可以直接借用，因为它的判定是「任一选择器放行」，天然支持同一个人在不同维度上持有互不重叠的授权；但主核是多写者引擎，判定与规则改写之间需要显式的读写互斥，Redis 靠单线程主循环免掉了这层——`ACLSetUser` 直接原地改位图、`ACLCheckAllPerm` 直接原地读位图，没有任何版本号或 RCU 结构。**架构推论：** Redis 把「命令 ID 分配」与「位图宽度固定」绑在一起，代价是命令数超过 1024 后无法再分配普通位；主核若命令面更大，需要位图之外的二级索引，或者把判定改成「分类位 + 名称哈希」两段。**架构推论：** 对 AI 推理接口而言，这里的可复用点是「鉴权在调度前一次完成、拒绝原因分类打点」这两个模式：Redis 在 `processCommand` 处一次判定并写 `ACL LOG`，AI 网关同样可以在请求入队前做一次权限判定并记录结构化拒绝原因，避免在流式输出中途才发现无权限。**架构推论：** `ACLCheckAllPerm` 复用 `pendingCommand` 的键结果缓存，说明「同一请求内的重复解析要缓存」是这条路径的既有取舍；AI 请求里的 token 计数、上下文裁剪若被多处调用，也该沿用同一份解析结果。

## 参考

- [Redis 文档：ACL（访问控制列表）](https://redis.io/docs/latest/operate/oss_and_stack/management/security/acl/)
- [Redis 源码：src/acl.c](https://github.com/redis/redis/blob/unstable/src/acl.c)
- [Redis 源码：src/server.h（用户与选择器标志、命令位图宽度）](https://github.com/redis/redis/blob/unstable/src/server.h)
- [Redis 命令文档：ACL GETUSER](https://redis.io/docs/latest/commands/acl-getuser/)
- [Redis 命令文档：ACL SETUSER](https://redis.io/docs/latest/commands/acl-setuser/)
