---
project: graphrag
feature: api-and-cli
title: API 与命令行调用面
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/api/index.py
    symbols: [build_index, _get_method]
  - path: packages/graphrag/graphrag/api/query.py
    symbols: [local_search, global_search, drift_search, basic_search]
  - path: packages/graphrag/graphrag/cli/index.py
    symbols: [index_cli, update_cli]
  - path: packages/graphrag/graphrag/cli/query.py
    symbols: [run_local_search, run_global_search, run_drift_search]
---

# API 与命令行调用面

## 一句话职责

把「索引」与「查询」两件事各包成一份**只依赖 DataFrame 的纯函数 API**（配置文件 + parquet DataFrame 进，结果是文本 + context 出），再让 CLI 充当**厚壳**：加载配置、从磁盘读索引产物、把流式输出打到终端、把错误变成退出码。

## 机制是什么

调用面分成两层，而两层的职责划分很干脆：

| 层 | 位置 | 输入 | 输出 |
|---|---|---|---|
| **API** | `graphrag.api` | `GraphRagConfig` + `pd.DataFrame` | 结果值或异步生成器 |
| **CLI** | `graphrag.cli` | 命令行参数 + 一个 `root_dir` | `print` 到 stdout + 进程退出码 |

**API 层不碰磁盘、不打日志到 stdout、不调 `sys.exit`。** 它拿到的 DataFrame 已经是从 parquet 读出来的。**这是一个刻意的「可嵌入」设计**：任何 Python 程序都能调用 `local_search(config, entities=..., ...)` 而不被 CLI 的行为绑架。

**而 CLI 层的全部工作就是「补齐 API 缺少的那部分」** —— 从磁盘读 DataFrame、把配置从 YAML 加载、把结果打到屏幕。**它是可抛弃的**：所有实质逻辑都在 API 层。

**四个查询 API 的差别只是「需要哪些输入表」：**

| API | 需要的表 | 检索方式 |
|---|---|---|
| `global_search` | entities / communities / community_reports | 社区报告 |
| `local_search` | 上者 + text_units / relationships / covariates | 实体邻域 + 原文块 |
| `drift_search` | entities / communities / community_reports / text_units / relationships | 先粗后细（DRIFT） |
| `basic_search` | **只有 text_units** | 纯向量检索 |

**`basic_search` 只需要一张表** —— 因为它压根不用图，只是一个普通 RAG。

## 怎么实现的

### 一、四个查询 API 是同一个模板的四份实例

**每一个都遵循完全相同的五步：**

```
@validate_call(config={"arbitrary_types_allowed": True})
async def xxx_search(config, ..., query, callbacks=None, verbose=False) -> tuple[...]:
    init_loggers(config=config, verbose=verbose, filename="query.log")

    callbacks = callbacks or []
    full_response = ""
    context_data = {}

    def on_context(context: Any) -> None:
        nonlocal context_data
        context_data = context

    local_callbacks = NoopQueryCallbacks()
    local_callbacks.on_context = on_context
    callbacks.append(local_callbacks)

    async for chunk in xxx_search_streaming(...):
        full_response += chunk
    logger.debug("Query response: %s", truncate(full_response, 400))
    return full_response, context_data
```

**五个步骤各有讲究：**

**第一步：`init_loggers(..., filename="query.log")`。**
每个 API 调用都会重新初始化日志，且**文件名固定为 `query.log`**（索引那边是默认名）。**于是查询与索引的日志分开写** —— 这对同一个目录下既有索引又有查询的场景是必要的。

**第二步：`callbacks = callbacks or []` 后**追加**一个内部回调。**

**注意方向：往调用方给的列表里 append。** 这修改了**调用方持有的列表** —— 一个副作用。若调用方复用同一个列表调两次，里面会积累两个 `on_context` 回调（后者覆盖前者，行为上无害但不干净）。

**第三步：用 `NoopQueryCallbacks` 的实例当「上下文捕获器」。**

