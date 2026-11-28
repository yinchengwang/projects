---
project: graphrag
feature: local-search
title: 局部检索
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/query/structured_search/local_search/search.py
    symbols: [LocalSearch]
  - path: packages/graphrag/graphrag/query/structured_search/local_search/mixed_context.py
    symbols: [LocalSearchMixedContext]
  - path: packages/graphrag/graphrag/prompts/query/local_search_system_prompt.py
    symbols: [LOCAL_SEARCH_SYSTEM_PROMPT]
---

# 局部检索

## 一句话职责

把用户问题用嵌入映射到若干实体，再从这些实体沿图取出关系、协变量、文本单元与所属社区报告，装进**一份**上下文窗口里——四类内容按一组可配置的比例分配 token 预算，装不下的部分明确丢弃。

## 机制是什么

local search 是 graphrag 的默认模式，它回答的是「具体问题」：谁是谁的老板、某公司发生了什么事。这类问题**有明确的实体焦点**，所以检索的入口是「先找出问题里提到的实体」。

于是有四类内容值得放进上下文，而它们的价值与可靠性依次递减：

| 内容 | 来源 | 为什么有用 | 风险 |
|---|---|---|---|
| 社区报告 | 实体所属社区 | 提供该话题的概括背景 | 层级可能过细，浪费预算 |
| 实体表 | 嵌入匹配到的实体 | 名字 + 类型 + 描述 | 描述可能是旧的摘要 |
| 关系表 | 实体的一跳邻边 | 提供实体间的连接事实 | 边多时线性膨胀 |
| 文本单元 | 实体关联的原文块 | 最原始的出处 | 最占 token |

**四类内容抢同一份预算**，这就是 `LocalSearchMixedContext` 存在的全部理由。它不是一个「检索器」，而是一个**预算分配器**。

而预算分配的规则有两条硬约束：

1. **总和不能超过窗口**。所以 `community_prop + text_unit_prop > 1` 直接抛错（第三个比例是算出来的：`1 - community_prop - text_unit_prop`）。
2. **对话历史优先**。历史上下文先算、先从总量里扣掉，之后三个比例才作用在**剩余量**上。

第二条是这一页最容易被忽略的设计：**历史不是四类内容之一，它是一个前置扣减**。所以「带了很多轮历史」会让所有四类内容等比缩水，而不是让某一类内容单独变少。

## 怎么实现的

### 入口：`LocalSearch.search` 的六步

```python
async def search(self, query, conversation_history=None, **kwargs) -> SearchResult:
    start_time = time.time()
    search_prompt = ""
    llm_calls, prompt_tokens, output_tokens = {}, {}, {}
    context_result = self.context_builder.build_context(
        query=query,
        conversation_history=conversation_history,
        **kwargs,
        **self.context_builder_params,
    )
    llm_calls["build_context"] = context_result.llm_calls
    prompt_tokens["build_context"] = context_result.prompt_tokens
    output_tokens["build_context"] = context_result.output_tokens
```

四处：

1. **参数合并顺序是 `**kwargs` 然后 `**self.context_builder_params`** —— 所以**构造时固定的参数会覆盖调用时传的**。

   **这是一个反直觉的顺序**：通常「调用时传的」应该覆盖「构造时的默认」。而这里的顺序意味着 `search(query, top_k_mapped_entities=5)` **无效**（会被 `context_builder_params` 里的 `top_k_entities` 覆盖）。

   要改参数只能改引擎的 `context_builder_params`。这是一处真实的接口陷阱——而 d04 的 DRIFT 内部调用 local 时正好依赖这个顺序（它要把自己的参数塞进去）。

2. **`build_context` 的成本单独记一格** —— 因为 local search 的上下文构建本身可能调 LLM（对话历史的重排/摘要，见 d07）。

3. **`llm_calls` 是字典而不是整数**（局部变量）。最后才 `sum(llm_calls.values())` 汇总成 `SearchResult.llm_calls`。

**两套 prompt 格式，由 `drift_query` 是否出现在 `kwargs` 里决定。**

```python
try:
    if "drift_query" in kwargs:
        drift_query = kwargs["drift_query"]
        search_prompt = self.system_prompt.format(
            context_data=context_result.context_chunks,
            response_type=self.response_type,
            global_query=drift_query,
            followups=kwargs.get("k_followups", 0),
        )
    else:
        search_prompt = self.system_prompt.format(
            context_data=context_result.context_chunks,
            response_type=self.response_type,
        )
```

