---
project: lightrag
feature: node-edge-merge
title: 节点与边的合并去重
page_type: feature
schema_version: 1
refs:
  - path: lightrag/operate.py
    symbols: [_merge_nodes_then_upsert, _merge_edges_then_upsert, merge_nodes_and_edges]
  - path: lightrag/operate.py
    symbols: [_combine_descriptions_dedup, collect_kg_merge_candidates]
---

# 节点与边的合并去重

## 一句话职责

同一个实体被十个文档提到时，图谱里必须只有一个节点——所以抽取出的每条碎片都要与已存节点**合并**：描述去重后拼接、`source_id` 与 `file_path` 求并集并截断、`entity_type` 按票数决定、关系权重按「不同真实来源数」累加。而整个合并过程在没有事务的前提下靠**先写恢复锚点再改图**保证崩溃可发现。

## 机制是什么

这一层要处理的核心矛盾是：**抽取是「增量、多次、并发」的，而图谱是「全局唯一」的**。

于是每个实体/关系都面对同一个问题：已存的那份与新来的这批怎么合成一份？而「合成」有四个字段各自不同：

| 字段 | 合并语义 | 关键难点 |
|---|---|---|
| `description` | 去重后拼接（存旧 + 新不重复的） | **重复处理会让描述无限增长** |
| `source_id` | 求并集后按上限截断 | 截断后哪些新碎片还有效 |
| `entity_type` | 按票数选（已存类型算一票且平票胜） | 一次误抽不能翻转类型 |
| `weight`（边） | 累加**未计入过的**来源权重 | **重复处理会让权重 1→2→3 增长** |

这四个字段里的两个（`description` 与 `weight`）都有一个共同的病：**幂等性**。同一份文档重跑一次（reprocess/resume），若不特殊处理，描述会被重复追加一遍、权重会多加一次。整个合并逻辑的一大半复杂度都在对抗这件事。

第二个矛盾是**没有事务**。合并会写四个地方：图存储（节点/边）、向量存储（实体/关系嵌入）、chunk 追踪存储、文档级候选清单。任一处失败都留下不一致。所以顺序被排成「先写恢复锚点 → 再改图」，让崩溃后的恢复能找到「这个文档可能碰过哪些对象」。

物理上相关的东西不会被同时写」这个约束下，能做的是**让崩溃后的残留在已知位置**（a01 讲过存储契约的同一前提）。

## 怎么实现的

### 编排：`merge_nodes_and_edges` 的三阶段

**阶段顺序是「可发现性先于变更」。**

```python
"""Merge extracted entities/relations into the KG behind write-ahead anchors.

Phase order (discoverability before mutation):
0. Phase 0: Persist the full candidate superset to ``full_entities`` /
   ``full_relations`` and flush both BEFORE any graph mutation, so a
   crash mid-merge always leaves a durable recovery anchor that purge /
   retry can discover the contributions from. Both rows are written even
   when empty; a failure here aborts the merge before mutation.
1. Phase 1: Process all entities concurrently
2. Phase 2: Process all relationships concurrently (may add missing entities)
"""
```

四处：

1. **Phase 0 写的是「候选超集」而不是「确定存在的对象」** —— 因为此时还没写图，无法知道哪些真的会存在。`collect_kg_merge_candidates` 的文档强调：「The result is a candidate SUPERSET, not proof of existence: purge must verify source ownership per candidate and treat absent objects as no-ops」。

2. **「Both rows are written even when empty」** —— 因为一次重处理可能产出**更少**（甚至零）候选，必须覆盖上一次的旧锚点，否则清理时会按旧清单去删已经不存在的对象。

3. **flush 是显式的屏障**：

```python
for storage_inst in (full_entities_storage, full_relations_storage):
    try:
        await storage_inst.index_done_callback()
    except Exception as e:
        namespace = getattr(storage_inst, "final_namespace", None) or getattr(storage_inst, "namespace", "")
        raise IndexFlushError(type(storage_inst).__name__, namespace, e) from e
```

   注释：「Narrow flush barrier: both anchors must be durable before the first graph mutation. A failure propagates — merging without a recovery anchor would make a later partial failure undiscoverable」。

   **注意 `upsert` 之后还要 `index_done_callback`** —— 因为 a01 讲过存储可能延迟写入。所以「写进去了」与「落盘了」是两件事，而这里的屏障要求后者。

4. **`on_anchors_durable` 钩子在最精确的时机被调用**：

```python
# Anchors are durable: this is the last instant at which "no graph
# mutation has happened" is still true. A raise here aborts the merge
# with the anchors already in place — the safe direction.
if on_anchors_durable is not None:
    await on_anchors_durable()
```

   文档解释了它的用途：「Lets the caller persist "this document may have touched the graph from here on" (`doc_status.metadata.kg_write_state`) in the one window where that statement flips」。

   **这是一个「状态翻转点上的钩子」** —— 在此之前可以断言「没碰过图」，此后不能。所以只有这一个位置能正确地记录这个翻转。

   而「A raise here aborts the merge with the anchors already in place — the safe direction」——钩子失败时的结果（锚点在、图未改）是可以恢复的。

**Phase 1 与 Phase 2 之间有一个关键事实：关系处理会创建缺失的端点实体。**

```python
2. Phase 2: Process all relationships concurrently (may add missing entities)
```

所以 `_merge_edges_then_upsert` 有一个 `added_entities` 列表参数，用来记录「我在处理边时顺手创建的实体」。这解释了为什么 `collect_kg_merge_candidates` 要把「每条关系的两个端点」也算进候选实体：

```python
for edge_key in maybe_edges:
    pair = tuple(sorted((edge_key[0], edge_key[1])))
    candidate_relations.add(pair)
    candidate_entities.update(pair)
```

**即：候选实体集 = 抽出的实体 ∪ 所有关系端点。** 因为关系端点可能从未作为实体被抽出（模型只说了「A 影响 B」而没说 A 是什么），此时 Phase 2 会为它创建一个 `UNKNOWN` 类型的节点。

**并发用信号量 + 按名键控锁（keyed lock）。**

```python
graph_max_async = global_config.get("llm_model_max_async", 4) * 2
semaphore = asyncio.Semaphore(graph_max_async)

async with get_storage_keyed_lock([entity_name], namespace=namespace, enable_logging=False):
```

