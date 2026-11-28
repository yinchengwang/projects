---
project: lightrag
feature: context-assembly
title: 上下文组装与块合并
schema_version: 1
page_type: feature
refs:
  - path: lightrag/operate.py
    symbols: [_find_related_text_unit_from_entities, _find_related_text_unit_from_relations, _merge_all_chunks]
  - path: lightrag/operate.py
    symbols: [_attach_content_headings, _get_vector_context, _resolve_kg_search_queries]
---

# 上下文组装与块合并

## 一句话职责

把「实体」「关系」「向量块」三路来源最终收敛成一份去重、有序、带标题路径的块列表：实体与关系各自沿 `source_id` 反向找回原文块并做**按位置的配额分配**，三路结果再**轮转合并**，最后回填标题面包屑。

## 机制是什么

三路来源的性质完全不同，而它们的处理方式正对应这个差异：

| 来源 | 怎么得到 | 天然顺序 | 去重难点 |
|---|---|---|---|
| 向量块 | 直接查块向量库 | 相似度降序 | 自身已唯一 |
| 实体块 | 沿实体的 `source_id` 找回 | **无**（`source_id` 是无序集合） | 同块被多实体引用 |
| 关系块 | 沿关系的 `source_id` 找回 | **无** | 同上，且要与实体块去重 |

**关键困难在第一行与第二行的差别**：向量检索天然给出排名，而 `source_id` 里的一串块 id 没有顺序信息。**于是「选哪几块」必须自己造一个顺序**——这就是 `WEIGHT` 与 `VECTOR` 两种挑选策略存在的理由。

## 怎么实现的

### 一、实体块：三次遍历造出一个可控的选择

**第一次遍历：拆 `source_id`，并顺手完成两层去重。**

```
for entity_info in entities_with_chunks:
    deduplicated_chunks = []
    for chunk_id in entity_info["chunks"]:
        chunk_occurrence_count[chunk_id] = chunk_occurrence_count.get(chunk_id, 0) + 1
        if chunk_occurrence_count[chunk_id] == 1:
            deduplicated_chunks.append(chunk_id)
    entity_info["chunks"] = deduplicated_chunks
```

**`chunk_occurrence_count` 一物两用**：它既统计「这个块被多少个实体引用」（加权轮询的权重来源），又充当去重判据（`== 1` 即首次出现）。

**去重规则是「保留位置更早的实体所持的那份」** —— 于是同一块只归给第一个引用它的实体。注释在下游解释了这条的必要性：块归因必须唯一，否则同一个块会被计入两个实体的配额。

**第二次遍历：把被去重清空的实体摘掉。**

```
entities_with_chunks = [entity_info for entity_info in entities_with_chunks if entity_info["chunks"]]
```

注释说明它「不让这些实体虚增每组的块预算」。一个所有块都被别人抢先的实体，若留在列表里，会让 `group_count` 变大，于是 `_vector_chunk_quota` 算出的配额被摊薄。

**第三次遍历：按被引用次数排序每组内部的块。**

```
sorted_chunks = sorted(entity_info["chunks"], key=lambda chunk_id: chunk_occurrence_count.get(chunk_id, 0), reverse=True)
```

**引用次数高 = 多个实体都提到它 = 更重要。** 这是在没有相似度分数时能拿到的最好信号。

**而在「空列表」这个问题上，实体路径与关系路径的注释形成了一个对照。**

```
# Defensive only: unreachable on this path. Deduplication drops a chunk
# solely because an earlier-positioned entity already claimed it, so the
# first entity keeps every chunk it carries and at least one group always
# survives. The relation path's equivalent guard IS reachable, because it
# additionally excludes the chunks already delivered by the entity path,
# which can empty every relation group.
```

**这是一个很有价值的注释**：它指出两处看似相同的守卫中只有一处真的可达，而**原因在于「去重的依据不同」**。实体路径只与自己竞争，第一个实体永远保住全部块；关系路径要与实体路径竞争，于是可能全军覆没。

