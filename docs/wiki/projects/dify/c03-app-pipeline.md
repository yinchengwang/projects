---
project: dify
feature: app-pipeline
title: 流水线应用与 DSL 校验
page_type: feature
schema_version: 1
refs:
  - path: api/core/app/apps/pipeline/pipeline_generator.py
    symbols: [PipelineGenerator, PipelineFileAdmission]
  - path: api/services/workflow/graph_check.py
    symbols: [GraphIssue, IssueCode, IssueSeverity]
  - path: api/services/workflow/graph_diff.py
    symbols: [WorkflowDiff, NodeChange]
---

# 流水线应用与 DSL 校验

## 一句话职责

`PipelineGenerator` 把「一批数据源 → 一批文档 → 一批后台执行」这条扇出链路串起来；`graph_check` 在真正跑之前把图里的问题逐条定位出来；`graph_diff` 则回答「草稿相对已发布版本改了什么」。

## 机制是什么

这一页把两件事放在一起，因为它们是同一个问题的两端：**流水线应用跑的是「一份工作流 × N 个输入项」**，所以「跑之前能不能确定这份图是好的」成了关键——一次扇出会创建 N 个文档、N 次执行，任一环节在运行期才炸都会留下半成品。

`graph_check` 的定位在模块 docstring 里一句话说完：「Problems in a workflow graph that would fail at import, in the editor or at run time, each located.」三个「失败时机」并列，说明它是**统一的问题聚合器**，而不是某一处的专用校验。

`graph_diff` 解决的是另一个具体问题：**怎么判断「草稿改了」而不会被编辑器噪音误报**。用户拖动一下节点、点选一下画布，`graph` JSON 就变了，但工作流行为没变。所以它要区分「布局变化」与「可运行内容变化」。

## 怎么实现的

**流水线生成器的整体形状是一次扇出。**

`generate` 收的东西比别的应用形态多：除了常规的 `session`/`pipeline`/`workflow`/`user`/`args`，还有 `invoke_from`、`call_depth`、`is_retry`。docstring 里的前置/后置条件写得很实在：「Input validation errors propagate before document or execution-log rows are added. Valid inputs are prepared once and shared by every datasource item. The caller owns committing any rows added to its session.」

第三句的措辞需要与代码对齐：**正式路径确实自己提交，而且提交发生在投递 Celery 之前**。具体条件是 `PUBLISHED_PIPELINE` 且非重试：先 `session.add` 文档行与执行日志行、`flush` 拿 ID，最后 `session.commit()`，然后才 `RagPipelineTaskProxy(...).delay()`。所以调试与重试路径完全不提交，而正式路径是「先落库、再派活」。这个顺序决定了失败时的可见性：Celery 任务没派出去时，文档行已经存在，用户能看到「有文档但没跑」。反过来若先派活再提交，worker 可能读不到还没提交的文档行。

文档行与执行日志行的**创建条件并不相同**：文档行还额外要求 `args` 里没有 `original_document_id`（重跑已有文档时不新建文档），而执行日志行没有这个限制。所以「重跑某文档」的情形是复用旧文档 ID、但新写一条执行日志。

流程的实际顺序是：

1. 取管线数据集（没有就抛 `ValueError`）。
2. 从 `args` 拆出 `inputs`、`start_node_id`、`datasource_type`、`datasource_info_list`。
3. `_format_datasource_info_list` 把输入项展开成实际条目——**在线网盘会被展开**，一个文件夹选择会递归枚举出全部文件。
4. 造一个批次号：`时间戳(秒) + 六位随机数`。注释没解释，但它显然是用来把同一批扇出产生的文档归组的。
5. 取管线配置，然后**只校验一次用户输入**（`_prepare_user_inputs`），注释说明「每次数据源项共享这个结果」。严格类型校验只在 `SERVICE_API` 调用时打开。
6. 只在「已发布 + 非重试 + 无原始文档」时创建文档行与执行日志行。
7. 逐项构造 `RagPipelineGenerateEntity`，然后分两条路：调试与重试**同步执行并立刻返回**；正式路径把实体攒进列表，最后交给 Celery 任务。

