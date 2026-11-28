---
project: open-interpreter
feature: session-index
title: 会话索引与倒序扫描
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/rollout/src/session_index.rs
    symbols: [SessionIndexEntry, scan_index_from_end, SESSION_INDEX_FILE]
  - path: codex-rs/rollout/src/reverse_jsonl_scanner.rs
    symbols: [ReverseJsonlScanner, ScanOutcome, READ_CHUNK_SIZE]
---

# 会话索引与倒序扫描

## 一句话职责

用一份**只追加、不改写**的 JSONL 记录「线程 id 到它的名字」的映射，靠「最新的行赢」替代原地更新；再从文件**尾部**按 64KB 分块倒着读，让「找最新的一条」不必加载整份索引。

## 机制是什么

`SessionIndexEntry` 三字段：`id: ThreadId`、`thread_name: String`、`updated_at: String`。`SESSION_INDEX_FILE = "session_index.jsonl"` 说明索引就落在 `codex_home` 根下一个固定文件。

关键语义在 `append_thread_name` 的 docstring 里：「名字更新是**只追加**的；解析名字或 id 时**最新的条目赢**」。

所以重命名不需要读改写——再追加一行同名新名字即可，旧行留着当历史。这是把「更新」退化成「追加」的做法，好处是写入变成一次 `O_APPEND` 加 `flush`，且并发安全由锁加 append 语义共同保证。

倒序扫描器 `ReverseJsonlScanner<R>` 是另一半：它从文件尾部往前读，逐条产出记录。`READ_CHUNK_SIZE = 64 * 1024` 是每次读取的字节数。

`ScanOutcome<T>` 两个变体：`Parsed(T)` 与 `Rejected(serde_json::Error)`。所以「某一行不是合法 JSON」不会中断扫描——调用方拿到 `Rejected` 后可以继续下一条。

## 怎么实现的

**为什么倒着读。** 索引里同一个线程会有多行（每次重命名追加一行）。要找「这个线程现在的名字」，正向读需要读到文件末尾才知道最新值——那意味着每次查询都读全文件。倒着读则第一条命中的就是最新值。

这个方向选择决定了整个数据结构：如果没有「最新行赢」的语义，倒序扫描就毫无意义；如果没有倒序扫描，只追加的写入就会让查询变慢。两者必须成对出现。

**扫描器的状态机。** `ReverseJsonlScanner` 有七个字段：

- `reader`（要 `Read + Seek`，因为要 `seek` 到任意偏移）
- `next_chunk_end: u64`（当前块结束处的字节偏移）
- `chunk_position: usize`（块内已被消费的位置，从后往前）
- `chunk: Vec<u8>`（固定 64KB 的缓冲）
- `record_reversed: Vec<u8>`（**倒序**的当前记录字节）
- `max_record_bytes: Option<usize>`（可选的长度上限）
- `discarding_oversized_record: bool`（正在丢弃一个超长记录）

`record_reversed` 存倒序字节是一个省事的做法：从后往前读到的片段，直接 reverse 后 append，最后在 `finish_record` 里整体 `reverse()` 一次再解析。否则每读一个片段都要在头部插入，成本是二次的。

**分块读的边界处理。** `scan_next` 的主循环：

若 `chunk_position == 0`（当前块已耗尽），先判 `next_chunk_end == 0`——到文件头了，若还在丢弃超长记录就直接返回 `None`，否则 `finish_record()` 交出最后一条。然后算出本次读多少：`min(next_chunk_end, READ_CHUNK_SIZE)`，前移偏移，`seek` 后 `read_exact`，把 `chunk_position` 设为读到的长度。

接着在块内 `rposition(b'\n')` 找**最后一个**换行。找到时，`newline_position + 1` 之后的那段就是「一条记录的尾部片段」；把 `chunk_position` 设为 `newline_position`（把这段消费掉），然后：

- 若在丢弃模式，清掉丢弃标记，`continue`（丢掉这条，继续下一条）。
- 否则把片段倒序存进 `record_reversed`，再 `finish_record()`。

没找到换行说明整块都属于同一条记录的中间部分——倒序 append 全部，`chunk_position = 0`，继续读下一块。

这个实现有个细节值得留意：**它按「换行符的位置」切割，而不是按「记录边界」**。所以最后一个记录（文件末尾没有换行）会被当作「没有 `\n` 收尾的片段」处理：第一次进入循环时 `chunk_position == 0` → 读最后一块 → `rposition` 找到的是倒数第二个换行 → 把最后一条的尾部当片段攒起来。若文件只有一行且无结尾换行，则找不到 `\n`，整块进 `record_reversed`，耗到文件头时 `finish_record()` 交出它。

