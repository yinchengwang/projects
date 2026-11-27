---
project: smolagents
feature: agent-memory
title: 记忆容器与重放
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/memory.py
    symbols: [AgentMemory, get_succinct_steps, replay, return_full_code]
---

# 记忆容器与重放

## 一句话职责

持有一个系统提示步骤加一串动作步骤，并提供三种读取方式：精简视图（去掉模型输入）、完整视图（含模型输入）、以及把所有代码动作拼成一个脚本——外加一个只负责往控制台回放的调试方法。

## 机制是什么

`AgentMemory` 只有两个字段：`system_prompt`（一个 `SystemPromptStep`）与 `steps`（`TaskStep | ActionStep | PlanningStep` 的列表）。

注意类型标注里**没有** `SystemPromptStep` 与 `FinalAnswerStep`——系统提示单独放在 `system_prompt` 字段里，最终答案不进 `steps`。所以记忆的语义是「系统提示 + 执行过程」，结果被排除在外。

四个公开方法各管一件事：

- `reset()`：把 `steps` 置空，**保留系统提示**。
- `get_succinct_steps()`：逐步 `dict()` 后去掉 `model_input_messages` 键。
- `get_full_steps()`：逐步 `dict()`，全字段；空记忆时提前返回空列表。
- `replay(logger, detailed)`：把步骤渲染到控制台，`detailed` 为真时额外打印每步的模型输入消息。
- `return_full_code()`：把所有 `ActionStep` 的 `code_action` 用两个换行拼起来。

## 怎么实现的

**精简视图的过滤是键级而非值级。** `get_succinct_steps` 的实现是一句推导：

```python
{key: value for key, value in step.dict().items() if key != "model_input_messages"}
```

所以它先完整构造 dict（含那些昂贵的序列化，比如图片转 bytes、嵌套消息转 JSON 安全值），再把一个键扔掉。这意味着**精简视图在构造期并不省成本**，只在输出体积上省。这个实现细节值得留意：如果目的是减少序列化开销，应该先过滤再序列化。

对 `PlanningStep` 与 `TaskStep` 而言，`dict()` 里本来就没有 `model_input_messages`（`PlanningStep` 有，`TaskStep` 没有），所以这个过滤只对 `ActionStep` 与 `PlanningStep` 生效。

**完整视图的空检查。** `get_full_steps` 里有个 `if len(self.steps) == 0: return []`。这看起来多余（列表推导对空列表也返回空列表），实际作用是**统一返回类型语义**：避免在空记忆时走完整的推导路径。这是一个微优化，不影响行为。

**replay 的类型分派。** 方法先打一行 `"Replaying the agent's steps:"`，再单独渲染系统提示（`log_markdown(title="System prompt", ...)`），然后遍历步骤，按 `isinstance` 分三支：

- `TaskStep` → `log_task(step.task, "", ...)`
- `ActionStep` → 打一条分隔线标步骤号；`detailed` 且 `model_input_messages` 非空时打消息；`model_output` 非空时打 markdown
- `PlanningStep` → 打「Planning step」分隔线；同样按 `detailed` 决定是否打输入消息；再打计划内容

**所有调用都传 `level=LogLevel.ERROR`。** 这是这一页最反直觉的一点：回放的内容被标记为 ERROR 级别。结合 `AgentLogger.log` 的实现（`if level <= self.level: print`）与 `LogLevel` 的取值（`OFF=-1`、`ERROR=0`、`INFO=1`、`DEBUG=2`），这意味着**回放只在日志级别为 ERROR 或更高（即 INFO、DEBUG）时才输出**，而在 OFF 时被完全抑制。

换句话说，`ERROR` 在这里被当作「最高可见性」的优先级使用——数字最小，所以在任何非 OFF 的配置下都会通过 `level <= self.level` 的判定。这是一个把「优先级」与「语义级别」混用的例子：回放内容明明是调试信息，却用 ERROR 保证它不被过滤。

**detailed 的代价警告。** docstring 里写得很直接：`detailed=True` 会「指数级增加日志长度，只在调试时使用」。这个措辞不是夸张——每步的 `model_input_messages` 包含该步之前**全部历史**的消息列表，而每一步都有这样一个列表。所以打印全部步骤的输入消息，输出体积大致是「步数 × 平均历史长度」，对长运行是平方级。

**return_full_code 的形状。** 一句推导加两个换行分隔：

```python
"\n\n".join([step.code_action for step in self.steps if isinstance(step, ActionStep) and step.code_action is not None])
```

它只挑 `ActionStep` 且有 `code_action` 的。注意 `code_action` 是**代码式 agent 专有**的字段——工具调用式 agent 的 `ActionStep` 里这个字段是 `None`，所以对它们返回空字符串。这让这个方法对两种 agent 都安全，只是对工具调用式无意义。

