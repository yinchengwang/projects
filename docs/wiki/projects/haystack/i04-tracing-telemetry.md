---
project: haystack
feature: tracing-telemetry
title: 追踪与遥测
page_type: feature
schema_version: 1
refs:
  - path: haystack/tracing/tracer.py
    symbols: [Tracer, Span, enable_tracing, disable_tracing]
  - path: haystack/tracing/logging_tracer.py
    symbols: [LoggingTracer, LoggingSpan]
  - path: haystack/telemetry/_telemetry.py
    symbols: [Telemetry, pipeline_running]
---

# 追踪与遥测

## 一句话职责

给整个框架铺一套「零成本可关」的追踪抽象：全局导出的名字是一个**代理对象**，它内部持有真 tracer，而默认真 tracer 是**空实现**——于是业务代码可以无条件地 `with tracing.tracer.trace(...)`，关掉追踪时只是跑几个空函数；同时另有一套完全独立的匿名用量统计，带每分钟一次的节流与「永不打断业务」的保护。

## 机制是什么

两套东西**互不相干**，只是同属可观测性：

**追踪**（`tracing/`）是**开发者的**工具，用来定位性能与调用结构。核心是四层：

1. `Span`（抽象基类）——`set_tag` 抽象、`set_tags` / `set_content_tag` / `raw_span` / `get_correlation_data_for_logs` 有默认实现。
2. `Tracer`（抽象基类）——`trace`（`@contextmanager`）与 `current_span` 两个抽象方法。
3. `ProxyTracer` / `NullTracer` / `NullSpan` ——三个具体实现。
4. 模块级单例 `tracer: ProxyTracer = ProxyTracer(provided_tracer=NullTracer())`。

**遥测**（`telemetry/`）是**框架作者的**工具，用来知道功能被用得如何。核心是 `Telemetry` 类（posthog 客户端封装）+ `send_telemetry` 装饰器 + `pipeline_running` 收集函数。

## 怎么实现的

**代理模式解决的是「用户可能已经 `from haystack.tracing import tracer` 拿走了引用」这个问题。** 模块底部的注释把动机写得很直接：

「我们用代理模式来允许方便地启用与停用追踪而**不必替换全局 tracer 实例**。这尤其在用户直接 import 了这个对象时很方便（那种情况下我们得在所有模块里 monkey-patch 它）」。

所以：

```python
tracer: ProxyTracer = ProxyTracer(provided_tracer=NullTracer())

def enable_tracing(provided_tracer: Tracer) -> None:
    tracer.actual_tracer = provided_tracer

def disable_tracing() -> None:
    tracer.actual_tracer = NullTracer()

def is_tracing_enabled() -> bool:
    return not isinstance(tracer.actual_tracer, NullTracer)
```

三个函数都只改 `tracer.actual_tracer` 这一个字段。所以「启用追踪」在任何时刻都对**已经持有 `tracer` 引用的代码**生效——因为它们持有的是代理，代理每次调用都转发给当前的 `actual_tracer`。

这是这一层的核心设计：**用一次间接调用换掉「全局引用替换」的难题**。代价是每次 `trace` 多一层函数调用（可忽略），以及代理必须实现 `Tracer` 的全部方法并转发（这里是两个）。

`is_tracing_enabled()` 的实现是 `not isinstance(..., NullTracer)` ——一个**类型判断**而不是布尔字段。理由是：不能只用一个 `enabled` 布尔，因为它需要与「什么 tracer 被装上」保持一致；用 `isinstance` 就只有一个真相源。

**`NullTracer` 的零成本体现在两处，其中一处的语义值得细看。**

```python
@contextlib.contextmanager
def trace(self, operation_name, tags=None, parent_span=None):
    yield NullSpan()

def current_span(self) -> Span | None:
    return NullSpan()
```

`trace` 里三个参数全标了 `# noqa: ARG002`（未使用参数）——这正说明它什么都不做。而 `current_span()` 返回的是一个 `NullSpan` **而不是 `None`**，尽管返回类型标注是 `Span | None`。

这个选择有实际后果：调用方拿到 `NullSpan` 之后调 `set_tag` / `set_content_tag` 都安全（空操作），所以**业务代码不需要判断 `current_span()` 是否为空**。若返回 `None`，每个使用者都得写 `if span: span.set_tag(...)`。让空对象永远非 `None`，消除了这类判空。

