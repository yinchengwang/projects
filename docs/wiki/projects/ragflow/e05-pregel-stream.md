---
project: ragflow
feature: pregel-stream
title: 流式事件与消息聚合
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/graph/pregel/stream.go
    symbols: [StreamManager, NewStreamManager, StreamEvent]
  - path: internal/harness/graph/pregel/messages.go
    symbols: [StreamMessagesHandler, MessageAggregator]
---

# 流式事件与消息聚合

## 一句话职责

两条独立的流：一条把**图的执行事件**（检查点、任务起止、状态更新、中断、错误、收尾）按 `StreamMode` 过滤后投递，另一条把**LLM 的 token 增量**按节点聚合成完整消息再转发 —— 而两者都用「满了就丢」的策略，**唯一保证送达的是那个最终事件**。

## 机制是什么

`StreamMode` 有七个值（e01 那页列过），而它们**不是同一种东西**，这一点是本页的起点：

| `StreamMode` | 谁能产出它 |
|---|---|
| `values` / `updates` / `tasks` / `checkpoints` / `debug` | **`StreamManager` 的九个事件类型** |
| `custom` | 节点内的 `StreamWriter` |
| `messages` | **`StreamMessagesHandler` 的 token 增量** |

**三种产出来源，而 `StreamManager` 只负责中间那五个。** 于是「流」这件事在这个包里是**三个并行机制**：

1. **`StreamManager`**：`chan *StreamEvent` + 一个 `includeFilter` 位图，按 mode 筛掉不需要的事件类型。
2. **`StreamMessagesHandler`** → **`MessageAggregator`** → **`MessageEmitter`**：把碎片合成消息，再由 emitter 投递。
3. **`types.ChannelStream`**：一个独立的 `StreamProtocol` 实现，`chan *StreamChunk` + 迭代器。

**而三者各自有自己的 `StreamMode` 处理方式，且它们互不知晓对方。**

| | `StreamManager` | `StreamMessagesHandler` | `ChannelStream` |
|---|---|---|---|
| 载体 | `chan *StreamEvent` | `chan` 由 emitter 决定 | `chan *StreamChunk` |
| 满时行为 | **丢** | 无缓冲概念 | **阻塞（带 `ctx.Done()` 出口）** |
| 关闭后写入 | 静默返回 | 返回错误 | 返回 `StreamError` |
| 谁消费 | 调用方直接读 `Events()` | `MessageEmitter` 回调 | `StreamIterator.Next` |

**「满时行为」这一栏的差异是最要紧的：`StreamManager` 丢事件，而 `ChannelStream` 阻塞。** 这两个选择各自有理由，而它们意味着**同一个系统里「背压」这个问题的答案有两个。**

## 怎么实现的

### 一、`StreamEvent`：九个类型，而 `Data` 是一个无类型的 map

```
type StreamEvent struct {
	Type      StreamEventType
	Timestamp time.Time
	Step      int
	Node      string
	TaskID    string
	Data      any
	Error     error
}
```

**七个字段，而前四个之外的三个都是「部分事件才有」的：**

| 字段 | 哪些事件填 |
|---|---|
| `Type` / `Timestamp` / `Step` | 全部 |
| `Node` | `TaskStart` / `TaskEnd` / `Update` / `Error` |
| `TaskID` | `TaskStart` / `TaskEnd` |
| `Error` | `TaskEnd`（可空）/ `Error` |
| `Data` | 全部（但每个事件类型的键都不同） |

**而九个事件类型的 `Data` 键是这样分布的：**

| 事件 | `Data` 里的键 |
|---|---|
| `checkpoint` | `checkpoint` |
| `task_start` | `node`、`task_id` |
| `task_end` | `node`、`task_id`、`output`、`duration`（**字符串**） |
| `update` | `node`、`output` |
| `values` | `values` |
| `interrupt` | `interrupts` |
| `error` | `node`、`error`（**字符串**） |
| `final` | `state` |
| `debug` | `message`、`data` |

**三个值得记的细节：**

**其一，`task_end` 的 `duration` 是 `duration.String()` —— 一个字符串而不是时长。**

```
event.Data = map[string]any{
	"node": node, "task_id": taskID, "output": output, "duration": duration.String(),
}
```

**这意味着消费方要比较耗时就必须重新解析字符串。** 而 `StreamEvent` 本身是 Go 结构体（不是 JSON），所以这个「先转字符串」的决定是**在构造事件时就把类型信息扔掉了** —— 大概是为了「消费者可能是 JSON 序列化路径」而做的统一，但代价是进程内消费者也拿到了字符串。

**其二，`error` 事件里有两处错误：**

```
func (sm *StreamManager) EmitError(step int, err error, node string) {
	...
	event.Error = err
	event.Data = map[string]any{ "node": node, "error": err.Error() }
}
```

**`Error` 字段是 `error` 接口，`Data["error"]` 是字符串。** 于是「同一个信息存在两处、且类型不同」 —— **结构化消费用 `Error`、序列化消费用 `Data`。** 这是一个为「两种消费方式都支持」付的代价。

**其三，`task_end` 的 `Error` 字段可以非空而且事件类型不是 `error`：**

```
func (sm *StreamManager) EmitTaskEnd(step int, node string, taskID string, output any, duration time.Duration, err error)
```

**于是「任务失败」有**两种**表达：一条带 `Error` 的 `task_end`，或者一条 `error` 事件。** 而 `shouldEmit(EventTypeError)` 的位图在 `tasks` 模式下是**开着的**：

```
case types.StreamModeTasks:
	sm.includeFilter[EventTypeTaskStart] = true
	sm.includeFilter[EventTypeTaskEnd] = true
	sm.includeFilter[EventTypeError] = true
	sm.includeFilter[EventTypeFinal] = true
```

**「`tasks` 模式包含 `error`」** —— 于是消费方在 `tasks` 模式下收到两类失败信号，且必须都处理。

### 二、`setupIncludeFilter`：五个 case + 一个什么都兜的 `default`

