---
project: sqlite
feature: master-journal
page_type: feature
schema_version: 1
refs:
  - path: src/pager.c
    symbols: [sqlite3PagerSetFlags, sqlite3PagerSavepoint, sqlite3PagerOpenSavepoint]
---

# 主日志与多文件原子提交

## 一句话职责

在 SQLite 的现行源码里，跨文件原子提交由 **super-journal（超级日志）** 承担：多文件写事务提交时，`vdbeCommit` 把参与事务的各个库的**回滚日志文件名**逐个写进一个 super-journal 文件并同步该文件，然后逐库调用 `sqlite3BtreeCommitPhaseOne(pBt, zSuper)` 把 super-journal 名写进各自的回滚日志尾部，最后**删除 super-journal 文件**——删除动作就是整笔多文件事务的原子性提交点。pager 层对应的是 `sqlite3PagerSavepoint` / `sqlite3PagerOpenSavepoint` 这一对 savepoint 开关（决定哪些页进入子日志与主日志、从而决定回滚粒度），以及 `sqlite3PagerSetFlags` 决定的同步级别（决定主日志与主库文件的落盘强度）。

## 机制是什么

### 命名说明（重要）

SQLite 历史上确实存在过「master journal（主日志）」这一概念，但**在本仓库这份源码中，pager 层已经不存在名为 master journal 的实现**：全仓检索 `master_journal`、`zMaster`、`MasterJournal` 在 `src/` 下无任何命中，`SQLITE_OPEN_MASTER_JOURNAL` 只在 `src/sqlite.h.in` 作为 VFS 层的遗留标志位保留（值 `0x00004000`，注释标注「VFS only」），而 `src/vfsstat.c` 里仍保留一条把该类文件归类为「master-journal」的 `VFSSTAT_MASTERJRNL` 分支。

现行源码里承载同一职责的机制叫 **super-journal**，相关符号在 `src/pager.c` 中全部以 `SuperJournal` 命名（`writeSuperJournal`、`readSuperJournal`、`pagerIsSuperJrnlName`、`freeSuperJournal`、`pager_delsuper`），在 `src/vdbeaux.c` 的提交编排里则以变量名 `zSuper` 出现。

**架构推论：** 从「master journal」到「super journal」是一次**职责反转**：旧设计是「主日志里记录各个数据库文件的名字」，现行设计是「主日志里记录各个数据库的**回滚日志**的名字」。反转之后，主日志不再直接指向主库文件，而是指向子日志——删除主日志的语义因此从「主库已提交」变成「所有子日志都可以安全终结」，把原子性边界从「数据落库」后移到了「日志终结」这一步，与 G03 页面所述的两段式提交保持一致。

### 核心不变式

跨文件原子提交依赖三条不变式，全部能在源码里找到对应的判断点：

1. **每个参与事务的库，其回滚日志尾部都记着同一个 super-journal 名。** 由 `writeSuperJournal` 保证：它把 super-journal 名写进该库回滚日志的末尾，格式由函数头注释给出。
2. **只要 super-journal 文件还在磁盘上，各库就都还处于「可回滚」状态。** 由 `pager_playback` 保证：回放前先 `readSuperJournal` 读出日志尾部记录的 super-journal 名，再用 `sqlite3OsAccess(pVfs, zSuper, SQLITE_ACCESS_EXISTS, &res)` 确认该文件是否存在；不存在则说明该日志不是 hot journal，无需回放。
3. **super-journal 只在所有子日志都回滚完毕后才能删除。** 由 `pager_delsuper` 保证：它遍历 super-journal 内容里列出的每个子日志名，逐个打开并 `readSuperJournal` 检查它是否仍指向本 super-journal；只要有一个还指向，就不删。

### 哪些场景需要 super-journal

`vdbeCommit`（`src/vdbeaux.c`）里有一个静态表 `aMJNeeded[]`，逐项对应 journal-mode 的枚举值，给出该模式**是否需要 super-journal**：

| `journalMode` 枚举值 | `aMJNeeded[]` 项 | 是否需要 super-journal |
|---|---|---|
| `PAGER_JOURNALMODE_DELETE`（0） | 1 | 需要 |
| `PAGER_JOURNALMODE_PERSIST`（1） | 1 | 需要 |
| `PAGER_JOURNALMODE_OFF`（2） | 0 | 不需要 |
| `PAGER_JOURNALMODE_TRUNCATE`（3） | 1 | 需要 |
| `PAGER_JOURNALMODE_MEMORY`（4） | 0 | 不需要 |
| `PAGER_JOURNALMODE_WAL`（5） | 0 | 不需要 |

枚举值本身来自 `src/pager.c` 里 `sqlite3PagerSetJournalMode` 的断言：`PAGER_JOURNALMODE_DELETE /* 0 */`、`PAGER_JOURNALMODE_PERSIST /* 1 */`、`PAGER_JOURNALMODE_OFF /* 2 */`、`PAGER_JOURNALMODE_TRUNCATE /* 3 */`、`PAGER_JOURNALMODE_MEMORY /* 4 */`、`PAGER_JOURNALMODE_WAL /* 5 */`。`aMJNeeded[]` 的下标就是 `sqlite3PagerGetJournalMode(pPager)` 的返回值。

### 本机制的边界

负责附加库场景下的跨文件提交：把各库的名字记进主日志，提交时同步主日志作为原子性边界；不负责单库事务（M 组）、不负责 WAL（H 组）。

## 怎么实现的

### 一、提交侧：`vdbeCommit` 的多文件分支

入口是 `src/vdbeaux.c` 的 `vdbeCommit`（静态），由 `sqlite3VdbeHalt` 调用。它的流程分三段。

#### 1）统计参与库、判定是否需要 super-journal

`vdbeCommit` 先调 `sqlite3VtabSync(db, p)`，源码注释解释了为什么必须排在判定之前：「This has to be done before determining whether a super-journal file is required, as an xSync() callback may add an attached database to the transaction.」

随后遍历 `db->aDb`，对每个 `sqlite3BtreeTxnState(pBt)==SQLITE_TXN_WRITE` 的库：置 `needXcommit = 1`，取 `sqlite3BtreePager(pBt)`，然后判断

```c
if( db->aDb[i].safety_level!=PAGER_SYNCHRONOUS_OFF
 && aMJNeeded[sqlite3PagerGetJournalMode(pPager)]
 && sqlite3PagerIsMemdb(pPager)==0
){
  nTrans++;
}
```

三个条件分别是：**同步级别不是 OFF**（`safety_level` 不等于 `PAGER_SYNCHRONOUS_OFF`）、**journal-mode 需要 super-journal**（查 `aMJNeeded[]` 表）、**不是内存库或临时库**（`sqlite3PagerIsMemdb`）。满足才 `nTrans++`。同时对该 pager 调 `sqlite3PagerExclusiveLock(pPager)` 拿排他锁。

#### 2）单库快路径

若 `sqlite3BtreeGetFilename(db->aDb[0].pBt)` 返回的字符串长度为 **0**（主库是 `:memory:` 或临时文件），或 `nTrans<=1`，走简单分支：逐库 `sqlite3BtreeCommitPhaseOne(pBt, 0)`（第二个参数 `0` 表示没有 super-journal），全部成功后再逐库 `sqlite3BtreeCommitPhaseTwo(pBt, 0)`。

