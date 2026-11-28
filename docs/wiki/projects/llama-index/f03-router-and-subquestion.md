---
project: llama-index
feature: router-and-subquestion
title: 路由查询与子问题分解
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/query_engine/router_query_engine.py
    symbols: [RouterQueryEngine, RetrieverRouterQueryEngine, ToolRetrieverRouterQueryEngine]
  - path: llama-index-core/llama_index/core/query_engine/sub_question_query_engine.py
    symbols: [SubQuestionQueryEngine, SubQuestionAnswerPair]
---

# 路由查询与子问题分解

## 一句话职责

两个「把一个问题分给谁答」的策略：**路由**（从若干个查询引擎里选一个或几个来答，再可选地汇总）、**子问题分解**（把复杂问题拆成若干个各自的子问题，逐个派给对应引擎，最后把「子问题+答案」当成新节点交给合成器）。

## 机制是什么

| 类 | 决策依据 | 执行几个引擎 | 结果如何合并 |
|---|---|---|---|
| `RouterQueryEngine` | **选择器**（`BaseSelector`，通常是 LLM 选） | 一个，或 `select_multi` 时多个 | 多个时用 `TreeSummarize` 汇总 |
| `RetrieverRouterQueryEngine` | **一个检索器**（检索出节点 → 节点转引擎） | **恰好一个**（多于一个直接报错） | 无需合并 |
| `ToolRetrieverRouterQueryEngine` | **工具检索器**（检索出 `QueryEngineTool`） | 检索出几个就跑几个 | 多个时汇总 |
| `SubQuestionQueryEngine` | **问题生成器**（LLM 把复杂问题拆成子问题 + 指定工具） | **每个子问题一个** | 「子问题 + 答案」变成节点，交给配置的合成器 |

三者都实现 `BaseQueryEngine`，所以可以互相嵌套——一个子问题引擎的路由目标里可以再放一个路由引擎。

## 怎么实现的

### 一、`RouterQueryEngine`：选择器 + 可选汇总

**它把 `QueryEngineTool` 拆成两个平行列表。**

```
self._query_engines = [x.query_engine for x in query_engine_tools]
self._metadatas = [x.metadata for x in query_engine_tools]
```

拆开的理由是：**选择器只看元数据**（引擎的名字与描述），不看引擎本身。于是「选谁」与「谁能答」是两个可以分别替换的东西——换选择策略不必换引擎，反之亦然。

**`_query` 的主流程是「选 → 跑 → 可选合并 → 打元数据」。**

```
result = self._selector.select(self._metadatas, query_bundle)

if len(result.inds) > 1:
    responses = []
    for i, engine_ind in enumerate(result.inds):
        log_str = f"Selecting query engine {engine_ind}: {result.reasons[i]}."
        ...
        selected_query_engine = self._query_engines[engine_ind]
        responses.append(selected_query_engine.query(query_bundle))
    if len(responses) > 1:
        final_response = combine_responses(self._summarizer, responses, query_bundle)
    else:
        final_response = responses[0]
else:
    try:
        selected_query_engine = self._query_engines[result.ind]
        ...
    except ValueError as e:
        raise ValueError("Failed to select query engine") from e
    final_response = selected_query_engine.query(query_bundle)
```

三处值得记：

1. **`result.inds` 与 `result.ind` 是两个字段**（多选与单选）。代码分别处理，而 `result.reasons[i]` 与 `result.reason` 也是两个——多选时每个选择有各自的理由。
2. **`try/except ValueError` 只包住了「按索引取引擎」这一步**，而把真正的 `query()` 留在外面。也就是说它防的是「选择器给了个越界索引」，不防引擎自身的错误。而它把 `ValueError` 重新包装成 `Failed to select query engine`——**原始异常被 `from e` 保留，但消息被换掉了**。
3. **「多个引擎但只有一个响应」这个分支是必要的防御**：`len(result.inds) > 1` 但实际 `responses` 只有一条的情形理论上不该出现，但代码仍然处理了。

**`combine_responses` 是一个独立函数，它做两件事：收集来源、用 summarizer 汇总。**

