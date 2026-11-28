---
project: ragas
feature: async-utils
title: 异步工具与追踪
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/async_utils.py
    symbols: [is_event_loop_running, run_async_tasks, as_completed]
  - path: src/ragas/callbacks.py
    symbols: [RagasTracer, MetricTrace, parse_run_traces]
---

# 异步工具与追踪

## 一句话职责

两件互不相干但同样「支撑运行时」的事：一半是在**已有事件循环里再跑 asyncio**这件麻烦事的解法（外加把一批协程按并发上限跑完并回收结果），另一半是把 LangChain 的 chain 回调攒成一棵树、再按「行 → 指标 → 提示」摊平成可读的追踪记录。

## 机制是什么

**异步这半边的核心难题只有一句话：`asyncio.run()` 不能在已在运行的事件循环里被调用**（会抛 `RuntimeError: This event loop is already running`）。而 ragas 的三个典型入口都会撞上它：Jupyter/Colab 里用户直接调同步 API、Web 服务里在异步 handler 里调同步评测、以及指标内部为了「同步外观」而调 `run(_async_wrapper)`。

解法是 `apply_nest_asyncio()`：检测到循环在跑就 `import nest_asyncio` 并 `apply()`，把 `asyncio.run` / `loop.run_until_complete` 打补丁成可嵌套。它**不是无条件的**——对 uvloop 这类非 asyncio 的循环，补丁打不上，于是返回 `False` 并在后续由调用方转成一个明确的错误。

**追踪这半边的骨架是「每个 chain 一个 `ChainRun`」**：`RagasTracer` 是 `BaseCallbackHandler` 子类，在 `on_chain_start` 里为每个 run 记一条 `ChainRun`（含 `run_id`、`parent_run_id`、`name`、`inputs`、`metadata`、`children`），在 `on_chain_end` 里补 `outputs`；父子关系靠「子启动时把 id append 到父的 `children`」。最后 `parse_run_traces` 按这棵树摊平：根链的子是「行」、行的子是「指标」、指标的子是「提示」。

## 怎么实现的

**`is_event_loop_running()` 用异常判定而不是状态查询。** `asyncio.get_running_loop()` 在没有运行循环时抛 `RuntimeError`，捕到就返回 `False`；否则返回 `loop.is_running()`。这是标准做法，也是唯一可靠的做法——没有「当前线程是否在跑循环」的公开 API。

**`apply_nest_asyncio()` 有五层判断，顺序值得逐条看。**

1. 没有运行循环 → 返回 `False`（不需要打补丁，`asyncio.run` 可以用）；
2. `import nest_asyncio` 失败 → 抛 `ImportError`，消息直接说明「看起来你在 jupyter 类环境里，请 `pip install nest_asyncio`」。**这是一个主动诊断**：把「缺可选依赖」翻译成「你大概在 Jupyter」；
3. 取当前循环，按类型名判 uvloop（`"uvloop" in loop_type.lower() or "uvloop" in str(type(loop))`）→ 打一条 debug 日志并返回 `False`；
4. `nest_asyncio.apply()` 成功 → 返回 `True`；
5. 捕 `ValueError` 且消息含 `"Can't patch loop of type"` → debug 日志 + `False`；其它 `ValueError` 原样抛出。

第 3 与第 5 步是两道「预判 + 兜底」：先按类名猜 uvloop（省一次打补丁失败），再用异常消息兜住其它不可打补丁的循环类型。两者都只记 debug 日志——即**默认静默降级**，由调用方决定后果。

**`run()` 的契约是「能跑就跑，跑不了就明确说为什么」。** 先按 `allow_nest_asyncio` 尝试打补丁；把「函数或协程」统一成协程对象；然后**再检查一次**循环状态：若循环在跑且补丁没打上，抛 `RuntimeError`，消息里带出循环类型名与三条建议（换标准 asyncio 循环、或重构避免嵌套）。最后 `asyncio.run(coro)`。

