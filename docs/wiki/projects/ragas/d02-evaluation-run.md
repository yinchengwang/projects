---
project: ragas
feature: evaluation-run
title: 评测执行与并发
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/evaluation.py
    symbols: [evaluate, aevaluate]
  - path: src/ragas/executor.py
    symbols: [Executor, run_async_batch]
---

# 评测执行与并发

## 一句话职责

`evaluate` / `aevaluate` 把「样本 × 指标」摊成一批异步任务交给 `Executor`，按提交顺序收齐结果后重新拼回「每行每指标」的分数表；单条任务失败默认折算成 `NaN` 而不是整体中断。

## 机制是什么

执行层要回答三个问题：任务怎么摊、结果怎么拼、失败怎么处理。

**任务摊平。** 两层循环：外层遍历样本，内层遍历指标。每个组合提交一个任务，任务名是「指标名-行号」。所以任务总数是 `样本数 × 支持该样本类型的指标数`，核酸顺序是「先按行、再按指标」，而这也是结果回填时索引的隐含约定。

**结果拼回。** `Executor` 给每个任务发一个**自增序号**，任务完成时把「序号 + 返回值」作为二元组产出；`aresults()` 先按序号排序再丢序号，于是结果列表严格按提交顺序排列。有了这个保证，回填就退化成一次算术：第 i 行第 j 个指标的值在 `results[len(metrics) * i + j]`。

**失败处理。** 两层都有开关，语义不同：

- 任务级：`wrap_callable_with_index` 捕获异常，`raise_exceptions=False`（默认）时记日志并返回 `np.nan`，同时**保留序号**——于是失败不破坏顺序对齐，只是那一格变成 NaN；设为真则原样抛出。
- 集合级：若 `aresults()` 返回空列表，主动抛 `ExceptionInRunner`。空列表意味着所有任务都没产出（通常是任务根本没被提交），这是一个「零结果」与「零任务」需要区别对待的场合。

## 怎么实现的

**两条入口与废弃声明。** `evaluate` 与 `aevaluate` 都在开头发 `DeprecationWarning`，指向 `@experiment` 装饰器这个新入口。两者的实际关系是：`aevaluate` 是异步实现；`evaluate` 只是给它的协程套一层运行器——`allow_nest_asyncio=True`（默认）时走 `ragas.async_utils.run`，为真时调 `apply_nest_asyncio()` 打补丁以便在 Jupyter 里嵌套运行；设为 `False` 则直接 `asyncio.run`。

所以「同步入口」不是一个独立实现，而是同一个协程的两种驱动方式。这也解释了为什么 `evaluate` 的模型类型注解比 `aevaluate` 少一项——它会把 LangChain 的模型包一层再交给异步实现。

**前置处理的固定顺序。** 依次是：校验指标类型（必须是 `Metric` 实例的列表，否则抛 `TypeError` 并给出正确写法示例）→ 若没给指标则装入四个默认指标（答案相关性、上下文精确率、忠实度、上下文召回率）→ 若数据是 HF Dataset 则先重映射列名、再做 v1 到 v2 的格式迁移、最后转成 `EvaluationDataset` → 两道校验（必需列与指标类型兼容性）→ 把 LangChain 模型包装成 ragas 包装器。

「默认四个指标」这个选择值得记一笔：它让 `evaluate(dataset)` 这种最简调用也能出结果，但同时把一组特定的 RAG 指标固化成了默认值。

**逐指标补依赖。** 遍历指标列表时按类型分派：

- `AspectCritic` 被记进二值指标名单（它的分数是 0/1，均值无意义）；
- `MetricWithLLM` 且 `llm is None` 时，若调用方没给 llm 就临时造一个默认模型，然后**回填到指标上**并记下该下标；
- `MetricWithEmbeddings` 且 `embeddings is None` 时，若调用方没给就**从 llm 推断供应商**再造嵌入，同样回填并记下标；
- `AnswerCorrectness` 若 `answer_similarity` 为空也记下标；
- 最后每个指标都调 `metric.init(run_config)`，这一步会把运行配置**下发到指标持有的模型对象上**（下文 d03 详述）。

回填是**就地改指标实例**，所以 `finally` 块里必须把改过的全部还原成 `None`——否则同一个指标对象被第二次用于评测时会带着上一次的模型，造成跨轮污染。这是全函数里唯一处理「副作用撤销」的地方。

