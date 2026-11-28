---
project: ragflow
feature: checkpoint
title: 检查点与待写记录
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/graph/checkpoint/checkpoint.go
    symbols: [BaseCheckpointer, Checkpoint, CheckpointTuple, PendingWrite]
---

# 检查点与待写记录

## 一句话职责

用一个**三方法的 `BaseCheckpointer` 接口**（`Get` / `Put` / `List`）接上内存与 NATS JetStream 两种持久化，并把一次运行的状态编成五类东西：**通道版本账、每节点见过的版本账、当前状态、已算但未提交的写（`PendingWrite`）、以及血缘元数据**。

## 机制是什么

这个文件里同时躺着**两个不同的持久化抽象**，而它们服务两个不同的阶段：

| | `BaseCheckpointer` | `CheckpointManager` |
|---|---|---|
| 方法数 | 3（`Get` / `Put` / `List`） | 15（`Save` / `PutWrites` / `Load` / `List` / `GetTuple` / `GetLineage` / `Delete` / `ClearThread` / `ClearAll` …） |
| 接口形态 | `map[string]interface{}` 进 `map[string]interface{}` 出 | 强类型 `*Checkpoint` |
| 谁用 | `graph` 与 `pregel`（编译期注入、执行期读写） | 内部管理（版本冲突检测、血缘遍历） |
| 是否有并发控制 | **完全没有** | 有（`VersionConflictError`） |

**这个分工值得先看清**，因为它决定了「一次运行的持久化」到底靠谁：**`BaseCheckpointer` 是引擎真正调的**（见 e04 那页的三种 `Durability` 档位），而 `CheckpointManager` 是那套结构的**内存权威实现 + 冲突检测器**。

**而 `Checkpoint` 的八个字段就是 Pregel 的全部持久化状态：**

| 字段 | 用途 |
|---|---|
| `ID` | 检查点标识（UUID） |
| `Version` | 单调递增版本号 |
| `ParentID` | 上一个检查点的 `ID`（血缘） |
| `ChannelVersions` | **每个通道的当前版本** |
| `VersionsSeen` | **每个节点见过的各通道版本** |
| `State` | 通道当前值（`map[channel]value`） |
| `PendingWrites` | **已算但未提交的写** |
| `Metadata` | ID / ParentID / ThreadID / Step / CreatedAt / Source |

**`ChannelVersions` 与 `VersionsSeen` 这两张账是「增量触发判断」的全部依据** —— 见 e04 那页对 `channelVersions` / `versionsSeen` 的说明。**而在这里它们被持久化了，于是「重启后哪些节点该重新触发」不需要重算。**

## 怎么实现的

### 一、`PendingWrite`：为「写入成功了但超步没跑完」准备

**它的六个字段，而其中两个在序列化时被丢掉（下面会讲）。**

```
type PendingWrite struct {
	Channel   string      // 目标通道
	Value     interface{} // 要写的值
	Overwrite bool        // 绕过 reducer 直接覆盖
	Node      string      // 发起这次写的节点
	Timestamp time.Time
	TaskID    string      // 创建这次写的任务 id
}

func NewPendingWrite(channel string, value interface{}, overwrite bool, node, taskID string) *PendingWrite
```

**「已算但未提交」这个状态为什么必须持久化？**

**因为 Pregel 的一个超步里，节点执行与「把写入应用到通道」是两步。** 若一个节点算完了、它的写入被记下了、但超步还没收尾就崩溃 —— 那么**重跑时不该再跑那个节点（它已经算过了），而该把它的写入应用上去。**

**`PendingWrite` 就是这个「中间账本」。** 而 `TaskID` 的存在说明了更细的粒度：**同一个通道可能有多条待写记录，来自不同任务，需要在重放时按任务去重。**

**而 `Overwrite` 这个布尔表达的是「这条写绕过 reducer」** —— 对应 e02 那页 `BinaryOperatorAggregate.isOverwrite` 识别的那四种形态里的前三种（进程内结构）。**但本页会看到，`PutWrites` 并没有真的用它。**

### 二、`Checkpoint` 的六个方法，而 `Clone` 是最重的一个

```
func NewCheckpoint(threadID string, step int) *Checkpoint {
	id := uuid.New().String()
	return &Checkpoint{
		ID:              id,
		Version:         0,
		ChannelVersions: make(map[string]int),
		VersionsSeen:    make(map[string]map[string]int),
		State:           make(map[string]interface{}),
		PendingWrites:   make([]PendingWrite, 0),
		Metadata: CheckpointMetadata{
			ID:        id,
			ThreadID:  threadID,
			Step:      step,
			CreatedAt: time.Now(),
			Source:    SourceNode,
			Metadata:  make(map[string]interface{}),
		},
	}
}
```

**三个要点：**

1. **`Version` 从 0 开始** —— 而这个 0 有特殊含义（见 `Save` 的第二个检查）。
2. **`Metadata.ID` 与 `ID` 是同一个 UUID** —— 于是「元数据里的 id」与「检查点自己的 id」不会分叉。
3. **`Metadata.Source` 默认 `SourceNode`** —— 而 `Source` 是五个值之一。

**`CheckpointSource` 五个值把「这个检查点是怎么产生的」编进了数据：**

| 值 | 何时产生 |
|---|---|
| `node` | 节点执行后 |
| `edge` | 边遍历后 |
| `interrupt` | 中断时 |
| `manual` | 手动创建 |
| `resume` | 恢复时 |