**第 7 步的分叉会造成一个真实行为差异。** `_generate` 是在循环内被 `return` 的，所以调试/重试模式**只处理第一个数据源项**，其余项静默丢弃。这在语义上是对的（调试时只想看一项），但它不是一个显式的门，而是一个藏在循环里的提前返回。读代码时很容易以为两条路都会处理全部。

在线网盘的递归守卫生效在这里。`_get_files_in_folder` 递归枚举文件夹，两道守卫都写在注释里：一是 `_visited_folder_ids` 记录已展开的前缀，防止「API 把文件夹当作自己的子项返回」这种自引用导致无限递归；二是**分页只在真的取到文件时才继续**（`is_truncated and has_files`）——注释说明了这个组合的必要性：「An empty folder that incorrectly reports `is_truncated=True` would otherwise recurse forever on the same empty page.」

第二道守卫值得单独记：单看 `is_truncated` 是 API 的承诺，但这个承诺可能是错的；单看 `has_files` 又会漏掉跨页的情况——某页恰好只含子文件夹、不含文件时 `has_files` 为假但确实需要继续翻页。两者相与是最保守的选择，代价是那种「整页都是文件夹」的多页场景会提前停止。

**文件准入是一道前置闸。**

`PipelineFileAdmission` 是一个只含 `__call__` 的协议，返回 `None` 表示放行，否则返回要记在文档上的拒绝原因。它的实现被注入，注释给的例子是「恶意软件扫描」。

调用点 `_scan_local_file` 的位置很讲究：它在 `_generate` 里、在建立队列管理器与工作线程**之前**。注释说明意图「before any node reads it」。命中拒绝时的动作是两步：**先把这个文档标成失败并写入原因**，再抛 `PipelineFileRejectedError`。所以「文档记录了为什么失败」这件事在抛错之前就完成了，不会因为上层捕获而丢失。

它只对本地文件类型生效，且解析不出上传文件 ID 时直接返回——**没有 ID 就没有可扫的东西**。

**文档名要被打磨，因为它的来源不可信。**

`_normalize_document_name` 的 docstring 说明了为什么需要它：「Datasource plugins control this value and can return anything: a crawler may put the whole page body into `title`.」

处理三步：非字符串或空白就退回 `"untitled"`、去首尾空白、截断到 255（对应数据库列长度）。名字来源按数据源类型分派，其中网站抓取有一处专门兜底：`title or source_url`，注释理由是「Crawlers may return an empty title」——**先退回 URL 再退回 `untitled`**，因为 URL 至少能让人认出是哪一页。

`_build_document` 还做了一件与内置元数据有关的事：若数据集开启了 `built_in_field_enabled`，就给文档填一整套内置字段（文档名、上传者、上传时间、更新时间、数据源类型），时间用同一个 `now`。**同一个时间戳给两个字段**，是有意的：新建文档的「上传时间」与「更新时间」此时确实相同。

**`context` 参数被同名变量遮蔽了。**

`_generate` 的签名收一个 `context: contextvars.Context`，进入后第一件事是 `preserve_flask_contexts(flask_app, context_vars=context)`。但建立工作线程之前有一行 `context = contextvars.copy_context()`——**重新赋值，覆盖了传入的那个**。

所以线程拿到的是「此处重新拷贝的一份」，而不是调用方精心准备的那一份。注释里没有说明这是有意的（对比同文件里另一处 `copy_context()` 没有任何说明）。这类遮蔽在类型上完全合法，行为上却把参数的作用抹掉了——调用方传什么都一样，除了第一处 `preserve_flask_contexts` 用到。

**后台执行的异常分类是五档。**

`_generate_worker` 里五条 `except`，语义各不相同：`GenerateTaskStoppedError` 静默吞掉（用户主动停止不是错误）；`InvokeAuthorizationError` 被包装成「Incorrect API key provided」再发出去；`ValidationError` 与 `ValueError` 记日志并发出（后者只在 `DEBUG` 配置下打堆栈）；其余一律「Unknown Error」。