```
for response in responses:
    if isinstance(response, (StreamingResponse, PydanticResponse)):
        response_obj = response.get_response()
    elif isinstance(response, AsyncStreamingResponse):
        raise ValueError("AsyncStreamingResponse not supported in sync code.")
    else:
        response_obj = response
    source_nodes.extend(response_obj.source_nodes)
    response_strs.append(str(response))
```

**流式响应在这里被强制取成完整响应**（`get_response()`）——因为汇总需要完整文本。于是**一个流式引擎经路由汇总之后就不再流式了**（除了最后 `summary` 恰好是生成器的情形）。

而 `AsyncStreamingResponse` 在同步路径里直接报错，理由是它无法在同步上下文里被消费。这是一个正确的拒绝——比「静默等待」好。

**汇总结果的包装又是四选一分派**（`str` / `BaseModel` / `Generator` / 其它当异步生成器）。注意最后一个分支是 `else` 兜底而不是 `isinstance` 检查——**任何非前三种的东西都被当成异步生成器**。若 summarizer 返回了别的类型（比如 `None`），这里会构造一个包着 `None` 的 `AsyncStreamingResponse`。

**`_aquery` 与 `_query` 的差别不只是 `await`：多引擎分支变成了 `asyncio.gather`。**

```
tasks.append(selected_query_engine.aquery(query_bundle))
responses = await asyncio.gather(*tasks)
```

于是**多个引擎在异步路径下是并行的**，而同步路径是串行。这个差别与合成器那页的 `Refine` 同步/异步差别是同一种。

**`final_response.metadata["selector_result"] = result` 是每次都会做的一步。**

```
final_response.metadata = final_response.metadata or {}
final_response.metadata["selector_result"] = result
```

把**选择结果整个塞进响应的元数据**。于是调用方能知道「选了谁、为什么」——这是路由这类「非确定性决策」必需的输出。`metadata or {}` 那一步是因为某些响应可能没有 metadata。

**`from_defaults` 里有一句几乎多余的 `assert`。**

```
selector = selector or get_selector_from_llm(llm, is_multi=select_multi)
assert selector is not None
return cls(selector, query_engine_tools, llm=llm, summarizer=summarizer, **kwargs)
```

`get_selector_from_llm` 的返回类型标注大概是 `Optional`，所以加了 `assert` 让类型检查器满意。**这在运行期不产生任何行为**——除非 `get_selector_from_llm` 真的返回 `None`（那时 `cls` 会拿到一个 `None` selector 并在查询时崩）。

**`_get_prompt_modules` 有一句注释值得记：`# NOTE: don't include tools for now`。** 它返回 `{"summarizer": ..., "selector": ...}` 而不含工具——也就是说**工具的提示不在这棵树的提示收集范围内**。注释承认了这一点，但没解释为什么。

### 二、`RetrieverRouterQueryEngine`：单节点路由（已废弃）

**它的文档第一句就是废弃声明：`NOTE: this is deprecated, please use our new ToolRetrieverRouterQueryEngine`。**

而它的机制非常受限：

```
nodes_with_score = self._retriever.retrieve(query_bundle)
# TODO: for now we only support retrieving one node
if len(nodes_with_score) > 1:
    raise ValueError("Retrieved more than one node.")
node = nodes_with_score[0].node
query_engine = self._node_to_query_engine_fn(node)
return query_engine.query(query_bundle)
```

**它要求检索器恰好返回一个节点**，多于一个直接报错。这是它被废弃的核心原因——检索器通常返回多个（`top_k` 默认大于 1），于是这个引擎几乎总要报错。

`_aquery` 直接调 `self._query`——**它没有真正的异步实现**，只是把同步路径包在 `async def` 里。这会阻塞事件循环，是一个明确的偷懒。

**`default_node_to_metadata_fn` 是它的配套函数，而从节点到工具的映射靠一个约定的元数据键。**

```
metadata = node.metadata or {}
if "tool_name" not in metadata: raise ValueError("Node must have a tool_name in metadata.")
return ToolMetadata(name=metadata["tool_name"], description=node.get_content())
```

也就是说**这个引擎要求「每个节点是一个查询引擎的代理」**：节点必须带 `tool_name`，描述用节点正文。这是一个把「文档」当「工具清单」用的做法。

### 三、`ToolRetrieverRouterQueryEngine`：检索出工具再跑

