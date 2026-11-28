---
project: open-interpreter
feature: model-info
title: 模型信息与基础指令
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/models-manager/src/model_info.rs
    symbols: [BASE_INSTRUCTIONS, model_info_from_slug, strip_personality_section]
  - path: codex-rs/models-manager/src/model_info.rs
    symbols: [with_config_overrides, PERSONALITY_SECTION_HEADER]
---

# 模型信息与基础指令

## 一句话职责

把「系统提示」当成模型元数据的一部分：`BASE_INSTRUCTIONS` 编译期嵌入二进制，未知模型得到一份保守的默认元数据，而配置覆盖层能在不改模板的情况下「删掉人格段落」或「整段替换」。

## 机制是什么

`BASE_INSTRUCTIONS: &str = include_str!("../prompt.md")` —— 275 行的系统提示在**编译期**被嵌进二进制。`PERSONALITY_SECTION_HEADER = "# Personality"` 是那段可被删除的小节的一级标题。

`with_config_overrides(model, config)` 是配置覆盖的入口，五段独立的条件修改。`model_info_from_slug(slug)` 是未知模型的兜底构造器。`strip_personality_section(instructions)` 是那个「按一级标题切出一节并删掉」的文本手术。

## 怎么实现的

**`include_str!` 而不是运行时读取。** 这个选择决定了三件事：

1. 提示词与代码是**同一个版本**——不存在「二进制是新的、prompt.md 是旧的」这种部署错配。
2. 提示词会进 Bazel 的编译数据依赖（这也是仓库 AGENTS.md 里那条「若你加了 `include_str!` 就要更新 `BUILD.bazel` 的 `compile_data`」规则的来由）。
3. 修改提示词需要重新编译——代价是迭代慢，收益是不可篡改。

**五个覆盖段，顺序与依赖都值得看。** `with_config_overrides` 里的五段：

1. `model_supports_reasoning_summaries == Some(true)` → 把 `supports_reasoning_summaries` 置真。注意是 `Some(true)` 而不是「非 None」——`Some(false)` 不做任何事，所以它是「只能打开、不能关闭」的开关。
2. `model_context_window` 有值 → `context_window = Some(min(配置值, max_context_window))`。这里 `max_context_window.map_or(context_window, |max| context_window.min(max))` 的写法表达了一个约束：用户设的窗口不得超过模型声明的最大值。若 `max_context_window` 是 `None` 就直接用配置值。
3. `model_auto_compact_token_limit` 有值 → 直接赋值。
4. `tool_output_token_limit` 有值 → 按 `truncation_policy.mode` 分两支重建策略：`Bytes` 模式把 token 数换算成字节数（`approx_bytes_for_tokens`）、`Tokens` 模式直接用。两支都用 `i64::try_from(...).unwrap_or(i64::MAX)` 兜住溢出。
5. `base_instructions` 与 `personality` 的互斥分支——见下。

第 2 段是唯一一处「不覆盖而是钳制」的字段。它做的事是防御性的：若不钳制，一个过大的配置值会让请求被后端拒绝（因为超过模型真实窗口）。钳制让配置变成「上限内的偏好」而非「绝对命令」。

**第 5 段的三分支结构。** 它是这一页的核心：

```rust
if let Some(base_instructions) = &config.base_instructions {
    let model_messages = model.model_messages.get_or_insert_default();
    model_messages.instructions_template = Some(base_instructions.clone());
    model_messages.instructions_variables = None;
} else if config.personality == Some(Personality::None)
    && let Some(instructions_template) = model.model_messages.as_mut()
        .and_then(|messages| messages.instructions_template.as_mut())
{
    *instructions_template = strip_personality_section(std::mem::take(instructions_template));
}
```

第一支：有 `base_instructions` → **整段替换**模板，并把 `instructions_variables` 置 `None`。

那个 `instructions_variables = None` 是个细节但重要：替换模板意味着旧模板的变量占位也失效了，留着它们可能让渲染时找不到对应的值或填错。所以「换模板」与「清变量」必须一起做。

第二支：没有 `base_instructions`，但 `personality == Some(Personality::None)` → **删掉 `# Personality` 那一节**。

两个分支是 `if / else if`，所以「显式替换模板」优先于「删人格段落」——你不可能既替换模板又删旧模板里的一节，那没有意义。

注意第二支用了 `std::mem::take(instructions_template)` ——把原字符串取走（留下空串），再传给 `strip_personality_section` 返回新字符串后写回。这样避免了 clone：`strip_personality_section` 的签名是 `mut instructions: String -> String`，所以它按值接收再返回值。

**`strip_personality_section` 是「按行扫描 + 字节偏移记录」。** 它维护 `section_start` 与 `section_end` 两个 `Option<usize>` 以及 `offset`：

