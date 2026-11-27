---
project: crewai
feature: unified-memory
title: 统一记忆入口
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/memory/unified_memory.py
    symbols: [Memory, remember, recall]
  - path: lib/crewai/src/crewai/memory/unified_memory.py
    symbols: [_submit_save, drain_writes, forget]
---

# 统一记忆入口

## 一句话职责

`Memory` 是记忆系统唯一对外的门面：写走一条「单线程串行队列 → 批量编码 Flow」的路径，读走「浅检索或自适应深检索」，两者都在同一套存储后端与同一套复合打分上。

## 机制是什么

`Memory` 是一个 Pydantic 模型，字段全是**配置**，运行时依赖（LLM 实例、嵌入器、存储后端）都藏在私有属性里并且是惰性创建的。这个分界很重要：构造 `Memory()` 不应该产生任何网络调用或磁盘写入，只有第一次真正需要时才初始化。

字段可分四组：

- **后端**：`llm`（分析用模型，默认一个模型名，可为实例）、`storage`（后端实例，或字符串——`lancedb` / `qdrant-edge` / 其他字符串当路径用 LanceDB）、`embedder`（可调用对象、provider 配置字典、或 `None` 表示默认 OpenAI 嵌入）。
- **打分权重**：`recency_weight` 0.3、`semantic_weight` 0.5、`importance_weight` 0.2、`recency_half_life_days` 30。
- **编码/合并**：`consolidation_threshold` 0.85、`consolidation_limit` 5、`default_importance` 0.5。
- **检索**：`confidence_threshold_high` 0.8、`confidence_threshold_low` 0.5、`complex_query_threshold` 0.7、`exploration_budget` 1、`query_analysis_threshold` 200。

另有两个语义开关：`read_only`（为真时 remember/remember_many 什么都不存、update 不改、recall 不刷新访问时间，但 forget/reset 仍生效）与 `root_scope`（结构根前缀，所有推断或显式的作用域都嵌在它下面）。

对外方法面很宽：`remember` / `remember_many` / `recall` / `forget` / `update` / `extract_memories` / `scope` / `slice` / `list_scopes` / `list_records` / `info` / `tree` / `list_categories` / `reset` / `reset_all`，以及一组 `a*` 异步变体（目前是同步实现的直接转发）。

## 怎么实现的

**构造期只做三件事。** `model_post_init` 把字段打包成一个内部 `MemoryConfig` 供两个 Flow 使用；把字符串形态的 llm 留空（惰性建）、把实例形态的 llm 包一层「非流式隔离」（`_non_streaming_analysis_llm` 用 `copy` 或 `model_copy` 复制一份并把 `stream` 置假，避免内部分析与用户自己的流式调用互相干扰）；按 `storage` 字符串解析后端——先问用户登记的存储工厂，再认两个内置名字，其余当路径。

**懒初始化的报错写得像文档。** `_llm` 与 `_embedder` 两个 property 都是「用到才建」，且失败时会抛一个长文案的 `RuntimeError`，逐条列出可行修法（设 API key、换模型、传实例、或干脆跳过 LLM 分析改用显式字段 + 浅检索），并附文档链接。这是有意为之的：这两个依赖都需要外部凭证，在导入期就报错会让「只想用内存做点别的」的用户直接卡住。

**写入：一条单线程队列 + 一个 Flow。** 所有写操作都经 `_submit_save`：它复制当前 contextvars，把任务提交给一个 `max_workers=1` 的线程池（`memory-save` 前缀），把 future 记进 `_pending_saves` 并挂完成回调。单线程池是刻意的：并发写同一后端容易撞版本冲突（尤其 LanceDB），串行化是最简单的解法。`_submit_save` 还有两个务实的兜底：线程池已关闭（`close()` 之后）时同步执行并返回一个已完成的 future；`_on_save_done` 回调**永不抛异常**，因为它可能在进程退出期间由线程池内部机制调用，此时事件总线可能已经关了。

`remember` 是同步语义：走同一条队列以保证串行，但立刻 `future.result()` 等结果，所以能返回 `MemoryRecord`。`remember_many` 是异步语义：提交后立即返回空列表，完成事件由后台线程发（开始与完成事件都在后台发，保证它们在事件总线的作用域栈上正确配对）。

真正的编码逻辑在 `_encode_batch`：构造一个 `EncodingFlow`（见 e04），把每个待存项包成字典作为输入 kickoff，然后从流程状态里挑出「没被丢弃且有结果记录」的项。这个「用 Flow 实现流水线」的选择让编码阶段天然拥有流程引擎的分支、并发与事件能力。

**读：浅与深两条路。** `recall` 的第一件事是 `drain_writes()` —— 读屏障，等他人在途的后台写入落完，否则刚 `remember_many` 的东西可能查不到。然后按 `root_scope` 归一作用域（没给 scope 就用 root，给了就拼在 root 下面）。

`depth="shallow"` 是直白的向量检索：嵌入查询 → 后端 `search` → 私密过滤 → 复合打分 → 排序。私密过滤有个细节值得指出：条件是「非私密，或（调用方给了 source 且记录 source 等于它）」，而不是简单的 `record.source == source`。源码注释解释了原因——后者在两边都是 `None` 时会成立，把无主私密记录泄漏给所有默认调用方。

`depth="deep"`（默认）转到 `RecallFlow`（见 e03）：LLM 蒸馏子查询、选作用域、并行搜索、按置信度决定是否再深探，最后拿回 `final_results`。

检索成功后还有一次 `touch_records`（更新访问时间），失败被忽略——注释写着「不因为刷新访问时间让整次检索失败」。`read_only` 为真时这一步跳过。

