---
project: graphrag
feature: graph-extraction
title: 实体关系抽取工作流
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/index/workflows/extract_graph.py
    symbols: [extract_graph, get_summarized_entities_relationships]
  - path: packages/graphrag/graphrag/index/workflows/extract_graph_nlp.py
    symbols: [extract_graph_nlp]
  - path: packages/graphrag/graphrag/prompts/index/extract_graph.py
    symbols: [GRAPH_EXTRACTION_PROMPT, CONTINUE_PROMPT, LOOP_PROMPT]
---

# 实体关系抽取工作流

## 一句话职责

从每个文本单元里用 LLM 抽出「实体 + 关系」，把跨文本单元的同名实体合并成一条（描述聚成列表、`frequency` 数次数），再用第二次 LLM 调用把那一堆描述**摘要成一段**；NLP 路径（`extract_graph_nlp`）用名词短语共现替代这两次 LLM 调用，产出同一形状的三列表。

## 机制是什么

这一步是 graphrag 全部成本与全部质量的所在。它要解决的核心矛盾是：**抽取必须以小块为单位（受上下文限制），但图必须以全局为单位（同名实体要合一）。**

于是流程被切成两段，中间有一次「按名字合并」：

1. **逐块抽取**：每个文本单元一次（或多次，见 gleaning）LLM 调用，产出**局部图**。这一段的输出里，同一个实体在 N 个块里出现就是 N 条记录，每条带自己的描述与 `source_id`。
2. **按名字合并**：`groupby(["title", "type"])` 之后，`description` 聚成列表、`text_unit_ids` 聚成列表、`frequency` 数出现次数。**这一步决定了「实体」这个概念在系统里的粒度**——同名即同实体，没有消歧。
3. **描述摘要**：把那条聚合出来的描述列表喂给 LLM，压成一段 `max_summary_length` 以内的文字。**这是必须的**：一份出现在 50 个块里的实体会有 50 段描述，直接拼进提示词会超限且冗余。

这个三段结构解释了两件容易被忽略的事：为什么 `frequency` 是一个字段（合并的副产品，同时是重要度信号），以及为什么抽取要用两个不同的模型配置（`extract_graph.completion_model_id` 与 `summarize_descriptions.completion_model_id`）——两次调用的性质完全不同，前者要发散（尽量抽全），后者要收敛（去重压短）。

## 怎么实现的

**`extract_graph` 的工作流外壳负责建两个模型，然后把所有参数摊给操作层。**

```
extraction_model = create_completion(
    extraction_model_config,
    cache=context.cache.child(config.extract_graph.model_instance_name),
    cache_key_creator=cache_key_creator,
)
...
summarization_model = create_completion(
    summarization_model_config,
    cache=context.cache.child(config.summarize_descriptions.model_instance_name),
    cache_key_creator=cache_key_creator,
)
```

三处细节：

1. **两个模型各有自己的 cache 子命名空间**（`extract_graph` / `summarize_descriptions` 的 `model_instance_name`）。这是必要的：两次调用的 prompt 完全不同，混在一个缓存命名空间里不会有正确性问题，但会让「清掉抽取缓存」变成不可能。
2. **`config.concurrent_requests` 被传了两次**——`extraction_num_threads` 与 `summarization_num_threads` 都取它。**两条阶段共用同一个并发度**（a01 讲过这个字段在顶层），无法分别调。
3. **`config.async_mode` 只传给抽取，不传给摘要。** 摘要那条路径用的是 `asyncio.Semaphore`（见下），与 `AsyncType` 无关——这是一个真实的不对称。

**抽取阶段的核心是「每行一次，全部并发」。**

```
async def extract_graph(text_units, callbacks, text_column, id_column, model, prompt,
                       entity_types, max_gleanings, num_threads, async_type):
    num_started = 0

    async def run_strategy(row):
        nonlocal num_started
        text = row[text_column]
        id = row[id_column]
        result = await _run_extract_graph(...)
        num_started += 1
        return result

    results = await derive_from_rows(
        text_units, run_strategy, callbacks,
        num_threads=num_threads, async_type=async_type,
        progress_msg="extract graph progress: ",
    )
```

