---
project: ragflow
feature: tools-node
title: 工具节点与工具注册表
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/core/tools_node.go
    symbols: [ToolsNode, ToolsNodeConfig, LoopGuard]
  - path: internal/harness/core/tool_registry.go
    symbols: [ToolRegistry, NewToolRegistry]
  - path: internal/harness/core/contracts.go
    symbols: [Tool, Model, ToolCapability, InvokableToolEndpoint]
---

# 工具节点与工具注册表

## 一句话职责

`ToolsNode` 把「模型吐出的一批工具调用」变成「一批工具结果消息」：按能力分批、只读并行、写类串行，并带一个专门抓「同参数反复调用」的死循环守卫；`ToolRegistry` 则是工具的名字空间与别名层。

## 机制是什么

模型输出里的工具调用是一个数组，`ToolsNode.Execute` 要回答三个问题：能不能同时跑、失败了怎么办、跑飞了怎么办。

**能不能同时跑**由 `ToolCapability` 决定。这是一个四值枚举：只读、写文件、执行代码、网络访问。只声明前者的工具可以并行，其余三类一律串行。默认值不是「只读」而是 `ToolCapWritesFiles`——**没有声明能力的工具被当成写类，保守串行**。这个默认方向是对的：把未知当危险比当安全划算。

**失败了怎么办**是「首错即停但结果照收」。并行批里用互斥锁记录第一个错误，其余错误丢弃；串行批里第一个错误同样只记一次但**继续跑完剩余工具**。最后若 `firstErr` 非空，把已收集的结果连同错误一起返回——注意返回的是 `nil` 结果加错误，也就是说**前面成功的工具结果在这一层被丢弃了**。这是刻意的：调用方（for 循环形态）拿到错误就终止整轮，不需要半成品。

**跑飞了怎么办**是 `LoopGuard` 的职责。它维护两张表：按「工具名 + 参数 MD5」计数的同参调用表，与按工具名计数的连续失败表。任一计数达到阈值就返回错误，而 `executeOne` 把这个错误**转换成一条工具结果消息**而不是向上抛——于是模型会看到「loop guard: tool 'X' called N times with identical arguments」，有机会自己换个做法，而不是让整个 Agent 崩掉。

这三条都不是「工具调用」的必需品，而是**把工具从演示推进到可用**的必需品。

## 怎么实现的

**工具的合并顺序是「后者覆盖前者」。** `NewToolsNode` 先按容量 `max(len(Tools), len(Registry.tools))` 建 map，先灌注册表里的全部工具，再灌 `Tools` 切片——同名时切片赢。这条规则让「注册表提供默认集合、配置切片做定点覆盖」成为可行写法。注册表的 `AllTools` 是遍历 map，顺序不确定，但因为最终只要有 map，顺序无关。

**单调用走快路径。** `len(toolCalls) == 1` 时直接执行，不起 goroutine、不加信号量。注释写明理由是「no goroutine overhead」。这条优化在小批量场景下有意义（一次工具调用是常见情形），但代价是同一段逻辑写了两遍——单调用路径在 `Execute` 顶部，多调用路径在下面。

**分批规划是「贪心合并相邻只读」。** `planBatches` 顺序扫过调用列表，遇到只读就攒进当前的并行批，遇到非只读就先把已攒的并行批刷出去、再单独成一个串行批。于是 `[读, 读, 写, 读]` 会变成 `[并行(读,读)] [串行(写)] [并行(读)]`——**写工具把前后的只读切成了两段**，而不是把写排到最后。这个选择保证了语义顺序：写之后才读的东西能读到写的结果。

未知工具（不在 `toolMap` 里）走「串行且单独成批」分支——因为 `toolCapFromTool` 拿到 nil 时会返回默认写类。所以一个拼错名字的工具会让整个分批多出一个串行批，然后才在 `executeOne` 里报「not found」。这是先规划后执行顺序决定的。

