---
project: goose
feature: injection-scanner
title: 提示注入扫描
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/security/scanner.rs
    symbols: [PromptInjectionScanner, ScanResult, ClassifierType]
  - path: crates/goose/src/security/scanner.rs
    symbols: [is_shell_tool_name, DetailedScanResult]
---

# 提示注入扫描

## 一句话职责

对**即将执行的 shell 命令**与**最近的用户消息**做两路并行打分，把两组分数按一套带阈值的规则合成一个最终置信度，再与可配置阈值比较，得出「是否恶意」。规则匹配与机器学习分类器各提供一路信号，谁高取谁，且规则命中有下限保护。

## 机制是什么

这个扫描器要回答的问题不是「这条命令像不像恶意」，而是「**这条命令加上这段会话上下文，整体有多可疑**」。所以它同时看两个来源：

| 来源 | 看什么 | 用什么 | 上限 |
|---|---|---|---|
| 命令本身 | 工具调用里的命令文本 | 正则模式 + Command 分类器 | 无 |
| 会话上下文 | 最近的用户消息 | Prompt 分类器 | 最近 10 条 |

两路信号的性质完全不同：命令文本是**单一、确定、可被规则捕获**的；用户消息是**多轮、模糊、只能靠分类器**的。所以它们用不同的检测手段，也按不同的方式聚合——命令取「max(规则, 分类器)」，上下文的 10 条消息取**最大值**（一条可疑就够了）。

最终分数不是简单相加，而是经过一段**分档规则**的合成。那段规则是这一页最值得读的部分，因为它编码了几种「两路信号该怎么互相影响」的判断。

## 怎么实现的

**只有 shell 被扫描，其余工具直接短路。** `analyze_tool_call_with_context` 的第一件事：

```rust
if !is_shell_tool_name(tool_call.name.as_ref()) {
    return Ok(ScanResult {
        is_malicious: false, confidence: 0.0,
        explanation: "Tool call skipped: only shell commands are scanned".to_string(),
        scanned: false,
    });
}
```

而 `is_shell_tool_name` 只匹配一个字面量：

```rust
fn is_shell_tool_name(name: &str) -> bool { matches!(name, "shell") }
```

这两处合起来是一个**明确的取舍**：注入检测目前只覆盖 shell 工具。理由可以从检测手段反推——正则模式集（`patterns.rs` 里三十余条）全部是针对命令行语法的（`rm -rf /`、`curl | bash`、`nc -l`、`base64 --decode | sh`），对非命令行工具的参数字段无从下手。分类器虽然是通用的，但它的名字叫 **Command** classifier——训练目标就是命令行。

`scanned: false` 这个字段值得单记。它把「没扫」与「扫了但干净」区分开。若压成一个 `is_malicious: false`，上层无法区分「这条命令通过检查」与「这条命令根本没被检查」。这对 d01 讲的「处置」很关键：`scanned: false` 意味着这个结论**不该被当作放行依据**。

**抽取待检文本的优先级。** `extract_tool_content` 先找 `arguments.command`：

```rust
if let Some(cmd_str) = tool_call.arguments.as_ref()
    .and_then(|args| args.get("command")).and_then(|v| v.as_str()) {
    return cmd_str.to_string();
}
let mut s = format!("Tool: {}", tool_call.name);
if let Some(args) = &tool_call.arguments {
    if let Ok(json) = serde_json::to_string(args) { s.push('\n'); s.push_str(&json); }
}
```

有 `command` 就**只**看 `command`；没有就退化成「工具名 + 全参数 JSON」。第一支是常态（shell 工具的参数就一个 `command`），第二支是兜底。

这个兜底行有个副作用：`build_explanation` 里做预览时用 `split_once('\n')` 取第二行之后，正是为了在兜底形态下跳过 `Tool: xxx` 这一行、只展示参数 JSON。两处配合起来说明兜底形态是**预期会用到**的，不是理论分支。

**两路并行，一次 `join`。** 主流程：

```rust
let (tool_result, context_result) = tokio::join!(
    self.analyze_text(&tool_content),
    self.scan_conversation(messages)
);
```

`tokio::join!` 而不是 `spawn` + await：两者都在当前任务里并发推进，不需要 `'static`，也不需要错误传播的额外包装。两路各自可能出错，`?` 逐个传播。

**命令路：max(规则, 分类器)。**

