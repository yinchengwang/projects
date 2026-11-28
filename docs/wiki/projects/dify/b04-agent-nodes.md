---
project: dify
feature: agent-nodes
title: 两代 Agent 节点
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/nodes/agent/agent_node.py
    symbols: [AgentNode]
  - path: api/core/workflow/nodes/agent_v2/agent_node.py
    symbols: [DifyAgentNode]
  - path: api/core/workflow/nodes/agent_v2/dify_tools_builder.py
    symbols: [ProviderToolsLister, MCPProviderIDResolver]
  - path: api/core/workflow/nodes/agent_v2/output_adapter.py
    symbols: [WorkflowAgentOutputAdapter, ToolFileRebacker]
---

# 两代 Agent 节点

## 一句话职责

两个类共用同一个节点类型名 `AGENT`、版本号分别是 `"1"` 与 `"2"`：v1 在进程内通过插件侧的 agent strategy 跑，v2 把执行交给外部 Agent 后端（HTTP + SSE）并在本侧加上「冻结快照、输出路由、逐输出类型校验、会话快照落盘」。

## 机制是什么

这是全库最典型的「同类型两代实现并存」。两者对外的位置完全一样——都是 `BuiltinNodeTypes.AGENT` 的节点，在工作流 DSL 里只差一个 `version` 字段——但内部是两套完全不同的执行模型：

| 维度 | v1 `AgentNode` | v2 `DifyAgentNode` |
|---|---|---|
| 执行位置 | 进程内，经插件守护进程拿 strategy | 进程外，Agent 后端 HTTP + SSE |
| 编排来源 | strategy 的 `agent_parameters` | 每轮运行冻结的节点 job 配置 |
| 会话状态 | conversation_id 传给 strategy | 会话快照 + 工作区 + 分支 ID |
| 失败处理 | 直接产出失败事件 | 逐输出校验 + 重试/默认值/失败分支 |
| 输出形状 | 一条消息流 | 结构化声明输出 + 可选路由出口 |

b02 里那段「`AGENT` + `version=2` + 无 kind 标记 → 改判成 `"1"`」的兜底，正是为这两代共存而写的：旧 Agent 节点用 `version=2` 表示**工具参数格式 v2**，而 v2 节点用 `version=2` 表示**节点实现 v2**。同号不同义，只能靠数据里有没有 kind 标记来分。

两代的复杂度差了一个数量级，这个差距本身就是信息：**v1 把 Agent 循环交给别人，v2 把 Agent 循环也交出去了，但把所有「平台侧的关切」收回到自己这里**。v1 的目标是「能用插件里的 Agent 策略」，v2 的目标是「Agent 的输出要能被工作流可靠消费」——后者才需要输出路由、类型校验、重试编排、文件回填这一整套。

## 怎么实现的

**v1：四件依赖注入 + 一条消息流。**

构造函数收四个协作者：strategy 解析器、展示信息提供者、运行时支撑、消息转换器。全是从外部注入的，所以这个节点本身不碰插件、不碰数据库，只负责编排顺序。

`_run` 的流程是一条直线：解析 strategy（失败即产出失败事件并返回）→ 读 `strategy.get_parameters()` → 调 `build_parameters` **两次**（一次用于实际执行，一次 `for_log=True` 用于记录）→ 若开启了评测追踪则额外抓一次「模型可见的工具快照」，失败只记 warning 不中断 → 构造凭据 → 从系统变量取 conversation_id → `strategy.invoke()` 拿到消息流 → `_message_transformer.transform()` 转成节点事件。

「build_parameters 调两次」这个细节值得记：一次给执行、一次给日志，说明**日志里的参数与执行用的参数可能不同**（脱敏、裁剪）。用同一个函数加 `for_log` 开关而不是写两份，是让两者保持同构的最省办法。

