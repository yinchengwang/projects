---
project: langchain
feature: parallel-and-each
title: 并行组合与逐项映射
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/runnables/base.py
    symbols: [RunnableParallel, RunnableEach, RunnableEachBase]
  - path: libs/core/langchain_core/runnables/base.py
    symbols: [RunnableBinding, RunnableBindingBase]
---

# 并行组合与逐项映射

## 一句话职责

`RunnableParallel` 把同一份输入喂给多个分支并合并它们的输出（同步走线程池、异步走 `gather`、流式走「多路 tee + 谁先完成谁先吐」）；`RunnableEach` 把「一个列表」逐项交给同一个 `Runnable` 并返回列表；`RunnableBinding` 则是不改变行为、只给内层附加 kwargs / config / 类型声明的那层包装——重试与回退都是它的子类。

## 机制是什么

这三个类看着是三种组合器，其实回答的是三个不同的问题。

**并行**要回答「一份输入如何分给 N 个分支，再把 N 份输出拼回一份」。这里的合并语义很具体：输出是一个**字典**，键就是分支名。于是「分支」与「输出字段」是同一件事——`RunnableParallel(a=f, b=g).invoke(x)` 得到 `{"a": ..., "b": ...}`。这也意味着并行器天然可嵌套在序列中扮演「展开成字典」的角色，而 `RunnablePassthrough.assign` 正是靠这一点实现的（见 a06）。

**逐项**要回答「同一个管道如何吃掉一列表输入」。它不是一个新算法——`RunnableEachBase._invoke` 的实现就是转手调用 `self.bound.batch(...)`。所以 `RunnableEach` 的价值在**接口形状**：它把一个「吃单个输入」的管道变成「吃列表」，从而让「批量」这件事能出现在序列的任意位置，而不只是在链的最外层调 `batch`。

**绑定**要回答一个更基础的问题：怎样给某个 `Runnable` 附加「作用域固定的参数」，同时不破坏它的九种调用方式、不丢失它的子类身份。重试、回退、可配置化全都建立在这个能力上——它们本质都是「用一个包装器持有内层 + 一份附加配置」。

三者合起来构成表达式语言的一半语法：并行是「并列」，`each` 是「映射」，绑定是「装饰」。

## 怎么实现的

**`RunnableParallel` 的输入侧是「所有分支输入字段的并集」。**

这件事在 `get_input_schema` 与 `InputType` 两处体现：

```
for step in self.steps__.values():
    if step.InputType:
        return step.InputType
return Any
```

以及 schema 侧把每个分支 schema 的字段并成一个大模型（排除 `__root__`）：

```
{... for k, v in get_fields(step.get_input_schema(config)).items() if k != "__root__"}
```

合并字段而不是取交集，是因为并行分支各自只读自己需要的键——`{"context": retriever, "question": passthrough}` 里分支一读 `context`、分支二读 `question`，合成的输入就必须同时含两者。若取交集，任何「两个分支各取不同键」的并行组合都会得到一个空 schema。

输出侧则是**逐键取分支的 `OutputType`**：

```
fields = {k: (v.OutputType, ...) for k, v in self.steps__.items()}
return create_model_v2(self.get_name("Output"), field_definitions=fields)
```

于是并行器的输出 schema 是「键 → 各分支输出类型」的精确字典模型，而不是笼统的 `dict[str, Any]`。这让上层的类型检查与 OpenAPI 生成都能看到真实形状。

**同步并行：一个线程池、每分支一个 future、配置随线程走。**

```
steps = dict(self.steps__)   # 浅拷贝，避免调用方在 invoke 期间改动 steps
with get_executor_for_config(config) as executor:
    futures = [executor.submit(_invoke_step, step, input, config, key) for key, step in steps.items()]
    output = {key: future.result() for key, future in zip(steps, futures, strict=False)}
```

四处都值得看：

