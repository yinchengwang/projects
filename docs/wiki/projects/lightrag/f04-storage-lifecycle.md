---
project: lightrag
feature: storage-lifecycle
title: 存储初始化与收尾
page_type: feature
schema_version: 1
refs:
  - path: lightrag/lightrag.py
    symbols: [initialize_storages, finalize_storages, _establish_embedding_baselines]
  - path: lightrag/lightrag.py
    symbols: [_release_after_early_failure, _commit_cache_pair_before_finalize, _quarantine_cache_before_finalize]
---

# 存储初始化与收尾

## 一句话职责

在启动时把十几个存储按固定次序拉起来、在启动任何向量存储**之前**校验嵌入基线、失败则逆序回滚并把失败粘在实例上；在收尾时按「配置存储最后释放」的次序逐个关掉存储、把成对的提交排在释放之前、并保证取消无法让收尾停在半途。

## 机制是什么

**两端共享同一个前提：`finalize` 是唯一没有「下一次运行」来补救的提交点。** 启动失败还能靠重启修，而收尾失败之后进程就走了——所以这一页的复杂度几乎全在「收尾时怎么不留下不可达的数据」。

三个状态构成生命周期的主干，而它们的语义被精确定义：

| 状态 | 含义 | 谁负责释放 |
|---|---|---|
| `CREATED` | 什么都没起来 | 无 |
| `INITIALIZED` | **资源已存在，`finalize_storages()` 必须释放它们** | 调用方 |
| `FINALIZED` | 已释放 | — |

**文档对 `INITIALIZED` 有一句关键澄清：**

> `INITIALIZED` 意味着资源存在且 `finalize_storages()` 必须释放它们——**而不是检查通过了**。一次成功的返回才意味着这个实例可以服务。

**这个区分解决一个实际矛盾**：启动检查可能失败（比如嵌入基线不匹配），但存储已经起来了、持有客户端与连接池。此时若状态还是 `CREATED`，`finalize_storages()` 会跳过整个拆卸——**调用方 catch 住拒绝之后会泄漏全部资源。**

## 怎么实现的

### 一、启动的十个步骤与两种「粘性」

**步骤序列（按代码注释的编号）：**

| 步 | 做什么 | 失败是粘性吗 |
|---|---|---|
| 前导 | 绑定事件循环、默认工作区、`pipeline_status`、0a–0c 锚点检查、目录占用 | **否** |
| 1 | 配置存储初始化 | 是 |
| 1b | 与锚点核对容器身份（或建立绑定） | 是 |
| 2–3 | 严格读三份嵌入基线并比对，预检不匹配则拒绝 | 是 |
| 4 | 业务存储逐个初始化 | 是 |
| 5 | 标 `INITIALIZED` | — |
| 6 | 覆盖度闸门 + 实体收养探针 | 是 |
| 7 | 为缺失的基线写入声明 | 是 |
| 8 | 刷配置存储 | 是 |

**「粘性」的定义在方法文档里：**

> 从步骤 1 起的每一次失败都是粘性的，取消也不例外：这个方法在之后每次调用时**重新抛出**它，而不是提前返回「已初始化」或在已被回滚关闭的存储上重跑那些步骤。**用一个新实例重试。**

**而为什么前导阶段不是粘性的，理由同样明确：**

> 步骤 1 之前的前导——绑定循环、默认工作区、`pipeline_status`、锚点检查 0a–0c 与目录占用——被有意放在外面：它**不打开任何东西、不移动任何状态**，所以那里的失败让实例与之前完全一样，下一次调用会真正重跑每一步。**让它粘性会因为一次瞬态的共享存储故障而杀死一个实例，且一无所获。**

**这是「粘性」判据的完整表述：粘性是为了防止「假装成功」，而不是为了惩罚失败。** 若失败没有留下任何副作用，重试是安全且正确的；若失败留下了残留（已初始化的存储、已写入的基线），重试就必须危险地重跑——此时新实例才是正确的重试方式。

**而实现的骨架正是这个判断：**

```
if self._startup_refusal is not None:
    raise self._startup_refusal
if self._storages_status != StoragesStatus.CREATED:
    return
```

