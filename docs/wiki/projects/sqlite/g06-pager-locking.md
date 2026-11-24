---
project: sqlite
feature: pager-locking
page_type: feature
schema_version: 1
refs:
  - path: src/pager.c
    symbols: [sqlite3PagerSharedLock, sqlite3PagerExclusiveLock, sqlite3PagerLockingMode]
---

# 分页器文件锁

## 一句话职责

`sqlite3PagerSharedLock` 负责在数据库文件上取得 `SHARED_LOCK`（读锁），并在拿锁后完成热日志检测与回放，把分页器从 `PAGER_OPEN` 推进到 `PAGER_READER`；`sqlite3PagerExclusiveLock` 负责在回滚日志模式的事务中把已持有的 `RESERVED_LOCK` 升级为 `EXCLUSIVE_LOCK`（写锁），是并发写入的互斥点；`sqlite3PagerLockingMode` 负责在 `normal` 与 `exclusive` 两种锁定模式之间切换，决定分页器是否长期独占数据库文件。

## 机制是什么

SQLite 的锁是**文件级**的协作式锁，不是记录级或页级锁。锁级别由 `src/os.h` 中的四个宏定义，数值决定强弱：

| 常量 | 数值 | 语义（`src/os.h` 注释原文） |
|---|---|---|
| `NO_LOCK` | `0` | 不持锁 |
| `SHARED_LOCK` | `1` | "Any number of processes may hold a SHARED lock simultaneously." |
| `RESERVED_LOCK` | `2` | "A single process may hold a RESERVED lock on a file at any time. Other processes may hold and obtain new SHARED locks." |
| `PENDING_LOCK` | `3` | "A single process may hold a PENDING lock on a file at any one time. Existing SHARED locks may persist, but no new SHARED locks may be obtained by other processes." |
| `EXCLUSIVE_LOCK` | `4` | "An EXCLUSIVE lock precludes all other locks." |

`PENDING_LOCK` 不能直接传给 `sqlite3OsLock()`，`src/os.h` 注释明确写道：「PENDING_LOCK may not be passed directly to sqlite3OsLock().」它是「申请 `EXCLUSIVE_LOCK` 但暂时拿不到」时的中间态。

锁状态由 `Pager.eLock` 记录。`src/pager.c` 的 `UNKNOWN_LOCK` 注释块说明了它必须存在的原因：由于 VFS 的 `xLock()` / `xUnlock()` 失败时无法判断操作是否真的成功，`pagerLockDb()` 与 `pagerUnlockDb()` 采取保守策略 —— 解锁时**总是**更新 `eLock`，加锁时**只在 VFS 调用成功时**更新。因此 `eLock` 的记录值低于系统实际持有的锁时属于正常现象，但**永远不会**高于实际持有的锁。唯一的例外是 `ERROR → OPEN` 状态迁移时的解锁失败：此时把 `eLock` 置为 `UNKNOWN_LOCK`（`src/pager.c` 的 `#define UNKNOWN_LOCK (EXCLUSIVE_LOCK+1)`），因为该处存在需要回滚的热日志，若此时进程其实仍持有 `EXCLUSIVE_LOCK`，`sqlite3OsCheckReservedLock()` 会误报「本进程或他人持有 RESERVED 锁」，导致热日志被误判为「别人活跃事务产生的日志」而跳过回滚。注释给出的解法是：「the code to do the OPEN->SHARED state transition omits the check for a hot-journal if Pager.eLock is set to UNKNOWN_LOCK lock. Instead, it assumes a hot-journal exists and obtains an EXCLUSIVE lock on the database file before attempting to roll it back.」

锁的**层级结构**由 `src/pager.c` 的 `sqlite3PagerSharedLock` 函数体注释给出（`OPEN -> READER` 那一段）：读事务期持 `SHARED_LOCK`；写事务开始时升到 `RESERVED_LOCK`（此时别的连接仍可读）；提交或回滚阶段升到 `EXCLUSIVE_LOCK`（此时任何别的连接都不能读写）。

**本机制的边界**（与页面清单一致）：负责在数据库文件上获取共享/排他/保留锁以及锁状态的升级降级，是并发写入的互斥点；不负责忙等待回调（A05）、不负责页缓存（I 组）。

## 怎么实现的

### 三层结构：语义层 → 等待层 → VFS 层

`src/pager.c` 把锁操作拆成三层，每层职责明确：

