---
project: open-interpreter
feature: file-search
title: 文件搜索与索引
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/file-search/src/lib.rs
    symbols: [FileSearchSession, FileSearchResults, FileMatch]
  - path: codex-rs/file-search/src/lib.rs
    symbols: [FileSearchOptions, IndexedEntry, MatchType]
---

# 文件搜索与索引

## 一句话职责

把「模糊找文件」拆成一条流水线：一个线程池遍历目录并把条目灌进 nucleo，另一个线程只负责重算计分与取前 N 条；查询更新只是「改模式并重新计分」，不重新走盘——所以边打边搜是便宜的。

## 机制是什么

`FileMatch` 五个字段：`score: u32`（nucleo 给的分数）、`path: PathBuf`（相对搜索目录）、`match_type: MatchType`、`root: PathBuf`、`indices: Option<Vec<u32>>`（匹配到的字符下标，仅在 `compute_indices` 开启时有）。它的 doc 把 `indices` 的性质写得很细：「唯一且升序，调用方可直接用于高亮」——这个保证来自 nucleo 的 `Pattern::indices`。

`MatchType` 两值 `File` / `Directory`，带 `#[serde(rename_all = "lowercase")]` 所以线上是 `"file"` / `"directory"`。

`IndexedEntry` 是内部索引项，两字段：`full_path: Arc<str>` 与 `match_type: MatchType`。注释点明了它的用途：「携带 walker 观察到的条目类型，这样匹配到的路径不必重新 stat」。用 `Arc<str>` 而不是 `String` 是为了让「同一个路径字符串」在索引里可共享。

`FileSearchOptions` 五字段：`limit: NonZero<usize>`（默认 20）、`exclude: Vec<String>`、`threads: NonZero<usize>`（默认 2）、`compute_indices`（默认 false）、`respect_gitignore`（默认 true）。

`FileSearchSession` 只有 `inner: Arc<SessionInner>` 一个字段——公开的会话句柄，实际状态都在内层。

## 怎么实现的

**两个 worker，一个共享索引。** `create_session` 起两路：`walker_worker` 用 `ignore` crate 的并行遍历把条目录灌进 nucleo 的 `Injector<IndexedEntry>`；`matcher_worker` 独占 nucleo 实例并响应信号。

两者通过三样东西耦合：`Injector`（单向灌入）、nucleo 内部的 `NucleoNotify` 回调（灌入后通知 matcher）、以及 `work_tx`/`work_rx` 这个 `WorkSignal` 通道。

**`WorkSignal` 四值各有分工。** `QueryUpdated(String)` 来自 `update_query`；`NucleoNotify` 来自 injector 的通知回调；`WalkComplete` 由 walker 在结束后自己发；`Shutdown` 由 `Drop` 发。

`FileSearchSession::update_query` 的实现只有一行 `work_tx.send(WorkSignal::QueryUpdated(...))`，并且注释强调：「更新查询相对于重新遍历应当很便宜」。这一行就是那个承诺的实现——查询变化只是往通道塞一个信号，不等、不查盘、不阻塞调用方。

**`Drop` 是关停协议。** 实现是置 `shutdown` 标志并 `send(WorkSignal::Shutdown)`。两个动作都必要：标志让正在跑的 walker 尽快退出（它在每 1024 条检查一次），信号让 matcher 跳出主循环。

注意 `send` 的结果被 `let _ =` 丢弃——若 matcher 已经退出（通道关闭），发送失败也无所谓。而 `shutdown` 这个 `AtomicBool` 是唯一对 walker 有效的停止手段。

**查询更新是一次「重解析」而不是重建。** `matcher_worker` 收到 `QueryUpdated(query)` 后做的是：

```rust
let append = query.starts_with(&last_query);
nucleo.pattern.reparse(0, &query, CaseMatching::Ignore, Normalization::Smart, append);
last_query = query;
will_notify = true;
next_notify = after(Duration::from_millis(0));
```

`append` 这个参数是这一层的性能关键：若新查询是旧查询的**前缀扩展**（用户又打了一个字符），`reparse` 可以增量地细化模式，而不必对全部条目重算。这个判断只有一行 `query.starts_with(&last_query)`。

