---
project: lightrag
feature: kg-rebuild
title: 图谱重建与抽取缓存
page_type: feature
schema_version: 1
refs:
  - path: lightrag/operate.py
    symbols: [rebuild_knowledge_from_chunks, _rebuild_from_extraction_result, _reextract_chunks]
  - path: lightrag/operate.py
    symbols: [_get_cached_extraction_results, _rebuild_single_entity, _rebuild_single_relationship]
---

# 图谱重建与抽取缓存

## 一句话职责

删掉一个文档后，图谱里那些「部分由它支撑」的实体/关系不能直接删（别的文档也提到了）也不能不动（描述里还含着已删内容）——所以要从**幸存块**的抽取缓存重新拼出它们。这一层管的就是这个重建：哪些块要重抽、缓存行怎么按 `llm_cache_list` 归位、以及**没有任何幸存证据时保留还是删除**。

## 机制是什么

图谱存储的是**合并后的**实体：一份 `description`、一个 `source_id` 列表、一个 `entity_type`。它不含「哪个块贡献了哪句话」。所以「删掉文档 D」这件事无法通过「从实体上减去 D 的贡献」实现——**合并是单向的**。

唯一的办法是**用剩下的证据重建**：拿幸存块的抽取缓存，重新解析，重新合并，覆盖上去。这也是 d04 那条「归一化只能 MERGE 不能 SPLIT」约束的来源——重建要靠**名称匹配**把新解析出的记录对上已存节点。

这个方案有三处难点，构成了这一页的全部复杂度：

1. **缓存可能不在。** 缓存行可能被清理（`LLM cache cleared`）、可能是别的后端不支持、可能是 gleaning 行而不是初始抽取行。所以需要「重抽」这条贵路径，而它是**要调 LLM 的**。
2. **要重建的集合是「候选」，不是「事实」。** 传进来的 `entities_to_rebuild` 是「这个文档可能碰过哪些实体」，其中有些其实全靠别的文档支撑。所以重建过程会发现「没有幸存证据」。
3. **「没有证据」不等于「该删」。** 一个实体可能因为**改名**或**合并**而失去证据（d04 讲过），此时删掉它是错的。所以默认策略是**保留语义字段、只修 provenance**。

第三点是这一页最重要的判断，源码把它写成了默认行为而不是可选项。

## 怎么实现的

### 两种策略，而它们的差别是「能不能调 LLM」

```python
Policies:

* ``best_effort`` (document deletion and resume purge): a surviving chunk whose INITIAL
  extraction is missing, is a gleaning row, or does not parse is re-extracted through
  ``extract_entities`` before any object is rebuilt, which calls the LLM; a
  re-extraction failure raises. A missing or unparseable gleaning row is accepted as
  gleaning that did not run. Chunks written by ``ainsert_custom_kg`` are never
  re-extracted. Cached records go through ``kg_extraction_validator`` like fresh ones.
* ``rollback`` (custom-chunk patch rollback): never calls the extraction LLM; missing or
  unusable extraction data is recorded in the report.
```

四处：

1. **`best_effort` 用于「文档删除」与「恢复清理」** —— 这两个场景下用户期待「尽量保住知识」，所以允许花 LLM 调用。
2. **`rollback` 用于「自定义 chunk 补丁回滚」** —— 回滚必须**可预测且无副作用**，所以绝不调 LLM。
3. **两种策略对「坏数据」的处理方向相反**：`best_effort` 重抽（修），`rollback` 只记录（不修）。
4. **`ainsert_custom_kg` 写的块永不重抽** —— 因为那是**用户提供的文本**，在「别人的删除操作」里把它送去 LLM 是不可接受的（隐私与费用）。

   源码在正文里又强调了一次：

```python
if chunk_id in custom_kg_chunk_ids:
    # User-supplied content: never sent to the LLM on someone else's
    # deletion. Its objects fall back like any unattested object.
    report.missing_cache_chunk_ids.add(chunk_id)
    continue
```

**「on someone else's deletion」这个措辞很准确**：用户 A 删除自己的文档，不该导致用户 B 通过 API 塞进来的内容被送去 LLM。

**策略的分派在主循环里是一处 `if rebuild_policy == "rollback": ... continue` 早退**，所以两种策略共享「加载缓存行」与「写回」的部分，只有「中间怎么得到 records」不同。

### 加载缓存行：归位靠 `llm_cache_list` 而不是行自己的 `chunk_id`

```python
"""Load each requested chunk's extract cache rows.

Returns ``(extract_rows, chunk_data_by_id)``. ``extract_rows`` maps every chunk whose
row exists to its extract keys in ``llm_cache_list`` order -- call order, so an initial
extraction precedes its gleaning -- each with its cache row, or ``None`` when the row is
gone or is not an extract row. A key is an extract key when its flattened form says so;
a legacy key that does not parse counts only when its row does.

Rows are attributed through ``llm_cache_list``, never through the row's own ``chunk_id``:
that field names only the row's first writer, and chunks of different documents with
identical text share one row.
"""
```

**这段是本页最关键的机制说明，值得逐句拆：**

