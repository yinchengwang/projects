---
project: haystack
feature: agent
title: 智能体组件与状态
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/agents/agent.py
    symbols: [Agent]
  - path: haystack/components/agents/agent.py
    symbols: [_get_model_exit_reason, _pending_tool_call_messages_from_state, _consume_continue_run]
---

# 智能体组件与状态

## 一句话职责

把「模型 → 调工具 → 把结果喂回模型 → 再调」这个循环做成一个 `@component`，而**循环的每一次迭代都以 State 为唯一真相源**：模型的回复写进 `messages`、工具的结果写进 `messages`、退出原因写进 `exit_reason`，于是「运行到哪里了」这件事在管道里是**数据**而不是组件的内部字段。

## 机制是什么

haystack 3.0 把 `ToolInvoker` 移除了，让 `Agent` 自己拥有工具调用。这个决定改变了组件契约的形状：普通组件是**无状态**的（`run(**inputs) -> dict`），而 `Agent` 必须记住「已经跑了几步」「上一轮模型说了什么」「工具调用还没执行」。

它的解法不是给自己加字段，而是把状态做成一个**显式的 `State` 对象**，且这个对象同时是：

1. **组件的输入**（`run` 的 `**kwargs` 会被按 `state_schema` 过滤后灌进 State）。
2. **组件的输出**（`_public_outputs` 从 State 里挑出非内部键返回）。
3. **每一步里所有协作方的共享黑板**（钩子、工具、执行器都读写它）。

所以 State 的三类键被**严格分级**：

| 类别 | 常量 | 是否可作输入 | 是否可作输出 | 内容 |
|---|---|---|---|---|
| 用户自定义 | `resolved_state_schema` | 是 | 是 | 用户声明的字段 |
| 运行元数据 | `_RUN_METADATA_STATE_KEYS` | **否** | 是 | `step_count`、`token_usage`、`tool_call_counts`、`exit_reason` |
| 内部状态 | `_INTERNAL_STATE_KEYS` | **否** | **否** | `continue_run`、`stop_run`、`tools`、`hook_context`、`context_tokens`、`max_agent_steps` |

源码对这两类的注释把差别说得极清楚：运行元数据「只作为 Agent 的输出暴露，不作为输入」；内部状态「与运行元数据键一样是保留的、用户不可重定义，但与它们不同，**不作为 Agent 的输入或输出暴露**（纯内部状态）」。

## 怎么实现的

**主循环只有一句话。** `run()` 的骨架：

```python
while exe_context.counter < self.max_agent_steps:
    if not self._run_step(exe_context=exe_context, agent_span=span):
        break
else:
    logger.warning("Agent reached maximum agent steps of {max_agent_steps}, stopping.", max_agent_steps=self.max_agent_steps)
    exe_context.state.set("exit_reason", _EXIT_REASON_MAX_STEPS)
```

注意 `while` 的 `else` 分支——**它是这段代码里最容易读错的地方**。注释专门解释：「只在循环没有 `break` 结束时到达。`break` 意味着某一步自己已经设了 `exit_reason`，所以这一支只在 `max_agent_steps` 是停止原因时运行」。

用 `while/else` 而不是「跑完循环后判断一次」的收益是：`exit_reason` 的赋值被放在唯一的、无歧义的位置。若写成「循环后 if counter >= max_steps」，那么当最后一步恰好用完 `max_agent_steps` 且它自己设了退出原因时，两个条件同时成立，就会覆盖掉真实原因。

**`_run_step` 是一条九段的直线流程，而每段都有明确的顺序理由。** 顺序是：

1. **重新 flatten 工具**（`flatten_tools_or_toolsets`）并查重名。
2. 把当前工具写进 `state["tools"]`（供 `before_llm` 钩子读取）。
3. 跑 `BEFORE_LLM` 钩子。
4. **检查 `stop_run`**：有值就以它为 `exit_reason` 立刻返回 `False`，连这次 LLM 调用都不发。
5. 调 chat generator，把 `replies` 写进 `state["messages"]`。
6. 记录 `token_usage` 与 `context_tokens`。
7. 判 `_get_model_exit_reason`；若「没有工具」或「有退出原因」→ 记步、设 `exit_reason`、走退出钩子。
8. 否则跑 `BEFORE_TOOL` 钩子，**重新从 State 读**待执行的工具调用，执行，写回 `messages`，记 `tool_call_counts`，跑 `AFTER_TOOL` 钩子。
9. 记步，判 `_check_exit_conditions`，有则设 `exit_reason` 并走退出钩子，否则返回 `True` 继续。

