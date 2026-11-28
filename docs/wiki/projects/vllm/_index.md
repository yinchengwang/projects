---
project: vllm
page_type: overview
display_name: vLLM
repo: https://github.com/vllm-project/vllm
license: Apache-2.0
language: Python
stars: 97327
stars_as_of: 2026-11-29
refs:
  - path: vllm/v1/engine/core.py
    symbols: [EngineCore]
  - path: vllm/v1/core/block_pool.py
    symbols: [BlockPool]
  - path: vllm/v1/attention/backend.py
    symbols: [AttentionBackend]
  - path: vllm/v1/executor/abstract.py
    symbols: [Executor]
---

# vLLM

## 一句话职责

GPU 上跑大模型的高吞吐推理引擎：它把一堆等着的请求切成批、塞进显存里的 KV cache、用最快的注意力 kernel 算完再把结果吐回去，目标是**让 GPU 的每一毫秒都在算有用的 token**。

## 机制是什么

vllm 与本库前面所有 AI 项目的差别是**它不解决「怎么把 LLM 编排起来」，它解决「怎么让 GPU 算得快」**。

langchain / llama-index / haystack 关心的是「怎么把 RAG 拆成可组合的步骤」；ragflow 关心的是「怎么把一个 RAG 产品做完整」；**vllm 关心的是「一次前向里，token 怎么切、KV 怎么存、kernel 怎么选、批怎么排」**。它的用户不是应用开发者，而是**部署 GPU 集群的运维者**。

这个定位带来一个很特别的后果：**vllm 的全部复杂度都压在四个地方**——

1. **模型怎么装进显存**（权重切分、量化、kernel 选型）；
2. **KV cache 怎么切块与复用**（这是 vllm 的性能命脉，见 B 组）；
3. **每一步批里塞多少请求**（调度策略，见 A 组）；
4. **多卡怎么并行**（张量/流水线/数据并行，见 E 组）。

**其余部分（HTTP 层、OpenAI 兼容层、工具解析）都是「必须存在但不难」的周边。** 这与 ragflow 恰好相反——ragflow 的 HTTP 层是主角。

**另一个容易被低估的事实是：vllm 的性能来自「复用」，不来自「更快的算子」。** 一次 prefill 算 1000 个 token 和一次 decode 算 1 个 token，GPU 有效算力利用率差两个数量级。vllm 真正在做的是**把 decode 批得足够大、把 prefill 的结果尽可能复用**（前缀缓存）、**把批之间的空隙填满**（连续批 + 异步调度）。**算子层面它大量直接调用 FlashAttention / FlashInfer 这些现成实现**——它的贡献是调度与内存，不是 kernel 本身。

## 怎么实现的

本文库按下列顺序组织 **7 个机制域、共 22 篇机制页**：

**A 引擎核心与进程模型**（3 篇）→ **B KV cache 管理**（4 篇）→ **C 注意力后端**（4 篇）→ **D 模型与算子层**（3 篇）→ **E 分布式与执行器**（3 篇）→ **F 服务入口与 API**（2 篇）→ **G 采样、工具与可观测**（3 篇）。

这个顺序不是随意的：**A 决定「一次迭代长什么样」，B 决定「显存花在哪」，C 决定「每一步的算子怎么跑」，D 决定「模型长什么样」，E 决定「多卡怎么切」，F/G 是外围。**

**A 组讲的是「引擎的心跳」。** `EngineCore.step()` 是整个系统的主循环：排批 → 异步起执行 → 拿 grammar 位掩码 → 等结果 → 处理取消 → 回填输出。这组里最值得看的细节是**「用 GPU 等待时间填 CPU 空闲」**（`_wait_for_output` 在等 future 时继续处理输入）——**推理引擎的性能，一半来自「不让 GPU 等 CPU」**。

