---
project: vllm
feature: llama-model
title: Llama 模型实现
page_type: feature
schema_version: 1
refs:
  - path: vllm/model_executor/models/llama.py
    symbols: [LlamaForCausalLM, LlamaModel]
  - path: vllm/model_executor/models/llama.py
    symbols: [LlamaDecoderLayer]
---

# Llama 模型实现

## 一句话职责

把 Llama 的四个结构件（注意力、MLP、解码层、整体模型）装配成一条可被张量并行 / 流水线并行 / 投机解码 / 多模态适配器复用的链，并用「骨架残留层 + 权重名映射」让一份 HF 权重在任意并行度下都能装进不同切片。

## 机制是什么

模型实现这一类文件看似只是「照抄论文结构」，实际要解决的全是**装配问题**：

- **并行切分**：同一份权重在 TP=1 与 TP=8 上要落到不同形状的参数里；
- **流水线切分**：每张卡只持有若干层，但状态要按 `hidden_states + residual` 两个量在卡间传递；
- **权重名差异**：HF 的 `q_proj/k_proj/v_proj` 在 vllm 里合并成 `qkv_proj`，`gate_proj/up_proj` 合并成 `gate_up_proj`；
- **投机解码与多模态**：草稿模型与多模态文本塔要复用同一个类，但入口不同。

vllm 的解法是让**模型类只描述语义，切分交给算子层**。所以 `LlamaAttention` 里看不到任何 `torch.chunk` 或 `narrow`——它只声明「我要一个 QKV 合并投影，输入 `hidden_size`、头数 `total_num_heads`」，切分由 `QKVParallelLinear` 按当前 TP 度决定。

这条分工的判据是：**「切多少」是运行期才知道的（取决于并行配置），「切哪个维度」是建模时就知道的**。前者要留在层里，后者可以写死在模型代码里。

## 怎么实现的

**头数在模型层算一次，两种保护策略并存。**

`LlamaAttention.__init__` 里先拿 `tp_size`，然后对 Q 头要求**必须整除**（`assert self.total_num_heads % tp_size == 0`）；对 KV 头分两种情况：

- `total_num_kv_heads >= tp_size`：要求**KV 头数被 TP 整除**，然后按 TP 切分；
- 否则：要求**TP 被 KV 头数整除**，即各卡复制同一份。

最后统一 `self.num_kv_heads = max(1, self.total_num_kv_heads // tp_size)`。

**这里三个断言合起来构成了一个完整的分支覆盖**：要么能切、要么能复制，两者之外都是配置错误。而 `max(1, ...)` 是给第二种情况的兜底——除下来是 0，取 1。

这与 d01 讲过的 `ModelConfig.get_num_kv_heads` 是同一逻辑的两处实现，但有一处差别：**这里的断言严格，那里的 `max(1, ...)` 是唯一的保护**。模型层显式拒绝了「KV 头数既不能整除 TP、TP 也不能整除 KV 头数」的情形（如 KV=3、TP=2），而配置层的计算方法不会抱怨。

**`head_dim` 的取法有一个回退。**

```python
head_dim = getattr(config, "head_dim", None)
self.head_dim = head_dim or self.hidden_size // self.total_num_heads
```

新版 HF 配置可能显式给 `head_dim`（因为它未必等于 `hidden_size // num_heads`，Llama 3 之后有了非对称的情形）。**用 `getattr` 加回退而不是直接读字段**，让老配置也能工作。

同时这也意味着 `hidden_size` 不再必然等于 `num_heads * head_dim`——`q_size` 与 `kv_size` 都基于 `head_dim` 算，而 `o_proj` 的 `input_size` 用 `total_num_heads * head_dim`。**三处都用 `head_dim` 而非 `hidden_size // num_heads`**，这样非对称情形才自洽。

**`qkv_proj` 之后立刻按三段 split。**

