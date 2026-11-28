---
project: goose
feature: acp-server
title: ACP 服务端与连接工厂
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/acp/server.rs
    symbols: [AcpProviderFactory, AcpConnection, AcpConnectionOptions]
  - path: crates/goose/src/acp/server.rs
    symbols: [is_acp_visible_session_type, AcpBuiltinSelection]
---

# ACP 服务端与连接工厂

## 一句话职责

让 goose 反过来充当 ACP 的**服务端**：把每条外部连接包装成一个 `AcpConnection`，通过一个同步工厂产出 provider 实例，并把客户端发来的 `_meta` 逐项解析成受校验的配置——会话可见范围、内建扩展选择、MCP 服务器超时与会话标签都在这一层设限。

## 机制是什么

`AcpConnectionOptions` 是「怎么造一条连接」的参数包：`provider_factory`、`builtin_selection`、`config_dir`、`services`、`additional_source_roots`、`session_cwd`、`live_voice`。

两个字段的注释值得单独看：

- `services` 的注释写「每个服务器一个，被每条连接共享」——所以它是进程级单例，不是连接级。
- `session_cwd` 的注释写「设了之后，新会话使用这个宿主控制的工作目录，而不是连接客户端发来的 `cwd`」。所以它是一个**覆盖**项，用来在托管场景下压过客户端的自述。

`AcpConnection` 是每条连接的状态集合，字段很多但按用途可分组：能力探测的 `OnceCell`（是否支持 elicitation、custom notification、recipe 参数请求、tool call 标签补全）、客户端连接句柄 `client_cx`、`closed_session_ids`、`provider_inventory`、`recipe_path_cache`，以及一个 `connection_id`（由模块级 `AtomicU64` 递增）。

`AcpProviderFactory` 是一个类型别名：`Arc<dyn Fn(String) -> BoxFuture<'static, Result<Arc<dyn Provider>>> + Send + Sync>`。所以「用 provider 名换一个 provider 实例」被表达成一个**同步返回 future** 的闭包——这是一个为适配下游 API 形态做的妥协，后面会讲。

## 怎么实现的

**会话类型白名单是一个常量数组。** `ACP_VISIBLE_SESSION_TYPES` 只有三个值：`User`、`Scheduled`、`Acp`；`is_acp_visible_session_type` 就是 `contains` 检查。

这三者的共同点是「都是面向最终用户或外部协议的用户会话」。被排除的是内部用途的会话类型（子智能体、后台任务之类），它们的历史不该出现在 `listSessions` 的返回值里——否则外部客户端会看到一个它无法理解、也无法恢复的会话列表。

**内建扩展的两段选择。** `AcpBuiltinSelection` 只有两个字段：`defaults` 与 `explicit`。`from_requested(builtins)` 的规则是二选一：

- 客户端**没给**任何名字 → `defaults = ["developer"]`，`explicit` 为空。
- 客户端**给了**名字 → `defaults` 清空，全部进 `explicit`。

这两段的消费方式不同，`selected_builtin_extensions` 里分得很清楚：`defaults` 里的每一项要先查配置，`configured_enabled_state(config, builtin) != Some(false)` 才启用——也就是「默认开，但用户可以在配置里关掉」；而 `explicit` 里的每一项**无条件**加入。

所以语义是：「你点名的我就给你，你没点名的按默认加但尊重配置里的关闭」。`initial_session_extensions` 里还有一句更硬的注释支撑这个设计：「客户端发来的选择就是整个会话：空列表不启动任何扩展，`[memory]` 启动 Memory 但不带默认内建」。

`AcpBuiltinSelection::default()` 是两者皆空——即「什么都不自动加」。所以「给默认值」这个行为只发生在 `from_requested` 这条路径上，任何别处构造出来的默认实例都是「空」。

**`_meta` 的每一类值都有专门的解析器与上限。** 三类：

