---
project: langchain
feature: config-and-context
title: 运行配置与上下文传播
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/runnables/config.py
    symbols: [RunnableConfig, ContextThreadPoolExecutor]
  - path: libs/core/langchain_core/runnables/configurable.py
    symbols: [DynamicRunnable, RunnableConfigurableFields, RunnableConfigurableAlternatives]
---

# 运行配置与上下文传播

## 一句话职责

`RunnableConfig` 是一份 `total=False` 的 `TypedDict`（八个键：`tags`、`metadata`、`callbacks`、`run_name`、`max_concurrency`、`recursion_limit`、`configurable`、`run_id`），它靠一个 `ContextVar` 沿调用栈**隐式**向下传播，靠 `ensure_config` / `merge_configs` / `patch_config` 三个函数在每一层做合并与覆盖；`ContextThreadPoolExecutor` 负责把这份上下文带过线程边界，`DynamicRunnable` 则在调用前把 `configurable` 里的值**折叠回对象字段**。

## 机制是什么

链上的每一层都需要读同一份配置（追踪标签、并发上限、递归上限、可配置字段），但若把它做成显式参数，每一个 `invoke` 签名都要多一个 `config` 形参，每个自定义函数都要被迫声明它。langchain 的选择是**两者兼有**：

| 通道 | 形态 | 谁用 |
|---|---|---|
| 显式 | `invoke(input, config=...)` 形参 | 框架内部的组合器 |
| 隐式 | `var_child_runnable_config` 这个 `ContextVar` | 用户写的函数、`RunnableLambda` |

`ContextVar` 解决的是「怎么不传参也能读到」，但它带来一个新问题：**线程和协程会切断上下文**。`ContextVar` 的语义是「每个执行上下文一份副本」，新线程默认拿到的是初始值（不是父线程当前值）。于是凡是把工作丢进线程池或 `asyncio` 任务的地方，都必须显式搬运上下文——`ContextThreadPoolExecutor` 就是为了这个而存在的。

`configurable` 这个键又引出一个二级机制：它是一种**「在运行期改写对象字段」**的请求。用户不能直接改 `ChatOpenAI(temperature=0)` 的字段（那是构造期的事），但他可以在 `config={"configurable": {"temperature": 0.9}}` 里请求「这次用 0.9」。把这个请求变成实际行为的责任，落在 `DynamicRunnable` 这一族身上——它们在真正调用前**用 config 里的值重建一个实例**。

三层结构因此成型：

```
RunnableConfig（数据形状）
   ↓ 靠 ContextVar + 三个合并函数传播
DynamicRunnable（把 configurable 折叠成字段，重建实例）
```

## 怎么实现的

**`total=False` 不是一个风格选择，而是两个机制的前提。**

```
class RunnableConfig(TypedDict, total=False):
    tags: list[str]
    metadata: dict[str, Any]
    callbacks: Callbacks
    run_name: str
    max_concurrency: int | None
    recursion_limit: int
    configurable: dict[str, Any]
    run_id: uuid.UUID | None
```

类注释把这个理由写全了：

> `total=False` 是刻意设置的：允许构造部分配置并由 `merge_configs` 合并；支持配置从父 runnable 向子 runnable 传播（靠 `var_child_runnable_config` 这个 `ContextVar`，无需显式传参），其中配置是被合并而不是被替换。

**「部分配置」是这套机制的基础单位。** 每一层只写自己关心的那几项，其余留给合并——若 `total=True`，那每一层都必须写全八项，合并就失去了意义（也无从知道哪些是「没设」还是「设成了 None」）。

紧跟着的两张常量表划定了「哪些键是配置」与「哪些键要深拷贝」：

```
CONFIG_KEYS = ["tags", "metadata", "callbacks", "run_name", "max_concurrency", "recursion_limit", "configurable", "run_id"]
COPIABLE_KEYS = ["tags", "metadata", "callbacks", "configurable"]
```

`COPIABLE_KEYS` 是四个**可变容器**。凡是从外部 plumb 进来的这四个键，都要 `.copy()` 一份再放进新 config——否则「子层往 `metadata` 里加一个键」会污染父层的字典。`run_name` / `run_id` / `max_concurrency` / `recursion_limit` 是标量，不需要复制。

**`ensure_config` 是唯一把「隐式」与「显式」两条通道汇合的地方。**

