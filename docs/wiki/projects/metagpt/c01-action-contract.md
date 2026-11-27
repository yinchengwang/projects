---
project: metagpt
feature: action-contract
title: Action 基类与提示装配
page_type: feature
schema_version: 1
refs:
  - path: metagpt/actions/action.py
    symbols: [Action, set_prefix, _aask, run]
---

# Action 基类与提示装配

## 一句话职责

Action 把「一次可被角色调度的最小工作单元」定成一个 pydantic 模型，统一了前缀注入、LLM 取用、上下文字典和「有节点就填节点、没节点就必须子类实现」这两条执行路径。

## 机制是什么

`Action` 继承三个基类：`SerializationMixin`（可序列化进出消息）、`ContextMixin`（拿 context/config/llm）、`BaseModel`。它自己只加五个字段：`name`、`i_context`、`prefix`、`desc`、`node`，外加一个 `llm_name_or_type`。

`i_context` 的类型是一个显式列举的联合：`dict`、`CodingContext`、`CodeSummarizeContext`、`TestingContext`、`RunCodeContext`、`CodePlanAndChangeContext`、`str`、`None`。这不是为了宽泛，而是把「动作之间传递的输入」限定成几种已登记的结构体——所有 SOP 动作都在这几个类型里往返。

`node: ActionNode` 被 `Field(exclude=True)` 排除在序列化之外。它是「这个动作的提示骨架」，不是数据。

## 怎么实现的

**两个前置校验器决定了两条建法。** `set_name_if_empty` 在 `mode="before"` 阶段兜底：名字为空就用类名。这让 `WriteCode()` 这种零参构造直接拿到 `"WriteCode"` 作为动作名，而这个名字同时是 `cause_by` 的匹配依据（角色用 `_watch` 按类名监听动作）。

`_init_with_instruction` 是另一条建法：如果构造时传了 `instruction` 关键字，就把它从 values 里 `pop` 掉，转而生成一个 `ActionNode(key=name, expected_type=str, instruction=i, example="", schema="raw")`。于是「一句话动作」可以直接写成 `Action(name=..., instruction=...)`，不用手工搭节点树。注意它用的是 `schema="raw"`——自由文本，不做结构化解析。

**一个后置校验器接管 LLM 选择。** `_update_private_llm` 拿 `llm_name_or_type` 去 `ModelsConfig.default().get(...)` 查配置，命中就 `create_llm_instance(config)` 造一个新的 LLM 实例，然后把旧实例的 `cost_manager` 搬过去。搬 cost_manager 这一步是刻意的：同一角色下不同动作可能指定不同模型，但成本必须记在同一本账上，否则团队级的预算控制会漏账。

**前缀注入是双份的。** `set_prefix` 做两件事：写 `self.llm.system_prompt`，以及把 `self.llm` 转发给 `self.node`。前者让 `_aask` 走 LLM 时自动带上角色人设，后者让节点填充时用同一个 LLM 实例。两份都要做，因为节点填充（`node.fill(req=..., llm=...)`）会显式接收 llm 参数，不读 `self.llm`。

**`_aask` 是纯透传。** 实现只有一行：`self.llm.aask(prompt, system_msgs)`。它存在的意义不是加逻辑，而是给子类一个稳定的「问模型」接缝——子类只调 `_aask`，不直接碰 `self.llm`，将来要加统一的埋点或改写只需要改这一处。

**两条执行路径。** `run` 的默认实现是：`self.node` 存在就走 `_run_action_node`，否则 `raise NotImplementedError("The run method should be implemented in a subclass.")`。

`_run_action_node` 把 `args[0]`（消息列表）倒序编号拼成 `## History Messages` 段落，再调 `node.fill(req=context, llm=self.llm)`。倒序是刻意的：最新的消息编号 0，最靠近模型注意力的位置放最新内容。

**子类怎么用这两条路。** 实际代码里两种都有人用。纯「一问一答、返回结构化」的动作只声明 `node` 就不必写 `run`；而 SOP 里那些产物是文件而非字符串的动作（写设计、写任务、写代码、跑代码）全部覆写 `run`，因为它们要在提示之外还做文件读写、增量合并、依赖声明。

## 关键取舍

**源码事实：** `run` 的默认实现在没有 `node` 时直接抛 `NotImplementedError`，而不是给一个「把 context 直接问一遍」的默认实现。这使「动作必须知道自己要什么」成为硬约束，代价是每个新动作至少得提供一个 `run` 或一个 `node`。

**源码事实：** `node` 与 `i_context` 都不进序列化。所以动作对象本身是不可持久化的——角色恢复时重建动作，状态靠 `RoleContext.state` 那个整数下标表达（见角色反应模式一页）。这是「动作是无状态函数」这一假设的代价。

**源码事实：** `llm_name_or_type` 命中时新建 LLM 实例并只继承 `cost_manager`，不继承 system_prompt。因此 `set_prefix` 必须在校验器之后调用（角色确实是先构造动作、再 `set_prefix`），否则新实例的 system_prompt 会被覆盖掉。

**架构推论：** 把「前缀」做成可后置注入的字段而不是构造参数，是为了让同一个动作类能被不同角色复用——同一个 `WriteCode` 在 Engineer 手里和在其他角色手里人设不同，但代码不必改。

**架构推论：** `i_context` 用封闭联合而不是 `Any`，实际是给动作间通信定了一份 ABI：上游动作产出的结构体必须属于这几个类型之一，下游才能安全地按字段读。要加新的传递形态，就得显式扩这个联合，而不是偷偷塞 dict。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/actions/action.py` | `Action` | 动作基类：字段声明与三个基类组合 |
| `metagpt/actions/action.py` | `Action.set_prefix` | 注入人设到 llm，并转发给节点 |
| `metagpt/actions/action.py` | `Action._aask` | 透传到 `llm.aask`，作为子类的统一问模型接缝 |
| `metagpt/actions/action.py` | `Action._run_action_node` | 消息倒序拼历史后填充节点 |
| `metagpt/actions/action.py` | `Action.run` | 有节点走节点，否则要求子类实现 |
| `metagpt/actions/action.py` | `Action._init_with_instruction` | 由一句 instruction 直接生成 raw 节点 |
| `metagpt/actions/action.py` | `Action._update_private_llm` | 按名字换模型并继承成本管理器 |
| `metagpt/actions/action_node.py` | `ActionNode` | 动作的提示骨架（下一节详述） |

## 参考

- [MetaGPT 源码：actions/action.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/action.py)
- [MetaGPT 源码：actions/action_node.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/action_node.py)
- [MetaGPT 文档：SOP 与动作设计](https://docs.deepwisdom.ai/main/en/guide/tutorials/agent_about.html)

## 与主核 / AI 的关系

「动作 = 无状态函数 + 外部注入的依赖」与主核里「算子只读上下文、不持有会话状态」是同一种约束。主核靠显式传 xid 避免状态藏在对象里，这里靠 `i_context` 的封闭联合把「动作之间能传什么」钉死。两边都接受同一个代价：一旦要传新东西，必须改类型定义而不是就地塞一个字段。

对 agent 工程最实际的一条是双份前缀注入。很多人给动作设了 `prefix` 却发现提示里没生效，原因通常是顺序：`set_prefix` 之前 LLM 实例已被 `llm_name_or_type` 重建，或者节点是在 `set_prefix` 之后才 `fill` 时没拿到同一个 llm 实例。理解了 `set_prefix` 要同时写 llm 与 node，这类问题就不必靠试错定位。
