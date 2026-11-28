---
project: goose
feature: prompt-template
title: 提示模板与转义
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/prompt_template.rs
    symbols: [render_template]
  - path: crates/goose/src/agents/moim.rs
    symbols: [compose_moim, escape_xml_text, turn_budget_part]
---

# 提示模板与转义

## 一句话职责

两处「把值放进提示文本」的实现：`render_template` 用 minijinja 渲染注册过的内置模板（用户可在配置目录覆盖），并附带一个自适应的 markdown 围栏过滤器；`compose_moim` 把每轮的运行上下文拼成一个 XML 标签块，其中所有值都过 `escape_xml_text`。

## 机制是什么

这两件事的共同点是「**模板文本与数据值必须是两个不同的东西**」，而它们的处理方式恰好相反：

| | `render_template` | `compose_moim` |
|---|---|---|
| 输入 | 模板字符串 + 序列化的上下文 | 值列表（工作目录、预算计数等） |
| 转义策略 | 由模板语法（`{{ }}`）自动转义 | 手工 `escape_xml_text` |
| 输出容器 | 任意文本（系统提示全文） | 固定的 XML 标签块 |

`prompt_template.rs` 的模板注册表有十个条目，分两类：

- **本仓自带**（`include_dir!("$CARGO_MANIFEST_DIR/src/prompts")` 编译进二进制）：`system.md`、`subagent_system.md`、`apps_create.md`、`apps_iterate.md`、`permission_judge.md`、`tiny_model_system.md`、`session_name.md`；
- **来自 oi-kit**：`compaction.md`、`compaction_summary.md`、`compaction_request.md`——`builtin_content` 在自带目录里找不到时会 `or_else(|| oi_kit_context_management::templates::builtin_template(name))`。

也就是说模板可以来自两个 crate，而查找顺序是「本仓优先，oi-kit 兜底」。这解释了为什么注册表里既有本仓的 `.md` 文件又有 `compaction` 系列——后者由 oi-kit 提供，本仓不复制一份。

`TEMPLATE_REGISTRY` 的每个条目除了文件名还有一句描述：

```
static TEMPLATE_REGISTRY: &[(&str, &str)] = &[
    ("system.md", "Main system prompt that defines goose's personality and behavior"),
    ...
];
```

描述不是装饰——`list_templates()` 会把它们填进 `Template.description` 供界面展示，而测试断言「每个模板的描述都非空」。于是这张表既是**白名单**（`is_registered` 用它决定能不能渲染/保存），也是**界面元数据**。

## 怎么实现的

**`render_template` 的优先级是「注册表检查 → 用户文件 → 内置内容」。**

```
pub fn render_template<T: Serialize>(name: &str, context: &T) -> Result<String, MiniJinjaError> {
    if !is_registered(name) {
        return Err(MiniJinjaError::new(minijinja::ErrorKind::TemplateNotFound,
                                         format!("Template '{}' is not registered", name)));
    }
    let user_path = user_prompts_dir().join(name);
    let template_str = if user_path.exists() {
        std::fs::read_to_string(&user_path).map_err(|e| { MiniJinjaError::new(InvalidOperation, format!("Failed to read user template: {}", e)) })?
    } else {
        builtin_content(name).ok_or_else(|| MiniJinjaError::new(TemplateNotFound, format!("Built-in template '{}' not found", name)))?
    };
    render_string(&template_str, context)
}
```

三层失败各有不同的错误种类：未注册是 `TemplateNotFound`（这个名字根本不存在），用户文件读失败是 `InvalidOperation`（文件在那儿但读不了），内置缺失是 `TemplateNotFound`（注册了但内容没有——理论上不该发生）。

**注册表检查放在最前，是一条安全边界**：`name` 是从调用方来的，若不做白名单检查，一个 `../../../etc/passwd` 之类的名字就会把任意文件读进提示。用户目录路径是 `Paths::config_dir().join("prompts")`，而只有注册过的名字才能拼进去。

`save_template` 与 `reset_template` 都先做同一个检查：

