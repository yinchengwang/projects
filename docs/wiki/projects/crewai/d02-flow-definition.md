---
project: crewai
feature: flow-definition
title: Flow 定义契约与状态模型
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/flow/flow_definition.py
    symbols: [FlowDefinition, FlowMethodDefinition, FlowConfigDefinition]
---

# Flow 定义契约与状态模型

## 一句话职责

`FlowDefinition` 是一份「可序列化、可手写、可校验、与运行引擎无关」的流程契约：它描述流程有哪些方法、每个方法干什么、何时触发、状态长什么样，既是 Python 类的投影结果，也是 YAML 声明的直接输入。

## 机制是什么

这份契约的核心判断是：**流程的语义应该能脱离 Python 类存在**。因此定义里不存函数对象、不存 LLM 实例、不存持久化后端，只存「引用」与「配置」。

定义的顶层结构分五块：

- `schema` 固定为 `crewai.flow/v1`，用于声明式文件的版本识别；
- `name` / `description`；
- `state`：状态契约，是一个按 `type` 判别的四选一联合——`dict`、`pydantic`、`json_schema`、`unknown`；
- `config`：执行配置（流式、追踪、记忆、输入提供方、事件抑制、单次最大方法调用数等）；
- `methods`：方法名到 `FlowMethodDefinition` 的映射，这是权重最大的部分。

`FlowMethodDefinition` 把「一个方法」拆成八个可独立填写的槽位：`do`（动作）、`start`（入口标记或条件）、`listen`（触发条件）、`router`（是否路由）、`emit`（可发出的标签）、`human_feedback`、`persist`、`description`。每个槽位都是可选的（`do` 必填），这正是装饰器叠加能工作的前提。

`do` 也不是单一类型，而是按 `call` 判别的七路联合：`code`（Python 可导入引用）、`tool`、`crew`、`agent`、`expression`（CEL）、`script`（内联 Python）、`each`（对列表逐项跑子流水线）。

## 怎么实现的

**状态契约的四种形态。** `dict` 只带一串默认值；`pydantic` 带 `module:qualname` 引用与可选的 `default`；`json_schema` 带内联 schema；`unknown` 表示构建期就没能序列化出类型，只能记一个尽力而为的引用。四者用 `Field(discriminator="type")` 挂在同一个字段上，所以给一份 YAML 时 `type` 一写错就立刻校验失败。运行时按这个判别结果决定怎么建状态模型（见 d03 的 `_build_definition_state_model`）。

**配置项的取舍尺度。** `FlowConfigDefinition` 把「能影响执行又需要跨进程传递」的东西都放进来：`stream`、`memory`（只存可序列化配置，不存实例）、`input_provider`（存引用字符串，加载时再解析）、`suppress_flow_events`、`max_method_calls`（默认 100，无限循环的兜底阈值）、`defer_trace_finalization`、`checkpoint`。`tracing` 是 `bool | None`——三态而非两态，因为「没表态」和「显式关掉」在追踪开关的决策链里语义不同。

**活对象与可序列化形态的双面性。** 有三个字段会出现「同一个字段，内存里是可以用的对象，落盘时是配置」的情况：`FlowPersistenceDefinition.persistence`、`FlowHumanFeedbackDefinition.llm`、`FlowHumanFeedbackDefinition.provider`。它们的处理方式一致——字段类型写成 `Any`，配一个 `when_used="json"` 的字段序列化器：持久化后端若是 Pydantic 模型就 dump，dump 不了就退化成 `{"ref": "模块:限定名"}`；LLM 走 `to_config_dict()`，没有这个方法就拼 `provider/model`；provider 一律退化成引用。源码注释把这层意思写得很直白：引擎在进程内会用用户给的那个确切实例，只有投影成声明时才降级。

**三个模型级校验。** 定义在构造完成后跑三条 `model_validator(mode="after")`：

1. 方法名必须匹配 `^[A-Za-z_][A-Za-z0-9_]*$`——因为方法名会成为事件标签、CEL 里的 `outputs.<name>`、以及可能的函数引用，不能用任意字符串。
2. 禁止自监听：递归检查 `listen` 条件是否引用了方法自身。这是无限循环最常见的成因（监听自己的完成事件），而且报错文案会分两种场景——会话式流程里 `@listen` 的标签与路由名共享命名空间，冲突更隐蔽，所以文案专门解释「标签其实就是路由名，这个处理器会自己循环」。
3. 对 `do` 做 CEL 校验：`expression` 动作的表达式必须以 `state` / `outputs` 为根；`code` / `tool` 的 `with` 映射、`crew` 的 `inputs`、`agent` 的 `input` 按模板校验其中的 `${...}`；`each` 的 `in` 用基础根集合、每个子步骤的 `if` 与动作额外允许 `item` 根。未知动作类型会走到末尾的 `raise TypeError`——这是一条显式的「新增动作类型必须补校验分支」提示，而不是默默放过。

**`is_start` 的三态归一。** 字段类型是 `bool | 条件 | None`。手写声明里可能显式写 `start: false` 来表示「这不是入口」，所以判入口用的是 `bool(self.start)` 而不是 `is True`：`False`、`None`、空串都算「不是入口」，只有真值与条件才算。

