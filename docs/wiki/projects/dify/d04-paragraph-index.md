---
project: dify
feature: paragraph-index
title: 段落索引处理器
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/index_processor/processor/paragraph_index_processor.py
    symbols: [ParagraphIndexProcessor]
---

# 段落索引处理器

## 一句话职责

`ParagraphIndexProcessor` 是默认的索引结构：一段文本切一片、一片一个向量、一片一条 `DocumentSegment` 行——它是另外两种结构（父子、问答）参照的基准实现。

## 机制是什么

六个阶段里它全部有实质实现，不像问答那样靠规则解析、也不像父子那样有两级文档。核心是 `transform` 与 `index` 两步，其余四步是外围：

- `extract` 直接转调 `ExtractProcessor.extract`，把 `is_automatic` 按 `process_rule_mode` 是否为 `automatic`/`hierarchical` 决定。
- `transform` 清洗 + 切分 + 打元数据 + 抽图片附件。
- `index` 把纯字符串列表或结构化的多模态分块转成 `Document`，落库并写向量/关键词。
- `load` / `clean` 在「已有文档列表」层面写与删，供重新索引的批量路径使用。
- `format_preview` 把分块列表变成前端预览结构。
- `generate_summary_preview` / `generate_summary` / `generate_summary_from_inputs` 三层摘要能力。

它的占比很有意思：677 行里，**真正属于「段落索引」的只有 transform/index/load/clean 约 150 行**，剩下大半是实现摘要——而摘要是三种结构都要的能力。所以这个类实际上是「段落处理器 + 摘要引擎的实现载体」。

## 怎么实现的

**`transform` 里的处理顺序不能调换。** 依次是：取 process_rule（缺失直接抛）→ 按 `mode` 决定规则来源（`automatic` 用 `DatasetProcessRule.AUTOMATIC_RULES` 常量、否则必须自带 `rules`）→ 校验 `segmentation` 存在 → 建切分器 → 逐文档清洗、切分、逐片段处理。

逐片段处理这一段有六个连续动作：

1. `strip()` 后为空则丢弃；
2. 生成 `doc_id`（UUID）与 `doc_hash`（内容哈希）；
3. 写进 metadata（`metadata` 非 `None` 才写）；
4. 抽图片附件（**这是唯一一处传 `current_user` 的地方**，因为外链图片下载要做权限判定）；
5. 有附件就挂到 `document_node.attachments`；
6. `remove_leading_symbols` 去掉片段开头的引导符号再 `strip`，仍非空才收下。

第 6 步值得单独看：**先判空、后清洗、再判空**。第一次判空挡的是切分器可能产出的空片段；第二次挡的是「清洗后只剩符号」的片段。两次判断之间还插了图片抽取——意味着一个「仅有引导符号 + 一张图」的片段会被抽图但最终丢弃。这是当前顺序的副作用。

**`doc_id` 与 `doc_hash` 是两个不同粒度的身份。** `doc_id` 每片段一个新 UUID，`doc_hash` 是内容哈希。前者让同一份文档的多个片段有独立 id，后者让「内容相同的片段」可被识别——重新索引时靠它做差异比对。

**`index` 有两条形态极不对称的输入路径。** `isinstance(chunks, list)` 时按纯字符串处理，否则当作 `MultimodalGeneralStructureChunk` 校验。第一条路径的元数据是现造的（`dataset_id`/`document_id`/`uuid`/内容哈希），第二条路径的元数据来自分块自身携带的信息，还会从 `general_chunk.files` 逐文件造 `AttachmentDocument`（`doc_type` 标成 `IndexDocType.IMAGE`），没有文件时才去正文里反查图片（用 `document.created_by` 找账号）。

两条路径最后汇到同一段收尾：算 token 数、`DatasetDocumentStore.add_documents(save_child=False)`、`session.commit()`、然后按 `indexing_technique` 分流——高质量走向量库（并额外 `create_multimodal` 落图片向量），经济走关键词库。

**`save_child=False` 是段落结构的标志。** 父子结构在那里传的是 `True`。这个布尔值就是「要不要额外建子块记录」的开关，段落结构没有子块。

**`load` 与 `index` 的差别在「谁来造 Document」。** `index` 从 chunks 造，`load` 收现成的 `Document` 列表（来自 `transform` 的输出）。声明上 `load` 收 `multimodal_documents` 参数，而 `index` 是自己内部收集。所以这两条路径是**平行的两条写入通道**，不是串行关系——批量重新索引走 `load`，初次索引走 `index`。

`load` 里有一处顺序细节：高质量模式落完向量后把 `with_keywords` 强制置 `False`。所以「高质量 + 关键词」不会同时写——高质量模式下只走向量，经济模式才写关键词。`with_keywords` 参数只在经济模式下有效。

**`clean` 里有一段注释解释「禁用」与「删除」的分野。** 摘要索引在片段被**禁用**时只是停用、不删除；只有真正删除片段时才删摘要，且必须显式传 `delete_summaries=True`。注释点明：禁用操作走的是任务里直接调 `disable_summaries_for_segments`，不经过这个方法。这是一个刻意的「默认不删」——摘要生成很贵，误删不可恢复。

**预览摘要是「并发 + 失败即整体失败」。** `generate_summary_preview` 用 `ThreadPoolExecutor` 并发，每个任务里 `flask_app.app_context()` 包一层（因为 worker 线程没有请求上下文），错误分两类处理：`ModerationInvocationError` 原样抛（要让 HTTP 层映射成内容审核错误），其余收进列表。只要有错就拼一条汇总消息抛 `ValueError`，只列前三条，后面写「(and N more)」。

