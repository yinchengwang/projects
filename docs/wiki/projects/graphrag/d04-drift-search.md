---
project: graphrag
feature: drift-search
title: DRIFT 检索
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/query/structured_search/drift_search/search.py
    symbols: [DRIFTSearch]
  - path: packages/graphrag/graphrag/query/structured_search/drift_search/primer.py
    symbols: [DRIFTPrimer, PrimerResponse]
  - path: packages/graphrag/graphrag/query/structured_search/drift_search/state.py
    symbols: [QueryState]
  - path: packages/graphrag/graphrag/query/structured_search/drift_search/action.py
    symbols: [DriftAction]
---

# DRIFT 检索

## 一句话职责

先用**全部社区报告**做一次全局「探路」——产出一个中间答案 + 一批后续问题；再把每个后续问题交给 local search 逐轮深化，每轮又问出新问题；最后把这一整片「问题-答案」图汇总成终答。

## 机制是什么

DRIFT（Dynamic Reasoning and Inference with Flexible Traversal）要解决的是 local 与 global 各自的结构性缺陷：

| 模式 | 强项 | 结构性缺陷 |
|---|---|---|
| local | 精确、可追溯到原文 | 只沿「问题里提到的实体」扩展，**看不到范围外的东西** |
| global | 覆盖全体、能回答宏观问题 | 只读社区报告，**给不出具体细节** |

**DRIFT 的假设是：好的答案需要「先知道该往哪找」**。所以它把 global 的覆盖能力用在一个前置的、只花一次的步骤上（`primer`），然后用 local 的精确性做多轮深化。

关键在于三处设计：

1. **primer 一次，但产出多个后续问题**。`PrimerResponse.follow_up_queries` 明确要求「Generate at least five good follow-up queries」。所以一次全局探路换来多条并行的局部路径。
2. **后续问题是「图上的节点」，而不是数组**。`QueryState` 用一个 `nx.MultiDiGraph` 存所有 `DriftAction`，每条边表示「这个问题的答案引出了那个问题」。所以进化的路径**可追溯**。
3. **medoid 式的收敛**：每轮只从「未完成」的 action 里取前 `drift_k_followups` 个，跑完标记完成。所以循环必然终止——`n_depth` 轮后或没有未完成 action 时停。

第三点的关键是 **`DriftAction.__hash__` 用 `query`**。所以「同一个问题字符串」在图里是同一个节点。这带来一个**隐含的去重**：若两个不同的 action 产生同样的后续问题，它们会指向同一个节点，第二个不会重复检索。

## 怎么实现的

### primer：HyDE 式查询扩展 + 多折并行分解

`primer.py` 里有**两个完全不同的 primer 机制**，这容易被忽略：

**第一个是 `PrimerQueryProcessor`，做的是 HyDE（Hypothetical Document Embeddings）。**

```python
async def expand_query(self, query: str) -> tuple[str, dict[str, int]]:
    template = secrets.choice(self.reports).full_content  # nosec S311
    prompt = (
        textwrap.dedent(f"""\
            Create a hypothetical answer to the following query: {query}

            Format it to follow the structure of the template below:

            """)
        + template
        + textwrap.dedent("""

            Ensure that the hypothetical answer does not reference new named entities that are not present in the original query.""")
    )
```

四处：

1. **`secrets.choice` 而不是 `random.choice`** —— 而 `# nosec S311` 注释说明「这不是安全用途」。用 `secrets` 是为了**避免 `random` 的全局状态被别的代码影响**（可复现性问题），不是因为需要密码学强度。这是一处有意思的取舍：`nosec` 明确告诉扫描器「我知道这不是安全场景」。

2. **随机取一份社区报告当「模板」** —— 目的是让模型生成的假想答案的**格式与真实报告一致**。所以这个随机选择不是为了多样性，而是为了「借一份真实文档的骨架」。

   **代价是结果不可复现**：同一 query 两次调用会借到不同的模板，于是假想答案不同、嵌入不同、检索结果不同。**这是一处刻意的非确定性**（而 c01 的 Leiden 用固定 seed 追求确定性，两者取向相反）。

3. **「Create a hypothetical answer to the following query」** —— 这就是 HyDE 的标准形态：让模型先编一个答案，再用它去检索。理由是「答案与答案的向量距离」比「问题与答案的向量距离」更近。

4. **「Ensure that the hypothetical answer does not reference new named entities that are not present in the original query」** —— 这条约束很关键：HyDE 的最大风险是模型编造实体，而那些编造的实体会把检索带偏。所以显式禁止引入新实体名。

**第二个是 `DRIFTPrimer`，做的是真正的查询分解。**

```python
async def decompose_query(self, query: str, reports: pd.DataFrame) -> tuple[dict, dict[str, int]]:
    community_reports = "\n\n".join(reports["full_content"].tolist())
    prompt = DRIFT_PRIMER_PROMPT.format(query=query, community_reports=community_reports)
    model_response = await self.chat_model.completion_async(
        messages=prompt, response_format=PrimerResponse,
    )
    parsed_response = model_response.formatted_response.model_dump()
```

