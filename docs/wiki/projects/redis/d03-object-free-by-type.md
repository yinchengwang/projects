---
project: redis
feature: object-free-by-type
page_type: feature
schema_version: 1
refs:
  - path: src/object.c
    symbols: [freeStringObject, freeListObject, freeHashObject, freeSetObject, freeZsetObject]
---

# 按类型释放值对象

## 一句话职责

这五个 `free*Object` 函数是「`robj` 外壳」与「底层结构」之间的桥：按对象的实际编码挑出正确的底层释放函数，外壳本身留给引用计数层归还。

## 机制是什么

一个 `robj` 的内存分两块：外壳本身（`sizeof(robj)`），以及 `ptr` 指向的底层结构。释放必须分两步——先按编码释放底层结构，再归还外壳。第二步由 `decrRefCount` 统一完成（那是 d01 的事），第一步就是本页这五个函数干的。

五个函数按逻辑类型分：`freeStringObject`（字符串）、`freeListObject`（列表）、`freeSetObject`（集合）、`freeZsetObject`（有序集合）、`freeHashObject`（哈希）。它们共同遵守两条约定：

- **只释放底层结构，不碰外壳。** 外壳的归还路径只有一条，集中在引用计数层。
- **必须按当前编码分支。** 同一个逻辑类型有多种物理表示，释放方式各不相同；用错编码释放轻则泄漏，重则读到已释放内存。

有些类型的释放还要连带处理「登记在全局结构里的元数据」——哈希的字段级过期时间就是这样，它挂在底层 dict 的元数据上，释放前必须确认它已经被标记为垃圾。

## 怎么实现的

**`freeStringObject`。** 函数体只有一句条件：`if (o->encoding == OBJ_ENCODING_RAW) sdsfree(o->ptr);`。EMBSTR 的 sds 与外壳在**同一块内存**里，没有独立可释放的堆块；INT 编码的值直接存在 `ptr` 槽里，更没有堆块。所以这两种编码下这个函数什么都不做，外壳由 `decrRefCount` 一次性 `zfree` 掉。

**`freeListObject`。** 按编码二分：`OBJ_ENCODING_QUICKLIST` → `quicklistRelease(o->ptr)`；`OBJ_ENCODING_LISTPACK` → `lpFree(o->ptr)`；其它编码 `serverPanic("Unknown list encoding type")`。

**`freeSetObject`。** 函数体只有一句 `setTypeFree(o)`。真正的分派在 `src/t_set.c`：`setTypeFree` 取 `setTypeGetOps(o->encoding)->free(o)`——集合的四种编码（INTSET / LISTPACK / LISTPACK_EX / HT）各自在 `setTypeOps` 表里注册了释放函数。`src/t_set.c` 的注释写明它「Frees the internals of a set object (but not the robj itself). Called by freeSetObject() in object.c」。

**`freeZsetObject`。** 三种编码三种释放：`OBJ_ENCODING_SKIPLIST` → 取 `zs = o->ptr`，然后 `dictRelease(zs->dict)` 释放「成员到分数」的字典、`zslFree(zs->zsl)` 释放跳表、`zfree(zs)` 释放 zset 自己的外壳；`OBJ_ENCODING_BTREE` → `zbtreeFree(o->ptr)`；`OBJ_ENCODING_LISTPACK` → `zfree(o->ptr)`；默认 `serverPanic("Unknown sorted set encoding")`。

**`freeHashObject`。** 函数体只有一句 `hashTypeFree(o)`。分派在 `src/t_hash.c` 的 `hashTypeFree`：

- `OBJ_ENCODING_HT` → 释放前先判 `isDictWithMetaHFE((dict*)o->ptr)`，若该哈希登记过字段级过期时间，就断言 `htGetMetadataEx((dict*)o->ptr)->expireMeta.trash == 1`（含义是「它已经被标记为垃圾」），然后 `dictRelease`。
- `OBJ_ENCODING_LISTPACK` → `lpFree(o->ptr)`。
- `OBJ_ENCODING_LISTPACK_EX` → 断言 `((listpackEx *) o->ptr)->meta.trash == 1` 后 `listpackExFree(o->ptr)`。
- `OBJ_ENCODING_TMPL_LP` → `hashTemplateLpFree(o->ptr)`；`OBJ_ENCODING_TMPL_ARRAY` → `hashTemplateArrayFree(o->ptr)`。
- 默认 `serverPanic("Unknown hash encoding type")`。

**唯一的调用者。** 这五个函数在 `src/object.c` 之外没有别的调用点：全仓搜下来，调用方只有 `decrRefCount` 里那个按 `o->type` 的 switch（`OBJ_STRING` → `freeStringObject`、`OBJ_LIST` → `freeListObject`、`OBJ_SET` → `freeSetObject`、`OBJ_ZSET` → `freeZsetObject`、`OBJ_HASH` → `freeHashObject`）。也就是说，**引用计数归零是进入按类型释放的唯一入口**，没有旁路。

**退化与回退路径。**

