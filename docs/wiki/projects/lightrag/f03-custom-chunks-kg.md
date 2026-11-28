---
project: lightrag
feature: custom-chunks-kg
title: 自定义块与手工图谱写入
page_type: feature
schema_version: 1
refs:
  - path: lightrag/lightrag.py
    symbols: [ainsert_custom_chunks, ainsert_custom_kg]
  - path: lightrag/lightrag.py
    symbols: [arollback_failed_custom_chunk_patches, _rollback_one_custom_chunk_patch, _union_doc_recovery_anchors]
---

# 自定义块与手工图谱写入

## 一句话职责

两条「调用方自己准备好数据」的写入路径，而它们给出了两种相反的可靠性承诺：`ainsert_custom_chunks` 用 `doc_status.metadata` 里的**持久日志**换取可恢复（崩溃后能前滚或回滚），`ainsert_custom_kg` 则**明确声明自己不在恢复保证之内**——没有日志、没有预写锚点，因此中途崩溃留下的东西任何清理机制都发现不了。

## 机制是什么

**这两条路径的差别不是实现质量，而是「谁负责一致性」这个契约选择。**

| | `ainsert_custom_chunks` | `ainsert_custom_kg` |
|---|---|---|
| 持久日志 | 有（`doc_status.metadata["custom_chunk_patch"]`） | **无** |
| 恢复锚点 | 提交时并集写入 `full_entities` / `full_relations` | **不预写** |
| 崩溃后可发现 | 是（扫描能枚举日志行） | **否** |
| 恢复方向 | SDK 重试前滚 / 扫描回滚 | 靠离线审计工具事后重建 |
| 文档状态 | 有完整 `doc_status` 记账 | 只写图谱与向量 |

**没有事务时，「可恢复」的唯一实现方式是「让残留物出现在一个会被扫描的地方」。** 日志就是那个地方：它写在文档状态行里，而文档状态存储支持按状态分页枚举（见 a01 那页）。

`ainsert_custom_kg` 缺的正是这个落点，所以它的文档用了一个 `.. warning::` 区块直白地说明这一点。

## 怎么实现的

### 一、`ainsert_custom_chunks`：三种模式由文档状态推导

**它不是「插入」，而是「对已有文档的一次增量操作」，而模式取决于目标文档此刻的状态。**

```
- Target document absent -> **create** mode: the document is created
  with full doc_status bookkeeping (the legacy behavior of this
  deprecated API, now crash-discoverable).
- Target document ``PROCESSED`` -> **patch** mode: the chunks are added
  to the document; recovery anchors and ``chunks_list`` are unioned at
  commit, never overwritten.
- Target document carrying a journal for the SAME operation (same
  doc + same chunk set) -> resume/roll-forward; for a DIFFERENT
  operation -> rejected until the original is retried or rolled back
  by scan.
- Target document in any other state -> rejected.
```

**四条规则里有两条是关于「同一个操作」的**，而这需要操作身份可比。于是：

```
Identity is deterministic and document-scoped: chunk ids hash
(doc_id, chunk_content) and the operation id hashes the ordered
chunk-id set, so repeating the same logical input is idempotent
(committed -> no-op; failed -> resume).
```

**两级哈希构成身份链**：块 id 哈希 `(doc_id, chunk_content)`、操作 id 哈希有序的块 id 集合。于是：

- **同一份输入重复调用 → 同一个操作 id → 幂等**（已提交则什么都不做，失败则续做）。
- **不同输入 → 不同操作 id → 拒绝**（在原始操作被重试或回滚之前）。

**而「块 id 含 `doc_id`」这一层有一个具体的理由，代码注释写得很清楚：**

```
# Deterministic, document-scoped identity: chunk ids hash
# (doc_key, content) -- length-prefixed, see make_custom_chunk_id --
# so identical text in two documents never shares a chunk row (a
# shared row's full_doc_id would be clobbered, and rollback could
# delete another document's chunk).
```

**两个后果被逐一列出**：共享行会让 `full_doc_id` 被互相覆盖；而回滚时按块 id 删除会删到别人文档的块。**「长度前缀」这个细节也重要**——否则 `("ab", "c")` 与 `("a", "bc")` 会哈希出同一个 id。

