---
project: sqlite
feature: mem0-page-allocator
page_type: feature
schema_version: 1
refs:
  - path: src/mem0.c
    symbols: [sqlite3MemInit, sqlite3MemFree, sqlite3MemSize, sqlite3MemRoundup, sqlite3MemShutdown]
---

# 小内存块分配器

## 一句话职责

为小尺寸申请提供按页切槽的固定尺寸内存池：一次性切好整块缓冲区，把余下的槽位挂进空闲链表反复复用，释放时按槽归还而不是还给系统分配器。

## 机制是什么

这一层解决的是「小而频繁」的内存申请。SQLite 里大量对象（B-tree cell、记录字段、解析器临时结构）的生命周期很短，但申请次数极多；每申请一次都走一次系统 `malloc`/`free`，系统调用与分配器内部元数据开销远大于对象本身。

本层用的是 **lookaside（旁路内存）** 机制：预先从堆上取一整块连续内存，按固定槽宽切成 N 个等大的槽位，每个槽位只在空闲链表上流转——申请时从链表头摘一个，释放时挂回链表头。槽位本身永不归还系统，直到连接关闭。

边界上要跟相邻两层分清：

- 通用分配器（j03，`src/malloc.c`）负责**请求尺寸记账、软/硬堆上限、OOM 标志**，并在申请超限时做决策。
- 本层负责**单块内存的页布局与复用**：一次切槽、链表复用、整块归还。
- 字符串格式化（j06）不在本层范围。

需要特别说明的是：本页 refs 指向的 `src/mem0.c`，在当前源码检出里**只有 `SQLITE_ZERO_MALLOC` 编译分支下的空实现**——所有分配函数都直接失败，真正的 lookaside 切槽与复用逻辑落在 `src/malloc.c`（取槽/还槽）与 `src/main.c`（切槽初始化）里。下面逐条给出这两处的真实行为。

## 怎么实现的

### `src/mem0.c`：零分配兜底实现

`src/mem0.c` 整个文件被 `#ifdef SQLITE_ZERO_MALLOC` 包住，文件头注释写明这是 "no-op memory allocation drivers"，且明确说明 "Real drivers must be substituted using sqlite3_config() before SQLite will operate"。这五个函数的行为是：

- `sqlite3MemMalloc(int nByte)`：`return 0;` —— 恒定失败。
- `sqlite3MemFree(void *pPrior)`：`return;` —— 空操作，参数不参与任何判断。
- `sqlite3MemSize(void *pPrior)`：`return 0;` —— 恒返回 0。
- `sqlite3MemRoundup(int n)`：`return n;` —— 不做任何取整，原样返回。
- `sqlite3MemInit(void *NotUsed)`：`return SQLITE_OK;` —— 恒成功，参数未使用。
- `sqlite3MemShutdown(void *NotUsed)`：`return;` —— 空操作。

这个文件里唯一有外部链接的符号是 `sqlite3MemSetDefault`：它构造一个 `static const sqlite3_mem_methods defaultMethods`，把上述六个函数指针按 `xMalloc / xFree / xRealloc / xSize / xRoundup / xInit / xShutdown` 的顺序填入，最后调 `sqlite3_config(SQLITE_CONFIG_MALLOC, &defaultMethods)` 把它们装进 `sqlite3GlobalConfig.m`。

**这意味着 `SQLITE_ZERO_MALLOC` 构建下的 SQLite 完全无法工作**——所有申请返回空指针。`src/mem0.c` 提供的是"接口占位 + 明确失败"，而不是可用分配器。

### 切槽：`setupLookaside`（`src/main.c`）

`setupLookaside(sqlite3 *db, void *pBuf, int sz, int cnt)` 是真正把一整块内存切成槽位的地方，它的约束链是：

