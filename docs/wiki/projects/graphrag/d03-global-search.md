---
project: graphrag
feature: global-search
title: 全局检索与社区报告
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/query/structured_search/global_search/search.py
    symbols: [GlobalSearch, GlobalSearchResult]
  - path: packages/graphrag/graphrag/query/structured_search/global_search/community_context.py
    symbols: [GlobalCommunityContext]
  - path: packages/graphrag/graphrag/prompts/query/global_search_map_system_prompt.py
    symbols: [MAP_SYSTEM_PROMPT]
---

# 全局检索与社区报告

## 一句话职责

把社区报告切成若干批、每批独立问一次 LLM 得到「带重要性评分的要点列表」，再收集所有要点、**丢掉评分为 0 的**、按分数降序拼成一份报告数据，让 reduce 阶段的模型据此产出最终答案。

## 机制是什么

global search 要回答「这个数据集的主要主题是什么」这类问题。它面对一个无法回避的算术问题：**社区报告总量远超一次上下文窗口**。

一个万级实体的图可能产出几千份社区报告。所以必须分批。而分批带来第二个问题：**每批的答案如何合成一个**。

graphrag 的答案是 **map-reduce，但中间结果不是自由文本，而是「带分数的要点列表」**。这个选择是整页的核心：

- 若 map 输出自由文本，reduce 就只能「读 N 段文字再总结」——无法丢弃不相关的内容。
- 而输出**结构化要点 + 重要性评分**，reduce 就能**筛选**：丢掉评分为 0 的，按分数排序，只保留最高的那几个直到预算用完。

**所以评分机制的真实用途不是「排序」，而是「压缩时能丢东西」。** 这是 map-reduce 在 RAG 里能工作的关键——它给了 reduce 阶段一个明确的可丢弃判据。

第二个关键点是 **map 阶段的 `NO_DATA_ANSWER` 短路**：如果所有批次的要点都是 0 分，直接返回一句「我不知道」，**不再调 reduce**。这既省一次调用，也避免了「模型拿着空数据硬编」的风险。

## 怎么实现的

### `GlobalSearchResult`：在基类上加三样东西

```python
@dataclass(kw_only=True)
class GlobalSearchResult(SearchResult):
    map_responses: list[SearchResult]
    reduce_context_data: str | list[pd.DataFrame] | dict[str, pd.DataFrame]
    reduce_context_text: str | list[str] | dict[str, str]
```

三处：

1. **`kw_only=True`** —— 因为子类要加必填字段而父类字段全有默认值。若不 `kw_only`，Python 会报「非默认参数跟在默认参数之后」。**这是 dataclass 继承的一个具体限制**，而 `kw_only` 是它的标准解法。

2. **`map_responses`** —— 每一批的中间结果都保留下来（含各自的 token 统计）。所以调试时能看到「第 3 批为什么没贡献要点」。

3. **`reduce_context_data` / `reduce_context_text` 与基类的 `context_data` / `context_text` 是两回事**：基类那对是 map 阶段的输入（社区报告表），新加这对是 reduce 阶段的输入（要点汇编）。**所以一个结果里有两层上下文**。

**这是一处容易被误用**：父类的 `context_data` 在 global search 里指「喂给 map 的社区报告」，而不是「最终用的上下文」。要看最终上下文得读 `reduce_context_text`。

### 构造器：map 与 reduce 的参数完全分开

```python
def __init__(self, model, context_builder, tokenizer=None,
             map_system_prompt=None, reduce_system_prompt=None,
             response_type="multiple paragraphs", allow_general_knowledge=False,
             general_knowledge_inclusion_prompt=None, json_mode=True,
             callbacks=None, max_data_tokens=8000,
             map_llm_params=None, reduce_llm_params=None,
             map_max_length=1000, reduce_max_length=2000,
             context_builder_params=None, concurrent_coroutines=32):
```

四处：

1. **两套 `*_llm_params` 与两个 `*_max_length`** —— 因为 map 与 reduce 的任务性质不同：map 要「短而有结构」（1000 词、JSON 模式），reduce 要「长而有叙述」（2000 词、流式）。

