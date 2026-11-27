---
project: metagpt
feature: role-react-modes
title: Role 的三种反应模式
page_type: feature
schema_version: 1
refs:
  - path: metagpt/roles/role.py
    symbols: [Role, RoleReactMode, _react, _plan_and_act]
  - path: metagpt/roles/role.py
    symbols: [think, act, _observe]
---

# Role 的三种反应模式

## 一句话职责

一个角色收到消息后到底怎么动，由 `RoleReactMode` 三选一决定：按 LLM 选出的阶段循环、按固定动作顺序、或先规划再逐个任务执行。

## 机制是什么

MetaGPT 把「角色怎么反应」抽象成一个枚举，而不是写死一条链路。三个取值分别对应三种推进方式：

| 模式 | 谁决定下一步 | 一轮的产物 |
|---|---|---|
| `react` | LLM 从阶段列表里选一个数字 | 最后一个动作的输出消息 |
| `by_order` | 动作在列表里的下标 | 最后一个动作的输出消息 |
| `plan_and_act` | 规划器维护的任务栈 | 已完成计划的整体回复 |

`react` 与 `by_order` 共用同一个 think-act 循环（`_react`），差异只在 `_think` 怎么定下一个状态；`plan_and_act` 走完全不同的 `_plan_and_act`，它的循环条件是「规划器里还有当前任务」，而不是「动作下标还没越界」。

## 怎么实现的

**状态机的位置。** 状态不是角色对象上的独立字段，而是挂在 `RoleContext` 上：`state` 是当前动作下标（`-1` 表示初始或终止，此时 `todo` 为 `None`），`todo` 是本轮要跑的动作。`_set_state` 同时更新这两个字段，`_think` 只负责决定下一个 `state` 是多少。

**`react` 模式的思考。** `_think` 在 `REACT` 分支下会拼一段提示：角色前缀（profile / name / goal / constraints）加上 `STATE_TEMPLATE`。这段模板把对话历史、所有阶段名、上一个状态序号一并交给模型，要求它只回答一个 `0..n-1` 的数字，或者在「目标已完成」时回答 `-1`。

返回值的处理值得注意：先 `extract_state_value_from_output` 从自由文本里抠数字，再校验「不是数字且不是 -1」或「数字不在合法区间」这两种情况，任一命中就把状态强制设成 `-1`（并在日志里 warning），而不是抛异常。也就是说模型答歪了不会中断运行，只会被当成「无事可做」。

**`by_order` 模式的短路。** 同一段 `_think` 在 `BY_ORDER` 分支里完全跳过 LLM：直接把 `state + 1`，并把 `max_react_loop` 对齐到动作数量。返回值是「新状态是否仍在区间内」，`-1` 或越界就让调用方停下。这条路径的存在说明一个事实：**顺序模式不需要模型参与决策**，它只是把动作列表当脚本跑。

**单动作与恢复的提前返回。** `_think` 开头有两处提前返回：动作只有一个时直接置状态 0 并返回真（没有可选的余地）；角色处于 `recovered` 且已有非负状态时沿用该状态，随后清掉 `recovered` 标记。第二处是断点续跑的关键——从持久化状态恢复时不重新让模型选阶段，而是接着上次的位置往下走，清标记则防止 `max_react_loop` 被反复消耗。

**`_react` 的循环。** 循环上界是 `rc.max_react_loop`，每轮先 `_think` 再 `_act`。`_think` 返回假就立刻 break。`rsp` 初值是一个内容为占位文本的 `AIMessage`，注释写明它会被 `_act` 覆盖——这个初值只在「第一次 `_think` 就返回假」时才真的被返回给调用方。

**`_act` 的结果归一。** 动作的返回值有三种形态，`_act` 分别处理：`ActionOutput` 或 `ActionNode` 会被包成带 `instruct_content` 的 `AIMessage`；已经是 `Message` 的直接用；其他一律当字符串塞进 `AIMessage`（`response or ""` 兜住 None）。无论哪条分支，结果都会被写进角色记忆。

**观测的过滤规则。** `_observe` 先从消息缓冲区（或恢复场景下的「未读消息」）取新消息，再去重，然后按两条规则过滤出「与己相关」的：消息的 `cause_by` 在角色的 `watch` 集合里，或消息的 `send_to` 含本角色名。`observe_all_msg_from_buffer` 开关决定写进记忆的是全部新消息还是只写过滤后的相关消息——前者让角色「知道但不反应」，后者只留它关心的。