`next_notify = after(Duration::from_millis(0))` ——立即安排一次 tick。所以「打一个字」到「看到结果」的延迟主要来自一次 `tick` 而不是防抖窗口。

**三种触发都用同一个通知槽位。** `will_notify` 与 `next_notify` 是两个变量，用来合并通知：

- `QueryUpdated`：立即（0ms）。
- `NucleoNotify`：若未安排，安排 10ms 后。
- `WalkComplete`：若未安排，立即；同时置 `walk_complete = true`。

所以「injector 灌入新条目」的通知被**防抖到 10ms**，而「查询变了」与「遍历结束」都是立即。这个不对称是对的：遍历是持续的背景活动，按每批都通知会让 tick 频率被 I/O 速度绑架；而查询与结束是低频且用户可感知的事件。

`will_notify` 的检查是「若已安排就不再安排」——所以多个 `NucleoNotify` 在 10ms 内合并成一次 tick。

**`tick` 之后只在 `status.changed` 时上报。** 主循环的 tick 分支：

```rust
let status = nucleo.tick(TICK_TIMEOUT_MS);
if status.changed { /* 构造 snapshot，调 reporter.on_update */ }
if !status.running && walk_complete { inner.reporter.on_complete(); }
```

所以「没变化就不上报」——避免在遍历还在跑但计分结果没变时反复推同样的列表。而 `on_complete` 的判据是 `!status.running && walk_complete`，即「matcher 认为没有更多工作」且「遍历已经结束」两个条件。

**前 N 条的构造里有三个需要留意的点。** 在 `status.changed` 分支里：

1. `let limit = inner.limit.min(snapshot.matched_item_count() as usize);` —— 上限与总匹配数取较小者。
2. `.take(limit)` 之后是 `filter_map`，其中 `snapshot.get_item(match_.idx)?` 用 `?` 跳过取不到项的情况。
3. 每个匹配都要重新 `get_file_path(Path::new(full_path), &inner.search_directories)?` 算相对路径与 root 下标。

第 3 点看起来是重复劳动——walker 已经算过一次相对路径并把它放进了 nucleo 的 `matcher_columns[0]`。为什么不复用？因为 `FileMatch.path` 要求的是「相对**搜索目录**的路径」而 `matcher_columns` 里存的就是那个相对路径（walker 的 `|_, cols| { cols[0] = Utf32String::from(relative_path); }`）。

实际上代码确实重算了。这样做的好处是「匹配结果是从 `full_path` 真值出发计算的」，而不是依赖一次更早的、可能不再一致的缓存。代价是 N 次路径操作——而 N 有上限（默认 20），所以这个成本可以忽略。

`indices` 的计算同理会为每个匹配新建一个 `Vec<u32>`、排序、去重。`indices_matcher` 只在 `compute_indices` 为真时存在（`inner.compute_indices.then(|| Matcher::new(config.clone()))`），所以默认路径下这一整块被跳过。

那个 `.sort_unstable(); .dedup();` 是为了兑现 `FileMatch.indices` doc 里「唯一且升序」的承诺——即使 nucleo 已经保证，这里再排一次是廉价的冗余。

**snapshot 里的四个统计字段。** `FileSearchSnapshot` 有 `query`、`matches`、`total_match_count`、`scanned_file_count`、`walk_complete`。

`total_match_count` 来自 `snapshot.matched_item_count()`，`scanned_file_count` 来自 `snapshot.item_count()`。前者是「匹配了多少条」，后者是「索引里一共多少条」。两个都报出去，让调用方能显示「匹配 X / 扫描 Y」——这解释了为什么 `scanned_file_count` 值得占一个字段。

`query: last_query.clone()` 用的是 matcher 侧记住的 `last_query` 而不是 `snapshot.pattern()` 反解出的文本。所以上报的查询串与调用方给的完全一致，不做规范化。

**`walker_worker` 的四个构建参数都带理由。** `WalkBuilder` 的配置：

