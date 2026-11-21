> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 存储子系统设计审查报告

**审查日期**：2026-07-15
**审查者**：Task 2.2 审查者
**审查范围**：

- `engine/include/db/index/storage_backend.h`
- `engine/include/db/index/heap/heap_vector_store.h`
- `engine/include/db/index/vector_ref.h`
- `engine/src/db/index/storage_backend/storage_backend_common.c`
- `engine/src/db/index/storage_backend/storage_memory.c`
- `engine/src/db/index/storage_backend/storage_page_file.c`
- `engine/src/db/index/storage_backend/storage_mmap.c`
- `engine/src/db/index/heap/heap_vector_store.c`
- `engine/test/db/index/storage_backend/storage_backend_test.cpp`
- `engine/test/db/index/heap/heap_vector_store_test.cpp`

**审查维度**：API 设计、错误处理、扩展性

---

## 一、整体评价

| 维度 | 评级 | 说明 |
|------|------|------|
| API 设计 | **B+** | 接口清晰、文档较完整，但部分约束文档与代码不一致，宏常量化与函数语义有混用 |
| 错误处理 | **B** | 边界条件基本覆盖，但 mmap 后端存在数据丢失/不一致的潜在路径，page_file 部分路径未持久化 page_count |
| 扩展性 | **A-** | ops 虚表 + 类型枚举 + 工厂分发，插件化结构清晰，新增后端成本低 |

整体而言，子系统结构合理、模块边界清晰，符合 `db/index` 子系统"接口 + 实现"的风格；测试覆盖较为完整。但 mmap 后端存在若干稳健性问题，heap_vector_store 在多次读改写时存在放大 I/O 的效率隐患，需在后续迭代中修复。

---

## 二、API 设计

### 2.1 优点

1. **不透明指针 + ops 虚表**：典型的 C 多态实现。`storage_backend_t` 暴露 `type / ctx / page_size / ops` 四个字段，调用方只看接口不看实现，方便替换后端实现。
2. **统一的页面抽象**：复用 `db/page.h` 的 `page_t` 不透明指针，避免在存储层定义第二套页面结构。
3. **构造/析构分离**：提供 `storage_backend_create` 工厂和专用 `storage_memory_create` / `storage_page_file_create` / `storage_mmap_create`，分层清晰。
4. **NULL 安全**：所有公共 API 都对 `NULL` 入参做了早返回；`storage_backend_destroy(NULL)` 是 no-op；查询接口（`heap_vector_count(NULL)` 等）返回 0，便于调用方链路安全。
5. **类型名称辅助函数**：`storage_backend_type_name()` 提供日志友好字符串，避免到处散落 magic 数字。
6. **`vector_ref_t` 设计紧凑**：12 字节（4+4+4）定长结构，可直接作为索引 Payload 嵌入 B+Tree 等结构。提供了 `vector_ref_is_valid` / `vector_ref_equal` / `vector_ref_make_invalid` 内联辅助函数。

### 2.2 问题

#### 问题 A1：`page_size` 与 `backend->page_size` 的一致性约束过强，且文档未明示

`heap_vector_store.c:166`：

```c
if (page_size != config->backend->page_size) {
    return NULL;
}
```

`heap_vector_store.h:99-103` 文档仅说"若非 0，必须与 `backend->page_size` 保持一致（避免越界 IO）"，但代码行为是"非 0 时一致才创建"。当 `config->page_size == 0` 时会替换为默认 `HEAP_VECTOR_DEFAULT_PAGE_SIZE = 8192`，然后**再**与 backend 比较 — 这意味着 backend 必须恰好配置为 8192，否则即使用户传 0 也创建失败。

**建议**：
- 在文档明确指出"backend 的 `page_size` 必须是 `HEAP_VECTOR_DEFAULT_PAGE_SIZE` 或与 `config->page_size` 保持一致"。
- 或者放宽约束为"若 `config->page_size == 0`，继承 `backend->page_size`"。这与现有 ops 接口"以 backend 为准"的语义更契合。

#### 问题 A2：`STORAGE_BACKEND_PAGE_FILE` 与 `STORAGE_BACKEND_FAISS` 类型名不一致

