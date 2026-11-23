---
project: redis
feature: object-encoding-transition
page_type: feature
schema_version: 1
refs:
  - path: src/object.c
    symbols: [tryObjectEncoding, getDecodedObject, dupStringObject]
---

# 对象编码与转换

## 一句话职责

字符串对象有 RAW / EMBSTR / INT 三种内部表示，写入时把对象换到最省的形态，读取时把它换回能直接取字节的形态，这就是 Redis 的编码转换。

## 机制是什么

`src/object.h` 里编码号是按位宽从窄到宽排的一组常量：`OBJ_ENCODING_RAW` 0、`OBJ_ENCODING_INT` 1、`OBJ_ENCODING_HT` 2、`OBJ_ENCODING_INTSET` 6、`OBJ_ENCODING_SKIPLIST` 7、`OBJ_ENCODING_EMBSTR` 8、`OBJ_ENCODING_QUICKLIST` 9、`OBJ_ENCODING_LISTPACK` 11。字符串这一类实际只用到前三种：

- **RAW**：外壳 `robj` 加一块独立的 sds 堆内存。
- **EMBSTR**：外壳与 sds 合并成**同一次分配**，sds 固定为 SDS_TYPE_8，长度有上限。
- **INT**：整个值就是一个整数，直接放在 `robj` 的 `ptr` 槽里（指针当整数用），不额外占堆内存。

编码转换有两个相反的方向：**写入方向**是「往省内存的方向换」（`tryObjectEncoding`），**读取方向**是「往好用的方向换」（`getDecodedObject`）。两者都只处理字符串；其他类型（list / set / hash / zset）也有各自的紧凑编码，但那是各类型命令自己管的，不在这条路径上。

本页只讲 `src/object.c` 里这一层的编码决策与解码。各类型的增删语义在 F 组，底层结构（sds、listpack、intset 等）的实现在 E 组。

## 怎么实现的

**写入方向：`tryObjectEncoding`。** 它本身只是 `tryObjectEncodingEx(o, 1)` 的薄包装，真正干活的是后者，顺序是：

1. 断言类型必须是 `OBJ_STRING`（`serverAssertWithInfo(NULL,o,o->type == OBJ_STRING)`）。注释写明「the only type we encode in this function. Other types use encoded memory efficient representations but are handled by the commands implementing the type」。
2. `if (!sdsEncodedObject(o)) return o;` —— 只有 RAW 或 EMBSTR 值得转换（`sdsEncodedObject` 是 `src/server.h` 里的宏，判编码为 RAW 或 EMBSTR）；已经是 INT 的对象原样返回。
3. `if (o->refcount > 1) return o;` —— 注释给出原因：共享对象「can be shared everywhere in the "object space" of Redis and may end in places where they are not handled. We handle them only as values in the keyspace.」
4. **整数化**：`len = sdslen(s)`，`if (len <= 20 && string2l(s,len,&value))`。注释解释「a string larger than 20 chars is not representable as a 32 nor 64 bit integer」。
   - 原编码是 RAW：`sdsfree(o->ptr)` 释放那块独立 sds，把 `o->encoding` 改成 `OBJ_ENCODING_INT`，`o->ptr = (void*) value`，返回**同一个**对象。
   - 原编码是 EMBSTR：不能就地改（EMBSTR 的 sds 与外壳同块内存），于是 `decrRefCount(o)` 释放原对象，返回 `createStringObjectFromLongLongForValue(value)` 新建的 INT 对象。
5. **EMBSTR 化**：`if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)`（`src/object.c` 里定义为 44）且当前不是 EMBSTR，就 `createEmbeddedStringObject(s, sdslen(s))` 建新对象、`decrRefCount(o)` 释放旧对象、返回新对象。注释写明理由：「the object and the SDS string are allocated in the same chunk of memory to save space and cache misses」。
6. **都换不了时**：`if (try_trim) trimStringObjectIfNeeded(o, 0);` 然后返回原对象。`trimStringObjectIfNeeded` 的注释是「Optimize the SDS string inside the string object to require little space, in case there is more than 10% of free space at the end of the SDS」，判定条件为 `sdsavail(o->ptr) > len/10`。

**读取方向：`getDecodedObject`。** 注释写明「Get a decoded version of an encoded object (returned as a new object). If the object is already raw-encoded just increment the ref count.」

- `if (sdsEncodedObject(o)) { incrRefCount(o); return o; }` —— RAW/EMBSTR 不复制，加个引用计数就把原指针返回。
- `if (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_INT)` —— 用 `char buf[32]` 与 `ll2string(buf,32,(long)o->ptr)` 把整数转成十进制文本，再 `createStringObject(buf,strlen(buf))` 生成一个 RAW 对象返回。
- 其余编码 `serverPanic("Unknown encoding type")`。

**复制：`dupStringObject`。** 注释写明它「Duplicate a string object, with the guarantee that the returned object has the same encoding as the original one」，并且「duplicating a small integer object (or a string object that contains a representation of a small integer) will always result in a fresh object that is unshared (refcount == 1)」。

- RAW → `createRawStringObject(o->ptr,sdslen(o->ptr))`；EMBSTR → `createEmbeddedStringObject(o->ptr,sdslen(o->ptr))`；INT → `createObject(OBJ_STRING, NULL)` 后把 `encoding` 置成 INT、`ptr` 从原对象复制；其它编码 `serverPanic("Wrong encoding.")`。