2. **`max_data_tokens=8000` 只管 reduce 阶段的输入** —— 注意它**不是** `context_builder_params` 里的 `max_context_tokens`。后者管「社区报告怎么分批」，前者管「要点汇编能有多长」。**两个预算在两个阶段，互不相关。**

3. **`json_mode=True` 时的处理方式很值得看**：

```python
if json_mode:
    self.map_llm_params["response_format_json_object"] = True
else:
    # remove response_format key if json_mode is False
    self.map_llm_params.pop("response_format", None)
```

   **两个分支做的事不对称**：真分支**加**键，假分支**删**键。而假分支删的是 `"response_format"`（不带 `_json_object` 后缀），真分支加的是 `"response_format_json_object"`。

   **这是两个不同的键名**，所以「`json_mode=False` 时删 `response_format`」并不能撤销「`json_mode=True` 时加的 `response_format_json_object`」——但如果 `map_llm_params` 里本来就有 `response_format`，它会被删掉。

   需要记的是：**这个开关是「就地修改传入的字典」**（`self.map_llm_params = map_llm_params if map_llm_params else {}` 是引用而非拷贝）。所以调用方传进来的字典会被改。若同一个字典被两个引擎共用，第二个引擎会看到第一个改过的内容。

4. **`self.semaphore = asyncio.Semaphore(concurrent_coroutines)`** 在构造器里创建。这有个副作用：`Semaphore` 在 Python 3.10+ 不绑定事件循环，但在更早版本会绑定创建时的事件循环。所以**在事件循环外创建它并在循环内使用**是有风险的（旧版本）。这里默认 `32`，而 `asyncio.gather` 会把所有批次一次性提交，由信号量限流。

### `_map_response_single_batch`：三层降级

```python
async def _map_response_single_batch(self, context_data, query, max_length, **llm_kwargs) -> SearchResult:
    start_time = time.time()
    search_prompt = ""
    try:
        search_prompt = self.map_system_prompt.format(context_data=context_data, max_length=max_length)
        messages_builder = (
            CompletionMessagesBuilder().add_system_message(search_prompt).add_user_message(query)
        )
        async with self.semaphore:
            model_response = await self.model.completion_async(
                messages=messages_builder.build(), response_format_json_object=True, **llm_kwargs,
            )
            search_response = await gather_completion_response_async(model_response)
            logger.debug("Map response: %s", search_response)
        try:
            processed_response = self._parse_search_response(search_response)
        except ValueError:
            logger.warning("Warning: Error parsing search response json - skipping this batch")
            processed_response = []
```

五处：

1. **`search_prompt = ""` 在 `try` 之前初始化** —— 与 d02 的 local search 同样的手法：为了让 `except` 分支能安全 `encode`。

2. **`response_format_json_object=True` 硬编码** —— 即使构造器里 `json_mode=False` 也传 `True`。所以**`json_mode=False` 实际上只删了 `response_format` 键，并没有关掉 JSON 模式**。

   **这是一处真实的矛盾**：构造器那个分支（`pop("response_format")`）与这里的硬编码 `True` 方向相反。结果是 `json_mode` 这个参数对 map 阶段**没有实际效果**（只有可能影响传入的其他 `response_format` 变体）。

3. **`async with self.semaphore` 只包住 LLM 调用** —— prompt 构造与 JSON 解析在锁外。所以并发上限精确地作用在「同时在飞的请求数」上。

4. **解析失败降级为 `[]`（空列表）而不是哨兵对象** —— 而 `processed_response = []` 意味着「这批没有要点」。相比之下**外层异常**降级为 `[{"answer": "", "score": 0}]`（一个 0 分要点）。

   **两种降级的结果等价**（都贡献不出有价值的要点），但形状不同。这让下游的 `_reduce_response` 需要同时处理两种形状——它用 `isinstance(element, dict)` 与键存在性检查兜住。

5. **`llm_calls=1` 硬编码** —— 与 d02 相同的问题：统计是「假设成功调用一次」。而它至少设置了 `llm_calls_categories` 为 `None`（继承默认）。

### `_parse_search_response`：三层校验，每层返回不同的兜底

