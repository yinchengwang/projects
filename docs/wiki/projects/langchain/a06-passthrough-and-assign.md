---
project: langchain
feature: passthrough-and-assign
title: 透传、赋值与字段选取
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/runnables/passthrough.py
    symbols: [RunnablePassthrough, RunnableAssign, RunnablePick]
  - path: libs/core/langchain_core/runnables/base.py
    symbols: [_RunnableCallableSync]
---

# 透传、赋值与字段选取

## 一句话职责

三个在「字典流水线」上做最小改动的类：`RunnablePassthrough` 原样返回输入（可选地先对输入执行一个副作用函数），`RunnableAssign` 在保留原字典的同时把若干新键算出来并**覆盖**同名键，`RunnablePick` 从字典里挑出一或几个键——三者合起来就是 LCEL 里「不改变输入、只往上下文里加料或减料」的语法。

## 机制是什么

RAG 链条里最高频的动作不是「转换」而是「保持与补充」：要把原始问题透传到下游、要在已有字典上追加一个检索结果字段、要在喂给模型之前只取两个字段。这些都不该需要一个真正的转换函数，于是有了这三个类。

它们的共同特征是**都在 `dict[str, Any]` 上工作**，而且都不改变字典的语义（不重排、不重建类型）：

| 类 | 输入 | 输出 | 关键行为 |
|---|---|---|---|
| `RunnablePassthrough` | 任意 | 同一份输入 | 可挂一个 `func` 作为副作用，返回值被丢弃 |
| `RunnableAssign` | 字典 | 字典 | 保留原键、追加 mapper 算出的键、同名键被覆盖 |
| `RunnablePick` | 字典 | 单值或字典 | `keys` 是字符串时返回裸值，是列表时返回子字典 |

「透传」这个名字容易误导：`RunnablePassthrough` 是**整个表达式语言里最不「无为」** 的一个类。它的 `func` 参数是一个真正的功能点（例如记录「这次输入是什么」），而它的 `.assign(...)` 类方法返回的其实是 `RunnableAssign`——**透传类自己不赋值，赋值是另一个类**。

这一点在源码里看得很清楚：

```
@classmethod
def assign(cls, **kwargs) -> RunnableAssign:
    return RunnableAssign(RunnableParallel[dict[str, Any]](kwargs))
```

以及 `Runnable.assign`（`Runnable` 的通用方法）的实现：

```
return self | RunnableAssign(RunnableParallel[dict[str, Any]](kwargs))
```

**同一个语义「把若干新键算出来并合并进输入」在任何 `Runnable` 上都可以用，方式是 `自身 | RunnableAssign(...)`。** 也就是说赋值不是一种独立能力，而是「并行器 + 合并逻辑」的组合。

## 怎么实现的

**`RunnablePassthrough` 的核心是 `identity` / `aidentity` 两个模块级函数。**

```
def identity(x: Other) -> Other:
    return x
```

它的 `invoke` 分两步：

```
if self.func is not None:
    call_func_with_variable_args(self.func, input, ensure_config(config), **kwargs)
return self._call_with_config(identity, input, config)
```

**先跑 `func`（丢弃返回值），再走 `identity`。** 顺序不能反：`func` 是「在输入进入下游之前做点事」，如果放在后面就变成「在返回之后做事」，语义不同。

`call_func_with_variable_args` 是一个签名探测式的调用器（见 a05 的 `config.py`）：它对 `func` 做 introspection，若 `func` 接受 `config` 或 `run_manager` 就把它们作为关键字传进去。于是用户写 `def f(x)` 或 `def f(x, config)` 都能用——**同一个参数位承载两种签名**。这是 LCEL 里「让用户写的函数不必了解框架内部」的实现方式。

构造期还有一个必要的修正：

```
if inspect.iscoroutinefunction(func):
    afunc = func
    func = None
```

