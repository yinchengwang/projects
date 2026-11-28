---
project: dify
feature: qa-index
title: 问答对索引处理器
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/index_processor/processor/qa_index_processor.py
    symbols: [QAIndexProcessor]
---

# 问答对索引处理器

## 一句话职责

`QAIndexProcessor` 不走「切片段」而走「造问答对」：把每段文本交给模型生成一组 `Qn/A n` 格式的问答，解析成结构化列表，**用问题当检索文本、用答案当元数据**。

## 机制是什么

这是三种结构里唯一**改变内容形态**的那个。段落与父子改变的是「切多大」，问答改变的是「存什么」：

- 段落：存片段原文，向量来自原文；
- 父子：存父块 + 子块，向量来自子块原文；
- 问答：**存问题**（`page_content` 是问题），**答案放在 `metadata["answer"]`**。

所以向量相似度算的是「用户提问」与「库里问题」的接近程度，而不是与原文段落的接近程度。这让检索的语义对齐方式发生了根本变化：从「措辞相似」变成「问法相似」。

它有一条独占的输入格式——**CSV 模板导入**（`format_by_template`），第一列问题、第二列答案。这条路径完全跳过切分与 LLM 生成。

## 怎么实现的

**`transform` 的主链路：切分 → 逐段生成问答 → 并发。** 切分部分与段落结构无关差异（清洗、切分、给每个片段造 `doc_id`/`doc_hash`、去引导符号），真正的差异在后半：

- 取 `tenant_id`，不是 `str` 或空串就抛 `ValueError("tenant_id is required for QA generation")`——因为后面要调模型，必须有租户上下文。
- 从 `app_ctx.app` 取 Flask 应用对象（模块级导入 `flask.globals.app_ctx`，而不是 `current_app` 代理），因为工作线程里需要**真实 app 对象**来开上下文。
- `preview` 为真时只处理**第一个片段**（`all_documents[0]`），够前端展示即可。
- 否则按批 10 个片段提交到 `ThreadPoolExecutor(max_workers=10)`，每批之后 `future.result()` 收集。

注意它**没有用 `as_completed`**，而是在每批里按提交顺序 `future.result()`。所以是「分批并发、批内按序等待」——不是完全流水线，并发度受批大小限制。

**`_format_qa_document` 的异常处理是分形的。** 方法体里：

```python
try:
    ... 生成与解析 ...
except ModerationInvocationError:
    raise
except Exception:
    logger.exception("Failed to format qa document")
```

**内容审核失败向上抛，其余异常吞掉只记日志**。而 `transform` 的 docstring 明确写了这条契约：「propagating moderation failures from preview and bounded workers. No partial result is returned after a failed moderation check.」——审核失败要让整批作废（因为内容合规是硬约束），而普通生成失败按「尽力而为」处理，那些片段就不产出问答对。

同时注意 `all_qa_documents.extend` 的位置：它在 `try/except` **内部**，且在 `except` 之后。所以审核失败上抛时，此前已生成的结果也不会被追加——`format_documents` 只属于这一次调用，抛出即丢弃。这是「审核失败不产生部分结果」的实现方式。

**`_format_split_text` 是解析格式的地方，正则值得逐段读。**

```python
regex = r"Q\d+:\s*(.*?)\s*A\d+:\s*([\s\S]*?)(?=Q\d+:|$)"
```

- `Q\d+:` 允许 `Q1`、`Q12` 这样的编号，空前缀是 `Q` 加数字加冒号；
- 问句用 `(.*?)` 非贪婪，**不跨行**（默认 `.` 不含换行）；
- 答案用 `([\s\S]*?)` 非贪婪跨行，用前瞻 `(?=Q\d+:|$)` 停在下一个 `Q` 或文末；
- 末尾过滤 `if q and a`，丢掉空问答。