`storage_backend.h:45` 注释说明 FAISS 是"只读"，但没有 `storage_faiss_create` 接口。`storage_backend_create` 对 FAISS 类型直接返回 NULL。这意味着 `type` 枚举值定义了但无法构造。

**建议**：
- 删除 `STORAGE_BACKEND_FAISS` 直到真正实现，或
- 在 `storage_backend_create` 中显式返回 `NULL` 并设置 errno / 日志提示"尚未实现"。

#### 问题 A3：`storage_backend_destroy` 中 `ops->close` 返回值被忽略

`storage_backend_common.c:24-26`：

```c
if (backend->ops != NULL && backend->ops->close != NULL) {
    backend->ops->close(backend->ctx);
}
```

`close` 是可能失败的（例如 mmap 的 msync），但销毁路径只关心资源释放，不在意返回值。建议在文档中明确"destroy 不报告 close 错误，调用方应提前 sync"。

#### 问题 A4：`heap_vector_get_batch` 失败时未指明"哪个索引失败"

接口文档："非 0 表示首个失败位置已停止"，但实现和测试都没有暴露"失败位置"。调用方拿到 -1 后无法判断已经成功处理了多少向量。

**建议**：增加 `out_first_failure_index` 可选参数，或返回值改为更具体的错误码。

#### 问题 A5：`heap_vector_capacity_per_page` 返回 `int` 而非 `size_t`

`heap_vector_store.h:204`。内部 `vectors_per_page` 是 `int`，且函数对 `store == NULL` 返回 0。返回类型用 `int` 会与 `calc_vectors_per_page` 内部 `int n` 对齐，但与"字节大小"概念混淆。`heap_vector_count` 用 `int32_t`、`heap_vector_dims` 也用 `int32_t`，三者类型不一致，建议统一为 `int32_t` 或 `size_t`。

#### 问题 A6：`vector_ref_t.offset` 与 `length` 截断风险

`heap_vector_store.c:258-259`：

```c
ref.offset = (uint32_t)data_offset;
ref.length = (uint32_t)vector_bytes;
```

在 `dims > INT32_MAX/4`（极不合理但理论可能）时 `vector_bytes` 会溢出；截断为 `uint32_t` 是静默的。建议至少在 `calc_vectors_per_page` 增加 `dims * 4 > UINT32_MAX` 的检查，并在插入路径中加 assert。

---

## 三、错误处理

### 3.1 优点

1. **统一的错误码约定**：0 成功，非 0 失败，符合 C 标准库惯例。
2. **NULL 指针全面检查**：所有公共 API 都对入参做了非空校验。
3. **`vector_ref_is_valid` 哨兵设计**：通过 `INVALID_PAGE_ID = -1` 让无效引用天然可判别。
4. **`heap_vector_get` 校验严格**：长度、偏移、槽位对齐、slot_count 都做了检查（`heap_vector_store.c:296-330`），能挡掉大部分损坏引用。

### 3.2 问题

#### 问题 E1（**严重**）：`mmap_alloc_page` 在 truncate 失败后未回滚文件长度

`storage_mmap.c:349-359`：

```c
if (mmap_platform_truncate(&mctx->plat, (uint64_t)new_size) != 0) {
    mctx->page_count--;
    return INVALID_PAGE_ID;
}

char *new_addr = (char *)mmap_platform_map(&mctx->plat, new_size);
if (new_addr == NULL) {
    /* 映射失败时尝试回滚扩展 */
    mctx->page_count--;
    return INVALID_PAGE_ID;
}
```

**问题点**：
- truncate 成功后**新地址会覆盖旧映射**（`mmap_platform_map` 内部会先 `UnmapViewOfFile`），失败时旧映射可能已经被解除，导致原有的 `mctx->map_addr` 指向已解除的虚拟内存。
- 注释说"回滚扩展"，但代码只是 `--page_count`，并没有 `ftruncate(plat, old_size)`。
- truncate 失败后，已 unmap 但未 map 成功的话，`mctx->map_addr` 仍指向已解除的旧地址 —— 后续 read/write 会触发 SIGBUS / Windows 访问违例。