**B 组是 vllm 的性能命脉。** KV cache 被切成固定大小的块，块用**内容哈希**索引，于是「相同前缀」可以被不同请求共享。`BlockPool` 管分配与引用计数，`SingleTypeKVCacheManager` 按注意力类型分别管理（全注意力 / 滑窗 / 线性 / SSM / 循环缓冲各有各的复用规则），`kv_cache_planning` 在启动时按显存算出每层能分多少块，`kv_offload` 把冷块卸到磁盘或远端。**「块」这个粒度是 vllm 的核心抽象**：太细则管理开销大，太粗则浪费显存。

**C 组讲注意力后端的抽象与选择。** vllm 定义了一套 `AttentionBackend` / `AttentionImpl` / `AttentionMetadataBuilder` 契约，把 FlashAttention、FlashInfer、FlexAttention、MLA、SSM 等十几种实现统一起来，再按硬件与配置自动选择（`attention/selector.py`）。**「一套契约 + 多个后端 + 自动选择」是所有 GPU 库的标准套路，但只有 vllm 把它做得这么彻底**——因为注意力是推理里最吃显存带宽的一环，后端选错就是数量级的差距。

**D 组讲模型层。** `ModelConfig` / `SchedulerConfig` / `CacheConfig` 三份配置决定了一切；`LlamaForCausalLM` 是「一个模型怎么被装配成可执行体」的范例；`RowParallelLinear` / `QKVParallelLinear` / `VocabParallelEmbedding` 是张量并行下算子怎么切。

**E 组讲分布式。** `GroupCoordinator` 管理 TP/PP/DP 通信组与集合通信原语；`Executor` 抽象出「谁来跑这个批」；`Worker` 是单个 GPU 上的模型装载与前向执行。

**F/G 组是外围但不可省。** F 组讲 HTTP 服务与路由分层（注意：**老路径 `entrypoints/openai/api_server.py` 已成弃用转发层，真实实现搬到 `entrypoints/launchers/`**）；G 组讲采样与结构化输出、工具调用解析（`tool_parsers/__init__.py` 是一份几十个模型家族的懒加载注册表）、指标与日志。

**盘点范围**：`vllm/` 下 6790 个 py 文件、约 148 万行（不含 `tests/`、`benchmarks/`、`docs/`）。`csrc/`（C++/CUDA kernel）、`cmake/`、`rust/`、`tools/` 不建页——**那些是 kernel 本身，不是 vllm 的机制**。

## 关键取舍

**vllm 最重要的取舍是「用复杂度换 GPU 利用率」。** 连续批、前缀缓存、多种并行模式、十几种注意力后端——每一样都增加了代码量，但每一样都直接换来吞吐。**这与 ragas 的「用复杂度换可解释性」是同一类交换**：两者都清楚「简单」不是目标。

**第二个取舍是「不自己写 kernel」。** vllm 直接用 FlashAttention、FlashInfer、CUTLASS，自己只写调度与内存管理。**这让 vllm 的技术风险集中在少数几个自己写得对的地方**，但也意味着它的性能上限被上游 kernel 决定。

**第三个取舍是「进程模型」。** 引擎核心跑在独立进程，前端通过 `EngineCoreClient` 通信（有 inproc / 多进程 / DP 多种变体）。**换来的是故障隔离与 GPU 独占，代价是一次序列化的开销**。

**第四个取舍是「多后端的复杂度」。** 维护十几种注意力后端意味着每次 vllm 升级都要同步维护十几种实现。**换来的是在不同硬件上都能跑出接近最优的性能**。

**与主核（多模态数据库内核）的关系**：两者在「批处理」这个共同问题上给出两种答案。数据库的批是「共享资源访问机会的协调单位」，vllm 的批是「共享算力的分摊单位」；数据库用锁超时处理队头阻塞，vllm 用 chunked prefill 把「独占」变成「插队」；数据库的 buffer pool 按页号复用，vllm 的前缀缓存按**内容哈希**复用——后者更强（不同请求共享同一前缀），代价是要维护哈希链与引用计数。**主核若要做「相同内容去重」，就要引入 vllm 这一套。**