```
local_callbacks = NoopQueryCallbacks()
local_callbacks.on_context = on_context
```

**这是「用一个空实现对象 + 动态赋属性」来做局部回调。** `NoopQueryCallbacks` 的所有方法都是空实现，于是只用 `on_context` 不会产生其它副作用。**而 `on_context` 是在实例上动态赋的**（不是子类覆盖），说明这个模式是「临时装配」而非类型化扩展。

**第四步（关键）：非流式 API 就是「消费流式生成器并拼接」。**

```
async for chunk in xxx_search_streaming(...):
    full_response += chunk
```

**于是「流式」是唯一的实现，「非流式」是它的一个消费者。** 这个设计的价值在于**两条路径绝不会产生不同的结果** —— 因为它们走同一段代码。

**代价**：非流式路径也要建一条流式的管道（含所有分块/生成器开销），而如果下游根本不需要流，这些开销是白付的。**但正确性优先于这点开销是显然的选择。**

**第五步：`truncate(full_response, 400)` 只用于日志。** 于是日志里是前 400 字符加省略符，而不是整段回答——**避免把用户数据全量写进日志**。

**返回值固定是二元组 `(response, context_data)`。**

**`context_data` 是从回调里捕获的** —— 也就是说它**不是这个函数自己算出来的**，而是搜索引擎在过程中通过 `on_context` 抛出来的。**这是一个「用回调收集旁路输出」的模式**：主返回值是回答，而「用了哪些上下文」通过回调浮出来。

**为什么这么设计**：因为 context 是在流式过程中逐步构建的（可能经过多次检索与合并），若等流结束再重新计算，成本和一致性都有问题。**而回调让「构建到哪一步就交出什么」成为可能。**

**`@validate_call` 装饰器。**

```
@validate_call(config={"arbitrary_types_allowed": True})
```

**它来自 pydantic，对参数做**类型校验**。** `arbitrary_types_allowed` 是必需的，因为参数里有 `GraphRagConfig` 与 `pd.DataFrame`（不是 pydantic 模型）。

**这个装饰器给 API 层提供了一层「运行时类型检查」** —— 于是「传错类型的 DataFrame」会在入口处被抓到，而不是在深层某个 `df["x"]` 报 `KeyError`。**对一个公开 API 来说是合理的防御。**

### 二、`build_index`：公开 API 里唯一「有状态」的那个

**它的形状与查询 API 完全不同——它没有流式孪生，而是直接返回结果列表。**

```
async def build_index(config, method=IndexingMethod.Standard, is_update_run=False,
                      callbacks=None, additional_context=None, verbose=False,
                      input_documents=None) -> list[PipelineRunResult]:
```

**七个参数，其中三个是「外部注入点」：**

| 参数 | 注入什么 |
|---|---|
| `callbacks` | 工作流生命周期回调（见 g02 那页） |
| `additional_context` | 任意字典，管道状态里能取到 |
| `input_documents` | **覆盖文档加载与解析**，直接给一批已解析好的文档 |

**`input_documents` 是一个重要的「跳过前半段」的入口**：传了它，管道就不读 CSV、不分块，直接从给定文档开始建图。**这让「我已有分好的块」这个场景不必伪造输入文件。**

**而它的执行是「创建管道 → 迭代 `run_pipeline` → 收集结果」。**

```
init_loggers(config=config, verbose=verbose)
workflow_callbacks = create_callback_chain(callbacks) if callbacks else NoopWorkflowCallbacks()
outputs: list[PipelineRunResult] = []

logger.info("Initializing indexing pipeline...")
method = _get_method(method, is_update_run)
pipeline = PipelineFactory.create_pipeline(config, method)

workflow_callbacks.pipeline_start(pipeline.names())

async for output in run_pipeline(pipeline, config, callbacks=workflow_callbacks, ...):
    outputs.append(output)
    if output.error is not None:
        logger.error("Workflow %s completed with errors", output.workflow)
        workflow_callbacks.pipeline_error(output.error)
    else:
        logger.info("Workflow %s completed successfully", output.workflow)
    logger.debug(str(output.result))

workflow_callbacks.pipeline_end(outputs)
return outputs
```

