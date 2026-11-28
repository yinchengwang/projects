---
project: ragflow
feature: agent-runtime
title: Agent 运行时
page_type: feature
schema_version: 1
refs:
  - path: internal/agent/runtime/context.go
    symbols: [AgentMessageEmitter, DeferredStream, WithDeferredNodeRegistry]
  - path: internal/agent/runtime/component.go
    symbols: [Component, ComponentFactory, UserFacingError]
  - path: internal/agent/component/base.go
    symbols: [Component, ParamBase, ParamError]
---

# Agent 运行时

## 一句话职责

把「一次 Agent 运行」的所有横切关注点塞进 `context`：状态、消息发射器、增量流、延迟节点登记表、执行选项——再配一套分层的组件契约与错误分类，使得一个组件的 `Invoke` 只写业务逻辑，而「这次调用能不能往外发消息」「这个节点的完成事件要不要等下游消费完才发」这类事由运行时决定。

## 机制是什么

这个包（`internal/agent/runtime`，17 个文件）是 `canvas` 与 `component` 两个包的**公共依赖**——f01 已经讲过它为什么存在（打断 import 环），这一页讲它里面装了什么。

核心手法只有一条：**所有跨层的运行期事实都挂在 `context` 上，用一个未导出的 `struct{}` 空结构体作 key。**

```go
type stateCtxKey struct{}
type agentMessageEmitterCtxKey struct{}
type canvasMessageEmitterCtxKey struct{}
type agentDeltaSinkCtxKey struct{}
type componentExecutionOptionsCtxKey struct{}
type deferredNodeRegistryCtxKey struct{}
```

六个 key，每一个对应一类运行期事实。而注释解释了为什么要定义在包级而不是每次调用用新的 `struct{}{}`：

> `stateCtxKey` 是由 `WithState` / `GetStateFromContext` 使用的未导出 context key。**定义在包作用域好让它的身份跨调用稳定**（每次调用一个新的 `struct{}{}` 会以不同的 key 命名，从而破坏 `ctx.Value` 查找）。

这是 Go 里 `context` 用法的经典坑：`struct{}{}` 看起来「空所以相等」，但**两个不同的 `struct{}{}` 值作为 interface 比较时不相等**（类型相同但值比较需要可比较类型，而空结构体字面量作为接口值是不同的对象）。所以必须把一个**具名类型的零值**存成包级变量——这样 `ctx.Value(key)` 用同一个值去查才能命中。

而 `context.go` 的包注释说明了这套 context 通道与 eino 的关系：

> `compile` 入口（`canvas/compile.go`）每次运行通过 `WithState` 把 `*CanvasState` 挂到 ctx 上一次。组件体通过 `GetStateFromContext` 取出它。**eino 内部的状态管道也通过 `WithGenLocalState` 传递状态；这条 context-key 路径是那些不跑在 eino handler 里的代码所用的兜底**（例如循环条件闭包、临时测试、以及 `BuildWorkflow` 安装的占位组件体）。

所以存在**两条状态传递通道**：eino 的（框架级）与 context 的（兜底级）。而 context 那条存在的理由是「不是每个东西都跑在 eino handler 里」——循环条件闭包就是被直接调用的。

## 怎么实现的

**`Component` 有三个不同的定义，各自面向一个消费者。**

| 位置 | 方法集 | 谁用 |
|---|---|---|
| `runtime.Component` | `Invoke` | 画布构建器（f01）|
| `component.Component` | `Name` / `Invoke` / `Stream` / `Inputs` / `Outputs` | 组件实现者 |
| `component.ParamBase` | `Update` / `Check` / `AsDict` | 需要参数校验的组件 |

`component/base.go` 的包注释把这个关系讲清了：

> `Component` 是每个 RAGFlow 组件实现的运行期契约；它是比 `internal/agent/runtime.Component`（画布在构建期需要的最小 `Invoke`-only 表面）**更丰富**的接口。这里任何具体的 `*Component` 都**结构化地满足** `runtime.Component`，这就是画布构建器如何通过 `runtime.DefaultFactory()` 消费一个注册组件的方式。

所以「富接口实现小接口」不是巧合，是设计：`component.Component` 的定义里多出来的 `Stream` / `Inputs` / `Outputs` 只对「组件实现者与工具化」有用，而构建器不需要它们。给每个消费者**只给它需要的那一面**，是 Go 接口切分的标准做法。

