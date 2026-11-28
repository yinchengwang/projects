---
project: lightrag
feature: vector-storage-impl
title: 向量存储实现
page_type: feature
schema_version: 1
refs:
  - path: lightrag/kg/faiss_impl.py
    symbols: [FaissVectorDBStorage]
  - path: lightrag/kg/nano_vector_db_impl.py
    symbols: [NanoVectorDBStorage, _sanitize_record_value]
  - path: lightrag/kg/milvus_impl.py
    symbols: [MilvusVectorDBStorage, MilvusIndexConfig, get_collection_gate]
---

# 向量存储实现

## 一句话职责

三种向量后端按「状态住在哪里」分成两派：Faiss 与 Nano 把索引放在进程内存、用一个（或一对）文件做同步面，因此必须处理「我是不是落后了」；Milvus 把状态交给服务器，因此**不需要栅栏**——但需要另一套东西：一个「读者租约 + 丢弃屏障」来保证重建集合时不会让在飞的读撞上「集合不存在」。

## 机制是什么

三个后端的取舍可以列成一张表，而核心那一列是**写冲突策略**：

| 后端 | 状态位置 | 持久化 | 写冲突策略 | 嵌入时机 |
|---|---|---|---|---|
| `FaissVectorDBStorage` | 进程内存（`_index` + `_id_to_meta`） | 两个文件（`.index` + `.meta.json`） | **重载并重放** | 延迟到刷盘 |
| `NanoVectorDBStorage` | 进程内存（`NanoVectorDB` 实例） | 一个 JSON 文件 | **重载并重放** | 延迟到刷盘 |
| `MilvusVectorDBStorage` | Milvus 服务器集合 | 服务器自动 | 交给服务器 | 延迟到刷盘 |

前两个的类 docstring 都用同一句话点明了策略，且都显式与图存储对照：

- Faiss：「像 `NanoVectorDBStorage` 而不像图存储，**这个后端从不拒绝过期写入**——它重载并重放。」
- Nano：「不像图存储，这个类从不拒绝过期写入：写路径是『先重载再重放』，所以两边都活下来，没有什么可报成失败的。**`LightRAG._flush_storages` 的 decline 传播在这里没有对应物**。」

所以这里有一个直接对立的设计对：**图存储拒绝（g02），向量存储重放。** 而 Nano 的 docstring 顺带解释了为什么可以这样——「两边都活下来，没有什么可报成失败的」。重放可行的前提是「操作能被重新施加到新快照上」，而这一点在向量上成立（按 id 的 upsert 与按 id 的 delete 都是幂等的），在图上不成立（`upsert_node` 之后 `delete_node` 的顺序与语义无法重放）。

## 怎么实现的

**两个文件型后端都采用「延迟嵌入」：`upsert` 不嵌入，只缓冲。**

Faiss 的 docstring 把三条一起说了：

> 嵌入**不在**这里做。每条记录以 `vector=None` 缓冲进 `self._pending_upserts`，嵌入模型在刷盘时（`index_done_callback` / `finalize` 里的 `_flush_pending_locked`）**每个 id 调一次**。这把同一 id 的重复 upsert 与许多小的 upsert 调用合并成一次嵌入 pass。

而「读你的写」被精确限定了范围：「直到刷盘前，一个被 upsert 的 id 只能通过 read-your-writes 的读路径观察到，**不能通过 `query`**」。

最后这半句是这一层最容易被忽略的语义：**缓冲的向量不可被检索**。它不是一个「实现不完整」的问题，而是一个明确的选择——因为 `query` 走的是索引结构，而缓冲里的向量还没进索引。若要让 `query` 看到它，就得在每次 `query` 前刷盘，那整个延迟机制就没意义了。

而 Nano 说得更直接：「`query` 与 `client_storage` 只看已物化的数据，所以未刷盘的行**故意不可检索**」。

`upsert` 的实现里有一段关于「时间戳与写序」的注释，两个文件型后端逐字相同：

```python
current_time = int(time.time())
write_seq = next_write_seq()
```

注释解释：「整批一个时间戳、一个写序 token：两者都为**写**定序，而重做日志的比较只会把来自两次不同写出的行并排放。**token 打破 `__created_at__` 留下的整秒并列**」。

