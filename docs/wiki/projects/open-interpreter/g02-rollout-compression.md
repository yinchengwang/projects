---
project: open-interpreter
feature: rollout-compression
title: 会话文件压缩
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/rollout/src/compression.rs
    symbols: [RolloutCompressionTrigger, spawn_rollout_compression_worker, materialize_rollout_for_append_blocking]
  - path: codex-rs/rollout/src/compression.rs
    symbols: [compressed_rollout_path, COMPRESSED_SUFFIX, MAX_NOT_FOUND_RETRIES]
---

# 会话文件压缩

## 一句话职责

把冷掉的会话文件转成 zstd，同时保证「压缩」与「追加」这两个会同时发生的事**永远不重叠**：压缩侧在发布前后都重新校验源文件状态，并只在拿到该线程的写锁后才把新文件换进去、删掉旧文件。

## 机制是什么

会话文件有两种表示：plain `.jsonl` 与 compressed `.jsonl.zst`。`COMPRESSED_SUFFIX` 就是那个 `".zst"`，`compressed_rollout_path(path)` 与 `plain_rollout_path(path)` 是双向换算——前者追加后缀（若已是压缩路径则原样返回），后者剥离后缀。

压缩由 `RolloutCompressionTrigger` 区分的两个入口触发：`Startup`（进程启动）与 `Rpc`（某个调用触发）。两个值都通过 `tag()` 变成 `"startup"` / `"rpc"` 字符串，进入所有指标的标签。枚举上的注释特意澄清了一件事：「这是**请求压缩的那个入口**，不是 Statsig 的分组」——避免读者把它误解成实验配置。

`spawn_rollout_compression_worker(codex_home, trigger)` 是那个「发射即忘」的后台任务，docstring 把三条性质讲全了：失败只记日志、启动不被阻塞、`codex_home` 下的一个运行标记防止同一本地存储的压缩互相重叠或过于频繁。

## 怎么实现的

**为什么要压缩，以及为什么不能简单压缩。** 会话文件是**活文件**——只要用户还在用那个会话，`RolloutRecorder` 就会持续追加。而压缩是一个「整个文件读一遍、写一个新文件、删掉旧文件」的操作。两者同时在同一个路径上演，就会得到三种灾难：压缩读到一半时追加了内容（压缩产物含陈旧数据）、压缩删掉旧文件的同时追加写进了已删的 inode、或者追加打开了旧文件而压缩把它换成了新文件。

所以这一页的全部复杂度都在处理这件事。三层防护依次是：**冷度判据**（只压 7 天没动过的）、**两次状态校验**（编码前后各一次）、**发布期写锁**（换文件与删旧文件期间禁止该线程写入）。

**冷度判据。** `MIN_ROLLOUT_AGE = 7 * 24 * 60 * 60` 秒（7 天）。`cold_file_state(path)` 先 stat，取 `len`、`modified`、`permissions` 组成 `FileState`；文件不存在或不是普通文件则返回 `NotCold(None)`；`age < MIN_ROLLOUT_AGE` 返回 `NotCold(Some(state))`；否则 `Cold(state)`。

`NotCold` 带一个 `Option<FileState>` 是为了指标：知道「有多大但不够冷」可以画「压缩为什么没触发」的分布。而 `None` 表示「连状态都没有」（不存在/非文件），那是另一类跳过。

**三层防护的完整顺序。** `compress_rollout_if_cold_blocking` 是这个模块的核心，值得逐步走：

