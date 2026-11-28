---
project: ragflow
feature: workflowx
title: 循环与并行结构
page_type: feature
schema_version: 1
refs:
  - path: internal/agent/workflowx/loop.go
    symbols: [WithLoopMaxIterations, WithLoopLifecycleHooks]
  - path: internal/agent/workflowx/parallel.go
    symbols: [WithParallelMaxConcurrency]
---

# 循环与并行结构

## 一句话职责

eino 的 `compose.Workflow` 只有节点与边，这两条「控制结构」是原生没有的——`loop.go`（1058 行）把「跑同一个子图 N 次直到谓词说停」做成一个节点，`parallel.go`（834 行）把「给定一批输入、每个跑一遍同一个子图、再收集」做成另一个节点；两者的复杂度几乎全在**中断与恢复**上，而不是在「循环」或「并行」本身。

## 机制是什么

两个文件的结构惊人地对称：一个 `XxxOption` 函数类型 + 一个 `xxxOptions` struct + 一组 `WithXxx...` 选项 + 一个 `defaultXxxCheckpointBuilder` + 一个 `XxxInterruptState` + 一个 bridge store。所以先看它们的选项表，就能看出这个子系统关心什么。

| | Loop | Parallel |
|---|---|---|
| 上限选项 | `WithLoopMaxIterations`（默认 **1024**，且「显式设的值是正常终止」） | `WithParallelMaxConcurrency`（默认 **0 = 顺序执行**） |
| 生命周期钩子 | `WithLoopLifecycleHooks(onStart, onFinish)` | 无（只有 `WithParallelContextBuilder`） |
| 检查点 ID | `WithLoopCheckpointIDBuilder` | `WithParallelCheckpointIDBuilder` |
| 子检查点开关 | `WithLoopEnableSubCheckpoint`（默认 **true**） | `WithParallelEnableSubCheckpoint`（默认 **true**） |
| 编译/运行选项 | `WithLoopCompileOptions` / `WithLoopRunOptions` | 同名两个 |
| 额外能力 | `WithLoopStream(mode)` / `WithLoopStatePersistence` | `WithParallelContextBuilder` |

两个上限的默认值差异很能说明问题：**Loop 的默认上限是 1024（一个「防跑飞」的兜底），而 Parallel 的默认并发是 0（顺序执行，不起 goroutine）**。前者是「不设上限会死循环」，后者是「不设并发就不并发」——两者的默认都选了「不引入新风险」那一侧。

而 `LoopStreamMode` 只有两个值，注释把它们的语义差异讲得很清楚：

```go
// LoopStreamFinalOnly buffers all iterations and exposes ONLY the
// final iteration's stream to the caller. This is the default and
// the safest mode because downstream consumers cannot observe
// intermediate iteration boundaries.
LoopStreamFinalOnly LoopStreamMode = "final_only"

// LoopStreamEveryIteration exposes each iteration's stream in
// sequence. Resume is iteration-granular: iterations already
// fully published are not replayed, while the interrupted
// iteration may be replayed from its start.
LoopStreamEveryIteration LoopStreamMode = "every_iteration"
```

所以「流式 + 循环」这个组合有两个答案：**缓冲全部只给最后一次**（默认，安全）或**逐次暴露**（可观察中间状态，但恢复只能到迭代粒度）。而后者那句「已完全发布的迭代不重放，而被打断的迭代可能从它的开头重放」是一条**精确的语义承诺**——它告诉调用方「你可能会看到一些重复」以及重复的边界在哪。

## 怎么实现的

**`LoopCondition` 的签名与语义被注释写成了一份完整契约。**

```go
type LoopCondition[T any] func(ctx context.Context, iteration int, prev, next T) (bool, error)
```

注释里逐参数说明，而其中三条最关键：

