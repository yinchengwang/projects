---
project: goose
feature: scheduler
title: 调度器与定时配方
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/scheduler/full.rs
    symbols: [Scheduler, RunningTasksMap, JobsMap]
  - path: crates/goose/src/scheduler/full.rs
    symbols: [read_validated_schedule_recipe, clear_running_state]
---

# 调度器与定时配方

## 一句话职责

把「按 cron 定时跑一份配方」做成一个可持久化、可暂停、可手动触发、可中途取消的作业列表；并用两张分离的表解决「哪些作业在跑」与「作业怎么跑」这两个生命周期完全不同的问题。

## 机制是什么

三个类型定义了一切：

```rust
type RunningTasksMap = HashMap<String, CancellationToken>;
type JobsMap = HashMap<String, (JobId, ScheduledJob)>;
```

以及

```rust
pub struct Scheduler {
    tokio_scheduler: TokioJobScheduler,
    jobs: Arc<Mutex<JobsMap>>,
    storage_path: PathBuf,
    running_tasks: Arc<Mutex<RunningTasksMap>>,
    session_manager: Arc<SessionManager>,
    services: ServicesCell,
}
```

所以「作业」有两份状态，各自一个 map：`JobsMap` 是**持久状态**（作业定义 + cron 引擎句柄），`RunningTasksMap` 是**易失状态**（一个取消令牌）。

`ServicesCell` 是 `Arc<SetOnce<Weak<StateMachineServices>>>`，注释一句话说明了为什么要弱引用：「弱引用是因为 services 持有 scheduler」。这是一个清晰的环：services 需要调度器来跑定时任务，调度器需要 services 才能执行任务。用 `Weak` 打断。

## 怎么实现的

**为什么两张 map 要分开。** 这是这一页最值得讲清的一点。

`JobsMap` 里的值要落盘（`persist_jobs` 把每个 `ScheduledJob` 序列化进 JSON），且每次状态变化（开始跑、跑完、暂停、改 cron）都要写一次文件。`RunningTasksMap` 里的值是 `CancellationToken`——一个纯内存的同步原语，**不可序列化也不需要持久**。

若合成一张表，会有三个具体后果：

1. 每次要读作业定义都得先拿能触及取消令牌的锁，反之亦然——两个生命周期完全不相关的状态被迫共享一把锁。
2. 落盘时不得不把令牌字段排除掉，于是序列化层要理解「哪些字段是易失的」。
3. 组织上无法独立回答「现在在跑什么」——那个查询只需要读 `RunningTasksMap`，而它与作业定义无关。

更重要的是语义差别：`JobsMap` 里一个作业**永远存在**（直到被删除），`RunningTasksMap` 里一条记录**只在执行期间存在**。把这两种「存在性语义」塞进一张表，会让 `get` 的返回类型变成「可能有一半字段」的怪物。

**cron 表达式的 5/6 字段兼容。** `create_cron_task` 先按空白切分 `job.cron`：

- 5 段 → 打一条 warning「这个作业有旧式 5 段 cron，正在转成 6 段」，然后前面补 `"0 "`。
- 6 段 → 直接用。
- 其他 → `CronParseError`，消息里带上实际段数。

6 段是 `秒 分 时 日 月 周` 的形态（`tokio-cron-scheduler` 要求秒级）。5 段是传统 cron。补 `"0 "` 就是「每秒的第 0 秒」，语义等价于传统 cron 的「每分钟那一瞬间」。这条兼容逻辑让旧作业在升级后继续工作，代价是日志里会留下一条 warning。

时区用 `Local::now().timezone()`——**按本地时区解释 cron**。这对「用户写 `0 9 * * *` 意思是他自己的早上 9 点」是正确选择，代价是同一份作业配置在不同时区的机器上触发时刻不同。

**每次触发都重新查一遍作业。** 触发闭包里做的第一件事是拿 `current_jobs_arc` 的锁，`get(&task_job_id)` 后判 `!j.paused`。

为什么不直接用闭包捕获的 `job_for_task` 判暂停？因为闭包是在**创建**时捕获的，那份克隆看不到后续的暂停操作。所以「是否该执行」必须读当前状态，而「怎么执行」才用捕获的克隆。

这带来的空洞是：若作业在创建后被修改（改了 cron 之外的字段），捕获的克隆仍是旧的。所以 `update_schedule` 必须重建 cron 任务（它确实这么做了：先 `remove(&old_uuid)` 再 `add(cron_task)`）。

