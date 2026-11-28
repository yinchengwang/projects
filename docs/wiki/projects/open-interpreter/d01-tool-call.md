---
project: open-interpreter
feature: tool-call
title: 工具调用与环境抽象
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/tools/src/tool_call.rs
    symbols: [ToolCall, ToolCallSource, ToolEnvironment]
  - path: codex-rs/tools/src/tool_call.rs
    symbols: [TurnItemEmitter, ExtensionTurnItem, HistoryLoader]
---

# 工具调用与环境抽象

## 一句话职责

`ToolCall` 是「一次工具调用」在宿主侧的完整上下文包：它同时携带调用身份、来源、预算、历史快照、环境集合与负载形状——让同一个工具实现既能服务模型的直接调用，也能服务代码模式里的嵌套调用。

## 机制是什么

这个类型的形状由三个约束逼出来，逐个看：

| 约束 | 后果 |
|---|---|
| 一次调用可能来自两条路径（模型直接调用 / 代码模式内嵌套） | 需要 `ToolCallSource` 来区分，且它**影响预算计算** |
| 工具实现可能需要读会话历史，但读历史要花内存 | 需要 `ConversationHistory` 做延迟物化 |
| 同一个逻辑工具在不同环境上执行（本地 / 远端 / exec-server） | 需要 `ToolEnvironment` 集合，而不是一个 `cwd` |

它的字段清单本身就说明了「一次工具调用要考虑多少件事」：

```rust
pub struct ToolCall<'call> {
    pub turn_id: String,
    pub call_id: String,
    pub tool_name: ToolName,
    pub model: String,
    pub codex_turn_metadata: Option<String>,
    pub truncation_policy: TruncationPolicy,
    pub source: ToolCallSource,
    pub conversation_history: ConversationHistory,
    pub turn_item_emitter: Arc<dyn TurnItemEmitter>,
    pub environments: Vec<ToolEnvironment<'call>>,
    pub payload: ToolPayload,
}
```

十三个字段分三组：**身份**（turn/call/tool/model）、**能力**（历史、发射器、环境）、**策略**（截断、来源、负载）。

## 怎么实现的

**`ConversationHistory` 用 `LazyLock` 做延迟物化，而这条设计的动机写得极清楚。**

```rust
pub struct ConversationHistory {
    items: Arc<LazyLock<Box<[ResponseItem]>, HistoryLoader>>,
}
type HistoryLoader = Box<dyn FnOnce() -> Box<[ResponseItem]> + Send + Sync>;
```

两个字段但实际上是一个「只算一次」的槽位。构造有两种：

```rust
pub fn new(items: Vec<ResponseItem>) -> Self { Self::new_deferred(move || items) }

pub fn new_deferred(load: impl FnOnce() -> Vec<ResponseItem> + Send + Sync + 'static) -> Self {
    Self { items: Arc::new(LazyLock::new(Box::new(move || load().into_boxed_slice()))) }
}
```

`new` 是「已经有历史」的简单情形（它内部也走 `new_deferred`，只是闭包直接返回现成的 vec）。`new_deferred` 才是关键：

> Materializes an invocation-time snapshot only when a tool reads its history. The loader must capture that snapshot rather than querying mutable session state.

三条信息：

1. **只在工具真的读历史时才物化**。绝大多数工具调用不读历史（读文件、跑命令都不需要），而历史可能很大——所以不能在每次调用时都克隆一份。
2. **快照必须在闭包里「捕获」，不能去查活的会话状态**。这是避免「调用开始时与读取时读到不同历史」的竞态。注释写得很明确：loader 的职责是**给出那个时刻的切片**，而不是提供一个「读当前状态」的入口。
3. **`FnOnce`** 而不是 `Fn`：`LazyLock` 只调一次，所以 loader 只需可调用一次。这让实现方可以 `move` 所有权进闭包，不必克隆。

`Arc` 的存在让 `ConversationHistory` 可 `Clone`（`ToolCall` 需要 `Clone`），而第一个 clone 之后的读取都命中已物化的缓存。

`Debug` 实现是「非穷尽」的：

```rust
formatter.debug_struct("ConversationHistory").finish_non_exhaustive()
```

即打印 `ConversationHistory { .. }`，不打印内容。理由与 d03 讲的 `__repr__` 排除大字段同理——历史可能有几千条消息，若 `Debug` 打全量，任何一次 `{:?}` 都会产生巨量输出。

