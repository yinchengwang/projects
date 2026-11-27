---
project: crewai
feature: hooks
title: 工具与模型调用钩子
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/hooks/dispatch.py
    symbols: [register, InterceptionPoint, HookAborted]
  - path: lib/crewai/src/crewai/hooks/tool_hooks.py
    symbols: [run_before_tool_call_hooks, register_before_tool_call_hook]
  - path: lib/crewai/src/crewai/hooks/llm_hooks.py
    symbols: [before_llm_call_reducer, LLMCallHookContext, register_before_llm_call_hook]
---

# 工具与模型调用钩子

## 一句话职责

在模型调用与工具调用前后提供可中断的拦截点：钩子能就地改消息、改工具参数、改返回结果，或用 `HookAborted` 直接叫停；老的「返回 False 表示拦截」写法与新的「抛异常表示拦截」写法共用同一条有序队列。

## 机制是什么

这套机制有一个统一引擎加四个适配层。引擎是 `hooks/dispatch.py`：一个以 `InterceptionPoint` 为键的全局钩子表，加一份 `ContextVar` 里的「执行作用域」钩子表；运行顺序固定为**全局先、作用域后**，镜像了事件总线用 `ContextVar` 做作用域的做法。四个适配层是 `before/after_llm_call` 与 `before/after_tool_call`，各自把自己那套旧式注册接口（`register_*`、模块级列表、`return False`）接到同一个队列上。

`InterceptionPoint` 枚举现有 10 个点：执行级的 `EXECUTION_START`、`INPUT`、`OUTPUT`、`EXECUTION_END`；模型/工具边界 `PRE_MODEL_CALL`、`POST_MODEL_CALL`、`PRE_TOOL_CALL`、`POST_TOOL_CALL`；以及 `PRE_STEP`、`POST_STEP`。源码注释说明「新点随使用它的接缝一起引入，所以枚举只列有活的消费者」。

钩子的调用约定是「同步函数，接收一个类型化上下文，可以就地改它，也可以返回替代载荷，或抛 `HookAborted` 停止操作」。`HookAborted` 带 `reason` 与 `source` 两个字段，`source` 用于遥测和错误信息标识是谁中止的。

## 怎么实现的

**分派的最小实现。** `dispatch(point, ctx, reducer=None, verbose=True)` 先 `_resolve_hooks(point)` 拿到有序列表；空列表就**直接返回 ctx**（注释称为「no-op 快路径，一次字典查找」），否则交给 `run_hooks`。

`run_hooks` 逐个调 `_invoke_hook`，一个 hook 返回真表示上下文被改过，最后按「是否改过」决定遥测 outcome 是 `modified` 还是 `proceeded`。

**`_invoke_hook` 的三条出口。** 调 `hook(ctx)` 再 `reducer(ctx, result)`。异常处理分两种：`HookAborted` 先补上 `source`（若钩子自己没设）再重抛——它是设计上要传播的；**其他任何异常都被吞掉**并返回 `False`，`verbose` 为真时打一条黄色提示。模块 docstring 把这个取舍写得很明确：`HookAborted` 按设计传播，其他异常 fail-open，因为「框架要保护自己不被有 bug 的用户钩子拖垮」。

**默认 reducer 的语义。** `_default_reducer`：`result is not None` 且 `ctx` 有 `payload` 属性时，把 `result` 赋给 `ctx.payload` 并返回 `True`。这个「非 None 返回即替换载荷」是通用约定；具体适配层会用自己的 reducer 覆盖它。

**遥测。** `_emit_telemetry` 构造 `HookDispatchedEvent`：拦截点、outcome（`proceeded`/`modified`/`aborted`）、钩子数量、耗时毫秒、中止原因与来源。整段包在 `try/except: pass` 里，注释写「遥测绝不能打断分派」。它放在 `finally` 里，所以中止路径也会发。

