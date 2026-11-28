---
project: vllm
feature: kv-offload
title: KV cache 落盘与分层
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/kv_offload/base.py
    symbols: [RequestOffloadingContext, OffloadPolicy, LookupResult]
  - path: vllm/v1/kv_offload/config.py
    symbols: [OffloadingConfig, OffloadingModelConfig, OffloadingGroupConfig]
  - path: vllm/v1/kv_offload/factory.py
    symbols: [OffloadingSpecFactory]
---

# KV cache 落盘与分层

## 一句话职责

把 GPU 上放不下的 KV cache 搬到 CPU 内存、本地磁盘或远端存储，并在后续请求命中同一前缀时搬回来：`OffloadingManager` 管「哪些块在外部、能不能读、要不要淘汰」，`OffloadingWorker` 管真正搬字节，`OffloadingSpec` 把两者绑在一起，`OffloadingConfig` 是喂给它的归一化配置。

## 机制是什么

卸载（offload）在概念上很简单：GPU 满了就把冷块挪走。但一旦落地就出现四个必须分开的问题：

| 问题 | 谁来答 | 关键约束 |
|---|---|---|
| 这个块在外部能不能读？ | `OffloadingManager.lookup` | 可能在搬、可能待重试 |
| 外部空间满了淘汰谁？ | `prepare_store` 返回 `evicted_keys` | 淘汰与写入是同一次决策 |
| 谁真正搬字节？ | `OffloadingWorker` | 异步，有 job id |
| 配置从哪来？ | `OffloadingConfig` | 归一化，与具体后端解耦 |

**「管理」与「传输」被切成两个接口，跑在两个地方。** `OffloadingManager` 的 docstring 写明它「runs in the scheduler」——它在调度进程里，只维护元数据，一次搬字节都不做。真正搬数据的是 worker，跑在 GPU worker 进程上。这个切分的理由是显然的：调度器必须能**同步地**回答「这个前缀命中了吗」，而传输只能是异步的。

**「命中」不是一个布尔值，而是四态。**

```python
class LookupResult(Enum):
    MISS = auto()
    HIT = auto()
    HIT_PENDING = auto()
    RETRY = auto()
```

四态的存在反映了一个真实场景：**外部存储的元数据可能已经登记，但字节还没搬到可读位置**（正在从更低一级的层提升，或者正在写入）。此时不能报 MISS（否则会白算一遍），也不能报 HIT（因为还读不了）。`HIT_PENDING` 正是这个中间态，`RETRY` 则让调度器稍后再试。

**这四种状态是「异步分层存储」的必然产物**，不是过度设计：只要外部存储有多级（CPU → 磁盘 → 远端），就一定有「已登记但未就绪」的窗口。

## 怎么实现的

### 配置：把「拓扑」和「后端」分开

`OffloadingConfig` 有九个字段，但真正决定后端能做什么的是嵌套的四个子配置：

| 子配置 | 关键字段 | 影响什么 |
|---|---|---|
| `OffloadingModelConfig` | `name`、`dtype`、`max_model_len` | 决定单块字节数 |
| `OffloadingGroupConfig` | `tokens_per_block`、`layer_names`、`sliding_window_size_in_chunks` | 决定一个块覆盖多少 token |
| `OffloadingCacheConfig` | `tokens_per_hash`、`blocks_per_chunk` | 决定哈希粒度与合并写入的块数 |
| `OffloadingParallelConfig` | 十一个并行度字段 + `is_parallelism_agnostic` | 决定字节布局是否可跨拓扑复用 |

**`OffloadingGroupConfig.sliding_window_size_in_chunks` 的类型是 `int | None`，`None` 的语义写在注释里**：「attention reaches back without a bound (a full layer)」。也就是说：

- 有界（滑窗）：只保留窗口内的 chunk，可以按环形复用外部空间。
- `None`（全注意力）：必须保留全部，外部空间随序列线性增长。

**这个 `None` 是后端做容量规划的唯一依据**——它决定了「一个请求可能要占多少外部空间」，而这个判断必须在没有任何请求时为真。

最值得单独看的是 `OffloadingParallelConfig.is_parallelism_agnostic`，它的注释把两种布局的区别讲透了：

