---
project: postgres
feature: logical-replication-apply
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/logical/worker.c
    symbols: [ApplyWorkerMain, apply_dispatch, apply_handle_insert]
  - path: src/backend/replication/logical/launcher.c
    symbols: [ApplyLauncherMain, logicalrep_worker_launch]
---

# 逻辑复制应用

## 一句话职责

订阅端的 apply 系列后台 worker 接收发布端按逻辑复制协议推送的事务变更消息，把其中的行变更与 DDL 元数据在本地库上真正执行，并回送进度确认。

## 机制是什么

订阅端不需要 WAL 读取权：它以普通客户端身份连接发布端，用 libpqwalreceiver 走 `START_REPLICATION`，收到的是已经解码并序列化成协议消息的流（BEGIN/COMMIT/INSERT/UPDATE/DELETE/RELATION 等）。`worker.c` 负责把这些消息翻译成本地 SQL 执行——打开本地关系、按 relation map 建立发布端列与本地列的对应、设置会话用户、执行行操作并维护事务边界。launcher 进程不处理数据，它周期性扫描 `pg_subscription`，为每个启用的订阅保证一个 leader apply worker 存活，按需拉起 tablesync、sequencesync 与 parallel apply 子 worker。

本页只管订阅端的 worker 生命周期与消息到本地执行。不复述发布端的解码与快照构建（i04 与 i07），不描述表的初始数据拷贝流程细节（i08），也不负责 origin 进度状态的存储机制（i09）——这里只是作为调用方使用 `replorigin_session_setup` 与 `replorigin_xact_state`。物理流复制协议本身（i01）与此无关。

## 怎么实现的

1. `logicalrep_worker_launch` 在 `LogicalRepWorkerLock` 保护下取一个 `LogicalRepCtx->workers[]` 空槽，登记 `type`/`dbid`/`subid`/`relid`/`generation` 等字段，构造 `BackgroundWorker` 描述（`ApplyWorkerMain`、`TableSyncWorkerMain`、`ParallelApplyWorkerMain`、`SequenceSyncWorkerMain` 之一）并 `RegisterDynamicBackgroundWorker`；启动失败要按 `generation` 校验后清理槽位，成功则等待 worker attach。
2. 请求类型决定受限资源：tablesync/sequencesync 受 `max_sync_workers_per_subscription` 限制，parallel apply 受 `max_parallel_apply_workers_per_subscription` 限制，全部 worker 受 `max_logical_replication_workers` 限制；额度不足时 tablesync 请求静默返回 false，完全无空槽才告警。
3. `ApplyLauncherMain` 每轮在临时内存上下文中取订阅列表，跳过未启用订阅，已运行者跳过；未运行且距上次启动已超过 `wal_retrieve_retry_interval` 才重新拉起，避免故障订阅被无限快速重启。若订阅开启 `retain_dead_tuples`，先建立/维护冲突检测槽位并计算要保护的 `oldest_nonremovable_xid` 下限。
4. `ApplyWorkerMain` 先走 `SetupApplyOrSyncWorker` 完成 attach、信号设置与 libpqwalreceiver 加载，再进 `run_apply_worker`：连接发布端、`replorigin_by_name`/`replorigin_create` 取得 origin、`replorigin_session_setup` 绑定会话 origin 并从 origin 进度得到 `origin_startpos`，然后设置流选项并进入 `start_apply`。
5. `LogicalRepApplyLoop` 读到一条消息后交给 `apply_dispatch`，后者按消息类型分派到对应 handler。`apply_handle_begin` 记录远端 xid 与 `final_lsn` 并置 `in_remote_transaction`；行消息逐条处理；`apply_handle_commit` 校验 `commit_lsn` 与 `remote_ctx.finish_lsn` 一致，提交本地事务并回送确认。
6. `apply_handle_insert` 在跳过模式或流式事务处理分支先行返回，否则 `logicalrep_read_insert` 解析远端元组、`logicalrep_rel_open` 加 `RowExclusiveLock` 打开本地关系，用 `should_apply_changes_for_rel` 过滤不发布的关系；随后按 `runasowner` 切换到表 owner 用户，建 `ApplyExecutionData`/`EState`、把远端元组装进 virtual slot 并填默认值，普通表直接 `ExecSimpleRelationInsert`，分区表则走 `apply_handle_tuple_routing` 找目标分区。
7. 每条行消息被 `begin_replication_step`/`end_replication_step` 包住以界定内部执行步；出错时 `start_apply` 的 PG_CATCH 先 `replorigin_xact_clear` 阻止 origin 进度被错误推进，再根据 `disableonerr` 走 `DisableSubscriptionAndExit` 或中止事务后重抛。

## 关键取舍

- **源码事实：**leader apply worker 的连接 `application_name` 用订阅名，tablesync 用同步槽位名，以便发布端区分二者（同步复制、槽位命名都依赖这一点）。放弃统一命名换来可分辨的会话身份。
- **源码事实：**launcher 对同一订阅的 apply worker 重启用 `wal_retrieve_retry_interval` 节流，避免错误订阅把 worker 反复拉起。代价是配置变更后需要等待，除非有进程显式清除该订阅的 last-start 记录（`ApplyLauncherForgetWorkerStartTime` 的调用点如 `DisableSubscriptionAndExit`）。
- **源码事实：**行应用默认以表 owner 身份执行（除非 `run_as_owner` 为真），并对目标表做 `ACL_INSERT` 等权限检查；这使触发器/默认表达式以 owner 权限运行，同时也意味着订阅的普通用户不能借复制绕过目标表权限。
- **源码事实：**`apply_handle_commit` 会严格比对远端 commit LSN 与已记录的 `finish_lsn`，不一致直接报协议违规；宁可中断也不接受位置错乱的消息序列，因为错位会破坏 origin 进度与断点续传的可信度。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/logical/worker.c` | `ApplyWorkerMain` | leader apply worker 入口：attach 槽位、初始化环境后进入应用主循环 |
| `src/backend/replication/logical/worker.c` | `apply_dispatch` | 按逻辑复制协议消息类型分派到各 handler 并保存/恢复当前命令上下文 |
| `src/backend/replication/logical/worker.c` | `apply_handle_insert` | 把一条远端 INSERT 消息解析为本地插入，含关系映射、权限切换与分区路由 |
| `src/backend/replication/logical/launcher.c` | `ApplyLauncherMain` | 周期扫描订阅，按节流策略保证每个订阅的 apply worker 存在 |
| `src/backend/replication/logical/launcher.c` | `logicalrep_worker_launch` | 分配 worker 共享槽位、注册动态后台 worker 并等待其 attach |

## 与主核 / AI 的关系

暂无直接对应。

## 参考

- [PostgreSQL 文档：逻辑复制](https://www.postgresql.org/docs/current/logical-replication.html) —— 支撑订阅端 worker 应用变更的整体模型。
- [PostgreSQL 官方源码：worker.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/logical/worker.c) —— 支撑消息分派、行应用与冲突/错误处理。
- [PostgreSQL 官方源码：launcher.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/logical/launcher.c) —— 支撑 apply worker 的启动、节流与额度限制。
