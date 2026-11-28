---
project: goose
feature: prompt-assembly
title: 系统提示组装
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/agents/prompt_manager.rs
    symbols: [PromptManager, SystemPromptBuilder, SystemPromptParts]
  - path: crates/goose/src/agents/prompt_manager.rs
    symbols: [SystemPromptContext]
---

# 系统提示组装

## 一句话职责

`PromptManager` 把「基础模板 + 扩展说明 + 会话覆盖 + 提示附加段」组装成系统提示，并且为了多会话的**提示缓存命中**做了三件刻意的稳定化：时间截断到小时、扩展按名字排序、以及把「基础部分」与「追加部分」分成两个可分别读取的值。

## 机制是什么

这里解决的问题不只是「拼字符串」，而是「拼出来的字符串能不能被 provider 缓存复用」。提示缓存通常按**前缀**命中，所以任何在提示靠前位置变化的东西都会让整份缓存作废。源码在此处有三层防御，且都能在代码里对应到具体行。

组装有两个入口，粒度不同：

- `build()` → `String`：`build_parts()?.join()`，把全部内容拼成一整段。
- `build_parts()` → `SystemPromptParts`：返回 `rendered`（基础部分）与 `extras`（附加段），可分别使用。

这个二分的用途在 `SystemPromptParts` 的字段上就能看出来：`base`、`base_is_override`、`extras` 三个公开字段都对外暴露，说明有调用方需要**单独知道基础部分是什么、它是不是用户覆盖的、以及附加段有哪些**——而不只是拿到最终字符串。

`SystemPromptContext` 是模板变量集合，它决定了 `system.md` 能引用哪些值：

```
struct SystemPromptContext {
    extensions: Vec<ExtensionInfo>,
    current_date_time: String,
    goose_mode: GooseMode,
    is_autonomous: bool,
    code_execution_mode: bool,
    include_extensions: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    moim_system_prompt_block: Option<String>,
}
```

七个字段里有两个是**派生**而非独立配置：`is_autonomous = goose_mode == GooseMode::Auto`，以及 `moim_system_prompt_block` 由 `moim::system_prompt_block()` 现算（可能是 `None`，线程局部开关可关闭它）。

## 怎么实现的

**时间的截断是缓存策略，不是显示精度。**

```
pub fn new() -> Self {
    PromptManager {
        // Use the fixed current date time so that prompt cache can be used.
        // Filtering to an hour to balance user time accuracy and multi session prompt cache hits.
        current_date_timestamp: Utc::now().format("%Y-%m-%d %H:00 %:z").to_string(),
    }
}
```

格式串里的 `%H:00` 把分钟与秒抹成零。注释把权衡说得非常直白：「用固定的当前时间，以便提示缓存可用。截断到小时，以平衡用户时间准确性与多会话提示缓存命中」。

这个设计有一个容易被忽略的后果：**`PromptManager` 是在构造时取时间**，而不是每次组装时取。于是同一个 manager 实例存活期间，所有会话看到的是同一个时间戳——即使跨过整点也不会变。要与测试对齐时另有 `with_timestamp`（`#[cfg(test)]`），它用的是**带秒**的格式 `%Y-%m-%d %H:%M:%S %:z`：

```
#[cfg(test)]
pub fn with_timestamp(dt: DateTime<Utc>) -> Self {
    PromptManager { current_date_timestamp: dt.format("%Y-%m-%d %H:%M:%S %:z").to_string() }
}
```

两处格式不同不是笔误：生产用粗粒度换缓存，测试用细粒度换可断言的快照。有一个测试正是拿它验证时区：构造 `It is currently {{current_date_time}}` 的覆盖模板，断言渲染结果为 `1970-01-01 00:00:00 +00:00`。

**扩展说明的排序是为了跨会话的缓存。**

```
// Stable tool ordering is important for multi session prompt caching.
extensions_info.sort_by(|a, b| a.name.cmp(&b.name));
```

按 `name` 排序——不是按注册顺序，也不是按启用顺序。注释把它归因到「多会话提示缓存」：两个会话即使扩展注册顺序不同，只要启用的是同一批，排序后就得到相同的前缀。这条注释被测试间接兜住（`test_all_platform_extensions` 里快照前也手动 `sort_by`）。

