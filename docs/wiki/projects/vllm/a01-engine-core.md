---
project: vllm
feature: engine-core
title: 引擎核心与调度循环
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/engine/core.py
    symbols: [EngineCore, EngineCoreOutput, EngineCoreProc]
  - path: vllm/v1/engine/core.py
    symbols: [EngineShutdownState, DPEngineCoreProc]
---

# 引擎核心与调度循环

## 一句话职责

在引擎核心进程里把「一堆等着的请求」变成「一批批喂给 GPU 的 token」：排批次、起异步执行、等结果、把输出分派回各请求，并处理取消、限流、缓存重置与关停。

## 机制是什么

vllm 的全部价值主张是「**把 GPU 喂满**」。要喂满，引擎核心必须同时解决三件事，而这三件事的时间尺度完全不同：

- **毫秒级**：这一批放哪些请求进去、每条请求切几个 token；
- **十毫秒级**：模型前向跑完，拿到 logits；
- **秒到分钟级**：某个请求跑完了要把它从批里摘出去、把 KV cache 还回去。

**「一个请求的生命周期」横跨三个时间尺度，而 GPU 的批必须同频**。这就是引擎核心的全部复杂度来源：它不是「调用模型」，而是「**在批里维护一堆错峰进出的请求**」。

一个具体的困境：批里有 256 个请求，其中 255 个已经跑完只剩最后一个还在生成。此时如果等最后一个，GPU 就闲了；如果不等，就得把它单独摘出来另开一批，而重开一批的代价（KV cache 搬移、CUDA graph 重新绑定）可能比空转还贵。**「等还是不等」没有普适答案，所以 vllm 把它做成了可配的调度策略**，而不是一个固定行为。

## 怎么实现的

**`step()` 是整个引擎的心跳，六步一行。** 它返回 `(outputs, model_executed)` 两个值，第二个值告诉调用方「这一步模型到底跑了没有」。

```python
def step(self) -> tuple[dict[int, EngineCoreOutputs], bool]:
    if not self.scheduler.has_requests():
        return {}, False
    scheduler_output = self.scheduler.schedule(self._should_throttle_prefills())
    if scheduler_output.total_num_scheduled_tokens > 0:
        self.num_executed_steps += 1
    future = self.model_executor.execute_model(scheduler_output, non_block=True)
    grammar_output = self.scheduler.get_grammar_bitmask(scheduler_output)
    with self.log_error_detail(scheduler_output):
        model_output = self._wait_for_output(future)
        if model_output is None:
            model_output = self.model_executor.sample_tokens(grammar_output)
    ...
```

几处值得单独说：

**`non_block=True` 是异步调度的关键。** `execute_model` 立刻返回一个 `Future`，核心随后去拿 grammar 位掩码（结构化输出的约束位图，b 组/g 组会展开）。**「先把约束算好，再等结果」这个顺序不是随意的**——grammar 位掩码必须和本次采样的 logits 对齐，若在模型执行期间才去算，就可能算到下一批的约束上。

**`model_output is None` 是一条独立路径。** 异步调度下，模型执行与采样可以分开：某些场景（流水线并行、投机解码）只返回 draft token 而不在本步采样，此时 `sample_tokens` 才真正做采样。**把「执行」与「采样」拆成两个可独立触发的动作**，是 vllm 支持多种推理策略的结构前提。

**`_wait_for_output` 在等结果的同时处理输入。** 它的 docstring 写着「Wait for a step's model output, meanwhile handling inputs if possible」。这是**用 GPU 的等待时间填 CPU 的空闲时间**：等待 GPU 跑前向的十几毫秒里，核心可以先把新到达的请求 tokenize 好、送进调度器。**GPU 同步点被当成「免费的 CPU 时间」来用**，这是所有推理引擎都会做但很少写在文档里的优化。

**`_should_throttle_prefills` 是限流而非公平。** 它的 docstring 说：「PP, with a free batch slot: wait on the oldest batch, but return True as soon as a request is added」。逻辑是 `interval > 1 and self.num_executed_steps % interval != 0`——**流水线并行下，每隔若干步才允许排一次 prefill**，否则 decode 请求会被源源不断的 prefill 挤掉（prefill 计算量大但只产出 1 个 token，decode 计算量小但每步产出一个 token）。

**这个取舍的本质是「prefill 与 decode 谁优先」**。prefill 一次性消化一个长 prompt，decode 逐 token 推进已开始的请求。如果放任 prefill 无限优先，已开始的请求会饿死；如果 decode 无限优先，新请求又进不来。vllm 用「间隔」而不是「配额」来调节，代价是**延迟不可预测**——用户看到的是「有时首 token 快，有时慢」，而不是稳定的排队策略。

