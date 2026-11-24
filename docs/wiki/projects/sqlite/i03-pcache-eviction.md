---
project: sqlite
feature: pcache-eviction
page_type: feature
schema_version: 1
refs:
  - path: src/pcache.c
    symbols: [sqlite3PcacheShrink, sqlite3PcacheTruncate, sqlite3PcacheIterateDirty, sqlite3PcacheDrop]
---

# 页缓存淘汰与截断

## 一句话职责

当页缓存装不下新页时，从脏页链表的尾部挑一页把它写回磁盘腾出位置（spill），并在事务提交或数据库变小时把页号大于阈值的缓存条目整体丢弃。

## 机制是什么

SQLite 的页缓存（page cache）分两层：`src/pcache.c` 里的 `PCache` 是管理层，维护一张按 LRU 顺序排列的脏页双向链表 `pDirty` / `pDirtyTail`；`src/pcache1.c` 里的 `PCache1` 是默认实现，持有真正的页内存并以哈希表按页号索引。

管理层自己并不决定"扔哪一页"，它把决策分成三件事：

1. **选页**：从脏页链表尾部往回找一页未被引用（`nRef == 0`）的脏页。
2. **腾位**：调用 `xStress` 回调让上层把这页写回磁盘；写成功后该页变干净（`PGHDR_CLEAN`），从脏链表摘除，从而可以被复用。
3. **截断**：在页号大于某个阈值时，把整批缓存条目丢掉。

页的脏/净状态由 `src/pcache.h` 里的 `PGHDR_CLEAN`（`0x001`，不在脏链表上）与 `PGHDR_DIRTY`（`0x002`，在脏链表上）两个标志位表达；`PGHDR_NEED_SYNC`（`0x008`）表示该页写回前必须先同步回滚日志。`PCache` 结构里还有一个 `pSynced` 指针，专门用来加速"找一页不需要日志同步的脏页"这一搜索。

## 怎么实现的

### 淘汰路径：sqlite3PcacheFetchStress

`src/pcache.c` 的 `sqlite3PcacheFetchStress` 是 spill 的实际触发点，调用方是 `src/pager.c` 的 `sqlite3PagerGet`（在 `sqlite3PcacheFetch` 返回空之后调用）。

它的步骤是：

- 若 `pCache->eCreate == 2`，直接返回 0，不做任何事。`eCreate` 的取值语义写在 `sqlite3PcacheFetch` 的注释里：`0` 不分配新页、`1` 便宜时才分配、`2` 不惜代价也要分配。`eCreate == 2` 意味着当前缓存里没有脏页（或不是可回收缓存），没有可 spill 的对象。
- 若 `sqlite3PcachePagecount(pCache) > pCache->szSpill`，进入 spill 分支。`szSpill` 是 spill 阈值，在 `sqlite3PcacheOpen` 里被初始化为 `1`。
- 选页：第一轮从 `pCache->pSynced` 开始沿 `pDirtyPrev`（向链表更老的方向）回溯，跳过 `nRef` 非零的页和带 `PGHDR_NEED_SYNC` 的页，找到第一个既未被引用又不需要日志同步的脏页；如果找不到，第二轮从 `pCache->pDirtyTail`（链表最老的一端）起沿 `pDirtyPrev` 回溯，只跳过 `nRef` 非零的页，接受任意未被引用的脏页。
- 找到页后调用 `pCache->xStress(pCache->pStress, pPg)`。这个回调在 `src/pager.c` 里就是 `pagerStress`：`pagerStress` 统计 `pPager->aStat[PAGER_STAT_SPILL]++`，在 WAL 模式下写一帧（`pagerWalFrames`），在回滚日志模式下必要时 `syncJournal` 再 `pager_write_pagelist`，成功后调用 `sqlite3PcacheMakeClean(pPg)` 把页变干净并从脏链表摘除。
- `xStress` 返回值不是 `SQLITE_OK` 且不是 `SQLITE_BUSY` 时，`sqlite3PcacheFetchStress` 原样返回该错误码，放弃分配新页。
- 最后无论如何都调用 `xFetch(pCache->pCache, pgno, 2)` 强行分配新页；分配失败返回 `SQLITE_NOMEM_BKPT`。

### 截断路径：sqlite3PcacheTruncate

`src/pcache.c` 的 `sqlite3PcacheTruncate(PCache *pCache, Pgno pgno)` 做三件事：

