---
project: llama-index
feature: batch-eval-and-dataset
title: 批量评测与数据集生成
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/evaluation/batch_runner.py
    symbols: [BatchEvalRunner, eval_response_worker, eval_worker]
  - path: llama-index-core/llama_index/core/evaluation/dataset_generation.py
    symbols: [DatasetGenerator, QueryResponseDataset]
---

# 批量评测与数据集生成

## 一句话职责

把「单个评测器评单条样本」扩成闭环的两端：`BatchEvalRunner` 用**信号量 + 重试**对「一批 query-response」并行跑多个评测器；`DatasetGenerator` 反过来**从文档生成问题**（可选再生成参考答案），产出一个 `QueryResponseDataset` 供前者使用——于是没有标注数据也能开始评测。

## 机制是什么

| 类 | 方向 | 输入 | 输出 |
|---|---|---|---|
| `BatchEvalRunner` | **评** | `queries` / `response_strs` 或 `responses` | `{评测器名: [EvaluationResult, ...]}` |
| `DatasetGenerator` | **造** | `nodes`（或 `documents`） | `QueryResponseDataset`（问题 + 可选答案） |
| `QueryResponseDataset` | 载体 | — | 两个按 id 对齐的字典：`queries` / `responses` |

三者的接口是咬合的：`DatasetGenerator` 造出来的 `questions` 恰好是 `BatchEvalRunner` 的 `queries`。

## 怎么实现的

### 一、`BatchEvalRunner`：三层结构（校验 → 扇出 → 归拢）

**三个 worker 函数是模块级的，各自带同一个重试装饰器。**

```
@retry(reraise=True, stop=stop_after_attempt(3), wait=wait_exponential(multiplier=1, min=4, max=10))
async def eval_response_worker(semaphore, evaluator, evaluator_name, query=None, response=None, eval_kwargs=None):
    eval_kwargs = eval_kwargs or {}
    async with semaphore:
        return (evaluator_name, await evaluator.aevaluate_response(query=query, response=response, **eval_kwargs))
```

三个 worker（`eval_response_worker` / `eval_worker` / `response_worker`）的形状完全一致，都是：

1. **`async with semaphore`** —— 并发闸门。
2. **返回 `(evaluator_name, result)` 元组** —— 这是唯一的归拢依据（见下）。
3. **`@retry(reraise=True, stop=stop_after_attempt(3), wait=wait_exponential(multiplier=1, min=4, max=10))`** —— 三次尝试、指数退避 4~10 秒、最终仍失败则抛出。

**`reraise=True` 是这里最关键的一个参数。** 默认情况下 tenacity 在重试用尽后会抛一个 `RetryError` 包着最后一次异常——那会**丢失原始异常类型**。`reraise=True` 让它抛原始异常，于是评测失败的诊断信息完整。

**而 `semaphore` 是在 `with` 里获取的，也就是「重试时会重新排队」**——一次失败的重试会先释放信号量、等退避、再重新获取。所以**重试不会长期占住一个并发槽**，但也意味着退避期间的并发度会下降。

**三个 worker 的差别只在「调用哪个评测入口」。**

| worker | 调的方法 | 用在 |
|---|---|---|
| `eval_response_worker` | `evaluator.aevaluate_response(query, response)` | `aevaluate_responses`（传 `Response` 对象） |
| `eval_worker` | `evaluator.aevaluate(query, response_str, contexts)` | `aevaluate_response_strs`（传字符串） |
| `response_worker` | `query_engine.aquery(query)` | `aevaluate_queries`（先跑引擎拿答案） |

也就是说 `response_worker` **不是评测而是生成**——它存在的理由见 `aevaluate_queries`。

**`__init__` 里有一个容易被忽略的事实：信号量是「实例级」的。**

```
self.semaphore = asyncio.Semaphore(self.workers)
```

`asyncio.Semaphore` 在构造时**不绑定事件循环**（Python 3.10+ 起它惰性绑定）。于是一个 `BatchEvalRunner` 实例**可以在不同 `asyncio_run` 调用之间复用**——这很重要，因为同步包装 `evaluate_response_strs` 每次都 `asyncio_run` 一个新循环。若信号量绑死了循环，第二次调用就会报「attached to a different loop」。