**`_process_aborts_queue` 在处理模型输出之前执行。** 注释写得很清楚：「Before processing the model output, process any aborts that happened during the model execution.」**取消请求是「运行中发生的事」，而模型输出是「运行前就定下的事」**——先处理取消，才能保证本步就结束的请求不会白白占着 KV cache。

**`EngineCoreProc` 与 `DPEngineCoreProc` 是「引擎核心」的进程化形态。** 前者是常规形态，后者是数据并行形态——DP 下多个 rank 各有核心，需要对齐某些决策。代码里有 `EngineCoreActorMixin` 与 `DPMoEEngineCoreActor`，说明 DP 引擎核心里还嵌了 MoE 相关的 actor。

**`EngineShutdownState` 说明关停不是一个布尔量。** 大模型推理的关停要处理：正在跑的请求（要不要等）、GPU 显存（要不要释放）、子进程（会不会变孤儿）、分布式组（会不会挂住）。**「关停中」被建模成一个显式状态**，而不是 `if shutting_down: return`，这让关停路径可以与正常路径共存。

**缓存重置是四个独立入口**：`reset_mm_cache`（多模态）、`reset_prefix_cache`（前缀缓存）、`reset_encoder_cache`（编码器缓存）、以及 `_reset_caches` 与 `_finish_pause(clear_cache, reset_connector)`。**分开是因为它们的代价与语义不同**：清前缀缓存会让所有已缓存的 token 重新计算（很贵），清编码器缓存只影响多模态请求。

## 关键取舍

**源码事实：** `step()` 依次做「有请求检查 → `scheduler.schedule()` → `execute_model(non_block=True)` → `get_grammar_bitmask()` → 等输出 → 处理 abort 队列 → `update_from_output()`」，返回 `(outputs, model_executed)`。

**源码事实：** `model_output is None` 时走独立的 `model_executor.sample_tokens(grammar_output)` 路径——执行与采样是可分离的两个动作。

**源码事实：** `_wait_for_output` 在等待 GPU 结果期间调用 `_handle_inputs_while_waiting` 处理输入；`_handle_inputs_while_waiting` 的基类实现直接返回 `False`。

**源码事实：** `_should_throttle_prefills` 判据是 `interval > 1 and num_executed_steps % interval != 0`，注释说明这是流水线并行下「有空闲批次槽」时的行为。

**源码事实：** `_process_aborts_queue` 在 `update_from_output` 之前调用，注释写明「处理模型执行期间发生的 abort」。

**源码事实：** 引擎核心有 `EngineCoreProc` 与 `DPEngineCoreProc` 两种进程形态，DP 形态带 `EngineCoreActorMixin` / `DPMoEEngineCoreActor`。

**源码事实：** 缓存重置分成 `reset_mm_cache` / `reset_prefix_cache` / `reset_encoder_cache` 三条独立路径，另有 `_reset_caches` 与 `_finish_pause(clear_cache, reset_connector)`。

**架构推论：** 「执行与采样可分离」是支持投机解码、流水线并行与异步调度的**共同前提**。如果一步里必须「执行完立刻采样」，那投机解码的 draft 验证就无法推迟到下一步，流水线的 stage 边界也无法与采样解耦。**这条拆分让同一套调度循环能套多种推理策略**，代价是核心里出现了「`model_output is None` 说明什么」这类需要读注释才懂的分支。

**架构推论：** 用 GPU 等待时间填 CPU 空闲，是**把同步点当成资源**的典型做法。它的价值随模型变小而变大——模型越小，GPU 等待占比越高，这个优化的相对收益越高。代价是**等待逻辑与调度逻辑耦合**：`step()` 里出现了「一边等结果一边收新请求」的控制流，而不是干净的「提交 → 等待 → 处理」三段式。

**架构推论：** prefill 限流用「间隔」而非「配额」，是因为**调度器看不到未来**——它不知道下一个请求有多长。用配额（给 prefill 多少 token 上限）可以精确控制，但这会让 prefill 请求被切成多步、频繁切换批形态。**「用简单规则换批形态稳定」是这里的选择**：牺牲一点公平性，换取每个批的形状更规整。

**架构推论：** 取消先于输出处理这个顺序，是一个**因果链的正确排序**：取消是「运行期间的新事实」，输出是「运行前的旧事实」，先处理新事实才能让本步就该结束的请求立即释放 KV cache。**这类顺序错误不会报错、只会让资源被多占几毫秒**，所以必须靠注释把它钉住。

