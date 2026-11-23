---
project: redis
feature: rdb-snapshot
page_type: feature
schema_version: 1
refs:
  - path: src/rdb.c
    symbols: [rdbSave, rdbLoad]
---

# RDB 快照格式与落盘

## 一句话职责

`rdbSave` 把整个数据集在某一时刻序列化成一份自描述的二进制快照并原子地替换到磁盘上，`rdbLoad` 把这样一份快照读回内存、还原成数据集。

## 机制是什么

RDB 是 Redis 的全量快照格式，文件内容是一条 opcode 流：开头 9 字节是魔数 `REDIS` 加四位版本号（`src/rdb.h` 里 `RDB_VERSION` 定义为 17，因此实际写出的是 `REDIS0017`），中间是若干「单字节 opcode + 变长参数」，结尾是一个 EOF opcode 与 8 字节的 CRC64 校验和。

格式叠了三层，缺一层就读不懂：

- **opcode 层**——`rdbSaveType` 写一个字节，`rdbLoadType` 读一个字节。同一个字节空间既表示对象类型（字符串、列表、集合……），也表示控制指令（选库、写过期时间、EOF 等）。
- **长度层**——`rdbSaveLen` 把长度压成 6 位、14 位、32 位、64 位四种形态，读侧按首字节高两位判形态：6 位以下占 1 字节，14 位以下占 2 字节，32 位以内占 1+4 字节，更大占 1+8 字节。
- **字符串层**——`rdbSaveRawString` 决定一段字节是按整数编码存、按 LZF 压缩存，还是按「长度 + 原始字节」原样存。

落盘不是直接写目标文件。`rdbSave` 先写 `temp-<pid>.rdb`，成功后再 `rename` 到目标路径，最后 fsync 目录；源码注释写明理由是「Use RENAME to make sure the DB file is changed atomically only if the generate DB file is ok」。目标路径上于是要么是上一份完整快照，要么是这一份完整快照，不会留下半截文件。

读回路径分四层：`rdbLoad` → `rdbLoadWithEmptyFunc` → `rdbLoadRio` → `rdbLoadRioWithLoadingCtx` → `rdbLoadRioWithLoadingCtxInternal`，最后一层才是逐 opcode 解析的循环。

本页只讲快照本身的序列化与反序列化。写命令的追加日志是 AOF（g02），把快照传给副本的传输协议属于复制（h01）。

## 怎么实现的

**写出侧的外壳。** `rdbSave` 先 `startSaving(rdbflags)`（给模块发持久化开始事件），把临时文件名拼成 `temp-<pid>.rdb`，调 `rdbSaveInternal`；失败则 `stopSaving(0)` 并返回 `C_ERR`。成功后才 `rename`，再 `fsyncFileDir`；两步都成功后把 `server.dirty` 清零、`server.lastsave` 记为当前时间、`server.lastbgsave_status` 记 `C_OK`，最后 `stopSaving(1)`。

`rdbSaveInternal` 负责真正的文件操作：`fopen` 打开临时文件（失败时打日志并带上当前工作目录），`rioInitWithFile` 建 rio；若配置项 `rdb-save-incremental-fsync` 打开（默认打开），用 `rioSetAutoSync` 按 `REDIS_AUTOSYNC_BYTES` 边写边 sync——`src/server.h` 里这个常量是 `1024*1024*4`，注释写「Sync file every 4MB」——并在不带 `RDBFLAGS_KEEP_CACHE` 时打开 `rioSetReclaimCache`。`rdbSaveRio` 成功后依次 `fflush`、`fsync(fileno(fp))`、`reclaimFilePageCache`、`fclose`。任何一步出错都走 `werr`：打日志、`fclose`、`unlink` 掉这个临时文件，避免残留半截文件。

