# Gap-Analysis 路线图 — 任务清单（**P0/P1 部分已失效**）

> **For agentic workers:** 本 OPSX 变更是**路线图跟踪 + 追溯已交付物**性质，**不实施任何代码**。
> 每个 P0 / P1 / P2 实施时独立开子 OPSX 变更（如 `p1b-checkpoint-manager`），本变更仅做 backlog 登记与进度跟踪。

---

## ⚠️ 阶段 2.1 / 2.3 / 2.4 已失效（2026-11-18 裁决）

**这批 backlog 的目标载体不存在了。**

13 篇 gap-analysis 子报告（2026-10-16）全部以 `engineering/src/db`
（PostgreSQL 风格的 CMake 内核）为研究对象。而 2026-11-16 的路线翻转
（`docs/superpowers/plans/2026-11-16-db-capability-matrix.md`）已把它
**冻结为历史参考**，唯一主核换成 `engine/db`（自带 Makefile 的多模态内核）。

两者不是同一套代码：存储格式、事务实现、执行器、API 全不兼容。
所以下面这些 P0/P1/P2 **不可能"在 src/db 上实施"**，逐条勾掉也没有意义。

处置：

| 阶段 | 原内容 | 新状态 |
|---|---|---|
| 阶段 1（追溯已交付物） | 13 篇子报告 commit 链 | ✅ 仍然有效（历史记录） |
| 阶段 2.1（25 项 P0 路线图） | 指向 src/db | ❌ **失效** —— 不实施 |
| 阶段 2.2.3（pre-existing 修复） | vdb_e2e 两个失败例 | ⚠️ **见下** |
| 阶段 2.3.1 / 2.4.1（P1/P2 登记） | 指向 src/db | ❌ **失效** —— 不实施 |
| 阶段 3.1 / 3.2 / 4.1 | 进度跟踪 / 归档 | ❌ 随 2.1 一起失效 |

**P0/P1/P2 里的能力本身不会消失**：它们作为"主核缺什么"的对比参考仍然有用，
但要实施只能在 `engine/db` 上重新设计，不能照抄 src/db 的路线。
将来若要在主核补能力，按 `docs/superpowers/plans/2026-11-16-db-capability-matrix.md`
的 B3 流程（一次一个垂直切片）走，不回到这份 backlog。

**2.2.3 是唯一还有实际意义的项**：它指的是 `vdb_e2e_test` 里
SearchVectors 的排序方向断言与 DropCollection 的失败 —— 那是 src/db 的测试，
随内核一起冻结，**同样不修**。若这些语义在主核需要，另开主核任务。

**为什么文件还留在这里**：这是"当时判断缺什么"的历史记录，
删除会丢掉那 13 篇子报告的索引。归档到 `openspec/archived/` 也不合适 ——
归档意味着"已完成"，而这批 P0 从未实施。

---

## 阶段 1：追溯已交付物（已完，无工作）

> 13 篇子报告 + 总报告 + README 链接已 commit 到 main 分支，本阶段仅做文档化。

- [x] **Task 1.1** — 13 篇子报告 commit 链追溯登记
  - 13 篇：relational / key-value / document / columnar / time-series / search / vector / graph / stream / embedded / extension / rag / agent
  - 第一篇：`13b5fbd2d` relational (13:54)
  - 末篇：`d583886bf` agent (16:09)
  - 验证：`git log --reverse -- docs/gap-analysis/*.md` 全部 13 行 + 总报告 1 行 + README 链接 1 行 + 清理 1 行

- [x] **Task 1.2** — 总报告 `docs/gap-analysis/README.md`（58KB / 6 节）
  - commit: `e5fd7f029`
  - 6 节：§1 摘要 / §2 差距诊断 / §3 优势亮点 / §4 RAG+Agent / §5 6 月路线图 / §6 风险与陷阱
  - 验收：5-7 条差距诊断 / 3-5 条优势 / 6 月路线图 / 5-7 条风险 — 全部已勾选（README §8 验收清单 7/7 通过）

- [x] **Task 1.3** — README 直达链接追加到 wiki
  - commit: `a31496645`（三个 wiki README 末尾追加 gap-analysis 直达链接）
  - `.gitkeep` 清理：`b333b52fa`

