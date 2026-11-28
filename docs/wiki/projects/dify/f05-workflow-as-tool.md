---
project: dify
feature: workflow-as-tool
title: 工作流即工具
page_type: feature
schema_version: 1
refs:
  - path: api/core/tools/workflow_as_tool/tool.py
    symbols: [WorkflowTool]
  - path: api/core/workflow/workflow_tool_node.py
    symbols: [DifyWorkflowToolNode]
  - path: api/core/workflow/workflow_tool_container_handler.py
    symbols: [WorkflowToolContainerHandler]
---

# 工作流即工具

## 一句话职责

让一条工作流被当成一个工具调用：对外它实现 `Tool` 接口、有参数 schema、返回文本与文件；对内它需要跑一整张子图——而**这张子图有两种执行方式**，一种是把工作流当黑盒跑到底（`WorkflowTool`），另一种是交给引擎作为「容器帧」调度（`DifyWorkflowToolNode` + `WorkflowToolContainerHandler`）。

## 机制是什么

「工作流即工具」有两层含义，容易被混为一谈：

| 视角 | 载体 | 谁在调它 | 执行方式 |
|---|---|---|---|
| **工具侧** | `WorkflowTool` | Agent、工具节点、插件反向调用 | 起一个 `WorkflowAppGenerator`，`streaming=False` 跑完拿结果 |
| **节点侧** | `DifyWorkflowToolNode` | 引擎调度器 | 声明 `execution_type = CONTAINER`，交还一个 `CustomContainerRequest` |
| **容器侧** | `WorkflowToolContainerHandler` | 引擎的容器处理器 | 建子帧、把子图排进调度器、等它跑完再 resume 父帧 |

三者不是替代关系，而是**同一种能力在不同宿主里的三种落地**。`WorkflowTool` 是「我只要结果」的黑盒用法，能跑但不能暂停、不能与父图共享调度；节点+容器那一套是「引擎自己管子图」，代价是必须接进 `graphon` 的帧机制。

差别最大的一处是**是否支持暂停**。`WorkflowTool._invoke` 里显式写了：

```
pause_state_config=None,
allow_human_input=False,
```

注释解释了原因：「workflow pausing mechanisms (such as HumanInput) are not supported within WorkflowTool execution context」。所以一旦子工作流里有「人工输入」节点、并且真的暂停了，`_invoke` 里那句判断就会抛错：

```
if data.get("status") == WorkflowExecutionStatus.PAUSED:
    raise ToolInvokeError("Paused Workflow Tool execution requires Engine-managed container execution.")
```

**这条错误消息本身就是设计文档**：它告诉调用方「你走错路径了，要用容器执行」。而容器路径确实支持暂停——`WorkflowToolContainerHandler.prepare_frame_event` 里专门处理 `NodeRunPauseRequestedEvent` + `HitlRequired`。

## 怎么实现的

### 工具侧：`WorkflowTool`

**它是 `Tool` 的子类，实现 `_invoke` 与 `tool_provider_type`，构造参数有六个「身份」字段。**

`workflow_app_id`、`workflow_id`、`workflow_as_tool_id`、`version`、`workflow_call_depth`、`execution_driver`——前四个定位「跑哪个工作流的哪个版本」，第五个是递归深度，第六个是执行驱动器。**`execution_driver` 必须在构造时给**，`_invoke` 里 `if self.execution_driver is None: raise ToolInvokeError("Workflow execution driver is required")`，注释写在 docstring 之外很直白。

`_invoke` 的流程七步：

1. `_get_app` / `_get_workflow` 从数据库取应用与工作流（`_get_workflow` 带 `version` 参数，说明工作流有版本概念）；
2. `_transform_args` 把工具参数变成「工作流输入 + 文件列表」；
3. `WorkflowAppGenerator(execution_driver=...)` 构造生成器；
4. `_resolve_user(user_id)` 解析出 `Account` 或 `EndUser`，解析不到抛 `ToolInvokeError("User not found")`；
5. 调 `generate(...)`，参数里 `call_depth=self.workflow_call_depth + 1`（**深度在这里递增，这是防递归的关键一行**）；
6. 检查 `data["status"]` 是否 `PAUSED`、`data["error"]` 是否非空；
7. 把 outputs 拆成消息：文件先 `create_file_message`，非保留键走 `create_variable_message`，最后一条 `create_text_message`（JSON 字符串）加一条**带 `suppress_output=True` 的** `create_json_message`。