**五个要点：**

1. **`create_callback_chain(callbacks) if callbacks else NoopWorkflowCallbacks()`** —— 无回调时用一个空实现而非 `None`（同 e01 那页 `NoopMetricsStore` 的手法），于是下游不必判空。
2. **`_get_method` 在创建管道**之前**被调用** —— 因为方法名决定了创建哪个管道。
3. **`pipeline_start` 在迭代之前、`pipeline_end` 在之后** —— 而 `pipeline_error` 在**每个**失败的工作流之后调用**多次**。**所以「`pipeline_end` 一定会被调用」这个契约是成立的**（没有提前 `return`）。
4. **失败的 workflow 不中断迭代** —— 循环继续，结果都收进 `outputs`。**于是「部分成功」是可能的，而调用方要从 `outputs` 里逐个检查 `error`。**
5. **`logger.debug(str(output.result))`** —— 结果只在 DEBUG 级打印（可能是很大的 DataFrame）。

**`todo` 注释指出了一处已知的设计问题：**

```
# todo: this could propagate out to the cli for better clarity, but will be a breaking api change
method = _get_method(method, is_update_run)
```

**「方法名的合成本可以在 CLI 侧做，但那会是破坏性的 API 变更」** —— 也就是说：`_get_method` 把 `method` 与 `is_update_run` 折成一个字符串（`standard-update`），而这个合成逻辑藏在 API 内部。**CLI 传两个参数、API 内部合成**，是一个「本该由调用方决定」的知识被埋在实现里。**而改成让 CLI 直接传合成后的名字，会破坏已有调用方。**

### 三、`_get_method`：四行函数承担了「更新跑用哪套管道」

```
def _get_method(method: IndexingMethod | str, is_update_run: bool) -> str:
    m = method.value if isinstance(method, IndexingMethod) else method
    return f"{m}-update" if is_update_run else m
```

**它做两件事：把枚举拆成字符串值、然后在更新跑时加 `-update` 后缀。**

**于是「文件插入」与「增量更新」是**同名的两套管道**（`standard` 与 `standard-update`）。** 这个命名约定把「复用大部分工作流、替换少数几个」表达成名字后缀。

**而 `isinstance(method, IndexingMethod)` 的判断让参数可以同时接受枚举与裸字符串** —— 因为枚举是 `StrEnum`，`method.value` 与 `method` 在多数场景下等价，但显式取 `.value` 保证了类型是 `str`。

### 四、CLI 层：厚壳的全部内容

**`index_cli` 与 `update_cli` 是两个近乎相同的函数。**

```
def index_cli(root_dir, method, verbose, cache, dry_run, skip_validation):
    config = load_config(root_dir=root_dir)
    _run_index(config=config, method=method, is_update_run=False, verbose=verbose,
               cache=cache, dry_run=dry_run, skip_validation=skip_validation)

def update_cli(root_dir, method, verbose, cache, skip_validation):
    config = load_config(root_dir=root_dir)
    _run_index(config=config, method=method, is_update_run=True, verbose=verbose,
               cache=cache, dry_run=False, skip_validation=skip_validation)
```

**两个差异：`is_update_run` 的值，以及 `update_cli` **没有** `dry_run` 参数（硬传 `False`）。**

**「更新跑不支持 dry run」是一个语义决定** —— 因为 dry run 的用途是「检查配置能不能跑」，而更新跑依赖已有索引的状态，它的「试跑」没有意义。

**而 `_run_index` 是真正的实现，它有五个阶段。**

