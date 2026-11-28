---
project: open-interpreter
feature: otel-tracing
title: 追踪上下文与遥测目标
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/otel/src/trace_context.rs
    symbols: [inject_span_w3c_trace_headers, current_span_w3c_trace_context, TRACEPARENT_ENV_VAR]
  - path: codex-rs/otel/src/targets.rs
    symbols: [is_trace_safe_target, OTEL_TARGET_PREFIX]
  - path: codex-rs/otel/src/provider.rs
    symbols: [OtelProvider, resource_attributes, ResourceKind]
---

# 追踪上下文与遥测目标

## 一句话职责

让一条 trace 能跨进程边界继续：从 `TRACEPARENT` 环境变量或调用方传的 W3C 双头里接上父上下文，往出的 HTTP 请求里注入同一份头；同时用**日志的目标名字**决定哪些事件可以成为 span——因为有些事件天生不安全进 span。

## 机制是什么

`trace_context.rs` 的四个关键符号：

- `TRACEPARENT_ENV_VAR = "TRACEPARENT"` —— 环境变量的名字。注意它是 `const` 而不是 `pub`，但被 `load_traceparent_context` 用作唯一来源。
- `current_span_w3c_trace_context()` —— 取当前 span 的 W3C 双头。
- `inject_span_w3c_trace_headers(span, headers) -> bool` —— 把指定 span 的上下文写进一个 `http::HeaderMap`。
- `W3cTraceContext` —— 来自 `codex_protocol`，两字段 `traceparent` 与 `tracestate`，都是 `Option<String>`。

`targets.rs` 三个常量：

```rust
OTEL_TARGET_PREFIX = "codex_otel";
OTEL_LOG_ONLY_TARGET = "codex_otel.log_only";
OTEL_TRACE_SAFE_TARGET = "codex_otel.trace_safe";
```

两个判定函数：

```rust
fn is_log_export_target(target) -> bool { target.starts_with(OTEL_TARGET_PREFIX) && !is_trace_safe_target(target) }
fn is_trace_safe_target(target) -> bool { target.starts_with(OTEL_TRACE_SAFE_TARGET) }
```

`provider.rs` 里：

- `OtelProvider` 六字段：`logger`、`tracer_provider`、`tracer`、`metrics`、`shutdown_started: AtomicBool`、`shutdown_worker: Option<SyncSender<ShutdownWorker>>`。
- `ResourceKind` 两值 `Logs` / `Traces`。
- `resource_attributes(settings, host_name, kind)` 生成 resource 的属性列表。

## 怎么实现的

**`targets.rs` 的两个判定是互补的，且判据是「名字前缀」。** 这是这一层最有意思的设计。`OTEL_TARGET_PREFIX = "codex_otel"` 是所有「本 crate 产生的遥测事件」的目标名前缀；`OTEL_TRACE_SAFE_TARGET = "codex_otel.trace_safe"` 是其中「可以安全地成为 span」的子集。

于是两个函数的关系是：一个事件是「只导出日志」的（`codex_otel` 前缀但**不是** trace_safe），或者「可安全追踪」的（`codex_otel.trace_safe` 前缀）。

为什么会有「不安全追踪」的事件？这个问题的答案取决于 span 与日志的差别：一条日志是「在某时刻发生了什么事」的单条记录；一个 span 是「一段有开始与结束的、可能被采样、可能被持久化为树节点的实体」。把某些事件提升为 span 会带来额外后果——它们会占据 trace 的节点预算、会影响采样判定、会被下游按调用树解读。

所以「用目标名做分类」的做法，本质上是让**代码作者在声明日志时同时声明它的遥测语义**：写 `target: "codex_otel.trace_safe"` 就是「这一条可以成为 span」；写 `target: "codex_otel"` 就是「这一条只是日志」。

用一个字符串前缀来承载这个决定，而不是用类型或属性，看起来很粗糙。但它有一个实际优势：**日志宏的 `target` 参数已经是声明式的**，任何 `tracing::info!(target: "...", ...)` 都天然带了这个信息。所以不需要再引入一套新机制。

**`span_w3c_trace_context` 先验有效再注入。** 实现：

```rust
let context = span.context();
if !context.span().span_context().is_valid() { return None; }
let mut headers = HashMap::new();
TraceContextPropagator::new().inject_context(&context, &mut headers);
let tracestate = headers.remove("tracestate");
...
Some(W3cTraceContext { traceparent: headers.remove("traceparent"), tracestate: merge_tracestate_entries(tracestate.as_deref(), &guard) })
```

四步：取上下文、验有效性、用 propagator 注入到一个 map、把两个键取出来。

