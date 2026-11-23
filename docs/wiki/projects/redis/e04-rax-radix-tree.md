---
project: redis
feature: rax-radix-tree
page_type: feature
schema_version: 1
refs:
  - path: src/rax.c
    symbols: [raxInsert, raxFind, raxSeek, raxNext, raxNew]
---

# 压缩前缀树

## 一句话职责

rax 是 Redis 用的基数树：它把字节串按公共前缀压成一条边，于是查找只走键长那么多步，同时保留了**字典序**——能按字节比较做前驱 / 后继定位，这是哈希表给不了的。

## 机制是什么

`src/rax.h` 的注释用一张 ASCII 图把结构讲清楚了：一个节点的 `data` 段里放的是**边上的字节**，不是节点本身的内容。普通节点（非压缩）是 `[abc][a-ptr][b-ptr][c-ptr](value-ptr?)`——三个字节是三条出边的首字节，后面跟同样数量的子节点指针。压缩节点（`iscompr = 1`）是 `[xyz][z-ptr](value-ptr?)`——它代表 `x → y → z` 三个单子节点被合并成的链，只有最后一个 `z` 节点真实存在并被指针引用。

压缩的目的写在注释里：单子节点链越少越好，节点越少则指针越少、内存越小、查找越快。反过来，插入一个与现有前缀部分重合的键时，必须把压缩节点**拆开**（node splitting），源码注释用 "ANNIBALE" 举例列出了四种拆分情形。

节点头是 `raxNode`：`iskey:1`、`isnull:1`、`iscompr:1` 三个标志位，`size:29` 存子节点数或压缩串长度。`size` 用 29 位是因为 `RAX_NODE_MAX_SIZE` 是 `(1<<29)-1`。节点末尾可选地带一个值指针。

**固定长度键的叶子内联。** `raxNewEx` 的 `keyFixedLen` 参数大于 0 时表示「所有键长度都固定为这个值」，此时**叶子节点被省掉**：值指针直接存在父节点的子指针槽里，深度到 `keyFixedLen - 1` 的节点变成「叶子父节点」，其槽里放的是值而不是子节点指针。注释说这是「a very common case in Redis so this implementation is very useful」——每个键省一次 `raxNode` 分配。

**迭代器。** `raxIterator` 里有一份 `key_static_string[128]`（`RAX_ITER_STATIC_LEN`），当前键直接放在迭代器里；另有一份 `raxStack`，`RAX_STACK_STATIC_ITEMS` 是 32——前 32 层祖先走静态数组，不额外分配。栈的存在是因为节点没有 parent 指针（注释说这是为了省空间）。

本页只讲树的形状与查找 / 插入 / 迭代接口。哈希表的遍历在 e02，stream 的命令语义在 F 组。

## 怎么实现的

**创建：`raxNew` → `raxNewEx(0, NULL, 0)`。** `raxNewEx(metaSize, alloc_size, keyFixedLen)` 的动作：断言 `keyFixedLen <= RAX_NODE_MAX_SIZE`；分配 `sizeof(*rax) + metaSize`（`metadata[]` 是给调用方挂自定义元数据用的柔性数组）；`numele = 0`、`numnodes = 1`；`head = raxNewNode(rax, 0, 0)` 建一个空的根节点。`alloc_size` 非空时，rax 会把自身占用的字节累加到这个指针上——这是给调用方做内存记账用的。

**查找：`raxFind` → `raxFindLink` → `raxLowWalk`。** `raxFind(rax, s, len, value)` 只做一件事：调 `raxFindLink(rax, s, len, value, &link)`，返回 1 表示找到（并把关联值写进 `value`），返回 0 表示没找到。

`raxFindLink` 除了返回「找到 / 没找到」，还返回一条 `raxNodeLink`——**停在哪里、该往哪个父槽写**。`src/rax.h` 把这个契约写得很清楚：「raxFindLink() produces a link. raxInsertAt() consumes it.」以及失效约定：「They become stale on ANY intervening rax mutation. Callers MUST commit (or discard) immediately after the find; do not interleave other rax calls on the same tree, do not retain across yield points.」