**而 `self.asyncio_mod = asyncio_module(show_progress=self.show_progress)` 是「tqdm 或原生 asyncio」的二选一。** 它的两个方法名与原生一致（`gather`），所以调用点不必分叉。

**`_validate_and_clean_inputs` 解决的是一个「可选参数列表怎么对齐」的问题。**

```
input_len = None
for inputs in inputs_list:
    if inputs is not None:
        input_len = len(inputs); break
if input_len is None: raise ValueError("At least one item in inputs_list must be provided.")

new_inputs_list = []
for inputs in inputs_list:
    if inputs is None: new_inputs_list.append([None] * input_len)
    else:
        if len(inputs) != input_len: raise ValueError("All inputs must have the same length.")
        new_inputs_list.append(inputs)
return new_inputs_list
```

三步：**取第一个非 `None` 的长度作为基准** → **把 `None` 补成 `[None] * len`** → **校验其余的长度一致**。

这是一个「部分可选、部分必填」的列表参数的正确处理方式。它让「只给 `queries`」与「给 `queries` + `response_strs`」都能工作，且下游不必到处判 `None`。

代价是**它引入了隐含的顺序依赖**：`input_len` 取的是第一个非 `None` 的长度，所以若 `queries` 比 `response_strs` 长，报错发生在**校验 `response_strs`** 时——消息是「All inputs must have the same length」，没有指出哪个不一致。

**`_validate_nested_eval_kwargs_types` 处理一个「向后兼容的双形态」参数。**

```
for evaluator, eval_kwargs in eval_kwargs_lists.items():
    if isinstance(eval_kwargs, list):
        eval_kwargs_lists[evaluator] = self._validate_and_clean_inputs(eval_kwargs)[0]
    elif isinstance(eval_kwargs, dict):
        for k in eval_kwargs:
            v = eval_kwargs[k]
            if not isinstance(v, list): raise ValueError(f"nested inner values in eval_kwargs must be a list. ...")
            eval_kwargs_lists[evaluator][k] = self._validate_and_clean_inputs(v)[0]
    else: raise ValueError(f"eval_kwargs must be a list or a dict. ...")
```

两种形态：

- **`{kwarg: [list of values]}`（单评测器）** —— 顶层直接是列表。这是旧形态，注释写着 `# maintain backwards compatibility - for use with single evaluator`。
- **`{evaluator: {kwarg: [list of values]}}`（多评测器）** —— 每个评测器有各自的 kwargs 列表。这让「评测器 A 需要 `reference`、评测器 B 不需要」能表达。

**而这次改造是原地修改 `eval_kwargs_lists`（`eval_kwargs_lists[evaluator] = ...`）。** 因为它是从 `**eval_kwargs_lists` 收集来的新字典，所以不污染调用方的对象——但**它会从「列表」被替换成「列表的列表」**（`_validate_and_clean_inputs(...)[0]` 取 `[0]` 那个 = 补全后的列表）。于是同一个键在函数前后语义不同。

**`multi_kwargs` 那个判据是「看第一个值是不是 dict」。**

```
multi_kwargs = len(eval_kwargs_lists) > 0 and isinstance(next(iter(eval_kwargs_lists.values())), dict)
```

**这依赖「所有值形态一致」这个前提**——`_validate_nested_eval_kwargs_types` 已经校验了每个值只能是 list 或 dict，但**没有校验它们全是同一类**。若传了 `{"a": [..], "b": {"x": [..]}}`，`multi_kwargs` 取决于第一个键的顺序，然后对另一种形态会出错（`eval_kwargs_lists[name]` 拿到 list，随后 `_get_eval_kwargs` 会 `v[idx]` 索引一个 list 再索引字符串）。

**扇出的双层循环是 `for idx, query` × `for name, evaluator`。**