```
empty = RunnableConfig(tags=[], metadata={}, callbacks=None, recursion_limit=DEFAULT_RECURSION_LIMIT, configurable={})
if var_config := var_child_runnable_config.get():
    empty.update({k: v.copy() if k in COPIABLE_KEYS else v for k, v in var_config.items() if v is not None})
if config is not None:
    empty.update({k: v.copy() if k in COPIABLE_KEYS else v for k, v in config.items() if v is not None and k in CONFIG_KEYS})
if config is not None:
    for k, v in config.items():
        if k not in CONFIG_KEYS and v is not None:
            empty["configurable"][k] = v
```

四段各有明确职责：

1. **先铺一份默认值**：空列表、空字典、`callbacks=None`、`recursion_limit=DEFAULT_RECURSION_LIMIT`（该常量是 `25`）。于是下游读 `config["recursion_limit"]` 不必担心键缺失。
2. **再叠 `ContextVar` 里的配置**，跳过值为 `None` 的键。这一步是「父层 → 子层」的隐式传递。
3. **再叠显式传入的配置**，只收 `CONFIG_KEYS` 里的键，同样跳过 `None`。显式优先于隐式。
4. **最后把「不在 `CONFIG_KEYS` 里的键」统统塞进 `configurable`**。

第 4 段是最容易被忽略的一处**兼容设计**。它意味着：

```
chain.invoke(x, config={"temperature": 0.9})
```

与

```
chain.invoke(x, config={"configurable": {"temperature": 0.9}})
```

等价——顶层放错位置的键会被自动降级进 `configurable`，而不是被静默丢弃。`configurable` 的语义本来就是「运行期可覆盖的字段」，把未知键放进去是最合理的处置。若不做这一步，早期把参数写在顶层的代码会静默失效。

末尾还有一小段：

```
for configurable_key in ("model", "checkpoint_ns"):
    if isinstance(configurable_value := empty.get("configurable", {}).get(configurable_key), str) and configurable_key not in empty["metadata"]:
        empty["metadata"][configurable_key] = configurable_value
```

`model` 与 `checkpoint_ns` 这两个可配置键，若值是字符串且 `metadata` 里还没有同名键，就被**提升到 `metadata`**。理由是追踪与过滤需要它们作为一级字段（按 model 维度筛 trace），而不是埋在 `configurable` 里。这条硬编码的两个键名叫作「为观测而做的特例」，它的存在说明 `configurable` 与 `metadata` 的边界在实践中并不干净。

`DEFAULT_RECURSION_LIMIT = 25` 这个数值本身值得记一笔：它限制的是「同一条链上能连续嵌套多少层 run」。在 `RunnableSequence` 里它不会被动消耗（序列本身不递归），真正消耗它的是递归调用（一个 `Runnable` 在自身内部再调自己）。

**`var_child_runnable_config` 是一个模块级 `ContextVar`，默认 `None`。**

```
var_child_runnable_config: ContextVar[RunnableConfig | None] = ContextVar("child_runnable_config", default=None)
```

默认 `None` 而不是空 dict，是为了让 `ensure_config` 里 `if var_config := ...` 这个真值判断成立——空 dict 也是假值，但显式 `None` 更清楚表达「没有父层配置」。

它的注释里还有一句「This is imported and used in langgraph, so don't break」（那是在 `_set_config_context` 上），说明这个名字已经被外部包依赖，改名就是破坏性变更。

**`set_config_context` 不只是设一个 `ContextVar`，它还搬移 LangSmith 的追踪上下文。**

```
@contextmanager
def set_config_context(config: RunnableConfig) -> Generator[Context, None, None]:
    from langsmith.run_helpers import _set_tracing_context  # noqa: PLC0415
    ctx = copy_context()
    config_token, _ = ctx.run(_set_config_context, config)
    try:
        yield ctx
    finally:
        ctx.run(var_child_runnable_config.reset, config_token)
        ctx.run(_set_tracing_context, {"parent": None, "project_name": None, "tags": None, "metadata": None, "enabled": None, "client": None})
```

三个关键点：

1. **`copy_context()` 先复制一份当前上下文**，然后在**那份副本里**执行 `_set_config_context`。于是「设置」这个动作不会污染调用方的上下文——调用方仍然读到父层的 config，只有拿到 `ctx` 并在里面 `run` 的子层才看得到新的 config。这就是 `with set_config_context(cfg) as context: context.run(...)` 这个用法的前提。
2. **`_set_config_context` 还顺手设置了追踪的 parent run：**