- `split_inclusive('\n')` 保留换行符，这样 `offset += line_with_ending.len()` 能精确累计字节位置。
- 判断行内容前先剥掉 `\n`，再剥掉可能残留的 `\r` ——所以 CRLF 文件也能正确比较。
- `section_start` 未定时：`line == PERSONALITY_SECTION_HEADER` 就记 `section_start = offset`。
- `section_start` 已定后：遇到任意 H1 就记 `section_end = offset` 并 `break`。

所以「这一节的结束」是「下一个一级标题的开始」，而不是「下一个二级标题」。这个判据的正确性来自 prompt.md 的实际结构：`# Personality` 与 `# AGENTS.md spec` 都是 H1，`## Personality` 内部的子节是 H2/H3。

循环结束后：

```rust
if let Some(section_start) = section_start {
    let section_end = section_end.unwrap_or(instructions.len());
    instructions.replace_range(section_start..section_end, "");
}
```

`unwrap_or(instructions.len())` 处理「人格段是最后一节」的情况——那时没有后续 H1，删到文件末尾。

`replace_range` 用字节范围，所以 `section_start` 与 `section_end` 必须落在字符边界上。这个保证来自 `offset` 的累加方式：它是 `line_with_ending.len()` 的和，而 `split_inclusive` 的每片都是完整 UTF-8 子串，所以累计出来的每个 `offset` 都是字符边界。

这是一个「用结构保证而非用断言保证」的细节：没有 `is_char_boundary` 检查，但也不会 panic，因为起点与终点都来自同一套按行累加。

**`is_h1_heading` 的宽容。** 实现：

```rust
let Some(rest) = line.strip_prefix('#') else { return false; };
rest.is_empty() || rest.starts_with(' ') || rest.starts_with('\t')
```

所以 `# `、`#\t`、以及单独一个 `#` 都算 H1；而 `##` 不算（因为 `strip_prefix` 之后还剩 `#`，不满足三个条件之一）。这个判据比严格的 Markdown 解析宽松，但它只需要「区分一级与更深层标题」这一个功能，所以足够。

**`model_info_from_slug` 的关键是那个 warn 与 `used_fallback_model_metadata: true`。** 函数一进来就：

```rust
warn!("Unknown model {slug} is used. This will use fallback model metadata.");
```

所以「用未知模型名」不是错误，而是一次**可观测的降级**。这条 warn 是这层唯一的告警出口，它让「用户打错模型名」这件事在日志里可见。

返回的默认值里有几个值得注意的选择：

- `visibility: ModelVisibility::None` —— 不在列表里展示。所以未知模型不会出现在模型选择器里，但可以被显式指名使用。
- `priority: 99` —— 优先级最低。
- `supported_in_api: true` —— 允许通过 API 用。
- `truncation_policy: bytes(10_000)` —— 保守的输出截断。
- `context_window` 与 `max_context_window` 都是 `Some(272_000)`。
- `effective_context_window_percent: 95` —— 只用 95% 的窗口，给估算误差留余量。
- `used_fallback_model_metadata: true` —— 这一位直接告诉下游「你拿到的是兜底数据」。
- `model_messages: Some(local_model_messages())` —— **仍然给出一份系统提示**（`BASE_INSTRUCTIONS`）。
- 大量 `*_support` 与 `*_instructions` 布尔都是 `false`，即不启用任何可选能力。

所以兜底策略是「功能全关、但能跑」：一个未知模型名仍然会得到完整的系统提示与基本的 272k 窗口，只是所有可选特性都不启用。这比「拒绝未知模型」宽容，比「当作已知模型处理」安全。

`used_fallback_model_metadata` 这一位的存在说明下游会**分支**：某些逻辑对兜底元数据要保守处理（例如不启用依赖精确元数据的特性）。这与 `warn` 是两个层次的信号——一个是给人看的，一个是给代码看的。

**`local_model_messages` 只有一行有效赋值。** 它构造 `ModelMessages { instructions_template: Some(BASE_INSTRUCTIONS.to_string()), ..Default::default() }`。所以未知模型的系统提示就是内置的那份——不是空、不是占位文本。

## 关键取舍

**源码事实：** `BASE_INSTRUCTIONS` 用 `include_str!("../prompt.md")`，编译期嵌入。

**源码事实：** `supports_reasoning_summaries` 只在配置为 `Some(true)` 时被打开，`Some(false)` 不做任何事。

**源码事实：** `context_window` 的配置值会被 `max_context_window` 钳制（取较小者）。

**源码事实：** `base_instructions` 覆盖模板时同时把 `instructions_variables` 置 `None`。

**源码事实：** `personality: None` 的删除动作只在「没有 `base_instructions`」时执行。

**源码事实：** `strip_personality_section` 以「下一个 H1」为节结束判据，没有后续 H1 时删到文件末尾。