**取消与失败后的处置也被明确规定：**

> 在日志已持久化之后发生失败/取消时，文档被标成 `FAILED` 而**日志（以及任何已暂存的数据）被保留**——同一个调用可以由 SDK 调用方重试，`/documents/scan` 可以回滚这次操作。**部分工作绝不会被盲目刷盘。**

**「部分工作绝不盲目刷盘」是这一整页的纲领**：它把「要么全落，要么留下可查的残迹」变成了一个可执行的规则。

**而调用方边界被显式保护。**

> 当嵌入函数声明了正的 `max_token_size` 时，一个超过它的块会在忙碌槽被预留或任何写入发生**之前**抛 `ChunkTokenLimitExceededError`，而不是被切分或截断。`chunk_token_size` 在这里不是边界。重试会在当前限制下再次检查，所以如果限制在失败操作之后被调低，它的重试会被拒绝而日志留下来给 `/documents/scan` 回滚。

**三个决定都指向「不擅自替调用方做主」**：

1. **超限就拒绝，不切分也不截断** —— 因为这是调用方自己切的块，切分它等于推翻调用方的边界。
2. **`chunk_token_size` 不算边界** —— 那是内部分块器的参数，与「嵌入模型的硬上限」是两件事。
3. **重试时用当前限制重新检查** —— 因为限制可能已变，而变更后的合规性必须重判。

### 二、`_union_doc_recovery_anchors`：为什么是并集而不是覆盖

**这个函数的文档把「为什么不能覆盖」说成了一个具体的删除场景：**

> 一个自定义块补丁**绝不能覆盖**基础文档的 `full_entities` / `full_relations` 行——基础文档继续拥有它此前已提交的贡献——所以补丁的候选被**并集**进去，两行都通过窄屏障刷盘。

```
entity_row = await self.full_entities.get_by_id(doc_id) or {}
union_names = set(entity_row.get("entity_names") or []) | set(entity_names)
await self.full_entities.upsert({doc_id: {"entity_names": sorted(union_names), "count": len(union_names)}})

relation_row = await self.full_relations.get_by_id(doc_id) or {}
union_pairs = {tuple(pair) for pair in (relation_row.get("relation_pairs") or [])} | {tuple(pair) for pair in relation_pairs}
await self.full_relations.upsert({doc_id: {"relation_pairs": [list(pair) for pair in sorted(union_pairs)], "count": len(union_pairs)}})

await self._flush_storages([self.full_entities, self.full_relations])
```

**这两个表就是「这份文档贡献了哪些实体/关系」的索引**，而它是删除文档时知道要清理什么的唯一依据。若补丁覆盖了它，基础文档的贡献就从索引里消失——**于是删除基础文档时不会清理那些实体，图谱里留下孤儿节点。**

**三个实现细节：**

1. **读-并-写，而不是 upsert 覆盖** —— 因为后端会整行替换。
2. **`sorted()` 让结果确定** —— 同样的输入集合总产生同样的行，于是重复执行幂等、且便于比对。
3. **`_flush_storages([...])` 是「窄屏障」** —— 只刷这两个存储，而不是全量刷盘。文档里称之为「预写屏障的提交步骤」。

**「预写」，因为它发生在真正的图/向量写入之前。** 若锚点写不进去就失败，那后续写入不会被误认为「可清理」；反之若写了锚点而图写失败，锚点只是多列了几个不存在的实体——**这是安全的方向**（多列可以事后清理，漏列会导致孤儿）。

### 三、`arollback_failed_custom_chunk_patches`：扫描侧的回滚

**这个函数的存在理由是一句对现实的承认：**

> 在 `/documents/scan` 开头调用的管理逃生口：SDK 调用方拥有前滚（重复同一个调用会续做它），而扫描把未完成的操作**回滚**到此前已提交的文档状态——**反复前滚可能一直失败，而日志行否则会永远脏着。**

**「反复前滚可能一直失败」是关键** —— 前滚依赖同样的代码路径，若失败原因是数据本身（比如一个块永远超限），重试永远不会成功。回滚是唯一的出路。

**它从 `doc_status` 直接发现候选，而不依赖输入文件：**

