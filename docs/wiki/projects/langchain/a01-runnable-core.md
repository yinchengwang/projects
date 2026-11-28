---
project: langchain
feature: runnable-core
title: Runnable 契约与序列组合
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/runnables/base.py
    symbols: [Runnable, RunnableSerializable, RunnableSequence]
  - path: libs/core/langchain_core/runnables/base.py
    symbols: [RunnableLambda, RunnableGenerator, _ChunkAccumulator]
---

# Runnable 契约与序列组合

## 一句话职责

`Runnable` 用九组「同步 / 异步」成对方法把「一个可调用的处理步骤」标准化成一个管道零件——同步版本必须实现，其余全部由基类给出可用默认；`RunnableSequence` 是 `|` 运算符的落地形态，它把嵌套的组合摊平成一条线性步骤表，并保证任何一步失败时异常原样往外抛。

## 机制是什么

这套东西的核心不是「链」，而是**一份契约**。契约的内容很具体：任何一个 `Runnable`，只要实现了 `invoke`，就自动获得了异步、批量、批量流式、单条流式、流式转换这八种能力——因为基类为它们都提供了**有实际意义的默认实现**，而不是抛 `NotImplementedError`。

这个设计的价值在组合处显形：`A | B | C` 里任意一段换成第三方实现、换成 lambda、换成生成器函数，整条链的九种调用方式都还能用。否则「链」这个概念就只是把几个函数串起来，串完只剩一种调用方式。

契约的三对半：

| 同步 | 异步 | 默认实现从哪来 |
|---|---|---|
| `invoke` | `ainvoke` | `ainvoke` 默认把 `invoke` 丢进线程池 |
| `batch` | `abatch` | 默认用线程池并发跑 `invoke` |
| `stream` | `astream` | 默认把单条输入包成迭代器交给 `transform` |
| `transform` | `atransform` | 真正实现流式的钩子，序列靠它保持流式 |

注意最后那一对不是「第五种调用方式」，而是**流式能力的真实落点**：`stream` 是 `transform` 的特例（把单个输入变成一个只含一项的迭代器），所以一个步骤只要实现 `transform`，`stream` 就自动正确。这也解释了为什么 `RunnableSequence` 的流式能力取决于「每一步是否实现了 `transform`」——lambda 默认不实现，于是放在链中间就会把流切断。

再往上一层是 `RunnableSerializable`：它是 `Serializable` 与 `Runnable` 的交叉，于是可组合的管道同时可以是 pydantic 模型、可以被序列化成 JSON 再反序列化回来。整个框架里绝大多数具名组合器（序列、并行、分支、绑定、重试、配置化）都继承它，只有少数纯行为型的（`RunnableLambda`、`RunnableGenerator`）不继承。

## 怎么实现的

**九组方法的默认实现全是「降级」而不是「报错」。**

`invoke` 是唯一的抽象方法：

```
@abstractmethod
def invoke(self, input, config=None, **kwargs) -> Output: ...
```

`ainvoke` 的默认体只有一行：

```
return await run_in_executor(config, self.invoke, input, config, **kwargs)
```

也就是说**同步实现优先**，异步是它的线程池包装。这个方向与直觉相反（很多框架要求先写异步），但它是有理由的选型：绝大多数第三方库（向量库客户端、本地模型）是同步的，若契约以异步为主，每个接入方都得自己造一层 `run_in_executor`。反方向的代价是真正的异步实现会丢失——所以真正异步的子类仍然要覆盖 `ainvoke`，默认实现只是「能跑」的下限。

`batch` 的默认实现同样务实，注释直接写明「默认实现用线程池并发跑 `invoke`，对 IO 密集的 `Runnable` 表现良好；若底层有真正的批量接口，子类应当覆盖它」：

```
if not inputs:
    return []
configs = get_config_list(config, len(inputs))
if len(inputs) == 1:
    return [invoke(inputs[0], configs[0])]
with get_executor_for_config(configs[0]) as executor:
    return list(executor.map(invoke, inputs, configs))
```

三处细节都是刻意的：**空输入直接返回空表**（不构造线程池）；**单条输入不构造线程池**（注释「只有一个输入就不必麻烦执行器了」）；**执行器从 `configs[0]` 拿**（`max_concurrency` 是全局配置，取第一条即可）。

