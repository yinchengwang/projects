# -*- coding: utf-8 -*-
"""vllm 机制清单（阶段 2E 第一个项目）。

盘点范围：`vllm/` 下 6790 个 py 文件、约 148 万行（不含 tests/benchmarks/docs）。
`csrc/`（C++/CUDA kernel）、`cmake/`、`rust/`、`tools/` 不建页。

vllm 与本文库前面所有项目的差别是**它是 GPU 上跑大模型的服务引擎，不是编排框架**：

- ragflow 是「用 Go 写的完整服务端」，关心的是 HTTP 接口与业务分层；
- **vllm 是「让 GPU 把尽可能多的 token 算完」**：它的全部复杂度都压在四件事上——
  模型怎么装进显存、KV cache 怎么切块与复用、每一步批里塞多少请求、
  kernel 怎么选与怎么并行。

因此它的机制骨架是「**引擎核心（进程内）+ 客户端（跨进程）+ 执行器（多 GPU）**」
三层，加上「**注意力后端 + KV cache 管理**」这两条真正的性能命脉。

⚠️ vllm 的 v1 引擎近期做过一次大重构：老路径（`gpu_model_runner.py`、`WorkerOutput`、
`eagle.py`、`kv_offloading.py`、`serving_chat.py`）都已不存在或改名，新路径是
`v1/engine/*`、`v1/attention/backend.py`、`v1/kv_offload/base.py`、
`entrypoints/serve/*`。**读 vllm 必须先确认自己在读哪一代代码。**

源码锚点：`vllm@162277e73f2562d040c35d3677f296bc4379b3b9`。
"""

GROUP_ORDER = [
    "A-引擎核心与进程模型",
    "B-KV cache 管理",
    "C-注意力后端",
    "D-模型与算子层",
    "E-分布式与执行器",
    "F-服务入口与 API",
    "G-采样、工具与可观测",
]

