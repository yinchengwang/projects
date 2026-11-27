---
project: openai-agents
feature: mcp-manager
title: MCP 多服务器生命周期管理
page_type: feature
schema_version: 1
refs:
  - path: src/agents/mcp/manager.py
    symbols: [MCPServerManager, _ServerWorker, _ServerCommand]
---

# MCP 多服务器生命周期管理

## 一句话职责

把多个 MCP server 的连接、失败隔离、重连与逆序清理收在一个 async context manager 里，并可选地为每个服务器分配专属 worker task，确保连接与清理在同一 task 上执行。

## 机制是什么

`MCPServerManager` 接收 server 列表，构造时先去重，然后维护四个视图：全部服务器、当前 active、失败服务器及其错误。`active_servers` 在 `drop_failed_servers=True` 时只含成功连接者；默认 `strict=False`，所以单台 server 失败可记录后继续让其它 server 工作。`strict=True` 则在完成尝试后将失败升级为异常。

生命周期由 `connect_all`、`reconnect`、`cleanup_all` 管理；`async with` 进入时连接，退出时清理。两个超时分别默认 10 秒，可传 `None` 关闭限制；构造与后续属性赋值都校验正数有限值，零被拒绝，因为零会变成立即超时。

并行模式是重要的可选路径：`connect_in_parallel=True` 会为每台 server 创建 `_ServerWorker`，通过命令队列安排 connect 与 cleanup。它不只是并发连接，更是为了保持 task affinity——部分 MCP 传输要求初始化与清理在同一 asyncio task，否则 anyio cancel scope 会报错。

## 怎么实现的

**重复 server 去重。** `_unique_servers` 按对象哈希与相等语义保序去重，避免同一个实例重复连接或清理。`all_servers` 与 `active_servers` 等属性返回 list 快照，调用方改返回值不会直接改管理器内部列表。

**生命周期串行化。** `connect_all`、`reconnect`、`cleanup_all` 都先取 `_lifecycle_lock`。若等待锁时调用被取消，`suppress_cancelled_error=True` 默认会返回 False，外层方法不执行生命周期动作；设为 False 则向上传播取消。拿到锁后始终用 finally 释放。

**连接阶段与失败策略。** `_connect_all` 先快照旧 connected/active 状态，再清空本轮失败表。默认顺序连接逐台 `_attempt_connect`；并行则用 `_connect_all_parallel` 创建任务并 `gather(return_exceptions=True)`。普通异常记录失败；strict 决定是否抛。若连接流程自身被 BaseException 中断，会清理本轮已连及失败的服务器，再按 `drop_failed_servers` 恢复适当 active 视图后重抛。

`_record_failure` 记录错误、维护失败列表，并打经过 `get_mcp_server_log_message` 安全化的日志。成功重连后 `_attempt_connect` 从失败集合移除 server 并清除错误。

**active 视图。** `_refresh_active_servers` 在 drop 模式下由 `_connected_servers` 计算；不 drop 则仍暴露全部 server，包括失败者。后者适合调用方自己处理失败，但 `active_servers` 名称此时不代表都已连通。

**重连。** `reconnect(failed_only=True)` 默认只对失败服务器先 cleanup，再重试；设 False 则先 cleanup 全部、重置失败表，再重新连全体。两种路径最终都重新计算 active 列表。连接成功的服务器不会因 failed_only 重连而被重复连接。

**逆序清理与错误收集。** `_cleanup_all` 按 `_all_servers` 的反序逐个清理。普通清理错误记入 errors 并继续，默认也会记录被抑制的 CancelledError；设 suppress_cancelled_error=False 才立即传播取消。finally 更新 active 视图。

`_cleanup_servers` 对给定子集也采用逆序策略，逐台执行并返回成功清理的 server，便于重连只重试清理完成的服务器，避免在旧连接尚存时另起连接。

**worker 命令协议。** `_ServerCommand` 包含 action、timeout 与对应 asyncio Future。worker 的 `_submit` 将命令入队、await future；`_run` 独占执行循环，按 action 调用 `_run_with_timeout_in_task`，结束时 settle future。worker 退出时，队列里尚未执行的命令 future 全部 cancel，避免调用方永久等待。

**同 task 超时。** `_run_with_timeout_in_task` 在支持 Python 3.11 的环境使用 `asyncio.timeout`，避免 `asyncio.wait_for` 在旧版本创建新 task；旧版本则用 `loop.call_later` 取消当前 task，并区分自设超时与外部取消。目标是 connect 与 cleanup 本身不被挪到另一个 task。