### 二、两种挑选策略：`WEIGHT` 与 `VECTOR` 的对等关系

**`WEIGHT` 用加权轮询，而它的配额是线性梯度。**

```
selected_chunk_ids = pick_by_weighted_polling(entities_with_chunks, max_related_chunks, min_related_chunks=1)
```

**`VECTOR` 用相似度挑选，配额由 `_vector_chunk_quota` 算出（见 e03 那页）。**

```
num_of_chunks = _vector_chunk_quota(max_related_chunks, len(entities_with_chunks))
selected_chunk_ids = await pick_by_vector_similarity(...)
```

**两者必须「对等」，而 `_vector_chunk_quota` 的文档解释了怎么对等**：VECTOR 的配额 `max(1, int(max * n / 2))` 是 WEIGHT 的线性梯度预算 `n * (max + 1) / 2` **减掉 `n / 2`** 那一项。

**这个「减掉一半」的含义**：WEIGHT 的梯度是 `max, max-1, ..., 1`（第一组拿最多），总和 `n(max+1)/2`；而 VECTOR 平均每组 `max/2` 块。也就是说 **VECTOR 的配额相当于 WEIGHT 的「平均组」**，而不是「第一组」。

**这是一个刻意的保守**：向量选择按相似度取，不保证每组都有份，所以总量取平均而不是最优情况的量。

**降级路径有三条，而它们都指向 `WEIGHT`。**

```
if kg_chunk_pick_method == "VECTOR" and not (query and chunks_vdb):
    logger.warning("Vector similarity chunk selection needs both a query and chunks_vdb, falling back to WEIGHT method")
    kg_chunk_pick_method = "WEIGHT"
...
if not actual_embedding_func:
    logger.warning("No embedding function found, falling back to WEIGHT method")
    kg_chunk_pick_method = "WEIGHT"
```

第一条的注释说明后果：「没有这个切换，下面两个分支都不会跑，每一块都会丢」。**这是一个「必须降级否则全丢」的关键路径。**

**而第三条降级来自配额为 0（e03 那页详述）：`pick_by_vector_similarity` 返回 `[]`，调用点把它读作「向量选择失败」并降级。** 于是 `WEIGHT` 是唯一的兜底——**这也解释了为什么 `WEIGHT` 必须是默认值**。

### 三、关系块：多一层「与实体块去重」

**它的第一步与实体路径相同，但多了一段前置的排除。**

```
entity_chunk_ids = set()
if entity_chunks:
    for chunk in entity_chunks:
        chunk_id = chunk.get("chunk_id")
        if chunk_id: entity_chunk_ids.add(chunk_id)
```

然后在拆 `source_id` 时：

```
for chunk_id in relation_info["chunks"]:
    if chunk_id in entity_chunk_ids:
        removed_entity_chunk_ids.add(chunk_id)
        continue
    chunk_occurrence_count[chunk_id] = chunk_occurrence_count.get(chunk_id, 0) + 1
    ...
```

**`removed_entity_chunk_ids` 这个集合记录「被跳过多少次」，而它防的是「重复计数」。** 若同一个块被三个关系都提到，且它已在实体块里，那么每次遇到它都会 `continue`——而计数提升必须在 `continue` 之前就被跳过，否则那个块的权重会被虚增。

**于是关系路径的块是「实体路径没覆盖的那部分」** —— 三路来源形成了一种**优先级顺序**：向量块 → 实体块 → 关系块。而 `_merge_all_chunks` 的轮转顺序与此一致。

**为什么这个顺序是对的**：向量块是「整篇里最像的」，实体块是「与命中实体直接相关」，关系块是「与命中关系相关」——**逐级放宽**。用后面的补前面的缺口，符合「精度递减」的直觉。

### 四、`_merge_all_chunks`：轮转合并与两个细节