```
switch sm.mode {
case types.StreamModeValues:
	sm.includeFilter[EventTypeValues] = true
	sm.includeFilter[EventTypeFinal] = true

case types.StreamModeUpdates:
	sm.includeFilter[EventTypeUpdate] = true
	sm.includeFilter[EventTypeFinal] = true

case types.StreamModeTasks:
	sm.includeFilter[EventTypeTaskStart] = true
	sm.includeFilter[EventTypeTaskEnd] = true
	sm.includeFilter[EventTypeError] = true
	sm.includeFilter[EventTypeFinal] = true

case types.StreamModeCheckpoints:
	sm.includeFilter[EventTypeCheckpoint] = true
	sm.includeFilter[EventTypeFinal] = true

case types.StreamModeDebug:
	// Include all events in debug mode
	sm.includeFilter[EventTypeCheckpoint] = true
	sm.includeFilter[EventTypeTaskStart] = true
	sm.includeFilter[EventTypeTaskEnd] = true
	sm.includeFilter[EventTypeUpdate] = true
	sm.includeFilter[EventTypeValues] = true
	sm.includeFilter[EventTypeInterrupt] = true
	sm.includeFilter[EventTypeError] = true
	sm.includeFilter[EventTypeDebug] = true
	sm.includeFilter[EventTypeFinal] = true

default:
	// Default to values mode
	sm.includeFilter[EventTypeValues] = true
	sm.includeFilter[EventTypeFinal] = true
}
```

**三个要点：**

**其一，`final` 在**所有**分支里都被置位。** 这是「最终状态一定有」的保证 —— 而它与下面的 `EmitFinal` 的阻塞语义配套（见下一节）。

**其二，`interrupt` 只出现在 `debug` 分支里。**

**这是本页最值得记的一条事实：在 `values` / `updates` / `tasks` / `checkpoints` 四种模式下，`interrupt` 事件**永远不会发出**。**

**而 `EmitInterrupt` 存在且被调用（e04 那页说过「命中中断就先存检查点、发射中断事件」）——** 于是「发了但被过滤」这条路径上，调用方完全不会知道发生了中断。** 调用方只能通过 `values` 事件里的状态、或最终返回的错误来判断。

**其三，`custom` 与 `messages` **没有 case**，于是落到 `default`，得到的位图是「`values` + `final`」。**

**这个后果很具体：** `StreamWriter.Write` 的实现是：

```
func (w *StreamWriter) Write(data any) error {
	w.streamManager.EmitDebug(w.step, fmt.Sprintf("custom output from node %s", w.node), data)
	return nil
}
```

**它发的是 `EventTypeDebug`。** 而在 `custom` 模式下 `includeFilter` 只有 `values` 与 `final` —— **于是 `shouldEmit(EventTypeDebug)` 返回 `false`，节点的自定义输出被静默丢弃。**

**「函数有实现、路径通、但不生效」** —— 这是本页最严重的源码事实，而它不报错、不打日志。**`Write` 还返回了 `nil`。**

**而 `StreamModeDebug` 下 `StreamWriter` 是能工作的**（`debug` 位图里 `EventTypeDebug` 是开的）。**于是「想让 `StreamWriter` 生效，必须用 `debug` 模式」** —— 而 `debug` 模式会把所有其它事件也一起发出来。

**这说明三个机制之间的接口没有对齐：`StreamModeCustom` 是 `types` 包定义的一个合法模式，而 `StreamManager` 没有给它任何位图。**

### 三、`emit` 丢事件，`EmitFinal` 阻塞 —— 两个投递语义

**八个 `Emit*` 方法都走同一个私有 `emit`：**

```
func (sm *StreamManager) emit(event *StreamEvent) {
	sm.mu.RLock()
	defer sm.mu.RUnlock()

	if sm.mu.closed { return }

	select {
	case sm.eventCh <- event:
		// Event sent
	default:
		// Channel full, drop event
	}
}
```

**`select` 带 `default` = 「塞不进去就丢」。** 注释就三个字「Channel full, drop event」，没有别的解释。

**而 `EmitFinal` 是唯一例外的：**

```
// EmitFinal emits final event with complete state.
// Uses blocking channel send to guarantee delivery — dropping the final event
// would cause RunSync to return nil state (silent data loss).
func (sm *StreamManager) EmitFinal(step int, state any) {
	if !sm.shouldEmit(EventTypeFinal) { return }
	event := NewStreamEvent(EventTypeFinal, step)
	event.Data = map[string]any{ "state": state }
	sm.eventCh <- event
}
```

**注释把理由讲清了：「丢最终事件会让 `RunSync` 返回 nil 状态（静默数据丢失）」。**

**于是两条投递语义在同一个结构里共存：**

| | 中间事件（八个） | 最终事件 |
|---|---|---|
| 语义 | **at-most-once（满了就丢）** | **阻塞直到送达** |
| 理由 | 流式事件的用途是「进度」「观察」，丢一帧可接受 | 「最终状态」是返回值本体，丢了等于运行失败 |
| 风险 | 消费方看到的序列有洞 | **消费方若不读，发送方永久阻塞** |

**「若消费方不读则永久阻塞」这个风险是真实的**：`EmitFinal` **没有 `ctx.Done()` 分支**，也没有超时。**而 e04 那页提到 `Run` 的收尾 `defer` 顺序** —— 若转发 goroutine 因为 `outputCh` 无人读而卡住，`EmitFinal` 会让整个收尾卡死。

**而这正是 e04 那页说的「`Run` 的文档要求调用方读到 `outputCh` 关闭为止，否则泄漏 goroutine」的另一半** —— **不只是泄漏 goroutine，还可能卡在 `EmitFinal` 那一步。**

**还有一个更细的问题：`EmitFinal` **没有拿 `sm.mu.RLock()`**。**

**八个 `emit` 路径都在 RLock 下检查 `closed` 并发送，而 `Close` 在 Lock 下 `close(sm.eventCh)`。** 这个搭配是安全的（`emit` 持 RLock 期间 `Close` 无法拿到 Lock）。

