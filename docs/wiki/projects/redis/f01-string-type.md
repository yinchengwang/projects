---
project: redis
feature: string-type
page_type: feature
schema_version: 1
refs:
  - path: src/t_string.c
    symbols: [setGenericCommand, getGenericCommand, getrangeCommand]
---

# 字符串类型

## 一句话职责

`src/t_string.c` 把 SET/GET/GETRANGE 这一族字符串命令收敛成三个入口：写入统一走 `setGenericCommand`，读取走 `getGenericCommand`，取子串走 `getrangeCommand`。

## 机制是什么

字符串是 Redis 最基础的类型，值的内部表示有 raw 与 int 两种编码——编码如何挑选属于对象层（d02 页），本页只讲命令语义层。

写入侧被刻意做成**一个带 flag 的通用入口**。`setGenericCommand` 的注释写明它服务于 SET、SETEX、PSETEX、SETNX、GETSET，行为差异全部由 `flags` 表达，源码里是一组 `OBJ_` 开头的宏：`OBJ_SET_NX`、`OBJ_SET_XX`、`OBJ_EX`、`OBJ_PX`、`OBJ_EXAT`、`OBJ_PXAT`、`OBJ_KEEPTTL`、`OBJ_SET_GET`、`OBJ_PERSIST`，以及条件匹配的 `OBJ_SET_IFEQ`、`OBJ_SET_IFNE`、`OBJ_SET_IFDEQ`、`OBJ_SET_IFDNE`。调用方只负责解析参数并拼好 flags：`setCommand` 走 `parseExtendedStringArgumentsOrReply` 解析后传入，`setnxCommand` 传 `OBJ_SET_NX` 并把成功/失败的回复分别指定为 `shared.cone` 与 `shared.czero`，`setexCommand`/`psetexCommand` 分别传 `OBJ_EX`+`UNIT_SECONDS` 与 `OBJ_PX`+`UNIT_MILLISECONDS`。

读取侧只有一个真正的基础动作：`getGenericCommand`——按键取值、判类型、回 bulk。它同时被 `getCommand` 直接调用，也被 `getdelCommand`、`getsetCommand` 以及写入路径上的 `OBJ_SET_GET` 选项复用。

取子串是独立命令 `getrangeCommand`：它不复用 `getGenericCommand`，因为它需要在拿到值之后按索引切一段。

位操作（SETBIT/GETBIT/BITCOUNT/BITPOS/BITFIELD）不在本页，见 L 组；对象编码的选择与转换也不在本页，见 d02 页。

## 怎么实现的

**写入：`setGenericCommand`。** 顺序是有讲究的，源码注释在每一步都标了理由。

1. **归一化过期时间。** 若带了过期参数，先调 `getExpireMillisecondsOrReply`：它把参数解析成绝对毫秒时间戳——EX/PX 是相对 TTL，要加上 `commandTimeSnapshot()`；EXAT/PXAT 本身即绝对时间。单位是秒时先乘 1000。溢出用 `add_overflow_ll` 检出，非法值回错误。
2. **GET 选项先读。** `flags` 带 `OBJ_SET_GET` 时先调 `getGenericCommand(c)` 把旧值吐给客户端，失败（类型不符）直接返回。
3. **一次查找拿到值与链接。** 用 `lookupKeyWriteWithLink` 同时得到当前 kvobj 与 `dictEntryLink`，`found` 即键是否存在。
4. **NX/XX 前置判定。** 键已存在且要求 NX，或键不存在且要求 XX/IFEQ/IFDEQ，都走 abort 分支：不带 GET 时回 `abort_reply ? abort_reply : shared.null[c->resp]`。注意 XX 与 IFEQ/IFDEQ 要求键**必须存在**，而 NX 要求键**必须不存在**。
5. **条件匹配。** 存在且带 IFEQ/IFNE 时用 `sdscmp` 比较解码后的字符串；带 IFDEQ/IFDNE 时先 `validateHexDigest` 校验参数是十六进制摘要，再与 `stringDigest(current)` 的结果比较。源码注释特别指出这一步要复用第 3 步拿到的 kvobj——再查一次可能触发 rehash 让 `link` 失效。
6. **过期时刻已到时不写键。** `checkAlreadyExpired(milliseconds)` 为真时：键存在就 `dbDelete` 并把命令改写成 DEL/UNLINK 传播出去、发 `del` 键空间通知、`server.dirty++`；`server.stat_expiredkeys++`；最后仍然回 OK。注释解释了双重口径：统计上当作「插入后又过期」，但对模块的键空间通知要如实反映「删了旧键、没有插入新键」。
7. **真正写入。** `setKeyByLink` 落值；若带过期则 `dbRefreshLink` 刷新链接后再 `setExpireByLink`（注释说明这一步可能重分配对象，所以必须刷新）。随后 `incrRefCount(*valref)` 把引用计数从 1 提到 2——注释写明客户端的 `c->argv` 仍持有一份引用，`call()` 结束时会 `decrRefCount`。
8. **传播改写。** 带 EX/PX/EXAT 时，命令会被传播成 `SET key value PXAT <绝对毫秒>`。源码注释区分了两条路径：若命令恰好就是 `SET key value EX/PX/EXAT ttl`（`c->cmd->proc == setCommand && c->argc == 5`），只原地改写第 3、4 个参数；否则整条命令用 `rewriteClientCommandVector` 重写，把 NX/XX/GET 等额外选项一并剥掉。
9. **剥掉 GET。** 若用了 GET 选项且没有过期（有过期时整条命令已经重写过），遍历参数把每个 `GET` 参数置空，因为副本侧不需要旧值。

