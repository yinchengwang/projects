---
project: ragflow
feature: canvas
title: 画布与组件工厂
page_type: feature
schema_version: 1
refs:
  - path: internal/agent/canvas/canvas.go
    symbols: [Canvas, CanvasState, CanvasComponentObj, ComponentFactory]
---

# 画布与组件工厂

## 一句话职责

把用户在前端连出来的一张图（节点 + 连线 + 每个节点的一坨参数）读成 Go 值，并提供一个「给一个组件名和参数字典，换回一个能跑的组件」的工厂契约——前者是可序列化的**数据**，后者是运行期的**行为入口**，而这两件事被刻意拆进了两个包（`internal/agent/canvas` 与 `internal/agent/runtime`），目的是把历史上存在的 `canvas ↔ component` 循环依赖切断。

## 机制是什么

读这张画布的数据结构，只需要注意三件事：

1. **`Canvas` 是 DSL 的内存形态，不是运行时状态。** 它的字段里 `Components`、`Path`、`History`、`Memory`、`Retrieval`、`Globals` 全部来自用户存下来的那一行 JSON；唯一带 `json:"-"` 的字段 `NodeParents` 是**解码侧的镜像**，注释写明「后端把进来的 DSL 当作只读，这个镜像只用来决定编译期哪些节点属于一个 Loop / Parallel 的体」。

2. **`CanvasComponent` 的 `DisplayName` 也是 `json:"-"`。** 同一段注释区分了两类字段：「可编辑的图节点名，用于执行日志」——也就是**给人看的**，不是「可执行的组件拓扑」的一部分。这是一个很容易被混淆的边界：日志里显示的节点人名 与 程序用来路由的节点标识 是两套东西。

3. **`CanvasComponentObj` 只有两个字段：`ComponentName` 与 `Params`。** 组件名匹配 `agent/component/<name>.py` 里的类名（大小写不敏感，按 Python v1 DSL 的语义）。所以整个画布的可执行部分被压缩成「一个名字 + 一个 `map[string]any`」——这正是组件工厂签名 `func(name string, params map[string]any)` 的形状。

而 `ComponentFactory` 这个类型本身是在 `runtime` 包里定义的，`canvas` 只是别名转发。包注释把这个动机写得很直接：

> Shared runtime contracts (CanvasState, Component, ComponentFactory, state context plumbing, template helpers) live in `internal/agent/runtime`. Canvas re-exports them through thin aliases so existing call sites keep working while **breaking the historic canvas ↔ component import cycle**.

所以这一层的架构决定是：**把「契约」与「实现」切开**。`canvas` 需要知道「组件长什么样」（接口），但不需要知道「有哪些组件」（注册表）；`component` 需要知道「状态与工厂怎么用」，但不需要知道「画布怎么解码」。全部共享物搬到第三个包，两个包各自指向中间。

## 怎么实现的

**工厂是一个包级变量 + 读写锁，而 `SetDefaultFactory` 的语义被明确改成「总是替换」。**

```go
var (
    factoryMu      sync.RWMutex
    defaultFactory ComponentFactory
)

func SetDefaultFactory(f ComponentFactory) {
    factoryMu.Lock()
    defer factoryMu.Unlock()
    defaultFactory = f
}
```

注释记录了一次**设计变更**：

> After Phase 0, the "first writer wins" guard is **REMOVED**: `SetDefaultFactory` now ALWAYS replaces the active default, regardless of whether one is already installed. This preserves the existing test-override pattern where tests save the previous factory, install a stub, and restore on `t.Cleanup`. Passing nil clears the factory.

这条值得单独说：原来的「先写者赢」是一个**防止生产代码被覆盖**的保护，但它同时让「测试装桩」变成不可能——因为组件包在 `init()` 里已经写了第一次。所以这个保护与测试模式是直接冲突的。解法是**放弃这个保护**，把「不许乱装」的责任交给约定（测试要在 `t.Cleanup` 里还原），代价是任何 `init()` 顺序错乱都可能导致工厂被错误替换。这是一个真实的取舍：**进程级可变全局 + 测试隔离**这两个需求在设计上就是矛盾的。

**两层模型：生产是一个闭包，覆盖是直接换值。**

> * Production: `installDefaultRegistryFactory` installs a closure that calls `runtime.DefaultRegistry.Lookup` on every invocation. **It captures `DefaultRegistry` by reference**, so even if `installDefaultRegistryFactory` runs before all `init()` registrations complete, the factory is correct at every subsequent lookup.
> * Override: tests call `SetDefaultFactory(stub)` directly.

