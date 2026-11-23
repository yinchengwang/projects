---
project: redis
feature: config-runtime
page_type: feature
schema_version: 1
refs:
  - path: src/config.c
    symbols: [configSetCommand, rewriteConfig]
---

# 运行时配置与配置重写

## 一句话职责

`configSetCommand` 是 `CONFIG SET` 的命令入口：它把一次请求里所有「参数名 / 参数值」成对取出来，一次性做完全部合法性判定（未知项、immutable、protected、loading 期禁用、重复项），备份旧值，逐个调用该配置的类型 setter，再对需要 apply 的项调用 apply，任一环节失败就用 `restoreBackupConfig` 把已经改过的项整批回滚。`rewriteConfig` 是 `CONFIG REWRITE` 的执行体：读入旧配置文件、逐项调用重写回调把内存值写回 rewrite state、清理孤儿行、最后用「临时文件 + rename」原子替换原文件。命令入口 `configRewriteCommand` 只负责判空 `server.configfile` 并转调 `rewriteConfig`。

## 机制是什么

运行时配置既不是散落在 `server` 结构里的裸变量，也不是一张 key-value 表，而是**一张描述表加一套类型接口**：

- 描述表的静态实例是 `src/config.c` 里的 `standardConfig static_configs[]`，末尾以 `{NULL}` 收尾；每一项由 `createBoolConfig` / `createIntConfig` / `createStringConfig` / `createSDSConfig` / `createEnumConfig` / `createSpecialConfig` 这类宏展开出来，宏里固定填三样东西：名字与别名、`flags`、`typeInterface` 的五个函数指针。
- `typeInterface` 的五个指针各管一件事，注释写明：`init` 在启动时用默认值初始化；`set` 在启动加载与 `CONFIG SET` 时调用，**返回 1 表示真的改了、2 表示没有实际变化、0 表示出错**并可写回错误串；`apply` 只在 `CONFIG SET` 语境下、`set` 之后调用；`get` 供 `CONFIG GET` 取字符串表示；`rewrite` 供 `CONFIG REWRITE` 把当前值写进 rewrite state。
- 运行时索引是 `dict *configs`，由 `initConfigValues` 以 `sdsHashDictType` 创建，`dictExpand` 的大小取自 `static_configs` 的元素数；它把每个配置的**主名**注册一次，若该项有 `alias`，再以 `registerConfigValue(..., alias=1)` 注册一条 `ALIAS_CONFIG` 副本，两条 entry 共享同一份 `standardConfig` 拷贝。定位只有一条路径：`lookupConfig(const sds name)` 直接在 `configs` 里 `dictFind`。
- 配置项的能力位定义在 `src/server.h`：`MODIFIABLE_CONFIG` 取 0（默认即可改）、`IMMUTABLE_CONFIG (1ULL<<0)` 只能在启动时设、`SENSITIVE_CONFIG (1ULL<<1)` 值含敏感信息、`DEBUG_CONFIG (1ULL<<2)`、`MULTI_ARG_CONFIG (1ULL<<3)` 该配置收多参数、`HIDDEN_CONFIG (1ULL<<4)` 不参与 `CONFIG GET` 的模式匹配、`PROTECTED_CONFIG (1ULL<<5)` 受 `enable-protected-configs` 约束、`DENY_LOADING_CONFIG (1ULL<<6)` loading 期禁止改、`ALIAS_CONFIG (1ULL<<7)`、`MODULE_CONFIG (1ULL<<8)`、`VOLATILE_CONFIG (1ULL<<9)` 表示配置存的是「指向数据的引用」而不是数据本身、值没变也要执行 apply。
- 存储形态由类型决定：`BOOL_CONFIG` / `NUMERIC_CONFIG` / `STRING_CONFIG` / `SDS_CONFIG` / `ENUM_CONFIG` 的 `typeData` 里都直接存着 `server.xxx` 的地址；`SPECIAL_CONFIG` 没有通用数据字段，全靠自定义的 set/get/rewrite 函数（`dir`、`save`、`client-output-buffer-limit`、`oom-score-adj-values`、`notify-keyspace-events`、`bind`、`replicaof`、`latency-tracking-info-percentiles` 八个）。

