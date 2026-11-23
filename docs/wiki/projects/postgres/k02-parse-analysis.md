---
project: postgres
feature: parse-analysis
page_type: feature
schema_version: 1
refs:
  - path: src/backend/parser/analyze.c
    symbols: [parse_analyze_fixedparams, transformStmt]
  - path: src/backend/parser/parse_expr.c
    symbols: [transformExpr]
  - path: src/backend/parser/parse_clause.c
    symbols: [transformFromClause, transformWhereClause]
---

# SQL 语义分析

## 一句话职责

语义分析把 raw parse tree 补全为带类型、关系编号和权限信息的 `Query` 树。

## 机制是什么

`parse_analyze_fixedparams` 是带已知参数类型的分析入口：它创建 `ParseState`、登记 `$n` 参数类型、调用 `transformTopLevelStmt`，最后计算 query id、触发 `post_parse_analyze_hook` 并上报统计。`ParseState` 在这次分析内持有 rtable、namespace、joinlist、目标表关系等状态，同一 `ParseState` 的子层级由 `parse_sub_analyze` 以父子关系建立。

`transformStmt` 按节点类型分派：SELECT/INSERT/UPDATE/DELETE/MERGE 等可优化语句走完整变换，工具类语句只被包进 `CMD_UTILITY` 的 `Query` 外壳。边界在于它只确定语义与结构，不做规则改写、不做规划、不生成路径；权限检查在此仅记录所需权限位，实际校验在执行阶段。它也不是表达式求值器。

## 怎么实现的

1. `parse_analyze_fixedparams` 建立 `pstate`、设置 `p_sourcetext`，若有参数则 `setup_parse_fixed_parameters` 登记类型；`p_queryEnv` 携带查询环境。
2. `transformTopLevelStmt` 先尝试 `transformOptionalSelectInto`（把顶层 `SELECT ... INTO` 转成 `CreateTableAsStmt`），再进入 `transformStmt` 递归分派，回填 `stmt_location` 与 `stmt_len`。
3. `transformStmt` 对可优化语句调用各自 `transformXxxStmt`；MERGE、VALUES、集合操作、`DECLARE CURSOR`、`EXPLAIN`、`CREATE TABLE AS`、`CALL` 单独处理；其余语句生成 `CMD_UTILITY` 外壳。返回后统一设置 `querySource = QSRC_ORIGINAL` 与 `canSetTag = true`。
4. `transformFromClause` 从左到右处理 FROM 项，每项经 `transformFromClauseItem` 加入 rtable 并产生 namespace 项；处理时先把新项标为仅 LATERAL 可见，全部处理完再统一放开可见性，从而保证 LATERAL 只能引用左侧项。
5. `transformWhereClause` 先 `transformExpr` 再 `coerce_to_boolean`；`transformExpr` 保存并设置 `p_expr_kind` 后递归，按节点类型分派 `A_Expr`、`FuncCall`、`SubLink`、`TypeCast` 等到具体变换函数，期间完成操作符/函数解析和类型转换。
6. 目标表由 `setTargetTable` 打开并取 `RowExclusiveLock`（一直持有到事务结束），记入 `p_target_relation` 与 `p_target_nsitem`，并把 RTE 的所需权限设为传入的 ACL 模式。
7. 分析结束前汇总 collation、检查聚合与窗口函数位置，返回 `Query`；`free_parsestate` 关闭关系但不释放锁。`ParseState` 与结果树都在当前内存上下文，无跨进程共享。

## 关键取舍

- **源码事实：**工具类语句只被包进 `CMD_UTILITY` 的 `Query`，注释说明工具命令在此不加锁，因为无法保证执行时仍持有；`DECLARE CURSOR`、`EXPLAIN`、`CREATE TABLE AS` 例外，因为内部含可优化语句。**架构推论：**避免为不存在的锁生命周期承诺语义分析结果；若对工具语句做完整变换并据此加锁，锁会跨越不可控的执行时刻。
- **源码事实：**`AcquireRewriteLocks` 之外的常规 SELECT 关系锁由分析阶段的 `parserOpenTable`/`setTargetTable` 取得，锁持续到事务结束。**架构推论：**把锁生命周期与事务绑定，换取重写、规划、执行三阶段都看到稳定模式；若按阶段释放，中途 DDL 可让已分析的树失效。
- **源码事实：**`transformFromClause` 逐项处理 FROM 并在结束时统一放开 namespace 可见性。**架构推论：**这样只允许 LATERAL 引用左侧项，天然阻止前向引用；若一次性放开所有项，LATERAL 的依赖方向将无法静态判定。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/parser/analyze.c` | `parse_analyze_fixedparams` | 建立 ParseState、登记参数类型并驱动顶层分析 |
| `src/backend/parser/analyze.c` | `transformStmt` | 按语句类型分派变换或包成 CMD_UTILITY Query |
| `src/backend/parser/parse_expr.c` | `transformExpr` | 设置表达式上下文并递归完成表达式语义与类型 |
| `src/backend/parser/parse_clause.c` | `transformFromClause` | 处理 FROM 项并管理 LATERAL 可见性 |
| `src/backend/parser/parse_clause.c` | `transformWhereClause` | 变换条件表达式并强制为布尔类型 |

## 参考

- [PostgreSQL 文档：解析器阶段](https://www.postgresql.org/docs/current/parser-stage.html) —— 语义分析把 raw parse tree 转为查询树
- [PostgreSQL 源码：analyze.c](https://github.com/postgres/postgres/blob/master/src/backend/parser/analyze.c) —— 语句分派与 Query 构造

## 与主核 / AI 的关系

主核 SQL 前端同样需要“语法树到带类型查询表示”的语义补全阶段，本页给出可对照的阶段职责与锁生命周期处理。暂无直接 AI 对应。