```python
def _parse_search_response(self, search_response: str) -> list[dict[str, Any]]:
    search_response, j = try_parse_json_object(search_response)
    if j == {}:
        return [{"answer": "", "score": 0}]

    parsed_elements = json.loads(search_response).get("points")
    if not parsed_elements or not isinstance(parsed_elements, list):
        return [{"answer": "", "score": 0}]

    return [
        {"answer": element["description"], "score": int(element["score"])}
        for element in parsed_elements
        if "description" in element and "score" in element
    ]
```

五处：

1. **`try_parse_json_object` 先清洗再解析，返回 `(text, dict)` 两件** —— `j == {}` 表示「清洗后仍解析不出」。**用空字典当失败信号**（而不是 `None`），所以「合法的空对象 `{}`」会被当作解析失败。这是一个可以接受的近似（合法的抽取结果不该是空对象）。

2. **`json.loads(search_response)` 又解析了一次** —— 而 `try_parse_json_object` 已经返回了解析好的 `j`。所以这里有**一次重复解析**。

   **这是一处明显的低效**（虽然 JSON 不大）。正确的写法是直接用 `j.get("points")`。

3. **`if not parsed_elements or not isinstance(parsed_elements, list)`** —— `not` 先判空（`None` / `[]` / `0` 都触发），再判类型。所以 `"points": []` 也走兜底。

4. **列表推导里的 `if "description" in element and "score" in element`** —— 逐项过滤。所以一个畸形的 points 列表会丢掉坏项、保留好项。**但 `int(element["score"])` 在过滤之后的表达式里**，所以 `score` 是字符串数字（`"85"`）能转，而非数字（`"high"`）会抛 `ValueError` —— 而外层 `try/except ValueError` 会捕到它，于是**整批降级为 `[]`**（丢掉所有好项）。

   **这是一处「一个坏项毁掉整批」的缺陷**：过滤条件只检查键存在，不检查值可转。

5. **`element["description"]` 直接下标** —— 因为 `if` 子句保证键存在。

### reduce 阶段：三段相同的逻辑被写了两遍

`_reduce_response`（非流式）与 `_stream_reduce_response`（流式）里，从「收集 key_points」到「拼 text_data」的这一整段是**逐行重复的**：

```python
key_points = []
for index, response in enumerate(map_responses):
    if not isinstance(response.response, list):
        continue
    for element in response.response:
        if not isinstance(element, dict):
            continue
        if "answer" not in element or "score" not in element:
            continue
        key_points.append({"analyst": index, "answer": element["answer"], "score": element["score"]})

filtered_key_points = [point for point in key_points if point["score"] > 0]
```

四处：

1. **`"analyst": index`** —— 所以每个要点都记住「来自第几批」。而 `index` 是 `enumerate(map_responses)` 的位置，**不是**社区的 id。所以「Analyst 3」这个标签对使用者没有语义，只是一批的编号。

   **这是一个命名问题**：`analyst` 暗示「某个分析者」，实际是「第 N 批社区报告」。而 prompt 里的示例也沿用了这个称呼。

2. **三层 `isinstance` / 键检查** —— 因为 map 阶段可能返回三种形状（正常列表、`[]`、`[{"answer": "", "score": 0}]`），且 `SearchResult.response` 是联合类型。

   **所以这里的防御是必要的**——它是在为 `_map_response_single_batch` 的两种降级兜底。

3. **`point["score"] > 0`** —— 过滤掉 0 分。而 prompt 里明确要求「An 'I don't know' type of response should have a score of 0」，所以这条过滤精确地丢掉「我不知道」的批次。

4. **`"analyst"` 用 `index` 而 `score` 用 `element["score"]`** —— 即批次的**顺序**信息保留了，但**社区的来源**信息丢了。所以最终答案里的引用 `[Data: Reports (...)]` 是模型根据 `data` 里的文本自己编的编号，**不是真的社区 id**。

   **这是一处真实的引用可靠性问题**：reduce 阶段的输入是「要点 + 分数」，不含社区 id。所以 prompt 要求模型写 `[Data: Reports (report ids)]` 但模型没有权威 id 可用。**它只能从 map 阶段答案里残留的数字里猜。**

**然后是那个短路：**

