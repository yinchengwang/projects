---
project: langchain
feature: tracers
title: 追踪器与事件流
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/tracers/base.py
    symbols: [BaseTracer, AsyncBaseTracer]
  - path: libs/core/langchain_core/tracers/event_stream.py
    symbols: [RunInfo, _AstreamEventsCallbackHandler]
  - path: libs/core/langchain_core/tracers/context.py
    symbols: [tracing_v2_enabled]
---

# 追踪器与事件流

## 一句话职责

把「一次运行树」变成可消费的数据——要么在一棵树的终点整份提交（`BaseTracer` 的持久化路径），要么在运行过程中把每一步变成一条可增量订阅的事件（`_AstreamEventsCallbackHandler` 的流式路径）；而 `tracing_v2_enabled` 让「此刻要不要追踪」成为一个上下文变量而不是一个参数。

## 机制是什么

三层，各自的位置很不同：

| 层 | 文件 | 职责 |
|---|---|---|
| 核心数据结构与生命周期 | `tracers/core.py` 的 `_TracerCore` | 维护 `run_map`、`order_map`、父子挂接、dotted order |
| 同步/异步回调实现 | `tracers/base.py` 的 `BaseTracer` / `AsyncBaseTracer` | 把十几个 `on_*` 事件翻译成 `Run` 的创建与收尾 |
| 事件流 | `tracers/event_stream.py` 的 `_AstreamEventsCallbackHandler` | 把同一批事件变成可迭代的 `StreamEvent` |
| 开关 | `tracers/context.py` 的 `tracing_v2_enabled` | 用 ContextVar 表达「这次要不要追踪」 |

`_TracerCore` 抽象基类本身没有 `on_*` 方法——它只提供一个 **run 注册表**。`BaseTracer` 才把它接到回调体系上。这个分层让「事件流的实现」可以绕过 `BaseTracer` 而共用注册表逻辑。

## 怎么实现的

### 一、`_TracerCore`：两个映射与一个点分序号

**它持有三个映射，各自的清空时机都写在了属性注释里。**

| 映射 | 键 → 值 | 何时清空 |
|---|---|---|
| `run_map` | run id（字符串）→ `Run` | **运行结束时清** |
| `order_map` | run id（`UUID`）→ `(trace_id, dotted_order)` | **tracer 被 GC 时清** |
| `_external_run_ids` | 外部注入的 run id → 活跃子运行计数 | **最后一个子运行结束时清** |

注释把三个生命周期明明白白写出来，而这正是这个类最容易被误读的地方：**为什么有两份 id 映射而不是一份？** 因为它们服务两类查询——`run_map` 用来「按 id 找那个 run 对象」（运行结束后不再需要），`order_map` 用来「按父 id 找它的 trace 归属」（父运行结束后它的子运行可能还在跑，所以必须留到 GC）。

**`_start_trace` 是全篇的核心，它负责给每个 run 算 trace_id 与 dotted_order。**

```
current_dotted_order = run.start_time.strftime("%Y%m%dT%H%M%S%fZ") + str(run.id)
```

也就是说一个 run 的「自身序号片段」是 **微秒精度的时间戳 + 自己的 id**。时间在前、id 在后，于是同一时刻的多个 run 也能按 id 排出确定顺序。

然后分三种情形：

1. **有父且父在 `order_map` 里** → 继承父的 `trace_id`，`dotted_order = 父的 + "." + 自己的片段`，并在 `run_map` 里找到父对象后 `_add_child_run` 挂上去。
2. **有父但父不在 `order_map` 里** → **把 `parent_run_id` 置 `None`，当成根运行**，并 `logger.debug` 记录「Parent run ... not found ... Treating as a root run」。这个行为**可关**：类属性 `log_missing_parent: bool = True`。
3. **无父** → 自己就是根，`trace_id = run.id`。

第 2 条是这套机制里最重要的一个降级：**父丢失不是错误，而是「当成新的根」。** 注释（日志文案）把这件事说清楚了，而 `log_missing_parent` 这个开关说明在某些场景下这种降级会很常见（例如跨进程续接），日志太吵。