源码注释说明了 `:memory:` 的特殊处理：「If the return value of sqlite3BtreeGetFilename() is a zero length string, it means the main database is :memory: or a temp file. In that case we do not support atomic multi-file commits, so use the simple case then too.」

#### 3）多库分支：建 super-journal、写库名、同步、提交、删除

多库分支（位于 `#ifndef SQLITE_OMIT_DISKIO` 内）的执行顺序：

**选名。** 先 `zSuper = sqlite3MPrintf(db, "%.4c%s%.16c", 0, zMainFile, 0); zSuper += 4;` —— 前面 4 字节是分配长度前缀，后面 16 字节是 `\0` + 8 字节随机数 + 4 字节 `\0`，用于内存分配器的尺寸对齐（`freeSuperJournal` 里用 `sqlite3_free(&zSuper[-4])` 回收）。

随后是一个 `do { ... } while( rc==SQLITE_OK && res );` 循环生成并校验文件名：

- `sqlite3_randomness(sizeof(iRandom), &iRandom);` 取 4 字节随机数。
- `sqlite3_snprintf(13, &zSuper[nMainFile], "-mj%06X9%02X", (iRandom>>8)&0xffffff, iRandom&0xff);` —— 写 **13** 字节后缀，格式是 `-mj` + 6 位十六进制 + 固定字符 `9` + 2 位十六进制。
- 固定字符 `9` 的作用由源码注释给出：「The antipenultimate character of the super-journal name must be "9" to avoid name collisions when using 8+3 filenames.」对应断言 `assert( zSuper[sqlite3Strlen30(zSuper)-3]=='9' );`。
- `sqlite3FileSuffix3(zMainFile, zSuper);` 在需要时套用 8.3 文件名后缀。
- `sqlite3OsAccess(pVfs, zSuper, SQLITE_ACCESS_EXISTS, &res)` 检查重名；已存在则重试。

重试次数有上限：`retryCount>100` 时打日志 `sqlite3_log(SQLITE_FULL, "MJ delete: %s", zSuper);` 并删掉冲突文件后跳出循环；`retryCount==1` 时打日志 `"MJ collide: %s"`。

**建文件。** `sqlite3OsOpenMalloc(pVfs, zSuper, &pSuperJrnl, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_EXCLUSIVE|SQLITE_OPEN_SUPER_JOURNAL, 0)`。这里用到的 `SQLITE_OPEN_SUPER_JOURNAL` 值是 `0x00004000`（`src/sqlite.h.in`），与遗留的 `SQLITE_OPEN_MASTER_JOURNAL` 同值但不同名——`src/sqlite.h.in` 中 `SQLITE_OPEN_SUPER_JOURNAL` 定义为 `0x00004000`，`SQLITE_OPEN_MASTER_JOURNAL` 同样定义为 `0x00004000`。

**写库名。** 遍历 `db->aDb`，对每个 `sqlite3BtreeTxnState(pBt)==SQLITE_TXN_WRITE` 的库取 `sqlite3BtreeGetJournalname(pBt)`（它内部返回 `sqlite3PagerJournalname(pPager)`，即 `pPager->zJournal`）；若为 `0` 则跳过（源码注释：「Ignore TEMP and :memory: databases」）。否则：

```c
rc = sqlite3OsWrite(pSuperJrnl, zFile, sqlite3Strlen30(zFile)+1, offset);
offset += sqlite3Strlen30(zFile)+1;
```

每个名字后跟 **1** 个 `0x00` 终止字节。`pager_delsuper` 的函数头注释给出同样的描述：「the end of each child journal file is marked with a nul-terminator byte (0x00). i.e. the entire contents of a super-journal file for a transaction involving two databases might be: `"/home/bill/a.db-journal\x00/home/bill/b.db-journal\x00"`」。写失败则关闭并删除 super-journal，返回错误。

**同步 super-journal。**

```c
if( 0==(sqlite3OsDeviceCharacteristics(pSuperJrnl)&SQLITE_IOCAP_SEQUENTIAL)
 && SQLITE_OK!=(rc = sqlite3OsSync(pSuperJrnl, SQLITE_SYNC_NORMAL))
){ ... }
```

即：设备不支持顺序写特性时才需要显式同步；同步级别固定为 `SQLITE_SYNC_NORMAL`（**不是** `SQLITE_SYNC_FULL`）。

**提交第一阶段。** 逐库 `sqlite3BtreeCommitPhaseOne(pBt, zSuper)`。这个调用把 super-journal 名写进每个库的回滚日志尾部，并同步主库文件。源码注释说明了错误时的处理原则：「If an error occurs here, do not delete the super-journal file. If the error occurs during the first call to sqlite3BtreeCommitPhaseOne(), then there is a chance that the super-journal file will be orphaned. But we cannot delete it, in case the super-journal file name was written into the journal file before the failure occurred.」

**删除 super-journal —— 原子性提交点。** `rc = sqlite3OsDelete(pVfs, zSuper, 1);`（第二个参数 `1` 表示同时同步目录）。源码注释：「Delete the super-journal file. This commits the transaction. After doing this the directory is synced again before any individual transaction files are deleted.」

**提交第二阶段。** 删除成功后，逐库 `sqlite3BtreeCommitPhaseTwo(pBt, 1)`。这一步被刻意降级为「尽力而为」：

```c
disable_simulated_io_errors();
sqlite3BeginBenignMalloc();
for(i=0; i<db->nDb; i++){ ... sqlite3BtreeCommitPhaseTwo(pBt, 1); }
sqlite3EndBenignMalloc();
enable_simulated_io_errors();
```

源码注释：「All files and directories have already been synced, so the following calls to sqlite3BtreeCommitPhaseTwo() are only closing files and deleting or truncating journals. If something goes wrong while this is happening we don't really care. The integrity of the transaction is already guaranteed, but some stray 'cold' journals may be lying around. Returning an error code won't help matters.」

### 二、pager 层：`writeSuperJournal` 的写入格式

`src/pager.c` 的 `writeSuperJournal`（静态）是 super-journal 名进入回滚日志的唯一入口。函数头注释给出精确的字节布局：

```
+ 4 bytes: PAGER_SJ_PGNO.
+ N bytes: super-journal filename in utf-8.
+ 4 bytes: N (length of super-journal name in bytes, no nul-terminator).
+ 4 bytes: super-journal name checksum.
+ 8 bytes: aJournalMagic[].
```

校验和的定义也在注释里：「The super-journal page checksum is the sum of the bytes in the super-journal name, where each byte is interpreted as a signed 8-bit integer.」实现就是 `for(nSuper=0; zSuper[nSuper]; nSuper++){ cksum += zSuper[nSuper]; }`。

`PAGER_SJ_PGNO(pPager)` 展开为 `(pPager)->lckPgno`（`src/pager.h`），其值在 `src/pager.c` 的 `sqlite3PagerOpen` 里被设为 `(Pgno)(PENDING_BYTE/pageSize) + 1`。`src/pager.h` 注释解释了它的用途：「Page number PAGER_SJ_PGNO is never used in an SQLite database (it is reserved for working around a windows/posix incompatibility). It is used in the journal to signify that the remainder of the journal file is devoted to storing a super-journal name - there are no more pages to roll back.」