1. `meta_string(meta, key)`——要求值是字符串，不是字符串就 `invalid_params`。
2. `meta_tags(meta)`——读 `_meta.tags`，要求是对象；超过 `MAX_SESSION_TAGS = 16` 条报错；键必须 1 到 `MAX_SESSION_TAG_KEY_BYTES = 128` 字节；值必须是字符串且不超过 `MAX_SESSION_TAG_VALUE_BYTES = 1024` 字节；最后**按键排序**后返回 `Vec<(String, String)>`。
3. `extract_timeout_from_meta`——读 `_meta.timeout` 并 `as_u64()`，读不到就 `None`（不报错）。

`meta_tags` 里那个 `sort()` 不是装饰：注释说「按 key 排序的客户端拥有的字符串标签」。排序让同一组标签在不同到达顺序下产生相同的结构，于是下游可以按位置比较或做稳定的 diff，而不必自己再排一遍。

三道上限（条数、键长、值长）都是在**边界**上挡「用元数据把内存或界面撑爆」这类问题。键必须有内容（`key.is_empty()` 也报错）——空键在展示与查询时都会变成麻烦。

`extract_timeout_from_meta` 的宽容与前面两个形成对比：超时读不到就用 `None`（表示用默认超时），读到一个非 u64 的值也静默变成 `None`。所以它是「可选提示」而非「契约字段」。

**超时被透传进 MCP 扩展配置。** `mcp_server_to_extension_config` 里，`McpServer::Stdio` 与 `McpServer::Http` 都调 `extract_timeout_from_meta`，把结果塞进 `ExtensionConfig` 的 `timeout` 字段。所以「客户端希望这个 MCP 服务器用多久超时」是通过 `_meta` 表达的，goose 只做转发。

同一个函数对 `McpServer::Sse(_)` 直接返回错误：`"SSE is unsupported, migrate to streamable_http"`。这是一个**明确的拒绝**而不是静默降级——旧传输被协议新版本淘汰后，服务端选择让调用方知道，而不是用一个可能行为不同的替代品顶上。

Http 分支还多一个动作：`client_http_headers(http.headers, &mut env_keys)` 把客户端给的头转成配置，同时把「需要从环境变量取值的键」收集进 `env_keys`。于是「头里可以放密钥引用」这个能力被保留下来了。

**工厂类型是为同步接口做的妥协。** `serve` 与 `LazyAcpConnection` 的注释解释了这件事：

```rust
// agent-client-protocol-http 的服务端需要一个同步工厂，
// 每条连接产出一个 ConnectTo<Client>；
// 但创建 AcpConnection 是异步的，
// 所以它发生在 ConnectTo::connect_to 里，也就是连接的服务 future 里。
```

所以 `AcpProviderFactory` 是「同步函数返回 boxed future」，而不是 `async fn`——因为要把它塞进一个要求同步工厂的位置。这与 g01 里 `ProviderDef` 用 `BoxFuture` 的理由同源：trait 对象或闭包位置不接受 `async fn`。

**连接的析构要按连接 id 反注册。** `Drop for AcpConnection` 里做的是：取当前 tokio 运行时句柄，`spawn` 一个异步任务调 `services.detach_connection(connection_id)`。所以「连接被丢弃」时，共享的 services 会收到通知并解除该连接的绑定。

用 `tokio::runtime::Handle::try_current()` 而不是直接 `spawn`，是因为 Drop 可能在非异步上下文里执行——那时没有运行时可用，就不反注册（`if let Ok(handle)`）。

**预热的 services 构造。** `acp_services_config` 里有一句 `tokio::spawn(async move { let _ = storage.pool().await; })`——注释说「急切初始化 SQLite 池，让 provider 与会话需要它时已经就绪」。

这是一次「启动期多花一点，避免首请求时阻塞」的取舍。注意它把错误 `let _ =` 掉了：池初始化失败不阻止服务起来，会在真正用的时候再失败。