**快照主体。** `rdbSaveRio` 的顺序是：写 9 字节魔数 → `rdbSaveInfoAuxFields` → 模块的 BEFORE_RDB 辅助数据 → 函数库 → 哈希模板注册表 → 逐库 `rdbSaveDb` → 模块的 AFTER_RDB 辅助数据 → EOF opcode → 8 字节 CRC64（`server.rdb_checksum` 关闭时不更新校验和，写出去的校验和是 0，读侧据此跳过检查）。末尾打一条日志报「BGSAVE done, %ld keys saved, %llu keys skipped, %zu bytes written.」。`req` 上的 `SLAVE_REQ_RDB_EXCLUDE_DATA` 与 `SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS` 两个标志可以整段跳过数据与函数库。

**AUX 字段。** `rdbSaveInfoAuxFields` 写的是「快照生成时的环境信息」：`redis-ver`（版本号）、`redis-bits`（指针宽度，64 或 32）、`ctime`（生成时刻）、`used-mem`（当时的 `zmalloc_used_memory()`）；`rsi` 非空时再加 `repl-stream-db`、`repl-id`、`repl-offset`；再加一个 `aof-base`（本文件是否作为 AOF 前导）；集群模式下还有 `cluster-asm-task`。

**逐库与逐键。** `rdbSaveDb` 先写 `RDB_OPCODE_SELECTDB` 与库号，再写 `RDB_OPCODE_RESIZEDB` 与「键数、过期键数」两个长度（读侧据此预分配，避免无用的 rehash），空库直接返回 0 不写任何东西。然后用 kvstore 迭代器逐键写；集群模式下换槽时额外写 `RDB_OPCODE_SLOT_INFO`（槽号 + 该槽键数 + 该槽过期数）。正在被槽位迁移清理的槽（`isSlotInTrimJob`）整个跳过并计入 `skipped` 计数。子进程里为了压写时复制：换槽时对上一个槽调 `dismissDictBucketsMemory`，单键写出字节数超过 `server.page_size/2` 时对刚写完的对象调 `dismissObject`。进度上报是「每 1024 个键查一次时钟，每秒才发一次 `sendChildInfo`」，源码注释说明这是为了避免每次迭代都调 `mstime()`。

**键值对。** `rdbSaveKeyValuePair` 的字段顺序是：有过期时间就写 `RDB_OPCODE_EXPIRETIME_MS` 加毫秒时间戳；淘汰策略带 LRU 标志时写 `RDB_OPCODE_IDLE` 加空闲秒数（`estimateObjectIdleTime` 估算后再除以 1000，注释说「Using seconds is enough and requires less space」）；带 LFU 标志时写 `RDB_OPCODE_FREQ` 加 1 字节频率（`LFUDecrAndReturn`，注释解释频率是对数刻度所以 0–255 一个字节够用，且不存减半时间，因为载入时重置一次对频率影响不大）；键被标记 NO-EVICT 时写 `RDB_OPCODE_KEY_NOEVICT`；模块元数据位非零时写模块元数据；最后才是「类型字节 + 键 + 值」。

**类型字节按编码选。** `rdbSaveObjectType` 是「内部编码 → 文件格式」的那张映射表：字符串写 `RDB_TYPE_STRING`；列表的 quicklist 与 listpack 都写 `RDB_TYPE_LIST_QUICKLIST_2`；集合按编码写 intset、普通 set、带成员过期的 `RDB_TYPE_SET_METADATA`、listpack、listpack_ex；有序集合的 listpack 与 skiplist/btree 分别写 `RDB_TYPE_ZSET_LISTPACK` 与 `RDB_TYPE_ZSET_2`；哈希按编码写 listpack、listpack_ex、普通 hash、带字段过期的 `RDB_TYPE_HASH_METADATA`、模板 hash 的四种形态；流固定写 `RDB_TYPE_STREAM_LISTPACKS_5`；模块值写 `RDB_TYPE_MODULE_2`。落到文件里的类型字节与当下的内存编码并非一一对应——同一类对象的不同编码可能共用同一个类型字节，具体形态由类型字节后面的数据决定。