```
config_token = var_child_runnable_config.set(config)
current_context = None
if ((callbacks := config.get("callbacks"))
    and (parent_run_id := getattr(callbacks, "parent_run_id", None))  # Is callback manager
    and (tracer := next((h for h in getattr(callbacks, "handlers", []) if isinstance(h, LangChainTracer), None)))
    and (run := tracer.run_map.get(str(parent_run_id)))):
    current_context = get_tracing_context()
    _set_tracing_context({"parent": run})
return config_token, current_context
```

也就是说：**若 config 的 `callbacks` 是一个 callback manager（有 `parent_run_id`），其中又有一个 `LangChainTracer` 实例，且该 tracer 的 `run_map` 里有这个父 run，那么追踪上下文里的 `parent` 被设成那个 run。** 三个条件全满足才生效。

这段逻辑的意义是让 LangSmith 的父子关系与 `Runnable` 的父子关系对齐。但它依赖**内部字段名**（`parent_run_id`、`handlers`、`run_map`），都是 `getattr` 加 `isinstance` 探测——一种对第三方库内部结构的软依赖。写法上很谨慎（每个属性都可能不存在），但这种耦合的脆弱性无法完全避免。

3. **清理阶段把整个追踪上下文重置成一套全 `None` 的值**，而不是恢复到之前的状态。这是一个**有意的「清空而非回滚」**：重置一份空上下文比精确回滚更简单，且退出时确实希望 parent 关系断掉（否则兄弟分支会在追踪上串成一条链）。

注意 `set_config_context` 里那两个 `from ... import` 都带 `# noqa: PLC0415` 与注释「延迟导入以避免模块级导入 langsmith（约 132ms）」。**132 毫秒**这个具体数字出现在注释里，说明这是一次实测驱动的优化——`langchain_core` 被大量下游包在导入时连带导入，如果它也硬拉 langsmith，会把冷启动拖慢一个可观的比例。

`_set_config_context` 存在的原因也与「langgraph 要用」一致：它分成两步（拿到 token 并返回、之后由调用方在同一个 ctx 里 reset），比 `contextmanager` 更适合被别的框架嵌入。

**`patch_config` 有两个「副作用」式的删除，这是本页最值得记的细节。**

```
def patch_config(config, *, callbacks=None, recursion_limit=None, max_concurrency=None, run_name=None, configurable=None):
    config = ensure_config(config)
    if callbacks is not None:
        # If we're replacing callbacks, we need to unset run_name
        # As that should apply only to the same run as the original callbacks
        config["callbacks"] = callbacks
        if "run_name" in config:
            del config["run_name"]
        if "run_id" in config:
            del config["run_id"]
    ...
```

**替换 callbacks 时同时删掉 `run_name` 与 `run_id`。**

理由写在注释里：「换了 callbacks 就应当 unset run_name，因为它只对原来那份 callbacks 的那次 run 有效」。更具体地说：`run_name` 与 `run_id` 是**这一次 run 的身份**，而 `callbacks` 是承载这次 run 的观测通道。换了通道，身份就作废——沿用旧 `run_id` 会让追踪把两个不同的 run 当成同一个。

这条逻辑几乎每个组合器都在用：`run_manager.get_child(f"seq:step:{i}")` 造出的新 callback manager 通过 `patch_config(config, callbacks=...)` 塞进 config，顺手把父层的 `run_name` / `run_id` 清掉。于是子层会得到新身份，而不会误用父身份。

另一个细节：`configurable` 在这里是**合并**而不是替换：

```
if configurable is not None:
    config["configurable"] = {**config.get("configurable", {}), **configurable}
```

其余几个键（`recursion_limit` / `max_concurrency` / `run_name`）都是直接赋值（替换）。差异有理由：`configurable` 里的键各自独立（改一个不该清掉另一个），而 `recursion_limit`、`max_concurrency` 之类的标量是整体语义。

**`merge_configs` 对不同键采取六套不同策略，这是合并这件事真正的复杂度所在。**

```
for config in (ensure_config(c) for c in configs if c is not None):
    for key in config:
        if key == "metadata":
            base["metadata"] = _merge_metadata_dicts(base.get("metadata", {}), config.get("metadata") or {})
        elif key == "tags":
            base["tags"] = sorted(set(base.get("tags", []) + (config.get("tags") or [])))
        elif key == "configurable":
            base["configurable"] = {**base.get("configurable", {}), **(config.get("configurable") or {})}
        elif key == "callbacks":
            ... 六种情况 ...
        elif key == "recursion_limit":
            if config["recursion_limit"] != DEFAULT_RECURSION_LIMIT:
                base["recursion_limit"] = config["recursion_limit"]
        elif key in COPIABLE_KEYS and config[key] is not None:
            base[key] = config[key].copy()
        else:
            base[key] = config[key] or base.get(key)
```

