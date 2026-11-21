> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Vector 模态差距深度分析

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查（未运行新基准）
> 代码位置：`engine/src/db/storage/vector/`（~7.0K 行）+ `engine/src/db/index/vector_index/`（~41.5K 行，148 文件）

## 1. 实现现状盘点

### 1.1 模块清单

| 层 | 模块 | 关键文件 | 行数 |
|----|------|---------|------|
| 存储层 | 向量引擎主体 | `storage/vector/vector_engine.c` | 2574 |
| 存储层 | VecPage 页池 | `storage/vector/vec_page.c` | 849 |
| 存储层 | 向量 WAL | `storage/vector/vector_wal.c` | 924 |
| 存储层 | Segment 管理 | `storage/vector/vector_segment.c` | 621 |
| 存储层 | 索引持久化 | `storage/vector/vector_index_persist.c` | 423 |
| 存储层 | Buffer 协调 | `storage/vector/vector_buf.c` | 548 |
| 存储层 | 图去重 | `storage/vector/graph_dedup.c` | 656 |
| 索引层 | faiss_hnsw 内存版 | `index/vector_index/faiss_hnsw/`（12 文件） | ~2000+ |
| 索引层 | 21+ 种索引 | `index/vector_index/` 下 ivf/ivf_flat/ivf_hnsw/scann/annoy(隐含)/bq/opq/itq/lvq/rq/ssg/diskann/BM25 等 | ~35000+ |
| 索引层 | GPU 加速 | `index/vector_index/gpu/`（gpu_hnsw/gpu_ivf/gpu_ivf_pq/simd） | ~2600 |
| 索引层 | 流式索引 | `index/vector_index/streaming/`（并发搜索/写缓冲/合并调度） | ~2200 |
| 索引层 | 删除与 GC | `index/vector_index/delete/`（删除位图/图修复/VACUUM） | ~1200 |
| 索引层 | 索引选择器 | `index/vector_index/vector_index_selector.c` | 380 |

### 1.2 三套 HNSW 路径并存（历史遗留）

1. **旧路径**：`storage/vector/faiss_hnsw_stub.c`（165 行，stub）+ 旧 `hnsw/faiss_hnsw.h` 头（仍被约 24 处引用，含 delete/storage_backend 扩展 API）
2. **新内存版**：`index/vector_index/faiss_hnsw/`（完整实现：create/level/search_layer/search/quantize/visited_table/minimax_heap/filtered search）
3. **持久化版占位**：`index/vector_index/hnsw/hnsw_placeholder.c`（73 行占位）+ `storage/vector/vector_index_persist.c`

### 1.3 测试覆盖

16+ 个向量相关测试文件（`test/db/storage/vector_*.cpp`、`test/db/vector_index/`、`test/db/api/vector_api_test.cpp`、`test/db/benchmark/vector_benchmark.cpp` 等），另有 10 个 ANN 索引的 SIFT Small 召回率专项测试（`*_recall.c`）。

### 1.4 已知基准（引自旧对比文档与 memory，未复测）

- 100K / 1M 数据集 Recall@10 = 0.99（ef = k*5+50 调优后）
- 插入吞吐 ~158K vec/s
- GPU-HNSW / GPU-IVF / GPU-IVF-PQ 已实现（P2-1 完成）

## 2. 代码级质量审查

### 2.1 并发正确性（评分依据：多处确认缺陷）

**缺陷 1：自旋读写锁存在读者-写者同时持有的竞态窗口「确认·实现质量缺陷」**

`storage/vector/vector_engine.c:1565-1598` 的 `simple_rwlock_t`：读者在 `__sync_fetch_and_add(&readers, 1)` 后再次检查 `writer_active`（:1573），写者在 `while (readers > 0)` 自旋后 CAS `writer_active`（:1589-1593）。两个检查互相之间没有原子性：读者读到旧的 `writer_active=0` 并返回的同时，写者已读到旧的 `readers==0` 并 CAS 成功——双方同时持有锁，临界区失去保护。窗口窄但真实存在，且该锁是引擎唯一的并发防线。