- `.threads(inner.threads)` —— 默认 2。
- `.hidden(false)` —— 注释：「允许隐藏条目」。所以 `.git`、`.env` 这些也会被索引（后续由 ignore 规则过滤）。
- `.follow_links(true)` —— 「跟随符号链接以搜索它们的内容」。
- `.require_git(true)` —— 这是最需要解释的一个。

源码给它写了一段很长的注释，值得完整对待：「walker 用 `require_git(true)` 以匹配 git 自己的忽略语义：git 从不读取仓库根之上的目录里的 `.gitignore` 文件。没有这个标志，`ignore` crate 会从**所有**祖先目录读取 `.gitignore`——这是相对 git 的刻意偏离，为非 git 用例准备的——从而让一个宽泛的父级忽略规则（例如 `~/.gitignore` 里含 `*`）静默地抑制遍历中的每一个文件」。

所以这一行是在防一个具体的灾难：用户家里有个 `~/.gitignore`，里面写了 `*`（也许是为了某个别的用途）。若不加 `require_git`，那这个规则会一路生效到当前项目，导致**搜索永远返回零结果**，而用户完全不知道原因。

这个 bug 的形态很有教育意义：一个「父目录里的配置」影响了「一个看起来无关的目录的行为」，且症状是「功能完全不工作」而不是「行为略有偏差」。

`respect_gitignore == false` 时的五个 `.xxx(false)` 调用也是同一条注释的一部分：关闭 `git_ignore`、`git_global`、`git_exclude`、`ignore`、`parents`——即**所有**忽略处理。所以「不尊重 gitignore」是一个全面的关闭，不是只关 `.gitignore` 一项。

**`exclude` 用 override 反向模式。** `build_override_matcher(search_directory, exclude)` 把每个 exclude 项拼成 `format!("!{exclude}")` 再塞进 `OverrideBuilder`。

`!` 前缀在 `ignore` crate 的 override 语义里表示「排除」。所以 `exclude: ["*.log"]` 会变成「排除所有 `*.log`」的规则集。`exclude.is_empty()` 时返回 `None`（不设 override），省掉一个匹配器。

**`get_file_path` 处理多搜索目录的嵌套。** 给定一个路径与一组搜索目录，它要找「哪个根是这个路径的前缀」，且在多个都匹配时取**最深**的那个。

实现的比较是 `search_directories[best_idx].components().count() >= root_depth` ——若当前最佳根的组件数**不少于**新候选的深度，就不换。所以只有严格更深的候选才会取代。

为什么要「取最深」？因为多个搜索目录可能相互嵌套（例如 `/a` 与 `/a/b`）。一个在 `/a/b` 下的文件同时以两个根为前缀；相对 `/a` 是 `b/x.txt`，相对 `/a/b` 是 `x.txt`。后者更符合「在我选择的那个目录里找」的直觉，也让 `FileMatch.root` 指向更精确的来源。

这个 `best_match` 的更新逻辑里有个细节：初始 `None` 时直接赋值（`_ => { best_match = Some(...) }`），所以第一轮就把第一个匹配的作为候选，之后才做深度比较。而 `>=` 让「同深度」不替换——保持先到的那个。

`rel_path.to_str()` 最后一步用 `Option` ——非 UTF-8 路径直接丢弃。所以这个搜索不支持非 UTF-8 文件名。这是一个明确的限制（`full_path` 那边同样 `path.to_str()` 也丢弃），原因是索引项里存的是 `Arc<str>`。

**`walker_worker` 每 1024 条检查一次取消。** 闭包里用 `const CHECK_INTERVAL: usize = 1024` 与计数器 `n`，达到间隔时检查 `cancelled` 与 `shutdown`，任一为真就返回 `ignore::WalkState::Quit`。

用计数而不是时间：遍历是 CPU/IO 密集的，按条数检查能保证开销比例恒定（每 1024 条一次原子读）。若是按时间检查，就要在每条上读时钟，成本高得多。

`WalkState::Quit` 是 `ignore` crate 提供的「停止整个遍历」的返回值——所以取消能真的停下来，而不是只跳过后续处理。

