---
project: ragflow
feature: graph-api
title: 状态图装配接口
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/graph/graph/graph.go
    symbols: [NewStateGraph, WithCheckpointer, WithInterrupts, MapFields]
  - path: internal/harness/harness.go
    symbols: [NewStateGraph, NewMemorySaver, DefaultRetryPolicy]
---

# 状态图装配接口

## 一句话职责

把「节点 + 边 + 通道」组装成一张可编译的图：`AddNode` 拿回一个可继续改的节点句柄、`AddEdge` 在装配期就校验端点存在、`Compile` 把六个编译选项烙进一个 `compiledGraph`，而真正的执行被推给一个**通过全局变量安装**的 `PregelRunFunc`。

## 机制是什么

`stateGraph` 的字段就是这张图的全部状态，而它值得逐个看一遍：

```
type stateGraph struct {
	nodes            map[string]*types.Node
	edges            []*types.Edge
	dataEdges        []*types.DataEdge
	conditionalEdges []*types.ConditionalEdge
	branches         []*types.Branch
	entryPoint       string
	finishPoints     []string
	channels         map[string]channels.Channel
	reducers         map[string]types.ReducerFunc
	stateSchema      interface{}
	inputSchema      interface{}
	outputSchema     interface{}
	NodeTriggerMode  types.NodeTriggerMode
}
```

**四种边、三个 schema、一个通道表、一个 reducer 表。** 这个字段清单本身就是 graphrag/harness 的世界观：

| 概念 | 载体 | 语义 |
|---|---|---|
| 控制流（无条件） | `edges` | A 完成后走 B |
| 控制流（按条件） | `conditionalEdges` | A 完成后按条件走映射表里的某个目标 |
| 控制流（动态多路） | `branches` | A 完成后由函数返回一个目标列表 |
| 数据流 | `dataEdges` | A 的输出按字段路径映射进 B |
| 状态 | `channels` + `reducers` | 每个状态字段是一条通道，写入按归并语义合并 |
| I/O 契约 | `stateSchema` / `inputSchema` / `outputSchema` | 三个都可能不同 |

**「四种边」是这个接口最容易被低估的部分。** 大多数人只用 `AddEdge` 与 `AddConditionalEdges`；而 `AddDataEdge` 与 `AddBranch` 各解决一类 `AddConditionalEdges` 表达不了的情况：

- **`AddDataEdge` 不只是「走这条边」，它还带一组 `(From, To)` 字段映射** —— 于是「A 的输出经过变换后进入 B 的某个字段」是一等公民，不需要在节点里手写搬运代码。
- **`AddBranch` 的 `then` 是 `func(interface{}) []string`** —— 返回**列表**而不是单目标。于是「一条件触发多路」是原生支持的，而 `AddConditionalEdges` 的映射表只能给一个目标。

**而三个 schema 的默认值是同一个对象：**

```
stateSchema:      stateSchema,
inputSchema:      stateSchema,
outputSchema:     stateSchema,
```

**只有显式 `WithInputSchema` / `WithOutputSchema` 才让它们分化。** 于是「输入输出是状态的子集」这件事默认不成立 —— 默认是全部相等。

## 怎么实现的

### 一、`AddNode` 返回句柄，`AddNodeWithOptions` 才吃选项

**两个方法的关系是「后者是前者的超集」。**

```
func (g *stateGraph) AddNode(name string, fn types.NodeFunc) *types.Node {
	node := &types.Node{
		Name: name, Function: fn,
		Triggers: make([]string, 0), Writes: make([]string, 0),
		Tags: make([]string, 0), Metadata: make(map[string]interface{}),
	}
	g.nodes[name] = node
	return node
}
```

**返回 `*types.Node` 是一个「可变的句柄」设计** —— 调用方可以拿到它再逐个设字段：

```
node := builder.AddNode("x", fn)
node.RetryPolicy = &policy
```

**而 `AddNodeWithOptions` 提供的是另一条路：把选项塞进 `NodeOptions` 结构。**

```
func (g *stateGraph) AddNodeWithOptions(name string, fn types.NodeFunc, opts types.NodeOptions) *types.Node {
	if opts.StatePre != nil || opts.StatePost != nil {
		orig := fn
		pre := opts.StatePre
		post := opts.StatePost
		fn = func(ctx context.Context, state interface{}) (interface{}, error) {
			if pre != nil {
				state, err = pre(ctx, state)
				if err != nil { return nil, fmt.Errorf("state pre-handler for '%s': %w", name, err) }
			}
			out, err := orig(ctx, state)
			if err != nil { return nil, err }
			if post != nil {
				out, err = post(ctx, out)
				if err != nil { return nil, fmt.Errorf("state post-handler for '%s': %w", name, err) }
			}
			return out, nil
		}
	}
	node := g.AddNode(name, fn)
	if opts.RetryPolicy != nil { node.RetryPolicy = opts.RetryPolicy }
	if len(opts.Tags) > 0     { node.Tags = append(node.Tags, opts.Tags...) }
	if len(opts.Metadata) > 0 { for k, v := range opts.Metadata { node.Metadata[k] = v } }
	if len(opts.Triggers) > 0 { node.Triggers = append(node.Triggers, opts.Triggers...) }
	if len(opts.Writes) > 0   { node.Writes = append(node.Writes, opts.Writes...) }
	if len(opts.FieldMapping) > 0 { node.FieldMapping = append(node.FieldMapping, opts.FieldMapping...) }
	return node
}
```

**函数包裹是「在装配期做」而不是「在执行期做」** —— 注释写得很清楚，这是三个 override 函数（`WithStatePreHandler` / `WithStatePostHandler` / `WithFieldMapping`）的落点：

```
func WithStatePreHandler(fn types.NodeFunc) func(*types.NodeOptions) {
	return func(opts *types.NodeOptions) { opts.StatePre = fn }
}
```

**注意这三个函数返回的是「接受 `*NodeOptions` 的函数」而不是 `NodeOptions` 本身。** 而它们**并不被 `Compile` 使用** —— `Compile` 只吃 `CompileOption`（`func(*compiledGraph)`）。**这是一套未接线的选项机制：调用方需要自己把它们应用到 `NodeOptions` 上再传给 `AddNodeWithOptions`。**

**包裹顺序是「pre → 节点 → post」，而 `StatePre` / `StatePost` 的类型都是 `NodeFunc`** —— 意味着它们**可以做任何事**，不只是变换。**这是一个刻意的宽松：名字说「handler」，类型说「完整节点」。**

