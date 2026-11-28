---
project: vllm
feature: model-registry
title: 模型注册与配置
page_type: feature
schema_version: 1
refs:
  - path: vllm/config/model.py
    symbols: [ModelConfig]
  - path: vllm/config/scheduler.py
    symbols: [SchedulerConfig]
  - path: vllm/config/cache.py
    symbols: [CacheConfig]
---

# 模型注册与配置

## 一句话职责

三份 dataclass 共同定义「这次运行是什么形状」：`ModelConfig` 描述模型本身（架构、精度、头数、上下文长度），`SchedulerConfig` 描述批次预算，`CacheConfig` 描述 KV cache 的物理形态——三者互相校验，且大量字段是「用户可不填、由其余信息推导」的。

## 机制是什么

推理引擎的配置有一个与其他系统不同的性质：**用户想说的可能只有一句「跑这个模型，用这张卡」，其余上百个参数都必须由系统推出来且必须互相自洽。**

这带来三组张力，三份配置正好各承担一组：

| 配置 | 核心张力 |
|---|---|
| `ModelConfig` | 「模型说什么」vs「用户覆盖什么」 |
| `SchedulerConfig` | 「吞吐」vs「显存」vs「正确性」 |
| `CacheConfig` | 「块大小的物理约束」vs「前缀命中的匹配粒度」 |

而三份配置之间的耦合非常紧：`ModelConfig` 的头数决定 `CacheConfig` 的每块字节数，`SchedulerConfig` 的批预算决定显存需求，而 `max_model_len` 既验证调度预算又决定缓存容量。

**vllm 用 `InitVar` 处理了「值存在 A 里、但这里需要它来校验」这类依赖。** `SchedulerConfig` 的 `max_model_len` 与 `is_encoder_decoder` 都是 `InitVar`，注释写明「This is stored in the ModelConfig, and is used only here to validate」。所以**校验需要的输入不复制成字段，只在构造时借用**——这避免了同一事实存两份而漂移的风险。

## 怎么实现的

**`ModelConfig` 的字段分三类，三类的可写性完全不同。**

第一类是**用户输入**（`model`、`dtype`、`max_model_len`、`tokenizer` 等），可写。

第二类是**`init=False` 的派生字段**（`hf_config`、`hf_text_config`、`is_submodel_config`、`word_embeddings_untied_by_checkpoint`），构造后由内部填充，用户不能传。

第三类是**查询方法**而不是字段：`get_head_size`、`get_num_attention_heads`、`get_num_kv_heads`、`get_total_num_hidden_layers`、`get_layers_start_end_indices`。**这是刻意的**——这些值需要结合并行配置才能算，不能存成一个数。

**`is_submodel_config` 是一条防止误校验的护栏。**