`stream` 的默认实现是 `transform` 的退化调用，`transform` 的默认实现则把整条输入流攒起来再一次性 `invoke`。于是「不支持流式」不是错误，而是「流式会退化成阻塞」——调用方拿到的是一个只产出一个块的迭代器。

**`_ChunkAccumulator` 是这套契约里唯一一处为了性能而复杂的地方。**

它的存在理由是流式跑完时**必须重新拼出完整值**（用于回调、用于 `invoke`、用于错误上报）。最笨的做法是 `acc = acc + chunk` 逐个累加，而代码注释说清了为什么不能这样：

> 前导的 `AIMessageChunk` 先攒着，等读值时一次合并：每次 `AIMessageChunk` 相加都会重新解析迄今为止收到的工具调用参数，所以逐个相加在工具调用长度上是平方级的。若那次相加抛出异常，再退回逐个相加。

于是实现分两轨：一连串 `AIMessageChunk` 进 `_message_chunks` 列表，读 `value` 时用 `chunks[0] + chunks[1:]` 一次加完；若这次批量加抛出 `TypeError`，退回逐个 `_add`。

对于非 `AIMessageChunk` 的块则直接 `_add`，其语义是：

```
if self._value is None or not self._addable:
    self._value = chunk
    return
try:
    self._value = self._value + chunk
except TypeError:
    self._value = chunk
    self._addable = not self._stop_after_unaddable
```

**「加不动就替换」**是这里的核心语义：两个块类型不同（`str` 与 `dict`、`dict` 与 `Message`）时不做类型体操，直接用新块覆盖旧值。再加一个开关 `stop_after_unaddable`：一旦发生过替换，后续每个块都替换。两个取值对应两种场合——`stop_after_unaddable=False` 用于**回调上报**（尽量拼出最有代表性的值，哪怕混混搭搭），`True` 用于 `_transform_stream_with_config` 里（一旦拼接失败，后续拼接没有意义，越拼越错）。

`value_for_error()` 是给错误路径专门准备的：

```
try:
    return self.value
except Exception:
    return self._value
```

注释写明「给错误处理器用，这样一个失败的合并不至于把正在上报的错误替换掉」。这类细节说明作者在意的是「错误路径本身不能再出错误」——`_flush` 里的批量合并会抛，若它抛在 `on_chain_error` 的实参求值上，调用方根本看不到真正的原始错误。

**`RunnableSequence` 的三段式结构是为类型检查服务的，不是为语义服务的。**

```
first: Runnable[Input, Any]
middle: list[Runnable[Any, Any]] = Field(default_factory=list)
last: Runnable[Any, Output]
```

字段上方的注释直说：「步骤被拆成 first、middle、last，纯粹是为了类型检查——这样可以把 `Input` 标在第一个上、`Output` 标在最后一个上」。`steps` 属性再拼回列表。这个取舍在 Python 的类型系统里是必要的：一个 `list[Runnable[Any, Any]]` 无法表达「第一个吃 Input、最后一个吐 Output」。

**摊平发生在构造期，所以组合不会随写法的括号深度长成树。**

```
for step in steps:
    if isinstance(step, RunnableSequence):
        steps_flat.extend(step.steps)
    else:
        steps_flat.append(coerce_to_runnable(step))
if len(steps_flat) < _RUNNABLE_SEQUENCE_MIN_STEPS:
    raise ValueError(...)
```

`__or__` 也做同样的事：右操作数是序列时，把两边的 first/middle/last 拼成一个新序列。于是 `a | b | c` 与 `a | (b | c)` 得到**同一个扁平结构**，而不是两种嵌套形态。这对 `get_graph`、`config_specs` 与错误的 `seq:step:i` 编号都重要——编号基于扁平下标，嵌套的话编号会变成路径。

`coerce_to_runnable` 让 `int | f | g` 这类写法成立：右侧的裸函数被包成 `RunnableLambda`。

**逐步执行时的三件事：子 run、配置上下文、kwargs 只给第一步。**

```
for i, step in enumerate(self.steps):
    config = patch_config(config, callbacks=run_manager.get_child(f"seq:step:{i + 1}"))
    with set_config_context(config) as context:
        if i == 0:
            input_ = context.run(step.invoke, input_, config, **kwargs)
        else:
            input_ = context.run(step.invoke, input_, config)
```

三点：