**摘要的三层结构值得单独说明。** `generate_summary_preview`（并发编排）→ `generate_summary`（单条，含图片加载策略）→ `generate_summary_from_inputs`（实际调模型，带 `@with_credit_usage_created_by` 记额度）。中间那层的图片策略是：有 `segment_id` 就先从段附件绑定表取图，取不到（或无 id）再从正文里的 Markdown 反查图——**优先用已经落库的附件，回退到正文解析**。

**自定义提示词不会被填充语言占位符。** `generate_summary_from_inputs` 里只有默认提示才做 `.format(language=...)`，用户自定义提示原样使用。注释给了理由：避免干扰用户自己模板里的花括号。

**思考型模型的输出会被剥掉推理段。** 该方法的 docstring 写明「Generate summary text with model reasoning removed」，返回值是「清理后文本 + 原始 `LLMUsage`」；若整段输出只有推理，返回空串让调用方处理。这条设计承认了推理模型的存在：摘要要的是结论，不是思考过程。

## 关键取舍

**源码事实：** `transform` 对每个片段的处理顺序是「判空 → 造 id/哈希 → 写 metadata → 抽图片 → 挂附件 → 去引导符号 → 再判空」，图片抽取发生在第二次判空之前。

**源码事实：** `index` 按 `chunks` 是不是 `list` 分两条路径，第二条路径按 `MultimodalGeneralStructureChunk` 校验并使用其自带的元数据。

**源码事实：** 高质量模式下 `load` 强制 `with_keywords = False`，只写向量库；经济模式才写关键词库。

**源码事实：** 段落结构的 `add_documents` 传 `save_child=False`（父子结构传 `True`）。

**源码事实：** `clean` 默认不删摘要，必须显式传 `delete_summaries=True`；注释说明禁用操作走另一条路径。

**源码事实：** 预览摘要并发执行、任一失败即抛；内容审核错误保留原类型上抛，其余合并成一条 `ValueError` 只列前三条。

**源码事实：** 只有默认摘要提示会填充语言占位符，自定义提示原样使用。

**架构推论：** 「段落结构是基准实现」这个定位解释了为什么摘要引擎长在这个类里：三种结构里段落最常用，摘要需求也最常落在它身上；等父子/问答需要摘要时，是它们去调这个类的静态方法，而不是反过来。代价是**类职责明显超出单个结构**——677 行里大半与「段落」无关，看类名猜不到这里有摘要引擎。

**架构推论：** `doc_id` 每片段一个新 UUID、`doc_hash` 是内容哈希，这两个身份的组合让「重新索引」能同时满足两种需求：按 `doc_id` 定位具体片段行、按 `doc_hash` 判断内容是否变化。只用 id 无法知道内容变没变；只用哈希无法定位具体行。这是「行身份 + 内容身份」双列的典型形态，与主核里「行 id + 内容校验和」的做法同构。

**架构推论：** 预览路径「失败即整体失败」而正式路径「尽力而为」——这两条路径的失败语义相反，是有意的。预览是用户在做决策，返回不完整的结果会误导决策；正式索引只需尽力把能写的写进去，失败的片段会留在待重试状态。**同一个能力在两条路径上给出相反的容错级别，判断依据是「这次调用的产物要不要被人拿去做决定」。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor` | 段落结构的六阶段实现 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor.transform` | 清洗、切分、打双 id、抽图片附件 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor.index` | 两种 chunks 形态转文档并落库/写库 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor.load` | 现成文档批量写入，高质量模式跳过关键词 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor.clean` | 按需删摘要 + 删向量/关键词 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor.format_preview` | 分块转预览结构 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor.generate_summary_preview` | 预览态并发摘要，失败即整体失败 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor.generate_summary` | 单条摘要，含图片加载优先级 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor.generate_summary_from_inputs` | 调模型、剥推理段、记额度 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphFormatPreviewDict` | 预览结构形状 |
| `api/core/rag/index_processor/processor/paragraph_index_processor.py` | `ParagraphIndexProcessor._extract_images_from_text` | 正文 Markdown 反查图片 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：paragraph_index_processor.py](https://github.com/langgenius/dify/blob/main/api/core/rag/index_processor/processor/paragraph_index_processor.py)
- [dify 文档：知识库分段设置](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents/chunking-and-cleaning-text)
- [dify 文档：多模态知识库](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents)

## 与主核 / AI 的关系

「一片一行 + 内容哈希」与主核里「一条记录一行 + 校验和」是同一种双身份设计：行定位靠自增/随机 id，内容比对靠哈希。主核用校验和做备份验证与损坏检测，这里用 `doc_hash` 做增量重索引的差异判断——同一个信息（内容指纹）在两个系统里服务于不同的目的。

对 RAG 工程最实用的四条。其一：**高质量模式不会写关键词索引**，所以「高质量建库后切换到经济模式」不会自动补上关键词，需要重建。其二：**`clean` 默认不删摘要**，想彻底清掉必须显式传参——这既是保护（摘要很贵）也是坑（以为删了就干净）。其三：**同一份正文里的同一张图会被抽成多个附件**（按出现次数），占多个向量槽位；图片密集的文档要留意向量数量膨胀。其四：**预览与正式索引的失败语义不同**，前者整体失败、后者尽力而为；写自动化脚本时不要把预览路径的异常当成正式索引也会失败。