重写侧的角色是 `rewriteConfigState`，字段是 `option_to_line`（配置名 → 旧文件里出现该名字的行号链表）、`rewritten`（已处理过的配置名集合）、`numlines` / `lines`（当前文件的行数组）、`needs_signature`（是否还要补写签名注释）、`force_write`、`append_after_include`。

## 怎么实现的

### 一、`configSetCommand` 的四段式流程

第一段是**参数解析与全量校验**。`c->argc & 1` 为真就说明参数个数是奇数（`CONFIG SET` 加名字必须成对），直接回 `shared.syntaxerr`；否则 `config_count = (c->argc - 2) / 2`，按 `config_count` 分配 `set_configs` / `config_names` / `new_values` / `old_values` / `config_changed` 五个数组。随后对每一对参数：

1. `lookupConfig(c->argv[2+i*2]->ptr)` 找不到该名字：只在第一次命中时记下 `invalid_arg_name` 并置 `invalid_args = 1`，然后 `continue`。
2. 带 `SENSITIVE_CONFIG` 的项一律调用 `redactClientCommandArgument(c, 2+i*2+1)`——注意擦除的是**值**的下标，不是名字。该函数在 `src/networking.c` 里，先 `retainOriginalCommandVector` 保留原始参数向量，再把对应 `c->original_argv[]` 项换成 `shared.redacted`，源码注释说明这是为了防止 CONFIG SET 里的口令出现在 slowlog / monitor 等回显里。
3. `IMMUTABLE_CONFIG`，或 `PROTECTED_CONFIG` 且 `!allowProtectedAction(server.enable_protected_configs, c)`：置 `invalid_args = 1` 并记下错误串，区分两种文案 `can't set immutable config` 与 `can't set protected config`，再 `continue`。
4. `server.loading` 且带 `DENY_LOADING_CONFIG`：置 `deny_loading_error = 1` 与 `invalid_args = 1`，`continue`。
5. 与前 `i` 项重复：用 `configIsSameConfig(a, b)` 判定，它先比指针，再在任一方带 `ALIAS_CONFIG` 时用 `configGetPrimaryName` 取主名后 `strcasecmp`，命中即报 `duplicate parameter`。

这一段的 `continue` 都是**故意不立刻返回**：注释写明即使已经出错也要把剩余参数遍历完，好让所有 `SENSITIVE_CONFIG` 的值都被擦除。全部合法才进入第二段，否则直接 `goto err`。

第二段是**备份**：`old_values[i] = set_configs[i]->interface.get(set_configs[i])`，用 getter 的字符串形态当回滚素材（这样回滚也走统一的 set 接口）。

第三段是**设置**：对每一对调 `performInterfaceSet`。该函数只做一件事：`MULTI_ARG_CONFIG` 的项用 `sdssplitlen(value, sdslen(value), " ", 1, &argc)` 按**单个空格**拆成多参数数组，其余项把 `value` 当成只有一个元素的数组；然后 `config->interface.set(config, argv, argc, errstr)`。返回 0 时，`restoreBackupConfig(set_configs, old_values, i+1)` 立刻回滚前 `i+1` 项，`goto err`；返回 1 记 `config_changed[i] = 1`，返回 2 记 0。

第四段是**apply 与通知**：只对 `config_changed[i] == 1` 且 `configNeedsApply` 为真的项执行；`configNeedsApply` 的口径是「`MODULE_CONFIG` 且 `moduleConfigNeedsApply(privdata)` 为真，或者 `interface.apply` 非空」。`MODULE_CONFIG` 走 `moduleConfigApply(config->privdata, &errstr)`（`src/module.c`），其余走 `interface.apply(&errstr)`。任何一项 apply 失败，就 `restoreBackupConfig(set_configs, old_values, config_count)` **全量**回滚。全部通过后 `moduleFireServerEvent(REDISMODULE_EVENT_CONFIG, REDISMODULE_SUBEVENT_CONFIG_CHANGE, &cc)` 通知模块，再 `addReply(c, shared.ok)`。

`restoreBackupConfig` 自己的实现是两轮：先对所有 `count` 项用 `performInterfaceSet` 写回旧值，再对所有 `configNeedsApply` 为真的项再调一次 apply（`MODULE_CONFIG` 同样走 `moduleConfigApply`）。两轮里的失败都只 `serverLog(LL_WARNING)`，不回执客户端——此时 `configSetCommand` 自己已经要输出 `CONFIG SET failed`。