1. **`run_manager.get_child(f"seq:step:{i+1}")`** 让每一步在 trace 里成为根 run 的子节点，且标签带序号。这是「一条链在观测面板上展开成 N 段」的实现方式。
2. **`set_config_context` + `context.run`** 把配置放进 `ContextVar`，于是下游不用显式传 config 也能读到（这是 a05 的主题）。
3. **`**kwargs` 只传给第一步**。这是个容易忽略但影响很大的约定：`chain.invoke(x, temperature=0.3)` 里的温度只作用于链首——因为链首通常是模型。后续步骤若也收到，`temperature` 会漏给解析器之类的组件，变成 `TypeError`。

**错误传播有两套策略，取决于 `return_exceptions`。**

`invoke` 走的是最直接的一套：

```
try:
    for i, step in enumerate(self.steps): ...
except BaseException as e:
    run_manager.on_chain_error(e)
    raise
else:
    run_manager.on_chain_end(input_)
    return cast("Output", input_)
```

异常原样 `raise`，只额外发一次错误回调。于是**链的中段抛错时，调用方拿到的是那一步抛出的原始异常**，不是被包装成「第 3 步失败」之类。这个选择让上游可以直接 `except ValueError` 而不必按链的结构去拆包装。

`batch` 则有两套：

- **`return_exceptions=False`（默认）** 时，任何一条输入在任一步失败，整批都失败。但代码里有两处细节值得看：失败时给**每个** run manager 都发了 `on_chain_error`（`for rm in run_managers: rm.on_chain_error(e)`），而 `return_exceptions` 为真时返回的是 `[e for _ in inputs]`——**同一个异常对象重复 len(inputs) 次**。这是刻意的：调用方拿到的列表与输入等长，失败项的占位是异常实例，成功项没有。用同一个对象而不是复制，是因为异常本来就不是可复制的值。
- **`return_exceptions=True`** 时走「按索引隔离」的路径：维护一个 `failed_inputs_map`（原下标 → 异常），每一轮只把**还没失败**的输入子集交给下一步，并把成功结果按下标写回；若全部输入都失败就提前 `break`。最后按原下标重排输出，失败位置填异常实例。

第二套路径的价值在于：一条输入在中段失败不会连累其他输入跑到最后一步，也不可能「把失败值当成输入传给下一步」。第一套则是简单直接——反正整批要失败，没必要精细区分。

**流式路径的全貌由 `_transform_stream_with_config` 一处统一实现。**

它是所有 `stream` / `transform` 的实际执行体，做四件事：

```
input_for_tracing, input_for_transform = tee(inputs, 2)
final_input = _ChunkAccumulator(stop_after_unaddable=True)
final_input.add(next(input_for_tracing, None))
final_output = _ChunkAccumulator(stop_after_unaddable=True)
```

1. **`tee` 分两路**：一路给真正的转换器，一路留给「跑完之后拼完整输入」用。
2. **先拉一个输入块再启动回调**。注释是「启动输入迭代器以确保上游 `Runnable` 早于本步开始」——这一步让 trace 的顺序与真实执行顺序一致，否则回调里的 `on_chain_start` 会在上游启动之前发出。
3. **转换器可能接受 config / run manager**，通过 `accepts_config` 与 `accepts_run_manager` 两个签名探测自动注入 kwargs（`kwargs["config"] = child_config`）。这是「让用户写的转换函数可以只要一个参数、也可以顺手接 config」的实现。
4. **流式回调通过 `tap_output_iter` 挂进输出迭代器**：找到第一个 `_StreamingCallbackHandler` 实例，把它包一层，于是 `astream_log` 之类的处理器能看到每个块。

然后是一个容易看漏但很关键的顺序：

```
while True:
    chunk = context.run(next, iterator)
    yield chunk
    final_output.add(chunk)
except (StopIteration, GeneratorExit):
    pass
for ichunk in input_for_tracing:
    final_input.add(ichunk)
merged_input = final_input.value
merged_output = final_output.value
```

**先把两个累积值读完（`value` 会触发合并且可能抛），再 `on_chain_end`。** 源码注释写得明白：「读值会触发合并，而合并可能抛；在这里读，这样这次 run 仍然会结束，先读 inputs 以便错误路径能拿到它们」。若把 `value` 写到 `on_chain_end(...)` 的实参里，一次合并异常会让这次 run 既不结束也不报错。

错误路径对应地长这样：

