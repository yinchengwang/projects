---
project: haystack
feature: llm-evaluators
title: LLM 评测器
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/evaluators/llm_evaluator.py
    symbols: [LLMEvaluator]
  - path: haystack/components/evaluators/context_relevance.py
    symbols: [ContextRelevanceEvaluator]
  - path: haystack/components/evaluators/faithfulness.py
    symbols: [FaithfulnessEvaluator]
---

# LLM 评测器

## 一句话职责

用一个 `LLMEvaluator` 把「我有一组输入、想让模型逐条判个是/非」这件事做成组件：**输入与输出都是用户在构造时声明的**，于是同一个类能评测任何指标；两个具体评测器（上下文相关性、忠实性）在其上只加一层后处理——把「模型返回的 JSON 字典」折算成一个 `0/1` 分数与一个 `nan` 感知的均值。

## 机制是什么

`LLMEvaluator` 是一个 `@component`，但它的输入 socket 是**构造时动态注册**的：

```python
component.set_input_types(self, **dict(inputs))
```

`inputs` 的类型是 `list[tuple[str, type[list]]]` ——每个输入是「名字 + 列表类型」。所以一个 `LLMEvaluator` 实例的输入形状由用户在 `__init__` 里决定。这解释了为什么 `run` 的签名是 `def run(self, **inputs: Any)` ——它无法提前知道有哪些键。

输出 socket 则是固定的三个：`results`（每个输入一条结果字典）、`meta`（可选，来自模型回复的元数据）。

评测结果的取值约定写在类 docstring 里：「每条结果是一个字典，用户自定义的键与取值为 `0`（假）或 `1`（真）」。

**默认后端是 OpenAI 的 JSON 模式**，且带一个固定种子：

```python
generation_kwargs = {"response_format": {"type": "json_object"}, "seed": 42}
self._chat_generator = OpenAIChatGenerator(generation_kwargs=generation_kwargs)
```

`response_format=json_object` 让模型必须回 JSON；`seed=42` 让结果尽量可复现——对一个**评测**组件来说，可复现比多样性重要得多。

## 怎么实现的

**五道构造期校验，每一道都针对一种「用户会写错」的形态。** `validate_init_parameters` 是 `@staticmethod`，检查四件事：

**输入**——必须是列表，每项是长度 2 的元组，第一个元素是 `str`，第二个是 `list` 或 `list` 的泛型：

```python
(_input[1] is list or get_origin(_input[1]) is list)
```

那个 `get_origin(...) is list` 是为了放行 `list[str]` 这种参数化类型——`list[str] is list` 为假，但 `get_origin(list[str])` 返回 `list`。所以两种写法都算合法。

**输出**——必须是字符串列表。

**例子**——必须是字典列表。

**每个例子的形状**——这一条写得最紧：

```python
if ({"inputs", "outputs"} != example.keys() or ...)
```

用的是 `!=` 集合比较，所以**不仅要包含这两个键，还不能有第三个**。一个带 `"note"` 或 `"id"` 的例子会被拒绝。这是「严格契约」而不是「至少包含」。

后面还有两层：`inputs` 与 `outputs` 的值都必须是字典，且它们的**所有键**都必须是 `str`。

**运行期的输入校验是三个独立断言。** `validate_input_parameters(expected, received)` 依次检查：所有期望的输入名都在 `received` 里；所有值都是 `list`；所有列表长度相同且非空（`length == 0` 也报错）。

第三条很实在：**批量评测要求所有输入等长并行**，因为 `run` 的实现是

```python
input_names, values = inputs.keys(), list(zip(*inputs.values(), strict=True))
list_of_input_names_to_values = [dict(zip(input_names, v, strict=True)) for v in values]
```

`zip(*inputs.values())` 把「N 个并行列表」转成「N 个元组」，每个元组是第 i 条的全部输入。`strict=True` 在这里是必需的——若列表长度不等，`zip` 默认会静默截断到最短，导致**少评几条而不报错**。加了 `strict=True` 才会抛错。

但注意：`validate_input_parameters` 已经检查过等长，所以 `strict=True` 在这条路径上是冗余的。它是一道**防御性的第二保险**——若将来有人改了校验逻辑，这里仍会炸而不是静默丢数据。