`Stream` 的 docstring 说明了它的可选性：

> 流式变体。默认实现可以返回一个缓冲通道，发出与 `Invoke` 相同的载荷然后关闭——原生流式的组件（LLM、Message）覆盖它。**对非流式组件可以返回 `(nil, nil)`。**

而 `Inputs()` / `Outputs()` 的 docstring 明确它们**不用于运行期**：

> `Inputs()` 与 `Outputs()` 返回给工具 / 文档 / 图自省用的参数元数据——`name → human description`。**不在运行期使用。**

这两句合起来是一个很实用的边界声明：接口里混着「必须实现的行为」与「只为自省存在的元数据」，而注释把它们区分开了。

**`ParamBase` 的三方法分工，以及它为什么是可选接口。**

```go
type ParamBase interface {
    Update(conf map[string]any) error   // 把 conf 拷进接收者，校验类型
    Check() error                        // 深度校验（必填、范围），Invoke 前调一次
    AsDict() map[string]any              // 序列化/调试用
}
```

docstring：「需要校验的组件可以嵌入 `*BaseParam` 或直接实现这个接口。**不需要的组件（例如 ExitLoop）可以省略**。」

所以「参数校验」是可选能力而不是强制契约——这与 f01 里 `ExitLoop` 被折掉的事实呼应：那个组件本来就没有参数。

而 `Update` 与 `Check` 的分工值得注意：`Update` 做**类型层面的拷贝与转换**（从 DSL 的 `map[string]any` 到结构化字段），`Check` 做**语义层面的深度校验**（必填、范围）。分成两步的理由是：类型错误在 `Update` 时就能报（且能指出哪个字段），而语义错误需要整个对象组装完才能判（例如「A 与 B 互斥」）。

**错误分类是这一层最重要的产出，一共四类。**

```go
var ErrNotImplemented = fmt.Errorf("component: not yet implemented (placeholder)")

type UserFacingError struct{ Err error }
type DeferredStreamError struct{ Text string; Err error }
type ParamError struct{ Field string; Reason string }
```

四类的语义各不相同：

| 类型 | 谁该看到 | 怎么用 |
|---|---|---|
| `ErrNotImplemented` | 开发者 / 测试 | `errors.Is` 断言，**构建器不拦截** |
| `UserFacingError` | 最终用户 | 直接返回客户端，**不带画布执行前缀** |
| `DeferredStreamError` | 运行处理器 | 用 `FailureText()` 取给用户的那句 |
| `ParamError` | 用户 + 开发者 | 带字段名，能指出哪个参数 |

`ErrNotImplemented` 的 docstring 里有一段关于「消息是给人看的、哨兵是给代码用的」的告诫：

> 测试或日志 grep 代码若想匹配这个字符串，应当用 `errors.Is(err, ErrNotImplemented)` 而不是子串匹配——**消息是给人的，哨兵是给代码的**。

`UserFacingError` 的价值在于**它是一次「这句错误可以直接给用户看」的声明**：

```go
// UserFacingError marks a validated request failure whose message is safe to
// return directly to the Agent client without canvas execution prefixes.
```

「without canvas execution prefixes」是关键：正常的错误会被层层包装（`workflowx: iteration 3: component LLM: ...`），而用户不该看到这个调用栈。所以 `UserFacingError` 是一个**显式的「到此为止」标记**。

而 `DeferredStreamError` 的三层信息分工很精细：

```go
type DeferredStreamError struct {
    Text string   // 给用户的失败文本（Agent 的 `_ERROR` 输出）
    Err  error    // 底层原因（流本身打开失败时），让取消传播
}

func (e *DeferredStreamError) FailureText() string {
    if e.Err != nil { return e.Err.Error() }
    return e.Text
}
```

docstring：「`Text` 持有面向用户的失败（Agent 的 `_ERROR` 输出）；`Err` 持有底层原因（流本身打开失败时），**所以取消可以继续传播**。运行处理器在聊天消息流里呈现 `FailureText`，而不是一个错误帧」。

三件事在这里被分开：

