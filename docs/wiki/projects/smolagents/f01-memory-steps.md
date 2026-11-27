---
project: smolagents
feature: memory-steps
title: 记忆步骤类型与消息转换
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/memory.py
    symbols: [MemoryStep, ActionStep, PlanningStep]
  - path: src/smolagents/memory.py
    symbols: [TaskStep, SystemPromptStep, FinalAnswerStep]
---

# 记忆步骤类型与消息转换

## 一句话职责

把「agent 做过的事」表示成六种步骤类型，每种都同时提供两个视图：`dict()` 给持久化与回放用，`to_messages()` 给下一轮模型调用用——同一个对象，两种截然不同的形状。

## 机制是什么

`MemoryStep` 是基类，只有两个方法：`dict()`（默认 `asdict(self)`）与 `to_messages(summary_mode)`（默认 `NotImplementedError`）。

六个子类按「这一步是什么」划分：

- `TaskStep`：用户给的任务，可带图片。
- `SystemPromptStep`：系统提示。
- `ActionStep`：一步动作，字段最多——步骤号、计时、模型输入消息、工具调用、错误、模型输出消息、模型输出原文、代码动作、观察结果、观察图片、动作输出、用量、是否最终答案。
- `PlanningStep`：一次规划，含输入消息、输出消息、计划文本、计时、用量。
- `FinalAnswerStep`：最终答案，只有一个 `output`。

`to_messages` 的结构差异正好揭示了「哪些步骤会进入提示、以什么角色进入」。

## 怎么实现的

**ActionStep 的消息序列。** 这是最复杂的一个，`to_messages` 按固定顺序拼最多五条消息：

1. 模型输出（`ASSISTANT`）——**仅当不是 summary 模式**。内容被包成 `[{"type": "text", "text": self.model_output.strip()}]`。
2. 工具调用（`TOOL_CALL`）——若 `tool_calls` 非空，内容是把每个调用的 `dict()` 拼成 `"Calling tools:\n" + str([...])`。
3. 观察图片（`USER`）——若有 `observations_images`，每个图一个 `{"type": "image", "image": ...}` 块。
4. 观察文本（`TOOL_RESPONSE`）——若有 `observations`，内容 `f"Observation:\n{...}"`。
5. 错误（`TOOL_RESPONSE`）——若有 `error`，内容包含错误文本加一段**下一步指导**。

第 5 条那段指导值得看原文：错误消息后面紧跟「现在重试：注意不要重复之前的错误！如果你已经重试了几次，就尝试完全不同的方法。」这是一句写给模型的行为约束，直接嵌在消息模板里。并且若 `tool_calls` 非空，错误消息前面还会加 `f"Call id: {self.tool_calls[0].id}\n"`——把错误关联回具体的调用 id。

**为什么 tool-call 与 tool-response 是分开的两条。** 它们对应内部的两个自造角色（见 e01），在发给厂商前会被 `get_clean_message_list` 压回 `assistant` 与 `user`。所以「模型说要调工具」进提示时是助手消息，「工具结果」进提示时是用户消息——这是最通用的兼容形态，几乎所有模型都接受。

**PlanningStep 的角色切换技巧。** 非 summary 模式下它产出两条消息：一条 `ASSISTANT` 放计划文本，一条 `USER` 放 `"Now proceed and carry out this plan."`。

源码在这里留了一条注释，解释第二条消息存在的理由：「这第二条消息制造了一次角色切换，防止模型只是简单地继续计划消息」。也就是说，若只有一条助手消息装着计划，模型倾向于把它当作文本续写；插入一条用户消息后，模型被推入「接受指令」的位置。这是一条关于提示工程的实现级经验，被固化成了代码。

**summary 模式的裁剪。** 三个方法都检查 `summary_mode`：

- `SystemPromptStep.to_messages`：summary 时返回空列表。
- `PlanningStep.to_messages`：summary 时返回空列表。
- `ActionStep.to_messages`：summary 时**只跳过第一条**（模型输出），其余四条照常。

所以 summary 模式的含义不是「全部丢弃」，而是「去掉那些可以由别的途径还原的内容」。系统提示与计划在 summary 模式里由调用方另行注入（或压缩成一句），而动作步骤的观察、错误、工具调用必须保留——它们是不可压缩的事实。

`FinalAnswerStep` 干脆不实现 `to_messages`，继承基类的 `NotImplementedError`。因为它不是「历史的一部分」，而是「结果」，不该出现在下一轮提示里。

**两个 dict 视图的手写覆盖。** `MemoryStep.dict()` 默认 `asdict(self)`，但 `ActionStep` 与 `PlanningStep` 都覆盖了。原因是 `asdict` 会递归展开所有 dataclass 字段，而这些步骤里有**不可序列化的东西**：

- `observations_images` 是 PIL Image。手写版本调 `image.tobytes()`——注意这产出**原始字节**，没有格式信息，也没有编码。所以持久化后的图片只能当裸数据用，不能直接还原成图片。
- `timing`、`error`、`token_usage` 都有各自的 `dict()`，手写版本显式调用它们而不是让 `asdict` 展开。
- `model_input_messages` 与 `model_output_message` 是 `ChatMessage` 列表，用 `make_json_serializable(get_dict_from_nested_dataclasses(msg))` 两层处理——先抹掉 dataclass 嵌套，再递归转成 JSON 安全的值。
- `tool_calls` 走每个 `ToolCall.dict()`，而 `ToolCall.dict()` 内部对 `arguments` 再调一次 `make_json_serializable`（因为参数可能含非 JSON 类型）。
- `action_output` 是最不可控的（工具返回什么就是什么），所以也过 `make_json_serializable`。