**缺陷 2：锁默认关闭，并发保护是可选项「确认·实现质量缺陷」**

`vector_engine.c:219` 初始化 `db->use_lock = false`，必须显式调用 `vector_engine_enable_lock()`（:1608）才启用。默认路径下所有读写完全无保护。

**缺陷 3：共享计数器非原子更新「确认·实现质量缺陷」**

`vector_engine.c:412` 与 `:431` 的 `db->num_vectors++` 为普通自增，并发插入会丢失计数；且 `tuple_insert` 全程不取写锁（锁的获取依赖调用方自觉）。

**缺陷 4：faiss_hnsw 增删与搜索完全无锁，realloc 直接制造 UAF「确认·实现质量缺陷」**

- 插入路径 `index/vector_index/faiss_hnsw/faiss_hnsw_stubs.c:211-241` 在容量不足时 `realloc` `idx->vectors/levels/offsets/neighbors`
- 搜索路径 `faiss_hnsw/faiss_hnsw_search.c:26` 直接解引用 `idx->vectors + vec_id * dims`
- `faiss_hnsw_search_filtered.c` 全文件无任何锁（grep 无 mutex/lock 命中）

并发「一边插入触发扩容、一边搜索」时，搜索线程读到已释放的旧数组——释放后使用。FAISS 的对策是搜索期间持有只读引用/快照（`IndexRef`/omp 下的 add-search 串行约定），自研版没有等价机制。

**缺陷 5：写者饥饿与超时参数失效「确认·实现质量缺陷」**

`vector_engine.c:1565` 读者只检查 `writer_active`、从不检查 `writers_waiting`，持续读流量下写者可能无限饥饿（自旋烧 CPU）；`:1664` 的 `vector_engine_write_lock(rel, timeout_ms)` 签名带超时参数，实现里完全忽略，永远自旋。

**正面证据**：WAL 内部互斥使用规范——`vector_wal.c:258/304`（append）、`:317/354`（delete）锁获取与释放路径配对完整，包括错误分支（:286、:339）。

### 2.2 崩溃恢复（评分依据：结构完整但持久化语义不完整）

**缺陷 1：WAL 写失败仅告警后继续写数据「确认·实现质量缺陷」**

`vector_engine.c:377-379`：`vector_wal_append` 失败只 `LOG_WARN("WAL 写入失败，数据可能不安全")` 然后继续执行插入——违反 WAL-first 语义。PostgreSQL 的铁律是 WAL 失败必须中止事务，自研版会把不可恢复的数据写入主存储。

**缺陷 2：「同步模式」没有 fsync「确认·实现质量缺陷」**

`vector_wal.c:282-289`：`VECTOR_WAL_SYNC` 模式仅 `fwrite + fflush`，无 `fsync/fdatasync`（Windows 下 `FlushFileBuffers`），全文件及 checkpoint（:404）同样只有 `fflush`。断电场景下"已提交"记录可能丢失——只防进程崩溃不防系统崩溃。Redis AOF 的 `always` 策略、PostgreSQL `synchronous_commit=on` 都是真正 fsync。

**缺陷 3：WAL 记录 ID 与实际存储 ID 可能错位「疑似·实现质量缺陷」**

`vector_engine.c:376` WAL 记录的 `vec_id` 取自插入前的 `db->num_vectors`（"预分配 ID"），而 `:384` 实际 ID 由 `vector_page_append` 返回（传入 -1 由页池自行分配）——两者无强制一致性约束，恢复重放时向量 ID 映射可能偏移。需运行验证恢复后 ID 正确性。

**正面证据**：WAL 记录带 CRC32 校验（`vector_wal.c:277`）、有完整重放（`:528` `vector_wal_replay`）与恢复流程（`:684` `vector_index_recover`，含状态报告），torn record 可被 CRC 拒绝——这部分设计合格。