1. **用户看到的文本** ——`Text`（或 `Err` 的文本，若后者存在）。
2. **取消信号** ——通过 `Unwrap()` 返回 `Err`，于是 `errors.Is(err, context.Canceled)` 仍然成立。
3. **呈现位置** ——不是错误帧，而是聊天消息流里的普通消息。

第 2 点是最容易做错的：如果为了「给出友好的错误消息」而丢掉底层 error，那么一次用户主动取消会被呈现成「Agent 出错了」。保留 `Err` 让取消语义不被吞掉。

**`MarkdownSafeErrorText` 是这一层最冷门但最具体的一个函数。**

```go
const einoNodePathSeparator = "\n------------------------\n"

func MarkdownSafeErrorText(err error) string {
    return strings.ReplaceAll(err.Error(), einoNodePathSeparator, "\n\n")
}
```

注释讲了原因：

> `einoNodePathSeparator` 是 eino 的 `internalError.Error()` 插在包裹原因与其节点路径诊断之间的装饰：
>
> ```
> [NodeRunError] <cause>
> ------------------------
> node path: [...]
> ```
>
> **一行只有破折号的段落下面直接跟正文，是合法的 Markdown setext 标题语法**，所以聊天客户端会把整个错误块渲染成 `<h2>`。换成一个空行：节点路径作为普通段落仍然可见，而错误以正常正文字号渲染。

这是一个真实的跨系统交互缺陷：Go 的错误消息格式（用 `----` 做视觉分隔）与 Markdown 的语法（`----` 在文字下方 = setext 二级标题）恰好冲突。而修法是最小的：把分隔线换成空行。

**`DeferredStream` 是「生产者不打开、消费者才打开」的惰性值，而它的类型定义只有一行。**

```go
type DeferredStream struct {
    Open func(context.Context, AgentDeltaSink) (map[string]any, error)
}

func IsDeferredStream(v any) bool {
    deferred, ok := v.(*DeferredStream)
    return ok && deferred != nil && deferred.Open != nil
}
```

docstring：「`DeferredStream` 是一个惰性组件值。**生产者故意由消费方的 Message 节点打开。**」

这个设计的动机可以从前面的 `ComponentExecutionOptions` 看出来：

```go
type ComponentExecutionOptions struct {
    DeferAgentToMessage        bool
    DeferLLMToMessage          bool
    SuppressAgentMessageEvents bool
}
```

三个开关都是**编译期图决策**，docstring 说明它们「把编译期的图决策带进一次组件调用，而**不把内部标志泄漏进 DSL 参数**」。

`DeferAgentToMessage` / `DeferLLMToMessage` 的意思是：Agent / LLM 组件**不自己往外发流**，而是返回一个 `DeferredStream`，由下游的 Message 组件打开并消费。为什么？因为只有下游知道「这个回答最终要发到哪里、以什么形态发」。而如果 Agent 自己发，那它在图里的位置就无所谓了——但一个 Agent 后面可能接一个 Message 也可能接一个条件分支。

`IsDeferredStream` 的三重检查（类型、非 nil、`Open` 非 nil）说明调用方会拿任意值来问「你是不是延迟流」，所以这里不能只做一个类型断言。

**而「延迟」这件事不只影响数据流，还影响事件时序——这就是 `WithDeferredNodeRegistry` 存在的原因。**

```go
func WithDeferredNodeRegistry(ctx context.Context) context.Context
func RegisterDeferredNode(ctx context.Context, nodeID string, complete func(err error))
func CompleteDeferredNode(ctx context.Context, nodeID string, err error)
func CompleteAllDeferredNodes(ctx context.Context, errs ...error)
```

`WithDeferredNodeRegistry` 的 docstring：

> 安装每次运行用的回调，用来**把 Agent 节点的 `node_finished` 事件延迟到它的下游 Message 消费完那个惰性流之后**。

所以问题是这样：Agent 组件跑完了（它返回了 `DeferredStream`），但**工作还没做完**——真正的输出要等 Message 打开流才知道。如果在 Agent 返回时就发 `node_finished`，那么前端会在「Agent 完成」之后才收到文本，时序错了。

`RegisterDeferredNode` 注册一个节点 id 与完成回调；`CompleteDeferredNode` 按 id 取回回调、删掉、再调用（**先删后调，所以恰好触发一次**）；`CompleteAllDeferredNodes` 是兜底（运行结束或失败时把所有未完成的都触发，避免事件永久缺失）。