**过滤器包装。** `on(point, agents=None, tools=None)` 装饰器把 `point`、`_filter_tools`、`_filter_agents` 存到函数属性上。若不是方法（第一个参数不是 `self`），立刻 `register` 注册；若有过滤条件，注册的是 `_wrap_with_filters` 包出来的闭包——它在调用时检查 `ctx.tool_name` 是否在 `tools` 里、`ctx.agent.role`（或 `ctx.agent_role`）是否在 `agents` 里，不匹配就返回 `None`（等价于「不改」）。注册完成后把真实注册的可调用体存回 `func._registered_hook`，让 `unregister_hook(func)` 能找回包装器。

方法形态（在 `@CrewBase` 类里定义、第一个参数是 `self`）则**推迟注册**，注释说留给 crew 初始化时按 crew 作用域注册。

**作用域钩子。** `scoped_hooks()` 上下文管理器把一个字典设进 `_scoped_hooks_var`，退出时 reset。`register_scoped` 在这个作用域里追加，未进入作用域就调会 `RuntimeError`。`get_scoped_hooks` 让那些「已经快照了自己的钩子列表」的接缝（例如每个执行器的 LLM 钩子列表）能把作用域钩子按同样的顺序并进来。

**工具侧适配。** `tool_hooks.py` 把模块级的 `_before_tool_call_hooks` 与 `_after_tool_call_hooks` 直接**别名到**引擎的全局列表上（`get_global_hook_list(PRE_TOOL_CALL)` 返回的那个 list 对象）。所以老的 `register_before_tool_call_hook` 与新的 `@on(PRE_TOOL_CALL)` 写进同一个有序队列——注释专门说明了这个别名关系。

两个 reducer 承担旧约定的翻译。`before_tool_call_reducer`：`result is False` 就 `raise HookAborted(reason="before_tool_call hook returned False")`，否则返回 `False`（表示只有就地改动，没有载荷替换）。`after_tool_call_reducer`：`result` 是 `str` 就写进 `context.tool_result` 并返回 `True`，否则 `False`。

`run_before_tool_call_hooks` 把 `HookAborted` 捕获并返回 `True`（表示「被拦了」），其余情况返回 `False`。所以调用方看到的是一个布尔，而引擎内部的异常被适配层吃掉了。`run_after_tool_call_hooks` 直接返回 `context.tool_result`（可能被钩子改过）。

**模型侧适配。** `llm_hooks.py` 同理别名到 `PRE_MODEL_CALL` / `POST_MODEL_CALL`。`before_llm_call_reducer` 在 `result is False` 时抛的是 **`LegacyHookBlocked`**——`HookAborted` 的子类。注释解释了为什么需要这个子类：布尔约定要保留「映射成文档里的 `ValueError`」这条老行为，而钩子自己抛的 `HookAborted` 必须原样抵达调用方，两者不能混。

`after_llm_call_reducer`：`result` 非 `None` 且是 `str` 时替换 `context.response`。

**上下文对象。** `LLMCallHookContext` 有两种构造方式：给 `executor` 就从它身上取 `messages`、`llm`、`iterations`、`agent`、`task`、`crew`（`AgentExecutor` 有这三个属性；`LiteAgent` 没有 `agent` 属性，走 `original_agent` 分支并把 task/crew 置 `None`）；不给 executor 就直接用传入的参数，`iterations` 为 0。所以同一套钩子既能服务 crew 里的执行器，也能服务直接的 LLM 调用。

`ToolCallHookContext` 更简单：`tool_name`、`tool_input`、`tool`、`agent`、`task`、`crew`、`tool_result`、`raw_tool_result`。

两个上下文都提供 `request_human_input(prompt, default_message)`：暂停实时刷新、打印提示、阻塞读一行、恢复刷新。这让「工具调用前的审批门」可以只用几行钩子实现。

