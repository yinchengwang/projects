---
project: llama-index
feature: llm-rerank
title: LLM 重排
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/postprocessor/llm_rerank.py
    symbols: [LLMRerank]
  - path: llama-index-core/llama_index/core/postprocessor/structured_llm_rerank.py
    symbols: [StructuredLLMRerank, DocumentWithRelevance, default_parse_structured_choice_select_answer]
  - path: llama-index-core/llama_index/core/postprocessor/rankGPT_rerank.py
    symbols: [RankGPTRerank, get_default_llm]
---

# LLM 重排

## 一句话职责

三种用大模型给检索结果重新打分的策略：**批内打分**（`LLMRerank`，把 10 条拼进一个提示让模型给分）、**结构化打分**（`StructuredLLMRerank`，靠函数调用拿到强类型的文档号 + 相关度）、**排列重排**（`RankGPTRerank`，让模型直接输出「这些段落该怎么排」）。三者的差别不在「用不用 LLM」，而在**向模型要什么样的答案**。

## 机制是什么

| 类 | 向模型要什么 | 解析方式 | 输出分数 |
|---|---|---|---|
| `LLMRerank` | 一段文本：「哪些文档相关、各给几分」 | 正则/文本解析（`default_parse_choice_select_answer_fn`） | 模型给的相关度 |
| `StructuredLLMRerank` | 一个 `DocumentRelevanceList` 对象 | 函数调用 + pydantic 校验 | 模型给的相关度（1–10） |
| `RankGPTRerank` | 一个整数排列（如 `[2] > [1] > [3]`） | 只提取数字，去重、补全 | **沿用原分数**，只改顺序 |

三者的共同前提是「检索器的向量分数不够准」，而共同代价是**每条候选都要过一遍模型**——所以 `choice_batch_size`（默认 10）与 `top_n`（默认 10 / 5）是这类组件的核心旋钮。

## 怎么实现的

### 一、`LLMRerank`：批内打分，文本解析

**它按 `choice_batch_size` 分批，每批一次 LLM 调用。**

```
def _get_node_batches(self, nodes):
    return [[node.node for node in nodes[idx: idx + self.choice_batch_size]] for idx in range(0, len(nodes), self.choice_batch_size)]
```

**为什么要分批而不一次全给？** 两个限制同时存在：提示长度（10 条节点的正文已经很长）与**模型对「编号引用」的准确度**（引用 50 个编号里的第 37 个，错误率显著上升）。10 是个折中值。

**提示的选择是按是否 chat 模型自动切换的。**

```
choice_select_prompt = choice_select_prompt or SelectorPromptTemplate(
    default_template=DEFAULT_CHOICE_SELECT_PROMPT,
    conditionals=[(is_chat_model, CHAT_CONTENT_CHOICE_SELECT_PROMPT)],
)
```

`SelectorPromptTemplate` 是一个「按条件选模板」的容器：非 chat 模型走 `DEFAULT_CHOICE_SELECT_PROMPT`，chat 模型走 `CHAT_CONTENT_CHOICE_SELECT_PROMPT`。这个自动选择省掉了使用者必须知道「我的模型是哪种」这件事。

**节点格式化的函数也是按 chat 与否二选一的。**

```
self._format_node_batch_fn = format_node_batch_fn or (
    default_format_node_batch_for_chat_fn if is_chat_model(llm) else default_format_node_batch_fn
)
```

而 `_get_predict_kwargs` 里的键名也跟着变：

```
if is_chat_model(self.llm): kwargs["context_messages"] = fmt_batch
else: kwargs["context_str"] = fmt_batch
```

代码注释写着 **"Don't include non text types for non-chat models"**——也就是说非 chat 模型只能收纯文本，所以格式化时要把图片之类的非文本块剔除。**同一个逻辑在三个地方（模板、格式化函数、kwargs 键名）都要按 chat 与否分叉**，这是这套抽象里最不干净的一处。

**解析出来的分数是可选的。**

```
raw_choices, relevances = self._parse_choice_select_answer_fn(raw_response, len(nodes_batch))
choice_idxs = [int(choice) - 1 for choice in raw_choices]
choice_nodes = [nodes_batch[idx] for idx in choice_idxs]
relevances = relevances or [1.0 for _ in choice_nodes]
```

