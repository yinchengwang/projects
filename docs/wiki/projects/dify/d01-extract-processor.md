---
project: dify
feature: extract-processor
title: 文档抽取分派
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/extractor/extract_processor.py
    symbols: [ExtractProcessor]
---

# 文档抽取分派

## 一句话职责

`ExtractProcessor` 把「一个上传文件 / 一个 Notion 页面 / 一个网址」变成 `list[Document]`，靠的是按**数据源类型 + 文件后缀 + ETL 模式**这三层键去查一张手写的分派表。

## 机制是什么

三个入口，一个真正的实现：

- `load_from_upload_file`：把 ORM 的 `UploadFile` 包成 `ExtractSetting`，再走 `extract`；`return_text=True` 时把多份 `Document` 的正文用换行拼成一段字符串。
- `load_from_url`：先 `remote_fetcher.make_request` 把内容抓下来，写进临时目录，再当本地文件走同一条路径。
- `extract`：唯一带分派逻辑的方法，按 `extract_setting.datasource_type` 分三支——`FILE`、`NOTION`、`WEBSITE`；都不是就 `raise ValueError`。

关键点在于：**它不是工厂，也不是注册表**。文件分支是一段约百行的 `if/elif` 链，直接 `new` 出具体 extractor 类。要加一种格式，只能改这段链——但这也意味着「支持哪些格式」在一屏代码里能全部看到。

`is_automatic` 这个布尔值会影响分支走向，是整段分派里唯一「同一个后缀走不同实现」的地方。

## 怎么实现的

**两个 ETL 模式各有一整套 `if/elif`。** `dify_config.ETL_TYPE` 是 `"Unstructured"` 还是别的，决定进入哪一半。两半共享的后缀（`.xlsx`/`.xls`、`.pdf`、`.htm`/`.html`、`.docx`、`.csv`）在两半里各写一遍，构造参数完全相同；差异只体现在少数几个后缀上：

| 后缀 | Unstructured 模式 | 另一模式 |
|---|---|---|
| `.md` `.markdown` `.mdx` | `is_automatic` 为真走 `UnstructuredMarkdownExtractor`，否则走 `MarkdownExtractor` | 恒走 `MarkdownExtractor` |
| `.doc` | `UnstructuredWordExtractor` | **不支持**（落到 `TextExtractor`） |
| `.msg` `.eml` `.ppt` `.pptx` `.xml` | 全部走对应的 Unstructured 提取器 | 不支持 |
| `.epub` | `UnstructuredEpubExtractor` 带 API 参数 | `UnstructuredEpubExtractor` 不带 API 参数 |

也就是说「ETL 模式」并不是给所有格式换实现，而是**给 Unstructured 模式多开几种格式**。`.doc` 这个差异最值得注意：在非 Unstructured 模式下它不会报错，而是**静默落到 `TextExtractor`**——因为那条链的最后是 `else: TextExtractor(...)`。

**兜底的 `else` 既是「txt 支持」也是「未知格式的接收器」。** 两条链的注释都写着 `# txt`，但它的实际语义是「所有没被前面 `elif` 命中的后缀」。所以上传 `.xyz` 不会失败，会被当文本读。**这是静默降级，不是显式拒绝。**

**后缀来源在被分派之前就已经确定，但两条入口的来源不同。** 文件入口用 `Path(upload_file.key).suffix`——即存储 key 的后缀，而不是原始文件名；URL 入口则费了不少代码从 URL 推导，且刻意只用 `urlsplit(url).path` 的后缀，注释说明了原因：query string 与 fragment 不属于文件名，混进去会让后缀变成 `".pdf?token=xxx"` 这样的东西，从而误路由到错误提取器。

URL 那条的回退链有三层：URL 路径后缀 → `Content-Type` 子类型（先按分号截断，丢掉 `charset=utf-8` 这类参数）→ `Content-Disposition` 里的文件名（再 `unquote` 一次）。三层都拿不到就留下空后缀，最终落到 `TextExtractor`。

**`UploadFile` 在三个分支里的假设不一致。** 文件分支开头 `assert upload_file is not None`，但同一条链里 pdf 那几个构造却写 `upload_file.tenant_id if upload_file else None`——已经断言非空了，还在判断空。这是 URL 入口复用 `extract` 时留下的痕迹：URL 路径构的 `ExtractSetting` **不带 upload_file**，靠显式传 `file_path` 绕过下载分支。于是同一个方法要同时容忍「有 upload_file」与「只有 file_path」两种调用，而断言只在前者成立的那条子路径上。

`extract_setting` 里的 `document_model="text_model"` 也是这两条入口硬写的常量，不是从数据集配置读来的。

**三个数据源类型的分支密度极不对称。** `NOTION` 与 `WEBSITE` 各只有十几行，`FILE` 独占了近两百行。原因是后两者是「一个 provider → 一个类」的映射（`match website_info.provider` 分 `firecrawl`/`watercrawl`/`jinareader`，`case _` 抛 `ValueError`），而文件是「一个后缀 → 一个类」的映射，维度天然更宽。

**Notion 的凭证是延迟取的。** `NotionExtractor` 收一个 `notion_token_loader=lambda: ...`，调用时才去数据库读 access token。这是刻意的：token 可能刷新，构造期取值会让长时间任务用到过期凭证。

