---
project: open-interpreter
feature: harness-model
title: Harness 模型与名称解析
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/tools/src/harness.rs
    symbols: [Harness, from_config_name]
  - path: codex-rs/tools/src/harness.rs
    symbols: [is_claude_code, is_kimi_code, is_minimal, is_swe_agent]
---

# Harness 模型与名称解析

## 一句话职责

`Harness` 是一个十五个已知变体加一个兜底 `Other(String)` 的枚举，`from_config_name` 把配置文件里那串 kebab-case 名字解析成变体，`is_*` 一族谓词把「种类判断」从散落的 `match` 收成命名方法——整个项目的「壳」概念只有这一个类型作为入口。

## 机制是什么

`Harness` 是这个 fork 里最基础的一个词。它回答的是「这次请求要按哪个 agent 命令行工具的形状发出去」，而不是「用哪个模型」。两个问题正交：同一个模型可以跑在任意一个壳下。

枚举的完整形状：

```
pub enum Harness {
    #[default] Native,
    ClaudeCode,
    ClaudeCodeBare,
    DeepSeekTui,
    KimiCode,
    KimiCli,
    ZCode,
    LittleCoder,
    MiniSweAgent,
    OpenCode,
    Pi,
    QwenCode,
    SweAgent,
    Terminus2,
    Minimal,
    Other(String),
}
```

四点值得先记：

1. **`Native` 是 `#[default]`**。缺省不是「某种复刻」，而是「用本项目自己的工具集」。于是「没配 harness」与「配了一个不存在的 harness」必须区分——前者是默认值，后者进 `Other`。
2. **`ClaudeCodeBare` 与 `ClaudeCode` 是两个变体而不是一个变体加参数**。bare 是「精简版」的另一个壳，它对工具集与提示的需求不同，所以要有自己的身份。
3. **`KimiCode` 与 `KimiCli` 同样是两个**。仓库里有一条测试专门守着这一点：`current_kimi_code_does_not_receive_legacy_kimi_cli_guidance` 断言 `KimiCode` 拿到 `None` 而 `KimiCli` 拿到那份指引。也就是说它们是**演化关系**（新壳不继承旧壳的补丁），而不是别名。
4. **`Other(String)` 让枚举保持开放**。用户写了一个本项目还不认识的壳名时，解析不失败、不退回 `Native`，而是保留原名——于是「未知壳」这件事在类型层面可表达，后续可以按名字处理而不是被静默吞掉。

`from_config_name` 的输入是 `Option<&str>`，不是 `&str`：配置里没有 `harness` 键与写了空串 `""` 都解析成 `Native`：

```
pub fn from_config_name(name: Option<&str>) -> Self {
    match name {
        None | Some("") => Self::Native,
        Some("claude-code") => Self::ClaudeCode,
        ...
        Some(other) => Self::Other(other.to_string()),
    }
}
```

把 `None` 与 `Some("")` 合并到同一个分支是一个具体的取舍：配置文件里 `harness = ""` 往往是「清空这个设置」的表达，应当等价于不设。若把它当成未知壳，`Other("")` 就会流进后续逻辑，而后续逻辑多半会用这个名字做字符串比较——一个空串会静默地匹配到任何「相等」的地方。

## 怎么实现的

**解析表是手写的，且必须与别处的定义表对齐。**

这里有两张独立的表，都描述同一批壳，但用途不同：

| 表 | 位置 | 用途 |
|---|---|---|
| `Harness::from_config_name` 的 `match` | `tools/src/harness.rs` | **解析**：名字 → 变体 |
| `HARNESS_DEFINITIONS` | `app-server/src/interpreter_catalog.rs` | **展示与可选性**：名字、界面标签、描述、支持的 wire API |

`HARNESS_DEFINITIONS` 的每一项是一个 `HarnessDefinition { id, label, description, wire_apis }`，`id` 用的是与 `from_config_name` 相同的 kebab-case 字符串（`""` 表 `Native`、`"claude-code"`、`"minimal"` 等）。两张表之间**没有编译期一致性检查**，靠的是一条测试：`interpreter_catalog_tests` 里断言

```
!matches!(Harness::from_config_name(Some(id)), Harness::Other(_))
```

即「目录里列出的每个 id 都必须能被解析成已知变体」。这条断言把「加了一个新壳却忘了在解析表里登记」变成一个可见的失败。

**`is_*` 谓词的存在理由可以从它们的实现看出来。**

```
pub fn is_claude_code(&self) -> bool {
    matches!(self, Self::ClaudeCode | Self::ClaudeCodeBare)
}
```

这是**唯一一个跨两个变体的谓词**。其余都是单变体判断，看上去几乎多余：

```
pub fn is_kimi_code(&self) -> bool { matches!(self, Self::KimiCode) }
pub fn is_minimal(&self) -> bool { matches!(self, Self::Minimal) }
pub fn is_swe_agent(&self) -> bool { matches!(self, Self::SweAgent) }
```

