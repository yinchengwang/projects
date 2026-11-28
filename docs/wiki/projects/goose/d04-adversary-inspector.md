---
project: goose
feature: adversary-inspector
title: 对手模拟与配置
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/security/adversary_inspector.rs
    symbols: [AdversaryInspector, AdversaryConfig]
  - path: crates/goose/src/security/patterns.rs
    symbols: [ThreatPattern, PatternMatcher]
---

# 对手模拟与配置

## 一句话职责

装一个「反方」模型审主 agent 的工具调用：给它用户最初的任务、最近几条用户原话与用户自己写的规则，让它只回一个词 `ALLOW` 或 `BLOCK`。它由配置文件启用，配置格式极简，出错时一律放行。

## 机制是什么

前面几个审查器（d02 注入扫描、d03 出口观测）都是**在代码里写好规则**。这一位不同：它的策略**由用户写在配置文件里**，它的判定**由另一个模型来做**。

这条设计的动机写在这个类型的文档注释里：「protecting the user in case the other agent is rogue」。也就是换一个**视角**：主 agent 的上下文可能已经被污染，但对手模型拿到的是一份独立组装的简报——用户的原任务 + 用户的原话 + 用户写的规则。如果主 agent 被注入操纵，它想做的危险操作会与「用户原本要什么」不一致，而这个不一致只有站在外面看才能发现。

所以它有三个来源：

| 输入 | 来自 | 作用 |
|---|---|---|
| `rules` | `adversary.md` 的正文 | 用户定义的 BLOCK/ALLOW 判据 |
| 原任务 | 会话里第一条用户消息 | 「用户到底要什么」 |
| 近期原话 | 最近 4 条用户消息 | 「用户最近在强调什么」 |
| 待审调用 | 工具名 + 参数 JSON | 待判定的对象 |

这个审查器的**启用**也是一条设计：靠一个文件的存在。没有 `adversary.md` 就完全不参与检查链（`is_enabled()` 返回假），连一次模型调用都不发生。

## 怎么实现的

**配置只有一个可选前置段。** `parse_adversary_md` 的解析规则可以完整地写成一张表：

| 输入形态 | `tools` | `rules` |
|---|---|---|
| 空内容或全空白 | 默认 `["shell"]` | `DEFAULT_RULES` |
| 有 `\n---` 分隔，分隔前有 `tools:` 行 | 该行列出的工具 | 分隔后的文本 |
| 有 `\n---` 分隔，分隔前无 `tools:` | 默认 `["shell"]` | 分隔后的文本 |
| 有 `\n---` 分隔，分隔后为空 | 由有无 `tools:` 决定 | `DEFAULT_RULES` |
| 无分隔 | 默认 `["shell"]` | **整个内容都是规则** |

三处细节：

- **分隔符是 `"\n---"` 而不是 `"---"`**。用 `split_once("\n---")` 意味着 `---` 必须自成一行开头（前面有换行）。这样文件里别处出现的 `---`（例如规则文本里的分隔线）不会被误当成分隔符。
- **`tools:` 行在「分隔前」的任意位置**都会被找到（`for line in frontmatter.lines()`），不要求是第一行。
- **规则里的 `tools:` 不会被误读**，因为扫描范围只限分隔符之前的部分。

四种边界情况都有测试固定：`test_parse_with_tools_frontmatter`、`test_parse_without_frontmatter`、`test_parse_empty`、`test_parse_frontmatter_empty_rules_uses_defaults`。最后一条测的是「只写了 `tools: shell` 和一个分隔符，规则为空」——此时规则回落到 `DEFAULT_RULES`，而不是变成空字符串。这条很关键：**空规则会让模型无所适从**，而 `DEFAULT_RULES` 里有一段「Err on the side of ALLOW」的兜底指示。

**默认规则本身是一份值得读的判据。** `DEFAULT_RULES` 列了五条 BLOCK 与一段 ALLOW：

```
BLOCK if the command:
- Exfiltrates data (curl/wget posting to unknown URLs, piping secrets out)
- Is destructive beyond the project scope (rm -rf /, modifying system files)
- Installs malware or runs obfuscated code
- Attempts to escalate privileges unnecessarily
- Downloads and executes untrusted remote scripts

ALLOW if the command is a normal development operation, even if it modifies files,
installs packages, runs tests, uses git, etc. Most commands are fine.
Err on the side of ALLOW — only block truly dangerous things.
```

