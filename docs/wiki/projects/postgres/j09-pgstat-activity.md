---
project: postgres
feature: pgstat-activity
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/activity/pgstat.c
    symbols: [pgstat_report_stat, pgstat_build_snapshot, pgstat_read_statsfile]
  - path: src/backend/utils/activity/backend_status.c
    symbols: [pgstat_report_activity]
---

# 运行统计

## 一句话职责

运行统计把各后端的活动计数器汇总进共享内存、按一致性级别为查询提供快照视图，并在服务正常关停时把累计统计落到磁盘、下次启动时读回。

## 机制是什么

统计数据有两类存储形态。**固定数量统计**（如 checkpointer、bgwriter、WAL、archiver 等每类只有一份）直接放在 `pgStatLocal.shmem` 的固定偏移里；**变长统计**（表、索引、函数、数据库等按对象计数的）放在 `dshash` 哈希表加 `dsa` 共享内存段里，每个条目内部再用一把 `LWLock` 保护其数据。后端本地则把增量攒在 `pgStatPending` 里，按需 flush 到共享内存，因此普通执行路径不需要为每次计数去抢共享锁。

读取侧由 GUC `stats_fetch_consistency` 决定：`none` 每次取都重建、`cache` 缓存固定统计、`snapshot` 在开始取时把可变统计整体拷进本进程私有 hash 表，使后续多次读取看到同一批次。本页负责“收集 → flush → 快照 → 落盘/读回”这条链路；具体每个统计种类的计数器含义（如 `pg_stat_activity` 各列、`pg_stat_user_tables` 各列）属于各自的视图语义，不在本页展开，autovacuum 如何消费统计选择候选库也不在本页。

## 怎么实现的