> True when the bytes that will be persisted for a block are portable across parallelism configurations: for the direct layout, concatenating the block's data across all workers in rank order yields the same bytes under any topology; for the canonical layout, the canonical page itself is topology-free.

**「落盘的字节能否跨并行配置复用」是一个必须显式建模的属性。** 直接布局下要按 rank 顺序拼接才等价，规范布局下每页本身就与拓扑无关。若这个属性为真，后端可以做「单副本主机布局」（`replicated_layout`）省一倍内存；若为假，每个 rank 都得存一份。

`replicated_layout` 与 `canonical_layout` 两个布尔字段的注释都提到「per-layer 认证在 worker 注册时完成」——**说明这是「配置声明 + 运行期校验」的组合**：调度器按声明规划，worker 真起来时再核对，不一致就报错。

### Manager：钩子极多，但只有三个是必须的

`OffloadingManager` 是 ABC，但只有四个方法是 `@abstractmethod`：`lookup`、`prepare_load`、`prepare_store`、`on_new_request`。其余十几个都有默认实现（返回 `None` / `()` / `False` / `nullcontext()`）。

这个设计让「最小可用后端」只需要实现四件事，而高级能力（事件、指标、重置、每步收尾）都是可选覆写。

**锁的约定写得异常清楚，且是「调用方持有」而非「方法内加锁」**：

```python
@property
def lock(self) -> AbstractContextManager[Any]:
    """...
    Every caller of an OffloadingManager method, except shutdown(), must hold it.
    Manager methods never enter it themselves, so a caller may hold it across an
    arbitrary sequence of operations -- which is required wherever one call
    establishes a fact a later call relies on, such as a lookup() HIT followed
    by the prepare_load() that pins it.
    ...
    Callers must not assume either: always enter it, and never re-enter it while
    held, since a real lock need not be reentrant.
    """
    return nullcontext()
```

这段注释解决了一个具体问题：**`lookup` 返回 HIT 与 `prepare_load` 把它钉住之间，不能被别的线程淘汰掉**。如果各方法自己加锁，这两步之间就有窗口。所以锁必须由调用方跨两步持有。

而默认实现是 `nullcontext()`——**「单线程场景下这把锁根本不存在」**。这带来一个隐蔽的约定：调用方必须**无条件** `with manager.lock:`，即使它知道实现是单线程的。因为「实现是单线程的」这件事可能明天就变。

`shutdown()` 是唯一例外：「call this without it. A manager with a background thread stops that thread here and may need the lock to do so.」——**关停要停线程，而停线程需要锁，所以关停必须在锁外调用。** 这是一条只能靠注释传达的规则。

**`on_request_finished` 的语义边界也标得很细**：「By the time this is called, the scheduler will issue no more submit-side calls for this request」，但「Completion callbacks for already-submitted transfers may still arrive afterward」，而且「This hook does NOT imply the data has been persisted」。

**这三句话把「结束」的三种含义区分开了**：不再提交新任务 ≠ 已提交的都完成了 ≠ 数据已持久化。任何做分层缓存的系统都需要这三层区分，而混用会导致「以为数据在盘上其实还在内存」这类故障。

### prepare_store 的三个返回字段各有故事

```python
@dataclass(slots=True)
class PrepareStoreOutput:
    keys_to_store: list[OffloadKey]
    store_spec: LoadStoreSpec
    evicted_keys: list[OffloadKey]
    keys_to_retry: list[OffloadKey] = field(default_factory=list)
```

前三个是「要写这些、写到这、顺便淘汰这些」。**「淘汰」与「写入」在同一次调用里返回**，因为空间不够时它们必须一起决策——你没法先答应写入再回头问「我刚才该淘汰谁」。

第四个 `keys_to_retry` 的注释是全文件最需要读两遍的：

> Offered keys to offer again because the manager had no room. Within a group (get_offload_group_idx), they are the offered keys from the first one that did not fit on, by token position, including keys that are already stored. Disjoint from keys_to_store.

