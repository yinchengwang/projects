---
project: crewai
feature: human-feedback
title: 人工反馈与恢复
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/flow/human_feedback.py
    symbols: [human_feedback, HumanFeedbackConfig, HumanFeedbackResult]
  - path: lib/crewai/src/crewai/flow/runtime/__init__.py
    symbols: [resume, pending_feedback, from_pending]
---

# 人工反馈与恢复

## 一句话职责

`@human_feedback` 让一个流程方法在产出之后停下来等人：阻塞式地当场读反馈，或者非阻塞地把流程冻结成一条「待反馈记录」，事后再用 `from_pending` + `resume` 接着跑。

## 机制是什么

装饰器本身只是元数据盖章：`HumanFeedbackConfig` 被写到方法的 `__human_feedback_config__` 上，流程定义构建时被提升成 `FlowHumanFeedbackDefinition`，真正的收集与路由由引擎驱动。

配置项有九个，分三组：

- **呈现**：`message`（给人看的提示）、`emit`（可选的结果标签集）、`metadata`（透传给企业集成的附加信息）。
- **判定**：`llm`（把自由文本反馈压成某个 emit 标签）、`default_outcome`（没拿到反馈时用哪个）、`provider`（自定义反馈通道）。
- **学习**：`learn`、`learn_source`、`learn_strict`。

`emit` 一旦有值，这个方法在定义层就被强制标成路由方法（见 d02）——反馈压出的结果是标签，标签是下一跳的事件名。而 `default_outcome` 必须有 `emit` 才能用，且必须落在 `emit` 里，这两条在装饰期就校验。

`HumanFeedbackResult` 是结果载体：`output`（给人看的产出，也就是要审的东西）、`feedback`（原始反馈文本）、`outcome`（压出的标签，无 `emit` 时为 `None`）、`timestamp`、`method_name`、`metadata`。它被追加进流程的 `human_feedback_history`，同时设为 `last_human_feedback` 供下游读取。

## 怎么实现的

**阻塞路径：当场读。** 引擎在方法执行完、`POST_STEP` 钩子之前插入反馈步骤。有 `provider` 就用 provider（可能同步返回字符串，也可能返回协程，都会 await）；没有就落到内置控制台实现：发 `HumanFeedbackRequestedEvent`、暂停 rich 的实时刷新、打印产出与提示、`input()` 读一行、发 `HumanFeedbackReceivedEvent`、恢复实时刷新。这两个事件的开闭被 `try/finally` 包着，保证异常时实时刷新能恢复。

**非阻塞路径：抛异常当控制流。** provider 若抛 `HumanFeedbackPending`，引擎把它当「暂停」而不是「失败」：

- 方法级：`_execute_method` 捕获后补上方法名到上下文，必要时创建默认持久化后端，发 `MethodExecutionPausedEvent`，把追踪的恢复上下文与方法输出列表写进上下文，调 `save_pending_feedback` 落盘，然后**把这个异常对象作为返回值返回**，而不是重新抛出——于是 `flow.kickoff()` 的返回值就是它，调用方用 `isinstance` 判断即可，不需要 try/except。
- 流程级：`kickoff_async` 在入口执行段外面再捕一层，做同样的事并 `return e`，让暂停能穿过整个 kickoff 变成返回值。

**暂停记录的构成。** `PendingFeedbackContext` 是恢复所需的全部信息：流程 id 与类名、方法名、被审的产出、提示文案、emit、default_outcome、metadata、序列化后的 llm 配置、请求时间、`execution_uuid`（恢复时沿用同一个追踪执行）、`trace_context`（上一段的追踪与根 span，用于接续）、`method_outputs`（暂停前已跑过的方法输出，使恢复后 `outputs.<step>` 仍可解析）。序列化走 `to_dict`，对任意值做逐级降级：标量原样、容器递归、Pydantic dump、dataclass asdict、其余 `str()`。

**恢复：`from_pending` 造实例，`resume` 补完这一步。** `from_pending` 先从持久化后端 `load_pending_feedback` 拿 (状态, 上下文)，拿不到就抛；然后造实例（声明式流程需要传定义，因为检查点里没有可调用对象），灌入状态、恢复方法输出列表、挂上待反馈上下文、置 `_is_execution_resuming = True`，并把 `_flow_match_id` 设为 `flow_id`——源码注释解释了原因：恢复阶段强制 `current_flow_id` 等于它，用量监听器与执行边界判定都靠这两个值相等来找到自己的事件。

`resume` 只做同步壳：检测到已有运行中的事件循环就报错并提示改用 `resume_async`（避免 `asyncio.run` 嵌套），否则 `asyncio.run` 转进去。

