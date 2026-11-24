---
project: sqlite
feature: pcache-lookup
page_type: feature
schema_version: 1
refs:
  - path: src/pcache.c
    symbols: [sqlite3PcacheFetch, sqlite3PcacheRef, sqlite3PcacheRelease, sqlite3PcachePageRefcount]
---

# 页缓存查找与固定

## 一句话职责

`src/pcache.c` 里的 `sqlite3PcacheFetch` 按页号向页缓存要一页，`sqlite3PcacheRef` / `sqlite3PcacheRelease` 维护这页的引用计数（pin），`sqlite3PcachePageRefcount` 把单页引用数读出来给上层判断。

## 机制是什么

页缓存（page cache）是 B-tree 层与磁盘文件之间的一层缓存，每条缓存项对应数据库文件里的一个固定大小页。`src/pcache.c` 是这层的"中间件"：它自己不存页内容，而是把查找/创建动作转交给可插拔的底层缓存实现（`sqlite3GlobalConfig.pcache2` 里的 `xFetch`），默认实现是 `src/pcache1.c` 的 `pcache1Fetch`。

这一层管的只有两件事：

1. **查找（lookup）**：给定页号 `pgno`，要么在缓存里找到那一页，要么新建一页。`sqlite3PcacheFetch` 返回的是 `sqlite3_pcache_page *`，不是最终可用的 `PgHdr *`——后者要再调 `sqlite3PcacheFetchFinish` 才拿得到。
2. **固定（pin）与引用计数**：每次成功的 fetch 对应一次"引用"。引用数降到 0 的页才可以被回收。`sqlite3PcacheRef` 加一，`sqlite3PcacheRelease` 减一。

`src/pcache.c` 里的 `PCache` 结构（定义在 `src/pcache.c` 顶部）持有 `i64 nRefSum`（所有页的引用数之和）、`PgHdr *pDirty` / `pDirtyTail`（脏页双向链表）、`PgHdr *pSynced`（脏页链表上最后一个已同步页）、`u8 eCreate`（传给 `xFetch` 的创建标志）、`int szCache` / `int szSpill`（缓存页数上限、溢出阈值）。

引用计数的语义是**页级**的，不是"用户数"的：`PgHdr.nRef` 是 `i64`，每加一就同步加到 `PCache.nRefSum`。`src/pcache.h` 里对此的注释是"One release per successful fetch. Page is pinned until released. Reference counted."

## 怎么实现的

### 一、查找：`sqlite3PcacheFetch`

`sqlite3PcacheFetch(PCache *pCache, Pgno pgno, int createFlag)` 做三件事：

1. **算 `eCreate`**：`eCreate = createFlag & pCache->eCreate`。`pCache->eCreate` 由 `pcacheManageDirtyList` 维护，取值 1 或 2：
   - `eCreate == 1`：缓存是 purgeable 且当前有脏页（`pCache->bPurgeable && pCache->pDirty`）——即"脏页还在，腾不出干净页"。
   - `eCreate == 2`：其余情况（没有脏页，或者不是 purgeable 缓存）。
   - `sqlite3PcacheOpen` 初始把 `eCreate` 设为 2；`pcacheManageDirtyList` 在把最后一个脏页移出链表时把 `eCreate` 设为 2，在把脏页加到链表尾时（`pCache->bPurgeable` 为真）把 `eCreate` 设为 1。
   - `createFlag` 只有 0 和 3 两种合法取值（函数内有 `assert( createFlag==3 || createFlag==0 )`）。`createFlag==3` 与 `eCreate` 按位与之后得到 0、1 或 2；`createFlag==0` 时 `eCreate` 恒为 0。
2. **调底层**：`pRes = sqlite3GlobalConfig.pcache2.xFetch(pCache->pCache, pgno, eCreate)`。
3. **返回** `pRes`（可能是 NULL）。

`sqlite3PcacheFetch` 自身不做哈希查找，也不碰引用计数——它只是把 `createFlag` 翻译成 `eCreate` 再转发。真正的哈希查找在 `src/pcache1.c` 的 `pcache1FetchNoMutex`：