`qkv.split([self.q_size, self.kv_size, self.kv_size], dim=-1)` —— 尺寸用**本 rank 的** `q_size`/`kv_size`，不是全局的。所以切分发生在合并投影的**输出**上，而合并投影已经按 TP 切好了输出维度。

**注释里没有解释这一点，但它是整个并行方案的关键**：`QKVParallelLinear` 的输出已经只是本卡负责的那部分头，所以模型层只需要按本卡的尺寸切开。

**`layer_types` 的分支专门为混合注意力模型准备，并带了一段 Eagle3 的兼容修正。**

若 `config.layer_types` 存在，就取出本层类型，判断是否 `sliding_attention`，是则把 `config.sliding_window` 传给注意力层（`per_layer_sliding_window`）。

Eagle3 的修正在于：**草稿模型复用目标模型的配置，所以 `layer_idx` 是从目标模型连续编号下来的**，需要减去 `config.target_layer_count` 才是草稿模型内的相对索引。代码用 `hasattr(config, "target_layer_count")` 区分「这是草稿」还是「这是目标」，然后断言相对索引在 `layer_types` 范围内。

**这是一处「同一份配置被两个模型共享」的典型代价**：配置里的列表是按目标模型的层数写的，草稿模型必须自己算偏移量。而这个偏移量没有任何类型约束，只能靠 `hasattr` 探测。

**注意力类的选择只有两个选项，但它的存在说明了扩展点。**

`EncoderOnlyAttention` 或 `Attention`，二选一。为什么不是更多？因为其它形态（交叉注意力、编码器-解码器）由别的模块承载——`attn_type` 作为参数传下去，由 `Attention` 内部按类型分派（见 c01 的 `supports_attn_type`）。

**所以这一层只区分「要不要 KV cache」**：纯编码器不需要，其它都需要。**判据是「有没有历史」**，不是「注意力类型是什么」。

**解码层的残差处理是一条「首次特殊、之后统一」的模式。**

`forward` 接受 `residual: torch.Tensor | None`。为 `None` 时（这是第一层），把 `hidden_states` 本身当残差、再对输入做 layernorm——不走融合的「加残差 + norm」路径，因为此时没有可加的残差。

之后每层都走 `self.input_layernorm(hidden_states, residual)` 返回 `(normed, new_residual)` 的双输出形式。**这个设计让 RMSNorm 能把「加残差」与「归一化」融成一个 kernel**，从而省掉一次全量读写。第一层的特例只是因为残差还不存在。

`return hidden_states, residual` 而不是只返回 `hidden_states`，是把这个「未融合的残差」传给下一层——**层间传的是两个张量，不是一个**。这也是流水线并行要在卡间传两个量的原因。

**`@support_torch_compile` 的 `dynamic_arg_dims` 是这段代码里最需要解释的装饰器。**

```python
dynamic_arg_dims={
    "input_ids": {0: "b"},
    "positions": {0: "b"},
    "intermediate_tensors": {0: "b"},
    "inputs_embeds": {0: "b"},
},
```

它声明「这四个参数的**第 0 维**是动态的，且共享同一个符号 `b`」。

所以 `torch.compile` 会把 batch 维当作符号维度，其余维度当作静态——**从而同一份编译产物能复用于不同批大小**。若不会声明，任何批大小变化都会触发重新编译。

上面还有一行被注释掉的 `# mark_unbacked_dims={"input_ids": 0}` 和一条 `# TODO[#32068]: Investigate recompilation`。**这说明动态维度这里曾经有问题（或者仍有），而当前方案是被验证过的那个。**

**模型的 `forward` 里 `aux_hidden_states` 的收集位置很讲究。**

两个收集点：第一层**之前**（`islice` 循环外的 `self._maybe_add_hidden_state(aux_hidden_states, self.start_layer, ...)`）与每层**之后**（`idx + 1`）。

前者的条件是 `get_pp_group().is_first_rank` —— 只有首个 PP rank 需要收「第 0 层之前」这个状态。**因为在 PP 下，非首 rank 的「第 0 层之前」是上一张卡的输出，而那份数据已经被上一张卡收过了**，重复收集会产生重复项。