```
init_loggers(config=config, verbose=verbose)

if not cache:
    config.cache.type = CacheType.Noop

if not skip_validation:
    validate_config_names(config)

logger.info("Starting pipeline run. %s", dry_run)
logger.info("Using default configuration: %s", redact(config.model_dump()))

if dry_run:
    logger.info("Dry run complete, exiting...")
    sys.exit(0)

_register_signal_handlers()

outputs = asyncio.run(api.build_index(config=config, method=method, is_update_run=is_update_run,
                                      callbacks=[ConsoleWorkflowCallbacks(verbose=verbose)], verbose=verbose))
encountered_errors = any(output.error is not None for output in outputs)

sys.exit(1 if encountered_errors else 0)
```

**六个决定，按重要性排：**

**第一，`--no-cache` 是通过改写配置实现的。** `config.cache.type = CacheType.Noop` —— 而 `CacheType.Noop` 的值是 `"none"`（见 f01 那页）。**于是「关缓存」的零成本路径在 CLI 这里被真正用上了。**

**第二，`redact(config.model_dump())` 把配置里的密钥打码后才记录。** 这是一个安全必需项：配置里有 `api_key`、`connection_string`。**而 `redact` 是 CLI 侧的工具（`graphrag.utils.cli`）**，API 层不做这件事——**因为它不负责打日志给终端用户**。

**第三，`dry_run` 用 `sys.exit(0)` 退出，而不是返回。** 于是「试跑通过」与「试跑失败」的区分靠退出码——而失败发生在 `validate_config_names` 抛异常时（非零退出由未捕获异常产生）。

**第四，`_register_signal_handlers()` 只注册 `SIGINT`（与 `SIGHUP`，非 Windows）。**

```
def handle_signal(signum, _):
    logger.debug(f"Received signal {signum}, exiting...")
    for task in asyncio.all_tasks():
        task.cancel()

signal.signal(signal.SIGINT, handle_signal)
if sys.platform != "win32":
    signal.signal(signal.SIGHUP, handle_signal)
```

**它取消所有 asyncio 任务** —— 于是 Ctrl-C 会让管道优雅终止而不是硬杀。**而 `SIGHUP` 只在非 Windows 上注册**，因为 Windows 没有这个信号。

**第五，`ConsoleWorkflowCallbacks(verbose=verbose)` 是唯一注册的回调。** 于是 CLI 的进度输出完全来自这个回调（见 g02 那页）。

**第六，退出码由「有没有任何 workflow 失败」决定。**

```
encountered_errors = any(output.error is not None for output in outputs)
sys.exit(1 if encountered_errors else 0)
```

**注意这是「任一失败即整体失败」** —— 而 `build_index` 的返回是「部分成功」的列表。**CLI 把它压成了一个布尔**，于是脚本化使用时无法区分「全部成功」与「有些失败」。

**而 `index_cli` 里的 `load_config` 与 `_run_index` 里的配置改写是可组合的**：`load_config` 从 YAML 读出 `GraphRagConfig`，`_run_index` 就地改 `cache.type`。**这是一个「配置对象可变」的用法** —— 而 pydantic 默认可变，所以能这么做。

### 五、查询 CLI：`_resolve_output_files` 与流式的两套代码

**四个 `run_*_search` 函数的骨架相同，而它们的公共部分是 `_resolve_output_files`。**

```
def _resolve_output_files(config, output_list, optional_list=None) -> dict[str, Any]:
    dataframe_dict = {}
    storage_obj = create_storage(config.output_storage)
    table_provider = create_table_provider(config.table_provider, storage=storage_obj)
    reader = DataReader(table_provider)
    for name in output_list:
        df_value = asyncio.run(getattr(reader, name)())
        dataframe_dict[name] = df_value

    if optional_list:
        for optional_file in optional_list:
            file_exists = asyncio.run(table_provider.has(optional_file))
            if file_exists:
                dataframe_dict[optional_file] = asyncio.run(getattr(reader, optional_file)())
            else:
                dataframe_dict[optional_file] = None
    return dataframe_dict
```

**三个要点：**