**插入：`raxInsert` = 一次查找 + 一次提交。** `raxInsert(rax, s, len, data, old)` 先调 `raxFindLink`（`value` 传 NULL）拿到 link，再调 `raxInsertAt` 提交。返回值语义是：**0 表示键已存在、值被覆盖**（旧值通过 `old` 出参带回），**1 表示新插入**。源码注释写明「Overwriting insert. One walk via raxFindLink, then commit at the recorded position.」——只走一遍树，不做「先查后插」的两遍遍历。另有 `raxTryInsert` 提供非覆盖语义：若键已存在则不动它，把旧值通过 `old` 返回并返回 0。

`raxInsertAt` 的分支顺序：

1. 若 `link->splitpos == RAX_LEAF_PARENT_STOP`（固定长度键、查找正好停在叶子父节点），直接覆盖那个内联槽，返回 0（已存在，被覆盖）。
2. 若走完了整个键（`consumed == len`）且当前节点不是压缩节点的中段：给节点 `raxReallocForData` 腾出值指针的空间；节点本来就是键就覆盖并返回 0；否则把它标成键（`raxSetData`）、`numele++`、返回 1。
3. 若停在压缩节点中间，必须先**分裂**：源码注释以 "ANNIBALE" 为例列了四种情形（插入 "ANNIENTARE"、"ANNIBALI"、"AGO" 等），共同点是「all the cases require the insertion of a non compressed node with exactly two children」，只有最后一种情形是单纯拆分压缩节点。
4. 否则在普通节点上加子节点，继续主插入循环：若当前节点没有子节点且剩余键长大于 1，就建成压缩节点把剩余后缀整段塞进去；否则逐字节加普通子节点。

**定位：`raxSeek(it, op, ele, len)`。** 操作符是一段字符串，函数按首字符分派：`>` 是大于、`>=` 是大于等于、`<` 是小于、`<=` 是小于等于、`=` 是精确匹配、`^` 是最小键、`$` 是最大键。源码注释写明识别方式：`op[0] == '>'` 看 `op[1] == '='` 决定是否带等号。

流程上：`^` 被翻译成 `raxSeek(it, ">=", NULL, 0)`——注释解释「Seeking the first key greater or equal to the empty string is equivalent to seeking the smaller key available.」；`$` 则从根一路走最右孩子（`raxSeekGreatest`）。其余操作符先做一次 `raxLowWalk` 定位到停点，再根据 `gt` / `lt` 决定往哪个方向走一步（`raxIteratorNextStep` 或 `raxIteratorPrevStep`）。

中途匹配有一个容易漏掉的细节，源码用 "f" / "foobar" 举例说明：若键集合是 `"f" = 1` 与 `"foobar" = 2`，那么 `seek("< foo")` 会停在压缩节点 "oobar" 的中间，但**这个节点本身就是答案 "f"**——所以这种情况不再往回走。源码注释：「if we stopped in the middle of a compressed node with perfect match, and the condition is to seek a key '<' than the specified one, then if this node is a key it already represents our match.」

**前进：`raxNext(it)`。** 它调 `raxIteratorNextStep(it, 0)`；返回 0 时区分两种情况——步进本身失败（内存不足）时置 `errno = ENOMEM`，走到末尾时置 `RAX_ITER_EOF` 并把 `errno` 清零。`raxPrev` 完全对称。

**真实 caller。**