四处：

1. **`"\n\n".join(reports["full_content"].tolist())`** —— 把若干整份报告（`full_content`，不是 `summary`）拼进 prompt。**所以 primer 的输入是社区报告的完整正文**，而不是摘要。这是「全局探路」的实际含义：让模型读完整的高层总结。

2. **`response_format=PrimerResponse` 传的是 Pydantic 类** —— 所以结构化输出由框架保证，不需要手写解析。这与 d03 的 global search（手写 `try_parse_json_object` + 三層校验）形成对比。

   **这是两代实现的差异**：DRIFT 用了结构化输出 API，global 用了「prompt 里写 JSON schema + 容错解析」。前者可靠得多。

3. **`PrimerResponse` 的三个字段描述里都藏着 prompt 指令**：

```python
intermediate_answer: str = Field(
    description="This answer should match the level of detail and length found in the community summaries. The intermediate answer should be exactly 2000 characters long. This must be formatted in markdown and must begin with a header that explains how the following text is related to the query.",
)
score: int = Field(
    description="A score on how well the intermediate answer addresses the query. A score of 0 indicates a poor, unfocused answer, while a score of 100 indicates a highly focused, relevant answer that addresses the query in its entirety."
)
follow_up_queries: list[str] = Field(
    description="A list of follow-up queries that could be asked to further explore the topic. These should be formatted as a list of strings. Generate at least five good follow-up queries."
)
```

   **这是一处非常重要的模式**：Pydantic `Field(description=...)` 的内容会被框架**作为 prompt 传给模型**。所以「字段说明」实际上是指令载体。

   - `optionally`：「exactly 2000 characters long」是一个**硬性的长度要求**（字符数，不是 token）。
   - `score`：0-100，且定义了端点语义。
   - `follow_up_queries`：**至少五个**。

   **把指令写在 schema 里而不是 prompt 里，好处是指令与字段绑定**（不会出现「prompt 说了但 schema 没这个字段」）。代价是 `intermediate_answer` 那句「2000 characters」这种硬约束容易与实际情况冲突（模型很难精确控制字符数）。

4. **`model_response.formatted_response.model_dump()`** —— 直接拿解析好的对象。**没有 `try_parse_json_object`，没有手写校验** —— 所以字段类型不对时框架会抛错，而不是静默降级。

**多折并行：**

```python
async def search(self, query: str, top_k_reports: pd.DataFrame) -> SearchResult:
    report_folds = self.split_reports(top_k_reports)
    tasks = [self.decompose_query(query, fold) for fold in report_folds]
    results_with_tokens = await tqdm_asyncio.gather(*tasks, leave=False)
```

```python
def split_reports(self, reports: pd.DataFrame) -> list[pd.DataFrame]:
    primer_folds = self.config.primer_folds or 1  # Ensure at least one fold
    if primer_folds == 1:
        return [reports]
    return [
        reports.iloc[indices]
        for indices in np.array_split(np.arange(len(reports)), primer_folds)
    ]
```

四处：

1. **`primer_folds or 1`** —— 把 `0`/`None` 都兜底成 1。所以「0 折」不会被当成「不分折」而是「一折」。
2. **`np.array_split`** —— 它允许**不等长分割**（比 `np.split` 更宽容），且**不报错**若折数多于行数（会产生空折）。
3. **空折会传给 `decompose_query`** → `"\n\n".join([])` = `""` → prompt 里 `community_reports` 是空串。**所以模型会在没有报告的情况下被要求分解查询。** 这不是错误但会产出低质结果。
4. **每折独立产出一份 `PrimerResponse`** —— 所以 N 折就有 N 组 `follow_up_queries`。而 `search` 返回 `response=[response for response, _ in results_with_tokens]`（一个列表）。

**所以 primer 的实际产出是「N 组中间答案 + N×5 个后续问题」**，而不是「一个中间答案 + 5 个后续问题」。

### `_process_primer_results`：把 N 份响应合成一个 action

```python
def _process_primer_results(self, query: str, search_results: SearchResult) -> DriftAction:
    response = search_results.response
    if isinstance(response, list) and isinstance(response[0], dict):
        intermediate_answers = [i["intermediate_answer"] for i in response if "intermediate_answer" in i]
        if not intermediate_answers:
            raise RuntimeError("No intermediate answers found in primer response. ...")
        intermediate_answer = "\n\n".join([
            i["intermediate_answer"] for i in response if "intermediate_answer" in i
        ])
        follow_ups = [fu for i in response for fu in i.get("follow_up_queries", [])]
        if not follow_ups:
            raise RuntimeError("No follow-up queries found in primer response. ...")
        score = sum(i.get("score", float("-inf")) for i in response) / len(response)
        response_data = {
            "intermediate_answer": intermediate_answer,
            "follow_up_queries": follow_ups,
            "score": score,
        }
        return DriftAction.from_primer_response(query, response_data)
    raise ValueError("Response must be a list of dictionaries.")
```

七处：