`invoke` 与 `transform` 两处各有一个异常分支，都转成 `StreamCompletedEvent(FAILED)`。注意 `transform` 只捕获 `PluginDaemonClientSideError`——**插件守护进程侧的错才被当作可报告的失败**，其它异常会向上抛给引擎。

变量映射的提取按参数类型分派：`mixed` 与 `constant` 走模板解析器抽 selector，`variable` 直接取参数值当 selector。**`constant` 也走模板解析**这一点容易误解——名字叫「常量」但值里仍可写模板变量。最后统一给键加 `节点 ID + "."` 前缀。

**v2：状态机的形态，而不是一条直线。**

构造期就有两条硬规则：若启用了输出路由而节点的错误策略是「默认值」，直接抛错——**两者语义冲突**（路由要让节点选一个出口，默认值要让节点在失败时仍产出值，同时存在就说不清该走哪条）；启用路由时把 `execution_type` 改成 `BRANCH`，因为引擎要按分支节点调度它。

`_session_branch_id()` 值得单独讲，它是这个节点里最精巧的一段。它要回答「当前这次执行处于哪个并行分支上」，做法是沿容器帧链向上走：从 `node_executions` 里找到本执行 ID 对应的帧，然后用根运行时状态的帧表与运行表逐级回溯，每遇到「迭代运行且该迭代节点标了 `is_parallel`」就记一对（节点 ID, 索引），最后**反转、JSON 序列化、取 sha256**。

三个设计点：其一，**只有并行迭代产生分支**，串行循环不切分会话——因为串行迭代的每一轮对同一个 Agent 来说就是同一个连续对话。其二，用哈希而不是原串，是为了给下游一个固定长度、无路径泄漏的标识。其三，链断了（帧不存在、运行不存在、帧类型不匹配）**直接抛错而不是降级**，注释写的是「fails closed」——会话串台的代价比执行失败更大。

`_run_inner` 的主干是一个**重试循环**，而不是一次执行。每轮做：构造 runtime 请求 → `create_run` → 消费事件流 → 判断终态 →（结构化输出时）逐输出类型校验 → 失败则问编排器决定重试还是别的。`attempt` 同时传给请求构造器（后端可能按尝试序号做不同处理）和编排器。

**重试与「inputs 只取第一次」是一条刻意的取舍。** 代码里有注释：只在第 0 次尝试时写 `inputs["agent_backend_request"]`，否则重试会让工作流详情页里看到的输入不断变化。**记录「第一次请求」而不是「最后一次」**，因为排查者想知道节点当初收到的是什么。

**`stream_text` 的条件是三重合取。** 只有「没有自定义输出」且「未启用输出路由」且「错误策略不是默认值」时，才把模型增量当作流式文本块吐给下游。注释解释了为什么：一旦某个 selector 已经有流式块，响应过滤器就不再读变量池，此时配置的兜底 `text` 会被丢弃——**流式与兜底值互斥**，这是响应层机制对节点层施加的约束。

同理，工具活动投影（把工具调用生命周期转成日志事件）也只在 `stream_text` 为真时开启。结构化输出、路由、默认值三种运行**全程缓冲**，不投日志。

**事件流消费是一个带返回值的生成器。** `_consume_event_stream` 与 `_read_event_stream` 都用 `yield` 产出下游可见的流式块，再用 `return (terminal, failure)` 把终态交回调用方。这是 Python 生成器的 `StopIteration.value` 技巧，代码里为此加了 `noqa: B901` 并注释说明这是「生成器的完成结果」。

`_read_event_stream` 对事件类型的处理是分层的：`RUN_STARTED` 直接丢弃；`STREAM_EVENT` 记元数据并可能投影成日志；`AGENT_MESSAGE_DELTA` 记计数并可能吐流式块；**其余事件必须是三种终态之一**，否则取消后端运行并按「非预期事件」失败。所以「未知事件」不是被忽略，而是被当成协议违约。