四处：

1. **`"drift_query" in kwargs` 是一个字符串键探测** —— 不是类型/参数，而是一个魔法键。所以「这个引擎被 DRIFT 复用」这件事是通过一个 kwargs 约定传达的。

2. **`LocalSearch` 因此有两个调用者**：用户直接查（走默认 prompt），和 DRIFT 的每一轮局部检索（走带 `{global_query}` / `{followups}` 的 prompt）。

   **这意味着 `system_prompt` 必须同时支持两种模板** —— 若 DRIFT 传了 `drift_query` 而系统 prompt 里没有 `{global_query}` 占位符，`str.format` 会**静默忽略多余的关键字参数**（Python 的 `format` 允许多余 kwargs）。所以它不会报错，只是那两个值没用上。

3. **`followups=kwargs.get("k_followups", 0)`** —— 默认 0。所以 DRIFT 不传时提示词里会写「0 followups」。

4. **`drift_query` 本身也被当作 `global_query` 传进 prompt** —— 即「原始全局问题」在局部检索时仍然可见。这是 DRIFT 能保持全局视角的机制之一。

**然后是流式调用与回调。**

```python
messages_builder = (
    CompletionMessagesBuilder()
    .add_system_message(search_prompt)
    .add_user_message(query)
)

full_response = ""
response = await self.model.completion_async(
    messages=messages_builder.build(), stream=True, **self.model_params,
)
async for chunk in response:
    response_text = chunk.choices[0].delta.content or ""
    full_response += response_text
    for callback in self.callbacks:
        callback.on_llm_new_token(response_text)
```

三处：

1. **`stream=True` 即使 `search()` 也要流式** —— 所以非流式接口内部是「流式接收后拼接」。这统一了两条路径，也意味着**回调在 `search()` 里同样会触发**。

2. **`chunk.choices[0].delta.content or ""`** —— `or ""` 处理 `content` 为 `None` 的情形（推理模型的某些 chunk 只有 `reasoning_content`）。

3. **`full_response += response_text`** 是 O(n²) 的字符串拼接。对几百到几千 token 的输出可接受，但这是 Python 里常见的性能点（通常用 list + join）。

**异常路径返回一个「看起来成功」的结果，而它的统计字段与成功路径不一致。**

```python
except Exception:
    logger.exception("Exception in _asearch")
    return SearchResult(
        response="",
        context_data=context_result.context_records,
        context_text=context_result.context_chunks,
        completion_time=time.time() - start_time,
        llm_calls=1,
        prompt_tokens=len(self.tokenizer.encode(search_prompt)),
        output_tokens=0,
    )
```

四处：

1. **不重新抛出，返回空 `response`** —— 所以调用方拿到的是一个「成功但答案为空」的结果。`logger.exception` 记了堆栈，所以错误不丢。

   **这个选择让「LLM 调用失败」与「模型返回空答案」在上层难以区分** —— 两者都是 `response=""`。

2. **`llm_calls=1` 是硬编码的猜测** —— 它假设「一次调用失败了」。但若失败发生在 `build_context` 之前，实际调用次数是 0；若发生在流式接收中途，可能是 1。

3. **`llm_calls_categories` 等三个字段没设**（保持 `None`）—— 所以 `llm_calls=1` 却没有任何明细。**这违反了 d01 里「总量 = 明细之和」的不变量**。

4. **`prompt_tokens` 用的是 `search_prompt`** —— 但 `search_prompt` 在异常发生时可能是 `""`（初值），此时 `encode("")` 是 0。所以这个字段在「格式化 prompt 就失败」的情形下是 0，在「LLM 调用失败」的情形下是真实值。

   **注意 `search_prompt = ""` 的初始化就在函数开头** —— 正是为了让这条异常路径能安全 `encode`。这是一处「为异常路径预先设默认值」的做法。

### `build_context`：比例分配的全部计算

```python
if community_prop + text_unit_prop > 1:
    raise ValueError("The sum of community_prop and text_unit_prop should not exceed 1.")
```

**只校验两个比例的**和，第三个不参与校验（因为它是 `1 - 另两个` 算出来的，只要和 ≤ 1 它就 ≥ 0）。

