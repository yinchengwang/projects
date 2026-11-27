---
project: langgraph
page_type: overview
display_name: LangGraph
repo: https://github.com/langchain-ai/langgraph
license: MIT
language: Python
stars: 45291
stars_as_of: 2026-11-23
refs:
  - path: libs/langgraph/langgraph/graph/state.py
    symbols: [StateGraph, compile]
  - path: libs/langgraph/langgraph/pregel/main.py
    symbols: [Pregel]
  - path: libs/langgraph/langgraph/pregel/_loop.py
    symbols: [PregelLoop]
  - path: libs/langgraph/langgraph/pregel/_algo.py
    symbols: [apply_writes, prepare_next_tasks]
  - path: libs/langgraph/langgraph/pregel/_checkpoint.py
    symbols: [create_checkpoint]
---
# LangGraph

## 一句话职责

LangGraph 是一个以「共享状态的图」为模型的 agent 运行时：节点读写通道，执行器按 superstep 推进，检查点负责暂停与恢复。

## 机制是什么

LangGraph 的核心不是「让 LLM 调用链跑起来」，而是把 agent 执行变成一套可中断、可恢复、可流式观测的图计算。图由 builder 构造，编译成 Pregel 实例；状态被拆成若干通道，每个通道自带合并语义；执行按轮推进，每轮结束后落检查点。

这套设计的三个支柱互相支撑：**通道**决定了状态怎么合并，**Pregel 循环**决定了每轮跑什么，**检查点**决定了中断后从哪继续。三者缺一，agent 就只能跑一次性的直线流程。

本文库按经审阅的机制单元组织：**9 个机制领域、共 40 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 9 个领域按下面的顺序排列：A 图构建与状态模式 → B 通道与状态合并 → C Pregel 执行循环 → D 任务调度与并发 → E 检查点与恢复 → F 通道读写 → G 流式输出 → H 中断超时与校验 → I 运行时上下文。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：本清单只覆盖 `libs/langgraph` 包本体。同仓的 `libs/checkpoint*`、`libs/prebuilt`、`libs/cli`、`libs/sdk-py`、`libs/sdk-js` 各有独立包边界，属于后续增量。

## 机制导航（40 篇 / 9 组）

### A-图构建与状态模式（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-stategraph-builder.md`](a01-stategraph-builder.md) | StateGraph 构建器与状态模式注册 |
| [`a02-add-node-overloads.md`](a02-add-node-overloads.md) | add_node 的重载与节点包装 |
| [`a03-edge-and-conditional.md`](a03-edge-and-conditional.md) | 静态边与条件边登记 |
| [`a04-branch-spec-route.md`](a04-branch-spec-route.md) | 分支规格与路由求值 |

### B-通道与状态合并（6 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-channel-base.md`](b01-channel-base.md) | BaseChannel 契约与检查点协议 |
| [`b02-last-value-channel.md`](b02-last-value-channel.md) | LastValue 与结束可见语义 |
| [`b03-binop-aggregate.md`](b03-binop-aggregate.md) | 二元聚合通道与覆盖判定 |
| [`b04-topic-channel.md`](b04-topic-channel.md) | Topic 累积通道与元组展平 |
| [`b05-ephemeral-any-untracked.md`](b05-ephemeral-any-untracked.md) | 临时值、任意值与不追踪通道 |
| [`b06-barrier-and-delta-channels.md`](b06-barrier-and-delta-channels.md) | 命名屏障与增量通道 |

### C-Pregel 执行循环（7 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-pregel-object.md`](c01-pregel-object.md) | Pregel 图对象与检查点解析 |
| [`c02-node-builder.md`](c02-node-builder.md) | NodeBuilder 的订阅与写入声明 |
| [`c03-pregel-loop-tick.md`](c03-pregel-loop-tick.md) | PregelLoop 单轮步进 |
| [`c04-pending-writes.md`](c04-pending-writes.md) | 挂起写入与恢复接入 |
| [`c05-apply-writes-algo.md`](c05-apply-writes-algo.md) | 写入合并算法 |
| [`c06-prepare-next-tasks.md`](c06-prepare-next-tasks.md) | 下一批任务的推导 |
| [`c07-task-writes-protocol.md`](c07-task-writes-protocol.md) | 任务写入协议对象 |

### D-任务调度与并发（3 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-pregel-runner.md`](d01-pregel-runner.md) | PregelRunner 并发执行与提交 |
| [`d02-futures-dict.md`](d02-futures-dict.md) | 完成回调与结果表 |
| [`d03-background-executor.md`](d03-background-executor.md) | 后台执行器与异步门控 |

### E-检查点与恢复（4 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-checkpoint-shape.md`](e01-checkpoint-shape.md) | 检查点结构与快照创建 |
| [`e02-checkpoint-versions.md`](e02-checkpoint-versions.md) | 版本推进与已见标记 |
| [`e03-delta-snapshot.md`](e03-delta-snapshot.md) | 增量通道快照与溢出判定 |
| [`e04-checkpoint-replay.md`](e04-checkpoint-replay.md) | 写入重放与超时判定 |