**轮转的骨架与 e01 那页的实体合并完全相同。**

```
max_len = max(len(vector_chunks), len(entity_chunks), len(relation_chunks))
for i in range(max_len):
    if i < len(vector_chunks): ... 处理向量块 ...
    if i < len(entity_chunks): ... 处理实体块 ...
    if i < len(relation_chunks): ... 处理关系块 ...
```

**三个分支各自做「取 id → 查重 → 追加」**，而 id 的来源有一个兼容写法：

```
chunk_id = chunk.get("chunk_id") or chunk.get("id")
```

**两种键名并存**——向量库返回的原始结果用 `id`，而 `_get_vector_context` 把它规范成 `chunk_id`。这行 `or` 让合并函数能接受两种形态。

**去重集合跨三个来源共用（`seen_chunk_ids`）**，于是同一块即使三路都给也只出现一次。

**合并日志是一份「三进一出」的账。**

```
origin_len = len(vector_chunks) + len(entity_chunks) + len(relation_chunks)
...
logger.info(f"Round-robin merged chunks: {origin_len} -> {len(merged_chunks)} (deduplicated {origin_len - len(merged_chunks)})")
```

**三个数字一起给**，于是「去重掉了多少」是可查的。**当块数比预期少时，这个差值直接指向「去重太狠」还是「检索太少」。**

**标题回填的位置是刻意的。**

```
# Backfill heading path before token truncation so it counts toward the budget
if text_chunks_db and text_chunks_db.global_config.get("enable_content_headings", False):
    await _attach_content_headings(merged_chunks, text_chunks_db)
```

**注释写明「在 token 截断之前」** —— 因为标题路径会进提示，占 token。若回填在截断之后，预算就会被低估，最终提示可能真的超限。

**这与 e01 那页的「预览与真实请求必须同预算」是同一条原则的另一个侧面：凡是会出现在最终提示里的东西，都必须在预算计算之前就位。**

### 五、`_attach_content_headings`：为什么不能沿用已有的标题

**注释解释了「为什么需要回填」而不是「直接复用检索结果里的标题」。**

> 向量块从不携带 `heading` 字段（`chunks_vdb` 不存它），而轮转合并也把实体/关系带来的标题丢掉了。所以我们按 `chunk_id` 到 `text_chunks` 存储里查，附上父标题路径。

三个原因叠加：

1. **向量块的来源（`chunks_vdb`）根本不存 `heading`** —— 向量库只存被嵌入的文本与少量元数据。
2. **实体/关系路径拿到的块可能带标题，但轮转合并把它们丢掉了** —— 合并时只保留 `content` / `file_path` / `chunk_id` 三个字段。
3. **所以必须回查 `text_chunks`**（键值存储里存着完整的块行）。

**这是一个「信息在中间环节被丢弃后在终点补回」的模式**，而它本来可以避免：合并时保留 `heading` 就好。但那样会让合并函数依赖「每个来源都带标题」这个不成立的前提。**回填把「标题从哪来」这件事集中到一处，代价是一次批量查询。**

**而「只有真的有父标题的块才写字段」是一个减小提示体积的选择。**

```
if headings:
    chunk["content_headings"] = headings
```

文档说明：「只有真的带父标题的块才得到这个字段，于是空路径不会出现在发给 LLM 的 JSON 里」。

**这与 e03 那页「摘掉 `file_path`/`created_at` 再算 token」是同一类优化**：不要让不携带信息的字段占据预算。

**而回填必须离开事件循环，理由写得很具体。**

```
# The whole loop is one submission, keeping submissions O(1) per request.
# It has to leave the event loop at all because this tokenizer is the
# long-lived one the storages captured at init, i.e. the SAME object every
# concurrent query uses -- and two of its other consumers
# (_apply_token_truncation, process_chunks_unified) now run in the tokenizer
# executor. Left here, two concurrent queries would contend: one holding the
# tokenizer in a worker thread while the other waits for it on the loop.
await run_in_tokenizer_executor(_backfill)
```

