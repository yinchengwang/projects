---
project: langchain
feature: branch-and-router
title: 分支、路由与回退
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/runnables/branch.py
    symbols: [RunnableBranch]
  - path: libs/core/langchain_core/runnables/router.py
    symbols: [RouterRunnable, RouterInput]
  - path: libs/core/langchain_core/runnables/fallbacks.py
    symbols: [RunnableWithFallbacks]
---

# 分支、路由与回退

## 一句话职责

三个作用在不同位置的选择器：`RunnableBranch` 按**顺序求值一组条件**、命中即走对应分支（末位是默认分支）；`RouterRunnable` 按输入里显式的 `key` **查表**选中一个子管道；`RunnableWithFallbacks` 则不在正常路径上做选择，而是**在失败时逐个换人重试**。

## 机制是什么

「选择」这件事在这里有三种形态，区别是**判定依据从哪来**：

| | 依据 | 求值时机 | 未命中怎么办 |
|---|---|---|---|
| `RunnableBranch` | 一组条件函数 / 管道，按序运行 | 运行期，逐条求值 | 落到末位默认分支 |
| `RouterRunnable` | 输入字典里的 `key` 字段 | 运行期，一次字典查找 | 直接 `ValueError` |
| `RunnableWithFallbacks` | 上一个候选抛出的异常类型 | 运行期，异常处 | 全部失败则抛首个错误 |

三者的共同点只有一条：都是**在运行期用实际数据决定走哪条路**，所以它们都必须在「逐条调用原语」之外另立类，而不能写成组合器的语法糖。

`RunnableBranch` 的形状是一个特殊情况：它把**最后一个位置**留给默认分支，前面的位置才是「条件 + 分支」对。于是构造参数是一个混合序列——前面若干 2 元组，最后一个裸 `RunnableLike`。这个设计让「必须有兜底」成为构造期的不变量，而不是运行时的 `None` 判断。

## 怎么实现的

**`RunnableBranch` 在构造期就把「至少两个分支、首个是二元组、末位是裸分支」全部校验掉。**

```
if len(branches) < _MIN_BRANCHES:
    msg = "RunnableBranch requires at least two branches"
    raise ValueError(msg)

default = branches[-1]
if not isinstance(default, (Runnable, Callable, Mapping)):
    msg = "RunnableBranch default must be Runnable, callable or mapping."
    raise TypeError(msg)
default_ = coerce_to_runnable(cast("Runnable[Input, Output]", default))

for branch in branches[:-1]:
    if not isinstance(branch, (tuple, list)):
        raise TypeError(f"RunnableBranch branches must be tuples or lists, not {type(branch)}")
    if len(branch) != _MIN_BRANCHES:
        raise ValueError(f"RunnableBranch branches must be tuples or lists of length 2, not {len(branch)}")
    condition, runnable = branch
    condition = cast("Runnable[Input, bool]", coerce_to_runnable(condition))
    runnable = coerce_to_runnable(cast("Runnable[Input, Output]", runnable))
    branches_.append((condition, runnable))
```

`_MIN_BRANCHES = 2` 是这个校验的核心：一个「只有默认分支」的 branch 在语义上等于它自己，毫无意义；而「一个条件 + 一个默认」是最小可用的形态。把这条写成常量并在构造期报错，比运行期发现「没得选」要好。

**条件被强制包装成 `Runnable` 而不是允许返回布尔值的裸函数。** `coerce_to_runnable(condition)` 会把函数包成 `RunnableLambda`，于是条件也能是一个真正的管道（例如「先取某个字段再判空」）。代价是条件必须返回 `bool` 语义的值——源码用 `if expression_value:` 判断，即 Python 的真值判断而非严格的 `is True`。

**求值循环用 `for ... else` 实现「未命中落到默认」，这是本页最值得看的一处语法用法。**

```
try:
    for idx, branch in enumerate(self.branches):
        condition, runnable = branch
        expression_value = condition.invoke(
            input,
            config=patch_config(config, callbacks=run_manager.get_child(tag=f"condition:{idx + 1}")),
        )
        if expression_value:
            output = runnable.invoke(
                input,
                config=patch_config(config, callbacks=run_manager.get_child(tag=f"branch:{idx + 1}")),
                **kwargs,
            )
            break
    else:
        output = self.default.invoke(
            input,
            config=patch_config(config, callbacks=run_manager.get_child(tag="branch:default")),
            **kwargs,
        )
except BaseException as e:
    run_manager.on_chain_error(e)
    raise
run_manager.on_chain_end(output)
```