```python
if len(filtered_key_points) == 0 and not self.allow_general_knowledge:
    logger.warning(
        "Warning: All map responses have score 0 (i.e., no relevant information found from the dataset), "
        "returning a canned 'I do not know' answer. You can try enabling `allow_general_knowledge` to "
        "encourage the LLM to incorporate relevant general knowledge, at the risk of increasing hallucinations."
    )
    return SearchResult(
        response=NO_DATA_ANSWER, context_data="", context_text="",
        completion_time=time.time() - start_time,
        llm_calls=0, prompt_tokens=0, output_tokens=0,
    )
```

四处：

1. **条件是「无要点」** 且 **「不允许通用知识」** —— 所以开了 `allow_general_knowledge` 就会继续调 reduce（用空数据）。

2. **返回 `llm_calls=0`** —— 精确地表示「这一步没花钱」。这是正确的统计（与 local search 异常路径的硬编码 `1` 形成对比）。

3. **`context_data=""` 与 `context_text=""`** —— 因为确实没有汇编任何数据。注意这**丢掉了** map 阶段的上下文信息（虽然它已经返回给调用方了）。

4. **日志里给了可操作建议**：「You can try enabling `allow_general_knowledge` to encourage the LLM to incorporate relevant general knowledge, **at the risk of increasing hallucinations**」。

   **这是一处诚实的风险披露**：建议开了它但也说明代价。

**预算截断用「超了就 break」而不是「截断最后一个」：**

```python
data = []
total_tokens = 0
for point in filtered_key_points:
    formatted_response_data = [
        f"----Analyst {point['analyst'] + 1}----",
        f"Importance Score: {point['score']}",
        point["answer"],
    ]
    formatted_response_text = "\n".join(formatted_response_data)
    if (total_tokens + len(self.tokenizer.encode(formatted_response_text)) > self.max_data_tokens):
        break
    data.append(formatted_response_text)
    total_tokens += len(self.tokenizer.encode(formatted_response_text))
text_data = "\n\n".join(data)
```

五处：

1. **`break` 而不是 `continue`** —— 因为已按分数降序，后面的分数更低，既然放不下高分的就不必再试。**这是一处依赖排序的剪枝**。

2. **`tokenizer.encode` 被调用两次**（判断时一次、累加时一次）—— 同一字符串编码两遍。对每个要点都多一次编码。**这是一处可以省的开销**（把长度存进局部变量）。

3. **`"----Analyst {n}----"` 与 `"Importance Score: {score}"` 都是纯文本分隔符** —— 不是 JSON。所以 reduce 阶段的输入是「markdown 风格的文本块」，而 map 阶段的输出是 JSON。**两个阶段的数据形状完全不同**。

4. **`{point['analyst'] + 1}`** —— 显示时从 1 开始（`index` 从 0 开始）。所以「Analyst 1」对应第 0 批。

5. **`"\n\n".join(data)`** —— 段间空行。所以最终 `report_data` 是「若干 Analyst 块」。

**`allow_general_knowledge` 的实现是一行追加：**

```python
if self.allow_general_knowledge:
    search_prompt += "\n" + self.general_knowledge_inclusion_prompt
```

**所以它是「在 reduce 的 system prompt 末尾加一段指令」** —— 而 map 阶段不受影响。这解释了为什么短路条件里也要看这个开关：开了就不短路（因为有通用知识可用）。

**`_stream_reduce_response` 有一个参数传递的 bug：**

```python
response_search = await self.model.completion_async(
    messages=messages_builder.build(),
    stream=True,
    **llm_kwargs.get("model_parameters", {}),
)
```

而它的调用方（`stream_search`）传的是：

```python
async for response in self._stream_reduce_response(
    map_responses=map_responses, query=query, max_length=self.reduce_max_length,
    model_parameters=self.reduce_llm_params,
):
```

**所以 `llm_kwargs` 是 `{"model_parameters": {...}}`，而代码取 `llm_kwargs.get("model_parameters", {})`** —— 这看起来是对应的。

**但对比非流式版**：`_reduce_response(..., **self.reduce_llm_params)` —— 它是**展开**的，所以 `llm_kwargs` 直接就是 `reduce_llm_params` 的内容，然后 `completion_async(..., **llm_kwargs)`。

