---
project: langchain
feature: callback-manager
title: 回调管理器与运行管理器
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/callbacks/manager.py
    symbols: [CallbackManager, BaseRunManager, RunManager, AsyncCallbackManager]
  - path: libs/core/langchain_core/callbacks/manager.py
    symbols: [CallbackManagerForLLMRun, CallbackManagerForToolRun, ParentRunManager]
---

# 回调管理器与运行管理器

## 一句话职责

把「一次运行开始/结束/出错」这类事件分发到一组外部观察者（日志、追踪、流式输出），并让每个事件**绑定它所属的那一次运行**——于是同一份 handler 列表在嵌套的链、模型、工具、检索器调用中不必各自携带上下文。

## 机制是什么

这个文件 2797 行，是仓库里最大的几个文件之一。它实现两套并行的体系：

| 体系 | 角色 | 生命周期 |
|---|---|---|
| `CallbackManager` / `AsyncCallbackManager` | **配置容器**：持有一组 handler 与标签/元数据 | 跨越多次运行，随调用链传递 |
| `BaseRunManager` 及其子类 | **运行句柄**：一个 handler 列表 + 一个 `run_id` + 一份快照的标签元数据 | 与单次运行同寿 |

两者的关系是：`CallbackManager.on_llm_start(...)` **执行事件分发并返回**一个 `CallbackManagerForLLMRun`；后续的结束/出错/流式事件都通过这个句柄发出。

运行句柄的继承树（同步侧）：

```
BaseRunManager            run_id + handlers + parent_run_id + tags/metadata（各分 inherit/local 两套）
├── RunManager            + on_text / on_retry
│   ├── ParentRunManager  + get_child()          → 能孕育子运行
│   │   ├── CallbackManagerForChainRun       (+ on_chain_end/error, on_agent_action/finish)
│   │   └── CallbackManagerForToolRun        (+ on_tool_end/error)
│   │   └── CallbackManagerForRetrieverRun   (+ on_retriever_end/error)
│   └── CallbackManagerForLLMRun             (+ on_llm_new_token/end/error, on_stream_event)
└── AsyncRunManager (ABC，要求 get_sync) — 与上面完全镜像的一套
```

## 怎么实现的

**「只会记录，不会说话」的基类被抽出去了。** `BaseRunManager` 只做一件事：把八个参数存成字段。`on_text` / `on_retry` 在 `RunManager` 上，而「链能生工具、模型不能生子运行」这个差别由**是否继承 `ParentRunManager`** 表达。

这个切分的价值在于：`CallbackManagerForLLMRun` 没有 `get_child`，于是「在模型运行里再开一个子运行」在类型层面就不成立。而 `CallbackManagerForToolRun` 有 `get_child`——因为工具里可以调用别的 Runnable。

**`get_child` 的实现是「继承可继承的那部分，再加一个局部标签」。** 代码是：

```
manager = CallbackManager(handlers=[], parent_run_id=self.run_id)
manager.set_handlers(self.inheritable_handlers)
manager.add_tags(self.inheritable_tags)
manager.add_metadata(self.inheritable_metadata)
if tag is not None:
    manager.add_tags([tag], inherit=False)
```

四个动作各自有含义：**handler 列表换成「可继承的那个子集」**（不是当前的完整列表），**标签与元数据也只带可继承部分**，而 `tag` 用 `inherit=False` 加进去——意味着这个标签**不会再传给孙运行**。

这条 `inherit=False` 是父子关系的核心语义：`get_child` 传递的是「允许继承的东西」，而 `tag` 是「只描述这一层的东西」。于是嵌套三层时，第一层的局部标签不会出现在第三层。

**`handle_event` 是同步分发的唯一入口，它做四件事。**

