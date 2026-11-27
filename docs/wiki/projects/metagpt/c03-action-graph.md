---
project: metagpt
feature: action-graph
title: 动作图与依赖执行
page_type: feature
schema_version: 1
refs:
  - path: metagpt/actions/action_graph.py
    symbols: [ActionGraph]
  - path: metagpt/actions/action.py
    symbols: [_run_action_node]
---

# 动作图与依赖执行

## 一句话职责

ActionGraph 把若干 ActionNode 之间的「谁依赖谁」记成有向边，算出拓扑序；真正的执行不在图里，而是由求解器按这个序把每个节点挨个填一遍。

## 机制是什么

`ActionGraph` 只有三个字段：`nodes`（键是节点的 `key`，值是节点）、`edges`（键是起点 key，值是终点 key 列表）、`execution_order`（拓扑排序的结果，初始为空）。

它提供四个方法：`add_node` 登记节点、`add_edge` 登记一条依赖并**同时**在两端节点上补 `nexts` / `prevs`、`topological_sort` 算执行序。没有 `run`，没有遍历执行，也没有并发调度。

「依赖」的方向是「先做 from_node，再做 to_node」。所以 `edges` 是从前置指向后置，与 `prevs`/`nexts` 的语义一致。

`ActionNode` 自己的 `prevs` / `nexts` 两个列表就是为这张图准备的——它们在节点构造时初始化为空，只有 `add_edge` 会往里加。

## 怎么实现的

**双写。** `add_edge` 一次更新三处：`self.edges[from.key]` 追加 `to.key`；`from.add_next(to)`；`to.add_prev(from)`。图结构与节点自己的邻接表保持同步，后者是给「直接从一个节点问它的上下游」这种用法准备的，不必反查图。

**拓扑排序是递归后序的前插。** `topological_sort` 定义一个内部 `visit(k)`：未访问则标记已访问，递归访问所有后继，然后 `stack.insert(0, k)`。这个「先递归后插到最前」的组合等价于逆后序，也就是合法的拓扑序。外层对所有节点各 `visit` 一次，所以图不连通也能全部覆盖。

`visited` 集合放在递归外，因此一个节点只会被插入一次——分叉处的汇合节点不会被重复处理。

**它不是 Kahn 算法，也不检测环。** 递归版本靠 `visited` 保证终止，所以一个环会被「打破」成某个任意顺序，而不是报错。这是这一版实现的实际行为：环不会崩，但执行序会与依赖矛盾。

**执行在求解器里。** 唯一的执行路径是 `NaiveSolver.solve` 的三行：先 `topological_sort()`，再遍历 `execution_order`，对每个节点 `await op.fill(req=self.context, llm=self.llm, mode="root")`。

关键点是 `mode="root"`：它让每个节点只填自己，不下钻子节点。这与图的语义吻合——图上每个节点是一个独立的思考单元，它们的依赖关系由边表达，而不是靠父子嵌套表达。若用了 `"children"`，根节点会把整棵子树一次性填完，图上的边就没意义了。

**另一条节点填充路径。** `Action._run_action_node` 是「动作内嵌一个节点」时的填充方式：它把收到的消息列表倒序编号拼成 `## History Messages` 语境，再 `node.fill(req=context, llm=self.llm)`。注意这里没传 `mode`，走的是默认 `"auto"`——有子节点就填子节点。

两条路径的差别值得记住：`_run_action_node` 是「一个动作用自己的节点去解析一段对话历史」，`NaiveSolver` 是「一组节点各自用同一份 context 填充」。前者服务于单动作的结构化输出，后者服务于「把一个问题的多个侧面分别问一遍」。

**没有暴露的入口。** 这份实现目前只在 `tests/metagpt/strategy/test_solver.py` 里被构造和断言（4 个节点、4 条排序结果、顺序与声明一致），在生产路径上由 `BaseSolver` 家族持有但只有 `NaiveSolver` 用。`TOTSolver`、`ReActSolver` 等五个求解器都是 `raise NotImplementedError`——图与求解器的分工已经划好，但只有最朴素的那一条被实现了。

## 关键取舍

**源码事实：** `add_edge` 不校验两端节点是否已登记。`nodes` 与 `edges` 是两份独立的字典，加边不需要先加节点。因此 `topological_sort` 的外层遍历只看 `nodes`：只出现在 `edges` 里、从未 `add_node` 的节点不会被访问到——不过它的后继仍可能被别的起点带到。

**源码事实：** `nodes` 以 `node.key` 为键，同名节点后加的会覆盖前面的。这使「同一个 key 的节点只能有一个」成为隐式约束，也是 `ActionNode.create_class` 用 `key` 派类名的前提。

**源码事实：** 拓扑排序不检测环。递归实现下环会静默产生一个任意但确定的顺序。对一个「表达思考依赖」的结构，这比抛异常更宽容，但也意味着环错误不会被发现。

**架构推论：** 把执行从图里拿出去，好处是同一张图能被不同求解器解释——`NaiveSolver` 顺序跑，将来说明书里的 `TOTSolver` 可以按树搜索跑，`ReActSolver` 可以按步交互跑。代价是 `execution_order` 这个缓存字段落在图上，属于「图持有求解器的中间产物」，语义上不太干净。

**架构推论：** 依赖方向只有一种（前置→后置），没有条件边、没有权重、没有分支。这把图压缩成一条偏序，代价是无法表达「A 成功才做 B，失败则做 C」——那类控制流在 MetaGPT 里由角色层的 `react` 状态机与 `Command` 承担，而不是动作图。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/actions/action_graph.py` | `ActionGraph` | 节点表、边表与拓扑序的容器 |
| `metagpt/actions/action_graph.py` | `ActionGraph.add_edge` | 登记边并同步两端的 prevs/nexts |
| `metagpt/actions/action_graph.py` | `ActionGraph.topological_sort` | 递归后序前插得到拓扑序 |
| `metagpt/actions/action.py` | `_run_action_node` | 动作内嵌节点时的填充路径（倒序历史 + auto 模式） |
| `metagpt/strategy/solver.py` | `NaiveSolver.solve` | 按拓扑序逐节点以 root 模式填充 |
| `metagpt/actions/action_node.py` | `ActionNode.prevs` | 节点的前置列表，供图与遍历使用 |

## 参考

- [MetaGPT 源码：actions/action_graph.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/action_graph.py)
- [MetaGPT 源码：strategy/solver.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/strategy/solver.py)
- [MetaGPT 文档：Data Interpreter 与任务规划](https://docs.deepwisdom.ai/main/en/guide/tutorials/agent_about.html)

## 与主核 / AI 的关系

「图只描述依赖、执行交给调度方」与主核里「查询计划与执行算子分离」完全同构：计划是数据，算子是代码，中间靠一个明确的顺序契约对接。主核的契约是算子树的后序遍历，这里的契约是 `execution_order` 这个字符串列表。

对 agent 工程的实际含义是：这份实现是一次探路而非主力。生产路径上，任务依赖由 `Plan` 的 `dependent_task_ids` 加 `_topological_sort` 承担（那里合并了「保留公共前缀、只替换尾部」的增量逻辑），动作图则停在一个可用的最小形态。要理解 MetaGPT 真正的顺序控制，应该看规划器与 `Plan.add_tasks`，而不是这张图；但要看「依赖如何被表达成图」的最小样本，这四十行是最清楚的入口。