> `iteration`：刚完成的那个迭代的 **1-based 索引**。`iteration == 1` 在第一次调用时……
>
> `prev`：喂进刚完成那个迭代的值，作为子工作流的输入。在 iteration 1 这是外层循环节点的输入；在 iteration N>1 它是 iteration N-1 产生的 `next`（即上一轮输出，也是这一轮的输入）。
>
> `next`：子工作流为这一轮**产生**的值。若谓词返回 `(true, nil)`，这就是成为循环最终输出的值。

所以这个谓词是**在每轮跑完之后**被调用的，且同时看到「进来什么」与「出去什么」。这个设计让「判断该不该停」能基于本轮实际输出，而不只是轮数——这是它比 `maxIterations` 更强的部分。

而 `(true, nil)` / `(false, nil)` / 非 nil error 三个返回值的语义也被列清了。第三条：

> 返回非 nil error 会**让整个循环运行失败**。

**四个哨兵错误各自对应一种失败,而第一个的注释区分了「正常终止」与「安全兜底」。**

```go
ErrLoopMaxIterationsExceeded = errors.New("workflowx: loop max iterations exceeded")
ErrLoopSubGraphInterrupted  = errors.New("workflowx: sub-workflow interrupted")
ErrLoopResumeStateInvalid   = errors.New("workflowx: resume state invalid")
ErrLoopQuitConditionFailed  = errors.New("workflowx: quit condition failed")
```

第一个的注释值得完整引用：

> `ErrLoopMaxIterationsExceeded` 在**默认安全上限**到达而 `shouldQuit` 未返回 true 时返回。**显式的循环上限是正常终止**，与 canvas Python runtime 一致。

所以同一个「达到上限」有两种结局：

| 上限来源 | `maxIterationsSet` | 结局 |
|---|---|---|
| 调用方显式设的 | `true` | 返回最后一轮的 `next`，**无错误** |
| 默认 1024 兜底 | `false` | 返回 `ErrLoopMaxIterationsExceeded` |

而这在代码里就是三行：

```go
if iteration >= options.maxIterations {
    bridgeState.delete(subCheckID)
    if options.maxIterationsSet {
        return next, nil
    }
    return zero, fmt.Errorf("%w: %d", ErrLoopMaxIterationsExceeded, options.maxIterations)
}
```

这个区分是**语义上正确**的：如果用户说「最多跑 10 轮」，那跑到 10 轮就是一个正常的结果；如果没人说过上限而系统跑到 1024 轮，那说明退出条件有 bug。**同一个数值事实（到了上限）在两个不同来源下含义相反**，而用一个布尔（`maxIterationsSet`）承载这个区分。

`WithLoopMaxIterations` 的 docstring 还补了两个边界：

> `0` 保持默认安全上限有效。正数是正常循环终止……**`1` 是合法的，产出单次迭代的 do-while 情形**。

「`1` 是 do-while」这句话很精确：因为检查在**每轮跑完之后**，所以 `maxIterations = 1` 意味着「无条件跑一轮，然后停」——这恰好是 do-while 的语义。

**`WithLoopLifecycleHooks` 的 docstring 揭示了一个 eino 层的限制。**

```go
// WithLoopLifecycleHooks installs callbacks around the outer loop node's
// execution. Canvas uses this to emit node lifecycle events for the Loop macro
// without relying on eino state post-processing, which is unsafe for streamed
// multi-iteration output.
```

所以这个钩子存在的理由是：**eino 的状态后处理在「流式多迭代输出」这个情形下不安全**，所以画布需要别的手段来发 Loop 宏节点的「开始 / 结束」事件。

而在 `AddLoopNode` 里，钩子的调用位置是精心安排的：

```go
func(ctx context.Context, input T, _ ...struct{}) (T, error) {
    if options.onStart != nil {
        options.onStart(ctx, input)
    }
    out, err := runLoopInvoke(ctx, key, compiled, input, shouldQuit, options)
    if options.onFinish != nil {
        options.onFinish(ctx, err)
    }
    return out, err
},
```

**invoke 路径**：`onStart` 在循环开始前、`onFinish` 在循环返回后（**无论成功失败**，因为它没判 err 就调）。

