---
project: sqlite
feature: connection-status-metrics
page_type: feature
schema_version: 1
refs:
  - path: src/status.c
    symbols: [sqlite3_status, sqlite3_db_status, sqlite3StatusUp, sqlite3StatusDown]
---

# 连接与全局状态统计

## 一句话职责

`src/status.c` 提供两套对外的运行指标接口：`sqlite3_status()` / `sqlite3_status64()` 报告**进程级**指标（堆内存占用、单次分配上限、页缓存用量），`sqlite3_db_status()` / `sqlite3_db_status64()` 报告**单个连接级**指标（lookaside 槽位占用与命中、页缓存内存、模式内存、语句内存、缓存命中/未命中/写回、临时溢出字节、未决外键约束），两者都同时返回当前值与高水位，并支持把高水位重置回当前值。

## 机制是什么

SQLite 把「运行状态」拆成两层，每层的存储位置、并发保护方式、取值来源都不同：

- **库级（global）**：状态向量 `sqlite3Stat` 是一份进程内唯一的静态结构（`src/status.c` 里 `static SQLITE_WSD struct sqlite3StatType sqlite3Stat`），内含 `nowValue[10]` 与 `mxValue[10]` 两个长度为 10 的数组。数组长度由 `sqlite3StatType` 的定义决定，而 `nowValue` 的下标空间被 `SQLITE_STATUS_*` 常量占满：`SQLITE_STATUS_MEMORY_USED` 为 0、`SQLITE_STATUS_PAGECACHE_USED` 为 1、`SQLITE_STATUS_PAGECACHE_OVERFLOW` 为 2、`SQLITE_STATUS_SCRATCH_USED` 为 3、`SQLITE_STATUS_SCRATCH_OVERFLOW` 为 4、`SQLITE_STATUS_MALLOC_SIZE` 为 5、`SQLITE_STATUS_PARSER_STACK` 为 6、`SQLITE_STATUS_PAGECACHE_SIZE` 为 7、`SQLITE_STATUS_SCRATCH_SIZE` 为 8、`SQLITE_STATUS_MALLOC_COUNT` 为 9（常量定义在 `src/sqlite.h.in`）。其中 `SQLITE_STATUS_SCRATCH_USED`、`SQLITE_STATUS_SCRATCH_OVERFLOW`、`SQLITE_STATUS_SCRATCH_SIZE` 三个在头文件里被标注为 `NOT USED`，对应官方文档中的 `No longer used`。
- **连接级（db）**：数据不在 `status.c` 里维护，而是现算。`sqlite3_db_status64()` 在一个 `switch (op)` 里逐个 case 去问对应的子系统：lookaside 问 `db->lookaside`，页缓存问 Pager，模式内存问 `db->aDb[i].pSchema`，语句内存问 `db->pVdbe` 链表，未决外键问 `db->nDeferredImmCons` / `db->nDeferredCons`。

两个层级共用同一套「当前值 + 高水位 + 可重置」的语义，但实现方式完全相反：库级是真维护两个数组（`sqlite3StatusUp()` 负责同时维护当前值与高水位），连接级是**当场计算**当前值，高水位要么来自连接内自己记的计数器（lookaside 的 `anStat[3]`），要么直接写成 0。

`SQLITE_DBSTATUS_*` 常量取值为：`SQLITE_DBSTATUS_LOOKASIDE_USED` 为 0、`SQLITE_DBSTATUS_CACHE_USED` 为 1、`SQLITE_DBSTATUS_SCHEMA_USED` 为 2、`SQLITE_DBSTATUS_STMT_USED` 为 3、`SQLITE_DBSTATUS_LOOKASIDE_HIT` 为 4、`SQLITE_DBSTATUS_LOOKASIDE_MISS_SIZE` 为 5、`SQLITE_DBSTATUS_LOOKASIDE_MISS_FULL` 为 6、`SQLITE_DBSTATUS_CACHE_HIT` 为 7、`SQLITE_DBSTATUS_CACHE_MISS` 为 8、`SQLITE_DBSTATUS_CACHE_WRITE` 为 9、`SQLITE_DBSTATUS_DEFERRED_FKS` 为 10、`SQLITE_DBSTATUS_CACHE_USED_SHARED` 为 11、`SQLITE_DBSTATUS_CACHE_SPILL` 为 12、`SQLITE_DBSTATUS_TEMPBUF_SPILL` 为 13，头文件里 `SQLITE_DBSTATUS_MAX` 取 13，注释为 `Largest defined DBSTATUS`。

## 怎么实现的

### 状态向量的存放与 WSD 兜底

