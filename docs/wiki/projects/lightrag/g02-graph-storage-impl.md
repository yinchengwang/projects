---
project: lightrag
feature: graph-storage-impl
title: 图存储实现（内存与外部）
page_type: feature
schema_version: 1
refs:
  - path: lightrag/kg/networkx_impl.py
    symbols: [NetworkXStorage, _walk_live_view]
  - path: lightrag/kg/neo4j_impl.py
    symbols: [Neo4JStorage, WorkspaceIndexCollisionError]
  - path: lightrag/kg/memgraph_impl.py
    symbols: [MemgraphStorage]
---

# 图存储实现（内存与外部）

## 一句话职责

三种图后端的取舍只有两条轴：**状态住在哪里**（进程内 `networkx.Graph` vs 远端 Cypher 服务器）与**并发冲突怎么处理**（拒绝提交 vs 交给数据库）——而 `NetworkXStorage` 因为把整张图放在进程内存、只用一个 GraphML 文件做跨进程同步面，成了整个仓库里**唯一声明 `requires_single_writer = True` 的存储**，于是它必须自带一套「双通道提交栅栏 + 恢复重载 + 脏图兜底」来保证不丢写。

## 机制是什么

`NetworkXStorage` 的类 docstring 把它的处境说得很清楚：整张图活在进程内存里，通过重写**一个** GraphML 文件发布；**那个文件是唯一的跨进程同步面**。

这带来一个其他后端没有的问题：`write_nx_graph` 序列化的是**整张图**。所以一个持有过期快照的写入者如果照常保存，会**用它没看见的旧状态覆盖掉对端刚刚提交的内容**。这就是「双通道栅栏」要拦的事。

两条通道（docstring 说两条都必须留，谁都不冗余）：

1. **本进程的 `storage_updated` 标志** ——它是加速通道；`True` 已经直接回答了问题。
2. **GraphML 文件的 `(st_mtime_ns, st_size)` 指纹** 与「本进程上次加载或写入时记录的指纹」比较 ——这是**权威**通道，也正是让「丢失的通知」可恢复的原因。

而这两条都属于「跨进程」通道。docstring 紧接着指出还有两个**不是通道**的条件：进程本地的恢复重载（最先测），与未决指纹（最后测）。

三档后端的对比可以直接列成表：

| 后端 | 状态位置 | 写入冲突处理 | `requires_single_writer` |
|---|---|---|---|
| `NetworkXStorage` | 进程内存 + 一个 GraphML 文件 | **拒绝提交**（decline） | `True`（唯一一个） |
| `Neo4JStorage` | Neo4j 服务器 | 交给数据库的事务与约束 | 未声明 |
| `MemgraphStorage` | Memgraph 服务器 | 同上 | 未声明 |

所以「拒绝提交」不是为了严谨，而是**内存型后端的唯一安全选择**：它没有任何机制能让自己的状态追上对端，因此一旦确认自己落后，唯一诚实的动作是不写。

## 怎么实现的

**`requires_single_writer` 是一个 `ClassVar` 而不是普通字段，而注释解释了这个细节。**

```python
requires_single_writer: ClassVar[bool] = True
```

注释：「唯一能因为对端提交而丢掉未提交修改的图存储——见契约文档。由 `LightRAG._admin_write_gate_required` 读取。`ClassVar`：这个类是 dataclass，一个普通带注解的属性会变成构造参数」。

第二条是 Python 的实情：`@dataclass` 会把每个带注解的类属性当成字段，所以一个「能力位」若不加 `ClassVar` 就会意外出现在 `__init__` 里。这个小细节在别处也出现过（`supports_strict_point_reads`、`supports_strict_point_reads`），说明作者对 dataclass 的这个坑很熟。

而这个声明的实际作用在别处：`_admin_write_gate_required` 读到它之后，**所有 admin 写都要进 `_admin_write_gate`**。所以「单写者」这个不变量不是靠约定维持的，而是靠一个能力位把写路径改道。这与 a01 里「用可实例化性当能力保证」是同一类手法：**把约束编码进配置面，而不是写进文档等人遵守**。

