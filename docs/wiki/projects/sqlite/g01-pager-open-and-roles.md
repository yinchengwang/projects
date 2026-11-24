---
project: sqlite
feature: pager-open-and-roles
page_type: feature
schema_version: 1
refs:
  - path: src/pager.c
    symbols: [sqlite3PagerOpen, sqlite3PagerGetJournalMode, sqlite3PagerSetJournalMode, sqlite3PagerOkToChangeJournalMode]
---

# 分页器与回滚模式切换

## 一句话职责

分页器打开时把一个数据库文件（或一个内存/临时库）包装成 `Pager` 对象并一次性分配其附属内存，同时决定这个 pager 初始处于哪种 journal 模式；此后 `sqlite3PagerSetJournalMode` 负责在回滚模式之间切换，`sqlite3PagerGetJournalMode` 负责读取当前模式，`sqlite3PagerOkToChangeJournalMode` 负责判定当前状态是否允许切换。

## 机制是什么

Pager 是 B-tree 与操作系统文件之间的中间层。每打开一个数据库文件（主库、附加库、临时库），B-tree 就通过 `sqlite3BtreeOpen` 调用一次 `sqlite3PagerOpen`，得到一个独立的 `Pager` 实例。Pager 自己不存储页内容——页缓存由 PCache 对象承担——它持有的是文件句柄、journal 文件句柄、journal 模式标志、页大小、锁状态等元数据。

「角色」指的是一个 pager 在事务持久性协议里的归属。SQLite 的 journal 模式用一组整型常量编码，定义在 `src/pager.h`：`PAGER_JOURNALMODE_DELETE` 为 0、`PAGER_JOURNALMODE_PERSIST` 为 1、`PAGER_JOURNALMODE_OFF` 为 2、`PAGER_JOURNALMODE_TRUNCATE` 为 3、`PAGER_JOURNALMODE_MEMORY` 为 4、`PAGER_JOURNALMODE_WAL` 为 5；另有 `PAGER_JOURNALMODE_QUERY` 为 -1 表示只查询不修改。这些数值通过 `PRAGMA journal_mode` 对外暴露，因此不能改动，否则会破坏兼容性。

角色的分配发生在打开阶段，而不是切换阶段。`sqlite3PagerOpen` 根据传入的 `flags` 与文件属性决定初始模式：若 `flags` 带 `PAGER_OMIT_JOURNAL`（`src/pager.h` 中定义为 `0x0001`），初始模式直接设为 `PAGER_JOURNALMODE_OFF`；否则若该 pager 是内存库（`PAGER_MEMORY`，`0x0002`）或底层 VFS 打开时返回了 `SQLITE_OPEN_MEMORY` 标志，则设为 `PAGER_JOURNALMODE_MEMORY`；其余情形保持 `sqlite3MallocZero` 分配后的零值，也就是 `PAGER_JOURNALMODE_DELETE`。

切换模式则由 `sqlite3PagerSetJournalMode` 完成。它接收一个新的模式常量并在允许时写入 `pPager->journalMode`，返回切换后的实际模式（可能与请求不同）。`sqlite3PagerGetJournalMode` 只是 `return (int)pPager->journalMode;`，不做任何加工。`sqlite3PagerOkToChangeJournalMode` 是一个准入门禁：当 `pPager->eState` 达到或超过 `PAGER_WRITER_CACHEMOD` 时返回 0，表示数据库已被修改过、不允许再改模式。

这层只管模式标志的读写与门禁。实际写 journal、回放回滚、提交两阶段都另有归属；WAL 也不在这层。

## 怎么实现的

### 打开：`sqlite3PagerOpen`

`sqlite3PagerOpen` 的第一个参数是 VFS，第二个是输出 `Pager*`，第三个是数据库文件名，第四个参数 `nExtra` 是每页额外分配的字节数，第五个 `flags` 是 `PAGER_*` 标志位组合，第六个 `vfsFlags` 透传给 VFS 的 `xOpen`，第七个是页重初始化回调 `xReinit`。

打开流程按顺序是：

1. **计算 journal 文件句柄大小**：`journalFileSize = ROUND8(sqlite3JournalSize(pVfs));`。`sqlite3JournalSize`（`src/memjournal.c`）返回 `MAX(pVfs->szOsFile, (int)sizeof(MemJournal))`。注释明确说明每个 pager 需要两个 journal 文件句柄——主日志与子日志（sub-journal）。

2. **处理内存库标志**：若 `flags` 含 `PAGER_MEMORY`，置 `memDb=1`；若同时给了非空文件名，则把文件名复制到 `zPathname` 并把 `zFilename` 置 0，使后面走「临时文件」分支。

