---
project: smolagents
feature: monitoring
title: 监控、用量与日志
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/monitoring.py
    symbols: [TokenUsage, Timing, Monitor]
  - path: src/smolagents/monitoring.py
    symbols: [AgentLogger, LogLevel]
---

# 监控、用量与日志

## 一句话职责

用三个小数据类（用量、计时、监控器）记录「花了多少 token、用了多少秒」，再用一个建立在 rich 之上的日志器把运行过程渲染成有颜色有面板的控制台输出——并为此专门做了对抗任意内容的转义。

## 机制是什么

`TokenUsage` 是三个字段的 dataclass：`input_tokens`、`output_tokens`、`total_tokens`。后者是 `field(init=False)`，在 `__post_init__` 里算成前两者之和。所以构造时只给两个数，总数自动派生。

`Timing` 是 `start_time` 加可选的 `end_time`，`duration` 是 property：`end_time` 为 `None` 时返回 `None`。所以「未结束」与「耗时 0」被区分开。

`Monitor` 持三样可变状态：`step_durations`（列表）、`total_input_token_count`、`total_output_token_count`，外加 `tracked_model` 与 `logger`。方法有 `reset()`、`get_total_token_counts()`、`update_metrics(step_log)`。

`LogLevel` 是 `IntEnum`，四个值：`OFF=-1`、`ERROR=0`、`INFO=1`、`DEBUG=2`。

`AgentLogger` 包一层 rich 的 `Console`，核心是 `log(*args, level=INFO, **kwargs)`：把 `level` 归一（字符串按名字查枚举），然后 `if level <= self.level: self.console.print(*args, **kwargs)`。

## 怎么实现的

**日志级别的比较方向。** 过滤条件是 `level <= self.level`，而枚举值是 `OFF=-1 < ERROR=0 < INFO=1 < DEBUG=2`。所以「消息级别数值 ≤ 当前配置级别数值」才输出。

这意味着：把 logger 设成 `INFO`（1）时，`ERROR`（0）与 `INFO`（1）的消息会输出，`DEBUG`（2）不会。设成 `OFF`（-1）时，没有任何 `level` 满足 `level <= -1`——除了那些显式传 `-1` 的调用。

这个方向与「数值越大越严重」的常见约定相反：这里是数值越大越啰嗦。所以 `LogLevel.ERROR` 的数值是 0 而不是 3。理解这一点才能解释 `memory.py` 里为什么用 `LogLevel.ERROR` 来确保回放内容可见（数值最小，在任何非 OFF 配置下都通过）。

**`Monitor.update_metrics` 的三件事。** 它是注册在回调表里的消费者（见 f03），签名是单参数（`step_log`），所以只收到步骤对象。

第一，`step_duration = step_log.timing.duration` 并追加进 `step_durations`。注意这里不判空：若某步骤的 `timing.duration` 是 `None`（未结束），列表里就会存进 `None`。后续若有人对这个列表求和会崩——但代码里没有这么做，只用 `len()` 取步号。

第二，若 `step_log.token_usage` 非空，累加到两个总计。

第三，拼一行控制台文本：`[Step N: Duration X.XX seconds | Input tokens: A | Output tokens: B]`，其中 N 是 `len(self.step_durations)`（刚追加后的长度）。若没有用量信息，就省掉 token 那一半，只剩 `[Step N: Duration X.XX seconds]`。最后用 `style="dim"` 的 `Text` 打出。

步骤号取自自己的计数而不是 `step_log.step_number`，是一个值得留意的选择：监控维护独立计数，不依赖步骤对象的字段。这样即使步骤对象上的编号有空洞（例如规划步骤不编号），监控仍然连续计数。

**`get_total_token_counts` 每步重建。** 它每次返回一个新的 `TokenUsage`，由两个累计值构造。所以 `total_tokens` 也每次重算。这种做法让「累计值」只存两个原始数，派生的总数在下游算——避免了三处状态不同步。

**`AgentLogger` 的渲染方法各自封装一种 rich 组件。** `log_markdown` 用 `Syntax(lexer="markdown")`，可选包一个 `Rule` 当标题；`log_code` 包 `Panel(Syntax(lexer="python"))` 用横线边框；`log_rule` 用 `Rule(characters="━")`；`log_task` 用 `Panel` 带标题与副标题；`log_messages` 把消息列表 `json.dumps(indent=4)` 后当 markdown 渲染。