**两处传参方式不同**：流式版多包了一层 `model_parameters`。所以若 `reduce_llm_params` 里有 `temperature`，非流式会传 `temperature=...`，流式会传 `model_parameters={"temperature": ...}` 给 `completion_async` —— 后者是否等价取决于 `completion_async` 的签名。

**这是一处真实的接口不一致**，而它只在「流式与非流式对同一参数的处理路径」上体现。

### `MAP_SYSTEM_PROMPT`：同一段被写了两遍

与 d02 的 local prompt 相同的模式：`---Goal---` 到「Limit your response length」整段**出现两次**。

而两次的差别是**顺序不同**：

- 第一次：JSON 格式说明在**中间**（在引用说明之前），然后才是引用规则。
- 第二次：引用规则先，JSON 格式说明在**最后**。

**所以第二次把「输出格式」推到了最末尾** —— 与 local prompt 把「格式强化」推到末尾是同一个动机。

四处细节值得记：

1. **`{{` 与 `}}` 转义** —— 因为这是 `.format()` 模板，所以 JSON 的花括号要双写。**这是一处容易漏的细节**：直接写 `{` 会让 `format` 报「unexpected key」。

2. **评分定义是 `0-100` 的整数**，且明确「An 'I don't know' type of response should have a score of 0」。这把「拒答」编码成了可过滤的数值。

3. **`{max_length}` 说「words」而不是 tokens** —— 因为它是给模型看的，而模型没有 tokenizer。

   而构造器里 `map_max_length=1000` 的注释没有说单位。**所以「1000」是 1000 个词**，约 1300 token。

4. **示例里 `[Data: Reports (2, 7, 64, 46, 34, +more)]`** —— 与 local 的示例一致（都用 `+more` 标记溢出），但 global 只允许 `Reports` 一种数据集，而 local 允许 `Sources`/`Reports`/`Entities`/`Relationships`/`Claims`。

### `GlobalCommunityContext`：批次从这里产生

```python
community_context, community_context_data = build_community_context(
    community_reports=community_reports, entities=self.entities,
    tokenizer=self.tokenizer, use_community_summary=use_community_summary,
    column_delimiter=column_delimiter, shuffle_data=shuffle_data,
    include_community_rank=include_community_rank, min_community_rank=min_community_rank,
    community_rank_name=community_rank_name, include_community_weight=include_community_weight,
    community_weight_name=community_weight_name,
    normalize_community_weight=normalize_community_weight,
    max_context_tokens=max_context_tokens, single_batch=False,
    context_name=context_name, random_state=self.random_state,
)
```

**`single_batch=False` 是关键** —— 所以 `build_community_context` 返回的是一个 `list[str]`（多批），而 global search 的 `context_chunks` 就是这批列表，每个元素交给一个 map 任务。

四处：

1. **`shuffle_data=True`（默认）** —— 与 local 的 `shuffle_data=False` 相反。**为什么？** 因为 global 要「覆盖全体」，而按 rank 排序会让高排名报告集中在第一批、低排名的一直被截断。打散能保证每批都有高排名报告。

   **这是一处刻意的随机化**：它牺牲了「批次内的一致性」换「各批质量均匀」。而 `random_state=86` 让打散可复现。

2. **`include_community_weight=True` + `normalize_community_weight=True`** —— global 给每份报告加一个「权重」（基于社区规模/实体数），并归一化。而 local 不用这两个。

   **权重在 global 里是信号**：它让模型知道「这个社区有 200 个实体」还是「3 个实体」。而在 local 里社区是靠实体匹配选出来的，权重没有增量信息。

3. **`min_community_rank=0`** —— 默认不按 rank 过滤。

4. **对话历史被拼成 `context_prefix` 加到每一批前面**：

```python
context_prefix = f"{conversation_history_context}\n\n" if conversation_history_context else ""
final_context = (
    [f"{context_prefix}{context}" for context in community_context]
    if isinstance(community_context, list)
    else f"{context_prefix}{community_context}"
)
```

   **历史被复制到每一批** —— 所以 N 批就有 N 份历史。这是「map 阶段每批独立，但都要知道对话背景」的直接结果。

   **代价是历史 token 被计 N 次**。若历史有 500 token、20 批，就是 10000 token 的重复。

   **这是一个明确的成本/正确性权衡**：不复制的话，只有第一批知道历史，其他批的回答会忽略对话背景。