1. **列表推导写了两遍** —— `intermediate_answers` 算了一次，只为判空；然后又算一次拼 `"\n\n".join(...)`。**这是一处重复计算**（`intermediate_answers` 变量除了判空完全没用）。

2. **中间答案用 `\n\n` 拼** —— 所以 N 折的答案被串成一段。而 `PrimerResponse` 的描述要求「must begin with a header」，所以拼接后会有 N 个标题。

3. **score 是算术平均** —— `sum(...) / len(response)`。所以多折的评分被平均掉。

   **`i.get("score", float("-inf"))`** —— 缺 score 时用负无穷，于是平均结果被拖成负无穷。**这是一处严厉的兜底**：任一折缺 score 就让整体分数变成 `-inf`。

   而 `-inf` 在 `rank_incomplete_actions` 里会被排到最后。

4. **`follow_ups` 用嵌套推导展平** —— `[fu for i in response for fu in i.get("follow_up_queries", [])]`。所以 N×5 = 至少 5N 个后续问题（若每折都给了 5 个）。

5. **两个 `raise RuntimeError`** —— 「没有中间答案」与「没有后续问题」都硬失败。所以 primer 完全失效会让整个 DRIFT 查询失败。

   **注意「没有中间答案」的检查与「`intermediate_answer` 键是否存在」不同**：若键存在但值是空串，检查通过（因为列表里有元素），`"\n\n".join` 得到空串，最终 `DriftAction.answer = ""`。而 `is_complete` 判的是 `answer is not None`，所以**空串算「已完成」**。

6. **`isinstance(response[0], dict)` 用 `[0]`** —— 若 `response` 是空列表会 `IndexError`。而空列表是可能的（`primer_folds=0` 被兜底成 1，所以至少一折；但 `results_with_tokens` 为空不可能）。所以实际上是安全的，但依赖「至少一折」。

7. **`DriftAction.from_primer_response(query, response_data)`** —— 用 `query`（原始问题）当 action 的 `query`。所以**根节点的问题就是用户的问题**。

### 主循环：medoid 式的收敛

```python
epochs = 0
while epochs < self.context_builder.config.n_depth:
    actions = self.query_state.rank_incomplete_actions()
    if len(actions) == 0:
        logger.debug("No more actions to take. Exiting DRIFT loop.")
        break
    actions = actions[: self.context_builder.config.drift_k_followups]
    llm_call_offset += (
        len(actions) - self.context_builder.config.drift_k_followups
    )
    results = await self._search_step(
        global_query=query, k_followups=self.context_builder.config.drift_k_followups,
        search_engine=self.local_search, actions=actions,
    )
    for action in results:
        self.query_state.add_action(action)
        self.query_state.add_all_follow_ups(action, action.follow_ups)
    epochs += 1
```

五处：

1. **`rank_incomplete_actions()` 不带 scorer** —— 所以走的是 `random.shuffle` 分支！

```python
def rank_incomplete_actions(self, scorer=None) -> list[DriftAction]:
    unanswered = self.find_incomplete_actions()
    if scorer:
        for node in unanswered:
            node.compute_score(scorer)
        return sorted(unanswered, key=lambda node: (node.score if node.score is not None else float("-inf")), reverse=True)
    # shuffle the list if no scorer
    random.shuffle(unanswered)
    return list(unanswered)
```

   **所以 DRIFT 的探索顺序是随机的**，不是按分数贪心。而 `action.score` 字段（来自 primer 与 local 的 `score`）**不参与选择**。

   **这是一个重要的实现事实**：`score` 只在 `serialize` 里被输出（用于 reduce 时参考），不参与遍历顺序决策。所以 DRIFT 的「D」（Dynamic）实际上体现为**随机广度扩展**，而不是「按分数优先探索」。

   `random.shuffle` 用的是全局随机源，没有 seed——所以**同一次查询两次运行会探索不同的路径**。这是第二处刻意的非确定性（第一处是 `secrets.choice` 选模板）。

2. **`llm_call_offset` 是死代码** —— 它被初始化、被 `+=`，**但从未被读取**。而它算的表达式 `len(actions) - drift_k_followups` 在 `actions` 已经被切到最多 `drift_k_followups` 之后求值，所以结果恒 ≤ 0。**这是一处明显的遗留代码**。

3. **`actions[: drift_k_followups]` 每轮最多处理 k 个** —— 而 `drift_k_followups` 同时是「primer 检索的报告数」与「每轮的 action 数」。**一个配置项管两件不相干的事**：在 `drift_context.build_context` 里它是 `nlargest(k, "similarity")` 的 k，在这里是每轮 action 数。

4. **`epochs += 1` 无条件递增** —— 所以「没有新后续问题」也会消耗一轮。若所有 action 都完成而 `n_depth` 还没用完，下一轮 `rank_incomplete_actions()` 返回空就 `break`。所以是安全的。

5. **`add_all_follow_ups` 在 `add_action` 之后** —— 顺序有意义：`add_action` 加节点，`add_all_follow_ups` 加子节点与边。

### `QueryState`：把 action 图当数据结构用

```python
class QueryState:
    def __init__(self):
        self.graph = nx.MultiDiGraph()
```