**第一行重抛留存的失败；第二行是幂等短路。** 而 `_startup_refusal` 的存在正是为了让「重抛」可行——否则重试会走第二行「已经初始化」而返回成功，**把一个 fail-closed 的闸门变成一次性的**。代码注释把这个后果写得很直接：

> 没有它，一次重试会走 `status != CREATED` 的提前返回路径并**在什么都没重跑的情况下返回成功**，把 fail-closed 闸门变成一次性的。

### 二、步骤 1b：为什么必须在读基线之前核对身份

**这一步的注释解释了一个排序理由：**

> 在任何基线被读取之前：**从一个错误容器读到的基线比没有更糟。** 与步骤 2 失败一样是粘性的且被回滚。

**「比没有更糟」是因为基线的语义是「这个工作区的向量是用哪个模型/空间建的」。** 若从一个错误的容器读到别人的基线，两个后果都严重：**匹配**时，你会以为自己与数据一致，实际不一致；**不匹配**时，你会拒绝一次本来正确的启动。

**前者是不可检测的正确性问题**，后者至少会报错。所以「宁可读不到」是成立的。

### 三、步骤 2–3 与「读取失败不是空」

**基线的预检有一个容易被忽略的判定：**

```
# A read that could not complete is a startup failure, never "absent".
if baselines_apply:
    recorded = await read_embedding_baselines(self.configuration_storage, self.workspace)
    bootstrap_targets = precheck_embedding_baselines(recorded, self.embedding_func, workspace=self.workspace)
```

**「读不成 = 启动失败，绝不是『缺失』」** —— 因为「缺失」会触发「建立基线」，而建立基线是一次**写入**。若把读失败当成缺失，就会在一个根本不知道历史状态的东西上写入新基线——**那可能覆盖掉真实的基线记录。**

**这条与 a01 那页「严格读 vs 宽松读」是同一个模式**：控制面（调度、校验）必须用严格读，因为一次错误会让后续所有决策建立在假前提上。

**而预检的位置被强调：**

> 不匹配在**这里**拒绝，在任何向量存储初始化之前——早于 Milvus、Qdrant、PostgreSQL 在 `initialize()` 里跑的旧容器迁移。

**「早于后端自己的迁移」是关键**：一旦向量后端开始初始化，它可能修改磁盘上的容器结构。**在一个已知会失败的启动路径上允许后端动数据，等于让一次配置错误变成一次不可逆的迁移。**

**而「需要一个向量重建」的情况有一条显式通道：**

```
expect_empty_vector_storage=self.rebuilding_vector_storage,
```

**重建工具（`lightrag-rebuild-vdb`）走的是「我预期向量存储是空的」这条路径**，于是覆盖度检查不会拦住它——**否则那个能修好这个状态的工具会被这个状态本身挡住。**

### 四、步骤 5：`INITIALIZED` 标记早于检查

**这是一个反直觉的顺序，而注释解释了它：**

> 在检查**之前**标记 `INITIALIZED`，而不是之后。上面每个存储都完成了 `initialize()`，所以它们持有客户端、连接池与锁——而 `finalize_storages()` 除非状态这么说否则会跳过整个拆卸。**一个 catch 住下面拒绝的调用方（为了上报它，或为了跑一次重建）否则会泄漏它们中的每一个，且在同一对象上重试会把它们初始化两次。**

**「早于检查」换来了两件事**：资源总能被释放；重试不会重复初始化。

**代价是「`INITIALIZED` 不等于能用」** —— 所以文档里那句澄清是必要的，且它明确指出「一次成功的返回才意味着可以服务」。

**步骤 6/7/8 的失败仍然是粘性的，而理由是取消：**

> 粘性（见本方法开头），取消也不例外：这里的 `CancelledError` 会让状态停在 `INITIALIZED` 而检查从未完成，只有留存的失败能让下一次调用不提前返回为就绪。

**也就是说：步骤 6 之后状态已经是 `INITIALIZED`，若无粘性，重试会走第二行提前返回「成功」而检查永远没跑过。**

### 五、`_establish_embedding_baselines`：只在正面证据上写入

**这是整个启动序列里最讲究「不猜」的一段。它的判定表被文档完整写出来了。**

