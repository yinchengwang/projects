---
project: ragflow
feature: middlewares
title: 中间件体系
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/core/middlewares/summarization/summarization.go
    symbols: [NewTyped, BeforeModelRewrite]
  - path: internal/harness/core/middlewares/filesystem/filesystem.go
    symbols: [ContributeTools, BeforeAgent]
  - path: internal/harness/core/middlewares/plantask/plantask.go
    symbols: [Manager, TaskState]
  - path: internal/harness/core/middlewares/subagent/subagent.go
    symbols: [SubAgentSpec, SubAgentMiddleware]
---

# 中间件体系

## 一句话职责

中间件是 Agent 的可插拔扩展点：`TypedReActMiddleware` 提供五个生命周期钩子，而 `ToolContributor` 这一可选接口额外提供「贡献工具」的能力——四类内置中间件正是按「用哪个钩子」分工的。

## 机制是什么

这套体系有**两套并行接口**，区分它们是把这一层读懂的关键。

**第一套是生命周期钩子**，五个方法按调用顺序排列：`BeforeAgent`（进 Agent 前，可改指令、工具表、直接返回表）、`BeforeModelRewrite`（每轮调模型前，可改状态）、`AfterModelRewrite`（调完模型后）、`AfterAgent`（整次执行成功后）、`WrapModel`（包住模型对象本身）。除了 `BeforeAgent` 与 `AfterAgent` 各只跑一次，中间两个是**每轮迭代都跑**。基类 `BaseMiddleware` 给全部五个方法的空实现，所以实现者只覆写需要的。

**第二套是 `ToolContributor`**，三个方法：`ContributeTools`（贡献工具实例）、`ContributeToolInfos`（贡献只用于绑定的元信息，对应那些没有实际 Tool 的伪工具，例如「搜索工具」本身）、`ContributeReturnDirectly`（贡献直接返回的工具名）。

两套接口的交界处有一条明确的演进痕迹。`ToolContributor` 的注释直说了它取代两种旧做法：「在 `BeforeAgent` 里改 `rc.Tools`」——因为那样改不会传播到 `ToolsNode`；以及 `BindToConfig`——因为那时序耦合（必须在构造 Agent 前调用）。**收集发生在 `BeforeAgent` 之前**，所以工具对 `ToolsNode` 构造与 `BeforeAgent` 处理都可见。

于是四类内置中间件的分工可以按「用哪个钩子」一眼看出：

| 中间件 | 主用钩子 | 职责 |
|---|---|---|
| summarization | `BeforeModelRewrite` | 压缩历史（改状态里的消息列表） |
| filesystem | `ContributeTools` | 贡献读写文件等七个工具（`BeforeAgent` 是空实现） |
| plantask | 都不是 | 它**不是中间件**，是工具库 + 会话内状态管理器 |
| subagent | `ContributeTools` + `BeforeModelRewrite` | 把子 Agent 包装成工具，并把元信息塞进本轮工具表 |

## 怎么实现的

**构造函数有两套命名，含义不同。** `summarization` 与 `filesystem` 都有 `NewTyped[M]` 与 `New`：前者泛型，返回 `TypedReActMiddleware[M]`；后者是 `NewTyped[*schema.Message]` 的便捷包装。`subagent` 只有 `New(specs, cfg)`，因为它是具体类型、不需要泛型。`plantask` 只有 `NewManager()`——它压根不实现中间件接口。所以「有没有 `NewTyped`」这个表面差异，实际反映的是「是否泛型化」。

**summarization 的 `BeforeModelRewrite` 是四阶段流水。** 顺序固定，且每一阶段都可能提前返回：

1. **Supersede**（可选）：先剔除「被后续操作取代的旧文件操作」。这是为了在判断阈值前把冗余历史去掉——否则可能因为一堆已被覆盖的读写记录而误触发压缩。
2. **阈值判定**：消息条数超 `MaxMessages`，或估算 token 超 `MaxTokens`，即触发。token 计数默认用「字符数 × 4/3」这种粗估，不接真分词器。
3. **压缩**：`Compactor` 负责「安全切分」（不把一轮工具调用与其结果拆开），摘要由回调 `generateSummary` 产生。这里有一个关键的容错选择：**压缩失败时返回原状态而不报错**（直接 `return ctx, state, nil`）——宁可这次不压，也不让整轮失败。
4. **摘要二次压缩 + 定稿**：若摘要外包了 `<summary>` 标签且启用了压缩，就按预算对内部内容做优先级压缩；最后过 `Finalize` 钩子（`FinalizerBuilder` 提供的实现是「摘要 + 保留最近 N 条原文」）。

