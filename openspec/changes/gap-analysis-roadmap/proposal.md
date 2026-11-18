# Gap-Analysis 路线图 — 变更提案（P0/P1 路线图**已失效**）

**变更 ID：** gap-analysis-roadmap
**日期：** 2026-10-16
**状态（2026-11-18 更新）：** ⚠️ **路线图部分失效** —— 目标载体 `engineering/src/db`
已于 2026-11-16 冻结；13 篇子报告的研究对象不再是主核。
**仍有效的部分**：13 篇子报告作为"能力差距"的历史记录 + 主核补能力时的对照参考。
**处置与证据**：见 `tasks.md` 顶部的裁决段。
**关联文档：**
- 设计：`docs/superpowers/specs/2026-10-16-gap-analysis-design.md`
- 总报告：`docs/gap-analysis/README.md`（58KB / 6 节 / 25 P0 路线图）
- 旧版纵向对照：`docs/gap-analysis/2026-08-29-multimodal-db-gap-analysis.md`

---

## 背景

13 篇 gap-analysis 子报告（relational / key-value / document / columnar / time-series / search / vector / graph / stream / embedded / extension / rag / agent）已于 2026-10-16 直接 commit 到 main 分支（无 OPSX 跟踪），汇总后形成 6 节总报告与 6 个月（24 周）路线图。

**13 篇 commit 链（已在 main 上）**：

| commit | 子报告 |
|--------|--------|
| `13b5fbd2d` | relational |
| `c3f918113` | key-value |
| `2df7bc191` | document |
| `d1fe10a0d` | columnar |
| `ebdc1e40d` | time-series |
| `20e6b6ac8` | search |
| `d8beb1f6f` | vector |
| `62beb6a66` | graph |
| `20c012267` | stream |
| `6ac390171` | embedded |
| `f1d149a31` | extension |
| `1ad9d8989` | rag |
| `d583886bf` | agent |
| `e5fd7f029` | 总报告（README.md）|
| `a31496645` | 三个 wiki README 末尾追加 gap-analysis 直达链接 |
| `b333b52fa` | 移除 gap-analysis 目录 .gitkeep 占位文件 |

**总报告规模**：
- 13 子报告合计 13×300-500 行 ≈ 5000 行
- 总报告 58KB / 6 节
- 25 项 P0 排程（§5.2-5.7）
- 30 项 P1 推迟到下季度（§5.10）
- 10 项 P2 暂放

## What Changes

### 追溯已交付物（无新增代码，文档化已完成工作）
1. 13 篇子报告 + 总报告 + README 链接统一归入本 OPSX 变更
2. 25 项 P0 + 30 项 P1 + 10 项 P2 路线图作为 backlog 纳入 OPSX 跟踪

### 路线图实施组织
- **总 P0 工作量**：约 164 人周（26-32 个月单人或 8 个月 2 人或 4 个月 3 人）
- **6 个月 1 人**只能完成 P0 约 25-30%
- **本变更追踪路线图**，但每个 P0 实施时**单独开子 OPSX 变更**（避免 1 个变更膨胀失控）
- 已在 main 上完成 P1-A（Vector WAL 完整化）作为路线图首个落地（`p1a-vector-wal-complete`）

### 必做项
1. 建 OPSX 骨架（proposal.md / design.md / tasks.md / notes/）
2. 文档化 13 篇子报告已交付物（commit hash + 路径 + 行数）
3. 文档化 25 项 P0 路线图（§5.2-5.7 排程 + §5.8 P0 索引）
4. 文档化 30 项 P1 backlog + 10 项 P2 backlog
5. 提供 P0 实施优先级判据（每开 1 个 P0 子 OPSX 前先看本变更 §3）

### Non-Goals
- 不在本变更实施任何 P0/P1/P2 实际代码（每个实施开新 OPSX 变更）
- 不回退 13 篇已 commit 的工作（OPSX 追溯 + OPSX-on-merged-commits 模式）
- 不修改 13 篇子报告内容（13 篇是只读交付物，修订走新 OPSX 变更）

## 验收

- [ ] OPSX 骨架建立（proposal.md / design.md / tasks.md）
- [ ] 13 篇子报告 commit 链全部列入追溯表
- [ ] 25 项 P0 路线图任务清单生成（每项指向对应子报告 §7）
- [ ] 30 项 P1 + 10 项 P2 backlog 任务清单生成
- [ ] 与 P1-A（已合入 main）关联：标记 P1-A 为本路线图的首个落地

## 关联 OPSX 变更

- **已完成：** `p1a-vector-wal-complete`（P1-A，路线图首个落地，6 月合入 main）
- **后续 P0 候选：**
  - `p1b-checkpoint-manager`（Vector CheckpointManager 完善，~2 周 / vdb_chaos 3 失败修复）
  - `p1c-chaos-recovery`（Chaos 恢复，~3 周 / vdb_chaos 3 失败加固）
  - 各模态 P0 优化（按子报告优先级 6 个月排程）
