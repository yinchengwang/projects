---
project: llama-index
feature: citation-and-retry
title: 引用追踪与重试
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/query_engine/citation_query_engine.py
    symbols: [CitationQueryEngine]
  - path: llama-index-core/llama_index/core/query_engine/retry_query_engine.py
    symbols: [RetryQueryEngine]
  - path: llama-index-core/llama_index/core/query_engine/transform_query_engine.py
    symbols: [TransformQueryEngine]
---

# 引用追踪与重试

## 一句话职责

三种「在标准查询引擎外面再套一层」的做法：`CitationQueryEngine` 把检索节点**再切碎成编号来源**，让回答能带 `[1]` 这样的角标；`RetryQueryEngine` 用评测器判定回答不合格就**带着反馈重新提问**；`TransformQueryEngine` 在每次进入引擎之前**先改写查询**。

## 机制是什么

| 类 | 它包住什么 | 干预点 | 副作用 |
|---|---|---|---|
| `CitationQueryEngine` | 检索器 + 合成器 | **检索之后**：把节点切成更细的来源 | 一次检索产出 N 倍节点 |
| `RetryQueryEngine` | 一个引擎 + 一个评测器 | **查询之后**：评测不过就重试 | 递归自调用，最多 `max_retries` 层 |
| `TransformQueryEngine` | 一个引擎 + 一个变换器 | **查询之前**：改写 `QueryBundle` | 每次入口都调用变换器 |

三者的共同点是它们**都不改变被包裹引擎的内部逻辑**——只在它的边界上动手。

## 怎么实现的

### 一、`CitationQueryEngine`：把来源编号塞进节点文本

**核心洞察：模型只能引用「提示里见过的东西」。** 所以要让回答带 `[2]` 这样的角标，提示里就必须有「Source 2: ...」这样的标注——而**标注是加在节点文本里的**，不是加在外部结构上。

**`_create_citation_nodes` 就是做这件事的地方，而它做四步。**

```
for node in nodes:
    text_chunks = self.text_splitter.split_text(node.node.get_content(metadata_mode=self._metadata_mode))
    ...
    for text_chunk in text_chunks:
        text = f"Source {len(new_nodes) + 1}:\n{text_chunk}\n"
        new_node = NodeWithScore(node=TextNode.model_validate(source_data), score=node.score)
        new_node.node.set_content(text)
        new_nodes.append(new_node)
```

1. **先把每个检索节点再切一遍**（用 `text_splitter`，默认 `SentenceSplitter`，块大小 `DEFAULT_CITATION_CHUNK_SIZE = 512`、重叠 20）。
2. **对每个切出来的片段编号**，编号用 `len(new_nodes) + 1`——**这是全局递增而非每个原节点重新从 1 开始**。
3. **构造新节点时复用原节点的元数据**（`model_dump()` 再 `model_validate`）。
4. **把编号前缀写进新节点的内容**。

**为什么要再切一遍？** 因为「引用粒度」与「检索粒度」不该相同。检索希望块大一点（命中率高），而引用希望段落小一点（`[3]` 应当指向一两句话而不是一整页）。这个引擎把「检索时大块、引用时小块」这个分离做在了检索之后。

**而那个「复用元数据」的代码有三行专门的清理，且注释解释了原因。**

```
# Each citation node holds a single chunk, so the source node's id and
# character offsets no longer describe it: copying the id would give
# every chunk of a source the same one, and the offsets would span the
# whole source. _create_multimodal_citation_nodes already omits both.
source_data = node.node.model_dump()
source_data.pop("id_", None)
source_data["start_char_idx"] = None
source_data["end_char_idx"] = None
```

三处清理各自对应一个「复制了会错」的字段：

- **`id_`**：若保留，同一个原节点切出的所有片段会拥有**同一个 node_id**——于是下游按 id 去重会把它们全并成一条，或者存储里 id 冲突。
- **`start_char_idx` / `end_char_idx`**：这两个偏移描述的是**原节点在原文中的位置**。对一个片段而言，它的位置是原节点范围的一个子区间，而代码没有重算——所以置空比给一个错的数值好。

**注释最后一句「`_create_multimodal_citation_nodes` already omits both」** 说明这个清理是**事后补上的**（多模态版从一开始就没复制这两个字段），而文本版原来复制了。

**多模态版本走的是另一条路：用 `ChatMessage.split(...)`。**

```
blocks = node.node.get_content_blocks(metadata_mode=self._metadata_mode)
chat_msg = ChatMessage(blocks=blocks)
split_msgs = chat_msg.split(max_tokens=self._citation_chunk_size, overlap=self._citation_chunk_overlap)
```

