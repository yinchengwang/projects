---
project: dify
feature: index-processor
title: 索引处理器工厂与基类
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/index_processor/index_processor.py
    symbols: [IndexProcessorFactory, IndexProcessor]
  - path: api/core/rag/index_processor/index_processor_base.py
    symbols: [BaseIndexProcessor]
---

# 索引处理器工厂与基类

## 一句话职责

`IndexProcessorFactory` 按数据集的 `chunk_structure` 三选一给出处理器实例，而 `BaseIndexProcessor` 定下六步流水线的抽象方法，并把「切分器选择」与「图片附件抽取」两段横切逻辑沉淀成模板方法。

## 机制是什么

摄取流水线被拆成六个阶段，每个阶段一个抽象方法：

| 阶段 | 方法 | 形态 |
|---|---|---|
| 抽取 | `extract(extract_setting)` | 输入设置 → 文档 |
| 转换 | `transform(documents)` | 文档 → 更小的文档 |
| 预览摘要 | `generate_summary_preview(texts)` | 预览文本 → 带摘要的预览 |
| 写入 | `load(dataset, documents)` | 文档 → 向量库/关键词库 |
| 清理 | `clean(dataset, node_ids)` | 按节点 id 或全量删除 |
| 预览格式化 | `format_preview(chunks)` | 分块 → 前端可展示结构 |

前四个是「写」，后两个是「读/删」。三层（段落 / 父子 / 问答）各自实现这六个方法，**签名里的 `session` 一律是关键字参数**（`*, session: Session`），只有 `index` 是位置参数——这处不一致本身是个信号：`index` 的签名（`dataset, document, chunks, session`）看着像旧版遗留，其余方法后来统一成了「位置参数 + 关键字 session」。

`IndexProcessorFactory` 只是把 `chunk_structure` 字符串映射到实例，构造时存类型、调用时才 new——所以它是「延迟构造」，同一个 factory 可以被反复 `init_index_processor()`。

`IndexProcessor` 是另一层门面，它**不是**基类的子类，而是编排者：持有一个 factory 选出处理器，然后负责「读数据集与文档 → 清理旧索引 → 写新索引 → 更新文档状态」这串事务性动作。

## 怎么实现的

**工厂只有三条分支加一个兜底。**

```python
if self._index_type == IndexStructureType.PARAGRAPH_INDEX: ...
elif self._index_type == IndexStructureType.QA_INDEX: ...
elif self._index_type == IndexStructureType.PARENT_CHILD_INDEX: ...
else: raise ValueError(f"Index type {self._index_type} is not supported.")
```

`index_type` 为空时抛 `ValueError("Index type must be specified.")`，与「类型不支持」分成两条不同消息——这个区分有用：空值是配置缺失，非法值是数据错误。

`summary` 虽然是第四种「索引结构」（在别处有 `SummaryIndex` 这类实现），但**不在这条分派里**。摘要索引不是 `chunk_structure` 的一个取值，而是叠加在上面三种之上的一层开关（`summary_index_setting.enable`）。这解释了为什么工厂只有三支。

**基类的 `_get_splitter` 是唯一被三种处理器共用的构造逻辑。** 它按 `processing_rule_mode` 分两路：

- `"custom"` 或 `"hierarchical"` → `FixedRecursiveCharacterTextSplitter.from_encoder`，用用户给的 `max_tokens`、`chunk_overlap`、`separator`。这里做了**上下界校验**：`max_tokens` 小于 50 或大于 `INDEXING_MAX_SEGMENTATION_TOKENS_LENGTH` 就抛 `ValueError`。分隔符会把字面 `\n` 换成真换行。
- 其他（即 `"automatic"`）→ `EnhanceRecursiveCharacterTextSplitter.from_encoder`，参数直接取 `DatasetProcessRule.AUTOMATIC_RULES` 里的常量，用户参数一概不用。

两路的分隔符列表都是 `["\n\n", "。", ". ", " ", ""]`——比切分器自身的缺省列表少了 `"\n"` 这一级。这是个容易忽略的细节：**自定义模式下「单换行」不是切分边界**，空行才是。

`hierarchical`（父子分块）与 `custom` 走同一个分支，说明父子结构在切分阶段与自定义段落完全等价——差异发生在后面「把父块再切成子块」那一步，而不是切分器选择上。

**`_get_content_files` 是多模态的入口，它做的是「从 Markdown 正文里反查图片」。** 流程是：正则抠所有 `![...](url)`，对每个 url 依次尝试三种模式：

1. `/files/<uuid>/image-preview`——v0.10.0 之前的上传图片。
2. `/files/<uuid>/file-preview`——v0.10.0 之后。
3. `/files/tools/<uuid>.<ext>`——工具产出的文件，要经 `_download_tool_file` 从 `ToolFile` 表与存储里取出来再转成 `UploadFile`。