**用 `MultiDiGraph`（有向多重图）** —— 所以两个 action 之间可以有多条边（虽然 `relate_actions` 只在 follow-up 时加一条）。而这带来一个问题：

```python
edges: list[dict[str, Any]] = [
    {
        "source": node_to_id[u],
        "target": node_to_id[v],
        "weight": edge_data.get("weight", 1.0),
    }
    for u, v, edge_data in self.graph.edges(data=True)
]
```

**这是错的**：`MultiDiGraph.edges(data=True)` 在 networkx 2.x/3.x 里对多重图会返回 `(u, v, key, data)` 四元组（默认 `keys=False` 时返回 `(u, v, data)` 但有重复），而这里解包成三个。

**实际上 networkx 的 `MultiDiGraph.edges(data=True)` 默认返回 `(u, v, data)` 并对每条边各产出一次** —— 所以解包是对的，但**同一条边若有多重边会产生重复项**且 `weight` 会是最后一次的。

**这是一处「用了多重图但按简单图处理」的不一致**。而因为 `relate_actions` 每次 `add_edge` 不加 `key`，networkx 会给新 key，所以**同一个 (parent, child) 对可以有多条边**——如果同一个 follow-up 被加两次。

而 `add_all_follow_ups` 里没有「边已存在就跳过」的检查。所以：

```python
def add_all_follow_ups(self, action, follow_ups, weight=1.0):
    if len(follow_ups) == 0:
        logger.warning("No follow-up actions for action: %s", action.query)
    for follow_up in follow_ups:
        if isinstance(follow_up, str):
            follow_up = DriftAction(query=follow_up)
        elif not isinstance(follow_up, DriftAction):
            logger.warning("Follow-up action is not a string, found type: %s", type(follow_up))
        self.add_action(follow_up)
        self.relate_actions(action, follow_up, weight)
```

四处：

1. **`if isinstance(follow_up, str)` 转换** —— 但 **`elif` 分支只 warning 不 `continue`**！所以一个既不是 `str` 也不是 `DriftAction` 的对象会**落到 `self.add_action(follow_up)`**，把非法对象加进图。然后 `add_node` 会接受它（networkx 接受任意可哈希对象）——但后续 `find_incomplete_actions` 里访问 `node.is_complete` 会 `AttributeError`。

   **这是一处真实的缺陷**：`elif` 的 warning 缺少 `continue`。

2. **`add_action` 对已存在的节点是幂等的**（networkx 的 `add_node` 对同 key 只是更新属性）—— 所以**重复的 follow-up 不会创建新节点**（因为 `DriftAction.__hash__` 用 query）。

3. **`logger.warning("No follow-up actions for action: %s", action.query)`** —— 每个无后续的 action 都 warning。在 `n_depth` 轮之后这可能刷屏。

4. **`self.relate_actions(action, follow_up, weight)`** —— 每次调用都加一条新边（多重图不合并）。所以同一个 (parent, child) 被处理两次就有两条边。

**`serialize` 的三处细节：**

```python
node_to_id = {node: idx for idx, node in enumerate(self.graph.nodes())}
nodes: list[dict[str, Any]] = [
    {
        **node.serialize(include_follow_ups=False),
        "id": node_to_id[node],
        **self.graph.nodes[node],
    }
    for node in self.graph.nodes()
]
```

三处：

1. **`include_follow_ups=False`** —— 避免递归序列化（子节点已经在图里了）。**这是必要的**：否则序列化会指数爆炸（每个节点带它的所有子孙）。

2. **`**self.graph.nodes[node]` 在最后展开** —— 所以图上的属性会**覆盖** `serialize()` 的结果。而 `add_action(action, metadata)` 的 metadata 会存到节点属性。当前调用都是 `add_action(action)` 无 metadata，所以是空字典、无覆盖。

3. **`node_to_id` 用 object 当键** —— 因为 `DriftAction.__hash__` 用 query，所以两个不同对象但同 query 会映射到同一个 id。**这是对的**（它们本来就是同一个节点）。

**而 `context_data` 的构造有一个隐含的假设：**

```python
if include_context:
    context_data = {
        node["query"]: node["metadata"]["context_data"]
        for node in nodes
        if node["metadata"].get("context_data") and node.get("query")
    }
    context_text = str(context_data)
    return {"nodes": nodes, "edges": edges}, context_data, context_text
```

四处：

1. **判 `.get("context_data")` 却下标 `["context_data"]`** —— `if` 子句保证键存在且值真，所以下标安全。但**这是两处访问同一键**，可以合并。
2. **`context_text = str(context_data)`** —— 直接把 dict 的 `repr` 当文本！所以 `context_text` 是 Python 字典的字符串表示，不是给 LLM 读的内容。

   **这是一处明显的问题**：`SearchResult.context_text` 本应是「进上下文窗口的文本」，而这里是一个 Python `repr`。而 DRIFT 的最终答案来自 `_reduce_response`，它用的是 `responses` 里的 `answer` 字段，**不用 `context_text`**。所以这个字段在 DRIFT 里只是「调试用」。