`resume_async` 的主体动作：刷新 `_cel_now`（流程可能挂了好几天，表达式要看到今天）、把 `current_flow_id` 强制设为 match id、重装用量聚合监听器、用待反馈上下文里的 `execution_uuid` 接续追踪并把 `parent_otel_context` 指向暂停时的追踪上下文。然后进 `_resume_async_body`。

**恢复体：先定标签，再往下推。** `_resume_async_body` 的顺序是：

1. 开 flow scope；
2. 补发一个 `MethodExecutionStartedEvent`（暂停时开了方法的开始事件但没有结束事件，这里配对）；
3. 跑 `_finalize_human_feedback`：没有反馈文本时按 `default_outcome` → `emit[0]` 的顺序取；有反馈且有 `emit` 时调 LLM 压缩成标签；`emit` 存在但压不出标签就抛 `HumanFeedbackCollapseError`；
4. 记住这一段产生的 lesson（若开了 `learn`），失败且 `learn_strict` 时要把刚记进历史的那条弹掉、把 `last_human_feedback` 还原，然后重新抛——源码注释说明理由：review 仍然处于待处理状态，重试时要能再记一次；
5. 把方法加进 `_completed_methods`、持久化、清空待反馈上下文并删掉持久化里的待反馈记录；
6. 发 `MethodExecutionFinishedEvent`（`emit` 时结果是标签，否则是结果对象）；
7. 清掉恢复标志，把输出追加进 `_method_outputs`（`emit` 时带上 `human_feedback` 字段，使表达式读到的 `outputs.<方法>` 是完整的反馈结果而不是标签）；
8. 若有标签就按标签触发监听器，否则按方法名触发；
9. 收尾：`OUTPUT` 与 `EXECUTION_END` 钩子、排空事件与记忆写入、发 `FlowFinishedEvent`、返回。

第 8 步外面还有一层捕获：若下游又抛 `HumanFeedbackPending`（多个人工审核点串联），就把新上下文挂回实例、再次落盘、发 `FlowPausedEvent`，返回新异常——于是「暂停 → 恢复 → 又暂停」可以无限接力。

**标签压缩有三级降级。** `_collapse_to_outcome` 先把 LLM 解析成实例（字符串建 `LLM`，`BaseLLM` 直接用）；然后动态构造一个只有一个字段的 Pydantic 模型，该字段是 `Literal[所有 emit 标签]`——用 `Literal` 而不是普通字符串字段，是为了借结构化输出的 schema 约束把模型输出限死在合法标签集内。调 `call(response_model=...)` 失败就降级成普通提示；普通提示也失败就抛 `HumanFeedbackCollapseError`。成功的返回值若还是字符串，先尝试 `json.loads`，失败则退到 `_match_collapse_outcome` 做文本匹配（先精确忽略大小写，再找「包含且最长」的标签）。

**`learn`：把人工反馈沉淀成记忆。** 开 `learn` 且流程有记忆时，两个时机各跑一次 LLM：

- **审前**（`_pre_review_with_lessons`）：拿「方法与产出」去记忆里召回 lessons，让 LLM 把产出改进一版再给人看。这里有一条很谨慎的守卫 `_has_hidden_values`：只有当产出是字符串或 Pydantic 模型、且模型里没有 Secret / 被排除的字段 / 非默认的私有属性时才改写。理由是模型只看得到可见字段，重建出来的对象会丢掉隐藏值——宁可原样送审也不丢数据。
- **审后**（`_distill_and_store_lessons`）：把人工反馈蒸馏成可复用的 lesson 列表写回记忆，`learn_source` 作为来源标签（默认 `hitl`）。无函数调用的模型走纯文本路径，按行切、去掉 `NONE` 行。

两处都显式放行 `HookAborted`（钩子中止不能被当成普通失败吞掉），其余异常在 `learn_strict=False` 时降级并告警，`True` 时上抛。

**`@ask()` 的旁路。** 流程方法里还可以调 `Flow.ask()` 主动要输入，它走 `InputProvider` 链（实例 → `flow_config.input_provider` → 控制台），并且在等输入前会做一次尽力而为的状态检查点（`_ask_checkpoint`），崩溃后至少状态还在。这条路径与 `@human_feedback` 不同：`ask` 是方法内部要值，`human_feedback` 是方法产出的审核关卡。

## 关键取舍

**源码事实：** 暂停用异常表达，但最终作为**返回值**交给调用方。这条设计让「流程暂停」不需要调用方写 try/except，代价是返回值类型变成「结果或暂停信号」的联合，调用方必须判断。

