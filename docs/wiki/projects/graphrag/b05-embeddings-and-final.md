---
project: graphrag
feature: embeddings-and-final
title: 文本嵌入与最终产物
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/index/workflows/generate_text_embeddings.py
    symbols: [generate_text_embeddings, EmbeddingFieldConfig]
  - path: packages/graphrag/graphrag/index/workflows/create_final_text_units.py
    symbols: [create_final_text_units]
  - path: packages/graphrag/graphrag/index/workflows/create_final_documents.py
    symbols: [create_final_documents]
---

# 文本嵌入与最终产物

## 一句话职责

`generate_text_embeddings` 按配置里点名的字段（文本单元正文 / 实体「标题:描述」/ 社区报告全文）逐字段流式嵌入并写入独立向量库；`create_final_text_units` 与 `create_final_documents` 则补上索引的最后一块拼图——**把「谁提到谁」的反向查找表铺平成每行的 id 列表**。

## 机制是什么

这两件事表面上无关，实际在同一层：**都是「把一个全表扫描的结果，写成每行自己的字段」。**

嵌入的扫描对象是「要嵌的文本」，结果写进向量库（不进主表）；反向引用的扫描对象是实体/关系/文本单元三张表，结果写成 `entity_ids` / `relationship_ids` / `text_unit_ids` 三列。后者是**空间换时间**的典型：`text_units` 表里的 `entity_ids` 完全可以从 `entities.text_unit_ids` 反推，但每查一次都要扫全表并做列表包含判断——所以把它预先算好。

而嵌入这一步的独特之处在于**它的输出不在这套表里**。索引的六张主表（documents / text_units / entities / relationships / communities / community_reports）都是可读可 diff 的表格，嵌入是二进制向量，存在 LanceDB 之类的向量库里，按 `index_schema` 的字段名分成多个索引。于是 `generate_text_embeddings` 是唯一一个「写出去的东西不在 `output_storage` 里」的 workflow——它写的是 `vector_store.db_uri` 指向的位置。

## 怎么实现的

**要嵌哪些字段是完全可配的，而可配的粒度是「字段名 → 一个配置对象」。**

```
@dataclass
class EmbeddingFieldConfig:
    """Configuration for a single embedding field.

    Describes which source table and column to embed, and an
    optional row transform to apply before embedding.
    """

    name: str
    table_name: str
    embed_column: str
    row_transform: RowTransformer | None = None


EMBEDDING_FIELDS: dict[str, EmbeddingFieldConfig] = {
    text_unit_text_embedding: EmbeddingFieldConfig(
        name=text_unit_text_embedding,
        table_name="text_units",
        embed_column="text",
    ),
    entity_description_embedding: EmbeddingFieldConfig(
        name=entity_description_embedding,
        table_name="entities",
        embed_column="title_description",
        row_transform=transform_entity_row_for_embedding,
    ),
    community_full_content_embedding: EmbeddingFieldConfig(
        name=community_full_content_embedding,
        table_name="community_reports",
        embed_column="full_content",
    ),
}
```

**三个字段名是常量**（`"text_unit_text"` / `"entity_description"` / `"community_full_content"`，定义在 `config/embeddings.py`），而 `EMBEDDING_FIELDS` 把它们映射成「从哪张表的哪一列取文本」。

四处值得看：

**第一，实体的 `embed_column` 是 `"title_description"`，一个源表里不存在的列。**

`"title_description"` 由 `transform_entity_row_for_embedding` 现造（a04 讲过）：`f"{title}:{description}"`。也就是说**嵌入的输入可以是「派生列」而不是原列**，这靠 `row_transform` 支持。

**为什么要拼？** 因为只嵌描述会丢掉「用户很可能就是按实体名搜」这个最强信号；只嵌名字又太短。拼起来让向量同时承载名字与语义。

**第二，`row_transform` 的类型是 `RowTransformer`，而它被直接传给 `TableProvider.open(transformer=...)`。**

```
input_table = await stack.enter_async_context(
    table_provider.open(
        field_config.table_name,
        truncate=False,
        transformer=field_config.row_transform,
    )
)
```