```c
pPage = pCache->apHash[iKey & (pCache->nHash-1u)];
while( pPage && pPage->iKey!=iKey ){ pPage = pPage->pNext; }
```

`apHash` 是桶头数组，`nHash` 始终是 2 的幂（由 `pcache1ResizeHash` 维持），所以取模退化成按位与。命中后若该页已被 unpin（`PAGE_IS_UNPINNED`，即 `pLruNext!=0`），则调 `pcache1PinPage` 把它从 LRU 链上摘下来重新 pin 住；若本来就 pin 着，直接返回。

### 二、创建失败后的回退：`sqlite3PcacheFetchStress`

`sqlite3PcacheFetch` 返回 NULL 时，**实际调用方是 `src/pager.c` 的 `getPageNormal`**：

```c
pBase = sqlite3PcacheFetch(pPager->pPCache, pgno, 3);
if( pBase==0 ){
  pPg = 0;
  rc = sqlite3PcacheFetchStress(pPager->pPCache, pgno, &pBase);
  ...
}
```

`sqlite3PcacheFetchStress` 的行为：

- 若 `pCache->eCreate==2`，直接 `return 0`（不做任何事）。
- 若 `sqlite3PcachePagecount(pCache) > pCache->szSpill`，尝试挤出一个脏页：从 `pCache->pSynced` 开始沿 `pDirtyPrev` 往回找，跳过 `nRef` 非 0 的页和带 `PGHDR_NEED_SYNC` 标志的页；找不到就从 `pCache->pDirtyTail` 开始往回找第一个 `nRef==0` 的脏页。找到后调 `pCache->xStress(pCache->pStress, pPg)` 让上层把这个页写回磁盘；`xStress` 返回值若既不是 `SQLITE_OK` 也不是 `SQLITE_BUSY`，该函数原样返回这个错误码。
- 最后再调一次 `xFetch(pCache->pCache, pgno, 2)`（这次 `createFlag` 硬编码为 2，即"不惜代价也要建"）；结果为 NULL 则返回 `SQLITE_NOMEM_BKPT`。

### 三、固定：引用计数的加减

`sqlite3PcacheRef(PgHdr *p)`：

```c
p->nRef++;
p->pCache->nRefSum++;
```

`sqlite3PcacheRelease(PgHdr *p)`：

```c
p->pCache->nRefSum--;
if( (--p->nRef)==0 ){
  if( p->flags&PGHDR_CLEAN ){
    pcacheUnpin(p);
  }else{
    pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT);
  }
}
```

要点：

