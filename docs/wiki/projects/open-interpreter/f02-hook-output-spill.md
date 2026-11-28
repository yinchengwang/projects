---
project: open-interpreter
feature: hook-output-spill
title: 钩子输出溢写
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/hooks/src/output_spill.rs
    symbols: [HookOutputSpiller, DEFAULT_HOOK_OUTPUT_TOKEN_LIMIT, spilled_hook_output_preview]
  - path: codex-rs/hooks/src/output_spill.rs
    symbols: [AdditionalContextLimit, AdditionalContext, HOOK_OUTPUTS_DIR]
---

# 钩子输出溢写

## 一句话职责

钩子可以往会话里注入任意文本（一段 `git status`、一次检查的结果、一个自定义脚本的输出），而它的体积不受控；这个模块给这些注入设一个**按 token 计的预算**，超了就把全文落到 `<临时目录>/hook_outputs/<thread_id>/` 下，只把一个**首尾预览加恢复路径**交回模型。

## 机制是什么

135 行，四个部件：

| 部件 | 内容 |
|---|---|
| `DEFAULT_HOOK_OUTPUT_TOKEN_LIMIT` | `2_500` |
| `AdditionalContextLimit` | 只包一个 `token_limit`，由 `from_config(Option<usize>)` 构造 |
| `AdditionalContext` | 一段文本 + 它自己的 limit |
| `HookOutputSpiller` | 持有输出目录，三个公开方法各自处理一种注入形态 |

三个方法是：`maybe_spill_text`（单段文本）、`maybe_spill_additional_contexts`（带各自 limit 的多段）、`maybe_spill_prompt_fragments`（钩子提示片段）。

## 怎么实现的

**目录是「临时目录 / 固定子目录 / 按线程分」三层，且构造时就定死。** 实现：

```
AbsolutePathBuf::resolve_path_against_base(std::env::temp_dir(), "/")
    .join(HOOK_OUTPUTS_DIR)          // "hook_outputs"
    .join(thread_id.to_string())
```

三个选择各有理由：

1. **落在系统临时目录而不是 goose 自己的配置目录**——这类文件是「会话期间的临时材料」，不该进入持久状态。
2. **固定子目录 `hook_outputs`** 让它在临时目录里可被识别与批量清理（与前面项目里 `goose_mcp_response_` 的前缀是同一思路）。
3. **按 `thread_id` 再分一层** ——多会话并行时各自的溢出文件互不混淆，且「清掉某个会话的临时文件」变成删一个目录。

第 3 点是比「平铺 + 文件名带 id」更好的做法：目录级隔离让清理动作有明确的边界。

**`resolve_path_against_base(temp_dir(), "/")` 这个调用值得单独看。** 它把一个「非绝对 base」变成绝对路径（`/` 作为相对的基准），于是即使 `temp_dir()` 返回一个相对路径（理论上不该，但 API 不保证），`output_dir` 仍是绝对的。`AbsolutePathBuf` 这个类型本身就在编码「这一定是绝对路径」这个不变量。

**预算是 token 而不是字节或字符，且默认 2500。** `DEFAULT_HOOK_OUTPUT_TOKEN_LIMIT` 的值与「钩子输出」这个用途是相称的：一段 `git status` 或测试输出通常几十到几百 token，2500 已经相当宽松；而一个遍历全仓库的脚本可能轻易上万。

**`AdditionalContextLimit::from_config(value: Option<usize>)` 的三重语义值得记。** 它的实现是 `token_limit: value.unwrap_or(DEFAULT_HOOK_OUTPUT_TOKEN_LIMIT)`。

于是：

| 输入 | 结果 |
|---|---|
| `None`（未配置） | 用默认 2500 |
| `Some(n)`，`n > 0` | 用 `n` |
| `Some(0)` | **用 0，而下文的判定会把 0 当成「不限制」** |

第三条需要看 `maybe_spill_text_with_limit` 才知道：

```
if token_limit == 0 || approx_token_count(&text) <= token_limit { return text; }
```

`token_limit == 0` 时**直接原样返回文本**，即「溢写机制被关掉」。也就是说配置一个 `0` 的效果是「让钩子输出无限注入」。

这是一个**语义上有争议但可用的约定**：`0` 没有被当成「什么都装不下」，而是被当成「不限」。风险在于——一个以为自己「设为 0 就禁用注入」的用户会得到相反的结果。但反过来看，若把 `0` 当成「全部溢写」，任何钩子的输出都会变成一句「全文在文件里」，这显然更荒谬。所以这里选的是**让 `0` 表示关掉限制**，与常见的「0 = 无限制」约定一致。