于是多模态的切分是**按内容块预算**（`max_tokens`）而不是字符——因为要同时处理文本、图片、音频、视频块。

**提示模板是这一页最具体的部分，而它的例子说明了想要的输出形态。**

```
"Answer: Water will be wet when the sky is red [2], which occurs in the evening [1]."
```

两个要点：

1. **例子把「一个论断可以引用多个来源」演示出来了**（`[2]` 与 `[1]` 各管一半）。
2. **它明确要求「没有来源有帮助时应当说明」**——这一条是防止模型在无依据时编一个 `[1]`。

提示里还有一条约束「Only cite a source when you are explicitly referencing it」——**这条在实测中经常被违反**（模型倾向于给每个句子都挂角标）。这解释了为什么需要评测（下一节 `g02` 的 `FaithfulnessEvaluator`）。

**四个模板（QA / refine / 两个 chat content 版）的内容几乎逐字相同**，只有 `{context_str}` 与 `{context_msg}` 的区别，以及 chat 版把块渲染成 `{% image %}` / `{% audio %}` / `{% video %}` 之类的标记。**这意味着「引用行为」被写进了四份提示里**——任何一处措辞改动都要同步四处。

**`_query` 走的是「检索 → 造引用节点 → 合成」三步，而检索与合成被覆盖成公开方法。**

```
def retrieve(self, query_bundle): ...
async def aretrieve(self, query_bundle): ...
def synthesize(self, query_bundle, nodes, additional_source_nodes=None): ...
async def asynthesize(...): ...
```

也就是说这个引擎**把 `BaseQueryEngine` 那两个「可选分解」实现了**——于是它比只实现 `_query` 的引擎多了一份能力：调用方可以把它的检索与合成单独控制。

### 二、`RetryQueryEngine`：评测驱动的重试，靠递归实现

**它的注入点是 `BaseEvaluator`，而重试的判据是 `eval.passing`。**

```
response = self._query_engine._query(query_bundle)
assert not isinstance(response, AsyncStreamingResponse)
if self.max_retries <= 0: return response
typed_response = response if isinstance(response, Response) else response.get_response()
eval = self._evaluator.evaluate_response(query_str, typed_response)
if eval.passing:
    return response
else:
    new_query_engine = RetryQueryEngine(self._query_engine, self._evaluator, self.max_retries - 1)
    query_transformer = FeedbackQueryTransformation()
    new_query = query_transformer.run(query_bundle, {"evaluation": eval})
    return new_query_engine.query(new_query)
```

五处值得拆：

1. **它调的是 `self._query_engine._query(...)` 而不是 `.query(...)`**——**绕过被包裹引擎的门面层**（也就是绕过它的 trace 事件与 `str`→bundle 转换）。这是一个刻意的选择（避免重复的 trace 嵌套），但它也意味着被包裹引擎的 `QueryStartEvent`/`QueryEndEvent` **不会发出**。

2. **`assert not isinstance(response, AsyncStreamingResponse)`** —— 同步重试无法消费异步流式响应，所以直接断言。这个断言在 `python -O` 下会被移除，于是生产环境下会走到后面的 `get_response()` 并可能崩在一个更隐晦的地方。

3. **`max_retries <= 0` 时**直接在**评测之前**返回。也就是说 `max_retries=0` 表示「完全不评测」而不只是「不重试」。这两件事在这里被合并了，而语义上它们是不同的。

4. **重试靠新建一个 `RetryQueryEngine` 实例**（`max_retries - 1`）而不是循环。于是「还剩几次」这个状态是**通过实例的递归嵌套表达的**——第 N 层是 `RetryQueryEngine(RetryQueryEngine(...))`。这种写法的好处是每层的 `query` 都走完整门面（带 trace），坏处是**创建了 N 个对象，且调用栈随重试次数增长**。

   注意它新建实例时**没有传 `callback_manager`**，于是每一层都用默认的 `Settings.callback_manager`——第一层显式传的 manager 在重试层丢失了。这是一个 trace 断链。

5. **`FeedbackQueryTransformation` 把评测结果带进新查询**——`{"evaluation": eval}` 作为 metadata 传给变换器。于是「上次为什么不合格」这个信息进入了下一次查询的构造过程。

**`_aquery` 的实现只有一句：`return self._query(query_bundle)`，且文档写着 `"Not supported."`。**

也就是说这个引擎在异步路径里是**同步执行**——会阻塞事件循环。而它自己的注释承认了这一点。

**`RetryGuidelineQueryEngine` 是同一个模式的兄弟类，差别是评测器类型（`GuidelineEvaluator`）与多一个 `resynthesize_query` 开关。**