> 直接从 `doc_status` 发现候选（FAILED 或带 `custom_chunk_patch` 日志的陈旧 PROCESSING 行）——**SDK 操作可能没有扫描可见的输入文件**。

**而「PROCESSING 行在这里必然陈旧」这一判断有一个前提：**

> 一个 PROCESSING 行在这里必然是陈旧的：一个活着的操作持有 pipeline 的 `busy` 槽，而扫描预留拒绝在 `busy` 被持有时启动。

**这是「用一个已有的互斥来推断状态」的典型用法**：不是给每个操作加心跳，而是靠「谁持有 `busy`」这个更强的锁来推。

**每文档的操作在一个按文档键控的锁下进行，而那个锁与操作本身用的是同一把：**

```
async with get_storage_keyed_lock([doc_id], namespace=doc_lock_namespace, enable_logging=False):
    row = await self.doc_status.get_by_id(doc_id)
    journal = doc_status_custom_chunk_patch(row)
    if journal is None:
        continue
```

**锁内重新读取**，注释说明：「SDK 可能在发现与这里之间续做（并提交）了这个操作」。**这是一个「乐观发现 + 加锁复核」的两阶段**，而 `journal is None` 时的 `continue` 处理了「发现之后被提交了」的情形。

**而报告被刻意限长。**

> 计数是精确的；id 列表被限制在 `ROLLBACK_REPORT_SAMPLE_CAP` 之内，因为**一个每个日志化文档都长一条的报告会重新引入这个扫荡存在的理由所要消除的 O(N) 累积。**

**「这个扫荡存在的理由」** —— 也就是说这个函数本身就是为了避免 O(N) 累积而写的，若它的返回值又变成 O(N) 就自相矛盾。**这是一个关于「修复本身不能带上被修复的缺陷」的自我约束。**

### 四、`_rollback_one_custom_chunk_patch`：顺序里全是依赖

**第一步是「在删除之前收集」。**

```
# Operation-scoped LLM cache ids must be collected BEFORE the purge
# deletes the staged chunk rows that reference them.
cache_ids: list[str] = []
if staged_chunk_ids:
    staged_rows = await self.text_chunks.get_by_ids(staged_chunk_ids)
    for staged_row in staged_rows:
        if isinstance(staged_row, dict):
            cache_ids.extend(cid for cid in (staged_row.get("llm_cache_list") or []) if isinstance(cid, str) and cid)
    cache_ids = list(dict.fromkeys(cache_ids))
```

**块行里存着它引用过的 LLM 缓存 id 列表**（`llm_cache_list`），而清理块行之后这个引用就没了。所以必须先读出、再去删。

**`list(dict.fromkeys(...))` 是一个保留顺序的去重**（Python 3.7+ 的 dict 保序）。用 `set()` 会丢顺序，而对「待删除 id 列表」来说顺序无关但可读性更好——更重要的是它是一次遍历完成，不需要先 set 再 list。

**第二步是清贡献，而 `patch_only` 标志区分两种模式。**

```
rebuild_report = await self._purge_kg_contributions(
    doc_id, staged_chunk_ids,
    candidate_entities=candidate_entities,
    candidate_relations=candidate_relations,
    patch_only=(mode == "patch"),
    rebuild_policy="rollback",
    pipeline_status=..., pipeline_status_lock=...,
)
```

**`patch_only=True` 表示「只去掉这次补丁带进来的贡献，保留基础文档的」** —— 而 `create` 模式下整份文档都要去掉，所以是 `False`。

**这也解释了为什么回滚需要「候选实体/关系」这个输入**：只有知道这次补丁引入了哪些，才能只删那些。而这些候选来自日志（`journal.get("entity_names")` / `relation_pairs`）。

**第三步删缓存，而刷盘是成对的。**

```
if cache_ids and self.llm_response_cache:
    await self.llm_response_cache.delete(cache_ids)
    # text_chunks joins the flush so the pair is ordered and fenced:
    # publishing this namespace publishes every buffered extract row,
    # not only the ids deleted above.
    await self._flush_storages([self.text_chunks, self.llm_response_cache])
```

**注释说明了为什么要连带刷 `text_chunks`**：「发布这个命名空间会发布**每一**条缓冲的抽取行，而不只是上面删掉的那些 id」。