```rust
let (pattern_confidence, pattern_matches) = self.pattern_based_scanning(text);
let ml_confidence = if let Some(classifier) = self.command_classifier.as_ref() {
    self.scan_with_classifier(text, classifier, ClassifierType::Command).await
} else { None };
Ok(DetailedScanResult {
    confidence: ml_confidence.map_or(pattern_confidence, |ml| ml.max(pattern_confidence)),
    ...
})
```

即「谁高取谁」。这是个**并集**语义——规则命中 `rm -rf /`（Critical，置信度 0.95）而分类器只给 0.4，结论仍是 0.95；分类器给 0.9 而规则没命中，结论是 0.9。

**规则置信度由风险等级映射，四档固定值。**

```rust
Critical => 0.95, High => 0.75, Medium => 0.60, Low => 0.45
```

`pattern_based_scanning` 用 `get_max_risk_level(&matches).map_or(0.0, |r| r.confidence_score())`——只取**最高风险**的那一条，不对多条取和。这是一个重要的选择：若按命中条数累加，一条写得很啰嗦的命令可能因为撞上多个低危模式而超过阈值，而实际上它只是包含了几个常见字符串。

模式集本身在 `patterns.rs`，共三十余条，编译时统一加 `(?i)` 前缀：

```rust
if let Ok(regex) = Regex::new(&format!("(?i){}", threat.pattern)) {
    patterns.insert(threat.name, regex);
}
```

注意这是**逐个编译并静默跳过编译失败的**——一条写错的正则会让对应威胁安静地失效。`LazyLock` 保证只编译一次。

`scan_for_patterns` 对每条模式跑 `find_iter` 收集**所有**命中位置，然后按「风险等级降序、位置升序」排序。保留全部命中（而不是只留第一条）是为了给解释文本提供素材：`build_explanation` 取 `max_by_key(risk_level)` 得到最能说服用户的那一条。

**上下文路：最近 10 条用户消息，取最大分。** `extract_user_messages` 的实现有三处判断：

```rust
messages.iter().rev()
    .filter(|m| effective_role(m) == EffectiveRole::User && !m.is_turn_context())
    .take(limit)
    .map(...只取 Text 内容，用 \n 拼接...)
    .filter(|s| !s.is_empty())
```

- **`.rev()` 然后 `.take(limit)`**：从最新往回取 10 条。想抓的是「最近在让 agent 干什么」，越近越相关。
- **`effective_role`** 而不是 `m.role`：用「有效角色」判断，说明消息的角色在会话演进中可能被改写（例如工具结果被包装成用户消息）。用 effective_role 才能拿到真实的说话人。
- **`!m.is_turn_context()`**：跳过轮次上下文这类注入的辅助消息。这类消息由框架生成（不是用户输入），扫它们等于扫系统自己的话术。

然后并发跑分类器：

```rust
stream::iter(user_messages)
    .map(|msg| async move { self.scan_with_classifier(&msg, classifier, ClassifierType::Prompt).await })
    .buffer_unordered(ML_SCAN_CONCURRENCY)
    .fold((0.0_f32, false), |(acc, scanned), result| async move {
        match result {
            Some(confidence) => (confidence.max(acc), true),
            None => (acc, scanned),
        }
    })
```

`buffer_unordered(3)` 限制并发为 3——这是一个**对远端分类器的礼貌上限**（10 条消息若全并发可能触发限流）。`fold` 取最大值，并单独记录「是否至少有一条真的被扫过」，用来决定 `ml_confidence` 是 `Some` 还是 `None`：

```rust
ml_confidence: any_scanned.then_some(max_confidence)
```

这个 `.then_some` 很关键：若所有消息的分类都失败，`ml_confidence` 是 `None` 而不是 `Some(0.0)`。`0.0` 会被合成逻辑当成「上下文很干净」，而 `None` 表示「没有上下文信号」。两者的差别在后面那段分档规则里会放大。

注意 `scan_conversation` 在两种情况下返回**全零且 `used_pattern_detection: false`** 的空结果：没有 Prompt 分类器、或没有用户消息。这两种「没有信号」在结构上被表达成「没有规则检测参与」，与命令路的 `used_pattern_detection: true` 形成对照。

**分类器失败不抛错，只降级成 `None`。**

```rust
match classifier.classify(text).await {
    Ok(conf) => Some(conf),
    Err(e) => { tracing::warn!("{} classifier scan failed: {:#}", type_name, e); None }
}
```