> 写什么取决于证据，逐个目标：一个**空**的来源只有在那个目标的向量容器也被确认是空的时候才记录配置空间（`origin=empty`）；一个**有数据**的来源只在那**一个**目标的探针给出**正面**结论时记录（`origin=probe`）。其它所有情形——来源或容器不可读、来源为空但背后有残留向量、探针不确定或无法运行——**什么都不写**并在下次启动重试。**任何目标都不会在兄弟目标的裁断上被记录。**

**五种情形，只有两种会写入：**

| 来源有数据？ | 容器空？ | 探针结论 | 动作 |
|---|---|---|---|
| 否 | **是** | — | 写 `origin=empty` |
| 否 | 否 | — | **不写**（有向量但无法证明模型） |
| 否 | 未知 | — | **不写** |
| 是 | — | **正面** | 写 `origin=probe` |
| 是 | — | 非正面/无法运行 | **不写** |
| 未知 | — | — | **不写** |

**而代码与这张表逐条对应：**

```
if populated is False:
    index_empty = evidence.index_empty.get(target)
    if index_empty is False:
        logger.warning(f"... its source holds no data but its vector storage still holds vectors, and nothing can vouch for the model they were written in. Rebuild them with lightrag-rebuild-vdb or clear the workspace; retried on the next start.")
        continue
    if index_empty is not True:
        logger.warning(f"... whether its vector storage is empty could not be established. Retried on the next start.")
        continue
    origin = BaselineOrigin.EMPTY
elif populated is True:
    if evidence.probes.get(target) is not True:
        logger.warning(f"... the adoption probe could not vouch for the stored vectors ({...}). Retried on the next start.")
        continue
    origin = BaselineOrigin.PROBE
else:
    logger.warning(f"... whether its source holds data could not be established. Retried on the next start.")
    continue
```

**四处判定全部用 `is False` / `is True` 而不是真值判断** —— 这里是三态：`True` / `False` / `None`（未知）。**真值判断会把「未知」和「否」混为一谈**，而那正是「读失败被当成缺失」的同一个错误。

**「任何目标都不会在兄弟目标的裁断上被记录」这条是最重要的约束。** 三个目标（实体、关系、块）各有独立的来源与容器，用其中一个的结论推断另一个是错的——因为它们可能不一致（比如换了模型只重建了块的向量）。

**而那条「来源空但容器有向量」的警告给了一个可操作的出路：**

> 用 `lightrag-rebuild-vdb` 重建它们，或清空工作区；**下次启动会重试。**

**「下次启动会重试」这句话在每个分支上都出现** —— 它明确了「不写」不是失败，而是「留待下次」。这与粘性失败形成对照：**这里不粘，因为它没有副作用。**

### 六、`finalize_storages`：三阶段与三层取消保护

**收尾的整体结构是「排队 → 屏蔽的拆卸 → 条件收尾」，而每一层都有明确的取消语义。**

**第一阶段：可中断的队列排水。**

```
try:
    try:
        await self._cancel_admin_release_drives()
        await self._shutdown_llm_and_rerank_queues()
    except asyncio.CancelledError as exc:
        cancelled = exc
        logger.error(f"[{self.workspace}] Cancelled while draining the queues at shutdown; the storages are released before the cancellation is re-raised")
```

**取消被吸收并记住，而不是让它传播。** 理由写得很清楚：

> 在这里吸收并在末尾重抛，而不是让它传播出去：排空一个 LLM 队列要等正在进行的工作，所以一个 shutdown 超时升级成取消的情况**在这两个 await 上比其他任何地方都更常发生**——而让它逃出去会跳下面整个存储拆卸，同时 `finally` 仍然会交回目录占用。**那正是这个占用要防的唯一组合：另一个服务器接管了目录而这个进程还持有共享命名空间占用与未刷盘的写入。**

**「超时升级成取消最常发生在排空队列时」是一个基于实际观测的判断**，而它引出的那条危险性正是因为 `finally` 里的目录释放无条件执行——**拆卸被跳过而占用被交回，是最坏的组合。**

**第二阶段：屏蔽的拆卸任务。**

