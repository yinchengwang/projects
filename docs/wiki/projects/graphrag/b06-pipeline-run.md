---
project: graphrag
feature: pipeline-run
title: 流水线执行与增量更新
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/index/run/run_pipeline.py
    symbols: [run_pipeline, _run_pipeline]
  - path: packages/graphrag/graphrag/index/workflows/factory.py
    symbols: [create_pipeline]
  - path: packages/graphrag/graphrag/index/workflows/update_entities_relationships.py
    symbols: [update_entities_relationships]
  - path: packages/graphrag/graphrag/index/workflows/update_text_units.py
    symbols: [update_text_units]
---

# 流水线执行与增量更新

## 一句话职责

`PipelineFactory.create_pipeline` 把「索引方法」翻译成一张**有序的工作流名列表**，`run_pipeline` 为这次运行准备好三套存储与表提供器（输出 / 缓存 / 状态）之后逐个 `await` 工作流并把结果 yield 出去；增量运行时，`update_entities_relationships` 与 `update_text_units` 负责把「旧索引」与「本次 delta」按名字对齐合并——**这一对函数是「增量 vs 全量」全部差别的所在。**

## 机制是什么

这套流水线的执行模型有三个决定性的简化，每一个都直接塑形了代码：

**第一，工作流之间没有显式依赖声明 —— 顺序就是依赖。** `Pipeline` 是一个 `(name, function)` 的有序列表，`pipeline.run()` 依次 await。谁依赖谁完全靠列表顺序保证。这让新增/重排工作流变成「改一个 Python 列表」，也让「漏排一步」变成运行期 `KeyError`（某张表还没被写出来）。

**第二，工作流之间不传内存值 —— 存储就是总线。** 每个 workflow 的签名都是 `(config, context)`，靠 `context.output_table_provider` 读写表。唯一的例外是 `context.state` 这个跨步骤的字典（用来传 DataFrame 与 id 映射）。**这个设计的收益是「可断点续跑」与「可观测」**（每一步的产物都落盘），代价是每个中间产物都要序列化一遍。

**第三，增量不是「重算变动的部分」，而是「重算全量到 delta 目录，再与旧索引按名字合并」。**

最后一点是这套系统的核心特征，值得先把它的机制说清：增量运行把**同样的标准工作流**（`create_base_text_units` → … → `generate_text_embeddings`）**完整跑一遍**，只是输出目录换成 `update_output/<时间戳>/delta/`。跑完之后，再跑八个 `update_*` 工作流，把 `delta/` 与 `previous/`（旧索引的备份）合并成新索引。

于是「增量」省的**不是计算量，而是「不需要重新读原始文档之外的所有东西」**——它仍然对新增文档做了完整抽取与嵌入。它真正省的是：不用重跑旧文档的抽取、不用重跑旧文档的嵌入。

## 怎么实现的

**`create_pipeline` 的全部逻辑就是「查表 + 组装」，而查表的键可能被配置覆盖。**

```
@classmethod
def create_pipeline(
    cls,
    config: GraphRagConfig,
    method: IndexingMethod | str = IndexingMethod.Standard,
) -> Pipeline:
    """Create a pipeline generator."""
    workflows = config.workflows or cls.pipelines.get(method, [])
    logger.info("Creating pipeline with workflows: %s", workflows)
    return Pipeline([(name, cls.workflows[name]) for name in workflows])
```

三处：

1. **`config.workflows or ...` 让配置永远赢。** `GraphRagConfig.workflows` 的字段注释写「always overrides any built-in workflow methods」。于是 `--method fast` 在配置显式给了 `workflows` 时无效——不报错、不警告（a02 讲过这个静默覆盖）。
2. **`cls.pipelines.get(method, [])` 的默认值是空列表。** 不认识的 method（例如拼错的字符串、或 a02 讲的 `"standard-update-update"`）会得到一个零工作流的 `Pipeline`，跑完什么都不做并打印「Indexing pipeline complete」。
3. **`cls.workflows[name]` 是直接下标** ——配置里写了一个未注册的工作流名会抛 `KeyError`。这是好事：拼错工作流名会立刻炸，而拼错 method 只会静默空跑。

**四张注册表全是「前缀相同、后缀不同」的组合。**

