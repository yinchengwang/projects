---
project: sqlite
feature: pcache-size-policy
page_type: feature
schema_version: 1
refs:
  - path: src/pcache.c
    symbols: [sqlite3PcacheSetCachesize, sqlite3PcacheGetCachesize, sqlite3PcacheSetSpillsize, sqlite3PcacheSize]
---

# 页缓存容量策略

## 一句话职责

把"用户想要多大的缓存"这一个外部设定，换算成页缓存内部实际使用的两个页数阈值——缓存上限 `szCache` 与 spill 阈值 `szSpill`——并把换算结果推给底层可插拔缓存模块。

## 机制是什么

SQLite 的页缓存容量有两层口径，用户能直接改的只有一层：

1. **对外口径（用户可写）**：`PRAGMA cache_size` 接受一个整数。正数表示"页数"，负数表示"字节数（以 KiB 为单位）"。
2. **内部口径（页数）**：`src/pcache.c` 的 `PCache` 结构里存两个 `int` 字段——`szCache`（配置的缓存大小）与 `szSpill`（发生 spill 的页数阈值）。它们都以页为单位，负值只在 `szCache` 上短暂存在，表示"字节口径"，换算发生在 `numberOfCachePages` 里。

`src/pcache.c` 的 `struct PCache` 定义了这两个字段：

```c
int szCache;                        /* Configured cache size */
int szSpill;                        /* Size before spilling occurs */
```

换算规则集中在静态函数 `numberOfCachePages`：

- `szCache >= 0`：直接返回 `szCache`，即按页数口径。
- `szCache < 0`：按字节口径换算，公式是 `n = ((-1024*(i64)p->szCache)/(p->szPage+p->szExtra))`，即 `|szCache| × 1024` 字节除以"每页总占用"（页大小 `szPage` 加每页附加空间 `szExtra`）。换算结果被硬性截断在 `1000000000` 页：源码里是 `if( n>1000000000 ) n = 1000000000;`。

注意除数是 `szPage + szExtra`，不是 `szPage`。`szExtra` 是每页额外分配的空间（`sqlite3PcacheOpen` 的注释说明这部分最终就是 pager 里的 `MemPage` 结构），把它算进每页成本，负值口径才真正对应"缓存实际占用的字节数"。

另有一条独立的换算路径在 `sqlite3PcacheSetSpillsize` 里：spill 阈值也支持负值字节口径，公式相同——`mxPage = (int)((-1024*(i64)mxPage)/(p->szPage+p->szExtra))`。

## 怎么实现的

### 设置缓存上限：sqlite3PcacheSetCachesize

`src/pcache.c` 的 `sqlite3PcacheSetCachesize(PCache *pCache, int mxPage)` 做两件事：

```c
void sqlite3PcacheSetCachesize(PCache *pCache, int mxPage){
  assert( pCache->pCache!=0 );
  pCache->szCache = mxPage;
  sqlite3GlobalConfig.pcache2.xCachesize(pCache->pCache,
                                         numberOfCachePages(pCache));
}
```

- 把原始参数（含符号）原样存进 `szCache`。负值口径就保留在这里，直到有人调用 `numberOfCachePages` 才被换算。
- 把换算后的页数通过可插拔缓存接口 `sqlite3GlobalConfig.pcache2.xCachesize` 交给底层。**注意这里传给底层的是页数而不是字节数**——字节口径在管理层就被折算掉了。

默认实现是 `src/pcache1.c` 的 `pcache1Cachesize`：仅当 `pCache->bPurgeable` 为真时生效；它把上限记入 `pCache->nMax`，并同步维护 `pCache->n90pct = pCache->nMax*9/10`（用于内部回收判断），同时更新共享组的 `pGroup->nMaxPage`；若新的 `nMax` 超过 `0x7fff0000 - pGroup->nMaxPage + pCache->nMax`，则按该值截断。

`PCache` 的初始值由 `sqlite3PcacheOpen` 设定：`p->szCache = 100;` 与 `p->szSpill = 1;`。

### 读取缓存上限：sqlite3PcacheGetCachesize

`src/pcache.c` 的 `sqlite3PcacheGetCachesize(PCache *pCache)` 直接返回 `numberOfCachePages(pCache)`——即返回的是**换算后的页数**，不是 `szCache` 里存的原始值。