两条隐含约束：**问句不能跨行**、**答案里不能出现 `Q<数字>:` 这样的字面**。前者让提示必须要求「一行一个问题」，后者意味着答案里若提到「Q2:」会被误切成新的问答对。

答案还会做一次换行归一：`re.sub(r"\n\s*", "\n", a.strip())`——把「换行 + 任意空白」压成单个换行，即去掉续行缩进。

**生成出的问答对继承片段元数据后再覆盖三个键。** `Document(page_content=result["question"], metadata=document_node.metadata.copy())`——先拷贝父片段的 metadata（继承 `dataset_id`/`document_id` 等），再写 `answer`、新的 `doc_id`、用**问题文本**算的 `doc_hash`。

`.copy()` 这个浅拷贝在这里是必要的修复：若不拷，同一片段生成的多个问答对会共享同一个 metadata 字典，`doc_id` 会互相覆盖。**问答处理器做了这一步，父子处理器没有做**（见那一页）。

**`load` 与经济模式。** `load` 只有高质量分支，写法与段落几乎一致，但**没有 `with_keywords` 的后续处理**——参数收了却完全不读。而 `index` 那边更直接：非高质量模式**显式抛错**：

```python
if dataset.indexing_technique == IndexTechniqueType.HIGH_QUALITY:
    ...
else:
    raise ValueError("Indexing technique must be high quality.")
```

所以「问答结构必须配高质量索引」是被强制的，不是静默降级。这点与父子结构不同——父子在 `load` 里只是什么都不做，问答则抛错。**同一个约束，两处一个强制一个静默，这是实现不一致而非设计差异。**

**`clean` 无条件删向量。** 它不像段落/父子那样按 `indexing_technique` 判断，而是直接 `Vector(dataset).delete_by_ids(...)`。结合上面「问答必须高质量」这条，这里的一致性是成立的：反正只可能是高质量。

**`format_by_template` 是 CSV 快捷通道。** 它只认 `.csv` 后缀（否则抛「Only CSV files are allowed」），用 `pd.read_csv` 读，取第 0 列当问题、第 1 列当答案，元数据里只放 `{"answer": ...}`。代码注释写「Skip the first row」但实现里**没有 `skiprows`**——只要文件有表头，第一条"问答"就会是表头本身。这是注释与实现不符，读的时候以代码为准。

另外 `pd.read_csv(file)` 直接把 `FileStorage` 传进去，靠 pandas 对 file-like 的支持；异常被包成 `ValueError(str(e))` 再抛。

**`index` 走结构化输入。** `QAStructureChunk.model_validate(chunks)` 后逐条造 `Document`，`doc_hash` 用问题文本算，`answer` 进 metadata，落库 `save_child=False`，高质量写向量、否则抛错。这条路径与 `transform` 的产物形态一致，区别只是问答对来自外部而非模型生成。

**`generate_summary_preview` 直接原样返回。** docstring 给了理由：「QA model uses question-answer pairs, which don't require summary generation.」这是三种结构里唯一不支持摘要的。注释还补了一句「qa_model doesn't generate summaries, but we clean them for completeness」——在 `clean` 里仍会删摘要，因为理论上可能有历史残留。

**`format_preview` 产出的是 `qa_preview` 而不是 `preview`。** 字段名不同导致 `IndexProcessor.format_preview` 那层要先读 `preview.get("qa_preview", [])` 单独处理一遍。这是三种处理器预览形状不统一的直接证据。

## 关键取舍

**源码事实：** `page_content` 存问题、答案在 `metadata["answer"]`，所以向量检索匹配的是「问题与问题的相似」。

**源码事实：** `_format_split_text` 的正则要求问句不跨行（`. ` 不含换行），答案以 `Q<数字>:` 或文末为界；答案里出现 `Q2:` 字样会被误切。

**源码事实：** `ModerationInvocationError` 上抛、其余异常只记日志；`extend` 在 try 内部，审核失败时不返回任何部分结果。