`relevances or [...]` 处理「模型只给了编号没给分数」的情形——此时全按 1.0。而这个 1.0 是**字面量哨兵值**，不是「平均分」：它意味着「这些被选中的都算同等相关」。因为下面还要按分数排序，全部相同就等于「保持模型给出的顺序」。

**`int(choice) - 1` 这个减一是提示里的编号从 1 开始的结果。** 若模型输出 `0`（或非数字），这里会得到一个负索引——**没有校验**，而 Python 的负索引会静默地从末尾取。这是一个潜在的静默错误：模型说「第 0 篇」时，会拿到最后那篇。

**`_postprocess_nodes` 与 `_apostprocess_nodes` 是两份几乎相同的实现，差的只是串行 vs 并行。**

- 同步版：`for nodes_batch in ...: self.llm.predict(...)`
- 异步版：`await run_jobs([self.llm.apredict(...) for nodes_batch in ...])`

也就是说 **async 路径把各批并行发出去了**，而同步路径串行。这是一个合理的差别（同步本来就没法并行），但代价是两份逻辑要一起维护。

**两者末尾的排序完全相同**：`sorted(..., key=lambda x: x.score or 0.0, reverse=True)[: self.top_n]`。`x.score or 0.0` 把 `None` 当 0，于是「无分数的」永远排最后。

### 二、`StructuredLLMRerank`：要一个对象，而不是一段文本

**它的设计动机可以从「多出来的那些东西」推出来。** 与 `LLMRerank` 相比，它多了：

1. 一个 `DocumentRelevanceList` / `DocumentWithRelevance` 的 pydantic 模型；
2. `output_cls` 参数（走函数调用结构化输出）；
3. 构造时的 function-calling 检查；
4. **`_raise_on_structured_prediction_failure` 开关**；
5. 两个 instrumentation 事件。

**`DocumentWithRelevance` 的两个字段各有一个细节。**

```
document_number: int = Field(description="The number of the document within the provided list")
relevance: int = Field(
    description="Relevance score from 1-10 ...",
    json_schema_extra={"minimum": 1, "maximum": 10},
)
```

**`json_schema_extra` 而不是 `ge=1, le=10`，而注释把理由写明了：**

> Put min/max as a json schema extra so that pydantic doesn't enforce it but the model sees it. Doesn't need to be strictly enforced but is useful for the model.

这是一个精确的取舍：**范围约束的目的是「告诉模型」，不是「校验模型」**。若用 pydantic 的 `ge`/`le`，一个给出 11 分的模型会让整个结构化解析失败、整批重排作废；把它放进 `json_schema_extra` 则**模型看得到范围、但不满足时不会炸**。这是一个「约束是提示而非门禁」的清醒判断。

**构造时对非 function-calling 模型只警告不报错。**

```
if not llm.metadata.is_function_calling_model:
    logger.warning("StructuredLLMRerank constructed with a non-function-calling LLM. This may not work as expected.")
```

这条警告是有意义的：结构化输出依赖模型支持工具/函数调用，但**具体实现可能靠「提示 + 解析」兜底**（很多 provider 都这么做），所以不必然失败。

**结构化失败的处理是这一页最重要的设计。**

```
if isinstance(result, str):
    start_idx = batch_idx * self.choice_batch_size
    msg = f"Structured prediction failed for nodes {start_idx} - {start_idx + self.choice_batch_size}: {result}"
    if self._raise_on_structured_prediction_failure: raise ValueError(msg)
    logger.warning(msg)
    return [NodeWithScore(node=node, score=0.0) for node in nodes_batch]
```

三件事：

1. **它靠「返回值的类型」判断失败**——`Union[BaseModel, str]`，而 `str` 表示「结构化解析失败、附带异常消息」。这是一个把错误编码进返回类型的做法，而不是 try/except。
2. **默认 `raise_on_structured_prediction_failure=True`**，也就是默认炸。
3. **关掉开关时，整批节点得 0 分并被保留**（不是丢弃）。得 0 分意味着它们会排在最后，但**不会消失**——这对「宁可少排也不要少给」的场景是对的。

