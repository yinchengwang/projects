---
project: redis
feature: list-type
page_type: feature
schema_version: 1
refs:
  - path: src/t_list.c
    symbols: [listTypePush, listTypePop, pushGenericCommand, listTypeTryConversion]
---

# 列表类型

## 一句话职责

`src/t_list.c` 给列表类型提供「从两端推入、从两端弹出」的命令语义，并在列表变大或变小时决定紧凑编码（listpack）要不要升级为通用编码（quicklist）。

## 机制是什么

列表在 Redis 里有两种内部表示：**listpack**（整个列表塞在一个紧凑数组里）与 **quicklist**（多个 listpack 节点串成双向链表，节点可压缩）。命令层看不到这个区别——`LPUSH`/`RPUSH`/`LPOP`/`RPOP` 对用户是同一套语义，差别被 `listTypePush`、`listTypePop` 这层适配掉了。

命令层真正需要操心的是**何时切换编码**。Redis 的做法是把它做成一个显式的、带上下文的判断，而不是在每次插入后检查长度。`listTypeTryConversion` 接受一个 `list_conv_type`：`LIST_CONV_AUTO`（刚建好列表，让函数自己选）、`LIST_CONV_GROWING`（即将加入元素，只考虑从 listpack 升到 quicklist）、`LIST_CONV_SHRINKING`（刚删掉元素，考虑从 quicklist 降回 listpack）。`server.h` 里 `list_conv_type` 枚举就这三个值。

底层 quicklist 自身的节点拆分合并属于 E 组（e06 页），本组内部不重复；阻塞客户端的解除也不在本页（见 I 组）。

## 怎么实现的

**推入：`pushGenericCommand`。** 四个变体 LPUSH/RPUSH/LPUSHX/RPUSHX 共用它，`where` 区分头尾、`xx` 区分「仅当键存在」。

1. `lookupKeyWriteWithLink` 取列表对象，`checkType(c, lobj, OBJ_LIST)` 判类型；`existed` 记录键之前是否存在。
2. 键不存在时：若命令是 `xx` 变体，回 `shared.czero`（整数 0）立即返回——**这是本命令的退化路径**，语义是「没推成」而不是报错。否则建 `createListListpackObject()` 并 `dbAddByLink` 插入键空间，初始编码就是 listpack。
3. `listTypeTryConversionAppend(lobj, c->argv, 2, c->argc-1, NULL, NULL)`：在真正推入之前，先用「本次要加入的所有元素的字节数与个数」预判要不要升成 quicklist。它是 `listTypeTryConversionRaw` 的 `LIST_CONV_GROWING` 包装。
4. 逐个 `listTypePush(lobj, c->argv[j], where)` 推入，每个元素 `server.dirty++`。
5. 回 `listTypeLength` 得到的长度；发 `lpush`/`rpush` 键空间通知；更新键大小直方图。
6. 唤醒阻塞客户端：源码注释说明 `dbAdd()` 只会为**新建**的键发信号，对已存在的列表推入必须显式 `signalKeyAsReadyNonEmptyList`——否则那些已经在等「再多来几条」的客户端（如 BLMOVEM EXACTLY）不会被重新处理。

**`listTypePush`。** 按目标对象的编码分派：quicklist 走 `quicklistPush`（头/尾由 `where` 决定）；listpack 走 `lpPrepend`/`lpAppend`。两条路上都额外处理了**整型编码的元素**：整型编码的值底层是整数而非 sds 字符串，无法直接取指针，所以先用 `ll2string` 格式化成 32 字节栈缓冲再交给 `quicklistPush`（listpack 那条路则有专门的 `lpPrependInteger`/`lpAppendInteger`）。未知编码直接 `serverPanic`。

**弹出：`listTypePop`。** quicklist 走 `quicklistPopCustom`，它把值以 `unsigned char*` 或整型两种形态之一返回，取不到字符串时用 `createStringObjectFromLongLong` 补一个字符串对象；listpack 走 `lpFirst`/`lpLast` 定位、`lpGet` 取出、再 `lpDelete` 删除。两条路都返回一个新分配的字符串对象，调用方负责释放。

**弹出命令：`popGenericCommand`。** 可选第三参数 `count`。`count` 为 0 时走快路径直接回 `shared.emptyarray`；不传 count 时取单个元素（`listTypePop` + `addReplyBulk`）；带 count 时不逐个弹，而是算好区间（`where` 决定从头取还是从尾取）后 `addListRangeReply` 一次性回，再用 `listTypeDelRange` 整段删除。

**编码转换：`listTypeTryConversion` 家族。** 真正的分派在 `listTypeTryConversionRaw`：

- 对象是 quicklist 且 `lct == LIST_CONV_GROWING` → **直接返回**，注释写明「Growing has nothing to do with quicklist」——增长期只考虑往上走。
- 对象是 quicklist 且非增长期 → `listTypeTryConvertQuicklist(o, shrinking)`。
- 对象是 listpack 且 `lct == LIST_CONV_SHRINKING` → **直接返回**，对称地，缩小时不考虑往下走。
- 对象是 listpack 且非缩小型 → `listTypeTryConvertListpack(o, argv, start, end, fn, data)`。

