---
project: open-interpreter
feature: context-fragments
title: 上下文片段与附加信息
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/context-fragments/src/fragment.rs
    symbols: [RenderedFragment, ContextualUserFragment, matches_marked_text]
  - path: codex-rs/context-fragments/src/additional_context.rs
    symbols: [MAX_ADDITIONAL_CONTEXT_VALUE_TOKENS, AdditionalContextUserFragment, additional_context_body]
---

# 上下文片段与附加信息

## 一句话职责

给「注入进上下文的每一段内容」定一个统一形状：谁的角色、正文怎么渲染、用什么标记认出它、属于哪个分类；附加信息这类带外部内容的部分还要按 token 预算中间截断，并把起止标记做成可反查的形式。

## 机制是什么

`ContextualUserFragment` 是一个 trait，把「一段注入内容」需要的全部信息收成一个契约：

- `role()` —— 这段内容以什么角色出现（`"user"`、`"developer"` 等）。
- `content_kind()` —— 一个稳定的 `<feature>.<name>` 分类字符串，共享片段用 `generic`。
- `requires_separate_message()` —— 默认 `false`；「这段是否必须记成独立的 response item」。
- `markers()` / `type_markers()` —— 实例级与类型级的起止标记。
- `body()` —— 正文。
- `matches_text(text)` —— 有默认实现，靠类型级标记判断「这段文本是不是我这种片段」。
- `render()` / `render_fragment()` / `into()` / `into_boxed_response_item()` —— 四个渲染出口。

trait 上的 doc comment 把渲染约定写得极细：「标记也提供用来事后认出被注入上下文的起止标记。`render()` 把标记与正文直接拼接而**不加分隔符**，所以实现应当把需要的空白写进 `body()` 里」。最后一句是关键补充：「未标记的片段应当让两个标记都为空，这种情况下默认助手只渲染正文，且**永不匹配任意文本**」。

`RenderedFragment` 是「已渲染的片段 + 拥有它标注内容的角色」：`role: &'static str` 与 `content: AnnotatedContent`。`&'static str` 意味着角色只能来自编译期常量集合，不能是运行时拼出的字符串——这防止了「注入一个角色名是用户输入的消息」这类问题。

`AnnotatedContent` 是「模型可见内容 + 分类」的配对：`content: ContentItem` 与 `kind: ContentItemKind`。两个字段一起构造、一起拆分（`into_parts`），中间不提供「只改一个」的公开入口。

## 怎么实现的

**为什么内容要与分类绑成一个值。** `AnnotatedContent` 的存在理由在 `to_annotated_content` 的 doc comment 里：

「取出一条消息的内容与它的**位置性**分类。遗留消息（包括已持久化的 rollout）可能没有分类。缺失的条目被分类为 unknown，让消息仍然可用」。

所以分类是**按位置**存的一个平行数组（`content_item_kinds: Vec<...>`），与 `content: Vec<ContentItem>` 一一对应。两个数组平行存放有一个天然风险：任何只改其中一个的操作都会让它们错位。`AnnotatedContent` 就是把「一条内容 + 它的分类」合并成一个单元，从此改内容必然带着分类一起改。

`to_annotated_content` 的实现印证了这个平行结构：它 `take()` 出 kinds（可能为空），然后 `zip` 内容与 kinds，kinds 用尽后无限补 `"unknown"`。所以「老数据没分类」和「分类比内容少」都被安全地降级。

`set_annotated_content` 反向做同一个动作：`unzip` 成两个数组分别写回，并用 `get_or_insert_default()` 保证 metadata 存在。两个函数的返回值都是 `Option<...>`，`None` 表示「这不是一条 Message 类型的 item」——所以调用方必须区分「不是消息」与「消息但没分类」。

**`render` 的三种形态。** `render()` 先取实例级的 `markers()`。若两个都为空就 `return body`——即「未标记片段只有正文」。否则 `format!("{start_marker}{body}{end_marker}")`，**不插任何分隔符**。