**值的分支。** `rdbSaveObject` 按 `o->type` 分派，各分支再按 `o->encoding` 分派：

- 字符串交给 `rdbSaveStringObject`：已经是整数编码就直接写整数，不解码再编码一遍。
- 列表：quicklist 写成「元素总数 + 每个节点的容器类型 + 节点内容」，节点内容若已压缩就写 LZF blob；listpack 编码则伪装成一个只有单节点的 quicklist（写元素数 1、`QUICKLIST_NODE_CONTAINER_PACKED`、listpack 原始字节）。
- 集合：哈希表编码先看有没有成员级过期——有就写最早的过期时刻，再用「相对该时刻的 TTL + 1」表示每个成员；整数集合与 listpack 直接写原始 blob；listpack_ex 先写最早过期时刻再写 blob。
- 有序集合：listpack 直接写 blob；skiplist 分支从**大到小**写出，源码注释给的理由是「the next loaded element will always be the smaller, so adding to the skiplist will always immediately stop at the head, making the insertion O(1) instead of O(log(N))」（源码注释同时说明 skiplist 只是临时编码，键空间里的有序集合是 listpack 或 btree）；btree 分支先写元素数再逐个写成员与分值（分值用二进制 double）。
- 哈希：模板编码在普通 RDB 里写紧凑引用形态（`TMPL_ARRAY` 是「模板 id + 各字段值」，`TMPL_LP` 是原始 listpack blob，模板 id 就是 blob 里的第一个条目）；在 DUMP/RESTORE 载荷里写自包含形态——字段名单独编码，字段名是塞进一个 listpack 还是逐个写出，由模板能否塞进 listpack 决定；listpack 编码直接写 blob（带字段过期时先写最早过期时刻）；哈希表编码有字段过期就写 `RDB_TYPE_HASH_METADATA` 布局（每个字段前加一个 TTL），否则写普通布局。
- 流：写出 rax 里的全部 listpack（键 + blob），再加元素总数、最后一个条目 ID、第一个条目 ID、最大墓碑 ID、`entries_added`；然后是消费者组数量、每组的组名、last_id、`entries_read`、全局 PEL、消费者列表、NACK 区（NACK 条目数 + 各 NACK ID）；最后是 IDMP 的时长、最大条目数、条目列表与两个累计计数。
- 模块值：先写模块类型 id，再调模块自己的 `rdb_save`，最后写 `RDB_MODULE_OPCODE_EOF` 结束标记。

**字符串的三种存法。** `rdbSaveRawString` 先试整数编码（长度不超过 11 字节时才有意义），失败后若 `server.rdb_compression` 打开且长度大于 20 就试 LZF 压缩——源码注释说「under 20 bytes it's unable to compress even aaaaaaaaaaaaaaaaaa so skip it」——压缩不划算才退回原样存储。

**读回的外壳。** `rdbLoad` 是 `rdbLoadWithEmptyFunc` 的薄封装（`emptyDbFunc` 传 NULL）。`rdbLoadWithEmptyFunc` 打开文件，`errno == ENOENT` 时返回 `RDB_NOT_EXIST`（不是错误，只是「没有快照」）；用 `fstat` 取真实大小，`loadingSetFlags` 打开载入状态并初始化进度，`emptyDbFunc` 若非空则先清空现有数据集，`loadingFireEvent` 通知模块。rio 的初始化分两种：拿到真实大小时用 `rioInitWithFileLoad`（注释说它需要真实文件大小才知道尾部校验和从哪里开始），`fstat` 失败才退回普通 `rioInitWithFile`。有 `HAVE_FADVISE` 时额外 `posix_fadvise(..., POSIX_FADV_SEQUENTIAL)`，让内核用更大的预读窗口。随后 `rdbLoadRio`；失败时再调一次 `emptyDbFunc` 清掉半截数据，最后 `stopLoading`。