1. **用 `getattr(reader, name)()` 按名字取方法** —— 于是「需要哪些表」就是一个字符串列表，而 `DataReader` 为每张表提供同名方法。**这是一个「表名即方法名」的约定**，代价是拼错表名会 `AttributeError`（没有校验）。
2. **每个表用独立的 `asyncio.run(...)`** —— 于是 N 张表 = N 个事件循环。**这是一个可以合并成单个 `asyncio.run` 里 `gather` 的优化**，但当前写法更简单。
3. **`optional_list` 的语义是「不存在则给 `None`」而不是「省略这个键」** —— 于是下游 `dataframe_dict["covariates"]` 一定可访问，只是可能是 `None`。**这与 `Cache.get_many` 的「缺失则省略键」相反**，而与 `VectorStoreDocument.vector` 可为 `None` 一致。

**而 `data_dir` 的覆盖是通过 `cli_overrides` 实现的。**

```
cli_overrides: dict[str, Any] = {}
if data_dir:
    cli_overrides["output_storage"] = {"base_dir": str(data_dir)}
config = load_config(root_dir=root_dir, cli_overrides=cli_overrides)
```

**于是 `--data <路径>` 不必改配置文件，而是覆盖其中的 `output_storage.base_dir`。** 这是一个「配置层做覆盖」的机制 —— 比「改完配置再写回」干净得多。

**而每个 `run_*_search` 里都有**两份**几乎相同的代码：流式与非流式。**

```
if streaming:
    async def run_streaming_search():
        full_response = ""
        context_data = {}
        def on_context(context: Any) -> None:
            nonlocal context_data
            context_data = context
        callbacks = NoopQueryCallbacks()
        callbacks.on_context = on_context
        async for stream_chunk in api.global_search_streaming(...):
            full_response += stream_chunk
            print(stream_chunk, end="")
            sys.stdout.flush()
        print()
        return full_response, context_data
    return asyncio.run(run_streaming_search())
# not streaming
response, context_data = asyncio.run(api.global_search(config=config, ..., verbose=verbose))
print(response)
return response, context_data
```

**注意流式分支里**又自己建了一个 `on_context` 回调** —— 因为 `global_search_streaming` 是「裸」的流式函数，它不负责帮你捕获 context（那是非流式 `global_search` 做的事）。**于是 CLI 必须重复一遍 API 层的回调装配。**

**这个重复是这一层最明显的冗余**：四个函数 × 两份代码 = 八段几乎相同的装配。**而它与 API 层的实现重复了两次**（API 的非流式版也做同样的装配）。

**而流式分支只对 `print` 与 `sys.stdout.flush()` 做了事** —— 这正是 CLI 存在的理由（把 chunk 推到终端）。

**注意 `print()` 在循环之后单独一行** —— 因为流式输出没有换行结尾，需要补一个。

### 六、两个 CLI 分层的工具函数

**`_register_signal_handlers` 在 `cli/index.py` 里，而查询 CLI 没有对应的东西。** 于是**查询被 Ctrl-C 时没有优雅取消** —— 而原因是查询是短操作（几秒），索引是长操作（几十分钟）。

**这是「按实际需要决定要不要做」的一个例子** —— 不是遗漏，而是判断了收益。

**而 `run_*_search` 的返回类型是 `tuple[response, context_data]`** —— 与 API 层一致。**而这个返回值在 CLI 里其实没用**（CLI 直接 `print`），它存在是为了**测试**能拿到数据。

## 关键取舍

**源码事实：** API 层是纯函数式的（配置 + DataFrame 进出）**不碰磁盘、不 `print`、不 `sys.exit`**；CLI 层的全部工作是「补齐 API 缺的那部分」（读磁盘、加载 YAML、打到屏幕、转成退出码）。

**源码事实：** 四个查询 API 走同一个模板：`init_loggers(filename="query.log")` → 追加内部 `on_context` 回调 → 消费流式生成器拼接 → 返回 `(response, context_data)`。

**源码事实：** 非流式 API 就是「消费流式生成器」，于是两条路径不可能产生不同结果，代价是非流式也付了建管道的开销。