单变体谓词的价值不在「省多少代码」，而在**给调用点一个稳定的名字**。调用方写 `harness.is_minimal()` 而不是 `matches!(harness, Harness::Minimal)`，于是：

- 若将来 `Minimal` 裂成两个变体（比如 `Minimal` 与 `MinimalBare`），只需改谓词一处，所有调用点自动覆盖新变体；
- 若某个判断被删掉，`is_*` 方法也会随之消失，残留的调用点会编译报错，而不是变成一个永远为假的 `matches!`。

`is_claude_code` 已经在享受这个好处了——它内部就是两个变体的 `matches!`，而调用方只知道「这是 claude-code 家族」。

**「非 Native 就挂别名工具」这条判断用的是变体而不是谓词。**

在 `core/src/tools/spec_plan.rs` 的 `add_harness_alias_tools` 里：

```
let harness = Harness::from_config_name(context.turn_context.config.harness.as_deref());
if matches!(harness, Harness::Native) {
    return;
}
```

这里用的是 `matches!(harness, Harness::Native)` 而不是「某个 `is_native()`」。理由很实在：这个判断是「是否要进入别名工具分支」，其语义是「除了 Native 之外的所有壳（含 `Other`）都要挂别名」——一个直接的否定判断比一个正向谓词更贴近这个语义。

同一函数继续用谓词做细分：

```
if harness.is_claude_code() {
    registry.add(HarnessAliasHandler::Agent);
    registry.add_with_exposure(HarnessAliasHandler::TaskOutput, hidden);
    registry.add_with_exposure(HarnessAliasHandler::TaskStop, hidden);
}
registry.add(HarnessAliasHandler::Bash);
...
if matches!(harness, Harness::KimiCode) {
    registry.add_with_exposure(HarnessAliasHandler::Agent, hidden);
    ...
}
```

两层结构的意图清楚：**先给所有非 Native 壳挂上一组公共别名（bash/read/write/edit/glob/grep/question 的大小写两种形态），再按具体壳追加差异部分**。公共部分保证「任何壳下模型看到的工具名都与它熟悉的那个工具集重叠」，差异部分只在真正需要时加。

**`Other` 在别名工具里落到「只挂公共别名」这一支。**

因为它既不是 Native，也不匹配任何 `is_*`，所以只拿到公共组。这是 `Other` 的合理落点：名字不认识，就没法知道它需要什么额外工具，但至少工具名要对得上主流形状。这也意味着**`Other` 是可用的**——不是错误状态。

**解析点的分布说明了「harness 从哪里来」。**

全仓 `Harness::from_config_name` 的调用点只有三处（测试不计）：

- `core/src/tools/spec_plan.rs`：构造工具集时，取 `context.turn_context.config.harness`；
- `core/src/harness/session.rs`：会话层，取 `config.harness.as_deref()`；
- `app-server/src/interpreter_catalog_tests.rs`：一致性测试。

也就是说 harness 是**配置项**（`config_toml` 里 `pub harness: Option<String>`），在每一轮 turn 的配置里随行，而不是在启动时定死一次。这一点与「用户可以用 `/harness` 中途切换」是同一件事的两面：切换只改配置，下一次构造工具集与请求时自然生效。

**`Native` 与「没有 harness」在目录表里是同一个 id。**

`HARNESS_DEFINITIONS` 第一条：

```
HarnessDefinition { id: "", label: "Native", description: "Use the native Open Interpreter tool harness.", wire_apis: ALL_WIRE_APIS }
```

它用**空字符串**作为 id，而 `harness_choice` 在生成对外结构时做了转换：

```
InterpreterHarness {
    id: (!definition.id.is_empty()).then(|| definition.id.to_string()),
    ...
}
```

于是空 id 变成 `None`，而 `None` 在 wire 上的含义与配置里的「没设 harness」一致。这条转换很关键：如果这里把空串原样发出去，客户端设置 `harness = ""` 时又会被解析成 `Native`——看起来等价，但客户端一旦用 `""` 做非空判断就会误判为「已选了一个壳」。把它归一成 `None` 让两个层面用同一种方式表达「没选」。

`Native` 还独占 `ALL_WIRE_APIS`（`Responses` + `Chat` + `Messages` 三种都支持），而绝大多数壳只有 `CHAT_WIRE_APIS`、`claude-code` 家族只有 `MESSAGES_WIRE_APIS`。这张表决定了「这个壳在当前 provider 的 wire API 下能不能用」——见下一段的过滤。

**可选性过滤是「wire API 兼容」而不是「偏好排序」。**

`harness_choices` 里：

```
let wire_api = provider.map(|p| p.wire_api).or_else(|| bundled_provider_catalog_entry(provider_id).map(|e| e.wire_api)).unwrap_or(WireApi::Chat);
...
let mut choices: Vec<_> = HARNESS_DEFINITIONS.iter().filter(|d| d.supports(wire_api)).collect();
choices.sort_by_key(|d| usize::from(d.id != recommended));
```

