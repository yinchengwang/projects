---
project: metagpt
feature: ext-environment
title: 外部 API 环境与可读可写标注
page_type: feature
schema_version: 1
refs:
  - path: metagpt/environment/base_env.py
    symbols: [ExtEnv, get_all_available_apis, read_from_api]
  - path: metagpt/environment/base_env.py
    symbols: [EnvType, mark_as_readable, mark_as_writeable]
---

# 外部 API 环境与可读可写标注

## 一句话职责

`ExtEnv` 把「外部真实环境（安卓、Minecraft、狼人杀、小镇）」的每一个能力做成一个带标记的普通方法，用两个模块级注册表收集成「可读 / 可写」两份 API 清单，再按名字动态调用。

## 机制是什么

这套设计解决一个具体问题：外部环境有几十个操作（截图、点屏幕、按返回、读方块、投票……），如果每个都写成独立类或独立动作，注册与暴露会极其啰嗦。MetaGPT 的做法是**用装饰器给方法打标记**，方法本身还是普通方法。

两个标记函数：`mark_as_readable` 表示「从环境取信息」（观测侧），`mark_as_writeable` 表示「对环境产生副作用」（动作侧）。两者做法一致——以 `func.__name__` 为键，把 `get_function_schema(func)` 的结果存进对应的注册表，然后原样返回函数。`get_function_schema` 抽取的是参数注解、返回注解、docstring 以及函数对象本身。

于是「环境的 API 清单」不是一个显式列表，而是**模块导入的副作用**：只要 `android_ext_env` 被导入，它那十几个带标记的方法就自动进了全局注册表。

调用侧有两个入口：`read_from_api` 接受字符串或 `EnvAPIAbstract`，`write_thru_api` 接受消息或 `EnvAPIAbstract`。两者都做「按名字查函数 → 判断是否协程 → 传 self 与参数调用」这三步。

## 怎么实现的

**注册表是模块级全局的。** 两个注册表实例定义在模块顶层，所有 `ExtEnv` 子类共用同一份。这意味着 API 名字空间是全局的，而不是每个环境类一份。目前四个外部环境共 37 个标记方法，名字互不重复，所以没出问题；但两个环境若各有 `reset` 之类同名方法，后导入的会覆盖先导入的，且没有任何冲突检测。

**schema 视图与函数本体分离。** 注册表里每条记录含 `func` 本体，但 `get_all_available_apis(mode)` 返回的是 `get_apis()` 的结果——它逐条剔除 `func` 键，并把其余值 `str()` 化。所以对外的「可用 API 清单」是纯文本化的描述（参数注解、返回注解、docstring），可以直接塞进提示词或工具 schema，不包含可调用对象。

**两个入口的参数形态不对称。** `read_from_api` 只接受「字符串」或「单个 `EnvAPIAbstract`」：字符串形态只传 `self`，抽象形态传 `self` 加 `args`/`kwargs`。`write_thru_api` 的签名声明了四种（字符串虽不在注解里但实际未被处理、`Message`、单个 `EnvAPIAbstract`、`list[EnvAPIAbstract]`），实际只实现了两种：`Message` 直接转给 `publish_message`，`EnvAPIAbstract` 走注册表调用。**列表形态没有任何分支处理**，会一路走到函数末尾返回 `None`——这是一个静默无操作的分支。

注意 `write_thru_api` 的类型注解里有 `str` 之外的三种，但字符串不在其中；相反 `read_from_api` 支持字符串、`write_thru_api` 不支持。两个入口的输入域不一致，是这套 API 最容易被误用的地方。

**参数对象的两个坑。** `EnvAPIAbstract` 的 `args` 字段被声明成 `set`，默认值却写成 `{}`（空 dict）。Pydantic 不校验默认值，所以默认实例的 `args` 真的是个 dict，对它做 `*env_action.args` 迭代得到的是空序列——恰好等价于「无位置参数」，能跑通。但显式传一个 dict 会直接抛校验错误；显式传 list 或 tuple 会被转成 set。而调用处是 `*env_action.args`，也就是**从集合里展开位置参数**——集合无序，一旦需要传两个以上位置参数，顺序就是不确定的。所以这套接口实际上只适合「零个或一个位置参数」的调用，其余必须走 `kwargs`。

**协程判定。** 两个入口都用 `is_coroutine_func`（即 `inspect.iscoroutinefunction`）决定是 `await` 还是直接调用。所以同一个环境下同步方法与异步方法可以混写，不需要两套注册表。代价是「返回 awaitable 但本身不是协程函数」的包装器会被当成同步调用，得到一个未 await 的协程对象。

**`_check_api_exist` 是个空校验。** 它接收的是「查出来的函数」，判断 `if not rw_api: raise ValueError`。但注册表的 `get` 在键不存在时已经抛 `KeyError`，所以它的入参永远不会是假值；错误路径实际由 `KeyError` 承担，这条校验分支不可达。