1. **`llm_cache_list` 的顺序就是调用顺序** —— 因为 d01 的 `extract_entities` 是「先初始抽取、再 gleaning」，而每次调用的 key 按顺序追加到这个列表。所以 `entries[0]` 是初始抽取，`entries[1:]` 是 gleaning。

   **这就是「初始抽取在第一位」这个假设的根据** —— 而不是靠行的类型判断。而 `_is_gleaning_extract_row`（d01）用 prompt 尾行来判，是**辅助**判断（用于发现「第一位其实是 gleaning 行」这种异常）。

2. **「A key is an extract key when its flattened form says so」** —— 缓存 key 是「扁平化」的字符串（由内容哈希 + 类型等构成），可以从 key 本身看出它是不是 `extract` 类型。这是零成本的过滤。

3. **「a legacy key that does not parse counts only when its row does」** —— 旧格式 key 解不出来，所以改为看行本身是不是抽取行。**这是「元数据不可信时看数据」的降级**。

4. **绝不能按行自己的 `chunk_id` 归位**，理由很硬：**「that field names only the row's first writer, and chunks of different documents with identical text share one row」**。

   **即：缓存是按内容哈希去重的。** 两个不同文档里有完全相同的块 → 共享同一行缓存 → 行的 `chunk_id` 只记第一个写入者。

   **所以按 `chunk_id` 归位会让第二个文档的块「找不到缓存」**（因为行上的 id 是第一个文档的）。必须反过来：从每个块的 `llm_cache_list` 出发去找行。

   这是一处很具体的「缓存去重与归属追踪冲突」——去重是为了省钱，而归属需要每块自己的索引。

### 主循环：三态分派

```python
for chunk_id in sorted(all_referenced_chunk_ids):
    chunk_data = chunk_data_by_id.get(chunk_id)
    if chunk_data is None:
        report.missing_cache_chunk_ids.add(chunk_id)
        continue
```

**先判块文本是否还在** —— 不在了就没法解析或重抽（没有输入文本）。所以这属于「证据不可用」而非「可重抽」。

```python
if rebuild_policy == "rollback":
    present = [(key, row) for key, row in entries if row is not None]
    if not present:
        report.missing_cache_chunk_ids.add(chunk_id)
        continue
    try:
        for _, row in present:
            _merge_chunk_records(nodes, edges, *await _parse_extract_row(...))
    except Exception as e:
        report.unusable_cache_chunk_ids.add(chunk_id)
        ...
    await _finish_cached_chunk(chunk_id, chunk_data, nodes, edges)
    continue
```

三处：

1. **`rollback` 处理的是「所有 `row is not None` 的行」** —— 不区分初始与 gleaning（因为不重抽，所以不必知道谁是第一个）。

2. **解析失败记 `unusable_cache_chunk_ids` 后**继续**（不 `continue`）** —— 它仍调 `_finish_cached_chunk`，所以「部分解析成功」的记录会被采纳。这与 `best_effort` 的「初始行解析失败就重抽」不同。

3. **`report` 是唯一的输出** —— 因为 `rollback` 不能调 LLM，所以「拿不到证据」只能如实上报。

`best_effort` 分支：

```python
initial_key, initial_row = entries[0] if entries else (None, None)
if initial_row is None or _is_gleaning_extract_row(initial_row):
    to_reextract[chunk_id] = None
    continue
try:
    _merge_chunk_records(nodes, edges, *await _parse_extract_row(text_chunks_storage, initial_row, chunk_id, chunk_data))
except Exception as e:
    logger.warning(f"Unparseable initial extraction row {initial_key} for chunk {chunk_id}; re-extracting: {e}")
    to_reextract[chunk_id] = initial_key
    continue
for gleaning_key, gleaning_row in entries[1:]:
    if gleaning_row is None:
        continue  # lost gleaning: accepted, see the contract
    try:
        _merge_chunk_records(...)
    except Exception as e:
        logger.warning(f"Ignoring unparseable gleaning row {gleaning_key} for chunk {chunk_id}: {e}")
```

五处：

1. **`entries[0]` 缺失或第一行其实是 gleaning 行 → 重抽**（`to_reextract[chunk_id] = None`，`None` 表示「没有要删的坏行」）。

   **为什么第一行是 gleaning 也要重抽？** 因为 gleaning 是对初始抽取的补充，没有初始抽取就没有「完整证据」。源码的文档说：「a surviving chunk whose INITIAL extraction is missing, is a gleaning row, or does not parse is re-extracted」。

2. **初始行不可解析 → 记下它的 key 并重抽**（`to_reextract[chunk_id] = initial_key`）。**记 key 是为了随后删掉它**——否则重抽后它还在，下次仍会先读到它。

3. **gleaning 行缺失或被忽略**（`continue` + 只有 warning）—— 文档说「A missing or unparseable gleaning row is accepted as gleaning that did not run」。

   **这是「不可恢复的缺失可以接受」的判断**：gleaning 是「补充召回」，缺了只降低召回，不会让证据错误。而初始抽取缺了就没有证据。

4. **两种失败用不同措辞**：初始行「re-extracting」（要修），gleaning 行「Ignoring」（不修）。