四处：

1. **`* 2`** —— 图合并的并发上限是 LLM 并发上限的两倍。因为合并的主要开销不是 LLM（摘要可能不走 LLM），而是存储 I/O。

2. **`get_storage_keyed_lock([entity_name], ...)`** —— 按**实体名**键控的锁。所以「同一实体的两个合并任务」互斥，「不同实体」并行。

   **这是必要的**：同一个实体可能在不同的 chunk 结果里出现（虽然 `all_nodes` 已经按名字聚合了，所以实际上同一批里一个实体只有一个任务）。但跨文档/跨批次时，两个并发的合并操作可能同时处理同一实体——而 a01 讲过存储层没有事务，所以必须靠这把锁。

3. **边的锁用排序后的键**：

```python
sorted_edge_key = sorted([edge_key[0], edge_key[1]])
async with get_storage_keyed_lock(sorted_edge_key, namespace=namespace, enable_logging=False):
```

   **排序是关键** —— 因为图是无向的（`all_edges` 的键也是 `tuple(sorted(edge_key))`），所以 `(A,B)` 与 `(B,A)` 是同一把锁。

4. **`namespace` 带工作区前缀**（`f"{workspace}:GraphDB"`）—— 所以不同工作区的同名实体互不阻塞。

**任务失败的传播用 `wait_tasks_with_drain`，而它在取消路径上还需要第二道保险。**

```python
processed_entities = await wait_tasks_with_drain(entity_tasks, context=f"entity merge {doc_id}")
```

```python
except (Exception, asyncio.CancelledError):
    # wait_tasks_with_drain only covers cancellation while it is waiting.
    # The creation loops yield (_cooperative_yield) before it starts, so a
    # cancellation landing there would leave the writers created so far
    # merging into the graph after this coroutine has exited and recovery
    # has begun.
    await cancel_and_drain_tasks(entity_tasks + edge_tasks)
    raise
```

**这是一处很实际的窗口**：任务创建循环里有 `await _cooperative_yield(i, every=16)`（或 `every=32`），所以「创建了一半任务」与「开始等待」之间是一个 await 点。取消落在这里时 `wait_tasks_with_drain` 还没被调用，于是已创建的任务没人管——它们会继续往图里写。

**所以「取消处理」必须覆盖「等待之前的那个窗口」，而不只是「等待本身」。**

**阶段汇总同样放 `finally`，且注释记录了一处更细的漏洞。**

```python
def _publish_summary_truncation() -> None:
    """... 
    Wrapping only the ``wait_tasks_with_drain`` calls was not enough: a
    cancellation at an inter-phase yield skipped the absorb, and the
    custom-chunk failure bookkeeping then recomputed the journal from the
    unabsorbed operation tally — overwriting the summary event its write-ahead
    hook had already persisted. Idempotent, so redundant calls cannot
    double-publish, and await-free, so it is safe to run inside a
    cancellation window.
    """
    nonlocal summary_truncation_published
    if summary_truncation_published:
        return
    summary_truncation_published = True
```

四处：

1. **比 d01 更细的问题**：那里是「成功与失败两条路都漏了取消」，这里连「包住两个 `wait_tasks_with_drain`」都不够——因为**阶段之间**还有 await 点（`asyncio.sleep(0)` 与状态锁获取）。取消落在那里就跳过了 absorb。

2. **后果更严重**：调用方的失败记账会**从未 absorb 的计数重算 journal**，从而**覆盖**写前钩子已经持久化的那个事件。

3. **`summary_truncation_published` 标志** —— 所以函数**幂等**（`finally` 里调用 + 可能已被别处调用）。

4. **`nonlocal` 而非全局** —— 状态在闭包里。

**而 `summary_tally` 与 `truncation_tally` 是两级。**

```python
summary_tally = TokenLimitTruncationTally()        # 本次合并
...
truncation_tally.absorb(summary_tally)             # 汇总进文档级
```

文档说明：「one aggregated status line for the whole merge rather than one per truncated summary, folded into the document-scoped tally so the `doc_status` record covers extraction and merge together」。

**所以「合并」与「抽取」的截断计数是分开累积、最后合并的。** 这样每个阶段的日志汇总各自准确。

**`truncation_write_ahead` 钩子的存在理由只对一类调用方成立。**

```python
truncation_write_ahead: Optional async hook awaited right after a
    summary truncation is recorded, before the object carrying that
    summary is upserted. For callers whose failure recovery is
    ADDITIVE (the custom-chunk path: a resume keeps the failed
    attempt's graph mutations), the record must be durable before the
    mutation it warns about — a FAILED-transition stamp alone loses
    the event on a hard crash. The pipeline's own merges do not need
    it: a reprocess purges the previous attempt's contributions, so
    nothing truncated survives to warn about.
```

**这是一个很精确的区分**：两种失败恢复语义（`ADDITIVE` vs `PURGE`）对「何时持久化告警」有不同要求。

- 自定义 chunk 路径：恢复是**累积**的（上一次的图变更保留），所以「被截断的摘要」会留在图里 → 告警必须在**变更之前**持久化。
- 管道自己的路径：重处理会**清理**上次的贡献，所以被截断的摘要不会存活 → 不需要写前持久化。

**判据是「这次变更的结果会不会在失败后存活」。**

### 实体合并：`_merge_nodes_then_upsert` 的十一步

**第一步读已存节点，而读回来的 `entity_type` 要再清洗一遍。**

```python
existing_entity_type = already_node.get("entity_type")
# Coerce to str before any string operations: non-string values from
# API/custom graph paths would otherwise raise TypeError on the comma check.
if not isinstance(existing_entity_type, str) or not existing_entity_type.strip():
    existing_entity_type = "UNKNOWN"
if "," in existing_entity_type:
    original = existing_entity_type
    tokens = [t.strip() for t in existing_entity_type.split(",")]
    non_empty = [t for t in tokens if t]
    existing_entity_type = non_empty[0] if non_empty else "UNKNOWN"
    logger.warning(...)
```

四处：

1. **`isinstance` 检查 + 非空检查** —— 注释说明了必要性：「non-string values from API/custom graph paths would otherwise raise TypeError」。因为 `AIM` 类接口（`ainsert_custom_kg`）允许用户直接写图，类型可能不是字符串。