**`prepare_template` 拼出的提示词把「要输出什么键」写死在文本里，而输入部分用模板占位符。**

输入段的构造是这段最绕的一行：

```python
inputs_section = "{" + ", ".join([f'"{input_socket[0]}": {{{{ {input_socket[0]} }}}}' for input_socket in self.inputs]) + "}"
```

四个花括号 `{{{{ ... }}}}` 在 f-string 里展开成 `{{ ... }}` ——也就是 PromptBuilder 的占位语法。所以最终产物是形如 `{"questions": {{ questions }}, "contexts": {{ contexts }}}` 的一段。

这个设计的巧妙处在于：**提示词里给模型看的输入是一个 JSON 对象的骨架，而占位符就写在值的位置**。模型看到的输入与它要输出的格式一致（都是 JSON 对象），所以「照着例子的形状回答」这件事变得自然。

例子段则是把每个例子的 `inputs` / `outputs` 各自 `json.dumps` 后拼成：

```
Inputs:
<json>
Outputs:
<json>
```

而整段模板以 `Outputs:\n` 结尾——**留了一个「该你填」的空位**，与前面几页里「拼提示词末尾补 `assistant:`」是同一手法。

`json.dumps(self.outputs)` 把输出键列表直接写进指令：

```
Generate the response in JSON format with the following keys:
["score"]
```

所以「要返回哪些键」不是靠自然语言描述，而是**把键名当作 JSON 数组写进提示词**。这让模型有一个精确的字符串目标可比对。

**`run` 的主循环有两条独立的容错路径，且它们的粒度不同。** 循环体：

```python
prompt = self.builder.run(**input_names_to_values)
messages = [ChatMessage.from_user(prompt["prompt"])]
try:
    with _trace_chat_generator_run(self._chat_generator, {"messages": messages}) as span:
        result = self._chat_generator.run(messages=messages)
        span.set_content_tag("haystack.component.output", result)
except Exception as e:
    if self.raise_on_failure:
        raise ValueError(f"Error while generating response for prompt: {prompt}. Error: {e}") from e
    logger.warning(...)
    results.append(None)
    errors += 1
    continue

parsed_result = self._parse_reply(result["replies"][0].text)
if parsed_result is None:
    results.append(None)
    errors += 1
else:
    results.append(parsed_result)
```

第一条容错针对**调用失败**（网络、限流、认证）；第二条针对**解析失败**（模型返回了非 JSON 或缺键）。

两条都遵守 `raise_on_failure`，但第一条在优雅降级时 `continue`（跳过后续处理），第二条不 `continue`（因为后面只是 `if result["replies"][0].meta:` 这个可选收集）。这个差别是对的：调用失败时没有 `result` 可用，必须跳过；解析失败时 `result` 还在，元数据仍值得收集。

**「失败的那条记为 `None`」这个约定贯穿整个组件。** 类 docstring 写：「若某个输入值发生异常，那条的结果将是 `None`」。而 `meta` 只收集成功响应的元数据，所以 `meta` 列表可能比 `results` 短——这是一个隐含的不对齐，`run` 的返回是 `{"results": results, "meta": metadata or None}`，两者长度不一致。

**`_parse_reply` 把「解析失败」收敛成 `None` 或异常。** 实现：

```python
try:
    return _parse_dict_from_json(reply_text, expected_keys=self.outputs, raise_on_failure=True)
except ValueError as e:
    if self.raise_on_failure: raise
    logger.warning("{component} failed to parse the ChatGenerator response: {response}. Setting the result for this input to None. Error: {error}", ...)
    return None
```

注意它传给 `_parse_dict_from_json` 的是 `raise_on_failure=True`（**强制抛**），然后自己在 `except` 里按 `self.raise_on_failure` 决定重抛还是吞掉。这是一个「把内部函数的容错关掉、把决策权留在本层」的写法——比让内部函数直接返回 `None` 更好，因为本层还要记一条带 `component` 名的警告。

而 `expected_keys=self.outputs` 意味着解析器会检查「返回的 JSON 是否包含所有声明的输出键」。所以「模型返回了合法 JSON 但少了 `score` 键」也算解析失败。

**循环结束后有一条汇总警告，这条设计值得单独提。**