三个要点：**从第一个装不下的位置起**（不是「所有装不下的」）、**按 token 位置顺序**（保证下次还是前缀连续的）、**包含已经存过的**（因为前缀必须连续，中间有洞就没法后续命中）。最后一句「Disjoint from keys_to_store」保证了两个集合不重叠，调用方可以直接把 retry 留到下一轮。

**这段语义只能在注释里说清**，因为类型上是两个 `list[OffloadKey]`，看不出任何约束。

### 事件与两级所有权

`OffloadingEvent` 有七个字段，其中三个是给分层用的：

- `ownership`：「Secondary tier identifier that generated the event, or None for primary.」
- `removal_expected`：删除是预期的还是意外的（外部因素导致）。
- `metadata_only`：「Failed allocations release metadata without announcing a public eviction.」

而 `take_events` 的 docstring 规定了分层聚合的边界：「A tier manager emits only events for storage state it owns. A composing manager may aggregate child event streams, but should not synthesize events on behalf of a child tier.」

**「可以聚合，不能代造」**——这条约束保证了事件的来源可信。若上层能替下层编造事件，那事件的 `ownership` 字段就失去意义。

`reset_cache` 与 `on_reset_flushed` 是一对，后者的 docstring 说明了它的精确时机：「Called once workers have reported every job in flight at the last reset_cache(); blocks it freed are no longer written by them.」

**重置是两阶段而不是一步**：先宣告重置（清元数据），等所有在途任务回报（确认没有 worker 还在往已释放的块里写），才算真完成。这是一个典型的「异步资源的撤销」模式——**撤销命令可以立刻下，撤销生效必须等到最后一封回执。**

### 两级判定：策略与触发

`OffloadPolicy` 只有两个值，但它们的差别是「要不要重复存」：

- `CHUNK_LEVEL`：「Offload only newly-computed chunks as they arrive; prefix-hit chunks (already offloaded by a prior request) are skipped.」
- `REQUEST_LEVEL`：「Offload all chunks for the request, including prefix hits.」

**`CHUNK_LEVEL` 是默认**（`RequestOffloadingContext.policy` 的默认值），因为它省 IO。而 `REQUEST_LEVEL` 存在的原因是某些层「需要完整的 KV 上下文」——例如做整段迁移或做校验的场景。

`StoreTrigger` 则区分「为什么现在要存」：`ON_COMPUTE`（块算完了，从容地存）与 `ON_HANDOFF`（KV 管理器要把状态块交出去复用，**源马上要消失，不能延后**）。

**「源数据会不会消失」决定了能否批处理。** `ON_COMPUTE` 可以攒一批再写以摊薄开销，`ON_HANDOFF` 必须立刻写。

### Key 与位置：为乱序到达做的准备

`ReqContext` 里有 `_offload_key_positions: dict[OffloadKey, int]`，注释解释：

> End-token position for each key in this request. The scheduler records these positions so managers can recover prefix order even when store calls arrive out of order (for example, SWA backfills).

**「store 调用的到达顺序不等于前缀顺序」**，因为滑窗需要回填（backfill）。所以位置必须由调度器显式记录，而不是靠调用顺序推断。这是一个容易被忽略但一定会踩的坑：**异步系统里，事件的到达顺序不是语义顺序。**

`ReqContext._state` 是另一处实用设计：「Per-request scratch space keyed by value type, so a tier can parse kv_transfer_params and kv_hints once (in on_new_request) and read the result back on later calls」。用 `dict[type, Any]` 做键——**一个类型只能存一份**，这对「解析一次请求参数」这个用途刚刚好，也不需要定义新类型。

而 `num_computed_tokens` / `num_prompt_tokens` 等四个属性在基类里全返回 `None`，注释说「Counts are live until the scheduler finishes the request, then frozen」。**基类返回 `None` 意味着「这个信息不可用」**，需要它的策略必须处理 `None`——同 b02 那页的 `None` 约定。

### 工厂：注册表 + 延迟导入 + 两档兜底

`OffloadingSpecFactory` 只有三个方法，但把「内置」与「外部」两条路都照顾到了。

`register_spec(name, module_path, class_name)` 存的是**一个闭包而不是类**：

```python
def loader() -> type[OffloadingSpec]:
    module = importlib.import_module(module_path)
    return getattr(module, class_name)
cls._registry[name] = loader
```

