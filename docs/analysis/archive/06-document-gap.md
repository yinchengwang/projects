> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Document 模态差距深度分析

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查
> 代码位置：`engine/src/db/storage/doc/`（~6.6K 行，10 文件）

## 1. 实现现状盘点

### 1.1 模块清单

| 模块 | 文件 | 行数 |
|------|------|------|
| 引擎主体 | `doc_engine.c` | 954 |
| 全文搜索 + 分析器 | `doc_fts.c` | 804 |
| 嵌套文档 | `doc_nested.c` | 861 |
| 聚合管道 | `doc_pipeline.c` | 1372 |
| 聚合 | `doc_agg.c` | 705 |
| JSONPath | `jsonpath.c` | 670 |
| 混合向量 | `doc_vector.c` | 584 |
| 倒排索引 | `doc_inverted.c` | 349 |
| BM25 | `bm25.c` | 299 |

### 1.2 关键事实修正（相对 8 月 25 日旧对比文档）

- 旧文档称"无聚合管道"——**不准确**。`doc_pipeline.c`（1372 行）+ `doc_agg.c`（705 行）实现完整 pipeline（match/group/sort/limit/skip/project + DocGroupStage 累加器，:466-706）
- 旧文档称"缺少分析器（中文分词/同义词/词干化）"——**部分不准确**。`doc_fts.c` 提供标准/空白/关键词三套 tokenizer（:196/285/346）+ DocSynonyms 同义词系统（:97-175）。**中文分词（IK/Jieba 等）未确认**——属于标准 tokenizer 之外的扩展；词干化（stemmer）也未确认

## 2. 代码级质量审查

### 2.1 并发正确性

**缺陷 1：第三个模态复刻同一 buggy 自旋读写锁「确认·实现质量缺陷」**

`doc_engine.c:750-770` `g_doc_lockmgr` + `doc_rwlock_t`——与 vector/ts 的 rwlock 是同一份代码的再次复制（readers/writers_waiting/writer_active + 竞态窗口 + 写者饥饿）。`use_lock` 默认 false（:114）。**三个模态复制同一个错误实现**——是工程层面最严重的"未抽出公共并发原语"信号。

### 2.2 崩溃恢复

**缺陷 1：doc_engine 无 WAL 集成「疑似·实现质量缺陷」**

`doc_engine.c` grep `wal_write_/xlog_insert/wal_log_` 零命中；insert 路径预计走 buffer pool 但无 redo log。`doc_inverted.c`（349）倒排索引的崩溃重建未核。`bm25.c`（299）的索引持久化路径未核。

### 2.3 内存安全

**正面证据**：模块拆分清晰，每个 stage 都有匹配的 `_create/_free`（如 DocMatchStage:466/479、DocGroupStage:492/538、DocSortStage:560/604、DocLimitStage:617/628、DocSkipStage:639/650、DocProjectStage:661/686）——命名一致利于审计。

**缺陷 1：未逐个核 stage 错误路径，需运行时 ASAN 验证「疑似·实现质量缺陷」**

Pipeline 阶段多（match/group/sort/limit/skip/project + 累加器），中间态内存释放语义靠 1372 行的精细 free 配对。

### 2.4 错误处理

**缺陷 1：doc_engine_free_results 是唯一 free 入口（:518）——资源回收契约分裂「疑似·实现质量缺陷」**

`doc_engine.c:518` 提供统一 results free，但 doc_pipeline 内部 stage 自己有 `_free` 入口。混合两种所有权模型易产生泄漏或双重释放。需核 doc_query → pipeline 调用的资源所有权链。

### 2.5 算法实现质量

**正面证据 1：BM25 + 倒排索引与 ElasticSearch 同形「确认」**

`bm25.c`（299）+ `doc_inverted.c`（349）——经典 Lucene 风格架构（postings + 词项统计）。

**正面证据 2：JSONPath 解析器 670 行——基础实现但可读「确认」**

`jsonpath.c`（670）含完整语法解析。具体覆盖度（过滤器/切片/递归下降 `..`）需读全文确认——Lucene/MongoDB 的 JSONPath 子集与自研版本的差异需运行时测试套件核对。

**缺陷 1：聚合管道实现规模与 MongoDB 差距大「确认·功能缺失」**

自实现 match/group/sort/limit/skip/project（6 阶段）vs MongoDB 30+ 阶段（含 $lookup/$facet/$bucket/$graphLookup/$unwind/$replaceRoot 等）。功能面窄但核心流可用。

