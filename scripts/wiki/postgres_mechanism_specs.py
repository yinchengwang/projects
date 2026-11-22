# -*- coding: utf-8 -*-
"""PostgreSQL 机制清单（人工审核后定稿，P2a 产物）。

本模块是 `_manifest.yml` 的唯一数据来源：一个条目 = 一个机制单元 = 一个页面。
条目顺序即 manifest 页序，`page_file` 已按组内序号固定，不允许生成期再推导。

refs 的每个 symbol 都在 Universal Ctags 6.1.0 对
`postgres@9b5d4e3331753b3399e84965093109cac8f364bf` 的 `kind=function` 输出中
以同 `path` 命中；词边界命中只是渲染门禁，不代表语义正确，机制归属仍由
`boundary` 描述并受人工审阅。
"""

GROUP_ORDER = [
    'A-存储与IPC',
    'B-事务与WAL',
    'C-堆表与元组',
    'D-索引族',
    'E-优化器',
    'F-执行器',
    'G-目录与缓存',
    'H-内存与排序',
    'I-复制与高可用',
    'J-服务端与命令面',
    'K-SQL处理与安全',
    'L-SQL类型与内建函数',
]

MECHANISM_SPECS = [
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'buffer-pool',
        "title": '共享缓冲池与替换',
        "page_file": 'a01-buffer-pool.md',
        "boundary": '共享 buffer 分配、pin/victim、脏页写出；不含 backend-local pool',
        "refs": [
            {
                "path": 'src/backend/storage/buffer/bufmgr.c',
                "symbols": ['ReadBufferExtended', 'BufferAlloc', 'FlushBuffer'],
            },
            {
                "path": 'src/backend/storage/buffer/freelist.c',
                "symbols": ['StrategyGetBuffer', 'StrategySyncStart'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'local-buffer-pool',
        "title": '后端本地缓冲池',
        "page_file": 'a02-local-buffer-pool.md',
        "boundary": '临时/本地关系缓冲的独立分配、刷新与释放；无跨 backend 竞争语义',
        "refs": [
            {
                "path": 'src/backend/storage/buffer/localbuf.c',
                "symbols": ['LocalBufferAlloc', 'FlushLocalBuffer'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'procarray-snapshot',
        "title": '活跃事务集合与 MVCC 快照',
        "page_file": 'a03-procarray-snapshot.md',
        "boundary": 'ProcArray 成员生命周期与 snapshot xmin/xmax 构造；不讲 XID 持久状态',
        "refs": [
            {
                "path": 'src/backend/storage/ipc/procarray.c',
                "symbols": ['ProcArrayAdd', 'ProcArrayRemove', 'GetSnapshotData'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'ssi-predicate-locks',
        "title": 'SSI 谓词锁与冲突检测',
        "page_file": 'a04-ssi-predicate-locks.md',
        "boundary": '可串行化快照隔离的 predicate lock 与读写冲突判定；不含 heavyweight lock',
        "refs": [
            {
                "path": 'src/backend/storage/lmgr/predicate.c',
                "symbols": ['CheckForSerializableConflictOut', 'CheckForSerializableConflictIn', 'PredicateLockRelation'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'lock-manager-deadlock',
        "title": '常规锁与死锁检测',
        "page_file": 'a05-lock-manager-deadlock.md',
        "boundary": '锁表、关系锁入口、等待队列与等待图环检测；不含 LWLock / SSI',
        "refs": [
            {
                "path": 'src/backend/storage/lmgr/lock.c',
                "symbols": ['LockAcquireExtended', 'LockRelease'],
            },
            {
                "path": 'src/backend/storage/lmgr/lmgr.c',
                "symbols": ['LockRelationOid', 'UnlockRelationOid'],
            },
            {
                "path": 'src/backend/storage/lmgr/deadlock.c',
                "symbols": ['DeadLockCheck'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'fd-cache-file-io',
        "title": '虚拟文件描述符与文件 I/O',
        "page_file": 'a06-fd-cache-file-io.md',
        "boundary": 'VFD 缓存、文件开关与 fsync 持久化',
        "refs": [
            {
                "path": 'src/backend/storage/file/fd.c',
                "symbols": ['PathNameOpenFile', 'FileClose', 'pg_fsync'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'backend-proc-slots',
        "title": '后端进程槽',
        "page_file": 'a07-backend-proc-slots.md',
        "boundary": 'PGPROC 初始化、进程级共享状态与睡眠等待；不讲 snapshot 集合',
        "refs": [
            {
                "path": 'src/backend/storage/lmgr/proc.c',
                "symbols": ['InitProcess', 'InitProcessPhase2', 'ProcSleep'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'smgr-md-storage',
        "title": '关系物理存储（smgr 与 md）',
        "page_file": 'a08-smgr-md-storage.md',
        "boundary": 'smgr 路由与 md 段文件创建/扩展/读写',
        "refs": [
            {
                "path": 'src/backend/storage/smgr/smgr.c',
                "symbols": ['smgropen'],
            },
            {
                "path": 'src/backend/storage/smgr/md.c',
                "symbols": ['mdcreate', 'mdextend'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'wait-event-sets',
        "title": '等待事件集',
        "page_file": 'a09-wait-event-sets.md',
        "boundary": 'fd 注册、多路等待与事件返回',
        "refs": [
            {
                "path": 'src/backend/storage/ipc/waiteventset.c',
                "symbols": ['CreateWaitEventSet', 'AddWaitEventToSet', 'WaitEventSetWait'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'lwlocks',
        "title": '轻量锁 LWLock',
        "page_file": 'a10-lwlocks.md',
        "boundary": 'tranche 初始化与 acquire/release；与常规锁分界',
        "refs": [
            {
                "path": 'src/backend/storage/lmgr/lwlock.c',
                "symbols": ['LWLockAcquire', 'LWLockRelease', 'LWLockInitialize'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'read-stream-prefetch',
        "title": '扫描预读流',
        "page_file": 'a11-read-stream-prefetch.md',
        "boundary": '消费端批量读 buffer 与预取接口；调用方不属于该机制',
        "refs": [
            {
                "path": 'src/backend/storage/aio/read_stream.c',
                "symbols": ['read_stream_begin_impl', 'read_stream_next_buffer', 'read_stream_end'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'shared-memory-allocation',
        "title": '主共享内存分配',
        "page_file": 'a12-shared-memory-allocation.md',
        "boundary": '主共享区请求与命名结构/哈希初始化；排除 DSM',
        "refs": [
            {
                "path": 'src/backend/storage/ipc/shmem.c',
                "symbols": ['ShmemInitStruct'],
            },
            {
                "path": 'src/backend/storage/ipc/shmem_hash.c',
                "symbols": ['ShmemInitHash'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'dynamic-shared-memory',
        "title": '动态共享内存 DSM',
        "page_file": 'a13-dynamic-shared-memory.md',
        "boundary": 'DSM 控制段生命周期与平台实现抽象',
        "refs": [
            {
                "path": 'src/backend/storage/ipc/dsm.c',
                "symbols": ['dsm_create', 'dsm_attach', 'dsm_detach'],
            },
            {
                "path": 'src/backend/storage/ipc/dsm_impl.c',
                "symbols": ['dsm_impl_op', 'dsm_impl_pin_segment', 'dsm_impl_unpin_segment'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'shared-memory-message-queues',
        "title": '共享内存消息队列',
        "page_file": 'a14-shared-memory-message-queues.md',
        "boundary": '有界队列的发送/接收与背压',
        "refs": [
            {
                "path": 'src/backend/storage/ipc/shm_mq.c',
                "symbols": ['shm_mq_create', 'shm_mq_send', 'shm_mq_receive'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'aio-io-methods',
        "title": '异步 I/O 请求与执行后端',
        "page_file": 'a15-aio-io-methods.md',
        "boundary": 'AIO 请求/完成生命周期及 worker/io_uring 后端；后端差异作分节',
        "refs": [
            {
                "path": 'src/backend/storage/aio/aio.c',
                "symbols": ['pgaio_io_acquire', 'pgaio_io_release'],
            },
            {
                "path": 'src/backend/storage/aio/method_worker.c',
                "symbols": ['pgaio_worker_submit'],
            },
            {
                "path": 'src/backend/storage/aio/method_io_uring.c',
                "symbols": ['pgaio_uring_check_capabilities', 'pgaio_uring_context_shmem_size'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'free-space-map',
        "title": '空闲空间映射 FSM',
        "page_file": 'a16-free-space-map.md',
        "boundary": '空间记录、查询与候选页选择',
        "refs": [
            {
                "path": 'src/backend/storage/freespace/freespace.c',
                "symbols": ['GetPageWithFreeSpace', 'RecordPageWithFreeSpace', 'RecordAndGetPageWithFreeSpace'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'temporary-buffiles',
        "title": '临时 BufFile',
        "page_file": 'a17-temporary-buffiles.md',
        "boundary": '分段临时文件与 fileset 生命周期',
        "refs": [
            {
                "path": 'src/backend/storage/file/buffile.c',
                "symbols": ['BufFileCreateTemp', 'BufFileCreateFileSet', 'BufFileOpenFileSet'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'process-signals-barriers',
        "title": '进程信号与屏障',
        "page_file": 'a18-process-signals-barriers.md',
        "boundary": '进程信号投递与 barrier 协同',
        "refs": [
            {
                "path": 'src/backend/storage/ipc/procsignal.c',
                "symbols": ['ProcSignalInit', 'SendProcSignal', 'EmitProcSignalBarrier'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'parallel-query-dsm',
        "title": '并行查询 DSM 基础设施',
        "page_file": 'a19-parallel-query-dsm.md',
        "boundary": 'parallel context、DSM 初始化与 worker 启动；执行器 Gather 归 F',
        "refs": [
            {
                "path": 'src/backend/access/transam/parallel.c',
                "symbols": ['CreateParallelContext', 'InitializeParallelDSM', 'LaunchParallelWorkers'],
            },
        ],
    },
    {
        "feature_group": 'A-存储与IPC',
        "feature": 'shared-invalidation',
        "title": '共享失效消息传输原语',
        "page_file": 'a20-shared-invalidation.md',
        "boundary": '共享失效消息缓冲、插入与消费的 IPC 原语；缓存对象语义归 G',
        "refs": [
            {
                "path": 'src/backend/storage/ipc/sinvaladt.c',
                "symbols": ['SIInsertDataEntries', 'SIGetDataEntries', 'SharedInvalBackendInit'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'wal-insertion-and-write',
        "title": 'WAL 记录插入与写出',
        "page_file": 'b01-wal-insertion-and-write.md',
        "boundary": 'record 组装、插入、WAL buffer 与写出/flush 基础流程',
        "refs": [
            {
                "path": 'src/backend/access/transam/xloginsert.c',
                "symbols": ['XLogBeginInsert', 'XLogRegisterBuffer', 'XLogInsert'],
            },
            {
                "path": 'src/backend/access/transam/xlog.c',
                "symbols": ['XLogInsertRecord', 'XLogWrite', 'XLogFlush'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'checkpoint-and-wal-flush',
        "title": 'Checkpoint 与持久化屏障',
        "page_file": 'b02-checkpoint-and-wal-flush.md',
        "boundary": 'checkpoint 编排与数据同步；不重复解释 record 插入',
        "refs": [
            {
                "path": 'src/backend/access/transam/xlog.c',
                "symbols": ['CreateCheckPoint', 'CheckPointGuts', 'XLogFlush'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'wal-recovery-and-restore',
        "title": 'WAL 恢复与归档取回',
        "page_file": 'b03-wal-recovery-and-restore.md',
        "boundary": 'startup redo、redo buffer 辅助、archive restore 与 timeline',
        "refs": [
            {
                "path": 'src/backend/access/transam/xlogrecovery.c',
                "symbols": ['PerformWalRecovery', 'InitWalRecovery'],
            },
            {
                "path": 'src/backend/access/transam/xlogutils.c',
                "symbols": ['XLogReadBufferForRedo'],
            },
            {
                "path": 'src/backend/access/transam/xlogarchive.c',
                "symbols": ['RestoreArchivedFile'],
            },
            {
                "path": 'src/backend/access/transam/timeline.c',
                "symbols": ['readTimeLineHistory'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'wal-reader',
        "title": '通用 WAL Reader',
        "page_file": 'b04-wal-reader.md',
        "boundary": 'WAL 跨页读取与 record 解码；不负责 redo 执行',
        "refs": [
            {
                "path": 'src/backend/access/transam/xlogreader.c',
                "symbols": ['XLogReaderAllocate', 'XLogReadRecord', 'XLogReaderFree'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'standby-recovery-conflict',
        "title": '热备恢复冲突',
        "page_file": 'b05-standby-recovery-conflict.md',
        "boundary": 'standby 查询与恢复的冲突判定/取消/日志；WAL 恢复本身见上页',
        "refs": [
            {
                "path": 'src/backend/storage/ipc/standby.c',
                "symbols": ['InitRecoveryTransactionEnvironment', 'ResolveRecoveryConflictWithSnapshot'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'multixact-member-status',
        "title": 'MultiXact 成员与状态',
        "page_file": 'b06-multixact-member-status.md',
        "boundary": 'MultiXact 创建、成员读取、运行状态与维护',
        "refs": [
            {
                "path": 'src/backend/access/transam/multixact.c',
                "symbols": ['MultiXactIdCreate', 'GetMultiXactIdMembers', 'MultiXactIdIsRunning'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'two-phase-commit',
        "title": '两阶段提交',
        "page_file": 'b07-two-phase-commit.md',
        "boundary": 'PREPARE 状态文件与 prepared transaction 完成',
        "refs": [
            {
                "path": 'src/backend/access/transam/twophase.c',
                "symbols": ['MarkAsPreparing', 'EndPrepare', 'FinishPreparedTransaction'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'transaction-status-slru',
        "title": 'SLRU 状态页缓存实现',
        "page_file": 'b08-transaction-status-slru.md',
        "boundary": 'SLRU 共享缓存读/写/同步抽象；仅为支撑层，不等于具体事务语义',
        "refs": [
            {
                "path": 'src/backend/access/transam/slru.c',
                "symbols": ['SimpleLruReadPage', 'SimpleLruWritePage'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'xid-commit-status',
        "title": 'XID 提交状态（CLOG/pg_xact）',
        "page_file": 'b09-xid-commit-status.md',
        "boundary": '事务树提交状态读写；不重讲 SLRU 实现',
        "refs": [
            {
                "path": 'src/backend/access/transam/clog.c',
                "symbols": ['TransactionIdSetTreeStatus', 'TransactionIdGetStatus'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'subtransaction-parent-map',
        "title": '子事务父映射',
        "page_file": 'b10-subtransaction-parent-map.md',
        "boundary": 'subXID→parent XID 映射',
        "refs": [
            {
                "path": 'src/backend/access/transam/subtrans.c',
                "symbols": ['SubTransSetParent', 'SubTransGetParent'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'commit-timestamp',
        "title": '提交时间戳存储',
        "page_file": 'b11-commit-timestamp.md',
        "boundary": 'commit ts 记录与查询',
        "refs": [
            {
                "path": 'src/backend/access/transam/commit_ts.c',
                "symbols": ['TransactionTreeSetCommitTsData', 'TransactionIdGetCommitTsData'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'xid-allocation',
        "title": 'XID 分配与防回卷',
        "page_file": 'b12-xid-allocation.md',
        "boundary": 'XID/full-XID 分配与推进',
        "refs": [
            {
                "path": 'src/backend/access/transam/varsup.c',
                "symbols": ['GetNewTransactionId', 'ReadNextFullTransactionId', 'AdvanceNextFullTransactionIdPastXid'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'transaction-lifecycle-and-status',
        "title": '事务生命周期与状态接口',
        "page_file": 'b13-transaction-lifecycle-and-status.md',
        "boundary": '事务块 begin/commit/abort 与状态判定；持久状态字节属 CLOG 页',
        "refs": [
            {
                "path": 'src/backend/access/transam/xact.c',
                "symbols": ['StartTransaction', 'CommitTransaction', 'AbortTransaction'],
            },
            {
                "path": 'src/backend/access/transam/transam.c',
                "symbols": ['TransactionIdDidCommit', 'TransactionIdDidAbort', 'TransactionIdCommitTree'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'recovery-wal-prefetch',
        "title": 'Redo WAL 预取',
        "page_file": 'b14-recovery-wal-prefetch.md',
        "boundary": '恢复期按 WAL 预取数据块；与 A 的通用 read-stream 分界',
        "refs": [
            {
                "path": 'src/backend/access/transam/xlogprefetcher.c',
                "symbols": ['XLogPrefetcherAllocate', 'XLogPrefetcherReadRecord', 'XLogPrefetchReconfigure'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'wal-resource-manager',
        "title": 'WAL rmgr 分派',
        "page_file": 'b15-wal-resource-manager.md',
        "boundary": 'rmgr 注册与 startup/cleanup 分派；不含描述器与具体 redo',
        "refs": [
            {
                "path": 'src/backend/access/transam/rmgr.c',
                "symbols": ['RmgrStartup', 'RmgrCleanup', 'RmgrNotFound'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'wal-rmgr-description',
        "title": 'WAL record 描述器',
        "page_file": 'b16-wal-rmgr-description.md',
        "boundary": 'record 类型识别与可读描述/反序列化辅助；rmgrdesc 全局唯一归属，不含 redo apply',
        "refs": [
            {
                "path": 'src/backend/access/rmgrdesc/xactdesc.c',
                "symbols": ['ParseCommitRecord', 'ParseAbortRecord', 'ParsePrepareRecord'],
            },
            {
                "path": 'src/backend/access/rmgrdesc/heapdesc.c',
                "symbols": ['heap_desc', 'heap2_desc', 'heap_xlog_deserialize_prune_and_freeze'],
            },
            {
                "path": 'src/backend/access/rmgrdesc/xlogdesc.c',
                "symbols": ['xlog_desc', 'xlog2_desc', 'get_checksum_state_string'],
            },
            {
                "path": 'src/backend/access/rmgrdesc/nbtdesc.c',
                "symbols": ['btree_desc', 'btree_identify'],
            },
            {
                "path": 'src/backend/access/rmgrdesc/gindesc.c',
                "symbols": ['gin_desc', 'gin_identify'],
            },
            {
                "path": 'src/backend/access/rmgrdesc/hashdesc.c',
                "symbols": ['hash_desc', 'hash_identify'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'wal-archive-backup-interfaces',
        "title": '备份与归档接口',
        "page_file": 'b17-wal-archive-backup-interfaces.md',
        "boundary": '面向备份/运维的 SQL 接口；不等于底层 WAL writer/recovery',
        "refs": [
            {
                "path": 'src/backend/access/transam/xlogfuncs.c',
                "symbols": ['pg_backup_start', 'pg_backup_stop', 'pg_walfile_name'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'synchronous-wal-wait',
        "title": 'WAL/LSN 等待与唤醒',
        "page_file": 'b18-synchronous-wal-wait.md',
        "boundary": '等待指定 LSN 并唤醒等待者；同步复制策略归 I',
        "refs": [
            {
                "path": 'src/backend/access/transam/xlogwait.c',
                "symbols": ['WaitForLSN', 'WaitLSNWakeup', 'WaitLSNCleanup'],
            },
        ],
    },
    {
        "feature_group": 'B-事务与WAL',
        "feature": 'generic-xlog-api',
        "title": 'Generic WAL API',
        "page_file": 'b19-generic-xlog-api.md',
        "boundary": '扩展可用的通用 WAL 封装；仅在确有读者需求时保留',
        "refs": [
            {
                "path": 'src/backend/access/transam/generic_xlog.c',
                "symbols": ['GenericXLogStart', 'GenericXLogRegisterBuffer', 'GenericXLogFinish'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'table-access-method-heap',
        "title": 'Heap TableAM 访问',
        "page_file": 'c01-table-access-method-heap.md',
        "boundary": 'TableAM 契约与 heap 扫描/访问/插入；不含索引 AM',
        "refs": [
            {
                "path": 'src/backend/access/table/tableam.c',
                "symbols": ['table_beginscan_catalog', 'table_slot_callbacks'],
            },
            {
                "path": 'src/backend/access/heap/heapam_handler.c',
                "symbols": ['GetHeapamTableAmRoutine'],
            },
            {
                "path": 'src/backend/access/heap/heapam.c',
                "symbols": ['heap_beginscan', 'heap_getnext', 'heap_insert'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'lazy-vacuum-and-pruning',
        "title": 'Lazy VACUUM 与页内剪枝',
        "page_file": 'c02-lazy-vacuum-and-pruning.md',
        "boundary": 'VACUUM 扫描/冻结协调与 dead tuple pruning；不含 VM 位语义',
        "refs": [
            {
                "path": 'src/backend/access/heap/vacuumlazy.c',
                "symbols": ['heap_vacuum_rel', 'lazy_scan_heap'],
            },
            {
                "path": 'src/backend/access/heap/pruneheap.c',
                "symbols": ['heap_page_prune_opt', 'heap_page_prune_and_freeze'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'visibility-map',
        "title": 'Visibility Map',
        "page_file": 'c03-visibility-map.md',
        "boundary": 'all-visible/all-frozen 位维护与 index-only scan 信号',
        "refs": [
            {
                "path": 'src/backend/access/heap/visibilitymap.c',
                "symbols": ['visibilitymap_set', 'visibilitymap_clear', 'visibilitymap_pin'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'heap-mvcc-visibility',
        "title": 'Heap tuple MVCC 可见性',
        "page_file": 'c04-heap-mvcc-visibility.md',
        "boundary": 'tuple hint bits 与可见性/更新判定；不含事务状态存储',
        "refs": [
            {
                "path": 'src/backend/access/heap/heapam_visibility.c',
                "symbols": ['HeapTupleSatisfiesMVCC', 'HeapTupleSatisfiesUpdate', 'HeapTupleSetHintBits'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'heap-rewrite',
        "title": 'Heap 重写',
        "page_file": 'c05-heap-rewrite.md',
        "boundary": 'CLUSTER/VACUUM FULL 的 tuple 重写与新旧映射',
        "refs": [
            {
                "path": 'src/backend/access/heap/rewriteheap.c',
                "symbols": ['begin_heap_rewrite', 'rewrite_heap_tuple', 'end_heap_rewrite'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'tuple-descriptor-layout',
        "title": 'Tuple descriptor 与 tuple 编码',
        "page_file": 'c06-tuple-descriptor-layout.md',
        "boundary": 'TupleDesc 元数据 + heap tuple 形成/解构；不含页内 line pointer',
        "refs": [
            {
                "path": 'src/backend/access/common/heaptuple.c',
                "symbols": ['heap_compute_data_size', 'heap_fill_tuple', 'heap_deform_tuple'],
            },
            {
                "path": 'src/backend/access/common/tupdesc.c',
                "symbols": ['CreateTupleDescCopy', 'FreeTupleDesc', 'equalTupleDescs'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'relation-options',
        "title": 'Relation options',
        "page_file": 'c07-relation-options.md',
        "boundary": 'reloption 注册/验证/变换/解析；不混 tuple layout',
        "refs": [
            {
                "path": 'src/backend/access/common/reloptions.c',
                "symbols": ['add_reloption_kind', 'transformRelOptions', 'parseRelOptions'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'toast-storage-and-compression',
        "title": 'TOAST 外置与压缩',
        "page_file": 'c08-toast-storage-and-compression.md',
        "boundary": '大字段外置、切片读取、压缩、更新与回收',
        "refs": [
            {
                "path": 'src/backend/access/heap/heaptoast.c',
                "symbols": ['heap_toast_insert_or_update', 'heap_toast_delete'],
            },
            {
                "path": 'src/backend/access/common/toast_internals.c',
                "symbols": ['toast_save_datum', 'toast_compress_datum'],
            },
            {
                "path": 'src/backend/access/common/detoast.c',
                "symbols": ['detoast_attr', 'detoast_attr_slice'],
            },
            {
                "path": 'src/backend/access/table/toast_helper.c',
                "symbols": ['toast_tuple_try_compression'],
            },
            {
                "path": 'src/backend/access/common/toast_compression.c',
                "symbols": ['pglz_compress_datum'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'tidstore',
        "title": 'TID 集合存储',
        "page_file": 'c09-tidstore.md',
        "boundary": 'VACUUM/bitmap 消费方的本地与共享 TID 集合',
        "refs": [
            {
                "path": 'src/backend/access/common/tidstore.c',
                "symbols": ['TidStoreCreateLocal', 'TidStoreCreateShared'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'synchronized-sequential-scan',
        "title": '同步顺序扫描',
        "page_file": 'c10-synchronized-sequential-scan.md',
        "boundary": '多 backend 顺序扫描位置协调',
        "refs": [
            {
                "path": 'src/backend/access/common/syncscan.c',
                "symbols": ['ss_get_location', 'ss_report_location'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'tuple-attribute-mapping-conversion',
        "title": 'Tuple 属性映射与转换',
        "page_file": 'c11-tuple-attribute-mapping-conversion.md',
        "boundary": '列映射与 TupleDesc 间按位置/名称转换',
        "refs": [
            {
                "path": 'src/backend/access/common/attmap.c',
                "symbols": ['make_attrmap', 'build_attrmap_by_position'],
            },
            {
                "path": 'src/backend/access/common/tupconvert.c',
                "symbols": ['convert_tuples_by_position', 'convert_tuples_by_name'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'heap-wal-redo',
        "title": 'Heap WAL redo',
        "page_file": 'c12-heap-wal-redo.md',
        "boundary": 'heap/heap2 record 重放与一致性屏蔽；正文链接 B 的 WAL 恢复族',
        "refs": [
            {
                "path": 'src/backend/access/heap/heapam_xlog.c',
                "symbols": ['heap_redo', 'heap2_redo', 'heap_mask'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'large-object-storage',
        "title": '大对象存储',
        "page_file": 'c13-large-object-storage.md',
        "boundary": '大对象创建/打开/读写的事务接口；区别于 TOAST',
        "refs": [
            {
                "path": 'src/backend/storage/large_object/inv_api.c',
                "symbols": ['inv_create', 'inv_open', 'inv_write'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'heap-page-layout',
        "title": 'Heap 页与 line pointer',
        "page_file": 'c14-heap-page-layout.md',
        "boundary": 'PageHeader、ItemId、空闲空间与页内 tuple 定位/插入',
        "refs": [
            {
                "path": 'src/backend/storage/page/bufpage.c',
                "symbols": ['PageInit', 'PageAddItemExtended'],
            },
            {
                "path": 'src/backend/access/heap/hio.c',
                "symbols": ['RelationPutHeapTuple', 'RelationGetBufferForTuple'],
            },
        ],
    },
    {
        "feature_group": 'C-堆表与元组',
        "feature": 'heap-index-fetch',
        "title": 'Heap 回表取行',
        "page_file": 'c15-heap-index-fetch.md',
        "boundary": '按索引 TID 取 heap tuple；D 负责产生 TID',
        "refs": [
            {
                "path": 'src/backend/access/heap/heapam_indexscan.c',
                "symbols": ['heapam_index_scan_begin'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'index-am-framework',
        "title": '通用 IndexAM 扫描框架',
        "page_file": 'd01-index-am-framework.md',
        "boundary": '索引扫描生命周期与 AM 接口/descriptor；不含单一 AM 算法',
        "refs": [
            {
                "path": 'src/backend/access/index/indexam.c',
                "symbols": ['index_open', 'index_beginscan'],
            },
            {
                "path": 'src/backend/access/index/genam.c',
                "symbols": ['RelationGetIndexScan', 'BuildIndexValueDescription'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'index-tuple-representation',
        "title": 'IndexTuple 编码',
        "page_file": 'd02-index-tuple-representation.md',
        "boundary": 'index tuple 构造、属性解码与变长布局',
        "refs": [
            {
                "path": 'src/backend/access/common/indextuple.c',
                "symbols": ['index_form_tuple', 'index_deform_tuple', 'nocache_index_getattr'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'btree-scan-search',
        "title": 'B-tree 扫描与搜索',
        "page_file": 'd03-btree-scan-search.md',
        "boundary": '根到叶搜索、比较、页读取与数组键扫描；不含插入分裂',
        "refs": [
            {
                "path": 'src/backend/access/nbtree/nbtree.c',
                "symbols": ['btgettuple', 'btbeginscan'],
            },
            {
                "path": 'src/backend/access/nbtree/nbtsearch.c',
                "symbols": ['_bt_search', '_bt_compare'],
            },
            {
                "path": 'src/backend/access/nbtree/nbtreadpage.c',
                "symbols": ['_bt_readpage', '_bt_start_array_keys'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'btree-insertion-split',
        "title": 'B-tree 插入与分裂',
        "page_file": 'd04-btree-insertion-split.md',
        "boundary": '插入路径与并发 split 完成；不含键预处理',
        "refs": [
            {
                "path": 'src/backend/access/nbtree/nbtree.c',
                "symbols": ['btinsert'],
            },
            {
                "path": 'src/backend/access/nbtree/nbtinsert.c',
                "symbols": ['_bt_doinsert', '_bt_finish_split'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'btree-scan-key-preprocessing',
        "title": 'B-tree 扫描键预处理',
        "page_file": 'd05-btree-scan-key-preprocessing.md',
        "boundary": 'quals/scankeys 规整为搜索约束；属扫描入口准备',
        "refs": [
            {
                "path": 'src/backend/access/nbtree/nbtpreprocesskeys.c',
                "symbols": ['_bt_preprocess_keys'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'btree-page-maintenance',
        "title": 'B-tree 页维护与去重',
        "page_file": 'd06-btree-page-maintenance.md',
        "boundary": 'metapage、split 定位、删除标记、dedup/bottom-up deletion',
        "refs": [
            {
                "path": 'src/backend/access/nbtree/nbtpage.c',
                "symbols": ['_bt_initmetapage', '_bt_vacuum_needs_cleanup'],
            },
            {
                "path": 'src/backend/access/nbtree/nbtsplitloc.c',
                "symbols": ['_bt_findsplitloc'],
            },
            {
                "path": 'src/backend/access/nbtree/nbtutils.c',
                "symbols": ['_bt_mkscankey', '_bt_killitems'],
            },
            {
                "path": 'src/backend/access/nbtree/nbtdedup.c',
                "symbols": ['_bt_dedup_pass', '_bt_bottomupdel_pass'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'btree-build',
        "title": 'B-tree 构建',
        "page_file": 'd07-btree-build.md',
        "boundary": '排序构建与并行构建',
        "refs": [
            {
                "path": 'src/backend/access/nbtree/nbtsort.c',
                "symbols": ['btbuild', '_bt_parallel_build_main'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'gin-write-and-pending-list',
        "title": 'GIN 写入与 pending list',
        "page_file": 'd08-gin-write-and-pending-list.md',
        "boundary": 'fastupdate/pending list 写入与清理',
        "refs": [
            {
                "path": 'src/backend/access/gin/gininsert.c',
                "symbols": ['ginEntryInsert', 'ginbuild'],
            },
            {
                "path": 'src/backend/access/gin/ginfast.c',
                "symbols": ['ginHeapTupleFastInsert', 'ginInsertCleanup'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'gin-search',
        "title": 'GIN 搜索',
        "page_file": 'd09-gin-search.md',
        "boundary": 'GIN 搜索与 bitmap TID 输出',
        "refs": [
            {
                "path": 'src/backend/access/gin/ginget.c',
                "symbols": ['gingetbitmap'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'gin-data-pages',
        "title": 'GIN 数据页',
        "page_file": 'd10-gin-data-pages.md',
        "boundary": 'data leaf/posting page 读写与插入',
        "refs": [
            {
                "path": 'src/backend/access/gin/gindatapage.c',
                "symbols": ['GinDataLeafPageGetItems', 'GinDataPageAddPostingItem'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'gist-search-insert',
        "title": 'GiST 搜索与插入',
        "page_file": 'd11-gist-search-insert.md',
        "boundary": '搜索/插入与 support procedure 契约',
        "refs": [
            {
                "path": 'src/backend/access/gist/gist.c',
                "symbols": ['gistinsert'],
            },
            {
                "path": 'src/backend/access/gist/gistproc.c',
                "symbols": ['rt_box_union', 'box_penalty'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'gist-build-and-page-split',
        "title": 'GiST 构建与页拆分',
        "page_file": 'd12-gist-build-and-page-split.md',
        "boundary": 'bulk build、缓冲页填充与 page fit/split',
        "refs": [
            {
                "path": 'src/backend/access/gist/gistbuild.c',
                "symbols": ['gistbuild'],
            },
            {
                "path": 'src/backend/access/gist/gistutil.c',
                "symbols": ['gistfillbuffer', 'gistfitpage'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'hash-index-storage-and-operations',
        "title": 'Hash 索引操作与溢出页',
        "page_file": 'd13-hash-index-storage-and-operations.md',
        "boundary": 'bucket 查找/插入、页缓冲、overflow 分配与释放',
        "refs": [
            {
                "path": 'src/backend/access/hash/hash.c',
                "symbols": ['hashbuild', 'hashinsert', 'hashgettuple'],
            },
            {
                "path": 'src/backend/access/hash/hashpage.c',
                "symbols": ['_hash_getbuf', '_hash_getinitbuf'],
            },
            {
                "path": 'src/backend/access/hash/hashovfl.c',
                "symbols": ['_hash_addovflpage', '_hash_freeovflpage'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'brin-core',
        "title": 'BRIN 扫描与维护',
        "page_file": 'd14-brin-core.md',
        "boundary": 'BRIN 插入、扫描与摘要维护主流程',
        "refs": [
            {
                "path": 'src/backend/access/brin/brin.c',
                "symbols": ['brininsert', 'brinbeginscan'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'brin-opclasses-summarization',
        "title": 'BRIN 摘要算子类',
        "page_file": 'd15-brin-opclasses-summarization.md',
        "boundary": 'minmax-multi 与 bloom 摘要结构/维护',
        "refs": [
            {
                "path": 'src/backend/access/brin/brin_minmax_multi.c',
                "symbols": ['AssertCheckRanges', 'AssertCheckExpandedRanges'],
            },
            {
                "path": 'src/backend/access/brin/brin_bloom.c',
                "symbols": ['bloom_init', 'bloom_add_value'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'spgist-access-maintenance',
        "title": 'SP-GiST 访问与维护',
        "page_file": 'd16-spgist-access-maintenance.md',
        "boundary": '插入/节点链接、扫描与 vacuum 清理',
        "refs": [
            {
                "path": 'src/backend/access/spgist/spgdoinsert.c',
                "symbols": ['spgdoinsert', 'spgUpdateNodeLink'],
            },
            {
                "path": 'src/backend/access/spgist/spgutils.c',
                "symbols": ['spgGetCache', 'initSpGistState'],
            },
            {
                "path": 'src/backend/access/spgist/spgscan.c',
                "symbols": ['spgbeginscan', 'spgrescan'],
            },
            {
                "path": 'src/backend/access/spgist/spgvacuum.c',
                "symbols": ['spgbulkdelete', 'spgvacuumcleanup'],
            },
        ],
    },
    {
        "feature_group": 'D-索引族',
        "feature": 'index-wal-redo',
        "title": '索引 WAL redo（合并篇）',
        "page_file": 'd17-index-wal-redo.md',
        "boundary": '索引 redo apply 通用骨架（按 rmgr 分派、页面加锁、修改页、置脏）；B-tree/Hash/SP-GiST 差异作分节',
        "refs": [
            {
                "path": 'src/backend/access/nbtree/nbtxlog.c',
                "symbols": ['btree_redo', 'btree_xlog_startup', 'btree_xlog_cleanup'],
            },
            {
                "path": 'src/backend/access/hash/hash_xlog.c',
                "symbols": ['hash_redo', 'hash_mask'],
            },
            {
                "path": 'src/backend/access/spgist/spgxlog.c',
                "symbols": ['spg_redo', 'spg_xlog_startup', 'spg_xlog_cleanup'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'planner-orchestration',
        "title": '查询规划编排',
        "page_file": 'e01-planner-orchestration.md',
        "boundary": 'PlannerInfo 初始化、子查询递归到顶层计划输出；不讲算法细节',
        "refs": [
            {
                "path": 'src/backend/optimizer/plan/planner.c',
                "symbols": ['standard_planner', 'subquery_planner', 'planner'],
            },
            {
                "path": 'src/backend/optimizer/plan/planmain.c',
                "symbols": ['query_planner'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'query-preprocessing',
        "title": '查询树预处理',
        "page_file": 'e02-query-preprocessing.md',
        "boundary": 'jointree/qual、聚合/目标列表、集合操作、相关子查询变换',
        "refs": [
            {
                "path": 'src/backend/optimizer/prep/prepjointree.c',
                "symbols": ['preprocess_relation_rtes', 'preprocess_function_rtes'],
            },
            {
                "path": 'src/backend/optimizer/prep/preptlist.c',
                "symbols": ['preprocess_targetlist'],
            },
            {
                "path": 'src/backend/optimizer/prep/prepagg.c',
                "symbols": ['preprocess_aggrefs'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'subquery-transform',
        "title": '子查询与子链接转换',
        "page_file": 'e03-subquery-transform.md',
        "boundary": '子链接转连接、子查询拉平、initplan/subplan 选择',
        "refs": [
            {
                "path": 'src/backend/optimizer/prep/prepjointree.c',
                "symbols": ['pull_up_sublinks', 'pull_up_subqueries'],
            },
            {
                "path": 'src/backend/optimizer/plan/subselect.c',
                "symbols": ['convert_ANY_sublink_to_join', 'make_subplan'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'rel-metadata',
        "title": '关系建模与继承展开',
        "page_file": 'e04-rel-metadata.md',
        "boundary": 'relation/RTE 规划期建模、继承子关系扩展、统计信息装载',
        "refs": [
            {
                "path": 'src/backend/optimizer/util/plancat.c',
                "symbols": ['get_relation_info', 'get_relation_statistics'],
            },
            {
                "path": 'src/backend/optimizer/util/inherit.c',
                "symbols": ['expand_inherited_rtentry'],
            },
            {
                "path": 'src/backend/optimizer/util/relnode.c',
                "symbols": ['build_simple_rel'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'path-generation',
        "title": '访问路径生成',
        "page_file": 'e05-path-generation.md',
        "boundary": 'base/append/index/TID/bitmap 候选路径生成；不含最终比较',
        "refs": [
            {
                "path": 'src/backend/optimizer/path/allpaths.c',
                "symbols": ['set_rel_pathlist', 'set_base_rel_pathlists'],
            },
            {
                "path": 'src/backend/optimizer/path/indxpath.c',
                "symbols": ['create_index_paths'],
            },
            {
                "path": 'src/backend/optimizer/path/tidpath.c',
                "symbols": ['create_tidscan_paths'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'join-search',
        "title": '连接枚举与算法候选',
        "page_file": 'e06-join-search.md',
        "boundary": 'joinrel 分层、合法连接顺序、nestloop/hash/merge 候选',
        "refs": [
            {
                "path": 'src/backend/optimizer/path/joinrels.c',
                "symbols": ['join_search_one_level', 'make_join_rel'],
            },
            {
                "path": 'src/backend/optimizer/path/joinpath.c',
                "symbols": ['add_paths_to_joinrel'],
            },
            {
                "path": 'src/backend/optimizer/path/allpaths.c',
                "symbols": ['standard_join_search'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'geqo',
        "title": '遗传查询优化',
        "page_file": 'e07-geqo.md',
        "boundary": '染色体编码、种群演化、适应度评估到最终连接树',
        "refs": [
            {
                "path": 'src/backend/optimizer/geqo/geqo_main.c',
                "symbols": ['geqo'],
            },
            {
                "path": 'src/backend/optimizer/geqo/geqo_eval.c',
                "symbols": ['geqo_eval'],
            },
            {
                "path": 'src/backend/optimizer/geqo/geqo_pool.c',
                "symbols": ['random_init_pool', 'alloc_pool'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'path-ranking',
        "title": '路径支配与最优选择',
        "page_file": 'e08-path-ranking.md',
        "boundary": '成本/参数化/排序属性比较、非支配候选保留、cheapest 选择',
        "refs": [
            {
                "path": 'src/backend/optimizer/util/pathnode.c',
                "symbols": ['add_path', 'set_cheapest', 'compare_path_costs'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'cost-cardinality',
        "title": '成本与基数估算',
        "page_file": 'e09-cost-cardinality.md',
        "boundary": 'CPU/I/O/并行/排序/连接成本与谓词选择率；`selfuncs` 属 ADT 域协作点',
        "refs": [
            {
                "path": 'src/backend/optimizer/path/costsize.c',
                "symbols": ['cost_seqscan', 'set_baserel_size_estimates', 'calc_joinrel_size_estimate'],
            },
            {
                "path": 'src/backend/optimizer/path/clausesel.c',
                "symbols": ['clauselist_selectivity', 'clause_selectivity'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'equivalence-and-order',
        "title": '等价类与排序传播',
        "page_file": 'e10-equivalence-and-order.md',
        "boundary": '等式推导、等价类规范化、pathkeys 传递与冗余排序消除',
        "refs": [
            {
                "path": 'src/backend/optimizer/path/equivclass.c',
                "symbols": ['process_equivalence', 'generate_base_implied_equalities'],
            },
            {
                "path": 'src/backend/optimizer/path/pathkeys.c',
                "symbols": ['make_canonical_pathkey', 'truncate_useless_pathkeys'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'predicate-reasoning',
        "title": '谓词推理与连接简化',
        "page_file": 'e11-predicate-reasoning.md',
        "boundary": '逻辑蕴含/反证、部分索引适用性、外连接/半连接简化',
        "refs": [
            {
                "path": 'src/backend/optimizer/util/predtest.c',
                "symbols": ['predicate_implied_by', 'predicate_refuted_by'],
            },
            {
                "path": 'src/backend/optimizer/plan/analyzejoins.c',
                "symbols": ['remove_useless_outer_joins'],
            },
            {
                "path": 'src/backend/optimizer/prep/prepqual.c',
                "symbols": ['canonicalize_qual'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'plan-construction',
        "title": 'Plan 树构造',
        "page_file": 'e12-plan-construction.md',
        "boundary": 'path→plan 转换、节点特化、目标列表与 sort/limit 塑形',
        "refs": [
            {
                "path": 'src/backend/optimizer/plan/createplan.c',
                "symbols": ['create_plan', 'create_scan_plan', 'create_join_plan'],
            },
        ],
    },
    {
        "feature_group": 'E-优化器',
        "feature": 'plan-reference-fixing',
        "title": 'Plan 引用修整',
        "page_file": 'e13-plan-reference-fixing.md',
        "boundary": 'Var/参数引用、子计划目标列表与执行节点引用定型',
        "refs": [
            {
                "path": 'src/backend/optimizer/plan/setrefs.c',
                "symbols": ['set_plan_references', 'fix_scan_expr'],
            },
            {
                "path": 'src/backend/optimizer/plan/subselect.c',
                "symbols": ['SS_finalize_plan'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'executor-lifecycle',
        "title": '执行器生命周期',
        "page_file": 'f01-executor-lifecycle.md',
        "boundary": 'ExecutorStart/Run/Finish/End、QueryDesc、快照与权限检查',
        "refs": [
            {
                "path": 'src/backend/executor/execMain.c',
                "symbols": ['ExecutorStart', 'ExecutorRun', 'ExecutorFinish', 'ExecutorEnd'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'executor-state',
        "title": '执行状态与资源生命周期',
        "page_file": 'f02-executor-state.md',
        "boundary": 'EState、ExprContext、tuple table 的分配/重置/释放',
        "refs": [
            {
                "path": 'src/backend/executor/execUtils.c',
                "symbols": ['CreateExecutorState', 'FreeExecutorState', 'CreateExprContext', 'FreeExprContext'],
            },
            {
                "path": 'src/backend/executor/execTuples.c',
                "symbols": ['ExecResetTupleTable'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'node-dispatch',
        "title": '节点初始化与驱动',
        "page_file": 'f03-node-dispatch.md',
        "boundary": 'ExecInitNode/ExecProcNode/MultiExec 分派与拉取模型',
        "refs": [
            {
                "path": 'src/backend/executor/execProcnode.c',
                "symbols": ['ExecInitNode', 'ExecProcNodeFirst', 'MultiExecProcNode'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'expression-evaluation',
        "title": '表达式求值',
        "page_file": 'f04-expression-evaluation.md',
        "boundary": 'ExprState/ExprEvalStep 构建与解释执行；不含代码生成独立页',
        "refs": [
            {
                "path": 'src/backend/executor/execExpr.c',
                "symbols": ['ExecInitExpr', 'ExecInitExprRec'],
            },
            {
                "path": 'src/backend/executor/execExprInterp.c',
                "symbols": ['ExecInterpExpr', 'ExecReadyInterpretedExpr'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'tuple-slots',
        "title": 'TupleTableSlot 表示',
        "page_file": 'f05-tuple-slots.md',
        "boundary": 'slot 虚接口、内存/heap/buffer/虚拟变体与延迟物化',
        "refs": [
            {
                "path": 'src/backend/executor/execTuples.c',
                "symbols": ['MakeTupleTableSlot', 'ExecStoreVirtualTuple', 'tts_buffer_heap_store_tuple', 'tts_virtual_init'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'scan-nodes',
        "title": '表与定位扫描节点',
        "page_file": 'f06-scan-nodes.md',
        "boundary": 'Seq/TID/TID-range/Sample 扫描按访问模型讲解',
        "refs": [
            {
                "path": 'src/backend/executor/nodeSeqscan.c',
                "symbols": ['ExecInitSeqScan', 'SeqNext'],
            },
            {
                "path": 'src/backend/executor/nodeTidscan.c',
                "symbols": ['ExecInitTidScan', 'TidNext'],
            },
            {
                "path": 'src/backend/executor/nodeSamplescan.c',
                "symbols": ['ExecInitSampleScan', 'tablesample_getnext'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'index-and-bitmap-scans',
        "title": '索引与位图扫描执行',
        "page_file": 'f07-index-and-bitmap-scans.md',
        "boundary": 'index/index-only runtime keys、amgettuple、可见性；bitmap 协作与 lossiness',
        "refs": [
            {
                "path": 'src/backend/executor/nodeIndexscan.c',
                "symbols": ['ExecIndexScan', 'ExecIndexBuildScanKeys', 'ExecIndexEvalRuntimeKeys'],
            },
            {
                "path": 'src/backend/executor/nodeIndexonlyscan.c',
                "symbols": ['IndexOnlyNext'],
            },
            {
                "path": 'src/backend/executor/nodeBitmapHeapscan.c',
                "symbols": ['BitmapHeapNext', 'BitmapHeapRecheck'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'join-algorithms',
        "title": '连接执行算法',
        "page_file": 'f08-join-algorithms.md',
        "boundary": 'nestloop 参数重扫、merge 组/标记、hash 批次/溢写与探测',
        "refs": [
            {
                "path": 'src/backend/executor/nodeNestloop.c',
                "symbols": ['ExecNestLoop', 'ExecReScanNestLoop'],
            },
            {
                "path": 'src/backend/executor/nodeMergejoin.c',
                "symbols": ['ExecMergeJoin', 'MJCompare'],
            },
            {
                "path": 'src/backend/executor/nodeHashjoin.c',
                "symbols": ['ExecHashJoin', 'ExecHashJoinNewBatch'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'aggregation-and-window',
        "title": '聚合与窗口',
        "page_file": 'f09-aggregation-and-window.md',
        "boundary": '分组哈希/排序、Agg 状态机、窗口帧推进',
        "refs": [
            {
                "path": 'src/backend/executor/nodeAgg.c',
                "symbols": ['ExecAgg', 'build_hash_table', 'hash_agg_enter_spill_mode'],
            },
            {
                "path": 'src/backend/executor/nodeWindowAgg.c',
                "symbols": ['ExecWindowAgg', 'update_frameheadpos'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'sort-and-materialization',
        "title": '排序与物化节点',
        "page_file": 'f10-sort-and-materialization.md',
        "boundary": 'Sort/Material/Memoize/IncrementalSort 的执行期状态',
        "refs": [
            {
                "path": 'src/backend/executor/nodeSort.c',
                "symbols": ['ExecSort', 'ExecInitSort'],
            },
            {
                "path": 'src/backend/executor/nodeIncrementalSort.c',
                "symbols": ['ExecIncrementalSort'],
            },
            {
                "path": 'src/backend/executor/nodeMaterial.c',
                "symbols": ['ExecMaterial', 'ExecMaterialMarkPos'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'append-and-subplans',
        "title": 'Append 族与子计划',
        "page_file": 'f11-append-and-subplans.md',
        "boundary": 'Append/MergeAppend、SubPlan、CTE 与 initplan 执行',
        "refs": [
            {
                "path": 'src/backend/executor/nodeAppend.c',
                "symbols": ['ExecAppend', 'choose_next_subplan_for_leader'],
            },
            {
                "path": 'src/backend/executor/nodeMergeAppend.c',
                "symbols": ['ExecMergeAppend'],
            },
            {
                "path": 'src/backend/executor/nodeSubplan.c',
                "symbols": ['ExecSubPlan', 'ExecSetParamPlan'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'parallel-execution',
        "title": '并行执行',
        "page_file": 'f12-parallel-execution.md',
        "boundary": 'Gather/GatherMerge 与 worker 协议；DSM 基础见 A 页',
        "refs": [
            {
                "path": 'src/backend/executor/nodeGather.c',
                "symbols": ['ExecInitGather', 'gather_getnext'],
            },
            {
                "path": 'src/backend/executor/nodeGatherMerge.c',
                "symbols": ['ExecGatherMerge'],
            },
            {
                "path": 'src/backend/executor/execParallel.c',
                "symbols": ['ExecInitParallelPlan', 'ParallelQueryMain'],
            },
        ],
    },
    {
        "feature_group": 'F-执行器',
        "feature": 'modify-and-partition-routing',
        "title": '修改节点与分区路由',
        "page_file": 'f13-modify-and-partition-routing.md',
        "boundary": 'ModifyTable、分区路由与元组分发',
        "refs": [
            {
                "path": 'src/backend/executor/nodeModifyTable.c',
                "symbols": ['ExecModifyTable', 'ExecInsert', 'ExecUpdate'],
            },
            {
                "path": 'src/backend/executor/execPartition.c',
                "symbols": ['ExecSetupPartitionTupleRouting', 'ExecFindPartition'],
            },
        ],
    },
    {
        "feature_group": 'G-目录与缓存',
        "feature": 'catalog-storage-bootstrap',
        "title": '系统目录持久化与引导',
        "page_file": 'g01-catalog-storage-bootstrap.md',
        "boundary": '目录结构、初始化数据与更新；排除缓存策略',
        "refs": [
            {
                "path": 'src/backend/catalog/catalog.c',
                "symbols": ['IsCatalogRelation', 'GetNewOidWithIndex'],
            },
            {
                "path": 'src/backend/bootstrap/bootstrap.c',
                "symbols": ['BootstrapModeMain', 'boot_openrel'],
            },
            {
                "path": 'src/backend/catalog/indexing.c',
                "symbols": ['CatalogTupleInsert'],
            },
        ],
    },
    {
        "feature_group": 'G-目录与缓存',
        "feature": 'syscache',
        "title": '系统缓存',
        "page_file": 'g02-syscache.md',
        "boundary": '按目录索引键的缓存命中与失效',
        "refs": [
            {
                "path": 'src/backend/utils/cache/syscache.c',
                "symbols": ['SearchSysCache', 'InitCatalogCache', 'ReleaseSysCache'],
            },
            {
                "path": 'src/backend/utils/cache/catcache.c',
                "symbols": ['SearchCatCache', 'InitCatCache'],
            },
        ],
    },
    {
        "feature_group": 'G-目录与缓存',
        "feature": 'relcache',
        "title": '关系缓存生命周期',
        "page_file": 'g03-relcache.md',
        "boundary": 'Relation/rd_att/索引元数据缓存、重建与失效',
        "refs": [
            {
                "path": 'src/backend/utils/cache/relcache.c',
                "symbols": ['RelationBuildDesc', 'RelationIdGetRelation', 'RelationClearRelation'],
            },
        ],
    },
    {
        "feature_group": 'G-目录与缓存',
        "feature": 'cache-invalidation-semantics',
        "title": '缓存失效语义',
        "page_file": 'g04-cache-invalidation-semantics.md',
        "boundary": 'shared inval 消息的对象级语义与本地执行；传输原语引用 A 页',
        "refs": [
            {
                "path": 'src/backend/utils/cache/inval.c',
                "symbols": ['LocalExecuteInvalidationMessage', 'InvalidateSystemCaches', 'CacheInvalidateHeapTuple'],
            },
        ],
    },
    {
        "feature_group": 'G-目录与缓存',
        "feature": 'object-dependencies',
        "title": '对象依赖与删除',
        "page_file": 'g05-object-dependencies.md',
        "boundary": 'pg_depend 记录与递归删除',
        "refs": [
            {
                "path": 'src/backend/catalog/dependency.c',
                "symbols": ['performDeletion', 'findDependentObjects', 'deleteOneObject'],
            },
            {
                "path": 'src/backend/catalog/pg_depend.c',
                "symbols": ['recordDependencyOn'],
            },
        ],
    },
    {
        "feature_group": 'G-目录与缓存',
        "feature": 'namespace-resolution',
        "title": '命名空间解析',
        "page_file": 'g06-namespace-resolution.md',
        "boundary": '名字到 OID 的可见性与 search_path 解析',
        "refs": [
            {
                "path": 'src/backend/catalog/namespace.c',
                "symbols": ['RangeVarGetRelidExtended', 'LookupCreationNamespace', 'recomputeNamespacePath'],
            },
        ],
    },
    {
        "feature_group": 'G-目录与缓存',
        "feature": 'acl-and-ownership',
        "title": '权限与属主',
        "page_file": 'g07-acl-and-ownership.md',
        "boundary": 'ACL 检查与属主变更主流程',
        "refs": [
            {
                "path": 'src/backend/catalog/aclchk.c',
                "symbols": ['ExecuteGrantStmt', 'ExecGrant_common'],
            },
            {
                "path": 'src/backend/catalog/objectaddress.c',
                "symbols": ['check_object_ownership'],
            },
        ],
    },
    {
        "feature_group": 'H-内存与排序',
        "feature": 'memory-contexts',
        "title": '内存上下文体系',
        "page_file": 'h01-memory-contexts.md',
        "boundary": 'palloc 家族、上下文层级、reset/delete；排除 libc 分配',
        "refs": [
            {
                "path": 'src/backend/utils/mmgr/mcxt.c',
                "symbols": ['MemoryContextInit', 'MemoryContextAlloc', 'MemoryContextReset', 'palloc'],
            },
            {
                "path": 'src/backend/utils/mmgr/aset.c',
                "symbols": ['AllocSetContextCreateInternal', 'AllocSetAlloc'],
            },
        ],
    },
    {
        "feature_group": 'H-内存与排序',
        "feature": 'resource-owners',
        "title": '资源所有者与错误期回收',
        "page_file": 'h02-resource-owners.md',
        "boundary": 'ResourceOwner 跟踪 buffer/tuple/lock 等并在出错时释放',
        "refs": [
            {
                "path": 'src/backend/utils/resowner/resowner.c',
                "symbols": ['ResourceOwnerCreate', 'ResourceOwnerRemember', 'ResourceOwnerRelease', 'ResourceOwnerDelete'],
            },
        ],
    },
    {
        "feature_group": 'H-内存与排序',
        "feature": 'tuplesort',
        "title": '元组外部排序',
        "page_file": 'h03-tuplesort.md',
        "boundary": '内存内排序、run 生成、归并与磁盘临时文件',
        "refs": [
            {
                "path": 'src/backend/utils/sort/tuplesort.c',
                "symbols": ['tuplesort_begin_common', 'tuplesort_performsort', 'tuplesort_gettuple_common'],
            },
            {
                "path": 'src/backend/utils/sort/tuplesortvariants.c',
                "symbols": ['tuplesort_begin_heap'],
            },
        ],
    },
    {
        "feature_group": 'H-内存与排序',
        "feature": 'logical-tapes',
        "title": '逻辑磁带',
        "page_file": 'h04-logical-tapes.md',
        "boundary": 'LogTape 逻辑段/读写与临时文件组织',
        "refs": [
            {
                "path": 'src/backend/utils/sort/logtape.c',
                "symbols": ['LogicalTapeSetCreate', 'LogicalTapeWrite', 'LogicalTapeRead'],
            },
        ],
    },
    {
        "feature_group": 'H-内存与排序',
        "feature": 'sort-primitives',
        "title": '排序基元',
        "page_file": 'h05-sort-primitives.md',
        "boundary": 'qsort 封装、堆操作与比较器契约',
        "refs": [
            {
                "path": 'src/backend/utils/sort/sortsupport.c',
                "symbols": ['PrepareSortSupportFromOrderingOp', 'FinishSortSupportFunction', 'comparison_shim'],
            },
            {
                "path": 'src/backend/utils/sort/tuplesort.c',
                "symbols": ['tuplesort_heap_insert', 'tuplesort_heap_delete_top'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'physical-streaming',
        "title": '物理流复制',
        "page_file": 'i01-physical-streaming.md',
        "boundary": 'walsender/walreceiver、反馈与复制协议；排除逻辑解码',
        "refs": [
            {
                "path": 'src/backend/replication/walsender.c',
                "symbols": ['StartReplication', 'XLogSendPhysical', 'WalSndLoop'],
            },
            {
                "path": 'src/backend/replication/walreceiver.c',
                "symbols": ['WalReceiverMain', 'XLogWalRcvProcessMsg'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'replication-slots',
        "title": '复制槽',
        "page_file": 'i02-replication-slots.md',
        "boundary": '槽创建/推进/持久化与保留策略',
        "refs": [
            {
                "path": 'src/backend/replication/slot.c',
                "symbols": ['ReplicationSlotCreate', 'ReplicationSlotSave', 'ReplicationSlotsComputeRequiredLSN'],
            },
            {
                "path": 'src/backend/replication/slotfuncs.c',
                "symbols": ['pg_replication_slot_advance'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'sync-replication',
        "title": '同步复制',
        "page_file": 'i03-sync-replication.md',
        "boundary": '同步备库确认与等待队列策略',
        "refs": [
            {
                "path": 'src/backend/replication/syncrep.c',
                "symbols": ['SyncRepWaitForLSN', 'SyncRepReleaseWaiters', 'SyncRepGetSyncRecPtr'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'logical-decoding',
        "title": '逻辑解码',
        "page_file": 'i04-logical-decoding.md',
        "boundary": 'WAL→事务变更、快照与输出插件回调',
        "refs": [
            {
                "path": 'src/backend/replication/logical/logical.c',
                "symbols": ['CreateDecodingContext', 'CreateInitDecodingContext', 'StartupDecodingContext'],
            },
            {
                "path": 'src/backend/replication/logical/decode.c',
                "symbols": ['LogicalDecodingProcessRecord'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'logical-replication-apply',
        "title": '逻辑复制应用',
        "page_file": 'i05-logical-replication-apply.md',
        "boundary": 'subscriber worker、apply 与冲突处理',
        "refs": [
            {
                "path": 'src/backend/replication/logical/worker.c',
                "symbols": ['ApplyWorkerMain', 'apply_dispatch', 'apply_handle_insert'],
            },
            {
                "path": 'src/backend/replication/logical/launcher.c',
                "symbols": ['ApplyLauncherMain', 'logicalrep_worker_launch'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'reorder-buffer',
        "title": 'ReorderBuffer',
        "page_file": 'i06-reorder-buffer.md',
        "boundary": '事务边界、变更累积与顺序输出',
        "refs": [
            {
                "path": 'src/backend/replication/logical/reorderbuffer.c',
                "symbols": ['ReorderBufferAllocate', 'ReorderBufferQueueChange', 'ReorderBufferCommit'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'snapshot-build',
        "title": '逻辑解码快照构建',
        "page_file": 'i07-snapshot-build.md',
        "boundary": 'running xacts 追踪与一致性起点',
        "refs": [
            {
                "path": 'src/backend/replication/logical/snapbuild.c',
                "symbols": ['AllocateSnapshotBuilder', 'SnapBuildProcessRunningXacts', 'SnapBuildBuildSnapshot'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'tablesync',
        "title": '表同步',
        "page_file": 'i08-tablesync.md',
        "boundary": '初始同步 worker 与状态推进',
        "refs": [
            {
                "path": 'src/backend/replication/logical/tablesync.c',
                "symbols": ['start_table_sync', 'copy_table', 'TableSyncWorkerMain'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'origin-tracking',
        "title": '复制来源追踪',
        "page_file": 'i09-origin-tracking.md',
        "boundary": 'origin 进度与循环检测',
        "refs": [
            {
                "path": 'src/backend/replication/logical/origin.c',
                "symbols": ['replorigin_create', 'replorigin_session_setup', 'replorigin_get_progress'],
            },
        ],
    },
    {
        "feature_group": 'I-复制与高可用',
        "feature": 'archive-and-base-backup',
        "title": '归档与基础备份',
        "page_file": 'i10-archive-and-base-backup.md',
        "boundary": 'archive 状态、BASE_BACKUP 协议与一致性',
        "refs": [
            {
                "path": 'src/backend/backup/basebackup.c',
                "symbols": ['SendBaseBackup', 'perform_base_backup'],
            },
            {
                "path": 'src/backend/access/transam/xlogfuncs.c',
                "symbols": ['pg_backup_start', 'pg_backup_stop'],
            },
            {
                "path": 'src/backend/postmaster/pgarch.c',
                "symbols": ['pgarch_archiveXlog', 'PgArchiverMain'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'postmaster-lifecycle',
        "title": 'postmaster 与子进程管理',
        "page_file": 'j01-postmaster-lifecycle.md',
        "boundary": '启动、监听、fork、重启/恢复调度',
        "refs": [
            {
                "path": 'src/backend/postmaster/postmaster.c',
                "symbols": ['PostmasterMain', 'ServerLoop', 'StartChildProcess', 'PostmasterStateMachine'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'backend-protocol-session',
        "title": '后端协议与会话循环',
        "page_file": 'j02-backend-protocol-session.md',
        "boundary": 'startup packet、simple/extended 协议循环',
        "refs": [
            {
                "path": 'src/backend/tcop/backend_startup.c',
                "symbols": ['BackendMain', 'ProcessStartupPacket'],
            },
            {
                "path": 'src/backend/tcop/postgres.c',
                "symbols": ['PostgresMain', 'exec_simple_query'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'utility-command-execution',
        "title": 'utility 命令执行',
        "page_file": 'j03-utility-command-execution.md',
        "boundary": 'ProcessUtility 分派与事务边界协调',
        "refs": [
            {
                "path": 'src/backend/tcop/utility.c',
                "symbols": ['ProcessUtility', 'standard_ProcessUtility'],
            },
            {
                "path": 'src/backend/tcop/pquery.c',
                "symbols": ['PortalRun', 'CreateQueryDesc'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'ddl-object-commands',
        "title": '对象 DDL 命令域',
        "page_file": 'j04-ddl-object-commands.md',
        "boundary": '表/类型/索引/函数/序列等 DDL 主流程',
        "refs": [
            {
                "path": 'src/backend/commands/tablecmds.c',
                "symbols": ['DefineRelation', 'ATController'],
            },
            {
                "path": 'src/backend/commands/indexcmds.c',
                "symbols": ['DefineIndex'],
            },
            {
                "path": 'src/backend/commands/typecmds.c',
                "symbols": ['DefineType'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'copy-commands',
        "title": 'COPY 命令',
        "page_file": 'j05-copy-commands.md',
        "boundary": 'COPY FROM/TO 解析、并行与错误处理',
        "refs": [
            {
                "path": 'src/backend/commands/copy.c',
                "symbols": ['DoCopy'],
            },
            {
                "path": 'src/backend/commands/copyfrom.c',
                "symbols": ['BeginCopyFrom', 'CopyFrom'],
            },
            {
                "path": 'src/backend/commands/copyto.c',
                "symbols": ['BeginCopyTo', 'DoCopyTo'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'background-worker-framework',
        "title": '后台 worker 框架',
        "page_file": 'j06-background-worker-framework.md',
        "boundary": 'bgworker 注册/启动与 launcher 管理',
        "refs": [
            {
                "path": 'src/backend/postmaster/bgworker.c',
                "symbols": ['RegisterBackgroundWorker', 'RegisterDynamicBackgroundWorker', 'BackgroundWorkerMain', 'BackgroundWorkerStateChange'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'autovacuum-launcher',
        "title": 'autovacuum 调度',
        "page_file": 'j07-autovacuum-launcher.md',
        "boundary": 'launcher/worker 派发与限流',
        "refs": [
            {
                "path": 'src/backend/postmaster/autovacuum.c',
                "symbols": ['AutoVacLauncherMain', 'do_start_worker', 'AutoVacWorkerMain', 'do_autovacuum'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'checkpointer-process',
        "title": 'checkpointer 进程',
        "page_file": 'j08-checkpointer-process.md',
        "boundary": '后台 checkpoint 触发与写延迟；算法见 B 页',
        "refs": [
            {
                "path": 'src/backend/postmaster/checkpointer.c',
                "symbols": ['CheckpointerMain', 'ExecCheckpoint', 'RequestCheckpoint'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'pgstat-activity',
        "title": '运行统计',
        "page_file": 'j09-pgstat-activity.md',
        "boundary": '统计收集、持久化与恢复',
        "refs": [
            {
                "path": 'src/backend/utils/activity/pgstat.c',
                "symbols": ['pgstat_report_stat', 'pgstat_build_snapshot', 'pgstat_read_statsfile'],
            },
            {
                "path": 'src/backend/utils/activity/backend_status.c',
                "symbols": ['pgstat_report_activity'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'error-reporting',
        "title": '错误上报',
        "page_file": 'j10-error-reporting.md',
        "boundary": 'ereport/elog 分级、递归保护与上报出口（跨域候选）',
        "refs": [
            {
                "path": 'src/backend/utils/error/elog.c',
                "symbols": ['errstart', 'errfinish', 'EmitErrorReport', 'in_error_recursion_trouble'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'async-notify-listen',
        "title": 'LISTEN/NOTIFY',
        "page_file": 'j11-async-notify-listen.md',
        "boundary": '异步通知队列与投递',
        "refs": [
            {
                "path": 'src/backend/commands/async.c',
                "symbols": ['Async_Notify', 'Async_Listen', 'ProcessIncomingNotify', 'PreCommit_Notify'],
            },
        ],
    },
    {
        "feature_group": 'J-服务端与命令面',
        "feature": 'session-and-guc',
        "title": 'GUC 与会话配置',
        "page_file": 'j12-session-and-guc.md',
        "boundary": 'GUC 解析/校验与作用域',
        "refs": [
            {
                "path": 'src/backend/utils/misc/guc.c',
                "symbols": ['SetConfigOption', 'InitializeGUCOptions', 'define_custom_variable'],
            },
            {
                "path": 'src/backend/utils/misc/guc_funcs.c',
                "symbols": ['ExecSetVariableStmt'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'parser-pipeline',
        "title": 'SQL 词法与语法解析',
        "page_file": 'k01-parser-pipeline.md',
        "boundary": 'scanner/grammar 到 raw parse tree；不含语义分析',
        "refs": [
            {
                "path": 'src/backend/parser/parser.c',
                "symbols": ['raw_parser'],
            },
            {
                "path": 'src/backend/parser/scansup.c',
                "symbols": ['downcase_truncate_identifier', 'scanner_isspace'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'parse-analysis',
        "title": 'SQL 语义分析',
        "page_file": 'k02-parse-analysis.md',
        "boundary": '表达式/子句/关系/类型/函数/聚合/目标列表变换',
        "refs": [
            {
                "path": 'src/backend/parser/analyze.c',
                "symbols": ['parse_analyze_fixedparams', 'transformStmt'],
            },
            {
                "path": 'src/backend/parser/parse_expr.c',
                "symbols": ['transformExpr'],
            },
            {
                "path": 'src/backend/parser/parse_clause.c',
                "symbols": ['transformFromClause', 'transformWhereClause'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'rewrite-and-views',
        "title": '重写与视图展开',
        "page_file": 'k03-rewrite-and-views.md',
        "boundary": 'rewrite rules、视图展开与 row security 挂接',
        "refs": [
            {
                "path": 'src/backend/rewrite/rewriteHandler.c',
                "symbols": ['QueryRewrite', 'rewriteTargetView', 'AcquireRewriteLocks'],
            },
            {
                "path": 'src/backend/rewrite/rewriteDefine.c',
                "symbols": ['DefineRule'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'node-tree-utilities',
        "title": '节点树工具',
        "page_file": 'k04-node-tree-utilities.md',
        "boundary": 'expression type/walker/mutator 基础设施',
        "refs": [
            {
                "path": 'src/backend/nodes/nodeFuncs.c',
                "symbols": ['exprType', 'exprTypmod', 'exprCollation'],
            },
            {
                "path": 'src/backend/nodes/equalfuncs.c',
                "symbols": ['equal'],
            },
            {
                "path": 'src/backend/nodes/copyfuncs.c',
                "symbols": ['copyObjectImpl'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'tablesample',
        "title": 'TABLESAMPLE 抽样',
        "page_file": 'k05-tablesample.md',
        "boundary": 'grammar 入口、SampleScan 与 system/bernoulli 方法（裁决 3）',
        "refs": [
            {
                "path": 'src/backend/access/tablesample/tablesample.c',
                "symbols": ['GetTsmRoutine'],
            },
            {
                "path": 'src/backend/access/tablesample/system.c',
                "symbols": ['tsm_system_handler', 'system_nextsampleblock'],
            },
            {
                "path": 'src/backend/access/tablesample/bernoulli.c',
                "symbols": ['tsm_bernoulli_handler', 'bernoulli_nextsampletuple'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'client-auth-hba',
        "title": '客户端认证与 HBA',
        "page_file": 'k06-client-auth-hba.md',
        "boundary": 'HBA 规则匹配与认证方法分派',
        "refs": [
            {
                "path": 'src/backend/libpq/hba.c',
                "symbols": ['load_hba', 'hba_getauthmethod', 'check_hba', 'parse_hba_line'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'auth-methods',
        "title": '认证方法',
        "page_file": 'k07-auth-methods.md',
        "boundary": 'SCRAM/MD5/GSS/LDAP/证书的凭据校验',
        "refs": [
            {
                "path": 'src/backend/libpq/auth.c',
                "symbols": ['ClientAuthentication', 'sendAuthRequest', 'CheckPasswordAuth'],
            },
            {
                "path": 'src/backend/libpq/auth-scram.c',
                "symbols": ['scram_exchange'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'tls-transport',
        "title": 'TLS 传输安全',
        "page_file": 'k08-tls-transport.md',
        "boundary": '服务端 TLS 初始化与握手',
        "refs": [
            {
                "path": 'src/backend/libpq/be-secure-openssl.c',
                "symbols": ['be_tls_init', 'be_tls_open_server', 'be_tls_read'],
            },
            {
                "path": 'src/backend/libpq/be-secure.c',
                "symbols": ['secure_open_server', 'secure_read'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'row-level-security',
        "title": '行级安全',
        "page_file": 'k09-row-level-security.md',
        "boundary": 'policy 表达式与查询期过滤',
        "refs": [
            {
                "path": 'src/backend/rewrite/rowsecurity.c',
                "symbols": ['get_row_security_policies', 'add_security_quals', 'add_with_check_options'],
            },
            {
                "path": 'src/backend/utils/misc/rls.c',
                "symbols": ['check_enable_rls'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'authorization-checks',
        "title": '权限检查',
        "page_file": 'k10-authorization-checks.md',
        "boundary": 'ACL 判定与错误上抛（与 G 的 ACL 主流程分界）',
        "refs": [
            {
                "path": 'src/backend/catalog/aclchk.c',
                "symbols": ['object_aclcheck_ext', 'aclcheck_error'],
            },
            {
                "path": 'src/backend/utils/adt/acl.c',
                "symbols": ['aclmask', 'has_privs_of_role'],
            },
        ],
    },
    {
        "feature_group": 'K-SQL处理与安全',
        "feature": 'event-triggers',
        "title": '事件触发器',
        "page_file": 'k11-event-triggers.md',
        "boundary": 'DDL 事件捕获与触发器执行',
        "refs": [
            {
                "path": 'src/backend/commands/event_trigger.c',
                "symbols": ['CreateEventTrigger', 'EventTriggerDDLCommandStart', 'EventTriggerInvoke', 'filter_event_trigger'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'type-system-and-typcache',
        "title": '类型系统与 typcache',
        "page_file": 'l01-type-system-and-typcache.md',
        "boundary": 'pg_type、类型缓存、I/O 与比较函数契约',
        "refs": [
            {
                "path": 'src/backend/utils/cache/typcache.c',
                "symbols": ['lookup_type_cache', 'lookup_rowtype_tupdesc'],
            },
            {
                "path": 'src/backend/utils/cache/lsyscache.c',
                "symbols": ['getTypeInputInfo', 'getTypeOutputInfo'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'array-types',
        "title": '数组类型',
        "page_file": 'l02-array-types.md',
        "boundary": '数组构造/解构、多维与下标语义',
        "refs": [
            {
                "path": 'src/backend/utils/adt/arrayfuncs.c',
                "symbols": ['array_in', 'array_out', 'construct_md_array', 'deconstruct_array'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'numeric-type',
        "title": 'numeric 与 decimal',
        "page_file": 'l03-numeric-type.md',
        "boundary": '任意精度运算、舍入与显示',
        "refs": [
            {
                "path": 'src/backend/utils/adt/numeric.c',
                "symbols": ['numeric_in', 'numeric_out', 'numeric_add', 'numeric_cmp'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'date-time-types',
        "title": '日期时间类型',
        "page_file": 'l04-date-time-types.md',
        "boundary": '时间戳/间隔/时区转换与运算',
        "refs": [
            {
                "path": 'src/backend/utils/adt/timestamp.c',
                "symbols": ['timestamp_in', 'timestamp_out', 'timestamptz_in', 'interval_in'],
            },
            {
                "path": 'src/backend/utils/adt/datetime.c',
                "symbols": ['DecodeDateTime', 'ParseDateTime'],
            },
            {
                "path": 'src/backend/utils/adt/date.c',
                "symbols": ['date_in', 'date_out'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'json-jsonb-types',
        "title": 'JSON/JSONB',
        "page_file": 'l05-json-jsonb-types.md',
        "boundary": '解析、存取路径、容器操作与 GIN 支撑',
        "refs": [
            {
                "path": 'src/backend/utils/adt/jsonb.c',
                "symbols": ['jsonb_in', 'jsonb_out'],
            },
            {
                "path": 'src/backend/utils/adt/jsonb_util.c',
                "symbols": ['JsonbValueToJsonb', 'JsonbIteratorNext'],
            },
            {
                "path": 'src/backend/utils/adt/jsonb_gin.c',
                "symbols": ['gin_extract_jsonb'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'text-and-varlena',
        "title": 'text 与 varlena',
        "page_file": 'l06-text-and-varlena.md',
        "boundary": '变长属性、编码/长度、字符串函数',
        "refs": [
            {
                "path": 'src/backend/utils/adt/varlena.c',
                "symbols": ['cstring_to_text', 'text_to_cstring', 'text_catenate', 'text_length'],
            },
            {
                "path": 'src/backend/utils/adt/varchar.c',
                "symbols": ['varchar', 'bpchar'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'range-and-multirange',
        "title": '范围类型',
        "page_file": 'l07-range-and-multirange.md',
        "boundary": '范围构造、包含/重叠运算',
        "refs": [
            {
                "path": 'src/backend/utils/adt/rangetypes.c',
                "symbols": ['range_in', 'range_out', 'range_contains'],
            },
            {
                "path": 'src/backend/utils/adt/multirangetypes.c',
                "symbols": ['make_multirange', 'multirange_contains_elem'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'network-types',
        "title": '网络类型',
        "page_file": 'l08-network-types.md',
        "boundary": 'inet/cidr/macaddr 解析与运算',
        "refs": [
            {
                "path": 'src/backend/utils/adt/network.c',
                "symbols": ['network_in', 'network_out', 'inet_out'],
            },
            {
                "path": 'src/backend/utils/adt/mac.c',
                "symbols": ['macaddr_in', 'macaddr_out'],
            },
        ],
    },
    {
        "feature_group": 'L-SQL类型与内建函数',
        "feature": 'regex-and-fulltext',
        "title": '正则与全文',
        "page_file": 'l09-regex-and-fulltext.md',
        "boundary": '正则引擎调用、tsvector/tsquery 运算',
        "refs": [
            {
                "path": 'src/backend/utils/adt/regexp.c',
                "symbols": ['RE_compile_and_execute', 'textregexeq', 'regexp_matches'],
            },
            {
                "path": 'src/backend/utils/adt/tsvector_op.c',
                "symbols": ['TS_execute', 'ts_match_tt'],
            },
            {
                "path": 'src/backend/utils/adt/tsquery.c',
                "symbols": ['parse_tsquery'],
            },
        ],
    },
]
