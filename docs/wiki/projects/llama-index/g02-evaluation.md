---
project: llama-index
feature: evaluation
title: 检索与回答的评测
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/evaluation/base.py
    symbols: [BaseEvaluator, EvaluationResult]
  - path: llama-index-core/llama_index/core/evaluation/faithfulness.py
    symbols: [FaithfulnessEvaluator]
  - path: llama-index-core/llama_index/core/evaluation/pairwise.py
    symbols: [PairwiseComparisonEvaluator, EvaluationSource]
---

# 检索与回答的评测

## 一句话职责

把「这个回答好不好」变成一个**可计算的布尔或分数**：`BaseEvaluator` 定义统一契约与结果结构，`FaithfulnessEvaluator` 判断回答是否**有上下文支持**（是不是编的），`PairwiseComparisonEvaluator` 判断**两个回答哪个更好**——而且它会把两个回答的顺序对调再问一遍，以消除模型的位置偏好。

## 机制是什么

| 层 | 内容 |
|---|---|
| 契约 | `BaseEvaluator`：`aevaluate` 是唯一的抽象方法，`evaluate` 是它的同步包装（`asyncio_run`） |
| 便捷入口 | `evaluate_response` / `aevaluate_response`：从 `Response` 对象里抽出「回答串 + 来源上下文」再调 `aevaluate` |
| 结果 | `EvaluationResult` 八个字段，其中 `passing` / `score` / `feedback` 三者是主要产出，`invalid_result` / `invalid_reason` 标记「评测本身没跑成」 |
| 两类评测 | 绝对评测（`Faithfulness`：单答案 → 通过/不通过）与相对评测（`Pairwise`：两个答案 → 谁更好） |

**注意 `Faithfulness` 完全不用 `query`**：它只回答「这个 `response` 是否被这些 `contexts` 支持」——传进来的 `query` 只是被原样放回结果里。这是一个关键设计：**忠实性（faithfulness）与相关性（relevance）是两件不同的事**，前者只看「有没有依据」，不看「有没有答对问题」。

## 怎么实现的

### 一、`EvaluationResult`：一个允许「不确定」的结果结构

**八个字段，而它们的设计倾向可以从「哪些字段是 `Optional`」读出来。**

```
query: Optional[str] = None
contexts: Optional[Sequence[str]] = None
response: Optional[str] = None
passing: Optional[bool] = None        # 二进制判定
feedback: Optional[str] = None        # 理由/推理
score: Optional[float] = None         # 连续分数
pairwise_source: Optional[str] = None # 只用于 pairwise
invalid_result: bool = False          # 默认 False
invalid_reason: Optional[str] = None
```

三个要点：

1. **`passing` 与 `score` 都是可选的，且可以同时给。** `Faithfulness` 两者都填（`passing` + `score=1.0/0.0`）；`Pairwise` 在平局时填 `passing=None, score=0.5`——也就是**用 `None` 表达「不是二元判定」**。
2. **`invalid_result` 用 `bool` 且默认 `False`，而不是 `Optional[bool]`。** 这与其它字段不同：它是一个**明确的二值**（评测是不 是有效），而不是「可能有也可能没有」。而 `invalid_reason` 补上原因。
3. **`invalid_result` 与 `passing=False` 是两件完全不同的事**：前者是「评测器没能产出判断」（比如模型输出的格式无法解析），后者是「评测器判断它不合格」。把这两者混为一谈是评测系统最常见的错误——`invalid_result` 这个字段的存在就是为了避免它。

**`BaseEvaluator.evaluate` 的实现只有一句，而它做了一个选择。**

```
return asyncio_run(self.aevaluate(query=query, response=response, contexts=contexts, **kwargs))
```

**唯一抽象方法是 `aevaluate`（异步），而同步版是它的包装。** 这与其它类（`BaseQueryEngine` 等）「两个都抽象、两个都实现」的做法不同——这里**只有异步是真正的实现**。理由是评测几乎总是要调 LLM，而 LLM 调用天然适合异步（尤其是批量评测时并行）。

代价是：一个纯同步的评测器也必须写成 `async def`。但这是可以接受的——写成 `async def` 而不 `await` 任何东西的成本极低。

**`evaluate_response` 里有一个「从 `Response` 对象抽参数」的固定映射。**

```
response_str = response.response
contexts = [node.get_content() for node in response.source_nodes]
return self.evaluate(query=query, response=response_str, contexts=contexts, **kwargs)
```