**而两处错误包装都带上了节点名** —— 于是多层包裹之后仍能定位到出错的那个节点。

**`AddNode` 直接覆盖同名节点而不报错** —— `g.nodes[name] = node` 是赋值，不是「已存在则拒绝」。**这与 `AddEdge` 的严格校验形成对比**（下面会看到）。

### 二、`AddEdge`：装配期就把端点校验掉

```
func (g *stateGraph) AddEdge(from, to string) error {
	if _, ok := g.nodes[from]; !ok && from != constants.Start {
		return &errors.NodeNotFoundError{NodeName: from}
	}
	if _, ok := g.nodes[to]; !ok && to != constants.End {
		return &errors.NodeNotFoundError{NodeName: to}
	}
	g.edges = append(g.edges, &types.Edge{From: from, To: to})
	if from == constants.Start { g.entryPoint = to }
	if to == constants.End {
		if !slices.Contains(g.finishPoints, from) { g.finishPoints = append(g.finishPoints, from) }
	}
	return nil
}
```

**四个要点：**

1. **`__start__` 与 `__end__` 是两个例外** —— 它们不是真实节点，所以校验时先排除。
2. **`AddEdge(Start, X)` 同时定义入口点** —— 于是「图从哪开始」不必单独调用 `SetEntryPoint`。
3. **`AddEdge(X, End)` 同时登记一个结束点** —— 而**多个节点可以连到 `End`**（`slices.Contains` 去重后 append）。
4. **校验在装配期而不是编译期** —— 于是「拼错的节点名」在 `AddEdge` 那一行就报出来，而不是等到 `Compile`。

**第 3 点值得单独说：`finishPoints` 是一个**列表**而不是单值。** 于是「多个出口」是原生支持的，而这正是「条件分支各自走到 End」的形态。

**`AddConditionalEdges` 校验的是**映射表里的所有目标**。**

```
func (g *stateGraph) AddConditionalEdges(from string, condition types.EdgeFunc, mapping map[string]string) error {
	if _, ok := g.nodes[from]; !ok { return &errors.NodeNotFoundError{NodeName: from} }
	for _, target := range mapping {
		if _, ok := g.nodes[target]; !ok && target != constants.End {
			return &errors.NodeNotFoundError{NodeName: target}
		}
	}
	g.conditionalEdges = append(g.conditionalEdges, &types.ConditionalEdge{From: from, Condition: condition, Mapping: mapping})
	return nil
}
```

**`from` 不能是 `Start`（因为这一支没有 `&& from != constants.Start`）** —— 也就是说**入口节点不能是条件分支的源**，入口必须由 `AddEdge(Start, X)` 确定。这是一个真实的约束。

**而 `AddBranch` 完全没法校验目标**：

```
func (g *stateGraph) AddBranch(from string, condition types.EdgeFunc, then func(interface{}) []string) error {
	if _, ok := g.nodes[from]; !ok { return &errors.NodeNotFoundError{NodeName: from} }
	g.branches = append(g.branches, &types.Branch{From: from, Condition: condition, Then: then})
	return nil
}
```

**`then` 的返回目标在运行期才知道，所以只能校验 `from`。** 这是一个不可避免的取舍：**表达能力越强的边，装配期校验越弱。**

**而 `AddDataEdge` 与 `AddEdge` 的校验完全一致**（都允许 `Start` / `End`），只是多存了 `Mapping`。

### 三、`Validate`：三道门，其中一道是 BFS 可达性

```
func (g *stateGraph) Validate() error {
	if g.entryPoint == "" { return fmt.Errorf("no entry point set") }
	if len(g.finishPoints) == 0 { return fmt.Errorf("no finish points set") }
	reachable := g.computeReachable()
	for name := range g.nodes {
		if !reachable[name] { return fmt.Errorf("node %s is not reachable from entry point", name) }
	}
	if err := g.ValidateStateSchema(); err != nil { return fmt.Errorf("state schema validation failed: %w", err) }
	return nil
}
```

**第三道门「每个节点都必须可达」是一个强约束，而它的实现是 BFS：**

```
queue := []string{g.entryPoint}
reachable[g.entryPoint] = true
for len(queue) > 0 {
	current := queue[0]; queue = queue[1:]
	for _, edge := range g.edges {
		if edge.From == current && !reachable[edge.To] && edge.To != constants.End {
			reachable[edge.To] = true; queue = append(queue, edge.To)
		}
	}
	for _, condEdge := range g.conditionalEdges {
		if condEdge.From == current {
			for _, target := range condEdge.Mapping {
				if _, ok := g.nodes[target]; ok && !reachable[target] && target != constants.End {
					reachable[target] = true; queue = append(queue, target)
				}
			}
		}
	}
}
```

**两个值得记的点：**

1. **BFS 只走 `edges` 与 `conditionalEdges`，**不走 `branches` 与 `dataEdges`。** 于是 `AddBranch` 的目标不在可达性计算里 —— 而 `AddDataEdge` 的目标更彻底（它连控制流都不算）。**这是「四种边」的一个隐藏后果：可达性检查只覆盖两种。**
2. **队列用 `queue[0]` + `queue[1:]` 切片** —— O(n²) 的 BFS，但对几十个节点完全够。**这不是性能问题，而是「简单代码优先」的选择。**

**「每个节点都必须可达」这条约束相当严。** 它拒绝「定义了但不用的节点」，好处是它拦住「拼错了节点名导致那个节点永远不会被触发」——**而坏处是任何「预留节点」（比如某个条件分支只在特定配置下可达）都无法登记。**

**第四道门 `ValidateStateSchema` 需要看 state.go（下一节）。**

### 四、schema → 通道：反射出来的默认归并语义

**`Validate` 的最后一道门会走 `validateStateSchema`，而那个函数是「自动建通道」的来源。**

```
switch v.Kind() {
case reflect.Struct:
	t := v.Type()
	for i := 0; i < t.NumField(); i++ {
		field := t.Field(i)
		if field.PkgPath != "" { continue }   // Unexported field
		info, err := processField(field)
		if err != nil { return nil, fmt.Errorf("field %s: %w", field.Name, err) }
		fieldInfos[info.Name] = info
	}
case reflect.Map:
	t := v.Type()
	if t.Key().Kind() != reflect.String { return nil, fmt.Errorf("state schema map must have string keys") }
	// For maps, we can't extract field annotations statically.
default:
	return nil, fmt.Errorf("state schema must be a struct or map, got %v", v.Kind())
}
```