**把一个 `async def` 传进 `func=` 会被自动挪到 `afunc`。** 若不挪，同步 `invoke` 路径会调用它并得到一个未被 await 的协程对象——那个协程永远不会被执行，而副作用也就永远不会发生。这是一个**静默失败**的经典形态，作者选择在构造期把它转成正确的字段而不是在运行期报错。

`__repr_args__` 那一段是另一个值得记的小细节：

```
def __repr_args__(self) -> Any:
    # Without this repr(self) raises a RecursionError
    # See https://github.com/pydantic/pydantic/issues/7327
    return []
```

注释给了 issue 链接。原因是这个类在 `_graph_passthrough` 这个模块级实例里被反复引用，pydantic 的默认 `repr` 会对字段递归取值，触发递归（模块级实例 → 类 → 字段 → 实例）。返回空列表就断掉了递归。

**流式版本对 `func` 的处理是这一页最需要读懂的一段。**

`transform` 里两个分支：

```
if self.func is None:
    for chunk in self._transform_stream_with_config(input, identity, config):
        yield chunk
else:
    final: Other
    got_first_chunk = False
    for chunk in self._transform_stream_with_config(input, identity, config):
        yield chunk
        if not got_first_chunk:
            final = chunk
            got_first_chunk = True
        else:
            try:
                final = final + chunk
            except TypeError:
                final = chunk
    if got_first_chunk:
        call_func_with_variable_args(self.func, final, ensure_config(config), **kwargs)
```

**块照常逐个转发，`func` 在流结束后才被调用一次，且入参是「攒起来的最终值」。**

这与 `invoke` 的语义对齐（`func` 收到的是完整输入），但它带来一个必须做对的细节：**攒不起来怎么办**。`final = final + chunk` 可能抛 `TypeError`（`str` 加 `dict`、两个不可加的 dataclass），此时 `final = chunk` —— **用最后一块替换**，而不是放弃。

也就是说这里的降级策略是「尽力给出最有代表性的一块」。对比 `RunnableBranch.stream`（a03）里遇到同样情况会置 `final_output_supported = False` 并**停止累加**，这里的做法更宽松：仍然继续尝试下一块能不能加在当前那块上。两种都合理，差别在于「是否认为可加性是稳定的属性」——branch 认为一旦不可加就永久不可加，passthrough 认为可能只是这一对不可加。

`atransform` 的注释把前提说得很清楚：

> 按定义，函数会作用于聚合后的输入。所以我们会把输入聚合到最后一个块为止。如果输入不可相加，就假定只能作用于最后一个块。

最后那句 `if got_first_chunk` 的存在理由是：空流的情况下不调用 `func`。**一个从未收到任何输入的透传不该声称自己被调用过**——否则 `func` 里的副作用（记账、日志）会在零输入时凭空发生一次。

**`RunnablePassthrough` 的 `InputType` / `OutputType` 是同一个字段的两个投影。**

```
@property
def InputType(self): return self.input_type or Any
@property
def OutputType(self): return self.input_type or Any
```

`input_type` 是构造参数（`*, input_type: type[Other] | None = None`），一个声明同时定义输入与输出——因为透传不改类型。这是全部组合器里最直接的一处「类型声明等于语义」。

**`RunnableAssign` 的本质是一次「并行 + 键过滤 + 字典合并」。**

同步路径：

```
def _invoke(self, value, run_manager, config, **kwargs):
    if not isinstance(value, dict):
        raise ValueError("The input to RunnablePassthrough.assign() must be a dict.")
    return {
        **value,
        **self.mapper.invoke(value, patch_config(config, callbacks=run_manager.get_child()), **kwargs),
    }
```

**先展开原字典，再展开 mapper 的输出。** 于是同名键被 mapper 覆盖——这正是「assign」的语义（重新赋值）。`Run` 的顺序也保证：mapper 收到的输入是原字典（含所有原键），因此它既可以读原字段也可以只读其中几个。