**`EnvType` 是一份未接线的枚举。** 它枚举了 Android、Gym、Werewolf、Minecraft、StanfordTown 五个类型，但在整个代码库里除了自身定义之外没有任何引用——没有工厂函数按它构造环境，模块的 README 示例写的 `env.create(EnvType.ANDROID)` 在当前代码里并不存在。实际用法是直接 `AndroidEnv(...)`。所以这个枚举属于「计划中的工厂」留下的遗迹。

**两个 Gym 空间字段。** `ExtEnv` 声明了 `action_space` 与 `observation_space`，默认用 `default_factory=spaces.Space`（直接以基类作工厂），且都 `exclude=True` 不参与序列化。它们的实际作用只是让类型上像一个 Gym 环境；具体环境的观测/动作空间定义在自己的 `env_space` 模块里（如狼人杀定义了 `EnvAction`、`EnvActionType` 与 `get_observation_space()`）。

## 关键取舍

**源码事实：** API 注册靠装饰器的导入副作用，而不是显式注册表构建。好处是新增方法只需加一行装饰器；代价是「环境有哪些能力」取决于导入了哪些模块，静态阅读代码无法确定清单，必须运行期 `get_all_available_apis`。

**源码事实：** 注册表是全局的、按函数名索引，不区分所属类。这让不同环境的 API 共享一个名字空间，同名即覆盖，且没有检测。当前靠命名习惯（各环境方法名不同）避免冲突。

**源码事实：** 「可读 / 可写」的区分只体现在注册到哪个注册表，不参与任何权限检查。`read_from_api` 不会阻止调用写 API——它只查读表，查不到就抛 `KeyError`。所以读写分离是**清单层面**的分离，不是执行层面的隔离。

**架构推论：** 这套「标记 + 名字调用」等价于把对象的方法暴露成 RPC。它与 LangGraph 的工具/节点机制形成鲜明对照：那边要显式声明 schema 与写入通道，这边任何方法加个装饰器就成了可调用能力，schema 由 `inspect` 反射得到。代价是反射出来的 schema 精度取决于注解质量，且没有参数校验——传错类型直到方法内部才炸。

**架构推论：** `read_from_api` 返回原始结果而不做归一化（`write_thru_api` 同理）。所以外部环境的返回值形状完全由各环境决定，上层拿到的东西没有统一契约。这是「快速接入异构外部环境」的必然代价：统一封装会挡住那些形状古怪的真实 API。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/environment/base_env.py` | `ExtEnv` | 外部环境基类，持有 Gym 空间占位与 API 调用入口 |
| `metagpt/environment/base_env.py` | `get_all_available_apis` | 返回读或写的 schema 清单（已剔除函数本体） |
| `metagpt/environment/base_env.py` | `read_from_api` | 按名字调用读 API，支持字符串与抽象参数两种形态 |
| `metagpt/environment/base_env.py` | `write_thru_api` | 按名字调用写 API，`Message` 转为消息投递 |
| `metagpt/environment/base_env.py` | `EnvType` | 五个外部环境类型枚举，当前无引用 |
| `metagpt/environment/base_env.py` | `mark_as_readable` | 把方法登记进读注册表 |
| `metagpt/environment/base_env.py` | `mark_as_writeable` | 把方法登记进写注册表 |
| `metagpt/environment/api/env_api.py` | `EnvAPIAbstract` | API 调用参数对象：名字、位置参数、关键字参数 |
| `metagpt/utils/common.py` | `get_function_schema` | 用 `inspect` 反射出参数/返回注解与文档 |

## 参考

- [MetaGPT 源码：environment/base_env.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/environment/base_env.py)
- [MetaGPT 源码：environment/android/android_ext_env.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/environment/android/android_ext_env.py)
- [MetaGPT 文档：环境与外部系统集成](https://docs.deepwisdom.ai/main/zh/guide/tutorials/agent_about.html)

## 与主核 / AI 的关系

「用装饰器把方法自动登记成可由名字调用的能力」与主核里把算子注册进算子表、再按名字从执行计划里调用同构：都是把静态代码变成运行期可枚举的清单。差别在于主核的算子表要处理类型与代价估计，这里的清单只是反射结果，不做校验也不做规划。

对 AI 工程而言，三条结论最实用。第一，`EnvType` 与 README 里的工厂示例是过期文档，实际接入外部环境要直接实例化具体环境类（如 `AndroidEnv`）。第二，位置参数经由 `set` 展开，顺序不可靠，需要多个位置参数时必须改用 `kwargs`。第三，`write_thru_api` 声明支持 `list[EnvAPIAbstract]` 但实现里没有对应分支，传列表会静默返回 `None`——批量写操作必须自己循环调用。对照 autogen 的工具注册与 crewai 的工具调用，这里的最大区别是「能力清单来自反射而非显式 schema」，因此可靠性取决于方法注解与 docstring 的书写质量。
