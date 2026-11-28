---
project: lightrag
feature: insert-flow
title: 文档插入流程
page_type: feature
schema_version: 1
refs:
  - path: lightrag/lightrag.py
    symbols: [LightRAG, ainsert]
  - path: lightrag/lightrag.py
    symbols: [_process_extract_entities, _flush_storages, _insert_done_with_cleanup]
---

# 文档插入流程

## 一句话职责

把「一份文档」变成「跨四个存储的一批写入」，并在没有事务的前提下保证**失败之后残留物的位置是已知的**：入队只写状态不写内容、抽取失败只标 FAILED 不丢队列、刷盘失败要能区分「拒绝了」与「没写成功」、而刷盘失败后到底是丢缓冲还是留缓冲取决于「重跑能不能重造这份数据」。

## 机制是什么

插入分成两个可独立失败的阶段，而它们的失败处置完全不同：

| 阶段 | 入口 | 写什么 | 失败后 |
|---|---|---|---|
| **入队** | `apipeline_enqueue_documents` | `full_docs` + `doc_status`（PENDING） | 内容已落盘，文档可见、可重试 |
| **处理** | `apipeline_process_enqueue_documents` | 图、向量、块、实体/关系溯源 | 标 FAILED 并**保留**，由扫描重试 |

**而中间那层「刷盘」是整套机制的核心难点。** 因为四个存储各有自己的缓冲与提交时机，一次「逻辑上完成」的插入可能只写进去了两个存储——**而剩下的两个会在什么时候被写、失败时会不会丢，取决于每个后端的实现**。

`ainsert` 是最薄的一层：它只是「入队 + 处理」的两行顺序调用，加上把 F 策略的运行时参数烤进快照。

## 怎么实现的

### 一、`ainsert`：为什么它的文档比代码长十倍

**实现只有三步。**

```
if track_id is None:
    track_id = generate_track_id("insert")

chunk_opts = resolve_chunk_options(self.addon_params, split_by_character=split_by_character, split_by_character_only=split_by_character_only)
await self.apipeline_enqueue_documents(input, ids, file_paths, track_id, chunk_options=chunk_opts)
await self.apipeline_process_enqueue_documents()
return track_id
```

**而它的文档字符串说明了一件容易误解的事：这个方法永远是 F 策略（固定 token 分块）。**

> SDK 便捷入口。它**总是**用固定 token（F）策略分块：`process_options` 被有意不传，于是文档走 F 分块器。……`ainsert` 不能选择递归字符（R）、语义向量（V）或段落语义（P）策略。

**而它紧接着说明「服务端不走这条路」：**

> LightRAG **server / REST API 不调用这个方法** —— 它通过 `apipeline_enqueue_documents` + `apipeline_process_enqueue_documents` 配一个每文档的 `process_options` 选择器来摄取，那才是 F/R/V/P/C 在那里被选择的方式。

**这段说明的价值在于防止一个非常自然的误用**：读代码的人会以为 `ainsert` 是主入口，然后奇怪为什么服务端的分块策略这么少。**实际它是 SDK 侧的一个便利封装，而真正的多策略入口是那两个 pipeline 方法。**

**而 `chunk_options` 的快照语义也被写明：**

```
# Capture the F-strategy runtime args into a chunk_options
# snapshot before enqueue so they become a per-document
# setting. ``apipeline_enqueue_documents`` itself doesn't take
# split args -- chunk_options is the canonical chunker-config
# carrier; runtime split args are ainsert-only concern.
```

**「让它们成为一个每文档设置」** —— 这是快照的意义：分块参数在入队时定下来，之后即使全局配置改了，这份文档仍按入队时的参数处理。

### 二、`apipeline_enqueue_documents`：`ids` 与 `docs_format` 的优先关系

**它的参数文档里有一段处理了两个参数冲突的规则，而这条规则很容易被误读成 bug。**

> `ids`：……**提供 `ids` 标记 SDK 原始直插路径**（`LightRAG.ainsert`）并**优先于 `docs_format`**：文档总以 RAW 入队——净化后的原文内容、不延迟给解析 worker——**这是设计而非疏漏**。`pending_parse` 是服务端上传路径，它从不传 `ids`。

**「这是设计而非疏漏」这句话本身就是对「看起来像 bug」的预判。** 两个参数同时能决定格式时，必须有一个赢；而赢的是 `ids`。

**为什么这样定**：SDK 调用方传了 `ids` 意味着它自己管身份，那它要的一定是「我给的这段文本就是内容」；而服务端上传的真实文件内容在请求体里还没有被解析，必须走 `pending_parse`。两条路径的语义由「谁提供 id」区分开。