**「中断时产生的检查点有自己的 `Source`」** —— 这与 e04 那页「中断是带持久化的一等退出」是同一条设计的另一半：**不仅要知道「有个检查点」，还要知道「它是中断造成的」**，因为「从哪种检查点恢复」决定了恢复后的行为（继续超步 vs 重跑中断节点）。

**`Clone` 是唯一一个会造新 id 的方法，而它的注释解释了每一处深拷贝的必要性：**

```
func (c *Checkpoint) Clone() *Checkpoint {
	newID := uuid.New().String()
	clone := &Checkpoint{
		ID:              newID,
		Version:         c.Version,
		ParentID:        c.ID,
		ChannelVersions: make(map[string]int, len(c.ChannelVersions)),
		VersionsSeen:    make(map[string]map[string]int, len(c.VersionsSeen)),
		State:           make(map[string]interface{}, len(c.State)),
		PendingWrites:   make([]PendingWrite, len(c.PendingWrites)),
		Metadata: CheckpointMetadata{
			ID:        newID, // same ID as the clone
			ParentID:  c.ID,
			ThreadID:  c.Metadata.ThreadID,
			Step:      c.Metadata.Step,
			CreatedAt: time.Now(),
			Source:    c.Metadata.Source,
			Metadata:  make(map[string]interface{}),
		},
	}
	// Copy channel versions
	for k, v := range c.ChannelVersions { clone.ChannelVersions[k] = v }
	// Copy versions seen
	for node, versions := range c.VersionsSeen {
		clone.VersionsSeen[node] = make(map[string]int)
		for k, v := range versions { clone.VersionsSeen[node][k] = v }
	}
	// Copy state
	for k, v := range c.State { clone.State[k] = deepCopy(v) }

	// Deep-copy pending writes — each Value must be independently copied so that
	// mutating the clone's Value never affects the original.
	clone.PendingWrites = make([]PendingWrite, len(c.PendingWrites))
	for i, pw := range c.PendingWrites {
		clone.PendingWrites[i] = pw
		clone.PendingWrites[i].Value = deepCopy(pw.Value)
	}

	// Copy metadata — deep copy to avoid shared maps/slices.
	for k, v := range c.Metadata.Metadata { clone.Metadata.Metadata[k] = deepCopy(v) }

	return clone
}
```

**四个决定：**

**其一，`ParentID = c.ID`。** 于是**「克隆」同时是一次「派生」** —— 克隆出来的检查点是原检查点的子节点。**这与 e01 那页 `Compile` 里「写回未编译的图」是同一类问题：一个看起来像拷贝的操作同时携带了血缘语义。** 若调用方只想「拿一份快照去改」，克隆出的那份会指向原节点 —— 之后 `Save` 的 ParentID 校验就可能因此通过或失败，取决于它想干什么。

**其二，`CreatedAt: time.Now()`。** 克隆得到**当前**时间而不是原检查点的时间。**这也是「克隆=派生」的证据** —— 若只是想复制，时间该跟着复制。

**其三，`PendingWrites` 用 `clone.PendingWrites[i] = pw` 再单独 `deepCopy(pw.Value)`。** 这一行比「整体深拷贝」讲究：**先按值复制整个结构体（含 `Timestamp` / `TaskID` 这些值类型），再单独深拷 `Value`。** 注释说得明确：**「每个 `Value` 必须独立拷贝，这样改克隆的 `Value` 永远不会影响原件」。**

**其四，`Metadata.Metadata` 也深拷** —— 「避免共享 map/切片」。

**而 `Metadata.ID: newID` 那行注解 `// same ID as the clone` 是一个让人停顿的注释** —— 它的意思大概是「与克隆体顶层 `ID` 相同」，而**字面上读起来像「与原件的 ID 相同」**。**这是一个「注释写得不够准」的实例**：代码是对的，注释读起来是反的。

**三个版本账方法很薄但语义精确：**

```
func (c *Checkpoint) IncrementChannel(channel string) { c.ChannelVersions[channel]++ }

func (c *Checkpoint) MarkSeen(node, channel string) {
	if _, ok := c.VersionsSeen[node]; !ok { c.VersionsSeen[node] = make(map[string]int) }
	c.VersionsSeen[node][channel] = c.ChannelVersions[channel]
}

func (c *Checkpoint) HasSeen(node, channel string) bool {
	if versions, ok := c.VersionsSeen[node]; ok {
		if version, ok := versions[channel]; ok {
			return version == c.ChannelVersions[channel]
		}
	}
	return false
}
```

**「`MarkSeen` 记的是通道**当前**的版本号」** —— 于是 `HasSeen` 是一个**相等**判断而不是「大于等于」。

**这个「相等」是有意的：** 它表达的是「这个节点已经消费到这个版本的通道了」。**若用「大于等于」，一个见过 v1 的节点在通道升到 v2 时会被判为「仍见过」—— 那就错了，因为它没见过 v2。**

**而 `HasSeen` 对「节点不在 `VersionsSeen` 里」返回 `false`** —— 于是**第一次运行的节点会被判定为「没见过」**，正确。

### 三、`ToMap` / `FromMap`：一次**丢字段**的序列化

**`ToMap` 把 `PendingWrite` 编成四键 map：**

```
pendingWrites := make([]map[string]interface{}, len(c.PendingWrites))
for i, pw := range c.PendingWrites {
	pendingWrites[i] = map[string]interface{}{
		"channel":   pw.Channel,
		"value":     pw.Value,
		"overwrite": pw.Overwrite,
		"node":      pw.Node,
	}
}
```

**而 `PendingWrite` 有**六个**字段。** 丢掉的是 `Timestamp` 与 `TaskID`。

**而 `FromMap` 那边只读回那四个：**