`supports_aux_hidden_states_over_pp = True` 是配套的能力声明：说明这个模型能把辅助隐藏状态跨 PP 传递（靠 `pack_local_aux_hidden_states`）。

**`aux_hidden_states` 有三个消费者，这解释了它的存在。**

投机解码的草稿模型（Eagle 系列）需要多个中间层的隐藏状态作为输入；`skip_o_proj` 之类的变体也需要；以及一些需要层间特征的模型。所以收集逻辑放在骨架里，由 `_maybe_add_hidden_state` 决定「这一层要不要收」——**方法在 mixin 里，判断依据在 mixin 里，骨架只负责在正确的位置调用**。

**权重名映射被写成一张声明表而不是一堆 if。**

`WeightsMapper(orig_to_new_stacked={...})` 用五个条目表达：

- `".q_proj"` → `(".qkv_proj", "q")`
- `".k_proj"` → `(".qkv_proj", "k")`
- `".v_proj"` → `(".qkv_proj", "v")`
- `".gate_proj"` → `(".gate_up_proj", 0)`
- `".up_proj"` → `(".gate_up_proj", 1)`

**左边的键带前导点**，说明匹配的是名字的中间段（如 `model.layers.0.self_attn.q_proj.weight` 里的 `.q_proj`），而不是前缀。**右值带 shard_id**，告诉加载器「这段权重该装进合并参数的哪一片」。

这是「**用声明代替分支**」的一个干净实例：新增一个合并只需加一行，不需要在加载逻辑里加 if。

**`LlamaModel.__init__` 里 `embed_tokens` 的创建条件是三个布尔量取或。**

```python
if (
    get_pp_group().is_first_rank
    or (config.tie_word_embeddings and get_pp_group().is_last_rank)
    or spec_decode_needs_target_embed(vllm_config)
):
```

三个理由分别是：嵌入只在首卡用（正常路径）；**绑定权重时末卡也要用它**（因为 `lm_head` 与它是同一份参数）；**投机解码时目标模型的嵌入也要用**（草稿模型需要目标的词嵌入）。

三个不相关的理由取或，意味着**这个条件无法用一句话概括**，只能逐条列出。这正是注释应该在场的地方，而这里注释缺失——读者必须自己推。

**`norm` 只在末卡创建，其余情况用 `PPMissingLayer()`。**

`make_layers` 也是同一手法：它在首尾各填 `start_layer` 与 `num_hidden_layers - end_layer` 个 `PPMissingLayer`，中间放真实层。

**所以每张卡上的 `self.layers` 长度都是 `num_hidden_layers`，但只有自己负责的那段是真的。** 这个选择让权重加载可以按统一的名字寻址——**层索引在所有卡上都是全局的，不需要加偏移**。

代价是**内存里有一堆空壳模块**（`PPMissingLayer` 继承 `torch.nn.Identity`，开销近乎为零）与**遍历时必须跳过它们**（用 `islice(self.layers, start, end)`）。

`PPMissingLayer` 是 `Identity` 的子类而不是 `Module` 的空实现——这很重要：**它在被误调用时会把输入原样返回**，而不是报错。这个选择在权重加载时是必要的（有些遍历会走到它），但在前向时是危险的（若循环范围算错，会静默返回未处理的数据）。

**`make_layers` 里还有一层 offloader 包装。**

```python
+ get_offloader().wrap_modules(layer_fn(...) for idx in range(start_layer, end_layer))
```

`get_offloader()` 让某些部署可以把层参数换出到 CPU（CPU offload）。**这层包装插在「层创建」与「装进 ModuleList」之间**，所以对上层完全透明——`self.layers[i]` 拿到的仍是可调用的模块。

**`make_empty_intermediate_tensors_factory` 生成的是「零填充的占位张量」。**

它按 `["hidden_states", "residual"]` 两个键造零张量。这些张量用于**CUDA graph 捕获**：图捕获需要真实形状的输入缓冲，用零填充即可（结果不参与实际输出）。