`src/status.c` 用 `SQLITE_WSD` 修饰 `sqlite3Stat`，并用 `wsdStat` / `wsdStatInit` 两个宏做编译期分派：

- 未定义 `SQLITE_OMIT_WSD` 时，`wsdStat` 直接展开成静态变量 `sqlite3Stat`，`wsdStatInit` 展开为空。
- 定义了 `SQLITE_OMIT_WSD`（目标平台不支持可写静态数据）时，`wsdStatInit` 展开成 `sqlite3StatType *x = &GLOBAL(sqlite3StatType,sqlite3Stat)`，把状态向量挪到运行期分配的进程本地存储里。

计数值本身的宽度也随平台变化：`src/status.c` 里 `SQLITE_PTRSIZE>4` 时 `sqlite3StatValueType` 取 `sqlite3_int64`，否则取 `u32`。这个 typedef 直接决定 `nowValue` / `mxValue` 的元素宽度。

### 并发保护：按指标归属选锁

`src/status.c` 里有一张静态表 `statMutex[10]`，每个元素标明该指标由哪把互斥量保护：值为 0 表示由内存分配器互斥量（`sqlite3MallocMutex()`）保护，值为 1 表示由页缓存互斥量（`sqlite3Pcache1Mutex()`）保护。表的内容依次是：内存已用量为 0、页缓存已用页数为 1、页缓存溢出字节数为 1、scratch 已用量为 0、scratch 溢出量为 0、单次分配大小为 0、解析器栈深度为 0、页缓存分配大小为 1、scratch 大小为 0、malloc 计数为 0。

所有读写的入口都用同一个模式选锁：`sqlite3StatusValue()`、`sqlite3StatusUp()`、`sqlite3StatusDown()`、`sqlite3StatusHighwater()` 四者都在 `assert()` 里检查 `sqlite3_mutex_held(statMutex[op] ? sqlite3Pcache1Mutex() : sqlite3MallocMutex())`；而 `sqlite3_status64()` 是自己显式加锁——它先算出 `pMutex = statMutex[op] ? sqlite3Pcache1Mutex() : sqlite3MallocMutex()`，再 `sqlite3_mutex_enter(pMutex)` 读出 `nowValue[op]` 与 `mxValue[op]`，若 `resetFlag` 非零则把 `mxValue[op]` 改写为 `nowValue[op]`，最后 `sqlite3_mutex_leave(pMutex)`。函数末尾有 `(void)pMutex;` 一行，注释写明用途是 `Prevent warning when SQLITE_THREADSAFE=0`。

### sqlite3StatusUp() 与 sqlite3StatusDown()：唯一的写入点

`sqlite3StatusUp(int op, int N)` 的函数体：先 `assert( op>=0 && op<ArraySize(wsdStat.nowValue) )` 与 `assert( op>=0 && op<ArraySize(statMutex) )`，再做持锁断言；随后 `wsdStat.nowValue[op] += N;`，然后 `if( wsdStat.nowValue[op]>wsdStat.mxValue[op] ) wsdStat.mxValue[op] = wsdStat.nowValue[op];`。也就是说**只有上升时才动高水位**。

`sqlite3StatusDown(int op, int N)` 的函数体：先 `assert( N>=0 )`（下降量必须非负），再做与 `sqlite3StatusUp()` 相同的 op 范围断言与持锁断言，最后只做 `wsdStat.nowValue[op] -= N;`——**高水位保持不变**。源码注释写明了这条规则：`The StatusDown() routine lowers the current value by N. The highwater mark is unchanged.`

这两个函数是全局状态唯一的写入口，实际调用方只有两处：

- `src/malloc.c` 的 `mallocWithAlarm()`：分配成功后按 `nFull = sqlite3MallocSize(p)` 调 `sqlite3StatusUp(SQLITE_STATUS_MEMORY_USED, nFull)`，并调 `sqlite3StatusUp(SQLITE_STATUS_MALLOC_COUNT, 1)`。`src/malloc.c` 的 `sqlite3_free()` 释放时对应地调 `sqlite3StatusDown(SQLITE_STATUS_MEMORY_USED, sqlite3MallocSize(p))` 与 `sqlite3StatusDown(SQLITE_STATUS_MALLOC_COUNT, 1)`。`src/malloc.c` 的 realloc 路径在新旧尺寸不等时调 `sqlite3StatusUp(SQLITE_STATUS_MEMORY_USED, nNew-nOld)`，其中 `nNew` 取 `sqlite3MallocSize(pNew)`、`nOld` 取 `sqlite3MallocSize(pOld)`。
- `src/pcache1.c` 的 `pcache1Alloc()`：从静态页缓存缓冲区取到槽位时调 `sqlite3StatusUp(SQLITE_STATUS_PAGECACHE_USED, 1)`；回退到 `sqlite3Malloc()` 时调 `sqlite3StatusUp(SQLITE_STATUS_PAGECACHE_OVERFLOW, sz)`，其中 `sz` 取 `sqlite3MallocSize(p)`。`src/pcache1.c` 的 `pcache1Free()` 释放槽位时调 `sqlite3StatusDown(SQLITE_STATUS_PAGECACHE_USED, 1)`，释放溢出的堆内存时调 `sqlite3StatusDown(SQLITE_STATUS_PAGECACHE_OVERFLOW, nFreed)`，其中 `nFreed` 取 `sqlite3MallocSize(p)`。