**读取：`getGenericCommand`。** `lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])`：键不存在时回 nil 并返回 `C_OK`——**不存在不是错误**。存在则 `checkType(c, o, OBJ_STRING)` 判类型，不是字符串回 WRONGTYPE。最后 `addReplyBulk(c, o)`。

**取子串：`getrangeCommand`。** 先把 start/end 解析成整数，再 `lookupKeyReadOrReply(..., shared.emptybulk)` 取值并判类型。取值方式分两路：若 `o->encoding == OBJ_ENCODING_INT`，说明这个值是整型编码、底层没有 sds 字符串，只能用 `ll2string` 格式化到 32 字节的栈缓冲 `llbuf`；否则直接取 `o->ptr` 与 `sdslen`。随后做索引归一化：start 与 end 都为负且 start > end 时直接回空串；负的 start 换算成 `strlen+start`，负的 end 换算成 `strlen+end`；仍为负则夹到 0；`end >= strlen` 则夹到 `strlen-1`。最后判断 `start > end` 或串长为 0 就回空串，否则用 `addReplyBulkCBuffer` 直接回 `str+start` 起 `end-start+1` 字节。

## 关键取舍

**源码事实：** 写入路径用 `lookupKeyWriteWithLink` 一次拿到值与链接，注释明确说明是为了避免二次查找触发 rehash 使 `link` 失效。**源码事实：** 过期时刻已到的 SET 不写键而是删键，并把命令改写成 DEL/UNLINK 传播。**源码事实：** 带相对 TTL 的 SET 一律传播为 `PXAT` 绝对时间戳。**源码事实：** GETRANGE 对整型编码的值必须先 `ll2string` 转成可寻址的字节序列。**源码事实：** `checkStringLength` 用 `server.proto_max_bulk_len` 作为写入长度的闸门，超限报 "string exceeds maximum allowed size (proto-max-bulk-len)"。

**架构推论：** 把 SET 全家族的语义收敛进一个带 flag 的函数，省掉了每个变体各写一遍「NX/XX 判定 + 过期处理 + 传播改写」的三份副本；代价是 flags 位运算的可读性下降，新人很难从 `setGenericCommand` 看出 SETNX 与 SET 的差别在哪几行。**架构推论：** 传播统一改写成绝对时间戳，可以让副本与 AOF 重放不依赖各自的时钟，规避 TTL 漂移；代价是持久化流里丢掉了原始的相对 TTL 形态（语义等价但可读性变差）。**架构推论：** GETRANGE 不拷贝子串、直接引用底层缓冲区回复，省一次内存分配与拷贝；代价是回复内容在对象被修改前必须保持有效——这靠 Redis 单线程语义兜底，多线程引擎必须改成拷贝。**架构推论：** 把「过期时刻已到」定义为「删除而非写入」，让键空间通知如实反映磁盘上发生的事，代价是统计口径与通知口径需要分开维护。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/t_string.c` | `setGenericCommand` | SET 全家族的写入入口：过期归一化、NX/XX 与条件匹配、过期时刻的删除语义、写入与传播改写 |
| `src/t_string.c` | `getGenericCommand` | 读取入口：按键取值、类型检查、回 bulk；键不存在回 nil 且不算错误 |
| `src/t_string.c` | `getrangeCommand` | 取子串：索引归一化、整型编码的字节化、直接引用底层缓冲区回复 |

## 参考

- [Redis 源码：t_string.c](https://github.com/redis/redis/blob/unstable/src/t_string.c)
- [Redis 文档：字符串数据类型](https://redis.io/docs/latest/develop/data-types/strings/)
- [Redis 命令：SET](https://redis.io/docs/latest/commands/set/)
- [Redis 命令：GETRANGE](https://redis.io/docs/latest/commands/getrange/)

## 与主核 / AI 的关系

「一族命令收敛成一个带 flag 的入口」这个做法主核可以直接借鉴：主核的多模态写入算子往往也有「覆盖/仅当不存在/仅当存在/带条件」几种变体，若各写一份，很容易出现某个变体漏掉条件判定或漏掉传播改写。Redis 这里的做法是让**唯一的写入点**负责条件、过期与传播三条不变量。

绝对时间戳传播对主核同样重要：主核是多写者引擎，副本若按各自时钟算 TTL 必然漂移，Redis 用「传播时把相对时间换成绝对时间戳」消除了这个偏差。

AI 侧常把 embedding 当字符串塞进 Redis，这时有两个坑要避开：一是整型编码（值能表示成整数时会切换到 int 编码），GETRANGE 会先 `ll2string` 还原，这是本分支的真实行为；二是 Redis 的字符串是**字节串**不是文本，多字节 UTF-8 的下标语义与字节下标一致，按「字符数」切片的直觉在这里不成立。