```
if !is_registered(name) {
    return Err(std::io::Error::new(std::io::ErrorKind::NotFound, format!("Template '{}' is not registered", name)));
}
```

于是「谁能被写」与「谁能被读」用同一张表——不存在「能覆盖一个未注册模板」的路径。

`reset_template` 的语义是「删掉用户那份」：`if path.exists() { remove_file(path) } else { Ok(()) }`。删完之后 `render_template` 自然回落到内置内容，不需要额外逻辑。这是「默认值就是内置文件」这条设计的直接后果——重置不需要「恢复」动作，只需要「移除覆盖」。

`template_source(name)` 是只读的版本：同样的两层查找，但不渲染。它的用途大概是「显示模板原文」，因此不需要上下文。

**`builtin_content` 的两级查找是一条「跨 crate 的默认值链」。**

```
fn builtin_content(name: &str) -> Option<String> {
    CORE_PROMPTS_DIR.get_file(name)
        .map(|file| String::from_utf8_lossy(file.contents()).to_string())
        .or_else(|| oi_kit_context_management::templates::builtin_template(name))
}
```

`include_dir!` 把 `src/prompts` 整个编进二进制，于是「内置模板」在发布后不可变、也不依赖运行时文件系统。代价是改模板要重新编译——而这正是 `user_content` 覆盖机制存在的理由。

**`render_string` 是所有渲染的唯一实现点，它的环境配置有三处。**

```
let mut env = Environment::new();
env.set_trim_blocks(true);
env.set_lstrip_blocks(true);
env.add_filter("code_fence", code_fence);
env.add_template("template", template_str)?;
let tmpl = env.get_template("template")?;
let ctx = MJValue::from_serialize(context);
let rendered = tmpl.render(ctx)?;
Ok(rendered.trim().to_string())
```

- `trim_blocks` + `lstrip_blocks`：模板里控制语句（`{% %}`）所在行不会留下空行。系统提示的排版对可读性敏感，而模板作者不该为「每个 `{% if %}` 后面手工删空行」操心。
- `add_filter("code_fence", code_fence)`：注册一个自定义过滤器（下段详述）。
- `render` 之后 `.trim()`：整份结果去掉首尾空白。于是模板文件末尾那个换行不会进入提示——这直接影响提示字节的稳定性（见 b02 的缓存讨论）。

minijinja 的 `{{ }}` 默认**不**做转义（`Environment::new()` 没设 auto-escape）。这个默认对 `.md` 模板是正确的——模板里写的是 markdown 与自然语言，HTML 转义会把 `<turn-context>` 这类标签变成 `&lt;turn-context&gt;`。所以这个文件**不依赖模板自动转义**，需要转义的地方都走 `escape_xml_text`。

**`code_fence` 是一个「围栏要比内容更长的围栏」实现。**

```
fn code_fence(code: String) -> String {
    let longest_run = code.chars().fold((0usize, 0usize), |(max, run), c| {
        if c == '`' { (max.max(run + 1), run + 1) } else { (max, 0) }
    }).0;
    let fence = "`".repeat((longest_run + 1).max(3));
    format!("{fence}\n{}\n{fence}", code.trim_end_matches('\n'))
}
```

它扫一遍内容，找出**最长的一段连续反引号**（那个 fold 的 `(max, run)` 状态机），然后围栏用「最长 + 1 根」反引号，且至少三根。

为什么需要：markdown 的围栏代码块要求闭合围栏至少与开启围栏一样长。若内容里已有一段 `` ``` ``，用一个三反引号围栏就会在那一段处**提前闭合**，后面的内容跑到围栏外面变成正文——一个把代码当指令的注入风险，也是一次排版事故。

两处细节：`.max(3)` 保证最短是三反引号（否则一行反引号围栏不是合法围栏）；`trim_end_matches('\n')` 去掉内容尾部换行，这样输出的 `{fence}\n{code}\n{fence}` 不会多一个空行。

注释把这个意图写得很明确：「用比它包含的任何反引号段都长的 markdown 围栏包裹代码，于是嵌入的围栏无法提前闭合块」。

