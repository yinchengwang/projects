---
project: dify
feature: parent-child-index
title: 父子分块索引处理器
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/index_processor/processor/parent_child_index_processor.py
    symbols: [ParentChildIndexProcessor]
---

# 父子分块索引处理器

## 一句话职责

`ParentChildIndexProcessor` 用两套切分参数做两级文档：父块保留完整语义、子块小到能精准命中——**检索用子块、喂给模型用父块**，靠 `children` 字段把它们绑在一起。

## 机制是什么

它比段落结构多三件事：

1. `Rule` 里多一个 `subchunk_segmentation`（子块切分参数），与 `segmentation`（父块参数）并存。
2. 多一个 `parent_mode` 开关，决定「父块边界」怎么来。
3. 产物从 `Document` 变成「带 `children` 的 `Document`」，且真正进向量库的是子块。

`parent_mode` 有两个取值，语义差别很大：

| 值 | 父块是什么 | 切法 |
|---|---|---|
| `PARAGRAPH` | 段落级片段 | 先按 `segmentation` 切一遍，每个片段是一个父块 |
| `FULL_DOC` | **整篇文档** | 把所有输入文档正文用换行拼成一个，唯一一个父块 |

所以 `PARAGRAPH` 是「多父多子」，`FULL_DOC` 是「一父多子」。后者适合短文档——父块就是全文，检索时命中任一子块都能拿到完整上下文。

## 怎么实现的

**`transform` 的两个分支几乎不共享代码。** `PARAGRAPH` 分支复制了段落处理器的一大段逻辑（清洗、切分、造双 id、去引导符号、抽图片），只是额外调 `split_child_nodes` 并把结果挂到 `document_node.children`。`FULL_DOC` 分支则从零拼文档：

```python
page_content = "\n".join([document.page_content for document in documents])
document = Document(page_content=page_content, metadata=documents[0].metadata)
```

三处细节需要留意：

- 元数据**只取第一篇文档的**，其余文档的 metadata 被丢弃。
- 拼接用 `"\n"`，不是 `"\n\n"`。
- 不经过 `CleanProcessor.clean`，也不去掉开头的引导符号。这两件事在 `PARAGRAPH` 分支都做了。

**`FULL_DOC` 分支的 `PAY` 顺序是「先切子块、后取 doc_id」。** 它先调 `split_child_nodes`，再生成父块的 `doc_id`/`doc_hash`。而 `split_child_nodes` 内部会把 `document_node.metadata` **同一个字典对象**传给每个子文档：

```python
child_document = ChildDocument(
    page_content=child_document_node.page_content, metadata=document_node.metadata
)
child_document.metadata["doc_id"] = doc_id
child_document.metadata["doc_hash"] = hash
```

pydantic 模型默认不深拷贝传入的 dict，所以所有子文档**共享同一个 metadata 对象**；循环里对 `doc_id` 的赋值会互相覆盖，最终**所有子文档的 `doc_id` 都等于最后一个子块的**。父块的 `doc_id` 在循环之后才写入，也会覆盖成父块自己的值。

在 `PARAGRAPH` 分支里这个副作用同样存在，但后果不同：每个父块的 metadata 是切分器产出的独立对象，所以每个父块内部的子块共享 `doc_id`，父块之间不共享。

**`load` 只写子块，不写父块。** 高质量模式下遍历每个父块，把 `children` 逐个 `model_validate(model_dump())` 转成 `Document` 再 `vector.create(...)`——**父块自身不进向量库**。这是个关键的实现事实：父块只存在于 `Document`（数据库文档与段表）里，向量库里只有子块。检索命中子块后，要靠 `doc_id`/`segment_id` 回查父块拿完整上下文。

`load` 在经济模式（关键词）下没有任何动作——这个方法体里只有 `if HIGH_QUALITY:` 一段，没有 `else`。所以父子结构**不支持纯关键词索引**。

**`clean` 的删除路径比段落多一层。** 它要删的是子块向量与子块行，而 `node_ids` 给的是**父块的** `index_node_id`。所以要先把父块映射到子块 id：优先用调用方预计算的 `precomputed_child_node_ids`，否则现场 join `ChildChunk` 与 `DocumentSegment` 查。注释直说了回退路径的隐患——「may fail if segments are already deleted」。**预计算参数的存在就是为了避开这个竞态**。

数据库侧的删除还受 `delete_child_chunks` 开关控制：向量删除总是做（只要拿到 id），`ChildChunk` 行删除要显式传参。全量删除分支（`node_ids` 为 `None`）里，向量清空后若开了开关，按 `(tenant_id, dataset_id)` 删——注释说明这是在利用已有复合索引。

**`split_child_nodes` 自己再建一个切分器。** 它用 `rules.subchunk_segmentation` 的参数调同一个 `_get_splitter`，所以子块与父块的切分**策略一致、参数不同**。这也意味着子块的 `max_tokens` 同样受 [50, 上限] 的校验约束。

**`index` 走的是结构化输入，不是纯文本。** 它把 `chunks` 按 `ParentChildStructureChunk` 校验，逐 `parent_child` 造父块与其子块列表，各自生成 `doc_id`/`doc_hash`（子块的哈希来自子块内容，不是父块内容）。这里的 metadata 是现造的独立字典，**不存在上面那种共享副作用**——手动构造时每个子块都拿到自己的字典。

**预览模式有两处截断，位置不同。** `PARAGRAPH` 分支在累计满 10 个**父块**后直接 `return`（硬编码 10，不是配置项）；`FULL_DOC` 分支在切完子块后按 `CHILD_CHUNKS_PREVIEW_NUMBER` 截断**子块列表**。前者截父块，后者截子块——因为两种模式下「数量不受控的维度」不同。