**建议**：
- 重新组织代码顺序：先 mmap 新地址成功后再 truncate / 替换指针；
- 失败路径需 `mctx->map_addr = NULL; mctx->mapped_size = 0;` 防止悬空。

#### 问题 E2（**严重**）：mmap 文件大小恢复时的"向下对齐丢弃尾部"

`storage_mmap.c:585-590`：

```c
if (file_size > 0) {
    /* 向下对齐到 page_size 边界 */
    size_t aligned_size = (size_t)file_size;
    size_t rem = aligned_size % page_size;
    if (rem != 0) {
        aligned_size -= rem;
    }
```

如果文件恰好是 `page_size + 5` 字节（损坏的尾部），启动时 `page_count` 会少 1，尾部 5 字节被静默丢弃。虽然实际中很少见，但作为持久化引擎这不应该被忽略。

**建议**：
- 检测到非页对齐时记录 warning；
- 或者 truncate 到对齐大小（已经做了），但应在文档中说明"启动时会修正非对齐尾部"。

#### 问题 E3（中等）：`pagefile_alloc_page` 不预分配文件空间

`storage_page_file.c:32-40`：

```c
static page_id_t pagefile_alloc_page(void *ctx)
{
    page_file_ctx_t *pctx = (page_file_ctx_t *)ctx;
    if (pctx == NULL) {
        return INVALID_PAGE_ID;
    }
    page_id_t page_id = pctx->page_count++;
    return page_id;
}
```

仅递增 page_count，**没有扩展文件**。这导致：
- `page_count` 与实际文件大小不同步（如果从未 write 到最后一个 page）；
- `read_page` 会读到 EOF 之外的位置（fread 返回 0，函数返回 -1），但 `page_count` 已被错误地认为有效；
- `pagefile_write_page` 在 line 67-70 会"扩展"page_count 跳号，破坏 ID 连续性。

**建议**：在 `alloc_page` 中同步扩展文件大小（fseek+write zero page 或显式 truncate）。

#### 问题 E4（中等）：`heap_vector_insert` 在持久化后端上的读-改-写放大

`heap_vector_store.c:234-244`：

```c
if (store->backend->ops->read_page(store->backend->ctx, page_id, page) == 0) {
    int32_t existing_slots = 0;
    if (!read_page_header(page, store->page_size, &existing_slots)) {
        /* 旧页或空页：按全零初始化，slot_count=0 */
        memset(page, 0, store->page_size);
    }
}
/* 若是第一次写入（已分配新页且 read 失败），page 已是零缓冲 */
```

每次插入都：
1. `malloc(page_size)` 临时缓冲；
2. `read_page` 整页读出；
3. `memcpy` 一个向量的字节；
4. `write_page` 整页写回；
5. `free(page)`。

**问题**：
- 每次插入都是 O(page_size) 的 I/O 而不是 O(dims)，且 8KB 页面 + 128 维 float = 512 字节有效数据，**I/O 放大 16 倍**。
- 注释"新分配的页面在持久化后端可能已经存在旧数据"是合理的，但对纯新分配的页面（`memory_alloc_page` 已 `memset(0)`）也会执行 read，浪费一次 memcpy。
- `read_page` 失败时不会报错，反而回退到零缓冲 —— 这在磁盘满 / IO 错误时会**静默丢失**之前的数据。

**建议**：
- 增加 `ops->is_new_page(ctx, page_id)` 谓词或 `alloc_page` 返回时由后端保证已清零；
- 或者维护 in-memory page cache（与 Buffer Pool 集成），热点页面只 read 一次。

#### 问题 E5（轻微）：`heap_vector_get` 中 magic 校验缺失

`heap_vector_store.c:285-335`：读取页面后仅校验 slot_count，**没有校验 magic 字段**。`read_page_header` 在 line 322 调用时仅校验了 magic 与 slot_count >= 0，但 magic 错配时直接返回 false，没有日志。

**建议**：在调试模式下对 magic 错配打 warning，便于诊断磁盘损坏。

#### 问题 E6（轻微）：`storage_page_file_create` 中目录创建失败被忽略

