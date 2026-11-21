> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Timeseries 模态差距深度分析

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查
> 代码位置：`engine/src/db/storage/ts/`（~4.7K 行，10 文件）

## 1. 实现现状盘点

### 1.1 模块清单

| 模块 | 文件 | 行数 |
|------|------|------|
| 引擎主体 | `ts_engine.c` | 853 |
| 列存压缩 | `ts_columnar.c` | 716 |
| 标签索引 | `ts_tag_index.c` | 524 |
| 连续聚合 | `ts_continuous_agg.c` | 514 |
| 物化视图 | `ts_mview.c` | 348 |
| Segment 管理 | `ts_segment.c` | 458 |
| SQL 函数 | `ts_sql_functions.c` | 456 |
| 数据保留 | `ts_retention.c` | 418 |
| 压缩缓冲 | `ts_compress.c` | 405 |

### 1.2 关键事实修正（相对 8 月 25 日旧对比文档）

- 旧文档称"无物化视图"——**不准确**。`ts_mview.c`（348 行）+ `ts_continuous_agg.c`（514 行，含 ContinuousAggConfig/State 与刷新调度）已实现
- 旧文档称"写入 ~50-100K 点/秒"——本次未复测，标注为存疑

## 2. 代码级质量审查

### 2.1 并发正确性

**缺陷 1：复刻 vector_engine 的 buggy 自旋读写锁「确认·实现质量缺陷」**

`ts_engine.c:745-770` 定义 `g_ts_lockmgr` 与 `ts_rwlock_t`（同 vector 的 readers/writers_waiting/writer_active 结构）：同样的竞态窗口（reader add + check 与 writer CAS+readers==0 之间交错），同样的写者饥饿（read_lock 只看 writer_active 不看 writers_waiting）。`use_lock` 默认 `false`（:133）。问题本质与 01-vector 相同——同一个错误实现被复制到第二个模态，说明缺少统一的锁原语库。

### 2.2 崩溃恢复

**缺陷 1：insert 路径未发现 WAL 集成「确认·实现质量缺陷」**

`ts_engine.c` grep `wal_write_/xlog_insert/wal_log_` 零命中。时序 DML 落入 segment 后无 redo log，崩溃后已确认但未刷盘的 segment 丢失；与 Relational 同病。

### 2.3 内存安全

**缺陷 1：`ts_compress_add` 缓冲区满时静默返回 -1 丢弃数据「确认·实现质量缺陷」**

`ts_compress.c:144-146`：
```c
if (idx >= TS_COMPRESS_BLOCK_SIZE) {
    return -1;
}
```
行 130 已有"块已满先 flush"处理（`:131`），但 flush 后 `block = comp->current_block` 已是新空块（line 132），新块的 num_points 为 0，idx 永远 < TS_COMPRESS_BLOCK_SIZE——这段死代码实际上不可达。但语义问题是：满块路径如果 flush 失败或新块分配失败，旧数据丢失无任何告警。

**正面证据**：`ts_compressor_free`（:112）依次释放 block→compressed_ts→compressed_values；`ts_compressor_create`（:88）calloc 失败回滚到 free(comp)。

### 2.4 错误处理

**缺陷 1：ts_compress_add 满块静默吞数据（同 2.3 缺陷 1）「确认·实现质量缺陷」**

**缺陷 2：连续聚合刷新失败处理不显式「疑似·实现质量缺陷」**

`ts_continuous_agg.c` 514 行中 `cagg_state_free` 等接口正常，但具体的 refresh 路径与失败处理需读全文（514 行未全核）；配置 `refresh_interval_ms=60000` 默认（:25）但失败重试与降级策略未在配置层暴露。

### 2.5 算法实现质量

**正面证据 1：Gorilla XOR 编码位宽自适应「确认」**

`ts_compress.c:40-42` `xor_bits_needed(a, b)` 返回 a^b 最高有效位位置——这是 Gorilla 论文核心（Pelkonen 等 2015）的"leading/trailing zero 截位"思想，省略了完整的 leading-zero + trailing-zero 控制字版本但保留位宽优化意图。

**缺陷 1：热路径不压缩——只缓冲，压缩延后到 flush「确认·功能缺失（性能）」**

`ts_compress.c:148-149` 在 add 阶段写入 `records[idx].timestamp/value`（int64+double = 16 B/点），未实时压缩；只在 `ts_compress_flush`（:175）才分配 `compressed_ts/compressed_values` 编码。内存中驻留未压缩数据→ cache 不友好（与 InfluxDB IOx 列式直接编码对比）。这是设计取舍（LSM-style），但旧对比文档"已实现 Gorilla 压缩"的措辞误导——写入路径不享受压缩收益。

