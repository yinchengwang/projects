---
project: dify
feature: workflow-entry
title: 工作流入口与图引擎边界
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/workflow_entry.py
    symbols: [WorkflowEntry, iter_dify_graph_engine_events]
  - path: api/core/workflow/graph_topology.py
    symbols: [WorkflowGraphTopology]
---

# 工作流入口与图引擎边界

## 一句话职责

`WorkflowEntry` 是本仓与外部图引擎 `graphon` 之间的唯一接缝：它把图对象、运行时状态、命令通道与三层装饰交给引擎，再把引擎的原始事件流套上 dify 自己的兼容过滤；同文件的 `WorkflowGraphTopology` 则在完全不执行图的前提下，对草稿 JSON 做一次静态拓扑问答。

## 机制是什么

这一页要先把边界画清楚，因为 dify 的图执行内核**不在这个仓里**。

`api/pyproject.toml` 依赖外部的 `graphon` 包（`langgenius/graphon`，pin 到具体 commit）。超步循环、通道合并、检查点、容器节点（循环/迭代）的推进，全在那边。本仓的 `api/core/workflow/` 只做三件事：**提供节点类**（`node_factory`，见 b02）、**装配运行时依赖**（`node_runtime`，见 b08）、**把引擎接进应用层**（本页）。

所以 `WorkflowEntry` 的角色不是「执行工作流」，而是「**为一次执行准备好引擎所需的全部上下文，并翻译它的输出**」。判断某个能力在这边还是那边，有个可靠的判据：**凡是需要 dify 的产品概念（租户、应用、用户、限流配置、文件访问控制、可观测性）的，在这边；凡是只需要图语义（节点、边、通道、状态）的，在那边。**

这解释了为什么这个类构造函数里堆了这么多东西——`file_runtime`、`ExecutionContextLayer`、`ExecutionLimitsLayer`、三个容器处理器工厂、可选的 `ObservabilityLayer`。它们全都是「dify 的关切」，引擎不关心，但引擎提供了挂载点。

本页还带一个看起来无关的类：`WorkflowGraphTopology`。它跟执行毫无关系，只解析 `Workflow.graph` 那份 JSON 里的 `nodes` 与 `edges` 做可达性判断。放在同一篇的理由是它们构成一对**对照**：一个靠引擎跑真实图，一个完全绕开引擎只读图结构；一个回答「会发生什么」，一个回答「这个图长什么样」。

## 怎么实现的

**构造函数的第一件事是深度校验。** 工作流可以调用工作流（作为工具），所以有递归风险。`call_depth` 与 `configs.dify_config.WORKFLOW_CALL_MAX_DEPTH` 比较，超过即抛 `ValueError`。注意这个判断在**构造期**而不是运行期——一个超深的调用在还没跑任何节点时就失败了。

**命令通道默认走内存，也可能走 Redis。** `command_channel` 参数为空时用 `InMemoryChannel()`。非空的场景是「暂停后恢复」：暂停时引擎把状态与通道一起持久化，恢复时把同一个 Redis 通道传进来，于是外部命令（停止、继续）能跨进程送达。这是本仓少见的「传输实现可替换」的地方，而它之所以可替换，是因为引擎把「收命令」抽象成了接口。

**响应流过滤器必须跨暂停复用同一实例。** `response_stream_filter` 的 docstring 写得很直白：调用方传入的实例必须是打算在暂停时持久化的那一个，因为过滤器内部维护 `paths_map`，记录**本次运行实际流出过哪些变量路径**。若恢复时新建一个空过滤器，引擎就不知道之前已经发过哪些路径，增量语义会错。这条约束是隐式的——类型上看它只是个可选参数，语义上它承载跨生命周期的状态。

**三层装饰按条件挂载。** `ExecutionContextLayer` 与 `ExecutionLimitsLayer` 无条件加；`ObservabilityLayer` 只在 `ENABLE_OTEL` 或 instrument flag 打开时加。限流层从配置读两个上限：最大步数与最大时间。这两个是**防跑飞的硬闸**，与节点级的循环守卫（b08 之外的机制）是不同层次的东西。

**三个容器处理器工厂是关键的类型分派点。** 循环与迭代这两个容器各被包一层 `WorkflowToolNestedContainerHandler`，再额外挂一个 `WorkflowToolContainerHandler`。三者的 `handler_factory` 或构造参数不同，但共享同一个 `limits_layer.on_event` 作为「隐藏事件监听器」——也就是说**容器内部的节点事件要同时喂给限流层**，否则一个十万次循环的容器会让步数上限失效。

