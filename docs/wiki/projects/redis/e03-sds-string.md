---
project: redis
feature: sds-string
page_type: feature
schema_version: 1
refs:
  - path: src/sds.c
    symbols: [sdsnewlen, sdsMakeRoomFor, sdsRemoveFreeSpace, sdscatlen]
---

# 动态字符串

## 一句话职责

sds 是 Redis 内部统一的字符串表示：在 `char *` 之前塞一段变长头部，把「长度」和「已分配容量」一起带在身上，于是取长度是 O(1)、追加不会算出目标大小、扩容可以按增长因子摊平。

## 机制是什么

一个 sds 指针指向的是**字符串内容的第一个字节**，头部在它前面。`src/sds.h` 定义了五种头部类型 `SDS_TYPE_5` / `SDS_TYPE_8` / `SDS_TYPE_16` / `SDS_TYPE_32` / `SDS_TYPE_64`，各自对应不同宽度的 `len` 与 `alloc` 字段；类型号存在紧贴内容前的那一个 `flags` 字节里（`sdsType(s)` 读 `s[-1]` 的低 3 位，`SDS_TYPE_BITS` 是 3）。`sdshdr8` 的布局是 `uint8_t len`、`uint8_t alloc`、`unsigned char flags`，`sdshdr16/32/64` 同理只是把前两个字段加宽，注释写明 `alloc` 是「excluding the header and null terminator」。

`sdshdr5` 只有 1 个字节的 `flags`，其中高 5 位直接存长度（`SDS_TYPE_5_LEN(s)` 取 `s[-1] >> SDS_TYPE_BITS`）。`src/sds.h` 的注释写明「sdshdr5 is never used, we just access the flags byte directly」——它的字段布局只是留作文档。

`flags` 字节里除类型号外的 5 个空闲位被当作「用户数据位」：`sdsSetAuxBit` / `sdsGetAuxBit` 可以往里塞标志位，注释说明用途是「special uses like immutable SDS embedded in other structures」，并警告**自动扩容会丢失这些位**。

类型的选择由 `sdsReqType(string_size)` 决定：长度小于 `1<<5` 用 TYPE_5；不超过 `(1<<8) - sizeof(struct sdshdr8) - 1` 用 TYPE_8；不超过 `(1<<16) - sizeof(struct sdshdr16) - 1` 用 TYPE_16；64 位 long 的平台上再依次是 TYPE_32 与 TYPE_64。注意阈值里减去了头部大小——`src/sds.c` 的注释解释原因：以前只按逻辑长度选类型、忽略头部大小，会导致 `alloc` 字段被截断，与真实缓冲区大小不一致。

本页只讲 sds 自身的分配、扩容与收缩。字符串在 Redis 对象里的编码是 D 组；底层分配器的实现是 e07。

## 怎么实现的

**创建：`sdsnewlen` → `_sdsnewlen`。** `sdsnewlen(init, initlen)` 调 `_sdsnewlen(init, initlen, 0)`（第三个参数 `trymalloc` 为 0 表示分配失败即 panic）；`sdstrynewlen` 则是 `trymalloc = 1` 的版本，分配失败返回 NULL。

`_sdsnewlen` 的步骤：

1. `type = sdsReqType(initlen)`；若 `type == SDS_TYPE_5 && initlen == 0`，改成 TYPE_8。源码注释写明原因：「Empty strings are usually created in order to append. Use type 8 since type 5 is not good at this.」——TYPE_5 记不住剩余空间，每次追加都得先扩容。
2. `hdrlen = sdsHdrSize(type)`；做溢出检查（`initlen + hdrlen + 1 <= initlen` 即为溢出）。
3. `s_malloc_usable(hdrlen + initlen + 1, &bufsize)` 一次性把**头部 + 内容 + 结尾空字节**申请下来。
4. `adjustTypeIfNeeded(&type, &hdrlen, bufsize)`：若实际可用字节数 `bufsize - hdrlen - 1` 超过了当前类型能表达的上限（`sdsTypeMaxSize`），就按真实可用大小重选类型并更新头部长度。注释解释触发条件：jemalloc 之类分配器可能返回比请求更大的缓冲区，导致 `alloc` 字段与真实缓冲区不匹配、浪费内存并带来不一致。
5. `sdsnewplacement(sh, bufsize, type, init, initlen)` 完成初始化——它相当于 C++ 的 placement new：按类型写好 `len` 与 `alloc`，`init` 为 NULL 时把内容清零，为 `SDS_NOINIT` 时完全不碰内容，否则 `memcpy` 进去，最后在 `s[initlen]` 写一个结尾空字节。