**这段注释把「为什么」拆成了两层：**

1. **必须离开事件循环** —— 因为 `tokenizer` 是存储初始化时捕获的长寿对象，**每个并发查询用的是同一个**。
2. **必须与另外两个消费者在同一个执行器里** —— 否则「一个在工作线程里持有 tokenizer，另一个在事件循环上等它」会形成争用。

**而「整段循环作为一次提交」是为了让提交数是 O(1) 而不是 O(块数)** —— 执行器通常是单线程的，逐块提交会让 N 个块排 N 次队。

**这是「共享可变对象的并发访问」的经典处理**：把访问串行化到一个专用执行器，而不是加锁——因为 tokenizer 本身可能不是线程安全的，加锁也不够。

### 六、`_get_vector_context`：不吞异常，但也不检查重复

**它的核心是「不 try/except」，而理由写在注释里。**

> 这里没有宽泛的 try/except——镜像 `_get_node_data` / `_get_edge_data`，它们的 `entities_vdb` / `relationships_vdb` 查询也被留作向上传播。**一个后端的查询失败不是「零个相关块」**：在这里吞掉它，会让一次瞬时的向量库错误表现为一个自信的「没有结果」答案（而在 mix 模式下会静默丢掉向量检索那一支，同时 KG 结果照常流出）。

**三种错误的区分在这里被明确**：零结果（合法的空）、查询失败（异常）、部分失败（mix 下丢一支）。**吞异常会把后两者伪装成第一种。**

**而它把向量库的结果规范成统一形态。**

```
chunk_with_metadata = {
    "content": result["content"],
    "created_at": result.get("created_at", None),
    "file_path": result.get("file_path", "unknown_source"),
    "source_type": "vector",   # Mark the source type
    "chunk_id": result.get("id"),  # Add chunk_id for deduplication
}
```

**`content` 用下标（必须存在）、其余用 `.get` 带默认值。** 而 `source_type: "vector"` 是一个标记——**它只在向量块上设，实体/关系路径设的是 `"entity"` / `"relation"`**。于是下游能知道每块来自哪条路。

**这个标记的用途在 `chunk_tracking` 里更明显**：

```
chunk_tracking[chunk_id] = {"source": "C", "frequency": 1, "order": i + 1}
```

**`"C"` 是向量块的代号（Chunk），而实体是 `"E"`、关系由关系路径记录。** 三个字母代号 + `frequency` + `order` 构成一份「这块是怎么来的」的完整账。这份账最终进 `raw_data`，让调用方能解释「为什么这几块出现在答案的引用里」。

**而 `"frequency": 1` 的注释值得记**：「向量块频率永远是 1」。因为向量检索每块只出现一次（不像实体块可能被多个实体引用），所以那个字段在向量路径上没有信息量——但它必须填，因为下游要统一处理。

### 七、`_resolve_kg_search_queries`：兜底不越界

**这个函数的文档写得比实现重要得多，因为它界定了「兜底到什么程度」。**

```
entity_query = ll_keywords if mode in ("local", "hybrid", "mix") else ""
relation_query = hl_keywords if mode in ("global", "hybrid", "mix") else ""
fallback = _kg_keyword_fallback(mode, ll_keywords, hl_keywords)
if fallback == "entities_by_high_level_keywords":
    entity_query = hl_keywords
    ...
elif fallback == "relationships_by_low_level_keywords":
    relation_query = ll_keywords
```

**兜底是「同一个存储换个关键词」，而不是「换个存储」。** 文档写得很直接：

> 一个模式永远不会掉到它不拥有的存储上：`local` 在没有低层关键词时仍然是一次实体搜索，它不会变成 `global`。

**这条约束的意义在于保持用户的意图**：用户选 `local` 是「只想看实体」，若因为关键词缺失而改成查关系，返回的就是用户没要的东西——**而且用户不会知道**。