- [x] **Task 1.4** — 关联已合入的 P1-A
  - commit: `3939cc03c`（p1a-vector-wal-complete 合入 main）
  - 首个路线图落地：Vector WAL 完整化 4 个独立测试 + vdb_e2e WALRecovery/CheckpointTest 全绿

## 阶段 2：路线图 backlog 登记

> 25 项 P0 + 30 项 P1 + 10 项 P2 来自 `docs/gap-analysis/README.md` §5.2-5.7 + §5.10。
> 本阶段只登记，**不实施**。实施时开子 OPSX 变更。

### 阶段 2.1：25 项 P0 路线图（按月排程，§5.2-5.7）

> 投入合计：约 80-100 人周 / 6 个月 1 人精排 + 适当加班
> 来源：每个 P0 标注子报告 §7 行号引用

- [ ] **Task 2.1.1 — 第 1 月 P0（基础设施集中：关系 + KV + 文档）**
  - 子报告：relational / key-value / document §7
  - P0 数量：~5 项
  - 投入：~16-20 人周
  - 实施：开 `p1b-infrastructure-batch` 子 OPSX（按月打包）

- [ ] **Task 2.1.2 — 第 2 月 P0（核心协议 + 优化器）**
  - 子报告：relational / sql / key-value §7
  - P0 数量：~4 项
  - 投入：~14-18 人周

- [ ] **Task 2.1.3 — 第 3 月 P0（向量 + 文档协议 + 列存引擎家族）**
  - 子报告：vector / document / columnar §7
  - P0 数量：~4 项
  - 投入：~14-18 人周

- [ ] **Task 2.1.4 — 第 4 月 P0（向量算法 + 搜索 NRT + RAG 编排协议）**
  - 子报告：vector / search / rag §7
  - P0 数量：~4 项
  - 投入：~14-18 人周

- [ ] **Task 2.1.5 — 第 5 月 P0（RAG 框架 + Agent 编排）**
  - 子报告：rag / agent §7
  - P0 数量：~4 项
  - 投入：~12-16 人周

- [ ] **Task 2.1.6 — 第 6 月 P0（生态适配 + 商业观测）**
  - 子报告：rag / agent / extension §7
  - P0 数量：~4 项
  - 投入：~10-14 人周

### 阶段 2.2：候选 P0 子 OPSX 变更（高优先级，已识别）

> 来自 P1-A 验收 result.md + 本路线图 §5.2-5.3

- [x] **Task 2.2.1 — `p1b-checkpoint-manager`（Vector CheckpointManager 完善）**
  - 背景：vdb_chaos_test 3 个失败（IndexRebuildAfterKill / MultipleCrashRecovery / ConcurrentWriteWithCrash）很可能与 CheckpointManager 行为相关
  - 范围：~2 周 / `engineering/src/db/storage/vector/vector_wal.c` + checkpoint_lsn 状态机
  - 验收：vdb_chaos 3 失败用例转绿 + 1 个新测试覆盖 checkpoint_lsn 起点
  - **已完成（2026-10-18）**
  - 实施分支：`feature/p1b-checkpoint-manager`（HEAD `340fea0f1`）
  - 11 个 commit：`dca41ba59` / `b3ac8e8fa` / `5dbc3f708` / `40fb3a76` / `176d58a06` / `6a5d42ec0` / `8fb89cd4b` / `3786e2e3f` / `fe6d6ddf1` / `8306b323a` / `f29121681` / `340fea0f1`
  - 最终测试：vdb_chaos 6/6 + vdb_checkpoint 6/6 + vdb_wal_recovery 4/4 + vdb_e2e 11/13（含 2 pre-existing）+ vdb_stress 5/5 = **15/15 关键路径全绿**
  - 详细交付报告：`openspec/changes/p1b-checkpoint-manager/result.md`