**双重分派防护。** `model_call_hooks_dispatched()` 上下文管理器把 `_model_call_hooks_dispatched` 置真。执行器用自己的富上下文（executor、task、crew）分派之后才走到 LLM 层，而 `BaseLLM._invoke_before_llm_call_hooks` 开头会检查这个标记，为真就直接返回——避免同一次调用被分派两遍。注释写明了理由。

**LLM 层的异常翻译。** `BaseLLM._invoke_before_llm_call_hooks` 捕获 `LegacyHookBlocked`，打印提示并抛 `LLMCallBlockedError`（一个 `ValueError` 子类）。选择 `ValueError` 是有意的：注释说这样「内部的模型调用周围那些 fail-open 处理器会继续吸收它」。

## 关键取舍

**源码事实：** 除 `HookAborted` 外的钩子异常全部被吞。这是明确的 fail-open 策略，理由是保护框架不被用户钩子拖垮。代价是「钩子静默不生效」——只有 `verbose=True` 时才打一行黄色提示。

**源码事实：** 遥测在 `finally` 里发，所以中止也会记一条 `aborted`。但整个遥测函数包在 `except: pass` 里，所以遥测失败不留任何痕迹。两处「不打断」叠加，意味着这条链路完全不可观测自己是否在工作。

**源码事实：** 老式「返回 `False` 拦截」被 reducer 翻译成 `HookAborted` 或 `LegacyHookBlocked`。两个适配层选了不同的子类，因为 LLM 层要把布尔拦截映射成 `LLMCallBlockedError`，而工具层只需要一个布尔结果。同一语义、两种下游契约。

**源码事实：** `on` 装饰器对「方法」形态不注册，只打属性。这意味着在类里定义的钩子必须由某个初始化流程去注册，否则完全不生效——且没有任何警告。

**源码事实：** 钩子是同步的。模块注释明确说「可能从异步接缝里被调用，所以不能阻塞在重 I/O 上」。所以想在钩子里做异步审批，只能自己桥接（而 `request_human_input` 用的正是阻塞的 `input()`）。

**架构推论：** 「就地上改」与「返回替换」两种修改方式并存，是这套设计的核心张力。上下文里的 list/dict 是共享引用，钩子 `append` 或改键就生效；但**替换整个引用**（`ctx.messages = []`）无效，因为执行器早先已经持有原对象。两处上下文类的 docstring 都用大段文字强调这一点，说明这是最高频的误用。

**架构推论：** 把两份旧式注册表**别名**到引擎的全局列表，而不是「迁移旧注册到新列表」，是让新旧方言共存的省事做法：一个 list 对象被两处引用，追加即共享。代价是任何清空操作都会同时影响两边——`clear_all_tool_call_hooks` 与引擎的 `clear` 操作的是同一个对象。