**但 `EmitFinal` 裸发 —— 于是「`Close` 之后 `EmitFinal`」会 `send on closed channel` 并**panic**。**

**这是一个真实的竞态缺陷**，而它恰好出现在「保证送达」的那条路径上。**讽刺的是它保证了「送达」但没保证「通道还开着」。**

**而更实际的场景是：`Close` 会先 `close(eventCh)` 再 `close(errorCh)`；若某个 goroutine 恰好在 `EmitFinal` 的裸发上，就会崩。**

### 四、`Close` 与 `shouldEmit` 的锁结构

```
func (sm *StreamManager) shouldEmit(eventType StreamEventType) bool {
	sm.mu.RLock()
	defer sm.mu.RUnlock()
	return sm.includeFilter[eventType]
}

func (sm *StreamManager) Close() {
	sm.mu.Lock()
	defer sm.mu.Unlock()
	if !sm.mu.closed {
		sm.mu.closed = true
		close(sm.eventCh)
		close(sm.errorCh)
	}
}
```

**`sm.mu` 是一个匿名内嵌结构：**

```
mu struct {
	sync.RWMutex
	closed bool
}
```

**「匿名结构里嵌 `sync.RWMutex` 再加一个 `closed` 布尔」** —— 于是「判关闭」与「保护通道」用的是同一把锁。**这是必需的**，因为「检查 closed 再发送」必须是原子的（否则在检查与发送之间有窗口）。

**而 `Close` 是**幂等**的（`if !sm.mu.closed`）—— 重复调用不会二次 `close` 而 panic。** 这一点做对了。

**而 `includeFilter` 在 `setupIncludeFilter` 里被写、在 `shouldEmit` 里被读，**中间没有锁保护**。**

**`NewStreamManager` 里 `setupIncludeFilter()` 在返回前被调用一次 —— 于是「构造完成时已冻结，之后只读」**，这个假设成立。**但类型上它是一张普通 `map`，任何后续写入都会与 `shouldEmit` 的读构成数据竞争。** 而代码里没有「冻结」的表达（没有把 map 换成数组/位图，也没有把写入收进构造函数）。

**`includeFilter` 用 `map[StreamEventType]bool` 而不是位掩码** —— 对九个类型来说 map 是可读性更好的选择，代价是每次 `shouldEmit` 一次哈希。**这在每个事件一次的量级上无关紧要。**

### 五、`bufferSize` 与 `errorCh` 的固定容量

```
func NewStreamManager(mode types.StreamMode, bufferSize int) *StreamManager {
	if bufferSize <= 0 { bufferSize = 100 }

	sm := &StreamManager{
		mode:          mode,
		eventCh:       make(chan *StreamEvent, bufferSize),
		errorCh:       make(chan error, 10),
		bufferSize:    bufferSize,
		includeFilter: make(map[StreamEventType]bool),
	}
	sm.setupIncludeFilter()
	return sm
}
```

**两件事：**

**其一，`bufferSize` 是**可配**的（默认 100），而 `errorCh` 是**硬编码 10**。**

**「事件缓冲可配、错误缓冲固定」** —— 而 `errorCh` 在整个文件里几乎没有被写过（`Errors()` 只是把通道暴露出去）。**它大概是为「错误也是一种流事件」的旧设计留的，而现在错误走 `EventTypeError` 的 `emit` 路径。**

**其二，`bufferSize` 被存成字段但**从未被读过**。**

**`100` 这个容量与「满了就丢」组合起来，语义是：**「最多积压 100 个事件，超出的直接丢」。** 一个慢消费者在 `debug` 模式（九个类型全开）下会丢得最多 —— 而 `debug` 恰是**最需要完整序列**的模式。

**「缓冲 × 丢弃」这个组合的可用性取决于「消费方多快」** —— 而由于 `Close` 时不再有任何补偿机制（没有「排空」逻辑），**关闭时缓冲里剩余的事件会被 `close` 之后被消费方按正常顺序读到**（Go 的 `range` 语义：关闭后仍能读到缓冲里的值）。**这一点是对的，值得记：`close` 不丢缓冲。**

### 六、`StreamWriter`：`custom` 模式的入口，而它写的是 `debug`

**上面已经点出它发 `EventTypeDebug`。这里再看它的全貌：**

```
type StreamWriter struct {
	streamManager *StreamManager
	step          int
	node          string
}

func NewStreamWriter(sm *StreamManager, step int, node string) *StreamWriter

func (w *StreamWriter) Write(data any) error {
	w.streamManager.EmitDebug(w.step, fmt.Sprintf("custom output from node %s", w.node), data)
	return nil
}

func (w *StreamWriter) WriteJSON(data any) error {
	jsonData, err := json.Marshal(data)
	if err != nil { return err }
	return w.Write(jsonData)
}
```

**四个要点：**

**其一，`Write` 把 `data` 塞进 `Data["data"]`，而 `message` 是固定拼接的字符串。** 于是消费方拿到的是 `{"message": "custom output from node X", "data": <你的数据>}` —— **额外的包装层没有任何语义收益**，只是让消费方多拆一层。

**其二，`WriteJSON` **先序列化再交给 `Write`，于是 `Data["data"]` 是一个 `[]byte`。** 而 `Write` 的签名接受 `any` —— **「已经序列化的」与「未序列化的」在同一通道里不可区分。**

**其三，`Write` 永远返回 `nil`。** 于是「事件被丢弃」对调用方是不可见的 —— **而它确实会被丢弃（在 `custom` 模式下）。**

**其四，`StreamWriter` 没有 `ctx`。** 于是即使想「等缓冲有空位」，它也拿不到取消信号。**而 `emit` 的 `default` 分支让「等」根本不发生。**

**这套设计的意图显然是「节点可以往流里插自定义数据」，而落地状态是「只能在 debug 模式下工作，且携带一层无用包装」。**

