---
project: vllm
feature: attention-selector
title: 后端选择
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/attention/selector.py
    symbols: [AttentionSelectorConfig, get_attn_backend]
  - path: vllm/v1/attention/selector.py
    symbols: [get_mamba_attn_backend, get_attn_spec_kind]
---

# 后端选择

## 一句话职责

把「这个模型在什么硬件上、用什么数据精度、要不要 MLA / 滑窗 / 稀疏 / 并行」这一堆事实收成一个不可变的配置对象，交给平台层挑一个后端类，并按 `(配置, 头数, KV cache 精度)` 缓存结果。

## 机制是什么

上一页的 `AttentionBackend` 提供了「能力清单」，这一页是**清单的消费方**。职责分工很干净：

- `AttentionBackend.validate_configuration` 回答「**这个**后端行不行」；
- `selector.get_attn_backend` 回答「**哪个**后端行」。

而 selector 自己**不做任何能力判断**。它只做三件事：收集事实、查缓存、把最终的挑类动作**委托给平台层**（`current_platform.get_attn_backend_cls`）。

这个委托是关键的结构选择：**后端列表是硬件相关的**（CUDA 上有 FlashAttention，CPU 上有 CPU attention，ROCm 上有另一套），把它放进通用选择器会让选择器绑死平台。所以 selector 只负责「把上下文整理好」，平台负责「按上下文挑」。

## 怎么实现的

**`AttentionSelectorConfig` 是一个 `NamedTuple`，不是 dataclass。**

用 `NamedTuple` 的理由不是风格，而是**它可以直接当缓存键**：`functools.cache` 需要一个可哈希的参数，`NamedTuple` 天然可哈希（前提是所有字段可哈希，而这里字段全是 `int` / `bool` / `str` / `torch.dtype` / `None`，都满足）。

若用普通 dataclass，需要 `frozen=True` 且手动实现 `__hash__`；用 `NamedTuple` 省掉这一步。代价是**它是元组**，可以按位置索引——这会让「加字段」变成潜在的破坏性变更（按位置构造的调用点会错位）。

字段有十八个，全部带默认值。手写的 `__repr__` 把每个字段都列出来，看起来冗余（`NamedTuple` 本身有 repr），但**这是为了让日志与错误信息能看出全部开关的状态**——一个后端选择失败了，需要知道当时那十八个开关各是什么。

**缓存键包含了一个「看起来该从配置读」的参数。**

`_cached_get_attn_backend` 的签名里有一个 `_run_kv_cache_dtype` 参数，带下划线前缀（表示内部用）。注释解释了它的存在：「Some platform selectors inspect the run-wide cache dtype. Keep it in the cache key even though they read the value from the current config.」

也就是说：**平台选择器实际是去读全局配置的，但缓存键里仍然带上这个值**。为什么？因为若不带，同一个 `AttentionSelectorConfig`（它的 `kv_cache_dtype` 可能是 `None` 表示 auto）在不同 run-wide cache dtype 下会命中同一缓存项，返回错误的后端。

这是一处**为了缓存正确性而加的参数**：它不参与函数体的计算，只参与键。这类参数很容易被「清理无用参数」的重构删掉，而删掉后错误是静默的（返回上次缓存的后端）。

**KV cache dtype 走两条路径，语义不同。**

`get_attn_backend` 收的 `kv_cache_dtype` 参数会被断言检查合法性（不在白名单就抛 `AssertionError` 并列出合法值），然后放进 `AttentionSelectorConfig`。

而 cache config 里的 `cache_dtype` 是**运行级**的值，通过 `_run_kv_cache_dtype` 单独传。

**两者是不同的东西**：前者是「这一层要什么精度」（可能来自层配置），后者是「这次运行整体用什么精度」。所以一个层可以说「auto」，而整次运行是 fp8。若把两者合并成一个参数，就无法表达这个组合。

**`block_size` 的三态被压成两态。**

代码是：若 cache config 存在且**用户显式指定**了 block size，就用它；否则传 `None`。

注释没有解释，但行为很清楚：`None` 表示「没有用户偏好，让后端自己选」。这与 `AttentionBackend.get_preferred_block_size(default_block_size)` 的签名对应——**`None` 是一个有意义的值，不是缺失**。若把默认值填进来，后端就无法区分「用户要这个值」与「框架推荐这个值」。