而 `NullSpan` 只实现了 `set_tag`，其余继承自 `Span` 的默认实现——`set_tags` 会循环调 `set_tag`（空操作），`set_content_tag` 会检查 `tracer.is_content_tracing_enabled` 再调 `set_tag`（也是空操作），`raw_span` 返回 `self`，`get_correlation_data_for_logs` 返回 `{}`。所以这一串默认实现在空实现下自然退化成空操作，不需要逐个覆盖。

**`set_content_tag` 是一个「默认关闭的内容开关」，它的默认实现就写在基类里。**

```python
def set_content_tag(self, key: str, value: Any) -> None:
    if tracer.is_content_tracing_enabled:
        self.set_tag(key, value)
```

docstring 把「什么是内容」列了出来：「内容属于敏感信息，例如查询的内容、文档的内容、答案的内容」。两条启用方式：设环境变量 `HAYSTACK_CONTENT_TRACING_ENABLED=true`，或在自定义 tracer 里覆盖 `set_content_tag`。

而 `is_content_tracing_enabled` 是 `ProxyTracer.__init__` 里算的一次：

```python
self.is_content_tracing_enabled = os.getenv(HAYSTACK_CONTENT_TRACING_ENABLED_ENV_VAR, "false").lower() == "true"
```

所以它在**构造时读一次**（也就是模块 import 时），之后改环境变量不生效。这是一个明确的取舍：追踪开关的读取成本被摊到零，代价是「运行中改环境变量无效」——而后者本来也不是正常用法。

注意这个字段放在**代理**上而不是 `Span` 上，所以 `set_content_tag` 的默认实现读的是全局代理的字段。这让「内容开关」是进程级的一个值，与具体 tracer 无关。

**`LoggingTracer` 是唯一的内置实现，而它的 `current_span()` 永远返回 `None`。**

```python
def current_span(self) -> Span | None:
    # we don't store spans in this simple tracer
    return None
```

这与 `NullTracer` 返回 `NullSpan` 形成对照——所以「`current_span()` 可能为 `None`」这件事在 `LoggingTracer` 下是真的。任何依赖 `current_span()` 的代码必须能处理 `None`。

而这正是 h02 里 `_create_tool_span` 与 i03 里 `_create_hook_span` 都用 `parent_span` 显式传参的原因之一：若靠 `current_span()`，在 `LoggingTracer` 下永远是 `None`，在别的 tracer 下又可能被并发污染。显式传参让两种 tracer 下行为一致。

**`LoggingTracer.trace` 的日志在 `finally` 里打，且用一把锁保护输出。**

```python
custom_span = LoggingSpan(operation_name, tags=tags or {})
try:
    yield custom_span
except Exception as e:  # noqa: TRY203
    raise e
finally:
    operation_name = custom_span.operation_name
    tags = custom_span.tags or {}
    with self._emit_lock:
        logger.debug("Operation: {operation_name}", operation_name=operation_name)
        for tag_name, tag_value in tags.items():
            ...
```

四件事各有一个理由：

1. **在 `finally` 里打** ——注释写明：「我们确保在上下文管理器退出时记录 span 的操作名与标签，**成功与出错两种情况**都记」。所以一个失败的 span 也会被记录下来。

2. **从 `custom_span` 重读 `operation_name` 与 `tags`** ——而不是用入参。因为 span 在 `yield` 期间可能被 `set_tag` 加了标签，也可能（理论上）被改名。重读才拿得到最新的。

3. **`except Exception as e: raise e` 是空转发，带 `# noqa: TRY203`** ——它的唯一作用是让 `except` 块存在，从而在语义上标明「异常不被处理、只用于让 `finally` 感知」。实际上 `finally` 不需要 `except` 也一定执行，所以这个块是冗余的，但它起到了注释的作用。lint 的 `TRY203` 规则正是为此存在（提示这个 `except` 没有做任何事）。

4. **`with self._emit_lock`** ——而构造函数的注释解释了这把锁的来源：「span 可以从并行的工作线程里被创建与关闭（例如 Agent 里每个工具调用一个 span）。一个 span 的操作名与它的标签被发出为**分开的日志记录**，所以我们把发出动作串行化，让每个 span 的记录保持连续，而不是与并发关闭的其他 span 交织」。