### 七、`EventStream`：与 `StreamManager` 平行的第二套

```
type EventStream struct {
	ctx          context.Context
	cancel       context.CancelFunc
	streamEvents chan *StreamEvent
	streamErrors chan error
}

func NewEventStream(ctx context.Context) *EventStream {
	ctx, cancel := context.WithCancel(ctx)
	return &EventStream{
		ctx: ctx, cancel: cancel,
		streamEvents: make(chan *StreamEvent, 100),
		streamErrors: make(chan error, 10),
	}
}
```

**它与 `StreamManager` 的差别是**三处**：**

| | `StreamManager` | `EventStream` |
|---|---|---|
| 缓冲 | 可配（默认 100） | 硬编码 100 |
| 满时 | **丢** | **阻塞 + `ctx.Done()` 出口** |
| 关闭 | `close` 两个通道 | `cancel()` **后** `close` 两个通道 |

**`Emit` 的实现是阻塞式的：**

```
func (es *EventStream) Emit(event *StreamEvent) {
	select {
	case es.streamEvents <- event:
	case <-es.ctx.Done():
	}
}
```

**「塞不进去就等，等到取消就放弃」** —— 而**它不再重试**（`select` 退出即返回）。于是「取消时的事件被丢」是明确的。

**而 `Cancel` 的顺序值得注意：**

```
func (es *EventStream) Cancel() {
	es.cancel()
	close(es.streamEvents)
	close(es.streamErrors)
}
```

**「先 `cancel` 再 `close`」** —— 于是任何阻塞在 `Emit` 里的 goroutine 会先因为 `ctx.Done()` 而退出 `select`，然后 `close` 才执行。**这个顺序避免了「send on closed channel」。**

**而 `Cancel` 不是幂等的** —— 第二次调用会 `close` 已关闭的通道而 panic。**这与 `StreamManager.Close` 的幂等性形成对比。**

**`ChannelStream` 的 `Close` 则是幂等的（`if s.closed { return nil }`），而它的 `Emit` 是「先查 `closed` 再阻塞发送」：**

```
s.mu.RLock()
if s.closed { s.mu.RUnlock(); return &StreamError{Message: "stream is closed"} }
s.mu.RUnlock()

chunk.Mode = s.mode
chunk.Timestamp = time.Now().UnixNano()

select {
case s.ch <- chunk:
	return nil
case <-ctx.Done():
	return ctx.Err()
}
```

**注意 `s.mu.RUnlock()` **之后**才发送 —— 于是「检查时没关、发送时已关」是可能的。** 而 `Close` 在 `Lock` 下 `close(s.ch)` —— **这是典型的「check-then-act」窗口，会在 `close` 之后 `send` 而 panic。**

**对比 `StreamManager.emit`：它在 RLock **期间**发送（`defer RUnlock`），所以没有这个窗口。** 两处的差异不是风格问题，而是**一个安全一个不安全**。

**这三个流结构的关闭安全性可以列成一张表：**

| 结构 | 关闭是否幂等 | 检查-发送是否在同一锁内 | 是否可能 send-on-closed |
|---|---|---|---|
| `StreamManager.emit` | 是（`Close` 幂等） | **是** | 否 |
| `StreamManager.EmitFinal` | — | **不检查** | **是（panic）** |
| `EventStream.Emit` | 否（`Cancel` 不幂等） | 靠 `cancel` 先于 `close` | 否（顺序保证） |
| `ChannelStream.Emit` | 是（`Close` 幂等） | **否**（检查后解锁再发） | **是（panic）** |

**「四种组合里两种可能崩」** —— 而它们都是「并发写通道」这个操作的常见写法。**这就是 Go 里 `close` 必须由发送方在确认没有其它发送者之后才做的原因**（e04 那页对 `outputCh` 的同一件事有更详细的讨论）。

### 八、`StreamMessagesHandler`：把 token 碎片拼成消息

**核心状态是「每个节点一条 `MessageStream`」：**

```
type StreamMessagesHandler struct {
	streams      map[string]*MessageStream
	mu           sync.RWMutex
	aggregator   *MessageAggregator
	flushTrigger FlushTrigger
}

type MessageStream struct {
	node    string
	chunks  []*MessageChunk
	current *MessageChunk
	closed  bool
	mu      sync.RWMutex
}

type MessageChunk struct {
	Index      int
	Content    string
	Metadata   map[string]any
	IsComplete bool
	Role       string
	ToolCalls  []*ToolCall
}
```

**「`chunks` 是已完成的消息列表、`current` 是正在累积的那一条」** —— 而 `Index = len(ms.chunks)` 在创建时算：

```
if ms.current == nil || ms.current.IsComplete {
	ms.current = &MessageChunk{ Index: len(ms.chunks), Metadata: make(map[string]any) }
}
```

**`OnChunk` 的合并逻辑是**六个顺序步骤**：**

```
// Merge chunk content
ms.current.Content += chunk.Content

// Update metadata
if chunk.Metadata != nil {
	if ms.current.Metadata == nil { ms.current.Metadata = make(map[string]any) }
	for k, v := range chunk.Metadata { ms.current.Metadata[k] = v }
}

// Update role
if chunk.Role != "" { ms.current.Role = chunk.Role }

// Merge tool calls
if len(chunk.ToolCalls) > 0 {
	if ms.current.ToolCalls == nil { ms.current.ToolCalls = make([]*ToolCall, 0) }
	ms.current.ToolCalls = append(ms.current.ToolCalls, chunk.ToolCalls...)
}

// Mark as complete if needed
if chunk.IsComplete {
	ms.current.IsComplete = true
	ms.chunks = append(ms.chunks, ms.current)
	ms.current = nil
	h.aggregator.AddMessage(node, ms.chunks[len(ms.chunks)-1])
	if h.flushTrigger == FlushOnComplete { return h.aggregator.Flush(ctx) }
} else if h.flushTrigger == FlushImmediate {
	return h.aggregator.Flush(ctx)
}
```

