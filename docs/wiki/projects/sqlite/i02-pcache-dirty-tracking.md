---
project: sqlite
feature: pcache-dirty-tracking
page_type: feature
schema_version: 1
refs:
  - path: src/pcache.c
    symbols: [sqlite3PcacheMakeDirty, sqlite3PcacheMakeClean, sqlite3PCacheIsDirty, sqlite3PCachePercentDirty]
---

# 脏页跟踪

## 一句话职责

在页缓存里给单个页打上「已修改」的脏标记并把它挂进脏页链表、写回成功后把标记摘掉，同时对外提供脏页占缓存容量的百分比，用于决定 TEMP 库是否需要在提交时落盘。

## 机制是什么

页缓存中的每个页都有一份「脏/净」状态。`src/pcache.c` 顶部的注释把语义写死了：*A page cache entry is "clean" if it exactly matches what is currently on disk. A page is "dirty" if it has been modified and needs to be persisted to disk.*（净＝与磁盘当前内容完全一致；脏＝已被修改、需要落盘。）

这个机制由三块组成：

1. **页级脏标记**：在 `PgHdr.flags` 上用 `PGHDR_DIRTY` / `PGHDR_CLEAN` 这对互斥位表示（定义在 `src/pcache.h`，取值分别为 `0x002` 和 `0x001`）。
2. **脏页链表**：`PCache.pDirty`（最新/最脏的一端）与 `PCache.pDirtyTail`（最旧的一端）串起一条双向链表，链表节点用 `PgHdr.pDirtyNext` / `PgHdr.pDirtyPrev`。`src/pcache.c` 的结构注释明确说明这条链表**按 LRU 顺序维护**：`p was added to the list more recently than p->pDirtyNext`（p 的加入时间比 `p->pDirtyNext` 更晚）。
3. **脏页占比统计**：`sqlite3PCachePercentDirty` 把脏页数换算成缓存容量的百分比。

边界上，本机制只回答「这一页现在算不算脏」和「缓存里脏页占比多少」，不负责把脏页真的写回文件（那是 G04 的 `pagerStress` → `pager_write_pagelist` 路径），也不负责 WAL 模式下脏页如何变成 WAL 帧（H 组）；本机制提供的所有原语在两种日志模式下共用，区别只在上层写回路径的选择。

**架构推论：** 因为脏状态记录在 `PgHdr.flags` 上、而不是记录在页内容里，页缓存这一层不需要知道日志模式就能判断「这页需不需要落盘」；WAL 与回滚日志的差异被推迟到 `pagerStress` 里才分流。

## 怎么实现的

### 置脏：`sqlite3PcacheMakeDirty`

```c
void sqlite3PcacheMakeDirty(PgHdr *p){
  assert( p->nRef>0 );
  assert( sqlite3PcachePageSanity(p) );
  if( p->flags & (PGHDR_CLEAN|PGHDR_DONT_WRITE) ){    /*OPTIMIZATION-IF-FALSE*/
    p->flags &= ~PGHDR_DONT_WRITE;
    if( p->flags & PGHDR_CLEAN ){
      p->flags ^= (PGHDR_DIRTY|PGHDR_CLEAN);
      pcacheTrace(("%p.DIRTY %d\n",p->pCache,p->pgno));
      assert( (p->flags & (PGHDR_DIRTY|PGHDR_CLEAN))==PGHDR_DIRTY );
      pcacheManageDirtyList(p, PCACHE_DIRTYLIST_ADD);
      assert( sqlite3PcachePageSanity(p) );
    }
    assert( sqlite3PcachePageSanity(p) );
  }
}
```

函数体的分支只有两层：