```
for idx, query in enumerate(cast(List[str], queries)):
    response_str = cast(List, response_strs)[idx]
    contexts = cast(List, contexts_list)[idx]
    for name, evaluator in self.evaluators.items():
        if multi_kwargs: kwargs = eval_kwargs_lists[name] if name in eval_kwargs_lists else {}
        else: kwargs = eval_kwargs_lists
        eval_kwargs = self._get_eval_kwargs(kwargs, idx)
        eval_jobs.append(eval_worker(self.semaphore, evaluator, name, query=query, response_str=response_str, contexts=contexts, eval_kwargs=eval_kwargs))
results = await self.asyncio_mod.gather(*eval_jobs)
```

**所以任务数是 `样本数 × 评测器数`**——也就是说**每个样本会被每个评测器各评一次**。这正是「多评测器批量」的语义：同时算 faithfulness、relevance、correctness。

**`_get_eval_kwargs` 把「按样本排列的列表」变成「本条样本的 kwargs 字典」**：`{k: v[idx] for k, v in eval_kwargs_lists.items()}`。于是索引对齐的责任全部压在前面的长度校验上。

**而 `_format_results` 是归拢的最后一步，它依赖 worker 返回的那个元组。**

```
results_dict: Dict[str, List[EvaluationResult]] = {name: [] for name in self.evaluators}
for name, result in results:
    results_dict[name].append(result)
```

**为什么用「名字元组」而不是按位置？** 因为 `gather` 的返回顺序与 `eval_jobs` 一致，理论上可以按下标算。但**按下标算需要重现双层循环的索引对应关系**，而「worker 自己带名字回来」让这件事变成纯数据驱动——无论任务怎么重排（比如重试改变了完成顺序），归拢都正确。

这是一个「让并发结果自带身份标签」的通用做法。

**`_format_results` 有一个隐含前提：每个评测器在结果里出现的次数等于样本数。** 若某个 worker 在重试耗尽后抛异常，整个 `gather` 会失败（`reraise=True`），于是**一条失败会让整批失败**。这是 `BatchEvalRunner` 的一个明确取舍——**没有 per-item 的容错**。

对比 `SubQuestionQueryEngine`（另一个页里）那种「一个子问题失败就略过」的容错，这里的取舍相反：**评测是批量指标计算，缺一条会让统计口径不清，所以宁可整批失败**。

**`aevaluate_queries` 是「先批量生成、再批量评测」两段。**

```
if queries is None: raise ValueError("`queries` must be provided")
response_jobs = []
for query in queries:
    response_jobs.append(response_worker(self.semaphore, query_engine, query))
responses = await self.asyncio_mod.gather(*response_jobs)
return await self.aevaluate_responses(queries=queries, responses=responses, **eval_kwargs_lists)
```

**注意 `queries is None` 被显式校验**，而另外两个入口（`aevaluate_response_strs` / `aevaluate_responses`）允许 `queries=None`（会补成 `[None]*len`）——**因为「评测器可能需要 query」但「跑引擎必须有 query」**。这是一个必要的不对称。

而两段都用 `self.semaphore`，于是**生成与评测共享同一个并发上限**——不会出现「生成阶段打满、评测阶段又打满」的双倍并发。

**三个同步包装都是 `asyncio_run(self.aevaluate_xxx(...))`。**

`evaluate_response_strs` / `evaluate_responses` / `evaluate_queries`——**只差一个 `await` 与 `asyncio_run`**。这是整个库的普遍模式（`g02` 里的 `BaseEvaluator.evaluate` 也是如此）。

### 二、`DatasetGenerator`：把「文档」变成「考题」

**先记一件事：这个类是已废弃的。**

```
@deprecated("Deprecated in favor of `RagDatasetGenerator` which should be used instead.", action="always")
class DatasetGenerator(PromptMixin):
```

`action="always"` 意味着**任何实例化都会发警告**。也就是说这一页描述的是一个「仍然可用但已被替代」的实现，而它的后继（`RagDatasetGenerator`）在另一个包里。

**它的构造参数里有一个「默认提示就是一段角色设定」。**

```
self.question_gen_query = question_gen_query or f"You are a Teacher/Professor. Your task is to setup {num_questions_per_chunk} questions for an upcoming quiz/examination. The questions should be diverse in nature across the document. Restrict the questions to the context information provided."
```