`num_started` 这个计数器被 `nonlocal` 递增，但**从未被读取**——它是一处死代码（大概曾用于进度显示，后来 `derive_from_rows` 自己的 progress 接管了）。

**合并是两个 groupby，而它们用不同的聚合函数，这个差别是语义性的。**

```
def _merge_entities(entity_dfs) -> pd.DataFrame:
    all_entities = pd.concat(entity_dfs, ignore_index=True)
    return (
        all_entities
        .groupby(["title", "type"], sort=False)
        .agg(
            description=("description", list),
            text_unit_ids=("source_id", list),
            frequency=("source_id", "count"),
        )
        .reset_index()
    )

def _merge_relationships(relationship_dfs) -> pd.DataFrame:
    all_relationships = pd.concat(relationship_dfs, ignore_index=False)
    return (
        all_relationships
        .groupby(["source", "target"], sort=False)
        .agg(
            description=("description", list),
            text_unit_ids=("source_id", list),
            weight=("weight", "sum"),
        )
        .reset_index()
    )
```

**四处值得逐个看：**

**第一，实体的分组键是 `["title", "type"]`，关系的分组键是 `["source", "target"]`。**

实体带 `type` 参与分组，意味着**同名不同类型的实体不会合并**。例如「Apple」（organization）与「Apple」（person）会变成两条。这是「用类型兜住歧义」的做法，但它与关系不一致——关系只按两端名字分组，不看任何类型。于是可能出现「A(org)→B 与 A(person)→B 被合并成一条」，而两端实体却是分开的两条。**这是一个真实的、未加防护的不一致。**

**第二，`frequency=("source_id", "count")` 数的是「在多少个文本单元里出现过」。** 因为 `source_id` 就是文本单元 id，而每块抽取一次，所以 count 就是出现次数。这与 `text_unit_ids` 是同一份数据（后者是 list，前者是它的长度）——冗余，但省一次 `len()`。

**第三，关系的 `weight=("weight", "sum")` 是求和。** 每块抽取时 LLM 给出一个 1-10 的强度分（`relationship_strength`，见提示词），合并时把这些分相加。于是「在两块里各 5 分」的关系权重要高于「在一块里 9 分」的关系。**这是一个刻意的选择：出现次数与强度被压进同一个数字。** 后果是权重不再有绝对含义（无法说「7 分以上是强关系」），只能相对比较。

**第四，`sort=False` 保留首次出现顺序。** 两处都用了它，于是输出顺序取决于 `concat` 的顺序（即文本单元的读入顺序）——这配合 c01 讲的「排序 + 固定 seed」保证可复现。

**`filter_orphan_relationships` 在抽取之后立刻跑一次。**

```
relationships = filter_orphan_relationships(relationships, entities)
```

**LLM 完全可能抽出一条两端里有一端没被抽成实体的关系**——它在描述里提到了 A 和 B 的关系，但只在实体列表里给了 A。这条边若不删，后面 `finalize_graph` 的度数计算会给出 `degree_map.get(title, 0)` 的 0，而社区划分（基于关系构建图）会因为找不到节点而报错。

**这里是「用过滤而不是用补实体」来解决**：不存在的端点意味着抽取不完整，补一个空壳实体会污染图（它没有描述、没有 `text_unit_ids`，但会被算进社区与报告）。删边更保守。

**`extract_graph` 对空结果的处理是「抛错」，而且分两次。**

```
if len(extracted_entities) == 0:
    raise ValueError("Graph Extraction failed. No entities detected during extraction.")
if len(extracted_relationships) == 0:
    raise ValueError("Graph Extraction failed. No relationships detected during extraction.")
```

**注意这两条都在摘要之前**——也就是说摘要不会在空图上白跑。而「有实体但零关系」也算失败：graphrag 的后续步骤（社区划分）需要一个真正的图，只有节点的图无法聚类。

**`get_summarized_entities_relationships` 的关键在 drop 再 merge。**

