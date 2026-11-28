---
project: ragas
feature: agent-metrics
title: Agent 与会话指标
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/agent_goal_accuracy/metric.py
    symbols: [AgentGoalAccuracyWithReference, AgentGoalAccuracyWithoutReference]
  - path: src/ragas/metrics/collections/tool_call_accuracy/metric.py
    symbols: [ToolCallAccuracy]
---

# Agent 与会话指标

## 一句话职责

面向多轮对话与 Agent 轨迹的两个指标：`AgentGoalAccuracy` 判断「这通会话最终有没有达成目的」（有/无参考两种形态），`ToolCallAccuracy` 判断「Agent 调用的工具序列与参考序列对不对得上」。

## 机制是什么

前四组 B 组指标（忠实度、相关性、上下文质量、事实正确性）都以「单轮问答」为输入：一个问题、一个答案、若干上下文。这两个指标的输入形态完全不同——**输入是一整条消息列表**，含人类消息、AI 消息与工具输出。这是 ragas 里唯一把「轨迹」当一等公民的地方，也是它必须定义 `HumanMessage`/`AIMessage`/`ToolMessage`/`ToolCall` 这套消息类型的原因。

两个指标走的路线截然不同：

| 指标 | 判定方式 | 依赖模型 | 输出 |
|---|---|---|---|
| `AgentGoalAccuracy` | 先把轨迹压成「目标」与「终态」两句自然语言，再比这两句是否一致 | 两次 LLM 调用 | 二值 0/1 |
| `ToolCallAccuracy` | 提取轨迹里的工具调用，与参考调用做序列对齐 + 参数比对 | 零次 | 连续 0–1 |

前者是「语义判定」，后者是「结构比对」。放在一起看，正好说明 agent 评测的两条互补路线：结果对不对（goal），过程对不对（tool calls）。

## 怎么实现的

**目标达成：同一套提示，两种比较对象。** 两个类共用同一个 `InferGoalOutcomePrompt`，它把整条轨迹渲染成文本，一次调用同时产出两个字段：`user_goal`（用户想干什么）与 `end_state`（最终发生了什么）。差异只出现在第二步比较的是哪一对：

- `AgentGoalAccuracyWithReference`：`ascore(user_input, reference)`，比较 `reference` 与 `end_state`，完全忽略推断出来的 `user_goal`；
- `AgentGoalAccuracyWithoutReference`：`ascore(user_input)`，比较 `user_goal` 与 `end_state`。

所以「无参考版」并不是缺了参考就判不了，而是**把参考换成了模型自己从轨迹里反推的目标**。这条设计的前提是一个判断：一条正常结束的 agent 轨迹里，用户想做什么通常在第一句人类消息里就说清楚了，而且工具调用与最终回复会把这个意图反复强化。代价是**模型自证**：如果它连目标都推错，后面的比较毫无意义，而这种情况在指标里表现为「一致」（因为目标和终态都来自同一次错误推断）。

**比较的返回值形态。** `CompareOutcomeOutput` 有两个字段：`reason`（解释）与 `verdict`。`verdict` 的类型是 `Literal["0", "1"]`——是**字符串**而不是整数或布尔，比较处再 `int(result.verdict)` 转回数字。用字符串常量的理由在提示装配层：字面量枚举比整数更不容易被模型写成「1.0」「true」「是」，减少一次解析歧义。

**轨迹渲染。** 两个类都有一份逐字相同的 `_format_conversation`，对每条消息调它的 `pretty_repr()` 后按行拼接。`pretty_repr` 是消息类型自己的方法：人类消息渲染成「Human: ...」，工具消息渲染成「ToolOutput: ...」，AI 消息若带工具调用则额外追加一段「Tools:」列表，每行形如「工具名: 参数字典」。所以轨迹的「可读性」被下沉到消息类型上，指标只负责拼行。