**三个要点：**

1. **`field.PkgPath != ""` 用来跳过未导出字段** —— Go 的反射不给未导出字段设 `PkgPath` 之外的访问能力，所以「未导出的状态字段」根本不能参与建通道。
2. **`map` 形式的 schema 只校验键类型是字符串，然后什么都不做** —— 注释明说「对 map，我们无法静态提取字段注解」「实践中通道是通过 `AddChannel` 动态添加的」。**于是 map schema 是一个「靠人工 AddChannel」的形态，不是一个真的 schema。**
3. **schema 可以是 `struct` 或指向 `struct` 的指针**（前面有 `if v.Kind() == reflect.Ptr { v = v.Elem() }`）。

**而 `processField` 做的事是「读 struct tag → 建通道」。**

```
tag := field.Tag.Get("harness")
if tag != "" {
	annotation, err := parseAnnotation(tag)
	if err != nil { return nil, fmt.Errorf("invalid annotation: %w", err) }
	info.Annotation = annotation
}
var reducer types.ReducerFunc
if info.Annotation != nil && info.Annotation.Reducer != nil { reducer = info.Annotation.Reducer }
channel, err := channels.CreateReducerChannel(field.Name, field.Type, reducer)
```

**tag 的格式是 `reducer=add` 或 `reducer=custom,meta=value`：**

```
pairs := strings.SplitSeq(tag, ",")
for pair := range pairs {
	kv := strings.SplitN(pair, "=", 2)
	if len(kv) != 2 {
		annotation.Metadata[pair] = true   // Could be a boolean flag
		continue
	}
	switch strings.TrimSpace(kv[0]) {
	case "reducer":
		reducer, ok := reducers[value]
		if !ok { return nil, fmt.Errorf("unknown reducer: %s", value) }
		annotation.Reducer = reducer
	default:
		annotation.Metadata[key] = value
	}
}
```

**三个决定：**

1. **只有 `reducer` 是「被理解的键」，其它键全部进 `Metadata`** —— 于是 tag 可扩展而不需要改解析器。
2. **`reducer` 的值必须命中三个内置名之一**（`add` / `append` / `merge`），否则**报错** —— 这是本页唯一「拼错就炸」的地方，而它是对的：**一个拼错的 reducer 名会静默退化成「覆盖写」，而覆盖写不会报错、只会丢数据。**
3. **`len(kv) != 2` 时当作布尔标志** —— 于是 `harness:"nostream"` 这种写法合法。

**而 `channels.CreateReducerChannel(field.Name, field.Type, reducer)` 决定**没**声明 reducer 时的默认语义，这是全页最重要的一节。**

```
switch fieldType.Kind() {
case reflect.Slice:
	operator := BinaryOperator(func(a, b interface{}) interface{} {
		return reflect.AppendSlice(reflect.ValueOf(a), reflect.ValueOf(b)).Interface()
	})
	if fieldType == reflect.TypeFor[[]any]() { operator = ListAppend }
	if reducer != nil { operator = BinaryOperator(reducer) }
	channel = NewBinaryOperatorAggregate(reflect.Zero(fieldType).Interface(), operator)
	channel.SetKey(fieldName)
	return channel, nil
case reflect.Array:
	channel = NewBinaryOperatorAggregate(fieldType, ListAppend)
case reflect.Map:
	channel = NewBinaryOperatorAggregate(fieldType, func(a, b interface{}) interface{} {
		if aMap, ok := a.(map[string]interface{}); ok {
			if bMap, ok := b.(map[string]interface{}); ok {
				result := make(map[string]interface{}, len(aMap)+len(bMap))
				maps.Copy(result, aMap); maps.Copy(result, bMap)
				return result
			}
		}
		return b
	})
case reflect.Int, ..., reflect.Float64:
	// Pass reflect.Zero(fieldType).Interface() instead of fieldType itself
	// so createZeroValue uses the correct underlying numeric type for its
	// reflect.Zero call, not the reflect.Type descriptor.
	channel = NewBinaryOperatorAggregate(reflect.Zero(fieldType).Interface(), IntAdd)
default:
	channel = NewLastValue(fieldType)
}
```

**于是「状态字段的类型决定默认归并语义」成了一条隐式规则：**

| 字段类型 | 默认通道 | 意味着 |
|---|---|---|
| 切片 | `BinaryOperatorAggregate` + append | **追加** |
| 数组 | 同上（但用 `ListAppend`） | 追加 |
| map | `BinaryOperatorAggregate` + merge | **合并** |
| 数字 | `BinaryOperatorAggregate` + `IntAdd` | **累加** |
| 其它 | `LastValue` | **覆盖** |

**这张表是 harness 版的「约定优于配置」，而它有两个值得注意的后果：**

**其一，`LastValue` 只接受每步一个写入。**

```
if len(values) > 1 {
	return false, &errors.InvalidUpdateError{
		Message: "Can receive only one value per step. Use a reducer to handle multiple values.",
	}
}
```

**于是「两个并行节点同时写同一个标量字段」会**报错** —— 而这条错误消息正好指导了修法（加一个 reducer）。

**其二，`Slice` 分支有一处可以谈的细节：**

```
if fieldType == reflect.TypeFor[[]any]() { operator = ListAppend }
if reducer != nil { operator = BinaryOperator(reducer) }
```

**当字段是 `[]any` 时用 `ListAppend`（能处理任意元素），而具体类型的切片用 `reflect.AppendSlice`。** 而**两处 `if` 的顺序意味着「显式 reducer 覆盖一切」** —— 包括覆盖掉 `[]any` 的 `ListAppend`。

**而 `numeric` 那一段的注释是一个真实的反射陷阱：**

> 传 `reflect.Zero(fieldType).Interface()` 而不是 `fieldType` 本身，**这样 `createZeroValue` 用的是正确的底层数值类型而不是 `reflect.Type` 描述符。**

**「把类型造一个零值传进去」而不是传类型本身** —— 因为下游要用那个值调 `reflect.Zero`。**这是一个「反射 API 需要值而代码手里只有类型」的通用适配。**

**`configureChannelsFromSchema` 把这些通道灌进图，而它的顺序值得注意：**

