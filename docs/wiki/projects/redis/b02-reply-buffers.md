---
project: redis
feature: reply-buffers
page_type: feature
schema_version: 1
refs:
  - path: src/networking.c
    symbols: [addReplyBulkCBuffer, addReplyErrorObject, addReplyLongLong, addReplyArrayLen, addReplyNull]
---

# 回复编码与缓冲

## 一句话职责

把命令的结果按 RESP 协议编码成字节，塞进客户端的输出缓冲——先是每个客户端自带的静态缓冲，溢出后再转进一条 `clientReplyBlock` 链表。

## 机制是什么

Redis 的客户端输出缓冲是**两级**的：

1. **静态缓冲** `c->buf`：客户端结构体里的一块连续内存，初始大小 `PROTO_REPLY_CHUNK_BYTES`（`src/server.h` 里定义为 `16*1024`，即 16KB）。用 `zmalloc_usable` 分配，分配器多给的那部分空间记在 `c->buf_usable_size` 里。
2. **回复链表** `c->reply`：一个 `list`，节点值是 `clientReplyBlock`。静态缓冲装不下的部分追加到这里，新块至少 `PROTO_REPLY_CHUNK_BYTES` 大小。

编码入口是一批 `addReply*` 函数。命令实现只管调这些函数，一个字节都不碰 socket——真正写出去是 b04 的事；缓冲装不下怎么办、要不要断开客户端，是 b03 的事。

本页覆盖五种回复形态：整数（`addReplyLongLong`）、数组（`addReplyArrayLen`）、批量串（`addReplyBulkCBuffer`）、错误（`addReplyErrorObject`）、空值（`addReplyNull`）。

## 怎么实现的

**写入闸门 `_prepareClientToWrite`。** 所有 `addReply*` 在真正写之前都要过这一关，返回 `C_ERR` 表示这次回复不该产生。它的判断顺序是：

- 带 `CLIENT_SCRIPT` 或 `CLIENT_MODULE` 标志 → 直接返回 `C_OK`。源码注释：Lua 客户端根本没有 socket，不装任何 handler。
- 带 `CLIENT_CLOSE_ASAP` → 返回 `C_ERR`。已经决定要关掉了，不必再写。
- 带 `CLIENT_REPLY_OFF` 或 `CLIENT_REPLY_SKIP`（且不是 `CLIENT_PUSHING`）→ 返回 `C_ERR`。这两个标志对应 `CLIENT REPLY OFF` 与 `CLIENT REPLY SKIP`。
- 带 `CLIENT_MASTER` 且没有 `CLIENT_MASTER_FORCE_REPLY` → 返回 `C_ERR`。注释：主库不收回复，除非显式要求。
- `!c->conn` → 返回 `C_ERR`，注释点名这是「加载 AOF 用的假客户端」。
- 最后，若客户端在主线程上且当前没有待发回复，就 `putClientInPendingWriteQueue(c)`——挂进待写队列而不是立刻装可写事件。

**`addReplyLongLong`。** 整数回复。值为 0 时直接用共享对象 `shared.czero`，值为 1 时用 `shared.cone`；其余走 `_prepareClientToWrite` 后调 `_addReplyLongLongWithPrefix(c, ll, ':')`，即输出 `:<数字>\r\n`。

**`addReplyArrayLen`。** 数组长度。先 `serverAssert(length >= 0)`，再过闸门，最后 `_addReplyLongLongMBulk(c, length)`——前缀换成了 `*`，输出 `*<数字>\r\n`。

这两条路径背后共用一段共享头逻辑。`_addReplyLongLongWithPrefix` 的注释写明：`$3\r\n`、`*2\r\n` 这类头部在协议里出现得极其频繁，所以准备了一批共享对象。当数值落在 `OBJ_SHARED_BULKHDR_LEN` 以内（`src/server.h` 定义为 32）且非负时，直接取 `shared.mbulkhdr[ll]` / `shared.bulkhdr[ll]` / `shared.maphdr[ll]` / `shared.sethdr[ll]` 的字节，一个字节都不额外生成；超出范围才用 `ll2string` 现场拼 `"<前缀><数字>\r\n"`。`OBJ_SHARED_HDR_STRLEN` 宏（同一文件）给出这些共享头的实际长度：小于 10 的是 4 字节，否则 5 字节。

