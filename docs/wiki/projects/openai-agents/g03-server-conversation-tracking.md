---
project: openai-agents
feature: server-conversation-tracking
title: 服务端会话已发送项追踪
page_type: feature
schema_version: 1
refs:
  - path: src/agents/run_internal/oai_conversation.py
    symbols: [OpenAIServerConversationTracker, _fingerprint_for_tracker]
---

# 服务端会话已发送项追踪

## 一句话职责

跟踪服务端 Responses 会话已经接收、存储或确认的输入与输出项，在恢复、重试与序列化重建之后过滤已送达内容，避免重复提交工具结果或整段历史。

## 机制是什么

`OpenAIServerConversationTracker` 用三种互补证据表示「服务端已经知道什么」：当前 Python 进程的对象身份、Responses 返回的稳定 item/call id，以及内容指纹。类注释说明三者各解决一类场景：对象身份适用于当前进程；服务端 id 适用于 provider 返回标识的项；指纹适用于 RunState 恢复时对象被重新构造、原对象身份丢失。

状态还包括 conversation_id、previous_response_id、自动 previous id 开关、首次输入是否已发、尚未发送的初始项、是否从状态灌入、reasoning item id 策略，以及「准备输入项 → 原始对象」的源映射。这样 tracker 不只是去重集合，也是请求确认与本地对象回溯的记账器。

## 怎么实现的

**从已保存状态重建。** `hydrate_from_state` 接受原始输入、生成的 RunItem、模型响应、可选 session 历史与未完成工具调用 id。它遍历这些材料，将可识别的服务端 item id、tool call id 与 fingerprint 填回 tracker。稳定 id 与内容指纹同时保留，因旧 provider 或合成项可能没有服务端 id。

**指纹规范化。** `_fingerprint_for_tracker` 对一般输入项直接调用 `fingerprint_input_item`。tool_search 项先转成可重放的输入项，再读取规范化后的 item id 与 call_id；如果缺少二者，就要求 fingerprint 忽略 id。任何转换或指纹异常都返回 None，不让去重辅助逻辑阻断模型运行。匿名 tool_search 另有内容指纹函数，用于状态恢复后识别没有稳定标识的服务端产物。

**准备下一次输入。** `prepare_input` 先清空上轮临时 source map。初次调用把 original_input 转成新输入项、规范化后登记对应源对象，并保留可重新发送的初始项；后续只重送 remaining_initial_input。生成项逐个跳过：审批项、无 raw_item 项、已被服务端确认的 input_id、已知 server item id、已有输出的 server tool_call id、对象身份已送达项、与服务端输出指纹重复项、恢复后与历史已送指纹重复项，以及匿名 tool_search 的重复项。

剩余 RunItem 经 `run_item_to_input_item` 转换，再做输入归一和 `drop_orphan_function_calls`（丢掉没有配套输出的孤立函数调用）。过滤后的每个项都登记回原始源与 input_id，最终结果是「初始项 + 尚未发送的生成项」。

**确认与撤回。** `mark_input_as_sent` 在模型调用成功后记录本次输入已发送，建立对象身份和 fingerprint 账本；若请求失败，runner 可 `rewind_input` 恢复本次未确认的初始输入状态。`mark_input_as_accepted` 根据服务端响应确认哪些输入 item 已被接受，返回接受 id 集合，供续跑路径继续过滤。

**调用响应后更新服务端视图。** `track_server_items(model_response)` 收集响应里服务端 item id、工具调用 id 与输出 fingerprint。`_normalize_server_item_id` 忽略 `FAKE_RESPONSES_ID`，因为非 Responses provider 的占位 id 不能作为稳定服务端去重键。只有带输出 payload 的 tool call id 才加入已完成工具调用集合，避免把尚未得到结果的 call 错当已完成。

**对象引用而不是整数 id。** `sent_items` 与 `server_items` 保存对象引用，`_is_tracked_object` 用 `is` 比较，`_track_object_once` 防止重复添加。注释说明原因：仅存 `id(obj)` 整数会在对象释放后被 Python 重用；持有对象引用保证用于去重的身份在 tracker 生命周期内不会撞地址。