注意这里的顺序：`apply_nest_asyncio()` **内部**已经检查过「循环是否在跑」，`run()` 又检查一次。第二次检查的价值在于区分两种失败：补丁打成功 → 直接跑；补丁没打上（uvloop 或缺包已被上面的 `ImportError` 拦掉）→ 给出一条能照做的错误。**这条错误消息是整页最有用的东西**：它把「RuntimeError: This event loop is already running」这种对用户无信息量的话，换成了「你用的是 uvloop，它不支持嵌套」。

**`as_completed()` 是「并发上限 + 取消检查」的包装。** 签名 `(coroutines, max_workers=-1, *, cancel_check=None, cancel_pending=True) -> Iterator[Future]`：

- `max_workers == -1` → 每个协程直接 `create_task`；
- 否则建一个 `asyncio.Semaphore(max_workers)`，把每个协程包进 `async with semaphore: return await coro`，再 `create_task`。

然后 `asyncio.as_completed(tasks)` 得到「按完成顺序吐出 future」的迭代器。若没传 `cancel_check` 就直接返回它；传了则外面套一个生成器：**每吐一个之前先问一次「该停了吗」**，该停就（默认）把所有未完成任务 `cancel()` 然后 `break`。

**这个取消是「协作式的、在消费边界」。** 协程内部无法被这个机制打断，只有下一个 future 完成时才有机会检查。若一批任务里有一个特别慢，取消也会拖到它完成——除非调用方自己另有超时。

**`process_futures()` 把 future 迭代器变成异步生成器。** 逐个 `await`，`asyncio.CancelledError` 原样重抛（保证真正取消能穿透），其它异常被**当成结果**yield 出去（`result = e`）。所以错误不会在这里被吞掉，而是变成流里的一个值——由消费方判断是「记日志继续」还是「立即抛」。

**`run_async_tasks()` 在同步外观下做三件事：分批、进度、首错。** 内部定义 `_run()` 协程，然后 `return run(_run)`。`_run()` 分两条路：

- 不分批：一个进度条，`async for result in process_futures(as_completed(tasks, max_workers, cancel_check))`，异常记 `logger.error` 并**只留第一个**到 `first_exception`，所有结果（含异常对象）都 append 进 `results`；
- 分批：`batched(tasks, batch_size)`，用 `create_nested_bars` 造外层/内层两条进度条，逐批同样的处理，外层进度按批大小推进。

最后 `if first_exception is not None: raise first_exception`——**先把所有任务跑完，再抛第一个错误**。这解释了为什么错误要先「记下来」：早抛会让剩余任务白跑，晚抛能让用户一次看到完整结果与失败原因。返回值是 `results` 列表，**顺序不保证**（docstring 明写「Order of results is not guaranteed!」）。

**`new_group()` 是追踪与评测的接缝。** 它把「一串回调」升级成一对 run manager：`CallbackManager.configure(...)` 建 manager、设 tags/metadata、`on_chain_start(...)` 起根 run，再用 `rm.get_child()` 的 handlers 造一个 `CallbackManagerForChainGroup`。返回 `(rm, group_cm)`：前者用于结束时 `on_chain_end`/`on_chain_error`，后者作为**子级的回调入口**往下传。评测里就是靠这个把「一次评测 → 每行 → 每指标」串成三层。

**`ChainRun` 是 pydantic 模型而非 dataclass，`children` 默认空列表、`outputs` 默认空字典。** `on_chain_start` 先建条目（此时 `outputs` 为空），再在**父条目存在时**把子 id 追加到父的 `children`。这里有一个隐含前提：**父的 `on_chain_start` 必须先于子发生**，否则父条目还不存在、子 id 会被丢掉。LangChain 的回调顺序保证了这一点。

**`ChainRunEncoder` 两个特例：`uuid.UUID` → `str`，`ChainType` → `.value`。** 后者是因为 metadata 里存了链类型枚举（`{"type": ChainType.EVALUATION}`），直接 `json.dumps` 会失败。有一条被注释掉的 `EvaluationResult` 分支，说明曾经想序列化整个结果对象、后来放弃了。

