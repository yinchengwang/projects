> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# KV 模态差距深度分析

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查
> 代码位置：`engine/src/db/storage/kv/`（~2.3K 行，6 文件）+ `engine/src/db/cf/`（Column Family）+ `include/db/kv.h`

## 1. 实现现状盘点

### 1.1 模块清单

| 模块 | 文件 | 行数 | 说明 |
|------|------|------|------|
| KV 主引擎 | `storage/kv/kv.c` | 732 | put/get/delete/scan/flush |
| 游标扫描 | `storage/kv/kv_iter.c` | 215 | 范围迭代器 |
| TTL | `storage/kv/kv_ttl.c` | 587 | TTL 管理 |
| 有序存储 | `storage/kv/kv_ordered.c` | 419 | 有序键存储 |
| 引擎封装 | `storage/kv/kv_engine.c` | 369 | 与 mm_storage 对接 |
| Column Family | `src/db/cf/`（P3-4 完成 commit bb2c77fbb） | - | KV 多命名空间 |
| 公共 API | `include/db/kv.h` | - | KV_MAX_KEY_SIZE=8192, KV_MAX_VALUE_SIZE=1MB |

### 1.2 关键事实修正（相对 8 月 25 日旧对比文档）

旧文档称"Value 大小受 VecPage 16KB 限制"——**不准确**。`include/db/kv.h:34` 定义 `KV_MAX_VALUE_SIZE = 1024*1024 = 1MB`，对 KV 是 64× 提升（但仍小于 Redis 的 512MB STRING、RocksDB 无限）。

### 1.3 测试覆盖

P3-4 Column Family commit bb2c77fbb，含对应测试。具体断言数未逐文件统计（参照提交记录）。

## 2. 代码级质量审查

### 2.1 并发正确性

**缺陷 1：kv.c 全文件零锁原语——并发 put 会丢更新「确认·实现质量缺陷」**

`storage/kv/kv.c`（732 行）`grep -E 'mutex|pthread_|spinlock|lock'` 零命中。`kv_put`（:383）执行序列：`kv_get` 取旧值（:396）→ `kv_page_find` 查存在（:409）→ 决定 insert/update。两条并发 put 修改同一 key：均读到旧值、均通过 `found==true` 路径，先后两次 `kv_page_update`——后者覆盖前者，第一个写入的 `num_keys` 自增与新数据全部丢失。结构体定义（`kv.h:66`）有 `lock_manager_t *lock_mgr` 字段，但 grep 未见使用（疑似接入后未启用）。

**缺陷 2：put 内多次 `buffer_unpin_page` 在并发遍历下可丢失修改「确认·实现质量缺陷」**

`kv_put` 内 `buffer_unpin_page`（:412）→ `buffer_get_page`（:422）→ `buffer_unpin_page`（:426）之间存在窗口，并发读访问正在迭代的同 page 时 PageData 可能被新插入推偏移（kv_page_insert 紧凑布局，:417 注释承认"紧凑布局变更长度会推后续记录"）。

### 2.2 崩溃恢复

**正面证据：KV 是三大写路径中唯一接入共享 WAL 的模块「确认」**

`storage/kv/kv.c:443-447` 写 `wal_write_update`、`:458-460` 写 `wal_write_insert`、`:671` 实现 `kv_wal_apply` 回放。这是关系模态（缺）、向量模态（独立 WAL）之外的第三个选择。

**缺陷 1：WAL 与页面写入顺序不严格（先 page 后 WAL）「疑似·实现质量缺陷」**

`kv_put:451` `kv_page_insert` 写入页面成功后再 `:458 wal_write_insert`——顺序正确（先 redo log 后写盘语义违反）。但 `kv_put:438-447` update 分支：先 `kv_page_update` 成功后 `:444 wal_write_update`——同样 page→WAL 顺序。如果 WAL 写失败（:444 的 `wal_write_update` 返回值被忽略！），页面已改、WAL 无记录，崩溃后 redo 不到这条更新——数据不一致。

**缺陷 2：`wal_write_*` 返回值被忽略「确认·实现质量缺陷」**