| 层 | 符号 | 职责 |
|---|---|---|
| 语义层 | `sqlite3PagerSharedLock` / `sqlite3PagerExclusiveLock` | 决定「什么情况下该持什么锁」，处理热日志、状态机迁移 |
| 等待层 | `pager_wait_on_lock` | 在 `SQLITE_BUSY` 时反复调用忙处理器，直到成功或回调返回 0 |
| 封装层 | `pagerLockDb` / `pagerUnlockDb` | 维护 `Pager.eLock` 的一致性，处理 `noLock` 与 `UNKNOWN_LOCK` |
| VFS 层 | `sqlite3OsLock` / `sqlite3OsUnlock` | 落到操作系统的文件锁原语 |

### `pagerLockDb`：升级锁并维护 `eLock`

`src/pager.c` 静态函数 `pagerLockDb(Pager *pPager, int eLock)` 的函数体只有一条主分支：

```c
if( pPager->eLock<eLock || pPager->eLock==UNKNOWN_LOCK ){
  rc = pPager->noLock ? SQLITE_OK : sqlite3OsLock(pPager->fd, eLock);
  if( rc==SQLITE_OK && (pPager->eLock!=UNKNOWN_LOCK||eLock==EXCLUSIVE_LOCK) ){
    pPager->eLock = (u8)eLock;
  }
}
```

三个要点：

1. **只升不降**：`pPager->eLock<eLock` 是升级条件。若已持有同级或更高级别的锁，函数体直接跳过，`sqlite3OsLock` 根本不会被调用。
2. **`noLock` 短路**：`pPager->noLock` 为真时直接返回 `SQLITE_OK`，不触碰 VFS。`noLock` 的两个来源在 `src/pager.c` 的 `sqlite3PagerOpen` 里：URI 参数 `nolock`（`pPager->noLock = sqlite3_uri_boolean(pPager->zFilename, "nolock", 0);`），以及临时文件/内存库/不可变文件路径（`act_like_temp_file` 分支里 `pPager->noLock = 1;`，同时 `pPager->eLock = EXCLUSIVE_LOCK;` 假装已独占）。
3. **`UNKNOWN_LOCK` 的特殊规则**：当 `eLock` 是 `UNKNOWN_LOCK` 时，只有目标锁是 `EXCLUSIVE_LOCK` 才更新 `eLock`。这正是 `UNKNOWN_LOCK` 注释块描述的「只有在一次成功的 `xLock(EXCLUSIVE)` 之后才恢复到真实状态」。

断言限定了合法输入：`assert( eLock==SHARED_LOCK || eLock==RESERVED_LOCK || eLock==EXCLUSIVE_LOCK );`。

### `pagerUnlockDb`：降级锁

`src/pager.c` 静态函数 `pagerUnlockDb(Pager *pPager, int eLock)` 的函数体：

- 断言 `eLock==NO_LOCK || eLock==SHARED_LOCK` —— 解锁只支持「降到 SHARED」和「完全放开」两级，不能降到 RESERVED。
- `rc = pPager->noLock ? SQLITE_OK : sqlite3OsUnlock(pPager->fd, eLock);`
- `if( pPager->eLock!=UNKNOWN_LOCK ){ pPager->eLock = (u8)eLock; }` —— 解锁时**无条件**更新 `eLock`（连 `UNKNOWN_LOCK` 也被覆盖），这正是 `UNKNOWN_LOCK` 注释块所说的保守策略。
- 收尾 `pPager->changeCountDone = pPager->tempFile;` —— 源码注释标注为 ticket `fb3b3024ea238d5c`。

### `pager_wait_on_lock`：忙等待层

`src/pager.c` 静态函数 `pager_wait_on_lock(Pager *pPager, int locktype)` 的函数体是一个 do-while：

```c
do {
  rc = pagerLockDb(pPager, locktype);
}while( rc==SQLITE_BUSY && pPager->xBusyHandler(pPager->pBusyHandlerArg) );
```

函数头注释说明：「If a similar or greater lock is already held, this function is a no-op (returning SQLITE_OK immediately).」

它的断言把**允许触发忙处理器的迁移**限定死了：

```c
assert( (pPager->eLock>=locktype)
     || (pPager->eLock==NO_LOCK && locktype==SHARED_LOCK)
     || (pPager->eLock==RESERVED_LOCK && locktype==EXCLUSIVE_LOCK)
);
```

这与 `src/pager.c` 中 `sqlite3PagerSetBusyHandler` 的函数头注释表格一致：

| 迁移 | 是否触发 `xBusyHandler` |
|---|---|
| `NO_LOCK -> SHARED_LOCK` | Yes |
| `SHARED_LOCK -> RESERVED_LOCK` | No |
| `SHARED_LOCK -> EXCLUSIVE_LOCK` | No |
| `RESERVED_LOCK -> EXCLUSIVE_LOCK` | Yes |