```
c.PendingWrites = append(c.PendingWrites, PendingWrite{
	Channel:   getString(pwMap, "channel"),
	Overwrite: getBool(pwMap, "overwrite", false),
	Node:      getString(pwMap, "node"),
	Value:     pwMap["value"],
})
```

**于是「序列化 → 反序列化」是一个**有损**往返：`Timestamp` 变成零值、`TaskID` 变成空串。**

**这是本页最重要的源码事实，而它的后果是两个具体的：**

**其一，`TaskID` 空掉之后，「按任务去重待写记录」这条能力在持久化之后失效。** 若重放逻辑依赖 `TaskID` 区分「同一通道的多次待写来自不同任务」，那么从检查点恢复之后它拿不到这个信息。

**其二，`Timestamp` 空掉意味着「待写的先后顺序」只能靠切片顺序维持，而不能靠时间。** 而顺序恰好是这里最要紧的东西 —— 多个待写写同一个通道时，**后写的应该赢**（这与 e02 那页 `AnyValue` 的「后者胜」是同一条约定）。

**这两处丢失都不报错。** 而 `ToMap` / `FromMap` 的注释都没有提到这件事 —— **读代码的人只有在逐字段对照时才会发现。**

**`ToMap` 的其余部分都是直白的拷贝，但 `CreatedAt` 用了 `Format(time.RFC3339Nano)` —— 而 `FromMap` 用同样的 layout 解析，且**解析失败不报错**：**

```
if createdAtStr := getString(md, "created_at"); createdAtStr != "" {
	if t, err := time.Parse(time.RFC3339Nano, createdAtStr); err == nil {
		c.Metadata.CreatedAt = t
	}
}
```

**「解析失败就留着零值」** —— 于是「时间格式坏了」会静默变成「1970 年」。**这是一个「宽容解析」的典型代价：错误被降级成数据。**

**而 `FromMap` 的三个 getter 都只认特定类型：**

```
func getString(data map[string]any, key string) string { if val, ok := data[key].(string); ok { return val }; return "" }
func getInt(data map[string]any, key string, defaultVal int) int { if val, ok := data[key].(float64); ok { return int(val) }; return defaultVal }
func getBool(data map[string]any, key string, defaultVal bool) bool { if val, ok := data[key].(bool); ok { return val }; return defaultVal }
```

**`getInt` 只认 `float64`** —— 因为 JSON 数字反序列化到 `interface{}` 全是 `float64`。**于是「传进来的 map 是手工构造的、里面存的是 `int`」时，`getInt` 会静默返回默认值。**

**这是一个「为 JSON 而写的 getter 被用在非 JSON 场景」的隐患。** 而它在本包里确实会被非 JSON 场景调用（`Clone` 不经过 `FromMap`，所以影响有限）。

### 四、`CheckpointTuple`：把「检查点 + 它的父配置」捆在一起

```
type CheckpointTuple struct {
	Config       *types.RunnableConfig
	Checkpoint   *Checkpoint
	ParentConfig *types.RunnableConfig
	Metadata     map[string]interface{}
}
```

**为什么需要「父配置」而不是「父检查点」？** 因为**恢复一个历史检查点时，调用方需要的是「怎么再取到它的父」**，而不是父的内容。**而 `ParentConfig` 就是一个恰好够用的 `RunnableConfig`（带 `thread_id` 与 `checkpoint_id`）。**

**`NewCheckpointTuple` 在 `checkpoint == nil` 时也造一个 `Metadata` 空 map：**

```
metadata := make(map[string]interface{})
if checkpoint != nil {
	metadata["id"] = checkpoint.ID
	metadata["version"] = checkpoint.Version
	metadata["thread_id"] = checkpoint.Metadata.ThreadID
	metadata["step"] = checkpoint.Metadata.Step
	metadata["source"] = string(checkpoint.Metadata.Source)
	metadata["created_at"] = checkpoint.Metadata.CreatedAt
}
```

**「`checkpoint` 为 nil 时返回一个 `Metadata` 空 map 而非 nil」** —— 于是调用方可以无条件 `tuple.Metadata["x"]` 而不会 panic。**这是一个细但正确的选择。**

**而 `GetTuple` 在没有检查点时返回 `NewCheckpointTuple(config, nil, nil)` 而不是 `nil, nil`：**

```
if len(checkpoints) == 0 {
	return NewCheckpointTuple(config, nil, nil), nil
}
```

**「返回一个 `Checkpoint` 为 nil 的元组」** —— 而 `Load` 在同一情况返回 `nil, nil`。**两个方法的「无数据」表示不一致。**

### 五、`CheckpointManager.Save`：两个检查，而第二个可以被绕过

**第一个检查是 `ParentID` 连续性：**

```
if checkpoints := cm.checkpoints[threadID]; len(checkpoints) > 0 {
	latest := checkpoints[len(checkpoints)-1]

	// Check 1: ParentID must match the latest checkpoint.
	if checkpoint.ParentID != "" && latest.ID != checkpoint.ParentID {
		return &VersionConflictError{
			CurrentVersion:  checkpoint.Version,
			ExpectedVersion: latest.Version + 1,
			CheckpointID:    checkpoint.ID,
			ThreadID:        threadID,
		}
	}
```

**「`ParentID` 非空且与最新检查点不符 → 冲突」** —— 于是「别人已经在你加载之后写了这个线程」会被抓住。

**而「`ParentID` 为空」是合法的** —— 它表达「这是一个线程的第一个检查点」或者「我不在血缘链上」。

**第二个检查是版本连续性，而它的条件里藏了一个逃逸口：**