逐个看：

**`metadata` 走 `_merge_metadata_dicts`，而这个函数里有一条特例。**

```
merged = {**base, **incoming}
base_versions = base.get("lc_versions")
incoming_versions = incoming.get("lc_versions")
if isinstance(base_versions, Mapping) and isinstance(incoming_versions, Mapping):
    merged["lc_versions"] = {**base_versions, **incoming_versions}
```

注释说明了这唯一一处递归合并的理由：

> 元数据一般是「后写覆盖」语义。`lc_versions` 这个 LangChain 自己维护的键是唯一会在多次合并中累加的嵌套映射，这样 core、`langchain` 与 partner 包的版本能共存。

也就是说：`metadata` 整体是浅合并（后写覆盖），但 `lc_versions` 这个子字典必须**累积**。否则每合并一层就只剩下最外层的版本号，追踪里就看不到「这条 run 涉及哪些包的哪些版本」。这条特例的存在本身就说明「统一的后写覆盖」在实践里不够用，而作者选择了加特例而不是改成深合并（深合并会在用户自定义的嵌套字典上产生意外行为）。

**`tags` 是集合并排序。**

```
base["tags"] = sorted(set(base.get("tags", []) + (config.get("tags") or [])))
```

排序是为了让「同样的标签集合」产生同样的顺序，从而 trace 里的标签列表可比。而 `set` 去重避免了重复层叠时标签越滚越长（`chain | model` 里如果每层都带同一个 tag，不去重就会看到三次）。

**`recursion_limit` 只在「不是默认值」时才写入。**

```
if config["recursion_limit"] != DEFAULT_RECURSION_LIMIT:
    base["recursion_limit"] = config["recursion_limit"]
```

这一条是为「部分配置」服务的：`ensure_config` 保证每个 config 都有 `recursion_limit` 键（默认 25），但那只代表「没设」。如果无脑合并，后一个 config 的「默认 25」会把前一个显式设的 40 覆盖掉——一个典型的「默认值吞掉用户设置」bug。判断「是否等于默认值」是在没有 `None` 哨兵的情况下区分「设了」与「没设」的唯一办法。

**`callbacks` 要处理六种组合。**

```
if isinstance(these_callbacks, list):
    if base_callbacks is None: base["callbacks"] = these_callbacks.copy()
    elif isinstance(base_callbacks, list): base["callbacks"] = base_callbacks + these_callbacks
    else:  # base 是 manager
        mngr = these_callbacks.copy(); for callback in base_callbacks: mngr.add_handler(callback, inherit=True); base["callbacks"] = mngr
else:  # these 是 manager
    if base_callbacks is None: base["callbacks"] = these_callbacks
    elif isinstance(base_callbacks, list): base["callbacks"] = base_callbacks + these_callbacks
    ...
```

也就是说 `callbacks` 的取值可以是 `None`、`list[handler]`、或一个 callback manager，两两组合共六种。合并的方向一致：**靠后的 config 是「更内层」的，它的 handlers 被保留，前者的 handlers 以 `inherit=True` 的形式加进去**。这个 `inherit=True` 是关键——它让父层的追踪 handler 继续接收子层的事件。

**`ensure_config` 的一个副作用值得单独提。**

`merge_configs` 第一步就把每个 config 过一遍 `ensure_config`。而 `ensure_config` 会**返回一个新字典**（`empty` 是新建的），于是「合并」不会变异任何一个输入 config。相比之下 `patch_config` 是就地修改（`config = ensure_config(config)` 后继续往里写）——两者对输入的处理不同，而 `patch_config` 的调用方拿到的确实是新字典（因为 `ensure_config` 已经造了一份）。

**`ContextThreadPoolExecutor` 的两处搬运对应线程池的两个入口。**

```
class ContextThreadPoolExecutor(ThreadPoolExecutor):
    def submit(self, func, *args, **kwargs):
        return super().submit(cast("Callable[..., T]", partial(copy_context().run, func, *args, **kwargs)))

    def map(self, fn, *iterables, **kwargs):
        contexts = [copy_context() for _ in range(len(iterables[0]))]
        def _wrapped_fn(*args):
            return contexts.pop().run(fn, *args)
        return super().map(_wrapped_fn, *iterables, **kwargs)
```

`submit` 是「每个任务各复制一份当前上下文」，用 `functools.partial` 把 `copy_context().run` 与目标函数拼成一个新的可调用对象——于是线程里执行的是「在副本上下文里跑 `func`」，`ContextVar` 读到的就是提交时刻的值。