`for ... else` 的语义正好是「循环没有被 `break` 打断才执行」——于是 `else` 分支天然就是「所有条件都为假」的落点，不需要额外维护一个 `matched` 标志。这个写法在 Python 里出了名的少用，但在这里确实是最贴的。

**每个条件与每个被选中的分支都有自己的子 run 标签**：`condition:{idx+1}` 与 `branch:{idx+1}`。于是观测面板上能直接看出「第 2 个条件为真、走了第 2 个分支」，而默认分支用固定标签 `branch:default` 区分。

**注意条件与分支都对 `**kwargs` 的处理不同**：条件调用不带 `kwargs`（`condition.invoke(input, config=...)`），分支调用带（`runnable.invoke(input, config=..., **kwargs)`）。这是刻意的——`kwargs` 是给业务步骤的参数（如模型的 `stop`），条件只是个判定器，不该收到。

**`stream` 版本的实现多了一份「怎么拼出完整输出」的记账。**

```
final_output: Output | None = None
final_output_supported = True
for idx, branch in enumerate(self.branches):
    ...
    if expression_value:
        for chunk in runnable.stream(input, config=..., **kwargs):
            yield chunk
            if final_output_supported:
                if final_output is None:
                    final_output = chunk
                else:
                    try:
                        final_output = final_output + chunk
                    except TypeError:
                        final_output = None
                        final_output_supported = False
        break
else:
    for chunk in self.default.stream(...):
        ... 同一段记账 ...
```

`final_output` 要在流式过程中不断累加，以便跑完后 `on_chain_end(final_output)` 有完整值可报（trace 与回调需要它）。但**很多输出类型根本不能相加**（一个 `dict` 与另一个 `dict`、一个自定义对象），所以一旦 `+` 抛 `TypeError` 就置 `final_output_supported = False` 并永久放弃累加——后续块只 `yield`，不再尝试相加。

这比「不报最终值」和「抛异常」都好：前者让 trace 里这条 run 没有输出，后者让一次本来可以成功的流式调用失败。真实原因是「能不能相加」是一种**运行期才知道的能力**——它既不是输入类型决定的（`dict` 在别处可能用 `AddableDict` 就能加），也不在类的能力声明里。

`astream` 是 `stream` 的异步镜像，逻辑逐行对应。`ainvoke` 是 `invoke` 的异步镜像。

**`RunnableBranch` 没有实现 `transform`，而 `stream` 是逐块转发的。**

所以它不参与「流式在链中传递」这件事：`branch.stream(x)` 会走完条件、开始跑选中分支的 `stream`，然后把块逐个 `yield` 出去——这看起来已经是流式了，但 `transform` 缺失意味着它**在 `RunnableSequence` 里是一个「流式断点」**：上游的块不会被逐个喂进来，而是等上游全部完成后才进入 branch。这与 a01 里说的「lambda 默认不实现 transform，放在链中就切断流」是同一条机制。

**`RouterRunnable` 的输入是一个 `TypedDict`，两个字段各司其职。**

```
class RouterInput(TypedDict):
    key: str       # 用来路由的键
    input: Any     # 传给被选中 Runnable 的输入
```

注意 `input` 字段名与「这个管道自己的输入」同名，容易看混：外层调用 `router.invoke({"key": "square", "input": 3})` 时，被选中的 `square` 收到的是裸 `3`，不是那个字典。**路由层把「选择依据」与「业务输入」做了分离**——这是它与 `RunnableBranch` 的结构性差别：branch 用整份输入既判定又处理，router 用输入的一半做判定、另一半做处理。

选中的过程没有任何复杂度：

```
key = input["key"]
actual_input = input["input"]
if key not in self.runnables:
    msg = f"No runnable associated with key '{key}'"
    raise ValueError(msg)
runnable = self.runnables[key]
return runnable.invoke(actual_input, config)
```

`f"...'{key}'"` 把实际键值写进错误消息——键拼错时这条消息是唯一线索。而 `config` 是**原样透传**的，没有 `get_child` 包裹，也就是说被选中的子管道不是「子 run」而是「同一个 run 的延续」。差异很实际：`branch` 会在 trace 里多出一层，`router` 不会。源码没有解释这个不一致，但它与两个类的定位相符（router 更像「表驱动分派」，branch 更像「组合器」）。