也就是说**上下文取自 `source_nodes` 的内容**——与查询引擎那页里「响应带一份来源节点元数据」是同一套机制的另一面。**评测的「依据」正是检索返回的那些节点**，于是评测不需要重新检索。

这三行也解释了为什么评测能在 RAG 管道末端跑：**答案与它的依据都在同一个 `Response` 对象里。**

### 二、`FaithfulnessEvaluator`：拿上下文造一个临时索引

**它的实现方式会让第一次读的人意外——它不是「把上下文拼进一个提示」，而是「把上下文建成一个索引再查询」。**

```
docs = [Document(text=context) for context in contexts]
index = SummaryIndex.from_documents(docs)
query_engine = index.as_query_engine(llm=self._llm, text_qa_template=self._eval_template, refine_template=self._refine_template)
response_obj = await query_engine.aquery(response)
raw_response_txt = str(response_obj)
```

也就是说：

1. 每条上下文变成一个 `Document`。
2. 建成一个 `SummaryIndex`（不嵌入、不检索，只是把所有文档都视为候选）。
3. 用**评测模板**作为 QA 模板、**回答串本身作为查询**。
4. 拿到 YES/NO 文本。

**为什么要绕这一圈？** 因为这样它**复用了合成器的「多块归约」能力**：上下文可能有几十条，一次装不进提示，而 `Refine` 的 `refine_template` 正好能「带着已有答案看下一块」。于是**评测与生成本身用的是同一套上下文管理机制**——这解释了为什么这个评测器要接两个模板（`eval_template` 与 `refine_template`）。

**`refine_template` 的语义是「只要答案是 YES 就保持 YES」。**

> 如果已有答案是 YES，仍然回答 YES。如果信息在新的上下文里出现，回答 YES。否则回答 NO。

这是一个**单调**的规则：一个 YES 不会被后续块推翻。也就是说「有任意一块支持就算忠实」——这在语义上是 `∃`（存在）而不是 `∀`（全部）。**这与「回答里的每个论断都必须有支持」是不同的判定**——后者才是严格意义上的忠实性。

**解析只有一句，而它用的是子串匹配而不是精确匹配。**

```
if "yes" in raw_response_txt.lower(): passing = True
else:
    passing = False
    if self._raise_error: raise ValueError("The response is invalid")
```

三个要点：

1. **`"yes" in ...lower()`** —— 任何包含 `yes` 的文本都判为通过。于是「The answer is not yes」也会被判 YES。这是一个宽松到近乎危险的解析，而它的理由大概是：评测模板要求输出 YES/NO，模型通常照做。
2. **失败时的处理有 `raise_error` 开关**——默认 `False`，即「不通过」是一个正常结果而不是异常。这与「`invalid_result` 才是异常」的区分是一致的。
3. 但它**没有设置 `invalid_result`**——一个无法解析的输出被当成 `passing=False` 而不是「评测无效」。**这与 `EvaluationResult` 里 `invalid_result` 的设计意图不一致**：`Pairwise` 在无法解析时设了 `invalid_result=True`，而这里没有。

**`score` 是 `passing` 的整数化。** `score=1.0 if passing else 0.0` —— 也就是说这里的 `score` 不携带任何额外信息，只是把布尔换成浮点。这与 `Pairwise` 里 `score` 表达「票数比例」不同。

**模板目录（`TEMPLATES_CATALOG`）是一个按模型名分发的提示表。**

```
TEMPLATES_CATALOG = {"llama3:8b": LLAMA3_8B_EVAL_TEMPLATE}
model_name = self._llm.metadata.model_name
self._eval_template = TEMPLATES_CATALOG.get(model_name, DEFAULT_EVAL_TEMPLATE)
```

目前只有一项（llama3:8b 专用模板），而 `LLAMA3_8B_EVAL_TEMPLATE` 与默认模板的差别在于**它把「部分支持就算支持」写得更明确**（"Answer YES if **any part** of the context supports the information, even if most of the context is unrelated"），并给了四个例子。

这个目录的存在说明一件事：**同一个评测语义在不同模型上需要不同的措辞**，而这是评测的可复现性隐患——**换个模型就换了一套提示**，于是两次评测的数字不可直接比较。

**构造里有一处 `if` 结构的 bug。**

```
self._eval_template: BasePromptTemplate
if isinstance(eval_template, str):
    self._eval_template = PromptTemplate(eval_template)
if isinstance(eval_template, BasePromptTemplate):
    self._eval_template = eval_template
else:
    model_name = ...
    self._eval_template = TEMPLATES_CATALOG.get(...)
```