**另一个参数说明了一个「不去读快照」的例外。**

> 快照里有一个键**不被**采纳：`semantic_vector.sentence_split_regex` 在**处理时**从 `addon_params` **实时重读**，快照值被丢弃（并记 WARNING）——见 `apply_trusted_sentence_split_regex` 与 GHSA-32jh-39m7-8x84。

**也就是说快照机制有一个刻意的漏洞**，而理由是安全：分句正则是正则表达式，若允许由文档快照带入，那么一个能控制快照的人就能注入正则。**所以它被固定在受信任的配置源里。**

**「快照 + 一个例外」这个形态本身值得记**：快照保证了「参数在入队时冻结」，而例外保证了「安全相关的参数不能冻结在数据里」。两者共存需要一条规则（哪些键允许被快照）与一条强制（违规时警告）。

**`admission_token` 处理的是「同一个请求不被重复计数」。**

> 当 `MAX_PENDING_DOCUMENTS > 0` 时，准入守卫把**那个**令牌重新加权为去重后文档数，而不是注册第二个预留，于是一个请求永远不会被算两次。

**`from_scan` 处理的是「扫描自己的写入不该被自己挡住」。**

> 当为 True 时，调用方是已持有 `pipeline_status["scanning"]` 的扫描后台任务。……外部调用方必须留 False。

**两个参数都是「调用方身份」的显式声明**，而不是从上下文推断。**这是把授权问题变成参数问题的做法**：谁能做特权操作不由它是不是扫描任务决定，而由它有没有权限声明这一点决定（并通过内部调用点保证）。

### 三、`_process_extract_entities`：一条守卫防止掩盖真正的错误

**整个函数是一个 `try` + 一个 `except`，而 `except` 里有一段注释值得逐字读。**

```
try:
    chunk_results = await extract_entities(chunk, global_config=..., pipeline_status=..., ...)
    return chunk_results
except Exception as e:
    error_msg = f"Failed to extract entities and relationships: {str(e)}"
    logger.error(error_msg)
    # Guard the status write: this method may be called with no
    # pipeline_status/lock (both default to None). Without the guard,
    # ``async with None`` raises TypeError here and masks the real
    # extraction error ``e`` that we want to surface.
    if pipeline_status is not None and pipeline_status_lock is not None:
        async with pipeline_status_lock:
            pipeline_status["latest_message"] = error_msg
            append_pipeline_history(pipeline_status, error_msg)
    raise e
```

**注释说的是一个非常具体的陷阱**：`pipeline_status` 与 `pipeline_status_lock` **都默认 `None`**。而 `async with None` 会抛 `TypeError`——那个 `TypeError` 会**在 `except` 块里冒出来**，从而取代正在传播的 `e`。

**结果就是：一次真实的抽取失败，被报成「`NoneType` 不支持异步上下文管理器」。** 而 `raise e` 永远不会执行到。

**这条守卫的成本是两个 `is not None`，收益是「错误消息永远是真正的错误」。** 这是一个典型的「错误处理代码自己成了新的错误源」的反例，而它被注释显式记录了下来。

**而 `append_pipeline_history` 与 `latest_message` 双写是两件事**：前者是历史轨迹（可追溯），后者是当前状态（`/health` 与前端进度条读它）。

### 四、`_flush_storages`：`False` 不是假值，是一个答复

**这是整套刷盘机制里最精妙的一段。它把 `index_done_callback` 的返回值分成三种含义。**

```
committed = await cast(StorageNameSpace, storage_inst).index_done_callback()
if committed is False:
    raise RuntimeError("index_done_callback declined the commit: the storage reloaded a newer snapshot from disk and discarded the pending mutation")
```

**注释解释了 `False` 的含义：**

> 一次**被拒绝的**提交，而不是失败的提交：后端拒绝写入，因为磁盘上的文件已经越过了它持有的快照，于是它**丢弃内存中的变更**以收敛到那个文件（`NetworkXStorage.index_done_callback` 的第一个块）。变更已经没了，所以**确认这次刷盘会让文档被标成 PROCESSED 而它的图写入被丢掉，且没有任何东西能恢复它们**。抛出让这个批次走 FAILED 路径，那里的重处理会重新抽取并重写它——那是被丢弃的工作回来的唯一途径。

**三种结果与三种处置：**

| 返回值/异常 | 含义 | 处置 |
|---|---|---|
| `None`（绝大多数后端） | 提交成功 | 继续 |
| `False` | 后端**主动拒绝**并丢弃了变更 | 抛 `RuntimeError` → 文档 FAILED → 重试重造 |
| 抛异常 | 提交失败 | 按是否可能丢引用分流 |

