---
project: crewai
feature: tool-usage
title: 工具调用编排与重复检测
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/tools/tool_usage.py
    symbols: [ToolUsage, use, _check_tool_repeated_usage]
---

# 工具调用编排与重复检测

## 一句话职责

把「模型说要调工具」这一段松散文本，变成一次带缓存、带配额、带重复检测、带失败分类、带事件记录的真实调用，并在出错时按次数上限决定是重试还是告诉模型「换条路」。

## 机制是什么

`ToolUsage` 是工具系统的调度台。它持有工具清单、工具描述、任务、agent、`function_calling_llm`、缓存处理器与指纹上下文，对外只暴露两个入口：`use(calling, tool_string)` 与 `ause(...)`，接收的是**已经解析好的** `ToolCalling`。

解析本身是另一条链路：`parse_tool_calling` → `_tool_calling`，先尝试从 agent 已经给出的 `action.tool` 与 `action.tool_input` 里直接取（`_original_tool_calling`），失败才让一个 LLM 把文本转成结构化调用（`_function_calling`，用 `Converter` 约束成 `ToolCalling` 或 `InstructorToolCalling` 两种模型之一）。

真正值得看的是 `_use` / `_ause` 这四百多行里的判断顺序，以及三个内建的状态位：`_run_attempts`（解析/执行尝试次数，上限由 `_max_parsing_attempts` 控制）、`last_raw_result`（上次的原始结果）、`last_failure`（上次的结构化失败）。

## 怎么实现的

**重复检测在事件之前。** `_check_tool_repeated_usage(calling)` 的实现很短：取 `tools_handler.last_used_tool`，比较**消毒后的工具名**与**未消毒的 arguments**是否都与本次相同。相同就返回真。此时不执行工具，直接返回一条国际化文案 `task_repeated_usage`，并调 `_telemetry.tool_repeated_usage`。源码注释点明这一步在事件发射之前，所以可以安全地提前返回——不会出现「发了 started 却没有 finished」的悬挂事件。

**事件的时序。** 通过重复检测后：记 `started_at`，若 `self.agent` 存在则拼 `event_data`（agent key/role、`self.action.tool`、tool_input、run_attempts，有指纹就并入指纹字段，有 task 就补 task 名与 id），发 `ToolUsageStartedEvent` 并置 `started_event_emitted`。收尾在 `finally` 里：`if started_event_emitted and not error_event_emitted` 才发 `ToolUsageFinishedEvent`。这个双标志组合保证**出错时不会同时发 finished 与 error**。

**缓存优先。** 若 `tools_handler.cache` 存在，先把 arguments 序列化成字符串（字典走 `json.dumps`，其他走 `str`），用 `cache.read(tool=消毒名, input=input_str)` 读；读到就用，并置 `from_cache=True`。

**工具查找是二次的。** `self.tools` 里按消毒名找 `available_tool`——注意这与 `_select_tool` 已经找到的 `tool` 是两次独立查找。前者是 `CrewStructuredTool`（真正被 invoke 的那个），后者可能来自 `_original_tool_calling` 的选择。配额检查用的是 `available_tool` 的 `max_usage_count` / `current_usage_count`，而执行用的是 `tool.invoke`。

**配额超限。** `_check_usage_limit` 返回文案字符串时：把文案设为 `result`、写 `last_raw_result`、构造 `ToolFailure(reason=USAGE_LIMIT)` 赋给 `last_failure`、调 telemetry，然后 `_format_result` 包一层。

**正常执行的参数过滤。** 有 arguments 时先试 `tool.args_schema.model_json_schema()["properties"].keys()` 做白名单过滤，只保留 schema 认识的键，再 `invoke`；这一步整体包在 `try` 里，一旦取 schema 失败（例如 `args_schema` 是 `None`）就 `except` 掉，退回**未过滤**的原始 arguments 再调一次。所以严格 schema 的工具能拒绝多余参数，而宽松或无 schema 的工具也不会因此报错。

