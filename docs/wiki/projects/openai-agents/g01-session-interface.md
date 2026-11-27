---
project: openai-agents
feature: session-interface
title: 会话协议与兼容适配
page_type: feature
schema_version: 1
refs:
  - path: src/agents/memory/session.py
    symbols: [Session, SessionABC, is_openai_responses_compaction_aware_session]
---

# 会话协议与兼容适配

## 一句话职责

规定「可持久化对话历史」的最小异步接口，并为可选的 Responses 压缩能力提供结构化能力检测、为新增 `wrapper` 上下文参数保留旧会话签名的兼容调用层。

## 机制是什么

`Session` 是运行时可检查的 `Protocol`：会话 id、可选 `session_settings`，以及 `get_items`、`add_items`、`pop_item`、`clear_session` 四个异步方法。SDK 调用方因此可以接受第三方对象，只要其结构符合协议，不要求继承某个 SDK 基类。

`SessionABC` 则是内置实现共享的抽象基类，四个方法同样抽象。源码明确建议第三方库实现 `Session` 协议，而把 ABC 定位为 SDK 内部与具体实现的基类。协议与 ABC 看似重复，实际服务不同边界：结构型扩展与代码复用。

会话另有两个可选能力：`OpenAIResponsesCompactionAwareSession` 在 Session 之上增加 `run_compaction(args)`，`is_openai_responses_compaction_aware_session` 用 `TypeGuard` 判断一个对象是否提供可调用的压缩方法。SDK 不要求所有 Session 都支持压缩，而是在需要该功能时选择性调用。

## 怎么实现的

**限制的解析。** `SessionSettings` 是配置 dataclass，目前核心字段为 `limit`。`resolve_session_limit(explicit_limit, settings)` 优先采用调用方显式传入的 limit，否则读取 session settings；两者都没有则返回 `None`（不限量）。零与负数保留原值，具体实现应把它们解释为空结果，而不是由解析器改成默认值。

**协议扩展不破坏旧签名。** 新的上下文感知会话方法可以选择增加 keyword-only `wrapper` 参数，但公开 `Session` 协议保留已发布的原签名。`_session_method_accepts_wrapper` 用 `inspect.signature` 判断某个绑定方法是否有名为 wrapper 的参数或 `**kwargs`；签名无法检查时返回 False。`_session_accepts_wrapper` 要求 `get_items`、`add_items`、`pop_item`、`clear_session` 四个方法**全部**接受 wrapper，才把它视为完整支持。

调用方经 `_call_session_method(method, *args, wrapper=..., **kwargs)` 进入适配层：仅当 wrapper 非空且该方法声明了 wrapper 时才注入 keyword 参数；同步返回值直接交还，awaitable 则 await。由此旧实现继续按旧形状工作，新实现按能力自愿接收运行上下文。

**检测可选压缩能力。** `is_openai_responses_compaction_aware_session` 对 None 直接返回 False，读取 `run_compaction` 属性出错也返回 False；否则要求它可调用。它不检查继承关系或额外方法清单，因此这是轻量鸭子类型能力探测。

**写操作取消语义。** `await_session_mutation` 为会话写操作提供取消边界：它先把 awaitable 包成 task，再在循环里等任务完成；调用方的取消被记录但不会取消正在进行的写。操作结束后，若调用方曾取消，则抛出第一次 `CancelledError`，即使底层写成功或失败也不把取消伪装成回滚。函数文档特别说明它不提供事务、超时、重试或互斥，底层客户端仍需配置超时。

## 关键取舍

**源码事实：** `Session` 使用结构协议，`SessionABC` 留给内置实现；第三方可不继承 SDK 类而接入。协议方法签名保持稳定，新能力通过单独的能力协议和探测函数添加。

**源码事实：** wrapper 兼容判断要求四个核心方法全部接收 wrapper；单个方法的能力不足以宣称整个会话实现上下文感知。无法 introspect 的 callable 被保守地视为不支持。

**源码事实：** `await_session_mutation` 把调用方取消延迟到写操作 settle 后才传播。因此取消异常不能被解释成写入已撤销；卡住的操作也会延迟取消，必须依赖底层超时。

**架构推论：** Protocol 与 ABC 双轨是「让外部实现低耦合、让内部实现有复用点」的边界设计。若只提供 ABC，第三方需要继承；若只提供 Protocol，SDK 内部会失去共享实现的落点。

**架构推论：** 用方法签名探测 wrapper 而非给 Session 增加版本号，属于渐进式协议演进：旧实现无需改动，新实现 opt-in。代价是 wrapper 的支持不是类型协议的一部分，静态检查不一定能完整表达运行期检测规则。

**架构推论：** 写操作对调用者取消采取「等待提交结果后再取消」策略，是为避免后台写仍在进行时上层误以为可以安全重试。这个契约强调异步取消是控制流信号，不等于数据库事务回滚。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/memory/session.py` | `Session` | 运行时可检查的结构型最小协议 |
| `src/agents/memory/session.py` | `SessionABC` | 内置实现复用的抽象基类 |
| `src/agents/memory/session.py` | `OpenAIResponsesCompactionAwareSession` | 可选的 Responses 压缩能力协议 |
| `src/agents/memory/session.py` | `is_openai_responses_compaction_aware_session` | 鸭子类型能力检测 |
| `src/agents/memory/session.py` | `_session_method_accepts_wrapper` | 检查方法能否接收 wrapper |
| `src/agents/memory/session.py` | `_call_session_method` | 新旧会话签名兼容调用器 |
| `src/agents/memory/session.py` | `await_session_mutation` | 等写操作完成后传播调用方取消 |
| `src/agents/memory/session_settings.py` | `resolve_session_limit` | 显式 limit 优先于 session 默认值 |

## 参考

- [openai-agents-python 源码：memory/session.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/memory/session.py)
- [openai-agents-python 源码：memory/session_settings.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/memory/session_settings.py)
- [Python 文档：typing.Protocol](https://docs.python.org/3/library/typing.html#typing.Protocol)

## 与主核 / AI 的关系

主核里相近的是存储接口：上层只知道「读、写、弹出、清空」，具体数据落在内存还是 SQLite 由实现决定。这里也用同一分层，但协议消费的是模型输入项而非关系记录，并且显式把「压缩」作为可选能力而非基础接口的一部分。

与 CrewAI 的 memory 抽象相比，这里的 Session 聚焦于**一段运行可续接的输入历史**，接口不承担长期语义检索；会话配置 `limit` 负责限制读出的尾部，而可选 compaction 协议面向 Responses API 的服务端或输入压缩能力。把二者混为一个「memory」接口会让所有适配器背上不需要的能力。

对 AI 工程最实用的是增量协议演进方式：基础方法签名保持原样，特性单独定义 Protocol，再用 TypeGuard 探测。这样本地会话、远端会话与第三方实现可以按能力组合，不必为了一个可选功能同时升级所有实现。