这正是 doc comment 里那句「实现应当把需要的空白写进 body()」的来源。以一个真实的实现为例，`additional_context_body` 返回的是 `format!("{key}>{value}</external_{key}")`——注意它**没有**前导 `<`，因为起始标记 `"<external_"` 会拼在前面，合起来才是 `<external_key>value</external_key>`。

所以「标记 + 正文」的拼装是逐字符精确的，任何多余的空格都会破坏格式。这也是为什么 doc comment 要专门写一句提醒——这个约定无法从类型上强制。

**标记匹配用「开头与结尾各看一段」。** `matches_marked_text(start_marker, end_marker, text)`：

1. 任一标记为空 → `false`。「未标记片段永不匹配任意文本」就是这一行。
2. `trim_start()` 后取前 `start_marker.len()` 个字符，`eq_ignore_ascii_case` 比较。
3. `trim_end()` 后从末尾倒取同样长度的后缀，同样忽略大小写比较。
4. 两者都要成立。

`get(..)` 与 `get(len.saturating_sub(...)..)` 用 `Option` 而不是切片——所以文本太短时得到 `None` 而不是 panic。这是「先安全地取，再比较」而不是「先判长度，再取」。`saturating_sub` 同样防了「标记比文本长」的溢出。

忽略大小写是有意的：模型可能把 `<EXTERNAL_FOO>` 写成不同的大小写形式，或者其他工具生成时大小写不一致。

**附加信息的两套标记。** `additional_context.rs` 三常量：

- `MAX_ADDITIONAL_CONTEXT_VALUE_TOKENS = 1_000`
- `ADDITIONAL_CONTEXT_END_MARKER_SUFFIX = ">"`（只是个 `>`
- `ADDITIONAL_CONTEXT_START_MARKER_PREFIX = "<external_"`

而 `AdditionalContextUserFragment` 与 `AdditionalContextDeveloperFragment` 两个结构**字段完全相同**（`key: String`、`value: String`），差别只有三处：

| | User 版 | Developer 版 |
|---|---|---|
| `role()` | `"user"` | `"developer"` |
| `type_markers()` | `("<external_", ">")` | `("", "")` |
| body | `<external_key>value</external_key>` | `<key>value</key>` |

所以「user 角色带标记、developer 角色不带标记」是刻意的成对设计。为什么？doc comment 在 trait 上给了线索：未标记片段「永不匹配任意文本」。对 developer 角色的内容来说，「事后认出它」这件事可能不需要或不该做——标记的用途是「识别注入的上下文」，而 developer 消息本来就是系统侧注入的，不需要额外标记。

而 user 侧用 `<external_...>` 这种伪 XML 标签，是因为这段内容来自**外部**（工具输出、文件内容之类），要让模型明确知道「这是外部内容，不是你用户说的话」。

**`matches_text` 被覆写成结构解析，而不是前后缀比较。** 这是这一对结构里最值得看的地方：`AdditionalContextUserFragment::matches_text` 的实现是：

1. `trim()` 后要求以 `"<external_"` 开头，否则 `false`。
2. `strip_prefix` 之后按 `">"` 分割一次，得到 `key` 与剩余。
3. 要求剩余**以 `</external_{key}>` 结尾**。

所以它不只验前后缀，还验「开头那个 key 与结尾那个 key 一致」。这比默认的 `matches_marked_text` 严格得多——默认实现只看首尾各 9 个字符与 1 个字符，无法保证内部标签配对。

为什么要这个严格性？因为默认的 `type_markers` 太宽松：`("<external_", ">")` 意味着任何以 `<external_` 开头、以 `>` 结尾的文本都会被判为「是我的片段」。而一段文件内容恰好以这些字符开头结尾时，就会被误认。覆写后要求 `</external_key>` 完整闭合，误认的概率大幅降低。

注意 `value_and_close.ends_with(...)` 用的是 `ends_with` 而不是精确匹配——所以中间可以任意，只要闭标签形式正确且 key 一致。

**正文按 token 预算中间截断。** 两个 body 函数都先 `truncate_middle_with_token_budget(value, MAX_ADDITIONAL_CONTEXT_VALUE_TOKENS).0`。中间截断（而不是尾部）意味着保留头尾、砍掉中段——对「外部内容」这是合理的：开头通常说明这段是什么，结尾可能是重要的结论或闭合标签。