`sqlite3PagerSetBusyHandler` 同时把回调指针通过 `sqlite3OsFileControlHint(pPager->fd, SQLITE_FCNTL_BUSYHANDLER, (void *)ap)` 传给 VFS。忙处理器本身由 `src/btree.c` 的 `lockBtree` 通过 `sqlite3PagerSetBusyHandler(pBt->pPager, btreeInvokeBusyHandler, pBt)` 装上。

### `sqlite3PagerSharedLock`：取读锁 + 热日志处理

`src/pager.c` 中 `sqlite3PagerSharedLock(Pager *pPager)` 的前置断言限定了调用条件：页缓存引用计数必须为 **0**（`assert( sqlite3PcacheRefCount(pPager->pPCache)==0 );`），状态必须是 `PAGER_OPEN` 或 `PAGER_READER`，且 `pPager->errCode==SQLITE_OK`。函数头注释说「This routine is only called from b-tree and only when there are no outstanding pages.」

函数体的执行顺序：

1. **入口条件**：`if( !pagerUseWal(pPager) && pPager->eState==PAGER_OPEN )` —— WAL 模式或非 `PAGER_OPEN` 状态时，整段热日志逻辑跳过。
2. **取读锁**：`rc = pager_wait_on_lock(pPager, SHARED_LOCK);` 失败时断言 `pPager->eLock==NO_LOCK || pPager->eLock==UNKNOWN_LOCK`，然后 `goto failed`。
3. **热日志检测**：`if( pPager->eLock<=SHARED_LOCK ){ rc = hasHotJournal(pPager, &bHotJournal); }` —— 只有还没拿 `RESERVED_LOCK` 时才需要检测。
4. **只读库遇到热日志**：若 `bHotJournal` 且 `pPager->readOnly`，返回 `SQLITE_READONLY_ROLLBACK` 并 `goto failed`。
5. **升级到排他锁做回放**：`rc = pagerLockDb(pPager, EXCLUSIVE_LOCK);`。这里的注释解释了为什么不经过 `RESERVED_LOCK`：「it is important that a RESERVED lock is not obtained on the way to the EXCLUSIVE lock. If it were, another process might open the database file, detect the RESERVED lock, and conclude that the database is safe to read while this process is still rolling the hot-journal back.」注意这里用的是 `pagerLockDb` 而**不是** `pager_wait_on_lock` —— 与上面那张表一致，`SHARED_LOCK -> EXCLUSIVE_LOCK` 不触发忙处理器。
6. **打开日志文件**：若 `!isOpen(pPager->jfd)` 且 `pPager->journalMode!=PAGER_JOURNALMODE_OFF`，用 `sqlite3OsAccess` 判存在后，以 `SQLITE_OPEN_READWRITE|SQLITE_OPEN_MAIN_JOURNAL` 打开；若实际以只读方式打开（`fout&SQLITE_OPEN_READONLY`），返回 `SQLITE_CANTOPEN_BKPT` 并关闭句柄。
7. **回放热日志**：若日志句柄已打开，先 `pagerSyncHotJournal(pPager)` 同步，再 `pager_playback(pPager, !pPager->tempFile)`，随后置 `pPager->eState = PAGER_OPEN`；若日志未打开且 `!pPager->exclusiveMode`，则 `pagerUnlockDb(pPager, SHARED_LOCK)` 降回读锁。
8. **回放失败的错误处理**：`pager_error(pPager, rc); goto failed;`。源码注释解释为何要先置错误态：「In order to get pager_unlock() to do this, set Pager.eState to PAGER_ERROR now.」
9. **文件版本校验**：若 `!pPager->tempFile && pPager->hasHeldSharedLock`，从文件偏移 **24** 处读 `sizeof(pPager->dbFileVers)` 字节与旧值比对，不一致则 `pager_reset(pPager)` 并 `sqlite3OsUnfetch(pPager->fd, 0, 0)`。注释说明用途：「Database changes are detected by looking at 15 bytes beginning at offset 24 into the file. The first 4 of these 16 bytes are a 32-bit counter that is incremented with each change.」
10. **WAL 检测与读事务**：`rc = pagerOpenWalIfPresent(pPager);`，随后 `if( pagerUseWal(pPager) ){ rc = pagerBeginReadTransaction(pPager); }`。
11. **出口**：失败走 `failed:` 标签 —— `pager_unlock(pPager)` 并断言状态回到 `PAGER_OPEN`；成功则置 `pPager->eState = PAGER_READER` 与 `pPager->hasHeldSharedLock = 1`。