**而兜底的名称是缓存键的一部分。**

```
def _kg_keyword_fallback(...):
    """The name is part of the answer-cache key (see ``kg_query``), so renaming a value retires the cached answers of every request that took that fallback."""
```

**这句「改名会作废那些走了兜底的请求的缓存答案」是一个很少见的提醒** —— 它把「一个字符串常量的命名」与「缓存失效」联系起来。**在会缓存结果的地方，任何进入缓存键的字符串都是版本号。**

## 关键取舍

**源码事实：** 实体块的选择必须自己造顺序（`source_id` 是无序集合），于是有 `WEIGHT`（按被引用次数的加权轮询）与 `VECTOR`（按相似度）两种策略，`WEIGHT` 是默认与兜底。

**源码事实：** `chunk_occurrence_count` 一物两用：既算权重（被多少实体引用），又当去重判据（`== 1` 即首次出现）。去重规则是「保留位置更早的实体所持的那份」。

**源码事实：** 被去重清空的实体必须从列表里摘掉，否则会让 `group_count` 变大、摊薄 `_vector_chunk_quota` 算出的配额。

**源码事实：** 实体路径与关系路径各有一处「列表被清空」的守卫，但只有关系路径的那处可达——因为它额外排除了实体路径已交付的块，可能把每一组都清空。

**源码事实：** 关系路径多一层「排除实体路径已交付的块」，并用 `removed_entity_chunk_ids` 防止同一块被多次跳过时虚增权重。于是三路形成优先级：向量 → 实体 → 关系。

**源码事实：** `VECTOR` 的配额（`max(1, int(max * n / 2))`）相当于 `WEIGHT` 线性梯度预算的「平均组」而不是「第一组」——因为向量选择不保证每组都有份，总量取保守值。

**源码事实：** 三条降级路径都指向 `WEIGHT`，其中「缺 query 或 chunks_vdb」那条的注释说明「没有这个切换，两个分支都不跑，每一块都会丢」。

**源码事实：** 三路轮转合并，`seen_chunk_ids` 跨来源共用；块 id 取 `chunk.get("chunk_id") or chunk.get("id")`（向量库原始结果用 `id`，规范化后用 `chunk_id`）。

**源码事实：** 合并日志同时给「原始总数 → 合并后数（去重掉的数）」三个数字，用于判断块少是去重太狠还是检索太少。

**源码事实：** 标题回填必须发生在 token 截断**之前**，因为标题会进提示占 token。

**源码事实：** 标题不能复用检索结果：`chunks_vdb` 不存 `heading`，而轮转合并只保留 `content`/`file_path`/`chunk_id` 三个字段。回填按 `chunk_id` 查 `text_chunks` 存储，且只写非空路径（空路径不进提示）。

**源码事实：** 回填必须走 tokenizer 执行器，因为 tokenizer 是存储初始化时捕获的**长寿共享对象**；且整个回填作为**一次**提交（O(1) 次提交/请求）。

**源码事实：** `_get_vector_context` 明确不吞异常——「后端的查询失败不是零个相关块」，吞掉会让瞬时错误表现为自信的「无结果」，在 mix 下还会静默丢一支。

**源码事实：** `chunk_tracking` 记 `{source, frequency, order}`，向量块代号 `"C"`、实体块 `"E"`，向量的 `frequency` 恒为 1（每块只出现一次）。

**源码事实：** 关键词兜底只在**同一个存储**上换关键词，绝不让模式掉到它不拥有的存储——`local` 缺低层词仍是实体搜索，不会变成 `global`。

**源码事实：** 兜底名称（`entities_by_high_level_keywords` 等）是答案缓存键的一部分，改名会作废走过该兜底的所有请求的缓存。

