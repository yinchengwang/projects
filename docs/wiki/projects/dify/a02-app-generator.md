---
project: dify
feature: app-generator
title: 应用响应生成与流式转换
page_type: feature
schema_version: 1
refs:
  - path: api/core/app/apps/base_app_generator.py
    symbols: [BaseAppGenerator]
  - path: api/core/app/apps/base_app_generate_response_converter.py
    symbols: [AppGenerateResponseConverter]
---

# 应用响应生成与流式转换

## 一句话职责

`BaseAppGenerator` 是执行侧的**输入关口**（把用户提交的表单值校验、清洗、转成文件对象，并管住工作线程的生命周期）；`AppGenerateResponseConverter` 是输出侧的规则层（把「两种调用来源 × 两种传输形态」映射成四种响应形状，并把异常翻译成统一的错误码）。

## 机制是什么

生成器这两个基类解决的其实是**一对镜像问题**：

| 部件 | 方向 | 核心问题 |
|---|---|---|
| `BaseAppGenerator` | 入 | 用户交上来的东西能不能用、要不要转、谁来收尾 |
| `AppGenerateResponseConverter` | 出 | 同一份内部结果，给谁看该长什么样 |

**输出侧的二维矩阵是这个文件的主干。** 两个维度：

- **调用来源**：`DEBUGGER` / `SERVICE_API` / `OPENAPI` 归为「全量」一侧，其余（`EXPLORE`、`OPENAPI`… 之外的网页端来源）归为「简化」一侧。
- **传输形态**：`AppBlockingResponse`（一次性）或 `Generator[AppStreamResponse]`（流式）。

2 × 2 = 4 个抽象方法，每个具体 generator 必须全实现：

| | 阻塞 | 流式 |
|---|---|---|
| 全量 | `convert_blocking_full_response` | `convert_stream_full_response` |
| 简化 | `convert_blocking_simple_response` | `convert_stream_simple_response` |

**「全量 vs 简化」的差别是真实的字段裁剪，不是换皮。** 简化侧通过 `_get_simple_metadata` 做三件事：重建 `retriever_resources`（只保留白名单字段）、**删掉 `annotation_reply`**、**删掉 `usage`**。所以给最终用户的 SSE 流里既看不到命中缓冲（`hit_count`）这类内部指标，也看不到 token 消耗与标注回复的来源标记。

**入侧的校验是按变量类型分派的严格转换。** `_validate_inputs` 对七种变量类型各有一套规则，且**校验与类型转换是同一个动作**：`NUMBER` 分支会把字符串 `"3.5"` 转成 `float`，`CHECKBOX` 分支会把 `"true"` / `"1"` / `"yes"` / `"on"` 归一成 `True`。这意味着下游收到的已经是正确类型，不需要再判。

## 怎么实现的

### BaseAppGenerator：入参处理

`_prepare_user_inputs` 是入口，五步：

1. `apply_file_input_defaults` 补文件类输入的默认值。
2. **按变量声明重建整个字典**——不是「校验用户给的」，而是「按 `variables` 逐个取值并校验」。用户多传的键会被**丢弃**（因为结果字典的键来自 `variables`）。这一步同时处理 `required` 与 `default`。
3. `_sanitize_value` 去掉字符串里的 `\x00`（NUL 字符）。**这是唯一一处清洗**，且只处理字符串。
4. 把 `FILE` 类型（单文件）与 `FILE_LIST` 类型（文件列表）的 dict 值转换成 `File` 对象，走 `file_factory.build_from_mapping` / `build_from_mappings`，并传入该变量自己声明的 `allowed_file_types` / `allowed_file_extensions` / `allowed_file_upload_methods`。
5. 合并回字典后做**两道反向检查**：剩下仍是 `dict` 的键，其变量类型必须属于 `{FILE, JSON_OBJECT}` 之一，否则抛 `ValueError`；剩下仍是「元素为 dict 的 list」的键，变量类型必须是 `FILE_LIST`，否则也抛。

第 5 步是防止「本该被转换成 `File` 却没转成功」的兜底：如果类型声明是 `FILE` 但值不是 dict（比如已经是 `File` 对象），它合法；如果声明的不是文件类型却留着一个 dict，说明有转换漏了。

`_validate_inputs` 的七种分支值得逐条看：