- 外层判断 `if( p->flags & (PGHDR_CLEAN|PGHDR_DONT_WRITE) )` 是唯一入口条件。若页既不净、又没有 `PGHDR_DONT_WRITE` 标志，函数直接返回——注释里给这个分支标了 `/*OPTIMIZATION-IF-FALSE*/`，意思是「正常路径上这个条件应当为假」，即绝大多数调用是重复置脏一个已经脏的页，此时不需要任何操作。
- 进入外层后先无条件 `p->flags &= ~PGHDR_DONT_WRITE;`——**置脏会清掉 `PGHDR_DONT_WRITE`**。`PGHDR_DONT_WRITE` 由 `sqlite3PagerDontWrite` 置位（`src/pager.c`），用于大 DELETE 时跳过不再需要的页的落盘；该函数自己也有前置条件 `if( !pPager->tempFile && (pPg->flags&PGHDR_DIRTY) && pPager->nSavepoint==0 )`，即只有非临时文件、已经脏、且不在保存点内才允许置这个标志。
- 内层 `if( p->flags & PGHDR_CLEAN )` 命中才真正翻转：`p->flags ^= (PGHDR_DIRTY|PGHDR_CLEAN);` 用异或同时清掉 `PGHDR_CLEAN`、置上 `PGHDR_DIRTY`（这两个位恒定互斥且异或前 `PGHDR_CLEAN` 必为 1，所以一次异或就完成互换）。随后调 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_ADD)` 把页挂进脏页链表。

前置断言有两条：`assert( p->nRef>0 )` 要求页必须已被引用（钉住），`assert( sqlite3PcachePageSanity(p) )` 校验页状态自洽。

**调用方**：`sqlite3PcacheMakeDirty` 的实际调用方有三处，都在 `src/pager.c`：

- `pager_write`：写页前先开回滚日志，再置脏。源码注释写明了这个顺序的理由——*This is done before calling sqlite3PcacheMakeDirty() on the page. Otherwise, if it were done after calling sqlite3PcacheMakeDirty(), then an error might occur and the pager would end up in WRITER_LOCKED state with pages marked as dirty in the cache.*（开日志必须先于置脏，否则开日志失败会留下「状态是 WRITER_LOCKED 但缓存里已有脏页」的组合。）置脏之后，如果 `pPager->pInJournal!=0` 且该页不在 `pInJournal` 位图里，还要走 `pagerAddPageToRollbackJournal` 把页的原像补进回滚日志。
- `sqlite3PagerMovepage`：页号搬迁时先 `sqlite3PcacheMove(pPg, pgno)` 改键，再 `sqlite3PcacheMakeDirty(pPg)`。
- `pager_playback_one_page`：回放子日志（savepoint 回滚）时，对一个刚被 `sqlite3PagerGet` 取进内存的页调 `sqlite3PcacheMakeDirty(pPg)`，让这份「从子日志读回的旧内容」本身成为脏内容，以便后续被写回。

### 置净：`sqlite3PcacheMakeClean`

```c
void sqlite3PcacheMakeClean(PgHdr *p){
  assert( sqlite3PcachePageSanity(p) );
  assert( (p->flags & PGHDR_DIRTY)!=0 );
  assert( (p->flags & PGHDR_CLEAN)==0 );
  pcacheManageDirtyList(p, PCACHE_DIRTYLIST_REMOVE);
  p->flags &= ~(PGHDR_DIRTY|PGHDR_NEED_SYNC|PGHDR_WRITEABLE);
  p->flags |= PGHDR_CLEAN;
  pcacheTrace(("%p.CLEAN %d\n",p->pCache,p->pgno));
  assert( sqlite3PcachePageSanity(p) );
  if( p->nRef==0 ){
    pcacheUnpin(p);
  }
}
```

与置脏对称，但有三点差异：

1. **前置断言更强**：要求进入时页确实是脏的（`PGHDR_DIRTY` 置位且 `PGHDR_CLEAN` 未置位）。也就是说这个函数**不做幂等保护**，重复调用会直接触发断言失败。
2. **一次清三个标志**：`p->flags &= ~(PGHDR_DIRTY|PGHDR_NEED_SYNC|PGHDR_WRITEABLE);`。除了清脏位，还连带清掉 `PGHDR_NEED_SYNC`（要求写库前先 fsync 回滚日志）和 `PGHDR_WRITEABLE`（已入日志、可修改），然后置上 `PGHDR_CLEAN`。语义上这表示「这一页的内容已经和磁盘一致，无需再写、也不需要日志同步」。
3. **引用计数为零时触发 unpin**：函数末尾 `if( p->nRef==0 ){ pcacheUnpin(p); }`。`pcacheUnpin` 内部先判 `if( p->pCache->bPurgeable )`，只有缓存有后端存储（磁盘文件）时才真的调 `sqlite3GlobalConfig.pcache2.xUnpin(...)`；内存库（`bPurgeable` 为假）在这里是空操作。

**调用方**：

- `pagerStress`：这是**真正的落盘后置动作**。`src/pager.c` 里 `pagerStress` 在写完页内容（回滚日志模式下是 `pager_write_pagelist`，WAL 模式下是 `pagerWalFrames`）之后，于 `if( rc==SQLITE_OK )` 分支里调 `sqlite3PcacheMakeClean(pPg)`。也就是说「写成功才置净」，写失败保持脏状态，页会留待重试。
- `sqlite3PcacheTruncate`：截断缓存时，把页号大于目标页号的脏页逐个 `sqlite3PcacheMakeClean(pPg)`，让它们不再需要写回。
- `sqlite3PcacheCleanAll`：遍历 `pCache->pDirty` 链表，对每个页调 `sqlite3PcacheMakeClean(p)`，把整个缓存清干净。这是 `sqlite3PagerRollback` 之后统一清脏的路径（`src/pager.c` 中 `pager_playback_one_page` 的注释提到 *the cache is normally cleaned by sqlite3PcacheCleanAll() after rollback*）。

### 脏页链表维护：`pcacheManageDirtyList`

这是脏页跟踪的真正数据结构操作，操作码定义在 `src/pcache.c`：

```c
#define PCACHE_DIRTYLIST_REMOVE   1    /* Remove pPage from dirty list */
#define PCACHE_DIRTYLIST_ADD      2    /* Add pPage to the dirty list */
#define PCACHE_DIRTYLIST_FRONT    3    /* Move pPage to the front of the dirty list */
```

`pcacheManageDirtyList` 用位与方式组合操作：`0x01` 位表示先移除，`0x02` 位表示再加入，两者同时做就等于把页移到链表最前端（`PCACHE_DIRTYLIST_FRONT` 就是 `REMOVE|ADD`）。

- **移除分支**：先把 `p->pSynced` 修正——若 `p->pSynced==pPage`，则 `p->pSynced = pPage->pDirtyPrev`（指向更旧的一页）。然后摘链：若有 `pDirtyNext` 就让后继的 `pDirtyPrev` 指向前驱，否则（该页是尾节点）`p->pDirtyTail = pPage->pDirtyPrev`；若有 `pDirtyPrev` 就让前驱的 `pDirtyNext` 指向后继，否则（该页是头节点）`p->pDirty = pPage->pDirtyNext`。摘掉头节点后若链表变空（`if( p->pDirty==0 )`），把 `p->eCreate` 置为 `2`。源码注释解释了这个赋值的用途：*This is an optimization that allows sqlite3PcacheFetch() to skip searching for a dirty page to eject from the cache when it might otherwise have to.*（`eCreate` 是传给底层 `xFetch` 的建页标志，`2` 表示「尽最大努力分配新页」。）
- **加入分支**：把新页插到链表头——`pPage->pDirtyPrev = 0; pPage->pDirtyNext = p->pDirty;`，并让原头节点的 `pDirtyPrev` 指向它；若原链表为空则 `p->pDirtyTail = pPage`；最后 `p->pDirty = pPage`。此时若 `p->bPurgeable` 为真且 `p->eCreate` 原本是 `2`，把它改回 `1`（`1` 的语义是「方便就分配新页」）。
- **加入时的 `pSynced` 维护**：`if( !p->pSynced && 0==(pPage->flags&PGHDR_NEED_SYNC) )` 则 `p->pSynced = pPage`。源码注释说明这是优化：*if pSynced points to a page with the NEED_SYNC flag set sqlite3PcacheFetchStress() searches through all newer entries of the dirty-list for a page with NEED_SYNC clear anyway.*（`pSynced` 用来在事务中途挑一个「不需要日志同步」的脏页去淘汰。）

此外 `sqlite3PcacheMove` 在页号变更且该页同时带 `PGHDR_DIRTY` 与 `PGHDR_NEED_SYNC` 时，调 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)` 把该页提到链表最前。

