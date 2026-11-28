---
project: vllm
feature: metrics
title: 指标与日志
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/metrics/loggers.py
    symbols: [LoggingStatLogger]
  - path: vllm/v1/metrics/stats.py
    symbols: [SchedulerStats, PrefixCacheStats, RequestStateStats]
---

# 指标与日志

## 一句话职责

`stats.py` 定义「一次迭代产生了哪些可观测数值」，`loggers.py` 把这些数值加工成人类可读的一行日志或 Prometheus 指标——前者是纯粹的数据形状，后者负责聚合、换算与输出。

## 机制是什么

三层结构，缺口很清楚：

| 层 | 内容 | 谁来填 |
|---|---|---|
| 事件层 | `RequestStateStats` / `KVCacheEvictionEvent` | 引擎核心在每个请求上累积 |
| 快照层 | `SchedulerStats` / `IterationStats` | 调度器每步产出 |
| 输出层 | `LoggingStatLogger` / `PrometheusStatLogger` | 按周期消费快照 |

**所有 stats 都是 `@dataclass(slots=True)`。** 它们每步都会被创建、传递、丢弃，`slots=True` 省掉 `__dict__` 是实际的收益——不是风格。

**指标的时间语义在这里被明确规定。** `RequestStateStats` 的字段注释区分了两种时钟：

```python
# This is an engine frontend timestamp (wall-clock)
arrival_time: float = 0.0
# These are engine core timestamps (monotonic)
queued_ts: float = 0.0
scheduled_ts: float = 0.0
first_token_ts: float = 0.0
last_token_ts: float = 0.0
```

**到达时间用墙钟，其余用单调钟。** 墙钟是为了与外部请求时间对齐（客户端看到的时间），单调钟是为了算延迟（不受 NTP 校时跳变影响）。**两种时钟混在同一个结构里时必须靠注释区分，否则算出来的延迟会在校时瞬间变成负数或跳变。**

## 怎么实现的

**`BaseCacheStats` + `CachingMetrics` 是「滑动窗口命中率」的完整实现。**

`BaseCacheStats` 只有四个字段：`reset`、`requests`、`queries`、`hits`。`reset` 是一个**带内信号**——它表示「这批统计之前发生过缓存重置」，消费者看到它要先清空累积值。

`CachingMetrics` 用 `deque` 维护最近 N 次观测：

```python
self.query_queue = deque[tuple[int, int, int]]()
```

`observe` 里有一处注释值得记住：

```python
# DO NOT appending empty stats to avoid helpful info get kicked out
# due to sliding window.
```

**空统计不入队**——否则空闲期的大量空观测会把窗口里真正有信息的历史挤出去，命中率就变成了「最近一千次里全是 0 查询」的无效值。这条规则说明**滑动窗口的容量应该按「有效事件数」而不是「被调用次数」计算**。

`reset` 为真时先 `self.reset()` 再聚合当前批——顺序不能反，否则重置会把刚算进去的这批也清掉。

**`PrefixCacheStats` 把「抢占」单独记账。** 它在基类三个字段之外加了 `preempted_requests` / `preempted_queries` / `preempted_hits`，`record` 里的第一句就是分流：

```python
if preempted:
    self.preempted_requests += 1
    self.preempted_queries += num_tokens
    self.preempted_hits += num_hits
    return
```

**被抢占的请求不算进正常统计。** 因为它们的前缀缓存命中是「第二次机会」而不是「首次命中的质量」，混在一起会让命中率虚高。

**还多一个 `hits_by_source`，并且有严格的守恒校验。**

```python
if sum(hits_by_source.values()) != num_hits:
    logger.warning_once("Connector attributed %s for %d cached tokens; reporting them as %s.", ...)
    hits_by_source = {CacheHitSource.EXTERNAL_UNSPECIFIED: num_hits}
```

**外部连接器报告的按来源分解若与总命中数不符，就整批降级成「来源不明」。** 这是「宁可丢掉分解维度，也不要输出自相矛盾的数字」——评测工具里最常见的错误就是分项之和与总数不等，而这里选择了显式兜底而不是让错误流出去。

**`SchedulerStats` 是快照里字段最多的一个，它的字段分组本身就是结构说明。**