`map` 则是**按元素个数预先复制 N 份上下文**，然后每次调用 `contexts.pop()` 拿一份。这里有两个隐含约束：

1. **`len(iterables[0])`** 假设第一个可迭代对象有长度（`map` 接受任意 iterable，但这里要求它可 `len`）。传一个生成器就会在 `len()` 上抛 `TypeError`。
2. **`pop()` 依赖调用顺序与 `contexts` 的顺序一致**。`ThreadPoolExecutor.map` 是按 iterable 顺序提交任务的，但**执行**顺序不保证——不过每个任务拿到的上下文内容都一样（都是提交时刻的同一份副本），所以顺序无关紧要。这个设计其实可以更简单地「每次调 `copy_context()`」，作者选择预先复制大概是省一次复制。

`get_executor_for_config` 是它的唯一出口：

```
with ContextThreadPoolExecutor(max_workers=config.get("max_concurrency")) as executor:
    yield executor
```

于是 `max_concurrency` 这个 config 键落实成线程数；`None` 时用 `ThreadPoolExecutor` 自己的默认值（`min(32, cpu+4)`）。**所有并行组合器都通过这个上下文管理器拿执行器**——`RunnableParallel.invoke`、`RunnableEachBase`、`RunnableRetry._batch`、`RunnableSequence.batch` 全部如此，于是「配置里的并发上限」在一个地方落实。

**`DynamicRunnable` 把 `configurable` 折叠回对象字段，靠的是「拆包循环」。**

```
def prepare(self, config=None) -> tuple[Runnable[Input, Output], RunnableConfig]:
    runnable: Runnable[Input, Output] = self
    while isinstance(runnable, DynamicRunnable):
        runnable, config = runnable._prepare(merge_configs(runnable.config, config))
    return runnable, cast("RunnableConfig", config)
```

**`while` 而不是 `if`**：一个 `DynamicRunnable` 可以被另一个包住（例如「可配置字段的包装」再套一层「可配置替代项的包装」）。循环逐层解包，每层把自己的 `config` 与当前 config 合并后再交给下一层。最终拿到的是一个**普通 `Runnable`** 加上一条累积起来的 config。

这解释了 `DynamicRunnable` 的所有方法为什么都长这样：

```
def invoke(self, input, config=None, **kwargs):
    runnable, config = self.prepare(config)
    return runnable.invoke(input, config, **kwargs)
```

**每次询问都会解包一次**。`get_input_schema` / `get_output_schema` / `get_graph` / `batch` / `stream` 全都如此——因为「解包结果取决于 config」，而 config 是每次调用时才确定的。

**`RunnableConfigurableFields._prepare` 是「用 config 里的值重建一个实例」。**

```
config = ensure_config(config)
specs_by_id = {spec.id: (key, spec) for key, spec in self.fields.items()}
configurable_fields = {
    specs_by_id[k][0]: v
    for k, v in config.get("configurable", {}).items()
    if k in specs_by_id and isinstance(specs_by_id[k][1], ConfigurableField)
}
configurable_single_options = {
    k: v.options[(config.get("configurable", {}).get(v.id) or v.default)]
    for k, v in self.fields.items() if isinstance(v, ConfigurableFieldSingleOption)
}
configurable_multi_options = {
    k: [v.options[o] for o in config.get("configurable", {}).get(v.id, v.default)]
    for k, v in self.fields.items() if isinstance(v, ConfigurableFieldMultiOption)
}
configurable = {**configurable_fields, **configurable_single_options, **configurable_multi_options}

if configurable:
    init_params = {k: v for k, v in self.default.__dict__.items() if k in type(self.default).model_fields}
    return (self.default.__class__(**{**init_params, **configurable}), config)
return (self.default, config)
```

三件事：

1. **`specs_by_id` 是「配置键 → (字段名, 字段规格)」的反查表**。于是用户写 `configurable={"temperature": 0.9}` 时，`temperature` 是 `ConfigurableField.id`（可能是用户自定义的字符串），而字段名是 `temperature` 本身。这个 indirection 让 id 与字段名可以不同（例如 `id="hub_commit"` 对应字段 `owner_repo_commit`）。
2. **三种可配置类型各走一条路径**：`ConfigurableField` 直接把 id 映射成字段；`ConfigurableFieldSingleOption` 从 `options` 字典里按 `config` 值（缺省用 `default`）取**实际值**——于是用户传的是选项名，得到的是选项对应的对象；`ConfigurableFieldMultiOption` 同理但取一个列表。
3. **重建实例时只带 `model_fields` 里有的键**（`init_params` 过滤）。这是一个必要的防御：`__dict__` 里可能还有非字段的内部缓存（例如 `_client`），把它们传进构造器会炸。然后再叠上 `configurable`，同名键被覆盖。