这些方法的共同点是「输入是纯文本，输出是带样式的 rich 对象」——样式与内容分离，调用方不需要知道 rich 的 API。

**对抗任意内容的转义。** `log_task` 里有一段注释值得完整对待：`content` 可能含任意工具日志或载荷，若把它嵌进 rich markup（例如用 f-string 拼上 `[bold]`），任何游离的 `[/...]` 序列或二进制字符都会让 rich 的 markup 解析器崩掉。所以代码**把内容渲染成 `Text` 对象**，样式通过 `Text(..., style=...)` 施加，而不是写在字符串里。

配套的 `sanitize_for_rich`（在 `utils.py`）做四件事：`None` 转空串；bytes 类按 UTF-8 解码并用 `errors="replace"` 兜底；逐字符遍历，保留 `\n`/`\t`/`\r`，把其他 ASCII 控制字符（码点小于 32 或等于 127）替换成可见的 `\xNN` 形式；其余原样。

这个函数的存在说明「把不可信内容打到终端」是一个真实的安全与稳定性问题：控制字符可以让终端执行转义序列（改标题、清屏、甚至在某些终端上触发剪贴板操作），而 rich 的 markup 解析器遇到不配对的方括号会抛异常。

`log_error` 也走这条路：`Text(sanitize_for_rich(error_message), style="bold red")`。

**`visualize_agent_tree` 的结构化展示。** 这个方法用 rich 的 `Tree` 与 `Table` 把 agent 的构成画出来：根节点是「类名 | 模型 id」，每个 agent 下挂一个工具表（三列：名称、描述、参数），若有 managed agents 就再挂一个分支并递归。

参数列的内容是逐项拼的：`f"{arg_name} (`{type}`{', optional' if optional else ''}): {description}"`。所以它读的是工具的 `inputs` 字典。

有两个地方用 `getattr` 兜底：`getattr(tool, "inputs", {})` 与 `getattr(tool, "description", str(tool))`。这意味着它对「不是标准 Tool 的对象」也能画出树——例如 mock 或适配器包装。

`CodeAgent` 特有 `additional_authorized_imports` 的显示，用的是 `agent.__class__.__name__ == "CodeAgent"` 字符串比较而不是 `isinstance`。这样做是为了避免 `monitoring.py` 导入 `agents.py`（那会造成循环依赖：`agents.py` 已经导入了 `monitoring.py`）。

**`sanitize_for_rich` 的一处遗漏。** 它处理了控制字符，但**没有处理方括号 markup**。注释说「转义可能被解释为 markup 的方括号序列，同时保留合法的 rich 标签」——实际实现里没有这一步。所以防护依赖 `log_task` 用 `Text` 而非字符串。两个层次里只有一层真正生效，这是代码与注释的一处不一致。

**`Console(highlight=False)` 的默认。** 构造时若未传 console，就建一个 `Console(highlight=False)`。关掉高亮是为了避免 rich 自动给数字、字符串等加颜色——对日志输出，自动高亮会让内容与人工着色混在一起，难以区分。

## 关键取舍

**源码事实：** `LogLevel` 的数值方向是「越大越啰嗦」（`ERROR=0`、`DEBUG=2`），过滤条件是 `level <= self.level`。所以 `ERROR` 是「最高可见性」而非「最高严重度」。

**源码事实：** `Monitor.update_metrics` 不检查 `timing.duration` 是否为 `None`，会把 `None` 存进 `step_durations`。

**源码事实：** 步骤号取自 `len(self.step_durations)` 而非 `step_log.step_number`，所以监控有独立计数。

**源码事实：** `log_task` 用 `Text` 对象而非 markup 字符串来承载任意内容，这是防 rich 解析崩溃的关键。但 `sanitize_for_rich` 的注释声称会转义方括号，实现里并没有做。

**源码事实：** `visualize_agent_tree` 用类名字符串比较判断 `CodeAgent`，以避免 `monitoring.py` 到 `agents.py` 的循环导入。

**架构推论：** 这三个数据类把「可观测性」拆成了三个正交维度：用量（成本）、计时（延迟）、结构（agent 组成）。它们之间没有依赖——`Monitor` 聚合前两者，`visualize_agent_tree` 只关心第三者。这种拆分让「只想统计成本」的场景不必构造 agent 树。