这是这一层最有价值的一处细节：**一段逻辑输出多条日志记录时，并发会让它们交错**。结果是「Operation: A」后面跟着「B 的标签」——日志完全没法读。用一次锁把「一个 span 的全部记录」变成一个原子段，输出的可读性就恢复了。

而代价是：所有 span 的日志发出被串行化。对 `debug` 级别日志这是可接受的（本来也不该在高频路径开）。

**`coerce_tag_value` 把任意值压成四种类型之一。** `Span.set_tag` 的 docstring 说「值会被序列化成字符串，所以最好用简单类型」，而实现上是三层：

```python
if isinstance(value, PRIMITIVE_TYPES): return value      # bool/str/int/float 原样
if value is None: return ""
try:
    serializable = _serializable_value(value=value, use_placeholders=True)
    return json.dumps(serializable)
except Exception as error:
    logger.debug(...)
    return str(value)
```

`PRIMITIVE_TYPES = (bool, str, int, float)` ——正好是 JSON 的标量类型（不含 `None`，因为 `None` 单独处理成空字符串）。而 `None → ""` 而不是 `"null"` 是一个选择：追踪后端里空字符串比字符串 `"null"` 更不容易被误解为真实值。

`_serializable_value` 有两个回退点值得看：

```python
if use_placeholders and getattr(value, "_to_trace_dict", None):
    return _serializable_value(value._to_trace_dict())
if getattr(value, "to_dict", None):
    return _serializable_value(value.to_dict())
return value
```

所以对象有两种「把自己变成可追踪形状」的方式：`_to_trace_dict()`（**优先**，可用占位符替换大对象）与 `to_dict()`（通用回退）。而 `getattr(value, "...", None)` 的真值判断意味着**方法存在且非空**才用——一个返回假值的方法不会被调用。

`_serializable_value` 的 docstring 有一句关键警告：「**单向的**：它从不被反序列化，所以允许丢数据（例如用占位符替换大对象）。**不要把它换成 `base_serialization` 的 schema 序列化器，那个是为了精确往返而造的**」。

所以这一层有**两个序列化器，用途相反**：追踪用的可以有损（要小、要快、不必还原），配置用的必须无损（要能还原成同一个对象）。混用会导致要么追踪里塞了巨大数据，要么配置还原失败。

**`Telemetry` 的三条自我保护，每一条都针对一种「不能因为遥测而坏事」的场景。**

第一条在 `__init__` 里，把 posthog 与 backoff 的日志压掉：

```python
for module_name in ["posthog", "backoff"]:
    logging.getLogger(module_name).setLevel(logging.CRITICAL)
    logging.getLogger(module_name).addHandler(logging.NullHandler())
    logging.getLogger(module_name).propagate = False
```

注释说明了第二个动作的必要性：「防止模块在 `emit()` 调用期间遇到异常时把错误发到 stderr」。所以不只是「日志级别高」——而是**连 handler 都换成 NullHandler 并关掉传播**，因为 posthog 可能用别的机制直接写 stderr。

第二条在 `send_event` 里：

```python
try:
    posthog.capture(distinct_id=self.user_id, event=event_name, properties={**self.event_properties, **event_properties})
except Exception as e:
    logger.debug("Telemetry couldn't make a POST request to PostHog.", exc_info=e)
```

网络失败只记 `debug`。所以遥测发送失败对用户完全静默。

第三条在 `send_telemetry` 装饰器里，而且这条最彻底：

```python
@functools.wraps(func)
def send_telemetry_wrapper(*args, **kwargs) -> None:
    try:
        if telemetry:
            output = func(*args, **kwargs)
            if output:
                telemetry.send_event(*output)
    except Exception as e:
        # Never let telemetry break things
        logger.debug("There was an issue sending a telemetry event", exc_info=e)
```

注释就四个字：「**绝不让遥测破坏东西**」。而这个装饰器还有一个副作用值得提：**被装饰的函数只有在 `telemetry` 非空时才真被调用**。

```python
telemetry = None
if os.getenv("HAYSTACK_TELEMETRY_ENABLED", "true").lower() in ("true", "1"):
    telemetry = Telemetry()
```