- 遍历脏链表 `pDirty`，对页号大于 `pgno` 的页调用 `sqlite3PcacheMakeClean(p)`，把它们从脏链表摘除。函数注释说明：带正数 `pgno` 调用本函数只可能紧接在 `sqlite3PcacheCleanAll()` 之后，因此这个循环里若存在脏页，必然是 `pgno == 0` 的情形。
- 若 `pgno == 0` 且 `pCache->nRefSum` 非零，取出第 1 页（`xFetch(pCache->pCache, 1, 0)`），把它的数据区 `memset` 清零为 `pCache->szPage` 字节，然后把 `pgno` 改成 `1`——即保留第 1 页对象但清空内容，而不是丢弃它。注释里解释了原因：只要 `nRefSum > 0`，第 1 页就一定在缓存中。
- 调用 `sqlite3GlobalConfig.pcache2.xTruncate(pCache->pCache, pgno+1)`，丢弃页号大于 `pgno` 的所有缓存条目。

`sqlite3PcacheClear(pCache)` 是 `sqlite3PcacheTruncate(pCache, 0)` 的包装，用于丢弃缓存全部内容。

`sqlite3PcacheTruncate` 的调用方是 `src/pager.c` 的 `pager_end_transaction`：在 `rc == SQLITE_OK` 时先按情况调用 `sqlite3PcacheCleanAll` 或 `sqlite3PcacheClearWritable`，然后调用 `sqlite3PcacheTruncate(pPager->pPCache, pPager->dbSize)`，把缓存裁剪到当前数据库实际大小。

底层实现是 `src/pcache1.c` 的 `pcache1Truncate` 与 `pcache1TruncateUnsafe`：后者扫描哈希表，把 `iKey >= iLimit` 的页从哈希链摘下并释放；如果待删除的页数很少（`pCache->iMaxKey - iLimit < pCache->nHash`），只扫描可能命中的那几个哈希桶，否则扫描整个哈希表。

### 单页丢弃：sqlite3PcacheDrop

`src/pcache.c` 的 `sqlite3PcacheDrop(PgHdr *p)` 从缓存里丢掉单独一页，调用前该页必须恰好有一个引用（`assert( p->nRef==1 )`）。若该页是脏页，先调用 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_REMOVE)` 把它从脏链表摘除，然后 `pCache->nRefSum--`，最后调用 `xUnpin(pCache->pCache, p->pPage, 1)`（第三个参数 `1` 表示"丢弃"而非"解除固定"）。

`sqlite3PcacheDrop` 的调用方有三处，都在 `src/pager.c`：

- `pagerUndoCallback`：WAL 回滚时，若某页引用计数为 1（`sqlite3PcachePageRefcount(pPg)==1`）就直接丢弃该页；否则调用 `readDbPage(pPg)` 把内容从数据库文件重新读回。
- `pagerAcquireMapPage` 的错误分支（`pager_acquire_err`）：`readDbPage(pPg)` 失败时丢弃刚取到的页。
- `pagerMovepage`：把页移到新页号时，若旧页存在且引用计数不为 1 之外的情况（`NEVER(pPgOld->nRef>1)`）直接报错返回 `SQLITE_CORRUPT_BKPT`；对临时库（`pPager->tempFile`）改用 `sqlite3PcacheMove` 把页挪走而不是丢弃，因为内存库可能还要回滚。

`sqlite3PcacheDrop` 本身也会被 `src/pcache.c` 的 `sqlite3PcacheMove` 内部调用，用来清掉目标页号上已有的旧页对象。

### 主动收缩：sqlite3PcacheShrink

`src/pcache.c` 的 `sqlite3PcacheShrink(PCache *pCache)` 只是一层转发，直接调用 `sqlite3GlobalConfig.pcache2.xShrink(pCache->pCache)`。

默认实现是 `src/pcache1.c` 的 `pcache1Shrink`：仅当 `pCache->bPurgeable` 为真时生效；它在持有组互斥量的情况下把 `pGroup->nMaxPage` 临时置为 `0`，调用 `pcache1EnforceMaxPage(pCache)`，再把 `nMaxPage` 恢复原值。`pcache1EnforceMaxPage` 的循环条件是 `pGroup->nPurgeable > pGroup->nMaxPage` 且候选页不是锚页（`isAnchor == 0`），每轮把一页 pin 住并从哈希表移除，从而把可回收页数压回上限。

调用方是 `src/pager.c` 的 `sqlite3PagerShrink(pPager)`，它再被 `src/main.c` 的 `sqlite3_db_release_memory` 调用——对每个已附加数据库的 Pager 调一次，用于 `sqlite3_db_release_memory()` 这一对外接口。

### 脏页遍历：sqlite3PcacheIterateDirty

`src/pcache.c` 的 `sqlite3PcacheIterateDirty(PCache *pCache, void (*xIter)(PgHdr *))` 沿脏链表 `pDirty` / `pDirtyNext` 遍历所有脏页，对每页调用一次 `xIter` 回调。

这个函数只在 `defined(SQLITE_CHECK_PAGES) || defined(SQLITE_DEBUG)` 的条件下编译。`src/pcache.h` 的注释写明它的接口"仅在编译时定义了 `SQLITE_CHECK_PAGES` 时才可用"。

调用方同样在 `src/pager.c`：

- `pager_end_transaction` 里，在 `SQLITE_CHECK_PAGES` 条件下调用 `sqlite3PcacheIterateDirty(pPager->pPCache, pager_set_pagehash)`，为每个脏页重算 page hash。
- `assertTruncateConstraint(pPager)` 在 `SQLITE_DEBUG` 条件下调用 `sqlite3PcacheIterateDirty(pPager->pPCache, assertTruncateConstraintCb)`，断言脏页的页号不超过 `pPager->dbSize`，除非该页在某个保存点的 `pInSavepoint` 位图里被标记。

## 关键取舍

**选页顺序：优先"不需要日志同步"的脏页。** `sqlite3PcacheFetchStress` 第一轮专门找 `PGHDR_NEED_SYNC` 已清除的脏页，因为这类页写回时不必先 `fsync` 回滚日志，能省掉一次同步；找不到时才退而求其次接受任意未被引用的脏页。这体现了 SQLite 在"缓存回收"和"持久化成本"之间的取舍：spill 是缓存行为，不应顺带制造额外的日志同步。

**用 `pSynced` 指针把 O(n) 搜索压成近似 O(1)。** `PCache.pSynced` 指向脏链表中"最近一个 `PGHDR_NEED_SYNC` 已清除"的页附近位置。`pcacheManageDirtyList` 在加入脏链表时会检查新页的 `PGHDR_NEED_SYNC`：若 `pSynced` 为空且新页不需要同步，就把 `pSynced` 指向它。`sqlite3PcacheFetchStress` 的注释明确指出这个指针可能不精确（"pSynced is just an optimization"），指向不理想的位置只会多扫几页，不会出错。

**被引用的页一律不参与淘汰。** 选页的两个循环都跳过 `nRef` 非零的页。`sqlite3PcacheRelease` 也只在引用计数降到 0 时才处理该页：若页是净的（`PGHDR_CLEAN`）就调 `pcacheUnpin` 让它可被复用；若是脏的就调 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)` 把它挪到脏链表头部（最新的位置，等价于把它标成"刚被用过"）。