### 2.3 内存安全（评分依据：两处高危 + 总体错误路径清理尚可）

**缺陷 1：VLA 栈溢出「确认·实现质量缺陷」**

`vector_wal.c:261`：`uint8_t record[VECTOR_WAL_RECORD_HEADER_SIZE + dims * sizeof(float)]`——变长数组分配在栈上，`dims` 由建表参数决定（业界支持到 32768+）。dims=100 万时直接栈溢出崩溃。FAISS 等价路径全部使用堆缓冲。

**缺陷 2：异步模式固定缓冲区无边界检查的 memcpy「确认·实现质量缺陷」**

`vector_wal.c:292-298`：缓冲区满时先刷盘再 `memcpy(wal->buffer + used, record, record_size)`——代码假设 `record_size <= buffer_size`，但 record 大小随 dims 线性增长，dims 大到超过整个 WAL 缓冲区时越界写。

**正面证据**：`faiss_hnsw_stubs.c:207-242` 的两个扩容函数失败时不修改容量字段、保留旧指针有效性，回滚语义正确；`faiss_hnsw_search.c:160-166` 搜索临时缓冲的 malloc 失败处理（含双指针部分失败）正确。

### 2.4 错误处理（评分依据：静默成功比报错更危险）

**缺陷 1：drop 操作是空操作却返回成功「确认·实现质量缺陷」**

`vector_engine.c:351-354`：`vector_engine_table_drop` 直接 `(void)name; return 0;`——上层认为删表成功，数据文件、WAL、内存结构全部原样保留。静默的数据滞留。

**缺陷 2：文件回退路径忽略 fwrite 结果且无 fsync「确认·实现质量缺陷」**

`vector_engine.c:422-429`：VecPage 失败后回退直接 `fwrite(data, 1, len, fp); fclose(fp)`，返回值丢弃、无 `fflush/fsync`、无部分写重试——磁盘满时插入"成功"但数据丢失。

**缺陷 3：扫描接口未实现却注册进引擎「确认·实现质量缺陷」**

`vector_engine.c:435-449`：`scan_begin` 恒返回 NULL、`scan_next` 恒返回 -1，但引擎以完整 AccessMethod 身份注册——调用方拿到 NULL 只能猜测原因，无法区分"空表"和"不支持"。

### 2.5 算法实现质量（评分依据：核心结构忠实，外围有语义级偏差）

**正面证据 1：HNSW 搜索结构与 FAISS 一致「确认」**

`faiss_hnsw_search.c:126-156` 上层贪婪下降 + 底层 beam search 的整体结构与 `reference/vector/faiss` 的 `HNSW::search` 逐层对应；`:121-124` 的 `ef = max(ef_search, k)`、最小 ef=16 约束与 FAISS 行为一致（P5-5 修复后底层起点取贪婪下降局部最优，:158-175）。

**正面证据 2：层级分配忠实复刻 FAISS 且有边界保护「确认」**

`faiss_hnsw_level.c:27-54`：assign_probas 累积概率逻辑与 `HNSW::random_level()` 一致（注释含原版对照），`:43` 的 `l < assign_probas_size - 1` 防越界比 FAISS 更保守。

**缺陷 1：内积度量静默退化为 L2「确认·实现质量缺陷」**

`faiss_hnsw_search.c:46-52`：非 L2/Cosine 的度量（内积 IP、汉明）fallback 到 L2 平方距离——IP 场景要的是"距离最小 = 内积最大"，语义完全错误，且没有任何告警。FAISS 对 IP 用倒序比较器。用了 IP 建索引的用户会拿到系统性错误的结果。

**缺陷 2：距离计算纯标量，无 SIMD「确认·功能缺失（性能）」**

`faiss_hnsw_search.c:29-52` 逐维标量循环。FAISS 同路径是 AVX2/AVX-512 向量化。注：`index/vector_index/gpu/simd.c`（813 行）有 SIMD 实现，但 faiss_hnsw 主搜索路径未接入。

