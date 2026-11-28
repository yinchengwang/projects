---
project: dify
feature: chat-apps
title: 对话与补全形态
page_type: feature
schema_version: 1
refs:
  - path: api/core/app/apps/chat/app_runner.py
    symbols: [ChatAppRunner]
  - path: api/core/app/apps/completion/app_runner.py
    symbols: [CompletionAppRunner]
  - path: api/core/app/apps/advanced_chat/generate_task_pipeline.py
    symbols: [AdvancedChatAppGenerateTaskPipeline, WorkflowSnapshot]
---

# 对话与补全形态

## 一句话职责

三种「一问一答」形态的执行器：`ChatAppRunner` 与 `CompletionAppRunner` 是一条**单次模型调用**的直线流程（区别只在有没有会话记忆与标注回复），`AdvancedChatAppGenerateTaskPipeline` 则是把图执行产生的事件流折算成对话消息与阻塞响应的转换器。

## 机制是什么

三种形态的交互模型差别是**渐进的**，而不是三种无关的东西：

| 形态 | 上下文来源 | 执行体 | 输出 |
|---|---|---|---|
| Completion | 仅本次输入（+ 可选的检索上下文） | 一次 `invoke_llm` | 文本 |
| Chat | 本次输入 + 会话记忆窗口 | 一次 `invoke_llm` | 文本 + 消息落库 |
| Advanced Chat | 会话 + 图编排（可含检索、工具、分支） | 图执行 | 事件流折算成消息 |

Chat 相比 Completion 多的是**记忆与标注回复**：它多一个 `Conversation` 参数、会构造 `TokenBufferMemory`、会先查标注库。Advanced Chat 相比 Chat 多的是**把「一次模型调用」换成「跑一张图」**，因此它不再有 `run()`，而是一个 `pipeline`：把图事件流翻译成对话层的 `StreamResponse`。

**两个 runner 的代码有九成相同，但有一处实现路径不同**：`ChatAppRunner` 用 `ModelInstance(provider_model_bundle=..., model=...)` 直接拿已解析好的 bundle，而 `CompletionAppRunner` 用 `ModelManager.for_tenant(tenant_id).get_model_instance(...)` 再查一次。同一件事两种写法并存，说明这两个文件是分头演化的。

## 怎么实现的

### 两个 runner 的同构骨架

两者的 `run` 都声明了同一个契约（docstring 一字不差）：「**不要在模型 I/O 期间保留 `session`**，数据库准备工作已提交、连接已释放」。落到代码上是四段：

**第一段：读应用记录，用后就脱离。** 两个 runner 都新开一个 `create_session()` 读 `App`，读到后立刻 `read_session.expunge(app_record)`——**把对象从 session 里踢出来**，这样后续在使用它时不会因为 session 关闭而失效，也不会在别处被意外 flush。这是「读一份只读引用出来」的标准做法。

**第二段：先拼一次提示词，再做审核。** 注意顺序：`organize_prompt_messages(...)` **在** `moderation_for_inputs` **之前**。这不是随意排的——审核若拦截，`direct_output` 需要拿 `prompt_messages` 作为「模型看到的提示词」写进事件里，所以必须先在手上。

审核失败的处理两个 runner 完全一样：`except ModerationError as e` → `self.direct_output(..., text=str(e), stream=...)` → `return`。**审核拦截不是异常，而是「直接输出审核文案」的一次正常返回**。这与 a02 那页讲的 `ModerationRejectedHTTPError` 是两个层次：这里是把拒绝文案当回答推给用户，那里是在转换层把异常翻译成错误帧。

**第三段：检索与外部数据。** 顺序是「外部数据工具 → 数据集检索」，随后 `session.commit(); session.close()`。

数据集检索这一段的差异最能说明两种形态的语义差别：