**`parse_run_traces` 是「按树摊平 + 两个断言」。** 传入全部 `ChainRun` 与根 run_id：

1. 过滤出 `parent_run_id == parent_run_id 参数` 的条目当根；
2. **若根多于一条就抛 `ValueError`**，文案是「Multiple root traces found! This is a bug on our end, please file an issue」——把内部不一致直接暴露成 bug；
3. 然后**直接取 `root_traces[0]`**：若一条根都没有，这里是 `IndexError` 而不是友好的错误。

摊平的形状是 `List[MetricTrace]`，每个 `MetricTrace` 对应一行：`scores[指标名] = metric_trace.outputs.get("output", {})`，同时 `metric_traces[指标名] = {提示名: {"input": ..., "output": ...}}`。

**`MetricTrace` 是一个 `dict` 子类 + dataclass，这个组合很反常。** `@dataclass class MetricTrace(dict)` 声明了 `scores` 字段，实际用法却是 `metric_traces.scores[...] = ...` 与 `metric_traces[f"{name}"] = prompt_traces` **两种索引并存**：`.scores` 是 dataclass 字段（普通 dict），`[...]` 是 dict 基类的 `__setitem__`。`__repr__`/`__str__` 被覆写成只打印 `.scores`，于是**方括号里的提示 IO 在打印时看不见**——只有分数会显示。

**提示 IO 的取值有两处兼容处理。** `output` 先 `.get("output", {})`，若结果是 list 就取 `[0]`——这是为「同一次提示调用产生了多条候选」准备的。`input` 取 `prompt_trace.inputs.get("data", {})`：键名写死为 `data`，与 `PydanticPrompt` 记录输入时的形状耦合。

## 关键取舍

**源码事实：** `asyncio.run` 不能嵌套是硬限制；`apply_nest_asyncio` 对 uvloop 明确跳过并用 debug 日志静默降级，`run()` 再把它转成带循环类型的 `RuntimeError`。

**源码事实：** `nest_asyncio` 缺失时抛 `ImportError` 且文案直接猜测「你在 jupyter 类环境」，等于把可选依赖诊断前置。

**源码事实：** `as_completed` 的取消检查发生在「每取一个 future 之前」，且只 `cancel()` 未完成任务、不等待；慢任务会推迟取消生效。

**源码事实：** `run_async_tasks` 先把所有任务跑完（异常记日志并只保留第一个），最后才抛第一个异常；返回顺序不保证（docstring 明写）。

**源码事实：** `parse_run_traces` 在多根时抛带「please file an issue」的 `ValueError`，在零根时落成 `IndexError`。

**源码事实：** `MetricTrace` 同时是 dict 子类与 dataclass，`.scores` 与 `[...]` 两套索引并存，`__repr__` 只打印 `.scores`。

**源码事实：** `parse_run_traces` 里提示输出的键 `"output"` 与输入的键 `"data"` 是硬编码的，与 prompt 层的记录形状耦合。

**架构推论：** 「在已有循环里跑异步」这件事没有干净解法，只有三档取舍：报错让用户改代码（最诚实）、打补丁（nest_asyncio 的做法，改全局 `asyncio.run` 行为）、或另起线程跑事件循环（隔离但难取结果）。ragas 选了第二档并做了兼容性降级——这在「用户可能在任何环境里调用」的库场景里是务实选择，代价是**全局补丁会影响进程内所有 asyncio 使用者**。

**架构推论：** `run_async_tasks` 的「跑完再抛首错」是典型的批处理契约：宁可多花时间也不丢结果。这也意味着**失败不会短路**——一百个任务里第一个就失败，剩下九十九个仍会全部执行（并各自计费）。对按量计费的 LLM 评测，这个契约的成本是实的。