**`RouterRunnable` 覆盖了 `batch` / `abatch` / `stream` / `astream`，方法表里没有 `transform`。**

`batch` 的实现是一个「先全部校验再并发跑」的两段：

```
keys = [input_["key"] for input_ in inputs]
actual_inputs = [input_["input"] for input_ in inputs]
if any(key not in self.runnables for key in keys):
    msg = "One or more keys do not have a corresponding runnable"
    raise ValueError(msg)

runnables = [self.runnables[key] for key in keys]
configs = get_config_list(config, len(inputs))
with get_executor_for_config(configs[0]) as executor:
    return list(executor.map(invoke, runnables, actual_inputs, configs))
```

**先做全局校验再开线程池**，于是「某个键不存在」这条错误在开始执行之前就抛出来，不会出现「一半输入跑完了才发现第三条键拼错」。这比逐个 `invoke` 时才报错要好得多——后者会留下部分副作用。

注意错误消息从单条的 `No runnable associated with key 'x'` 变成了批量的 `One or more keys do not have a corresponding runnable`，即**在批量路径上丢掉了具体键名**。这是为了不在错误消息里刷一长串键，但代价是排查时要自己去找。

`stream` 是纯转发：

```
yield from runnable.stream(actual_input, config)
```

`async for` 版本同理。于是 router 完全保持了被选中管道的流式能力——这一点与 branch 不同（branch 的 stream 是它自己逐块转发，但缺 `transform`）。

**`RunnableWithFallbacks` 的核心是「谁算作失败」这条判定，以及它被应用在哪一层。**

```
exceptions_to_handle: tuple[type[BaseException], ...] = (Exception,)
```

默认捕 `Exception`（不捕 `BaseException`，于是 `KeyboardInterrupt` / `SystemExit` 会穿透）。注释写明「任何不是这些异常子类的异常会立即抛出」——这条在实现里体现为两段式的 `except`：

```
except self.exceptions_to_handle as e:
    if first_error is None:
        first_error = e
    last_error = e
except BaseException as e:
    run_manager.on_chain_error(e)
    raise
else:
    run_manager.on_chain_end(output)
    return output
```

**先捕可处理的、再捕其余并立刻抛。** 顺序不能反：`BaseException` 是所有异常的基类，写在前面就吞掉了一切。

**`first_error` 与 `last_error` 这两个变量各表达一件事。** `last_error` 用来「把上一个错误塞进下一次尝试的输入」（配合 `exception_key`），`first_error` 用来「全部失败时抛哪一个」——代码选择**抛第一个**：

```
if first_error is None:
    msg = "No error stored at end of fallbacks."
    raise ValueError(msg)
run_manager.on_chain_error(first_error)
raise first_error
```

抛第一个而不是最后一个，是有道理的：第一个错误来自**首选方案**，它最能说明「为什么这次调用失败」。若抛最后一个，拿到的是「最强兜底方案也挂了」的错误——那通常是个级联症状而不是根因。

`exception_key` 那一段把错误喂回输入：

```
if self.exception_key and last_error is not None:
    input[self.exception_key] = last_error
```

于是兜底管道能看到「上一次为什么失败」（例如让兜底模型看到「前一个 provider 返回了 429」并据此降级回答）。但这条能力有前置约束，且校验放在方法开头：

```
if self.exception_key is not None and not isinstance(input, dict):
    raise ValueError("If 'exception_key' is specified then input must be a dictionary. ...")
```

**在 `invoke` / `ainvoke` / `batch` / `abatch` / `stream` / `astream` 里各写一次这条校验**，批量版本是 `all(isinstance(input_, dict) for input_ in inputs)`。这条要求是合理的（要往输入里加键就必须能变异），但它意味着 `exception_key` 与「输入不是字典」这个组合只在运行期才会被发现——构造期无法校验。

还有一处隐蔽行为：`input[self.exception_key] = last_error` **就地变异调用方的输入对象**。于是调用方在 `invoke` 之后会发现自己传进去的那个 dict 被加了 `exception_key`。源码注释 `# type: ignore[index]` 只说明类型问题，没提这个副作用。若同一个输入对象被复用（例如在循环里），上一次的错误会残留到下一次。

**`set_config_context` 在这里的作用是「让兜底管道看到正确的父级」。**