3. **求全路径**：若 `zFilename` 非空，分配 `2*(pVfs->mxPathname+1)` 字节并调用 `sqlite3OsFullPathname`。若返回 `SQLITE_OK_SYMLINK` 且 `vfsFlags` 含 `SQLITE_OPEN_NOFOLLOW`，改判为 `SQLITE_CANTOPEN_SYMLINK`。随后有一个关键长度检查：`if( rc==SQLITE_OK && nPathname+8>pVfs->mxPathname ) rc = SQLITE_CANTOPEN_BKPT;`——因为回滚日志路径是数据库路径加 8 字节后缀（`-journal`），路径放不下就连日志都开不了。

4. **一次性分配整块内存**：`sqlite3MallocZero` 申请一块连续内存，内部按固定顺序切分出 `Pager` 结构、`PCache` 对象、主库文件句柄（`pVfs->szOsFile` 字节）、两个 journal 文件句柄（各 `journalFileSize` 字节）、一个指回 `Pager*` 的指针、4 字节数据库前缀、数据库文件名、URI 查询参数、journal 文件名（`nPathname+8+1`）、WAL 文件名（`nPathname+4+1`，受 `SQLITE_OMIT_WAL` 条件编译保护）以及 3 字节终止符。源码注释明确说明这个顺序与分隔符格式不能改，因为有第三方软件依赖「从 WAL 文件名反查数据库文件名」这种写法。

5. **拼装 journal 与 WAL 文件名**：journal 名 = 数据库路径 + 字面量 `-journal`；WAL 名 = 数据库路径 + 字面量 `-wal`。两者都受 `SQLITE_ENABLE_8_3_NAMES` 条件编译下的 `sqlite3FileSuffix3` 影响。

6. **打开主库文件**：`sqlite3OsOpen(pVfs, pPager->zFilename, pPager->fd, vfsFlags, &fout)`。`fout` 回传 VFS 实际打开的模式，据此判定 `pPager->memVfs`（`fout&SQLITE_OPEN_MEMORY`）与 `readOnly`（`fout&SQLITE_OPEN_READONLY`）。如果 URI 参数 `immutable` 为真，强制只读并跳过下面一段，直接跳到 `act_like_temp_file` 标签。

7. **选默认页大小**：初始值 `szPageDflt = SQLITE_DEFAULT_PAGE_SIZE`（`src/sqliteLimit.h` 中默认定义为 4096）。若可写，先调 `setSectorSize(pPager)` 取扇区大小；若 `szPageDflt` 小于 `pPager->sectorSize`，则取扇区大小，但扇区大小超过 `SQLITE_MAX_DEFAULT_PAGE_SIZE`（默认 8192）时改用 `SQLITE_MAX_DEFAULT_PAGE_SIZE`。定义了 `SQLITE_ENABLE_ATOMIC_WRITE` 时，还会从 `szPageDflt` 起按 2 的幂逐级上探到 `SQLITE_MAX_DEFAULT_PAGE_SIZE`，检查设备特性里是否带 `SQLITE_IOCAP_ATOMIC` 或对应的原子写能力位（`ii>>8`），命中就把 `szPageDflt` 提升到该档。这里还读取 `sqlite3OsDeviceCharacteristics` 的 `SQLITE_IOCAP_IMMUTABLE` 位，命中则改为只读并跳到 `act_like_temp_file`。

8. **临时文件/内存库分支**（`act_like_temp_file` 标签）：`tempFile=1`，`pPager->eState=PAGER_READER`、`pPager->eLock=EXCLUSIVE_LOCK`（假装已持锁），`pPager->noLock=1`（不加锁），`readOnly` 取自 `vfsFlags&SQLITE_OPEN_READONLY`。注释说明临时文件的文件要到第一次 `OsWrite()` 才真正打开，且内存库走的是同一个分支——内存库就是一个永不落盘、用内存回滚日志的临时文件。

9. **设页大小与开 PCache**：调用 `sqlite3PagerSetPagesize(pPager, &szPageDflt, -1)`（该函数为每页额外分配 `pageSize+8` 字节，末尾 8 字节清零，用途见其源码注释「8 bytes of zeroed overrun space is sufficient so that the b-tree cell header parser will never run off the end of the allocation」），再调 `sqlite3PcacheOpen(szPageDflt, nExtra, !memDb, !memDb?pagerStress:0, (void *)pPager, pPager->pPCache)`。`nExtra` 先经 `ROUND8` 取整并有断言 `nExtra>=8 && nExtra<1000`。