即分类器不可用时，命令路退回纯规则分，上下文路退回 `None`。整体是**fail-open**：少了 ML 信号但仍可能被规则拦住。这与 d04（对手模型）的 fail-open 是同一种取向，但理由不同——这里的规则路还在，而 d04 里没有第二路。

**`combine_confidences` 是四档的分段函数，每一档都是一条明确的判断。**

```rust
let Some(context_confidence) = context_confidence else { return tool_confidence; };

if tool_confidence < 0.3 { return tool_confidence; }
if context_confidence < 0.3 { return tool_confidence * 0.9; }
if tool_confidence > 0.8 && context_confidence > 0.8 {
    let max_conf = tool_confidence.max(context_confidence);
    return (max_conf * 1.05).min(1.0);
}
tool_confidence * 0.8 + context_confidence * 0.2
```

逐档读：

1. **没有上下文信号** → 直接用命令分。这条让「只有一路信号」不会因为缺另一路而被惩罚。
2. **命令分 < 0.3 → 直接用命令分，上下文完全不影响**。注释写的是「If tool is safe, context is not taken into account」。这是一个**单向否决**：命令本身很干净时，就算上下文可疑也不抬高总分。理由是上下文信号太容易误报（用户可能只是粘贴了一份提到 `curl` 的日志），若让它单独定罪，误拦率会很高。
3. **上下文分 < 0.3 → 命令分打 0.9 折**。反方向的影响对称但更轻：上下文明显干净时稍微降一点分。为什么降？因为上下文干净是「agent 在干正事」的证据，能略微压低误报。
4. **两路都 > 0.8 → 取最大值再 ×1.05（封顶 1.0）**。这是一个**协同加分**：两路独立都认为可疑时，总分允许略高于任一单路。这是「两个独立证据互相印证」的贝叶斯直觉的手工近似。
5. **其余 → 0.8 命令 + 0.2 上下文**。加权平均，且**命令是主信号**（权重 4:1）。

这五档合起来的效果是：**上下文信号只能在命令已经有一定可疑度时才起作用**。它要么略微降低分数、要么在双高时加分、要么在中间区间贡献 20% 权重；但绝不可能单独把一个干净的命令推过阈值。这条设计直接回应了「上下文信号误报率高」这个现实。

`final_confidence` 还多做一步：

```rust
let final_confidence = self.combine_confidences(tool_result.confidence, context_result.ml_confidence)
    .max(tool_result.pattern_confidence);
```

**`.max(pattern_confidence)` 是下限保护**：合成结果无论被加权平均或打折降到多少，都不能低于原始规则命中的分数。也就是说规则命中 `rm -rf /`（0.95）后，即使上下文说「这很干净」（打 0.9 折 → 0.855），最终仍是 0.95。这防止了「上下文信号把高危规则的命中稀释掉」——一个真实存在的风险：任何加权平均都会让高分被低分拉低。

**阈值与判定。**

```rust
pub fn get_threshold_from_config(&self) -> f32 {
    Config::global().get_param::<f64>("SECURITY_PROMPT_THRESHOLD").unwrap_or(0.8) as f32
}
```

默认 0.8。注意它与规则置信度的映射有一处巧妙的对应：**Critical = 0.95 与 High = 0.75 分别落在默认阈值的两侧**。即默认配置下「Critical 命中一定拦」、「High 命中一定不拦（0.75 < 0.8）」。这不是巧合——`High` 的模式（如 `password_cracking_tools`）本就是「可疑但不该自动拦截」的一档。调阈值就等于在「拦 High」与「放 High」之间划线。

判定是 `is_malicious: final_confidence >= threshold`——**闭区间**，恰好等于阈值即算恶意。

**两个分类器是独立配置、独立存在的。** `ClassifierType` 是个只有两个变体的 `Copy` 枚举，`create_classifier` 用它的名字拼出配置键前缀：

```rust
let prefix = match classifier_type {
    ClassifierType::Command => "COMMAND",
    ClassifierType::Prompt => "PROMPT",
};
```

于是四组配置键：`SECURITY_{PREFIX}_CLASSIFIER_ENABLED`、`_MODEL`、`_ENDPOINT`、`_TOKEN`。三处细节：