`storage_page_file.c:172-176`：

```c
if (last_sep) {
    *last_sep = '\0';
    /* 递归创建父目录（忽略已存在的错误） */
    mkdir(dir, 0755);
}
```

`mkdir` 失败（权限、路径不存在父级）会被忽略，后续 `fopen` 失败才会暴露。问题：
- 注释说"递归创建"，但实现是**单层** mkdir（与 `storage_mmap.c` 的 `mmap_ensure_parent_dir` 行为一致）。如果路径是 `a/b/c.idx` 且 `a/b` 不存在，单层 mkdir 会失败。

**建议**：实现真正的递归 mkdir，或文档明确说明"仅创建单层目录"。

#### 问题 E7（轻微）：`pagefile_close` 对空指针返回 -1，与其它后端语义不一致

`storage_page_file.c:117-119`：

```c
static int pagefile_close(void *ctx)
{
    page_file_ctx_t *pctx = (page_file_ctx_t *)ctx;
    if (pctx == NULL) {
        return -1;
    }
```

`memory_close` / `mmap_close` 对 NULL 都返回 0。`pagefile_close` 返回 -1 在被 `storage_backend_destroy` 调用时虽无副作用，但破坏了"close(NULL) 视为幂等"的隐式约定。

**建议**：统一为返回 0。

#### 问题 E8（轻微）：`pagefile_sync` 在 `file == NULL` 时返回 -1，但其它后端在类似情形下不同

`storage_page_file.c:107-109`：file 为 NULL 返回 -1；`mmap_sync` 与 `memory_sync` 在 file/map_addr 为 NULL 时返回 0。语义不一致。

**建议**：以"空集为真"为通用原则（即对空 backend 同步视为 no-op）。

---

## 四、扩展性

### 4.1 优点

1. **Ops 虚表模式**：新增后端只需实现 7 个 ops 函数 + 公共 API，不需要修改 `storage_backend.c`（除 `storage_backend_create` 分发）。
2. **类型枚举可扩展**：`storage_backend_type_t` 是 enum，新加类型时需修改三处（enum、`type_name`、create 分发）。
3. **`STORAGE_BACKEND_FAISS` 预留**：枚举中已为 Faiss 后端预留槽位。
4. **CMake 模块化**：每个后端独立 `add_project_library`，构建时按需链接。
5. **跨平台抽象层**：`storage_mmap.c` 内置 POSIX/Windows 抽象（`mmap_platform_*`），新增平台只需实现 6 个函数。

### 4.2 问题

#### 问题 X1：新增后端需修改 `storage_backend_create` 工厂，缺少注册表机制

`storage_backend_common.c:53-70`：当前是 switch-case，新增一个类型需修改：
- `storage_backend_type_t` enum
- `storage_backend_type_name`
- `storage_backend_create`
- 新增独立的 `_create` 函数

**建议**：提供注册表 API：

```c
typedef storage_backend_t *(*storage_backend_factory_fn)(const char *path, size_t page_size);
int storage_backend_register(storage_backend_type_t type, storage_backend_factory_fn factory);
```

或者借鉴 Linux `file_operations` 的链表风格，让各后端自注册。考虑到本项目只有 4 种后端、且类型固定，switch-case 也能接受，但应在文档中明示扩展流程。

#### 问题 X2：`backend_is_valid` 校验过于刚性

`heap_vector_store.c:63-77`：要求 `ops->alloc_page / read_page / write_page` 三个回调都必须存在。如果一个只读后端（如未来 FAISS 只读模式）想复用接口，需要把 alloc_page / write_page 设为 no-op 然后返回 0。这削弱了接口的多态性。

**建议**：把最低需求从"三个回调"放宽为"至少 read_page 可用"，并在文档区分"完整 / 只读 / 追加"三种 profile。

#### 问题 X3：ops 函数表没有版本号 / 能力位

不同后端的能力差异显著（memory 不持久化、mmap 共享、page_file 仅本机）。当前接口对调用方完全黑盒 — 调用方无法询问"这个后端支持并发？"、"是否支持随机写？"、"是否需要 fsync？"。

**建议**：