10. **出错回滚**：任何一步返回非 `SQLITE_OK`，则 `sqlite3OsClose(pPager->fd)`、`sqlite3PageFree(pPager->pTmpSpace)`、`sqlite3_free(pPager)` 后直接返回错误码。

11. **填写角色相关字段**：`pPager->useJournal = useJournal`（`useJournal` 由 `flags & PAGER_OMIT_JOURNAL` 决定）；`pPager->mxPgno = SQLITE_MAX_PAGE_COUNT`（`src/sqliteLimit.h` 中为 `0xfffffffe`）；`pPager->tempFile`、`pPager->exclusiveMode`、`pPager->changeCountDone`、`pPager->memDb`、`pPager->readOnly` 依次赋值；`sqlite3PagerSetFlags(pPager, (SQLITE_DEFAULT_SYNCHRONOUS+1)|PAGER_CACHESPILL)` 设置同步级别（`SQLITE_DEFAULT_SYNCHRONOUS` 在 `src/sqliteInt.h` 中默认定义为 2，内部 `PAGER_SYNCHRONOUS_*` 是一基编号，所以这里要 +1 对应 FULL）；`pPager->journalSizeLimit = SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT`（`src/pager.h` 中默认定义为 -1，即不限制）；最后才按角色定 journal 模式——`!useJournal` → `PAGER_JOURNALMODE_OFF`，`memDb || memJM` → `PAGER_JOURNALMODE_MEMORY`，其余保持 DELETE。

### 读取：`sqlite3PagerGetJournalMode`

函数体只有一行 `return (int)pPager->journalMode;`，不做状态校验也不做转换。

### 设置：`sqlite3PagerSetJournalMode`

函数体逻辑：

1. 先用 `eOld = pPager->journalMode` 记住旧模式，并对入参做完整断言（只接受六个模式常量之一）。
2. 断言 `pPager->tempFile==0 || eMode!=PAGER_JOURNALMODE_WAL`，注释说明「This routine is only called from the OP_JournalMode opcode, and the logic there will never allow a temporary file to be changed to WAL mode」。
3. **内存库降级**：若 `MEMDB` 宏为真（`src/pager.c` 中该宏在 `SQLITE_OMIT_MEMORYDB` 未定义时展开为 `pPager->memDb`），则当 `eMode` 既不是 `PAGER_JOURNALMODE_MEMORY` 也不是 `PAGER_JOURNALMODE_OFF` 时，把 `eMode` 改回 `eOld`——内存库只允许这两种模式。
4. 若 `eMode==eOld` 则什么都不做，直接落到返回。
5. 否则写入 `pPager->journalMode = (u8)eMode`，然后处理「删除旧日志文件」这一优化：条件是 `!pPager->exclusiveMode && (eOld & 5)==1 && (eMode & 1)==0`。这个位运算是有意的编码——源码断言 `PAGER_JOURNALMODE_TRUNCATE & 5 == 1`、`PAGER_JOURNALMODE_PERSIST & 5 == 1`、`PAGER_JOURNALMODE_DELETE & 5 == 0`、`PAGER_JOURNALMODE_MEMORY & 5 == 4`、`PAGER_JOURNALMODE_OFF & 5 == 0`、`PAGER_JOURNALMODE_WAL & 5 == 5`。也就是说 `& 5` 把六个模式分成「会留下日志文件的」（TRUNCATE/PERSIST，位 0 置位）与「不留的」（DELETE/OFF，位 0 清零），而 WAL 的 `& 5` 是 5，位 0 也置位，因此从 WAL 切出时也会进入这条删除分支（此时删的是 WAL 文件，由上层 `sqlite3PagerCloseWal` 负责，见 `src/vdbe.c` 的 `OP_JournalMode` 分支）。
6. 删除动作需要保护：先 `sqlite3OsClose(pPager->jfd)`，若当前 `pPager->eLock>=RESERVED_LOCK` 则直接 `sqlite3OsDelete(pPager->pVfs, pPager->zJournal, 0)`；否则若状态是 `PAGER_OPEN` 先调 `sqlite3PagerSharedLock(pPager)` 拿共享锁，状态变为 `PAGER_READER` 后再调 `pagerLockDb(pPager, RESERVED_LOCK)` 拿保留锁，成功后才 `sqlite3OsDelete`。收尾时若状态是 `PAGER_READER` 则 `pagerUnlockDb(pPager, SHARED_LOCK)`，若状态是 `PAGER_OPEN` 则 `pager_unlock(pPager)`。源码注释说明删除日志只是优化，删不掉不算问题；且必须先拿保留锁，以免删掉别的连接正在用的日志。
7. 另一个分支：若 `eMode` 是 `PAGER_JOURNALMODE_OFF` 或 `PAGER_JOURNALMODE_MEMORY`，则 `sqlite3OsClose(pPager->jfd)` 关闭日志句柄。
8. 返回 `pPager->journalMode`（切换后的实际值）。