**注意 `truncate=False`** ——只读，不能清空源表。这是一个容易写错的地方：`open` 的默认是 `truncate=True`，而这里读的是**别人写好的表**，若清空就毁掉了上游产物。

**第三，三个字段各自的源表不同，而代码不假设它们一定存在。**

```
if not await table_provider.has(field_config.table_name):
    logger.warning(
        "Embedding %s is specified but source table '%s' "
        "is not in storage. Skipping.",
        field_config.name,
        field_config.table_name,
    )
    continue
```

**这是必要的**：`extract_claims` 默认关闭，所以社区报告可能没有 covariates；`Fast` 索引路径里某些表也可能缺失。缺表就跳过并告警，而不是抛错。

**第四，每个字段建一个独立的向量索引。**

```
vector_store = create_vector_store(
    config.vector_store,
    config.vector_store.index_schema[field_config.name],
)
vector_store.connect()
```

**`index_schema[name]` 直接下标**——这就是 a01 里 `_validate_vector_store`「给全部核心字段铺满 schema」的理由：否则用户只配了 `embed_text.names` 的一部分而没配 `index_schema` 时，这里会 `KeyError`。

**每个字段一个 `VectorStore` 实例、一个 `connect()`，且同一路径写同一目录。** 于是三个索引在同一个 LanceDB 目录下以不同表名共存（`index_schema` 里有 `index_name`）。

**`embed_text` 的核心是「缓冲到足够大再洗」。**

```
async def embed_text(input_table, callbacks, model, tokenizer, embed_column, batch_size,
                    batch_max_tokens, num_threads, vector_store, id_column="id",
                    output_table=None) -> int:
    """Embed text from a streaming Table into a vector store.

    Rows are buffered before flushing to ``run_embed_text``,
    which dispatches API batches concurrently up to
    ``num_threads``.  The buffer is sized so each flush produces
    enough batches to saturate the concurrency limit.
    """
    vector_store.create_index()

    buffer: list[dict[str, Any]] = []
    total_rows = 0
    flush_size = batch_size * num_threads

    async for row in input_table:
        text = row.get(embed_column)
        if text is None:
            text = ""

        buffer.append({id_column: row[id_column], embed_column: text})

        if len(buffer) >= flush_size:
            total_rows += await _flush_embedding_buffer(...)
            buffer.clear()

    if buffer:
        total_rows += await _flush_embedding_buffer(...)

    return total_rows
```

**`flush_size = batch_size * num_threads` 是关键公式，而注释解释了它：**

> The buffer is sized so each flush produces enough batches to saturate the concurrency limit.

**这处推理很值得记**：`run_embed_text` 内部按 `batch_size` 切批并发（上限 `num_threads`）。若每次只喂一个 batch，再多并发也没用——并发度退化成 1。所以缓冲到「批数 ≥ 并发数」再洗，才让并发真正起作用。默认 `batch_size=16`、`concurrent_requests=25`，于是 `flush_size=400` 行。

**`text is None` 时替换成空串而不是跳过。** 这是一个有意的选择：跳过会让 id 与向量错位（后面 `zip(ids, vectors, strict=True)` 依赖两边等长），而空串至少能嵌出一个向量（虽然无意义）。真正的跳过发生在后面——向量为 `None` 时才丢。

**洗缓冲区时有一个 `strict=True` 的 zip，它既是保障也是脆弱点。**

```
result = await run_embed_text(texts, callbacks, model, tokenizer, batch_size, batch_max_tokens, num_threads)

vectors = result.embeddings or []
skipped = 0
documents: list[VectorStoreDocument] = []
for doc_id, doc_vector in zip(ids, vectors, strict=True):
    if doc_vector is None:
        skipped += 1
        continue
    if type(doc_vector) is np.ndarray:
        doc_vector = doc_vector.tolist()
    documents.append(VectorStoreDocument(id=doc_id, vector=doc_vector))
```

**`zip(..., strict=True)` 保证「输入文本数」与「返回向量数」一致**——这是正确的防护：若对齐错了，后面所有 id 与向量都会错位一位，而且完全不会报错。