2. **非字符串或空 → `"UNKNOWN"`** —— 而不是丢弃。因为节点存在，只是类型坏了。

3. **逗号处理与 d01 的 `_normalize_and_validate_entity_type` 逻辑相同** —— 但这里**没有复用它**，而是重写了一遍。两处一致的规则实现两次，是一处维护风险。

   **为什么没复用？** 大概是时序/依赖原因：d01 那个函数先做 sanitize（`sanitize_and_normalize_extracted_text`），而这里读的是已存值（已 sanitize 过），只需要「取逗号前第一段」那段。所以复用的前提不成立。但「禁用字符 + 保留名」这些检查这里**完全没做**——一个坏数据里的 `__proto__` 会原样留下。

   **这是一处真实的缺口**：类型校验只在抽取入口做，读回来的路径（以及 `ainsert_custom_kg` 写进来的）不过同一套检查。

4. **`already_source_ids.extend(existing_source_id.split(GRAPH_FIELD_SEP))`** —— 存的是分隔符拼接的字符串，读时拆开。所以「列表」在存储层被压成一个字符串。

   **这是「一个字段存多个值」的做法，代价是：** 值里不能含分隔符；拆分后可能产生空串（`"a||b"` → `["a","","b"]`）；无法表达空列表（空列表存成 `""`，拆开是 `[""]`）。

**`source_id` 的三层：已存 / 追踪行 / 合并后的全量。**

```python
existing_full_source_ids = []
has_tracking_row = False
if entity_chunks_storage is not None:
    stored_chunks = await entity_chunks_storage.get_by_id(entity_name)
    # Row presence by schema, never list truthiness (see
    # has_chunk_tracking_row): a present-but-empty row is authoritative
    # and must NOT be reseeded from the graph node's source_id, which is
    # a truncated view that may still name purged chunks.
    has_tracking_row = has_chunk_tracking_row(stored_chunks)
    if has_tracking_row:
        existing_full_source_ids = [c for c in stored_chunks.get("chunk_ids", []) if c]

if not has_tracking_row:
    existing_full_source_ids = [c for c in already_source_ids if c]
```

四处：

1. **两个来源，优先级明确**：追踪存储（`entity_chunks_storage`，完整）优先于图节点的 `source_id`（截断视图）。

   **注释的关键论断**：「a present-but-empty row is authoritative and must NOT be reseeded from the graph node's source_id, which is a truncated view that may still name purged chunks」。

   即：**一个「存在但为空」的追踪行是权威的**——它表示「这个实体当前没有任何 chunk」（可能全被 purge 了）。若此时用图节点的 `source_id` 回填，就会把已删除的 chunk 重新引入。

2. **`has_chunk_tracking_row` 按 schema 判存在性，而不是判列表真值** —— 所以「行存在但 `chunk_ids` 为空」与「行不存在」是两件事。

   这与 a01 里「严格读 vs 宽松读」是同一类区分：**「没有」与「不知道」必须分开**。

3. **图节点的 `source_id` 是截断视图** —— 因为第 3 步会按上限截断它。所以它不能当「完整列表」用。

4. **`if c` 过滤空串** —— 因为 `split` 会产生空串（见上）。

**合并 `source_id` 时先写完整列表，再截断给图节点。**

```python
full_source_ids = merge_source_ids(existing_full_source_ids, new_source_ids)

if entity_chunks_storage is not None and full_source_ids:
    await entity_chunks_storage.upsert({
        entity_name: {"chunk_ids": full_source_ids, "count": len(full_source_ids)}
    })

source_ids = apply_source_ids_limit(full_source_ids, max_source_limit, limit_method, identifier=f"`{entity_name}`")
```

**顺序是对的**：追踪存储拿**完整**列表，图节点拿**截断**后的。所以「谁该有完整信息」被明确分配给追踪存储。

注意 `if full_source_ids` 的守卫 —— **空列表不写追踪行**。所以「实体存在但没有任何 chunk」这个状态**无法通过 upsert 表达**（只能靠行不存在）。这与前面「存在但为空是权威的」形成一处张力：代码承认这种状态合法，但不主动创建它。

**`KEEP` 与 `FIFO` 两种截断策略影响后续过滤。**

```python
if limit_method == SOURCE_IDS_LIMIT_METHOD_KEEP:
    allowed_source_ids = set(source_ids)
    filtered_nodes = []
    for dp in nodes_data:
        source_id = dp.get("source_id")
        # Skip descriptions sourced from chunks dropped by the limitation cap
        if (source_id and source_id not in allowed_source_ids
                and source_id not in existing_full_source_ids):
            continue
        filtered_nodes.append(dp)
    nodes_data = filtered_nodes
else:  # In FIFO mode, keep all nodes - truncation happens at source_ids level only
    nodes_data = list(nodes_data)
```

四处：

1. **`KEEP` 模式（保留最早的）会丢弃「来源被截断掉」的新描述** —— 因为那些 chunk 已经不在 `source_id` 里了，保留它们的描述会造出「描述有内容但 `source_id` 没有出处」的不一致。

2. **`source_id not in existing_full_source_ids` 的例外** —— 已存来源描述不过滤。因为它们是**之前**加进来的，属于「保留最早」的那一批。

3. **`FIFO` 模式不过滤任何描述** —— 注释说明：「In FIFO mode, keep all nodes - truncation happens at source_ids level only」。所以 FIFO 会留下「描述有、`source_id` 无」的不一致。**这是一处真实的语义不对称**。

4. **`nodes_data = list(nodes_data)`** —— FIFO 分支复制一下。这个复制没有实际作用（后面不会修改它），但它保持了「两个分支都产出新列表」的对称性。

**「已存来源已满且无新碎片」时直接返回旧数据，而这是唯一不写存储的路径。**

```python
if (limit_method == SOURCE_IDS_LIMIT_METHOD_KEEP and len(existing_full_source_ids) >= max_source_limit and not nodes_data):
    if already_node:
        logger.info(f"Skipped `{entity_name}`: KEEP old chunks {len(existing_full_source_ids)}/{len(full_source_ids)} (limit {max_source_limit})")
        existing_node_data = dict(already_node)
        return existing_node_data
    else:
        logger.error(f"Internal Error: already_node missing for `{entity_name}`")
        raise ValueError(...)
```

