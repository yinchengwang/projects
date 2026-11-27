---
project: metagpt
feature: tree-of-thought
title: 思维树与搜索空间
page_type: feature
schema_version: 1
refs:
  - path: metagpt/strategy/base.py
    symbols: [ThoughtNode, ThoughtTree, BaseEvaluator]
  - path: metagpt/strategy/tot.py
    symbols: [BFSSolver, DFSSolver, MCTSSolver]
  - path: metagpt/strategy/search_space.py
    symbols: [SearchSpace]
---

# 思维树与搜索空间

## 一句话职责

思维树把「想一步」变成「从当前状态生成若干个候选下一步、逐个打分、再挑出少数继续展开」，三种遍历策略只是在「怎么挑」上不同。

## 机制是什么

`ThoughtNode` 继承 `anytree.Node`，在此之上加四个字段：`name`（这一步的内容，也是路径上的标签）、`value`（累计得分）、`id`（模型给的序号）、`valid_status`（这一步是否还可行）。它有两个更新方法：`update_value` 直接赋值，`update_valid_status` 记可行性。

`ThoughtTree` 继承 `anytree.RenderTree`——注意是 `RenderTree` 而不是 `Node`，所以它本身不是树的一个节点，而是一个「以某节点为根的可遍历视图」。它提供四个能力：`all_nodes` 摊平所有节点、`update_node` 把一批模型返回的思考接到某个父节点下、`parse_node_path` 从某个节点回溯出根到它的名字序列、`show` 打印整棵树。

三个抽象基类在 `base.py` 里：`BaseParser` 负责「怎么问出候选」「怎么问出评分」，`BaseEvaluator` 负责「把模型的评分文本变成数值」与「判定这一步是否可行」，`ThoughtNode`/`ThoughtTree` 是数据结构。`tot.py` 的 `ThoughtSolverBase` 把「生成—评分—选择」三步串起来，三个子类各换一种遍历。

`SearchSpace` 是另一件东西：一个 `{key: ActionNode}` 字典，加 `add_node` 与 `get_node`。它与思维树没有代码耦合，属于同一目录下并列的另一个「候选集合」概念。

## 怎么实现的

**一次展开有三步，都在 `ThoughtSolverBase` 里。**

`generate_thoughts(current_state, current_node)`：用 `config.parser.propose(current_state=..., n_generate_sample=...)` 拼提示，拼上 `OUTPUT_FORMAT`（要求输出 `[{"node_id": ..., "node_state_instruction": ...}]` 的 JSON 列表），调模型，`CodeParser.parse_code` 抽代码块，然后 `eval` 成 Python 对象，最后交给 `thought_tree.update_node(thoughts, current_node)`——后者逐个 `ThoughtNode(name=..., parent=current_node, id=int(...))` 建节点。

`eval` 这一步值得单独记住：模型输出直接当 Python 表达式求值。注释里还留着一条 fixme，说「避免不跟随，生成过多 nodes」，紧跟着一行被注释掉的截断逻辑（`idx < self.n_generate_sample`）。所以现在模型给几个就建几个，不截断。

`evaluate_node(node, parent_value)`：用 `config.parser.value(input=node.name, node_id=node.id)` 问模型这一步值多少，`config.evaluator(evaluation, node_id=...)` 把文本转成数值，`config.evaluator.status_verify(value)` 判定可行性，然后 `node.update_valid_status(status)` 与 `node.update_value(parent_value + value)`。

注意是**累计**：节点存的是从根到它的路径总得分，不是本步得分。这让「选最优节点」退化成「取 value 最大的节点」，不用再算路径和。

**选择只有 greedy 一种实现。** `select_nodes` 按 `value` 降序取前 `n_select_sample` 个；`MethodSelect.SAMPLE` 分支直接 `raise NotImplementedError`。然后对每个落选节点做 `node.parent = None` ——在 anytree 里这一步等价于「从树中剪掉这个节点及其子树」，注释也写了「从树中删除节点」。所以选择不是打标记，是物理剪枝。