**缓存写入与 `cache_function`。** 执行后若 `tools_handler` 存在：默认 `should_cache=True`，然后从 `available_tool._original_tool.cache_function` 取策略（没有则取 `available_tool.cache_function`），取到时调 `cache_func(calling.arguments, result)` 决定是否缓存，最后 `tools_handler.on_tool_use(...)` 落缓存。**原始工具优先**这一点与 `BaseTool.to_structured_tool` 的反向指针配套。

**结果落账。** `last_raw_result = result`；`last_failure = detect_tool_failure(result)`（只认显式 `ToolFailure`）；`result = tool.format_output_for_agent(result)`；组一个 `data` 字典（result、工具名、参数），若 `available_tool.result_as_answer` 为真**且** `last_failure is None` 才加 `result_as_answer` 键——注释说明「失败的工具不能变成最终答案」。然后追加进 `agent.tools_results`。

**配额自增的两条分支。** 优先调 `available_tool._increment_usage_count()`（结构化工具内部会同步回原始工具），否则退化成直接 `available_tool.current_usage_count += 1`。两条都只在 verbose 且有上限时打印用量。

**失败分支与重试。** 执行抛异常时：`on_tool_error` 发 `ToolUsageErrorEvent`、置 `error_event_emitted=True`、`_run_attempts += 1`。若超过 `_max_parsing_attempts`：写 `last_raw_result` 与 `last_failure = failure_from_exception(e)`、任务错误计数加一、返回一条「失败并转场」的文案。否则置 `should_retry=True`，`finally` 发完 finished 事件后**递归调用自身** `self.use(calling=..., tool_string=...)`。

**重试的收口在 finally 之后。** 注释写明「在 finally 之后处理重试，确保 finished 事件已发出」。这让每次失败的尝试都有一对 started/finished 事件，重试是新的一个完整回合，而不是同回合内的静默循环。

**名字模糊匹配。** `_select_tool` 用 `SequenceMatcher` 对所有工具名与输入名算相似度，降序排；命中条件是「完全相等」或「相似度 > 0.85」。找不到就发 `ToolSelectionErrorEvent` 并抛异常，文案带全部可用工具描述；若名字为空字符串，文案换成「我忘了动作名」的另一种说法。

**参数解析的四级降级。** `_validate_tool_input` 依次尝试：`json.loads` → `ast.literal_eval` → `json5.loads` → `json_repair.repair_json` 后 `json.loads`。四级都失败才发 `ToolValidateInputErrorEvent` 并抛异常。所以模型给出的「带尾逗号、单引号、缺括号」这类 JSON 都能被救回来。

**解析失败的 telemetry 细节。** `_tool_calling` 重试超限时调 `tool_usage_error(llm=...)` **不传 tool_name**，注释解释：这是「调用解析失败」，模型想要的工具从未被确定，把原始模型输出塞进指标维度会成为无界基数。这是把可观测性的基数控制写进了代码注释。

## 关键取舍

**源码事实：** 重复检测只比对**紧邻的**上一次调用（`tools_handler.last_used_tool`），不做历史窗口或计数。所以「A、B、A」不会被判重复，「A、A」才会。这是单步去抖，不是循环检测。

**源码事实：** 重复检测比较的是 arguments 的原始对象相等性。模型对同一意图给出字段顺序不同但语义相同的字典，不会被判重复——除非字典相等。

**源码事实：** 执行路径里存在两次工具查找（`_select_tool` 与 `next(...)`），两者结果可能不同。配额与缓存策略都读 `available_tool`（第二次查找的结果），若第二次找不到则 `available_tool` 为 `None`，配额检查直接跳过（`_check_usage_limit` 里 `hasattr(None, "max_usage_count")` 为假），于是**配额被静默绕过**。

**源码事实：** 配额检查在缓存读之后、且与是否命中缓存无关：`_check_usage_limit` 总是先跑，超限时把 `result` 覆盖成配额文案，让后面的 `elif result is None` 不再执行。所以命中缓存不消耗配额，但「已达上限」的判定优先于缓存。