第二句解释了一个很细的 Go 初始化顺序问题：若闭包在安装时**捕获**了注册表的值（拷贝），那么「安装发生在部分 `init()` 之前」会让它永远看不到后续注册的组件。改为「每次调用时去读注册表」，就把这个顺序依赖消除了——**延迟绑定而不是提前绑定**。这是包级 `init()` 组合体（各组件在各自 `init()` 里注册）的标准修法。

**`canvas` 侧提供一个 per-pipeline 的工厂覆盖，走 context 而不是全局。**

```go
func WithComponentFactory(ctx context.Context, factory runtime.ComponentFactory) context.Context
```

docstring：「`BuildWorkflow` 在构造节点体时用它，好让一个 pipeline 实例能独立于进程级 `runtime.DefaultFactory` 解析组件」。而 `factory == nil` 时返回原 ctx（不装一个 nil 值），所以「没给覆盖」与「给了 nil」不可区分——这里选的是「nil 等于没给」。

这个设计让**同一进程里两个 pipeline 用不同组件集合**成为可能，而全局工厂仍然服务默认路径。代价是：任何需要工厂的地方都必须拿到 `ctx`，而不能直接读全局。

**`Canvas.Close()` 是一次「沿参数树找可关闭对象」的遍历。**

```go
func (c *Canvas) Close() {
    if c == nil { return }
    seen := make(map[any]bool)
    for _, comp := range c.Components {
        for _, v := range comp.Obj.Params { walkAndClose(v, seen) }
    }
}
```

`walkAndClose` 递归进 `map[string]any` 与 `[]any`，遇到实现 `Close()` 的值就调一次并**按接口身份去重**（`seen[closer]`）。而 `safeClose` 用 `recover()` 吞掉 panic：

```go
func safeClose(closer interface{ Close() }) {
    defer func() {
        if rec := recover(); rec != nil {
            common.Warn("agent: Close() panicked", zap.Any("recover", rec))
        }
    }()
    closer.Close()
}
```

三点关于这段的说明：

- **为什么需要它**：注释说「在 Go 的架构里 MCP 会话是每次调用一个且自动拆除，`Close()` 是一个 best-effort 钩子，确保空闲的 HTTP 连接被释放，即使适配器活得比一次调用更久」。所以它是一个**资源卫生钩子**，不是正确性依赖。
- **为什么要吞 panic**：`Canvas.Close()` 通常在 teardown 路径上被调，此时一个畸形资源不该让整个清理中断。这个方向是对的但有代价：**panic 被降级成 WARN，所以一个反复 panic 的 `Close()` 不会让任何人失败**。
- **`seen` 用 `map[any]bool` 意味着 key 必须是可比较的**。把接口值直接当 map key，在底层类型不可比较时（例如含 slice 的结构体）会 panic——而这里没有防护。实际上能实现 `Close()` 的对象通常是 `*SomeClient` 指针，所以实践中安全。

**别名转发的清单本身就是一份契约索引。**

`canvas` 包转发了：`CanvasState` / `NewCanvasState` / `Component` / `ComponentFactory` / `SetDefaultFactory` / `withState` / `GetStateFromContext` / `VarRefPattern` / `ExtractRefs` / `ResolveTemplate`。而 `runtime` 侧才是真正的定义地。这个模式有一个明确的好处：**老调用点不用改**（`canvas.X` 继续能编译），而新代码可以逐步迁到 `runtime.X`。注释也写了「new code should prefer the runtime package directly」——即这是一次**未完成的迁移**，别名是过渡层。

而 `legacyNoOpNames` 揭示了一个具体的兼容性问题：

```go
var legacyNoOpNames = map[string]bool{"exitloop": true}
```

注释说这是「Go 移植版认得、但没有真实实现的 DSL v1 组件名」，遇到时映射到「和占位体一样的 no-op echo lambda」。而更关键的是关于 Loop 语义的那一段：

> Membership semantics inside a Loop's sub-graph: legacy names that appear as descendants of a Loop are **absorbed as no-op members** of the sub-graph; they do not contribute to loop control. **Termination is driven by the Loop's `loop_termination_condition` predicate, not by reaching an ExitLoop node.**

所以 Python 版里「走到 ExitLoop 节点就退出循环」这个控制流**在 Go 版里不存在**：退出条件被改成了谓词。而这个差异不是 bug 而是设计——它顺带解释了 f06 里 `LoopCondition` 为什么是 `func(ctx, iteration, prev, next) (bool, error)` 的形状：**控制流从「移动到特殊节点」变成了「每轮结束后求值」**。