**那个 `start_idx` 的算法值得注意：它用 `batch_idx * choice_batch_size` 估算起始下标。** 对满批（每批都是 10 条）是准的，但**最后一批可能不足 10 条**，于是它的区间终点会超出实际数量。错误消息里的区间因此可能越界——一个仅影响可读性的小瑕疵。

**它多了两个 instrumentation 事件，而 `LLMRerank` 没有。**

```
dispatcher.event(ReRankStartEvent(query=query_bundle, nodes=nodes, top_n=self.top_n, model_name=self.llm.metadata.model_name))
```

`_rerank_start_event` 在 `_postprocess_nodes` 的**最开头**就被调用——**在 `query_bundle is None` 的校验之前**。也就是说一个缺 `query_bundle` 的调用会先发出一个 start 事件，然后抛错，而**没有对应的 end 事件**。这是一个事件配对的不对称。

### 三、`RankGPTRerank`：要一个排列，而不是分数

**它与前两者的根本差别是：它不改分数，只改顺序。**

```
for idx in rerank_ranks:
    initial_results.append(NodeWithScore(node=nodes[idx].node, score=nodes[idx].score))
return initial_results[: self.top_n]
```

**分数原样带着走。** 这个选择有一个直接后果：**排序后的分数序列是乱的**（不再单调递减）。若下游有依赖「分数单调」的逻辑（比如按分数阈值过滤），行为会变得奇怪。但这样做的好处是保留了检索器的原始置信度信息——模型只被用来改「谁排在前面」。

**它的默认 LLM 是硬编码的，且是一个老模型。**

```
def get_default_llm() -> LLM:
    from llama_index.llms.openai import OpenAI
    return OpenAI(model="gpt-3.5-turbo-16k")
```

两个细节：这次导入是**函数内延迟的**（`# pants: no-infer-dep` 注释表明依赖检查工具需要知道这是可选依赖）；而 `gpt-3.5-turbo-16k` 是一个**已过时的模型**。这个默认值没有跟着时代更新——它是一个历史遗留，使用者基本都该显式传 `llm`。

**提示的构造是一个「多轮对话伪装」。**

```
messages = self._get_prefix_prompt(query, num)     # system + user + assistant「好，请给段落」
rank = 0
for hit in item["hits"]:
    rank += 1
    messages.append(ChatMessage(role="user", content=f"[{rank}] {content}"))
    messages.append(ChatMessage(role="assistant", content=f"Received passage [{rank}]."))
messages.append(ChatMessage(role="user", content=self._get_post_prompt(query, num)))
```

它把「n 个段落」表达成 n 轮 user/assistant 交替——**先由 user 给一段，assistant 回一句「收到 [k]」**，最后再问「请排序」。这是为了在 chat 模型里制造「逐条投喂」的既有对话结构，而不是把 10 段塞进一条消息。

这个做法的成本是消息数翻倍（每段两条），收益是每段都有明确的编号锚点。而 `_get_prefix_prompt` 里那句 `Assistant: "Okay, please provide the passages."` 是为了让模型进入「等我给段落」的状态。

**每段内容被硬截断到 300 个词。**

```
content = content.replace("Title: Content: ", "")
content = content.strip()
# For Japanese should cut by character: content = content[:int(max_length)]
content = " ".join(content.split()[:300])
```

三件事：**去掉一个特定的前缀串**（`"Title: Content: "`，看起来是某个历史格式的残留）、`strip()`、**按空白切分后取前 300 项再拼回**。

第 3 步的效果是「保留前 300 个词，丢掉其余」。**注释明确指出这对日文不适用**（日文没有空格分词，按 `split()` 只会得到一个巨大的「词」）——作者知道这个限制，并把它写成注释而不是修掉。

**解析是一个「只留数字」的流水线，而它有四道修正。**

```
_clean_response: 把非数字字符全换成空格，只留数字
_receive_permutation:
    response_list = [int(x) - 1 for x in response.split()]
    response_list = _remove_duplicate(response_list)          # 去重（保留首次出现）
    response_list = [ss for ss in response_list if ss in range(rank_end)]   # 丢掉越界
    return response_list + [tt for tt in range(rank_end) if tt not in response_list]   # 补全漏掉的
```