该函数被 `#ifdef SQLITE_TEST` 包住，只在测试构建里存在。它的调用方是 `src/pager.c` 的 `sqlite3PagerStats`，放在返回数组的 `a[2]` 位置。

### 设置 spill 阈值：sqlite3PcacheSetSpillsize

`src/pcache.c` 的 `sqlite3PcacheSetSpillsize(PCache *p, int mxPage)`：

```c
int sqlite3PcacheSetSpillsize(PCache *p, int mxPage){
  int res;
  assert( p->pCache!=0 );
  if( mxPage ){
    if( mxPage<0 ){
      mxPage = (int)((-1024*(i64)mxPage)/(p->szPage+p->szExtra));
    }
    p->szSpill = mxPage;
  }
  res = numberOfCachePages(p);
  if( res<p->szSpill ) res = p->szSpill;
  return res;
}
```

三个关键点：

- **`mxPage == 0` 时不修改任何设置**。函数注释写明"Make no changes if the argument is zero"。这使"传 0"成为查询当前 spill 阈值的手段——`src/btree.c` 的 `sqlite3BtreeSetSpillSize` 注释也印证了这一点："If zero is passed as an argument, no changes are made to the spill size setting, so using mxPage of 0 is a way to query the current spill size."
- **负值走字节口径换算**，公式与 `numberOfCachePages` 相同（`-1024 × |mxPage| / (szPage + szExtra)`）。
- **返回值是"有效 spill 阈值"**：`res` 先取缓存页数 `numberOfCachePages(p)`，若它小于 `szSpill` 则改用 `szSpill`。也就是返回 `max(缓存页数, szSpill)`——函数注释说"Return the effective cache-spill size, which will be the larger of the szSpill and szCache"。

spill 阈值的实际消费点是 `src/pcache.c` 的 `sqlite3PcacheFetchStress`：判断条件是 `if( sqlite3PcachePagecount(pCache)>pCache->szSpill )`，只有当前缓存页数**超过** `szSpill` 才进入 spill 分支。所以 `szSpill` 是"上界 + 1"语义——页数等于 `szSpill` 时不 spill。

### 缓存对象大小：sqlite3PcacheSize

`src/pcache.c` 的 `sqlite3PcacheSize(void)` 返回 `sizeof(PCache)`，即管理层自己这个结构体占多少字节。它的调用方是 `src/pager.c` 的 `sqlite3PagerOpen`：用 `int pcacheSize = sqlite3PcacheSize();` 拿到字节数，作为 pager 内存分配的一部分。`sqlite3PcacheOpen` 的注释也说明了这个契约——"Storage space to hold the object has already been allocated and is passed in as the p pointer. The caller discovers how much space needs to be allocated by calling sqlite3PcacheSize()."

这个函数与"缓存容量"不是一回事：它管的是**元数据结构体**的尺寸，不是缓存里能放多少页。

### 调用链：从 PRAGMA 到 PCache

外部入口的完整路径（按调用顺序）：

1. `src/pragma.c` 的 `PragTyp_CACHE_SIZE` 分支：`sqlite3Atoi(zRight)` 解析出的值写入 `pDb->pSchema->cache_size`，然后调用 `sqlite3BtreeSetCacheSize(pDb->pBt, pDb->pSchema->cache_size)`。
2. `src/btree.c` 的 `sqlite3BtreeSetCacheSize`：进入 B-Tree 后调用 `sqlite3PagerSetCachesize(pBt->pPager, mxPage)`。
3. `src/pager.c` 的 `sqlite3PagerSetCachesize`：调用 `sqlite3PcacheSetCachesize(pPager->pPCache, mxPage)`。

spill 侧路径：

1. `src/pragma.c` 的 `PragTyp_CACHE_SPILL` 分支：`sqlite3GetInt32(zRight, &size)` 解析出的值（`size` 初值为 `1`）传给 `sqlite3BtreeSetSpillSize(pDb->pBt, size)`；同时 `sqlite3GetBoolean` 决定置位或清除 `db->flags` 上的 `SQLITE_CacheSpill`，最后调 `setAllPagerFlags(db)`。
2. `src/btree.c` 的 `sqlite3BtreeSetSpillSize`：调用 `sqlite3PagerSetSpillsize(pBt->pPager, mxPage)` 并把其返回值原样返回。
3. `src/pager.c` 的 `sqlite3PagerSetSpillsize`：调用 `sqlite3PcacheSetSpillsize(pPager->pPCache, mxPage)` 并返回其值。