```python
if conversation_history:
    pre_user_questions = "\n".join(
        conversation_history.get_user_turns(conversation_history_max_turns)
    )
    query = f"{query}\n{pre_user_questions}"
```

**历史里的用户问句被拼进 query 本身** —— 而不只是作为独立的上下文块。

**这是一个关键设计**：拼接后的 `query` 被用于**实体映射**（`map_query_to_entities`）与**最终的用户消息**（`add_user_message(query)`）。所以：

- **实体映射受历史影响** —— 「那它的 CEO 呢」这种省略主语的追问，靠历史问句补全才能映射到正确实体。
- **最终发给模型的用户消息里也含历史** —— 所以历史被传达了**两次**：一次作为 query 的一部分，一次作为独立的上下文块（见下）。

**这两次不是冗余**：拼进 query 是为了「让检索知道在问什么」（检索侧），独立上下文块是为了「让生成知道之前聊过什么」（生成侧）。但代价是**历史问句的 token 被计了两次**（一次在 system prompt 的 `{context_data}` 里，一次在 user message 里）。而预算扣减只扣了独立块那一次。

**实体映射用 `oversample_scaler=2`。**

```python
selected_entities = map_query_to_entities(
    query=query, text_embedding_vectorstore=self.entity_text_embeddings,
    text_embedder=self.text_embedder, all_entities_dict=self.entities,
    embedding_vectorstore_key=self.embedding_vectorstore_key,
    include_entity_names=include_entity_names, exclude_entity_names=exclude_entity_names,
    k=top_k_mapped_entities, oversample_scaler=2,
)
```

**`oversample_scaler=2` 的意思是「向量库取 2k 条，再过滤到 k 条」** —— 多取一倍是为了给 `include_entity_names` / `exclude_entity_names` 留余量：若先取 k 条再过滤，很可能剩下的不到 k 条。

**这是一处「过滤与 top-k 的顺序」问题**：向量库的 top-k 是按相似度，而过滤是硬条件。正确顺序必须是「先按硬条件筛出候选集，再在其中按相似度取 k」——`oversample_scaler` 是它的近似实现（多取一些，希望过滤后还够）。

**然后是四段构建，每段都有「空则跳过」。**

```python
final_context = list[str]()
final_context_data = dict[str, pd.DataFrame]()

if conversation_history:
    (conversation_history_context, conversation_history_context_data) = conversation_history.build_context(
        include_user_turns_only=conversation_history_user_turns_only,
        max_qa_turns=conversation_history_max_turns,
        column_delimiter=column_delimiter,
        max_context_tokens=max_context_tokens,
        recency_bias=False,
    )
    if conversation_history_context.strip() != "":
        final_context.append(conversation_history_context)
        final_context_data = conversation_history_context_data
        max_context_tokens = max_context_tokens - len(
            self.tokenizer.encode(conversation_history_context)
        )
```

四处：

1. **`recency_bias=False`** —— 历史构建时不偏向最近的轮次。这与 d07 会讲的「相关度打分」不同：这里用的是「最近 N 轮」的简单截断（`max_qa_turns`），不是打分。

2. **只有非空才扣预算**（`if ...strip() != ""`）—— 所以「有历史对象但产不出内容」不会白白扣掉预算。

3. **`final_context_data = conversation_history_context_data`（赋值而非合并）** —— 因为它是第一个，所以直接赋值。后面的用 `{**a, **b}` 合并。

4. **扣减是「减去实际编码长度」而不是「按比例预留」** —— 所以历史是**实报实销**：占了多少扣多少。这比预留比例更精确（预留会导致浪费）。

但这也带来一个问题：**若历史本身超过了 `max_context_tokens`，扣完会变成负数**。此时后面的 `max(int(negative * prop), 0)` 会得到 0 —— 所以是安全的（有 `max(..., 0)` 兜底），但**四类内容全部为空**，只有历史进上下文。

**community 与 local 的比例计算是独立的两次。**

```python
community_tokens = max(int(max_context_tokens * community_prop), 0)
...
local_prop = 1 - community_prop - text_unit_prop
local_tokens = max(int(max_context_tokens * local_prop), 0)
...
text_unit_tokens = max(int(max_context_tokens * text_unit_prop), 0)
```

**三处 `max(..., 0)` 都在** —— 所以负数比例（若用户传了负的 prop）不会传下去变成负 token 预算。

