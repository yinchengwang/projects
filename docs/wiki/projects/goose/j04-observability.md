---
project: goose
feature: observability
title: 追踪上下文与 OTLP 三层
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/otel/trace_context.rs
    symbols: [extract_from_meta, inject_current]
  - path: crates/goose/src/otel/otlp.rs
    symbols: [OtlpTracingLayer]
---

# 追踪上下文与 OTLP 三层

## 一句话职责

让一条链路在跨进程、跨协议时仍是同一个 trace：用 W3C 的 `traceparent` 塞进 ACP 与 MCP 的 `_meta` 里传出去，再从对端回传的 `_meta` 里恢复出来；同时把 trace、metric、log 三类信号各自装配成独立的导出层。

## 机制是什么

两部分，规模差得很远。

**传播层** `trace_context.rs` 只有两个函数加两个常量（`TRACEPARENT`、`TRACESTATE`）：

- `extract_from_meta(meta)` 从 `_meta` 里读 `traceparent`/`tracestate`，反序列化成一个 OpenTelemetry `Context`；无效或缺失返回 `None`。
- `inject_current(meta)` 把当前的 `Context` 序列化进 `meta`。

**导出层** `otlp.rs` 是一个完整的装配器：三个 provider（tracer / meter / logger）、三个包装过的 exporter、三个过滤器，加一个专用的 tokio 运行时。

`OtlpTracingLayer` 是个类型别名：`OpenTelemetryLayer<tracing_subscriber::Registry, opentelemetry_sdk::trace::Tracer>`，即「把 `tracing` crate 的事件桥接进 OTel tracer」的那一层。同族还有 `OtlpMetricsLayer` 与 `OtlpLogsLayer`。

## 怎么实现的

**只读小写的 SEP-414 键。** `extract_from_meta` 的实现是先组一个 `HashMap<String, String>` 载体，然后交给 `TraceContextPropagator::new().extract_with_context(&Context::new(), &carrier)`。

载体的构造用了一行 `filter_map`：只从 `meta` 里取 `TRACEPARENT` 与 `TRACESTATE` 这两个**小写**常量键，值必须是 `as_str()` 能成功的字符串。

注释点明了为什么不接受大小写变体：「只读小写 SEP-414 键，这样混合大小写的重复项不会竞态」。如果同时接受 `traceparent` 与 `TraceParent`，那么两个都出现在 `_meta` 里时，谁生效取决于遍历顺序——攻击者（或一个写错的对端）可以用一个无害的和一个恶意的来诱导不确定性。只认一种大小写，这个问题就不存在。

测试里专门有一条 `Some(json!({ "TraceParent": PARENT })); "non lowercase key"` 断言它被忽略。同组测试还覆盖了：没有 `meta`、没有 `traceparent`、格式错误、全零 trace id、值不是字符串——全部返回 `None`。

`is_valid()` 这道检查是必须的：`extract_with_context` 即使读到垃圾也会返回一个 `Context`（只是里面的 span context 无效）。所以「解析成功」不等于「有效」，必须显式查 `span_context().is_valid()`，然后 `.then_some(cx)` 把无效的折成 `None`。

**注入时过滤空值。** `inject_current` 先把 `Context::current()` 注入一个空的 `HashMap`，然后 `meta.extend(...)` 逐项写进 JSON，且 `.filter(|(_, value)| !value.is_empty())`。

这个过滤的必要性来自 propagator 的行为：`TraceContextPropagator` 在**没有有效上下文**时会给 `traceparent` 写空字符串（而不是不写这个键）。若不接受空值，`meta` 里就会出现 `{"traceparent": ""}`——一个看起来像「有追踪上下文」的垃圾字段。测试 `injects_nothing_without_current_context` 断言的就是「没有当前上下文时 `meta` 保持为空」。

**两处传播语义的差异。** 三个测试揭示了 `inject_current` 会输出什么：

- `relays_attached_remote_context_unchanged`：若当前上下文是一个**远程**上下文（通过 `.attach()`），注入出的 `traceparent` 与原始字符串**逐字相同**。
- `injects_entered_tracing_span_as_parent`：若中间进了一个 `tracing` span，注入出的 `traceparent` 的 span id 段换成了**那个 span 的 span id**，trace id 段不变。

所以「注入」测的是「当前活跃的那一层」：`tracing_opentelemetry` 层会把 `tracing` span 映射到 OTel span，于是 `Context::current()` 里最外层就是那个 span。这就是跨进程传播能在链路里插入新节点的原理——每一跳都把自己的 span 作为下一跳的父。

**装配器的三条独立路径。** `init_otlp_layers` 依次尝试三个创建函数，每个失败都只是略过（`if let Ok(layer)`），然后把成功的那些装进一个 `Vec<Box<dyn Layer>>` 并以各自过滤器包装。

