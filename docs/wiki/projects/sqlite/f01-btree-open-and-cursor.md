---
project: sqlite
feature: btree-open-and-cursor
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreeOpen, sqlite3BtreeCursor, sqlite3BtreeCloseCursor, sqlite3BtreeCursorZero]
---

# B-tree 打开与游标

## 一句话职责

`sqlite3BtreeOpen` 负责把一个数据库文件（或内存库）打开成可用的 B-tree 句柄并解析数据库头，`sqlite3BtreeCursor` 在指定根页上建立游标，`sqlite3BtreeCursorZero` 提供游标的零初始化，`sqlite3BtreeCloseCursor` 负责解链并释放游标。

## 机制是什么

B-tree 层在 SQLite 里是 VDBE 与 Pager 之间的中间层：上层给出一个「根页号 + 键比较规则」，B-tree 把它变成一个可以定位、遍历、增删单元的游标；下层只负责把页读进内存。页本身怎么落地（I/G 组）、单元内容怎么解析（F02 组）不在本页范围内。

这一层有两个对象：

- `Btree` 是「一个连接对某个文件的打开句柄」，每次 `sqlite3BtreeOpen` 产生一个；共享缓存模式下多个 `Btree` 可以共用同一个 `BtShared`。
- `BtShared` 是「文件级共享状态」，持有 pager、页大小、可用空间、页数 `nPage`、根页 `pPage1`、游标链表 `pCursor` 等。共享缓存下由 `pBt->nRef` 计数引用。

游标 `BtCursor` 本身是「指向某个 b-tree 内某个具体单元的指针」，由 `MemPage` 加页内单元下标 `MemPage.aCell[]` 共同定位。`BtCursor` 结构体被刻意切成两半：`BTCURSOR_FIRST_UNINIT`（即 `pBt` 字段）之前的成员由 `sqlite3BtreeCursorZero` 清零，之后的成员必须由 `btreeCursor` 手工初始化。分界线存在的唯一原因是性能——`apPage[]` 与 `aiIdx[]` 两个数组体积大且随后必被赋值，没必要 memset。

## 怎么实现的

### 一、打开：`sqlite3BtreeOpen`

1. **判定库类型。** `isTempDb` 为真当且仅当 `zFilename==0` 或首字符为空。`isMemdb` 为真当且仅当文件名恰为 `":memory:"`、或它是临时库且 `sqlite3TempInMemory(db)` 为真、或 `vfsFlags` 带 `SQLITE_OPEN_MEMORY`。命中 `isMemdb` 就给 `flags` 追加 `BTREE_MEMORY`；若同时带 `SQLITE_OPEN_MAIN_DB`，则把 `vfsFlags` 的 `SQLITE_OPEN_MAIN_DB` 换成 `SQLITE_OPEN_TEMP_DB`。
2. **前置断言。** `flags` 必须落在 8 位内；`BTREE_UNORDERED` 只能与 `BTREE_SINGLE` 同时出现；`BTREE_SINGLE` 只能用于临时库/临时文件。
3. **分配 `Btree`。** `sqlite3MallocZero(sizeof(Btree))`，失败直接返回 `SQLITE_NOMEM_BKPT`。`p->inTrans` 初始化为 `TRANS_NONE`。
4. **共享缓存查找。** 当 `vfsFlags` 带 `SQLITE_OPEN_SHAREDCACHE` 时，把文件名转成全路径（内存库则直接复制），在全局 `sqlite3SharedCacheList` 里按「全路径相同且 pager 的 VFS 相同」找已有的 `BtShared`。
   - 若命中，但同一个 `sqlite3` 连接里已经有 `Btree` 指向这个 `BtShared`（遍历 `db->aDb[iDb].pBt`），函数**返回 `SQLITE_CONSTRAINT`**——源码注释说明原因：同一连接里存在两个以上 `BtShared` 会导致加锁出问题。
   - 若命中且无冲突，则复用该 `BtShared` 并 `pBt->nRef++`，跳过后面的文件头解析。
