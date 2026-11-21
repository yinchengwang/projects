> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 集成测试计划

本文档定义 Phase 1 的集成测试计划，覆盖核心模块间的交互测试。

---

## 测试总览

| 测试 ID | 模块组合 | 优先级 | 状态 |
|---------|----------|--------|------|
| INT-001 | Buffer Pool + Heap | P0 | ❌ |
| INT-002 | Buffer Pool + BTree | P0 | ❌ |
| INT-003 | Buffer Pool + WAL | P0 | ❌ |
| INT-004 | BTree + MVCC | P0 | ❌ |
| INT-005 | SQL Executor SELECT | P0 | ❌ |
| INT-006 | 端到端测试 | P0 | ❌ |

---

## INT-001：Buffer Pool + Heap 读写测试

### 测试目标

验证 Buffer Pool 与 Heap Access Method 的集成正确性。

### 测试场景

| 场景 | 输入 | 预期输出 |
|------|------|----------|
| 页面分配 | 连续分配多个页面 | 页面 ID 连续递增 |
| 页面读取 | 写入后读取同一页面 | 数据一致 |
| 页面驱逐 | 缓存满后驱逐旧页面 | 脏页正确刷盘 |
| 页面释放 | 释放页面后重新分配 | 可正常分配 |

### 测试代码位置

`engine/test/db/test_buf_heap_integration.cpp`

### 验收标准

- [ ] 所有测试用例通过
- [ ] 无内存泄漏（Valgrind）
- [ ] 无数据竞争（TSan）

---

## INT-002：Buffer Pool + BTree 读写测试

### 测试目标

验证 Buffer Pool 与 BTree 索引的集成正确性。

### 测试场景

| 场景 | 输入 | 预期输出 |
|------|------|----------|
| BTree 创建 | 创建新索引 | 根节点正确分配 |
| 键值插入 | 插入 1000 条数据 | BTree 结构正确 |
| 键值查找 | 查找已插入的键 | 返回正确值 |
| 键值删除 | 删除已存在的键 | BTree 结构正确 |
| 范围查询 | 查询区间键 | 返回正确结果集 |

### 测试代码位置

`engine/test/db/test_buf_btree_integration.cpp`

### 验收标准

- [ ] 所有测试用例通过
- [ ] 无悬垂指针（ASAN）
- [ ] BTree 节点分裂/合并正确

---

## INT-003：Buffer Pool + WAL 持久化测试

### 测试目标

验证 Buffer Pool 与 WAL 日志的持久化协作。

### 测试场景

| 场景 | 输入 | 预期输出 |
|------|------|----------|
| 脏页标记 | 修改页面后 | 页面标记为脏 |
| 日志写入 | 脏页刷盘前 | WAL 日志已写入 |
| 崩溃恢复 | 模拟崩溃后重启 | 数据完整恢复 |
| 检查点 | 检查点生成 | 状态一致 |

### 测试代码位置

`engine/test/db/test_buf_wal_integration.cpp`

### 验收标准

- [ ] 崩溃恢复后数据完整
- [ ] WAL 日志与页面状态一致
- [ ] 检查点生成正确

---

## INT-004：BTree + MVCC 可见性测试

### 测试目标

验证 BTree 索引与 MVCC 多版本控制的可见性判断。

### 测试场景

| 场景 | 输入 | 预期输出 |
|------|------|----------|
| 读已提交 | 事务 A 读取 | 只能看到已提交版本 |
| 可重复读 | 事务 A 内多次读取 | 同一快照 |
| 并发写入 | 事务 A/B 并发更新同一行 | 正确处理冲突 |
| 索引可见性 | 索引扫描时 | 正确过滤不可见版本 |

### 测试代码位置

`engine/test/db/test_btree_mvcc_integration.cpp`

### 验收标准

- [ ] 可见性判断正确
- [ ] 版本链完整
- [ ] 无脏读/不可重复读

---

## INT-005：SQL Executor SELECT 测试

### 测试目标

验证 SQL Executor 的 SELECT 查询功能。

### 测试场景

| 场景 | SQL | 预期输出 |
|------|-----|----------|
| 全表扫描 | SELECT * FROM users | 返回所有行 |
| 条件查询 | SELECT * FROM users WHERE id = 1 | 返回匹配行 |
| 投影 | SELECT name FROM users | 只返回 name 列 |
| 空结果 | SELECT * FROM users WHERE id = 9999 | 返回空结果集 |

### 测试代码位置

`engine/test/db/test_sql_executor_select.cpp`

### 验收标准

- [ ] SELECT * FROM table WHERE column = value 可执行
- [ ] 结果正确
- [ ] 性能可接受

---

## INT-006：端到端测试

### 测试目标

验证完整的数据库操作流程。

### 测试场景

```
1. 创建表：CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(100))
2. 插入数据：INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob')
3. 查询数据：SELECT * FROM users WHERE id = 1
4. 更新数据：UPDATE users SET name = 'Alicia' WHERE id = 1
5. 删除数据：DELETE FROM users WHERE id = 2
6. 验证结果：SELECT * FROM users
```

### 测试代码位置

`engine/test/db/test_e2e.cpp`

### 验收标准

- [ ] 全流程可执行
- [ ] 数据正确性验证通过
- [ ] 无内存泄漏
- [ ] 无数据竞争

---

## 测试环境

### 编译配置

```bash
# Debug 模式（启用 ASAN/TSan）
cmake -B build/engineering -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,thread" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,thread"

# Release 模式
cmake -B build/engineering -DCMAKE_BUILD_TYPE=Release
```

### 运行测试

```bash
# 运行所有集成测试
ctest --test-dir build/engineering -R "integration" --output-on-failure

# 运行单个测试
./build/engineering/test/db/test_buf_heap_integration

# Valgrind 检测
valgrind --leak-check=full ./build/engineering/test/db/test_buf_heap_integration
```

---

## 测试报告模板

```markdown
### INT-XXX 测试报告

**测试时间**: YYYY-MM-DD HH:MM
**测试人员**: XXX
**测试结果**: ✅ 通过 / ❌ 失败

#### 通过的测试用例
- [ ] <场景1>
- [ ] <场景2>

#### 失败的测试用例
- [ ] <场景3>: <失败原因>

#### 性能数据
- 平均响应时间: XXX ms
- 吞吐量: XXX qps

#### 问题与改进
TODO
```

---

*创建时间: 2026-07-12*
*最后更新: 2026-07-12*