三条都不中时（且 `current_user` 存在），当外链处理：解析 URL，只接受 `http`/`https` 且有 netloc 的，然后 `_download_image` 下载并注册成 `UploadFile`。

**一份图片会被登记多次，这是刻意的。** 先把所有 `upload_file_id`（**含重复**）收进一个列表，注释写明「preserve occurrence count」；然后 `set` 去重只为查库，最后按原始列表（含重复）逐个生成 `AttachmentDocument`。所以同一张图在正文出现三次，就产出三个附件文档——它们分别对应三次出现的语义位置。

`_download_image` 的防御密度很高，值得逐条记住：`Content-Length` 头先挡一次超限；下载过程中按 8KB 分块累积，**边下边判**是否超 `ATTACHMENT_IMAGE_FILE_SIZE_LIMIT`；空内容拒绝；文件名从 `Content-Disposition` 取（用 werkzeug 的 `parse_options_header`，能解 `filename*`），取不到就退回 URL 的 basename，再取不到用 `downloaded_image_file`；后缀若缺失、或原名是 `.php`/`.jsp`/`.asp`/`.html` 这类服务端后缀，就用 `mimetypes.guess_extension(content_type)` 换成真实图片后缀。异常分三类分别记日志并返回 `None`（超时、请求错、其它），**不向上抛**。

那段后缀判断的运算符优先级容易被读错：

```python
if not current_ext and real_ext or current_ext in [".php", ".jsp", ".asp", ".html"] and real_ext:
```

`and` 优先级高于 `or`，实际语义是「（无后缀且有真实后缀）或（是服务端后缀且有真实后缀）」。它能工作，但读的时候需要自己加括号。

**`IndexProcessor.index_and_clean` 是那条事务脚本，顺序是有讲究的。** 依次：

1. 查数据集与文档（都带 tenant 约束），快照 `name`/`created_at` 等值。
2. 若给了 `original_document_id`，读出旧文档所有段的 `index_node_id`——这是「重新索引」场景。
3. `session.commit()`，注释写明原因：**上面那些元数据读不能带着事务跨过向量 I/O**。
4. 只有首次索引（无 `original_document_id`）才走 `VectorSpaceAdmissionService().ensure_pipeline_can_be_indexed`——注释写「V1 guards only first-time indexing」。
5. 有旧节点 id 就先 `clean`（`with_keywords=True, delete_child_chunks=True`），再删 `DocumentSegment` 行。
6. `index_processor.index(...)` 写新索引，commit。
7. 回填 `document.indexing_latency`（用 `perf_counter` 差值）、`indexing_status=COMPLETED`、`completed_at`、`word_count`（对段表 `sum(word_count)` 聚合）、`need_summary`；把该文档所有段置为 `COMPLETED` + `enabled=True`。
8. 组装 `IndexingResultDict` 返回给调用方（批次、文档名、创建时间戳、展示状态）。

第 3 步的「先 commit 再 I/O」和 `session.flush()` 而不是 `commit()` 收尾，这两处放在一起看才明白意图：**长事务被刻意切断成「读—提交—外部 I/O—写」**，让向量库那段不可控耗时不会把数据库事务一直挂着。

**`get_preview_output` 在预览路径上又实现了一遍摘要生成，且失败即整体失败。** 它与各处理器的 `generate_summary_preview` 做同一件事但代码不同：这里用 `ThreadPoolExecutor`，每个任务里开 `flask_app.app_context()` 与一个新的 `session_factory.create_session()`，超时是 `min(300, 60 * 片段数)`，超时与异常都收进 `errors`，最后若有错就抛 `KnowledgeIndexNodeError`。注释解释了为什么预览模式下必须失败而不是返回部分结果——**预览是用户在确认「要不要按这个规则建库」，返回半份摘要会让人以为一切正常**。

注意它只在 `indexing_technique == HIGH_QUALITY` 且摘要开关打开时才做摘要，否则直接返回纯预览。

## 关键取舍

**源码事实：** 工厂只有三支（段落 / 问答 / 父子），摘要不是第四种 `chunk_structure`，而是叠加在前三者之上的开关。

**源码事实：** `index` 的 `session` 是位置参数，其余五个抽象方法的 `session` 都是关键字参数；`index` 的签名形态与其它方法不一致。

**源码事实：** `_get_splitter` 在 `custom`/`hierarchical` 下校验 `max_tokens` 落在 [50, 上限]，并用 `FixedRecursiveCharacterTextSplitter`；`automatic` 下忽略用户参数、直接用常量走 `EnhanceRecursiveCharacterTextSplitter`。两路的分隔符列表都不含 `"\n"`。

