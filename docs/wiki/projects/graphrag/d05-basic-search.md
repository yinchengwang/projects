---
project: graphrag
feature: basic-search
title: 朴素向量检索
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/query/structured_search/basic_search/search.py
    symbols: [BasicSearch]
  - path: packages/graphrag/graphrag/query/structured_search/basic_search/basic_context.py
    symbols: [BasicSearchContext]
  - path: packages/graphrag/graphrag/prompts/query/basic_search_system_prompt.py
    symbols: [BASIC_SEARCH_SYSTEM_PROMPT]
---

# 朴素向量检索

## 一句话职责

用问题向文本单元向量库做一次相似度检索、取前 k 条、按 token 预算截断、拼成 CSV 表格塞进 prompt——**完全不碰图**。它的价值不在能力，而在于它是「图到底带来了多少增益」的对照组。

## 机制是什么

graphrag 提供 basic search 的理由只有一条：**没有基线就没有增益结论**。

一个有说服力的 graph-RAG 评测必须能回答「同样的文本、同样的模型、同样的 prompt 结构，只是不用图，效果差多少」。所以 basic search 的设计目标不是「好用」，而是**尽可能与 local search 可比**：

| 与 local search 一致的部分 | 与 local search 不同的部分 |
|---|---|
| 同一个 `BaseSearch` 基类与 `SearchResult` | 只检索文本单元，不检索实体/关系/社区 |
| 同样的 prompt 结构（Role / Goal / Data tables / 重复的 Goal） | `BASIC_SEARCH_SYSTEM_PROMPT` 只允许 `Sources` 一种数据集 |
| 同样的引用语法 `[Data: Sources (...)]` | 引用 id 来自**文本单元自己的 `short_id`** |
| 同样的 `+more` 溢出标记与 5 条上限 | 没有「实体映射 → 关系扩展」这一步 |

**「尽量对齐」这个目标解释了几乎所有的实现细节**。例如 prompt 的 Goal 段也重复两次（与 d02 完全同构）、也要求 `+more` 标记、也强调「id 不是 index」。这些都不是 basic search 自己需要的，而是「为了让对比公平」。

技术上 basic search 只有三步：

1. **相似度检索**：`similarity_search_by_text` 直接拿问题文本去搜。
2. **token 预算截断**：按行累加，超了就停。
3. **CSV 渲染**：`to_csv(sep=column_delimiter)`。

**第一步有个值得注意的细节**：它传的是 `text=query` 加一个 `text_embedder` 回调，而不是「先算 query 嵌入再搜」。所以**嵌入发生在向量库内部**——这保证了 query 与文档用同一个嵌入路径。

## 怎么实现的

### `BasicSearchContext.build_context`：一步检索

```python
def build_context(self, query, conversation_history=None, k=10, max_context_tokens=12_000,
                  context_name="Sources", column_delimiter="|",
                  text_id_col="id", text_col="text", **kwargs) -> ContextBuilderResult:
```

**七个参数里三个是列名与分隔符**（`text_id_col` / `text_col` / `column_delimiter`）—— 这说明「渲染成什么样的表格」被当成可配置的，而不是硬编码。

```python
if query != "":
    related_texts = self.text_unit_embeddings.similarity_search_by_text(
        text=query,
        text_embedder=lambda t: (
            self.text_embedder.embedding(input=[t]).first_embedding
        ),
        k=k,
    )
```

四处：

1. **`if query != ""` 的守卫** —— 空问题不检索，直接产出空 DataFrame。这避免了「空串嵌入」这种无意义调用。

2. **`text_embedder` 是 lambda，返回 `first_embedding`** —— 所以它把「嵌入器只回一条」这件事适配成向量库期待的签名。**这里假设 query 只产生一个嵌入**（没有多向量/多粒度）。

3. **`k=k` 默认 10** —— 而 local search 的 `top_k_mapped_entities` 也是 10、`top_k_relationships` 也是 10。所以三者的默认值被刻意对齐了。

4. **`similarity_search_by_text` 而不是 `similarity_search_by_vector`** —— 前者接受原始文本。所以嵌入用的模型由 `text_embedder` 决定，而**索引里的文档嵌入必须用同一个模型**（否则维度不匹配或语义空间不匹配）。这是一处隐式契约，没有校验。