最终产物有两个：**新的实例**与**原 config**（config 本身没有被改写，`configurable` 仍然保留在里面——所以重建后的实例若也被包了一层 `DynamicRunnable`，还能继续读到这些值）。

**`RunnableConfigurableAlternatives._prepare` 则是一次「选一」加一次「键名重映射」。**

```
which = config.get("configurable", {}).get(self.which.id, self.default_key)
if self.prefix_keys:
    config = {**config, "configurable": {k.removeprefix(f"{self.which.id}=={which}/"): v for k, v in config.get("configurable", {}).items()}}
if which == self.default_key:
    return (self.default, config)
if which in self.alternatives:
    alt = self.alternatives[which]
    if isinstance(alt, Runnable):
        return (alt, config)
    return (alt(), config)
raise ValueError(f"Unknown alternative: {which}")
```

四处：

1. **`which` 的取值**来自 `configurable[which.id]`，缺省是 `default_key`。
2. **`prefix_keys` 做键名前缀剥离**：`f"{self.which.id}=={which}/"` 这个前缀由 `prefix_config_spec` 在向上暴露配置规格时施加（`prefix_keys` 的字段注释给了例子：`temperature` 在名为 `gpt3` 的替代项下变成 `model==gpt3/temperature`）。剥离之后，被选中替代项的 `configurable` 键名回到它自己的命名空间。
3. **替代项可以是 `Runnable` 也可以是「返回 `Runnable` 的可调用对象」**——后者用 `alt()` 延迟构造。这允许「构造替代项本身就昂贵或有副作用」的场景（例如只有真的选中它时才建客户端）。
4. **`which` 未知时 `ValueError`**，而不是退回 `default`。这与 a03 里 `RouterRunnable` 的处理一致：**选择依据是显式给出的，给错了就是配置错误**，静默退回默认值会让拼错的键名成为长期潜伏的 bug。

`config_specs` 那一段还揭示了一个缓存：`_enums_for_spec` 是一个 `WeakValueDictionary`，用 `threading.Lock` 保护，把 `which` 规格对象映射成动态生成的 `StrEnum`（成员就是各替代项名 + `default_key`）。于是界面上「选哪个替代项」是一个下拉框而不是自由文本。用弱引用字典是因为规格对象本身是可回收的——回收后生成的枚举也应当消失，否则会随「每次调用新建可配置对象」的用法无限增长。

**`_get_langsmith_inheritable_metadata_from_config` 是一条在 `config` 与 `metadata` 之间搭的桥。**

```
configurable = config.get("configurable") or {}
metadata = {
    key: value for key, value in configurable.items()
    if not key.startswith("__") and isinstance(value, (str, int, float, bool))
    and key not in config.get("metadata", {}) and key not in CONFIGURABLE_TO_TRACING_METADATA_EXCLUDED_KEYS
}
return metadata or None
```

四个过滤条件：**不以 `__` 开头**（内部键）、**只收标量**（`str`/`int`/`float`/`bool`，于是对象与列表不进去）、**不与已有 metadata 重名**（避免覆盖显式设置）、**不在排除名单里**（该名单只有一个 `api_key`）。

于是「用户设置的可配置标量参数」会自动出现在追踪元数据里。这个设计的动机是「按参数维度筛 trace」——想知道哪些调用用了 `temperature=0`，直接看 metadata。而 `api_key` 被排除是因为它是一个**凭证**，绝不能进追踪；`__` 前缀被排除是因为那是内部标记，不是用户参数。

`# Users are expected to use the context API with a context object (which does not get traced)` 这条注释指出了一个更现代的做法（用 context 对象传参而不被追踪），说明「把 configurable 提升成 metadata」是一个过渡性的便利设计，而不是推荐做法。

`get_callback_manager_for_config` 把这条元数据接进 callback manager：

```
return CallbackManager.configure(
    inheritable_callbacks=config.get("callbacks"),
    inheritable_tags=config.get("tags"),
    inheritable_metadata=config.get("metadata"),
    langsmith_inheritable_metadata=_get_langsmith_inheritable_metadata_from_config(config),
)
```

**注意两个 metadata 参数分开传**（`inheritable_metadata` 与 `langsmith_inheritable_metadata`），说明 callback manager 内部对它们采取不同策略（后者只给 LangSmith handler，不广播给所有 handler）。这个区分避免把用户参数塞进第三方回调——第三方不该看到那些。

