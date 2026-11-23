---
project: redis
feature: dict-hashtable
page_type: feature
schema_version: 1
refs:
  - path: src/dict.c
    symbols: [dictAdd, dictFind, dictRehash]
---

# 哈希表与渐进 rehash

## 一句话职责

`src/dict.c` 的 dict 是 Redis 内部通用的哈希容器：它把「桶数翻倍」拆成一次搬一个桶的渐进 rehash，让查找与插入在任何时刻都只是常数级工作量，扩容成本被摊进日常读写，而不是集中在某一刻停下重建整表。

## 机制是什么

dict 的基本结构是「桶数组 + 冲突链」。`dictEntry` 由 next 指针、键指针和一个值联合体组成；桶数组是 `dictEntry**`，桶数恒为 2 的幂——`DICTHT_SIZE(exp)` 是 `1<<exp`，`DICTHT_SIZE_MASK(exp)` 是 `size-1`，所以定位就是一次 `hash & mask`。默认哈希函数是 SipHash（`dictGenHashFunction` 调 `siphash`），种子是 16 字节随机量 `dict_hash_function_seed`，由 `dictSetHashFunctionSeed` 注入。冲突用链式处理：新元素挂到桶头，`dictInsertKeyAtLink` 里的注释写明理由——在数据库场景里「recently added entries are accessed more frequently」。

**双表并存是 rehash 的载体。** 平时只有 `ht_table[0]` 在用；扩容时新建 `ht_table[1]`，`rehashidx` 记录下一个要搬的桶下标（`-1` 表示当前不在 rehash）。rehash 期间**新元素一律插进表 1**，查找必须同时查两张表，`ht_used[0]` 是还没搬完的元素数、`ht_used[1]` 是已经搬过去的。

**扩容与收缩的阈值。** 元素数达到桶数（1:1）就翻倍；`dict_force_resize_ratio` 是 `dict.c` 里的常量 4，用作「即使全局禁止 resize 也要扩容」的硬上限。收缩方向是元素数低于桶数的 1/8——`HASHTABLE_MIN_FILL` 在 `src/dict.h` 里定义为 8，旁边注释写着「Minimal hash table fill 12.5%(100/8)」。初始桶数由 `DICT_HT_INITIAL_EXP`（值为 2）决定，即 4 个桶。

**无值模式省内存。** 集合、哈希、有序集合的元素索引这类「只要键不要值」的 dict 用 `no_value` 类型：条目结构退化成 `dictEntryNoValue`（省掉值联合体），桶为空时甚至把键指针直接塞进桶槽、不分配条目——指针最低 3 位被拿来区分「这是键」还是「这是条目」（`encodeEntryKey` / `entryIsKey`）。

本页只讲插入、查找与 rehash 的推进。为遍历提供稳定游标与安全迭代的部分在 e02，键空间语义在 C 组。

## 怎么实现的

**插入：`dictAdd` → `dictAddRaw` → `dictFindLinkForInsert` + `dictInsertKeyAtLink`。** `dictAdd` 本体只有几行：调 `dictAddRaw(d, key, NULL)`，失败返回 `DICT_ERR`；成功则在 `no_value` 为假时用 `dictSetVal` 写值。`dictAddRaw` 把工作拆成两段——先 `dictFindLinkForInsert` 找位置，再 `dictInsertKeyAtLink` 落盘。

`dictFindLinkForInsert` 的动作顺序是：算 hash → `_dictRehashStepIfNeeded(d, idx)`（顺手推进一格 rehash）→ `_dictExpandIfNeeded(d)`（可能触发扩容）→ 在两张表里找这个键。找到就返回 NULL 并把已有条目填进 `*existing`；没找到就返回**目标桶的地址**，源码注释写明「If we are in the process of rehashing the hash table, the bucket is always returned in the context of the second (new) hash table.」

`dictInsertKeyAtLink` 断言传进来的桶确实属于当前该用的那张表（`htidx = dictIsRehashing(d) ? 1 : 0`，断言用 `DICTHT_SIZE_MASK` 卡住上界），然后二选一：`no_value` 且桶为空时用 `encodeEntryKey` 把键直接放进桶槽（不分配条目），否则 `zmalloc(sizeof(*entry))` 分配条目；最后 `entry->next = *bucket`、`*bucket = entry`、`ht_used[htidx]++`。

**查找：`dictFind` → `dictFindLink` → `dictFindLinkInternal`。** `dictFind` 只做一件事：调 `dictFindLink(d, key, NULL)`，拿到链指针就解引用返回条目。`dictFindLink` 先判空（`dictSize(d) == 0` 直接返回 NULL），否则调 `dictFindLinkInternal` 并带上 hash 与可选的桶回填指针。