**类 docstring 的第二段列了「这个类只在下列规则成立时才是正确的」，四条。** 这是很少见的写法——它不描述「这个类做什么」，而描述**它的证明依赖什么**：

1. **每个工作区最多一个进程同时跑 `upsert_*` / `delete_*` / `remove_*` / `index_done_callback`。** 流水线通过它的 `busy` 预留提供这一点；admin 流程通过 `_admin_write_gate` 提供。
2. **networkx 调用是同步的**，所以它们在事件循环上不可被抢占——这就是**变更方法不持 `_storage_lock`** 的原因。唯一的例外是 GraphML 序列化，它跑在工作线程里，由 `_commit_gate` 重新建立排他。
3. **最终一致性对读者足够**：写入者修改与提交之间的读可能返回先前快照。
4. **一次提交发布整个命名空间**，所以任何写入者的刷盘也会发布其他所有写入者在它这里的待定修改，**包括只做了一半的**。

第 2 条是这一页最容易误读的地方：不加锁不是疏忽，而是「同步调用不会被事件循环打断，所以临界区天然成立」。而一旦有真异步或线程化，这个论证就崩了——所以第 2 条把例外（GraphML 序列化）单独点名并说明它靠 `_commit_gate` 顶上。**把「为什么这里可以不加锁」写成可检查的前提，比加一把多余的锁更有信息量。**

第 4 条最危险，而它说的是实情：因为提交是整个文件重写，所以「我提交了」等于「所有到此为止的修改都持久了」，包括别人做到一半的操作。这就是 docstring 后半段那条「新调用者必须做两件事」的来源。

**「新调用者必须做两件事，两件都是承重的」——这段是契约里最实用的一部分。**

> 让它的 chunk 追踪行在**变更调用之前**就持久（不是只在刷盘之前——**一个未提交的 `upsert_node` 已经待在进程级图里了，下次刷盘就会发布它**），并且跑在 admin 写闸门或流水线预留里。绕过任何一个都会触发脏图兜底，它会**大声地让提交失败而不是静默地丢写**。

第一句点出了一个非常具体的时序陷阱：通常人会觉得「我还没提交，所以我的修改是安全的」，但因为图是进程级共享的、而提交是整体发布，**未提交的修改会在别人的下一次提交里被一起发布**。所以「chunk 追踪行必须在这个变更之前落地」——否则会出现「图里已经有这个节点，但追踪表里还没有它的归属」这种半态，而那个半态会被别人发布出去。

而「大声失败而不是静默丢写」对应的是后面要讲的 `GraphMutationsDiscardedError`。

**`_walk_live_view` 只有十几行，但它把一条 networkx 的糊涂错误变成了一句可执行的话。**

```python
iterator = iter(view)
while True:
    try:
        item = next(iterator)
    except StopIteration:
        return
    except RuntimeError as exc:
        raise RuntimeError(
            f"NetworkXStorage.{method}: the graph was modified while the "
            "iterator was suspended between batches; do not let a writer "
            "run on this graph until the iteration finishes"
        ) from exc
    yield item
```

docstring 说明了它存在的理由：「`iter_labels` / `iter_edges` 必须保持有界，所以它们不能快照视图；这只能把 networkx 那句裸的『dictionary changed size during iteration』变成一条说明调用方做了什么的错误」。

两个设计点：

**第一，为什么不快照。** `list(view)` 能彻底避免这个错误，但那就是把整张图物化进内存——而 `iter_*` 方法存在的全部意义就是**不要**物化。所以「有界」与「安全」在这里冲突，作者选了有界，并用一个更清晰的错误弥补。

**第二，错误消息里那句「do not let a writer run on this graph until the iteration finishes」** ——它不只是描述现象，而是**给出约束**：批次之间让写入者跑是不允许的。原来那句 `dictionary changed size during iteration` 会让人以为是 networkx 的 bug 或自己的并发写错了；这条消息直接指向「跨批次的写入」。

这是「错误消息质量」的一个好例子：**同一个异常，能归因到具体动作时，排查时间从小时降到分钟**。