```
child_config = patch_config(config, callbacks=run_manager.get_child())
with set_config_context(child_config) as context:
    output = context.run(runnable.invoke, input, config, **kwargs)
```

注意 `context.run` 的实参里传的是 **`config` 而不是 `child_config`** —— 也就是说回调上下文用的是父 config，而 `ContextVar` 里放的是 `child_config`。这个不一致看起来像疏漏，但它有实际后果：被包装的管道若从 `ContextVar` 读 config，会拿到 `child_config`（带子 run）；若从显式实参读，会拿到 `config`。这类不一致在主核里也会出现，且极难在测试里抓到。

异步版本用 `coro_with_context(coro, context)` 补上同一条上下文传递（在 a02 里讲过同样的手法）。

**批量版是这一页最复杂的实现：它把「逐条隔离」做到了每一轮。**

```
to_return: dict[int, Any] = {}
run_again = dict(enumerate(inputs))
handled_exceptions: dict[int, BaseException] = {}
first_to_raise = None
for runnable in self.runnables:
    outputs = runnable.batch(
        [input_ for _, input_ in sorted(run_again.items())],
        [patch_config(configs[i], callbacks=run_managers[i].get_child()) for i in sorted(run_again)],
        return_exceptions=True,
        **kwargs,
    )
    for (i, input_), output in zip(sorted(run_again.copy().items()), outputs, strict=False):
        if isinstance(output, BaseException) and not isinstance(output, self.exceptions_to_handle):
            if not return_exceptions:
                first_to_raise = first_to_raise or output
            else:
                handled_exceptions[i] = output
            run_again.pop(i)
        elif isinstance(output, self.exceptions_to_handle):
            if self.exception_key:
                input_[self.exception_key] = output
            handled_exceptions[i] = output
        else:
            run_managers[i].on_chain_end(output)
            to_return[i] = output
            run_again.pop(i)
            handled_exceptions.pop(i, None)
    if first_to_raise:
        raise first_to_raise
    if not run_again:
        break
```

三个映射表各管一件事：`run_again` 是「还得再试的输入」（原下标 → 输入），`to_return` 是「已成功」（原下标 → 输出），`handled_exceptions` 是「被捕获但没成功」（原下标 → 异常）。

每一轮只把 `run_again` 里的下标挑出来交给下一个候选，于是**已经成功的输入不会被重复执行**——这是它与 `RunnableSequence.batch` 那种「失败即整批失败」策略的根本区别。它也意味着「不同输入可能被不同候选处理」：第 1 条可能由首选成功，第 2 条一路兜底到最后。

三种结果的处理值得逐条看：

1. **不可处理的异常**：`return_exceptions=False` 时记入 `first_to_raise`（并在本轮末尾立刻抛）；`return_exceptions=True` 时也记进 `handled_exceptions` 并从 `run_again` 移除——注意这里把「不可处理」也当成「这条输入结束了」，因为再换候选也不会好。
2. **可处理的异常**：`exception_key` 存在时把它塞进该输入（就地变异），记入 `handled_exceptions`，**但保持在 `run_again` 里**——下一轮继续试下一个候选。这个「同时记冤枉又继续试」的写法是必要的：如果中途没有更多候选了，它就是最终要报的错误。
3. **成功**：报 `on_chain_end`、写入 `to_return`、从 `run_again` 移除，并且 `handled_exceptions.pop(i, None)` 清掉历史上的失败痕迹。

循环末尾的 `if first_to_raise: raise` 与 `if not run_again: break` 各有含义：前者保证「不该处理的异常立刻终止而不是继续试别的候选」，后者是「全部输入都有结论了就提前收工」。

收尾：

```
sorted_handled_exceptions = sorted(handled_exceptions.items())
for i, error in sorted_handled_exceptions:
    run_managers[i].on_chain_error(error)
if not return_exceptions and sorted_handled_exceptions:
    raise sorted_handled_exceptions[0][1]
to_return.update(handled_exceptions)
return [output for _, output in sorted(to_return.items())]
```

**先给所有失败输入补发 `on_chain_error`，再在非 `return_exceptions` 模式下抛最小的那个下标对应的错误**（`sorted(...)[0]` 即下标最小者，而非「第一个发生的」——在批量并发下这两者本来就不同）。`return_exceptions=True` 时把异常对象并进结果列表，并按原下标排序返回。