**回调与追踪的装配。** 每个指标实例建一个 tracer；若给了 `token_usage_parser` 则再建一个成本回调。这些回调按传入的 `callbacks` 类型分两条路注入：若是 `BaseCallbackManager` 就 `add_handler`，否则当作列表 `append`。

追踪结构是两层的：先建一个「评测」组的 run manager，然后**每行动建一个子组**（名字「row i」，输入是整行样本、元数据带行号）。指标任务提交时拿到的就是这个行组作为父回调——于是轨迹的层级天然是「评测 → 行 → 指标」。

**按样本类型分派任务。** 拿到 `dataset.get_sample_type()` 后分两支：单轮样本只提交 `SingleTurnMetric` 实例的 `single_turn_ascore`，多轮样本只提交 `MultiTurnMetric` 实例的 `multi_turn_ascore`。两支都用列表推导提交，提交个数必须等于指标总数——这正是回填算式 `len(metrics) * i + j` 成立的前提。而它由前面那道「指标类型兼容性」校验保证：任何不支持当前样本类型的指标会在到达这里之前就抛错。

**结果回填与指标键名。** 每行的字典以指标名为键；`ModeMetric`（带模式参数的指标）的键会加上 `(mode=...)` 后缀，避免同一指标的两个模式互相覆盖。这个后缀只影响输出键名，不影响计算。

**提前返回执行器。** `return_executor=True` 时函数在任务全部提交之后立刻返回 `Executor` 对象，调用方可以调 `cancel()` 取消，或调 `results()` 取结果。注意此时所有的前置准备（补模型、建回调）已经执行完，而 `finally` 里的还原**尚未发生**——取消或取结果之后不会有人帮指标还原模型。这是这条路径的一处副作用泄漏。

**异常路径与正常路径的收尾不同。** 取结果阶段抛异常时，只把评测组的 run manager 标成 `on_chain_error` 后重新抛出；正常路径则在构造 `EvaluationResult` 之后标 `on_chain_end`，把分数一并记进轨迹。

**`finally` 的三件收尾。** 还原被回填模型/嵌入的所有指标、还原 `AnswerCorrectness` 的 `answer_similarity`、刷新分析上报的批处理队列。三件都在 `finally` 里，所以成功失败都会执行——除了上面提到的提前返回路径。

**并发执行器的两个循环。** `Executor` 的 `_process_jobs` 先把任务列表**拷贝并清空原列表**（防止重复执行），然后两条分支：

- 无批大小：直接一次性把所有协程交给 `as_completed`，配一个进度条；
- 有批大小：切成若干批，逐批处理，配「总进度 + 批进度」两层进度条；**每批开始前查一次取消状态**，取消则跳出循环（已提交的批继续跑完，后续批不再开始）。

`as_completed` 会给每个协程包一层信号量（`max_workers` 非负时），所以「并发上限」不是靠线程池，而是靠 `asyncio.Semaphore`。`max_workers == -1` 时不包信号量，全部并发。

**`run_async_batch` 是个便利包装。** 它建一个 `Executor`（`raise_exceptions=True`、不保留进度条、用一个全新的 `RunConfig`），把同一函数的不同参数字典逐个提交，再调同步的 `results()`。它不接受外部 `RunConfig`——所以重试与超时策略用不了调用方的配置，只能用默认值。

## 关键取舍

**源码事实：** 结果对齐靠「自增序号 + 排序」，而不是靠每个任务返回自己的行/列坐标。序号在提交时分配，失败时序号仍保留（值换成 NaN），所以**一格失败不会让后续格错位**。

**源码事实：** 默认 `raise_exceptions=False`，失败折算成 `NaN` 并继续跑；设成真则立刻抛出，但要注意抛出时其余任务的状态取决于取消逻辑，不是「全部完成后再报」。

**源码事实：** 两条入口都是废弃状态，新入口是 `@experiment` 装饰器。`evaluate` 存在的唯一理由是给异步实现套一层事件循环驱动，并保留 `allow_nest_asyncio` 这个 Jupyter 兼容开关。

**源码事实：** 回填到指标实例上的模型/嵌入在 `finally` 里被还原。这是为了支持「同一批指标对象反复用于多次评测」。