它的注释写得极清楚：「Whether this is a view derived from an already-validated `VllmConfig`: a submodule view from `VllmConfig.with_hf_config` (e.g. a multimodal model's text stack, whose architecture list may be empty) or a draft model's view that reuses the target's configs. **Deployment-level validation must not run against it.**」

所以存在一种「看起来像 `ModelConfig` 但只是视图」的对象，对它跑部署级校验会误报（它的架构列表可能是空的）。**用一个人为标记把「真配置」与「视图」分开**，是共享同一类型带来的必要代价。

**`word_embeddings_untied_by_checkpoint` 记录了一次「推理出来的配置覆盖」。**

注释说明：当 checkpoint 里有自己的 `lm_head` 时，`tie_word_embeddings` 会被覆盖成 `False`；而如果两者实际相同，加载完权重后会重新绑回去。

**所以这个字段记录的是「曾经的判断」而不是「最终状态」**——它存在的意义是让加载后的重新绑定有依据。这类「记录推导过程」的字段在配置里很少见，但它避免了「加载后再猜一次」。

**头数的计算有三条不同的规则，而每条都对应一个真实约束。**

`get_num_attention_heads`：总头数整除张量并行度。**没有下限保护**——若总头数不是 TP 的整数倍，结果会静默截断。

`get_num_kv_heads`：先看 MLA，`if self.use_mla: return 1` 并附注释「When using MLA during decode it becomes MQA」。**这是一处把注意力机制直接写进头数计算的地方**：MLA 的 KV 只有一份，所以每 GPU 恒为 1。然后对普通情况用 `max(1, total // tp_size)`——**有下限保护**，因为 KV 头数可以少于 TP 度，此时各卡复制同一份。

**两个方法的保护策略不同，而理由不同**：Q 头必须真正切分（每卡至少一个才有意义），KV 头可以复制（少于卡数时复制更划算）。这个不对称是 MLA 与 GQA 的机制差异投射到并行策略上的结果。

**`get_layers_start_end_indices` 里有一行注释点明了 rank 的排布顺序。**

注释写的是 `# the layout order is: DP x PP x PCP x TP`，然后从全局 rank 反算 PP rank 的公式是：`(rank // (tp_size * pcp_size)) % pp_size`。

**先除以 TP、再除以 PCP、最后对 PP 取模**——这个顺序直接对应注释里的排布。若排布顺序变了而公式没改，层会切错，而症状是「模型能跑但输出乱」，很难归因到并行配置。

`get_num_layers` 就是 `end - start`，没有其他逻辑。

**`SchedulerConfig` 的两个 `InitVar` 展示了「借值校验」的用法。**

`__post_init__` 的签名是 `(self, max_model_len: int, is_encoder_decoder: bool)`——两个 `InitVar` 进来了，但它们**没有变成字段**。用完之后它们就消失了。

`is_encoder_decoder` 为真时做三件事：关掉 `disable_chunked_mm_input`、关掉 `enable_chunked_prefill`、把 `long_prefill_token_threshold` 归零，并打一条说明「encoder-decoder 模型既不支持分块预填充也不支持前缀缓存」。

**这是一处「模型形态反向决定调度策略」的强制覆盖。** 用户即使显式设了 `enable_chunked_prefill=True` 也会被改掉——因为这三种优化在 encoder-decoder 上不成立。

紧接着两行把 `max_num_encoder_input_tokens` 与 `encoder_cache_size` 都对齐到 `max_num_batched_tokens`。前者是 `init=False` 字段（注释说「This is not currently configurable」），后者仅在未指定时填充。**所以「编码器缓存与批预算同量」是一个隐含的设计判断**，用户不能单独调它。

**`verify_max_model_len` 用四条规则把批预算与序列长度的关系钉死，其中三条是硬错误、一条只是警告。**

第一条：`max_num_batched_tokens < max_model_len` 且关了分块预填充 → 报错。错误消息写得很好：它不只说「不合法」，而是解释了后果——「This effectively limits the maximum sequence length to max_num_batched_tokens and makes vLLM reject longer sequences」。

**这条的前半句是关键：`max_num_batched_tokens` 实际成了序列长度上限。** 若不分块预填充，一个长 prompt 必须一次塞进一批；批预算小于它就只能拒绝请求。

第二条：`max_num_batched_tokens < max_num_seqs` → 报错。因为每个请求至少要拿到一个 token。

第三条：`max_num_active_seqs > max_num_seqs` → 报错。子集不能大于全集。

第四条：`max_num_batched_tokens > max_num_seqs * max_model_len` → **只警告**。这个组合不违反任何约束（只是永远用不满），所以警告而非拒绝。**「不可能发生但无害」与「不可能发生且有害」被区别对待**，这是配置校验里少见的细致。

**`CacheConfig` 的 `block_size` 用了「可空入参 + 构造后必非空」的形态。**

字段声明是 `block_size: int = Field(default=None, gt=0)`，但 docstring 写「Accepts None (meaning "use default"). **After construction, always int.**」

配合 `user_specified_block_size: bool = field(default=False, init=False)` —— 这个派生字段记录「用户是否显式给了值」。所以 `None` 与「显式给了默认值」在构造后可以区分，而**下游只需要处理 int**。

这个设计直接支撑了 c02 讲过的一处逻辑：XPU 后端只有在「用户显式要求了非 64 倍数」时才声明支持 16。若没有 `user_specified_block_size`，后端无法区分这两种情形。

**`kv_cache_layout` 是一个「一次解析、全局广播、之后只读」的字段。**

注释说明了生命周期：`None` 表示未解析；引擎核心在显存测量前**解析一次**（`resolve_kv_cache_layout`）；每个 worker 通过 RPC 或 `KVCacheConfig` 采纳这个值，**在 KV cache 分配之前**完成；一旦设定就是终局，用 `get_resolved_kv_cache_layout` 读取（该函数在值为 `None` 时抛错）。

**「一次解析、全局广播、此后只读」是一个分布式配置项的标准形状。** 若每个 worker 各自解析，可能得出不同结果；若允许重设，已分配的缓存会与声明不符。所以用「解析前置 + 读取时断言非空」来保证一致性。

**`prefix_match_unit` 与物理块大小是两个独立概念。**

字段注释说它是「the finest token boundary a prefix-cache hit can land on」，可以设得比物理块更细（例如 32 对 1024 的混合模型块），**只要每个 KV cache 组的块大小都能被它整除**。

所以存在两个粒度：**存储粒度（块）**与**匹配粒度（哈希单位）**。注释明确说「It controls matching granularity only, not how often states are stored」——**匹配更细不等于存储更频繁**，因为一个物理块里可以有多个哈希边界。

并且它有一个启动期校验：若显式给了值但实际命中会回落到调度器块大小（例如滑窗组要求块对齐查找），启动就失败。**这条校验的价值在于「静默降级会让人以为配了但没生效」**，所以选择硬失败。

**`hash_block_size` 与 `cache_hit_alignment_tokens` 是两个相关但可能不同的派生值，注释里带了一条 TODO。**

注释说：「The two differ when hashes are finer than the scheduler block but partial hash hits are off (e.g. a sliding-window group cannot look up sub-block hits), so hits only land on the scheduler block.」然后是一行 `# TODO(zjy0516): Merge into one value`。

**这是一处「承认存在两个概念、并记录了合并意图」的诚实标注。** 它们不同的情形很具体：哈希更细但部分命中被关掉时，命中只能落在块边界上。

**前缀缓存的哈希算法是可选的，而每一档都标了安全边界。**

四个选项：`sha256`（pickle + SHA256，默认）、`sha256_cbor`（规范 CBOR、跨语言可复现）、`xxhash`（pickle + xxHash，非加密）、`xxhash_cbor`。

非加密选项的注释写了一整段警告：「theoretically increases the risk of hash collisions, which can cause undefined behavior or even **leak private information in multi-tenant environments**.」

**「哈希碰撞在多租户下会泄漏私有信息」**——这条不是理论洁癖。前缀缓存用哈希做键，碰撞意味着两个不同前缀共享同一份缓存，于是 A 租户的上下文可能被 B 租户读到。**在这里，碰撞不是性能问题而是安全问题。**

`sha256_cbor` 的存在理由也值得记：pickle 序列化在不同 Python 版本或实现间可能不同，而 CBOR 规范序列化让**跨语言、跨版本**的哈希一致。所以它是为「缓存可移植」而不是为「更安全」加的。

**`skip_page_size_padded` 记录了「不同精度的层必须共享页大小」这一物理约束。**

注释说它是「Optional override for the page size of layers skipped from KV cache quantization; set during block-size alignment so unquantized skip layers pad up to the quantized primary's page」。

配合 `kv_cache_dtype_skip_layers`（跳过量化的层）与 `mamba_page_size_padded`（保证与 attention 页对齐），三者共同解决同一个问题：**混合模型的不同缓存组必须能放进同一套块规划里，所以页大小要先统一再取最大值**。

`effective_attention_block_size`、`kv_cache_size_tokens`、`kv_cache_max_concurrency` 都是 `init=False` 的派生结果，注释里有一条特别说明了为什么它不能用 `num_gpu_blocks * block_size` 直接算：「Uses group-aware capacity since num_gpu_blocks * block_size can be wrong for hybrid models where requests occupy multiple KV cache groups.」

**「块数乘块大小」在混合模型下是错的**——因为一个请求可能同时占用多个组，每组各消耗块。这条注释纠正的是一种极其自然的错误算法。

**`gpu_memory_utilization` 的文档几乎是一篇运维指南。**

它说明了默认值随特性变化（「If unspecified, 1.0 with the extensible KV cache (sized from measured memory) and 0.92 otherwise」），并列出四种「应该下调」的情形：同卡有其他进程（包括 vLLM 启动后才分配的）、统一内存设备、权重传输缓冲、自建设备缓冲的 KV connector。

最后一句是核心取舍：「A lower value takes memory away from the KV cache.」——**所以调低它不会「让模型跑得更稳」，而是直接削减 KV cache 容量**，进而降低并发。

## 关键取舍

**源码事实：** `SchedulerConfig` 的 `max_model_len` 与 `is_encoder_decoder` 是 `InitVar`，注释说明它们存在 `ModelConfig`、此处仅用于校验。

**源码事实：** encoder-decoder 模型会在 `__post_init__` 里被强制关掉分块预填充与多模态分块输入，并把长预填充阈值归零，用户显式设置也会被覆盖。

**源码事实：** `verify_max_model_len` 前三条违规是 `ValueError`，第四条（批预算大于「序列数 × 最大长度」）只警告。

**源码事实：** `get_num_kv_heads` 在 MLA 下直接返回 1（注释「during decode it becomes MQA」），普通情况用 `max(1, total // tp)` 保证下限；`get_num_attention_heads` 无下限保护。

**源码事实：** `get_layers_start_end_indices` 的 rank 反算公式对应注释里的排布顺序 `DP x PP x PCP x TP`。

**源码事实：** `CacheConfig.block_size` 接受 `None` 表示「用默认」，构造后必为 int，并用 `user_specified_block_size` 记录是否显式提供。

**源码事实：** `kv_cache_layout` 的生命周期是「引擎核心解析一次 → 广播给 worker → 分配前采纳 → 此后只读」，`get_resolved_kv_cache_layout` 在未解析时抛错。

**源码事实：** `prefix_match_unit` 可以比物理块更细，只影响匹配粒度不影响存储频率；显式设置后若命中会回落到调度器块大小则启动失败。

**源码事实：** `hash_block_size` 与 `cache_hit_alignment_tokens` 在「哈希更细但部分命中关闭」时会不同，代码里有合并的 TODO。

**源码事实：** 非加密哈希算法的文档明确警告碰撞可能「leak private information in multi-tenant environments」；`sha256_cbor` 的卖点是规范序列化带来的跨语言可复现。

**源码事实：** `kv_cache_size_tokens` 的注释指出 `num_gpu_blocks * block_size` 在混合模型下会算错，因为一个请求可能占用多个缓存组。

**架构推论：** 「`InitVar` 借值校验」这个手法让「同一事实只存一处」与「构造时能交叉校验」同时成立。代价是**构造顺序变成了隐式契约**：必须先有 `ModelConfig` 才能构造 `SchedulerConfig`，而这一点只体现在调用方代码里，不在类型里。

**架构推论：** 「查询方法而非字段」是应对「值依赖上下文」的正确形状。头数依赖并行度，层范围依赖 rank，把它们做成方法强迫调用方提供上下文。**若做成字段，就必须在并行配置变化时刷新它们，而刷新时机是最容易出错的地方。**

**架构推论：** 「配置校验分硬错误与警告」需要一条判断标准，而这里用的标准是**「这个组合会不会导致行为与配置不符」**。前三条会让实际能力小于声明（序列被拒、每请求拿不到 token、子集大于全集），第四条只是资源用不满——**用不满是浪费，小于声明是欺骗**。

**架构推论：** 「安全相关的选项必须写清威胁模型」。哈希算法的四个档位里，两个非加密选项的注释解释了「碰撞 → 跨租户泄漏」这条链路，而不是只说「不够安全」。**没有威胁解释的安全警告会被当作教条忽略。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/config/model.py` | `ModelConfig` | 模型配置：用户输入、派生字段与查询方法的混合体 |
| `vllm/config/model.py` | `ModelConfig.get_head_size` | 从架构配置取头维度 |
| `vllm/config/model.py` | `ModelConfig.get_num_kv_heads` | KV 头数，MLA 下恒为 1，其余按 TP 切且保底 1 |
| `vllm/config/model.py` | `ModelConfig.get_num_attention_heads` | Q 头数，按 TP 整除且无保底 |
| `vllm/config/model.py` | `ModelConfig.get_layers_start_end_indices` | 按 `DP x PP x PCP x TP` 排布反算层范围 |
| `vllm/config/model.py` | `ModelConfig.verify_with_parallel_config` | 模型与并行配置的交叉校验 |
| `vllm/config/scheduler.py` | `SchedulerConfig` | 批预算与调度策略配置 |
| `vllm/config/scheduler.py` | `SchedulerConfig.__post_init__` | 借 `InitVar` 校验，encoder-decoder 强制覆盖 |
| `vllm/config/scheduler.py` | `SchedulerConfig.verify_max_model_len` | 四条批预算与序列长度的一致性规则 |
| `vllm/config/cache.py` | `CacheConfig` | KV cache 物理形态配置 |
| `vllm/config/cache.py` | `CacheConfig.user_specified_block_size` | 区分「未指定」与「显式指定默认值」 |
| `vllm/config/cache.py` | `CacheConfig.kv_cache_layout` | 一次解析、广播、分配前固定 |
| `vllm/config/cache.py` | `CacheConfig.prefix_match_unit` | 前缀命中粒度，可细于物理块 |
| `vllm/config/cache.py` | `CacheConfig.hash_block_size` | 每个哈希覆盖的 token 数，与命中落点可能不同 |
| `vllm/config/cache.py` | `CacheConfig.cache_hit_alignment_tokens` | 命中实际落点粒度 |
| `vllm/config/cache.py` | `CacheConfig.kv_cache_dtype_skip_layers` | 跳过量化的层，配合页大小对齐 |
| `vllm/config/cache.py` | `CacheConfig.effective_attention_block_size` | 含 DCP 的有效块大小，初始化后填充 |
| `vllm/config/cache.py` | `CacheConfig.kv_cache_size_tokens` | 组感知的缓存容量，不能用块数乘块大小 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：config/model.py](https://github.com/vllm-project/vllm/blob/main/vllm/config/model.py)
- [vllm 源码：config/cache.py](https://github.com/vllm-project/vllm/blob/main/vllm/config/cache.py)
- [vllm 文档：引擎参数](https://docs.vllm.ai/en/latest/configuration/engine_args.html)

## 与主核 / AI 的关系

**「借值校验而不复制字段」与主核的参数继承一致。** 数据库的 `work_mem` 有全局、数据库、角色、会话四层，实际值由继承算出，而不是层层复制。vllm 的 `SchedulerConfig` 用 `InitVar` 借 `max_model_len` 做校验而不存它，同一个思路：**值只有一个权威位置，其余是视图**。

**「查询方法而非字段」与主核的目录函数同源。** 数据库不缓存「表有多少页」，而是每次从目录读或从统计信息估算，因为值会变。vllm 的头数依赖并行度、层范围依赖 rank，都不适合固化。**凡是依赖上下文的派生值，做成函数比做成字段更安全。**

**「配置校验分硬错误与警告」与主核的参数校验同类。** 数据库对 `shared_buffers` 过大是警告而非拒绝（它仍可能起来，只是可能被 OOM 杀掉），对 `max_connections` 与 `superuser_reserved_connections` 的关系则硬报错。**判据同样是「能不能启动并正确工作」**。

**「哈希碰撞导致跨租户泄漏」与主核的页共享风险同构。** 数据库在共享缓冲区里用页号索引，页号唯一所以没有碰撞问题；一旦引入内容哈希做去重，碰撞就从「性能问题」变成「数据混淆」。**任何以「内容的摘要」当身份的设计，都必须把碰撞当作正确性问题而不是概率问题。**

对 agent 工程最实用的五条。其一：**配置项要区分「未指定」与「指定为默认值」**，否则无法实现「用户显式要求才降级」这类逻辑。其二：**派生值要做成函数**，依赖上下文的量一旦固化成字段就会漂移。其三：**校验消息要写清后果**，不只说「不合法」——「批预算实际限制了序列长度」这种话能省掉半小时排查。其四：**不可能但无害的组合用警告，会让实际能力小于声明的组合用错误**。其五：**安全选项要解释威胁链路**，否则用户会按性能排序选到不安全的那个。
