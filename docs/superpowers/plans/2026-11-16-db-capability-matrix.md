# 两套数据库能力差异矩阵（Stage B0 只读盘点）

盘点日期：2026-11-16。只读核验，未运行任何构建或测试。所有结论均以源码为证据，不采信 README 或计划文档的自我宣称。

## 判定口径

| 档位 | 含义 |
|---|---|
| **实测通过** | 有对应测试且已实际跑绿（旧 DB 侧 2026-11-16 跑过 `make check`） |
| **可用（静态）** | 读源码判定可端到端工作，但本次未运行验证 |
| **有代码但未完成** | 关键函数为 TODO / 空桩 / 简化实现，或调用链断裂 |
| **缺失** | 无实现 |

⚠️ 本次只核验源码，未运行主库 CMake/CTest。因此「可用（静态）」不等于实测通过。

## 一、核心存储层

| 能力 | engine/db（冻结） | engine/src/db（主库） | 关键证据 |
|---|---|---|---|
| Buffer pool / 页管理 | **实测通过** | **有代码但未完成** | 主库 `bufmgr.c:603-625` 命中路径不递增 refcount，pin/unpin 语义不可靠；`buf_write` 仅在 `db_file != NULL` 时落盘（902），CLI 用 `buf_init("")` 导致脏页永不持久化；`disk_write_page` 无 fsync。旧 DB `test_pager.c` 覆盖 10000 页逐出/读回/随机访问全通过 |
| WAL 写入 | **实测通过** | **可用（静态）** | 主库 `wal_write_record`(406-502)/`wal_flush`(730-754 含 fsync) 完整 |
| **WAL 崩溃恢复** | **实测通过** | **缺失（链路断裂）** | 主库 `wal_recover.c:56` 声明 `extern int wal_replay(...)`，**全仓无定义**；CLI/启动路径**零调用** `db_startup_recover`。旧 DB `test_crash.c`（20 轮断电恢复）、`test_ckpt_recovery_time.c` 全通过 |
| 事务状态机 | **实测通过** | **有代码但未完成** | 主库 `mvcc_mark_committed`(mvcc.c:144-150) 与 `mvcc_mark_aborted`(155-159) **均为空实现**；`mvcc_get_status`(164-170) **恒返回 IN_PROGRESS**。后果：可见性规则永远等不到 COMMITTED，已删除行仍可见 |
| **MVCC 可见性** | **实测通过** | **有代码但未完成** | 旧 DB `test_mvcc.c`（RC/RR、版本、abort、dead 判定）、`test_mvcc_it.c` 全通过。主库 `heap_visibility.c:150` 版本链遍历 TODO |
| **Vacuum / 空间回收** | **实测通过** | **有代码但未完成** | 主库 `vacuum_relation`(vacuum.c:105-162) 全 TODO 返回空 stats；`vacuum_count_dead_tuples` 恒 0（303-308）；`vacuum_freeze_all` 空转（197-207）。旧 DB `test_vacuum_compact` 实测 removed=750、compacted=22、freed=29583 |
| Heap AM 元组头 | **实测通过** | **有代码但未完成** | 主库 `heap_insert` **不写 xmin**（heapam.c:334-336 `(void)current_xid`，注释「kv_record_t 无 xmin 字段」）；`heap_update` 是 delete+insert（527-557）；`heap_page_prune` 直接 `return 0`（277-282） |
| Catalog 持久化 | **实测通过** | **有代码但未完成** | 主库 catalog 是纯内存 hash（catalog.c:83-93）；`catalog_init`(143-160) 不加载任何系统表，注释自承「生产环境应从持久化存储加载」。`catalog_persist`/`catalog_recover` 存在但**生产代码零调用**。旧 DB `test_catalog.c` 覆盖元数据读回、重开持久化、损坏检测 |
| BTree AM | **实测通过** | **有代码但未完成** | 主库 `bt_page_get_item`(81-98) 循环内 `(void)key` 从不比较；`btinsert`(185-194) 仅 `btpo_count++` 不落键值；`bt_getnext`(344-350) 把整数当指针返回；无分裂（179-183）；`bt_key_matches` 恒 true（382-383）。旧 DB `test_btree.c` 覆盖 10 万条插入/查找/范围扫描/删除/分裂 |