## 关键取舍

**源码事实：** `Canvas` 的 `NodeParents` 与 `CanvasComponent` 的 `DisplayName` 都标 `json:"-"`；前者是解码侧镜像（只用于编译期决定 Loop / Parallel 的成员），后者是日志用的人名——两者都不属于可执行拓扑。

**源码事实：** `CanvasComponentObj` 只有 `ComponentName` 与 `Params` 两个字段，所以整个画布的可执行部分被压成「一个名字 + 一个参数字典」，与 `ComponentFactory` 的签名同形。

**源码事实：** `CanvasState` / `Component` / `ComponentFactory` 的**定义**在 `internal/agent/runtime`，`canvas` 只做别名转发；包注释说明这是为了切断历史上的 `canvas ↔ component` 循环依赖，且注明「新代码应优先直接用 runtime 包」。

**源码事实：** `SetDefaultFactory` 的「先写者赢」保护被**移除**，现在是「总是替换」；理由是要保住「测试存旧值 → 装桩 → `t.Cleanup` 还原」这个模式。传 nil 是清空。

**源码事实：** 生产工厂是一个闭包，**在每次调用时读** `runtime.DefaultRegistry`（延迟绑定），注释说明这样「即使安装发生在部分 `init()` 注册完成之前，后续每次 lookup 都是对的」。

**源码事实：** `installDefaultRegistryFactory` 被额外导出，原因是调用点在另一个包（`internal/agent/component`），Go 的可见性不允许跨包访问未导出名。

**源码事实：** `WithComponentFactory` 提供 per-pipeline 的 context 级工厂覆盖，`factory == nil` 时返回原 ctx。

**源码事实：** `Canvas.Close()` 沿 `Obj.Params` 递归找到所有实现 `Close()` 的值按接口身份去重后调用，`safeClose` 用 `recover` 把 panic 降级成 WARN——因为它在 teardown 路径上，且注释明说这只是「best-effort」，Go 版 MCP 会话本身每次调用自动拆除。

**源码事实：** `legacyNoOpNames` 只含 `exitloop`，且 Loop 内出现时被「吸收为子图的 no-op 成员」；**循环终止由 `loop_termination_condition` 谓词驱动，而不是靠走到 ExitLoop 节点**。

**架构推论：** 「数据在 canvas、契约在 runtime、实现在 component」这个三包结构是**为打断循环依赖而做的拓扑重排**，而不是分层洁癖。判据很清晰：`canvas` 需要「组件是什么形状」（接口），`component` 需要「状态与工厂怎么用」，两者都不需要对方的实现细节——所以共享物必须在第三个包里。任何 Go 项目里出现 `A → B → A` 的 import 环，正确解法几乎都是「把双方都需要的类型提到第三个包，剩下的用别名保编译」。

**架构推论：** `Component` 接口在 `runtime` 里只有一个方法（`Invoke`），而 `component` 包里的同名接口有五个（`Name` / `Invoke` / `Stream` / `Inputs` / `Outputs`）。**用 Go 的结构化类型让「富接口实现小接口」**，于是画布构建器只依赖最小的那个面。这是「接口按消费者切分」的范例：不是「定义一个完整接口然后大家都用它」，而是「每个消费者拿它真正需要的那些方法」。