注意这些调用点全部被 `sqlite3GlobalConfig.bMemstat` 或 `SQLITE_DISABLE_PAGECACHE_OVERFLOW_STATS` 之类的编译开关包住（例如 `src/malloc.c` 的 `sqlite3Malloc()` 只在 `sqlite3GlobalConfig.bMemstat` 为真时才走 `mallocWithAlarm()` 路径，否则直接调 `sqlite3GlobalConfig.m.xMalloc()`），所以统计的开关本身就决定了这些计数器是否被维护。

### 高水位的另两种来源

`sqlite3StatusHighwater(int op, int X)` 是第三个写入口，只改高水位不改当前值：函数体先 `assert( X>=0 )`，把 `X` 转成 `sqlite3StatValueType newValue`，然后断言 `op` 只能是 `SQLITE_STATUS_MALLOC_SIZE`、`SQLITE_STATUS_PAGECACHE_SIZE`、`SQLITE_STATUS_PARSER_STACK` 三者之一，最后 `if( newValue>wsdStat.mxValue[op] ) wsdStat.mxValue[op] = newValue;`。

三个真实调用方分别是：

- `src/malloc.c` 的 `mallocWithAlarm()` 在 `sqlite3GlobalConfig.m.xRoundup(n)` 之后调 `sqlite3StatusHighwater(SQLITE_STATUS_MALLOC_SIZE, n)`，记录的是**用户请求的原始大小** `n` 而不是对齐后的 `nFull`；`src/malloc.c` 的 realloc 路径同样调 `sqlite3StatusHighwater(SQLITE_STATUS_MALLOC_SIZE, (int)nBytes)`。
- `src/pcache1.c` 的 `pcache1Alloc()` 在两条路径上都调 `sqlite3StatusHighwater(SQLITE_STATUS_PAGECACHE_SIZE, nByte)`，记录交给页缓存分配器的请求字节数。
- `src/tokenize.c` 在解析器栈增长时调 `sqlite3StatusHighwater(SQLITE_STATUS_PARSER_STACK, ...)`，记录最深解析栈深度。

### 读取入口与错误分支

`sqlite3_status64(int op, sqlite3_int64 *pCurrent, sqlite3_int64 *pHighwater, int resetFlag)` 的错误分支：先 `if( op<0 || op>=ArraySize(wsdStat.nowValue) ) return SQLITE_MISUSE_BKPT;`——越界直接返回 misuse 错误码，不碰任何状态。在 `SQLITE_ENABLE_API_ARMOR` 编译下还有 `if( pCurrent==0 || pHighwater==0 ) return SQLITE_MISUSE_BKPT;`。成功路径返回 `SQLITE_OK`。

`sqlite3_status()` 是 32 位薄壳：先在 `SQLITE_ENABLE_API_ARMOR` 下做同样的空指针检查，然后调 `sqlite3_status64(op, &iCur, &iHwtr, resetFlag)`，只在 `rc==0` 时才把 64 位值转成 `int` 写回 `*pCurrent` 与 `*pHighwater`，最后返回 `rc`。转换是直接强转 `(int)iCur`，不截位。

### 连接级：sqlite3_db_status64() 的逐项实现

`sqlite3_db_status64()` 先取 `db->mutex`（`sqlite3_mutex_enter(db->mutex)`），在 `SQLITE_ENABLE_API_ARMOR` 下还会做 `sqlite3SafetyCheckOk(db)` 检查，然后进入 `switch( op )`，末尾统一 `sqlite3_mutex_leave(db->mutex)` 并返回 `rc`。默认分支 `default: { rc = SQLITE_ERROR; }`，即未知动词返回 `SQLITE_ERROR`。

各分支的实测行为：