### 门禁：`sqlite3PagerOkToChangeJournalMode`

函数体是两条判断：若 `pPager->eState>=PAGER_WRITER_CACHEMOD` 返回 0；否则若 `NEVER(isOpen(pPager->jfd) && pPager->journalOff>0)` 也返回 0；其余返回 1。`PAGER_WRITER_CACHEMOD` 在 `src/pager.c` 中定义为 3，与 `PAGER_OPEN`（0）、`PAGER_READER`（1）、`PAGER_WRITER_LOCKED`（2）、`PAGER_ERROR`（6）构成状态序列。`journalOff` 是当前 journal 写入偏移，`journalOff>0` 说明日志里已经写过内容，此时切模式不安全。第二条带 `NEVER()` 包裹——按 SQLite 的约定，`NEVER()` 表示「理论上不该发生但仍需真实处理」的分支，因为损坏的数据库文件可能触发它。

### 调用链

`sqlite3PagerOpen` 在生产路径上的调用方是 `src/btree.c` 的 `sqlite3BtreeOpen`，它在分配 `Btree` 结构后调用 `sqlite3PagerOpen(pVfs, &pBt->pPager, zFilename, sizeof(MemPage), flags, vfsFlags, pageReinit)`，把 `sizeof(MemPage)` 作为每页额外空间、`pageReinit` 作为页重初始化回调。

journal 模式的切换入口是 `PRAGMA journal_mode`：`src/pragma.c` 的 `PragTyp_JOURNAL_MODE` 分支把模式名（`sqlite3JournalModename` 返回的小写名）转成 `PAGER_JOURNALMODE_*` 常量，然后对每个目标库发出 `OP_JournalMode` 指令。`src/vdbe.c` 的 `OP_JournalMode` 分支依次调用 `sqlite3PagerGetJournalMode` 取旧模式、用 `sqlite3PagerOkToChangeJournalMode` 做准入判断（不允许就退回旧模式）、对临时文件或 `sqlite3PagerWalSupported` 返回假的情况拒绝转入 WAL、进出 WAL 时调 `sqlite3PagerCloseWal` 与 `sqlite3BtreeSetVersion`，最后调 `sqlite3PagerSetJournalMode` 落地并用 `sqlite3JournalModename` 把结果转回字符串。

其他读取方：`src/backup.c` 的备份逻辑用 `sqlite3PagerGetJournalMode` 判断目标库是否处于 WAL 模式；`src/vacuum.c` 同样读取它；`src/vdbeaux.c` 用 `sqlite3PagerGetJournalMode(pPager)` 的结果作为 `aMJNeeded` 数组下标判断是否需要主日志。

## 关键取舍

- **模式用整数编码而不是枚举/字符串**：`src/pager.h` 注释说明这些数值经 `PRAGMA journal_mode` 对外暴露，改动会破坏兼容性，因此锁死在 `0`–`5`。代价是必须用位运算 `& 5` 去做模式分组，`sqlite3PagerSetJournalMode` 里那一串 `assert((PAGER_JOURNALMODE_XXX & 5)==...)` 就是这个选择的成本。

- **打开时一次性分配整块内存**：`sqlite3PagerOpen` 用一次 `sqlite3MallocZero` 切出 Pager、PCache、三个文件句柄和全部文件名。好处是打开路径上分配次数固定、不会出现部分初始化状态；代价是文件名区按 `nPathname` 上界预留（journal 名预留 `nPathname+8+1`，WAL 名预留 `nPathname+4+1`），路径长度直接决定内存开销，因此才有 `nPathname+8>pVfs->mxPathname` 就拒绝打开的检查。

- **临时文件与内存库共用一条打开分支**：源码注释明确「An in-memory database is the same as a temp-file that is never written out to disk and uses an in-memory rollback journal」。这条分支把 `eState` 预设为 `PAGER_READER`、`eLock` 预设为 `EXCLUSIVE_LOCK`、`noLock` 置 1，省掉了真实加锁与文件打开的开销。