最后那句「Err on the side of ALLOW」是关键。它解释了为什么这个审查器**判错的方向被刻意指向放行**：一个过于严苛的对手模型会把正常开发流程全部堵死（agent 连 `npm install` 都不敢跑），而这比偶尔漏掉一个危险命令更影响可用性。

**默认只审 `shell`。** `DEFAULT_TOOLS` 是 `&["shell"]`。文件里写了 `tools:` 才扩展。这份名单是**精确匹配**的：

```rust
config.tools.iter().any(|t| t == tool_name)
```

不是前缀或后缀匹配——所以要审 `developer__shell` 必须显式写出来（示例文档里正是这么写的：`tools: shell, developer__shell`）。这与 d03 出口审查器那套「精确 + 后缀」的宽松匹配形成对照：那里是为了尽量覆盖，这里的默认姿态是**尽量少审**。

**加载一次性完成，且读失败也启用。** `get_config` 用 `OnceLock<Option<AdversaryConfig>>`：

```rust
self.config.get_or_init(|| {
    let path = self.config_path.clone().unwrap_or_else(|| Paths::config_dir().join("adversary.md"));
    if !path.exists() { ... return None; }   // 文件不存在 -> 禁用
    let content = match std::fs::read_to_string(&path) {
        Ok(c) => c,
        Err(e) => {
            tracing::warn!("Failed to read adversary.md: {}", e);
            return Some(AdversaryConfig { tools: DEFAULT_TOOLS..., rules: DEFAULT_RULES... });
        }
    };
    ...
})
```

三处判断值得辨清：

- **文件不存在 → `None`（禁用）**。这是唯一的禁用路径。
- **文件存在但读失败（权限、编码等）→ 启用并使用默认规则**。这是一个**反直觉但刻意**的选择：读不了内容时仍启用，用默认规则兜底。理由是「文件存在」本身就表达了用户的意图（我要开启对手审查），而读失败是技术故障，不该因此放弃整层保护。
- **`OnceLock`**：配置只读一次，进程生命周期内不再重读。所以**编辑 `adversary.md` 后必须重启**才能生效。这是性能（每次工具调用都读文件不合理）与灵活性之间的取舍。

`with_config_dir` 是给测试用的构造器：把配置路径显式指定，而不是从 `Paths::config_dir()` 派生。对应测试 `test_disabled_when_no_adversary_md` 用它指向一个空临时目录，断言 `is_enabled()` 为假且 `inspect` 返回空结果。

**`is_enabled()` 直接委托给 `get_config().is_some()`。** 这意味着「是否启用」与「配置是否可加载」是同一件事，没有第二个开关。这个设计让检查链可以在调用前就问「你要不要参与」，避免为一个禁用的审查器付出任何代价。

**简报的组装顺序是这一页最值得看的部分。** `consult_llm` 构造的 user 消息由五段拼成：

```
The user's original task: {original_task}

{history_section}
The tool call to review:
```
{tool_description}
```

{rules}

Respond with a single word on the first line: ALLOW or BLOCK
Then on the next line, a brief reason.
```

逐段看：

1. **原任务在最前**。它是判定的基准——「这条命令服务于用户要的目标吗」。
2. **近期用户原话**（`history_section`），编号列出、每条截断到 200 字符：

```rust
s.push_str(&format!("{}. {}\n", i + 1, truncated));
```

3. **待审调用**用代码块包起来。这一步不只是排版：代码块的边界让模型能清楚区分「要审的东西」与「判据」——这是把不可信内容**明确标界**的做法（对比 c01 用的 `UNTRUSTED TOOL REQUEST DATA (JSON):` 前缀）。
4. **规则**放在最后。位置有意义：规则是「怎么判」，放在待审内容之后，模型读到规则时已经看过被审的东西。
5. **输出格式**明确要求：第一行一个词，第二行理由。

`format_tool_call` 生成的 `tool_description` 是 `Tool: {name}\nArguments: {pretty JSON}`。用 `to_string_pretty` 而不是紧凑格式，是为了让嵌套参数对模型更易读。

**原任务与近期原话用的是两套抽取逻辑，差别在方向。**

```rust
fn extract_original_task(messages: &[Message]) -> String {
    for msg in messages {                          // 正序
        if msg.role == User && !msg.is_turn_context() {
            let text = ...;
            if !text.is_empty() { return safe_truncate(&text, 500); }
        }
    }
    "(unknown)".to_string()
}
```

- **正序遍历、取第一条非空**：原任务是**会话里最早的**那条用户消息，不是最近的那条。
- **无则回落 `"(unknown)"`**：一条显式的占位符，而不是空串。这样简报里那句话始终有内容，模型不会看到 `The user's original task: ` 后面什么都没有。
- **截断到 500 字符**：原任务可能很长（用户可能贴了一大段需求），而判定只需要其主旨。