### 脏页占比：`sqlite3PCachePercentDirty`

```c
int sqlite3PCachePercentDirty(PCache *pCache){
  PgHdr *pDirty;
  int nDirty = 0;
  int nCache = numberOfCachePages(pCache);
  for(pDirty=pCache->pDirty; pDirty; pDirty=pDirty->pDirtyNext) nDirty++;
  return nCache ? (int)(((i64)nDirty * 100) / nCache) : 0;
}
```

- 脏页数靠**遍历整条脏页链表**统计，不是靠计数器维护——链表本身就是唯一事实来源。
- 分母 `nCache` 来自 `numberOfCachePages(pCache)`：若 `p->szCache>=0` 直接返回 `p->szCache`（页数口径）；否则按 `n = ((-1024*(i64)p->szCache)/(p->szPage+p->szExtra))` 把负数口径（千字节）换算成页数，并钳到上限 `if( n>1000000000 ) n = 1000000000;`。`sqlite3PcacheOpen` 里 `p->szCache` 的初值是 `100`。
- 百分比是**整数除法并向下取整**：`((i64)nDirty * 100) / nCache`。`nCache` 为 0 时返回 `0`，避免除零。

**调用方**：`src/pager.c` 的 `pagerFlushOnCommit` 里有一处 `return (sqlite3PCachePercentDirty(pPager->pPCache)>=25);`。该函数的完整前置条件是：`if( pPager->tempFile==0 ) return 1;`（非 TEMP 库一律要落盘）、`if( !bCommit ) return 0;`（只提交时）、`if( !isOpen(pPager->fd) ) return 0;`、`if( pPager->eState<PAGER_WRITER_CACHEMOD ) return 0;`，最后才落到那个 `>=25` 的判断上。所以这个 `25` 是**唯一使用脏页占比的地方**，且只对 TEMP 库生效。