5. **新建 `BtShared` 并解析文件头。** `sqlite3MallocZero(sizeof(BtShared))`，随后 `sqlite3PagerOpen` 打开文件、`sqlite3PagerSetMmapLimit`（取 `db->szMmap`）、`sqlite3PagerReadFileheader` 读前 100 字节到 `zDbHeader`。任一步失败走 `btree_open_out` 统一清理。
6. **解析页大小。** 页大小取自文件头偏移 16 处的 2 字节：`pBt->pageSize = (zDbHeader[16]<<8) | (zDbHeader[17]<<16)`。若 `pageSize < 512`、`pageSize > SQLITE_MAX_PAGE_SIZE`（`sqliteLimit.h` 中默认 `65536`）、或 `pageSize` 不是 2 的幂（判定式 `((pBt->pageSize-1)&pBt->pageSize)!=0`），则把 `pageSize` 置 0 并按「空库」处理，`nReserve` 取 0。
7. **解析保留区与 autoVacuum。** 合法页大小时，保留区大小是文件头偏移 20 的单字节 `zDbHeader[20]`，同时置位 `BTS_PAGESIZE_FIXED`；`autoVacuum` 取自偏移 `36 + 4*4` 处的 4 字节，`incrVacuum` 取自偏移 `36 + 7*4` 处的 4 字节（非零即真）。页大小非法时改用编译期默认值：`autoVacuum = (SQLITE_DEFAULT_AUTOVACUUM ? 1 : 0)`、`incrVacuum = (SQLITE_DEFAULT_AUTOVACUUM==2 ? 1 : 0)`，但 `":memory:"` 库例外——此时不启用 autoVacuum。
8. **设置 pager 参数。** `sqlite3PagerSetPagesize(pBt->pPager, &pBt->pageSize, nReserve)`，然后 `pBt->usableSize = pBt->pageSize - nReserve`。
9. **只读与安全删除标记。** `sqlite3PagerIsreadonly` 为真则置 `BTS_READ_ONLY`；定义了 `SQLITE_SECURE_DELETE` 则置 `BTS_SECURE_DELETE`，否则定义了 `SQLITE_FAST_SECURE_DELETE` 则置 `BTS_OVERWRITE`。
10. **登记共享列表与兄弟链。** `p->sharable` 为真时把 `BtShared` 挂到全局共享链表，并把新的 `Btree` 按 `pBt` 指针地址升序插入同一连接的兄弟链表。
11. **成功收尾。** `sqlite3BtreeSchema(p, 0, 0)==0`（说明 schema 尚未加载，即这是一个新库）时，调用 `sqlite3BtreeSetCacheSize(p, SQLITE_DEFAULT_CACHE_SIZE)` 设置页缓存大小；`SQLITE_DEFAULT_CACHE_SIZE` 在 `sqliteLimit.h` 中默认为 `-2000`，负值表示按字节计（`sqliteLimit.h` 注释说明含义是限制为 2048000 字节）。若 pager 底层文件有 `pMethods`，还会执行 `sqlite3OsFileControlHint(pFile, SQLITE_FCNTL_PDB, &pBt->db)`。

### 二、建立游标：`sqlite3BtreeCursor` → `btreeCursor`

1. `sqlite3BtreeCursor` 只做一件事：若 `p->sharable` 为真，走 `btreeCursorWithLock`（内部 `sqlite3BtreeEnter` / `sqlite3BtreeCursor` / `sqlite3BtreeLeave` 加锁）；否则直接调 `btreeCursor`。加锁是共享缓存模式下必需的，因为同一 `BtShared` 被多个连接共享。
2. `btreeCursor` 的 `wrFlag` 只允许三种取值：`0`、`BTREE_WRCSR`、`BTREE_WRCSR|BTREE_FORDELETE`（`btree.h` 中 `BTREE_WRCSR=0x00000004`、`BTREE_FORDELETE=0x00000008`）。
3. **前置条件用断言表达**：调用方必须已持有所需的表锁（`hasSharedCacheTableLock`），写游标不能存在读冲突（`hasReadConflicts`），必须已开启事务（`p->inTrans>TRANS_NONE`，写游标要求 `p->inTrans==TRANS_WRITE`），并且根页 1 必须已加载（`pBt->pPage1 && pBt->pPage1->aData`），写游标要求没有 `BTS_READ_ONLY`。
4. **根页合法性检查。** `iTable<1` 返回 `SQLITE_CORRUPT_BKPT`；`iTable==1` 且 `btreePagecount(pBt)==0`（`btreePagecount` 即返回 `pBt->nPage`）时，把 `iTable` 改为 0 表示空库。
5. **填充游标字段。** 设置 `pCur->pgnoRoot = iTable`、`pCur->iPage = -1`、`pCur->pKeyInfo = pKeyInfo`、`pCur->pBtree = p`、`pCur->pBt = pBt`、`pCur->curFlags = 0`、`pCur->eState = CURSOR_INVALID`。
6. **多游标标记。** 遍历 `pBt->pCursor` 链表，若已有游标的 `pgnoRoot` 与本次相同，则把双方的 `curFlags` 都置上 `BTCF_Multiple`——同一 b-tree 上有第二个游标时必须互相知道，否则写游标的修改可能被同表读游标看到。
7. **挂入链表。** `pCur->pNext = pBt->pCursor; pBt->pCursor = pCur;`。
8. **读写标志与临时空间。** `wrFlag` 非零时置 `BTCF_WriteFlag`、`pCur->curPagerFlags = 0`，并保证 `pBt->pTmpSpace` 已分配（`allocateTempSpace` 用 `sqlite3PageMalloc(pBt->pageSize)` 分配一整页大小，其前 4 字节预留给左孩子指针，分配时把前 8 字节清零以避免未初始化字节被写入文件）；否则 `pCur->curPagerFlags = PAGER_GET_READONLY`。