```
except BaseException as e:
    run_manager.on_chain_error(e, inputs=final_input.value_for_error())
    raise
```

用 `value_for_error()` 而不是 `value` —— 与前面那段一脉相承。

**`RunnableGenerator` 与 `RunnableLambda` 是「显式声明能力边界」的两个例子。**

`RunnableGenerator` 的 `transform` 在开头做能力检查：

```
if not hasattr(self, "_transform"):
    raise NotImplementedError(f"{self!r} only supports async methods.")
```

也就是说：只给了异步生成器函数的实例，调同步路径会拿到一条指明原因的 `NotImplementedError`，而不是 `AttributeError` 或静默失败。反向的 `atransform` 检查 `_atransform` 存在性，消息是「只支持同步方法」。

它的 `invoke` 是「把流拼起来」：

```
final = None
for output in self.stream(input, config, **kwargs):
    final = output if final is None else final + output
```

注意这里没有用 `_ChunkAccumulator` —— 因为它不需要「加不动就替换」的宽容语义：一个生成器应当产出可加的同质块，加不动就是真的错了。

`RunnableLambda` 的能力边界在 `invoke` 开头：

```
if hasattr(self, "func"):
    return self._call_with_config(self._invoke, input, ensure_config(config), **kwargs)
msg = "Cannot invoke a coroutine function synchronously. Use `ainvoke` instead."
raise TypeError(msg)
```

`func` 只在同步函数时存在（构造期把协程函数塞进 `afunc`）。于是把一个 `async def` 包成 `RunnableLambda` 之后，误调 `invoke` 得到的是明确的 `TypeError` 与替代方案提示，而不是在线程池里生成一个未被 await 的协程对象——那会静默返回垃圾值并附带一条 `RuntimeWarning`。

## 关键取舍

**源码事实：** 九个公开方法里只有 `invoke` 是抽象方法；`ainvoke` 默认用 `run_in_executor` 包装 `invoke`，`batch` 默认用线程池并发跑 `invoke`，`stream` 默认把单条输入包成迭代器交给 `transform`。

**源码事实：** `batch` 对空输入返回空表、对单条输入跳过线程池、执行器取 `configs[0]` 的 `max_concurrency`。

**源码事实：** `_ChunkAccumulator` 把连续 `AIMessageChunk` 攒起来、读值时一次相加，理由是每个 `AIMessageChunk` 相加都会重解析工具调用参数，逐个相加是平方级；批量相加抛 `TypeError` 时退回逐个相加。

**源码事实：** 累加器对加不动的块采取「替换」语义；`stop_after_unaddable` 决定替换后是否放弃后续拼接（回调上报用 `False`，`_transform_stream_with_config` 用 `True`）；`value_for_error()` 保证错误上报期间不会因二次合并失败而丢掉原始异常。

**源码事实：** `RunnableSequence` 的 `first` / `middle` / `last` 拆分只为类型检查；构造期摊平嵌套序列，`__or__` 拼接时也摊平，故 `a | b | c` 与 `a | (b | c)` 结构相同；少于两步直接 `ValueError`。

**源码事实：** 逐步执行时每步以 `seq:step:{i+1}` 作为子 run 标签，`**kwargs` 只传给第一步。

**源码事实：** 异常在 `invoke` 里原样 `raise`（仅附带一次错误回调）；`batch` 在 `return_exceptions=False` 时用「整批失败」策略并以同一异常对象填满输出列表，在 `True` 时用「按原下标隔离」策略、每一轮只对未失败子集继续执行。

**源码事实：** `_transform_stream_with_config` 先 `tee` 输入、先拉一个块再启动回调、把 `value` 的读取放在 `on_chain_end` 之前，并在错误路径用 `value_for_error()`。

**源码事实：** `RunnableGenerator` 在缺失 `_transform` / `_atransform` 时抛 `NotImplementedError` 并说明只支持哪一侧；`RunnableLambda` 对协程函数调 `invoke` 抛 `TypeError` 并提示改用 `ainvoke`。

**架构推论：** 「同步优先 + 异步降级」把接入成本压在框架侧而不是接入方。代价是真正的异步实现若不主动覆盖 `ainvoke`，就会被线程池包裹、失去并发度。这类默认值的方向选择在契约型抽象里比「哪个更现代」更重要——它决定了生态里第三方实现的质量下限。

