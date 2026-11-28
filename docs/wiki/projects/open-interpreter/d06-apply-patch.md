---
project: open-interpreter
feature: apply-patch
title: 补丁解析与应用
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/apply-patch/src/lib.rs
    symbols: [ApplyPatchArgs, ApplyPatchFileChange, apply_patch_file_update_mode_from_env]
  - path: codex-rs/apply-patch/src/lib.rs
    symbols: [ApplyPatchOptions, ApplyPatchError, CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS_ENV_VAR]
---

# 补丁解析与应用

## 一句话职责

把模型生成的 `*** Begin Patch` 格式补丁应用到文件系统，并且**在失败时能说清到底改了哪些东西**——因为一次补丁可能包含多个文件操作，中途失败会留下只做了一半的结果。

## 机制是什么

这个模块要处理的核心矛盾是**多文件操作的原子性**。一条补丁可以同时改动五个文件，而文件系统没有事务。若第三个文件的匹配失败：

- 前两个文件已经被改了——**不能被假装没发生**；
- 后两个文件还没动；
- 调用方（agent 循环）需要知道「现在磁盘上是什么状态」才能决定下一步。

所以这个模块有两个并列的产出，而不只是一个结果：

| 产出 | 类型 | 语义 |
|---|---|---|
| 成功 | `AppliedPatchDelta` | 实际提交了哪些改动 |
| 失败 | `ApplyPatchFailure`，内含 `AppliedPatchDelta` | **失败之前已经提交的那些改动** |

即**失败也带一份 delta**。这是整份设计最值得记的一点：它不承诺原子性，但承诺**可观测性**。

而第二个要处理的事是**行尾**。补丁是按行匹配的，而文件可能是 LF、CRLF 或 CR 结尾。稍不注意，一次「改一行」的补丁会把整个文件的行尾全部改写，产生一个 diff 里看不出原因的巨大变更。

## 怎么实现的

**`ApplyPatchFileUpdateMode` 只有两个变体，默认是「归一化为 LF」。**

```rust
pub enum ApplyPatchFileUpdateMode {
    /// Preserve the historical behavior of normalizing updated files to LF.
    #[default]
    NormalizeToLf,
    /// Preserve existing line endings and use the file's preferred ending for new lines.
    PreserveLineEndings,
}
```

读法：**默认行为是把更新后的文件统一成 LF**。这是「历史行为」——注释直说了。而新行为（保留原有行尾）必须显式开启。

为什么默认不是「保留」？因为历史行为已经这样了，大量测试与用户预期建立在其上。改默认值会让既有使用方式的输出发生变化（原本 CRLF 的文件被更新后变成 LF）。所以这里把更正确的行为做成了**可选**而不是默认。

它有两种开启路径，这揭示了一个进程边界问题：

```rust
pub const CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS_ENV_VAR: &str = "CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS";

pub fn apply_patch_file_update_mode_from_env() -> ApplyPatchFileUpdateMode {
    match std::env::var(CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS_ENV_VAR).as_deref() {
        Ok("1") => ApplyPatchFileUpdateMode::PreserveLineEndings,
        _ => ApplyPatchFileUpdateMode::NormalizeToLf,
    }
}
```

- **进程内调用**（宿主直接调 `apply_patch_with_options`）走 `ApplyPatchOptions`，由调用方显式传。
- **独立可执行文件**（arg0 分派出的 `apply_patch` 二进制）**只能通过环境变量**拿到这个选择。

第二种路径的必要性来自架构：arg0 分派意味着「同一个二进制以不同用途启动」，独立进程不知道宿主的运行时配置。所以要用一个环境变量把这个选择**传过进程边界**。

常量的文档注释写清了这一点：

> Internal environment variable used to carry the selected update mode through the arg0-dispatched standalone executable.

而 `CODEX_CORE_APPLY_PATCH_ARG1` 的注释则解释了另一个常量为什么放在这个 crate 里：

> Although this constant lives in `codex-apply-patch`（to avoid forcing `codex-arg0` to depend on `codex-core`）, it remains part of the "codex core" process-invocation contract for the standalone `apply_patch` command surface.

即「为了避免让 `codex-arg0` 依赖 `codex-core`，这个常量放在这里，但它在语义上属于 core 的进程调用契约」。这是一条典型的**依赖方向妥协**：常量语义上该属于 core，但放在 core 会让 arg0 依赖 core，所以下沉到更底层的 crate。