`user_specified_block_size` 这个字段名本身就是这条区分的载体。

**布尔开关的来源被分成三类，而分类本身说明了它们的性质。**

第一类来自函数参数（`use_mla`、`has_sink`、`use_sparse`、`use_mm_prefix`、`use_per_head_quant_scales`、`has_sliding_window`）：这些是**层级的**，同一模型不同层可能不同。

第二类来自 `vllm_config`（`use_non_causal` 来自 attention config，`use_pcp`/`use_dcp` 来自 parallel config，`use_rswa` 来自 model config）：这些是**全局的**，一次运行内不变。

第三类来自环境变量（`use_batch_invariant` 来自 `envs.VLLM_BATCH_INVARIANT`）：这是**进程级的**，通过 env 而不是配置传递。

三类的划分不是任意的：**层级的会进缓存键的多样性来源，全局的会进键但值固定，环境变量的也进键以便测试能切换**。

**`use_kv_connector` 的判据比名字窄。**

代码是 `kv_transfer_config is not None and kv_transfer_config.is_kv_transfer_instance`。所以「配了 KV transfer」还不够，必须是「这是个 KV transfer 实例」。这个额外的限定说明 `kv_transfer_config` 在别处还有非实例用途。

**`use_adaptive_verification` 有一段带注释的例外。**

先看 speculative config 是否开了自适应验证，若是则再查一个模块级的 `model_tag`，**若标记为 `dspark_head` 就把它关掉**。注释解释了原因：「The drafter always runs full-length blocks; only the verifier sees the trimmed, device-decided query lengths.」

所以这个开关的语义是「**本次前向会不会看到被 device 裁剪过的 query 长度**」，而不是「有没有开自适应验证」。**drafter 不裁剪，所以它不该受这条能力的约束**——否则一个纯 drafter 的前向会被要求支持「device 与 CPU query 长度不一致」。

这是「**能力位按实际数据流判断，而不是按配置开关判断**」的例子。配置说「开了」，但这条路径上数据没被裁剪，就不该带上这个约束。

**`use_hisparse` 是复合条件。**它是 `use_sparse and attention_config.hisparse_config is not None`——**必须是稀疏的，并且配了 HiSparse**。所以稀疏后端的两种模式（普通稀疏 / HiSparse）在这里被区分开了。

**`get_attn_spec_kind` 把「层级信号」映射成「KV cache 组类型」，而它和另一个函数是镜像关系。**