这句话点出一个具体问题：`int(time.time())` 的分辨率是秒，而一次批量 upsert 里很多行会共享同一秒。当重放阶段要判断「哪一行更新」时，秒级时间戳无法区分，所以需要一个独立的单调递增序号。

**两个后端的 `upsert` 都做同一件抵消动作：丢弃同 id 的待定删除。**

```python
self._pending_deletes.discard(doc_id)
self._pending_upserts[doc_id] = _PendingFaissDoc(record=record)
```

Faiss 的注释解释了理由：「一次新的 upsert 取代同 id 的排队删除：刷盘时先应用删除再物化新行，所以再应用那次删除是多余的工作」。而 Nano 的版本还多一句关于重做日志的：

> 这里**故意不碰**重做日志：这一行可能永远不物化（一个中止的批次会丢掉它），而指纹已经让一次重放不会落到同 id 写出的行上。

所以「缓冲」与「重做日志」是两个不同生命周期的结构：缓冲是「还没进索引的意图」，重做日志是「已经进索引但还没发布的既成事实」。

**`drop_pending_index_ops` 的 docstring 是整组 G 里最值得读的一段，因为它解释了一个「故意不做的回滚」。**

Faiss 的版本先说清范围：

> 只丢弃缓冲的 upsert；已经被先前的 `_flush_pending_locked` 物化进 `self._index`、而随后保存步骤失败的向量（`_index_dirty=True`）**被有意地不回滚**。

然后给了四条理由：

1. **流水线把每个文件当作原子单元**：中止会把受影响的文档标成 FAILED，整个文件在下次运行时重处理。
2. **upsert 的键是确定性 id**（实体名 / 关系 / 块哈希），重处理会幂等地覆盖这些向量，所以「最终状态与是否回滚无关」。
3. **这与服务器型后端一致**（Milvus / OpenSearch / Postgres / Mongo / Qdrant），它们同样在 abort 时保留兄弟刷盘已提交的部分数据。
4. **而且如果进程在下次保存前崩溃，这些内存里的写反正也会丢。**

结论：「只回滚 FAISS/Nano 会增加一个不一致的、非承重的『FAILED == 干净』保证，所以被**故意省略**」。

这四条论证的结构很值得学：**先证明「这个回滚不影响最终状态」（1、2），再证明「不做它才是一致的」（3、4）。** 而不是简单说「懒得做」。

而删除被沿同一条线切成两半，docstring 写得很清楚：

> `_pending_deletes` 是缓冲的工作，被丢弃；`_unsaved_deletes` 是**已经到达** `self._index` 的删除的重做日志，所以它被**保留**——丢掉它既不能恢复那些行也不能让它们保持消失，只会让删除重新变得脆弱，而那正是这个日志存在要结束的状态。

这个区分很精确：**「缓冲」丢掉了可以重来，「已达成的事实」丢掉了就丢了记录**。而保留重做日志的收益是「下次重放还能把它再删一次」——幂等操作保留日志是安全的。

**`_load_faiss_index` 的 docstring 记录了一次真实的 bug 修复，而它的写法是这一页最有价值的部分。**

> **Fail-loud。** 一个存在但读不回来的文件对会抛 `CorruptStorageSnapshotError`；其他每个失败按它自己传播。**这里没有任何东西降级成空索引，而这正是这个方法过去对任何异常都做的事。那个降级是延迟发生的静默数据丢失：空索引把自己认证成这个进程的嵌入空间，然后下一次 `index_done_callback` 把它保存到它读失败的那些字节之上——把一个不可读的文件永久变成一个空文件，还在上面盖了一个新基线。拒绝则把那些字节留在磁盘上，留给离线重建，而那是唯一被允许销毁它们的路径。**

这段话是「错误处理的代价分析」的极好样本。它把一次「降级到空」的后果链完整展开：

1. 读失败 → 降级成空索引（看起来无害，程序继续跑）
2. 空索引被**认证**为「本进程的嵌入空间」
3. 下一次提交把这个空索引**写回**它读失败的那份文件上
4. 结果：不可读 → **永久空**，且盖上了新基线（于是「之前有没有数据」这件事也查不出了）