- 引用数减到 0 且页是干净的（`PGHDR_CLEAN`）→ 调 `pcacheUnpin` 把它交给底层缓存（`sqlite3GlobalConfig.pcache2.xUnpin(..., 0)`，第二个参数 discard 为 0，页可以被保留也可以被丢）。`pcacheUnpin` 在 `p->pCache->bPurgeable` 为假时是空操作。
- 引用数减到 0 但页是脏的 → 调 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)`，把页挪到脏页链表头部（`PCACHE_DIRTYLIST_FRONT` 的值是 3，即 REMOVE(1) + ADD(2) 两个位都置上）。这样该页成为脏页链上最新的一个，在 `sqlite3PcacheFetchStress` 挑牺牲品时排在后面被选中。
- 引用数还 > 0 时，`sqlite3PcacheRelease` 只减计数，不做任何链表操作。

`sqlite3PcachePageRefcount(PgHdr *p)` 返回 `p->nRef`（`i64`）。它的实际调用方是 `src/pager.c` 的 `pagerUndoCallback`：WAL 回滚时如果某页在缓存里且 `sqlite3PcachePageRefcount(pPg)==1`（即只有自己这一个引用），就调 `sqlite3PcacheDrop(pPg)` 把页直接丢掉；否则调 `readDbPage(pPg)` 重读内容，再调 `sqlite3PagerUnrefNotNull(pPg)` 释放引用。

### 四、调用链全景

| 入口 | 调用方（真实函数名） | createFlag | 失败回退 |
|---|---|---|---|
| `sqlite3PcacheFetch` | `getPageNormal`（`src/pager.c`） | 3 | `sqlite3PcacheFetchStress` |
| `sqlite3PcacheFetch` | `sqlite3PagerLookup`（`src/pager.c`） | 0 | 无（查不到就返回 NULL，不读盘） |
| `sqlite3PcacheRef` | `sqlite3PagerRef`（`src/pager.c`） | — | — |
| `sqlite3PcacheRelease` | `sqlite3PagerUnrefNotNull`、`sqlite3PagerUnrefPageOne`（`src/pager.c`）、`pager_playback_one_page`（`src/pager.c`） | — | — |
| `sqlite3PcachePageRefcount` | `pagerUndoCallback`（`src/pager.c`）、`sqlite3PagerPageRefcount`（`src/pager.c`） | — | — |

`sqlite3PagerLookup` 与 `sqlite3PagerGet` 的区别在 `src/pager.c` 的注释里写明：`_get()` 会去磁盘读，`Lookup()` 只在缓存里找，找不到就返回 NULL，不做磁盘 I/O。

`sqlite3PagerUnrefNotNull` 与 `sqlite3PagerUnrefPageOne` 都会调 `sqlite3PcacheRelease`，区别在于后者额外调 `pagerUnlockIfUnused(pPager)`——因为 page 1 的最后一个引用消失时要把数据库文件锁放掉。

### 五、fetch 与 fetchFinish 的拆分

`sqlite3PcacheFetch` 返回 `sqlite3_pcache_page *`，`PgHdr` 的初始化在 `sqlite3PcacheFetchFinish` 里做。`src/pcache.c` 的注释解释了拆分原因："When separated they can both (usually) operate without having to push values to the stack on entry and pop them back off on exit, which saves a lot of pushing and popping."

`sqlite3PcacheFetchFinish` 的逻辑：

- 若 `pPgHdr->pPage` 为 0（首次使用该缓存项）→ 走 `pcacheFetchFinishWithInit`：`memset` 清空 `PgHdr` 中 `pDirty` 之后的字段，把 `pPage`、`pData`（= `pPage->pBuf`）、`pExtra`（= `&pPgHdr[1]`，前 8 字节清零）填好，`pCache`、`pgno` 就位，`flags` 设为 `PGHDR_CLEAN`。
- 否则（已初始化过）→ `pCache->nRefSum++; pPgHdr->nRef++;`。

注意：`nRef` 的加一发生在 `sqlite3PcacheFetchFinish`，**不是** `sqlite3PcacheFetch`。

## 关键取舍

1. **中间件层 + 可插拔底层**：`src/pcache.c` 只做参数翻译与引用计数管理，页的存放策略交给 `sqlite3GlobalConfig.pcache2` 的 `xFetch`。官方文档（`sqlite3_pcache_methods2`）明确说 "The cache must not perform any reference counting"——引用计数是 `src/pcache.c` 这一层的职责，底层缓存只认 pin/unpin。代价是多一层间接调用，收益是应用可以整体替换缓存策略。

2. **`eCreate` 三态而不是布尔**：`createFlag` 只有 0 和 3 两种合法值，但 `eCreate` 有 0/1/2 三态，语义是"不新建 / 便宜时新建 / 不惜代价新建"。用按位与 `createFlag & pCache->eCreate` 一次算出结果，省掉分支。`src/pcache.c` 的注释写明这三种含义分别是 "Do not allocate a new page" / "Allocate a new page if doing so is inexpensive" / "Allocate a new page even it doing so is difficult"。

3. **eCreate==2 时 `sqlite3PcacheFetchStress` 直接返回 0**：这意味着没有脏页的缓存里，fetch 失败就一定是内存不足，不做任何挤页尝试。

4. **`sqlite3PcacheRelease` 区分干净/脏页**：干净页引用归零后走 `pcacheUnpin` 交还给底层（可被 LRU 回收）；脏页引用归零后走 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)`，把它推到脏页链表头部而不是让它变成淘汰候选。`src/pcache.c` 头部注释说明脏页链表按 LRU 顺序维护，`pDirty` 是最新元素，`pDirtyTail` 是最旧元素。