**`_external_run_ids` 的用途写在属性注释里，且解释了一个真实边界。** 原文：这些 run 被加进 `run_map` **只是为了让子运行能找到它们的父**，但它们**不被 tracer 的回调生命周期管理**；当最后一个子运行结束时该条目被驱逐，**以避免内存泄漏**。

这就是引用计数存在的理由：一个 run 的开始与结束由外部（例如 LangSmith 的 `@traceable`）驱动，tracer 永远收不到它的 `on_*_end`。若把这个条目永远留在 `run_map` 里，长跑进程就会持续泄漏。

而计数放在哪一侧？**在父上按子数量计数**：

```
if parent_key in self._external_run_ids:
    self._external_run_ids[parent_key] += 1
```

子运行开始时给父的计数 +1，结束时 -1，归零就删。这是「**用下游的活跃数量推断上游的存活期**」的标准做法——因为上游自己不会通知。

**`_get_run` 用一个函数同时做两件事：查找与类型校验。**

```
run = self.run_map[str(run_id)]        # 找不到 → TracerException("No indexed run ID")
if run_types is not None and run.run_type not in run_types: raise TracerException(...)
```

`run_type` 参数接受 `str` 或 `set[str]`，于是「期望是 chain 或 tool」这种情形也能表达。两条错误消息都带着实际的 id 与类型——这在追踪系统里很重要，因为错误发生时你手上只有 id。

**`__deepcopy__` 与 `__copy__` 都直接返回 `self`。** 这不是偷懒，而是必需：tracer 持有 `run_map` 这类共享状态，深拷贝会产生两份互不相见的状态，导致「事件进了一个副本、查询打在另一个副本」。回调体系确实会 copy handler（例如 `get_child` 里 copy handler 列表），所以这条必须成立。

### 二、`BaseTracer` / `AsyncBaseTracer`：一次性提交与逐事件通知

**`BaseTracer` 只额外要求实现 `_persist_run`，其余全是事件翻译。**

`_start_trace` 与 `_end_trace` 被覆盖成「先调父类，再触发钩子」：

```
def _start_trace(self, run): super()._start_trace(run); self._on_run_create(run)
def _end_trace(self, run):
    if not run.parent_run_id: self._persist_run(run)     # 只有根运行被持久化
    self.run_map.pop(str(run.id))
    ...外部父引用计数递减...
    self._on_run_update(run)
```

**「只有根运行才 `_persist_run`」这一句定义了 `BaseTracer` 的整个语义**：一棵运行树在终点被整份提交。而子运行的结束只是更新内部状态。

这与 `_AstreamEventsCallbackHandler` 完全相反的取向：后者每个事件的 `_persist_run` 是空实现（`pass`，注释说「这是为整棵运行树只调用一次的 legacy 方法，在这里没用」），因为它靠的是逐事件发送而不是终局提交。

**同一套基类被两种截然不同的消费方式复用，靠的就是 `_on_run_create` / `_on_run_update` 这两个钩子。** 它们默认是 `return None` 的空操作（在 core.py 里），子类覆盖即可。

**`_on_llm_new_token` 这条路径有一个单独的钩子，因为 token 不是「一次事件」。** core 里有 `_on_llm_new_token`，`_StreamingCallbackHandler` 是另一个 mixin 用来「拦截输出迭代器」。`LogStreamCallbackHandler` 继承两者，把 token 追加进 `streamed_output_str`。这就是「流式输出的文字」与「流式输出的结构化事件」两种消费方式的差别所在。

**`AsyncBaseTracer` 是逐方法的镜像，而不是共用。** 它继承 `AsyncCallbackHandler`，每个 `on_*` 都写成 `async def`，每个 `_on_*` 钩子都是 `Coroutine | None`。这份镜像看起来是重复，但它换来的是**同步追踪器不必写 `async`、异步追踪器不必写同步桩**——而 `_TracerCore` 的钩子签名是 `Coroutine | None`，两种都能返回。

### 三、`_AstreamEventsCallbackHandler`：把事件变成流

**它维护两个映射而不是一个，理由写在代码注释里。**

```
self.run_map: dict[UUID, RunInfo] = {}          # run id → 运行信息
self.parent_map: dict[UUID, UUID | None] = {}   # run id → 父 id
```