**也就是说删除是逐个 id 的，而刷盘是整个命名空间的** —— 两者粒度不同。于是必须保证 `text_chunks` 先落盘，否则缓存行可能指向尚未落盘的块（与 f02 那页的成对顺序是同一个规则）。

**第四步，两种模式的收尾完全不同。**

**`create` 模式：整份文档本来不存在，于是删掉它的一切。**

```
if mode == "create":
    # ... The doc_status delete MUST be flushed here: unlike upsert
    # (immediate-persist), delete is deferred-commit on the JSON
    # backend, and reporting a rollback as successful while the FAILED
    # journal row can reappear after a crash would be a false success.
    await self.full_docs.delete([doc_id])
    await self.doc_status.delete([doc_id])
    await self._flush_storages([self.full_docs, self.doc_status])
    return
```

**「delete 在 JSON 后端是延迟提交」这个事实导致了必须显式刷盘。** 若不刷，回滚返回「成功」但崩溃后 FAILED 日志行会重新出现——**那就是一个假成功**。

**`patch` 模式：恢复基础文档到 PROCESSED，但要先剪掉不再拥有的锚点。**

```
staged_set = set(staged_chunk_ids)
base_chunk_ids = {cid for cid in ((row or {}).get("chunks_list") or []) if isinstance(cid, str) and cid and cid not in staged_set}
await self._prune_doc_recovery_anchors(doc_id, candidate_entities, candidate_relations, owned_chunk_ids=base_chunk_ids)
cleaned_chunks = [cid for cid in ((row or {}).get("chunks_list") or []) if isinstance(cid, str) and cid and cid not in staged_set]
```

**`owned_chunk_ids=base_chunk_ids` 是「剪枝时还要考虑哪些块仍被基础文档拥有」** —— 因为一个候选实体可能同时被基础文档的块引用，此时不能删它。

**这个「剪枝」与 `_union_doc_recovery_anchors` 的「并集」是一对偶操作**，而两者都必须按「是否仍被拥有」判定，不能盲目增删。

**而最后一个修正是本页最细的一处。**

```
# The FAILED row this restore copies from carries the failed attempt's
# llm_truncation stamp, describing extractions the purge above just
# removed -- restoring it verbatim would leave a clean base document
# permanently advertising truncation from rolled-back content. The
# journal's pre-operation snapshot is the authority: reinstate it, or
# drop the key when the base never truncated. A journal written before
# the snapshot existed has no key and changes nothing (its attempts
# never stamped a tally either).
truncation_restore: dict[str, Any] = {}
if "prior_llm_truncation" in journal:
    prior_truncation = journal["prior_llm_truncation"]
    if prior_truncation is None:
        truncation_restore["metadata_drop"] = (LLM_TRUNCATION_METADATA_KEY,)
    else:
        truncation_restore["metadata_extra"] = {LLM_TRUNCATION_METADATA_KEY: prior_truncation}
```

**问题：恢复时是从 FAILED 行复制的，而 FAILED 行带着「失败那次尝试的截断印记」。** 那个印记描述的是刚刚被清掉的抽取——**若原样恢复，一份干净的基础文档会永久标记着「被截断过」。**

**解法：日志里的「操作前快照」才是权威。** 三态：

| 日志里有 `prior_llm_truncation`？ | 值 | 恢复动作 |
|---|---|---|
| 没有这个键 | — | 什么都不做（旧版日志，其尝试从未写过印记） |
| 有，值为 `None` | 基础文档本来没截断 | **删掉**这个键 |
| 有，有值 | 基础文档本来有截断印记 | 恢复那个值 |

**「旧版日志没有这个键所以什么都不做」是向前兼容的写法**，而注释解释了它的正确性：那个版本的尝试也没写过印记，所以没有需要恢复的东西。

**这是一个「元数据的真实性」问题**：`llm_truncation` 印记是一个**关于数据质量的断言**，而回滚之后它必须回到「操作前」的真值，不能停留在「失败时」的值。

### 五、`ainsert_custom_kg`：一个把自己排除在保证之外的写路径

**它的 warning 区块直白到不寻常。**