关键细节：只有**至少有一层成功**时，才 `global::set_text_map_propagator(TraceContextPropagator::new())`。所以「传播器」不是无条件设置的——若什么都没导出，就不必（也不该）声称自己能传播追踪上下文。

**信号启用逻辑的三级回退。** `signal_exporter(signal)` 按顺序判：

1. `OTEL_SDK_DISABLED=true` → 全线禁用，返回 `None`。
2. `OTEL_{SIGNAL}_EXPORTER` 存在 → 按值解析（空串与 `"otlp"` 都算 `Otlp`，`"console"`/`"stdout"` 算 `Console`，其余算 `None` 并返回 `None`）。
3. 否则看端点：`OTEL_EXPORTER_OTLP_{SIGNAL}_ENDPOINT` 或 `OTEL_EXPORTER_OTLP_ENDPOINT` 非空 → `Otlp`；否则 `None`。

`ExporterType::from_env_value` 那三值映射是宽容的：`""` 与 `"otlp"` 等价。这符合 OTel 规范里「未设置时默认 otlp」的行为。

**协议检查是个实际的规避。** `signal_protocol_is_http(signal)` 读信号级 `OTEL_EXPORTER_OTLP_{SIGNAL}_PROTOCOL`，回退到共享的 `OTEL_EXPORTER_OTLP_PROTOCOL`，再按值判：空、`http/protobuf`、`http/json` 都算 HTTP；其余算不 HTTP。

注释解释了为什么需要这个检查：goose 的 `opentelemetry-otlp` 只启用了 `http-proto` 与 `reqwest-blocking-client` 传输特性，**没有** `grpc-tonic`。若环境里设了 gRPC 变体，`.with_http()` exporter 仍能构建成功，但它的后台批处理线程会在首次导出时 panic，报 `internal error: entered unreachable code: HTTP client should not receive Grpc protocol`。

所以这里选择**跳过该信号**而不是让它崩在后台线程里。三个 `create_otlp_*_layer` 都在 OTLP 分支开头调这个检查，不通过就调 `warn_grpc_protocol_skipped_once()` 并返回错误。

`warn_grpc_protocol_skipped_once` 用 `std::sync::Once` 加 `eprintln!` 而不是 `tracing::warn!`。原因在注释里：「用 `tracing::warn!` 会与正在初始化的 OTel subscriber 竞态；`eprintln` 保证它在任何 subscriber 状态下都可见」。这是一个很实际的细节——这条警告本身就是关于「导出层起不来」的，不能指望导出层来报告它。

**为 OTLP 单独准备一个 tokio 运行时。** `OTEL_RT` 是 `Mutex<Option<Arc<Runtime>>>`，`get_or_create_otel_rt` 用 `new_current_thread().enable_all()` 建一个并缓存。

理由在注释里讲得很清楚：`BatchSpanProcessor`、`BatchLogProcessor` 与 `PeriodicReader` 都创建**裸 std 线程**，没有 tokio reactor；而异步 reqwest HTTP 客户端在导出时会调 `tokio::time::sleep`，没有 reactor 就 panic。所以每个导出调用都被包进一个 `Tokio*Exporter`，其 `export` 方法判断「当前是否已在 tokio 上下文里」：在就直接 await，不在就 `self.rt.block_on(...)`。

这是一个「把没有运行时的同步调用点，包成有运行时的异步调用点」的适配器。三个 exporter（span / log / metric）结构相同，只是 trait 不同。

**三层各配一个过滤器。** 三者的策略不同，可以看出不同的意图：

- **tracing 过滤**：`INFO` 及以上全收；`DEBUG` 只在 target 以 `goose::`、`opentelemetry`、`tracing_opentelemetry` 开头时收；其余（含 `WARN` 以下的一切）丢弃。所以 DEBUG 是「白名单化」的。
- **metrics 过滤**：`INFO` 及以上全收；`DEBUG` 只在 target 以 `goose::telemetry`、`goose::metrics` 开头或**含 `"metric"`** 时收。
- **logs 过滤**：先按 `RUST_LOG`（有效就用，无效则回退 `OTEL_LOG_LEVEL`，再回退 `INFO`）构一个 `EnvFilter`，再 `.and(...)` 一个「排除抑制 target」的 `FilterFn`。

logs 这条与前两条性质不同：它把「级别」交给 `EnvFilter` 处理，所以 `RUST_LOG` 的指令（含 target 级指令）全部生效。注释写「有效的 `RUST_LOG` 指令优先于 `OTEL_LOG_LEVEL` 与默认 `INFO`；无效的 `RUST_LOG` 值回退」。