三处：

1. **跳过的条件**：KEEP 模式 + 已满 + **过滤后没有剩余碎片**。所以「满了但还有有效新碎片」仍会走完整流程（用新碎片更新描述）。

2. **返回 `dict(already_node)` 副本** —— 不改调用方的对象。

3. **`already_node` 缺失时抛 `ValueError`** —— 这是一个「逻辑上不可能」的分支（`existing_full_source_ids` 非空说明追踪行有内容，但图节点没有），所以标成 `Internal Error`。

**`entity_type` 的投票机制，而它的规则细节很值得看。**

```python
# 6.2 Finalize entity type by highest count. The stored type carries
# a single vote however many merges established it, so it goes first
# and wins a tie (sorted() is stable): one conflicting extraction must
# not flip it. An UNKNOWN placeholder -- written for a relation
# endpoint never extracted as an entity, or for a missing stored type
# -- is not evidence, casts no vote and yields to any real extraction;
# with no other vote it is kept as is.
type_votes = [t for t in already_entity_types if t.upper() != "UNKNOWN"] + [
    dp["entity_type"] for dp in nodes_data
]
entity_type = sorted(
    Counter(type_votes or already_entity_types).items(),
    key=lambda x: x[1],
    reverse=True,
)[0][0]
```

五处：

1. **已存类型算 1 票，不管它是多少次合并的结果** —— 所以「一个已经历过 100 次合并的 `person`」只有 1 票。这防止「历史积累压倒当前证据」。

2. **`sorted()` 是稳定的，且已存类型在列表最前** —— 所以平票时已存类型胜。注释：「one conflicting extraction must not flip it」。**这保证类型不会因为一次误抽而翻转。**

3. **`UNKNOWN` 不投票** —— 因为它不是证据（是占位符）。而 `type_votes or already_entity_types` 的兜底：**若没有其他票，就用原始已存列表**（可能全是 `UNKNOWN`），于是「只有 `UNKNOWN` 时保留 `UNKNOWN`」。

4. **`x.upper() != "UNKNOWN"`** —— 大小写不敏感。所以 `"unknown"` 也被排除。

5. **`Counter(...).items()` 的顺序是插入序** —— 而 `sorted` 稳定，所以「同票数时按首次出现顺序」。结合已存类型在前，这就是「已存优先」。

**描述的排序用的是 `(timestamp, -len)`，而这也是去重的一环。**

```python
for i, dp in enumerate(nodes_data, start=1):
    desc = dp.get("description")
    if not desc:
        continue
    if desc not in unique_nodes:
        unique_nodes[desc] = dp
    await _cooperative_yield(i, every=32)

sorted_nodes = sorted(
    unique_nodes.values(),
    key=lambda x: (x.get("timestamp", 0), -len(x.get("description", ""))),
)
sorted_descriptions = [dp["description"] for dp in sorted_nodes]
```

三处：

1. **按描述文本去重，保留首次出现** —— 所以同一个块里抽出两条一样的描述只留一条。

2. **排序键是「时间升序、同时间则描述长的在前」** —— 所以拼接出的描述是「按文档处理顺序，早期文档在前；同批次里长描述优先」。

   这决定了最终摘要里各条描述的**呈现顺序**，而顺序对 LLM 摘要的影响很大（前面几条权重更高）。

3. **`await _cooperative_yield(i, every=32)` 在去重循环里** —— 每 32 项让出一次。这是「纯 CPU 循环里插让出点」的统一做法（d01 的合并循环也是）。

**`_combine_descriptions_dedup` 是整个幂等性的关键，而它的文档把三处细节都解释了。**

```python
def _combine_descriptions_dedup(already_description, new_descriptions):
    combined = []
    seen = set()

    def _add_sanitized(descriptions):
        for desc in descriptions:
            sanitized = sanitize_text_for_encoding(desc)
            if sanitized and sanitized not in seen:
                seen.add(sanitized)
                combined.append(sanitized)

    _add_sanitized(already_description)
    already_fragment = len(combined)
    _add_sanitized(new_descriptions)
    return combined, already_fragment
```

四处（对应文档的四段论证）：

1. **跨「已存 + 新」去重，而不是只在新的内部去重** —— 文档：「Deduplicating across stored *and* new (not only within the new batch) prevents a re-extracted description from appending a duplicate fragment on every reprocess or resume; it also collapses any legacy duplicate fragments already stored」。

   **这是幂等的实现方式**：重跑时新抽出的描述已在 `seen` 里，于是不加。顺带把历史存档里的重复项也压掉。

2. **必须在 `sanitize_text_for_encoding` 之后比较**。文档的理由很具体：

> Fragments are compared and stored *after* `sanitize_text_for_encoding`: stored descriptions were already sanitized before being persisted ... while a freshly re-extracted description is still raw. Comparing raw-vs-sanitized would miss the duplicate whenever sanitization changed the string (e.g. an XML-illegal control char was stripped), so "dirty" descriptions would keep accumulating across reprocess.

   **即：如果比较用原始串，那么「清洗后会变相同」的两条会被判为不同，于是每次都加一条。** 这是一个不显然但必然发生的泄漏路径——只要文本里含不可编码字符（XML 非法控制符），每次重跑都会多一条。

3. **清洗后为空的碎片被丢弃** —— 文档：「they carry no information and would only inflate the fragment count / emit `<sep><sep>` artifacts on join」。即空碎片会让拼接出现连续分隔符。

4. **两阶段遍历是刻意的**：

> The two-phase walk is deliberate: `already_fragment` is captured after the stored pass so it stays an accurate count of surviving stored fragments. Do NOT collapse both phases into a single `dict.fromkeys` over the concatenation — that loses the stored/new boundary the merge accounting log depends on.

   **`already_fragment` 用于日志的 `已存+新增` 计数**（`f"Merged: ... | {already_fragment}+{num_fragment - already_fragment}"`）。若合成一次 `dict.fromkeys`，就分不出哪些是原有的、哪些是新加的。

   而这个计数**不只是日志**——`deduplicated_num = already_fragment + len(nodes_data) - num_fragment` 用它算「丢弃了多少重复」。

**描述全空时的处理：实体给兜底串，关系抛错。**

