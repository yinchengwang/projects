---
project: dify
feature: human-input
title: 人工介入：两代实现与表单编译
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/nodes/human_input/callback.py
    symbols: [DifyHITLCallback]
  - path: api/core/workflow/nodes/human_input/entities.py
    symbols: [ParagraphInputConfig, StringSource]
  - path: api/core/workflow/nodes/human_input/enums.py
    symbols: [HumanInputFormStatus, FormInputType]
  - path: api/core/workflow/nodes/human_input_v2/node.py
    symbols: [HumanInputNode]
---

# 人工介入：两代实现与表单编译

## 一句话职责

让工作流停下来等真人填表单并选一个按钮：v1 通过 `DifyHITLCallback` 把 dify 的语义桥接到引擎的 HITL 决策上，v2 改为自建节点 + 运行时端口，只用引擎的通用暂停契约；两代共享的是一套「表单定义即校验规则」的数据模型。

## 机制是什么

人工介入的核心难点不是「停下来」，而是**「停下来之后，工作流凭什么继续」**。b01 讲过引擎的暂停/恢复语义（节点从开头重跑、按序回放答复），这里要处理的是它上面那层：

**表单是一个有状态的实体。** 用户可能几小时后才填、可能永远不填、可能填了一半过期。所以需要一张表记录「这份表单现在什么状态」：等待、已提交、节点级超时、全局过期。这四个状态在 `HumanInputFormStatus` 里是一等公民，而且**前两个和后两个的处理方式完全不同**。

**超时有两层，语义必须分开。** 节点级超时（`TIMEOUT`）是「这个节点等够了，沿超时边继续跑」，工作流还能走下去；全局过期（`EXPIRED`）是「整个流程等太久了，不该再恢复」。代码里对这两者的处理截然不同：前者产出一个 `Expired` 决策并带上超时句柄，后者**抛 `AssertionError`**。docstring 直接把这条边界写成了设计原则：「only explicit node timeout resumes along the timeout handle, while global expiration is treated as an invalid resume state」。

**两代实现的差别在「谁理解表单」。** v1 走引擎的 `HITLCallback` 契约，回调函数返回 `PauseRequested` / `Completed` / `Expired` 三种决策之一——也就是**把 dify 的表单语义翻译成引擎能懂的决策**。v2 干脆不走这个契约，自己产出 `PauseRequestedEvent` + 自己的呈现事件，把「表单长什么样」留给自己。v2 的模块 docstring 说得很直白：「Dify form semantics do not pass through HITLCallback」。

## 怎么实现的

**v1 的回调是一个状态机。**

`DifyHITLCallback.__call__` 拿到 `HITLContext` 后第一件事是找表单：先按执行 ID 查（`_execution_id_getter`），拿不到就按节点 ID 查。找不到就**创建一份**并返回 `PauseRequested`（带一个签发的会话 ID）。这条路径对应「首次进入节点」。

找到了就走状态判断，顺序很重要：

1. 状态是 `WAITING` 且未提交 → 先检查全局期限（超了就抛断言，因为「等过的表单不该再恢复」），再检查节点期限（超了就调 `mark_timeout` 把状态改成 `TIMEOUT` 并重新归一状态）。
2. 状态是 `TIMEOUT` → 返回 `Expired`，句柄是 `TIMEOUT_HANDLE`（`__timeout`），输出里 action 两个字段置空、只保留渲染后的内容。
3. 状态是 `EXPIRED` → 抛断言。
4. 未提交（但状态是等待）→ 再返回一次 `PauseRequested`（重跑时的正常路径）。
5. 已提交 → 必须有 `selected_action_id`（否则断言失败），然后重建提交数据、重新渲染表单内容、拼上三个特殊输出，返回 `Completed`，句柄就是那个 action ID。

**表单内容的渲染分了两个阶段，这是这一层最实质的设计。**

暂停前渲染（`render_form_content_before_submission`）：把 `form_content` 里的模板变量用当前变量池替换。用的是 `convert_template` 后取 `.markdown`——也就是说，**变量值里的 Markdown 会被当作文档结构解释**，而不是转义成纯文本。

提交后渲染（`render_form_content_with_outputs`）：把 `{{#$output.字段名#}}` 这样的占位符换成用户提交的值。这里按输入类型分派渲染方式，规则很不寻常但都有理由：

- 文件输入 → 固定输出 `[file]`；
- 文件列表 → `[N files]`；
- 段落与下拉 → 直接 `str(value)`；
- 字典或列表（其它类型）→ JSON 序列化；
- 其余 → `str(value)`。