### 三、游标零初始化：`sqlite3BtreeCursorZero`

只做一件事：`memset(p, 0, offsetof(BtCursor, BTCURSOR_FIRST_UNINIT))`。也就是只清 `eState`、`curFlags`、`curPagerFlags`、`hints`、`skipNext`、`pBtree`、`aOverflow`、`pKey` 这些字段，跳过 `apPage[]`、`aiIdx[]` 等大数组。调用方在 `sqlite3BtreeCursor` 之前必须先调用它——源码注释明确写「假定调用 `sqlite3BtreeCursor()` 之前已调用 `sqlite3BtreeCursorZero()`」。VDBE 的实际调用点在 `src/vdbe.c` 的 `allocateCursor`：`sqlite3BtreeCursorZero(pCx->uc.pCursor)`。

### 四、关闭游标：`sqlite3BtreeCloseCursor`

1. 若 `pCur->pBtree` 为空，直接返回 `SQLITE_OK`（重复关闭是安全的）。
2. `sqlite3BtreeEnter(pBtree)` 加锁。
3. **从链表摘除**：若 `pBt->pCursor==pCur` 则令 `pBt->pCursor = pCur->pNext`；否则沿 `pNext` 找到前驱，把 `pPrev->pNext = pCur->pNext`。
4. **释放页引用**：`btreeReleaseAllCursorPages(pCur)` 遍历 `pCur->apPage[0..iPage-1]` 调 `releasePageNotNull`，再释放 `pCur->pPage`，最后把 `iPage` 置 -1。
5. **释放读锁**：`unlockBtreeIfUnused(pBt)`——当 `pBt->inTransaction==TRANS_NONE` 且 `pBt->pPage1!=0` 时，把 `pBt->pPage1` 置 0 并调 `releasePageOne(pPage1)`，源码注释说明这是「unref 第一页，作用是释放读锁」。
6. **释放内存**：`sqlite3_free(pCur->aOverflow)`、`sqlite3_free(pCur->pKey)`。
7. **单文件库自动关闭**：若 `pBt->openFlags` 带 `BTREE_SINGLE` 且此时 `pBt->pCursor==0`，直接调 `sqlite3BtreeClose(pBtree)`（此时 `pBtree->sharable==0`，源码注释说明因此不必配对 `sqlite3BtreeLeave`）；否则调 `sqlite3BtreeLeave(pBtree)` 释放锁。
8. 最后 `pCur->pBtree = 0`，返回 `SQLITE_OK`。

## 关键取舍