1. **按 `ignore_condition_name` 过滤**：`if ignore_condition_name is None or not getattr(handler, ignore_condition_name)`——例如 `on_llm_start` 传 `"ignore_llm"`，于是声明了 `ignore_llm = True` 的 handler 会被跳过。
2. **调 handler 方法**，若返回值是 coroutine 就收集起来（**同步上下文里也可能有异步 handler**）。
3. **捕获 `NotImplementedError` 并尝试降级**：`on_chat_model_start` 没实现时转调 `on_llm_start`，把消息列表先转成字符串。
4. **捕获其它异常并 `warn`**，只有 `handler.raise_error` 为真时才 `raise`。

第 1 条与 `ignore_*` 那组属性配套。`BaseCallbackHandler` 用七个 `@property` 声明 `ignore_llm`、`ignore_retry`、`ignore_chain`、`ignore_agent`、`ignore_retriever`、`ignore_chat_model`、`ignore_custom_event`，全部默认返回 `False`。

**用属性而不是字段是有理由的**：一个 handler 若想在运行期动态决定「现在要不要忽略 LLM 事件」（比如按日志级别、按采样率），只需重写一个 property；若是布尔字段，就必须在构造时定死。

**`on_chat_model_start` 的降级链是本页最细的一处兼容设计。** 一套 handler 可能只实现了 `on_llm_start`（老接口，参数是 prompt 字符串列表），而新代码调的是 `on_chat_model_start`（参数是消息列表的列表）。

于是两处都要处理这个错配：

- 同步路径：`except NotImplementedError` 里判断事件名是 `on_chat_model_start`，就把 `args[1]`（消息组）逐组 `get_buffer_string` 转成字符串，再递归调 `handle_event([handler], "on_llm_start", "ignore_llm", ...)`。
- 异步路径：`_achat_model_start_fallback` 包装协程，捕获 `NotImplementedError` 后调 `_ahandle_event_for_handler(handler, "on_llm_start", "ignore_llm", ...)`。

**异步路径那层包装的出现理由写在它的文档注释里，且理由很具体：** 同步 `handle_event` 会先把异步 handler 的协程收集到 `coros` 里，稍后由 `_run_coros` 统一执行；若不在收集时就包一层，那个 `NotImplementedError` 会被 `_run_coros` 的通用 `except` 吞掉，**于是这次追踪就丢了**。

也就是说这层包装不是可选的整洁做法，而是为了在「异步 handler 被同步路径调用」这个特殊情形下保住降级语义。

**同步路径收尾时的线程池那段是必要的绕行。** `finally` 里先判有没有运行中的事件循环：

```
try: asyncio.get_running_loop(); loop_running = True
except RuntimeError: loop_running = False
```

- **有运行中的 loop** → `_executor().submit(copy_context().run, _run_coros, coros).result()`：丢到全局线程池里跑完再等结果。
- **没有 loop** → 直接 `_run_coros(coros)`。

注释解释了第一条：若把协程提交给正在运行的 loop，会**死锁**——因为当前就在那个 loop 里执行一个同步函数，它无法被打断去跑新任务。所以只能换线程。

这是一个「同步 API 里混入异步 handler」必然要付的代价：异步 handler 在同步调用链里**不能真正并发**，只能整体挪到一个线程里跑完。

**`_run_coros` 用 `asyncio.Runner` 而不是 `asyncio.run`，且吞掉全部异常。** 实现是逐个 `runner.run(coro)`，每个都 `try/except` 并 `warn`；然后 `while pending := asyncio.all_tasks(...): runner.run(asyncio.wait(pending))`——把协程里派生出的任务也跑完。

那段注释说明了它与异步路径的**不对称**：

> 这些协程抛出的异常一律在这里被记录并吞掉，**无论 handler 的 `raise_error` 设置是什么**。于是通过同步 `handle_event` 触发的异步 handler 错误永远不会传播，这与同步 handler 的错误（它们遵守 `raise_error`）不同。这是同步与异步回调路径之间**既有的不对称**。

作者没有试图修掉这个不对称，而是把它写下来。这个处理方式值得记：**一个已知的语义缺口，如果不修，就必须写在代码里**——否则下一个人会以为 `raise_error` 对所有 handler 都有效。