`WorkflowToolContainerHandler` 还多接了三样：`sources`（工具来源表，运行期被填充）、一个事件监听器工厂、以及 `execution_context_factory`。所以「工作流被当成工具调用」这件事在执行层表现为**一个特殊的容器处理器**——它是引擎的扩展点，不是 dify 自己写的循环。

**`iter_dify_graph_engine_events` 是那个翻译层。** 它只做一件事：把 `engine.run()` 套上 `filter_engine_events`，并给两个过滤器：`HumanInputFormEventFilter`（表单事件过滤，接一个仓储做持久化查询）与 `ResponseStreamFilter`。函数 docstring 说明了为什么需要这层：**graphon 发的是原始变量流块，调用方必须显式选择加入 dify 想要的「按响应顺序」的旧流式行为**。换言之，事件顺序的「正确」定义在 dify 这边，引擎不负责。

**`run()` 的异常处理有两个不对称分支。** `GenerateTaskStoppedError` 被**静默吞掉**（`pass`），因为「用户主动停止」不是错误；其它任何异常都记日志、产出一个 `GraphRunFailedEvent` 并 `return`。所以调用方拿到的事件流在失败时是「正常结束 + 一个失败事件」，而不是抛出。这个设计让上层统一按事件流处理，代价是**调用方必须自己检查事件里有没有失败**，忘了就会把失败当成功。

**三种运行模式，能力边界各不相同。**

`run()` 是完整工作流。`single_step_run()` 是调试单个节点：从 `Workflow` 里取出该节点配置、解析节点类、建一个最小的 `RuntimeState`、把缺失变量从草稿池加载进来、再构造节点并跑。它显式拒绝循环与迭代节点（`raise ValueError`），理由是这两类必须走「引擎支撑的调试端点」——因为单个容器节点的执行语义与独立节点不同。

`single_step_run` 里还有一段值得单独看：**新式 Agent 节点的元数据过滤条件也要进变量映射**。代码从 `WorkflowAgentBindingResolver` 取回该节点的 Agent 配置快照，遍历其知识集的 manual 过滤条件，把条件值里的模板变量抽成 selector，加进 `variable_mapping`（键形如「节点 ID + metadata_filter + 路径」）。注释解释了为什么必须在这里做：这些条件只参与检索过滤，不出现在任务提示里，所以常规的变量映射收集不到。**同时它跳过了 `empty` 与 `not empty` 两个操作符**——原因是这两个操作符不消费值，因此不引入变量依赖。

`run_free_node()` 更窄：只允许参数提取器与问题分类器两类节点，版本硬编码成 `"1"`。它甚至不要求 `Workflow` 实例存在，自己拼一个「开始节点 → 目标节点」的最小图。这两个限制合起来说明它的定位：**画布上「单独试跑这个节点」按钮的后端**，不是通用执行入口。

**`mapping_user_inputs_to_variable_pool` 的排序是个隐式契约。** 它先按 `variable_selector` 的长度排序，注释说明理由：「整对象先于嵌套覆盖，与编辑器字段顺序无关」。也就是说同一次输入里既有整个对象、又有它的某个子字段时，必须先写整体再写子字段，否则子字段会被整体覆盖回去。这个顺序不是装饰性的，是正确性要求，而它靠排序保证、不靠文档约定。

方法本身的注释很值得引：作者写明「THIS METHOD'S SEMANTICS ARE NOT CLEARLY DEFINED, AND MULTIPLE PARTS OF THE CODEBASE DEPEND ON ITS CURRENT BEHAVIOR. MODIFY WITH CAUTION」。另一处注释要求它必须与 `load_into_variable_pool` 的存在性检查逻辑保持同步——**两份实现做同一件事，靠注释维系一致**。

它内部还有个形态嗅探：字典且含 `type` 与 `transfer_method` 两个键就当文件对象构造，列表且全部元素满足同一条件就当文件列表。这是「按形状判断语义」的又一例（与 b02 里 `version="2"` 的兜底同类）。

**`handle_special_values` 是一个带自我否定注释的工具函数。** 它递归遍历结构，把 `File` 对象转成字典。函数上方写着「避免在新代码里使用；尽量保持值结构化，只在序列化前一刻转换」。但它仍是全仓最广被调用的方法之一——响应转换、持久化、节点调试结果都在用它。**这正是一个「已知不好但已扩散」的接口的典型形态**：注释劝阻新增使用，存量却无处不在地依赖它。

**`WorkflowGraphTopology` 完全不碰引擎。** 它从 JSON 里抽两样东西：节点 id 集合、以及「目标 → 来源列表」的反向邻接表。三个能力：`has_node`、`is_upstream`（BFS，起点相等返回假）、`upstream_node_ids`（BFS 后与真实节点集求交）。