**输入校验的不对称。** 带参考版检查 `user_input` 是 list 且 `reference` 非空；无参考版只检查 `user_input`。两者都不检查消息条数——空列表会一路走到提示拼装，得到一个只含空白轨迹的请求，最终大概率被判成 0。

**别名。** 文件末尾有一行 `AgentGoalAccuracy = AgentGoalAccuracyWithReference`，把「默认带参考」这个取向写进名字。这也意味着按名字导入时拿到的是带参考版本，要用无参考版必须显式导入完整名字。

**工具调用：零模型的四步。** `ToolCallAccuracy.ascore` 的流程是：

1. **提取预测**：遍历消息列表，把所有 `AIMessage` 的 `tool_calls` 收进一个平铺列表（一条 AI 消息可以带多个调用）。
2. **三种边界提前返回**：两个列表都空 → `1.0`（空对空视为完全对齐，这是真空真）；只有预测非空 → 警告并 `0.0`；只有参考非空 → 警告并 `0.0`。
3. **可选的排序**：`strict_order=False` 时用 `sorted_key_for_tool_call` 对两侧排序，这个键是「工具名 + 按参数名排序后的名值对字符串」，因此参数书写顺序不影响比较。
4. **分数合成**：`sequence_aligned` 是 0/1 的二值因子（严格模式要求名字序列完全相等，宽松模式要求排序后相等）；参数分是逐对 `exact_match_args` 的累加值再除以参考长度；长度不等时再乘一个覆盖率惩罚。

最终分 = 参数平均分 × 覆盖率惩罚 × 序列对齐因子。

**参数比对的细节。** `exact_match_args` 只遍历**参考调用的键**：参考里有、预测里也有且 `str()` 后相等，得 1 分；预测多带的键不计分也不扣分。用 `str()` 比较意味着整数 `1` 与字符串 `"1"` 判为相同，而 `1` 与 `1.0` 判为不同——这是在「模型可能把数字写成字符串」与「类型严格」之间取了偏宽容的一侧。两侧都空返回 1.0，参考空而预测非空返回 0.0。

**长度不等时两侧都被截断。** `zip` 只会配对到较短的长度，所以多出来的预测调用完全不参与打分，只通过覆盖率惩罚「按比例」扣分——惩罚是 `compared_count / len(reference)`。注意**预测比参考长时惩罚为 1**（`compared_count` 等于参考长度），也就是说「多调用了工具」这件事在长度维度上不被惩罚，只有「少调用了工具」才扣。

## 关键取舍

**源码事实：** 无参考版用「同一次推断产出的 `user_goal`」当参考。这既是它存在的理由（无标注也能评测），也是它最弱的地方：目标与终态出自同一次模型调用，两者一致的概率天然偏高，分数有系统性乐观偏差。

**源码事实：** `verdict` 用字符串字面量 `"0"/"1"` 而非整数，是为了让结构化输出的枚举更稳定，代价是每次比较多一次 `int()` 转换。

**源码事实：** `ToolCallAccuracy` 的序列对齐是**二值因子**，不是部分匹配比例。只要序列不一致，整题归零，即使参数全对。这把「调用顺序」当成不可协商的约束。

**源码事实：** 参数比对用 `str()` 而非类型化比较，且只覆盖参考的键。这让「模型把 `1` 写成 `"1"`」不至于失败，但也让「漏传参数」无法被发现（漏传的键会判 0 分，但多传的键完全不罚）。

**源码事实：** 「预测与参考都为空」返回 1.0。这是真空真（vacuously true）：没有工具调用需求的任务，Agent 也没调用工具，确实算对齐。副作用是如果一批样本里大量样本本就不需要工具，这个指标会被动拉高。

**架构推论：** 两个指标都要求输入是消息列表，而不是单轮样本的字段。这使得它们只能跑在 `MultiTurnSample` 上，无法与单轮指标混在同一份数据集里一次性评估——`evaluate` 的执行循环会按样本类型只提交支持该类型的指标。