**源码事实：** 重试是递归调用 `use`，每次都在 `finally` 之后。所以 `_run_attempts` 会一路累加并跨重试保留，最终由同一个上限终止。

**架构推论：** 这套流程把「模型输出质量」当成一等公民来容错：参数解析四级降级、工具名 0.85 相似度匹配、失败后重试再转场。这三层加起来的效果是「模型说得不太对也大概率能跑起来」，代价是错误更容易被掩盖——一个名字拼错 90% 相似的工具会被静默匹配到，而不是报错让模型改。

**架构推论：** 事件与执行是两套独立的生命周期，靠 `started_event_emitted` / `error_event_emitted` 两个布尔维持互斥。这种「手工保证不变量」的写法在流程分支多时容易漏，所以 `finally` 里那一个条件判断是唯一的收口点——任何新增的提前返回都必须注意它。

**架构推论：** `result_as_answer` 与 `last_failure` 的联动说明「工具结果当成最终答案」是一条需要正确性保证的捷径：一旦工具失败，这条捷径必须关闭，否则 agent 会把一句错误文案当成最终交付。源码注释专门标注了这一点，且说明下游 `process_tool_results()` 会独立读回该状态。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage` | 工具调度台：解析、缓存、配额、事件、重试 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage.use` | 同步入口，按解析结果分派到 `_use` |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage.ause` | 异步入口，分派到 `_ause` |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._use` | 同步执行主流程，含事件、缓存、重试 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._ause` | 异步执行主流程，与 `_use` 同构 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._check_tool_repeated_usage` | 与上一次调用比对名字与参数 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._check_usage_limit` | 只读检查配额，返回错误文案或 None |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._select_tool` | 按名字消毒加相似度匹配选工具 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._validate_tool_input` | 四级 JSON 降级解析 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._tool_calling` | 解析调用，失败重试，超限降级为错误对象 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._format_result` | 计数工具使用次数并周期性回灌工具清单 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage.on_tool_use_finished` | 发 finished 事件，附带可上报的失败信息 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage._build_fingerprint_config` | 把 agent 与 task 指纹装成 config 而非参数 |

## 参考

- [CrewAI 源码：tools/tool_usage.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/tools/tool_usage.py)
- [CrewAI 文档：Tools](https://docs.crewai.com/en/concepts/tools)
- [Python 文档：difflib.SequenceMatcher](https://docs.python.org/3/library/difflib.html)

## 与主核 / AI 的关系

主核里对应的是「查询执行器的算子调度」：拿到一个计划节点，解析参数、决定走缓存还是重算、记录统计、失败则按策略重试或报错。差别在于主核的参数从解析器来，类型已确定；这里的参数从自然语言来，所以有一整套**降级解析链**——这是 AI 工程独有的那一层，也是这页最值得移植的部分。

与 AutoGen 的工具调用相比，这里明显多出三项。第一是**重复调用检测**：AutoGen 的 `tool_agent_caller_loop` 没有任何去抖，模型反复要求同一个调用就会反复执行；这里用「紧邻上一次同名同参」判定并直接返回提示文案，把死循环的第一圈就掐掉。第二是**使用配额在调度层与工具层双查**：调度层在调用前读 `current_usage_count`，工具层在 `_claim_usage` 里加锁再查再增。第三是**失败与事件的严格配对**：靠 `started_event_emitted` / `error_event_emitted` 两个标志保证每次尝试有且只有一对开始/结束事件，失败后重试作为新的独立回合。

对 AI 工程最实用的一条是「解析失败不占工具名指标」。把模型的原始输出当作指标标签会制造无界基数，这个坑在自建 observability 时几乎必踩；这里的做法是解析失败只记「空名」，宁可少一个维度也不炸指标库。

另一条是重试的位置：递归调用放在 `finally` 之后而不是 `finally` 里面。放在里面会导致「上一个回合的 finished 事件在下一次执行之后才发出」，时间戳与因果关系全乱。这个细节直接决定了事件记录图能不能被正确重建——同一件事在后续的事件上下文页与事件记录图页里还会再出现。