**执行前后的三段落盘。** 触发路径上的状态变化分三段写盘：

1. 置 `last_run` / `currently_running = true` / `process_start_time` 后写一次。
2. 执行完，清三个字段后写一次。
3. 中间那次 `running_tasks.insert` **不写盘**（因为令牌不是持久状态）。

所以「作业正在跑」这个事实在磁盘上是可见的——这直接引出下一段的启动恢复问题。

**崩溃后的 running 状态清理。** `clear_running_state(job)` 的逻辑很细：先算 `changed = currently_running || current_session_id.is_some() || process_start_time.is_some()`，然后三个字段一律清空，返回 `changed`。

注意它判断的是**三个字段中任意一个非空**，而不只是 `currently_running`。所以「上次崩在某个中间状态（比如已写了 session_id 但没写 running）」也能被识别并清掉。

调用点在两处：

- `load_jobs_from_storage` 里对每个加载的作业都调一次，并用 `|=` 累积 `reset_stale_running_state`；只要有一个变了就整体重新写盘。注释没写理由，但行为很清楚：**启动时看到「正在跑」的作业，说明上次进程没正常退出**。因为正常退出会清掉这些字段。
- `kill_running_job` 里，取消令牌之后也调一次，把状态立即清干净（不等执行路径收尾）。

所以这个函数承担两个职责：启动恢复 + 主动取消后的状态清理。两者都需要「把三个字段当一组清空」。

**加载时的宽容。** `load_jobs_from_storage` 的每一步失败都只记 error 然后 `return`：

- 文件不存在 → 直接返回（空列表）。
- 读文件失败 → 记日志，`Starting with empty schedule list.`
- 内容空白 → 直接返回。
- JSON 解析失败 → 同上。

所以「调度配置文件坏了」不会让进程起不来，代价是**所有作业静默消失**——日志里有一条 error，但用户不主动查是看不到的。这是一个明显偏向可用性的取舍。

**配方文件的读取有三层防护。** `read_validated_schedule_recipe(source_path)` 依次做：

1. `open_regular_schedule_recipe`——打开文件。这个 helper 在 `common.rs`，名字里的 `regular` 暗示它会拒绝非普通文件（符号链接、设备文件之类）。
2. 检查 `metadata.len() > MAX_SCHEDULE_RECIPE_BYTES`（1 MB）→ 报错。
3. 用 `.take(MAX_SCHEDULE_RECIPE_BYTES + 1).read_to_end(...)` 读，然后**再检查一次**长度。

第 2 与第 3 步都在检查同一件事，这是必要的：`metadata.len()` 可被竞态绕过（文件在检查与读取之间变大），所以读取时用 `take` 限流并在事后复查。`+ 1` 是关键——若只 take 上限值，读满就分不清「恰好等于上限」与「被截断」，多读一个字节才能区分。

4. `std::str::from_utf8` 校验 UTF-8，失败时映射成 `SchedulerRecipeError::GenericParse(format)`——注意这里把格式信息带上了（`format` 是从扩展名推出来的）。
5. `validate_recipe_for_scheduling(content, recipe_dir, format)`——真正的语义校验，把父目录也传进去（配方里可能有相对路径引用）。

返回的是**原始字节**而不是解析后的结构。所以「读取」与「解析」是分开的两步：这里只保证字节安全可用，解析发生在后面。因为同一份字节既被校验又要被存档（`write_schedule_recipe_bytes`），只传字节避免重复读文件。

**配方的存档与路径重写。** `add_scheduled_job_with_recipe` 里有一段路径迁移逻辑：把用户给的配方文件**复制**到调度器自己管理的 `scheduled_recipes/<job_id>.<ext>`，然后把作业的 `source` 改写成新路径，同时把原目录记进 `recipe_base_dir`。

这解决的是一个实际问题：用户给的配方可能在任意位置，若直接引用，用户移动或删除那个文件就会让定时任务失效。复制一份到托管目录，让作业自持。而 `recipe_base_dir` 保留原目录，是为了让配方里的相对路径引用仍能解析。

同时清掉 `current_session_id` 与 `process_start_time`——新作业不该带着前任的运行残留。

**`persist_jobs` 用整个 map 全量写。** 它拿锁、把所有作业收集成 `Vec`、`create_dir_all` 父目录、`to_string_pretty` 序列化、`fs::write` 覆盖。

