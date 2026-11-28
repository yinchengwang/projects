---
project: dify
feature: workflow-generator
title: 工作流自动生成器
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/generator/runner.py
    symbols: [WorkflowGenerator]
  - path: api/core/workflow/generator/types.py
    symbols: [WorkflowGenerateErrorCode, PlannerNodeDict]
---

# 工作流自动生成器

## 一句话职责

把一句自然语言描述变成一份可加载的草稿工作流图：先用一次短规划调用产出「有哪些节点、怎么连」的高层计划，再对**每个节点各发一次**并发的构建调用产出紧凑配置，最后补默认值、排版、修引用、做结构校验。

## 机制是什么

这个模块解决的是一个很具体的工程问题：**让模型一次生成整张图会失败，让它逐节点生成又要保证节点之间连得上。**

如果让模型一次吐出完整图的 JSON，节点数量一多就会出错——漏字段、连错边、引用不存在的变量。如果让它逐节点独立生成，每个节点都不知道别的节点长什么样，产出的配置无法拼成图。

所以流水线被切成三段：

| 阶段 | 调用次数 | 产出 |
|---|---|---|
| ROUTER | 0 或 1 | 从大工具目录里挑出相关子集 |
| PLANNER | 1 | 节点清单 + 边 + 起始输入声明 |
| BUILDER | N（每节点一次，并发） | 每个节点的紧凑配置 |

**图的结构由 planner 定，节点的内容由 builder 填。** builder 拿到的提示里包含完整的计划 JSON（含节点清单与边），所以它知道自己在图里的位置；但它只负责自己那个节点的 `data`，不负责连接。

这条分工的判据是「谁需要全局视野」：**拓扑需要全局，配置只需要局部**。planner 的产出短（几个节点，每个只有类型、标签、用途），所以一次调用能稳；builder 的产出复杂（一个节点的完整配置），所以单独一次调用、且可以并发。

`ROUTER` 是一个可选的第三阶段，只在工具目录**超过直接注入上限**时启用。它的存在理由同样是「上下文预算」：一个装了上百个工具的工作区，把所有工具定义塞给 planner 会挤掉真正重要的指令。

## 怎么实现的

**模式是「先定后落」。** 请求有三种取值：`workflow`、`advanced-chat`、`auto`。`auto` 不走额外的分类调用，而是**把选择权交给 planner**：提示里插一句「auto (choose workflow or advanced-chat)」，然后从 planner 回的 `mode` 字段读；若字段缺失或非法，就**看计划的终点节点类型**——有答案节点就是对话型，有结束节点就是工作流型；都没有就默认对话型。

三种来源，越靠后越宽松，且**任何一步的坏值都不会让生成失败**。注释直接写明「Lenient on purpose — a bad `mode` value must never fail the plan」。

还有一处细节：返回的信封里永远带**已解析的具体 mode**，绝不把 `auto` 这个哨兵漏给前端。未解析前的兜底映射是 `auto → advanced-chat`，理由写在注释里：「the same never-fail fallback the old standalone classifier used」。

**错误被建模成结构化条目，同时也保留旧字符串。**

`WorkflowGenerateResultDict` 里同时有 `error`（字符串，多条的 detail 用分号拼起来）与 `errors`（结构化列表，每项有 `code`、`detail`，可选 `node_id`）。注释解释了为什么两个都要：`error` 是与旧信封兼容，`errors` 是给前端做本地化与节点高亮。

`WorkflowGenerateErrorCode` 有十五个码，注释里有一条硬约束：「any change here MUST be mirrored in the FE i18n map」——**这个枚举是跨仓契约**，加一个码就得同时改前端。`node_id` 是可选字段，只在错误能定位到具体节点时才填。

**阶段级异常翻译成错误码。**

`_run_stage` 是统一包装：捕获 `_StageJSONError` 映射成 `INVALID_JSON`、`_StageSchemaError` 映射成 `INVALID_SCHEMA`、其余一律 `MODEL_ERROR`（并打完整堆栈）。

这个三级分类有明确用途：前两个是「模型答歪了」，提示可以改；第三个是「调用本身失败」（鉴权、配额、网络），前端应提示重试。

**LLM 调用有两条独立的重试线，且语义不同。**