```
teardown = asyncio.ensure_future(self._finalize_storages_impl())
while not teardown.done():
    try:
        await asyncio.shield(teardown)
    except asyncio.CancelledError as exc:
        if cancelled is None: cancelled = exc
teardown.result()
```

**`while not done(): await shield(...)` 这个循环就是「无论如何要等完」的实现** —— 因为单纯的 `await shield(task)` 会在调用方被取消时立即返回（而任务还在跑）。**循环让它反复重试直到任务真的完成。**

这与 `_release_after_early_failure` 里那段「shielded 保持存活但不会完成它」的注释是同一条认识。

**第三阶段：向量队列关闭，且**不在** `finally` 里。**

```
try:
    await self._shutdown_embedding_queue()
except asyncio.CancelledError as exc:
    if cancelled is None: cancelled = exc
```

**注释解释了为什么刻意不放 `finally`：**

> 向量关闭被刻意**不**放进 `finally`。它是**永久的**——一个关闭的包装会拒绝之后每一次调用——所以在一次被中断的收尾之后运行它，会让**重试**的向量刷盘失败并丢掉缓冲的向量，而存储仍被标为 `FINALIZED`。**一次存储拆卸抛异常的收尾因此把向量队列留开给重试。** 接受的残留：没有重试时，它的 worker 会挂到事件循环关闭（不丢数据；未刷的向量无论如何都会丢）。

**「永久操作不能在可重试路径上无条件执行」是一条精确的约束。** 向量队列的关闭不可逆，所以它必须排在「确定不会重试」的位置——即拆卸成功之后。

**而 `finally` 只做一件不可替代的事：交回目录占用与锚点锁。**

```
finally:
    if self._holds_working_dir:
        self._holds_working_dir = False
        release_working_dir_lock(self.config_dir)
    self._release_anchor_lock()
```

**「在 `finally` 里因为它是唯一没人能替你做的释放」** —— 一个已经走到出口的进程若还占着目录，会白拒下一个服务器。

### 七、`_release_after_early_failure`：逆序释放与「shield 不等于完成」

**这是启动失败的逆序回滚，而它有一句关于 `shield` 的重要注释。**

```
release = asyncio.ensure_future(storage.finalize())
try:
    await asyncio.shield(release)
except asyncio.CancelledError:
    # The shield kept the release ALIVE; it did not finish it. Awaiting a
    # shielded task returns the moment the awaiting task is cancelled, so
    # moving on here would only DETACH the release -- and a detached release
    # racing the loop's own shutdown loses exactly what it was called to
    # hand back (a flush still in flight, a shared-namespace hold).
    detached.append(release)
```

**「shield 保持了任务的存活，但它没有完成它」** —— 这是 `asyncio.shield` 最容易被误用的地方。awaiting 一个被 shield 的任务，在**等待方**被取消时会立即返回，而**被 shield 的任务继续跑**。

**于是「继续往下走」等于「把那个 release 变成孤儿」** —— 而孤儿 release 与事件循环关闭竞争时，会丢掉它本要交回的东西（在途的刷盘、共享命名空间占用）。

**解法是记住它、在 `finally` 里排空：**

```
finally:
    await _drain_detached_releases(detached, workspace=self.workspace)
    if self._holds_working_dir:
        self._holds_working_dir = False
        release_working_dir_lock(self.config_dir)
    self._release_anchor_lock()
```

**顺序是「先排空孤儿、再交回目录」** —— 否则另一个服务器可能在我们的 release 还在跑时就接管目录。

**而加锁顺序在代码里有两处对照：**

```
# The directory claim is taken before step 1, so it outlives every
# storage in the rollback list and is given back last
...
# Taken before the directory claim, so given back after it.
self._release_anchor_lock()
```

**目录占用在步骤 1 之前取得 → 最后交回；锚点锁在目录占用之前取得 → 在它之后交回。** 严格的逆序（LIFO），与锁的获取顺序对称。

**「只需释放回滚列表里列出的存储」这条也被论证了：**

> 只有回滚列表点名的存储需要释放。**没有一个后端会在它的 `initialize()` 运行之前取得进程级资源**，所以循环从未到达的存储什么都没持有——而 `finalize()` 在它上面也不是选项，因为没有后端承诺在从未初始化的实例上能工作。