```
	// Check 2: Version must be sequential. Only enforce when the caller
	// explicitly set a version (> 0); Version == 0 means the checkpoint
	// was created by NewCheckpoint (which always sets Version=0) and
	// the caller didn't intend to participate in version conflict detection.
	if checkpoint.Version > 0 && latest.Version != checkpoint.Version-1 {
		return &VersionConflictError{ ... }
	}
}
```

**注释把设计意图说清了：`Version == 0` 意味着「这是 `NewCheckpoint` 的默认值，调用方没打算参与冲突检测」，于是**跳过检查**。**

**这是一个**重要的语义漏洞**：任何用 `NewCheckpoint` 造出来、没手动设 `Version` 的检查点，**永远不会被版本检查拦住**。**而它同时又会因为 `ParentID` 可能为空而绕过第一个检查。**

**于是「两个检查都可以被绕过」** —— 只要调用方不做这两件事（不设 Version、不设 ParentID），`Save` 就退化成「append」。**对一个「冲突检测器」来说这是相当大的让步，而它的合理性在于**：`NewCheckpoint` 是最常见的构造路径，若默认值就触发检查，所有第一次调用都会冲突。

**`Clone` 出来的检查点会设 `ParentID`（= 原件的 ID）但**不**设 `Version`（`Clone` 保留原件的 `Version`）—— 于是克隆一个 `Version=0` 的检查点再 `Save`，两个检查都跳过。**这是一个会在实际使用中出现的路径。**

**而 `Save` 会在超过 `maxVersions` 时裁掉老的：**

```
cm.checkpoints[threadID] = append(cm.checkpoints[threadID], checkpoint)
if len(cm.checkpoints[threadID]) > cm.maxVersions {
	cm.checkpoints[threadID] = cm.checkpoints[threadID][len(cm.checkpoints[threadID])-cm.maxVersions:]
}
```

**`maxVersions` 默认取 `constants.DefaultCheckpointMaxVersions = 100`** —— 而 `NewCheckpointManager(0)` 或负数走默认。

**「裁剪只保留最近 100 个」** —— 于是**血缘链在超过 100 步之后断掉**：`GetTuple` 取父还能取到（只往前一步），但 `GetLineage` 能回到的最早节点是有限的。

### 六、`PutWrites`：用「版本链」代替「活动写表」

**这是本文件里最有信息量的注释：**

```
// PutWrites applies writes to a checkpoint with version-chain conflict detection.
// Uses a monotonic checkpoint version chain instead of a transient activeWrites map
// to avoid the TOCTOU race (activeWrites is cleared on success, allowing a stale
// concurrent writer to slip past undetected).
```

**「改用单调的检查点版本链，而不是一个临时的 `activeWrites` 映射，以避免 TOCTOU 竞态 —— `activeWrites` 在成功后就被清空，从而让一个过期的并发写者溜过去而未被发现。」**

**这段话记录了一次真实的修复，而它的技术内容是：**

| 方案 | 判据 | 缺陷 |
|---|---|---|
| `activeWrites` 表 | 「这个任务还有未完成的写吗」 | 成功后立即清空 → **窗口期内无法区分「已完成」与「从未开始」** |
| 版本链 | 「调用方加载的是不是当前最新」 | **有单调性，没有窗口** |

**「成功后清空」为什么会让过期写者溜过去？** 因为它造出了一个**三态**（未开始 / 进行中 / 已完成）压成**两态**（在表里 / 不在表里）。而两态无法区分「从没开始」与「已经成功」——**于是两者都会被当作「可以写」。**

**而版本链是两态但**有方向**：调用方持有的是一个具体的 `checkpoint_id`，若它不等于当前最新，则**一定是过期**（不可能有「从未开始」的歧义）。**

**这是「用单调量替代集合成员」的通用手法** —— 与 e02 那页 `ChannelVersions` 是同一思路（用版本号而不是「有没有变」的布尔）。

**而它的实现是一条「必须提供 checkpoint_id」的硬性前置：**

```
threadID := config.ThreadID
if threadID == "" {
	return fmt.Errorf("thread ID is required for put_writes")
}

current := checkpoints[len(checkpoints)-1]

callerID := config.GetOrEmpty("checkpoint_id")
if callerID == "" {
	return fmt.Errorf("checkpoint_id is required for put_writes on thread %s", threadID)
}
if callerID != current.ID {
	return &VersionConflictError{ ... }
}
```

**「缺 `checkpoint_id` 就拒绝」** —— 注释说得清楚：「若缺失，这次写就没有被正确地限定作用域、无法被校验」。**「无法校验就拒绝」比「无法校验就放行」正确得多。**

**然后它克隆当前检查点、升版本、把写应用上去：**

```
newCheckpoint := current.Clone()
newCheckpoint.Version = current.Version + 1
newCheckpoint.ParentID = current.ID

for _, write := range writes {
	newCheckpoint.State[write.Channel] = write.Value
	newCheckpoint.IncrementChannel(write.Channel)
}

cm.checkpoints[threadID] = append(cm.checkpoints[threadID], newCheckpoint)
```

**而这里有两处值得单独说：**

**其一，`newCheckpoint.State[write.Channel] = write.Value` 是**直接赋值**，完全绕过通道与 reducer。**

**这意味着：`PendingWrite.Overwrite` 这个字段在 `PutWrites` 里**被忽略**。无论 `Overwrite` 是 `true` 还是 `false`，写都是「直接覆盖」。**

**而在 LangGraph 里，待写重放是要穿过通道的** —— 于是「`Overwrite=false` 的写会被 reducer 合并」。**这里不是。** 两个后果：

