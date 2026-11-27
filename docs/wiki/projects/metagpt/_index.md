---
project: metagpt
page_type: overview
display_name: MetaGPT
repo: https://github.com/geekan/MetaGPT
license: MIT
language: Python
stars: 71419
stars_as_of: 2026-11-23
refs:
  - path: metagpt/roles/role.py
    symbols: [Role, RoleReactMode]
  - path: metagpt/team.py
    symbols: [Team, invest]
  - path: metagpt/environment/base_env.py
    symbols: [Environment, publish_message]
  - path: metagpt/actions/action.py
    symbols: [Action, run]
  - path: metagpt/strategy/planner.py
    symbols: [Planner, update_plan]
  - path: metagpt/memory/memory.py
    symbols: [Memory, add]
---
# MetaGPT

## 一句话职责

MetaGPT 把「软件公司的岗位分工与标准流程」写成代码结构：角色按 SOP 串起动作，团队按预算投资，环境承载消息与共享状态。

## 机制是什么

MetaGPT 的核心主张是 **SOP as code**。它不提供通用的「让模型自己决定怎么协作」的框架，而是把产品经理、架构师、工程师、QA 这些岗位各自该做什么、产出什么、按什么顺序交接，直接编码成角色与动作的组合。

这套结构由四层组成：**Environment** 提供消息总线与角色地址；**Role** 是状态机，按 react / by_order / plan_and_act 三种模式推进；**Action** 是流程中的一个具体步骤，用 `ActionNode` 描述结构化输出；**Team** 在其上做资源管理——雇佣角色、投入预算、检查余额。

在此之上还有三套支撑机制：**记忆与经验池**（角色记忆、脑记忆、长期记忆、经验检索）、**RAG 检索链路**（引擎、检索器工厂、重排）、**工具与技能系统**（工具注册表、推荐、代码转 schema、技能声明）。

本文库按经审阅的机制单元组织：**8 个机制领域、共 30 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 8 个领域按下面的顺序排列：A 角色与团队 → B 环境与消息 → C 动作与动作图 → D 策略与规划 → E 记忆与经验池 → F RAG 检索链路 → G 模型提供方 → H 工具与技能。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`metagpt/` 主包。`rag/engines/flare.py` 只是把 llama-index 的 FLARE 查询引擎转出，没有本仓机制，因此不含独立页面。`tools/` 下的各厂商适配（Azure TTS、讯飞 TTS、搜索引擎各家实现）属于外部集成，不在本清单内。

## 机制导航（30 篇 / 8 组）

### A-角色与团队（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-base-role-contract.md`](a01-base-role-contract.md) | BaseRole 抽象与三类生命周期钩子 |
| [`a02-role-react-modes.md`](a02-role-react-modes.md) | Role 的三种反应模式 |
| [`a03-role-context.md`](a03-role-context.md) | 角色上下文与状态注入 |
| [`a04-team-investment.md`](a04-team-investment.md) | 团队投资与成本控制 |

### B-环境与消息（4 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-base-environment.md`](b01-base-environment.md) | BaseEnvironment 抽象与角色注册 |
| [`b02-environment-and-addresses.md`](b02-environment-and-addresses.md) | Environment 的角色管理与地址分配 |
| [`b03-message-exchange.md`](b03-message-exchange.md) | 消息对象与收发路径 |
| [`b04-ext-environment.md`](b04-ext-environment.md) | 外部 API 环境与可读可写标注 |

### C-动作与动作图（4 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-action-contract.md`](c01-action-contract.md) | Action 基类与提示装配 |
| [`c02-action-node.md`](c02-action-node.md) | ActionNode 与结构化输出 |
| [`c03-action-graph.md`](c03-action-graph.md) | 动作图与依赖执行 |
| [`c04-sop-actions.md`](c04-sop-actions.md) | SOP 动作序列：需求到代码 |

### D-策略与规划（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-solver-family.md`](d01-solver-family.md) | 求解器家族与策略抽象 |
| [`d02-planner.md`](d02-planner.md) | 任务规划器与人工确认 |
| [`d03-experience-retriever.md`](d03-experience-retriever.md) | 经验检索器与策略注入 |
| [`d04-tree-of-thought.md`](d04-tree-of-thought.md) | 思维树与搜索空间 |

### E-记忆与经验池（5 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-role-memory.md`](e01-role-memory.md) | 角色记忆与消息归档 |
| [`e02-brain-memory.md`](e02-brain-memory.md) | 脑记忆与短期上下文压缩 |
| [`e03-longterm-memory.md`](e03-longterm-memory.md) | 长期记忆与向量检索 |
| [`e04-document-store.md`](e04-document-store.md) | 文档存储后端抽象 |
| [`e05-experience-pool.md`](e05-experience-pool.md) | 经验池与经验生命周期 |

### F-RAG 检索链路（3 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-simple-engine.md`](f01-simple-engine.md) | SimpleEngine 与检索器能力校验 |
| [`f02-retriever-factory.md`](f02-retriever-factory.md) | 检索器工厂与混合检索 |
| [`f03-rerank-postprocessor.md`](f03-rerank-postprocessor.md) | 重排后处理与对象还原 |