**架构推论：** `SetDefaultFactory` 放弃写保护，本质上承认了**进程级全局与测试隔离无法同时满足**。一旦接受这个前提，正确的做法就不是「加更复杂的保护」，而是「把保护的责任外化成约定 + 提供 `ResetDefaultFactoryForTesting`」。反过来说：任何仍然有「先写者赢」这类保护的可变全局，都要问一句「测试怎么装桩」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agent/canvas/canvas.go` | `Canvas` | DSL 的内存形态，含 `NodeParents` 解码镜像 |
| `internal/agent/canvas/canvas.go` | `CanvasState` | `runtime.CanvasState` 的别名 |
| `internal/agent/canvas/canvas.go` | `CanvasComponentObj` | 组件名 + 参数字典，与工厂签名同形 |
| `internal/agent/canvas/canvas.go` | `ComponentFactory` | `runtime.ComponentFactory` 的别名 |
| `internal/agent/canvas/canvas.go` | `Canvas.Close` | 沿参数树找可关闭对象，best-effort 释放 |
| `internal/agent/canvas/canvas.go` | `legacyNoOpNames` | 只含 `exitloop`，Loop 终止改由谓词驱动 |
| `internal/agent/canvas/factory_context.go` | `WithComponentFactory` | per-pipeline 的 ctx 级工厂覆盖 |
| `internal/agent/runtime/component.go` | `Component` | 最小契约：只有 `Invoke` |
| `internal/agent/runtime/component.go` | `ComponentFactory` | `func(name, params) (Component, error)` |
| `internal/agent/runtime/component.go` | `SetDefaultFactory` / `DefaultFactory` | 全局工厂，总是替换，读写锁保护 |
| `internal/agent/runtime/component.go` | `InstallDefaultRegistryFactory` | 装一个延迟绑定注册表的闭包 |
| `internal/agent/runtime/component.go` | `ErrNotImplemented` | 未移植组件的哨兵，**构建器不拦截** |
| `internal/agent/runtime/registry.go` | `DefaultRegistry` | 组件注册表，按名 `Lookup` |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agent/canvas/canvas.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/canvas/canvas.go)
- [Go 文档：结构化类型与接口满足](https://go.dev/ref/spec#Interface_types)
- [Go 文档：包初始化顺序](https://go.dev/ref/spec#Package_initialization)

## 与主核 / AI 的关系

主核里最接近的是**系统目录（system catalog）**：一张声明式的表描述「有哪些对象类型、每种有哪些属性、属性什么类型」，而执行引擎按目录读出元数据后实例化真正的算子。这里的 `Canvas` 就是那份「声明」（节点 + 边 + 参数），`ComponentFactory` 就是「按名字实例化算子」的那一步。

**差别在于「目录是否可信」。** 主核的目录由 DDL 维护，所以「表存在 → 一定能实例化」是一个不变式。而这里的画布来自用户的手工编辑与历史版本迁移：一个 DSL 可以引用 Go 版没有实现的组件（`ErrNotImplemented`）、可以引用 v1 遗留的哨兵组件（`legacyNoOpNames`）、可以用 `function_name` 与 `component_name` 互相矛盾（f04 里那个「信任 component_name」的规则）。**所以工厂必须有降级路径，而主核的算子查找可以 fail-fast。** 这不是 ragflow 做得差，而是输入可信度不同带来的必然差异。

与 f02（DSL 归一）的关系是这一页的直接下游：`Canvas` 是「已经读进来的形状」，而 f02 讲的是「同一份 DSL 归一成两种形态（能保存的 / 能跑的）」。所以两页的边界是：这一页答「画布是什么」，f02 答「画布怎么被规范」。

与 f03（Agent 运行时）的关系更近：`Component` / `ComponentFactory` / `CanvasState` 这三个类型的**真正归属**都在 `internal/agent/runtime`，而 f03 正是讲那个包。所以读这一页时应当把它当成「类型在哪儿、为什么在那儿」的问题，而 f03 是「拿这些类型跑起来会发生什么」。

对 AI / agent 工程最实用的一条是**「组件工厂要能表达三种失败」**。这一层里对应三个不同的处理：组件不存在（`unsupported tool`，硬错）、组件存在但没移植（`ErrNotImplemented` 哨兵，且**构建器不拦截**、让它沿调用链失败）、参数校验不过（`ParamError`，可返回给用户）。多数 agent 框架只有一个「报错」，于是「用户 DSL 写错了」与「我们没实现」这两件事对用户是同一个消息——而它们的处置完全不同（改 DSL vs 等版本）。

另一条是那个 **per-pipeline 的工厂覆盖**。任何「一个进程服务多个租户/多个版本」的 agent 平台都会遇到「不同 pipeline 需要不同组件集合」的问题，而把工厂放在全局变量上就无法表达。把覆盖放进 `context` 是 Go 里的标准做法，代价是所有消费者必须拿着 ctx——但这是必要的，因为**工厂的选择是 per-run 的事实而不是 per-process 的事实**。

最后一条是 `legacyNoOpNames` 揭示的迁移形态：Python 版用「特殊节点」表达的控制流（ExitLoop），在 Go 版里被改成「谓词」。这是**从控制流图到求值函数**的转变，而它带来的连带效果是「遗留 DSL 里的 ExitLoop 节点必须能编译通过但不参与控制」。任何做语言/运行时迁移的项目都会碰到这一类：旧语法里的「有行为的关键字」在新实现里变成「无害的占位符」。正确做法是保留它能解析、明确它无行为、并写进注释——而不是删掉它导致旧数据整体编译失败。