**缺陷 2：缺中文分词与语言分析「确认·功能缺失」**

`doc_fts.c` 三套 tokenizer（standard/whitespace/keyword）——按空格/标点切分，对中文（CJK）默认按字符切分，未实现 IK/Jieba/MMSeg 等词典分词。中文场景召回率与 ElasticSearch IK/ES-IK 差距大。

**缺陷 3：缺词干化（stemming）「疑似·功能缺失」**

英文搜索场景需 Snowball/Porter stemmer；`doc_fts.c` 未读到 stemmer 实现。

### 2.6 API 设计

**正面证据**：聚合管道与全文搜索分文件解耦（pipeline/fts），Hybrid 检索走 `doc_vector.c`（584 行混合稠密向量），形成 ES-style 完整 surface。

**缺陷 1：API 入口分散，缺乏统一 DocumentQuery 抽象「疑似·API 设计」**

match/group/sort 等 stage 各自 create + 自由组装，缺少"按 JSON spec 一键构建 pipeline"的工厂函数（如 MongoDB 接受 `{ $match: {...}, $group: {...} }` 一次性描述）。

## 3. 业界标杆对比

| 维度 | 自实现 | MongoDB 7.0 | Elasticsearch 8.x | CouchDB 3.x |
|------|--------|-------------|-------------------|-------------|
| 全文 BM25 | ✓ (bm25.c) | 依赖 Atlas Search | ✓ 完整 Lucene | 基础 Lucene |
| 同义词 | ✓ (doc_fts.c:97-175) | ✓ Atlas | ✓ | 基础 |
| 中文分词 | ✗ 标准 tokenizer | 依赖 Atlas 分词插件 | IK/pinyin 插件 | 基础 |
| 词干化 | 未确认 | ✓ Atlas | ✓ Snowball/KStem | 基础 |
| 聚合管道 | ✓ 6 阶段 | 30+ 阶段 | ✓ Aggregations | Map-Reduce |
| JSONPath | ✓ 670 行 | 完整 BSON | DSL | Mango Selector |
| 倒排索引 | ✓ (doc_inverted) | ✓ | ✓ 多层 | ✓ |
| 嵌套文档 | ✓ (doc_nested.c 861) | ✓ | ✓ | ✓ |
| Change Streams | ✗ | ✓ | Ingest Pipeline | CRDT 同步 |
| 分布式 | ✗ | Sharded Cluster | Index 分片 + 副本 | Multi-master |

## 4. 差距矩阵

| 维度 | 评分 | 关键证据 |
|------|------|---------|
| 并发正确性 | 3 | 第三个模态复刻 buggy 自旋锁 `doc_engine.c:750-770`；默认无锁 `:114` |
| 崩溃恢复 | 3 | 无 WAL 集成；倒排索引崩溃重建待核 |
| 内存安全 | 5 | stage 命名一致（`_create/_free` 配对）；所有权分裂待核 |
| 错误处理 | 5 | pipeline 错误处理需运行时验证 |
| 算法实现质量 | 5 | BM25/倒排与 ES 同形；缺中文分词/词干化；管道 6 阶段 vs MongoDB 30+ |
| API 设计 | 5 | 全文/聚合/混合检索分文件解耦好；缺统一 spec 工厂 |

**实现质量缺陷清单（3 项确认 + 3 项疑似）**：
1. 复刻 vector/ts 的 buggy 自旋读写锁（并发）
2. 无 WAL 集成（疑似，崩溃）
3. doc_engine_free_results vs stage 自由 mixed 所有权（疑似）
4. 缺中文分词与词干化（功能缺失）
5. 聚合管道 6 阶段 vs MongoDB 30+（功能缺失）
6. 缺统一 spec 工厂（疑似 API 设计）

## 5. 改进优先级

| 优先级 | 项目 | 分类 | 工作量 |
|--------|------|------|--------|
| P0 | 抽取公共并发原语库（pthread_rwlock 包装）——三模态同时修 | 实现质量缺陷 | M |
| P0 | doc_engine 接入 WAL | 实现质量缺陷 | M |
| P1 | IK/Jieba 中文分词（业界有 BSD/Apache 词典） | 功能缺失 | M |
| P1 | Snowball 词干化（port libstemmer_c 即可） | 功能缺失 | S |
| P2 | 聚合管道扩展（$lookup/$unwind/$bucket/$facet/$graphLookup） | 功能缺失 | L |
| P2 | MongoDB-style 一键 spec → pipeline 工厂 | API | M |