### 二、错误回执的优先级

`err:` 标签处的分叉顺序是固定的，`deny_loading_error` 排在最先，源码注释写明理由：loading 错误会被客户端区别处理，与普通 `-ERR` 不同，所以优先级最高。四档依次是：

- `deny_loading_error` → `addReplyErrorObject(c, shared.loadingerr)`；
- `invalid_arg_name` → `Unknown option or number of arguments for CONFIG SET - '<name>'`；
- `errstr` → `CONFIG SET failed (possibly related to argument '<name>') - <errstr>`；
- 都没有 → `CONFIG SET failed (possibly related to argument '<name>')`。

尾部统一释放五个数组，`old_values` 里的 sds 逐个 `sdsfree`。

### 三、受保护配置的判定：`allowProtectedAction`

`allowProtectedAction(int config, client *c)` 只有一行逻辑：`config == PROTECTED_ACTION_ALLOWED_YES`，或 `config == PROTECTED_ACTION_ALLOWED_LOCAL` 且 `connIsLocal(c->conn) == 1`。三个枚举值来自 `protected_action_enum`（`no` / `yes` / `local`），默认取 `PROTECTED_ACTION_ALLOWED_NO`；开关本身是 `enable-protected-configs`，带 `IMMUTABLE_CONFIG` 所以只能启动时设。带 `PROTECTED_CONFIG` 的实例在描述表里只有两个：`dbfilename` 的 flags 是 `MODIFIABLE_CONFIG | PROTECTED_CONFIG`，`dir` 的 flags 是 `MODIFIABLE_CONFIG | PROTECTED_CONFIG | DENY_LOADING_CONFIG`。

### 四、类型 setter 的三态与取值解析

三态不是约定而是代码里的实际返回值，逐类型都对得上：

- `boolConfigSetInternal`：`yesnotoi` 返回 -1 时报 `argument must be 'yes' or 'no'` 并回 0；有 `is_valid_fn` 且否决时回 0；值真的变了回 1；否则按 `VOLATILE_CONFIG` 回 1 或 2。
- `numericConfigSetInternal`：先 `numericBoundaryCheck`，再 `is_valid_fn`，再与 `prev` 比较；变了 `setNumericType` 回 1，否则同样按 `VOLATILE_CONFIG` 取 1 或 2。
- `sdsConfigSet` / `stringConfigSetInternal` / `enumConfigSet` 同构，都以 `VOLATILE_CONFIG` 决定「值没变时算不算改了」。

数值解析在 `numericParseString` 里按 flags 依次尝试：`MEMORY_CONFIG` 先 `memtoull`；`PERCENT_CONFIG` 且末字符是 `%` 且前缀能被 `string2ll` 解析且非负，则**取负存储**；`OCTAL_CONFIG` 用 `strtoll(value, &endptr, 8)`；最后无 flags 时用 `string2ll`。失败时的错误串分 `argument must be a memory or percent value`、`argument must be a memory value`、`argument couldn't be parsed as an octal number`、`argument couldn't be parsed into an integer` 四种。

边界检查在 `numericBoundaryCheck`：`NUMERIC_TYPE_ULONG_LONG` / `NUMERIC_TYPE_UINT` / `NUMERIC_TYPE_SIZE_T` 三个无符号类型按 unsigned 比较上下界；其余类型里，`PERCENT_CONFIG` 且值为负时只与 `lower_bound` 比较（所以百分比的上限语义是「绝对值不超过 `-lower_bound`」，错误串写作 `percentage argument must be less or equal to ...`），其他情况按有符号比较。边界值本身就是表里的常量，例如 `port` 是 `createIntConfig("port", NULL, MODIFIABLE_CONFIG, 0, 65535, server.port, 6379, INTEGER_CONFIG, NULL, updatePort)`。

### 五、`CONFIG REWRITE` 的四步

`rewriteConfig(char *path, int force_write)` 的步骤在源码注释里就是四段：