**第二个 `if` 是一个新的 `if`（不是 `elif`），所以当 `eval_template` 是 `str` 时**：第一个 `if` 把它设成 `PromptTemplate`，然后第二个 `if` 判 `eval_template`（原始参数，还是 `str`）**不为 `BasePromptTemplate`** → 走 `else` → **覆盖成目录里的模板**。

结果是**传字符串模板完全不生效**——它被静默丢弃并替换成默认模板。只有传 `BasePromptTemplate` 实例才有效。这是一个明确的 bug（应为 `elif`），而它的表现形式是「我传了自定义模板但评测行为没变」。

`refine_template` 那边的 `if/else` 是对的（`if isinstance(str)` / `else`），所以两个模板的处理方式不一致。

### 三、`PairwiseComparisonEvaluator`：用「对调」消偏

**它的核心机制是 `_enforce_consensus`：** 把两个回答的顺序对调再问一遍，只有两次都一致才认这个结论。

**解析函数只认三个标记。**

```
if "[[A]]" in eval_response: passing, score = True, 1.0
elif "[[B]]" in eval_response: passing, score = False, 0.0
elif "[[C]]" in eval_response: passing, score = None, 0.5
else: passing, score, feedback = None, None, None
```

三态输出对应「A 更好 / B 更好 / 平局」，而第四种（都没匹配上）返回全 `None` 交由上层判为无效。

**用 `[[A]]` 这样的双括号标记是一个刻意的设计**：它极不可能出现在自由文本里，于是「模型是否真的给出了判定」可以靠子串匹配判断，而不必解析整段。

**`_resolve_results` 是这一页最有内容的函数，而它是「两轮投票」的计票。**

```
votes_1 = eval_result.score + (1 - flipped_eval_result.score)
votes_2 = (1 - eval_result.score) + flipped_eval_result.score
if votes_1 + votes_2 != 2: raise ValueError("Impossible score results. Total amount of votes is 2.")
```

**关键在 `(1 - flipped_eval_result.score)`**：对调之后 `[[A]]` 指的是「原来的第二个回答」。所以：

- 原轮投 A（score=1）→ 第一票给 answer_1；
- 对调轮投 A（引的是原来的 answer_2）→ **`1 - 1 = 0`**，不给 answer_1；
- 对调轮投 B（引的是原来的 answer_1）→ **`1 - 0 = 1`**，给 answer_1。

**这个 `1 - score` 就是「对调带来的翻转换算」，而它正是消偏的数学形式。**

而那个 `!= 2` 的校验在**平局时也会成立**：若两轮都是 `0.5`，`votes_1 = 0.5 + 0.5 = 1`、`votes_2 = 1`，和是 2——没问题。所以那条 `raise` 保护的是「分数不是 0/0.5/1 之一」的情形（比如自定义 parser 给了 0.3）。

**平局的两支处理区分了「真平局」与「不确定」。**

```
if votes_1 > votes_2: return voters_1[0]
elif votes_2 > votes_1: return voters_2[0]
else:
    if eval_result.score == 0.5:
        return eval_result          # 两轮都是平局 → 真平局
    else:
        return EvaluationResult(..., passing=None, score=0.5, feedback="", pairwise_source=EvaluationSource.NEITHER)   # 两轮矛盾 → 不确定
```

不等式拆开：`votes_1 == votes_2` 只可能有两种情形——**两轮都是 0.5（两边都说平局）**，或**两轮的判断互相矛盾**（一轮说 A 好、另一轮也说 A 好…等一下，那不会平；真正矛盾的是「一轮 A 好、另一轮 B 好」→ 那会给出 `votes_1 = 1 + 0 = 1`、`votes_2 = 0 + 1 = 1`）。

**所以「矛盾」（一轮偏 A、一轮偏 B）与「一致平局」在票数上无法区分**，代码用 `eval_result.score == 0.5` 这一条来判——若原轮就是平局则认平局，否则认「不确定」。

这个区分的必要性：**模型的位置偏好会让它在两轮里都选「第一个」，于是两轮会「一致地」判出两个相反的结论**——这正是需要消除的偏差。而它被正确地识别为「不确定」而不是「平局」。

**而 `voters_1` / `voters_2` 的构造解释了返回值为什么要挑一个「投票者」。**

```
voters_1 = [eval_result] * (eval_result.score == 1.0) + [flipped_eval_result] * (flipped_eval_result.score == 0.0)
```

`[x] * True` 得到 `[x]`、`[x] * False` 得到 `[]`——所以这两个列表收集的是「投了 answer_1 的那些轮次的 `EvaluationResult`」。然后 `return voters_1[0]` 返回其中任一轮的结果。