读回当前 spill 阈值走的是同一条链：`src/pragma.c` 的 `PragTyp_CACHE_SPILL` 分支在 `!zRight` 时调用 `sqlite3BtreeSetSpillSize(pDb->pBt, 0)`（传 0 = 只查询不改设置）。

`src/btree.c` 的 `sqlite3BtreeSetCacheSize` 与 `sqlite3BtreeSetSpillSize` 都在 `sqlite3BtreeEnter(p)` / `sqlite3BtreeLeave(p)` 之间执行，并断言 `sqlite3_mutex_held(p->db->mutex)`。

### 默认容量

- 编译期默认：`src/sqliteLimit.h` 里 `#ifndef SQLITE_DEFAULT_CACHE_SIZE / # define SQLITE_DEFAULT_CACHE_SIZE -2000`。该处的注释写明"The default suggested cache size is -2000, which means the cache size is limited to 2048000 bytes of memory"——即 2000 KiB。
- 生效点：`src/btree.c` 的 B-Tree 打开路径里，当 `sqlite3BtreeSchema(p, 0, 0)==0` 时调用 `sqlite3BtreeSetCacheSize(p, SQLITE_DEFAULT_CACHE_SIZE)`。注释说明这一句在"打开已存在的共享 pager-cache"时会被跳过，以免覆盖共享缓存的设置。
- `src/prepare.c` 的 schema 加载路径里：`size = sqlite3AbsInt32(meta[BTREE_DEFAULT_CACHE_SIZE-1]);`，若 `size==0` 则回落到 `SQLITE_DEFAULT_CACHE_SIZE`；schema 里没有该项时 `pDb->pSchema->cache_size = SQLITE_DEFAULT_CACHE_SIZE`。
- `src/pragma.c` 的 `PragTyp_DEFAULT_CACHE_SIZE` 分支把 `SQLITE_DEFAULT_CACHE_SIZE` 写进 `OP_SetCookie` 的 `aOp[6].p1`，`aOp[1]` 位置用 `BTREE_DEFAULT_CACHE_SIZE`（`src/btree.h` 里 `#define BTREE_DEFAULT_CACHE_SIZE 3`）作为 cookie 索引。

### 页大小变更时的容量传递

`src/pcache.c` 的 `sqlite3PcacheSetPageSize(PCache *pCache, int szPage)` 在页大小变化时重建底层缓存对象：新对象由 `xCreate(szPage, pCache->szExtra + ROUND8(sizeof(PgHdr)), pCache->bPurgeable)` 创建后，立刻调用 `sqlite3GlobalConfig.pcache2.xCachesize(pNew, numberOfCachePages(pCache))` 把当前容量传下去——因为每页成本变了，负值字节口径算出的页数也会变。

该函数有前置断言 `assert( pCache->nRefSum==0 && pCache->pDirty==0 )`，调用方必须保证没有未释放的页引用和脏页。

## 关键取舍

**用"页数"作为内部统一口径。** 管理层的 `szCache` 与 `szSpill` 都是页数，字节口径只作为 `szCache` 的一个负数编码存在，在 `numberOfCachePages` 这一个函数里折算。这样 spill 判断（`sqlite3PcachePagecount(pCache) > pCache->szSpill`）可以直接用页数比较，不必每次重算字节。

**换算用整数除法，向下取整。** `(-1024*(i64)p->szCache)/(p->szPage+p->szExtra)` 是整数除法，结果向下取整；负值口径下实际拿到的内存**不超过**用户请求的字节数。乘数先转成 `i64` 再算，避免 `szCache` 较大时 32 位溢出。

**负值口径设 10 亿页的硬上限。** `numberOfCachePages` 里 `if( n>1000000000 ) n = 1000000000;`。截断发生在换算之后，作用是防止极大的负值请求换算出溢出 int 的页数。

**`szSpill` 与 `szCache` 相互独立，但有效值取二者较大者。** `sqlite3PcacheSetSpillsize` 返回 `max(numberOfCachePages(p), p->szSpill)`。这意味着把 spill 阈值设得比缓存容量小，实际不会提前 spill——`src/pragma.c` 的 `PragTyp_CACHE_SPILL` 注释也明确写了"If the number of cache_spill pages is less then the number of cache_size pages, no spilling occurs until the page count exceeds the number of cache_size pages."