**热日志的判定标准**由 `src/pager.c` 的 `hasHotJournal` 给出，四条同时成立才算 hot：无进程持 `RESERVED` 或更高级别的锁；数据库文件本身大于 **0** 字节；日志文件首字节存在且不等于 `0x00`。`hasHotJournal` 里还有一条针对空库的特例：`nPage==0 && !jrnlOpen` 时先 `pagerLockDb(pPager, RESERVED_LOCK)` 再 `sqlite3OsDelete` 删掉残留日志，随后 `if( !pPager->exclusiveMode ) pagerUnlockDb(pPager, SHARED_LOCK);` 降锁。

### `sqlite3PagerExclusiveLock`：升级到写锁

`src/pager.c` 中 `sqlite3PagerExclusiveLock(Pager *pPager)` 的函数体极短：

```c
int rc = pPager->errCode;
assert( assert_pager_state(pPager) );
if( rc==SQLITE_OK ){
  assert( pPager->eState==PAGER_WRITER_CACHEMOD
       || pPager->eState==PAGER_WRITER_DBMOD
       || pPager->eState==PAGER_WRITER_LOCKED
  );
  if( 0==pagerUseWal(pPager) ){
    rc = pager_wait_on_lock(pPager, EXCLUSIVE_LOCK);
  }
}
return rc;
```

四个要点：

1. **WAL 模式是 no-op**：函数头注释说「If the connection is in WAL mode, this call is a no-op.」WAL 的并发控制不走文件锁，走共享内存索引。
2. **错误优先**：已有错误时直接返回 `pPager->errCode`，不做任何加锁动作。
3. **只做升级**：它假定调用方已持有 `RESERVED_LOCK`，把 `eLock` 推到 `EXCLUSIVE_LOCK`。这是 `RESERVED_LOCK -> EXCLUSIVE_LOCK` 迁移，按上面那张表会**触发忙处理器**。
4. **失败返回 `SQLITE_BUSY` 或 I/O 错误**：函数头注释原文：「Otherwise, either SQLITE_BUSY or an SQLITE_IOERR_XXX error code is returned.」

### 锁的获取与释放：完整调用链

| 时机 | 调用方 | 锁动作 |
|---|---|---|
| 首次读事务 | `src/btree.c` 的 `lockBtree` | `rc = sqlite3PagerSharedLock(pBt->pPager);` —— 取得 `SHARED_LOCK`，状态 `PAGER_OPEN -> PAGER_READER` |
| 写事务开始 | `src/pager.c` 的 `sqlite3PagerBegin` | `rc = pagerLockDb(pPager, RESERVED_LOCK);` —— 非 WAL 分支取保留锁；若 `exFlag` 为真则紧接 `rc = pager_wait_on_lock(pPager, EXCLUSIVE_LOCK);` |
| 提交前同步日志 | `src/pager.c` 的 `syncJournal` | `rc = sqlite3PagerExclusiveLock(pPager);` —— 把锁升到 `EXCLUSIVE_LOCK` |
| 多库事务前 | `src/vdbeaux.c` | `rc = sqlite3PagerExclusiveLock(pPager);` —— 在 `sqlite3BtreeEnter(pBt)` 保护下对每个数据库文件取排他锁 |
| WAL 独占模式 | `src/pager.c` 的 `sqlite3PagerBegin` | `rc = pagerLockDb(pPager, EXCLUSIVE_LOCK);` —— 条件是 `pPager->exclusiveMode && sqlite3WalExclusiveMode(pPager->pWal, -1)` |
| 打开 WAL 文件 | `src/pager.c` 的 `pagerOpenWal` | `rc = pagerExclusiveLock(pPager);` —— 条件是 `pPager->exclusiveMode` |
| 从 WAL 切回回滚模式 | `src/pager.c` 的 `sqlite3PagerCloseWal` | `rc = pagerLockDb(pPager, SHARED_LOCK);` —— 判定 WAL 文件是否存在 |
| 切换 journal_mode | `src/pager.c` 的 `sqlite3PagerSetJournalMode` | `rc = sqlite3PagerSharedLock(pPager);` 后 `rc = pagerLockDb(pPager, RESERVED_LOCK);` —— 删日志前先确认无人使用 |
| 提交/回滚结束 | `src/pager.c` 的 `pager_end_transaction` | `rc2 = pagerUnlockDb(pPager, SHARED_LOCK);` —— 从写锁降回读锁 |
| 关闭连接/出错 | `src/pager.c` 的 `pager_unlock` | `rc = pagerUnlockDb(pPager, NO_LOCK);` —— 完全放开；若此时 `rc!=SQLITE_OK && pPager->eState==PAGER_ERROR`，则 `pPager->eLock = UNKNOWN_LOCK;` |

