---
project: sqlite
feature: pager-page-cache-ref
page_type: feature
schema_version: 1
refs:
  - path: src/pager.c
    symbols: [sqlite3PagerGet, sqlite3PagerRef, sqlite3PagerUnref, sqlite3PagerPageRefcount, sqlite3PagerPagecount]
---

# 分页器的页获取与引用计数

## 一句话职责

`sqlite3PagerGet` 按页号从分页器的页缓存里取出一页（缓存命中直接返回，未命中则从磁盘读入），`sqlite3PagerRef` / `sqlite3PagerUnref` 维护这页的引用计数（引用计数归零即页可被回收），`sqlite3PagerPageRefcount` 报出单页的引用数，`sqlite3PagerPagecount` 报出当前数据库的页数。

## 机制是什么

分页器（Pager）把「页」当作唯一的物理管理单位。一次取页 = **拿到一个指向该页内存映像的句柄，并把该页的引用计数加一**；一次放页 = **引用计数减一**。引用计数归零时，这一页在缓存里就变成「可被回收」的状态。

这一层要区分三件事，源码里是三个不同的对象：

1. **`PgHdr`（`src/pcache.h` 的 `struct PgHdr`）**：页在缓存里的控制结构。它持有 `sqlite3_pcache_page *pPage`（可插拔缓存的句柄）、`void *pData`（页数据首地址）、`void *pExtra`（页后附加区）、`Pgno pgno`（页号）、`u16 flags`（状态位）、以及私有的 `i64 nRef`（引用计数）。
2. **`sqlite3_pcache_page`**：可插拔页缓存（PCache）层的对象，通过 `sqlite3GlobalConfig.pcache2.xFetch` / `xUnpin` / `xPagecount` 这组函数指针访问。
3. **`DbPage`**：分页器对外暴露的不透明类型；在 `src/pager.c` 的实现里它就是 `PgHdr *`（`DbPage` 与 `PgHdr` 在 `src/pager.c` 内通过 typedef 关联，见该文件中 `sqlite3PagerGet` 的 `DbPage **ppPage` 出参用法与 `sqlite3PcacheFetchFinish` 返回 `PgHdr *`）。

**引用计数的状态位**（`src/pcache.h`）：

| 宏 | 值 | 含义 |
|---|---|---|
| `PGHDR_CLEAN` | `0x001` | 该页不在脏页链表上 |
| `PGHDR_DIRTY` | `0x002` | 该页在脏页链表上 |
| `PGHDR_WRITEABLE` | `0x004` | 已入日志，可以修改内容 |
| `PGHDR_NEED_SYNC` | `0x008` | 写该页前需先 fsync 回滚日志 |
| `PGHDR_DONT_WRITE` | `0x010` | 不把内容写回磁盘 |
| `PGHDR_MMAP` | `0x020` | 这是 mmap 页对象 |
| `PGHDR_WAL_APPEND` | `0x040` | 已追加进 WAL 文件 |

**取页的三条实现路径**由 `Pager.xGet` 函数指针分派。`setGetterMethod`（`src/pager.c`）按以下顺序选择：

```
if( pPager->errCode ){
  pPager->xGet = getPageError;
}else if( USEFETCH(pPager) ){
  pPager->xGet = getPageMMap;
}else{
  pPager->xGet = getPageNormal;
}
```

- **`getPageError`**：分页器处于错误状态时的取页器，直接返回 `pPager->errCode` 并把 `*ppPage` 置 **0**。
- **`getPageMMap`**：启用了 mmap I/O（`SQLITE_MAX_MMAP_SIZE>0` 编译条件成立且 `USEFETCH(pPager)` 为真）时的取页器。
- **`getPageNormal`**：普通取页器，走可插拔缓存 + 必要时读盘。

**本机制的边界：** 不负责缓存的淘汰策略（那是 pcache 层的 `sqlite3PcacheFetchStress` / `pcacheManageDirtyList` 与 I 组页面负责）、不负责把脏页写回文件（G04 页面负责）、不负责回滚日志的写入（G03 页面负责）。本机制只覆盖「**按页号取页**」与「**引用计数的增减与查询**」这两件事。

## 怎么实现的

### `sqlite3PagerGet`：按页号取页

签名（`src/pager.c`）：`int sqlite3PagerGet(Pager *pPager, Pgno pgno, DbPage **ppPage, int flags)`。

它本体是一个**纯分派器**，只有一句实质代码：`return pPager->xGet(pPager, pgno, ppPage, flags);`。`flags` 的取值来自 `src/pager.h`：

```
#define PAGER_GET_NOCONTENT     0x01  /* Do not load data from disk */
#define PAGER_GET_READONLY      0x02  /* Read-only page is acceptable */
```

`src/btree.c` 的 `btreeGetPage` 断言 `flags` 只能是 **0** / `PAGER_GET_NOCONTENT` / `PAGER_GET_READONLY` 三者之一。

### `getPageNormal`：普通取页路径

这是取页的核心实现，执行顺序如下：

