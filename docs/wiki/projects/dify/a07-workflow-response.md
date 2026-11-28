---
project: dify
feature: workflow-response
title: 工作流响应转换
page_type: feature
schema_version: 1
refs:
  - path: api/core/app/apps/common/workflow_response_converter.py
    symbols: [WorkflowResponseConverter]
---

# 工作流响应转换

## 一句话职责

把图引擎产生的**队列事件**翻译成**对外的流响应对象**：`WorkflowResponseConverter` 是唯一持有「这次运行的状态」的转换器，它把开始、节点起止、迭代/循环、暂停、结束这些事件逐个变成 `WorkflowStreamResponse`，并负责截断变量、编码特殊类型、收集文件。

## 机制是什么

这一层的位置很特殊：上游是图引擎的 `AppQueueEvent`（内部事件），下游是 `StreamResponse`（对外协议）。两者不是一对一：

| 上游事件 | 转换产出 |
|---|---|
| `QueueWorkflowStartedEvent` | 单个 `WorkflowStartStreamResponse` |
| `QueueNodeStartedEvent` / `QueueNodeFinishedEvent` | 单个节点响应，但需要查回开始时的快照 |
| `QueueWorkflowPausedEvent` | **一个列表**（可能多个表单响应） |
| 各类事件 | 都要经过「截断 + 编码」两步处理 |

**这个转换器是有状态的，而它的状态正是「事件的配对关系」。** 节点完成事件本身不带标题与序号，这些信息在节点开始事件里。所以转换器必须自己维护 `_node_snapshots` 字典，按 `node_execution_id` 把开始时的信息存下来，等完成事件到来时取回。

这就是为什么它是 `WorkflowResponseConverter` 的**实例方法**而不是 `@classmethod`——同为转换层，`AppGenerateResponseConverter`（a02 那页）是无状态的类方法集合，而这一层必须持有跨事件的状态。

## 怎么实现的

### 构造期：三个决定

`__init__` 做三件事，每件都有明确理由。

**一，把系统变量平铺进输入。** `_prepare_workflow_inputs` 从 `application_generate_entity.inputs` 拷一份，然后把每个系统变量以 `sys.<字段名>` 的形式塞进去，**但跳过 `CONVERSATION_ID`**。两处注释说明了原因：

- 平铺本身是妥协：`TODO(@future-refactor): store system variables separately from user inputs so we don't need to flatten sys.* entries into the input payload just for rerun/export tooling.` 也就是为了让「重跑/导出」工具能拿到完整的输入，不得不把系统变量混进用户输入。
- 跳过会话 ID 是为了让输入可复用：「Conversation IDs are session-scoped; omitting them keeps workflow inputs reusable without pinning new runs to a prior conversation.」

最后还要过一遍 `WorkflowEntry.handle_special_values(inputs)`——把特殊值（文件、密钥之类）归一。

**二，按调用来源决定是否截断。** `SERVICE_API` 用 `DummyVariableTruncator()`（不截断），其他用 `VariableTruncator.default()`。注释写明理由是「保持向后兼容」——**API 调用方早已习惯了完整返回，改截断会破坏他们的解析**。而网页端可以截断，因为前端会按需再取。

这是一个很典型的兼容性判据：**同一条输出契约，老消费者的行为被冻结，新消费者接受更严格的处理**。

**三，初始化状态容器。** `_node_snapshots`（节点快照）、`_workflow_execution_id`（运行 ID，惰性记忆）、`_workflow_started_at`（开始时间，用于算耗时）。

### 状态的三个锚点

`_ensure_workflow_run_id(workflow_run_id=None)` 是这个类里最小的一个函数，但它承担了「惰性记忆」的职责：若传了值就记下，若没记过就抛 `ValueError("workflow_run_id missing before streaming workflow events")`。于是**所有后续转换方法都能无条件拿到 run_id，而不必各自传参**。

`_store_snapshot(event)` 有一个防止重复的细节：先 `_get_snapshot` 查一次，存在就返回。注释说明：「引擎可能已经持久化了这次开始，早于恢复元数据被加载时」。于是恢复场景下重放的开始事件不会覆盖已有快照。

`workflow_start_to_stream_response` 是唯一会**写两个状态**的地方：记 `_workflow_started_at = naive_utc_now()`、调 `workflow_execution_timing.start()`。而且在 `reason == RESUMPTION` 时把传入的 `node_execution_snapshots` 批量灌进 `_node_snapshots`——**恢复时必须靠调用方提供的快照重建状态**，因为恢复时的「开始事件」并不是每个节点真正的开始。