**`ToolCallSource` 只有两态，但它决定了预算公式。**

```rust
pub enum ToolCallSource {
    Direct,
    CodeMode { cell_id: String, runtime_tool_call_id: String },
}
```

`response_byte_budget` 的分支：

```rust
match &self.source {
    ToolCallSource::Direct => max_response_bytes.min(with_serialization_allowance(self.truncation_policy).byte_budget()),
    ToolCallSource::CodeMode { .. } => max_response_bytes,
}
```

读法：

- **直接调用**取「工具自身的上限」与「宿主文本输出预算」的**较小值**。因为直接调用要把结果作为文本塞进模型的上下文，所以受宿主的截断策略约束。
- **代码模式调用**只用工具自身的上限。方法的文档说明了理由：「Code Mode receives typed results without that truncation」。

即代码模式里的工具结果**不经过文本截断**——它由运行时代码消费（可能是 `json` 解析、字段提取），截断会破坏结构。这让两个来源的预算语义必须分开。

`with_serialization_allowance` 这个调用名本身就说明了另一处细节：截断策略给的是**内容**预算，而实际序列化（加 JSON 引号转义、加包装对象）会额外占字节，所以要先算出「含序列化开销」的预算再比较。方法文档也点了这一点：「Callers must include serialization overhead when fitting a response to this budget」。

**`payload` 是形状判别位，且访问它有专门的失败。**

```rust
pub fn function_arguments(&self) -> Result<&str, FunctionCallError> {
    match &self.payload {
        ToolPayload::Function { arguments } => Ok(arguments),
        _ => Err(FunctionCallError::Fatal(format!("tool {} invoked with incompatible payload", self.tool_name))),
    }
}
```

注意两点：

- **失败是 `Fatal` 而不是「给模型看的错误」**。`FunctionCallError::Fatal` 意味着「这是宿主侧的编程错误」，不是「模型给错了参数」。用 `Fatal` 是因为「一个只接受函数负载的工具收到了别的负载」不该由模型负责——这是宿主调度错了。
- **错误消息带工具名**。这让 Fatal 错误能被定位到具体工具，而不是一条泛泛的「负载不兼容」。

`ToolCall` 自己没有 `pub fn arguments()` 这种「随便取」的入口，只提供这个带类型判定的访问器。这是「需要什么形状就显式声明」的做法。

**`ToolEnvironment` 把「一个环境」的四个成分绑在一起。**

```rust
pub struct ToolEnvironment<'call> {
    pub environment_id: String,
    pub cwd: AbsolutePathBuf,
    pub file_system: Arc<dyn ExecutorFileSystem>,
    pub file_system_sandbox_context: FileSystemSandboxContext,
    pub _lifetime: PhantomData<&'call ()>,
}
```

四处：

- **`environment_id`** 是「稳定 id，用来路由执行器作用域的能力」。即同一套工具在不同环境（本地进程、远端 exec-server）上的实例靠它区分。
- **`cwd`** 是 `AbsolutePathBuf` 而不是 `PathBuf`——类型层面强制绝对路径。工具实现不必自己判「我拿到的是相对还是绝对」。
- **`file_system`** 是 `Arc<dyn ExecutorFileSystem>`，即文件操作要通过这个抽象，而不能直接 `std::fs`。这是让「远端环境」可行的前提：同一个工具代码在远端上跑时，`file_system` 指向远端的实现。
- **`file_system_sandbox_context`** 与 c01/c03 讲的那套权限体系相接——工具做文件操作时不能只看 `cwd`，还要看这份沙箱上下文。
- **`PhantomData<&'call ()>`** 带一条 TODO 注释：「Replace the marker with callback-scoped environment access」。即这个生命周期标记是临时的，理想形态是让环境访问被回调作用域约束，而不是靠一个标记参数。

`environments` 是 `Vec` 而不是单个，因为一次调用可能涉及多个环境（例如「把 A 环境的文件写到 B 环境」这类跨环境操作）。

**`TurnItemEmitter` 是一个能力注入点，默认实现是空操作。**

```rust
pub trait TurnItemEmitter: Send + Sync {
    fn emit_started<'a>(&'a self, item: ExtensionTurnItem) -> TurnItemEmissionFuture<'a>;
    fn emit_completed<'a>(&'a self, item: ExtensionTurnItem) -> TurnItemEmissionFuture<'a>;
}

pub struct NoopTurnItemEmitter;
```