- **Command 的启用开关多一层 override**：`get_override("SECURITY_COMMAND_CLASSIFIER_ENABLED_OVERRIDE")` 优先于配置值。Prompt 没有这一层。这是一个刻意的非对称——Command classifier 可能在测试或特定运行里被强制开关，Prompt 不需要。
- **解析顺序是「模型名 → 端点+token → 旧的 model_type」**。前三者任一成功即可；Command 还有第三档 `from_model_type("command", None)` 作为兼容旧配置的退路。
- **全部失败就 `bail!`**，但 `with_ml_detection` 把两个 `create_classifier` 的 `Result` 都 `.ok()` 掉了，只在**两个都失败**时才 `bail!`：

```rust
let command_classifier = Self::create_classifier(ClassifierType::Command).ok();
let prompt_classifier = Self::create_classifier(ClassifierType::Prompt).ok();
if command_classifier.is_none() && prompt_classifier.is_none() {
    anyhow::bail!("ML detection enabled but no classifiers could be initialized");
}
```

也就是说**只有一路分类器可用也照常工作**：单路时另一路走 `None` 分支，合成逻辑自动适配（没有上下文信号就直接用命令分）。这个容错让「只配了 Command 分类器」成为一种合法部署。

**解释文本要挑最能说服人的那条。** `build_explanation` 的逻辑可归成三层：

- 分数低于阈值 → `"No security threats detected"`。
- 有「决定性」的规则命中（`pattern_confidence >= threshold`）→ 用 `max_by_key(risk_level)` 取最高危的那条，格式化成 `Pattern-based detection: {description} (Risk: {level})\nFound: '{截断的命中文本}'\n\nCommand:\n{预览}`；若同时分类器也过阈值，末尾追加一句 `Classifier detection confidence: {:.1}%`。
- 否则若有分类器分 → `Security threat detected (confidence: {:.1}%)`；否则有规则命中 → 用最高危那条；否则只有命令预览。

一处细节：`decisive_ml` 与 `decisive_pattern_match` 都是**按阈值过滤后**才用的。低分的命中不会写进解释——解释要回答「为什么拦」，而不是「扫到了什么」。

**扫描结果同时写结构化日志。** 主流程里有一段 `tracing::info!`，带一组字段：

```rust
security.event_type = "prompt_injection_scan",
security.confidence = final_confidence,
security.threshold = threshold,
security.above_threshold = final_confidence >= threshold,
scanner.tool_confidence = tool_result.confidence,
scanner.context_confidence = ?context_result.ml_confidence,
scanner.used_command_ml = tool_result.ml_confidence.is_some(),
scanner.used_prompt_ml = context_result.ml_confidence.is_some(),
scanner.used_pattern_detection = tool_result.used_pattern_detection,
```

值得注意 `?context_result.ml_confidence`：可选的 `Option<f32>` 在日志里用 `?` 记成「缺失」而不是 `0.0`。这一点与代码里「`None` 不等于 0」的坚持一致——**日志是事后调阈值时的唯一依据，它必须能区分「信号为零」与「没有信号」**。

## 关键取舍

**源码事实：** `is_shell_tool_name` 只匹配 `"shell"`，其余工具直接返回 `scanned: false` 的干净结果，把「没扫」与「扫了干净」区分开。

**源码事实：** 规则置信度由风险等级映射成四个固定值（0.95 / 0.75 / 0.60 / 0.45），只取命中中的最高风险，不按条数累加。

**源码事实：** `combine_confidences` 是五档分段：无上下文信号即用命令分、命令分低于 0.3 时上下文完全不参与、上下文低于 0.3 时命令分打 0.9 折、双高时取最大值 ×1.05（封顶 1.0）、其余按 0.8/0.2 加权。

**源码事实：** 合成结果再与原始规则分取 `max`，保证高危规则命中不会被上下文信号稀释。

**源码事实：** 默认阈值 0.8 恰好把 `Critical`（0.95，拦）与 `High`（0.75，不拦）分开。

**源码事实：** 上下文侧只取最近 10 条有效用户消息（跳过轮次上下文、只取文本），并发上限 3，取最大值；全部扫描失败时 `ml_confidence` 为 `None` 而不是 `0.0`。

**源码事实：** 两个分类器独立配置、独立解析；只有两者都初始化失败才整体报错，单路可用即继续。

**源码事实：** 分类器调用出错只 warning 并降级为 `None`，整体 fail-open（规则路仍然生效）。

**架构推论：** 这一段分档规则的实质是**给两路信号定了不等的主权**。命令是主证据（权重 4:1、单独可定罪、不被削弱），上下文是辅助证据（只能修饰、不能单独定罪）。这个不对称不是技术限制，而是对误报率的现实判断：用户粘进上下文的一段日志里出现 `curl` 不该导致拦截，但 `curl | bash` 出现在待执行命令里必须考虑拦截。