```
for fieldName, info := range fieldInfos {
	if _, exists := g.channels[fieldName]; !exists {
		g.channels[fieldName] = info.Channel
	}
	if info.Annotation != nil && info.Annotation.Reducer != nil {
		g.reducers[fieldName] = info.Annotation.Reducer
	}
}
```

**「已存在的通道不被覆盖」** —— 于是显式 `AddChannel` 优先于 schema 推导。**这给「schema 推导错了就手工覆盖」留了出口。**

**而它是 `Compile` 的第一步之一：**

```
func (g *stateGraph) Compile(opts ...interface{}) (types.CompiledGraph, error) {
	if err := g.Validate(); err != nil { return nil, fmt.Errorf("graph validation failed: %w", err) }
	if err := g.configureChannelsFromSchema(); err != nil { return nil, fmt.Errorf("failed to configure channels from schema: %w", err) }
	...
```

**「先 Validate 再 configureChannels」意味着 `Validate` 里的 `ValidateStateSchema` 已经跑了一遍反射，而 `configureChannelsFromSchema` 又跑一遍。** 两次反射是重复开销 —— 但换来的是「验证阶段的错误信息更早、更明确」。

### 五、`Compile`：`...interface{}` 与一个被静默忽略的参数

**`Compile` 在 `types.StateGraph` 接口里的签名是：**

```
Compile(opts ...interface{}) (CompiledGraph, error)
```

**而实现在内部做类型断言：**

```
for _, opt := range opts {
	if fn, ok := opt.(CompileOption); ok {
		fn(cg)
	}
}
```

**这是一个**类型擦除**：`CompileOption` 定义在 `graph` 包（`func(*compiledGraph)`），而 `compiledGraph` 是**非导出的**。于是 `types` 包无法声明 `Compile(opts ...CompileOption)` —— 它根本引不到那个类型。**

**而代价是**两个**：**

1. **传错类型的选项被静默忽略** —— 没有 `else` 分支报错。于是 `WithRecursionLimit(10)` 若因为某种原因不是 `CompileOption`（比如被包成了别的函数类型），它**什么都不做且无提示**。
2. **调用方无法从签名看出「该传什么」** —— `harness` 包把 `WithCheckpointer` / `WithInterrupts` 等 re-export 出去，于是文档成了唯一线索。

**`compiledGraph` 的六个字段：**

```
type compiledGraph struct {
	graph           *stateGraph
	checkpointer    interface{}            // checkpoint.BaseCheckpointer
	interrupts      map[string]bool
	interruptsAfter map[string]bool
	recursionLimit  int
	debug           bool
	nodeTriggerMode types.NodeTriggerMode
}
```

**注意 `checkpointer` 的类型是 `interface{}`，注释写着 `// checkpoint.BaseCheckpointer`** —— **又一次类型擦除，理由相同：`types` 包不能依赖 `checkpoint` 包（会成环）。** 于是 `WithCheckpointer(checkpointer interface{})` 接受任何东西，而真正的类型断言发生在 pregel 那一层：

```
cp, _ := cg.GetCheckpointer().(checkpoint.BaseCheckpointer)
```

**`_` 丢掉了断言的 `ok`。** 于是「传了一个不是 `BaseCheckpointer` 的东西」会得到一个 `nil` 的检查点器，而 pregel 的行为取决于它怎么处理 `nil`（见 e04 那页的三种持久化档位）。**这是一个值得记的静默降级点。**

**默认值四个：**

```
recursionLimit:  constants.DefaultRecursionLimit,   // 50
nodeTriggerMode: types.NodeTriggerAnyPredecessor,
interrupts:      make(map[string]bool),
interruptsAfter: make(map[string]bool),
```

**而 `WithInterrupts` 支持 `constants.All`？** 看实现：

```
func WithInterrupts(nodes ...string) CompileOption {
	return func(cg *compiledGraph) {
		for _, node := range nodes { cg.interrupts[node] = true }
	}
}
```

**它只是把每个名字设成 `true`。** 而 `types/types.go` 里有 `const All = "*"`，注释说「用于表示图应在所有节点上中断」。**于是 `WithInterrupts("*")` 会设一个名为 `"*"` 的键，而**是否真的被解释成「所有节点」取决于 pregel 那边有没有处理这个哨兵**。这是一个「哨兵值跨包传递、解释权在下游」的脆弱点。

**而 `Compile` 的最后一行把 trigger mode 写回 `stateGraph`：**

```
g.NodeTriggerMode = cg.nodeTriggerMode
```

**于是「编译后的模式」被记在**未编译的图**上。** 若同一张 `stateGraph` 被 `Compile` 两次且选项不同（比如一次 `WithNodeTriggerMode(All)` 一次不传），**第二次的默认会覆盖第一次的，而第一次编译出的 `compiledGraph.nodeTriggerMode` 仍保留旧值。** 于是两个 `compiledGraph` 的行为不一致而同源 —— 一个共享可变状态的副作用。

**`Invoke` 与 `Stream` 是两条几乎对称的入口：**

```
func (cg *compiledGraph) Invoke(ctx context.Context, input interface{}, config ...*types.RunnableConfig) (interface{}, error) {
	rc := &types.RunnableConfig{}
	if len(config) > 0 && config[0] != nil { rc = config[0] }
	return cg.run(ctx, input, rc, types.StreamModeValues)
}
```

**注意 `Invoke` 用的是 `StreamModeValues`** —— 于是「非流式调用」实际上是「以 values 模式跑一次流」。**与 g01 那页 graphrag 的「非流式 = 消费流式」是完全同一个决定。**

**而 `Stream` 起一个 goroutine，把 `cg.run` 的单一返回值塞进一个容量 1 的 channel：**

```
outputCh := make(chan interface{}, 1)
errCh := make(chan error, 1)
go func() {
	defer close(outputCh); defer close(errCh)
	result, err := cg.run(ctx, input, rc, mode)
	if err != nil {
		select { case errCh <- err: case <-ctx.Done(): }
		return
	}
	select { case outputCh <- result: case <-ctx.Done(): }
}()
return outputCh, errCh
```

**「一个 goroutine + 容量 1 的 channel + 两个 select 带 `ctx.Done()` 出口」** —— 这是「把一个返回值转成 channel」的最小实现，而**两个 select 都带 `ctx.Done()` 分支是为了防「调用方不读 channel 导致 goroutine 永久阻塞」。**

### 六、`run` 与那个全局函数指针