第一条是 `_invoke_with_retry`：**传输层重试**。只对三个瞬时异常（连接错误、服务不可用、限流）重试，最多三次，退避 0.5 秒与 1.5 秒。注释明确排除了鉴权与参数错误：「those are permanent misconfigurations where a retry only adds latency and burns quota」。

第二条是 `_invoke_and_parse_json`：**解析层重试**。解析失败时重试**一次**，并在末尾追加一条纠错提示。只用一次的理由写得很实在：「each LLM call is expensive... One retry with a corrective hint catches the dominant failure mode — a stray markdown fence or trailing prose — without masking a model that simply can't follow the spec.」

所以最坏情况下一次 builder 调用会发出 `3 × 2 = 6` 次模型请求。这个乘积关系不写在任何地方，但它是实际成本上界。

纠错提示的**位置**有一个跨供应商的具体理由：它作为**最后一条用户消息**追加，而不是追加系统消息。注释说明了原因：「a system message appended after the user turn is silently dropped or reordered by several providers (Anthropic, Gemini)」。**这是一条真实的经验记录**——系统消息必须在最前面是很多供应商的实际约束，而 API 类型不会告诉你这件事。

**planner 的 schema 校验是严格的，且有第二次机会。**

`_validate_planner_schema` 要求：`nodes` 是非空列表、每个节点有非空 `node_type` 与非空唯一 `id`、`edges` 是非空列表、每条边的 `source` 与 `target` 都必须在节点 id 集合里。

任一不满足就抛 `_StageSchemaError`。而 `_run_planner` 捕获它之后**换一条不同的纠错提示再来一次**（`_PLANNER_SCHEMA_RETRY_HINT`，专门讲拓扑要求），然后再校验一次——这次失败就往上抛。

所以 planner 最多两轮、每轮最多三次传输重试。注意两轮的纠错提示不同：JSON 那轮讲「别加代码围栏」，schema 那轮讲「每个节点要有唯一 id、边的两端要引用这些 id」。**针对不同失败原因给不同的纠正**，比一句通用的「重试」有效得多。

这里有个边界：空 `nodes` 数组**能通过** schema 校验（函数在 `if not nodes: return parsed`），但会被上层当作 `EMPTY_PLAN` 错误。所以「空计划」不是 schema 错误而是业务错误，两者在错误码上分开。

**builder 的并发与快速失败。**

`_run_parallel_node_builders` 用 `ThreadPoolExecutor` 并发跑每个节点的构建，并发上限取 `min(配置值, 待建节点数)`。配置值从环境变量读，注释说明默认 6「enough to run the planner's recommended 3–6-node plans as a single wave」。

**它是读取时才取的**（一个函数而不是模块常量），注释写明是为了「让测试与在线配置重载不必重新导入模块」。

失败处理是快速失败：任一个 future 抛错，**取消其余待跑的**，然后重新抛出。注释解释了为什么不做「收集所有错误」：「one failed fragment aborts the whole graph, so queued builder calls would only burn quota and delay the error envelope」——注意它也承认「In-flight calls cannot be interrupted; they finish while the pool shuts down」。

**refine 模式靠 `keep` 标记省调用。** 若传入的 `current_graph` 里已有同 id 的节点、且计划里该节点标了 `action: "keep"`，则该节点**完全不调模型**，直接沿用旧配置。这是「改一个节点」场景的成本优化：只重建改动的那些。

builder 的提示会按节点类型裁剪：只有 LLM/问题分类器/参数提取器会得到「选定的模型」段（provider/name/mode 逐字复制）；只有工具节点会得到工具目录段（且若 planner 已指定具体工具，就只给那一个工具的定义而不是整个目录）；只有开始节点会得到起始输入声明段。**同一个提示模板，按类型填不同的段**，这是控制单次请求体积的主要手段。

**工具节点的「找到就用具体定义、找不到就给全目录」有一个前提。** `_find_planned_tool_entry` 先看结构化的 `tool` 选择器，找不到就退化到**在 purpose 文本里搜工具标识**。注释说这是为了「让更小/更老的模型仍然可用」——它们会按要求在 purpose 里写出 `provider/tool` 但漏掉可选的结构化字段。**这是在提示约束之外加的一层文本兜底**，属于对模型能力差异的务实妥协。

**后处理是「补、排、修、校」四步。**