- **`Overwrite` 字段在 `Manager` 这一层是死字段**（它只在 `BinaryOperatorAggregate.isOverwrite` 那里被识别，而那个识别处理的是**节点返回的写**，不是待写重放）。
- **待写重放与正常写路径的语义不同** —— 一个 `Overwrite=false` 的列表追加写，正常路径会 append，重放路径会替换。

**这是一个真实的语义缺陷，且它不报错** —— 表现为「从检查点恢复后，列表少了几项」。

**其二，`IncrementChannel` 只对 `ChannelVersions` 加一，**不**更新 `VersionsSeen`。**

**于是「应用了一次写之后，没有节点被标记为见过新版本」** —— 正确，因为还没有节点消费它。**而 `VersionsSeen` 的更新留给引擎那一层（见 e04）。**

**第三，`PutWrites` **没有** `maxVersions` 裁剪。**

**对比 `Save` 有裁剪 —— `PutWrites` 没有。** 于是「一个靠 `PutWrites` 持续写入的线程」会**无界增长**。**这份历史只在内存里（`CheckpointManager` 是纯内存的），所以后果是进程内存而不是存储成本。**

### 七、`deepCopy`：JSON round-trip，失败返回 nil

```
func deepCopy(val any) any {
	if val == nil { return nil }
	if m, ok := val.(map[string]any); ok { return deepCopyMap(m) }
	if slice, ok := val.([]any); ok { return deepCopySlice(slice) }

	// For all other types, JSON round-trip provides reliable deep copy.
	data, err := json.Marshal(val)
	if err != nil { return nil }
	var result any
	if err := json.Unmarshal(data, &result); err != nil { return nil }
	return result
}
```

**注释里有两句关键的话：**

> （把数字统一地转成 `float64`）……
> **序列化失败时（通道、函数、循环引用）返回 `nil` 而不是一个会静默在检查点与它的克隆之间传播变更的共享引用。**

**两个后果必须记住：**

**其一，`int` 会变成 `float64`。** 于是「状态里一个 `int` 字段」经过 `Clone`（进而经过 `ToMap` / `Put` 的 `deepCopyMap`）之后**类型变了**。**这在 Go 里是有实际影响的：** 之后若把它断言回 `int` 会失败。

**而 `Clone` 里 `State` 的每个值都走 `deepCopy`** —— 于是**「克隆一个检查点会改变状态值的数字类型」**。这是一个很隐蔽的行为变更。

**其二，`json.Marshal` 失败返回 `nil`。** 注释解释了为什么（宁可丢值也不要共享引用），**但 `nil` 是一个合法值** —— 于是「一个含函数的通道值」在克隆之后变成 `nil`，而**没有任何错误返回**。

**「宁可丢值也不要共享引用」这个优先级是对的**（共享引用会静默传播变更，那是更难查的缺陷）。**但「丢值」本身也是静默的** —— 它只在注释里被说明。

**而 `deepCopyMap` / `deepCopySlice` 是逐键/逐元素递归的，于是它们的复杂度随状态大小线性增长 —— 而 `Clone` 会被 `PutWrites` 与 `Load` / `List` / `GetTuple` 反复调用。**

**「每个读操作都克隆一份」是为了防调用方改到内部状态，但也意味着「读检查点」在大状态上是昂贵的。** 这是本文件没有讨论的成本。

### 八、两个实现：内存与 NATS，而 GC 是两层的

**`MemorySaver` 有两套索引，而它们存的东西不同：**

```
type MemorySaver struct {
	mu          sync.RWMutex
	checkpoints map[string]map[string]interface{}  // threadID -> 最新检查点
	versions    map[string][]checkpointEntry        // threadID -> 历史
}
```

**`Put` 同时写两处：**

```
s.versions[threadID] = append(s.versions[threadID], entry)
s.checkpoints[threadID] = deepCopyMap(checkpoint)
```

**于是 `checkpoints` 只保留最新一份（是「快查表」），而 `versions` 是完整历史。** 而 `Get` 优先走 `versions`：

```
if checkpointID, ok := config[constants.ConfigKeyCheckpointID].(string); ok {
	for _, entry := range versions { if entry.ID == checkpointID { return deepCopyMap(entry.Checkpoint), nil } }
	return nil, fmt.Errorf("checkpoint not found: %s", checkpointID)
}
versions := s.versions[threadID]
if len(versions) == 0 { return nil, nil }
return deepCopyMap(versions[len(versions)-1].Checkpoint), nil
```

**「带 `checkpoint_id` 就按 id 找、不带就取最新」** —— 与 `BaseCheckpointer` 的三方法契约对齐（`Get(ctx, config)` 的 `config` 里带不带 id 是两种模式）。

**而 `checkpoints` 这个 map 在 `Get` 里**根本没被用到** —— 它只在 `Put` 里被写。**于是它是一个**冗余索引**：唯一可能的用途是「O(1) 取最新」，而 `Get` 的实现选择了遍历 `versions`。**这是一个「索引建了但没用」的痕迹。**

**`List` 的注释与实现不一致：**

```
// List lists checkpoints for a thread.

// Return most recent checkpoints first
result := make([]map[string]interface{}, limit)
start := len(checkpoints) - limit
for i := 0; i < limit; i++ {
	result[i] = checkpoints[start+i].Clone()
}
```

**「先返回最近的」** —— 而实现是 `start+i` **升序**，即**在选出的窗口内从旧到新**。**窗口本身确实是最新的 `limit` 个**，但**顺序是旧的在前。**