第 1 步的注释解释了为什么每步都重 flatten：「这样动态工具集（例如 `SearchableToolset`）能在后续步骤里浮现出前几步发现的工具」。而 `_ExecutionContext.tools` 因此存的是**未 flatten 的原始形式**——docstring 明写理由：「存未扁平化的形式让每一步重新扁平化并拾取动态工具集随时间发现的工具；扁平化会冻结一份快照」。

这是一个「把重新计算当作正确性来源」的选择：代价是每步一次 flatten 与查重，收益是「工具集可以在一轮运行内变化」。

第 4 步那个 `stop_run` 检查的位置也很关键——它在 LLM 调用**之前**。docstring 写着「钩子请求停止：在步边界结束运行，**在再花一次 LLM 调用之前**」。所以「让钩子提前终止」这件事的价值就是省掉一次真实的模型调用费用。

**`_get_model_exit_reason` 的四条判据顺序决定了它的语义。** 完整实现：

```python
if not messages or any(message.tool_call for message in messages):
    return None
last = messages[-1]
if not last.is_from(ChatRole.ASSISTANT):
    return None
if last.meta.get("finish_reason") == _EXIT_REASON_LENGTH:
    return _EXIT_REASON_LENGTH
if last.meta.get("finish_reason") == _EXIT_REASON_CONTENT_FILTER:
    return _EXIT_REASON_CONTENT_FILTER
if last.text:
    return _EXIT_REASON_TEXT
return None
```

四步各有一个不显然的点：

1. **第一条的 `any(...)` 不是只看最后一条** ——只要有任一条消息带 tool_call 就不退出。所以一个「模型先调工具、又说了一句话」的多消息回复仍会被当成要继续。
2. **`finish_reason` 的两种不完整原因优先于 `text`** ——docstring 的理由是「让调用方能区分部分响应与完整答案」。一个被截断的回复也有 `text`，若不优先判 `length`，它就会被当成正常结束。
3. **空回复且无终止原因时不退出** ——这是最微妙的一条。docstring 写：「没有识别出终止原因的空响应不触发退出，**保留 Agent 对畸形工具调用的恢复行为**——那些被 Chat Generator 丢弃了的调用」。

所以这条「什么都不做」的分支是有意的：某些 chat generator 看到格式错误的 tool_call 会丢弃它并返回空消息。若此时 Agent 退出，用户就看到一个空答案；不退出则下一轮模型还有机会。**这是把「不确定」映射到「继续试」而不是「结束」**。

4. `last.text` 用的是真值判定，所以空字符串不算。

**`_pending_tool_call_messages_from_state` 只看最后一条消息，且这个「只看最后一条」正是它存在的意义。** 实现只有四行：

```python
messages = state.data.get("messages") or []
if not messages:
    return []
last_message = messages[-1]
return [last_message] if last_message.tool_calls else []
```

docstring 写得很明确：「`before_tool` 钩子可能改写 `state.data["messages"]`。在它们跑完之后，Agent **有意只检查 State 里当前的最后一条消息**。若那条有工具调用就执行；若没有，则这一步不跑任何工具、不触发基于工具的退出条件，Agent 回到下一轮 LLM 调用（除非已达 `max_agent_steps`）」。

所以这个函数是一个**重读点**。它不是为了取数据（第 5 步已经把 `replies` 存过了），而是为了在钩子可能改过 State 之后**重新确认该执行什么**。没有这一步，`before_tool` 钩子对工具调用的改写（例如人工确认后拒绝某个调用）就不会生效。

而「只看最后一条」这个限制意味着：钩子若要阻止工具执行，让它**删掉最后一条消息的 tool_calls** 即可；若它追加一条新消息，那条新消息才是被执行的对象。两种改法都自然。

