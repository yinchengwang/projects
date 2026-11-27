---
project: metagpt
feature: planner
title: 任务规划器与人工确认
page_type: feature
schema_version: 1
refs:
  - path: metagpt/strategy/planner.py
    symbols: [Planner, update_plan, process_task_result]
  - path: metagpt/strategy/planner.py
    symbols: [ask_review, confirm_task]
---

# 任务规划器与人工确认

## 一句话职责

Planner 维护一份任务计划和一个「当前任务」指针，把「生成计划—执行—评审—勾掉或改计划」这个循环封装起来，并在每个环节留出人工确认的插口。

## 机制是什么

`Planner` 有三个字段：`plan`（`Plan` 对象，含 goal、context、tasks、current_task_id）、`working_memory`（一个只服务于「当前任务」的临时记忆，任务完成就清空）、`auto_run`（是否跳过人工确认）。

它的状态机全部落在 `Plan` 上：`current_task_id` 指向第一个 `is_finished=False` 的任务，`finish_current_task()` 把它标记完成并重新算当前任务。所以「进度」是一个指针，不是计数器。

对外只有四个动作：`update_plan`（生成或修订计划）、`process_task_result`（处理一个任务的结果）、`ask_review`（求确认）、`confirm_task`（勾掉任务）。外加两个取上下文的工具方法 `get_useful_memories` 与 `get_plan_status`。

## 怎么实现的

**update_plan 是一个「生成—预检—确认」的循环。** 结构是 `while not plan_confirmed`：

1. 用 `get_useful_memories()` 拼上下文，调 `WritePlan().run(context, max_tasks)` 拿一段文本响应；
2. 把响应作为 assistant 消息写进 `working_memory`（这样下一轮它能看到自己上次生成的计划）；
3. `precheck_update_plan_from_rsp` 做**干跑校验**——它 `deepcopy` 当前计划，在副本上真的执行一次 `update_plan_from_rsp`，异常则返回 `(False, error)`。这一步是纯本地 JSON 解析与 `Plan` 操作，不花 LLM 钱就能判出「格式不对」或「task_id 不合法」；
4. 预检失败且还有重试次数，就把错误信息也写进 `working_memory`（让模型下一轮看到「你上次错在哪」），`max_retries -= 1`，`continue`；
5. 预检通过才 `ask_review`，未确认就再循环。

循环结束后才 `update_plan_from_rsp(rsp, self.plan)` 真正改计划，然后 `working_memory.clear()`。

**预检这一步是整页最值得注意的设计。** 它把「模型输出是否可解析」与「用户是否认可这个计划」分成两道关，而且顺序是先解析后问人。这避免了「人看着一份其实解析不了的 JSON 说确认」这种尴尬，也让 `max_retries` 只消耗在格式问题上，不用打扰用户。

**确认有两条完全不同的通道。** `ask_review` 按 `auto_run` 分岔：

- 非自动：调 `AskReview().run(...)`，这是个用 `get_human_input` 问真人的动作。它把计划全表打出来（每个任务的 id、instruction、是否完成）、把最近的上下文打出来，再按 trigger 类型选择提示语（任务评审或代码评审）。用户的输入里若含 `exit` 就直接 `exit()` 退出进程。
- 自动：`confirmed = task_result.is_success if task_result else True`。注意它完全不调模型——自动模式的判断依据是「代码跑成功了没有」。

这个分岔带来一个语义差异，docstring 写得很明确：人工确认时「用户说完成就算完成，不管代码跑没跑通」；自动模式下「代码必须跑成功才算完成」。两条路径的严格程度不同，是有意的。

**确认判定的细节。** `AskReview` 里 `confirmed` 的计算是：

```
rsp.lower() in CONTINUE_WORDS or CONTINUE_WORDS[0] in rsp.lower()
```

即「完全等于某个确认词」或「包含 confirm」。这允许用户说「confirm 这个任务，但把下一个任务改成……」——既确认了当前任务，又带了额外指令。这个「带尾巴的确认」在 `confirm_task` 里被专门处理。

**process_task_result 是三路分支。** 拿到 `TaskResult` 后先求评审：

- 确认通过 → `confirm_task`；
- 评审文本里含 `"redo"` → 什么都不做（注释说「simply pass, not confirming the result」）。效果是当前任务指针不动，下一轮 `_plan_and_act` 会再取到同一个任务重新执行，而这次会带上刚才写进 `working_memory` 的评审意见作为反馈；
- 其他情况 → `update_plan()`，即按用户反馈改计划（可能增删任务、调整顺序）。

redo 这一支的实现是「空操作」而不是显式重置，这有点反直觉但有效：任务没被标完成，`current_task` 自然还是它；而评审意见已经进了 `working_memory`，会在下次 `get_useful_memories` 时被带上。

**confirm_task 处理「带尾巴的确认」。** 它先 `task.update_task_result(...)`（把这次执行的 code 与 result 追加到任务上），再 `finish_current_task()`，再清 `working_memory`。然后判断那句有点绕的表达式：

```
confirmed_and_more = (CONTINUE_WORDS[0] in review.lower() and review.lower() not in CONTINUE_WORDS[0])
```

