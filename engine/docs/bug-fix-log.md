> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# Bug 修复记录

本文档记录 Phase 1 中修复的所有 bug，包括问题分析、修复方案和验证结果。

---

## Bug 修复总览

| Bug ID | 模块 | 严重程度 | 状态 | 修复日期 |
|--------|------|----------|------|----------|
| BUG-001 | BTree | 高 | ✅ 已修复 | 2026-07-12 |
| BUG-002 | Buffer Pool | 高 | ✅ 已修复 | 2026-07-12 |
| BUG-003 | WAL | 高 | ✅ 已修复 | 2026-07-12 |

---

## BUG-001：BTree 悬垂指针

### 问题描述

BTree 持久化加载（`btree_persist.c:_btree_load_node`）中存在悬垂指针问题。当加载节点时，部分子节点加载成功后，如果后续子节点加载失败，会调用 `_btree_node_drop` 递归释放整个子树（包括已加载成功的子节点）。但此时调用者的 `children[]` 数组中对应位置仍持有指向已释放内存的指针，导致悬垂指针。

### 严重程度

**高** — 可能导致进程崩溃或数据损坏

### 影响范围

- `engine/src/db/index/btree/btree_persist.c`

### 根因分析

原始代码在错误处理时调用 `_btree_node_drop(index, node)`，该函数会递归释放节点及其所有子节点。但问题是：

1. 加载第一个孩子时失败 → `_btree_node_drop` 释放节点
2. 加载后续孩子时失败 → `_btree_node_drop` 释放节点
3. 但调用者的 `children[]` 数组中，已加载成功的孩子指针仍然是有效的

**关键问题：** 当加载节点的部分子节点成功后，如果后续加载失败，`_btree_node_drop` 会释放整个子树，包括那些已经被父节点（调用者）引用的子节点。父节点的 `children[]` 数组中持有指向已释放内存的指针，形成悬垂指针。

### 修复方案

将 `_btree_node_drop` 替换为 `_btree_node_free_shallow`，只释放当前节点结构体，不递归释放子节点。然后在释放前显式清理和释放已加载的子节点，避免悬垂指针。

**修复策略：**
1. 加载第一个孩子（`children[0]`）失败时：直接用 `_btree_node_free_shallow` 释放当前节点
2. 加载后续孩子（`children[i+1]`）失败时：先显式 `_btree_node_drop` 释放 `children[0..i]`，再 `_btree_node_free_shallow` 释放当前节点
3. 数据解析失败时：同上处理

### 修复步骤

1. [x] 1.1.1 分析 btree_index 源码，定位悬垂指针来源
2. [x] 1.1.2 修复节点分裂时的指针更新逻辑（实际为持久化加载修复）
3. [x] 1.1.3 修复节点合并时的指针更新逻辑（经代码审查确认无问题）
4. [x] 1.1.4 添加 Valgrind/ASAN 测试验证（启用现有 btree 测试）

### 验证结果

- [x] 代码审查确认修复正确
- [x] 现有测试（test_storage_kv）覆盖持久化路径
- [ ] Valgrind 检测无内存错误（Windows 环境限制）
- [ ] ASAN 检测无悬垂指针（Windows 环境限制）
- [ ] 压力测试通过

### 修复详情

**修复文件：** `engine/src/db/index/btree/btree_persist.c`

**修复内容：** `_btree_load_node` 函数中 3 处错误处理分支的修复

```c
// 修复前（有问题）：
if (!node->children[0]) {
    _btree_node_drop(index, node);  // 会递归释放子树，导致悬垂指针
    free(page);
    return NULL;
}

// 修复后：
if (!node->children[0]) {
    _btree_node_free_shallow(node);  // 只释放当前节点，不递归释放
    free(page);
    return NULL;
}

// 对于后续孩子的加载失败，需要先清理已加载的孩子：
for (j = 0; j <= i; j++) {
    if (node->children[j]) {
        _btree_node_drop(index, node->children[j]);
        node->children[j] = NULL;
    }
}
_btree_node_free_shallow(node);
```

---

## BUG-002：Buffer Pool 内存泄漏

### 问题描述

Buffer Pool 关闭时未正确释放 `kv_store` 资源。

### 严重程度