**它是上面那个的替代品，区别是「检索的产物是 `QueryEngineTool` 而不是普通节点」。**

```
query_engine_tools = self._retriever.retrieve(query_bundle)
responses = []
for query_engine_tool in query_engine_tools:
    query_engine = query_engine_tool.query_engine
    responses.append(query_engine.query(query_bundle))
if len(responses) > 1:
    final_response = combine_responses(self._summarizer, responses, query_bundle)
else:
    final_response = responses[0]
```

**它没有「多于一个就报错」的限制**——检索出几个就跑几个，多个就汇总。这是与废弃版最实质的进步。

**但它有一个空结果的漏洞：`responses` 为空时 `responses[0]` 会抛 `IndexError`。** 代码里没有对空列表的处理。一个检索不到任何工具的查询会让这个引擎崩掉——而这正是路由系统里最常见的情形之一（问题与所有候选都不相关）。

**它把检索到的工具列表写进响应元数据**（`metadata["retrieved_tools"] = query_engine_tools`），与 `RouterQueryEngine` 写 `selector_result` 是同一种可观测性做法。

**三个路由引擎都没有真正的异步查询：`ToolRetrieverRouterQueryEngine._aquery` 与 `_query` 逐句相同、只是各自 `await` 引擎的异步接口。** 多个引擎时它是串行 await，不是 `gather`。这是与 `RouterQueryEngine` 的一个实际差别（后者在多引擎时确实并行）。

### 四、`SubQuestionQueryEngine`：拆问题

**它的核心是一个三元组 `SubQuestionAnswerPair`。**

```
sub_q: SubQuestion            # 子问题 + 目标工具名
answer: Optional[str] = None
sources: List[NodeWithScore] = Field(default_factory=list)
```

`answer` 是 `Optional` 而 `sources` 默认空列表——也就是说这个结构**可以表示「问了但还没答」与「答了但没有来源」**。而它在回调事件里被用作 payload 的两个阶段（开始时 `answer=None`、结束时带答案）。

**主流程是「生成子问题 → 并发/串行执行 → 过滤失败 → 把结果变成节点 → 交给合成器」。**

```
sub_questions = self._question_gen.generate(self._metadatas, query_bundle)
...
qa_pairs = list(filter(None, qa_pairs_all))       # 过滤掉失败的
nodes = [self._construct_node(pair) for pair in qa_pairs]
source_nodes = [node for qa_pair in qa_pairs for node in qa_pair.sources]
response = self._response_synthesizer.synthesize(query=query_bundle, nodes=nodes, additional_source_nodes=source_nodes)
```

四个要点：

1. **`filter(None, ...)` 是在过滤「失败的子问题」**——因为 `_query_subq` 在异常时返回 `None`。于是**一个子问题失败不会让整体失败**，它只是被略过。这是这一页最重要的容错设计。
2. **子问题答案被包装成一个新节点**，而节点的构造方式很朴素：

```
node_text = f"Sub question: {qa_pair.sub_q.sub_question}\nResponse: {qa_pair.answer}"
return NodeWithScore(node=TextNode(text=node_text))
```

**这个格式 `Sub question: ...\nResponse: ...` 是给合成器的提示**——它让最终的回答能引用「哪个子问题得到了什么答案」。注意它**没有分数**（`NodeWithScore` 只传了 node）。

3. **来源节点是「所有成功子问题的来源并集」**，通过 `additional_source_nodes` 传给合成器。而 `additional_source_nodes` 正是合成器那页里那个「在合成之后追加来源」的机制——**这里用它来表达「最终答案的依据包括所有子问题的检索结果」**。
4. **合成器看到的是「子问题答案节点」，不是原始检索节点。** 这是一个两阶段压缩：原始节点（可能几十条）→ 子问题答案（几条）→ 最终答案。

**`_query_subq` 与 `_aquery_subq` 是两份近乎相同的实现，而它们各有一个「全捕获」。**

```
except Exception:
    logger.warning(f"[{sub_q.tool_name}] Failed to run {question}", exc_info=True)
    return None
```

**裸 `except Exception`**（不是 `BaseException`，所以不吞 `KeyboardInterrupt`/`SystemExit`）——把所有失败降级成 `None`。

