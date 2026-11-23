---
project: redis
feature: robj-refcount
page_type: feature
schema_version: 1
refs:
  - path: src/object.c
    symbols: [incrRefCount, decrRefCount, createStringObject]
---

# 对象与引用计数

## 一句话职责

Redis 用一个 22 位引用计数标记一个值对象被谁持有；计数归零即销毁对象，而「共享对象」与「栈上对象」用两个保留值把这两种非所有权语义编码进同一个字段。

## 机制是什么

`robj`（`struct redisObject`，定义在 `src/object.h`）是 Redis 里所有值的统一外壳：`type:4`、`encoding:5`、`refcount:22`、`iskvobj:1`、`metabits:8`、`lru:24` 几个位域加一个 `void *ptr`。`type` 说明它是哪种逻辑类型，`encoding` 说明它内部用哪种物理表示，`ptr` 指向底层结构。`metabits` 与 `lru` 只在 `iskvobj` 为 1 时有意义——那时这个 `robj` 就是键空间里的 `kvobj`，键的 sds 被嵌在外壳之后；`src/object.h` 的注释写明「metabits and lru are Relevant only when iskvobj is set」。

引用计数是这一层唯一的「持有权」凭证：谁想留着这个对象，谁就 `incrRefCount`；用完必须 `decrRefCount`，减到 0 就地释放。计数不是普通整数，`src/object.h` 留了两个保留值：

- `OBJ_SHARED_REFCOUNT` = `(1 << OBJ_REFCOUNT_BITS) - 1`，`OBJ_REFCOUNT_BITS` 是 22，即 4194303，注释写的是「Global object never destroyed」。
- `OBJ_STATIC_REFCOUNT` = `(1 << OBJ_REFCOUNT_BITS) - 2`，即 4194302，注释是「Object allocated in the stack」。
- `OBJ_FIRST_SPECIAL_REFCOUNT` 就等于 `OBJ_STATIC_REFCOUNT`。

这两个保留值的意义在于：**共享对象的计数是「不可变」的**，增和减都是空操作，因此多个线程可以不加锁地读同一个共享对象；**栈上对象的计数是「只读标记」**，对它做引用计数操作属于编程错误，调用方必须显式绕开。

本页只讲计数本身的增减规则与字符串对象的创建形态。按类型释放底层结构是另一件事（d03），编码如何选择与切换也是另一件事（d02）。

## 怎么实现的

**增。** `incrRefCount` 分三层：正常计数只有在 `o->refcount < OBJ_FIRST_SPECIAL_REFCOUNT - 1` 时才 `++`；等于 `OBJ_SHARED_REFCOUNT` 时走注释里写的「Nothing to do: this refcount is immutable」；等于 `OBJ_STATIC_REFCOUNT` 时 `serverPanic("You tried to retain an object allocated in the stack")`；其余落到 `serverPanic("You tried to retain an object with maximum refcount")`。也就是说普通对象的计数最高只能涨到 `OBJ_FIRST_SPECIAL_REFCOUNT - 1`，再往上就是越界。

**减。** `decrRefCount` 依次做这些事：

1. 计数等于 `OBJ_SHARED_REFCOUNT` 就直接 `return`——共享对象永远不会被销毁。
2. 计数 `<= 0` 走 `serverPanic("illegal decrRefCount for object with: type %u, encoding %u, refcount %d")`，把类型、编码、计数一起打出来。
3. 计数减一；只有减到 0 才进入释放。
4. 释放先试一条 EMBSTR 快路径：`o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_EMBSTR && !o->iskvobj` 时，`serverAssert(sdsType(o->ptr) == SDS_TYPE_8)`（断言旁的注释写明「embstr always type_8」），然后 `zfree_with_size(o, sizeof(robj) + sdsAllocSize(o->ptr))` 一次性归还。源码注释写明这条快路径的目的：「no inner allocation to free, and we can compute the alloc size to hint jemalloc for a faster deallocation」。
5. 非快路径下，`o->ptr != NULL` 时按 `o->type` 分派到 `freeStringObject` / `freeListObject` / `freeSetObject` / `freeZsetObject` / `freeHashObject` / `freeModuleObject` / `freeStreamObject` / `freeGCRAObject`（`ENABLE_GCRA` 编译开关下）/ `freeArrayObject` / `bitroarFree`，默认 `serverPanic("Unknown object type")`。分派前还要处理 `iskvobj` 情形：`alloc = kvobjGetAllocPtr(o)` 取到真实分配指针，若 `getModuleMetaBits(o->metabits)` 非零还要先 `keyMetaOnFree((kvobj *)o)` 摘掉模块元数据。最后 `zfree(alloc)` 归还外壳本身。

**共享对象的持有约定。** `src/object.c` 的 `makeObjectShared` 把 `refcount` 置成 `OBJ_SHARED_REFCOUNT`；它上方的注释给出理由：共享对象「can be shared everywhere in the "object space" of Redis」，计数不可变，于是「free to access shared objects such as small integers from different threads without any mutex」。`src/server.c` 的 `createSharedObjects` 用它造了 `OBJ_SHARED_INTEGERS` 个共享小整数（`OBJ_SHARED_INTEGERS` 在 `src/server.h` 里是 10000），`src/object.c` 的 `createStringObjectFromLongLongWithOptions` 在值落在 `[0, OBJ_SHARED_INTEGERS)` 且未被禁用共享时，直接返回 `shared.integers[value]`。

