---
project: ragflow
feature: prebuilt-agents
title: 预制 Agent 形态
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/core/prebuilt/deep/deep.go
    symbols: [TaskManager, NewWithSubAgents]
  - path: internal/harness/core/prebuilt/supervisor/supervisor.go
    symbols: [AgentSpec]
---

# 预制 Agent 形态

## 一句话职责

`prebuilt` 把「一组配置 → 一个可用的 Agent」这段装配工作固化下来：deep 走「主 Agent 派子任务」，supervisor 走「主 Agent 只做路由」，coding 走「文件工具 + 权限白名单」，三者都是 `ReActAgent` 的预配置外壳。

## 机制是什么

这三者不是新的执行引擎，而是**同一套 ReAct 内核的三种典型装配**。它们的差别在于「主 Agent 拿到什么工具、系统提示怎么写」：

| 形态 | 主 Agent 的角色 | 工具来源 | 子 Agent 关系 |
|---|---|---|---|
| deep | 分解任务并自己执行 | 用户工具 + 三个任务管理工具 + 可选 shell | 子 Agent 作为可移交对象 |
| supervisor | 只判断「该交给谁」 | 无（不给自己工具） | 子 Agent 带「只能交回主管」约束 |
| coding | 读写代码 | 七个文件工具 + 可选受限 shell | 可挂子 Agent 中间件 |

关键在于 deep 与 supervisor 的**分工逆转**：deep 的主 Agent 是干活的那个，子 Agent 是它派出去的分身；supervisor 的主 Agent 几乎不干活，全部工作交给子 Agent。两者复用的是同一个 `SetSubAgents` 装配函数，差别只在系统提示与是否给主 Agent 工具。

## 怎么实现的

**deep 的装配是「默认值补全 + 工具拼装」。** `NewTyped` 依次填三类默认：`MaxIterations` 缺省 20、`Name` 缺省 `deep_agent`、`Instruction` 缺省内置提示。然后做一件值得单独看的事：若设了 `OutputKey`，**把「把最终答案存到会话的某个键下」这句话拼到系统提示末尾**。也就是说输出键不只用于落值，还要在提示里明说——否则模型不知道要把结论写哪儿。

工具拼装顺序是：用户工具 → 三个任务管理工具（创建、列出、更新） → 可选 shell。任务管理器 `TaskManager` 在这里被 `NewTaskManager()` 创建，**没有暴露给调用方**，所以一个 deep Agent 任务表是私有的、生命周期等于 Agent 实例。

`cfg.SubAgents` 在 `NewTyped` 里完全不参与——它只在 `NewWithSubAgents` 里被读取。后者的逻辑极短：先建出基础 deep Agent，若没有子 Agent 就直接返回；否则把每个 `SubAgentSpec` 的 `Agent` 收成一个列表交给 `core.SetSubAgents`。

**`SetSubAgents` 是三者共用的装配点，它的行为需要看清楚。** 它把传入的 Agent 包成 `flowAgent`（若还不是），补上默认的历史改写器，然后：
- 若已有子 Agent 就报错「sub-agents already set」——**只允许设置一次**，不可增量追加；
- 每个子 Agent 都被 `toFlowAgent` 包一层并带上 `WithDisallowTransferToParent()`。

最后那条是整个多 Agent 结构的安全阀：子 Agent 默认**不允许把控制权交回父 Agent**，否则「主管交给专家、专家再交回主管」会形成无限来回。deep 用它实现「派出去的子任务不再回头」，supervisor 用它实现「专家干完直接结束，不回到主管」。

值得注意的是这里有一处已知的双记账问题，写在 `flow.go` 的注释里：某些路径会既创建子 Agent、又把它们注入 `flowAgent.subAgents`，造成「workflowAgent 与 flowAgent 各存一份」。这不是设计而是待清理的历史包袱。

**supervisor 的装配是「提示即路由表」。** `New` 先做唯一一个硬校验：`Model` 为空直接报错（deep 不校验这个，因为 deep 可能靠 failover 模型）。然后 `buildAgentDescriptions` 把每个 `AgentSpec` 拼成一行「- 名字: 描述」，塞进系统提示的 `%s` 占位。

**这个提示就是路由表本体。** 提示里明确要求「用 `transfer_to_agent` 工具把任务交给选中的 Agent」，而主管自己的 `ReActConfig` **只设了模型与指令、没有工具**。于是主管能做的唯一动作就是移交——移交能力不是工具列表里的某项，而是 `flowAgent` 层的机制。

子 Agent 在这里多包了一层 `AgentWithDeterministicTransfer`。这层包装的作用是：把子 Agent 的输出事件原样转发，同时在**末尾追加一条移交事件**，指向 `cfg.Name`（即主管名字）。转发逻辑里有两条早退：若最后一个事件带有中断或退出动作，就不追加移交——因为那两种情况下控制流已经另有出口，再追加移交会把「中断等人」变成「移交给人」。