`.0` 说明这个函数返回元组（大概是「截断后文本 + 是否被截断」），这里只要文本。而截断发生在**拼标签之前**，所以标签永远完整——不会出现「截断把 `</external_key>` 砍掉一半」的情况。这个顺序很重要，否则 `matches_text` 就再也认不出它了。

**`content_kind` 用 key 做后缀。** 两个实现的 `content_kind` 都是 `ContentItemKind(format!("additional_content.{}", self.key))`。所以分类不是「附加信息」一个大桶，而是按 key 细分（`additional_content.memory`、`additional_content.file` 之类）。这与 trait doc 里「稳定的 `<feature>.<name>` 分类」的约定一致。

**转换到 role 与 content 的方式。** `From<RenderedFragment> for ResponseItem` 构造一个 `ResponseItem::Message`，其中：

- `id: None`——片段没有 id（不是从模型或用户来的消息）。
- `content: vec![content]`——**单个**内容项。
- `internal_chat_message_metadata_passthrough: Some(...)` 带上 `content_item_kinds: Some(vec![content_kind])`——分类被写到 metadata 的平行数组里，长度 1 对 1。

所以一个片段 = 一条单内容项的消息。这也解释了 `requires_separate_message()` 这个标志：默认 `false` 意味着「可以和其他内容合并进同一条消息」，设为 `true` 时调用方必须为它单独开一条。

## 关键取舍

**源码事实：** `render()` 在标记与正文之间**不加分隔符**，trait doc 明确要求实现把空白写进 `body()`。

**源码事实：** 两个标记都为空时 `render()` 只出正文，且 `matches_marked_text` 直接返回 `false`——未标记片段不参与识别。

**源码事实：** 标记匹配忽略大小写，并用 `get(..)` 与 `saturating_sub` 保证文本过短时不 panic。

**源码事实：** `AdditionalContextUserFragment` 覆写了 `matches_text`，要求起始 key 与闭合标签的 key 一致，比默认首尾比较严格得多。

**源码事实：** Developer 版的标记是 `("", "")`，即无标记；User 版是 `("<external_", ">")`。

**源码事实：** 附加信息的值按 1k token 预算**中间**截断，且在拼标签之前截断，所以标签永远完整。

**源码事实：** `role` 的类型是 `&'static str`，只能是编译期常量。

**架构推论：** `AnnotatedContent` 把「内容」与「分类」绑成一个不可分的值，是对「平行数组」这个结构缺陷的根治。任何两个必须等长且按位置对应的数组，都该被合并成一个数组的配对元素——否则每一次单边修改都是一个潜在的错位 bug，而错位在序列化往返之后往往不可见。

**架构推论：** 「带标记的用严格解析、不带标记的不参与识别」这个二分，说明标记机制的价值不在于「标记」本身，而在于**它是否可被可靠地反向识别**。一个标记若只能靠首尾字符猜，那它带来的误判风险可能超过收益。所以这一对结构选择只给 user 侧加标记，并给它配一个真正的解析器。