```
.. warning:: (direct-writer audit)
   This path is OUTSIDE the document-level recovery guarantee. It has
   no durable operation journal and does not prewrite
   ``full_entities`` / ``full_relations`` recovery anchors, so a
   crash or partial failure mid-call can leave graph/vector/tracking
   contributions that per-document purge, retry, and scan rollback
   cannot discover. Use the journaled ingestion paths (``ainsert`` /
   ``ainsert_custom_chunks``) when crash recovery matters; the
   offline ``lightrag.tools.kg_integrity_repair`` audit can
   reconstruct anchors for data written here after the fact.
```

**它列了「缺什么、后果是什么、该用什么替代、以及补救手段」四件事。** 而补救手段是**离线审计工具**——即承认这个洞只能事后修，不能在线修。

**而它仍然做了三件事：**

**第一，写前校验。** 名字归一化、`file_path` 校验（含孤立代理项、`U+FFFE`/`U+FFFF`、除制表/换行/回车外的控制字符）在**任何存储写入之前**完成，违规就抛 `ValueError` 且什么都不写。

**第二，写已有实体/关系时「替换描述、合并溯源」。**

> 写入一个已存在的实体或关系会替换它的描述、类型与关键词，但**合并**它的溯源：自定义块加入 `entity_chunks` / `relation_chunks` 行、图的 `source_id` 与 `file_path`，而已有的关系端点会得到这个关系的块。一个被合并的关系保留 `max(自定义权重, 已存权重, 证据数)`。

**而文档给出了「不合并会怎样」的具体后果：**

> 没有合并的话，删除一个更早的文档会把它一起删掉（见 `docs/ProgramingWithCore.md`，*Insert Custom KG*）。

**这正是 `_union_doc_recovery_anchors` 那条规则的同一个道理**：溯源是「谁引用了我」的多值属性，覆盖等于丢失其他引用者的所有权。

**第三，证据数构成权重下限。**

> 一个关系的权重下限是它的**不同真实证据来源数**。显式权重可以提升这个基线；省略关系的 `source_id` 则使用一个无来源的分数权重。

**「证据数当权重下限」是一个有意思的设计**：它让「被更多文档引用」自动意味着「更重要」，而显式权重只能往上调不能往下压。**这防止了一次人为指定把一条被 20 篇文档引用的关系降到最低。**

**而 `max(自定义权重, 已存权重, 证据数)` 这个三路取大是合并时的最终规则。**

### 六、`_upsert_custom_chunk_status`：整行重建的原因

**doc_status 后端的 upsert 会替换整个记录（含 `metadata` 块），所以必须从 `base_row` 重建全字段。**

```
if base_row:
    metadata = dict(base_row.get("metadata") or {})
    payload: dict[str, Any] = {
        "status": status,
        "content_summary": base_row.get("content_summary") or "",
        "content_length": base_row.get("content_length") or 0,
        "created_at": base_row.get("created_at") or now_iso,
        "updated_at": now_iso,
        "file_path": base_row.get("file_path") or file_path or normalize_document_file_path(""),
        "track_id": base_row.get("track_id"),
        "content_hash": base_row.get("content_hash"),
        "metadata": metadata,
    }
```

**九个字段全部重建，而其中三个有「优先用 base、回退到参数」的逻辑**（`file_path` 尤其明显）。注释说明：「基础文档的元数据逐字携带，只加上 `custom_chunk_patch` 键（日志）或去掉它（提交时）」。

**「返回 upsert 后的载荷」这一设计让连续状态转换不必重读：**

```
Returns the upserted payload so consecutive transitions can pass it back
in as ``base_row`` without refetching.
```

**一个操作会经历 PENDING/PROCESSING/FAILED/PROCESSED 多次转换，每次都重读会浪费一次往返**（而且每次都在 `busy` 持有期间）。**把「上一行的内容」当作下次的输入，是把读-改-写折叠成写-写。**

## 关键取舍

**源码事实：** 两条自定义写入路径的可靠性承诺相反：`ainsert_custom_chunks` 有持久日志 + 预写锚点（崩溃可发现、可前滚/回滚），`ainsert_custom_kg` 两者皆无，其 `.. warning::` 明确声明「在这次调用中途崩溃或部分失败会留下 per-document purge、重试和扫描回滚都无法发现的贡献」，补救靠离线审计工具。