**`ahandle_event` 分成两批，这一批的划分是性能与语义的交界。**

```
for handler in [h for h in handlers if h.run_inline]:
    await _ahandle_event_for_handler(...)      # 串行
await asyncio.gather(*(... for handler in handlers if not handler.run_inline))   # 并发
```

- `run_inline = True` 的 handler **串行且 await 完成**——因为它的语义依赖「按顺序、在当前上下文里跑」。
- 其余 handler **并发 gather**——它们互相独立。

`_ahandle_event_for_handler` 内部还有一层分流：如果 handler 方法本身是 `async def`，直接 `await`；如果它是**同步函数**而 `run_inline` 为真，直接调用；否则 `await get_running_loop().run_in_executor(None, ...)` 丢到默认线程池。

也就是说「同步 handler 在异步路径里」会被挪到线程池——避免阻塞事件循环。这与同步路径里「异步 handler 被挪到线程池」正好是对称的镜像，两处都是同一个问题（**执行模型与 handler 的模型不一致**）的两个方向。

**`shielded` 装饰器的作用是「让这个 await 不被取消」。** 实现是捕获当前 context、`create_task(coro, context=ctx)`、再 `await asyncio.shield(task)`。

文档只说了「始终屏蔽取消」。而它的存在场景可以从名称与用法推出：**回调是旁路观察，不该因为主流程被取消而半途中断**——一个已经开始写的追踪记录若被取消在半路，会留下不一致的记录。

`copy_context()` 那一步保证 contextvar（例如 `tracing_v2_callback_var`）在新 task 里可见，这也是回调能自动拿到当前 tracer 的前提。

**`CallbackManager.on_llm_start` 返回的是一个列表而不是单个句柄，这个形状有原因。** 实现里对 `prompts` 逐个循环：每个 prompt 一次 `handle_event`、一个 `run_id`、一个 `CallbackManagerForLLMRun`。

而 `run_id` 的处理是 `run_id if i == 0 and run_id is not None else uuid7()`——**调用方给的 run_id 只用于第一个**，其余全部新生成。注释写着「不能有两个相同 run id 的运行」。这是一次正确的防御：一个 batch 调用里若有两条用同一个 id，追踪记录会互相覆盖。

**`_configure` 是「配置容器怎么从外部输入组装出来」的全部逻辑，它有四段值得拆开。**

第一段：**标签与元数据都分「可继承」与「局部」两套**。`add_tags(tags, inherit=True/False)` 与 `add_metadata(...)` 各调两次。于是 `inherit=False` 的那些**只对这个 manager 及其派生可见性有限**——具体由 `set_handlers` / `get_child` 传递「可继承子集」来实现。

第二段：**外部追踪上下文与 LangChain 上下文的父子关系要靠「点分序号」判断。** 代码是：

```
if parent_run_id is not None and (parent_run_id_ is None or (run_tree and str(parent_run_id_) in run_tree.dotted_order)):
    parent_run_id_ = parent_run_id
```

注释给出理由：**如果 LangChain 的父 id 已经反映在 run tree 里（在它的 `dotted_order` 里），说明 run tree 要么是同一个父、要么是它的子**；否则就认为 LangChain 上下文已经跑在 run tree 之外，**不该继承那个父**。

这一段处理的是两套追踪系统的挂接：一个 LangSmith `@traceable` 函数里调 LangChain 代码，两边各有自己的 run 层级，必须决定「谁是父」。判据用 `dotted_order`（LangSmith 的层级编码路径）而不是简单的时间或 id 比较，是因为**只有那个字符串能表达「包含关系」**。

第三段：**旧版追踪环境变量会直接报错，而不是静默忽略。** 判据是：

```
v1 = env_var_is_set("LANGCHAIN_TRACING") or env_var_is_set("LANGCHAIN_HANDLER")
v2 = _tracing_v2_is_enabled()
if v1 and not v2: raise RuntimeError("Tracing using LangChainTracerV1 is no longer supported. ...")
```