**外部取消后的紧急清理。** worker 统计 task.cancel 调用次数，区分外部取消与 server 自己抛出的 CancelledError。connect 命令被外部取消时，worker 在同一 task 中按当前 manager cleanup timeout 做一次紧急 cleanup，再将取消通过 command future 传回并结束 worker。若紧急清理失败，错误记入 cleanup future，worker 留在管理器里，后续 reconnect 必须先看到该失败，不会对可能仍开着的传输再建第二条连接。

## 关键取舍

**源码事实：** 默认 `strict=False`、`drop_failed_servers=True`：失败隔离并继续工作，active 只保留连接成功者。这是「部分可用优先」的默认。

**源码事实：** 并行连接使用每台 server 一个 worker，而不是普通 gather 直接调用 server.connect。worker 带来任务管理复杂度，但保住要求连接/清理同 task 的传输约束。

**源码事实：** cleanup 默认按逆序运行，并在普通异常后继续清理后续 server。清理失败记入错误表，不让单个失败阻止其余连接释放。

**源码事实：** 清理超时与 connect 超时分开配置；外部取消触发的紧急清理使用 manager 当前 cleanup timeout，而非被中断 connect 的 timeout。

**架构推论：** manager 不将异常简化成一个总错误，而是保留每台服务器对应的 errors 与 failed_servers，使「部分成功」对调用方可见。strict 只是决定失败是否升级为 control-flow 异常，不会抹掉失败状态。

**架构推论：** worker 的取消计数看似实现细节，实际上为区分取消来源服务：服务器内部可能合法抛 CancelledError，而调用方取消代表生命周期被中断。把两者区别开，才能只对外部取消执行紧急清理。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/mcp/manager.py` | `MCPServerManager` | 多服务器连接、重试、active 视图与清理编排 |
| `src/agents/mcp/manager.py` | `MCPServerManager.connect_all` | 生命周期锁下连接全部服务器 |
| `src/agents/mcp/manager.py` | `MCPServerManager._attempt_connect` | 记录取消、失败与成功恢复状态 |
| `src/agents/mcp/manager.py` | `MCPServerManager.reconnect` | 失败子集或全体重连 |
| `src/agents/mcp/manager.py` | `MCPServerManager._cleanup_all` | 逆序清理，失败记账后继续 |
| `src/agents/mcp/manager.py` | `MCPServerManager._connect_all_parallel` | 并行启动每服务器连接任务 |
| `src/agents/mcp/manager.py` | `MCPServerManager._get_worker` | 复用或替换 server 专属 worker |
| `src/agents/mcp/manager.py` | `_ServerWorker` | 同 task 执行连接/清理命令的队列 worker |
| `src/agents/mcp/manager.py` | `_ServerCommand` | action、超时与完成 future 的命令数据 |
| `src/agents/mcp/manager.py` | `_run_with_timeout_in_task` | 在当前 task 内实施生命周期超时 |
| `src/agents/mcp/manager.py` | `_validate_lifecycle_timeout` | 拒绝零、负数与非有限超时 |

## 参考

- [openai-agents-python 源码：mcp/manager.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/mcp/manager.py)
- [openai-agents-python 源码：mcp/server.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/mcp/server.py)
- [Model Context Protocol 规范](https://modelcontextprotocol.io/introduction)

## 与主核 / AI 的关系

主核的资源管理器也要处理多实例启动失败、逆序释放与部分成功状态。MCP manager 的特别约束是「某些传输要求 connect/cleanup 在同一个 asyncio task 内」，因此它不仅是并行调度器，更是 task 所有权管理器。

与 CrewAI 的 MCP 注入路径相比，OpenAI Agents SDK 把 server 生命周期从 Agent 定义里剥离：`MCPServerManager` 可以在应用 lifespan 中连接好服务器，再把 `active_servers` 交给 Agent。这样长驻应用能复用连接，而某台 server 故障时可让其余服务器继续提供工具；代价是应用需明确管理 manager 的 async context。

对 AI 工程最实用的是取消路径上的资源所有权：外部 task 被取消时，连接可能已部分建立，但 `async with` 的退出路径未必会执行。worker 在同一 task 做紧急清理，并让 cleanup failure 留在管理器状态里，避免下一次重连在旧连接未清理时重叠建立。对于任何有异步传输与生命周期约束的 SDK，这比「取消后忽略」安全得多。