- Completion 里有一段 Chat 没有的前置：`if dataset_config.retrieve_config.query_variable: query = inputs.get(...)`——**补全形态允许把检索查询指定为某个表单变量**，而不是用主 query。因为补全应用的主文本未必是问题。
- Chat 传 `memory=memory` 给检索，Completion 不传（它没有记忆）。
- 两者的 `vision_enabled` 都是从 `app_model_config_dict` 里穿透 `file_upload.image.enabled` 取的，同样的一段嵌套 `.get()` 链。

**第四段：再拼一次提示词，然后调模型。** 这是全流程最值得注意的设计：`organize_prompt_messages` **被调用了两次**。第一次不带 `context`/`context_files`，第二次带。原因是检索结果只有在运行期才知道，而第一次调用服务于「审核拦截时也要有提示词」。

于是 `recalc_llm_max_tokens` 只对**第二次**的结果生效——这才是真正要发给模型的提示词。顺序正确，但代价是提示词模板渲染了两遍（含一次上下文变量替换）。对带大量知识库上下文的场景，这个重复不便宜。

调用模型的差异同样在：

- Chat：`ModelInstance(provider_model_bundle=..., model=...)`，bundle 来自 `application_generate_entity.model_conf`。
- Completion：`ModelManager.for_tenant(tenant_id=...).get_model_instance(tenant_id=..., provider=..., model_type=ModelType.LLM, model=...)`。

两者都往 `request_metadata` 里塞 `app_id`、`app_type`（来自 `get_credit_usage_app_type`）、`created_by`（来自 `get_credit_usage_created_by`）——**计费归因靠这三个字段**，且必须由 runner 主动填，调用链上游并不代劳。

### Chat 独有的两段

**会话记忆**只在 `conversation_id` 非空时创建：先建 `ModelInstance`，再 `TokenBufferMemory(conversation=..., model_instance=...)`。这个 memory 只读不写（注释写着 `read-only`），因为写入由消息落库那侧负责。

**标注回复**是 Chat 独有的一条早退路径，而且它的顺序有着细微的正确性问题：它是**在第一次拼提示词之后、第二次之前**查的。命中后做三件事：

1. `queue_manager.publish(QueueAnnotationReplyEvent(...))` ——先告诉客户端「这条回复来自标注」。
2. `direct_output(..., text=annotation_reply.content, ...)` ——把标注内容当回答输出。
3. `return`。

`session.commit(); session.close()` 也在这一步显式做了——**因为查询标注会写 `MessageAnnotation` 的命中计数之类的记录**，而且这条路径要提前退出，不能指望后面的统一提交。

### Advanced Chat 的 pipeline

`AdvancedChatAppGenerateTaskPipeline` 的第一个要点在构造期：它接收的是三个**快照**而不是 ORM 对象。

`WorkflowSnapshot` / `ConversationSnapshot` / `MessageSnapshot` 都是 `@dataclass(frozen=True, slots=True)`，各有 `from_*` 类方法把 ORM 实例压成不可变值。`WorkflowSnapshot.from_workflow` 里还多一层 `dict(workflow.features_dict)`——**把可变字典复制一份**，否则快照仍与 ORM 对象共享那个 dict，`frozen` 只挡住属性赋值挡不住内容修改。

这套快照化的动机很实际：pipeline 会跑在一个可能很久的流程里，而 ORM 对象绑定在一个会被关掉的 session 上。**把它压成值，就彻底断开了与数据库会话的关系。**

构造期的其余部分：

- 用 `match user` 分 `EndUser` / `Account` 两支，取 `user_session_id`（`EndUser` 用 `session_id`，`Account` 用 `id`）与 `created_by_role`，其他类型抛 `NotImplementedError`。
- `build_system_variables(...)` 造系统变量，注意这里额外带了 `query`、`conversation_id`、`dialogue_count`——**对话形态的系统变量比工作流形态多三项**。
- 建 `WorkflowResponseConverter`、`WorkflowTaskState`、`MessageCycleManager`，最后 `_seed_graph_runtime_state_from_queue_manager()` 从队列管理器接过图运行时状态。
- `_seed_task_state_from_message`：**若消息状态是 `PAUSED` 且已有 answer，就把 answer 预置进任务状态**。这是让「暂停后恢复」能接着上次的答案继续累积，而不是从空串开始。

