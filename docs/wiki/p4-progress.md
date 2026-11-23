# P4 进度记录

> 计划：[2026-11-23-multimodal-wiki-p4-other-libraries.md](../../../ai-docs/methodology/planning/2026-11-23-multimodal-wiki-p4-other-libraries.md)
> 按批次追加，不覆盖历史批次。

## 批次 0：基础设施（2026-11-23）

### Task 1 泛化 manifest 生成器 — **完成**

| 项 | 结果 |
|---|---|
| 新增 `scripts/wiki/gen_manifest.py` | 库无关：`--project` / `--spec-module` / `--src-prefix`（支持逗号多前缀）/ `--verify` / `--repo-root` |
| 5 处 postgres 专用耦合 | 全部参数化：模块名、project 名、分组表、`src/` 前缀、伪标题规则 |
| 等价性验证 | 用 `gen_manifest.py --project postgres --spec-module postgres_mechanism_specs --src-prefix src` 生成的 manifest 与仓库内 `_manifest.yml` **逐字节相同** |
| `gen_postgres_manifest.py` | 收敛为薄封装（24 增 / 126 删），行为不变，其自身测试与重新生成验证仍通过 |
| 新增 `scripts/wiki/test_gen_manifest.py` | 222 行，覆盖 spec 自洽、伪标题检测、前缀多值、refs 校验 |

### Task 2 catalog stars 回填 — **部分完成（150/152）**

| 项 | 结果 |
|---|---|
| 新增 `scripts/wiki/fill_stars.py` | GitHub REST `/repos/{owner}/{repo}` 取 `stargazers_count` / `license` / `language`；55 次/轮上限、403/429 与 `X-RateLimit-Remaining=0` 即停、常规网络错误重试 3 次、`--dry-run`、断点续跑 |
| 缓存 | `scripts/wiki/catalog_stars_cache.json`，96 条记录带 `fetched_at`（ISO8601 UTC） |
| 回填结果 | **150 / 152 条为真实取数**，`stars_as_of` 为 `2026-11-23`（1 条为 `2026-11-22` 的既有真实值） |
| 两轮实跑 | 第 1 轮 48 个成功（限流用尽即停）；第 2 轮 1 个失败（限流窗口已恢复但目标库 404） |
| 未回填 2 条 | 见下表，**保持占位并在本节登记**，不填假数 |

| 条目 | 原因 | 处置 |
|---|---|---|
| `openGauss` | repo 是 `https://gitee.com/opengauss/openGauss-server`，**非 GitHub 托管**，脚本按设计拒绝请求 | 保持 `stars: 0` / `stars_as_of: 1970-01-01` / `language: unknown`。要取真实值需另写 Gitee API 路径，属后续批次 |
| `pklite` | `https://github.com/nicholasgasior/pklite` 返回 **HTTP 404**（仓库已删除或改名） | 保持占位；缓存里留 `status: failed` + `http_status: 404` + `fetched_at`，可追溯。要么换真实 repo URL，要么从清单里移除——这是清单维护决策，不在本批擅改 |

**未编造任何数字**：`db-bench` 取到真实 `stars=0`，`stars_as_of` 为当天，属真实 0 而非占位 0；这一条已用 `test_real_zero_stars_is_not_placeholder` 固化为回归测试。

### Task 3 门禁接入 CI — **完成**

| 项 | 结果 |
|---|---|
| 新增 `projects/.github/workflows/wiki.yml` | 4 个 job：`wiki-lint` / `wiki-manifest` / `wiki-refs` / `wiki-tests`；触发路径 `docs/wiki/**`、`scripts/wiki/**`、`apps/wiki-rag/**`、workflow 自身 |
| refs job | 先 `repo_cache.ensure_repo` 浅克隆 postgres，`actions/cache` 键为 `wiki-repo-postgres-<sha>`；clone 失败 `exit 1` 并打 `::error::`，不静默跳过 |
| 门禁范围 | `docs/wiki/projects`（页面树）；**不能**用 `docs/wiki`，否则 `wiki-schema.md` 与三份报告会被判 FAIL |
| **修正一处真实缺陷** | 原稿 4 个 job 都写了 `working-directory: projects` —— 这是顶层聚合仓 `C:\code` 的相对写法，在 `projects` 子仓里会变成不存在的 `projects/projects`，**CI 必然全 job 失败**。已全部删除并在文件头注释说明原因 |
| 本地验证 | 按 CI 里的原样命令在仓根执行：`wiki_lint docs/wiki/projects` → 152 页 0 失败；`wiki_refs --repo-root .cache/repos/postgres docs/wiki/projects` → 152 页 0 失败；`wiki_manifest_lint` → PASS；`unittest discover` → 341 tests OK |

**未做**：`actionlint` 本地不可用，YAML 语法靠人工核对 + 与 `ci.yml` 的既有约定对齐；GitHub 侧的真实跑通需等下一次 push 后的 Actions 记录（本批在本地复现了全部 4 个 job 的实际命令）。

### Task 4/5 首批三库（sqlite / mysql-server / redis） — **未开始**

原因：Task 2 的 stars 回填在两轮限流窗口里已用完配额，Task 5 依赖 Task 2 的清单口径稳定；且三库各自需要独立的机制盘点（Phase 1），是本阶段最重的一步，须单独排窗口。**下一批次从这里起。**

---

## 遗留（本批结束时的未决项）

1. 首批三库（sqlite / mysql-server / redis）机制盘点未开始。
2. `openGauss`（Gitee）与 `pklite`（404）两条占位需清单维护决策。
3. `actionlint` 未装，workflow 只做了人工与本地命令验证，未经 GitHub Actions 实跑。
4. 对比页（`_compare/`）未做（计划 §6 说首批三库后才做样板）。