**`sqlite3PcacheDrop` 要求恰好一个引用。** 函数注释写明"This function deletes that reference, so after it returns the page pointed to by p is invalid"。调用方必须先确认引用计数，不能直接丢。

**截断时保留第 1 页的内容为空而不是丢弃对象。** `sqlite3PcacheTruncate` 在 `pgno == 0` 且仍有引用时把第 1 页数据区清零并把阈值改成 `1`，注释解释为"第 1 页对象不丢弃、只清数据区"，因为 `nRefSum > 0` 意味着第 1 页必然还在被使用。

**spill 与错误传播。** `sqlite3PcacheFetchStress` 里 `xStress` 返回 `SQLITE_BUSY` 时不当作失败（继续尝试分配新页），其他非 `SQLITE_OK` 的返回码直接向上传播。而 `pagerStress` 在 `pPager->doNotSpill` 置了 `SPILLFLAG_ROLLBACK` 或 `SPILLFLAG_OFF` 时直接返回 `SQLITE_OK` 且不写盘——也就是说这条路径上"没 spill 成功"不会报错，只是这页没能变干净。

**调试接口与生产路径分离。** `sqlite3PcacheIterateDirty` 被条件编译在 `SQLITE_CHECK_PAGES` / `SQLITE_DEBUG` 之下，而 `sqlite3PcacheShrink`、`sqlite3PcacheTruncate`、`sqlite3PcacheDrop` 是无条件编译的生产接口。