而环境变量的写入侧在 `core`：

```rust
pub fn inject_apply_patch_env(env: &mut HashMap<String, String>, features: &Features) {
    env.retain(|key, _| !key.eq_ignore_ascii_case(CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS_ENV_VAR));
    if features.enabled(Feature::ApplyPatchPreserveLineEndings) {
        env.insert(CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS_ENV_VAR.to_string(), "1".to_string());
    }
}
```

三处值得看：

1. **先 `retain` 删掉同名键**（大小写不敏感），再按需插入。注释解释了顺序：「Apply this after inherited or client-provided environment overrides so the active feature configuration remains authoritative」。即**先擦掉（可能来自继承环境或客户端的）用户设置，再写入权威值**。若不先删，客户端可以通过设一个同名环境变量来控制补丁行为——这是一个环境注入面。
2. **只写 `"1"`，不写 `"0"`**。关闭状态用「不设这个变量」表达，而不是设成 `"0"`。这与读取侧 `Ok("1") => 保留，_ => 归一化` 的非穷尽匹配配合——任何非 `"1"` 的值都等于关闭。这样客户端设 `"0"` 或 `"false"` 都无法开启。
3. **`features.enabled(Feature::...)`**：这是一个特性开关，不是用户配置。所以这个行为是**逐步放量**的，而不是让用户选。

**`ApplyPatchOptions` 两个字段，两个不同的关注点。**

```rust
pub struct ApplyPatchOptions {
    pub update_file_mode: ApplyPatchFileUpdateMode,
    pub follow_symlinks: bool,
}

impl Default for ApplyPatchOptions {
    fn default() -> Self {
        Self { update_file_mode: ApplyPatchFileUpdateMode::default(), follow_symlinks: true }
    }
}
```

`update_file_mode` 管**内容**（行尾），`follow_symlinks` 管**路径解析**。两者的默认值方向相反：前者取历史行为（归一化），后者取 `true`（跟随符号链接）。

`follow_symlinks: true` 的注释在类型文档里：

> Policy for one patch application. Standalone callers follow symlinks by default.

即默认跟随。这与 c03 讲的那套沙箱符号链接处理形成对照——那里为了安全对深层符号链接是拒绝的，而这里默认跟随。差别在于用途：补丁工具面对的是用户工程里的符号链接（`node_modules` 里大量符号链接），拒绝跟随会让正常补丁失败。安全性由外层的沙箱上下文（`sandbox: Option<&FileSystemSandboxContext>`）负责。

**`ApplyPatchFileChange` 是三变体，每个都带**完整**内容而不是差值。**

```rust
pub enum ApplyPatchFileChange {
    Add { content: String },
    Delete { content: String },
    Update { unified_diff: String, move_path: Option<PathUri>, new_content: String },
}
```

三处：

- **`Add` 与 `Delete` 都带完整 `content`**。`Add` 带的是将要写入的内容；`Delete` 带的是将要被删除的内容（即读取到的原文）。后者是必要的——删除前必须知道要删什么，而且失败时能恢复。
- **`Update` 同时带 `unified_diff` 与 `new_content`**：前者是给人看的（也是给 UI 展示 patch preview 用的），后者是给写入用的。注释只说 `new_content`：「new_content that will result after the unified_diff is applied」。两者并存意味着**这个结构既服务于展示也服务于执行**。
- **`move_path: Option<PathUri>`**：更新可以伴随移动。这是「改内容 + 改路径」合成一个操作，而不是拆成 delete + add——后者会丢失文件身份（权限、其他元数据）。

**`ApplyPatchAction` 是验证阶段的产物，与 `AppliedPatchDelta` 严格区分。**

```rust
pub struct ApplyPatchAction {
    changes: HashMap<PathUri, ApplyPatchFileChange>,
    update_file_mode: ApplyPatchFileUpdateMode,
    pub patch: String,
    pub cwd: PathUri,
}
```

类型文档：「By construction, all paths should be absolute paths」。即**这个结构里不会有相对路径**——相对路径在构造时已按 `cwd` 解析。这个不变量由构造过程保证（不是靠运行期检查），是 Rust 里「用构造函数守不变量」的标准做法。

`patch` 字段的注释解释了它的用途：

> The raw patch argument that can be used to apply the patch. i.e., if the original arg was parsed in "lenient" mode with a heredoc, this should be the value without the heredoc wrapper.

