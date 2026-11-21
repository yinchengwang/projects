> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# SQL 执行引擎用户手册

**版本**：v5.0
**更新日期**：2026-07-15

---

## 目录

1. [快速开始](#1-快速开始)
2. [添加新算子](#2-如何添加新算子)
3. [编写测试](#3-如何编写测试)
4. [性能调优](#4-性能调优指南)
5. [常见问题](#5-常见问题)
6. [示例代码](#6-示例代码)

---

## 1. 快速开始

### 1.1 环境要求

- CMake 3.20+
- C11 / C++17 编译器
- 支持 pthreads（Linux/macOS）或 Windows 线程

### 1.2 构建

```bash
# 克隆项目
git clone https://github.com/your-project/book.git
cd book

# 创建构建目录
mkdir build && cd build

# 配置
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build . --parallel

# 运行测试
ctest --output-on-failure
```

### 1.3 第一个程序

创建 `myapp.c`:

```c
#include "db/sql/sql_driver.h"
#include "db/storage.h"
#include <stdio.h>

int main() {
    /* 打开数据库（:memory: 表示内存数据库） */
    void *db = kv_open(":memory:");
    if (!db) {
        fprintf(stderr, "Failed to open database\n");
        return 1;
    }

    /* 创建表 */
    int ret = execute_ddl(
        "CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT)",
        db
    );
    if (ret != 0) {
        fprintf(stderr, "Failed to create table: %d\n", ret);
        return 1;
    }

    /* 插入数据 */
    execute_sql("INSERT INTO users VALUES (1, 'Alice', 30)", db);
    execute_sql("INSERT INTO users VALUES (2, 'Bob', 25)", db);
    execute_sql("INSERT INTO users VALUES (3, 'Charlie', 35)", db);

    /* 查询数据 */
    QueryResult *result = execute_sql(
        "SELECT name, age FROM users WHERE age > 25 ORDER BY age",
        db
    );

    /* 遍历结果 */
    printf("Users older than 25:\n");
    for (int i = 0; i < result->nrows; i++) {
        char *name = result->rows[i * result->ncols + 0];
        char *age = result->rows[i * result->ncols + 1];
        printf("  %s, age %s\n", name, age);
    }

    /* 释放结果 */
    FreeQueryResult(result);

    /* 关闭数据库 */
    kv_close(db);

    printf("Success!\n");
    return 0;
}
```

编译并运行:

```bash
# 使用 pkg-config（如果配置了）
gcc myapp.c -o myapp $(pkg-config --cflags --libs book)

# 或者手动指定
gcc myapp.c -o myapp \
    -I./engineering/include \
    -L./build/engineering/src/db -lsql_engine \
    -L./build/engineering/src/storage -lkv_storage \
    -lpthread
```

---

## 2. 如何添加新算子

本节介绍如何添加一个新的执行算子。以添加 `Limit` 算子为例。

### 2.1 创建头文件

在 `engine/include/db/sql/` 下创建 `nodeLimit.h`:

```c
/**
 * nodeLimit.h - Limit 算子定义
 *
 * Limit 算子限制输出元组数量，用于分页查询
 */

#ifndef DB_SQL_NODE_LIMIT_H
#define DB_SQL_NODE_LIMIT_H

#include "db/sql/nodes/nodetags.h"
#include "db/sql/nodes/plannodes.h"

/* ====== 计划节点 ====== */

/**
 * Limit - Limit 计划节点
 *
 * 限制子节点的输出行数
 */
typedef struct Limit {
    Plan        plan;          /* 基类，包含通用属性 */

    int64       count;         /* 限制的行数（NULL 表示无限制） */
    int64       offset;        /* 跳过的行数 */
} Limit;

/* ====== 执行状态 ====== */

/**
 * LimitState - Limit 执行状态
 */
typedef struct LimitState {
    PlanState       ps;         /* 基类状态 */

    int64           offset;     /* 剩余要跳过的行数 */
    int64           count;      /* 剩余要输出的行数 */

    /* 元组计数 */
    int64           position;   /* 当前处理的元组位置 */
} LimitState;

/* ====== 公共 API ====== */

/**
 * ExecInitLimit - 初始化 Limit 算子
 *
 * @param plan    计划节点
 * @param estate  执行器状态
 * @param eflags  执行标志
 * @return        初始化后的执行状态
 */
PlanState *ExecInitLimit(Plan *plan, EState *estate, int eflags);

/**
 * ExecLimit - 获取下一个元组
 *
 * @param pstate  执行状态
 * @return        下一个元组，无更多元组返回 NULL
 */
TupleTableSlot *ExecLimit(PlanState *pstate);

/**
 * ExecEndLimit - 清理 Limit 算子
 *
 * @param node    执行状态
 */
void ExecEndLimit(LimitState *node);

/**
 * ExecReScanLimit - 重置 Limit 算子状态
 *
 * @param node    执行状态
 */
void ExecReScanLimit(LimitState *node);

#endif /* DB_SQL_NODE_LIMIT_H */
```

### 2.2 创建实现文件

在 `engine/src/db/sql/` 下创建 `nodeLimit.c`:

```c
/**
 * nodeLimit.c - Limit 算子实现
 *
 * 实现 LIMIT/OFFSET 子句的功能
 */

#include "db/sql/nodeLimit.h"
#include "db/sql/executor.h"
#include "db/sql/memctx.h"
#include <stdlib.h>

/* ====== ExecInitLimit ====== */

/**
 * 初始化 Limit 算子
 */
PlanState *ExecInitLimit(Plan *plan, EState *estate, int eflags)
{
    LimitState *state;
    Limit *limit = (Limit *)plan;

    /* 分配状态结构 */
    state = (LimitState *)palloc0(
        estate->es_query_cxt,
        sizeof(LimitState)
    );

    /* 设置节点类型 */
    state->ps.type = T_LimitState;

    /* 设置计划引用 */
    state->ps.plan = plan;
    state->ps.state = estate;

    /* 设置迭代函数 */
    state->ps.ExecProcNode = ExecLimit;

    /* 初始化子节点 */
    state->ps.lefttree = ExecInitNode(limit->plan.lefttree, estate, eflags);

    /* 初始化 Limit 参数 */
    state->offset = limit->offset;
    state->count = limit->count;
    state->position = 0;

    return &state->ps;
}

/* ====== ExecLimit ====== */

/**
 * 获取下一个元组（实现 Volcano 迭代器）
 */
TupleTableSlot *ExecLimit(PlanState *pstate)
{
    LimitState *node = (LimitState *)pstate;
    TupleTableSlot *slot;

    /* 检查是否已达到限制 */
    if (node->count == 0) {
        return NULL;
    }

    /* 从子节点获取元组 */
    while ((slot = ExecProcNode(node->ps.lefttree)) != NULL) {
        node->position++;

        /* 处理 OFFSET */
        if (node->position <= node->offset) {
            continue;
        }

        /* 输出元组 */
        node->count--;
        return slot;
    }

    /* 无更多元组 */
    return NULL;
}

/* ====== ExecEndLimit ====== */

/**
 * 清理 Limit 算子
 */
void ExecEndLimit(LimitState *node)
{
    if (!node) {
        return;
    }

    /* 清理子节点 */
    ExecEndNode(node->ps.lefttree);
}

/* ====== ExecReScanLimit ====== */

/**
 * 重置 Limit 算子状态
 */
void ExecReScanLimit(LimitState *node)
{
    Limit *limit = (Limit *)node->ps.plan;

    /* 重置计数 */
    node->position = 0;
    node->offset = limit->offset;
    node->count = limit->count;

    /* 重置子节点 */
    ExecReScan(node->ps.lefttree);
}
```

### 2.3 注册节点类型

在 `engine/include/db/sql/nodes/nodetags.h` 的 `NodeTag` 枚举中添加:

```c
typedef enum NodeTag {
    /* ... 现有类型 ... */

    /* Limit 节点 */
    T_Limit,
    T_LimitState,

    /* ... 其他类型 ... */
} NodeTag;
```

### 2.4 更新构建配置

在 `engine/src/db/sql/CMakeLists.txt` 中添加源文件:

```cmake
add_library(sql_engine STATIC
    # ... 现有源文件 ...
    nodeLimit.c
)
```

### 2.5 注册到执行器分发函数

在 `engine/src/db/sql/executor.c` 的 `ExecInitNode` 函数中添加:

```c
PlanState *ExecInitNode(Plan *plan, EState *estate, int eflags)
{
    PlanState *result;

    switch (nodeTag(plan)) {
        /* ... 现有 case ... */

        case T_Limit:
            result = ExecInitLimit(plan, estate, eflags);
            break;

        /* ... 其他 case ... */
    }

    return result;
}
```

---

## 3. 如何编写测试

### 3.1 测试文件结构

在 `engine/test/db/sql/` 下创建测试文件 `test_limit.cpp`:

```cpp
/**
 * test_limit.cpp - Limit 算子测试
 *
 * 使用 GoogleTest 框架测试 Limit 算子的功能
 */

#include <gtest/gtest.h>
#include "db/sql/nodeLimit.h"
#include "db/sql/executor.h"
#include "db/sql/memctx.h"
#include "db/storage.h"

extern "C" {

/* ====== 测试夹具 ====== */

class LimitTest : public ::testing::Test {
protected:
    EState *estate;           /* 执行器状态 */
    MemoryContext ctx;        /* 内存上下文 */
    void *db;                /* 数据库句柄 */
    Relation rel;            /* 测试表 */

    void SetUp() override {
        /* 创建内存上下文 */
        ctx = AllocSetContextCreate(
            NULL,
            "LimitTestContext",
            0,
            8192,
            8192
        );

        /* 创建执行器状态 */
        estate = CreateExecutorState();
        estate->es_query_cxt = ctx;

        /* 创建内存数据库 */
        db = kv_open(":memory:");
        ASSERT_NE(db, nullptr);

        /* 创建测试表 */
        int ret = execute_ddl(
            "CREATE TABLE t (id INT, val INT)",
            db
        );
        ASSERT_EQ(ret, 0);

        /* 插入测试数据 */
        for (int i = 1; i <= 100; i++) {
            char sql[128];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO t VALUES (%d, %d)", i, i * 10);
            execute_sql(sql, db);
        }
    }

    void TearDown() override {
        /* 清理执行器状态 */
        if (estate) {
            FreeExecutorState(estate);
        }

        /* 删除内存上下文 */
        if (ctx) {
            MemoryContextDelete(ctx);
        }

        /* 关闭数据库 */
        if (db) {
            kv_close(db);
        }
    }
};

/* ====== 测试用例 ====== */

/**
 * 测试 Limit 初始化
 */
TEST_F(LimitTest, InitLimit) {
    /* 创建 Limit 计划 */
    Limit limit_plan;
    limit_plan.plan.type = T_Limit;
    limit_plan.plan.lefttree = NULL;
    limit_plan.count = 10;
    limit_plan.offset = 0;

    /* 初始化 Limit 状态 */
    PlanState *ps = ExecInitLimit((Plan *)&limit_plan, estate, 0);
    ASSERT_NE(ps, nullptr);

    /* 验证状态 */
    LimitState *state = (LimitState *)ps;
    EXPECT_EQ(state->count, 10);
    EXPECT_EQ(state->offset, 0);
    EXPECT_EQ(state->position, 0);

    /* 清理 */
    ExecEndLimit(state);
}

/**
 * 测试 LIMIT 限制行数
 */
TEST_F(LimitTest, LimitCount) {
    /* 执行查询：LIMIT 5 */
    QueryResult *result = execute_sql(
        "SELECT * FROM t LIMIT 5",
        db
    );

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->nrows, 5);

    /* 验证数据 */
    for (int i = 0; i < result->nrows; i++) {
        int id = atoi(result->rows[i * result->ncols + 0]);
        EXPECT_EQ(id, i + 1);
    }

    FreeQueryResult(result);
}

/**
 * 测试 OFFSET 跳过行数
 */
TEST_F(LimitTest, LimitOffset) {
    /* 执行查询：OFFSET 95 */
    QueryResult *result = execute_sql(
        "SELECT * FROM t OFFSET 95",
        db
    );

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->nrows, 5);

    /* 验证数据 */
    for (int i = 0; i < result->nrows; i++) {
        int id = atoi(result->rows[i * result->ncols + 0]);
        EXPECT_EQ(id, 96 + i);
    }

    FreeQueryResult(result);
}

/**
 * 测试 LIMIT + OFFSET
 */
TEST_F(LimitTest, LimitOffset) {
    /* 执行查询：LIMIT 10 OFFSET 20 */
    QueryResult *result = execute_sql(
        "SELECT * FROM t LIMIT 10 OFFSET 20",
        db
    );

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->nrows, 10);

    /* 验证数据 */
    for (int i = 0; i < result->nrows; i++) {
        int id = atoi(result->rows[i * result->ncols + 0]);
        EXPECT_EQ(id, 21 + i);
    }

    FreeQueryResult(result);
}

/**
 * 测试 LIMIT 0
 */
TEST_F(LimitTest, LimitZero) {
    QueryResult *result = execute_sql(
        "SELECT * FROM t LIMIT 0",
        db
    );

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->nrows, 0);

    FreeQueryResult(result);
}

/**
 * 测试 LIMIT 超出数据量
 */
TEST_F(LimitTest, LimitExceed) {
    QueryResult *result = execute_sql(
        "SELECT * FROM t LIMIT 200",
        db
    );

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->nrows, 100);  /* 实际只有 100 行 */

    FreeQueryResult(result);
}

} /* extern "C" */
```

### 3.2 更新测试构建配置

在 `engine/test/db/sql/CMakeLists.txt` 中添加:

```cmake
add_project_test(test_limit test_limit.cpp)
```

### 3.3 运行测试

```bash
# 构建测试
cd build/engineering
cmake --build . --target test_limit

# 运行测试
./test/db/sql/test_limit

# 使用 ctest 运行
ctest -R test_limit --output-on-failure
```

---

## 4. 性能调优指南

### 4.1 内存调优

#### 4.1.1 work_mem 配置

`work_mem` 控制查询使用的内存上限，用于哈希连接、排序、聚合等操作。

```c
/* 获取/设置 work_mem */
extern int work_mem;  /* 默认 4MB */

/* 设置更大的 work_mem */
set_config_int("work_mem", "64MB");

/* 代码中设置 */
EState *estate = CreateExecutorState();
estate->es_work_mem = 64 * 1024 * 1024;  /* 64MB */
```

#### 4.1.2 内存上下文管理

```c
/* 在查询开始时重置上下文 */
MemoryContextReset(TopMemoryContext);

/* 监控内存使用 */
MemoryContextStats(TopMemoryContext);

/* 输出示例:
 * AllocSetContext: TopMemoryContext 0/8192 KB
 *   Child 0: CacheMemoryContext 0/8192 KB
 *   Child 1: MessageContext 0/8192 KB
 */
```

### 4.2 并行执行调优

#### 4.2.1 启用并行查询

```c
/* 配置并行 worker 数 */
set_config_int("max_parallel_workers", 4);

/* 单查询并行度 */
ParallelContext *pcxt = CreateParallelContext(4);

/* 启用 Gather 节点 */
Gather *gather = make_gather(nworkers);
```

#### 4.2.2 并行阈值配置

```c
/* 启用并行查询的最小成本阈值 */
set_config_double("parallel_threshold", 1000000);

/* 并行 worker 成本加成 */
set_config_double("parallel_worker_cost", 1.0);
```

### 4.3 查询优化技巧

#### 4.3.1 使用索引

```sql
-- 创建索引
CREATE INDEX idx_users_id ON users(id);
CREATE INDEX idx_users_age ON users(age);

-- 使用索引扫描
EXPLAIN SELECT * FROM users WHERE id > 100;

-- 输出:
-- Index Scan using idx_users_id on users
--   Index Cond: (id > 100)
```

#### 4.3.2 批量插入

```c
/* 批量插入而非单条插入 */
BatchInsert *batch = BeginBatchInsert(rel, 1000);

for (int i = 0; i < 10000; i++) {
    HeapTuple tuple = build_tuple(values);
    BatchInsertRow(batch, tuple);
}

EndBatchInsert(batch);
```

#### 4.3.3 预处理语句

```c
/* 使用预处理语句 */
PreparedStatement *stmt = PrepareStatement(
    "SELECT * FROM users WHERE age > ? AND name = ?"
);

for (int i = 0; i < queries_count; i++) {
    Datum params[2] = { queries[i].age, queries[i].name };
    QueryResult *result = ExecutePreparedStatement(stmt, params, 2);
    process_result(result);
    FreeQueryResult(result);
}

FreePreparedStatement(stmt);
```

### 4.4 EXPLAIN 诊断

#### 4.4.1 基本 EXPLAIN

```sql
EXPLAIN SELECT * FROM users WHERE age > 30;

                   QUERY PLAN
--------------------------------------------------
Seq Scan on users  (cost=0.00..35.50 rows=810 width=36)
  Filter: (age > 30)
```

#### 4.4.2 详细统计

```sql
EXPLAIN (ANALYZE, BUFFERS, TIMING) SELECT * FROM users WHERE age > 30;

                    QUERY PLAN
-----------------------------------------------------------------
Seq Scan on users  (cost=0.00..35.50 rows=810 width=36)
                   (actual time=0.015..0.156 rows=810 loops=1)
  Buffers: shared hit=35 read=0
  I/O Timings: read=0.000
Planning Time: 0.234 ms
Execution Time: 0.189 ms
```

#### 4.4.3 JSON 输出

```sql
EXPLAIN (FORMAT JSON) SELECT * FROM users WHERE id < 50;

[{
  "Plan": {
    "Node Type": "Seq Scan",
    "Relation Name": "users",
    "Filter": "(id < 50)",
    "Cost": {
      "Startup": 0.00,
      "Total": 35.50
    },
    "Rows": 810
  }
}]
```

---

## 5. 常见问题

### Q1: 如何调试执行计划？

**答**：使用 `EXPLAIN` 命令查看计划，使用 `EXPLAIN (ANALYZE, BUFFERS)` 查看实际执行情况。

```sql
-- 查看计划
EXPLAIN SELECT * FROM users JOIN orders ON users.id = orders.user_id;

-- 查看详细执行信息
EXPLAIN (ANALYZE, BUFFERS, COSTS, VERBOSE) SELECT * FROM users;
```

### Q2: 如何查看内存使用情况？

**答**：使用 `pg_stat_get_memory()` 或在代码中调用内存统计函数。

```c
#include "db/sql/memctx.h"

/* 打印所有内存上下文统计 */
MemoryContextStats(TopMemoryContext);

/* 获取特定上下文使用量 */
Size used = GetMemoryContextUsage(ctx);
printf("Used: %zu bytes\n", used);
```

### Q3: 如何启用 JIT 编译？

**答**：JIT 编译可以加速表达式求值，但需要 LLVM 库支持。

```c
#include "db/sql/jit.h"

/* 检查 JIT 是否可用 */
if (JITAvailable()) {
    /* 启用 JIT */
    JITConfig config = GetJITConfig();
    config.enabled = true;
    config.optimization_level = 3;
    SetJITConfig(config);
}
```

### Q4: 触发器不执行怎么办？

**答**：检查触发器是否正确创建并启用。

```sql
-- 查看表的触发器
SELECT tgname, tgtype, tgenabled
FROM pg_trigger
WHERE tgrelid = 'users'::regclass;

-- 启用触发器
ALTER TABLE users ENABLE TRIGGER trigger_name;
```

### Q5: 并行查询不生效？

**答**：检查并行配置和查询成本。

```sql
-- 查看并行设置
SHOW max_parallel_workers;
SHOW parallel_threshold;

-- 检查查询成本是否超过阈值
EXPLAIN SELECT * FROM large_table;  -- 成本应该较高
```

### Q6: 分区表插入失败？

**答**：检查分区路由和分区边界。

```sql
-- 查看分区信息
SELECT * FROM pg_partition_tree('partitioned_table');

-- 检查插入值是否在分区范围内
INSERT INTO partitioned_table VALUES ('2025-01-01', 'data');
-- 如果日期超出范围会失败
```

---

## 6. 示例代码

### 6.1 完整查询示例

```c
#include "db/sql/sql_driver.h"
#include "db/storage.h"
#include <stdio.h>
#include <stdlib.h>

void print_result(QueryResult *result) {
    if (!result) {
        printf("(no result)\n");
        return;
    }

    /* 打印列头 */
    for (int j = 0; j < result->ncols; j++) {
        if (j > 0) printf("\t");
        printf("%s", result->colnames[j]);
    }
    printf("\n");

    /* 打印数据行 */
    for (int i = 0; i < result->nrows; i++) {
        for (int j = 0; j < result->ncols; j++) {
            if (j > 0) printf("\t");
            printf("%s", result->rows[i * result->ncols + j] ?: "NULL");
        }
        printf("\n");
    }
}

int main() {
    void *db = kv_open(":memory:");
    if (!db) {
        fprintf(stderr, "Failed to open database\n");
        return 1;
    }

    /* 创建表 */
    execute_ddl(
        "CREATE TABLE users ("
        "  id INT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  email TEXT UNIQUE,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ")",
        db
    );

    /* 插入数据 */
    const char *inserts[] = {
        "INSERT INTO users (id, name, email) VALUES (1, 'Alice', 'alice@example.com')",
        "INSERT INTO users (id, name, email) VALUES (2, 'Bob', 'bob@example.com')",
        "INSERT INTO users (id, name, email) VALUES (3, 'Charlie', 'charlie@example.com')",
        "INSERT INTO users (id, name, email) VALUES (4, 'Diana', 'diana@example.com')",
        "INSERT INTO users (id, name, email) VALUES (5, 'Eve', 'eve@example.com')"
    };

    for (int i = 0; i < 5; i++) {
        QueryResult *r = execute_sql(inserts[i], db);
        if (r) FreeQueryResult(r);
    }

    /* 查询所有用户 */
    printf("=== 所有用户 ===\n");
    QueryResult *result = execute_sql("SELECT * FROM users", db);
    print_result(result);
    FreeQueryResult(result);

    /* 条件查询 */
    printf("\n=== ID > 2 的用户 ===\n");
    result = execute_sql("SELECT name, email FROM users WHERE id > 2", db);
    print_result(result);
    FreeQueryResult(result);

    /* 排序查询 */
    printf("\n=== 按名字排序 ===\n");
    result = execute_sql("SELECT * FROM users ORDER BY name DESC", db);
    print_result(result);
    FreeQueryResult(result);

    /* 聚合查询 */
    printf("\n=== 统计信息 ===\n");
    result = execute_sql("SELECT COUNT(*), MIN(id), MAX(id) FROM users", db);
    print_result(result);
    FreeQueryResult(result);

    kv_close(db);
    printf("\nDone!\n");
    return 0;
}
```

### 6.2 事务处理示例

```c
#include "db/sql/sql_driver.h"
#include "db/storage.h"
#include <stdio.h>

int main() {
    void *db = kv_open("test.db");

    /* 开始事务 */
    execute_sql("BEGIN", db);

    /* 创建表 */
    execute_ddl(
        "CREATE TABLE accounts ("
        "  id INT PRIMARY KEY,"
        "  name TEXT,"
        "  balance DECIMAL(10,2)"
        ")",
        db
    );

    /* 插入初始数据 */
    execute_sql("INSERT INTO accounts VALUES (1, 'Alice', 1000.00)", db);
    execute_sql("INSERT INTO accounts VALUES (2, 'Bob', 500.00)", db);

    /* 转账操作 */
    printf("转账: Bob -> Alice 100.00\n");
    execute_sql("UPDATE accounts SET balance = balance - 100 WHERE id = 2", db);
    execute_sql("UPDATE accounts SET balance = balance + 100 WHERE id = 1", db);

    /* 提交事务 */
    QueryResult *result = execute_sql("COMMIT", db);
    if (result) {
        printf("事务提交成功!\n");
        FreeQueryResult(result);
    }

    /* 查看结果 */
    result = execute_sql("SELECT * FROM accounts", db);
    for (int i = 0; i < result->nrows; i++) {
        printf("Account %s: %s\n",
               result->rows[i * result->ncols + 0],
               result->rows[i * result->ncols + 2]);
    }
    FreeQueryResult(result);

    kv_close(db);
    return 0;
}
```

### 6.3 预处理语句示例

```c
#include "db/sql/prepared.h"
#include "db/storage.h"
#include <stdio.h>

int main() {
    void *db = kv_open(":memory:");

    /* 准备语句 */
    PreparedStatement *stmt = PrepareStatement(
        db,
        "INSERT INTO users (id, name, age) VALUES (?, ?, ?)"
    );

    /* 批量插入 */
    const char *names[] = {"Alice", "Bob", "Charlie", "Diana"};
    int ages[] = {25, 30, 35, 28};

    for (int i = 0; i < 4; i++) {
        Datum params[3] = {
            Int64GetDatum(i + 1),
            CStringGetDatum(names[i]),
            Int32GetDatum(ages[i])
        };
        ExecutePreparedStatement(stmt, params, 3);
    }

    /* 查询 */
    QueryResult *result = ExecutePreparedStatement(
        stmt,
        "SELECT * FROM users WHERE age > ?",
        (Datum[]){Int32GetDatum(27)},
        1
    );

    for (int i = 0; i < result->nrows; i++) {
        printf("%s, age %s\n",
               result->rows[i * result->ncols + 1],
               result->rows[i * result->ncols + 2]);
    }

    FreeQueryResult(result);
    FreePreparedStatement(stmt);
    kv_close(db);

    return 0;
}
```

---

## 附录 A: 错误代码

| 错误代码 | 说明 |
|---------|------|
| 0 | 成功 |
| -1 | 一般错误 |
| -2 | 语法错误 |
| -3 | 表不存在 |
| -4 | 列不存在 |
| -5 | 类型不匹配 |
| -6 | 约束违反 |
| -7 | 内存不足 |
| -8 | IO 错误 |

---

## 附录 B: 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `work_mem` | 4MB | 查询工作内存 |
| `max_parallel_workers` | 4 | 最大并行 worker 数 |
| `parallel_threshold` | 1000000 | 并行查询成本阈值 |
| `enable_hashjoin` | true | 启用哈希连接 |
| `enable_nestloop` | true | 启用嵌套循环连接 |
| `enable_seqscan` | true | 启用顺序扫描 |
| `enable_indexscan` | true | 启用索引扫描 |
| `jit_enabled` | false | 启用 JIT 编译 |

---

*手册版本：v5.0*
*更新日期：2026-07-15*