**为什么要返回「某一轮的结果」而不是新造一个？** 因为 `feedback`（模型的推理文本）是有价值的，而它就存在那一轮的结果里。返回投票者的结果等于**顺带把「为什么这么判」也带出来**。

而 `pairwise_source` 字段记录了这一轮是 `ORIGINAL` 还是 `FLIPPED`——于是调用方能知道最终结论是来自哪一轮。

**`aevaluate` 的流程把「无效结果」的传播做对了。**

```
if self._enforce_consensus and not eval_result.invalid_result:
    flipped_eval_result = await self._get_eval_result(query, second_response, response, reference)
    if not flipped_eval_result.invalid_result:
        resolved = await self._resolve_results(eval_result, flipped_eval_result)
    else:
        resolved = EvaluationResult(..., invalid_result=True, invalid_reason="Output cannot be parsed.")
else:
    resolved = eval_result
```

三处：

1. **原轮无效时根本不做对调**（`and not eval_result.invalid_result`）——省一次 LLM 调用。
2. **对调轮无效时整个结果无效**——而不是「只信原轮」。这是一个保守的选择：一轮能解析、一轮不能，说明评测器在这个问题上不稳定。
3. **关掉 `_enforce_consensus` 时直接用原轮结果**——也就是「信任模型的单次判断」。

**它忽略 `contexts` 参数。**

```
del kwargs  # Unused
del contexts  # Unused
```

**`del contexts` 写得很明确** —— 成对评测用不到检索上下文，它只比较两个回答（可选还有个 `reference`）。这是一个「用 `del` 声明某参数被故意忽略」的可读性做法（比默默不用好）。

`sleep_time_in_seconds` 参数也是同一个思路：`await asyncio.sleep(sleep_time_in_seconds)` —— 给批量评测时**限速**用，避免打爆 API 配额。两个评测器都有这个参数。

## 关键取舍

**源码事实：** `BaseEvaluator` 只有 `aevaluate` 是抽象方法，同步 `evaluate` 是 `asyncio_run` 的包装——评测天然适合异步。

**源码事实：** `EvaluationResult` 的 `invalid_result`（默认 `False`）与 `passing=False` 是两件不同的事：「评测没跑成」vs「评测判定不合格」。`Pairwise` 在无法解析时设 `invalid_result=True`，`Faithfulness` 没有（它把无法解析当 `passing=False`）。

**源码事实：** `evaluate_response` 从 `Response` 的 `source_nodes` 取上下文——所以评测不需要重新检索，答案与依据在同一个对象里。

**源码事实：** `FaithfulnessEvaluator` 把上下文建成 `SummaryIndex` 再查询，从而**复用合成器的多块归约**（这解释了它为何需要 `refine_template`）。

**源码事实：** 它的 refine 规则是「已有 YES 就保持 YES」——即「任意一块支持就算忠实」，是 `∃` 而非「每个论断都有支持」的严格判定。

**源码事实：** 它的解析是 `"yes" in raw_response_txt.lower()`，宽松到「The answer is not yes」也会判通过；且失败时只设 `passing=False`、不设 `invalid_result`。

**源码事实：** `TEMPLATES_CATALOG` 按模型名分发模板（目前只有 `llama3:8b` 一项）——换模型就换提示，于是跨模型的评测数字不可直接比较。

**源码事实：** `eval_template` 的处理有 `if`/`if`/`else` 的结构错误（第二个 `if` 应为 `elif`），结果是**传字符串模板会被静默丢弃并替换为默认模板**；`refine_template` 那边的 `if/else` 是正确的，两者不一致。

**源码事实：** `Pairwise` 用 `[[A]]`/`[[B]]`/`[[C]]` 三个标记解析，三态对应 A 优 / B 优 / 平局，未匹配返回全 `None` 并交由上层判为无效。

**源码事实：** 消偏的数学形式是 `votes_1 = score + (1 - flipped_score)`——对调后的 `[[A]]` 指的是原来的第二个回答，所以用 `1 - score` 换算。

**源码事实：** `votes_1 == votes_2` 时用「原轮是否为 0.5」区分「真平局」与「两轮矛盾（不确定）」；矛盾的情形被标为 `pairwise_source=NEITHER`。

**源码事实：** `voters_1` / `voters_2` 用 `[x] * bool` 收集投票轮次，返回其中一轮的结果从而顺带保留 `feedback`。