1. **前置断言**：`pPager->errCode==SQLITE_OK`、`pPager->eState>=PAGER_READER`、`pPager->hasHeldSharedLock==1`。
2. **页号 0 非法**：`if( pgno==0 ) return SQLITE_CORRUPT_BKPT;`。
3. **向缓存要页**：`pBase = sqlite3PcacheFetch(pPager->pPCache, pgno, 3);`。第三个参数 `createFlag` 取 **3**，在 `src/pcache.c` 的 `sqlite3PcacheFetch` 里被处理为 `eCreate = createFlag & pCache->eCreate`，其语义是：**0 = 不分配新页；1 = 分配新页但要求不费劲（仅当缓存可回收且已有脏页时）；2 = 即使费劲也分配新页**。`sqlite3PcacheFetch` 的注释明确写出这层语义：「If the createFlag is 0, then NULL is always returned if the page is not already in the cache. If createFlag is 1, then a new page is created only if that can be done without spilling dirty pages and without exceeding the cache size limit.」
4. **取不到就「加压」重试**：`pBase==0` 时调 `sqlite3PcacheFetchStress(pPager->pPCache, pgno, &pBase)`；若它返回非 `SQLITE_OK` 则跳到错误出口；若它成功但 `pBase` 仍为 **0**，则 `rc = SQLITE_NOMEM_BKPT` 并跳错误出口。`sqlite3PcacheFetchStress` 内部会：若 `sqlite3PcachePagecount(pCache)>pCache->szSpill` 则先从脏页链表里挑一个无引用（`nRef==0`）的脏页，调用 `pCache->xStress` 把它挤出去（写回），再重新 `xFetch(..., 2)`；只有 OOM 才会失败。
5. **转成 `PgHdr`**：`pPg = *ppPage = sqlite3PcacheFetchFinish(pPager->pPCache, pgno, pBase);`。`sqlite3PcacheFetchFinish`（`src/pcache.c`）在页面首次初始化时（`!pPgHdr->pPage`）走 `pcacheFetchFinishWithInit`：把 `PgHdr` 清零、`pPage`/`pData`/`pExtra` 填好、`pCache` 与 `pgno` 记录下来、`flags` 置为 `PGHDR_CLEAN`；否则走快速路径 `pCache->nRefSum++; pPgHdr->nRef++;`。
6. **判断是否命中缓存**：`noContent = (flags & PAGER_GET_NOCONTENT)!=0;`。若 `pPg->pPager` 非空（说明这是已初始化过的页）**且** `!noContent`，则缓存命中：`pPager->aStat[PAGER_STAT_HIT]++` 后直接 `return SQLITE_OK`（`PAGER_STAT_HIT` 定义为 **0**，`PAGER_STAT_MISS` 定义为 **1**，见 `src/pager.c`）。
7. **未命中则初始化页内容**，这一段有三个分支：
   - **非法页号检查**：`if( pgno==PAGER_SJ_PGNO(pPager) ){ rc = SQLITE_CORRUPT_BKPT; goto pager_acquire_err; }`。`PAGER_SJ_PGNO(x)` 定义在 `src/pager.h` 为 `((x)->lckPgno)`，是留给锁页（locking page）的保留页号，正常数据库里不会出现。
   - **超出库大小 / 只要空白内容**：`if( !isOpen(pPager->fd) || pPager->dbSize<pgno || noContent )`：
     - 先查上限：`if( pgno>pPager->mxPgno ){ rc = SQLITE_FULL; ... }`。若 `pgno<=pPager->dbSize`（页号没超但已超上限的极端情形），还会先 `sqlite3PcacheRelease(pPg)` 并把 `pPg` 置 **0**。`mxPgno` 的初值由 `src/pager.c` 设为 `SQLITE_MAX_PAGE_COUNT`，而 `src/sqliteLimit.h` 里 `# define SQLITE_MAX_PAGE_COUNT 0xfffffffe /* 4294967294 */`。
     - 若 `noContent`：在 `sqlite3BeginBenignMalloc()` / `sqlite3EndBenignMalloc()` 包裹下，若 `pgno<=pPager->dbOrigSize` 则 `sqlite3BitvecSet(pPager->pInJournal, pgno)`，并调 `addToSavepointBitvecs(pPager, pgno)` —— 源码注释解释这是为了让该页「将来即使变成可写也不需要再入日志」，属于省 I/O 的优化；注释同时说明置位失败（OOM）是良性的，只是可能多做一些日志工作。
     - 然后 `memset(pPg->pData, 0, pPager->pageSize);` —— 用全零填满整页，`IOTRACE` 打出 `ZERO` 事件。
   - **需要真读盘**（`else` 分支）：`pPager->aStat[PAGER_STAT_MISS]++;` 后 `rc = readDbPage(pPg);`，失败则跳错误出口。
8. **收尾**：`pager_set_pagehash(pPg);`（该函数在非 `SQLITE_CHECK_PAGES` 编译下被定义为空宏 `#define pager_set_pagehash(X)`），然后 `return SQLITE_OK`。
9. **错误出口 `pager_acquire_err`**：若 `pPg` 非空则 `sqlite3PcacheDrop(pPg)`（`src/pcache.c` 中它断言 `p->nRef==1`，会从脏页链表摘除并调用 `xUnpin(..., 1)` 真正丢弃该页）；随后 `pagerUnlockIfUnused(pPager)`（引用总数归零时回滚事务并解锁）；`*ppPage = 0;` 返回错误码。