`buffers` 参数支持额外的 token-major 跨层状态（如某些模型的额外传递量），形状是 `(batch_size, *buffer.shape[1:])`——**第一维跟着 batch 走，其余维度跟 buffer 模板走**。

**`LlamaForCausalLM` 的 `lm_head` 与 `logits_processor` 都只在末卡创建。**

`tie_word_embeddings` 为真时执行 `self.lm_head = self.lm_head.tie_weights(self.model.embed_tokens)` —— **把 `lm_head` 的权重指向 `embed_tokens`**，从而共享同一份参数。

注意这行在 `if get_pp_group().is_last_rank` 之内，所以只有末卡绑定。而 `embed_tokens` 只在「首卡 or 绑定且末卡」时创建，两者条件正好接上——**绑定情形下末卡确实有一份 `embed_tokens` 可以绑**。

`logit_scale` 用 `getattr(config, "logit_scale", 1.0)` 取，回退到 1.0。所以这个字段也是「新版配置才有」。

**两个 `compute_logits` 的区别只在一个参数。**

`compute_logits` 走完整流程（含跨 TP 的 all-gather 得到完整词表）；`compute_logits_local` 传 `skip_gather=True`，只对本卡持有的词表分片算 logits。

**后者服务于「只要本卡上的 argmax」的场景**——`LlamaForCausalLM` 继承的 `LocalArgmaxMixin` 就是消费者。**这解释了为什么类要同时继承那么多 mixin**：每个 mixin 对应一种「这个模型能以另一种方式被使用」的能力。

**`packed_modules_mapping` 与 `embedding_modules` 是给 LoRA 用的。**

前者声明「哪些合并参数由哪些原始参数组成」，后者声明「哪些是嵌入类模块（LoRA 作用的目标）」。**这两个表与 `hf_to_vllm_mapper` 是同一件事的三个用途**：权重加载、LoRA 适配、嵌入定位。三处都硬编码同样的事实，而代码里**没有任何机制保证它们一致**。

**最后两个类是「零代码子类」，它们的全部内容在注释里。**

```python
class LlamaBidirectionalForSequenceClassification(...):
    # This class sets the correct attention type and pooling type
    # through LlamaBidirectionalConfig.
    pass
```

它们通过 `as_seq_cls_model(LlamaForCausalLM)` 与 `as_embedding_model(LlamaForCausalLM)` 这两个适配器生成基类，然后空实现。**所以「把生成模型变成分类/嵌入模型」这件事完全是配置驱动的**（`LlamaBidirectionalConfig` 设定注意力类型与池化类型），适配器只负责换掉输出层。

`as_seq_cls_model` 在 `TYPE_CHECKING` 下的分支把基类写成 `LlamaForCausalLM` 本身——**这是为类型检查器服务的**，让它知道这两个类有 `LlamaForCausalLM` 的全部接口，而不必理解动态生成的基类。

## 关键取舍

**源码事实：** Q 头要求被 TP 整除（硬断言）；KV 头分「被 TP 整除则切分」与「TP 被 KV 头整除则复制」两种，并用 `max(1, ...)` 兜底。

**源码事实：** `head_dim` 用 `getattr(config, "head_dim", None)` 取，缺失时回退到 `hidden_size // total_num_heads`；`q_size`、`kv_size`、`o_proj.input_size` 三者都基于 `head_dim`。

**源码事实：** `layer_types` 存在时按本层类型决定是否传 `per_layer_sliding_window`；草稿模型需减去 `target_layer_count` 得到相对层索引，该判断靠 `hasattr` 探测。

**源码事实：** 注意力类只在 `EncoderOnlyAttention` 与 `Attention` 之间二选一，其余类型由 `attn_type` 参数进 `Attention` 内部再分派。

**源码事实：** 解码层首次调用时残差为 `None`，把输入自身当残差；之后每层用双输出的 layernorm 返回 `(normed, residual)`，层间传两个张量。