- **`SQLITE_DBSTATUS_LOOKASIDE_USED`**：调 `sqlite3LookasideUsed(db, &H)` 取当前值，`*pHighwtr` 写 `H`。`sqlite3LookasideUsed()` 的函数体用 `countLookasideSlots()` 分别数出 `db->lookaside.pInit` 与 `db->lookaside.pFree` 两个链表的长度（`countLookasideSlots()` 就是一条 `while( p ){ p = p->pNext; cnt++; }` 的链表遍历），在 `SQLITE_OMIT_TWOSIZE_LOOKASIDE` 未定义时再加上 `pSmallInit` 与 `pSmallFree` 两段；返回值是 `db->lookaside.nSlot - (nInit+nFree)`，高水位是 `db->lookaside.nSlot - nInit`。函数内断言 `db->lookaside.nSlot >= nInit+nFree`。`resetFlag` 非零时，把 `pFree` 链表尾接到 `pInit` 链表头上（`p->pNext = db->lookaside.pInit; db->lookaside.pInit = db->lookaside.pFree; db->lookaside.pFree = 0;`），小链表同样处理一次——即把所有空闲槽位「退还」为未使用状态。
- **`SQLITE_DBSTATUS_LOOKASIDE_HIT` / `_MISS_SIZE` / `_MISS_FULL`**：这三个动词在源码里靠 `op - SQLITE_DBSTATUS_LOOKASIDE_HIT` 的偏移取 `db->lookaside.anStat[]` 数组，函数里有 `assert( (op-SQLITE_DBSTATUS_LOOKASIDE_HIT)>=0 )` 与 `assert( (op-SQLITE_DBSTATUS_LOOKASIDE_HIT)<3 )` 守住边界。`anStat` 在 `src/sqliteInt.h` 里被注释为 `0: hits. 1: size misses. 2: full misses`。这三个分支里 `*pCurrent` 被写成 0，值只经 `*pHighwtr` 输出；`resetFlag` 非零时把对应的 `anStat` 项清零。三个 `anStat` 计数点的埋设位置在 `src/malloc.c` 的 `sqlite3DbMallocRawNN()`：请求大小 `n>db->lookaside.sz` 时 `db->lookaside.anStat[1]++`；从小池或大池取到槽位时 `db->lookaside.anStat[0]++`；两条链表都空、走 `dbMallocRawFinish()` 兜底时 `db->lookaside.anStat[2]++`。
- **`SQLITE_DBSTATUS_CACHE_USED` / `_CACHE_USED_SHARED`**：遍历 `i` 从 0 到 `db->nDb`，对每个非空的 `db->aDb[i].pBt` 取 `sqlite3BtreePager(pBt)` 得到 Pager，调 `sqlite3PagerMemUsed(pPager)` 累加。`_CACHE_USED_SHARED` 分支额外把每个 Pager 的数值除以 `sqlite3BtreeConnectionCount(pBt)`——按连接数摊分共享缓存的内存。整段被 `sqlite3BtreeEnterAll(db)` / `sqlite3BtreeLeaveAll(db)` 包住。这两个分支的 `*pHighwtr` 被写成 0，源码注释明确写 `The highwater mark is meaningless and is returned as zero`。
- **`SQLITE_DBSTATUS_SCHEMA_USED`**：函数体先置 `db->pnBytesFreed = &nByte`，把 `db->lookaside.pEnd` 临时挪到 `db->lookaside.pStart`（`assert( db->lookaside.pEnd==db->lookaside.pTrueEnd )` 守着前置条件），遍历 `db->nDb` 个 schema，把 `tblHash` / `trigHash` / `idxHash` / `fkeyHash` 四个哈希表的元素个数乘以 `sqlite3GlobalConfig.m.xRoundup(sizeof(HashElem))` 累加，再加四个哈希表自身 `sqlite3_msize()` 的开销；随后遍历 `trigHash` 调 `sqlite3DeleteTrigger()`、遍历 `tblHash` 调 `sqlite3DeleteTable()`，靠 `db->pnBytesFreed` 这个计数器把释放的字节数累加起来。收尾时把 `db->pnBytesFreed` 置 0、`db->lookaside.pEnd` 还原成 `db->lookaside.pTrueEnd`。`*pHighwtr` 写成 0。
- **`SQLITE_DBSTATUS_STMT_USED`**：同样借 `db->pnBytesFreed` 与 `db->lookaside.pEnd` 的挪位，遍历 `db->pVdbe` 链表（`pVdbe=db->pVdbe; pVdbe; pVdbe=pVdbe->pVNext`）逐个调 `sqlite3VdbeDelete(pVdbe)`，把释放的字节数累加到 `nByte`。`*pHighwtr` 写成 0。
- **`SQLITE_DBSTATUS_CACHE_HIT` / `_CACHE_MISS` / `_CACHE_WRITE` / `_CACHE_SPILL`**：`SQLITE_DBSTATUS_CACHE_SPILL` 的处理方式是 `op = SQLITE_DBSTATUS_CACHE_WRITE+1;` 然后 `deliberate_fall_through` 落到后面三个 case。函数里有 `assert( SQLITE_DBSTATUS_CACHE_MISS==SQLITE_DBSTATUS_CACHE_HIT+1 );` 与 `assert( SQLITE_DBSTATUS_CACHE_WRITE==SQLITE_DBSTATUS_CACHE_HIT+2 );`。遍历 `db->nDb`，对每个有 `pBt` 的连接调 `sqlite3PagerCacheStat(pPager, op, resetFlag, &nRet)` 累加进 `nRet`。`src/pager.c` 的 `sqlite3PagerCacheStat()` 里先 `eStat -= SQLITE_DBSTATUS_CACHE_HIT;` 再 `*pnVal += pPager->aStat[eStat];`，`reset` 非零时 `pPager->aStat[eStat] = 0;`。`SQLITE_DBSTATUS_CACHE_SPILL` 之所以要用 `CACHE_WRITE+1` 而不是独立下标，源码注释写明 `The _SPILL case is not contiguous because it was added later.`，且 `src/pager.c` 的 `sqlite3PagerCacheStat()` 用 `assert( PAGER_STAT_HIT==0 && PAGER_STAT_MISS==1 && PAGER_STAT_WRITE==2 && PAGER_STAT_SPILL==3 )` 保证映射关系。这组分支的 `*pHighwtr` 写成 0。
- **`SQLITE_DBSTATUS_TEMPBUF_SPILL`**：只取 `db->aDb[1].pBt`（临时库）的 Pager，调 `sqlite3PagerCacheStat(pPager, SQLITE_DBSTATUS_CACHE_WRITE, resetFlag, &nRet)`，再 `nRet *= sqlite3BtreeGetPageSize(db->aDb[1].pBt)` 把写次数折成字节数；然后加上 `db->nSpill`；`resetFlag` 非零时 `db->nSpill = 0`。`db->nSpill` 在 `src/sqliteInt.h` 里注释为 `TEMP content spilled to disk`，`src/vdbesort.c` 里做外部排序溢出的任务会 `db->nSpill += pSorter->aTask[ii].nSpill;`。`*pHighwtr` 写成 0。
- **`SQLITE_DBSTATUS_DEFERRED_FKS`**：`*pCurrent = db->nDeferredImmCons>0 || db->nDeferredCons>0;`，即只回「有/没有」一个布尔量；`*pHighwtr` 写成 0。