**传 0 表示"查询"而非"清零"。** `sqlite3PcacheSetSpillsize` 用 `if( mxPage )` 包住赋值分支，所以 0 永远不会把 `szSpill` 写成 0。这条约定贯穿 `sqlite3BtreeSetSpillSize` → `sqlite3PagerSetSpillsize` → `sqlite3PcacheSetSpillsize` 三层，`src/pragma.c` 的读回分支正是靠它实现。

**容量是"软上限"。** `src/btree.c` 的 `sqlite3BtreeSetCacheSize` 注释写明："Unused and unmodified pages will be recycled when the number of pages in the cache exceeds this soft limit. But the size of the cache is allowed to grow larger than this limit if it contains dirty pages or pages still in active use."——即含脏页或仍被引用的页时，缓存可以超出上限。

**底层再叠一层可回收组上限。** `src/pcache1.c` 的 `pcache1Cachesize` 把 `nMax` 汇总到 `pGroup->nMaxPage`，并以 `0x7fff0000 - pGroup->nMaxPage + pCache->nMax` 为界截断；`pGroup->mxPinned = pGroup->nMaxPage + 10 - pGroup->nMinPage`，其中 `nMin` 是每个可回收缓存的保留页数——`src/pcache1.c` 在创建可回收缓存时把它设为 `10` 并累加到 `pGroup->nMinPage`；另有 `pCache->n90pct = pCache->nMax*9/10` 用于回收触发判断。这层限制的是**可回收**（purgeable）页的总量，与单个缓存的 `szCache` 是两个维度。

**架构推论：** `src/pcache.c` 的管理层只把容量当作一个数往下传（`xCachesize`），并不解释这个数的语义；"这个缓存该多大"由 pager/btree/PRAGMA 层决定，"这么多页放不下怎么办"由 `src/pcache1.c` 决定。也就是说容量策略是分层而非集中的：管理层提供换算与转发的机制，策略参数（默认值、PRAGMA 语义）留在上层。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pcache.c | sqlite3PcacheSetCachesize | 存下原始 `szCache`，把换算后的页数交给 `pcache2.xCachesize` |
| src/pcache.c | sqlite3PcacheGetCachesize | 返回换算后的缓存页数（仅 `SQLITE_TEST` 下编译） |
| src/pcache.c | sqlite3PcacheSetSpillsize | 设置 `szSpill`；`mxPage` 为 0 时不改设置；返回 `max(缓存页数, szSpill)` |
| src/pcache.c | sqlite3PcacheSize | 返回 `sizeof(PCache)`，供调用方预分配管理层结构体 |
| src/pcache.c | numberOfCachePages | 按 `szCache` 符号换算页数：正数按页数，负数按 `|N|×1024/(szPage+szExtra)` 字节，上限 1000000000 页 |
| src/pcache.c | sqlite3PcacheOpen | 初始化 `szCache = 100`、`szSpill = 1`、记录 `szPage` 与 `szExtra` |
| src/pcache.c | sqlite3PcacheSetPageSize | 页大小变化时重建底层缓存并用 `numberOfCachePages` 传回容量 |
| src/pcache.c | sqlite3PcacheFetchStress | 消费 `szSpill`：页数超过 `szSpill` 才进入 spill 分支 |
| src/pcache.c | sqlite3PCachePercentDirty | 以 `numberOfCachePages` 为分母计算脏页百分比 |
| src/pcache1.c | pcache1Cachesize | `xCachesize` 默认实现：维护 `nMax`、`n90pct`、`pGroup->nMaxPage` 与 `mxPinned` |
| src/pcache1.c | pcache1EnforceMaxPage | 按 `nPurgeable > nMaxPage` 回收可回收页 |
| src/pragma.c | PragTyp_CACHE_SIZE | 解析 `PRAGMA cache_size`，写入 `pSchema->cache_size` 并调 `sqlite3BtreeSetCacheSize` |
| src/pragma.c | PragTyp_CACHE_SPILL | 解析 `PRAGMA cache_spill`，调 `sqlite3BtreeSetSpillSize` 并置 `SQLITE_CacheSpill` 标志 |
| src/pragma.c | PragTyp_DEFAULT_CACHE_SIZE | 把 `SQLITE_DEFAULT_CACHE_SIZE` 写入 schema cookie |
| src/btree.c | sqlite3BtreeSetCacheSize | 加锁后转发 `sqlite3PagerSetCachesize` |
| src/btree.c | sqlite3BtreeSetSpillSize | 加锁后转发 `sqlite3PagerSetSpillsize`，返回其值 |
| src/btree.c | sqlite3BtreeOpen | B-Tree 打开时用 `SQLITE_DEFAULT_CACHE_SIZE` 设置默认容量 |
| src/pager.c | sqlite3PagerSetCachesize | 转发 `sqlite3PcacheSetCachesize` |
| src/pager.c | sqlite3PagerSetSpillsize | 转发 `sqlite3PcacheSetSpillsize` |
| src/pager.c | sqlite3PagerOpen | 用 `sqlite3PcacheSize()` 取得字节数并分配 PCache |
| src/pager.c | sqlite3PagerStats | 在 `a[2]` 位置返回 `sqlite3PcacheGetCachesize` 的结果 |
| src/prepare.c | sqlite3InitOne | 读 schema cookie 中的 cache_size，为 0 时回落到 `SQLITE_DEFAULT_CACHE_SIZE` 并调 `sqlite3BtreeSetCacheSize` |
| src/sqliteLimit.h | SQLITE_DEFAULT_CACHE_SIZE | 编译期默认容量，默认值为 `-2000`（对应 2048000 字节） |

