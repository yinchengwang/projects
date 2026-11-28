---
project: dify
feature: llm-node
title: LLM 节点与模型环境变量
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/llm_node.py
    symbols: [DifyLLMNode]
  - path: api/core/workflow/llm_environment_variable.py
    symbols: [LLMEnvironmentVariable, LLMModelSelection]
  - path: api/core/workflow/llm_environment_variable.py
    symbols: [resolve_llm_model_config]
---

# LLM 节点与模型环境变量

## 一句话职责

LLM 节点本身在外部图引擎里，本仓只加了一个 47 行的子类用来挂「轮询收尾」钩子；真正属于 dify 的机制是**模型环境变量**：把「用哪个模型」从节点配置里抽出来变成工作流级变量，让多个 LLM 节点共用一个可切换的模型引用。

## 机制是什么

一个工作流里往往有七八个 LLM 节点。如果要把它从 A 模型换到 B 模型，逐个节点改是灾难。dify 的解法是把模型选择做成**工作流环境变量**，节点只存一个 selector。

这带来一个类型系统上的麻烦，而这个麻烦正是本组两个文件存在的原因。图引擎的变量体系里**没有「LLM」这种类型**——它只有字符串、数字、对象等。所以运行期这个值只能存成普通对象变量；但**持久化与 API 边界需要区分「这是个普通对象」还是「这是个模型选择」**，否则前端拿到一个对象不知道该渲染成什么表单。

dify 的解法是**双表示**：运行期借用 `ObjectVariable` 的形状，序列化时打一个 `value_type: "llm"` 的标记把语义补回来。模块 docstring 把这件事写得非常清楚：「Graphon does not have an LLM segment type, so the runtime representation stays an object variable. Workflow persistence and API boundaries use the semantic `llm` value type through the serialization helpers in this module.」

所以这一层要做的事，本质是**在类型系统不支持的地方用一个旁路标记维持语义**，而这个旁路标记必须在每个跨越边界的地方都被记得补上——序列化、值类型查询、DSL 导入导出、控制器渲染，一处漏了就退化成「一个看不懂的对象」。

第二个机制是**引用解析**。节点存的不是模型名，而是一个 selector（形如「环境变量节点 ID + 变量名」）。执行到该节点时，工厂拿 selector 去变量池取值、校验、把节点原有的模型配置按这个值覆盖一部分，得到最终模型。

覆盖是**部分覆盖**，这条边界很关键：环境变量提供 `provider`、`name`，以及可选的 `completion_params`；`mode` 不覆盖而是**校验相等**。源码写得直白——若变量里的 mode 与节点上的 mode 不一致，直接抛错。理由是 mode（对话/补全/嵌入）决定了调用协议，两者不同意味着这个引用本身是错的，不能靠覆盖修正。

## 怎么实现的

**`DifyLLMNode` 的全部内容是一个 `finally`。** 这个类只做三件事：把版本固定成 `"1"`；构造函数多收一个 `polling_finalizer` 回调；重写 `_invoke_llm_with_polling`，在 `yield from` 外层套 `try/finally`，无论正常结束、异常还是生成器被关闭，都调一次收尾器。

文件顶部有一行 TODO，说明它为什么存在：「Remove this Dify-specific node once graphon exposes a polling finalization hook.」——**这是一个明确的待搬迁补丁**：dify 需要一个生成器生命周期结束时的回调，而引擎当前没有这个扩展点，所以只能靠子类硬包。注意 `finally` 覆盖「生成器被提前关闭」这一点：调用方停止工作流时会 `close()` 生成器，此时 `finally` 仍会执行，收尾不会被跳过。

**`LLMModelSelection` 是一个冻结的严格模型。** 四个字段：`provider`、`name`（都要求非空字符串）、`mode`（枚举）、`completion_params`（可空字典）。配置上做了三件事：`extra="forbid"` 拒绝未知字段、`frozen=True` 不可变、`str_strip_whitespace=True` 自动去空白。所以一个多一个字段的旧 DSL 会在这里被拒——**严格校验同时也是一道兼容性闸门**。

**`LLMEnvironmentVariable` 靠一个 before 校验器完成形状转换。** 它继承 `ObjectVariable`，只在 `value` 字段上加 `field_validator(mode="before")`：进来什么先过 `LLMModelSelection` 校验，再 `model_dump(mode="json", exclude_none=True)` 转成字典。于是「非法值在构造期就被拒」这条被保证了，而存下来的仍是字典（因为运行期表示必须是对象）。