三处设计：

- **返回 future 而不是 `async fn`**。`TurnItemEmissionFuture<'a> = Pin<Box<dyn Future<Output = ()> + Send + 'a>>`。这是 Rust trait 里表达异步的显式写法——把「返回的 future 必须 `Send` 且生命周期绑在 `&'a self` 上」写在签名里，而不是靠 `#[async_trait]` 或 `async fn in trait` 隐式约定。
- **`NoopTurnItemEmitter` 返回 `std::future::ready(())`**，而不是空函数体。这样调用方无需为「有没有发射器」分支——两个实现都返回同一个 future 类型。
- **`ExtensionTurnItem` 带 `legacy_events`**，注释解释了为什么需要：

> Core intentionally does not inspect extension-owned payloads, so it cannot derive their legacy fanout. It emits the canonical lifecycle event first, then these extension-provided events.

即核心不认识扩展的负载形状，所以无法自己推导出「这个扩展项目该发哪些兼容事件」。于是扩展自己提供这批事件，宿主按顺序（先规范事件、再遗留事件）发出去。这是把「域知识留在拥有它的那一层」的具体实现。

还有一句：「Core also skips global turn-item contributors here so extensions cannot mutate items owned by other extensions」。即**扩展不能通过这个通道改别人的项目**——只发自己的。

**`ToolCall` 的 `Debug` 实现刻意不打印 `payload` 与历史的内容。**

```rust
.field("has_codex_turn_metadata", &self.codex_turn_metadata.is_some())
.field("turn_item_emitter", &"<host turn item emitter>")
.field("environment_count", &self.environments.len())
.field("payload", &self.payload)
```

四个取舍：

- **`codex_turn_metadata` 打成 `has_...: bool`**：它是 `Option<String>`，内容可能很长。
- **发射器打成固定字符串**：它是 `dyn` trait 对象，没有有意义的 `Debug` 输出。
- **环境打数量**：每个环境带 `Arc<dyn ExecutorFileSystem>`，打印它没意义。
- **`payload` 照打**：因为它是调用的核心内容，调试时必须有。

最后 `finish()` 而不是 `finish_non_exhaustive()`——所有字段都显式处理了，所以不需要留省略号。

**`ToolCall<'call>` 带生命周期参数，链接到环境与历史。** 这决定了它的用法：`ToolCall` 不能随便跨作用域保存，它借用调用栈上的环境数据（`ToolEnvironment<'call>`）。这个设计让「工具执行期间的上下文」与「长期持有的会话状态」在类型上分开——前者是借用的，后者是 `Arc` 共享的。

## 关键取舍

**源码事实：** `ConversationHistory` 用 `LazyLock<Box<[ResponseItem]>, Box<dyn FnOnce...>>` 延迟物化，只在工具读历史时才执行 loader；loader 必须捕获调用时刻的快照而不是查询活的会话状态。

**源码事实：** `response_byte_budget` 对直接调用取「工具上限」与「宿主文本预算」的较小值，对代码模式调用只用工具上限，因为代码模式拿到的是不截断的类型化结果。

**源码事实：** `function_arguments` 在负载形状不匹配时返回 `FunctionCallError::Fatal`（宿主侧错误），而不是可给模型看的工具错误。

**源码事实：** `ToolEnvironment` 把 `environment_id`、绝对化 `cwd`、`Arc<dyn ExecutorFileSystem>` 与沙箱上下文绑在一起，并用 `PhantomData` 标记生命周期（附 TODO 说明这是临时形态）。

**源码事实：** `TurnItemEmitter` 用显式的 `Pin<Box<dyn Future + Send + 'a>>` 而不是 `async fn`，并提供 `NoopTurnItemEmitter` 让调用方不必分支。

**源码事实：** `ExtensionTurnItem` 同时携带规范项目与扩展自带的 `legacy_events`，因为核心不认识扩展负载的形状、无法自行推导遗留事件。

**源码事实：** `ToolCall` 与 `ConversationHistory` 的 `Debug` 都刻意不打印大内容（历史非穷尽、元数据打布尔、环境打计数、发射器打固定串）。