所以「优雅降级」在这里恰好是**最坏的选项**：它把一次可恢复的读失败变成一次不可恢复的数据销毁。而 fail-loud 的收益是「字节还在磁盘上」，留给唯一被允许销毁它们的路径（离线重建）。

**同时它也精确划出了「什么不算损坏」的边界**：文件不存在不是损坏（那是首次启动）；不可读的 `.space.json` 标记不是证据（`_read_vector_space_file` 会吞掉它）；但「索引存在而旁边的 `.meta.json` 不存在」是**撕裂的一对**，因为「元数据是这个存储的提交标记（`_fingerprint_paths`），所以它的缺失与存在的索引矛盾」——因此报成损坏，让运维走同一条恢复路径。

这三条边界合起来说明：**「缺失」与「损坏」必须分开判，而判据是「这个缺失是否与别的事实矛盾」**。索引在而元数据不在 → 矛盾 → 损坏。两者都不在 → 一致 → 首次启动。这是一个可复用的判据。

而 `_load_faiss_index` 里还有一段关于**给谁看**的细节：

```python
reading = self._faiss_index_file
...
reading = self._meta_file
```

注释：「`container` 是运维者的**诊断**——那个读不出来的文件——而多数解析失败无法事后归因：`json.JSONDecodeError` 不带 `filename`，所以一个满是垃圾的 `.meta.json` 否则会被报成那个完好的 `.index`，把运维者送去检查错的产物。**恢复范围不受影响**：`artifacts` 总是点名全部三个文件」。

所以这里维护了两个变量：一个用于「报错时说是哪个文件」（诊断），一个用于「恢复时该动哪些文件」（范围）。诊断要精确到文件，范围要覆盖全组——两个需求不同，所以不能共用一个值。

**跨文件原子性在 Faiss 上是明确不保证的，而处理方式分三档。**

docstring：

> **跨文件原子性不保证。** 两个 `atomic_write` 重命名是独立的，所以它们之间崩溃会让这对文件描述不同的快照。`_load_faiss_index` 静默丢弃 `meta > index` 的行，并对 `index > meta` 只警告不修复，所以孤儿向量留在加载的索引里，按 custom-id 查不到。**一对完全读不回来的文件是性质不同的**：它抛 `CorruptStorageSnapshotError` 且从不降级成空索引，因为下一次保存会把这个空发布到那些读不出的字节之上。

三档对应三种不一致程度：可容忍（多出来的元数据行，丢弃）、可警告（多出来的向量，留着但查不到）、不可接受（完全读不回）。

第二档的选择值得单独想：`index > meta` 时**不修复也不丢弃**，只是警告。理由是修复需要知道哪些向量是孤儿（那要全扫索引），而丢弃整个索引代价太大。所以「留着但不可达」是这两者之间唯一便宜的中间态——它不消耗额外工作，只留下一点空间浪费。

**`MilvusVectorDBStorage` 的复杂度全在「读者租约 + 丢弃屏障」，而 `_CollectionGate` 的 docstring 解释了为什么需要一个「租约」而不只是一个标志。**

> 两半合起来才构成保证：读者在每次阻塞 SDK 调用期间持有一个**租约**，而重建先**关闭**闸门（不再授予新租约），然后等待在外的租约排空。**只用标志做不到**——在读者检查标志与它的调用抵达线程池之间，重建可以把集合从底下删掉。

然后它精确限定了租约的范围：

> 租约跨一次提交，**从不跨整个方法**：它覆盖等待池许可与 SDK 往返，别的什么都不覆盖。那个边界正是这件事之所以可行的全部原因——**重建从不等待读者碰巧夹在两次调用之间的嵌入往返**，而那才是一个排空在破坏性路径上可行的原因。

这两段合起来是一条很实用的原则：**锁的范围要按「资源真正被使用的窗口」算，而不是按「方法调用」算**。若租约覆盖整个 `query`（含嵌入调用），那一次慢的嵌入会让 `drop()` 等上几十秒，而 `drop()` 又持有 `_flush_lock` 阻塞所有写——于是整个工作区卡在一次嵌入上。

紧接着还有一条诚实的补充：