`exclude_none=True` 这个细节有实际后果：若没给 `completion_params`，序列化后的字典里**没有这个键**，而不是 `None`。下游若用 `.get("completion_params", {})` 与 `["completion_params"]` 两种取法会得到不同结果。

**两个序列化辅助函数成对维护语义标记。** `dump_environment_variable` 先普通序列化，若是 LLM 变量再补 `value_type`；`environment_variable_value_type` 则回答「这个变量的公开值类型是什么」，LLM 变量返回 `"llm"`，其余退回 `value_type.exposed_type()`。

这两个函数的存在说明**标记的补充是显式的、逐点的**。它不是基类行为，不是自动的；任何直接调 `variable.model_dump()` 的地方都会丢掉 `"llm"`，得到一个看起来像普通对象变量、行为却不同的东西。这类「必须记得调对函数」的设计，错误是静默的。

**selector 的判定分「宽进」与「严出」两步，而这两步的分工是这一层最微妙的地方。**

`should_resolve_llm_model_selector` 是宽进：空值返回假（不解析）；如果是个非字符串序列且首元素不是 `"env"`，也返回假——**这意味着旧图里的 `["start", "MODEL"]` 这类 selector 不会走新契约，而是保留节点的静态模型**。docstring 明说要让它们继续工作。

`parse_llm_model_selector` 是严出：必须是长度恰为 2 的序列、首元素必须是 `"env"`（即 `ENVIRONMENT_VARIABLE_NODE_ID`）、第二个元素必须是非空字符串，否则抛 `ValueError`。

两函数配合的效果是三层分派：**旧式 selector 静默跳过（继续用静态模型）、符合契约的走严格解析（格式错就报错）、其余（含首元素是 `env` 但长度不对的）走严格解析并失败**。这是一个「向后兼容 + 向前严格」的经典组合：对已知的历史形状放行，对任何新形状都不将就。

代码里那个布尔表达式值得逐字看一遍，它是否定形式套在合取上：`not (是序列 and 不是字符串 and 长度>0 and 首元素 != "env")`。等价于「只要不是『非 env 开头的合法序列』就返回真」。所以 `None` 之外的任何「首元素非 env 的序列」返回假、其余返回真。**这种写法能跑对，但读起来需要展开德摩根**——是一处可读性成本换行数的地方。

**`resolve_llm_model_config` 是覆盖逻辑的唯一实现。** 三步：校验变量值（失败时把变量名拼进错误消息，便于定位是哪个环境变量坏了）、校验 mode 相等、构造 `model_copy(update=...)`。

注意第三步的 `update` 字典是**条件性构建**的：`provider` 与 `name` 总是覆盖；`completion_params` **只在变量提供了才覆盖**。于是「变量没写 completion_params」意味着「沿用节点原有的温度、top_p 等参数」——这正是 docstring 说的「preserving legacy node-local parameters」。这个设计让环境变量可以只表达「换 provider 和模型名」，而把调参留在各节点上。

反过来，「变量写了 completion_params」是**整体替换**而不是逐键合并。如果一个节点原来设了 `temperature` 与 `max_tokens`，而变量只写了 `temperature`，那么 `max_tokens` 会消失。这是「整体覆盖」的语义，与「部分覆盖」易混淆，读代码时必须分清这两个层次。

**`resolve_llm_model_config_from_environment` 是给校验路径用的包装。** 它从「变量名 → 变量」的映射里取值，类型不对就报错说「未找到或不是 LLM 变量」（两种原因共用一个消息，因为对调用方来说没有区别），然后转调覆盖函数。

**`validate_llm_environment_model_references` 是发布前的静态校验。** 它遍历图里的节点、只看 `type == "llm"` 的、只看 selector 走新契约的，然后做一次完整解析——**解析失败就抛错**，于是坏引用在保存/发布时就被拦住，而不是等到执行。这解释了两套解析路径的必要性：执行期从运行时变量池取，校验期从持久化变量列表取，两者数据来源不同，但校验逻辑一致（都收敛到同一个覆盖函数）。