注意条件是 `v1 and not v2`——**两者都设了就不报错，只静默忽略 v1**（注释原话「if both are enabled, can silently ignore the v1 tracer」）。

这个设计的方向是对的：给还在用 v1 变量名的人一个明确的迁移错误，但给已经迁到 v2 又没删旧变量的人一条静默通过的路径。

第四段：**`verbose` / `debug` / `tracing_v2` 三者各自注入对应的 handler，且都带「已存在就不重复加」的检查。** 三个分支分别注入 `StdOutCallbackHandler`、`ConsoleCallbackHandler`、`LangChainTracer`。

其中 v2 那支还有一个回退：若 `tracing_v2_callback_var` 里已有 tracer 就用它，否则现造一个；造失败只 `warn` 并给出「unset `LANGCHAIN_TRACING_V2` 以关闭这个警告」的指引。

**`_configure` 的导入是函数内延迟的，注释给了具体数字**：**为了避开在模块层 import langsmith（约 132ms）**。也就是说 `manager.py` 被 import 时不能连带 langsmith——而它确实被很多不追踪的场景 import。这是一个用「延迟导入 + 注释里的耗时数字」记录起来的性能决定。

**`trace_as_chain_group` 解决的是一个具体缺口：几次彼此独立、不组成一条 Runnable 链的调用想要被当成同一个运行。** 它返回 `CallbackManagerForChainGroup`，并在退出时结束那个分组运行。

这个 API 的存在说明了「运行」与「调用」的层级并不总由调用结构决定——有时需要人来声明「这几步算一次」。

## 关键取舍

**源码事实：** 运行句柄的「能不能生子运行」由**是否继承 `ParentRunManager`** 表达；`CallbackManagerForLLMRun` 不继承，所以「在模型运行里开子运行」在类型上不成立。

**源码事实：** `ignore_*` 是七个 `@property`（全部默认 `False`）而不是布尔字段，于是 handler 可以在运行期动态决定是否忽略某类事件。

**源码事实：** `handle_event` 的异常策略分两层：`NotImplementedError` 走降级（`on_chat_model_start` → `on_llm_start`），其它异常 `warn` 且只在 `raise_error` 为真时抛出。

**源码事实：** 同步路径里的异步 handler 会被「收集后挪到全局线程池执行」，因为直接提交给正在运行的 loop 会死锁。

**源码事实：** `_run_coros` **无条件吞掉异常并 warn**，`raise_error` 对它无效。源码注释明确把这称为「同步与异步回调路径之间既有的不对称」。

**源码事实：** 异步路径的批处理把 `run_inline` 的 handler 串行 `await`，其余 `gather` 并发；同步 handler 在异步路径里会被丢进 `run_in_executor`。

**源码事实：** `get_child` 只传「可继承」的 handler/标签/元数据，`tag` 参数以 `inherit=False` 加入而不外传。

**源码事实：** `on_llm_start` 因为一个调用可能带多个 prompt，返回句柄列表；调用方给的 `run_id` 只用于第一个。

**源码事实：** `_configure` 用 `run_tree.dotted_order` 判断外部追踪上下文与 LangChain 上下文谁是父。

**源码事实：** 旧版 `LANGCHAIN_TRACING` 在未启用 v2 时报 `RuntimeError`，但两者都设时静默忽略 v1。

**源码事实：** `_configure` 内 `langsmith` 是函数内延迟导入，注释给出约 132ms 的耗时。

**架构推论：** 这个文件的结构说明了回调系统的核心矛盾：**它必须同时支持「同步与异步」「配置容器与运行句柄」「LangChain 自己的层级与外部追踪的层级」三个正交维度**。前两个维度靠镜像继承树解决（同步一套、异步一套，`get_sync` 做桥接），第三个维度靠 `dotted_order` 那段判断解决。