```
_standard_workflows = [
    "create_base_text_units", "create_final_documents", "extract_graph", "finalize_graph",
    "extract_covariates", "create_communities", "create_final_text_units",
    "create_community_reports", "generate_text_embeddings",
]
_fast_workflows = [
    "create_base_text_units", "create_final_documents", "extract_graph_nlp", "prune_graph",
    "finalize_graph", "create_communities", "create_final_text_units",
    "create_community_reports_text", "generate_text_embeddings",
]
_update_workflows = [
    "update_final_documents", "update_entities_relationships", "update_text_units",
    "update_covariates", "update_communities", "update_community_reports",
    "update_text_embeddings", "update_clean_state",
]
PipelineFactory.register_pipeline(IndexingMethod.Standard, ["load_input_documents", *_standard_workflows])
PipelineFactory.register_pipeline(IndexingMethod.Fast, ["load_input_documents", *_fast_workflows])
PipelineFactory.register_pipeline(IndexingMethod.StandardUpdate, ["load_update_documents", *_standard_workflows, *_update_workflows])
PipelineFactory.register_pipeline(IndexingMethod.FastUpdate, ["load_update_documents", *_fast_workflows, *_update_workflows])
```

**逐项对照 `_standard_workflows` 与 `_fast_workflows`，差异精确地是四处：**

| 位置 | 标准 | 快速 |
|---|---|---|
| 抽取 | `extract_graph`（LLM） | `extract_graph_nlp`（NLP）+ `prune_graph` |
| 报告 | `create_community_reports` | `create_community_reports_text` |
| claims | `extract_covariates` | 无 |

**快速路径多了 `prune_graph`** ——这很合理：NLP 抽取会产出大量低质量的名词短语共现，必须先剪掉（`PruneGraphDefaults` 的 `min_node_freq=2`、`min_edge_weight_pct=40.0`、`remove_ego_nodes=True`）才能得到可用的图。而标准路径的 LLM 抽取质量足够，不需要剪。

**而两条路径都没有「删除旧索引」这一步。** 全量运行的输出目录用的是 `truncate=True` 的默认表，每张表在 `open()` 时会被覆盖；但**没被本次写入的表会留下上一次的内容**。这就是为什么 `create_final_documents` 之类「每步都会写」的表没问题，而一个「上次跑过、这次被 `config.workflows` 裁掉」的表会残留——旧数据与新数据混在同一个索引里。

**`run_pipeline` 先建三套存储，再分叉到全量/增量两条路。**

```
input_storage = create_storage(config.input_storage)
output_storage = create_storage(config.output_storage)
output_table_provider = create_table_provider(config.table_provider, output_storage)
cache = create_cache(config.cache)

# load existing state in case any workflows are stateful
state_json = await output_storage.get("context.json")
state = json.loads(state_json) if state_json else {}

if additional_context:
    state.setdefault("additional_context", {}).update(additional_context)
```

**四处：**

1. **`state` 是从 `context.json` 读回来的。** 于是上一次运行留下的 state 会被这次继承——这是「断点续跑」与「增量」共用的机制。
2. **`additional_context` 是调用方注入的**（`build_index` 的 `additional_context` 参数），它被放到 `state["additional_context"]` 里供任意 workflow 读取。
3. **`state` 是原样的 json.loads，没有 schema。** 任何 workflow 往里写什么，下一次运行都能读到——一个无类型的跨运行共享字典。
4. **`context.json` 的写入在 `_run_pipeline` 的 `_dump_context_json` 里，且刻意排除了 `additional_context`**：

```
def _dump_context_json(context: PipelineRunContext) -> None:
    """Dump context state to storage."""
    # Dump context state, excluding additional_context
    temp_context = context.state.pop("additional_context", None)  # Remove reference only, as object size is uncertain
    try:
        state_blob = json.dumps(context.state, indent=4, ensure_ascii=False)
    finally:
        if temp_context:
            context.state["additional_context"] = temp_context

    await context.output_storage.set("context.json", state_blob)
```

**注释说明了理由：「as object size is uncertain」** ——`additional_context` 可能含不可序列化的对象（甚至整个 DataFrame），所以落盘前临时摘出去、`finally` 里再放回。

**但 `context.state` 里还有别的东西也会导致序列化失败**：增量路径会把整个 `merged_entities_df` 放进 `context.state["incremental_update_merged_entities"]`（见下），而那是 DataFrame——`json.dumps` 会抛 `TypeError`。**这意味着增量运行到 `_dump_context_json` 时可能直接崩**，除非那些 DataFrame 在 dump 之前被移走。这是一处没有防护的具体风险。

**增量路径的准备是「三套目录 + 备份旧索引」。**