第 7 步那个 `suppress_output=True` 值得单看：**JSON 消息同时被产出但被标记为「不显示」**，于是下游既能拿到结构化结果，又不会在界面上重复显示一遍。

**`_transform_args` 按参数类型三分支，每一支的处置都不同。**

- `SYSTEM_FILES`：值是一组文件映射，逐个 `build_file_from_stored_mapping` 还原成 `File`，再按 `transfer_method` 做成目标格式的字典——`TOOL_FILE` → `tool_file_id`、`LOCAL_FILE` → `upload_file_id`、`DATASOURCE_FILE` → `datasource_file_id`、`REMOTE_URL` → `url`。**这一个 `match` 把四种传输方式收敛成四种子键名**，下游按 `transfer_method` 决定读哪个键。整段包在 `try/except` 里并且**只记日志不抛出**——单个文件转换失败不阻断整个调用。
- `FILES`：可选且为空时**归一成 `[]`**，判空逻辑在 `_is_empty_files_parameter_value`：`None`、空字符串、或「全由 `None`/空串组成的列表」都算空。注释说明这是「Identify empty optional file-list placeholders before workflow input validation」——**在类型校验之前把占位符清掉**，否则一个空列表会触发「必须是文件数组」的校验错误。
- 其它：原样取 `tool_parameters.get(name)`。

**`_derive_usage_from_result` 是三层回退的用量提取。** 先看 `data["usage"]`；再看 `data["metadata"]["usage"]`；都没有就退回旧的扁平字段 `total_tokens`/`total_price`/`currency`，用 `contextlib.suppress(TypeError, ValueError)` 做 `int(str(...))` 的容错转换。**这是为新旧两种响应格式做兼容**——新格式给结构化的 usage，老格式给扁平总量。

**`fork_tool_runtime` 是深拷贝式的克隆**：新实例保留全部身份字段与 `execution_driver`，并额外复制 `_parent_trace_context`（用 `model_copy()`）与 `_trace_session_id`。这两个字段是**追踪上下文**，有专门的 setter/clear：

- `set_parent_trace_context(parent_workflow_run_id, parent_node_execution_id)`，docstring 写的是「Attach outer workflow trace context without exposing it as tool input」；
- `clear_parent_trace_context()` 的 docstring 是「Remove parent trace context before invoking this tool outside a nested workflow」。

**「不作为工具输入暴露」这条约束很重要**：追踪上下文如果走参数，会被模型看见、也会被参数校验当成业务参数。做成实例上的私有字段，等于给它开了一条「带外通道」。

**`prepare_container_inputs` 是给容器路径用的输入准备**：`{**tool_parameters, **runtime.runtime_parameters}` —— **运行时配置覆盖调用参数**（顺序决定优先级，后者的键赢），然后 `_transform_tool_parameters_type` 处理类型、再走 `_transform_args`。

### 节点侧：`DifyWorkflowToolNode`

**类声明只有两行关键信息**：`execution_type = NodeExecutionType.CONTAINER` 与 `version() -> "1"`。前者把它从「普通同步节点」变成「容器节点」——引擎不会直接等它跑完，而是期待它交回一个「我要开一个子帧」的请求。

`_run` 返回 `Generator[NodeEventPayload | ContainerAwaitRequest]`，做三件事：

1. `_get_tool_runtime`（失败就 `_build_failure_event` 并 `return`）；
2. 生成参数两份——`parameters` 与 `parameters_for_log`，**分别用于执行与日志**（日志版本可能脱敏）；
3. `build_workflow_tool_container_payload(...)` 造请求负载，`yield CustomContainerRequest(payload=payload.model_dump_json())`。

**`payload` 是 JSON 字符串**而不是对象，因为容器处理器要把它塞进 `CustomContainerFrameState` 持久化，之后靠 `model_validate_json` 反序列化回来。**跨帧传递用序列化形态，是「帧可能被落库/重放」的直接后果。**