**前三个是「累加 / 覆盖 / 非空才覆盖」，第四与第五个各有问题：**

**其一，`Content += chunk.Content` 是纯字符串拼接。** 于是 token 增量天然被支持 —— **而若上游给的是「完整内容」而不是「增量」，拼接就会重复。** 这个契约没有在类型里表达（`MessageChunk` 没有「这是增量还是全量」的标志）。

**其二，`ToolCalls` 直接 `append` 而不合并。** 于是流式工具调用（`arguments` 分片到达）会得到**多个半成品 `ToolCall`** 而不是一个完整的。**而 `ToolCall` 结构里 `Arguments` 是 `map[string]any`（已解析的），于是「分片的 JSON 字符串」根本放不进去。**

**这是一个真实的限制：`ToolCall.Arguments` 是 map 而不是 `string`，所以「流式 arguments 增量」在这个结构上没有落点。**

**其三，`IsComplete` 触发「封条 + 入列 + 交给 aggregator」** —— 而 `FlushOnComplete` 时**立刻 Flush**。**于是默认行为是「每条消息完成就发一次」。**

**而 `OnComplete`（节点结束）走另一条路：**

```
func (h *StreamMessagesHandler) OnComplete(ctx context.Context, node string) error {
	h.mu.Lock()
	defer h.mu.Unlock()

	if ms, ok := h.streams[node]; ok {
		ms.mu.Lock()
		defer ms.mu.Unlock()

		if !ms.closed {
			ms.closed = true

			// Complete any pending chunk
			if ms.current != nil {
				ms.current.IsComplete = true
				ms.chunks = append(ms.chunks, ms.current)
				ms.current = nil
				h.aggregator.AddMessage(node, ms.chunks[len(ms.chunks)-1])
			}

			if h.flushTrigger == FlushOnEnd { return h.aggregator.Flush(ctx) }
		}
	}
	return nil
}
```

**「节点结束时把还在累积的那一条强制完成」** —— 这是一个必要的补偿，否则一个没有 `IsComplete` 的最后一个 chunk 永远不会被发出去。

**而 `FlushOnEnd` 时 `OnComplete` 才 Flush** —— 于是「一次运行只发一次」。**这与 `FlushImmediate`（每个 chunk 都 flush）是两个极端。**

**三个 `FlushTrigger` 值各自的语义可以总结成：**

| `FlushTrigger` | 何时发出 |
|---|---|
| `FlushOnComplete`（默认） | 每条消息 `IsComplete` 时 |
| `FlushImmediate` | **每个 chunk 到达时** |
| `FlushOnEnd` | 节点结束时 |

**`FlushImmediate` 意味着「同一个消息会被拆成多次 emit」** —— 而 `MessageAggregator` 只是把 chunk 逐个交给 emitter，**所以下游会收到半成品。** 而 `MessageChunk.Content` 在累积过程中是**逐渐变长的**，于是「增量」与「全量」在不同 trigger 下不一样。

**这是一个**契约缺失**：同一个 `MessageChunk` 类型在三种 trigger 下承载三种语义（完整消息 / 前缀 / 增量），而消费方无法从数据上区分。**

### 九、`MessageAggregator`：`Flush` 边发边删，而错误会漏掉后面的

```
func (a *MessageAggregator) Flush(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	for node, chunks := range a.messages {
		for _, chunk := range chunks {
			for _, emitter := range a.emitters {
				if err := emitter(ctx, node, chunk); err != nil { return err }
			}
		}
		delete(a.messages, node)
	}
	return nil
}
```

**三个问题，且都是「返回值与副作用不一致」：**

**其一，`delete` 在双层循环**之后**执行** —— 于是「一个节点的部分 chunk 发完了、emitter 报错」会**整体保留这个节点的所有 chunk**（包括已经发出去的那些）。**下一轮 Flush 会重复发已发过的。**

**其二，`delete(a.messages, node)` 在 `for node := range` 里**删除正在遍历的 map**。** Go 允许遍历时删除（删除的键不会被访问），所以不崩，**但被删的键若在后面被重新加回来，行为是不确定的。**

**其三，`Flush` **返回第一个 emitter 错误就退出**，剩下的节点**一条都不发**。** 于是「某个 emitter 坏了」会让其它节点的消息全部积压。

**「边发边删 + 出错即退」这个组合是**最差的一种**：既不原子（部分发出了）、也不幂等（重发会重复）、也不隔离（一个节点失败影响全部）。**

**正确的做法应该是「先把所有 chunk 从 map 里摘出来，再逐个发，成功的删、失败的留」。** 而现在的实现把它写成了「边发边删」。

**而 `MessageAggregator` 的 `AddMessage` 只是 append：**

```
func (a *MessageAggregator) AddMessage(node string, chunk *MessageChunk) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.messages[node] = append(a.messages[node], chunk)
}
```

**「`a.messages[node]` 是 `[]*MessageChunk`」——** 存的是指针**。** 而 `StreamMessagesHandler.GetAllMessages` 拷贝切片但**共享 `*MessageChunk` 指针**：

```
chunks := make([]*MessageChunk, len(ms.chunks))
copy(chunks, ms.chunks)
```

**「拷贝切片、共享指针」** —— 与 e03 那页 `deepCopy` 的两种策略同理（浅拷贝容器 vs 深拷贝值）。**而这里选了浅的：调用方改 `chunk.Content` 会改到内部状态。**

### 十、三个 `MessageEmitter` 和一个 `ExtractMessagesFromOutput`

**`StreamToChannel` 把 `MessageChunk` 映射成 `types.StreamChunk` 并塞进 `ChannelStream`：**