1. **`dict(self.steps__)` 浅拷贝**，注释写明是为了「避免调用方在 `invoke()` 期间改动 steps 造成的问题」。这个防御很便宜，但少了它就会出现「跑一半 steps 变了」的诡异行为。
2. **`_invoke_step` 里 `patch_config(... get_child(f"map:key:{key}"))`** —— 每个分支在 trace 里是独立子 run，标签带键名。这与序列的 `seq:step:{i}` 是同一套约定，于是观测面板上「并行」与「串行」的区别一眼可见。
3. **`with set_config_context(child_config) as context: return context.run(step.invoke, input_, child_config)`** —— 分支在自己的 config 上下文里跑。这是 a05 讲的上下文传播在并行场景的落点：若不设，分支读到的会是父级（甚至更上层）的 config。
4. **`future.result()` 的收集顺序与 `steps` 的键顺序一致**（`zip(steps, futures, strict=False)`）。于是输出字典的键顺序稳定，与分支定义顺序对应。

**注意 `RunnableParallel` 没有覆盖 `batch`。** 于是 `parallel.batch([x1, x2])` 走的是基类默认实现：用线程池把每个输入各跑一次 `invoke`，而每次 `invoke` 内部又开一个线程池跑分支。这是**嵌套并行**——外层 `max_concurrency` 控制输入条数，内层同样读 `max_concurrency`。源码没为这个组合做特化，所以一个「3 分支 × 100 输入」的 `batch` 会真的去开 300 个任务。这个行为不是 bug，但它解释了为什么大规模并行批处理更常见的是「先 `batch` 外层管道」而不是「把并行器放进 `batch` 里」。

**异步并行换成 `gather`，且每个分支的协程被 `create_task` 包过。**

```
results = await asyncio.gather(*(_ainvoke_step(step, input, config, key) for key, step in steps.items()))
output = dict(zip(steps, results, strict=False))
```

`_ainvoke_step` 结尾是：

```
return await coro_with_context(step.ainvoke(input_, child_config), context, create_task=True)
```

`coro_with_context(..., create_task=True)` 是这里的关键：协程必须在**被创建的那个上下文**里执行。若只 `await` 它，`await` 表达式本身会切回调用方的上下文，分支里读 `ContextVar` 就会读到父级的值。用 `create_task` 让协程在独立任务里跑，并显式把上下文按过去。

**流式并行的实现是这一页最精巧的一段。**

```
steps = dict(self.steps__)
input_copies = list(safetee(inputs, len(steps), lock=threading.Lock()))
with get_executor_for_config(config) as executor:
    named_generators = [
        (name, step.transform(input_copies.pop(), patch_config(config, callbacks=run_manager.get_child(f"map:key:{name}")))
        for name, step in steps.items()
    ]
    futures = {executor.submit(next, generator): (step_name, generator) for step_name, generator in named_generators}
    while futures:
        completed_futures, _ = wait(futures, return_when=FIRST_COMPLETED)
        for future in completed_futures:
            (step_name, generator) = futures.pop(future)
            try:
                yield AddableDict({step_name: future.result()})
                futures[executor.submit(next, generator)] = (step_name, generator)
            except StopIteration:
                pass
```

四个要点：

1. **`safetee(inputs, len(steps), lock=threading.Lock())`** —— 把一条输入流复制成 N 条，每条由不同线程消费。普通 `itertools.tee` 不是线程安全的：两个 peer 并发读同一条源流会破坏缓冲。`safetee` 的 `tee_peer` 在锁内检查「我等待期间别的 peer 是否已产出」并重试，且**把读到的项追加到所有 peer 的缓冲**——于是并发消费仍是正确顺序。
2. **每个分支的生成器被 `executor.submit(next, ...)` 驱动**，而不是在主线程里轮转。于是「谁先产出」由线程调度决定，主线程只做收集。
3. **每个完成的分支立即被再次提交下一次 `next`**，`StopIteration` 是这个分支结束的唯一信号，此时不再重新提交。`while futures` 终止条件就是「所有分支都耗尽」。
4. **产出的是 `AddableDict({step_name: chunk})` 而不是普通 dict**。这是并行流能被下游正确拼接的前提：下游（例如序列里的下一步，或最终用户拿到的 `stream()`）会把这些小字典用 `+` 累加。