**然后是一个容易看漏的转换：**

```python
text_unit_ids = {t.document.id for t in related_texts}
text_units_filtered = [
    {text_id_col: t.short_id, text_col: t.text}
    for t in self.text_units or []
    if t.id in text_unit_ids
]
related_text_df = pd.DataFrame(text_units_filtered)
```

五处：

1. **先用 `{t.document.id for t in related_texts}` 取 id 集合** —— 向量库返回的是带 `document` 的搜索结果，而 `document.id` 是**完整 id**。

2. **然后遍历 `self.text_units`（全量）按 id 过滤** —— 这是**为了拿到 `short_id` 与 `text`**：向量库里可能只存了 id + 向量，没有原文。

   **所以这里做了一次「向量库 → 内存对象」的 join**，而 join 键是完整 id。

3. **`self.text_units or []`** —— 因为构造时 `text_units` 可以是 `None`。所以「没给文本单元」时结果为空 DataFrame（而不是崩）。

4. **顺序问题**：`text_units_filtered` 的顺序是 **`self.text_units` 的顺序**，不是相似度顺序！

   **这意味着 basic search 的上下文里，文本单元的排列与其相关性无关。** 而截断是按这个顺序做的——所以「哪些块能进上下文」取决于它们在 `text_units` 列表里的位置，而不是相似度排名。

   **这是一处实实在在的缺陷**：`k` 条相似结果里，若文本单元列表很长且顺序与相关性无关，截断可能留下最不相关的那些。

   对比 local search 的 `_build_text_unit_context`：它显式 `sort(key=lambda x: (x[1], -x[2]))` 按实体顺序与关系数排序。而 basic search **没有对应的排序步骤**。

5. **`{text_id_col: t.short_id, ...}`** —— 用 `short_id` 而不是 `id`。所以 prompt 里让模型引用的「id」是短 id（人类可读的编号），不是哈希 id。

   **这与 d03 的对比很有意思**：global search 的 reduce 阶段拿不到真实社区 id，只能让模型猜；而 basic search 把 `short_id` 直接放进表格，所以引用是可靠的。

**token 预算循环：**

```python
current_tokens = 0
text_ids = []
current_tokens = len(self.tokenizer.encode(text_id_col + column_delimiter + text_col + "\n"))
for i, row in related_text_df.iterrows():
    text = row[text_id_col] + column_delimiter + row[text_col] + "\n"
    tokens = len(self.tokenizer.encode(text))
    if current_tokens + tokens > max_context_tokens:
        logger.warning(f"Reached token limit: {current_tokens + tokens}. Reverting to previous context state")
        break
    current_tokens += tokens
    text_ids.append(i)
```

六处：

1. **`current_tokens = 0` 紧接着被覆盖** —— 第一行赋值完全是死代码。

2. **初值计的是「表头」**（`id|text\n`）—— 因为 `to_csv` 会输出表头。**这是一处精确的细节**：预算把表头算进去了。

3. **`text_ids` 的名字误导** —— 它收集的是 **DataFrame 的 index**（`i`），不是文本 id 也不是 `short_id`。

4. **`row[text_id_col] + column_delimiter + row[text_col]`** —— 手工拼一行，**假设这两个值都是字符串**。若 `short_id` 是数字（如 `int`），这里会 `TypeError`。

5. **`break` 而不是 `continue`** —— 与 d03 的 reduce 相同：一旦放不下就不再看后续。而这里因为**没有按相关性排序**，`break` 的语义只是「前若干条能放下的」。

6. **`logger.warning` 里说「Reverting to previous context state」** —— 措辞是「回退到上一状态」，实际是「不含这一行」。措辞略有偏差但语义可懂。

**渲染成 CSV：**

```python
final_text_df = cast(
    "pd.DataFrame",
    related_text_df[related_text_df.index.isin(text_ids)].reset_index(drop=True),
)
final_text = final_text_df.to_csv(index=False, escapechar="\\", sep=column_delimiter)
```

三处：