5. **`_merge_chunk_records` 用 `*` 展开两个返回值** —— 所以它是 `_merge_chunk_records(nodes, edges, new_nodes, new_edges)`。这个名字有点误导（看起来像在合并块），实际是「把一个行的 records 折进本块的累积表」。

### 重抽：`_reextract_chunks` 的三步准备

```python
"""...
Before the call, an unparseable initial row is deleted (re-extracting would otherwise
hit it again) and the chunk's extract keys are dropped from its ``llm_cache_list``, so
the keys the new run attaches, in call order, make the new initial extraction the first
one again. Raises on any failure.
"""
```

三步：

```python
stale_rows = [key for key in to_reextract.values() if key]
if stale_rows:
    await llm_response_cache.delete(stale_rows)

reset_rows: dict[str, dict[str, Any]] = {}
for chunk_id in to_reextract:
    row = chunk_data_by_id[chunk_id]
    extract_keys = {key for key, _ in extract_rows.get(chunk_id, [])}
    cache_list = list(row.get("llm_cache_list") or [])
    kept = [key for key in cache_list if key not in extract_keys]
    if kept != cache_list:
        reset_rows[chunk_id] = {**row, "llm_cache_list": kept}
if reset_rows:
    await text_chunks_storage.upsert(reset_rows)
    chunk_data_by_id.update(reset_rows)
```

四处：

1. **删掉坏的缓存行** —— 「re-extracting would otherwise hit it again」。因为 `use_llm_func_with_cache` 会先查缓存，若坏行还在就会直接返回它。

2. **从 `llm_cache_list` 里摘掉所有 extract key** —— 注意是**所有**（不只是坏的那个），因为重抽会产出新的初始行 + 新的 gleaning 行，旧的都不该留着。

3. **这样「新 run 附加的 key 按调用顺序」就让新初始抽取重新成为第一个** —— 所以前提 1（`entries[0]` 是初始抽取）被**重建**了。

   **这是一处很精细的修复**：它不是「标记谁是初始」，而是**通过清空并按顺序重建来恢复不变量**。

4. **`chunk_data_by_id.update(reset_rows)`** —— 内存里的副本也更新，否则后面的 `extract_entities` 拿到的是旧的 `llm_cache_list`。

```python
results = await extract_entities(
    {chunk_id: chunk_data_by_id[chunk_id] for chunk_id in chunk_ids},
    global_config, pipeline_status=..., llm_response_cache=..., text_chunks_storage=...,
)
return dict(zip(chunk_ids, results))
```

**复用 `extract_entities`（d01）而不是另写一套** —— 所以重抽与正常抽取走完全相同的校验、gleaning、多模态注入逻辑。这是正确的（否则两条路径会漂移）。

而 `dict(zip(chunk_ids, results))` 依赖 d01 那个「结果必须按 `ordered_chunks` 顺序」的修复 —— **若 d01 没修那个 set 顺序 bug，这里就会把结果对错块**。

**`publish` 的消息给了一条明确的运维建议。**

```python
await publish(
    f"Re-extracting {len(to_reextract)} surviving chunk(s) whose initial extraction "
    f"cache is missing or unusable, from document(s): {sample}. "
    "Reprocess these documents if the knowledge graph lost facts.",
    logging.WARNING,
)
```

**「Reprocess these documents if the knowledge graph lost facts」** —— 即：重抽可能有信息损失（LLM 输出不保证与上次一致），所以告诉运维「如果发现知识少了，重新处理这些文档」。

### 顺序：关系先，实体后

```python
try:
    # Phase 1: relations. On any failure every sibling is cancelled and
    # drained before the first exception propagates, and no entity is
    # touched.
    await wait_tasks_with_drain(
        [asyncio.create_task(_locked_rebuild_relationship(src, tgt, chunk_ids))
         for (src, tgt), chunk_ids in relationships_to_rebuild.items()],
        context="KG rebuild (relations)",
    )
    # Phase 2: entities, which may now read the already-rebuilt edges.
    await wait_tasks_with_drain(
        [asyncio.create_task(_locked_rebuild_entity(entity_name, chunk_ids))
         for entity_name, chunk_ids in entities_to_rebuild.items()],
        context="KG rebuild (entities)",
    )
except Exception:
    await _publish(_summary("KG rebuild failed"), logging.ERROR)
    raise
```

**顺序的理由在入口文档里**：「Relations are rebuilt first and entities only after every relation succeeded, so an entity known only through its edges reads edges that are already rebuilt」。

**即：有些实体只通过关系被知道（d02 讲过关系端点会创建 `UNKNOWN` 实体）。这些实体自己的描述是靠「从边汇总」得到的（`_rebuild_single_entity` 的 `summarize_from_edges` 分支）。所以边必须先重建完。**

**而「关系阶段失败则实体完全不动」** —— `wait_tasks_with_drain` 在任何失败时取消并排空所有兄弟任务，然后抛出，所以 Phase 2 不会开始。

**这是一个「先决条件明确」的分阶段设计**：不是「尽量做完」，而是「前一阶段全成功才做后一阶段」。

