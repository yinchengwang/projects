> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# SQL 层 - 架构设计

## 概述

SQL 层负责 SQL 语句的解析、语义分析、优化和执行，是将 SQL 文本转换为存储层操作的完整处理管道。

---

## 一、子系统架构概览

```mermaid
flowchart TB
    subgraph "SQL 层"
        TOKENIZER[词法分析器<br/>sql_tokenizer_t]
        PARSER[语法分析器<br/>sql_parser_t]
        SEMANTIC[语义分析器<br/>sql_semantic.h]
        OPTIMIZER[优化器<br/>optimizer.h]
        EXECUTOR[执行器<br/>sql_executor_t]

        AST[AST 节点<br/>sql_node_t]
    end

    subgraph "下层依赖"
        ACCESS_METHOD[访问方法层<br/>Relation/HeapAM]
        STORAGE[存储核心层<br/>Buffer Pool/Disk/WAL]
        CATALOG[Catalog 系统<br/>元数据查询]
        TXN[事务管理器<br/>txn.h]
    end

    subgraph "上层接口"
        WIRE[Wire 协议<br/>PostgreSQL 协议]
        CLI[CLI 命令行<br/>cli.h]
        API[编程 API<br/>kv.h/table.h]
    end

    WIRE --> TOKENIZER
    CLI --> TOKENIZER
    API --> EXECUTOR

    TOKENIZER --> PARSER
    PARSER --> AST
    AST --> SEMANTIC
    SEMANTIC --> OPTIMIZER
    OPTIMIZER --> EXECUTOR

    EXECUTOR --> ACCESS_METHOD
    EXECUTOR --> CATALOG
    EXECUTOR --> TXN
    ACCESS_METHOD --> STORAGE
```

---

## 二、解析流程

### 2.1 完整处理管道

```mermaid
flowchart LR
    subgraph "SQL 处理管道"
        SQL_TEXT[SQL 文本<br/>SELECT * FROM users]
        step1["1. 词法分析"]
        step2["2. 语法分析"]
        step3["3. 语义分析"]
        step4["4. 逻辑优化"]
        step5["5. 物理优化"]
        step6["6. 执行"]
    end

    SQL_TEXT --> step1
    step1 --> |Token 流| step2
    step2 --> |AST| step3
    step3 --> |类型解析后 AST| step4
    step4 --> |重写后计划| step5
    step5 --> |执行计划| step6
    step6 --> |结果集| RESULT[ResultSet]
```

### 2.2 Token 类型

```mermaid
classDiagram
    class sql_token_type_e {
        <<enum>>
        +SQL_TOKEN_INT
        +SQL_TOKEN_FLOAT
        +SQL_TOKEN_STRING
        +SQL_TOKEN_IDENT
        +SQL_TOKEN_SELECT
        +SQL_TOKEN_INSERT
        +SQL_TOKEN_UPDATE
        +SQL_TOKEN_DELETE
        +SQL_TOKEN_CREATE
        +SQL_TOKEN_DROP
        +SQL_TOKEN_FROM
        +SQL_TOKEN_WHERE
        +SQL_TOKEN_EQ (==)
        +SQL_TOKEN_LT (<)
        +SQL_TOKEN_GT (>)
        +SQL_TOKEN_INT_TYPE
        +SQL_TOKEN_VARCHAR
        +SQL_TOKEN_TEXT
    }

    class sql_node_type_e {
        <<enum>>
        +SQL_NODE_CREATE_TABLE
        +SQL_NODE_DROP_TABLE
        +SQL_NODE_INSERT
        +SQL_NODE_UPDATE
        +SQL_NODE_DELETE
        +SQL_NODE_SELECT
        +SQL_NODE_COLUMN_DEF
        +SQL_NODE_EXPR_BINARY
        +SQL_NODE_EXPR_UNARY
        +SQL_NODE_LITERAL
        +SQL_NODE_COLUMN_REF
        +SQL_NODE_WHERE
    }

    class sql_node_t {
        +sql_node_type_t type
        +sql_data_type_t data_type
        +char* name
        +union fields
        +sql_node_t* left
        +sql_node_t* right
        +sql_node_t* next
    }
```

