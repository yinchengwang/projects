---
project: goose
feature: context-management
title: 上下文压缩
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/context_mgmt/mod.rs
    symbols: [auto_compact_threshold, capped_compaction_threshold, is_compaction_summary]
  - path: crates/goose/src/context_mgmt/mod.rs
    symbols: [compute_tool_call_cutoff, tool_ids_to_summarize]
---

# 上下文压缩

## 一句话职责

决定「什么时候开始压缩历史」与「压缩哪些内容」：阈值由两个互相制约的参数决定，工具调用则成对按批次摘要化，并且必须能识别「这一条已经是压缩摘要」，否则压缩会自我叠加。

## 机制是什么

上下文压缩有三类常见做法：按条数截断最早的、按 token 数截断最早的、以及让模型把早期历史摘要成一条。goose 走的是第三种，但它把问题拆成两个独立决策：

- **何时触发**：阈值。
- **压哪些**：工具调用对（`tool call` 与它的 `tool response` 必须成对），按批次做摘要。

这两个决策分开的原因很实际：触发是按整段对话的 token 数算的，而「能压什么」受结构约束——一条工具调用与它的响应如果被拆开，模型会看到一个没有响应的调用，provider 会直接拒绝。所以压缩单位不是「消息」，而是「对」。

## 怎么实现的

**两个阈值参数互相制约。** `auto_compact_threshold` 取两个配置项：一个比例（`GOOSE_AUTO_COMPACT_THRESHOLD`）与一个绝对 token 上限（`GOOSE_AUTO_COMPACT_TOKEN_LIMIT`，默认 225000）。然后交给 `capped_compaction_threshold` 取二者的**较小值**（以比例形式返回）。

这个设计解决的是一个具体问题：不同模型的上下文窗口差异很大。如果只配比例，那么在超大窗口的模型上，225000 这个绝对量早就被超过却还没触发——而超过某个输入 token 数之后，供应商的计价会跳到更高的档位。源码的注释把理由写得很直接：`DEFAULT_AUTO_COMPACT_TOKEN_LIMIT` 是「在 OpenAI 长上下文档位之下留出余量」，并特意声明「这不是请求大小上限」。

所以两个参数的角色不同：比例是「相对窗口的安全余量」，绝对上限是「价格档位的护栏」。取较小值意味着**哪个先到就按哪个触发**。

**`capped_compaction_threshold` 的早退。** 三个条件直接返回原阈值：`threshold <= 0`、`threshold >= 1`、`context_limit == 0`。前两个是非法比例（压缩在 0% 或 100% 触发都没有意义），第三个是「不知道窗口大小」——此时无法把 token 上限换算成比例，只能退回纯比例语义。这个早退避免了除零，也避免了「配置非法时静默改行为」。

**识别已有的压缩摘要。** `is_compaction_summary` 有两条判据，第一条是看消息上的 `compaction_summary` 标志；第二条是**回退识别**：若消息是 agent 可见的、非用户可见的，且某个文本块**以**三种「续接文本」之一结尾，就认定它是旧摘要。

第二条为什么需要：`compaction_summary` 这个标志是后加的，更早的会话里存的摘要没有这个标志。同时「会话修复」过程会把摘要的多个文本块**拼接**起来，所以原本独立的续接文本会变成摘要文本的尾部。判据因此写成 `ends_with` 而不是相等匹配。

这段逻辑有专门的测试，覆盖三种情况：带标志的旧消息（拼接前）、普通消息、以及**只包含续接文本的消息**（不算摘要——因为没有实际摘要内容）。第三个反例很关键：单看「结尾是续接文本」会把它误判成摘要。

**工具调用对的批量摘要。** `compute_tool_call_cutoff` 用有效上限（`context_limit × threshold`）算出一个阈值：`3 × effective_limit / 20000`，夹在 `[10, 500]` 区间。这个式子的含义是「每 20000 token 允许保留约 3 个工具调用对」。夹紧区间保证小窗口下不会压得过狠、大窗口下不会一次压太多。

`tool_ids_to_summarize` 收集所有 agent 可见消息里的工具请求 id，然后减去 `protect_last_n`（当前轮的工具调用永不摘要），再判断「可摘要的数量」是否超过 `cutoff + 批次大小`。**这个判断用 `+ 批次大小`**：如果剩余可压的量不到一整个批次，就干脆不压。理由是「压半个批次」会让压缩收益抵不过一次额外模型调用的成本，而且批次边界不清会让后续判断更难。