而 `deferredNodeRegistry` 的结构只有两个字段：

```go
type deferredNodeRegistry struct {
    mu          sync.Mutex
    completions map[string]func(err error)
}
```

值得注意的是 **`CompleteAllDeferredNodes` 接收 `errs ...error` 但只取 `errs[0]`**：

```go
var err error
if len(errs) > 0 {
    err = errs[0]
}
```

所以它的签名看似「可以传多个错误」，实际只用第一个。这是一个签名与行为不一致的小瑕疵——但它保证了调用方不需要先判断「有几个错误」就能调。

**`EmitAgentMessage` 那一族函数有六个状态字段，而它们都在同一个 struct 上。**

```go
type agentMessageEmitterState struct {
    mu            sync.Mutex
    emit          AgentMessageEmitter
    finalize      func() bool
    reset         func()
    emitted       bool
    suppressed    bool
    runEmitted    bool
    runSuppressed bool
    agentContent  strings.Builder
}
```

`emitted` / `suppressed` 是**本次迭代**的状态，`runEmitted` / `runSuppressed` 是**整次运行**的状态。所以同一个「有没有发过」的问题有两个粒度的答案，而两者都需要（一次迭代里发过不等于整次运行发过；一次迭代被抑制不等于整次都该被抑制）。

`agentContent` 是一个 `strings.Builder` 累积 Agent 的全部输出，而 `ResetAgentMessageEmission` 会把 `emitted` / `suppressed` / `agentContent` 全部清掉（并调 `reset` 回调）。所以这个 struct 同时承担「状态机」与「内容累积」两个职责。

## 关键取舍

**源码事实：** 六个 context key 全部是包级的未导出空结构体类型，注释明确解释：每次调用新建 `struct{}{}` 会让 key 身份不同，从而破坏 `ctx.Value` 查找。

**源码事实：** 存在**两条**状态传递通道：eino 内部的 `WithGenLocalState`，与 `WithState` 挂 context 的兜底路径；兜底路径服务「不跑在 eino handler 里」的代码（循环条件闭包、测试、占位组件体）。

**源码事实：** `runtime.Component` 只有 `Invoke`，`component.Component` 有五个方法；`component/base.go` 的包注释明确「任何具体的 `*Component` 结构化地满足 `runtime.Component`」。

**源码事实：** `Stream` 允许返回 `(nil, nil)` 表示非流式；`Inputs()` / `Outputs()` 的 docstring 明说「不在运行期使用」，只为工具 / 文档 / 图自省。

**源码事实：** `ParamBase` 是**可选**接口（`ExitLoop` 这类无参组件省略它）；`Update` 与 `Check` 分两步：前者做类型转换与拷贝，后者做语义深度校验，`Invoke` 前调一次 `Check`。

**源码事实：** `ErrNotImplemented` **构建器不拦截**——它沿 `workflowx` 层传播并让整次运行失败，与 Go「依赖返回的 error 让调用失败」的默认语义一致；想要软失败必须自己 wrap `Invoke`。

**源码事实：** `ErrNotImplemented` 的 docstring 要求用 `errors.Is` 而不是子串匹配——「消息是给人的，哨兵是给代码的」；且用 `fmt.Errorf` 而非 `errors.New`（值相同但少了 `New` 的语义暗示）。

**源码事实：** `UserFacingError` 的契约是「消息可以**不带画布执行前缀**直接返回客户端」——它是一次显式的「错误包装到此为止」标记。

**源码事实：** `DeferredStreamError` 把三层信息分开：`Text` 是给用户的、`Err` 是底层原因（保留它让 `errors.Is(err, context.Canceled)` 仍成立）、呈现位置是**聊天消息流而不是错误帧**。

**源码事实：** `MarkdownSafeErrorText` 存在的具体原因是：eino 的错误消息里用一行 `------------------------` 做视觉分隔，而 Markdown 把「文字下方一行破折号」解析成 setext 二级标题，于是整个错误块被渲染成 `<h2>`。

**源码事实：** `DeferredStream` 的 docstring 明说「生产者故意由消费方的 Message 节点打开」；`IsDeferredStream` 做类型 + 非 nil + `Open` 非 nil 三重检查，因为它会被拿去问任意值。