```
if is_update_run:
    logger.info("Running incremental indexing.")

    update_storage = create_storage(config.update_output_storage)
    # we use this to store the new subset index, and will merge its content with the previous index
    update_timestamp = time.strftime("%Y%m%d-%H%M%S")
    timestamped_storage = update_storage.child(update_timestamp)
    delta_storage = timestamped_storage.child("delta")
    # Build table providers via child() so Cosmos providers use namespace
    # isolation while file/blob providers delegate to Storage.child().
    update_base_provider = create_table_provider(config.table_provider, update_storage)
    update_table_provider = update_base_provider.child(update_timestamp)
    delta_table_provider = update_table_provider.child("delta")
    # copy the previous output to a backup folder, so we can replace it with the update
    # we'll read from this later when we merge the old and new indexes
    previous_table_provider = update_table_provider.child("previous")

    await _copy_previous_output(output_table_provider, previous_table_provider)

    state["update_timestamp"] = update_timestamp
```

**这段目录布局值得画出来，因为它决定了合并时的读写方向：**

```
output/                          ← 旧索引（本次运行结束后会被新索引覆盖）
update_output/
  20260101-120000/              ← 本次运行的时间戳目录
    delta/                       ← 标准工作流写这里（只含新文档的产物）
    previous/                    ← 旧索引的完整拷贝
```

**`_copy_previous_output` 是「先全量复制旧索引」：**

```
async def _copy_previous_output(output_table_provider, previous_table_provider) -> None:
    """Copy all parquet tables from output to previous storage for backup."""
    for table_name in output_table_provider.list():
        table = await output_table_provider.read_dataframe(table_name)
        await previous_table_provider.write_dataframe(table_name, table)
```

**它逐表 `read_dataframe` 再 `write_dataframe`** ——把每一张表整个读进内存。对一份大索引（实体、关系、文本单元、社区报告、向量快照）这会是一次完整的内存复制。注释说「Copy all parquet tables」，但实际是按 `list()` 拿到的所有表，不限于 parquet。

**`child()` 逐级构建是必要的，注释解释了原因**：「Cosmos providers use namespace isolation while file/blob providers delegate to `Storage.child()`」——即 `child` 在两种后端上的语义不同（一个是命名空间前缀，一个是子目录），但对上层统一。

**`input_documents` 参数允许跳过文档加载，且用的是 `pipeline.remove`。**

```
# if the user passes in a df directly, write directly to storage so we can skip finding/parsing later
if input_documents is not None:
    await delta_table_provider.write_dataframe("documents", input_documents)
    pipeline.remove("load_update_documents")
```

（全量分支同样，只是 provider 与工作流名不同。）

**这里是「有依赖声明的流水线」与「无依赖声明的流水线」的差别所在**：因为 `Pipeline` 只是名字列表，删一步只能靠 `remove(name)`，不能靠「跳过不满足前置条件的工作流」。而 `remove` 一个不存在的名字会怎样，取决于 `Pipeline.remove` 的实现——如果它不校验，那么「配置里已经删掉了 `load_input_documents`」时这次 `remove` 会静默无效，若校验则抛错。

**`_run_pipeline` 是执行循环全部内容。**

```
async def _run_pipeline(pipeline, config, context) -> AsyncIterable[PipelineRunResult]:
    start_time = time.time()
    last_workflow = "<startup>"

    try:
        await _dump_stats_json(context)
        await _dump_context_json(context)

        logger.info("Executing pipeline...")
        for name, workflow_function in pipeline.run():
            last_workflow = name
            context.callbacks.workflow_start(name, None)

            with WorkflowProfiler() as profiler:
                result = await workflow_function(config, context)

            context.callbacks.workflow_end(name, result)
            yield PipelineRunResult(workflow=name, result=result.result, state=context.state, error=None)
            context.stats.workflows[name] = profiler.metrics
            await _dump_stats_json(context)
            if result.stop:
                logger.info("Halting pipeline at workflow request")
                break

        context.stats.total_runtime = time.time() - start_time
        logger.info("Indexing pipeline complete.")
        await _dump_stats_json(context)
        await _dump_context_json(context)

    except Exception as e:
        logger.exception("error running workflow %s", last_workflow)
        yield PipelineRunResult(workflow=last_workflow, result=None, state=context.state, error=e)
```

**七处关键点：**

**第一，`try` 包住整个循环，异常被转成「最后一个 workflow 的失败结果」。**

```
except Exception as e:
    yield PipelineRunResult(workflow=last_workflow, result=None, state=context.state, error=e)
```

**异常不会向上传播，而是被 yield 成一条带 `error` 的结果。** 于是调用方（`build_index`）要自己检查：