- [x] **Task 2.2.2 — `p1c-chaos-recovery`（Chaos 恢复加固）**
  - 背景：vdb_chaos 3 失败独立加固（间隔参数化 + 5 chaos_e2e + 3 短 interval 子用例）
  - 范围：~3 周 / CheckpointManager 间隔参数化 / kill-during-checkpoint / kill-during-replay / partial-write / 短 interval stress / 并发 save+crash
  - 验收：chaos_e2e 5/5 + vdb_chaos 9/9 + vdb_checkpoint 6/6 + vdb_wal_recovery 4/4 + vdb_stress 5/5 = **24/24 关键路径全绿**
  - **已完成（2026-10-19）**
  - 实施分支：`feature/p1c-chaos-recovery`
  - commit 链（7 个）：`8dcd1e1e9` / `2681398ef` / `fd2a1a86e` / `0a12c55c5` / `769127e45` / `1bb7c936d` / `51f99ec0c`
  - 详细交付报告：`openspec/changes/p1c-chaos-recovery/result.md`

- [ ] **Task 2.2.3 — pre-existing 修复（vdb_e2e SearchVectors / DropCollection）**
  - 背景：与 WAL 无关，单独登记
  - 范围：~3-5 天
  - 验收：vdb_e2e 13/13 全绿

### 阶段 2.3：30 项 P1 backlog（推迟到下季度，§5.10）

- [ ] **Task 2.3.1 — P1 推迟项登记（30 项 / 80000-120000 行 / 40-60 人周）**
  - 来源：`docs/gap-analysis/README.md` §5.10
  - 暂不实施，按子报告分组：
    - relational P1：~5 项
    - key-value P1：~3 项
    - document P1：~3 项
    - columnar P1：~3 项
    - time-series P1：~2 项
    - search P1：~2 项
    - vector P1：~3 项
    - graph P1：~2 项
    - stream P1：~2 项
    - embedded P1：~2 项
    - extension P1：~1 项
    - rag P1：~1 项
    - agent P1：~1 项

### 阶段 2.4：10 项 P2 backlog（暂放，§5.10 末尾）

- [ ] **Task 2.4.1 — P2 暂放项登记（10 项）**
  - 来源：`docs/gap-analysis/README.md` §5.10
  - 暂不实施，长期 backlog

## 阶段 3：进度跟踪机制

> 本变更作为路线图 tracker，**长期存在**直到所有 P0 完成（或被推迟/取消）

- [ ] **Task 3.1** — 每月底更新 backlog 进度
  - 哪些 P0 已开子 OPSX 变更
  - 哪些 P0 已完成（含 commit hash）
  - 哪些 P0 推迟到下月 / 下季度
  - 更新本 tasks.md 的 `[ ]` → `[x]` 状态

- [ ] **Task 3.2** — 季度回顾
  - P0 完成率统计
  - P1 重新评估
  - 路线图调整（新增 / 删除 / 降级）

## 阶段 4：归档

- [ ] **Task 4.1** — 当所有 P0 完成或路线图整体调整时，本变更归档到 `openspec/changes/archive/`
  - 归档要求：所有 P0 任务有最终状态（done / dropped / deferred）
  - 归档 commit：单独 1 个 commit 移动目录

---

## 完成状态

- [x] Task 1.1-1.4：追溯已交付物（13 篇子报告 + 总报告 + 链接 + P1-A 关联）
- [x] Task 2.2.1：p1b-checkpoint-manager（Vector CheckpointManager 完善）— 2026-10-18 完成，HEAD `340fea0f1`
- [ ] Task 2.1.1-2.1.6：25 项 P0 路线图登记（待开子 OPSX 实施）
- [x] Task 2.2.2：p1c-chaos-recovery（2026-10-19 完成）
- [ ] Task 2.2.3：pre-existing 修复（vdb_e2e SearchVectors / DropCollection）
- [ ] Task 2.3.1：30 项 P1 backlog
- [ ] Task 2.4.1：10 项 P2 backlog
- [ ] Task 3.1-3.2：进度跟踪
- [ ] Task 4.1：归档

## 关联 OPSX 变更

- **已完成：** `p1a-vector-wal-complete`（首个落地，main `3939cc03c`）
- **已完成：** `p1b-checkpoint-manager`（Vector CheckpointManager 完善，2026-10-18，feature 分支 HEAD `03eb3f91e`）
- **已完成：** `p1c-chaos-recovery`（Chaos 恢复加固，2026-10-19，feature 分支 HEAD `51f99ec0c`）
- **候选 next：** `p1d-replay-refactor` / `p1e-mkdir-crossplatform` / `p1f-os-level-chaos`（按 P0 路线图第 1-2 月）