拼接而不是「合并去重」，是因为多步代码之间常有变量依赖（第一步定义，第二步使用），必须按原顺序保留。注释写「拼接成单个脚本」，语义就是「按执行顺序缝起来，可以直接跑」。

## 关键取舍

**源码事实：** `steps` 的类型标注不含 `SystemPromptStep` 与 `FinalAnswerStep`，所以「系统提示」与「最终答案」都不在 `steps` 里。前者单独成字段，后者根本不进记忆。

**源码事实：** `get_succinct_steps` 先 `dict()` 再过滤键，所以精简视图不节省序列化开销，只节省输出体积。

**源码事实：** `replay` 用 `LogLevel.ERROR` 渲染所有内容，借此绕过日志级别过滤。回放内容因此无法单独用一个更细的级别开关控制——要么全开（级别 ≥ ERROR）要么全关（OFF）。

**源码事实：** `replay` 的 `detailed=True` 会打印每步的完整输入历史，代价是平方级的输出体积。docstring 明确警告。

**源码事实：** `return_full_code` 对工具调用式 agent 返回空字符串（因为 `code_action` 为 `None`），而不是报错或返回占位。方法对两种 agent 都「不炸」，但只有对代码式 agent 有意义。

**架构推论：** 「精简 vs 完整」两个视图对应两种持久化需求：日志与遥测要精简（不要每步的历史消息），调试与复现要完整（要能重建当时看到的确切提示）。把选择权做成两个方法而不是一个带参数的方法，让调用点更明确自己在要什么。

**架构推论：** `reset()` 保留系统提示是个有含义的选择。系统提示在 agent 生命周期内不变，清空它意味着下次运行要重新构造提示；而 `steps` 是这次运行的过程，每次 `run(reset=True)` 都该清空。这个划分让「同一 agent 跑多个任务」成为自然用法。

**架构推论：** `replay` 依赖 `AgentLogger` 而不是自己 `print`，让输出风格（颜色、面板、语法高亮）与正常日志一致。代价是它与日志级别的耦合变得隐晦——用 ERROR 级别换可见性，这个 hack 在代码里没有注释解释，只能从 `LogLevel` 的数值关系推出来。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/memory.py` | `AgentMemory` | 记忆容器：系统提示加步骤列表 |
| `src/smolagents/memory.py` | `AgentMemory.reset` | 清空步骤但保留系统提示 |
| `src/smolagents/memory.py` | `AgentMemory.get_succinct_steps` | 去掉 `model_input_messages` 的精简视图 |
| `src/smolagents/memory.py` | `AgentMemory.get_full_steps` | 全字段视图，用于复现 |
| `src/smolagents/memory.py` | `AgentMemory.replay` | 按步骤类型分派的控制台回放 |
| `src/smolagents/memory.py` | `AgentMemory.return_full_code` | 拼接所有代码动作成单个脚本 |
| `src/smolagents/monitoring.py` | `AgentLogger.log` | 按 `level <= self.level` 决定是否输出的过滤入口 |
| `src/smolagents/monitoring.py` | `LogLevel` | OFF/ERROR/INFO/DEBUG 四级的整数值 |

## 参考

- [smolagents 源码：memory.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/memory.py)
- [smolagents 源码：monitoring.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/monitoring.py)
- [smolagents 文档：Memory](https://huggingface.co/docs/smolagents/main/en/reference/memory)

## 与主核 / AI 的关系

主核里最接近的是「执行轨迹与慢查询日志」：同一个查询执行完，会留下两种记录——一种是喂给优化器与监控的精简统计（耗时、行数、扫描方式），一种是完整的执行细节（每个算子的计划与代价），后者只在需要复现问题时才打开。`get_succinct_steps` 与 `get_full_steps` 正是这两个层次，连「完整版体积巨大只能调试时用」这条注意事项都一致。

与 CrewAI 的记忆相比，CrewAI 的记忆是「可检索的经验库」（向量 + 元数据 + 作用域），查询方式是语义相似；这里的记忆是「按顺序的步骤序列」，读取方式是「去头或全量」。前者服务「跨任务学习」，后者服务「同一任务内的上下文延续与事后复盘」。两者都叫 memory，但完全不是同一件事。

对 AI 工程最实用的一条是那个「精简视图先序列化再过滤」的隐含成本。它提醒我们：当过滤发生在「构造之后」时，省下的只有传输体积，不省 CPU。在大步骤（含图片、含长历史）上，这个构造代价并不小。若要真正省，得让 `dict()` 支持字段排除，或者让步骤自己提供 `succinct_dict()`。

另一条是 replay 用 ERROR 级别换可见性的做法——它是个反面教材式的提醒：把「优先级」与「语义」混用会让日志系统失去可配置性。更好的做法是给回放一个独立的输出通道（直接 `console.print`），或者让 `LogLevel` 支持「永远显示」的语义值。看到这类 hack 时，值得先问一句「能不能不改语义就达到目的」。