1. `sqlite3LookasideUsed(db,0)>0` 说明已有槽位在用，直接 `return SQLITE_BUSY`。
2. 若 `db->lookaside.bMalloced` 为真，先 `sqlite3_free(db->lookaside.pStart)` 把上一块缓冲区还掉，避免新旧两块同时占内存。
3. `sz = ROUNDDOWN8(sz)`：槽宽向下取整到 8 的倍数。`ROUNDDOWN8(x)` 定义在 `src/sqliteInt.h`，为 `((x)&~7)`。
4. `if( sz<=(int)sizeof(LookasideSlot*) ) sz = 0;` —— 槽宽必须大于一个指针大小，否则连空闲链表的 `pNext` 都放不下。
5. `if( sz>65528 ) sz = 65528;` —— 槽宽存放在 `db->lookaside.sz` 这个 `u16` 字段里（`sz = (u16)sz`），65528 是能让 `u16` 容纳的最大 8 的倍数。
6. `if( cnt<1 ) cnt = 0;` —— 槽数至少 1 才有意义，否则直接归零。
7. `if( sz>0 && cnt>(0x7fff0000/sz) ) cnt = 0x7fff0000/sz;` —— 总字节数封顶在 `0x7fff0000`，源码注释写的是 "not so large as to use more than 0x7fff0000 total bytes for lookaside"。
8. `szAlloc = (i64)sz*(i64)cnt;`。`szAlloc==0` 时槽宽与起点一并清零；`pBuf==0` 时用 `sqlite3BeginBenignMalloc()` / `sqlite3EndBenignMalloc()` 包住 `sqlite3Malloc(szAlloc)` 去申请（这两个宏让分配失败不触发 OOM 故障处理），申请成功后再用 `sqlite3MallocSize(pStart)` 回读真实占用写回 `szAlloc`。

**两尺寸切分**（`SQLITE_OMIT_TWOSIZE_LOOKASIDE` 未定义时启用）：

- 当 `sz>=LOOKASIDE_SMALL*3` 时：`nBig = szAlloc/(3*LOOKASIDE_SMALL+sz)`，`nSm = (szAlloc - (i64)sz*(i64)nBig)/LOOKASIDE_SMALL`。分母里的 `3*LOOKASIDE_SMALL+sz` 表明：一个 1200 字节的大槽在内存里被当成 3 个小槽的空间来折算。
- 当 `sz>=LOOKASIDE_SMALL*2`（但不足 `LOOKASIDE_SMALL*3`）时：`nBig = szAlloc/(LOOKASIDE_SMALL+sz)`，`nSm = (szAlloc - (i64)sz*(i64)nBig)/LOOKASIDE_SMALL`。
- 否则（`sz>0`）：`nBig = szAlloc/sz; nSm = 0;`，即单一尺寸。
- `sz==0` 时 `nBig = nSm = 0`。

`LOOKASIDE_SMALL` 定义在 `src/sqliteInt.h`：两尺寸 lookaside 打开时为 `128`，定义 `SQLITE_OMIT_TWOSIZE_LOOKASIDE` 时为 `0`。

**链表铺设**：大槽从 `pStart` 开始按 `sz` 步长推进，逐个挂进 `db->lookaside.pInit`（"尚未使用过"链表）；小槽从大槽区末尾的 `db->lookaside.pMiddle` 开始按 `LOOKASIDE_SMALL` 步长推进，逐个挂进 `db->lookaside.pSmallInit`。`db->lookaside.pEnd` 记为切槽后的末端指针，`db->lookaside.pTrueEnd = db->lookaside.pEnd`。最终 `db->lookaside.nSlot = nBig+nSm`。

**默认值**：`SQLITE_DEFAULT_LOOKASIDE` 定义在 `src/global.c`，两尺寸 lookaside 打开时是 `1200,40`（源码注释标为 "48KB of memory"），关闭时是 `1200,100`（"120KB of memory"）。`src/global.c` 里 `sqlite3Config` 的初始化把 `SQLITE_DEFAULT_LOOKASIDE` 填进 `szLookaside` / `nLookaside`。`src/main.c` 的连接打开路径调 `setupLookaside(db, 0, sqlite3GlobalConfig.szLookaside, sqlite3GlobalConfig.nLookaside)` 启用该子系统。