`_hydrate_tool_nodes` 把工具节点的占位配置替换成目录里的真实元信息；`_postprocess_graph` 补安全默认值、按拓扑分层排版、去重边 id；`_validate_structure` 做最终结构检查（起止节点形状、容器拓扑、工具是否幻觉、变量引用是否指向会暴露它的节点）。

单独值得说的是**排版**：节点位置不是模型给的，而是后处理算出来的——先按拓扑算深度，同一深度的节点纵向叠（`_NODE_Y_STEP = 160`，注释说明默认节点高 100，所以留 60 的空隙）。**位置是纯粹的呈现问题，不该占用模型的注意力预算**，这是让它算而不是让它猜的正确判断。

文件类起始变量有一个显式的兜底：若 builder 没给必需的 `allowed_file_types`，就填上四种标准类型（文档/图片/音频/视频），但**刻意排除 `custom`**，因为那会反过来要求非空的扩展名列表。注释坦白了这是补丁：「The real fix is the builder now documenting and emitting these fields; this is the safety net that guarantees a loadable draft.」

**结构校验失败仍然返回部分图。** 注释写明：「We still return the partial graph so the caller can debug or salvage it.」所以失败时前端拿到的是「一个可能不完整的图 + 结构化错误」，而不是空图。**这让用户有可能手动修好它**，而不是从零重来。

**模块 docstring 记录了一份「故意不做」的清单。**

Mermaid 渲染、多解情况的语义自动修复、带「可修复/需用户介入」分类的多步校验引擎、模型目录过滤——四件都被明确推迟，理由写在最后一句：「If quality regresses below product threshold we add those back; for now the planner and bounded parallel node builders shipped behind cmd+k `/create` are enough.」

**这份清单本身是文档**：它告诉后来者「这些不是忘了做，是评估后不做」，避免每次读到缺失都想加一遍。

## 关键取舍

**源码事实：** 流水线是 ROUTER（可选）→ PLANNER（1 次）→ BUILDER（每节点 1 次，并发）→ 后处理与校验；图的结构由 planner 定，节点内容由 builder 填。

**源码事实：** `auto` 模式不额外调用分类模型，而是从 planner 的 `mode` 字段读，缺失或非法时按终点节点类型推断，再兜底成 `advanced-chat`；信封里永远不含 `auto` 哨兵。

**源码事实：** 两条独立重试线：传输层最多 3 次（只对连接/不可用/限流），解析层最多 1 次（追加纠错提示）。最坏情况一次 builder 调用发 6 次模型请求。

**源码事实：** 解析重试的纠错提示作为**最后一条用户消息**追加，注释说明追加系统消息会被部分供应商丢弃或重排。

**源码事实：** planner 的 schema 校验失败会换一条**专门讲拓扑要求**的提示重试一轮；空节点数组能过 schema 校验，但上层会判成 `EMPTY_PLAN`。

**源码事实：** builder 并发上限在调用时从配置读取；任一节点失败即取消其余待跑任务并抛错，已在飞的调用无法中断。

**源码事实：** refine 模式下标记 `keep` 且已存在的节点跳过模型调用，直接沿用旧配置。

**源码事实：** 节点位置由后处理按拓扑深度计算，不由模型产出。

**源码事实：** 文件类起始变量缺 `allowed_file_types` 时兜底填四种标准类型并刻意排除 `custom`，注释自述这是安全网而非正解。

**源码事实：** 结构校验失败仍返回部分图供调试或抢救；模块 docstring 明确列出四项「故意不做」的能力。

**架构推论：** 「结构由一次调用定、内容由 N 次调用填」是这张流水线的核心权衡。它把「一致性」压给了 planner（只有它需要全局视野），把「复杂度」分散到 builder（每次只处理一个节点）。**代价是 planner 的错误会传播到所有 builder**——一个错的边定义会让下游节点按错误的上下文生成配置，而 builder 无从发现。这也解释了为什么 planner 的 schema 校验最严格、且专门为它准备了一轮重试。

**架构推论：** 「传输重试」与「解析重试」分成两层而不是合成一层，是因为它们的**失败原因与代价完全不同**：传输失败不是模型的错，重试无副作用；解析失败是模型的错，重试要付出一整次调用的成本。分开之后，每层可以用适合自己的策略（指数退避 vs 一次性纠错提示），而合成一层只能取两者的折中。