`_run` 里有一个耐人寻味的判断：

```
variable_pool = None
if self.node_data.version != "1" or self.node_data.tool_node_version is not None:
    variable_pool = self.runtime_state.variable_pool
```

**只有「非 v1 节点」或「显式声明了 tool_node_version」才传变量池。** 这意味着老的 v1 节点用另一套参数取值路径（不走变量选择器），是一个版本兼容分支——和节点工厂里那个 `version="2"` 两义问题是同一类历史包袱。

`_resume_container_events` 是「子图跑完之后怎么办」：检查结果类型是 `ContainerExecutionResult`，转成 `NodeRunResult` 并补上 metadata（工具信息 + 用量），若成功且 `outputs["text"]` 是有内容的 `StringSegment` 就先发一个非最终的 `StreamChunkEvent`，再发一个空的最终块，最后 `StreamCompletedEvent`。**两个 chunk 的写法（内容块 + 空终止块）是流式协议的标准收尾。**

### 容器侧：`WorkflowToolContainerHandler`

**它实现 `ContainerHandler` 协议，六个方法覆盖「建帧 / 恢复帧 / 处理请求 / 准备事件 / 是否向外发 / 完成 / 记失败」。**

`handle_request` 是入口：

1. 校验请求是 `CustomContainerRequest`；
2. 从 `run_state.payload` 反序列化出 `WorkflowToolContainerPayload`；
3. **帧 id 用 `f"{invocation_id}:workflow-tool"`** —— 由调用 id 派生，保证一调用一帧、可重复推导；
4. `_create_frame(...)`；**失败时不抛**，而是构造一个 `ContainerExecutionResult(status=FAILED, ...)` 塞进 `enqueue_ready_task`，让父帧拿到失败结果继续走；
5. 成功后把子帧快照 `put_container_frame(CustomContainerFrameState(...))`，然后 `child_frame.scheduler.enqueue_node(child_frame.graph.root_node.id)` —— **子图从根节点开始排队，交给引擎调度**。

第 4 条是这个类的核心姿态：**建帧失败也要走正常的 resume 通道**，而不是向上抛异常。这样父节点收到的是一个「结构化失败」而不是一个「异常中断」，调用方能继续处理（比如在画布上显示失败原因）。

`prepare_frame_event` 里有两处很细的处理：

- **失败事件计数回退**：`frame.state.graph_execution.exceptions_count -= 1`，注释是「Graphon increments immediately after container preparation; only the outer Tool failure belongs here」——因为引擎在容器准备阶段就先加了一次异常计数，若不回退，一次失败会被数两次；
- **人工输入的表单归属**：遇到 `NodeRunPauseRequestedEvent` 且原因是 `HitlRequired`，解析出 `form_id` 并把 `human_input_container_selector(form_id)` 映射到 `frame.container_id`，注释写「Keep source identity in Graphon; persist form ownership for Dify's response boundary」——**暂停时要把「这个表单属于哪个容器」记下来**，否则恢复时找不到入口。

还有一段把 `_HIDDEN_CHILD_EVENT_KEY` 写进 `event.node_run_result.process_data`：如果已经存在就沿用，否则新建一个含 `frame_id` 与 `container_metadata`（只挑 loop/iteration 四个键）的字典。**这是在事件穿过容器边界时给它贴上一个「我是谁的孩子」的标签**，否则外层容器会把子事件和自己的元数据搅在一起。

`should_emit` **恒返回 `False`**，但在返回前做两件副作用：`_save_workflow_tool_event`（按 `frame_id` 派发给注册的监听器，失败只记日志）与 `_hidden_event_listener`（若配置了）。**「子图的事件不往上层冒，但要落库」** 是这个 Handler 的核心语义——工作流工具对调用方是一个黑盒，但对可观测性系统不是。

`record_frame_failure` 把失败写进变量池的一个**带前缀的 selector**（`_FAILURE_SELECTOR_PREFIX`），注释是「Graphon custom frames have no metadata slot; scope the pool key to this invocation」——**容器帧没有 metadata 槽位，所以借用变量池存失败信息，键名按帧隔离**。这是一个典型的「用现有容器承载缺失的槽位」的绕道，必须在注释里说清，否则下一个人会以为变量池里为什么多了个怪键。

