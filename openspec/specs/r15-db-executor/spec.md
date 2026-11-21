> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# R15 DB SQL 执行引擎 Spec

## 概述

R15 聚焦 SQL 执行引擎的端到端流程，包括解析、重写、优化、执行四个阶段。

## 能力规格

### 1. SQL 解析 (db_query_parser)

- **词法分析**：将 SQL 文本转换为 Token 流
- **语法分析**：递归下降解析生成 AST
- **关键字支持**：SELECT/FROM/WHERE/JOIN/INSERT/UPDATE/DELETE
- **操作符支持**：算术、比较、逻辑、LIKE、IN、BETWEEN

### 2. 查询重写 (db_query_rewrite)

- **谓词下推**：将过滤条件下推到 JOIN 之前
- **常量折叠**：编译时计算常量表达式
- **子查询展开**：IN/EXISTS 子查询转换为 JOIN
- **视图合并**：展开嵌套视图定义

### 3. 逻辑计划 (db_logical_plan)

- **关系代数**：Scan/Project/Filter/Join/Aggregate
- **代价估算**：行数估计、代价计算
- **规则优化**：列裁剪、常量折叠

### 4. 物理计划 (db_physical_plan)

- **算子选择**：SeqScan vs IndexScan、HashJoin vs NestLoop
- **Join 重排**：基于代价选择最优 Join 顺序
- **索引选择**：根据选择性选择最优索引

### 5. 执行引擎 (db_execution_operators)

- **火山模型**：统一的 next()/open()/close() 接口
- **算子实现**：SeqScan/Filter/Project/HashAgg/NestLoop
- **元组槽**：TupleTableSlot 元组传递机制

### 6. 执行器细节 (db_execution_operators)

- **连接算子**：HashJoin、NestedLoop、MergeJoin
- **聚合算子**：HashAgg、SortAgg、WindowAgg
- **向量化**：批量处理、SIMD 加速（可选）

### 7. 列式存储 (db_executor_summary)

- **列式布局**：按列存储 vs 行式存储
- **批量处理**：一次处理多行
- **SIMD 加速**：利用 CPU 向量指令

## 工程对照

| 学习卡 | 工程源码 | 行数 |
|--------|----------|------|
| db_query_parser | engineering/src/db/sql/sql_parser.c | ~2000 |
| db_query_rewrite | engineering/src/db/sql/ | 待扩展 |
| db_execution_operators | engineering/src/db/sql/sql_executor.c | ~750 |

## 验收标准

- [x] 7 张卡 scaffold 完成
- [x] 7 张卡 gcc 编译通过
- [x] 7 张卡运行输出正确
- [x] NOTES.md 工程对照 ≥100 字
- [x] statuses.json 更新为 done