**源码事实：** 自定义块的三种模式由目标文档状态推导（不存在 → create、PROCESSED → patch、有同操作日志 → 前滚、有不同操作日志 → 拒绝、其它 → 拒绝）。

**源码事实：** 身份是确定性且文档作用域的：块 id 哈希 `(doc_id, chunk_content)`（长度前缀，避免 `("ab","c")` 与 `("a","bc")` 撞）而操作 id 哈希有序块 id 集合。于是重复同一逻辑输入幂等，不同输入被拒绝。

**源码事实：** 块 id 含 `doc_id` 有两个后果被注释出来：共享行会让 `full_doc_id` 被互相覆盖；按块 id 回滚删会删到别的文档。

**源码事实：** 嵌入超限的块在预留 `busy` 或任何写入之前抛 `ChunkTokenLimitExceededError`——**不切分也不截断**（因为这是调用方自己切的块）；`chunk_token_size` 不是这里的边界；重试按当前限制重新检查，若限制被调低则拒绝重试并保留日志给扫描回滚。

**源码事实：** 日志持久化之后失败/取消，文档标 FAILED 而日志与已暂存数据保留；「部分工作绝不会被盲目刷盘」。

**源码事实：** `_union_doc_recovery_anchors` 必须**并集**而非覆盖，否则删除基础文档时不会清理那些实体、图谱留下孤儿。它用「读-并-写 + `sorted()` 保证确定性 + 窄屏障刷盘」，且刷盘发生在真正的图/向量写入之前（预写）：漏列会造成孤儿，多列可以事后清——方向是安全的。

**源码事实：** `arollback_failed_custom_chunk_patches` 从 `doc_status` 直接发现候选（SDK 操作可能没有扫描可见的输入文件）；「PROCESSING 行必然陈旧」的依据是「活着的操作持有 `busy`，而扫描预留拒绝在 `busy` 被持有时启动」。

**源码事实：** 回滚每文档在操作所用的同一把键控锁下进行，且锁内重读日志（SDK 可能在发现与回滚之间提交了它），`journal is None` 时 `continue`。

**源码事实：** 回滚报告限长到 `ROLLBACK_REPORT_SAMPLE_CAP`，因为「每个日志化文档都长一条的报告会重新引入这个扫荡存在的理由所要消除的 O(N) 累积」。

**源码事实：** `_rollback_one_custom_chunk_patch` 的顺序有依赖：先读块行收集 `llm_cache_list`（清理块后就没了）、再清贡献（`patch_only` 决定是否保留基础文档的）、再删缓存并**成对刷** `text_chunks`（因为删除是逐 id 的而刷盘是整个命名空间的）、最后按模式收尾。

**源码事实：** `create` 模式的 `doc_status.delete` 必须显式刷盘——JSON 后端的 delete 是延迟提交，不刷的话回滚会「报告成功但崩溃后 FAILED 日志行重新出现」。

**源码事实：** `patch` 模式恢复前要剪掉不再拥有的锚点（`owned_chunk_ids=base_chunk_ids`），因为一个候选实体可能仍被基础文档的块引用。

**源码事实：** 恢复时必须用日志里的 `prior_llm_truncation` 快照修印记，否则「一份干净的基础文档会永久标记着被截断过」。三态处理（无键 → 不动、值为 None → 删键、有值 → 恢复）且对旧版日志向前兼容。

**源码事实：** `ainsert_custom_kg` 的合并规则是「替换描述/类型/关键词，合并溯源」，权重取下限为真实证据来源数、合并时取 `max(自定义, 已存, 证据数)`。不合并的后果是「删除一个更早的文档会把它一起删掉」。

**源码事实：** `_upsert_custom_chunk_status` 必须整行重建（后端 upsert 替换整个记录含 `metadata`），且返回 upsert 后的载荷让连续状态转换不必重读。

**架构推论：** 「无事务时可恢复性 = 残留物出现在会被扫描的地方」这条规律在这两条路径上得到了对照验证。日志写在 `doc_status.metadata` 里而不是单独的日志表，正是因为它所在的存储**天然支持按状态分页枚举**——`FAILED` 与 `PROCESSING` 都是扫描的常规输入。**选择把日志放哪里，其实是选择「谁会来发现它」。**