`complete_frame_if_ready` 收尾：若调度器判定执行完成，从变量池读回失败信息（并**校验形状**：必须是长度 2 的 list、`[str, str|None]`），然后构造 `ContainerExecutionResult`：

- 状态：`failure_variable is None` → `SUCCEEDED`，否则 `FAILED`；
- `steps` 来自 `frame.state.node_run_steps`（**子图消耗的步数透传给父图**，这是预算归集的关键）；
- `llm_usage` 来自 `frame.state.llm_usage`（**用量也透传**，否则成本统计会漏掉整个子工作流）；
- 成功时 outputs 走 `_build_tool_outputs`，失败时给 `{}`。

最后 `parent_frame.state.enqueue_ready_task(ResumeTask(invocation_id=..., result=result))` 唤醒父帧，再 `pop_container_frame`、`_frame_registry.remove`、`_event_listeners.pop` —— **三处清理必须都做，否则帧注册表会泄漏**。

`_build_variable_pool` 揭示了子图启动时的环境构造顺序：

1. 新建空池；
2. **先把父帧的所有系统变量搬进来**（`get_all_system_variables(parent_frame.state.variable_pool)`）；
3. 加 `source.environment_variables`；
4. 用 `file_factory.build_from_mappings` 还原 `payload.system_files`，其中 `strict_type_validation=(run_context.invoke_from == InvokeFrom.SERVICE_API)` —— **只有从服务 API 进来的调用才做严格类型校验**；
5. 覆盖三个系统变量：`APP_ID`、`WORKFLOW_ID`、`FILES`；
6. 从 `source.graph_config` 里找到根节点配置、`StartNodeData.model_validate` 解析、`_prepare_user_inputs` 处理用户输入（同样带那条 `SERVICE_API` 严格校验判断）；
7. `add_node_inputs_to_pool`，别名来自 `get_compatible_start_aliases(workflow_kind=..., root_node_id=...)`。

第 2 步与第 5 步的顺序要注意：**先继承父的所有系统变量，再覆盖三个**。所以子图能看见父的 `conversation_id`、`user_id` 之类，但 `APP_ID`/`WORKFLOW_ID`/`FILES` 被子图自己的值替换。这正是「子工作流是一个独立的图、但在同一个执行上下文里」的语义。

第 7 步的 `get_compatible_start_aliases` 说明**根节点的变量选择器有多种历史写法**，需要按 `workflow_kind` 与 `root_node_id` 生成兼容别名——与 `DifyWorkflowToolNode` 里那个 v1 判断同源。

## 关键取舍

**源码事实：** `WorkflowTool._invoke` 用 `WorkflowAppGenerator` 以 `streaming=False` 跑工作流，显式传 `pause_state_config=None`、`allow_human_input=False`；遇到 `PAUSED` 状态抛 `ToolInvokeError` 并提示要用容器执行。

**源码事实：** `call_depth=self.workflow_call_depth + 1` 是递归深度的唯一递增点。

**源码事实：** `_transform_args` 对 `SYSTEM_FILES` 按四种 `transfer_method` 生成四种不同的子键名，单个文件转换失败只记日志不抛；`FILES` 可选值为空时归一成 `[]`，判空在类型校验之前。

**源码事实：** `_derive_usage_from_result` 三层回退：`data.usage` → `data.metadata.usage` → 扁平 `total_tokens`/`total_price`/`currency`。

**源码事实：** 追踪上下文（`_parent_trace_context`、`_trace_session_id`）走实例私有字段与 setter，docstring 明确「without exposing it as tool input」。

**源码事实：** `DifyWorkflowToolNode.execution_type = NodeExecutionType.CONTAINER`，`_run` 以 `CustomContainerRequest(payload=payload.model_dump_json())` 交还控制权；只有非 v1 节点或声明了 `tool_node_version` 才传变量池。