三条退出路径都做同一件事——**取消后端运行并等待**（`cancel_run_and_wait`，带 `after=last_event_id`）。分别是：非预期事件、后端错误、流意外结束。最后那条的 reason 会区分「图被中止」与「流自己断了」，因为两者的归因不同。取消失败只记 warning，不覆盖原始错误。

**输出侧的四个协作者各管一段。**

`WorkflowAgentToolLayers`（工具层构建产物）是一条冻结的 dataclass，带 `plugin_tools`、`core_tools` 与 `tool_labels`。注释特别强调 labels 是**脱离运行时、不含凭据的展示快照**，不会进凭据配置。`exposed_tool_names()` 把两侧的工具名合起来，给上层做重复检查。

构建器的 `build_layers` 有一串值得记的行为：

- **provider 级条目会展开**。`tool_name` 为空意味着「这个 provider 下的全部工具」，于是去查该 provider 当前声明的工具名列表，逐个展开成具体条目；已被显式列出的名字不再重复展开。
- **MCP 的 provider 引用要被规范化**成运行时侧的 server identifier（持久化的可能是行 ID，也可能是标识符本身）。
- **`knowledge_fs` 的 research 工具会按知识空间裂变**：一个条目变成 N 个，每个暴露名加 `_{index+1}` 后缀，描述里补上空间名、别名与描述。注释解释了这个设计意图——模型只看到 query 与 mode，空间的 `control_space_id` 通过一个**隐藏表单参数**注入，所以「另一个空间」对模型表现为「另一个工具」。
- **重名直接报错**，不静默覆盖。
- **插件 vs 核心工具分流**：插件类走原有的守护进程直连，内置/api/workflow/mcp 走「core tools」通道。数据集检索被显式拒绝，因为它走知识路径。
- **`max_calls_per_run` 在两条通道上都保留**，注释写明是为了「暴露另一个名字不能增加配额」。
- 缺必填运行时参数、凭据不是标量、参数 schema 非法，都抛带稳定错误码的异常。

`WorkflowAgentOutputAdapter` 负责把后端终态事件变成节点运行结果。它的输出归一化有四层优先级，顺序很关键：已是文件类型就原样返回；**先试旧式 ToolFile 回填**；再按声明类型转换；都不是就原样保留。

「先试旧式回填」这条依赖 `ToolFileRebacker` 这个协议，触发条件极窄：值必须是**键集恰好等于 `{"id"}`** 的映射，且回填器与 tenant_id 都非空。满足时从服务端 ToolFile 行取真实文件，**而不是相信沙箱给的内容**——注释点明这是「让下游元数据以服务端为准」。这是安全边界，不是兼容性糖。

声明类型转换只处理两种：`FILE` 且值是映射 → 文件段；`ARRAY[FILE]` 且值是映射列表 → 文件数组段。其余声明类型不改值。

文件载荷的校验是严格的：`transfer_method` 必须存在；**键集必须与该传输方式要求的集合完全相等**（远程 URL 要 `transfer_method` + `url`，其余要 `transfer_method` + `reference`），多一个键少一个键都抛错。远程 URL 直接构造文件并显式置 `size=-1`、元数据全空——**因为不下载就不知道真实元数据，填假值比留空更糟**。其余方式要求 reference 是规范形态，且 `tenant_id` 必填，然后交给服务端文件工厂还原元数据。

`ToolFileRebacker` 与 `ProviderToolsLister`、`MCPProviderIDResolver` 一样，都是 `Protocol` 而不是具体类型。这是刻意的 DI 缝：让节点逻辑可以脱离数据库与插件系统被测试，也让「回填」这个动作可以被替换。

## 关键取舍

**源码事实：** 两个类都是 `BuiltinNodeTypes.AGENT`，版本分别 `"1"` 与 `"2"`；b02 的版本兜底正是为消歧这两者而存在。

**源码事实：** v1 的四个协作者全由外部注入；`build_parameters` 被调用两次，第二次带 `for_log=True`。