`is_valid()` 这道检查是必要的（与 goose 的 j04 同理）：propagator 对无效上下文会写入空字符串，而不是不写键。所以「注入成功」不等于「有有效上下文」。

而 `headers.remove("traceparent")` 的返回值是 `Option<String>`——所以如果一个有效上下文居然没产出 `traceparent`，这里是 `None` 而不是空串。但更常见的是产出空串；把空串留在 `W3cTraceContext` 里会让下游误判。这里没有显式过滤空串，但因为前面已验 `is_valid()`，一个有效上下文的 `traceparent` 不会为空。

`tracestate` 的处理比 `traceparent` 复杂得多：它要经过 `merge_tracestate_entries` 与**进程级配置**合并。

**`merge_tracestate_entries` 是「反向迭代维持顺序」。** 它先 `TraceState::from_str(tracestate)`，失败就 warn 并用 `Default`（空 tracestate）——所以一个非法的入站 tracestate 不会阻断 traceparent 的传播。

然后：

```rust
for (key, fields) in configured_entries.iter().rev() {
    let value = merge_tracestate_member_fields(trace_state.get(key), fields);
    trace_state = match trace_state.insert(key.clone(), value) { Ok(ts) => ts, Err(err) => { warn!(...); break; } };
}
```

注释解释了 `rev()`：「`TraceState::insert` 把成员放在**前面**。反向迭代在更新配置成员内部字段时保持确定的 map 顺序」。

所以 W3C tracestate 的语义是「最近的在最前」，而 `insert` 就实现了这个。若正向迭代，最后插入的会跑到最前——顺序就与 `BTreeMap` 的顺序相反。反向迭代让最终 header 里的成员顺序与 map 的顺序一致。

`Err(err)` 分支是 `break` 而不是 `continue`：tracestate 有成员数量上限（W3C 规定 32 个），插入失败通常意味着「满了」，继续插入也不会成功。所以 `break` 是唯一合理的动作。

最后 `let tracestate = trace_state.header(); (!tracestate.is_empty()).then_some(tracestate)` ——空了就返回 `None`，避免注入一个空的 `tracestate` 头。

**`inject_span_w3c_trace_headers` 的返回值是「有没有有效上下文」。** 实现：

```rust
let Some(trace) = span_w3c_trace_context(span) else { return false; };
match trace.traceparent { Some(tp) => { if let Ok(value) = HeaderValue::from_str(&tp) { headers.insert("traceparent", value); } }, None => { headers.remove("traceparent"); } }
```

注意 `None` 分支是 `remove` 而不是「什么都不做」。这一点很重要：doc 说「既有的 `traceparent` 与 `tracestate` 值会被替换，所以调用方可以安全地复用一个请求 header map，同时保持提供的 span 为真相来源」。

所以这个函数的契约是「让 header map 里的追踪头与给定 span 一致」——包括「删掉不该有的」。若只做 `insert` 不 `remove`，一个复用的 map 会残留上一次请求的头，导致「这次请求声明了上一次的父上下文」。

`HeaderValue::from_str` 失败时**不清除也不插入**——即保留原值。这是一个轻微的不一致：当 traceparent 字符串无法成为合法头值时（例如含非法字符），函数既没更新也没删除旧值。返回 `true`（因为有有效上下文），但 header 没变。这是一个值得记录的边界。

最后无条件 `return true` ——所以返回值只说「有没有上下文」，不说「头是否真的改了」。

**`TRACEPARENT` 环境变量是「外部注入的父上下文」。** `traceparent_context_from_env()` 用 `OnceLock<Option<Context>>` 缓存，`get_or_init(load_traceparent_context)`。

`load_traceparent_context` 读两个环境变量、调 `context_from_trace_headers`，成功打 debug（「检测到 TRACEPARENT；从父上下文继续 trace」），失败打 warn（「TRACEPARENT 已设置但无效；忽略追踪上下文」）。

两级日志的差别说明了这个场景的重要性：`TRACEPARENT` 存在意味着「上游有东西希望这条 trace 接上」，成功是正常路径（debug），失败是配置问题（warn）——需要用户看到。

用 `OnceLock` 而不是每次读 `env::var`，是因为环境变量在进程生命周期内不变（至少在启动后不该变），且这个值会被多处查询。

**`context_from_trace_headers` 的三行注释是关键。** 实现是：`traceparent` 缺失直接 `None`；把两个头塞进 HashMap；`extract_with_context(&Context::new(), &headers)`；`is_valid()` 不成立则 `None`。

那条注释：「无效的入站头**不得**回落到一个无关的活跃 span」。