所以关掉遥测时，`pipeline_running` 的函数体根本不执行——**连「收集组件信息」这个开销都省了**。这是把「开关检查」放在装饰器里而不是函数体里的收益：函数体可以写得很重（它要 `pipeline.walk()` 遍历全部组件），而关掉时零成本。

但这也带来一个**可观察的行为差异**：`pipeline_running` 里有两行副作用：

```python
pipeline._telemetry_runs += 1
if pipeline._last_telemetry_sent and (datetime.datetime.now() - pipeline._last_telemetry_sent).total_seconds() < MIN_SECONDS_BETWEEN_EVENTS:
    return None
pipeline._last_telemetry_sent = datetime.datetime.now()
```

`_telemetry_runs` 这个计数器只在遥测开启时才会增长。所以关闭遥测后，`pipeline._telemetry_runs` 永远是 0——若有别的代码读它，就会得到不一致的值。这是一个「把开关放在外层」的固有代价：内层的副作用也一起被跳过。

**`pipeline_running` 的节流是「每分钟最多一次」。**

```python
MIN_SECONDS_BETWEEN_EVENTS = 60
...
if pipeline._last_telemetry_sent and (datetime.datetime.now() - pipeline._last_telemetry_sent).total_seconds() < MIN_SECONDS_BETWEEN_EVENTS:
    return None
pipeline._last_telemetry_sent = datetime.datetime.now()
```

返回 `None` 就表示「这次不发」——而装饰器里 `if output:` 正是为此存在。所以「不发」的信号通过返回值传递，而不是通过异常或额外的布尔。

而 `runs` 字段带的是**累计运行次数**：

```python
return "Pipeline run (3.x)", {
    "pipeline_id": str(id(pipeline)),
    "pipeline_type": generate_qualified_class_name(type(pipeline)),
    "runs": pipeline._telemetry_runs,
    "components": components,
}
```

所以即使每分钟只上报一次，上报里也带着「到现在一共跑了多少次」。这避免了节流导致的信息损失——高频运行的用户不会被算成每分钟一次。

`pipeline_id` 用的是 `str(id(pipeline))` ——**进程内的对象地址**。所以它只在一次进程生命周期内有意义，不能跨重启关联。这是一个刻意的隐私选择：不做持久标识，就无法跨会话追踪单个用户。

而 `components` 是一张「限定类名 → 该类型的实例列表」的表：

```python
components: dict[str, list[dict[str, Any]]] = defaultdict(list)
for component_name, instance in pipeline.walk():
    component_qualified_class_name = generate_qualified_class_name(type(instance))
    if hasattr(instance, "_get_telemetry_data"):
        telemetry_data = instance._get_telemetry_data()
        if not isinstance(telemetry_data, dict):
            raise TypeError(f"Telemetry data for component {component_name} must be a dictionary but is {type(telemetry_data)}.")
        components[component_qualified_class_name].append({"name": component_name, **telemetry_data})
    else:
        components[component_qualified_class_name].append({"name": component_name})
```

所以「一个类型在管道里出现几次」这件事变得可见（列表长度），而组件可以自愿加细节（`_get_telemetry_data`）。

那句 `raise TypeError` 值得注意：**它是整个遥测路径里唯一抛异常的地方**，而且抛在 `pipeline_running` 里——但外层装饰器的 `except Exception` 会接住它。所以实际效果是「组件返回了非 dict 就不上报这次事件」，而不是让用户的管道崩掉。

这是一个双重保护的具体例子：局部看起来是个硬错误，全局看来是优雅降级。而表型上它是「不好的」代码（在遥测里抛异常），但正确性由外层保证。

**`Telemetry.__init__` 的 user_id 处理有两分支，且都吞异常。** 配置文件存在则读，不存在则生成并写：

```python
CONFIG_PATH = Path("~/.haystack/config.yaml").expanduser()
```

读失败只记 `debug` 并**保持 `user_id = ""`** ——所以一个读坏了的配置文件会导致所有事件带空 `distinct_id`。写失败同样只记 `debug`，所以每次启动都会重新生成一个 id（因为没有持久化）。这两个降级都不影响功能，但会让统计质量下降。

而不存在文件时打一条 `logger.info` 长文案，把「opt-out 的方法」与文档链接都写进去。这是「首次运行时告知」的做法——不做成一堆弹窗，而是在日志里放一段完整说明。

## 关键取舍