**缺陷 3：top-k 用 O(n²) 选择排序「确认·功能缺失（性能，小 ef 可接受）」**

`faiss_hnsw_search.c:184-202`：ef 较大时（如 ef=500）排序开销显著，FAISS 用 `maxheap_heapify` O(n log k)。

**正面证据 3：删除能力超出 FAISS 基线「确认」**

`index/vector_index/delete/`（删除位图 `vector_delete_bitmap.c`、过滤搜索 `vector_search_filter.c`、图修复 `graph_repair.c`、VACUUM `vector_gc_vacuum.c`）——FAISS 本体不支持删除，自研版带图修复的删除是相对优势项。

### 2.6 API 设计（评分依据：抽象层存在但契约不完整）

**缺陷 1：三套 HNSW 路径并存无收敛计划「确认·实现质量缺陷」**

`storage/vector/faiss_hnsw_stub.c`、`index/vector_index/faiss_hnsw/`、`index/vector_index/hnsw/hnsw_placeholder.c` 三套并存，旧头文件 `hnsw/faiss_hnsw.h` 仍被 24 处引用——同一能力三个入口，维护成本与行为分歧风险高。

**缺陷 2：mm_insert 的序列化契约靠手工偏移解析「确认·实现质量缺陷」**

`vector_engine.c:360-372`：`tuple_insert` 用 `ptr += sizeof(uint64_t)`、`memcpy(&dim, ptr, sizeof(int32_t))` 手工解析字节流——插入格式（id + dim + floats）没有共享的结构体定义，两端各自硬编码偏移，格式演进无版本号。

**缺陷 3：drop/scan 的契约空洞（见 2.4）「确认·实现质量缺陷」**

注册为完整 AccessMethod 但 drop 空转、scan 返回 NULL，违背 `mm_storage.h` 声明的接口语义（`mm_drop_model`/`mm_scan_begin` 的调用方无法得到有效错误信息）。

**正面证据**：21+ 种索引通过 `vector_index_selector.c`（380 行）统一选择入口，过滤搜索/混合检索/流式索引均有独立 DESIGN.md（`faiss_hnsw/DESIGN.md`、`streaming/DESIGN.md`），索引层 API 组织度高于存储层。

## 3. 业界标杆对比

| 维度 | 自实现 | FAISS | Milvus | Qdrant | pgvector |
|------|--------|-------|--------|--------|----------|
| 索引类型 | 21+ 种（含 GPU 3 种、DiskANN、BQ/OPQ/ITQ/LVQ/RQ/SSG） | 40+ 种 | 10 种 | HNSW+Flat | 3 种 |
| 并发写读 | 无锁或自旋锁（有竞态窗口） | OpenMP 批式（非在线并发） | 分片并发 + 快照读 | RwLock + segment 不可变 | PostgreSQL MVCC |
| 崩溃恢复 | CRC32 WAL + replay，无 fsync | 序列化文件（无 WAL） | WAL + Checkpoint + Segment | WAL + Snapshot | PG WAL（真 fsync） |
| 删除 | 位图 + 图修复 + VACUUM | 不支持 | 支持（Compaction） | 支持 | 支持 |
| 距离度量 | L2/Cosine 正确，IP 退化 L2 | L2/IP/L1/Linf/Jaccard 等 12+ | 6 种 | 5 种 | 6 种 |
| SIMD | 有 simd.c 但主路径未接入 | AVX2/AVX-512 全面 | 有 | 有 | SSE/AVX |
| 召回率 | 0.99 @ 100K/1M（ef=k*5+50） | 0.99 | ~0.99 | ~0.99 | ~0.98 |
| 写入吞吐 | ~158K vec/s（C 绑定开销瓶颈） | CPU ~500K / GPU 更高 | 集群百万级 | ~百万级 | ~50 万 |
| GPU | 3 种 GPU 索引（自研） | GPU_IVF 全系 | GPU_CAGRA | 无 | 无 |
| 磁盘索引 | DiskANN/Vamana 已实现 | DiskANN | DiskANN | 无 | 0.8+ 有 |