`dictFindLinkInternal` 的关键动作是 `_dictRehashStepIfNeeded(d, idx)` 推进 rehash；然后按 `tables = dictIsRehashing(d) ? 2 : 1` 决定查几张表。**在表 0 上，如果 `idx < rehashidx` 就直接跳过**——这些桶已经搬走，元素都在表 1 里。比较时先做指针相等判断（`key == visitedKey`），再调 dictType 的 `keyCompare`（未设置时用默认的 `dictDefaultCompare`）。

`_dictRehashStepIfNeeded` 有一处偏向缓存的细节：若刚访问的下标 `visitedIdx >= rehashidx` 且表 0 的该桶非空，就只搬这一个桶（`_dictBucketRehash`，注释说这是「being more CPU cache friendly」）；否则退回按 `rehashidx` 顺序搬（注释说这种路径「not CPU cache friendly」）。

**渐进 rehash：`dictRehash`。** `dictRehash(d, n)` 一次最多搬 n 个桶。函数开头的 `empty_visits = n*10` 是关键约束——源码注释写明：表里可能有大片空桶，「it will visit at max N*10 empty buckets in total, otherwise the amount of work it does would be unbound and the function may block for a long time」。也就是说预算按**桶数**计而不是按元素数计，遇到稀疏表会提前放弃。

两个全局闸门。`dict_can_resize` 是原子变量，三态 `DICT_RESIZE_ENABLE` / `DICT_RESIZE_AVOID` / `DICT_RESIZE_FORBID`；`FORBID` 直接返回 0。`AVOID` 时，扩容方向若 `s1 < 4*s0` 就跳过，收缩方向若 `s0 < 8*4*s1`（即 1/32）也跳过——源码注释把这两个阈值写成 `dict_force_resize_ratio` 与 `1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio)`。

搬运由 `rehashEntriesInBucketAtIndex` 完成：把整条链的节点逐个摘下来接到新表的桶头。桶号计算分两种情况——扩容时用新掩码重算 hash，收缩时直接用旧桶号掩到新掩码，源码注释解释：两个表大小都是 2 的幂，所以在小表上掩一下就是小表的桶号。每搬一个元素，`ht_used[0]--`、`ht_used[1]++`。

搬完由 `dictCheckRehashingCompleted` 收尾：`ht_used[0]` 归零时释放旧表、把表 1 的三元组复制到表 0、把表 1 重置、`rehashidx = -1`。

**rehash 由谁推进，三条路径。**

1. **读写时顺带推进。** `_dictRehashStepIfNeeded` 被 `dictFindLinkInternal`（查找）、`dictGenericDelete`（删除）、`dictFindLinkForInsert`（插入定位）调用；`_dictRehashStep` 另外被 `dictGetRandomKey` 与 `dictGetSomeKeys` 调用。
2. **后台按时间预算推进。** `dictRehashMicroseconds(d, us)` 反复调 `dictRehash(d, 100)` 直到时间用完；它开头检查 `d->pauserehash > 0`，非零就直接返回 0。
3. **扩容时整体重建。** `_dictResize` 里若 dictType 带 `force_full_rehash`，就 `while (dictRehash(d, 1000))` 把整表重排一遍。

**真实 caller。**

- `dictSetResizeEnabled` 在真实代码里的调用方是 `src/server.c` 的 `updateDictResizePolicy`：在 fork 子进程里置 `DICT_RESIZE_FORBID`，有活跃子进程时置 `DICT_RESIZE_AVOID`，否则置 `DICT_RESIZE_ENABLE`。该函数注释写明理由——有子进程时不要搬动大量内存页，「otherwise when a resize happens lots of memory pages are copied」，即规避写时复制。
- `dictRehashMicroseconds` 在 `src/dict.c` 之外的真实调用方是 `src/kvstore.c` 的 `kvstoreIncrementallyRehash`，它按 `threshold_us` 时间预算推进键空间里各个字典的 rehash，函数注释写明「Our hash table implementation performs rehashing incrementally while we write/read from the hash table. Still if the server is idle, the hash table will use two tables for a long time.」
- `force_full_rehash` 在真实代码里只有 `src/server.c` 的 `commandTableDictType` 打开，注释写「force full rehashing」——命令表扩容时整表重排。
- 键空间还多一道内存闸门：`dbDictType` 的 `resizeAllowed` 回调是 `dictResizeAllowed`，它会调 `overMaxmemoryAfterAlloc` 判断内存压力，所以内存吃紧时键空间会拒绝扩容。

**退化与回退路径。**