**头部校验。** 先读 9 字节，`memcmp` 比 `"REDIS"`，然后 `atoi(buf+5)` 取版本号；版本号不在 1 到 `RDB_VERSION` 之间就打日志「Can't handle RDB format version %d」并返回 `C_ERR`。

**opcode 循环。** 主循环每次读一个类型字节，再按类型分派：

- `EXPIRETIME`（秒）与 `EXPIRETIME_MS`（毫秒）先记下过期时间再 `continue` 读下一个 opcode；`FREQ` 读 1 字节频率；`IDLE` 读一个长度当空闲时间；`KEY_NOEVICT` 只置一个标志位。
- `EOF` 跳出循环。
- `SELECTDB` 切库；库号超出 `server.dbnum` 时打「FATAL: Data file was created with a Redis server configured to handle more than %d databases. Exiting」并 `exit(1)`。
- `RESIZEDB` 记下键数与过期数，等下一个键真正读入前调 `dbExpand` 与 `dbExpandExpires` 预分配；集群模式下 `SLOT_INFO` 则按槽调 `kvstoreDictExpand`。槽号越界会走 `rdbReportCorruptRDB`——源码注释解释了原因：槽号直接当 kvstore 的下标，畸形 RDB 能给出越界值导致数组越界访问。
- `AUX` 读出键值两个字符串后按名字分派：`repl-stream-db` / `repl-id` / `repl-offset` 填进 `rsi`；`redis-ver`、`ctime`、`used-mem`、`aof-preamble`、`aof-base`、`cluster-asm-task` 各有各的处理；`lua` 字段被明确忽略（注释写「Won't load the script back in memory anymore」）；不认识的名字按 AUX 字段契约忽略并打一条 DEBUG 日志。
- `MODULE_AUX` 找模块类型并调它的 `aux_load`；找不到模块或模块不支持 AUX 时打日志并 `exit(1)`。
- `FUNCTION_PRE_GA` 直接报「Pre-release function format not supported.」并退出；`FUNCTION2` 交给 `rdbFunctionLoad`；`HASH_TEMPLATE` 交给 `rdbLoadHashTemplate`。

**键的处理。** 读到真正的键类型后先 `rdbResolveKeyType`，再以 `RDB_LOAD_SDS` 读出键（直接拿 sds 而不是包装成对象），值交给 `rdbLoadObject`。值读失败时区分两种错误：`RDB_LOAD_ERR_EMPTY_KEY` 说明是空键，源码注释说这是历史 bug（引用了 issue 8453）留下的，所以选择静默丢弃继续加载，前 10 条会打一条 NOTICE；其他错误直接走文件尾错误分支。

**过期键在载入时的处理。** 只有一个很窄的条件会丢弃已过期的键：本机是主库（`iAmMaster()`）、文件不是 AOF 前导（`!(rdbflags & RDBFLAGS_AOF_PREAMBLE)`）、且 `expiretime` 已经小于当前时间。源码注释解释了为什么不普遍丢弃：从主库收到的 RDB 里过期由主库负责，副本自己再过期一次会让主副本不一致；base 文件是 RDB 格式的 AOF 时也要把键全加载，因为增量 AOF 的日志假定键空间状态精确匹配。另外在 `RDBFLAGS_FEED_REPL` 下，丢弃的动作会同时用 `replicationFeedSlaves` 把 DEL/UNLINK 推进复制积压。

**重复键。** 默认直接 `serverPanic("Duplicated key found in RDB file")`。只有 `RDBFLAGS_ALLOW_DUP` 时才先 `dbSyncDelete` 再插入——源码注释说这个标志是给 `DEBUG RELOAD` 的特殊模式用的。