`mapper` 的构造被限定在 `RunnableParallel`（字段类型就是 `RunnableParallel`），于是「算哪些键」这件事由并行的分支定义——`RunnableAssign(RunnableParallel(a=f, b=g))` 就是「追加 a、b 两个键」。

**流式版本必须解决一个真正的并发问题：透传流与 map 流要同时推进，但两者消费同一条输入流。**

```
mapper_keys = set(self.mapper.steps__.keys())
for_passthrough, for_map = safetee(values, 2, lock=threading.Lock())
map_output = self.mapper.transform(for_map, patch_config(config, callbacks=run_manager.get_child()), **kwargs)
with get_executor_for_config(config) as executor:
    first_map_chunk_future = executor.submit(next, map_output, None)
    for chunk in for_passthrough:
        if not isinstance(chunk, dict):
            raise ValueError("...must be a dict.")
        filtered = AddableDict({k: v for k, v in chunk.items() if k not in mapper_keys})
        if filtered:
            yield filtered
    first_chunk = first_map_chunk_future.result()
    if first_chunk is not None:
        yield first_chunk
        for chunk in map_output:
            yield chunk
```

四步：

1. **`safetee(values, 2, lock=threading.Lock())`** 把输入流复制成两份，一份给透传、一份给 mapper。用 `safetee` 而不是 `itertools.tee` 是因为 mapper 的流在另一个线程里推进（下一步），并发读同一条源流必须有锁。
2. **`executor.submit(next, map_output, None)`** 在后台**先拉一块**。`next` 的第二个参数 `None` 是默认值——于是「mapper 一个块都没有」时得到 `None` 而不是 `StopIteration`。这一步是必须的：mapper 的 `transform` 只有在被 `next` 拉取时才会开始推进，而这里要让它与透传流**并行**启动，否则就要先等透传全部结束才开始算 map。
3. **透传流逐块产出，但先剔除 mapper 的键**：

```
filtered = AddableDict({k: v for k, v in chunk.items() if k not in mapper_keys})
if filtered:
    yield filtered
```

`k not in mapper_keys` 防止「原输入里已经有 `a` 键、mapper 也要算 `a`」时产生两个 `a` 的块。于是下游用 `AddableDict.__add__` 累加时不会出现「先旧值后新值」的中间态——旧值根本没被吐出去。而 `if filtered:` 跳过空字典：一块只含 mapper 键的输入不该产生一个空块。

4. **先 `first_map_chunk_future.result()` 再继续 `for chunk in map_output`**。于是 map 的产出**严格排在透传产出之后**——块顺序是「所有原键 → 所有新键」。这个顺序是对 `dict` 合并语义的忠实翻译（`{**value, **mapper}` 里新键在后），也让下游的 `AddableDict` 累加得到一致的最终字典。

`_invoke` 与 `_transform` 的两处 `isinstance(value, dict)` 检查用了同一个错误消息：「The input to RunnablePassthrough.assign() must be a dict.」——**消息里写的是 `RunnablePassthrough.assign()` 而不是 `RunnableAssign`**。这是一个遗留的措辞（assign 的历史入口是 `RunnablePassthrough.assign`），用户从报错消息能认出自己用的是哪个 API，所以保留反而更有用。

**`RunnableAssign` 的 schema 是「输入 schema 与 mapper 输出的并集」。**

```
def get_input_schema(self, config=None):
    map_input_schema = self.mapper.get_input_schema(config)
    if not issubclass(map_input_schema, RootModel):
        return map_input_schema     # mapper 输入是 dict → 直接用
    return super().get_input_schema(config)

def get_output_schema(self, config=None):
    map_input_schema = self.mapper.get_input_schema(config)
    map_output_schema = self.mapper.get_output_schema(config)
    if not issubclass(map_input_schema, RootModel) and not issubclass(map_output_schema, RootModel):
        fields = {}
        for name, field_info in get_fields(map_input_schema).items():
            fields[name] = _get_schema_field_definition(field_info)
        for name, field_info in get_fields(map_output_schema).items():
            fields[name] = _get_schema_field_definition(field_info)
        return create_model_v2("RunnableAssignOutput", field_definitions=fields)
    ...
```