- **游标内存由调用方提供，层与层解耦。** `sqlite3BtreeCursor` 的最后一个参数是调用方给的 `BtCursor *`，B-tree 不分配游标本体，只填内容。VDBE 把 `BtCursor` 直接放在 VDBE 内存块里（`src/vdbe.c` 的 `allocateCursor` 用 `pCx->uc.pCursor = (BtCursor*)&pMem->z[SZ_VDBECURSOR(nField)]`），游标随 VDBE 内存一起释放，`src/vdbeaux.c` 的游标清理处才调 `sqlite3BtreeCloseCursor`。代价是调用方必须自己保证内存对齐与生命周期。
- **只清一半内存，是刻意的性能优化。** `sqlite3BtreeCursorZero` 用 `offsetof(BtCursor, BTCURSOR_FIRST_UNINIT)` 把 memset 范围截在 `pBt` 之前，源码注释说明原因是 `apPage[]` 与 `aiIdx[]` 数组大且随后必被赋值。代价是 `BtCursor` 的「谁负责初始化哪些字段」变成一条隐式契约，靠 `BTCURSOR_FIRST_UNINIT` 这个宏和结构体里的注释维持。
- **页大小直接从文件头读，而不是走参数。** `sqlite3BtreeOpen` 用 `zDbHeader[16]`/`zDbHeader[17]` 算出页大小，再用 `pageSize<512`、`pageSize>SQLITE_MAX_PAGE_SIZE`、非 2 的幂三条判据决定它是否合法；不合法就退化成「新库」，此时页大小交由 pager 按 `SQLITE_DEFAULT_PAGE_SIZE` 默认值决定，autoVacuum 退回 `SQLITE_DEFAULT_AUTOVACUUM` 编译期默认。这一取舍让打开一个损坏文件不至于直接失败，而是当作空库重新初始化。
- **同一连接重复打开同一共享库直接报错，不做复用。** 源码注释给出的理由是：同一连接里出现两个 `BtShared` 会导致加锁出问题。因此命中共享缓存但发现本连接已有同 `BtShared` 的 `Btree` 时，`sqlite3BtreeOpen` 返回 `SQLITE_CONSTRAINT` 而不是继续。
- **关闭游标与关闭 B-tree 解耦，但单文件库有例外。** `sqlite3BtreeCloseCursor` 正常路径只是摘链、释放页引用与缓冲；但当 `openFlags` 带 `BTREE_SINGLE` 且这是最后一个游标时，它直接调 `sqlite3BtreeClose(pBtree)` 把整个 b-tree 关掉。这是 `BTREE_SINGLE`（「文件最多含 1 个 b-tree」，`btree.h` 定义为 `4`）语义的必然结果。
- **写游标受共享缓存的多重条件约束，且这些条件在断言里而非运行时。** `btreeCursor` 的源码注释列出可写的四个条件：带 `BTREE_WRCSR`、其他共享连接的同表游标不处于 `READ_UNCOMMITTED` 状态、数据库可写（无 `BTS_READ_ONLY`）、存在活动事务。前两条在实现里体现为 `hasSharedCacheTableLock` 与 `hasReadConflicts` 两个只用于断言的函数（`btree.c` 中它们被 `#ifdef SQLITE_DEBUG` 包住）。**架构推论：** 这意味着非 debug 构建下共享缓存的写冲突不会被 `btreeCursor` 拦截，而是依赖更上层（VDBE/共享缓存的锁管理）保证，换来的是发布版路径上没有额外开销。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/btree.c` | `sqlite3BtreeOpen` | 打开 B-tree：判定内存/临时库、查找或新建 `BtShared`、解析文件头得出页大小与保留区、设置 pager 参数 |
| `src/btree.c` | `sqlite3BtreeCursor` | 对外游标入口：按 `p->sharable` 决定是否走带锁路径 |
| `src/btree.c` | `btreeCursor` | 实际建游标：校验根页与事务条件、填充游标字段、置 `BTCF_Multiple`、挂入 `pBt->pCursor` 链表、分配写游标临时空间 |
| `src/btree.c` | `btreeCursorWithLock` | 共享缓存下的加锁包装：`sqlite3BtreeEnter` / `btreeCursor` / `sqlite3BtreeLeave` |
| `src/btree.c` | `sqlite3BtreeCursorZero` | 游标零初始化：只 memset `BTCURSOR_FIRST_UNINIT` 之前的字段 |
| `src/btree.c` | `sqlite3BtreeCloseCursor` | 关闭游标：从链表摘除、释放页引用与缓冲、释放读锁、`BTREE_SINGLE` 时顺带关闭 B-tree |
| `src/btree.c` | `btreeReleaseAllCursorPages` | 释放游标持有的全部页引用并把 `iPage` 置 -1 |
| `src/btree.c` | `unlockBtreeIfUnused` | 无活动事务时释放首页引用，作用是释放文件读锁 |
| `src/btree.c` | `allocateTempSpace` | 为写游标分配 `pBt->pTmpSpace`（一页大小），前 4 字节预留左孩子指针 |
| `src/btree.c` | `sqlite3BtreeClose` | 关闭 B-tree：回滚事务、关 pager、释放 schema 与临时空间 |
| `src/btree.c` | `btreePagecount` | 返回 `pBt->nPage`，用于判定空库 |
| `src/btree.c` | `hasSharedCacheTableLock` | 断言用：检查当前连接是否持有对应根页的读写锁 |
| `src/btree.c` | `hasReadConflicts` | 断言用：检查是否有其它连接在读同一张表 |
| `src/btreeInt.h` | `BTCURSOR_FIRST_UNINIT` | `BtCursor` 中首个必须手工初始化字段的标记，`sqlite3BtreeCursorZero` 以它为 memset 终点 |
| `src/btreeInt.h` | `BTCURSOR_MAX_DEPTH` | 游标页栈深度上限（值为 `20`），决定 `apPage[]` 与 `aiIdx[]` 数组长度 |
| `src/btreeInt.h` | `BTCF_Multiple` | 标记同一 b-tree 上存在多个游标 |
| `src/btree.h` | `BTREE_SINGLE` | 「文件最多含 1 个 b-tree」标志（值 `4`），触发关闭游标时连带关闭 B-tree |
| `src/btree.h` | `BTREE_WRCSR` / `BTREE_FORDELETE` | 写游标标志（`0x00000004`）与 seek-and-delete 提示（`0x00000008`） |
| `src/vdbe.c` | `allocateCursor` | VDBE 侧游标的实际创建点：调 `sqlite3BtreeCursorZero` 后调 `sqlite3BtreeCursor` |
| `src/vdbeaux.c` | — | VDBE 释放游标时调 `sqlite3BtreeCloseCursor` 的位置 |
| `src/main.c` | `sqlite3BtreeOpen` | 主库打开的调用方：打开后紧接 `sqlite3SchemaGet` 装载 schema |

## 与主核 / AI 的关系

主核（`engine/db/`）自有独立的 B-tree 实现，不复用 SQLite 的 btree 层，因此本页的机制对主核没有直接移植价值。可迁移的是**接口形状**：`sqlite3BtreeCursor` 把「根页号 + 键比较规则 + 读写意图」收敛成一次调用，调用方提供游标内存而不由底层分配——这一形状让上层（VDBE）能把游标嵌进自己的内存 arena，生命周期与语句绑定。

对 AI 辅助开发的实际意义有两条：第一，**打开期的失败点是可枚举的**——`sqlite3BtreeOpen` 的错误出口只有 `SQLITE_NOMEM_BKPT`、`SQLITE_CONSTRAINT`、pager 打开/读头失败三类，页面大小非法不算失败而是退化成新库；据此可以判断「打开失败」时该查内存、共享缓存重复打开，还是文件与 VFS 层。第二，**游标失效有明确的三个状态位**（`CURSOR_VALID`、`CURSOR_INVALID`、`CURSOR_FAULT`），其中 `skipNext` 在 `CURSOR_FAULT` 状态下装的是故障错误码而非跳过计数——这意味着「游标移动出错」与「游标已在表尾」在状态上是可区分的，调试时不能只看游标是否还有效。

## 参考

- [SQLite 官方文档：文件格式（数据库头布局、页大小、保留区）](https://sqlite.org/fileformat2.html) —— 支撑文件头偏移 16 的页大小、偏移 20 的保留区、偏移 36 起的 autoVacuum 标志的语义
- [SQLite 官方文档：B-tree 实现](https://sqlite.org/src/doc/trunk/doc/b-tree.html) —— 支撑 B-tree 层与 pager 层的职责划分
- [SQLite 官方文档：sqlite3BtreeCursor 接口](https://sqlite.org/src/doc/trunk/doc/btree.h) —— 支撑 `BTREE_WRCSR` / `BTREE_FORDELETE` 等标志与游标协议
- [GitHub 源码页：src/btree.c](https://github.com/sqlite/sqlite/blob/master/src/btree.c) —— 支撑 `sqlite3BtreeOpen`、`sqlite3BtreeCursor`、`sqlite3BtreeCloseCursor`、`sqlite3BtreeCursorZero` 的实现细节
- [GitHub 源码页：src/btreeInt.h](https://github.com/sqlite/sqlite/blob/master/src/btreeInt.h) —— 支撑 `BtCursor` 结构体分段、`BTCURSOR_FIRST_UNINIT`、`BTCURSOR_MAX_DEPTH` 的定义