`upstream_node_ids` 的 docstring 点出一个真实的数据形态：**边可以引用 `nodes` 里不存在的 id**，注释称之为「half-deleted graphs」，即删除操作没清理干净的草稿。所以「返回上游」不是返回 BFS 访问到的全部，而是**与真实节点集求交**。这是防御式设计：把脏数据当成常态而不是异常。

这个类的提取理由写在模块 docstring 里：两个调用点（Agent v2 的发布校验、以及 composer 候选端点）需要解析同一份 JSON 形状，所以从校验器里抽出来共享。**这说明它的归属判据是「共享同一个解析」而不是「同属工作流执行」**——与 b02 里 `get_default_root_node_id` 因为共享「什么算入口」而留在节点工厂是同一个原则。

## 关键取舍

**源码事实：** 图执行内核在外部包 `graphon`，`WorkflowEntry` 只负责构造引擎、挂三层装饰、提供容器处理器工厂与命令通道。

**源码事实：** `call_depth` 与 `WORKFLOW_CALL_MAX_DEPTH` 的比较发生在构造函数里，超限即抛 `ValueError`。

**源码事实：** `response_stream_filter` 必须是跨暂停持久化的同一实例，因为它的 `paths_map` 记录本次运行实际流出过的路径。

**源码事实：** `ObservabilityLayer` 按 `ENABLE_OTEL` 或 instrument flag 条件挂载；`ExecutionContextLayer` 与 `ExecutionLimitsLayer` 无条件挂载，后者的上限来自配置项 `WORKFLOW_MAX_EXECUTION_STEPS` 与 `WORKFLOW_MAX_EXECUTION_TIME`。

**源码事实：** 循环与迭代容器都被包一层 `WorkflowToolNestedContainerHandler`，并把 `limits_layer.on_event` 作为隐藏事件监听器接入，使容器内部事件也计入步数限制。

**源码事实：** `run()` 静默吞掉 `GenerateTaskStoppedError`，其余异常转成 `GraphRunFailedEvent` 后结束生成器，不向上抛出。

**源码事实：** `single_step_run` 拒绝循环与迭代节点；`run_free_node` 只允许参数提取器与问题分类器，且把版本硬编码为 `"1"`。

**源码事实：** Agent 节点的 manual 元数据过滤条件会被补进变量映射，且跳过 `empty` 与 `not empty` 两个不消费值的操作符。

**源码事实：** `mapping_user_inputs_to_variable_pool` 按 selector 长度排序以保证「整对象先于嵌套覆盖」，源码注释自述其语义未明确定义且被多处依赖。

**源码事实：** `handle_special_values` 的注释要求新代码不要使用它，但响应转换、持久化、节点调试结果仍依赖它。

**源码事实：** `WorkflowGraphTopology.upstream_node_ids` 会把 BFS 结果与真实节点集求交，因为边可能引用已被删除的节点 id。

**架构推论：** 「入口」这一层最容易被误读成「执行器」。它的真实职责是**把产品概念翻译成引擎可挂载的扩展点**：限流是层、可观测性是层、工作流即工具是容器处理器、文件访问是运行时。这套设计的收益是 dify 不必修改引擎就能加产品级约束；代价是**边界的可理解性变差**——想知道「为什么这个工作流跑了 51 步就停了」，得先知道 `ExecutionLimitsLayer` 被挂上了，而这个事实只写在构造函数里。

**架构推论：** 把「停止」当成非错误、把「失败」当成事件而非异常，是一个统一的取舍：**让调用方只有一条控制流要处理**。它的反面是「异常用于控制流」，那会让调用方在每一层都要写两条路径。代价转移到了「必须检查事件内容」上——而这恰恰是容易被漏掉的一步，因为它不需要写代码就不会被想起。

**架构推论：** `single_step_run` 与 `run_free_node` 的存在，说明**调试路径与生产路径是两套装配**。它们共享节点类与变量池，但不共享引擎、不共享容器语义、不共享深度限制（`call_depth` 硬编码 0）。这种「共享一半」的复用最容易产生行为差异：调试时能跑通、发布后不行的情形，往往就出在这半数不共享的地方（例如单步执行不支持循环、忽略深度限制）。