所以每次状态变化都是「全量重写整个文件」。对几十个作业的规模，这个代价与「增量更新」的复杂度相比是划算的。但它有一个明确的正确性弱点：**并发写**。虽然有 `Arc<Mutex<JobsMap>>` 保证内存一致，但两次写盘之间的进程崩溃会让文件停留在中间版本——不过因为写的是 pretty JSON 且是覆盖写，最坏情况是文件被截断，那会被下一次加载解析失败并按「空列表」处理。

**`run_now` 与 cron 触发的差异。** `run_now` 是自己起一次执行，不进 cron 引擎；它与触发路径共享 `execute_job`，但有几处不同：

- 它先判 `job.currently_running` 并**报错**（`"Job '{}' is already running"`），而触发路径是静默跳过（判 `paused` 后 `return`）。
- 它先置 `last_run` 在**结束时**（`last_run = Some(Utc::now())` 放在清理块里），而触发路径在**开始时**就置。
- 取消时返回错误而不是成功：`_ if was_cancelled => Err(... "Job '{}' was successfully cancelled")`。

最后这条很值得注意：**取消被表达成一个错误**。消息里写「successfully cancelled」，但类型是 `Err`。所以调用方无法用「成功/失败」区分「跑完了」与「被取消了」——只能看错误消息文本。

**暂停与更新的前置检查。** `pause_schedule` 与 `update_schedule` 都先判 `currently_running`，正在跑就报错。这是必要的：暂停一个正在跑的作业，它的执行会继续到下一次触发才生效，用户会以为「暂停了但还在跑」。所以直接拒绝。

`update_schedule` 还多一条：`if new_cron == job.cron { return Ok(()) }`——cron 没变就直接返回，不重建任务。这避免了「改个没什么变化的 cron 却把句柄换掉」的无谓工作。

**改 cron 是「先改内存、再换句柄」。** 顺序是：在锁内 `job.cron = new_cron` 并创建新 cron 任务；释放锁；`remove(&old_uuid)`；`add(new_cron_task)` 拿 `new_uuid`；再拿锁把 map 里的 uuid 更新。

这里有一个窗口：从 `job.cron` 被改到新句柄装进去之间，作业的 cron 字段与引擎里的实际触发器不一致。但因为 `currently_running` 已被检查为假，且这个窗口在单进程内极短，实践中可接受。

**`get_running_job_info` 的两个 `None`。** 它返回 `Result<Option<(String, DateTime<Utc>)>>`，语义分三层：

- 作业不存在 → `Err(JobNotFound)`。
- 作业存在但没在跑 → `Ok(None)`。
- 在跑 → 取 `(current_session_id, process_start_time)`，**两个都有**才返回 `Some`，否则 `Ok(None)`。

第三个分支的 `_ => Ok(None)` 说明「在跑但缺少 session_id 或 start_time」被视为「没有可报告的信息」。所以返回类型把「不存在」「没在跑」「在跑但信息不全」压成两种：错误与非错误。调用方拿不到「在跑但信息不全」这个状态——这正是 `clear_running_state` 要清三个字段的原因：尽量让这三个字段同时存在或同时不存在。

## 关键取舍

**源码事实：** 作业状态分两张表：`JobsMap`（可序列化，随作业长期存在）与 `RunningTasksMap`（不可序列化的取消令牌，只在执行期间存在）。

**源码事实：** cron 按**本地时区**解释；5 段表达式会被补 `"0 "` 转成 6 段并记 warning，其余段数报错。

**源码事实：** 触发时重新读当前状态判 `paused`，但执行用的是创建时捕获的作业克隆。所以改字段必须重建 cron 任务。

**源码事实：** `clear_running_state` 判三个字段中**任意一个**非空即视为脏状态，然后三字段一起清空。

**源码事实：** 配方读取先查 `metadata.len()`，再用 `take(上限 + 1)` 读后复查——`+ 1` 是为了区分「恰好到上限」与「被截断」。

**源码事实：** `Sse` 之类不支持的东西在配置转换层被显式拒绝（见 i02），而调度配置层选择宽容——文件坏了就当空列表。

**源码事实：** `run_now` 在被取消时返回 `Err`，消息文本是「successfully cancelled」。

**架构推论：** 「持久状态与易失状态分表」这条原则可以直接推广。任何「一部分要落盘、一部分只是运行时句柄」的聚合，都应该拆成两个容器：一个可序列化，一个不可。合在一起会导致序列化层必须知道哪些字段是易失的，而那是它最不该知道的事。