**抑制名单的具体动机。** `OTLP_SUPPRESSED_TARGETS` 只有一项 `"rmcp::service"`，但它的注释非常具体：`rmcp::service` 把完整的 `InitializeResult`（含扩展指令与用户记忆内容）作为 `peer_info` 属性记在每次 MCP 握手上，可能 400KB 以上且含 PII。「这些事件在 OTLP 里没有分析价值——完全抑制」。

`is_otlp_suppressed_target` 的实现用了一个小技巧：`target == suppressed || target.strip_prefix(suppressed).is_some_and(|suffix| suffix.starts_with("::"))`。所以它匹配 `rmcp::service` 与 `rmcp::service::xxx`，但**不匹配** `rmcp::serviceX`。用 `strip_prefix` 加边界检查而不是 `starts_with`，避免了「前缀相同但不同模块」的误伤。

**三层的 provider 都存进静态，为的是 shutdown。** `TRACER_PROVIDER` / `METER_PROVIDER` / `LOGGER_PROVIDER` 三个 `Mutex<Option<...>>` 持有 provider；`is_otlp_initialized()` 检查任意一个非空。

`shutdown_otlp` 从配置读 `otel_shutdown_timeout_ms`（默认 5000），然后逐个 `take()` 并 `shutdown_with_timeout(timeout)`。用 `take()` 而不是读——所以 shutdown 之后静态变量变空，`is_otlp_initialized` 返回假。

这个「存进静态」的做法与服务退出流程直接相关：OTLP 的批处理器需要显式刷出缓冲，否则最后一批 span 会随进程消失。

**resource 里的身份信息。** `create_resource` 固定写五个属性：`service.name = "goose"`、`service.version`（来自 `CARGO_PKG_VERSION`）、`service.namespace = "goose"`、`host.name` 与 `user.name`（来自 `session_context`），再挂两个探测器：`EnvResourceDetector`（读 `OTEL_RESOURCE_ATTRIBUTES`）与 `TelemetryResourceDetector`。

注释还解释了一个刻意的省略：`OTEL_SERVICE_NAME` 有最高优先级，但**不挂** `SdkProvidedResourceDetector`——因为它在未设置时会填 `"unknown_service"`，那会把没有名字的服务都标成同一个。

**logs 桥接的 span 属性白名单。** `create_otlp_logs_layer` 里的 bridge 带了 `.with_tracing_span_attributes(TracingSpanAttributes::allowlist([...]))`，白名单四项：`session.id`、`session.user`、`session.host`、`session.agent_type`。

所以「日志记录要不要带上当前的 span 属性」不是全带，而是只带这四个。这防止了「任意 span 属性（可能含请求体、文件内容）随每条日志一起外传」。

**temporality 的手写解析。** `temporality_preference()` 读 `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`，`delta` → `Delta`、`lowmemory` → `LowMemory`、其余 → `Cumulative`。上面有一行 `TODO` 注明「等上游 PR 发布后移除」。

**配置到环境变量的提升。** `promote_config_to_env` 把配置项 `otel_exporter_otlp_endpoint` 与 `otel_exporter_otlp_timeout` 写成环境变量，但**只在对应环境变量尚未设置时**（`if env::var(...).is_err()`）。

所以优先级是「环境变量 > 配置文件」。这个方向符合 OTel 生态的惯例：OTel 的所有配置都从环境读，若允许配置文件覆盖环境，会让「用环境变量临时改行为」失效。

## 关键取舍

**源码事实：** `extract_from_meta` 只读小写键，注释明确说是为了「混合大小写的重复项不会竞态」。测试覆盖了大小写变体被忽略。

**源码事实：** `inject_current` 过滤空字符串，因为 propagator 在没有有效上下文时会写空值；不滤就会出现 `{"traceparent": ""}` 这种伪字段。

**源码事实：** 只有至少一层成功创建时才设置全局 text map propagator。

**源码事实：** 遇到 gRPC 协议配置时**跳过该信号**而不是让它崩在后台线程；警告走 `eprintln` 加 `Once`，因为 `tracing::warn` 会与正在初始化的 subscriber 竞态。

**源码事实：** 为 OTLP 单独建一个 current-thread tokio 运行时，因为批处理器的裸线程没有 reactor，而 reqwest 导出要 `tokio::time::sleep`。

**源码事实：** logs 层的 bridge 只允许四个 span 属性外传。

**架构推论：** 「只读一种大小写」是一个值得推广的边界原则。凡是「同一个键可能有多种拼写」的协议字段，若接受多种拼写，就必须定义冲突消解规则；而多数实现没有定义，冲突时就变成「谁先被遍历到」。把接受集合收成一个，比定义消解规则便宜得多。

**架构推论：** 那个「包装 exporter 塞进独立运行时」的适配器，是所有「在无 reactor 的裸线程里调异步 HTTP」场景的通用解。`BatchSpanProcessor` 的线程模型是上游库的设计决定，无法改；能做的只有在自己这一侧提供一个运行时。这个模式（外层用时序库的线程、内层用自己的 runtime）在日志、指标、任何异步导出的地方都适用。