**`__post_init__` 之外的三个 `_*_context` 小方法只有一个用途：错误消息前缀。** `_node_context` 返回 `[{workspace}] node \`{node_id}\``，`_edge_context` 返回 `[{workspace}] edge \`{src}\`~\`{tgt}\``。它们存在的意思很明确——这个类抛的每一条错误都要能定位到「哪个工作区、哪个节点/边」。这在批量写入里尤其重要：一次批量 upsert 里 5000 个节点有一个失败，光说「写失败」毫无用处。

**`load_nx_graph` / `write_nx_graph` 被实现成 `@staticmethod`，而写的那一个直接调 `atomic_write`。**

```python
@staticmethod
def write_nx_graph(graph: nx.Graph, file_name, workspace="_"):
    logger.info(f"[{workspace}] Writing graph with {graph.number_of_nodes()} nodes, {graph.number_of_edges()} edges")
    atomic_write(file_name, lambda tmp: nx.write_graphml(graph, tmp), workspace)
```

所以它**继承**了 `atomic_write` 的全部保证（g01 那一页讲的三条：异常时清 tmp、硬杀留孤儿、mode 位保留），而不用自己写一遍。而那句日志把规模记下来——`number_of_nodes` / `number_of_edges` 是排障时最有用的两个数字（能立刻看出「图是不是被清空了」）。

**`_get_graph` 是这一页的核心方法，而它的四个分支的顺序是有论证的，注释逐支解释了顺序的理由。**

docstring 先定位：「这是每个公开方法获取 `self._graph` 都要经过的**唯一入口**。它也是**读者转向更新磁盘快照的唯一位置**：当另一个进程（通过 `index_done_callback`）提交后，这里的下一次调用会重新解析整个 GraphML 文件重建 `self._graph`。networkx 没有增量同步 API——重载无条件地是整个文件重载」。

四个分支依次是：

**第一支：进程本地恢复重载（最先）。**

```python
if self._recovery_reload_pending:
    sampled = self._stat_fingerprint()
    if self._count_unannounced_peer_commit_locked(sampled):
        logger.warning(f"...a notification was lost. Recovered through the file channel, folded into the recovery reload below (occurrence #{self._missed_notification_reloads} in this process).")
    logger.warning(f"...a reload this process owed itself failed earlier... Discarding them now.")
    self._reload_locked(sampled, arm_dirty_discard=False)
```

注释解释了为什么它必须最先：「它是免费的、不可能错的，而且它回答的是另一个问题：不是『对端提交了吗？』而是『**我自己的内存是不是未持久？**』」。

但紧接着有一句约束：「这个优先级**绝不能**让计数漏掉对端提交，所以下面这个分支在重载**之前**先分类」。

这就是 `_count_unannounced_peer_commit_locked(sampled)` 必须在 `_reload_locked` 之前调的原因：`_reload_locked` 会采纳文件指纹，之后就没有任何信息能判断「刚才是否也有一个未通知的对端提交」——**而那正是这条栅栏被衡量的数字**。

这两个理由合起来说明了一件事：**分支顺序同时承载两个语义 —— 「哪个条件优先」（恢复重载）与「哪个观测必须先做」（分类）。** 后者是纯粹的信息保留问题，与优先级无关，但因为都在同一个函数里，只能靠顺序兼顾。

**第二支：`storage_updated` 标志（加速通道）。**

```python
elif self.storage_updated.value:
    self._recovery_reload_pending = True
    logger.info(f"...reloading graph ... due to modifications by another process")
    sampled = self._stat_fingerprint()
    arm_dirty_discard = True
    if sampled is file_fingerprint.UNREADABLE:
        logger.debug(f"...whose identity could not be sampled; the reload below refuses rather than load blind")
    elif file_fingerprint.fence_enabled() and not (self._peer_commit_detected(sampled)):
        logger.debug(f"...names a snapshot whose identity matches the one already loaded; reloading anyway")
    self._reload_locked(sampled, arm_dirty_discard=arm_dirty_discard)
```

三个细节值得单独看：