### 2.3 AST 结构示例

```mermaid
flowchart TB
    subgraph "SELECT AST 示例"
        SELECT_NODE[SELECT 节点]
        COL_LIST[列列表<br/>name, age]
        FROM_CLAUSE[FROM 子句<br/>users]
        WHERE_CLAUSE[WHERE 子句<br/>age > 18]

        WHERE_BINOP[二元运算符: >]
        WHERE_COL[列引用: age]
        WHERE_LIT[字面量: 18]
    end

    SELECT_NODE --> COL_LIST
    SELECT_NODE --> FROM_CLAUSE
    SELECT_NODE --> WHERE_CLAUSE
    WHERE_CLAUSE --> WHERE_BINOP
    WHERE_BINOP --> WHERE_COL
    WHERE_BINOP --> WHERE_LIT
```

---

## 三、执行器架构

### 3.1 执行器结构

```mermaid
classDiagram
    class sql_executor_t {
        +void* catalog
        +void* buffer_pool
        +wal_t* wal
        +wal_buf_t* wal_buf
        +uint32_t txn_id
        +bool in_transaction
        +uint64_t queries_executed
        +uint64_t rows_processed
        +char* error_msg
        +sql_executor_init(exec, path, buf_count)
        +sql_executor_create_table(...)
        +sql_executor_insert(...)
        +sql_executor_select(...)
        +sql_executor_update(...)
        +sql_executor_delete(...)
        +sql_executor_begin()
        +sql_executor_commit()
        +sql_executor_rollback()
    }

    class sql_exec_t {
        +sql_exec_create(db)
        +sql_exec_destroy(exec)
        +sql_exec(exec, node)
        +sql_exec_ddl(exec, node)
        +sql_exec_errmsg(exec)
    }

    class sql_query_result_t {
        +char** columns
        +int* col_types
        +int ncolumns
        +char** rows
        +int nrow
        +int row_capacity
    }

    sql_executor_t "1" --> "*" sql_query_result_t
```

### 3.2 DDL 执行流程（CREATE TABLE）

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Exec as 执行器
    participant Catalog as Catalog 系统
    participant WAL as WAL
    participant Page as 页面管理
    participant Disk as 磁盘

    Client->>Exec: 解析 CREATE TABLE 语句

    Exec->>Exec: 词法分析 (tokenizer)
    Exec->>Exec: 语法分析 → AST
    Exec->>Exec: 语义分析（类型检查）

    Exec->>Catalog: catalog_create_table(name, columns, ncolumns)

    Catalog->>Catalog: 分配表 OID
    Catalog->>Catalog: 分配 filenode

    Catalog->>Page: 创建元数据页面
    Page->>Disk: disk_alloc_page(file, META)
    Disk-->>Page: 新页面

    Catalog->>Page: 写入表头元组
    Catalog->>Catalog: 更新 pg_class 缓存

    loop 每列
        Catalog->>Catalog: 构造 column_info_t
        Catalog->>Page: 写入列元组到 pg_attribute
    end

    Catalog->>WAL: 记录系统表变更
    Catalog-->>Exec: 返回表 OID

    Exec-->>Client: 表创建成功
```

### 3.3 INSERT 执行流程

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Exec as 执行器
    participant Catalog as Catalog 系统
    participant HeapAM as Heap AM
    participant Buffer as Buffer Pool
    participant WAL as WAL

    Client->>Exec: INSERT INTO users VALUES (...)

    Exec->>Catalog: 查找表 metadata
    Catalog-->>Exec: table_info + column_info

    Exec->>Exec: 验证列数、类型
    Exec->>Exec: 构造元组

    Exec->>HeapAM: heap_insert(rel, tuple, len)

    HeapAM->>Buffer: 获取目标页面
    alt 页面空间不足
        Buffer->>Buffer: 分配新页面
    end
    Buffer-->>HeapAM: 页面数据

    HeapAM->>HeapAM: 设置 xmin = 当前事务 ID
    HeapAM->>HeapAM: 复制元组到页面

    HeapAM->>WAL: wal_write_insert(txn_id, key, value)
    WAL-->>HeapAM: 返回 LSN

    HeapAM->>HeapAM: 更新页面 LSN
    HeapAM->>Buffer: 标记页面为脏

    HeapAM-->>Exec: 插入成功
    Exec-->>Client: 影响行数 = 1
```