**错误一律 `Continue`。** 闭包里 `Err(_) => return WalkState::Continue`，以及 `path.to_str()` 为 `None` 时也 `Continue`。所以「某个条目读不了」不会中断遍历。这是一个必要选择：遍历中必然会遇到权限不足的目录、正在被删除的文件，把这些当成致命错误会让搜索几乎总是失败。

而 `injector.push(...)` 的第二个参数是一个闭包 `|_, cols| { cols[0] = Utf32String::from(relative_path); }`——它把相对路径写进 nucleo 的第 0 号匹配列。所以「匹配是在相对路径上做的」，而不是在全路径上。这符合直觉：用户搜的是文件名而不是 `home/user/projects/...`。

**`matcher_worker` 的 `default(100ms)` 分支。** `select!` 的最后一路是 `default(Duration::from_millis(100)) => { /* 偶尔检查取消标志 */ }`。

这个空分支的作用是让循环有一个最大等待时间，从而周期性地检查 `cancel_requested()` 与 `shutdown_requested()`。若没有它，一个「既无信号也无通知」的长时间段会让循环永久阻塞在 select 上，取消失效。

而循环末尾的 `if cancel_requested() || shutdown_requested() { break; }` 是统一的退出点——在每条路径（收到信号、tick 完成、超时）之后都检查一次。

退出循环后还有一行 `inner.reporter.on_complete();`——注释说「如果我们取消了或以其他方式退出了循环，要确保 reporter 被通知」。这与 `SessionReporter::on_complete` 的 doc 呼应：「在会话变为空闲或被取消时调用。**保证每次 update_query 至少调用一次**」。

所以 `on_complete` 的契约是「至少一次」，而实现里的两条路径（`!status.running && walk_complete` 与退出循环）都可能调用它——可能调两次。这在契约允许范围内，因为 doc 说的是「至少一次」。

## 关键取舍

**源码事实：** 查询更新只做 `reparse` 加「安排一次立即 tick」，不重走盘。`append` 判据是 `query.starts_with(&last_query)`，用于增量细化。

**源码事实：** 灌入通知防抖 10ms，查询更新与遍历结束都是立即。

**源码事实：** `require_git(true)` 防的是「祖先目录的宽泛 ignore 规则静默抑制一切」。

**源码事实：** `respect_gitignore == false` 关闭全部五项忽略处理（含 `parents`）。

**源码事实：** 每 1024 条检查一次取消，用 `WalkState::Quit` 真停。

**源码事实：** 条目错误与不可 UTF-8 路径都 `Continue`，不中断遍历。

**源码事实：** `get_file_path` 在多个根都匹配时取**最深**的那个根。

**源码事实：** `indices` 在回报前再 `sort_unstable` 与 `dedup` 一次，兑现「唯一且升序」。

**源码事实：** 主循环用 `default(100ms)` 保证周期性检查取消标志。

**架构推论：** 「遍历与计分分离」这个结构，是把两种完全不同节奏的工作解耦：遍历是分钟级的、I/O 密集的、单调推进的；计分是毫秒级的、CPU 密集的、随查询变化反复重算的。合并成一个线程会让「边打边搜」变成「每次打字都等你遍历完」。分开之后，遍历在后台慢慢填索引，而查询响应只依赖「已索引的部分」——这也是为什么结果会「逐步变好」而不是「一次到位」。

**架构推论：** `require_git(true)` 那一大段注释值得当作范例。它写的不是「这行代码做什么」，而是「如果不这么写会发生什么」——一个具体的、可复现的、症状严重的失败（`~/.gitignore` 里的 `*` 让搜索永远无结果）。注释该写这类「为什么」，因为代码本身已经把「是什么」说清了。