**空记录的过滤在 `finish_record` 里。** 它先 `reverse()`，然后判 `record_reversed.iter().all(u8::is_ascii_whitespace)`——全空白就返回 `None`（不产出）。这个判断放在这里而不是调用方，是因为「末尾空行」「连续空行」在追加写里很常见，让扫描器统一吞掉它们可以简化所有使用方。

**超长记录的处理分两条路径，不是一条。** `with_max_record_bytes` 允许设上限。到达上限时不做缓冲、不做解析——直接置 `discarding_oversized_record = true` 并清掉已攒的字节。上面两个 append 分支里都写了一遍同样的判据与置位逻辑：

```rust
if self.max_record_bytes.is_some_and(|max| self.record_reversed.len().saturating_add(fragment.len()) > max) {
    self.record_reversed.clear();
    self.discarding_oversized_record = true;
} else {
    self.record_reversed.extend(...);
}
```

两处重复看起来该抽成函数，但抽出后每处都要在借用 `chunk` 的同时可变借用 `self`，Rust 的借用检查会立刻拒绝——所以这里用重复。这是一个被语言逼出来的重复，不是疏忽。

丢弃模式的退出在 `rposition` 命中时：`discarding_oversized_record = false; continue;`。而在文件头时若仍在丢弃，返回 `None` 而不是 `finish_record()`——因为那条记录已经被截断，交出去也没意义。

`saturating_add` 而不是普通加法：防止 `len + fragment.len()` 在极端大文件上溢出成小值，从而把「超长」误判成「没超」。这是防守一个有符号整数边界。

**`new_at` 让「冻结前缀」成为可能。** `new(reader)` 从 `SeekFrom::End(0)` 开始；`new_at(reader, end_byte_offset)` 允许指定逻辑终点，并校验它不越过真实文件尾（越过就 `InvalidInput`）。

docstring 说明用途：「让调用方扫描一个冻结的 JSONL 前缀，而不读取前缀被捕获之后追加的记录」。

这是倒序扫描的一个独特优势：正向读要「读到某个位置停下来」，但你不能确定那个位置之后的内容是否属于你要的范围；倒序读则可以先固定一个终点，然后往前扫——追加发生在终点之后，天然被排除。

**`scan_next_rollout_line` 是两段解码。** 它先 `scan_next::<Value>()`（通用 JSON），再对 `Parsed(value)` 走 `crate::decode_rollout_line`，把「这个 JSON 值能不能变成一条 rollout 行」的判断交给规范解码器。失败则折成 `Rejected`。

所以「JSON 语法正确但语义不是 rollout 行」被归为 `Rejected`，与「JSON 语法错误」同一桶——调用方的处理方式相同（跳过），所以合并是合理的。

**索引的三种写入与两种读取。** 写入：

1. `append_thread_name` → 构造 `SessionIndexEntry`（`updated_at` 用 RFC3339，格式化失败时写 `"unknown"` 而不是报错）→ `append_session_index_entry`。
2. `append_session_index_entry` → 取静态 `SESSION_INDEX_LOCK`（`Mutex<()>`，`LazyLock` 惰性初始化）→ `OpenOptions::new().create(true).append(true)` → 序列化 + 加换行 → `write_all` + `flush`。
3. `remove_thread_name_entries` → 同样持锁 → 读全文 → 逐行解析，id 匹配的丢弃 → 若有删除，写到 `*.jsonl.tmp` 再 `rename`。

第 3 项的「读全文改全文再 rename」看似破坏了「只追加」的原则，但它是**删除**操作——删除无法用追加表达。注意它用 `is_ok_and` 判匹配：解析失败的行被当作「不匹配」保留（`is_ok_and` 失败时返回 false → `should_remove` 为 false → 保留）。所以坏行不会被误删。

`removed` 标记让「没有任何行匹配」时直接返回，不做 rename——避免无谓的文件替换。

读取：

1. `find_thread_name_by_id` → `spawn_blocking(scan_index_from_end_by_id)`——倒序扫描找第一条 id 匹配。
2. `find_thread_names_by_ids` → **正向**流式读整个文件，维护一个 `HashMap<ThreadId, String>`，`insert` 覆盖。

第 2 项的方向与前一项相反，值得解释：批量查询要为 N 个 id 各找最新名字，若逐个倒扫就要扫 N 遍。正向读一遍、用 `insert` 覆盖（后出现的赢 = 最新的赢），只需一遍。所以**两个方向的扫描器都被用上了：单查询倒扫、批查询正扫**。