```
if output.error is not None:
    logger.error("Workflow %s completed with errors", output.workflow)
    workflow_callbacks.pipeline_error(output.error)
```

**而 `build_index` 只是记日志与调回调，不会让 `build_index` 本身抛错。** 于是「索引失败了」对调用方来说表现为「跑完了但某一步的 result 里带 error」。一个不检查 `error` 的调用方会以为成功。

**第二，`last_workflow` 的初值是 `"<startup>"`。** 于是「第一个 workflow 之前就失败」（例如 `_dump_stats_json` 抛错）被报告为 `<startup>` 失败。这是一个必要的小技巧：否则异常路径上 `last_workflow` 未绑定。

**第三，每一步之后立即落盘 stats。**

```
context.stats.workflows[name] = profiler.metrics
await _dump_stats_json(context)
```

**这是「可观测性优先」的体现**：进程若在中途被杀，`stats.json` 里保有已经完成的步骤的耗时。代价是**每个 workflow 一次全量 stats 写入**（`json.dumps(asdict(stats), indent=4)`），步骤多则写入次数多。

**第四，`result.stop` 允许工作流主动中止流水线。**

```
if result.stop:
    logger.info("Halting pipeline at workflow request")
    break
```

`WorkflowFunctionOutput` 有一个 `stop` 字段（默认为 `False`）。这是**唯一一种「提前结束但不报错」的机制** ——例如所有人都被判为重复文档时，继续跑抽取毫无意义。

**第五，`yield` 的时机在 `workflow_end` 回调之后、`stats` 落盘之前。**

于是消费这个 async generator 的代码（`build_index` 里的 `async for`）会在 stats 落盘**之前**拿到结果。这没什么问题（`build_index` 只记日志），但它意味着「看到第 N 条结果时，stats.json 里还没有第 N 步的耗时」。

**第六，`state=context.state` 被放进每条结果里。**

**这是把整个跨步骤状态字典的引用传给了调用方**，而 `context.state` 之后还会被继续修改——任何把 `result.state` 存起来的调用方最终拿到的是同一个（持久的）dict 的最终内容，而不是当时快照。

**第七，`_dump_context_json` 在开头与结尾各调一次，中间不调。** 于是进程中途被杀，`context.json` 只有上一次运行留下的内容——增量更新依赖的 `state["update_timestamp"]` 等键会丢失。这是「stats 每步落盘、context 只在首尾落盘」的不对称。

**增量更新的核心是那一对 merge 函数，而它们都靠「按名字对齐 + 拼接列表 + 覆盖派生量」。**

`update_entities_relationships` 的骨架：

```
old_entities = await DataReader(previous_table_provider).entities()
delta_entities = await DataReader(delta_table_provider).entities()

merged_entities_df, entity_id_mapping = _group_and_resolve_entities(old_entities, delta_entities)

old_relationships = await DataReader(previous_table_provider).relationships()
delta_relationships = await DataReader(delta_table_provider).relationships()
merged_relationships_df = _update_and_merge_relationships(old_relationships, delta_relationships)

merged_relationships_df = filter_orphan_relationships(merged_relationships_df, merged_entities_df)
```

注意顺序：**先合并实体（得到 id 映射），再合并关系，再用新实体表清孤儿边**。第三步是必需的——旧关系可能指向一个在合并后消失的实体名（理论上不该发生，但防一手）。

**然后它重新跑一次摘要，这是增量最贵的一步。**

```
summarization_model_config = config.get_completion_model_config(config.summarize_descriptions.completion_model_id)
prompts = config.summarize_descriptions.resolved_prompts()
model = create_completion(summarization_model_config, cache=cache.child("summarize_descriptions"), cache_key_creator=cache_key_creator)

merged_entities_df, merged_relationships_df = await get_summarized_entities_relationships(
    extracted_entities=merged_entities_df,
    extracted_relationships=merged_relationships_df,
    ...
    num_threads=config.concurrent_requests,
)
```

**题外但重要：这里的 `cache.child("summarize_descriptions")` 用的是硬编码字符串**，而 `extract_graph` 里用的是 `config.summarize_descriptions.model_instance_name`。若用户改了 `model_instance_name`，增量路径与全量路径会命中不同的缓存命名空间——增量会重新算一遍摘要（缓存失效），成本翻倍但不报错。

**`_group_and_resolve_entities` 是「找同名 → 记映射 → 拼接 → 按 title 聚合」。**