`NewWithRouter` 是「纯路由」的便捷入口，它对 `Config` 只填模型与 Agent 列表，其余全用零值。于是它连 `Name` 都是空的——这意味着子 Agent 的「只能交回」目标名是个空串。这是便捷构造器的隐含代价。

**coding 的装配是「中间件栈」，与前两者不同层。** 它不碰 `SetSubAgents`，而是按顺序往 `Middlewares` 里塞：shell 白名单（**在文件系统之前**，注释写明是为了拦截 `execute` 调用）、文件系统中间件、子 Agent 中间件。顺序有实际语义：白名单先于文件系统，才能对「执行 shell」这件事做前置否决。

三个中间件里只有 filesystem 用 `ContributeTools` 贡献工具，所以 coding 的工具集来自中间件而非 `cfg.Tools`——这与 deep 直接往 `Tools` 里塞工具是两种不同的装配哲学：**deep 用直接工具列表，coding 用中间件贡献**。前者直观、后者可复用。

**任务管理的两处重复。** `prebuilt/deep` 有一个 `TaskManager`，`middlewares/plantask` 也有一个 `Manager`，两者职责重叠（都是任务表 + 管理工具）。plantask 的包注释已经点明建议「整合其 TaskManager 与 prebuilt/deep 的 TaskManager 以消除重复」。这是本组里最明确的待偿技术债。

**语言选择的两套机制。** deep 里有一个按语言键查提示的映射表（英文与中文两份系统提示），并暴露 `SelectPrompt(lang)`；但 `NewTyped` 并不调用它，而是直接用包级 `systemPrompt` 常量。所以**语言选择器存在但未接线**——想用中文提示必须自己在 `cfg.Instruction` 里传。

**supervisor 的类型识别缺失。** `New` 里有一条 TODO，直说「`NewReActAgent` 返回具体类型，所以无法轻易给 supervisor 加 `GetType` 来做识别」。这条留白有实际后果：拿到的 `ResumableAgent` 无法区分自己是不是 supervisor，只能靠名字判断。

## 关键取舍

**源码事实：** deep、supervisor、coding 都不实现新的执行引擎，全部收敛到 `ReActAgent`；差别只在提示、工具集与是否挂子 Agent。

**源码事实：** deep 的 `OutputKey` 既用于落值也拼进系统提示；`TaskManager` 由 `NewTyped` 内部创建，不对外暴露。

**源码事实：** `cfg.SubAgents` 只在 `NewWithSubAgents` 里读取，`NewTyped` 完全忽略它——直接调 `NewTyped` 拿到的 Agent 不会有任何子 Agent。

**源码事实：** `SetSubAgents` 只允许设置一次（已有子 Agent 即报错），且给每个子 Agent 加 `WithDisallowTransferToParent`。

**源码事实：** supervisor 的主 Agent 不设任何工具，只靠 `flowAgent` 的移交机制与提示里的 `transfer_to_agent` 约定。

**源码事实：** 子 Agent 的确定性移交包装在末尾追加一条指向主管的移交事件，但最后一个事件若是中断或退出则跳过追加。

**源码事实：** `NewWithRouter` 不设 `Name`，因此子 Agent 的「交回目标」是空串。

**源码事实：** coding 的中间件顺序是「shell 白名单 → 文件系统 → 子 Agent」，白名单在前是为了拦截执行类调用。

**源码事实：** deep 有按语言选提示的 `SelectPrompt`，但 `NewTyped` 不调用它；`prebuilt/deep` 与 `middlewares/plantask` 的任务管理职责重复。

**源码事实：** supervisor 没有类型标识，源码 TODO 明确记录了这一点与原因。

**架构推论：** 「预制」在这里的含义很具体：**把「提示措辞 + 工具清单 + 子 Agent 装配」这套每次都要重写的样板固化下来**。它不提供新的能力边界，提供的是「正确的样板」。这也解释了为什么 deep 与 supervisor 能共用 `SetSubAgents`——多 Agent 结构只有一种实现，差别全在配置。

**架构推论：** supervisor 不给主 Agent 工具，是一个刻意的约束设计：**如果主管自己也能干活，它就会倾向于自己干而不是分派**。把工具拿走，模型的选择空间只剩「交给谁」或「直接回答」两种，路由行为因此变得可预测。这与「给 Agent 全部能力让它自己判断」是完全相反的取向，取舍点在于「可预测性」优先于「灵活性」。

**架构推论：** 子 Agent 的「不许交回父」约束是这类多 Agent 结构能成立的前提。没有它，「主管 → 专家 → 主管」的循环会让整个系统在两次移交之间空转，而每次移交都要重建历史（`flow.go` 里那条 TODO 提到「每次移交都要重放全部历史事件，复杂度随会话长度线性增长」）。约束一次移交方向，等于同时挡住了无限循环与线性的历史重建成本。