**退出与「继续」由一对标志完成，且 `continue_run` 是消费型的。** `_continue_after_exit_hooks`：

```python
if not self.hooks.get(ON_EXIT):
    return False
exe_context.state.set("continue_run", False)
_run_hooks(hooks=self.hooks, hook_point=ON_EXIT, state=exe_context.state)
return _consume_continue_run(exe_context.state)
```

三步：没有 `on_exit` 钩子就直接停（省掉整个流程）；先把标志置 `False`（清掉上一轮的残留）；跑钩子；消费标志。

`_consume_continue_run` 本身只做「读并复位」：

```python
should_continue = state.data["continue_run"]
state.set("continue_run", False)
return should_continue
```

docstring 说「返回 `continue_run` 控制标志并复位它，使它不会带到下一次退出尝试」。所以「继续」必须由钩子**每次都重新设置**——一次设置不能换来两次继续。

这个「消费型标志」的设计让「无限继续」不可能发生：即使钩子写错了逻辑每次都设 `True`，循环仍被 `max_agent_steps` 挡住。所以两层保护是独立的——标志管「这一次要不要继续」，步数管「最多继续几次」。

**`_check_exit_conditions` 里有一条「错误取消退出」的规则。** 实现先收集出错的工具名：

```python
errored_tools = {tool_msg.tool_call_result.origin.tool_name for tool_msg in tool_messages if tool_msg.tool_call_result is not None and tool_msg.tool_call_result.error}
```

然后遍历所有消息的所有 tool_call，命中 `exit_conditions` 的记为 `first_match`，但**若那个工具在 `errored_tools` 里就立刻 `return None`**。注释写明：「一个出错的退出条件工具会取消退出，即使另一个退出条件工具成功了」。

这条规则的理由值得想：若你在 `exit_conditions=["save_result"]` 上配置了「保存完就停」，而 `save_result` 报错了，那停下来就是把一次失败的保存当成成功。取消退出让模型看到错误并有机会重试。

而 `first_match = first_match or tool_call.tool_name` 保序：多个退出条件工具在同一步被调用时，返回**遇到的第一个**。docstring 说「消息里的每个工具调用都被检查，所以并行工具调用的顺序无关」。

还有一个细节在调用处：

```python
exit_condition_tool = (
    None if self.exit_conditions == ["text"] else self._check_exit_conditions(...)
)
```

所以 `exit_conditions == ["text"]` 时**跳过整个检查**——因为「文本退出」已经由 `_get_model_exit_reason` 处理过了，这里不需要再看工具。

**`_validate_hooks` 在构造期做三道校验，其中一道是「钩子自己声明它能挂在哪儿」。** 三道：

1. 钩子点必须在 `VALID_HOOK_POINTS` 里。
2. 每个钩子必须有可调用的 `run`；若是「可调用但不是 Hook 对象」，错误消息直接给出修法：「如果它是一个函数，用 `@hook` 装饰器包一下」。
3. 若钩子声明了 `allowed_hook_points`，注册位置必须在其中。

第三道带注释举例：「一个钩子可以声明 `allowed_hook_points` 来限制它能跑在哪（例如 `ConfirmationHook` 只在 `before_tool` 有意义）。没有它的钩子可以注册在任何钩子点下」。

三道全在构造期，所以「钩子注册错了」会在建 Agent 时报错，而不是在第一次运行时才发现。

**`_public_outputs` 是一行过滤器，但它定义了「外部看到什么」。**

```python
return {key: value for key, value in state.data.items() if key not in _INTERNAL_STATE_KEYS}
```

注意它只排除了 `_INTERNAL_STATE_KEYS`，**没有**排除 `_RUN_METADATA_STATE_KEYS` ——所以 `step_count`、`token_usage`、`tool_call_counts`、`exit_reason` 会作为输出返回。这与那两个常量的注释完全一致（运行元数据「只作为输出暴露」）。

`_run_step` 的最后还把最后一条消息提升成一个显式输出：

```python
if msgs := result.get("messages"):
    result["last_message"] = msgs[-1]
```