返回的是**前** `BATCH_SIZE`（10）个 id，不是全部——每次压缩只处理一批。

**摘要消息的形状。** `summarize_tool_call` 产出的消息被刻意设成 `role = User`、`created` 取匹配消息的最后一条时间、`metadata = MessageMetadata::agent_only()`，然后 `with_generated_id()`。三处都是必要的：role 不能是 assistant（它是对历史的描述，不是模型说的话）；created 要跟着被摘要的内容，否则排序会错；`agent_only` 让它只对模型可见、不对用户展示。

## 关键取舍

**源码事实：** 两个阈值参数取较小值：比例是窗口安全余量，绝对 token 上限是价格档位护栏。注释明确写 `DEFAULT_AUTO_COMPACT_TOKEN_LIMIT` 不是「请求大小上限」。

**源码事实：** `is_compaction_summary` 有「标志」与「续接文本尾部」两条判据，后者是为兼容旧会话与「会话修复会拼接文本块」这两个事实。测试里专门有一条「只含续接文本但无摘要内容」的反例。

**源码事实：** 工具调用的可摘要判断带 `+ TOOLCALL_SUMMARIZATION_BATCH_SIZE`，即「不足一个批次就不压」。

**源码事实：** 摘要消息强制 `role = User` + `created` 跟随被摘要内容 + `agent_only` 元数据。三处都影响后续排序与可见性。

**架构推论：** 「压什么」按结构单位（工具调用对）而不是按消息条数，说明压缩的**边界必须对齐协议的边界**。这是所有「摘要式压缩」的共同约束：摘要可以任意粗，但被摘要的原始片段必须能被下游重新解释。

**架构推论：** 两个阈值而非一个，反映的是「同一个数值在不同模型上含义不同」这个现实。任何跨模型的容量管理都要同时考虑「相对窗口」与「绝对计费档位」，单参数方案必然在其中一侧失准。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/context_mgmt/mod.rs` | `DEFAULT_AUTO_COMPACT_TOKEN_LIMIT` | 绝对 token 上限（价格档位护栏） |
| `crates/goose/src/context_mgmt/mod.rs` | `auto_compact_threshold` | 合并两个配置参数得最终阈值 |
| `crates/goose/src/context_mgmt/mod.rs` | `capped_compaction_threshold` | 比例与绝对上限取小，含三个早退条件 |
| `crates/goose/src/context_mgmt/mod.rs` | `is_compaction_summary` | 识别已有摘要（含旧会话回退判据） |
| `crates/goose/src/context_mgmt/mod.rs` | `compute_tool_call_cutoff` | 按有效上限算出可保留的工具对阈值 |
| `crates/goose/src/context_mgmt/mod.rs` | `tool_ids_to_summarize` | 选出本批次要摘要的工具调用 id |
| `crates/goose/src/context_mgmt/mod.rs` | `agent_visible_tool_pair` | 取出成对的工具请求与响应，缺一报错 |

## 参考

- [goose 源码：context_mgmt/mod.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/context_mgmt/mod.rs)
- [goose 文档](https://block.github.io/goose/)
- [Anthropic 工程博客：有效上下文工程](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)
- [OpenAI 文档：长上下文与计费](https://platform.openai.com/docs/guides/long-context)

## 与主核 / AI 的关系

上下文压缩与主核（多模态数据库内核）的缓冲池淘汰在**目标**上一致：在有限容量里保住最重要的东西。但**单位**不一致，而单位的选择正是两类系统的分野。

主核的单位是**页**：一页是存储层的原子读写单位，淘汰一页不影响其他页的可解释性。goose 的单位是**工具调用对**：因为一条工具调用与它的响应如果被拆开，模型会看到无响应的调用，provider 直接拒绝。这说明 LLM 应用里的「片段」必须对齐协议语义，而数据库里的「片段」对齐的是存储布局。

**两个阈值取小**这条机制与主核里的「软上限 + 硬上限」是同一思路。主核的刷脏可以按比例触发（相对脏页比例），也可以按绝对量触发（脏页数量上限），哪个先到就刷。比例适合不同大小的缓冲池，绝对量适合规避某个具体的性能悬崖——goose 的绝对量规避的是**价格悬崖**，这在数据库里没有对应物，但在任何按量计费的云服务里都存在。

`is_compaction_summary` 的回退识别值得单独记：它承认了「标志是后加的，历史数据里没有」。主核里对应的问题是「旧格式的数据文件如何被新版本识别」——答案同样是「用内容特征做回退判据，而不是假设元数据一定存在」。
