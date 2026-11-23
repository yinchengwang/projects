---
project: postgres
feature: parallel-query-dsm
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/parallel.c
    symbols: [CreateParallelContext, InitializeParallelDSM, LaunchParallelWorkers]
---

# 并行查询 DSM 基础设施

## 一句话职责

并行上下文把 worker 所需的共享状态打包到动态共享内存中，并协调其启动与回收。

## 机制是什么

本页聚焦 `parallel.c` 中的通用并行上下文与 DSM 基础设施：创建 `ParallelContext`，估算并分配 DSM 段，以带键目录（TOC）组织状态，序列化 leader 上下文，并请求启动登记的 worker 入口。worker 可通过句柄附加同一共享段，读取状态块或与 leader 使用共享通信结构。

它不是查询执行器节点、计划并行度决策或 Gather/Gather Merge 的结果汇集协议；这些由执行器层机制处理。本基础设施也不把任意后端私有指针变成跨进程有效指针，数据必须明确序列化或放入共享段。DSM 分配失败或没有请求 worker 时，初始化代码可在本地内存建立供 leader 使用的基础 TOC，并将 worker 数设为零；该 TOC 仅保证基础设施及 leader bookkeeping 可用，不保证调用方所需共享状态都能单进程使用。worker 注册/启动不足时，具体 caller 决定是否、以及如何退化；调用方不能假定并行执行必然发生。

## 怎么实现的

`CreateParallelContext` 要求已进入并行模式，将上下文放入顶层事务上下文，记录子事务归属、worker 数、库名和函数名，并初始化 `shm_toc` 空间估算器。此生命周期约束让错误恢复与子事务结束时能识别需要清理的上下文。

`InitializeParallelDSM` 汇总固定状态、错误队列和各子系统共享状态所需空间，包括用户/角色、时间戳、快照、事务状态、GUC、已加载库信息、会话 DSM 句柄及若干目录或同步状态。它先估算 chunk 与 key 数，再尝试 `dsm_create`；成功时将 TOC 建在 DSM 地址上，失败或无需 worker 时则使用 backend-private memory，且将 worker 数降为零。

共享对象通过稳定的键插入 `shm_toc`，消费者按键查找，不依赖分配顺序。固定结构保存 leader 进程信息和原子 WAL 末端等；非固定部分按各子系统序列化接口复制，不能直接跨进程传递函数指针，因此入口以库名和函数名表示。每个 worker 有独立错误消息队列，使用 `shm_mq`；注释限定它传递错误/通知协议消息，不应当作任意大型 tuple 流。

`LaunchParallelWorkers` 依据上下文准备情况注册并启动 worker，并记录实际启动数；并行 worker 入口附加 DSM、从 TOC 恢复状态并调用所指定函数。上下文销毁时要等待或处理 worker 退出，释放 DSM attach/segment 与本地 bookkeeping。跨进程共享的生命周期必须覆盖所有 worker 使用期，具体 worker 可用性还受系统资源与启动限制影响。

## 关键取舍

**源码事实：**`InitializeParallelDSM` 在三种情况下把 `pcxt->nworkers` 直接置 0 而不是报错：当前进程无法处理中断（`INTERRUPTS_CAN_BE_PROCESSED()` 为假）、拿不到 per-session DSM segment handle（`session_dsm_handle == DSM_HANDLE_INVALID`）、以及 `dsm_create` 返回 NULL（已达 `max_dynamic_shared_memory` 上限）。第三种情况下它把 `private_memory` 分配在 `TopMemoryContext` 并用 `shm_toc_create` 在这块进程私有内存上建同一个 TOC，于是**整个并行上下文的 TOC 结构不变，只是没有 worker 可启动**。**源码事实：**`LaunchParallelWorkers` 的注释明确要求调用方必须能容忍实际启动数少于请求数（worker 注册失败或 slot 不足时不报错，只记 `nworkers_launched`）。**架构推论：**DSM 加 TOC 以显式分块和键值定位换取跨进程共享与可扩展的状态组合，代价是空间预估、序列化与严格生命周期管理；若直接复制 leader 地址空间，普通进程模型下指针、锁与资源所有权都不可移植；若把所有状态塞进一个固定 C 结构，扩展新子系统就会造成结构耦合。这里选择「降级为 0 worker 但保留 TOC」而不是直接失败，是把「能否并行」降级成性能问题而非正确性问题；代价是上层必须自己判断`nworkers_launched == 0` 并继续按串行路径执行，否则会拿到一个没有 worker 的计划。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/access/transam/parallel.c` | `CreateParallelContext` | 建立事务作用域内的并行上下文 |
| `src/backend/access/transam/parallel.c` | `InitializeParallelDSM` | 估算、创建 DSM 并装配 TOC 状态块 |
| `src/backend/access/transam/parallel.c` | `LaunchParallelWorkers` | 请求启动 worker 并收集实际启动情况 |
| `src/backend/access/transam/parallel.c` | `DestroyParallelContext` | 协调上下文及共享资源回收 |

## 参考

- [PostgreSQL 官方文档：动态共享内存配置](https://www.postgresql.org/docs/current/runtime-config-resource.html#GUC-MAX-DYNAMIC-SHARED-MEMORY-TYPES)
- [PostgreSQL 官方文档：并行计划](https://www.postgresql.org/docs/current/how-parallel-query-works.html)

## 与主核 / AI 的关系

DSM 是主核进程间共享状态的运行时基础，不代表共享内容具有持久性或自动同步语义。AI 解释时应将状态序列化、DSM/TOC 的存储、worker 启停与执行器结果合并分层，不要把本篇扩大为 Gather 执行语义。