5. **`pSynced` 是纯优化**：`src/pcache.c` 的注释直说 "pSynced is just an optimization"。它是脏页链表上最后一个 `PGHDR_NEED_SYNC` 标志清零的页，用于在挤页时优先挑一个不需要 journal sync 的页。`pcacheManageDirtyList` 里的注释也承认这个指针可能指错："This is Ok, as pSynced is just an optimization."

6. **`sqlite3PcacheRelease` 标了 `SQLITE_NOINLINE`**：把它从热路径里拆出来，让调用方不必为它做栈帧开销。

7. **脏页挤出的两级筛选**：`sqlite3PcacheFetchStress` 先找 `PGHDR_NEED_SYNC` 清零的脏页，找不到才退而求其次找任意 `nRef==0` 的脏页。`PGHDR_NEED_SYNC` 的含义见 `src/pcache.h`："Fsync the rollback journal before writing this page to the database"。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pcache.c | `sqlite3PcacheFetch` | 按页号向底层缓存要一页；把 `createFlag` 与 `PCache.eCreate` 按位与算出 `eCreate` 后调 `sqlite3GlobalConfig.pcache2.xFetch` |
| src/pcache.c | `sqlite3PcacheRef` | 页引用数加一，同时给 `PCache.nRefSum` 加一 |
| src/pcache.c | `sqlite3PcacheRelease` | 页引用数减一；归零时，干净页走 `pcacheUnpin`，脏页走 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)` |
| src/pcache.c | `sqlite3PcachePageRefcount` | 返回 `PgHdr.nRef`，即该页当前的引用数 |
| src/pcache.c | `sqlite3PcacheFetchStress` | `sqlite3PcacheFetch` 返回 NULL 后的回退：挤出一个脏页后用 `createFlag=2` 再取一次，失败返回 `SQLITE_NOMEM_BKPT` |
| src/pcache.c | `sqlite3PcacheFetchFinish` | 把 `sqlite3_pcache_page *` 转成已初始化的 `PgHdr *`，并在此处做 `nRef`/`nRefSum` 加一 |
| src/pcache.c | `pcacheFetchFinishWithInit` | 首次使用某缓存项时的初始化路径：`memset` 头部字段、设 `flags = PGHDR_CLEAN` |
| src/pcache.c | `pcacheManageDirtyList` | 维护脏页双向链表与 `PCache.eCreate`、`PCache.pSynced` |
| src/pcache.c | `pcacheUnpin` | 干净页引用归零后调用底层 `xUnpin(..., 0)`；非 purgeable 缓存时为空操作 |
| src/pcache.c | `struct PCache` | 缓存实例：`nRefSum`、`pDirty`/`pDirtyTail`/`pSynced`、`szCache`、`szSpill`、`eCreate`、`xStress` |
| src/pcache.h | `PGHDR_CLEAN` / `PGHDR_DIRTY` / `PGHDR_NEED_SYNC` | 页头标志位：是否在脏页链表上、写入前是否需要 fsync 回滚日志 |
| src/pcache.h | `sqlite3PcacheFetch` 等声明 | 本层对外接口的原型声明 |
| src/pcache1.c | `pcache1Fetch` / `pcache1FetchNoMutex` | 默认缓存实现的 `xFetch`：在 `apHash` 桶链上按 `iKey` 查找，命中则 `pcache1PinPage` 重新 pin |
| src/pcache1.c | `pcache1FetchStage2` | 默认实现中 createFlag 非 0 时的建页路径：回收 LRU 页或新分配缓冲 |
| src/pcache1.c | `pcache1Unpin` | 默认实现的 `xUnpin`：把页放回 PGroup 的 LRU 链或直接摘除 |
| src/pager.c | `getPageNormal` | `sqlite3PcacheFetch` 的主要调用方（createFlag=3），失败时回退到 `sqlite3PcacheFetchStress` |
| src/pager.c | `sqlite3PagerLookup` | 只查缓存不读盘的入口，用 createFlag=0 调 `sqlite3PcacheFetch` |
| src/pager.c | `sqlite3PagerRef` | 转发 `sqlite3PcacheRef` |
| src/pager.c | `sqlite3PagerUnrefNotNull` / `sqlite3PagerUnrefPageOne` | 转发 `sqlite3PcacheRelease`；后者额外调 `pagerUnlockIfUnused` |
| src/pager.c | `pagerUndoCallback` | WAL 回滚路径，用 `sqlite3PcachePageRefcount(pPg)==1` 判定能否直接 `sqlite3PcacheDrop` |

## 与主核 / AI 的关系

本仓主核（`engine/db`）在 `engine/db/src/storage/pager.c` 与 `engine/db/src/storage/pager_frame.c` 里自建了一套缓冲池，概念上与 SQLite 这套一一对应，但取舍不同。**架构推论：** 主核的 `pager_fetch_frame` 与 SQLite 的 `sqlite3PcacheFetch` + `sqlite3PcacheFetchFinish` 是同一职责的两种拆法——SQLite 拆成"取指针"与"初始化并加引用"两段，主核则在 `pager_fetch_frame` 一段里做完哈希查找、`pin_count` 加一与内容锁获取。

对照要点（均已读过两边函数体）：

- **引用计数的作用位置**：SQLite 用 `PgHdr.nRef` 与 `PCache.nRefSum`（`i64`），引用归零才交还底层缓存；主核用 `PageFrame.pin_count`（`atomic_int`），`pager_unpin` 里 `atomic_fetch_sub(&f->pin_count, 1)`，且注释写明把它从池级互斥量改成原子量后，"一次页访问的池级锁次数从 4 次降到 2 次"。
- **锁序不同**：SQLite 的 `sqlite3PcacheRef` / `sqlite3PcacheRelease` 不加任何锁（页缓存本身假定单线程访问，`src/pcache.c` 的 `sqlite3PcacheInitialize` 注释写明 "Neither of these functions are threadsafe"）；主核则用"池级读写锁 + 每帧内容锁"两把锁，并维护"持有内容锁 ⇒ pin_count ≥ 1"的不变式。
- **逐出策略不同**：SQLite 默认实现走 LRU 链（`pcache1Unpin` 把页放到 `pGroup->lru` 链头，`pcache1FetchStage2` 从 `pGroup->lru.pLruPrev` 取受害者）；主核走 Clock 算法，`pick_victim` 只挑 `pin_count == 0` 的帧，并配 `clock_ref` 引用位。
- **查找表不同**：SQLite 用 `apHash` 桶链（`nHash` 恒为 2 的幂，用按位与代替取模）；主核用 `pager_hash_find` 的哈希表映射 `page_id → 帧下标`。
- **释放语义不同**：SQLite 的 `sqlite3PcacheRelease` 在引用归零时按干净/脏分叉处理（脏页推到脏链表头）；主核的 `pager_unpin` 顺带处理 `dirty` 标志与 `rec_lsn`（"自上次刷盘以来，这一页第一次被改动时"的日志 LSN），多退一次返回 `E_INVAL`——"unpin 不配对：这是调用方的 bug"。

对本仓 AI 协作的直接含义：`engine/db/src/storage/pager.c` 的 `pager_unpin` 已经把"先放内容锁、再减 pin_count"的顺序写死并给了死锁推理，主核这一侧不需要再从 SQLite 借鉴 pin 语义。反过来，SQLite 这套 `createFlag` 三态与"挤脏页前先筛 `PGHDR_NEED_SYNC` 清零的页"属于回滚日志语境的优化，主核用 WAL + `rec_lsn`，没有对应物。

## 参考

- [Application Defined Page Cache（sqlite3_pcache_methods2 官方文档）](https://sqlite.org/c3ref/pcache_methods2.html)
- [PRAGMA cache_size（官方文档，说明缓存大小口径与默认值 -2000）](https://sqlite.org/pragma.html#pragma_cache_size)
- [Architecture of SQLite（官方架构说明，含 Page Cache 一节）](https://sqlite.org/arch.html)
- [sqlite/sqlite 源码：src/pcache.c](https://github.com/sqlite/sqlite/blob/master/src/pcache.c)
- [sqlite/sqlite 源码：src/pcache1.c](https://github.com/sqlite/sqlite/blob/master/src/pcache1.c)