排序之后立刻做一次**净化**：

```
let sanitized_extensions_info: Vec<ExtensionInfo> = extensions_info.into_iter()
    .map(|mut ext_info| { ext_info.instructions = sanitize_unicode_tags(&ext_info.instructions); ext_info })
    .collect();
```

`sanitize_unicode_tags` 是这一页的第二个主题：它去掉 Unicode 标签字符（`U+E0000..U+E007F` 区段的「不可见标签字符」）。这类字符可以让人眼读到的文本与模型读到的文本不同——一种典型的提示注入载体。三个测试用 `\u{E0041}\u{E0042}\u{E0043}` 覆盖了三条路径：覆盖模板（`system_prompt_override`）、附加段（`prompt_extras`）、扩展说明（`extensions_info`）。

**净化的三个位置都是「外部可控文本」的入口。**

```
let sanitized_override_prompt = sanitize_unicode_tags(override_prompt);
let extras = extras.into_iter().map(|(key, extra)| (key, sanitize_unicode_tags(&extra))).collect();
```

加上扩展说明，恰好是三处：用户/项目提供的覆盖模板、操作贡献的附加段、扩展提供的使用说明。这不是漏掉了 `hints`——`hints` 是通过 `extras.insert("hints", hints)` 进入附加段的，因此在同一个 `map` 里被净化。也就是说**净化发生在追加之后、拼装之前**，而不是在各来源分别做。

有一个测试专门保住了「正常 Unicode 不受影响」：

```
let legitimate_unicode = "Instruction with 世界 and 🌍 emojis";
assert!(result.contains("世界")); assert!(result.contains("🌍"));
```

这是净化这类操作的必备测试——去掉标签字符与保留全部非 ASCII 之间只有一线之隔，一个写坏的正则会把中文与 emoji 一起删掉。

**模板渲染分两条路：覆盖模板走字符串，内置模板走注册表。**

```
let base_is_override = self.system_prompt_override.is_some();
let rendered = if let Some(override_prompt) = &self.system_prompt_override {
    prompt_template::render_string(&sanitized_override_prompt, &context)
} else {
    prompt_template::render_template("system.md", &context)
}.map_err(|error| anyhow::anyhow!("Could not render system prompt: {error}"))?;
```

`render_string` 直接渲染一段字符串，`render_template` 还会做「注册表检查 + 用户覆盖文件优先」那套逻辑（见 b04）。两者的错误被统一包成 `Could not render system prompt: {error}`——用户看到的是一句带原因的话，而不是裸的 minijinja 错误。

`base_is_override` 这个布尔被**保留下来并暴露出去**，而不是用完就丢。它回答的是「这份提示的基础部分是用户自定的还是内置的」，下游据此决定是否还要追加某些默认内容。

**`SystemPromptParts::join()` 定义了附加段的连接格式。**

```
pub fn join(&self) -> String {
    if self.extras.is_empty() { return self.rendered.clone(); }
    format!("{}\n\n# Additional Instructions:\n\n{}", self.rendered,
            self.extras.values().cloned().collect::<Vec<_>>().join("\n\n"))
}
```

三点值得记：**空附加段时完全不加标题**（不是多一个空章节）；连接用两个换行（`\n\n`），即空行分隔；附加段的**顺序**就是 `IndexMap` 的插入顺序（`extras` 是 `IndexMap`，不是 `HashMap`）。

用 `IndexMap` 而不是 `HashMap` 不是巧合——这个顺序决定最终提示的字节，而字节决定缓存。一个 `HashMap` 在这里会让同一份逻辑产生不同的字符串，缓存命中率就随机化了。有一个测试专门验证键的顺序被保留：

```
assert_eq!(parts.extras.keys().map(String::as_str).collect::<Vec<_>>(), ["recipe", "project"]);
```

**`hints` 被放进附加段，而不是基础模板。**

```
if let Some(hints) = self.hints {
    extras.insert("hints".to_string(), hints);
}
```