这句话点出了一个容易犯的错误：`propagator.extract(&Context::current(), &headers)` 会以当前上下文为底——若 header 无效，结果可能就是「当前的活跃 span」。那会让「上游给了个坏 traceparent」表现为「这条 trace 悄悄接在了本地某个 span 上」，而调用方以为它接上了上游。

用 `&Context::new()` 作为底、再显式验 `is_valid()`，就把「无效」与「有效」彻底分开：无效就是 `None`，不猜测。

**`set_parent_from_w3c_trace_context` 是一个两行的组合器。** 它从 `W3cTraceContext` 造出 `Context`（失败返回 `false`），成功则 `set_parent_from_context(span, context)` 并返回 `true`。而 `set_parent_from_context` 的实现是 `let _ = span.set_parent(context)` ——丢弃错误。

`set_parent` 可能失败的原因包括「span 已被采样」之类。这里选择忽略，是因为追踪上下文的传播是尽力而为：接不上父上下文也应当继续跑，不该让业务逻辑失败。

**`OtelProvider` 的 shutdown 是一次性的。** `shutdown(&self)` 用 `shutdown_started.swap(true, AcqRel)` ——若已是 `true` 就立即返回。所以「最多关一次」。注释就是这句：「冲洗并关停在配置的 exporter，至多一次」。

三个 provider 各自 `let _ = xxx.shutdown()` ——错误被丢弃。理由同类：关停是清理阶段，报错无处可去。

`Drop for OtelProvider` 调 `self.shutdown()` ——所以「忘了显式关停」也会在析构时被兜住。

但这里还有一个更精巧的结构：`shutdown_worker` 与 `ShutdownWorker`。

`prepare_shutdown_worker` 起一个**专用 OS 线程**（名为 `codex-otel-shutdown`），通过两个 `sync_channel(1)` 做握手：先等 `ready_tx` 确认线程起来了，然后把它自己的接收端存起来。

注释说：「在关停时的资源压力之前启动这个分离的关停 worker」。所以它的目的是把「可能阻塞的 exporter 关停」放到一个已经就绪的线程上——而不是在进程退出时临时创建线程（那时可能已经资源紧张，甚至线程创建会失败）。

`prepare_shutdown_worker_with_spawner` 抽出一个 `spawn` 参数，让测试可以注入自己的线程创建逻辑。`if self.shutdown_worker.is_some() { return Ok(()) }` 让这个操作幂等。

两个 channel 都是 `sync_channel(1)`（容量 1），且 `ready_rx.recv()` 是**阻塞**的——所以 `prepare` 会等到线程真的开始执行。若 `ready_rx.recv()` 失败（通道关闭，线程没发 ready），错误消息是「telemetry shutdown worker stopped before initializing」并返回 `BrokenPipe`。

`shutdown_with_timeout(mut self, timeout)` 是那个带预算的关停：

1. 若没有准备好的 worker（`shutdown_worker.take()` 为 `None`），把 self 包成 `ManuallyDrop`（**不执行析构**）并返回 `NotConnected` 错误。注释解释了：「尽力而为的关停不应该在一个可能阻塞的析构函数上运行，当它的 worker 无法被准备时」。所以「没有 worker」= 「不关停也不析构」——宁可漏掉 flush，也不要让退出流程挂住。
2. 否则把 `self` 包成 `ManuallyDrop` 塞进 `ShutdownWorker`，连同 `completed_tx`（`tokio::sync::oneshot`）一起发过去。
3. 线程侧：`ManuallyDrop::into_inner(worker.provider)` 拿回 provider，`provider.shutdown()`，`drop(provider)`（这次是真的析构，会调 `Drop::drop` → `shutdown()`，而 `shutdown_started` 已为真所以不会再关一遍），然后 `completed_tx.send(())`。

`ManuallyDrop` 的三次使用（两次包、一次取）解决了「移动一个实现了 `Drop` 的类型，却又不想在移动时析构」这个问题。`OtelProvider` 有 `Drop` 实现，所以它不能被简单地从 `self` 里「搬」进线程——用 `ManuallyDrop` 才能既搬走又不触发析构。

注意线程侧那句 `provider.shutdown(); drop(provider);` ——显式关停之后再 drop，drop 里的 `shutdown()` 会因 `swap` 已真而空转。所以这两步在功能上冗余，但它们表达了两件事：先明确关停（可预期地 flush），再释放（清理字段）。

**resource 的属性按 `ResourceKind` 分叉。** `resource_attributes(settings, host_name, kind)` 总是给 `service.version` 与 `env` 两个属性，但 `host.name` 只在 `kind == Logs` 时加上。