- **队列规模**：`num_running_reqs`、`num_waiting_reqs`、`num_skipped_waiting_reqs`（被瞬时约束跳过的）、`num_preempted_reqs`。三个等待相关的字段各有注释区分语义，其中 `num_skipped_waiting_reqs` 的注释是「waiting requests deferred by transient constraints or a blocked status」——**「在等」与「被跳过」是两种状态**，混成一个数就看不出「有没有请求被饿着」。
- **内部 DP 负载均衡**：`step_counter`、`current_wave`，注释写「These are used for internal DP load-balancing」——**这两个字段不是给人看的，是被别的模块读去做决策的**。指标结构里混入了控制状态。
- **迭代记录**：`iteration_records: list[SchedulerIterationDetails]`，注释写「timed on the GPU by the output worker, or untimed from EngineCore」——**同名字段可能来自 GPU 计时也可能来自核心不加计时**，靠 `elapsed_ms` 是否为 `None` 区分。
- **前缀缓存**：`prefix_cache_stats` 与可选的 `connector_prefix_cache_stats`——**分开是因为两类缓存的重置时机不同**，外部连接器的缓存不受 `reset_prefix_cache` 影响。
- **可选块**：`spec_decoding_stats`、`kv_connector_stats`、`ec_connector_stats`、`cudagraph_stats`、`perf_stats` 全是 `None` 默认。**用「可选且默认为 None」而不是「字段总是存在但值为 0」**，让「这个引擎根本没开投机解码」与「开了但这步没投机解码数据」可区分。

`num_kv_fetch_reqs_by_stage` 用字典而不是三个独立字段，配合模块级常量 `KV_FETCH_STAGES`（三个阶段的名字元组）。**用常量元组约束字典键**，让遍历顺序稳定。

**`SchedulerIterationDetails` 里有一个 `is_dummy` 字段。** docstring 没有解释，但日志格式化处有：

```python
" (dummy)" if details.is_dummy else "",
```

**空跑（dummy）的迭代被显式标记而不是被排除。** 因为 dummy 迭代会消耗 GPU 时间、影响吞吐计算，把它们完全丢掉会让「实际耗时」与「上报吞吐」对不上。

**`RequestStateStats` 有三个「异常标记」字段，它们的组合构成一条诊断链。**

```python
is_corrupted: bool = False          # logits 里有 NaN
is_watermarked: bool = False        # 用了引擎水印采样
is_watermark_skipped: bool = False  # 请求要水印但用不上
```

**三者不能合并成一个枚举**，因为它们可以同时为真/假的不同组合：一个请求可能既损坏又没水印，也可能请求了水印但因不支持而跳过（`is_watermark_skipped=True, is_watermarked=False`）。**用独立布尔而不是状态枚举，是因为这些是互相独立的属性而不是互斥的状态。**

**`LoggingStatLogger` 的核心是「区间累积 + 定期结算」。**

构造时 `self._reset(time.monotonic())` 初始化五个累积量：`num_prompt_tokens`、`num_generation_tokens`、`num_corrupted_reqs`、`num_preemptions`、`num_finished_reqs`。

`record` 只做累积，不做输出：

```python
self.num_prompt_tokens += iteration_stats.prompt_token_stats.computed
```

注意取的是 `prompt_token_stats.computed` 而不是总数，注释写「Use computed tokens for prompt throughput (excludes cached/transferred)」——**吞吐率只算真正算过的 token**，缓存命中与传输过来的都不算，否则吞吐会被前缀缓存虚高。

`log` 才做结算：算吞吐、拼日志、重置。

**吞吐计算里有一个「连续两轮都为 0 才算空闲」的判据。**

```python
self.engine_is_idle = not any((prompt_throughput, generation_throughput, self.last_prompt_throughput, self.last_generation_throughput))
log_fn = logger.debug if self.engine_is_idle else logger.info
```

**四个值里任一非零就不算空闲**——包括上一轮的。也就是说「刚停下来」的那一轮仍然按 `info` 输出，要连续两轮全零才降级成 `debug`。注释写「Avoid log noise on an idle production system」。这个设计避免了「间歇性收到请求时日志在 info/debug 之间来回切」。

`_get_throughput` 里 `delta_time <= 0.0` 时返回 0.0——**防止时钟异常导致的除零或负吞吐**。

**日志是一段「可变长度拼接」，用两个平行列表保证格式串与参数一一对应。**

```python
log_parts = ["Avg prompt throughput: %.1f tokens/s", ...]
log_args = [self.last_prompt_throughput, ...]
if self.last_scheduler_stats.num_skipped_waiting_reqs > 0:
    log_parts.append("Deferred: %d reqs")
    log_args.append(self.last_scheduler_stats.num_skipped_waiting_reqs)
...
log_fn(self.log_prefix + ", ".join(log_parts), *log_args)
```

**每一个条件字段都成对 append**，最后一次性格式化。这个模式让「只有在有意义时才打印」与「参数顺序正确」同时成立——比用 f-string 逐段拼接更不容易出错，因为所有占位符与实参都在同一处可见。

几个条件字段的判据值得记：