1. **读旧档**：`rewriteConfigReadOldFile(path)` 返回 `rewriteConfigState`。`fopen` 失败且 `errno != ENOENT` 时返回 NULL（`ENOENT` 视为「文件不存在」，返回一个空 state）；`sb.st_size == 0` 也直接返回空 state。逐行处理时：以 `#` 开头或空行原样 `rewriteConfigAppendLine`；`sdssplitargs` 失败、或首词既不是 `lookupConfig` 认识的配置名、也不是 `include` / `rename-command` / `user` / `loadmodule` / `sentinel` 这五种「只存在于文件解析层」的指令，就把整行改写成以 `# ??? ` 开头的注释再追加，从而让无法识别的旧行既不生效也不丢失；其余行先 `sdstolower` 首词，alias 换回主名，Sentinel 模式下把 `sentinel <x>` 拼成复合键，用 `rewriteConfigAddLineNumberToOption` 登记进 `option_to_line`。
2. **逐项重写**：遍历 `configs`，跳过带 `ALIAS_CONFIG` 的副本（只重写主名），对 `interface.rewrite` 非空的项调用 `config->interface.rewrite(config, dictGetKey(de), state)`。
3. **特殊指令补写**：`rewriteConfigUserOption(state)`、`rewriteConfigLoadmoduleOption(state)`，Sentinel 模式下再调 `src/sentinel.c` 提供的 `rewriteConfigSentinelOption(state)`。`rewriteConfigUserOption` 在 `server.acl_filename` 非空时只 `rewriteConfigMarkAsProcessed("user")` 不写行；否则遍历 `Users` rax，用 `ACLDescribeUser` 生成描述再写成 `user <name> <desc>`，source-aware 模式下跳过来源是 CLI 或 include 的用户。`rewriteConfigLoadmoduleOption` 跳过 `loadmod->path` 为空的内部模块，其余按 `loadmodule <path> <arg>...` 强制写。
4. **清孤儿 + 落盘**：`rewriteConfigRemoveOrphaned(state)` 遍历 `option_to_line`，若该配置不在 `state->rewritten` 里就只打一条 `LL_DEBUG` 日志并保留原行（「重写过程不认识的选项不能抹掉」），否则把它的**所有**剩余行号对应的 `state->lines[]` 换成空 sds；`rewriteConfigGetContentFromState(state)` 拼接全部行、把连续空行折叠成一行；`rewriteConfigOverwriteFile(path, newcontent)` 落盘。

单行级别的写入由 `rewriteConfigRewriteLine` 决定「复用旧行还是追加新行」：若 `state->append_after_include` 置位，先把 `l` 清空并强制 `force = 1`；`rewriteConfigMarkAsProcessed(state, option)` 总是先登记；`!l && !force && !state->force_write` 时丢弃新行并返回 0（这就是「默认值且文件里本来没有」的项不会被写出来）；有旧行则取链表首节点替换该行并把它从映射里摘掉；没有旧行时，若 `state->needs_signature` 仍为真就先追加 `# Generated by CONFIG REWRITE` 这一行（常量 `REDIS_CONFIG_REWRITE_SIGNATURE`），再追加新行。

落盘函数的原子性来自「临时文件 + rename」：先在 `<configfile>.XXXXXX`（`tmp_suffix` 常量）上 `mkostemp`（非 `_GNU_SOURCE` 分支退回 `mkstemp`），循环 `write` 且遇 `EINTR` 重试，然后依次 `fsync(fd)`、`fchmod(fd, 0644 & ~server.umask)`、`rename(tmp_conffile, configfile)`、`fsyncFileDir(configfile)`；四步里任一失败都只记 `LL_WARNING` 并跳到 `cleanup`，`cleanup` 负责 `close(fd)`、失败时 `unlink(tmp_conffile)` 并恢复 errno。返回 0 表示成功，-1 表示失败。

各类型的重写回调只负责把值格式化成一行，再由上面这套机制决定写不写、写到哪。格式化口径与 getter 对齐：`rewriteConfigBytesOption` 调 `rewriteConfigFormatMemory`，按 `1024*1024*1024` / `1024*1024` / `1024` 是否能整除决定输出 `gb` / `mb` / `kb` 后缀，都不整除就输出裸字节数；`rewriteConfigPercentOption` 对内部存成正数的百分比写 `%lld%%`；`rewriteConfigYesNoOption` 写 `yes` / `no`；`rewriteConfigEnumOption` 用 `configEnumGetName` 拼名字；`rewriteConfigStringOption` / `rewriteConfigSdsOption` 用 `sdscatrepr` 加引号，`value == NULL` 时改走 `rewriteConfigClearStringOption`——非 include 覆盖场景只 `rewriteConfigMarkAsProcessed`（省略该项重启后仍是 NULL），include 覆盖场景则写 `<option> ""` 显式覆盖。