```python
if errors > 0:
    logger.warning("{component} failed for {errors} out of {total} inputs. The results for these inputs are None.", component=type(self).__name__, errors=errors, total=len(list_of_input_names_to_values))
```

逐条失败已经有警告，再汇总一条的理由是**规模可读性**：评测 500 条里有 30 条失败时，30 条逐条警告会被淹没在日志里，而「30 out of 500」是一眼能看到的。这也提示了「部分失败」在这类批量任务里是常态——不该当成异常终止整批。

**两个具体评测器都是「调父类 + 后处理」，而后处理的核心是 `nan` 的传播。** `ContextRelevanceEvaluator._postprocess_results`：

```python
for idx, res in enumerate(result["results"]):
    if res is not None:
        statements = res["relevant_statements"]
        if isinstance(statements, list):
            res["score"] = 1 if statements else 0
            res["status"] = "evaluated"
            continue
        value_type = type(statements).__name__
        if self.raise_on_failure:
            raise ValueError(f"Input {idx}: expected `relevant_statements` to be a list, got {value_type}.")
        logger.warning(...)
    result["results"][idx] = {"relevant_statements": [], "score": float("nan"), "status": "error"}
```

然后：

```python
scores = [res["score"] for res in result["results"]]
valid_scores = [s for s in scores if not math.isnan(s)]
skipped = len(scores) - len(valid_scores)
if skipped:
    logger.warning("{skipped} query(s) failed and were excluded from the score.", skipped=skipped)
result["score"] = mean(valid_scores) if valid_scores else float("nan")
result["individual_scores"] = scores
```

四个决定：

1. **失败项用 `float("nan")` 而不是 `None` 或 `0`。** 这个选择使「排除失败项」可以靠 `math.isnan` 完成，而 `0` 会被误当真实分数。`None` 也会让后续的 `mean` 崩掉。

2. **`individual_scores` 保留 `nan`**，而 `score` 只对有效项求均值。所以「逐条分数」是完整长度（含 `nan`），「整体分数」是排除后的均值。两者语义不同：前者供 `EvaluationRunResult` 用（需要对齐原始行），后者是给人看的汇总。

3. **`isinstance(statements, list)` 的类型检查是为了「模型返回了字符串」这种情况**。若模型返回 `"relevant_statements": "Rome is the capital"`，那 `if statements` 会为真、分数算 1——但那是错的（不是列表）。所以先验类型。而报错消息带 `Input {idx}` 让用户能定位是哪条。

4. **`res["score"] = 1 if statements else 0`** ——空列表得 0。所以「上下文完全不相关」与「评测失败」被区分开了：前者是 `0`，后者是 `nan`。

**`FaithfulnessEvaluator` 的后处理多一层「每句一个分数」的一致性检查。**

```python
if res is not None and len(res["statements"]) != len(res["statement_scores"]):
    message = f"FaithfulnessEvaluator expected one score per statement, but received {len(res['statements'])} statements and {len(res['statement_scores'])} scores."
    if self.raise_on_failure: raise ValueError(message)
    logger.warning("{error} Setting the result for this input to an error.", error=message)
    res = None
```

然后：

```python
res["status"] = "evaluated"
if not res["statements"]:
    res["score"] = 0
else:
    res["score"] = np_mean(res["statement_scores"])
```

这里的检查是必需的，因为指标定义是「**声明的每一条是否都被上下文支持**」——分数的分母是 `statements` 的数量。若模型给了 5 条声明但只给 4 个分数，后续任何计算都无意义。所以这条检查挡的是「模型输出自相矛盾」而非「格式错误」。

而 `if not res["statements"]: res["score"] = 0` ——**声明为空时分数为 0**。这与 `ContextRelevanceEvaluator` 里「`relevant_statements` 为空得 0」同构，但语义相反：那边空 = 没有相关语句 = 不相关（0 合理）；这边空 = 回答里没有可验证的声明 = 忠实性无从谈起，取 0 是一个保守的默认（而不是 `nan` 的「不知道」）。

**两个评测器的 `run` / `run_async` 各写一遍，且都用 `super(ClassName, self)` 显式形式。**

```python
result = super(ContextRelevanceEvaluator, self).run(**inputs)  # noqa: UP008
return self._postprocess_results(result)
```