**架构推论：** 启动时清 `currently_running` 是一个「进程崩溃后的状态修复」模式：把「正在执行」这个标志当作**进程本地的**事实，而不是跨进程的持久事实。它之所以要持久化，只是为了在崩溃后能被检测并清除——不是为了在下次进程里继续「认为自己还在跑」。理解这一点才能解释为什么清理逻辑放在加载路径上而不是执行路径上。

**架构推论：** 配方文件「先复制到自己目录、再改写 source 并记 base_dir」这套做法，解决的是「外部文件的生命周期不受控」。任何引用外部资源的持久作业都需要这个形态：要么复制自持，要么每次执行前校验存在性。这里两者都做了（复制了，且 `add_scheduled_job` 里会检查 `original_recipe_path.is_file()`）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/scheduler/full.rs` | `Scheduler` | 调度器主体：cron 引擎、作业表与运行表 |
| `crates/goose/src/scheduler/full.rs` | `RunningTasksMap` | 作业 id 到取消令牌的易失表 |
| `crates/goose/src/scheduler/full.rs` | `JobsMap` | 作业 id 到「引擎句柄 + 作业定义」的持久表 |
| `crates/goose/src/scheduler/full.rs` | `read_validated_schedule_recipe` | 三层防护的配方读取与语义校验 |
| `crates/goose/src/scheduler/full.rs` | `clear_running_state` | 三字段一起清空的脏状态修复 |
| `crates/goose/src/scheduler/full.rs` | `Scheduler.create_cron_task` | cron 兼容转换与触发闭包 |
| `crates/goose/src/scheduler/full.rs` | `Scheduler.load_jobs_from_storage` | 宽容加载并清零残留运行状态 |
| `crates/goose/src/scheduler/full.rs` | `Scheduler.persist_jobs` | 全量写盘的持久化 |
| `crates/goose/src/scheduler/full.rs` | `Scheduler.run_now` | 手动触发，取消以错误形式返回 |
| `crates/goose/src/scheduler/full.rs` | `Scheduler.kill_running_job` | 取消令牌并立即清状态 |
| `crates/goose/src/scheduler/full.rs` | `Scheduler.update_schedule` | cron 未变则跳过，变了则换句柄 |
| `crates/goose/src/scheduler/full.rs` | `Scheduler.get_running_job_info` | 三层返回语义的运行信息查询 |

## 参考

- [goose 源码：scheduler/full.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/scheduler/full.rs)
- [goose 文档：定时配方](https://block.github.io/goose/docs/guides/recipes/scheduled-recipes/)
- [tokio-cron-scheduler 文档](https://docs.rs/tokio-cron-scheduler/)

## 与主核 / AI 的关系

主核里最接近的是「后台维护任务的调度」：定时做 checkpoint、统计刷新、日志轮转。它们同样需要三个东西——持久化的任务定义、易失的运行句柄、以及崩溃后的状态修复。这一页的三张表结构正对应这三件事，连「启动时把残留的运行标志清掉」这个细节都一样：主核重启后若看到某个 checkpoint 标记为「进行中」，也只能假定上次崩溃并重做。

与 smolagents 的 `Monitor` 相比，两者的「状态容器」设计取向正好相反。`Monitor` 把耗时列表与 token 两个计数放在同一个对象里，而且**不持久化**——因为它只是给当前进程看的统计。`Scheduler` 则必须把「作业定义」持久化、把「取消令牌」留在内存，于是被迫分表。差别来自「状态是否需要跨进程存活」这个根本问题。

对 AI 工程最实用的一条是「持久状态与易失状态必须分容器」。这个原则听起来像常识，但违反它的成本很隐蔽：一旦混在一张表里，`serde` 就得跳过某些字段，而每个跳过点都是一个潜在的「恢复后字段缺失」bug。分开之后，序列化器只看得到它该看到的东西。

另一条是 `clear_running_state` 那种「把一组字段当整体处理」的写法。它判断的是三个字段里**任意**一个非空，清的时候三个一起清。这比「逐个字段各自判断与清理」更安全——后者会留下「清了 A 但漏了 B」的中间状态，而那种状态在查询时表现为「在跑但信息不全」，正是 `get_running_job_info` 里那个 `_ => Ok(None)` 分支要吞掉的情况。相关字段成组定义、成组清理，是让状态机保持可枚举的简单办法。