**源码事实：** `context_data` 不是函数自己算的，而是通过 `on_context` 回调捕获——因为 context 是流式过程中逐步构建的，事后重算既有成本也有一致性问题。

**源码事实：** `callbacks = callbacks or []` 之后直接 `append`，**修改了调用方持有的列表**；复用同一列表调两次会积累多个 `on_context`。

**源码事实：** `NoopQueryCallbacks()` 实例上**动态赋** `on_context` 属性（而非子类覆盖），是「临时装配」的形态。

**源码事实：** `@validate_call(config={"arbitrary_types_allowed": True})` 给 API 层加了运行时类型校验，而 `arbitrary_types_allowed` 是因为参数里有 `GraphRagConfig` 与 `pd.DataFrame`。

**源码事实：** `build_index` 的三个注入点是 `callbacks`、`additional_context`、`input_documents`；其中 `input_documents` 让「已有分好的块」不必伪造输入文件。

**源码事实：** `build_index` 中失败的 workflow **不中断迭代**（结果都收进 `outputs`），`pipeline_error` 会被多次调用，而 `pipeline_end` 保证被调用（没有提前 return）。

**源码事实：** `_get_method` 把 `(method, is_update_run)` 合成 `"{m}-update"`，**藏在 API 内部**；`todo` 注释承认「本可提到 CLI，但那是破坏性 API 变更」。

**源码事实：** `--no-cache` 通过就地改写 `config.cache.type = CacheType.Noop` 实现（真正用上了 f01 那页的零成本路径）。

**源码事实：** `redact(config.model_dump())` 在记日志前给密钥打码——API 层不做，因为「给终端用户打日志」是 CLI 的职责。

**源码事实：** `_register_signal_handlers` 取消所有 asyncio 任务（Ctrl-C 优雅终止），且 `SIGHUP` 只在非 Windows 注册；**查询 CLI 没有对应的信号处理**（查询是短操作，判断了收益）。

**源码事实：** CLI 退出码是 `any(output.error is not None)`，把「部分成功」压成一个布尔——脚本化使用时无法区分「全部成功」与「有些失败」。

**源码事实：** `_resolve_output_files` 用 `getattr(reader, name)()` 按表名取方法（无校验，拼错即 `AttributeError`）、每张表一个独立 `asyncio.run`、`optional_list` 的缺失项给 `None` 而非省略键。

**源码事实：** `--data` 通过 `cli_overrides["output_storage"] = {"base_dir": ...}` 覆盖配置，而不是改配置文件。

**源码事实：** 查询 CLI 的流式分支必须**再建一次** `on_context` 回调，因为 `global_search_streaming` / `local_search_streaming` 是裸流式函数、不负责捕获 context；于是四个函数 × 两份代码 = 八段几乎相同的装配。

**架构推论：** 「API 纯、CLI 厚」这个分层让 graphrag 能被嵌进任何 Python 程序——而它的判据是**「API 层不产生不可回收的副作用」**（不到 stdout、不退出进程、不写文件）。一旦 API 里出现 `print`，嵌入方就无法控制自己的输出流。**这条纪律比「代码分层好看」重要得多**，因为它决定了这个库能不能被当作库用。

