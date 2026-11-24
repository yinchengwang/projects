---
project: sqlite
feature: db-connection-open
page_type: feature
schema_version: 1
refs:
  - path: src/main.c
    symbols: [sqlite3_open_v2, sqlite3ParseUri]
---

# 数据库连接与打开

## 一句话职责

`sqlite3_open_v2` 把一次「打开数据库」的请求拆成 URL 解析（`sqlite3ParseUri`）、VFS 选择、连接对象分配与后端文件打开四段，并在打开失败时把连接置为 `SQLITE_STATE_SICK` 或直接销毁。

## 机制是什么

一次连接建立的完整链路有四个阶段，彼此职责不重叠：

1. **入口归一**：`sqlite3_open_v2` 只做一件事——把 `(filename, ppDb, flags, zVfs)` 转交给 `openDatabase`，自己不做任何判断。`sqlite3_open` 是它的特例，固定传 `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE` 且 VFS 名传 `0`（走默认 VFS）。
2. **URL 解析**：`sqlite3ParseUri` 决定「文件名」到底是普通路径还是 URI。它同时负责三件事：切分路径与查询参数、把 `cache=` / `mode=` 参数翻译成 `SQLITE_OPEN_*` 标志位、按 `vfs=` 参数或调用方传入的名字选定 VFS。
3. **连接对象建立**：`openDatabase` 分配 `sqlite3` 结构体、初始化互斥量、写入各默认值（限额表、编码、排序规则、编译期开关对应的 `db->flags` 位）。
4. **后端打开**：`sqlite3BtreeOpen` 用选定的 VFS 打开底层文件并读文件头，页大小与保留区字节由此确定。

边界上，本机制**不负责**：语句编译、事务状态机、页缓存内容管理与字节级 I/O。这些分别属于 B 组、E 组、M 组、I 组与 J 组。`sqlite3ParseUri` 明确不触碰数据库内容——它只看字符串。

## 怎么实现的

### 入口与标志位净化

`sqlite3_open_v2` 直接调用 `openDatabase(filename, ppDb, (unsigned int)flags, zVfs)`。`openDatabase` 在建立任何状态之前先做两件事：

- 若未定义 `SQLITE_OMIT_AUTOINIT`，先调 `sqlite3_initialize()`，失败即返回。
- 依据 `SQLITE_OPEN_NOMUTEX` / `SQLITE_OPEN_FULLMUTEX` / `sqlite3GlobalConfig.bFullMutex` 三者算出 `isThreadsafe`，据此决定是否为该连接分配 `SQLITE_MUTEX_RECURSIVE` 互斥量。

随后净化标志位：`flags &= ~( SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_TEMP_DB | SQLITE_OPEN_TRANSIENT_DB | SQLITE_OPEN_MAIN_JOURNAL | SQLITE_OPEN_TEMP_JOURNAL | SQLITE_OPEN_SUBJOURNAL | SQLITE_OPEN_SUPER_JOURNAL | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_WAL )`。这些位在源码注释里被明确称为「harmful bits」，静默清除是为了避免下层的 `assert()` 被非法组合触发。净化后仍保留 `SQLITE_OPEN_READONLY` / `SQLITE_OPEN_READWRITE` / `SQLITE_OPEN_CREATE` / `SQLITE_OPEN_SHAREDCACHE` / `SQLITE_OPEN_PRIVATECACHE` / `SQLITE_OPEN_EXRESCODE`。

共享缓存标志的最终形态由两处决定：带 `SQLITE_OPEN_PRIVATECACHE` 时先清掉 `SQLITE_OPEN_SHAREDCACHE`；否则若 `sqlite3GlobalConfig.sharedCacheEnabled` 为真则强制置上 `SQLITE_OPEN_SHAREDCACHE`。

### URI 解析：sqlite3ParseUri

进入 URI 分支的条件是两个条件同时成立：调用方传了 `SQLITE_OPEN_URI`，或 `sqlite3GlobalConfig.bOpenUri` 为真（后者由 `sqlite3_config(SQLITE_CONFIG_URI, 1)` 原子写入）；并且 `nUri>=5 && memcmp(zUri, "file:", 5)==0`。一旦进入分支，函数先无条件 `flags |= SQLITE_OPEN_URI`，目的是告诉 VFS 的 `xOpen` 方法「文件名后面可能跟着额外参数」。