**输入直接取 mapper 的输入 schema**（因为 assign 不引入新的必须字段——新键是算出来的）。

**输出是 mapper 的输入字段 + mapper 的输出字段，且后者覆盖前者。** 两段 `for` 循环按顺序写进同一个 `fields` 字典，于是同名键取后者的类型定义——与运行时 `**value, **mapper` 的覆盖语义严格对应。`RootModel` 的判断是在问「这个 schema 是不是一个裸类型（比如 `str`）而不是对象」；若是裸类型，说明 mapper 的输入根本不是字典，那种情况下退回父类的通用处理。

**`get_graph` 给并行子图插了一根「旁路」。**

```
graph = self.mapper.get_graph(config)
input_node = graph.first_node()
output_node = graph.last_node()
if input_node is not None and output_node is not None:
    passthrough_node = graph.add_node(_graph_passthrough)
    graph.add_edge(input_node, passthrough_node)
    graph.add_edge(passthrough_node, output_node)
return graph
```

mapper 的图本身是「输入 → 各分支 → 输出」。assign 的语义要求「原输入也要流到输出」，于是加一条 `input → passthrough → output` 的旁路，把这条路径画出来。`_graph_passthrough` 是模块级单例（`RunnablePassthrough[Any]()`），复用它而不是每次新建，是为了让图里的这个节点身份稳定（同一张图重复导出时 id 一致）。

**`RunnablePick` 的返回形态取决于 `keys` 的类型，且空结果返回 `None`。**

```
def _pick(self, value: dict[str, Any]) -> Any:
    if not isinstance(value, dict):
        raise ValueError("...must be a dict.")
    if isinstance(self.keys, str):
        return value.get(self.keys)
    picked = {k: value.get(k) for k in self.keys if k in value}
    if picked:
        return AddableDict(picked)
    return None
```

三处：

1. **`str` 形态用 `value.get(...)`** —— 键不存在时返回 `None` 而不是抛 `KeyError`。这是刻意的宽容：下游可以据此判断「没有这个字段」。
2. **列表形态先按 `self.keys` 顺序构建，且只用 `value.get`**，于是**没说到的键不会出现在结果里**（`k in value` 过滤）。这与「取子集」的语义一致。
3. **结果包成 `AddableDict` 而不是普通 `dict`** ——于是 pick 的输出放进流式管道时也能被 `+` 累加。这是 passthrough 家族与并行的共同约定：**凡是产出的字典都应当是可加的**。
4. **`picked` 为空时返回 `None`**，而不是空字典。这个选择的后果是下游要处理两种「空」（`None` 与 `{}`）；好处是 `None` 更容易被 `RunnableBranch` 的条件判为假。

`get_name` 里有一处明确的字符串拼接：

```
name = (name or self.name or "RunnablePick" f"<{','.join([self.keys] if isinstance(self.keys, str) else self.keys)}>")
```

`"RunnablePick" f"<...>"` 是**相邻字符串字面量的隐式拼接**——两段字面量紧邻，Python 会直接连成一个。它看起来像笔误（少了 `+`），但实际合法且作者显然是有意的（否则会写成一行）。这种写法在格式化工具眼里容易变成「可疑」，属于可读性上的欠债。

**`_RunnableCallableSync` 这一组 Protocol 是「函数签名到 Runnable 能力」的映射表。**

```
class _RunnableCallableSync(Protocol[Input, Output]):
    def __call__(self, _in: Input, /, *, config: RunnableConfig) -> Output: ...
```

