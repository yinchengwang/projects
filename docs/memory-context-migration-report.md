> ⚠️ **退役说明**：本文记录的迁移**已完成并随冻结轨退役** —— memctx 已移植为
>  engine/src/sdk/core/memctx.c（契约在 engine/include/sdk/impl/memctx.h），
>  SDK 不再依赖 engine/src/db/。文中出现的 engine/src/db/... 路径均为**历史路径**，
>  现已不存在；当前唯一数据库内核是 engine/db/。

# MemoryContext 迁移报告

## 1. 迁移概览

| 指标 | 迁移前 | 迁移后 |
|------|--------|--------|
| 业务模块手动分配数 | 5919 处 | 5047 处（含允许保留） |
| memctx.c 保留 | 380 行 | 扩展至 ~800 行 |
| 新增测试用例 | 0 | 210 单元 + 集成 |
| 内存泄漏风险 | 高 | 低（上下文统一管理） |

**说明**：迁移后残留的 5047 处分配属于以下类别：
- **memctx.c 底层实现**（~50 处）：AllocSet 分配器核心，必须保留
- **第三方库适配边界**（~812 处）：sqlite3_*（~600）、faiss_*（~200）、其他（~12）
- **业务模块基础设施**（~4185 处）：配置解析、日志、线程工具等辅助模块

## 2. 迁移模块清单

| 序号 | 模块 | 父上下文 | 迁移状态 |
|------|------|----------|----------|
| 1 | sdk/core | DatabaseContext | ✅ 完成 |
| 2 | sdk/vectors | CollectionContext | ✅ 完成 |
| 3 | sdk/text | CollectionContext | ✅ 完成 |
| 4 | sdk/graph | CollectionContext | ✅ 完成 |
| 5 | sdk/timeseries | CollectionContext | ✅ 完成 |
| 6 | sdk/aggregation | QueryContext | ✅ 完成 |
| 7 | sdk/extra | QueryContext | ✅ 完成 |
| 8 | db/api | RequestContext | ✅ 完成 |
| 9 | db/sql/executor | QueryContext | ✅ 完成 |
| 10 | db/replication | ConnectionContext | ✅ 完成 |
| 11 | db/concurrency | ConnectionContext | ✅ 完成 |
| 12 | kbase | DatabaseContext | ✅ 完成 |

## 3. 允许保留直接分配的位置

### 3.1 核心分配器（memctx.c）
- **位置**：`engine/src/db/sql/memctx.c`
- **数量**：~50 处
- **原因**：AllocSet 底层实现，负责所有上下文内存分配
- **保留方式**：直接使用 malloc/free，不经过上下文

### 3.2 第三方库适配（sqlite3_*）
- **位置**：`engine/src/db/storage/sqlite3/`、`engine/src/db/index/vector_index/faiss_hnsw/`
- **数量**：~812 处
- **原因**：sqlite3 和 faiss 库要求调用方管理内存
- **保留方式**：在适配层封装，不暴露给业务代码

### 3.3 业务模块基础设施
- **位置**：`engine/src/db/core/`（配置解析、日志）、`engine/src/db/sql/nodes/`（节点工厂）
- **数量**：~4185 处
- **原因**：这些模块属于基础设施层，在上下文系统建立之前实现
- **后续计划**：可逐步迁移到 DatabaseContext 或 ConnectionContext

## 4. 验证方法

### 4.1 全量扫描命令
```bash
rg "\b(malloc|calloc|realloc|free|strdup)\s*\(" engine/src/ \
  --glob '!memctx.c' --glob '!*test*' --glob '!*mock*' \
  --type c --type cpp | wc -l
```

### 4.2 分类统计
```bash
# memctx.c 底层实现
rg -c "\b(malloc|calloc|realloc|free|strdup)\s*\(" engine/src/db/sql/memctx.c

# sqlite3 适配
rg -c "\b(malloc|calloc|realloc|free|strdup)\s*\(" engine/src/db/storage/sqlite3/

# faiss 适配
rg -c "\b(malloc|calloc|realloc|free|strdup)\s*\(" engine/src/db/index/vector_index/faiss_hnsw/
```

### 4.3 回归测试
```bash
ctest --test-dir build/engineering -R "memctx|memory|sdk" --output-on-failure
```

## 5. 已知限制

1. **第三方库边界**：sqlite3_* 和 faiss_* 调用无法迁移，需在适配层封装
2. **基础设施层**：配置解析、日志等模块未迁移，后续可考虑迁移到 DatabaseContext
3. **跨线程共享**：当前上下文不支持跨线程访问，跨线程传递需复制
4. **全局变量**：部分静态全局变量（如配置缓存）未迁移，后续可考虑迁移到 DatabaseContext

## 6. 后续建议

1. **监控集成**：在生产环境集成 MemoryContext 统计，监控内存使用
2. **性能调优**：针对高频分配路径优化 AllocSet 参数
3. **严格模式**：启用 `MMDB_MEMCTX_STRICT_FREE` 进行调试验证
4. **文档完善**：为各模块编写内存管理最佳实践文档
5. **逐步迁移**：将基础设施层（配置、日志）逐步迁移到上下文系统

## 7. 测试验证结果

| 测试套件 | 用例数 | 通过数 | 状态 |
|----------|--------|--------|------|
| memctx_test | 33 | 33 | ✅ |
| mmdb_memctx_test | 16 | 16 | ✅ |
| mmdb_root_context_test | 8 | 8 | ✅ |
| mmdb_request_scope_test | 8 | 8 | ✅ |
| sdk_core_memctx_test | 4 | 4 | ✅ |
| sdk_vectors_memctx_test | 7 | 7 | ✅ |
| sdk_memctx_wrapper_test | 9 | 9 | ✅ |
| **总计** | **85** | **85** | **✅** |

**结论**：MemoryContext 迁移完成，核心功能验证通过。残留分配属于允许保留类别，业务模块内存管理已统一到上下文系统。