注释解释了为什么文件要打码：「so the final content stays readable and does not inline transport metadata」——文件段里带着传输方式、URL 之类的结构化信息，直接拼进 Markdown 会变成噪音。**这是「渲染给人看」而不是「渲染给机器」的取向。**

**占位符语法由一条正则固定。** `OUTPUT_VARIABLE_PATTERN` 匹配 `{{#$output.名字#}}`，名字必须以字母或下划线开头、最长 30 字符。`extract_output_variable_names` 按**源序**返回所有槽位名。

这个语法有三处硬约束值得记：名字长度上限 30（来自正则量词）、必须符合标识符规则、以及**占位符只能出现在 form_content 里**——因为提取函数只接受 `form_content` 一个参数。所以「表单里有哪些输出字段」这个事实，是从**文本内容里扫出来的**，而不是从 inputs 列表读的。

这是一个异步语义：inputs 定义了「怎么收集」，form_content 里的占位符定义了「怎么展示」。两者可以不一致——一个 input 没有对应占位符，它会被收集但不显示；一个占位符没有对应 input，它渲染成空串。

**输入类型是标签联合，但默认值不是。**

`FormInputConfig` 用 `Annotated[..., Field(discriminator="type")]` 做判别式联合，四个变体各有 `Literal` 标签：段落、下拉、单文件、文件列表。但**默认值配置 `StringSource` 是刻意不用判别式联合的**，源码注释写明了理由：UI 在 `VARIABLE` 与 `CONSTANT` 之间切换时要保留另一侧的值，所以两种形态的字段必须同时存在。这是一个**「UI 需求反向决定数据模型」的实证**——纯从数据角度看，判别式联合更干净，但那样切换就会丢值。

于是 `StringSource` 有一个业务校验：若 `type` 不是 `CONSTANT`，`selector` 长度必须至少是引擎规定的最小长度。也就是说「变量来源就必须给一个合法 selector」，而常量来源允许 selector 为空。

`StringListSource` 则没有这个校验——它的 `selector` 与 `value` 都只是默认空值，没有 model_validator。两者不一致：字符串来源校验了，字符串列表来源没校验。

**输入配置的四个变体各实现了两个抽象方法**，其中「解析默认值」这件事只有段落输入真的做了：`resolve_default_value` 在常量类型下返回 `None`（没有运行时依赖），在变量类型下从变量池取值。下拉、单文件、文件列表三个变体**一律返回 `None`**。

所以「默认值」实际只对段落输入生效。这不是遗漏而是取舍：下拉的默认值语义应该是「选中哪一项」，单文件的默认值应该是「预置哪个文件」——这两个都会引入额外的语义（选中项必须在选项列表里、预置文件必须符合类型限制），而代码选择了暂不支持。

**`must_resolve_action_value` 有一个名不副实的实现。** 方法名与 docstring 都说返回「workflow-facing value」，但实现返回的是 `action.title`——**同一个值 `find_action_text` 也返回**。所以「按钮的显示文本」与「按钮传给工作流的值」实际是同一个字段，`find_action_text` 与 `must_resolve_action_value` 是同义函数（差别只在后者找不到时抛错）。

这意味着**改按钮文案会同时改下游拿到的值**。如果下游用这个值做条件判断，改文案就断了。

**`UserActionConfig.id` 是双重身份。** 注释写明「It also serves as the identifiers of output handle」——它就是边的句柄名。所以它必须是合法标识符（正则校验），且长度不超过 20。两条限制都来自同一个事实：**这个 id 会出现在工作流的边定义里**。

**v2 节点把「呈现」与「暂停」拆成了两个事件。**

`_run` 先调 `human_input_runtime.prepare_form(...)`（一个运行时端口，出了投递错误就产出失败事件）。然后按 `form.status` 分四种情况：

`WAITING` → 先产出一个 `NodeRunHumanInputV2FormRequiredEvent`（带完整 `prepared_form`），再产出 `PauseRequestedEvent`（带 `HitlRequired`，只含恢复所需的引用）。注释解释了为什么拆成两个：「Keep the live Dify presentation separate from graphon's persisted pause reason, which carries only the reference needed to resume.」——**呈现信息可能很大、可能含敏感字段，不该进持久化的暂停原因里**。

`SUBMITTED` → 从 `resolved.actions` 里按 id 找到动作、渲染填充后的内容、产出表单已填事件、然后 `StreamCompletedEvent`，`edge_source_handle` 就是 action id。

`TIMEOUT` → 产出超时事件 + 成功结果，句柄硬编码 `"__timeout"`，输出三个特殊字段（action 置空、渲染内容用 `legacy_form_content`）。