### `sqlite3PagerLockingMode`：normal / exclusive 模式切换

`src/pager.c` 中 `sqlite3PagerLockingMode(Pager *pPager, int eMode)` 的函数体：

```c
if( eMode>=0 && !pPager->tempFile && !sqlite3WalHeapMemory(pPager->pWal) ){
  pPager->exclusiveMode = (u8)eMode;
}
return (int)pPager->exclusiveMode;
```

三个要点：

1. **`eMode` 取值范围**由 `src/pager.h` 的三个宏固定：`PAGER_LOCKINGMODE_QUERY` 为 `-1`，`PAGER_LOCKINGMODE_NORMAL` 为 `0`，`PAGER_LOCKINGMODE_EXCLUSIVE` 为 `1`。断言 `PAGER_LOCKINGMODE_QUERY<0` 与 `PAGER_LOCKINGMODE_NORMAL>=0 && PAGER_LOCKINGMODE_EXCLUSIVE>=0` 固定了「查询是负值、两种模式是非负值」的约定。
2. **`eMode>=0` 才是设置**：传 `PAGER_LOCKINGMODE_QUERY`（`-1`）是纯查询，不改变模式。
3. **临时文件与堆内存 WAL 不受影响**：`!pPager->tempFile && !sqlite3WalHeapMemory(pPager->pWal)` 是设置条件；`sqlite3WalHeapMemory` 为真意味着 WAL 索引放在堆内存里（独占模式），此时外部进程看不到，切换没有意义。

`exclusiveMode` 标志被大量锁逻辑读取，行为差异明确写在源码注释里：

- `pagerUnlockDb` 的断言 `assert( !pPager->exclusiveMode || pPager->eLock==eLock );` —— 独占模式下解锁目标必须与当前锁级别一致。
- `pagerUnlockAndRollback` 的 `else if( !pPager->exclusiveMode )` 分支 —— 非独占模式才在 `PAGER_READER` 状态调 `pager_end_transaction`。
- `sqlite3PagerSharedLock` 的 `else if( !pPager->exclusiveMode ){ pagerUnlockDb(pPager, SHARED_LOCK); }` —— 独占模式下热日志回放后**不降锁**，继续保持 `EXCLUSIVE_LOCK`。函数体末尾的断言确认了这一点：`assert( (pPager->eLock==SHARED_LOCK) || (pPager->exclusiveMode && pPager->eLock>SHARED_LOCK) );`。
- `pager_unlock` 的 `else if( !pPager->exclusiveMode )` —— 独占模式下整个数据库文件解锁分支被跳过，文件锁一直保持。

`sqlite3PagerLockingMode` 的真实调用方有三个：`src/pragma.c` 的 `PRAGMA locking_mode` 实现（`eMode = sqlite3PagerLockingMode(pPager, eMode);`，并在无库名时对所有附加库逐个调用 + 更新 `db->dfltLockMode`）、`src/attach.c`（`sqlite3PagerLockingMode(pPager, db->dfltLockMode);` 为新附加的库继承默认模式）、`src/main.c`（`sqlite3PagerLockingMode(sqlite3BtreePager(db->aDb[0].pBt), ...)` 为主库设置模式）。

### `exclusiveMode` 下的特殊路径

独占模式下还有两条专门的锁路径：

- `pagerExclusiveLock`（`src/pager.c` 静态）：「Attempt to take an exclusive lock on the database file. If a PENDING lock is obtained instead, immediately release it.」其函数体在 `pagerLockDb(pPager, EXCLUSIVE_LOCK)` 失败时调 `pagerUnlockDb(pPager, eOrigLock);`，把 Unix VFS 在升级路径上拿到的 `PENDING_LOCK` 释放掉。
- `sqlite3PagerWalSupported` 的判断：`if( pPager->noLock ) return 0; return pPager->exclusiveMode || (pMethods->iVersion>=2 && pMethods->xShmMap);` —— 独占模式可以支持 WAL 而不依赖 VFS 的共享内存方法。

### VFS 层：锁如何落到操作系统

`src/os_unix.c` 的 `unixLock`（函数头注释）给出了完整的锁协议：