1. `pgstat_report_stat(bool force)` 只在没有活跃事务时调用（源码断言 `!IsTransactionOrTransactionBlock()`），因为它要用事务停止时间做节奏判定。先吸收 `pgStatForceNextFlush` 的强制请求；若 `pgStatPending` 为空且 `pgstat_report_fixed` 为假就立刻返回 0，连时钟都不读。
2. 非强制路径用 `GetCurrentTransactionStopTimestamp()` 做时基。若挂起已超过 `PGSTAT_MAX_INTERVAL`（60000ms）则升级为强制；若距上次 flush 不足 `PGSTAT_MIN_INTERVAL`（1000ms）就记录 `pending_since` 并返回 `PGSTAT_IDLE_INTERVAL`（10000ms），让调用方知道下次再来的最早时刻。
3. 真正 flush 时 `nowait = !force`：变长统计走 `pgstat_flush_pending_entries(nowait)`，固定统计则遍历所有 `PgStat_Kind` 调各自的 `flush_static_cb`。`nowait` 的语义是“抢不到共享锁就先跳过”，所以非强制 flush 允许部分成功：一旦 `partial_flush` 为真就保留 `pending_since` 并返回 `PGSTAT_IDLE_INTERVAL`，等下一次再来；全部成功才清 `pending_since` 与 `pgstat_report_fixed`。
4. 会话活动由 `pgstat_report_activity(state, cmd_str)` 写入自己的 `PgBackendStatus`：若 `track_activities` 被关闭，它只做**一次收尾写**——把 `st_state` 置 `STATE_DISABLED` 并清空活动文本、各类起始时间戳与 `wait_event_info`，之后不再更新。开启时先算好待写数据（`len = Min(strlen(cmd_str), pgstat_track_activity_query_size - 1)`、语句起始时间、当前时间），再进临界写。
5. 状态切换时补记时长：旧状态是 `STATE_RUNNING`/`STATE_FASTPATH` 就累加 `pgstat_count_conn_active_time`，是 `STATE_IDLEINTRANSACTION`/`STATE_IDLEINTRANSACTION_ABORTED` 就累加 `pgstat_count_conn_txn_idle_time`，差值来自 `beentry->st_state_start_timestamp` 到当前时间。进入 `STATE_RUNNING` 会把 `st_query_id`/`st_plan_id` 归零，避免把上一条语句的标识当成新语句的。
6. 写入 `beentry` 统一用 `PGSTAT_BEGIN_WRITE_ACTIVITY` / `PGSTAT_END_WRITE_ACTIVITY` 包裹，靠前后各增一次 `st_changecount` 让读取方（`pg_stat_get_activity` 一侧）能识别撕裂读并重试；这是为“一个后端写、任意后端读”的共享状态设计的无锁读协议。
7. `pgstat_build_snapshot()` 仅在 `stats_fetch_consistency = snapshot` 下被调用，且已建好就直接返回。它为每个变长 hash 用 `dshash_seq_init/next` 顺序扫描：跳过属于其它数据库且 `accessed_across_databases` 为假的条目、跳过 `dropped` 条目，为每条目在私有 context 里分配 `pgstat_get_entry_len(kind)` 大小的副本，在 `stats_data->lock` 共享锁下 `memcpy` 整块数据。固定统计随后按 kind 逐个 `pgstat_build_snapshot_fixed()`。
8. `pgstat_build_snapshot_fixed(kind)` 的行为随一致性级别分三支：`none` 每次把 `valid[idx]` 置假强制重建；`cache` 若已 `valid[idx]` 为真直接返回；`snapshot` 下若被重复调用则触发断言，因为快照模式不应二次构建。真正构建走 kind 自己的 `snapshot_cb()`，成功后置 `valid[idx] = true`。
9. 落盘发生在关停路径：`pgstat_before_server_shutdown` 先 `pgstat_report_stat(true)` 把自己挂起的增量刷掉，然后**只在退出码为 0（正常关停）**时置 `shmem->is_shutdown = true` 并调 `pgstat_write_statsfile()`；异常关停不写文件，也不置 shutdown 标记，因为此时无法保证自己是最后退出的进程。源码注释明确这个回调必须由“恰好一个进程”调用，实际是 checkpointer 通过 `before_shmem_exit` 注册的。
10. `pgstat_write_statsfile` 断言调用者是 `B_CHECKPOINTER`（或单用户模式），先把 `stats_fetch_consistency` 强制改成 `none` 以免写盘过程中重建快照，再写临时文件 `PGSTAT_STAT_PERMANENT_TMPFILE`：头部写 `PGSTAT_FILE_FORMAT_ID`，然后对每个 `fixed_amount` 且 `write_to_file` 的 kind 写 `PGSTAT_FILE_ENTRY_FIXED` + kind + 数据；变长条目按 `PgStat_HashKey` 或名字序列化。
11. `pgstat_read_statsfile` 反向恢复：它由 `pgstat_restore_stats()` 在启动早期调用，断言不在 postmaster 里。文件不存在（`ENOENT`）视为“还没有统计”而不是错误，走 `pgstat_reset_after_failure()` 并置 `STATS_DISCARD`；其它 open 失败记 LOG 同样降级。format ID 不匹配、条目类型未知、kind 非法、`fixed_amount` 与条目类型不符、重复 key 等任何异常都记 WARNING 后 `goto error`，最终记 LOG “corrupted statistics file” 并整体丢弃。
12. 恢复时固定统计直接写进 `shmem` 的 `shared_ctl_off + shared_data_off` 偏移或自定义 kind 的 `custom_data[idx]`；变长条目先 `pgstat_alloc_entry_body` 分配 DSA body，再 `dshash_find_or_insert_extended(..., DSHASH_INSERT_NO_OOM)` 插入，失败（OOM）选择 `elog(ERROR)` 而非降级为 WARNING——源码注释说明若降级，环境 OOM 会导致**全部统计被丢弃**，因此宁可报错。名字型条目可通过 `from_serialized_name` 判定“不关心”的条目并 `fseek` 跳过其数据。
13. 读完后必须 `PGSTAT_FILE_ENTRY_END` 之后紧跟 `EOF`，否则按损坏处理；成功路径 `unlink(statfile)` —— 统计文件是一次性交接介质，读完即删，避免下次启动再读到旧内容。最后对所有 kind 调 `finish(status)` 回调，用 `STATS_READ`/`STATS_DISCARD` 告知最终结果。

## 关键取舍