两处细节值得记：鉴权错误的对外消息被**改写了**，不暴露原始错误内容；`ValueError` 的非调试模式下只记简单日志，因为这类错误多为用户输入问题，堆栈没有价值。

`_handle_response` 里还有一条针对性的错误识别：若 `ValueError` 的第一个参数恰好是 `"I/O operation on closed file."`，就转成 `GenerateTaskStoppedError`。**用字符串匹配识别「客户端已断开」**，这是没有专门异常类型时的常见权宜做法，代价是措辞一变就失效。

**`graph_check` 是分层的检查流水线，前置形状门控放在最前。**

`check_graph` 的结构很短：先跑 `_shape_issues`，**有结果就直接返回**——不再做任何后续检查。理由很清楚：后续每个检查都要索引 `nodes`、读 `data`，形状不对时这些操作本身会抛异常。

形状门控检查四件事：`nodes` 必须是列表；每个节点必须是映射、必须有非空字符串 `id`、`id` 必须唯一、`data` 必须是映射；`edges` 若存在必须是列表且每项是映射。**这层只回答「能不能安全地读」，不回答「读出来对不对」**。

通过之后建一个 `_Graph` 视图：把节点逐个读成 `_Node`（含填好编辑器默认值的 `data`、读它时发现的问题、以及它引用的变量）。这里跳过 `custom-note`——就是 b02 提过的画布便签，**它在 DSL 里占位但不参与执行**，所以校验也跳过它。

然后依次跑五个检查：模式兼容、节点数据、边、分支未连接、输出名重复、变量引用。最后若提供了 `resources` 回调，再逐节点问一次资源可用性。

**问题被建模成「码 + 位置」，严重度由码推导。**

`GraphIssue` 有四个字段：`code`、`message`、可选 `node_id`、以及 `loc`（一个元组路径，如 `("nodes", 节点ID, "data", ...)`）。

`loc` 的形状与 JSON Schema 的 `instancePath` 同构，目的是**让前端能在画布上精确高亮那个字段**，而不是只告诉你「某个节点有问题」。

`IssueSeverity` 不是字段而是**由码推导的属性**：`IssueCode.severity` 读一个白名单 `_WARNING_CODES`，不在里面的都是 `ERROR`。目前白名单只有两项：分支未连接与资源不可用。这个设计的后果是**新增一个码默认是错误级**——保守方向正确，因为把该拦的降级比把该提醒的升级更危险。

`refused_node_types` 按应用模式返回**被拒绝的节点类型**：对话型拒绝结束节点与全部触发器，工作流型拒绝答案节点。这两个集合的来源是产品语义——对话流的终点是答案节点，工作流型的终点是结束节点。

**`graph_diff` 的核心是「投影成可运行视图再比」。**

`_runnable` 把图投影成 `{节点ID: data, 边: 排序后的边描述}`，其中 `data` 经过两步处理：先 `fill_node_data` 补导入默认值，再剔除三个 UI 键（`selected`、`_connectedSourceHandleIds`、`_connectedTargetHandleIds`）。

补默认值的理由写在 `_data` 的 docstring 里：「so a default added only by an import is not a change」——若某一版代码给某个节点加了新的默认字段，老草稿导入后会自动带上它，而用户实际没改任何东西。不补就会把这批默认值误报成改动。

剔 UI 键的理由同样明确：那是编辑器状态，不属于行为。

`same_graph` 就是 `_runnable(a) == _runnable(b)`。**「布局与选择状态不算」这条判断被压缩成一次字典相等比较**，简洁且不会漏项——因为它比的是投影而不是原始 JSON。

边被表示成**字符串**：`"源 → 目标 (句柄)"`。这带来一个副作用：句柄变化会被算成「一条边删了、另一条边加了」，而不是「同一条边改了」。对「改了什么」这个问题来说，这是可接受的近似。

`diff_workflows` 的细节：对同 id 节点逐字段比较，把差异字段名排序后放进 `NodeChange.fields`；未发布时把整个草稿算作「全部新增」（`published=False`）；`features_changed` 在未发布时强制为假。

