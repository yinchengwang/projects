---
project: redis
feature: skiplist
page_type: feature
schema_version: 1
refs:
  - path: src/t_zset.c
    symbols: [zslInsert]
---

# 跳表

## 一句话职责

`zslInsert` 往跳表里插入一个新成员：随机决定这个节点占几层、按分值找到该落的位置、把节点接进各层并维护每层的跨度计数。

## 机制是什么

跳表是一个**多层单向链表**：最底层（level 0）串起全部节点，上层是稀疏的「快速通道」，每个节点以一定概率出现在上层。查找时从最高层往下走，能一次跳过一整段底层节点。

Redis 的跳表为有序集合提供**按分值排序**的能力：节点的排序键是 `(score, member)`——分值相同时按成员字符串的字典序排。节点还额外维护一个 `span` 字段，记录「从本节点到本层下一个节点之间，底层一共有多少个元素被跨过」，这个计数让「名次」变成可累加的量，从而支持 O(log N) 的排名查询。

**本分支的位置。** 有序集合的大集合在本分支已经改用打包 B+ 树，跳表成了兼容旧数据与旧 RDB 的**遗留编码**——但它仍然被完整支持：`zsetAdd`、`zsetScore`、`zsetDel`、`zsetRank` 都有专门的 skip list 分支，`zsetConvertAndExpand` 也保留着 listpack↔skip list 与 B+ 树↔skip list 的双向转换。此外，ZUNION/ZINTER 的无 STORE 变体仍需要一条真实链接的跳表来产出回复。跳表自身的插入与层级维护在本页；有序集合的命令语义与编码切换在 f05 页。

两个常量定义在 `src/server.h`：`ZSKIPLIST_MAXLEVEL` 为 **32**（注释写「Should be enough for 2^64 elements」），`ZSKIPLIST_P` 为 **0.25**（注释写「Skiplist P = 1/4」）。

## 怎么实现的

**`zslInsert(zsl, score, ele)`** 只有三步，但它的两条前置约定很重要：

- 开头 `serverAssert(!isnan(score))`——分值不能是 NaN。
- 注释明确写着：函数**假定元素不在表里**，「we allow duplicated scores, reinserting the same element should never happen since the caller of zslInsert() should test in the hash table if the element is already inside or not」。也就是说「元素是否已存在」的判定责任在调用方（有序集合编码里就是那个哈希表），跳表自己不查重。
- 注释还写明：成员字符串 `ele` 会被**拷贝**进新节点，调用方保留所有权、可以自行释放。实际由 `zslCreateNode` 完成：它一次性分配「节点固定部分 + level 数组 + 内嵌 sds 缓冲」这一整块内存（注释写明这是「a single allocation containing: node + level[] + embedded sds」），并把内嵌 sds 的偏移量记在节点信息里。

**随机定层：`zslRandomLevel`。** 阈值是 `static const int threshold = ZSKIPLIST_P * RAND_MAX`，level 从 1 开始，`while (random() < threshold) level += 1;` 一直加，最后 `(level < ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL` 钳位。因此层级服从 p = 1/4 的幂律分布：约 3/4 的节点只有一层，约 3/16 有两层，依此类推。最低是 1（此时跳表退化成普通链表，查找变成 O(n)），最高是 32。

**接线：`zslInsertNode(zsl, node)`。** 这是跳表插入的核心，分几步：

1. **准备两组辅助数组**：`update[ZSKIPLIST_MAXLEVEL]` 记录每一层上「新节点应当插入到谁之后」，`rank[ZSKIPLIST_MAXLEVEL]` 记录到达该位置时在底层跨过了多少个元素。
2. **自顶向下定位**。从 `x = zsl->header` 出发，`for (i = zsl->level-1; i >= 0; i--)` 逐层下降。每层的 rank 初值是 `i == (zsl->level-1) ? 0 : rank[i+1]`（最高层从 0 开始，其余层承接下一层的计数）；内层用 `while (zslCompareWithNode(score, ele, x->level[i].forward) > 0)` 继续前进，前进时累加 `zslGetNodeSpanAtLevel(x, i)`。循环结束后 `update[i] = x`。
3. **抬高层级**（若新节点的 level 超过当前 `zsl->level`）：对新增的那些层把 `rank[i]` 置 0、`update[i]` 指向 header、并把该层 header 的 span 设为 `zsl->length`（因为此时 header 到末尾跨过了全部元素）；然后 `zsl->level = level`，同时把 header 自身的层数也更新为 level。
4. **接线并维护 span**。对 `i < level` 的每一层：新节点的后继取 `update[i]` 的原后继，`update[i]` 的后继改成新节点；span 更新为 `zslSetNodeSpanAtLevel(node, i, zslGetNodeSpanAtLevel(update[i], i) - (rank[0] - rank[i]))` 与 `zslSetNodeSpanAtLevel(update[i], i, (rank[0] - rank[i]) + 1)`——即新节点继承前驱剩余的跨度，前驱的跨度则缩短为「到新节点为止跨过的元素数 + 1」。
5. **未触及的层**（`i` 从 `level` 到 `zsl->level-1`）：这些层不插入新节点，但它们的跨度计数要加一，`zslIncrNodeSpanAtLevel(update[i], i, 1)`。
6. **回指针**：`node->backward = (update[0] == zsl->header) ? NULL : update[0]`；若新节点有底层后继，就把后继的 backward 指向它，否则 `zsl->tail = node`。
7. `zsl->length++`。