即它存的是**去掉 heredoc 包裹后的原始补丁文本**。为什么需要：因为补丁可能以多种形态进来（`*** Begin Patch` 直接给、或被 shell heredoc 包起来），而这个字段统一成「干净的补丁文本」。这对「重新应用」或「展示给用户确认」都有用。

而 `MaybeApplyPatchVerified` 的四变体是这个验证阶段的**结果分类**：

```rust
pub enum MaybeApplyPatchVerified {
    Body(ApplyPatchAction),
    ShellParseError(ExtractHeredocError),
    CorrectnessError(ApplyPatchError),
    NotApplyPatch,
}
```

四个变体区分了四种完全不同的情形：

| 变体 | 含义 | 该做什么 |
|---|---|---|
| `Body` | 这是 apply_patch 调用且补丁正确 | 应用它 |
| `ShellParseError` | 无法解析 argv 以判断是不是 apply_patch | 当作普通命令 |
| `CorrectnessError` | 是 apply_patch 但补丁本身有错 | 回报错误 |
| `NotApplyPatch` | 明确不是 apply_patch | 当作普通命令 |

这个四分类的价值在于把「解析失败」与「不是补丁」分开。若合成一个 `Err`，调用方无法决定「该不该报错给用户」——`NotApplyPatch` 是完全正常的（大多数 shell 命令都不是补丁），而 `CorrectnessError` 必须报。

**`AppliedPatchDelta` 记录「实际提交了什么」，它比 `ApplyPatchAction` 更强。**

```rust
pub struct AppliedPatchDelta {
    changes: Vec<AppliedPatchChange>,
    exact: bool,
}
```

三处与 `ApplyPatchAction` 的差别：

1. **`Vec` 而不是 `HashMap`**。注释：「A committed file change, preserved in **the order it was applied**」。顺序是信息——`Move` 操作与后续的 `Update` 都依赖顺序。用 `HashMap` 会丢掉它。
2. **`exact: bool`**。这个字段是关键。它表示「这份 delta 是否精确地反映了磁盘状态」。若能确定，为 `true`；若中间有不确定的时刻（例如某次写入的结果无法读出），为 `false`。
3. **`append` 保留聚合的精确性**：

```rust
pub fn append(&mut self, other: Self) {
    self.changes.extend(other.changes);
    self.exact &= other.exact;
}
```

   即「精确 ∧ 精确 = 精确」。任何一段不精确就让整体不精确。这是「合取而非析取」的选择——对事实的记录，不确定的部分会污染整体。

**`AppliedPatchFileChange` 比 `ApplyPatchFileChange` 多出两个字段，都在表达「被覆盖了什么」。**

```rust
pub enum AppliedPatchFileChange {
    Add { content: String, overwritten_content: Option<String> },
    Delete { content: String },
    Update { move_path: Option<PathUri>, old_content: String, overwritten_move_content: Option<String>, new_content: String },
}
```

两处差异：

- **`Add` 多 `overwritten_content`**：若目标文件已存在，这次「添加」实际是覆盖，所以要记录被覆盖的原文。没有它，这次操作就不可能被撤销。
- **`Update` 多 `old_content` 与 `overwritten_move_content`**：`old_content` 是更新前的原文（撤销需要它）；`overwritten_move_content` 是「移动到目标路径时目标已存在」的情形下，被覆盖掉的那份内容。

这个类型的存在说明一件事：**`AppliedPatchFileChange` 的职责不只是「记录改了什么」，还要「记录破坏了什么」**。后者的唯一用途是撤销/回滚，而这是「失败也带 delta」这条设计的延伸——delta 里带的信息足够撤销已提交的改动。

**`ApplyPatchFailure` 把错误与 delta 绑在一起。**

```rust
pub struct ApplyPatchFailure {
    error: ApplyPatchError,
    delta: AppliedPatchDelta,
}
```

两个构造函数区分得很清楚：

```rust
fn new(error: ApplyPatchError, delta: AppliedPatchDelta) -> Self { Self { error, delta } }
fn without_delta(error: ApplyPatchError) -> Self { Self::new(error, AppliedPatchDelta::empty()) }
```

`without_delta` 只用于**解析阶段**的失败——那时还没碰文件系统，所以 delta 必然是空的。这个构造函数的命名直接表达了这条事实，比传 `AppliedPatchDelta::empty()` 更清楚。