这个方向与 `RouterQueryEngine` 那个「包成 `Failed to select query engine` 再抛」正相反：路由选择失败要抛，而**子问题失败要静默略过**。差别的理由很实际——**子问题是可以少一个的**（六个子问题漏一个，剩下五个还能答），而路由失败意味着没有引擎可跑。

**`_construct_node` 里那个 f-string 有一个问题：当 `answer` 为 `None` 时会打印 `Response: None`。** 但实际上 `qa_pairs` 已经被 `filter(None, ...)` 过滤过，而失败的 `pair` 整个是 `None`（不是 `answer=None` 的 pair）。所以这条路不会走到——**除非某个 `_query_subq` 成功返回了一个 `answer=None` 的 pair**，而代码里所有成功路径都传了 `response_text`。

**`verbose` 的默认值是 `True`，这与其它类的默认（`False`）相反。**

```
def __init__(self, ..., verbose: bool = True, use_async: bool = False):
```

而 `use_async` 默认 `False`——**也就是说默认是「啰嗦且串行」**。文档里也写了「Defaults to True」与「Defaults to False」。这是一个刻意的默认：子问题分解是一个「调试友好」的功能，把中间过程打出来有助于理解它在做什么。

**`get_color_mapping(str(i) for i in range(len(sub_questions)))`** 给每个子问题分配一个颜色，于是并行/串行输出时能分辨哪一行属于哪个子问题。这是一个纯为可读性服务的设计，而它说明这个引擎被预期在「人在看终端」的场景下使用。

**`_query_engines` 是一个字典而不是列表。**

```
self._query_engines = {tool.metadata.name: tool.query_engine for tool in query_engine_tools}
```

因为子问题生成器指定的是**工具名**而非索引——这比按索引更稳健：LLM 更容易说对一个名字（它在提示里见过）而不是一个序号。

代价是**名字重复会静默覆盖**，且一个生成器给出不存在的名字会在 `self._query_engines[sub_q.tool_name]` 上抛 `KeyError`——而那个 `KeyError` 会被上面那个全捕获吞掉，变成「这个子问题失败」。

## 关键取舍

**源码事实：** `RouterQueryEngine` 把 `QueryEngineTool` 拆成「引擎列表」与「元数据列表」两个平行数组，于是选择器只看元数据、不看引擎。

**源码事实：** 路由的 `try/except ValueError` 只包住「按索引取引擎」，不包引擎自身的执行；原始异常用 `from e` 保留但消息被换成 `Failed to select query engine`。

**源码事实：** `combine_responses` 会**强制把流式响应取成完整响应**才能汇总，于是经汇总的路由结果不再流式；`AsyncStreamingResponse` 在同步路径直接报错。

**源码事实：** 汇总结果的分派最后一个分支是 `else` 兜底（任何非 `str`/`BaseModel`/`Generator` 都被当成异步生成器）。

**源码事实：** `_aquery` 在多引擎分支用 `asyncio.gather` 并行，而 `_query` 是串行。

**源码事实：** 每次路由都把 `selector_result` 写进响应元数据；工具检索版写 `retrieved_tools`。

**源码事实：** `RetrieverRouterQueryEngine` 已废弃，且**要求检索器恰好返回一个节点**（多于一个报错）；它的 `_aquery` 只是把同步 `_query` 包进 `async def`，会阻塞事件循环。

**源码事实：** `ToolRetrieverRouterQueryEngine` 没有对「检索到零个工具」做处理，`responses[0]` 会抛 `IndexError`；它的多引擎路径是串行 await 而非 `gather`。

**源码事实：** `SubQuestionQueryEngine` 用 `filter(None, ...)` 过滤失败的子问题，于是**一个子问题失败不会让整体失败**；这与路由器「选择失败就抛」的取向相反。

**源码事实：** 子问题结果被包装成 `TextNode`，文本格式是 `Sub question: ...\nResponse: ...`，没有分数；所有来源通过 `additional_source_nodes` 传给合成器。

**源码事实：** `_query_subq` / `_aquery_subq` 用裸 `except Exception` 把所有失败降级为 `None` 并记录 warning。

**源码事实：** `verbose` 默认 `True`、`use_async` 默认 `False`——默认是「啰嗦且串行」，因为子问题分解被预期在人在看终端的场景下使用。