**源码事实：** v1 只把 `PluginDaemonClientSideError` 转成失败事件，其余异常上抛；v2 则在 `_run` 层兜住所有异常，但**仅当 `process_data` 非空时才转成失败事件**，否则重新抛出。

**源码事实：** v2 构造期拒绝「输出路由 + 默认值错误策略」的组合，并在启用路由时把 `execution_type` 改成 `BRANCH`。

**源码事实：** `_session_branch_id` 只对并行迭代记录分支，对串行循环返回空串；容器链不完整时抛错（fails closed）。

**源码事实：** `stream_text` 需同时满足「无自定义输出」「未启用输出路由」「错误策略非默认值」；工具活动投影也只在 `stream_text` 时开启。

**源码事实：** 事件流三条异常退出路径都会取消后端运行并传 `after=last_event_id`；取消失败只记 warning。

**源码事实：** provider 级工具条目会展开成该 provider 声明的全部工具；`knowledge_fs` 的研究工具按知识空间裂变并加序号后缀；暴露名重复直接报错。

**源码事实：** 旧式 ToolFile 回填只在键集恰为 `{"id"}` 时触发，且要求 tenant_id；远程 URL 文件的元数据显式为 `size=-1` 与空。

**架构推论：** 「两代并存」在这里不是过渡状态，而是**产品语义确实变了**：v1 的 Agent 是「插件里的一个策略」，v2 的 Agent 是「外部服务 + 冻结配置 + 结构化输出」。这两者的差异大到无法用一个类加开关表达，所以只能开两个版本号。代价是 b02 里那段永久兜底，以及「读工作流代码时必须先确认版本」。

**架构推论：** v2 的复杂度几乎全部服务于**可靠性**而不是功能：重试编排、类型校验、会话快照、取消失等、失败分支。这是「把 Agent 嵌进生产工作流」的必然成本——v1 那种「一条消息流到底」的模型在出问题时不提供任何处置手段，只能整节点失败。

**架构推论：** 三处 `Protocol` 注入（provider 列表器、MCP 解析器、文件回填器）都在**跨越持久化或子系统边界**的位置。这不是为了抽象而抽象：它们背后分别是数据库查询、MCP 服务与文件工厂，都是「节点逻辑不该知道」的东西。**判据是「这个依赖会不会把节点的测试拖进整个基础设施」**，会的话就用协议隔开。

