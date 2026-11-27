---
project: metagpt
feature: base-role-contract
title: BaseRole 抽象与三类生命周期钩子
page_type: feature
schema_version: 1
refs:
  - path: metagpt/base/base_role.py
    symbols: [BaseRole, is_idle, react]
---

# BaseRole 抽象与三类生命周期钩子

## 一句话职责

`BaseRole` 把「一个角色」压缩成五个成员：三个抽象生命周期钩子（思考、行动、反应）、一个运行入口、一个空闲探针，其余一切（记忆、LLM、动作表）都不在基类里。

## 机制是什么

这个基类只有三十多行，却有明确的取舍：它声明的是**调度器需要什么**，不是**角色拥有什么**。

抽象成员共五个：`think`（想清楚下一步做什么）、`act`（执行当前动作）、`react`（按某种策略对外部消息作出反应）、`run`（观测—思考—行动的组合入口）、`get_memories`（取最近若干条记忆）。`react` 与 `run` 都被声明成 `async`，也就是说这个契约假设整个生命周期是异步的——因为真实实现里每一步都要等 LLM。

`is_idle` 是唯一的 `@property`，实现体直接 `raise NotImplementedError`。它刻意**没有**加 `@abstractmethod`：子类不实现它也能实例化，只是运行期取用时才会炸。这个非对称是整套契约里最值得注意的一处。

另外基类只声明了 `name: str` 这一条字段注解，没有默认值、没有校验。名字之所以只作为类型注解存在，是因为它同时被用作环境里的注册键与消息路由地址，具体怎么生成由子类决定。

## 怎么实现的

**抽象约束靠哪个元类生效。** `BaseRole` 继承 `BaseSerialization`，后者是 Pydantic 的 `BaseModel` 子类。Pydantic 的 `ModelMetaclass` 本身继承自 `ABCMeta`，所以 `@abstractmethod` 在这里是真的生效的：任何子类若漏实现 `think`、`act`、`react`、`run`、`get_memories` 中的任一，实例化时就会抛 `TypeError`，文案会点名缺哪几个。这条链路不靠文档约定，是元类层面强制的。

**序列化契约。** `BaseSerialization` 做了两件事：序列化时在产物里追加一个 `__module_class_name` 字段（形如「模块名.类名」）；反序列化时先设法把 dict 还原成真实子类。它的构造配置是 `extra="forbid"`，意味着多传一个未知字段就报错——这对「配置写错」比「静默忽略」更友好，代价是向前兼容性差。

还原逻辑里有个细节值得记下：类里有一个 `__is_polymorphic_base` 标记，但它在本仓从未被置为真。真正起作用的是一条兜底路径——当待还原的 dict 里的类名与当前 `cls` 不一致时，代码不只是放弃，而是继续往下查 `__subclasses_map__`（由 `__init_subclass__` 在每次子类定义时登记「模块名.类名 → 类对象」），查到就用那个类构造。于是「以基类名义反序列化出子类实例」这件事实际由这条兜底完成，而不是由那个标记完成。查不到时抛 `TypeError`，注释里留了「以后可以改成动态 import」的 TODO。

**为什么 `is_idle` 不做成抽象方法。** 它是调度信号：环境每轮遍历角色，跳过 `is_idle` 为真的；团队跑批时一旦所有角色都空闲就整轮退出。这样一个关键探针不强制实现，看起来是疏漏，实际上和「基类不想规定角色怎么判断空闲」有关——不同角色的空闲定义差异很大（有无未读消息、有无待办动作、缓冲是否为空）。代价是漏实现时错误暴露得晚。

**`name` 为什么只是注解。** 因为它承担双重身份：既是环境注册表 `roles` 的 key，又是消息路由的地址之一。某些角色希望名字就是类名（基类风格），某些希望带上实例化后的角色描述，所以基类只承诺「你能拿到一个字符串」，怎么算出来交给子类。

## 关键取舍

**源码事实：** 三个钩子（`think`/`act`/`react`）加两个入口（`run`/`get_memories`）构成契约，而「记忆」「LLM」「动作表」全部缺席。基类是调度视角的接口，不是角色的能力清单。

**源码事实：** `is_idle` 用属性实现而非方法，且不加抽象装饰。取用方式是 `role.is_idle` 而不是 `role.is_idle()`，这让调用侧读起来像在问「这个角色现在闲着吗」——一个状态，而不是一次动作。

**源码事实：** 抽象成员全部是 `async`（除了 `get_memories` 与 `is_idle`）。把异步写进基类契约，等于宣布「不支持同步实现的角色」——这与很多 agent 框架保留 sync/async 双入口的做法相反。

**架构推论：** 让 `BaseRole` 继承带多态反序列化的序列化基类，意味着角色可以被整体存盘再读回，且读回的是真实子类而不是被削平成基类。代价是所有子类都必须接受 `extra="forbid"`，新增字段必须同步到字段声明，不能靠运行时挂属性。

**架构推论：** 契约里没有「订阅」「发送」「加入环境」这类方法。也就是说基类不知道环境的存在——角色与环境的耦合被推到了 `Role` 子类和 `Environment` 一侧，基类只表达「给我消息我就动」这个最小假设。这是 MetaGPT 与 LangGraph 的明显分野：LangGraph 的调度单元是图上的节点，节点由框架装配读写集合；这里是角色自己持有动作表，框架只负责决定谁该动。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/base/base_role.py` | `BaseRole` | 角色抽象基类，声明五个抽象成员与一个字段注解 |
| `metagpt/base/base_role.py` | `is_idle` | 空闲探针，属性形式，未强制实现 |
| `metagpt/base/base_role.py` | `react` | 反应入口抽象方法，返回一条消息 |
| `metagpt/base/base_serialization.py` | `BaseSerialization` | 多态序列化基类，`extra="forbid"` 与子类登记表 |
| `metagpt/roles/role.py` | `Role` | 唯一实际实现者，补齐全部抽象成员 |

## 参考

- [MetaGPT 源码：base/base_role.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/base/base_role.py)
- [MetaGPT 源码：base/base_serialization.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/base/base_serialization.py)
- [Pydantic 文档：模型序列化](https://docs.pydantic.dev/latest/concepts/serialization/)

## 与主核 / AI 的关系

「基类只声明调度器所需的最小接口、把实现细节推到子类」与主核里抽象存储接口只暴露读页/写页/刷盘是同一种克制：接口窄，替换实现才便宜。差别在于主核的接口是数据面的原子操作，这里的接口是行为面的生命周期钩子，且带着一个不强制实现的探针。

对 AI 工程而言，这一页最实用的结论是角色契约的边界：`BaseRole` 不承诺记忆、不承诺 LLM、不承诺消息路由。所以任何「角色之间怎么通信」的问题都不该到基类找答案，而在环境与 `Role` 的两侧。对照 autogen 的 agent 以对话为中心、crewai 的 agent 以任务为中心，MetaGPT 的基类几乎什么都不说——它的规约藏在 `Role` 的动作表与环境的地址表里，这正是「SOP 驱动」的体现：流程不是基类给的，是具体角色配出来的。
