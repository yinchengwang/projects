---
project: autogen
feature: graph-flow
title: 图式编排与激活条件
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py
    symbols: [DiGraph, GraphFlowManager, has_cycles_with_exit]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_graph_builder.py
    symbols: [DiGraphBuilder]
---

# 图式编排与激活条件

## 一句话职责

把「谁接谁」从策略里搬到显式的有向图里：节点是智能体、边带条件与激活组，管理器用一张剩余计数表在运行时判断哪些节点已经「凑齐输入」可以执行，并在图自然走完时终止。

## 机制是什么

`DiGraph` 是纯数据结构：`nodes`（名字 → `DiGraphNode`）、`default_start_node`、以及一个带缓存的 `_has_cycles`。`DiGraphNode` 含 `name`、`edges`、`activation`。`DiGraphEdge` 含 `target`、`condition`（字符串/可调用/`None`）、`activation_group`、`activation_condition`（`"all"` 或 `"any"`），以及一个仅供序列化规避用的 `condition_function`。

`GraphFlowManager` 维护四张表来表达执行状态：

- `_remaining`：`目标节点 → Counter[激活组] → 剩余待满足边数`，初值来自 `get_remaining_map()`（统计指向每个目标、每个激活组的边数）。
- `_origin_remaining`：上面这张表在构造时的快照，用于执行完后复位。
- `_enqueued_any`：`目标 → 激活组 → 是否已入队`，只对 `"any"` 组有意义，防止同组多次触发重复入队。
- `_ready`：就绪双端队列，初值是 `get_start_nodes()`。
- 另有 `_triggered_activation_groups`：记录某个节点本轮被哪些激活组触发，以便执行后精确复位。

## 怎么实现的

**边的条件语义。** `check_condition` 三分支：有 `condition_function` 就调它；否则 `condition` 是字符串就判断 `condition in message.to_model_text()`（注意用的是 `to_model_text` 而不是 `to_text`）；否则（`None`）恒真。`_validate_condition` 在模型校验后做两件事：把可调用对象搬到 `condition_function` 并把 `condition` 置 `None`（因为不可序列化）；`activation_group` 缺省时填成 `target` 名字。**默认行为是「所有指向同一目标的边同属一个激活组」**，这决定了默认的多入边是「全部满足」语义。

**单节点的边条件一致性。** `graph_validate` 里对每个节点检查：若它有任一条带条件的出边，又有任一条无条件出边，就 `ValueError("Node ... has a mix of conditional and unconditional edges.")`。理由很实际：无条件边永远满足，与条件边混在一起会让条件失去意义（永远走那条无条件的）。

**激活条件的一致性。** `_validate_activation_conditions` 用 `目标 → 激活组 → activation_condition` 的映射检查冲突：同一个（目标, 激活组）里若有边声明 `"all"`、另一条声明 `"any"`，抛错并指名冲突双方的来源节点（靠 `_find_edge_source_by_target_and_group` 反查）。这条校验的必要性来自 `_build_lookup_tables` 的写法——它按（目标, 激活组）只保留**第一个**见到的 `activation_condition`，后续同键的值会被忽略。若允许冲突，结果就依赖边的遍历顺序。

**环检测与出口条件。** `has_cycles_with_exit` 用颜色标记的 DFS（`visited` + `rec_stack` + `path` 栈）找环：遇到「已在递归栈中」的目标就说明成环，此时从 `path` 里截出环上的节点，收集环上所有节点的出边，若**全部**边都既无 `condition` 也无 `condition_function`，就抛 `ValueError("Cycle detected without exit condition: A -> B -> A")`；只要环上有一条条件边，就认为该环有出口，合法。返回值是「是否存在（合法）环」，不是「是否报错」——无环时返回 `False`。

**环与终止条件的耦合校验。** `GraphFlowManager.__init__` 里：`graph.graph_validate()` 先跑；然后若 `graph.get_has_cycles()` 为真且 `termination_condition` 与 `max_turns` **都**为 `None`，抛 `ValueError("A termination condition is required for cyclic graphs without a maximum turn limit.")`。也就是说静态的「环上有条件边」这一保证，不足以替代运行时的总兜底——因为条件边只是「可能退出」，不保证一定退出。