**正面证据 2：连续聚合与物化视图有完整配置-状态分离「确认」**

`ts_continuous_agg.c:25-89` 配置（refresh_interval、bucket、func、group_by tag、window）+ State（last_refresh_time、stats）分离——比 TimescaleDB 的 hypertable-continuous_aggregate 双层更紧凑。

**缺陷 2：segment 写入时序乱序处理未核「疑似·功能缺失」**

`ts_segment.c` 458 行未细读，时序写入常遇到乱序事件（IoT 设备时钟漂移），InfluxDB/TimescaleDB 都有显式的 late-arrival 处理（如 time-bucket re-write、merge-on-read）；本卷未核实是否实现。

### 2.6 API 设计

**正面证据**：`ts_sql_functions.c`（456 行）提供 SQL 函数表面，与 Relational 模态 SQL 路径整合——非孤立 API。

**缺陷 1：连续聚合配置 API 不支持 DROP/ALTER「疑似·功能缺失」**

`ts_continuous_agg.c` 提供 config_set_* 与 config_free，但未读到 `cagg_drop/cagg_alter` 入口——生命周期管理只覆盖创建/查询/释放。

## 3. 业界标杆对比

| 维度 | 自实现 | InfluxDB 3.0 | TimescaleDB | TDengine | QuestDB |
|------|--------|--------------|-------------|----------|---------|
| 压缩 | 热路径未压缩、flush 时 Gorilla XOR | Gorilla + ZSTD | Gorilla + ZSTD (10-50x) | 专利列式 | Gorilla + 自适应 |
| 写入吞吐 | ~50-100K 点/s（旧，未复测） | ~100 万 | ~10-50 万 | ~100 万 | ~100 万 |
| 物化视图 | ts_mview + ts_continuous_agg（已落地） | CONTINUOUS QUERIES | 持续聚合 + 重新聚合 | 连续查询 | 物化视图 |
| SQL | ts_sql_functions（456） | Flux/InfluxQL | 标准 PG SQL | 标准 SQL | 标准 SQL |
| 乱序 | 未核 | 原生支持 | Hypertable 分区 | 原生支持 | 原生 |
| TTL | ts_retention（418） | RETENTION POLICY | DROP CHUNK | KEEP | SQL TTL |
| 分布式 | 单机 | IOx K8s 原生 | Citus 分片 | 雾/边缘 | 单机 |
| 专用函数 | 未读全文 | derivative/rate/percentile | 全部 | 全部 | 全部 |

## 4. 差距矩阵

| 维度 | 评分 | 关键证据 |
|------|------|---------|
| 并发正确性 | 3 | 复刻 vector 的 rwlock bug `ts_engine.c:745-770`；use_lock 默认关 `:133` |
| 崩溃恢复 | 3 | ts_engine 无 WAL 集成 grep 零命中；segment 写入无 redo |
| 内存安全 | 5 | destroy 路径完整 `ts_compressor_free:112`；满块静默 -1 `ts_compress.c:144-146`（死代码但语义有害） |
| 错误处理 | 4 | 满块静默 `ts_compress.c:144-146`；连续聚合失败处理未核 |
| 算法实现质量 | 5 | Gorilla XOR 思想保留 `ts_compress.c:40-42`；连续聚合配置完整 `:25-89`；热路径未压缩 |
| API 设计 | 5 | SQL 函数表面完整；连续聚合无 DROP/ALTER 入口（疑似） |

**实现质量缺陷清单（4 项确认 + 3 项疑似）**：
1. 复刻 vector 的 buggy 自旋读写锁（并发）
2. 无 WAL 集成（崩溃）
3. ts_compress_add 满块静默 -1（错误处理）
4. 热路径不压缩——内存 16B/点驻留（功能缺失/性能）
5. 乱序写入处理未核（疑似）
6. 连续聚合刷新失败处理（疑似）
7. 连续聚合无 DROP/ALTER 入口（疑似）

## 5. 改进优先级

| 优先级 | 项目 | 分类 | 工作量 |
|--------|------|------|--------|
| P0 | 统一替换为 pthread_rwlock 或自研 RCU——消灭跨模态复制 bug | 实现质量缺陷 | M |
| P0 | ts_engine 接入 WAL（参照 KV 模式） | 实现质量缺陷 | M |
| P1 | 热路径增量压缩（每个点即时 XOR 编码） | 功能缺失（性能） | M |
| P1 | ts_compress 满块静默 -1 改显式 flush+retry 或返回 KV_FULL 等价错误码 | 实现质量缺陷 | S |
| P2 | 乱序事件 late-arrival 处理 | 功能缺失 | M |
| P2 | 连续聚合 ALTER/DROP API | 功能缺失 | S |
