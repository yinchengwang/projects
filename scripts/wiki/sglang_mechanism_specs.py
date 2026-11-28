# -*- coding: utf-8 -*-
"""sglang 机制清单（阶段 2E 第二个项目）。

盘点范围：`python/sglang/` 下约 9741 个 py 文件、195 万行（不含 tests/scripts）。
另有 `python/sglang/kernels/`（387k 行，CUDA/Triton kernel）与
`python/sglang/multimodal_gen/`（391k 行，多模态生成模型）——后两块归到
D 组与「多模态」页，但只讲机制，不逐 kernel 展开。

sglang 与 vllm 的差别是这个项目的全部信息量所在：

- **vllm 的组织轴是「引擎 → 调度 → 执行器 → GPU」**（进程为核心），前端的
  OpenAI 兼容层薄得像一层膜；
- **sglang 的组织轴是「进程 → 调度器 → 调度策略 → 批 → 前向」**，而且它的
  **tokenizer 管理器是独立进程**，detokenizer 又是另一个独立进程。

sglang 的结构选择是：**把「文本」与「token」之间那层也拆成进程**。
这是它与 vllm 最不一样的地方，也是它多模态能力强、但部署复杂度高的根源。

⚠️ sglang 近期做过大规模重构：老路径（`managers/schedule_batch.py` 之外的
`mem_cache/allocator.py`、`function_call/function_call.py`、`observability/otel.py`、
`speculative/eagle_worker.py`、`model_executor/weight_utils.py`）都已改名或搬家。
**读 sglang 必须先确认自己在读哪一代代码。**

源码锚点：`sglang@7c483341e81931398d33923b7334a749b0f0b37d`。
"""

GROUP_ORDER = [
    "A-进程与调度骨架",
    "B-调度策略与批构造",
    "C-前缀缓存与内存池",
    "D-模型执行与权重加载",
    "E-注意力与算子后端",
    "F-分布式与数据并行",
    "G-服务入口与 API",
]