**架构推论：** 回滚的每一步都在修一个「顺序依赖」：缓存 id 必须在删块前读、贡献必须在删块前清、`text_chunks` 必须在缓存前刷。这类代码的正确性无法靠局部推理验证，只能靠「每一步的注释都说明它在保护什么」来维持。**顺序依赖密集的清理逻辑，注释是不可省的基础设施。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/lightrag.py` | `ainsert_custom_chunks` | 三种模式、确定性身份、日志写入、失败保留 |
| `lightrag/lightrag.py` | `ainsert_custom_kg` | 无日志的直写路径与其 `.. warning::` 声明 |
| `lightrag/lightrag.py` | `_union_doc_recovery_anchors` | 并集写锚点 + 窄屏障刷盘（预写） |
| `lightrag/lightrag.py` | `_prune_doc_recovery_anchors` | 与并集对偶的剪枝，按「是否仍被拥有」判定 |
| `lightrag/lightrag.py` | `arollback_failed_custom_chunk_patches` | 扫描侧回滚，限长报告 |
| `lightrag/lightrag.py` | `_rollback_one_custom_chunk_patch` | 顺序敏感的逐步回滚 |
| `lightrag/lightrag.py` | `_purge_kg_contributions` | 按候选清除贡献（`patch_only` 区分模式） |
| `lightrag/lightrag.py` | `_upsert_custom_chunk_status` | 整行重建，返回载荷免重读 |
| `lightrag/lightrag.py` | `prior_llm_truncation` | 回滚时的元数据真值恢复 |
| `lightrag/utils_pipeline.py` | `make_custom_chunk_id` | 长度前缀的 `(doc_id, content)` 哈希 |

## 参考

- [lightrag 源码：lightrag.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/lightrag.py)
- [lightrag 文档：用核心编程](https://github.com/HKUDS/LightRAG/blob/main/docs/ProgramingWithCore.md)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [论文：LightRAG](https://arxiv.org/abs/2410.05779)

## 与主核 / AI 的关系

这两条路径与主核的**两条写入口**形成精确对照：一条走完整的 WAL + 事务（可靠但贵），一条走「直接写文件」（快但不保证）。数据库里后者叫 `COPY ... WITH (FREEZE)` 或未记日志的表——**它们的存在理由都是「批量写入时日志开销不可接受」。**

**「日志写在会被扫描的地方」对应数据库的「把 undo 放在数据页里」。** 传统数据库把 undo 放在独立的回滚段，崩溃后由恢复进程扫描；而某些内存数据库把 undo 放在数据页头部，靠遍历页来恢复。**选择哪种取决于「谁会来发现它」**——lightrag 选文档状态存储，因为扫描本来就要遍历它。

**`prior_llm_truncation` 的三态恢复对应数据库的「补偿日志（CLR）里的 `UNDO_NEXT`」。** 事务回滚时，一个已被部分回滚的操作需要一个精确的「恢复到哪个状态」记录，而不能靠「再执行一次逆操作」——因为逆操作不幂等。这里的表现是：印记不能「关掉」，必须恢复成操作前的具体值。

**离线审计工具（`kg_integrity_repair`）对应主核的 `pg_amcheck` / `ANALYZE` 这类离线校验器。** 承认在线路径有洞、并用离线工具兜底，是一个成熟的取舍——但前提是那个洞被**明确记录在文档里**，而不是靠使用者自己发现。

对 agent 工程的直接价值有三条。其一，**可靠性承诺必须显式声明**。两条路径一条有日志一条没有，而后者用 `.. warning::` 把「你放弃了什么、该用什么替代」写清楚——这比让使用者以为两条路径一样可靠要好得多。其二，**溯源是并集属性，覆盖即丢失**。「谁引用了这个实体」是多值事实，任何覆盖式写入都会让某些引用者的所有权消失，进而让删除操作留下孤儿。其三，**回滚顺序无法靠局部推理保证**。缓存 id 要在删块前读、贡献要在删块前清、块要在缓存前刷——每一步都在保护一步，所以每一步都需要一句注释说明它在保护什么。