`question_gen_query` 的语义是**「查询串」而不是「模板」**——它会被当作 query 传给一个查询引擎（见下）。所以这段「你是老师」的文字**是查询而不是提示模板**。这是一个容易误读的命名。

`num_questions_per_chunk` 默认 10，被拼进这段字符串里。

**它给你的两个字典各有分工：`text_question_template` 用于「生成问题」，`text_qa_template` 用于「生成答案」。**

**核心方法 `_agenerate_dataset` 做的是「每个节点建一个临时索引，问一次问题生成器」——而那个「临时索引」正是关键技巧。**

```
index = SummaryIndex.from_documents([Document(text=node.get_content(metadata_mode=self._metadata_mode), metadata=node.metadata)], callback_manager=self.callback_manager)
query_engine = index.as_query_engine(llm=self.llm, text_qa_template=self.text_question_template, use_async=True)
task = query_engine.aquery(self.question_gen_query)
```

也就是说：**把一个节点变成一个只含这一个文档的 `SummaryIndex`，然后拿「老师出题」那句话当查询打进去。**

**为什么要建索引而不是直接调 LLM 填模板？** 因为 `SummaryIndex` 的作用是「把全文当作上下文喂给模型」——而这个包了一层的好处是：**问题的数量、模板、上下文预算全部复用了合成器的既有机制**。这与 `g02` 里 `FaithfulnessEvaluator` 用 `SummaryIndex` 是同一个手法。

**收集任务时用 `num` 做的是「提前截断而不是事后截断」。**

```
for node in nodes:
    if num is not None and len(query_tasks) >= num: break
    ...query_tasks.append(task)...
```

**它在加入任务前就检查数量**，于是**只申请了 `num` 次 LLM 调用**。而末尾还有一次事后截断：

```
query_ids = list(queries.keys())
if num is not None:
    query_ids = query_ids[:num]
```

**两处截断的理由不同**：前面那个控制「多少个节点参与」（省成本），后面那个控制「多少条问题进最终数据集」（因为每个节点产出多个问题，`num_questions_per_chunk=10` 时一个节点就有 10 条）。

**清理问题文本的两步正则很具体。**

```
result = str(response).strip().split("\n")
cleaned_questions = [re.sub(r"^\d+[\).\s]", "", question).strip() for question in result]
cleaned_questions = [question for question in cleaned_questions if len(question) > 0]
```

`^\d+[\).\s]` 处理三种编号形态：`1)`、`1.`、`1 `（数字后跟空格）。而**只去掉一个前导编号字符组**，不去掉内部的。

这与 `FaithfulnessEvaluator` 那种「宽松子串匹配」形成对比：这里用的是**结构化清理**（因为输出是可预期的编号列表）。

**问题 id 用 `uuid4()` 而不是序号。**

```
cur_queries = {str(uuid.uuid4()): question for question in cleaned_questions}
```

而 `QueryResponseDataset.from_qr_pairs` 用的是**序号字符串**（`str(idx)`）。两种 id 策略并存：

- `uuid4()` 保证**跨批次不冲突**（多次生成不会撞 id）。
- 序号简单、可读。

而 `DatasetGenerator` 用 uuid 是因为「多次生成后合并数据集」是常见用法。

**`generate_response=True` 时对每个问题再跑一次查询。**

```
qa_query_engine = index.as_query_engine(llm=self.llm, text_qa_template=self.text_qa_template)
qr_task = qa_query_engine.aquery(query)
```

**它复用了同一个 `index`**（`summary_indices[idx]`）——于是「生成问题的上下文」与「生成答案的上下文」完全一致。这对「参考答案」这件事是必要的：**答案必须能从那个节点里得到**，否则数据集自带不可答的样本。

**但注意它每次都新建一个 `query_engine`**（在循环内）——也就是说一个节点有 10 个问题就建 10 个引擎。引擎本身很轻（只是包装索引与提示），所以这不是性能问题，但它让「提示可以按问题变化」成为可能（虽然代码里没有）。