同族还有 `_RunnableCallableAsync`、`_RunnableCallableIterator`、`_RunnableCallableAsyncIterator`，四个协议按「同步/异步 × 单值/迭代器」排列。它们集中定义在 `base.py`，用途是给 `RunnableLike` 与 `coerce_to_runnable` 做类型标注：

```
RunnableLike = (
    Runnable[Input, Output]
    | Callable[[Input], Output]
    | Callable[[Input], Awaitable[Output]]
    | Callable[[Iterator[Input]], Iterator[Output]]
    | Callable[[AsyncIterator[Input]], AsyncIterator[Output]]
    | Mapping[str, ...]
    | ...
)
```

也就是说：`coerce_to_runnable` 根据**函数的签名形状**决定包装成哪种 `Runnable`——单值函数 → `RunnableLambda`，迭代器函数 → `RunnableGenerator`。四个协议就是这四个判定的形式化描述。

回到 `RunnablePassthrough`：`identity` 的签名 `def identity(x: Other) -> Other` 符合 `_RunnableCallableSync`，于是 `_call_with_config(identity, input, config)` 能把它当成一个标准的同步可调用体使用——**它不需要是 `Runnable`，只需要符合这张签名表**。这是这套框架里「鸭子类型」最多的地方，也是 `coerce_to_runnable` 存在的理由。

## 关键取舍

**源码事实：** `RunnablePassthrough.invoke` 先跑 `func`（丢弃返回值）再走 `identity`；构造期把 `async def` 自动从 `func` 挪到 `afunc`；`__repr_args__` 返回空列表以规避 pydantic 的 `RecursionError`。

**源码事实：** `transform` / `atransform` 在 `func` 存在时**照常逐块转发**，但把块攒成 `final` 并在流结束后调用一次 `func`；攒不起来时用最后一块替换；空流不调用 `func`。

**源码事实：** `RunnablePassthrough.assign` 与 `Runnable.assign` 都返回 `RunnableAssign(RunnableParallel(kwargs))`——赋值不是透传类自身的能力。

**源码事实：** `RunnableAssign._invoke` 返回 `{**value, **mapper 输出}`，同名键被覆盖；输入非 dict 时抛 `ValueError`，消息里写的是 `RunnablePassthrough.assign()`（历史入口名）。

**源码事实：** `RunnableAssign._transform` 用 `safetee(..., 2, lock=threading.Lock())` 分两路，用后台线程先拉 map 的第一块（`next(..., None)`），透传块先剔除 mapper 键再用 `AddableDict` 产出，最后才接上 map 的余下块——于是块顺序是「所有原键 → 所有新键」。

**源码事实：** `RunnableAssign` 的输入 schema 直接取 mapper 的输入 schema；输出 schema 是「mapper 输入字段 + mapper 输出字段」的并集，后者覆盖前者；`get_graph` 在 mapper 的图上加一条 `input → _graph_passthrough → output` 旁路。

**源码事实：** `RunnablePick._pick` 对 `str` 形态返回裸值（缺失为 `None`），对列表形态构建 `AddableDict` 子集（缺失键不出现），空结果返回 `None` 而不是 `{}`。

**源码事实：** `_RunnableCallableSync` / `_RunnableCallableAsync` / `_RunnableCallableIterator` / `_RunnableCallableAsyncIterator` 四个 `Protocol` 定义了「什么形状的函数可以被当成 `RunnableLike`」，`coerce_to_runnable` 据此选包装类。

**架构推论：** 这三个类共同确立了一条约定——**凡是在字典流水线里产出的字典都必须是 `AddableDict`**。原因是流式下游要靠 `+` 把它们累加成一个完整字典；普通 `dict` 的 `+` 会抛 `TypeError`，于是「并行器的输出接不上透传器的输出」这类问题会在运行期才暴露。

