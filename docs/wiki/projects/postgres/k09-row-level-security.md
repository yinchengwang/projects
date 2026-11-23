---
project: postgres
feature: row-level-security
page_type: feature
schema_version: 1
refs:
  - path: src/backend/rewrite/rowsecurity.c
    symbols: [get_row_security_policies, add_security_quals, add_with_check_options]
  - path: src/backend/utils/misc/rls.c
    symbols: [check_enable_rls]
---

# 行级安全

## 一句话职责

在查询重写阶段把策略表达式转成扫描限定条件与写入检查项，使不可见或不允许写入的行在访问路径上被过滤或直接报错。

## 机制是什么

行级安全把策略保存为 `RowSecurityPolicy`（含命令类型、适用角色数组、USING 与 WITH CHECK 表达式以及 `hassublinks` 标记），在重写期为每个可访问的关系决定“读哪些行可见”与“写哪些行可以落盘”。判定是否启用由 `check_enable_rls` 完成，它读取 `pg_class.relrowsecurity`/`relforcerowsecurity`，并考虑 BYPASSRLS、表所有者与 `row_security` GUC。

本机制只负责“生成本查询的策略限定”，不负责策略的存取控制（CREATE/ALTER POLICY 的权限与 catalog 写入）、不负责执行期的行过滤本身，也不负责用户对表的对象级 ACL 判定。限定最终以普通表达式和 `WithCheckOption` 的形式挂到查询树上，因此实际过滤由扫描与修改节点使用通用表达式机制执行。与普通视图的 `WITH CHECK OPTION` 复用同一套 `WithCheckOption` 结构，但 RLS 产生的检查带独立 kind，便于执行期区分。

## 怎么实现的

1. `get_row_security_policies` 只处理 `RTE_RELATION` 且 `relkind` 为普通表或分区表；其他 relkind 直接返回空。它从 `rteperminfos` 取 `RTEPermissionInfo`，按 `checkAsUser` 或当前用户确定 `user_id`，然后把该 user 显式传给 `check_enable_rls`，使视图等“以别人身份检查”的场景也能正确判定。
2. `check_enable_rls` 三级返回：`RLS_NONE` 表示不启用（内置关系或没有 RLS 标记）；`RLS_NONE_ENV` 表示当前不应用、但结论依赖环境（BYPASSRLS 用户、或未 FORCE 的表所有者）；`RLS_ENABLED` 表示必须应用。所有者分支仅在 `relforcerowsecurity` 为真且不处于 `InNoForceRLSOperation` 时才升级为启用，注释说明这是为了让引用完整性检查在 FORCE 场景下仍能通过。
3. `get_row_security_policies` 对 `RLS_NONE_ENV` 只置 `hasRowSecurity = true` 而不加任何限定。这种“标记但不过滤”的形态配合计划缓存，使环境（GUC 或当前角色）变化时可以强制重新规划，避免把旧环境下的判定结果长期复用。
4. 命令类型由 `rt_index == root->resultRelation` 决定：目标关系用查询的命令类型，其它关系一律按 `CMD_SELECT`。于是 `UPDATE t1 ... FROM t2` 中 t1 得到 UPDATE 策略、t2 得到 SELECT 策略。对于 `SELECT ... FOR UPDATE/SHARE`（权限位含 `ACL_UPDATE`），先加 UPDATE 的 USING 限定，再叠加 SELECT 的，源码注释说明顺序是“更高特权的策略优先”。
5. `add_security_quals` 把同类的 permissive 策略表达式用 OR 合成一条，restrictive 策略则各自单独加入（天然 AND）。两类都用 `ChangeVarNodes` 把内层 varno 1 改写为当前 `rt_index`。若一条 permissive 都没有，则插入常量 false，形成隐式默认拒绝。
6. `add_with_check_options` 结构相同，但产物是 `WithCheckOption`：permissive 合成一条且 `polname` 置空（失败时含义是“没有任何策略允许”，而非违反某条具体策略），restrictive 每条各生成一个并保留策略名以便报错定位。宏 `QUAL_FOR_WCO` 决定取 WITH CHECK 表达式，或在该策略未定义时回退用 USING；`force_using` 为真时一律用 USING。
7. 写入面的组合比较复杂：INSERT/UPDATE 先为自身命令类型加检查；若还需要 SELECT 权限（如带 RETURNING），再把 ALL/SELECT 策略以“写入检查项”而非扫描限定的形式加入，注释说明这样做是为了让违规时报错而不是静默丢弃行。`INSERT ... ON CONFLICT DO UPDATE/DO SELECT` 额外产生 `WCO_RLS_CONFLICT_CHECK` 来校验冲突行，DO UPDATE 再为实际执行的 UPDATE 加 `WCO_RLS_UPDATE_CHECK`；MERGE 则预先取 UPDATE/DELETE/INSERT 策略，待执行期按最终动作决定用哪一套。
8. `hasSubLinks` 在收集过程中被逐条 OR 进来，只要任一策略表达式含子链接就置真，供上层决定是否需要额外处理。全程对策略表达式使用 `copyObject`，避免把 catalog 中的共享表达式节点直接挂到查询树上而被后续改写修改。