这四步合起来是**把模型输出「修复成一个排列」**：去重（模型可能重复某个编号）、过滤越界（模型可能编出不存在的编号）、补全遗漏（模型可能漏掉某些段落，按原顺序追加在末尾）。

**这个设计的价值在于：无论模型输出多不规范，结果都是原节点列表的一个合法排列。** 对比 `AutoprevNext` 那页里「判不出来就抛错」的做法，这里的策略完全相反——**尽最大努力修复，而不失败**。

代价是：模型漏掉的那几段会被排到最后，而**使用者无法区分「模型认为它最不相关」与「模型根本没提它」**。这两种情况的排序结果相同，但含义完全不同。

**`_llm_result_to_nodes` 对「模型没给出内容」有一个兜底。**

```
if permutation.message is not None and permutation.message.content is not None: ...
else: return nodes[: self.top_n]
```

也就是说模型返回空时退回「原来的前 top_n」。这是三段代码里唯一一处「模型失败就静默降级」的地方——而它降级到的是原顺序，不是错误。

**`apostprocess_nodes` 是一个与 `_apostprocess_nodes` 并存的方法，且它的名字少了一个 p。**

```
async def apostprocess_nodes(self, nodes, query_bundle=None, query_str=None):
    if query_str is not None and query_bundle is not None: raise ValueError(...)
    elif query_str is not None: query_bundle = QueryBundle(query_str)
    ...
```

`apostprocess_nodes`（少一个 p）是**公开的便利方法**、额外接受 `query_str`（会包成 `QueryBundle`，且与 `query_bundle` 互斥）；而 `_apostprocess_nodes`（双 p）是内部实现。**这两个名字只差一个字符**，在实际使用与调试时很容易调错——而调错的表现是「走了另一个入口」或参数校验意外触发。

## 关键取舍

**源码事实：** `LLMRerank` 按 `choice_batch_size`（默认 10）分批，每批一次 LLM 调用；同步串行、异步并行（`run_jobs`）。

**源码事实：** `LLMRerank` 在三个地方按「是否 chat 模型」分叉：提示模板（`SelectorPromptTemplate` 的条件项）、节点格式化函数、kwargs 键名（`context_messages` vs `context_str`）。

**源码事实：** `LLMRerank` 解析时 `int(choice) - 1` 未校验，若模型输出 `0` 会得到负索引并静默取到末尾节点。

**源码事实：** `LLMRerank` 在模型只给编号不给分数时用 `1.0` 哨兵填充，效果等于「保持模型给出的顺序」。

**源码事实：** `StructuredLLMRerank` 的 `relevance` 范围用 `json_schema_extra` 而非 pydantic `ge`/`le`，注释写明「范围是给模型看的，不需要严格强制」。

**源码事实：** 结构化失败以「返回 `str` 而不是 `BaseModel`」表达；默认 `raise_on_structured_prediction_failure=True`；关掉时整批节点得 0 分并被**保留**而非丢弃。

**源码事实：** 失败消息里的 `start_idx` 用 `batch_idx * choice_batch_size` 估算，最后一批不足时区间会越界。

**源码事实：** `StructuredLLMRerank` 的 `ReRankStartEvent` 在校验之前发出，因此缺 `query_bundle` 时会有一个没有配对 end 的 start 事件。

**源码事实：** `RankGPTRerank` **不改分数**，只改顺序——排序后分数不再单调，但保留了检索器原始置信度。

**源码事实：** 它的默认 LLM 是硬编码的 `gpt-3.5-turbo-16k`，函数内延迟导入并标注 `no-infer-dep`。

**源码事实：** 它用「n 轮 user/assistant 交替」表达多个段落，且每段硬截断到 300 个词；注释指出这个截断对日文不适用。

**源码事实：** 排列解析是四道修正（去重、去越界、补全遗漏），结果是**总能得到一个合法排列**；代价是无法区分「最低相关」与「模型漏掉」。

**架构推论：** 三种策略的差别本质上是在回答一个问题：**「模型输出的可靠性」与「输出信息的丰富度」怎么权衡**。

- `LLMRerank` 要「编号 + 分数」，信息最丰富而解析最脆弱（依赖文本格式）。
- `StructuredLLMRerank` 要一个强类型对象，解析可靠但依赖模型支持函数调用，且失败处理需要开关。
- `RankGPTRerank` 只要「顺序」——这是三者中信息量最小、容错最强的形态（任何数字串都能修复成排列），代价是丢掉了分数。