**架构推论：** `.max(pattern_confidence)` 这一步看似冗余（规则分已经参与了合成），实际上是整段逻辑里最能体现经验的一处。任何「加权/打折」的合成方式都会让高分被低分拉低，而规则命中是**确定性证据**——它不该被概率信号稀释。把「确定性证据」设为下界，是混合检测系统里通用的一条规则。

**架构推论：** 「只有 shell 被扫描」这个限制暴露了一个演进方向：整套机制是为命令行设计的，扩展到别的工具需要新的模式集与新的分类器。`ClassifierType` 只有两个变体，也说明目前只有「命令」与「提示」两类语义被建模。若将来要覆盖文件写入或数据库查询，这里必须扩展而不是微调。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/security/scanner.rs` | `PromptInjectionScanner` | 扫描器：两路并行、分类器持有、阈值读取 |
| `crates/goose/src/security/scanner.rs` | `analyze_tool_call_with_context` | 主流程：shell 短路、并行、合成、判定、日志 |
| `crates/goose/src/security/scanner.rs` | `ScanResult` | 对外结果：`is_malicious` / `confidence` / `explanation` / `scanned` |
| `crates/goose/src/security/scanner.rs` | `DetailedScanResult` | 内部中间结果：规则分、ML 分、命中列表、是否用了规则 |
| `crates/goose/src/security/scanner.rs` | `ClassifierType` | `Command` / `Prompt` 两变体，决定配置键前缀与检测目标 |
| `crates/goose/src/security/scanner.rs` | `is_shell_tool_name` | 只认 `shell` 的工具名过滤 |
| `crates/goose/src/security/scanner.rs` | `combine_confidences` | 五档分段合成规则 |
| `crates/goose/src/security/scanner.rs` | `analyze_text` | 命令路：max(规则, Command 分类器) |
| `crates/goose/src/security/scanner.rs` | `scan_conversation` | 上下文路：最近 10 条用户消息、并发 3、取最大 |
| `crates/goose/src/security/scanner.rs` | `build_explanation` | 按阈值过滤后挑最高危规则生成解释 |
| `crates/goose/src/security/scanner.rs` | `extract_user_messages` / `extract_tool_content` | 待检文本抽取 |

## 参考

- [goose 源码：security/scanner.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/security/scanner.rs)
- [goose 源码：security/patterns.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/security/patterns.rs)
- [goose 文档：安全](https://block.github.io/goose/docs/guides/security/)
- [OWASP：LLM01 提示注入](https://genai.owasp.org/llmrisk/llm01-prompt-injection/)
- [Rust 文档：tokio::join!](https://docs.rs/tokio/latest/tokio/macro.join.html)

## 与主核 / AI 的关系

「确定性证据是概率证据的下界」这条，与主核里的**约束与统计的混合决策**同源。主核的规划器在估算成本时会用统计信息（概率性的），但一旦存在「必须满足」的硬约束（唯一索引、外键），那个判断就是确定的，不会被成本估计稀释。这里的 `.max(pattern_confidence)` 是同一件事：规则命中是硬证据，分类器分数是软估计，合成时前者设下界。

「多路信号不等权」在数据库里对应的是**代价模型的多因子加权**：顺序扫描与索引扫描各自有代价，加权系数不是等分的，而是按各因子的可靠性定。这里 0.8/0.2 的权重同样表达了「命令文本比会话上下文更可靠」这一先验。

`scanned: false` 这个字段的价值可以对照主核的**统计信息缺失标记**。主核在缺少统计信息时不会假装「行数为 0」，而是标记「未知」并让规划器走保守路径。这里把「没扫」表达成 `scanned: false` 而不是 `confidence: 0.0`，是同一种诚实——`0.0` 会被上游读成「查过了，很干净」。

对 AI 工程的实际意义在于：提示注入检测的工程难点不是算法，而是**误报与漏报的非对称代价**。漏报意味着一次恶意命令被执行；误报意味着用户被无谓地打断，而长期的误报会训练用户「无脑点通过」，最终让所有安全提示失效。这份实现给出的三条经验可直接用：**确定性规则命中不能被概率信号拉低**、**上下文信号不能单独定罪**、以及**默认阈值要落在两个风险档次之间**（0.8 恰好把 Critical 与 High 分开，于是「High 只是可疑」这个判断被编码进数字而不是散落在代码里）。