**`QueryResponseDataset` 的结构是两个字典，而它的不变量靠一个 property 来校验。**

```
queries: Dict[str, str]    # query id -> query
responses: Dict[str, str]  # query id -> response

@property
def qr_pairs(self):
    for query_id in self.queries:
        if query_id not in self.responses: raise ValueError(f"Query id {query_id} not in responses")
    return [(self.queries[qid], self.responses[qid]) for qid in self.queries]
```

**校验发生在读取时而不是构造时。** 于是可以构造一个「缺了响应」的数据集（`DatasetGenerator` 在 `generate_response=False` 时正是这样产出 `responses={}`），但只要不读 `qr_pairs` 就不会报错。而 `questions` property 只读 `queries.values()`，**不触发校验**——所以「只生成问题」的用法可以正常工作。

这是一个刻意设计的不变量：「问题齐全、答案可选」是合法状态，「有问题的答案缺失」不是。而它的实现方式（读时校验）让两种状态在类型上不做区分，只在行为上分叉。

**`save_json` / `from_json` 用 `model_dump()` 与 `cls(**data)`，且都显式 `encoding="utf-8"`。** 后者避免了 Windows 上默认编码写中文出错——这与 `g01` 那页里 `open(..., encoding="utf-8")` 的处理是同一个考虑。

## 关键取舍

**源码事实：** 三个 worker 都是模块级函数，形状统一：`async with semaphore` + 返回 `(evaluator_name, result)` + 同一个 `@retry(reraise=True, stop_after_attempt(3), wait=wait_exponential(min=4, max=10))`。

**源码事实：** `reraise=True` 让重试耗尽后抛原始异常而不是 tenacity 的 `RetryError`，保住诊断信息；而 `semaphore` 在 `with` 内获取，于是重试会先释放并发槽、退避后再重新获取。

**源码事实：** `asyncio.Semaphore` 在构造时不绑事件循环，因此 `BatchEvalRunner` 实例可以跨多次 `asyncio_run`（同步包装每次都新建循环）复用。

**源码事实：** `_validate_and_clean_inputs` 以**第一个非 `None` 的长度**为基准补全 `None`，并校验其余一致；错误消息不指出哪个不一致。

**源码事实：** `eval_kwargs` 支持两种形态（单评测器的 `{kwarg: [list]}` 与多评测器的 `{evaluator: {kwarg: [list]}}`），且 `multi_kwargs` 的判据是「看第一个值是不是 dict」——没有校验所有值形态一致。

**源码事实：** 任务数是「样本数 × 评测器数」；归拢靠 worker 自带的 `evaluator_name` 而不是按位置算下标。

**源码事实：** 重试耗尽后一条失败会让整批 `gather` 失败——**没有 per-item 容错**，与「子问题引擎一个失败就略过」的取向相反。

**源码事实：** `aevaluate_queries` 显式校验 `queries is None`（因为跑引擎必须有 query），而另外两个入口允许 `queries=None`；两段（生成 + 评测）共享同一个信号量。

**源码事实：** `DatasetGenerator` 已废弃（`action="always"`，任何实例化都警告），后继是 `RagDatasetGenerator`。

**源码事实：** `question_gen_query` 的语义是**查询串而非模板**——它会被当作 query 打给一个只含该节点的 `SummaryIndex`；默认值是一段「你是老师，请出 10 道题」的角色设定。

**源码事实：** 用 `SummaryIndex` 包住单个节点，从而复用合成器的上下文预算与模板机制（与 `FaithfulnessEvaluator` 同一手法）。

**源码事实：** `num` 有两处截断：加入任务前（省 LLM 调用）与产出后（每节点会产出多个问题）；问题 id 用 `uuid4()`（支持多次生成后合并），而 `from_qr_pairs` 用序号字符串。

**源码事实：** 问题清理用 `re.sub(r"^\d+[\).\s]", "", ...)` 处理 `1)` / `1.` / `1 ` 三种编号形态。

**源码事实：** `QueryResponseDataset` 的两个字典允许 `responses` 为空（只生成问题的合法状态），不变量在 `qr_pairs` 读取时校验，而 `questions` 不触发校验。