**最终答案取全局最高分节点。** `update_solution` 用 `max(self.thought_tree.all_nodes, key=lambda x: x.value, default=None)`，再 `parse_node_path` 回溯出完整路径。注意 `all_nodes` 是遍历当前树得到的，被剪掉的节点已经不在其中。

**BFS：逐层展开、按层剪枝。** `solve` 建根节点、建树、把 `[root]` 作为当前层，循环 `config.max_steps` 次：每轮 `_bfs_build(current_nodes)` 对当前层每个节点并发地「生成 + 评分」（`asyncio.gather` 套 `asyncio.gather`，两层并发），收集所有子节点作为下一层的候选，然后 `select_nodes` 剪枝，剩下的成为新的当前层。

所以 BFS 的形态是：每轮先把这一层所有节点都展开完（横向铺开），再统一剪枝保留 `n_select_sample` 条。总候选量是 `n_select_sample × n_generate_sample` 量级。

**DFS：单链下钻、连败两次就停。** `_dfs(root)` 从根开始，每步只取 `thought_nodes[0]` 这一个孩子，给它评分，若判定不可行就 `impossible_state_cnt += 1`，累计到 2 就 break 并打日志「impossible state reached, break」。可行就以此节点为新当前位置继续。

注意它只评分第一个孩子，其余兄弟节点**已被建出来但从未评分**（`value` 保持 0），却仍留在树里。这直接影响最后的 `update_solution`：那些 0 分节点会参与 `max` 比较。

`solve` 是 `for n in range(config.n_solution_sample): await self._dfs(root)` —— 从同一个根重复下钻多次。但每轮都从根开始、且每步都取 `children` 的第一个，而 `update_node` 是往当前节点上追加子节点，所以第二轮会取到第一轮已经建过的那个孩子（`children` 列表的第一个仍是它），于是路径几乎必然重复。源码里那条 fixme 说的正是这件事：「需要产生回退，当前节点不可用时回退到父节点，产生新的节点继续探索」。

**DFS 有一处会直接报错。** `_dfs` 的循环写的是 `for step in range(self.max_steps)`，而 `max_steps` 只存在于 `self.config.max_steps`——`ThoughtSolverBase` 上没有这个属性。同一份代码里 BFS 用的是 `self.config.max_steps`。所以 `DFSSolver` 走到 `_dfs` 就会抛 `AttributeError`。

**MCTS 是空的。** `MCTSSolver.solve` 一行 `raise NotImplementedError`。

**门面按策略选求解器。** `TreeofThought` 是个 pydantic 模型，字段有 `config`、`solver`、`strategy`（默认 `Strategy.BFS`），构造时 `_initialize_solver` 按策略把 `solver` 换成分派到的实例；非法策略抛 `NotImplementedError`。

它的 `solve` 调用 `await self.solver.solve(init_prompt)` 但**没有 return**，所以门面的调用方拿不到解路径，只能从 `self.solver.thought_tree` 里自己去取。这与 `BFSSolver.solve` 返回路径的设计不一致。

## 关键取舍

**源码事实：** 节点存累计分而非本步分。好处是「找最优」只需一次 `max`；代价是评分必须在展开时一路累加，父节点的分变了（比如后来被重评）不会回溯刷新子节点。

**源码事实：** 剪枝是物理删除（`node.parent = None`），不是打标记。因此被剪掉的子树无法恢复，也无法在事后解释「当时为什么没走这条路」——树只保留幸存路径。

**源码事实：** `select_nodes` 的 SAMPLE 分支未实现，只有 greedy。所谓「采样选择」在这个版本里不存在，每层留下的永远是分数最高的那几条。

**源码事实：** `generate_thoughts` 用 `eval` 解析模型输出，且注释里那段「按 `n_generate_sample` 截断」的代码被注释掉了。这是当前实现的一处真实风险：模型输出多余节点不会被拦，输出非法 Python 会直接抛异常。