**而三个预算是互相独立的**：community 用不完的 token **不会**给 local。所以「实际用了多少」可能远小于 `max_context_tokens`。

**这是一处保守的设计**：宁可少用预算，也不让某一类内容挤占其他类。代价是上下文利用率低。

### `_build_community_context`：按「匹配实体数 + rank」排序

```python
community_matches = {}
for entity in selected_entities:
    if entity.community_ids:
        for community_id in entity.community_ids:
            community_matches[community_id] = community_matches.get(community_id, 0) + 1
```

**统计每个社区被多少个选中实体命中** —— 所以「多个选中实体同属一个社区」会让那个社区排更前。这是合理的：它是这些实体的共同话题。

```python
selected_communities = [
    self.community_reports[community_id]
    for community_id in community_matches
    if community_id in self.community_reports
]
for community in selected_communities:
    if community.attributes is None:
        community.attributes = {}
    community.attributes["matches"] = community_matches[community.community_id]
selected_communities.sort(
    key=lambda x: (x.attributes["matches"], x.rank),
    reverse=True,
)
for community in selected_communities:
    del community.attributes["matches"]
```

五处：

1. **`if community_id in self.community_reports` 过滤** —— 实体可能引用一个没有报告的社区（d01 讲过 roll up 会丢掉未被引用的社区）。所以这一步是必需的。

2. **排序键是元组 `(matches, rank)` 且 `reverse=True`** —— 所以先按命中数降序，平票时按 rank 降序。

3. **`matches` 被写进 `community.attributes` 再排序再删除** —— 这是一个**很值得注意的手法**：因为 `attributes` 是 `CommunityReport` 的一个通用字段，而 Python 的 `sort` 需要一个可以取值的键。

   **为什么要「写进去再删掉」而不是直接用 `key=lambda` 闭包？** 完全可以写 `key=lambda x: (community_matches[x.community_id], x.rank)`。

   **代价是这段代码不是线程安全的、也不是可重入的**：它**修改了共享的 `CommunityReport` 对象**。若同一个 `community_reports` 被两个并发查询使用，一个查询的 `del` 可能删掉另一个查询刚写入的 `matches`，导致 `KeyError`。

   **这是一处真实的可重入性缺陷**，而 `get_local_search_engine` 构造的 `LocalSearchMixedContext` 通常被复用（一个引擎多次查询）。

4. **`if community.attributes is None: community.attributes = {}`** —— 因为 `attributes` 可能是 `None`。这是第二处就地修改。

5. **`reverse=True` 而不是在 key 里取负** —— 对元组来说两者等价。

**然后调用共享的 `build_community_context`（d06），并 `single_batch=True`。**

```python
context_text, context_data = build_community_context(
    community_reports=selected_communities, tokenizer=self.tokenizer,
    use_community_summary=use_community_summary, column_delimiter=column_delimiter,
    shuffle_data=False, include_community_rank=include_community_rank,
    min_community_rank=min_community_rank, max_context_tokens=max_context_tokens,
    single_batch=True, context_name=context_name,
)
if isinstance(context_text, list) and len(context_text) > 0:
    context_text = "\n\n".join(context_text)
```

三处：

1. **`single_batch=True`** —— 因为这个 builder 支持「分多批」输出（global search 需要），而 local search 只要一份。所以 `True` 意味着「不要分批」，返回的仍是元组形态。

2. **`isinstance(context_text, list)` 判断** —— 因为 `single_batch` 影响返回类型：`True` 时返回 `(str, dict)`，`False` 时返回 `(list[str], dict)`。这里虽然传了 `True` 但仍做了类型检查——**防御性的冗余**（或历史遗留）。

3. **`shuffle_data=False`** —— 稳定输出，便于缓存与调试。

### `_build_text_unit_context`：两级排序

```python
unit_info_list = []
relationship_values = list(self.relationships.values())

for index, entity in enumerate(selected_entities):
    entity_relationships = [
        rel for rel in relationship_values
        if rel.source == entity.title or rel.target == entity.title
    ]
    for text_id in entity.text_unit_ids or []:
        if text_id not in text_unit_ids_set and text_id in self.text_units:
            selected_unit = deepcopy(self.text_units[text_id])
            num_relationships = count_relationships(entity_relationships, selected_unit)
            text_unit_ids_set.add(text_id)
            unit_info_list.append((selected_unit, index, num_relationships))

unit_info_list.sort(key=lambda x: (x[1], -x[2]))
```