`sqlite3_db_status()` 是 32 位薄壳，调 `sqlite3_db_status64()` 后在 `rc==0` 时写回 `*pCurrent = C & 0x7fffffff;` 与 `*pHighwtr = H & 0x7fffffff;`。`& 0x7fffffff` 是显式掩码而不是截断，这一点与 `sqlite3_status()` 的直接强转不同。

### 谁会真正调用这些接口

`src/shell.c.in` 的命令行外壳是真实调用方：它用 `sqlite3_status64(iStatusCtrl, &iCur, &iHiwtr, bReset)` 查全局指标，并用 `sqlite3_db_status(db, SQLITE_DBSTATUS_LOOKASIDE_USED, ...)`、`SQLITE_DBSTATUS_LOOKASIDE_HIT`、`SQLITE_DBSTATUS_LOOKASIDE_MISS_SIZE`、`SQLITE_DBSTATUS_LOOKASIDE_MISS_FULL`、`SQLITE_DBSTATUS_CACHE_USED`、`SQLITE_DBSTATUS_CACHE_HIT`、`SQLITE_DBSTATUS_CACHE_MISS`、`SQLITE_DBSTATUS_CACHE_WRITE`、`SQLITE_DBSTATUS_CACHE_SPILL`、`SQLITE_DBSTATUS_SCHEMA_USED`、`SQLITE_DBSTATUS_STMT_USED` 查连接级指标，用 `sqlite3_db_status64(db, SQLITE_DBSTATUS_TEMPBUF_SPILL, ...)` 查临时溢出字节。

库内部还有两处读取全局指标的内部调用方：