**而那个 `is False` 的写法被专门解释了：**

```
# Identity, not truthiness: ``BaseGraphStorage``'s signature is
# ``-> None`` and every other backend returns nothing, so only an
# explicit ``False`` is an answer. Same rule as
# ``utils_graph._commit_graph_or_raise``.
```

**「只有显式的 `False` 才是一个答复」** —— 这是整个存储契约里反复出现的模式（见 a01 那页的哨兵类讨论）：**「没说话」与「说了否」必须用 `is` 而不是真值判断区分。**

### 五、`_flush_storages`：两种失败，两种记忆

**异常路径的第一分叉是「这个后端是不是 `text_chunks`，且失败是否可能丢了引用」。**

```
except Exception as e:
    if storage_inst is self.text_chunks and flush_may_have_lost_reference(e):
        await self._record_chunk_reference_commit_failure(f"{type(storage_inst).__name__} flush failed")
    elif storage_inst is self.text_chunks:
        ...
```

**为什么需要把这个失败「记住」而不是只让它传播，注释给的理由非常具体：**

> **粘性的，因为这个失败不会在任何别的东西里存活下来。** 一个按项失败的后端在抛出之前会**从缓冲区里丢掉一个永久失败的操作**，于是之后每次重试这个命名空间都会找到空缓冲并报告成功——失败尾声的与中止批次的清理都一样。异常也带不了它：它只是若干被收集结果中的一个，且不必是传播出去的那个。

**三层理由，每一层都是「为什么不能靠别的东西记住它」：**

1. **缓冲区里已经没有了**（后端自己丢的），所以下次刷盘「看起来成功」。
2. **异常也不是可靠载体**（它在 `gather` 的结果里，可能不是最终传播出去的那个）。
3. 于是只能在实例上记一个字段（`_chunk_reference_commit_failed`）。

**而「只有完整的成对提交才能清除它」是配套的**：因为这个记录的含义是「块引用与缓存的成对一致性可能被破坏了」，而只有一次成功的有序对提交才能证明一致性恢复。

**`strict_references` 参数把「返回了但有残留」也算作失败。**

```
await self._flush_storages(pair, strict_references=True)
```

> `strict_references` 额外在块提交**返回了但仍有操作被缓冲**时保留缓存。**只从没有下一次提交的调用方传它** —— `_commit_cache_pair_before_finalize` 是今天唯一的那个。

**这个参数的语义是「更严的成功判据」**：普通的刷盘只问「有没有抛异常」，而它还要问「是不是真的都下去了」。而它只能用在 shutdown 路径上，因为正常运行时有下一次提交可以补救。

**而「哪个存储被跳过」被精确规定。**

> 「每次刷盘都跑」的唯一例外是 `llm_response_cache`，它被串在 `text_chunks` **之后**，于是当后者失败时被跳过——这是刻意的，且只是延迟：两个缓冲都保留待处理状态给下一次提交，而中止批次的清理（`_discard_pending_index_ops`）会在丢弃前刷一遍缓存。

**「只是延迟而非丢弃」这个措辞是关键**：跳过的理由不是「不用做了」，而是「顺序要求它等下一轮」。

**为什么需要这个顺序**：缓存行引用块行（`llm_cache_list`）。若缓存先落盘而块没落，那些缓存行就永久不可达（见 f04 那页的「隔离」机制）。

### 六、`_insert_done_with_cleanup`：丢弃 vs 保留的判断

**这个函数只有五行，而它的文档说明了「什么时候该丢缓冲」。**

```
try:
    await self._insert_done()
except IndexFlushError:
    await self._discard_pending_index_ops(skip_enqueue_owned=False)
    raise
```

**为什么直插路径需要丢弃而管道不需要：**

> 文件管道会中止并集中调用 `_discard_pending_index_ops()`，但直插调用方（自定义 KG / 块插入）没有这样的清理。没有它，一个永久刷盘失败会**把中毒的操作留在缓冲里**——OpenSearch 保留一个不可重试的 bulk item；milvus/qdrant/postgres/mongo 保留整个缓冲——而之后每次 `_insert_done()` 都会重放它，**即使在调用方提交了其它合法工作之后**。

**「中毒的操作」这个概念是核心**：一个永久失败的操作留在共享缓冲里，会让之后每一次刷盘都失败。于是「丢缓冲」不是放弃数据，而是**把一个会扩散的故障局部化**。

**而丢完必须 `raise`** ——「这样失败仍然浮现给调用方」。丢弃是清理，不是掩盖。