- **`TEXT_INPUT` / `SELECT` / `PARAGRAPH`**：先要求 `isinstance(value, str)`（不是字符串直接抛），后面再按类型细分——`SELECT` 检查值在 `options` 里，`TEXT_INPUT` 与 `PARAGRAPH` 检查 `max_length`。
- **`NUMBER`**：接受 `int` / `float` 直接返回；接受 `str` 时，空串返回 `None`，否则**按有没有小数点决定用 `float` 还是 `int`**；其他类型抛 `TypeError`。这里有一个细节：`"3.0"` 会变成 `float(3.0)`，`"3"` 会变成 `int(3)`——**同一个数值字段可能产出两种 Python 类型**。
- **`FILE`**：要求 `dict` 或 `File`。
- **`FILE_LIST`**：要求 `list` 且元素**全为 dict 或全为 File**（混合会失败），再检查数量不超过 `max_length`。
- **`CHECKBOX`**：字符串按四个真值词与四个假值词归一，`1`/`0` 数字也归一。**其他字符串原样保留**（既不报错也不转换），这是一个宽松的口子。
- **`JSON_OBJECT`**：非空且不是 dict 才报错——空值放行。
- 兜底 `raise AssertionError("this statement should be unreachable.")`。

`value is None` 的分支有一个考虑周到的细节：**必填则报错，非必填则用 `default` 并继续走校验**（注释明写「继续校验以确保类型转换」）；若 `default` 也是 `None` 才直接返回 `None`。这避免了「默认值没做类型转换」的常见缺陷。

可选文件类型还有一个特殊处理：`value` 是空字符串时返回 `None`。注释说明这是「把前端的空占位当成未设置」，且 `FILE_LIST` 的空列表 `[]` 允许穿过（不转成 `None`）。

`_bind_file_access_scope` 把「租户 + 用户 + 用户种类 + 调用来源」绑成一个上下文管理器，供下游文件查找做归属判定。**`user_id` 不是非空字符串时返回 `nullcontext()`**——静默降级而不是报错。

`_restore_workflow_run_graph` 处理「恢复运行」场景：从 `WorkflowRun` 里把当时那份 `graph` 用 `set_committed_value` 写回 `workflow` 对象。**用 `set_committed_value` 而不是普通赋值**，是为了让 SQLAlchemy 认为这个值「已从数据库加载」，从而不会在 flush 时把它当成脏字段写回。这是「用历史快照覆盖当前对象但不想持久化」的正确做法。

`_join_worker_thread` 是这份代码里少见的**防御性运维措施**：`join` 带 300 秒上限，超时就打一条 `Possible app worker thread leak` 的 warning 然后继续。注释写明动机：「避免泄漏的 app worker 无限占用一个执行槽」。`_wrap_stream_with_worker_thread_join` 则在生成器的 `finally` 里调它——**把「等生产线程收尾」绑在响应流的生命周期上**，流结束（正常或异常）就一定等一次。

`convert_to_event_stream` 是 SSE 编码器，做法极简：值若是 `dict` 直接返回（非流式）；否则逐条产出——`Mapping`/`dict` 编码成 `data: <orjson>\n\n`，其他值当作**事件名**编成 `event: <name>\n\n`。所以生成器可以混着吐「数据帧」与「控制帧」。

`_get_draft_var_saver_factory` 按调用来源返回两种 saver：`DEBUGGER` 返回真实落盘的 `_DebuggerDraftVariableSaver`（内部自己开 session 并 `session.begin()`），其他来源返回 `NoopDraftVariableSaver`。**草稿变量只在调试器里有意义**——网页端与 API 调用不该把中间变量写进草稿表。

`_DebuggerDraftVariableSaver` 用 `@final` 装饰，注释写着「Adapter that binds SQLAlchemy session setup outside the saver port」——它存在的原因是真实的 saver 需要一个 session，而工厂契约里没有 session 参数，所以用适配器在内部自建。

### AppGenerateResponseConverter：出参规则

`convert` 是唯一的公开方法，它只做两件事：**判来源**决定 full 还是 simple，**判类型**决定 blocking 还是 stream。流式分支再包一层生成器函数，把 `convert_stream_*_response` 的产出原样 `yield from`——这层包装是必要的，因为**调用方需要立刻拿到生成器而不是在函数返回时就执行完**。