路径解析分三段：

- **authority 段**：默认编译下，若 `zUri[5]=='/' && zUri[6]=='/'`，authority 之后的第一个 `/` 之前必须是空串或恰好 `"localhost"`（长度 12 的比较），否则报 `"invalid uri authority: %.*s"` 并返回 `SQLITE_ERROR`。若定义了 `SQLITE_ALLOW_URI_AUTHORITY`，这段检查被替换成允许 UNC 路径的处理（`///` 前缀与 `//localhost/` 前缀各有一处特判）。
- **文件名与查询参数**：主循环用 `eState` 三态机区分「文件名」「参数名」「参数值」三段，遇到 `?` 进入参数名段，遇到 `&` 或 `=` 在参数名段内则结束该参数。`%HH` 转义在此过程中解码：两个十六进制位都合法才解码，`%00` 在默认编译下会忽略当前正在解析的这段剩余文本并跳到下一个 `?`/`=`/`&` 边界；若定义了 `SQLITE_ENABLE_URI_00_ERROR`，则 `"unexpected %%00 in uri"` 并返回 `SQLITE_ERROR`。
- **选项解释**：只有三类选项在此函数内被解释——`vfs=`、`cache=`、`mode=`。其余选项（如 `immutable`、`nolock`）原样存入文件名缓冲区，交给更下层处理。

`cache=` 只接受 `shared` 与 `private` 两个值，分别映射到 `SQLITE_OPEN_SHAREDCACHE` 与 `SQLITE_OPEN_PRIVATECACHE`。`mode=` 接受 `ro` / `rw` / `rwc` / `memory`，分别映射到 `SQLITE_OPEN_READONLY` / `SQLITE_OPEN_READWRITE` / `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE` / `SQLITE_OPEN_MEMORY`。值不在表中时报 `"no such %s mode: %s"` 并返回 `SQLITE_ERROR`。对 `mode=`，允许集是 `mask & flags`——即调用方没传的访问权限位，URI 也无权加上；越权时报 `"%s mode not allowed: %s"` 并返回 `SQLITE_PERM`。`cache=` 的允许集则是完整的 `SQLITE_OPEN_SHAREDCACHE|SQLITE_OPEN_PRIVATECACHE` 掩码，不受调用方 flags 约束。

非 URI 分支（`zFile = sqlite3_malloc64(nUri+8)`）只做原样拷贝，并执行 `flags &= ~SQLITE_OPEN_URI`。

**VFS 选定**在函数末尾：`*ppVfs = sqlite3_vfs_find(zVfs)`。`zVfs` 初值是调用方传入的 VFS 名，只有 URI 里出现 `vfs=` 才被覆盖。`sqlite3_vfs_find` 在名字为 `0` 时返回 VFS 链表上的第一个（默认 VFS）；名字非 `0` 则按 `strcmp(zVfs, pVfs->zName)` 精确匹配。查不到时报 `"no such vfs: %s"` 并返回 `SQLITE_ERROR`。

失败时 `sqlite3_free_filename(zFile)` 并把 `*pzFile` 置 `0`；成功时 `*pzFile` 指向的缓冲区交给调用方用 `sqlite3_free_filename()` 释放（不是 `sqlite3_free()`）。

### 连接对象建立

`openDatabase` 用 `sqlite3MallocZero(sizeof(sqlite3))` 分配连接体，随后写入一批默认值：