**prompt 的 run 生命周期。** `on_prompt` 里有几处值得看：

- 注释写明「ACP 的 session_id 就是线程 id」，所以会话 id 直接被当作 goose 侧的会话标识。
- `run_id = format!("run_{}", Uuid::new_v4())`——每次 prompt 一个唯一运行 id。
- `ActiveRunDropGuard` 的注释说「若这个 future 在 prompt 中途被丢弃（例如承载它的漫游连接被撤销或丢失），它负责释放这次运行；正常完成的显式清理优先，会让 guard 的清理变成空操作」。所以它是「取消/cancel 也要解除占用」的保险。
- 它在真正开始前发一个 active run 更新，让客户端知道「现在这条会话在跑哪个 run」。
- `extract_from_meta(args.meta)` 从客户端的 `_meta` 里恢复 trace 上下文，取不到才用 `Context::current()`。注释强调「客户端的追踪上下文必须在整轮的 span 与工具调用被轮询期间保持附着」。

**能力探测的 OnceCell 而非字段。** `client_supports_acp_elicitation` 等四个都是 `OnceCell<bool>`，`client_cx` 是 `OnceCell<ConnectionTo<Client>>`。用 `OnceCell` 而不是 `Option` 的意义是「写了就不该再改」——这些值都来自 `initialize` 握手，握手只发生一次。

**服务的共享与反向依赖。** `Scheduler`（见 j02）持 `Weak<StateMachineServices>`，注释写「weak because the services hold the scheduler」。同理 `AcpConnection` 持 `Arc<StateMachineServices>` 且 `AcpConnectionOptions` 注释说明它「每个服务器一个」——所以真正的生命周期是：services 是进程级对象，连接与调度器都是它的消费者，但它们互相之间必须用弱引用避免环。

## 关键取舍

**源码事实：** `ACP_VISIBLE_SESSION_TYPES` 是编译期常量数组，只含 `User`、`Scheduled`、`Acp` 三类。新增会话类型默认不可见。

**源码事实：** `session_cwd` 一旦设置，就会覆盖客户端发来的 `cwd`。所以「客户端说自己要工作在哪」不是权威。

**源码事实：** `meta_tags` 有 16 条、键 128 字节、值 1024 字节三道上限，且按键排序返回。超限一律 `invalid_params`。

**源码事实：** `extract_timeout_from_meta` 读不到或类型不对都静默返回 `None`，不报错。

**源码事实：** `McpServer::Sse` 被显式拒绝并给出迁移提示，不做静默降级。

**源码事实：** `AcpProviderFactory` 是同步闭包返回 boxed future，因为下游的 HTTP 服务端要求同步工厂。

**源码事实：** `services` 是进程级单例，`AcpConnection` 与 `Scheduler` 对它的引用分别是 `Arc`（消费者）与 `Weak`（反向依赖解环）。

**架构推论：** 白名单而非黑名单，是这个边界最重要的一条。服务端对外暴露什么，应当由「已知安全的集合」定义而不是「已知不安全的集合」——因为新增内部会话类型时，黑名单会让它默认泄漏，白名单会让它默认隐藏。

**架构推论：** `meta_tags` 那三道上限揭示了一类常被忽略的攻击面：**元数据也是输入**。它不参与业务逻辑，却会进日志、进界面、进持久化，于是「往 `_meta` 里塞一万条超长字符串」能造成与「塞一个超大 prompt」相当的影响。在协议边界上给每个自由度配上限，是这类服务端的基本职责。