`workflow_finish_to_stream_response` 则依赖这两个状态：`started_at is None` 时直接抛 `ValueError("... called before workflow_start_to_stream_response")`。**没有开始就没有结束**，顺序错误当场暴露，不做兜底。

耗时算两遍：`elapsed_time = self._application_generate_entity.workflow_execution_timing.finish()` 取自共享的计时器，而 `finished_at - started_at` 由两个时间戳算。两个来源并存，说明「计时」这件事既在实体上有一份，也在转换器里有一份。

### 截断与编码：两道统一处理

`_truncate_mapping(mapping)` 是三段式：`None` 直接返回 `(None, False)`；空映射返回 `({}, False)`；否则先 `handle_special_values` 归一，再交给 `self._truncator.truncate_variable_mapping(...)`，返回「截断后的映射 + 是否被截断」的二元组。

**「是否截断」这个布尔值要一路带到响应里**，否则客户端不知道自己拿到的是完整值还是被砍过的值。这是 a01 那页 `PayloadTruncated` 同一原则的又一次出现：**有界化的输出必须自证有界**。

`_encode_outputs(outputs)` 极简：`WorkflowRuntimeTypeConverter().to_json_encodable(outputs)`。存在的理由是图输出里可能含非 JSON 原生类型（`Segment`、`File`、`datetime`、`Decimal`），不转一遍 `json.dumps` 会失败。

注意 `workflow_finish_to_stream_response` 里**没有**用 `_encode_outputs` 而是**内联**了一次 `WorkflowRuntimeTypeConverter().to_json_encodable(outputs_mapping)`——同一个动作两种调用方式，是演化留下的重复。

### 用户身份的两种投影

`workflow_finish_to_stream_response` 里用 `match user` 分两支构造 `created_by`：`Account` 填 `{id, name, email}`，`EndUser` 填 `{id, user: session_id}`。两个 `TypedDict`（`AccountCreatedByDict` / `EndUserCreatedByDict`）就是为这两个形状准备的。

**注意 `EndUser` 的键名是 `user` 而不是 `name`**——同一个语义位置（「这是谁」）在两个变体里用了不同的键名。这是对外协议的形状差异，不是笔误；但它意味着下游必须按变体分别解析。

### 文件收集：三种表示都要认

`fetch_files_from_node_outputs` → `_fetch_files_from_variable_value` 是这段逻辑的核心。它要处理五种输入形态，因为「工作流的文件输出」在不同节点里表示不同：

- `FileSegment` → 单个 `value.to_dict()`。
- `ArrayFileSegment` → 逐个 `to_dict()`。
- `File` → `to_dict()`。
- `list` → 逐项过 `_get_file_var_from_value`，取到的才收。
- `dict` → 过一次 `_get_file_var_from_value`。

`_get_file_var_from_value` 用一个**约定字段**识别文件字典：`value.get("dify_model_identity") == FILE_MODEL_IDENTITY`。也就是说「它是不是文件」不靠 `type` 判断，而靠字典里有没有这个身份标记。这是跨进程/跨语言传递对象时的常规手法——**用自描述字段代替类型系统**。

### 一个 `_HumanInputPauseOutput` 的用途

这个 `@dataclass` 只有两个字段：`response`（`HumanInputRequiredResponse`）与 `pause_reason`（字典）。它的 docstring 说得很直白：「Keep a form event paired with its entry in the workflow pause response.」——**暂停事件要产出两个东西，一个给客户端、一个给持久化的暂停原因，两者必须配对**。用一个只含两字段的类替代 `tuple`，是为了让「配对」这件事在类型上可见。

## 关键取舍

**源码事实：** 转换器有状态：持有节点快照映射、运行 ID、开始时间。节点完成事件必须靠开始事件存下的快照补齐标题与序号。

**源码事实：** 系统变量被平铺成 `sys.<名>` 混进用户输入（有 TODO 承认这是为工具链妥协），唯独 `CONVERSATION_ID` 被排除以保证输入可复用。

**源码事实：** `SERVICE_API` 调用不截断变量，其他来源截断；注释理由是向后兼容。

**源码事实：** `workflow_finish_to_stream_response` 在没有先调开始转换时抛 `ValueError`，不做兜底。

**源码事实：** 文件识别靠 `dify_model_identity == FILE_MODEL_IDENTITY` 的自描述字段，而不是类型判断。

**源码事实：** `created_by` 的两个变体键名不同（`name`/`email` 对 `user`）。

**架构推论：** 「有状态转换器」是这个层次唯一可行的形态，因为要对外输出比上游事件更丰富的信息（节点标题、序号、耗时）。这类状态的代价是**它无法被并发复用**——一个转换器实例只能服务一次运行。这也解释了为什么它是实例方法而不是类方法，与 `AppGenerateResponseConverter` 的类方法形成对照：**无状态可共享，有状态只能独占。**