**`__getattr__` 让 fallbacks 具备「穿透式方法调用」的能力。**

```
attr = getattr(self.runnable, name)
if _returns_runnable(attr):
    @wraps(attr)
    def wrapped(*args, **kwargs):
        new_runnable = attr(*args, **kwargs)
        new_fallbacks = []
        for fallback in self.fallbacks:
            fallback_attr = getattr(fallback, name)
            new_fallbacks.append(fallback_attr(*args, **kwargs))
        return self.__class__(**{**self.model_dump(), "runnable": new_runnable, "fallbacks": new_fallbacks})
    return wrapped
return attr
```

判定靠**返回类型注解**：

```
def _returns_runnable(attr):
    if not callable(attr):
        return False
    return_type = typing.get_type_hints(attr).get("return")
    return bool(return_type and _is_runnable_type(return_type))
```

而 `_is_runnable_type` 递归处理三类：类（`issubclass(type_, Runnable)`）、泛型别名（看 `__origin__`）、以及 `Union`（要求所有成员都是）。于是 `def bind_tools(...) -> Runnable[Any, Any]` 这类注解会被识别出来。

效果是：`model.with_fallbacks([alt]).bind_tools([...])` 会对**首选与每一个兜底**都调一次 `bind_tools`，返回一个新的 fallbacks 包装。没有这个机制，用户就必须手写 `model.bind_tools(...).with_fallbacks([alt.bind_tools(...)])`。

这个设计的风险也很实在：它依赖类型注解的正确性。若某个第三方实现把返回类型写成 `Any`（或干脆没注解），`get_type_hints` 拿不到 Runnable 类型，于是 `wrapped` 不会被生成、直接返回首个候选的方法——一次只在首选上生效的调用，静默地丢失了兜底侧的对应调用。这类「能力声明依赖注解」的机制在 Python 生态里少见但有效，前提是注解可靠。

**`__getattr__` 的兜底语义也有边界**：非方法属性（如 `model.model_name`）直接返回首选的属性值，不尝试在兜底上找。这符合直觉（兜底是备胎，不是并列实例），但它意味着「读属性会掩盖兜底之间的差异」。

## 关键取舍

**源码事实：** `RunnableBranch` 在构造期校验「至少 2 个分支」、「末位必须是 `Runnable` / 可调用 / 映射」、「前面的每一项必须是长度为 2 的 tuple 或 list」，并把条件与被选分支都 `coerce_to_runnable`。

**源码事实：** `RunnableBranch` 用 `for ... else` 实现「所有条件为假时走默认分支」；每个条件带 `condition:{idx+1}` 子 run、被选分支带 `branch:{idx+1}`、默认分支带 `branch:default`；条件调用不带 `**kwargs`，分支调用带。

**源码事实：** `RunnableBranch.stream` 在流式过程中累加 `final_output` 供 `on_chain_end` 使用，一旦 `+` 抛 `TypeError` 就置 `final_output_supported = False` 并停止累加（块仍照常 `yield`）。

**源码事实：** `RunnableBranch` 未实现 `transform`，因此在 `RunnableSequence` 中会成为流式断点。

**源码事实：** `RouterInput` 把「路由键」与「业务输入」分成两个字段，被选中管道收到的是裸的业务输入；`config` 原样透传、不套 `get_child`。

**源码事实：** `RouterRunnable.batch` 先对所有键做全局校验再开线程池，批量路径的错误消息只说「一个或多个键不存在」而不列出具体键；`stream` / `astream` 是纯转发，保住被选中管道的流式能力。

**源码事实：** `RunnableWithFallbacks` 默认只处理 `Exception`（不处理 `BaseException`），且用「先捕可处理、再捕其余并立刻抛」的两段 `except`。

**源码事实：** 全部候选失败时抛**第一个**错误（`first_error`）；`exception_key` 存在时把上一个错误就地写进输入字典，且该模式要求输入是字典（六处方法各写一次校验）。

**源码事实：** `RunnableWithFallbacks.batch` 用 `run_again` / `to_return` / `handled_exceptions` 三张按下标索引的表，做到「已成功的输入不重复执行、可处理异常继续换候选、不可处理异常立刻终止」；收尾按最小下标抛错。

**源码事实：** `__getattr__` 通过 `typing.get_type_hints` 的返回注解判断该方法是否返回 `Runnable`；是则对首选与每个兜底都调用一次并重新包装。