`EXPIRED` → **不产出任何完成事件，直接调 `graph_execution.abort(...)`**。注释说明了这个选择的理由：「A node failure can take an error branch. Global expiration must stop the graph itself, without dispatching a completion event.」——**节点失败可以走错误分支，但全局过期必须停掉整个图**。这是与 v1「抛断言」的等价选择，只是手段从「抛错」变成了「显式中止」。

v2 的 `execution_type` 是 `BRANCH`（因为完成时要选边），并且提供 `get_default_config` 让画布新建节点时有一个「有消息模板、无收件人」的合法初值。

**变量依赖的提取在 v2 里做得比 v1 完整得多。** `_extract_variable_selector_to_variable_mapping` 扫四类来源：表单内容、消息模板的标题与正文、每个输入的 selector、以及动态邮箱收件人的 selector。返回的键形式是 `节点 ID.#selector#`——注意那对 `#` 号，是这套语法特有的分隔符。

docstring 有一句重要的边界声明：「extracting dependencies must not invoke either operation」——**提取依赖不能触发真正的解析与渲染**。这是一个纯函数性要求：这个函数会被单节点调试路径调用，而那时没有运行时资源。

## 关键取舍

**源码事实：** 节点级超时与全局过期的处理完全不同：前者产出超时句柄并继续，后者在 v1 抛 `AssertionError`、在 v2 调 `graph_execution.abort`。

**源码事实：** `HumanInputFormStatus` 四态中，`WAITING` 与 `TIMEOUT` 会继续执行流，`EXPIRED` 终止整个图。

**源码事实：** `StringSource` 刻意不用判别式联合，注释说明理由是 UI 在两种来源间切换时要保留双方字段；它在变量来源下校验 selector 最小长度，`StringListSource` 没有对应校验。

**源码事实：** 只有 `ParagraphInputConfig.resolve_default_value` 会真的解析默认值；下拉、单文件、文件列表一律返回 `None`。

**源码事实：** `must_resolve_action_value` 返回的是 `action.title`，与 `find_action_text` 同义；按钮文案与传给工作流的值是同一个字段。

**源码事实：** `UserActionConfig.id` 既是按钮标识也是边的句柄名，因此受标识符正则与 20 字符长度限制。

**源码事实：** 表单输出槽位由扫 `form_content` 里的 `{{#$output.名字#}}` 占位符得出，名字必须在 30 字符内；与 `inputs` 列表无强制一致性。

**源码事实：** 提交后渲染对文件与文件列表打码成 `[file]` / `[N files]`，注释说明是为了避免把传输元数据写进内容。

**源码事实：** v2 把呈现事件与暂停事件分开产出，注释说明暂停原因只携带恢复所需的引用。

**源码事实：** v2 的变量依赖提取覆盖表单内容、消息模板标题与正文、输入 selector、动态邮箱 selector，且明确要求不得触发解析或渲染。

**架构推论：** 「两层超时必须分开」是因为它们的**失败责任不同**。节点超时是这个节点的局部问题，可以交给工作流的设计者决定怎么办（走超时边）；全局过期是运行环境级别的放弃，任何工作流设计都不该有权继续。**把这两种「等太久」混成一个状态，会让「设计者可控」与「系统强制」的边界消失。**

**架构推论：** v2 抛弃 `HITLCallback` 而自建节点，说明**引擎的抽象不够用了**。回调契约只允许返回一个决策，而 v2 需要同时产出「呈现事件」与「暂停事件」，还需要在全局过期时主动中止整个图。这三个需求都超出了回调的能力，所以只能绕开。代价是两套实现并存，且两代的表单语义要各自维护。

**架构推论：** 「输出槽位从文本里扫」是一个把**展示与契约混在一起**的设计。它带来一个好处（作者写什么就显示什么）和一个坏处（字段名成了文本内容的一部分，改名要改文本）。更彻底的做法是让 inputs 自带展示顺序，但那要求作者维护两份一致的信息。**在「单一真相」与「作者便利」之间，这里选了后者。**