`src/global.c` 的注释给了这组默认值的实际意义："The default configuration of 1200,40 actually provides 30 1200-byte slots and 93 128-byte slots"——也就是 30 个大槽 + 93 个小槽，比旧版 1200,100 的单一尺寸配置提供了更多 lookaside。

### 取槽：`sqlite3DbMallocRawNN`（`src/malloc.c`）

`sqlite3DbMallocRawNN(sqlite3 *db, u64 n)` 的取槽顺序是固定的：

1. `if( n>db->lookaside.sz )` —— 请求超过槽宽，走 `dbMallocRawFinish(db, n)` 回落到堆分配。此时若 `db->lookaside.bDisable` 为 0，`db->lookaside.anStat[1]++` 记一次"尺寸不匹配"；`bDisable` 为 0 但 `db->mallocFailed` 已置位则直接 `return 0`。
2. `if( n<=LOOKASIDE_SMALL )` —— 先试小池：摘 `db->lookaside.pSmallFree` 的头（已释放过的小槽），为空则摘 `db->lookaside.pSmallInit` 的头（从未用过的小槽）。命中任一都 `db->lookaside.anStat[0]++`。
3. 再试大池：摘 `db->lookaside.pFree` 的头，为空则摘 `db->lookaside.pInit` 的头，同样 `anStat[0]++`。
4. 两条链表都空：`db->lookaside.anStat[2]++` 记一次"池满"，落到 `dbMallocRawFinish`。

`anStat[3]` 的语义在 `src/sqliteInt.h` 的结构体注释里写明："0: hits. 1: size misses. 2: full misses"。

`dbMallocRawFinish` 是 `SQLITE_NOINLINE` 的慢路径：调 `sqlite3Malloc(n)`（走 j03 的策略层），返回 0 则 `sqlite3OomFault(db)`，成功后用 `sqlite3MemdebugSetType` 打上 `MEMTYPE_LOOKASIDE`（`bDisable==0` 时）或 `MEMTYPE_HEAP`（`bDisable` 非 0 时）。

**`bDisable` 是关掉 lookaside 的开关**：`src/sqliteInt.h` 定义 `DisableLookaside` 为 `db->lookaside.bDisable++;db->lookaside.sz=0`，`EnableLookaside` 为 `db->lookaside.bDisable--;db->lookaside.sz=db->lookaside.bDisable?0:db->lookaside.szTrue`。因为 `sqlite3DbMallocRawNN` 的第一个判断就是 `n>db->lookaside.sz`，把 `sz` 置 0 就等于让所有请求都超限、直接走堆。`src/prepare.c` 在解析期用 `pParse->disableLookaside` 计数并配合这两个宏临时关闭 lookaside。

### 还槽：`sqlite3DbFreeNN`（`src/malloc.c`）

`sqlite3DbFreeNN(sqlite3 *db, void *p)` 用指针区间判断归属：

- `((uptr)p) < (uptr)(db->lookaside.pEnd)` 且 `>= (uptr)(db->lookaside.pMiddle)` —— 是小槽：挂回 `db->lookaside.pSmallFree` 链表头，直接 `return`。
- `((uptr)p) < (uptr)(db->lookaside.pEnd)` 且 `>= (uptr)(db->lookaside.pStart)` —— 是大槽：挂回 `db->lookaside.pFree` 链表头，直接 `return`。
- 都不满足 —— 不是 lookaside 槽位，走 `sqlite3_free(p)` 归还系统。

两条还槽分支都带 `assert( db->pnBytesFreed==0 )`——这个断言的前提是 `pnBytesFreed` 路径下的指针不在 lookaside 区间内。

