---
project: redis
feature: set-type
page_type: feature
schema_version: 1
refs:
  - path: src/t_set.c
    symbols: [setTypeAdd, setTypeRemove, setTypeIsMember, setTypeSize, setTypeMaybeConvert, setTypeRandomElement]
---

# 集合类型

## 一句话职责

`src/t_set.c` 负责集合类型的成员级语义：加成员、删成员、判成员、算基数、随机取元素，并在集合变大时决定是否放弃紧凑编码换成哈希表。

## 机制是什么

集合是**无序、去重**的成员容器。内部有四种表示：**intset**（全是整数时的排序数组）、**listpack**（普通紧凑数组）、**listpack_ex**（带成员级过期时间的紧凑数组）、**HT**（哈希表）。

命令层对外只有一套语义，但内部编码差异很大。Redis 用一张**操作表**（`setTypeOps`）把这层差异隔开：每个编码注册一组函数指针（`rawAdd`/`rawRemove`/`isMember`/`size`/`randomElement`/`iterInit`/`iterNext`/…），`setTypeGetOps(set->encoding)` 按当前编码取出对应的那张表，所有集合 API 都经它分派。

整数集合内部的宽度升级算法（16 位 → 32 位 → 64 位）属于 D 组（d04 页）；哈希表本身的实现属于 E 组（e01 页）。本页讲的是**集合命令层**的行为：成员操作语义，以及「什么时候该换编码」。

## 怎么实现的

**加成员：`setTypeAdd` → `setTypeAddInline` → `setTypeAddAuxInline`。** 三层是同一套逻辑的包装：`setTypeAdd` 收 sds 字符串，`setTypeAddAuxInline` 收「字符串+长度」或「裸整数」，`setTypeAdd` 只是把 `sdslen` 填进去。

`setTypeAddAuxInline` 先处理**裸整数输入**：只有 intset 能直接吃整数，其它编码必须先把整数 `ll2string` 成 32 字节的栈缓冲 `tmpbuf` 再当字符串用（源码注释写明「Only intset can consume a bare integer directly」）。然后调 `ops->rawAdd(...)`，返回值有三种含义：

- 返回 **1**：加进去了。
- 返回 **0**：成员已存在，什么也没做。
- 返回 **-1**：**当前编码装不下这个值**。此时 `serverAssert(str)` 保证手里有字符串形态（否则无从转换），然后 `setTypeConvertAndExpand(set, target_enc, ops->size(set) + 1, 1)` 换成目标编码，再用新编码的 `rawAdd` 重来一次，并断言这次必须成功。

加完之后还有一条 intset 专属的升级检查：若加成功且当前编码是 intset，就调 `maybeConvertIntset(set)`——它比较 `intsetLen(subject->ptr)` 与 `intsetMaxEntries()`，超了就 `setTypeConvert(subject, OBJ_ENCODING_HT)`。`intsetMaxEntries()` 返回 `server.set_max_intset_entries`，但源码里额外压了一道：`if (max_entries >= 1<<30) max_entries = 1<<30;`，注释写明是「limit to 1G entries due to intset internals」。

**删成员：`setTypeRemove` → `setTypeRemoveAux`。** 同样先把裸整数转成字符串（intset 除外），再 `setTypeGetOps(setobj->encoding)->rawRemove(...)` 返回 1/0。

**判成员：`setTypeIsMember` → `setTypeIsMemberAux`。** 有一条**带成员过期时间的集合**的额外分支：`setTypeHasExpireSupport(set)` 为真时，先用 `ops->getExpire` 取该成员的过期时间——取不到（成员不存在）直接返回 0；取到了就返回 `!setTypeExpireTimeElapsed(expire)`，也就是**逻辑上已过期的成员视为不是成员，但这里不删除它**。

`setTypeExpireTimeElapsed` 自己也有豁免条件，源码注释写得很细：处于 AOF 重放（`server.loading`）或当前命令来自主客户端（`CLIENT_MASTER`，涵盖 ASM 导入）时，一律不按本地时钟判过期——注释给的理由是这些路径操作的是主库已经决定过的状态，主库对每个过期成员都有显式的 SREM，若副本按自己的时钟判断就会与主库分叉。

**基数：`setTypeSize`。** 一行：`setTypeGetOps(subject->encoding)->size(subject)`。

**随机取元素：`setTypeRandomElement`。** 同样是 `ops->randomElement(...)` 分派，返回值是当前编码，由调用方决定把 `str` 还是 `llele` 当成结果读。相邻的 `setTypePopRandom` 对 listpack 有一份专门实现：用 `lpNextRandom` 随机定位并直接删除，避免重新查找。

**编码切换判据：`setTypeMaybeConvert(set, size_hint)`。** 这是集合层的「该不该换编码」决策点，规则很短：

- 集合是 listpack 或 listpack_ex 且 `size_hint > server.set_max_listpack_entries` → 转 HT。
- 集合是 intset 且 `size_hint > server.set_max_intset_entries` → 转 HT。

两条规则都只往 HT 转，**没有从 HT 转回紧凑编码的路径**。它的唯一调用者是 `saddCommand`：键已存在时用 `c->argc - 2`（本次要加的成员数）作为 size_hint；键不存在时走 `setTypeCreate` 另有一套选编码的逻辑。

