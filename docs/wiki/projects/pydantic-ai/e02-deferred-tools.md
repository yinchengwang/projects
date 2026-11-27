---
project: pydantic-ai
feature: deferred-tools
title: 延迟工具调用与审批
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_deferred.py
    symbols: [DeferredToolRequests, DeferredToolResults, ToolApproved, ToolDenied]
  - path: pydantic_ai_slim/pydantic_ai/exceptions.py
    symbols: [ApprovalRequired, CallDeferred]
---

# 延迟工具调用与审批

## 一句话职责

延迟工具让 agent 在一步之内「停下来把某些调用交出去」：把需要人工审批或外部执行的调用打包成 `DeferredToolRequests` 作为本次运行的输出，等调用方拿 `DeferredToolResults` 带结果回来时再续跑——被延迟的调用本身的参数已经校验过。

## 机制是什么

延迟有三种来源，最终汇成同一个数据结构：

- `ApprovalRequired`：需要人工审批。来自 `requires_approval=True` 的工具、`ApprovalRequiredToolset`、或工具函数/校验器主动抛出。
- `CallDeferred(kind='external')`：结果要在 agent 之外产生（上游服务、慢任务）。
- `CallDeferred(kind='question')`：模型在向人索取内容（例如提问），由 `DeferredToolRequests.kind()` 读回。

对应的结果也有三型：`ToolApproved` / `ToolDenied`（审批）、`ToolReturn` / `ToolFailed` / `ModelRetry`（外部执行）、以及裸值（会被包成 `ToolReturn`）。

关键点是**这些调用被延迟时参数已经通过了校验**，所以 `DeferredToolRequests.validated_args` 保存着「工具将真正收到的参数」——`CallDeferred` 的 `args_validator` 可以在参数合法之后才决定延迟，那时 `args_valid` 仍为真。

## 怎么实现的

**从异常到数据。** 执行层跑一个调用时若拿到 `CallDeferred` / `ApprovalRequired`，就把这两类异常当成**控制流**收集起来，而不是失败：

- `_call_tools` 的 `handle_call_or_result` 捕获 `CallDeferred` 时按 `e.kind` 记进延迟桶，捕获 `ApprovalRequired` 时固定记进 `'unapproved'` 桶，两者都保存 `e.metadata`。
- `exhaustive` 并行策略里 `run_one` 把这两类异常当作返回负载；主循环据此分桶。
- `_execute_tool_call` 额外记一件事：`CallDeferred(kind='question')` 的调用会存进 `question_calls`，因为它的回答在运行内到达时要经过工具的 `after_tool_execute` 钩子，与「工具自己等答案」时行为一致。

**三个桶与最终组装。** `_DeferredBucket` 是 `'unapproved'` / `'external'` / `'question'`，`_DEFERRED_BUCKETS` 定义了顺序。`_deferred_tool_requests` 把 external 与 question 的调用合并进 `calls`，但**按模型发出的顺序**排：question 是单独收集的，所以要用 `positions` 映射按原顺序重排。`approvals` 只取 unapproved 桶。

`_route_deferred_call` 说明了「路由依据」的优先级：若校验阶段抛出了延迟（`validated.deferral`），按**该延迟的类型**分桶（`CallDeferred` 用它的 `kind`，`ApprovalRequired` 固定 unapproved）；若校验没延迟，才按工具定义本身的 `kind` 分。注释写的理由很直白：校验中抛出的延迟要和工具函数抛出的一样处理。

**`validated_args` 为什么要 JSON 化。** `jsonable_validated_args` 用 `to_jsonable_python(serialize_unknown=True, bytes_mode='base64')`。docstring 给了两条理由：一个 Pydantic 无法序列化的值会存成它的 `str()`，`bytes` 存 base64（因为它不一定是 UTF-8）——「这样一个工具参数不会让整个 `DeferredToolRequests` 变成不可序列化的」。注释还补了一句：`DeferredToolRequests.calls` / `approvals` 里存的是**模型发来的原始参数**，而 `validated_args` 是「校验并跑过校验钩子之后」的形态——值被强制转换、schema 不接受的键被丢弃、钩子的改写被应用，且按工具参数名索引，嵌套模型按别名展开、`bytes` 转 base64，所以能原样跨序列化边界。还有一个细节：每个模型用它自己的序列化器 dump，因此被 `Field(exclude=True)` 排除的字段不会出现在这里——尽管工具真的会收到它。

**`DeferredToolRequests` 的两个便利方法。** `build_results` 从请求反向构造结果，并做三件事：拒绝 `approvals` / `calls` 里出现不属于对应类别请求的 id（报 `ValueError` 并列出多余的 id）；`approve_all=True` 时给所有未列出的审批请求补 `ToolApproved()`；`metadata` / `answered_by` 直通。

`remaining` 是「部分解决」的关键：先用 `filter_deferred_results` 过滤（只保留 id 属于对应类别的结果），再按 id 差集重建一个 `DeferredToolRequests`，剩下什么都没有就返回 `None`。这条「剩下就算没解决」的语义是能力处理器可以逐个解决调用的基础。