`kv.c:445, 459` 均不检查 `wal_write_insert/update` 返回值——WAL 失败只可能是磁盘满或 IO 错误，被静默吞掉后页面已更新，崩溃后数据丢失无痕迹。

**缺陷 3：TTL 与 WAL 协调未核「疑似·实现质量缺陷」**

`kv_ttl.c:587` 独立 TTL 管理（基于"过期键文件"），与主 WAL 写入关系待核——TTL 过期删除时是否记录 WAL？过期清理的崩溃后一致性需要验证。

### 2.3 内存安全

**正面证据**：`kv_put` 错误路径正确 `free(old_value)`（:402/439/452）；`kv_get` 返回动态分配 buffer 给调用方（接口契约需复核，可能存在二次 free 风险）。

**缺陷 1：`kv_get` 内存分配与释放契约跨文件「疑似·实现质量缺陷」**

`kv_get`（:468-?）返回 `void *value` 与 `size_t *value_len` 由调用方释放。`kv.h` 注释需复核是否明确"调用方负责 free"。如未明确，多个调用方（kv_put:396、kv_delete:523、kv_mget:608）可能漏 free 或重复 free。

### 2.4 错误处理

**缺陷 1：WAL 失败静默吞（见 2.2 缺陷 2）「确认·实现质量缺陷」**

**缺陷 2：无 CAS/事务语义，乐观并发完全靠调用方「确认·功能缺失」**

`include/db/kv.h` 列出 `kv_put/get/delete/scan`——无 `kv_cas`、`kv_watch`、`kv_multi/exec`。旧文档列入 P0 Gap。FoundationDB 提供 Watch/Read-Conflict-Range + Transaction，Redis 提供 WATCH/MULTI/EXEC，自研无任何等价物。

**缺陷 3：page full 错误处理语义模糊「疑似·实现质量缺陷」**

`kv_put:453` `kv_set_error("Failed to insert (page full)")` 后返回 `KV_ERROR`——page full 应是明确的 `KV_FULL` 错误码（kv_result_t 枚举不含），需扩展以支持调用方分情况重试（如分裂 page）。

### 2.5 算法实现质量

**正面证据**：KV 是单文件简单实现（kv.c 732），核心 B+Tree-like 页面布局清晰；紧凑布局的注释（:417-419）诚实承认"长度变更需 delete+insert"。

**缺陷 1：缺少 page 分裂——大插入集落到 page full 错误而非自动分裂「确认·功能缺失」**

`kv_page_insert` 返回非 0 时只报错，没有分裂逻辑——KV 单页容量受限，并发批量插入会因 page full 失败。B+Tree 通常在 insert 满时分裂为两页并向上推键；自研缺此层。

**缺陷 2：游标扫描范围语义仅字节序（memcmp）非字典序「确认·功能缺失」**

`kv_iter.c` 的 `kv_scan(db, "user:", 5, "user~", 5)` 使用字节序遍历——遇到非 ASCII 键（UTF-8 中文、binary keys）行为不当。Redis 0.0 字节序 + escape 字符是公认 trick 但仍为 bytes；DuckDB/RocksDB 支持 collation-aware 比较。

### 2.6 API 设计

**正面证据 1**：错误码枚举（`KV_OK/NOT_FOUND/ERROR/CORRUPT/NOMEM/EXISTS/INVALID`）清晰，比直接返回 int 自定义码更稳健；`KV_EXISTS` 提示支持 put-if-not-exists 语义（但实现未确认）。

**正面证据 2**：P3-4 Column Family（commit bb2c77fbb）落地，等价 RocksDB CF 的多命名空间隔离。

**缺陷 1：无 CAS/Watch/Multi——API 层级低于主流 KV「确认·功能缺失」**

FoundationDB 的乐观事务、Redis 的 WATCH/MULTI/EXEC、etcd 的 compare-and-swap——共同特征是给客户端并发控制原语。自研只有 put/get/delete/scanner，客户端必须自行外加锁。

**缺陷 2：Key 8KB / Value 1MB 上限低于业界「确认·功能缺失」**