### G-模型提供方（2 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-base-llm-provider.md`](g01-base-llm-provider.md) | 模型基类与成本计费 |
| [`g02-llm-provider-registry.md`](g02-llm-provider-registry.md) | 提供方注册与选择 |

### H-工具与技能（4 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-tool-registry.md`](h01-tool-registry.md) | 工具注册表与 schema 生成 |
| [`h02-tool-recommend.md`](h02-tool-recommend.md) | 工具推荐与检索策略 |
| [`h03-code-to-tool-schema.md`](h03-code-to-tool-schema.md) | 代码转工具 schema 的 AST 分析 |
| [`h04-skill-system.md`](h04-skill-system.md) | 技能声明与技能动作 |

## 关键取舍

**源码事实：** 角色反应模式是运行时字段（`RoleContext.react_mode`）而非子类差异，同一个角色类可以切换 react / by_order / plan_and_act。这让「同一套角色定义支持不同确定性程度」成为可能，代价是行为分支集中在 `_think` 里。

**源码事实：** 状态用整数下标表达，`todo` 不参与序列化。恢复时只存数字，不存动作对象。这让持久化极简，代价是动作列表的顺序成了隐式契约。

**源码事实：** `_observe` 用「动作来源是否在 watch 集合」和「显式收件人是否含本角色」两条规则过滤消息，不依赖语义相似度。路由完全确定可调试，但标错 `cause_by` 或 `send_to` 会让消息被静默丢弃。

**源码事实：** Team 有成本管理：`invest` 投入预算、`_check_balance` 检查余额、超支抛 `NoMoneyException`。把成本控制做进框架本体，而不是留给调用方。

**架构推论：** SOP as code 的收益是「多智能体协作不用靠提示词碰运气」——流程、交接物、评审环节都在代码里。代价是它假设的业务形态很具体：如果目标不是「用 LLM 模拟一个软件公司」，这套角色与动作的划分就不合用。

**架构推论：** 经验池（exp_pool）与经验检索器（experience_retriever）是 MetaGPT 相对其他 agent 框架的独特投入：它试图让「过去做过类似任务的成功/失败轨迹」影响本次决策。这需要检索层与策略层紧耦合，也是它同时维护 RAG 链路的原因之一。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `metagpt/roles/role.py` | `Role` | 角色状态机：观测、思考、行动、反应 |
| `metagpt/roles/role.py` | `RoleReactMode` | 三种反应模式枚举 |
| `metagpt/team.py` | `Team` | 团队：雇佣、投资、成本检查、项目运行 |
| `metagpt/environment/base_env.py` | `Environment` | 环境：角色登记、地址分配、消息广播 |
| `metagpt/actions/action.py` | `Action` | 动作基类：提示装配与运行入口 |
| `metagpt/actions/action_node.py` | `ActionNode` | 结构化输出节点：填充、评审、修订 |
| `metagpt/strategy/planner.py` | `Planner` | 任务规划器：任务栈、计划更新、人工确认 |
| `metagpt/memory/memory.py` | `Memory` | 角色记忆：增删查与按动作检索 |
| `metagpt/exp_pool/manager.py` | `ExperienceManager` | 经验池：创建、查询、后端选择 |
| `metagpt/tools/tool_registry.py` | `ToolRegistry` | 工具注册表与 schema 生成 |

## 参考

- [MetaGPT 源码仓库](https://github.com/geekan/MetaGPT)
- [MetaGPT 文档：核心概念](https://docs.deepwisdom.ai/main/en/guide/get_started/introduction.html)
- [MetaGPT 论文：SOP 驱动的多智能体元编程](https://arxiv.org/abs/2308.00352)
- [MetaGPT 文档：Agent 与 Role](https://docs.deepwisdom.ai/main/en/guide/tutorials/agent_about.html)

## 与主核 / AI 的关系

MetaGPT 与主核（多模态数据库内核）有三处可对照：

**SOP as code vs 执行计划。** MetaGPT 把业务流程固化进角色与动作的组合，主核把查询逻辑固化进计划树。两者都是「提前确定结构、运行期只填内容」，区别是主核的计划可由优化器重排，MetaGPT 的动作顺序由角色定义固定。

**Team 预算 vs 资源配额。** `invest` 与 `_check_balance` 把模型调用成本当成可耗尽的资源，与主核的连接数、内存配额同构。不同在于成本超支在这里表现为业务异常（`NoMoneyException`），而非系统级拒绝。

**经验池 vs 统计信息。** 经验池让历史轨迹影响本次决策，主核靠统计信息影响计划选择。两者都在尝试「用过去的执行结果改善下一次」，但主核的统计信息是结构化的数值，经验池里存的是自然语言的轨迹。

对 agent 工程的判断价值：MetaGPT 是「框架 vs 平台」的一个样本——它选择把某个具体业务形态（软件公司）做深，而不是做通用的编排原语。这在它的目标场景里收益明显（流程与交接物都现成），但迁移到别的业务时要先问「这套岗位划分还成立吗」。