**扩容：`sdsMakeRoomFor` → `_sdsMakeRoomFor(s, addlen, greedy)`。** `sdsMakeRoomFor` 传 `greedy = 1`；`sdsMakeRoomForNonGreedy` 传 0。函数体：

1. `avail = sdsavail(s)`；`avail >= addlen` 就直接返回原串，什么都不做。
2. `reqlen = newlen = len + addlen`，并断言 `newlen > len` 以捕捉 size_t 溢出。
3. **贪心增长**：`greedy == 1` 时，若 `newlen < SDS_MAX_PREALLOC`（`src/sds.h` 里定义为 `1024*1024`，即 1MB）就 `newlen *= 2`，否则 `newlen += SDS_MAX_PREALLOC`。注释把这两种情况的理由写在一起：「When greedy is 1, enlarge more than needed, to avoid need for future reallocs on incremental growth. When greedy is 0, enlarge just enough so that there's free space for 'addlen'.」
4. `type = sdsReqType(newlen)`；若算出 TYPE_5 就改成 TYPE_8，注释写明：「Don't use type 5: the user is appending to the string and type 5 is not able to remember empty space, so sdsMakeRoomFor() must be called at every appending operation.」
5. **头部长度变了就不能 realloc**：`use_realloc = (oldtype == type)`。类型相同走 `s_realloc_usable(sh, hdrlen + newlen + 1, &bufsize, NULL)`，地址可能整体搬家；若 `adjustTypeIfNeeded` 又改了类型，还要 `memmove` 把内容挪到新的头部长度之后，并重写 `s[-1] = type` 与长度。类型不同则只能 `s_malloc_usable` + `memcpy` + `s_free(sh)`，注释写明「Since the header size changes, need to move the string forward, and can't use realloc」。
6. 最后 `usable = bufsize - hdrlen - 1`，`sdssetalloc(s, usable)`。

**收缩：`sdsRemoveFreeSpace(s, would_regrow)` → `sdsResize(s, sdslen(s), would_regrow)`。** `sdsResize` 的行为：

- `sdsalloc(s) == size` 就立刻返回原串。
- `size < len` 时把 `len` 截到 `size`（数据被截断）。
- `type = sdsReqType(size)`；若 `would_regrow` 为真且类型是 TYPE_5，改成 TYPE_8，注释写明「Don't use type 5, it is not good for strings that are expected to grow back.」——`would_regrow` 这个参数就是给「马上还要往里写」的调用方留的提示。
- `use_realloc = (oldtype == type || (type < oldtype && type > SDS_TYPE_8))`：同类型、或降级但仍高于 TYPE_8 时走 realloc，否则重新分配。走 realloc 时还有一个 jemalloc 专属优化：源码先算 `alloc_already_optimal = (je_nallocx(newlen, 0) == bufsize)`，注释解释「We aim to avoid calling realloc() when using Jemalloc if there is no change in the allocation size, as it incurs a cost even if the allocation size stays the same.」——分配尺寸没变就不调 realloc。
- 收尾 `s[len] = 0`、`sdssetlen(s, len)`、`sdssetalloc(s, newsize)`。源码注释特别说明：「The sdsAlloc size will be set to the requested size regardless of the actual allocation size, this is done in order to avoid repeated calls to this function when the caller detects that it has excess space.」——**记账按请求值走，不按实际分配值走**，这样调用方看到「还有富余」就不会反复来调。

**追加：`sdscatlen(s, t, len)`。** 取 `curlen = sdslen(s)`，先 `s = sdsMakeRoomFor(s, len)`（分配失败返回 NULL），再 `memcpy(s + curlen, t, len)`，`sdssetlen(s, curlen + len)`，最后写 `s[curlen + len] = '\0'`。整个过程**没有中间的拷贝缓冲**——这是 sds 相对「C 字符串 + 预估容量」的核心差别。

