---
project: haystack
feature: llm-routers
title: LLM 决策路由
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/routers/llm_messages_router.py
    symbols: [LLMMessagesRouter]
  - path: haystack/components/routers/openai_decisions_text_router.py
    symbols: [OpenAIDecisionsTextRouter]
  - path: haystack/components/routers/openai_decisions_image_router.py
    symbols: [OpenAIDecisionsImageRouter]
---

# LLM 决策路由

## 一句话职责

让语言模型在读完全部输入后决定这条数据该流向哪个下游分支，并把「选了哪个分支、为什么」一起交出来——而不是由人预先写死判断条件。

## 机制是什么

haystack 有三类路由：条件路由（用户写 Jinja 表达式）、元数据/文件类型路由（按字段值与 MIME 判断）、以及这一页的 **LLM 决策路由**。

三者的分工很清楚：**能用确定性规则表达的判断绝不该交给模型**。所以 LLM 路由只在两种情况下用：判据本身是语义的（这段文本在问产品还是问账单），或者输入是多模态的、规则无从下手（这张图该走 OCR 还是走图像理解）。

它的代价是显式的：多一次模型调用、输出不确定、失败模式从「表达式求值异常」变成「模型给出了不在候选里的答案」。所以这一页的机制重心不在「怎么调模型」，而在**怎么把模型的不确定输出收敛成一个确定的、可审计的分派决定**。

## 怎么实现的

**`LLMMessagesRouter` 是最基础的一个。** 它接收消息列表与一组「分支描述」，让模型从中选一个，然后只往被选中的输出 socket 写值。关键设计是：**分支描述也是输入**。也就是说路由的可选范围不是编译期固定的，而是随数据流一起进来的——这让同一个路由组件能服务不同的下游集合。

它必须处理一个现实问题：模型可能返回一个不在候选里的名字。这类情况没有统一的「正确答案」——抛错会让整条管道停住，静默选默认又会让错误被埋掉。源码选择的是**把校验放在分派之前**，让失败发生在「决定」这一步而不是「已经写错 socket」之后。

**两个 OpenAI 决策式路由按模态分家。** `OpenAIDecisionsTextRouter` 处理文本消息，`OpenAIDecisionsImageRouter` 处理图像。分开而不是合成一个，原因是**两者的输入构造完全不同**：文本路由把对话历史直接送去，图像路由需要先把图像编码成模型可接受的形态（data URL 或引用）。用一个组件兼容两种情况，内部就会出现「按输入类型分叉」的代码，而那正是这个框架在别处一直避免的形态。

**「决策原因」是输出的一部分，不是日志。** 这类路由的输出除了被选中的分支名，还带一段模型给出的理由。它之所以进输出而不进日志，是因为**路由决定的可审计性是生产系统的基本要求**：一条答案不可信时，第一个要查的就是「它为什么走了这条路径」。把理由当副作用写进日志，意味着它无法被下游消费、无法被评测、无法随管道快照一起保存。

## 关键取舍

**源码事实：** 三类路由并存（条件 / 元数据 / LLM），LLM 路由只用于判据本身是语义的或输入是多模态的场景。

**源码事实：** `LLMMessagesRouter` 把「分支描述」当输入而非构造参数，所以可选分支集合是运行期数据。

**源码事实：** 文本路由与图像路由是两个独立组件（`OpenAIDecisionsTextRouter` / `OpenAIDecisionsImageRouter`），不合并成一个多模态路由。

**架构推论：** 把「原因」放进输出而不是日志，是一条容易被忽视但影响深远的决定。它让路由决定成为**数据流的一部分**，因此可被持久化、可被评测、可被下游条件路由二次利用。反过来，如果只写日志，任何「按路由结果做后续判断」的需求都会变成解析日志——那是把结构化数据降级成文本。

**架构推论：** 模型输出的不合法值必须在校验层收敛。这条约束对 LLM 路由比对手写规则路由严得多，因为手写规则的失败模式是「表达式抛异常」（类型明确、位置明确），而模型的失败模式是「给出了一个看起来合理但不在候选里的名字」。**前者的错误会自己冒出来，后者需要主动检查**——这是所有「让模型做分类」的系统都要付的成本。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/routers/llm_messages_router.py` | `LLMMessagesRouter` | 按消息内容在候选分支间选择 |
| `haystack/components/routers/openai_decisions_text_router.py` | `OpenAIDecisionsTextRouter` | 文本模态的决策式路由 |
| `haystack/components/routers/openai_decisions_image_router.py` | `OpenAIDecisionsImageRouter` | 图像模态的决策式路由 |
| `haystack/components/routers/conditional_router.py` | `ConditionalRouter` | 对照项：手写表达式的确定性路由 |

## 参考

- [haystack 源码仓库](https://github.com/deepset-ai/haystack)
- [haystack 文档：路由](https://docs.haystack.deepset.ai/docs/routers)
- [haystack 文档：管道与分支](https://docs.haystack.deepset.ai/docs/pipelines)
- [haystack 源码：routers 目录](https://github.com/deepset-ai/haystack/tree/main/haystack/components/routers)

## 与主核 / AI 的关系

LLM 决策路由与主核（多模态数据库内核）的**查询优化器选计划**在目标上同构：给定输入与一组候选执行路径，选一个去执行。

**判据的可表达性是分水岭。** 优化器用**代价模型**选计划：候选集合固定、代价可估算、选择过程完全确定且可复现。LLM 路由用**语义理解**选分支：候选集合运行期可变、代价无法估算、选择过程不确定。这个差别决定了可优化性——数据库可以通过统计信息与代价估计持续改进选择质量，LLM 路由只能通过改提示与换模型。

**「选择原因」在两边都是必需品，但用途不同。** 数据库的 `EXPLAIN` 输出是给人调优用的（看完就走）；LLM 路由的理由要进数据流，因为它影响答案的可信度评估。前者是诊断工具，后者是数据。

**不合法输出的校验。** 优化器不会产出「不在候选列表里的计划」，因为它就在自己的数据结构里选。LLM 会。所以 LLM 路由必须显式校验，而优化器不需要——**这是「在闭集里选」与「让外部系统在闭集里选」的根本差别**。任何把决策交给模型的设计，都要把这层校验当成必需组件而不是防御性代码。

**条件路由与 LLM 路由并存的分工**，对应数据库里「谓词下推」与「运行时决策」的分工：能用静态规则判的绝不留到运行期，因为静态规则可优化、可验证、可预测。