- `Deferred` 只在 `num_skipped_waiting_reqs > 0` 时出现；
- `KV fetch` 只在 `any(kv_fetch.values())` 时出现——三个阶段全零就不打；
- `Preemptions` 只在 `> 0` 时出现；
- `Corrupted` 只在 `envs.VLLM_COMPUTE_NANS_IN_LOGITS` 开启时出现（**因为不开启时根本不会统计 NaN**）；
- 三个命中率各自只在 `not empty` 时出现。

**输出后立刻 `self._reset(now)`。** 所以日志里的 `Finished: N reqs` 是「本区间内完成数」而不是累计值。这个语义对读日志的人很关键——想知道总量得自己累加。

**`log_prefix` 用 `f"Engine {self.engine_index:03d}: "` 补零到三位。** 多引擎日志交错时，定宽前缀让它们能对齐排序。

**`record` 里有一处刻意的条件：`if not self.aggregated: self.last_scheduler_stats = scheduler_stats`。**

`self.aggregated` 是基类默认 `False` 的字段，在 `AggregatedLoggingStatLogger` 里会被置真。**所以基类的 `record` 在多引擎聚合场景下不更新 `last_scheduler_stats`**——聚合版的 `aggregate_scheduler_stats()` 会自己算出一份合并后的快照。**同一个 `log()` 代码路径，靠一个布尔开关决定「用自己存的还是用聚合的」**。

**`AggregatedLoggingStatLogger` 是多重继承的组合。**

```python
class AggregatedLoggingStatLogger(LoggingStatLogger, AggregateStatLoggerBase):
```

它复用 `LoggingStatLogger` 的日志格式化（`log`、`_log_iteration` 等），只覆写 `record`、`aggregate_scheduler_stats`、`log_prefix`、`_enable_perf_stats`、`log_engine_initialized`。**这是「模板方法 + 少量覆写」的典型用法**：格式化逻辑一份，聚合逻辑另一份。

`PerEngineStatLoggerAdapter` 是第三种形态：它也继承 `AggregateStatLoggerBase`，但职责是**把一个聚合接口适配成单体接口**——让多引擎输出看起来像逐引擎输出。

**`PrometheusStatLogger` 的 `record` 有近 200 行**（从 `record` 到 `record_admission_rejection`），远超日志版。这个体量差异本身说明：**日志是给人扫一眼的，指标是要被机器长期存储与聚合的**，所以每个维度都要单独注册、单独更新。

**`record_sleep_state` 挂在 `StatLoggerBase` 上且默认是 `# noqa` 的空实现。** 只有 `PrometheusStatLogger` 覆写它——因为「引擎睡着」这个状态对日志没意义（睡着的引擎不打日志），但对指标有意义（需要一个 gauge 表示当前是否睡眠）。

## 关键取舍

**源码事实：** 所有 stats 都是 `@dataclass(slots=True)`。

**源码事实：** `RequestStateStats` 的 `arrival_time` 是墙钟时间戳，`queued_ts` / `scheduled_ts` / `first_token_ts` / `last_token_ts` 是单调钟时间戳，注释分别标明。

**源码事实：** `CachingMetrics.observe` 跳过空统计，注释说明「避免有用信息被滑动窗口挤掉」；`stats.reset` 为真时先重置再聚合。

**源码事实：** `PrefixCacheStats` 把抢占请求单独记账（`preempted_*` 三字段），且 `hits_by_source` 之和与总命中不符时整批降级为 `EXTERNAL_UNSPECIFIED` 并 warning。

**源码事实：** `SchedulerStats` 含 `step_counter` / `current_wave` 两个「内部 DP 负载均衡」用的字段，不是纯观测量。

**源码事实：** `SchedulerIterationDetails.elapsed_ms` 可为 `None`（未计时），`is_dummy` 显式标记空跑迭代。

**源码事实：** `RequestStateStats` 用三个独立布尔标记损坏、已水印、水印跳过。

**源码事实：** 日志吞吐用 `prompt_token_stats.computed`（排除缓存与传输），不用总 token 数。

**源码事实：** `engine_is_idle` 判据是「本轮与上一轮四个吞吐值全为 0」，空闲时降级为 `debug`。

**源码事实：** 日志格式用「格式串列表 + 参数列表」成对 append，各条件字段只在有意义时出现，某些受环境变量控制（如 `Corrupted`）。

**源码事实：** `record` 里 `if not self.aggregated: self.last_scheduler_stats = scheduler_stats`——聚合形态下不更新自身快照。

**架构推论：** 「快照结构里有字段是给别的模块读的」（`step_counter` / `current_wave`）说明指标层与决策层没有完全分离。它的好处是省一次数据传递，代价是指标结构的演进受制于负载均衡的需求——**改一个字段要考虑两个消费者**。

**架构推论：** `hits_by_source` 的守恒校验是「宁可降级也不输出自相矛盾数据」的范例。分项之和 ≠ 总数这类错误在监控系统里会静默传播：图表照画、告警照配，只是数字永远对不上。**在数据进入管道之前做一次守恒检查，比事后排查便宜得多。**

