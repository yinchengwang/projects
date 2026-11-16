# Stage B：路线 B 实施设计（engine/db 为唯一主核）

> **状态：**用户已选择 B。本文件是实施前路线与门禁草案；除 Stage A 已完成的仓库重构外，Stage B 代码/服务/数据迁移尚未开始。
>
> **关键决策：**`engine/db` 成为唯一主数据库内核；`engine/src/db` 冻结并永久保留为历史实现（未来删除或恢复维护须另行设计、用户批准）。

## 1. 背景与已核实事实

Stage B0 能力矩阵及路线 B 工程面复核已发现：`engine/src/db` 外围协议/RAG/分布式目录并不等于可用服务能力。PG/MySQL query handlers 未接执行器、SDK 不解析返回结果、RAG 检索路径多有 TODO、Raft transport 发送部分留空；REST/vdb/vector_api 与 src/db 向量/WAL/checkpoint 深度耦合。另一方面，`engine/db` 的单机存储/事务/索引核心有完整独立门禁与近期实测。

| 项 | 证据/状态 |
|---|---|
| `engine/db` | 90 个测试程序、`make check` 输出 117 PASS / 0 FAIL（2026-11-16）；strict `-Werror`、deps 门禁。JSONB 源码已提交，但只 standalone tests，通过源码搜索未发现其接入 `db_exec`/SQL。 |
| `engine/src/db` | 494 个 C 源文件/约 21 万行、289 个测试文件；本轮对核心路径做了源码级缺口核验：WAL recovery 入口缺 `wal_replay` 定义、MVCC commit/abort 空实现、vacuum 空转、Catalog 未从持久化启动加载、BTree 搜索/插入/扫描未完成、SQL WHERE 明确拒绝。本轮未运行主库 CMake/CTest。 |
| `engine/src/db` 工程面 | wire handlers / SDK / RAG / Raft transport 多为 stub 或未接线；REST 向量 API 深度依赖 src/db 向量私有实现；sharding `migrate_manager` 直接使用 src/db KV。薄代码容易挪并不代表实际功能可用。 |
| 旧数据 | `engine/db` 根无 `.db/.wal/.bin`，Git 无跟踪数据库数据；约 576 MB 全在 ignored `test/tmp/`。当前初步指向测试/bench/debug 产物，但每个文件尚需逐项核实用途；未授权删除。 |
| README / 目录 | Stage A 已完成：code 聚合仓、四仓 submodule、README、`update=none` clean-clone 验证都已推送。 |

## 2. 唯一内核与仓目录策略

```text
projects/engine/db/       # 唯一维护的 DB 核心：继续开发与默认门禁
projects/engine/src/db/   # 冻结历史实现：保留目录、源码、测试、CMake 和历史
```

- 新 DB 功能、唯一对外主 API、旧数据目标落点、未来默认 build/test/CI 都以 `engine/db` 为准。
- `engine/src/db` 不被删除、不机械搬入 `engine/db`；其模块只能先审计实际行为和依赖，再决定适配、重写、仅存参考或舍弃。
- 不让主核链接两套 pager/WAL/MVCC/catalog/BTree，不形成并行事务/持久化路径。
- `engine/db` 当前公开 `include/db.h` 和 `StorageAM/IndexAM` 作为新能力适配的稳定边界；接口若不足，先设计/测试窄适配层，而不是在两个 DB 内核之间私用私有头文件。

## 3. 分阶段实施路线

### B0：资产、测试与数据基线（只读）

**输出：**
- 能力矩阵逐项区分：主核实测通过 / src/db 实测可用 / 源码存在未测 / stub/TODO / 缺失 / 仅设计；记录真实调用路径、build profile、对应测试及最近结果。
- `engine/db/test/tmp/` 全量文件清单：相对路径、字节数、mtime、文件类型/页头初识别、相关测试或 benchmark 源文件、是否能由确定命令+输入重建、是否需迁移。
- 与文档所称约 576 MB 的差异复核；WAL 和主 DB 配对检查，不按扩展名单独处置。
- 引擎服务化需求：明确首期只是嵌入式单进程，还是要 TCP server；旧 DB 目前无 socket/auth/ACL，连接是进程内共享实例，活跃事务上限 64、snapshot XIP 上限 128、模态语句有全局 recursive mutex。这些硬边界不能因迁移外层代码而消失。

**验证与安全：**只读扫文件/源码/配置；检查数据库内容时只能用确认不会改写源文件的只读方法或只读副本。不得在原文件上启动可能触发 recovery/WAL/checkpoint 的 DB 程序。不可证明可重建的数据默认保留。

**退出门：**所有用户数据/未知产物逐项定类；用户批准需要迁移的数据范围、备份路径和可清理范围。没有批准就不开始 B2 导出，也不删除 `test/tmp/`。

### B1：src/db 工程面资产审计（只读）

按每个候选模块的实际调用路径、构建目标和测试断言分类：真实且验证、真实未验证、stub/未接线、深度耦合。当前已看到：PG/MySQL query handlers 没有接执行器，SDK 发 SQL 后不解析结果，RAG 有 `void*` 解耦但核心检索步骤占位，Raft transport 部分 RPC 发送仍空，REST/vector_api 与 src/db vector 私有实现深度耦合，`sharding/migrate_manager` 直接依赖 src/db KV。

**退出门禁**：每个拟迁模块有文件/行号、调用者、CMake profile、对应测试和迁移/舍弃理由；未知项保持 unknown，stub 不计作现成能力。