**源码事实：** 原轮无效则不做对调；对调轮无效则整个结果无效（不采信原轮）。

**架构推论：** 这一套的设计围绕一个区分展开：**「评测器给出的判断」与「评测器本身是否可靠」是两个正交维度**。

- `passing` / `score` 表达判断本身；
- `invalid_result` / `invalid_reason` 表达「这个判断值不值得信」。

而 `Pairwise` 的对调机制正是为第二个维度服务的：它不改变「哪个答案更好」的判定逻辑，而是**检验这个判定是否稳定**。这也解释了为什么它在两轮矛盾时给「不确定」而不是「平局」——**平局是一个结论，不确定是「无法下结论」**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/evaluation/base.py` | `EvaluationResult` | 八字段结果，含 `invalid_result` 与 `pairwise_source` |
| `llama-index-core/llama_index/core/evaluation/base.py` | `BaseEvaluator` | 只有 `aevaluate` 抽象；`evaluate` 是 `asyncio_run` 包装 |
| `llama-index-core/llama_index/core/evaluation/base.py` | `evaluate_response` / `aevaluate_response` | 从 `Response` 抽回答与上下文 |
| `llama-index-core/llama_index/core/evaluation/faithfulness.py` | `FaithfulnessEvaluator` | 上下文建成索引再查询，判是否编造 |
| `llama-index-core/llama_index/core/evaluation/faithfulness.py` | `DEFAULT_REFINE_TEMPLATE` | 「已有 YES 保持 YES」的单调归约规则 |
| `llama-index-core/llama_index/core/evaluation/faithfulness.py` | `TEMPLATES_CATALOG` | 按模型名分发的评测模板目录 |
| `llama-index-core/llama_index/core/evaluation/faithfulness.py` | `aevaluate` 的解析 | `"yes" in ...lower()`，宽宽松掉 `invalid_result` |
| `llama-index-core/llama_index/core/evaluation/pairwise.py` | `PairwiseComparisonEvaluator` | 成对比较 + 对调消偏 |
| `llama-index-core/llama_index/core/evaluation/pairwise.py` | `_default_parser_function` | `[[A]]`/`[[B]]`/`[[C]]` 三态解析 |
| `llama-index-core/llama_index/core/evaluation/pairwise.py` | `_resolve_results` | 两轮计票，`1 - score` 换算，区分平局与不确定 |
| `llama-index-core/llama_index/core/evaluation/pairwise.py` | `EvaluationSource` | `ORIGINAL` / `FLIPPED` / `NEITHER` 标记结论来源 |
| `llama-index-core/llama_index/core/evaluation/pairwise.py` | `_enforce_consensus` 分支 | 原轮无效不对调，对调轮无效则整体无效 |

## 参考

- [llama-index 源码：evaluation/faithfulness.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/evaluation/faithfulness.py)
- [llama-index 源码：evaluation/pairwise.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/evaluation/pairwise.py)
- [论文：Judging LLM-as-a-Judge（位置偏差）](https://arxiv.org/abs/2306.05685)
- [llama-index 文档：评测](https://docs.llamaindex.ai/en/stable/module_guides/evaluating/)

## 与主核 / AI 的关系

用 LLM 当裁判与主核里的 **`ANALYZE` 采样估算**有相同的认识论地位：两者都是「用近似来替代精确」，且都可能给出一个**看起来可信但实际偏差很大**的数字。数据库对此的对策是「让估算可被检验」（`EXPLAIN` 能显示基数估算与实际的差），而这里对应的机制正是 `invalid_result` 与 `pairwise_source`——**让调用方能知道这个数字是怎么来的、值不值得信**。

主核里没有「评测」这个环节，因为 SQL 的正确性是二值的（结果对不对）。而 RAG 的答案没有唯一正解，只能靠间接指标——这就是为什么这一整套机制存在。

对 agent 工程的直接价值有三条。其一，**「不合格」与「评测失效」必须分开**。把无法解析的输出当成 `passing=False` 会让评测指标系统性偏低；`invalid_result` 这个字段就是为这个区分存在的，而 `Faithfulness` 恰恰漏用了它。其二，**消偏要改的是「检验方式」而不是「判定逻辑」**。对调顺序再问一遍，不改变「谁更好」的算法，只检验它是否稳定——这个位置是对的，因为判定逻辑本身已经很难再改。其三，**评测提示与模型绑定会破坏可比性**。`TEMPLATES_CATALOG` 按模型名换模板，说明同一评测在不同模型上用了不同措辞；跨版本比较数字前必须先确认提示是否一致。