**架构推论：** `must_resolve_action_value` 返回标题这件事，表面是命名失误，实质是**没有把「显示」与「取值」分成两个字段**。一旦下游依赖这个值，文案就变成了接口。修法不是改函数名，而是给 `UserActionConfig` 加一个独立的 `value` 字段。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/nodes/human_input/callback.py` | `DifyHITLCallback` | v1 桥接：把表单状态翻译成引擎的 HITL 决策 |
| `api/core/workflow/nodes/human_input/callback.py` | `DifyHITLCallback.__call__` | 表单查找、状态分派、两层超时判定 |
| `api/core/workflow/nodes/human_input/callback.py` | `render_form_content_before_submission` | 暂停前用变量池渲染表单内容 |
| `api/core/workflow/nodes/human_input/callback.py` | `DifyHITLCallback.render_form_content_with_outputs` | 提交后替换输出占位符 |
| `api/core/workflow/nodes/human_input/callback.py` | `DifyHITLCallback._render_output_placeholder_value` | 按输入类型分派渲染，文件打码 |
| `api/core/workflow/nodes/human_input/callback.py` | `DifyHITLCallback._restore_submitted_data` | 把提交载荷重建为运行时值 |
| `api/core/workflow/nodes/human_input/entities.py` | `HumanInputNodeData` | 节点配置：表单内容、输入、按钮、超时 |
| `api/core/workflow/nodes/human_input/entities.py` | `ParagraphInputConfig` | 段落输入，唯一支持默认值解析的变体 |
| `api/core/workflow/nodes/human_input/entities.py` | `StringSource` | 默认值来源，刻意不判别但带条件校验 |
| `api/core/workflow/nodes/human_input/entities.py` | `UserActionConfig` | 按钮定义，id 同时是边句柄 |
| `api/core/workflow/nodes/human_input/entities.py` | `extract_output_variable_names` | 从表单内容扫出输出槽位 |
| `api/core/workflow/nodes/human_input/entities.py` | `validate_human_input_submission` | 提交校验：动作合法 + 必填齐全 |
| `api/core/workflow/nodes/human_input/enums.py` | `HumanInputFormStatus` | 四态枚举，前两态继续、`EXPIRED` 终止 |
| `api/core/workflow/nodes/human_input/enums.py` | `FormInputType` | 四种输入类型的判别标签 |
| `api/core/workflow/nodes/human_input/constants.py` | `TIMEOUT_HANDLE` | 超时边的固定句柄名 |
| `api/core/workflow/nodes/human_input_v2/node.py` | `HumanInputNode` | v2 节点：自建暂停与呈现事件 |
| `api/core/workflow/nodes/human_input_v2/node.py` | `HumanInputNode._run` | 四态分派，`EXPIRED` 直接中止图 |
| `api/core/workflow/nodes/human_input_v2/node.py` | `HumanInputNode._extract_variable_selector_to_variable_mapping` | 全量依赖提取，禁止触发渲染 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [graphon：dify 外置的图执行内核](https://github.com/langgenius/graphon)
- [dify 源码：nodes/human_input/callback.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/nodes/human_input/callback.py)
- [dify 文档：人工介入节点](https://docs.dify.ai/guides/workflow/node/human-input)

## 与主核 / AI 的关系

**「两层超时」与主核里的 `statement_timeout` 和空闲事务超时同构。** 数据库里语句超时杀掉当前查询但连接还在，空闲事务超时断开连接。两者都叫「超时」，责任范围完全不同。dify 的节点超时与全局过期是同一对照。**判据是「超时之后谁该负责」**：局部超时的责任在设计者，全局超时的责任在运行环境。

**「节点失败可走错误分支，全局过期必须停图」与主核的崩溃恢复策略一致。** 一条 SQL 报错可以重试或走补偿逻辑；数据库进程崩溃则整个会话结束。两者都按「错误的影响范围」分级处理。**能被局部处理的问题应该局部处理，不能的必须升级**——把全局过期也做成「走一条边」，等于允许工作流设计者无视系统级别的放弃。

**「呈现与暂停分离」与主核的参数化查询同源。** 数据库把 SQL 文本与参数分开传（`PREPARE` + `EXECUTE`），参数里可能是用户数据、可能有敏感字段，而计划缓存里只留结构。dify 把「给用户看的大块呈现」与「恢复所需的最小引用」分开，同一个考虑：**持久化的东西应该尽可能小且不含敏感内容**。

**「从文本扫字段」与主核里从 SQL 文本扫依赖同类。** 数据库解析 SQL 得到表与列的依赖，dify 解析表单内容得到输出槽位。差别是数据库有一套完整的语法与类型系统，dify 只有一条正则。**用正则代替解析器的代价是表达能力被悄悄限制**，而作者不知道自己撞到了限制。

对 agent 工程最实用的五条。其一：**节点超时与全局过期必须分开建模**，混成一个状态会让「系统强制终止」变成「工作流可选分支」。其二：**按钮的 id 必须是合法标识符**，它同时是边的句柄名，改名等于改拓扑。其三：**按钮文案就是传给下游的值**，不要靠文案做条件判断，或先给它加独立的 value 字段。其四：**输出槽位是从表单文本里扫的**，input 列表与占位符可以不一致，排查「字段没出现」先看两处是否对齐。其五：**v2 不走 HITL 回调**，读代码时不要把两代的暂停机制混为一谈。