1. `cold_file_state` 取 `before`。不够冷就返回 `SkippedNotCold`。
2. 若 `compressed_path` 已存在 → `SkippedAlreadyCompressed`（避免重压）。
3. 建临时文件（`tempfile::Builder` 带 `rollout-compress-` 前缀与 `.tmp` 后缀，落在同目录）。
4. `encode_zstd_to_writer` 流式编码，并 `set_pledged_src_size`（注释：「保留分页历史的快速字节上界检查，不必解码整个文件」——zstd 会把原始大小写进帧头，读侧可以不解码就知道总字节数）。
5. `flush` → `verify_zstd`（把整个压缩文件解码到一个 sink 里走一遍，确保产物真的可读）。
6. **第二次** `same_file_state(path, &before)`：源文件变了（长度/修改时间/权限任一不同）就返回 `SkippedChanged`。
7. `set_file_metadata` 把原文件的修改时间与权限搬到临时文件上。
8. `sync_all` 落盘，读 `compressed_bytes`。
9. **取发布锁**：`writer_locks.try_acquire_for_publication(thread_id)`。返回 `None` 说明有活跃写入者 → `SkippedBusy`。
10. **第三次** `same_file_state` 校验（拿到锁之后再来一次，因为从第 6 步到这里有时间窗）。
11. `temp_file.persist_noclobber(compressed_path)`：原子发布且不覆盖。若已存在（另一进程刚发布了）→ `SkippedAlreadyCompressed`。
12. **第四次** `same_file_state` 校验：这里若变了，就**删掉刚发布的压缩文件**并返回 `SkippedChanged`——因为压缩产物已不可信，留着会让后续读取拿到陈旧内容。
13. `remove_file(path)` 删掉 plain 原件。

四次状态校验看起来冗余，其实每一次都对应一个不同的时间窗：3 到 6 之间是编码期（可能几秒到几十秒，`RolloutRecorder` 完全可能在追加）；6 到 10 之间是元数据设置与落盘；10 到 11 之间是发布；11 到 12 之间是「已发布但还没删原件」——这个窗口里若有人追加，压缩件与原件都会存在且内容不同，所以第 12 步必须校验并回滚。

源码在那段写了一句很准确的注释：「编码与校验不阻塞写入者。协调机制防止的是我们在**重检、发布、删除原件**期间被写入者取得锁。」

这句话点明了锁的范围：不是「整个压缩过程持锁」，而是「只在最后那段临界区持锁」。否则一次几十秒的编码会把整个会话锁死。

**源文件状态比对是三项，不是一项。** `same_file_state` 比 `len`、`modified`、`permissions`。加权限是必要的：某些工具链会 chmod，那也意味着文件被动过。用 `modified()` 直接比 `SystemTime` 而不是比秒级时间戳，避免精度丢失导致的误判。

**发布用「链接 + 不覆盖重命名」两种手段。** `persist_temp_file_noclobber` 用 `tempfile::TempPath::persist_noclobber`，把「文件已存在」当成成功（`AlreadyExists` → `Ok(())`）而不是错误。所以并发两个压缩任务同时做同一个文件时，第二个会安静地把自己的临时文件丢掉。

而 `materialize_rollout_for_append_blocking` 走相反方向（压缩 → plain），它的发布手法更细：先 `std::fs::hard_link(temp, plain)`，`AlreadyExists` 也算成功；只有硬链接不可用的场景才退到 `persist_temp_file_noclobber`。硬链接优先是因为它是原子的且不复制数据——解压出来的几 MB 文件可以瞬间「出现」。

**materialize 的对称问题。** 压缩是「活文件 → 冷存档」，materialize 是「冷存档 → 活文件」。它必须先检查 `plain_path.exists()`：存在就直接返回（`"plain_exists"`），因为活文件优先于压缩件。然后检查压缩件是否存在，都不存在时返回 plain 路径（`"missing"`）——所以这个函数对「两个都没有」的情况不报错，返回一个还不存在的路径，调用方自己会去创建。

解压后的步骤与压缩对称：建临时文件 → 解码写入 → `flush` → `sync_all` → 发布 → `set_times` 恢复修改时间 → `sync_all` → 删临时 → 删压缩件。其中有 `metadata.modified()?` 在 `set_times` 那行内再取一次——说明它在恢复时间戳这件事上依赖原始元数据仍在手（前面 `std::fs::metadata` 拿到过）。

`stage` 变量是这个函数的诊断骨架：从 `"prepare_directory"` 开始，每一步前更新，失败时 `FailureMetric::Materialize.record(stage, err)`。所以错的指标能精确到「在哪一步失败」，而不是笼统的「materialize 失败」。这是把「阶段」做成可观测维度的直接做法。

**异步版只是包一层。** `materialize_rollout_for_append(path, writer_lock)` 做 `spawn_blocking(move || { let _writer_lock = writer_lock; materialize_..._blocking(path) })`。

`let _writer_lock = writer_lock;` 这一行是移动语义的显式表达——把守卫移进闭包，让它在整个阻塞任务期间存活。若忘了这一行，守卫会在 `spawn_blocking` 调用返回后立刻 drop，锁就提前释放了。用 `_` 开头的绑定名而不是 `drop`，是为了「持有但不用」。