3. **`node["metadata"]["context_data"]` 是全量 DataFrame** —— `DriftAction.search` 里 `self.metadata.update({"context_data": search_result.context_data})`。所以每个 action 都存一份完整上下文。

   **内存代价**：N 个 action 各存一份 `dict[str, DataFrame]`，而它们可能包含大量重复数据。

4. **`node["query"]` 当字典键** —— 所以同 query 的节点会互相覆盖（但它们本来就是同一节点，所以不会发生）。

**`action_token_ct` 假设 metadata 里有三个键：**

```python
def action_token_ct(self) -> dict[str, int]:
    llm_calls, prompt_tokens, output_tokens = 0, 0, 0
    for action in self.graph.nodes:
        llm_calls += action.metadata["llm_calls"]
        prompt_tokens += action.metadata["prompt_tokens"]
        output_tokens += action.metadata["output_tokens"]
```

**用 `[...]` 而不是 `.get()`** —— 所以在 `DriftAction.__init__` 里那三个键被初始化为 0 是必需的。而 `DriftAction.deserialize` 里 `action.metadata = data.get("metadata", {})` **会覆盖成空字典**（若序列化数据没带 metadata）—— 那之后调 `action_token_ct` 就会 `KeyError`。

**这是一处脆弱的耦合**：`deserialize` 与 `action_token_ct` 对 metadata 形状的假设不一致。

### `DriftAction`：两个特殊方法决定图的语义

```python
def __hash__(self) -> int:
    """Allow DriftAction objects to be hashable for use in networkx.MultiDiGraph.
    Assumes queries are unique.
    """
    return hash(self.query)

def __eq__(self, other: object) -> bool:
    if not isinstance(other, DriftAction):
        return False
    return self.query == other.query
```

三处：

1. **用 query 当身份** —— 所以「同一个问题」在图里只有一个节点。**这是 DRIFT 去重的全部机制**。

2. **「Assumes queries are unique」** —— 文档直说了这个假设。而**它不是可靠的**：两个不同 parent 很容易问出同一个字符串问题。那种情况下它们会指向同一节点，而**第二个 parent 的边也会连到它**——所以图会变成 DAG 而不是树。

   **这实际上是特性而非 bug**：多个 parent 指向同一 follow-up 意味着那个问题被多个路径需要，只需检索一次。这是**记忆化**。

3. **`__eq__` 与 `__hash__` 一致** —— 若只定义 `__eq__` 不定义 `__hash__`，Python 会把类变成不可哈希（`__hash__ = None`），则无法进图。

**而 `DriftAction.search` 里的容错设计值得单独看：**

```python
# Do not launch exception as it will roll up with other steps
# Instead return an empty response and let score -inf handle it
_, response = try_parse_json_object(search_result.response, verbose=False)

self.answer = response.pop("response", None)
self.score = float(response.pop("score", "-inf"))
self.metadata.update({"context_data": search_result.context_data})

if self.answer is None:
    logger.warning("No answer found for query: %s", self.query)
```

五处：

1. **注释说明了设计意图**：「不抛异常，因为它会随其他步骤一起滚上去」。所以一个 action 失败不该让整次 DRIFT 查询失败。

2. **`float(response.pop("score", "-inf"))`** —— `"-inf"` 是**字符串**，而 `float("-inf")` 得到负无穷。所以缺 score 时降级为最低分。

   **注意 `float()` 可能抛 `ValueError`** —— 若模型返回 `"score": "high"`，这里会抛。而注释说「不抛异常」，所以这是一个**未覆盖的路径**。

3. **`response.pop(...)` 两次** —— 从字典里删掉 `response` 与 `score` 两个键，剩下的就是 `follow_up_queries`。所以：

```python
self.follow_ups = response.pop("follow_up_queries", [])
if not self.follow_ups:
    logger.warning("No follow-up actions found for response: %s", response)
```

   **`response` 现在只剩「没有被 pop 掉的键」** —— 所以 warning 打印的是「剩下的东西」，而不是原始响应。**这是一处调试信息损失**。

4. **`self.follow_ups = response.pop(...)` 直接是字符串列表**，而 `DriftAction.__init__` 的 `follow_ups` 类型是 `list[DriftAction]`。所以**类型不一致** —— 但 `add_all_follow_ups` 会处理 `str`（转成 `DriftAction`）。

   而 `serialize(include_follow_ups=True)` 会对 `self.follow_ups` 调 `action.serialize()` —— 若它们是字符串会 `AttributeError`。**所以 `serialize` 的默认 `include_follow_ups=True` 在 `search` 之后会崩。** 而 `QueryState.serialize` 传的是 `False`，所以不触发。

5. **`self.metadata["llm_calls"] += 1` 而不是 `+= search_result.llm_calls`** —— 因为 local search 一次调用（虽然内部可能含 build_context 的 LLM 调用，那个被漏掉了）。

### reduce：把图里所有答案拼起来

```python
reduce_responses = []
if isinstance(responses, str):
    reduce_responses = [responses]
else:
    reduce_responses = [
        response["answer"]
        for response in responses.get("nodes", [])
        if response.get("answer")
    ]
```