四个 `convert_*` 都是 `@classmethod` + `@abstractmethod`。用类方法而不是实例方法，说明**转换器是无状态的**，每个形态只需一个类，不需要实例化。

`_get_simple_metadata` 的字段白名单是硬编码的 20 个键。这种写法的后果是：**内部结果里新增字段默认不会出现在简化输出里**（因为重建字典时只取白名单），而新增字段要暴露给最终用户必须改这里。默认安全，代价是同步成本。

`_error_to_stream_response` 是错误翻译表，值得单独看，因为它把三类错误处理得不一样：

1. **`AgentBackendRunFailedError` 且 `error_type` 是 `AGENT_RUN_LIMIT_EXCEEDED`**：单独判，返回 400 带这个具体的 code。
2. **`ModerationInvocationError`**：交给 `moderation_http_exception` 分流成 `ModerationRejectedHTTPError`（400）或 `ModerationUnavailableHTTPError`（503）。注意后者的描述文案是**刻意不含 provider 诊断信息**的（docstring：「不以 provider 诊断暴露安全可重试的传输错误」）。
3. **其余走查表**：`ValueError` → `invalid_param` / 400，`ProviderTokenNotInitError` → `provider_not_initialize` / 400，`QuotaExceededError` → 400 带一段引导用户配置自己凭据的固定文案，`ModelCurrentlyNotSupportError` → 400，`InvokeError` 与 `AgentBackendError` 都映射成 `completion_request_error` / 400，`InvokeRateLimitError` → 429。

**遍历顺序是有意义的**：`for k, v in error_responses.items(): if isinstance(e, k): data = v` **不 break**，所以最后一个匹配的类型胜出。而字典里 `AgentBackendError` 排在 `InvokeError` 后面——如果两个异常有继承关系，行为会取决于声明顺序。这是一个隐藏的、依赖插入顺序的判定。

**没有匹配到任何类型时记 `logger.error(e)` 并返回通用的 500 文案**，不把原始异常消息透给客户端。

`data.setdefault("message", ...)` 只在表里那一项没有自定义 message 时才填——所以 `QuotaExceededError` 的定制文案优先，其余用异常的 `description` 或 `str(e)`。

## 关键取舍

**源码事实：** 输入校验同时承担类型转换（`NUMBER` 的字符串解析、`CHECKBOX` 的真假词归一），下游拿到的是已转换的值。

**源码事实：** `_prepare_user_inputs` 按变量声明重建字典，用户多传的键被丢弃。

**源码事实：** 非必填且值为 `None` 时取 `default` 并**继续走校验**；可选文件类型的空字符串被视为未设置。

**源码事实：** `_restore_workflow_run_graph` 用 `set_committed_value` 覆盖 `graph`，避免被当成脏字段回写。

**源码事实：** `_join_worker_thread` 有 300 秒上限，超时只记 warning 后继续；`_wrap_stream_with_worker_thread_join` 把这次等待放在生成器的 `finally` 里。

**源码事实：** 简化响应会重建 `retriever_resources`（20 个字段白名单）并删除 `annotation_reply` 与 `usage`。

**源码事实：** 错误翻译表的遍历不 break，「最后一个 `isinstance` 匹配的类型胜出」。

**架构推论：** 「全量 / 简化」的分界线是**调用来源**而不是请求参数。这意味着最终用户在浏览器里看到的响应结构与 API 调用者看到的**永远不同**，且用户无法要求全量。这是一个产品决策被编码到了类型分派里——好处是不可能配错，代价是「想让网页端也能看到 usage」必须改代码。

**架构推论：** 输入校验兼做类型转换，把「验证」与「规范化」合并成一步。这消除了「校验通过但类型不对」的中间态，代价是**校验函数有副作用**（改变返回值类型），从签名上看不出来。`NUMBER` 字段可能变成 `int` 也可能变成 `float`，下游若依赖具体类型就会出错。

**架构推论：** 简化响应的字段白名单是「默认拒绝」策略。这与错误响应的「未匹配就 500」是同一种保守取向：**不认识的输入视为不可信，不认识的输出视为不该暴露**。两者都在新字段出现时要求显式声明。