**源码事实：** `ComponentExecutionOptions` 的三个字段是编译期图决策（`DeferAgentToMessage` / `DeferLLMToMessage` / `SuppressAgentMessageEvents`），docstring 说这是为了「不把内部标志泄漏进 DSL 参数」。

**源码事实：** `CompleteDeferredNode` **先删后调**，所以同一节点 id 的回调恰好触发一次；`CompleteAllDeferredNodes` 是兜底，防事件永久缺失。

**源码事实：** `CompleteAllDeferredNodes` 的签名是 `errs ...error` 但**只用 `errs[0]`**——签名与行为不完全一致，好处是调用方不必先数错误个数。

**源码事实：** 消息发射状态有六个字段，`emitted` / `suppressed` 是本次迭代粒度，`runEmitted` / `runSuppressed` 是整次运行粒度；`agentContent` 用 `strings.Builder` 累积 Agent 全部输出。

**架构推论：** 把横切关注点放进 `context` 而不是函数参数，代价是**不可见性**——读一个组件的 `Invoke` 看不出它依赖了状态、发射器、延迟登记表中的哪一个。收益是不必给每个组件都加一套参数，且新的横切关注点可以后加。判据是：**这个事实是「整次运行」的属性还是「这个组件」的属性？** 前者进 context，后者进参数。这一层把「状态 / 发射器 / 增量流 / 延迟登记 / 执行选项」都判为前者，是对的——它们确实都不属于任何单个组件。

**架构推论：** 错误类型按「谁该看到」切分，比按「哪里出错」切分更有用。这一层的四类恰好对应四个受众：`ErrNotImplemented`（开发者/测试，用 `errors.Is`）、`UserFacingError`（最终用户，可直接展示）、`DeferredStreamError`（运行处理器，需要同时拿用户文本与取消信号）、`ParamError`（用户+开发者，需要字段名）。**同一个故障对四个受众需要四种消息形态**，而用单一 error 类型时只能挑一个受众，其余三个只能被迫看自己不该看的文本。