**延迟导入的意义在于「不注册也要能省掉导入开销」**：所有内置 spec 都在模块末尾注册，但只有被用到的那个才会真正 import 它的模块。这对一个可能有十几个后端的连接器体系是必要的——否则光是 import 就会拖慢启动。

`get_spec_cls` 有两档兜底：

1. 在注册表里 → 调 loader 拿到类。
2. 不在注册表但给了 `spec_module_path` → 直接 `importlib.import_module(...)` + `getattr`，并打两条日志：一条 `warning_once` 说「这是实验性 API，未来可能变」，一条 `info` 记录加载了什么。
3. 都没有 → `raise ValueError(f"Unsupported spec type: {spec_name}")`。

第 2 档的存在是「允许树外扩展」，而 `warning_once` 的措辞（「experimental and subject to change」）是**给使用者的免责声明**。最后还有一句 `assert issubclass(spec_cls, OffloadingSpec)`——**动态加载的类型必须过一道 `issubclass` 检查**，否则后面所有调用都可能 AttributeError。

`spec_name` 的默认值是 `"CPUOffloadingSpec"`——**不配就用 CPU 卸载**。这是一个友好的默认（CPU 内存几乎总是可用的），但它也意味着「配错了名字且没给 module_path」会直接用 CPU 版本而不报错……不，实际会报错：注册表里没有那个名字时走第 2 档，`spec_module_path is None` 就抛 `ValueError`。**默认值只在这个键完全缺失时生效。**

## 关键取舍

**源码事实：** `lookup` 返回四态（`MISS`/`HIT`/`HIT_PENDING`/`RETRY`），因为外部存储存在「已登记但不可读」的窗口。

**源码事实：** `lock` 由调用方持有且可跨多次调用，方法内不自锁；默认实现是 `nullcontext()`；`shutdown` 是唯一要求在锁外调用的方法。

**源码事实：** `prepare_store` 同时返回「要写」「写到哪」「淘汰了谁」「哪些下次再试」，且 `keys_to_retry` 从第一个装不下的位置起、按 token 位置、包含已存过的。

**源码事实：** `on_request_finished` 明确区分「不再提交新任务」「已提交的完成」「数据已持久化」三件事。

**源码事实：** `reset_cache` 与 `on_reset_flushed` 构成两阶段重置。

**源码事实：** `OffloadingGroupConfig.sliding_window_size_in_chunks` 为 `None` 表示无界（全注意力），是容量规划的唯一依据。

**源码事实：** `is_parallelism_agnostic` 描述落盘字节能否跨拓扑复用，直接决定后端能否用单副本布局。

**源码事实：** `ReqContext` 显式记录每个 key 的 token 结束位置，因为存调用可能乱序到达（滑窗回填）。

**源码事实：** 工厂用闭包做延迟导入，未注册的名字可通过 `spec_module_path` 动态加载，加载后 `assert issubclass`。

**架构推论：** 「管理在调度器、传输在 worker」这个切分是所有分层缓存的默认形态，因为**元数据必须能同步查询，而数据搬运只能是异步的**。它带来的结构后果是：调度器要维护一份「外部有什么」的镜像，而这份镜像必然与真实状态有滞后——`HIT_PENDING` 与 `RETRY` 四态就是这个滞后的显式化。

**架构推论：** 「锁由调用方持有」把组合操作的正确性责任交给了最了解语义的那一层。这是对的，因为只有调用方知道「这两步必须原子」。代价是**每个方法都多了一个对调用者的隐含要求**，而违反它不会立刻报错，只会在并发下偶发失效。

