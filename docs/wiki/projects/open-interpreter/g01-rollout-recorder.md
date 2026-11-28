---
project: open-interpreter
feature: rollout-recorder
title: 会话滚动记录
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/rollout/src/recorder.rs
    symbols: [RolloutRecorder, RolloutRecorderParams, RolloutWriterTask]
  - path: codex-rs/rollout/src/recorder.rs
    symbols: [RolloutCmd, ThreadListArchiveFilter, ThreadListRepairMode]
---

# 会话滚动记录

## 一句话职责

把一次会话的完整历史追加进一个 JSONL 文件，但**不在调用方线程上写**：所有写入被编成命令送进一个有界通道，一个后台任务独占文件句柄、串行执行，并用「延迟建文件 + 只排空已写前缀 + 失败即重开句柄重试」三条规则保证「要么写进去，要么还留在内存里可以再试」。

## 机制是什么

`RolloutRecorder` 只有三个字段：`tx: Sender<RolloutCmd>`（命令发送端）、`writer_task: Arc<RolloutWriterTask>`（后台任务的可观测状态）、`rollout_path: PathBuf`。整个结构 `#[derive(Clone)]`，克隆只复制句柄与路径——所以多个持有者写同一个文件时不需额外同步，因为它们写的是同一个通道。

`RolloutCmd` 是五种命令：`AddItems(Vec<RolloutItem>)`、`Persist { ack }`、`Flush { ack }`、`Shutdown { ack }`、`Discard { ack }`。后四种都带一个 `oneshot::Sender` 作为回执通道——所以「排队」与「确认完成」是两个分离的动作。

`RolloutRecorderParams` 是构造参数，两个变体：`Create { ... }` 与 `Resume { path }`。`Create` 变体有近二十个字段（会话 id、线程 id、fork 来源、来源类型、基础指令、动态工具、历史模式与历史基准、初始窗口 id 等），因为它要把「这个会话是怎么开始的」完整刻进 `SessionMeta`。`Resume` 只有一个路径——续跑时那些元信息已经在文件里了。

`RolloutWriterTask` 是任务的观测量，三个字段：`_writer_lock`（可选的写锁守卫）、`handle`（`Mutex<Option<JoinHandle>>`）、`terminal_failure`（`Mutex<Option<Arc<IoError>>>`）。

那个字段名带下划线不是「未使用」，注释写得很清楚：「**任务**而不是它的调用方持有锁，直到排队的文件写入完成」。所以写锁的生命周期被挂在了任务对象上，而不是 `new_inner` 的栈帧上。

## 怎么实现的

**为什么不是直接写文件。** 会话历史是**边跑边追加**的：一轮对话会产生几十条条目（消息、工具调用、工具结果、token 计数……），若每条都在调用方线程上做一次 `write` + `flush`，agent 主循环就会被磁盘延迟拖住。

所以这里用「命令 + 后台任务」的模型：调用方只做一次 `send`，真正的文件操作全部发生在 `tokio::task` 里。注释解释了通道大小与反压：「一个大小合理的**有界**通道（256）。若缓冲满了，`send` 的 future 会让出——这没问题，我们只需要确保**不在调用方线程上做阻塞 I/O**」。

注意这条 comment 的措辞：反压在这里被接受（发送方会等），但它等的是「通道有位置」而不是「磁盘写完」。这比「无界通道」正确——无界会让内存随磁盘变慢而无限增长；也比「同步写」正确——同步写会拖住主循环。

**延迟建文件（deferred creation）。** 新建会话时 `RolloutWriterState` 的初值是 `writer: None, deferred_creation: true`——**文件还不在磁盘上**。

`is_deferred()` 就一句：`writer.is_none() && deferred_creation`。于是所有写路径都先判它：

- `flush_if_materialized`：deferred 就直接返回。所以 `AddItems` 到达时若文件还没建，条目只是进 `pending_items`，不写盘。
- `flush` / `shutdown`：`deferred && pending_items.is_empty()` 才返回，否则照常写。