为什么 traces 不带 host.name 而 logs 带？这是一个值得注意的信息设计：host 名在日志里用于「这条日志来自哪台机器」（多机收集日志时必需），而在 trace 里它更适合作为 span 属性或由后端从 resource 的其他字段推断——因为 trace 常常跨机器，把「接收端的 host 名」挂在整条 trace 的 resource 上会误导。

而 `make_resource` 两处调用（`ResourceKind::Logs` 与 `ResourceKind::Traces`）分别造两个 resource——所以同一个进程里日志与追踪的 resource 是不同的。

`normalize_host_name` 只做 trim 与判空：空白或空的 host 名被丢弃（而不是发出一个空字符串属性）。

`detected_host_name()` 用 `gethostname()` 取其 lossy 字符串——所以非 UTF-8 的 host 名会被替换字符污染，但仍会有值。

**`SpanAttributesProcessor` 把配置属性挂到每个 span 上。** 实现 `on_start` 里遍历属性并 `span.set_attribute(...)`。

doc 区分得很清楚：「resource 属性描述 provider 进程。这些属性是**按 span 的**元数据，所以需要在每个 span 导出之前挂上」。

所以两类属性的归属：`service.name`、`service.version`、`env`、`host.name` 是「进程级」的（resource）；而配置里那份 `span_attributes`（`BTreeMap<String, String>`）是「每个 span 都该有的」但 OTel 的 API 没有「默认 span 属性」这个概念——所以只能靠一个 `on_start` 处理器逐 span 挂上。

`tracer_provider_builder` 里 `if span_attributes.is_empty() { builder } else { builder.with_span_processor(SpanAttributesProcessor { ... }) }` ——所以空配置时不装处理器，省掉每个 span 的一次遍历。

## 关键取舍

**源码事实：** 遥测语义由日志的 `target` 前缀决定：`codex_otel.trace_safe` 可成为 span，其余 `codex_otel` 前缀只导出日志。

**源码事实：** `span_w3c_trace_context` 先验 `is_valid()` 才注入。

**源码事实：** `inject_span_w3c_trace_headers` 在「有上下文但该头为 None」时**删除**旧头，实现「header map 与 span 一致」。

**源码事实：** `context_from_trace_headers` 以 `Context::new()` 为底，注释明确「无效入站头不得回落到无关的活跃 span」。

**源码事实：** tracestate 与进程级配置合并时反向迭代，因为 `insert` 把成员放在最前。

**源码事实：** `TRACEPARENT` 用 `OnceLock<Option<Context>>` 缓存，成功 debug、失败 warn。

**源码事实：** `host.name` 只加在 logs 的 resource 上，不加在 traces。

**源码事实：** `shutdown` 用 `swap` 保证至多一次；`shutdown_with_timeout` 用 `ManuallyDrop` 把 provider 搬进专用线程。

**源码事实：** 没有准备好的 shutdown worker 时，`shutdown_with_timeout` 错误返回且**不析构** provider。

**架构推论：** 「用日志目标名承载遥测语义」是这一层最经济的设计。它把一个需要「声明」的决定（这条能否成为 span）复用到了已有的声明机制（`tracing` 的 `target`）。代价是这个语义只存在于字符串约定里——一个拼错 `target` 的事件会静默地落到「只导出日志」桶。所以这类约定需要固定的常量（`OTEL_TRACE_SAFE_TARGET`）而不是各处手写。

**架构推论：** 「无效输入不得回落到当前上下文」这条规则值得推广到所有传播/解析场景。它防的不是崩溃，而是**错误的成功**：一次本该失败的解析因为借用了本地状态而「成功」了，症状是「trace 看起来接上了但接错了」。判据是：解析的输入应当自我完备，不依赖调用者的环境。