**所以 reduce 的输入是「图上所有有答案的节点的 answer」** —— 顺序是 `nodes` 的顺序（即 `self.graph.nodes()` 的插入顺序）。

三处：

1. **没有排序、没有截断** —— 所以「先回答的 action」排在前面，且**全部塞进 prompt**。若图有 50 个 action，prompt 会很长。

   这与 d03 的 global search（按分数排序 + `max_data_tokens` 截断）形成鲜明对比。**DRIFT 的 reduce 没有预算控制。**

2. **`if response.get("answer")`** —— 过滤掉空答案。但 `answer` 可能是 `None`（`DriftAction.search` 里可能 pop 不到）。

3. **`isinstance(responses, str)` 分支** —— 处理 `stream_search` 传入的 `result.response[0]`（那是个字符串，因为 `stream_search` 里 `result.response = result.response[0]`）。

   **等等** —— `search(reduce=False)` 返回的 `response=response_state`（一个 dict）。所以 `stream_search` 里 `isinstance(result.response, list)` 为假，`result.response` 仍是那个 dict。而 `_reduce_response_streaming(responses=result.response, ...)` 收到 dict，走 `responses.get("nodes")` 分支。所以 `str` 分支实际上走不到。

   **这是一处防御性但不可达的分支。**

## 关键取舍

**源码事实：** DRIFT 分三步：primer（一次，用**全部** `full_content` 报告做全局探路）→ 多轮 action 搜索（每轮最多 `drift_k_followups` 个未完成 action，各交给 `LocalSearch`）→ reduce（把图上所有 answer 拼进 prompt）。

**源码事实：** `drift_k_followups` **一个配置项管两件不相干的事**：在 `drift_context.build_context` 里是报告相似度 `nlargest` 的 k，在 `search` 主循环里是每轮处理的 action 数。

**源码事实：** `rank_incomplete_actions()` 调用时**不传 scorer**，所以走 `random.shuffle` 分支——**探索顺序是随机的，`action.score` 不参与选择**。DRIFT 的「Dynamic」实际是随机广度扩展而非按分数贪心。用的是全局随机源、无 seed，所以同一查询两次运行路径不同。

**源码事实：** `PrimerQueryProcessor.expand_query` 是 HyDE：让模型编一个假想答案，且**随机取一份社区报告当格式模板**（`secrets.choice` 配 `# nosec S311`，理由是避开 `random` 的全局状态而非需要密码学强度）。所以 primer 有**两处刻意的非确定性**（模板随机 + 探索顺序随机），与 c01 的固定 seed 取向相反。

**源码事实：** HyDE prompt 显式约束「must not reference new named entities that are not present in the original query」——防止编造的实体把检索带偏。

**源码事实：** `DRIFTPrimer.decompose_query` 用 `response_format=PrimerResponse`（Pydantic 类）做结构化输出，直接 `formatted_response.model_dump()`，**没有手写解析与容错**（对比 d03 global search 的 `try_parse_json_object` + 三层校验）——两代实现的分野。

**源码事实：** `PrimerResponse` 的三个 `Field(description=...)` 就是 prompt 指令：`intermediate_answer` 要求「exactly 2000 characters」+ markdown + 以标题开头；`score` 定义 0/100 端点语义；`follow_up_queries` 要求「Generate at least five good follow-up queries」。

**源码事实：** `split_reports` 用 `primer_folds or 1`（`0`/`None` 兜底成 1）与 `np.array_split`（允许不等长、折数多于行数时产生空折）；空折会让 prompt 的 `community_reports` 是空串。

**源码事实：** primer 的多折产出是「N 组中间答案 + N×至少 5 个后续问题」；`_process_primer_results` 用 `"\n\n"` 拼所有中间答案、展平所有 follow-up、score 取**算术平均**（缺 score 时用 `-inf`，于是任一折缺失就整体 `-inf`）。

**源码事实：** `_process_primer_results` 里 `intermediate_answers` 列表推导**写了两遍**（一次仅判空、一次拼接），前者的变量除判空外未使用；`isinstance(response[0], dict)` 用 `[0]`，依赖「至少一折」。

**源码事实：** 空串答案算「已完成」——`is_complete` 判的是 `answer is not None`，而 `_process_primer_results` 只判「键存在」，所以 `intermediate_answer=""` 能通过检查并被标为完成。

**源码事实：** `llm_call_offset` 是**死代码**：初始化、`+=`，但从未被读取；且算式在 `actions` 已切到不超过 k 之后求值，结果恒 ≤ 0。

**源码事实：** `QueryState` 用 `nx.MultiDiGraph`，但 `relate_actions` 每次 `add_edge` 都不带 `key`，所以同一个 (parent, child) 被处理两次会有两条边；`serialize` 的 edges 用 `(u, v, data)` 三解包（对多重图会产出重复项）。

**源码事实：** `add_all_follow_ups` 的 `elif` 分支**只 warning 不 `continue`**，所以既非 `str` 也非 `DriftAction` 的对象会被 `add_action` 加进图，之后 `find_incomplete_actions` 访问 `node.is_complete` 会 `AttributeError`。

