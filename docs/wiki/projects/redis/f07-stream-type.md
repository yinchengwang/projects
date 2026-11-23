---
project: redis
feature: stream-type
page_type: feature
schema_version: 1
refs:
  - path: src/t_stream.c
    symbols: [streamAppendItem, streamIteratorStart, streamIteratorGetID, streamTrim, streamDeleteItem, streamCompareID]
---

# 流类型

## 一句话职责

`src/t_stream.c` 负责流里的**条目本身**：追加条目、按 ID 区间迭代读取、按长度或 ID 下界裁剪、删除单条，以及 ID 的比较与生成语义。

## 机制是什么

流是 Redis 的**追加日志**类型：条目只能追加到尾部，不能就地修改；每条由若干 field-value 对组成；每条有一个单调递增的 ID。

ID 是两段式的 `<毫秒时间戳>-<序号>`，两段各是一个 uint64。物理上，ID 被当作 **128 位大端整数**编码，这样 radix tree 的字典序就等于 ID 的数值序。流的存储结构是「一棵 radix tree，每个节点挂一个 listpack」：同一节点里的条目共享一个 **master entry**——第一条目的字段名被存成该节点的字段名模板，后续条目若字段名完全相同就只存值（`STREAM_ITEM_FLAG_SAMEFIELDS`）；条目的 ID 也只存相对 master ID 的差值。

消费者组的游标、待确认列表与确认语义在 f08 页；radix tree 本身的实现在 e04 页，本页都不重复。

## 怎么实现的

**ID 比较：`streamCompareID(a, b)`。** 先比 `ms`，相等再比 `seq`，全等返回 0。返回 -1/0/1。

**ID 递增：`streamIncrID`。** `seq` 加一；`seq` 已是 `UINT64_MAX` 时 `ms` 加一并把 `seq` 归零；两者都是 `UINT64_MAX` 时回绕到 0-0 并返回 `C_ERR`（注释称这是「the last possible streamID」）。配套的 `streamNextID` 用它生成下一个自动 ID：当前毫秒时间大于 `last_id.ms` 就用当前时间、序号归零；否则沿用 `last_id` 并递增序号——注释写明这样「never go backward」。

**追加：`streamAppendItem(s, argv, numfields, added_id, use_id, seq_given)`。**

1. **定 ID。** 给了 `use_id` 且 `seq_given` → 直接采用；给了 `use_id` 但没给序号 → 若 `s->last_id.ms == use_id->ms` 则沿用 `last_id` 并把序号加一（序号已是 `UINT64_MAX` 时 `errno = EDOM` 返回 `C_ERR`），否则直接用 `use_id`；完全没给 → `streamNextID` 自动生成。
2. **单调性闸门。** `streamCompareID(&id, &s->last_id) <= 0` 即 `errno = EDOM` 返回 `C_ERR`。注释指出自动生成的 ID 也可能因为序号溢出而回绕，所以这个检查对自动 ID 同样必要。
3. **体积闸门。** 把 `numfields*2` 个元素的 sds 长度累加成 `totelelen`，超过 `STREAM_LISTPACK_MAX_SIZE` 就 `errno = ERANGE` 返回 `C_ERR`。这个常量在 `src/t_stream.c` 里定义为 `(1<<30)`，注释给的理由是「listpack can only host up to 32bit length strings, and also a total listpack size can't be bigger than 32bit length」。
4. **找尾节点并判是否要开新节点。** `raxSeek("$")` 取最后一个节点。若 `lp_bytes + totelelen >= node_max_bytes` 就开新节点；`node_max_bytes` 取 `server.stream_node_max_bytes`，但若它是 0 或超过 `STREAM_LISTPACK_MAX_SIZE` 会被钳到 `STREAM_LISTPACK_MAX_SIZE`。另外若配了 `server.stream_node_max_entries`，还要数该 listpack 里「有效条目 + 已删条目」，达到上限同样开新节点。两个配置项的默认值在 `src/config.c`：`stream-node-max-bytes` 为 **4096**，`stream-node-max-entries` 为 **100**。
5. **建 master entry（开新节点时）。** listpack 头部依次写：有效条目数 1、已删条目数 0、字段个数、各字段名、末尾的 0 终止符。新 listpack 预分配 `STREAM_LISTPACK_MAX_PRE_ALLOCATE`（`src/t_stream.c` 里定义为 **4096**）字节，若配置的 `stream_node_max_bytes` 更小则取更小的值——注释说这是为了避免每次 XADD 都 realloc。
6. **判字段名是否可复用（追加到已有节点时）。** 若本次字段个数与 master 相同，就逐个比长度并 `memcmp`；全部相同才置 `STREAM_ITEM_FLAG_SAMEFIELDS`。
7. **写条目。** 依次写 flags、ms 差、seq 差、（非 SAMEFIELDS 时）字段个数、字段名与值，最后写 `lp-count`。`lp-count` 的算法写在注释里：等于 `numfields + 3`（flags + ms 差 + seq 差），非 SAMEFIELDS 时再加 `numfields + 1`（字段名段与字段个数）。它的作用是让**反向遍历**能一次跳回本条的开头。
8. **收尾。** `s->length++`、`s->entries_added++`、`s->last_id = id`；`s->length == 1` 时设 `s->first_id = id`。