```python
if not description_list:
    fallback_description = f"Entity {entity_name}"
    logger.warning(f"Entity `{entity_name}` has no description; fallback to `{fallback_description}`")
    description_list = [fallback_description]
```

```python
if not description_list:
    logger.error(f"Relation {src_id}~{tgt_id} has no description")
    raise ValueError(f"Relation {src_id}~{tgt_id} has no description")
```

**不对称的理由能推出来**：实体可以「存在但没描述」（它是图谱的一个点，有价值），而关系没有描述就**没有任何内容可嵌入**（关系的向量就是这个描述），所以造一个空关系没有意义。

**写图的顺序：先构造并校验 VDB 载荷，再写图，最后写 VDB。**

```python
# Construct and verify the VDB payload BEFORE the first graph
# mutation: if truncation fails (a deterministic, non-retryable
# content-shape problem), nothing has been written yet. The actual
# VDB I/O call still happens after the graph write below.
data_for_vdb = None
if entity_vdb is not None:
    entity_vdb_id = compute_mdhash_id(str(entity_name), prefix="ent-")
    entity_content = _truncate_vdb_content(f"{entity_name}\n{description}", global_config, f"entity:{entity_name}")
    data_for_vdb = {...}

await knowledge_graph_inst.upsert_node(entity_name, node_data=node_data)
node_data["entity_name"] = entity_name
if entity_vdb is not None:
    await safe_vdb_operation_with_exception(operation=lambda payload=data_for_vdb: entity_vdb.upsert(payload), ...)
```

四处：

1. **「先构造并校验载荷」的目的是让确定性失败发生在任何写入之前** —— 注释：「if truncation fails (a deterministic, non-retryable content-shape problem), nothing has been written yet」。

   **「确定性、不可重试」是关键限定**：截断失败是内容形状问题，重试也没用。所以它的检查要放在写入之前。

2. **图先写，向量后写** —— 因为向量失败可以用 `safe_vdb_operation_with_exception` 重试（`max_retries=3`），而图写入是「主体」。顺序是「先主体后派生」。

3. **`node_data["entity_name"] = entity_name` 在写图之后、返回之前** —— 因为 `node_data` 传给 `upsert_node` 时**不含** `entity_name`（它在字典的键里），但返回值需要它。所以补上。

   **注意这是就地修改 `node_data`** —— 而它刚被传给 `upsert_node`。若 `upsert_node` 保留了引用（不复制），那么它存的节点里也会有 `entity_name`。这是一个依赖存储实现细节的行为（多数后端会序列化，所以不保留引用）。

4. **`lambda payload=data_for_vdb: ...` 用默认参数捕获** —— 因为 `lambda` 在循环里会晚绑定，而这里虽然不在循环里，但保持这个习惯是对的。

**`finally` 里的性能日志。**

```python
finally:
    performance_timing_log("[_merge_nodes_then_upsert] `%s` completed in %.4fs", entity_name, time.perf_counter() - timing_start)
```

**即：无论成功失败都记录耗时。** 这让「失败很快」与「失败很慢」的区别可观察——而后者往往意味着重试或超时。

### 边的合并：`_merge_edges_then_upsert` 的差异

**自环在最前面被拒（第二次）。**

```python
if src_id == tgt_id:
    return None
```

d01 里 `_handle_single_relationship_extraction` 已经拒过自环，这里再拒一次。**第二道是因为直接调用路径（`ainsert_custom_kg`）可以绕过抽取器。**

而返回 `None` 而不是抛错 —— 调用方（`_locked_process_edges`）判 `if edge_data is None: return None, []`。

**权重累加是本页最复杂的一段逻辑，而注释用了七段来说明为什么。**

```python
already_edge_source_set = set(already_source_ids)
new_weight_by_source: dict[str, float] = {}
for dp in edges_data:
    dp_source_id = dp.get("source_id")
    if not dp_source_id or dp_source_id in already_edge_source_set:
        continue
    new_weight_by_source.setdefault(dp_source_id, dp["weight"])
weight = sum(new_weight_by_source.values()) + sum(already_weights)
```

五处，每处对应注释里的一段论证：

1. **基本意图**：「Each extracted relation contributes weight 1.0 (or its parsed strength), so a stored edge's weight tracks the number of distinct contributing sources」。

2. **为什么过滤已存来源**：「On reprocess/resume the same source can be re-fed while it is already reflected in `already_weights` (the stored scalar), so only sum weights of edges whose `source_id` is NOT already stored -- otherwise weight double-counts and grows 1 -> 2 -> 3 per reprocess (the sibling of description accumulation)」。

   **这是幂等性的第二处，与描述去重同构。** 而注释点明了它们是「sibling」——同一类问题的两处表现。

3. **为什么用 `already_source_ids` 而不是 `existing_full_source_ids`**（这段最关键）：

> Filter on the EDGE's own `source_ids` (`already_source_ids`) -- the same source as `already_weights` -- NOT `existing_full_source_ids`, which can come from `relation_chunks_storage`, a SEPARATE store. `relation_chunks` can run ahead of the graph edge on a partial write (chunks upserted, `upsert_edge` not yet), so filtering on it would wrongly skip a genuinely new source (under-count), or, with no stored weight at all, recover the edge with weight 0.

   **即：两个存储可能不同步。** 追踪行可能已经写入了新数据，而图边还没更新。若用追踪行过滤，就会把「真的新来源」当成「已计入」而跳过——导致权重少算，甚至在「没有任何已存权重」时算出 0。

   **所以要过滤的对象必须是「与 `already_weights` 同源的那个列表」。** 这是一条精确的因果对应要求：**过滤依据必须与累加依据来自同一份快照**。

4. **假值的 `source_id` 不投票**：「A falsy source_id is dropped from `new_source_ids` above (it never persists), so it must not add weight either -- otherwise weight would outgrow the stored source count」。

   即权重不能超过来源数，否则 `weight >= 来源数` 那条契约（d03）会被破坏但方向相反。

5. **同一来源在同一次调用里可能出现多条记录**：

> `edges_data` can also carry more than one record for the SAME `source_id` within this one call: a single LLM extraction can state the same relation twice, or once in each direction (both land under the sorted edge key). Each source must contribute weight once ... keep the first record seen per source, the same first-occurrence convention used for description dedup.

   所以用 `setdefault`（只在键不存在时赋值）——**保首个**。

   而「or once in each direction」是因为 `all_edges` 的键是排序后的，所以 `(A,B)` 与 `(B,A)` 合并到同一个桶，同一个 `source_id` 可能出现两次。