1. **用 `index.isin(text_ids)` 筛选** —— 而 `text_ids` 就是连续的 index 前缀（因为循环是顺序的）。所以这等价于 `iloc[:len(text_ids)]`。**`isin` 是 O(n) 的，而 `iloc` 是 O(1) 的切片**——这是一处不必要的开销，但它对「顺序保证」不那么敏感（若将来改成非连续保留，`isin` 仍然对）。

2. **`reset_index(drop=True)`** —— 让输出的 CSV 行号连续。

3. **`to_csv(index=False, escapechar="\\", sep=column_delimiter)`** —— 用 `|` 当分隔符、`\` 当转义符。

   **这是一处必须的组合**：文本单元的正文里很可能含逗号，所以不能用 `,`；而含 `|` 的可能性小但非零，所以需要 `escapechar`。而 `to_csv` 遇到需要转义时会加引号或转义符。

   **但 `escapechar="\\"` 只在 `quoting=QUOTE_NONE` 时生效**。默认 `quoting=QUOTE_MINIMAL` 会用**引号**包裹含分隔符的字段，而 `escapechar` 只用于转义引号本身。所以带 `|` 的文本会被包成 `"a|b"`。

   这条 CSV 会被模型当作「表格」读——而带引号的字段在 markdown 里不直观。**这是一处格式上的将就**。

### `BasicSearch.search`：与 local search 几乎逐行相同

把 `basic_search/search.py` 与 `local_search/search.py`（d02）并排看，两者的 `search` 方法除了 prompt 格式化部分**几乎完全一致**：

- 都是 `search_prompt = ""` 预初始化
- 都是 `llm_calls/prompt_tokens/output_tokens` 三个字典
- 都是 `build_context` 后记 `build_context` 一格
- 都是 `stream=True` 再拼接
- 都是 `except Exception` 返回空 response

**差别只有三处**，而每一处都值得看：

**第一处：没有 `drift_query` 分支。**

```python
search_prompt = self.system_prompt.format(
    context_data=context_result.context_chunks,
    response_type=self.response_type,
)
```

**而 local search 有那个 `if "drift_query" in kwargs` 分支**（d02 讲过）。所以 basic search 不能被 DRIFT 复用——这符合设计（DRIFT 的每轮深化必须用图）。

**第二处：`llm_calls=1` 是硬编码而不是 `sum(llm_calls.values())`。**

```python
return SearchResult(
    response=response, ...,
    llm_calls=1,
    prompt_tokens=len(self.tokenizer.encode(search_prompt)),
    output_tokens=sum(output_tokens.values()),
    llm_calls_categories=llm_calls,
    prompt_tokens_categories=prompt_tokens,
    output_tokens_categories=output_tokens,
)
```

四处：

1. **`llm_calls=1`** —— 但 `llm_calls` 字典里可能有 `build_context` 与 `response` 两格。而 basic search 的 `build_context` 不调 LLM（它只做向量检索），所以 `context_result.llm_calls` 应该是 0。

   **而 `ContextBuilderResult` 的默认值就是 0** —— 所以 `llm_calls = {"build_context": 0, "response": 1}`，`sum` 也是 1。**所以这里硬编码 `1` 与实际相符**。

   但这是一处脆弱的一致性：若将来 `build_context` 加了 LLM 调用，这里的 `llm_calls` 就会少算，而 `_categories` 会多于总量。

2. **`prompt_tokens=len(self.tokenizer.encode(search_prompt))`** —— 注意它**不含 `build_context` 那一格**（那一格是 0，所以无损）。但**也不含 user message（`query`）**——

   **这是一处普遍的偏差**：所有模式的 `prompt_tokens` 都只算 system prompt，不算 user message。而 user message 虽然短，但它是真实消耗。所以「总量」是偏低的。

3. **`output_tokens=sum(output_tokens.values())`** —— 与 `llm_calls` 的硬编码不一致（这里用了 `sum`）。所以同一个 `SearchResult` 里，两个字段一个手算一个求和。

4. **`llm_calls_categories=llm_calls`** —— 所以明细是 `{"build_context": 0, "response": 1}`。而总量是 `1`。**两者一致**，但靠的是「`build_context` 恰好是 0」这个巧合。

**第三处：异常路径比 local search 多传了三个 `_categories`。**

```python
except Exception:
    logger.exception("Exception in _asearch")
    return SearchResult(
        response="", ...,
        llm_calls=1,
        prompt_tokens=len(self.tokenizer.encode(search_prompt)),
        output_tokens=0,
        llm_calls_categories=llm_calls,
        prompt_tokens_categories=prompt_tokens,
        output_tokens_categories=output_tokens,
    )