- `db->errMask` 由 `SQLITE_OPEN_EXRESCODE` 决定：有该位时为 `0xffffffff`（扩展结果码全开），否则为 `0xff`（只保留主码）。
- `db->nDb = 2`（`main` 与 `temp` 两个数据库槽），`db->eOpenState = SQLITE_STATE_BUSY`（建立中）。
- `db->nFpDigit = 17`，即浮点数按 15 位有效数字解析、保留 2 位额外位。
- `db->aLimit` 整体拷贝自静态数组 `aHardLimit`，该数组按 `SQLITE_LIMIT_*` 顺序填入 `SQLITE_MAX_LENGTH`、`SQLITE_MAX_SQL_LENGTH`、`SQLITE_MAX_COLUMN`（默认 2000）、`SQLITE_MAX_EXPR_DEPTH`、`SQLITE_MAX_COMPOUND_SELECT`、`SQLITE_MAX_VDBE_OP`、`SQLITE_MAX_FUNCTION_ARG`、`SQLITE_MAX_ATTACHED`（默认 10）、`SQLITE_MAX_LIKE_PATTERN_LENGTH`、`SQLITE_MAX_VARIABLE_NUMBER`、`SQLITE_MAX_TRIGGER_DEPTH`、`SQLITE_MAX_WORKER_THREADS`、`SQLITE_MAX_PARSER_DEPTH`、`SQLITE_MAX_SCHEMA`、`SQLITE_MAX_TRIGGER_STEPS`、`SQLITE_MAX_CTES`；紧接着单独把 `db->aLimit[SQLITE_LIMIT_WORKER_THREADS]` 覆盖为 `SQLITE_DEFAULT_WORKER_THREADS`（默认 0，上限常量 `SQLITE_MAX_WORKER_THREADS` 默认 8）。
- `db->autoCommit = 1`，`db->nextAutovac = -1`。
- `db->szMmap = sqlite3GlobalConfig.szMmap`，取自 `sqlite3_config()` 的全局配置。
- `db->init.azInit = sqlite3StdType`。
- `db->flags` 写入一批编译期决定的默认位：`SQLITE_ShortColNames | SQLITE_EnableTrigger | SQLITE_EnableView | SQLITE_CacheSpill | SQLITE_AttachCreate | SQLITE_AttachWrite | SQLITE_Comments` 恒开；`SQLITE_TrustedSchema` 依 `SQLITE_TRUSTED_SCHEMA` 宏；`SQLITE_DqsDML` / `SQLITE_DqsDDL` 由 `SQLITE_DQS`（未定义时 `#define SQLITE_DQS 3`）的位 1 / 位 2 决定；`SQLITE_AutoIndex` 依 `SQLITE_DEFAULT_AUTOMATIC_INDEX`；`SQLITE_CkptFullFSync` 依 `SQLITE_DEFAULT_CKPTFULLFSYNC`；`SQLITE_LegacyFileFmt` 在 `SQLITE_DEFAULT_FILE_FORMAT<4` 时置位；`SQLITE_LoadExtension`、`SQLITE_RecTriggers`、`SQLITE_ForeignKeys`、`SQLITE_Defensive`、`SQLITE_LegacyAlter` 等各依对应编译宏。
- 排序规则：`createCollation` 注册 `BINARY`（分别建 UTF-8 / UTF-16BE / UTF-16LE 三个版本）与 `NOCASE`、`RTRIM`。

### 模式合法性检查与打开

访问模式组合由一个位技巧判定：`if( ((1<<(flags&7)) & 0x46)==0 )` 则 `rc = SQLITE_MISUSE_BKPT`。源码里紧邻的 `assert()` 给出三个常量的值：`SQLITE_OPEN_READONLY == 0x01`、`SQLITE_OPEN_READWRITE == 0x02`、`SQLITE_OPEN_CREATE == 0x04`。`flags&7` 取低三位，因此 `1<<(flags&7)` 落在 1 到 7 之间；掩码 `0x46` 在这个范围内只覆盖 bit 1（`0x02`，即 `SQLITE_OPEN_READWRITE`）与 bit 2（`0x04`，即 `SQLITE_OPEN_CREATE`），其 bit 6（`0x40`）永远不可能被命中。于是实际判据是「读写位与建库位至少要有一个为 1」，恰好对应源码注释列出的三种合法组合：只读（`0x01`）、读写（`0x02`）、读写加建库（`0x02|0x04`）。`zFilename==0` 时被替换为 `":memory:"`。

`sqlite3ParseUri` 成功后调用 `sqlite3BtreeOpen(db->pVfs, zOpen, db, &db->aDb[0].pBt, 0, flags | SQLITE_OPEN_MAIN_DB)`。若返回 `SQLITE_IOERR_NOMEM`，被改写为 `SQLITE_NOMEM_BKPT`。`sqlite3BtreeOpen` 内部：`:memory:` 或 `SQLITE_OPEN_MEMORY` 会置上 `BTREE_MEMORY`；当 `SQLITE_OPEN_MAIN_DB` 与「内存库或临时库」同时成立时，`SQLITE_OPEN_MAIN_DB` 会被降级替换为 `SQLITE_OPEN_TEMP_DB`。随后 `sqlite3PagerOpen` 打开文件，`sqlite3PagerReadFileheader` 读前 100 字节头部，页大小取自头部偏移 16 与 17 的两个字节，必须满足不小于 512、不大于 `SQLITE_MAX_PAGE_SIZE` 且是 2 的幂，否则 `pBt->pageSize` 置 0（表示待定，由后续写入时确定）；保留区大小取自头部偏移 20 的一个字节。