**架构推论：** `keys_to_retry` 的「从第一个装不下处开始」是一条很硬的语义：它保证了重试集合总是前缀连续的。如果允许「隔一个存一个」，外部存储里就会留下无法命中的碎片——**前缀缓存的价值完全建立在「连续」上，任何破坏连续性的优化都会让缓存失效。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/kv_offload/base.py` | `OffloadingManager` | 调度器侧的元数据管理接口 |
| `vllm/v1/kv_offload/base.py` | `OffloadingManager.lock` | 调用方持有的可组合锁，默认空实现 |
| `vllm/v1/kv_offload/base.py` | `OffloadingManager.lookup` | 四态命中查询 |
| `vllm/v1/kv_offload/base.py` | `OffloadingManager.prepare_store` | 写入决策与淘汰的联合返回 |
| `vllm/v1/kv_offload/base.py` | `OffloadingManager.on_request_finished` | 三层「结束」语义的边界 |
| `vllm/v1/kv_offload/base.py` | `LookupResult` | 命中、待就绪、重试、未命中 |
| `vllm/v1/kv_offload/base.py` | `OffloadPolicy` | 只存新块 vs 存全部 |
| `vllm/v1/kv_offload/base.py` | `StoreTrigger` | 从容存 vs 源即将消失 |
| `vllm/v1/kv_offload/base.py` | `RequestOffloadingContext` | 每请求的卸载策略 |
| `vllm/v1/kv_offload/base.py` | `PrepareStoreOutput` | 写入、淘汰、重试三者的打包 |
| `vllm/v1/kv_offload/base.py` | `OffloadingEvent` | 带所有权与预期标记的状态变更事件 |
| `vllm/v1/kv_offload/base.py` | `ReqContext` | 每请求上下文、位置记录与状态暂存 |
| `vllm/v1/kv_offload/base.py` | `OffloadingWorker` | 真正搬字节的异步执行者 |
| `vllm/v1/kv_offload/base.py` | `OffloadingSpec` | 把 manager 与 worker 绑在一起的规格 |
| `vllm/v1/kv_offload/config.py` | `OffloadingConfig` | 归一化的后端无关配置 |
| `vllm/v1/kv_offload/config.py` | `OffloadingGroupConfig` | 组级 token 跨度与滑窗有界性 |
| `vllm/v1/kv_offload/config.py` | `OffloadingParallelConfig` | 并行拓扑与字节可移植性 |
| `vllm/v1/kv_offload/config.py` | `OffloadingModelConfig` | 模型标识与 dtype |
| `vllm/v1/kv_offload/factory.py` | `OffloadingSpecFactory` | 延迟导入注册表与动态兜底 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：v1/kv_offload/base.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/kv_offload/base.py)
- [vllm 源码：v1/kv_offload/config.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/kv_offload/config.py)
- [vllm 文档：KV cache 卸载与连接器](https://docs.vllm.ai/en/latest/features/kv_cache_offloading.html)

## 与主核 / AI 的关系

「元数据在调度器、数据在 worker」与主核里「目录缓存（catalog cache）在内存、页数据在磁盘」是同一个分工：**查询「有没有」必须快且同步，取「内容」可以慢且异步**。主核的目录缓存也有「已删除但还有人引用」的中间态，与 `HIT_PENDING` 的处境同源——都是「状态声明与物化进度不同步」的产物。

`on_request_finished` 那句「不暗示数据已持久化」，与主核里「事务提交不暗示脏页已刷盘」是同一层次的区分。数据库用 WAL 保证崩溃可恢复，所以「提交」有明确含义；卸载层没有 WAL，于是「已存」只能靠 `complete_store` 这个显式回执。**没有日志的异步写入，只能靠回执来确认。**

`reset_cache` / `on_reset_flushed` 的两阶段撤销，与主核里「DROP TABLE 先标记、等所有引用释放再真正删除」同理。数据库用的是引用计数 + 延迟回收；这里用的是「等所有在途 job 回报」。**撤销不可立即生效，只能「宣告 + 等待」。**

对 agent / RAG 工程的实际含义有四条。其一，外部缓存命中不等于立刻能读，`HIT_PENDING` 期间调度器要么等要么重算，这影响首 token 延迟。其二，`keys_to_retry` 只从第一个装不下的位置起重试，所以外部空间不足时**尾部的块会一直存不进去**，而不是随机丢一些。其三，滑窗层的 `sliding_window_size_in_chunks` 有界，意味着这类模型的外部缓存可以按环形复用，容量不随对话长度增长。其四，落盘字节默认可能依赖并行拓扑，换部署形态（TP/PP 数变化）后老缓存可能直接不可用——`is_parallelism_agnostic` 为假时这是必然的。