**(a) `_recovery_reload_pending = True` 写在最前面**，注释说「从这里起就欠了，所以在路上任何东西出错之前先记下它——见 *Recovery reload*。一次完成的 `_reload_locked` 会再次清掉它」。这是「欠账先记」的模式：重载可能失败，失败了也要记得「我欠一次重载」。

**(b) 只取一次指纹**，注释说「日志行和重载共用一次采样。这个分支不计数（通知**确实**到了，所以没丢东西），但两者仍必须对看到的东西一致：采样两次会让日志声称文件没变而重载拒绝了一个读不出的文件」。

这句话很实在：**同一轮决策里的多次采样会引入不一致的可能**。而这里两者都要用同一个观测值，所以只能采一次。

**(c) `arm_dirty_discard = True` 即使在「指纹相同」的情况下也保持 `True`**，而这一段注释是全篇最长的，因为它推翻了一个早先的设计：

「这个兜底最初被规定为在这个分支里解除武装，理由是自我通知不会替换任何对端写的东西。对一个自我通知来说这是对的——**但相等的指纹并不识别出自我通知**。同 tick、同尺寸的对端提交是文件通道的已知盲点……**而标志被设置正是『这个通道此刻是盲的』的证据**：那里的 docstring 说标志存在的目的就是覆盖这个碰撞。在盲通道的裁决上解除武装会静默丢弃一张被对端替换过的脏图，而那正是兜底存在的原因。所以 fail-loud 方向胜出：最坏情况下一次真实的自我通知会付出一次被拒的提交（它清掉标志并收敛）；最好情况它抓住一次真实的丢失」。

这段是这一页最有分量的论证。它把一个「看似可以优化的小分支」还原成了**信息论上的判断**：`fingerprint == loaded_fingerprint` 有两种归因（自我通知 / 同 tick 同尺寸的对端提交），而观测无法区分它们。于是选择「假阳性一次被拒的提交」而不是「假阴性丢弃对端数据」——因为前者的代价有界且能自愈（提交失败→重试→收敛），后者的代价是永久数据丢失。

而它给出的判据「at worst ... at best ...」是可复用的：**当两个错误方向的代价不对称且无法区分时，选那个「最坏情况也有界」的**。

**第三支：文件指纹通道（权威）。**

```python
elif file_fingerprint.fence_enabled():
    sampled = self._stat_fingerprint()
    if self._peer_commit_detected(sampled):
        self._recovery_reload_pending = True
        if self._count_unannounced_peer_commit_locked(sampled):
            logger.warning(...)
```

注释里有一段自我批评，值得原文引用：这段代码「过去在条件里采样一次、在计数里又采一次，那只因为 `_storage_lock` 是跨进程的且两次之间没有 await 才成立——一条未写下来的不变量在支撑一个说『三次来自一次观测』的契约，且没有例外」。

所以改动后的做法是**一次观测喂给决策、计数、采纳、以及下面的未决指纹测试**。而单进程模式下栅栏整个关闭（`fence_enabled()` 为假），「一个分叉的文件是外部编辑，不是对端提交」，所以**根本不做 stat**。

这几句合起来是「契约一致性」的一个样板：当文档说「一次观测」，代码里就不能靠「反正中间没有 await」来偷懒——因为那条理由是隐形的，改动一引入 `await` 就崩。

**第四支：未决指纹（最后）。** docstring 说它「由文件通道在有采样时自己解决：任何具体状态相对于 `None` 都是分叉，所以它会重载」。这一支覆盖的是「文件通道采不到样」的情况，因为「一次读不出的 `stat` 会报告『无变化』并把问题留过了一次变更」。

**`index_done_callback` 有两个 `async with` 块，而注释明写「这是有意的，不要合并」。** 两个块的语义不同：