Btree 打开成功后设置两个数据库槽的默认名与同步级别：`db->aDb[0].zDbSName = "main"`，`db->aDb[0].safety_level = SQLITE_DEFAULT_SYNCHRONOUS+1`；`db->aDb[1].zDbSName = "temp"`，`db->aDb[1].safety_level = PAGER_SYNCHRONOUS_OFF`。`SQLITE_DEFAULT_SYNCHRONOUS` 未定义时为 2，因此主库默认 safety_level 为 3。源码注释解释了 `+1` 的原因：`PAGER_SYNCHRONOUS_*` 内部宏比外部零基编号大 1（`OFF=1`/`NORMAL=2`/`FULL=3`/`EXTRA=4` 对应 `0`/`1`/`2`/`3`）。

随后 `db->eOpenState = SQLITE_STATE_OPEN`，`sqlite3RegisterPerConnectionBuiltinFunctions(db)` 注册每连接内置函数——源码注释明确「不尝试读取数据库模式，这被推迟到第一次访问数据库时」。之后加载编译期内置扩展（遍历 `sqlite3BuiltinExtensions`）与自动扩展（`sqlite3AutoLoadExtensions(db)`）。

最后 `setupLookaside(db, 0, sqlite3GlobalConfig.szLookaside, sqlite3GlobalConfig.nLookaside)` 启用 lookaside 内存池，其内部把槽大小向下取整到 8 的倍数，槽大小不超过指针大小时置 0，槽大小超过 65528 时截为 65528，槽数小于 1 时置 0，槽数超过 `0x7fff0000/sz` 时截到该值。`sqlite3_wal_autocheckpoint(db, SQLITE_DEFAULT_WAL_AUTOCHECKPOINT)` 把自动检查点阈值设为常量 `SQLITE_DEFAULT_WAL_AUTOCHECKPOINT`（`sqliteLimit.h` 中为 1000）。

### 失败路径

统一跳到 `opendb_out` 标签收尾：若最终错误码主码是 `SQLITE_NOMEM`，则 `sqlite3_close(db)` 并把 `db` 置 `0`；否则若 `rc!=SQLITE_OK`，置 `db->eOpenState = SQLITE_STATE_SICK`——连接对象仍然交给调用方，但标记为「病态」。无论成败都执行 `sqlite3_free_filename(zOpen)` 释放 URI 解析出的文件名缓冲区。

### 降级与回退

- **URI 未启用时的回退**：若 `SQLITE_OPEN_URI` 未置位且全局 `bOpenUri` 为假，`sqlite3ParseUri` 走非 URI 分支，把整个字符串当普通路径拷贝，`flags &= ~SQLITE_OPEN_URI`。
- **VFS 缺省回退**：`sqlite3_vfs_find(0)` 返回 VFS 链表首个（默认 VFS），调用方通过 `sqlite3_open_v2` 第四参数显式指定的 VFS 名只在 URI 无 `vfs=` 参数时生效。
- **`SQLITE_OMIT_AUTOINIT` 下的回退**：跳过 `sqlite3_initialize()` 自动初始化调用，改为由调用方在 `sqlite3_open_v2` 之前显式初始化。
- **内存库回退**：`sqlite3BtreeOpen` 在 `:memory:` 且定义了 `SQLITE_OMIT_MEMORYDB` 时，`isMemdb` 恒为 0，`:memory:` 退化为一个普通文件名交给 VFS 打开。

## 关键取舍

**静默净化标志位而非报错**。`openDatabase` 把 12 个「harmful bits」用掩码清零而不是返回 `SQLITE_MISUSE`。理由写在源码注释里：这些位历史上是被忽略的，如果在这里拦下会破坏既有调用方；而放过去又可能在更下层触发 `assert()`。代价是调用方拼错标志位不会得到任何反馈。