`AddableDict` 的 `__add__` 定义了合并语义：

```
for key in other:
    if key not in chunk or chunk[key] is None:
        chunk[key] = other[key]
    elif other[key] is not None:
        try:
            added = chunk[key] + other[key]
        except TypeError as exc:
            raise TypeError(f"Cannot add incompatible types for key {key!r}: ...") from exc
        chunk[key] = added
```

**按键合并、同键相加、`None` 视为缺失**。三个分支各自只在自己的键上出现，所以实际走的是 `key not in chunk` 那一支——「拼装」而不是「相加」。只有当同一个键在两个 chunk 里都有值时才会真的相加（例如某个分支自己又是个流式模型，吐出同一键的多个增量）。

那个 `TypeError` 的报错文本很讲究：它点名了键与两侧的类型名。这比原生的 `unsupported operand type(s)` 有用得多，因为流的合并出错时，调用方根本不知道是哪个键炸的。

**图的导出把并行画成「输入节点 → 每分支子图 → 输出节点」。**

```
graph = Graph()
input_node = graph.add_node(self.get_input_schema(config))
output_node = graph.add_node(self.get_output_schema(config))
for step in self.steps__.values():
    step_graph = step.get_graph()
    step_graph.trim_first_node()
    step_graph.trim_last_node()
    if not step_graph:
        graph.add_edge(input_node, output_node)
    else:
        step_first_node, step_last_node = graph.extend(step_graph)
        graph.add_edge(input_node, step_first_node)
        graph.add_edge(step_last_node, output_node)
```

两个细节：**分支子图的首尾节点被裁掉**（因为并行的入口与出口由外层的输入/输出节点代表），以及**空分支直接连输入到输出**（一个不产生任何节点的分支等价于直通）。这两条规则让「并行器导出成图」的结果既正确又不会长出一堆无意义的中间节点。

**`RunnableEachBase` 的实现只有一行有效逻辑。**

```
def _invoke(self, inputs, run_manager, config, **kwargs):
    configs = [patch_config(config, callbacks=run_manager.get_child()) for _ in inputs]
    return self.bound.batch(list(inputs), configs, **kwargs)
```

**每个元素拿到一份自己的 config（各自为子 run），然后整体交给内层的 `batch`。** 于是 `each.invoke([a, b, c])` 等于 `bound.batch([a, b, c])`，逐项的内层实现可以按自己的优化路径跑（有原生批量的模型就一次调，没有的就线程池并发）。

这个设计让 `RunnableEach` 成了「批量能力的位置搬运工」：批量原本只在链的最外层可用（`chain.batch(...)`），`each` 让它能出现在序列中段（`chain = prep | each_worker | post`）。代价是语义上**它不是串行 for 循环**——若内层支持批量，元素会被并发处理。

`get_graph` 直接转发内层（`return self.bound.get_graph(config)`）：逐项映射在结构上没有额外的节点，图就是内层的图。

`astream_events` 则是显式不支持：

```
if version == "v3":
    return self._astream_events_unsupported_v3()
return self._astream_events_unsupported_v1_v2()
```

两个辅助函数都 `raise NotImplementedError("RunnableEach does not support astream_events yet.")`。分成两个函数是为了满足「v3 返回协程、v1/v2 返回异步生成器」的签名差异（后者末尾还留了个 `yield` 让函数成为异步生成器）。这是一个诚实的能力声明：逐项映射的事件流语义确实难以定义（一个 `each` 事件对应 N 个子事件，边界在哪）。

**`RunnableBindingBase` 的字段就是「内层 + 四份附加物」。**

```
bound: Runnable[Input, Output]
kwargs: Mapping[str, Any] = Field(default_factory=dict)
config: RunnableConfig = Field(default_factory=RunnableConfig)
config_factories: list[Callable[[RunnableConfig], RunnableConfig]] = Field(default_factory=list)
custom_input_type: Any | None = None
custom_output_type: Any | None = None
```