**最值得注意的是那条 WARNING，它划出了这个清理的禁区。**

```
WARNING: do NOT use this on deletion paths. ``_discard_pending_index_ops``
drops pending DELETES too, but deletes are not regenerable by
reprocessing (the document is being removed, nothing re-issues them).
Dropping them -- while a deletion may still report success -- would leave
stale vectors/KV searchable. Deletion paths must use plain
``_insert_done`` so failed deletes stay buffered for a later retry.
```

**「删除不可由重处理重造」是这条禁区的全部理由。**

插入失败 → 文档标 FAILED → 重试 → 重新抽取并重写。这条恢复路径成立的前提是**插入的数据可以从原文重造**。

而删除不是：一个「删除文档 X」的操作失败后，没有任何东西会重新发出它——文档已经从状态里移除，重处理不会跑。**于是丢掉待处理的删除 = 那些向量与 KV 永久残留且可被检索到。**

**这是一个「可重造性」判据的完整应用**：同一个丢弃动作，对可重造的操作是正确的清理，对不可重造的操作是数据泄漏。

### 七、`_index_storages`：一份「一起刷的有哪些」

```
return [storage_inst for storage_inst in [
    self.full_docs, self.doc_status, self.text_chunks,
    self.full_entities, self.full_relations, self.entity_chunks, self.relation_chunks,
    self.llm_response_cache, self.entities_vdb, self.relationships_vdb,
    self.chunks_vdb, self.chunk_entity_relation_graph,
] if storage_inst is not None]
```

**十二个存储，而顺序不是随意的** —— `text_chunks` 在 `llm_response_cache` **之前**（成对顺序），而 `full_docs` / `doc_status` 在最前（它们是「这份文档存在」的证据）。

**`if storage_inst is not None` 的过滤**让同一个列表能适配「没配块向量库」之类的简化部署——**功能降级靠配置留空，而不是靠分支。**

## 关键取舍

**源码事实：** `ainsert` 永远是 F（固定 token）策略，`process_options` 被有意不传；服务端/REST API 不调用它，而是用 `apipeline_enqueue_documents` + `apipeline_process_enqueue_documents` 配每文档选择器，F/R/V/P/C 在那里选。

**源码事实：** `chunk_options` 是分块器配置的规范载体，`split_by_character` 等运行时参数在入队时烤进**每文档快照**，于是之后改全局配置不影响已入队的文档。

**源码事实：** 快照有一个刻意的例外：`semantic_vector.sentence_split_regex` 在处理时从 `addon_params` **实时重读**、快照值被丢弃并记 WARNING——因为允许从数据里带入正则等于允许注入。安全相关的参数不能冻结在数据里。

**源码事实：** 提供 `ids` 时文档总以 RAW 入队，`docs_format` 被忽略。注释明写「这是设计而非疏漏」：SDK 传 `ids` 意味着自管身份，服务端上传路径从不传 `ids`。

**源码事实：** `_process_extract_entities` 在写状态前必须检查 `pipeline_status` / `pipeline_status_lock` 都非 `None`——否则 `async with None` 的 `TypeError` 会取代正在传播的真实抽取错误，`raise e` 永不执行。

**源码事实：** `index_done_callback` 返回 `False` 是「后端主动拒绝并丢弃了变更」，必须抛 `RuntimeError` 让文档走 FAILED 重试——确认它会标 PROCESSED 而图写入已丢。判断用 `is False` 而非真值判断，因为其他后端返回 `None`。

**源码事实：** `text_chunks` 刷盘失败若可能丢引用，必须记在实例字段上（粘性）。理由有三层：后端按项失败时会自己丢掉那个操作（下次重试看到空缓冲）、异常可能在 gather 里不是传播出去的那个、而没有别的地方能记住它。只有成功的成对提交才清除该记录。

**源码事实：** `llm_response_cache` 串在 `text_chunks` 之后，后者失败时跳过它——这是**延迟而非丢弃**：两个缓冲都保留待处理状态给下一次提交，中止批次时会先刷缓存再丢弃。

**源码事实：** `strict_references=True` 把「提交返回但仍有操作被缓冲」也算失败，只允许在「没有下一次提交」的调用方（shutdown）使用。

**源码事实：** `_insert_done_with_cleanup` 在直插路径上丢弃待处理操作，因为一个永久失败的中毒操作留在共享缓冲里会让之后每次刷盘都重放失败；丢完必须 `raise` 让失败浮现。

**源码事实：** 该清理**禁止用于删除路径**：待处理删除被丢弃后没有东西会重新发出它（文档已移除、重处理不会跑），而那些向量与 KV 会永久残留且可被检索到。