## 关键取舍

- 源码事实：`RLS_NONE_ENV` 被当作“标记可能有 RLS 但不加限定”的第三态，`get_row_security_policies` 仅置 `hasRowSecurity` 就返回。放弃“当前不适用就当作完全没有 RLS”使后续环境变化能被感知并触发重新规划；代价是这类查询多背一个“必须重规划”的标记，且 `hasRowSecurity` 为真并不代表本查询真的加了任何过滤。
- 源码事实：写入路径把 ALL/SELECT 策略作为写入检查项加入，并在 `get_row_security_policies` 的注释中写明是为了“避免静默丢弃要插入的行”。放弃把这些限定当作扫描条件过滤掉，换取违规时给出明确错误；代价是同一策略在 SELECT 与 INSERT ... RETURNING 场景下的产物类型不同，阅读时需按 `WCOKind` 分辨。
- 源码事实：`add_with_check_options` 给 permissive 合成的检查项故意不填策略名，restrictive 则各自保留名字。放弃统一报出具体策略名换取语义准确——permissive 全部失败意味着“没有任何策略授权”，把它归咎于某一条策略会误导运维；代价是错误信息在两种策略下的详细程度不对等。
- 源码事实：`add_security_quals` 在没有 permissive 策略时插入常量 false，而不是直接报错或跳过。放弃“无策略即无限制”的直觉默认，换取默认拒绝的安全姿态；代价是所有策略都被删除或角色不匹配的表在查询时表现为“返回零行”，需要使用者自己意识到这是 RLS 生效而非数据为空。
- 架构推论：MERGE 的策略预取与执行期选择把“用哪套策略”推迟到动作确定之后。若在重写期强行二选一，会在同一语句中同时执行 UPDATE 与 INSERT 时用错策略；当前形态避免了这一点，但代价是查询树上挂载了多套检查项，执行期必须按动作挑选。源码只说明了这一组织方式，未对代价作显式讨论。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/rewrite/rowsecurity.c` | `get_row_security_policies` | 判定 RLS 状态、按命令类型收集策略并产出扫描限定与写入检查项 |
| `src/backend/rewrite/rowsecurity.c` | `add_security_quals` | 把 permissive 策略用 OR 合成、restrictive 逐条加入，并处理默认拒绝 |
| `src/backend/rewrite/rowsecurity.c` | `add_with_check_options` | 生成写入检查项，按策略是否有 WITH CHECK 表达式决定回退到 USING |
| `src/backend/utils/misc/rls.c` | `check_enable_rls` | 依据 relrowsecurity/relforcerowsecurity、BYPASSRLS 与所有者身份返回三态判定 |

## 参考

- [PostgreSQL 文档：行级安全策略](https://www.postgresql.org/docs/current/ddl-rowsecurity.html) —— 支撑策略、permissive/restrictive 组合与默认拒绝语义。
- [PostgreSQL 文档：CREATE POLICY](https://www.postgresql.org/docs/current/sql-createpolicy.html) —— 支撑 USING 与 WITH CHECK 的表达式含义及适用命令类型。
- [PostgreSQL 文档：行级安全与 FORCE ROW LEVEL SECURITY](https://www.postgresql.org/docs/current/sql-altertable.html) —— 支撑表所有者与 FORCE 选项对 RLS 生效范围的影响。
- [PostgreSQL 官方源码：RLS 重写](https://github.com/postgres/postgres/blob/master/src/backend/rewrite/rowsecurity.c) —— 支撑策略收集、限定合成与写入检查项细节。
- [PostgreSQL 官方源码：RLS 启用判定](https://github.com/postgres/postgres/blob/master/src/backend/utils/misc/rls.c) —— 支撑三态返回与环境相关判定。

## 与主核 / AI 的关系

暂无直接对应。主核若实现行级策略，可借鉴“permissive 取并、restrictive 取交、缺省拒绝”“读限定与写检查项分开产出”“环境相关判定要留可重规划标记”这三条；策略目录、身份切换与错误上抛需由主核自身的权限模型承接。