六处：

1. **`rel.source == entity.title or rel.target == entity.title`** —— 关系里存的是**实体 title 而不是 id**。所以「名字重复的实体」会让关系归属出错。这是一处依赖「title 唯一」的假设。

2. **`text_unit_ids_set` 去重** —— 一个文本单元可能被多个选中实体引用，只保留第一次。

3. **`deepcopy(self.text_units[text_id])`** —— **深拷贝！** 而其他几类内容（实体、关系）都是原对象。为什么？

   `build_text_unit_context` 可能会**改写** `text_unit.text`（例如按 token 预算截断）。若不改写就不需要拷贝。这里选择拷贝说明 downstream 可能改它，而 `self.text_units` 要复用。

   **这是一处「防御性深拷贝」**，代价是每个文本单元都复制一份（文本可能很长）。

4. **`sort(key=lambda x: (x[1], -x[2]))`** —— `x[1]` 是 `index`（第几个选中实体），`x[2]` 是关系数。所以排序是「**先按实体顺序，再按关系数降序**」。

   **注意 `-x[2]` 而不是 `reverse=True`** —— 因为 `x[1]` 要升序、`x[2]` 要降序，不能整体 reverse。这是混合方向的正确写法。

5. **`entity_order` 优先** —— 所以「第一个选中实体的文本单元」排在「第二个选中实体的」前面，即使后者关系更多。

   **这是一个隐含的价值判断**：实体映射的相似度排序比「文本单元连接了多少关系」更重要。

6. **`entity_relationships` 在外层循环里算，但内层只对它用** —— 所以每个实体算一次关系列表，然后对它的每个文本单元算交集。这是 O(实体数 × 关系数) 的过滤，而 `relationship_values` 是全量关系。

   **大图上的性能点**：若关系有 10 万条、选中 10 个实体，这个循环是 100 万次比较。而其实应该先按 source/target 建索引。

### `_build_local_context`：渐进式加入

```python
entity_context, entity_context_data = build_entity_context(
    selected_entities=selected_entities, tokenizer=self.tokenizer,
    max_context_tokens=max_context_tokens, column_delimiter=column_delimiter,
    include_entity_rank=include_entity_rank, rank_description=rank_description,
    context_name="Entities",
)
entity_tokens = len(self.tokenizer.encode(entity_context))
```

**实体上下文先全量构建，不设预算** —— 注意它把 `max_context_tokens` 传下去了，但 `build_entity_context` 内部的预算逻辑是它自己的（d06）。所以实体表可能已经用掉大部分预算。

```python
added_entities = []
for entity in selected_entities:
    current_context = []
    added_entities.append(entity)

    (relationship_context, relationship_context_data) = build_relationship_context(
        selected_entities=added_entities, ...
    )
    total_tokens = entity_tokens + len(self.tokenizer.encode(relationship_context))
    for covariate in self.covariates:
        covariate_context, covariate_context_data = build_covariates_context(...)
        total_tokens += len(self.tokenizer.encode(covariate_context))
```

**这是一个增量重算模式**：每加一个实体，就把「所有已加实体的关系 + 协变量」**整体重算一遍**。

四处：

1. **`added_entities.append(entity)` 在循环开头** —— 所以第 i 轮重算的是前 i+1 个实体。

2. **`total_tokens = entity_tokens + ...` 每轮重置** —— 所以每轮都重新算总账。

3. **`self.covariates` 是一个 `dict[str, list[Covariate]]`** —— 所以「每种协变量类型」单独产出一段上下文。循环里 `for covariate in self.covariates` 遍历的是**类型名**。

4. **复杂度是 O(n²)** —— n 个实体就把关系与协变量上下文重算 n 次。而每次重算都包含 `tokenizer.encode` 全文。

   **这是一处明确的性能热点**，但它换来了「精确的增量预算」：只有在加入下一个实体确实超预算时才停，而不是预估。

**循环的退出条件在哪？** 在 `total_tokens` 超过 `max_context_tokens` 时 `break`，并且**不保留这一轮的 `current_context`**（保留上一轮的结果）。