- `src/malloc.c` 的 `sqlite3_memory_used()` 调 `sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &res, &mx, 0)` 并返回 `res`；`src/malloc.c` 的 `sqlite3_memory_highwater(int resetFlag)` 调 `sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &res, &mx, resetFlag)` 并返回 `mx`。
- `src/malloc.c` 的 `mallocWithAlarm()` 在 `mem0.alarmThreshold>0` 时用 `sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED)` 读当前已用内存，与 `mem0.alarmThreshold` 比较决定是否触发 `sqlite3MallocAlarm()`；`src/malloc.c` 的 `sqlite3_soft_heap_limit64()` 同样用 `sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED)` 计算超出量，再调 `sqlite3_release_memory()` 归还。`src/malloc.c` 的 realloc 路径也用 `sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED)` 与 `mem0.hardLimit` 比较来判断是否拒绝扩容。

`src/loadext.c` 把 `sqlite3_status`、`sqlite3_status64`、`sqlite3_db_status`、`sqlite3_db_status64` 一并放进扩展 API 表，`src/sqlite3ext.h` 用 `#define sqlite3_status sqlite3_api->status` 等宏把它们重定向到该表，使扩展也能调用同一套实现。

## 关键取舍

**库级状态维护「当前值 + 高水位」两个数组，连接级只算当前值。** 这是本模块最核心的分工。库级指标（内存、页缓存）是持续累加的，写入点分散在 `src/malloc.c` 与 `src/pcache1.c`，所以必须在写入时就顺手维护高水位；连接级指标（模式内存、语句内存、缓存用量）全都可以当场遍历结构体算出来，不需要额外记账，代价是这些分支的高水位一律返回 0——官方文档也确认了这一点，例如 `SQLITE_DBSTATUS_CACHE_USED` 与 `SQLITE_DBSTATUS_SCHEMA_USED` 的 highwater 标为 `always 0`。

**连接级的「内存用量」用「释放掉再量」的方式得到精确值。** `SQLITE_DBSTATUS_SCHEMA_USED` 与 `SQLITE_DBSTATUS_STMT_USED` 的实现是把 `db->pnBytesFreed` 指向一个局部计数器，然后真的调 `sqlite3DeleteTrigger()` / `sqlite3DeleteTable()` / `sqlite3VdbeDelete()` 把对象释放掉，让释放路径把字节数累加到计数器里。这不是估算，是「先拆了再量」。同一手法也用来做高水位：lookaside 的高水位用 `db->lookaside.nSlot - nInit` 得到，即「总槽位数减去从未使用过的槽位数」，`src/sqliteInt.h` 里 `Lookaside` 结构体的注释解释了这套双链表设计的用意：`This dual-list allows use to compute a high-water mark - the maximum number of allocations outstanding at any point in the past`。

**32 位接口对高位做掩码而不是截断。** `sqlite3_db_status()` 用 `& 0x7fffffff` 保证结果非负，官方文档说明这是为了在超过 2,147,483,647 时「truncate the value」，而 `sqlite3_db_status64()` 返回完整值。相比之下 `sqlite3_status()` 用的是直接强转 `(int)iCur`，官方文档对它的措辞是「the values returned by sqlite3_status() are undefined」——库级接口在数值过大时不做保证。

**每个指标绑定一把互斥量，而不是全局一把锁。** `statMutex` 表把页缓存类指标（页缓存已用页数、页缓存溢出字节、页缓存分配大小）绑到 `sqlite3Pcache1Mutex()`，其余绑到 `sqlite3MallocMutex()`。这样页缓存的计数不会去争内存分配器的锁。代价是读接口必须先查表再取锁，多一次间接。

**下降不回收高水位。** `sqlite3StatusDown()` 只改 `nowValue`，函数体里没有任何碰 `mxValue` 的语句。这让高水位真正表示「历史峰值」而不是「近期的低水位反弹」。

**未使用的常量位保留而不重排。** `SQLITE_STATUS_SCRATCH_USED` / `_SCRATCH_OVERFLOW` / `_SCRATCH_SIZE` 三个下标被占用但不再使用，头文件里用 `NOT USED` 标注。保留空位意味着 `statMutex` 表的顺序与 `SQLITE_STATUS_*` 常量一一对应，代价是表里有三个永远为 0 的条目。