```
func (cg *compiledGraph) run(ctx context.Context, input interface{}, config *types.RunnableConfig, streamMode types.StreamMode) (interface{}, error) {
	if types.PregelRunFunc == nil {
		return nil, fmt.Errorf("graph: pregel engine not installed")
	}
	return types.PregelRunFunc(ctx, cg, input, config, streamMode)
}
```

**`PregelRunFunc` 是一个 `var`：**

```
// PregelRunFunc is the pluggable pregel execution function.
// Set by pregel.init() via SetPregelRunFunc.
var PregelRunFunc func(ctx context.Context, cg CompiledGraph, input interface{}, config *RunnableConfig, streamMode StreamMode) (interface{}, error)
```

**而它被谁设置？`harness.go` 的空白导入：**

```
_ "ragflow/internal/harness/graph/pregel" // triggers pregel.init() → sets PregelRunFunc
```

**这一行是整页机制的要害。** 它解决的是一个**循环依赖**：`pregel` 包要用 `graph` 包（它执行 `compiledGraph`），而 `graph` 包要调用 `pregel`。于是：

- `graph` 声明一个函数指针变量，`run` 时检查非 nil；
- `pregel` 在 `init()` 里调 `SetPregelRunFunc` 把自己装进去；
- `harness` 用空白导入保证「只要有人用 harness，pregel 就被链接进来」。

**而 `init_pregel.go` 就是被装进去的那个函数：**

```
ctx context.Context, cg types.CompiledGraph, input interface{}, config *types.RunnableConfig, streamMode types.StreamMode,
) (interface{}, error) {
	interruptKeys := make([]string, 0, len(cg.GetInterrupts()))
	for k := range cg.GetInterrupts() { interruptKeys = append(interruptKeys, k) }
	interruptAfterKeys := make([]string, 0, len(cg.GetInterruptsAfter()))
	for k := range cg.GetInterruptsAfter() { interruptAfterKeys = append(interruptAfterKeys, k) }

	cp, _ := cg.GetCheckpointer().(checkpoint.BaseCheckpointer)
	engine := NewEngine(cg.GetGraph(),
		WithCheckpointer(cp),
		WithInterrupts(interruptKeys...),
		WithInterruptsAfter(interruptAfterKeys...),
		WithRecursionLimit(cg.GetRecursionLimit()),
		WithDebug(cg.IsDebug()),
		WithConfig(config),
	)
	return engine.RunSync(ctx, input)
}
```

**这一段是「编译期选项」到「执行期引擎」的翻译表，而它暴露出一个事实：`compiledGraph` 上存的东西，全部被搬进了一个 `Engine`。**

**注意它**丢掉**了 `streamMode` —— `RunSync` 不接受它。** 于是 `Stream` 传入的 `mode` 在 `RunSync` 这条路径上不起作用；而 `CompiledGraph.Stream` 返回的 channel 里只有一个最终值（就是 `RunSync` 的结果），**并不是「按 mode 分级的流式事件」**（那要等 e05 那页的 `StreamManager`）。**这是一个接口与实际行为的落差：`Stream(ctx, input, mode)` 的 `mode` 在这里没有落地。**

**而 `GetInterrupts` 是一个 `map[string]bool`，要转成 `[]string` 才能传给 `WithInterrupts`** —— 于是「集合」与「变参」两种表示各一次往返。**而 `interruptKeys` 的遍历顺序是 map 的随机顺序**，不过因为只是设布尔值，顺序无关。

### 七、`harness.go`：一层 re-export 墙

**`harness.go` 有 463 行，而它几乎不含逻辑 —— 全是类型别名、函数变量、常量转发。**

**三个值得单看的东西：**

**第一，`NewStateGraph` 是唯一有函数体的转发之一：**

```
func NewStateGraph(stateSchema interface{}) StateGraph {
	return graph.NewStateGraph(stateSchema)
}
```

**为什么这个是函数而不是 `var NewStateGraph = graph.NewStateGraph`？** 因为它**需要返回 `types.StateGraph` 接口而不是 `*stateGraph`** —— 而 `graph.NewStateGraph` 的返回类型正是 `types.StateGraph`，所以其实 `var` 也行。**这个不一致大概是无意的。**

**对比同文件的 `var` 转发：**

```
var (
	WithCheckpointer     = graph.WithCheckpointer
	WithInterrupts       = graph.WithInterrupts
	WithInterruptsAfter  = graph.WithInterruptsAfter
	WithRecursionLimit   = graph.WithRecursionLimit
	WithDebug            = graph.WithDebug
	WithNodeTriggerMode  = graph.WithNodeTriggerMode
)
```

**第二，`DefaultRetryPolicy` 也是函数：**

```
func DefaultRetryPolicy() RetryPolicy {
	return types.DefaultRetryPolicy()
}
```

**而它返回的策略是：**

```
InitialInterval: 500 * time.Millisecond,
BackoffFactor:   2.0,
MaxInterval:     128 * time.Second,
MaxAttempts:     3,
Jitter:          true,
RetryOn:         DefaultRetryOn,
```

**而 `DefaultRetryOn` 是 `return true`** —— **「默认重试一切错误」。** 这与 e03 那页 graphrag 的「15 个类名的白名单」相反：**这里默认全重试，那里默认几乎全不重试。**

**而 `CalculateBackoff` 的抖动方向也相反：**

```
backoff := min(time.Duration(float64(p.InitialInterval)*powFloat(p.BackoffFactor, attempt-1), p.MaxInterval)
if p.Jitter {
	// Subtract up to 50% to spread retry bursts.
	jitter := time.Duration(float64(backoff) * 0.5 * randFloat())
	backoff = max(backoff-jitter, 0)
}
```

**「减掉最多 50% 以打散重试突发」** —— 而 graphrag 那边是**加**最多 1 秒。**两个实现选了两个方向，而对「打散」这个目的来说两者都有效**（减的好处是不突破 `MaxInterval`，加的好处是不缩短最小间隔）。

**第三，`NewMemorySaver`：**

```
func NewMemorySaver() *MemorySaver {
	return checkpoint.NewMemorySaver()
}
```

**而 `MemorySaver` 是 `checkpoint.MemorySaver` 的别名。** 于是「拿一个内存检查点器」是 `harness.NewMemorySaver()`。

**而常量与模式的转发几乎无遗漏：**