**栈上对象。** `src/server.h` 的 `initStaticStringObject` 宏把栈上对象的 `refcount` 置为 `OBJ_STATIC_REFCOUNT`。`src/server.c` 的 `dictEncObjKeyCompare` 是这条约定的真实 caller，它的注释写明「Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without good reasons, because it would incrRefCount() the object, which is invalid」，于是它在调用 `getDecodedObject` 前后都先判 `o->refcount != OBJ_STATIC_REFCOUNT`，栈上对象就完全绕开了引用计数路径。

**字符串对象的两种形态。** `createStringObject` 按长度二选一：长度不超过 `OBJ_ENCODING_EMBSTR_SIZE_LIMIT`（在 `src/object.c` 里定义为 44）走 `createEmbeddedStringObject`，否则走 `createRawStringObject`。函数上方的注释写明「The current limit of 44 is chosen so that the biggest string object we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc」。两者的差别在内存布局：`createEmbeddedStringObject` 用 `zmalloc_usable(sizeof(robj) + val_sds_size)` 一次分配，外壳后面紧跟一个 SDS_TYPE_8 的 sds，外壳与字符串在同一块内存里；`createRawStringObject` 则是 `createObject(OBJ_STRING, sdsnewlen(ptr, len))`，外壳与 sds 是两块独立分配。

**退化路径。** 引用计数层有三条「什么都不做」的分支，都必须点名真实 caller：

- **共享对象增减皆空操作。** 真实 caller 是 `src/object.c` 的 `getDecodedObject`——它在 `sdsEncodedObject(o)` 为真时无条件 `incrRefCount(o)` 并把**原指针**返回（`sdsEncodedObject` 是 `src/server.h` 里的宏，判的是编码为 RAW 或 EMBSTR）。这条路径对共享对象是安全的：计数不变，调用方随后 `decrRefCount` 也不释放。
- **栈上对象增减皆非法。** 真实 caller 是上面提到的 `src/server.c` 的 `dictEncObjKeyCompare`，它显式跳过。
- **EMBSTR 免分派释放。** 真实 caller 是 `src/object.c` 的 `decrRefCount` 自身——计数归零时 EMBSTR 字符串不会走到 `freeStringObject`，而是直接 `zfree_with_size` 一次性归还；RAW 字符串才需要 `freeStringObject` 去 `sdsfree` 那块独立的 sds。

## 关键取舍

**源码事实：** 引用计数是 22 位位域，普通对象的计数上限是 `OBJ_FIRST_SPECIAL_REFCOUNT - 1`，越界即 panic。**源码事实：** 共享对象的计数不可变，增减都是空操作，因此共享对象永不销毁。**源码事实：** 栈上对象的计数是错误标记，对它做增减都会 panic。**源码事实：** EMBSTR 的释放有专门快路径，跳过了按类型的分派。**源码事实：** `createStringObject` 的 44 字节阈值来自 jemalloc 的 64 字节 arena。

**架构推论：** 用两个保留值把「共享」与「栈上」这两种非所有权语义塞进同一个计数字段，省掉了额外标志位与额外分支，代价是这两个值不能再当普通计数用、正常计数上限被压到 4194301。**架构推论：** 共享对象「计数不可变」把并发读的安全性做进了数据表示本身——读线程不需要任何同步，因为计数根本不会被写；代价是共享对象永远不可改写，所以只能承载回复串、小整数这类不可变负载。**架构推论：** EMBSTR 把外壳与 sds 合成一次分配，收益是省一次分配、少一次 cache miss，代价是 sds 一旦需要扩容就要搬迁（sds 自身用预分配吸收小增长）。**架构推论：** 「计数归零即销毁」让生命周期管理成为线性成本，但 Redis 的值对象几乎不跨线程共享，计数争用极小；主核是多写者引擎，若照搬这套「单计数器 + 保留值」的约定，必须另外解决并发增减的原子性。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/object.c` | `incrRefCount` | 引用计数递增，含共享对象、栈上对象、计数越界三条特殊分支 |
| `src/object.c` | `decrRefCount` | 引用计数递减，归零时按类型释放，含 EMBSTR 快速释放路径 |
| `src/object.c` | `createStringObject` | 按长度在 EMBSTR 与 RAW 两种形态之间创建字符串对象 |

## 参考

- [Redis 源码：object.c](https://github.com/redis/redis/blob/unstable/src/object.c)
- [Redis 源码：object.h](https://github.com/redis/redis/blob/unstable/src/object.h)
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/)
- [Redis 文档：OBJECT 命令](https://redis.io/docs/latest/commands/object/)

## 与主核 / AI 的关系

Redis 这套「单一计数 + 两个保留值」的持有权约定可以直接借鉴的是**语义部分**：主核若要给句柄（表、索引、页、事务上下文）做生命周期管理，同样需要区分「我拥有」「别人拥有」「我只是在读一个不该被改的共享结构」这三态，并让越权操作立刻失败而不是静默通过。不能照搬的是**实现部分**：Redis 的计数增减发生在单线程主循环里，天然不需要原子操作；主核是多写者引擎，同一个句柄的引用计数会被多个写者同时改动，主核必须用原子增减或读侧引用保护（epoch/RCU 之类）来替代裸计数，「计数不可变即线程安全」这条推论在主核不成立。AI 侧若把检索结果对象挂在缓存里并由推理线程异步读取，引用计数的判定与释放时机就要按主核自己的并发模型重新定义，不能假设读侧只是「看一眼」。