```
merged = delta_entities_df[["id", "title"]].merge(
    old_entities_df[["id", "title"]],
    on="title",
    suffixes=("_B", "_A"),
)
id_mapping = dict(zip(merged["id_B"], merged["id_A"], strict=True))

initial_id = old_entities_df["human_readable_id"].max() + 1
delta_entities_df["human_readable_id"] = np.arange(initial_id, initial_id + len(delta_entities_df))

combined = pd.concat([old_entities_df, delta_entities_df], ignore_index=True)

aggregated = (
    combined
    .groupby("title")
    .agg({
        "id": "first",
        "type": "first",
        "human_readable_id": "first",
        "description": lambda x: list(x.astype(str)),
        "text_unit_ids": lambda x: list(itertools.chain(*x.tolist())),
        "degree": "first",  # todo: we could probably re-compute this with the entire new graph
    })
    .reset_index()
)

aggregated["frequency"] = aggregated["text_unit_ids"].apply(len)
```

**五处：**

**第一，`id_mapping` 是 `{新 id: 旧 id}`，用于把 delta 里对实体的引用改指到旧 id。** 这是「保持旧索引里实体 id 稳定」的关键动作：新增文档里出现的「同一个实体」会继承旧实体的 id，于是旧的文本单元的 `entity_ids` 不用改。

**第二，`concat` 的顺序是 `[old, delta]`，且 `id` / `type` / `human_readable_id` / `degree` 都取 `"first"`。** 于是**旧记录赢**：合并后的实体保留旧 id、旧 type、旧短名、旧度数。只有 `description` 与 `text_unit_ids` 是拼接的。

**第三，`degree: "first"` 上有一句明确的 TODO：**

```
"degree": "first",  # todo: we could probably re-compute this with the entire new graph
```

**这是这套增量实现里最重要的一处已知缺陷，而作者自己标出来了。** 合并后实体的 `degree` 仍是旧值，即使新文档给它加了新边。于是**度数只在「第一次全量索引」时准确**；多轮增量之后，`degree` 会系统性地偏小。而 `degree` 是社区报告提示词里判断实体重要度的唯一依据（b04 的 `_prep_nodes`）——**增量索引跑得越多，报告里挑出的「重要实体」就越偏向旧文档**。

**第四，`text_unit_ids` 用 `itertools.chain(*x.tolist())` 展平。**

这里的 `x` 是一列 Series，每个元素本身是一个列表（或 numpy 数组）。`x.tolist()` 把 Series 变成「列表的列表」，`chain(*...)` 把它们串成一个列表。**没有去重** ——若同一文本单元在两个来源里都提到这个实体，它会重复出现，进而 `frequency` 被高估。

**第五，`frequency` 是事后从 `text_unit_ids` 长度重算的**，所以它也继承了上面那个重复问题。注释「recompute frequency to include new text units」说明意图是「把新文本单元算进去」。

**`_update_and_merge_relationships` 与实体版几乎同构，但两处聚合函数不同。**

```
aggregated = (
    merged_relationships
    .groupby(["source", "target"])
    .agg({
        "id": "first",
        "human_readable_id": "first",
        "description": lambda x: list(x.astype(str)),
        "text_unit_ids": lambda x: list(itertools.chain(*x.tolist())),
        "weight": "mean",
        "combined_degree": "sum",
    })
    .reset_index()
)
```

**`weight` 用 `mean`（平均值），而全量路径的 `_merge_relationships` 用 `sum`（求和）。**

**这是一处真实的语义不一致**：全量索引里，一条边在 N 个块中出现 → 权重是 N 块打分的**和**；增量合并时，旧边的权重与新边的权重取**平均**。于是同样的两段文本，若分两次索引会得到平均分，一次索引会得到和——**索引结果的权重取决于「分几次跑」，而不是数据本身**。

**而紧接着它又把 `combined_degree` 重算了一遍，且这次是全对的：**

```
final_relationships["source_degree"] = final_relationships.groupby("source")["target"].transform("count")
final_relationships["target_degree"] = final_relationships.groupby("target")["source"].transform("count")
final_relationships["combined_degree"] = final_relationships["source_degree"] + final_relationships["target_degree"]
```

**注意这与实体的 `degree` 形成鲜明对比**：关系的 `combined_degree` 在增量路径里被**重新计算**（从合并后的关系表数每个 source / target 出现几次），而实体的 `degree` 直接取旧的（`"first"`）。

**于是同一份增量索引里，边的两端度数之和是新的、节点自己的度数是旧的**——而这两者本应一致：`combined_degree` 的定义就是「两端 degree 之和」（b03）。合并之后这个恒等式不成立了。