- **模式切换失败时静默退回旧模式而非报错**：`sqlite3PagerSetJournalMode` 对内存库降级、对 `PRAGMA` 层级的多种限制，都走「改回 `eOld` 再返回」的路子；`OP_JournalMode` 在 `rc` 非零时也把 `eNew` 置回 `eOld`。这样 `PRAGMA journal_mode` 的语义是「尽力切换并返回结果」，而不是「要么切换成功要么报错」。

- **删日志文件只是优化**：`sqlite3PagerSetJournalMode` 里删除旧日志的整段逻辑，源码注释写明「Deleting the journal file here is an optimization only」，删不掉不报错。为避免删掉别人在用的文件，删除前要拿 `RESERVED_LOCK`，这会让一次模式切换产生额外的锁状态往返（`PAGER_OPEN` → `sqlite3PagerSharedLock` → `PAGER_READER` → `pagerLockDb(RESERVED_LOCK)`）。

- **`sqlite3PagerOkToChangeJournalMode` 用状态位而非显式标志**：`eState>=PAGER_WRITER_CACHEMOD` 这一条把「数据库已被修改」与「禁止改模式」合成一个判断，省掉单独标志位；代价是模式变更的准入条件被耦合到 pager 状态机上，改动状态机就要重新审视这条门禁。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/pager.c` | `sqlite3PagerOpen` | 打开数据库文件，分配 Pager/PCache/句柄/文件名区，并按 flags 与文件属性确定初始 journal 模式 |
| `src/pager.c` | `sqlite3PagerGetJournalMode` | 直接返回 `pPager->journalMode` |
| `src/pager.c` | `sqlite3PagerSetJournalMode` | 切换 journal 模式，含内存库降级、删除旧日志文件、关闭日志句柄等分支 |
| `src/pager.c` | `sqlite3PagerOkToChangeJournalMode` | 依据 `eState` 与 `journalOff` 判定当前是否允许切换模式 |
| `src/pager.h` | `PAGER_JOURNALMODE_*` | 六种 journal 模式的整型编码（DELETE 0 / PERSIST 1 / OFF 2 / TRUNCATE 3 / MEMORY 4 / WAL 5） |
| `src/pager.h` | `PAGER_OMIT_JOURNAL`、`PAGER_MEMORY` | `sqlite3PagerOpen` 的 flags 位，分别决定省略回滚日志与使用内存库 |
| `src/btree.c` | `sqlite3BtreeOpen` | `sqlite3PagerOpen` 的生产调用方 |
| `src/vdbe.c` | `OP_JournalMode` | `PRAGMA journal_mode` 的执行入口，编排查询、准入判断与模式切换 |
| `src/pragma.c` | `sqlite3JournalModename` | 模式常量与小写模式名的双向映射入口 |
| `src/pager.c` | `setSectorSize` | 决定 `pPager->sectorSize`，临时文件固定取 512 |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己实现的 `storage/pager.c`，与 SQLite 的 Pager 是两套独立代码，符号名相同不表示语义相同。主核若复用本机制，值得带走的是「角色在打开阶段一次性定下来、切换阶段只改标志位」这一分层：把「这个库是主库/临时库/内存库、是否用日志」在构造期定清楚，后续的事务路径就不必反复追问。SQLite 这边的一个具体教训是：模式准入门禁必须绑定在真实的状态机上（`eState>=PAGER_WRITER_CACHEMOD`），凭「当前没有未提交改动」这类外部布尔量判断会在并发下失效。

AI 侧不直接参与这一层：journal 模式是持久性协议的选择，取舍依据是崩溃安全与吞吐，不是模型能力。但 AI 应用若通过 SQLite 承载向量索引等大对象数据，会直接撞上模式选择的后果——`PAGER_JOURNALMODE_MEMORY` 与 `PAGER_JOURNALMODE_OFF` 都放弃崩溃安全，`PRAGMA journal_mode=OFF` 在 `SQLITE_Defensive` 打开时会被 `src/pragma.c` 降级为查询而非设置。这些是应用层决策，需要在选型时显式说明，而不是让默认值静默决定。

## 参考

- [SQLite 官方文档：PRAGMA journal_mode](https://www.sqlite.org/pragma.html#pragma_journal_mode)
- [SQLite 官方文档：Atomic Commit In SQLite](https://www.sqlite.org/atomiccommit.html)
- [SQLite 官方文档：In-Memory Databases](https://www.sqlite.org/inmemorydb.html)
- [SQLite 源码：src/pager.c](https://github.com/sqlite/sqlite/blob/master/src/pager.c)
- [SQLite 源码：src/pager.h](https://github.com/sqlite/sqlite/blob/master/src/pager.h)