**迭代：`streamIteratorStart` 与 `streamIteratorGetID`。**

`streamIteratorStart` 把起止 ID 编成 128 位大端 key（`start` 缺省为 0-0，`end` 缺省为 `UINT64_MAX`-`UINT64_MAX`），同时解出本机字节序的 ms/seq 供比较用（注释说是为了「faster comparisons during iteration」）。正向时若给了非零的 `start` 就 `raxSeek("<=", start_key)`，越界则回退到 `"^"`；反向时用 `end_key` 做同样的定位。最后把 `skip_tombstones` 置 1——默认不吐墓碑。

`streamIteratorGetID` 是双层循环：外层按 radix 节点推进（正向 `raxNext`、反向 `raxPrev`），内层在当前 listpack 里逐条推进。读出 flags 后，按「master ID + ms 差 + seq 差」还原出条目 ID；字段数按 `STREAM_ITEM_FLAG_SAMEFIELDS` 决定是从 master 拿还是就地读。正向的放行条件是 `id >= start && id <= end`，反向相反；超出区间返回 0 表示迭代结束。带 `STREAM_ITEM_FLAG_DELETED` 的条目在 `skip_tombstones` 为真时被跳过、不返回。反向遍历靠 `lp-count`：从 listpack 尾部读出当前条目由几个元素组成，往回跳那么多步再退一步，就落到上一条的 `lp-count` 上。

**删除：`streamDeleteItem(s, id)`。** 用 `streamIteratorStart` 把区间设成 `(id, id)` 精确定位，找到后调 `streamIteratorRemoveEntry`。

`streamIteratorRemoveEntry` **并不真的把数据删掉**，而是给该条目的 flags 打上 `STREAM_ITEM_FLAG_DELETED`（墓碑），同时把 master entry 里的「有效条目数」减一、「已删条目数」加一。若这个 listpack 里已经没有有效条目（计数变成 1，即删的是最后一条），就整个节点从 radix tree 里摘掉并释放。最后它用当前 ID 重新起一次迭代器（`streamIteratorStop` + `streamIteratorStart`），修掉被 `lpReplaceInteger` 破坏的游标状态。

**裁剪：`streamTrim(db, s, args, max_deleted_id)`。** 参数由 `streamAddTrimArgs` 描述：策略 `TRIM_STRATEGY_MAXLEN`（按长度）或 `TRIM_STRATEGY_MINID`（按 ID 下界）、`approx_trim`（近似）、`limit`（本次最多删多少条）、`delete_strategy`（`KEEPREF` / `DELREF` / `ACKED`）。

从 radix tree 头部开始逐节点处理。MAXLEN 策略下若 `s->length <= maxlen` 就停；MINID 策略下看该节点最后一条的 ID 是否小于目标 ID，小于则整个节点可删。**整节点删除只在 `KEEPREF` 策略下允许**——源码注释给的理由是「we don't need to check or clean up consumer group references」。删完整节点后 `raxSeek(">=")` 继续往下走。

近似裁剪（`approx` 且 `KEEPREF`）下，整节点删不了就 `break`；但注释特别说明：非 KEEPREF 策略时，即使节点符合删除条件但因为需要检查消费者组引用而不能删，**也应当继续在节点内逐条处理**，不能 break。

节点内逐条处理时，先从 flags 与两个差值还原出条目 ID，再按策略决定是否停止（MAXLEN 看 `s->length <= maxlen`，MINID 看当前 ID 是否已大于等于目标）。已打过墓碑的条目跳过。删除策略在这里起作用：`ACKED` 要求 `streamEntryIsReferenced(s, &currid) == 0` 才允许删——源码注释把「被引用」定义为两种情况：条目的 ID 小于所有消费者组 `last_id` 的最小值（即至少有一个组还没读到它），或者它还挂在某个组的 PEL 里；`DELREF` 会先 `streamCleanupEntryCGroupRefs` 清掉组引用。真正打标记时用 `lpReplaceInteger` 改 flags——源码注释提醒 `lpReplaceInteger` 可能以不同宽度重编码并 realloc listpack，所以要先保存游标与 flags 元素的距离，改完再恢复。

节点内的条目全被删掉就把整个节点移除；否则更新 master entry 的有效/已删计数。**这里留着一处明确的未完成项**：源码写的是 `if (entries + marked_deleted > 10 && marked_deleted > entries/2) { /* TODO: perform a garbage collection. */ }`——也就是说「墓碑过多时回收空间」这条路径**当前没有实现**。