**真实 caller。** `tryObjectEncoding` 的 caller 全在 `src/t_string.c`：`setCommand`、`setnxCommand`、`setexCommand`、`psetexCommand`、`getsetCommand`、`msetGenericCommand`（MSET / MSETNX 共用）、`msetexCommand`、`appendCommand`。共同模式是「先把命令行参数里的值对象换到最省的编码，再交给 `setKey` 写进库」。`getDecodedObject` 的 caller 横跨读写两侧：`src/t_string.c` 的 `setGenericCommand`（`OBJ_SET_IFEQ` 条件比较分支）与 `lcsCommand`、`src/db.c` 的 `dbUnshareStringValueByLink`、`src/bitops.c`、`src/pubsub.c`、`src/rdb.c` 等。`dupStringObject` 的 caller 是 `src/db.c` 的对象复制分支、`src/lazyfree.c`、`src/module.c`、`src/slowlog.c`。

**退化与回退路径。** 转换不是必然发生的，每条不转换的分支都得点名真实 caller：

- **共享对象不转换。** 真实 caller 是 `src/t_string.c` 的 `setCommand`——它对 `c->argv[2]` 调 `tryObjectEncoding`，若该对象被多处共享（`o->refcount > 1`）则原样返回，代价是这次写入拿不到 INT 或 EMBSTR 的内存收益。
- **已是 INT 编码的不再转换。** 真实 caller 同为 `setCommand`：`tryObjectEncoding` 的 `!sdsEncodedObject(o)` 判断让 INT 对象直接返回。
- **长度超阈值的只做裁剪。** 真实 caller 同为 `setCommand`：`len <= 20` 与 `len <= 44` 两条阈值任一不满足，就只剩 `trimStringObjectIfNeeded` 收掉 SDS 尾部 10% 以上的空闲空间。
- **就地改写前必须先解除共享。** 真实 caller 是 `src/db.c` 的 `dbUnshareStringValueByLink`——它判断 `o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW` 时，用 `getDecodedObject` 取出解码形态，再用 `createRawStringObject` 复制成一份独占的 RAW 对象替换回库，最后 `decrRefCount(decoded)` 释放解码副本。

## 关键取舍

**源码事实：** `tryObjectEncoding` 只处理字符串类型，其他类型的紧凑编码由各类型命令自己负责。**源码事实：** 两条阈值写死在源码里——20 个字符（能否塞进整数）与 44 字节（能否塞进 jemalloc 的 64 字节 arena）。**源码事实：** 共享对象被明确排除在转换之外。**源码事实：** `getDecodedObject` 对 RAW/EMBSTR 不复制，只加引用计数；对 INT 则新建一个 RAW 对象。**源码事实：** `tryObjectEncoding` 里没有把 INT 变回 RAW/EMBSTR 的分支，INT 变回 RAW 形态只发生在写就地的路径上（`src/db.c` 的 `dbUnshareStringValueByLink`）。

**架构推论：** 编码选择放在「解析完命令行参数、还没写库」的那一刻，而不是「写进库的那一刻」，是因为此刻参数对象还没有被数据库以外的东西持有，转换代价最低、也最不容易踩到共享。**架构推论：** 用「解码返回新对象 + 原对象加计数」而不是「就地解码」，把不可变表示与可变表示彻底分开，读路径永远不会写脏共享对象；代价是解码 INT 对象每次都要分配一次内存。**架构推论：** INT 编码让「12345」这类值完全不占堆内存，是 Redis 内存优化的主力之一，代价是所有需要按字节处理字符串的命令都必须先解码一次。**架构推论：** 共享对象禁止转换，是为了让「不可变即可无锁并发读」这条安全性质不被破坏；主核是多写者引擎，若要复用同一套编码转换思路，必须显式区分「独占可变」与「共享不可变」两种所有权状态，主核的转换通常只能在获得写所有权之后做。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/object.c` | `tryObjectEncoding` | 写入方向的编码选择：INT 化、EMBSTR 化，都不成时裁剪 SDS 尾部 |
| `src/object.c` | `getDecodedObject` | 读取方向的解码：RAW/EMBSTR 加计数返回，INT 转成新的 RAW 对象 |
| `src/object.c` | `dupStringObject` | 按原编码复制字符串对象，保证副本不被共享且引用计数为 1 |

## 参考

- [Redis 源码：object.c](https://github.com/redis/redis/blob/unstable/src/object.c)
- [Redis 源码：object.h](https://github.com/redis/redis/blob/unstable/src/object.h)
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/)
- [Redis 文档：SET 命令](https://redis.io/docs/latest/commands/set/)

## 与主核 / AI 的关系

Redis 这套「同一份值有多种物理表示、按读写方向各自转换」的设计，对主核最有参考价值的是**表示与逻辑解耦**这一点：主核的列式行存里，同一个逻辑字段可以有字典编码、RLE、位图、量化向量等形态，读路径与写路径各自决定用哪种。但主核不能照搬「写时换到最省形态」这条时机选择——主核是多写者引擎，行可能在写线程与后台压缩线程之间流转，编码切换必须由持有写锁的一方发起，并保证切换过程中读者看到的是一致形态；Redis 之所以敢在参数解析阶段就地换编码，前提是那会儿对象还没被共享。另一处差别在解码策略：Redis 的 `getDecodedObject` 每次解码都可能分配内存，主核的解码若发生在扫描热路径上会放大分配压力，通常需要缓存解码结果或提供不解码的算子。AI 侧若用 Redis 存 embedding 或特征向量，用 JSON 字符串塞进去会直接触发「长度超阈值、不转换、只裁剪」这条退化路径，既拿不到 INT 编码也拿不到 EMBSTR，内存占用会比预期高得多。