**载入后的收尾。** 键插进 db 后还要补三件事：带字段或成员过期的哈希与集合注册进 `subexpires`；流注册给 IDMP 生产者做基于 cron 的过期；`objectSetLRUOrLFU` 恢复 LRU/LFU 计数。循环结束后，版本号不小于 5 才校验 CRC64；写文件时关掉校验的话文件里的校验和是 0，读侧打一条 NOTICE 后跳过；不匹配则打「Wrong RDB checksum expected: (%llx) but got (%llx). Aborting now.」并报损坏。读到文件尾之前意外结束走文件尾错误分支：打「Short read or OOM loading DB. Unrecoverable error, aborting now.」并调 `rdbReportReadError`——注释说这个函数在多数情况下会让 Redis 退出，但从 socket 读全量同步时只报错让调用方重试。

**长时间载入不阻塞服务。** `rdbLoadProgressCallback` 挂在 rio 的读回调上，每读到 `server.loading_process_events_interval_bytes` 的整数倍就更新进度、调 `processEventsWhileBlocked` 回一次事件循环，并在处于全量同步传输态时给主库发一个换行保活。

**退化路径与真实调用方。** 目标文件不存在时 `rdbLoad` 返回 `RDB_NOT_EXIST` 而不是报错——`src/server.c` 的启动装配据此走「AOF 或空库」的分支；文件存在但内容坏了则返回 `C_ERR`，启动路径直接 `exit(1)`。`src/rdb.c` 的 `saveCommand` 是 `rdbSave` 的同步调用方，`bgsaveCommand` 与 `src/server.c` 的定时触发走的是 `rdbSaveBackground`（fork 子进程，父进程只记 `rdb_save_time_start` 与 `rdb_child_type`）。`src/debug.c` 的 `DEBUG RELOAD` 是先 `rdbSave` 再 `rdbLoad` 的调试入口，AOF 打开时它还会补一次 `flushAppendOnlyFile(1)`。`src/db.c` 的 `flushAllDataAndResetRDB`（FLUSHALL/FLUSHDB）在数据被清空之后、且配置了保存点时，会同步补一次 `rdbSave`，把磁盘上的快照重写成与已清空的数据集一致。`src/module.c` 的模块接口也能通过 `rdbLoad` 载入文件。

**磁盘无盘同步。** 复制全量同步走的是 `rdbSaveToSlavesSockets` 与 `rdbSaveRioWithEOFMark`：后者在 RDB 内容前面加一段 `$EOF:` 加 40 字节随机十六进制加换行，后面加同样那 40 字节十六进制（`RDB_EOF_MARK_SIZE` 在 `src/server.h` 里定义为 40），接收方靠尾部这串标记判断载荷何时结束而无需解析内容。这属于复制的传输协议，本页不展开。

## 关键取舍

**源码事实：** 快照替换是「临时文件 + rename + fsync 目录」三步，rename 是原子的，因此目标文件不会出现半截内容。**源码事实：** 写快照默认 fork 子进程（`rdbSaveBackground` 用 `redisFork(CHILD_TYPE_RDB)`），父进程靠 `dismissDictBucketsMemory` 与 `dismissObject` 主动归还内存来压低写时复制的放大。**源码事实：** 格式分 opcode、长度、字符串三层，每层各自压缩。**源码事实：** AUX 字段有向前兼容契约：不认识的字段必须跳过而不是报错。**源码事实：** 版本号被限制在 1 到 17 之间，超出范围直接拒绝加载；CRC64 只在版本不小于 5 时才校验。**源码事实：** 重复键默认 panic，只有 `DEBUG RELOAD` 用的 `RDBFLAGS_ALLOW_DUP` 才放行。**源码事实：** 载入时丢弃过期键的条件被限定在「本机是主库且文件不是 AOF 前导」。**源码事实：** skiplist 编码的有序集合故意反序写出，把载入时的插入复杂度从 O(log N) 压到 O(1)。**源码事实：** 写入时按当下配置决定要不要带 LRU/LFU 信息，带了才恢复，否则恢复出来的对象淘汰起点是错的。**源码事实：** 类型字节与内存编码不是一一对应，读侧会按当前配置重新选择内存编码——写哈希表编码的集合用 `RDB_TYPE_SET`，读回来时若元素数不超过当前的 `set_max_intset_entries` 就建成整数集合。