`draft_token` 是这套投影的另一个用途：算一个「什么东西变了就变」的指纹，注释说明布局与密钥值不参与。所以同一份逻辑可以有稳定的 token，用于判断「要不要重新导入」。

## 关键取舍

**源码事实：** 用户输入只校验一次并共享给全部数据源项；严格类型校验只在 `SERVICE_API` 调用时开启。

**源码事实：** 调试与重试路径在数据源循环内**提前返回**，只处理第一个数据源项；正式路径把全部项攒成实体列表交给 Celery。

**源码事实：** 在线网盘文件夹递归带两道防环：已访问前缀集合，以及「`is_truncated` 且真的取到文件」才继续翻页。

**源码事实：** 文件准入在建立队列与工作线程之前执行，拒绝时先把文档标成失败并写入原因，再抛 `PipelineFileRejectedError`。

**源码事实：** 文档名会被去空白并截断到 255，非字符串或空白退回 `untitled`；网站抓取的名字退回顺序是标题、来源 URL、`untitled`。

**源码事实：** `_generate` 在建立工作线程前用 `contextvars.copy_context()` 覆盖了同名参数 `context`。

**源码事实：** `_generate_worker` 把鉴权错误改写为「Incorrect API key provided」再发出；`_handle_response` 用字符串匹配 `"I/O operation on closed file."` 来识别客户端断开。

**源码事实：** `check_graph` 的形状门控失败时直接返回，不跑任何后续检查；`custom-note` 节点在校验时被跳过。

**源码事实：** `IssueCode.severity` 由白名单推导，白名单只含「分支未连接」与「资源不可用」两项，其余码默认是错误级。

**源码事实：** `graph_diff` 比较的是投影后的可运行视图：节点数据补了导入默认值、剔掉了三个 UI 键，边被表示成「源 → 目标 (句柄)」字符串。

**架构推论：** 「输入校验只做一次」是扇出结构的必然选择：N 个数据源项共享同一份用户输入，逐项校验既浪费又可能给出不一致的结论（若校验依赖随机性或外部状态）。**共享校验的前提是校验本身是纯的**，一旦它读数据库，就必须换成「逐项独立的校验」。

**架构推论：** 「形状门控先于语义检查」是所有分层校验器的正确结构。语义检查假定数据结构可用，形状检查负责建立这个前提。把两者混在一起，就会出现「为了报告一个缺字段错误而抛了另一个 TypeError」的混乱信息。**先确保能读，再判断读出来对不对**，这个顺序在 JSON 校验、SQL 解析、配置加载里都成立。

**架构推论：** 「严重度由码推导而非字段指定」是一个减少不一致的手法：如果严重度是独立字段，就可能出现「同一个码在两处被判成不同级别」。用白名单推导保证**一个码只有一个级别**，代价是新增码必须显式进白名单才能降级——而默认不降级是正确的方向。