**源码事实：** `_query_engines` 是按工具名索引的字典（子问题生成器指定名字而非序号），名字重复会静默覆盖，未知名字的 `KeyError` 会被外层全捕获吞掉。

**架构推论：** 两种策略的差别在于「不确定性发生在哪一步」。路由的不确定性在**选择**（一个对多个的判定），子问题分解的不确定性在**拆解**（一个问题变成几个）。前者错了就全错（选错了引擎等于答错），所以选择失败要抛错；后者错了一个只是少一部分，所以可以静默略过。

这个差别直接决定了它们的容错策略，也解释了为什么子问题引擎的默认 `verbose=True`——**它把「拆成了什么」打印出来，正是为了让使用者能在拆错时看见**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/query_engine/router_query_engine.py` | `RouterQueryEngine` | 选择器驱动，支持单/多选与汇总 |
| `llama-index-core/llama_index/core/query_engine/router_query_engine.py` | `combine_responses` / `acombine_responses` | 收集来源 + `TreeSummarize` 汇总，强制解流式 |
| `llama-index-core/llama_index/core/query_engine/router_query_engine.py` | `RetrieverRouterQueryEngine` | 已废弃；要求恰好一个节点；假异步 |
| `llama-index-core/llama_index/core/query_engine/router_query_engine.py` | `ToolRetrieverRouterQueryEngine` | 检索工具后逐个执行，多个则汇总 |
| `llama-index-core/llama_index/core/query_engine/router_query_engine.py` | `default_node_to_metadata_fn` | 节点 → 工具，靠约定的 `tool_name` 元数据键 |
| `llama-index-core/llama_index/core/query_engine/router_query_engine.py` | `selector_result` / `retrieved_tools` | 把决策写进响应元数据 |
| `llama-index-core/llama_index/core/query_engine/sub_question_query_engine.py` | `SubQuestionQueryEngine` | 拆问题 → 逐子问题执行 → 合成 |
| `llama-index-core/llama_index/core/query_engine/sub_question_query_engine.py` | `SubQuestionAnswerPair` | 子问题 + 可选答案 + 来源，也用作事件载荷的两阶段形态 |
| `llama-index-core/llama_index/core/query_engine/sub_question_query_engine.py` | `_construct_node` | 把「子问题+答案」变成 `TextNode` 供合成器使用 |
| `llama-index-core/llama_index/core/query_engine/sub_question_query_engine.py` | `_query_subq` / `_aquery_subq` | 裸 `except Exception` 降级为 `None` |

## 参考

- [llama-index 源码：query_engine/router_query_engine.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/query_engine/router_query_engine.py)
- [llama-index 源码：query_engine/sub_question_query_engine.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/query_engine/sub_question_query_engine.py)
- [llama-index 文档：查询引擎模块](https://docs.llamaindex.ai/en/stable/module_guides/deploying/query_engine/)
- [论文：Least-to-Most Prompting（子问题分解）](https://arxiv.org/abs/2205.10625)

## 与主核 / AI 的关系

这两个策略对应主核里的两种「查询改写/分发」：**视图路由**（同一个查询打到哪个分区/视图）与**查询展开**（一个查询被拆成多个可并行的子查询）。数据库的两种做法都是**确定性**的——依据是元数据与统计信息。

而这里的路由依赖 LLM 选、子问题依赖 LLM 拆，于是引入了**必须是幂等的重试**问题：同一个查询两次路由可能选出不同引擎。

对 agent 工程的直接价值有三条。其一，**不确定性的位置决定了容错策略**。选择错 = 全错 → 抛错；拆解错一部分 = 少一部分 → 静默略过。把这两者用同一套策略处理（都抛或都略）都会有明显问题。其二，**决策结果必须进元数据**。路由的 `selector_result` 与工具检索的 `retrieved_tools` 让「为什么这么答」可追溯——对一个非确定性的分支点，这是唯一的事后诊断手段。其三，**「检索到空」是最常见情形之一，必须处理**。`ToolRetrieverRouterQueryEngine` 在零工具时 `IndexError`，而这个情形在真实使用里高频出现（问题与所有候选都不相关时）。