**同一个缺陷在 `CheckpointManager.List` 里也存在**（注释也是「Return most recent checkpoints first」，实现也是升序）。**两处相同的注释-实现矛盾**，说明这大概是「先写注释、后改实现顺序」的产物。

**而 `NATSSaver` 的文档头把 GC 分两层讲清了：**

> **第一层 —— 每键版本管理（零人工）：** NATS KV 的 `History=N` 自动丢弃每个键的旧版本。每个图实例只保留最近 N 个检查点。这处理正常情况：活跃的图持续写，旧版本被 NATS 自然淘汰。
>
> **第二层 —— 已完成的图实例清理（后台）：** 当一个图执行完毕（或崩溃）后，它的键变成休眠。后台 GC 周期性扫描所有键，清掉「最新检查点早于 `MaxGraphIdle`」的那些。**一个休眠的键 = 一个已完成/被放弃的图实例。** 这防止孤儿检查点数据累积。

**「休眠键 = 已完成的实例」是一个用**时间**推断**生命周期**的判据** —— 因为没有显式的「完成」标记可查。**而它的成立前提写在注释里：「活跃的图每隔几秒就检查点一次，所以它们永远触发不了这个」。**

**而它列出的「每键一个图实例」这个模型与 `CheckpointManager` 完全不同：**

> 键格式：`{tenant_id}:{graph_instance_id}` —— 每个图实例有自己的键。一个「图实例」对应 Pregel 引擎里的一个执行线程。

**「一个线程一个键」而不是「一个线程一个键下的多版本」** —— 于是**版本历史被压进了 NATS KV 的 `History` 机制**，而 `Checkpoint` 结构里那套 `ParentID` / `Version` 血缘在 NATS 上就**只能靠 KV 的版本序号**表达。

**这是一个两种实现根本不同的证据：** 内存版存的是强类型 `*Checkpoint`（血缘可查），NATS 版存的是 `map[string]interface{}`（血缘需要自己拼）。

## 关键取舍

**源码事实：** 文件里有两个持久化抽象：`BaseCheckpointer`（三方法、`map` 进出、无并发控制，供 `graph`/`pregel` 用）与 `CheckpointManager`（15 方法、强类型 `*Checkpoint`、有 `VersionConflictError`）。前者是引擎真正调的，后者是内存权威实现 + 冲突检测器。

**源码事实：** `Checkpoint` 的八个字段中，`ChannelVersions` 与 `VersionsSeen` 是增量触发判断的全部依据，而它们**会被持久化** —— 于是重启后「哪些节点该重新触发」不必重算。

**源码事实：** `NewCheckpoint` 的 `Version` 从 **0** 开始且 `Metadata.ID == ID`（同一 UUID）；`Metadata.Source` 默认 `SourceNode`，而 `CheckpointSource` 有五个值（`node` / `edge` / `interrupt` / `manual` / `resume`）。

**源码事实：** `PendingWrite` 有六个字段（`Channel` / `Value` / `Overwrite` / `Node` / `Timestamp` / `TaskID`），而 **`ToMap` 只序列化四个 —— `Timestamp` 与 `TaskID` 被丢弃，`FromMap` 也就读不回来。** 后果：持久化后无法按任务去重待写，待写的先后顺序只能靠切片顺序。

**源码事实：** `Clone` 会造新 UUID 并设 `ParentID = c.ID`、`CreatedAt = time.Now()` —— **「克隆」同时是一次「派生」**；`PendingWrites` 用「先按值复制结构体、再单独深拷 `Value`」的手法，注释理由是「改克隆的 `Value` 永远不会影响原件」。

**源码事实：** `Metadata.ID: newID` 的注解是 `// same ID as the clone` —— 字面读起来像「与原件的 ID 相同」，实际意思是「与克隆体顶层 `ID` 相同」。代码对、注释容易误读。

**源码事实：** `MarkSeen` 记录的是通道**当前**版本号，于是 `HasSeen` 是**相等**判断（而非「大于等于」）；`HasSeen` 对不在 `VersionsSeen` 里的节点返回 `false`。

**源码事实：** `FromMap` 对 `created_at` 的解析失败**不报错**（静默保留零值）；三个 getter 只认特定类型，`getInt` **只认 `float64`**（为 JSON 而写），用在手工构造的 map 上会静默返回默认值。

**源码事实：** `CheckpointTuple` 只带 `ParentConfig`（怎么再取到父）而不是父检查点本身；`NewCheckpointTuple` 在 `checkpoint == nil` 时给出 `Metadata` 空 map 而非 nil；而 `GetTuple` 在无检查点时返回「nil 检查点的元组」、`Load` 返回 `nil, nil` —— **同包两处「无数据」表示不一致。**

**源码事实：** `CheckpointManager.Save` 两个检查：`ParentID` 非空且不符则冲突；**版本连续性只在 `checkpoint.Version > 0` 时检查**（`Version == 0` 是 `NewCheckpoint` 的默认值，视为「调用方不打算参与冲突检测」，于是跳过）。两个检查都可以被绕过，`Save` 在那种情况下退化成 append。

**源码事实：** `Save` 在超过 `maxVersions`（默认 `DefaultCheckpointMaxVersions = 100`）时裁掉老的，于是血缘链最多回溯 100 步；而 **`PutWrites` 没有裁剪** —— 靠它持续写入的线程会无界增长（内存）。

**源码事实：** `PutWrites` 的注释记录了一次真实修复：「用单调的检查点版本链替代临时的 `activeWrites` 映射，避免 TOCTOU —— `activeWrites` 在成功后清空，会让一个过期的并发写者溜过去未被发现」；它要求 `config` 里必须有 `checkpoint_id`（「缺了就无法限定作用域、无法校验」），不符则抛 `VersionConflictError`。