### 3.4 SELECT 执行流程

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Exec as 执行器
    participant Catalog as Catalog 系统
    participant Rel as Relation
    participant HeapAM as Heap AM
    participant TXN as 事务管理器
    participant Result as 结果集

    Client->>Exec: SELECT * FROM users WHERE age > 18

    Exec->>Catalog: 查找表 metadata
    Catalog-->>Exec: table_info + column_info
    Exec->>Rel: relation_open(table_oid)

    Exec->>TXN: 创建快照（Read Committed）
    TXN-->>Exec: snapshot

    Exec->>HeapAM: table_beginscan_all(rel)
    HeapAM-->>Exec: TableScanDesc

    loop 扫描所有页面
        Exec->>HeapAM: heap_getnext(scan, ForwardScanDirection)

        loop 遍历页面元组
            HeapAM->>TXN: 检查元组可见性
            TXN-->>HeapAM: 可见/不可见

            alt 元组可见
                Exec->>Exec: 提取列值
                Exec->>Exec: sql_eval_expr(where_expr, row_data)

                alt WHERE 条件满足
                    Exec->>Result: 添加到结果集
                else 条件不满足
                    HeapAM->>HeapAM: 跳过
                end
            else 不可见
                HeapAM->>HeapAM: 跳过
            end
        end
    end

    Exec->>HeapAM: table_endscan(scan)
    Exec->>Rel: relation_close(rel)

    Exec->>Result: 排序/投影

    Exec-->>Client: 返回结果集
```

### 3.5 UPDATE 执行流程

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Exec as 执行器
    participant HeapAM as Heap AM
    participant TXN as 事务管理器
    participant WAL as WAL

    Client->>Exec: UPDATE users SET age=20 WHERE id=1

    Exec->>HeapAM: 扫描表找到匹配行

    loop 扫描匹配元组
        HeapAM->>TXN: 锁定元组
        TXN-->>HeapAM: 锁定成功

        HeapAM->>HeapAM: 保存旧值（Undo）
        HeapAM->>HeapAM: 应用新值

        HeapAM->>WAL: wal_write_update(txn, key, old_val, new_val)
        WAL-->>HeapAM: 返回 LSN

        HeapAM->>HeapAM: 更新页面 LSN
        HeapAM->>HeapAM: 标记页面为脏
    end

    HeapAM-->>Exec: 更新完成
    Exec-->>Client: 影响行数 = N
```

---

## 四、优化器

### 4.1 优化器结构

```mermaid
classDiagram
    class Optimizer {
        +sql_node_t* optimize(sql_node_t* ast)
        +sql_node_t* rewrite_rules(sql_node_t* ast)
        +void cost_estimation(sql_node_t* plan)
        +void index_selection(sql_node_t* plan)
        +void join_ordering(sql_node_t* plan)
    }

    class RewriteRules {
        +常量化简
        +谓词下推
        +列裁剪
        +子查询展开
    }

    class CostEstimation {
        +表扫描成本
        +索引扫描成本
        +排序成本
        +统计信息（npages, ntupes）
    }

    Optimizer --> RewriteRules
    Optimizer --> CostEstimation
```

### 4.2 逻辑优化流程

```mermaid
flowchart TD
    Start[输入 AST] --> Rewrite1[规则 1: 常量化简<br/>WHERE 1=1 → 省略]

    Rewrite1 --> Rewrite2[规则 2: 谓词下推<br/>将 WHERE 条件下推到叶子节点]

    Rewrite2 --> Rewrite3[规则 3: 列裁剪<br/>只读取需要的列]

    Rewrite3 --> Rewrite4[规则 4: 子查询展开<br/>子查询 → JOIN]

    Rewrite4 --> Cost[成本估算<br/>选择最优路径]

    Cost --> Output[输出执行计划]
```