注释原文大意：**父运行的结束回调可能比子运行的结束回调更早被调用**，那会导致 `run_map` 被清理；所以父子关系放在另一个容器里，**它随 tracer 被 GC 才清**。

也就是说和 `_TracerCore` 的 `order_map` 是同一个问题：**结束事件的到达顺序不保证与嵌套结构一致**。若只留一份 map 并在结束时删除，那么晚到的子运行就找不到自己的父了。

**`RunInfo` 是一个 `TypedDict`，字段分离得比其它内部类型更细。**

必填：`name`、`tags`、`metadata`、`run_type`、`parent_run_id`。
可选（`NotRequired`）：`inputs`、`tool_call_id`。

`tool_call_id` 单独存在的理由是注释里的一句话：**存 `tool_call_id` 以便把错误关联回具体的工具调用**。这是流式消费者最需要的一条信息——拿到一个 `on_tool_error` 事件时，它要知道「这是哪一次工具调用失败了」，而 id 只能定位 run，对不上模型视角里的工具调用编号。

**`_write_run_start_info` 对 `inputs` 有一个绕过 `TypedDict` 惯例的处理。**

```
if "inputs" in kwargs:
    info["inputs"] = kwargs["inputs"]
```

注释解释：**特判 `inputs` 是为了区分「没提供」与「提供了 `None`」**。

这是一个精确的三态问题：`TypedDict` 上写 `NotRequired` 意味着「键可能不存在」，但如果代码写成 `info["inputs"] = kwargs.get("inputs")`，那「没提供」会变成「键存在、值是 `None`」——对消费者来说这是两种完全不同的语义（前者是「这个事件类型没有 input」，后者是「input 就是 null」）。

用 `in kwargs` 而不是 `kwargs.get`，是这类问题唯一正确的判法。

**`_assign_name` 是一个三级回退，与 `FileCallbackHandler` 里的链名解析同源。**

```
name 参数 → serialized["name"] → serialized["id"][-1] → "Unnamed"
```

最后兜底 `"Unnamed"` 而不是空串，是因为这个 `name` 会成为事件里的 `name` 字段与流里的键。

**`_get_parent_ids` 里有一个「这不该发生」的断言。**

```
while parent_id := self.parent_map.get(run_id):
    str_parent_id = str(parent_id)
    if str_parent_id in parent_ids:
        raise AssertionError(f"Parent ID {parent_id} is already in the parent_ids list. This should never happen.")
    parent_ids.append(str_parent_id)
    run_id = parent_id
return parent_ids[::-1]
```

逐个向父走，若发现同一个 id 出现两次就抛 `AssertionError`——因为那意味着**父子关系成环**。这个循环若是无限的在环上转，会把进程挂死；所以用断言把它变成一个立即失败的错误。

而返回前 `[::-1]` 让顺序变成「根在最前、直接父在最后」。这个顺序不是随意：事件里 `parent_ids` 的语义就是「从根到父」，消费者按这个顺序拼接层级。

**`_send` 经过 `_RootEventFilter` 才入流。**

```
if self.root_event_filter.include_event(event, event_type):
    self.send_stream.send_nowait(event)
```

而 filter 的六个参数在构造函数里接收：`include_names` / `include_types` / `include_tags` 与对应的 `exclude_*`。于是「只要某些 Runnable 的事件」在**发送前**就被过滤掉，而不是让消费者自己丢。

这个位置的选择是有意义的：**过滤发生在写入流的那一刻**，于是流本身只承载消费者关心的东西。代价是过滤条件必须在构造 tracer 时给全，无法在消费过程中改。

### 四、`tracing_v2_enabled`：让「要不要追踪」成为上下文

**它是一个 `@contextmanager`，做的事情只有三行。**

```
cb = LangChainTracer(example_id=..., project_name=..., address=..., tags=..., client=...)
token = tracing_v2_callback_var.set(cb)
try: yield cb
finally: tracing_v2_callback_var.reset(token)
```

**`token` 与 `reset(token)` 的组合是关键。** 用 `set` 返回的 token 复位，而不是 `set(None)`——后者会在嵌套使用时把外层的 tracer 一起清掉。于是：