> 不过也不**只**是对服务器执行中的调用：一个持有租约但还在等 `MILVUS_SUBMIT_LIMIT` 许可的读也持着它，所以重建还要等闸门关闭那一刻排着的积压。**有界且不增长**——关闭的闸门不放新读者进来，积压按全池宽度排空——但在饱和的池子上那是几次往返，不是一次。

这一段是「不把话说满」的范例：它承认了保证有比理想情况更大的等待，同时给出了「有界且不增长」这个可验证的性质。而最后它还写明了「为什么不去优化这一点」：要精确知道提交时刻需要让 `bounded_submit` 提供回调，而「它故意不声明自己的关键字参数；那个接缝得在 `lightrag/utils.py` 里切，为每个共享它的池子」。

**`acquire_read` 的两步之间不能有 `await`，这是正确性的关键。**

```python
async def acquire_read(self) -> None:
    while True:
        await self._open.wait()
        self._leases += 1
        self._idle.clear()
        if self._open.is_set():
            return
        self.release_read()
```

docstring：「`acquire_read` 先取租约然后**重新检查**闸门，若重建在这期间关闭了就把它交回去。**两步之间没有 `await`**，所以重建要么看到那个租约（并等它），要么看到一个已经退让的读者——**绝不会看到一个它错过的租约**。」

这个「取完再查、中间不 await」是一个经典的 double-check 模式，而这里的要点是：在单线程事件循环里，「两步之间不 await」等价于「原子」。所以这里不需要锁，只需要保证没有让步点。

**`get_collection_gate` 的注册表按事件循环分域，而 docstring 解释了为什么必须这样做。**

```python
loop = asyncio.get_running_loop()
with _COLLECTION_GATES_GUARD:
    for stale_key, (stale_loop, _) in list(_COLLECTION_GATES.items()):
        if stale_loop.is_closed():
            _COLLECTION_GATES.pop(stale_key, None)
    key = id(loop)
    entry = _COLLECTION_GATES.get(key)
    if entry is None or entry[0] is not loop:
        entry = (loop, {})
        _COLLECTION_GATES[key] = entry
```

docstring 三条：

1. **「必须从将要 await 它的那个循环内部调用——绝不要缓存在实例上，实例比任何一个循环活得久（在同一个 `LightRAG` 对象上连续调 `asyncio.run()` 是这件事的常规发生方式）」。**

2. **「条目故意持有对循环的强引用：一个从未需要阻塞过的事件永远不会学到自己属于哪个循环，于是没有东西可以测 `is_closed()`，也没有办法检测 `id()` 重用」。**

3. **「关闭的循环在下一次调用时被清扫，与 `_fallback_semaphore` 相同」。**

第 2 条是这一页最精细的一处：`id(loop)` 会被重用（CPython 的对象地址复用），所以只按 id 存会误命中一个已死的循环。解法是同时存强引用并比较 `entry[0] is not loop`——**身份比较而不是 id 比较**，id 只用来做哈希桶。

而「`asyncio.Event` 若从未阻塞过就不绑定循环」这一点更微妙：`asyncio.Event` 在 Python 3.10+ 之前会在构造时抓当前循环，新版本改成首次使用时才绑定。所以一个「从未 wait 过」的 Event 不知道自己属于哪个循环，也就无法被判定失效。**强引用是唯一的补丁**：只有把循环对象留住，才能反过来问它 `is_closed()`。

**Milvus 的 `upsert` 与两个文件型后端几乎逐字相同，但多了一处 VARCHAR 截断的注释。**

```python
# _sanitize_varchar_fields already byte-truncates the stored
# `content` when it is a meta field; the pending doc keeps the full
# untruncated text so the embedding sees the complete chunk.
source = self._sanitize_varchar_fields({...})
pending_docs.append((k, _PendingVectorDoc(source=source, content=v["content"])))
```

所以同一个 `content` 被用了两次、取了两种形态：`source` 里的是**按 Milvus 字段上限截断过的**（要写进数据库），而 `content` 字段是**完整原文**（要送去嵌入）。这个区分是必需的——若嵌入用的是截断后的文本，那么超过上限的块会永远检索不准，而且没有报错。

**`MilvusIndexConfig` 是「环境变量 + 参数」两级配置，而它的 `metric_type` 字段是一个保留的兼容位。**

```python
metric_type: Optional[str] = None  # Deprecated: always resolved to COSINE
```