`into_parts` 提供解构：

```rust
pub fn into_parts(self) -> (ApplyPatchError, AppliedPatchDelta)
```

而 `delta()` 给借用访问。两者并存是标准的「按需选择所有权」模式——若调用方只需要报错，可以只要前者；若需要更新自己的状态，拿后者。

**`ApplyPatchError` 六个变体，其中两个值得单看。**

```rust
pub enum ApplyPatchError {
    ParseError(#[from] ParseError),
    IoError(#[from] IoError),
    ComputeReplacements(String),
    PathUri(#[from] PathUriParseError),
    ImplicitInvocation,
}
```

- **`ImplicitInvocation`** 的文案很具体：

```
"patch detected without explicit call to apply_patch. Rerun as [\"apply_patch\", \"<patch>\"]"
```

   这是一个**带可操作指引**的错误。它的场景是：模型直接吐了一段补丁文本而没有包在工具调用里。错误信息不只说「错了」，而是给出正确的调用形态。这类错误对模型自愈很有帮助——把正确的形状写在错误里，模型下一轮就照做了。

- **`IoError` 是一个带 `context` 的包装结构**，且手写了 `PartialEq`：

```rust
impl PartialEq for IoError {
    fn eq(&self, other: &Self) -> bool {
        self.context == other.context && self.source.to_string() == other.source.to_string()
    }
}
```

   即比较时用「错误的字符串表示」而不是错误值本身。理由：`std::io::Error` 不实现 `PartialEq`（它可能含 `Box<dyn Error>`），所以要比较就只能比字符串。这是为了满足测试里 `assert_eq!` 的需要，代价是「同一种错误的不同实例」可能被判不等（若字符串略有差异）。

**`apply_patch_with_options` 的失败路径有两个出口，两者都带 delta。**

```rust
match apply_hunks_to_files(hunks, options, cwd, fs, sandbox, &mut delta).await {
    Ok(affected_paths) => {
        print_summary(&affected_paths, stdout).map_err(|error| {
            ApplyPatchFailure::new(ApplyPatchError::from(error), delta.clone())
        })?;
        Ok(delta)
    }
    Err(error) => {
        let msg = error.to_string();
        writeln!(stderr, "{msg}").map_err(|error| {
            ApplyPatchFailure::new(ApplyPatchError::from(error), delta.clone())
        })?;
        ...
        Err(ApplyPatchFailure::new(error, delta))
    }
}
```

值得看的是**成功路径也可能失败**：`print_summary` 若写 stdout 失败（管道断了），整个操作就报错——尽管文件已经全改完了。此时仍然返回带 delta 的失败，所以调用方知道「改动已提交，只是打印失败」。这个细节很正确：把「副作用是否发生」与「报告是否成功」分开。

失败路径上还有一段错误转换：

```rust
let error = if let Some(io) = error.downcast_ref::<std::io::Error>() {
    ApplyPatchError::from(io)
} else {
    ApplyPatchError::IoError(IoError { context: msg, source: std::io::Error::other(error) })
};
```

即「如果能 downcast 成 `io::Error` 就用它，否则包成 `IoError`」。这是一处「尽力保留原始类型」的做法——`anyhow::Error` 丢掉了具体类型，只能靠 downcast 找回。

**两个阶段的 delta 传递用 `&mut` 而不是返回值。**

```rust
async fn apply_hunks_to_files(..., delta: &mut AppliedPatchDelta) -> anyhow::Result<AffectedPaths>
```

参数是 `&mut AppliedPatchDelta`。这让「应用过程中逐条追加」成为可能——每成功改一个文件就追加一条，于是**中途失败时 `delta` 里已经有前面那些成功的记录**。若改成返回值，中途失败就什么也拿不到。

这是「失败也带 delta」在实现层面的支点：**累加器必须在外部持有**。

**行尾处理的两种模式在 `file_update.rs` 里分成两条完全不同的代码路径。**

- **`NormalizeToLf`**：`original_contents.split('\n')` 得到行，做完替换再 `join("\n")`。CRLF 文件里的 `\r` 会留在每行末尾（`split('\n')` 不切它），于是替换后写回时……这里的行为细节值得注意：`split('\n')` 保留 `\r`，`join("\n")` 也会保留它，所以**实际上 CRLF 会被保留**，除非替换逻辑本身动了行尾。而注释说这个模式是「normalizing updated files to LF」——即它归一化的对象是**新写入的行**（通过 `+` 加进来的行不带 `\r`）。

  还能看到一行：