**架构推论：** `defaults` 受配置约束而 `explicit` 不受，是一条清晰的权力分配：默认行为属于用户（他可以在本地配置里长期关掉某个内建），单次请求属于调用方（他这次明确要什么）。若反过来——默认无条件加、点名要查配置——就会出现「客户端要什么取决于另一台机器上的配置」这种不可预期的行为。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/acp/server.rs` | `AcpProviderFactory` | provider 实例工厂，同步闭包返回 boxed future |
| `crates/goose/src/acp/server.rs` | `AcpConnection` | 单条连接的状态：能力探测、会话集合、缓存 |
| `crates/goose/src/acp/server.rs` | `AcpConnectionOptions` | 造连接的参数包，含宿主覆盖的工作目录 |
| `crates/goose/src/acp/server.rs` | `is_acp_visible_session_type` | 会话类型白名单判定 |
| `crates/goose/src/acp/server.rs` | `ACP_VISIBLE_SESSION_TYPES` | 白名单常量：User/Scheduled/Acp |
| `crates/goose/src/acp/server.rs` | `AcpBuiltinSelection` | 默认与点名两段内建扩展选择 |
| `crates/goose/src/acp/server.rs` | `AcpBuiltinSelection::from_requested` | 空则给默认、非空则只认点名 |
| `crates/goose/src/acp/server.rs` | `meta_tags` | 解析 `_meta.tags` 并施加三道上限 |
| `crates/goose/src/acp/server.rs` | `extract_timeout_from_meta` | 宽容读取 `_meta.timeout` |
| `crates/goose/src/acp/server.rs` | `mcp_server_to_extension_config` | MCP 服务器转扩展配置，SSE 显式拒绝 |
| `crates/goose/src/acp/server.rs` | `acp_services_config` | 预热 SQLite 池的进程级 services 构造 |
| `crates/goose/src/acp/server.rs` | `Drop for AcpConnection` | 按 connection id 反注册到 services |
| `crates/goose/src/acp/server.rs` | `AcpConnectionHandler::on_prompt` | 会话即线程、运行守卫、追踪上下文附着 |

## 参考

- [goose 源码：acp/server.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/acp/server.rs)
- [Agent Client Protocol 规范](https://agentclientprotocol.com/)
- [goose 文档：ACP 服务端模式](https://block.github.io/goose/docs/guides/acp-providers/)

## 与主核 / AI 的关系

主核里最接近的是「对外协议端点的输入校验层」：一条连接进来说明自己是谁、要什么，服务端必须把每个自由度都转换成有界的内部值——长度、枚举范围、超时、可见范围。这一页几乎全是这类转换，且每一条都配了上限。

与 smolagents 的 `MCPClient` 对比，两者正好站在协议的两端。smolagents 是**客户端**：连一个已有的 MCP 服务器，把远端工具拉进来，代码 170 行，因为校验责任在服务端。goose 这里是**服务端**：接收外部 ACP 客户端，要对它发来的一切做校验与限流，代码近 4000 行。同一份协议，客户端可以薄，服务端必须厚——因为不可信的一方在对面。

与 openai-agents 的 `MCPServer` 抽象对照也有意思：那边定义了「一个 MCP 服务器该长什么样」的契约（连接、列工具、调工具、清理），而 goose 这里是把 MCP 服务器**作为参数**接进来（`mcp_server_to_extension_config`），并额外支持一个 `_meta.timeout` 覆盖。同一个概念，一层是「我是服务器」，一层是「我要消费服务器」。

对 AI 工程最实用的一条是「元数据也是输入」。任何协议里带 `_meta`、`metadata`、`annotations` 这类自由字段的地方，都会被人用来传业务参数（超时、标签、优先级），而这些字段通常没有 schema 也就没有天然上限。把它们当成一等输入来校验——条数、键长、值类型——比事后在日志层做截断要可靠得多。`meta_tags` 那三道上限就是可直接抄的样板。

另一条是「白名单决定对外可见性」。做对外列表类接口（列会话、列资源、列历史）时，若用「排除某某类型」的写法，每次新增内部类型都会默认泄漏一次。改成「只包含这些类型」，新增类型默认隐藏，泄漏永远需要一次显式修改才能发生。这个方向上的默认值差异，正是这类 bug 反复出现的根源。