**并行批的错误处理有个真实缺陷。** 并行批里每个 goroutine 拿到错误时做的是 `if err != nil && firstErr == nil { firstErr = ...; return }`——`return` 之后**不再写 `parResults[idx]`**。而收集阶段是遍历 `parResults` 并跳过 nil 的。所以出错调用的位置留在 nil，被跳过，其余成功的结果照收。行为上正确，但依赖「未写入即零值」这一点，不显式也不明显。

**`executeOne` 的三层防护。** 第一层是 `defer recover()`：工具内部 panic 被捕获并转成结果消息「Error: tool 'X' panicked: ...」，且**把 err 显式置 nil**——panic 不向上传播。第二层是 `LoopGuard.CheckSameArgs`。第三层是未知工具的处理器：配了 `UnknownToolHandler` 就交给它（例如服务端工具搜索），没配就返回「not found」消息。

**参数别名重映射在进入工具前做。** `ArgumentsAliases` 的形状是「工具名 → 规范参数名 → 别名列表」。`remapToolArgs` 先把 JSON 解成 `map[string]json.RawMessage`，再对每个规范名尝试它的别名：**只有在规范名不存在时才覆盖并删除别名**。所以规范名优先，别名只做兜底。JSON 解析失败时原样返回参数串，不报错——因为这一步是「尽力而为」的兼容层，不该成为新的失败点。

**中止语义是 `AgentAction`。** 工具执行前先查 `ReturnDirectly`，命中就设一个 `NewExitAction()`；执行完把 action 一起返回。调用方看到 `action.Exit` 就终止循环。所以「直接返回」不是工具自己决定的事，而是**工具名在配置表里**这件事决定的。

**工具级中断被单独识别。** 若工具返回的错误是 `ToolInterruptError`，`executeWithNewChain` 不把它当失败，而是：把中断状态写进 ctx、把工具调用 ID 追加成一个地址段、复制地址、返回一条内容为「[interrupted: ...]」的结果消息加一个 `interruptResult` 错误值。图形态（e07）在 `execute_tools` 里用 `errors.As` 认出它，把这条消息存进缓存、跳出循环、让图引擎去存检查点。**所以工具中断在图形态里是「正常结束本节点」，在 for 循环形态里则不是**——for 循环没有对应的识别逻辑。

**`LoopGuard` 的两个 reset 条件不对称。** `Reset(toolName)` 会清掉该工具的所有同参计数与失败计数，但**代码里没有调用点**：`executeOne` 只在执行前检查，成功执行后不 reset。也就是说同参计数是**只增不减的累计值**，而不是「连续同参」——注释说的是「tracks consecutive same-args calls」，实现却是累计。实际效果是：一个工具在整个会话里累计被同参调用 N 次就触发，中间穿插别的调用也不清零。这个偏差让门槛比名字暗示的更严。

另外 `Reset` 清理同参表时用的是**前缀匹配**（`k[:len(toolName)] == toolName`），而 key 的格式是 `工具名|哈希`。前缀匹配本身没错，但它对「一个工具名是另一个工具名前缀」的情形会误删（如 `search` 与 `search_web`）。当前没有调用点，所以尚未暴露。

**`ToolRegistry` 的并发策略是「快照后应用」。** `Merge` 的写法值得单独看：先拿对方的读锁把三张表全量拷进局部变量，释放后再拿自己的写锁应用。这样做是因为 Go 的 `sync.RWMutex` 不可重入，若持自己的写锁再去读对方，而对方正好在 `Merge` 自己，就会死锁。源码注释直接写明「snapshot-then-apply pattern to avoid deadlock」，并显式处理了自合并（`r == other` 直接返回）。

`Filter` 与 `Merge` 相反：它持自己的读锁，但在锁内调 `result.Register`——注册到的是新建的 registry，不涉及本对象的写锁，所以不冲突。**这是一处需要仔细看才敢确认无死锁的代码**：跨对象加锁时，锁的归属必须一眼看清。