**释放。** `sdsfree` 对 TYPE_5 走 `s_free(sdsAllocPtr(s))`，其余类型走 `s_free_with_size(sdsAllocPtr(s), sdsAllocSize(s))`。源码注释解释了为什么 TYPE_5 要区别对待：「TYPE_5 has no alloc field so sdsAllocSize() returns the requested size which may not match the actual allocation, so not suitable for s_free_with_size().」

**真实 caller。**

- `sdsnewlen` 的真实调用方遍布全仓，代表性的有：`src/networking.c` 用 `sdsnewlen(SDS_NOINIT, c->bulklen+2)` 预分配批量字符串的读取缓冲、`src/aof.c` 用它预分配 AOF 命令缓冲、`src/config.c` 读配置文件、`src/eval.c` 算脚本的 SHA1、`src/acl.c` 存用户名、`src/cluster_legacy.c` 存节点 ID。`init` 传 `SDS_NOINIT` 的用法（`src/aof.c`、`src/networking.c`、`src/cluster.c`、`src/config.c`、`src/bitroar.c`）是「只申请空间、内容自己填」，省掉一次 memcpy；传 NULL 的用法（`src/bitops.c`、`src/hyperloglog.c`、`src/evict.c`）是「申请并清零」。
- `sdsMakeRoomFor` 的真实调用方：`src/networking.c` 的查询缓冲（`c->querybuf = sdsMakeRoomFor(c->querybuf, readlen)`）、`src/replication.c` 的复制缓冲、`src/rio.c` 的连接输出缓冲、`src/tracking.c` 拼装客户端追踪失效消息。
- `sdsRemoveFreeSpace` 的真实调用方：`src/server.c` 的 `clientsCronResizeQueryBuffer`（`c->querybuf = sdsRemoveFreeSpace(c->querybuf, 1)`，因为查询缓冲马上还要继续写）、`src/networking.c` 收缩命令参数数组（`argv[j] = sdsRemoveFreeSpace(argv[j], 0)`）、`src/object.c` 收缩字符串对象（`o->ptr = sdsRemoveFreeSpace(o->ptr, 0)`）、`src/module.c` 收缩模块 key。
- `sdscatlen` 的真实调用方：`src/t_string.c` 的 APPEND 命令、`src/tracking.c` 拼装失效消息协议、`src/server.c` 生成 INFO 文本、`src/sentinel.c`、`src/script_lua.c`。

**退化与回退路径。**

- **类型 5 是「省空间但不能追加」的退化形态。** `sdsavail` 对 TYPE_5 恒返回 0，`_sdsMakeRoomFor` 永远不选 TYPE_5，`sdsResize` 在 `would_regrow` 为真时也避开 TYPE_5。所以 TYPE_5 只可能出现在「长度 1 到 31 的新建字符串」上，一旦要追加就必须先升级成 TYPE_8。
- **分配失败有两条路。** `sdsnewlen` 走的是 panic 路径（`trymalloc = 0`），失败直接终止进程；`sdstrynewlen` 返回 NULL。`_sdsMakeRoomFor` 在 realloc 或 malloc 失败时返回 NULL，调用方必须检查——`sdscatlen` 就是 `if (s == NULL) return NULL`。
- **头部类型被迫升级时的搬移。** `adjustTypeIfNeeded` 在分配器给了更大的缓冲区时会把类型往上抬，这就要求把内容整体后移。`_sdsMakeRoomFor` 的 realloc 分支用 `memmove` 处理，`sdsResize` 的非 realloc 分支用 `memcpy` 处理——两者都用 `len + 1` 作为拷贝长度，**结尾空字节一起搬**，所以搬完不需要额外补写。
- **辅助位会丢。** `flags` 字节里的 5 个用户位在头部被重写时就消失——`adjustTypeIfNeeded` 改类型时 `s[-1] = type` 直接覆盖，`sdsResize` 走 realloc 且类型变化时同样覆盖。`src/sds.h` 的注释已提前警告「The aux bits are lost if the SDS is auto-resized」。
- **收缩会截断数据。** `sdsResize` 里 `if (size < len) len = size;`——把 size 传小就是截断，没有任何保护。

## 关键取舍