`generateSummary` 里还有一处值得注意的分支：模型为空时**不报错，直接返回「(N messages)」这种占位摘要**。这让 summarization 在无模型场景下也能跑通（压缩成一句话），代价是摘要毫无信息量。重试逻辑是手写的指数退避，次数优先取配置里的 `MaxRetries`，否则用「1 + 重试配置的 MaxRetries」；耗尽后返回占位摘要加最后一个错误——**错误被返回给调用方，但调用方（`BeforeModelRewrite`）收到 err 时也仍返回原状态，所以这个 err 实际被吞掉了**。

**filesystem 的 `ContributeTools` 是「可配置的固定集合」。** 它构造七个工具：读、写、编辑、列目录、glob、grep、执行 shell。每个工具的生成都过 `maybeTool`，后者支持三种覆盖：`Disabled` 直接不产出、`Custom` 用自定义函数替换实现、`Name`/`Description` 改名改描述。所以「关掉 shell 工具」「把 grep 换成基于索引的搜索」都只是配置。

参数解析统一采用**双形态**：先试 JSON，失败再退回 `|` 分隔的字符串。写工具是 `path|content`，编辑是 `path|old|new`，grep 是 `pattern|path|output_mode`。读工具特殊：**不加 JSON 解析，直接把整个参数当路径**，并且会给每行加行号前缀。这个「行号前缀」是有意的：让模型能引用具体行，也让后续 `edit` 的定位更容易。

`BeforeAgent` 在这个中间件里是**空实现**，注释写明「retained for backward compatibility with code that calls BeforeAgent directly (e.g., existing tests)」——也就是说这个钩子对 filesystem 没有任何功能作用，它存在的唯一理由是有测试直接调它。这是全库「保留兼容表面」的一个明确样本，与该仓 AGENTS 里「删兼容层」的取向相反。

**plantask 自己承认不是中间件。** 包注释里有一段 TODO，直说「placed under middlewares/ but does not implement TypedReActMiddleware[M]. It is a tool library rather than a middleware」，并建议要么移到工具目录、要么与 `prebuilt/deep` 的 `TaskManager` 去重。所以它的定位是：**一个会话级任务表 + 五个工具**。

`Manager` 是那张表：`tasks` 存全部任务（含子任务的扁平 map）、`nextID` 是自增号。`taskInternal` 把公开的 `Task` 与子任务索引分开，于是「按 ID 查」是 O(1)、「查某父任务的子任务」也是 O(1)，而「列顶层任务」要遍历全表。父子注册**在同一个锁内完成**，注释写明 `TOCTOU-safe`——若拆成「建任务」与「挂到父任务」两步加锁，中间会存在「任务已存在但不在父任务下」的窗口。

`Create` 会校验父任务存在，不存在就报错。但 `Delete` **只删自己、不处理子任务**，也没有清理父任务的 `subtasks` 索引——所以删掉一个中间层任务会留下指向已删对象的悬空子任务，且 `Count` 仍会把它们算进去。这是当前实现的边界。

会话挂载靠 run-session：`InitManager` 把 Manager 写进会话的 `run local value`（键是 `_plantask_manager`），工具执行时 `GetManagerFromContext` 取回。所以**任务表跟着会话走**，不进 Agent 配置，也不进检查点。`TaskState` 是五值字符串枚举：待办、进行中、完成、失败、取消。

**subagent 同时用两套接口，这是最复杂的一个。** 它的 `ContributeTools` 返回「把子 Agent 包成工具」的那批 wrapper；`ContributeToolInfos` 返回**实际构建成功的那些** spec 的元信息；`BeforeModelRewrite` 再把 `builtInfos` 追加进 `state.ToolInfos`。

这里有一处冗余值得点出：工具元信息既通过 `ContributeToolInfos` 进入 Agent 构建期的工具表，又在每轮 `BeforeModelRewrite` 里再追加一次。于是同一个子 Agent 的 ToolInfo 可能在 `state.ToolInfos` 里出现两遍。源码注释没有解释这个重复，但从路径看：前者给「模型绑定时用」，后者给「本轮状态里用」，两条路的消费者不同。

`SubAgentSpec` 有三种给 Agent 的方式，优先级明确：`AgentConfig`（声明式，最高）→ `Agent`（预构建）→ `AgentFactory`（惰性工厂，最低）。`resolveAgent` 按这个顺序短路，三者皆空则返回 nil、该 spec **被静默跳过**。

`InheritParentMiddlewares` 只对 `AgentConfig` 有效，注释明写「InheritParentMiddlewares is silently ignored for pre-built agents」——因为预构建 Agent 的中间件列表在构造时就固定了。继承时的过滤有两条：用标记接口 `subAgentMarker` 排掉自身（**防止无限递归**），再按「类型全名字符串」排掉配置里点名的那些。类型名靠 `fmt.Sprintf("%T", mw)` 拼——用字符串做排除判据是一处脆弱点：泛型实例化后类型名很长，写错一个字符就静默不排除。