### 六、source-aware 重写分支

`config-rewrite-source-aware` 登记为 `createBoolConfig("config-rewrite-source-aware", NULL, IMMUTABLE_CONFIG | HIDDEN_CONFIG, server.config_rewrite_source_aware, 0, NULL, NULL)`，即默认关闭、只能启动时设、对 `CONFIG GET` 通配匹配隐藏。开启且 `force_write == 0` 时，`rewriteConfig` 会额外调 `rewriteConfigReadFileValues(path)` 建立「文件侧有效值」基线：

- `rewriteConfigReadFileValues` 以 `config_startup_cwd` 为基准解析主文件路径（`serverAssert(config_startup_cwd[0] != '\0')`，这个 cwd 在 `loadServerConfig` 首次加载时由 `getcwd` 抓取，早于 `dir` 指令生效），再调 `rewriteConfigParseFile(root, CONFIG_SOURCE_MAIN, values, &cwd, 1)`；主文件缺失算空基线，`include` 指向的文件缺失算错误。
- `rewriteConfigParseFile` 按文件顺序逐行走，`include` 无通配直接打开、带 `*?[` 用 `glob`，通配无匹配按启动期同款语义忽略；普通指令用 `rewriteConfigNormalizeFileValue` 把文件语法规范化成 getter 的形态，记下 `source` 与 `seen_in_include`。aggregate 类指令（`save`、`client-output-buffer-limit`）靠 `rewriteFileValue` 累积。
- 遍历阶段因此多了三条判断：`config->source == CONFIG_SOURCE_CLI` 直接跳过（CLI 提供的值重启后会被重新施加，不该落盘）；当前 `interface.get` 的值与基线相同（基线取文件值，文件里没有就取 `rewriteConfigGetDefaultValue` 声明的默认值）就 `continue`；写行前把 `state->append_after_include` 置成「这条指令的生效来源是 include 或曾在 include 出现过」。

`CONFIG_SOURCE_*` 五个取值在 `src/server.h`：`DEFAULT` / `MAIN` / `INCLUDE` / `CLI` / `RUNTIME`。运行时命令设值会把来源记成 `RUNTIME` 的路径只在模块与 ACL 侧（`src/module.c`、`src/acl.c`），`configSetCommand` 本身不改 `source`。

### 七、`rewriteConfig` 的真实调用方

- `configRewriteCommand`：`server.configfile == NULL` 时直接 `The server is running without a config file`；否则 `rewriteConfig(server.configfile, 0)`，失败时先把 `errno` 存进局部变量再 `serverLog(LL_WARNING)` 并回 `Rewriting config file: <strerror>`，成功回 `shared.ok` 并记 `CONFIG REWRITE executed with success.`。
- `sentinelFlushConfig`（`src/sentinel.c`）：临时把 `server.hz` 改成 `CONFIG_DEFAULT_HZ`（`src/server.h` 定义为 10），调完 `rewriteConfig(server.configfile, 0)` 再恢复，失败返回 `C_ERR`。
- `restartServer`（`src/server.c`）：带 `RESTART_SERVER_CONFIG_REWRITE` 标志且 `server.configfile` 非空时先 `rewriteConfig(server.configfile, 0)`，失败则放弃重启并返回 `C_ERR`。
- `DEBUG CONFIG-REWRITE-FORCE-ALL`（`src/debug.c`）：传 `force_write = 1`，这是唯一把 `state->force_write` 置位并让所有项强制写出的外部入口。

## 关键取舍