## 与主核 / AI 的关系

**架构推论：** 本页描述的是 SQLite 原生 `src/pcache.c` 的容量策略，与本仓 `engine/db/` 主核的存储层不是同一套实现；主核的 `engine/db/storage/` 与 `engine/db/wal/` 是自研的分层存储引擎，其容量上限不来自 `src/pcache.c`。可迁移的是本页的取舍经验，而不是代码。

对 AI 辅助开发有实际价值的一点是：SQLite 把"负数 = 字节、正数 = 页数"这一用户可见的复合约定，**收敛到一个纯函数**（`numberOfCachePages`）里，其余代码只处理页数。这让口径换算成为可单独验证的单元——本仓 `engine/db/` 的自研存储层同样存在"按页数还是按字节配置缓存"的接口设计问题，把换算集中到单一函数、并对越界值（如本例的 1000000000 页硬上限）做显式截断，是可以直接沿用的模式。

另一处值得注意的实现细节是"0 = 查询"的约定（`sqlite3PcacheSetSpillsize` 的 `if( mxPage )` 保护），它让同一函数同时承担设置与读取两种职责，避免再开一个 getter 而多维护一份状态。

## 参考

- SQLite 官方文档：PRAGMA cache_size（[sqlite.org/pragma.html#pragma_cache_size](https://sqlite.org/pragma.html#pragma_cache_size)）—— 说明 `cache_size` 正数按页数、负数按 KiB 的对外语义。
- SQLite 官方文档：PRAGMA cache_spill（[sqlite.org/pragma.html#pragma_cache_spill](https://sqlite.org/pragma.html#pragma_cache_spill)）—— 说明 spill 阈值与 `cache_size` 的关系，以及布尔形式对所有附加库生效。
- SQLite 官方文档：Compile-time Options（[sqlite.org/compile.html](https://sqlite.org/compile.html)）—— 说明 `SQLITE_DEFAULT_CACHE_SIZE` 编译期选项。
- SQLite 官方文档：In-Memory Databases（[sqlite.org/inmemorydb.html](https://sqlite.org/inmemorydb.html)）—— 说明 `bPurgeable` 为假时 `pcache1Cachesize` 不生效的原因。
- SQLite 源码镜像（GitHub）：[sqlite/sqlite 仓库的 src/pcache.c](https://github.com/sqlite/sqlite/blob/master/src/pcache.c) —— 本页 refs 指向的文件。
- SQLite 源码镜像（GitHub）：[sqlite/sqlite 仓库的 src/pcache1.c](https://github.com/sqlite/sqlite/blob/master/src/pcache1.c) —— 默认页缓存实现，含 `pcache1Cachesize` 与 `pcache1EnforceMaxPage`。