- **stream 是 rax 最大的使用方。** `src/stream.h` 里 `stream` 结构有 `rax *rax`（注释「The radix tree holding the stream.」）、`rax *cgroups`、`rax *cgroups_ref`（消息 ID 到消费组的索引）、`rax *idmp_producers`；`streamCG` 有 `rax *pel`（该组的待确认消息列表）与 `rax *consumers`；`streamConsumer` 也有自己的 `rax *pel`。消息 ID 是定长二进制，所以这里天然吃到固定长度叶子内联的优化。
- **客户端追踪。** `src/tracking.c` 的 `TrackingTable` 与 `PrefixTable` 都是 rax——前者按被追踪的键建索引，后者按前缀建索引；`src/server.h` 里客户端的 `client_tracking_prefixes` 也是 rax。追踪失效消息需要按前缀做范围查找，哈希表做不到。
- **ACL。** `src/acl.c` 的 `Users` 是 rax（`src/server.h` 里声明为 `extern rax *Users`），另外还有一个 `commandId` rax 把命令名映射到命令 ID。
- **其他 rax。** `src/server.h` 的 `errors`（错误表）、`clients_index`（按客户端 ID 索引活跃客户端）、`clients_timeout_table`（阻塞客户端超时表）、`replication.c` 的 `blocks_index`（复制积压缓冲的块索引）、`src/ebuckets.c` 用 `raxNewEx(sizeof(uint64_t), NULL, EB_KEY_SIZE)` 建固定长度键的 rax 来索引集群异步迁移的桶、`src/module.c` 的 `RM_GetServerInfo` 用 rax 存放 INFO 的键值对。

**真实调用形态。** 全仓 `raxSeek` 的操作符分布是：`^` 73 次（取最小键后全量前向遍历）、`>=` 11 次、`$` 8 次、`<=` 4 次、`=` 1 次、`>` 1 次。也就是说**绝大多数用法是「从头到尾扫一遍」**，只有少数场景真的用到了有序定位。

**退化与回退路径。**

- **不支持的操作符直接失败。** `raxSeek` 的分派里没有匹配项时置 `errno = 0` 并返回 0——调用方必须检查返回值，不能假定它一定成功。
- **空树的 seek 是「成功但无元素」。** `it->rt->numele == 0` 时函数置 `RAX_ITER_EOF` 并返回 1——返回 1 不代表有元素，调用方还要看迭代器是否处于 EOF 状态。
- **查找时的内存不足。** `raxLowWalk` 若在压栈时分配失败，栈的 `oom` 标志会被置上；`raxSeek` 检查 `it->stack.oom` 后返回 0。
- **插入中途 OOM 的回滚。** `raxInsertAt` 有一条 `oom:` 标签：源码注释写明「This code path handles out of memory after part of the sub-tree was already modified. Set the node as a key, and then remove it. However we do that only if the node is a terminal node, otherwise if the OOM happened reallocating a node in the middle, we don't need to free anything.」——回滚只在停点是终端节点时做，中途失败则不释放（因为没有独立分配）。
- **link 的失效是最容易踩的坑。** `raxFindLink` 产出的 `stopnode` 与 `parentlink` 是**树内部指针**，任何一次 rax 变更都会让它们失效。源码的头文件与函数注释都反复强调「commit (or discard) immediately after the find」，并且「a second commit on the same link is undefined」。
- **随机遍历的分布是已知偏差。** `raxRandomWalk` 的注释写明「NOTE: if you use this function to generate random elements from the radix tree, expect a disappointing distribution. A random walk produces good random elements if the tree is not sparse, however in the case of a radix tree certain keys will be reported much more often than others.」——基数树上的随机游走**不是均匀采样**。

## 关键取舍

**源码事实：** 边上的字节存在父节点里，压缩节点把单子节点链合并成一段字符串。**源码事实：** 查找沿键的字节逐层下降，代价是键长。**源码事实：** 固定长度键可以省掉叶子节点，值直接内联在父节点的子指针槽里。**源码事实：** `raxInsert` 是「一次查找 + 一次提交」，返回 0 表示覆盖了旧值。**源码事实：** `raxSeek` 支持 `>`、`>=`、`<`、`<=`、`=`、`^`、`$` 七种操作符。**源码事实：** `raxFindLink` 产出的 link 在任何变更后即失效。**源码事实：** 随机游走在 rax 上分布不均。