写入顺序在 `writeSuperJournal` 函数体里是：先 `write32bits(pPager->jfd, iHdrOff, PAGER_SJ_PGNO(pPager))`，再 `sqlite3OsWrite(pPager->jfd, zSuper, nSuper, iHdrOff+4)`，再 `write32bits(..., iHdrOff+4+nSuper, nSuper)`（长度），再 `write32bits(..., iHdrOff+4+nSuper+4, cksum)`（校验和），最后 `sqlite3OsWrite(pPager->jfd, aJournalMagic, 8, iHdrOff+4+nSuper+8)`。

三处前置条件会让它直接返回 `SQLITE_OK` 而不写任何东西：`!zSuper`（单库事务）、`pPager->journalMode==PAGER_JOURNALMODE_MEMORY`（内存日志）、`!isOpen(pPager->jfd)`（日志没打开）。这三条正是 `aMJNeeded[]` 把 MEMORY 标为「不需要 super-journal」的对应实现。

若 `pPager->fullSync` 为真，写入前先把 `pPager->journalOff` 推到下一个扇区边界 `journalHdrOffset(pPager)`，源码注释：「In case the previous page written to the journal has already been synced.」写完后 `pPager->journalOff += (nSuper+20);` —— **20** = 4（页号）+ 4（长度）+ 4（校验和）+ 8（魔数）。

**持久日志模式下的截断。** 写完后函数检查 `sqlite3OsFileSize(pPager->jfd, &jrnlSize)`，若 `jrnlSize>pPager->journalOff` 则 `sqlite3OsTruncate(pPager->jfd, pPager->journalOff)`。源码注释说明了原因：「the physical journal-file may extend past the end of the super-journal name and 8 bytes of magic data just written to the file. This is dangerous because the code to rollback a hot-journal file will not be able to find the super-journal name to determine whether or not the journal is hot.」

### 三、读取侧：`readSuperJournal` 的校验

`readSuperJournal`（`src/pager.c`，静态）从回滚日志尾部读回 super-journal 名。入参 `nSuper` 是允许的最大名字长度，由 `pager_playback` 传入 `1+(i64)pPager->pVfs->mxPathname`。

它先读文件大小 `szJ`，再从 `szJ-16` 处读 4 字节长度 `len`，从 `szJ-12` 处读 4 字节校验和 `cksum`，从 `szJ-8` 处读 8 字节魔数 `aMagic`。以下任一条成立就直接返回、视作没有 super-journal：

- `szJ<16`
- `len>=nSuper`（名字超长）
- `len>szJ-16`
- `len==0`

读出名字后做四项校验，任一不过就 `freeSuperJournal(zOut); zOut = 0;`：

- `!pagerIsSuperJrnlName(zOut)` —— 名字格式不合法
- `cksum` 非零 —— 校验和不对（读的时候逐字节减，`for(u=0; u<len; u++){ cksum -= zOut[u]; }`）
- `memcmp(aMagic, aJournalMagic, 8)!=0` —— 魔数不对

### 四、名字格式校验：`pagerIsSuperJrnlName`

`pagerIsSuperJrnlName`（`src/pager.c`，静态）是防「伪造日志让 SQLite 删任意文件」的第一道闸门。函数头注释列出两种合法格式：

1. **8.3 文件名模式**（仅在 `SQLITE_ENABLE_8_3_NAMES` 打开时生效）：倒数第 3 个字符是 `'9'`，再往后的 2 个字符都是十六进制数字，且倒数第 4 个字符是 `'.'`。代码里的判断是 `if( zSuper[nSuper-3]!='9' ) return 0;`、两次 `sqlite3Isxdigit`、以及 `if( zSuper[nSuper-4]=='.' ) return 1;`。
2. **通用模式**：`nSuper<12` 返回 0；`memcmp(&zSuper[nSuper-12], "-mj", 3)` 不为 0 返回 0；`zSuper[nSuper-3]!='9'` 返回 0；然后 `for(ii=nSuper-9; ii<nSuper; ii++)` 循环检查每个字节 `sqlite3Isxdigit(zSuper[ii])`，非十六进制返回 0。

`pager_delsuper` 的第一件事就是再查一次这个函数：`if( NEVER(pagerIsSuperJrnlName(zSuper)==0) ){ return SQLITE_OK; }`，注释写明：「This is to limit the degree to which a crafted journal file can be used to cause SQLite to delete arbitrary files.」

### 五、回滚侧：`pager_delsuper` 的删除判定

`pager_delsuper`（`src/pager.c`，静态）在「某个子日志刚被回滚完」时判断能否删除 super-journal。步骤：

1. `pagerIsSuperJrnlName` 校验名字，不合法直接返回 `SQLITE_OK`。
2. `sqlite3MallocZero(2 * (i64)pVfs->szOsFile)` 一次性分配两个 `sqlite3_file` 描述符（`pSuper` 与 `pJournal`，后者紧跟在 `pSuper` 之后 `pVfs->szOsFile` 字节处）。
3. `sqlite3OsOpen(pVfs, zSuper, pSuper, SQLITE_OPEN_READONLY|SQLITE_OPEN_SUPER_JOURNAL, 0)` 打开 super-journal。
4. `sqlite3OsFileSize(pSuper, &nSuperJournal)` 取大小，再 `sqlite3Malloc(4 + nSuperJournal + 2)` 一次性把整个 super-journal 内容读进内存。
5. 逐个遍历 `0x00` 分隔的子日志名：
   - 若 `strcmp(zJournal, pPager->zJournal)==0` 则置 `bSeen = 1`（这就是本 pager 自己的日志）。
   - 否则 `sqlite3OsAccess(pVfs, zJournal, SQLITE_ACCESS_EXISTS, &exists)` 检查存在性；存在则以 `SQLITE_OPEN_READONLY|SQLITE_OPEN_SUPER_JOURNAL` 打开它，调 `readSuperJournal(pJournal, 1+(u64)pVfs->mxPathname, &zSuperPtr)`，比对 `zSuperPtr!=0 && strcmp(zSuperPtr, zSuper)==0`。**匹配上就返回、不删除 super-journal**。
6. 循环结束且 `bSeen` 为真，则 `sqlite3OsDelete(pVfs, zSuper, 0)` 删除。

源码注释说明了 `bSeen` 的作用：「Only delete the super-journal if bSeen is true - indicating that the super-journal contained a pointer to this database's journal」。

调用点是 `pager_playback` 的收尾：先 `rc = pager_end_transaction(pPager, zSuper!=0, 0);`，若 `rc==SQLITE_OK && zSuper && res` 则 `rc = pager_delsuper(pPager, zSuper);`，最后 `freeSuperJournal(zSuper);`。

### 六、`pager_end_transaction` 的 `hasSuper` 参数

`pager_end_transaction(Pager *pPager, int hasSuper, int bCommit)`（`src/pager.c`，静态）中 `hasSuper` 只影响持久日志模式的终结方式：

```c
}else if( pPager->journalMode==PAGER_JOURNALMODE_PERSIST
  || (pPager->exclusiveMode && pPager->journalMode<PAGER_JOURNALMODE_WAL)
){
  rc = zeroJournalHdr(pPager, hasSuper||pPager->tempFile);
  pPager->journalOff = 0;
}
```