也就是说项目级的提示文件（`AGENTS.md` 一类）是**追加**在系统提示之后的，而不是嵌在 `system.md` 里。这个决定有明确好处：基础部分保持稳定（利于缓存），项目相关的内容集中在「Additional Instructions」下的一个块里。

`with_hints` 的构造也很讲究：

```
pub fn with_hints(mut self, working_dir: &Path) -> Self {
    let hints_filenames = get_context_filenames();
    let ignore_patterns = build_gitignore(working_dir);
    let hints = load_hint_files(working_dir, &hints_filenames, &ignore_patterns);
    if !hints.is_empty() { self.hints = Some(hints); }
    self
}
```

它同时传入「要读哪些文件名」与「按什么 gitignore 规则忽略」。后者不是可选的便利，而是**安全边界**：有一个测试 `project_git_metadata_does_not_reach_system_prompt` 构造了一个 `.git/config` 里带 `oauth2:PROMPT_SECRET` 的仓库，`AGENTS.md` 通过 `@.git/config` 引用它，然后断言提示里**不含那个 secret**、但含 `docs/config.md` 的合法内容。也就是说 `.git` 被 gitignore 规则挡掉了，而 `docs/` 下的文件正常引入。

**`build_system_prompt_parts` 的组合方式透露了「谁提供什么」。**

```
pub fn build_system_prompt_parts(&self, session: &Session, prompt_parts: Vec<(String, String)>, goose_mode: GooseMode) -> Result<SystemPromptParts> {
    self.builder()
        .with_session(session)
        .with_prompt_extras(prompt_parts)
        .with_hints(&session.working_dir)
        .with_goose_mode(goose_mode)
        .without_extensions()
        .build_parts()
}
```

注意最后那个 `without_extensions()`：这条路径**不**带扩展说明。这与 builder 的默认值 `include_extensions: true` 形成对照——说明「带扩展的完整提示」是另一条路径（`builder()` 手工加 `with_extension`），而这条是「操作贡献 + 会话覆盖 + hints」的组合。`include_extensions` 之所以是模板变量而不是真正的过滤，从 `SystemPromptContext` 里可以看到它被原样传给模板——模板自己决定要不要渲染扩展章节。

有一个测试正好验证这条路径的效果：

```
assert!(prompt.contains("## developer"));
assert!(!prompt.contains("No extensions are defined"));
```

即「提示附加段里提供了 extensions 内容时，模板不该再输出『没有扩展』的默认话术」。

**builder 的字段与它刻意不持有的东西。**

```
pub struct SystemPromptBuilder<'a, M> {
    manager: &'a M,
    system_prompt_override: Option<String>,
    extensions_info: Vec<ExtensionInfo>,
    prompt_extras: IndexMap<String, String>,
    hints: Option<String>,
    code_execution_mode: bool,
    include_extensions: bool,
    goose_mode: Option<GooseMode>,
}
```

`<'a, M>` 的泛型参数 `M` 目前只被 `PromptManager` 实例化——但泛型化本身说明「builder 借用一个 manager 拿时间戳，而不是复制它」。`goose_mode: Option<GooseMode>` 是三态：显式设了用设的，没设则回退到全局配置：

```
let goose_mode = self.goose_mode.unwrap_or_else(|| Config::global().get_goose_mode().unwrap_or_default());
```

两层回退（显式 → 全局 → 默认值）让「调用方不关心 mode」与「策略由配置决定」两种情形都不需要额外代码。

## 关键取舍

**源码事实：** 时间戳被截断到小时（`%H:00`）并在 `PromptManager` 构造时取一次，理由写在注释里：「截断到小时，以平衡用户时间准确性与多会话提示缓存命中」。

**源码事实：** 扩展说明在组装时按 `name` 排序，注释明确归因为「多会话提示缓存」；`extras` 用 `IndexMap` 而非 `HashMap`，因为插入顺序即最终字节顺序。

**源码事实：** `sanitize_unicode_tags` 作用在三处外部可控文本：覆盖模板、附加段（含 `hints`）、扩展说明；净化发生在追加之后、拼装之前。另有测试保证中文与 emoji 等正常 Unicode 不被误删。

