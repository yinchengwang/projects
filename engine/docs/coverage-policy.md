> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 工程轨道覆盖率策略（Engineering Coverage Policy）

> 适用范围：`engine/` 轨道（含 `engine/src/`, `engine/apps/`, `engine/rag/`, `engine/sdk/`）。学习轨已切出为独立仓 `yinchengwang/learning`，不在本仓范围内。

## 目标

- **总体目标**：核心模块（db, algo）lines 覆盖率 ≥ 80%
- **最低目标**：所有工程模块 lines 覆盖率 ≥ 50%（低于此进入警告）
- **持续追踪**：每次 PR 自动对比基线，下降 >2% 触发 PR comment 警告

## 工具链

| 工具 | 用途 | 版本 |
|---|---|---|
| gcc | 编译 + 内置 gcov | 任意 |
| lcov | 收集 .gcno/.gcda → .info | 2.x |
| genhtml | .info → HTML | lcov 自带 |

CI 上 Ubuntu 22.04 + apt 安装 lcov。本地 Windows 不强制（验证以 CI 为准）。

## 模块划分

按 `engine/src/<lib>/` 一级目录：

| 模块 | 路径 | 优先级 |
|---|---|---|
| db | `engine/src/db/` | 核心，必须 ≥ 80% |
| algo | `engine/src/algo/` | 核心，必须 ≥ 80% |
| redis | `engine/src/redis/` | 工具，≥ 50% |
| cpp | `engine/src/cpp/` | 演示，≥ 50% |

`engine/apps/`、`engine/rag/`、`engine/sdk/` 不计入核心覆盖率统计（应用层代码覆盖率意义有限）。

## 基线管理

- 基线文件：`engine/docs/coverage-baseline.json`
- 更新规则：**仅当覆盖率 ≥ 现有基线时更新**（避免基线倒退）
- 更新时机：CI coverage job 跑完后人工 review + commit（自动化更新留待后续 change）

## 报警规则

| 触发条件 | 标记 | 行为 |
|---|---|---|
| 任意模块 lines 下降 > 2% | ⚠️ | PR comment 提示 |
| 任意模块 lines 下降 > 5% | 🚨 | PR comment 提示 + 建议阻塞 merge |
| 总体 lines 覆盖率 < 50% | 🚨 | 强烈建议补充测试 |

**当前策略**：仅 comment，**不阻塞 merge**。可在后续 change 中升级为阻塞。

## 例外规则

以下情况**不**触发下降报警：

1. **新增模块**：第一次进入 baseline 时，按当前覆盖率初始化
2. **大规模重构**：在 commit message 中加 `[coverage-skip]` 标记可豁免单次 PR
3. **依赖升级**：仅当第三方库版本变更导致覆盖率统计口径变化时

## 工作流

### 本地验证

```bash
# 需要 lcov 已安装（apt-get install -y lcov）
./engine/scripts/coverage/run.sh

# 查看 HTML 报告
open coverage-html/index.html
```

### CI 自动跑

每次 push / PR 到 `main` 或 `project` 分支，CI 自动：

1. 配置 CMake（`-DENABLE_COVERAGE=ON`）
2. 编译 + 跑测试
3. lcov capture + filter
4. genhtml HTML 报告
5. aggregate.py → 按模块聚合 JSON
6. compare.py → 与 baseline 对比，生成 markdown 表格
7. badge.py → 生成 SVG 徽章
8. 上传 artifact（coverage-html/, coverage.info, coverage-current.json, coverage-diff.md, coverage-badge.svg）

### PR comment 自动化

当前：手动 review coverage-diff.md
未来：在 `.github/workflows/ci.yml` 增加 `peter-evans/create-or-update-comment` 自动发评论

## 故障排查

| 问题 | 解决 |
|---|---|
| `lcov: command not found` | apt-get install -y lcov |
| `geninfo: ERROR: cannot read profile` | 删除 `*.gcda` 后重新编译运行 |
| 覆盖率始终为 0 | 检查 CMake 是否正确传递 `-DENABLE_COVERAGE=ON` |
| 第三方代码被计入 | 检查 `lcov --extract` 路径过滤规则 |

## 历史

- **2026-07-10**：S2 实施，本策略文档初版
- baseline 文件创建，所有模块占位为 0（等待 CI 首次跑完后填实）