```
const (
	Start = constants.Start;  End = constants.End
	TagNoStream = constants.TagNoStream;  TagHidden = constants.TagHidden
)

const (
	StreamModeValues      = types.StreamModeValues
	StreamModeUpdates     = types.StreamModeUpdates
	StreamModeCustom      = types.StreamModeCustom
	StreamModeMessages    = types.StreamModeMessages
	StreamModeCheckpoints = types.StreamModeCheckpoints
	StreamModeTasks       = types.StreamModeTasks
	StreamModeDebug       = types.StreamModeDebug
)
```

**七个 `StreamMode` 与 `types/types.go` 里的定义完全对应。** 而 `StreamManager` 只认其中五个（见 e05 那页）—— **`custom` 与 `messages` 不走 `StreamManager` 的 include filter，而是各自另有通道。**

**`harness.go` 顶部的包文档是一份完整的可运行示例，而它值得一看因为它说明了最小可用图：**

```
type State struct { Messages []string; Counter int }
builder := harness.NewStateGraph(State{})
builder.AddNode("agent", func(ctx context.Context, state interface{}) (interface{}, error) {
	s := state.(State)
	s.Messages = append(s.Messages, "Hello from agent")
	s.Counter++
	return s, nil
})
builder.AddEdge("__start__", "agent")
builder.AddEdge("agent", "__end__")
graph, err := builder.Compile()
result, err := graph.Invoke(context.Background(), State{Messages: []string{"Hello"}, Counter: 0})
```

**注意示例里节点接收的 `state` 是 `interface{}` 而它做了 `state.(State)` 类型断言** —— 于是「状态的实际类型」在运行期才被确认。**而 `State.Messages` 是 `[]string` 切片，按上一节的默认语义它走 append 通道；`Counter` 是 `int`，走累加通道。**

**但这个示例有一个矛盾：节点自己做了 `s.Messages = append(s.Messages, ...)` 并返回整个 `s`。** 若通道对切片做 append，则「节点内 append 一次 + 通道 append 一次」会得到两条消息。**这说明示例与实际的通道语义之间需要读者自己对齐** —— 或者示例里返回的 `State` 被当作「整状态覆盖」而不是「字段级增量」。**这是一个值得在写 agent 时先验证一次的地方。**

### 八、`MapFields` 与 `MapTo`：两个一行的构造函数

```
func MapFields(from, to string) types.FieldMapping {
	return types.FieldMapping{From: from, To: to}
}

func MapTo(to string) types.FieldMapping {
	return types.FieldMapping{To: to}
}
```

**两者的差别是 `From` 是否为空。** 而 `FieldMapping` 结构本身只有两个字符串字段：

```
type FieldMapping struct {
	From string
	To   string
}
```

**「`From` 为空表示整个输出」** —— 这个语义不在类型里，只在 `MapTo` 的注释里：

> 创建一个把**整个输出**映射到目标路径的 `FieldMapping`。

**而 `FieldMapping` 被两个地方消费：**

1. **`AddDataEdge(from, to, mappings...)`** —— 边级映射。
2. **`NodeOptions.FieldMapping` / `Node.FieldMapping`** —— 节点级映射。

**而 `FieldMapping.From` 是**路径**（注释在 `DataEdge` 上说是「字段级映射」）。** 于是 `From` / `To` 是点分路径还是单段名，取决于消费方的实现 —— **而那两个消费点都在 `pregel` 里（不在本页范围内）。**

**这解释了 `MapFields` 为什么这么薄：它只是一个「让调用点读起来像配置」的糖。** `AddDataEdge(a, b, types.FieldMapping{From: "x", To: "y"})` 与 `AddDataEdge(a, b, MapFields("x", "y"))` 完全等价，而后者在行内更清楚。

## 关键取舍

**源码事实：** `stateGraph` 有四种边（`edges` / `conditionalEdges` / `branches` / `dataEdges`）、三个 schema（默认全部指向同一个对象，只有显式 `WithInputSchema` / `WithOutputSchema` 才分化）、一张通道表与一张 reducer 表。

**源码事实：** `AddNode` 直接覆盖同名节点（不报错）并返回可继续修改的 `*types.Node`；`AddNodeWithOptions` 在**装配期**把 `StatePre` / `StatePost` 包裹进节点函数（顺序 pre → 节点 → post），两处错误都带节点名。

**源码事实：** `WithStatePreHandler` / `WithStatePostHandler` / `WithFieldMapping` 返回的是「接受 `*NodeOptions` 的函数」，而 `Compile` 只吃 `CompileOption`（`func(*compiledGraph)`）—— **两套选项机制没接线，调用方要自己把前者应用到 `NodeOptions`。**

**源码事实：** `AddEdge` / `AddDataEdge` 在**装配期**校验端点存在（`__start__` / `__end__` 例外），且 `AddEdge(Start, X)` 自动设 `entryPoint`、`AddEdge(X, End)` 自动登记 `finishPoints`（去重、可多个）。

**源码事实：** `AddConditionalEdges` 校验 `from` 与映射表里所有目标，但 **`from` 不能是 `Start`**（该分支无 `&& from != Start`）—— 入口必须由 `AddEdge(Start, X)` 确定。

**源码事实：** `AddBranch` 只能校验 `from`，因为 `then` 的返回目标是运行期才知道的 —— **表达能力越强的边，装配期校验越弱。**

**源码事实：** `Validate` 四道门：有入口、有出口、**每个节点都可达**（BFS）、state schema 合法；**BFS 只走 `edges` 与 `conditionalEdges`，不走 `branches` 与 `dataEdges`** —— 于是 `AddBranch` / `AddDataEdge` 的目标不受可达性检查约束。

**源码事实：** 可达性是硬约束（拒绝「定义了但不可达的节点」），好处是拦住拼错的节点名，代价是无法登记「只在特定配置下可达」的预留节点。

**源码事实：** schema 可以是 `struct` 或指向它的指针；`map` 形式只校验键是字符串、**不提取任何字段注解**（注释明说「实践中通道靠 `AddChannel` 动态添加」）；未导出字段（`field.PkgPath != ""`）被跳过。

**源码事实：** struct tag 是 `harness`，格式 `reducer=add` / `reducer=custom,meta=value`；只有 `reducer` 是「被理解的键」，其余全部进 `Annotation.Metadata`；`len(kv) != 2` 时当作布尔标志；**`reducer` 的值必须命中 `add` / `append` / `merge`，否则报错**（因为拼错的 reducer 名会静默退化成覆盖写、只丢数据不报错）。