**工厂侧的接入点有两处，而且都绕过了版本解析。** `_resolve_node_class` 里有一行无条件分派：只要 `node_type == LLM` 就返回 `DifyLLMNode`，完全不看版本。所以**b02 讲的「版本精确匹配 → 退到 latest」对 LLM 节点不生效**——它只有一个实现。

`_resolve_llm_model_reference` 则在构造节点前把模型配置替换掉：取 selector、查变量池、要求取到的是 `ObjectSegment`（不是就报错）、调覆盖函数、`model_copy` 回节点数据。这里**校验的是 `ObjectSegment` 而不是 `LLMEnvironmentVariable`**——因为变量池里存的是运行期表示（对象段），语义类型已经在入库时校验过了。这是双表示带来的第二个后果：**不同边界上要检查不同的类型**，运行期查形状，持久化期查语义类型。

## 关键取舍

**源码事实：** `DifyLLMNode` 只做生命周期包装，用 `try/finally` 保证轮询收尾一定会跑，包括生成器被提前关闭的情形。

**源码事实：** 该类顶部有 TODO，说明它是等待上游（graphon）提供「轮询收尾钩子」期间的过渡实现。

**源码事实：** `DifyLLMNode.version()` 固定返回 `"1"`；`_resolve_node_class` 对 LLM 类型无条件返回该类，不参与版本匹配。

**源码事实：** `LLMModelSelection` 使用 `extra="forbid"`、`frozen=True`、`str_strip_whitespace=True`，`provider` 与 `name` 要求非空。

**源码事实：** `LLMEnvironmentVariable` 继承 `ObjectVariable`，在 `value` 的 before 校验里做严格校验后转成字典（`exclude_none=True`）。

**源码事实：** 语义标记 `value_type: "llm"` 只在 `dump_environment_variable` 与 `environment_variable_value_type` 两个辅助函数里补，不在基类自动生效。

**源码事实：** selector 判定分宽进与严出：非 `env` 开头的旧 selector 静默跳过并保留静态模型；`env` 开头的走严格解析，格式错即报错。

**源码事实：** 覆盖逻辑中 `provider` 与 `name` 总是覆盖，`completion_params` 只在变量提供时**整体替换**（不是逐键合并）；`mode` 不覆盖而要求相等。

**源码事实：** 发布期校验 `validate_llm_environment_model_references` 遍历图节点做一次完整解析，失败即抛错；执行期解析检查的是变量池里的 `ObjectSegment`，持久化期检查的是 `LLMEnvironmentVariable`。

**架构推论：** 「双表示 + 旁路标记」是在**类型系统不支持某语义**时的通用手法，代价是标记的补充必须逐点记得。它的失败模式是静默的：漏补标记的地方不会报错，只会让前端拿到一个无法识别的对象。**任何靠约定维持的语义都比靠类型维持的脆弱**——这条与 b02 里「版本号复用靠数据形状消歧」是同一类问题。

**架构推论：** 「mode 不覆盖而校验相等」是一条正确的保守选择。mode 决定调用协议，若允许覆盖，一个对话节点被指向嵌入模型时会得到一个语法上合法、语义上荒谬的执行。**对「结构性的选择」应当校验一致性，对「可替换的实现」才允许覆盖**——provider 与 name 是后者，mode 是前者。

**架构推论：** 宽进严出的 selector 判定，等价于**给历史数据开一条永久通道**。旧图里的 `["start", "MODEL"]` 会永远继续用静态模型，永不被引导到新契约。这是一次性的、不可回收的兼容成本；好处是升级时不破坏存量工作流。判据与 b02 的版本兜底相同：**如果这个兼容会不会改变已有数据的语义？** 这里不会（旧图行为不变），所以是安全的那一类。