**这意味着「最后一个加入的实体可能是被丢弃的」** —— 所以 `final_context` 里的实体数与 `added_entities` 长度可能不同。

### prompt：整段重复了一次

`LOCAL_SEARCH_SYSTEM_PROMPT` 有一个非常显眼的特征：**`---Goal---` 到 `---Target response length and format---` 这整段在文件里出现了两次**。

第一次与第二次的唯一差别是第二次末尾多了两行：

```
Add sections and commentary to the response as appropriate for the length and format. Style the response in markdown.
```

**这是一个明确的「末尾强化」模式**：LLM 对 prompt 末尾的指令遵循度高于中段，所以把最关键的「格式要求」重复放到最后。

三处值得记的实现细节：

1. **引用格式是自定义的**：`[Data: <dataset name> (record ids); ...]`。示例里 `Sources (15, 16), Reports (1), Entities (5, 7); Relationships (23); Claims (2, 7, 34, 46, 64, +more)`。

   **注意 `;<` 与 `,` 混用** —— 分号分行内分组，逗号分 id。这是一种**紧凑的引用语法**，目的是让引用不占太多 token 同时保留可追溯性。

2. **「Do not list more than 5 record ids in a single reference. Instead, list the top 5 most relevant record ids and add "+more"」** —— 明确的数量上限 + 溢出标记。

   **这是一处「引用成本控制」**：若不限制，一个答案可能带几十个 id，吃掉大量输出 token。

3. **「where 15, 16, 1, ... represent the id (not the index) of the relevant data record」** —— 显式澄清「id 不是序号」。因为模型很容易把表格的行号当 id。而 `{context_data}` 里的表格**带 id 列**，所以这个澄清是必需的。

4. **「If you don't know the answer, just say so. Do not make anything up.」** 与 **「Do not include information where the supporting evidence for it is not provided.」** —— 两句都是「拒答优先」。而第二句更严：不只整体拒答，还要求**逐条**有据。

**占位符只有两个**：`{response_type}` 与 `{context_data}`。所以 DRIFT 传的 `{global_query}` / `{followups}` 在这个默认 prompt 里**不存在**——`str.format` 会忽略它们（详见前面的分析）。

## 关键取舍

**源码事实：** `LocalSearch.search` 里 `**kwargs` 在 `**self.context_builder_params` **之前**展开，所以构造时固定的参数会覆盖调用时传的——`search(query, top_k_mapped_entities=5)` 无效。

**源码事实：** 两套 prompt 由 `"drift_query" in kwargs` 这个魔法键分派；DRIFT 分支额外传 `global_query` 与 `followups`，而默认 `LOCAL_SEARCH_SYSTEM_PROMPT` 不含这两个占位符，`str.format` 会静默忽略多余关键字。`LocalSearch` 因此有两个调用者（直接查 / DRIFT 每轮）。

**源码事实：** 即使 `search()` 也走 `stream=True` 再拼接，所以回调在非流式路径同样触发；`full_response += text` 是 O(n²) 拼接。

**源码事实：** 异常路径**不重新抛出**，返回 `response=""` 的 `SearchResult`（`logger.exception` 留堆栈），所以「LLM 失败」与「模型答空」在上层无法区分。该路径 `llm_calls=1` 硬编码且 `_categories` 三个字段留 `None`，**违反 d01 的「总量 = 明细之和」不变量**；`search_prompt` 在函数开头初始化为 `""` 正是为了让这条路径的 `encode` 安全。

**源码事实：** 历史问句被拼进 `query` **本身**（不只作为独立上下文块），所以实体映射也受历史影响（支持「那它的 CEO 呢」这类省略主语的追问）；代价是历史问句的 token 在 system prompt 与 user message 里**各计一次**，而预算扣减只扣独立块那一次。

**源码事实：** 预算顺序是「历史先扣（按实际编码长度，实报实销）→ 剩余量再按 `community_prop` / `text_unit_prop` / `1-两者` 三分」。若历史单独就超过窗口，扣减后为负，三处 `max(..., 0)` 兜底成 0，于是上下文只剩历史块。

**源码事实：** 三个子预算**互相独立**，社区用不完的 token 不会给 local，所以实际使用量常小于 `max_context_tokens`。

**源码事实：** `community_prop + text_unit_prop > 1` 直接抛 `ValueError`；第三个比例不校验（它是算出来的，和 ≤ 1 即非负）。