**架构推论：** 反序写出 skiplist 是用「文件里元素的顺序」换「载入时的插入成本」，代价是文件不再是分值的自然顺序，依赖顺序的外部工具读这个文件会看到倒序。**架构推论：** 用 fork + 写时复制而不是「复制一份数据集再写盘」，把快照的内存成本从 O(数据集) 降到 O(被写时触碰的页)，代价是写盘期间父进程仍在服务，写时复制带来的额外内存峰值无法完全消除。**架构推论：** 格式按「内部编码」分支写出、读回时又按当前配置重建编码，使文件格式与运行时状态之间多了一层松耦合：同一份 RDB 在不同配置下恢复出的内存布局可能不同。**架构推论：** 把「丢弃过期键」限定为主库专属，是把副本的一致性责任完全推给主库；代价是主库自身在载入路径上多了一份判定逻辑，且副本重启后必须等主库再次传播删除才能收敛。**架构推论：** 遇到重复键直接 panic 而不覆盖，是把「快照文件损坏」当成不可恢复的启动期错误；代价是运维无法自动救回，只能换文件重启动。主核若做快照恢复，更常见的做法是保留最后一份并让恢复失败可回退，而不是让整库不可用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/rdb.c` | `rdbSave` | 快照落盘的入口：临时文件、原子改名、目录 fsync 与保存后的记账 |
| `src/rdb.c` | `rdbLoad` | 快照读回的入口：打开文件、载入期状态管理、解析循环与失败清理 |

## 参考

- [Redis 源码：rdb.c](https://github.com/redis/redis/blob/unstable/src/rdb.c) —— 快照格式的写出与读回实现。
- [Redis 源码：rdb.h](https://github.com/redis/redis/blob/unstable/src/rdb.h) —— opcode、类型字节与版本号常量的定义。
- [Redis 文档：持久化](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/) —— RDB 与 AOF 两种持久化方式的定位与取舍。
- [Redis 命令：BGSAVE](https://redis.io/docs/latest/commands/bgsave/) —— 触发后台快照的命令语义。
- [Redis 命令：SAVE](https://redis.io/docs/latest/commands/save/) —— 同步快照的命令语义。

## 与主核 / AI 的关系

Redis 这套快照机制里最值得主核借的是「先写临时文件、再原子改名」这一层：主核的多模态对象（尤其是大向量段）在序列化时同样需要一份「要么完整要么不存在」的落地物，否则崩溃后恢复出来的库可能带着半截段。需要注意的是 Redis 的快照是「某一时刻」的一致切面，靠的是 fork 出的子进程看到父进程冻结后的内存视图；主核是多写者引擎，没有等价的内存快照机制，就得靠 WAL 加事务边界自己切出这个一致点——这正是主核把持久化做成 WAL 而不是全量快照的原因。

「按内部编码分别写出、读回时按当前配置重新选择内存编码」这套思路对主核的直接价值有限：主核的向量段与元数据布局跟 Redis 的 listpack、intset 没有对应物，抄这套分支只会把主核自己的存储格式耦合到运行时配置上。

AI 侧若用 Redis 承担推理结果缓存，RDB 快照决定的是「缓存能不能扛过一次重启」：快照文件越大，fork 的写时复制峰值与落盘耗时越高，`rdb-save-incremental-fsync` 打开时还会在每写满 4MB 就 sync 一次，把刷盘成本摊进快照过程。纯缓存场景通常更愿意让上层在重启后重建而不是靠快照恢复，这时关掉 AOF 与 RDB、把预算留给内存反而更划算。