```
with tracing_v2_enabled():        # 外层，token_A
    with tracing_v2_enabled():    # 内层，token_B
        ...
    # 内层退出 → 复位到 token_B 之前，即外层那个 tracer
# 外层退出 → 复位到没有 tracer
```

这个正确性只靠 token 就能保证，不需要任何栈结构。

**它之所以能生效，是因为 `_configure` 会读这个变量。** 回调管理器那页里 `_configure` 的第四段正是：

```
tracer_v2 = tracing_v2_callback_var.get()
if tracing_v2_enabled_ and not any(isinstance(handler, LangChainTracer) for handler in callback_manager.handlers):
    callback_manager.add_handler(tracer_v2 or LangChainTracer(...))
```

也就是说**这个上下文变量不是被读一次，而是在每次配置回调管理器时被读**。于是进入上下文之后创建的每个运行都自动带上 tracer，而退出之后创建的运行一个都不带。

这条「配置时读取」的路径也解释了 `_tracing_v2_is_enabled` 的实现顺序：**先看 contextvar，再看环境变量**。

```
if tracing_v2_callback_var.get() is not None: return True
return ls_utils.tracing_is_enabled()
```

contextvar 优先，意味着一个显式的 `with tracing_v2_enabled()` **可以压过环境里没开追踪的状态**。反过来，若环境开了而代码里没有 with 块，环境那一侧也能生效。两个方向都给到了。

**`(example_id, project_name, client, address, tags)` 五个参数各自透传给 `LangChainTracer`。** 而 `address` 的文档带 `!!! warning "Beta"` —— 「agent addressing 必须在 LangSmith 工作区启用」。这是一个「参数已存在但依赖服务端开关」的情形，标注比隐藏好。

**`_get_tracer_kwargs` 处理了一个容易搞错的细节：project 与 address 必须一起传。**

文档注释：**把 project 与 address 一起转发，这样被寻址的父运行永远不会捡到环境里的 project；把环境默认与校验留给 `RunTree`。**

逻辑是：若只给了 `project_name` 或只给了 `address`，就不从父运行继承——因为从父继承会把「父的 project」与「显式给的 address」混成一个不存在的组合。而当两个都没给时，才按「父 → 上下文」的顺序取。

**`register_configure_hook` 是这个扩展点唯一的注册入口，而且它自己带一个校验。**

```
if env_var is not None and handle_class is None: raise ValueError("If env_var is set, handle_class must also be set to a non-None value.")
_configure_hooks.append((cast(...), inheritable, handle_class, env_var))
```

那个校验的理由是：一个「按环境变量启用」的钩子必须知道要启用**哪个** handler 类，否则收到环境变量也无从构造。把这条放在注册入口，比在每个消费点写防御好。

而 `cast` 那一行的注释解释了类型系统的局限：`ContextVar` 的泛型参数不是协变的，所以 `ContextVar[LangChainTracer | None]` 不能直接塞进 `ContextVar[BaseCallbackHandler | None]` 的位置——只能用 `cast` 绕。这是一个**由语言本身的限制导致的强制转换**，注释写明原因。

**`run_collector_var` 是这条注册路径的实例**（文件末尾就有一句 `register_configure_hook(run_collector_var, inheritable=False)`），而 `inheritable=False` 意味着它不会传播进子上下文——与 `usage` 那个 `inheritable=True` 恰好相反。这个对比值得记：**「用量统计要覆盖整棵树」所以继承，「收集本次调用的顶层 run」所以不继承。**

### 五、事件流怎么被驱动

**`_AstreamEventsCallbackHandler` 不是被动被调用的，它主动驱动一个 Runnable 并拦截其输出。**

`tap_output_aiter(run_id, output)` 的职责是：拿一个异步迭代器，**在每次产出时既发事件又原样转发**。它的存在让「事件流」与「输出流」不是两条路径——一个 Runnable 的 `astream` 输出被同一次迭代同时用于两件事。

而 `consume_astream` 那段把整台机器串起来：

```
async with aclosing(runnable.astream(value, config, **kwargs)) as stream:
    async for _ in event_streamer.tap_output_aiter(run_id, stream): pass
finally:
    await event_streamer.send_stream.aclose()
```