**源码事实：** `LlamaModel` 的 `dynamic_arg_dims` 声明四个参数的维度 0 共享符号 `b`；`mark_unbacked_dims` 被注释掉并留有关于重编译的 TODO。

**源码事实：** `aux_hidden_states` 在首层之前只由首 rank 收集、每层之后无条件调用钩子；`supports_aux_hidden_states_over_pp` 声明可跨 PP 传递。

**源码事实：** 权重名映射用 `WeightsMapper` 的声明表，左键带前导点（匹配中间段）、右值带 shard_id（指定装入合并参数的第几片）。

**源码事实：** `embed_tokens` 在「首卡 / 绑定权重且末卡 / 投机解码需要目标嵌入」三条件取或时创建，否则用 `PPMissingLayer`；`make_layers` 在首尾各填 PPMissingLayer，使每卡层数与全局一致。

**源码事实：** `make_layers` 把真实层包一层 offloader，使 CPU offload 对上层透明。

**源码事实：** `compute_logits` 与 `compute_logits_local` 差别只在 `skip_gather`，后者服务 `LocalArgmaxMixin`。

**源码事实：** `packed_modules_mapping`、`embedding_modules`、`hf_to_vllm_mapper` 三处硬编码同样的事实（哪些参数由哪些组成），代码中无一致性检查。

**源码事实：** 序列分类与嵌入模型是两个 `pass` 子类，行为全由适配器与 `LlamaBidirectionalConfig` 决定；`TYPE_CHECKING` 分支把基类写成 `LlamaForCausalLM` 以让类型检查器理解接口。

**架构推论：** 「模型描述语义、算子决定切分」这条分工是 vllm 能支持任意 TP 度的根本原因。若模型层自己切，那么每加一种并行策略就要改所有模型文件。**判据是「这个知识是建模期确定的还是部署期确定的」**——建模期的写死，部署期的下沉。

**架构推论：** 「每卡层数等于全局层数、绝大部分是空壳」这个选择用空间（几乎为零）换来了**索引无需偏移**。若只保留本地层，那么所有涉及层索引的地方（权重名、`layer_types` 索引、辅助状态收集）都要加偏移量，而偏移量的错算不会有编译错误。**用一点点冗余消除一整类索引错误，是划算的。**

**架构推论：** 三处硬编码同一份「哪些参数被合并」的事实，是**耦合没有被表达出来的典型**。正确做法是让 `hf_to_vllm_mapper` 成为唯一来源，`packed_modules_mapping` 从它派生。现状下新增一个合并参数需要改三处，漏一处的症状分别是：权重加载失败（mapper 漏）、LoRA 报错（packed 漏）、嵌入适配失败（embedding 漏）。

