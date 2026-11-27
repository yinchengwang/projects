---
project: metagpt
feature: role-context
title: 角色上下文与状态注入
page_type: feature
schema_version: 1
refs:
  - path: metagpt/roles/role.py
    symbols: [RoleContext, important_memory, history]
  - path: metagpt/roles/role.py
    symbols: [set_env, _set_state, publish_message]
---

# 角色上下文与状态注入

## 一句话职责

`RoleContext` 把「一个角色运行期需要知道的一切」收进一个可部分序列化的对象：环境引用、私有消息缓冲、记忆、动作游标、关注集合、反应模式；角色自身的字段只保留静态配置。

## 机制是什么

角色被拆成两层：`Role` 上的 `name`/`profile`/`goal`/`constraints`/`actions`/`llm` 是**配置**，`RoleContext` 里的十个字段是**运行期状态**。拆分标准不是「会不会变」，而是「要不要进存档」——上下文里带 `exclude=True` 的字段（环境、消息缓冲、当前动作、news）都不会被写进序列化产物。

字段清单与各自的序列化处置：

| 字段 | 类型 | 作用 | 是否序列化 |
|---|---|---|---|
| `env` | `BaseEnvironment` | 所在环境 | 否（避免循环引用导致递归超限） |
| `msg_buffer` | `MessageQueue` | 私有收件缓冲，异步追加 | 否 |
| `memory` | `Memory` | 长期记忆（消息+按动作索引） | 是 |
| `working_memory` | `Memory` | 规划模式的工作记忆 | 是 |
| `state` | `int` | 动作游标，`-1` 表示初始或终止 | 是 |
| `todo` | `Action` | 本轮要执行的动作 | 否 |
| `watch` | `set[str]` | 关注的动作来源集合 | 是 |
| `news` | `list[Message]` | 本步观测到的相关消息 | 否 |
| `react_mode` | 枚举 | 三选一的反应策略 | 是 |
| `max_react_loop` | `int` | think-act 循环上限 | 是 |

`news` 与 `todo` 被排除是同一个理由：它们都是从「已序列化的东西」加上「这一轮收到的消息」推导出来的中间量，存下来反而会出现与记忆不一致的陈旧副本。

## 怎么实现的

**两个只读视图。** `important_memory` 与 `history` 都是 `@property`，语义分得很清：前者是 `memory.get_by_actions(watch)`，按「消息的 `cause_by` 落在关注集合里」从记忆的**动作索引**里取消息；后者是 `memory.get()`，取全部历史。

`history` 依赖一个易被忽略的边界行为：记忆的 `get(k)` 实现是 `storage[-k:]`，而 `k=0` 时 `-0` 就是 `0`，于是切片退化成「整个列表」。所以「取最近 k 条」与「取全部」被同一个参数表达，`k=0` 是「不限」而不是「零条」。这个约定在 `get_memories(k=0)` 与 `_act` 传 `rc.history` 两处都被依赖。

`important_memory` 走索引而非线性扫描：写入记忆时若消息带 `cause_by`，同一条消息会被同时追加进 `index[cause_by]` 列表。因此「某角色关心哪几类消息」是一次按 key 的字典查找，代价是索引与主存储必须同步（删除消息时两处都要动）。

**状态的注入点。** `_set_state(state)` 做两件事：写 `rc.state`，再 `set_todo(actions[state] if state >= 0 else None)`。`set_todo` 在值非空时把角色自己的 `self.context` 挂到动作上——这是动作能拿到 LLM 与配置的唯一通路。所以「状态推进」与「动作获得运行依赖」是同一个原子动作，任何绕过 `_set_state` 直接改 `rc.state` 的写法都会留下一个没有 context 的动作。

**环境注入。** `set_env(env)` 不是简单赋值，它连锁做了四件事：写 `rc.env`；让环境登记本角色的地址（`env.set_addresses(self, self.addresses)`）；用 `_get_prefix()` 重算 LLM 的系统提示（此时提示里才能拼上「你和哪些角色在同一环境里」）；把角色所属的 `context.cost_manager` 挂到 LLM 上做费用记账；最后 `set_actions(self.actions)` 重建动作表，使每个动作重新绑定 LLM、前缀与 context。

`set_actions` 是销毁式重建：先 `_reset()` 清空 `states` 与 `actions`，再逐个 `_init_action` 重新追加。这让「换环境后重新注入」是幂等的，代价是动作实例上的临时状态会被重建覆盖（动作实例本身被复用，但 LLM 与前缀被重置）。

`_get_prefix` 有一条分支值得注意：若角色设了 `desc`，直接返回它，完全跳过 profile/goal/constraints 与团队描述。这给了角色一个「覆盖自我描述」的开关，也让「提示里没有其他角色名」成为一种合法的可选状态——它不一定意味着环境为空，可能只是设了 `desc`。

**出站路径。** `publish_message` 是一个带三处早退的归一化过程：

