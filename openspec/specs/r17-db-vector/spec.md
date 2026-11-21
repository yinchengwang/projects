> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# r17-db-vector Specification

## Purpose
R17 DB 向量数据库与 SQLite 学习卡闭环规范。

## Requirements

### Requirement: R17 9 张卡必须通过四要素审计方可推 done

系统 SHALL 仅在一张 R17 卡从 `todo` 推到 `done` 时，在 `statuses.json` 与 `r17-progress.md` 中**同时满足**：

1. **scaffold 产物**：commit SHALL 含可编译运行的 C 源（`learning/scaffold/db/<card-id>/main.c` + `Makefile`）
2. **README.md**：含卡片简介和运行说明
3. **NOTES.md 工程对照**：必须含 ≥100 中文字符的"工程对照"段落，明确指出本卡知识在工程侧的具体复用位置（带文件路径或行号引用）
4. **r17-progress.md 行**：与 `statuses.json` status 严格同步

### Requirement: 9 张 DB 向量卡清单

| 卡 ID | 主题 | scaffold 目录 |
|-------|------|---------------|
| vector_basic | 向量基础 | `learning/scaffold/db/vector_basic/` |
| ivf_variants | IVF 变体 | `learning/scaffold/db/ivf_variants/` |
| hnsw | HNSW 图索引 | `learning/scaffold/db/hnsw/` |
| graph_index | 图索引 | `learning/scaffold/db/graph_index/` |
| pq_quant | 乘积量化 | `learning/scaffold/db/pq_quant/` |
| quantization | 量化技术 | `learning/scaffold/db/quantization/` |
| scalar_quant | 标量量化 | `learning/scaffold/db/scalar_quant/` |
| diskann | DiskANN | `learning/scaffold/db/diskann/` |
| sqlite_arch | SQLite 向量扩展 | `learning/scaffold/db/sqlite_arch/` |

### Requirement: 工程对照 ≥100 字

每张卡的 `NOTES.md` 必须含 ≥100 中文字符的"工程对照"段落，对照 `engineering/src/db/` 或 `engineering/src/algo-prod/` 下的实际代码文件。

### Requirement: 编译验证

每张卡的 scaffold 必须通过 `gcc -std=c11 main.c -o main && ./main` 验证。