即「含 confirm 但不是纯 confirm」。命中就把评审文本重新塞回 `working_memory` 并再调一次 `update_plan()`——因为用户除了确认还要求改计划。清空之后又塞回去，顺序上是对的：先清掉本轮任务的工作记忆，再放用户的新指令进去。

**两个上下文构造器分工明确。** `get_useful_memories` 造的是「结构性上下文」：goal、plan 的 context、全部任务的 JSON（可排除字段）、当前任务的 JSON。它拼成一条 user 消息，后面接 `working_memory` 的全部内容。注释说这是为了缩短上下文、提升性能——它不复用角色记忆里的大段历史。

`get_plan_status` 造的是「进度报告」：已完成任务的代码（`remove_comments` 去注释后拼接）、已完成任务的执行结果、当前任务的 instruction、当前任务已完成部分的代码与结果，以及一段来自 `TaskType.get_type(...)` 的指引文本。它支持 `exclude` 参数把 `code` 或 `task_result` 换成占位串，用于在提示里省略过长的部分。

两者用途不同：前者喂给「写计划」与「评审」，后者喂给「写代码」——所以后者带了具体的代码与执行结果，前者只带任务描述。`remove_comments` 那一步是为了省 token，因为已完成任务的注释对新代码没有参考价值。

## 关键取舍

**源码事实：** 预检用 `deepcopy` 在副本上真跑一次更新，失败则回滚。这让「解析失败」与「用户不认可」互不干扰，也避免了计划被半途改坏。

**源码事实：** 自动模式完全不调模型评审，判断依据是 `TaskResult.is_success`。人工模式则完全以人的判断为准，代码失败也可以确认完成。两条路径的语义不同，不是同一逻辑的自动化。

**源码事实：** `redo` 的处理是空操作。依赖「任务未标完成 → 指针不动 → 下一轮重取同一任务」这个副作用，而不是显式重置。

**源码事实：** `working_memory` 在任务完成后清空，但在「带尾巴的确认」场景里立刻被写回用户指令。它实际上是「当前任务的工作台」，不是跨任务的记忆。

**架构推论：** 计划的真相只有一个地方——`Plan.tasks` 与 `current_task_id`。Planner 自己不持有进度状态，所有判定都从 `Plan` 派生。这让「保存/恢复」变成只序列化 `Plan` 一件事，代价是每次操作都要重算拓扑序（`_update_current_task` 里就调了 `_topological_sort`）。

**架构推论：** 人工确认被放在「计划生成」与「任务完成」两个节点上，而不是每一步。这意味着人只在两个关键决策点介入：这个计划行不行、这个结果行不行。中间的执行细节不打扰人，这与人机协作的实际节奏吻合。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/strategy/planner.py` | `Planner` | 计划、工作记忆与自动模式的持有者 |
| `metagpt/strategy/planner.py` | `Planner.update_plan` | 生成—预检—确认循环，通过后才真正改计划 |
| `metagpt/strategy/planner.py` | `Planner.process_task_result` | 确认、重做、改计划三路分支 |
| `metagpt/strategy/planner.py` | `Planner.ask_review` | 人工通道走 `AskReview`，自动通道看 `is_success` |
| `metagpt/strategy/planner.py` | `Planner.confirm_task` | 记结果、勾任务、处理带尾巴的确认 |
| `metagpt/strategy/planner.py` | `Planner.get_useful_memories` | 结构化上下文：目标、计划、当前任务 |
| `metagpt/strategy/planner.py` | `Planner.get_plan_status` | 进度报告：已完成代码与结果、当前任务进度、任务类型指引 |
| `metagpt/actions/di/write_plan.py` | `precheck_update_plan_from_rsp` | 在深拷贝的副本上干跑一次以验证可解析性 |
| `metagpt/actions/di/write_plan.py` | `update_plan_from_rsp` | 单任务改/增与整份计划替换的分流 |
| `metagpt/actions/di/ask_review.py` | `AskReview` | 打印计划与上下文，读真人输入并判定确认 |
| `metagpt/schema.py` | `Plan.finish_current_task` | 标记完成并把指针移到下一个未完成任务 |

## 参考

- [MetaGPT 源码：strategy/planner.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/strategy/planner.py)
- [MetaGPT 源码：actions/di/write_plan.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/di/write_plan.py)
- [MetaGPT 源码：schema.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/schema.py)
- [MetaGPT 文档：Data Interpreter 任务规划](https://docs.deepwisdom.ai/main/en/guide/tutorials/agent_about.html)

## 与主核 / AI 的关系

「先本地干跑校验、再交给人审」这个顺序与主核里「先语义分析、再执行」是同一个骨架：能在不付代价的阶段发现的错误，就不要留到付费阶段。主核的语义分析不碰磁盘，这里的预检不碰 LLM。

对 agent 工程最实用的是两点。其一，`update_plan_from_rsp` 里那条「单任务 vs 整份计划」的判定规则——若生成结果只有一个任务，或第一个任务声明了 `dependent_task_ids`（说明它依赖既有计划里的任务），就只更新那一个；否则才整份替换，并用「公共前缀」策略保留已有进度。这条规则决定了「改计划」不会把已完成的进度清掉。其二，自动模式的确认依据是代码是否跑成功，而不是再问一次模型——这避免了「模型自评通过、实际没跑通」这类假阳性，代价是自动模式无法接受「结果不完美但可接受」的情形。