**推进入口被重定向到 `update_message_thread`。** 这是图式编排最特别的一处：基类里「选人」发生在 `handle_agent_response` 里调 `_transition_to_next_speakers`，而图的推进逻辑写在 `update_message_thread` 的重写里。`handle_agent_response` 与 `handle_start` 都会在追加线索**之前**调用 `update_message_thread`，所以「把消息记入线索」这个动作同时承担了「沿边传播激活」的职责。

具体传播算法：取 `messages[-1]`，若其 `source` 不在图节点里就直接返回（忽略图外来源）；否则对 `source` 的每条出边跑 `check_condition`，不满足就跳过；满足则看该（目标, 激活组）的 `activation_condition`：`"all"` 分支把 `_remaining[target][group]` 减一，减到 0 就把目标压进 `_ready` 并记录触发组；`"any"` 分支检查 `_enqueued_any`，未入队则入队、置标志、记录触发组。

**选人就是抽干就绪队列。** `select_speaker` 的实现很短：`while self._ready` 逐个 `popleft` 进列表，并对每个发言人调 `_reset_triggered_activation_groups(speaker)`。返回值是列表，因此**并行扇出天然支持**——同一轮可以点多个节点的名。复位逻辑按激活组分派：`"any"` 组把 `_enqueued_any` 置回 `False`；`"all"` 组把 `_remaining` 恢复成 `_origin_remaining` 里的原值（注意用的是构造时快照，而不是「重新数一遍」）。最后清空该发言人已记录的触发组。

**终止的双重判定。** `_apply_termination_condition` 被重写为「先看图是否走完」：`if not self._ready` 就直接构造 `StopMessage("Digraph execution is complete")`、`_reset_execution_state()`、重置终止条件与轮次、发终止信号、返回 `True`。只有 `_ready` 非空时才回落到基类逻辑（终止条件 → 轮次上限）。

注意这里的时序：本方法在 `handle_agent_response` 里被调用，而此刻 `_transition_to_next_speakers` 还没执行。所以判定用的 `_ready` 是「传播后、抽干前」的状态——若传播没产生任何新就绪节点，说明图走完了。

**三处「重置」语义不同。** `_reset_execution_state` 只重置执行状态（`_remaining`、`_enqueued_any`、`_ready`）不动线索；`reset` 额外清线索、清轮次、重置终止条件。而 `_apply_termination_condition` 在自然走完时调的是前者加终止条件重置，线索保留——这样 `save_state` 之后仍能看到历史。

**状态序列化用手写字典。** 尽管定义了 `GraphFlowManagerState`（含 `active_nodes` 字段），`GraphFlowManager.save_state` 实际返回的是一个普通 `dict`：`message_thread`（dump 后的列表）、`current_turn`、`remaining`（Counter 转 dict）、`enqueued_any`、`ready`（转 list）。`load_state` 反向重建。所以图状态里没有 `active_nodes`，那个模型类在当前实现中未被使用。

**`DiGraphNode.activation` 也没被用上。** 校验与执行用的都是**边**上的 `activation_condition`（经 `_build_lookup_tables` 聚合到 `目标/激活组` 维度），节点上的 `activation` 字段在管理器里没有任何读取点。要用 `"any"` 语义必须在加边时指定 `activation_condition="any"`，改节点字段不生效。

**`DiGraphBuilder` 的职责。** 它是一个流式构造器：`add_node` 幂等（同名只加一次）、`add_edge` 前先校验两端节点都已存在、缺省的 `activation_group` 与 `activation_condition` 在加边时补全（`target` 名字与 `"all"`）、`set_entry_point` 校验节点存在、`build()` 组装 `DiGraph` 并**立即跑 `graph_validate()`**。所以错误的图在 `build()` 阶段就会失败，不必等到团队构造。`get_participants()` 按插入顺序返回智能体列表——这个顺序就是为了喂给 `GraphFlow(participants=...)`。`add_conditional_edges` 已标记为将变更的接口并发出 `DeprecationWarning`，内部只是把「关键字 → 目标」的映射展开成多条字符串条件边。

## 关键取舍

**源码事实：** 图完成的判定是「就绪队列为空」，而不是「所有叶节点都执行过」。这意味着若某条分支的条件全部不满足，该分支被静默跳过，图仍算走完。同理，若某节点永远凑不齐输入（例如 `"all"` 组少一条边被条件挡住），图也会提前结束——不会报「有节点未执行」。

**源码事实：** 环上的条件边必须存在是静态检查，但「条件总有一天会满足」无法静态保证。因此源码额外要求有环图必须配终止条件或 `max_turns`，这是两道防线中唯一可靠的那道。