**源码事实：** DFS 只评估每步的第一个孩子，兄弟节点带着默认分 0 留在树里，最终 `max` 会把它们当成候选。DFS 的解路径正确性因此依赖于「零分节点恰巧不是最高分」。

**架构推论：** 把「生成候选」与「评估候选」拆成两个独立的模型调用，是这个设计的核心代价与收益：每次展开要花 `1 + n_generate_sample` 次调用（一次生成、每候选一次评分），换来的是评分可以独立控制（换 `BaseEvaluator` 就能换评分标准），而不必在生成时就把判断塞进同一个提示。

**架构推论：** `ThoughtTree` 继承 `RenderTree` 而非 `Node`，意味着它没有自己的根，每次遍历都要从构造它的那个根开始。所以它天然是「某个根节点的视图」，一个根一棵树，不能多根共存。`parse_node_path` 依赖 anytree 的 `parent` 链，因此剪枝（把 parent 置 None）会让被剪节点变成孤立根——它的路径从此只剩自己。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/strategy/base.py` | `ThoughtNode` | 思维节点：名字、累计分、可行性 |
| `metagpt/strategy/base.py` | `ThoughtTree` | 树的视图：摊平、接子节点、回溯路径、打印 |
| `metagpt/strategy/base.py` | `BaseEvaluator` | 评分与可行性判定的抽象接口 |
| `metagpt/strategy/base.py` | `BaseParser` | propose/sample/value 三个问法接口 |
| `metagpt/strategy/tot.py` | `ThoughtSolverBase.generate_thoughts` | 生成候选并把它们挂到当前节点下 |
| `metagpt/strategy/tot.py` | `ThoughtSolverBase.evaluate_node` | 评分并按父值累计、更新可行性 |
| `metagpt/strategy/tot.py` | `ThoughtSolverBase.select_nodes` | 贪心选前 k 并物理剪掉落选分支 |
| `metagpt/strategy/tot.py` | `BFSSolver` | 逐层展开、按层剪枝的广度优先 |
| `metagpt/strategy/tot.py` | `DFSSolver` | 单链下钻、连败两次即停的深度优先 |
| `metagpt/strategy/tot.py` | `MCTSSolver` | 蒙特卡洛树搜索的占位实现 |
| `metagpt/strategy/tot.py` | `TreeofThought` | 按策略选求解器的门面 |
| `metagpt/strategy/search_space.py` | `SearchSpace` | 与思维树并列的候选节点容器 |

## 参考

- [MetaGPT 源码：strategy/tot.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/strategy/tot.py)
- [MetaGPT 源码：strategy/base.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/strategy/base.py)
- [思维树论文：用树搜索做刻意推理](https://arxiv.org/abs/2305.10601)
- [anytree 文档：节点与渲染树](https://anytree.readthedocs.io/en/latest/)

## 与主核 / AI 的关系

「每个候选都带累计分、按分剪枝」与主核里查询优化器的计划搜索是同一种结构：都是在一棵可能的状态树上做有界搜索，用代价模型排序，只展开最有希望的分支。差别在于主核的代价模型是确定性的、可以在展开前估算；这里的评分是另一次模型调用，必须在展开后才知道——所以它每次展开的成本包含 `1 + n_generate_sample` 次调用，这让搜索宽度天然受限。

对 agent 工程的实际含义有两点。其一，「生成」与「评估」分成两次调用，换来评分标准的可替换性，代价是本该一次想清楚的事情被拆成两轮、且第二轮看不到第一轮为什么这么想。其二，DFS 那条「连败两次就停」的规则是这套实现里唯一的失败退出机制——它用「连续不可行」而不是「总步数」来判断走进死胡同，这比纯靠 `max_steps` 更贴近人思考的方式，但因为那条 `self.max_steps` 的引用错误，当前这条路跑不起来；要用这套东西，BFS 是唯一可用的入口。