### 有无脏页：`sqlite3PCacheIsDirty`

```c
int sqlite3PCacheIsDirty(PCache *pCache){
  return (pCache->pDirty!=0);
}
```

只有一行——脏链表头非空即表示缓存里有脏页。注意这个函数被 `#ifdef SQLITE_DIRECT_OVERFLOW_READ` 包住，**只在启用该编译选项时才会被编入**。

**调用方**：`src/pager.c` 的 `sqlite3PagerDirectReadOk` 里有 `if( sqlite3PCacheIsDirty(pPager->pPCache) ) return 0;`。该函数的注释列了直读需要同时满足四个条件，其中第 (3) 条正是「缓存中没有脏页」；它还要求 `pPager->fd->pMethods` 非空（条件 1）、VFS 具备 `SQLITE_IOCAP_SUBPAGE_READ` 特性（条件 2）、且目标页不在 WAL 中（条件 4，通过 `sqlite3WalFindFrame` 查 WAL 帧）。

## 关键取舍

1. **用链表而不是计数器**：`sqlite3PCachePercentDirty` 每次调用都要 O(n) 遍历脏链表换算百分比，没有维护独立的脏页计数。取舍在于脏链表本身是必需的（要按 LRU 顺序挑页淘汰），再维护一个计数器要多一处状态同步点，而脏页占比的查询频率极低——全仓只有 `pagerFlushOnCommit` 一处调用。

2. **`PGHDR_DIRTY` 与 `PGHDR_CLEAN` 双位互斥**：源码用 `p->flags ^= (PGHDR_DIRTY|PGHDR_CLEAN)` 一次异或完成切换，比「先清后置」两步少一个中间态。代价是页对象里同时存在两个语义相反的位，注释里明确写了 `assert( (p->flags & (PGHDR_DIRTY|PGHDR_CLEAN))==PGHDR_DIRTY );` 来保证不会出现两位同置或同清。

3. **置脏必须先于写日志，置净必须在写成功之后**：`pager_write` 里开回滚日志严格排在 `sqlite3PcacheMakeDirty` 之前，源码注释给出了理由（避免开日志失败时留下脏页）；`pagerStress` 里 `sqlite3PcacheMakeClean` 被 `if( rc==SQLITE_OK )` 包住，写失败就不置净。脏页状态因此始终精确反映「内容与磁盘不一致且仍需写回」。

4. **置净时连带清 `PGHDR_NEED_SYNC` 与 `PGHDR_WRITEABLE`**：因为页一旦和磁盘一致，就既不需要「写前同步日志」，也不再是「已入日志可修改」状态。这两个标志的清理被内建进置净动作，调用方不需要单独处理。