**源码事实：** 全局 `tracer` 是一个 `ProxyTracer`，`enable_tracing` / `disable_tracing` 只改它的 `actual_tracer` 字段，所以已经持有引用的代码立即生效。

**源码事实：** 默认真 tracer 是 `NullTracer`，它的 `current_span()` 返回 `NullSpan`（非 `None`），所以调用方不必判空。

**源码事实：** `is_tracing_enabled()` 用 `not isinstance(..., NullTracer)` 而不用布尔字段，保持单一真相源。

**源码事实：** `set_content_tag` 的默认实现检查全局代理上的 `is_content_tracing_enabled`，该值在**构造时（模块 import 时）读一次**环境变量。

**源码事实：** `LoggingTracer.current_span()` 永远返回 `None`（不存 span），所以「`current_span()` 可能为 `None`」在某个内置实现下是真的。

**源码事实：** `LoggingTracer.trace` 在 `finally` 里打日志（成功与失败都记），用一把 `threading.Lock` 把「一个 span 的多条日志记录」串成原子段，避免并发交错。

**源码事实：** `coerce_tag_value` 三层降级：原语直通 → JSON 序列化 → `str()`；`None` 变空字符串。

**源码事实：** `_serializable_value` 是**单向有损**的，优先用对象的 `_to_trace_dict()`，而它的 docstring 明确禁止换成能精确往返的 schema 序列化器。

**源码事实：** `send_telemetry` 装饰器只在 `telemetry` 非空时才调用被装饰函数——关掉遥测时连收集开销都省了。

**源码事实：** 关掉遥测会让 `pipeline_running` 里的 `_telemetry_runs` 计数器不再增长（副作用被一起跳过）。

**源码事实：** 节流通过返回 `None` 表达（装饰器里 `if output:` 判断）；上报里带累计 `runs` 避免节流造成信息损失。

**源码事实：** `pipeline_id` 用 `str(id(pipeline))`，只在进程内有意义。

**架构推论：** 「代理 + 空对象」这个组合解决的是**两个不同的**问题，缺一不可。代理解决「引用已经发出去、无法替换」；空对象解决「关闭时调用方仍要能无脑调用」。若只有代理没有空对象，关闭状态就需要一个返回 `None` 的实现，而那会逼所有调用方判空；若只有空对象没有代理，启用追踪就得替换全局引用。两者合起来，才能让「代码里无条件写追踪调用」与「运行时可选开关」同时成立。

**架构推论：** `LoggingTracer` 的 `current_span() → None` 与 `NullTracer` 的 `current_span() → NullSpan` 这两个不同的选择，暴露了「当前 span」这个概念在两种实现下的含义差异。`NullTracer` 把它当成「总有一个可以丢弃的上下文」（可空对象的空实现形态），`LoggingTracer` 把它当成「本实现不追踪嵌套关系」。所以依赖 `current_span()` 的代码在两种实现下行为不同——这就是为什么并发场景必须显式传 `parent_span`。