```
func StreamToChannel(ch *types.ChannelStream) MessageEmitter {
	return func(ctx context.Context, node string, chunk *MessageChunk) error {
		chunkData := map[string]any{
			"node": node, "role": chunk.Role, "content": chunk.Content,
			"index": chunk.Index, "complete": chunk.IsComplete,
		}
		if len(chunk.ToolCalls) > 0 { chunkData["tool_calls"] = chunk.ToolCalls }
		if len(chunk.Metadata) > 0  { chunkData["metadata"] = chunk.Metadata }
		return ch.Emit(ctx, &types.StreamChunk{
			Mode: types.StreamModeMessages, Data: chunkData, Node: node,
		})
	}
}
```

**「把六个字段重新装进一个 `map[string]any`」** —— 于是**一个强类型的 `MessageChunk` 被降级成无类型 map 才跨出去**。**这与 `StreamEvent.Data` 的 `any` 是同一种「类型信息在边界上被丢掉」的形态。**

**`StreamToWriter` 有三种输出格式：**

```
switch format {
case "json":
	output = fmt.Sprintf(`{"node":"%s","role":"%s","content":%q}`, node, chunk.Role, chunk.Content)
case "compact":
	output = fmt.Sprintf("[%s:%s] %s", node, chunk.Role, chunk.Content)
default:
	output = fmt.Sprintf("%s: %s", chunk.Role, chunk.Content)
}
```

**「`json` 分支手工拼 JSON」** —— 而 `node` 与 `role` 用 `%s` 直接插入双引号内，**没有转义**。**一个含双引号的节点名会产出非法 JSON。** 而 `content` 用了 `%q`（Go 的引号转义），**所以只有两个字段有风险。**

**这是一个「手拼 JSON」的典型疏漏** —— 而它旁边就有一个 `WriteJSON`（用 `json.Marshal`）。

**`CollectToMap` 最简单：往目标 map 里 append。**

**而 `ExtractMessagesFromOutput` 是「把节点输出反推成消息」：**

```
func ExtractMessagesFromOutput(output any) ([]*MessageChunk, error) {
	if chunk, ok := output.(*MessageChunk); ok { return []*MessageChunk{chunk}, nil }
	if chunks, ok := output.([]*MessageChunk); ok { return chunks, nil }
	if m, ok := output.(map[string]any); ok {
		if content, ok := m["content"].(string); ok {
			role := "assistant"
			if r, ok := m["role"].(string); ok { role = r }
			chunk := &MessageChunk{ Content: content, Role: role, Metadata: make(map[string]any) }
			...
		}
	}
}
```

**「接受三种输入形态」** —— 而 `map` 那一支持**只认 `content` 与 `role` 两个键**，其它字段丢弃。**而 `role` 默认 `asistant`** —— 这是一个「节点返回了一个 `{"content": ...}` 的 map」时的合理默认。

**注意它的返回值 `([]*MessageChunk, error)` 里 `error` 在实现中大概总是 `nil`**（三个分支全是「命中就返回」）—— **除非最后有「不认识的形态就报错」的兜底。** 这让「必须检查 error」成了一种仪式。

## 关键取舍

**源码事实：** 三条独立的流机制：`StreamManager`（`chan *StreamEvent` + `includeFilter` 位图）、`StreamMessagesHandler`→`MessageAggregator`→`MessageEmitter`（token 聚合）、`types.ChannelStream`（`chan *StreamChunk` + 迭代器）；三者各自处理 `StreamMode` 且互不知晓对方。

**源码事实：** `final` 事件在所有 `StreamMode` 分支里都被置位；而 **`interrupt` 事件只在 `debug` 分支里置位** —— 于是 `values` / `updates` / `tasks` / `checkpoints` 四种模式下 `EmitInterrupt` 的调用是「发了但被过滤」，调用方无法从流里知道中断发生了。

**源码事实：** `custom` 与 `messages` 两个 `StreamMode` **在 `setupIncludeFilter` 里没有 case**，落入 `default`（得到 `values` + `final`）。而 `StreamWriter.Write` 发的是 `EventTypeDebug` —— **于是在 `custom` 模式下节点的自定义输出被 `shouldEmit` 过滤掉、`Write` 仍返回 `nil`。`StreamWriter` 只在 `debug` 模式生效。**

**源码事实：** 八个 `Emit*` 走 `emit`，用 `select` 带 `default` **丢事件**（注释仅「Channel full, drop event」）；而 `EmitFinal` 用**裸阻塞发送**，注释理由「丢最终事件会让 `RunSync` 返回 nil 状态（静默数据丢失）」，且**没有 `ctx.Done()` 也没有超时**。

**源码事实：** `EmitFinal` **不获取 `sm.mu.RLock()`**，而 `Close` 在 `Lock` 下 `close(sm.eventCh)` —— 于是「`Close` 之后 `EmitFinal`」会 send on closed channel 并 **panic**。其余八个 `emit` 在 RLock 期间发送（`defer RUnlock`），与 `Close` 的 Lock 互斥，安全。

**源码事实：** `emit` 在 `sm.mu.closed` 时静默返回；`Close` 幂等（`if !sm.mu.closed`）；`includeFilter` 在构造时写入一次、之后只读但**无类型层面的冻结表达**。

**源码事实：** `bufferSize` 可配（默认 100）而 `errorCh` 能力硬编码 10；`bufferSize` 被存成字段但**从未被读过**；`close` 之后缓冲里剩余事件仍能被消费方读到（Go channel 语义），于是「关闭不丢缓冲」。

**源码事实：** `StreamWriter.Write` 把数据装进 `Data["data"]` 并配一句固定的 `message` 字符串（无信息量）；`WriteJSON` 先序列化再走 `Write`，于是「已序列化」与「未序列化」在同一通道里不可区分。

**源码事实：** `EventStream.Emit` 是**阻塞 + `ctx.Done()` 退出**（不重试、取消即丢）；`Cancel` **先 `cancel` 再 `close`**（避免 send-on-closed），但**不幂等**（二次调用 panic）。

**源码事实：** `ChannelStream.Emit` 先查 `closed`（RLock）**再解锁**、之后才阻塞发送 —— 「检查时没关、发送时已关」窗口存在，`Close` 的 `close(s.ch)` 会导致 panic；`ChannelStream.Close` 本身幂等。