**事件与错误成对。** 写有 `MemorySaveStartedEvent` / `MemorySaveCompletedEvent` / `MemorySaveFailedEvent`，读有 `MemoryQueryStartedEvent` / `MemoryQueryCompletedEvent` / `MemoryQueryFailedEvent`，都带耗时毫秒与 `source_type="unified_memory"`。失败路径先发失败事件再重新抛——即观测不改变语义。

**作用域与视图。** `scope(path)` 返回 `MemoryScope`，`slice(scopes, ...)` 返回 `MemorySlice`（见 e02）。这两个视图本身不复制状态，只持有对同一个 `Memory` 的私有引用与一个路径前缀，所有操作转发回 `Memory` 并把路径拼好。

**深拷贝要特判。** `__deepcopy__` 手工重建：`__dict__` 与 Pydantic 元数据深拷贝，但 `_save_pool`、`_pending_lock`、`_reset_lock` 这些不可 pickle 的私有属性改为「取默认值」。这使「复制一个 Flow（含其自动创建的 Memory）」这种操作不会因为线程池而崩。同理 `close()` 会先 `drain_writes`、再关存储、最后关线程池。

## 关键取舍

**源码事实：** 写路径被强制串行（单线程池），读路径完全并行（深检索内部多线程）。这是「写少读多」假设下的资源分配，也顺带消除了并发写同一记录导致的版本冲突。

**源码事实：** `remember` 同步阻塞、`remember_many` 立即返回空列表。两者的差别不是性能优化而是语义选择：单条写入的调用方通常需要拿到记录 id 做后续引用；批量写入的调用方（例如智能体在一轮结束后倾倒记忆）不想被拖住。

**源码事实：** 读屏障放在 `recall` 开头而不是放在「写入返回前」。这等于承认「写入是最终一致的」，把一致性成本转给读者。

**源码事实：** 私密可见性用「显式 source 相等」而不是 `==` 比较。这是一处已经写进注释的安全修复，属于典型的「默认值相同导致越权」的坑。

**源码事实：** LLM 与嵌入器都是惰性创建并带完整报错指引。这让 `Memory` 能在没有凭证的环境里被构造（例如只做作用域管理或使用显式字段的写入路径），代价是「配置错了」要到第一次真正使用才暴露。

**架构推论：** `Memory` 只做门面与调度，编码、去重、合并、检索策略分别落在 `EncodingFlow` 与 `RecallFlow` 两个内部流程里。这让每个阶段都能单独测试、单独演化，也让记忆系统复用了流程引擎的能力（分支、并发、事件）。代价是「一次写入」的调用链跨越三个模块，排障需要顺着 Flow 的 kickoff 往里看。

**架构推论：** 打分是三项加权和（语义相似度、时间衰减、重要度），权重全在字段上可调。这让「记忆最近的事」与「记忆重要的事」成为同一机制的两个旋钮，而不需要两套检索策略。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/memory/unified_memory.py` | `Memory` | 记忆门面：配置字段、惰性后端、作用域口径、事件 |
| `lib/crewai/src/crewai/memory/unified_memory.py` | `remember` | 同步写入：经串行队列跑编码 Flow 并返回记录 |
| `lib/crewai/src/crewai/memory/unified_memory.py` | `recall` | 检索：读屏障、浅/深两条路径、私密过滤、访问时间刷新 |
| `lib/crewai/src/crewai/memory/unified_memory.py` | `remember_many` | 后台批量写入，立即返回 |
| `lib/crewai/src/crewai/memory/unified_memory.py` | `_submit_save` | 单线程写队列、上下文复制、池关闭时的同步兜底 |
| `lib/crewai/src/crewai/memory/unified_memory.py` | `drain_writes` | 读屏障：等全部在途写入完成且不重抛失败 |
| `lib/crewai/src/crewai/memory/unified_memory.py` | `forget` | 按作用域/分类/时间/元数据/记录 id 删除 |
| `lib/crewai/src/crewai/memory/unified_memory.py` | `_encode_batch` | 构造并驱动 `EncodingFlow`，收集未被丢弃的记录 |

## 参考

- [CrewAI 源码：unified_memory.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/memory/unified_memory.py)
- [CrewAI 文档：Memory 概念](https://docs.crewai.com/en/concepts/memory)
- [LanceDB 文档](https://lancedb.github.io/lancedb/)

## 与主核 / AI 的关系

这套「单写队列 + 读屏障」的并发模型，与主核里「单写多读 + WAL」是同一目标的不同实现。主核让写者持锁修改页并在 WAL 里留记录，读者读快照；这里让写者排成一条队逐个跑，读者在开始检索前先等队列排空。差别在于粒度：主核的读者不必等写者，靠版本号判断可见性；这里的读者直接等，代价是延迟，好处是实现极简且不需要后端支持 MVCC。

`resolve` 存储后端的那段顺序（用户工厂 → 内置名 → 当路径）与主核里「扩展存储引擎」的注册思路一致：给应用一个进程级入口，把默认后端换掉而不改动调用点。

对 AI 工程的实际意义在于：记忆系统的真正难点不在「存向量」，而在几个不上台面的地方——写入要不要阻塞调用方、并发写会不会撞后端版本、刚写的能不能立刻读到、私密记录会不会越权泄漏、没有凭证时能不能只是构造而不炸。这四条在这份实现里各占一段代码，且每段都能在注释里看到踩坑痕迹：单线程池、读屏障、`source is not None` 的显式判断、惰性初始化的长报错文案。这些细节比「用什么向量库」更能决定一套记忆系统在生产里是否可靠。