**两处锁都是按名键控，关系用排序键。**

```python
sorted_key_parts = sorted([src, tgt])
async with get_storage_keyed_lock(sorted_key_parts, namespace=namespace, enable_logging=False):
```

与 d02 一致（无向图，两方向是同一把锁）。而实体锁是 `[entity_name]`。

### `_rebuild_single_entity`：四分支

**入口先确认节点还在。**

```python
current_entity = await knowledge_graph_inst.get_node(entity_name)
if not current_entity:
    return False
```

**返回 `False` 表示「没降级」** —— 注意这个返回值的语义是「是否 degraded」，而不是「是否成功」。节点不存在时返回 `False`（不算降级，因为它本来就不在）。

**然后 `_update_entity_storage` 是一个内嵌的写回函数。**

```python
updated_entity_data = {
    **current_entity,
    "description": final_description,
    "entity_type": entity_type,
    "source_id": GRAPH_FIELD_SEP.join(source_chunk_ids),
    "file_path": GRAPH_FIELD_SEP.join(file_paths) if file_paths else current_entity.get("file_path", "unknown_source"),
    "created_at": int(time.time()),
    "truncate": truncation_info,
}
```

三处：

1. **`**current_entity` 打底** —— 所以节点的其他字段（可能由插件/手工加的）被保留。**这是「只改我负责的字段」的做法**。

2. **`created_at` 被刷新成现在** —— 注意这是**重建时间**，不是创建时间。所以这个字段在重建后失去「创建于何时」的含义。**这是一处语义漂移**（字段名说是创建，实际是「最后一次写入」）。

3. **`truncate` 记录截断信息** —— 与 d02 的 `truncate` 字段同义。

而写序与 d02 一致：先构造并校验 VDB 载荷，再写图，最后写 VDB（带重试）。

**第一个分支：无任何幸存证据 → 保留语义字段、只修 provenance。**

```python
if not all_entity_data:
    edges = [] if structural_fallback or not summarize_from_edges else await knowledge_graph_inst.get_node_edges(entity_name)
    if not edges:
        # Kept, not removed: no surviving chunk's records name it, which
        # also happens to a renamed or merged entity, so absence of
        # evidence is not evidence that only deleted chunks supported it.
        logger.warning("No cached entity data found for `%s`; preserving semantic fields while repairing provenance", entity_name)
        file_paths = _surviving_chunk_file_paths(limited_chunk_ids, chunk_data_by_id or {}, global_config)
        await _update_entity_storage(current_entity.get("description", ""), current_entity.get("entity_type", "UNKNOWN"), file_paths, limited_chunk_ids)
        return True
```

四处：

1. **`structural_fallback or not summarize_from_edges` → `edges = []`** —— 所以这两种情况直接走「保留」而不查询边。

   - `structural_fallback`（`rollback` 策略）为真：不查图。
   - `summarize_from_edges` 为假：调用方判断「这个实体不该从边汇总」（见下）。

2. **`summarize_from_edges` 的来源**是 `_named_as_endpoint(entity_name, chunk_ids, chunk_relationships)`：

```python
def _named_as_endpoint(entity_name, chunk_ids, chunk_relationships) -> bool:
    """True when a surviving chunk's relation records name the entity.

    Without such a record nothing attests the entity under this name (a rename, a
    merge, an older normalization, custom KG content, or name drift after
    re-extraction), and summarizing its stored edges would only replace its
    description with theirs.
    """
    return any(entity_name in edge_key
               for chunk_id in chunk_ids
               for edge_key in chunk_relationships.get(chunk_id, ()))
```

   **判据是「幸存块的关系记录里有没有提到这个名字」**。没有 → 不从边汇总，因为「summarizing its stored edges would only replace its description with theirs」（把实体描述替换成它的边的描述，等于降级）。

   **这处判断很精妙**：一个实体若在幸存记录里连「作为关系端点」都没出现，那它大概是改名/合并的产物，此时用边描述覆盖它自己的描述是纯损失。

3. **「Kept, not removed」的理由是本页最重要的那句注释**：

> no surviving chunk's records name it, which also happens to a renamed or merged entity, so **absence of evidence is not evidence that only deleted chunks supported it**

   **即：「没有幸存证据」这条判据无法区分两种情况**：
   - 它只被删除的块支撑 → **该删**
   - 它因为改名/合并而失去证据 → **不该删**

   所以默认保留（只修 `source_id`/`file_path` 到幸存块），并在 `report.degraded_entities` 里记下，让运维决定。

4. **`return True`** —— 标记为 degraded。

**第二个分支：有边但无实体记录 → 从边汇总。**