**`ToolInfos` 的分级。** 注册表导出工具元信息时，若工具实现了 `ToolInfoProvider` 就用它的完整结构化信息（含参数 JSON schema），否则退化成「只有名字与描述」。这意味着**不实现该接口的工具，模型看不到参数 schema**，只能靠描述猜——这是把「绑定质量」责任交给工具作者的明确信号。

**`InvokableToolEndpoint` 与 `StreamableToolEndpoint` 是孤立的类型声明。** 这两个函数类型（同步返回字符串 / 返回字符串流）外加两个 enhanced 变体，在 `contracts.go` 里定义得很清楚，但**整个仓库没有任何地方使用它们**——`ToolToInvokeFn` 走的是 `Tool.Invoke` 接口方法，`EnhancedToolToInvokeFn` 走 `EnhancedInvoke`。所以这四类 endpoint 目前是预留的函数签名，不是实际的可插拔点。`Tool` 接口本身则只有两个方法：`Invoke` 返回字符串、`Stream` 返回字符串流——**流式能力是接口的一部分，但 `ToolsNode` 只调 `Invoke`**，流式工具在工具节点里没有被用到。

## 关键取舍

**源码事实：** 工具能力默认 `ToolCapWritesFiles`（串行），只有显式实现 `CapableTool` 并返回 `ToolCapReadOnly` 的工具才并行。

**源码事实：** 分批是贪心合并相邻只读，写工具会切断并行批，而不是被统一挪到末尾。

**源码事实：** 单工具调用走无 goroutine 的快路径，与多工具路径是两段独立代码。

**源码事实：** 并行批出错时用 `return` 提前结束该 goroutine，靠 `parResults` 的零值 + 收集阶段跳过 nil 来实现「失败位置留空」。

**源码事实：** 工具 panic 被 `recover` 转成结果文本并把 err 置 nil，不向上传播。

**源码事实：** `LoopGuard.CheckSameArgs` 只查不重置，`Reset` 全仓无调用点；同参计数实际是累计而非「连续」。

**源码事实：** 未实现 `ToolInfoProvider` 的工具只能导出名字与描述，没有参数 schema。

**源码事实：** `InvokableToolEndpoint` / `StreamableToolEndpoint` 及两个 enhanced 变体仅声明、无使用点；`ToolsNode` 只调用 `Tool.Invoke`，不调用 `Tool.Stream`。

**架构推论：** 「能力声明 → 分批 → 并行」这条链条把并发安全的责任前置到工具作者。框架无法判断一个工具是否安全，只能读它的自述。默认值选保守侧是对的，但后果是**忘记实现 `CapableTool` 会让整批退化成全串行**，而这件事在运行时完全静默——没有日志、没有指标。

**架构推论：** 把循环守卫的报错做成「工具结果」而不是「错误」是一个语义选择：它假设模型有能力从「你重复了」这句话里学会换策略。这是把「防死循环」从硬停机降级成软提示。代价是如果模型固执，累计计数会一路上涨直到超过 `maxSame` 后每次都触发——但那时每次仍然只是一条结果消息，循环仍靠 `MaxIterations` 兜底。**两层防护叠加而非互斥**：`LoopGuard` 管「重复同样的错」，`MaxIterations` 管「一直错下去」。