即：**有 super-journal 指向时，不能只清零日志头**——否则该日志会看起来像 hot journal 而触发错误的回滚。`hasSuper` 的取值来自调用方：`sqlite3PagerCommitPhaseTwo` 传 `pPager->setSuper`，`sqlite3PagerRollback`（WAL 分支）传 `pPager->setSuper`，`pager_playback` 传 `zSuper!=0`，`pagerUnlockAndRollback` 传 **0**。

`pPager->setSuper` 这个标志由 `writeSuperJournal` 在真正写入 super-journal 名时置 **1**（`pPager->setSuper = 1;`），并由 `pager_end_transaction` 收尾时清 **0**。

### 七、savepoint 层：`sqlite3PagerOpenSavepoint` 与 `sqlite3PagerSavepoint`

这两个函数不直接参与 super-journal 的创建，但它们决定了**哪些页会进入回滚日志与子日志**，从而决定了跨文件回滚的粒度——是 super-journal 机制的直接支撑。

#### `sqlite3PagerOpenSavepoint`

签名：`int sqlite3PagerOpenSavepoint(Pager *pPager, int nSavepoint);`。函数体只有两行：

```c
if( nSavepoint>pPager->nSavepoint && pPager->useJournal ){
  return pagerOpenSavepoint(pPager, nSavepoint);
}else{
  return SQLITE_OK;
}
```

即：**只有当请求的 savepoint 数超过当前已有数、且 pager 启用了日志时**才真正干活，否则是 no-op。

`pagerOpenSavepoint`（`src/pager.c`，静态，`SQLITE_NOINLINE`）做四件事：

1. `sqlite3Realloc(pPager->aSavepoint, sizeof(PagerSavepoint)*nSavepoint)` 扩容 `Pager.aSavepoint` 数组，失败返回 `SQLITE_NOMEM_BKPT`。
2. 逐个填充新结构体：`aNew[ii].nOrig = pPager->dbSize;`（savepoint 打开时的库页数）；`aNew[ii].iOffset` 取 `pPager->journalOff`（若 `isOpen(pPager->jfd) && pPager->journalOff>0`）否则取 `JOURNAL_HDR_SZ(pPager)`；`aNew[ii].iSubRec = pPager->nSubRec;`；`aNew[ii].bTruncateOnRelease = 1;`。
3. `aNew[ii].pInSavepoint = sqlite3BitvecCreate(pPager->dbSize);` —— 建位图记录哪些页属于这个 savepoint，分配失败返回 `SQLITE_NOMEM_BKPT`。
4. `if( pagerUseWal(pPager) ){ sqlite3WalSavepoint(pPager->pWal, aNew[ii].aWalData); }` —— WAL 模式下额外保存 WAL savepoint 上下文。

#### `sqlite3PagerSavepoint`

签名：`int sqlite3PagerSavepoint(Pager *pPager, int op, int iSavepoint);`。`op` 只能是 `SAVEPOINT_RELEASE` 或 `SAVEPOINT_ROLLBACK`（函数头有断言）。`iSavepoint` 为 **0** 表示最外层 savepoint，为 `Pager.nSavepoint-1` 表示最近创建的；大于该值是 no-op；**负值只允许配 `SAVEPOINT_ROLLBACK`**，表示回滚整个事务但不终止事务、不解锁。

函数体的分支：

- 初始 `int rc = pPager->errCode;`（已有错误则直接带着错误返回）。
- `if( rc==SQLITE_OK && iSavepoint<pPager->nSavepoint )` 才继续。
- 计算存活的 savepoint 数 `nNew = iSavepoint + (( op==SAVEPOINT_RELEASE ) ? 0 : 1);`，然后 `for(ii=nNew; ii<pPager->nSavepoint; ii++){ sqlite3BitvecDestroy(pPager->aSavepoint[ii].pInSavepoint); }` 并置 `pPager->nSavepoint = nNew;`。
- **RELEASE 分支**：若 `pRel->bTruncateOnRelease && isOpen(pPager->sjfd)` 且 `sqlite3JournalIsInMemory(pPager->sjfd)`（只截断内存子日志），则 `i64 sz = (pPager->pageSize+4)*(i64)pRel->iSubRec;` 后 `sqlite3OsTruncate(pPager->sjfd, sz);`，并把 `pPager->nSubRec = pRel->iSubRec;` 复位。**（`pageSize+4` 这个乘数只在这里出现，代表子日志每条记录是「页数据 + 4 字节校验和」，没有页号字段。）**
- **ROLLBACK 分支**：`else if( pagerUseWal(pPager) || isOpen(pPager->jfd) )` 则 `PagerSavepoint *pSavepoint = (nNew==0)?0:&pPager->aSavepoint[nNew-1];` 再 `rc = pagerPlaybackSavepoint(pPager, pSavepoint);`。函数头注释说明 `pSavepoint==NULL` 的含义：「the entire super-journal file. The case pSavepoint==NULL occurs when a ROLLBACK TO command is invoked on a SAVEPOINT that is a transaction savepoint.」

`pagerPlaybackSavepoint`（`src/pager.c`，静态）的实现要点：先 `pDone = sqlite3BitvecCreate(pSavepoint->nOrig);` 建位图保证每页只回滚一次；再 `pPager->dbSize = pSavepoint ? pSavepoint->nOrig : pPager->dbOrigSize;` 恢复库大小；`if( !pSavepoint && pagerUseWal(pPager) ){ return pagerRollbackWal(pPager); }` 处理 WAL 下的整事务回滚；主日志回放的上界是 `szJ = pPager->journalOff;`（源码注释：「The actual file might be larger than this in PAGER_JOURNALMODE_TRUNCATE or PAGER_JOURNALMODE_PERSIST. But anything past pPager->journalOff is off-limits to us.」）。

#### 真实调用方

| 调用方 | 文件 | 调用方式与语义 |
|---|---|---|
| `sqlite3BtreeBeginStmt` | `src/btree.c` | `rc = sqlite3PagerOpenSavepoint(pBt->pPager, iStatement);` —— 语句子事务以匿名 savepoint 形式打开，`iStatement` 是含新匿名 savepoint 在内的 savepoint 总数 |
| `sqlite3BtreeBeginTrans` / `btreeBeginTrans` | `src/btree.c` | `rc = sqlite3PagerOpenSavepoint(pPager, p->db->nSavepoint);` —— 开写事务时确保 pager 的 savepoint 数与 SQL 层一致 |
| `sqlite3BtreeSavepoint` | `src/btree.c` | `rc = sqlite3PagerSavepoint(pBt->pPager, op, iSavepoint);` —— 回滚时先 `saveAllCursors(pBt, 0, 0)` 再调 pager 层 |
| `sqlite3PagerRollback` | `src/pager.c` | `rc = sqlite3PagerSavepoint(pPager, SAVEPOINT_ROLLBACK, -1);` —— WAL 模式下整事务回滚，`iSavepoint` 传 **-1** |
| `sqlite3BtreeSetPagerFlags` | `src/btree.c` | `sqlite3PagerSetFlags(pBt->pPager, pgFlags);` —— `PRAGMA synchronous` 等的落地点 |
| `setDefaultSyncFlag` | `src/btree.c` | `sqlite3PagerSetFlags(pBt->pPager, pDb->safety_level | (db->flags & PAGER_FLAGS_MASK));` —— WAL 与回滚日志模式同步级别不同时的默认值调整 |
| `sqlite3PagerOpen` | `src/pager.c` | `sqlite3PagerSetFlags(pPager, (SQLITE_DEFAULT_SYNCHRONOUS+1)|PAGER_CACHESPILL);` —— pager 创建时的默认标志 |
| `vdbeCommit` | `src/vdbeaux.c` | `rc = sqlite3BtreeCommitPhaseOne(pBt, zSuper);` —— 多文件事务把 super-journal 名逐库下发 |
| `pager_delsuper` | `src/pager.c` | `readSuperJournal(pJournal, 1+(u64)pVfs->mxPathname, &zSuperPtr);` —— 判断子日志是否仍指向本 super-journal |
| `pager_playback` | `src/pager.c` | `readSuperJournal(pPager->jfd, 1+(i64)pPager->pVfs->mxPathname, &zSuper);` + `sqlite3OsAccess(...)` —— 判定日志是否 hot |