- **源码事实：** `CONFIG SET` 是「全量校验 → 备份 → 逐个 set → 逐个 apply」的事务式批处理，不是逐项提交。代价在 `restoreBackupConfig`：它写回旧值和重跑 apply 的失败都只有 `serverLog(LL_WARNING)`，客户端此时收到的仍是 `CONFIG SET failed`，回滚失败对调用方不可见。
- **源码事实：** 出错后仍遍历完剩余参数，唯一目的是让所有 `SENSITIVE_CONFIG` 的值都被 `redactClientCommandArgument` 擦除；因此这条命令是「先保证不泄漏，再保证报错」的顺序。
- **源码事实：** 默认值不是「总会写进文件」——`rewriteConfigRewriteLine` 只在 `force`（`value != defvalue`）或 `state->force_write` 或该配置旧文件里已有行时才落笔。默认值的语义因此是「文件里没有它，重启后仍取默认」，而不是「文件里显式写着默认值」。
- **源码事实：** 字符串类配置设成 NULL 时默认一行都不写（只 `rewriteConfigMarkAsProcessed`），只有在 `append_after_include` 场景才写 `<option> ""` 来压掉 include 里的取值；`replicaof` 在主节点且 `append_after_include` 时写的是 `replicaof no one`。
- **源码事实：** 重写不认识的旧行既不当错误也不删除：`rewriteConfigReadOldFile` 把无法解析的行改写成 `# ??? ` 注释，`rewriteConfigRemoveOrphaned` 对不在 `rewritten` 集合里的配置直接保留原行。
- **源码事实：** 落盘走「临时文件 + fsync + rename + 目录 fsync」的序列，权限显式 `fchmod(fd, 0644 & ~server.umask)`，失败路径 `unlink` 临时文件并把 errno 复原后返回给调用方，所以 `configRewriteCommand` 能拿到准确的 `strerror`。
- **架构推论：** 主名与别名各占一条 dict entry、共享同一份 `standardConfig` 拷贝，是为了让所有按名字的查找都是 O(1)，代价是 `configSetCommand` 必须用 `configIsSameConfig` 显式扫一遍已收集项才能查重，且 `rewriteConfig` 必须跳过 `ALIAS_CONFIG` 才不会写两遍。
- **架构推论：** `setter` 用 1/2/0 三态而不是 bool，是为了让 `CONFIG SET` 只在「值真的变了」时才跑 apply；`VOLATILE_CONFIG` 的存在说明「值字符串相等」不等于「效果等价」（`tls-key-file` 存的是路径，文件内容换了而路径字符串没换），这一位就是给这类间接配置留的后门。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/config.c` | `configSetCommand` | `CONFIG SET` 入口：解析、校验、备份、设置、apply、回滚与错误回执 |
| `src/config.c` | `performInterfaceSet` | 按 `MULTI_ARG_CONFIG` 决定拆不拆空格，再调类型 setter |
| `src/config.c` | `configNeedsApply` | 判定某配置是否需要跑 apply（模块 apply_fn 或 `interface.apply`） |
| `src/config.c` | `restoreBackupConfig` | 回滚：写回旧值 + 重跑 apply，失败只记日志 |
| `src/config.c` | `configIsSameConfig` / `configGetPrimaryName` | 别名归一后的重复参数判定 |
| `src/config.c` | `allowProtectedAction` | `enable-protected-configs` 的 yes / local / no 三态判定 |
| `src/config.c` | `initConfigValues` / `registerConfigValue` / `lookupConfig` | 建 `configs` 字典、登记主名与别名、O(1) 定位 |
| `src/config.c` | `numericParseString` / `numericBoundaryCheck` | 数值/内存/百分比/八进制解析与上下界检查 |
| `src/config.c` | `rewriteConfig` | 重写主流程：读旧档 → 逐项 rewrite → 清孤儿 → 落盘 |
| `src/config.c` | `rewriteConfigReadOldFile` | 把旧文件拆成行并建立「配置名 → 行号」映射 |
| `src/config.c` | `rewriteConfigRewriteLine` / `rewriteConfigMarkAsProcessed` | 单行替换/追加与「已处理」登记，决定默认值是否落盘 |
| `src/config.c` | `rewriteConfigRemoveOrphaned` / `rewriteConfigGetContentFromState` | 空行化孤儿行、折叠连续空行拼出最终内容 |
| `src/config.c` | `rewriteConfigOverwriteFile` | 临时文件 + fsync + fchmod + rename + 目录 fsync 的原子替换 |
| `src/config.c` | `rewriteConfigGetDefaultValue` / `rewriteConfigReadFileValues` / `rewriteConfigParseFile` | source-aware 重写用的文件侧基线与默认值基线 |
| `src/config.c` | `rewriteConfigBytesOption` / `rewriteConfigFormatMemory` | 字节值的 `gb` / `mb` / `kb` / 裸数格式化 |
| `src/config.c` | `rewriteConfigStringOption` / `rewriteConfigClearStringOption` | 字符串行生成与 NULL 值的两种处理 |
| `src/config.c` | `configRewriteCommand` | `CONFIG REWRITE` 命令实现与错误回执 |
| `src/config.c` | `static_configs` | 全部配置项的描述表 |
| `src/server.h` | `IMMUTABLE_CONFIG` / `SENSITIVE_CONFIG` / `MULTI_ARG_CONFIG` / `VOLATILE_CONFIG` 等 | 配置能力位常量 |
| `src/server.h` | `configSource` 枚举 | `DEFAULT` / `MAIN` / `INCLUDE` / `CLI` / `RUNTIME` 五种来源 |
| `src/networking.c` | `redactClientCommandArgument` | 把敏感值替换成 `shared.redacted` |
| `src/module.c` | `moduleConfigApply` / `moduleConfigNeedsApply` | 模块配置的 apply 调用与是否需要 apply 的判定 |
| `src/sentinel.c` | `sentinelFlushConfig` | Sentinel 借同一套重写引擎持久化自身状态 |
| `src/server.c` | `restartServer` | `RESTART_SERVER_CONFIG_REWRITE` 下的重写前置步骤 |
| `src/debug.c` | `DEBUG CONFIG-REWRITE-FORCE-ALL` 分支 | 唯一以 `force_write = 1` 调用重写的外部入口 |

## 与主核 / AI 的关系

**架构推论：** 这套「描述表 + 类型接口 + 三态 setter」的骨架可以脱离 Redis 的语义直接搬走：主核（`engine/db/`）若要暴露运行时参数（缓存大小、压缩阈值、并行度），把每个参数的「解析、校验、生效、格式化、持久化」拆成五个函数指针，就能让 `SET` 与 `REWRITE` 两条路径共用一份定义，而不需要在两处各写一遍参数名。真正的成本不在表，而在「生效」这一步——Redis 里凡是涉及重新绑定监听、重建上下文、重开文件的参数，都必须单独写 apply 并在失败时能被回滚，这部分无法泛化。

**架构推论：** 对 AI 场景最值得借鉴的是 `restoreBackupConfig` 的「全量回滚但不回执」这个取舍。批量改参数时，回滚自身也会失败——源码里 `restoreBackupConfig` 的两轮循环各自都留了 `serverLog(LL_WARNING)` 分支，说明写回旧值与重跑 apply 都不是必成的。Redis 的选择是只记日志，把错误留给调用方已经收到的那个失败回执。主核若要在运行时调整索引参数（如向量索引的 `ef`、HNSW 的 `M`），同样会面对「部分子系统已重建、部分失败」的中间态，与其试图做到完全原子，不如像这里一样：先备份所有旧值的字符串形态，失败时按数组下标从 0 到 `count-1` 重放一遍，并把重放失败记为告警而非致命错误。

**架构推论：** source-aware 分支说明了「运行时改的值该不该持久化」是个独立的策略层，而不是 setter 的职责：值怎么设与值从哪来、要不要写回文件是三件事。主核若同时支持命令行覆盖、配置文件、运行时 API 三种入口，这条「来源决定是否落盘」的规则需要在设计之初就留出字段，否则后面只能在重写阶段做字符串比对来猜。

## 参考

- [Redis 文档：CONFIG SET](https://redis.io/docs/latest/commands/config-set/) —— 多参数形式、返回值与「不写回配置文件」的口径。
- [Redis 文档：CONFIG REWRITE](https://redis.io/docs/latest/commands/config-rewrite/) —— 「默认值不写入、注释保留」的官方描述。
- [Redis 文档：Redis configuration](https://redis.io/docs/latest/operate/oss_and_stack/management/config/) —— 配置文件语法、命令行 `--` 前缀与 include 语义。
- [Redis 源码：src/config.c](https://github.com/redis/redis/blob/unstable/src/config.c) —— 本页所有符号、常量与错误串的真实定义位置。
- [Redis 源码：src/server.h](https://github.com/redis/redis/blob/unstable/src/server.h) —— 配置能力位、`configSource` 枚举与类型枚举的定义位置。