**架构推论：** 用 `IndexedEntry.match_type` 携带文件类型，而不是匹配阶段重新 stat，是一个正确的性能决定。stat 是系统调用，而每次查询更新都要对 N 条匹配算一次——若每次都 stat，打字就会变成系统调用风暴。所以遍历时顺手记下类型，是唯一合理的方案。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/file-search/src/lib.rs` | `FileSearchSession` | 会话句柄，只暴露 `update_query` 与 Drop 关停 |
| `codex-rs/file-search/src/lib.rs` | `FileSearchSession.update_query` | 发一个信号，不等结果 |
| `codex-rs/file-search/src/lib.rs` | `FileSearchResults` | 结果集与总数 |
| `codex-rs/file-search/src/lib.rs` | `FileMatch` | 分数、相对路径、类型、根与可选高亮下标 |
| `codex-rs/file-search/src/lib.rs` | `MatchType` | 文件与目录，线上小写 |
| `codex-rs/file-search/src/lib.rs` | `FileSearchOptions` | 上限、排除、线程数、下标开关、忽略开关 |
| `codex-rs/file-search/src/lib.rs` | `FileSearchOptions.respect_gitignore` | 关闭时五项忽略全关 |
| `codex-rs/file-search/src/lib.rs` | `IndexedEntry` | 全路径与类型的索引项，免二次 stat |
| `codex-rs/file-search/src/lib.rs` | `FileSearchSnapshot` | 查询、匹配、总数、扫描数、是否遍历完 |
| `codex-rs/file-search/src/lib.rs` | `walker_worker` | 并行遍历、注入索引、按条数检查取消 |
| `codex-rs/file-search/src/lib.rs` | `matcher_worker` | 信号合并、reparse、tick、取前 N |
| `codex-rs/file-search/src/lib.rs` | `get_file_path` | 多根前缀匹配时取最深根 |
| `codex-rs/file-search/src/lib.rs` | `build_override_matcher` | `!pattern` 形式的排除规则集 |
| `codex-rs/file-search/src/lib.rs` | `create_session` | 起两路 worker 并接好三处耦合 |
| `codex-rs/file-search/src/lib.rs` | `SessionReporter.on_complete` | 契约：每次 update_query 至少一次 |

## 参考

- [open-interpreter 源码：file-search/src/lib.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/file-search/src/lib.rs)
- [nucleo 文档：增量模糊匹配器](https://docs.rs/nucleo/)
- [ignore crate 文档：忽略规则与 require_git](https://docs.rs/ignore/latest/ignore/struct.WalkBuilder.html)

## 与主核 / AI 的关系

主核里最接近的是「索引与查询的分离」：后台线程构建索引，查询线程只读索引做匹配。这一页的两 worker 结构与那个模式同构，连「索引先可用、结果逐步变好」这个用户可见的行为都一样。

与 j01（工作树）的关联很直接：这个搜索的根目录通常就是某个工作树。`get_file_path` 的多根取最深，正好服务「主仓库 + 若干工作树同时可搜」的场景。

与 goose 的 i03（handoff 预算装箱）对照：那边把「不可分的单元」作为装箱粒度，这里把「已索引的条目」作为匹配粒度。两者都在处理「很多小项 + 一个硬约束」——一个是 token 预算，一个是线程数与防抖窗口。

对 AI 工程最实用的一条是 `require_git(true)` 背后的教训：**父目录的配置会污染子目录的行为**。这类问题的症状往往是「功能完全失效」而不是「结果不准」，最容易被误判成「这个功能坏了」。任何读取「从当前目录向上找配置」的工具（ignore 规则、配置文件、AGENTS.md 的上溯搜索见 g05）都要想清楚「上溯到哪儿为止」，默认应当是「到项目边界为止」而不是「到文件系统根」。

另一条是「查询更新不重走盘」。把「索引」与「匹配」分开之后，输入法的每次按键都只触发一次计分，而遍历可以在后台继续。这个结构对任何「边输入边搜索」的场景都适用，判据是：这个搜索的数据源是否需要遍历/网络请求？是就分两个阶段，不是就可以合并。

最后一条是 `default(100ms)` 那个空分支。在 `select!` 里加一个超时分支，是为了让「周期性检查外部标志」这件事有确定的上界。没有它，一个完全空闲的循环就只能在「有信号」时检查取消——而取消恰恰是「没有新信号」的场景。给每个等待循环一个最大等待时间，是让取消总能生效的通用手段。