docstring：「`metric_type` / `MILVUS_METRIC_TYPE` 已废弃且被忽略：度量总是 `VECTOR_METRIC_TYPE`（COSINE），非 COSINE 的值只记一条警告。**字段保留是为了让传它的调用方继续工作**」。

而 `__post_init__` 里的处理是：

```python
requested_metric = self.metric_type or os.environ.get("MILVUS_METRIC_TYPE")
_warn_ignored_metric_type(requested_metric)
self.metric_type = VECTOR_METRIC_TYPE
```

即「读它、警告它、然后覆盖它」。而 `index_type` 是另一个模式：

```python
self.index_type = (self.index_type or os.environ.get("MILVUS_INDEX_TYPE", "AUTOINDEX")).upper()
```

参数优先于环境变量，然后 `upper()` 规范化。

两层配置的写法在这里很统一：**每个字段都是「参数 → 环境变量 → 默认值」，且都用 `is None` 判断而不是真值判断**（`if self.hnsw_m is None: self.hnsw_m = _get_env_int(...)`）。这一点与前几组里 `description if description is not None else docstring` 是同一条纪律——用 `is None` 才能让「显式传 0/空」生效。

**`drop()` 的注释是整组 G 里最长的，因为它要解释「为什么重建必须是一次提交」。**

> **取消：重建不可中断**，任何取消——调用方自己的，或关闭时取消所有任务——都不会在线程池完成之前返回这个方法。两样东西承载这一点，**且单独任何一个都不够**：它是**一次**提交而不是一串 await（见 `_rebuild_collection`），并且它是从调用方自己的任务里**不可中断地**提交的（见 `run_in_milvus_executor_uninterruptible`），所以 `_flush_lock`、关闭的读者闸门与调用方自己的 `destructive_busy` 预留**都恰好被持有到它们排除的工作跑完**。之后取消被重抛并打上标记，让重写它的调用方知道这次丢弃确实落地了。

而 `_rebuild_collection` 的 docstring 补充了「为什么 awaiting 不构成保护」：

> 不是四次提交：一个在它们之间被取消的任务会让命名空间没有集合，而运行期没有任何东西重建它。**awaiting 不是保护这个序列的东西**——一次取消所有待处理任务的关闭（`asyncio.run` 的 `_cancel_all_tasks`、ASGI 拆卸）**直接**抵达内部任务，而直接取消是一次 `_wait_deferring_cancellation` 会重抛而不是延后的东西。一旦它被提交，池线程把它带到底，不管循环做什么，**而那是这个保证唯一能采取的形式**。

这段区分很关键：「外层 await 让取消点在提交之间」这种想法只在「取消通过 await 传递」时成立。而 `_cancel_all_tasks` 是**直接 cancel 每个任务**，绕过 await 链。所以唯一可靠的做法是「把整个序列放进一个同步函数，提交到线程池一次」——线程池里的同步代码没有取消点。

而「顺序：备份先走」也有明确理由：

> 备份先走。在它们之后崩溃会留下活跃集合仍被填充且没有东西可以从它里面重新填充，重跑就会清掉；反过来崩溃会留下一个自由名旁边一个备份，下次 `initialize()` 会迁移或……。

所以顺序的依据是「崩溃后哪种状态更容易修」：先删备份 → 崩溃后是「活跃集合还在，只是没备份」，重跑即可；先重建活跃集合 → 崩溃后是「活跃集合空了，但备份还在」，下次 initialize 会把它迁回来，那可能是**不该发生**的恢复。

**`_run_gated` 里有一段关于「为什么必须给每次提交加闸而不是每个方法加闸」的论证，而它指向一个具体的静默失败。**

> 每次提交加闸而不是每个方法加闸才是要点：一个在入口通过了一次检查的读者随后可能坐在一次嵌入往返里好几秒，而在这段空隙里启动的 drop 会让它身后的那次检查变得毫无价值。

紧接着是那个具体失败：

> 没有它，一个在 `drop()` 移除集合后发出的读会失败请求——而 `get_by_id`，它的 `except Exception` 返回 `None`，**会把一个缺失的集合报成一个缺失的行**，而那是 `AGENTS.md` 的 *Consistency without transactions* 禁止的静默失败。