后者的意义是：重试时不只带上评测反馈，还可以**重新合成查询**（而不是仅做变换）。这个开关传给 `FeedbackQueryTransformation` 的构造——也就是说「重试时的改写强度」是一个可配置项。

### 三、`TransformQueryEngine`：包在任何入口之前改写

**它的实现极为规整：四个入口方法各自「先变换、再委托」。**

```
def retrieve(self, query_bundle):
    query_bundle = self._query_transform.run(query_bundle, metadata=self._transform_metadata)
    return self._query_engine.retrieve(query_bundle)

def synthesize(self, query_bundle, nodes, additional_source_nodes=None):
    query_bundle = self._query_transform.run(query_bundle, metadata=self._transform_metadata)
    return self._query_engine.synthesize(...)

def _query(self, query_bundle):
    query_bundle = self._query_transform.run(query_bundle, metadata=self._transform_metadata)
    return self._query_engine.query(query_bundle)

async def _aquery(self, query_bundle):
    query_bundle = self._query_transform.run(query_bundle, metadata=self._transform_metadata)
    return await self._query_engine.aquery(query_bundle)
```

**同一句变换在四个地方重复。** 这是一个必然的重复：因为 `BaseQueryEngine` 的四个入口（`_query` / `_aquery` / `retrieve` / `synthesize`）都可能被直接调用，而变换必须在**每一个**入口之前发生，否则「先 retrieve 再 synthesize」的调用方会让变换执行两次或一次都不执行。

**注意这里的后果：`retrieve` 与随后的 `synthesize` 各自都会变换一次。** 若调用方按「`retrieve()` 拿节点 → `synthesize()` 用那些节点」的模式使用，变换会跑两遍——而若变换器是**非幂等**的（比如「把查询改写成三个子查询」），第二遍就是对已变换的查询再变换。

这是一个「装饰器式组合与分解式调用」的固有冲突：**把变换放在最外层时，无法知道调用方会怎么拆**。

**`_query` 委托的是 `self._query_engine.query(...)`（门面）而不是 `_query`。** 这与 `RetryQueryEngine` 的选择正好相反——`Transform` 保留被包裹引擎的 trace，`Retry` 绕过它。

两处都对：`Transform` 只是「改输入」，保留内层的完整追踪是合理的；而 `Retry` 是「同一层逻辑的重复执行」，若保留内层门面会让 trace 里出现 N 层相同的 query 事件。

**`transform_metadata` 是一个「不参与变换计算但要传给变换器」的旁路参数。** 它的用途（从 `RetryQueryEngine` 的用法可以反推）是承载「上下文信息」——比如上次的评测结果。于是同一个 `FeedbackQueryTransformation` 既能被 `TransformQueryEngine` 用（无反馈）、也能被 `RetryQueryEngine` 用（带反馈）。

## 关键取舍

**源码事实：** `CitationQueryEngine` 在检索之后**再次切分**节点（默认 512 字符、重叠 20），因为「检索粒度」与「引用粒度」不该相同；编号是全局递增的。

**源码事实：** 复用原节点元数据时**清掉 `id_` 与 `start_char_idx`/`end_char_idx`**——否则同一原节点的所有片段共享一个 id，且偏移会跨越整个原节点。注释指出多模态版从一开始就没复制这两个字段。

**源码事实：** 四个提示模板（QA / refine / 两个 chat content 版）几乎逐字相同，「引用行为」写在四份里，任何措辞改动要同步四处。

**源码事实：** 提示里既有「只在你确实引用某来源时才标」这样的约束，也要求「没有有用来源时要说明」——而前者在实测中经常被违反，这正是需要评测的原因。

**源码事实：** `RetryQueryEngine` 调被包裹引擎的 `_query` 而非 `query`，因此**绕过内层门面的事件**；`TransformQueryEngine` 用的是 `query`（保留内层 trace）——两者相反且都有理由。

**源码事实：** `RetryQueryEngine` 的重试靠**递归新建实例**（`max_retries - 1`）实现，且新建时不传 `callback_manager`，于是第一层的 manager 在重试层丢失。

**源码事实：** `max_retries <= 0` 时在**评测之前**返回——「不重试」与「不评测」被合并成同一件事。

**源码事实：** 两处 `assert not isinstance(response, AsyncStreamingResponse)` 在 `python -O` 下会被移除，生产环境会走到更隐晦的失败点。

**源码事实：** `RetryQueryEngine._aquery` 直接调 `_query` 并注明 `"Not supported."`——异步路径同步执行、阻塞事件循环。