`SQLITE_DEBUG` 构建下，两条分支都会先 `memset(p, 0xaa, ...)` 把槽内容涂成 `0xaa`（小槽涂 `LOOKASIDE_SMALL` 字节，大槽涂 `db->lookaside.szTrue` 字节），源码注释写 "Trash freed content"，用来暴露"用了已释放的内存"这类错误。

`sqlite3DbNNFreeNN` 是同一套区间判断逻辑的另一份拷贝，额外要求 `db!=0`。`sqlite3DbFree(db, p)` 是薄封装：`p` 非空时转调 `sqlite3DbFreeNN`。

### 尺寸回读

`sqlite3DbMallocSize(sqlite3 *db, const void *p)` 对 lookaside 区间内的指针直接返回槽宽而不去问底层分配器：落在 `pMiddle` 之上返回常量 `LOOKASIDE_SMALL`，落在 `pStart` 到 `pMiddle` 之间返回 `db->lookaside.szTrue`，都不在区间才回落 `sqlite3GlobalConfig.m.xSize((void*)p)`。这是必须的——lookaside 块来自一整块连续内存，逐槽问系统分配器会得到错误答案。

### 槽位占用统计

`sqlite3LookasideUsed(sqlite3 *db, int *pHighwater)` 在 `src/status.c`：分别用 `countLookasideSlots` 数出 `pInit`、`pFree`、`pSmallInit`、`pSmallFree` 四条链表的长度（`countLookasideSlots` 逐个走 `p->pNext` 累加），返回 `nSlot - (nInit+nFree)`，即已用槽数；`pHighwater` 非空时写入 `nSlot - nInit`。`src/status.c` 的 `sqlite3_db_status64` 在 `SQLITE_DBSTATUS_LOOKASIDE_USED` 分支调用它，并支持把 `pFree` 整条并回 `pInit`、`pSmallFree` 整条并回 `pSmallInit` 以重置统计。

### 尺寸取整：`sqlite3MemRoundup`

`src/mem1.c` 的 `sqlite3MemRoundup(int n)` 实现为 `return ROUND8(n);`，`ROUND8(x)` 定义在 `src/sqliteInt.h` 为 `(((x)+7)&~7)`——向上取整到 8 的倍数。`src/mem2.c` 的同名 `sqlite3MemRoundup` 实现完全一致。`src/sqlite.h.in` 里 `xRoundup` 的注释说明了它的契约："The xRoundup method returns what would be the allocated size of a memory allocation given a particular requested size... If xRoundup returns 0, that causes the corresponding memory allocation to fail."

`src/mem1.c` 的 `sqlite3MemMalloc` 在没有 `SQLITE_MALLOCSIZE` 宏时用 `sqlite3_int64` 在分配块头部存下原始请求长度（`p = SQLITE_MALLOC(nByte+8); p[0] = nByte; p++;`），`sqlite3MemSize` 再把指针回退一格读回这个长度——这就是 `sqlite3MemSize` 的实现原理：在"不能向系统问尺寸"的平台上自带尺寸账本。

## 关键取舍