`listTypeTryConvertListpack` 的判据是 `quicklistNodeExceedsLimit(server.list_max_listpack_size, lpBytes(o->ptr) + add_bytes, lpLength(o->ptr) + add_length)`。`add_bytes` 只累加**字符串编码**元素的 sds 长度——源码注释解释：只有字符串编码对象的长度能 O(1) 拿到，整型编码的元素长度不可预知，被跳过。超限则新建 `quicklistNew(server.list_max_listpack_size, server.list_compress_depth)`，把原 listpack 挂上去（长度为 0 时直接 `lpFree` 释放），然后把对象编码改成 `OBJ_ENCODING_QUICKLIST`。

`listTypeTryConvertQuicklist` 的降级条件更严：quicklist 必须**只有一个节点**且该节点的容器是 `QUICKLIST_NODE_CONTAINER_PACKED`，否则直接返回（多节点的 quicklist 不会退回 listpack）。然后用 `quicklistNodeLimit(server.list_max_listpack_size, ...)` 拿到字节与个数两个阈值；若本次是收缩（`shrinking` 为真），**两个阈值都折半**——源码注释写明这是为了避免因频繁增删而在 listpack 与 quicklist 之间反复横跳。只有节点大小与元素数都低于（折半后的）阈值时才降级：把 listpack 从唯一节点里摘出来、`quicklistRelease` 掉整个 quicklist、编码改回 `OBJ_ENCODING_LISTPACK`。

**阈值的来源。** `list-max-listpack-size` 配置项默认值是 `-2`（`src/config.c` 里 `createIntConfig("list-max-listpack-size", ..., -2, ...)`）。负值在 `src/quicklist.c` 里通过 `quicklistNodeNegFillLimit` 查一张表 `optimization_level[] = {4096, 8192, 16384, 32768, 65536}`，偏移量是 `(-fill) - 1`，所以 `-2` 对应 **8192 字节**。正值则转为「节点最多放多少个元素」的条数限制。

## 关键取舍

**源码事实：** 编码切换是**滞回**的——增长期只升不降，收缩期要低于一半阈值才降，源码注释直接写明「避免因频繁插入删除而反复转换」。**源码事实：** 降级要求 quicklist 只剩一个 packed 节点，多节点结构永远停在 quicklist。**源码事实：** 转换判据在插入**之前**用待插入元素的总字节数一次算清，而不是插完再看。**源码事实：** 整型编码的元素在长度预估里被跳过，因为其长度不可 O(1) 获取。**源码事实：** 已存在的列表被推入时需要显式 `signalKeyAsReadyNonEmptyList`，新建键由 `dbAdd` 代劳。

**架构推论：** 用「预估 + 滞回阈值」而不是「每次插入后检查长度」，把编码转换的触发次数从「随元素个数线性增长」压到「每次跨越阈值才一次」，代价是阈值附近会有一段「明明已经超了但还没转」的窗口。**架构推论：** 半阈值滞回等价于给转换加了一个迟滞回环，避免在阈值附近因一次 push/pop 就翻转编码；代价是小列表可能长期占用 quicklist 内存。**架构推论：** 把整型编码的处理泄漏到 `listTypePush`（每次都要判断元素是不是 int 编码）说明「同一语义、不同表示」的知识被复制到了多处；主核若有多表示的值对象，更干净的做法是在值对象层统一抽象出「取字节视图」接口。**架构推论：** 阻塞唤醒信号由 `dbAdd` 与命令两条路径分别负责，语义正确但容易漏——注释专门记了这个坑，说明这类「谁负责通知」的不变量最好集中到一处。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/t_list.c` | `pushGenericCommand` | LPUSH/RPUSH/LPUSHX/RPUSHX 的共同入口：建键、预判转换、逐个推入、唤醒阻塞者 |
| `src/t_list.c` | `listTypePush` | 按目标编码分派，把元素压入 quicklist 或 listpack，并处理整型编码元素 |
| `src/t_list.c` | `listTypePop` | 按目标编码分派，从 quicklist 或 listpack 摘出首/尾元素并返回新对象 |
| `src/t_list.c` | `listTypeTryConversion` | 编码切换的外层入口（不传待插入元素），向下分派到 quicklist↔listpack 的两个判据 |

## 参考

- [Redis 源码：t_list.c](https://github.com/redis/redis/blob/unstable/src/t_list.c)
- [Redis 源码：quicklist.c](https://github.com/redis/redis/blob/unstable/src/quicklist.c)
- [Redis 文档：列表数据类型](https://redis.io/docs/latest/develop/data-types/lists/)
- [Redis 命令：LPUSH](https://redis.io/docs/latest/commands/lpush/)

## 与主核 / AI 的关系

Redis 的列表只有「紧凑数组」和「分片链表」两种表示，靠滞回阈值切换；主核若要给多模态文档做「小对象内联、大对象外置」的分级存储，这套「先预估、后切换、切换带迟滞」的做法可以直接照搬，尤其是**迟滞**这一点——没有迟滞的分级会在阈值附近疯狂搬迁对象，主核的多写者场景下这种抖动会直接变成写放大。

命令层同时知道「listpack 还是 quicklist」和「元素是不是整型编码」，这是典型的表示泄漏。主核的多模态值对象（向量、标量、文本、图像）同样有多种内部表示，若在每个算子里都写一遍「如果是向量就取 float 指针、否则取字节指针」，很快会出现漏改的分支；更稳的做法是让值对象自己提供统一的字节视图或访问器。

AI 侧用 Redis 存对话历史时常用 list 存消息、用 ZSET 存「按时间排序的会话」。若历史里混入了可表示成整数的 token id，列表会走整型编码路径，字节预估会把它们跳过——这在容量规划时是个盲区，不能按「所有元素都计入字节预算」来估算内存。
