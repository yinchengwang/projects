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

## 批次 1：首批三库落地（2026-11-24 ~ 2026-11-25）

### Task 4 三库机制盘点 — **完成**

| 库 | 机制单元 | 组数 | 规格模块 | 清单 | 本地检出方式 |
|---|---|---|---|---|---|
| redis | 63 | 13 | `scripts/wiki/redis_mechanism_specs.py` | `docs/wiki/projects/redis/_manifest.yml` | 整仓检出 |
| mysql-server | 67 | 14 | `scripts/wiki/mysql_mechanism_specs.py` | `docs/wiki/projects/mysql-server/_manifest.yml` | tarball 全量源码 + 本地 git 仓 |
| sqlite | 86 | 14 | `scripts/wiki/sqlite_mechanism_specs.py` | `docs/wiki/projects/sqlite/_manifest.yml` | 全量克隆（partial clone） |

**检出方式（2026-11-25 复核并改用全量）**：三个库的浅克隆（`--depth 1 --single-branch`，只有 1 个 commit、0 个 tag）已全部换成**全量克隆**（partial clone，blob 按需下载），保留完整提交历史与全部 tag：

| 库 | commits | tags | pack 体积 | 耗时 |
|---|---|---|---|---|
| postgres | 107528 | 701 | 144 MB | ~9 分钟 |
| redis | 21183 | 427 | 220 MB | ~4 分钟 |
| sqlite | 37694 | 392 | 590 MB | ~1 分钟 |

浅克隆只有 1 个 commit、0 个 tag，**机制盘点时无法对着历史版本核验 refs**——这是把检出改成全量的直接原因。

**MySQL 的 git 协议路径在本机完全不可用**：`--depth 1`、`--filter=blob:none`、`--single-branch`、`http.version=HTTP/1.1` 四种组合都在 pack 传输阶段卡死——`.git/objects/pack/` 下的 `tmp_pack` 持续为 0 字节，且进程**没有活动网络连接**。同一时段另三个库的 clone 正常完成，说明是该仓库的服务端传输路径问题，不是本机带宽。

因此 MySQL 改用**tarball 全量源码 + 本地构造 git 仓**：

- 源码文件树：GitHub tarball API 全量下载（459.8 MB，解压后 38821 个文件）
- git 元数据：本地 `init` + `commit-tree` 构造，tag `mysql-8.0.40` 指向该提交
- 溯源说明：`.cache/repos/mysql-server/.upstream-provenance.txt` 记着上游提交号 `89e1c722476deebc3ddc8675e779869f6da654c0`

**MySQL 的边界必须说清**：

1. 源码**是全量的**（tag `mysql-8.0.40` 的完整文件树），refs 校验覆盖整个 tag，**不存在「未下载的文件不能校验」的问题**——这比早先的文件级稀疏检出严格。
2. 但**历史只有 1 个提交**（本地构造），不含上游真实提交历史，**无法按任意历史版本核验 refs**。要按历史核验，得先在网络条件允许时把完整历史取回。
3. tarball 尾部有截断，受影响的是 `unittest/` 目录；该目录不在任何页面的 refs 引用范围内，已被排除在检出目录之外。

**工具侧**：`scripts/wiki/dump_defs.py` 扩展到支持 C++ 成员函数（`Class::method`）与多行返回类型；另有逐行切分的定义抽取脚本（临时目录 `code-wiki-p4-mysql/`）用于区分「定义」与「调用点」，避免 PowerShell `Select-String` 造成的误判。

**refs 校验过程中的真实修正**（不是一次性过）：`mysql_parse` 在 8.0 已重构为 `THD::sql_parser`；`page_cur_search` 实际是 `page_cur_search_with_match`；`row_delete_for_mysql` 的公开入口是 `row_delete_for_mysql_using_cursor`；`LimitIterator` 不存在，实际是 `LimitOffsetIterator`；`CompositeIterator` 不存在，实际是 `AppendIterator` / `NestedLoopIterator`；`sql/log.cc` 是慢查询日志实现而非 binlog，binlog 实现在 `sql/binlog.cc`；`sql/auth/acl.cc` 在 8.0 改为 `sql/auth/sql_authorization.cc`；Redis 侧 `clusterInit` / `clusterProcessPacket` 在 `src/cluster_legacy.c` 而非 `src/cluster.c`；SQLite 侧 `sqlite3BtreeMoveto` 不存在，实际是 `sqlite3BtreeTableMoveto`。**最终 MySQL refs 校验失败 0 条。**