而 **stream 路径**的 `onFinish` 被 `wrapLoopStreamFinish` 包起来了：

```go
reader, err := runLoopStream(ctx, key, compiled, input, shouldQuit, options)
if err != nil {
    if options.onFinish != nil {
        options.onFinish(ctx, err)
    }
    return nil, err
}
return wrapLoopStreamFinish(ctx, reader, options.onFinish), nil
```

所以流式路径的 `onFinish` 不能在 `runLoopStream` 返回时调——那时流还没读完。它必须等**流真正结束**（或出错）才调。这就是 `wrapLoopStreamFinish` 存在的理由：它把一个 `onFinish` 挂到 `StreamReader` 的生命周期末尾。

**这个模式值得单独记住**：流式 API 的「完成」与「返回」是两个事件。`runLoopStream` 返回的是一个 `*schema.StreamReader`，而「循环结束」发生在读者读到 EOF 或出错时。所以任何「结束时的回调」都必须绑到 reader 而不是绑到函数返回。

**`runLoopInvoke` 的恢复逻辑用「两个字段的联合判据」而不是一个布尔。**

```go
var current T
if snap.startIteration == 1 && snap.current == nil {
    current = input
} else {
    if err := json.Unmarshal(snap.current, &current); err != nil {
        return zero, fmt.Errorf("%w: decode current input: %w", ErrLoopResumeStateInvalid, err)
    }
}
```

`startIteration == 1 && snap.current == nil` 同时成立才走「全新运行」分支。所以：

- `startIteration == 1` 但 `current != nil` → 走恢复分支（解 JSON）。
- `startIteration > 1` 但 `current == nil` → 也走恢复分支，而 `json.Unmarshal(nil, ...)` 会失败 → 报 `ErrLoopResumeStateInvalid`。

这是一个**用联合条件表达「两个字段必须一致」**的写法。若只判 `startIteration == 1`，那么一份 `startIteration == 1` 但带了 `current` 的畸形快照会被当作全新运行、静默丢掉 `current`。而现在的判据让「部分恢复的状态」明确失败。

**每轮的输入被 JSON 序列化保存，而注释说明了为什么必须这样。**

```go
// Marshal the input we are about to feed the sub-workflow.
// We persist the JSON so a resume can re-feed the same
// input without re-running the previous iteration.
currentJSON, err := json.Marshal(current)
```

所以恢复时的起点是「上一轮喂进去的输入」，而不是「上一轮的输出」。这两者其实是同一个值（`current = next` 在循环末尾），但**保存它而不是重算它**保证了一件事：恢复不需要重跑任何已完成的迭代。若靠重算，那得从头跑到第 N 轮——那就是「恢复 = 重跑」，失去意义。

**中断路径的 `errors.Join` 用法讲清了「哨兵给代码、复合中断给框架」。**

```go
return zero, errors.Join(ErrLoopSubGraphInterrupted,
    compose.CompositeInterrupt(ctx, nil, state, runErr))
```

注释：

> `errors.Join` 保留哨兵（通过 `errors.Is`），而框架仍然看到复合中断错误。

所以这是一个**一个 error 值同时满足两个读者**的写法：`errors.Is(err, ErrLoopSubGraphInterrupted)` 让调用方能判断「这是循环层面被中断」，而 `compose.CompositeInterrupt` 的类型断言让 eino 能识别「这是需要恢复的中断」并序列化 `state`。

这与 f03 里 `DeferredStreamError` 的「一个 error 两个受众」是同一条原则，只是手段不同（那里用 `Unwrap` 链，这里用 `errors.Join` 的多值链）。

**`bridgeState` 是「子工作流检查点」的进程内中转，而它的删除时机被放在三个位置。**

```go
if quit {
    bridgeState.delete(subCheckID)
    return next, nil
}
if iteration >= options.maxIterations {
    bridgeState.delete(subCheckID)
    ...
}
bridgeState.delete(subCheckID)
current = next
```