- **第一个块是拒绝路径**：本进程即将把整张图序列化到一个已经前进了的文件上。两个测试（标志 + 指纹）都在这里。注释特别指出：`storage_updated.value` 这一半在单写者不变量成立时**永久为 `False`**（这是契约文档的不变量 1），所以它是**针对绕过两道闸门的写入者的防御性脚手架**。而指纹那一半**不是**——「不变量 1 给出的是一次一个写入者，不是总是同一个进程，所以一个通知丢失的 worker 可以成为下一个合法的写入者，持着过期快照。**那就是这道栅栏存在的理由——丢失的写**，在这里被拒绝」。
- **第二个块是真正的保存 + 通知**。

这段区分很精确：**标志通道是「正常情况下永远不会触发」的，指纹通道是「正常运维下会触发」的**。而一个只看标志的实现会在通知丢失时静默丢写——这正是为什么两条都要留。

**脏图兜底（`GraphMutationsDiscardedError`）是「唯一一处丢弃变成拥有者能看见的失败的地方」。**

在任一 `async with` 之前：

```python
if self._dirty_discard_pending:
    self._dirty_discard_pending = False
    if self._graph_dirty:
        self._recovery_reload_pending = True
    raise GraphMutationsDiscardedError(f"[{self.workspace}] Refusing to save graph {self._graphml_xml_file}: a reload since the last commit discarded uncommitted in-memory mutations (a peer commit landed mid-operation), so committing now would report an operation as successful without the changes it made. The operation fails instead; its pipeline batch is reprocessed or its admin request must be retried. This is a bypass of the admin-write gate / pipeline busy reservation -- see NetworkXStorage *Dirty-graph backstop*.")
```

三个细节：

**第一，标志先清再判。** 注释：「先清掉，所以拒绝恰好发生一次：**一个粘性的拒绝会永远阻塞此后的每一次提交**（与 `_reload_locked` 对恢复标志给出的警告相同）」。所以「让拥有者看见一次失败」与「不让系统卡死」的平衡点是「精确一次」。

**第二，`_graph_dirty` 为真时再欠一次恢复重载**，注释：「丢弃性重载**之后**应用的修改属于那个即将失败的操作。欠一次恢复重载，让下一次 `_get_graph` 丢弃它们（免于重新武装这个兜底），而不是让某个后来无关的提交发布它们」。

**第三，错误消息里给出了两条恢复路径**：「这个操作失败了；它的流水线批次会被重处理，或它的 admin 请求必须重试」。所以读者不需要去读文档判断该怎么办。

**`discard_uncommitted_mutations` 是一个同步方法，而 docstring 说明了「不需要锁」的三条理由。**

```python
if not self._graph_dirty:
    return False
self._recovery_reload_pending = True
logger.error(f"...holds uncommitted in-memory mutations after {reason}, and no operation is left to publish them. Owing a reload so they are discarded instead of being published by a later, unrelated commit.")
return True
```

理由：「标志是一个普通 `bool`，闸门的 admin 锁与 `busy` 预留意味着不存在其他写入者，而并发读者只能要么重载（替我们做了丢弃）要么不重载（留给下次调用）」。

而它**不在原地重载而是「欠一次重载」**，这是「恢复重载」这条规则的另一半：**在重载成为欠账时武装，只在一次完成的重载中清除**。两个消费者都已经处理了这个情形并写在日志里：`_get_graph` 会重载并丢弃（免于脏图兜底，因为丢掉的已被报为失败），`index_done_callback` 会**拒绝**一次会发布它的提交。

**Neo4j 与 Memgraph 的差异全在「名称编码」这一层，而 `WorkspaceIndexCollisionError` 是它的失败模式。**

`Neo4JStorage` 把工作区名映射到网络图里不存在的几种标识符，每种用途各一个方法，而每个 docstring 都写明了安全边界：

```python
def _get_raw_workspace_label(self) -> str:
    """实际的 Neo4j label 名（无转义）。可以安全地绑定为查询参数……必须 NOT 直接内插进查询字符串。"""

def _get_workspace_label(self) -> str:
    """转义反引号（加倍）以防止通过 LIGHTRAG-WORKSPACE 头的 Cypher 注入……"""
```

所以是**两套方法各自对应一种使用方式**：绑定参数用原始值（驱动负责编码，无注入风险），内插进反引号标识符用转义值（转义后仍保持 1:1 映射）。