**这是「为什么可以只释放子集」的证明**，而不是「反正没起来的应该没问题」。

### 八、`_commit_cache_pair_before_finalize` 与 `_quarantine_cache_before_finalize`：收尾时的数据可达性

**这两个函数解决一个具体问题：缓存行引用块行，而收尾是最后一次发布机会。**

**第一个：有序提交，且用最严的判据。**

```
async def _commit_cache_pair_before_finalize(self) -> None:
    pair = [inst for inst in (self.text_chunks, self.llm_response_cache) if inst is not None]
    if len(pair) < 2: return
    try:
        await self._flush_storages(pair, strict_references=True)
    except asyncio.CancelledError: raise
    except Exception as e:
        logger.error(f"Ordered cache commit before finalize failed: {e}")
```

**文档说明为什么必须用 `strict_references`：**

> 排序必须在块提交**只是返回了**而仍有操作被缓冲时就保留缓存，而不仅是在它失败时。缓存自己 finalize 之前的闸门覆盖不了那个——它在这对之后运行，而这对已经发布的行已经在磁盘上，任何隔离都够不到。

**「只是返回了而仍有操作被缓冲」是一种「看起来成功但实际没完成」** —— 而普通刷盘只问「有没有抛异常」。**在收尾这个唯一不能补救的时刻，判据必须更严。**

**第二个：在把缓存刷出去之前，先丢掉那些引用已不可达的行。**

```
reason: str | None = None
if chunks_finalize_failed:
    reason = "text_chunks.finalize() failed"
elif self._chunk_reference_commit_failed:
    reason = "a chunk-reference commit failed earlier in this process"
else:
    try:
        if await cast(StorageNameSpace, self.text_chunks).has_pending_index_ops():
            reason = "text_chunks operations are still buffered"
    except Exception as e:
        logger.error(f"Failed to inspect pending chunk operations: {e}")
if reason is None: return
try:
    dropped = await cast(StorageNameSpace, self.llm_response_cache).drop_pending_upserts(cache_types=set(_REFERENCE_CARRYING_CACHE_TYPES))
```

**三条 reason 对应三种「块没落盘」的原因**，而第三条来自 a01 那页的 `has_pending_index_ops` —— 那个「默认 `False` 是未实现的答案而不是真相」的方法。

**而两种结局的日志措辞准确到近乎残酷：**

> `0` 在这里是寻常情况：上面那个有序对已经在它的块那一半失败时隔离了它们。

**第一条（有后端能单独丢弃）：**「N 条缓冲的抽取缓存行在最终缓存刷盘之前被丢弃了，**没有留下不可达的**。它们会在下次运行被重新计算。」

**第二条（后端不能单独丢弃）：**「最终缓存刷盘可能发布那些**拥有它们的块行从未到达磁盘**的行；**进程正在退出，所以没有任何东西会治愈它们。删除受影响的文档也不会回收那些缓存行。**」

**这两段日志的差别就是「有救」与「没救」的差别，而它们被写成了完全不同的句子。** 这是「日志要说明后果」的最好范例——不是「丢弃了 0 行」，而是「这 0 行意味着什么」。

**而文档最后承认了这个机制的边界：**

> 这个闸门在快照后端上**不闭合**，那里丢弃是基类的空操作。那个残留被记录在 *LLM extraction cache reachability*，`docs/design/PurgeRecoveryContract.md`。

**「闸门不闭合」是一个精确的表述**：在某些后端上，这段代码会执行但什么都没做（因为 `drop_pending_upserts` 是基类的空操作），于是不可达的缓存行仍会被发布。**承认它并记录在残留表里，比假装它闭合了好。**

## 关键取舍

**源码事实：** 三个状态中 `INITIALIZED` 的定义是「资源存在且必须被释放」，**不是「检查通过了」**；一次成功的返回才意味着实例可服务。

**源码事实：** 步骤 1 之后的失败一律粘性（含取消），重试必须换新实例；步骤 1 之前的前导不粘，因为它不打开任何东西、不移动状态——「让那里粘性会因为一次瞬态共享存储故障而杀死一个实例，且一无所获」。