**源码事实：** `PutWrites` 应用写时是 `newCheckpoint.State[write.Channel] = write.Value` —— **直接赋值，完全绕过通道与 reducer，`PendingWrite.Overwrite` 字段在这一层被忽略**。于是「`Overwrite=false` 的列表追加写」在正常路径会 append、在待写重放路径会替换 —— 一个不报错的语义缺陷。

**源码事实：** `deepCopy` 对 `map[string]any` / `[]any` 递归处理，其余类型走 JSON round-trip；注释明说**「把数字统一转成 `float64`」**，以及**「序列化失败时返回 `nil` 而不是共享引用」**。后果是 `Clone` 会改变状态值的数字类型，且含函数/通道的值会静默变成 `nil`。

**源码事实：** `MemorySaver` 维护两套索引（`checkpoints` 只存最新、`versions` 存历史），而 **`Get` 从不读 `checkpoints`** —— 它是一个建了但没用的冗余索引。

**源码事实：** `MemorySaver.List` 与 `CheckpointManager.List` 的注释都写「Return most recent checkpoints first」，而两者实现都是**在选定窗口内升序**（旧的在前）—— 两处相同的注释-实现矛盾。

**源码事实：** `NATSSaver` 的文档头把 GC 分两层：第一层靠 NATS KV 的 `History=N`（默认 3）自动淘汰旧版本；第二层是后台扫描「最新检查点早于 `MaxGraphIdle`（默认 30 分钟）」的休眠键并整体清除，判据是「休眠键 = 已完成/被放弃的图实例」，其成立前提是「活跃的图每隔几秒就检查点一次」。键格式 `{tenant_id}:{graph_instance_id}`，一个图实例一个键。

**架构推论：** 这个文件最深的取舍是「**检查点必须能被序列化，而可序列化的东西必须是值而不是引用**」。`deepCopy` 的两种策略（递归 map/slice、其余走 JSON）都在服务这一条，而它的两个副作用（数字变 `float64`、无法序列化的值变 `nil`）都是这条纪律的代价。**在数据库里对应的是「缓冲区页面必须能被写到磁盘」** —— 任何持有指针的结构都不能进缓冲池。**而 ragflow 的解法比数据库软弱得多：它不拒绝不可序列化的值，而是把它们静默转成 `nil`。** 数据库会选择报错，因为「静默丢页」是不可接受的；而这里「静默丢一个状态字段」被接受了。

**架构推论：** `PendingWrite` 的「有损序列化」加「重放绕过 reducer」，两者合起来是一个**完整的待写重放缺陷**：`Overwrite` 标志丢了意义（因为重放总是覆盖）、`TaskID` 丢了（于是无法按任务去重）、`Timestamp` 丢了（于是顺序只靠切片）。**而这三件事都不是「没实现」，而是「实现了但没打通」** —— `ToMap` 少写两个键、`PutWrites` 少调一次 reducer。**这类缺陷的可怕之处在于每一处都看起来能工作**：正常路径完全正确，只有「崩溃后恢复」才暴露，而那时候现场已经没了。