**架构推论：** 语言选择器未接线、`NewWithRouter` 不设名字、任务管理重复——这三处都是「已识别但未完成」的痕迹。它们共同说明一件事：**预制层是快速堆叠出来的**，比内核层欠打磨。读这一层时应当以源码为准，不要假设某个字段真的生效。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/core/prebuilt/deep/deep.go` | `TaskManager` | deep 私有的任务表，由构造期创建 |
| `internal/harness/core/prebuilt/deep/deep.go` | `NewTyped` | 补默认值、拼输出键提示、组装工具集 |
| `internal/harness/core/prebuilt/deep/deep.go` | `NewWithSubAgents` | 建基础 Agent 后挂子 Agent 列表 |
| `internal/harness/core/prebuilt/deep/deep.go` | `SubAgentSpec` | deep 视角的子 Agent 声明（名字、描述、实例） |
| `internal/harness/core/prebuilt/deep/deep.go` | `SelectPrompt` | 按语言取系统提示，当前未被构造路径调用 |
| `internal/harness/core/prebuilt/supervisor/supervisor.go` | `AgentSpec` | supervisor 的子 Agent 声明 |
| `internal/harness/core/prebuilt/supervisor/supervisor.go` | `New` | 拼路由提示、建无工具主 Agent、包装子 Agent |
| `internal/harness/core/prebuilt/supervisor/supervisor.go` | `NewWithRouter` | 纯路由便捷入口，其余配置取零值 |
| `internal/harness/core/prebuilt/supervisor/supervisor.go` | `buildAgentDescriptions` | 把子 Agent 列表渲染成提示里的路由表 |
| `internal/harness/core/prebuilt/coding/coding.go` | `Config` | coding 装配的全部旋钮，含子 Agent 与 shell 白名单 |
| `internal/harness/core/prebuilt/coding/coding.go` | `New` | 按序搭中间件栈并构造 ReActAgent |
| `internal/harness/core/flow.go` | `SetSubAgents` | 三者共用的子 Agent 装配点 |
| `internal/harness/core/flow.go` | `defaultHistoryRewriter` | 跨 Agent 移交时的历史改写规则 |
| `internal/harness/core/agent_handoff.go` | `AgentWithDeterministicTransfer` | 给子 Agent 追加确定性的交回移交 |
| `internal/harness/core/agent_handoff.go` | `forwardEventsAndAppendTransfer` | 转发事件并在末尾补移交，中断与退出时跳过 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：prebuilt/deep/deep.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/core/prebuilt/deep/deep.go)
- [ragflow 源码：prebuilt/supervisor/supervisor.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/core/prebuilt/supervisor/supervisor.go)
- [AutoGen 文档：多 Agent 编排模式](https://microsoft.github.io/autogen/)

## 与主核 / AI 的关系

**「预制」等价于主核里的执行计划模板。** 数据库不会让用户手写每个算子的连接方式，而是提供「索引扫描 + 嵌套循环」这类现成形状，参数化后即用。deep 与 supervisor 就是两种现成的多 Agent 形状：一个「分而治之」，一个「路由分发」。**形状固定、参数可变**，这是预制层存在的全部价值。

**拿走能力以换取可预测性，与主核的权限分级同构。** 数据库里只读账号不能写，不是因为它做不到，而是因为「不该做」比「能做但不做」更可靠。supervisor 不给主 Agent 工具正是这个逻辑：**用能力边界代替提示词约束**。提示词可以被模型无视，缺少工具则无法无视。

**「只允许设置一次」与主核的不可变配置一致。** `SetSubAgents` 第二次调用直接报错，而不是覆盖或追加。主核里很多配置在启动后即冻结（线程池大小、缓冲区布局），理由相同：**可变的结构让所有依赖它的缓存与索引失效**。这里的代价是「想换子 Agent 就得重建 Agent 实例」。

**线性的历史重建是一处真实的性能债。** `flow.go` 的 TODO 指出每次移交都要重放全部历史事件。这与主核里「每次查询都重新解析 SQL」是同一类问题——功能正确但每次都付全量成本。主核的解法是计划缓存，这里需要的则是「按 Agent 缓存重建结果并在状态变更时失效」。

对 agent 工程最实用的五条：其一，**deep 的子 Agent 只在 `NewWithSubAgents` 里生效**，直接调 `NewTyped` 会拿到一个没有子 Agent 的 Agent。其二，**supervisor 的路由能力依赖提示里的 `transfer_to_agent` 约定**，改提示时要保留这条指令，否则主管会退化成「直接回答」。其三，**子 Agent 不许交回父 Agent**是防循环的关键，自定义多 Agent 结构时应保留这条约束。其四，**coding 的 shell 白名单必须排在文件系统中间件之前**，顺序调换会让执行类调用绕过后置检查。其五，**每处「已识别未完成」都要按源码判断**——语言选择器未接线、`NewWithRouter` 名字为空、任务管理有两份实现，这些都不能靠命名想当然。