5. **`isinstance(community_context, list)` 的类型分支** —— 因为 `single_batch` 影响返回类型。这里传了 `False`，所以走列表分支。但这个 `else` 分支仍然保留着。

**动态社区选择的接线：**

```python
community_reports = self.community_reports
if self.dynamic_community_selection is not None:
    (community_reports, dynamic_info) = await self.dynamic_community_selection.select(query)
    llm_calls += dynamic_info["llm_calls"]
    prompt_tokens += dynamic_info["prompt_tokens"]
    output_tokens += dynamic_info["output_tokens"]
```

三处：

1. **`select` 是 `async`** —— 因为 d06 的动态选择会调 LLM。
2. **成本被计入 context builder 的统计** —— 所以 `build_context` 这一格的 `llm_calls` 不为 0（在用动态选择时）。
3. **`self.community_reports` 不被修改** —— 每次查询从全量开始筛选。所以同一引擎可并发查询（不像 d02 的 `attributes` 副作用）。

**而构造器里的 `pop` 手法有一处副作用：**

```python
if dynamic_community_selection and isinstance(dynamic_community_selection_kwargs, dict):
    self.dynamic_community_selection = DynamicCommunitySelection(
        community_reports=community_reports, communities=communities,
        model=dynamic_community_selection_kwargs.pop("model"),
        tokenizer=dynamic_community_selection_kwargs.pop("tokenizer"),
        **dynamic_community_selection_kwargs,
    )
```

**`pop` 会就地修改传入的字典** —— 所以 `model` 与 `tokenizer` 两个键被从调用方的字典里**删掉**了。若那个字典被复用（如两次构造），第二次会 `KeyError`。

**这是一处真实的副作用**，而 `factory.py`（d01）每次都新建 `{}` 再 update，所以当前不会触发。但它是一个脆弱的前提。

## 关键取舍

**源码事实：** `GlobalSearchResult` 用 `@dataclass(kw_only=True)` —— 因为子类要加必填字段而父类字段全有默认值，不 `kw_only` 会报参数顺序错误。

**源码事实：** `GlobalSearchResult` 有两层上下文：基类 `context_data`/`context_text` 是 **map 阶段的输入**（社区报告表），新增 `reduce_context_data`/`reduce_context_text` 才是 **reduce 阶段的输入**（要点汇编）。

**源码事实：** map 与 reduce 的参数完全分开：两套 `*_llm_params`、`map_max_length=1000`（单位是**词**，prompt 明说 `words`）、`reduce_max_length=2000`；`max_data_tokens=8000` 只管 reduce 输入，与 `context_builder_params` 里的 `max_context_tokens`（管分批）**互不相关**。

**源码事实：** `json_mode` 的实现是就地修改传入字典（`self.map_llm_params = map_llm_params` 是引用）：真分支加 `response_format_json_object`，假分支 `pop("response_format")`——**两个不同键名**，且 `_map_response_single_batch` 里硬编码 `response_format_json_object=True`，所以该参数对 map 阶段实际无效果。

**源码事实：** map 阶段有**两种降级**：JSON 解析失败 → `processed_response = []`（空列表）；外层异常 → `[{"answer": "", "score": 0}]`（一个 0 分要点）。两者语义等价但形状不同，所以 reduce 阶段要三层 `isinstance`/键检查兜住。

**源码事实：** `_parse_search_response` 里 `try_parse_json_object` 已返回解析好的 dict，却又 `json.loads(search_response)` **重复解析一次**；空字典被当作失败信号（所以合法的 `{}` 会被判失败）。

**源码事实：** `int(element["score"])` 在过滤条件**之后**执行，所以一个 `score="high"` 的坏项会抛 `ValueError` 被外层捕获，**整批降级为空**——一个坏项毁掉整批。

**源码事实：** reduce 阶段的关键操作是 `point["score"] > 0` 过滤 + 按分数降序 + 累积到 `max_data_tokens`。**过滤掉 0 分批次是评分机制的主要用途**（prompt 明确要求「I don't know」给 0 分），排序只用于「放不下就 `break`」的剪枝。