**源码事实：** 长度与容量存在头部里，`sdslen` 是 O(1)，尾部永远带一个空字节。**源码事实：** 五种头部类型按长度与真实可用容量选取，阈值扣除了头部大小。**源码事实：** 贪心扩容在 1MB 以下翻倍、1MB 以上加 1MB。**源码事实：** TYPE_5 没有 `alloc` 字段、可用空间恒为 0，因此不能用于追加。**源码事实：** `sdsResize` 把 `alloc` 记成请求值而非实际分配值。**源码事实：** `flags` 字节有 5 个用户位，扩容会丢。

**架构推论：** 把长度放进头部，让「取长度」从 O(n) 变成 O(1)，代价是每个字符串多付 1 到 16 字节的头部，以及 `sds *` 指针在扩容后可能失效——Redis 用「所有会改长度的函数都返回新指针」这条纪律来兜住，调用方必须重新赋回。**架构推论：** 头部类型随长度变化，意味着同一段代码要处理 5 种布局；Redis 用 `SDS_HDR_VAR` 宏把分支集中到头部的 switch 里，换来的是调用侧代码不必关心类型。**架构推论：** 「记账按请求值」与「分配器可能给更大缓冲」这两条组合起来，说明 sds 的 `alloc` 是**可用性上界**而不是**精确占用量**——这对精确的内存统计是不利的，Redis 因此另有 `zmalloc` 层面的记账（e07）。**架构推论：** 贪心扩容用「翻倍」换摊还次数，用「加 1MB」换大字符串不再反复搬——两段式增长律是典型的空间时间折中，代价是短命的字符串可能浪费接近一半的分配空间。主核若要复用这套字符串表示，要特别注意**并发写者下指针失效**这条纪律会变成数据竞争：Redis 是单线程所以「返回新指针、由调用方重新赋回」是安全的，主核必须改用不可变字符串 + 引用计数，或者在锁内完成更新。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/sds.c` | `sdsnewlen` | 创建入口：选头部类型、申请「头部 + 内容 + 空字节」，并按真实可用容量校正类型 |
| `src/sds.c` | `sdsMakeRoomFor` | 扩容入口：保证尾部至少有指定长度的空间，贪心增长并处理头部类型变化带来的搬移 |
| `src/sds.c` | `sdsRemoveFreeSpace` | 收缩入口：把分配收紧到当前长度，`would_regrow` 提示调用方是否还会继续写入 |
| `src/sds.c` | `sdscatlen` | 追加入口：先保证容量再直接拷贝，无中间缓冲 |

## 参考

- [Redis 源码：sds.c](https://github.com/redis/redis/blob/unstable/src/sds.c)
- [Redis 源码：sds.h](https://github.com/redis/redis/blob/unstable/src/sds.h)
- [Redis 文档：数据类型（String）](https://redis.io/docs/latest/develop/data-types/strings/)

## 与主核 / AI 的关系

sds 值得主核直接借鉴的是**「长度随字符串一起走」这条最朴素的原则**：它把 `strlen` 从热路径上彻底移除，并且让「追加」变成一次 memcpy 而不是「估算容量 + 可能失败」。主核的多模态记录里字段值经常是变长的（文本、元数据、向量二进制块），用长度前缀而不是空字节结尾来定界，能同时拿到「二进制安全」与「O(1) 取长度」。

另一条值得带走的是**「所有改变长度的操作返回新指针，由调用方重新赋回」**这条纪律——在单线程里它足以防止悬垂引用；主核是多写者引擎，同样的纪律必须配上加锁或写时复制，否则「重新赋回」本身就是一个竞态窗口。

不可照搬的是**「头部类型随长度分档」**：它让短字符串省下头部，也让所有访问都要过一遍 switch；主核的字段长度分布通常不如 Redis 那样集中（小 key 多、字段短），用单一长度前缀（固定 4 或 8 字节）通常更简单也更快。

AI 侧若把 prompt、上下文片段或向量元数据塞进 Redis 字符串，要留意 sds 的**扩容是按指针返回的**：批量拼接时若把中间结果存进容器而不重新赋回指针，就会在第一次扩容后留下悬垂指针——这类 bug 在小数据量下不出现，只有当字符串跨过 1MB 阈值或触发头部类型升级时才会显形。