`DeferredToolResults.to_tool_call_results` 做归一：`True` → `ToolApproved()`、`False` → `ToolDenied()`、既非 bool 也非这两种就报 `UserError`；`calls` 里不是 `DeferredToolCallResult` 联合成员的裸值，包成 `ToolReturn(值)`。

**判别器要兼容三种形态。** `_deferred_tool_call_result_discriminator` 逐层降级判断：先看是不是 `ToolFailed` / `ModelRetry` 实例，再看字典里的 `kind` / `part_kind`，再看对象属性 `kind` / `part_kind`。这个函数的存在说明「结果」可能以实例、字典、或历史部件三种形态到达——尤其是从序列化历史恢复时。

`RetryPromptPart` 仍在联合里但已标 `Deprecated`：注释说明「用它的处理器得到的结果与 `ModelRetry` 相同」，所以旧处理器不必改，`v3` 才删除。

**`_resolve_deferred_calls` 是运行内解决的一站。** 顺序是：

1. `_show_private_deferred_calls_redacted`：把 `ui_visibility='private'` 的延迟调用改标为 `'redacted'`。注释的理由很实际：UI 客户端必须看见它被要求作答的那次调用。它同时改写三个地方以保持一致：延迟桶里的调用、`question_calls` 里对应的已验证调用、以及历史里那条响应（用副本替换，绝不原地修改历史部件）。历史替换还有一条精度要求：一个响应可能在 `graceful` 的不同批次里重复同一个 `tool_call_id`，所以只替换「同一个 `tool_call_id` 且 `tool_name` 相同」的那个部件。
2. **重复 id 检查**：把三个桶里的延迟调用合起来查重复 id。注释说延迟调用要交还给调用方、之后按 id 匹配回来，重复 id 会让匹配有歧义。
3. 组装 `DeferredToolRequests` 并先发 `DeferredToolRequestsEvent`——让流式消费者看见「有一批待交互」，而不必耦合到解决处理器。
4. 调 `ToolManager.resolve_deferred_tool_calls` → 能力的 `handle_deferred_tool_calls`。若拿到结果，发 `DeferredToolResultsEvent`，再把结果**喂回同一条工具执行流水线**（`_call_tools`），注释说明了理由：这样审批、拒绝、重试与 `ToolReturn` 的解包都与「用户提示节点续跑」时完全一样。
5. 对审批通过（`ToolApproved`）的调用，先 `_validate_approved_call` 重新校验（处理器可能给了 `override_args`），校验失败直接发事件并抛出。
6. 用 `remaining(handler_results)` 收缩剩余请求；若在解决过程中又有调用延迟（例如一个被批准的调用重新执行时抛 `CallDeferred`），**不再发第二个 `DeferredToolRequestsEvent`**——注释说同一次调用 id 被再次宣告（可能换了 kind）对消费者是歧义的；新的待定批次最终仍作为本次运行的输出浮现。
7. 剩余请求非空时，若输出 schema 不允许延迟工具（`allows_deferred_tools` 为假），抛 `UserError` 并提示「把 `DeferredToolRequests` 加进输出类型，或用 `HandleDeferredToolCalls` 处理」。否则把剩余请求设成这次运行的最终结果（`FinalResult(requests, None, None)`——注意 tool_name 与 call_id 都是 `None`）。

**内联解决的单条路径。** `ToolManager._resolve_single_deferred` 是 `handle_call` 版本：只带一条调用，构造一个最小的 `DeferredToolRequests`（`approvals` / `calls` 择一，`metadata` 只在有内容时带，`validated_args` 带上该调用，`_kinds` 在 question 时标记），先调 `on_deferred_requests` 回调（注释说与图里发事件同一个时序——处理器可能要等很久，调用方可以先把待定请求呈现出去），再问处理器。它的 docstring 很详细地列了返回值与异常契约，并特别警告「这里的派发分支要与 `_call_tool` 保持同步」。它还明确说明 `wrap_validation_errors` 会继续透传到「批准后的重新校验与重新执行」，所以沙箱这类原始模式调用方在延迟解决路径上仍是原始错误契约；但处理器自己构造的重试信号（返回 `ModelRetry` / `RetryPromptPart`）**无论如何**都变成 `ToolRetryError`，因为「那是处理器的输出，不是校验或工具体抛出的异常」。

**`filter_deferred_results` 的小心之处。** 它在过滤 `answered_by` 时用的是 `approval_ids | call_ids`，而 `metadata` 反而原样传递——即元数据不按 id 过滤。这是一处刻意的不对称，大概是元数据本身不参与「哪个调用被解决」的语义。

## 关键取舍

**源码事实：** 延迟被表达成异常（`CallDeferred` / `ApprovalRequired`），但被当作返回负载收集，而不是当作失败。于是「停下来交出去」与「工具自己做完了」在同一条执行流水线里共存，不需要第二套驱动。