**源码事实：** 全 0 分且 `allow_general_knowledge=False` 时**短路**：返回 `NO_DATA_ANSWER`，`llm_calls=0`、`context_data=""`、`context_text=""`，不调 reduce。日志给出建议并附风险披露（「at the risk of increasing hallucinations」）。

**源码事实：** 要点标签 `"analyst": index` 是「第几批」而非社区 id，所以 reduce 阶段拿不到真实社区 id——而 prompt 仍要求模型写 `[Data: Reports (report ids)]`，模型只能从 map 答案里残留的数字猜。**这是一处引用可靠性问题。**

**源码事实：** 预算截断用 `break` 而非 `continue`（依赖已排序的剪枝）；`tokenizer.encode(formatted_response_text)` 每项调用**两次**（判长度 + 累加）。

**源码事实：** `allow_general_knowledge` 的实现只是给 reduce 的 system prompt 末尾追加一段指令，map 阶段不受影响。

**源码事实：** `_reduce_response` 与 `_stream_reduce_response` 里「收集 key_points → 过滤 0 分 → 排序 → 拼 text_data」整段**逐行重复**；两处传参方式不同（非流式 `**self.reduce_llm_params`，流式 `**llm_kwargs.get("model_parameters", {})`）——**同一参数走了两条不同路径**。

**源码事实：** `MAP_SYSTEM_PROMPT` 把 Goal 段写了两遍，第二次把 JSON 格式说明**移到最末尾**（与 d02 的 local prompt「末尾强化格式」同一动机）；JSON 花括号用 `{{ }}` 转义因为要过 `.format()`；示例只允许 `Reports` 一种数据集（local 允许五种）。

**源码事实：** `GlobalCommunityContext.build_context` 传 `single_batch=False`，所以返回 `list[str]` 多批；`shuffle_data=True`（与 local 的 `False` 相反）以保证「高排名报告分散到各批」而不是全部挤在前几批；`random_state=86` 让打散可复现。

**源码事实：** global 用 `include_community_weight=True` + `normalize_community_weight=True`（local 不用），因为「社区规模」在 global 里是信号、在 local 里没有增量信息。

**源码事实：** 对话历史被作为 `context_prefix` **复制到每一批**前面，所以 N 批有 N 份历史——不复制的话只有第一批知道对话背景。

**源码事实：** `DynamicCommunitySelection` 的构造用 `pop("model")` / `pop("tokenizer")` **就地修改传入字典**；每次查询从全量 `self.community_reports` 重新筛选，不改实例状态（与 d02 的 `attributes` 副作用相反，所以可并发）。

**架构推论：** 「map 输出结构化要点 + 重要性分数」这个选择是 global search 能工作的根本原因。自由文本的 map 输出只能被 reduce「再总结一遍」，而带分数的要点让 reduce 能做**筛选**——这是压缩的必要条件。**判据是「中间结果是否携带可判断丢弃的信息」**：若中间结果只能整体保留或整体丢弃，那分批只是把问题推迟；若每条都带可比较的质量信号，才能在合成时真正减量。

**架构推论：** 「全 0 分就短路」这个设计比它看起来更有价值。若不短路，模型会拿一份空数据被要求作答——而 prompt 里虽有「say so if you don't know」，但**空输入 + 强制作答**是幻觉的高发场景。短路把「无据可答」变成一个明确的、不花钱的返回值。而 `llm_calls=0` 的准确性（对比 d02 异常路径的硬编码 `1`）说明这条路是被认真设计过的。