**源码事实：** `_index_storages` 列十二个存储，次序上 `text_chunks` 在 `llm_response_cache` 之前（成对顺序）、`full_docs`/`doc_status` 在最前；`is not None` 过滤让简化部署靠留空配置降级而非分支。

**架构推论：** 插入流程的可靠性完全建立在**「可重造性」**这个判据上：插入的数据能从原文重造，所以「丢弃缓冲 + 标 FAILED + 重试」是正确恢复；删除的数据不能重造，所以同样的动作变成数据泄漏。**同一个清理动作在不同操作上的正确性相反**——这就是为什么代码里要有一条全大写的 WARNING 划出禁区。

**架构推论：** 「记住一个失败」这个需求（`_chunk_reference_commit_failed`）揭示了一个常见误解：**以为异常能携带全部信息**。异常只能到达一个处理者，而这里需要的是「之后所有重试都知道这件事」。当失败事实的存活期超过异常的生命周期时，就必须把它落到持久状态（哪怕是实例字段）上。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/lightrag.py` | `LightRAG` | 主类，组合三个 mixin 持有全部存储 |
| `lightrag/lightrag.py` | `ainsert` | SDK 便利入口（仅 F 策略），入队 + 处理 |
| `lightrag/lightrag.py` | `_process_extract_entities` | 抽取包装，守卫状态写以免掩盖真实错误 |
| `lightrag/lightrag.py` | `_flush_storages` | 三种提交结果的区分与两种失败记忆 |
| `lightrag/lightrag.py` | `_insert_done_with_cleanup` | 直插路径的丢弃清理与删除路径禁区 |
| `lightrag/lightrag.py` | `_index_storages` | 十二个存储的成对/先后次序 |
| `lightrag/lightrag.py` | `_record_chunk_reference_commit_failure` | 粘性记录引用可能丢失 |
| `lightrag/lightrag.py` | `_chunk_reference_commit_failed` | 实例字段，只有成对提交能清除 |
| `lightrag/pipeline.py` | `apipeline_enqueue_documents` | 入队：`ids` vs `docs_format`、准入令牌 |
| `lightrag/parser/routing.py` | `resolve_chunk_options` | 生成每文档的分块参数快照 |

## 参考

- [lightrag 源码：lightrag.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/lightrag.py)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [论文：LightRAG](https://arxiv.org/abs/2410.05779)
- [lightrag 文档：文件处理流水线](https://github.com/HKUDS/LightRAG/blob/main/docs/FileProcessingPipeline.md)

## 与主核 / AI 的关系

插入流程与主核的**写路径**面对同一个问题：一次逻辑写入要落到多个物理位置，而它们没有共同的原子提交点。而两者的答案不同——主核用 WAL 把它们变成一个原子操作，lightrag 用「声明残留位置 + 下次运行修复」代替。

**「可重造性」这个判据在主核里对应的是「操作是否幂等」。** 数据库的重做日志要求操作幂等（重放多少次结果一样），因为崩溃后必须能重放。lightrag 的插入天然幂等（同 id 同内容重写得到同一结果），所以能靠重试恢复；而删除在这里不幂等（删两次与删一次在「文档已不在」时看起来一样，但**丢弃那个删除请求**会让状态永远错下去）。

**「记住一个失败」对应数据库的 `pg_control` 里的「需要恢复」标志。** 两者都是「把一个失败事实从瞬时的异常提升为持久的状态」，因为它们必须在处理者退出后仍然有效。差别是数据库写在磁盘上（要跨进程崩溃），lightrag 写在实例字段上（只需跨重试）。

**「`is False` 而非真值判断」与 a01 那页的哨兵类是同一个模式**：在一个「返回 `None` 表示成功」的契约里，`False` 是一个**有信息的答复**，而 `0`、`""`、空列表这些假值不是。**用真值判断会把这三种答复混为一谈**——这正是 SQL 三值逻辑要解决的问题。

对 agent 工程的直接价值有三条。其一，**失败处理代码本身会成为新的错误源**。`async with None` 那个陷阱说明：在 `except` 块里做任何可能抛异常的操作，都会把真实错误替换掉。其二，**同一个清理动作在不同操作上的正确性可能相反**。丢弃待处理操作的判据是「这份数据能不能重造」，不是「这个操作成不成功」。其三，**被延迟不等于被丢弃**。`llm_response_cache` 因顺序被跳过，但两个缓冲都保留着——把「这次没做」和「这次放弃了」写成同一种代码，会让恢复路径消失。