## 4. 差距矩阵

| 维度 | 评分 | 关键证据 |
|------|------|---------|
| 并发正确性 | 3 | 读写锁竞态窗口 `vector_engine.c:1565-1598`；无锁 add/search + realloc UAF `faiss_hnsw_stubs.c:211` / `faiss_hnsw_search.c:26`；默认不加锁 `vector_engine.c:219` |
| 崩溃恢复 | 4 | WAL 失败继续写 `vector_engine.c:377-379`；SYNC 模式无 fsync `vector_wal.c:282-289`；CRC+replay 合格 `vector_wal.c:277,528` |
| 内存安全 | 4 | VLA 栈溢出 `vector_wal.c:261`；memcpy 越界 `vector_wal.c:292-298`；扩容回滚正确 `faiss_hnsw_stubs.c:207-230` |
| 错误处理 | 3 | drop 空转返回成功 `vector_engine.c:351-354`；fwrite 忽略 `vector_engine.c:428`；scan 恒 NULL `vector_engine.c:435-444` |
| 算法实现质量 | 7 | 搜索结构忠实 FAISS `faiss_hnsw_search.c:126-156`；IP 度量退化 L2 `faiss_hnsw_search.c:46-52`；无 SIMD；删除能力超 FAISS |
| API 设计 | 5 | 三套 HNSW 路径并存；手工字节偏移解析 `vector_engine.c:360-372`；selector 统一入口是亮点 `vector_index_selector.c` |

**实现质量缺陷清单（8 项确认 + 1 项疑似）**：
1. 读写锁竞态窗口（并发正确性）
2. 默认无锁 + 插入路径不取锁（并发正确性）
3. faiss_hnsw add/search 无锁 + realloc UAF（并发正确性）
4. WAL 失败后继续写主存（崩溃恢复）
5. SYNC 模式无 fsync（崩溃恢复）
6. WAL 记录 VLA 栈溢出 + 异步 memcpy 越界（内存安全）
7. drop 空转 / fwrite 忽略 / scan 恒 NULL（错误处理）
8. IP 度量静默退化 L2（算法正确性）
9. WAL ID 与存储 ID 错位（疑似，需运行验证）

## 5. 改进优先级

| 优先级 | 项目 | 分类 | 工作量 | 说明 |
|--------|------|------|--------|------|
| P0 | faiss_hnsw 加读写锁或插入时 copy-on-write 快照 | 实现质量缺陷 | M | 唯一的 UAF 级风险，线上并发必炸 |
| P0 | 重写 simple_rwlock（消除竞态窗口 + 写者优先 + 尊重 timeout）或换 pthread_rwlock | 实现质量缺陷 | S | 竞态窗口 + 饥饿 + 假超时三合一修复 |
| P0 | IP 度量修正（倒序比较器）或显式报错拒绝 | 实现质量缺陷 | S | 静默错误结果最危险 |
| P1 | WAL-first 硬约束：append 失败中止插入 | 实现质量缺陷 | S | |
| P1 | WAL 记录堆缓冲 + 边界检查；SYNC 模式加 fsync | 实现质量缺陷 | S | |
| P1 | drop/scan 接口实装或返回明确错误码（如 -ENOTSUP） | 实现质量缺陷 | S | |
| P2 | 主搜索路径接入 simd.c 向量化距离 | 功能缺失（性能） | M | 吞吐瓶颈之一 |
| P2 | 三套 HNSW 路径收敛，旧头文件 24 处引用迁移 | 实现质量缺陷 | L | 技术债 |
| P2 | 插入序列化格式版本化 | 实现质量缺陷 | S | |
| P3 | top-k 选择排序换堆 | 功能缺失（性能） | S | ef 大时才有感 |
