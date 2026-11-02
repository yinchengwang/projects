# v1.3 执行方案与计划 · 模态 4：全文检索（倒排索引 + segment 合并）

> 状态：计划已生成，未执行。执行记录见 §3。
> 前置：v1.2（内核收尾）已完成。
> 总览背景：`PLAN-modality-expansion.md` §1。

---

## 一、方案

### 1.0 这一版要证明什么

DESIGN.md 目标第 3 条点名的对标之一是 **Lucene 的 segment**。这一版学的是：

> **不变文件 + 后台合并**。索引 segment 写完就不可变；删除只打标记；
> 后台把小 segment 合并成大的（合并时才物理丢弃被删文档）。
> ——blob 的对象不可变、v1.5 时序的分区压缩、Lance 的 fragment，全是这一个思想。

判据两条：

1. **合并是透明的**：任意时刻触发合并，检索结果前后全等（用户视角合并不存在）。
2. **合并的代价是可解释的**：10 万文档分批插入，合并总 IO 呈
   `O(N × log_f N)` 量级（f = 合并因子），放大系数可测。

### 1.1 范围

**做：**

1. 英文分词器（小写化 + 非字母数字切分）。
2. 倒排索引：term 字典（有序数组 + 二分）+ posting list（`(doc_id, tf)` 有序对，delta 编码）。
3. BM25 评分（k1=1.2, b=0.75）；TF-IDF 作对照实现。
4. segment 物理格式（不可变）+ Tiered 合并 + tombstone 删除。
5. 虚拟表 `ft_docs`（文档元数据）+ 专用检索 API `db_ft_search`。
6. WAL 保护（`WAL_PAGEDATA` 整页镜像，第 4 个模态复用）。

**不做（明确划掉）：**

| 项 | 为什么 |
|---|---|
| 中文分词 | 分词器是独立工程（词典/统计模型），对理解倒排零增量 |
| FST / 前缀压缩的 term 字典 | 字典压缩是内存优化；有序数组 + 二分已能学 posting 机制 |
| 位置索引 / 短语查询 / 高亮 | 预留接口（posting 项留 pos 字段位），机制不做 |
| 近实时搜索（NRT / 内存 segment） | Lucene 的 NRT 是工程甜点，不是核心机制 |
| 多字段加权 schema | 单字段已覆盖全部评分机制 |

### 1.2 关键判决：检索语义走专用 API，不进 SQL 谓词

**这是本版最重要的边界判决**（对齐 v1.0 的"距离排序不进 SQL"先例）：

| 路径 | 内容 | 判决 |
|---|---|---|
| `db_ft_search(conn, term, top_k)` | 专用 API：返回 `(doc_id, score)` 按 BM25 排序 | **主路径** |
| `ft_docs` 虚拟表 | `(doc_id, doc_len, nterms, sealed)` —— 标量列，`doc_id` 等值/范围下推 | 元数据路径 |
| `match(col, 'term')` 进 SQL 谓词 | 需要 `PD_MATCH`、相关性排序键、执行器拥有全局统计（idf/avgdl） | **否决** |

**否决的理由**（写清楚，防止将来心血来潮去改）：

1. `pushdown.h` 的 `Predicate` 是 `(col_idx, op, DbValue)` —— 单值标量比较。
   相关性评分依赖**全局统计**（idf 随语料变、avgdl 随 segment 合并变），
   执行器根本无法独立重求值 —— **"下推给超集、执行器重判"的契约无法成立**。
2. 为单一模态改三个已验收模态共用的公共契约，违反 v0.9 的教训
   （接口是照着调用方的需要写的 —— 执行器不需要）。
3. 同性质先例已有三个：graph 遍历、vector knn、geo 窗口（v1.4）。
   **模态特有语义不进标量谓词系统** —— 这条边界在 v1.4 结束后写进 DESIGN 复盘。

### 1.3 segment 的物理形态

```
一个 segment = 三种页的区间（写完不可变）：

PT_FT_HEAD（段头页，1 页）：
  seg_id / doc_count / 创建 LSN / avgdl / 段内 max doc_id
  dict_page_start / dict_page_count / post_page_start / post_page_count

PT_FT_DICT（字典页，若干）：
  term 有序数组：[term_len, term_bytes, df, post_ptr]
  跨页二分查找（页内有序 + 页间有序）

PT_FT_POST（posting 页，若干）：
  (doc_id_delta, tf) 有序对数组，doc_id 用 delta 编码
  一个 term 的 posting 可能跨页链（页头带 next）
```

**为什么 term 用有序数组而不是哈希**：检索要"按字典序批量扫"（前缀查询的将来），
且**合并 = 多路归并**（有序对有序，天然归并）—— 哈希两头都不占。

**tombstone（删除标记）**：按 `seg_id` 组织的 doc_id 位图；
内存持有 + WAL 逐条追加（`WAL_FT_TOMB` 新记录类型，内容仅 `seg_id + doc_id`）；
重启时重放重建。**查询时过滤，合并时物理丢弃**。