**架构推论：** `DeferredStream` 与 `WithDeferredNodeRegistry` 的组合揭示了一条通用规律：**惰性求值会把「完成」拆成两个事件**——「生产者返回」与「消费者消费完」。任何引入惰性（延迟打开、延迟展开、流式）的系统都会遇到这个问题，而事件时序必须跟着数据流一起改。这里的解法是显式的登记表 + 兜底完成，而不是让生产者猜「下游什么时候会用完」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agent/runtime/context.go` | `stateCtxKey` 等六个 key | 包级未导出空结构体，身份稳定 |
| `internal/agent/runtime/context.go` | `AgentMessageEmitter` | 发射可见的 assistant 增量 |
| `internal/agent/runtime/context.go` | `DeferredStream` | 惰性组件值，消费方才打开 |
| `internal/agent/runtime/context.go` | `IsDeferredStream` | 三重检查（类型/非 nil/Open 非 nil） |
| `internal/agent/runtime/context.go` | `WithDeferredNodeRegistry` | 装延迟节点完成回调的登记表 |
| `internal/agent/runtime/context.go` | `RegisterDeferredNode` / `CompleteDeferredNode` | 注册与触发，触发恰好一次 |
| `internal/agent/runtime/context.go` | `CompleteAllDeferredNodes` | 兜底，防事件永久缺失 |
| `internal/agent/runtime/context.go` | `ComponentExecutionOptions` | 编译期图决策传给组件调用 |
| `internal/agent/runtime/context.go` | `agentMessageEmitterState` | 迭代级 + 运行级的两套发射状态 |
| `internal/agent/runtime/component.go` | `Component` | 最小契约，只有 `Invoke` |
| `internal/agent/runtime/component.go` | `ComponentFactory` | `func(name, params) (Component, error)` |
| `internal/agent/runtime/component.go` | `UserFacingError` | 可直接返回客户端的错误标记 |
| `internal/agent/runtime/component.go` | `DeferredStreamError` | 用户文本 + 取消传播 + 非错误帧呈现 |
| `internal/agent/runtime/component.go` | `ErrNotImplemented` | 未移植组件的哨兵，构建器不拦截 |
| `internal/agent/runtime/component.go` | `ParamError` | 带字段名的参数校验失败 |
| `internal/agent/runtime/component.go` | `MarkdownSafeErrorText` | 把 eino 的分隔线换成空行，防 setext 标题 |
| `internal/agent/component/base.go` | `Component` | 五方法富接口，结构化满足 `runtime.Component` |
| `internal/agent/component/base.go` | `ParamBase` | 可选的参数校验/序列化表面 |
| `internal/agent/component/base.go` | `ParamError` | `runtime.ParamError` 的别名，共享同一类型 |
| `internal/agent/component/base.go` | `BeOutput` | 把单值包成 `{"content": v}` 的统一输出帧 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agent/runtime/context.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/runtime/context.go)
- [Go 文档：context 包](https://pkg.go.dev/context)
- [Markdown 规范：setext 标题](https://spec.commonmark.org/0.31.2/#setext-heading)

## 与主核 / AI 的关系

主核里最接近的是**执行器上下文（executor context / memory context）**：PostgreSQL 的 `ExprContext` 一次查询里传递「当前元组、参数、内存上下文、超时」等一切横切事实，而算子函数不需要把这些当参数一个个接。这里的 `context` 挂载与那个 `ExprContext` 是同一种设计：**横切事实集中在一个通行对象上，算子签名保持干净**。

**差别在于「是否有结构化类型」。** 主核的 `ExprContext` 是一个 C 结构体，字段在编译期就有类型、访问是直接内存读取。而 Go 的 `context.Value` 是 `any`，每次取值都要类型断言、且取不到时只能拿到零值。这一层的 `GetStateFromContext` 因此必须返回 `(state, error)` 两个值，而 `ComponentExecutionOptionsFromContext` 在缺失时静默返回零值 struct（于是「没装选项」与「装了一个全 false 的选项」不可区分）。**这是 context 方案的固有代价**：方便，但把「字段存在性」变成了运行期问题。

与 e04（Pregel 引擎）的关系是这一页最重要的对照，因为它们都在「让 Agent 图跑起来」这件事上做选择，而选择相反：

- **e04 的引擎**用超步栅栏换确定性，用检查点换可恢复，用 `AnyPredecessor` / `AllPredecessor` 两种语义换 DAG / 图两种用途。它是**一个自包含的执行内核**。
- **这一页的运行时**不执行任何东西——它只提供契约（`Component`）、错误分类、以及 context 挂载的横切事实。它假定别人来调 `Invoke`。

所以两者是「引擎 + 契约」的分工：`internal/harness/graph/pregel` 决定**什么时候按什么顺序调**，`internal/agent/runtime` 决定**调的时候能拿到什么、失败了怎么分类**。这个切分让引擎可以对组件一无所知，而组件对执行顺序一无所知。

与 f01（画布）的关系是类型归属：`Component` / `ComponentFactory` / `CanvasState` 的定义都在这一页，f01 只是别名转发。所以读 f01 是「哪些类型存在」，读这一页是「这些类型为什么这么设计」。

对 AI / agent 工程最实用的一条是 **`DeferredStream` + 延迟节点完成登记**这对组合。任何流式 Agent 都会遇到同一个时序问题：一个生成类节点「跑完了」但输出还没发出去，而前端的进度指示需要准确的完成时刻。若不处理，用户会看到「Agent 已完成」之后才开始滚出文字。正确做法是把「完成」拆成两个事件（生产者返回 / 消费者消费完）并显式登记，而不是把 `node_finished` 在生产者返回时就发出去。

另一条是 `DeferredStreamError` 那种**「给用户的文本」与「给机器的错误」分开持有**的形态。把两者塞进一个 error 会逼出两种坏设计：要么把底层错误包装成友好文案（于是取消变成「出错了」），要么给用户看原始错误（于是暴露调用栈）。分开持有再提供一个 `FailureText()` 取值入口，两边都满足。

最后一条是 `MarkdownSafeErrorText`。它提醒的是跨系统格式冲突这个类别：**一个系统用来做视觉分隔的字符序列，在另一个系统里可能是语法**。这类问题不会在单元测试里暴露（因为错误消息断言的是语义而不是渲染结果），只在用户截图里出现。可复用的判据是：**任何要进入 Markdown / HTML / 终端颜色序列的文本，都要过一遍「目标格式的语法检查」**。