**span 为什么重要。** `zslGetRank` 在下降时直接把每一跳的 span 累加进 rank，得到名次——这正是 span 存在的理由。它是 **1 基**的，源码注释写明「due to the span of zsl->header to the first element」。反过来 `zslGetRankByNode` 从目标节点**向后**走累加 span，再用 `zsl->length - distance_to_end` 反推名次，注释说这样「avoids expensive string comparisons during traversal」。

**改分值的快路径。** `zslUpdateScore` 先判断「改完分值后位置是否不变」——条件是前驱分值小于新分值、且底层后继分值大于新分值。满足就直接改 `node->score` 返回，节点指针不动，因此哈希表里存的指针依然有效，**不需要更新哈希表**；不满足才走慢路径（先按旧分值定位并摘除节点，改分值，再重新插回）。

**删除时的降层。** `zslUnlinkNode` 在摘除节点后有一个 `while (zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL)` 循环：最高层空了就把它降掉，并把该层 header 的 span 清零。

## 关键取舍

**源码事实：** 层级按 p = 1/4 的幂律分布随机生成，上限 32 层（`src/server.h` 注释说 32 层够 2^64 个元素）。**源码事实：** `span` 计数让名次查询变成可累加的量，是 O(log N) 的来源。**源码事实：** `zslInsert` 拷贝成员字符串进节点内嵌的 sds，调用方仍持有自己的那一份。**源码事实：** `zslUpdateScore` 有「位置不变就原地改分」的快路径，此时不需要动哈希表。**源码事实：** 摘除节点时若最高层空了就降层。**源码事实：** 本分支大集合的默认编码是打包 B+ 树，跳表是遗留编码，但 ZUNION/ZINTER 的无 STORE 变体仍需真实链接的跳表。

**架构推论：** 用随机层级而不是确定性分层，避免了「所有节点挤在一层」或「某几层过密」造成的退化，把查找的期望复杂度稳定在 O(log N)；代价是每次插入都要取随机数，且结构不可预测——这与 B+ 树那种「顺序写、可批量追加」的特性正好相反。**架构推论：** `span` 是把「排名」这一额外查询维度塞进链表结构的代价；没有它，跳表只能沿底层链表逐个数元素，名次查询会退化成 O(n)。**架构推论：** 节点内嵌 sds 的一次性分配，减少了「节点结构 + 字符串」两次分配带来的碎片与指针追逐；代价是节点大小随成员长度变化，删除时不能像分离式结构那样把字符串块还给分配器的通用池。**架构推论：** 跳表的指针追逐对 CPU 缓存不友好，本分支把大集合换成打包 B+ 树正是往「顺序访问 + 缓存友好」的方向走；主核若自研有序索引，应当先按访问模式（范围扫描 vs 随机点查）选结构，而不是照抄 Redis 的历史选择。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/t_zset.c` | `zslInsert` | 跳表插入入口：断言分值非 NaN、随机定层、建节点（内嵌拷贝成员）、交给 `zslInsertNode` 接线 |

## 参考

- [Redis 源码：t_zset.c](https://github.com/redis/redis/blob/unstable/src/t_zset.c)
- [Redis 文档：有序集合数据类型](https://redis.io/docs/latest/develop/data-types/sorted-sets/)
- [Redis 源码：zset_btree.c](https://github.com/redis/redis/blob/unstable/src/zset_btree.c)

## 与主核 / AI 的关系

跳表最值得主核借鉴的不是结构本身，而是**span 计数**这个设计：在有序结构里额外挂一个「跨过了多少个元素」的计数器，排名查询就从 O(n) 变成 O(log n)。主核的向量 TopK 如果用平衡树或跳表实现排序，排名（以及按排名取第 k 个）往往是额外需求，span（或树里的子树大小）是把这类需求做进结构的低成本手段。

Redis 自己的演进给主核的启示更直接：**同一个语义要同时支持三种编码，就会出现三份实现、三条退化路径**。本分支的 zset 同时存在 listpack、skip list、B+ tree 三套实现，`zsetAdd`/`zsetScore`/`zsetDel`/`zsetRank` 四个函数都要写三遍——这正是本分支把跳表降级为兼容编码、最终必然要删掉它的原因。主核的多模态存储若一开始就有多种物理布局（行存/列存/向量索引），要预先想清楚「布局是命令的实现细节」还是「布局是数据类型的属性」：前者会像 zset 一样积压三份实现，后者则能让新增布局只影响该类型。

AI 侧若用有序集合承载「向量相似度排序」，Redis 的跳表排序键是 `(score, member)`——分值相同时按成员名排。这意味着**相同相似度的向量顺序是确定的**，但也意味着每次插入都要比较成员字符串，成员名越长比较越贵；若用文档 id 做成员名，长 id 会直接抬高插入成本。