### `getPageMMap`：mmap 取页路径

`getPageMMap` 只在 `#if SQLITE_MAX_MMAP_SIZE>0` 下编译。执行顺序：

1. **判定能否用只读 mmap 页**：`bMmapOk = (pgno>1 && (pPager->eState==PAGER_READER || (flags & PAGER_GET_READONLY)))`。源码注释说明：除页 1 之外的任何页，在没有写事务打开或调用方指定了 `ACQUIRE_READONLY` 时都可用只读（mmap）页；且临时库 / 内存库不适用。
2. **页号 0 非法**：`if( pgno<=1 && pgno==0 ){ return SQLITE_CORRUPT_BKPT; }`。源码注释说明这个写法的用意：把 `pgno<=1` 放在前面能让编译器复用上一条 `pgno>1` 的判断结果，从而在常见的「大页号」场景省掉一次 `pgno==0` 判断。
3. **WAL 模式下先定位帧**：`if( bMmapOk && pagerUseWal(pPager) )` 则 `rc = sqlite3WalFindFrame(pPager->pWal, pgno, &iFrame);`，失败则 `*ppPage = 0;` 直接返回。
4. **尝试 mmap 映射**：`if( bMmapOk && iFrame==0 )` 时 `sqlite3OsFetch(pPager->fd, (i64)(pgno-1) * pPager->pageSize, pPager->pageSize, &pData)` 取映射地址；成功后若 `pPager->eState>PAGER_READER || pPager->tempFile` 则先 `sqlite3PagerLookup(pPager, pgno)` 看缓存里有没有同页，有就 `sqlite3OsUnfetch(...)` 撤销映射并复用缓存页；没有则 `pagerAcquireMapPage(pPager, pgno, pData, &pPg)` 建一个带 `PGHDR_MMAP` 标志的页。
5. **回退**：`return getPageNormal(pPager, pgno, ppPage, flags);` —— mmap 路径拿不到页时，最终落回普通取页路径。

### `sqlite3PagerLookup`：只查缓存不读盘

`sqlite3PagerLookup(Pager *pPager, Pgno pgno)`（`src/pager.c`）只查缓存：`pPage = sqlite3PcacheFetch(pPager->pPCache, pgno, 0);`（`createFlag` 为 **0**，即「页不在缓存就返回 NULL，绝不新分配」），查不到直接返回 **0**；查到则 `sqlite3PcacheFetchFinish(pPager->pPCache, pgno, pPage)` 把引用计数加一并返回 `PgHdr *`。函数头注释明确写出与 `sqlite3PagerGet` 的差别：`_get()` 会去磁盘读，`Lookup()` 只在缓存里有就返回，否则返回 NULL，且因为从不碰磁盘，所以也从不需要处理锁与日志文件。

### `sqlite3PagerRef` / `sqlite3PagerUnref`：引用计数的增减

**`sqlite3PagerRef(DbPage *pPg)`**（`src/pager.c`）只有一行实质代码：`sqlite3PcacheRef(pPg);`。`sqlite3PcacheRef`（`src/pcache.c`）的内容是：

```
assert(p->nRef>0);
assert( sqlite3PcachePageSanity(p) );
p->nRef++;
p->pCache->nRefSum++;
```

即**同时**增加单页引用计数 `PgHdr.nRef` 与缓存级引用总数 `PCache.nRefSum`。

**`sqlite3PagerUnref(DbPage *pPg)`**（`src/pager.c`）是空指针安全的包装：`if( pPg ) sqlite3PagerUnrefNotNull(pPg);`。

**`sqlite3PagerUnrefNotNull(DbPage *pPg)`**（`src/pager.c`）的实际工作：

```
if( pPg->flags & PGHDR_MMAP ){
  assert( pPg->pgno!=1 );  /* Page1 is never memory mapped */
  pagerReleaseMapPage(pPg);
}else{
  sqlite3PcacheRelease(pPg);
}
assert( sqlite3PcacheRefCount(pPager->pPCache)>0 ); /* tag-20230419-2 */
```

`sqlite3PcacheRelease`（`src/pcache.c`）是引用计数的真正减法点：

```
p->pCache->nRefSum--;
if( (--p->nRef)==0 ){
  if( p->flags&PGHDR_CLEAN ){
    pcacheUnpin(p);
  }else{
    pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT);
  }
}
```