**`ToolCall` 与 `ChatMessageToolCall` 是两个类。** 前者在 `memory.py`，只有 `name`/`arguments`/`id`，`dict()` 产出 `{"id", "type": "function", "function": {"name", "arguments"}}`；后者在 `models.py`，是消息协议的一部分。所以记忆层与模型层各自持有一个「工具调用」表示，中间靠 `dict()` 的形状一致来衔接。

**`TaskStep` 的最简实现。** 它只产出一条 `USER` 消息：`f"New task:\n{self.task}"`，若有图片就追加 image 块。没有 summary 分支——任务描述永远保留。

## 关键取舍

**源码事实：** `ActionStep.dict()` 把图片存成 `image.tobytes()`，不存格式与尺寸。所以「记忆持久化后还原图片」需要额外约定格式，代码里没有提供。

**源码事实：** summary 模式对 `ActionStep` 只去掉模型输出，保留观察与错误。所以压缩历史时，「模型说过什么」可以被丢弃，「世界返回了什么」不能。

**源码事实：** 错误消息里嵌了一段行为指导（「现在重试，不要重复之前的错误」），并且会带上第一个工具调用的 id。这让错误消息不只是报告，而是**指导下一步动作**的提示。

**源码事实：** `PlanningStep` 强制插入一条 `"Now proceed and carry out this plan."` 的用户消息，理由是防止模型把计划当文本续写。这是一条从实践中得来的提示工程约束。

**源码事实：** `FinalAnswerStep` 不实现 `to_messages`，调用它会抛 `NotImplementedError`。所以「结果」与「历史」在类型上是分开的，不会被误当历史注入。

**架构推论：** 「同一对象两种视图」的设计，让「存什么」与「喂什么」可以独立演进。若只维护一份形状，要么持久化里塞满提示细节，要么提示里混进持久化字段。分开之后，`dict()` 可以放心加调试字段，`to_messages()` 可以放心调提示措辞。

**架构推论：** 六个步骤类型实际上是「一个 agent 运行的状态机」的语法：任务开始、可能规划、循环动作、结束。把它们建模成独立类型而不是一个通用 step 加 type 字段，好处是每个类型可以有完全不同的字段集与消息转换；代价是消费侧要写 isinstance 分支（`replay` 就是这么做的）。

**架构推论：** 手写覆盖 `dict()` 而不是依赖 `asdict`，是「嵌套里有不可序列化对象」的必然结果。这类代码往往会随字段增加而不断打补丁——每加一个字段就要记得在 dict() 里补一行。用 `make_json_serializable` 兜底能减轻，但不能消除「忘记显式处理导致结构不一致」的风险。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/memory.py` | `MemoryStep` | 步骤基类：`dict()` 与 `to_messages()` 两视图 |
| `src/smolagents/memory.py` | `ActionStep` | 一步动作的完整记录与消息序列 |
| `src/smolagents/memory.py` | `ActionStep.dict` | 手写序列化，处理图片与嵌套 dataclass |
| `src/smolagents/memory.py` | `ActionStep.to_messages` | 输出→工具调用→图片→观察→错误五段消息 |
| `src/smolagents/memory.py` | `PlanningStep` | 规划步骤与强制角色切换的提示构造 |
| `src/smolagents/memory.py` | `TaskStep` | 用户任务，带可选图片 |
| `src/smolagents/memory.py` | `SystemPromptStep` | 系统提示，summary 模式下省略 |
| `src/smolagents/memory.py` | `FinalAnswerStep` | 最终答案，不参与消息构造 |
| `src/smolagents/memory.py` | `ToolCall` | 记忆层的工具调用表示与序列化 |

## 参考

- [smolagents 源码：memory.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/memory.py)
- [smolagents 文档：Memory](https://huggingface.co/docs/smolagents/main/en/reference/memory)
- [smolagents 文档：多步 agent 的工作原理](https://huggingface.co/docs/smolagents/conceptual_guides/intro_agents)

## 与主核 / AI 的关系

主核里最接近的是「执行计划的节点与执行轨迹」：同一份数据结构既要能落进日志（结构化、可查），又要能还原成可重放的执行序列。`dict()` 与 `to_messages()` 正是这两个用途，差别在于主核的两个视图都由同一份 schema 派生，而这里的手写 `dict()` 是逐字段拼的。

与 CrewAI 的 `TaskOutput`/`Memory` 相比，CrewAI 的记忆偏「键值存储 + 语义检索」，存的是文本片段与元数据；smolagents 的记忆是**结构化的步骤序列**，每步保留输入消息、输出、观察、错误、用量。前者适合「回忆相似经验」，后者适合「原样重放这次执行的上下文」。这是两种不同的记忆模型，服务于不同的续跑方式。

对 AI 工程最实用的一条是 `PlanningStep` 那个「强制角色切换」。它把一条提示工程经验固化成代码：模型在助手消息里看到自己的计划后，倾向于继续写计划而不是执行计划。插入一条用户消息（哪怕内容只是「现在去执行」）能显著改变行为，因为模型对「用户指令」与「自己说过的话」的处理方式不同。这类细节写在代码里比写在文档里可靠。

另一条是 summary 模式的裁剪策略。做上下文压缩时，最容易一刀切地丢掉整段历史。这里的做法是「保留观察与错误，丢弃模型输出」——因为观察是世界的事实，模型输出可以从观察与错误重新推导。这个区分原则可以直接搬到任何「压缩对话历史」的场景：先问「哪些信息是不可再生的事实」，其余都可以压。