**把「合法组合」检查放在 URI 解析之前**。`((1<<(flags&7)) & 0x46)==0` 这一处检查必须早于 `sqlite3ParseUri`，否则 `mode=` 参数改写后的标志位组合不会被复检。源码注释说明这样早拦是为了「不在这里阻止非法组合的话，可能在下层触发 `assert()`」。

**URI 里的 `mode=` 权限不得超过调用方给的 flags**。这是 `sqlite3ParseUri` 里 `limit = mask & flags` 与 `(mode & ~SQLITE_OPEN_MEMORY)>limit` 两处判断的直接后果，也是 `SQLITE_PERM` 这条错误码在打开路径上的唯一来源。它防止一个只申请只读打开的调用方被 URI 悄悄升级成可写。

**打开失败仍返回连接对象**。除 `SQLITE_NOMEM` 外，`openDatabase` 在 `rc!=SQLITE_OK` 时把 `db->eOpenState` 置为 `SQLITE_STATE_SICK` 并把指针交给调用者，而不是直接置空。这样调用方仍可调用 `sqlite3_errmsg()` 取到错误文本并用 `sqlite3_close()` 释放资源。

**模式（schema）加载推迟到首次访问**。`sqlite3RegisterPerConnectionBuiltinFunctions` 之后的注释写明「do not attempt to read the database schema yet」。这让 `sqlite3_open_v2` 的耗时与数据库内容规模解耦。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/main.c` | `sqlite3_open_v2` | 打开入口，把 filename/flags/zVfs 转交 `openDatabase` |
| `src/main.c` | `sqlite3ParseUri` | URI 解析：`vfs=`/`cache=`/`mode=` 翻译与 VFS 选定 |
| `src/main.c` | `openDatabase` | 连接对象分配、标志位净化、模式合法性检查、打开与失败收尾 |
| `src/main.c` | `sqlite3_open` | 以 `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE` 与默认 VFS 打开的特例 |
| `src/main.c` | `setupLookaside` | 连接级 lookaside 内存池初始化与槽数/槽大小钳位 |
| `src/main.c` | `aHardLimit` | 各 `SQLITE_LIMIT_*` 的硬上限来源表 |
| `src/btree.c` | `sqlite3BtreeOpen` | 用选定 VFS 打开后端文件，读文件头确定页大小与保留区 |
| `src/os.c` | `sqlite3_vfs_find` | 按名查 VFS；名字为空时返回链表首个默认 VFS |

## 与主核 / AI 的关系

SQLite 的「打开」把三件事压在一次调用里：路径语义解析（URI）、抽象层选择（VFS）、连接对象建立。可直接借用的设计是把「文件名」视为带查询参数的结构化输入，而不是裸字符串——`sqlite3ParseUri` 用三态机把参数名与参数值分离，从而让 `mode=` / `cache=` / `vfs=` 三类参数在打开阶段就落到 `SQLITE_OPEN_*` 标志位上，不需要在更下层重复解析。

`isThreadsafe` 的判定把「库是否编译为线程安全」「全局默认是否 full mutex」「本次调用显式要 NOMUTEX 还是 FULLMUTEX」三处条件收敛成一个布尔量，再据此决定是否分配连接级递归互斥量——这个「多来源优先级收敛到单值」的模式可直接映射到本仓主核的连接初始化。

`SQLITE_STATE_BUSY → SQLITE_STATE_OPEN → SQLITE_STATE_SICK` 的状态枚举表明连接对象的生命周期是被显式建模的，而不是靠错误码旁路表达；本仓主核若要把「打开中/已打开/打开失败但对象仍在」这类状态暴露给上层，值得对齐这种显式建模而非用 `NULL` 指针表达失败。

## 参考

- [SQLite 官方文档：sqlite3_open_v2()](https://sqlite.org/c3ref/open.html)
- [SQLite 官方文档：URI filenames](https://sqlite.org/uri.html)
- [SQLite 官方文档：How To Create Custom VFS](https://sqlite.org/c3ref/vfs.html)
- [GitHub 源码镜像：src/main.c（sqlite3_open_v2 与 sqlite3ParseUri 所在文件）](https://github.com/sqlite/sqlite/blob/master/src/main.c)