**架构推论：** 「旧式回填」与「严格键集校验」并列，说明这一层同时在做两件相反的事：对历史数据宽容（认得裸 ID），对新数据严格（键多了就拒）。这与 b03 的宽进严出是同一种模式，而它的成立条件是**历史形态是可枚举的**——一旦旧形态多于一种，宽进就会变成猜谜。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/nodes/agent/agent_node.py` | `AgentNode` | 第一代 Agent 节点，进程内策略执行 |
| `api/core/workflow/nodes/agent/agent_node.py` | `AgentNode._run` | 解析策略、构造参数与凭据、消费消息流 |
| `api/core/workflow/nodes/agent/agent_node.py` | `AgentNode._extract_variable_selector_to_variable_mapping` | 按参数类型分派抽取 selector |
| `api/core/workflow/nodes/agent_v2/agent_node.py` | `DifyAgentNode` | 第二代 Agent 节点，外部后端执行 |
| `api/core/workflow/nodes/agent_v2/agent_node.py` | `DifyAgentNode._run_inner` | 绑定解析、重试循环、类型校验、路由选择 |
| `api/core/workflow/nodes/agent_v2/agent_node.py` | `DifyAgentNode._session_branch_id` | 并行迭代分支标识，链断即失败 |
| `api/core/workflow/nodes/agent_v2/agent_node.py` | `DifyAgentNode._consume_event_stream` | 流式块投影与终态返回 |
| `api/core/workflow/nodes/agent_v2/agent_node.py` | `DifyAgentNode._read_event_stream` | 逐事件分派、未知事件即取消 |
| `api/core/workflow/nodes/agent_v2/agent_node.py` | `DifyAgentNode._save_session_snapshot` | 快照落盘，失败只记元数据不影响结果 |
| `api/core/workflow/nodes/agent_v2/dify_tools_builder.py` | `WorkflowAgentToolLayers` | 工具层构建产物，含无凭据展示标签 |
| `api/core/workflow/nodes/agent_v2/dify_tools_builder.py` | `ProviderToolsLister` | provider 工具名列表的注入协议 |
| `api/core/workflow/nodes/agent_v2/dify_tools_builder.py` | `MCPProviderIDResolver` | MCP provider 引用规范化的注入协议 |
| `api/core/workflow/nodes/agent_v2/dify_tools_builder.py` | `WorkflowAgentDifyToolsBuilder.build_layers` | 展开、规范化、分流、裂变、校验 |
| `api/core/workflow/nodes/agent_v2/output_adapter.py` | `WorkflowAgentOutputAdapter` | 终态事件到节点运行结果的转换 |
| `api/core/workflow/nodes/agent_v2/output_adapter.py` | `WorkflowAgentOutputAdapter._normalize_outputs` | 四层优先级的输出归一 |
| `api/core/workflow/nodes/agent_v2/output_adapter.py` | `WorkflowAgentOutputAdapter._file_from_payload` | 严格键集校验与远程文件构造 |
| `api/core/workflow/nodes/agent_v2/output_adapter.py` | `ToolFileRebacker` | 服务端 ToolFile 回填协议 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [graphon：dify 外置的图执行内核](https://github.com/langgenius/graphon)
- [dify 源码：nodes/agent_v2/agent_node.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/nodes/agent_v2/agent_node.py)
- [dify 文档：Agent 节点](https://docs.dify.ai/guides/workflow/node/agent)

## 与主核 / AI 的关系

**「两代实现并存」与主核的执行器代际更替是同一类问题，但收敛方式相反。** 数据库换执行器时通常**不做双版本并存**——旧计划在升级后直接按新执行器跑，因为执行器对用户不可见。dify 做不到这一点，因为版本号写在用户的工作流 DSL 里，是**用户可见的契约**。判据很清楚：**版本号一旦进入用户数据，就永久不能删**。

**「fails closed」与主核的隔离级别选择同构。** 会话分支链不完整时抛错而不是猜一个空分支，等价于数据库在无法确定读视图时宁可报错也不给一个可能错的一致视图。**正确性可疑时选择失败**是两者的共同取向，代价是可用性。

**「只记第一次请求」与主核的慢日志采样一致。** 采样时数据库记的是「第一次的绑定参数」而不是最后一次重试的，因为排查需要的是原始意图。**日志要记录的是原因，不是结果**——重试改变的是结果，原因只有一个。

**「不信沙箱给的元数据」与主核的外部数据校验一致。** 那边的做法是内容嗅探而非后缀信任，这边的做法是用 ID 回数据库取权威行。共同点：**跨越信任边界的数据，只用来做索引，不用来当事实。**

**输出归一化的四层优先级，与主核的类型转换等级类似但更危险。** 数据库的隐式转换有明确的等级与方向；这里的「先试旧式、再按声明、否则原样」是**按值形状逐层试探**，一旦某层的判据过宽就会误判（例如一个恰好只有 `id` 键的正常对象会被当文件）。**当类型信息缺失时，形状匹配的每一层都是在赌。**

对 agent 工程最实用的五条。其一：**先确认节点版本再读实现**，v1 与 v2 是两套模型，混看会持续困惑。其二：**输出路由与默认值错误策略不能同时开**，构造期就会拒绝。其三：**结构化输出、路由、默认值三种运行不流式**，下游要按「一次性拿到」处理。其四：**旧式 ToolFile 回填是安全机制不是兼容糖**，网关侧不要喂带 `id` 的对象当普通字段。其五：**并行迭代会切分会话，串行循环不会**，理解 Agent 记忆边界时要按这条来推。