**源码事实：** `WorkflowToolContainerHandler.handle_request` 在建帧失败时构造 `FAILED` 结果入队而不是抛异常；帧 id 为 `f"{invocation_id}:workflow-tool"`。

**源码事实：** `prepare_frame_event` 对直接子节点的 `NodeRunFailedEvent` 做 `exceptions_count -= 1` 回退；对 `HitlRequired` 暂停把 `form_id` 的 selector 映射到 `frame.container_id`。

**源码事实：** `should_emit` 恒返回 `False`，但先派发事件给监听器与落库。

**源码事实：** `record_frame_failure` 借变量池存失败信息（键带 `__workflow_tool_container__` 前缀），因为自定义帧没有 metadata 槽位。

**源码事实：** `complete_frame_if_ready` 把子图的 `node_run_steps` 与 `llm_usage` 透传给父帧，并做三处清理（容器帧、帧注册表、事件监听器）。

**架构推论：** 两条执行路径的存在不是重复实现，而是**能力的真实差异**：黑盒路径简单但不能暂停、不能共享调度预算；容器路径能暂停、能透传步数与用量，但必须接进引擎的帧机制。**当一个能力有两种「运行语境」，正确做法是给两条路径并让它们在边界上给出明确的错误提示**，而不是把黑盒路径改造成勉强支持暂停。

**架构推论：** 「建帧失败也走 resume 通道」是容器式设计的通用原则：**子执行的所有终局（成功/失败/超时）都应该以同一个数据形态回到父节点**。抛异常会让父节点同时要处理「正常结果」与「异常」两套逻辑，而容器边界恰恰是最不适合抛异常的地方（跨帧、可能已持久化）。

**架构推论：** 借变量池存失败信息、用带前缀的 selector 做命名空间，是「宿主容器缺少所需槽位」时的通用绕道。这类绕道的代价是**变量池的键空间被污染**，所以必须：前缀唯一、按帧隔离、并在注释里写明原因（这里做到了三点）。