**`compose_moim` 的骨架是固定的标签序列。**

```
fn compose_moim(working_dir: &Path, parts: Vec<String>, turn_start: DateTime<Local>) -> String {
    let timestamp = turn_start.format("%Y-%m-%d %H:%M:00 %:z");
    let mut lines = vec![
        open_tag(TURN_CONTEXT_TAG),
        tag(CURRENT_TIME_TAG, &timestamp.to_string()),
        tag(WORKING_DIRECTORY_TAG, &working_dir.display().to_string()),
    ];
    for part in parts {
        if !part.trim().is_empty() {
            lines.push(String::new());
            lines.push(part);
        }
    }
    lines.push(close_tag(TURN_CONTEXT_TAG));
    lines.join("\n")
}
```

四层结构：固定的开标签、两个内置字段（时间、工作目录）、可变的操作贡献段、固定的闭标签。

三处取舍值得记：

1. **时间戳格式带 `%H:%M:00`**——分钟精度、秒抹零。与 b02 里的系统提示时间戳（`%H:00`，小时精度）用途不同：这里是「这一轮什么时候开始的」，需要分钟级；而系统提示里那份只为让模型知道大致日期。
2. **空段被跳过且不占位**：`if !part.trim().is_empty()`，于是贡献了空字符串的操作不会在提示里留下空行。但非空段之前会插一个空行（`lines.push(String::new())`），形成段落分隔。
3. **贡献段被原样插入，不做转义**——因为它们已经是 XML 片段（`turn_budget_part` 就是）。只有**内置的两个字段**过 `escape_xml_text`。这是一个明确的责任划分：`compose_moim` 转义它自己生成的值，操作贡献的片段由操作自己负责。

**`escape_xml_text` 转义三个字符，顺序是承重的。**

```
fn escape_xml_text(value: &str) -> String {
    value.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;")
}
```

必须是 `&` 最先——如果先转 `<` 成 `&lt;`，随后那次 `&` 替换会把刚生成的实体里的 `&` 再转一次，得到 `&amp;lt;`，模型看到的是字面 `&lt;`。这是所有手写 XML/HTML 转义函数的第一条纪律，而这里顺序是对的。

为什么必须转义？因为这两个值来自不可信环境：`working_dir` 是用户给的路径（Windows 路径里可以有 `&`，Unix 路径里可以有 `<`），时间戳虽安全但走同一条 `tag()` 路径不特殊处理。若不转义，一个名为 `<turn-context>` 的目录就能提前闭合这个块，让后续内容跑到块外。

**`turn_budget_part` 的触发条件是「过半才显示」。**

```
fn turn_budget_part(turns_taken: u32, max_turns: u32) -> Option<String> {
    if max_turns == 0 || turns_taken.saturating_mul(2) < max_turns {
        return None;
    }
    Some(tag("turn-budget", &format!("{turns_taken}/{max_turns} used")))
}
```

`turns_taken * 2 < max_turns` 为真时不显示——即**只在用掉一半以上时才出现**。`max_turns == 0` 视为无限制、永不显示。`saturating_mul` 防溢（`u32` 的 `* 2` 在极端值下会 panic，而这里不需要那层精度）。

它的配套说明写在 `SYSTEM_PROMPT_BLOCK_TEMPLATE` 里，那段文本解释了为什么这个块存在：

> 当 `<turn-budget>` 出现时，把它当作「还剩多少自主工作」的信号。预算变低时应当更直接：减少探索、批量必要的工具调用、做合理假设、专注于完成用户的任务。

也就是说这个块的**行为含义**写在了系统提示里，而不是让模型自己猜「3/10 used」是什么意思。整个 `system_prompt_block()` 会随系统提示一起注入，并且可以被线程局部的 `SKIP` 开关关掉。

**`SKIP` 是一个线程局部的全局关闭开关。**

```
thread_local! {
    pub static SKIP: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}
```