## 关键取舍

**源码事实：** `RunnableConfig` 是 `total=False` 的 `TypedDict`，八个键；类注释明确说明 `total=False` 是为了支持部分配置与「靠 `ContextVar` 合并而非替换」的传播。

**源码事实：** `CONFIG_KEYS` 八项；`COPIABLE_KEYS` 四项（四个可变容器），凡是复制 config 时都要 `.copy()`。

**源码事实：** `ensure_config` 铺默认值（`recursion_limit = DEFAULT_RECURSION_LIMIT = 25`）→ 叠 `ContextVar` 配置 → 叠显式配置 → 把不在 `CONFIG_KEYS` 里的键降级进 `configurable`；并把 `configurable` 里的 `model` / `checkpoint_ns` 字符串值提升进 `metadata`。

**源码事实：** `set_config_context` 先 `copy_context()` 再在副本里设置 config，于是调用方上下文不被污染；它同时把 LangSmith 的追踪 parent 设成 callback manager 里 `LangChainTracer.run_map` 命中 `parent_run_id` 的那个 run（三条件全满足才生效）；退出时把追踪上下文重置成一套全 `None` 而不做精确回滚。

**源码事实：** 两处 langsmith 导入都延迟到函数体内，注释给出实测理由「模块级导入约 132ms」。

**源码事实：** `patch_config` 在替换 `callbacks` 时**删除 `run_name` 与 `run_id`**（注释：`run_name` 只对原来那份 callbacks 的那次 run 有效）；`configurable` 是合并，其余标量是替换。

**源码事实：** `merge_configs` 的六套策略：`metadata` 走 `_merge_metadata_dicts`（其中 `lc_versions` 是唯一累加的嵌套映射）、`tags` 集合并排序、`configurable` 浅合并、`callbacks` 按三种形态的两两组合分六种情况、`recursion_limit` 只在非默认值时才写入、其余 `COPIABLE_KEYS` 复制。

**源码事实：** `ContextThreadPoolExecutor.submit` 用 `partial(copy_context().run, func, ...)` 逐任务复制上下文；`map` 按 `len(iterables[0])` 预先复制 N 份上下文并 `pop()` 取用。`get_executor_for_config` 用 `config.max_concurrency` 作为 `max_workers`。

**源码事实：** `DynamicRunnable.prepare` 用 `while` 逐层解包嵌套的 `DynamicRunnable` 并累积合并 config；所有方法（含 schema 与 graph 查询）都先 `prepare` 再转发。

**源码事实：** `RunnableConfigurableFields._prepare` 由 `ConfigurableField` / `SingleOption` / `MultiOption` 三类各走一条路径算出字段值，重建实例时只带 `model_fields` 里存在的键；`RunnableConfigurableAlternatives._prepare` 按 `which` 选一、`prefix_keys` 时剥离 `id==key/` 前缀、替代项可为工厂函数（`alt()`）、未知键抛 `ValueError`。

**源码事实：** `_get_langsmith_inheritable_metadata_from_config` 把 `configurable` 里的标量（排除 `__` 前缀、排除 `api_key`、不与已有 metadata 重名）提升为 LangSmith 专用元数据；`get_callback_manager_for_config` 把它作为独立参数传入。

**架构推论：** 隐式 `ContextVar` 传播 + 显式形参的「双通道」是解决「配置无处不在但签名不能膨胀」的标准办法。代价是隐式通道在跨越线程/协程边界时会断，必须靠 `ContextThreadPoolExecutor` 与 `coro_with_context` 这类工具逐一修桥——而这些桥只要漏掉一处，症状就是「某个并行分支读到的配置不对」，非常难查。

**架构推论：** 「默认值吞掉用户设置」是合并逻辑里最普遍的陷阱。这里用「只在非默认值时才写入」这条特判绕过去了，但那只在默认值是**常量**时可行。若默认值本身是动态的，就需要 `None` 哨兵或显式的 `Optional` 语义来区分「没设」与「设成了默认」。

