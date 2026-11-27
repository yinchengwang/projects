---
project: metagpt
feature: role-memory
title: 角色记忆与消息归档
page_type: feature
schema_version: 1
refs:
  - path: metagpt/memory/memory.py
    symbols: [Memory, add, try_remember]
  - path: metagpt/memory/memory.py
    symbols: [get_by_role, get_by_actions, delete_newest]
---

# 角色记忆与消息归档

## 一句话职责

`Memory` 是一个角色见过什么消息的账本：一条主列表按到达顺序保存全部消息，外加一份按「触发它的动作」建立的反向索引，让角色能按来源而不是按时间取回历史。

## 机制是什么

角色的所有认知都从记忆里取：`_observe` 把新消息写进来，`_think` 的提示词从记忆里拼历史，`_act` 把动作输出也写回来。这套读写的代价必须极低，因此 `Memory` 没有做语义检索，只做两件事：

- **主列表 `storage`**：`list[Message]`，追加即归档，天然有序。
- **反向索引 `index`**：`defaultdict[str, list[Message]]`，键是 `message.cause_by`（触发该消息的动作的字符串标识），值是同一动作产出的消息列表。

两个容器的成员关系由 `add` 与 `delete` 同步维护。注意索引只对 `cause_by` 非空的消息建立——用户直发的消息没有上游动作，它只能从主列表按时间取到。

## 怎么实现的

**`add` 的三步。** 第一，若 `ignore_id` 为真，先把消息 id 强行改成 `IGNORED_MESSAGE_ID`——这是给「不关心消息身份」的场合准备的开关，让去重逻辑退化。第二，`if message in self.storage: return`，用 `Message` 的相等比较做去重，重复消息直接丢弃。第三，追加进主列表，并在 `cause_by` 非空时同步追加进索引。

**`try_remember` 是唯一的全文匹配入口。** 它对 `storage` 做子串包含判断（`keyword in message.content`），不做分词、不做相似度。同类的还有 `get_by_content`，两者行为几乎一致，区别只在命名意图：一个是「回忆关键词」，一个是「按内容查」。它们是这个类里唯一非精确匹配的查询，仍然是 O(n) 线性扫描。

**按角色取回。** `get_by_role` 遍历主列表筛 `message.role`。角色取值是 user / assistant / system 这类会话角色，与 MetaGPT 的「角色（Role 对象）」不是同一个概念——这一点在读代码时容易混淆：`Memory` 里没有「哪个 Role 说的」这个维度。

**按动作取回。** `get_by_action` 与 `get_by_actions` 走索引。前者把动作对象转成字符串后直接查索引，**不做存在性检查**，所以查一个从未出现过的动作会返回 `defaultdict` 自动创建的**空列表**——注意是「返回空列表」而不是「返回 None」，但同时也把这个键永久写进了索引。后者用 `any_to_str_set` 归一多个动作，逐个查并跳过不存在的键，再拼接结果。

拼接顺序值得注意：`get_by_actions` 的结果按「动作在集合里的迭代顺序」分组，而不是按消息的原始时间顺序。调用方若依赖时间序，必须自己再排一遍。

**`delete_newest` 的回滚语义。** 它从主列表尾部弹出一条，若该消息有 `cause_by` 且在索引里存在就同步移除，返回被删掉的这条；列表为空时返回 `None`（而不是抛异常）。这个方法服务于「动作执行失败后撤销它刚写下的消息」这种场景，所以只删最新一条，不做任意位置删除。

`delete` 做的是任意位置删除：同样先处理 `ignore_id`，再 `list.remove`（消息不存在会抛 `ValueError`），最后同步索引。两个删除入口的容错度不同——`delete_newest` 容忍空表，`delete` 不容忍缺失。

**`get` 的 k=0 陷阱。** 实现是 `self.storage[-k:]`。`k=0` 时 `-0` 等于 `0`，切片退化成 `storage[0:]`，即**返回全部消息**。文档里写的是「k=0 时返回全部」，所以这个行为是靠 Python 切片的副作用实现的，而不是靠显式分支。任何把 `k` 当「最多取几条」来理解的调用方都会在这里出错。