```

**而 d02 的 local search 异常路径没有这三个字段**（留 `None`）。

**所以 basic search 的异常路径在统计上更诚实**：它至少给出了 `{"build_context": 0, "response": 0}` 这样的明细，与 `llm_calls=1` 并列（尽管两者矛盾——明细之和是 0，总量说 1）。

**这是一处两页实现不一致的细节**，而它说明两份代码是分别演化的。

### 那个 `await` 遗漏

```python
response_stream: AsyncIterator[LLMCompletionChunk] = (
    self.model.completion_async(
        messages=messages_builder.build(),
        stream=True,
        **self.model_params,
    )
)
```

**注意这里没有 `await`** —— 而 `stream_search` 里同样的调用有：

```python
response_stream: AsyncIterator[LLMCompletionChunk] = await self.model.completion_async(
    messages=messages_builder.build(), stream=True, **self.model_params,
)
```

**`completion_async` 是 `async def`**，所以不 `await` 它得到的是一个**协程对象**，而不是 `AsyncIterator`。而 `async for chunk in response_stream` 对协程会抛 `TypeError`。

**这是一个真实的 bug** —— 除非 `completion_async` 返回的是「已经在内部启动的异步迭代器」而不是协程（即它是个返回 `AsyncIterator` 的普通函数，或者返回一个「awaitable 且 async-iterable」的对象）。

**从 `stream_search` 加了 `await` 而这里没有看，两者必有一个是多余的**。而不 `await` 协程会触发 `RuntimeWarning: coroutine was never awaited`。

**这是一处两页实现不一致的细节**，而它说明两份代码是分别演化的。

而那个 `# type: ignore` 注释正好在括号外——恰好把类型检查器的警告压掉了，所以静态检查发现不了。

### `BASIC_SEARCH_SYSTEM_PROMPT`：与 local 的三处差异

**差异一：数据集只有 `Sources`。**

```
"This is an example sentence supported by multiple data references [Data: Sources (record ids)]."
```

而 local 的示例是：

```
"[Data: Sources (15, 16), Reports (1), Entities (5, 7); Relationships (23); Claims (2, 7, 34, 46, 64, +more)]"
```

**所以 basic 的引用语法是 local 的子集**（只有一种数据集、没有分组分号）。这直接反映了「上下文里只有文本单元表」这个事实。

**差异二：id 的来源被说清楚了。**

```
where 1, 2, 3, 7, 34, 46, and 64 represent the source id taken from the "id" column in the provided tables.
```

**注意 `"id"` 带引号** —— 因为表格的表头就是 `id`（`text_id_col` 默认值）。**而 local 说「the id (not the index) of the relevant data record」**（更含糊，因为它的表格有多种）。

**这是一处更精确的措辞**，它依赖「表格列名固定为 `id`」这个事实。

**差异三：Goal 段第二次出现时把「tables」改成了「data」。**

第一次：
```
summarizing all relevant information in the input data tables appropriate for the response length and format
```

第二次：
```
summarizing all relevant information in the input data appropriate for the response length and format
```

**「data tables」→「data」** —— 只丢了 ` tables`。这大概是无意的笔误（复制粘贴时改了一句别的），但它不影响语义。

**而两次的滚动排列与 local 完全一致**：Role → Goal → Target → Data tables → Goal → Target（末尾多两行格式强化）。

## 关键取舍

**源码事实：** basic search 完全不用图：一次 `similarity_search_by_text` → token 预算截断 → `to_csv(sep="|")` → 一个 prompt。

**源码事实：** 它的设计目标是**与 local search 可比**（同一个 `BaseSearch`、同一个 `SearchResult`、同一套 prompt 骨架与引用语法、同样重复两次的 Goal 段），所以「对齐」解释了它的多数实现细节，而不是能力需求。