**`addReplyNull`。** 空值。RESP2 下发 `$-1\r\n`，RESP3 下发 `_\r\n`——分支依据是 `c->resp`。同一族里 `addReplyNullArray` 在 RESP2 下是 `*-1\r\n`、RESP3 下仍是 `_\r\n`，源码注释解释 RESP3 里已经没有「空数组」这个概念。

**`addReplyBulkCBuffer`。** 批量串。这是本次改动后的一条新路径，注释说明了它的设计意图：把 `"$<len>\r\n"` 头在栈上拼好，把 payload **按引用**交出去（不拷进临时缓冲），然后头、载荷、尾随 CRLF 三段**一次性**成批写入——把原来每调用一次就扫一遍的三趟压成一趟，载荷大小不限。

实现上：先过闸门；栈上开一块 `char hdrbuf[LONG_STR_SIZE + 3]`，注释标注「`$` + 最多 20 位十进制（64 位）+ `\r\n`，`LONG_STR_SIZE` 已经算进了结尾的 NUL」。若 `len < OBJ_SHARED_BULKHDR_LEN`（32），头部直接指向 `shared.bulkhdr[len]->ptr`，`hdr_len` 取 `OBJ_SHARED_HDR_STRLEN(len)`，注释标注「直接指向共享对象，不用拷贝」；否则在栈上用 `ll2string` 手工拼。最后构造 `replySegment seg[3]`，三段分别是头、载荷、`"\r\n"`，交给 `_addReplySegmentsToBufferOrList`。

**`addReplyErrorObject`。** 错误回复。它要求传入的对象**以 `-ERRORCODE` 开头、以 `\r\n` 结尾**（注释原文：「Unlike addReplyErrorSds and others alike which rely on addReplyErrorLength」）。实现只有两步：先 `addReply(c, err)` 把整段协议字节写进输出缓冲，再 `afterErrorReply(c, err->ptr, sdslen(err->ptr)-2, 0)`——长度减 2 是为了忽略尾部那对 `\r\n`，后处理这一步做统计与日志。

`afterErrorReply` 里有一处分流值得注意：模块客户端（`CLIENT_MODULE`）的错误不会被立即统计，而是塞进 `c->deferred_reply_errors` 链表，注释解释了两类模块调用者的差别——`RM_Call` 的错误并不返回给客户端所以不该计数，而 `RM_ReplyWithError` 的错误之后会被主线程并入真实客户端。普通客户端则走 `server.stat_total_error_replies++` 与 `incrementErrorCount`（按错误码前缀分类，前缀搜索范围限制在 32 字符内）。

底层还有 `addReplyErrorLength`：若传入串不是以 `-` 开头就自动补 `-ERR `，然后补 `\r\n`。`addReplyProto` 则是更底层的入口——只过闸门再 `_addReplyToBufferOrList`。

**批量写入的落点 `_addReplySegmentsToBufferOrList`。** 它做四件事：

1. `CLIENT_CLOSE_AFTER_REPLY` 直接返回。
2. 副本客户端特判：`unlikely(clientTypeIsSlave(c))` 时用 `logInvalidUseAndFreeClientAsync` 断开。注释写明理由：复制链路只用来传数据不产生回复，「这是检查命令是否产生了回复的最简单方式」。
3. 累加 `c->net_output_bytes_curr_cmd`，并调 `reqresSaveClientReplyOffset(c)`（注释说这是幂等的）。
4. **push 消息延后**：若客户端带 `CLIENT_PUSHING`、且它就是 `server.current_client`、且正在执行的命令不是「以 push 作回复」的命令（`cmdHasPushAsReply`），那么这些字节被写进 `server.pending_push_messages` 而不是直接进回复缓冲。注释解释：PUBLISH 到自己订阅的通道时，push 消息要排在命令回复之后（`multi-exec` 期间尤其重要）。注释还指出 `SUBSCRIBE` 命令族是例外——它们本身就是 push 作回复。

最后逐段写入：先 `_addReplyPayloadToBuffer(c, s, len, PLAIN_REPLY)`，写不下的尾部再 `_addReplyPayloadToList(c, c->reply, s + reply_len, len - reply_len, PLAIN_REPLY)`。函数上方的注释说明了「隐式溢出」的语义：一旦某段溢进链表，后续段就整体路由到链表，顺序不变。