`find_thread_names_by_ids` 里还有一处细节：`entry.thread_name.trim()` 后为空则跳过。所以「名字被清空」这个操作被表达为「追加一个空名字」，批查询会略过它——即该 id 在结果 map 里保持缺省。但单查询 `find_thread_name_by_id` 不会跳过空名字，会返回 `Some("")`。两者对「空名字」的处理不一致。

**`scan_index_from_end_for_each` 是回调式内核。** 它建 `ReverseJsonlScanner`，循环 `scan_next::<SessionIndexEntry>()`，对 `Parsed(entry)` 调 `visit_entry`；若回调返回 `Some` 就整个返回。`Rejected` 则 `continue`——坏行静默跳过。

外壳 `scan_index_from_end` 把「谓词 + 返回克隆」包成回调；`stream_thread_ids_from_end_by_name` 则在回调里做更多事（去重 + 名字匹配 + 通过 channel 发出）。

`stream_thread_ids_from_end_by_name` 里的 `seen.insert(entry.id)` 是这段代码最需要注意的地方：注释写「第一次看到的行是它最新的名字。**忽略该 id 的更旧行**，以免线程改名后历史名字被当作当前名字」。

所以它用 `seen` 集合做「每个 id 只考虑最新那行」。若一个线程曾叫 `foo` 后改叫 `bar`，现在按 `foo` 搜：倒扫先看到 `bar`（最新），`seen` 插入 `foo` 的 id、名字不匹配 → 跳过；再看到 `foo`（旧行），`seen` 已含该 id → 忽略。所以**旧名字搜不出来**——这是正确的语义（改名后旧名字不该再命中）。

`tx.blocking_send(entry.id)` 在 `spawn_blocking` 里用阻塞发送（channel 容量 1），配合 `return Ok(Some(entry.clone()))` 让扫描立即终止。所以它是一条「边扫边吐、吐到调用方决定停」的流。

**候选排序解决的是「擦除」问题。** `find_thread_meta_candidates_by_name_str` 的注释点明了两个陷阱：

1. 「流式吐匹配 id 而不是在第一个名字命中就停下：最新的条目可能指向一个**从未物化过 rollout** 的线程」。
2. 「持续走到有可加载 rollout 的 id，这样一个未保存或部分完成的改名不会**遮蔽**更旧但已持久化的同名会话」。

所以它把「所有同名的历史 id 都收集起来、逐个校验可加载性」，然后按修改时间**降序**排序。排序前先过滤（source、model_provider），注释写「过滤发生在排序之前，这样更新的不合格重复项不会藏住更旧的可会话」。

provider 过滤是单向的：「只拒绝**明确不匹配**，因为更旧的 rollout 省略了 provider 元数据」——`is_none_or` 让「元数据缺失」通过。这条宽容是必要的：老文件的格式里没有这个字段，若要求必须有就永远恢复不了它们。

## 关键取舍

**源码事实：** 索引只追加不改写，「最新行赢」。重命名 = 追加一行。

**源码事实：** 倒序扫描按 64KB 分块，`record_reversed` 存倒序字节，最后一次 `reverse()`。

**源码事实：** `new_at` 允许把逻辑终点固定在某个偏移，于是「冻结前缀」可扫，追加内容天然被排除。

**源码事实：** 超长记录被整条丢弃，不缓冲不解析；丢弃发生在两个 append 分支里（重复代码是被借用检查逼出来的）。

**源码事实：** `finish_record` 吞掉全空白记录，所以末尾空行不产生条目。

**源码事实：** 单查询用倒扫，批查询用正扫一遍加覆盖写入。两个方向都有用。

**源码事实：** 空名字在批查询里被跳过（该 id 缺省），在单查询里被返回为空串。

**源码事实：** 按名字找会话会收集所有同名历史 id（不去重、不停在第一个命中），逐个校验可加载性，再按 mtime 降序。

**架构推论：** 「只追加 + 最新行赢 + 倒序扫描」是一组不可拆的设计。它把「更新」的写入成本压到最低（一次 append），同时把「读最新」的成本也压到最低（只读尾部）。代价是文件单调增长，且需要一条「最后写入者赢」的判定规则——没有倒扫能力时，只追加的索引会让查询变成 O(文件大小)。

**架构推论：** 「按名字找历史会话」那段的三层防御揭示了一个真实的产品问题：名字是用户可随意改的、且可以重名。所以「按名字定位会话」本质上是一个**模糊查询**，必须准备「多个候选 + 无法加载的候选 + 更新的不合格候选」三种情况。把这三条写进注释而不是让它成为隐式行为，是这个模块最容易读懂的优点。