- **一次性切块，换取申请速度**：切槽在连接建立时一次完成，之后每次小申请只是一次链表摘除，避开了系统分配器的元数据维护与 `free` 的合并逻辑。代价是这块内存从建立连接起就被整块占住，即使期间没有小对象产生。
- **固定尺寸，不做变长**：`setupLookaside` 只支持"一个大尺寸 + 一个小尺寸（`LOOKASIDE_SMALL`）"两档。第三档请求直接走 `dbMallocRawFinish` 回落到堆。这是刻意的：变长会让空闲链表退化成需要搜索的分配器，抵消掉按页分配的全部收益。
- **槽宽上限 65528 是类型决定的**：`db->lookaside.sz` 是 `u16`，`setupLookaside` 里 `if( sz>65528 ) sz = 65528;` 把槽宽压到 `u16` 能容纳的最大 8 的倍数（65528 = 8191 × 8）。这是硬约束，不是可调策略。
- **总量上限 0x7fff0000 是防御性的**：`cnt` 被 `0x7fff0000/sz` 夹住，保证 lookaside 总字节数不超过约 2GB，防止一次误配置申请走天文数字的内存。
- **两尺寸折算是空间换命中率**：大槽在账面上被当作 3 个小槽的空间来折算（分母 `3*LOOKASIDE_SMALL+sz`），意味着 1200 字节的槽会"让出"一部分空间给 128 字节的小槽。`src/global.c` 的注释直接给出收益：默认 `1200,40` 实际提供 30 个 1200 字节槽 + 93 个 128 字节槽，比旧版 `1200,100` 的可用 lookaside 更多。代价是大尺寸请求更容易落空到堆。
- **还槽时涂毒只在 DEBUG 构建**：`memset(p, 0xaa, ...)` 被 `#ifdef SQLITE_DEBUG` 包住。生产构建不涂，DEBUG 构建涂——用生产性能换取开发期的悬挂指针检出能力。
- **`MEMTYPE_LOOKASIDE` 标记的是"可能来自 lookaside"而非"一定来自"**：`src/sqliteInt.h` 的注释写明 "If an allocation is MEMTYPE_LOOKASIDE, that means it might have been allocated by lookaside, except the allocation was too large or lookaside was already full."。因此 `sqlite3MemdebugSetType` 的检查条件是 `sqlite3GlobalConfig.m.xFree==sqlite3MemFree`——只有当前分配器就是 mem2 的调试分配器时才生效；`SQLITE_MEMDEBUG` 未定义时 `sqlite3MemdebugSetType` 是空宏、`sqlite3MemdebugHasType` / `sqlite3MemdebugNoType` 恒返回 1。
- **关掉 lookaside 的开关是计数器而非布尔**：`DisableLookaside` 做 `bDisable++`，`EnableLookaside` 做 `bDisable--`，且只有 `bDisable` 归零时才恢复 `db->lookaside.sz = db->lookaside.szTrue`。这样嵌套的 disable/enable 不会互相踩踏。`src/prepare.c` 用 `pParse->disableLookaside` 做同样的引用计数。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/mem0.c | sqlite3MemInit | `SQLITE_ZERO_MALLOC` 分支下的初始化桩，恒返回 `SQLITE_OK` |
| src/mem0.c | sqlite3MemFree | `SQLITE_ZERO_MALLOC` 分支下的释放桩，空操作 |
| src/mem0.c | sqlite3MemSize | `SQLITE_ZERO_MALLOC` 分支下的尺寸桩，恒返回 0 |
| src/mem0.c | sqlite3MemRoundup | `SQLITE_ZERO_MALLOC` 分支下的取整桩，原样返回入参不做取整 |
| src/mem0.c | sqlite3MemShutdown | `SQLITE_ZERO_MALLOC` 分支下的反初始化桩，空操作 |
| src/mem0.c | sqlite3MemSetDefault | 把上述桩函数填入 `sqlite3_mem_methods` 并经 `sqlite3_config(SQLITE_CONFIG_MALLOC, ...)` 注册 |
| src/main.c | setupLookaside | 切槽核心：按 `ROUNDDOWN8` 对齐槽宽、夹紧槽宽与槽数上限、两尺寸折算出 `nBig`/`nSm`、铺设 `pInit` 与 `pSmallInit` 空闲链表 |
| src/main.c | sqlite3_db_config | `SQLITE_DBCONFIG_LOOKASIDE` 分支转调 `setupLookaside(db, pBuf, sz, cnt)` |
| src/malloc.c | sqlite3DbMallocRawNN | 取槽：超槽宽落 `dbMallocRawFinish`；先小池后大池，依次取 `pSmallFree`/`pSmallInit`/`pFree`/`pInit` |
| src/malloc.c | dbMallocRawFinish | 取槽落空时的慢路径：走 `sqlite3Malloc` 并按 `bDisable` 打 `MEMTYPE_LOOKASIDE`/`MEMTYPE_HEAP` 标记 |
| src/malloc.c | sqlite3DbFreeNN | 还槽：按指针落在 `pMiddle`/`pStart` 区间的归属判断，挂回 `pSmallFree` 或 `pFree` 链表头 |
| src/malloc.c | sqlite3DbMallocSize | 对 lookaside 区间内的指针直接返回 `LOOKASIDE_SMALL` 或 `szTrue`，不回落到 `xSize` |
| src/mem1.c | sqlite3MemRoundup | 默认分配器的尺寸取整，实现为 `ROUND8(n)` |
| src/mem1.c | sqlite3MemSize | 默认分配器的尺寸回读；无 `SQLITE_MALLOCSIZE` 时从块头部的 `sqlite3_int64` 长度字段读回 |
| src/status.c | sqlite3LookasideUsed | 统计四条链表长度，算出已用槽数与高水位 |
| src/sqliteInt.h | LOOKASIDE_SMALL | 小槽尺寸常量，两尺寸 lookaside 打开时为 128 |
| src/sqliteInt.h | ROUND8 / ROUNDDOWN8 | 8 字节对齐的向上取整与向下取整宏 |
| src/global.c | SQLITE_DEFAULT_LOOKASIDE | 默认槽宽与槽数，两尺寸打开时为 `1200,40`（48KB） |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 自带的 `engine/db/src/storage/heap.c` 实现的是带页布局的通用堆，与 SQLite 的 lookaside 不是同一套机制：lookaside 只服务"小且频繁"的两档固定尺寸申请，超出就整块回落到通用分配器；主核的堆要承接页级、变长、带元数据的申请。两者可对照的点是同一类权衡——用"整块预取 + 槽位复用"换取申请路径的常数开销。