**两级缓冲的落点。** `_addReplyPayloadToBuffer` 只在 `c->reply` 链表为空时才写静态缓冲（注释：链表里已经有东西就不能再往静态缓冲里塞），否则直接返回 0，让调用方把数据交给链表。`_addReplyPayloadToList` 先尝试追加到链表尾节点 `clientReplyBlock`：尾节点是编码缓冲且放得下就追加，放不下就新建；尾节点不是编码缓冲而新数据需要编码，就不能追加（注释：「tail is non-encoded but new payload needs encoding, can't append」）。新建节点时按 `zmalloc_usable` 分配，**至少** `PROTO_REPLY_CHUNK_BYTES`（16KB）大小，并把分配器的内部碎片算进 `tail->size`。新节点建好后 `c->reply_bytes += tail->size`，并立刻调 `closeClientOnOutputBufferLimitReached(c, 1)`——这是写入侧唯一的超限检查点。

## 关键取舍

**源码事实：** 输出缓冲分两级，静态缓冲初始 16KB（`PROTO_REPLY_CHUNK_BYTES`），溢出后转进链表，新链表块同样至少 16KB。**源码事实：** 长度头在数值小于 `OBJ_SHARED_BULKHDR_LEN`（32）时直接复用共享对象，一个字节都不生成。**源码事实：** `addReplyBulkCBuffer` 用三段式 `replySegment` 一次成批写入，payload 按引用传递不拷贝。**源码事实：** 静态缓冲只在链表为空时使用；一旦数据进了链表，静态缓冲就不再接收新数据。**源码事实：** 输出缓冲超限检查发生在**新建链表块**的时刻（`_addReplyPayloadToList`）以及编码引用写入的时刻（`_addBulkStrRefToBuffer`），不是在每次追加时。

**架构推论：** 「编码」与「发送」彻底分离，使命令实现完全不知道 socket 的存在，回复路径因此可以在未来换成别的传输而不动命令代码。**架构推论：** 共享长度头 + 按引用 payload，是「少生成字节」与「少拷贝字节」两个方向上的优化：前者省的是栈上 `ll2string` 的开销，后者省的是把大 value 复制进缓冲的内存带宽。**架构推论：** 静态缓冲优先、链表兜底，让绝大多数小回复一次分配都不需要额外 malloc；代价是一旦越过阈值就会额外分配 16KB 的块，短期内存占用可能反而变大。**架构推论：** 超限检查放在「新建块」而不是「每次追加」，是因为追加路径在热路径上、必须足够快；代价是单个大块在写入过程中可能已经超限，等下一块才被发现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/networking.c` | `addReplyLongLong` | 整数回复：0/1 走共享对象，其余输出 `:<数字>\r\n` |
| `src/networking.c` | `addReplyArrayLen` | 数组回复：断言长度非负后输出 `*<数字>\r\n`，小数值复用共享头对象 |
| `src/networking.c` | `addReplyBulkCBuffer` | 批量串回复：栈上拼长度头或复用共享头，载荷按引用三段成批写入 |
| `src/networking.c` | `addReplyErrorObject` | 错误回复：写出整段 `-CODE ...\r\n`，再走错误统计与日志后处理 |
| `src/networking.c` | `addReplyNull` | 空值回复：RESP2 发 `$-1\r\n`，RESP3 发 `_\r\n` |

## 参考

- [Redis 源码：networking.c](https://github.com/redis/redis/blob/unstable/src/networking.c) —— 回复编码与输出缓冲的完整实现。
- [Redis 源码：server.h](https://github.com/redis/redis/blob/unstable/src/server.h) —— `PROTO_REPLY_CHUNK_BYTES`、`OBJ_SHARED_BULKHDR_LEN` 等缓冲与共享头常量。
- [Redis 文档：RESP 协议规范](https://redis.io/docs/latest/develop/reference/protocol-spec/) —— 各种回复类型在线路上的确切字节形态。
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/) —— 输出缓冲占用与客户端内存的关系。

## 与主核 / AI 的关系

「命令实现只管调 `addReply*`、完全不碰连接」这条分层，主核可以直接借鉴：查询结果编码与传输解耦后，同一套算子既能服务协议客户端，也能服务内部调用。但 Redis 的两级缓冲是为**固定协议 + 固定连接模型**设计的，主核若要支持流式返回（如 AI 的逐 token 输出），静态缓冲加链表的模型不足以表达「还不知道总长度的长结果」，需要换成真正的流式/分块响应通道。共享头对象这类「小值免分配」的优化在主核收益有限，因为主核的返回值多是行集或二进制大对象；主核更该关注的是大对象是否被整块物化——Redis 的 `addReplyBulkCBuffer` 至少保证了载荷只拷一次。