三条路径都删：**正常退出、达到上限、继续下一轮**。所以「每轮结束时清掉这一轮的子检查点」——因为它已经用完了。若不删，`bridgeState.snapshot()` 会带上所有历史轮次的检查点，而那个快照会被写进中断状态里。

这是一个「中转存储的生命周期必须与它服务的那一轮对齐」的实例，而**三处删除**保证了没有任何一条路径漏掉。这类「每条退出路径都要清理」的模式很容易漏掉某一处，而漏掉的后果不是崩溃而是**快照无限增长**——只在长循环 + 多次中断的场景下暴露。

**`defaultCheckpointBuilder` 的默认值带 UUID，而注释说明了它为什么不适合恢复。**

```go
func defaultCheckpointBuilder(nodeKey string, iteration int) string {
    return fmt.Sprintf("workflowx-cp:%s:%d:%s", nodeKey, iteration, uuid.NewString())
}
```

注释：

> 默认返回一个基于 UUID 的子检查点 ID。它**故意是非确定性的**，好让没有配置 `WithLoopCheckpointIDBuilder` 的调用方在每次迭代都得到一个新的子检查点，那是对不需要跨运行恢复的调用而言的安全默认。

而 `WithLoopCheckpointIDBuilder` 的 docstring 把代价讲明了：

> 若没提供 builder，会用一个保留命名空间的默认值，它把循环节点 key 与迭代号和一个 UUID 组合。**默认值对临时调用没问题，但不保证可重入恢复**：一次恢复的运行会推导出一个新的 UUID，而子工作流找不到被打断那次的部分状态。**生产调用方需要检查点/恢复时必须提供一个跨调用返回稳定 ID 的 builder**（例如 `<parent-id>:<nodeKey>:<iteration>`）。

所以这是「默认值选安全但不完整」的典型：UUID 保证**不会碰撞**（多次运行不互相污染），代价是**不能恢复**。而需要恢复的人必须显式提供 builder。

**而 `parallel.go` 的默认 builder 恰好相反——它不需要 UUID。**

```go
func defaultParallelCheckpointBuilder(nodeKey string, index int) string {
    return fmt.Sprintf("workflowx-parallel:%s:%d", nodeKey, index)
}
```

注释解释了为什么：

> 不像 loop 扩展，parallel 扩展**默认不需要 UUID**，因为同一个 item 索引在恢复时天然可以从持久化的 `InterruptedIndices` 重新推导——所以同一个 ID 会被重用。

所以两者的差异来自**恢复时能否重新推导出那个键**：

| | 恢复时的键来源 | 默认是否稳定 |
|---|---|---|
| Loop | 迭代号从快照读，但「这是第几次中断」无天然顺序 | 需要 UUID（除非显式给 builder） |
| Parallel | item index 从 `InterruptedIndices` 直接读出 | 天然稳定，不需要 UUID |

这个对比很好：**同一个「默认值是否稳定」的问题，答案是数据结构的性质决定的，不是风格偏好。** Parallel 的输入是「一批有索引的 item」，所以索引就是天然的身份；Loop 的迭代是「无限延伸的序列」，所以需要一个额外的实例标识。

**`WithParallelMaxConcurrency` 的 `n <= 1` 与 `n > 1` 是两个完全不同的执行路径。**

```go
// n <= 1 — sequential execution on the calling goroutine (no
//         goroutines are spawned for any input length).
// n > 1  — up to n workers invoke items concurrently.
//
// The default is 0 (sequential).
```

注意第三条：**`n <= 1` 时连一个 goroutine 都不起**。所以 `maxConcurrency = 1` 与 `maxConcurrency = 0` 在行为上相同（都是顺序），而 `= 0` 只是「没设」的默认值表达方式。

而 `n >= 0` 的守卫让负值被静默忽略（保留原值）：

```go
if n >= 0 {
    o.maxConcurrency = n
}
```