**与 AI 工程的关系**：vllm 的三个机制对 agent 系统直接有用。其一，**「用 GPU 等待时间填 CPU 空闲」**——agent 的工具调用、SSE 写回都该塞进模型等待窗口。其二，**KV cache 按内容哈希复用**——同一个系统提示词在所有会话里相同，前缀缓存能省掉大量 prefill。其三，**「运行集合」与「计算单元」分离**——长会话不活跃时不该一直占着模型批。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/engine/core.py` | `EngineCore` | 引擎核心与调度循环 |
| `vllm/v1/engine/core_client.py` | `EngineCoreClient` | 跨进程传输 |
| `vllm/v1/core/block_pool.py` | `BlockPool` | KV 块池与前缀哈希 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `SingleTypeKVCacheManager` | 按注意力类型管理块 |
| `vllm/v1/core/kv_cache_planning.py` | `DefaultKVCacheConfigBuilder` | 启动期容量规划 |
| `vllm/v1/attention/backend.py` | `AttentionBackend` | 注意力后端契约 |
| `vllm/v1/attention/selector.py` | `get_attn_backend` | 后端自动选择 |
| `vllm/v1/executor/abstract.py` | `Executor` | 执行器抽象 |
| `vllm/distributed/parallel_state.py` | `GroupCoordinator` | 并行组与通信 |
| `vllm/entrypoints/launchers/app.py` | `build_app` | HTTP 应用构建 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 文档：引擎架构](https://docs.vllm.ai/en/latest/design/arch_overview.html)
- [vllm 文档：性能调优与调度](https://docs.vllm.ai/en/latest/configuration/optimization.html)
- [vllm 文档：KV cache 管理](https://docs.vllm.ai/en/latest/features/automatic_prefix_caching.html)

## 与主核 / AI 的关系

vllm 与主核的共同点是**都在做资源调度**，但调度的对象与目标相反。

**调度对象相反。** 主核调度的是**页**（数据页进 buffer pool），vllm 调度的是**块**（token 块进 KV cache）。两者的分配单位都固定大小、都支持复用，但**主核复用的是「同一个页被谁访问过」，vllm 复用的是「同样的内容出现过」**。内容哈希复用更强——它让不同请求共享同一段前缀——但需要维护哈希链与引用计数，且**块的内容变了哈希就变**。

**复用粒度的代价不同。** 数据库页是 8KB 左右的固定块，浪费有限；vllm 的块大小直接决定前缀复用的粒度——太大则短前缀无法复用，太小则块表与调度开销膨胀。**这是一个「没有普适最优」的参数**，只能在具体模型与负载上调。

**空闲利用的方向相反。** 主核的空闲期用来做 checkpoint、页整理（CPU 侧工作）；vllm 的空闲期用来处理新请求（`_wait_for_output` 在等 GPU 时收请求）。**一个借 CPU，一个借 GPU**——但共同点是「**等待是不可避免的，所以要把它变成有用工作的窗口**」。

**队头阻塞的处理方式不同。** 数据库遇到锁竞争就等（可设超时）；vllm 遇到超长 prompt 就 chunked prefill（插队而非独占）。**数据库选择「等」，推理引擎选择「切」**，因为前者的等待成本是锁时间，后者的等待成本是「其他所有请求都被拖慢」。

**失败语义也不同。** 数据库的事务失败可以回滚，因为有 WAL；vllm 的请求失败只能重跑，因为模型计算没有持久化日志。**「重跑成本高」是推理引擎与数据库最本质的差异**，它决定了 agent 系统必须把模型调用设计成**幂等或可重入**的。

对 agent 工程最直接的价值有五条。其一，**「GPU 等待时间是免费的 CPU 时间」**——agent 的工具调用、SSE 推送、鉴权都该在这个窗口里做。其二，**KV cache 按内容哈希复用**——共享系统提示词是 agent 系统的常态，前缀缓存能直接省掉大部分 prefill。其三，**「运行集合」与「计算单元」分离**——长会话不该持续占用模型批。其四，**执行与采样可分离**——这是投机解码与流水线的前提，agent 做「先出草稿再验证」也需要这个结构。其五，**取消要先于输出处理**——用户中止必须立刻释放上下文与配额，不能等本步算完。