```rust
if !new_lines.last().is_some_and(String::is_empty) {
    new_lines.push(String::new());
}
```

  即**确保文件以换行结尾**。`split('\n')` 会产生一个尾部空元素（若原文以 `\n` 结尾），这一行检查并补上。这是一个容易被漏掉的不变量——POSIX 文本文件约定以换行结束，而补丁工具若不保证它，会让文件末尾的「无换行」状态在多次编辑后变得不一致。

- **`PreserveLineEndings`**：走 `SourceFile::parse` / `apply_replacements` / `into_contents` 那一套，由 `text_file.rs` 实现。那里把每行拆成「文本 + 可选行尾」，并记录 `preferred_ending`：

```rust
/// The first existing ending becomes the preferred style for inserted lines;
/// files without an ending default to LF.
```

  即**新插入的行使用文件中第一个出现的行尾风格**。这是这个模式的核心行为：不只是保留原有行尾，还让新增行与文件风格一致。

**为什么两个模式要两套实现而不是参数化一套？** 因为 `NormalizeToLf` 的实现是「按 `\n` 切字符串」，而 `PreserveLineEndings` 需要「识别并记住每行的行尾」。后者的数据结构（`SourceLine { text, ending }`）与前者的「纯字符串向量」不兼容。强行参数化会让两条路径都写得更绕。

`seek_sequence.rs` 里还有一处按模式分支的细节：

```rust
crate::ApplyPatchFileUpdateMode::PreserveLineEndings => eof_start.max(start),
```

即「序列匹配越界时的起点」在两种模式下不同。这说明行尾模式的影响不止于写入阶段，还渗到了**匹配阶段**——因为 CRLF 文件按 `\n` 切出来的行末尾带 `\r`，补丁里的行不带，匹配时要处理这个差异。

## 关键取舍

**源码事实：** 失败也返回 delta——`ApplyPatchFailure` 内含 `AppliedPatchDelta`，记录失败**之前**已提交的改动，且 `AppliedPatchFileChange` 额外记录「被覆盖的原内容」以支持撤销。

**源码事实：** 逐文件累加 delta 用的是 `&mut` 参数而不是返回值，这样中途失败时已成功的部分仍在累加器里。

**源码事实：** `AppliedPatchDelta` 用 `Vec` 保持应用顺序，并带 `exact: bool`；`append` 用 `&=` 合并精确性（任一段不精确则整体不精确）。

**源码事实：** 行尾模式默认是 `NormalizeToLf`（历史行为），`PreserveLineEndings` 需显式开启；后者让新插入行使用文件中**第一个出现**的行尾风格，无行尾的文件默认 LF。

**源码事实：** 独立可执行文件只能通过环境变量拿到行尾模式，而进程内调用走 `ApplyPatchOptions`；写入侧先 `retain` 删掉同名键（大小写不敏感）再写入，防止客户端通过环境变量覆盖。

**源码事实：** 环境变量只写 `"1"`，读取侧任何非 `"1"` 值都等于关闭，所以客户端设 `"0"` 或 `"false"` 都无法开启。

**源码事实：** `MaybeApplyPatchVerified` 把「无法解析」（`ShellParseError`）、「是补丁但补丁有错」（`CorrectnessError`）与「明确不是补丁」（`NotApplyPatch`）分成三个变体。

**源码事实：** `ImplicitInvocation` 的错误文案包含正确的调用形态（`["apply_patch", "<patch>"]`），供模型自愈。

**源码事实：** 成功路径也可能因为 `print_summary` 写 stdout 失败而返回失败，此时仍带完整 delta——把「副作用已发生」与「报告成功」分开。

**架构推论：** 「不承诺原子性但承诺可观测性」是这个模块的核心立场。文件系统没有事务，所以一次多文件补丁无法真正原子；与其伪造原子性（失败时假装没改），不如**精确报告已发生的部分**。这让调用方可以提供准确的恢复动作——而 `AppliedPatchFileChange` 里那些「被覆盖的原内容」正是为这个恢复动作准备的素材。

**架构推论：** 行尾模式从默认归一化改成显式保留，是为了不破坏既有行为而分两步走。这类「行为变更要先做成可选」的手法在长期维护的工具链里很常见——**默认值一旦定下就是契约**，改动必须经由开关逐步迁移。