```
entity_summaries, relationship_summaries = await summarize_descriptions(...)

relationships = extracted_relationships.drop(columns=["description"]).merge(
    relationship_summaries, on=["source", "target"], how="left"
)

extracted_entities = extracted_entities.drop(columns=["description"])
entities = extracted_entities.merge(entity_summaries, on="title", how="left")
return entities, relationships
```

**先把原来那个 list 形式的 `description` 列 drop 掉，再 merge 回摘要结果。** 为什么不直接覆盖？因为 `drop` 与 `merge` 的组合让「列的顺序」变成「原列（除 description）+ 新 description 追加在末尾」——而 `merge(..., how="left")` 保证行数与顺序不变。

三处细节：

1. **实体 merge 的键是 `["title"]`，关系是 `["source", "target"]`** —— 与前面 groupby 的键一致，除了实体这里**不含 `type`**。这是又一处不一致：合并时按 `(title, type)` 分组，摘要回填时按 `title` 匹配。若同一 title 有两种 type，两行都会拿到同一个摘要。

2. **`how="left"` 而不是内连接。** 摘要失败（某行 LLM 出错返回空）时，那行会保留并有 `NaN` 描述，而不是整行消失。

3. **`raw_` 副本在摘要之前就拷好了**：

```
raw_entities = extracted_entities.copy()
raw_relationships = extracted_relationships.copy()
```

**这是在 `drop(columns=["description"])` 之前拍的快照**，于是 `raw_*` 里保有原始的描述列表。这两个表只在 `config.snapshots.raw_graph` 为真时写出（默认 `False`）——用于调试「摘要到底把什么压成了什么」。

**`extract_graph_nlp` 走完全不同的路，但产出同一形状。**

```
async def extract_graph_nlp(text_units_table, cache, entities_table, relationships_table,
                           text_analyzer, normalize_edge_weights) -> dict[str, list[dict]]:
    extracted_nodes, extracted_edges = await build_noun_graph(
        text_units_table,
        text_analyzer=text_analyzer,
        normalize_edge_weights=normalize_edge_weights,
        cache=cache,
    )

    if len(extracted_nodes) == 0:
        raise ValueError("NLP Graph Extraction failed. No entities detected during extraction.")
    if len(extracted_edges) == 0:
        logger.error("NLP Graph Extraction failed. No relationships detected during extraction.")
    ...
```

**这里有一处与标准路径明显不同的处理：零关系时只 `logger.error` 而不 `raise`。**

对照标准路径的同名检查——那里是 `raise ValueError(...)`。NLP 路径零关系**继续执行**，只是打一条 error 日志。

这个差别有实际后果：NLP 路径会写出「只有实体、没有关系」的表，然后 `prune_graph`（`_fast_workflows` 里紧接着的一步）与 `create_communities` 会在一个无边图上跑——聚类退化成每个节点一个社区，或直接空结果。**这是一条比标准路径更宽松、更容易产出无意义索引的路径**，而它只在日志里留痕。

**两边写表时都把字段集写死，且值是常量。**

```
# NLP 路径的实体
row = {
    "title": row_tuple.title,
    "frequency": row_tuple.frequency,
    "text_unit_ids": row_tuple.text_unit_ids,
    "type": "NOUN PHRASE",
    "description": "",
}

# NLP 路径的关系
row = {
    "source": row_tuple.source,
    "target": row_tuple.target,
    "weight": row_tuple.weight,
    "text_unit_ids": row_tuple.text_unit_ids,
    "description": "",
}
```

**`type` 是常量 `"NOUN PHRASE"`，`description` 是空字符串。**

这两处合起来说明 NLP 路径的产物「形状对、内容缺」：
- 没有类型信息（名词短语抽取器能识别 NER 标签，但这里没有把标签写进 `type`）。
- 没有描述，于是后续的 `summarize_descriptions` 没有输入、社区报告里的实体描述是空的。

**这也是 `Fast` 索引与 `Standard` 的真正差别**：`Fast` 省掉的不是「摘要」而是「质量」。枚举注释说「NLP for graph construction and language model for summarization」，但代码里 NLP 路径的实体 `description` 是空串，所以报告阶段的摘要实际拿不到实体描述——它只能靠标题与关系结构。