---

## 五、表达式求值

### 5.1 WHERE 条件求值

```mermaid
flowchart TD
    Start[sql_eval_expr] --> GetType[获取表达式类型]

    GetType --> CheckType{表达式类型}

    CheckType -->|二元运算| EvalBinOp[计算左右子树<br/>SQL_NODE_EXPR_BINARY]
    CheckType -->|一元运算| EvalUnOp[计算子表达式<br/>SQL_NODE_EXPR_UNARY]
    CheckType -->|列引用| EvalColRef[提取列值<br/>SQL_NODE_COLUMN_REF]
    CheckType -->|字面量| EvalLit[直接返回<br/>SQL_NODE_LITERAL]

    EvalBinOp --> Compare{运算符类型}
    Compare -->|==| Eq[比较相等]
    Compare -->|!=| Ne[比较不等]
    Compare -->|<| Lt[小于]
    Compare -->|>| Gt[大于]
    Compare -->|AND| And[逻辑与]
    Compare -->|OR| Or[逻辑或]

    Eq --> Return[返回 bool]
    Ne --> Return
    Lt --> Return
    Gt --> Return
    And --> Return
    Or --> Return
    EvalUnOp --> Return
    EvalColRef --> Return
    EvalLit --> Return
```

---

## 六、并发控制集成

### 6.1 事务内执行

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Exec as 执行器
    participant TXN as 事务管理器
    participant Storage as 存储层

    Client->>Exec: BEGIN
    Exec->>TXN: txn_begin(db, READ_WRITE)
    TXN-->>Exec: txn_context_t*

    Client->>Exec: INSERT INTO users VALUES (...)
    Exec->>TXN: txn_next_cid()
    Exec->>Storage: 插入操作

    Client->>Exec: UPDATE users SET age=20 WHERE id=1
    Exec->>TXN: txn_next_cid()
    Exec->>Storage: 更新操作

    Client->>Exec: COMMIT
    Exec->>TXN: txn_commit(txn)
    TXN->>Storage: 刷写 WAL
    TXN->>TXN: 释放锁
    TXN-->>Exec: 提交成功
    Exec-->>Client: COMMIT OK
```

---

## 七、支持的 SQL 语句

| 类别 | 语句 | 说明 |
|------|------|------|
| **DDL** | `CREATE TABLE` | 创建表，支持列定义和主键 |
| | `DROP TABLE` | 删除表及其所有数据 |
| | `CREATE INDEX` | 创建索引 |
| | `DROP INDEX` | 删除索引 |
| **DML** | `INSERT INTO ... VALUES` | 插入数据 |
| | `UPDATE ... SET ... WHERE` | 更新数据 |
| | `DELETE FROM ... WHERE` | 删除数据 |
| | `SELECT ... FROM ... [WHERE ...]` | 查询数据 |
| **数据类型** | `INT`, `VARCHAR(n)`, `TEXT`, `BLOB` | 基本类型 |
| **表达式** | `=`, `!=`, `<`, `>`, `<=`, `>=` | 比较运算符 |
| | `AND`, `OR`, `NOT` | 逻辑运算符 |

---

## 八、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| 词法分析 | `engine/include/db/parser/sql/sql.h` | `engine/src/db/parser/` |
| 语法分析 | `engine/include/db/parser/sql/sql.h` | `engine/src/db/parser/` |
| 语义分析 | `engine/include/db/executor/sql/sql_semantic.h` | `engine/src/db/executor/sql/` |
| 执行器 | `engine/include/db/executor/sql/sql_executor.h` | `engine/src/db/executor/sql/` |
| 优化器 | `engine/include/db/optimizer/optimizer.h` | `engine/src/db/optimizer/` |
| 表达式求值 | `engine/include/db/executor/sql/sql_exec.h` | `engine/src/db/executor/sql/` |