**源码事实：** 实体映射用 `oversample_scaler=2` —— 向量库取 2k 条再过滤到 k 条，为 `include_entity_names` / `exclude_entity_names` 留余量（否则过滤后可能不足 k）。

**源码事实：** `_build_community_context` 把 `matches` **写进共享 `CommunityReport.attributes`**、排序、再 `del` 掉——完全可以改用 `key=lambda` 闭包，所以这是一处**非重入副作用**：并发的两个查询可能互相删掉对方刚写的键（`LocalSearchMixedContext` 通常被复用）。

**源码事实：** 社区排序键是 `(matches, rank)` 降序；`if community_id in self.community_reports` 过滤掉「实体引用了但无报告」的社区（d01 的 roll up 会丢这类）。

**源码事实：** `build_community_context` 传 `single_batch=True`（local 只要一份），但仍保留了 `isinstance(context_text, list)` 的类型分支——防御性冗余。

**源码事实：** `_build_text_unit_context` 对 `self.text_units[text_id]` 做 `deepcopy`（其他几类内容都用原对象），说明 downstream 可能改写 `text`；排序键 `(entity_order, -num_relationships)` 是混合方向（升序 + 降序），所以用 `-x[2]` 而非 `reverse=True`。

**源码事实：** 文本单元的实体顺序优先于关系数——「第一个选中实体的块」排在「第二个选中实体的块」之前，即使后者关系更多。而关系匹配用 `rel.source == entity.title`（**title 不是 id**），隐含「title 唯一」假设；`entity_relationships` 在全量关系列表上线性过滤，大图上是 O(实体 × 关系)。

**源码事实：** `_build_local_context` 是增量重算：每加一个实体就把「已加实体的关系 + 全部协变量」整体重算一次（含 `tokenizer.encode` 全文），复杂度 O(n²)，换来精确的增量预算判断；超预算时 `break` 且**不保留当前轮**，所以 `final_context` 里的实体数可能少于 `selected_entities`。

**源码事实：** `LOCAL_SEARCH_SYSTEM_PROMPT` 把 `---Goal---` 到「目标长度与格式」整段**重复两次**，第二次末尾多两行（`Add sections and commentary...` + `Style the response in markdown`）——末尾强化格式要求。

**源码事实：** prompt 里引用语法是 `[Data: <dataset> (ids); ...]`（分号分组、逗号分 id），单条引用上限 5 个 id 并用 `+more` 标记；显式澄清「id 不是 index」，因为上下文表格带 id 列而模型容易填行号。

**源码事实：** 占位符只有 `{response_type}` 与 `{context_data}`。

**架构推论：** local search 的本质是**预算分配器**而不是检索器。它的技术含量全在「四类内容抢一份窗口」这个约束下如何分配，而分配规则的两条性质（历史前置扣减、三份预算独立）决定了它的行为特征：带很多历史会让所有内容等比缩水，而某一类内容用不完的预算会被浪费。若要提升上下文利用率，需要的是「配额 + 回收」，而不是调比例。

**架构推论：** 「历史拼进 query」这个做法解决了省略主语的追问，但它把一个**检索侧**的需求与一个**生成侧**的需求混在同一份数据上，导致 token 重复计费。更干净的做法是把历史只放在检索侧（用独立字段传给 `map_query_to_entities`），生成侧靠独立上下文块。当前做法的好处是实现简单（一个 `query` 变量贯穿），代价是 token 账目不准。