**`normalize_edge_weights` 是一个布尔开关，控制是否用 PMI 重算权重。**

```
if normalize_edge_weights and not edges_df.empty:
    edges_df = calculate_pmi_edge_weights(nodes_df, edges_df)
```

默认 `True`（`ExtractGraphNLPDefaults`）。这一步把「共现次数」换成 PMI（b03 详述），目的是**惩罚高频词的假关系**——两个都出现几千次的词共现不说明它们有关系。标准路径没有这一步，因为 LLM 给的权重已经是语义判断。

**`GRAPH_EXTRACTION_PROMPT` 是一个「用分隔符输出结构化数据」的提示。**

```
GRAPH_EXTRACTION_PROMPT = """
-Goal-
Given a text document that is potentially relevant to this activity and a list of entity types, identify all entities of those types from the text and all relationships among the identified entities.

-Steps-
1. Identify all entities. For each identified entity, extract the following information:
- entity_name: Name of the entity, capitalized
- entity_type: One of the following types: [{entity_types}]
- entity_description: Comprehensive description of the entity's attributes and activities
Format each entity as ("entity"<|><entity_name><|><entity_type><|><entity_description>)

2. From the entities identified in step 1, identify all pairs of (source_entity, target_entity) that are *clearly related* to each other.
For each pair of related entities, extract the following information:
- source_entity: name of the source entity, as identified in step 1
- target_entity: name of the target entity, as identified in step 1
- relationship_description: explanation as to why you think the source entity and the target entity are related to each other
- relationship_strength: a numeric score indicating strength of the relationship between the source entity and target entity
 Format each relationship as ("relationship"<|><source_entity><|><target_entity><|><relationship_description><|><relationship_strength>)

3. Return output in English as a single list of all the entities and relationships identified in steps 1 and 2. Use **##** as the list delimiter.

4. When finished, output <|COMPLETE|>
...
Entity_types: {entity_types}
Text: {input_text}
######################
Output:"""
```

四层设计值得拆：

**第一，用两个自造分隔符而不是 JSON。** `TUPLE_DELIMITER = "<|>"`、`RECORD_DELIMITER = "##"`、`COMPLETION_DELIMITER = "<|COMPLETE|>"`。这三个常量在 `graph_extractor.py` 里定义，与提示词里的文字一一对应。

**选分隔符而不是 JSON 的理由是解析容错**：LLM 输出 JSON 时常见的失败是漏引号、多逗号、加 markdown 围栏——每一样都要写修复逻辑。而「按 `##` 切记录、按 `<|>` 切字段」只要模型照格式写就能解析，且解析代码只有十几行。代价是**模型偶尔不照格式写**（例如用 `|` 或 `,##`），那样整块会被静默丢弃。

**第二，示例（`-Examples-`）占了提示词的绝大部分篇幅。** 两个完整例子（Verdantis 的虚构新闻、以及一个组织关系例子）逐条演示了实体的格式与关系的格式。这是 few-shot 的标准做法：`<|>` 这种非自然分隔符**必须靠示例教会**，光靠文字描述模型不会稳定遵守。

**第三，`-Goal-` 与 `-Steps-` 的四步结构把「抽什么」与「怎么格式化」分开。** 步骤 1、2 讲内容，步骤 3、4 讲格式与终止。**终止符 `<|COMPLETE|>` 是必需的**——没有它，解析器无法区分「输出结束」与「模型中途截断」。

**第四，`relationship_strength` 只要求「a numeric score」而没给范围。** 示例里给了 2、8、9 这些值（实际分布看起来是 1-10），但提示词没有约束。于是模型可能给出 0.8（概率式）或 95（百分制）——这两者混进同一个 `weight` 列再被 `sum` 求和，结果没有统一含义。**这是一个真实存在的范围缺失**，靠示例隐式约定。

**Gleaning 循环是「再问几轮」的机制，它有两个退出条件。**