`setTypeCreate(value, size_hint)` 的选法是：值能表示成整数且 `size_hint <= server.set_max_intset_entries` → intset；否则 `size_hint <= server.set_max_listpack_entries` → listpack；否则建哈希表并 `dictExpand(o->ptr, size_hint)` 预扩容。源码注释承认提示可能不准：「We may oversize the set by using the hint if the hint is not accurate, but we will assume this is acceptable to maximize performance」。

`setTypeConvertAndExpand` 限定目标编码只能是 HT、LISTPACK、LISTPACK_EX 三者之一，并且有一条断言：带成员过期的集合（listpack_ex）**只能转 HT**，不能转回普通 listpack——注释写明「A set with member expirations keeps them: it can only grow to a hashtable」。

两个阈值来自配置项，源码默认值在 `src/config.c`：`set-max-listpack-entries` 默认 **128**，`set-max-intset-entries` 默认 **512**。

## 关键取舍

**源码事实：** 紧凑编码装不下新值时的策略是「换编码再重试」，而不是原地扩容——因为紧凑编码的元素宽度与容器形态在创建时就定死了。**源码事实：** intset 的条目上限除了配置项外还被 `1<<30` 额外压住，注释说明是 intset 内部表示的限制。**源码事实：** 集合只向哈希表单向升级，没有自动降级路径。**源码事实：** 带成员级过期的集合只能往 HT 走，不能退回普通 listpack。**源码事实：** 逻辑过期的成员在判存时算「不存在」但不被删除；真正的清理发生在写路径上——`saddCommand` 开头会调 `setTypeExpireIfNeeded` 扫掉逻辑过期成员并逐个传播 SREM。**源码事实：** `setTypeExpireIfNeededSlow` 在副本、载入中、允许访问过期数据、过期被暂停时都直接返回不扫；它的注释还写明一条已知限制：这些情况下 SPOP/SRANDMEMBER 可能选中逻辑上已过期的成员。

**架构推论：** 用一张 `setTypeOps` 表把「同一语义、五种编码」隔离开，新增一种编码只要补一张表而不必改所有调用点；代价是每次调用多一层间接跳转，热路径上靠 `setTypeAddAuxInline` 上的 `always_inline` 抵消——源码注释明确说 SADD 每个成员都要付一次函数帧、寄存器保存与栈保护器的开销，所以要把内联版本塞进调用者。**架构推论：** 单向升级（不回缩）避免了「集合变大变小反复转换」的抖动，代价是曾经很大的集合即使被删到只剩几个成员，也会一直占着哈希表的内存——这是一个明确的内存换 CPU 的取舍。**架构推论：** 「逻辑过期」与「物理删除」分离，让读路径保持无副作用，回收集中到写路径与后台扫描；代价是采样类命令（SPOP/SRANDMEMBER）在回收没跑之前可能选到逻辑过期成员，源码把这条限制写在了注释里而不是试图在读路径上兜底。**架构推论：** `size_hint` 来自命令的参数个数（`c->argc - 2`），是**乐观估计**——一次 SADD 加 1000 个成员就可能让一个紧凑集合直接升到哈希表，即使其中多数是重复成员；用「预判 + 可能过度升级」换来了插入路径上不必反复检查长度。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/t_set.c` | `setTypeAdd` | 加成员：经 ops 表分派，编码装不下时换编码重试，intset 超限后转哈希表 |
| `src/t_set.c` | `setTypeRemove` | 删成员：经 ops 表分派，返回是否真的删掉了 |
| `src/t_set.c` | `setTypeIsMember` | 判成员：支持成员级过期时把逻辑过期成员算作不存在 |
| `src/t_set.c` | `setTypeSize` | 基数：经 ops 表分派返回元素个数 |
| `src/t_set.c` | `setTypeMaybeConvert` | 集合层的编码升级判据：按 size_hint 与两个阈值决定是否转哈希表 |
| `src/t_set.c` | `setTypeRandomElement` | 随机取元素：经 ops 表分派，返回当前编码由调用方解释 |

## 参考

- [Redis 源码：t_set.c](https://github.com/redis/redis/blob/unstable/src/t_set.c)
- [Redis 文档：集合数据类型](https://redis.io/docs/latest/develop/data-types/sets/)
- [Redis 命令：SADD](https://redis.io/docs/latest/commands/sadd/)
- [Redis 命令：SMEMBERS](https://redis.io/docs/latest/commands/smembers/)

## 与主核 / AI 的关系

Redis 的集合是「精确去重 + 随机取样」的容器，intset 这一层专门为「全整数成员」优化。对 AI 侧最常见的用法——用集合存「已经处理过的文档 id」「已经向量化的 chunk id」——成员天然是整数，intset 编码能用排序数组做到 O(log n) 查找与极致的内存密度，这比任何通用哈希实现都省。

但集合只向 HT 单向升级这条规则对主核有直接教训：主核的「已处理 id 集合」这类结构如果按天累积，Redis 会在某个阈值后一次性把内存占用抬上去且不再回落；主核的多写者引擎如果照搬这种「只升不降」策略，峰值内存会被永久性地钉在最高水位上，更合理的做法是保留一条显式的压缩/归档路径（Redis 自己的做法是在 RDB 载入路径上显式调用转换函数，而不是靠自动降级）。

另外，「逻辑过期与物理删除分离」对主核的 TTL 语义很关键：主核要保证「读到过期即读不到」，但回收可以滞后；Redis 把这条不变量落在 `setTypeExpireTimeElapsed` 的几个豁免条件里（副本、AOF 重放、主客户端命令），主核的多副本场景需要逐条对应设计，否则副本按本地时钟删除会让主从分叉。