**架构推论：** 「一个 span 的多条日志必须原子输出」这条要求，源于「日志是一行一条、而一个逻辑单元需要多行」这个固有张力。任何「把一段结构化数据打成多条记录」的系统都会遇到它，而解法都是同一个：要么合成一条（但可读性降），要么加锁（但吞吐降）。这一层选了加锁，因为它作用于 `debug` 级别——一个本就不该在热路径开启的通道。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/tracing/tracer.py` | `Span` | 抽象 `set_tag`，其余方法有默认实现 |
| `haystack/tracing/tracer.py` | `Span.set_content_tag` | 内容标签的默认关闭开关 |
| `haystack/tracing/tracer.py` | `Tracer` | `trace` 与 `current_span` 两个抽象 |
| `haystack/tracing/tracer.py` | `ProxyTracer` | 持有真 tracer 的转发层 |
| `haystack/tracing/tracer.py` | `NullTracer` | 空实现，`current_span` 返回 `NullSpan` |
| `haystack/tracing/tracer.py` | `NullSpan` | 只实现 `set_tag`，其余靠默认退化 |
| `haystack/tracing/tracer.py` | `tracer` | 模块级代理单例 |
| `haystack/tracing/tracer.py` | `enable_tracing` | 只替换 `actual_tracer` |
| `haystack/tracing/tracer.py` | `disable_tracing` | 换回 `NullTracer` |
| `haystack/tracing/tracer.py` | `is_tracing_enabled` | 靠类型判断，不额外维护布尔 |
| `haystack/tracing/tracer.py` | `HAYSTACK_CONTENT_TRACING_ENABLED_ENV_VAR` | 内容追踪的环境变量名 |
| `haystack/tracing/logging_tracer.py` | `LoggingTracer` | 唯一内置实现，日志输出标签 |
| `haystack/tracing/logging_tracer.py` | `LoggingSpan` | 可变 dataclass，`set_tag` 直接存 |
| `haystack/tracing/logging_tracer.py` | `LoggingTracer.trace` | `finally` 里打日志，锁保护连续性 |
| `haystack/tracing/utils.py` | `coerce_tag_value` | 三层降级到可追踪类型 |
| `haystack/tracing/utils.py` | `_serializable_value` | 单向有损序列化，优先 `_to_trace_dict` |
| `haystack/telemetry/_telemetry.py` | `Telemetry` | posthog 封装，含 user_id 持久化 |
| `haystack/telemetry/_telemetry.py` | `Telemetry.send_event` | 合并系统属性，失败静默 |
| `haystack/telemetry/_telemetry.py` | `send_telemetry` | 装饰器，遥测关闭时不调用原函数 |
| `haystack/telemetry/_telemetry.py` | `pipeline_running` | 遍历组件收集数据，每分钟限一次 |
| `haystack/telemetry/_telemetry.py` | `MIN_SECONDS_BETWEEN_EVENTS` | 节流窗口 60 秒 |

## 参考

- [haystack 源码：tracing/tracer.py](https://github.com/deepset-ai/haystack/blob/main/haystack/tracing/tracer.py)
- [haystack 源码：telemetry/_telemetry.py](https://github.com/deepset-ai/haystack/blob/main/haystack/telemetry/_telemetry.py)
- [haystack 文档：追踪](https://docs.haystack.deepset.ai/docs/tracing)

## 与主核 / AI 的关系

主核里最接近的也是「零成本可关的插桩」：一个编译期开关控制的性能计数器或执行计划记录，关掉时只剩几个几乎为空的函数调用。`NullTracer` 与那些空宏的定位完全一致。

差别在于**开关的时机**。主核的插桩开关通常在编译期确定（或用静态分支预测把开销压到接近零）；这里的开关是运行时改一个字段，每次调用都多一层转发。换来的是「部署后可以动态打开追踪」——这在对线上问题做诊断时价值很高，因为你的生产构建与调试构建是同一个二进制。

与 i03（钩子）的对照很有意思：两者都是「用户可插入的扩展点」，但追踪这一层的**内容标签默认关闭**，而钩子那层的 State 完全不进追踪。所以两处对「敏感内容」的处理是一致的——**追踪里默认只记结构与元信息，不记内容**。

与 h02（工具调用执行）的连接：那边每个工具调用一个 span 并显式传 `parent_span`，正是为了绕开 `current_span()` 在并发下的不可靠。而 `LoggingTracer` 返回 `None` 更说明「不能依赖当前 span」这条约束在内置实现下就成立。

对 AI 工程最实用的一条是「内容追踪默认关闭，只记结构」。RAG 与 agent 的追踪里最有价值的信息往往是「哪个组件、耗时多少、调用了哪些工具」——这些都不含用户数据。而把查询文本、文档内容、模型回答写进追踪，等于把用户数据复制到一个可能长期保留、访问控制更松的地方。默认关闭 + 显式开启是唯一安全的方向。

另一条是「关闭开关时，把检查放在最外层」。`send_telemetry` 装饰器让被装饰函数根本不执行，于是「遍历全部组件」这个不廉价的操作在关闭时为零成本。若把 `if not enabled: return` 写在函数体第一行，也能省掉主逻辑，但省不掉「进函数、建参数、算默认值」那部分。差别不大，但把这个模式用在真正昂贵的入口上时差别明显。

最后一条是「一个逻辑单元的多条日志要原子输出」。并发环境下多行日志交错是排查时的真实痛点——你会看到「Operation: A」紧接着「B 的 tag」，而两者其实属于不同的 span。加一把锁的成本只有在高频路径上才明显，而对 `debug` 级日志来说完全可接受。