而 `extract_recent_user_messages` 是反序取最近 N 条**再反回来**（`into_iter().rev().collect()`），所以最终顺序是「旧 → 新」，与 `history_section` 的注释一致：

```rust
let mut s = String::from("Recent user messages (oldest first):\n");
```

这个双重反转是为了让模型按时间顺序读，而不是倒着读。编号 `1.`、`2.`… 配合这个顺序，读起来像一份递进的对话摘要。

**两者都跳过 `is_turn_context()`。** 有测试 `user_context_extraction_skips_turn_context_events` 专门固定这条——它构造一段夹着三条 turn context 的对话，断言抽取结果里没有任何 turn context 文本，且原任务是那条 `never delete files outside the repo`。这个测试的名字也点出了理由：turn context 是框架注入的辅助消息，不是用户在说话。

顺带注意：这里用的是 `m.role == User`，而 d02 的注入扫描用的是 `effective_role(m) == EffectiveRole::User`。两处对「用户消息」的定义不同——扫描器用有效角色（可能包含被改写的角色），对手审查器用字面角色。这个差异会让两个审查器看到的「用户消息」不完全一致，但它对各自的目标都是合理的：扫描器要尽量覆盖，审查器只需真实用户输入的文本。

**`safe_truncate` 被用在三处，各有不同上限。**

| 位置 | 上限 | 为什么 |
|---|---|---|
| 原任务 | 500 | 只保留主旨 |
| 单条近期消息 | 200 | 四条加起来不超过 800 |
| 展示用（结果里） | — | 结果里不截断理由 |

三处都用 `safe_truncate` 而不是 `truncate`：「safe」意味着不切坏 UTF-8 边界。用户消息里只要有中文或 emoji 就必须这样——按字节截断会 panic 或产生替换字符。

**响应解析：找 `BLOCK`，其余一律 `ALLOW`。**

```rust
let upper = output.to_uppercase();
if upper.starts_with("BLOCK") || upper.contains("\nBLOCK") {
    // 取第二行起作为理由
    let reason = ...; let reason = if reason.is_empty() { "Blocked by adversary" } else { reason };
    Ok((false, reason))
} else {
    // 同样取第二行起作为理由
    Ok((true, reason))
}
```

五处判断：

1. **两个位置检查**：`starts_with("BLOCK")` 覆盖正常回答；`contains("\nBLOCK")` 覆盖模型多说一句开场的情况（「I reviewed this. \nBLOCK because…」）。
2. **整体 `to_uppercase()`**：大小写不敏感。模型回 `Block`、`block` 都能判。
3. **只认 `BLOCK` 这一个词**：`ALLOW` 不需要检查——**没找到 BLOCK 就是 ALLOW**。这是又一次 fail-open 的具体形态：任何输出（包括模型回了一段无关的话）都会被当作放行。
4. **理由取第二行起**。`skip(1)` 之后全部行 join 成一行。若为空则用 `"Blocked by adversary"`（仅 BLOCK 分支有此兜底）——ALLOW 分支允许理由为空。
5. **不解析结构化输出**。要求模型回两行纯文本而不是 JSON，是为了降低「模型输出格式不合规」的概率。用 JSON 的话还得处理解析失败，而这里解析失败根本不会发生（任何文本都能被解析成「ALLOW + 理由」）。

**三态结果共用同一套动作，区别在 `action` 与 `confidence`。**

| 分支 | action | confidence | reason 前缀 |
|---|---|---|---|
| `Ok((true, _))` | `Allow` | `1.0` | `Adversary: {理由}` |
| `Ok((false, _))` | `Deny` | `1.0` | `🛡️ Adversary blocked: {理由}` |
| `Err(e)` | `Allow` | `0.0` | `Adversary error (fail-open): {e}` |

三处值得说：

- **BLOCK 用 `Deny` 而不是 `RequireApproval`**。这是四个审查器里唯一一个直接拒绝的：安全审查器给 `RequireApproval`（让用户决定）、权限审查器给 `Deny`/`RequireApproval`、出口只观测。对手模型既然是「独立的复核者」，它的 BLOCK 被当作终局判断。这个选择更激进——用户没有机会推翻。
- **错误分支 `confidence: 0.0`**，与成功分支的 `1.0` 形成鲜明对比。这样上层若按置信度加权或排序，能分辨「这是模型真的判了」还是「我们根本没拿到判定」。
- **日志级别不同**：ALLOW 用 `tracing::debug!`，BLOCK 与错误用 `tracing::warn!`。这样默认日志级别下能看到拦截与故障，而正常的放行不刷屏。