## 关键取舍

**源码事实：** 指纹失败会降级成 None，而不是阻止请求。此时仍可由对象身份或服务端 id 去重，但对于恢复后重建且没有稳定 id 的项，防重能力会变弱。

**源码事实：** 初始输入与生成历史分开处理：初始输入只有首次发送或明确保留的剩余尾部参与构造，生成项则按多种账本过滤。这避免把服务端已有历史和本地生成结果重复拼接。

**源码事实：** 匿名 tool_search 有单独的内容 fingerprint 集合。普通 server item 可用 id，tool_search 在部分响应里没有稳定 id，需依赖转换后内容匹配。

**源码事实：** `drop_orphan_function_calls` 在所有其它去重后执行，保证即使调用本身没有被 id/fingerprint 过滤，缺少输出配对的函数调用也不会单独送到模型。

**架构推论：** 多账本去重是为了解决分布式确认的部分性：客户端可能知道对象发送过，服务端可能只返回 item id，也可能恢复后只能比较内容。任何单一标识在这三种时序里都不够。

**架构推论：** `mark_input_as_sent` 与 `mark_input_as_accepted` 区分「请求提交成功」和「服务端逐项确认」两个阶段。对有服务端状态的协议，调用成功不代表所有本地项已被接受，因此需要阶段性账本。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/run_internal/oai_conversation.py` | `OpenAIServerConversationTracker` | 管理服务端会话与本地输入的多重去重状态 |
| `src/agents/run_internal/oai_conversation.py` | `OpenAIServerConversationTracker.hydrate_from_state` | 从 RunState、响应与 session 历史重建账本 |
| `src/agents/run_internal/oai_conversation.py` | `OpenAIServerConversationTracker.prepare_input` | 过滤重复项、转换并登记源对象 |
| `src/agents/run_internal/oai_conversation.py` | `OpenAIServerConversationTracker.mark_input_as_sent` | 请求成功后记下已发送输入 |
| `src/agents/run_internal/oai_conversation.py` | `OpenAIServerConversationTracker.mark_input_as_accepted` | 记录服务端明确接受的 item id |
| `src/agents/run_internal/oai_conversation.py` | `OpenAIServerConversationTracker.track_server_items` | 从响应更新服务端 item/call/fingerprint 集合 |
| `src/agents/run_internal/oai_conversation.py` | `_fingerprint_for_tracker` | 按项类型计算恢复路径去重指纹 |
| `src/agents/run_internal/oai_conversation.py` | `_normalize_server_item_id` | 排除 fake id 的稳定 id 归一 |
| `src/agents/run_internal/oai_conversation.py` | `_track_object_once` | 保留对象引用以防 id 重用 |

## 参考

- [openai-agents-python 源码：run_internal/oai_conversation.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/oai_conversation.py)
- [OpenAI 文档：Responses API](https://platform.openai.com/docs/api-reference/responses)
- [Agents SDK 文档：Sessions](https://openai.github.io/openai-agents-python/sessions/)

## 与主核 / AI 的关系

主核的复制与事务机制通过 LSN、事务 id 等稳定标识区分已应用操作，恢复后能判断哪些记录已经持久化。这里没有一个统一事务日志，所以以对象身份、服务端 id、内容 fingerprint 拼出应用层的「至少一次发送 + 去重」账本。

与 AutoGen 的本地 Session 历史相比，这个 tracker 关注的是**远端 Responses 服务端状态**：本地 session 只回答「我保存了哪些消息」，tracker 还要回答「哪些已经送到服务端、服务端响应里又确认了哪些」。二者协同后，运行器才能同时利用服务端 conversation 延续与本地恢复。

对 AI 工程最实用的是将「准备发送」「发送完成」「逐项接受」区分开。网络重试、工具调用结果与状态恢复都会制造重复风险；用多种稳定性不同的键组成账本，并在每个生命周期节点更新对应集合，比依赖消息文本相等更可靠。