而它们的共同前提值得记：**三者都在为「向量检索的分数不准」付 LLM 的费用**。这笔费用的大小由 `choice_batch_size` × 批数决定——也就是说，重排的成本与候选数量线性相关。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/postprocessor/llm_rerank.py` | `LLMRerank` | 批内打分，chat/非 chat 三处分叉 |
| `llama-index-core/llama_index/core/postprocessor/llm_rerank.py` | `_get_node_batches` | 按 `choice_batch_size` 分批 |
| `llama-index-core/llama_index/core/postprocessor/llm_rerank.py` | `_parse_raw_response` | 编号转索引，分数缺失时用 `1.0` |
| `llama-index-core/llama_index/core/postprocessor/llm_rerank.py` | `_apostprocess_nodes` | 异步版，各批并行 |
| `llama-index-core/llama_index/core/postprocessor/structured_llm_rerank.py` | `StructuredLLMRerank` | 结构化打分，含失败开关与仪表事件 |
| `llama-index-core/llama_index/core/postprocessor/structured_llm_rerank.py` | `DocumentWithRelevance` | 文档号 + 1–10 相关度，范围进 `json_schema_extra` |
| `llama-index-core/llama_index/core/postprocessor/structured_llm_rerank.py` | `default_parse_structured_choice_select_answer` | 过滤越界编号，返回编号与分数两个列表 |
| `llama-index-core/llama_index/core/postprocessor/structured_llm_rerank.py` | `_parse_batch_result` | 失败返回整批 0 分并保留节点 |
| `llama-index-core/llama_index/core/postprocessor/rankGPT_rerank.py` | `RankGPTRerank` | 排列重排，不改分数 |
| `llama-index-core/llama_index/core/postprocessor/rankGPT_rerank.py` | `get_default_llm` | 硬编码 `gpt-3.5-turbo-16k` 的延迟导入 |
| `llama-index-core/llama_index/core/postprocessor/rankGPT_rerank.py` | `create_permutation_instruction` | n 轮交替消息 + 每段截断 300 词 |
| `llama-index-core/llama_index/core/postprocessor/rankGPT_rerank.py` | `_receive_permutation` | 四道修正把任意数字串修成合法排列 |

## 参考

- [llama-index 源码：postprocessor/llm_rerank.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/postprocessor/llm_rerank.py)
- [论文：RankGPT（LLM 作为重排器）](https://arxiv.org/abs/2304.09542)
- [llama-index 文档：节点后处理器](https://docs.llamaindex.ai/en/stable/module_guides/querying/node_postprocessors/)
- [pydantic 文档：JSON Schema 附加信息](https://docs.pydantic.dev/latest/concepts/json_schema/)

## 与主核 / AI 的关系

把「排序」交给一个非确定性组件，与主核里「用采样估算而非精确统计」有相同的味道：都是拿**不确定性换取一种更廉价的近似**。数据库的 `ANALYZE` 也是采样估算行数，而它的结果只用于选择计划，不影响正确性。

这三个重排器的位置更接近「影响结果质量但不影响正确性」——排错了会给模型更差的上下文，但不会给出错误的数据结构。这个位置决定了容错策略的宽松度：`RankGPTRerank` 敢把任何数字串修成排列，就是因为最坏结果只是「排序不理想」。

对 agent 工程的直接价值有三条。其一，**要模型给什么，决定了容错策略**。要「编号+分数」就必然依赖文本格式，要「一个对象」就依赖函数调用，只要「顺序」则几乎任何输出都能修复——**输出形态越简单，系统越稳**。其二，**范围约束应当是提示而非门禁**。`json_schema_extra` 里写 `minimum`/`maximum` 让模型看到范围却不强制校验，这个区别很实用：强制校验会让一个 11 分把整批结果作废。其三，**「模型漏掉」与「模型判为最不相关」不可区分**。`_receive_permutation` 把漏掉的补在末尾，于是这两种情形产出相同结果——若业务上需要区分，就得让模型显式输出全部编号，而这个要求要写进提示并校验完整性。