**错误处理里有一处「没有 provider 也算成功」。**

```rust
let Ok(session) = self.session_manager.get_session(session_id, false).await else {
    return Ok((true, "No provider available".to_string()));
};
let Ok(provider) = self.providers.provider_for(&session).await else {
    return Ok((true, "No provider available".to_string()));
};
```

两条失败都返回 `Ok((true, ...))`——即**伪装成「判定为允许」**而不是返回 `Err`。于是这两个分支会走到 `Ok((true, reason))` 那一支，动作 `Allow`、**置信度 `1.0`**、理由 `Adversary: No provider available`。

这是一个可以商量的地方：真实故障被当成了「判定结果」，且置信度被标为 1.0。相比之下，第三处失败（provider 调用本身失败）走的是 `Err`，得到 `confidence: 0.0`。两处失败的处理不一致——一个伪装成高置信放行，一个诚实标为不确定。

不过从可用性看，这个选择是可以解释的：**若没有 provider 就报错，整个工具调用链可能被阻断**（取决于上游如何处理 `Err`）。对手审查是一个可选增强，它不该因为自己拿不到模型而让整个 agent 停摆。但代价是「没 provider」这件事在结果里以 `confidence: 1.0` 出现，容易被误读。

**`inspect` 的四件事顺序。** 拿到配置（禁用则早退）→ 抽原任务 → 抽近期消息 → 逐个请求过滤与复核。「抽原任务与近期消息」在循环**之外**，因为这两个量对所有请求都一样。这是明显的优化：不必为每个请求重算一次会话摘要。

而 **`should_review` 的过滤在循环内、模型调用之前**：

```rust
if !Self::should_review(config, request) { continue; }
```

所以未列入 `tools:` 的工具完全不产生结果（`results` 里没有它），而不是产生一个 `Allow`。这个差别对 d01 讲的「权限基线 + 其它审查器覆盖」很关键：不产结果意味着「本审查器对此无意见」，产出一个 `Allow` 则可能被理解为「本审查器认为安全」。

## 关键取舍

**源码事实：** 启用靠 `adversary.md` 的存在；文件不存在 → 禁用；文件存在但读失败 → 启用并使用默认规则。配置由 `OnceLock` 只读一次，改配置需重启。

**源码事实：** 默认只审精确匹配的 `shell`，要审其它工具必须在 `tools:` 里显式列出（含 `extension__tool` 形式）。未列入的工具不产生任何检查结果。

**源码事实：** `parse_adversary_md` 用 `\n---`（要求 `---` 自成行）作为前置段分隔；前置段里任意位置的 `tools:` 行都会被识别；分隔后为空则回落到默认规则。

**源码事实：** `DEFAULT_RULES` 末句是「Err on the side of ALLOW — only block truly dangerous things」，把误判方向明确指向放行。

**源码事实：** 简报顺序是「原任务 → 近期用户原话（旧到新、编号、各截 200 字）→ 待审调用（代码块包住）→ 规则 → 输出格式要求」；原任务取会话中最早一条非空用户消息（截 500 字），无则 `"(unknown)"`。

**源码事实：** 两者都跳过 turn context 消息；原任务按字面 `role == User` 判断，而注入扫描器用 `effective_role`。

**源码事实：** 响应解析只找 `BLOCK`（首行或任意 `\n` 之后），其余一律 ALLOW；理由取第二行起，BLOCK 且理由为空时用 `"Blocked by adversary"`。

**源码事实：** BLOCK 产出 `Deny`（终局拒绝，不是「要求批准」）；错误产出 `Allow` 且 `confidence: 0.0`；「没有 provider」两条失败则伪装成 `Allow` 且 `confidence: 1.0`。

**架构推论：** 这个审查器的定位是**用一个独立组装的简报做交叉验证**。它的价值不在于「比规则更聪明」，而在于它看到的东西与主 agent 不同——主 agent 有完整上下文（可能被污染），对手只看到用户原话与规则。这种「视角隔离」是它唯一无法被注入替代的能力。

**架构推论：** 三处 fail-open（解析只认 BLOCK、错误放行、无 provider 放行）都指向同一个判断：**对手审查是可选增强，不该成为可用性的单点**。代价是它的保护是「尽力而为」的——用户在配置里写下 `BLOCK if…` 并不能保证一定拦得住。这个取舍必须由用户知情，`adversary.md` 的存在即是知情的形式。

