---
project: metagpt
feature: team-investment
title: 团队投资与成本控制
page_type: feature
schema_version: 1
refs:
  - path: metagpt/team.py
    symbols: [Team, hire, invest, _check_balance]
  - path: metagpt/team.py
    symbols: [run_project, start_project]
---

# 团队投资与成本控制

## 一句话职责

`Team` 把「预算」当成一等公民：投资额直接写进成本管理器的上限，每一轮跑批前先查余额，超支就抛 `NoMoneyException` 终止整个项目。

## 机制是什么

一个 `Team` 只有四个字段：环境、投资额、初始需求、是否使用 MGX 环境。它不持有角色列表——角色挂在环境里。它也不持有 SOP——流程由角色各自的动作表拼出来。

所谓「投资」是两处赋值的合体：`self.investment = investment` 记下名义金额，`self.cost_manager.max_budget = investment` 把这笔钱交给成本管理器。而 `cost_manager` 是一个只读属性，实际返回的是 `self.env.context.cost_manager`——所以预算真正的落点不在团队对象上，而在环境共享的上下文里。

这意味着「投资」不是一个计数，而是一次**共享状态的上限设定**：同一个环境里的所有角色用同一份成本管理器，因此团队投资额就是整个进程里这批角色的合计花费上限。

## 怎么实现的

**构造期的环境选择。** `Team.__init__` 先跑父类初始化，然后按三种情况决定环境从哪来：显式传入 `env` 时只把它的 `context` 换成新上下文；未传且 `use_mgx` 为真时造一个 `MGXEnv`；未传且 `use_mgx` 为假时造一个普通 `Environment`。默认 `use_mgx=True`，也就是说默认走的是带团队协调规则的那套环境，而不是裸环境。

随后处理两个来自 `data` 的便捷键：`roles` 会被 `hire` 掉，`env_desc` 会被写进环境的 `desc`。这两个键不在字段声明里，靠的是构造参数直读，所以它们不能出现在 `model_dump` 之后再反序列化的路径上。

**雇佣。** `hire(roles)` 只有一行：`self.env.add_roles(roles)`。团队不自己维护成员表，也不做去重或冲突检查——重名角色会在环境的 `roles` 字典里互相覆盖（环境侧是按名字作 key 的）。

**预算检查的位置。** `_check_balance` 比较 `total_cost >= max_budget`，注意是 `>=` 而不是 `>`：花到刚好等于预算也算超支。它在 `run` 的循环里、每轮 `env.run()` **之前**调用。所以顺序是「查余额 → 跑一轮 → 再查余额」，最后那一轮的花费不会被事后检查——只有下一轮开头才会发现上一轮已经超支。

这个位置选择是刻意的还是凑巧的：放在 `env.run()` 之后会让「已经花完了」变成必须先跑一轮才知道，放在之前则等价于「入场费」——你被允许花掉最后一笔钱，但不被允许再开新一轮。

**跑批循环。** `run(n_round=3, idea="", send_to="", auto_archive=True)` 的结构是三段：

1. 若给了 `idea`，先 `run_project` 把需求作为一条消息投进环境。
2. `while n_round > 0`：先看 `env.is_idle`，全闲就 break；否则减一轮、查余额、`await env.run()`。
3. 循环结束后 `env.archive(auto_archive)`，返回 `env.history`。

`run_project` 本身极简：记下 `self.idea`，然后 `self.env.publish_message(Message(content=idea))`。它连 `send_to` 参数都没用上——这个参数在签名里保留着，但实现里没有消费它。需求的收件人完全由消息的默认路由（`<all>`）决定，也就是广播给所有订阅者。

`start_project` 是被标记废弃的兼容入口：发一条 `DeprecationWarning` 后转调 `run_project`。它的存在说明这个 API 曾经叫另一个名字，改名时选择了保留而非直接删除。

**归档。** `env.archive(auto_archive)` 只在「自动归档开启且上下文里设了 `project_path`」时才建 Git 仓库并提交。这是团队跑完后的收尾动作，与预算无关，但属于同一轮项目生命周期。