**架构推论：** 「先起关停线程、后关停」这个顺序是被资源紧张逼出来的。进程退出时创建线程可能失败（内存碎片、线程数上限），而此时恰恰是最需要 flush 的时刻。提前在正常运行时启动一个空闲线程等待指令，就把「创建」与「使用」的时间点分开了。`ready_rx.recv()` 的阻塞握手保证「声称准备好了」是真的准备好了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/otel/src/trace_context.rs` | `inject_span_w3c_trace_headers` | 把 span 上下文写进 HTTP 头，删掉多余项 |
| `codex-rs/otel/src/trace_context.rs` | `current_span_w3c_trace_context` | 取当前 span 的 W3C 双头 |
| `codex-rs/otel/src/trace_context.rs` | `span_w3c_trace_context` | 验有效性后注入并合并 tracestate |
| `codex-rs/otel/src/trace_context.rs` | `TRACEPARENT_ENV_VAR` | 外部父上下文的环境变量名 |
| `codex-rs/otel/src/trace_context.rs` | `traceparent_context_from_env` | OnceLock 缓存的进程级父上下文 |
| `codex-rs/otel/src/trace_context.rs` | `context_from_trace_headers` | 以空上下文为底，无效即 None |
| `codex-rs/otel/src/trace_context.rs` | `merge_tracestate_entries` | 反向迭代维持成员顺序 |
| `codex-rs/otel/src/trace_context.rs` | `set_parent_from_context` | 设置父上下文并忽略失败 |
| `codex-rs/otel/src/targets.rs` | `OTEL_TARGET_PREFIX` | 本 crate 遥测事件的目标前缀 |
| `codex-rs/otel/src/targets.rs` | `is_trace_safe_target` | 是否可成为 span 的判定 |
| `codex-rs/otel/src/targets.rs` | `is_log_export_target` | 前缀命中但不是 trace_safe |
| `codex-rs/otel/src/provider.rs` | `OtelProvider` | 四个 provider 加一次性关停与关停线程 |
| `codex-rs/otel/src/provider.rs` | `OtelProvider.shutdown` | swap 保证至多一次 |
| `codex-rs/otel/src/provider.rs` | `prepare_shutdown_worker` | 提前起线程并阻塞等待就绪 |
| `codex-rs/otel/src/provider.rs` | `shutdown_with_timeout` | 用 `ManuallyDrop` 搬运，无 worker 时不析构 |
| `codex-rs/otel/src/provider.rs` | `resource_attributes` | host.name 只给 logs |
| `codex-rs/otel/src/provider.rs` | `ResourceKind` | Logs / Traces 两值的 resource 分叉 |
| `codex-rs/otel/src/provider.rs` | `SpanAttributesProcessor` | 逐 span 挂配置属性 |

## 参考

- [open-interpreter 源码：otel/src/trace_context.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/otel/src/trace_context.rs)
- [W3C Trace Context 规范](https://www.w3.org/TR/trace-context/)
- [OpenTelemetry 文档：Resource 与 Span 属性](https://opentelemetry.io/docs/concepts/resources/)

## 与主核 / AI 的关系

主核里最接近的是「追踪上下文的跨进程传播」：一个请求跨多个服务时，每一跳从入站头里取出 trace 上下文、作为自己的父 span、再把上下文写进下一跳的头里。这一页的 `inject_span_w3c_trace_headers` 与 `context_from_trace_headers` 就是那对「出站注入、入站提取」，连「无效入站头不能借用本地上下文」这条规矩都一样。

与 goose 的 j04（OTLP 三层）对照很有意思。goose 那边是「三个信号（trace/metric/log）各配一个 provider 与 exporter」，且用 `signal_exporter` 三级回退决定启用与否；这边是「logs 与 traces 共用部分逻辑但 resource 属性不同」，且关停机制复杂得多（专用线程加 `ManuallyDrop`）。差别来自代码位置：goose 的 otel 是**应用自己的初始化**，这里的 otel 是被多处复用的**库**——库要处理「调用方可能在任意时刻、任意线程关停」这种局面。

与 g06（上下文片段）的联系在于「结构化的元信息」这件事：那边给注入内容加可识别的标记，这边给日志加可分类的目标名。两者都是「用字符串约定承载语义」，都需要常量来避免手写漂移。

对 AI 工程最实用的一条是「无效输入不得回落到当前上下文」。这是一个很小的写法差别（`Context::new()` vs `Context::current()`），但决定了失败是「明确失败」还是「错误的成功」。任何解析外部标识的代码都应检查这一点：如果解析失败时的默认值恰好是「本地的某个东西」，那失败就被掩盖了。

另一条是「关停线程要提前起」。任何需要在进程退出时做 I/O 的组件（flush 日志、提交指标、写入缓冲），都会遇到「退出时资源紧张、创建新线程可能失败」的问题。提前起一个 idle 线程并阻塞握手确认它活着，把风险从「最坏时刻」挪到了「正常时刻」。

最后一条是 `host.name` 只进 logs 的 resource。这条设计揭示了一个常被忽略的细节：resource 属性是**描述产生遥测的那个进程**，而 trace 会跨进程聚合。把接收端/发送端的机器名挂在整个 trace 的 resource 上，会让下游误以为整条 trace 都在那台机器上。日志不会跨进程聚合，所以那里的 host 名是准确的。属性该挂在哪一层，取决于这一层的数据是否会被跨主体合并。