三个函数都先查它：`system_prompt_block()`、`turn_context_message()`、`turn_context_event()`。用它实现的是一件很具体的事：**同一段提示既要进模型请求，又不能污染某些旁路调用**（判分、名称生成一类的辅助请求不该带上轮次上下文）。用线程局部而非参数，是因为这些辅助调用发生在调用栈深处，逐层传一个布尔会污染大量签名。

`turn_context_event` 还有第二个开关：

```
if SKIP.with(|f| f.get()) || should_skip_moim(context_limit) { return None; }
fn should_skip_moim(context_limit: Option<usize>) -> bool {
    context_limit.is_some_and(|limit| limit < MIN_CONTEXT_FOR_MOIM)
}
```

`MIN_CONTEXT_FOR_MOIM = 32_000`。上下文窗口太小的模型不注入这个块——省下来的 token 比这点上下文提示更有价值。这是一个**按模型能力裁剪**的判断，而不是节流。

**`turn_context_event` 的标记与去重。**

```
Message::user().with_text(compose_moim(working_dir, parts, turn_start))
    .with_metadata(MessageMetadata::agent_only().with_turn_context())
```

两个修饰都不是可选的：`agent_only()` 让这条消息**只对模型可见**（用户界面看不到一堆 XML 块），`with_turn_context()` 打上标记，供各处按需过滤。仓库里有六处测试名直接标出这个标记的用途——`messages_to_prompt_skips_turn_context_events`、`extract_user_messages_skips_turn_context_events`、`shell_history_skips_turn_context_events`、`user_context_extraction_skips_turn_context_events`。也就是说**同一个标记被六条不同的下游路径共用**：转提示、提取用户消息、shell 历史、安全扫描的用户上下文抽取等。

`inference_preparation` 里还有一处去重：

```
let last = turn.iter().rev().find(|message| message.is_turn_context()).map(Message::as_concat_text);
let additional_messages = moim::turn_context_event(&session.working_dir, context_limit, input.moim_parts, turn_start)
    .filter(|event| Some(event.as_concat_text()) != last)
    .into_iter().collect();
```

**同一轮里若这次的轮次上下文与上一条逐字相同，就不再追加。** 这直接减少了重复发送的 token，也避免了「同一轮里两次请求带两份几乎一样的上下文」这种噪声。`turn_start` 取自 kickoff 消息的时间戳（`turn.first().and_then(|m| from_timestamp(m.created, 0))`），于是同一轮内多次请求的时间戳也一致——去重才能真正命中。

**`moim` 这个名字与它的定位。**`compose_moim` 里的 `moim` 是「message of internal metadata」之类的缩写，而这个模块的实际角色是「每轮注入的操作上下文」。它与 `prompt_manager` 里的 `moim_system_prompt_block` 是成对的：一个解释这个块是什么（系统提示里），一个生成这个块（每条请求里）。有测试 `turn_context_event_is_marked_and_skips_small_contexts` 同时验证「带标记」与「小上下文被跳过」两条。

## 关键取舍

**源码事实：** 模板注册表有十个条目，既当白名单（决定能渲染/保存什么），也当界面元数据（`description`）。查找顺序是「用户配置目录文件 → 本仓内置 → oi-kit 内置」。

**源码事实：** 未注册的名字在读、写、重置三处都被拒；用户目录路径由 `Paths::config_dir().join("prompts")` 加注册名拼成，因此名白名单同时是路径安全边界。

**源码事实：** 渲染时开启 `trim_blocks` / `lstrip_blocks`，且渲染结果整体 `.trim()`——这对提示字节的稳定性有直接贡献。

**源码事实：** minijinja 环境没有开启自动转义，因此 `.md` 模板里的 `<turn-context>` 这类标签不会被破坏；需要转义的地方一律走 `escape_xml_text`。

**源码事实：** `code_fence` 扫描内容里最长的连续反引号段，围栏取「最长 + 1」且至少三根，注释明确说这是为了让嵌入的围栏无法提前闭合代码块。

**源码事实：** `escape_xml_text` 先转 `&` 再转 `<`、`>`；顺序保证不会产生 `&amp;lt;` 这类双重转义。