**架构推论：** `exceptions_count -= 1` 这种「抵消上游记账」的修正，说明**两个组件对「什么算一次异常」的理解不一致**。这类修正极其脆弱：一旦上游改了计数时机，这里就静默错。更稳的做法是由上游提供「准备阶段不计数」的选项，而不是事后减一。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/tools/workflow_as_tool/tool.py` | `WorkflowTool` | 工具侧实现：跑黑盒工作流并产出消息 |
| `api/core/tools/workflow_as_tool/tool.py` | `WorkflowTool._invoke` | 取应用/工作流 → 转参数 → 跑生成器 → 拆输出 |
| `api/core/tools/workflow_as_tool/tool.py` | `WorkflowTool._transform_args` | 参数按类型三分支，文件按传输方式四分 |
| `api/core/tools/workflow_as_tool/tool.py` | `WorkflowTool._derive_usage_from_result` | 三层回退的用量提取 |
| `api/core/tools/workflow_as_tool/tool.py` | `WorkflowTool.prepare_container_inputs` | 容器路径的输入准备，运行时配置覆盖调用参数 |
| `api/core/tools/workflow_as_tool/tool.py` | `WorkflowTool.fork_tool_runtime` | 克隆工具实例并继承追踪上下文 |
| `api/core/tools/workflow_as_tool/tool.py` | `WorkflowTool.set_parent_trace_context` | 带外附加父追踪上下文（不作为工具输入） |
| `api/core/tools/workflow_as_tool/tool.py` | `WorkflowTool._is_empty_files_parameter_value` | 类型校验前清掉空文件占位符 |
| `api/core/workflow/workflow_tool_node.py` | `DifyWorkflowToolNode` | 容器型工具节点，交还 `CustomContainerRequest` |
| `api/core/workflow/workflow_tool_node.py` | `DifyWorkflowToolNode._run` | 取运行时、生成两份参数、构造容器负载 |
| `api/core/workflow/workflow_tool_node.py` | `DifyWorkflowToolNode._resume_container_events` | 子图完成后转发 chunk 与完成事件 |
| `api/core/workflow/workflow_tool_node.py` | `DifyWorkflowToolNode._build_failure_event` | 失败也以完成事件形式返回 |
| `api/core/workflow/workflow_tool_container_handler.py` | `WorkflowToolContainerHandler` | 容器处理器：建帧、暂停、失败、完成、清理 |
| `api/core/workflow/workflow_tool_container_handler.py` | `WorkflowToolContainerHandler.handle_request` | 建子帧并把子图根节点入队 |
| `api/core/workflow/workflow_tool_container_handler.py` | `WorkflowToolContainerHandler.prepare_frame_event` | 异常计数回退、人工输入归属、子事件打标 |
| `api/core/workflow/workflow_tool_container_handler.py` | `WorkflowToolContainerHandler.should_emit` | 恒不发事件，但落库与派发监听器 |
| `api/core/workflow/workflow_tool_container_handler.py` | `WorkflowToolContainerHandler.record_frame_failure` | 借变量池记录失败（帧级命名空间） |
| `api/core/workflow/workflow_tool_container_handler.py` | `WorkflowToolContainerHandler.complete_frame_if_ready` | 汇总步数与用量并 resume 父帧 |
| `api/core/workflow/workflow_tool_container_handler.py` | `WorkflowToolContainerHandler._build_variable_pool` | 子图启动环境：继承系统变量、覆盖三个、注入输入 |
| `api/core/workflow/workflow_tool_container_handler.py` | `WorkflowToolNestedContainerHandler` | 抑制嵌套内建容器里冒出的工作流工具事件 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/tools/workflow_as_tool/tool.py](https://github.com/langgenius/dify/blob/main/api/core/tools/workflow_as_tool/tool.py)
- [dify 源码：core/workflow/workflow_tool_container_handler.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/workflow_tool_container_handler.py)
- [dify 文档：工作流节点](https://docs.dify.ai/guides/workflow/node)

## 与主核 / AI 的关系

「工作流即工具」与主核里的**存储过程被当作表函数调用**是同一个结构：对外是一个普通的可调用对象（有参数、有返回、有 schema），对内是一整个执行计划。两者的关键问题也一致：**子执行的资源消耗、错误、中断如何回到父执行**。

**步数与用量的透传 vs 数据库的 SPI 连接记账。** PostgreSQL 里一个函数内部执行的 SQL 也会计入该会话的统计（`pg_stat_statements` 会分别记外层与内层）。dify 的 `complete_frame_if_ready` 把 `node_run_steps` 与 `llm_usage` 显式塞进返回结果——**子执行的消耗必须向上汇总，否则父级的预算与成本统计都是错的**。这一条在嵌套 Agent 系统里尤其容易漏。

**两种执行路径 vs 数据库的「普通函数」与「返回游标的函数」。** 前者一次调用跑到底，后者把控制权交还调用方由其逐行拉取、中间可以暂停。`WorkflowTool` 对应前者、容器路径对应后者。**语义上「能不能暂停」不由业务决定，而由执行模型决定**，所以用错路径只能报错而不能降级——这里那句 `Paused Workflow Tool execution requires Engine-managed container execution` 就是明确的边界声明。

**借变量池存失败 vs 数据库的 `SQLSTATE` 带外通道。** 自定义帧没有 metadata 槽位，于是借变量池的一个保留键存 `[error, error_type]`。数据库里也有类似的「借用」——`PL/pgSQL` 用 `GET STACKED DIAGNOSTICS` 暴露错误详情，而早期版本只能靠 `SQLERRM` 字符串。**缺少结构化槽位时，任何系统都会临时借用现有容器**，关键是借用要有唯一前缀与清理逻辑（这里三点都做到了）。

对 agent 工程四条结论。其一：**「子任务」有两种运行语义——黑盒跑完、或交还控制权**，两者不能靠一套代码硬撑；给两条路径并在边界明确报错，比让黑盒路径勉强支持暂停更可靠。其二：**子执行的步数与 token 必须向上透传**，否则父级预算与成本全都算少。其三：**容器边界的失败要以数据形态返回**，不要抛异常，否则父节点要维护两套逻辑。其四：**递归深度要在唯一一处递增**（这里是 `call_depth + 1`），散落在多个调用点必然有一处漏掉，表现为偶发的无限嵌套。