`# noqa: UP008` 表明 lint 建议改成 `super().run(...)`，而这里保留了显式形式。两种写法等价，显式形式的唯一好处是在阅读时不必往上找类名。

同步与异步两个方法各写一份是这一层的实际成本：**后处理逻辑只有一份（`_postprocess_results`），但「调父类再后处理」这个包装写了四次**（两个类 × 同步/异步）。这说明「模板方法」在这里只被用在最内层（后处理），外层的接线仍然是重复的。

**`to_dict` 里有一句直白的注释解释了一个序列化妥协。**

```python
# Since we cannot currently serialize tuples, convert the inputs to a list.
inputs = [[name, serialize_type(type_)] for name, type_ in self.inputs]
```

所以 `inputs` 在内存里是元组列表，序列化时转成列表，而 `from_dict` 再转回元组：

```python
data["init_parameters"]["inputs"] = [(name, deserialize_type(type_)) for name, type_ in data["init_parameters"]["inputs"]]
```

`serialize_type` / `deserialize_type` 处理的是「第二个元素是一个类型对象」这件事——类型不可 JSON 序列化，所以转成某种字符串表示。这套机制是这一层能「把输入形状存进 YAML」的关键：**一个评测器的完整定义包含它的输入类型声明**，而那些类型必须能被写下来再读回来。

## 关键取舍

**源码事实：** `LLMEvaluator` 的输入 socket 在 `__init__` 里通过 `component.set_input_types` 动态注册，所以 `run` 只能写成 `**inputs`。

**源码事实：** 默认后端是 OpenAI 的 JSON 模式，`generation_kwargs` 带 `seed: 42`。

**源码事实：** `examples` 的形状用了 `{"inputs", "outputs"} != example.keys()` 的集合相等，所以多余键会被拒绝。

**源码事实：** `zip(*inputs.values(), strict=True)` 的两处 `strict=True` ——校验层已保证等长，这是防御性第二保险。

**源码事实：** 提示词里输入段用 `{{ ... }}` 占位符嵌在一个 JSON 对象骨架里；输出键列表用 `json.dumps(self.outputs)` 直接写进指令。

**源码事实：** 两条容错路径（调用失败 / 解析失败）都遵守 `raise_on_failure`，但前者 `continue` 跳过后续、后者不跳过（还要收集 meta）。

**源码事实：** 失败项在 `results` 里记 `None`，而 `meta` 只收成功项，所以两者长度可能不一致。

**源码事实：** `_parse_reply` 强制内部解析器抛异常，再由本层按配置决定重抛或吞掉并记带组件名的警告。

**源码事实：** 逐条警告之外还有一条汇总警告（`errors out of total`）。

**源码事实：** 两个具体评测器用 `float("nan")` 表示失败项，`individual_scores` 保留 `nan`，`score` 只对有效项求均值。

**源码事实：** `FaithfulnessEvaluator` 检查 `statements` 与 `statement_scores` 等长，不等则按配置报错或降级为 `nan`。

**架构推论：** 「输入形状由构造参数决定」是这一层最有意思的设计。它让一个类覆盖无数指标，代价是**类型信息脱离静态检查**——`run(**inputs)` 的键在类型层面是 `Any`，而真实约束只在运行期的 `validate_input_parameters` 里。这是「配置驱动的通用组件」的固有代价：通用性与静态可检查性在这里是互斥的，而 haystack 选了通用性（因为评测指标本来就千变万化）。

**架构推论：** `nan` 而不是 `0` 或 `None` 表示失败，是这一层最实用的决定。`0` 会让「评测失败」被当成「确实不相关」，从而**虚低**平均分；`None` 会让均值计算崩掉。`nan` 在数值语义里恰好就是「这个位置无效」，而 `math.isnan` 过滤是标准做法。任何「批量打分」的系统都该用这个约定——关键是同时保留一份带 `nan` 的逐条分数（供对齐）与一份排除后的均值（供汇总）。