5. **置脏会撤销 `PGHDR_DONT_WRITE`**：这个标志是 `sqlite3PagerDontWrite` 为大 DELETE 场景打的「此页不必写回」标记，一旦该页再次被修改就必须重新落盘，所以置脏路径无条件清掉它。

6. **`sqlite3PCachePercentDirty` 的整数截断**：结果用 `((i64)nDirty * 100) / nCache` 计算，向上取整丢失。因此「脏页占比 ≥ 25%」这个判据实际是「至少 25%」，不存在因截断而少触发的情况；反过来，占比在 24.99% 附近时会落到 24 而不触发。

7. **`sqlite3PCacheIsDirty` 的编译期可选**：它被 `SQLITE_DIRECT_OVERFLOW_READ` 条件编译包住，未启用该选项时整个函数不存在。这让 `sqlite3PagerDirectReadOk` 这条直读路径整体成为编译期可裁剪的优化。

8. **脏链表同时承担 LRU 与同步提示两个职责**：`PCache.pDirty` / `pDirtyTail` 按 LRU 顺序维护，用于事务中途挑页淘汰；`PCache.pSynced` 指向「较旧且 `PGHDR_NEED_SYNC` 已清」的位置，用于快速找到不需要日志同步的淘汰候选。两者共用一条链表，所以 `pcacheManageDirtyList` 的移除分支必须同步修正 `pSynced`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pcache.c | `sqlite3PcacheMakeDirty` | 把页置脏：清 `PGHDR_DONT_WRITE`、翻转 `PGHDR_DIRTY`/`PGHDR_CLEAN`、入脏页链表；外层条件是 `p->flags & (PGHDR_CLEAN\|PGHDR_DONT_WRITE)` |
| src/pcache.c | `sqlite3PcacheMakeClean` | 把页置净：出脏页链表、清 `PGHDR_DIRTY`/`PGHDR_NEED_SYNC`/`PGHDR_WRITEABLE`、置 `PGHDR_CLEAN`；`nRef==0` 时调 `pcacheUnpin` |
| src/pcache.c | `pcacheManageDirtyList` | 脏页双向链表（LRU 序）的增删移操作，操作码 `PCACHE_DIRTYLIST_REMOVE`/`ADD`/`FRONT`；同步维护 `PCache.pSynced` 与 `PCache.eCreate` |
| src/pcache.c | `sqlite3PCacheIsDirty` | 返回 `pCache->pDirty!=0`，即缓存中是否存在脏页；受 `SQLITE_DIRECT_OVERFLOW_READ` 条件编译保护 |
| src/pcache.c | `sqlite3PCachePercentDirty` | 遍历脏链表得脏页数，除以 `numberOfCachePages` 换算百分比，整数除法向下取整 |
| src/pcache.c | `numberOfCachePages` | 缓存容量口径换算：`szCache>=0` 直接取页数，否则按 `(-1024*szCache)/(szPage+szExtra)` 换算并钳到 `1000000000` |
| src/pcache.c | `sqlite3PcacheCleanAll` | 遍历 `pCache->pDirty`，对每个脏页调 `sqlite3PcacheMakeClean`，整体清脏 |
| src/pcache.c | `sqlite3PcacheTruncate` | 截断时把页号大于目标值的脏页逐个 `sqlite3PcacheMakeClean` |
| src/pcache.c | `sqlite3PcacheMove` | 页号搬迁；若页同时带 `PGHDR_DIRTY` 与 `PGHDR_NEED_SYNC`，调 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_FRONT)` 提到链表最前 |
| src/pcache.c | `sqlite3PcacheDrop` | 丢弃页时若带 `PGHDR_DIRTY`，先 `pcacheManageDirtyList(p, PCACHE_DIRTYLIST_REMOVE)` |
| src/pcache.c | `pcacheUnpin` | 真正向底层缓存模块发 `xUnpin` 的包装；`bPurgeable` 为假（内存库）时是空操作 |
| src/pcache.c | `sqlite3PcacheOpen` | 初始化 `PCache`：`p->szCache = 100`、`p->szSpill = 1`、`p->eCreate = 2` |
| src/pcache.h | `PGHDR_CLEAN` / `PGHDR_DIRTY` / `PGHDR_WRITEABLE` / `PGHDR_NEED_SYNC` / `PGHDR_DONT_WRITE` | `PgHdr.flags` 的位定义，取值分别为 `0x001` / `0x002` / `0x004` / `0x008` / `0x010` |
| src/pager.c | `pager_write` | 写页入口：先 `pager_open_journal` 开回滚日志，再 `sqlite3PcacheMakeDirty`，随后按 `pInJournal` 位图补 `pagerAddPageToRollbackJournal` |
| src/pager.c | `pagerStress` | 落盘后置：`rc==SQLITE_OK` 时调 `sqlite3PcacheMakeClean(pPg)` 把写成功的页置净 |
| src/pager.c | `pagerFlushOnCommit` | 唯一的脏页占比调用点：`sqlite3PCachePercentDirty(pPager->pPCache)>=25`，且仅对 TEMP 库生效 |
| src/pager.c | `sqlite3PagerDirectReadOk` | 直读条件 (3)：`if( sqlite3PCacheIsDirty(pPager->pPCache) ) return 0;` |
| src/pager.c | `sqlite3PagerDontWrite` | 置 `PGHDR_DONT_WRITE` 标志，前置条件为非 TEMP 文件、页已脏、`nSavepoint==0` |
| src/pager.c | `pager_playback_one_page` | 回放子日志时对内存页调 `sqlite3PcacheMakeDirty` |

## 与主核 / AI 的关系

本机制在主核里的位置是「页缓存层与分页器层之间的一条窄接口」：页缓存（`src/pcache.c`）只维护内存里的脏状态和脏链表，分页器（`src/pager.c`）负责决定这些脏页何时、以何种日志模式落盘。两侧的契约就是本页描述的四个函数加上一组 `PGHDR_*` 标志位——脏页状态本身从不跨层复制，跨层传递的只有「这个页现在脏不脏」这一个比特。

**架构推论：** 脏页占比这一路统计之所以只服务 TEMP 库，是因为非 TEMP 库在提交路径上有回滚日志保证持久性，不需要靠脏页比例来决定是否提前落盘；TEMP 库没有 journal 文件，脏页只存在于内存，崩溃即丢失，所以要按占比判断是否值得先落盘。

对 AI 辅助开发而言，这一层是典型的「需要读源码而非读文档才能确认行为」的位置：`sqlite3PcacheMakeDirty` 的外层条件 `p->flags & (PGHDR_CLEAN|PGHDR_DONT_WRITE)` 带着 `/*OPTIMIZATION-IF-FALSE*/` 标记、`sqlite3PCacheIsDirty` 藏在 `#ifdef SQLITE_DIRECT_OVERFLOW_READ` 里、`sqlite3PCachePercentDirty` 的 `25` 阈值只写在 `pager.c` 的调用点上而不在 `pcache.c` 里——这些细节靠符号名猜是猜不出来的。同理，`sqlite3PcacheMakeClean` 的三个前置断言说明它**不是幂等函数**，调用方必须保证「调用时该页确实是脏的」，这一点也无法从函数名反推。

## 参考

- [sqlite3_pcache_methods2：应用自定义页缓存接口](https://sqlite.org/c3ref/pcache_methods2.html) —— 页缓存可插拔接口的官方说明，`sqlite3PCacheIsDirty` 所依赖的 `PCache` 结构就建立在这套接口之上。
- [PRAGMA cache_size](https://sqlite.org/pragma.html#pragma_cache_size) —— `numberOfCachePages` 的口径来源：正数为页数、负数为千字节，官方文档说明默认建议值是 `-2000`。
- [SQLite 源码：src/pcache.c](https://github.com/sqlite/sqlite/blob/master/src/pcache.c) —— 脏页跟踪四个符号及 `pcacheManageDirtyList`、`numberOfCachePages` 的实现。
- [SQLite 源码：src/pcache.h](https://github.com/sqlite/sqlite/blob/master/src/pcache.h) —— `PgHdr` 结构与 `PGHDR_*` 标志位定义。
- [WAL 模式](https://sqlite.org/wal.html) —— 脏页在 WAL 模式下改写为 WAL 帧的对照说明。