**源码事实：** `return_executor=True` 的路径跳过了 `finally` 的还原。提前返回是显式的设计，但副作用撤销没有跟上。

**架构推论：** 「样本 × 指标」全摊平而不是「每个样本串行跑它的指标」，使并发度不受样本数限制，而受 `max_workers` 限制。代价是同一个样本的多个指标可以并行，而它们往往共享同一份上下文——若指标内部有缓存还好，没有就重复做功。

**架构推论：** 批处理只影响「同时提交多少」，不影响结果语义。但每批结束才检查取消，所以取消的响应粒度是「一批」而不是「一个任务」。批越大，取消越迟钝。

**架构推论：** 把「补模型」与「还原模型」写成一对前后动作，而不是让指标自己懒加载，使得 `evaluate` 能统一控制所有指标的模型来源。代价是这段代码必须严格成对，任何提前返回都可能破坏配对——代码里确实存在这样一条路径。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/evaluation.py` | `aevaluate` | 异步主流程：前置处理、任务摊平、回填结果 |
| `src/ragas/evaluation.py` | `evaluate` | 同步入口，给异步实现套事件循环驱动 |
| `src/ragas/executor.py` | `Executor` | 异步任务执行器，序号对齐、进度条、取消 |
| `src/ragas/executor.py` | `Executor.submit` | 提交任务并分配自增序号 |
| `src/ragas/executor.py` | `Executor.wrap_callable_with_index` | 失败折算为 NaN 并保留序号 |
| `src/ragas/executor.py` | `Executor.aresults` | 按序号排序后返回结果 |
| `src/ragas/executor.py` | `Executor._process_batched_jobs` | 分批处理与逐批取消检查 |
| `src/ragas/executor.py` | `run_async_batch` | 同一函数多参数并行的便利包装 |
| `src/ragas/validation.py` | `validate_required_columns` | 必需列校验，决定哪些指标能跑 |
| `src/ragas/validation.py` | `validate_supported_metrics` | 样本类型与指标类型兼容性校验 |
| `src/ragas/async_utils.py` | `as_completed` | 用信号量限并发，支持取消检查 |

## 参考

- [ragas 源码：evaluation.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/evaluation.py)
- [ragas 源码：executor.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/executor.py)
- [ragas 文档：执行评测](https://docs.ragas.io/en/stable/getstarted/rag_eval/)
- [Python asyncio 文档：协程与任务](https://docs.python.org/3/library/asyncio-task.html)

## 与主核 / AI 的关系

**「失败折算成占位值而不是中断整批」与主核的批量导入取舍一致。** 数据库批量导入遇到坏行有两条路：整批回滚，或逐行记录错误继续。评测这边选了后者（默认 NaN），理由是评测的价值在于「看清整体分布」，一条脏数据让整次跑作废太贵。但两侧都要回答同一个问题：**占位值必须能被下游识别为缺失**，否则它会被当成真实的最差值参与聚合。

**序号对齐与主核的行序号稳定性同构。** 并发任务不携带自己的坐标、而靠提交顺序的序号回填，前提是「提交顺序 = 逻辑顺序」这一不变量成立。主核在并行扫描时同样依赖「每个工作线程按分片顺序返回」来保证结果确定性。一旦这个不变量被破坏（比如跳过了某个任务），回填就会整体错位——这也是为什么「提交个数必须等于指标总数」这个隐含前提值得单独记住。

**取消粒度是批而不是任务。** 主核里取消一条大查询通常也是批级检查（在两个批次之间检查中断标志），因为在单个数据块内部检查的代价太高。这里的取舍是同一个：**检查频率与响应灵敏度成正比，与开销也成正比**。

对 AI 工程最实用的四条：其一，**默认失败不报错**，想要硬失败必须显式 `raise_exceptions=True`；否则你会在报告里看到一堆 NaN 却不知道该修哪里。其二，**同一批指标对象复用会带上前一次的模型**，除非走完整流程让 `finally` 还原；提前返回执行器的路径不会还原。其三，**`max_workers` 与 `batch_size` 解决的是不同问题**，前者限并发度、后者限同时提交量，二者都可设但意义不同。其四，**`run_async_batch` 不能传运行配置**，需要自定义重试时得自己建执行器。