**但 `vectors = result.embeddings or []` 这个兜底让脆弱的场景更脆弱了**：若 `embeddings` 是 `None`（例如模型调用整体失败），`vectors` 变成 `[]`，于是 `zip(ids, [], strict=True)` 在 `len(ids) > 0` 时**直接抛 `ValueError`**（不是 「Skipped N rows」的告警）。于是「整体失败」表现为异常，而「部分向量为 None」表现为消融——两条路径的处理方式完全不同，前者会中断整个 workflow。

**`if type(doc_vector) is np.ndarray` 用的是精确类型比较而非 `isinstance`。** 于是 `np.ndarray` 的子类不会被转换——`tolist()` 只对精确的 `ndarray` 调用。这是一个「够用但不严谨」的写法（`isinstance` 才是常规选择）。

**告警统计的是「本缓冲区跳过多少」，而不是「总跳过多少」。**

```
if skipped > 0:
    logger.warning(
        "Skipped %d rows with None embeddings out of %d",
        skipped, len(buffer),
    )
```

**`skipped` 在每次 `_flush_embedding_buffer` 内重新计数**，所以告警是「这一批 400 行里跳了 3 行」。若一个索引有 100 个批次各跳 3 行，会打印 100 条告警而没有任何汇总。而返回给调用方的是 `len(buffer)`（**包括被跳过的行**）——所以 `total_rows` 是「处理过的行数」而不是「嵌入成功的行数」，日志里那句 `Embedded %d rows for %s` 会高估。

**可选的 `output_table` 把向量另存一份到表里。**

```
if output_table is not None:
    for doc_id, doc_vector in zip(ids, vectors, strict=True):
        if doc_vector is None:
            continue
        if type(doc_vector) is np.ndarray:
            doc_vector = doc_vector.tolist()
        await output_table.write({"id": doc_id, "embedding": doc_vector})
```

**第二个循环与第一个几乎重复**（同样的 `zip`、同样的 `None` 判断、同样的 `tolist`）。可以合并成一个循环，但它分成两个的效果是「文档列表先攒齐一次 `load_documents`，再逐行写表」——批量写向量库与逐行写表两种节奏不同。

**这段只在 `config.snapshots.embeddings` 为真时启用**（默认 `False`）。启用后向量会以 `embeddings.<字段名>` 为表名落在 output storage 里，供调试或外部使用。

**`run_workflow` 的返回是 `result=None`。**

```
await generate_text_embeddings(config=config, table_provider=context.output_table_provider, ...)
return WorkflowFunctionOutput(result=None)
```

**与其他 workflow 返回「前 5 行样本」不同，这里什么都不返回** ——因为产物不在表里，没有「样本行」可言。这是这套流水线里唯一一个 `result=None` 的 workflow。

**`create_final_text_units` 靠三个倒排 map 补齐反向引用。**

```
entity_map = await _build_multi_ref_map(entities_table)
relationship_map = await _build_multi_ref_map(relationships_table)
covariate_map: dict[str, list[str]] = (
    await _build_covariate_map(covariates_table) if covariates_table is not None else {}
)
```

而 `_build_multi_ref_map` 只有五行：

```
async def _build_multi_ref_map(table: Table) -> dict[str, list[str]]:
    """Build a text_unit_id -> [row_id] reverse lookup from a table with a text_unit_ids list field.

    Expects the table to have been opened with a transformer that
    already parsed ``text_unit_ids`` into a Python list.
    """
    result: dict[str, list[str]] = {}
    async for row in table:
        for tuid in row["text_unit_ids"]:
            result.setdefault(tuid, []).append(row["id"])
    return result
```

**注意 docstring 里那句「Expects the table to have been opened with a transformer that already parsed `text_unit_ids` into a Python list」** ——这是与 a04 的直接耦合点。若忘了传 `transformer=transform_entity_row`，`row["text_unit_ids"]` 会是字符串 `"['a', 'b']"`，而 `for tuid in "...":` 会**逐字符迭代**，产出「每个字符都是一个文本单元 id」的垃圾映射。