### 八、同步级别：`sqlite3PagerSetFlags` 如何决定主日志的落盘强度

`sqlite3PagerSetFlags(Pager *pPager, unsigned pgFlags)`（`src/pager.c`）是本页 refs 中的第三个符号。它不创建 super-journal，但决定了「主库文件与日志的同步强度」，也就是 super-journal 能否成为有效的原子性边界。

函数体第一行 `unsigned level = pgFlags & PAGER_SYNCHRONOUS_MASK;`，掩码 `PAGER_SYNCHRONOUS_MASK` 定义为 `0x07`（`src/pager.h`）。`level` 的取值对应 `src/pager.h` 的四个常量：

| `level` | 常量 | `pgFlags` 位 |
|---|---|---|
| `0x01` | `PAGER_SYNCHRONOUS_OFF` | `0x01` |
| `0x02` | `PAGER_SYNCHRONOUS_NORMAL` | `0x02` |
| `0x03` | `PAGER_SYNCHRONOUS_FULL` | `0x03` |
| `0x04` | `PAGER_SYNCHRONOUS_EXTRA` | `0x04` |

分支：

- `if( pPager->tempFile || level==PAGER_SYNCHRONOUS_OFF )` —— 临时文件或 OFF 级：置 `noSync = 1`、`fullSync = 0`、`extraSync = 0`。**这一条直接决定了 `vdbeCommit` 里 `db->aDb[i].safety_level!=PAGER_SYNCHRONOUS_OFF` 这条判定**：同步级别为 OFF 时该库不计入 `nTrans`，也就不需要 super-journal。
- 否则 `fullSync = level>=PAGER_SYNCHRONOUS_FULL ? 1 : 0;`。**`fullSync` 是 `writeSuperJournal` 里「写 super-journal 名前先对齐扇区边界」的唯一触发条件**，也是 `pager_end_transaction` 里 TRUNCATE 模式额外 `sqlite3OsSync` 的触发条件。
- `extraSync`：`if( level==PAGER_SYNCHRONOUS_EXTRA || (sqlite3OsDeviceCharacteristics(pPager->fd) & SQLITE_IOCAP_BATCH_ATOMIC) )` 置 **1**，否则置 **0**。`extraSync` 的用途在注释里写明：「SQLite syncs the directory to disk immediately after deleting a journal file in "PRAGMA journal_mode=DELETE" mode.」它在 `pager_end_transaction` 的 DELETE 分支里作为 `sqlite3OsDelete(pPager->pVfs, pPager->zJournal, pPager->extraSync)` 的第三参数出现。
- 同步原语：`if( pPager->noSync ){ pPager->syncFlags = 0; }else if( pgFlags & PAGER_FULLFSYNC ){ pPager->syncFlags = SQLITE_SYNC_FULL; }else{ pPager->syncFlags = SQLITE_SYNC_NORMAL; }`。`PAGER_FULLFSYNC` = `0x08`。
- WAL 同步标志：`pPager->walSyncFlags = (pPager->syncFlags<<2);`，若 `fullSync` 再 `pPager->walSyncFlags |= pPager->syncFlags;`；若 `pgFlags & PAGER_CKPT_FULLFSYNC` 且 `!noSync`，再 `pPager->walSyncFlags |= (SQLITE_SYNC_FULL<<2);`。`PAGER_CKPT_FULLFSYNC` = `0x10`。
- 缓存溢出标志：`if( pgFlags & PAGER_CACHESPILL ){ pPager->doNotSpill &= ~SPILLFLAG_OFF; }else{ pPager->doNotSpill |= SPILLFLAG_OFF; }`。`PAGER_CACHESPILL` = `0x20`，`SPILLFLAG_OFF` = `0x01`。

**默认值的追源。** `sqlite3PagerOpen` 里调 `sqlite3PagerSetFlags(pPager, (SQLITE_DEFAULT_SYNCHRONOUS+1)|PAGER_CACHESPILL);`。`SQLITE_DEFAULT_SYNCHRONOUS` 默认定义为 **2**（`src/sqliteInt.h`），所以传给 pager 的 `level` 是 **3**，即 `PAGER_SYNCHRONOUS_FULL`。`src/sqliteInt.h` 的注释解释了「+1」的由来：

```
**           PAGER_SYNCHRONOUS       DEFAULT_SYNCHRONOUS
**   OFF           1                         0
**   NORMAL        2                         1
**   FULL          3                         2
**   EXTRA         4                         3
```

`PRAGMA synchronous` 的数值换算在 `src/pragma.c`：`iLevel = (iLevel+1) & PAGER_SYNCHRONOUS_MASK;` 且 `if( iLevel==0 ) iLevel = 1;`，随后 `pDb->safety_level = iLevel;` 与 `setAllPagerFlags(db);`。

`PAGER_FLAGS_MASK` = `0x38`（`src/pager.h`，注释为「All above except SYNCHRONOUS」），它把 `PAGER_FULLFSYNC`(`0x08`)、`PAGER_CKPT_FULLFSYNC`(`0x10`)、`PAGER_CACHESPILL`(`0x20`) 打包成一个可整体传递的掩码。

### 九、关键常量与数值出处