收尾时若 `s->length == 0` 则把 `first_id` 归零，否则若确实删过东西就用 `streamGetEdgeID(s, 1, 1, &s->first_id)` 重新取首个非墓碑 ID。两个包装函数 `streamTrimByLength` 与 `streamTrimByID` 只负责填参数：近似模式下 `limit` 取 `100 * server.stream_node_max_entries`，精确模式下取 0（不限量）。

## 关键取舍

**源码事实：** 条目的 ID 用相对 master ID 的差值编码，字段名与 master 完全相同时整个字段名段被省略。**源码事实：** 删除是打墓碑标记而不是真删；只有当一个 listpack 里再无有效条目时才整个节点释放。**源码事实：** 墓碑占比过半且总量超过 10 时本应做垃圾回收，但源码里是 TODO，尚未实现。**源码事实：** 裁剪有精确与近似两种模式，近似模式只按 radix 节点整块删。**源码事实：** 三种删除策略直接决定要不要连带处理消费者组引用。**源码事实：** `max_deleted_id` 允许是墓碑——注释写明「that doesn't matter, since no live entry lies between it and the last one actually deleted」。

**架构推论：** delta 编码加字段名模板，把「同构日志」的压缩率做到极致；代价是解码强依赖 master entry，master 一旦损坏整个节点都读不出来，所以才有 `streamValidateListpackIntegrity` 这类完整性校验。**架构推论：** 墓碑删除把「O(n) 数组搬移」换成「O(1) 打标记」，代价是需要额外的回收机制和一个「首个有效 ID」的重算；Redis 至今没实现那段 GC，说明墓碑回收是这类结构的长期负债。**架构推论：** 按 radix 节点为单位的近似裁剪，让 XTRIM 的成本与**节点数**相关而不是条目数相关，适合高频小批量裁剪；代价是裁剪精度变粗（近似模式下可能删得比要求的少，或留下比要求的多）。**架构推论：** ID 用「客户端毫秒时间 + 序号」两段式，天然适配「按时间排序 + 同毫秒内有序」；但它依赖客户端时钟，时钟回拨就会触发 `EDOM`，这正是追加路径上必须做单调性检查的原因。**架构推论：** 三种删除策略把「裁剪」与「消费者组语义」耦合在一起（`ACKED` 要查引用、`DELREF` 要清引用），代价是流的裁剪逻辑无法独立于组机制演进。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/t_stream.c` | `streamCompareID` | ID 比较：先比毫秒部分再比序号部分 |
| `src/t_stream.c` | `streamAppendItem` | 追加条目：生成并校验 ID、查体积闸门、判断是否开新 radix 节点、建 master entry、delta 编码写条目 |
| `src/t_stream.c` | `streamIteratorStart` | 迭代器初始化：起止 ID 编成大端 key、在 radix tree 上定位起始节点、设定墓碑策略 |
| `src/t_stream.c` | `streamIteratorGetID` | 迭代取下一条：还原 delta 编码的 ID 与字段数、按方向做区间判定、跳过墓碑 |
| `src/t_stream.c` | `streamTrim` | 裁剪：MAXLEN/MINID 两种策略、整节点删除与节点内逐条删除、三种删除策略与消费者组引用的处理 |
| `src/t_stream.c` | `streamDeleteItem` | 删除单条：精确定位后交给迭代器打墓碑标记 |

## 参考

- [Redis 源码：t_stream.c](https://github.com/redis/redis/blob/unstable/src/t_stream.c)
- [Redis 文档：流数据类型](https://redis.io/docs/latest/develop/data-types/streams/)
- [Redis 命令：XADD](https://redis.io/docs/latest/commands/xadd/)
- [Redis 命令：XRANGE](https://redis.io/docs/latest/commands/xrange/)

## 与主核 / AI 的关系

流是 Redis 里唯一为「多消费者分摊 + 至少一次投递」设计的类型，主核若要做任务队列或事件溯源日志，这套结构值得整体借鉴：**追加只写不读改、ID 单调、消费者组各自维护投递水位与待确认列表**。其中「delta 编码 + 字段名模板」这一层是存储优化的典范——同构日志（字段名固定）的压缩率极高，AI 侧的推理事件流（每条都是 `request_id / model / latency_ms / status`）正好是同构日志的典型形态。

墓碑删除与未实现的 GC 是主核必须避免的坑：主核的多写者引擎没法像 Redis 那样靠单线程顺序推进来保证「墓碑数不会失控」，一旦墓碑占比高，随机读要跳过大量墓碑就会变成性能悬崖。主核要么一开始就用「标记删除 + 后台 compaction」并把 compaction 做成真正实现的路径，要么在页式存储里直接做行删除。

ID 依赖客户端时钟这一点对 AI 侧尤其要小心：多台推理机同时写同一个流时，各自的时钟不一致会导致 ID 回退并触发 `EDOM` 错误，追加直接失败。Redis 提供的解法是让序号在同毫秒内兜底，但跨机的毫秒部分仍然不可比——主核若做分布式日志，ID 分配必须由单一权威点（类似 Raft 的序号分配）产生，不能沿用「客户端时钟 + 本地序号」。