`kwargs` 是「每次调用都自动带上」的参数（如 `stop=["-"]`）；`config` 是绑定的配置；`config_factories` 是**每次调用时才求值**的配置工厂（用于需要读调用侧 config 才能算出的值）；两个 `custom_*_type` 覆盖类型声明。

构造期有一处必须留意的补丁：

```
super().__init__(bound=bound, kwargs=..., config=config or {}, ...)
# if we don't explicitly set config to the TypedDict here,
# the pydantic init above will strip out any of the "extra"
# fields even though total=False on the typed dict.
self.config = config or {}
```

也就是说 pydantic 的 init 会把 `RunnableConfig`（一个 `total=False` 的 `TypedDict`）里未声明的键**剥掉**，即便这些键在运行时是被允许的（比如用户塞进去的自定义 config 键）。于是构造完必须再赋一次原对象。这行注释是这一层里最有价值的注释——没有它，下一个人会以为这次重复赋值是冗余的。

调用时的配置合并是两步：

```
def _merge_configs(self, *configs: RunnableConfig | None) -> RunnableConfig:
    config = merge_configs(self.config, *configs)
    return merge_configs(config, *(f(config) for f in self.config_factories))
```

**先合并绑定配置与调用配置，再把合并结果喂给每个 `config_factory`，最后合并工厂的产物。** 顺序有意义：工厂能看到前两步的全部结果（含调用侧传入的 `configurable`），从而按调用参数算出附加配置。若先算工厂再合并，工厂就只能看到绑定配置。

`merge_configs` 自己对不同类型的键采取不同策略：

- `metadata`：逐层合并字典；
- `tags`：**取集合并排序**（`sorted(set(base + these)`），于是标签与顺序无关；
- `configurable`：字典浅合并，后写的覆盖先写的；
- `callbacks`：分六种情况处理（因为值可以是 `None`、handler 列表或 manager 三种形态，两两组合共六种），列表相加、manager 则复制并追加。

**kwargs 的合并顺序决定「谁赢」：**

```
return self.bound.invoke(input, self._merge_configs(config), **{**self.kwargs, **kwargs})
```

**绑定的 kwargs 在前、调用时的 kwargs 在后**，于是调用侧覆盖绑定值。这与 config 合并方向一致（后写的覆盖先写的）——「显式传入的这一次」永远优先于「事先绑定的」。

`invoke` / `ainvoke` / `batch` / `abatch` / `stream` / `astream` / `transform` 等九个方法全都是一行转发，只是把 config 换成 `_merge_configs(config)`、把 kwargs 换成 `{**self.kwargs, **kwargs}`。这九个转发就是「binding 保留全部能力」的实现：能力没有被裁剪，只是被套了一层。

**`RunnableBinding` 与 `RunnableBindingBase` 的差别只有一处，但那一处决定了子类能否链式调用。**

`RunnableBinding.bind` 返回的是 `self.__class__(...)`：

```
return self.__class__(
    bound=self.bound,
    config=self.config,
    config_factories=self.config_factories,
    kwargs={**self.kwargs, **kwargs},
    custom_input_type=self.custom_input_type,
    custom_output_type=self.custom_output_type,
)
```

用 `self.__class__` 而不是 `RunnableBinding`，于是 `RunnableRetry(...).bind(stop=["-"])` 得到的仍是一个**带重试**的包装，而不是一个被抹掉了重试的裸 binding。这是一个很小但很关键的细节：若这里写死类名，任何「先重试再绑定」的写法都会静默地丢掉重试配置。

`RunnableBinding` 的类注释把这一层定位成「runnable decorator」，并列出它挂出的六个方法：`bind`（绑 kwargs）、`with_config`（绑 config）、`with_listeners`（绑生命周期回调）、`with_types`（覆盖输入输出类型）、`with_retry`、`with_fallbacks`。前四个在 `RunnableBindingBase` 上直接实现，后两个在别处构造各自的子类。