**架构推论：** `_join_worker_thread` 的 300 秒上限与「超时继续」是一条成熟的运维取舍：一个泄漏的线程会让一次调用永久卡住并占住一个执行槽，而记录 warning 让问题可见但不阻塞服务。**在有资源池的系统里，「永远不阻塞」优先于「等干净」。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/app/apps/base_app_generator.py` | `BaseAppGenerator` | 输入关口与线程收尾的公共实现 |
| `api/core/app/apps/base_app_generator.py` | `_prepare_user_inputs` | 按声明重建输入、清洗、转文件对象、反向校验 |
| `api/core/app/apps/base_app_generator.py` | `_validate_inputs` | 七种变量类型的校验与类型转换 |
| `api/core/app/apps/base_app_generator.py` | `_sanitize_value` | 去除 NUL 字符 |
| `api/core/app/apps/base_app_generator.py` | `_restore_workflow_run_graph` | 用历史快照覆盖 graph 且不标记为脏 |
| `api/core/app/apps/base_app_generator.py` | `_bind_file_access_scope` | 绑定文件归属标记，无用户时静默降级 |
| `api/core/app/apps/base_app_generator.py` | `_join_worker_thread` | 有上限的工作线程收尾，超时只告警 |
| `api/core/app/apps/base_app_generator.py` | `_wrap_stream_with_worker_thread_join` | 把线程收尾绑到流生命周期 |
| `api/core/app/apps/base_app_generator.py` | `convert_to_event_stream` | SSE 编码：数据帧与事件帧 |
| `api/core/app/apps/base_app_generator.py` | `_get_draft_var_saver_factory` | 调试器落盘、其他来源 no-op |
| `api/core/app/apps/base_app_generate_response_converter.py` | `AppGenerateResponseConverter` | 来源 × 形态的四种转换契约 |
| `api/core/app/apps/base_app_generate_response_converter.py` | `AppGenerateResponseConverter.convert` | 二维分派与流式包装 |
| `api/core/app/apps/base_app_generate_response_converter.py` | `_get_simple_metadata` | 简化响应的字段白名单与删除项 |
| `api/core/app/apps/base_app_generate_response_converter.py` | `_error_to_stream_response` | 异常到错误帧的统一翻译 |
| `api/core/app/apps/base_app_generate_response_converter.py` | `moderation_http_exception` | 审核错误二分：拒绝 vs 暂时不可用 |
| `api/core/app/apps/base_app_generate_response_converter.py` | `ModerationUnavailableHTTPError` | 503 且不含 provider 诊断 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/app/apps/base_app_generator.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/base_app_generator.py)
- [dify 源码：core/app/apps/base_app_generate_response_converter.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/base_app_generate_response_converter.py)
- [Server-Sent Events 规范](https://html.spec.whatwg.org/multipage/server-sent-events.html)

## 与主核 / AI 的关系

「按声明重建输入」与主核里「按表定义编解码一行」是同一种做法：主核不信任客户端发来的字节序列，而是按列类型逐个解析；这里不信任客户端发来的字典，而是按 `VariableEntity` 逐个取值校验。**两者都把「多传的字段」直接丢弃**——这是最省事也最安全的策略，因为未知字段没有归属。

「全量 vs 简化」的二分与主核里「系统视图 vs 用户视图」相通：同一份数据按调用者身份裁剪。差别是主核的裁剪由权限系统决定（能看哪些列/行），dify 的裁剪由**调用来源的枚举值**硬编码决定（调试器看全量，最终用户看简化）。前者可组合，后者只能在代码里加分支。

`_error_to_stream_response` 的「未匹配就 500 且不透露细节」与主核的「内部错误只回通用消息、详情进日志」完全一致：**错误信息本身是攻击面**。dify 在这里特别小心的地方是 `ModerationUnavailableHTTPError` 的文案——它刻意不带 provider 名称与原始诊断，避免泄露「你用的是哪个模型服务商」。

对 agent / RAG 工程的实际含义有四条。其一，网页端与 API 拿到的响应结构不同（简化侧没有 `usage` 与 `annotation_reply`），做前端联调时不要拿 API 文档对照网页响应。其二，`NUMBER` 类输入可能落成 `int` 也可能落成 `float`，用它参与类型敏感的分支前要显式归一。其三，非必填字段的 `default` 会被完整校验，所以默认值写错会在运行时暴露而不是被静默采用。其四，调试器之外的所有调用来源都不会保存草稿变量，把「中间变量留档」当功能依赖会只在调试模式下成立。