**判定用 `approx_token_count(&text)`，即估算而不是精确分词。** 精确的 token 计数需要模型的分词器，而钩子输出可能是一段任意文本、也可能是几十 MB 的日志——精确计数既慢又没必要。估算是可以的，因为预算本身就是个粗略界限。

**溢出文件用 UUID 命名，且没有可读的部分。** `format!("{}.txt", Uuid::new_v4())`。于是文件名与钩子名、时间、内容都无关。

这个选择的好处是**不需要处理文件名冲突与非法字符**（钩子内容完全可能来自用户输入的路径、命令名）；代价是**人无法从文件名推断内容**——而要定位一个溢出文件必须靠预览里的路径。

固定后缀 `.txt` 是有用的：它让读文件的一方（模型或用户）知道这是文本。

**写盘失败与建档失败都退回「纯截断」，而不是报错。** 两条路径：

- `fs::create_dir_all(parent)` 失败 → `warn!` 然后 `formatted_truncate_text(&text, TruncationPolicy::Tokens(token_limit))`；
- `fs::write(path, &text)` 失败 → 同样 `warn!` 然后截断。

**这个降级方向是这一页最重要的取舍。** 一个钩子输出过大时，最坏情况是「模型看不到全文」；而如果这里返回错误，会让整个钩子调用失败——那相当于**因为输出太长而让钩子彻底失效**。所以选择「截断 + 警告」：模型至少能看到前一部分，日志里有失败的痕迹。

这与 `goose` 的 `e03` 里「落盘失败时附全文加警告」是**相反的取向**：那边宁可撑爆上下文也不丢内容，这边宁可截断也不让钩子失败。差别的来源是两者的位置不同——那边的响应已经产生、丢掉就永远没有了；这里的钩子输出可以重新跑一次钩子得到。

**`spilled_hook_output_preview` 的预算算术是这一页最细的一处。** 实现：

```
let footer = format!("\n\nFull hook output saved to: {}", path.display());
let preview_policy = TruncationPolicy::Tokens(token_limit.saturating_sub(approx_token_count(&footer)));
format!("{}{footer}", formatted_truncate_text(text, preview_policy))
```

也就是**先算恢复路径那段 footer 的 token 数，再从预算里减掉，剩下的才给预览**。

文档注释把理由写明了：**路径 footer 在截断之前就被计入预算，这样加上恢复路径不会侵占配置的预览预算**。

这条很实在：如果不这么做，一个长路径会让「总 token 数」超预算，而预算是硬上限。而用 `saturating_sub` 意味着**footer 自己就超过预算时，预览部分是 0 token**——即只剩 footer。这是一个正确的极端处理（至少让模型知道「全文在哪个文件」）。

**`formatted_truncate_text` 配 `TruncationPolicy::Tokens` 而不是字节或行数**，与判定用的 `approx_token_count` 是同一套单位。**判定与裁剪必须同单位**，否则「判定说超了、裁出来还是超」这类不一致就会出现。

**三个公开方法的差异只在「怎么遍历」，判定逻辑是共用的。**

| 方法 | 传入的 limit |
|---|---|
| `maybe_spill_text(text)` | `AdditionalContextLimit::default()`（即 2500 或配置值） |
| `maybe_spill_additional_contexts(contexts)` | **每段自己的 `context.limit`** |
| `maybe_spill_prompt_fragments(fragments)` | `maybe_spill_text`（统一默认） |

第二条是本页的核心设计：**`AdditionalContext` 自带 `limit`，所以同一次注入里不同段落可以有不同预算**。而第三类（钩子提示片段）没有 per-fragment 预算，统一用默认——因为片段的来源是钩子执行结果，没有「每段配置」这个概念。

**这几个方法是 `async` 的，而它们在一个循环里被调用。** `maybe_spill_additional_contexts` 与 `maybe_spill_prompt_fragments` 都是「逐个 await」。于是多段溢出时是**串行写盘**。这个选择牺牲了并发，但保证了「顺序与输入一致」——对注入内容来说顺序是有意义的（提示片段按顺序拼接进对话）。

**溢写后的片段保留 `hook_run_id`。** `maybe_spill_prompt_fragments` 里构造新片段是：

```
HookPromptFragment { text: self.maybe_spill_text(fragment.text).await, hook_run_id: fragment.hook_run_id }
```

也就是**只换文本、不动 run id**。这是必要的：run id 是「这段注入来自哪次钩子执行」的追溯键，而溢写只改变了它的呈现长度。若这一层把 run id 丢了，溢出过的提示就变成无来源的注入。

## 关键取舍

**源码事实：** 默认预算 2500 **token**；判定用 `approx_token_count` 估算，裁剪用 `TruncationPolicy::Tokens`——两端同单位。