这个设计解决一个具体问题：`Create` 时是先分配路径、后开始对话的。如果构造时就建文件，那么「创建了会话但从没发过消息」会留下一堆空 JSONL。延迟到「真有内容要写」时才建，空会话就不留痕迹。

`ensure_writer_open` 里 `open_log_file(...)` 成功后把 `deferred_creation = false`——这是「文件已存在」的唯一标记位。

**只排空已写前缀。** `write_pending_items_once` 是这一页最需要细看的一段：

它遍历 `pending_items`，每条前先取当前 ordinal（失败就记 `Err` 并 break），然后 `write_rollout_item(item, ordinal)`，成功后 `ordinal_state.advance()` 并 `written_count += 1`。循环结束后**只 `drain(..written_count)`**——即只丢弃真正写成功的那个前缀，中间的失败条目与之后的所有条目全部留在内存里。

所以「写了一半就失败」不会丢数据：写成功的部分已落盘（且 ordinal 已推进），未写的部分仍在缓冲里等下一次重试。这个「前缀排空」是让重试安全的唯一方式——如果按「全部排空」或「按索引删」来处理，就会出现「ordinal 已推进但条目没写进去」或「条目删了但没写」的错配。

**失败即换句柄重试。** `write_pending_with_recovery(operation)` 的结构是两次尝试：

1. `write_pending_once()` 成功 → 清 `last_logged_error`，返回 `Ok`。
2. 失败 → `enter_recovery_mode(&first_err)`，打一条 warn 说「正在重开并重试」，再 `write_pending_once()`。
3. 第二次也失败 → `enter_recovery_mode(&second_err)`，打一条含**两个**错误的 warn，返回 `Err(second_err)`。

`enter_recovery_mode` 只做两件事：按错误消息去重后记一条 error，然后 `self.writer = None`。

`writer = None` 是「换句柄」的实现——下次 `ensure_writer_open` 会重新 `open_log_file`，得到一个全新的 `tokio::fs::File`。为什么要换而不是继续用原来的句柄？因为 I/O 错误的常见原因是句柄本身进入了坏状态（连接断了、文件被替换、磁盘满后的一致性不确定），继续用同一个句柄重试往往还是同一个错误。

`last_logged_error` 的去重避免了一个真实问题：一个持续失败的文件会每轮都打一条 error 把日志刷爆。它只在消息变化时才记，成功了就清空。

**命令循环的分支差异很微妙。** `rollout_writer` 的 match：

- `AddItems` → `add_items` 后 `flush_if_materialized`（不 ack，因为不需要确认）。
- `Persist` / `Flush` → 执行并把结果 ack 回去，**继续循环**。
- `Discard` → 直接 ack 空结果并 `break`（不写任何东西）。
- `Shutdown` → 执行 `shutdown()`（实质是 flush）；成功则 ack `Ok` 并 `break`，**失败则 ack `Err` 但不 break**。

最后这条是关键：shutdown 失败时任务继续活着。于是调用方可以再发一次 `Flush` 或 `Shutdown` 重试，而不是面对一个已经退出、数据永远写不出去的任务。方法注释也确认了这一点：「若排空失败，writer 保持存活，让调用方可以继续重试 flush/shutdown」。

**两种失败被严格区分。** 任务外层 spawn 的闭包里，只有 `rollout_writer` **本身返回 Err**（也就是循环退出）时，才打 error 并 `mark_failed(&err)`。注释明确：「这是后台任务的**终止性**失败路径。普通 I/O 失败留在 `rollout_writer` 内部，通过命令 ack 报告，并把条目留在缓冲里等待重试」。

于是 `terminal_failure` 只在「任务死了」时才有值。而 `record_canonical_items` / `persist` / `flush` 在 `send` 失败时（通道关闭，意味着任务已死）用 `terminal_failure` 覆盖默认错误——让调用方拿到「真正的原因」而不是一句「failed to queue」。

**读路径的宽容度。** `load_rollout_items` 有三个值得记的细节：