**源码事实：** `turn_budget_part` 只在用掉一半以上时才出现，`max_turns == 0` 永不出现，乘法用 `saturating_mul` 防溢。

**源码事实：** `SKIP` 是线程局部开关，`system_prompt_block` / `turn_context_message` / `turn_context_event` 都先查它；另有 `MIN_CONTEXT_FOR_MOIM = 32_000` 的按能力裁剪。

**源码事实：** 轮次上下文消息带 `agent_only()` 与 `with_turn_context()` 两个修饰，后者被至少六条下游路径用来过滤；`inference_preparation` 还会在「与上一条逐字相同」时不追加。

**架构推论：** 「模板可覆盖、内置不可变」这条组合让定制的收益与默认的稳定性同时成立：内置内容编进二进制保证发布后行为可预测，用户覆盖文件提供调整空间，而 `reset_template` 只需删文件即可回到默认。

**架构推论：** 手工转义与模板自动转义不混用，是这一页最重要的一致性。若 `.md` 模板开启了 auto-escape，所有刻意写的 XML 标签都会被破坏；若 `compose_moim` 依赖自动转义，它就不再是一个可以独立测试的纯函数。两者各守一边，边界清晰。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/prompt_template.rs` | `render_template` | 注册检查 → 用户文件 → 内置内容 → 渲染 |
| `crates/goose/src/prompt_template.rs` | `render_string` | 唯一渲染实现：trim 控制块、注册 `code_fence`、结果 trim |
| `crates/goose/src/prompt_template.rs` | `code_fence` | 自适应围栏长度，防嵌入反引号提前闭合 |
| `crates/goose/src/prompt_template.rs` | `TEMPLATE_REGISTRY` / `is_registered` | 十项白名单兼界面元数据，读/写/重置共用 |
| `crates/goose/src/prompt_template.rs` | `builtin_content` | 本仓内置目录 → oi-kit 内置模板的两级查找 |
| `crates/goose/src/agents/moim.rs` | `compose_moim` | 固定标签序列 + 内置字段转义 + 跳过空段 |
| `crates/goose/src/agents/moim.rs` | `escape_xml_text` | `&` → `<` → `>` 的顺序转义 |
| `crates/goose/src/agents/moim.rs` | `turn_budget_part` | 过半才显示预算块，`max_turns=0` 永不显示 |
| `crates/goose/src/agents/moim.rs` | `turn_context_event` | 组装消息并打 `agent_only` + `with_turn_context` 标记 |

## 参考

- [goose 源码：prompt_template.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/prompt_template.rs)
- [goose 源码：agents/moim.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/moim.rs)
- [minijinja 文档：环境配置与过滤器](https://docs.rs/minijinja/latest/minijinja/)
- [CommonMark 规范：围栏代码块](https://spec.commonmark.org/0.31.2/#fenced-code-blocks)

## 与主核 / AI 的关系

「模板可覆盖 + 内置不可变」在主核里对应的是「默认配置编进二进制 + 用户配置文件覆盖」：主核的内置参数保证升级后的行为可预测，而配置文件允许调优，重置就是删掉配置文件。这条组合的关键是**默认值必须存在且可用**，否则「重置后能不能跑」就取决于运气。

对 AI 工程的实际意义有三条。

第一，**围栏长度必须自适应**。任何「把用户或模型产出的文本包进定界符」的场景都有这个坑：定界符出现在内容里就能提前闭合。这里的解法（扫最长连续段 + 1）是通用且低成本的，比「禁用内容里的反引号」温和，也比「只加三根」可靠。

第二，**转义顺序不能错**。`&` 必须最先替换，否则后续生成的实体会被二次转义。这个错误的结果看起来很微妙——模型看到的文本里出现字面 `&lt;` 而不是 `<`——以至于很难被当成 bug 报告上来。

第三，**「同一轮内不重复注入相同上下文」是个值得单独实现的优化**。`inference_preparation` 用「与上一条逐字比较」来去重，而这个比较能成立的前提是时间戳取自轮次开始（同一轮内恒定）。如果时间戳用 `now()`，去重永远不会命中——两个设计必须配套。