MECHANISM_SPECS = [
    # ============ A-引擎核心与进程模型 ============
    {
        "feature_group": "A-引擎核心与进程模型",
        "feature": "engine-core",
        "title": "引擎核心与调度循环",
        "page_file": "a01-engine-core.md",
        "boundary": "负责引擎核心进程内的调度循环（收请求、排批次、推输出）与核心状态机；"
                    "不负责客户端传输（a02）、不负责执行器与多卡（E 组）。",
        "refs": [
            {"path": "vllm/v1/engine/core.py",
             "symbols": ["EngineCore", "EngineCoreOutput", "EngineCoreProc"]},
            {"path": "vllm/v1/engine/core.py",
             "symbols": ["EngineShutdownState", "DPEngineCoreProc"]},
        ],
    },
    {
        "feature_group": "A-引擎核心与进程模型",
        "feature": "core-client",
        "title": "核心客户端与进程传输",
        "page_file": "a02-core-client.md",
        "boundary": "负责前端到引擎核心的跨进程传输（多进程/多机/DP/负载均衡变体）；"
                    "不负责核心内部调度（a01）、不负责输入输出加工（a03/a04）。",
        "refs": [
            {"path": "vllm/v1/engine/core_client.py",
             "symbols": ["EngineCoreClient", "InprocClient", "MPClient"]},
            {"path": "vllm/v1/engine/core_client.py",
             "symbols": ["SyncMPClient", "AsyncMPClient", "DPAsyncMPClient"]},
        ],
    },
    {
        "feature_group": "A-引擎核心与进程模型",
        "feature": "input-output-processor",
        "title": "输入输出加工",
        "page_file": "a03-input-output-processor.md",
        "boundary": "负责请求的输入侧加工（tokenize、多模态）与输出侧加工（增量 detokenize、"
                    "采样结果成流）；不负责核心调度（a01）、不负责采样算法（G 组）。",
        "refs": [
            {"path": "vllm/v1/engine/input_processor.py", "symbols": ["InputProcessor"]},
            {"path": "vllm/v1/engine/output_processor.py",
             "symbols": ["OutputProcessor", "RequestState", "StreamingUpdate"]},
            {"path": "vllm/v1/engine/detokenizer.py",
             "symbols": ["IncrementalDetokenizer", "FastIncrementalDetokenizer",
                         "SlowIncrementalDetokenizer"]},
        ],
    },

    # ============ B-KV cache 管理 ============
    {
        "feature_group": "B-KV cache 管理",
        "feature": "block-pool",
        "title": "块池与前缀哈希",
        "page_file": "b01-block-pool.md",
        "boundary": "负责 KV cache 的块分配、释放与前缀复用（内容哈希去重）；"
                    "不负责单类型管理器的策略（b02）、不负责落盘卸载（b03）。",
        "refs": [
            {"path": "vllm/v1/core/block_pool.py",
             "symbols": ["BlockPool", "BlockHashToBlockMap"]},
            {"path": "vllm/v1/core/kv_cache_utils.py",
             "symbols": ["KVCacheBlock", "BlockHashListWithBlockSize", "get_block_hash"]},
        ],
    },
    {
        "feature_group": "B-KV cache 管理",
        "feature": "kv-cache-manager",
        "title": "单类型 KV cache 管理器",
        "page_file": "b02-kv-cache-manager.md",
        "boundary": "负责按注意力类型（全注意力/滑窗/线性/SSM 等）分别管理块分配；"
                    "不负责块池（b01）、不负责协调器（b04）。",
        "refs": [
            {"path": "vllm/v1/core/single_type_kv_cache_manager.py",
             "symbols": ["SingleTypeKVCacheManager", "FullAttentionManager",
                         "SlidingWindowManager"]},
            {"path": "vllm/v1/core/single_type_kv_cache_manager.py",
             "symbols": ["RSWAManager", "MambaManager", "CircularBufferManager"]},
        ],
    },
    {
        "feature_group": "B-KV cache 管理",
        "feature": "kv-offload",
        "title": "KV cache 落盘与分层",
        "page_file": "b03-kv-offload.md",
        "boundary": "负责 KV cache 的卸载/回载（本地层与远端层的分级命中）；"
                    "不负责块池（b01）、不负责分布式组（E 组）。",
        "refs": [
            {"path": "vllm/v1/kv_offload/base.py",
             "symbols": ["RequestOffloadingContext", "OffloadPolicy", "LookupResult"]},
            {"path": "vllm/v1/kv_offload/config.py",
             "symbols": ["OffloadingConfig", "OffloadingModelConfig", "OffloadingGroupConfig"]},
            {"path": "vllm/v1/kv_offload/factory.py", "symbols": ["OffloadingSpecFactory"]},
        ],
    },
    {
        "feature_group": "B-KV cache 管理",
        "feature": "kv-cache-planning",
        "title": "KV cache 容量规划",
        "page_file": "b04-kv-cache-planning.md",
        "boundary": "负责启动时按显存与模型结构算出各层 KV cache 的块数与布局；"
                    "不负责运行期分配（b02）、不负责注意力后端（C 组）。",
        "refs": [
            {"path": "vllm/v1/core/kv_cache_planning.py",
             "symbols": ["DefaultKVCacheConfigBuilder", "unify_kv_cache_spec_page_size"]},
            {"path": "vllm/v1/core/kv_cache_coordinator.py",
             "symbols": ["KVCacheCoordinator"]},
        ],
    },

    # ============ C-注意力后端 ============
    {
        "feature_group": "C-注意力后端",
        "feature": "attention-contract",
        "title": "注意力后端契约",
        "page_file": "c01-attention-contract.md",
        "boundary": "负责注意力后端的抽象契约（metadata、impl、层装配）；"
                    "不负责具体 kernel 实现（c02）、不负责模型层（D 组）。",
        "refs": [
            {"path": "vllm/v1/attention/backend.py",
             "symbols": ["AttentionBackend", "AttentionImpl", "AttentionMetadataBuilder"]},
            {"path": "vllm/v1/attention/backend.py",
             "symbols": ["AttentionMetadata", "AttentionLayer", "CommonAttentionMetadata"]},
        ],
    },
    {
        "feature_group": "C-注意力后端",
        "feature": "attention-selector",
        "title": "后端选择",
        "page_file": "c02-attention-selector.md",
        "boundary": "负责按硬件与配置挑注意力后端、并在多后端间回退；"
                    "不负责后端契约（c01）、不负责具体 kernel（c03）。",
        "refs": [
            {"path": "vllm/v1/attention/selector.py",
             "symbols": ["AttentionSelectorConfig", "get_attn_backend"]},
            {"path": "vllm/v1/attention/selector.py",
             "symbols": ["get_mamba_attn_backend", "get_attn_spec_kind"]},
        ],
    },
    {
        "feature_group": "C-注意力后端",
        "feature": "flash-attn-backend",
        "title": "FlashAttention 后端",
        "page_file": "c03-flash-attn-backend.md",
        "boundary": "负责 FlashAttention 系后端的实现与元数据构造；"
                    "不负责后端契约（c01）、不负责 MLA（c04）。",
        "refs": [
            {"path": "vllm/v1/attention/backends/flash_attn.py",
             "symbols": ["FlashAttentionBackend", "FlashAttentionMetadata"]},
        ],
    },
    {
        "feature_group": "C-注意力后端",
        "feature": "mla-backend",
        "title": "MLA 与多头潜在注意力",
        "page_file": "c04-mla-backend.md",
        "boundary": "负责 MLA（多头潜在注意力）这一 DeepSeek 系结构的后端与算子；"
                    "不负责通用注意力契约（c01）、不负责模型定义（D 组）。",
        "refs": [
            {"path": "vllm/model_executor/layers/mla.py", "symbols": ["MLAAttention"]},
            {"path": "vllm/v1/attention/backends/flashinfer.py",
             "symbols": ["FlashInferImpl"]},
        ],
    },

    # ============ D-模型与算子层 ============
    {
        "feature_group": "D-模型与算子层",
        "feature": "model-registry",
        "title": "模型注册与配置",
        "page_file": "d01-model-registry.md",
        "boundary": "负责模型类注册、配置解析与权重加载入口；"
                    "不负责具体模型实现（d02）、不负责并行（D 组并行组）。",
        "refs": [
            {"path": "vllm/config/model.py", "symbols": ["ModelConfig"]},
            {"path": "vllm/config/scheduler.py", "symbols": ["SchedulerConfig"]},
            {"path": "vllm/config/cache.py", "symbols": ["CacheConfig"]},
        ],
    },
    {
        "feature_group": "D-模型与算子层",
        "feature": "llama-model",
        "title": "Llama 模型实现",
        "page_file": "d02-llama-model.md",
        "boundary": "以 Llama 为范例讲清「模型类如何装配成可执行体」；"
                    "不负责算子库（d03）、不负责注意力后端（C 组）。",
        "refs": [
            {"path": "vllm/model_executor/models/llama.py",
             "symbols": ["LlamaForCausalLM", "LlamaModel"]},
            {"path": "vllm/model_executor/models/llama.py", "symbols": ["LlamaDecoderLayer"]},
        ],
    },
    {
        "feature_group": "D-模型与算子层",
        "feature": "parallel-layers",
        "title": "张量并行算子",
        "page_file": "d03-parallel-layers.md",
        "boundary": "负责张量并行下的线性层、词嵌入与 RoPE 的切分与聚合；"
                    "不负责分布式组（E 组）、不负责注意力（C 组）。",
        "refs": [
            {"path": "vllm/model_executor/layers/linear.py",
             "symbols": ["RowParallelLinear", "QKVParallelLinear", "MergedColumnParallelLinear"]},
            {"path": "vllm/model_executor/layers/vocab_parallel_embedding.py",
             "symbols": ["VocabParallelEmbedding", "ParallelLMHead"]},
            {"path": "vllm/model_executor/layers/rotary_embedding/__init__.py",
             "symbols": ["get_rope"]},
        ],
    },

    # ============ E-分布式与执行器 ============
    {
        "feature_group": "E-分布式与执行器",
        "feature": "parallel-state",
        "title": "并行组与通信",
        "page_file": "e01-parallel-state.md",
        "boundary": "负责张量/流水线/数据并行组的建立与集合通信原语；"
                    "不负责执行器（e02）、不负责模型层（D 组）。",
        "refs": [
            {"path": "vllm/distributed/parallel_state.py",
             "symbols": ["GroupCoordinator", "init_distributed_environment"]},
            {"path": "vllm/distributed/parallel_state.py",
             "symbols": ["all_reduce", "reduce_scatter", "suspend_device_comms"]},
        ],
    },
    {
        "feature_group": "E-分布式与执行器",
        "feature": "executor",
        "title": "执行器抽象",
        "page_file": "e02-executor.md",
        "boundary": "负责「谁来真正跑这个 batch」的执行器抽象与多进程/多机实现；"
                    "不负责核心调度（a01）、不负责并行组（e01）。",
        "refs": [
            {"path": "vllm/v1/executor/abstract.py", "symbols": ["Executor"]},
        ],
    },
    {
        "feature_group": "E-分布式与执行器",
        "feature": "worker",
        "title": "Worker 与模型执行器",
        "page_file": "e03-worker.md",
        "boundary": "负责单个 GPU worker 的模型装载与前向执行；"
                    "不负责执行器调度（e02）、不负责核心调度（a01）。",
        "refs": [
            {"path": "vllm/v1/worker/worker_base.py",
             "symbols": ["WorkerBase", "WorkerWrapperBase"]},
            {"path": "vllm/v1/worker/gpu_worker.py", "symbols": ["Worker"]},
        ],
    },

    # ============ F-服务入口与 API ============
    {
        "feature_group": "F-服务入口与 API",
        "feature": "api-server",
        "title": "HTTP 服务与路由",
        "page_file": "f01-api-server.md",
        "boundary": "负责 OpenAI 兼容 API 的进程入口、路由注册与应用状态初始化；"
                    "不负责具体 chat 请求处理（F 组其他页）、不负责引擎（a01）。",
        "refs": [
            {"path": "vllm/entrypoints/launchers/app.py", "symbols": ["build_app"]},
            {"path": "vllm/entrypoints/launchers/api_server/app_state.py",
             "symbols": ["init_app_state"]},
            {"path": "vllm/entrypoints/launchers/api_server/routers.py",
             "symbols": ["register_api_routers"]},
        ],
    },
    {
        "feature_group": "F-服务入口与 API",
        "feature": "serve-routers",
        "title": "服务路由分层",
        "page_file": "f02-serve-routers.md",
        "boundary": "负责按协议分层的路由（OpenAI / Anthropic / Cohere / 生成池等）；"
                    "不负责 API 进程入口（f01）、不负责模型侧工具解析（G 组）。",
        "refs": [
            {"path": "vllm/entrypoints/launchers/api_server/entry.py",
             "symbols": ["build_and_serve", "run_server"]},
        ],
    },

    # ============ G-采样、工具与可观测 ============
    {
        "feature_group": "G-采样、工具与可观测",
        "feature": "sampling",
        "title": "采样与结构化输出",
        "page_file": "g01-sampling.md",
        "boundary": "负责采样参数处理与结构化输出约束；"
                    "不负责核心调度（a01）、不负责工具解析（g02）。",
        "refs": [
            {"path": "vllm/v1/sample/ops/topk_topp_sampler.py",
             "symbols": ["apply_top_k_top_p", "flashinfer_sample"]},
            {"path": "vllm/v1/structured_output/backend_types.py",
             "symbols": ["StructuredOutputBackend", "StructuredOutputOptions"]},
        ],
    },
    {
        "feature_group": "G-采样、工具与可观测",
        "feature": "tool-parsers",
        "title": "工具调用解析",
        "page_file": "g02-tool-parsers.md",
        "boundary": "负责把模型输出的工具调用文本解析成结构化调用；"
                    "不负责工具执行（不属推理引擎）、不负责采样（g01）。",
        "refs": [
            {"path": "vllm/tool_parsers/__init__.py",
             "symbols": ["ToolParser", "ToolParserManager", "register_lazy_tool_parsers"]},
            {"path": "vllm/tool_parsers/abstract_tool_parser.py",
             "symbols": ["ToolParser", "ToolParserManager"]},
        ],
    },
    {
        "feature_group": "G-采样、工具与可观测",
        "feature": "metrics",
        "title": "指标与日志",
        "page_file": "g03-metrics.md",
        "boundary": "负责请求级与系统级指标的采集与日志输出；"
                    "不负责核心调度（a01）、不负责执行器（e02）。",
        "refs": [
            {"path": "vllm/v1/metrics/loggers.py", "symbols": ["LoggingStatLogger"]},
            {"path": "vllm/v1/metrics/stats.py",
             "symbols": ["SchedulerStats", "PrefixCacheStats", "RequestStateStats"]},
        ],
    },
]