## 二、查询层

| 能力 | engine/db | engine/src/db | 关键证据 |
|---|---|---|---|
| SQL 解析 | 可用 | **可用（静态）** | 主库 Bison/flex 预生成文件已提交，`sql_parse()`(sql_parse.c:21)；语法覆盖 CREATE/INSERT/UPDATE/DELETE/DROP/SELECT |
| **WHERE 谓词** | **实测通过** | **缺失（显式拒绝）** | 主库 `sql_driver.c:894`（SELECT）、`1369`（UPDATE）、`1535`（DELETE）三处均直接报错「WHERE 暂不支持」；烟测 `test_driver_smoke.cpp:275-288` 强制断言拒绝。旧 DB `test_query.c`、`test_cbo.c`、`test_agg_sqlite.c`（25 条聚合 SQL 与 SQLite 逐格全等）全通过 |
| 聚合 / GROUP BY | **实测通过** | 未确认 | 旧 DB `test_agg_sqlite.c` 25 条全等 |
| 优化器 | 规则 CBO，实测通过 | **有代码但未完成** | 主库 `optimizer_optimize`(optimizer.c:958) 无任何 SQL 侧调用者；`sql/optimizer.c:23-33` 四条规则标「TODO 桩」=false |

## 三、模态

| 模态 | engine/db | engine/src/db | 关键证据 |
|---|---|---|---|
| **blob**（对象→帧→页） | **实测通过** | 有代码但未完成 | 旧 DB `test_blob.c`(11 项)、`test_blob_am.c`(7 项)、`test_blob_it.c` 全通过 |
| **图** | **实测通过**（含崩溃恢复、BFS 多跳、真实下推） | 部分可用 | 主库 BFS 真队列实现（traverse.c:268）；但 **PageRank 是错的**（traverse.c:672-676 仅把自身得分回写给自己，未做入边汇聚）却已暴露为 SQL 函数 `GRAPH_PAGERANK`；`scan_begin` 恒 NULL（graph_engine.c:299-306）。旧 DB `test_graph.c`/`test_graph_am.c`/`test_graph_crash.c`/`test_graph_it.c` 全通过 |
| **向量** | **实测通过**（HNSW recall@10=0.999、KNN、重启一致、崩溃恢复） | 核心可用，外围简化 | 主库 `faiss_hnsw_index_add` 是真 FAISS 算法；但**索引持久化零调用**（`save_index`/`load_index` 全仓无调用者，`table_close` 直接 drop）；WAL 回放未接（`vector_engine.c:350` 只 open）；标量过滤「假设所有结果都满足条件」（2243-2246）；混合检索 BM25 占位（2305-2312）；精排 stub |
| **全文** | **实测通过**（BM25 对照线性扫描逐位全等、多段合并、tombstone、崩溃恢复、页读可证） | 有代码但未完成 | 旧 DB `test_ft_*` 十余个测试全通过 |
| **空间** | **实测通过**（R-tree、精确 KNN、窗口查询、崩溃恢复） | **有代码但危险** | 主库 `rtree_remove`(427-433) 空桩「暂不支持删除」；`spatial_engine_tuple_delete`(442-452) **写入空文件 —— 删除任一条目会清空整库**；子节点满时不分裂（341-359）；`scan_begin` 恒 NULL |
| **时序** | **实测通过**（dod/RLE/Gorilla 三压缩、封窗 60 页→4 页、TTL 整分区回收、崩溃恢复、中途崩溃一致性） | 写入可用，读取不可用 | 主库 scan 恒 NULL（ts_engine.c:281-290）；三处 bucket 聚合「返回空结果」（ts_sql_functions.c:99/171/213）；压缩未集成 |
| **文档 / JSON** | 编解码实测通过，**未接入 SQL** | 简化实现，JSONB 未接入 SQL | 两侧相同结论：JSONB 都未进 SQL。主库 `pipeline $match` 全部放行（doc_pipeline.c:982-994）；JSONPath 过滤表达式直接跳过（jsonpath.c:503-505）；`sql_driver.c:120-137` 类型映射无 jsonb |
| KV | 缺失 | **可用（静态）** | 主库页式 KV 基于 buffer pool，**WAL 真实**（kv.c:396-416 打开即回放，`kv_replay_wal` 943）；TTL 有实现但 Windows 用 `clock()`（CPU 时间而非墙钟，kv_ttl.c:50-52）；LSM `lsm_tree_get` 只查 MemTable（776 TODO）、无调用者 |
| 列存 / 流式 / RDF / 时空 | — | 低可用度 | 列存 `col_tuple_insert` 恒 -1（414-419）无 scan；流式窗口函数 not implemented |