| 数值 / 宏 | 含义 | 出处 |
|---|---|---|
| `20` | super-journal 名记录中除文件名外的固定字节数（4 页号 + 4 长度 + 4 校验和 + 8 魔数） | `writeSuperJournal` 的 `pPager->journalOff += (nSuper+20);` |
| `8` | `aJournalMagic[]` 魔数长度 | `writeSuperJournal` 的 `sqlite3OsWrite(pPager->jfd, aJournalMagic, 8, ...)` 与 `readSuperJournal` 的 `sqlite3OsRead(pJrnl, aMagic, 8, szJ-8)` |
| `16` | 回滚日志尾部固定区的字节数（4 长度 + 4 校验和 + 8 魔数） | `readSuperJournal` 的 `read32bits(pJrnl, szJ-16, &len)` 与 `read32bits(pJrnl, szJ-12, &cksum)` |
| `12` | `"-mj"` 后缀在文件名中的起始倒序位置 | `pagerIsSuperJrnlName` 的 `memcmp(&zSuper[nSuper-12], "-mj", 3)` |
| `13` | `sqlite3_snprintf` 写入 super-journal 后缀的字节数 | `vdbeCommit` 的 `sqlite3_snprintf(13, &zSuper[nMainFile], "-mj%06X9%02X", ...)` |
| `6` / `2` | 后缀中十六进制随机数的位数（`%06X` / `%02X`） | 同上 `sqlite3_snprintf` 格式串 |
| `9` | 后缀中固定字符的位置，用于避开 8.3 文件名冲突 | 同上格式串与 `assert( zSuper[sqlite3Strlen30(zSuper)-3]=='9' );` |
| `100` | super-journal 名冲突的最大重试次数 | `vdbeCommit` 的 `if( retryCount>100 )` 分支 |
| `1` | 写进 super-journal 的每个子日志名后的 `0x00` 终止字节 | `vdbeCommit` 的 `sqlite3OsWrite(pSuperJrnl, zFile, sqlite3Strlen30(zFile)+1, offset)` |
| `1` | 删除 super-journal 时是否同步目录 | `vdbeCommit` 的 `sqlite3OsDelete(pVfs, zSuper, 1);` |
| `0x07` | `PAGER_SYNCHRONOUS_MASK`，同步级别掩码 | `src/pager.h` |
| `0x01` / `0x02` / `0x03` / `0x04` | `PAGER_SYNCHRONOUS_OFF` / `_NORMAL` / `_FULL` / `_EXTRA` | `src/pager.h` |
| `0x08` / `0x10` / `0x20` / `0x38` | `PAGER_FULLFSYNC` / `PAGER_CKPT_FULLFSYNC` / `PAGER_CACHESPILL` / `PAGER_FLAGS_MASK` | `src/pager.h` |
| `0x01` | `SPILLFLAG_OFF` | `src/pager.c` |
| `2` | `SQLITE_DEFAULT_SYNCHRONOUS` 默认值（换算后为 `PAGER_SYNCHRONOUS_FULL`） | `src/sqliteInt.h` |
| `0x00004000` | `SQLITE_OPEN_SUPER_JOURNAL`（与遗留的 `SQLITE_OPEN_MASTER_JOURNAL` 同值） | `src/sqlite.h.in` |
| `pageSize+4` | 子日志中一条记录的字节数（页数据 + 4 字节校验和） | `sqlite3PagerSavepoint` 的 `i64 sz = (pPager->pageSize+4)*(i64)pRel->iSubRec;` |
| `PAGER_SJ_PGNO(pPager)` = `(pPager)->lckPgno` | 在日志中标记「后面是 super-journal 名，不再有页记录」的保留页号 | `src/pager.h` 的 `PAGER_SJ_PGNO` / `PAGER_SJ_PGNO_COMPUTED` |
| `aMJNeeded[]` 的 6 项 | 各 journal-mode 是否需要 super-journal | `src/vdbeaux.c` 的 `vdbeCommit` |

### 十、关键调用方（真实调用点）

| 调用方 | 文件 | 调用方式与语义 |
|---|---|---|
| `sqlite3VdbeHalt` | `src/vdbeaux.c` | `rc = vdbeCommit(db, p);` —— VDBE 停机时统一走提交编排 |
| `sqlite3BtreeCommitPhaseOne` | `src/btree.c` | `rc = sqlite3PagerCommitPhaseOne(pBt->pPager, zSuperJrnl, 0);` —— `zSuperJrnl` 为 super-journal 名，单库事务时为 `0`；`noSync` 传 **0** |
| `sqlite3BtreeGetJournalname` | `src/btree.c` | `return sqlite3PagerJournalname(p->pBt->pPager);` —— `vdbeCommit` 写 super-journal 时取得各库的日志名 |
| `sqlite3BtreeBeginStmt` | `src/btree.c` | `rc = sqlite3PagerOpenSavepoint(pBt->pPager, iStatement);` —— 打开语句级匿名 savepoint |
| `sqlite3BtreeSavepoint` | `src/btree.c` | `rc = sqlite3PagerSavepoint(pBt->pPager, op, iSavepoint);` —— 回滚前先 `saveAllCursors(pBt, 0, 0)` |
| `sqlite3BtreeSetPagerFlags` | `src/btree.c` | `sqlite3PagerSetFlags(pBt->pPager, pgFlags);` —— `PRAGMA synchronous` 的下发点 |
| `pager_playback` | `src/pager.c` | `readSuperJournal(...)` + `sqlite3OsAccess(...)` + `pager_delsuper(pPager, zSuper)` —— 回放后尝试删除 super-journal |
| `sqlite3PagerCommitPhaseTwo` | `src/pager.c` | `rc = pager_end_transaction(pPager, pPager->setSuper, 1);` —— `hasSuper` 参数决定持久日志是否清零头部 |
| `sqlite3PagerRollback` | `src/pager.c` | `rc = sqlite3PagerSavepoint(pPager, SAVEPOINT_ROLLBACK, -1);` + `pager_end_transaction(pPager, pPager->setSuper, 0);` —— WAL 模式整事务回滚 |

## 关键取舍