所以下游组件要「只看最终回答」时不必自己切列表末尾。

**`_StateInputOmitted` 是给「可选 state 输入 socket」的哨兵。** docstring：「当管道提供这个默认值时，Agent 把对应的键从它的运行输入里排除。显式提供的值（包括 `None`）被保留」。

这与 a03 里 `_NoOutputProduced` 是同一类手法，但用途相反：那边表示「上游没给这个 socket」，这边表示「这个 socket 没被填，所以别把它当成用户传的 `None`」。两者都解决「`None` 无法区分『没提供』与『提供了 None』」这个同一问题。

**`_get_run_method_params` 用内省而不是硬编码。** 实现：

```python
sig = inspect.signature(instance.run)
return {name for name, p in sig.parameters.items() if p.kind != inspect.Parameter.VAR_KEYWORD}
```

它从 `run` 的签名里取出参数名（排除 `**kwargs`）。注释在 `run` 里也印证了这个用途：「运行时工具在 `run` 的签名里有显式参数」。所以「哪些 state 键可以被作为输入传入」这件事**由 `run` 的签名决定**，不靠单独的清单——加一个参数就自动多一个可传的键。

**`_initialize_fresh_execution` 有一处关于并发的重要注释。** 在 `_select_tools` 里：

```python
# Toolsets are spawned per run (see _spawn_tools / _select_tools_by_name) so concurrent runs
# sharing the same configured Toolset don't corrupt each other's run-scoped state.
if tools is None:
    return _spawn_tools(tools=self.tools)
```

而 `Toolset.spawn` 的 docstring 说明了默认行为与子类义务：「一个普通 `Toolset` 没有运行期作用域的状态，所以默认实现返回 `self` 并忽略选择（由 Agent 物化它）。**有运行期作用域状态的子类（例如 `SearchableToolset`）覆盖这个方法来返回一份携带选择的副本**，这样并发运行共享同一个配置好的 `Toolset` 时不会互相破坏」。

所以「每次运行 spawn 一份」是一个**契约而不是实现细节**：普通 Toolset 可以省掉拷贝，有状态的子类必须自己实现隔离。这是把成本留给需要的实现，而不是让所有人付拷贝的代价。

**`_initialize_fresh_execution` 里 State 的初始化顺序暴露了「哪些键必须一开始就存在」。** 它依次设：

```python
state.set("messages", messages)
state.set("step_count", 0)
state.set("max_agent_steps", self.max_agent_steps)
state.set("token_usage", {})
state.set("context_tokens", 0)
state.set("tool_call_counts", {tool.name: 0 for tool in flat_tools})
state.set("exit_reason", None)
state.set("continue_run", False)
state.set("tools", flat_tools)
state.set("hook_context", hook_context or {})
```

十个键。其中 `tool_call_counts` 按**每个工具名**预置 0（而不是空字典），所以输出里每个工具都有计数，哪怕从没被调用——这比「只列被调用过的」更有用（下游能区分「调了 0 次」与「这个工具不存在」）。

而用户传入的 `**kwargs` 只按 schema 过滤：

```python
state_kwargs = {key: kwargs[key] for key in self.resolved_state_schema.keys() if key in kwargs}
```

所以「多传的键」被静默忽略，不会报错。这与 `_public_outputs` 的宽松一致——这一层对输入输出都采取「按声明过滤」而不是「严格拒绝」。

**一个值得单独提的细节：`BEFORE_RUN` 钩子跑完之后，计数器从 State 里重读。**

```python
_run_hooks(hooks=self.hooks, hook_point=BEFORE_RUN, state=exe_context.state)
# A before_run hook can restore a saved State, so resume the execution counter from its step count.
exe_context.counter = exe_context.state.data.get("step_count", 0)
```

注释的理由是「一个 `before_run` 钩子可以恢复一份保存的 State，所以从它的 `step_count` 恢复执行计数」。这是「钩子能改变控制流」这条设计的一个具体后果：因为 State 可以整体替换，计数器不能只存在于 `_ExecutionContext` 里，必须每轮从 State 同步。