```python
logger.warning(f"No entity data found for `{entity_name}`, trying to rebuild from relationships")

# Only the description comes from the edges. Provenance follows the
# surviving chunks like source_id does: an edge's file_path may name
# files outside this entity's chunks and carries its own truncation
# marker, which must not be counted or kept as a path.
relationship_descriptions = []
for src_id, tgt_id in edges:
    edge_data = await knowledge_graph_inst.get_edge(src_id, tgt_id)
    if edge_data and edge_data.get("description"):
        relationship_descriptions.append(edge_data["description"])
file_paths = _surviving_chunk_file_paths(limited_chunk_ids, chunk_data_by_id or {}, global_config)

description_list = list(dict.fromkeys(relationship_descriptions))
if description_list:
    final_description, _ = await _handle_entity_relation_summary("Entity", entity_name, description_list, GRAPH_FIELD_SEP, global_config, llm_response_cache=llm_response_cache)
else:
    final_description = current_entity.get("description", "")
```

四处：

1. **「Only the description comes from the edges」** —— **`file_path` 不从边取**，理由很具体：「an edge's file_path may name files outside this entity's chunks and carries its own truncation marker, which must not be counted or kept as a path」。

   **即：边的 `file_path` 是边自己的来源，不是实体的。** 而且它可能带截断占位符（`"...N more...(KEEP Old)"`），混进路径列表会变成假路径。

   **这是「借来的数据要分清归属」的具体实例** —— 描述可以从边借（语义相关），但 provenance 不能（归属不同）。

2. **`file_paths = _surviving_chunk_file_paths(limited_chunk_ids, ...)`** —— 从幸存块反查文件路径。所以 provenance 始终跟着「幸存块」走。

3. **`dict.fromkeys` 去重保序** —— 与 d03/d04 同一手法。

4. **`_handle_entity_relation_summary` 被复用（d03）** —— 所以「从边汇总」与「从碎片汇总」走同一个摘要入口。

   **注意它没传 `truncation_tally`** —— 所以这个路径上的截断不上报。**这是一处缺口**（重建造成的截断对运维不可见）。

**第三个分支：有记录 → 正常重建。** 按实体类型投票、描述去重、摘要、文件名限长 —— 与 d02 的合并逻辑同构但更简单（没有 `already_*` 的部分，因为这里是「从零重建」）。

**注意它的 `entity_type` 投票只看缓存记录，不看已存值**：

```python
if entity_types:
    type_counts = Counter(entity_types)
    entity_type = sorted(type_counts.items(), key=lambda x: x[1], reverse=True)[0][0]
else:
    entity_type = current_entity.get("entity_type", "UNKNOWN")
```

**与 d02 的差别**：d02 把已存类型算一票且平票胜（防止一次误抽翻转）；这里**完全按缓存记录投票**（因为重建的语义就是「按证据重算」）。

**而 `Counter` 的 `sorted` 稳定** —— 所以同票数时按**首次出现顺序**，而首次出现顺序取决于 `limited_chunk_ids` 的顺序。

**这里有一个潜伏的风险**：`limited_chunk_ids` 来自 `apply_source_ids_limit`，而它按 `limit_method` 可能保留头或尾。所以**同票时的类型选择依赖截断策略**。而 d02 靠「已存类型排最前」消除了这个依赖。

**`_rebuild_single_entity` 的三条路径返回值的语义**

| 路径 | 返回 | 含义 |
|---|---|---|
| 节点不存在 | `False` | 不算降级（本就不在） |
| 无证据、无边 | `True` | degraded：保留语义字段 |
| 无证据、有边 | `False` | 从边重算，不算降级 |
| 有记录 | `False` | 正常重建 |

**所以「degraded」的定义是「没能重算，只能保留原文」** —— 而不是「结果变差了」。

### `_rebuild_single_relationship`：权重的地板与「不保留 boost」

**degraded 时用已存值作为唯一来源。**

```python
descriptions = [current_relationship.get("description", "")] if degraded else []
raw_keywords = [current_relationship.get("keywords", "")] if degraded else []
weights = [current_relationship.get("weight", 1.0)] if degraded else list(weight_by_chunk.values())
file_paths_list = (_surviving_chunk_file_paths(limited_chunk_ids, chunk_data_by_id or {}, global_config) if degraded else [])
```

**四处都按 `degraded` 二选一** —— 无证据时「以已存值为基础」，有证据时「从缓存重算」。

**权重的地板检查带了一整段论证，而它回答了一个关键问题：为什么不用 `max` 保留旧的 boost。**

```python
weight = sum(weights) if weights else current_relationship.get("weight", 1.0)
weight = max(float(weight), float(len(set(limited_chunk_ids))))
```

注释：

> A rebuilt relation must not fall below its evidence floor: `limited_chunk_ids` becomes the edge's `source_id`, while cached fragments only cover those surviving chunks whose extraction cache is still present (none at all on the degraded path). Their summed weight can therefore under-count the evidence and mint exactly the rows relation edits later refuse. Count the post-limit list, matching `_merge_edges_then_upsert` and the `source_id` actually stored on the edge.
>
> This is a floor, not boost preservation: a rebuild that found fragments re-derives weight from them, exactly as it re-derives description and keywords, so weight follows evidence DOWN as a purge removes chunks and an edit_relation boost is not carried across. Folding the stored scalar into the max would freeze weight at its pre-purge value forever, since the scalar cannot be decomposed into evidence and boost. The degraded path has nothing to re-derive from, so it seeds `weights` with the stored value above and the boost survives there.

五处：