即：引用数减到 **0** 时，若该页是干净的（`PGHDR_CLEAN`）则调 `pcacheUnpin(p)` —— `src/pcache.c` 里它只在 `p->pCache->bPurgeable` 为真时调 `sqlite3GlobalConfig.pcache2.xUnpin(p->pCache->pCache, p->pPage, 0)`，也就是「解除固定、可被回收」；若该页是脏的（`PGHDR_DIRTY`），则调 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)` 把它挂到脏页链表头部，等待写回。

**页 1 是特例**：函数头注释写明 `sqlite3PagerUnref()` / `sqlite3PagerUnrefNotNull()` 只能用于「不是页 1 最后一次引用」的场景，因为 B-tree 层在整个事务期间一直持有页 1；注释指明要用 `sqlite3PagerUnrefPageOne()` 来释放页 1。`sqlite3PagerUnrefPageOne` 断言 `pPg->pgno==1` 且 `!(pPg->flags & PGHDR_MMAP)`，然后 `sqlite3PcacheRelease(pPg)` + `pagerUnlockIfUnused(pPager)` —— 后者在引用总数归零时调 `pagerUnlockAndRollback(pPager)`（`src/pager.c`）。

### `sqlite3PagerPageRefcount`：查单页引用数

`sqlite3PagerPageRefcount(DbPage *pPage)`（`src/pager.c`）的实现是 `return sqlite3PcachePageRefcount(pPage);`，而 `sqlite3PcachePageRefcount`（`src/pcache.c`）返回 `p->nRef` —— 即 `PgHdr.nRef` 本身。

注意与 `sqlite3PagerRefcount(Pager *pPager)` 区分：后者返回的是**全缓存的引用计数之和** `sqlite3PcacheRefCount(pPager->pPCache)`（即 `PCache.nRefSum`，`src/pcache.c` 的注释明确说「This is not the total number of pages referenced, but the sum of the reference count for all pages」），并且它被 `#ifdef SQLITE_DEBUG` 包住，只在调试构建里存在。

### `sqlite3PagerPagecount`：查数据库页数

`sqlite3PagerPagecount(Pager *pPager, int *pnPage)`（`src/pager.c`）只有一句实质代码：

```
assert( pPager->eState>=PAGER_READER );
assert( pPager->eState!=PAGER_WRITER_FINISHED );
*pnPage = (int)pPager->dbSize;
```

函数头注释给出两条使用约束：只能在读事务已打开时调用；返回的是数据库总页数，但「**若文件大小在 1 到小于一个页长之间，则视为 1 页的文件**」。

### 把页标记为脏：`sqlite3PagerWrite` 与 `pager_write`

标记脏页的入口是 `sqlite3PagerWrite(PgHdr *pPg)`（`src/pager.c`），它的分派结构是：

```
if( (pPg->flags & PGHDR_WRITEABLE)!=0 && pPager->dbSize>=pPg->pgno ){
  if( pPager->nSavepoint ) return subjournalPageIfRequired(pPg);
  return SQLITE_OK;
}else if( pPager->errCode ){
  return pPager->errCode;
}else if( pPager->sectorSize > (u32)pPager->pageSize ){
  assert( pPager->tempFile==0 );
  return pagerWriteLargeSector(pPg);
}else{
  return pager_write(pPg);
}
```

`pager_write`（`src/pager.c`，静态）里的标记动作是 `sqlite3PcacheMakeDirty(pPg);`。`sqlite3PcacheMakeDirty`（`src/pcache.c`）的逻辑是：若 `p->flags & (PGHDR_CLEAN|PGHDR_DONT_WRITE)`，先清 `PGHDR_DONT_WRITE`，再 `p->flags ^= (PGHDR_DIRTY|PGHDR_CLEAN);` —— 即**把 CLEAN 位换成 DIRTY 位**。`pager_write` 在标记脏之前还会：若 `pPager->eState==PAGER_WRITER_LOCKED` 则先 `pager_open_journal(pPager)` 开回滚日志（源码注释说明必须先开日志再标脏，否则一旦出错会留下「状态是 WRITER_LOCKED 但缓存里已有脏页」的错位状态）；标脏后若用回滚日志且该页尚未入日志，则 `pPager->pInJournal` 位图为 0 时按页号是否超过 `pPager->dbOrigSize` 分别走 `pagerAddPageToRollbackJournal(pPg)`（原像入日志）或置 `PGHDR_NEED_SYNC`（文件尾部新页）。

### 真实调用方