`get_name` 也转发内层（`return self.bound.get_name(suffix, name=name)`），于是 trace 里的名字看起来就像那个内层组件——一层包装不该改变可观测量。

**`RunnableEach` 只需覆盖 `get_name`、`bind`、`with_config` 三处。**

```
def get_name(self, suffix=None, *, name=None):
    name = name or self.name or f"RunnableEach<{self.bound.get_name()}>"
    return super().get_name(suffix, name=name)

def bind(self, **kwargs):
    return RunnableEach(bound=self.bound.bind(**kwargs))
```

第一处让名字带上内层名字（`RunnableEach<RunnableSequence<...>>`），第二处把绑定**穿透到内层**而不是包在 `each` 外面——于是 `each.bind(stop=["-"])` 里的 `stop` 直接作用于真正执行的内层管道，语义更直接。

## 关键取舍

**源码事实：** `RunnableParallel` 的输入 schema 是**所有分支输入字段的并集**（排除 `__root__`），`InputType` 取第一个非空的分支；输出 schema 是「键 → 各分支 `OutputType`」的精确模型。

**源码事实：** 同步并行用 `dict(self.steps__)` 浅拷贝防改动、每分支一个 future、每分支带 `map:key:{key}` 子 run 标签并在自己的 `config` 上下文里跑；收集顺序与键顺序一致。

**源码事实：** `RunnableParallel` **没有覆盖 `batch`**，于是 `parallel.batch(...)` 走基类默认实现，形成「外层每个输入一个任务 + 内层每分支一个任务」的嵌套并行。

**源码事实：** 异步并行用 `asyncio.gather`，且每个分支的协程经 `coro_with_context(..., create_task=True)` 在被创建的上下文里执行。

**源码事实：** 流式并行用 `safetee(..., lock=threading.Lock())` 复制输入流，用线程池驱动每个分支生成器的 `next`，`wait(..., FIRST_COMPLETED)` 收集，产出 `AddableDict({name: chunk})` 并在该分支未耗尽时立即重新提交下一次。

**源码事实：** `AddableDict.__add__` 按键合并：缺失或 `None` 直接取新值，两侧非 `None` 才相加，相加失败时抛带键名与两侧类型名的 `TypeError`。

**源码事实：** `RunnableEachBase._invoke` 只是给每个元素各造一份子 config 后调用 `self.bound.batch(...)`；`get_graph` 转发内层；`astream_events` 以 `NotImplementedError` 明确声明不支持（v3 与 v1/v2 分成两个签名）。

**源码事实：** `RunnableBindingBase` 在构造后必须再赋一次 `self.config`，否则 pydantic 会剥掉 `TypedDict` 里未声明的键。

**源码事实：** `_merge_configs` 先合并绑定配置与调用配置，再把结果喂给每个 `config_factory` 后再合并其产物；`merge_configs` 对 `tags` 取集合并排序、`metadata` 逐层合并、`configurable` 后写覆盖、`callbacks` 分六种形态处理。

**源码事实：** binding 的调用转发里 kwargs 是 `{**self.kwargs, **kwargs}`，即调用侧覆盖绑定值；`RunnableBinding.bind` 返回 `self.__class__(...)`，保住子类身份。

**架构推论：** 「输出字典的键 = 分支名」这个决定让并行器同时承担了「命名展开」的职责，也解释了为什么 `RunnablePassthrough.assign` 的实现只是一个 `RunnableAssign(RunnableParallel(...))`——赋值的本质就是「把新算的键并进输入字典」，而并行器已经在做「算出多个键的字典」。

**架构推论：** `each` 把批量能力从「链的最外层」搬到「序列中任意位置」，这件事只有在内层真正支持批量时才有收益。若内层是纯逐条的（例如一个同步 HTTP 客户端），`each` 与手写 for 循环的区别只剩 trace 结构与错误处理——而它的错误会以 `batch` 的语义出现（默认整批失败），不是「跑到第几条停下」。