所以 `WithParallelMaxConcurrency(-5)` 是一个 no-op——不报错，也不生效。这是一个宽容的选择，与 f05 里 `ValidateConfig` 的严格拒收形成对比；差别在于这里是**库级选项**（错误处理靠约定），那里是**用户配置**（错误必须可见）。

**`ParallelInterruptState` 有一个关键的适配解释。**

> 这个 struct 镜像参考 batch 节点的 `NodeInterruptState`，有一处重要适配：**`OriginalInputs` 被存成 JSON 字节切片（不是 `[]any`）**，好让 parallel 扩展在恢复时用原始 Go 类型重新解码。**JSON 的默认行为会把数字解成 `float64`，否则会破坏整数与其他有类型的输入。**

所以这是一个「序列化格式的语义陷阱」：`json.Unmarshal` 到 `any` 会把所有数字变成 `float64`。若类型是 `[]any` 而里面本该有 `int`，恢复后就变成 `float64` 了——而下游可能用类型断言 `v.(int)` 从而失败（或者更糟，静默地按 `float64` 处理导致精度或格式化差异）。

解法是存 JSON 字节而不解：恢复时用 `json.Unmarshal(jsonBytes, &typedSlice)` 让 Go 按目标类型解码。

这条注释的价值在于它记录了**一个具体的、常见的、会导致难以归因故障的陷阱**。

**`contextBuilder` 的错误语义是「跳过这一项」而不是「让整批失败」。**

```go
// WithParallelContextBuilder decorates the per-item sub-workflow
// context before Invoke. A builder error skips invoking that item.
```

所以 `WithParallelContextBuilder` 返回 error 时，那一项被跳过（不调用子工作流），其余项继续。这是一个「部分失败」的策略，而它与 `maxConcurrency` 的「全部成功或全部失败」不同。

而 `WithParallelCompileOptions` 的 docstring 有一条关于顺序的说明：

> parallel 扩展**总是先传它自己的 bridge `CheckPointStore`**（当子检查点启用时），所以任何通过这个选项设置的 store **不会与 bridge store 冲突**。

「先传」的意义在 eino 的选项模型里：后传的覆盖先传的。所以把 bridge store 放在最前面，保证了用户传的 store 会覆盖它——即「用户的会赢」。这与「让库的实现不被意外覆盖」的直觉相反，但注释说的是「不会冲突」，暗示用户的 store 确实会赢而 bridge 只在用户没给时生效。

**两个扩展都必须「先编译子工作流」。**

```go
// Compile the sub-workflow up front. Surface compile-time
// failures directly so the caller never sees a half-built outer
// workflow.
compiled, err := sub.Compile(ctx, compileOpts...)
if err != nil {
    return nil, fmt.Errorf("workflowx: compile sub workflow %q: %w", key, err)
}
```

注释的理由很好：**不让调用方看到一个半构造的外层工作流**。而注释里还有一段关于 `AddLoopNode` 三个前置检查的（`wf` / `sub` / `shouldQuit` 都 nil 检查）——三者都在最前面，所以「构造一个循环节点」在返回之前要么成功要么什么都没变。

而 `compileOpts` 的拼接方式：

```go
compileOpts := append([]compose.GraphCompileOption{}, options.compileOpts...)
if options.enableSubCheckpoint {
    compileOpts = append(compileOpts, compose.WithCheckPointStore(newLoopBridgeStore()))
}
```

先复制一份（避免 `append` 写到调用方切片底层数组），再追加 bridge store。**复制**这一步与 f04 里 `append([]string(nil), ...)` 同理。

**`AddLoopNode` 用 `compose.AnyLambda[T, T, struct{}]` 而选项类型是 `struct{}`。**

```go
lambda, err := compose.AnyLambda[T, T, struct{}](
    func(ctx context.Context, input T, _ ...struct{}) (T, error) { ... },
    func(ctx context.Context, input T, _ ...struct{}) (*schema.StreamReader[T], error) { ... },
    nil,
    nil,
)
```