**架构推论：** 要求模型回两行纯文本而不是 JSON，是「降低接口脆弱性」的选择。结构化输出在审计场景里看起来更专业，但它引入了解析失败这一整类故障，而这类故障在 fail-open 系统里会静默地变成放行。两行文本的解析器几乎是全函数（任何输入都有输出），于是「解析失败」这一类故障被消除了。

**架构推论：** 一处值得指出的不一致：`Deny` 让对手审查成了四个审查器里唯一不需要用户确认就能拦下的。这在「保护用户」的名义下是合理的，但也意味着一个判错的对手模型会让正常操作无法执行、且用户无从推翻。把它改成 `RequireApproval` 并保留模型的理由，可能是更平衡的选择——不过这需要权衡 `Deny` 带来的「自动化流程不被中途打断」这一好处。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/security/adversary_inspector.rs` | `AdversaryInspector` | 对手审查器：配置加载、简报组装、模型调用、结果映射 |
| `crates/goose/src/security/adversary_inspector.rs` | `AdversaryConfig` | 解析后的配置：审查工具名单 + 规则文本 |
| `crates/goose/src/security/adversary_inspector.rs` | `parse_adversary_md` | 可选前置段的解析与四种边界回落 |
| `crates/goose/src/security/adversary_inspector.rs` | `get_config` / `is_enabled` | 一次性加载、启用判定、读失败仍启用 |
| `crates/goose/src/security/adversary_inspector.rs` | `should_review` | 精确工具名匹配过滤 |
| `crates/goose/src/security/adversary_inspector.rs` | `consult_llm` | 简报组装、系统提示、响应解析与理由提取 |
| `crates/goose/src/security/adversary_inspector.rs` | `extract_original_task` | 取最早一条非空用户消息，截 500 字，无则 `"(unknown)"` |
| `crates/goose/src/security/adversary_inspector.rs` | `extract_recent_user_messages` | 反序取最近 N 条后反回，保持旧到新 |
| `crates/goose/src/security/adversary_inspector.rs` | `format_tool_call` | 工具名 + pretty JSON 参数的待审描述 |
| `crates/goose/src/security/patterns.rs` | `ThreatPattern` | 一条威胁模式的形状：名字、正则、描述、风险级、类别 |
| `crates/goose/src/security/patterns.rs` | `PatternMatcher` | 编译后的模式表与扫描、取最高风险、判断是否有高危命中 |

## 参考

- [goose 源码：security/adversary_inspector.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/security/adversary_inspector.rs)
- [goose 源码：security/patterns.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/security/patterns.rs)
- [goose 文档：安全](https://block.github.io/goose/docs/guides/security/)
- [OWASP：LLM01 提示注入](https://genai.owasp.org/llmrisk/llm01-prompt-injection/)

## 与主核 / AI 的关系

「引入一个视角不同的独立复核者」与主核里的**独立校验器**同构：主核在 WAL 与校验和的设计里就引入了一条独立路径——写入的数据由另一段代码算出校验值，读出时再独立校验。若是同一段代码算、同一段代码验，那么同一个 bug 会同时让两边出错，校验就失效了。对手模型的价值正在于它不共用主 agent 的上下文——**独立性而非更强的能力**。

「配置靠文件存在与否」这条与主核的配置文件习惯一致：某个功能开关往往不是配置里的一项布尔值，而是「这个文件在不在」。好处是零配置即默认安全（不装就不开），代价是排查时要记得看文件系统。

本页里那套「多种失败都放行」的取向，与主核的**可选组件**相似：一个可选的插件挂了不该让整个数据库起不来。但主核通常会**记录并暴露**这个故障（可观测且可查询），而这里「没有 provider」被伪装成高置信放行——这正是本页值得提出的批评点。更一致的做法是把 `confidence` 也标为 0.0 并保留原因，让「放过一次但故障可见」成为默认行为。

对 AI 工程的实际意义在于：单模型 agent 的最大弱点是「它自己的上下文就是它的全部世界观」。一旦上下文被污染，它无法靠自身察觉。对手模型是成本最低的补救手段——一次额外调用，不需要额外基础设施。这份实现给出的三条经验可直接用：**简报要显式强调用户的原任务**（那是判定的基准，而不是 agent 的自述）、**要求极简的输出格式**（一个词，减少解析失败这类故障）、以及**默认规则必须包含「宁可放行」的指示**（否则模型会过度拦截，把可用性拖垮）。