## 四、工程能力

| 能力 | engine/db | engine/src/db |
|---|---|---|
| 分布式 / 分片 / Raft / 2PC | **明确不做**（DESIGN.md:19-28 列为非目标） | 有代码（distributed/consensus/sharding），完成度未核验 |
| 协议层（PG wire / MySQL wire） | 无 | 有（protocol/，PG v3.0 + MySQL 4.1+） |
| RAG | 无 | 有（executor/rag/），部分为预留 |
| 多语言 SDK | 无 | 有（sdk/） |

## 五、规模与门禁

| 项 | engine/db | engine/src/db |
|---|---|---|
| C 源文件 | 134 | 494 |
| 源码行数 | 约 6.8 万 | 约 21 万 |
| 测试文件 | 90 个测试程序 | 289 个 |
| 构建门禁 | `-Werror` 零警告强制 | 非 MSVC 下不启用 `-Werror` |
| 依赖方向门禁 | `tools/check_deps.sh`（L0–L4 分层，硬约束） | CMake 目标依赖，无分层方向检查 |
| 最近实测 | 2026-11-16 `make check`：117 PASS / 0 FAIL | **本次未运行** |

## 六、结论

1. **主库的「核心路径」恰恰是最不完整的部分。** 崩溃恢复（`wal_replay` 无定义）、事务状态（空实现）、vacuum（全 TODO）、Catalog 持久化（零调用）、BTree（不存键值）—— 这五项合起来意味着主库**当前不存在可用的「写到读再到崩溃恢复」端到端流程**。
2. **旧 DB 恰好在这五项上全部实测通过**，且有 90 个测试程序、117 条 PASS 支撑。
3. 主库的优势在**广度和工程面**：分布式/共识/协议/SDK/RAG 这些旧 DB 明确不做。
4. 两侧 JSONB 都未接入 SQL，处于同等状态；旧 DB 的 JSONB 编解码/path/overflow 已有实测通过的 standalone 测试。
5. 「以 `engine/src/db` 为唯一主内核」这一既定决策，与「旧 DB 有更完整的核心存储层」这一事实**存在实质冲突**，需要重新裁决。

## 七、需要用户决定的三条路线

- **路线 A：维持 src/db 为主内核**，则必须先补齐其崩溃恢复/事务/vacuum/Catalog/BTree 五处核心缺口（工作量约等于重写存储层），旧 DB 仅作参考。
- **路线 B：以 engine/db 核心存储层为主**，把 src/db 的工程面（协议、分布式、SDK、RAG）向下适配到旧 DB 的接口。旧 DB 的 SPI（StorageAM/IndexAM）已有清晰契约。
- **路线 C：按能力分层共用**，抽出共同的 pager/WAL/txn 底座（以实测通过的那套为准），上层各取所需。代价是两套上层代码都要改。

本次只读盘点未做任何选择，也未修改任何代码。