而 `_normalize_index_suffix` 是第三套，也是**有损的那一套**：

```python
normalized = re.sub(r"[^A-Za-z0-9_]+", "_", workspace_label).strip("_")
if not normalized: normalized = "base"
if not re.match(r"[A-Za-z_]", normalized[0]): normalized = f"ws_{normalized}"
```

三步：非字母数字下划线全换下划线、去首尾下划线、首字符不能是数字（否则加 `ws_` 前缀）。

**有损就意味着碰撞。** `WorkspaceIndexCollisionError` 的 docstring 把因果说全了：

> `_get_fulltext_index_name` 是有损的，所以不同的工作区标签（例如 `foo-bar` 与 `foo_bar`）会解析到同一个索引名。在索引初始化时抛出，使实例**拒绝启动**而不是复用、或者删掉重建另一个工作区的索引。**永远不要捕获它继续**；改名其中一个工作区。

三句话分别是：原因（有损规范化）、动作（拒绝启动）、以及**为什么不能吞掉**（继续意味着要么复用别人的索引——数据串了，要么删了重建别人的——数据没了）。

这是「命名冲突当作特性而不是缺陷」的范例，与 a01 里「容器名编码模型」是同一条思路：**用命名做隔离，把语义错误转成命名冲突，因为命名冲突是显式的**。而这里额外补了一层——**冲突时拒绝启动而不是自动解决**，因为自动解决的两种可能都不可接受。

而 `_LUCENE_RESERVED` 那段是另一个同类问题：全文查询解析器在分析器之前解释这些字符（例如 `-` 当 NOT），所以「一个裸的 `tb-` 会变成一个 NOT 从句并静默匹配不到东西」。解法是替换成空格，「因为索引本来就在这些边界上分词」。

「静默匹配不到」是这类问题的典型症状：**查询语法与用户输入共享一个字符集，而用户输入从不转义**。全文检索里这是通病，判别方法是「用户输入里出现语法字符时结果会怎样」——如果答案是「结果变少但没报错」，就是这个问题。

**Memgraph 与 Neo4j 的骨架几乎逐字相同**：`__init__` 里同样的 env 覆盖优先级（`MEMGRAPH_WORKSPACE` → 参数 → `'base'`）、同样在 `super().__init__()` 之后才记日志（因为 `self.workspace` 那时才初始化）、同样一对 `_get_raw_workspace_label` / `_get_workspace_label`。差别在驱动与方言。

这个重复本身是信息：**两个后端共享「工作区 → label」的映射契约与它的安全规则，只是换个数据库实现它**。所以这一组的真正差异不在三个类之间，而在「内存型 vs 服务器型」这一刀——服务器型把并发交给数据库，因此不需要栅栏、不需要恢复重载、不需要脏图兜底。

## 关键取舍

**源码事实：** `NetworkXStorage` 声明 `requires_single_writer: ClassVar[bool] = True`，是全仓唯一一个，由 `LightRAG._admin_write_gate_required` 读取以把 admin 写改道进闸门。

**源码事实：** 类 docstring 列出「这个类只在下列规则成立时正确」四条，其中一条是「networkx 调用同步故不可被事件循环抢占」——这就是变更方法不持 `_storage_lock` 的论证，而唯一的例外（GraphML 序列化跑在工作线程）由 `_commit_gate` 顶上。

**源码事实：** `_get_graph` 有四个分支，顺序同时承载两个语义：恢复重载最先（免费且不可能错），而「未通知对端提交」的计数必须在 `_reload_locked` **之前**做（重载会采纳指纹，之后无法判断）。

**源码事实：** `storage_updated` 标志通道在单写者不变量成立时永久为 `False`，因此是防御性脚手架；**指纹通道才是生产里活的**，因为「一次一个写入者」不等于「总是同一个进程」，通知丢失的 worker 会成为下一个合法写入者并持着过期快照。

**源码事实：** `arm_dirty_discard` 在「指纹相同」时仍保持武装，因为相等的指纹**不识别**自我通知——同 tick 同尺寸的对端提交是文件通道的已知盲点，而这正是标志存在的目的。注释明确论证了「最坏情况有界、最好情况抓住真实丢失」。