**架构推论：** `Save` 的版本检查在 `Version == 0` 时跳过，是对「默认值参与校验」这个经典问题的让步。**当一个检查的判据恰好等于构造函数给的默认值，这个检查等于不存在** —— 因为最常见的构造路径永远不满足它。**正确的做法是让「未设置」与「设为 0」可区分**（`*int` 或一个哨兵），而这里选择了「把 0 解释成未设置」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/graph/checkpoint/checkpoint.go` | `BaseCheckpointer` | 三方法契约：`Get` / `Put` / `List` |
| `internal/harness/graph/checkpoint/checkpoint.go` | `Checkpoint` | 八字段持久化状态（含两套版本账） |
| `internal/harness/graph/checkpoint/checkpoint.go` | `NewCheckpoint` | `Version=0`、`Metadata.ID == ID` |
| `internal/harness/graph/checkpoint/checkpoint.go` | `Clone` | 造新 UUID + `ParentID`/`CreatedAt` 改写 + 逐层深拷 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `IncrementChannel` / `MarkSeen` / `HasSeen` | 版本账三操作（`HasSeen` 是相等判断） |
| `internal/harness/graph/checkpoint/checkpoint.go` | `AddPendingWrite` / `ClearPendingWrites` | 待写累积与清空（后者不用 `nil` 而用空切片） |
| `internal/harness/graph/checkpoint/checkpoint.go` | `ToMap` / `FromMap` | 有损往返：待写丢 `Timestamp` 与 `TaskID` |
| `internal/harness/graph/checkpoint/checkpoint.go` | `getString` / `getInt` / `getBool` | 为 JSON 而写的宽容 getter（`getInt` 只认 `float64`） |
| `internal/harness/graph/checkpoint/checkpoint.go` | `CheckpointMetadata` / `CheckpointSource` | 血缘元数据与五个来源值 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `PendingWrite` / `NewPendingWrite` | 六字段待写（序列化时丢两个） |
| `internal/harness/graph/checkpoint/checkpoint.go` | `CheckpointTuple` / `NewCheckpointTuple` | 检查点 + 父配置（`ParentConfig` 而非父检查点） |
| `internal/harness/graph/checkpoint/checkpoint.go` | `PutWrites` / `NewPutWrites` | 待写批 + 任务 id 的结构 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `CheckpointManager` | 内存权威实现 + 冲突检测 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `CheckpointManager.Save` | 两个检查（`Version==0` 时跳过第二个）+ 100 版裁剪 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `CheckpointManager.PutWrites` | 版本链 CAS + 直接赋值绕过 reducer + 不裁剪 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `CheckpointManager.Load` / `LoadByCheckpointID` | 「无数据」返回 `nil, nil` |
| `internal/harness/graph/checkpoint/checkpoint.go` | `CheckpointManager.List` / `GetTuple` / `GetTupleByVersion` | 列表（注释与顺序矛盾）与元组 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `CheckpointManager.GetLineage` / `Delete` / `ClearThread` / `ClearAll` | 血缘遍历与清理 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `VersionConflictError` | 带四字段的冲突错误 |
| `internal/harness/graph/checkpoint/checkpoint.go` | `deepCopy` / `deepCopyMap` / `deepCopySlice` | JSON round-trip 深拷，失败返 `nil` |
| `internal/harness/graph/checkpoint/checkpoint.go` | `RunnableConfigToMap` | 单一适配点：`thread_id` / `checkpoint_id` / `checkpoint_ns` |
| `internal/harness/graph/checkpoint/memory.go` | `MemorySaver` / `NewMemorySaver` | 两套索引（`checkpoints` 未被读） |
| `internal/harness/graph/checkpoint/memory.go` | `checkpointEntry` / `CheckpointState` | 历史条目与「检查点+元数据」 |
| `internal/harness/graph/checkpoint/nats.go` | `NATSSaver` / `NewNATSSaver` | 一个线程一个键 + 两层 GC |
| `internal/harness/graph/checkpoint/nats.go` | `NATSConfig` / `defaults` | `History=3` / `MaxGraphIdle=30m` / `GCInterval=10m` |
| `internal/harness/graph/constants/constants.go` | `DefaultCheckpointMaxVersions` / `DefaultCheckpointListLimit` | 100 / 10 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：checkpoint/checkpoint.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/graph/checkpoint/checkpoint.go)
- [LangGraph 文档：checkpointers 与 persistence](https://langchain-ai.github.io/langgraph/concepts/persistence/)
- [NATS JetStream 文档：Key/Value Store](https://docs.nats.io/nats-concepts/jetstream/key-value-store)
- [Go 文档：encoding/json](https://pkg.go.dev/encoding/json)

## 与主核 / AI 的关系

检查点与主核的**日志（WAL）** 是两个都叫「崩溃后能恢复」的东西，而它们的记录粒度正好相反。

**WAL 记**操作**（重放得到状态），检查点记**状态快照**（直接读回）。** 而 ragflow 这里的 `Checkpoint` 里**同时**有两者的痕迹：`State` 是快照，`PendingWrites` 是待重放的操作。

**`PendingWrite` 与 WAL 的比较值得单独看：**

| | WAL 记录 | `PendingWrite` |
|---|---|---|
| 内容 | 「改了这个页的这个偏移」 | 「这个通道要写这个值」 |
| 何时写 | **在改数据之前**（保证一致性） | **在节点算完之后**（保证不重算） |
| 重放语义 | 幂等（写同一个值） | **取决于 reducer** |
| 丢弃条件 | 提交点之后 | 超步收尾之后 |

**最后一行是分歧的根源。** WAL 的重放语义天然幂等（它就是「把值写到那里」），所以 WAL 可以随便重放。**而 `PendingWrite` 的重放要穿过 reducer** —— 一个「追加」型的写重放两次就会变成两条。**这就是为什么 LangGraph 那边要区分「这条写是否 `Overwrite`」，而 ragflow 这边干脆绕过 reducer（见上面 `PutWrites` 直接赋值）。**

**「绕过 reducer 保正确性、但丢失归并语义」是一个典型的二选一。** 而它选的这一支在列表型状态上是错的。

**`VersionConflictError` 与数据库的 `serialization_failure` 是同一个东西** —— 都是「你的读已经过期了」。**而数据库的重试责任在客户端（捕获错误、回滚、重来），这里的责任也在调用方（`Save` / `PutWrites` 返回错误、调用方重新 `Load`）。**

**而「用版本链替代活动写表」这个修复，在数据库里对应的是「用 `xmin` 而不是 `pg_locks` 判断元组的可见性」** —— 前者是持久化的单调量，后者是易失的运行时状态。**任何「靠一个会被清空的表来判断过去发生了什么」的设计都有同一个缺陷：清空失去了「曾发生过」的信息。**

**两层的 NATS GC 在数据库里对应「段文件的回收」**：活跃段不回收、关闭的段进回收队列。**而 ragflow 用「最后一次写入距今多久」推断「这个图还活着吗」，数据库用显式的关闭标记（`xlog` 段切换）。** 前者简单但会误判（一个长时间不写检查点的真实活跃图会被当成死掉），后者准确但要维护状态。

对 agent 工程的直接价值有三条。其一，**「已算但未提交」这个中间态必须持久化，否则崩溃会导致重复执行**。而持久化它比持久化快照更难，因为要处理「重放语义」——**幂等性必须显式设计。** 其二，**序列化边界上的丢字段是最难发现的缺陷**。`ToMap` 少写两个键、`FromMap` 静默读默认值，两处都不报错，而后果只在恢复路径上出现。**任何 `ToMap`/`FromMap` 对必须逐字段对照，最好用一个「往返相等」的测试钉住。** 其三，**让「未设置」与「设置为默认值」可区分**。`Version == 0` 被解释成「未设置」，于是版本检查在最常见的构造路径上等于不存在——**一个检查的判据若与构造函数默认值重合，它就不是检查。**