**源码事实：** 生成问答对时 metadata 用 `.copy()` 再覆盖，因此每个问答对有独立 `doc_id`/`doc_hash`（`doc_hash` 用问题文本算）。

**源码事实：** `index` 在非高质量模式下显式抛 `ValueError`；`load` 没有对应分支也没有 `with_keywords` 处理。

**源码事实：** `format_by_template` 注释写「Skip the first row」但实现里没有 `skiprows`。

**源码事实：** `generate_summary_preview` 原样返回；`format_preview` 用 `qa_preview` 键而非 `preview`。

**架构推论：** 「存问题而非原文」把 RAG 从「找相似段落」变成「找相似提问」。收益是**用户问法与库内问法风格一致时命中率显著更高**——因为两边都是「问句」，措辞分布接近；代价是**答案的覆盖面被问题数量限制**：一段文本只生成了三个问题，就只在这三个问法上可被召回，其他表达方式全部漏掉。所以这套结构的召回率高度依赖生成阶段的问题多样性，而不是原文信息量。

**架构推论：** 内容审核失败要整体作废、普通失败要尽力而为——这条不对称的异常策略是对的。审核是合规红线，部分通过等于没通过；而「某段文本没生成出问答对」只是数据缺失，整体放弃反而浪费。实现上靠「上抛发生在 extend 之前」来保证第一部分，这是**用代码顺序实现事务语义**，脆弱但有效。

**架构推论：** 「问答必须高质量索引」这条约束在两处写法不同（`index` 抛错、`load` 静默），说明它是后来补上的判断而非初始设计。这类不一致的危害不在功能而在诊断：同一份配置在不同入口下表现不同，排障时容易以为是数据问题。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor` | 问答结构的六阶段实现 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor.transform` | 切分、并发生成问答、分批收集 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor.format_by_template` | CSV 模板导入（问题 + 答案两列） |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor.load` | 现成文档写入，仅高质量分支 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor.clean` | 无条件删向量 + 按需删摘要 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor.index` | 结构化问答输入，非高质量即抛错 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor.format_preview` | 产出 `qa_preview` 形状 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor.generate_summary_preview` | 原样返回，不支持摘要 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor._format_qa_document` | 单片段生成问答，审核错误上抛 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAIndexProcessor._format_split_text` | `Qn/An` 格式解析与答案换行归一 |
| `api/core/rag/index_processor/processor/qa_index_processor.py` | `QAFormatPreviewDict` | 预览结构（`qa_preview` 键） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：qa_index_processor.py](https://github.com/langgenius/dify/blob/main/api/core/rag/index_processor/processor/qa_index_processor.py)
- [dify 文档：知识库索引方式与问答模式](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents)
- [论文：用生成式问答对做稠密检索（QA-pair 索引思路）](https://arxiv.org/abs/2104.08663)

## 与主核 / AI 的关系

「用派生内容建索引、原始信息放元数据」与主核里「用摘要列建二级索引、原值留在主记录」是同一种信息分离：索引项不是数据本身，而是它的一个投影。主核的投影是 B 树里的键，这里的投影是模型生成的问题。**共同后果是：索引命中后必须回表取原值，否则拿到的不是答案。**

对 RAG 工程最实用的四条。其一：**问答结构的召回率上限由生成的问题数决定**，一段文本只生三个问题就只在三种问法上可召回；对高召回要求的场景，要么提高每段生成数量，要么混用段落结构。其二：**问答结构必须配高质量索引**，选经济模式会在 `index` 路径抛错；而 `load` 路径只是不写向量（不报错），排障时要注意入口。其三：**答案里不能出现 `Q<数字>:` 形式的字面**，否则会被解析器误认为新一轮问答的开头——自定义提示词时要考虑这一点。其四：**CSV 模板导入不会自动跳过表头**，用之前先把第一行处理掉，否则会得到一条"问题=列名"的脏数据。