```
if self._max_gleanings > 0:
    for i in range(self._max_gleanings):
        messages_builder.add_user_message(CONTINUE_PROMPT)
        response = await self._model.completion_async(messages=messages_builder.build())
        response_text = response.content
        messages_builder.add_assistant_message(response_text)
        results += response_text

        # if this is the final glean, don't bother updating the continuation flag
        if i >= self._max_gleanings - 1:
            break

        messages_builder.add_user_message(LOOP_PROMPT)
        response = await self._model.completion_async(messages=messages_builder.build())
        if response.content != "Y":
            break
```

**这段循环的设计很值得细看，因为两次 LLM 调用做的是两件不同的事：**

- `CONTINUE_PROMPT` 是**抽取指令**：「上一次抽漏了很多，记住只输出符合已抽类型的实体，用同样格式加在下面」。它让模型继续产出记录，这些记录被 `results += response_text` **直接拼接到同一个字符串里**。
- `LOOP_PROMPT` 是**判定指令**：「看起来还有些实体关系可能被漏掉了。如果还有需要补的就答 Y，没有就答 N。请只回答单个字母 Y 或 N」。

**所以每一轮 gleaning 实际花两次 LLM 调用**（一次抽取 + 一次判定），除了最后一轮只抽不判定。默认 `max_gleanings=1`，于是默认行为是：1 次初始抽取 + 1 次补抽，**没有判定调用**（因为 `i >= max_gleanings - 1` 在第一次迭代就成立，直接 `break`）。也就是说默认配置下 `LOOP_PROMPT` 从未被使用——它只在 `max_gleanings >= 2` 时才可能被发出。

**「拼接而非合并」这个实现带来一个后果：所有轮次的输出被当成一个字符串解析。** 于是解析器必须容忍多个 `<|COMPLETE|>` 出现（每个轮次的输出都以它结尾）。`_process_result` 里那句 `if not record or record == COMPLETION_DELIMITER: continue` 正是为此——它把 `##` 切出来的 `<|COMPLETE|>` 段跳过。于是第二轮及之后的记录能被正常解析。

**`LOOP_PROMPT` 的判定用一个精确的字符串比较：`response.content != "Y"`。**

**只要模型回答了 `"Yes"`、`"y"`、`"Y."`、`" Y"` 中的任何一个，判定就失败并退出循环。** 提示词要求「请只回答单个字母 Y 或 N」，但这是请求不是保证——一个加了句号或空格的响应会静默终止 gleaning。这是一个**真实的脆弱点**：没有 `strip()`、没有 `upper()`、没有前缀匹配。

**解析器是手写的、对每类畸形都单独降级。**

```
records = [r.strip() for r in result.split(record_delimiter)]

for raw_record in records:
    record = re.sub(r"^\(|\)$", "", raw_record.strip())
    if not record or record == COMPLETION_DELIMITER:
        continue

    record_attributes = record.split(tuple_delimiter)
    record_type = record_attributes[0]

    if record_type == '"entity"' and len(record_attributes) >= 4:
        entity_name = clean_str(record_attributes[1].upper())
        entity_type = clean_str(record_attributes[2].upper())
        entity_description = clean_str(record_attributes[3])
        entities.append({... "source_id": source_id})

    if record_type == '"relationship"' and len(record_attributes) >= 5:
        source = clean_str(record_attributes[1].upper())
        target = clean_str(record_attributes[2].upper())
        edge_description = clean_str(record_attributes[3])
        try:
            weight = float(record_attributes[-1])
        except ValueError:
            weight = 1.0
        relationships.append({...})
```

五处容错点：