**架构推论：** 目标达成指标用「压成两句自然语言再比较」而不是「把两条轨迹逐条比对」，是把「过程」彻底丢弃、只保留「结果」。这是一种刻意的粗粒度：只要终态对得上，中间走了多少弯路都不影响分数。要求过程也对的场景必须去看工具调用指标，而它又完全不看自然语言结果。两个指标互为补集，不能互相替代。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/agent_goal_accuracy/metric.py` | `AgentGoalAccuracyWithReference` | 带参考版：比较参考终态与推断终态 |
| `src/ragas/metrics/collections/agent_goal_accuracy/metric.py` | `AgentGoalAccuracyWithoutReference` | 无参考版：比较推断目标与推断终态 |
| `src/ragas/metrics/collections/agent_goal_accuracy/metric.py` | `_format_conversation` | 用消息自身的渲染方法拼出轨迹文本 |
| `src/ragas/metrics/collections/agent_goal_accuracy/util.py` | `InferGoalOutcomePrompt` | 一次性推断目标与终态的提示与输出模型 |
| `src/ragas/metrics/collections/agent_goal_accuracy/util.py` | `CompareOutcomePrompt` | 比较两个自然语言结果，输出字符串形式的判定 |
| `src/ragas/metrics/collections/tool_call_accuracy/metric.py` | `ToolCallAccuracy` | 工具调用准确率的四步计算 |
| `src/ragas/metrics/collections/tool_call_accuracy/metric.py` | `_is_sequence_aligned` | 严格/宽松两种序列对齐判定 |
| `src/ragas/metrics/collections/tool_call_accuracy/util.py` | `exact_match_args` | 逐参数比对，按参考键覆盖率归一 |
| `src/ragas/metrics/collections/tool_call_accuracy/util.py` | `sorted_key_for_tool_call` | 与参数书写顺序无关的排序键 |
| `src/ragas/messages.py` | `AIMessage` | 承载 `tool_calls` 的消息类型 |

## 参考

- [ragas 源码：agent_goal_accuracy/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/agent_goal_accuracy/metric.py)
- [ragas 源码：tool_call_accuracy/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/tool_call_accuracy/metric.py)
- [ragas 文档：Agent 评测指标](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/agents/)
- [ragas 文档：多轮对话指标](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/multi_turn/)

## 与主核 / AI 的关系

这两个指标暴露了评测工具的一个结构性困境，与主核里「门禁 vs 度量」的分野完全对应。

**二值门禁与连续度量的分工。** 目标达成是二值：达成或没达成，没有中间态。工具调用是连续：序列对齐是二值因子乘上参数覆盖率，于是「调对了工具但参数写错一半」能拿到 0.5 左右的分数。主核里约束检查（`CHECK`）与性能指标的分工是同一个思路——**硬的用二值、软的用连续**。把两者混成一个分数会让「错得离谱」与「差一点点」看起来一样。

**自证问题 vs 自我校验。** 无参考版把「参考」换成了模型自己推断的目标，这在数据库语境里相当于「用自己算出来的索引去校验自己写的数据」——闭环内一致，闭环外无从验证。主核靠写前日志与恢复校验打破这种自闭环，评测这边只能靠外部标注。**任何自证型指标给出的高分都只是内部一致性，不是正确性。**

**参数比对的空值方向。** `exact_match_args` 在「参考与预测都空」时返回满分、在「参考空而预测非空」时为 0。这与主核里「NULL 与空串不是一回事」的取向一致：空对空是合法的相等，一侧有值一侧没有是对不上。反过来如果把「都空」算成 0 分，所有不需要参数的工具都会无故失分。

对 agent 工程最直接的三条：其一，**结果与过程必须分开评**，用「终态是否匹配」代替不了「调用是否正确」，反之亦然。其二，**参数比对要决定多传是否算错**——本实现的选择是「多传不罚、少传按比例扣」，如果你的 Agent 可能乱调工具，这个指标会高估它。其三，**无参考指标的高分要打折看**，它的目标是模型自己说的，不是用户说的。