**架构推论：** 倒序扫描器实现成「通用 JSONL 倒扫器」而不是「索引专用解析器」，使它可被复用于其他尾部查询（例如「取最近 N 条 rollout 记录」）。这是把「读取方向」与「内容结构」解耦带来的复用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/rollout/src/session_index.rs` | `SessionIndexEntry` | 索引行：id、名字、更新时间 |
| `codex-rs/rollout/src/session_index.rs` | `SESSION_INDEX_FILE` | `session_index.jsonl` 常量 |
| `codex-rs/rollout/src/session_index.rs` | `append_thread_name` | 重命名即追加一行 |
| `codex-rs/rollout/src/session_index.rs` | `append_session_index_entry` | 锁下的单行 append 加 flush |
| `codex-rs/rollout/src/session_index.rs` | `remove_thread_name_entries` | 删改全文后 rename 替换 |
| `codex-rs/rollout/src/session_index.rs` | `find_thread_name_by_id` | 倒扫找第一条 id 命中 |
| `codex-rs/rollout/src/session_index.rs` | `find_thread_names_by_ids` | 正扫一遍覆盖写入，批量解析 |
| `codex-rs/rollout/src/session_index.rs` | `scan_index_from_end` | 谓词式倒扫外壳 |
| `codex-rs/rollout/src/session_index.rs` | `scan_index_from_end_for_each` | 回调式倒扫内核，坏行静默跳过 |
| `codex-rs/rollout/src/session_index.rs` | `stream_thread_ids_from_end_by_name` | 每个 id 只看最新行的流式匹配 |
| `codex-rs/rollout/src/session_index.rs` | `find_thread_meta_candidates_by_name_str` | 候选收集、过滤先于排序 |
| `codex-rs/rollout/src/reverse_jsonl_scanner.rs` | `ReverseJsonlScanner` | 从尾部倒读的 JSONL 扫描器 |
| `codex-rs/rollout/src/reverse_jsonl_scanner.rs` | `ReverseJsonlScanner.new_at` | 固定逻辑终点以扫描冻结前缀 |
| `codex-rs/rollout/src/reverse_jsonl_scanner.rs` | `ReverseJsonlScanner.scan_next` | 分块读、找换行、攒倒序记录 |
| `codex-rs/rollout/src/reverse_jsonl_scanner.rs` | `ScanOutcome` | Parsed / Rejected 两分，坏行不中断 |
| `codex-rs/rollout/src/reverse_jsonl_scanner.rs` | `READ_CHUNK_SIZE` | 64KB 分块大小 |

## 参考

- [open-interpreter 源码：rollout/session_index.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/rollout/src/session_index.rs)
- [open-interpreter 源码：rollout/reverse_jsonl_scanner.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/rollout/src/reverse_jsonl_scanner.rs)
- [open-interpreter 文档](https://www.openinterpreter.com/docs)

## 与主核 / AI 的关系

主核里最接近的是「只追加的日志 + 尾查」：写入只往日志末尾追加，读取要找最新状态时从尾部往回扫，扫到第一条有效记录即停。这一页的索引与那类日志结构几乎同构，连「最新行赢」这条语义都一样。

差别在失败容忍度。主核的日志记录有校验和与严格格式，坏记录通常意味着磁盘问题；这里的 `ScanOutcome::Rejected` 把「这行不是合法 JSON」当成正常情况跳过——因为索引是**辅助数据**，丢一行只让某个名字查不到，不该让整个会话列表打不开。这个「辅助程度决定宽容度」的判断很值得记：同样是坏数据，主数据该报错，辅助数据该跳过。

与 g01（会话滚动记录）的关系：g01 写的是主数据（会话历史），`load_rollout_items` 遇到坏行只记 `parse_errors` 计数并继续——那是宽容，但会报告计数。g03 的索引更宽容，连计数都不报。这个梯度反映了数据的可重建程度：会话历史丢了无法重建，索引丢了可以重新扫目录重建。

对 AI 工程最实用的一条是「冻结前缀扫描」。做「读取某个时间点的快照」这类需求时，正向读要解决「读到哪儿停」，而位置之后可能还有内容。先把终点固定成字节偏移，再从那里往前扫，就天然只覆盖快照范围。这个模式对任何「追加型文件的区间读取」都适用，且不需要文件系统支持快照。

另一条是「超长记录整条丢弃而不是截断解析」。截断后的 JSON 一定解析失败，白花一次解析成本；而且截断可能恰好截在一个看起来合法的前缀上，产生一个语义错误的「成功」——那是更糟的结果。直接丢弃并 Documented 为「这条太大，跳过」，是这套设计里的正确选择。这也解释了为什么 `max_record_bytes` 是可选的：默认不设时行为是无损的，设了才启用这条降级路径。