**架构推论：** binding 用「九个方法各一行转发」换取零能力损失。这类包装器的风险集中在两处：一是 kwargs / config 的合并优先级（这里选「调用侧优先」），二是子类身份是否被保住（这里用 `self.__class__`）。两者都不是行为的一部分，却决定了整个装饰器体系是否可组合。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/runnables/base.py` | `RunnableParallel` | 同一输入喂多分支，输出按键合并；同步线程池、异步 gather、流式多路 tee |
| `libs/core/langchain_core/runnables/base.py` | `RunnableEachBase` | 逐项映射的实现体：给每个元素配一份子 config 后转调内层 `batch` |
| `libs/core/langchain_core/runnables/base.py` | `RunnableEach` | 只补 `get_name` 与穿透式 `bind` / `with_config` 的薄子类 |
| `libs/core/langchain_core/runnables/base.py` | `RunnableBindingBase` | 持有内层 + kwargs / config / config_factories / 类型覆盖，九个方法各一行转发 |
| `libs/core/langchain_core/runnables/base.py` | `RunnableBinding` | 「runnable 装饰器」，`bind` 用 `self.__class__` 保住子类身份 |
| `libs/core/langchain_core/runnables/config.py` | `merge_configs` | tags 取并集排序、metadata 逐层合并、configurable 覆盖、callbacks 六态处理 |
| `libs/core/langchain_core/runnables/utils.py` | `AddableDict` | 流式并行的输出块类型，按键合并并在类型不兼容时报带键名的错 |
| `libs/core/langchain_core/utils/iter.py` | `safetee` | 线程安全的 tee，锁内检查并追加到所有 peer 缓冲 |

## 参考

- [langchain 源码：runnables/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/base.py)
- [langchain 文档：并行执行](https://python.langchain.com/docs/concepts/lcel/)
- [Python 文档：`itertools.tee`](https://docs.python.org/3/library/itertools.html#itertools.tee)
- [Python 文档：`concurrent.futures.wait`](https://docs.python.org/3/library/concurrent.futures.html#concurrent.futures.wait)

## 与主核 / AI 的关系

并行执行器与主核里的「并行扫描」几乎同构：把一份输入分给多个算子、收集多份输出、在某个失败时决定是整批失败还是逐项隔离。这里有三条经验可以直接搬。

**第一，合并语义必须是数据类型自带的，而不是调用方约定的。** `AddableDict` 把「同键相加、缺失即取」写进 `+` 运算符，于是并行的流可以被任意下游用同一个 `+` 拼起来（accumulator、序列、用户手写都一样）。主核里若要让「部分结果」可合并（例如多个并行分区的聚合），同样应当让合并语义成为值类型的一部分，而不是在各处手写「按键并进目标字典」。

**第二，tee 的并发版本必须自己实现。** `safetee` 的存在是因为 `itertools.tee` 不是线程安全的：两个 peer 并发取项会破坏共享缓冲。这个坑在主核里对应「多个消费者读同一份结果流」——若直接用无锁的缓冲共享，症状是偶发的错位或丢项，而它只在真正并发时出现。

**第三，包装器的两层身份要分开。** `RunnableBindingBase` 是「字段与转发」，`RunnableBinding` 是「保住子类身份的构造函数」。这个拆分让 `RunnableRetry.bind()` 仍然返回 `RunnableRetry`。主核里若做「给执行器套一层指标 / 重试 / 限流」，同样需要一个「返回 `self.__class__`」的构造函数，否则「先重试再绑定」会静默地丢掉重试。

还有一条值得注意的**反向**经验：`RunnableParallel` 没有特化 `batch`，于是两个维度的并行会相乘。这在主核里对应「并行算子里再开并行」——它不会错，但会把并发度从「配置值」变成「配置值的乘积」，从而打爆下游连接池。要么限制层数，要么在某一层显式串行化。