- **`SHARED_LOCK`**：先在 `'pending byte'` 上取读锁，成功后在 `'shared byte range'`（**510** 字节）上取读锁，然后释放 `'pending byte'` 上的锁。
- **`RESERVED_LOCK`**：「A process may only obtain a RESERVED lock after it has a SHARED lock.」实现为在 `'reserved byte'` 上取写锁。
- **`EXCLUSIVE_LOCK`**：「An EXCLUSIVE lock may only be requested after either a SHARED or RESERVED lock is held.」实现为在整个 `'shared byte range'` 上取写锁。
- **`PENDING_LOCK`**：「If a process that holds a RESERVED lock requests an EXCLUSIVE, then a PENDING lock is obtained first.」作用是「no new SHARED locks can be obtained, but existing SHARED locks are allowed to persist.」

`unixLock` 里有三条断言固定了迁移规则：

```c
assert( pFile->eFileLock!=NO_LOCK || eFileLock==SHARED_LOCK );
assert( eFileLock!=PENDING_LOCK );
assert( eFileLock!=RESERVED_LOCK || pFile->eFileLock==SHARED_LOCK );
```

第二条正是 `src/os.h` 注释「SQLite never explicitly requests a pending lock」的落地。

同进程内的多线程共享通过 `unixInodeInfo` 里的引用计数处理：`unixLock` 中若同进程的 `pInode->eFileLock` 已是 `SHARED_LOCK` 或 `RESERVED_LOCK` 且请求 `SHARED_LOCK`，则 `pInode->nShared++`、`pInode->nLock++` 后直接返回 `SQLITE_OK`，不再走系统调用。

## 关键取舍

- **锁的粒度是文件级，不是页级。** 锁级别只有 `NO_LOCK` / `SHARED_LOCK` / `RESERVED_LOCK` / `PENDING_LOCK` / `EXCLUSIVE_LOCK` 五个（定义在 `src/os.h`），没有页号或记录号参与。取舍是：实现简单、跨平台行为一致（Windows 用 `LockFile()`，Unix 用 POSIX `fcntl` 记录锁），代价是写者必须等所有读者退出读事务才能提交。
- **`eLock` 宁可记松，不记严。** `src/pager.c` 的 `UNKNOWN_LOCK` 注释块明确写出：「the Pager.eLock variable may be set to a less exclusive (lower) value than the lock that is actually held at the system level, but it is never set to a more exclusive value.」源码给出的理由是：「If an xUnlock fails or appears to fail, there may be a few redundant xLock() calls or a lock may be held for longer than required, but nothing really goes wrong.」—— 多持锁、多调一次 `xLock` 只是效率损失，而记严则会导致误判热日志造成数据损坏。
- **`UNKNOWN_LOCK` 只在 `ERROR` 状态解锁失败时使用。** 源码注释规定：「Pager.eLock may only be set to UNKNOWN_LOCK when the pager is in PAGER_OPEN state.」这是为了同时解决「解锁结果不可知」和「热日志检测被自己的锁干扰」两个问题。
- **热日志回放刻意绕过 `RESERVED_LOCK`。** `sqlite3PagerSharedLock` 用 `pagerLockDb(pPager, EXCLUSIVE_LOCK)` 直接升到排他锁，注释解释了理由：若中途持 `RESERVED_LOCK`，别的进程会看到保留锁并认为「数据库可以安全读取」，从而在本进程回滚完成前读到未回滚的数据。
- **`SHARED -> EXCLUSIVE` 不触发忙处理器。** `sqlite3PagerSetBusyHandler` 的注释表格把它列为「No」。这条设计的动机写在 `sqlite3PagerSharedLock` 的注释里：因为走的是 `pagerLockDb` 而非 `pager_wait_on_lock`，别的进程在这个窗口拿不到排他锁时会自己失败，避免两个进程在热日志回放上互相空转。
- **`RESERVED -> EXCLUSIVE` 触发忙处理器。** 这是提交路径上唯一需要等待读者的升级点，也是写者被读者拖住的地方。
- **WAL 模式完全绕开文件锁。** `sqlite3PagerExclusiveLock` 在 `pagerUseWal(pPager)` 为真时是 no-op；`sqlite3PagerBegin` 的 WAL 分支调 `sqlite3WalBeginWriteTransaction(pPager->pWal)` 而不是取 `RESERVED_LOCK`。取舍是 WAL 的并发控制搬到共享内存索引里，代价是多一层共享内存协议。
- **独占模式用「永不主动降锁」换吞吐。** `sqlite3PagerSharedLock` 在独占模式下回放热日志后不降锁，`pager_unlock` 在独占模式下整个解锁分支被跳过。代价是独占期间其它进程完全无法访问该库。
- **临时文件与内存库不锁。** `src/pager.c` 的 `act_like_temp_file` 分支直接 `pPager->eLock = EXCLUSIVE_LOCK; pPager->noLock = 1;`，假装已独占。理由是这些文件对其它进程不可见。
- **`nolock=1` URI 参数可彻底关掉锁。** `pPager->noLock = sqlite3_uri_boolean(pPager->zFilename, "nolock", 0);`。这是把并发正确性完全交给使用者的显式选择。