**源码事实：** `HumanFeedbackResult.output` 与「流程最终结果」是两回事。带 `emit` 时方法返回值被替换成标签用于路由，真实产出被暂存在 `_human_feedback_method_outputs` 里，等 `_execute_method` 收尾时替换回输出条目。这个「暂存再替换」是为了让「路由用标签」和「最终结果用产出」两个需求共存。

**源码事实：** 标签压缩用 `Literal[emit 元组]` 动态建模型。这是把「输出必须落在合法集合内」的约束压给结构化输出层，而不是事后校验。降级路径则退回文本匹配，宁可做出最佳猜测也不失败。

**源码事实：** `learn` 的审前改写有一道「有隐藏值就不改」的守卫。这是明确承认「LLM 往返会丢数据」并选择保守。

**源码事实：** `learn_strict` 只影响两类失败：审前改写失败与蒸馏失败。而 lessons 写入记忆是后台行为，写入失败不会被上抛——docstring 明确写了这个例外。

**架构推论：** 恢复不是「从某个方法继续」，而是「补完那个方法」。暂停发生在方法执行中的反馈步骤，恢复时重跑的是「收尾这一步」：拿反馈、定标签、发事件、推进下游。这解释了为什么 `_method_outputs` 必须随上下文落盘——恢复时并没有保存在内存里的方法栈，所有上下文都要从数据重建。

**架构推论：** 恢复必须沿用原来的 `execution_uuid` 并接续追踪上下文，否则一次人工审核会把同一次执行切成两条不相干的追踪记录。这也是为什么 `PendingFeedbackContext` 里带着追踪信息——它把「观测」也纳入了「恢复所需状态」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/flow/human_feedback.py` | `human_feedback` | 兼容导入路径，转发到 DSL 实现 |
| `lib/crewai/src/crewai/flow/human_feedback.py` | `HumanFeedbackConfig` | 九个配置项的载体，装饰器盖在方法上的元数据 |
| `lib/crewai/src/crewai/flow/human_feedback.py` | `HumanFeedbackResult` | 单次反馈的结果：产出、原文、标签、时间、方法名、元数据 |
| `lib/crewai/src/crewai/flow/human_feedback.py` | `_pre_review_with_lessons` / `_distill_and_store_lessons` | 审前用历史 lesson 改进产出、审后蒸馏新 lesson |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `from_pending` | 从持久化的待反馈记录重建可恢复实例 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `pending_feedback` | 暴露当前待反馈上下文 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `resume` | 同步恢复壳，检测事件循环冲突 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_finalize_human_feedback` / `_collapse_to_outcome` | 定标签：默认值、LLM 压缩、降级匹配 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_request_human_feedback` | 内置控制台反馈通道与事件开闭 |

## 参考

- [CrewAI 源码：human_feedback.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/flow/human_feedback.py)
- [CrewAI 文档：Flows 概念](https://docs.crewai.com/en/concepts/flows)
- [CrewAI 源码：async_feedback 类型与待反馈上下文](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/flow/async_feedback/types.py)

## 与主核 / AI 的关系

「暂停 → 落盘 → 恢复」这套结构，与主核里的事务回滚与崩溃恢复是同一类问题的两套解法。主核的恢复依据是 WAL 里的物理/逻辑日志，恢复目标是「数据结构一致」；这里的恢复依据是待反馈记录里的上下文，恢复目标是「继续这次业务会话」。前者要求可重放且幂等，后者只要求「把这一步补完」——所以这里不需要日志序号、不需要撤销操作，只需要一份足够完整的快照。

两者的共同点是**恢复所需的状态必须显式列举**。主核的日志记录必须包含重做所需的全部信息，漏一项就无法恢复；这里的 `PendingFeedbackContext` 也一样，`method_outputs`、`execution_uuid`、`trace_context` 都是后来补进上下文里的字段（docstring 里明确标注了「旧记录没有这个字段」的兼容处理）。这是同一种失效模式：恢复机制漏字段，要等真实事故才会发现。

与主核里「多阶段提交」相比，这里的暂停是单阶段的：把状态写下去就结束，没有「准备-提交」两段。代价是若在写盘过程中崩溃，会出现状态写了但待反馈记录没写（或反之）的窗口——源码没有对此加事务，靠的是 SQLite 单连接内的两次写入与 `store_lock`。

对 AI 工程的实际意义在于：人工审核是 agent 流程最难做的部分，因为它把「机器时间」和「人的时间」解耦。这套机制的做法值得借鉴的三点：把暂停表达成可返回的普通值而不是异常抛出；把恢复所需的上下文完整序列化（含可观测性上下文）；把「自由文本反馈」压成受限标签集再用于路由，且压缩失败时有明确的降级与明确的报错两种选择。
