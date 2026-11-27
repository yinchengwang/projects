---
project: metagpt
feature: solver-family
title: 求解器家族与策略抽象
page_type: feature
schema_version: 1
refs:
  - path: metagpt/strategy/solver.py
    symbols: [BaseSolver, ReActSolver, COTSolver]
  - path: metagpt/strategy/solver.py
    symbols: [NaiveSolver, TOTSolver]
---

# 求解器家族与策略抽象

## 一句话职责

BaseSolver 把「怎么在一张动作图上求解一个问题」定成一个只含 `solve()` 的接口，六个子类各代表一种推理策略——但真正实现了的只有按拓扑序顺序跑的那一个。

## 机制是什么

`BaseSolver` 的构造签名就是它的全部设定：`(graph, search_space, llm, context)`。四个东西分别代表「做什么」（有依赖关系的动作节点图）、「可搜索什么」（候选节点集合）、「用谁来想」（LLM）、「在什么语境下」（原始输入）。

子类只有六个，命名直接对应各家的推理范式：

| 子类 | 策略 |
|---|---|
| `NaiveSolver` | 拓扑序逐节点填充 |
| `TOTSolver` | 思维树搜索 |
| `COTSolver` | 思维链 |
| `ReActSolver` | 推理与行动交错 |
| `IOSolver` | 直接把问题丢给模型 |
| `DataInterpreterSolver` | 在图上写码并运行 |

`SearchSpace` 是另一个极简容器：一个 `{key: node}` 字典，加 `add_node` 与 `get_node`。它的注释写明「搜索空间中的节点是 ActionNode 类」——所以图和搜索空间装的是同一种对象，区别只在语义：图表达顺序约束，搜索空间表达「可选项」。

## 怎么实现的

**接口只有一行方法。** `solve` 被 `@abstractmethod` 标注，注释写着「abstract method to solve the problem」，签名不带参数也不带返回值标注。这意味着：

- 求解过程需要的所有东西必须在构造时给全（`graph`/`search_space`/`llm`/`context`），不能像 `run(input)` 那样每次传参；
- 返回值约定靠各子类自己定，`NaiveSolver` 返回 `None`，`TOTSolver` 按注释应当返回解路径。

**唯一实现的求解器只有三步。** `NaiveSolver.solve`：

1. `self.graph.topological_sort()` —— 把执行序算出来写进图的 `execution_order`；
2. 遍历 `execution_order` 里的每个 key，从 `graph.nodes` 取出节点；
3. `await op.fill(req=self.context, llm=self.llm, mode="root")`。

关键在 `mode="root"`：每个节点只填自己，不下钻子节点。这与动作图的语义配套——图上每个节点是一个独立的思考单元，节点之间的关系由边而非父子嵌套表达。若这里改成 `"children"`，第一个根节点就会把整棵子树填完，后面的边全成了摆设。

另外注意它完全没用到 `search_space`。参数被收下、存在 `self.search_space` 上，但在顺序执行范式里没有「选择候选」这一步——这与「图和搜索空间是两种不同的结构」这个设定一致，只是当前实现只用了前者。

**另外五个都是占位。** `TOTSolver`、`COTSolver`、`ReActSolver`、`IOSolver`、`DataInterpreterSolver` 的 `solve` 全部是 `raise NotImplementedError`，每个类只有一行 docstring 说明意图。

这不是遗漏，而是一份**已划好边界但尚未填满**的设计：接口、参数、命名都已就位，任何一条策略都能在不动其他代码的前提下补上实现。实际能跑的替代品在别处——思维树的真实实现在 `strategy/tot.py`（`BFSSolver` / `DFSSolver`），ReAct 循环在角色层的 `_react`，Data Interpreter 在 `roles/di/`。也就是说这份 family 里的名字是「概念目录」，而不是实际调度入口。

**测试验证的正是这条唯一路径。** `test_solver.py` 构造四个 PRD 相关节点（`ISSUE_TYPE`、`PRODUCT_GOALS`、`COMPETITIVE_ANALYSIS`、`REQUIREMENT_POOL`），加四条边构成菱形依赖，然后用 `NaiveSolver` 跑一遍，断言三件事：节点数 4、`execution_order` 长度 4、以及顺序恰好是 `ISSUE_TYPE → PRODUCT_GOALS → COMPETITIVE_ANALYSIS → REQUIREMENT_POOL`。