- 源码事实：`pgstat_report_stat` 非强制路径用返回值的毫秒数（`PGSTAT_IDLE_INTERVAL` = 10000，`PGSTAT_MIN_INTERVAL` = 1000，`PGSTAT_MAX_INTERVAL` = 60000）来指导调用方何时再来，且 flush 抢不到锁时保留 `pending_since` 返回 idle 间隔。架构推论：把“什么时候 flush”做成调用方驱动的建议值，而不是后台线程定时器，让统计上报搭在既有的会话空闲周期上；代价是上报延迟受调用方节奏影响，因此必须靠 `PGSTAT_MAX_INTERVAL` 兜底防止无限堆积，并由 `postgres.c` 在进入 idle 时按返回值设超时。
- 源码事实：`pgstat_report_stat` 在 `!force` 时用 `nowait` 抢锁，允许 `partial_flush` 部分成功；`force` 时断言不可能部分失败。架构推论：普通查询路径不能被统计上报的锁竞争拖慢，宁可这一轮少报一些；而关停/进程退出等强制路径必须完整落地，否则数据丢失。若所有 flush 都阻塞等锁，高并发下统计会成为写入路径的新争用点。
- 源码事实：`pgstat_build_snapshot` 只在 `stats_fetch_consistency = snapshot` 下跑，且把变长条目整块 `memcpy` 到私有 hash 表、固定条目按 kind 缓存 `valid` 位。架构推论：用“每会话拷一份”换取一次查询内多次读取的一致性（避免同一视图不同行来自不同瞬间）；代价是快照内存开销与一次全量拷贝，且写方仍可能在你拷贝中途更新，只能靠条目级共享锁保证单条不撕裂，跨条目一致性靠“同一时间窗”近似。
- 源码事实：`pgstat_read_statsfile` 对 OOM 与重复 key 用 `elog(ERROR)`，但对格式/内容异常只用 WARNING 后整体 `STATS_DISCARD`；`pgstat_before_server_shutdown` 只在 `code == 0` 时写文件并置 `is_shutdown`。架构推论：正常关停才值得信任“我是最后一个退出、共享内存状态完整”，异常关停直接丢弃统计更安全；读取时区分“环境资源不足”（重试也无意义但不应静默丢弃，故报错）与“文件本身不可信”（可安全重置为全零）。
- 源码事实：`pgstat_report_activity` 在 `track_activities` 关闭时只做一次 `STATE_DISABLED` 收尾写，并在状态切换时把时长累加进 `pgstat_count_conn_active_time`/`pgstat_count_conn_txn_idle_time`。架构推论：关闭采样不能让共享条目停在过期状态（否则 `pg_stat_activity` 会长期显示一个虚假的 active 会话），也不能丢掉已经发生的累计时长，因此在“关掉”这一刻做最后一次结算；代价是该函数必须记住旧状态与起始时间，并依赖 `st_changecount` 协议让读方看到一致结果。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/activity/pgstat.c` | `pgstat_report_stat` | 后端增量统计的 flush 调度：按 min/max 间隔与强制标记决定是否上报，用 nowait 抢锁允许部分成功，并返回下次建议时刻。 |
| `src/backend/utils/activity/pgstat.c` | `pgstat_build_snapshot` | 按 `stats_fetch_consistency` 建立一致视图：扫描各变长 hash 拷入私有 hash，再构建固定统计，供一次查询内多次读取复用。 |
| `src/backend/utils/activity/pgstat.c` | `pgstat_read_statsfile` | 启动时从统计文件恢复：校验 format ID 与条目布局，把固定统计写入共享内存固定偏移、把变长条目插入 dshash，异常则整体丢弃并重置。 |
| `src/backend/utils/activity/backend_status.c` | `pgstat_report_activity` | 维护本会话的 `PgBackendStatus`：写状态与活动文本、结算上一状态的持续时长、按 `st_changecount` 协议让读方安全读取。 |

## 与主核 / AI 的关系

主核没有 PostgreSQL 这种“共享内存固定统计 + dshash 变长统计 + 会话私有快照 + 关停落盘”的运行统计体系，也没有 `pg_stat_activity` 这类会话状态共享条目。可借鉴的是把高频计数留在本地、按间隔批量 flush，并读侧显式区分 `none`/`cache`/`snapshot` 三档一致性；不要把它当作主核已有的指标系统。

## 参考

- [PostgreSQL 文档：监控数据库活动](https://www.postgresql.org/docs/current/monitoring-stats.html) —— 说明统计视图、`pg_stat_activity` 与会话状态字段。
- [PostgreSQL 文档：统计收集器配置](https://www.postgresql.org/docs/current/runtime-config-statistics.html) —— 说明 `track_activities`、`stats_fetch_consistency` 等参数语义。
- [PostgreSQL 官方源码：pgstat 实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/activity/pgstat.c) —— 支撑 flush 节奏、快照构建与统计文件读写事实。