**读侧对表示切换的容错。** `open_rollout_line_reader` 有一个 `MAX_NOT_FOUND_RETRIES = 3` 的重试循环：遇到 `NotFound` 就 sleep `50ms` 再试。

注释解释了为什么需要它：「若请求的路径在**表示切换**期间消失，这里会短暂重试解析，让调用方不必知道磁盘上是哪种表示」。具体场景是：压缩删掉 plain 后、还没来得及让你看到压缩件，这中间有一瞬两个都不存在（因为 `remove_file` 发生在 `persist_noclobber` 之后，但路径解析与打开之间有间隔）。三次 × 50ms 覆盖这个窗口。

读取时按 `is_compressed_rollout_path` 分两条：压缩件走 `spawn_blocking` 里建 `zstd::Decoder` 加 `BufReader`，plain 走异步 `tokio::fs::File`。所以**读取压缩件是阻塞 I/O**——因为 zstd 的解码是同步的。这个取舍很实际：异步解码要额外的线程池与缓冲管理，而读会话文件本来就是冷路径。

`existing_rollout_with_metadata` 表达的优先级是「plain 优先于 compressed」，并**同时返回 metadata**。注释说这个设计的目的：「返回元数据让调用方不必再 stat 一次就能检查选中的文件」。所以一次 `existing_rollout_path` 调用完成两件事。

**运行标记的三态。** `CompressionRunMarker::try_claim` 返回 `Option<Self>`：

- 用 `create_new(true)` 建标记文件成功 → `Some`（拿到了）。
- `AlreadyExists` → 读标记文件的修改时间，不够旧（`< RUN_MARKER_STALE_AFTER` 即 6 小时）就返回 `None`（别人在跑）。
- 够旧 → 删掉再重建，成功则 `Some`，又被别人抢先则 `None`。

所以这是一个「带陈旧过期的互斥标记」。`RUN_MARKER_STALE_AFTER` 的 6 小时比 `WORKER_MAX_RUNTIME`（5 小时）略长——这个设计很紧：任何正常结束的压缩最多跑 5 小时，所以标记活过 6 小时必然是崩溃残留。

`Drop` 里默认 `remove_on_drop: true` 会删标记；正常结束时调 `persist()` 把它置成 `false`——**标记留在磁盘上**。这个反向逻辑值得注意：成功完成时**保留**标记，让下一次压缩在 6 小时内被跳过（防止过频）；异常退出时删掉，让下一次能立刻重试。所以这个标记同时承担「运行中互斥」与「最近已跑过」两件事。

**双层互斥。** `run` 先取 `try_acquire_rollout_maintenance_lock(codex_home)`，拿不到就 `"skipped_maintenance"` 返回；再 `try_claim` 标记，拿不到就 `"skipped_already_running"`。

两个锁的语义不同：maintenance lock 是「整个 rollout 维护工作」（可能还包括别的维护）的锁，标记是「压缩这一项」的锁。分开的原因是一个进程里可能有多种维护任务，压缩不该与它们并发，但也不该让「别的维护正在跑」变成「压缩被记成刚跑过」。

**并发度是 2，且有反压。** `MAX_CONCURRENT_COMPRESSION_JOBS = 2`，循环里 `while jobs.len() >= MAX { collect_next_compression_job(...).await }`——满了就先收一个结果再起下一个。所以扫描（异步、轻）与压缩（阻塞、重）能流水线并行，但同时最多 2 个压缩在跑。

`JoinSet` 的 `spawn_blocking` 让每个压缩占一个阻塞线程。限成 2 是给 `spawn_blocking` 池留余量——那个池的默认大小有限，被压缩全占了会让其他阻塞任务（如读取、写入）排队。

**时间预算与提前退出。** `WORKER_MAX_RUNTIME = 5 * 60 * 60`（5 小时），在三处检查：目录栈弹出一项前、读目录条目循环里、两个根目录之间。超时则 `time_budget_exhausted = true` 并 `break`。

完成后 `completion` 标签取 `"time_budget"` 或 `"scan_finished"`，且注释明确「保留既有的 completed 结果：它表示这一趟返回了，而不是每个目录都被扫过或每个文件都被压缩过」。这是一条很诚实的指标语义说明——`status=completed` 不等于「全部压完了」。