**源码事实：** 粘性需要 `_startup_refusal` 才能生效，否则重试会走 `status != CREATED` 的提前返回「在什么都没重跑的情况下返回成功」，把 fail-closed 闸门变成一次性的。

**源码事实：** 步骤 1b（与锚点核对容器身份）必须在任何基线读取之前，因为「从一个错误容器读到的基线比没有更糟」。

**源码事实：** 基线读取中「读不成 = 启动失败，绝不是缺失」——因为「缺失」会触发写入新基线，可能覆盖真实记录。这与严格读 vs 宽松读是同一条规则。

**源码事实：** 基线预检的位置早于任何向量存储初始化，也早于 Milvus/Qdrant/PostgreSQL 在 `initialize()` 里的旧容器迁移——否则一次配置错误会变成一次不可逆的迁移。

**源码事实：** `INITIALIZED` 在检查**之前**标记：上面每个存储都已 `initialize()` 并持有客户端/连接池/锁，而 `finalize_storages()` 除非状态这么说否则跳过整个拆卸。代价是「`INITIALIZED` 不等于能用」，收益是资源总可释放且重试不重复初始化。

**源码事实：** 基线的建立只在正面证据上发生：来源空 **且** 容器确认为空 → `origin=empty`；来源有数据 **且** 该目标的探针正面 → `origin=probe`；其余五种情形全部不写并下次重试。四处判定都用 `is True` / `is False`（三态），真值判断会把「未知」和「否」混淆。

**源码事实：** 「任何目标都不会在兄弟目标的裁断上被记录」——三个目标的来源与容器可能不一致（比如换模型只重建了块的向量）。

**源码事实：** `finalize_storages` 三段结构：可中断的队列排水（取消被吸收并记住）、屏蔽并排空到完成的拆卸任务（`while not done(): await shield(...)`）、拆卸成功后才关向量队列。

**源码事实：** 向量队列关闭刻意**不在** `finally` 里，因为它是永久的：在一次被中断的收尾之后运行它，会让重试的向量刷盘失败并丢掉缓冲的向量，而存储仍被标成 `FINALIZED`。

**源码事实：** `finally` 只做两件不可替代的事：交回目录占用与锚点锁。目录占用在步骤 1 前取得故最后交回；锚点锁在目录占用之前取得故在它之后交回（严格 LIFO）。

**源码事实：** `_release_after_early_failure` 里 `await shield(release)` 被取消只保证任务存活、不保证完成——继续往下走等于把 release 变成孤儿。解法是记住并在 `finally` 里排空，且排空必须在交回目录之前。

**源码事实：** 「只需释放回滚列表里的存储」有证明：没有后端在 `initialize()` 之前取得进程级资源，而未初始化的实例上 `finalize()` 也没有后端承诺可用。

**源码事实：** 收尾前必须用 `strict_references=True` 提交缓存对，因为「块提交只是返回了而仍有操作被缓冲」也算失败——缓存自己 finalize 前的闸门够不到已经发布的行。

**源码事实：** `_quarantine_cache_before_finalize` 用三条 reason 判断块是否落盘（`text_chunks.finalize()` 失败 / 本进程里更早的引用提交失败 / `has_pending_index_ops()` 为真），然后丢弃引用已不可达的缓存行。两条结局日志措辞完全不同：有救的说「没有留下不可达的，下次运行会重算」，没救的说「进程正在退出，没有什么会治愈它们，删文档也不会回收」。

**源码事实：** 这个闸门在快照后端上**不闭合**（那里的 `drop_pending_upserts` 是基类空操作），该残留被记录在设计文档的残留表里。

**架构推论：** 这一页的核心规律是**「失败是否留下副作用」决定它该不该粘性**。启动前导的失败没有副作用，重试安全；步骤 1 之后的失败留下已初始化的存储，重试会重复初始化——所以必须换实例。**粘性不是惩罚，而是对「不可安全重试」的诚实标注。**