MECHANISM_SPECS = [
    # ============ A-进程与调度骨架 ============
    {
        "feature_group": "A-进程与调度骨架",
        "feature": "engine",
        "title": "引擎门面与多进程拉起",
        "page_file": "a01-engine.md",
        "boundary": "负责 Engine 门面：启动调度器/分词器/detokenizer 等子进程、建立连接、"
                    "对外提供 generate 接口；不负责调度策略（B 组）、不负责缓存（C 组）。",
        "refs": [
            {"path": "python/sglang/srt/entrypoints/engine.py",
             "symbols": ["Engine", "SchedulerInitResult"]},
        ],
    },
    {
        "feature_group": "A-进程与调度骨架",
        "feature": "scheduler",
        "title": "调度器主循环",
        "page_file": "a02-scheduler.md",
        "boundary": "负责调度器进程的主循环（收请求、排批、推输出、事件广播）；"
                    "不负责策略选择（B 组）、不负责批的具体字段（B 组）、不负责缓存（C 组）。",
        "refs": [
            {"path": "python/sglang/srt/managers/scheduler.py", "symbols": ["Scheduler"]},
        ],
    },
    {
        "feature_group": "A-进程与调度骨架",
        "feature": "tokenizer-manager",
        "title": "分词器管理器",
        "page_file": "a03-tokenizer-manager.md",
        "boundary": "负责独立进程里的分词、请求状态跟踪与多模态输入广播；"
                    "不负责 detokenize（a04）、不负责调度（a02）。",
        "refs": [
            {"path": "python/sglang/srt/managers/tokenizer_manager.py",
             "symbols": ["TokenizerManager", "ReqState"]},
            {"path": "python/sglang/srt/managers/multi_tokenizer_mixin.py",
             "symbols": ["MultiTokenizerRouter", "MultiDetokenizerRouter"]},
        ],
    },
    {
        "feature_group": "A-进程与调度骨架",
        "feature": "detokenizer",
        "title": "增量 detokenize",
        "page_file": "a04-detokenizer.md",
        "boundary": "负责把 token 流还原成文本流（增量解码、跳过特殊 token）；"
                    "不负责分词（a03）、不负责调度（a02）。",
        "refs": [
            {"path": "python/sglang/srt/managers/detokenizer_manager.py",
             "symbols": ["DetokenizerManager"]},
            {"path": "python/sglang/srt/managers/incremental_detokenize.py",
             "symbols": ["DetokenizeState", "DetokenizeStateTable", "LimitedCapacityDict"]},
        ],
    },

    # ============ B-调度策略与批构造 ============
    {
        "feature_group": "B-调度策略与批构造",
        "feature": "schedule-policy",
        "title": "调度策略",
        "page_file": "b01-schedule-policy.md",
        "boundary": "负责「先服务谁」的策略（缓存感知、prefill/decode 混合、准入）；"
                    "不负责批的字段（B 组其他页）、不负责缓存实现（C 组）。",
        "refs": [
            {"path": "python/sglang/srt/managers/schedule_policy.py",
             "symbols": ["SchedulePolicy", "CacheAwarePolicy", "CacheAgnosticPolicy"]},
            {"path": "python/sglang/srt/managers/schedule_policy.py",
             "symbols": ["PrefillAdder", "AddReqResult"]},
        ],
    },
    {
        "feature_group": "B-调度策略与批构造",
        "feature": "schedule-batch",
        "title": "批对象",
        "page_file": "b02-schedule-batch.md",
        "boundary": "负责批对象本身的结构（请求数组、token 数组、位置、KV 索引映射）；"
                    "不负责策略（b01）、不负责内存池（C 组）。",
        "refs": [
            {"path": "python/sglang/srt/managers/schedule_batch.py",
             "symbols": ["ScheduleBatch", "Req"]},
            {"path": "python/sglang/srt/managers/schedule_batch.py",
             "symbols": ["ReqKvInfo", "NextBatchPlan"]},
        ],
    },
    {
        "feature_group": "B-调度策略与批构造",
        "feature": "io-struct",
        "title": "进程间消息结构",
        "page_file": "b03-io-struct.md",
        "boundary": "负责 tokenizer/scheduler/detokenizer 三个进程之间传递的消息结构；"
                    "不负责批本身（b02）、不负责调度（a02）。",
        "refs": [
            {"path": "python/sglang/srt/managers/io_struct.py",
             "symbols": ["BaseReq", "BaseBatchReq", "SessionParams"]},
            {"path": "python/sglang/srt/managers/io_struct.py",
             "symbols": ["BeamSearchOutput", "MMInputsProcessError"]},
        ],
    },

    # ============ C-前缀缓存与内存池 ============
    {
        "feature_group": "C-前缀缓存与内存池",
        "feature": "radix-cache",
        "title": "基数树前缀缓存",
        "page_file": "c01-radix-cache.md",
        "boundary": "负责用基数树组织 token 前缀以复用 KV cache；"
                    "不负责内存池的物理分配（C 组其他页）、不负责淘汰策略（c03）。",
        "refs": [
            {"path": "python/sglang/srt/mem_cache/radix_cache.py",
             "symbols": ["RadixCache", "TreeNode", "RadixKey"]},
        ],
    },
    {
        "feature_group": "C-前缀缓存与内存池",
        "feature": "base-prefix-cache",
        "title": "前缀缓存抽象与接口",
        "page_file": "c02-base-prefix-cache.md",
        "boundary": "负责前缀缓存的接口契约（匹配、插入、释放的参数与结果对象）；"
                    "不负责具体实现（c01）、不负责物理池（c04）。",
        "refs": [
            {"path": "python/sglang/srt/mem_cache/base_prefix_cache.py",
             "symbols": ["PrefixCacheTrait", "MatchPrefixParams", "InsertParams"]},
            {"path": "python/sglang/srt/mem_cache/base_prefix_cache.py",
             "symbols": ["InsertResult", "CacheRequestOutcome", "CacheRequestHandle"]},
        ],
    },
    {
        "feature_group": "C-前缀缓存与内存池",
        "feature": "evict-policy",
        "title": "淘汰策略",
        "page_file": "c03-evict-policy.md",
        "boundary": "负责内存池满时按什么规则淘汰缓存条目；"
                    "不负责前缀树（c01）、不负责分配器（c04）。",
        "refs": [
            {"path": "python/sglang/srt/mem_cache/evict_policy.py",
             "symbols": ["EvictionStrategy", "LRUStrategy", "LFUStrategy"]},
            {"path": "python/sglang/srt/mem_cache/evict_policy.py",
             "symbols": ["FIFOStrategy", "MRUStrategy", "FILOStrategy"]},
        ],
    },
    {
        "feature_group": "C-前缀缓存与内存池",
        "feature": "memory-pool",
        "title": "KV 内存池与分配",
        "page_file": "c04-memory-pool.md",
        "boundary": "负责 KV cache 的物理显存池、token 到槽位的映射与分配；"
                    "不负责前缀树（c01）、不负责淘汰（c03）。",
        "refs": [
            {"path": "python/sglang/srt/mem_cache/memory_pool.py",
             "symbols": ["ReqToTokenPool", "KVCache", "MambaPool"]},
            {"path": "python/sglang/srt/mem_cache/memory_pool.py",
             "symbols": ["HybridReqToTokenPool", "KVWriteLoc", "KvBufferDesc"]},
        ],
    },

    # ============ D-模型执行与权重加载 ============
    {
        "feature_group": "D-模型执行与权重加载",
        "feature": "model-runner",
        "title": "模型执行器",
        "page_file": "d01-model-runner.md",
        "boundary": "负责模型前向的执行（装载、图捕获、采样前准备）；"
                    "不负责批字段（b02）、不负责注意力后端（E 组）。",
        "refs": [
            {"path": "python/sglang/srt/model_executor/model_runner.py",
             "symbols": ["ModelRunner", "ModelRunnerOutput"]},
            {"path": "python/sglang/srt/model_executor/forward_batch_info.py",
             "symbols": ["ForwardBatch"]},
        ],
    },
    {
        "feature_group": "D-模型执行与权重加载",
        "feature": "model-loader",
        "title": "权重加载",
        "page_file": "d02-model-loader.md",
        "boundary": "负责把权重从磁盘/HF 格式读进显存并按层分配；"
                    "不负责前向执行（d01）、不负责量化 kernel（E 组）。",
        "refs": [
            {"path": "python/sglang/srt/model_loader/loader.py",
             "symbols": ["get_model_loader", "DefaultModelLoader"]},
            {"path": "python/sglang/srt/model_loader/loader.py",
             "symbols": ["BaseModelLoader", "LayeredModelLoader", "ShardedStateLoader"]},
        ],
    },
    {
        "feature_group": "D-模型执行与权重加载",
        "feature": "cuda-graph",
        "title": "CUDA 图捕获",
        "page_file": "d03-cuda-graph.md",
        "boundary": "负责用 CUDA graph 消除每步的 kernel launch 开销；"
                    "不负责前向语义（d01）、不负责注意力后端（E 组）。",
        "refs": [
            {"path": "python/sglang/srt/model_executor/cuda_graph_config.py",
             "symbols": ["CudaGraphConfig", "Phase", "Backend"]},
            {"path": "python/sglang/srt/model_executor/cuda_graph_buffer_registry.py",
             "symbols": ["CudaGraphBufferRegistry", "GraphSlot", "PaddingPolicy"]},
        ],
    },

    # ============ E-注意力与算子后端 ============
    {
        "feature_group": "E-注意力与算子后端",
        "feature": "attn-backend-contract",
        "title": "注意力后端契约",
        "page_file": "e01-attn-backend-contract.md",
        "boundary": "负责注意力后端的抽象契约与注册机制；"
                    "不负责具体 kernel（e02）、不负责模型层（D 组）。",
        "refs": [
            {"path": "python/sglang/srt/layers/attention/base_attn_backend.py",
             "symbols": ["AttentionBackend", "SharedReadEnds"]},
            {"path": "python/sglang/srt/layers/attention/attention_registry.py",
             "symbols": ["ATTENTION_BACKENDS", "OOT_LINEAR_ATTENTION_BACKENDS"]},
        ],
    },
    {
        "feature_group": "E-注意力与算子后端",
        "feature": "flashinfer-backend",
        "title": "FlashInfer 后端",
        "page_file": "e02-flashinfer-backend.md",
        "boundary": "负责 FlashInfer 注意力后端的实现与元数据构造；"
                    "不负责契约（e01）、不负责 MoE（e04）。",
        "refs": [
            {"path": "python/sglang/srt/layers/attention/flashinfer_backend.py",
             "symbols": ["FlashInferAttnBackend", "FlashInferIndicesUpdaterDecode"]},
            {"path": "python/sglang/srt/layers/attention/flashinfer_backend.py",
             "symbols": ["PrefillMetadata", "DecodeMetadata", "WrapperDispatch"]},
        ],
    },
    {
        "feature_group": "E-注意力与算子后端",
        "feature": "flashattention-backend",
        "title": "FlashAttention 后端",
        "page_file": "e03-flashattention-backend.md",
        "boundary": "负责 FlashAttention 后端与多步（multi-step）变体；"
                    "不负责契约（e01）、不负责 FlashInfer（e02）。",
        "refs": [
            {"path": "python/sglang/srt/layers/attention/flashattention_backend.py",
             "symbols": ["FlashAttentionBackend", "FlashAttentionMetadata"]},
            {"path": "python/sglang/srt/layers/attention/flashattention_backend.py",
             "symbols": ["FlashAttentionMultiStepBackend"]},
        ],
    },
    {
        "feature_group": "E-注意力与算子后端",
        "feature": "moe-kernels",
        "title": "MoE 算子与路由",
        "page_file": "e04-moe-kernels.md",
        "boundary": "负责混合专家模型的路由、分组与专家计算 kernel；"
                    "不负责注意力（E 组其他页）、不负责权重装载（d02）。",
        "refs": [
            {"path": "python/sglang/srt/layers/moe/fused_moe_triton/layer.py",
             "symbols": ["FusedMoE"]},
            {"path": "python/sglang/srt/layers/moe/topk.py",
             "symbols": ["select_experts", "TopK", "TopKConfig"]},
            {"path": "python/sglang/srt/layers/moe/moe_runner/triton_utils/moe_align_block_size.py",
             "symbols": ["moe_align_block_size"]},
        ],
    },

    # ============ F-分布式与数据并行 ============
    {
        "feature_group": "F-分布式与数据并行",
        "feature": "tp-worker",
        "title": "张量并行 worker",
        "page_file": "f01-tp-worker.md",
        "boundary": "负责张量并行下 worker 的模型切分与前向执行；"
                    "不负责数据并行（f02）、不负责模型执行器（D 组）。",
        "refs": [
            {"path": "python/sglang/srt/managers/tp_worker.py",
             "symbols": ["TpModelWorker", "BaseTpWorker"]},
        ],
    },
    {
        "feature_group": "F-分布式与数据并行",
        "feature": "dp-controller",
        "title": "数据并行控制",
        "page_file": "f02-dp-controller.md",
        "boundary": "负责多机多卡的数据并行编组、负载均衡与故障切换；"
                    "不负责张量并行（f01）、不负责调度策略（B 组）。",
        "refs": [
            {"path": "python/sglang/srt/managers/data_parallel_controller.py",
             "symbols": ["DataParallelController", "LoadBalanceMethod"]},
            {"path": "python/sglang/srt/managers/data_parallel_controller.py",
             "symbols": ["WorkerPortHandshake", "DPRoutingUnavailable", "DPBudget"]},
        ],
    },
    {
        "feature_group": "F-分布式与数据并行",
        "feature": "disaggregation",
        "title": "PD 分离",
        "page_file": "f03-disaggregation.md",
        "boundary": "负责 prefill/decode 分离部署（两个角色互传 KV）；"
                    "不负责单机调度（a02）、不负责缓存结构（C 组）。",
        "refs": [
            {"path": "python/sglang/srt/disaggregation/prefill.py",
             "symbols": ["SchedulerDisaggregationPrefillMixin", "PrefillBootstrapQueue"]},
            {"path": "python/sglang/srt/disaggregation/decode.py",
             "symbols": ["SchedulerDisaggregationDecodeMixin", "DecodeTransferQueue"]},
            {"path": "python/sglang/srt/disaggregation/utils.py",
             "symbols": ["TransferBackend", "MetadataBuffers", "KVClassType"]},
        ],
    },

    # ============ G-服务入口与 API ============
    {
        "feature_group": "G-服务入口与 API",
        "feature": "serving-chat",
        "title": "OpenAI 兼容 chat 服务",
        "page_file": "g01-serving-chat.md",
        "boundary": "负责 chat/completions 协议的请求解析、流式响应与工具调用渲染；"
                    "不负责分词（a03）、不负责调度（a02）。",
        "refs": [
            {"path": "python/sglang/srt/entrypoints/openai/serving_chat.py",
             "symbols": ["OpenAIServingChat", "ThinkingMode"]},
        ],
    },
    {
        "feature_group": "G-服务入口与 API",
        "feature": "function-call",
        "title": "函数调用解析器",
        "page_file": "g02-function-call.md",
        "boundary": "负责把模型输出的工具调用文本解析成结构化调用（四十多个格式检测器）；"
                    "不负责工具执行、不负责调度。",
        "refs": [
            {"path": "python/sglang/srt/function_call/function_call_parser.py",
             "symbols": ["FunctionCallParser"]},
            {"path": "python/sglang/srt/function_call/core_types.py",
             "symbols": ["ToolCallItem", "StreamingParseResult", "StructureInfo"]},
            {"path": "python/sglang/srt/function_call/parser_names.py",
             "symbols": ["TOOL_CALL_PARSER_NAMES"]},
        ],
    },
    {
        "feature_group": "G-服务入口与 API",
        "feature": "observability",
        "title": "可观测与指标",
        "page_file": "g03-observability.md",
        "boundary": "负责请求级指标、trace 与请求日志；"
                    "不负责调度（a02）、不负责服务协议（G 组其他页）。",
        "refs": [
            {"path": "python/sglang/srt/observability/metrics_collector.py",
             "symbols": ["MetricsCollector"]},
            {"path": "python/sglang/srt/observability/trace.py",
             "symbols": ["TraceEvent", "TraceThreadContext", "SpanAttributes"]},
        ],
    },
    {
        "feature_group": "G-服务入口与 API",
        "feature": "sampling",
        "title": "采样",
        "page_file": "g04-sampling.md",
        "boundary": "负责采样参数与采样过程（top-k/top-p、温度、惩罚）；"
                    "不负责调度（a02）、不负责结构化输出。",
        "refs": [
            {"path": "python/sglang/srt/sampling/sampling_batch_info.py",
             "symbols": ["SamplingBatchInfo", "ProcessorEntry"]},
            {"path": "python/sglang/srt/sampling/sampling_params.py",
             "symbols": ["SamplingParams"]},
        ],
    },
]