**源码事实：** `DriftAction.__hash__` 用 `query`（文档直说「Assumes queries are unique」），所以「同一问题字符串」在图里是同一节点——这带来**记忆化**（多个 parent 指向同一 follow-up，只检索一次），代价是「不同上下文下的同一问题被合并」。

**源码事实：** `QueryState.serialize` 用 `include_follow_ups=False` 避免递归爆炸；`context_text = str(context_data)` 是 **Python dict 的 repr**，不是给 LLM 的内容（而 DRIFT 的终答不用它，所以只是调试用）；`context_data` 的字典键是 `node["query"]` 而值是该 action 的**完整** `context_data` DataFrame（每个 action 存一份，内存代价高）。

**源码事实：** `DriftAction.search` 明说不抛异常（「it will roll up with other steps」），改用 `score = -inf` 让排序兜住；但 `float(response.pop("score", "-inf"))` 在 score 是非数字字符串时会抛 `ValueError`——这是未覆盖的路径。三次 `pop` 之后 `response` 只剩未识别的键，所以「No follow-up actions」的 warning 打印的是**残留内容而非原始响应**。

**源码事实：** `self.follow_ups` 在 `search` 之后是**字符串列表**（与 `__init__` 声明的 `list[DriftAction]` 不一致），靠 `add_all_follow_ups` 转换；所以 `serialize(include_follow_ups=True)` 在 `search` 之后会 `AttributeError`（`QueryState.serialize` 传 `False` 所以不触发）。

**源码事实：** `DriftAction.search` 里 `metadata["llm_calls"] += 1` 而非 `+= search_result.llm_calls`——**local search 的 `build_context` 内部 LLM 调用被漏计**。

**源码事实：** `action_token_ct` 用 `action.metadata["llm_calls"]` 直接下标，而 `DriftAction.deserialize` 里 `action.metadata = data.get("metadata", {})` 可能覆盖成空字典——两处对 metadata 形状的假设不一致。

**源码事实：** reduce 阶段把**所有**有答案的节点的 `answer` 按图节点插入顺序拼进 prompt，**没有排序、没有 token 预算截断**（对比 d03 的按分数排序 + `max_data_tokens`）；`isinstance(responses, str)` 分支实际不可达（`search(reduce=False)` 返回 dict）。

**架构推论：** DRIFT 的形态是「一次昂贵的全局探路 + 多轮廉价的局部深化」，而它相对于 global 的增量价值在**可追溯性**：`QueryState` 的图让「这个结论是从哪个问题问出来的」可查。但代价是 reduce 阶段**没有预算控制**——所有答案塞进 prompt，所以大图上的 DRIFT 会因上下文过长而退化。这是「可追溯性优先于成本控制」的取舍。

**架构推论：** 「用 `query` 当节点身份」这个选择把「记忆化」与「图结构」合并成了一件事。好处是同一个后续问题只检索一次；代价是「同一问题在不同上下文里应该有不同的答案」这个语义被丢弃（因为节点只有一个 `answer` 字段）。**判据是「问题的语义是否依赖上下文」**：这里假设不依赖，所以可以。若将来需要上下文敏感的答案，就得改成「(query, parent) 当身份」。

**架构推论：** `rank_incomplete_actions` 在没有 scorer 时随机打乱，这个默认行为决定了 DRIFT 的实际性质。它的表面理由是「没有分数可用」，但深层含义是**作者不认为「按 primer 分数贪心」优于「随机覆盖」**。这与 d03 的 `shuffle_data=True` 是同一取向：**广度均匀优于深度优先**——因为一旦贪心，探索会集中在少数路径上，覆盖面就退化了。