**权重最后被夹到一个下界。**

```python
evidence_count = len({source for source in source_ids if source and source not in RELATION_NO_EVIDENCE_SOURCE_IDS})
weight = max(float(weight), float(evidence_count))
```

**注释说明了它的用途**：「Repair legacy/manual rows that predate the shared weight contract: every distinct real source contributes a baseline of 1, while an existing larger weight remains an optional importance boost」。

三处：

1. **`evidence_count` 是「不同真实来源数」** —— 排除空串与 `RELATION_NO_EVIDENCE_SOURCE_IDS`（无证据的哨兵值）。

2. **`max(weight, evidence_count)`** —— 向上夹。所以「权重永远不会小于来源数」，而「更大的权重被保留」（当作重要性加成）。

   这就是 d03 会详述的那条契约 `weight >= 不同真实来源 id 数` 的**实现点**。它的作用不是规范新数据（新数据天然满足），而是**修复历史/手工写入的旧数据**。

3. **注意用的是 `source_ids`（截断后的）而不是 `full_source_ids`** —— 所以截断会让下界变小。这与「权重随来源数增长」的意图一致（截断掉的来源不再计入）。

   但这也意味着：**FIFO 模式下权重会随截断而降不下来但也不再涨**，而 KEEP 模式下同理。所以「权重 >= 来源数」是相对**当前可见来源**的，不是「历史总来源」。

**关键词合并是排序后拼接。**

```python
all_keywords = set()
for i, keyword_str in enumerate(already_keywords, start=1):
    if keyword_str:
        all_keywords.update(k.strip() for k in keyword_str.split(",") if k.strip())
    await _cooperative_yield(i, every=32)
for i, edge in enumerate(edges_data, start=1):
    if edge.get("keywords"):
        all_keywords.update(k.strip() for k in edge["keywords"].split(",") if k.strip())
    await _cooperative_yield(i, every=32)
keywords = ",".join(sorted(all_keywords))
```

四处：

1. **用逗号拆分，而不是 `GRAPH_FIELD_SEP`** —— 注意 `already_keywords` 的取得用了 `split_string_by_multi_markers(already_edge["keywords"], [GRAPH_FIELD_SEP])`，然后这里又按逗号拆。所以两级拆分：先按字段分隔符拆出「关键词串」，再按逗号拆出「单个关键词」。

2. **`set` 去重 + `sorted` 后拼接** —— 所以关键词是**确定序**（字典序）。这与 `description` 的顺序（时间序）不同：关键词无顺序语义，所以排序即可。

3. **`sorted` 让输出可复现** —— 若用 `set` 直接拼，每次运行的顺序不同，会导致**同一份数据重算得到不同字符串**，进而让下游的相等比较失效。

   **这是一处与 d01 里 `done` 是 `set` 那个 bug 同类的防御**（虽然这里做对了）。

4. **两侧都有 `_cooperative_yield`** —— 两个循环都插了让出点。

**边的描述为空时抛错（而实体给兜底）。**

前面已经对比过。这里补一点：**边的描述是它的向量内容**，所以「无描述的关系」在向量库里没有表示，检索不到。所以抛错比造一个空关系合理。

### 候选超集与两阶段过滤

**`collect_kg_merge_candidates` 只做集合运算，但它的文档定义了契约。**

```python
for maybe_nodes, maybe_edges in chunk_results:
    candidate_entities.update(maybe_nodes.keys())
    for edge_key in maybe_edges:
        pair = tuple(sorted((edge_key[0], edge_key[1])))
        candidate_relations.add(pair)
        candidate_entities.update(pair)
```

三处：

1. **`tuple(sorted(...))` 是无向图规范化的唯一位置**（在候选收集这里）。而 `all_edges` 那边也做了一次 `tuple(sorted(edge_key))`。

   **两处各做一次排序** —— 若只做一处，两边的关系键可能不一致。

2. **`candidate_entities.update(pair)`** —— 关系端点进实体候选。

3. **返回两个 `set`** —— 而写入时**排序后转 list**：

```python
"entity_names": sorted(candidate_entities),
"relation_pairs": [list(pair) for pair in sorted(candidate_relations)],
```

   **排序是为了让持久化结果确定**。这与 d01 里 `done` 是 `set` 的教训一致——若直接把 `set` 写进存储，不同运行会得到不同的顺序，从而让「这份清单有没有变」无法比较。

**`all_nodes` / `all_edges` 的聚合用 `defaultdict(list)` 累积。**

```python
for i, (maybe_nodes, maybe_edges) in enumerate(chunk_results, start=1):
    for entity_name, entities in maybe_nodes.items():
        all_nodes[entity_name].extend(entities)
    for edge_key, edges in maybe_edges.items():
        sorted_edge_key = tuple(sorted(edge_key))
        all_edges[sorted_edge_key].extend(edges)
    await _cooperative_yield(i, every=32)
```

**注意这里是 `extend` 而不是 `update`** —— 所以同一个实体的多个块碎片被**拼成一个长列表**，然后整个列表交给 `_merge_nodes_then_upsert`。

**这就是「描述的排序键含 `timestamp`」有意义的原因**：碎片来自不同块（可能有不同时间戳），而 `extend` 保留了块的处理顺序，`sorted` 再按时间重排。

而 `chunk_results` 的顺序由 d01 里那个「必须从 `tasks` 取而不是 `done`」的修复保证 —— **这一页的正确性依赖 d01 那一处修复**。

## 关键取舍

**源码事实：** 合并分三阶段：Phase 0 先把**候选超集**写进 `full_entities`/`full_relations` 并**显式 flush**（`index_done_callback`）后，才允许第一次图变更；候选集 = 抽出实体 ∪ 所有关系端点（因为边处理会创建缺失端点）；空行也写（覆盖上次的旧锚点）；Phase 0 失败直接中止合并。

**源码事实：** `on_anchors_durable` 钩子在「锚点已持久化、尚未变更图」这个窗口内被 `await` 一次，用于把 `doc_status.metadata.kg_write_state` 的翻转记录下来；它抛错会中止合并（此时锚点在、图未改，方向安全）。