注释：「循环节点不需要 per-call lambda 选项；eino 在运行时传零值 `struct{}` 选项」。所以 `struct{}` 是一个「我不用选项」的占位——比定义一堆空接口更精确。

而两个 `nil` 是其余两个 lambda 变体（可能是不流式但带选项的、流式带选项的），因为这里只实现了「不流式不带选项」与「流式不带选项」两个。

## 关键取舍

**源码事实：** Loop 的默认上限是 **1024**（`defaultMaxIterations`），而 `WithLoopMaxIterations` 显式设的正值意味着**正常终止**；两者的区分由 `maxIterationsSet` 布尔承载——到上限时显式设过就返回最后一轮的 `next` 无错误，用默认兜底就返回 `ErrLoopMaxIterationsExceeded`。

**源码事实：** `WithLoopMaxIterations(1)` 是合法的，产出「单次迭代的 do-while」——因为检查在每轮跑完之后。

**源码事实：** `WithLoopLifecycleHooks` 存在的理由写在 docstring 里：「画布用它来发 Loop 宏节点的生命周期事件，而不依赖 eino 的状态后处理，那对流式多迭代输出不安全」。

**源码事实：** 流式路径的 `onFinish` **不能**在 `runLoopStream` 返回时调（那时流还没读完），必须经 `wrapLoopStreamFinish` 绑到 reader 的生命周期末尾；而 invoke 路径的 `onFinish` 无条件调用（成功失败都调）。

**源码事实：** `runLoopInvoke` 用 `snap.startIteration == 1 && snap.current == nil` **两个条件的合取**判「全新运行」，所以「`startIteration == 1` 但带 `current`」与「`startIteration > 1` 但无 `current`」都会走恢复分支（后者会报 `ErrLoopResumeStateInvalid`）。

**源码事实：** 每轮的输入被 `json.Marshal` 保存，注释说明「恢复能重喂同一个输入而不重跑上一轮」——保存而非重算，是「恢复不等于重跑」的前提。

**源码事实：** 中断路径用 `errors.Join(ErrLoopSubGraphInterrupted, compose.CompositeInterrupt(...))`——哨兵给 `errors.Is`，复合中断给框架。

**源码事实：** `bridgeState.delete(subCheckID)` 出现在**三条**路径（正常退出 / 达上限 / 继续下一轮），所以每轮结束都清掉该轮子检查点，避免快照随轮次无限增长。

**源码事实：** Loop 的默认检查点 builder 带 `uuid.NewString()`（故意非确定性），代价是「不保证可重入恢复」；Parallel 的默认 builder 不带 UUID，因为「同一个 item 索引在恢复时天然可从持久化的 `InterruptedIndices` 重新推导」。

**源码事实：** `WithParallelMaxConcurrency` 的 `n <= 1` 是「在调用方 goroutine 上顺序执行，**任何输入长度都不起 goroutine**」，`n > 1` 才是并发；默认 0（顺序）；`n >= 0` 的守卫让负值静默 no-op。

**源码事实：** `ParallelInterruptState` 的 `OriginalInputs` 存成 **JSON 字节**而不是 `[]any`，因为「JSON 默认会把数字解成 `float64`，否则会破坏整数与其他有类型的输入」。

**源码事实：** `WithParallelContextBuilder` 的错误语义是**跳过那一项**而不是让整批失败。

**源码事实：** `WithParallelCompileOptions` 的 store「不会与 bridge store 冲突」，因为扩展**总是先传** bridge store。

**源码事实：** 两个 `Add*Node` 都**先编译子工作流**并直接暴露编译失败，理由是「不让调用方看到一个半构造的外层工作流」；`compileOpts` 拼接前先复制切片（避免 `append` 写到调用方底层数组）。

**源码事实：** `LoopStreamEveryIteration` 的恢复是**迭代粒度**：「已完全发布的迭代不重放，而被打断的迭代可能从它的开头重放」。