**高水位只在「显式重置」时回落，不随当前值自动回落。** `sqlite3_status64()` 与 `sqlite3_db_status64()` 里 `resetFlag` 非零时才把高水位改写为当前值。这让「采集峰值」与「重置基线」成为调用方显式控制的两个动作，而不是隐式行为。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/status.c | `sqlite3_status` | 32 位库级状态查询薄壳：调 `sqlite3_status64()`，成功时把 64 位值强转回 `int` 写回 |
| src/status.c | `sqlite3_status64` | 库级状态查询：op 越界返回 `SQLITE_MISUSE_BKPT`，按 `statMutex` 选锁读写 `nowValue` / `mxValue`，`resetFlag` 非零时把高水位压回当前值 |
| src/status.c | `sqlite3_db_status` | 32 位连接级状态查询薄壳：调 `sqlite3_db_status64()`，写回时用 `& 0x7fffffff` 掩码 |
| src/status.c | `sqlite3_db_status64` | 连接级状态查询：`switch(op)` 分派到 lookaside / 页缓存 / 模式 / 语句 / 缓存计数 / 临时溢出 / 未决外键各分支，未知动词落到 `default` 返回 `SQLITE_ERROR` |
| src/status.c | `sqlite3StatusUp` | 库级当前值与高水位的唯一上升写入口：`nowValue[op] += N`，超过高水位时同步抬升高水位 |
| src/status.c | `sqlite3StatusDown` | 库级当前值的下降写入口：`nowValue[op] -= N`，不动高水位；`N` 必须非负 |
| src/status.c | `sqlite3StatusValue` | 读库级当前值，调用方必须已持有对应互斥量 |
| src/status.c | `sqlite3StatusHighwater` | 只抬升高水位不改当前值，允许的 op 仅 `SQLITE_STATUS_MALLOC_SIZE` / `SQLITE_STATUS_PAGECACHE_SIZE` / `SQLITE_STATUS_PARSER_STACK` |
| src/status.c | `sqlite3LookasideUsed` | 统计 lookaside 当前占用槽位数与历史高水位，四个链表按 `SQLITE_OMIT_TWOSIZE_LOOKASIDE` 条件增减 |
| src/status.c | `countLookasideSlots` | 遍历 lookaside 链表统计节点数，`sqlite3LookasideUsed()` 的辅助函数 |
| src/status.c | `statMutex` | 静态互斥量归属表，逐个 `SQLITE_STATUS_*` 下标标注由内存分配器锁还是页缓存锁保护 |
| src/status.c | `sqlite3Stat` | 库级状态向量：`nowValue[10]` 与 `mxValue[10]`，元素类型由 `SQLITE_PTRSIZE>4` 决定取 `sqlite3_int64` 还是 `u32` |
| src/malloc.c | `mallocWithAlarm` | `sqlite3StatusUp(SQLITE_STATUS_MEMORY_USED, ...)` 与 `sqlite3StatusUp(SQLITE_STATUS_MALLOC_COUNT, 1)` 的埋设点，并用 `sqlite3StatusValue()` 读当前用量比对 `mem0.alarmThreshold` |
| src/malloc.c | `sqlite3_free` | 对应地调 `sqlite3StatusDown(SQLITE_STATUS_MEMORY_USED, ...)` 与 `sqlite3StatusDown(SQLITE_STATUS_MALLOC_COUNT, 1)` |
| src/malloc.c | `sqlite3_soft_heap_limit64` | 用 `sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED)` 计算超限量，再调 `sqlite3_release_memory()` 归还 |
| src/malloc.c | `sqlite3_memory_used` / `sqlite3_memory_highwater` | 分别通过 `sqlite3_status64()` 取 `SQLITE_STATUS_MEMORY_USED` 的当前值与高水位 |
| src/malloc.c | `sqlite3DbMallocRawNN` | lookaside 三个命中计数的埋设点：超出 `db->lookaside.sz` 计 size miss，取到槽位计 hit，双链表都空计 full miss |
| src/pcache1.c | `pcache1Alloc` | 埋设 `SQLITE_STATUS_PAGECACHE_USED` 与 `SQLITE_STATUS_PAGECACHE_OVERFLOW` 的上升计数，并调 `sqlite3StatusHighwater()` 记录页缓存分配大小 |
| src/pcache1.c | `pcache1Free` | 对应地调 `sqlite3StatusDown()` 归还页缓存已用页数与溢出字节 |
| src/pager.c | `sqlite3PagerCacheStat` | 按 `eStat - SQLITE_DBSTATUS_CACHE_HIT` 映射到 `pPager->aStat[]`，累加或按 `reset` 清零 |
| src/pager.c | `sqlite3PagerMemUsed` | `SQLITE_DBSTATUS_CACHE_USED` 的数据来源：按单页开销乘页数再加 Pager 自身大小 |
| src/btree.c | `sqlite3BtreeConnectionCount` | `SQLITE_DBSTATUS_CACHE_USED_SHARED` 用它把共享缓存内存按连接数摊分 |
| src/btree.c | `sqlite3BtreeGetPageSize` | `SQLITE_DBSTATUS_TEMPBUF_SPILL` 用它把写次数折成字节数 |
| src/tokenize.c | `sqlite3StatusHighwater` 调用点 | 记录最深解析器栈深度到 `SQLITE_STATUS_PARSER_STACK` |
| src/sqliteInt.h | `Lookaside` | lookaside 状态结构：`anStat[3]` 注释为 `0: hits. 1: size misses. 2: full misses`，`nSlot` 为已分配槽位总数 |
| src/sqlite.h.in | `SQLITE_STATUS_*` | 库级状态常量，取值 0 到 9，其中三个 scratch 常量标注 `NOT USED` |
| src/sqlite.h.in | `SQLITE_DBSTATUS_*` | 连接级状态常量，取值 0 到 13，`SQLITE_DBSTATUS_MAX` 取 13 |