**这是一个极隐蔽的失败模式**：没有异常、没有告警，只是倒排表全是单字符。它之所以不常发生，是因为 `run_workflow` 里每张表都传了对应的 transformer：

```
context.output_table_provider.open("text_units", transformer=transform_text_unit_row) as text_units_table,
context.output_table_provider.open("entities", transformer=transform_entity_row) as entities_table,
context.output_table_provider.open("relationships", transformer=transform_relationship_row) as relationships_table,
```

**但 `_build_multi_ref_map` 的名字里那个 `multi_ref` 与 docstring 里「expects」的措辞，说明作者知道这个前置条件**——只是用注释而不是断言来表达。

**`covariates_table` 用 `nullcontext()` 兜住「未启用 claims」的情况。**

```
has_covariates = (
    config.extract_claims.enabled
    and await context.output_table_provider.has("covariates")
)
# nullcontext() stands in when covariates are disabled so we can use
# a single async with block regardless.
cov_ctx = (
    context.output_table_provider.open("covariates")
    if has_covariates
    else nullcontext()
)
```

**注释说明了理由：「这样无论有没有 covariates 都只用一个 `async with` 块」。** 代价是块内要判 `covariates_table is not None`（`nullcontext()` 的产出是 `None`）。

**`_build_covariate_map` 与 `_build_multi_ref_map` 结构不同，因为字段名不同。**

```
async def _build_covariate_map(table: Table) -> dict[str, list[str]]:
    """Build a text_unit_id -> [covariate_id] reverse lookup from the covariate table."""
    result: dict[str, list[str]] = {}
    async for row in table:
        result.setdefault(row["text_unit_id"], []).append(row["id"])
    return result
```

**covariate 表的字段是 `text_unit_id`（单数）**，因为一个 claim 只来自一个文本单元；而实体/关系是 `text_unit_ids`（复数列表），因为一个实体可能出现在多个块里。**单复数在字段名上区分，而两个 map 的结构因此不同** ——一个是「遍历列表往多键插」，一个是「单键直接插」。

**`create_final_text_units` 的主循环把三张 map 拼成一行。**

```
async for row in text_units_table:
    fields = {
        "id": row["id"],
        "human_readable_id": human_readable_id,
        "text": row["text"],
        "n_tokens": row["n_tokens"],
        "document_id": row["document_id"],
        "entity_ids": entity_map.get(row["id"], []),
        "relationship_ids": relationship_map.get(row["id"], []),
        "covariate_ids": covariate_map.get(row["id"], []),
    }
    out = {c: fields[c] for c in TEXT_UNITS_FINAL_COLUMNS}
    await output_table.write(out)
    human_readable_id += 1
```

**注意读与写用的是同一个文件的两个 Table 句柄。**

```
context.output_table_provider.open("text_units", transformer=transform_text_unit_row) as text_units_table,   # 读
...
context.output_table_provider.open("text_units") as output_table,                                            # 写
```

`run_workflow` 里的注释解释了为什么安全：

> The read and write handles for text_units share the same file. CSVTable writes to a temp file and moves it on close(), so reads from the original remain safe throughout.

**这是 CSVTable 的 `truncate=True` 实现细节（写临时文件、close 时覆盖）被上层依赖的一个例子。** 换成「就地截断再写」的实现，这个 workflow 会把自己的输入清掉。

**`create_final_documents` 更简单：一个 map + 一次流式回填。**

```
async def create_final_documents(text_units_table, documents_table, output_table) -> list[dict]:
    mapping: dict[str, list[str]] = {}
    async for row in text_units_table:
        document_id = row.get("document_id", "")
        if document_id:
            mapping.setdefault(document_id, []).append(row["id"])

    sample_rows: list[dict[str, Any]] = []
    async for row in documents_table:
        row["text_unit_ids"] = mapping.get(row["id"], [])
        out = {c: row.get(c) for c in DOCUMENTS_FINAL_COLUMNS}
        await output_table.write(out)
        if len(sample_rows) < 5:
            sample_rows.append(out)
    return sample_rows
```