## 与主核 / AI 的关系

分页器文件锁是 SQLite「单文件、单写者」这一产品定位在代码层面的落点：整条并发正确性链路只靠 `src/pager.c` 里这几十行锁管理加上 VFS 的文件锁原语维持。

**架构推论：** `sqlite3PagerSharedLock` 把「取读锁」与「热日志检测 + 回放」焊在同一个函数里，说明在 SQLite 的设计中，「读事务的建立」与「崩溃恢复」不是两件事 —— 拿读锁的那一刻就是判定「上一个写者是否中途崩溃」的那一刻。主核 `engine/db/` 若要实现同等强度的崩溃恢复，需要在获取读快照的入口处一并完成恢复判定，而不能假设「文件打开时是干净的」。

**架构推论：** `Pager.eLock` 这个「允许记松、禁止记严」的记账方式，把「VFS 锁操作失败时结果不可知」这一现实约束显式编码进了状态机。主核的分页器若采用行存或页存架构，锁对象的粒度会不同，但「锁状态与真实锁状态会不一致」这条约束依然存在 —— 只要底层锁原语不是事务性的，就必须有一条「保守记账」的规则。

**架构推论：** `UNKNOWN_LOCK` 注释块里描述的失效链条（「自己仍持有排他锁 → `xCheckReservedLock()` 误报 → 热日志被当成活跃事务的日志 → 跳过回滚 → 读到未回滚数据」）是**自锁干扰恢复判定**的典型。主核若采用「日志 + 数据文件分离」的架构，同样需要显式区分「我自己的锁」和「别人的锁」，否则会重现同一类误判。