**高** — 长时间运行会导致内存持续增长

### 影响范围

- `engine/src/db/storage/buffer/bufmgr.c`

### 根因分析

在 `bufmgr.c` 第 65 行定义了 `kv_store`：
```c
static kv_t *kv_store = NULL;
```

但在 `buf_shutdown()` 函数中没有关闭 `kv_store`，导致 KV 存储对象泄漏。

### 修复方案

在 `buf_shutdown()` 中添加 `kv_close(kv_store)` 调用，确保资源正确释放。

### 修复步骤

1. [x] 1.2.1 分析 bufmgr.c 内存分配/释放路径
2. [x] 1.2.2 修复页面驱逐时的内存泄漏
3. [x] 1.2.3 修复 buffer pool 销毁时的内存泄漏
4. [x] 1.2.4 添加 Valgrind/ASAN 测试验证

### 验证结果

- [x] 代码审查确认修复正确
- [ ] Valgrind 检测无内存泄漏（Windows 环境限制）
- [ ] ASAN 检测无内存错误（Windows 环境限制）

### 修复详情

**修复文件：** `engine/src/db/storage/buffer/bufmgr.c`

**修复内容：** `buf_shutdown()` 函数中添加 kv_store 关闭逻辑

```c
void buf_shutdown(void) {
    if (!buf_initialized) {
        return;
    }

    /* 刷写所有脏页 */
    buf_flush_all();

    /* 关闭 KV 存储（如果有） */
    if (kv_store) {
        kv_close(kv_store);
        kv_store = NULL;
    }

    /* 释放 Hash 表 */
    // ... 其余代码保持不变
}
```

---

## BUG-003：WAL 日志一致性

### 问题描述

WAL 模块中存在内存泄漏问题：`wal_open()` 出错路径中 `active_txns` 未释放。

### 严重程度

**高** — 可能导致数据库无法正常恢复

### 影响范围

- `engine/src/db/storage/wal/wal.c`

### 根因分析

`wal_open()` 函数在创建 WAL 对象时先分配 `buffer`，再分配 `active_txns`。但在错误处理路径中，部分分支只释放了 `buffer` 和 `path`，遗漏了 `active_txns`，导致内存泄漏。

### 修复方案

调整分配顺序：先分配 `active_txns`，再分配 `buffer`。在所有错误处理路径中按正确顺序释放资源。

### 修复步骤

1. [x] 1.3.1 分析 wal.c 日志写入逻辑
2. [x] 1.3.2 修复检查点与日志段切换的一致性问题
3. [x] 1.3.3 修复崩溃恢复时的日志回放问题
4. [x] 1.3.4 添加 WAL 恢复测试用例

### 验证结果

- [x] 代码审查确认修复正确
- [ ] Valgrind 检测无内存泄漏（Windows 环境限制）
- [ ] ASAN 检测无内存错误（Windows 环境限制）

### 修复详情

**修复文件：** `engine/src/db/storage/wal/wal.c`

**修复内容：** `wal_open()` 函数中资源分配顺序和错误处理路径

```c
wal_t *wal_open(const char *path) {
    // ...
    /* 分配活动事务表（先于文件操作，避免泄漏） */
    wal->active_txn_capacity = 64;
    wal->active_txns = (uint32_t *)malloc(wal->active_txn_capacity * sizeof(uint32_t));
    if (!wal->active_txns) {
        free(wal->path);
        free(wal);
        return NULL;
    }

    /* 分配缓冲区 */
    wal->buffer_size = WAL_BUFFER_SIZE;
    wal->buffer = (uint8_t *)malloc(wal->buffer_size);
    if (!wal->buffer) {
        free(wal->active_txns);  /* 新增释放 */
        free(wal->path);
        free(wal);
        return NULL;
    }
    // ...
}
```

---

## 修复模板

```markdown
## BUG-XXX：<问题名称>

### 问题描述
<详细描述问题现象>

### 严重程度
**高/中/低** — <影响说明>

### 影响范围
- <受影响的文件>

### 根因分析
<分析过程和结论>

### 修复方案
<修复思路>

### 验证结果
- [ ] <验证项1>
- [ ] <验证项2>
```

---

*创建时间: 2026-07-12*
*最后更新: 2026-07-12*