**这段与前一个的结构差异在于「方向」**：`create_final_text_units` 是把「多个来源」汇到文本单元上（三张表 → 一个表），`create_final_documents` 是把「一个来源」铺到文档上（一张表 → 一个表）。

**但它与 `create_final_text_units` 有一个实质差异：后者用了 `_build_multi_ref_map` 抽出的辅助函数，前者把同样的逻辑内联写了一遍。** 两处的模式（遍历列表、`setdefault(...).append(...)`）完全一致，只是键名不同——一次的键是 `text_unit_ids`（列表），一次的键是 `document_id`（标量）。

**`if document_id:` 这个判断过滤空值。** `text_units` 表的 `document_id` 理论上必填（b01 里写入时取 `doc["id"]`），但这里仍做了防护——空串的 `document_id` 若进 map 会造出一个「孤儿文档」键。

**两张最终表都按 `*_FINAL_COLUMNS` 白名单裁列。**

```
out = {c: fields[c] for c in TEXT_UNITS_FINAL_COLUMNS}
out = {c: row.get(c) for c in DOCUMENTS_FINAL_COLUMNS}
```

**注意前者用 `fields[c]`（直接下标），后者用 `row.get(c)`。** 差异是刻意的：`fields` 是刚构造的字典，八个键全都显式写了，下标是安全的；而 `row` 来自流式读取，某些列可能不存在（例如早期索引没有 `creation_date`），用 `.get` 会得到 `None` 而不是 `KeyError`。

**`human_readable_id` 被重新编号。**

```
human_readable_id = 0
async for row in text_units_table:
    fields = {"id": row["id"], "human_readable_id": human_readable_id, ...}
    human_readable_id += 1
```

**`text_units` 表在 b01 阶段没有 `human_readable_id` 列**（写入时只有 `id` / `document_id` / `text` / `n_tokens`），所以这里是它第一次拿到短名。而 `create_final_documents` 不重新编号——文档的 `human_readable_id` 在 `load_input_documents` 时就有了（b01），这里靠 `row.get(c)` 原样带出。

**这两步在流水线里的位置暗示了一个顺序约束。**

`_standard_workflows` 的顺序是：

```
create_base_text_units → create_final_documents → extract_graph → finalize_graph
→ extract_covariates → create_communities → create_final_text_units
→ create_community_reports → generate_text_embeddings
```

**`create_final_documents` 排在 `extract_graph` 之前**，而它需要 `text_units.document_id`——那个字段在 `create_base_text_units` 时就有了。所以顺序成立。

**`create_final_text_units` 排在 `create_communities` 之后**，虽然它只需要实体与关系的 id（`extract_graph` 之后就有）。它被放在后面不是需求驱动，而是「最终产物放在最后两步统一生成」的编排习惯。

**`generate_text_embeddings` 在最后**，因为它要嵌的三张表（`text_units` / `entities` / `community_reports`）都必须已经定稿。**这是全套依赖里唯一的强约束**：嵌入必须最后做，否则嵌的是被后续步骤改写过的旧值。

## 关键取舍

**源码事实：** `EMBEDDING_FIELDS` 是「字段名 → `EmbeddingFieldConfig(name, table_name, embed_column, row_transform)`」的三条映射；实体的 `embed_column="title_description"` 是源表里不存在的派生列，由 `transform_entity_row_for_embedding` 现造（`f"{title}:{description}"`）。

**源码事实：** 要嵌哪些字段由 `config.embed_text.names` 决定（默认三个全嵌），但 `config.vector_store.index_schema` 必须为**全部**核心字段预先铺好 schema（a01 的 `_validate_vector_store` 负责），因为这里用 `index_schema[name]` 直接下标。

**源码事实：** 源表不存在时 `generate_text_embeddings` 记 warning 并跳过该字段，不抛错；打开源表时用 `truncate=False`（只读，不清空别人的表）。

**源码事实：** `embed_text` 的 `flush_size = batch_size * num_threads`，注释说明目的是「让每次 flush 产生足够多的 batch 以打满并发上限」；`text is None` 时替换成空串（而不是跳过），以维持 id 与向量的对齐。