**架构推论：** 「中间截断 + 先截断后拼标签」的组合，是为了让「被截断的片段仍然是合法片段」。若先拼标签再截断，产物既不是合法片段（标签残缺）也不再能被 `matches_text` 认出——于是这段内容会永久地以「来源不明」的形式留在历史里。任何「有结构的产物 + 有预算约束」的地方，都该按「先满足结构、再满足预算」的顺序处理。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/context-fragments/src/fragment.rs` | `RenderedFragment` | 已渲染片段与 owning 角色，`&'static str` |
| `codex-rs/context-fragments/src/fragment.rs` | `ContextualUserFragment` | 注入内容的统一契约与渲染出口 |
| `codex-rs/context-fragments/src/fragment.rs` | `ContextualUserFragment.render` | 标记与正文直接拼接，不加分隔符 |
| `codex-rs/context-fragments/src/fragment.rs` | `ContextualUserFragment.render_fragment` | 合成角色、正文与分类 |
| `codex-rs/context-fragments/src/fragment.rs` | `matches_marked_text` | 首尾标记比较，忽略大小写，越界安全 |
| `codex-rs/context-fragments/src/fragment.rs` | `From<RenderedFragment> for ResponseItem` | 转成单内容项消息并写入分类 |
| `codex-rs/context-fragments/src/annotated_content.rs` | `AnnotatedContent` | 内容与分类的不可分配对 |
| `codex-rs/context-fragments/src/annotated_content.rs` | `to_annotated_content` | 拆开平行数组，缺失分类补 unknown |
| `codex-rs/context-fragments/src/annotated_content.rs` | `set_annotated_content` | 一起写回内容与分类数组 |
| `codex-rs/context-fragments/src/additional_context.rs` | `MAX_ADDITIONAL_CONTEXT_VALUE_TOKENS` | 附加信息值 1k token 上限 |
| `codex-rs/context-fragments/src/additional_context.rs` | `AdditionalContextUserFragment` | user 角色、带标记、覆写严格匹配 |
| `codex-rs/context-fragments/src/additional_context.rs` | `AdditionalContextDeveloperFragment` | developer 角色、无标记、不参与识别 |
| `codex-rs/context-fragments/src/additional_context.rs` | `additional_context_body` | 先截断值再拼 `<external_key>` 标签 |
| `codex-rs/context-fragments/src/additional_context.rs` | `ADDITIONAL_CONTEXT_START_MARKER_PREFIX` | `"<external_"` 起始标记前缀 |

## 参考

- [open-interpreter 源码：context-fragments/src/fragment.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/context-fragments/src/fragment.rs)
- [open-interpreter 源码：context-fragments/src/additional_context.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/context-fragments/src/additional_context.rs)
- [open-interpreter 文档](https://www.openinterpreter.com/docs)

## 与主核 / AI 的关系

主核里最接近的是「内部记录的类型标签」：一条内部记录除了内容，还带一个标明它是什么种类、由谁产生的标签，读取时按标签分派。`AnnotatedContent` 就是「内容 + 标签」的同一个结构，连「旧数据可能没有标签，要降级成 unknown」这条兼容规则都一样。

差别在于这一层的标签与内容**必须等长且按位置配对**（平行数组），而主核内部记录通常一个记录一个标签。这个差别带来了一整类风险：任何只改一边的操作都会错位。`AnnotatedContent` 的作用就是把这个风险从「运行时可能错位」降级成「结构上不可能错位」。

与 goose 的 i03（handoff 备忘录）对照：那边把历史压成一段文本，必须在文本里植入「这是历史而不是新任务」的声明；这边把每段注入内容包成带标记的片段，让它可以被事后认出。两者的目标相同——**让模型看到的内容带着自己的来历**——手段不同：一个是自然语言声明，一个是机器可识别的标记。

对 AI 工程最实用的一条是「标记必须能被严格反识别，否则不如不加」。这一页给 user 侧片段加了 `<external_key>...</external_key>` 这样的成对标签，并专门覆写匹配函数去验证 key 配对；而 developer 侧干脆不加标记。这个取舍的依据很实在：一个宽松的标记（只看首尾几个字符）会把恰好长成那样的普通文本误判成注入片段，而误判的后果是「把用户内容当成系统注入的内容处理」——那比没有标记更糟。

另一条是「先满足结构、再满足预算」。任何需要截断的有结构文本（XML、JSON、带闭合标签的片段），截断都必须在**结构完整**的前提下进行：要么截断最内层的值（这里是 `value`），要么整体放弃。绝不能截断已经拼好的外壳——那会产生一段永远无法再被识别的半成品。

最后一条是「未标记内容不参与识别」这个默认值。它看起来是保守，实际是正确方向：`matches_marked_text` 在任一标记为空时直接返回 `false`，所以调用方永远不会因为「标记为空」而意外匹配到任意文本。把「不适用」表达为「永不匹配」而不是「总是匹配」，是这类判定函数该有的默认。