### F-通道读写（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-channel-read.md`](f01-channel-read.md) | ChannelRead 读通道节点 |
| [`f02-pregel-node.md`](f02-pregel-node.md) | PregelNode 的读写编排 |
| [`f03-channel-write.md`](f03-channel-write.md) | ChannelWrite 写通道节点 |
| [`f04-io-mapping.md`](f04-io-mapping.md) | 输入输出与命令映射 |

### G-流式输出（4 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-stream-mux.md`](g01-stream-mux.md) | StreamMux 多路复用与推流 |
| [`g02-stream-transformers.md`](g02-stream-transformers.md) | 内置流式转换器 |
| [`g03-run-stream-driver.md`](g03-run-stream-driver.md) | 流式驱动与投影 |
| [`g04-stream-part-types.md`](g04-stream-part-types.md) | 流事件类型与负载 |

### H-中断超时与校验（4 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-interrupts-and-errors.md`](h01-interrupts-and-errors.md) | 中断、父命令与图错误 |
| [`h02-timeout-scope.md`](h02-timeout-scope.md) | 超时解析与尝试范围 |
| [`h03-tool-call-streaming.md`](h03-tool-call-streaming.md) | 工具调用流处理 |
| [`h04-graph-validation.md`](h04-graph-validation.md) | 图结构与键校验 |

### I-运行时上下文（4 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-runtime-object.md`](i01-runtime-object.md) | Runtime 与执行信息 |
| [`i02-config-accessors.md`](i02-config-accessors.md) | 配置、存储与流写入器取用 |
| [`i03-serde-allowlist.md`](i03-serde-allowlist.md) | 序列化允许清单构建 |
| [`i04-managed-values.md`](i04-managed-values.md) | 托管值与剩余步数 |

## 关键取舍

**源码事实：** 图是声明、通道是执行机制。边与节点在 builder 里只是登记，真正决定「这轮跑谁」的是通道的可用性判定。这使动态跳转（Command / Send）能绕过静态拓扑，代价是渲染图与真实执行路径可能不一致。

**源码事实：** 状态键的合并语义藏在类型注解里，不在运行期判断。默认通道是覆盖写（LastValue），任何累积需求都必须显式声明 reducer 或通道。宁可让用户多写一个注解，也不让状态静默无限增长。

**源码事实：** 检查点存的是「通道快照 + 版本 + 挂起写」三件套，恢复时靠版本推进决定谁需要重跑。增量通道把快照换成哨兵，用重放换体积，前提是 reducer 满足批量不变性。

**架构推论：** 这套结构让「中断—人工介入—恢复」成为一等能力，而不是靠外部重试模拟。代价是每轮都要序列化状态，长会话必须用增量通道或把大对象移出状态。

**架构推论：** 单线程串行推进 superstep、任务内部并发执行，是「确定性优先」的选择。它让同样的输入序列必然得到同样的状态，也因此检查点才能被可靠重放。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `libs/langgraph/langgraph/graph/state.py` | `StateGraph` | 图构建器：schema 注册、节点与边登记 |
| `libs/langgraph/langgraph/graph/state.py` | `compile` | 编译成 Pregel，含通道装配与校验 |
| `libs/langgraph/langgraph/pregel/main.py` | `Pregel` | 可执行图对象：节点表、通道表、流式模式 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `PregelLoop` | 单轮 superstep 的生命周期 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `apply_writes` | 把任务写入合并成新状态 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `prepare_next_tasks` | 由本轮写入推导下一批任务 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `create_checkpoint` | 生成检查点快照 |
| `libs/langgraph/langgraph/channels/base.py` | `BaseChannel` | 通道契约：读写与序列化协议 |

## 参考

- [LangGraph 源码仓库](https://github.com/langchain-ai/langgraph)
- [LangGraph 文档：Low Level 概念](https://langchain-ai.github.io/langgraph/concepts/low_level/)
- [LangGraph 文档：持久化与检查点](https://langchain-ai.github.io/langgraph/concepts/persistence/)
- [LangGraph 文档：流式输出](https://langchain-ai.github.io/langgraph/concepts/streaming/)

## 与主核 / AI 的关系

LangGraph 与主核（多模态数据库内核）在三个地方可以直接对照：

**状态与通道 vs 元组与页面。** 通道是状态的最小单位，元组是表的最小单位；前者按合并语义分化，后者按存储格式分化。两者都面临「并发写同一槽位」的问题：主核用行锁与 MVCC 收敛，LangGraph 用「拒绝一轮多写」把冲突前移。

**superstep vs 事务边界。** 一轮 superstep 内的写入在轮末统一合并，与事务提交时统一落盘同构。差别在于 LangGraph 的合并是纯函数式的通道更新，主核还要处理 WAL、崩溃恢复与可见性。

**检查点 vs WAL 重做。** 两者都靠「记录足够的输入」而非「记录最终结果」来恢复：检查点记通道快照与挂起写，WAL 记页面变更。增量通道的哨兵化，等价于主核里「只记改变、不记全量」。

对 AI 工程而言，这一页给出的结论是：agent 的可恢复性不是加个 try/except 就有的，它要求执行模型本身按轮推进、状态有明确合并语义、每轮有可序列化的边界。LangGraph 的 40 篇机制页，本质是在讲这三件事怎么互相约束。