**扫描时跳过某些文件的两条规则。** 扫描阶段 `if rollout_file.is_compressed() { continue; }`——压缩件不再压。而 `RolloutFile::from_path` 的注释说它对「被已存在 plain 文件遮住的压缩兄弟」返回 `None`，所以同一逻辑文件只被算一次。

**指标的错误分类。** 这一层的指标设计比功能本身更值得学：

- `metrics::file(trigger, outcome.tag())`——五种 outcome：`compressed`、`skipped_not_cold`、`skipped_busy`、`skipped_changed`、`skipped_already_compressed`。**跳过不是一个桶**，而是按原因分开的五类。这决定了「压缩为什么没生效」能否从指标上直接读出来。
- `FailureMetric::File(trigger).record(stage, err)`——失败带 `stage`（`prepare_directory`、`create_temp`、`encode_and_write`、`flush`、`verify`、`recheck_source`、`set_metadata`、`sync`、`read_metadata`、`writer_lock`、`publish`、`remove_source`）。
- 元数据读取失败的 warn 有一条严格约束写在注释里：「**只用有界标签与已解析的 id，绝不用路径或错误消息**」。代码也照做了：`%rollout_id`、`reason`、`error_kind`、`file_size_bytes`、`mtime_age_seconds` 全是有限取值或数值。同时 `MAX_METADATA_WARNINGS_PER_RUN = 5` 限制条数，防止一个坏目录刷满日志。
- 压缩比用整数直方图：`(compressed * 10_000) / source`，注释「让比例直方图保持整数值同时保留亚百分比精度」。因为指标 API 只收整数，直接发浮点会被截断成 0 或 1，失去意义。

**编码参数的三个选择。** `COMPRESSION_LEVEL = 3`（偏快的档位，因为这是后台任务，不值得为几个百分点花更多 CPU）。`set_pledged_src_size` 让帧头带原始大小，于是读侧记录边界检查不必解压。`verify_zstd` 是把产物整体解一遍——这个成本是「写的时候多读一次」，换来「读的时候不会遇到坏文件」，对归档数据是值得的。

## 关键取舍

**源码事实：** `MIN_ROLLOUT_AGE` 是 7 天。压缩只碰冷文件，所以「正在用的会话文件永不被压缩」。

**源码事实：** 编码与校验**不持锁**；只有「重检 → 发布 → 删原件」这段临界区持该线程的发布锁。源码注释明确写了这一点。

**源码事实：** 源文件状态比对是三元素（长度、修改时间、权限）。并且在编码前、编码后、取锁后、发布后各校验一次，共四次。

**源码事实：** 发布后用第四次校验发现源变了，会**删掉刚发布的压缩文件**并放弃。

**源码事实：** 运行标记成功时保留、异常时删除。所以它同时表达「正在跑」与「最近跑过」（6 小时窗口）。

**源码事实：** 两层互斥：maintenance lock（整个 rollout 维护）与 run marker（压缩本身）。

**源码事实：** 并发压缩上限 2，超过先收一个结果再起新的。

**源码事实：** `materialize_rollout_for_append_blocking` 优先用硬链接发布，`AlreadyExists` 视为成功。

**架构推论：** 这类「后台改写别人的活文件」的任务，正确的设计顺序不是「先加锁再干活」，而是「**先干完不可逆之外的所有事，最后才拿锁做那段不可回退的替换**」。压缩的 90% 时间（编码、校验、落盘）都可以在不持锁的情况下完成，只有「换文件」需要独占。把锁的范围缩到最小，是让「压缩不影响用户」成立的关键。

**架构推论：** 四次状态校验看来是过度防御，实际每一次都对应一个真实的失败模式。这类「检查—动作—再检查」的链在文件系统上没有事务可用时是唯一手段。判据是：**任何「我基于观察做了决定，然后去改被观察的对象」的地方，都必须重新观察再改**。