1. **`re.sub(r"^\(|\)$", "", ...)` 去掉两端括号。** 提示词要求的格式是 `("entity"<|>...)`，所以每条记录都被圆括号包着。`^\(|\)$` 只去一个（不是 `[()]` 全局替换），因为只有最外层那一对。
2. **`record_type == '"entity"'` 带引号比较。** 因为提示词写的是 `("entity"<|>...)`，模型输出的第一段就是带双引号的 `"entity"`。**注意它比较的是原始字符串，没有 `strip('"')`** ——如果模型输出 `("entity" <|>...)`（多一个空格），`record_attributes[0]` 是 `'"entity" '`，比较失败、该记录被静默丢弃。
3. **`len(record_attributes) >= 4` / `>= 5` 而不是 `== 4` / `== 5`。** 允许字段多余（描述里若含 `<|>` 会被切开，但取前 4 个仍正确）。而对关系，`weight = float(record_attributes[-1])` 用**最后一个**字段——这是配套的：即使描述被切碎，最后一段仍是权重。
4. **权重解析失败退回 `1.0`。** 模型给出 `"high"` 或 `"9/10"` 时不会炸，但所有这类关系挤在同一个默认值 1.0 上。
5. **名字与类型被 `.upper()`（描述不）。** 实体名统一大写是**合并的前提**——否则「Apple」与「APPLE」会变成两个实体。描述保留原大小写，因为它是给人读的。

**而 `.upper()` 也解释了提示词里那句「Name of the entity, capitalized」为什么说 `capitalized` 而在代码里做 `upper`**：提示词要的是首字母大写，代码统一成全大写。两者方向一致（都是规范化），但**提示词与实现的规范形式不同**，模型看到的示例是全大写（`CENTRAL INSTITUTION`），所以实际输出正好匹配 `upper`。

**`_empty_entities_df()` / `_empty_relationships_df()` 保证空结果也有列名。**

```
def _empty_entities_df() -> pd.DataFrame:
    return pd.DataFrame(columns=["title", "type", "description", "source_id"])

def _empty_relationships_df() -> pd.DataFrame:
    return pd.DataFrame(columns=["source", "target", "weight", "description", "source_id"])
```

**没有这两行，`pd.concat([])` 会返回一个无列的空 DataFrame，后续 `groupby(["title", "type"])` 会抛 `KeyError`。** 这是「空输入也要有正确 schema」的标准做法。它们被用在两处：单块抽取抛异常时的兜底返回，以及 `_process_result` 里 `if entities else _empty_entities_df()`。

**上下文构建是「追加式」的，这是 gleaning 能工作的基础。**

```
messages_builder = CompletionMessagesBuilder().add_user_message(...)
response = await self._model.completion_async(messages=messages_builder.build())
results = response.content
messages_builder.add_assistant_message(results)

if self._max_gleanings > 0:
    for i in range(self._max_gleanings):
        messages_builder.add_user_message(CONTINUE_PROMPT)
        ...
        messages_builder.add_assistant_message(response_text)
```

**每一轮都把上一轮的回答加进对话历史**，于是「补抽」是在「已经抽过什么」的上下文里进行的——模型能看到自己上次抽了什么，从而避免重复。这是 gleaning 有效的关键：**如果不带历史，第二次调用会重复抽同一批实体。**

代价是 token 消耗线性增长：第 N 轮的输入 = 原始提示 + 前 N-1 轮的完整输出。`max_gleanings` 大了会让单块成本快速上升。

## 关键取舍

**源码事实：** 标准路径每块一次 LLM 抽取 + 全量合并后每实体/每关系一次 LLM 摘要；抽取与摘要各自有独立的模型配置（`extract_graph.completion_model_id` / `summarize_descriptions.completion_model_id`）与独立的 cache 子命名空间。

**源码事实：** `config.concurrent_requests` 同时作为抽取与摘要的并发度；`config.async_mode` 只传给抽取（摘要用 `asyncio.Semaphore`）。

**源码事实：** 实体合并按 `["title", "type"]` 分组、关系按 `["source", "target"]` 分组；实体的 `frequency` 是 `source_id` 的计数，关系的 `weight` 是各块 LLM 打分的**求和**。摘要回填时实体按 `["title"]` merge、关系按 `["source", "target"]`——实体在合并时含 `type` 而回填时不含，同名异类型的两行会拿到同一个摘要。

**源码事实：** 抽取后立刻 `filter_orphan_relationships(relationships, entities)` 删除端点缺失的边（用删边而不是补空壳实体，以免污染图与报告）。

**源码事实：** 标准路径对「零实体」与「零关系」都 `raise ValueError`；NLP 路径对零实体 `raise`，对零关系只 `logger.error` 后继续，会让无边图流进 `prune_graph` / `create_communities`。

