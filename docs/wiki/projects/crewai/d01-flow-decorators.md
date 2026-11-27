---
project: crewai
feature: flow-decorators
title: Flow 装饰器与触发条件
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/flow/dsl/_start.py
    symbols: [start]
  - path: lib/crewai/src/crewai/flow/dsl/_listen.py
    symbols: [listen]
  - path: lib/crewai/src/crewai/flow/dsl/_router.py
    symbols: [router]
  - path: lib/crewai/src/crewai/flow/dsl/_conditions.py
    symbols: [or_, and_]
---

# Flow 装饰器与触发条件

## 一句话职责

`@start` / `@listen` / `@router` 与 `or_` / `and_` 不做任何调度，它们的唯一产出是「把方法在流程里的角色写进方法自身的元数据」——执行、触发判定、并发与持久化全在运行时那一侧。

## 机制是什么

三个装饰器各自负责一种角色，并且可以叠加在同一方法上：

- `@start` 声明入口。它接受一个可选条件：不传就是无条件入口，传了就变成「被别的执行触发才启动的入口」。
- `@listen` 声明触发条件。条件是必填的：可以是字符串标签、别的方法引用，或 `or_()` / `and_()` 构造的复合条件。
- `@router` 声明「返回值是下一跳的标签」。它可选地带自己的触发条件（不带时用于和 `@start` / `@listen` 叠加），并且可以显式声明它会发出哪些标签。

叠加之所以成立，是因为每个装饰器产出的不是完整定义，而是一个只填了自己那几个字段的 `FlowMethodDefinition` 片段，再按「本片段显式设过的字段」合并进方法已有的定义。于是 `@start()` 加 `@listen("request_changes")` 得到的是「既是入口、又能在收到 request_changes 时重跑」的方法，而不是后者覆盖前者。

`or_` / `and_` 与三个装饰器不同：它们不接触方法，只返回一棵条件树。条件树是运行时可判定的数据结构，同时又能被投影成声明式的 `{"or": [...]}` / `{"and": [...]}` 形态。

## 怎么实现的

**包装器先造，元数据后挂。** 每个装饰器都先 `StartMethod(func)` / `ListenMethod(func)` / `RouterMethod(func)` 造一个包装器，再调 `_merge_flow_method_definition` 挂上定义片段。包装器本身是描述符：保留被装饰方法的静态签名与 `__name__`，`__get__` 在实例访问时重新绑定并逐个搬移非内部属性；元数据属性（人工反馈配置、持久化配置、方法定义、会话专用标记）在构造包装器时被逐一复制过来，这样多个装饰器叠加时前一个装饰器留下的配置不会被后一个的包装器吃掉。

**合并只认显式字段。** 合并用的是 Pydantic 的 `model_fields_set`，而不是字段当前值。这是叠加语义的关键：`@listen` 片段只设了 `do` 与 `listen`，合并时就只写这两个字段，`start` 原样保留。反过来说，`do` 每个片段都会重设，值都是同一个 `_method_action(func)` 产出的 `{"call": "code", "ref": "模块:限定名"}`，所以谁最后合并都不改变结果。

**start 的两种形态。** 条件为 `None` 时写 `start=True`；否则把条件交给 `_to_definition_condition`。前者是「kickoff 就跑」的强入口，后者是「等条件满足才跑」的条件入口——这个区分在运行时的 kickoff 决策里被直接读取（见 d03）。

**router 的 emit 有两个来源。** 显式 `emit=` 优先，经 `_normalize_router_emit` 去重（按出现顺序保序，元素一律转字符串）；没给就退回返回值注解，靠 `_get_router_return_events` 解析：先把可调用对象解包（`__func__` / `__wrapped__` / `unwrap()`），再取 `get_type_hints` 的返回值注解，从 `Literal`、字符串枚举、以及 `Union`/`Annotated` 里递归收集所有字符串字面量，去重后作为潜在事件。两条来源都没有时，emit 写 `None`——定义里不会出现下游事件，静态图与可视化就看不到这条边。

**条件树的构造与校验。** `or_` / `and_` 调同一个 `_condition_tree`，`type` 取大写常量 `"OR"` / `"AND"`，`conditions` 是子项列表。每个子项经 `_coerce_trigger` 归一：字符串直接用；可调用对象取其 `__name__`；已经是条件树的则做结构校验——键集合必须恰好是 `{type, conditions}`，`type` 必须在两个常量里，子项要么能取出名字、要么本身是合法条件树。任一项不合法立刻抛 `ValueError("Invalid condition")`，所以错误发生在装饰（导入）期，而不是运行到一半。