**`find_news` 的比较方式。** 它取最近 k 条作为「已见集合」，然后逐条检查传入的 `observed` 是否在其中。判断用 `i in already_observed`，即逐个 `__eq__` 比较，复杂度是观察数乘已见数。这个朴素实现对几十条消息够用，但它同时是长期记忆的接入点：`LongTermMemory` 覆写它，在短期过滤之后再加一层向量相似度过滤。

**`get_by_position` 的容错。** 它挂了 `handle_exception` 装饰器，越界时返回 `None` 而不是 `IndexError`。这让「取倒数第 memory_k+1 条」这类可能越界的定位可以安全调用——长期记忆正是靠这条路径取「刚滑出窗口的那一条」。

## 关键取舍

**源码事实：** 索引键是 `message.cause_by` 单个字符串，不是列表。一条消息只能挂在一个动作名下，即使它在语义上由多个动作共同导致。这让索引结构最简，代价是「多因消息」只能选一个代表动作。

**源码事实：** 去重依赖 `Message.__eq__`，而 `Message` 是 pydantic 模型，相等比较覆盖全部字段。因此「内容相同但 metadata 不同」的两条消息会被当作不同消息同时留下——去重不是按内容做的。

**源码事实：** `get_by_action` 会因查不存在的键而污染索引（`defaultdict` 副作用）。这是有意的省事还是疏漏无法从代码判断，但后果确定：索引里会出现值为空列表的键，`get_tool_tags` 一类的「键即存在」判断如果在 Memory 上照搬会误判。

**架构推论：** 整套记忆不区分时间窗口。`storage` 只增不减，唯一的收缩手段是 `delete`/`delete_newest`/`clear`。这就是为什么后来需要 `LongTermMemory` 与 `BrainMemory` 两个更重的实现：前者负责把旧消息外迁到向量库，后者负责把历史压成摘要。基础 `Memory` 只提供「全量保留」这一种策略。

**架构推论：** `Memory` 是 pydantic `BaseModel`，`storage` 与 `index` 都是可序列化字段。因此角色的记忆可以随角色一起持久化、恢复，不需要额外协议。代价是每次读写都走 pydantic 的字段校验路径，比裸列表慢。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/memory/memory.py` | `Memory` | 主列表 + 按动作索引的记忆容器 |
| `metagpt/memory/memory.py` | `Memory.add` | 去重、可选的 id 抹除、双容器同步写入 |
| `metagpt/memory/memory.py` | `Memory.try_remember` | 关键词子串匹配取回 |
| `metagpt/memory/memory.py` | `Memory.get_by_role` | 按会话角色线性筛选 |
| `metagpt/memory/memory.py` | `Memory.get_by_actions` | 多动作索引查询，缺失键跳过 |
| `metagpt/memory/memory.py` | `Memory.delete_newest` | 尾部回滚，空表返回 None |
| `metagpt/memory/memory.py` | `Memory.find_news` | 短期去重，长期记忆的覆写点 |
| `metagpt/memory/memory.py` | `Memory.get_by_position` | 带异常兜底的下标取回 |

## 参考

- [MetaGPT 源码：memory/memory.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/memory/memory.py)
- [MetaGPT 文档：角色与记忆](https://docs.deepwisdom.ai/main/en/guide/tutorials/agent_about.html)
- [MetaGPT 源码：roles/role.py 中的 _observe](https://github.com/geekan/MetaGPT/blob/main/metagpt/roles/role.py)

## 与主核 / AI 的关系

「主结构 + 反向索引，两份容器靠同一个写入入口保持同步」与主核里「堆表 + 二级索引」是同一种结构：主核在插入行时同步维护索引项，删除时同步移除，任何绕过统一入口的写入都会让索引失真。这里用 `add`/`delete` 两个入口守住同一件事，而 `ignore_id` 与 `defaultdict` 两处副作用则分别对应主核里的「跳过索引」与「索引项残留」。

对 agent 工程而言，这一页解释了两个实际行为。其一，角色不会自动遗忘：基础 `Memory` 只增不减，长会话必然把提示词撑爆，所以生产上要么换 `LongTermMemory` 要么换 `BrainMemory`。其二，`get(k=0)` 返回全部而不是空，若拿它做「取最近 k 条」的默认参数，第一次调用就会把完整历史塞进提示词——这是这类「0 表示不限制」约定最容易踩的坑。