**架构推论：** 版本解析被 LLM 节点绕过，说明**「节点注册表」不是唯一的类型解析入口**，还有硬编码的特例分支。这类特例的收益是可以给单个节点做定制（如挂收尾器），代价是类型解析的逻辑分散在两处，读 `node_factory` 时不能只看 `resolve_workflow_node_class`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/llm_node.py` | `DifyLLMNode` | LLM 节点子类，唯一职责是保证轮询收尾执行 |
| `api/core/workflow/llm_node.py` | `DifyLLMNode._invoke_llm_with_polling` | 在生成器外层套 `finally`，覆盖提前关闭 |
| `api/core/workflow/llm_environment_variable.py` | `LLMModelSelection` | 冻结的严格模型选择值对象 |
| `api/core/workflow/llm_environment_variable.py` | `LLMEnvironmentVariable` | 运行期借对象变量形状承载模型选择 |
| `api/core/workflow/llm_environment_variable.py` | `dump_environment_variable` | 序列化时补回 `llm` 语义标记 |
| `api/core/workflow/llm_environment_variable.py` | `environment_variable_value_type` | 回答变量的公开值类型 |
| `api/core/workflow/llm_environment_variable.py` | `should_resolve_llm_model_selector` | 宽进判定，放行旧式 selector |
| `api/core/workflow/llm_environment_variable.py` | `parse_llm_model_selector` | 严出解析，格式错即失败 |
| `api/core/workflow/llm_environment_variable.py` | `resolve_llm_model_config` | 部分覆盖模型配置，mode 校验相等 |
| `api/core/workflow/llm_environment_variable.py` | `validate_llm_environment_model_references` | 发布期静态校验整图引用 |
| `api/core/workflow/node_factory.py` | `DifyNodeFactory._resolve_node_class` | 对 LLM 类型硬编码分派，跳过版本匹配 |
| `api/core/workflow/node_factory.py` | `DifyNodeFactory._resolve_llm_model_reference` | 执行期从变量池解析模型引用 |
| `api/core/workflow/variable_prefixes.py` | `ENVIRONMENT_VARIABLE_NODE_ID` | 环境变量节点的固定 ID，selector 首段 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [graphon：dify 外置的图执行内核](https://github.com/langgenius/graphon)
- [dify 源码：core/workflow/llm_environment_variable.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/llm_environment_variable.py)
- [dify 文档：工作流环境变量](https://docs.dify.ai/guides/workflow/variables)

## 与主核 / AI 的关系

**「运行期借用一种表示、边界处补语义标记」与主核里「存储格式与逻辑类型的分离」同构。** 数据库把 `timestamp` 存成整数、把 `numeric` 存成变长字节，物理表示与逻辑类型不是一回事；补回来的方式是把类型信息记在目录里（`pg_attribute`），而不是靠值自己带标记。差别在于**目录是集中且强制的**，dify 的标记是分散且靠约定的。**集中登记的错误是「漏登记就查不到」，分散补标记的错误是「漏补就静默退化」**——后者更难发现。

**「结构性选择校验相等、可替换实现允许覆盖」与主核的算子绑定一致。** 一个算子绑定了参数类型就无法改（`=` 不能凭空变成 `+`），但可以换实现（换索引访问方法扫描同一张表）。dify 的 mode 是前者，provider/name 是后者。这条判据在两边都成立：**能替换的是实现，不能替换的是契约。**

**发布期静态校验 vs 执行期解析，与主核的两种校验时机对应。** 数据库在 `PREPARE`（解析+规划）阶段就发现错误，不必等执行；dify 的 `validate_llm_environment_model_references` 干的是同一件事。两边都刻意让同一份校验逻辑在「提前」与「执行」两个时机复用——数据库复用解析器，dify 复用 `resolve_llm_model_config`。**校验逻辑只写一份，是一致性的唯一保障**。

**「硬编码特例绕过类型解析」与主核的内建类型。** 数据库有内建类型（`int4`、`text`）不走用户定义的 `CREATE TYPE` 路径，而走专门的代码。dify 的 LLM 节点绕版本匹配同一个道理：**特例的存在说明这个类型需要通用机制提供不了的东西**。风险也相同——特例分散在通用逻辑之外，读通用逻辑的人会漏掉它。

对 agent 工程最实用的五条。其一：**换模型用环境变量而不是逐节点改**；`completion_params` 一旦在变量里出现就是整体替换，节点级调参会全部丢失。其二：**mode 必须与节点一致**，不一致会在解析时报错而不是静默降级，这是刻意设计，别绕过。其三：**语义标记靠辅助函数补**，自己调 `model_dump` 导出 LLM 变量会丢掉 `"llm"` 类型。其四：**旧式 selector（首段不是 `env`）会一直用静态模型**，迁移时不会报错也不会提示。其五：**LLM 节点不参与版本解析**，所以给它配 `version=2` 之类不会生效，也不会有任何提示。