而 `source_degree` / `target_degree` 是**有向**计数（`groupby("source")` 只数出边、`groupby("target")` 只数入边），而 b03 的 `_build_degree_map` 算的是**无向**度数。于是又一处不一致：`combined_degree` 从「无向和无向」变成「出度 + 入度」。

**`update_text_units` 比实体/关系简单得多，因为它只需要改引用。**

```
def _update_and_merge_text_units(old_text_units, delta_text_units, entity_id_mapping) -> pd.DataFrame:
    # Look for entity ids in entity_ids and replace them with the corresponding id in the mapping
    if entity_id_mapping:
        delta_text_units["entity_ids"] = delta_text_units["entity_ids"].apply(
            lambda x: [entity_id_mapping.get(i, i) for i in x] if x is not None else x
        )

    initial_id = old_text_units["human_readable_id"].max() + 1
    delta_text_units["human_readable_id"] = np.arange(initial_id, initial_id + len(delta_text_units))
    # Merge the final text units
    return pd.concat([old_text_units, delta_text_units], ignore_index=True)
```

**三处：**

1. **`entity_id_mapping.get(i, i)` 把 delta 里指向新实体的 id 换成旧实体 id** ——这正是 `_group_and_resolve_entities` 产出映射的用途。`get(i, i)` 的默认值是 `i`（映射里没有就保持原样），于是「delta 里独有、旧索引里没有」的实体保持自己的 id。
2. **`human_readable_id` 从旧的 max 之后续编** ——与实体/关系同样的模式。
3. **`pd.concat` 直接拼，不去重。** 文本单元的 id 是 `sha512(text)`（b01），所以**同一段文本若在旧文档与新文档里都出现，会得到同一个 id，但在这里变成两行**。全量路径靠「表写入按 id 覆盖」自然去重，增量路径的 `concat` 没有这个效果。

**这是增量路径的一个结构性缺陷**：`text_units` 表会出现重复 id 的行，而后面的 `create_final_text_units`（在 `_update_workflows` 之前已经跑过）已经给它编好了 `human_readable_id`——现在 delta 的新行带着新的短名混进来，同一个 `id` 有两个短名。任何按 `id` 做 join 的地方（检索、过滤）都会拿到重复行。

**`update_text_units` 还把合并结果放进 state。**

```
context.state["incremental_update_merged_text_units"] = merged_text_units
```

**与 `update_entities_relationships` 放进 state 的两个 DataFrame 一样**，这是为了让后续的 `update_*` 工作流（`update_communities`、`update_community_reports`）不必重新从存储读。

**但这就是前面提到的 `_dump_context_json` 风险**：`context.state` 里现在有 DataFrame，而 `json.dumps` 处理不了。

**八个 `update_*` 工作流的顺序也值得看一眼。**

```
"update_final_documents", "update_entities_relationships", "update_text_units",
"update_covariates", "update_communities", "update_community_reports",
"update_text_embeddings", "update_clean_state"
```

**`update_entities_relationships` 在 `update_text_units` 之前**，因为后者需要前者的 `incremental_update_entity_id_mapping`。**`update_communities` 在 `update_text_units` 之后**，因为它需要合并后的文本单元（社区的 `text_unit_ids` 要含新块的）。**`update_clean_state` 在最后**，负责清掉 state 里的中间 DataFrame（包括那些无法序列化的）。

**于是 `_dump_context_json` 的风险其实被 `update_clean_state` 化解了——只要它真的清了那些键。** 而「状态清理作为流水线的最后一步」这个设计本身说明：**作者知道 state 里会有不可序列化的东西，所以把它当成一个必须显式收尾的临时区域。**

## 关键取舍

**源码事实：** `Pipeline` 是一个有序的 `(name, function)` 列表，工作流之间无依赖声明，顺序即依赖；跨步骤只通过存储（表）与 `context.state`（字典）通信。

**源码事实：** `create_pipeline` 用 `config.workflows or cls.pipelines.get(method, [])`——配置总是覆盖内建方法；未知 method 回退为空列表（生成零工作流的管道，静默空跑）；配置里的未知工作流名会因 `cls.workflows[name]` 直接下标而抛 `KeyError`。

**源码事实：** 标准与快速路径的差异是：`extract_graph` vs `extract_graph_nlp` + `prune_graph`；`create_community_reports` vs `create_community_reports_text`；标准有 `extract_covariates` 而快速没有。快速路径多出的 `prune_graph` 用于剪掉 NLP 共现图里的低质部分。