**源码事实：** `validated_args` 与 `calls` / `approvals` 里保存的参数**不是同一份形态**：前者已校验、已过钩子、已 JSON 化；后者是模型原始发出的。这个双份保存让「谁来执行」与「展示了什么」各取所需。

**源码事实：** `remaining` 是「部分解决」的唯一支撑，返回 `None` 表示全部解决。这使能力处理器可以只处理它能处理的一部分。

**源码事实：** `ui_visibility='private'` 的延迟调用一律降为 `'redacted'`，并且只替换那一条具体调用（按 id 且按工具名匹配），不原地改历史部件。

**源码事实：** 解决过程中再次延迟不再发第二个批次事件，注释给的歧义理由很具体。

**源码事实：** 剩余请求必须被输出类型接受，否则抛 `UserError` 并给出两条修法。这是「延迟能力是可选加入的」的体现——框架不会为一个没声明过的输出类型凭空造一个。

**架构推论：** 用「本步输出」而不是「回调钩子」交付待审批请求，使这套机制与持久执行天然兼容：一次运行结束、请求被序列化、下次运行带着结果回来。代价是调用方必须显式声明输出类型，且每次延迟都要完整跑一趟「模型响应 → 收集 → 交还」的循环。

**架构推论：** 让能力的 `handle_deferred_tool_calls` 可以**运行内**解决，是把「批处理往返」压缩成「同一步内闭环」的通道。但它沿用了同一条执行流水线（`_call_tools`），所以「批准后执行」的钩子、重试、拒绝语义与恢复路径完全一致——这是避免两套语义分叉的正确做法，源码用两条注释反复强调「保持同步」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/exceptions.py` | `CallDeferred` | 声明「结果在外面产生」，可带 `metadata` 与 `kind` |
| `pydantic_ai_slim/pydantic_ai/exceptions.py` | `ApprovalRequired` | 声明「需要人工审批」，可带 `metadata` |
| `pydantic_ai_slim/pydantic_ai/_deferred.py` | `DeferredToolRequests` | 待定批次：调用、审批、元数据、已校验参数、kind 记录 |
| `pydantic_ai_slim/pydantic_ai/_deferred.py` | `DeferredToolResults` | 结果集合：审批、调用结果、元数据、作答者 |
| `pydantic_ai_slim/pydantic_ai/_deferred.py` | `ToolApproved` / `ToolDenied` | 审批的两种终态，前者可带 `override_args` |
| `pydantic_ai_slim/pydantic_ai/_deferred.py` | `DeferredToolRequests.remaining` / `build_results` | 部分解决与从请求反向构造结果 |
| `pydantic_ai_slim/pydantic_ai/_deferred.py` | `jsonable_validated_args` | 已校验参数的 JSON 安全化，单参数不致整批不可序列化 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_route_deferred_call` | 按校验期延迟优先、工具 kind 兜底的分桶规则 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_resolve_deferred_calls` | 运行内解决：脱敏、查重、发事件、喂回流水线、收缩剩余 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `_resolve_single_deferred` | 单条内联解决，保持与图路径一致的派发契约 |

## 参考

- [pydantic-ai 源码：_deferred.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_deferred.py)
- [pydantic-ai 源码：_tool_execution.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_tool_execution.py)
- [Pydantic AI 文档：延迟工具](https://ai.pydantic.dev/tools-toolsets/deferred-tools/)
- [Pydantic AI 文档：人工审批](https://ai.pydantic.dev/tools-toolsets/deferred-tools/#human-in-the-loop-tool-approval)

## 与主核 / AI 的关系

这套「把一批待定工作交还调用方、带结果再回来」的结构，对应主核里的事务「两阶段提交」的意图——都是把一个需要外部决定的步骤拆成「提出请求」与「带回结果」两段，中间状态必须可序列化。差别在原子性：主核的两阶段提交有明确的 prepare/commit 语义与回滚路径；这里的「暂停」没有回滚概念，只是把运行结束掉，责任移交给调用方。它不需要回滚，因为它从不修改持久状态。

`remaining` 这个设计值得对照主核里的「部分完成的批处理」：都允许一批工作里只完成一部分，剩余部分重新表达为「还没做的」。主核里这靠游标与重试记录，这里靠「按 id 差集重建请求对象」。后者明显简单，因为它不需要在存储层留下痕迹。

最值得对照的是「校验后参数」与「原始参数」双份保存。主核里对应的是「解析后的查询树」与「原始 SQL 文本」——前者给执行器，后者给日志、审计与报错展示。两份形态的差异同样是刻意的：执行需要已归一化的值，而展示需要用户原本写的东西。

对 AI 工程的实际意义在于：人工审批是 agent 落地的必经环节，而它最容易做错的地方恰是这里被特别处理的几处——审批时要展示的是**将要执行的那个调用**（所以私密调用必须降级为可展示）、批准后的执行必须走**同一条流水线**（否则拒绝/重试语义会分叉）、以及「等结果」必须能跨越进程边界（所以待定请求与已校验参数都要能序列化）。