```c
typedef struct storage_backend_ops {
    const char *name;
    uint32_t capabilities; /* STORAGE_CAP_PERSISTENT | STORAGE_CAP_SHARED | STORAGE_CAP_RANDOM_WRITE */
    page_id_t (*alloc_page)(void *ctx);
    /* ... */
} storage_backend_ops_t;

int storage_backend_has_capability(const storage_backend_t *backend, uint32_t cap);
```

#### 问题 X4：`heap_vector_store` 与 `backend` 的耦合度过紧

`heap_vector_store.c:220` 直接调用 `store->backend->ops->alloc_page(store->backend->ctx)`。每次插入都要 4 次函数指针跳转（read、check header、write）。当 Heap 与其他向量索引（HNSW/IVF）共用 backend 时，这种开销难以摊销。

**建议**：考虑提供 `storage_backend_inline_ops_t`（在 backend 头文件直接暴露 ops）便于编译器内联；或一次性提供 `batch_read_page` / `batch_write_page` 减少函数调用次数。

#### 问题 X5：缺乏 buffer pool / page cache 集成

每次 `heap_vector_insert` / `heap_vector_get` 都是 `malloc(page_size) + read + ... + free`。与现有 `db/buf.h`（Buffer Pool）完全脱节，意味着：
- 重复读相同页面时无法命中缓存；
- 无并发同步原语；
- 与上层事务 / WAL 隔离级别无关联。

**建议**：将 backend 接口抽象为 "PageSource"，让上层可以注入 Buffer Pool；或单独提供 `heap_vector_store_attach_buffer_pool()`。

---

## 五、并发与生命周期

### 5.1 问题

#### 问题 L1：并发模型不一致

- `storage_backend.h` 未说明并发保证（ops 是否线程安全？同一 backend 多线程同时 alloc 是否安全？）。
- `heap_vector_store.h:57` 明说"单写者 / 多读者"，但未提供任何加锁原语。
- `mmap_close` 在 Windows 上若其他线程正在 read/write，会触发 `ERROR_LOCK_VIOLATION` 或类似问题。

**建议**：在 `storage_backend_t` 中明确"thread-safety contract"，并在 `storage_backend_ops` 上标注每种 op 的并发安全级别（thread-safe / reader-safe / exclusive）。

#### 问题 L2：`heap_vector_store_destroy` 不通知 backend

`heap_vector_store.c:193-200`：destroy 只 free 自身，**不调用任何 backend 回调**。如果调用方顺序是 `heap_vector_store_destroy` → 后续插入另一组向量（仍在用同一个 backend），无所谓；但如果顺序颠倒（destroy heap_store 但忘记 destroy backend），backend 资源会泄漏。

**建议**：在 destroy 时把 `store->backend = NULL`，并增加 `heap_vector_store_destroy_full(store, destroy_backend)` 一次性清理。

#### 问题 L3：`backend` 生命周期完全交由调用方管理，但缺少引用计数

`heap_vector_store_t` 与 `faiss_hnsw_index_t` 等可能共用同一 backend。当多个索引共享一个 backend 时，谁负责 destroy？没有引用计数或 RAII 包装，调用方需自行维护生命周期。

**建议**：在 `storage_backend_t` 中加入 `ref_count` 字段，配套 `storage_backend_acquire` / `storage_backend_release`，确保最后一个 release 才真正 destroy。

---

## 六、文档完整性

### 6.1 优点

1. 头文件都有详尽 Doxygen 注释；
2. `heap_vector_store.h` 中"所有权与生命周期"段写得很清晰，明确了 backend 不被接管；
3. 文件头注释说明了设计目标、并发模型、跨平台策略。

### 6.2 问题

#### 问题 D1：`heap_vector_store.h` 的"页面布局"图缺少字段对齐

```text
+---------------------+-------------------+--------------------+
|  magic (uint32)     |  slot_count       |  vector_0          |
|  (4 字节)           |  (int32, 4 字节)  |  (dims * 4 字节)   |
```

图正确（8 字节头），但没说明 `vector_0` 的对齐方式。当前实现使用紧凑布局，未做 4/8/16 字节对齐，对 SIMD 优化不友好。