**`plan_and_act` 的两段。** 第一次进入时若规划器还没有目标，就从记忆里取最新一条用户需求作为 goal 并调 `update_plan`。随后是一个 `while self.planner.current_task` 循环：取出当前任务、`_act_on_task` 执行、`process_task_result` 处理结果（含评审、确认、计划更新），直到任务栈耗尽。最后把规划器的第一条有用记忆改写成 assistant 角色作为回复。

`_act_on_task` 在基类里直接 `raise NotImplementedError`——这是刻意的：规划模式要求子类知道「一个 Task 该怎么变成一次动作调用」，框架不替它决定。

**统一收尾。** 三种模式都从 `react()` 出去，收尾动作一致：把状态重置成 `-1`（`todo` 随之清空）、若回复是 `AIMessage` 就盖上 agent 标记。这样下一轮 `_think` 一定从干净状态开始。

## 关键取舍

**源码事实：** `REACT` 与 `BY_ORDER` 共用同一个循环函数 `_react`，只在 `_think` 内部按模式分支。这意味着两种模式的差异被压缩成「状态怎么算出来」，循环结构、动作执行、结果归一完全一致。

**源码事实：** 模型返回非法状态时不报错，而是降级成 `-1` 并只记 warning。这让「模型输出不可靠」不至于变成运行失败，代价是「模型其实想做点什么但格式写错了」会被静默吞成「收工」。

**源码事实：** `recovered` 标记在恢复路径上被消费一次就清掉。这既保证续跑从原位置继续，也保证该标记不会一直生效导致后续动作无法推进。

**源码事实：** `_act_on_task` 在基类抛 `NotImplementedError`，而不是给一个默认实现。这使 `plan_and_act` 模式在基类上是不可用的，必须由子类显式支持。

**架构推论：** 把「反应模式」做成运行时可切换的上下文字段（`rc.react_mode`）而非子类差异，意味着同一个角色类可以在不同场景下用不同推进方式。代价是行为分支集中在 `_think` 里，读代码时必须同时看模式和分支。

**架构推论：** 状态用整数下标表达，使「恢复」变成「把数字存下来再读回来」这种极简形式，不需要序列化动作对象本身（`todo` 在 context 里被排除序列化）。代价是动作列表的顺序成了隐式契约——一旦调整动作顺序，旧状态的含义就变了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/roles/role.py` | `RoleReactMode` | 三种反应模式的枚举定义 |
| `metagpt/roles/role.py` | `Role._think` | 按模式决定下一个状态，含降级与恢复提前返回 |
| `metagpt/roles/role.py` | `Role._react` | think-act 循环，受 `max_react_loop` 限制 |
| `metagpt/roles/role.py` | `Role._plan_and_act` | 规划模式：建计划、逐任务执行、处理结果 |
| `metagpt/roles/role.py` | `Role._act` | 动作结果归一为消息并写记忆 |
| `metagpt/roles/role.py` | `Role._observe` | 新消息去重、按 watch/send_to 过滤并写记忆 |
| `metagpt/roles/role.py` | `Role.react` | 三种模式的统一入口与收尾 |
| `metagpt/roles/role.py` | `Role.is_idle` | 空闲判定：无新消息、无待办、缓冲为空 |

## 参考

- [MetaGPT 源码：roles/role.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/roles/role.py)
- [MetaGPT 文档：Agent 与 Role 概念](https://docs.deepwisdom.ai/main/en/guide/tutorials/agent_about.html)
- [ReAct 论文：把推理与行动交错起来](https://arxiv.org/abs/2210.03629)

## 与主核 / AI 的关系

「状态是整数下标、动作列表顺序即契约」这套做法，与主核里「页面布局由定长槽位决定」是同一种取舍：用最短的可序列化表示换取最低的持久化成本，代价是布局一变旧数据就失去意义。主核靠 schema 版本号应对，这里的应对方式是清空状态重跑。

对 agent 工程最实际的观察是三种模式的适用面：`by_order` 适合流程完全确定的流水线（例如「写 PRD → 设计接口 → 写代码」这种软件公司 SOP），`react` 适合需要模型自己判断「现在该做哪一步」的开放任务，`plan_and_act` 适合需要先出计划再执行、且计划本身要评审的任务。MetaGPT 支持三者共存的理由很清楚：它的目标场景是「模拟软件公司」，而真实公司里既有固定流程也有需要判断的环节。

`_observe` 的过滤规则也值得对照：它用「动作来源」和「显式收件人」两个维度决定相关性，而不是靠语义相似度。这让消息路由完全确定、可调试，代价是必须由发送方正确设置 `cause_by` 与 `send_to`——一旦标错，消息会被静默丢弃，且没有兜底机制。