**源码事实：** 增量运行的目录布局是 `update_output/<时间戳>/{delta,previous}`：`delta` 供标准工作流写新文档产物，`previous` 由 `_copy_previous_output` 从旧输出**逐表整表读进内存再写**得到。

**源码事实：** `input_documents` 参数让调用方直接提供文档表，此时用 `pipeline.remove("load_input_documents")`（增量则为 `remove("load_update_documents")`）把加载步骤从列表里删掉。

**源码事实：** `_run_pipeline` 用一个大 `try` 包住循环，异常被 yield 成一条 `PipelineRunResult(workflow=last_workflow, error=e)` 而不向上传播；`build_index` 只记日志并调 `pipeline_error` 回调，自身不抛错。

**源码事实：** 每完成一个 workflow 就把 `stats.json` 全量重写一次；`context.json` 只在流水线首尾各写一次，且落盘时临时摘出 `additional_context`（注释：object size is uncertain）。

**源码事实：** `result.stop` 是唯一「提前结束但不报错」的机制；`yield` 发生在 `workflow_end` 回调之后、`stats` 落盘之前，且 `state=context.state` 传的是引用。

**源码事实：** `_group_and_resolve_entities` 在旧/新实体间按 `title` 建 `{新 id: 旧 id}` 映射；concat 顺序 `[old, delta]` 且 `id`/`type`/`human_readable_id`/`degree` 均取 `"first"`（旧记录赢），只有 `description` 与 `text_unit_ids` 拼接；`frequency` 事后由 `text_unit_ids` 长度重算且不去重。

**源码事实：** 合并后实体的 `degree` 保留旧值，代码里有一句 `# todo: we could probably re-compute this with the entire new graph`；而关系的 `combined_degree` 在增量路径里被**重新计算**为 `source_degree + target_degree`（有向出度 + 入度），与全量路径的无向度数定义不一致，破坏了「combined_degree = 两端 degree 之和」的恒等式。

**源码事实：** 关系的 `weight` 在增量合并里取 `"mean"`，而全量合并（`_merge_relationships`）取 `"sum"`——同一份数据分几次索引会得到不同的权重。

**源码事实：** `update_text_units` 用 `entity_id_mapping.get(i, i)` 改指 delta 的实体引用，`human_readable_id` 续编，然后 `pd.concat` **不去重**——同一个 `sha512(text)` id 可能以多行出现且带不同短名。

**源码事实：** 三个 `update_*` 工作流把 DataFrame 放进 `context.state`，而 `_dump_context_json` 用 `json.dumps` 无法序列化它们；`update_clean_state` 作为流水线最后一步负责清理这些键。

**架构推论：** 「顺序即依赖」让流水线极易读、极易改，但没有机制在**源码层面**保证「某步读取的表已被前一步写出」。这个保证完全由测试与运行期 `KeyError` 承担。主核里若有类似的「按阶段脚本执行」的构建流程（迁移、批处理、DDL 序列），同样的取舍成立；**要提升安全性，最低成本的做法是每个 workflow 声明「读哪些表、写哪些表」，让工厂在组装期校验顺序**。

**架构推论：** 增量更新的收益边界很清晰：**它省的是「旧文档的重算」，不是「全流水线的重算」**。旧索引被整表复制、实体关系被重新摘要、社区被重新划分、嵌入被重新生成（`update_text_embeddings`）——只有抽取与初次嵌入被跳过。判断一个增量方案是否值得，要看「可跳过的部分」在总成本里的占比；这里大头（抽取）被跳过，所以值得。