**源码事实：** `MessageStream.OnChunk` 六步合并：`Content +=`（纯字符串拼接，**无法区分增量与全量**）、`Metadata` 逐键覆盖、`Role` 非空才覆盖、**`ToolCalls` 直接 `append` 而不合并**（而 `ToolCall.Arguments` 是 `map[string]any`，流式 JSON 分片无处落）、`IsComplete` 时封条入列并交给 aggregator。

**源码事实：** 三个 `FlushTrigger` 让同一个 `MessageChunk` 类型承载**三种语义**：`FlushOnComplete`（完整消息）、`FlushImmediate`（每次 chunk 都发，半成品）、`FlushOnEnd`（节点结束时一次）；消费方无法从数据上区分。

**源码事实：** `OnComplete` 在节点结束时**强制完成还在累积的那一条**（否则没有 `IsComplete` 的末 chunk 永远不会发出）。

**源码事实：** `MessageAggregator.Flush` 在双层循环**内部**返回 emitter 错误、**循环之后**才 `delete(a.messages, node)` —— 于是「部分发出 + 全部保留」导致下一轮重复发已发过的；且**返回第一个 emitter 错误就退出**，剩下的节点一条都不发。

**源码事实：** `MessageAggregator` 存 `[]*MessageChunk` 指针；`GetAllMessages` 拷贝切片但**共享指针**，调用方改 `chunk.Content` 会改到内部状态。

**源码事实：** `StreamToChannel` 把强类型的 `MessageChunk` 六个字段重新装进 `map[string]any` 才交给 `types.StreamChunk`；`StreamToWriter` 的 `json` 分支**手工拼 JSON**且 `node`/`role` 未转义（含双引号的节点名会产出非法 JSON），而 `content` 用了 `%q`。

**源码事实：** `ExtractMessagesFromOutput` 接受三种形态（`*MessageChunk` / `[]*MessageChunk` / 带 `content` 的 map），`role` 默认 `"assistant"`，其它键丢弃。

**架构推论：** 「中间事件丢、最终事件必达」这个分档是正确的优先级判断 —— 流式事件的价值在「进度可见」而不在「完整」，而最终状态是返回值本体。**但这个分档的实现只做了一半：`EmitFinal` 为了「必达」改成了阻塞发送，却没有同时处理「通道可能已关闭」与「无人读则永久阻塞」这两个必然后果。** 「保证送达」是一个需要两侧配合的承诺（发送方阻塞 + 接收方保证会读），而代码只改了发送方。

**架构推论：** 三个流结构里两处「check-then-act on closed channel」的差异说明**这个问题在同一个包里被独立地犯了两次**。`StreamManager.emit` 把发送放在锁内是对的（而 `EmitFinal` 漏了），`ChannelStream.Emit` 把发送放在锁外是错的。**「关闭通道」之所以是 Go 里最容易写错的一步，根本原因是它必须由发送方发起，而发送方往往有多个。** 正确形态只有两种：要么所有发送都在同一把锁内 + 关闭也在锁内（`StreamManager.emit`），要么用 `context` 做「已关闭」的信号 + 永不 close（`EventStream`）。