**架构推论：** `total=False` 让「部分配置」成为合法值，于是「缺键」与「键值为 None」在合并时都必须被当作「没设」处理——`ensure_config` 与 `merge_configs` 里的 `if v is not None` 遍布各处。这与主核里「列可为 NULL」的处理是同一类问题的不同解法：这里把 `None` 当成「未设置」而不是「设置为空」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/runnables/config.py` | `RunnableConfig` | 八个键的 `total=False` TypedDict，部分配置可合并的基础 |
| `libs/core/langchain_core/runnables/config.py` | `CONFIG_KEYS` / `COPIABLE_KEYS` | 「哪些是配置键」与「哪些需要复制」两张表 |
| `libs/core/langchain_core/runnables/config.py` | `var_child_runnable_config` | 隐式传播配置的 `ContextVar`，被 langgraph 依赖 |
| `libs/core/langchain_core/runnables/config.py` | `_set_config_context` / `set_config_context` | 在上下文副本里设 config 与 LangSmith 追踪 parent，退出时清空 |
| `libs/core/langchain_core/runnables/config.py` | `ensure_config` | 默认值 → `ContextVar` → 显式三层叠加，未知键降级进 `configurable` |
| `libs/core/langchain_core/runnables/config.py` | `patch_config` | 替换 callbacks 时清掉 `run_name` / `run_id`；`configurable` 合并 |
| `libs/core/langchain_core/runnables/config.py` | `merge_configs` / `_merge_metadata_dicts` | 六套按键合并策略；`lc_versions` 是唯一累加的嵌套映射 |
| `libs/core/langchain_core/runnables/config.py` | `ContextThreadPoolExecutor` / `get_executor_for_config` | 把上下文带过线程边界，`max_concurrency` 落实为线程数 |
| `libs/core/langchain_core/runnables/config.py` | `_get_langsmith_inheritable_metadata_from_config` | 把 `configurable` 里的标量提升为 LangSmith 元数据（排除 `api_key`） |
| `libs/core/langchain_core/runnables/configurable.py` | `DynamicRunnable` | `prepare` 用 `while` 逐层解包，把 `configurable` 折叠回对象字段 |
| `libs/core/langchain_core/runnables/configurable.py` | `RunnableConfigurableFields` | 三类可配置字段各走一条路径，用 config 值重建实例 |
| `libs/core/langchain_core/runnables/configurable.py` | `RunnableConfigurableAlternatives` | 按 `which` 选一替代项，`prefix_keys` 剥离键名前缀 |

## 参考

- [langchain 源码：runnables/config.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/config.py)
- [langchain 文档：RunnableConfig 与可配置字段](https://python.langchain.com/docs/how_to/configure/)
- [Python 文档：`contextvars`](https://docs.python.org/3/library/contextvars.html)
- [Python 文档：`ThreadPoolExecutor`](https://docs.python.org/3/library/concurrent.futures.html#threadpoolexecutor)

## 与主核 / AI 的关系

「运行配置要能被任意深度的组件读到，又不能让每个函数签名都长出一截」这个问题，在主核里同样存在（会话参数、隔离级别、统计开关、慢查询阈值）。langchain 的解法是 `ContextVar` + 显式形参双通道，这里有三条经验。

**第一，跨越线程/协程边界时必须显式搬运上下文。** `ContextVar` 的语义是「每个执行上下文一份」，新线程继承的是**初始值**而不是父线程的当前值。`ContextThreadPoolExecutor` 就是在补这个认知差。主核里凡是把工作丢进线程池（并行扫描、异步刷盘、后台统计）的地方，只要那段逻辑要读会话级参数，就必须同样搬运——否则读到的是默认值，而症状是「某些并发路径下参数不生效」。

**第二，合并逻辑必须区分「没设」与「设成了某个值」。** 这里的两道防线是「`None` 视为未设」与「等于默认值视为未设」。前者靠 `if v is not None`，后者靠 `patch_config` 里那条特判。主核的配置合并（会话配置 / 表级选项 / 系统默认三层）会遇到同一个陷阱：若默认值参与了合并，用户显式设置的值会被下一层的默认值覆盖，而现象只是「设置不生效」。

**第三，`configurable` 是「把运行期参数折回构造期字段」的通用手法。** 它让同一个对象实例可以在不同调用上表现出不同参数，而不必为每个组合建一个新对象。主核里若做「同一份执行计划按参数微调」，可以借用这个分层：请求携带一份覆盖字典，执行器在开工前重建自己的参数快照。要注意的是这里重建的是**整个实例**（`self.default.__class__(**{**init_params, **configurable})`）——因为它要保证「未被覆盖的字段保持原值」。若只打补丁，就必须自己维护「哪些字段被改过」，而那份状态会随层数增长。

还有一条更细但很有用的：**`api_key` 被显式排除在追踪元数据之外**。任何「自动把参数提升成观测字段」的设计都必须配一张排除名单，且名单里第一个应当是凭证。否则令牌会随 trace 一起落到日志里，而这通常不会被任何测试发现。