**架构推论：** 「`degree` 保留旧值 + `combined_degree` 重算」这对组合是增量实现里最需要警惕的模式：**同一个不变量的一半被更新而另一半没有。** 它不会立刻出错（度数只是偏小），但会让「越跑越偏」成为系统性行为——而这类偏差在单次运行里看不出来，只有对同一份数据做「一次全量 vs 多次增量」对比才暴露。**凡是增量路径，都必须把「哪些派生量重算、哪些继承」列成清单并与全量路径逐项对照。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/index/workflows/factory.py` | `PipelineFactory.register_pipeline` / `register` | 四张注册表：方法 → 工作流名列表、名字 → 工作流函数 |
| `packages/graphrag/graphrag/index/workflows/factory.py` | `PipelineFactory.create_pipeline` | 组装有序 `Pipeline`；`config.workflows` 优先，未知 method 回退空列表 |
| `packages/graphrag/graphrag/index/workflows/factory.py` | `_standard_workflows` / `_fast_workflows` / `_update_workflows` | 三条工作流序列的字面量定义 |
| `packages/graphrag/graphrag/index/run/run_pipeline.py` | `run_pipeline` | 建输入/输出存储、表提供器、缓存；读回 `context.json`；分叉全量/增量 |
| `packages/graphrag/graphrag/index/run/run_pipeline.py` | `_run_pipeline` | 执行循环：逐工作流 await、写 stats、yield 结果、异常转结果 |
| `packages/graphrag/graphrag/index/run/run_pipeline.py` | `_copy_previous_output` | 把旧索引逐表整表复制到 `previous/` |
| `packages/graphrag/graphrag/index/run/run_pipeline.py` | `_dump_stats_json` / `_dump_context_json` | 每步写 stats；首尾写 context（落盘时摘出 `additional_context`） |
| `packages/graphrag/graphrag/index/workflows/update_entities_relationships.py` | `update_entities_relationships` | 合并实体关系 → 清孤儿边 → 重新摘要 → 写回；产出 `entity_id_mapping` 进 state |
| `packages/graphrag/graphrag/index/update/entities.py` | `_group_and_resolve_entities` | 按 `title` 建 `{新 id: 旧 id}` 映射；`[old, delta]` concat 后聚合（`degree` 取旧值） |
| `packages/graphrag/graphrag/index/update/relationships.py` | `_update_and_merge_relationships` | 按 `(source, target)` 聚合；`weight` 取 `mean`；重算有向 `combined_degree` |
| `packages/graphrag/graphrag/index/workflows/update_text_units.py` | `update_text_units` / `_update_and_merge_text_units` | 用 id 映射改指实体引用、续编短名、`concat` 不去重 |

## 参考

- [graphrag 源码：run_pipeline.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/run/run_pipeline.py)
- [graphrag 源码：workflows/factory.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/workflows/factory.py)
- [graphrag 文档：增量索引](https://microsoft.github.io/graphrag/index/methods/)
- [graphrag 源码仓库](https://github.com/microsoft/graphrag)

## 与主核 / AI 的关系

流水线执行与增量更新在主核（多模态数据库内核）里对应的是**批处理作业编排与增量检查点**——都是「一串有序阶段 + 每阶段落盘 + 部分重跑」。四条经验。

**第一，「顺序即依赖」在小规模流水线里是净收益，在需要重构时会变成负债。** 这里新增一步只要改一个列表，但没有机制保证「读的表已被写出」。主核里做多阶段构建（DDL 序列、迁移脚本、离线索引重建）同理。**低成本加固方式：每个阶段声明读写表集合，编排器在启动时按前向顺序推演一遍「此刻已存在哪些表」，发现读空表就报错。** 这比等运行到第 7 步才 `KeyError` 便宜得多，也比给每个阶段写显式依赖图轻。

**第二，增量方案的价值必须用「可跳过部分在总成本里的占比」衡量，不能只看「省了重算」。** 这里增量仍然复制整份旧索引、重新摘要全部实体关系、重新划分社区、重新生成全部嵌入——只有抽取与初次嵌入被跳过。抽取是 LLM 调用，通常占大头，所以值得；若换成「嵌入是瓶颈」的配置，同样的增量方案就几乎没有收益。**判断准则：把成本按阶段拆开，看增量能裁掉哪几段，再对比这几段在总耗时里的比例。**

**第三，派生量必须有一份「重算 vs 继承」的清单，且增量与全量要逐项对照。** 这里 `degree` 继承旧值、`combined_degree` 重算，两者本应满足的恒等式被打破——而这个破坏在单次运行里完全不可见。主核里做增量统计（表行数、索引基数的估算值、可见性映射）时同理：**每一个派生量都要能回答「它在增量路径上是被重算、被增量修正、还是被继承」，三者混用且无序，就会产出「越跑越偏」的统计，而查询计划会依据这些统计做出越来越差的选择。**

**第四，跨阶段共享的可变状态是最容易藏隐患的地方。** `context.state` 里既能放标量也能放 DataFrame，既跨步骤又跨运行（从 `context.json` 读回），而序列化能力只有 `json.dumps` 的水平——于是必须有一个专门的「清理状态」步骤收尾。主核里凡是「跨阶段传句柄/缓存/中间结果」的设计，都应当明确三件事：**它的生命周期（单次运行 / 跨运行）、它的可序列化性（能否落盘续跑）、以及谁负责清理。** 缺任何一项，都会演变成「某些键在特定路径上残留、下次运行读到脏值」这类极难复现的问题。