而这三者的交叉点就是那两个函数（`handle_event` / `ahandle_event`），也正因为如此它们里面塞满了「执行模型不一致时怎么办」的分支。**这个文件之所以有 2797 行，大半来自这些交叉情形，而不是来自事件本身的数量。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/callbacks/manager.py` | `CallbackManager` | 同步配置容器，各 `on_*_start` 分发并返回运行句柄 |
| `libs/core/langchain_core/callbacks/manager.py` | `AsyncCallbackManager` | 异步版本，与同步版镜像 |
| `libs/core/langchain_core/callbacks/manager.py` | `BaseRunManager` | 运行句柄基类：run_id + handler 列表 + 标签元数据（各分两套） |
| `libs/core/langchain_core/callbacks/manager.py` | `BaseRunManager.get_noop_manager` | 什么都不做的空管理器 |
| `libs/core/langchain_core/callbacks/manager.py` | `RunManager` | 加 `on_text` / `on_retry` |
| `libs/core/langchain_core/callbacks/manager.py` | `ParentRunManager` | 加 `get_child`，只传可继承部分 |
| `libs/core/langchain_core/callbacks/manager.py` | `CallbackManagerForLLMRun` | 模型运行句柄：token/end/error/stream_event |
| `libs/core/langchain_core/callbacks/manager.py` | `CallbackManagerForChainRun` | 链运行句柄：可生子运行 + agent 动作事件 |
| `libs/core/langchain_core/callbacks/manager.py` | `CallbackManagerForToolRun` | 工具运行句柄：可生子运行 + `on_tool_end` / `on_tool_error` |
| `libs/core/langchain_core/callbacks/manager.py` | `handle_event` | 同步分发：ignore 过滤、协程收集、降级、raise_error |
| `libs/core/langchain_core/callbacks/manager.py` | `ahandle_event` | 异步分发：inline 串行 + 其余并发 |
| `libs/core/langchain_core/callbacks/manager.py` | `_run_coros` | 用 `asyncio.Runner` 跑收集到的协程，异常全吞 |
| `libs/core/langchain_core/callbacks/manager.py` | `_achat_model_start_fallback` | 保住「异步 handler 在同步路径」下的降级语义 |
| `libs/core/langchain_core/callbacks/manager.py` | `shielded` | 让回调的 await 不被取消 |
| `libs/core/langchain_core/callbacks/manager.py` | `_configure` | 四段组装逻辑：继承、父子判定、v1 报错、handler 注入 |
| `libs/core/langchain_core/callbacks/manager.py` | `trace_as_chain_group` | 把几次独立调用包成一个分组运行 |

## 参考

- [langchain 源码：callbacks/manager.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/callbacks/manager.py)
- [langchain 文档：回调](https://python.langchain.com/docs/concepts/callbacks/)
- [Python 文档：contextvars](https://docs.python.org/3/library/contextvars.html)
- [Python 文档：asyncio.Runner](https://docs.python.org/3/library/asyncio-runner.html)

## 与主核 / AI 的关系

这套「配置容器 + 运行句柄」的两层结构，与主核里「连接句柄 + 语句句柄」的分层同构：数据库也把「这次会话的配置（隔离级别、超时）」与「这条语句的执行状态（语句 id、游标）」分开，事件（日志、统计、审计）挂在前者、状态挂在后者。两者都在处理同一个问题：**一份配置要被多次运行复用，而每次运行的上下文不同**。

对 agent 工程的直接价值有三条。其一，**「能不能派生下级」应当用类型表达，不要用标志位**。`ParentRunManager` 的有无就是这条规则的实例——比 `can_spawn_child: bool` 好，因为前者在写错时是编译/类型错误而非运行期静默失败。其二，**执行模型不一致时的绕行必须写注释**。同步路径里跑异步 handler 要换线程、异步路径里跑同步 handler 也要换线程，两处都是死锁/阻塞的规避；不写注释的话，下一个人会以为这是随意选择。其三，**已知的语义缺口要留在源码里**。`_run_coros` 无条件吞异常这件事被注释明确标注为「既有不对称」，这比悄悄留着好得多——它至少能让读代码的人知道 `raise_error` 在这条路径上无效。