1. **为什么需要地板**：`limited_chunk_ids` 会成为边的 `source_id`，而缓存碎片可能只覆盖其中一部分。所以「碎片权重之和」可能**小于证据数** → 造出 d03 的 `validate_relation_weight` 会拒绝的行。

   **即：这个地板是为了让重建的结果能被后续编辑操作接受。**

2. **地板用 `len(set(limited_chunk_ids))`** —— 与 `_merge_edges_then_upsert`（d02）和实际存储的 `source_id` 一致。

3. **「This is a floor, not boost preservation」** —— 这是最关键的一句。**重建会丢掉手工 boost**，因为：

> the scalar cannot be decomposed into evidence and boost

   **即：`weight` 是一个标量，无法拆成「证据数」与「boost」两部分。** 若用 `max(新算的, 已存的)`，就把 boost 永久冻结在「清理前的值」——因为下次重建还会读那个旧标量。

4. **「weight follows evidence DOWN as a purge removes chunks」** —— 这是**有意的**：删掉文档后，那条关系的证据确实变少了，权重理应下降。若保留旧值，权重就与实际证据脱节。

5. **degraded 路径反过来**：因为没有可重算的东西，所以 `weights` 直接以已存值为种子 → **boost 在那条路径上存活**。

**这是一处「同一个字段在两条路径上有不同语义」的设计**：有证据时「按证据重算」（boost 丢），无证据时「保留原值」（boost 留）。而判断标准同样是「有没有可重算的证据」。

### 上报：`KGRebuildReport` 与两处有界日志

```python
report.referenced_chunk_ids.update(all_referenced_chunk_ids)
report.missing_cache_chunk_ids.add(chunk_id)
report.unusable_cache_chunk_ids.add(chunk_id)
report.reextracted_chunk_ids.update(reextracted)
report.degraded_entities[entity_name] = list(chunk_ids)
report.degraded_relationships[(src, tgt)] = list(chunk_ids)
```

**六类信息**，而它们的用途不同：

| 字段 | 含义 | 后续动作 |
|---|---|---|
| `referenced_chunk_ids` | 涉及的块 | 统计总量 |
| `missing_cache_chunk_ids` | 缓存不可用 | 运维决定是否重处理 |
| `unusable_cache_chunk_ids` | 缓存坏了（rollback 策略） | 同上 |
| `reextracted_chunk_ids` | 重抽过 | 同上 |
| `degraded_entities` | 保留未重建的实体 | **需要人看** |
| `degraded_relationships` | 同上（关系） | 同上 |

而日志处的处理体现「有界」：

```python
kept = [f"`{name}`" for name in report.degraded_entities] + [f"`{src}`~`{tgt}`" for src, tgt in report.degraded_relationships]
sample = ", ".join(kept[:_REBUILD_SUMMARY_SAMPLE])
if len(kept) > _REBUILD_SUMMARY_SAMPLE:
    sample += f" (+{len(kept) - _REBUILD_SUMMARY_SAMPLE} more)"
```

**`_REBUILD_SUMMARY_SAMPLE = 10`**，注释说明：「the pipeline history is a bounded ring」。

**所以所有「可能很长」的列表在日志里都截到 10 + 「+N more」。** 这与 d01 里 `TokenLimitTruncationTally` 的「首次 + 汇总」是同一族做法——**保护有界的日志缓冲**。

**而「重抽」的日志也用了同样的截断**（`doc_ids` 的样本）。

## 关键取舍

**源码事实：** 两种策略：`best_effort`（文档删除/恢复清理，**可调 LLM 重抽**）与 `rollback`（自定义 chunk 回滚，**绝不调 LLM**，坏数据只记录）。

**源码事实：** `ainsert_custom_kg` 写的块**永不重抽**，理由是「never sent to the LLM on someone else's deletion」——用户提供的文本不该因为别人的删除被送去 LLM。识别靠 `_find_custom_kg_chunks`：无 extract key 且（行有 `source_id` **或** `full_doc_id` 指向不存在的 `doc_status` 行）。

**源码事实：** 缓存行归位**靠块自己的 `llm_cache_list`，绝不用行的 `chunk_id`**——因为缓存按内容哈希去重，`chunk_id` 只记第一个写入者，不同文档里相同的块共享一行。

**源码事实：** `llm_cache_list` 的顺序**就是调用顺序**，所以 `entries[0]` 是初始抽取、`entries[1:]` 是 gleaning；这是「初始在第一位」这个假设的唯一根据。旧格式 key 解不出类型时改为看行本身。

**源码事实：** `best_effort` 的三态：块文本不存在 → `missing_cache_chunk_ids`；初始行缺失**或第一行其实是 gleaning 行** → 重抽；初始行不可解析 → 记下它的 key 并重抽（key 用于随后的删除）。gleaning 行缺失或不可解析**只 warning 并忽略**（「accepted as gleaning that did not run」，只降召回不改证据）。

**源码事实：** `_reextract_chunks` 三步准备：删掉不可解析的初始缓存行（否则重抽会再次命中它）；从 `llm_cache_list` 里摘掉**所有** extract key；把更新写回存储与内存副本。这样新 run 附加的 key 按调用顺序让新初始抽取**重新成为第一位**——不变量被重建而不是被标记。