**架构推论：** 追踪树的价值在于把「三层嵌套的执行」变成可读的平铺记录，但它的实现强依赖 LangChain 回调的启动顺序与 run_id 传播。`parse_run_traces` 里那句「please file an issue」暴露的是**内部结构假设被违反时的无措**——这类「框架自己的树结构自己摊不平」的情况，在多回调、并发、异常路径下最容易发生。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/async_utils.py` | `is_event_loop_running` | 用 `get_running_loop` 抛异常来判定 |
| `src/ragas/async_utils.py` | `apply_nest_asyncio` | 五层判断：无循环 / 缺包 / uvloop / 打补丁 / 异常兜底 |
| `src/ragas/async_utils.py` | `run` | 统一协程入口；打不上补丁时抛带循环类型的错误 |
| `src/ragas/async_utils.py` | `as_completed` | 并发上限（信号量）+ 消费边界取消检查 |
| `src/ragas/async_utils.py` | `process_futures` | future → 异步生成器，异常变成结果值 |
| `src/ragas/async_utils.py` | `run_async_tasks` | 分批/进度/首错延后抛出，顺序不保证 |
| `src/ragas/callbacks.py` | `new_group` | 造一对 run manager，把回调串成三层 |
| `src/ragas/callbacks.py` | `RagasTracer` | 每个 chain 记一条 `ChainRun`，父子靠 children |
| `src/ragas/callbacks.py` | `ChainRun` | 追踪条目：run_id、父子、输入输出、元数据 |
| `src/ragas/callbacks.py` | `ChainRunEncoder` | `UUID`/`ChainType` 的序列化特例 |
| `src/ragas/callbacks.py` | `MetricTrace` | dict 子类 + dataclass，两套索引并存 |
| `src/ragas/callbacks.py` | `parse_run_traces` | 按树摊平成「行 → 指标 → 提示」记录 |
| `src/ragas/callbacks.py` | `ChainType` | 四类链：评测、指标、行、提示 |
| `src/ragas/evaluation.py` | `RagasTracer` 与 `new_group` 的接线 | 评测主流程如何建三层链并收集 traces |
| `src/ragas/dataset_schema.py` | `EvaluationResult.traces` | 用 `parse_run_traces` 把 traces 摊平挂到结果上 |

## 参考

- [ragas 源码：async_utils.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/async_utils.py)
- [ragas 源码：callbacks.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/callbacks.py)
- [nest_asyncio 仓库](https://github.com/erdewit/nest_asyncio)
- [Python 文档：asyncio](https://docs.python.org/3/library/asyncio.html)

## 与主核 / AI 的关系

「在已有事件循环里再跑异步」在主核里没有对应物，但有一个同构的问题：**在已有事务里开子事务**。两者都是「嵌套使用一个本不允许嵌套的资源」，解法也都是三档——拒绝、打补丁（模拟嵌套）、或另起上下文（真隔离）。主核选了「真隔离」（子事务用独立上下文 + savepoint），ragas 选了「打补丁」。补丁的好处是调用方代码不用改，坏处是**资源语义变了**：打补丁后 `asyncio.run` 不再保证「跑完就清理循环」，这会影响依赖该假设的其它库。

对 AI 工程四条结论。第一，**同步外观的异步 API 必须有明确的失败模式**：`run()` 那条「你用的是 uvloop」的错误消息，比任何兜底都值钱。第二，**并发上限要落在起任务之前**：这里用信号量包协程再 `create_task`，是正确顺序；先起后限会导致全部任务立刻占住内存与连接。第三，**取消只在消费边界生效**：批量取消不能指望立刻停止，慢任务会拖住它。第四，**「跑完再抛首错」要算成本**：对计费 API，失败不短路意味着钱照花。

关于追踪这半边，一条判断：看一个框架的 trace 能不能用，不看它记了多少字段，而看**它能否在异常路径上仍然完整**。这里的 `parse_run_traces` 在零根时落成 `IndexError`、在多根时抛「please file an issue」，说明异常路径维护得比正常路径薄得多——这是所有自建追踪体系的通病，也是排查线上问题时最先崩的一环。