1. **逐行容错**：某一行 JSON 解析失败时只打 warn、`parse_errors` 计数、继续；解码失败（`decode_rollout_line`）同样。所以一个文件里几行坏掉不会让整次续跑失败。
2. **空文件是错误**：`saw_non_empty_line` 为假时返回 `"empty session file"`。所以「一个零行文件」不是「空会话」，而是「数据坏了」。
3. **首个 `SessionMeta` 才是权威**：注释写「用文件里遇到的**第一个** `SessionMeta` 作为规范线程 id 与主会话信息，保留所有条目」。后续的 `SessionMeta` 可能来自 fork 历史被复制进来的，所以 `reject_unknown_thread_history_mode` 只在「还没解析出第一个 SessionMeta」时调用——避免用一个「外来」的历史模式去拒绝整个文件。

**列表查询的两个维度。** `ThreadListArchiveFilter { Active, Archived }` 与 `ThreadListRepairMode { ScanAndRepair, StateDbOnly }` 是两枚正交的小枚举，被作为参数传进同一个 `list_threads_with_db_fallback`。

- ArchiveFilter 决定「列活的还是归档的」——它们由不同的两个公开方法分别触发。
- RepairMode 决定「要不要在扫描时修数据」：`ScanAndRepair` 会边扫边修，`StateDbOnly` 只查状态库不动文件。

两个都是 `#[derive(Clone, Copy)]` 的不可携带数据的标签。用枚举而不是 bool，正是为了让调用点自解释——`list_threads_with_db_fallback(..., ThreadListArchiveFilter::Active, ThreadListRepairMode::ScanAndRepair, ...)` 一眼能读出来，而 `(false, true)` 不能。

**`append_rollout_item_to_path` 是受限的旁路。** 它给「已卸载线程的元信息更新」提供一个直接追加的入口，注释明确要求「活跃会话应当用 `record_canonical_items`，好让 rollout 写入与会话流其余部分保持顺序」。

两条路径的差别在时序保证：走命令的路径所有写入被同一个任务串行化，所以文件里的顺序与产生顺序一致；旁路直接追加，若同时有活跃写入者，就可能插到别的位置。注释把这个约束写出来了，而不是靠调用方自觉。

**ordinal 是写入位置的单调计数。** `RolloutOrdinalState` 每次成功写入后 `advance()`，写入前取 `current()`。`for_new_rollout(history_mode, history_base)` 说明它可以从某个历史基准开始计数——这对 fork/续跑是必要的：新文件的 ordinal 要接上被 fork 的那一段。而 `folded_from_ordinal_exclusive` 这个字段名里带「exclusive」也印证了「ordinal 是区间端点」的语义。

## 关键取舍

**源码事实：** 通道是有界 256 的，注释明确写「缓冲满时 send 会让出」，且目标是「不在调用方线程上做阻塞 I/O」。所以反压被接受，阻塞 I/O 不被接受。

**源码事实：** 新建会话时 `deferred_creation: true`、`writer: None`——文件直到第一次真要写东西时才创建。空会话不留文件。

**源码事实：** `write_pending_items_once` 只 `drain(..written_count)`，即只丢弃已写成功的前缀。

**源码事实：** 失败时 `enter_recovery_mode` 把 `writer` 置空（丢弃句柄），下次写会重开文件再重试；同一错误消息只记一次日志。

**源码事实：** `Shutdown` 失败时不 `break`，任务继续存活以便重试。

**源码事实：** `terminal_failure` 只在后台任务**退出**时被写入；普通 I/O 失败通过命令 ack 报告。

**源码事实：** `load_rollout_items` 逐行容错、空文件报错、首个 `SessionMeta` 才是权威。

**架构推论：** 「命令 + 后台任务」这个模型的价值不在并发，而在**把磁盘延迟从关键路径上摘掉**，同时保住顺序。若改成「每写一条起一个任务」，顺序就丢了；若改成「调用方直接写」，延迟就进了主循环。单任务串行队列是唯一同时满足两者的结构。

**架构推论：** 「延迟建文件 + 前缀排空 + 换句柄重试」三条规则合起来，才能让「会话历史」这种**不可重建**的数据具备可重试性。普通日志丢了就丢了；但会话历史丢了，用户就没法续跑。所以这里所有失败路径都必须保留数据在内存里，且所有成功路径都必须精确记账（ordinal + drain count）。