- **原子性边界放在「删除 super-journal」这个动作上，而不是放在 fsync 上。** 提交顺序是：写子日志名 → 同步 super-journal → 逐库 `sqlite3BtreeCommitPhaseOne`（同步各库主文件并把 super-journal 名写进各库日志）→ **删除 super-journal** → 逐库 `sqlite3BtreeCommitPhaseTwo`（清理日志）。源码注释明确：「Delete the super-journal file. This commits the transaction.」在删除之前，任何库的日志都还可回滚；删除之后，所有库的日志都被视为已提交。
- **super-journal 里记的是「回滚日志名」而不是「数据库文件名」。** 源码注释给出的内容示例是 `"/home/bill/a.db-journal\x00/home/bill/b.db-journal\x00"`。这让主日志直接指向「可回滚的证据」，删除主日志即等价于「所有回滚证据作废」。
- **删除 super-journal 后，第二阶段的错误被刻意忽略。** `vdbeCommit` 在删除成功后用 `disable_simulated_io_errors()` / `sqlite3BeginBenignMalloc()` 包住 `sqlite3BtreeCommitPhaseTwo` 循环，且不检查其返回值。源码注释：「The integrity of the transaction is already guaranteed, but some stray 'cold' journals may be lying around. Returning an error code won't help matters.」代价是可能残留冷日志，收益是提交结果不因清理失败而变成错误。
- **super-journal 写失败时删除它，写库名失败时也删除它，但第一阶段失败时不删。** 前两种情况各库的日志里还没有 super-journal 名（`pPager->setSuper` 未置位），删掉主日志不影响各库独立回滚；第三种情况已经写进去了，源码注释说明：「we cannot delete it, in case the super-journal file name was written into the journal file before the failure occurred.」代价是可能留下孤儿主日志文件，收益是绝不会出现「主库已提交但日志仍指向已删主日志」的错误回滚。
- **同步级别 OFF 的库不参与 super-journal。** `vdbeCommit` 的判定里 `db->aDb[i].safety_level!=PAGER_SYNCHRONOUS_OFF` 排在最前面，且 `aMJNeeded[]` 把 `PAGER_JOURNALMODE_OFF` 标为 **0**。这是显式的取舍：用户自己声明了「掉电可能损坏」，就不为该库付出多文件原子性的代价。
- **journal-mode 决定是否需要 super-journal，用查表而非条件链实现。** `aMJNeeded[]` 是一张 6 项的 `static const u8` 表，下标是 `sqlite3PagerGetJournalMode(pPager)` 的返回值。表项取值 DELETE/PERSIST/TRUNCATE 为 **1**，OFF/MEMORY/WAL 为 **0**。好处是新增 journal-mode 时只需加一项常量，且与 `sqlite3PagerSetJournalMode` 里断言的枚举顺序天然对齐。
- **super-journal 名字带格式约束，是为了防止恶意日志诱导删文件。** `pagerIsSuperJrnlName` 双重格式校验 + `pager_delsuper` 入口的再次校验，源码注释：「This is to limit the degree to which a crafted journal file can be used to cause SQLite to delete arbitrary files.」约束之一是倒数第 3 个字符必须是 `'9'`，注释解释：「The antipenultimate character of the super-journal name must be "9" to avoid name collisions when using 8+3 filenames.」
- **回放时先确认 super-journal 是否存在，才决定是否回滚。** `pager_playback` 里 `sqlite3OsAccess(pVfs, zSuper, SQLITE_ACCESS_EXISTS, &res)`，不存在则跳过回放。源码注释：「If a super-journal file name is specified, but the file is not present on disk, then the journal is not hot and does not need to be played back.」这把「主日志存在」变成了「事务未提交」的判据。
- **持久日志模式下有 super-journal 时不清零日志头。** `pager_end_transaction` 里 `zeroJournalHdr(pPager, hasSuper||pPager->tempFile)` 的第二参数把 `hasSuper` 传了进去。这是必需的：清零头部会让该日志不再被识别为 hot，但此时还有别的库可能需要它。
- **super-journal 的内容一次性读进内存。** `pager_delsuper` 用 `sqlite3Malloc(4 + nSuperJournal + 2)` 分配并 `sqlite3OsRead` 整个文件。源码注释的 TODO 承认这是可改进点：「This function allocates a single block of memory to load the entire contents of the super-journal file. This could be a couple of kilobytes or so - potentially larger than the page size.」
- **super-journal 的同步级别固定为 `SQLITE_SYNC_NORMAL`，而各库主文件用的是 `pPager->syncFlags`。** `vdbeCommit` 里 `sqlite3OsSync(pSuperJrnl, SQLITE_SYNC_NORMAL)`；`sqlite3PagerSync` 里 `sqlite3OsSync(pPager->fd, pPager->syncFlags)`。而 `syncFlags` 会被 `sqlite3PagerSetFlags` 按 `PAGER_FULLFSYNC` 设为 `SQLITE_SYNC_FULL`，否则为 `SQLITE_SYNC_NORMAL`。
- **savepoint 的释放会截断内存子日志以回收空间。** `sqlite3PagerSavepoint` 的 RELEASE 分支算 `sz = (pPager->pageSize+4)*(i64)pRel->iSubRec;` 后 `sqlite3OsTruncate(pPager->sjfd, sz);`，并把 `pPager->nSubRec` 复位。注释限定了条件：「Only truncate if it is an in-memory sub-journal.」
- **savepoint 的回滚有严格的上界，不允许越界回放。** `pagerPlaybackSavepoint` 把主日志回放上界设为 `pPager->journalOff` 而非文件实际大小，源码注释：「The actual file might be larger than this in PAGER_JOURNALMODE_TRUNCATE or PAGER_JOURNALMODE_PERSIST. But anything past pPager->journalOff is off-limits to us.」
- **「每个库都已提交」与「整笔事务已提交」是两件事。** 多库分支里 `sqlite3BtreeCommitPhaseOne` 逐库调用，期间任何一个失败都会导致整个 `vdbeCommit` 返回错误，此时 super-journal 不被删除，各库在崩溃后仍可回滚。

## 与主核 / AI 的关系

**架构推论：** SQLite 把「跨文件原子提交」的实现放在 **VDBE 层**（`vdbeCommit`）而不是 pager 层，意味着多文件原子性被视为**事务级语义**而非存储层语义：pager 只提供「把一个名字写进我的日志尾部」和「把这个名字读回来」两个原语，谁来组织多库协作由上层决定。主核 `engine/db/` 若要支持跨 catalog 的原子提交，编排点也应放在事务管理器而不是存储引擎内部。

**架构推论：** super-journal 是一个**外部协调文件**（存在磁盘上、内容是其它文件名），而不是一个共享内存结构。这意味着多文件原子性天然支持「提交点与协调者解耦」——写日志的进程与删主日志的进程不必是同一个。主核若用共享内存锁表做跨文件协调，就失去了「协调文件可在崩溃后被其它进程读出」这一性质，崩溃恢复时的判定会更困难。

**架构推论：** `vdbeCommit` 在删除 super-journal 之后才做各库的日志清理，并且刻意吞掉清理阶段的错误，说明**「提交已生效」与「资源已回收」被显式解耦**。这是一种「先保证语义、再保证卫生」的工程排序。

**架构推论：** `aMJNeeded[]` 用一张查表把「journal-mode → 是否需要跨文件原子性」显式化，等于把**持久化策略与事务语义的关系**变成了一张可审计的表。主核若有多套存储后端（WAL / 回滚日志 / 内存），同样需要一张类似的表来说明「哪种后端组合能提供原子性」，而不是散落在各个条件判断里。

**架构推论：** `pagerIsSuperJrnlName` 对文件名格式的强约束，配合 `readSuperJournal` 的魔数与校验和校验，构成了一条「外部输入必须先通过结构化验证才被信任」的完整链路。主核若也有跨文件的协调文件（如分布式事务的协调日志），需要同等强度的格式验证，否则一个伪造的协调文件就能诱导引擎删除任意文件。