「把集合不存在报成行不存在」是这类 `except Exception: return None` 的通病：它把「基础设施故障」与「数据确实不存在」合并成一个值。而 `_CollectionGate` 的存在部分是为了让这种合并**不会发生**——因为闸门保证读要么成功要么在等待，而不是撞上缺失的集合。

而 docstring 还写明了写者**不能**用这个闸门：「写者跑在 `_flush_lock` 下，而 `drop()` 在整个重建期间持有它，所以它们已经被排除——而 `drop()` 自己（它关闭这个闸门）会等待自己」。

**`NanoVectorDBStorage._sanitize_record_value` 是「只剥掉必定写不进去的东西」的精确实现。**

```python
if isinstance(value, str):
    if not _LONE_SURROGATE_PATTERN.search(value):
        return value
    return _LONE_SURROGATE_PATTERN.sub("", value)
```

docstring 给了两条理由，第二条尤其重要：

> 命名空间文件由外部 `NanoVectorDB.save` 用 `json.dump(..., ensure_ascii=False)` 写进一个 UTF-8 文件，所以记录里任何位置的孤立代理项都会让提交抛 `UnicodeEncodeError`——**而被污染的行还留在缓冲里**，所以那个命名空间此后每次提交都以同样方式失败，直到这行被删掉。

> 只移除保存无法编码的东西。**其他每个字符，包括 U+FFFE / U+FFFF，过去都被原样持久化，所以必须保持原样：一个向量记录的 `entity_name` / `src_id` / `tgt_id` 是作为图键被查找的，重写一个可编码字符会让记录与它的图对象脱钩。**

第二条是这一页最实用的一个判断：**净化函数的作用域必须精确等于「会导致失败的那个集合」，任何超出都是数据破坏**。因为这里的字符串是**跨存储的键**（向量记录的 `entity_name` 指向一个图节点），改动一个字符就等于指向了另一个（不存在的）节点。而「净化得更多一点更安全」这种直觉在这里正好是错的。

同时第一条揭示了一个「毒行」现象：**一行坏数据会让整个命名空间的提交永久失败**，因为它在缓冲里、下次还会被写、还失败。这解释了为什么必须在 upsert 时而不是保存时净化——保存时净化已经晚了（失败已经发生），且需要在失败后从缓冲里剔除那一行。

## 关键取舍

**源码事实：** Faiss 与 Nano 在写冲突上**重载并重放**，与图存储的「拒绝」相反；Nano docstring 明确「`_flush_storages` 的 decline 传播在这里没有对应物」，因为这个策略下「两边都活下来，没有什么可报成失败的」。

**源码事实：** 两个文件型后端的 `upsert` 都不嵌入，只缓冲 `vector=None` 的记录，嵌入在刷盘时**每 id 一次**；缓冲期间该 id 只能通过 read-your-writes 路径可见，**`query` 看不到**。

**源码事实：** `upsert` 用「一个整秒时间戳 + 一个独立写序 token」，因为 `__created_at__` 的秒级分辨率无法为同批行定序，而重做日志比较会把两次写出的行并排。

**源码事实：** `upsert` 会 `discard` 同 id 的待定删除，但对重做日志**故意不动**——因为它可能永不物化，而指纹已能防止重放到同 id 的行上。

**源码事实：** `drop_pending_index_ops` 只丢缓冲 upsert/delete，**不回滚**已物化的向量；四条理由是「流水线把文件当原子单元 + id 确定性使重处理幂等 + 与服务器型后端一致 + 崩溃本来也会丢」。

**源码事实：** `_load_faiss_index` 过去对任何异常都降级成空索引，那是「延迟的静默数据丢失」——空索引认证自己、然后被写回它读失败的字节之上。现在一个读不回来的文件对抛 `CorruptStorageSnapshotError`。

**源码事实：** 「文件缺失」不算损坏（首次启动），「不可读的 `.space.json`」不算证据，但「索引在而 `.meta.json` 不在」是撕裂对（元数据是提交标记）→ 报损坏。

**源码事实：** Faiss 跨文件原子性不保证，三档处理：`meta > index` 静默丢弃、`index > meta` 只警告不修复、整对读不回则抛错。