**两种条件形态的转换。** 运行期用的是大写 `OR`/`AND` 的树；声明契约用的小写 `{"or": [...]}` / `{"and": [...]}`，由 `_to_definition_condition` 递归转换，键名用小写。裸字符串条件在两边形态一致，所以最简条件（`@listen("approved")`）在定义里就是一个字符串。

## 关键取舍

**源码事实：** 装饰器是纯元数据标记，不包装调用。被装饰方法在被调用时仍按原函数执行，包装器只负责携带定义。这让「声明」与「执行」彻底分离，代价是调试时栈帧里看到的是包装器而不是原函数。

**源码事实：** 非法条件在装饰期就失败，而不是等到 kickoff。`@listen(123)` 这类错误会在类体执行时抛 `ValueError`，早于任何状态初始化。

**源码事实：** router 的下游事件要么写在注解里、要么写在 `emit=` 里，两者都没有就真的失明。源码因此专门提供 `emit=` 兜底，并在定义构建阶段对「有 emit 但不是 router」单独告警（见 d02）。

**源码事实：** `emit` 会去重且保序，但不做合法性校验——`emit` 里写一个永远不会被任何 `@listen` 消费的标签是允许的，只是那条边悬空。

**架构推论：** 「角色」与「条件」被拆成两组原语（装饰器 / 组合子），而不是给 `@listen` 增加 `any_of=`、`all_of=` 之类的参数。好处是条件可以任意嵌套复用；代价是条件只能引用「方法名 / 路由标签」这一种命名空间，字符串标签与路由结果共享同一空间，写错不会报错，只会静默不触发——这正是 d02 里专门加自监听校验的原因。

**架构推论：** 因为定义是数据，同一份流程可以有两种作者身份：Python 类投影出来的定义，或手写的 YAML 声明。装饰器只是前者的语法糖，这决定了 CrewAI 能把流程画出静态图、能在没有 Python 元数据的情况下复刻同一张图。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/flow/dsl/_start.py` | `start` | 声明入口；无条件写 `start=True`，有条件则转换条件树 |
| `lib/crewai/src/crewai/flow/dsl/_listen.py` | `listen` | 声明触发条件，条件必填 |
| `lib/crewai/src/crewai/flow/dsl/_router.py` | `router` | 声明路由方法；解析 `emit` 或返回值注解得到下游标签 |
| `lib/crewai/src/crewai/flow/dsl/_conditions.py` | `or_` / `and_` | 构造并校验递归条件树，转成声明式形态 |
| `lib/crewai/src/crewai/flow/dsl/_utils.py` | `_merge_flow_method_definition` | 按 `model_fields_set` 合并定义片段，实现装饰器叠加 |
| `lib/crewai/src/crewai/flow/flow_wrappers.py` | `FlowMethod` 及其子类 | 描述符包装器，保留签名并搬运元数据 |

## 参考

- [CrewAI 源码：Flow DSL 入口装饰器](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/flow/dsl/_start.py)
- [CrewAI 源码：路由装饰器](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/flow/dsl/_router.py)
- [CrewAI 文档：Flows 概念](https://docs.crewai.com/en/concepts/flows)

## 与主核 / AI 的关系

「装饰器只登记元数据、不参与执行」与主核里 `CREATE INDEX` 只写系统目录、真正检索由执行器后续使用是同一类设计：声明是数据，行为另算。差别在主核的目录条目会被规划器强制消费（索引不匹配就无法下推），而这里的定义允许悬空——`emit` 写了没人接、条件引用了不存在的方法名，都不会在加载期被拦住，只会在运行时表现为「没触发」。

条件树的形状也与主核的谓词树同构：都是小型的递归结构，都在运行时对着具体值求值。但语义不同——SQL 谓词对每行独立求值，无状态；Flow 的 `and_` 条件对「已发生事件集合」求值（见 d03 的 `_condition_met`），是有状态的累积。这个差别决定了同一份 `and_` 条件在循环流程里的行为不能按纯函数理解。

对 AI 工程的实际意义在于：把编排写成数据，才可能对编排本身做静态检查（CEL 根白名单、自监听环路）、做可视化、做跨语言复刻。代价是所有静态检查都只能是保守的近似——注解里没写的路由事件、字符串写错的标签，静态侧看不出来，只能靠运行时兜底（`max_method_calls` 无限循环保护）。