**源码事实：** **字段类型决定默认通道**：切片/数组 → append，map → merge，数字 → 累加，其它 → `LastValue`；`[]any` 用 `ListAppend` 而具体类型切片用 `reflect.AppendSlice`，且显式 reducer 覆盖两者。`LastValue` 每步只接受一个写入（多于一个报 `InvalidUpdateError` 并提示加 reducer）。

**源码事实：** 数字分支的注释是一个真实反射陷阱：传 `reflect.Zero(fieldType).Interface()` 而非 `fieldType` 本身，这样下游 `createZeroValue` 用的是正确底层类型而不是 `reflect.Type` 描述符。

**源码事实：** `configureChannelsFromSchema` **不覆盖已存在的通道**（显式 `AddChannel` 优先于 schema 推导）；`Compile` 先 `Validate`（内含一次 schema 反射）再 `configureChannelsFromSchema`（第二次反射）。

**源码事实：** `Compile(opts ...interface{})` 是类型擦除：`CompileOption` 定义在 `graph` 包且 `compiledGraph` 非导出，`types` 包引不到它；**实现里 `if fn, ok := opt.(CompileOption); ok` 没有 `else` 分支 —— 传错类型的选项被静默忽略。**

**源码事实：** `compiledGraph.checkpointer` 的类型是 `interface{}`（注释 `// checkpoint.BaseCheckpointer`），同样是防循环依赖；而 `init_pregel` 里用 `cp, _ := cg.GetCheckpointer().(checkpoint.BaseCheckpointer)` **丢掉了断言的 `ok`** —— 传错类型会得到一个 `nil` 检查点器而非错误。

**源码事实：** `Compile` 最后一行 `g.NodeTriggerMode = cg.nodeTriggerMode` 把编译选项写回**未编译的图**；同一张 `stateGraph` 编译两次而选项不同时，第二次的默认会覆盖第一次的，而第一次的 `compiledGraph` 保留旧值。

**源码事实：** `types.All = "*"` 存在且注释说「表示在所有节点上中断」，但 `WithInterrupts` 只是把它当作一个普通节点名存进 `interrupts` map —— 是否解释成哨兵取决于 pregel 下游。

**源码事实：** `Invoke` 用 `StreamModeValues` 跑一次 `run` —— 「非流式 = 以 values 模式跑一次流」；`Stream` 用一个 goroutine + 容量 1 的 channel + 两个带 `ctx.Done()` 的 `select` 把单返回值转成 channel。

**源码事实：** `run` 通过全局变量 `types.PregelRunFunc` 调用引擎，nil 时报「graph: pregel engine not installed」；由 `harness.go` 的**空白导入** `_ "ragflow/internal/harness/graph/pregel"` 触发 `init()` 安装，用来打破 `graph ↔ pregel` 的循环依赖。

**源码事实：** `init_pregel` 把 `compiledGraph` 上的检查点器、中断集、递归上限、debug 全部搬进一个 `Engine` 然后 `RunSync`；**它丢掉了 `streamMode`**（`RunSync` 不接受），于是 `Stream(ctx, input, mode)` 的 `mode` 在这条路径上没有落地。

**源码事实：** `DefaultRetryPolicy` 是 `InitialInterval 500ms` / `BackoffFactor 2.0` / `MaxInterval 128s` / `MaxAttempts 3` / `Jitter true` / **`RetryOn = 恒真`**（默认重试一切错误，与 graphrag 那份「15 项白名单」相反）；`CalculateBackoff` 的抖动是**减去最多 50%**（与 graphrag 的「加最多 1 秒」方向相反）。

**源码事实：** `harness.go` 的包文档示例里节点自行 `append` 切片并返回整个 `State`，而通道对切片默认也做 append —— **示例与通道语义需要读者自己对齐。**

**架构推论：** 这个装配接口的核心取舍是「**校验前移**」。`AddEdge` 在装配期查端点、`Validate` 在编译期查可达性、schema 在编译期查 tag —— 三步都把「拼错」挡在运行之前。**而它的代价写在 `AddBranch` 与 `AddDataEdge` 上：表达力越强，能校验的越少。** 这不是缺陷，而是「静态可检性」与「运行期灵活性」的必然权衡；关键是**让调用方知道哪条边被校验了、哪条没有**，而当前的 API 通过「`AddConditionalEdges` 校验映射表而 `AddBranch` 只能校验源」这种不对称把信息传递出去了。

**架构推论：** 三处 `interface{}`（`Compile` 的选项、`checkpointer`、`NodeFunc` 的 state）都源于同一个问题：**包的依赖方向被限定成 acyclic，于是「下游才知道的类型」只能以 `interface{}` 穿越边界。** 代价是**每一处都丢掉了一层类型检查**，而其中两处（选项与检查点器）是**静默**的 —— 传错不报错、只是不生效。**这类静默降级在框架代码里危害最大，因为调用方会以为配置生效了。**