**源码事实：** `_flush_embedding_buffer` 用 `zip(ids, vectors, strict=True)` 保证文本数与向量数一致；`vectors = result.embeddings or []` 的兜底使得「整体失败（`embeddings` 为 `None`）」会因长度不等而抛 `ValueError`，而「部分向量为 `None`」只是被 `skipped` 计数并告警。

**源码事实：** `skipped` 与告警都是**每批**统计，无跨批汇总；返回的 `total_rows` 是 `len(buffer)` 之和（含被跳过的行），因此日志「Embedded %d rows」会高估成功数。

**源码事实：** np 数组判定用 `type(x) is np.ndarray`（精确类型）而非 `isinstance`；可选的 `output_table` 把向量另存为 `{"id", "embedding"}` 行，仅当 `config.snapshots.embeddings` 为真。

**源码事实：** `generate_text_embeddings` 是唯一返回 `WorkflowFunctionOutput(result=None)` 的 workflow（产物在向量库，不在表里）。

**源码事实：** `_build_multi_ref_map` 依赖「表已用 transformer 把 `text_unit_ids` 解析成列表」这个前置条件，且只用 docstring 表达；若漏传 transformer，字符串会被逐字符迭代，产出「每字符一个 id」的垃圾倒排表且无任何告警。

**源码事实：** `create_final_text_units` 的读句柄与写句柄指向同一个 `text_units` 文件，其安全性依赖 CSVTable「写临时文件、close 时覆盖」的实现（代码注释已说明）。

**源码事实：** covariate 表用 `text_unit_id`（单数），实体/关系用 `text_unit_ids`（复数列表），因此两个倒排 map 的构建方式不同（单键直插 vs 遍历列表）；`nullcontext()` 用于统一「启用/未启用 claims」两种情况的 `async with` 结构。

**源码事实：** 两张最终表都按 `*_FINAL_COLUMNS` 白名单裁列，但 `create_final_text_units` 用 `fields[c]` 直接下标（键全显式写了），`create_final_documents` 用 `row.get(c)`（容忍列缺失）。

**源码事实：** `text_units` 的 `human_readable_id` 在 `create_final_text_units` 里首次按流式顺序编号；`documents` 的则在 `load_input_documents` 时已有，这里原样带出。

**架构推论：** 「嵌入字段可配置」与「schema 必须预先铺满」这对要求是矛盾的，解法是把矛盾前移到加载期（铺满 schema）而不是让消费点容错。这是对的选择，但代价是配置文件里出现一堆「这次不用」的 schema。**任何「可选字段 + 下游直接下标」的组合，都必须在某个早期阶段把所有可能的键补全。**

**架构推论：** 「缓冲到打满并发」这个公式（`batch_size * num_threads`）是把「批量 API」包成「流式接口」时必须显式处理的一层。若直接逐行调 API，延迟由往返时间决定；若只喂单批，并发闲置。**这层缓冲大小不是性能微调，而是正确利用并发的必要条件**——主核里做批量 WAL 刷盘、批量页预取时是同一个道理。

**架构推论：** 「部分失败可容忍、整体失败要炸」这个区分在这里被 `zip(strict=True)` 意外地实现了，而不是被显式设计。**这是一个应当被显式化的策略**：单条数据坏 → 跳过并记账；整批失败 → 中断。若依赖 `strict` 的副作用来表达，就没人能确定「空 embeddings」到底是否预期。