`MaxDepth` 通过 context 逐层传递，超过就返回错误。深度不是状态字段，所以它天然随调用栈走，不需要额外清理。

**构建时机有三种，且有并发保护。** `ensureBuiltSimple`（无父配置，不继承）与 `ensureBuilt`（有父配置，可继承）都先用互斥锁检查 `built` 标志再置位；`BindToConfig` 同样。三条路都幂等。**但这些检查是「先查后置」在同一次加锁内完成的，而实际构建在锁外**——两个 goroutine 同时进来，先到者置 `built` 后开始构建，后到者看到 `built` 已置位就返回空工具列表（`m.tools` 还没填）。这是一个真实的并发窗口，只在多 goroutine 同时首次构建时触发。

**`BindToConfig` 已标记废弃，但它有一个副作用值得记住：** 它把 `config.ToolsConfig = nil`。理由是它已经把工具塞进 `config.Tools`，若不清空 `ToolsConfig` 会导致 e07 里讲过的「两个来源重复收集」。清单式的废弃标注在这里对应着一条真实的数据一致性要求。

## 关键取舍

**源码事实：** 中间件有两套并行接口——五个生命周期钩子（`BeforeAgent`/`BeforeModelRewrite`/`AfterModelRewrite`/`AfterAgent`/`WrapModel`）与三个贡献方法（`ContributeTools`/`ContributeToolInfos`/`ContributeReturnDirectly`）。后者在 Agent 构建期先收集，早于 `BeforeAgent`。

**源码事实：** `ToolContributor` 的注释直说它取代「在 `BeforeAgent` 里改 `rc.Tools`」与 `BindToConfig` 这两种旧做法，理由是前者不传播到 `ToolsNode`、后者有时序耦合。

**源码事实：** filesystem 的 `BeforeAgent` 是空实现，注释说明保留是为了「直接调 `BeforeAgent` 的既有测试」，功能上无作用。

**源码事实：** summarization 压缩失败、`Finalize` 失败、以及摘要生成耗尽重试，三条失败路径都返回原状态，不向调用方报错。

**源码事实：** summarization 的默认 token 计数是「字符数 × 4/3」的粗估，不接真分词器。

**源码事实：** plantask 的包注释自认「不是中间件，是工具库」，并建议与 `prebuilt/deep` 的 `TaskManager` 去重。`Delete` 不处理子任务与父索引，会留下悬空引用。

**源码事实：** subagent 的 `built` 标志采用「锁内置位、锁外构建」，存在两个 goroutine 同时首次构建时后到者拿到空工具列表的窗口。

**源码事实：** `InheritParentMiddlewares` 对预构建的 `Agent` 静默失效；中间件排除按 `fmt.Sprintf("%T", mw)` 的类型名字符串匹配。

**架构推论：** 「贡献工具」与「改写状态」被分成两套接口，是因为它们的**生效时机根本不同**：工具必须在构建 `ToolsNode` 之前就位（否则模型看不到、调度不到），而状态改写发生在每轮模型调用前后。把两者塞进同一个钩子会逼着所有中间件都实现一个「我想加工具」的入口，并且时机由框架猜。分开之后，谁需要工具贡献谁实现可选接口，语义清晰。

**架构推论：** summarization 的三条失败路径全部降级为「不压缩」而不是报错，是一条明确的可用性优先原则：历史太长会让下一次模型调用超上下文而彻底失败，但不压缩只是这一轮容易失败。**「降级成已知的坏结果」优于「抛错终止」**——因为 Bad Case 还有机会被上层重试，而终止没有。