**架构推论：** 两种失败被分成「任务死了」与「这次写失败」两类，是很值得借鉴的分层。前者不可恢复（通道关闭），后者可恢复（重试）。把它们混成一个 `Result` 会让调用方无法决定「该不该重试」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/rollout/src/recorder.rs` | `RolloutRecorder` | 命令发送端、任务句柄与路径的持有者，可 Clone |
| `codex-rs/rollout/src/recorder.rs` | `RolloutRecorderParams` | Create / Resume 两变体的构造参数 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutCmd` | 五种写入命令，后四种带回执 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutWriterTask` | 写锁、任务句柄与终止性失败的观测点 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutRecorder.record_canonical_items` | 入队条目，不阻塞调用方线程 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutRecorder.persist` | 物化文件并落盘缓冲，幂等 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutRecorder.flush` | 等所有排队写入提交 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutRecorder.shutdown` | 排空后停任务，失败则任务保活 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutRecorder.discard` | 不物化延迟条目直接停任务 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutWriterState.write_pending_items_once` | 逐条写入并只排空已写前缀 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutWriterState.write_pending_with_recovery` | 失败换句柄重试一次 |
| `codex-rs/rollout/src/recorder.rs` | `rollout_writer` | 命令循环，含 Shutdown 失败不退出 |
| `codex-rs/rollout/src/recorder.rs` | `load_rollout_items` | 逐行容错的读取与首个元信息定权威 |
| `codex-rs/rollout/src/recorder.rs` | `append_rollout_item_to_path` | 已卸载线程的受限旁路追加 |
| `codex-rs/rollout/src/recorder.rs` | `ThreadListArchiveFilter` | 活跃 / 归档的选择标签 |
| `codex-rs/rollout/src/recorder.rs` | `ThreadListRepairMode` | 边扫边修 / 只查状态库的选择标签 |

## 参考

- [open-interpreter 源码：rollout/recorder.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/rollout/src/recorder.rs)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [open-interpreter 文档](https://www.openinterpreter.com/docs)

## 与主核 / AI 的关系

主核里最接近的是「WAL 写入路径」：事务提交不能等每一页都刷到盘上，所以先顺序追加日志再异步落地，且必须精确记账到哪个位置已经持久化。这里的 `ordinal_state` 加「前缀排空」与 WAL 的 LSN 加「已落盘位置」是同一个结构——**写入位置必须单调且精确，否则重试会重写或跳过**。

与 goose 的 `RolloutRecorder`（goose 的 g03 会话追踪）对照很有意思：那边追踪的是「服务端已持有哪些项」，用对象身份/服务端 id/内容指纹三套账本；这边追踪的是「文件里写到了哪个位置」，只有一个单调 ordinal。差别在于目标：那边要去重（因为远端可能重复接受），这边只要顺序与完整性（因为文件是自己写的）。

与 smolagents 的记忆层相比，`MemoryStep` 在内存里累积、按需 `dict()` 序列化；这里没有内存模型，直接追加进文件。前者适合短命进程与小历史，后者适合「几小时的长会话、要能崩溃后恢复」。

对 AI 工程最实用的一条是「延迟建文件」。任何「会话开始时分配资源」的设计都会遇到「创建了但没用」的问题——空的 session 目录、空的日志文件、空的数据库行。把资源创建推迟到第一次真正写入，是消除这类垃圾最省事的办法，代价是所有写路径都必须理解「可能还没建」这个状态（这里用 `is_deferred()` 显式承担了）。

另一条是「失败后换句柄而不是继续用」。文件 I/O 失败后，原句柄的状态往往不可信；`writer = None` 再重开是一个成本极低、效果确定的恢复动作。比它更常见但更糟的做法是「原地重试」，因为那会把一个可能是永久性的错误变成一个繁忙的重试循环，且日志刷屏——所以还要配一个「同一错误只记一次」的去重。

最后一条是 `Shutdown` 失败不退出任务。这个设计允许「关不干净就再关一次」，而不会让调用方陷入「数据没写完但任务已死」的绝境。任何「收尾时必须落盘」的组件都该照这个形状实现。