**源码事实：** `raw_entities` / `raw_relationships` 在摘要之前 `copy()`，仅当 `config.snapshots.raw_graph` 为真时写出。

**源码事实：** NLP 路径的实体 `type` 恒为 `"NOUN PHRASE"`、`description` 恒为 `""`；关系 `description` 恒为 `""`。因此 `Fast` 索引的后续步骤拿不到任何实体/关系描述。

**源码事实：** 提示词用自造分隔符（`<|>` 元组分隔、`##` 记录分隔、`<|COMPLETE|>` 完成标记）而非 JSON；`relationship_strength` 只说「a numeric score」未给范围，示例用 1-10 区间。

**源码事实：** gleaning 每轮花两次 LLM 调用（`CONTINUE_PROMPT` 抽取 + `LOOP_PROMPT` 判定 Y/N），最后一轮跳过判定；默认 `max_gleanings=1` 时判定调用永不发生。判定用 `response.content != "Y"` 精确比较，`"Yes"` / `"y"` / `"Y."` 都会终止循环。

**源码事实：** 解析器对每条记录做 `re.sub(r"^\(|\)$", "", ...)` 去外层括号；`record_type == '"entity"'` 带引号精确比较（多一个空格即被静默丢弃）；字段数用 `>=` 而非 `==`；关系权重取 `record_attributes[-1]` 且解析失败退回 `1.0`；实体名与类型 `.upper()`（合并的前提），描述保留原样。

**源码事实：** 对话历史是追加式的——每轮把上一轮输出 `add_assistant_message` 进 `messages_builder`，因此 gleaning 能看到自己抽过什么；代价是第 N 轮输入 token 随轮数线性增长。

**架构推论：** 「逐块抽取 + 按名字合并 + 描述摘要」这个三段式是「局部抽取、全局聚合」的唯一可行形状，而它的质量上限由第一段决定：**合并策略（同名即同实体）无法修复抽取阶段的错误，只能放大或压平它们。** 没有实体消歧意味着「Apple Inc.」与「苹果公司」永久是两条——这对多语言语料是硬伤。

**架构推论：** 用自造分隔符而非 JSON，是用「解析脆弱性」换「解析简单性」。当前实现的脆弱点很具体：`record_type` 的精确字符串比较、`weight` 的 `-1` 索引、分隔符出现在描述里就会切错。这些都能被一个更宽容的正则 + `strip` 修掉，但**代价是每次改提示词都要同步改解析器**——而这正是 JSON + schema 想解决的问题。这条路径的正确终点是「要求模型输出 JSON 并用容错解析器恢复」。