对 agent 工程最直接的价值有四条。其一：**「维护一堆错峰进出的请求」才是推理引擎的本体**，任何「把请求塞进模型跑一次」的原型都缺这一层。其二：**GPU 同步点是免费的 CPU 时间**，这个视角能直接用在 agent 的工具调用等待上。其三：**执行与采样分离**是支持投机解码与流水线的前提，也是 agent 做「先出草稿再验证」的结构依据。其四：**取消必须先于输出处理**，否则已取消的请求会占着 KV cache 到本步结束。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/engine/core.py` | `EngineCore` | 引擎核心（调度循环主体） |
| `vllm/v1/engine/core.py` | `step` | 单步：排批 → 执行 → 等输出 → 分派 |
| `vllm/v1/engine/core.py` | `_should_throttle_prefills` | prefill 限流判据 |
| `vllm/v1/engine/core.py` | `_wait_for_output` | 等 GPU 结果时并行处理输入 |
| `vllm/v1/engine/core.py` | `_process_aborts_queue` | 输出处理前的取消队列 |
| `vllm/v1/engine/core.py` | `EngineCoreProc` | 引擎核心的进程化形态 |
| `vllm/v1/engine/core.py` | `DPEngineCoreProc` | 数据并行形态 |
| `vllm/v1/engine/core.py` | `EngineShutdownState` | 关停状态机 |
| `vllm/v1/engine/core.py` | `reset_prefix_cache` | 前缀缓存重置 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 文档：引擎架构](https://docs.vllm.ai/en/latest/design/arch_overview.html)
- [vllm 文档：性能调优与调度](https://docs.vllm.ai/en/latest/configuration/optimization.html)
- [vllm 源码：vllm/v1/engine/core.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/engine/core.py)

## 与主核 / AI 的关系

vllm 的引擎核心与主核（多模态数据库内核）在**「批处理与资源调度」这个共同问题上，给出了两种答案**，而且它们面对的其实是同一个问题的两半。

**批调度 vs 事务调度。** 数据库的调度单位是「事务」，要保证的是**隔离性**——同一批里的事务互不可见。vllm 的调度单位是「请求」，要保证的是**公平性**——同一批里的请求都在推进。差别在于：数据库的批是**串行执行但逻辑并行**（一个事务的中间结果对别的不可见），vllm 的批是**真并行执行**（一个请求的 KV cache 对别的可见但互不干扰）。

**「同一批里不同生命周期的请求」vs 数据库的「同一事务里多语句」。** 数据库在一个事务里执行多条语句，遇到锁等待会挂起整个事务；vllm 在一个批里执行多个请求，长请求（长 prompt 的 prefill）会占住更多算力。**两者的「队头阻塞」问题同构**，但解法不同：数据库用锁超时 + 回滚，vllm 用「prefill 限流」把长请求的份额压下来。

**KV cache 复用 vs 数据库的 buffer pool 复用。** 主核的 buffer pool 缓存的是**数据页**，命中靠页号；vllm 的前缀缓存命中靠**内容哈希**（把 prompt 的 token 前缀哈希成块标识）。这是本质差别：**数据页的复用是「同一页被谁访问过」，内容哈希的复用是「同样的内容出现过」**。后者更强（不同请求共享同一前缀），代价是要维护哈希链与引用计数。**主核若要做「相同内容去重」，就要引入 vllm 这一套**——这是多模态存储的一个自然延伸点。

**关停状态机 vs 数据库的 WAL 恢复。** 数据库崩了靠 WAL 重放恢复；vllm 关停靠 `EngineShutdownState` 显式处理。前者假设「进程可能突然死」，后者假设「进程被优雅停止」。**推理引擎的关停语义更接近数据库的「正常关闭」而非「崩溃恢复」**——因为重跑一个 LLM 请求的成本远高于重放一条 WAL。

**「GPU 等待时间填 CPU 空闲」vs 数据库的「IO 等待时间填 CPU 空闲」。** 数据库的 `prefetch`、vllm 的 `_handle_inputs_while_waiting` 是同一个手法：**把不可避免的等待变成有用工作的窗口**。差别在于数据库等的是磁盘（延迟可预测、有队列），vllm 等的是 GPU 计算（延迟随 batch 形状变化）。**等待时间越不可预测，这个优化越难做**——所以 vllm 里 `_handle_inputs_while_waiting` 的基类实现直接返回 `False`（什么都不做），只有特定场景才覆盖它。

对 agent 工程最直接的价值有五条。其一：**agent 的「模型调用 + 工具调用」本身就是一个批调度问题**，多个并发会话时，agent 引擎要处理同样的「错峰进出的请求」。其二：**等 LLM 的时候是免费的 CPU 时间**，可以拿来准备工具参数、跑鉴权、写状态。其三：**执行与采样分离**可以直接支撑 agent 的「先出结构化草稿，再验证」策略。其四：**KV cache 按内容哈希复用**是 agent 共享系统提示词场景下的天然加速点——同一个系统提示在所有会话里都相同，按前缀哈希复用能省掉大量 prefill。其五：**取消要先于输出处理**，agent 里的「用户中止」必须立刻释放上下文与配额。