**架构推论：** 「零代码子类 + 配置驱动」把差异推到了配置层。收益是新模型只需要写一份 `LlamaBidirectionalConfig`；代价是**读代码看不出这两个类到底和父类差在哪**。注释在这里承担了全部信息量，而它只有一行。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/model_executor/models/llama.py` | `LlamaMLP` | 门控 MLP，合并投影加可选融合的激活量化 |
| `vllm/model_executor/models/llama.py` | `LlamaAttention` | TP 切分的头数计算、合并 QKV、RoPE、注意力调用 |
| `vllm/model_executor/models/llama.py` | `LlamaAttention._init_rotary_emb` | 用 Neox 风格构造 RoPE |
| `vllm/model_executor/models/llama.py` | `LlamaDecoderLayer` | 残差双张量传递与算子顺序 |
| `vllm/model_executor/models/llama.py` | `LlamaModel` | 骨架：嵌入、层列表、末端归一化、辅助状态收集 |
| `vllm/model_executor/models/llama.py` | `LlamaModel.forward` | PP 分支、层遍历、中间张量打包 |
| `vllm/model_executor/models/llama.py` | `LlamaForCausalLM` | 因果语言模型，含 lm_head 绑定与 logits 处理 |
| `vllm/model_executor/models/llama.py` | `LlamaForCausalLM.compute_logits_local` | 跳过 all-gather 的本地 logits |
| `vllm/model_executor/models/llama.py` | `LlamaBidirectionalForSequenceClassification` | 零代码子类，行为由配置决定 |
| `vllm/model_executor/models/utils.py` | `make_layers` | 用 PPMissingLayer 填齐，并包 offloader |
| `vllm/model_executor/models/utils.py` | `PPMissingLayer` | `Identity` 子类，空壳但可被误调用 |
| `vllm/model_executor/models/utils.py` | `make_empty_intermediate_tensors_factory` | 造零填充占位张量供图捕获 |
| `vllm/model_executor/models/utils.py` | `maybe_prefix` | 名字前缀可选拼接 |
| `vllm/model_executor/models/utils.py` | `AutoWeightsLoader` | 按 mapper 加载并堆叠权重 |
| `vllm/model_executor/layers/linear.py` | `QKVParallelLinear` | QKV 合并投影，按 TP 切输出 |
| `vllm/model_executor/layers/linear.py` | `MergedColumnParallelLinear` | 门控/上投影合并，按 shard_id 切 |
| `vllm/model_executor/layers/linear.py` | `RowParallelLinear` | 行并行输出投影 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `VocabParallelEmbedding` | 词表并行嵌入 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `ParallelLMHead` | 词表并行输出头，可绑定嵌入权重 |
| `vllm/model_executor/layers/rotary_embedding/__init__.py` | `get_rope` | 按顺序与配置分发 RoPE 实现 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：models/llama.py](https://github.com/vllm-project/vllm/blob/main/vllm/model_executor/models/llama.py)
- [LLaMA 论文（Touvron et al., 2023）](https://arxiv.org/abs/2302.13971)
- [vllm 文档：模型实现指南](https://docs.vllm.ai/en/latest/contributing/model/basic.html)

## 与主核 / AI 的关系

**「语义在模型层、切分在算子层」与主核的存储与访问方法分工同构。** 数据库的表定义只描述列与类型，页格式与压缩方式由访问方法决定。同一张表换一种 AM 就能换物理格式。vllm 的模型换一个 TP 度就换参数形状，模型代码不变。**这条分工的价值在于「一份逻辑正确性覆盖所有部署形态」。**

**「空壳占位保持全局索引」与主核的段页式映射同类。** 数据库用连续的页号映射到碎片化的物理位置，好处是逻辑页号连续、无需特判。`PPMissingLayer` 让层索引在所有卡上连续，目的相同。**用一层间接（占位/映射表）换取「索引无需特判」，是消除一类错误的通用手法。**

**「残差双张量传递」与主核的「就地更新 vs 复制」取舍类似。** 融合「加残差 + 归一化」省掉一次全量读写，代价是接口从「一个张量」变成「两个张量」，并且第一层必须特例处理。**性能优化一旦改变接口形状，就会引入特例**，而特例是最容易被后续改动破坏的地方。

**「三处硬编码同一事实」与主核的系统表冗余同类。** 数据库里 `pg_class.relnatts` 与 `pg_attribute` 的行数必须一致，靠维护机制保证而不是靠类型。vllm 这三个表连维护机制都没有，只能靠人工。**当冗余不可避免时，至少要有一个「谁是权威」的声明**——这里缺的正是这个。

对 agent 工程最实用的五条。其一：**切分交给最了解运行期的层**，模型代码只描述语义，否则每加一种并行策略要改遍所有模型。其二：**用占位保持全局索引连续**，比在每处判断「本卡有没有这层」更不容易错。其三：**合并参数的组成事实只能有一处权威**，三处硬编码意味着三种不同的漏改故障。其四：**融合优化会改变接口形状**，接受它的同时要为特例写清楚理由（第一层的残差特例就是范例）。其五：**动态维度要显式声明共享符号**，否则编译缓存会被批大小打散。