**架构推论：** 「前置条件只写在 docstring 里」是这类辅助函数的通病。`_build_multi_ref_map` 若漏传 transformer 会静默产出垃圾数据（每个字符一个 id），而这类错误在产物层面表现为「检索时实体匹配永远为空」。**代价极低的防护是加一行断言：`assert isinstance(row["text_unit_ids"], list)`。** 主核里凡是「依赖上游已做某种转换」的读取路径，都该有这个断言。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/index/workflows/generate_text_embeddings.py` | `EmbeddingFieldConfig` | 单字段嵌入配置：字段名、源表、源列、可选行变换 |
| `packages/graphrag/graphrag/index/workflows/generate_text_embeddings.py` | `EMBEDDING_FIELDS` | 三个字段的映射表（文本单元正文 / 实体 title_description / 报告全文） |
| `packages/graphrag/graphrag/index/workflows/generate_text_embeddings.py` | `generate_text_embeddings` | 按 `embed_text.names` 逐字段建索引、开表、嵌入；缺表则跳过 |
| `packages/graphrag/graphrag/index/operations/embed_text/embed_text.py` | `embed_text` | 流式缓冲（`batch_size × num_threads`）+ 分批 flush |
| `packages/graphrag/graphrag/index/operations/embed_text/embed_text.py` | `_flush_embedding_buffer` | `zip(strict=True)` 对齐、跳 None、`load_documents`、可选写表 |
| `packages/graphrag/graphrag/data_model/row_transformers.py` | `transform_entity_row_for_embedding` | 造 `title_description` 列（嵌入输入） |
| `packages/graphrag/graphrag/index/workflows/create_final_text_units.py` | `create_final_text_units` | 三个倒排 map → 文本单元三列 id 列表；首次编 `human_readable_id` |
| `packages/graphrag/graphrag/index/workflows/create_final_text_units.py` | `_build_multi_ref_map` | `text_unit_id → [row_id]` 倒排（依赖 transformer 已解析列表） |
| `packages/graphrag/graphrag/index/workflows/create_final_text_units.py` | `_build_covariate_map` | `text_unit_id → [covariate_id]` 倒排（单数字段，直接插入） |
| `packages/graphrag/graphrag/index/workflows/create_final_documents.py` | `create_final_documents` | `document_id → [text_unit_id]` 回填到文档行 |

## 参考

- [graphrag 源码：generate_text_embeddings.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/workflows/generate_text_embeddings.py)
- [graphrag 源码：create_final_text_units.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/workflows/create_final_text_units.py)
- [LanceDB 文档](https://lancedb.github.io/lancedb/)
- [graphrag 源码仓库](https://github.com/microsoft/graphrag)

## 与主核 / AI 的关系

嵌入与最终产物在主核（多模态数据库内核）里对应的是**二级索引构建**——都要扫一遍主数据、产出「主数据之外的派生结构」。四条经验。

**第一，派生结构的「可选性」与「下游直接下标」是一对矛盾，必须靠「提前铺满」调和。** 这里 `embed_text.names` 决定嵌哪几个，而 `index_schema[name]` 是直接下标，于是加载期必须为所有字段铺 schema。主核里建索引时同理：**「这个表可能有哪些索引」必须在某种元数据里先声明齐，运行期才能按下标取**；否则每处取用都得写 `.get` 加默认分支，而那会把「忘记声明」变成「静默用默认」。

**第二，批量接口要包成流式接口，中间那层缓冲大小是被并发度决定的，不是被内存决定的。** `flush_size = batch_size × num_threads`。主核里做批量刷盘、批量预取、批量 WAL 提交时同理：**若一批的大小不足以让并发通道全部排满，并发度形同虚设**。这个数不能凭直觉给，要从「下游并发上限」反推。

**第三，「读同一张表又写同一张表」必须依赖明确的写入语义。** 这里的安全完全来自 CSVTable「写临时文件、close 时覆盖」——换一个「就地截断」的实现，workflow 会吃掉自己的输入。主核里凡是「原地更新」的路径（页内更新、索引重建），都必须把「读的是快照还是活数据」这件事写清楚；**依赖实现细节的隐式安全，是最容易在优化中被破坏的那类正确性。**

**第四，「上游已完成某种转换」这个前置条件应当有断言，不能只有注释。** `_build_multi_ref_map` 若拿到未解析的字符串会逐字符迭代，产出静默垃圾。主核里凡是跨模块传递「已归一化的值」（已解码的元组、已排序的键、已对齐的向量）都必须能自证——**一行 `isinstance` 断言的成本，远低于查一个「检索结果永远为空」的怪异问题。**