**源码事实：** 重抽**复用 `extract_entities`**（d01），所以校验/gleaning/多模态注入逻辑与正常抽取完全一致；`dict(zip(chunk_ids, results))` 依赖 d01 那个「结果必须按 `ordered_chunks` 顺序」的修复。

**源码事实：** 重抽的前置日志给出运维建议「Reprocess these documents if the knowledge graph lost facts」——因为 LLM 重抽不保证与上次输出一致。

**源码事实：** 关系先重建、实体后重建，且**关系阶段失败则实体完全不动**（`wait_tasks_with_drain` 取消并排空兄弟后抛出）。理由是「有些实体只通过关系被知道，其描述要从边汇总，所以边必须先重建完」。

**源码事实：** `_rebuild_single_entity` 的四条路径与返回值：节点不存在 → `False`；无证据且无边 → 保留语义字段、修 provenance、返回 `True`（degraded）；无证据但有边 → 从边汇总、返回 `False`；有记录 → 正常重建、返回 `False`。**所以「degraded」= 「没能重算，只能保留原文」**。

**源码事实：** 「无证据」时的默认行为是**保留而不是删除**，注释给出核心理由：「absence of evidence is not evidence that only deleted chunks supported it」——改名或合并也会造成无证据。这条判据无法区分两者，所以选择安全方向并记入 `report.degraded_entities`。

**源码事实：** 是否「从边汇总」由 `_named_as_endpoint` 决定：幸存块的关系记录里必须提到这个名字。否则「summarizing its stored edges would only replace its description with theirs」——纯损失。

**源码事实：** 从边汇总时**只借描述，不借 `file_path`**——「an edge's `file_path` may name files outside this entity's chunks and carries its own truncation marker」。provenance 始终从幸存块反查。

**源码事实：** 重建路径上的实体类型投票**只看缓存记录**（不像 d02 把已存类型算一票），所以同票时按首次出现顺序，而该顺序依赖 `apply_source_ids_limit` 的截断策略——一处潜伏的不确定性。

**源码事实：** `created_at` 在重建时被刷新成现在，所以该字段在重建后表达「最后写入时间」而非创建时间。

**源码事实：** 关系权重的 `max(weight, len(set(limited_chunk_ids)))` 是**地板不是 boost 保留**：重建会丢掉 `edit_relation` 的 boost，因为 `weight` 是标量、无法拆成「证据 + boost」，用 `max` 会把 boost 永久冻结；degraded 路径反过来以已存值为种子，boost 在那里存活。地板的必要性是让结果能被 d03 的 `validate_relation_weight` 接受。

**源码事实：** 重建路径裁剪文件名时**不带 `truncate` 标记**（`_update_entity_storage` 的 `truncation_info` 只在 source_id 被截断时设），而 d02 的合并会写 `truncate`；从边汇总时调 `_handle_entity_relation_summary` **没传 `truncation_tally`**，所以这条路径的截断对运维不可见。

**源码事实：** 所有可能很长的列表在日志里都截到 `_REBUILD_SUMMARY_SAMPLE = 10` + 「+N more」，因为「the pipeline history is a bounded ring」。

**架构推论：** 这一层的核心是「**用证据重建，而证据可能不在**」，所以它的第一原则是「**证据缺失 ≠ 对象该删**」。判据无法区分「只被删除的块支撑」与「因改名/合并而失去证据」，而在不确定时保留是唯一安全方向——因为删除不可恢复，而保留可以靠运维事后处理。这与 d02 的 Phase 0「先写可发现性锚点」是同一思路的两端：一个保证「可能碰过什么」可查，一个保证「碰过的不能误删」。

**架构推论：** `_reextract_chunks` 那三步「删旧行 + 清 key + 重建列表」很值得记。它面对的是「一个位置敏感的不变量被破坏后怎么修」——不变量是「`llm_cache_list[0]` 是初始抽取」，而修复不是「标记谁是初始」，而是**清空并让新流程按顺序重填**。**这比加标记更可靠**：标记需要每处读写都遵守，而重建顺序只需要一个入口正确。

**架构推论：** 权重那段「地板不是 boost 保留」的推理揭示了一个通用问题：**标量字段一旦混合了两个语义，就无法只更新其中一个。** `weight = evidence + boost` 这个设计在正常合并时没问题（两者都增加），但在「证据减少」的场景（清理）就无法表达「evidence 降、boost 不变」。所以只能二选一：要么丢 boost（现在这样），要么冻结整个值（导致权重与证据脱节）。要真正解决得把 `weight` 拆成两个字段——但那就破坏了 d03 的契约。