## 与主核 / AI 的关系

**架构推论：** 主核（`engine/db/`）的分层规范要求 L0 common 之下每层只允许向下依赖。若主核也要暴露运行指标，最自然的做法是仿照 SQLite 拆成「库级指标（进程唯一状态向量 + 高水位）」与「连接级指标（当场遍历连接对象计算）」两层，并让库级写入点只落在内存分配层与页缓存层这两处。若把指标散落到各层自己维护，主核的 L2/L3 层就会直接依赖统计模块，违反依赖方向约束。

**架构推论：** SQLite 这套「按指标绑定互斥量」的表驱动做法，对主核的直接启示是：指标不是无成本的可观测点。`src/status.c` 里 `sqlite3StatusUp()` 的 `assert()` 明确要求调用方已持有对应互斥量，说明「埋一个计数点」等于「把该点的锁语义也定下来」。主核若要加埋点，必须同时把该点归入某一层的锁域，而不是在任意位置随手加一个全局计数器。

**架构推论：** 主核的嵌入式 SDK（`engine/src/sdk/`）只依赖 SQLite 一个外部库。若 SDK 想把 SQLite 的指标暴露给自己的使用者，可直接复用 `sqlite3_status64()` / `sqlite3_db_status64()` 这对 64 位接口——它们不截断，官方文档明确说明 `sqlite3_db_status()` 在值超过 2,147,483,647 时会截断而 `sqlite3_db_status64()` 返回完整值。在嵌入式场景下，SDK 用 32 位变体读大数值会静默丢数据。

**架构推论：** SQLite 的连接级内存指标是「释放后测量」而非「估算」（`SQLITE_DBSTATUS_SCHEMA_USED` / `_STMT_USED` 走 `db->pnBytesFreed` 计数器实测）。这对主核的含义是：如果主核想给出「模式占多少内存」这类精确数字，代价是必须支持一次可回收的遍历。主核若改用「按对象数乘平均大小」的估算路径，则必须显式声明该数字是估算值，不能与 SQLite 的语义混同。

## 参考

- [sqlite3_status — sqlite.org C 接口文档](https://sqlite.org/c3ref/status.html)：`sqlite3_status()` / `sqlite3_status64()` 的官方语义，以及各 `SQLITE_STATUS_*` 参数的说明（`SCRATCH_*` 标注为 `No longer used`）。
- [sqlite3_db_status — sqlite.org C 接口文档](https://sqlite.org/c3ref/db_status.html)：`sqlite3_db_status()` / `sqlite3_db_status64()` 的官方语义与各 `SQLITE_DBSTATUS_*` 选项的说明，含 highwater 恒为 0 的说明。
- [sqlite/sqlite 仓库 src/status.c — GitHub 源码页](https://github.com/sqlite/sqlite/blob/master/src/status.c)：`sqlite3_status()`、`sqlite3_db_status()`、`sqlite3StatusUp()`、`sqlite3StatusDown()` 的实现原文。
- [sqlite/sqlite 仓库 src/malloc.c — GitHub 源码页](https://github.com/sqlite/sqlite/blob/master/src/malloc.c)：`sqlite3StatusUp()` / `sqlite3StatusDown()` / `sqlite3StatusValue()` / `sqlite3StatusHighwater()` 的埋设点与堆告警逻辑。
- [sqlite/sqlite 仓库 src/pcache1.c — GitHub 源码页](https://github.com/sqlite/sqlite/blob/master/src/pcache1.c)：页缓存已用页数与溢出字节的计数点，以及静态页缓存缓冲区耗尽时回退到 `sqlite3Malloc()` 的路径。
- [sqlite.org Fossil 源码树 src/status.c](https://sqlite.org/src/doc/trunk/src/status.c)：SQLite 官方 Fossil 仓库中 `src/status.c` 的在线浏览版。