**架构推论：** `Fast` 路径与 `Standard` 路径产出的表**列相同、内容质量差一个量级**（无描述、无类型、关系权重是同构共现而非语义判断）。这种「形状兼容、语义降级」的替换最危险的地方在于：**下游所有代码都能跑通，没有任何地方会报错，只是结果变差。** 若要在系统里支持这类降级路径，应当在产物里标记来源（例如 `type="NOUN PHRASE"` 已经起到部分作用），并让检索/报告阶段能据此调整策略。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/index/workflows/extract_graph.py` | `extract_graph` | 标准抽取外壳：建两个模型、调操作层、写 entities/relationships、可选写 raw_* |
| `packages/graphrag/graphrag/index/workflows/extract_graph.py` | `get_summarized_entities_relationships` | drop 原 `description` 后 merge 摘要结果（实体按 `title`、关系按 `source,target`） |
| `packages/graphrag/graphrag/index/operations/extract_graph/extract_graph.py` | `extract_graph` | 逐行并发抽取 → 合并 → 删孤儿边 |
| `packages/graphrag/graphrag/index/operations/extract_graph/extract_graph.py` | `_merge_entities` / `_merge_relationships` | 两个 groupby：描述聚列表、`frequency` 计数、`weight` 求和 |
| `packages/graphrag/graphrag/index/operations/extract_graph/graph_extractor.py` | `GraphExtractor` | gleaning 循环 + 分隔符解析（三个分隔符常量在此定义） |
| `packages/graphrag/graphrag/index/operations/extract_graph/graph_extractor.py` | `_process_result` | 手写解析：去括号、带引号的类型比较、`>=` 字段数、权重失败退回 1.0 |
| `packages/graphrag/graphrag/index/workflows/extract_graph_nlp.py` | `extract_graph_nlp` | NLP 路径：`build_noun_graph` → 写同形状三列表（`type="NOUN PHRASE"`、空描述） |
| `packages/graphrag/graphrag/index/workflows/extract_graph_nlp.py` | `_write_entities` / `_write_relationships` | 流式写行；零关系只记日志 |
| `packages/graphrag/graphrag/index/operations/summarize_descriptions/summarize_descriptions.py` | `summarize_descriptions` | 用 `asyncio.Semaphore(num_threads)` 控制并发的批量摘要 |
| `packages/graphrag/graphrag/prompts/index/extract_graph.py` | `GRAPH_EXTRACTION_PROMPT` | 主提示：两段格式要求 + 两个完整示例 + `{entity_types}` / `{input_text}` 占位 |
| `packages/graphrag/graphrag/prompts/index/extract_graph.py` | `CONTINUE_PROMPT` | gleaning 的补抽指令 |
| `packages/graphrag/graphrag/prompts/index/extract_graph.py` | `LOOP_PROMPT` | gleaning 的 Y/N 判定指令 |
| `packages/graphrag/graphrag/index/operations/build_noun_graph/build_noun_graph.py` | `build_noun_graph` | 名词短语共现构图，`normalize_edge_weights` 为真时套 PMI |

## 参考

- [graphrag 源码：extract_graph.py（工作流）](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/workflows/extract_graph.py)
- [graphrag 源码：prompts/index/extract_graph.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/prompts/index/extract_graph.py)
- [graphrag 文档：索引方法对比](https://microsoft.github.io/graphrag/index/methods/)
- [graphrag 源码仓库](https://github.com/microsoft/graphrag)

## 与主核 / AI 的关系

抽取这一步在主核（多模态数据库内核）里对应的是**外部数据的导入与规范化**：都要把不可信的输入转成符合 schema 的记录，且都要容忍输入的畸形。四条经验。

**第一，让模型输出结构化数据时，「要求 JSON」和「要求分隔符」是两种不同的风险分配。** 分隔符方案把风险放在**解析脆弱性**上（多一个空格、描述里含分隔符就坏），JSON 方案把风险放在**生成失败**上（漏引号、多逗号、加围栏）。前者解析代码简单但要求模型稳定守格式，后者解析复杂但可用 schema 校验兜底。**正确选择取决于「能不能对模型输出做校验再重试」**：能重试就选 JSON，不能重试（成本太高）就选分隔符 + 宽容解析。

**第二，「精确字符串比较判定」在任何 LLM 输出上都不可靠。** `response.content != "Y"` 会因为一个句号而误判。主核里凡是解析外部工具输出（`EXPLAIN` 输出、CLI 结果、第三方 API 的枚举字符串）都要做归一化：`strip().upper().rstrip(".")`。**判断准则：只要比较的另一端是「别人生成的字符串」，就必须先归一化再比较。**

**第三，降级路径必须让「降级」可被下游感知。** `Fast` 索引产出 `type="NOUN PHRASE"` 与空描述——形状与标准路径一致，语义差一个量级，而所有下游代码都能跑通。主核里若有「快速模式 / 精确模式」两条实现（近似索引 / 精确索引、采样统计 / 全量统计），**产物里必须留下可识别的降级标记**，否则用户拿到的是「能跑但结论错误」的结果，而这比报错更难发现。

**第四，合并策略决定系统的语义上限，且无法在下游修补。** 这里「同名即同实体」意味着没有消歧，跨语言/别名永久分裂。主核里凡是「把多条外部记录归并成一条」的导入流程（同一实体的多源元数据合并），**合并键的选择就是数据质量的边界**：按 id 合并最准但要求上游有稳定 id，按名字合并最省事但引入别名问题。这个决定必须在设计期做对，运行期无法补救。
