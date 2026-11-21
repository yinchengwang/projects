> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# OpenSpec 规格组织

> **本目录是 OpenSpec 治理的根**：所有归档后的规格（spec）按双轨架构分类。
>
> **变更中的规格**：见 `../../changes/<change-name>/specs/<capability>/spec.md`

## 子目录结构

- **`engineering/`**（59 个）：工程层规格——描述 `engineering/src/<子系统>/` 或 `engineering/apps/<模块>/` 的能力
  - 数据库子系统：`db-storage-engine/`, `db-redis/`, `db-index-query/` 等
  - 向量索引：`vector-diskann/`, `vector-hybrid-search/`, `vdb-core-index-algorithms/` 等
  - 并发/事务：`mvcc/`, `raft-ha/`, `sharding/`, `distributed-transaction/` 等
  - SQL 子系统：`sql-parser/`, `sql-planner/`, `sql-executor/`
  - 独立应用：`apps-games-2048/`, `apps-common-menu/` 等
  - CLI/API：`rest-api/`, `python-sdk/`, `jdbc-driver/`

- **学习层规格已迁出本仓**（2026-10-25）：原 35 个学习层规格现由**学习仓**
  `yinchengwang/learning` 持有 —— `learning/openspec/specs/learning/`（35 个，
  含学习路线图 `adaptive-learning-roadmap/`、Linux 知识栈 `linux-kernel-subsystems/`、
  Quiz 测验系统 `quiz-system-navigation/`、Mermaid2Excalidraw 与笔记渲染等）
  另有 12 个纯学习向轮次规格 `r{12b,19,23,24,26,29,30,31,32,33,37,38}-*/` 一并迁入。
  本仓只保留**工程层**规格与 20 个 `r*` 轮次规格。

## 命名约定

- spec 名称 kebab-case，与 `src/` 目录树同构：`db-storage-engine` 对应 `engineering/src/db/storage/`
- 每个 spec 含 1 个 `spec.md`（OpenSpec 声明式格式：Purpose + Requirements + Scenarios）
- 单个 spec.md 不少于 30 行（少于视为空泛，应通过 change 流程丰富）

## 添加新规格流程

1. 在 `openspec/changes/<change-name>/specs/<capability>/spec.md` 起草新规格
2. 归档时（`openspec/changes/archive/<date>-<change>/`）由 Claude 自动移到 `openspec/specs/engineering/<capability>/spec.md`（学习层规格归学习仓，本仓不再产出）
3. 使用 OpenSpec 模板：Purpose / Requirements / Scenario 三段式

## 删除规格流程

1. 在 change/archive 中标记为 REMOVED
2. `git rm -r openspec/specs/<capability>/`
3. 在本 README 末尾 ARCHIVED 段添加删除原因

## ARCHIVED（已移除）

| 规格 | 移除时间 | 原因 |
|---|---|---|
| `build-my-db-art-index` | 2026-07-10 | S1 之前的 mini-DB 项目已迁为 `engineering/src/db/storage/art.c`，原路径已废弃 |
| `build-my-db-kv-api` | 2026-07-10 | 同上 |
| `build-my-db-storage-engine` | 2026-07-10 | 同上 |
| `build-my-db-transaction-wal` | 2026-07-10 | 同上 |
| `dl-content-classification` | 2026-07-10 | 与代码架构脱节，应为笔记而非 OpenSpec 规格 |
| `dl-content-generation` | 2026-07-10 | 同上 |
| `dl-content-integration` | 2026-07-10 | 同上 |
| `practice-curriculum` | 2026-07-10 | 被 S2/S3 (学习层建设) 取代 |

## 统计

- 工程层规格（`engineering/`）：59 个
- 轮次规格（顶层 `r*` 目录）：20 个
- 单文件规格（`learning-card-rigor/`、`kanban-statuses-json/`）：2 个
- 学习层规格：**已迁出本仓** → 见 learning 仓 `openspec/specs/`（47 个 = 35 个 `learning/` + 12 个 `r*`）
- 已归档：8 个
- 当前本仓总共可见：**81 个**