**建议**：在文档中说明"向量以紧凑布局存储，未做对齐填充"，并在变更 SIMD 优化时同步更新。

#### 问题 D2：`storage_backend.h` 缺少错误码定义

返回 `int` 但没有定义 `STORAGE_BACKEND_ERR_*` 常量，调用方无法区分"参数错误" / "IO 错误" / "磁盘满"。

**建议**：

```c
#define STORAGE_BACKEND_OK              0
#define STORAGE_BACKEND_ERR_INVAL      -1
#define STORAGE_BACKEND_ERR_IO         -2
#define STORAGE_BACKEND_ERR_NOMEM      -3
#define STORAGE_BACKEND_ERR_NOSPC      -4
```

#### 问题 D3：`STORAGE_BACKEND_FAISS` 文档提及但无实现

`storage_backend.h:45` 注释说明 FAISS 是只读，但：
- 没有 `storage_faiss_create` 函数；
- `storage_backend_create` 走 FAISS 分支返回 NULL；
- 测试未覆盖。

**建议**：删除该枚举值，或在头文件加"（尚未实现）"标识。

---

## 七、测试覆盖评估

### 7.1 已覆盖

| 场景 | storage_backend | heap_vector_store |
|------|-----------------|-------------------|
| 创建/销毁（含 NULL） | 是 | 是 |
| 参数校验（page_size 边界、dims=0、backend=NULL） | 是 | 是 |
| 单页读写 | 是 | 是 |
| 批量读写 | memory | 是 |
| 跨页 | 不适用 | 是 |
| sync / flush | memory / pagefile / mmap | 不适用 |
| 引用有效性 / 偏移对齐 / 长度校验 | 不适用 | 是 |

### 7.2 缺失

1. **mmap 后端持久化测试**：未验证进程退出后重新打开能读回数据。
2. **mmap 后端大文件扩展**：未测试跨多次 remap 的场景。
3. **heap_vector_store 与 pagefile / mmap 后端的集成**：仅 memory 后端被测试。
4. **错误注入**：磁盘满 / IO 错误 / 权限错误 / 文件被占用等场景。
5. **并发测试**：尽管文档说"单写者 / 多读者"，但没有多线程测试。
6. **`batch_write` 的失败语义**：memory 后端测试了 happy path，未测试中途失败。
7. **`storage_backend_create` 工厂**：未测试通过 type 枚举分发的路径（仅测试了具体 `_create` 函数）。
8. **mmap 启动时 file_size 对齐丢弃尾部**：未测试非对齐 file_size 行为。

---

## 八、优先级建议

| 优先级 | 问题编号 | 摘要 |
|--------|---------|------|
| **P0** | E1 | mmap_alloc_page 失败后可能产生悬空指针 |
| **P0** | E2 | mmap 启动时丢弃非对齐尾部字节 |
| **P1** | E3 | pagefile_alloc_page 不同步扩展文件 |
| **P1** | E4 | heap_vector_insert I/O 放大且 read 失败时静默 |
| **P2** | A1 | page_size 与 backend 一致性约束文档化 |
| **P2** | X3 | 缺少 capability 标志位 |
| **P2** | L1 | 并发模型在接口层未明确 |
| **P3** | A4-A6 | API 类型一致性 / 截断风险 |
| **P3** | D2 | 错误码定义 |
| **P3** | E5-E8 | 轻微错误处理不一致 |

---

## 九、总结

存储子系统的整体架构合理，模块边界清晰，ops 虚表 + 工厂分发的扩展性良好。**最大的风险点**集中在 mmap 后端的稳健性（E1 / E2）和 heap_vector_store 的 I/O 效率（E4）。建议：

1. **立即修复**：E1、E2（数据安全风险）。
2. **下一迭代**：E3、E4、L1（持久化正确性、性能、并发契约）。
3. **规划改进**：X3 capability 位、D2 错误码（接口进化）。

子系统通过清晰的 ops 抽象和模块化 CMake，为后续接入 Faiss / RocksDB / 自定义云存储等后端打下了良好基础。建议在修复 P0/P1 问题后，再补充并发与持久化端到端测试。