### 1.4 评分与合并

**BM25**（k1=1.2, b=0.75）：

```
idf(t)   = ln(1 + (N - df + 0.5) / (df + 0.5))     ← Lucene 式，非教科书式
score    = Σ_t idf(t) · tf·(k1+1) / (tf + k1·(1 - b + b·dl/avgdl))
```

**为什么选 Lucene 式 idf**：教科书式的 idf 在 df=0 时为负、单文档语料时除零；
Lucene 式恒正且平滑 —— **抄业界实现，不抄教科书**（本项目的既定方针）。
TF-IDF 作为对照实现保留（验收 A2 用：两种评分对同一语料的排序差异要**可解释** ——
主要来自 BM25 的文档长度归一）。

**跨 segment 的统计合并**：`N`（总文档数）和 `avgdl` 是全局量 ——
检索时实时聚合各段头（段数 ≤ 阈值 8，聚合代价可忽略）。
**这正是合并的动机之一**：段越少，聚合越便宜（与 A4 的 IO 论证同向）。

**Tiered 合并**：

```
段数 > 8 时：挑"文档数之和最小"的相邻若干段合并成一个
  合并 = 字典多路归并（term 对齐）+ posting 拼接（重算 delta）+
         tombstone 过滤（被删文档物理丢弃）+ 重建段头
旧段页 → 回收（freelist，页号空间共享）
```

### 1.5 验收

| # | 验收项 | 怎么验 |
|---|---|---|
| A1 | **检索正确性**：千文档语料，`db_ft_search` 的 top-k 与线性扫描 BM25 全等（含分数） | 单元测试 |
| A2 | **评分可解释**：BM25 vs TF-IDF 对同一语料的排序差异，逐例解释（长度归一项） | 单元测试 |
| A3 | **删除语义**：删除后立即不出现在结果（tombstone 生效）；合并后仍不出现（物理丢弃）且**段页数下降** | 集成测试 |
| A4 | **合并代价**：10 万文档分批插入，合并总 IO ≈ `c·N·log_f N`（c 实测，量级论证） | bench |
| A5 | **IO 可证**：单 term 检索页读 = 字典二分页数 + 该 term 的 posting 页数（与总文档量无关） | `db_ft_page_reads()` |
| A6 | **合并透明**：随机时刻触发合并，前后检索结果全等 | 集成测试 |
| A7 | 崩溃恢复：segment 页 + tombstone 重放后检索一致 | 集成测试（test_crash 模式） |
| A8 | 执行器零改动（`grep ft_\|fulltext src/query/` 零命中）；旧验收全绿 | 机械检查 |

### 1.6 风险清单

| # | 风险 | 应对 |
|---|---|---|
| R1 | **BM25 公式抄错**（idf 变体多） | TF-IDF + 线性 BM25 双对照；公式出处写进代码注释 |
| R2 | **合并归并 bug**（term 对齐/delta 重算错） | 有序数组参考实现 diff（v0.3 的 B+tree 方法复用） |
| R3 | **tombstone 丢失**（WAL 重放漏） | 删除后崩溃恢复测试（A7 覆盖） |
| R4 | 分词边角（连字符/数字/混合） | 边界用例表（`state-of-the-art` → 4 词）写进单测 |
| R5 | 反向验证编译不过 → 跑旧二进制 | 老规矩 |

---

## 二、执行计划

### 步骤 1 · 分词器 + 评分器（纯内存）

1. `src/modalities/ft/tokenizer.{h,c}` + `bm25.{h,c}`。
2. 线性扫描评分（无索引）—— 这同时是验收的**基准实现**。

**验证**：tokenizer 边界用例 + BM25 手算对照。

### 步骤 2 · 单 segment 写读

1. 段格式（三种页）+ 构建（一批文档 → 一个 segment）+ 单段检索。

**验证**：单段检索 == 线性扫描（A1 的单段版）。

### 步骤 3 · 多 segment + tombstone + Tiered 合并

1. 多段管理、跨段统计聚合、tombstone、合并。

**验证**：A1（多段）/ A3 / A6。

### 步骤 4 · SPI 接线

1. `spi/ft_am.{h,c}`：`ft_docs` 虚拟表（`visible` 恒真 —— 对齐 blob/graph 的语句级语义）。
2. `db_ft.{h,c}`：`db_ft_index / search / delete / merge / page_reads`，全部 latch（硬规则 8）。
3. REPL：`FT INDEX / SEARCH`。

**验证**：A8 + 旧验收全绿。

### 步骤 5 · 崩溃恢复 + 基准

A7（含 tombstone 重放）+ A4 / A5（`bench/ft_bench.c`）。

### 步骤 6 · 反向验证 + 文档

1. 注入 1：合并时**不过滤 tombstone** → A3 的"物理丢弃"断言红。
2. 注入 2：检索**跳过字典二分的最后一页** → A1 红（漏 term）。
3. 文档四件套 + 记忆。

---

## 三、执行记录

（执行后填）