**源码事实：** `_dirty_discard_pending` 先清后判，使拒绝**恰好发生一次**，避免粘性拒绝永久阻塞此后每次提交。

**源码事实：** `GraphMutationsDiscardedError` 是唯一一处「丢弃」被升级为拥有者可见失败的位置，错误消息里给出两条恢复路径（批次重处理 / admin 重试）。

**源码事实：** 恢复重载是「欠账」：武装于重载成为欠账时，只由一次完成的重载清除；`discard_uncommitted_mutations` 是同步的且不取锁，理由是「标志是普通 bool + 单写者不变量保证没有其他写入者」。

**源码事实：** `_walk_live_view` 不物化视图（`iter_*` 的存在意义就是有界），而是把 networkx 的 `dictionary changed size during iteration` 换成一条指明「批次之间不要放写入者进来」的错误。

**源码事实：** `Neo4JStorage` 与 `MemgraphStorage` 各有一套值域映射：原始 label（绑参数用）、转义 label（反引号标识符用，双写反引号防 Cypher 注入）、有损规范化的索引后缀。

**源码事实：** `WorkspaceIndexCollisionError` 在索引初始化时抛出使实例**拒绝启动**，docstring 明确「永远不要捕获它继续」——因为继续的两种可能（复用别人的索引 / 删掉重建别人的）都不可接受。

**源码事实：** `_LUCENE_RESERVED` 替换全文查询里的保留字符，因为解析器在分析器之前解释它们，一个裸 `tb-` 会变成 NOT 从句并静默匹配不到。

**架构推论：** 「内存型 + 单文件发布」这个组合逼出了一整套分布式协议，而这套协议的核心不是「保证不冲突」，而是**「冲突时如何诚实失败」**。它从不试图合并或修复：落后就拒绝、丢弃就抛错、通知丢失就靠指纹兜。这是唯一自洽的选择——内存型后端没有任何手段让自己的状态追上对端，所以「假装能继续」等于静默丢数据。

**架构推论：** 代码里反复出现「一次观测喂给所有决策」这条纪律（`_get_graph` 的三支、`index_done_callback` 的拒绝路径），并伴随对过去违反它的自我批评。这说明**在一个到处是跨进程读的类里，「同一个事实被读两次」是一个系统性风险**——两次读之间文件可能变、Manager 代理可能超时、指纹可能不可读。纪律「一次观测」把这类风险压成零，代价是所有消费者必须接受同一个快照。