**架构推论：** 「跳过而非崩溃」这个选择针对的是一个具体的失败形态：`.with_http()` 能成功构建、但后台线程在首次导出时 panic。这比「构建时就失败」危险得多——因为构建成功会让调用方以为一切就绪，panic 发生在数百毫秒后的另一个线程里。识别这类「构建期成功、运行期异步崩」的依赖，需要在装配层主动做协议校验。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/otel/trace_context.rs` | `extract_from_meta` | 从 `_meta` 恢复远程 trace 上下文，无效折成 None |
| `crates/goose/src/otel/trace_context.rs` | `inject_current` | 把当前上下文写进 `_meta`，滤掉空值 |
| `crates/goose/src/otel/trace_context.rs` | `TRACEPARENT` | 小写 SEP-414 键，只认这一种拼写 |
| `crates/goose/src/otel/otlp.rs` | `OtlpTracingLayer` | tracing→OTel 桥接层的类型别名 |
| `crates/goose/src/otel/otlp.rs` | `init_otlp_layers` | 三层装配与传播器的条件设置 |
| `crates/goose/src/otel/otlp.rs` | `signal_exporter` | 三级回退的信号启用判定 |
| `crates/goose/src/otel/otlp.rs` | `signal_protocol_is_http` | 拒绝未编译的 gRPC 传输 |
| `crates/goose/src/otel/otlp.rs` | `warn_grpc_protocol_skipped_once` | 用 eprintln 加 Once 避免 subscriber 竞态 |
| `crates/goose/src/otel/otlp.rs` | `TokioSpanExporter` | 在无 reactor 线程里驱动异步导出 |
| `crates/goose/src/otel/otlp.rs` | `get_or_create_otel_rt` | OTLP 专用 current-thread 运行时 |
| `crates/goose/src/otel/otlp.rs` | `create_resource` | 服务身份属性与两个探测器 |
| `crates/goose/src/otel/otlp.rs` | `is_otlp_suppressed_target` | 前缀加 `::` 边界的抑制判定 |
| `crates/goose/src/otel/otlp.rs` | `shutdown_otlp` | 按配置超时刷出三个 provider |
| `crates/goose/src/otel/otlp.rs` | `promote_config_to_env` | 配置提升为环境变量，环境优先 |

## 参考

- [goose 源码：otel/trace_context.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/otel/trace_context.rs)
- [goose 源码：otel/otlp.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/otel/otlp.rs)
- [W3C Trace Context 规范](https://www.w3.org/TR/trace-context/)

## 与主核 / AI 的关系

主核里最接近的是「分布式追踪的上下文传播」：一个请求跨多个节点时，各节点要给自己的处理段打上同一个 trace id 与不同 span id，并把上下文传给下一个节点。这一页的 `extract_from_meta` / `inject_current` 就是那个「接上下文、传上下文」的一对，而 goose 的传播介质是协议里的 `_meta` 字段，主核里是 RPC 头。

与 openai-agents 的追踪层（i01/i02/i03）对比很有意思：那边的三层是「span 抽象 → processor → scope」，处理器收事件后自行决定怎么导出，`BatchTraceProcessor` 用**一个** daemon 线程导出一批对象。goose 这里的三层是**信号类型**（trace/metric/log），各自有独立的 provider 与 exporter，且每个 exporter 还要外包一层运行时。差别的根源在于「导出目标」：openai-agents 是一个自家后端端点，goose 是标准 OTLP（可以有任意 collector），所以必须处理协议细节（gRPC/http、temporality、resource detection、抑制名单）。

与 smolagents 的 `Monitor` 相比，那是「进程内自省」，连导出都没有。两者的差距正好标出「可观测性」的三档：进程内统计 → 导出到自家后端 → 接入标准生态。每一次跨档都引入一批新的兼容负担（协议、协议特性编译开关、上游库线程模型）。

对 AI 工程最实用的一条是「先判协议特性再装配」。`signal_protocol_is_http` 那段注释值得完整读一遍：一个依赖在缺少某个编译特性时不会在构建期报错，而是构建出「能创建但会在后台线程 panic」的对象。这是最难排查的失败形态之一。任何装配外部导出器的地方，都该像这样显式校验「我的构建真的支持你要的传输」，并在不支持时选择跳过加一次可见的警告。

另一条是 `inject_current` 里的空值过滤。传播器在没有有效上下文时会写空字符串而不是省略键——这个行为在多数实现里都存在，而后果是下游看到 `{"traceparent": ""}` 时会认为「有上下文但无效」，与「明确没有上下文」混在一起。写传播代码时，注入后一定要清理空值，否则「无追踪」这个状态就无法准确表达。