**`ParentChildFormatPreviewDict` 比段落那版多一个 `parent_mode` 字段**，前端要靠它决定怎么渲染（展示多个独立片段 vs 展示一个父块带一串子块）。

## 关键取舍

**源码事实：** `parent_mode=PARAGRAPH` 时父块来自段落级切分，`FULL_DOC` 时父块由所有输入文档用 `"\n"` 拼接、只取第一篇的 metadata。

**源码事实：** `FULL_DOC` 分支不做 `CleanProcessor.clean`、不去开头引导符号；`PARAGRAPH` 分支两件都做。

**源码事实：** `split_child_nodes` 把父块的 metadata 字典**同一对象**传给每个子文档，循环内对 `doc_id` 的赋值互相覆盖。

**源码事实：** `load` 只把子块写进向量库，父块不进；方法体只有高质量分支，经济模式下无动作。

**源码事实：** `clean` 需要把父块 id 映射成子块 id，优先用 `precomputed_child_node_ids`，回退路径的注释承认「段已删则查不到」。

**源码事实：** 预览模式下 `PARAGRAPH` 按 10 个父块截断（硬编码），`FULL_DOC` 按 `CHILD_CHUNKS_PREVIEW_NUMBER` 截断子块。

**架构推论：** 「父块不进向量库」是这个结构成立的核心，也是最容易误解的地方。它的收益是**召回精度与上下文完整性可以分别调优**：子块小 → 向量相似度准；父块大 → 模型看到的上下文全。代价是**检索结果不再自包含**——拿到一条命中记录必须先回表查父块，否则喂给模型的只有那个小片段，等于退化成段落结构。任何「从向量库直接读内容去拼接 prompt」的路径都要检查有没有做这步回表。

**架构推论：** 所有子块共享同一个 metadata 对象，是个**真实的实现缺陷而非设计**。它的可见后果是子块的 `doc_id` 失去唯一性——若下游用 `doc_id` 做子块的定位或去重，会误判成同一条。相对地，`doc_hash` 是每个子块单独算的（传入的是各自内容），所以内容指纹仍然正确。**「哪个字段被覆盖了」这种问题只能靠读赋值顺序才能发现，类型检查完全看不出来。**

**架构推论：** 经济模式（纯关键词）不支持父子结构，把「两种索引技术 × 三种结构」的矩阵挖掉了一个角。这是合理的能力边界——关键词检索拿到的是片段本身，没有「用小的找、用大的答」这回事，父子结构在那里没有意义。但这条限制只在代码里体现（`load` 无 `else` 分支），文档与接口层若不显式拒绝，用户会以为「切了父子 + 选经济模式」只是质量差一点。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/index_processor/processor/parent_child_index_processor.py` | `ParentChildIndexProcessor` | 父子结构的六阶段实现 |
| `api/core/rag/index_processor/processor/parent_child_index_processor.py` | `ParentChildIndexProcessor.transform` | 两种父块模式的分支：段落级 vs 全文档 |
| `api/core/rag/index_processor/processor/parent_child_index_processor.py` | `ParentChildIndexProcessor.split_child_nodes` | 用子块参数再切一次，产出 `ChildDocument` |
| `api/core/rag/index_processor/processor/parent_child_index_processor.py` | `ParentChildIndexProcessor.load` | 只写子块向量，无经济模式分支 |
| `api/core/rag/index_processor/processor/parent_child_index_processor.py` | `ParentChildIndexProcessor.clean` | 父块 id 到子块 id 的映射与两级删除 |
| `api/core/rag/index_processor/processor/parent_child_index_processor.py` | `ParentChildIndexProcessor.index` | 结构化父子输入的入库路径 |
| `api/core/rag/index_processor/processor/parent_child_index_processor.py` | `ParentChildFormatPreviewDict` | 预览结构（多一个 `parent_mode`） |
| `api/core/rag/entities/processing_entities.py` | `ParentMode` | 父块模式枚举：段落 / 全文档 |
| `api/core/rag/entities/processing_entities.py` | `Rule` | 处理规则：父块分段与子块分段两组参数 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：parent_child_index_processor.py](https://github.com/langgenius/dify/blob/main/api/core/rag/index_processor/processor/parent_child_index_processor.py)
- [dify 文档：父子分段](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents/chunking-and-cleaning-text)
- [论文：小片段检索、大片段生成（Parent Document Retriever 思路）](https://arxiv.org/abs/2212.10496)

## 与主核 / AI 的关系

「用小单元建索引、用大单元做返回」与主核里「二级索引指向主记录」是同一结构：索引项小、指向的目标大，检索阶段只碰索引，取数阶段再回主表。主核靠指针（`ctid` 或主键）回表，这里靠 `doc_id`/`segment_id` 回表。**两者的共同前提是：索引与数据分离后，任何「只读索引」的路径都会拿到不完整的信息。**

对 RAG 工程最实用的四条。其一：**父子结构的向量库只有子块**，从向量库直接取出的文本不足以回答，必须回表取父块；如果某条 RAG 链路「检索后直接拼 prompt」，父子结构的收益会完全消失。其二：**`FULL_DOC` 适合短文档、`PARAGRAPH` 适合长文档**——前者父块是全文，一篇一万字的文档塞进去会超出模型上下文，此时应改用 `PARAGRAPH`。其三：**经济模式（关键词）不支持父子结构**，选了也不会报错，只是子块没进关键词库；要在接口层自己拦。其四：子块的 `doc_id` 存在被共享覆盖的实现问题，**不要用子块的 `doc_id` 做唯一性判断**，用 `doc_hash` 或数据库行 id 更可靠。