Redis 512MB / RocksDB 3GB / Badger 无限——1MB 适合元数据但文档/JSON/小图片装不下。旧对比文档建议"支持更大 Value"已部分实现（VecPage → 1MB）但仍未达标。

## 3. 业界标杆对比

| 维度 | 自实现 | Redis 7.x | RocksDB | etcd | FoundationDB |
|------|--------|-----------|---------|------|--------------|
| Key 上限 | 8KB | 512MB | 3GB | 1MB | 10KB |
| Value 上限 | 1MB | 512MB | 无限 | 1MB | 10MB |
| 有序 | 是（kv_ordered.c） | SORTED SET | 是（MemTable+有序） | 是（Raft Log） | 是（Range Read） |
| TTL | 是（kv_ttl.c） | EX/PX | 是 | lease | 是 |
| 持久化 | WAL + 页面 | RDB+AOF | SSTable + 压缩 | Raft Log | Redo Log |
| WAL fsync | 待核（可能缺） | always/everysec/no | always | always | always |
| 事务/原子 | 无 | MULTI/EXEC、WATCH | TransactionDB 乐观 CAS | Txn 2PC-like | 严格 ACID |
| Column Family | 是（P3-4） | 无（Redis Stack 模块） | 是 | 无 | 是（Directory） |
| 分布式 | 单机 | Cluster（16384 slot） | 单机 | Raft（3-5） | 分片 + 多副本 |
| 并发控制 | 无锁（结构体有 lock_mgr 字段） | 单线程主 + I/O 线程 | 多线程 Compaction | Raft 串行 | Watch + 乐观事务 |

## 4. 差距矩阵

| 维度 | 评分 | 关键证据 |
|------|------|---------|
| 并发正确性 | 2 | kv.c 全文件零锁 + `kv_put` 读-改-写无原子性 `kv.c:383-462`；结构体有 lock_mgr 字段未见使用 `kv.h:66` |
| 崩溃恢复 | 4 | 唯一接入共享 WAL `kv.c:443-447,671`；WAL 失败返回值忽略 `:445,459`；page→WAL 顺序 OK |
| 内存安全 | 6 | 错误路径 free 正确 `kv.c:402,439,452`；kv_get 释放契约待复核 |
| 错误处理 | 4 | WAL 静默吞 `:445,459`；page full 无 KV_FULL 码 `kv.c:453`；无 CAS/事务 |
| 算法实现质量 | 4 | 紧凑布局诚实但缺 page 分裂；游标扫描字节序而非 collation |
| API 设计 | 5 | 错误码枚举清晰 `kv.h:44-52`；CF 落地（P3-4）；无 CAS/Watch/Multi |

**实现质量缺陷清单（5 项确认 + 2 项疑似）**：
1. 并发 put 丢更新（并发）
2. WAL 失败返回值忽略（错误处理/崩溃）
3. buffer pin/unpin 间隙并发不安全（并发）
4. kv_get 释放契约未文档化（疑似，内存）
5. page full 误用 KV_ERROR 而非专用码（错误处理）
6. TTL 清理与 WAL 协调待核（疑似）
7. 单页无分裂——批量插入失败（功能缺失）

## 5. 改进优先级

| 优先级 | 项目 | 分类 | 工作量 |
|--------|------|------|--------|
| P0 | kv_put 引入锁（沿用结构体 lock_mgr 或换 pthread_rwlock）+ 写时校验 | 实现质量缺陷 | S |
| P0 | WAL 写入返回值检查 + 失败回滚页面（page→WAL→page 逆序或 2PC） | 实现质量缺陷 | M |
| P1 | 实现 kv_cas（compare-and-swap）与 kv_watch | 功能缺失 | M |
| P1 | 错误码扩展 KV_FULL/KV_CONFLICT 等 | 实现质量缺陷 | S |
| P1 | kv_get 释放契约文档化 + 调用方加注释 | 内存安全 | S |
| P2 | B+Tree 页分裂（解除 page full 限制） | 功能缺失 | M |
| P2 | 游标扫描 collation 选项 | 功能缺失 | S |
| P3 | 提升 Value 上限到 16MB+（页溢出/外部文件） | 功能缺失 | M |