**架构推论：** 「默认分支占末位」把「必须有兜底」从运行期检查提升为构造期不变量。对照 `RouterRunnable`（键缺失就 `ValueError`），两者对「未命中」的处理方向相反——branch 认为未命中是常态（因此强制兜底），router 认为未命中是配置错误（因此报错）。这个差异反映的是「条件判定容易漏」与「键查表应当完备」两种不同的预期。

**架构推论：** `final_output_supported` 这种「运行期探测能力、失败后降级」的写法，本质是在补一个类型系统看不到的缺口：能不能相加取决于值的运行时类型。凡是把「累加」当成通用契约的地方都有这个缺口。

**架构推论：** 批量 fallbacks 的「逐条隔离」比「整批失败」昂贵得多（每轮都要重排输入、维护三张表），换来的是「一条输入失败不连累其他」。这个取舍只在批量规模不大、失败率不低时才划算——大规模批量下，重试带来的重复调用成本可能超过隔离带来的收益。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/runnables/branch.py` | `RunnableBranch` | 顺序求值条件、命中即走、末位为默认分支；构造期校验形状 |
| `libs/core/langchain_core/runnables/branch.py` | `RunnableBranch.stream` / `astream` | 逐块转发并在可累加时记账，不可累加则降级 |
| `libs/core/langchain_core/runnables/router.py` | `RouterInput` | 路由键与业务输入分离的两字段 TypedDict |
| `libs/core/langchain_core/runnables/router.py` | `RouterRunnable` | 按 key 查表分派；批量先全局校验；stream 纯转发 |
| `libs/core/langchain_core/runnables/fallbacks.py` | `RunnableWithFallbacks` | 按异常类型逐个换候选；抛首个错误；`exception_key` 把错误喂回输入 |
| `libs/core/langchain_core/runnables/fallbacks.py` | `RunnableWithFallbacks.batch` / `abatch` | 三张下标表实现逐条隔离的兜底批量 |
| `libs/core/langchain_core/runnables/fallbacks.py` | `_returns_runnable` / `_is_runnable_type` | 靠返回注解判断方法是否产出 `Runnable`，支撑穿透式调用 |
| `libs/core/langchain_core/runnables/base.py` | `Runnable.get_graph` | 默认导出「输入 schema → 本节点 → 输出 schema」三点图，分支与回退都复用它 |

## 参考

- [langchain 源码：runnables/branch.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/branch.py)
- [langchain 源码：runnables/router.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/router.py)
- [langchain 源码：runnables/fallbacks.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/fallbacks.py)
- [Python 文档：`for` 语句的 `else` 子句](https://docs.python.org/3/tutorial/controlflow.html#break-and-continue-statements-and-else-clauses-on-loops)

## 与主核 / AI 的关系

「运行期按实际数据决定走哪条路」与主核里的**谓词下推 / 执行计划选择**是同一类问题：条件表达式、运行时统计、失败重试，三者都要在计划执行到那一步才知道答案。这里有四条经验。

**第一，`for ... else` 是正确的「未命中」表达。** 用一个布尔标志手写「是否匹配」的版本，错在把它初始化为 `False` 后再在循环里置真——一旦有人在 `break` 之前插入一行 `continue`，标志就会出错。`for ... else` 把这个状态交给语法。

**第二，「必须有兜底」要成为构造期不变量。** `RunnableBranch` 强制末位是默认分支，于是运行期不存在「没得选」的分支——决策表因此无法漏项。主核里若做「按类型分派处理器」，同样应当在注册期就要求一个 default，而不是在分派失败时抛异常。

**第三，失败换候选时抛哪个错误，是一个需要明确的选择。** 这里抛第一个（首选方案的错误），因为它最接近根因。很多重试框架抛最后一个，症状是「用户看到的错误来自最不相关的兜底服务」。主核里凡是「主路径失败后换备用路径」的地方，都应当明确记录并上报首个失败。

**第四，「穿透式方法调用」依赖注解，因此注解是契约的一部分。** `with_fallbacks(...).bind_tools(...)` 之所以能对每条候选生效，靠的是 `bind_tools` 的返回注解。主核若要对扩展实现做类似的方法镜像，就必须把「返回类型必须标注」写进实现规范——否则镜像会静默地只在主实现上生效，而这个缺陷不会有任何报错。