**源码事实：** `_CollectionGate` 用「租约 + 关闭 + 排空」而非单一标志，因为「检查标志」与「调用抵达线程池」之间重建可以删掉集合；租约只跨一次提交（覆盖等许可与往返），不跨整个方法。

**源码事实：** `acquire_read` 取租约后重查闸门，两步之间不 `await`，所以在事件循环里天然原子——重建要么见到租约要么见到已退让的读者。

**源码事实：** `get_collection_gate` 的注册表按循环分域且**强引用循环**，因为「从未阻塞过的 `asyncio.Event` 不知道自己属于哪个循环」，而 `id()` 会被重用，所以必须靠 `is` 判断身份。

**源码事实：** Milvus 的 `_run_gated` 给**每次提交**加闸而非每个方法，因为「入口检查」与「实际提交」之间可能隔着几秒的嵌入往返；没有它，`get_by_id` 的 `except Exception: return None` 会把「集合不存在」报成「行不存在」。

**源码事实：** Milvus `drop()` 的重建是**一次**线程池提交而非一串 await，因为「取消所有任务」的关闭会绕过 await 链直接取消内部任务；备份集合先删，理由是崩溃后的可恢复性。

**源码事实：** `MilvusIndexConfig.metric_type` 保留但被忽略（总是 COSINE），「字段保留是为了让传它的调用方继续工作」；所有字段都用 `is None` 判默认而不是真值判。

**源码事实：** `_sanitize_record_value` 只剥孤立代理项，**保留 U+FFFE / U+FFFF**，因为记录的 `entity_name` / `src_id` / `tgt_id` 被当作图键查找，改写任何可编码字符都会让记录与图对象脱钩。

**架构推论：** 「拒绝」与「重放」的分野不在实现难度，而在**「重放是否可定义」**。按 id 的 upsert / delete 天然幂等，重放有意义；而图的变更序列（先加后删）没有可定义的重放顺序。所以两种策略不是保守与激进之分，而是**各自适应其数据模型的唯一正确选择**。这也解释了为什么同一个仓库里能同时存在两种策略而不矛盾。

**架构推论：** Faiss 那三档跨文件不一致的处理，展示了一个可复用的分级：**「多余的数据」可以丢、「缺失的入口」可以警告、「完全不可读」必须拒绝。** 判据是「这个不一致会不会被后续写放大」——多余的行下次保存自然消失，缺失的元数据只是让部分 id 查不到，而完全读不回会被下一次保存变成永久空。**只有最后一种会把一次临时故障升级成永久数据销毁**，所以只有它必须 fail-loud。