**架构推论：** 忙处理器的触发规则被压缩成「升级到读锁」和「升级到写锁」两个迁移点，中间迁移不触发 —— 这是一种「只在会长时间阻塞的位置才让用户介入」的设计。主核若有更细的锁粒度，需要重新回答「哪些迁移值得让用户回调参与」这个问题，而不是简单地让每次锁请求都走回调。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pager.c | `sqlite3PagerSharedLock` | 取读锁的语义层入口：`pager_wait_on_lock(SHARED_LOCK)` → `hasHotJournal` → 必要时升 `EXCLUSIVE_LOCK` 回放热日志 → 文件版本校验 → `pagerOpenWalIfPresent` → 状态置 `PAGER_READER` |
| src/pager.c | `sqlite3PagerExclusiveLock` | 把已持有的 `RESERVED_LOCK` 升到 `EXCLUSIVE_LOCK`；WAL 模式下是 no-op；失败返回 `SQLITE_BUSY` 或 `SQLITE_IOERR_XXX` |
| src/pager.c | `sqlite3PagerLockingMode` | 切换 normal / exclusive 锁定模式；`PAGER_LOCKINGMODE_QUERY` 为纯查询；临时文件与堆内存 WAL 不受影响；返回当前模式 |
| src/pager.c | `pagerLockDb` | 加锁封装：只升不降、`noLock` 短路、`UNKNOWN_LOCK` 仅在升到 `EXCLUSIVE_LOCK` 时才更新 `eLock` |
| src/pager.c | `pagerUnlockDb` | 解锁封装：只支持降到 `SHARED_LOCK` 或 `NO_LOCK`；解锁时无条件更新 `eLock`；收尾重置 `changeCountDone` |
| src/pager.c | `pager_wait_on_lock` | 忙等待层：`do { pagerLockDb } while( rc==SQLITE_BUSY && pPager->xBusyHandler(...) )` |
| src/pager.c | `sqlite3PagerSetBusyHandler` | 装忙处理器并通过 `SQLITE_FCNTL_BUSYHANDLER` 告知 VFS；注释给出四类迁移的触发表 |
| src/pager.c | `hasHotJournal` | 热日志判定：无进程持保留锁、库文件非零字节、日志首字节非 `0x00`；空库时删残留日志并降回读锁 |
| src/pager.c | `sqlite3PagerBegin` | 写事务起点：非 WAL 分支 `pagerLockDb(RESERVED_LOCK)`，`exFlag` 为真时再 `pager_wait_on_lock(EXCLUSIVE_LOCK)`；WAL 分支走 `sqlite3WalBeginWriteTransaction` |
| src/pager.c | `syncJournal` | 提交前同步日志时调 `sqlite3PagerExclusiveLock(pPager)` 把锁升到排他级 |
| src/pager.c | `pager_end_transaction` | 事务收尾降锁：`pagerUnlockDb(pPager, SHARED_LOCK)`，状态置 `PAGER_READER` |
| src/pager.c | `pager_unlock` | 关闭连接或出错时的完全解锁：`pagerUnlockDb(pPager, NO_LOCK)`；ERROR 态解锁失败时置 `UNKNOWN_LOCK` |
| src/pager.c | `pagerUnlockAndRollback` | 回滚并解锁的组合入口；`!pPager->exclusiveMode` 时才走 `pager_end_transaction` |
| src/pager.c | `pagerExclusiveLock` | 独占模式下尝试取排他锁，失败时释放 Unix VFS 升级路径上拿到的 `PENDING_LOCK` |
| src/pager.c | `pagerOpenWal` | 打开 WAL 时若处于独占模式，先 `pagerExclusiveLock(pPager)` 再打开 |
| src/pager.c | `sqlite3PagerCloseWal` | 从 WAL 切回回滚模式前用 `pagerLockDb(pPager, SHARED_LOCK)` 探测 WAL 文件是否存在 |
| src/pager.c | `sqlite3PagerSetJournalMode` | 切 journal_mode 时先 `sqlite3PagerSharedLock` 再 `pagerLockDb(RESERVED_LOCK)`，确认无人使用日志后才删除 |
| src/pager.c | `sqlite3PagerWalSupported` | 判断 VFS 是否支持 WAL：无锁模式直接返回 0，独占模式或具备 `xShmMap` 的 iVersion>=2 才返回真 |
| src/pager.c | `sqlite3PagerOpen` | 设置 `noLock` 的来源：URI 参数 `nolock`；临时文件 / 内存库 / 不可变文件走 `act_like_temp_file`，假装已持 `EXCLUSIVE_LOCK` 且 `noLock=1` |
| src/pager.c | `UNKNOWN_LOCK` | `#define UNKNOWN_LOCK (EXCLUSIVE_LOCK+1)`；配套注释说明「锁状态宁可记松不记严」以及热日志误判的成因 |
| src/pager.h | `PAGER_LOCKINGMODE_QUERY` / `PAGER_LOCKINGMODE_NORMAL` / `PAGER_LOCKINGMODE_EXCLUSIVE` | 锁定模式的三个取值：`-1`（查询）、`0`（normal）、`1`（exclusive） |
| src/os.h | `NO_LOCK` / `SHARED_LOCK` / `RESERVED_LOCK` / `PENDING_LOCK` / `EXCLUSIVE_LOCK` | 五个锁级别的常量定义（`0` / `1` / `2` / `3` / `4`）与各自语义说明 |
| src/os_unix.c | `unixLock` | Unix VFS 的锁实现：pending byte / reserved byte / shared byte range 的 fcntl 协议；含三条断言固定迁移规则 |
| src/btree.c | `lockBtree` | 读事务的真实入口：`rc = sqlite3PagerSharedLock(pBt->pPager);`；并调 `sqlite3PagerSetBusyHandler(pBt->pPager, btreeInvokeBusyHandler, pBt)` 装忙处理器 |
| src/vdbeaux.c | 多库事务编排 | 对每个有活跃事务的数据库文件调 `rc = sqlite3PagerExclusiveLock(pPager);` |
| src/pragma.c | `PRAGMA locking_mode` 实现 | 通过 `sqlite3PagerLockingMode` 查询 / 设置模式；无库名时对所有附加库逐个设置并更新 `db->dfltLockMode` |
| src/attach.c | 附加库处理 | `sqlite3PagerLockingMode(pPager, db->dfltLockMode);` 让新附加的库继承默认锁定模式 |
| src/main.c | 主库初始化 | `sqlite3PagerLockingMode(sqlite3BtreePager(db->aDb[0].pBt), ...)` 为主库设置锁定模式 |

## 参考

- [SQLite Locking Overview（sqlite.org 官方文档）](https://sqlite.org/lockingv3.html)
- [SQLite PRAGMA locking_mode（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_locking_mode)
- [SQLite Atomic Commit In SQLite（sqlite.org 官方文档）](https://sqlite.org/atomiccommit.html)
- [SQLite Write-Ahead Logging（sqlite.org 官方文档）](https://sqlite.org/wal.html)
- [SQLite Source Code Browser：src/pager.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/pager.c)
- [sqlite/sqlite 仓库中的 src/pager.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/pager.c)
- [sqlite/sqlite 仓库中的 src/os_unix.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/os_unix.c)