三个要点：

1. **消费方的 `async for event in event_streamer` 与这个是并行的**——一个 task 跑 Runnable，一个 task 从流里读。
2. **`aclosing` 保证无论怎么退出都关闭底层异步迭代器。**
3. **`finally` 里关闭 `send_stream`**，于是消费方那边的 `async for` 会正常结束而不是永远等。

**第一个事件有一个专门的修补。** 代码注释：

> 这是绕过「链的 inputs 在整个输入被消费完之前不可用」这个问题的工作区。作为临时方案，我们把 input 改成传入链的那个 `value`。

也就是说根运行的 input 从事件里拿不到（因为模型/链要吃掉整个输入才知道它是什么），所以直接把调用方给的值塞进第一个事件的 `data["input"]`。而根运行的 `_end` 事件会**删掉 `input`**，因为它已经在第一个事件里给过了——避免同一个 input 在流里出现两次。

`event["run_id"] == first_event_run_id and event["event"].endswith("_end")` 这个判定把「根运行的结束」认出来。

**取消路径上有一条 `task.cancel()` 加 `contextlib.suppress(asyncio.CancelledError)` 的组合。** 前者保证 Runnable 不再跑，后者保证**在 await 它时不因取消异常而打断清理**——注释写着「无论如何都 await 它，以运行清理代码并传播异常」。

这个组合是异步资源管理的标准形态：**取消一个 task 之后仍然要 await 它，否则它的 `finally` 可能没机会跑。**

## 关键取舍

**源码事实：** `_TracerCore` 用三份映射（`run_map` / `order_map` / `_external_run_ids`），各有不同清空时机；`run_map` 随运行结束清，`order_map` 随 GC 清。

**源码事实：** dotted order 的自身片段是「微秒时间戳 + 自己的 id」，于是同一时刻的多个 run 也有确定顺序。

**源码事实：** 父运行在 `order_map` 里找不到时**把父 id 置 `None`、当成根运行**，只记一条 `debug` 日志，且该日志可用 `log_missing_parent` 关闭。

**源码事实：** `_external_run_ids` 是对「外部注入的 run」的引用计数，因为它们的开始与结束都不经 tracer；计数归零即驱逐以避免泄漏。

**源码事实：** `BaseTracer` **只在根运行的结束处调用 `_persist_run`**——即「一棵树整份提交」；事件流 handler 的 `_persist_run` 是空的 `pass`。

**源码事实：** `__deepcopy__` / `__copy__` 都返回 `self`，因为 tracer 的共享状态不能有第二份。

**源码事实：** `_AstreamEventsCallbackHandler` 把 run 信息与父子关系分成两个映射，因为**父的结束回调可能早于子**。

**源码事实：** `RunInfo.inputs` 用 `if "inputs" in kwargs` 而不是 `kwargs.get`，为了区分「没提供」与「提供了 `None`」。

**源码事实：** `_get_parent_ids` 用 `AssertionError` 检测成环，并返回「根在最前」的顺序。

**源码事实：** 事件在 `_send` 处就过 `_RootEventFilter`，于是流本身只承载关心的内容；代价是过滤条件只能在构造时给。

**源码事实：** `tracing_v2_enabled` 用 `token` + `reset(token)` 而不是 `set(None)`，于是嵌套使用能正确回退。

**源码事实：** `_tracing_v2_is_enabled` 先看 contextvar 再看环境变量，于是显式 with 块能压过环境未开启的状态。

**源码事实：** `_get_tracer_kwargs` 只在 project 与 address 都没显式给出时才从父运行继承，避免「父的 project + 显式 address」混成不存在的组合。

**源码事实：** `register_configure_hook` 在注册处校验「有 `env_var` 就必须有 `handle_class`」，并用 `cast` 绕过 `ContextVar` 泛型不协变的限制（注释写明了原因）。

**架构推论：** 这一整套的重心不在「记录事件」，而在**「事件到达的顺序不等于结构顺序」这件事实**。结束回调可能早于子的结束、父可能根本不在（外部注入）、根运行的 input 可能晚于第一个事件才可用——`parent_map`、`_external_run_ids`、第一个事件的 input 修补，三条都是针对这个事实的不同应对。

