---
project: llama-index
feature: node-postprocessor
title: 节点后处理与相似度裁剪
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/postprocessor/node.py
    symbols: [KeywordNodePostprocessor, SimilarityPostprocessor, LongContextReorder]
  - path: llama-index-core/llama_index/core/postprocessor/node.py
    symbols: [PrevNextNodePostprocessor, AutoPrevNextNodePostprocessor]
---

# 节点后处理与相似度裁剪

## 一句话职责

检索给出「一堆带分数的节点」之后、合成器把它们拼进提示之前，这一层做三件事——**按分数/关键词裁掉不合格的**、**按节点间关系补齐相邻的**、**按位置重排以对抗「中间遗忘」**。

## 机制是什么

五个后处理器，可以按「它们对节点列表做了什么」分成三类：

| 类型 | 类 | 动作 |
|---|---|---|
| 过滤 | `KeywordNodePostprocessor` | 必需关键词必须命中、排除关键词不能命中 |
| 过滤 | `SimilarityPostprocessor` | 按分数阈值裁掉 |
| 扩展 | `PrevNextNodePostprocessor` | 沿文档关系向前/向后取 N 个相邻节点 |
| 扩展 | `AutoPrevNextNodePostprocessor` | **让 LLM 判断该向前还是向后**，再取 |
| 重排 | `LongContextReorder` | 把最相关的放到两端、最不相关的放中间 |

共同契约是 `BaseNodePostprocessor._postprocess_nodes(nodes, query_bundle=None) -> List[NodeWithScore]`——**输入输出都是「节点 + 分数」的列表**，所以多个后处理器可以串成一条链。

## 怎么实现的

### 一、两个过滤器：一个用短语匹配，一个只看分数

**`KeywordNodePostprocessor` 用的是 spaCy 的 `PhraseMatcher`，而不是子串查找。**

```
nlp = spacy.blank(self.lang)
required_matcher = PhraseMatcher(nlp.vocab)
required_matcher.add("RequiredKeywords", list(nlp.pipe(self.required_keywords)))
```

用 `spacy.blank(lang)` 意味着**只加载语言的分词规则，不加载任何模型**——因为短语匹配只需要分词，不需要词性/实体。`nlp.pipe(...)` 把关键词逐条转成 `Doc` 后加入匹配器。

判定顺序是「先必需、后排除」：

```
if self.required_keywords and not required_matcher(doc): continue
if self.exclude_keywords and exclude_matcher(doc): continue
```

两个 `if` 都带「配置非空才查」的前置条件，于是**默认（两个列表都空）时这个处理器是恒等变换**。这个默认是有意义的：一个没配关键词的过滤器不该把一切都过滤掉。

而先判必需再判排除的顺序意味着：一个既缺必需词又含排除词的节点，**它的第一个失败原因会被记成「缺必需词」**。这不影响结果（两种都丢弃），但影响调试时的理解。

**`SimilarityPostprocessor` 的实现里有两条值得记的判断。**

```
sim_cutoff_exists = self.similarity_cutoff is not None
...
if sim_cutoff_exists:
    similarity = node.score
    if similarity is None: should_use_node = False
    elif cast(float, similarity) < cast(float, self.similarity_cutoff): should_use_node = False
```

第一，**`score is None` 的节点会被丢弃**。也就是说一个「分数未知」的节点在这个处理器下等同于不合格。这是一个保守取向：既然阈值是用来裁的，无法判定是否达标的就该裁掉。

第二，**默认 `similarity_cutoff = 0.0` 而不是 `None`**，但代码里却检查了 `is not None`——说明这个字段**类型上允许 `None`** 以表达「不裁剪」，而默认值是 `0.0`。两个值的行为差别很实际：`0.0` 会丢掉所有负分数节点（例如余弦相似度不可能为负的向量库里无害，但某些距离度量会产出负值）；`None` 才是彻底不裁。

### 二、`PrevNextNodePostprocessor`：沿关系链取邻居

**它是唯一直接依赖 `docstore` 的后处理器——因为「相邻节点」不在检索结果里，必须去存储取。**

`get_forward_nodes` 与 `get_backward_nodes` 是两条迭代循环，都是**沿 `NodeRelationship.NEXT` / `prev_node` 逐个走**：

```
while cur_count < num_nodes:
    if NodeRelationship.NEXT not in node.relationships: break
    next_node_info = node.next_node
    if next_node_info is None: break
    next_node = docstore.get_node(next_node_info.node_id)
    nodes[next_node.node_id] = NodeWithScore(node=next_node)
    node = next_node
    cur_count += 1
```