**架构推论：** 「流式是唯一实现、非流式是它的消费者」这个决定，把**两条路径的一致性问题彻底消除了** —— 而不是靠测试去保证它们一致。**当一个功能有两种输出形态时，让一种成为另一种的特例，比让它们平行实现要可靠一个量级。** 代价是非流式付了流式的开销，而这个代价可以用「提前合并 chunk」优化掉，无需分裂实现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/api/index.py` | `build_index` | 索引 API：创建管道、迭代结果、回调分派 |
| `packages/graphrag/graphrag/api/index.py` | `_get_method` | `(method, is_update_run)` → `"{m}-update"` |
| `packages/graphrag/graphrag/api/query.py` | `global_search` / `local_search` / `drift_search` / `basic_search` | 四个查询 API 的同一模板 |
| `packages/graphrag/graphrag/api/query.py` | `on_context` 局部回调 | 从流式过程中捕获 `context_data` |
| `packages/graphrag/graphrag/api/query.py` | `global_search_streaming` / `local_search_streaming` | 真正的实现，普通函数返回 AsyncGenerator |
| `packages/graphrag/graphrag/cli/index.py` | `index_cli` / `update_cli` | 加载配置后转调 `_run_index`（差异只有 `is_update_run`） |
| `packages/graphrag/graphrag/cli/index.py` | `_run_index` | 五个阶段：日志、`--no-cache` 改写、校验、dry run、信号与退出码 |
| `packages/graphrag/graphrag/cli/index.py` | `_register_signal_handlers` | 取消所有 asyncio 任务，`SIGHUP` 仅非 Windows |
| `packages/graphrag/graphrag/cli/query.py` | `run_global_search` / `run_local_search` / `run_drift_search` / `run_basic_search` | 读产物、调用 API、流式打到终端 |
| `packages/graphrag/graphrag/cli/query.py` | `_resolve_output_files` | 按表名取 `DataReader` 方法，`optional_list` 缺失给 `None` |
| `packages/graphrag/graphrag/index/workflows/factory.py` | `PipelineFactory` | 按 method 名创建管道 |
| `packages/graphrag/graphrag/index/run/utils.py` | `create_callback_chain` | 把回调列表包成一个 `WorkflowCallbacksManager` |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：查询 API](https://microsoft.github.io/graphrag/query/overview/)
- [graphrag 文档：CLI](https://microsoft.github.io/graphrag/cli/)
- [pydantic 文档：validate_call](https://docs.pydantic.dev/latest/concepts/validation_decorator/)

## 与主核 / AI 的关系

这一页的两层与主核的**客户端协议 + `psql`** 分工完全同构：一层是协议实现（可被任意客户端复用），一层是终端工具（负责输入解析、输出渲染、退出码）。

**而 graphrag 的 API 层比数据库协议更「纯」** —— 它连「读磁盘」都不做，DataFrame 由调用方给。**这相当于让协议层接受「已经解析好的结果集」而不是自己去扫表。** 好处是极容易测试（给两个 DataFrame 就能调），坏处是**每个调用方都要自己实现「从产物目录读表」这一段**（于是 CLI 那 400 行就是在做这件事）。

**`context_data` 通过回调旁路返回，与数据库的 `NOTICE` 消息同类。** 两者都是「主结果之外的过程信息」，且都不能混进主返回值（一个是要打印的文本，一个是结果集）。**差别是数据库的 `NOTICE` 是异步推给客户端的（不需要预先注册回调），而这里要先传一个回调对象。**

**「流式是唯一实现」与数据库的**游标（cursor）**是同一个答案。** 数据库不提供「一次取全部」的独立接口——`fetchall()` 只是「循环 `fetchone()`」。**结果的唯一来源是游标**，于是「分批取」与「一次取」不可能不一致。graphrag 这里用生成器达到了同样的效果。

**「CLI 把部分成功压成布尔退出码」是一个与数据库工具同样的取舍。** `pg_dump` 遇到某个表失败时的退出码也是单一的——而正确做法（区分「全部成功 / 部分失败 / 完全失败」）需要一个更丰富的退出码体系。**POSIX 的单一退出码是这类信息损失的根本原因**，而几乎所有 CLI 都接受了它。

对 agent 工程的直接价值有三条。其一，**库层不能有不可回收的副作用**。`print`、`sys.exit`、写固定路径的日志，都会让这个库在某些宿主里不可用。其二，**让一种输出形态成为另一种的特例**。流式与非流式平行实现，两者的差异迟早会出现（一个修了另一个没修）；而「非流式 = 消费流式」从结构上排除了这种可能。其三，**回调可以承载旁路输出，但要注意列表是引用**。`callbacks or []` 之后 `append` 修改的是调用方的列表——这类「看起来无害的顺手操作」会在复用场景里积累出重复回调。