| 调用方 | 文件 | 调用方式与语义 |
|---|---|---|
| `btreeGetPage` | `src/btree.c` | `rc = sqlite3PagerGet(pBt->pPager, pgno, (DbPage**)&pDbPage, flags);` —— B-tree 取页的通用入口，`flags` 取 0 / `PAGER_GET_NOCONTENT` / `PAGER_GET_READONLY` |
| `getAndInitPage` | `src/btree.c` | `rc = sqlite3PagerGet(pBt->pPager, pgno, (DbPage**)&pDbPage, bReadOnly);` —— 取页后若 `pPage->isInit==0` 再调 `btreeInitPage(pPage)` 初始化页头 |
| `allocateBtreePage` / `freePage` 等页管理路径 | `src/btree.c` | `sqlite3PagerGet(pBt->pPager, iPtrmap, &pDbPage, 0)` 取指针映射页（`btreeInitPtrmap`），取完 `sqlite3PagerUnref(pDbPage)` |
| `sqlite3BtreeMovetoUnpacked` 的溢出页链遍历 | `src/btree.c` | `rc = sqlite3PagerGet(pBt->pPager, nextPage, &pDbPage, (eOp==0 ? PAGER_GET_READONLY : 0))`，用完 `sqlite3PagerUnref(pDbPage)` |
| `sqlite3BtreePayload`（读溢出页） | `src/btree.c` | `rc = sqlite3PagerGet(pSrcPager, ovflIn, &pPageIn, PAGER_GET_READONLY);`，读 `sqlite3PagerGetData(pPageIn)` 后 `sqlite3PagerUnref(pPageIn)` |
| `releasePageNotNull` | `src/btree.c` | `sqlite3PagerUnrefNotNull(pPage->pDbPage);` —— B-tree 释放普通页的统一出口 |
| `releasePageOne` | `src/btree.c` | `sqlite3PagerUnrefPageOne(pPage->pDbPage);` —— 页 1 的专用释放口 |
| `sqlite3BtreeOpen` / `lockBtree` 路径 | `src/btree.c` | `rc = sqlite3PagerGet(pBt->pPager, 1, &pPgHdr, 0);` 取页 1，取完 `sqlite3PagerUnref(pPgHdr)`（`sqlite3PagerUnrefPageOne` 的同类用法见 `src/btree.c` 的 `releasePageOne`） |
| `sqlite3BtreeMovetoUnpacked` 的新页分配 | `src/btree.c` | `sqlite3PagerRef(pPage->pDbPage);` —— 新分配页要额外持一份引用，由后续的 `sqlite3PagerUnref` 释放 |
| `backupStep` | `src/backup.c` | `rc = sqlite3PagerGet(pSrcPager, iSrcPg, &pSrcPg, PAGER_GET_READONLY);` —— 备份时以只读方式取源页 |
| `sqlite3BtreeIntegrityCheck` | `src/btree.c` | `if( sqlite3PagerGet(pCheck->pPager, (Pgno)iPage, &pOvflPage, 0) ){ ... }`，读 `sqlite3PagerGetData(pOvflPage)` 后 `sqlite3PagerUnref(pOvflPage)` |
| `sqlite3PagerOpen` 之后的写事务准备 | `src/pager.c` | `rc = sqlite3PagerGet(pPager, pgno, &pPg, 1);`（`sqlite3PagerOpenSavepoint` 附近）取页并让 `flags` 传 **1**（即 `PAGER_GET_NOCONTENT`，只要空白页） |
| `pager_playback_one_page` | `src/pager.c` | `rc = sqlite3PagerGet(pPager, 1, &pPgHdr, 0);` 取页 1 用于回放，随后 `sqlite3PagerUnrefNotNull(pPgHdr)` |
| `pagerUndoCallback` / `sqlite3PagerRollback` 路径 | `src/pager.c` | `rc = sqlite3PagerGet(pPager, needSyncPgno, &pPgHdr, 0);` 取页后 `sqlite3PagerUnrefNotNull(pPgHdr)` |

### 关键常量与数值出处

| 数值 / 宏 | 含义 | 出处 |
|---|---|---|
| `PAGER_GET_NOCONTENT` = `0x01` | 不从磁盘读页内容 | `src/pager.h`：`#define PAGER_GET_NOCONTENT 0x01` |
| `PAGER_GET_READONLY` = `0x02` | 接受只读（mmap）页 | `src/pager.h`：`#define PAGER_GET_READONLY 0x02` |
| `PGHDR_CLEAN` = `0x001` | 页不在脏页链表上 | `src/pcache.h`：`#define PGHDR_CLEAN 0x001` |
| `PGHDR_DIRTY` = `0x002` | 页在脏页链表上 | `src/pcache.h`：`#define PGHDR_DIRTY 0x002` |
| `PGHDR_WRITEABLE` = `0x004` | 已入日志，可修改 | `src/pcache.h`：`#define PGHDR_WRITEABLE 0x004` |
| `PGHDR_NEED_SYNC` = `0x008` | 写该页前需先 fsync 回滚日志 | `src/pcache.h`：`#define PGHDR_NEED_SYNC 0x008` |
| `PGHDR_DONT_WRITE` = `0x010` | 不把内容写回磁盘 | `src/pcache.h`：`#define PGHDR_DONT_WRITE 0x010` |
| `PGHDR_MMAP` = `0x020` | 该页是 mmap 页对象 | `src/pcache.h`：`#define PGHDR_MMAP 0x020` |
| `PAGER_STAT_HIT` = **0** | 缓存命中计数下标 | `src/pager.c`：`#define PAGER_STAT_HIT 0` |
| `PAGER_STAT_MISS` = **1** | 缓存未命中计数下标 | `src/pager.c`：`#define PAGER_STAT_MISS 1` |
| **3** | `sqlite3PcacheFetch` 的 `createFlag`（`getPageNormal` 传入），语义是「尽量分配」 | `src/pager.c` 的 `sqlite3PcacheFetch(pPager->pPCache, pgno, 3)` 与 `src/pcache.c` 的 `eCreate = createFlag & pCache->eCreate` |
| **0** | `sqlite3PcacheFetch` 的 `createFlag`（`sqlite3PagerLookup` 传入），语义是「只在缓存里找」 | `src/pager.c` 的 `sqlite3PcacheFetch(pPager->pPCache, pgno, 0)` |
| **1** / **2** | `sqlite3PcacheFetch` 里 `eCreate` 的三档语义（不分配 / 便宜地分配 / 尽力分配） | `src/pcache.c` 的 `sqlite3PcacheFetch` 注释与 `eCreate = createFlag & pCache->eCreate` |
| `SQLITE_MAX_PAGE_COUNT` = `0xfffffffe`（**4294967294**） | 库最大页数上限，写入 `pPager->mxPgno` | `src/sqliteLimit.h`：`# define SQLITE_MAX_PAGE_COUNT 0xfffffffe /* 4294967294 */`；`src/pager.c` 的 `pPager->mxPgno = SQLITE_MAX_PAGE_COUNT;` |
| `PAGER_SJ_PGNO(x)` = `x->lckPgno` | 锁页保留页号，取该页返回 `SQLITE_CORRUPT_BKPT` | `src/pager.h`：`#define PAGER_SJ_PGNO(x) ((x)->lckPgno)`；`src/pager.c` 的 `getPageNormal` 判据 |
| **0** | `getPageNormal` 中 `pgno==0` 的错误码来源 | `src/pager.c`：`if( pgno==0 ) return SQLITE_CORRUPT_BKPT;` |
| **1** | `sqlite3PagerRefcount` 与 `sqlite3PagerPageRefcount` 的区别：前者是全缓存引用数之和 `PCache.nRefSum`，后者是单页 `PgHdr.nRef` | `src/pcache.c` 的 `sqlite3PcacheRefCount` 与 `sqlite3PcachePageRefcount` |
| `SQLITE_MAX_MMAP_SIZE` | `getPageMMap` 的编译条件，>0 才编译 mmap 取页器 | `src/pager.c` 的 `#if SQLITE_MAX_MMAP_SIZE>0` |
| `1000000000` | 负数 `cache_size` 换算成页数时的上限 | `src/pcache.c` 的 `numberOfCachePages`：`if( n>1000000000 ) n=1000000000;` |