**架构推论：** 「拒绝提交」与「重载重放」是两种相反的冲突策略，而这一组后端恰好两派都有：图是拒绝派，向量是重放派（见 g03）。判据不在实现难度，而在**「重放是否可定义」**：向量 upsert 可以重放（把缓冲的向量重新加进新快照），而图变更做不到——一次 `upsert_node` 之后又 `delete_node`，重放两者的顺序与语义都没有定义。所以「拒绝」不是保守，而是**当重放语义不存在时唯一正确的动作**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/kg/networkx_impl.py` | `NetworkXStorage` | 进程内存 + 单 GraphML 文件，唯一 `requires_single_writer` |
| `lightrag/kg/networkx_impl.py` | `NetworkXStorage.requires_single_writer` | 能力位，把 admin 写改道进 `_admin_write_gate` |
| `lightrag/kg/networkx_impl.py` | `NetworkXStorage._get_graph` | 四分支：恢复重载 / 标志 / 指纹 / 未决指纹 |
| `lightrag/kg/networkx_impl.py` | `NetworkXStorage.index_done_callback` | 两段式：先拒绝路径再保存路径 |
| `lightrag/kg/networkx_impl.py` | `NetworkXStorage.discard_uncommitted_mutations` | 同步「欠一次重载」，不取锁的三条理由 |
| `lightrag/kg/networkx_impl.py` | `NetworkXStorage._recover_from_failed_save_locked` | 保存未落地后从文件恢复进程视图 |
| `lightrag/kg/networkx_impl.py` | `_walk_live_view` | 不物化视图，把并发修改错误换成约束性消息 |
| `lightrag/kg/networkx_impl.py` | `write_nx_graph` | 走 `atomic_write`，日志记节点与边数 |
| `lightrag/kg/networkx_impl.py` | `_node_context` | 节点写的错误消息前缀 |
| `lightrag/kg/neo4j_impl.py` | `Neo4JStorage` | 服务器型，并发交给数据库 |
| `lightrag/kg/neo4j_impl.py` | `WorkspaceIndexCollisionError` | 有损索引名碰撞时拒绝启动 |
| `lightrag/kg/neo4j_impl.py` | `_get_workspace_label` | 双写反引号防 Cypher 注入 |
| `lightrag/kg/neo4j_impl.py` | `_normalize_index_suffix` | 三步有损规范化，首字符非字母加 `ws_` |
| `lightrag/kg/neo4j_impl.py` | `_LUCENE_RESERVED` | 全文查询保留字符，防静默匹配不到 |
| `lightrag/kg/neo4j_impl.py` | `_CJK_BIGRAM_PATTERN` | CJK 双字索引，查询需同时发原串与前缀 |
| `lightrag/kg/memgraph_impl.py` | `MemgraphStorage` | 与 Neo4j 同骨架换驱动，env 覆盖优先级相同 |

## 参考

- [lightrag 源码：kg/networkx_impl.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/kg/networkx_impl.py)
- [lightrag 源码：kg/neo4j_impl.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/kg/neo4j_impl.py)
- [NetworkX 文档](https://networkx.org/documentation/stable/)
- [Neo4j Cypher 手册](https://neo4j.com/docs/cypher-manual/current/)

## 与主核 / AI 的关系

主核里最接近的是**单写者的 WAL 与 checkpoint**：只有一个写入者时不需要复杂的并发控制，但必须保证「提交要么全落地要么全不落地」，且中途崩溃后能判断出「哪些修改没落地」。

`NetworkXStorage` 的处境在结构上完全一样，但手段相反：主核靠 WAL 的物理顺序与 LSN 判断，这里靠**文件指纹**判断。指纹方案的优点是不需要任何额外的持久结构（只要一个 `(mtime_ns, size)`）；代价是它的分辨率有限——注释里承认的「同 tick 同尺寸的对端提交」就是分辨率不足的具体表现，而应对方式居然是**故意保留一个假阳性**。

这与 a01 的分工：那一页讲「四类契约无事务」，这一页讲「在无事务前提下，内存型后端如何把冲突变成显式失败」。而与本组 g01 的对照最直接：**键值后端用共享内存消除了冲突这个问题本身**，图后端用指纹栅栏**把冲突暴露出来**。两者是同一条不可能三角上的两个顶点（共享内存 / 私有副本 + 冲突检测），没有哪个更「对」。

对 AI 工程最实用的一条是**「未提交的修改会被别人的提交一起发布」**。这在任何「进程级共享状态 + 整体快照提交」的系统里都成立，而它推翻了一个很强的直觉（「我还没提交，所以我的改动还不存在」）。正确的做法是把这个事实显式化：先让「归属/索引」这类必须与图同步的记录落地，再做图变更——顺序反了就会产生「图里有、追踪里没有」的半态，而那个半态会被下一个无关提交发布出去。

另一条是 **`arm_dirty_discard` 那段论证的判据**：当观测无法区分两种归因（自我通知 / 对端提交），而两个错误方向的代价不对称时，**选「最坏情况有界」的那个**。假阳性（一次被拒的提交，重试即愈）远优于假阴性（静默丢弃对端数据）。这个判据在任何「用弱信号做安全决策」的地方都适用。

最后一条是 `WorkspaceIndexCollisionError` 的「永远不要捕获它继续」。一个「拒绝启动」的错误看起来粗暴，但它把「数据可能已经串了」这件事变成了一次可见的停机。相比之下「自动解决冲突」在这类场景里几乎总是错的，因为自动解决需要知道用户意图，而系统只知道字符串。