**架构推论：** 收尾的整个结构都在处理一件事：**一个不可逆操作的顺序不能由取消决定**。向量队列关闭不可逆、目录占用交回不可逆、存储拆卸部分可逆——于是它们被放在不同的取消语义层级里（可中断 / 屏蔽后等完 / 只在成功路径）。**「哪些步骤可以被打断」是一个必须逐项决定的设计问题，而不是全局开关。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/lightrag.py` | `initialize_storages` | 十步启动序列与两种粘性 |
| `lightrag/lightrag.py` | `_startup_refusal` | 让粘性失败在重试时重抛而非假成功 |
| `lightrag/lightrag.py` | `_establish_embedding_baselines` | 只在正面证据上写基线，六种情形逐一判定 |
| `lightrag/lightrag.py` | `_embedding_baselines_apply` | 是否启用基线机制的总开关 |
| `lightrag/lightrag.py` | `check_vector_space_at_startup` | 覆盖度闸门与收养探针（步骤 6） |
| `lightrag/lightrag.py` | `flush_configuration_storage` | 步骤 8 的显式刷盘 |
| `lightrag/lightrag.py` | `finalize_storages` | 三段结构与三层取消保护 |
| `lightrag/lightrag.py` | `_finalize_storages_impl` | 逆序释放十三个存储，配置存储最后 |
| `lightrag/lightrag.py` | `_release_after_early_failure` | 启动失败的逆序回滚与孤儿 release 排空 |
| `lightrag/lightrag.py` | `_commit_cache_pair_before_finalize` | 收尾前的有序对提交（唯一用 `strict_references`） |
| `lightrag/lightrag.py` | `_quarantine_cache_before_finalize` | 三条 reason + 丢弃不可达缓存行（快照后端不闭合） |
| `lightrag/lightrag.py` | `_shutdown_parser_executor` | 先置关闭事件再关执行器（顺序有讲究） |

## 参考

- [lightrag 源码：lightrag.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/lightrag.py)
- [lightrag 文档：配置存储契约](https://github.com/HKUDS/LightRAG/blob/main/docs/design/ConfigurationStorageContract.md)
- [lightrag 文档：向量空间溯源](https://github.com/HKUDS/LightRAG/blob/main/docs/design/VectorSpaceProvenance.md)
- [Python 文档：asyncio shield](https://docs.python.org/3/library/asyncio-task.html#asyncio.shield)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

启动序列与主核的**崩溃恢复（redo/undo）**面对同一个问题：把一堆互相依赖的资源按一个安全的次序拉起来，任何一步失败都不能留下半初始化状态。而两者的解法不同——主核用 WAL 把「做到哪一步」变成持久事实，重启后重放；lightrag 用「逆序回滚 + 粘性失败」把「做到哪一步」限制在单次调用内。

**「逆序释放」与主核的析构顺序是同一条纪律。** 加锁顺序与释放顺序必须严格对称，否则会出现「释放了一个别人还依赖的资源」。这里的证据是那句「目录占用最后交回、锚点锁在它之后交回」——**两把锁的顺序在代码里各有一句注释说明它为什么在那个位置。**

**「`await shield(task)` 只保证存活不保证完成」这条与主核的异步 I/O 取消语义同构。** 数据库的后台 I/O 也需要区分「已发出」与「已完成」，而取消一个等待不会撤回已发出的 I/O。lightrag 的 `while not done()` 循环就是「等到底」的实现，与数据库的 `IO_WAIT` 状态机是同一个模式。

**「基线只在正面证据上写入」对应主核的「不推定元组可见性」。** PostgreSQL 的 `hint bit` 设置必须基于确证的提交状态，推定错误会造成数据丢失；lightrag 的基线写入必须基于确证的容器空/探针正面，推定错误会让一组向量带着错误的模型标签继续服务。**两者的共同判断是：写下一个「关于数据状态」的断言，比不写要危险得多。**

对 agent 工程的直接价值有三条。其一，**粘性失败的判据是「有没有留下副作用」**。没有副作用就允许重试，有副作用就必须换实例——并且需要一个显式的留存字段，否则重试会静默走「已成功」的短路。其二，**不可逆操作不能放在 `finally` 里**。`finally` 会在每条退出路径上执行，包括「前面的步骤失败了可能还要重试」的那条；把一个永久操作放进去，会让重试失去前提。其三，**日志要说明后果而不是动作**。「丢弃了 0 行」与「这 0 行意味着永久不可达且删文档也不会回收」是两种完全不同的信息密度。