**源码事实：** `token_limit == 0` 表示**关掉限制**（原样返回文本），而不是「什么都装不下」。语义与常见的「0 = 无限制」一致，但会让「设 0 以禁用注入」的预期落空。

**源码事实：** 输出目录是「系统临时目录 / `hook_outputs` / `thread_id`」三层，按线程隔离使「清掉一个会话的临时文件」变成删一个目录。

**源码事实：** 溢出文件名是 `UUID.txt`，与内容、钩子名都无关——避免文件名冲突与非法字符，代价是无法从名字推断内容。

**源码事实：** 建档失败与写盘失败都降级为「纯截断 + `warn!`」，不让钩子因输出过长而失败。这与把全文塞回上下文的取向相反，因为钩子输出可以重跑得到。

**源码事实：** footer（恢复路径）的 token 数**先从预算里扣掉**再算预览，所以加路径不会侵占预览预算；`saturating_sub` 保证 footer 单独超预算时预览为 0 而不是下溢。

**源码事实：** `AdditionalContext` 自带 limit，于是同一次注入里不同段落可以有不同预算；钩子提示片段则统一用默认。

**源码事实：** 溢写只换片段文本、保留 `hook_run_id`，因为 run id 是这段注入的追溯键。

**架构推论：** 这层与前面 goose 的 `e03` 是同一个机制的两种取向，把它们放一起看最有价值：**同一个「输出太大」的问题，一旦出现在不同的位置上，正确解就不一样**。

- 工具响应已经产生、丢掉就没了 → 宁可撑爆也要保留（附全文加警告）；
- 钩子输出可以重跑 → 宁可截断也要让流程继续（截断加警告）。

判断依据不是「哪个更安全」，而是**「这份数据是否可重建」**。这是设计这类降级路径时最该先问的一个问题。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/hooks/src/output_spill.rs` | `HookOutputSpiller` | 持有输出目录，三个溢写方法 |
| `codex-rs/hooks/src/output_spill.rs` | `HookOutputSpiller::new` | 三层目录：临时目录 / `hook_outputs` / thread id |
| `codex-rs/hooks/src/output_spill.rs` | `maybe_spill_text_with_limit` | 判定 + 写盘 + 预览，含两条降级路径 |
| `codex-rs/hooks/src/output_spill.rs` | `maybe_spill_additional_contexts` | 逐段用它自己的 limit 溢写 |
| `codex-rs/hooks/src/output_spill.rs` | `maybe_spill_prompt_fragments` | 只换文本、保留 `hook_run_id` |
| `codex-rs/hooks/src/output_spill.rs` | `spilled_hook_output_preview` | footer 预算预扣后的首尾预览 |
| `codex-rs/hooks/src/output_spill.rs` | `DEFAULT_HOOK_OUTPUT_TOKEN_LIMIT` | 2500 token 的默认预算 |
| `codex-rs/hooks/src/output_spill.rs` | `AdditionalContextLimit::from_config` | `None` 用默认，`Some(0)` 表示关掉限制 |
| `codex-rs/hooks/src/output_spill.rs` | `AdditionalContext` | 自带 limit 的一段注入文本 |
| `codex-rs/hooks/src/output_spill.rs` | `HOOK_OUTPUTS_DIR` | 临时目录下的固定子目录名 |

## 参考

- [open-interpreter 源码仓库](https://github.com/OpenInterpreter/open-interpreter)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [open-interpreter 文档：钩子](https://www.openinterpreter.com/docs/terminal/hooks)
- [OpenAI 文档：token 与上下文窗口](https://platform.openai.com/docs/guides/text-generation)

## 与主核 / AI 的关系

这套「预算 + 溢出 + 预览」与主核里的「大字段溢出到外部存储」同构：行里放不下就溢出去，行内留一个指针。两者都要回答同一个问题：**溢出失败时怎么办**。数据库的选择通常是报错（因为溢出是行格式的一部分，静默截断会破坏数据）；这里选择降级为截断，因为它保护的对象是「流程能不能继续」而不是「数据完整」。

对 agent 工程的直接价值有三条。其一，**降级方向要按「数据能否重建」定**，不是按「哪个看起来更安全」。可重建的就截断，不可重建的就保留并加警告。其二，**预算的加法要预扣全部固定开销**。footer 的 token 数先扣掉再算预览，否则一个长路径就会让总量超预算——这类「总量硬上限 + 可变尾缀」的组合都需要同样的处理。其三，**`0` 的语义要显式定义并写在注释里**。`token_limit == 0` 是「不限」还是「全禁」，两种都能自圆其说，但用户会按其中一种去用；不写清楚就一定会有人设错。