**架构推论：** `assign` 的流式实现展示了「两路消费一条输入流」的正确做法：先 `tee`，让慢的那一路在后台线程里预取一块（否则它根本不启动），然后按「原键在前、新键在后」的顺序吐出。这个顺序不是实现细节，而是对 `dict` 合并语义的忠实翻译——若顺序反过来，下游按 `AddableDict` 累加会得到同一个最终值，但中间态的可见性不同（订阅者会先看到新键）。

**架构推论：** 「`func` 收到的是聚合值」这条语义只能靠「攒块」实现，而攒块必然面对「有些块加不起来」。这里的降级（用最后一块）与 `RunnableBranch` 的降级（放弃累加）方向相反，但两者都承认同一件事：**可加性不是类型属性，而是值的属性**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/runnables/passthrough.py` | `RunnablePassthrough` | 原样透传；可选 `func` 副作用；流式时攒块后调用一次 |
| `libs/core/langchain_core/runnables/passthrough.py` | `identity` / `aidentity` | 透传的实际执行体，被当作符合 `_RunnableCallableSync` 的可调用体使用 |
| `libs/core/langchain_core/runnables/passthrough.py` | `RunnableAssign` | 保留原字典 + 覆盖同名键；流式时剔除旧键、先透传后 map |
| `libs/core/langchain_core/runnables/passthrough.py` | `RunnablePick` | 按 `keys` 挑键：`str` 返裸值、`list` 返 `AddableDict`、空结果返 `None` |
| `libs/core/langchain_core/runnables/passthrough.py` | `_graph_passthrough` | 模块级单例，用作图里那条旁路节点的数据 |
| `libs/core/langchain_core/runnables/base.py` | `_RunnableCallableSync` | 「同步单值函数」的协议，`coerce_to_runnable` 的判定依据之一 |
| `libs/core/langchain_core/runnables/base.py` | `Runnable.assign` | 通用赋值入口，等价于 `self | RunnableAssign(RunnableParallel(kwargs))` |

## 参考

- [langchain 源码：runnables/passthrough.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/passthrough.py)
- [langchain 文档：如何向链中传递数据](https://python.langchain.com/docs/how_to/passthrough/)
- [Python 文档：`TypedDict` 与 `Protocol`](https://docs.python.org/3/library/typing.html#typing.Protocol)
- [pydantic issue #7327：`repr` 递归](https://github.com/pydantic/pydantic/issues/7327)

## 与主核 / AI 的关系

「不改变输入、只补充或裁剪字段」在主核里对应的是**投影（projection）与行扩展**：查询计划里经常需要「保留原有列，再加一列计算结果」，或者「只取两列给上层」。这三条经验可以直接搬。

**第一，「赋值」应当是「并行 + 合并」的组合，而不是独立算子。** 这里 `assign` 的实现就是 `自身 | RunnableAssign(RunnableParallel(kwargs))`，于是「算什么新列」由并行的分支定义、「怎么合并」由 assign 定义，两件事各自复用已有能力。主核里若做「追加计算列」，同样应当把「计算」与「合并」拆开——合并语义（同名覆盖还是报错）是一个独立的决定。

**第二，流式下「两路消费一条输入流」必须预取。** `_transform` 里那个 `executor.submit(next, map_output, None)` 是必须的：不主动拉一块，另一路根本不会启动，于是「并行」变成了「串行」。主核里若做「同一份扫描结果同时喂给两个消费者」（例如同时写出与统计），同样需要主动推进较慢的那一路。

**第三，字典流水线里产出的字典必须可加。** 这是 `AddableDict` 反复出现的原因。主核里若做「分片结果累积」，对应的要求是「部分结果类型必须支持合并运算」，否则累加步骤只能靠手工的 `merge` 函数，而每个调用点都会写成不同版本。

还有一条更细的：`RunnablePick` 在「一个键都没命中」时返回 `None` 而不是空字典。这个选择把「空」和「空容器」区分开了——对下游的条件判定（真值判断）更友好，但要求调用方处理两种缺失形态。主核里做投影时同理：`SELECT` 出零列是一个需要明确语义的边界情况。