**架构推论：** 「关系先于实体」这个顺序说明**重建的依赖图是有向的**：实体可能依赖边（`summarize_from_edges`），而边不依赖实体（边的描述来自缓存碎片）。所以正确顺序是先叶后根。而「前一阶段全成功才做后一阶段」比「尽量做完」更合适，因为**半成品状态比不做更糟**（实体读到了没重建完的边，会写入错误的描述）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/operate.py` | `rebuild_knowledge_from_chunks` | 重建入口：策略分派、缓存归位、两阶段编排 |
| `lightrag/operate.py` | `rebuild_knowledge_from_chunks._publish` | 同时写服务端日志与管道状态 |
| `lightrag/operate.py` | `rebuild_knowledge_from_chunks._finish_cached_chunk` | 缓存记录也要过 validator 与多模态注入 |
| `lightrag/operate.py` | `rebuild_knowledge_from_chunks._locked_rebuild_entity` | 信号量 + 实体名键控锁 |
| `lightrag/operate.py` | `rebuild_knowledge_from_chunks._locked_rebuild_relationship` | 同上，锁键排序 |
| `lightrag/operate.py` | `_load_chunk_extract_rows` | 按 `llm_cache_list` 归位（绝不用行的 `chunk_id`） |
| `lightrag/operate.py` | `_get_cached_extraction_results` | 按 `create_time` 排序的简化视图（追踪修复工具用） |
| `lightrag/operate.py` | `_reextract_chunks` | 三步准备 + 复用 `extract_entities` |
| `lightrag/operate.py` | `_rebuild_from_extraction_result` | 缓存结果解析：JSON 优先、失败降级到分隔符解析 |
| `lightrag/operate.py` | `_parse_extract_row` | 单行解析，不可用则抛 |
| `lightrag/operate.py` | `_merge_chunk_records` | 把一行的 records 折进块的累积表 |
| `lightrag/operate.py` | `_find_custom_kg_chunks` | 识别 `ainsert_custom_kg` 的块（永不重抽） |
| `lightrag/operate.py` | `_named_as_endpoint` | 判断实体是否可「从边汇总」 |
| `lightrag/operate.py` | `_rebuild_single_entity` | 实体重建四分支 |
| `lightrag/operate.py` | `_rebuild_single_entity._update_entity_storage` | 先校验 VDB 载荷再写图、再写 VDB |
| `lightrag/operate.py` | `_rebuild_single_relationship` | 关系重建 + 权重地板（丢掉 boost） |
| `lightrag/operate.py` | `_REBUILD_SUMMARY_SAMPLE` | 日志列表截断（有界环形缓冲） |
| `lightrag/operate.py` | `_is_gleaning_extract_row` | 靠 prompt 尾行识别 gleaning 行 |
| `lightrag/utils_graph.py` | `_surviving_chunk_file_paths` | 从幸存块反查文件路径 |

## 参考

- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)
- [lightrag 设计文档：清理恢复契约](https://github.com/HKUDS/LightRAG/blob/main/docs/design/PurgeRecoveryContract.md)
- [论文：基于图的 RAG 与增量更新](https://arxiv.org/abs/2410.05779)
- [Python 文档：asyncio.Task 分组与取消](https://docs.python.org/3/library/asyncio-task.html#task-groups)

## 与主核 / AI 的关系

「用幸存证据重建」与主核（多模态数据库内核）的 **`VACUUM` 与死元组回收**面对的问题形态相同：删掉一些行之后，「由它们派生出来的东西」（索引项、统计量、物化视图）需要更新。但主核的解法完全不同——它用 `xmin`/`xmax` 标记死元组，索引项按可见性判断是否有效，**不需要重建**。而这里没有多版本，合并后的实体是一份不可分解的记录，所以只能重算。

**这个差别的根源是「是否保留出处」**：主核的每个元组都知道自己是哪个事务写的；而合并后的实体只保留一个 `source_id` 列表（且被截断），无法知道「哪句话来自哪个块」。所以主核能做增量维护，这里只能全量重建。**判据是「派生数据能否追溯到输入」**——能则增量，不能则重算。

第二处对照是「证据缺失 ≠ 该删」与主核的**外键 `ON DELETE` 语义**。主核的 `ON DELETE RESTRICT` 在「有引用时拒绝删除」，`CASCADE` 则连引用一起删。两者都是**确定的**：要么拒绝要么级联。而这里面对的是「不确定」——「没有证据」既可能是「确实没别的支撑」也可能是「归属丢了」。所以它选了一个数据库没有的第三态：**保留但标记为 degraded**（`report.degraded_entities`），把判断交给运维。**这是「无法自动决定时，保留 + 可观测」的模式**，在数据不可恢复的场景里比二选一更安全。

第三处是「权重地板不是 boost 保留」与主核的**统计信息重建**。`ANALYZE` 重算统计信息时也是「完全按当前数据重算」，不保留历史估计——因为历史估计无法与当前数据混合（混合结果没有意义）。同理，`weight` 重算时无法保留 boost（因为它无法从标量里分离出来）。

对 AI 工程的实际意义在于：图谱删除是 graph-RAG 最难做对的操作，因为图谱是「有损合并」的产物。这份实现给出的三条经验可直接用：**证据缺失不等于对象该删**（改名/合并也会造成无证据，保留并上报）、**缓存归属要靠每条的索引而不是缓存行自己的字段**（缓存去重会让后者失真）、以及**重建会丢手工加成**（混合语义的标量字段在「减少」场景下无法只更新一部分）。