**源码事实：** `hints` 走 `build_gitignore` 的忽略规则，有测试断言项目 `.git/config` 里的凭据串不会进入提示。

**源码事实：** `join()` 在附加段为空时不输出 `# Additional Instructions:` 标题；非空时以两个换行连接。

**源码事实：** `base_is_override` 被保留并对外暴露，回答「基础部分是用户自定还是内置」。

**源码事实：** `build_system_prompt_parts` 调用 `without_extensions()`，而 builder 默认为 `include_extensions: true`——两条路径的扩展处理不同。

**源码事实：** `goose_mode` 是两层回退（显式 → 全局配置 → 默认值），而 `is_autonomous` 是从它派生而非独立配置。

**架构推论：** 「让提示字节稳定」在 agent 框架里是一项持续性的工程负担，而不是一次性的格式化工作。这里用三处（时间粗化、扩展排序、有序 extras）分别挡住三种漂移源：时间、注册顺序、map 哈希。任何一处失守，缓存命中率都会退化成「偶尔命中」。

**架构推论：** 把「基础部分」与「附加段」分成两个值而不是拼完就丢，使下游能分别利用：基础部分参与缓存前缀，附加段可以按需省略或重排。若只有一个 String，这类优化无从下手。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/agents/prompt_manager.rs` | `PromptManager` | 持有粗粒度时间戳，提供两个组装入口 |
| `crates/goose/src/agents/prompt_manager.rs` | `SystemPromptBuilder` | 逐步收集覆盖模板、扩展说明、附加段、hints、mode |
| `crates/goose/src/agents/prompt_manager.rs` | `SystemPromptParts` | 基础部分 + 附加段的二分，含 `join()` 的连接格式 |
| `crates/goose/src/agents/prompt_manager.rs` | `SystemPromptContext` | 模板变量集合，含派生的 `is_autonomous` 与可选 moim 块 |
| `crates/goose/src/agents/prompt_manager.rs` | `build_parts` | 排序、净化、渲染、追加 hints 的唯一实现点 |
| `crates/goose/src/agents/prompt_manager.rs` | `with_hints` | 读取项目提示文件，同时传入 gitignore 忽略规则 |
| `crates/goose/src/utils/…` | `sanitize_unicode_tags` | 去除不可见标签字符，三处外部文本共用 |

## 参考

- [goose 源码：agents/prompt_manager.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/prompt_manager.rs)
- [goose 文档：系统提示与配置](https://block.github.io/goose/docs/guides/config-file/)
- [Unicode 规范：Tags 区块（U+E0000–U+E007F）](https://www.unicode.org/charts/PDF/UE0000.pdf)
- [Anthropic 文档：提示缓存](https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching)

## 与主核 / AI 的关系

「为了缓存命中而刻意让输出稳定」与主核里「为让执行计划可复用而做规范化」是同一种思路：主核把等价的查询规范成同一个文本（常量折叠、谓词重排），使计划缓存能命中；这里把等价的提示规范成同一段字节（排序、粗化时间、固定顺序）。共同点是**规范化必须发生在生成之后、使用之前**，而且规范化的目标不是「更好看」，而是「更可比较」。

对 AI 工程的实际意义有三条。第一，**时间戳是提示缓存的头号杀手**。任何把「现在」写进系统提示的框架，都在每次请求上放弃整份缓存。截断到小时是这组取舍里性价比最高的一档：用户仍能看到大致时间，而同一小时内的全部请求共享前缀。

第二，**不可见字符是提示注入的现实载体**。`sanitize_unicode_tags` 只清一个特定区块，而不是「全部非 ASCII」——这个精确度很重要，因为误删中文的代价远高于漏掉某个注入向量。而三个净化点都用同一函数、同一时机（追加之后、拼装之前），使「哪些文本被净化」这个问题有唯一答案。

第三，**项目提示文件的读取必须带忽略规则**。那个 `.git/config` 的测试是一条真实风险的证明：一个看起来无害的 `@` 引用就能把仓库凭据拖进系统提示并发送给模型。把 `build_gitignore` 与文件读取绑在同一个构造方法里（而不是留给调用方自己配），是把安全边界做成了默认行为。