**架构推论：** `FlushTrigger` 把同一个数据结构在三种不同粒度上复用，是这个包最需要重构的地方。**一个"消息"在 `FlushImmediate` 下是增量、在 `FlushOnComplete` 下是完整、在 `FlushOnEnd` 下是整轮汇总** —— 三种语义都塞进 `MessageChunk`，而它没有字段表达自己处于哪种。**这直接导致 `StreamToChannel` 无法告诉下游「该累积还是该追加」**，于是同一个下游代码在切换 trigger 时就会错。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/graph/pregel/stream.go` | `StreamEventType` / `EventTypeValues` 等九个常量 | checkpoint / task_start / task_end / update / values / interrupt / error / final / debug |
| `internal/harness/graph/pregel/stream.go` | `StreamEvent` / `NewStreamEvent` | 七字段事件（`Data` 是 `any`） |
| `internal/harness/graph/pregel/stream.go` | `StreamManager` / `NewStreamManager` | `chan *StreamEvent`（可配缓冲）+ `includeFilter` |
| `internal/harness/graph/pregel/stream.go` | `setupIncludeFilter` | 五个 case + 兜底 `default`；`interrupt` 只在 `debug` 开 |
| `internal/harness/graph/pregel/stream.go` | `shouldEmit` | `includeFilter` 查表（RLock） |
| `internal/harness/graph/pregel/stream.go` | `emit` | `select` + `default` → **丢事件** |
| `internal/harness/graph/pregel/stream.go` | `EmitCheckpoint` / `EmitTaskStart` / `EmitTaskEnd` / `EmitUpdate` / `EmitValues` / `EmitInterrupt` / `EmitError` / `EmitDebug` | 八个走 `emit` 的事件 |
| `internal/harness/graph/pregel/stream.go` | `EmitFinal` | **裸阻塞发送**（无 ctx、无锁） |
| `internal/harness/graph/pregel/stream.go` | `Events` / `Errors` / `Close` | 通道出口与幂等关闭 |
| `internal/harness/graph/pregel/stream.go` | `StreamWriter` / `NewStreamWriter` | 节点内自定义输出入口 |
| `internal/harness/graph/pregel/stream.go` | `StreamWriter.Write` / `WriteJSON` | 发 `EventTypeDebug`（`custom` 模式下被过滤）、永远返回 `nil` |
| `internal/harness/graph/pregel/stream.go` | `EventStream` / `NewEventStream` | 第二套：阻塞发送 + `ctx.Done()` |
| `internal/harness/graph/pregel/stream.go` | `EventStream.Cancel` | 先 `cancel` 再 `close`，不幂等 |
| `internal/harness/graph/pregel/stream.go` | `StreamProcessor` / `Process` / `ProcessStream` | filter → transform → handler 三段管道 |
| `internal/harness/graph/pregel/messages.go` | `StreamMessagesHandler` / `NewStreamMessagesHandler` | per-node `MessageStream` + aggregator |
| `internal/harness/graph/pregel/messages.go` | `MessageStream` / `MessageChunk` / `ToolCall` | 消息与工具调用结构 |
| `internal/harness/graph/pregel/messages.go` | `FlushTrigger` / `WithFlushTrigger` | 三个触发器（三种语义） |
| `internal/harness/graph/pregel/messages.go` | `OnChunk` | 六步合并（Content 拼接、ToolCalls 直接 append） |
| `internal/harness/graph/pregel/messages.go` | `OnComplete` | 节点结束时强制完成末条 |
| `internal/harness/graph/pregel/messages.go` | `Flush` / `AddEmitter` / `GetMessages` / `GetAllMessages` / `Close` | handler 出口（`GetAllMessages` 共享指针） |
| `internal/harness/graph/pregel/messages.go` | `MessageAggregator` / `NewMessageAggregator` | 待发消息表 |
| `internal/harness/graph/pregel/messages.go` | `MessageAggregator.Flush` | 边发边删、出错即退、先发后删 |
| `internal/harness/graph/pregel/messages.go` | `MessageEmitter` / `AddEmitter` | 回调类型 |
| `internal/harness/graph/pregel/messages.go` | `StreamToChannel` | `MessageChunk` → `types.StreamChunk`（降级成 map） |
| `internal/harness/graph/pregel/messages.go` | `StreamToWriter` | 三种格式（`json` 分支手工拼接、两个字段未转义） |
| `internal/harness/graph/pregel/messages.go` | `CollectToMap` | 收集到 map |
| `internal/harness/graph/pregel/messages.go` | `ExtractMessagesFromOutput` | 从节点输出反推消息（`role` 默认 `assistant`） |
| `internal/harness/graph/types/stream.go` | `StreamChunk` | 七字段 chunk（`Mode` / `Data` / `Step` / `Node` / `Metadata` / `Timestamp` / `Index`） |
| `internal/harness/graph/types/stream.go` | `StreamProtocol` / `StreamIterator` | 五方法协议 + 三方法迭代器 |
| `internal/harness/graph/types/stream.go` | `ChannelStream` / `NewChannelStream` | 通道实现（检查与发送不在同一锁内） |
| `internal/harness/graph/types/stream.go` | `ChannelStream.Emit` / `Close` | 阻塞发送 / 幂等关闭 |
| `internal/harness/graph/types/stream.go` | `channelIterator.Next` | `io.EOF` 收尾 + `ctx.Done()` |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：pregel/stream.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/graph/pregel/stream.go)
- [ragflow 源码：pregel/messages.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/graph/pregel/messages.go)
- [LangGraph 文档：streaming](https://langchain-ai.github.io/langgraph/concepts/streaming/)
- [Go 文档：channel 与 close 的同步语义](https://go.dev/ref/spec#Close_of_channels)
- [Go 官方博客：Go 并发模式（channel）](https://go.dev/blog/codelab-share)

## 与主核 / AI 的关系

流式事件与主核的**预写日志（WAL）发送**在结构上高度可比：都是「一边做事一边往外送消息」，都要回答「送不出去时怎么办」。

**WAL 的答案是 `fsync`** —— 送不出去就阻塞、就报错、就中止事务。**因为没有日志就没有恢复能力，丢一条日志等于丢一段账。** 而 `StreamManager` 的答案是「丢」—— **因为流式事件不是账，它是给人和 UI 看的进度。** 两者都没有错，区别在于「丢了之后能不能补」。

**这个判据可以形式化：** 一个信号是否可以「丢了不补」，取决于「有没有另一个权威副本」。WAL 没有（它就是权威），所以必须同步；流式事件有（状态在检查点里），所以可以丢。**而 `EmitFinal` 之所以必须阻塞，正是因为「最终状态」没有别的副本** —— 它就是返回值。**这个分界恰好落在「有副本/无副本」上，而不是落在「重要/不重要」上。**

**「`close` 必须由发送方在确认无其它发送者之后才做」这条纪律，在数据库里对应的是「关停时必须先等所有后台进程退出再释放共享内存」** —— PostgreSQL 的 shutdown 顺序（`SIGTERM` → 等 checkpoint → 等 walwriter → 释放共享内存）就是这个形状。**顺序错了的后果是「访问已释放的内存」，与 Go 的「send on closed channel」同类。**

**`FlushTrigger` 这个三档设计在数据库里没有直接对应物**，但它的**问题**有：同一个数据结构承载多种粒度。**数据库用「元组级 / 页级 / 事务级」三种不同的结构来表达不同粒度，而不试图一个结构通吃。** 这是从反面得到的教训：**粒度为王的场合，不要复用一个类型。**

**「`TaskEnd` 里 `duration` 是字符串」这个决定，在数据库里对应的是「性能计数器统一按 double 存」。** 后者牺牲精度换统一（可以任意汇总、比较），前者牺牲可比较性换序列化方便。**统一成一个「可计算的类型」通常比统一成一个「可打印的类型」正确** —— 因为从数值到字符串是 trivial 的，反过来不是。

对 agent 工程的直接价值有三条。其一，**流式输出的背压策略必须按信号类型分档**。有权威副本的可丢、无副本的必须送达——而「必须送达」的实现要同时处理「通道关闭」与「接收方不读」，否则「保证送达」会变成「保证死锁或 panic」。其二，**token 增量与完整消息在同一个类型里必须可区分**。`Content +=` 这个实现依赖「上游只送增量」这一隐式契约，而换一个 trigger 就变成「送前缀」——**消费方无从判断，于是同一段下游代码在两种配置下都会错一种**。其三，**流式工具调用需要「参数分片」的落点**。`ToolCall.Arguments` 是已解析的 map，于是「`arguments` 的 JSON 分片」无处可存——**这不是实现问题而是数据结构问题**，需要在类型里给「未完成的参数」留一个字符串槽位。