**架构推论：** `ToolRegistry` 支持别名与分类，但 `ToolsNode` 的查找用的是**原始 map**（`tn.toolMap[tc.Function.Name]`），而不是 registry 的 `Lookup`。所以**别名在工具执行时不生效**——别名只在注册表自己的 API 里有意义。这是一个容易误判的能力边界：注册了别名不代表模型用别名调用能被解析。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/core/tools_node.go` | `ToolsNode` | 工具执行节点：分批、并行、结果归一 |
| `internal/harness/core/tools_node.go` | `ToolsNodeConfig` | 工具集、注册表、直接返回表、循环守卫、别名表 |
| `internal/harness/core/tools_node.go` | `ToolsNode.Execute` | 单调用快路径与多调用分批执行 |
| `internal/harness/core/tools_node.go` | `ToolsNode.planBatches` | 按能力贪心拆并行/串行批 |
| `internal/harness/core/tools_node.go` | `ToolsNode.executeOne` | panic 恢复、循环守卫、未知工具处理 |
| `internal/harness/core/tools_node.go` | `ToolsNode.executeWithNewChain` | 别名重映射、中间件链、工具中断识别 |
| `internal/harness/core/tools_node.go` | `LoopGuard` | 同参调用与连续失败的双阈值守卫 |
| `internal/harness/core/tools_node.go` | `LoopGuard.CheckSameArgs` | 同参调用的累计计数与触发 |
| `internal/harness/core/tools_node.go` | `LoopGuard.Reset` | 清理工具计数，当前无调用点 |
| `internal/harness/core/tools_node.go` | `toolCapFromTool` | 能力读取与保守默认值 |
| `internal/harness/core/tool_registry.go` | `ToolRegistry` | 名字、别名、分类三张表加读写锁 |
| `internal/harness/core/tool_registry.go` | `NewToolRegistry` | 构造空注册表 |
| `internal/harness/core/tool_registry.go` | `ToolRegistry.Merge` | 快照后应用，规避跨对象加锁死锁 |
| `internal/harness/core/tool_registry.go` | `ToolRegistry.ToolInfos` | 分级导出元信息，缺接口时退化为名字加描述 |
| `internal/harness/core/contracts.go` | `Tool` | 工具契约：名字、描述、同步调用、流式调用 |
| `internal/harness/core/contracts.go` | `Model` | 模型契约：生成、流式、绑定工具 |
| `internal/harness/core/contracts.go` | `ToolCapability` | 四值能力枚举，决定并发调度 |
| `internal/harness/core/contracts.go` | `InvokableToolEndpoint` | 预留的同步工具函数签名，无使用点 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：core/tools_node.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/core/tools_node.go)
- [ragflow 源码：core/tool_registry.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/core/tool_registry.go)
- [OpenAI 函数调用文档](https://platform.openai.com/docs/guides/function-calling)

## 与主核 / AI 的关系

**「能力声明决定并发度」与主核的锁粒度设计同构。** 数据库要知道一条语句读不读、写不写，才能决定加共享锁还是排他锁；这里要知道一个工具只读还是写，才能决定并行还是串行。两边都面对同一个问题：**判断错了会静默出错，而不是报错**。数据库靠语法分析得到准确判断，这里只能靠工具自述——所以默认值必须选保守侧。

**保守默认与主核的「未知即最坏」一致。** 主核在优化器统计信息缺失时会给最差选择率假设；这里在能力未声明时给最严格的串行调度。**「不知道就按最坏的来」是分布式与并发系统的共同默认**，因为高估安全比低估安全便宜得多。

**循环守卫的定位类似主核的查询超时。** 数据库不试图判断一条 SQL 会不会跑死，只用超时兜底；但也会检测明显的退化模式（如笛卡尔积）并提前警告。`LoopGuard` 是后者、`MaxIterations` 是前者——两级并存。区别在于数据库的两级都由系统强制，这里的第一级只能「劝」模型。

**别名只在注册表层生效，执行层用原名。** 这与主核里「视图名与底层表名」的关系正好相反：数据库会在解析阶段把视图名改写成真实表名，所以两处都能用。这里没有这个改写步骤，于是「注册了别名但模型用别名却查不到工具」成为一个静默失败点。

对 agent 工程最实用的五条。其一：**自定义工具想并行必须显式实现 `CapableTool` 返回只读**，否则会被串行化。其二：**工具 panic 不会让 Agent 崩**，它变成一条结果文本交给模型；排查工具 bug 时不能只看 Agent 是否失败。其三：**`LoopGuard.Reset` 没人调用**，同参计数是累计的——长时间会话会越来越容易触发。其四：**`ToolsNode` 只用 `Invoke`，不用 `Stream`**，实现流式工具在工具节点这条路径上拿不到流式输出。其五：**别名不影响工具解析**，它只在注册表 API 里有意义。