1. 消息的 `send_to` 里若含 `<self>` 占位，先替换成本角色的类全名（写入）再把该占位删掉。
2. `sent_from` 为空或仍是 `<self>` 时补成类全名。
3. 若收件人全是「本角色的类全名或本角色名」，直接 `put_message` 进自己的缓冲并返回——自投递不进环境，因而不广播。
4. 没有环境时直接 `return`，消息被静默丢弃（日志里也没有痕迹）。
5. 否则给未标 agent 的 `AIMessage` 盖上 `_setting`（形如「名字(角色)」），再交给 `env.publish_message`。

三处早退的顺序很重要：自投递检查在环境检查之前，所以「没有环境但发给自己」能成功入缓冲。这也解释了为什么 `Role.put_message` 是公开方法——它是自投递的底层入口。

## 关键取舍

**源码事实：** 上下文与角色本体用两个对象分离，且上下文的字段逐个标注是否入库。这让「角色配置」与「角色运行痕迹」在存档里彻底分开：存档能恢复记忆与游标，但不会试图恢复环境对象与未读缓冲。

**源码事实：** `rc.env` 与 `rc.msg_buffer` 必须排除序列化，否则会出现图的循环引用（环境持有角色、角色持有环境）与队列不可序列化的问题。源码注释把这条写成了对递归超限的规避，而不是纯风格选择。

**源码事实：** 状态是「整数游标 + 动作表下标」，不是动作对象引用。因此恢复时只要动作表顺序不变，游标就能对应回原动作；一旦调整动作顺序，旧游标会指向另一个动作。

**源码事实：** `Role` 里 `name` 既是字段（`name: str = ""`）又在类体里被同名 `@property` 覆盖，所有内置角色都在子类里重新声明类属性（产品经理是 `Alice`、架构师是 `Bob`）。字段声明与属性同名时，字段收集阶段会把该属性当成字段的默认值，并把属性从类命名空间里摘掉——于是 `name` 字段的默认值是那个 property 对象本身，而不是空串。结果是「名字必须显式给出」由子类纪律保证；若某角色既不设类属性也不传参，`self.name` 得到的是 property 对象，`_setting` 拼出来的「名字(角色)」也会带着它。

**架构推论：** `set_env` 与 `set_actions` 的连锁重置意味着「角色一旦加入环境就会重建动作表」。好处是不会出现「动作拿着上一份 LLM 配置」的错配；代价是每次换环境都丢失动作实例上的临时状态，且有副作用的动作初始化会被重复执行。

**架构推论：** 出站归一化集中在 `publish_message` 而不是让业务代码自己拼地址，使「发给自己」「广播」「点对点」三种意图可以由 `send_to` 的值域表达。而没有环境时静默丢弃这一点，是「角色可以脱离环境单独测试」的代价：调试时消息凭空消失，只能靠 `put_message` 或显式检查 `rc.env`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/roles/role.py` | `RoleContext` | 运行期上下文：环境、缓冲、记忆、游标、关注集、模式 |
| `metagpt/roles/role.py` | `RoleContext.important_memory` | 按关注集合从动作索引取相关消息 |
| `metagpt/roles/role.py` | `RoleContext.history` | 取全部记忆，`k=0` 表示不限 |
| `metagpt/roles/role.py` | `Role.set_env` | 注入环境并连锁刷新地址、前缀、成本管理器与动作表 |
| `metagpt/roles/role.py` | `Role._set_state` | 推进游标并把对应动作挂上 context |
| `metagpt/roles/role.py` | `Role.publish_message` | 出站归一化：自投递短路、无环境丢弃、交给环境广播 |
| `metagpt/roles/role.py` | `Role.set_todo` | 设置当前动作并注入 context |
| `metagpt/memory/memory.py` | `Memory.get_by_actions` | 关注集合到消息列表的索引查询 |

## 参考

- [MetaGPT 源码：roles/role.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/roles/role.py)
- [MetaGPT 源码：memory/memory.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/memory/memory.py)
- [MetaGPT 文档：团队与角色协作](https://docs.deepwisdom.ai/main/zh/guide/tutorials/multi_agent_101.html)

## 与主核 / AI 的关系

「把状态拆成可持久化与不可持久化两半、并逐个字段标注」与主核区分「缓冲池里的脏页」和「已落盘的页」是同一类边界工作：都要明确哪些东西能在崩溃后重建、哪些必须写盘。差别在于主核的重建靠日志重放，这里靠「记忆 + 游标」这一对最小信息。

对 AI 工程而言，两处取舍最值得记住。其一是 `history` 与 `important_memory` 的分工：前者进提示的历史，后者决定「哪些消息值得关注」，两者都不做语义相似度筛选，全靠 `cause_by` 与 `send_to` 两个显式字段。这让行为可复现，但要求发送方标对来源与收件人。其二是出站的三条早退——自投递、无环境、广播——它们把「消息去哪了」变成了三个可判定的分支；调试「角色没反应」时应当先确认消息是进了自己的缓冲、还是根本没有环境可投、还是投出去后没人订阅。