四处细节：

1. **返回的是 `Dict[str, NodeWithScore]` 而不是列表**——因为多个起点可能走到同一个邻居，用字典天然去重。
2. **扩出来的节点分数是 `None`**（`NodeWithScore(node=next_node)` 没传分数）。这是一个明确的信息损失：新节点没有「有多相关」的信息，若后面还接一个 `SimilarityPostprocessor`，它们会全部被裁掉。
3. **向前走时不用检查 `get_node` 返回 `None`**，向后走时检查了（`if prev_node is None: break`）。这是一个不对称：向前那条若 `docstore` 返回 `None` 就会在 `next_node.node_id` 上崩。**两条代码路径对这个边界的处理不一致**。
4. **`cur_count` 是「已取到的邻居数」，不含起点。** 所以 `num_nodes=1` 只取一个邻居。

**`_postprocess_nodes` 里的排序是这一页最绕的一段。** 收集完所有节点后，它做的是「插入式排序」：

```
for node in all_nodes_values:
    node_inserted = False
    for i, cand in enumerate(sorted_nodes):
        node_id = node.node.node_id
        prev_node_info = cand.node.prev_node
        next_node_info = cand.node.next_node
        if prev_node_info is not None and node_id == prev_node_info.node_id:
            node_inserted = True; sorted_nodes.insert(i, node); break
        elif next_node_info is not None and node_id == next_node_info.node_id:
            node_inserted = True; sorted_nodes.insert(i + 1, node); break
    if not node_inserted: sorted_nodes.append(node)
```

也就是说：对每个节点，扫描已排序列表，若它是某个已排节点的 `prev_node` 就**插在它前面**，若是 `next_node` 就**插在它后面**；都不匹配就追加到末尾。

**这个排序只保证「直接相邻关系」被修复，不保证全局有序。** 若一条链上有 A-B-C，而迭代顺序恰好是 C、A、B，那么 C 先入列；A 插入时找不到 C 的 prev/next 关系（C 的 `prev_node` 是 B 而非 A），于是 A 被追加；B 插入时发现自己在 A 后面也在 C 前面……结果不一定是 A-B-C。

**正确的做法本应是按 `node_id` 或某个序号排**——而 `AutoPrevNextNodePostprocessor` 恰好就这么做了（`sorted(all_nodes.values(), key=lambda x: x.node.node_id)`）。这个对比说明：**手工关系排序只能处理两节点相邻，多节点链需要全序键**。

### 三、`AutoPrevNextNodePostprocessor`：让 LLM 决定方向

**它的核心不是「取邻居」，而是「判断该往哪边取」，而那个判断被交给 LLM。**

提示模板 `DEFAULT_INFER_PREV_NEXT_TMPL` 给出的判定只有三个答案：`PREVIOUS` / `NEXT` / `NONE`，且它用三个例子把语义钉住：

> Context: 描述作者在 Y Combinator 的经历。Question: 作者在 YC 之后做了什么？→ **NEXT**
> Question: 作者在 YC 之前做了什么？→ **PREVIOUS**
> Question: 作者在 YC 期间做了什么？→ **NONE**

三个例子恰好覆盖了「向后」「向前」「不需要」三种情形，而判据是**问句里的时间副词**（「之后」「之前」「期间」）。也就是说这个提示实际上是在做时间方向分类。

**它没有直接调 LLM，而是通过 `get_response_synthesizer` 包了一层。**

```
response_builder = get_response_synthesizer(llm=llm, text_qa_template=infer_prev_next_prompt, refine_template=refine_infer_prev_next_prompt, response_mode=self.response_mode)
raw_pred = response_builder.get_response(text_chunks=[node.node.get_content()], query_str=query_bundle.query_str)
```

代码注释说明了理由：**用响应合成器而不是直接调 LLM，是为了对长上下文的处理更稳健**。

这段话很关键：一个节点的内容可能就有几百 token，而 `response_mode` 可以从 `compact` 到 `refine` 到 `tree_summarize`——**合成器负责把「内容 + 提示」压进模型能处理的范围**。若直接 `llm.predict(prompt)`，一个长节点会直接把提示撑爆。

也就是说这里复用合成器的方式不是「为了合成回答」，而是**为了它的上下文管理能力**。这是一个把既有组件当基础设施用的例子。