**人工反馈会改写路由。** `FlowMethodDefinition` 上有一条校验：只要 `human_feedback.emit` 有值，就强制把 `router` 置真并清空方法级 `emit`。理由是「带 emit 结果的人工反馈本质上就是路由」——把这条规范形式统一在契约层，运行时就不必再分情况判断。

**加载与导出。** `from_declaration` 接受定义对象、字典或 YAML 文本/文件路径；YAML 走 `yaml.safe_load` 后必须得到映射；空内容与空文件分别给出不同错误。加载完立刻跑一遍 `log_flow_definition_issues`，把「有 emit 但不是 router」「人工反馈的 `default_outcome` 缺少 emit 或不在 emit 里」记成告警或错误日志——注意是记日志而不是抛异常，这决定了这类配置错误默认不会中断执行。反向的 `to_dict` 用 `by_alias=True` 导出，于是 `schema_` 变回 `schema`、`with_` 变回 `with`、`if_` 变回 `if`、`in_` 变回 `in`——下划线后缀是为了避开 Python 关键字，别名是为了声明文件里能写自然名字。

## 关键取舍

**源码事实：** 契约里没有函数对象，方法体一律以 `{"call": "code", "ref": "模块:限定名"}` 表示。这让定义能落成 YAML、能跨进程传递、能被静态画图，代价是加载时必须能 import 到那个名字，重命名或移动函数会让旧声明失效。

**源码事实：** 定义级错误分两档：结构性错误（判别字段非法、方法名不合法、自监听、CEL 根越界）在构造时抛异常；语义性疑点（emit 无 router、default_outcome 不匹配）只写日志。这条分界线画在「能否被确定地判定为非法」，而不是「严重程度」。

**源码事实：** `FlowConfigDefinition.max_method_calls` 默认 100 且进入契约，说明无限循环保护是**可导出、可复现**的配置，而不是运行时开关。同一份声明在别处重放时，同一个阈值也跟着走。

**源码事实：** `FlowPersistenceDefinition` 与 `FlowHumanFeedbackDefinition` 的 docstring 都专门声明了「活对象/序列化形态」的双重身份。这是刻意的妥协：为了让内存中不必做一次序列化-反序列化往返，同时又要保证 dump 不炸。

**架构推论：** 把「状态」做成按 `type` 判别的联合而非继承层次，使新增状态形态（例如从 JSON Schema 建模型）无需改动消费方——消费方只需要按 `type` 分派。代价是每种形态都得自己带够建模型所需的信息，`unknown` 就是信息不足时的显式降级位。

**架构推论：** 定义只描述「方法做什么」而不描述「方法执行后状态怎么变」。状态更新靠方法体自己去写 `self.state`。这让契约保持稳定（方法逻辑变了定义不用变），但也意味着静态分析无法预知状态演进。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/flow/flow_definition.py` | `FlowDefinition` | 契约顶层：schema 版本、状态、配置、方法表、加载与导出、三项校验 |
| `lib/crewai/src/crewai/flow/flow_definition.py` | `FlowMethodDefinition` | 单个方法的八槽位定义、`is_start` 归一、人工反馈改写为路由 |
| `lib/crewai/src/crewai/flow/flow_definition.py` | `FlowConfigDefinition` | 可序列化的执行配置，含无限循环保护阈值 |
| `lib/crewai/src/crewai/flow/flow_definition.py` | `FlowStateDefinition` | 按 `type` 判别的四态状态契约联合 |
| `lib/crewai/src/crewai/flow/flow_definition.py` | `FlowActionDefinition` | 按 `call` 判别的七动作联合 |
| `lib/crewai/src/crewai/flow/flow_definition.py` | `log_flow_definition_issues` | 语义性疑点的告警出口（不抛异常） |

## 参考

- [CrewAI 源码：flow_definition.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/flow/flow_definition.py)
- [CrewAI 文档：Flows 概念](https://docs.crewai.com/en/concepts/flows)
- [Pydantic 文档：判别联合](https://docs.pydantic.dev/latest/concepts/unions/#discriminated-unions)

## 与主核 / AI 的关系

这份契约与主核里的「系统目录 + 执行计划」是同一思路的两种落点：主核把表结构、索引定义、约束都存成目录里的数据，执行器读目录而不是读源码；这里把流程结构存成定义数据，引擎读定义而不是读类。差别在严格程度——主核的目录与磁盘结构强绑定，改不了；这里允许「定义声明的方法类里没有」，那种情况在绑定期才报错（见 d03 的 `_class_bound_methods`）。

CEL 根白名单的校验方式特别值得对照：它在**编译期**收集表达式里出现的根标识符，减去宏局部绑定（`all` / `exists` / `filter` / `map` 的接收者变量），再与允许集合比对。这与 SQL 里「校验列引用是否存在」是同构的，但只做到根一级——`state.foo` 的 `foo` 是不是真的字段，要等运行期取值才知道。这是刻意留的松紧带：状态可能是 dict，静态不可枚举。

对 AI 工程的实际意义在于：一旦编排是数据，「换一种表达方式运行同一份编排」就变成可能——同一份定义可以有 Python 实现、可以有脚本实现、可以只用来画图、可以用来做静态检查。这套折中的代价都集中在两处：活对象不得不随身带一个可序列化投影，以及所有语义检查必须在「确定非法」与「可能不对劲」之间划一条保守的线。