**架构推论：** 默认值「全部可用」与「全部报错」是两种截然不同的契约哲学。这里选了可用，于是组合的鲁棒性来自基类而不是每个实现；副作用是「这个组件其实不支持流式」这件事在类型层面不可见，只能在运行时表现为「流被攒完再吐一个块」。

**架构推论：** 摊平构造让组合器的所有下游逻辑（图导出、配置规格收集、trace 编号、批量隔离的索引映射）都只需处理线性列表。若保留嵌套结构，这些地方每一个都要走树遍历，而树遍历的正确性成本远高于它带来的表达力。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/runnables/base.py` | `Runnable` | 管道零件的抽象契约：`invoke` 抽象，其余九种调用给出可用默认 |
| `libs/core/langchain_core/runnables/base.py` | `RunnableSerializable` | `Serializable` + `Runnable` 的交叉，使组合器可被序列化 |
| `libs/core/langchain_core/runnables/base.py` | `RunnableSequence` | `|` 的落地形态：构造期摊平、逐步执行、子 run 标注、错误原样抛出 |
| `libs/core/langchain_core/runnables/base.py` | `RunnableLambda` | 把同步或异步函数包成 `Runnable`，协程函数误调同步路径时报明确错误 |
| `libs/core/langchain_core/runnables/base.py` | `RunnableGenerator` | 把生成器函数包成 `Runnable`，保住流式；缺失一侧实现时抛 `NotImplementedError` |
| `libs/core/langchain_core/runnables/base.py` | `_ChunkAccumulator` | 流式结束后重建完整值；`AIMessageChunk` 批量合并避免平方级相加 |
| `libs/core/langchain_core/runnables/base.py` | `_transform_stream_with_config` | 所有 `stream` / `transform` 的统一执行体：tee、子 run、流式回调挂载、结束前读累积值 |
| `libs/core/langchain_core/runnables/config.py` | `run_in_executor` / `get_executor_for_config` | 异步降级与批量默认实现所依赖的线程池入口 |

## 参考

- [langchain 源码：runnables/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/base.py)
- [langchain 文档：Runnable 接口与 LCEL](https://python.langchain.com/docs/concepts/runnables/)
- [langchain 文档：流式（streaming）](https://python.langchain.com/docs/concepts/streaming/)
- [Python 文档：`contextvars` 与 `context.run`](https://docs.python.org/3/library/contextvars.html)

## 与主核 / AI 的关系

`Runnable` 的契约设计与主核（多模态数据库内核）里「执行器 / 算子」的抽象有同一个骨架：**一个算子只声明最小实现，其余能力由框架补齐**。主核的执行器也是「实现 `next()`，框架负责批处理、并行、取消、指标」。两边都在回答同一个问题：怎样让第三方实现少写代码而又不把能力做成假的。

三条可以对照的经验。

**第一，默认实现的方向必须跟着生态走。** 这里选了「同步优先、异步降级」，因为要接入的多是同步客户端。主核若把默认值选反（例如以异步为先，逼所有磁盘 I/O 实现都造一层包装），接入成本会从框架侧转移到每个实现侧，而实现侧的质量没有保证。反过来，默认值一旦选对，生态里的实现会自然趋向「能跑就行」，而框架不必为此负责。

**第二，性能上有害的默认必须显式标注风险。** `_ChunkAccumulator` 的批量合并注释直接点出「逐个相加是工具调用长度的平方级」。主核里同样存在这类陷阱（逐行 `memcpy` 拼缓冲区、逐条 `fsync`），而它们通常不会表现为「很慢」，只表现为「工具调用一长就卡」。把「为什么不能逐条」写成注释，比事后加一个 benchmark 更有效——因为下一个人只会看到 `+=`。

**第三，错误路径本身必须不可失败。** `value_for_error()` 的存在是因为「重建结果」这个动作可能抛异常，而它出现在错误上报的实参里。主核的错误处理（回滚、写 WAL、上报崩溃）同样有此隐患：如果崩溃处理路径要分配内存、要读盘，那么内存不足时它自己就会失败。把错误路径简化成「不做可能失败的事」，是与性能无关的硬要求。

另一条更具体的：**摊平构造**。`a | b | c` 与 `a | (b | c)` 得到同一个结构，于是所有下游只需处理线性表。主核里凡是「表达式 → 执行计划」的地方都可以照搬：在构造期摊平，而不是在执行期走树。