### B2：定义旧 DB 的工程接入目标与最小服务闭环

`engine/db` 当前是嵌入式单进程单库 API/REPL：无 socket listener、认证/ACL、跨进程文件锁、网络 session；有进程内 `db_connect`。硬限制包括 `TXN_MAX_ACTIVE=64`、`SNAP_MAX_XIP=128`，模态路径仍用全局 recursive mutex。用户须先确认目标是继续嵌入式，还是要做单机 TCP 服务；不得因为选了路线 B 就默认建设网络服务。

若需要 server，单独设计并拆开验收：监听/关闭、session 生命周期、协议 framing、执行器适配、结果/null/错误码映射、认证/权限、取消/超时、文件互斥、连接/事务容量。先保持“单写/受限并发”也是可选首期产品范围；只有有真实需求再扩展 lock/MVCC。

**退出门禁**：用户确认首期服务场景、支持/不支持项、连接/认证/并发合同；协议或 RAG stub 不作为已实现资产。

### B3：选择并完成一个主核垂直切片

- 候选从 B1 确认为有价值的资产中选；同一轮只选一个子系统（例如纯协议 codec + 最小只读 SQL session，或 RAG 检索逻辑的一个真实算子）。
- 先在 `engine/db` 对外行为面写失败测试：输入→真实 `db_exec`/C API→结果或错误→关闭/重开数据一致性。
- 在 `engine/db` 原分层里实现；遵守 L0-L4 方向检查；不把 src/db private headers、对象库或 runtime 状态加入依赖。
- 每次改动后跑聚焦测试、`make check`（strict build + deps + unit/integration），持久化改动另跑 crash/reopen 故障场景。
- 只有当迁移成本/功能闭环/测试质量经 review 通过，才选下一项；若要复制第二套核心底座，停止并重审路线。

### B4：旧数据逻辑导出/导入（仅对 B0 批准的用户数据执行）

Stage B0 若证实只有可重建测试库且用户确认不保留内容，则本阶段记为 N/A；本身不清理任何文件。若有需迁移数据：

1. 用户确认原始只读备份目标、数据列表、容量、源文件 checksum；先复制备份，原件保持不动。
2. 设计有 schema/version 的导出中间格式，覆盖 SQL 列类型/NULL/主键、模态实体 ID 与 metadata、blob 内容；不支持项必须报错列出。
3. 导出器只从源副本读取，导出 manifest 含源 DB commit、文件 SHA、table/object counts、每条或分块内容 SHA、错误明细。
4. 创建新 DB 目标目录（须用户批准具体路径）；将数据导入 `engine/db` 主核对应 schema/API；操作可幂等重跑，不回写源副本。
5. 独立校验器比较源导出与目标读回的 row/object counts、ID 集合、字段类型/null、规范化内容 hash、blob 长度/hash 和索引重建结果。
6. 输出逐项迁移差异与未映射数据清单；用户检查并批准后，原始备份和导出包继续保留。本设计不授权删除备份或测试库。

### B5：冻结 src/db 并清理默认开发面

- 更新 `projects/CLAUDE.md`、`projects/README.md`、`engine/README.md`、`engine/db/README.md`：`engine/db` 为唯一活跃 DB，`engine/src/db` 冻结保留。
- 移除 src/db 子系统**默认** build/test/CI 维护入口须逐项审查；保留历史 CMake 和测试文件在目录内，不删除、不批量搬迁。
- 若其中模块作为独立可移植资产被迁出，原目录仍保存源历史，README 标记被替代/只读参考。
- code/Stage A README 只更新主核路径和链接，不声称 src/db 的 stub 能力已迁移。

**最终退出门禁**：用户批准迁移清单；唯一主核 build/test 可复现；所有纳入迁移的数据通过独立核验；README、CI、CLAUDE.md 对冻结状态一致；`engine/src/db` 与 `engine/db/test/tmp` 仍在原位。任何删除都另行设计和批准。

## 4. 当前状态与阻塞

- Stage A clean clone 已验证：code 本体与 projects/learning/ai-docs 均成功，reference 在普通 update 下跳过；显式覆盖后只取 catalog/笔记，不拉 95 个上游源码。总体积 1.5 GB；consistency 38 Python tests 全绿。
- `m5-verify` 已按用户授权迁到 code 根并忽略；旧 StillEvolving 本地目录已按单独授权删除，远程仓与历史保留。
- `engine/db` JSONB 15 文件已提交推送；strict build、deps、90 个测试程序 / 117 PASS 已验证。JSONPath 溢出风险经 probe 证伪；UTF-8 改为严格校验；obj_set realloc 复核无逻辑/安全缺陷；json_ovf 的单帧 alloc failure 回滚已加测试，read/free 明确由调用方串行化。
- Stage B0 初盘：`engine/db` 根无用户 DB 文件、Git 无跟踪 `.db/.wal/.bin`；约 576 MB 在 `test/tmp/`，其中 155/224 个文件名直见于 test/bench 源码，其余多为 WAL/测试残留/调试文件。**还未对所有源与配对文件完成审计**，未经用户确认不清理。
- 路线 B 下 `engine/db` 已选为唯一主核；`engine/src/db` 冻结保留。后续只能继续 B0/B1 只读盘点与服务化范围确认；任何代码迁移或数据导出，须先有更具体设计、批准的源/目标位置与数据范围。