- **rehash 被暂停时读写照常。** `d->pauserehash` 由 `src/dict.h` 的 `dictPauseRehashing` / `dictResumeRehashing` 宏加减。`_dictRehashStepIfNeeded` 见到 `pauserehash != 0` 就直接返回，两张表继续并存——查找本来就查两张表，所以**暂停 rehash 不影响正确性，只影响搬迁速度**。
- **resize 被禁时仍有硬上限。** `dict_can_resize` 为 `AVOID` 时，`dictExpandIfNeeded` 仍允许在 `ht_used[0] >= dict_force_resize_ratio * DICTHT_SIZE(...)`（4 倍）时扩容，源码注释写明「A hash table is still allowed to expand if the ratio between the number of elements and the buckets >= dict_force_resize_ratio」。
- **插入位置失效有兜底。** `dictSetKeyAtLink` 的注释警告：拿到 `link` 之后到提交之间若表被扩容重分配，`link` 会失效；函数自己先存下两张表的 `ht_size_exp` 快照，事后发现尺寸变了就重新查一次桶。
- **扩容失败有出参。** `_dictResize` 有 `malloc_failed` 出参，`dictTryExpand` 把分配失败映射成 `DICT_ERR`；而 `dictExpand` / `dictShrink` 在尺寸非法时直接返回 `DICT_ERR`，不改动表。

## 关键取舍

**源码事实：** 桶数恒为 2 的幂，定位是一次位与；冲突用链式，新元素挂桶头。**源码事实：** rehash 期间新写只进表 1，查找要查两张表，表 0 上 `idx < rehashidx` 的桶被跳过。**源码事实：** 搬桶预算按桶数计，每次最多访问 `n*10` 个空桶。**源码事实：** 扩容阈值 1:1、收缩阈值 1:8、强制扩容上限 4 倍，初始 4 桶。**源码事实：** resize 的三态闸门是进程级全局变量，由 `src/server.c` 的 `updateDictResizePolicy` 按 fork 子进程 / 活跃子进程切换。**源码事实：** `no_value` 类型的 dict 省掉值联合体，桶空时键指针直接入槽。

**架构推论：** 把扩容摊成「每次读写搬一个桶」，换来的是**任何时刻都不会出现一次长暂停**；代价是 rehash 期间内存占用翻倍、且同一批键的查找要比平时多查一张表。**架构推论：** 用「rehash 期间双写表 1」而不是「原地搬迁」，让插入路径不必判断目标桶属于哪张表（`dictInsertKeyAtLink` 用一个断言把这条不变式钉住），代价是 `_dictRehashStepIfNeeded` 这类函数必须处处记得「表 0 的旧桶已经空了」这条不变式——一旦漏掉就是静默丢键。**架构推论：** 找预算按桶数而不是元素数，是为了在稀疏表上止损；代价是负载因子很低时 rehash 会被拖得很慢（每次只推进一步却撞上大片空桶）。**架构推论：** 用进程级全局开关而不是「按 dict 粒度」的 resize 策略，是因为 Redis 的 fork 子进程共享整份地址空间，一个全局开关就足以保护写时复制；代价是粒度太粗，无法针对某个 dict 单独决策。主核是多写者引擎，若照抄这套「全局闸门 + 双表」，必须先想清楚并发写者下双表如何加锁——Redis 靠单线程事件循环天然免掉了这个问题。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/dict.c` | `dictAdd` | 插入入口：委托 `dictAddRaw` 定位并落盘，成功后写入值 |
| `src/dict.c` | `dictFind` | 查找入口：委托 `dictFindLink` 在两张表中定位键 |
| `src/dict.c` | `dictRehash` | 渐进 rehash 的推进器：按桶逐个搬迁，受 resize 闸门与空桶预算约束 |

## 参考

- [Redis 源码：dict.c](https://github.com/redis/redis/blob/unstable/src/dict.c)
- [Redis 源码：dict.h](https://github.com/redis/redis/blob/unstable/src/dict.h)
- [Redis 文档：键空间遍历](https://redis.io/docs/latest/develop/use/keyspace/)
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/)

## 与主核 / AI 的关系

「渐进 rehash」这件事本身主核大概率不需要——主核是多写者引擎，哈希表扩容要面对并发写者，而 Redis 这套双表 + 免锁假设建立在单线程事件循环之上，直接照搬会立刻出现「两个线程同时往同一张新表里插」的竞态。主核真正值得带走的是**「扩容在内存压力下要被拒绝」这条闸门**：`dictResizeAllowed` 把 `overMaxmemoryAfterAlloc` 接到扩容决策上，等于承认「扩容本身要吃内存」，因此在高水位时宁可让负载因子变差也不扩表。

另一条可借鉴的是 `no_value` 这种**按用途裁剪条目结构**的做法：主核的向量索引里，「字段名到向量的映射」与「主键到整条记录的映射」是两种负载，用同一套条目结构会白付值指针的空间。AI 侧若用 Redis 存 embedding，dict 的 rehash 期间内存会短暂翻倍且查找变慢，这会体现为批量写入时的延迟尖峰——若 embedding 召回对延迟敏感，写入侧与读取侧最好不要共用同一个 keyspace。