**架构推论：** 主核的嵌入式 SDK（`engine/src/sdk/`）只依赖 SQLite，运行在内存受限环境里。SQLite 这一层暴露了 `sqlite3_db_config(db, SQLITE_DBCONFIG_LOOKASIDE, pBuf, sz, cnt)`，允许应用自带一块缓冲区当 lookaside 池——SDK 可以用这个入口把自管理的内存池接给 SQLite，避免 SDK 自身再叠一层分配器。

**架构推论：** `sqlite3MemdebugSetType` / `sqlite3MemdebugHasType` / `sqlite3MemdebugNoType` 这组带 `MEMTYPE_HEAP` / `MEMTYPE_LOOKASIDE` 掩码的断言接口，是把"内存来源"做成可断言属性的机制。AI 辅助的缺陷定位若沿用这套思路，可以把"这块内存来自哪里"变成可机检的约束，而不是靠人读代码判断。

## 参考

- SQLite 内存分配接口（`sqlite3_mem_methods` / `SQLITE_CONFIG_MALLOC`）：<https://sqlite.org/c3ref/mem.html>
- SQLite 自带分配器列表（mem1/mem2/mem3/mem5）：<https://sqlite.org/malloc.html>
- `sqlite3_db_config` 与 `SQLITE_DBCONFIG_LOOKASIDE`：<https://sqlite.org/c3ref/db_config.html>
- `sqlite3_status` / `SQLITE_DBSTATUS_LOOKASIDE_USED`：<https://sqlite.org/c3ref/status.html>
- 源码镜像 `src/mem0.c`：<https://github.com/sqlite/sqlite/blob/master/src/mem0.c>
- 源码镜像 `src/malloc.c`（lookaside 取槽与还槽）：<https://github.com/sqlite/sqlite/blob/master/src/malloc.c>
- 源码镜像 `src/main.c`（`setupLookaside` 切槽）：<https://github.com/sqlite/sqlite/blob/master/src/main.c>