**架构推论：** plantask 的存在揭示了一个模式：有些能力既需要「状态」又需要「暴露给模型」，但它不属于任何一次性钩子。这类能力的正确归宿是「会话级状态 + 一组工具 + 一个初始化钩子」，而不是硬套中间件接口。包注释里的 TODO 就是这条认识的书面记录——**代码区划与概念区划不一致时，先把不一致写下来，比悄悄将错就错好**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/core/middlewares/summarization/summarization.go` | `NewTyped` | 泛型构造函数，补全阈值与计数器默认值 |
| `internal/harness/core/middlewares/summarization/summarization.go` | `BeforeModelRewrite` | 四阶段流水：剔除旧操作、判阈值、压缩、二次压缩与定稿 |
| `internal/harness/core/middlewares/summarization/summarization.go` | `shouldCompact` | 消息条数与估算 token 的双阈值判定 |
| `internal/harness/core/middlewares/summarization/summarization.go` | `generateSummary` | 手写指数退避重试，模型缺失或耗尽时返回占位摘要 |
| `internal/harness/core/middlewares/summarization/summarization.go` | `FinalizerBuilder.Build` | 摘要加保留最近若干条原文的定稿器 |
| `internal/harness/core/middlewares/filesystem/filesystem.go` | `ContributeTools` | 按配置产出七个文件系统工具 |
| `internal/harness/core/middlewares/filesystem/filesystem.go` | `BeforeAgent` | 空实现，仅为兼容直接调用方保留 |
| `internal/harness/core/middlewares/filesystem/filesystem.go` | `maybeTool` | 单工具的禁用、改名、自定义实现三档覆盖 |
| `internal/harness/core/middlewares/filesystem/filesystem.go` | `Backend` | 文件系统操作抽象，工具层只依赖它 |
| `internal/harness/core/middlewares/plantask/plantask.go` | `Manager` | 会话级任务表，父子注册在同一锁内完成 |
| `internal/harness/core/middlewares/plantask/plantask.go` | `TaskState` | 五值任务状态枚举 |
| `internal/harness/core/middlewares/plantask/plantask.go` | `InitManager` | 把任务表写进 run session |
| `internal/harness/core/middlewares/plantask/plantask.go` | `AllTools` | 五个任务管理工具的集合 |
| `internal/harness/core/middlewares/subagent/subagent.go` | `SubAgentSpec` | 子 Agent 声明：声明式配置、预构建、工厂三选一 |
| `internal/harness/core/middlewares/subagent/subagent.go` | `SubAgentMiddleware` | 贡献子 Agent 工具并在每轮注入元信息 |
| `internal/harness/core/middlewares/subagent/subagent.go` | `resolveAgent` | 三档 Agent 来源的优先级短路 |
| `internal/harness/core/middlewares/subagent/subagent.go` | `inheritedMiddlewares` | 按标记接口与类型名双重排除的中间件继承 |
| `internal/harness/core/middlewares/subagent/subagent.go` | `BindToConfig` | 已废弃的显式注入路径，带清空 `ToolsConfig` 的副作用 |
| `internal/harness/core/tool_contributor.go` | `ToolContributor` | 贡献工具的接口与三个收集函数 |
| `internal/harness/core/contracts.go` | `TypedReActMiddleware` | 五个生命周期钩子的接口定义 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：middlewares/summarization](https://github.com/infiniflow/ragflow/blob/main/internal/harness/core/middlewares/summarization/summarization.go)
- [ragflow 源码：middlewares/subagent](https://github.com/infiniflow/ragflow/blob/main/internal/harness/core/middlewares/subagent/subagent.go)
- [Google ADK 文档：回调与中间件](https://google.github.io/adk-docs/)

## 与主核 / AI 的关系

**两套接口对应主核里「钩子」与「注册」的区别。** 数据库的扩展点也分两类：一类是逐次调用的钩子（如每读一页时的替换策略），一类是初始化期注册（如注册一个索引访问方法）。前者改行为，后者改能力清单。ragflow 这里的 `TypedReActMiddleware` 是前者、`ToolContributor` 是后者——**两者的生效时机差一个数量级，混在一起会逼着框架去猜**。

**「失败降级为不优化」在主核里也很常见。** 优化器估算代价失败时会退回默认计划，而不是让查询失败；JIT 编译失败会退回解释执行。summarization 压缩失败退回原状态是同一动作。判据是：**优化能带来收益，但不是正确性前提**——凡是符合这个条件的路径，失败都该降级而不是中断。反过来，filesystem 的 `Backend` 缺失时返回空工具列表也是降级（少一组工具），而不是让 Agent 建不起来。

**会话级状态与检查点的边界。** plantask 把任务表挂在 run session 上，不进检查点。这意味着**中断恢复后任务表是空的**，模型看不到之前的任务清单。主核里同样有这种「进程内缓存不进持久化」的选择（如查询计划缓存），代价是重启后要重建。区别在于数据库能重建得便宜，而任务表重建需要模型重新创建——这是一个「状态归属」判断失误时的真实成本。

**兼容层的代价。** filesystem 的 `BeforeAgent` 空实现、`BindToConfig` 的废弃保留、plantask 的包位置，三处都是「代码区划已变、旧表面还在」。该仓 AGENTS 明确要求删除兼容层，这三处是尚未还清的账。**保留一个空实现不是零成本**：读代码的人必须先花时间确认「它什么也不做」，才能继续。

对 agent 工程最实用的五条：其一，**加工具要用 `ContributeTools`，不要用 `BeforeAgent` 改 `rc.Tools`**——后者改不到 `ToolsNode`。其二，**summarization 失败不报错**，历史没被压缩时不会有异常，只能从上下文超限的错误反推。其三，**plantask 的任务表不持久化**，中断恢复后需要重建。其四，**子 Agent 的 `MaxDepth` 靠 context 传递**，绕过 Agent 正常调用路径直接 invoke 子 Agent 会丢失深度限制。其五，**`InheritParentMiddlewares` 只对 `AgentConfig` 形式生效**，用预构建 Agent 时该字段静默无效。