**架构推论：** 「两套评测逻辑 = 一个通用基类 + 每类一层后处理」的结构说明指标之间的真实差异在哪。基类负责的是**所有 LLM 评测共有的部分**（拼提示词、批量调模型、解析 JSON、容错）；而指标间的差别只体现为「怎么把返回的 JSON 折算成分数」。所以加一个新指标不需要重新实现调用与容错——这层抽象切在了正确的位置。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/evaluators/llm_evaluator.py` | `LLMEvaluator` | 输入输出形状由构造参数决定的通用评测器 |
| `haystack/components/evaluators/llm_evaluator.py` | `LLMEvaluator.validate_init_parameters` | 四类构造期校验，例子的键集合必须恰好两个 |
| `haystack/components/evaluators/llm_evaluator.py` | `LLMEvaluator.validate_input_parameters` | 运行期三断言：键齐、都是列表、等长非空 |
| `haystack/components/evaluators/llm_evaluator.py` | `LLMEvaluator.prepare_template` | 拼提示词，输入用 `{{ }}` 占位符嵌进 JSON 骨架 |
| `haystack/components/evaluators/llm_evaluator.py` | `LLMEvaluator._parse_reply` | 强制内部抛错，本层按配置决定重抛或吞 |
| `haystack/components/evaluators/llm_evaluator.py` | `LLMEvaluator.run` | 并行 zip 输入、两条容错路径、汇总警告 |
| `haystack/components/evaluators/llm_evaluator.py` | `LLMEvaluator.to_dict` | 元组转列表并把类型序列化 |
| `haystack/components/evaluators/context_relevance.py` | `ContextRelevanceEvaluator` | 语句级相关性，仅覆写后处理 |
| `haystack/components/evaluators/context_relevance.py` | `_postprocess_results` | 空语句集得 0，失败项得 `nan` |
| `haystack/components/evaluators/faithfulness.py` | `FaithfulnessEvaluator` | 语句级忠实性，仅覆写后处理 |
| `haystack/components/evaluators/faithfulness.py` | `_DEFAULT_EXAMPLES` | 内置少样本示例 |
| `haystack/components/evaluators/faithfulness.py` | `FaithfulnessEvaluator._postprocess_results` | 语句数与分数数的一致性检查 |

## 参考

- [haystack 源码：components/evaluators/llm_evaluator.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/evaluators/llm_evaluator.py)
- [haystack 文档：评测器](https://docs.haystack.deepset.ai/docs/evaluators)
- [haystack 源码：components/evaluators/context_relevance.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/evaluators/context_relevance.py)

## 与主核 / AI 的关系

主核里最接近的是「EXPLAIN 的代价估算与统计收集」：用一套通用机制对任意查询给出量化评估，而具体指标（行数估算、选择率、代价）由不同的估算器提供。

差别在于**评估结果的可靠性**。主核的代价估算是确定性的（同样的统计与查询得同样的数）；LLM 评测的本质是「用另一个模型判断」，所以结果是概率性的——这正是 `seed: 42` 与 `nan` 语义存在的原因。**一个不确定的计算必须能把「这次算失败了」与「算出来的值是 0」区分开**，否则汇总就失去意义。

与本组 i02（检索指标）的分工很清楚：那一页的指标（Recall / MRR / MAP / EM）是**纯字符串与集合运算，完全确定性**；这一页是「请模型判断」。所以两页在容错上的处理完全不同——确定性指标不需要 `nan`，因为它们的失败就是异常。

与 h01（智能体组件）的呼应：两者都是 `@component` 但都做了「超越单次 `run`」的事——Agent 内部循环，评测器内部批量迭代。区别是 Agent 的状态要跨步骤保留（所以需要 State），而评测器的循环是无状态的（每条独立）。

对 AI 工程最实用的一条是「用 `nan` 表示评测失败，并且同时保留逐条与汇总两份分数」。逐条那份要带 `nan`（因为下游要按行对齐原始数据），汇总那份要排除 `nan`（因为把失败当 0 会虚低分数）。若只保留一份，要么丢失对齐关系，要么汇总被污染。

另一条是「模型返回的 JSON 必须校验键集合而不只是可解析性」。`expected_keys=self.outputs` 让「合法 JSON 但缺键」也算失败——这是必要的，因为模型很容易返回一个 `{"reasoning": "..."}` 而不带 `score`。只校验 JSON 合法性的话，这种输出会被当成成功，而下游取 `res["score"]` 就 KeyError。

最后一条是「批量任务的失败要汇总报」。逐条警告在几十条时有用，在几百条时会淹没；而「30 out of 500 failed」这一行是运维时真正会看的。两者都写，成本几乎为零。