**架构推论：** 那个环境变量的写入侧「先删再写」是一条容易被忽略的安全细节。若不先删，一个客户端提供的同名环境变量就能控制补丁行为（例如强制开启某个未放量的模式）。而「只写 `"1"` 不写 `"0"`」让「关闭」只能由不设变量表达，进一步排除了外部干预。

**架构推论：** 两种行尾模式用两套实现而不是参数化，代价是重复的匹配逻辑（`compute_replacements` 被两条路径各调一次，且 `seek_sequence` 里还有分支）。收益是每条路径都只有自己需要的复杂度。这类「为了清晰而接受重复」的决定在 Rust 里很常见，因为泛型化会让类型参数渗到每个调用点。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/apply-patch/src/lib.rs` | `ApplyPatchArgs` | 解析结果：原始补丁文本 + hunks + 工作目录 |
| `codex-rs/apply-patch/src/lib.rs` | `ApplyPatchFileChange` | 三变体：Add / Delete / Update（含 `unified_diff` 与 `move_path`） |
| `codex-rs/apply-patch/src/lib.rs` | `ApplyPatchOptions` | 行尾模式 + 是否跟随符号链接 |
| `codex-rs/apply-patch/src/lib.rs` | `ApplyPatchError` / `IoError` | 五类错误，含带调用指引的 `ImplicitInvocation` |
| `codex-rs/apply-patch/src/lib.rs` | `CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS_ENV_VAR` | 跨进程传递行尾模式的内部环境变量 |
| `codex-rs/apply-patch/src/lib.rs` | `apply_patch_file_update_mode_from_env` | 只认 `"1"` 为开启，其余一律归一化 |
| `codex-rs/apply-patch/src/lib.rs` | `AppliedPatchDelta` / `AppliedPatchChange` | 已提交改动（有序 + `exact` 标记） |
| `codex-rs/apply-patch/src/lib.rs` | `AppliedPatchFileChange` | 额外记录被覆盖的内容，供撤销 |
| `codex-rs/apply-patch/src/lib.rs` | `ApplyPatchFailure` | 错误 + 已提交 delta 的绑定，`without_delta` 表示解析期失败 |
| `codex-rs/apply-patch/src/lib.rs` | `MaybeApplyPatchVerified` | 四分类：Body / ShellParseError / CorrectnessError / NotApplyPatch |
| `codex-rs/apply-patch/src/lib.rs` | `apply_hunks_to_files` | 以 `&mut` 累加 delta，逐文件提交 |
| `codex-rs/core/src/exec_env.rs` | `inject_apply_patch_env` | 先删同名键再按特性开关写入，防止外部覆盖 |

## 参考

- [open-interpreter 源码：apply-patch/src/lib.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/apply-patch/src/lib.rs)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [POSIX 文本文件行尾约定](https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap03.html)
- [unified diff 格式（GNU diff 手册）](https://www.gnu.org/software/diffutils/manual/html_node/Unified-Format.html)

## 与主核 / AI 的关系

「多步操作失败后报告已完成的部分」与主核里的**部分失败语义**同源。主核执行一条多行 `INSERT` 时，若第三行触发约束冲突，前两行在某些隔离级别下已可见——而 SQL 的 `ROLLBACK` 是唯一的补救。本页做的是同一件事的手工版本：**没有事务可回滚，所以精确记录已发生的事**，让调用方自己决定怎么补。

`AppliedPatchFileChange` 里「被覆盖的原内容」与主核里的**前像（before image）**是同一种东西：撤销或恢复所需的旧值必须与改动一同记录下来，否则恢复不可能。主核把这个信息存在 WAL 里，这里存在返回值里。

「默认值一旦定下就是契约」这条与主核里**版式与配置变更加版本号**的做法一致：主核改 `pg_hba.conf` 的语义时要考虑既有配置文件是否仍能读。把新行为做成可选开关、保留旧默认，是跨版本兼容的通用手法。

对 AI 工程的实际意义在于：文件编辑是 coding agent 最核心也最危险的动作。这份实现给出的三条经验可直接用：**失败要带「已发生什么」而不只是错误**（否则 agent 会在一个以为自己没改过的磁盘上继续操作）、**行尾这类细节要显式建模并保留开关**（否则一次改一行会变成全文件重写），以及**把正确调用形态写进错误文案**（让模型下一轮能自愈）。