最后那条断言值得注意：`COMPETITIVE_ANALYSIS` 与 `REQUIREMENT_POOL` 都依赖 `PRODUCT_GOALS`，而 `REQUIREMENT_POOL` 还依赖 `COMPETITIVE_ANALYSIS`。拓扑序把两者压成了线性，所以「菱形」在这里退化成一条链。这也侧面说明求解器不做并行。

## 关键取舍

**源码事实：** `solve` 不带参数，也不返回值约定。所有输入在构造期固定。好处是子类实现简单（不用解析参数），坏处是同一个求解器实例不能被复用于第二个问题——必须重新构造。

**源码事实：** `search_space` 是必填参数，但唯一实现的求解器不用它。这让接口对所有策略统一，代价是调用方为一个当前无用的参数做构造（测试里就写了一个空的 `SearchSpace()`）。

**源码事实：** 五个子类是空实现而非缺失。保留 `raise NotImplementedError` 而不是不写这些类，使「支持哪些策略」这件事在代码里可枚举，也让类型注解与文档有落点。

**架构推论：** 把四种推理范式并列成同一基类的子类，隐含一个判断：它们对外的接口是相同的（给定图、搜索空间、模型、语境，产出一个解）。这个抽象在 `NaiveSolver` 上成立，但对 `ReActSolver` 是否成立存疑——ReAct 需要多轮观察，把中间结果写回图或搜索空间，接口里没有表达这个。空实现恰好把这个张力保留下来了。

**架构推论：** 「图」与「搜索空间」分成两个对象而不是合并成一个，说明设计者预期有些策略需要拓扑（顺序执行、DAG 调度），有些需要候选集（采样、剪枝）。TOT 那一路真正需要的是搜索空间，Naive 那一路真正需要的是图。当前两边各用一半。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/strategy/solver.py` | `BaseSolver` | 求解器接口：构造期收图/搜索空间/模型/语境，只暴露 `solve` |
| `metagpt/strategy/solver.py` | `NaiveSolver` | 唯一实现：拓扑排序后逐节点以 root 模式填充 |
| `metagpt/strategy/solver.py` | `TOTSolver` | 思维树策略的占位实现 |
| `metagpt/strategy/solver.py` | `COTSolver` | 思维链策略的占位实现 |
| `metagpt/strategy/solver.py` | `ReActSolver` | 推理-行动交错策略的占位实现 |
| `metagpt/strategy/solver.py` | `IOSolver` | 直问模型策略的占位实现 |
| `metagpt/strategy/solver.py` | `DataInterpreterSolver` | 图上写码执行策略的占位实现 |
| `metagpt/strategy/search_space.py` | `SearchSpace` | 候选节点的键值容器，与动作图并列 |

## 参考

- [MetaGPT 源码：strategy/solver.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/strategy/solver.py)
- [MetaGPT 源码：strategy/search_space.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/strategy/search_space.py)
- [ReAct 论文：把推理与行动交错起来](https://arxiv.org/abs/2210.03629)
- [思维树论文：用树搜索做刻意推理](https://arxiv.org/abs/2305.10601)

## 与主核 / AI 的关系

「一个抽象基类 + 若干命名清晰的空实现」在主核里也常见：接入层先定好各存储引擎的接口，具体引擎逐个补。这种做法的价值不在代码量，而在**让架构意图可读**——看类名就知道系统打算支持哪几种策略，看空实现就知道哪些还没到。

对 agent 工程的实际提示是：不要按这份 family 去找「怎么切换求解器」。真正在跑的顺序控制在 `Plan` 的任务栈与角色反应模式里，思维树的真实实现在 `tot.py` 的 `BFSSolver`/`DFSSolver`。想扩展这套接口的正确姿势是挑一个子类补上 `solve`，同时决定它到底用 `graph` 还是 `search_space`——两者同时用上的策略（比如「在图上按依赖顺序，但每步采样多个候选」）会暴露出当前接口的一个真实缺口：没有地方承载「中间候选状态」。