**架构推论：** 这个类型的复杂度不在字段数量，而在它**同时要服务两种调用来源**。`ToolCallSource` 看起来只是「我在哪儿被调用的」这类信息字段，实际上它决定了预算公式、决定了结果是否被文本截断、也决定了下游错误处理的方式。把两条路径统一到一个类型里，代价是每个消费者都要记得看 `source`；好处是工具实现只需写一份。

**架构推论：** 「loader 必须捕获快照而不是查活状态」这条注释是一处非常具体的并发约束。它无法用类型系统表达（闭包签名允许两种做法），只能靠文档。这类「形状相同但语义不同」的约束，在 Rust 里最容易被误实现，也最容易在压测时才暴露。

**架构推论：** `NoopTurnItemEmitter` 的存在揭示了一个实用手法：**给 trait 提供一个「什么都不做」的实现，并把返回类型统一成同一个具体形态**。这样调用点不需要 `if let Some(emitter)`，而 trait 对象与空实现都能直接调用。这与 `Option` + `map` 的写法相比，省掉了一个分支但多了一个类型。

**架构推论：** `Fatal` 与「工具错误」的区别，是这一页里最能体现错误分类成熟度的一处。工具错误会回流给模型看（模型可以据此重试），而 `Fatal` 表示宿主自己的 bug——后者绝不能变成「模型再试一次」。判据是**谁该为这个错误负责**，而不是错误的严重程度。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/tools/src/tool_call.rs` | `ToolCall` | 一次工具调用的完整上下文（身份、能力、策略） |
| `codex-rs/tools/src/tool_call.rs` | `ToolCallSource` | 直接调用 / 代码模式嵌套，决定预算与截断 |
| `codex-rs/tools/src/tool_call.rs` | `ConversationHistory` | 延迟物化的历史快照，只读一次 |
| `codex-rs/tools/src/tool_call.rs` | `ToolEnvironment` | 环境 id、绝对 cwd、文件系统抽象、沙箱上下文 |
| `codex-rs/tools/src/tool_call.rs` | `TurnItemEmitter` / `NoopTurnItemEmitter` | 生命周期事件的注入点与空实现 |
| `codex-rs/tools/src/tool_call.rs` | `ExtensionTurnItem` | 规范项目 + 扩展自带的遗留事件 |
| `codex-rs/tools/src/tool_call.rs` | `TurnItemEmissionFuture` | 显式 `Pin<Box<dyn Future + Send + 'a>>` 别名 |
| `codex-rs/tools/src/tool_call.rs` | `ToolCall::response_byte_budget` | 按来源分支的响应预算 |
| `codex-rs/tools/src/tool_call.rs` | `ToolCall::function_arguments` | 形状判定访问器，失败为 `Fatal` |

## 参考

- [open-interpreter 源码：tools/src/tool_call.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/tools/src/tool_call.rs)
- [Rust 文档：std::sync::LazyLock](https://doc.rust-lang.org/std/sync/struct.LazyLock.html)
- [Rust 文档：PhantomData](https://doc.rust-lang.org/std/marker/struct.PhantomData.html)
- [Rust 异步编程：返回 Future 的 trait 方法](https://rust-lang.github.io/async-book/07_workarounds/05_async_in_traits.html)

## 与主核 / AI 的关系

「一次操作要携带完整上下文，而不只是参数」与主核里的**执行器上下文**同构：主核执行一条查询时要带会话、事务、权限、超时、内存限额，而不只是语法树。两者都把「这次执行的处境」当成一等公民，因为很多决策（能否用某个索引、要不要报错、结果能不能给用户看）依赖处境而非参数。

「延迟物化历史」这条与主核的**按需读页**有相同的经济学：不读就不用付出代价，而绝大多数执行路径不需要全量历史。差别在一致性要求——主核读页有事务可见性保证，这里的 loader 靠「捕获快照」这个手工约定。

`Fatal` 与「工具错误」的分层，与主核里**内部错误码与用户错误的分离**一致：`E_*` 内部码不直接暴露给用户，公开层映射成 `DB_E_*`。判据同样是「谁该负责」。

对 AI 工程的实际意义在于：工具调用是 agent 的核心动作，而这个类型把「一次调用需要考虑的每一件事」都显式化了。三条经验可直接用：**把「来源」做成判别位而不是靠约定**（否则预算与截断逻辑会悄悄错位）、**历史这类大对象要延迟物化**（否则每次调用都克隆一份）、以及**给宿主侧错误与模型侧错误分不同的类型**（否则模型会对宿主的 bug 反复重试）。