**而 `_parse_prediction` 的容错是「包含即命中」而不是严格相等。**

```
pred = raw_pred.strip().lower()
if "previous" in pred: return "previous"
elif "next" in pred: return "next"
elif "none" in pred: return "none"
raise ValueError(f"Invalid prediction: {raw_pred}")
```

于是「The answer is NEXT.」也能被解析。但**顺序有陷阱**：`"previous"` 先判，而一个含「next」的字符串若恰好也含「previous」（比如解释性回答里同时提到两者）会被判成 previous。

判不出来时抛 `ValueError` 而**不是默认 `none`**——这会让整个查询失败。这是一个「宁可炸也不猜」的选择，而它的代价是：LLM 偶尔输出非预期格式就会让整个查询崩掉。对比 `PrevNextNodePostprocessor` 那种「配置了就一定按配置走」的确定性，这里的脆弱性来自把判定交给模型。

**最后它用 `node_id` 排序。** `sorted(all_nodes.values(), key=lambda x: x.node.node_id)` ——`node_id` 是 UUID 字符串，于是「排序」实际上是**按 UUID 字典序**，与文档顺序毫无关系。这是一个明显的缺陷：加了邻居之后节点顺序会变成随机。

它至少保证了确定性（同一批节点每次都得到同一顺序），但不是有意义的顺序。相比之下 `PrevNextNodePostprocessor` 那个手工插入排序虽然脆弱，至少在试图还原文档顺序——**两个后处理器在这一点上的取舍恰好相反**。

### 四、`LongContextReorder`：把重要的放到两端

**这是最简单的一个（十行），而它的依据是一篇论文。**

类的文档直接引用了研究：

> 模型很难访问长上下文**中间**的重要细节。一项研究观察到，最佳性能通常出现在**关键数据位于输入上下文的开头或结尾**时。此外，随着上下文变长，性能显著下降。

代码只用了一个奇偶判断：

```
ordered_nodes = sorted(nodes, key=lambda x: x.score if x.score is not None else 0)
for i, node in enumerate(ordered_nodes):
    if i % 2 == 0: reordered_nodes.insert(0, node)      # 偶数位插到前面
    else: reordered_nodes.append(node)                  # 奇数位追加到后面
```

**结果是什么，值得算一遍。** 先按分数升序排列（`score is None` 当 0），然后偶数为（下标 0、2、4…）依次 `insert(0, ...)`——**每次插到最前，于是顺序被反转**；奇数位依次 `append`——**保持原序**。

以 5 个节点、分数升序 `[n0, n1, n2, n3, n4]`（n0 分最低）为例：

- i=0 n0 → `[n0]`
- i=1 n1 → `[n0, n1]`
- i=2 n2 → `[n2, n0, n1]`
- i=3 n3 → `[n2, n0, n1, n3]`
- i=4 n4 → `[n4, n2, n0, n1, n3]`

最终顺序是 `n4, n2, n0, n1, n3`——**分数最高的 n4 在最前，第二高的 n3 在最后，最低的 n0 在正中**。这正是想要的效果：**两端是高分，中间是低分**。

**它对 `None` 分数的处理把这类节点当成最低分（`else 0`）。** 若分数是负值（某些距离度量），`None` 会被排到负值之前——仍不算错，但顺序不严格。

注意这个处理器**只重排、不丢任何节点**，所以它是链里唯一可以放在最后的位置（在它之后再加过滤器，过滤的依据会与展示顺序无关）。

## 关键取舍

**源码事实：** 五类处理器共用 `_postprocess_nodes(nodes, query_bundle)` 契约，输入输出都是 `NodeWithScore` 列表，因此可任意串联。

**源码事实：** `KeywordNodePostprocessor` 用 spaCy 的 `PhraseMatcher` 而非子串查找，且用 `spacy.blank(lang)`（不加载模型）；两个关键词列表都空时是恒等变换。

**源码事实：** `SimilarityPostprocessor` 丢弃 `score is None` 的节点；默认阈值 `0.0` 但字段类型允许 `None` 以表达「完全不裁」。

**源码事实：** `get_forward_nodes` / `get_backward_nodes` 返回字典以天然去重；**扩出来的节点分数是 `None`**，所以若后面接分数过滤器会全被裁掉。

**源码事实：** 向前走不检查 `docstore.get_node` 的 `None`，向后走检查了——两条代码路径对同一边界处理不一致。