**源码事实：** `"all"` 与 `"any"` 的选择粒度是（目标, 激活组），不是边也不是节点。同一个目标可以被拆进多个激活组，各自独立计数——这正是「A→B→C→B」这类多路径回到同一节点的场景所需要的（两条回边归入同一组用 `"any"`，或分组用 `"all"`）。

**源码事实：** 传播只看 `messages[-1]`，即最后一条消息的来源。若某个参与者的响应里 `inner_messages` 也带 `source`，只有最后那条（通常是 `chat_message`）会驱动传播——`handle_agent_response` 把 `inner_messages + chat_message` 拼成 delta 后调 `update_message_thread`，因此传播依据的是本轮最后一条。

**源码事实：** 边的字符串条件用 `to_model_text()` 判断包含关系。对于 `MultiModalMessage`，图片会被替换成占位符（默认 `[image]`），所以以图片内容为条件的边不可能命中。

**源码事实：** `GraphFlow` 与其它三种群聊的差异：不接受 `Team` 参与者（`isinstance(participant, ChatAgent)` 检查）、没有 `emit_team_events` 参数（管理器构造时也不传，默认 `False`），并额外保存 `_input_participants` 与 `_input_termination_condition` 供 `_to_config` 使用。注释里还留了一句历史说明：不再往图里塞 `_StopAgent` 或 `StopMessageTermination`，终止改由管理器直接处理。

**源码事实：** `DiGraph` 的 `_has_cycles` 是带下划线的 pydantic 私有属性，用作 `has_cycles_with_exit` 的结果缓存；`graph_validate` 末尾会主动重算并缓存，避免之后每次查询都跑 DFS。

**架构推论：** 图式编排把「控制流」从模型手里拿回到代码里。与 Swarm 正好相反：Swarm 让模型自由指定下一个发言人，GraphFlow 让代码预先规定允许的跳转以及各自的条件。代价是图必须能覆盖所有内容分支，否则会出现「条件都不满足、分支被跳过、图静默走完」这种难以察觉的路径缺失。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py` | `DiGraphEdge` | 边：目标、条件（三种形态）、激活组、激活语义 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py` | `DiGraph` | 图结构与校验：父表、入口、叶节点、环检测 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py` | `has_cycles_with_exit` | DFS 找环并要求环上至少一条条件边 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py` | `get_remaining_map` | 剩余计数表的初值来源 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py` | `GraphFlowManager` | 执行器：计数表、就绪队列、触发组复位 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py` | `update_message_thread` | 推进核心：按最后一条消息沿边传播激活 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py` | `select_speaker` | 抽干就绪队列，天然支持并行扇出 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py` | `_apply_termination_condition` | 图走完优先于终止条件与轮次上限 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_graph_builder.py` | `DiGraphBuilder` | 流式构造器：加节点/边、设入口、构建即校验 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_graph_builder.py` | `add_edge` | 补全缺省激活组与激活语义 |

## 参考

- [AutoGen 源码：_digraph_group_chat.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py)
- [AutoGen 源码：_graph_builder.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_graph_builder.py)
- [AutoGen 文档：GraphFlow](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/graph-flow.html)

## 与主核 / AI 的关系

这套「就绪队列 + 剩余计数」的推进方式与主核里的依赖调度、以及构建设备的拓扑排序是同一族算法：每个节点等它的前置边全部（或任一）满足，满足即可执行，执行完把结果传播给后继。差别是主核的依赖图在编译期固定，而这里的边条件在运行期按消息内容动态求值，等价于「带数据依赖的动态 DAG 展开」。

对 AI 工程最实用的三条推论。其一，**图的正确性主要靠条件覆盖**：静态校验只管「环上有没有条件边」，管不了「条件是否覆盖真实输出」，所以生产上必须配合 `termination_condition` 与 `max_turns` 双兜底。其二，**并行扇出是廉价的，汇聚是有状态的**：`"all"` 组靠 `_remaining` 计数等到所有前置完成，一旦某前置因条件不满足被跳过，计数永远不归零，该目标就永不执行——这类「汇聚饥饿」是图式编排里最典型的静默失败。其三，**激活语义要写在边上**：`DiGraphNode.activation` 字段当前没有被管理器读取，误以为改节点就能改语义会导致行为与预期不符。