**架构推论：** `Timing.duration` 返回 `None` 而不是 `0`，是「未开始/未结束」与「零耗时」的区分。这个细节在统计时很重要：把未结束的步骤当成 0 秒会把平均值拉低。代码里虽然没做统计，但类型上留了这个区分，是正确的默认。

**架构推论：** 日志器建立在 rich 之上而不自己实现配色，收益是输出质量（表格、树、语法高亮、自动换行）免费获得；代价是把「内容安全」的责任交给了调用方——rich 会把字符串当 markup 解析，所以任何不可信内容都必须先转成 `Text` 或经过转义。这正是 `log_task` 那段注释在说的事，也是这类「样式库 + 不可信内容」组合的通用陷阱。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/monitoring.py` | `TokenUsage` | 输入/输出/总 token 用量，总数派生 |
| `src/smolagents/monitoring.py` | `Timing` | 起止时间，`duration` 在未结束时为 `None` |
| `src/smolagents/monitoring.py` | `Monitor` | 累计每步耗时与 token，并打控制台摘要 |
| `src/smolagents/monitoring.py` | `Monitor.update_metrics` | 单参数回调：追加耗时、累加用量、输出一行 |
| `src/smolagents/monitoring.py` | `Monitor.get_total_token_counts` | 把两个累计值组装成 `TokenUsage` |
| `src/smolagents/monitoring.py` | `LogLevel` | OFF/ERROR/INFO/DEBUG 四级，数值越大越啰嗦 |
| `src/smolagents/monitoring.py` | `AgentLogger` | rich 控制台封装与级别过滤 |
| `src/smolagents/monitoring.py` | `AgentLogger.log_task` | 用 `Text` 承载任意内容以避免 markup 崩溃 |
| `src/smolagents/monitoring.py` | `AgentLogger.visualize_agent_tree` | 用 Tree 与 Table 展示 agent 构成 |
| `src/smolagents/utils.py` | `sanitize_for_rich` | 控制字符可见化与 bytes 安全解码 |

## 参考

- [smolagents 源码：monitoring.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/monitoring.py)
- [smolagents 源码：utils.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/utils.py)
- [Rich 文档：Text 与 markup](https://rich.readthedocs.io/en/stable/text.html)

## 与主核 / AI 的关系

主核里最接近的是「执行器的运行时统计视图」：查完之后能看到每个算子的耗时、行数、扫描方式，以及整个查询的累计。`Monitor` 的 `step_durations` 与 token 累计正是这个层次，只不过统计对象从算子变成了 agent 步骤。差别在于主核的统计由引擎内部维护且有固定的展示接口，这里靠「注册成步骤回调」来采集——采集机制是复用的，不是专门实现的。

与 CrewAI 的可观测性相比，两边定位不同。CrewAI 有事件总线，追踪、检查点、外部集成都从事件流里拿数据，`Monitor` 这种聚合器是事件的一个消费者。smolagents 没有事件总线，最细的观测粒度是「步骤」，所以统计只能到步骤级——无法观测到「一次工具调用内部的重试」这种更细的事。这是「有总线」与「只有回调」在可观测性能力上的真实差距。

对 AI 工程最实用的一条是 `log_task` 那个「用 `Text` 而不是 markup 字符串」的做法。任何把不可信内容（工具输出、模型输出、文件内容）打到富文本终端的场景，都有两个独立风险：控制字符可以操纵终端，标记语法可以让渲染器崩溃或注入样式。这两个风险要分开防：前者靠字符级转义，后者靠「永远不用字符串拼 markup」。代码里恰好只做对了第二个（用 `Text`），第一个的转义函数虽然存在但漏了方括号——这正是两层防护都需要显式检查的例子。

另一条是 `LogLevel` 数值方向这个坑。把「数值越大越啰嗦」与「过滤条件是小于等于」组合起来，结果是 `ERROR` 成为最高可见性级别。这本身没问题，但它让「用 ERROR 级别输出调试信息」成了常见 hack（`memory.replay` 就这么做了）。设计日志级别时，更稳的做法是显式区分「严重度」与「优先级」两个维度，或者像很多系统那样提供「永远输出」的语义值。否则一旦有人开始借用级别来达到可见性目的，日志系统就失去了可配置性。