换句话说：追踪系统的复杂度几乎全部来自**回调流是事件流、而它要还原的是一棵树**。这两者的结构不一致，是所有「实时追踪」系统的共同难题。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/tracers/core.py` | `_TracerCore` | run 注册表与生命周期，三份映射 |
| `libs/core/langchain_core/tracers/core.py` | `_TracerCore._start_trace` | 算 trace_id 与 dotted_order，父子挂接，父丢失时当根 |
| `libs/core/langchain_core/tracers/core.py` | `_TracerCore._get_run` | 按 id 查找并校验 run_type，错误带 id 与类型 |
| `libs/core/langchain_core/tracers/core.py` | `_TracerCore.__deepcopy__` / `__copy__` | 都返回 `self`，共享状态不可复制 |
| `libs/core/langchain_core/tracers/base.py` | `BaseTracer` | 同步回调实现，只在根结束处 `_persist_run` |
| `libs/core/langchain_core/tracers/base.py` | `BaseTracer._end_trace` | 持久化根、清 map、外部父计数递减、触发 `_on_run_update` |
| `libs/core/langchain_core/tracers/base.py` | `AsyncBaseTracer` | 逐方法镜像的异步版本 |
| `libs/core/langchain_core/tracers/event_stream.py` | `RunInfo` | 每个 run 的流式元信息，含可选的 `tool_call_id` |
| `libs/core/langchain_core/tracers/event_stream.py` | `_AstreamEventsCallbackHandler` | 事件流 handler：两个映射 + 根事件过滤器 |
| `libs/core/langchain_core/tracers/event_stream.py` | `_write_run_start_info` | 特判 `inputs` 以区分「未提供」与 `None` |
| `libs/core/langchain_core/tracers/event_stream.py` | `_get_parent_ids` | 向上走并断言不成环，返回根在前 |
| `libs/core/langchain_core/tracers/event_stream.py` | `tap_output_aiter` | 把输出迭代器同时用于发事件与转发 |
| `libs/core/langchain_core/tracers/context.py` | `tracing_v2_enabled` | 上下文管理器，token 复位支持嵌套 |
| `libs/core/langchain_core/tracers/context.py` | `_tracing_v2_is_enabled` | contextvar 优先于环境变量 |
| `libs/core/langchain_core/tracers/context.py` | `_get_tracer_kwargs` | project 与 address 必须成对继承 |
| `libs/core/langchain_core/tracers/context.py` | `register_configure_hook` | 扩展点注册，含「env_var 需配 handle_class」校验 |
| `libs/core/langchain_core/tracers/log_stream.py` | `LogStreamCallbackHandler` | 把事件变成 JSON-patch 形态的 `RunLogPatch` |

## 参考

- [langchain 源码：tracers/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/tracers/base.py)
- [langchain 源码：tracers/event_stream.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/tracers/event_stream.py)
- [langchain 文档：astream_events](https://python.langchain.com/docs/how_to/streaming/#using-stream-events)
- [RFC 6902：JSON Patch](https://www.rfc-editor.org/rfc/rfc6902)

## 与主核 / AI 的关系

追踪器的「事件流还原成运行树」与主核里的**执行计划到运行时状态的映射**同构：数据库的 `EXPLAIN ANALYZE` 也是从一串算子级事件（开始、结束、行数、耗时）重建一棵树，而事件顺序同样不保证与树结构一致（并行算子会让两个分支交错到达）。

对 agent 工程的直接价值有三条。其一，**「事件顺序不等于结构顺序」要有成对的应对**：一份即时可删的映射用于运行期查找，一份留到 GC 的映射用于父子追溯——只留一份就必然在某个晚到事件上出错。其二，**外部注入的节点要引用计数**。它的开始与结束都不经过你，你只能靠「下游还剩几个活跃」推断上游何时可回收；不留计数就是内存泄漏。其三，**「没提供」与「提供了空值」必须分开表达**。`if key in kwargs` 与 `kwargs.get(key)` 在这一点上是两种语义，而追踪数据里 `input: null` 与「没有 input 字段」对消费者完全不是一回事。