**架构推论：** 把「故意不做」写进模块 docstring 是一个值得模仿的做法。工程上最难判断的不是「还缺什么」，而是「缺的那些是有意为之还是遗漏」。**用文档把「已知的边界」与「未知的缺陷」分开**，能让后续维护者不重复评估同一个决定。

**架构推论：** 「失败时返回部分图」与 b01 的「失败时产出失败事件而非抛错」是同一种取向：**把可修复的中间产物交给调用方**。它假设调用方（或用户）有能力判断与修补，而这个假设在画布型产品里是成立的——用户看得见图。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator` | 三段流水线的门面，含同步与流式两个入口 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._iter_generation_events` | 共享核心：规划、构建、后处理、校验 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._run_planner` | 规划调用，含 schema 失败后的第二轮纠错 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._validate_planner_schema` | 拓扑契约校验：唯一 id、边两端必须存在 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._run_parallel_node_builders` | 并发构建、快速失败与取消 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._run_node_builder` | 单节点构建，按类型裁剪提示段 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._resolve_prompt_tool_catalogue` | 工具目录路由与降级 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._run_tool_router` | 从大目录提取能力查询 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._invoke_with_retry` | 传输层重试，只针对瞬时异常 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._invoke_and_parse_json` | 解析层重试，纠错提示作为末条用户消息 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator._run_stage` | 异常到错误码的翻译 |
| `api/core/workflow/generator/runner.py` | `_resolve_generation_mode` | 三种来源的模式解析与兜底 |
| `api/core/workflow/generator/types.py` | `WorkflowGenerateErrorCode` | 十五个错误码，与前端 i18n 强耦合 |
| `api/core/workflow/generator/types.py` | `PlannerNodeDict` | 计划节点的最小契约：类型、标签、用途 |
| `api/core/workflow/generator/types.py` | `WorkflowGenerateResultDict` | 结果信封：图、消息、结构化错误、已解析模式 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：generator/runner.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/generator/runner.py)
- [dify 文档：工作流编排](https://docs.dify.ai/guides/workflow)
- [json-repair：容错 JSON 解析](https://github.com/mangiucugna/json_repair)

## 与主核 / AI 的关系

**「结构一次定、内容多次填」与主核的「解析一次、执行多次」同构。** 数据库把 SQL 解析成算子树是一次性的全局动作，然后每个算子各自执行、互不关心彼此。dify 的 planner 相当于解析器，builder 相当于算子。差别是数据库的解析结果是确定的、可校验的，而 planner 的输出来自模型、只能靠 schema 校验兜住一部分。**把不确定性集中在最少的调用上，是这类生成系统的通用手法。**

**两层重试与主核的重试分类一致。** 数据库区分「死锁可重试」与「约束冲突不可重试」，判据是「重试会不会改变结果」。dify 的传输层只重试瞬时错误、且明确排除鉴权与参数错误，判据完全相同。**分类重试比统一重试重要，因为对不可重试的错误重试既浪费又掩盖问题。**

**「纠错提示放最后一条用户消息」是一条供应商现实约束。** 这与主核里「SQL 提示（hint）必须写在特定位置才被解析器识别」是同一类问题：**协议的形式规范与实现的宽容度不总一致，必须按实现的约束写**。类型系统不会告诉你这件事，只有踩过才知道。

**「故意不做清单」与主核的已知限制文档同类。** 数据库也会在文档里列出「本版本不支持并行哈希连接」这类边界。**把边界写成文档，等于把「未知」变成「已知」，这是可维护性的关键分界。**

对 agent 工程最实用的五条。其一：**一次生成整图会失败，逐节点生成又要靠计划约束**，两段式是这类任务的最低可行结构。其二：**传输重试与解析重试必须分开**，前者可以指数退避，后者只能一次纠错——否则一次模型犯傻会被放大成多次调用。其三：**纠错提示要作为最后一条用户消息**，追加系统消息在部分供应商会被丢弃。其四：**并发构建失败要快速取消**，否则排队中的调用只会白烧配额。其五：**错误码枚举是跨仓契约**，加一个码就得同步前端 i18n，否则前端会显示原始英文 detail。