**架构推论：** 这一页的核心是**「顺序从哪来」**。向量检索天然有序，而图谱内容——实体、关系、它们的 `source_id` 集合——全都是无序的。于是组装层必须自己制造顺序，而它用的两个信号都很粗糙：被引用次数（`WEIGHT`）与语义相似度（`VECTOR`）。**这不是缺陷，而是图谱结构的固有代价**：图告诉你「谁连着谁」，不告诉你「谁更重要」。

**架构推论：** 三路来源的优先级（向量 → 实体 → 关系）反映的是**精度递降**，而它通过「后面的排除前面的」来实现。这种做法比「按权重加权求和」简单得多，代价是低优先级来源在高优先级来源覆盖充分时会一无所获。**当来源的精度差异远大于其数量差异时，排除法优于加权法**——因为加权法会给高精度来源的重复块额外加分，反而稀释了它。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/operate.py` | `_find_related_text_unit_from_entities` | 实体块的三次遍历：拆 `source_id`、去重、按引用数排序 |
| `lightrag/operate.py` | `_find_related_text_unit_from_relations` | 关系块，额外排除实体已交付的块 |
| `lightrag/operate.py` | `pick_by_weighted_polling` / `pick_by_vector_similarity` | 两种挑选策略，后者失败则降级到前者 |
| `lightrag/operate.py` | `_merge_all_chunks` | 三路轮转合并与跨来源去重 |
| `lightrag/operate.py` | `_attach_content_headings` | 按 `chunk_id` 回填标题路径，在 token 截断之前 |
| `lightrag/operate.py` | `_truncate_section_context` | 标题路径的 token 预算（e03 详述） |
| `lightrag/operate.py` | `_get_vector_context` | 块向量检索，异常向上传播 |
| `lightrag/operate.py` | `_resolve_kg_search_queries` | 模式 → 存储映射 + 同存储内换词兜底 |
| `lightrag/operate.py` | `_kg_keyword_fallback` | 兜底名称（也是答案缓存键的一部分） |
| `lightrag/operate.py` | `chunk_tracking` | `{source, frequency, order}` 三件套的来源账 |

## 参考

- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)
- [论文：LightRAG](https://arxiv.org/abs/2410.05779)
- [lightrag 文档](https://github.com/HKUDS/LightRAG/blob/main/README.md)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

块合并与主核的**多路归并（merge join / union all）**同构：都是把若干有序流按某种规则合成一个。而 lightrag 的版本有一个数据库不会遇到的前提：**只有一路有序**。

主核里每一路输入都自带排序键，于是归并是纯粹的「比较头部」。这里只有向量块有序，图谱那两路的顺序是组装层临时造出来的——**这相当于数据库里对没有索引的列做排序，但连排序键都要现场发明**。

**「后面的排除前面的」与主核的去重（`UNION` 而非 `UNION ALL`）动机相同但做法不同。** SQL 的 `UNION` 在归并时按行标识去重，两个来源地位对等；这里的排除法给三个来源排了序，**先到先得**。差别的根源是「等价」的定义：SQL 里两行只要值相同就等价，而这里同名块虽然等价，来源的可靠性却不等价。

**`chunk_tracking` 与数据库的执行计划统计（`EXPLAIN ANALYZE` 的行数来源）同构。** 两者都记录「这个结果来自哪个算子、多少行」，用途也一样：出了问题能定位到具体一路。

对 agent 工程的直接价值有三条。其一，**无序内容的顺序必须自己造，而造顺序的方案决定了下游能做什么**。用被引用次数造顺序简单但粗糙（长文档的自然优势）；用相似度造顺序精确但昂贵且需要 query。默认选前者、有条件用后者，是合理的分层。其二，**「后面排除前面」优于「加权求和」当来源精度差异明显时**。加权法会让高精度来源的重复块反复加分，稀释它自己的信号。其三，**中间环节丢掉的字段要在终点补回，但补的地方要集中**。标题在合并时被丢、在回填时补，代价是一次批量查询；若让每个来源都负责带上标题，合并函数就要假设一个不成立的前提。