**架构推论：** `matches` 写进共享对象再删除这个手法说明**作者需要一个「排序时可见的中间键」但不想改数据结构**。改用闭包是一行的事，所以这更像「没意识到」而不是「有意选择」。而它的后果（不可重入）在单线程 CLI 场景下不会暴露，只在「一个引擎并发处理多查询」时才出现——**这正是这类缺陷难以发现的典型形态**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/query/structured_search/local_search/search.py` | `LocalSearch` | 局部检索主流程：参数合并、两套 prompt、流式拼接、异常兜底 |
| `packages/graphrag/graphrag/query/structured_search/local_search/search.py` | `LocalSearch.search` | 非流式入口（内部仍走 stream） |
| `packages/graphrag/graphrag/query/structured_search/local_search/search.py` | `LocalSearch.stream_search` | 逐 chunk 产出并触发回调 |
| `packages/graphrag/graphrag/query/structured_search/local_search/mixed_context.py` | `LocalSearchMixedContext` | 四类内容的预算分配器 |
| `packages/graphrag/graphrag/query/structured_search/local_search/mixed_context.py` | `LocalSearchMixedContext.build_context` | 历史扣减 + 三段比例 + 空段跳过 |
| `packages/graphrag/graphrag/query/structured_search/local_search/mixed_context.py` | `LocalSearchMixedContext._build_community_context` | 按命中实体数 + rank 排序（写 `attributes` 再删） |
| `packages/graphrag/graphrag/query/structured_search/local_search/mixed_context.py` | `LocalSearchMixedContext._build_text_unit_context` | 两级排序 + `deepcopy` |
| `packages/graphrag/graphrag/query/structured_search/local_search/mixed_context.py` | `LocalSearchMixedContext._build_local_context` | 实体/关系/协变量的增量重算 |
| `packages/graphrag/graphrag/query/context_builder/entity_extraction.py` | `map_query_to_entities` | 问题 → 实体（含 `oversample_scaler`） |
| `packages/graphrag/graphrag/query/context_builder/source_context.py` | `build_text_unit_context` | 文本单元渲染成表格 |
| `packages/graphrag/graphrag/query/context_builder/source_context.py` | `count_relationships` | 文本单元连接的关系数 |
| `packages/graphrag/graphrag/query/context_builder/local_context.py` | `build_entity_context` | 实体表构建 |
| `packages/graphrag/graphrag/query/context_builder/local_context.py` | `build_relationship_context` | 关系表构建 |
| `packages/graphrag/graphrag/query/context_builder/community_context.py` | `build_community_context` | 社区报告渲染（local 用 `single_batch=True`） |
| `packages/graphrag/graphrag/prompts/query/local_search_system_prompt.py` | `LOCAL_SEARCH_SYSTEM_PROMPT` | 引用语法 + 重复的 Goal 段 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：局部检索](https://microsoft.github.io/graphrag/query/local_search/)
- [graphrag 论文（Edge et al., 2024）](https://arxiv.org/abs/2404.16130)
- [graphrag 源码：local_search/mixed_context.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/query/structured_search/local_search/mixed_context.py)

## 与主核 / AI 的关系

`LocalSearchMixedContext` 的预算分配与主核（多模态数据库内核）的**共享缓冲区分配**是同一类问题：有限的资源（buffer pool / context window）要在多个消费者（多个关系 / 四类内容）之间分配。但两者的策略完全相反：主核用 LRU/K 淘汰**动态回收**（谁的页被用到就给谁），而这里用**固定配额**（谁的比例就是谁的）。

**这个差别来自反馈信号的有无**：主核能观测「页是否被访问」，所以能动态调整；而这里**无法知道「多给社区报告 500 token 能不能提升答案质量」**，所以只能用静态配额。判据很清楚：**有可观测的效用信号才谈动态分配**。这也解释了为什么这里的比例要暴露成配置——它是靠人工调参代替反馈信号。

第二处对照是 `oversample_scaler=2` 与主核的**索引扫描加过滤**。两者的共同问题是「先取 top-k 再过滤会不足 k 条」。主核的解法是**把过滤下推到索引扫描**（`IndexScan` 的 `indexqual`），从而保证「取到的 k 条就是满足条件的 k 条」；这里只能**多取一倍再过滤**——因为向量库的 top-k 是黑盒，无法下推。**判据是「过滤条件能否进入检索算法内部」**：能则下推（精确），不能则过采样（近似）。

第三处是「历史拼进 query 后再独立成块」与主核的**谓词下推与投影重叠**。两者都是「同一份数据在两个环节各用一次」，而代价都是重复成本（token / I/O）。主核的优化器会通过「投影只算一次、结果复用」消除重复；这里没有等价机制，只能接受重复计费。

对 RAG 工程的实际意义在于：local search 是所有 graph-RAG 实现的默认路径，而它最容易踩的坑不在图算法上，而在**预算算术**上。这份实现给出的三条经验可直接用：**历史要前置扣减而不是挤占某一类内容**（否则「聊得久了突然答不出具体问题」）、**固定配额会导致预算浪费**（要显式接受或改成可回收）、以及**过滤条件不能下推时必须过采样**（否则 top-k 会静默不足）。