docstring 写得很清楚：「Mirrors `get_kv_cache_kind` (which derives the kind from the layer's KV cache manager config and spec) so users can target groups by kind when setting `AttentionConfig.backend_per_kind`.」

**镜像的意思是：同一个「组类型」可以从两个方向推出**——从层的信号（这个函数），或从 KV cache 管理器（另一个函数）。若两者不一致，按组指定后端就会指向错误的组。

判断顺序是四层：`ENCODER_ONLY` → `CROSS_ATTENTION`（由 `ENCODER_DECODER` 推出）→ MLA 分支（`SLIDING_WINDOW_MLA` 或 `MLA_ATTENTION`）→ 普通分支（`SLIDING_WINDOW` 或 `FULL_ATTENTION`）。

注意 MLA 优先于滑窗判定：先判 `use_mla`，再在其中判滑窗。所以「MLA + 滑窗」得到 `SLIDING_WINDOW_MLA`，而不是被当成普通滑窗。**组合类型必须先于单特征类型判断**，否则组合会被拆解成其中一个维度。

**注释里有一条明确的「不在这里推导」的排除项。**

docstring 说：「`SINK_FULL_ATTENTION` is intentionally not derived here: it is produced only by the `StaticSinkAttention` layer, whereas a plain `Attention` layer with attention sinks (e.g. gpt-oss) still uses the full-attention or sliding-window manager. Sinks therefore do not change the kind.」

所以「有 sink」不改变组类型。理由是组类型由**KV cache 管理方式**决定，而 sink 只是一个前向细节。**一个只有特定层类才产生的类型，不能从信号推导**——因为信号不足以区分「普通 Attention + sink」与「StaticSinkAttention」。

这是「**可推导性边界**」的一个精确判断：推导只对「由信号完全决定」的属性成立。

**`backend_per_kind` 的覆盖是「按组指定后端」。**

`attention_config.backend_per_kind` 是一个从组类型值到后端名的映射，命中则覆盖全局后端，未命中回落。

代码用 `.get(kind.value, backend)` ——**`kind.value` 而不是 `kind`**，说明映射的键是字符串。这与 `IssueCode` 之类枚举当键的写法不同，意味着配置来自外部（JSON/YAML），必须能用字符串表示。

注释里还有一条重要限制：「The KV cache layout is resolved across all of the model's backends at once in the engine core; a single selection cannot see its peers.」——**一次选择看不到同模型的其他后端**，所以 KV cache 布局这类需要全局视野的决定不能在这里做。这条注释划清了 selector 的能力边界。

**缓存粒度是「每个不同的（后端名、配置、头数、运行精度）组合一次」。**

`@cache` 无参数限制（即 `maxsize=None`），因为组合数是有限的：头数在模型内固定、配置由模型结构决定、后端名来自配置。所以缓存项数上界是「模型内不同注意力层几何的种数」，很小。

**Mamba 走完全独立的选择路径。**

`get_mamba_attn_backend` 收一个 `MambaAttentionBackendEnum` 成员，直接从枚举上取类（`mamba_type.get_class()`）。**没有平台委托、没有能力比对、没有配置对象**——因为 Mamba 后端的种类由模型声明决定，不需要在运行时挑。

它只做一条校验：若开了 batch invariant 但后端不支持，抛 `RuntimeError`。

**两套路径的对比很有信息量**：注意力后端要靠「能力比对 + 平台决策」来挑（因为候选多、约束多），Mamba 后端只需要「模型说用哪个就用哪个」（因为候选少、无硬件分歧）。**同一层里两种选择机制的差异，反映的是候选空间规模的差异。**

## 关键取舍

**源码事实：** `AttentionSelectorConfig` 是 `NamedTuple`，可以直接当作 `functools.cache` 的键；它有十八个字段、全部带默认值，并有手写 `__repr__` 逐项列出。

**源码事实：** `_cached_get_attn_backend` 的缓存键里带一个 `_run_kv_cache_dtype` 参数，注释说明平台选择器实际从全局配置读该值，但它必须参与键以免跨精度复用缓存。

**源码事实：** `get_attn_backend` 的 `kv_cache_dtype` 参数（层级精度，可为 `auto`）与 `cache_config.cache_dtype`（运行级精度）是两条不同路径。

**源码事实：** `block_size` 只在用户显式指定时传入真实值，否则传 `None` 表示无偏好。

**源码事实：** `use_adaptive_verification` 在 `model_tag == "dspark_head"` 时被强制关闭，注释说明 drafter 总跑全长块、只有 verifier 看到被裁剪的 query 长度。

**源码事实：** `use_hisparse` 是 `use_sparse` 与 `hisparse_config` 非空的合取。

**源码事实：** `get_attn_spec_kind` 的判定顺序是编码器类优先、然后 MLA 分支、最后普通分支；「有 sink」刻意不改变组类型，理由是普通 Attention 层带 sink 时仍用普通 manager。

**源码事实：** `backend_per_kind` 用 `kind.value`（字符串）作键查表，未命中回落全局后端。

**源码事实：** 平台委托通过 `current_platform.get_attn_backend_cls` 完成，返回空则抛 `ValueError`；实际类由 `resolve_obj_by_qualname` 按限定名延迟导入。

**源码事实：** `get_mamba_attn_backend` 直接从枚举取类，只校验 batch invariant 兼容性，不做平台委托与能力比对。

**架构推论：** 「选择器不做能力判断」这条分工让新增硬件只需实现平台方法，不必改选择器。代价是**选择器无法在挑之前给出「为什么都不可用」的汇总**——它拿到的是平台返回的空值，只能报「这个设备上没有可用后端」。**把判断推给平台换来可扩展性，代价是错误信息的粒度变粗。**

**架构推论：** 缓存键里带一个「不参与计算只参与键」的参数，是缓存正确性最容易出错的地方。它的危险在于看起来很冗余，容易被重构删除；而删除后的错误是「返回上次缓存的后端」，可能只在特定精度组合下才暴露。**任何只进键不进函数体的参数，都应当在注释里说明「不能删」。**

**架构推论：** 「层级信号 vs 全局配置 vs 环境变量」三类来源的划分，实际对应三种变更频率与三种测试手段：层级信号要跑不同模型才能覆盖，全局配置改 `VllmConfig` 即可，环境变量可以用夹具切换。**把开关放哪一层，决定了它多容易被测试**——这解释了为什么 `VLLM_BATCH_INVARIANT` 选了环境变量：它主要服务于确定性测试。

**架构推论：** `get_attn_spec_kind` 与 `get_kv_cache_kind` 的镜像关系是一处**必须同步维护的不变量**，而代码层面没有任何机制保证它们一致。用 `kind` 做配置键的价值是「用户可以按组调后端」，代价是「两个推导函数必须永远说同一件事」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/attention/selector.py` | `AttentionSelectorConfig` | 十八项选择信号，`NamedTuple` 以便直接做缓存键 |
| `vllm/v1/attention/selector.py` | `AttentionSelectorConfig.__repr__` | 逐项列出全部开关，便于诊断 |
| `vllm/v1/attention/selector.py` | `get_attn_backend` | 收集信号、解析按组覆盖、查缓存、委托平台 |
| `vllm/v1/attention/selector.py` | `_cached_get_attn_backend` | 带缓存的挑类入口，键含运行级精度 |
| `vllm/v1/attention/selector.py` | `get_attn_spec_kind` | 由层级信号推导 KV cache 组类型 |
| `vllm/v1/attention/selector.py` | `get_mamba_attn_backend` | Mamba 后端选择，按枚举直取 |
| `vllm/v1/attention/selector.py` | `_cached_get_mamba_attn_backend` | Mamba 选择的缓存与 batch invariant 校验 |
| `vllm/v1/attention/backends/registry.py` | `MambaAttentionBackendEnum` | Mamba 后端的枚举与类映射 |
| `vllm/config/cache.py` | `is_known_kv_cache_dtype` | KV cache 精度白名单校验 |
| `vllm/config/cache.py` | `KV_CACHE_DTYPES` | 合法精度列表，用于错误信息 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/v1/attention/selector.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/attention/selector.py)
- [vllm 文档：注意力后端选择](https://docs.vllm.ai/en/latest/design/kernel/paged_attention.html)
- [FlashInfer 项目主页](https://github.com/flashinfer-ai/flashinfer)

## 与主核 / AI 的关系

**「选择器收集事实、平台挑实现」与主核的优化器分工同构。** 数据库的优化器收集代价参数（行数、选择率、页数），然后把「用哪个访问方法」的最终决定交给 AM 提供的成本函数。dify 之类应用层常犯的错误是把硬件判断硬编码进通用逻辑；vllm 在这里避免了它。**判据是「这个决策依赖的信息是否与平台强耦合」**——是，就推开；不是，就留下。

**「`None` 作为有意义的值」与主核的三值语义一致。** SQL 的 `NULL` 不是「空字符串」也不是「0」，是「未知」。vllm 的 `block_size=None` 也不是「用默认值」，是「无偏好，让内核决定」。**把「未指定」与「指定为默认值」区分开，是一个系统能否正确处理继承与覆盖的前提。**

**「镜像不变量」与主核的目录一致性同类。** 数据库里有大量「同一事实记在两处必须一致」的情形（如 `pg_class.relpages` 与索引大小），靠 VACUUM 与系统函数维护，且出问题时表现为性能偏差而非错误。`get_attn_spec_kind` 与 `get_kv_cache_kind` 的关系就是这样：**两处推导必须同步，错配只会让某个按组配置失效，不会报错。**

**「选择失败信息粒度」与主核的 planner 错误同类。** PostgreSQL 在找不到可用索引时能给「为什么每个索引都被排除」的详细理由；vllm 的平台委托模式拿不到这个。**能力判断离选择点越远，能给出的理由越少**——这是把决策推给扩展点的固有代价。

对 agent 工程最实用的五条。其一：**缓存键里「看似冗余」的参数不要删**，它可能只参与键不参与计算。其二：**区分「层级的」与「全局的」开关**，混在一起会让缓存命中率崩塌或结果错误。其三：**能力位按实际数据流判断，不要按配置开关判断**（`dspark_head` 的例外是范例）。其四：**`None` 要保留为「未指定」**，不要提前填默认值，否则下游无法区分用户意图。其五：**两处推导同一事实时，要显式写出镜像关系**，否则改动一处会让另一处静默失效。