**架构推论：** `EXECUTION_BOUNDARY_POINTS` 这个 frozenset 把「开启与闭合一整次运行」的四个点与「运行内的步骤」区分开。这层分类存在但在这几页涉及的接缝里没被用到——它服务于更上层的介入（例如在整个运行前后加审批），说明枚举先于消费者扩展是这套设计的预期节奏。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/hooks/dispatch.py` | `InterceptionPoint` | 十个拦截点的枚举 |
| `lib/crewai/src/crewai/hooks/dispatch.py` | `HookAborted` | 中止信号，带 reason 与 source |
| `lib/crewai/src/crewai/hooks/dispatch.py` | `register` | 全局钩子登记 |
| `lib/crewai/src/crewai/hooks/dispatch.py` | `dispatch` | 解析全局加作用域钩子并分派，空表走快路径 |
| `lib/crewai/src/crewai/hooks/dispatch.py` | `run_hooks` | 逐个执行并汇总 outcome 与耗时遥测 |
| `lib/crewai/src/crewai/hooks/dispatch.py` | `_invoke_hook` | 单钩子执行，`HookAborted` 传播、其余异常吞掉 |
| `lib/crewai/src/crewai/hooks/dispatch.py` | `_default_reducer` | 非 None 返回替换 `ctx.payload` |
| `lib/crewai/src/crewai/hooks/dispatch.py` | `on` | 装饰器登记，支持 agents/tools 过滤与方法延迟注册 |
| `lib/crewai/src/crewai/hooks/dispatch.py` | `scoped_hooks` | 执行作用域钩子表 |
| `lib/crewai/src/crewai/hooks/tool_hooks.py` | `register_before_tool_call_hook` | 旧式工具前钩子登记，写进引擎同一列表 |
| `lib/crewai/src/crewai/hooks/tool_hooks.py` | `run_before_tool_call_hooks` | 返回布尔表示是否被拦 |
| `lib/crewai/src/crewai/hooks/tool_hooks.py` | `ToolCallHookContext` | 工具调用上下文，含人工审批入口 |
| `lib/crewai/src/crewai/hooks/llm_hooks.py` | `LLMCallHookContext` | 模型调用上下文，可从执行器或直连参数构造 |
| `lib/crewai/src/crewai/hooks/llm_hooks.py` | `before_llm_call_reducer` | 布尔拦截翻译成 `LegacyHookBlocked` |
| `lib/crewai/src/crewai/hooks/llm_hooks.py` | `register_before_llm_call_hook` | 旧式模型前钩子登记 |
| `lib/crewai/src/crewai/hooks/llm_hooks.py` | `model_call_hooks_dispatched` | 防止执行器与模型层重复分派 |

## 参考

- [CrewAI 源码：hooks/dispatch.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/hooks/dispatch.py)
- [CrewAI 源码：hooks/tool_hooks.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/hooks/tool_hooks.py)
- [CrewAI 文档：Event Listeners 与钩子](https://docs.crewai.com/en/concepts/event-listener)

## 与主核 / AI 的关系

主核里最接近的是「扩展钩子」（例如执行器钩子、认证钩子）：在固定位置回调用户代码，返回特定值可以改变默认行为。差别在于主核的钩子出错通常必须让上层知道（否则可能破坏一致性），而这里的钩子全 fail-open。这条差异不是风格问题，而是**被保护对象不同**：主核保护数据正确性，这里保护一次运行的可用性。

与 AutoGen 相比，AutoGen 的对应机制是 `InterventionHandler`：一个可以拦截入站/出站消息、丢弃或改写消息的处理器。两者思路相近但作用面不同——AutoGen 只在消息收发这一层拦截，CrewAI 在模型调用前后与工具调用前后都有独立的拦截点，且钩子能拿到完整的执行上下文（agent、task、crew、iterations、raw 结果）。CrewAI 也没有「丢弃消息」这种操作，取而代之的是 `HookAborted` 中止当前操作并带原因。

对 AI 工程最有价值的是 `request_human_input` 这个挂在上下文上的方法。它把「人在环审批」的实现成本压到几行：钩子里判断工具名，弹一行提示，读一行输入，不批准就返回 `False`。相比之下，把审批做成工具或独立 agent 要处理消息路由与状态。注意它用的是阻塞 `input()`，所以只适合 CLI 与脚本，不适合服务端。

另一条是「就地上改」这条约定。它是把「可变引用」当成 API 的一部分：钩子拿到的是执行器正在用的那个 list，改它就等于改执行器。这样无需返回值就能做复杂修改（插入、删除、重排），但「替换引用」无效，而且不会有任何报错。设计这种钩子接口时，要么明确只支持一种方式并强制检查，要么在文档与 docstring 里反复强调——CrewAI 选了后者。

最后是那个双重分派防护。同一个逻辑接缝被两层都包了钩子（执行器一层、LLM 一层），如果不加标记就会出现「同一次调用钩子跑两遍」，症状是副作用重复（日志重复、审批弹两次）。用一个 ContextVar 标记「已经分派过」，比让两层各自判断上下文里有没有 executor 更稳。