**架构推论：** 「投影成可运行视图再比较」把「什么算变化」这件事**从隐式约定变成显式函数**。若直接比原始 JSON，那么每次编辑器改动一个 UI 字段都会产生假差异；若用一堆 `if` 逐字段排除，则新增字段时必然漏掉。投影的优势是：**只要定义了「可运行视图中有什么」，剩下的自然都不算变化**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator` | 流水线生成器：扇出文档、建执行实体、分派同步或异步 |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator.generate` | 主扇出链路，含调试/重试的提前返回 |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineFileAdmission` | 文件准入协议，返回拒绝原因或 `None` |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator._scan_local_file` | 准入检查，先标记文档失败再抛错 |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator._generate` | 建队列、起工作线程、汇流响应 |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator._generate_worker` | 后台执行与五档异常分类 |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator._build_document` | 构造未持久化文档并填内置元数据 |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator._normalize_document_name` | 名字兜底与截断 |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator._format_datasource_info_list` | 在线网盘展开为文件清单 |
| `api/core/app/apps/pipeline/pipeline_generator.py` | `PipelineGenerator._get_files_in_folder` | 递归枚举，双重防环 |
| `api/services/workflow/graph_check.py` | `GraphIssue` | 一条问题：码、消息、节点 ID、定位路径 |
| `api/services/workflow/graph_check.py` | `IssueCode` | 十一类问题码，严重度由白名单推导 |
| `api/services/workflow/graph_check.py` | `IssueSeverity` | 错误与警告两级 |
| `api/services/workflow/graph_check.py` | `check_graph` | 形状门控加五个语义检查加可选资源检查 |
| `api/services/workflow/graph_check.py` | `_shape_issues` | 结构与唯一性门控，失败即短路 |
| `api/services/workflow/graph_check.py` | `refused_node_types` | 按应用模式给出被拒绝的节点类型 |
| `api/services/workflow/graph_check.py` | `credential_check` | 把凭据校验包装成资源检查回调 |
| `api/services/workflow/graph_diff.py` | `WorkflowDiff` | 差异结果：增删改节点、增删边、特性与环境变量变化 |
| `api/services/workflow/graph_diff.py` | `NodeChange` | 单个节点的变化描述，含变化字段名 |
| `api/services/workflow/graph_diff.py` | `diff_workflows` | 逐节点逐字段比较，未发布时全算新增 |
| `api/services/workflow/graph_diff.py` | `_runnable` | 投影为可运行视图，剔除布局与选择状态 |
| `api/services/workflow/graph_diff.py` | `same_graph` | 可运行视图相等性比较 |
| `api/services/workflow/graph_diff.py` | `draft_token` | 内容指纹，布局与密钥值不参与 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：apps/pipeline/pipeline_generator.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/pipeline/pipeline_generator.py)
- [dify 源码：services/workflow/graph_check.py](https://github.com/langgenius/dify/blob/main/api/services/workflow/graph_check.py)
- [dify 文档：知识流水线](https://docs.dify.ai/guides/knowledge-base/knowledge-pipeline)

## 与主核 / AI 的关系

**「形状门控先于语义检查」与主核的解析阶段划分完全一致。** SQL 查询先做词法与语法分析（确认能构成一个表达式），再做语义分析（表是否存在、类型是否匹配）。dify 的 `_shape_issues` 就是语法层，五个语义检查是语义层。**把两层合成一层，错误信息就会互相污染**——用户看到一个类型错误，而真正的原因是括号不匹配。

**「严重度由码推导」与数据库的错误码分级同源。** SQLSTATE 的前两位决定类别，具体码决定细节；严重度不是每次调用时另填的。**让严重度成为码的属性而不是调用点的选择**，能避免同一个问题在不同路径上得到不同处理。

**「投影成可运行视图」与主核的计划比较同理。** 比较两个执行计划是否等价时，不能比原始文本（空白、别名不同），而要比规范化后的算子树。dify 的 `_runnable` 就是这种规范化：剔除不影响行为的字段、补齐会由导入补上的默认值。**判断「有没有变化」必须先定义「什么算同一个」。**

**「先标记失败再抛错」与主核的日志先行同源。** 数据库在崩溃恢复时依赖已落盘的日志；dify 在抛异常前先把失败原因写进文档行。两者都是**把「发生了什么」持久化在「控制流中断」之前**，这样上层无论怎么处理都不会丢失归因。

对 agent 工程最实用的五条。其一：**调试与重试路径只处理第一个数据源项**，是循环内提前返回，不是显式门控，验证多项扇出必须走正式路径。其二：**文件准入要在任何节点读文件之前做**，且拒绝时先把原因写进文档再抛错，否则上层捕获后就查不到原因。其三：**数据源返回的文档名必须当作不可信输入**，爬虫可能把整页正文塞进标题，截断与兜底缺一不可。其四：**图的校验要先过形状门控**，否则后续检查会在坏数据上抛 TypeError 而不是给出可读问题。其五：**判断「草稿有没有改」要比投影后的可运行视图**，直接比 JSON 会被编辑器状态反复误报。