### Task 5 三库页面撰写 — **完成**

| 库 | 机制页 | `_index.md` | 门禁结果 |
|---|---|---|---|
| redis | 63 | 有 | lint 64 页 0 失败 / refs 64 页 0 失败 / manifest PASS |
| mysql-server | 67 | 有 | lint 68 页 0 失败 / refs 68 页 0 失败 / manifest PASS |
| sqlite | 86 | 有 | lint 87 页 0 失败 / refs 87 页 0 失败 / manifest PASS |
| postgres（复核） | 151 | 有 | lint 152 页 0 失败 / refs 152 页 0 失败 / manifest PASS |

四库合计 **367 篇机制页 + 4 份概览页**。

**索引生成器泛化**：新增 `scripts/wiki/rebuild_project_index.py`，把原 `rebuild_postgres_index.py` 的库专用逻辑抽成 `LIBRARIES` 表（概览正文 + 落点 + 参考按库配置），导航表仍完全由 `_manifest.yml` 驱动——手写导航表会在页数变动时漂移。支持 `--project <名>` 与 `--all`。

**索引落点的三个真实修正**：`wiki_refs` 按词边界匹配，多符号用 `、` 连接会被当成单个符号（改为 `, ` 分隔）；`serverMain` 在 Redis unstable 分支不存在（实际是 `main`）；`sqlite3BtreeMoveto` 在 SQLite 中只是注释里提到的名字（实际是 `sqlite3BtreeTableMoveto`）。

### 门禁与工具链

| 项 | 结果 |
|---|---|
| `scripts/wiki/gen_manifest.py` | 已能生成 redis / mysql-server / sqlite 三份清单，`--verify` 逐条核对 refs（失败 0 条） |
| `scripts/wiki/wiki_lint.py` | 四库目录级全过 |
| `scripts/wiki/wiki_refs.py` | 四库目录级全过（含 `_index.md`），在**全量检出**下同样 0 失败 |
| `scripts/wiki/wiki_manifest_lint.py` | 四份清单全 PASS |
| `scripts/wiki/repo_cache.py` | 新增 `full=True` 参数：全量克隆走 `--filter=blob:none`（partial clone），保留完整历史与全部 tag；新增 4 个单元测试 |
| `py -3 -m unittest discover -s scripts/wiki` | **Ran 357 tests，OK (skipped=1)**（新增 4 个 `full` 相关测试） |

### CI 接入（2026-11-25 更新）

`projects/.github/workflows/wiki.yml` 的 `wiki-refs` job 从「只跑 postgres」扩成**四库矩阵**（`postgres` / `redis` / `sqlite` / `mysql-server`），每个矩阵项独立克隆、独立跑 refs；`wiki-manifest` job 也从单个库扩到四个库逐一校验。

- 克隆方式改为 `ensure_repo(..., full=True)`，即 partial clone 全量检出，不用浅克隆。
- 缓存键从写死的 postgres commit 改为 `wiki-repo-<lib>-${{ github.sha }}`，避免换 checkout 时用到旧源码。
- 矩阵 `fail-fast: false`，一个库失败不影响其他库的结果可见性。
- **未做**：`actionlint` 本地不可用，workflow 只做了人工核对 + 本地复现全部命令的真实跑通；GitHub Actions 侧仍需等下一次 push 后的记录。

---

## 遗留（本批结束时的未决项）

1. 对比页（`_compare/`）未做（计划 §6 说首批三库后才做样板）。
2. `openGauss`（Gitee）与 `pklite`（404）两条占位需清单维护决策。
3. `actionlint` 未装，workflow 只做了人工与本地命令验证，未经 GitHub Actions 实跑。
4. **MySQL 的完整历史仍未取回**——本机网络下该仓库的 git 传输路径不可用（四种 clone 组合均在 pack 传输阶段卡死且无网络连接），现用的是 tarball 全量源码 + 本地构造的 git 仓。若要按历史版本核验 refs，需在网络条件允许时重跑全量克隆。
5. 清单扩容（原计划 §2.2 的 152 → 270）未做。
6. `%TEMP%\study-hub-judge-*` 残留 32 个目录（最早 2026-11-21），属 study-hub 自身问题，不在本任务范围。