`process()` 只有三步：起一个「生成会话名」的线程、拿 `_wrapper_process_stream_response()` 得到事件生成器、按 `stream` 决定转成流式还是阻塞响应。**会话名生成是异步的**，它不阻塞主流程。

`_to_blocking_response` 是这个类的核心折算逻辑，用 `match` 处理四种流响应：

- `ErrorStreamResponse` → **`raise stream_response.err`**（把事件里的异常重新抛出）。
- `HumanInputRequiredResponse` → 收集进列表。
- `WorkflowPauseStreamResponse` → 立刻返回 `AdvancedChatPausedBlockingResponse`，把 `paused_nodes`、`reasons`、`status`、`elapsed_time`、`total_tokens`、`total_steps` 全带上。
- `MessageEndStreamResponse` → 返回 `ChatbotAppBlockingResponse`，`metadata` 只在非空时作为 `extras["metadata"]` 带上。
- 其他 → `continue`。

循环结束后若收集到了人工输入响应，就 `_build_paused_blocking_response_from_human_input`；否则 `raise ValueError("queue listening stopped unexpectedly.")`。

这个兜底很关键：**生成器正常耗尽但既没收到结束事件也没收到暂停事件，说明事件流被意外截断**。用异常而不是返回空响应，让上层知道「这次执行没有结论」。

`_build_paused_blocking_response_from_human_input` 里有一处去重：`paused_nodes = list(dict.fromkeys(...))`——用 dict 保序去重，因为多个 `HumanInputRequiredResponse` 可能来自同一个节点（节点内有多个表单字段）。`reasons` 则逐个用 `HumanInputRequiredPauseReasonPayload.from_response_data(...).model_dump(mode="json")` 转成 JSON 化字典。

## 关键取舍

**源码事实：** 两个 runner 都遵守「模型 I/O 期间不持 session」：读完 `App` 就 `expunge`，检索后 `commit` + `close`。

**源码事实：** `organize_prompt_messages` 在每个 runner 里被调用两次（审核前一次、检索后一次），`recalc_llm_max_tokens` 只作用于第二次。

**源码事实：** 审核拦截走 `direct_output` 并正常返回，不是抛异常。

**源码事实：** 标注回复只在 Chat 形态存在，且命中后会显式提交并关闭 session 再早退。

**源码事实：** Chat 用 `ModelInstance(provider_model_bundle=...)`，Completion 用 `ModelManager.for_tenant(...).get_model_instance(...)`，同一件事两种实现。

**源码事实：** Completion 独有「用某个表单变量当检索查询」的 `query_variable` 覆盖。

**源码事实：** pipeline 接收三个 `frozen` + `slots` 快照，`WorkflowSnapshot` 还复制了 `features_dict`。

**源码事实：** `_to_blocking_response` 里错误事件被重新抛出，生成器耗尽且无结论时抛 `ValueError("queue listening stopped unexpectedly.")`。

**架构推论：** 「二次拼提示词」是「审核需要提示词」与「检索结果晚到」这两个约束交叉出的妥协。要消除它，就得把审核挪到检索之后——但那时若被拦截，已经白跑了一次检索。**当前的顺序是「花一次模板渲染」换「不为被拒的请求跑检索」**，从成本看是对的。

**架构推论：** 快照化是「让长流程与 ORM 会话脱钩」的唯一干净做法。`frozen=True` 只保证属性不可赋值，`features_dict` 那种嵌套可变字段必须手动复制——**不可变数据类的边界只到第一层**，这是 Python 里很容易踩的一处。

**架构推论：** 两个 runner 的模型获取方式不一致，说明这一层没有强制的公共入口。风险是「租户校验」这类逻辑可能只在一个路径上生效：`ModelManager.for_tenant(tenant_id)` 显式绑了租户，而 `ModelInstance(provider_model_bundle=...)` 依赖 bundle 里已含租户信息。**同一语义两条实现，必然有一条的保证更弱。**