**源码事实：** `TransformQueryEngine` 把同一句变换重复在四个入口（`_query`/`_aquery`/`retrieve`/`synthesize`），于是「先 retrieve 再 synthesize」的调用会让变换跑两遍。

**架构推论：** 这三个引擎展示了「装饰器式组合」的三种干预点：**检索后**（加来源编号）、**查询后**（评测重试）、**查询前**（改写）。

而它们的共同风险是**装饰器与被装饰者之间的隐式约定**：`Retry` 假设被包裹引擎能在 `max_retries` 次内收敛（否则白花钱）、`Citation` 假设被包裹的合成器会照着提示的编号引用（否则角标对不上真来源）、`Transform` 假设变换器幂等（否则分解式调用会重复变换）。**这些假设都没有在代码里被校验，也没有在文档里被声明。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/query_engine/citation_query_engine.py` | `CitationQueryEngine` | 检索后把节点切碎成编号来源 |
| `llama-index-core/llama_index/core/query_engine/citation_query_engine.py` | `_create_citation_nodes` | 二次切分 + 全局编号 + 清理 id/偏移 |
| `llama-index-core/llama_index/core/query_engine/citation_query_engine.py` | `_create_multimodal_citation_nodes` | 用 `ChatMessage.split` 按 token 预算切块 |
| `llama-index-core/llama_index/core/query_engine/citation_query_engine.py` | `CITATION_QA_TEMPLATE` / `CITATION_REFINE_TEMPLATE` | 带编号引用的文本提示（两份几乎相同） |
| `llama-index-core/llama_index/core/query_engine/citation_query_engine.py` | `CITATION_CHAT_CONTENT_QA_TEMPLATE` / `CITATION_CHAT_CONTENT_REFINE_TEMPLATE` | 多模态版提示，按 block 类型渲染 |
| `llama-index-core/llama_index/core/query_engine/citation_query_engine.py` | `DEFAULT_CITATION_CHUNK_SIZE` / `DEFAULT_CITATION_CHUNK_OVERLAP` | 引用粒度的默认 512 / 20 |
| `llama-index-core/llama_index/core/query_engine/retry_query_engine.py` | `RetryQueryEngine` | 评测驱动重试，递归新建实例 |
| `llama-index-core/llama_index/core/query_engine/retry_query_engine.py` | `RetryGuidelineQueryEngine` | 同一模式 + 可重新合成查询 |
| `llama-index-core/llama_index/core/query_engine/retry_query_engine.py` | `_query` 的重试分支 | 用 `FeedbackQueryTransformation` 把评测带进新查询 |
| `llama-index-core/llama_index/core/query_engine/transform_query_engine.py` | `TransformQueryEngine` | 四个入口各变换一次，再委托 |
| `llama-index-core/llama_index/core/query_engine/transform_query_engine.py` | `transform_metadata` | 旁路参数，承载「上次评测结果」这类上下文 |

## 参考

- [llama-index 源码：query_engine/citation_query_engine.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/query_engine/citation_query_engine.py)
- [llama-index 源码：query_engine/retry_query_engine.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/query_engine/retry_query_engine.py)
- [llama-index 文档：引用查询引擎](https://docs.llamaindex.ai/en/stable/examples/query_engine/citation_query_engine/)
- [论文：Self-RAG（自反思检索与生成）](https://arxiv.org/abs/2310.11511)

## 与主核 / AI 的关系

「装饰器套在主流程外面干预一个点」与主核里的**触发器 / 重写规则**是同类结构：数据库也有「在语句执行前后插入一段逻辑」的机制（`BEFORE/AFTER` 触发器、规则重写）。而主核的这些机制都是**确定性**的——触发器不会自己判定「这次要不要重跑」。

`RetryQueryEngine` 引入了一个主核里不存在的东西：**用另一个模型来判断「该不该重做」，并按判定递归重做**。这带来一个成本问题——重试的期望代价是 `Σ p(fail)^k`，而 `p(fail)` 由评测器决定；**若评测器过严，成本会指数上升**，而 `max_retries` 是唯一的刹车。

对 agent 工程的直接价值有三条。其一，**「检索粒度」与「引用粒度」应当分开**。同一批文档，检索希望块大（命中率高）、引用希望块小（角标准确）——把切分放在检索之后、编号之前，就同时满足两者。其二，**复制元数据时要清掉「已失效的派生字段」**。`id_` 与字符偏移在原节点被切碎之后就不再描述新节点了；留着它们比留空更糟，因为它们看起来有效。其三，**非幂等的变换器与「分解式调用」不兼容**。把变换放在每个入口之前，就无法保证它只执行一次——若不幂等（比如「改写成三个子查询」），分解式调用会让查询被变换两次。