**源码事实：** 图片附件按出现次数生成（含重复），`set` 只用于查库去重。

**源码事实：** `_download_image` 对超限做了两道检查（`Content-Length` 头 + 分块累积），异常一律吞掉返回 `None`。

**源码事实：** `index_and_clean` 在进入向量 I/O 前显式 `session.commit()`，并且只有首次索引才做向量空间准入检查。

**架构推论：** 六阶段抽象 + 三个实现，是本项目里最标准的模板方法用法。它的价值在于**新结构只需实现六个方法**，而「切分器怎么选」「图片怎么抽」这两个所有结构都要做的事已经在基类里。代价也清晰：任何结构若想用不同的切分策略（比如按 token 而不是字符），就得绕开 `_get_splitter` 自己造——当前没有留钩子。

**架构推论：** 把摘要做成「叠加开关」而非「第四种结构」，避免了一个组合爆炸：否则「父子 + 摘要」「问答 + 摘要」都要各写一遍处理器。代价是每个处理器的 `generate_summary_preview` 都得自己判断「我这类要不要生成摘要」——问答那类直接原样返回，段落那类真做，父子那类做自己的版本。**开关外置省了类数量，却把那句判断复制到了每个实现里。**

**架构推论：** 「先 commit 再做外部 I/O」是这类混合事务的通用手法，但它同时也意味着**一旦向量写入失败，数据库里已经提交的元数据读是既成事实**——不会有回滚把它们撤销。真正的一致性靠的是「文档状态仍是 indexing（未 COMPLETED）」这个标记，靠重试来补。这是「跨系统事务」的必然形态，必须靠状态机而不是事务来保证最终一致。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/index_processor/index_processor.py` | `IndexProcessorFactory` | 按 `chunk_structure` 三选一分派处理器 |
| `api/core/rag/index_processor/index_processor.py` | `IndexProcessor` | 编排门面：读元数据、清理旧索引、写入、回填状态 |
| `api/core/rag/index_processor/index_processor.py` | `IndexProcessor.index_and_clean` | 重新索引事务脚本，含跨系统 I/O 的事务切分 |
| `api/core/rag/index_processor/index_processor.py` | `IndexProcessor.get_preview_output` | 预览路径，并发生成摘要且失败即整体失败 |
| `api/core/rag/index_processor/index_processor.py` | `IndexProcessor.format_preview` | 统一三种处理器形状不同的预览 |
| `api/core/rag/index_processor/index_processor_base.py` | `BaseIndexProcessor` | 六阶段抽象方法定义 |
| `api/core/rag/index_processor/index_processor_base.py` | `BaseIndexProcessor._get_splitter` | 按处理规则模式选切分器，自定义模式校验范围 |
| `api/core/rag/index_processor/index_processor_base.py` | `BaseIndexProcessor._get_content_files` | 从 Markdown 反查图片并按出现次数生成附件 |
| `api/core/rag/index_processor/index_processor_base.py` | `BaseIndexProcessor._download_image` | 外链图片下载，双重体积限制与后缀纠正 |
| `api/core/rag/index_processor/index_processor_base.py` | `BaseIndexProcessor._download_tool_file` | 工具产出文件转上传文件 |
| `api/core/rag/index_processor/index_processor_base.py` | `SummaryIndexSettingDict` | 摘要开关的配置形状（enable 加可选模型与提示） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/rag/index_processor/index_processor.py](https://github.com/langgenius/dify/blob/main/api/core/rag/index_processor/index_processor.py)
- [dify 源码：core/rag/index_processor/index_processor_base.py](https://github.com/langgenius/dify/blob/main/api/core/rag/index_processor/index_processor_base.py)
- [dify 文档：知识库索引方式](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents)

## 与主核 / AI 的关系

「六阶段抽象 + 一处分派」与主核里「访问方法（AM）抽象 + 存储引擎分派」是同一种分层：把一个跨系统的写路径切成固定阶段，每阶段由具体实现填充。主核的 AM 只有 insert/scan/update/delete 四个阶段，这里的阶段更多（因为多了「抽取」与「预览」这两件事）。

对 RAG 工程最实用的四条。其一：**`automatic` 模式忽略你传的分段参数**——想改片段大小必须切到 `custom`，否则改参数不会生效且不报错。其二：**`hierarchical` 与 `custom` 在切分阶段等价**，父子结构的特殊之处只体现在「父块再切子块」那一步。其三：**图片附件是按出现次数生成的，不是按图片去重**，所以正文里同一张图重复引用会产出多个附件文档、占用多个向量槽位；想省空间要在清洗阶段先去重。其四：**索引失败后数据库里可能已经留下元数据**（因为进外部 I/O 前已 commit），重试前应先确认文档状态而不是假设「什么都没写」。