**源码事实：** `similarity_search_by_text` 接受原始文本 + `text_embedder` 回调（返回 `first_embedding`），所以嵌入发生在向量库内部，保证 query 与文档走同一路径；但「索引文档嵌入必须用同一模型」这条契约没有校验。

**源码事实：** 检索结果先转成 `{t.document.id ...}` 的 id 集合，再遍历**全量** `self.text_units` 按 id 过滤——这是为了拿到 `short_id` 与 `text`（向量库可能只存 id + 向量）。**顺序是 `self.text_units` 的顺序，不是相似度顺序**，而截断按这个顺序做——所以「哪些块进上下文」与相关性无关（对比 local search 有显式排序）。

**源码事实：** 表格里放的是 `short_id`（人类可读编号）而非哈希 id，所以 basic search 的引用是**可靠的**（对比 d03 的 global reduce 阶段拿不到真实 id，模型只能猜）。

**源码事实：** token 预算把「表头 `id|text\n`」计入初值（因为 `to_csv` 会输出表头）；`current_tokens = 0` 那行是死代码；`text_ids` 收集的是 **DataFrame 的 index**（名字误导）；`break` 而非 `continue`（但因为没有相关性排序，剪枝语义只是「前若干条能放下的」）。

**源码事实：** 筛选用 `index.isin(text_ids)`（等价于 `iloc[:n]` 但 O(n)）；`to_csv(index=False, escapechar="\\", sep="|")` 里 `escapechar` 只对 `quoting=QUOTE_NONE` 生效，默认 `QUOTE_MINIMAL` 会用引号包裹含 `|` 的字段——所以带分隔符的正文在「表格」里会变成带引号的字段。

**源码事实：** `BasicSearch.search` 与 `LocalSearch.search` 几乎逐行相同，差别三处：没有 `drift_query` 分支（所以不能被 DRIFT 复用）；`llm_calls=1` **硬编码**而 `output_tokens` 用 `sum(...)`（同一结果对象里两种算法）；异常路径**多传了三个 `_categories`**（d02 那边留 `None`）。

**源码事实：** `llm_calls=1` 与 `_categories` 之和一致只是巧合——因为 basic 的 `build_context` 恰好不调 LLM（`ContextBuilderResult` 默认 0）；若将来 `build_context` 加 LLM 调用，总量会少算。

**源码事实：** 所有模式的 `prompt_tokens` 都只算 **system prompt**，不含 user message（`query`）——所以总量偏低。

**源码事实：** `BasicSearch.search` 里 `self.model.completion_async(...)` **缺 `await`**（而 `stream_search` 里同样的调用有 `await`）；若它是 `async def`，不 `await` 得到的是协程对象，`async for` 会 `TypeError`，且会触发 `RuntimeWarning: coroutine was never awaited`。`# type: ignore` 恰好压掉了静态警告。

**源码事实：** `BASIC_SEARCH_SYSTEM_PROMPT` 与 local 的三处差异：引用只允许 `Sources` 一种数据集（是 local 语法的子集）；明说 id 来自「the `"id"` column」（带引号，因为表头固定）；第二次 Goal 段把「input data tables」写成「input data」（丢了 ` tables`，疑似笔误）。两次 Goal 段的排列与 local 完全一致（末尾多两行格式强化）。

**架构推论：** basic search 是一个**对照组**而非功能，所以评价它的标准不是「好不好用」而是「对比是否公平」。这份实现为此付出的代价是：多一份近乎重复的 `search` 实现（约 180 行）、多一份 prompt（约 73 行）、多一个上下文构建器。**判据是「基线是否值得维护成本」**：在 graph-RAG 这类「增益不显然」的技术里，基线的价值极高——没有它就无法证明图带来了什么。

**架构推论：** 「顺序丢失」这个缺陷恰好揭示了 basic search 与 local search 的一个本质差异：local 的检索路径**自带排序信号**（实体映射的相似度 → 实体顺序 → 文本单元顺序），而 basic 的检索只有「k 条结果」这一个信号，且它在 join 时丢了。**所以「有没有天然的排序依据」决定了实现能否简单**：local 天然有，basic 需要额外一步把相似度带过来——而它没做。