**架构推论：** `PCache` 只持有一个 `pgno` 阈值（`sqlite3PcacheTruncate` 的参数），截断行为因此是"按页号前缀"的一次性裁剪，而不是按 LRU 逐页淘汰；`PCache1` 层的哈希表结构使"丢弃页号 ≥ N 的所有条目"成为可能，而 LRU 淘汰能力则落在 `pcache1EnforceMaxPage` 依据 `nPurgeable > nMaxPage` 的循环里——两层各管一种淘汰维度。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pcache.c | sqlite3PcacheShrink | 转发到 `pcache2.xShrink`，请求底层尽可能释放页内存 |
| src/pcache.c | sqlite3PcacheTruncate | 丢弃页号大于阈值的缓存条目，并清理对应脏页 |
| src/pcache.c | sqlite3PcacheIterateDirty | 遍历脏链表，对每个脏页调用回调（仅 `SQLITE_CHECK_PAGES` / `SQLITE_DEBUG` 下编译） |
| src/pcache.c | sqlite3PcacheDrop | 丢弃单独一页，要求该页恰有一个引用 |
| src/pcache.c | sqlite3PcacheFetchStress | 缓存满时选页并调用 `xStress` 做 spill，随后强行分配新页 |
| src/pcache.c | pcacheManageDirtyList | 脏链表增删改（`PCACHE_DIRTYLIST_REMOVE` / `ADD` / `FRONT`），维护 `pSynced` |
| src/pcache.c | pcacheUnpin | 净页引用归零后调用 `pcache2.xUnpin` 交还底层 |
| src/pcache.c | sqlite3PcacheMakeClean | 摘出脏链表并置 `PGHDR_CLEAN`，引用为 0 时立即 unpin |
| src/pcache.c | sqlite3PcacheClear | `sqlite3PcacheTruncate(pCache, 0)` 的包装，丢弃缓存全部内容 |
| src/pcache.c | sqlite3PcacheMove | 改页号时清掉目标页号上的旧页对象（内部调用 `sqlite3PcacheDrop`） |
| src/pcache.c | numberOfCachePages | 按 `szCache` 符号换算缓存页数，负值口径下上限为 `1000000000` 页 |
| src/pcache1.c | pcache1Shrink | `xShrink` 默认实现：临时把 `nMaxPage` 置 0 并调用 `pcache1EnforceMaxPage` |
| src/pcache1.c | pcache1EnforceMaxPage | 在 `nPurgeable > nMaxPage` 时持续把 LRU 页从哈希表移除 |
| src/pcache1.c | pcache1Truncate | `xTruncate` 默认实现，按 `iLimit` 裁剪并更新 `iMaxKey` |
| src/pcache1.c | pcache1TruncateUnsafe | 扫描哈希表摘除 `iKey >= iLimit` 的页，少量删除时只扫部分哈希桶 |
| src/pager.c | pagerStress | `xStress` 回调：统计 spill 计数、写日志帧或写回数据库文件，成功后把页置净 |
| src/pager.c | pager_end_transaction | 事务结束时调用 `sqlite3PcacheTruncate(pPager->pPCache, pPager->dbSize)` 裁剪缓存 |
| src/pager.c | pagerUndoCallback | WAL 回滚时对引用计数为 1 的页调用 `sqlite3PcacheDrop` |
| src/pager.c | sqlite3PagerShrink | 对外收缩入口，转发 `sqlite3PcacheShrink` |
| src/pager.c | assertTruncateConstraint | 调试断言，遍历脏页校验页号与 `dbSize` 的一致性 |
| src/main.c | sqlite3_db_release_memory | 对每个附加库的 Pager 调用 `sqlite3PagerShrink` 释放内存 |

## 与主核 / AI 的关系

**架构推论：** 本页描述的是 SQLite 原生页缓存的淘汰与截断，与本仓 `engine/db/` 主核的页缓存不是同一套实现；主核的 `engine/db/storage/` 与 `engine/db/wal/` 是自研的分层存储引擎，其页回收策略不来自 `src/pcache.c`。可迁移的是本篇的取舍经验，而非代码。

对 AI 辅助开发的实际意义在于：`src/pcache.c` 里的 `PCache` 结构把"页脏净状态"（`PGHDR_CLEAN` / `PGHDR_DIRTY` / `PGHDR_NEED_SYNC`）与"页生命周期"（`nRef` 引用计数）分开表达，`sqlite3PcachePageSanity` 又把这些不变量写成断言（如"WRITEABLE implies DIRTY"、净页不得在脏链表上）。这类"状态位 + 不变量断言"的写法，是让 AI 生成的缓存代码可被 `assert` 校验的关键模式：任何修改页状态的 AI 生成补丁，只要破坏这些不变量，在 `SQLITE_DEBUG` 构建下会立刻失败，而不是留下隐蔽的数据损坏。

## 参考

- SQLite 官方文档：Page Cache（[sqlite.org/pcachetrace.html](https://sqlite.org/pcachetrace.html)）—— 说明页缓存的固定/释放与 `pcachetrace` 调试手段。
- SQLite 官方文档：PRAGMA cache_size 与 cache_spill（[sqlite.org/pragma.html#pragma_cache_size](https://sqlite.org/pragma.html#pragma_cache_size)）—— 说明 `szCache` 与 `szSpill` 两个容量口径的对外语义。
- SQLite 官方文档：In-Memory Databases（[sqlite.org/inmemorydb.html](https://sqlite.org/inmemorydb.html)）—— 说明 `bPurgeable` 为假（内存库）时 `pcacheUnpin` 不生效的原因。
- SQLite 源码镜像（GitHub）：[sqlite/sqlite 仓库的 src/pcache.c](https://github.com/sqlite/sqlite/blob/master/src/pcache.c) —— 本页 refs 指向的文件。
- SQLite 源码镜像（GitHub）：[sqlite/sqlite 仓库的 src/pcache1.c](https://github.com/sqlite/sqlite/blob/master/src/pcache1.c) —— 默认页缓存实现，含 `pcache1Shrink` 与 `pcache1Truncate`。