**架构推论：** savepoint 层的两个函数（`sqlite3PagerOpenSavepoint` / `sqlite3PagerSavepoint`）虽然不直接管主日志，但它们通过「哪些页进入子日志」决定了回滚的粒度。主核的分页器若实现多级回滚点，需要在「页缓存 → 日志」的路径上建立同等的分层：主日志负责跨文件边界，子日志负责单文件内的语句级边界。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pager.c | `sqlite3PagerSetFlags` | 按 `pgFlags` 设置同步级别与落盘策略：`level = pgFlags & PAGER_SYNCHRONOUS_MASK`；OFF 级或临时文件置 `noSync=1`；`level>=PAGER_SYNCHRONOUS_FULL` 置 `fullSync`；`PAGER_SYNCHRONOUS_EXTRA` 或设备支持批量原子写置 `extraSync`；按 `PAGER_FULLFSYNC` 选 `SQLITE_SYNC_FULL`/`SQLITE_SYNC_NORMAL`；按 `PAGER_CACHESPILL` 开关缓存溢出 |
| src/pager.c | `sqlite3PagerSavepoint` | 释放或回滚 savepoint：计算存活的 savepoint 数 `nNew`、销毁被销毁者的位图；RELEASE 时按 `(pageSize+4)*iSubRec` 截断内存子日志；ROLLBACK 时调 `pagerPlaybackSavepoint` |
| src/pager.c | `sqlite3PagerOpenSavepoint` | 确保 savepoint 数足够：仅当 `nSavepoint>pPager->nSavepoint && pPager->useJournal` 时转调 `pagerOpenSavepoint`，否则返回 `SQLITE_OK` |
| src/pager.c | `pagerOpenSavepoint` | 真正开 savepoint：扩容 `Pager.aSavepoint`、填 `nOrig`/`iOffset`/`iSubRec`/`bTruncateOnRelease`、`sqlite3BitvecCreate(dbSize)` 建页位图、WAL 模式下调 `sqlite3WalSavepoint` |
| src/pager.c | `writeSuperJournal` | 把 super-journal 名写进回滚日志尾部：4 字节 `PAGER_SJ_PGNO` + 文件名 + 4 字节长度 + 4 字节校验和 + 8 字节魔数；`fullSync` 时先对齐扇区边界；持久日志模式下截断到 `journalOff` |
| src/pager.c | `readSuperJournal` | 从回滚日志尾部读回 super-journal 名：校验长度 `len` 与上界 `nSuper`、逐字节核验校验和、比对 `aJournalMagic`、用 `pagerIsSuperJrnlName` 验名字格式；任一项不过即返回空名 |
| src/pager.c | `pagerIsSuperJrnlName` | 校验 super-journal 名格式：8.3 模式（`SQLITE_ENABLE_8_3_NAMES`）或通用模式（倒数第 12 位起为 `-mj`、倒数第 3 位为 `9`、其余为十六进制） |
| src/pager.c | `pager_delsuper` | 子日志回滚完后判断能否删 super-journal：把整个主日志读进内存、遍历其中的子日志名、逐个 `readSuperJournal` 比对；仍有子日志指向本主日志则不删 |
| src/pager.c | `pager_end_transaction` | 事务收尾：`hasSuper` 决定持久日志是否只清零头部（`zeroJournalHdr(pPager, hasSuper||pPager->tempFile)`）；按 journal-mode 分四种方式终结日志；清 `setSuper` |
| src/pager.c | `pager_playback` | 回放日志：先 `readSuperJournal` + `sqlite3OsAccess` 判定是否 hot，再循环读日志头与逐页回放，最后 `pager_end_transaction(pPager, zSuper!=0, 0)` 与 `pager_delsuper(pPager, zSuper)` |
| src/pager.c | `pagerPlaybackSavepoint` | savepoint 级回放：建 `pDone` 位图保证每页只回滚一次、把 `dbSize` 复位到 `nOrig`、主日志回放上界取 `pPager->journalOff`；`pSavepoint==NULL` 且 WAL 时转 `pagerRollbackWal` |
| src/pager.c | `sqlite3PagerCommitPhaseOne` | 提交第一阶段：接受 `zSuper` 参数并传给 `writeSuperJournal`；`zSuper!=0` 时禁用批量原子写路径 |
| src/pager.c | `sqlite3PagerCommitPhaseTwo` | 提交第二阶段：`pager_end_transaction(pPager, pPager->setSuper, 1)` |
| src/pager.c | `sqlite3PagerRollback` | 回滚：WAL 分支调 `sqlite3PagerSavepoint(pPager, SAVEPOINT_ROLLBACK, -1)`，随后 `pager_end_transaction(pPager, pPager->setSuper, 0)` |
| src/pager.c | `sqlite3PagerSync` | 同步主库文件：`SQLITE_FCNTL_SYNC` 文件控制 + `sqlite3OsSync(pPager->fd, pPager->syncFlags)`；`zSuper` 只作为 `pArg` 传给文件控制 |
| src/pager.c | `freeSuperJournal` | 释放 `readSuperJournal` 分配的缓冲区：`sqlite3_free(&zSuper[-4]);`（回收前面 4 字节的分配长度前缀） |
| src/pager.c | `PAGER_SJ_PGNO` / `PAGER_SJ_PGNO_COMPUTED` | 日志中标记「其后是 super-journal 名」的保留页号；`PAGER_SJ_PGNO(x)` 展开为 `(x)->lckPgno` |
| src/pager.c | `PAGER_SYNCHRONOUS_*` / `PAGER_FULLFSYNC` / `PAGER_CKPT_FULLFSYNC` / `PAGER_CACHESPILL` / `PAGER_FLAGS_MASK` / `SPILLFLAG_OFF` | `sqlite3PagerSetFlags` 使用的全部标志位与掩码常量 |
| src/vdbeaux.c | `vdbeCommit` | 多文件提交编排：统计 `nTrans`、用 `aMJNeeded[]` 判定是否需要 super-journal、生成 `-mj%06X9%02X` 名字、写各库日志名、同步主日志、逐库 `sqlite3BtreeCommitPhaseOne`、删除主日志（提交点）、逐库 `sqlite3BtreeCommitPhaseTwo` |
| src/vdbeaux.c | `sqlite3VdbeHalt` | VDBE 停机时调用 `vdbeCommit(db, p)` |
| src/btree.c | `sqlite3BtreeCommitPhaseOne` | 把 super-journal 名透传给 pager 层：`sqlite3PagerCommitPhaseOne(pBt->pPager, zSuperJrnl, 0)` |
| src/btree.c | `sqlite3BtreeGetJournalname` | 返回 `sqlite3PagerJournalname(p->pBt->pPager)`，供 `vdbeCommit` 写入 super-journal |
| src/btree.c | `sqlite3BtreeBeginStmt` / `sqlite3BtreeSavepoint` | pager savepoint 层的两个调用方：开匿名 savepoint 与释放/回滚 savepoint |
| src/btree.c | `sqlite3BtreeSetPagerFlags` / `setDefaultSyncFlag` | `sqlite3PagerSetFlags` 的两个调用方：PRAGMA 下发与 WAL/回滚模式同步级别不一致时的默认值调整 |
| src/pager.h | `PAGER_SJ_PGNO` / `PAGER_SJ_PGNO_COMPUTED` / `PAGER_SYNCHRONOUS_*` / `PAGER_FLAGS_MASK` | 页号保留位与同步标志位的定义处，并注明 `PAGER_SJ_PGNO`「在日志中表示其后是 super-journal 名」 |
| src/sqliteInt.h | `SQLITE_DEFAULT_SYNCHRONOUS` | 默认同步级别 **2**（换算为 pager 内部 `PAGER_SYNCHRONOUS_FULL`），并给出「外部零基、内部一基」的对照表 |
| src/sqlite.h.in | `SQLITE_OPEN_SUPER_JOURNAL` / `SQLITE_OPEN_MASTER_JOURNAL` | 两者值均为 `0x00004000`；前者是现行 super-journal 的 VFS 标志，后者是遗留的 master-journal 标志（注释标注「VFS only」） |

## 参考

- [SQLite Atomic Commit In SQLite（sqlite.org 官方文档）](https://sqlite.org/atomiccommit.html)
- [SQLite Write-Ahead Logging（sqlite.org 官方文档）](https://sqlite.org/wal.html)
- [SQLite PRAGMA journal_mode（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_journal_mode)
- [SQLite PRAGMA synchronous（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_synchronous)
- [SQLite 源码浏览：src/pager.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/pager.c)
- [sqlite/sqlite 仓库中的 src/pager.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/pager.c)
- [sqlite/sqlite 仓库中的 src/vdbeaux.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbeaux.c)