**源码事实：** 图变更的并发上限是 `llm_model_max_async * 2`；每个实体/关系用 `get_storage_keyed_lock` 按名互斥，关系锁用**排序后**的键（无向图，`(A,B)` 与 `(B,A)` 同一把锁），命名空间带工作区前缀。

**源码事实：** `wait_tasks_with_drain` 只覆盖「等待中」的取消；任务**创建循环**里有 `_cooperative_yield`，所以取消可能落在「创建了一半、尚未开始等待」的窗口。因此 `except` 里必须再 `cancel_and_drain_tasks(entity_tasks + edge_tasks)`，否则已创建的任务会在协程退出、恢复流程开始后继续写图。

**源码事实：** 合并的阶段汇总放 `finally` 且**幂等**（`summary_truncation_published` 标志）且 **await-free**；注释记录比 d01 更细的漏洞——只包住两个 `wait_tasks_with_drain` 不够，因为**阶段之间**还有 await 点（`sleep(0)`、状态锁），取消落在那里会跳过 absorb，导致调用方从未 absorb 的计数重算 journal、**覆盖**写前钩子已持久化的事件。

**源码事实：** `summary_tally`（本次合并）与 `truncation_tally`（文档级）两级，最后 `absorb` 合并，让 `doc_status` 同时覆盖抽取与合并。

**源码事实：** `truncation_write_ahead` 只对「失败恢复是累积式（ADDITIVE）」的调用方需要（自定义 chunk 路径），因为管道自身的重处理会 purge 上次贡献——判据是「这次变更的结果会不会在失败后存活」。

**源码事实：** 从图读回的 `entity_type` 会重新清洗（非字符串/空 → `"UNKNOWN"`，含逗号取第一个非空 token），但**没有复用** d01 的 `_normalize_and_validate_entity_type`——所以禁用字符与 `_RESERVED_ENTITY_TYPES` 的检查在这条读回路径上**不生效**。

**源码事实：** `source_id` 取两个来源且优先级明确：追踪存储（完整）优先于图节点（截断视图）；「存在但为空的追踪行是权威的」，不能从图节点回填（那会把已 purge 的 chunk 重新引入）；`has_chunk_tracking_row` 按 schema 判存在性而非列表真值。追踪存储写**完整**列表、图节点写**截断**后的列表；空列表不写追踪行。

**源码事实：** `KEEP` 模式会丢弃「来源已被截断掉」的新描述（避免「描述有内容但 `source_id` 无出处」），`FIFO` 模式**不过滤**任何描述（截断只在 `source_id` 层面）——两者语义不对称。

**源码事实：** `entity_type` 投票：已存类型算 **1 票**（不管经历过多少次合并）、排在列表最前且 `sorted` 稳定所以**平票胜**（一次误抽不能翻转类型）；`UNKNOWN`（大小写不敏感）不投票、让位给任何真实抽取，但若没有其他票则保留。

**源码事实：** 描述先去重（按文本、保首个）再按 `(timestamp, -len)` 排序，所以拼接顺序是「早期文档在前，同批次长描述优先」。

**源码事实：** `_combine_descriptions_dedup` **跨「已存 + 新」去重**（这是幂等的实现方式，顺带压掉历史重复）；比较与存储都在 `sanitize_text_for_encoding` **之后**（否则「清洗后才相同」的两条每次都会被当新的，造成持续泄漏）；清洗后为空的碎片丢弃（否则拼接出现连续分隔符）；**两阶段遍历刻意保留**，因为 `already_fragment` 是「存活已存碎片数」，日志与 `deduplicated_num` 都依赖它——不能合成一次 `dict.fromkeys`。

**源码事实：** 描述全空时实体给兜底串 `f"Entity {name}"` 并 warning，关系则**抛 `ValueError`**——因为关系的向量就是它的描述，无描述的关系无可检索内容。

**源码事实：** 写序是「构造并校验 VDB 载荷 → 写图 → 写 VDB（带重试）」；先构造载荷是为了让**确定性、不可重试**的截断失败发生在任何写入之前；图是主体、向量是派生。

**源码事实：** 边权重 = 「来源不在已存集合中的那些记录的权重之和（每来源只算首个）」+ 已存权重；**过滤依据必须是 `already_source_ids`**（与 `already_weights` 同源），不能用 `existing_full_source_ids`（来自独立存储、可能领先于图边，会导致少算甚至权重归零）；假值 `source_id` 不投票（否则权重会超过来源数）；同一 `source_id` 的多条记录用 `setdefault` 保首个。

**源码事实：** 权重最后被 `max(weight, evidence_count)` 向上夹，`evidence_count` 是 `source_ids`（截断后）里排除空串与 `RELATION_NO_EVIDENCE_SOURCE_IDS` 后的**不同**来源数——这是 `weight >= 来源数` 契约的实现点，用途是修复历史/手工写入的旧数据。

**源码事实：** 关键词先按 `GRAPH_FIELD_SEP` 拆、再按逗号拆，`set` 去重后 `sorted` 拼接——排序让输出确定可复现。

**源码事实：** 自环在边合并入口再拒一次（`return None`），因为 `ainsert_custom_kg` 可以绕过 d01 的抽取器校验。

**源码事实：** `collect_kg_merge_candidates` 返回 `set`，但**写入时排序后转 list**；`all_edges` 与候选集两处各自做 `tuple(sorted(...))` 规范化——两处必须都做，否则关系键不一致。

**架构推论：** 这一层的核心是**幂等性**，而它有两个同构的敌人：描述重复追加与权重重复累加。两者的修法相同——「按来源去重」——但实现位置不同：描述用文本去重（`seen` 集合），权重用来源 id 去重（`already_edge_source_set`）。这说明「幂等」不是在入口加一个「是否已处理」标志，而是**在每个累加点上问「这条的来源是不是已经算过了」**。前者需要对整份文档去重（且无法区分部分重跑），后者天然支持任意粒度重入。

**架构推论：** 「过滤依据必须与累加依据同源」这条纪律值得单独记。权重那段注释花了最长篇幅解释「不能用 `existing_full_source_ids`」，根因是**两个存储可能不同步**（追踪行已写、图边未写）。若混用，就会把新来源误判为已计入。这条纪律的一般形式是：**当你用 A 判断「是否已计入」、用 B 累加「计入了多少」时，A 与 B 必须来自同一次快照。**