**架构推论：** `sys.*` 平铺给「重跑」功能留了后门：把一份完整输入（含系统变量）存下来，将来原样重放就能复现同一结果。排除会话 ID 是这条路上的必要例外——否则重跑会强行挂进一个旧会话。

**架构推论：** 「API 不截断、网页截断」这种按来源分叉的输出契约，短期看是兼容性的最小代价，长期看会固化两套行为：任何新的输出处理规则都要问一次「API 要不要跟进」。**这是向后兼容的常见税。**

**架构推论：** 用 `dify_model_identity` 识别文件与 a05 那页「用白名单构造元数据而非采信模型声称」是同一思路的两面：前者为了识别，后者为了拒绝。共同的根因是**数据里带着自描述字段，而类型系统在边界外失效**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/app/apps/common/workflow_response_converter.py` | `WorkflowResponseConverter` | 事件到流响应的有状态转换器 |
| `api/core/app/apps/common/workflow_response_converter.py` | `_prepare_workflow_inputs` | 系统变量平铺与特殊值归一 |
| `api/core/app/apps/common/workflow_response_converter.py` | `_ensure_workflow_run_id` | 惰性记忆运行 ID，缺失即报错 |
| `api/core/app/apps/common/workflow_response_converter.py` | `_store_snapshot` | 节点快照去重存储，支持恢复重放 |
| `api/core/app/apps/common/workflow_response_converter.py` | `_merge_metadata` | 把迭代/循环 ID 合入元数据 |
| `api/core/app/apps/common/workflow_response_converter.py` | `_truncate_mapping` | 归一 + 截断，返回「是否截断」 |
| `api/core/app/apps/common/workflow_response_converter.py` | `_encode_outputs` | 图输出的 JSON 可编码化 |
| `api/core/app/apps/common/workflow_response_converter.py` | `workflow_start_to_stream_response` | 记录开始时间与恢复快照 |
| `api/core/app/apps/common/workflow_response_converter.py` | `workflow_finish_to_stream_response` | 产出结束响应，含耗时、用量、创建者 |
| `api/core/app/apps/common/workflow_response_converter.py` | `workflow_pause_to_stream_response` | 暂停事件产出一组响应 |
| `api/core/app/apps/common/workflow_response_converter.py` | `human_input_v2_required_data` | 表单数据投影成 HITL 线格式 |
| `api/core/app/apps/common/workflow_response_converter.py` | `_fetch_files_from_variable_value` | 五种表示的文件收集 |
| `api/core/app/apps/common/workflow_response_converter.py` | `_get_file_var_from_value` | 靠身份字段识别文件字典 |
| `api/core/app/apps/common/workflow_response_converter.py` | `handle_agent_log` | Agent 日志事件转流响应 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/app/apps/common/workflow_response_converter.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/common/workflow_response_converter.py)
- [dify 文档：流式响应与事件](https://docs.dify.ai/guides/workflow/publish-and-share)
- [Pydantic 文档：JSON 编码与自定义序列化](https://docs.pydantic.dev/latest/concepts/serialization/)

## 与主核 / AI 的关系

「把内部事件翻译成对外协议」与主核里「把执行器内部的行格式翻译成客户端可见的行版本/快照格式」是同一层职责。主核的做法是把可见性规则编进行头（`xmin`/`xmax`/命令 ID），转换成本近乎零；这里因为内部事件与对外形状差异很大（节点完成事件缺标题、缺序号），只能靠转换器额外缓存。**信息差决定了这一层要不要状态。**

`_truncate_mapping` 返回「是否截断」这一点值得与主核的「值是否被 TOAST 压缩」对照：一个是输出层的截断标记，一个是存储层的编码决策，都要让上层知道「你拿到的是原样的还是处理过的」。**任何有损处理都必须带标记，否则上层会把近似当精确。**

`dify_model_identity` 字段与主核里「行的类型标签」功能相同：在没有共同类型系统的边界上，用一个数据字段自证身份。数据库用 `pg_class.reltype`，这里用字典里的字符串常量——强度差很多，但解决的是同一个问题。

对 agent / RAG 工程的实际含义有四条。其一，用 `SERVICE_API` 拿到的变量**可能比网页端更长**，两端的解析逻辑不要共享。其二，节点完成响应里的标题与序号来自开始事件，若中间漏收开始事件（比如客户端重连），节点信息会缺失。其三，工作流输出的文件必须在字典里带 `dify_model_identity` 才会被收集，自己构造的「类文件」字典不会被识别。其四，暂停响应是一组而不是一个——多个节点同时等人填写时，客户端要把它们全部呈现，只取第一个会漏。