**源码事实：** `PrevNextNodePostprocessor` 的排序是「按 prev/next 关系插入」，只保证直接相邻，**不保证多节点链的全局有序**。

**源码事实：** `AutoPrevNextNodePostprocessor` 用三个例子把「之后/之前/期间」映射到 NEXT/PREVIOUS/NONE；它通过 `get_response_synthesizer` 而非直接调 LLM，注释写明是为了对长上下文更稳健。

**源码事实：** `_parse_prediction` 用「包含即命中」且判序为 previous → next → none，判不出时抛 `ValueError` 而非默认为 none。

**源码事实：** `AutoPrevNextNodePostprocessor` 最终按 `node_id`（UUID 字符串）排序，于是顺序与文档顺序无关。

**源码事实：** `LongContextReorder` 按分数升序，偶数位前插、奇数位后追，结果是**两端高分、中间低分**；它只重排不丢弃。

**架构推论：** 这五个处理器展示了「检索后、合成前」这个位置的三种不同价值：**过滤是减少噪声**（模型看不懂的内容不如不给）、**扩展是补足上下文**（一个被切开的论证需要邻居）、**重排是对抗位置偏差**（同样的内容放中间会被忽略）。

而它们的共同风险都在「把判定交给外部」这一侧：`Keyword` 依赖刷对关键词、`Similarity` 依赖阈值合适、`AutoPrevNext` 依赖 LLM 输出格式。**唯一不依赖外部判断的是 `LongContextReorder`**——它只需要分数，而分数是检索器给的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/postprocessor/node.py` | `KeywordNodePostprocessor` | 短语匹配式关键词过滤，空配置时恒等 |
| `llama-index-core/llama_index/core/postprocessor/node.py` | `SimilarityPostprocessor` | 分数阈值裁剪，`None` 分数算不合格 |
| `llama-index-core/llama_index/core/postprocessor/node.py` | `PrevNextNodePostprocessor` | 按关系取前/后邻居，插入式排序 |
| `llama-index-core/llama_index/core/postprocessor/node.py` | `get_forward_nodes` / `get_backward_nodes` | 沿关系链迭代取邻居，返回字典去重 |
| `llama-index-core/llama_index/core/postprocessor/node.py` | `AutoPrevNextNodePostprocessor` | LLM 判定方向，经合成器调用 |
| `llama-index-core/llama_index/core/postprocessor/node.py` | `DEFAULT_INFER_PREV_NEXT_TMPL` | 三个例子把时间副词映射到三个方向 |
| `llama-index-core/llama_index/core/postprocessor/node.py` | `_parse_prediction` | 包含即命中，判不出抛 `ValueError` |
| `llama-index-core/llama_index/core/postprocessor/node.py` | `LongContextReorder` | 两端高分、中间低分的重排 |

## 参考

- [llama-index 源码：postprocessor/node.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/postprocessor/node.py)
- [llama-index 文档：节点后处理器](https://docs.llamaindex.ai/en/stable/module_guides/querying/node_postprocessors/)
- [论文：Lost in the Middle（长上下文中间遗忘）](https://arxiv.org/abs/2307.03172)
- [spaCy 文档：PhraseMatcher](https://spacy.io/api/phrasematcher)

## 与主核 / AI 的关系

这一层与主核里的**查询结果后处理**（排序、过滤、投影）位置相同：都在「拿到一批候选」与「返回给调用方」之间插入一段加工。数据库的 `ORDER BY` / `WHERE` / `LIMIT` 也是同一位置的算子，而它们的判定全是确定性的——不依赖外部模型。

这里的三种加工里只有 `LongContextReorder` 是纯确定性的，而它对抗的偏差（中间遗忘）在数据库里没有对应物——因为数据库的消费者是程序，不是注意力机制。

对 agent 工程的直接价值有三条。其一，**扩展出来的节点没有分数，这会让后续过滤器误杀**。`get_forward_nodes` 返回的 `NodeWithScore` 不带分，若链里接了阈值过滤，那些补进来的上下文会全部被丢掉——这需要调用方知道顺序（先扩展后过滤）或显式补偿分数。其二，**把判定交给 LLM 的处理器必须容错**。`_parse_prediction` 抛 `ValueError` 会让整个查询失败，而模型输出格式偏差是常态；至少应当回退到确定性默认。其三，**重排要比过滤晚做**。`LongContextReorder` 只改顺序不改集合，它放在最后才有意义；放在过滤之前，后面的过滤会按「分数」动手而破坏刚建立的排布。
