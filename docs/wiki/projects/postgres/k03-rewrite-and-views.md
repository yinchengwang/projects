---
project: postgres
feature: rewrite-and-views
page_type: feature
schema_version: 1
refs:
  - path: src/backend/rewrite/rewriteHandler.c
    symbols: [QueryRewrite, rewriteTargetView, AcquireRewriteLocks]
  - path: src/backend/rewrite/rewriteDefine.c
    symbols: [DefineRule]
---

# 重写与视图展开

## 一句话职责

重写阶段把一条已分析的查询按规则与视图定义展开成若干待执行查询，并挂上行级安全条件。

## 机制是什么

`QueryRewrite` 只处理 `QSRC_ORIGINAL` 且 `canSetTag` 的顶层查询。它分三步：先用 `RewriteQuery` 应用所有非 SELECT 规则（可能得到零到多条查询），再对每条查询执行 `fireRIRrules` 展开 SELECT 规则即视图，最后决定哪条查询负责设置命令结果标签。返回的是一个查询列表，因为一条规则可以展开出多条动作。

边界上，此机制不规划、不执行，也不进行权限校验；它只做结构改写并记录权限位。视图展开把视图的 ON SELECT 规则换成基表引用，`rewriteTargetView` 只处理目标是视图、且视图可自动更新的情形；含 INSTEAD OF 触发器或不可自动更新的视图交由递归的 `RewriteQuery` 与执行器处理。行级安全（RLS）条件也在此阶段注入。

## 怎么实现的

1. `AcquireRewriteLocks` 递归遍历 rtable：关系 RTE 按 `rellockmode` 取锁并持有到事务结束（`forExecute` 为 false 时降为 `AccessShareLock`），同时刷新 `rte->relkind`；JOIN RTE 中被删除列的 alias var 置为 NULL；子查询 RTE 与 CTE、SubLink 子查询递归处理。
2. `QueryRewrite` 调 `RewriteQuery` 应用非 SELECT 规则，得到 `querylist`；每条查询再经 `fireRIRrules` 展开 SELECT 规则，并把 `queryId` 回填为原始查询的 id。
3. `fireRIRrules` 逐个 RTE 检查 `rd_rules` 中的 `CMD_SELECT` 规则；对已使用的关系应用 `ApplyRetrieveRule`；用 `activeRIRs` 检测视图自引用造成的无限递归。
4. `rewriteTargetView` 复制视图的 ON SELECT 查询，校验 `view_query_is_auto_updatable` 与列级可更新性，取基表的 `RowExclusiveLock`，把基表 RTE 追加到外层 rtable，把视图 targetlist 的 Var 改指新 RTE。
5. 随后迁移权限信息：新 RTE 的 `checkAsUser` 依 `security_invoker` 取查询者或视图属主，`requiredPerms` 继承视图，`selectedCols` 保留基表位图，`insertedCols`/`updatedCols` 经 `adjust_view_column_set` 映射到基表列；视图 RTE 的 `securityQuals` 移到新目标 RTE。
6. `ReplaceVarsFromTargetList` 替换外层所有引用视图的 Var，`ChangeVarNodes` 把 `resultRelation` 等 RTI 引用改指基表，非 DELETE 语句还要按视图 targetlist 修正 resno。
7. RLS 挂接在 `fireRIRrules` 内：对普通表与分区表 RTE 调 `get_row_security_policies` 取 `securityQuals` 与 `withCheckOptions`，插入到 RTE 与查询的前部；新条件含 SubLink 时补锁并递归触发 RIR，同样用 `activeRIRs` 防环。
8. 规则定义侧由 `DefineRule` 完成：先 `transformRuleStmt` 分析动作与条件，再以 `AccessExclusiveLock` 锁定关系，交给 `DefineQueryRewrite` 校验可适用 relkind、属主权限、禁止改写 OLD/NEW，并写入 `pg_rewrite` 与依赖记录。

## 关键取舍

- **源码事实：**`AcquireRewriteLocks` 对关系 RTE 取锁后 `relation_close(rel, NoLock)`，注释说明锁要持有到事务结束；`forExecute=false` 时统一用 `AccessShareLock`。**架构推论：**重写、规划、执行都依赖同一份模式快照，因此放弃阶段性释放锁；若提前放锁，中途 DDL 会让已展开的查询与基表结构不一致。
- **源码事实：**`rewriteTargetView` 仅对可自动更新的视图做基表替换，并把视图权限位映射到基表；不可自动更新时 `error_view_not_updatable` 报错而非静默回退。**架构推论：**把视图展开放在重写阶段，使规划器只需处理基表；若留给规划器再展开，权限与 RLS 条件的挂接位置将无法在此阶段确定。
- **源码事实：**RLS 的 `securityQuals` 被拼到 RTE 现有 `securityQuals` 之前，注释说明这样保证表自身的 RLS 条件优先于 security-barrier 视图的条件。**架构推论：**顺序即优先级；若反过来拼接，视图屏障条件可能先于策略过滤，造成策略可见范围被错误放大。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/rewrite/rewriteHandler.c` | `QueryRewrite` | 顶层重写入口，三步产出查询列表并决定命令标签 |
| `src/backend/rewrite/rewriteHandler.c` | `rewriteTargetView` | 把可自动更新视图改写为基表目标并迁移权限位 |
| `src/backend/rewrite/rewriteHandler.c` | `AcquireRewriteLocks` | 递归取关系锁、刷新 relkind、清理失效 JOIN alias |
| `src/backend/rewrite/rewriteDefine.c` | `DefineRule` | CREATE RULE 的分析、加锁与规则落库入口 |

## 参考

- [PostgreSQL 文档：规则系统](https://www.postgresql.org/docs/current/rules.html) —— 规则与视图展开的用户可见机制
- [PostgreSQL 文档：CREATE RULE](https://www.postgresql.org/docs/current/sql-createrule.html) —— 规则定义语法与限制
- [PostgreSQL 源码：rewriteHandler.c](https://github.com/postgres/postgres/blob/master/src/backend/rewrite/rewriteHandler.c) —— 重写与视图展开实现

## 与主核 / AI 的关系

主核若有视图或等价抽象，本页给出的“重写期展开 + 权限位迁移 + 安全条件前插”顺序可作对照；规则系统本身不必照搬。暂无直接 AI 对应。