**架构推论：** 「字段类型决定默认归并语义」这张表是本页最实用的部分，也是最容易踩的部分。它让「`[]string` 字段自动追加、`int` 字段自动累加」这样的事无需声明；但它同时意味着**改一个字段的类型就改了它的语义** —— 把 `Counter int` 改成 `Counter string`，通道就从「累加」变成「覆盖」。**类型即语义的代价是「类型重构静默改变了行为」。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/graph/graph/graph.go` | `stateGraph` | 四种边 + 三 schema + 通道表 + reducer 表 |
| `internal/harness/graph/graph/graph.go` | `NewStateGraph` | 三 schema 全指向同一对象 |
| `internal/harness/graph/graph/graph.go` | `AddNode` / `AddNodeWithOptions` | 返回句柄 / 装配期包裹 pre-post |
| `internal/harness/graph/graph/graph.go` | `AddEdge` | 端点校验 + 自动设入口/出口 |
| `internal/harness/graph/graph/graph.go` | `AddConditionalEdges` / `AddBranch` | 映射表可校验、函数返回列表不可 |
| `internal/harness/graph/graph/graph.go` | `AddDataEdge` / `MapFields` / `MapTo` | 带字段映射的边与两个构造函数 |
| `internal/harness/graph/graph/graph.go` | `AddChannel` / `SetReducer` / `AddChannelWithReducer` | 显式通道与 reducer 注册（优先于 schema 推导） |
| `internal/harness/graph/graph/graph.go` | `Validate` / `computeReachable` | 四道门与只走两种边的 BFS |
| `internal/harness/graph/graph/graph.go` | `configureChannelsFromSchema` | 不覆盖已存在通道 |
| `internal/harness/graph/graph/graph.go` | `Compile` | 类型擦除的选项循环 + 写回 `NodeTriggerMode` |
| `internal/harness/graph/graph/graph.go` | `compiledGraph` | 七个字段，其中 `checkpointer` 是 `interface{}` |
| `internal/harness/graph/graph/graph.go` | `WithCheckpointer` / `WithInterrupts` / `WithInterruptsAfter` | 六个编译选项 |
| `internal/harness/graph/graph/graph.go` | `Invoke` / `Stream` / `run` | 非流式=values 流 / 单值转 channel / 全局函数指针 |
| `internal/harness/graph/graph/state.go` | `validateStateSchema` / `processField` | 反射建通道，跳过未导出字段 |
| `internal/harness/graph/graph/state.go` | `parseAnnotation` / `reducers` | `harness` tag 解析与三个内置 reducer |
| `internal/harness/graph/channels/reducer.go` | `CreateReducerChannel` | 字段类型 → 默认通道（切片 append / map merge / 数字累加 / 其余 `LastValue`） |
| `internal/harness/graph/channels/reducer.go` | `ReducerChannel.Update` | 把当前值与新值按 reducer 折成一个 |
| `internal/harness/harness.go` | `NewStateGraph` / `NewMemorySaver` / `DefaultRetryPolicy` | 统一入口 |
| `internal/harness/harness.go` | `_ "ragflow/internal/harness/graph/pregel"` | 触发 `init()` 安装 `PregelRunFunc` |
| `internal/harness/harness.go` | `WithCheckpointer` 等 `var` 转发 | 六个编译选项的对外名字 |
| `internal/harness/graph/pregel/init_pregel.go` | 匿名安装函数 | `compiledGraph` → `Engine` 的翻译 |
| `internal/harness/graph/types/types.go` | `NodeTriggerMode` / `StreamMode` / `All` | 语义枚举与哨兵 |
| `internal/harness/graph/types/types.go` | `Node` / `Edge` / `ConditionalEdge` / `Branch` / `DataEdge` / `FieldMapping` | 六种结构 |
| `internal/harness/graph/types/types.go` | `NodeOptions` / `RetryPolicy` / `DefaultRetryPolicy` / `CalculateBackoff` | 选项与重试策略 |
| `internal/harness/graph/types/types.go` | `StateGraph` / `CompiledGraph` 接口 | `Compile(opts ...interface{})` 的出处 |
| `internal/harness/graph/types/types.go` | `SetPregelRunFunc` / `PregelRunFunc` | 打破循环依赖的安装点 |
| `internal/harness/graph/constants/constants.go` | `Start` / `End` / `DefaultRecursionLimit` | `__start__` / `__end__` / 50 |
| `internal/harness/graph/constants/constants.go` | `Reserved` / `IsReserved` | 保留键表（含 `IsReserved` 判定） |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：graph/graph.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/graph/graph/graph.go)
- [ragflow 源码：harness.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/harness.go)
- [LangGraph 文档：Graph API](https://langchain-ai.github.io/langgraph/concepts/low_level/)
- [Go 文档：reflect 与 struct tag](https://pkg.go.dev/reflect#StructTag)
- [Go 文档：init 函数](https://go.dev/doc/effective_go#init)

## 与主核 / AI 的关系

这个装配接口与主核的**查询计划器（planner）** 处在同一个位置：把「用户声明的结构」转成「可执行的计划」。而两者的校验时机不同。

**主核在 `parse_analyze` 阶段就把「表不存在、列不存在、类型不匹配」全部查出来** —— 因为它的元数据是权威的、可查的。**而这个装配接口的元数据就是它自己刚收集的那点东西**（节点表、边列表、通道表），于是它能查的只有「引用一致性」：`AddEdge` 查端点、`Validate` 查可达性。**至于「这个节点的输入类型对不对」「这条边传的字段存不存在」，它一概不知道** —— 因为 Go 的类型系统在 `NodeFunc` 这一层已经被 `interface{}` 抹掉了。

**三处 `interface{}` 与主核的 `Datum` 是同一个现象的两种解法。** 数据库用 `Datum`（一个带类型标签的联合）来表达「任意值」，于是运行期仍能检查类型；而这里用 `interface{}`（无标签），于是运行期只能靠断言。**差别在于「类型信息是否随值一起流动」** —— 而 `NodeFunc` 的签名 `func(context.Context, interface{}) (interface{}, error)` 恰好把类型信息丢在了签名里。

**「可达性检查只覆盖两种边」这个事实在数据库里有精确对应：`EXPLAIN` 只分析它能建模的部分。** 一个 `LATERAL` 子查询或一个函数的内部行为，计划器看不到。**ragflow 的 `AddBranch` 也是同样意义上的「规划器看不见」** —— 它的目标是运行期算出来的。

**「默认重试一切」与「白名单」这两种策略，在数据库里对应的是「错误分类」。** PostgreSQL 的 `ERRCODE_*` 分类决定了事务是否被中止，而「可重试」是一个更上层、更保守的判断。**默认全重试的风险是「对一个必然失败的请求重试 3 次」**（浪费但无害）；**默认不重试的风险是「一次瞬时故障导致整批失败」**（有害）。**ragflow 的两套代码分别选了这两个默认，说明这个选择必须按场景定。**

对 agent 工程的直接价值有三条。其一，**校验前移的收益随「表达能力」递减**。无条件边能全查、条件边能查一半、动态分支只能查源 —— 于是**API 应该让调用方感知到这个递减**（ragflow 通过「哪些方法返回目标校验错误」隐式地传递了它）。其二，**`interface{}` 穿边界必然丢类型检查，而丢掉的检查会在最坏的位置变成静默降级**。`Compile` 忽略错类型选项、`GetCheckpointer` 丢掉断言结果 —— 这两处都是「配置没生效但没人知道」。其三，**「字段类型决定归并语义」这个约定很省事但很脆**。它让普通字段零配置，也让「改类型」变成一次静默的语义变更 —— 在 agent 状态里，把 `[]Thing` 改成 `Thing` 会让「追加」变成「覆盖」，而编译器不会警告。