## 关键取舍

- **`sqlite3PagerGet` 做成纯分派器，把实现全推给 `Pager.xGet`。** 代价是每次取页多一次间接调用；收益是「mmap / 普通 / 错误状态」三种模式可以各自独立演进，且 `setGetterMethod` 能在分页器状态变化时整体切换。本页的边界说明取页路径时都必须点明是哪一条。
- **取页与「不读盘」由 `PAGER_GET_NOCONTENT` 显式区分，而不是隐式判断。** `getPageNormal` 里 `noContent` 为真时直接把页内容清零，并顺手把该页在 `pPager->pInJournal` 与各 savepoint 的位图里置位——源码注释说明这是「将来变成可写时也不必再入日志」的省 I/O 手段。代价是位图置位失败（OOM）被 `sqlite3BeginBenignMalloc` 包裹成良性错误，只会导致「多写一点日志」。
- **`sqlite3PagerUnref` 与 `sqlite3PagerUnrefNotNull` 的空指针差别被刻意保留。** 前者容忍 NULL，后者断言非空，源码注释把两者的使用前提写死：不能用它们释放页 1 的最后一次引用，必须走 `sqlite3PagerUnrefPageOne`（后者额外调 `pagerUnlockIfUnused`）。这个约束由 `assert( sqlite3PcacheRefCount(pPager->pPCache)>0 ); /* tag-20230419-2 */` 在调试构建里把关。
- **引用计数加一加在两个地方。** `PgHdr.nRef` 是单页计数，`PCache.nRefSum` 是全缓存计数之和；`sqlite3PcacheRef` / `sqlite3PcacheRelease` 必须同时维护两者，否则 `sqlite3PagerRefcount` 反映的整体状态就会与实际不符。
- **引用数归零不立即丢弃页。** 干净页走 `pcacheUnpin`（只是解除固定，可被回收）；脏页走 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)` 挂到脏页链表头等待写回。这把「没人用了」与「可以扔了」分成两步，正是本机制不负责写回却必须把页安全交接给写回机制的地方。
- **缓存取页失败有两条路径，且优先级明确。** 先用 `createFlag=3` 尝试「尽量分配」；失败才调 `sqlite3PcacheFetchStress` 去挤掉一个无引用的脏页再试；仍失败且返回 NULL 才是 `SQLITE_NOMEM_BKPT`。`sqlite3PcacheFetchStress` 里挑脏页的顺序也有讲究：先找 `PGHDR_NEED_SYNC` 已清且无引用的页，找不到才退而求其次找任意无引用脏页，源码注释说明这会偶发让 `pCache->pSynced` 指向不完美的位置，但「`pSynced` 只是一个优化」。
- **页 1 被排除在 mmap 之外，且释放路径独立。** `getPageMMap` 的 `bMmapOk` 要求 `pgno>1`；`sqlite3PagerUnrefNotNull` 对带 `PGHDR_MMAP` 的页断言 `pPg->pgno!=1`（注释「Page1 is never memory mapped」）；`sqlite3PagerUnrefPageOne` 也断言页 1 且不带 `PGHDR_MMAP`。
- **`sqlite3PagerPagecount` 报的是 `pPager->dbSize`，不是「文件实际字节数除以页长」。** 函数头注释特别说明「若文件大小在 1 到小于一个页长之间，则视为 1 页的文件」——这是一个针对截断/残缺文件的防御性约定，而不是简单的除法。
- **`sqlite3PagerPageRefcount` 暴露的是底层字段本身。** 它直接返回 `PgHdr.nRef`，没有做任何聚合或裁剪；调用方要区分「这一页被引用了几次」与「整个缓存一共持有了多少次引用」（后者是 `sqlite3PagerRefcount`，且仅在 `SQLITE_DEBUG` 下存在）。

## 与主核 / AI 的关系

**架构推论：** `sqlite3PagerGet` 采取「分派器 + 可替换取页器」的结构（`Pager.xGet` 由 `setGetterMethod` 在三种实现间切换），意味着 SQLite 把「页从哪来」做成了**可替换策略**：`getPageMMap` 换的是存储介质（内存映射 vs 预读），`getPageError` 换的是错误状态下的行为。主核 `engine/db/storage/pager.c` 若也需要「同一套页接口、多种取页策略（普通文件 / mmap / 压缩 / 加密）」，对应的做法就是保留一个函数指针槽位而非在调用点写 `if`。

**架构推论：** 「引用计数归零」与「页可被回收」被刻意分成两步（干净页 `pcacheUnpin`、脏页 `pcacheManageDirtyList`），本质上是把**读者生命周期**与**写回生命周期**解耦：读者放手后，脏页仍要留在缓存里直到被写回。主核的多模态存储层若把页的引用计数直接绑定到「立刻可释放」，在有后台刷写线程时会出现「读者已放手但刷写线程还没写完」的窗口。

**架构推论：** `sqlite3PagerPageRefcount`（单页）与 `sqlite3PagerRefcount`（全局引用之和）两种粒度并存，说明 SQLite 的诊断能力是被显式设计过的，而不是事后补的统计。主核若要在压测或故障定位时回答「某一页被谁持有」，需要同等粒度的可观测接口；但注意 `sqlite3PagerRefcount` 处于 `SQLITE_DEBUG` 之下，说明全局引用计数在生产构建里并不保证存在。

**架构推论：** `PAGER_GET_NOCONTENT` 携带的「先给零页、以后再补内容」语义，是 SQLite 处理「页还没写但逻辑上已经存在」这类场景的手段（空闲链表叶页、savepoint 回滚时载入的新页，源码注释列举了这两种场景）。主核若要在变长记录或多模态大对象上做延迟物化，可以照搬这种「先占位、后填充」的接口形状，但必须同时提供「何时真的去读内容」的明确触发点——SQLite 的触发点就是 `sqlite3PagerWrite`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pager.c | `sqlite3PagerGet` | 取页入口，纯分派器：`return pPager->xGet(pPager, pgno, ppPage, flags);` |
| src/pager.c | `getPageNormal` | 普通取页实现：`sqlite3PcacheFetch(...,3)` → `sqlite3PcacheFetchStress` 兜底 → `sqlite3PcacheFetchFinish` → 命中计 `PAGER_STAT_HIT` / 未命中 `PAGER_STAT_MISS` + `readDbPage` / `PAGER_GET_NOCONTENT` 时清零并置日志位图 / 超 `mxPgno` 返回 `SQLITE_FULL` / `pgno==0` 或 `PAGER_SJ_PGNO` 返回 `SQLITE_CORRUPT_BKPT` |
| src/pager.c | `getPageMMap` | mmap 取页实现：`bMmapOk` 判定、`sqlite3WalFindFrame` 定位 WAL 帧、`sqlite3OsFetch` 取映射、`pagerAcquireMapPage` 建 `PGHDR_MMAP` 页；不满足条件时回退 `getPageNormal` |
| src/pager.c | `getPageError` | 错误状态下的取页器：直接返回 `pPager->errCode` 并把 `*ppPage` 置 0 |
| src/pager.c | `setGetterMethod` | 按 `errCode` / `USEFETCH(pPager)` 在三个取页器之间设置 `Pager.xGet` |
| src/pager.c | `sqlite3PagerLookup` | 只查缓存不读盘：`sqlite3PcacheFetch(..., 0)`，查不到返回 NULL；被 `getPageMMap` 与 `btreePageLookup` 使用 |
| src/pager.c | `sqlite3PagerRef` | 引用计数加一：`sqlite3PcacheRef(pPg);` |
| src/pager.c | `sqlite3PagerUnref` | 引用计数减一的空指针安全包装：`if( pPg ) sqlite3PagerUnrefNotNull(pPg);` |
| src/pager.c | `sqlite3PagerUnrefNotNull` | 实际释放：mmap 页走 `pagerReleaseMapPage`，普通页走 `sqlite3PcacheRelease`；带 `tag-20230419-2` 断言 |
| src/pager.c | `sqlite3PagerUnrefPageOne` | 页 1 的专用释放口：`sqlite3PcacheRelease` + `pagerUnlockIfUnused` |
| src/pager.c | `sqlite3PagerPageRefcount` | 返回单页引用数：`sqlite3PcachePageRefcount(pPage)` |
| src/pager.c | `sqlite3PagerRefcount` | 返回全缓存引用数之和 `sqlite3PcacheRefCount(pPager->pPCache)`，仅 `SQLITE_DEBUG` 下存在 |
| src/pager.c | `sqlite3PagerPagecount` | 返回 `pPager->dbSize`；要求 `eState>=PAGER_READER` 且 `eState!=PAGER_WRITER_FINISHED` |
| src/pager.c | `sqlite3PagerWrite` | 把页标记为脏的对外入口：按 `PGHDR_WRITEABLE` / `errCode` / `sectorSize>pageSize` 三路分派到 `subjournalPageIfRequired` / `pagerWriteLargeSector` / `pager_write` |
| src/pager.c | `pager_write` | 标记脏页：`pager_open_journal` 先开日志 → `sqlite3PcacheMakeDirty(pPg)` → 必要时 `pagerAddPageToRollbackJournal` 或置 `PGHDR_NEED_SYNC` |
| src/pager.c | `pagerUnlockIfUnused` | 引用总数归零时 `pagerUnlockAndRollback(pPager)`；`getPageNormal` 错误出口与 `sqlite3PagerUnrefPageOne` 都会调它 |
| src/pager.h | `PAGER_GET_NOCONTENT` / `PAGER_GET_READONLY` | 取页标志，取值 `0x01` / `0x02` |
| src/pager.h | `PAGER_SJ_PGNO` | 锁页保留页号 `x->lckPgno`，取该页报 `SQLITE_CORRUPT_BKPT` |
| src/pcache.c | `sqlite3PcacheFetch` | 可插拔缓存取页；`createFlag` 与 `pCache->eCreate` 按位与得到 `eCreate` 的三档语义 |
| src/pcache.c | `sqlite3PcacheFetchStress` | 取页失败后的加压重试：必要时挤掉一个无引用脏页再 `xFetch(..., 2)`；只 OOM 才失败 |
| src/pcache.c | `sqlite3PcacheFetchFinish` | 把 `sqlite3_pcache_page` 转成 `PgHdr` 并把 `nRef` / `nRefSum` 加一 |
| src/pcache.c | `pcacheFetchFinishWithInit` | 首次初始化页：`PgHdr` 清零、填 `pPage`/`pData`/`pExtra`、置 `PGHDR_CLEAN` |
| src/pcache.c | `sqlite3PcacheRef` | `p->nRef++; p->pCache->nRefSum++;` |
| src/pcache.c | `sqlite3PcacheRelease` | `nRefSum--`；`nRef` 归零时干净页 `pcacheUnpin`、脏页 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)` |
| src/pcache.c | `pcacheUnpin` | 解除固定：仅当 `bPurgeable` 时调 `xUnpin(..., 0)` |
| src/pcache.c | `sqlite3PcacheMakeDirty` | 把 `PGHDR_CLEAN` 位换成 `PGHDR_DIRTY` 位并清 `PGHDR_DONT_WRITE` |
| src/pcache.c | `sqlite3PcacheDrop` | 彻底丢弃一页：断言 `nRef==1`，脏页先摘链，再 `xUnpin(..., 1)` |
| src/pcache.c | `sqlite3PcachePageRefcount` / `sqlite3PcacheRefCount` | 单页 `nRef` / 全缓存 `nRefSum` 的读取口 |
| src/pcache.h | `struct PgHdr` | 页控制结构：`pPage` / `pData` / `pExtra` / `pCache` / `pgno` / `flags` / `nRef` |
| src/pcache.h | `PGHDR_CLEAN` / `PGHDR_DIRTY` / `PGHDR_WRITEABLE` / `PGHDR_NEED_SYNC` / `PGHDR_DONT_WRITE` / `PGHDR_MMAP` | 页状态位，取值 `0x001` / `0x002` / `0x004` / `0x008` / `0x010` / `0x020` |
| src/sqliteLimit.h | `SQLITE_MAX_PAGE_COUNT` | 库最大页数上限 `0xfffffffe`（4294967294），写入 `pPager->mxPgno` |
| src/btree.c | `btreeGetPage` / `getAndInitPage` | B-tree 取页的两个入口，断言 `flags` 只能是 0 / `PAGER_GET_NOCONTENT` / `PAGER_GET_READONLY` |
| src/btree.c | `releasePageNotNull` / `releasePageOne` | B-tree 的释放口，分别调 `sqlite3PagerUnrefNotNull` 与 `sqlite3PagerUnrefPageOne` |
| src/backup.c | `backupStep` | 备份时以 `PAGER_GET_READONLY` 取源页 |
| src/btree.c | `sqlite3BtreePayload` | 读溢出页：`sqlite3PagerGet(..., PAGER_GET_READONLY)`，读完 `sqlite3PagerUnref` |

## 参考

- [SQLite Pager 相关文档：PRAGMA cache_size（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_cache_size)
- [SQLite B-tree 实现（sqlite.org 官方文档）](https://sqlite.org/btreetree.html)
- [SQLite 源码浏览：src/pager.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/pager.c)
- [sqlite/sqlite 仓库中的 src/pager.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/pager.c)
- [sqlite/sqlite 仓库中的 src/pcache.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/pcache.c)