**架构推论：** `prompt_tokens` 只算 system prompt 这个偏差在所有模式里一致，说明它是**复制传播**的结果而非独立决定。这类偏差的危险在于「看起来对」：数值非零、随输入变化、单调增长——只有与实际的 API 上报对比才会发现偏低。**这类指标的正确性只能靠「与权威来源对账」来保证**，内部自洽没有用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/query/structured_search/basic_search/search.py` | `BasicSearch` | 检索编排（与 local 几乎逐行相同） |
| `packages/graphrag/graphrag/query/structured_search/basic_search/search.py` | `BasicSearch.search` | 缺 `await` 的流式调用 + 硬编码统计 |
| `packages/graphrag/graphrag/query/structured_search/basic_search/search.py` | `BasicSearch.stream_search` | 带 `await` 的正确版本 |
| `packages/graphrag/graphrag/query/structured_search/basic_search/basic_context.py` | `BasicSearchContext` | 文本单元向量检索 + CSV 渲染 |
| `packages/graphrag/graphrag/query/structured_search/basic_search/basic_context.py` | `BasicSearchContext.build_context` | 检索 → 按 `text_units` 顺序 join → 预算截断 |
| `packages/graphrag/graphrag/prompts/query/basic_search_system_prompt.py` | `BASIC_SEARCH_SYSTEM_PROMPT` | 只允许 `Sources` 的引用语法（Goal 段重复两次） |
| `packages/graphrag/graphrag/query/context_builder/builders.py` | `BasicContextBuilder` | basic 的上下文构建器抽象 |
| `packages/graphrag/graphrag/query/context_builder/builders.py` | `ContextBuilderResult` | 统一结果（`llm_calls` 默认 0） |
| `packages/graphrag/graphrag/query/input/retrieval/text_units.py` | `get_candidate_text_units` | 按实体反查文本单元（local 用，basic 不用） |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：基础检索](https://microsoft.github.io/graphrag/query/basic_search/)
- [graphrag 论文（Edge et al., 2024）中的基线对比](https://arxiv.org/abs/2404.16130)
- [向量检索综述（Karpukhin et al., 2020）](https://arxiv.org/abs/2004.04906)

## 与主核 / AI 的关系

basic search 与主核（多模态数据库内核）的**全表顺序扫描**处境相同：都是「不用任何索引结构的朴素路径」。而两者存在的理由也相同——**作为对照基准来判断索引是否值得**。主核的 `enable_seqscan = on` 就是这个作用：优化器要能算出「走索引反而更慢」并选全扫。

**差别在于两者是否能被优化器切换**：主核有个基于代价的优化器在两者之间选；graphrag 的四种模式是**用户显式指定**的（`--method basic|local|global|drift`），没有自动选择。**判据是「能否给每种模式一个代价模型」**：主核能（页数、选择性），graphrag 不能（LLM 调用成本可估，但质量不可估）。

第二处对照是「顺序丢失」这个缺陷与主核的**行顺序语义**。主核的 `SELECT` 无 `ORDER BY` 时结果顺序不保证——所以应用不该依赖它。而 basic search 恰好**隐式依赖**了 `self.text_units` 的顺序（截断按它做）。**这是一个「把未定义顺序当已定义用」的实例**，而后果是「检索到的最相关 k 条可能大部分被截断掉」。

第三处是那个缺 `await` 的 bug 与主核里「忘记 `latch_unlock`」这类配对缺失。两者都是**成对操作少了一半**，且都不会在编译期被发现（Python 甚至运行期也不报错，只 `RuntimeWarning`）。**这类缺陷只能靠「代码审查时看配对」或「测试覆盖到该路径」来发现**。

对 RAG 工程的实际意义在于：任何声称「图 RAG 比向量 RAG 好」的结论都需要一个严格对齐的基线。这份实现给出的三条经验可直接用：**基线与实验组要共用骨架**（同一个基类、同一套 prompt 结构，否则差异归因不清）、**检索结果的相关性排序要在 join 之后保留**（否则截断会丢掉最相关的）、以及**成本指标要与权威来源对账**（内部自洽的统计可能系统性偏低）。