**架构推论：** `WorkflowGraphTopology` 与执行路径分离，说明「静态图分析」被识别成了一个独立关切。它只用 JSON，不需要图引擎、不需要数据库、不需要租户上下文——因此可以在发布校验这种「图还没准备好运行」的时刻调用。**把「能静态回答的问题」从「需要运行才能回答的问题」里拆出来，是降低延迟与失败面的常见手法**，代价是两套图模型（JSON 形状 vs 引擎内部结构）必须保持语义一致，而这没有编译期保证。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/workflow_entry.py` | `WorkflowEntry` | 构造引擎、挂装饰层、提供三种运行模式 |
| `api/core/workflow/workflow_entry.py` | `iter_dify_graph_engine_events` | 把引擎原始事件套上 dify 兼容过滤 |
| `api/core/workflow/workflow_entry.py` | `WorkflowEntry.run` | 完整工作流执行，吞停止、转失败事件 |
| `api/core/workflow/workflow_entry.py` | `WorkflowEntry.single_step_run` | 单节点调试执行，拒绝循环与迭代 |
| `api/core/workflow/workflow_entry.py` | `WorkflowEntry.run_free_node` | 无工作流上下文的裸节点试跑 |
| `api/core/workflow/workflow_entry.py` | `WorkflowEntry.mapping_user_inputs_to_variable_pool` | 用户输入写入变量池，按 selector 长度排序 |
| `api/core/workflow/workflow_entry.py` | `WorkflowEntry.handle_special_values` | 递归把文件对象转字典，注释劝阻新用法 |
| `api/core/workflow/workflow_entry.py` | `WorkflowEntry._run_node_with_layers` | 单节点执行时手工复刻装饰层与 context 传递 |
| `api/core/workflow/graph_topology.py` | `WorkflowGraphTopology` | 草稿图 JSON 的静态拓扑查询 |
| `api/core/workflow/graph_topology.py` | `WorkflowGraphTopology.is_upstream` | 反向邻接表上的可达性判断 |
| `api/core/workflow/graph_topology.py` | `WorkflowGraphTopology.upstream_node_ids` | 上游集合与真实节点集求交 |
| `api/core/workflow/node_factory.py` | `DifyGraphInitContext` | 入口构造引擎时使用的冻结上下文 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [graphon：dify 外置的图执行内核](https://github.com/langgenius/graphon)
- [dify 源码：core/workflow/workflow_entry.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/workflow_entry.py)
- [dify 文档：工作流编排](https://docs.dify.ai/guides/workflow)

## 与主核 / AI 的关系

**「入口层」与主核里的「查询入口」是同一个位置，承担同样的翻译职责。** 数据库的 `PostgresMain` 循环也是把「客户端连接」这类产品概念翻译成执行器能用的上下文：事务状态、会话参数、权限身份。执行器只认这些，不认连接。dify 这边把租户、应用、限流、可观测性翻译成「层」与「容器处理器」，思路一致。**判据也一样：靠近用户的放进入口层，靠近数据的放进内核。**

**「停止不是错误」与主核的取消语义对应。** 数据库取消一个查询时，客户端收到的是明确的取消响应，而不是「连接断了」。两边都把「用户主动中止」建模成一个正常终态。差别是数据库把它做成了协议层的独立消息，dify 做成了「生成器正常结束」——于是**失败与停止在调用方看来都是「流结束了」**，要区分只能看有没有失败事件。协议化的取消更不容易被忽略。

**静态分析提前到发布前，与主核的 DDL 校验同构。** 建索引时数据库会先校验定义合法，不会等到第一次查询才发现索引表达式有错。`WorkflowGraphTopology` 让 dify 能在发布工作流时就回答「这个节点的上游存不存在」这类问题，而不必真跑一遍。**能静态判定的错误不该留到运行期**，这条在两边都成立；区别是数据库有完整的类型与依赖元数据，dify 只有一份画布 JSON，脏数据（悬空边）是常态，所以必须先求交再回答。

**「按形状判断语义」在两边都是防御式妥协。** 这里靠「含 `type` 与 `transfer_method` 两个键」认出文件对象；主核在解析外部数据时也常靠魔数而非扩展名。**当类型信息在跨越边界时丢失，形状是唯一剩下的线索**——代价是它会把「碰巧长得像」的数据误判。

对 agent 工程最实用的五条。其一：**这个仓只做装配，执行细节要去看 graphon**；把 `api/core/workflow/` 当成执行器会在读代码时持续困惑。其二：**事件流结束时不代表成功**，失败是流里的一个事件，调用方必须显式检查。其三：**暂停恢复必须复用同一个响应流过滤器实例**，新建空过滤器会让增量语义出错。其四：**容器的内部事件要透传给限流层**，自定义容器处理器时漏接这一根线，步数上限就对容器内部失效。其五：**单节点调试与生产执行是两套装配**，调试通过不等于发布后行为一致。