- **字符串在 EMBSTR / INT 编码下没有可释放的底层结构。** 真实 caller 是 `src/object.c` 的 `decrRefCount`：EMBSTR 走 `zfree_with_size(o, sizeof(robj) + sdsAllocSize(o->ptr))` 快路径，直接跳过 `freeStringObject`；INT 编码走普通路径调用 `freeStringObject`，而该函数因编码不是 RAW 而什么都不做。
- **未知编码直接 panic，不静默放过。** 真实 caller 是 `decrRefCount` 的 switch：一旦 `o->type` 与 `o->encoding` 对不上（例如编码被改坏），`freeListObject` / `freeZsetObject` / `hashTypeFree` 会 `serverPanic`。
- **集合与哈希的释放要多走一层。** 真实 caller 仍是 `decrRefCount`：`freeSetObject` → `setTypeFree` → `setTypeGetOps(...)->free(o)`，`freeHashObject` → `hashTypeFree` → 按编码的 switch。
- **旁路：只归还内存、不逐个析构。** `src/object.c` 里与这五个函数一一对应的还有 `dismissStringObject` / `dismissListObject` / `dismissSetObject` / `dismissZsetObject` / `dismissHashObject`，它们走的是 `dismissMemory` 而不是 `free`，效果是把内存还给分配器以便操作系统回收。统一入口 `dismissObject` 的注释写明它为什么有条件地逐个归还：「Because of the cost of iterating all node/field/member/entry of complex data types, we iterate and dismiss them only when approximate average we estimate the size of an individual allocation is more than a page size of OS」——平均单元不超过一个内存页时，只交出顶层结构，不进去遍历。

**边界说明。** 这些函数调用的是 `sdsfree` / `lpFree` / `quicklistRelease` / `dictRelease` / `zslFree` / `zfree` 等，最终落到 `zmalloc` / `zfree`；分配器统计与 RSS 记账属于 e07，本页不展开。

## 关键取舍

**源码事实：** 五个 free 函数只释放底层结构，外壳一律由 `decrRefCount` 归还。**源码事实：** 释放必须按当前编码分支，未知编码一律 panic。**源码事实：** 集合与哈希把编码分派下沉到各自的文件（`src/t_set.c` / `src/t_hash.c`），`src/object.c` 里只剩一行转发。**源码事实：** 哈希释放前要断言字段级过期元数据已被标记为 trash。**源码事实：** `freeStringObject` 对 EMBSTR 与 INT 编码不做任何释放。

**架构推论：** 把「按编码分派」下沉到类型文件、对象层只保留「类型名 → 释放函数」的映射，好处是新增编码时不必改对象层；代价是多了一层调用，`src/object.c` 里的 `freeSetObject` / `freeHashObject` 已经薄到只剩一行。**架构推论：** 未知编码选择 panic 而不是静默跳过，是把「内存状态被改坏」这类问题在第一时间炸出来，而不是带着可疑状态继续跑；代价是任何编码改动若漏改释放函数，都会立刻变成服务不可用而不是慢慢泄漏。**架构推论：** 字符串释放要区分 RAW 与 EMBSTR，根源是 EMBSTR 把两块内存合成了一块；这条差异把心智负担推给了上层——拷贝路径必须自己判断编码，编码判断因此散落在多处。**架构推论：** 「dismiss」与「free」分成两套并行实现，可以让后台删除只归还内存而不逐个析构，代价是同一套类型分派被写了两遍，两边不同步就会产生行为差异（例如 dismiss 路径覆盖的编码集合与 free 路径不一致）。主核若要在多写者下做到同样的效果，更可能选择单一析构路径 + 引用计数归零触发，而不是维护两套并行的类型分派。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/object.c` | `freeStringObject` | 释放字符串的底层 sds；只有 RAW 编码才有独立堆块可释放 |
| `src/object.c` | `freeListObject` | 按 QUICKLIST / LISTPACK 编码释放列表的底层结构 |
| `src/object.c` | `freeHashObject` | 转发给 `src/t_hash.c` 的 `hashTypeFree`，由后者按哈希编码释放 |
| `src/object.c` | `freeSetObject` | 转发给 `src/t_set.c` 的 `setTypeFree`，由后者按集合编码释放 |
| `src/object.c` | `freeZsetObject` | 按 SKIPLIST / BTREE / LISTPACK 编码释放有序集合的底层结构 |

## 参考

- [Redis 源码：object.c](https://github.com/redis/redis/blob/unstable/src/object.c)
- [Redis 源码：t_hash.c](https://github.com/redis/redis/blob/unstable/src/t_hash.c)
- [Redis 源码：t_set.c](https://github.com/redis/redis/blob/unstable/src/t_set.c)
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/)

## 与主核 / AI 的关系

主核若采用「一份逻辑值对应多种物理表示」的设计，释放路径就必须像 Redis 一样**按当前表示分派**，而不能按逻辑类型分派——否则就会重演「用 listpack 的方式释放一个 quicklist」这类错误。Redis 这套做法里最值得抄的是**把编码分派下沉到拥有该编码的类型文件**，主核的多模态列（向量列、倒排列、位图列）可以照此把各自的析构逻辑收在列自己的实现里，对象/元数据层只维护「列类型 → 析构入口」的映射。反过来，主核不能照抄的是**用进程 panic 处理编码不匹配**：Redis 是单线程，panic 等于快速失败；主核是多写者引擎，编码状态被改坏时更可能表现为读到了半析构的对象，需要靠校验与断言把问题定位到写入点，而不是让整个引擎退出。AI 侧若把模型输出、向量缓存挂在同一套对象层上，释放时的编码判断必须按主核自己的并发模型重新确认，尤其是「引用计数归零」这个唯一入口在主核里不能照搬成简单的计数相等。