**架构推论：** 这两个扩展 90% 的复杂度在**中断/恢复**，而不是循环或并行本身。`for` 循环与 `errgroup` 各三行就能写，而「跑到一半被打断、把状态序列化、几小时后从断点续」需要：每轮检查点 ID、每轮输入持久化、子检查点中转、复合中断包装、恢复时的状态校验。**这说明「可中断」是一个远比「能执行」更贵的能力**——而 e04 那页讲的 `Durability` 三档与这里的中断机制是同一个需求的两种粒度（超步级 vs 节点内迭代级）。

**架构推论：** 两个默认 builder 的差异（UUID vs 无 UUID）揭示了「默认值必须由数据结构的性质决定」。Parallel 的输入有天然索引，所以索引即身份，默认稳定；Loop 的迭代没有天然实例标识（同一节点第 3 轮在不同运行里不是同一个东西），所以需要 UUID 来保证不碰撞。**判据是：恢复时能否从持久化状态里重新推导出这个键？** 能则默认稳定，不能则必须加 UUID 并把「要恢复就显式给 builder」写进文档。

**架构推论：** `maxIterationsSet` 那个布尔是「同一数值、两种语义」的标准解法。这里「跑到第 1024 轮」在「用户说的 1024」下是成功、在「系统兜底的 1024」下是失败——**区分它们必须靠来源而不是靠值**。这与 i05 里 `max_request_body_bytes_explicit` 的论证结构完全一致（那里是「设成默认值」vs「没设」不可区分，所以必须有显式标志）。凡是「默认值同时可能被用户显式选择」的配置，都需要一个这样的旁路布尔。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agent/workflowx/loop.go` | `LoopCondition` | 每轮后的退出谓词，签名与语义有完整契约注释 |
| `internal/agent/workflowx/loop.go` | `LoopStreamMode` | `final_only`（默认）与 `every_iteration` |
| `internal/agent/workflowx/loop.go` | `defaultMaxIterations` | 1024，仅作防跑飞兜底 |
| `internal/agent/workflowx/loop.go` | `WithLoopMaxIterations` | 显式上限 = 正常终止；`1` 是 do-while |
| `internal/agent/workflowx/loop.go` | `WithLoopLifecycleHooks` | 绕开 eino 状态后处理发节点事件 |
| `internal/agent/workflowx/loop.go` | `WithLoopStatePersistence` | 调用方拥有的跨中断状态 |
| `internal/agent/workflowx/loop.go` | `WithLoopCheckpointIDBuilder` | 生产环境恢复所必需 |
| `internal/agent/workflowx/loop.go` | `defaultCheckpointBuilder` | 带 UUID，故意非确定性 |
| `internal/agent/workflowx/loop.go` | `ErrLoopMaxIterationsExceeded` | 默认兜底触顶时的错误 |
| `internal/agent/workflowx/loop.go` | `AddLoopNode` | 先编译子图，再装 lambda 与钩子 |
| `internal/agent/workflowx/loop.go` | `runLoopInvoke` | 恢复判据、每轮 JSON 输入、中断包装 |
| `internal/agent/workflowx/loop.go` | `wrapLoopStreamFinish` | 把 `onFinish` 绑到 reader 生命周期 |
| `internal/agent/workflowx/loop.go` | `loopBridgeState` | 子检查点进程内中转，三处删除 |
| `internal/agent/workflowx/parallel.go` | `WithParallelMaxConcurrency` | `n<=1` 完全不起 goroutine，默认 0 |
| `internal/agent/workflowx/parallel.go` | `WithParallelCheckpointIDBuilder` | 空返回视为「跳过 per-item 检查点 ID」 |
| `internal/agent/workflowx/parallel.go` | `defaultParallelCheckpointBuilder` | 无 UUID，索引即身份 |
| `internal/agent/workflowx/parallel.go` | `ParallelInterruptState` | `OriginalInputs` 存 JSON 字节防 float64 |
| `internal/agent/workflowx/parallel.go` | `WithParallelContextBuilder` | 错误 = 跳过该项 |
| `internal/agent/workflowx/parallel.go` | `ErrParallelResumeStateInvalid` | 恢复状态缺失/畸形/Inputs 为空 |
| `internal/agent/workflowx/parallel.go` | `runParallelFanout` | 并发扇出主体 |
| `internal/agent/workflowx/parallel.go` | `buildPendingIndices` | 从已完成集合推导待跑索引 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agent/workflowx/loop.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/workflowx/loop.go)
- [ragflow 源码：agent/workflowx/parallel.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/workflowx/parallel.go)
- [eino 文档：compose.Workflow](https://www.cloudwego.io/docs/eino/core_modules/chain_and_graph_orchestration/)

## 与主核 / AI 的关系

主核里最接近的是**游标与批量处理**：一个 `FOR` 循环跑一批行、每批之间提交、崩溃后能从「上次提交的位置」继续，而不是从头。这里的 `loop.go` 是同一件事在 agent 图上的形态：每轮一个检查点、恢复从断点起、`bridgeState` 就是那个「当前批的位置」。

**差别在于「什么可以作为断点」。** 数据库的断点是**数据位置**（行的主键或 LSN），天然可比、天然有序、天然持久。而这里循环的断点是「第几轮 + 那一轮的输入 JSON」——轮次是逻辑序号，输入是序列化快照。所以数据库可以只存一个 LSN 就恢复，而这里必须存「轮次 + 输入 + 子检查点集合」，且还得处理「这个轮次号在另一次运行里是不是同一个东西」（就是 UUID 那个问题）。

与 e04（Pregel 引擎）的关系是最直接的对照：那边是**超步级**的检查点（`Durability` 三档决定每个超步落不落盘），这边是**节点内迭代级**的检查点（每轮一个 `subCheckID`）。所以粒度更细、成本更高、且需要一套 bridge 机制把子检查点从 eino 的内部存储里中转出来。两页合起来构成 ragflow 的「可中断执行」全貌：引擎管超步，workflowx 管超步内部的循环/并行。

与 f03（Agent 运行时）的关系：`WithLoopLifecycleHooks` 发的节点事件，正是 f03 里 `AgentEvent` / `AgentMessageEmitter` 那套要消费的东西；而 `WithDeferredNodeRegistry` 的「延迟 node_finished」与这里的 `wrapLoopStreamFinish` 是同一问题的两个粒度——前者延迟的是 Agent 节点的完成事件（等下游 Message 消费完），后者延迟的是 Loop 节点的完成回调（等流读完）。

与 f01（画布）的关系：`legacyNoOpNames` 里那条「Loop 的终止由 `loop_termination_condition` 谓词驱动，而不是靠走到 ExitLoop 节点」讲的正是这一页的 `LoopCondition`。所以 f01 说「Python 的控制流节点在 Go 里被改成了谓词」，这一页说「那个谓词长什么样、什么时候被调」。

对 AI / agent 工程最实用的一条是**「显式上限 = 正常终止，默认兜底 = 错误」**。这个区分几乎每个 agent 框架都会做错：把所有「达到上限」都当成错误，于是用户配的 `max_iterations=10` 在第 10 轮报错；或者都当成正常，于是真的死循环时静默返回半成品。正确做法是用一个`是否显式设置过`的旁路布尔区分来源，而这在配置系统里是很便宜的一件事。

另一条是 `ParallelInterruptState` 那个「存 JSON 字节而不是 `[]any`」。`json.Unmarshal` 到 `any` 会把数字变 `float64`，而任何需要持久化再恢复的结构化数据都会踩这个坑。判据是：**恢复时我需要原始 Go 类型吗？** 需要的话就存字节、用目标类型解码，绝不经过 `any`。

最后一条是流式 API 的「完成 ≠ 返回」。`runLoopStream` 返回 reader 时循环还在跑，所以任何「结束时」的逻辑必须绑到 reader（`wrapLoopStreamFinish`）。这类错误的表现是「回调触发得太早」，而它通常不会让测试失败——只会在真实流式输出下让事件顺序错乱。