**架构推论：** 这一页的两个类构成了评测闭环的两端，而它们对「失败」的态度完全不同：

- **造数据时失败是数据的一部分**（一条问题格式怪，清理掉即可）；
- **算指标时失败是必须暴露的**（一条缺失会让统计口径不清）。

于是 `DatasetGenerator` 清理、容错、截断；`BatchEvalRunner` 重试、`reraise`、整批失败。**同一套代码里这两种取向并存，正是「造数据」与「算指标」在心智上不同的体现。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/evaluation/batch_runner.py` | `BatchEvalRunner` | 批量评测：信号量 + 重试 + 多评测器扇出 |
| `llama-index-core/llama_index/core/evaluation/batch_runner.py` | `eval_worker` / `eval_response_worker` | 带信号量与重试的评测任务 |
| `llama-index-core/llama_index/core/evaluation/batch_runner.py` | `response_worker` | 生成任务（非评测），供 `aevaluate_queries` 用 |
| `llama-index-core/llama_index/core/evaluation/batch_runner.py` | `_validate_and_clean_inputs` | 以首个非 `None` 长度为基准补全与校验 |
| `llama-index-core/llama_index/core/evaluation/batch_runner.py` | `_validate_nested_eval_kwargs_types` | 单/多评测器两种 kwargs 形态的兼容 |
| `llama-index-core/llama_index/core/evaluation/batch_runner.py` | `_format_results` | 按 worker 带回的名字归拢 |
| `llama-index-core/llama_index/core/evaluation/batch_runner.py` | `aevaluate_queries` | 先批量生成再批量评测，共享信号量 |
| `llama-index-core/llama_index/core/evaluation/dataset_generation.py` | `DatasetGenerator` | 已废弃；从节点生成问题（可选答案） |
| `llama-index-core/llama_index/core/evaluation/dataset_generation.py` | `_agenerate_dataset` | 单节点临时索引 + 两处 `num` 截断 + 问题清理 |
| `llama-index-core/llama_index/core/evaluation/dataset_generation.py` | `question_gen_query` | 语义是「查询串」而非模板 |
| `llama-index-core/llama_index/core/evaluation/dataset_generation.py` | `QueryResponseDataset` | 两字典载体，不变量在读 `qr_pairs` 时校验 |
| `llama-index-core/llama_index/core/evaluation/dataset_generation.py` | `from_qr_pairs` / `save_json` / `from_json` | 序号 id 构造与 UTF-8 持久化 |

## 参考

- [llama-index 源码：evaluation/batch_runner.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/evaluation/batch_runner.py)
- [llama-index 源码：evaluation/dataset_generation.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/evaluation/dataset_generation.py)
- [tenacity 文档：retry 与 reraise](https://tenacity.readthedocs.io/)
- [llama-index 文档：端到端评测](https://docs.llamaindex.ai/en/stable/module_guides/evaluating/)

## 与主核 / AI 的关系

批量的「扇出 → 逐个执行 → 归拢」与主核里的**并行查询与聚合**同构：数据库执行多个子查询时也会控制并发度（`max_parallel_workers`），而两者的差别在于失败处理——主核通常能让单个子查询失败并返回部分结果，而这里的 `BatchEvalRunner` 选择整批失败。

这个差别的理由在语义层：**一次查询的「部分结果」可以用来回答问题，而一批评测的「部分结果」会让指标失真**。前者是有意义的降级，后者是无意义的数字。

对 agent 工程的直接价值有三条。其一，**并发闸门应当只包住真正的调用，而重试要重新排队**。`semaphore` 放在 `with` 内部让退避期间不占槽，这是对的；但这也意味着重试时的并发度会短暂下降，需要在容量规划里考虑到。其二，**让并发结果自带身份标签**。worker 返回 `(name, result)` 而不是靠下标推算，是让归拢逻辑免疫于执行顺序变化的正确做法。其三，**「造数据」与「算指标」的容错策略必须分开**。前者要清理、容错、尽量多产出；后者要重试、暴露、宁可失败——把两边的错误处理混用，要么得到脏指标，要么得到过少的数据。