**架构推论：** `shuffle_data=True` 是这一页最容易被忽略但影响很大的一处。它解决的是「按 rank 排序 + 分批截断」的偏差：高排名报告全进第一批，后面的批次全是低排名垃圾。打散让每批质量均匀，从而「任一批被丢弃」的损失是对称的。**代价是批次之间不再有质量梯度**——若未来想做「优先保证第一批质量」，打散就变成障碍。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/query/structured_search/global_search/search.py` | `GlobalSearch` | 检索编排：分批 map → 过滤 → reduce |
| `packages/graphrag/graphrag/query/structured_search/global_search/search.py` | `GlobalSearchResult` | 基类结果 + map 中间结果 + reduce 上下文 |
| `packages/graphrag/graphrag/query/structured_search/global_search/search.py` | `GlobalSearch._map_response_single_batch` | 单批 map（三层降级） |
| `packages/graphrag/graphrag/query/structured_search/global_search/search.py` | `GlobalSearch._parse_search_response` | 解析 points（三层校验 + 重复解析） |
| `packages/graphrag/graphrag/query/structured_search/global_search/search.py` | `GlobalSearch._reduce_response` | 过滤 0 分、排序、预算截断、短路 |
| `packages/graphrag/graphrag/query/structured_search/global_search/search.py` | `GlobalSearch._stream_reduce_response` | 流式 reduce（重复的收集逻辑 + 不同传参） |
| `packages/graphrag/graphrag/query/structured_search/global_search/community_context.py` | `GlobalCommunityContext` | 报告分批 + 历史前缀 + 可选动态选择 |
| `packages/graphrag/graphrag/query/structured_search/global_search/community_context.py` | `GlobalCommunityContext.build_context` | `single_batch=False` 产出 `list[str]` |
| `packages/graphrag/graphrag/prompts/query/global_search_map_system_prompt.py` | `MAP_SYSTEM_PROMPT` | JSON 要点 + 0-100 评分（Goal 段重复两次） |
| `packages/graphrag/graphrag/prompts/query/global_search_reduce_system_prompt.py` | `REDUCE_SYSTEM_PROMPT` | 按重要性汇编最终答案 |
| `packages/graphrag/graphrag/prompts/query/global_search_reduce_system_prompt.py` | `NO_DATA_ANSWER` | 全 0 分时的固定回答 |
| `packages/graphrag/graphrag/prompts/query/global_search_knowledge_system_prompt.py` | `GENERAL_KNOWLEDGE_INSTRUCTION` | 追加到 reduce prompt 的通用知识授权 |
| `packages/graphrag/graphrag/query/context_builder/community_context.py` | `build_community_context` | 多批渲染（global 用 `single_batch=False`） |
| `packages/graphrag/graphrag/query/llm/text_utils.py` | `try_parse_json_object` | 容错 JSON 抽取 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：全局检索](https://microsoft.github.io/graphrag/query/global_search/)
- [graphrag 论文（Edge et al., 2024）](https://arxiv.org/abs/2404.16130)
- [论文：Map-Reduce 式长文档问答](https://arxiv.org/abs/2005.00661)

## 与主核 / AI 的关系

map-reduce 的中间结果带分数，与主核（多模态数据库内核）的**并行查询的 partial aggregate**是同构的：两边都在「分片算中间结果、再合成」。而关键差别在于**中间结果是否可比较**。主核的聚合函数有明确语义（`count`/`sum`/`max`），所以 reduce 是数学上确定的；这里的「要点 + 0-100 分」没有客观语义——同一个要点的分数依赖模型当次的判断。**所以主核能把 reduce 做成确定性的，这里只能做成启发式的筛选。**

第二处对照是 `break` 剪枝与主核的**排序 + LIMIT 的下推**。两者都依赖排序做剪枝：主核的 `OrderBy + Limit` 可以在索引上只读前 N 条；这里因为要点已经在内存里排好序，所以「放不下就 break」等价于「按顺序取前若干条」。**判据是「能否按顺序增量消费」**——能则剪枝，不能则必须全量看完。

第三处是「历史复制到每一批」与主核的**广播变量 / 参数化计划的重复传参**。两者都是「一份共享输入要在多个并行单元里各用一次」。主核的解法是把公共参数放在**共享内存/计划节点**里只存一份（`ParamListInfo`）；这里因为每一批是独立的 LLM 调用（跨进程/跨网络），只能物理复制。**判据是「并行单元是否共享内存」**：不共享就必须复制，代价按份数线性增长。

对 RAG 工程的实际意义在于：global search 是 graph-RAG 相对向量 RAG 唯一能回答「全局问题」的路径，而它的成本结构由分批数决定。这份实现给出的三条经验可直接用：**中间结果要带可丢弃的质量信号**（分数让 reduce 能真正减量）、**全无相关时短路比硬答便宜且安全**、以及**分批要打散不要排序**（否则高质量内容集中在一批，截断损失不对称）。