**源码事实：** 未知 slug 会打一条 warn、返回功能全关但可用的元数据，并置 `used_fallback_model_metadata: true`。

**架构推论：** 「系统提示属于模型元数据」这个建模是把提示词工程纳入可配置面的关键一步。若提示是硬编码在推理路径里，那么「不同模型用不同提示」这件事就只能靠 `if model == "x"` 的分支实现。放进 `ModelInfo` 之后，每个模型条目都能携带自己的模板，覆盖层也能按配置改。代价是提示词成了一个数据字段——它的正确性不再由代码保证，而由数据保证。

**架构推论：** 「删一节」而不是「留一个开关让渲染时跳过」这个做法，看起来粗一些，但避免了整条渲染链都要感知「人格是否启用」。渲染链只需要处理「模板字符串」，而「要不要人格」在生成模板时就已经决定完了。代价是原模板信息丢失（无法再恢复那一段），但这在「模板是编译期常量」的前提下不构成问题。

**架构推论：** 「兜底元数据 + 一个布尔标记」比「兜底元数据」更完整。任何降级都要同时给出「降级后的数据」与「这是降级数据」的标记，否则下游无法区分「这个模型真的不支持 X」与「我们不知道它支不支持 X」。前者可以安全地禁用功能，后者必须更保守。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/models-manager/src/model_info.rs` | `BASE_INSTRUCTIONS` | 编译期嵌入的 275 行系统提示 |
| `codex-rs/models-manager/src/model_info.rs` | `PERSONALITY_SECTION_HEADER` | `# Personality` 一级标题常量 |
| `codex-rs/models-manager/src/model_info.rs` | `with_config_overrides` | 五段配置覆盖，含模板替换与人格删除 |
| `codex-rs/models-manager/src/model_info.rs` | `strip_personality_section` | 按行扫描、以 H1 为界的删除 |
| `codex-rs/models-manager/src/model_info.rs` | `is_h1_heading` | 宽容的一级标题判定 |
| `codex-rs/models-manager/src/model_info.rs` | `model_info_from_slug` | 未知模型的保守兜底元数据 |
| `codex-rs/models-manager/src/model_info.rs` | `local_model_messages` | 兜底时仍给出内置系统提示 |
| `codex-rs/models-manager/src/model_info.rs` | `used_fallback_model_metadata` 赋值 | 机器可读的降级标记 |

## 参考

- [open-interpreter 源码：models-manager/src/model_info.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/models-manager/src/model_info.rs)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [AGENTS.md 规范](https://agents.md/)

## 与主核 / AI 的关系

主核里最接近的是「表定义的默认值与未知类型的处理」：一个未知列类型不能直接拒绝（那样任何新版本写入的数据都读不了），而是给出一份保守的默认描述并标记「这是推断的」。`model_info_from_slug` 与这个模式一致，连「标记降级」的做法都一样。

与 goose 的 g02（模型配置回落）对照：那边是「未知配置项 → 用内置默认值 → 再用远端目录填补」，多层回落但没有降级标记；这里只有一个回落层，但明确标出 `used_fallback_model_metadata`。两者的差别在于失败的可观测性要求：goose 的目录是可选增强，这里的模型元数据直接影响能力判定。

与 smolagents 的模型层相比，那边的系统提示完全在调用方手里（`system_prompt` 参数），框架不带默认提示。这里自带 275 行提示并在未知模型时照样使用——因为一个 CLI agent 的提示词包含大量行为约定（工具使用、审批流程、AGENTS.md 语义），不是调用方能随便替换的。

对 AI 工程最实用的一条是「提示词按一级标题分段以便程序化删减」。`strip_personality_section` 之所以能工作，是因为模板里有一个明确的一级标题 `# Personality`，且后续节都是同为 H1。若提示词用二级标题表示顶级分区，这段代码就得改成「找 H2 但避开代码块里的 H2」——复杂得多。**给提示词定一份可机器识别的结构**（顶级分区一律 H1），是让「按功能开关裁剪提示」这件事可行且低成本的前提。

另一条是「覆盖模板时清掉变量」。`instructions_variables = None` 这一行看起来是清理，实际是防一类静默错误：新模板不含某些占位，但旧的变量表还在，渲染时可能出现「多余变量被忽略」或「缺少变量渲染成空」。任何「替换模板」的接口都该同时定义「旧变量怎么办」，而最安全的选择是「一起换掉」。

最后一条是「钳制而不是覆盖」。`context_window` 的取值被 `max_context_window` 限制住在物理上限内。配置项越是「看起来可以任意设」，越需要一个物理上界来兜住；否则用户设了一个后端的非法值，失败会出现在很远的地方（一次请求报错），而原因很难回溯到那个配置项。