两件事各司其职：`filter` 是**硬约束**（一个 Messages-only 的壳在 Chat 提供方下根本发不出请求，所以不能出现在列表里）；`sort_by_key` 是**软推荐**（`recommended` 由 `default_harness_for_provider_model(provider_id, provider, model)` 给出，命中的排在前面并加 `" (recommended)"` 后缀）。

`wire_api` 的解析本身有两级回退：先看已配置的 provider，再查内置 provider 目录，最后兜底 `WireApi::Chat`。兜底选 Chat 而不是 Responses 也是有依据的——Chat Completions 是兼容面最广的一种形态。

## 关键取舍

**源码事实：** `Harness` 有 15 个具名变体 + `Other(String)`，`Native` 是 `#[default]`；`ClaudeCode`/`ClaudeCodeBare` 与 `KimiCode`/`KimiCli` 都是成对的不同变体，不是别名。

**源码事实：** `from_config_name` 把 `None` 与 `Some("")` 合并成 `Native`，未知名字保留原串进 `Other` 而不失败。

**源码事实：** `is_claude_code` 覆盖两个变体，其余 `is_*` 都是单变体；`is_*` 是 `pub` 方法而非 `matches!` 宏内联。

**源码事实：** 解析表与 `HARNESS_DEFINITIONS` 是两张独立的手写表，一致性由 `interpreter_catalog_tests` 里的断言守住（目录 id 必须解析为已知变体，不能是 `Other`）。

**源码事实：** 目录表里 `Native` 的 id 是空字符串，`harness_choice` 再把它转成 `None`；`Native` 支持三种 wire API，其余壳按家族各自限于 Chat 或 Messages。

**源码事实：** 别名工具对所有非 `Native` 的壳挂同一组公共别名（含大小写两种形态），再按具体壳追加；`Other` 只拿公共组。

**架构推论：** 「成对变体」而非「变体 + 参数」反映的是**演化方向**而不是配置差异。`claude-code-bare` 与 `claude-code` 的差别不只是少几个常量，而是提示与工具集都可以分开演进；把它们压成一个带 `bare: bool` 的变体，等于强迫两条演化线共用一份代码。

**架构推论：** 两张手写表 + 一条一致性测试，是「不可能自动对齐的东西就用测试兜住」的典型做法。若改成从枚举派生目录表，`label` 与 `description` 这些展示文案仍然要手写，所以派生的收益有限；而测试的成本只有一行断言。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/tools/src/harness.rs` | `Harness` | 15 个具名壳 + `Other(String)`，`Native` 为默认 |
| `codex-rs/tools/src/harness.rs` | `from_config_name` | kebab-case 名 → 变体；`None`/空串 → `Native`，未知 → `Other` |
| `codex-rs/tools/src/harness.rs` | `is_claude_code` | 唯一跨两变体的谓词（`ClaudeCode` + `ClaudeCodeBare`） |
| `codex-rs/tools/src/harness.rs` | `is_kimi_code` / `is_swe_agent` / `is_minimal` | 单变体谓词，为调用点提供稳定名字 |
| `codex-rs/app-server/src/interpreter_catalog.rs` | `HARNESS_DEFINITIONS` | 展示与可选性表：id、标签、描述、支持的 wire API |
| `codex-rs/app-server/src/interpreter_catalog.rs` | `harness_choices` | 按 wire API 硬过滤 + 按推荐项排序 |
| `codex-rs/core/src/tools/spec_plan.rs` | `add_harness_alias_tools` | 非 Native 壳挂别名工具的入口 |

## 参考

- [open-interpreter 源码：tools/src/harness.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/tools/src/harness.rs)
- [open-interpreter 文档：Harness](https://www.openinterpreter.com/docs/terminal/harness)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [Rust 枚举与 `#[default]` 派生](https://doc.rust-lang.org/std/default/trait.Default.html)

## 与主核 / AI 的关系

「枚举加一个 `Other(String)` 兜底」是兼容层里一个很实用的形状，主核里对应的是方言/扩展类型的建模：已知的方言做成具名变体以便特判，未识别的保留原串以便还能按名字走通用路径。若把未知一律映射到默认值，一次拼写错误就会静默变成「用默认方言执行」——一个不报错但结果不同的错误。

对 AI 工程的实际意义有三条。

第一，**「没设」与「设了一个不存在的」必须分开**。这里 `None`/`""` → `Native`，`"minmal"` → `Other("minmal")`。前者是默认行为，后者是可诊断的异常。把两者合并会让后者永远无法被发现。

第二，**成对变体是演化留出的口子**。`claude-code` 与 `claude-code-bare`、`kimi-code` 与 `kimi-cli` 都是「新的那个不再继承旧的补丁」的显式表达。若当初图省事用一个变体加布尔参数，等两条线真的分叉时，参数会变成一串互相耦合的开关。

第三，**手写表之间的一致性只能靠测试**。这里两张表描述同一批壳却不共享定义，唯一的一致性保障是「目录里的每个 id 都必须能被解析」这一条断言。这类断言很便宜，但一旦漏掉，症状是「界面上能选但选了不生效」——排查成本远高于写断言的成本。