**架构推论：** 三种形态共用 `AppRunner` 的准备能力但不共用流程，这是本组设计的主线。好处是每条流程都能按自己的语义裁剪（Completion 不需要记忆、Chat 需要标注、Advanced 要图）；代价是**同一处修复要改三遍**，第 4 条「源码事实」里的模型获取分歧就是这种代价的实证。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/app/apps/chat/app_runner.py` | `ChatAppRunner` | 对话形态：记忆 + 标注回复 + 单次调用 |
| `api/core/app/apps/chat/app_runner.py` | `ChatAppRunner.run` | 四段骨架与两条早退路径 |
| `api/core/app/apps/completion/app_runner.py` | `CompletionAppRunner` | 补全形态：无记忆无标注 |
| `api/core/app/apps/completion/app_runner.py` | `CompletionAppRunner.run` | 同构骨架 + `query_variable` 覆盖 |
| `api/core/app/apps/advanced_chat/generate_task_pipeline.py` | `AdvancedChatAppGenerateTaskPipeline` | 图事件流折算成对话响应 |
| `api/core/app/apps/advanced_chat/generate_task_pipeline.py` | `WorkflowSnapshot` | 工作流的三字段不可变快照 |
| `api/core/app/apps/advanced_chat/generate_task_pipeline.py` | `ConversationSnapshot` | 会话快照 |
| `api/core/app/apps/advanced_chat/generate_task_pipeline.py` | `MessageSnapshot` | 消息快照 |
| `api/core/app/apps/advanced_chat/generate_task_pipeline.py` | `AdvancedChatAppGenerateTaskPipeline.process` | 起命名线程、取事件流、分流式/阻塞 |
| `api/core/app/apps/advanced_chat/generate_task_pipeline.py` | `_to_blocking_response` | 四种事件折算与兜底异常 |
| `api/core/app/apps/advanced_chat/generate_task_pipeline.py` | `_seed_task_state_from_message` | 暂停恢复时预置已有答案 |
| `api/core/app/apps/advanced_chat/generate_task_pipeline.py` | `_build_paused_blocking_response_from_human_input` | 保序去重的暂停节点列表 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/app/apps/chat/app_runner.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/chat/app_runner.py)
- [dify 源码：core/app/apps/completion/app_runner.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/completion/app_runner.py)
- [dify 文档：应用类型](https://docs.dify.ai/guides/application-orchestrate/creating-an-application)

## 与主核 / AI 的关系

「模型 I/O 期间不持数据库连接」与主核里「事务里不做网络 I/O」是同一条纪律，理由也相同：一个慢的外部调用会把稀缺资源（连接 / 锁）占住，进而拖垮整个实例。dify 用 `expunge` + `commit/close` 显式释放，主核用「把慢操作移出临界区」的结构约束。**两者都是把「资源持有时间」当一等公民来设计。**

快照化与主核的「行版本快照」形似而神不同：主核的快照是为了**读一致性**（事务看到某个时间点的数据），这里的快照是为了**解耦生命周期**（让对象在 session 关闭后仍可用）。前者必须与版本管理配合，后者只需要一次深拷贝。

「二次拼提示词」这个妥协在主核里有一个精确的对应物：**谓词下推前的代价估算**。数据库为了决定是否下推，会先做一次轻量估算再真正执行；如果估算后发现不该下推，那次估算就白做了。两处都在「先用一次便宜的计算换后面不做无用功」，都把重复计算当作可接受的成本。

对 agent / RAG 工程的实际含义有四条。其一，审核拦截是**正常返回**而非错误，客户端会把审核文案当作回答显示，这一点在实现前端时要区分。其二，标注回复会让后续流程完全不执行（不调模型、不检索），所以「为什么这条回答没有引用来源」的答案可能是「它来自标注」。其三，对话形态的记忆是只读窗口，写入由落库侧负责，不要在 runner 里期待它被更新。其四，高级对话的阻塞响应在「事件流意外截断」时抛 `ValueError`，这说明**收到了多少事件都不算完成，必须看到结束或暂停事件才算**。