**架构推论：** 「滑动窗口按有效事件计数而非调用次数」这条规则适用于所有速率类指标。若按调用次数计，空闲期的空采样会稀释窗口，让所有窗口内指标变成「最近 N 次里有 M 次有数据」的加权平均——**这在低流量服务上会让指标看起来总是接近 0**。

**架构推论：** 用「可选且默认 None」表达「可能不存在」而不是用 0，是本页最值得复制的一条。**0 与「没有」在聚合时的行为完全不同**：求平均时 0 会拉低均值，`None` 会被跳过。指标系统里把「未采集」写成 0 是导致误告警最常见的原因之一。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/metrics/stats.py` | `BaseCacheStats` | 缓存统计基类：重置标志 + 请求/查询/命中 |
| `vllm/v1/metrics/stats.py` | `CachingMetrics` | 滑动窗口命中率，跳过空观测 |
| `vllm/v1/metrics/stats.py` | `PrefixCacheStats` | 前缀缓存统计，抢占单独记账 + 来源守恒校验 |
| `vllm/v1/metrics/stats.py` | `KVCacheEvictionEvent` | 单块驱逐样本（存活/空闲/复用间隔） |
| `vllm/v1/metrics/stats.py` | `SchedulerIterationDetails` | 单迭代详情，含可空耗时与空跑标记 |
| `vllm/v1/metrics/stats.py` | `SchedulerStats` | 调度器快照：队列、缓存、可选子统计 |
| `vllm/v1/metrics/stats.py` | `RequestStateStats` | 跨增量更新的请求状态，墙钟与单调钟并存 |
| `vllm/v1/metrics/stats.py` | `KV_FETCH_STAGES` | KV 拉取三阶段的常量元组（约束字典键） |
| `vllm/v1/metrics/loggers.py` | `StatLoggerBase` | 日志器接口（record / log / 空实现钩子） |
| `vllm/v1/metrics/loggers.py` | `LoggingStatLogger` | 区间累积 + 定期结算的文本日志 |
| `vllm/v1/metrics/loggers.py` | `LoggingStatLogger._track_iteration_stats` | 只用 computed token 算吞吐 |
| `vllm/v1/metrics/loggers.py` | `LoggingStatLogger._calc_throughput` | 空闲判据（本轮 + 上轮全零） |
| `vllm/v1/metrics/loggers.py` | `LoggingStatLogger.log` | 动态拼装日志字段并输出后重置 |
| `vllm/v1/metrics/loggers.py` | `AggregatedLoggingStatLogger` | 多引擎聚合版，复用格式化逻辑 |
| `vllm/v1/metrics/loggers.py` | `PrometheusStatLogger` | 指标注册与逐项更新 |
| `vllm/v1/metrics/prometheus.py` | `setup_multiprocess_prometheus` | 多进程指标目录初始化 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/v1/metrics/stats.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/metrics/stats.py)
- [vllm 源码：vllm/v1/metrics/loggers.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/metrics/loggers.py)
- [Prometheus 文档：指标类型与多进程模式](https://prometheus.io/docs/instrumenting/writing_exporters/)

## 与主核 / AI 的关系

「快照结构 + 定期结算」与主核里「统计视图 + 后台 autovacuum 采样」是同一结构：**执行路径只累积廉价计数，昂贵的聚合与输出放到周期任务里**。主核的 `pg_stat_*` 视图读的是共享内存里的计数器，vllm 的 logger 读的是 `SchedulerStats` 快照——共同点是**采样不影响热点路径**。

**「0 与未采集」的区别 vs 主核的 NULL。** 数据库用三值逻辑区分「未知」与「零」；指标系统若把未采集写成 0，聚合结果就会偏离。vllm 用「默认 None 的可选子统计」实现了同样的区分：`spec_decoding_stats is None` 表示「没开投机解码」，`spec_decoding_stats.num_draft_tokens == 0` 表示「开了但这步没用上」。

**滑动窗口按有效事件计 vs 主核的统计信息采样。** 主核收集统计时只在数据变化时更新（避免全表扫描），vllm 的 `CachingMetrics` 只在有实际查询时入队。**两者都在避免「空事件污染估计」**——这是所有采样式统计的共同纪律。

对 agent 工程最直接的价值有四条。其一：**指标的「未采集」必须与「0」区分**，用可选类型而不是默认零值。其二：**速率类指标要按有效事件建窗口**，不要按调用次数。其三：**分项与总数要做守恒校验**，不成立时宁可降级也不要输出矛盾数据——监控里最难查的错误就是数字永远对不上。其四：**日志的空闲降级要留一轮缓冲**，否则间歇流量会让日志级别反复切换、淹没真正有用的行。