**架构推论：** 「成功保留标记、失败删除标记」这个方向可能看着反了（通常希望失败后能立刻重试），但配合 6 小时的过期窗口就自洽了：失败时删掉让下一次启动立刻能重试；成功时保留让下一次启动跳过（避免每次启动都扫一遍几百个文件）。它把「重试」与「降频」两个需求塞进了一个标记的两态里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/rollout/src/compression.rs` | `RolloutCompressionTrigger` | 启动 / RPC 两个触发入口的标签 |
| `codex-rs/rollout/src/compression.rs` | `spawn_rollout_compression_worker` | 发射即忘的后台压缩入口 |
| `codex-rs/rollout/src/compression.rs` | `compress_rollout_if_cold_blocking` | 四次校验加发布锁的核心流程 |
| `codex-rs/rollout/src/compression.rs` | `materialize_rollout_for_append_blocking` | 压缩件解回 plain，硬链接发布 |
| `codex-rs/rollout/src/compression.rs` | `compressed_rollout_path` | plain 与压缩路径的换算 |
| `codex-rs/rollout/src/compression.rs` | `COMPRESSED_SUFFIX` | `.zst` 后缀常量 |
| `codex-rs/rollout/src/compression.rs` | `MAX_NOT_FOUND_RETRIES` | 表示切换窗口内的重试次数 |
| `codex-rs/rollout/src/compression.rs` | `open_rollout_line_reader` | 透明处理两种表示的读取入口 |
| `codex-rs/rollout/src/compression.rs` | `same_file_state` | 长度、时间、权限三元素比对 |
| `codex-rs/rollout/src/compression.rs` | `CompressionRunMarker.try_claim` | 带陈旧过期的互斥标记 |
| `codex-rs/rollout/src/compression.rs` | `CompressionOutcome` | 五类结果，把跳过按原因分开 |
| `codex-rs/rollout/src/compression.rs` | `FailureMetric` | 带 stage 维度的失败指标 |
| `codex-rs/rollout/src/compression.rs` | `worker::run` | 双层互斥、时间预算与并发上限 |

## 参考

- [open-interpreter 源码：rollout/compression.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/rollout/src/compression.rs)
- [zstd 文档：帧头与 pledged source size](https://facebook.github.io/zstd/zstd_manual.html)
- [open-interpreter 文档](https://www.openinterpreter.com/docs)

## 与主核 / AI 的关系

主核里最接近的是「在线归档与压缩」：冷数据段被合并压缩后替换原段，而活跃写入可能正在同一个段上。做法同样是「先构建新产物、最后用一次原子替换换进去」，且替换前必须重新校验源状态。这一页的四次校验与主核的「代际 + 校验和」是同一职责的两种实现——主核用版本号加校验和，这里用三元素元数据。

与 g01（会话滚动记录）的关系值得单独说清：两者操作**同一批文件**。g01 的写入者持有 `WriterLockGuard`，g02 的发布者要 `try_acquire_for_publication` 同一把锁。所以「压缩与追加的并发关系」实际上是由 `WriterLockCoordinator` 这个第三方组件来仲裁的。理解这一点才能明白为什么这两页不能分开读——它们是一个并发协议的两端。

与 smolagents 的 `truncate_content`、goose 的 `elide_to_budget` 相比，那两处是「内容层面的压缩」，这里是「存储层面的压缩」。前者的目标是「让模型看到的东西变短」，后者的目标是「让磁盘占用变小」。有意思的是它们的危险点完全不同：内容压缩的危险是「语义被删坏了」，存储压缩的危险是「两个写者撞上了」。

对 AI 工程最实用的一条是「把锁缩到最后那段不可逆的替换」。很多实现会把整个耗时操作放在锁内（「我要改这个文件，所以先锁住它，然后慢慢干活」），结果长任务把使用者全挡住。正确的形状是「先在旁边把新版本做出来并校验，最后拿锁做 rename 与 unlink」。这段临界区通常只有几次系统调用。

另一条是「跳过要按原因分类统计」。这一层把跳过分成五类，于是运维能立刻回答「为什么这么多文件没压」——不够冷？有写者在忙？刚被改过？已经压过了？如果只用一个 `skipped` 计数，这些问题都答不了，而这恰恰是「后台任务看起来在工作但没效果」时最需要的信息。

最后一条是那条日志约束：「只用有界标签与已解析的 id，绝不用路径或错误消息」。会话文件路径含用户名与目录结构，错误消息可能含系统内部信息。把一个 warn 写成「跳过 ${path}」看起来很有帮助，实际是把用户目录结构写进了集中式遥测。用 `rollout_id` 这种内部标识就够了——需要定位时可以从 id 反查。