**整个 `run` 带一层装饰。** `@serialize_decorator` 包住 `run`：正常结束时原样返回结果；一旦抛异常（含 `KeyboardInterrupt`），记日志、调 `self.serialize()` 把团队信息连同上下文写成 JSON，然后**返回 None**（装饰器没有重新抛出）。也就是说超支导致的 `NoMoneyException` 不会传播给调用方，而是被转换成「存档 + 返回 None」。

## 关键取舍

**源码事实：** 预算是「共享成本管理器上的一个上限」，而不是团队自己扣减的计数器。角色调用 LLM 时直接把花费记进同一个管理器，团队只读总额。这让费用统计没有中间层，代价是团队看不到「某个角色花了多少」，也没有按角色分配预算的能力。

**源码事实：** 余额检查用 `>=`，且只在每轮开头做。所以「刚好花完预算」被视为超支，「超支一轮」是允许的——最坏情况下会超出整整一轮的花费。

**源码事实：** `run` 的异常被 `serialize_decorator` 吞掉并转成存档。这使超支不再是调用方能捕获的失败信号，而是一个「跑完了但返回 None」的静默结局。要判断项目是否因缺钱失败，必须去看存档或日志，不能靠返回值。

**架构推论：** 团队不持有角色，只持有环境。这使「多团队共享一个环境」在结构上可行（预算会互相串），也使得团队本身是可序列化的轻对象——它存档时主要是存上下文，角色与环境各自的序列化是另一条路径。

**架构推论：** 项目启动只有「投一条需求消息」这一个动作，没有显式的任务分配。SOP 的推进完全依靠角色的 `watch` 与动作表：谁订阅了需求类动作，谁就会在下一轮被唤醒。这也解释了为什么 `run_project` 的 `send_to` 参数没有实现——在这个模型里，「指定收件人」会破坏「流程由订阅关系涌现」的设计。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/team.py` | `Team` | 团队对象：环境、投资额、需求、环境选择开关 |
| `metagpt/team.py` | `Team.hire` | 把角色交给环境注册，团队自身不留成员表 |
| `metagpt/team.py` | `Team.invest` | 写入名义投资额并设定成本管理器上限 |
| `metagpt/team.py` | `Team._check_balance` | 每轮开头的余额检查，`>=` 判超支 |
| `metagpt/team.py` | `Team.run_project` | 把需求作为一条广播消息投入环境 |
| `metagpt/team.py` | `Team.start_project` | 已废弃的兼容入口，转调 `run_project` |
| `metagpt/team.py` | `Team.run` | 跑批循环：空闲退出、余额检查、归档收尾 |
| `metagpt/utils/cost_manager.py` | `CostManager` | 累计 token 与金额，持有 `max_budget` |
| `metagpt/utils/common.py` | `NoMoneyException` | 超支时抛出的异常类型 |

## 参考

- [MetaGPT 源码：team.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/team.py)
- [MetaGPT 源码：utils/cost_manager.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/utils/cost_manager.py)
- [MetaGPT 文档：多智能体与成本意识](https://docs.deepwisdom.ai/main/zh/guide/tutorials/multi_agent_101.html)

## 与主核 / AI 的关系

「共享一个可累加的资源计数器 + 在批处理边界检查上限」与主核的资源配额（内存上限、连接数上限）是同一种治理手法：不做事前精确预留，只做事后累计 + 边界拒绝。差别在于主核的上限一旦触达会影响分配路径，这里的上限只体现在「下一轮不许开跑」。

对 AI 工程而言，这一页的三个结论最实用。第一，预算是按团队（准确说是按环境上下文）共享的，多团队共用一个环境时预算会互相消耗，需要各自独立上下文才能隔离。第二，超支不是异常而是静默结束：`run` 被装饰器包住后返回 None，因此编排层必须以「返回 None / 日志里有 `NoMoneyException`」作为失败判据，而不能只 try/except。第三，`run_project` 不支持定向投递，想指定「这个需求先给产品经理」必须靠消息的 `send_to` 与角色的 `watch` 配合，而不是靠团队 API——这与 crewai 那种在任务上直接指定 `agent` 的模型是根本区别。