**架构推论：** 用压缩换空间与查找步数，代价是**插入要能拆分节点**——代码复杂度显著高于哈希表，而拆分规则（源码里四种情形的图解）是最容易写错的地方。**架构推论：** 把「查找」与「插入」合成一次遍历（`raxFindLink` + `raxInsertAt`）省掉一整趟树遍历，代价是引入了「link 在提交前不得失效」这条脆弱契约——任何在两次调用之间插入其他操作的代码都会踩到未定义行为。**架构推论：** 固定长度键的叶子内联说明 Redis 里**定长二进制键是主流**（stream 消息 ID、集群桶键），针对这一形态做特化比追求通用性划算；代价是同一个 rax 实现里要同时维护压缩节点、普通节点、内联叶子三种形态，遍历代码到处要做 `raxSlotsAreValues` 判断。**架构推论：** 随机遍历分布不均这个「已知缺陷被写在注释里」的做法值得学——它把限制变成契约的一部分，而不是留作隐藏行为。主核若要做向量索引，同样要在「精确查找」与「近似最近邻」之间做选择：rax 只能做前者；ANN 类索引（hnsw 等）只能做后者且分布有偏，主核若把两者混在一个接口里，必须把这条差异暴露给调用方。AI 侧若用 Redis 的 rax 存「按前缀组织的文档块」，可以利用 `>=` / `<=` 做范围定位；但若用 `raxRandomWalk` 抽样做重算，会因为分布偏斜而反复命中少数几个前缀。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/rax.c` | `raxNew` | 创建空基数树，根节点无子节点，可选开启固定长度键的叶子内联 |
| `src/rax.c` | `raxFind` | 精确查找：沿键字节下行，返回是否命中并回填关联值 |
| `src/rax.c` | `raxInsert` | 覆盖式插入：先查找定位、再在记录的位置提交，返回 0 表示覆盖了旧键 |
| `src/rax.c` | `raxSeek` | 按比较操作符把迭代器定位到指定位置，支持大于等于、小于等于、首尾等七种语义 |
| `src/rax.c` | `raxNext` | 迭代器前进：走到下一个键，末尾时置 EOF 并与内存不足区分 |

## 参考

- [Redis 源码：rax.c](https://github.com/redis/redis/blob/unstable/src/rax.c)
- [Redis 源码：rax.h](https://github.com/redis/redis/blob/unstable/src/rax.h)
- [Redis 文档：数据类型（Stream）](https://redis.io/docs/latest/develop/data-types/streams/)

## 与主核 / AI 的关系

rax 对主核的直接用途是**有序的定长键索引**：主核的多模态表若需要「按主键范围扫描」或「按前缀定位」，基数树比 B 树更省一层间接，且查找代价直接与键长成正比。值得抄的是**「定长键特化」**这条思路——主核的记录 ID、向量 ID、文档哈希往往都是定长二进制，为它们单独做一条「值内联到父节点槽位」的路径，能省下每个键一次节点分配，在亿级条目下这是可观的差异。

另一条值得带走的是 `raxFindLink` / `raxInsertAt` 这种**「一次遍历完成查找与插入」**的写法：主核若用 B 树做索引，同样的「先定位再插入」合并能把两次树高降为一次。代价是那条脆弱契约——主核是多写者引擎，**定位与插入之间极可能被另一个写者插入**，因此这条契约在并发下必须重新设计成「持锁完成整个 find+insert」或「用带回滚日志的乐观插入」，不能照抄 Redis 的「调用方保证中间不插入」。

不可照搬的是**「用压缩节点换内存」在并发下的代价**：拆分压缩节点意味着要原地改动多个父指针，在主核的行锁粒度下这会与「只锁一行」的假设冲突；主核更适合用不可变节点 + copy-on-write 的树结构。

AI 侧要注意：rax **只能做精确匹配**，不能做相似度检索。若 AI 侧把 embedding 塞进 Redis 期待「近似查找」，用 rax 只会得到精确命中的那一条；近似最近邻必须交给专门的 ANN 索引，而这类索引普遍存在分布偏斜与召回 / 延迟的取舍。