**架构推论：** `elif` 缺 `continue` 这个缺陷很典型：`isinstance` 链的第二支通常做「转换」，第三支做「拒绝」。而这里第三支只记日志不返回，于是流程继续用非法值。**这类缺陷不会在正常路径暴露**（模型总会返回字符串），只在构造特殊输入时出现——所以很难通过测试发现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/query/structured_search/drift_search/search.py` | `DRIFTSearch` | 三步编排 + medoid 主循环 |
| `packages/graphrag/graphrag/query/structured_search/drift_search/search.py` | `DRIFTSearch.init_local_search` | 用 DRIFT 配置装配内部 `LocalSearch` |
| `packages/graphrag/graphrag/query/structured_search/drift_search/search.py` | `DRIFTSearch._process_primer_results` | 多折响应 → 单个根 action |
| `packages/graphrag/graphrag/query/structured_search/drift_search/search.py` | `DRIFTSearch._search_step` | 并发执行一批 action |
| `packages/graphrag/graphrag/query/structured_search/drift_search/search.py` | `DRIFTSearch._reduce_response` | 拼所有 answer → 终答（无预算控制） |
| `packages/graphrag/graphrag/query/structured_search/drift_search/primer.py` | `PrimerResponse` | 结构化输出 schema（description 即指令） |
| `packages/graphrag/graphrag/query/structured_search/drift_search/primer.py` | `PrimerQueryProcessor.expand_query` | HyDE 假想答案 + 随机模板 |
| `packages/graphrag/graphrag/query/structured_search/drift_search/primer.py` | `DRIFTPrimer.decompose_query` | 全量 `full_content` → 分解 + 后续问题 |
| `packages/graphrag/graphrag/query/structured_search/drift_search/primer.py` | `DRIFTPrimer.split_reports` | `np.array_split` 分折（可能空折） |
| `packages/graphrag/graphrag/query/structured_search/drift_search/state.py` | `QueryState` | `MultiDiGraph` 存 action |
| `packages/graphrag/graphrag/query/structured_search/drift_search/state.py` | `QueryState.rank_incomplete_actions` | 无 scorer 时 `random.shuffle` |
| `packages/graphrag/graphrag/query/structured_search/drift_search/state.py` | `QueryState.add_all_follow_ups` | `elif` 缺 `continue` |
| `packages/graphrag/graphrag/query/structured_search/drift_search/state.py` | `QueryState.serialize` | `include_follow_ups=False` 防递归；`context_text` 是 dict repr |
| `packages/graphrag/graphrag/query/structured_search/drift_search/state.py` | `QueryState.action_token_ct` | 直接下标 metadata（与 `deserialize` 假设不一致） |
| `packages/graphrag/graphrag/query/structured_search/drift_search/action.py` | `DriftAction` | 问题/答案/分数/后续 + 元数据 |
| `packages/graphrag/graphrag/query/structured_search/drift_search/action.py` | `DriftAction.__hash__` / `__eq__` | 用 `query` 当图节点身份（记忆化） |
| `packages/graphrag/graphrag/query/structured_search/drift_search/action.py` | `DriftAction.search` | `-inf` 兜底、三次 `pop`、`follow_ups` 变字符串列表 |
| `packages/graphrag/graphrag/query/structured_search/drift_search/drift_context.py` | `DRIFTSearchContextBuilder` | 内部持有 `LocalSearchMixedContext` |
| `packages/graphrag/graphrag/query/structured_search/drift_search/drift_context.py` | `DRIFTSearchContextBuilder.build_context` | HyDE → 余弦相似度 → top-k 报告 |
| `packages/graphrag/graphrag/query/structured_search/drift_search/drift_context.py` | `DRIFTSearchContextBuilder.check_query_doc_encodings` | 嵌入类型/长度兼容性检查 |
| `packages/graphrag/graphrag/config/models/drift_search_config.py` | `DRIFTSearchConfig` | `n_depth` / `drift_k_followups` / `primer_folds` 等 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：DRIFT 检索](https://microsoft.github.io/graphrag/query/drift_search/)
- [论文：HyDE——用假想文档做零样本检索](https://arxiv.org/abs/2212.10496)
- [graphrag 论文（Edge et al., 2024）](https://arxiv.org/abs/2404.16130)

## 与主核 / AI 的关系

DRIFT 的「探路 → 深化」循环与主核（多模态数据库内核）的**查询优化器的计划搜索**是同构的：两者都在「用一次便宜的探索决定后续昂贵的执行」。但优化器有**成本模型**（选择性估计、页数估计），所以能按代价排序候选计划；DRIFT 只有 `score`（一个模型打的分），而且**连这个分都没用在选择上**——它随机挑。

**这个差别不是疏忽而是判断**：成本模型的估计在新数据上会错，而 DRIFT 面对的「哪个后续问题更值得问」根本无法估计。所以随机覆盖是「不假装知道优先级」的做法。**判据是「是否存在可校准的代价信号」**：有则贪心，无则均匀覆盖。

第二处对照是 `QueryState` 的 `MultiDiGraph` 与主核的**查询计划树**。计划树是 DAG，且算子可以共享（同一个子查询只算一次）；DRIFT 的 action 图**也**通过 `__hash__ = query` 实现了共享。**这是两种「避免重复工作」的机制**：主核靠引用计数与物化，DRIFT 靠身份去重。

第三处是 `DriftAction.search` 的「不抛异常、用 `-inf` 兜底」与主核的**错误行 vs 失败行**。主核在 `ON CONFLICT DO NOTHING` 这类语义下也倾向「记录但不中断」；但它会把错误放进 `ErrorData` 结构里保留全部信息。而这里 `-inf` 是一个**信息量为零的哨兵**——事后无法知道「这条 action 到底是模型答不出还是调用失败」。**判据是「失败是否需要被区分处理」**：需要则保留结构化错误，不需要则哨兵够用。

对 RAG 工程的实际意义在于：DRIFT 是 graph-RAG 里唯一「用图做推理路径」的模式，而它的成本是 local 的 `n_depth × k` 倍。这份实现给出的三条经验可直接用：**全局探路只做一次但要多产出几个方向**（primer 要求至少 5 个后续问题）、**后续问题要用身份去重来避免重复检索**（hash 用 query）、以及**reduce 阶段必须有预算控制**（这是它当前的短板——所有答案无截断塞进 prompt）。