**所有 extractor 都是「构造后 `extract()`」，没有生命周期。** 除了 pdf/word 那两个多收了 `session` 与 `session_maker`（它们要落中间产物，例如图片提取），其余提取器只有文件路径与少量参数。所以这个类的职责边界很清晰：**只做选择，不做转换**。

## 关键取舍

**源码事实：** 分派是手写的 `if/elif` 链，不是注册表或工厂映射。新增格式必须改这个方法。

**源码事实：** 两条链的最后都是 `else: TextExtractor(...)`，注释写 `# txt`，但实际接收所有未匹配后缀——未知格式静默当文本处理。

**源码事实：** `ETL_TYPE` 为 Unstructured 时多支持 `.doc`/`.msg`/`.eml`/`.ppt`/`.pptx`/`.xml`；非 Unstructured 模式下 `.doc` 落到 `TextExtractor` 而不是报错。

**源码事实：** URL 入口的后缀推导优先用 URL 路径，然后 `Content-Type` 子类型，最后 `Content-Disposition` 文件名；注释点明 query string 与 fragment 不能进后缀。

**源码事实：** 文件分支开头断言 `upload_file` 非空，但 pdf/word 构造里仍写 `if upload_file else None`——因为 URL 入口复用 `extract` 时不传 `upload_file`、只传 `file_path`。

**源码事实：** Notion 的 access token 用 lambda 延迟加载，不在构造期取值。

**架构推论：** 手写 `if/elif` 而非注册表，在**格式种类有限且每种都要不同构造参数**时是正确选择。注册表要求「所有实现同一签名」，而这里的 extractor 构造参数差异很大（有的要 tenant_id、有的要 unstructured API key、有的要 session）。要抽象就得引入参数包，那反而比 `if/elif` 更难读。**代价只是「加格式要改中心文件」这一条**，而在单仓单团队场景下这条代价很低。

**架构推论：** 「未知后缀落到文本提取器」是这一层最危险的取舍。它把「不支持这个格式」这种配置/用户错误变成了「内容看起来是乱码」这种质量问题，而且失败发生在下游切分与嵌入之后。安全的方向应该是**显式白名单 + 显式拒绝**，至少对二进制后缀要拒绝——把 `.docx` 误当文本读出来的东西会被切成大量垃圾片段，且很难从结果倒推原因。

**架构推论：** 两份几乎相同的 `if/elif` 说明「ETL 模式」被实现成了编译期分支而不是运行期策略。改成「后缀 → 候选提取器列表，按模式挑选」能让共享部分只写一次；但当前写法有个隐性好处：**任一模式下支持什么格式，读那一段就够了**，不必跨表推理。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/extractor/extract_processor.py` | `ExtractProcessor` | 抽取统一入口（三个类方法） |
| `api/core/rag/extractor/extract_processor.py` | `ExtractProcessor.load_from_upload_file` | 上传文件入口，可返回文本或文档列表 |
| `api/core/rag/extractor/extract_processor.py` | `ExtractProcessor.load_from_url` | 抓取 URL 到临时文件后复用文件路径 |
| `api/core/rag/extractor/extract_processor.py` | `ExtractProcessor.extract` | 三层键分派：数据源类型 → ETL 模式 → 后缀 |
| `api/core/rag/extractor/entity/extract_setting.py` | `ExtractSetting` | 分派输入：数据源类型 + 各类型专属信息 |
| `api/core/rag/extractor/extractor_base.py` | `BaseExtractor` | 提取器契约（`extract() -> list[Document]`） |
| `api/core/rag/extractor/notion_extractor.py` | `NotionExtractor` | Notion 页面抽取，凭证延迟加载 |
| `api/core/rag/extractor/firecrawl/firecrawl_web_extractor.py` | `FirecrawlWebExtractor` | 网址抓取的一种 provider |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/rag/extractor/extract_processor.py](https://github.com/langgenius/dify/blob/main/api/core/rag/extractor/extract_processor.py)
- [dify 文档：设置 ETL 与 Unstructured](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents/chunking-and-cleaning-text)
- [Unstructured 文档：分区与提取](https://docs.unstructured.io/open-source/core-functionality/partitioning)

## 与主核 / AI 的关系

抽取层的选择结构与主核里「按文件/表空间形态挑访问方法」是同一件事，但两者的失败策略截然相反。主核遇到不认识的文件头会**明确报错**，因为它要保证数据完整性；这里遇到不认识的后缀会**退回文本提取**，因为它要保证「用户上传了就能继续走流程」。

对 RAG 工程最实用的三条。其一：**未知格式静默降级成文本**是这一层最需要警惕的行为。上传一份 `.doc` 而 ETL 没配 Unstructured，得到的不是报错而是乱码片段——排查时应该先确认解析出来的 `page_content` 是否是人类可读的，再去怀疑切分与嵌入。其二：**URL 导入的后缀判定只看路径**，所以 `https://host/download?id=123` 这类没有扩展名的链接会走 `Content-Type` 回退；如果服务端返回 `application/octet-stream`，最终会落到文本提取器。其三：`ETL_TYPE` 是**全局配置**，切换它会让同一批文档产出不同的片段数与内容，所以索引过的知识库不要在中途改这个开关。