这也解释了为什么 `_ExecutionContext.counter` 与 `state["step_count"]` 是**两份数据**：前者是热循环里的快速变量，后者是持久化/可恢复的事实。它们通过「每步都 `state.set("step_count", ...)`」保持同步，并在每轮循环条件里用前者。

## 关键取舍

**源码事实：** `Agent` 是 `@component`，但它的运行状态存在一个显式的 `State` 对象里，State 同时是输入、输出与共享黑板。

**源码事实：** State 的键分三级——用户自定义、运行元数据（`_RUN_METADATA_STATE_KEYS`，只出不进）、内部状态（`_INTERNAL_STATE_KEYS`，不进不出）。`_public_outputs` 只过滤后者。

**源码事实：** 主循环用 `while...else`，`else` 分支专门承载「因 `max_agent_steps` 停止」这一种情况，避免覆盖某一步自己设的 `exit_reason`。

**源码事实：** `_ExecutionContext.tools` 存**未 flatten** 的工具集，每步重新 flatten，以便动态工具集在运行中浮现新工具。

**源码事实：** `stop_run` 在 LLM 调用**之前**检查，所以钩子能省掉一次真实的模型调用。

**源码事实：** `_get_model_exit_reason` 让 `length` / `content_filter` 优先于 `text`，且**空回复无终止原因时不退出**，以保留对畸形工具调用的恢复能力。

**源码事实：** `_pending_tool_call_messages_from_state` 只取 `messages[-1]`，它是 `before_tool` 钩子改写之后的重读点。

**源码事实：** `continue_run` 是消费型标志（读完即复位），所以「继续」必须每次重新设置；「无限继续」被 `max_agent_steps` 独立挡住。

**源码事实：** `_check_exit_conditions` 中「出错的退出条件工具会取消退出，即使另一个成功的退出条件工具也存在」。

**源码事实：** `BEFORE_RUN` 钩子跑完后计数器从 `state["step_count"]` 重读，因为钩子可以整体恢复 State。

**源码事实：** 工具集每次运行 `spawn` 一份；普通 `Toolset` 的 `spawn` 返回 `self`，有运行期状态的子类必须自己实现隔离。

**架构推论：** 「把循环状态放进 State」这个决定，让 Agent 成为**可暂停、可恢复、可被钩子干预**的组件。若状态是组件字段，那么「保存到一半的 agent 运行」不可能实现，钩子也无法参与控制流。代价是每步都要在 State 与局部变量之间同步（`counter` / `step_count` 两份），且任何新增的状态都必须显式声明进两级常量，否则会漏进输出或漏做初始化。

**架构推论：** 「只在最后一条消息上执行工具」这个限制看似武断，实际是让「钩子改写」有一个明确的落点。若 Agent 遍历所有消息的所有 tool_call，那钩子要阻止某个调用就必须精确地找到并删掉它；而「只看最后一条」把干预面缩成「改最后一条消息」这一个动作。**把可干预面收窄，是让扩展点可用的前提**。