**架构推论：** Phase 0 的「先写恢复锚点」是「没有事务时用可发现性换原子性」的具体形态。它不能阻止不一致，但保证不一致是**可发现的**——清理流程能按锚点找到「这个文档可能碰过哪些对象」，再逐个验证所有权。而 `on_anchors_durable` 钩子把「图可能已被污染」这个布尔状态翻转的那一刻精确地暴露出来，这是**把时间点做进接口**的做法。

**架构推论：** 两处「取消窗口」的教训（任务创建循环里的 `_cooperative_yield`、阶段之间的 `sleep(0)` 与锁获取）说明一件事：**只要有 `await`，就有一个取消窗口**。所以「在哪里处理取消」不能按「函数结构」设计，而必须按「所有 await 点」枚举。这份实现最终选择了 `finally` + 幂等 + await-free 三件套，这是唯一能覆盖任意窗口的组合。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/operate.py` | `merge_nodes_and_edges` | 三阶段编排：锚点 → 实体 → 关系 |
| `lightrag/operate.py` | `merge_nodes_and_edges._publish_summary_truncation` | 幂等、await-free 的阶段汇总 |
| `lightrag/operate.py` | `merge_nodes_and_edges._locked_process_entity_name` | 信号量 + 按名键控锁 + 取消检查 |
| `lightrag/operate.py` | `merge_nodes_and_edges._locked_process_edges` | 同上，锁键排序（无向图） |
| `lightrag/operate.py` | `collect_kg_merge_candidates` | 候选超集 = 实体 ∪ 关系端点 |
| `lightrag/operate.py` | `_merge_nodes_then_upsert` | 实体合并十一步 |
| `lightrag/operate.py` | `_merge_edges_then_upsert` | 关系合并（权重累加 + 关键词合并） |
| `lightrag/operate.py` | `_combine_descriptions_dedup` | 跨「已存+新」去重、sanitize 后比较、两阶段计账 |
| `lightrag/operate.py` | `_merge_nodes_then_upsert` 的 `entity_type` 投票段 | 已存 1 票 + 平票胜 + `UNKNOWN` 不投票 |
| `lightrag/operate.py` | `_merge_edges_then_upsert` 的权重段 | 按来源去重累加 + `max(weight, evidence_count)` 下界 |
| `lightrag/operate.py` | `_merge_nodes_then_upsert` 的 `source_id` 三层段 | 追踪存储优先、空行权威、完整/截断分流 |
| `lightrag/operate.py` | `_merge_nodes_then_upsert` 的 `file_path` 段 | 去重 + 上限截断 + 占位符 |
| `lightrag/base.py` | `StorageNameSpace.index_done_callback` | Phase 0 的 flush 屏障 |
| `lightrag/utils.py` | `sanitize_text_for_encoding` | 描述比较与存储前的归一 |
| `lightrag/utils.py` | `apply_source_ids_limit` | `KEEP` / `FIFO` 两种截断 |
| `lightrag/utils.py` | `has_chunk_tracking_row` | 按 schema 判追踪行存在性 |
| `lightrag/utils.py` | `merge_source_ids` | 来源 id 求并集 |
| `lightrag/utils.py` | `_cooperative_yield` | 纯 CPU 循环里的让出点 |

## 参考

- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)
- [lightrag 论文：Graph-based RAG](https://arxiv.org/abs/2410.05779)
- [设计文档：清理恢复契约](https://github.com/HKUDS/LightRAG/blob/main/docs/design/PurgeRecoveryContract.md)
- [Python 文档：asyncio 任务取消与 asyncio.shield](https://docs.python.org/3/library/asyncio-task.html#task-cancellation)

## 与主核 / AI 的关系

**合并实体**与主核（多模态数据库内核）的 **UPSERT + 多版本并发**面对同一个问题：同一行被多次写入时怎么合成一行。主核的答案是 MVCC——每次写入产生新版本，旧版本按 `xmin`/`xmax` 可见性规则存活。而这里没有版本，只有一份「就地合并」的记录。所以主核可以「先写着，之后决定留哪个」；这里必须**写的时候就把合并规则定下来**，且规则必须幂等——因为重跑会让同一条路径再走一遍。

**而「权重按来源去重累加」与主核的「唯一约束去重」是两条不同的路。** 主核用唯一索引把「重复插入」变成「约束冲突」，由调用方决定 `ON CONFLICT DO NOTHING/UPDATE`。这里没有唯一索引（关系用字符串键，且两个存储都可能不同步），所以只能靠「先查已存来源集合，再只累加新的」。**本质差别是：主核能用约束挡住重复，这里只能在代码里识别重复。**

第二处对照是 Phase 0 的「写前锚点」与主核的 **WAL**。WAL 的规则是「先写日志再改数据」——崩溃后重放日志即可恢复。这里的锚点是同一思路的弱化版：它不记录「改了什么」，只记录「可能改了哪些对象」。所以崩溃后**不能自动重放**，只能让清理流程去逐个验证。差别在于：WAL 需要精确的变更记录（所以值级日志），而这里只需要一个「可能触及」的集合（因为清理动作是「删掉这个文档的所有贡献」，不需要知道具体改了什么）。

第三处是 `entity_type` 的「已存类型只算一票」，与主核的 **`ANALYZE` 统计量更新**里「采样不覆盖全表时要限幅」是同一类谨慎。判据是**历史的权重不该压倒当前证据**——一个经历过百次合并的类型不该因为累积而永久锁定，否则一次早期的误判就再也修不回来。而「平票胜」又保证单次误抽不会翻转，两者合起来是「缓慢可纠、不可突变」。

对 AI 工程的实际意义在于：图谱合并是 graph-RAG 最容易出隐性数据问题的地方（描述越滚越长、权重越滚越大），而这些问题不会报错，只会让检索质量缓慢下降。这份实现给出的三条经验可直接用：**幂等要在每个累加点按来源去重，而不是在入口加标志**、**过滤依据必须与累加依据同源**（两个存储可能不同步）、以及**没有事务时先写可发现性锚点**（不求自动恢复，只求可定位）。