**架构推论：** `_CollectionGate` 与 `_run_gated` 解决的是「破坏性维护 vs 长尾在飞请求」这个普遍问题。判据是「租约该覆盖什么」：覆盖**资源真正被占用**的窗口（一次 RPC），而不是方法边界。覆盖太宽会让维护等待用户级延迟（几次嵌入），覆盖太窄会让请求撞上已删除的资源。这一层的注释把这个权衡写成了可检查的性质——「有界且不增长」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/kg/faiss_impl.py` | `FaissVectorDBStorage` | 两文件快照 + 重载重放，余弦靠 L2 归一化 |
| `lightrag/kg/faiss_impl.py` | `FaissVectorDBStorage.upsert` | 延迟嵌入 + 抵消同 id 删除 + 写序 token |
| `lightrag/kg/faiss_impl.py` | `FaissVectorDBStorage.index_done_callback` | 重载 → 刷嵌入 → 存两文件 → 通知 |
| `lightrag/kg/faiss_impl.py` | `FaissVectorDBStorage._load_faiss_index` | fail-loud 加载，三档不一致分级 |
| `lightrag/kg/faiss_impl.py` | `FaissVectorDBStorage.drop_pending_index_ops` | 只丢缓冲，不回滚已物化 |
| `lightrag/kg/nano_vector_db_impl.py` | `NanoVectorDBStorage` | 单 JSON 文件快照 + 重载重放 |
| `lightrag/kg/nano_vector_db_impl.py` | `NanoVectorDBStorage.upsert` | 同上，并在入口净化记录字符串 |
| `lightrag/kg/nano_vector_db_impl.py` | `NanoVectorDBStorage.index_done_callback` | 四步含「重做日志在发布之后才退休」 |
| `lightrag/kg/nano_vector_db_impl.py` | `_sanitize_record_value` | 只剥孤立代理项，保住跨存储的键 |
| `lightrag/kg/milvus_impl.py` | `MilvusVectorDBStorage` | 服务器集合，无需栅栏，需读者租约 |
| `lightrag/kg/milvus_impl.py` | `MilvusVectorDBStorage.upsert` | 截断版进库、完整版送嵌入 |
| `lightrag/kg/milvus_impl.py` | `MilvusVectorDBStorage.drop` | 重建必须是一次提交，备份先删 |
| `lightrag/kg/milvus_impl.py` | `MilvusVectorDBStorage._run_gated` | 每次提交加闸，避免「集合不存在」误报成「行不存在」 |
| `lightrag/kg/milvus_impl.py` | `_CollectionGate` | 读者租约 + 关闭 + 排空 |
| `lightrag/kg/milvus_impl.py` | `get_collection_gate` | 按事件循环分域并强引用循环 |
| `lightrag/kg/milvus_impl.py` | `MilvusIndexConfig` | 环境变量 + 参数两级，`metric_type` 保留但忽略 |
| `lightrag/kg/milvus_impl.py` | `_flush_pending_vector_ops` | 写路径的批量刷盘 |

## 参考

- [lightrag 源码：kg/faiss_impl.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/kg/faiss_impl.py)
- [lightrag 源码：kg/milvus_impl.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/kg/milvus_impl.py)
- [Faiss 文档](https://faiss.ai/)
- [Milvus 文档](https://milvus.io/docs)

## 与主核 / AI 的关系

主核里最接近的是**索引重建（REINDEX / VACUUM）与并发查询的关系**：重建期间要么加排他锁（查询全停），要么用版本化的索引结构让新查询走新版本、老查询走老版本。这一层的 `_CollectionGate` 是第一条路的精细版：不是「全停」，而是「拒绝新读者、等在飞读者排空」。

**差别在于「等谁」。** 主核的排空等的是「持锁的算子」，其耗时可控；这里的排空等的是「读者已经提交给服务器的那次 RPC」，而**故意不等**它前后的嵌入调用。这个界定让 `drop()` 的等待从「用户级延迟」降到「网络往返级」——这是把锁范围从「方法」精确到「资源占用窗口」带来的直接收益。

与 g02（图存储）的对照是整组 G 的主轴：那边**拒绝**过期写，这边**重放**。而 g02 的分析给出了判据——重放要求操作幂等且顺序无关，向量 upsert 满足、图变更不满足。所以「同一份契约、两种冲突策略」不是不一致，而是**按数据模型选策略**。

与 g01（JSON 键值存储）的对照更有意思：那边用共享内存**消除**了冲突（没有副本，就没有落后），这边用文件快照**制造**出冲突然后用重放解决。三种方案（消除 / 拒绝 / 重放）恰好覆盖了这个问题的全部解法空间，而每一层的代价都很清楚：消除要放弃进程内存隔离，拒绝要放弃可用性，重放要求操作可重放。

对 AI 工程最实用的一条是 **`_load_faiss_index` 那个「降级成空索引」的教训**。任何「读失败就当空」的容错在**有写回路径**的系统里都是数据销毁器：空状态会被后续正常流程认证为新真相并持久化。判据很简单：**这个默认值之后会不会被写回它来自的地方？** 会的话，它必须是异常而不是默认值。

另一条是 `_sanitize_record_value` 的边界：**净化函数的作用域必须精确等于「会导致失败的那个字符集」**。多净化一点看起来更安全，但当被净化的字符串是**跨系统的键**（向量记录指向图节点）时，多改一个字符就是让数据脱钩。这条在 ID、路径、URL 这类「既是值又是引用」的字段上尤其重要。

最后一条是「延迟嵌入 + read-your-writes 但不可检索」这个组合。它看起来不完整，其实是必需的：要让缓冲的向量可检索，就得在每次查询前刷盘，延迟机制立刻失效。所以正确的做法是把这条限制写成显式契约（「未刷盘的行故意不可检索」），而不是让使用者自己发现——因为发现的方式通常是「检索结果比预期少」，那是最难排查的一类症状。