**架构推论：** 那个「空回复不退出」的分支，是这一层里最反直觉但最有价值的设计。它承认了「chat generator 可能静默丢弃畸形工具调用」这个现实，并把 Agent 的响应定为「再试一轮」而不是「结束」。任何「消费外部模型输出」的循环都需要这样一个「什么都不像就继续」的兜底——因为对不可控的上游，「保守地继续」比「保守地停止」更容易恢复。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/agents/agent.py` | `Agent` | `@component` 的有状态循环组件 |
| `haystack/components/agents/agent.py` | `Agent.run` | `while/else` 主循环，`else` 专管步数耗尽 |
| `haystack/components/agents/agent.py` | `Agent._run_step` | 九段直线流程，钩子与工具的交错点 |
| `haystack/components/agents/agent.py` | `Agent._initialize_fresh_execution` | 建 State 并预置十个键 |
| `haystack/components/agents/agent.py` | `Agent._check_exit_conditions` | 出错工具取消退出，多命中返回第一个 |
| `haystack/components/agents/agent.py` | `Agent._continue_after_exit_hooks` | 先清标志、再跑钩子、最后消费 |
| `haystack/components/agents/agent.py` | `_ExecutionContext` | 存未 flatten 的工具集与两份计数器 |
| `haystack/components/agents/agent.py` | `_get_model_exit_reason` | 四判据，不完整原因优先且空回复不退出 |
| `haystack/components/agents/agent.py` | `_pending_tool_call_messages_from_state` | 钩子改写后的重读点，只看最后一条 |
| `haystack/components/agents/agent.py` | `_consume_continue_run` | 读并复位「继续」标志 |
| `haystack/components/agents/agent.py` | `_validate_hooks` | 构造期三道校验，含 `allowed_hook_points` |
| `haystack/components/agents/agent.py` | `_public_outputs` | 只排内部键，运行元数据照常输出 |
| `haystack/components/agents/agent.py` | `_StateInputOmitted` | 「可选 state socket 未被填」的哨兵 |
| `haystack/components/agents/agent.py` | `_RUN_METADATA_STATE_KEYS` | 四个只出不如的运行元数据键 |
| `haystack/components/agents/agent.py` | `_INTERNAL_STATE_KEYS` | 六个不进不出的内部状态键 |
| `haystack/components/agents/agent.py` | `_get_run_method_params` | 用 `run` 签名内省出可传的 key |

## 参考

- [haystack 源码：components/agents/agent.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/agents/agent.py)
- [haystack 文档：Agent](https://docs.haystack.deepset.ai/docs/agent)
- [haystack 3.0 迁移说明](https://docs.haystack.deepset.ai/docs/migration-guide-3.0)

## 与主核 / AI 的关系

主核里最接近的是「事务中的会话状态」：一个会话执行过程中有临时状态（当前事务 id、锁持有者、游标位置），这些状态在语句之间传递，且必须能被中断后恢复。

`Agent` 的 State 与那个会话状态在职责上一致，差别在**谁能改它**。主核的会话状态由引擎独占修改，客户端只能通过语句间接影响；这里 State 是完全开放的——钩子、工具、甚至 `before_run` 都能整体替换它。所以这一层的设计重心不是「保护状态」，而是「让多方协作修改状态时有明确的顺序与重读点」。

与 a03（组件就绪判定）的连接很直接：那一页讲「组件该不该跑」，而 `Agent` 是那个判定的特殊消费者——它一旦被触发就会自己循环很多轮，所以在管道看来它是「一次运行内部做了 N 步」。`step_count` 作为输出暴露，正是为了让外部（以及钩子）看得见这个内部循环的进度。

与本组 h02（工具调用执行）的分工：这一页管「什么时候调、调完怎么继续」，h02 管「实际怎么调、并发怎么排、结果怎么合回状态」。`_run_step` 里那一行 `tool_messages, exe_context.state = _run_tool(**tool_execution_inputs)` 就是两页的分界线。

与 i03（钩子）的关系更紧：`Agent` 里六处 `_run_hooks` 调用（`BEFORE_RUN` / `BEFORE_LLM` / `BEFORE_TOOL` / `AFTER_TOOL` / `ON_EXIT` / `AFTER_RUN`）是这个组件最复杂的部分，而它们全部依赖 State 传递信息。所以「状态是共享黑板」这个决定，首先是为了让钩子有意义。

对 AI 工程最实用的一条是「消费型标志」。`continue_run` 读完即复位，这个模式能杜绝「一次授权永久生效」这类问题。任何「钩子能延长循环」的设计都该这么做——否则一个写得宽松的钩子（例如「只要还有预算就继续」）会让循环一直跑到步数上限，而调试时很难看出是钩子的问题。

另一条是「不完整输出优先于内容判断」。`finish_reason == length` 时即使有 `text` 也判为截断——这条规则避免了「把半截答案当完整答案返回」，而那类 bug 极难在生产里发现（用户拿到的是通顺但缺后半段的回答）。

最后一条是「空回复不退出」。这个选择假定上游模型的行为不可靠，并用「再试一轮」来对冲。代价是可能多花一次模型调用；收益是遇到畸形工具调用时能自愈。判据是：**这个循环的上游有没有可能静默失败？** 有的话，就该有一个「什么都不像就继续」的兜底分支，而不是把所有异常态映射到「结束」。
