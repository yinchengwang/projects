---
project: vllm
feature: sampling
title: 采样与结构化输出
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/sample/ops/topk_topp_sampler.py
    symbols: [apply_top_k_top_p, flashinfer_sample]
  - path: vllm/v1/structured_output/backend_types.py
    symbols: [StructuredOutputBackend, StructuredOutputOptions]
---

# 采样与结构化输出

## 一句话职责

采样这一步干两件事：把 logits 按 top-k / top-p 削成候选集再抽一个 token；而结构化输出负责把那一步的 logits 预先遮住不让违规 token 出现——两者最终都作用在同一块 logits 上。

## 机制是什么

**采样是「先过滤再抽样」，过滤与抽样都可以换实现。** `apply_top_k_top_p` 只做过滤（把不该选的 token 置成 `-inf`），抽样是对剩下的分布做加权随机。这两步被拆开是因为过滤有极多实现（Triton、PyTorch、CPU C++ 内核、FlashInfer），而抽样的语义必须一致。

**结构化输出是「在过滤之前再加一层遮罩」。** 它靠 `fill_bitmask` 填一个「哪些 token 在语法上合法」的位图，再把不合法的位置也置成 `-inf`。所以它不是采样的一部分，而是**采样前的约束层**——顺序是：语法位图 → top-k/top-p → 抽样。

这也是为什么这两个概念被放在同一页：它们都改写 logits，且都必须与 batch 形状逐行对齐。

## 怎么实现的

**`apply_top_k_top_p` 是一个五路分派的薄壳。**

```python
if p is None and k is None:
    return logits
if HAS_TRITON:
    return apply_top_k_top_p_triton(logits, k, p, has_topp_only_rows=has_topp_only_rows)
if current_platform.is_cpu() and current_platform.get_cpu_architecture() in (ARM, POWERPC, S390X):
    return apply_top_k_top_p_cpu(logits, k, p)
is_cpu = current_platform.is_cpu()
return apply_top_k_top_p_pytorch(logits, k, p, allow_cpu_sync=is_cpu)
```

几个要点：

- **两个参数都为空时直接返回原张量**，不做任何拷贝。
- **Triton 优先**——只要装了 Triton 就走融合内核，不看平台。
- **CPU 又分了两种**：ARM / POWERPC / S390X 走 C++ 三元搜索内核；其它 CPU 走 PyTorch 实现但允许 CPU 同步。
- 参数 `has_topp_only_rows` 的 docstring 给了一条前置条件：「must be True if any row has p < 1 and k >= logits.shape[-1]（没有生效的 top-k）」。**这是调用方必须正确填的一个提示位**，用来让内核走不同的分支。

**`allow_cpu_sync` 是一个为了「不排序」而付出的同步代价。** 在 `apply_top_k_top_p_pytorch` 里：

```python
if p is None:
    if k is None: return logits
    if allow_cpu_sync:
        return apply_top_k_only(logits, k)
```

只有「纯 top-k 且允许 CPU 同步」时才走 `apply_top_k_only`——它用 `topk` 替代全排序，但需要把 `k` 读到 CPU 上比较。`apply_top_k_only` 的 docstring 直接把这个代价写明：「it involves a GPU->CPU sync which can be detrimental for async scheduling performance」。

**`apply_top_k_only` 的实现有一段必须有的技巧。**

```python
no_top_k_mask = k == logits.shape[1]
k = k.masked_fill(no_top_k_mask, 1)
max_top_k = k.max()
k_index = k.sub_(1).unsqueeze(1)
top_k_mask = logits.topk(max_top_k, dim=1).values.gather(1, k_index.long())
top_k_mask.masked_fill_(no_top_k_mask.unsqueeze(1), -float("inf"))
return logits.masked_fill_(logits < top_k_mask, -float("inf"))
```

**「关闭 top-k」的表达是 `k == vocab_size`，不是 `k == 0`。** 因为 `topk(0)` 无意义、`gather` 也要有合法下标。所以先把这些行改成 `1` 以便统一 `topk(max_top_k)`，算完再用掩码把它们的阈值改成 `-inf`（等于不过滤）。CPU 内核那边也断言了同一约定：「k values must be > 0; use k=vocab_size to disable top-k filtering」。

**PyTorch 版排序后同时做 top-k 与 top-p，最后再 scatter 回原位置。**

```python
logits_sort, logits_idx = logits.sort(dim=-1, descending=False)
```

注意是**升序**排序。这个选择让 top-p 的实现变简洁：

```python
probs_sort = logits_sort.softmax(dim=-1)
probs_sum = torch.cumsum(probs_sort, dim=-1, out=probs_sort)
top_p_mask = probs_sum <= 1 - p.unsqueeze(dim=1)
top_p_mask[:, -1] = False
```

升序排列后，累计和从「最不可能的 token」开始累加；`<= 1 - p` 就是「把尾部累积概率不超过 1-p 的那些全砍掉」。而 `top_p_mask[:, -1] = False` 保证**至少留一个**——否则极端情况下全被砍掉会得到空分布。这一行注释写「at least one」。

升序还有个好处：top-k 的掩码计算变成 `logits_sort.size(1) - k` 作为下标去 gather 阈值，比降序后再换算更直接。

最后的 `logits.scatter_(dim=-1, index=logits_idx, src=logits_sort)` 用排序时记下的索引把结果散回原位置——**整个过程只排一次序**，且是原地写回。

**`flashinfer_sample` 把过滤与抽样合成一步，靠拒绝采样避掉排序。**

```python
if k is None:
    probs = logits.softmax(dim=-1, dtype=torch.float32)
    next_token_ids = flashinfer.sampling.top_p_sampling_from_probs(probs, p, deterministic=True)
elif p is None:
    probs = logits.softmax(dim=-1, dtype=torch.float32)
    next_token_ids = flashinfer.sampling.top_k_sampling_from_probs(probs, k, deterministic=True)
else:
    next_token_ids = flashinfer.sampling.top_k_top_p_sampling_from_logits(logits, k, p, deterministic=True)
```

三条分支按「只有 p / 只有 k / 两者都有」分。**注意第三条直接用 logits 而不先 softmax**——因为该接口内部自己处理。前两条先 `softmax(..., dtype=torch.float32)`，**显式指定 float32 精度**，避免低精度下的概率误差。

docstring 说明了它的收益：「Statistically, this function is equivalent to top-k/top-p filtering followed by weighted random sampling. However, this function is faster because it avoids sorting the logits tensor via rejection sampling.」——**统计等价但不用排序**，代价是它把过滤与抽样绑在一起，无法只做过滤。

三个分支都传 `deterministic=True`。**这个参数的含义是「用确定性算法而不是近似」**，通常意味着更多的计算量换取可复现。

`return next_token_ids.view(-1)` 把输出压平——内核返回的可能是 `[B, 1]`。

**`flashinfer_sampler_supported` 是一段有层次的可用性判定，判定顺序本身就是知识。**

1. 非 CUDA → `False`（无日志）。
2. `VLLM_USE_FLASHINFER_SAMPLER` 为关 → `False` + `info_once`。
3. `has_flashinfer()` 为假 → 原因「FlashInfer kernels are disabled」。
4. `FlashInferBackend.supports_compute_capability(capability)` 为假 → 原因「unsupported compute capability ...」。
5. `num_compute_units <= 16` → 原因「top-k masking requires more than 16 SMs; device has N」。注释给了根因：「FlashInfer 0.7+ rejects multi-CTA top-k masking on <=16 SMs because its cross-CTA software barrier cannot guarantee forward progress.」——**这是硬件并行度导致的正确性问题，不是性能优化**。
6. 否则查 JIT 能否编译（`_flashinfer_jit_unsupported_reason`）。

第 6 步那个函数有一大段注释，值得记住：

> FlashInfer swallows arch-detection errors when building its compilation context (e.g. SM 12.x with a CUDA toolkit older than 12.9), leaving an empty target-arch set that makes every JIT spec fail with a misleading "requires sm75 or higher" error at first use — killing the engine during startup profiling.

所以它先 `check_cuda_arch()`，失败后再从 `CompilationContext._normalize_cuda_arch(capability.major, capability.minor)` **重新推导真实错误**（例如「SM 12.x requires CUDA >= 12.9」），把这个真实原因返回。而且「FlashInfer internals changed」时用一个空 `except` 保住原错误——`return reason` 用的是外层捕获的字符串。

**「用户显式设了就报错」这条规则很实用。**

```python
if envs.is_set("VLLM_USE_FLASHINFER_SAMPLER"):
    raise RuntimeError(f"FlashInfer top-p/top-k sampling unavailable: {unsupported_reason}. Unset VLLM_USE_FLASHINFER_SAMPLER=1.")
logger.warning_once("... falling back. Set VLLM_USE_FLASHINFER_SAMPLER=0 to silence.")
```

**没显式设 → 静默降级；显式设了 → 直接失败。** 这两条路的分歧是「意图是否明确」：用户没说要用，降级是善意；用户明说要用了却用不了，那继续跑就是在骗人。

**`flashinfer_sampler_supported` 的 docstring 还有一条「调用方必须额外保证」的约束**：`logprobs_mode` 不能要求「top-k/top-p 之后的 logits/logprobs」，因为 FlashInfer 不暴露它们。这是把一部分前置条件交给调用方——典型的「能力判定只回答自己能回答的部分」。

**XPU 的融合内核与 FlashInfer 是同一位置的两个实现。**

`xpu_sampler_supported` 只有两条判据（是 XPU、开关没关）。`xpu_sample` 的实现里有一处正确的 RNG 处理：

```python
generator = torch.xpu.default_generators[logits.device.index]
state = generator.get_state()
seed, offset = state.view(torch.int64).tolist()
...
torch.ops.vllm.xpu_topk_topp_sampler(sampled, logits_to_return, logits, k, p, logprobs_mode, seeds)
offset = (offset + logits.numel() + 3) // 4 * 4
state.view(torch.int64)[1] = offset
generator.set_state(state)
```

**自定义内核内部消费了随机数，所以要把生成器的 offset 手动推进。** `(offset + numel + 3) // 4 * 4` 是向上取整到 4 的倍数，注释写「pytorch: offset must be multiple of 4」。如果不做这一步，下一次调用会重复使用同一批随机数——**这个 bug 不会报错，只会让采样结果可预测**。

docstring 还写明两个限制：「never materializes the probability tensor」（不生成概率张量，省显存）、「per-request generators aren't supported」（因为只用设备默认生成器）。

**`apply_top_k_top_p_cpu` 分三种情形，且有一处硬断言。**

```python
assert (k > 0).all(), "k values must be > 0; use k=vocab_size to disable top-k filtering"
```

然后按「只有 k / 只有 p / 两者都有」调三个不同的 C++ 内核。**三个内核而不是一个，是因为三元搜索的边界条件不同**——单参数的搜索比双参数的简单得多。所有路径都先 `.contiguous()`，因为 C++ 侧按连续内存访问。

**结构化输出的接口被切成「引擎级」与「请求级」两层，这是这一页的第二个核心。**

`StructuredOutputBackend`（引擎级，`@dataclass`）持三个字段：`vllm_config`、`tokenizer`、`vocab_size`，只有一个方法 `compile_grammar(request_type, grammar_spec, stop_token_ids) -> StructuredOutputGrammar`。

`StructuredOutputGrammar`（请求级，`ABC`）六个方法：`accept_tokens`、`validate_tokens`、`rollback`、`fill_bitmask`、`is_terminated`、`reset`。

**为什么要分两层**：编译语法是昂贵的（要解析 JSON Schema、构建 FSM），但同一个语法可以被同一批里的多个请求共享；而语法状态是每请求独立的（各自走到不同的位置）。所以引擎级负责**编译与缓存**，请求级负责**状态推进**。`StructuredOutputKey = tuple[StructuredOutputOptions, str]` 就是那个缓存键。

**六个方法里有三对值得对照着看。**

- `accept_tokens` 与 `validate_tokens`：前者**推进状态**，后者只看不推进。`accept_tokens` 的 docstring 有一条重要约束：「A false result must leave the grammar state unchanged.」——**拒绝必须是原子的**，否则状态机就错位了。`validate_tokens` 的返回值是「输入 token 列表的前缀」，所以它同时回答了「能接受几个」。
- `fill_bitmask` 与 `reset`：前者把当前合法 token 写进位图，后者回到起点。
- `rollback` 与 `is_terminated`：前者用于投机解码被拒时回退，docstring 特意说「Will also revert counters for the number of processed tokens」——**回退要连计数器一起退**，只回状态机是不够的。

`compile_grammar` 的 `stop_token_ids` 参数有一句精确的说明：「the request's EOS and user stop-token ids, masked until the grammar terminates」。也就是说**在语法未完成之前，停止 token 也要被遮住**——否则模型会在 JSON 还没写完整时就输出 EOS。这是个容易被忽略的正确性细节。

`StructuredOutputOptions` 六个取值：`JSON`、`JSON_OBJECT`、`REGEX`、`GRAMMAR`、`CHOICE`、`STRUCTURAL_TAG`。前四个是标准形态（JSON Schema / 任意 JSON 对象 / 正则 / 语法），`CHOICE` 是「只能选这几个字符串之一」，`STRUCTURAL_TAG` 是 vllm 自己的结构化标签格式。

## 关键取舍

**源码事实：** `apply_top_k_top_p` 五路分派：双空返回原张量、Triton 优先、ARM/POWERPC/S390X 走 C++ 内核、其余 CPU 走 PyTorch 且允许同步、非 CPU 走 PyTorch。

**源码事实：**「关闭 top-k」的约定是 `k == vocab_size`，不是 0；CPU 内核对此有硬断言。

**源码事实：** PyTorch 版用**升序**排序，top-p 的掩码是 `probs_sum <= 1 - p`，并强制 `top_p_mask[:, -1] = False` 保证至少留一个。

**源码事实：** `apply_top_k_only` 只在「纯 top-k 且 `allow_cpu_sync`」时使用，docstring 明说它引入 GPU→CPU 同步。

**源码事实：** `flashinfer_sample` 三情形分派，前两种先以 float32 softmax，第三种直接用 logits；全部传 `deterministic=True`；docstring 说它与「过滤后加权随机」统计等价但避免排序。

**源码事实：** `flashinfer_sampler_supported` 的判定顺序是 CUDA → 环境开关 → kernels 可用 → 计算能力 → SM 数 > 16 → JIT 编译；SM ≤ 16 被拒的原因是跨 CTA 软件屏障无法保证前进。

**源码事实：** 环境变量**显式设置**时若不可用则抛 `RuntimeError`，未显式设置时只 warning 并降级。

**源码事实：** `xpu_sample` 在调用自定义内核后手动把生成器 offset 推进到「4 的倍数」。

**源码事实：** 结构化输出分引擎级 `StructuredOutputBackend`（编译 + 缓存）与请求级 `StructuredOutputGrammar`（状态推进）；缓存键是 `(选项, 语法串)`。

**源码事实：** `accept_tokens` 返回 `False` 时必须保持状态不变；`rollback` 要连已处理 token 计数一起回退；`compile_grammar` 的 `stop_token_ids` 在语法终止前一直被遮住。

**架构推论：** 「过滤」与「抽样」可分离是这一层能做多实现的前提。若把 top-k/top-p 与抽样写在一起（像 FlashInfer 那样），就无法只做过滤（例如需要返回 top-k 之后的 logprobs）。**接口切在哪里，决定了后续能加什么功能**——vllm 保住了「只过滤」这条路，FlashInfer 那条则换来了性能。

**架构推论：** 升序排序 + 尾部累积是 top-p 的正确实现方式，而降序 + 头部累积也能做但要多一次减法。选升序的真正收益是**顺带让 top-k 的阈值 gather 更直接**。这类「换个方向让两种操作都变简单」的选择，在写数值代码时值得刻意找一找。

**架构推论：** 结构化输出的两分层是「编译贵、状态便宜」的直接映射。缓存键用 `(选项, 语法串)` 而不是 request id，说明**同一个语法在多请求间共享**——这是把「一次 JSON Schema 解析」摊到整个 batch 上的关键。若每个请求各编译一遍，相同 schema 的并发请求会做 N 倍无用功。

**架构推论：** 「显式设置就报错、未设置就降级」这条规则值得在所有「可优化但非必需」的能力上复用。它的判据是**用户的意图是否已经被表达**：没表达时替用户选更稳的路，表达了就不要偷偷违背。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `apply_top_k_top_p` | 过滤入口，五路分派 |
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `apply_top_k_top_p_pytorch` | 升序排序 + 尾部累积的完整实现 |
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `apply_top_k_only` | 纯 top-k 免排序路径（代价是 CPU 同步） |
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `apply_top_k_top_p_cpu` | CPU C++ 三元搜索内核的三情形分派 |
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `flashinfer_sample` | 拒绝采样版过滤+抽样一体 |
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `flashinfer_sampler_supported` | 分层可用性判定与「显式设置即报错」 |
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `_flashinfer_jit_unsupported_reason` | 还原被 FlashInfer 吞掉的真实架构错误 |
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `xpu_sample` | XPU 融合内核，含生成器 offset 手动推进 |
| `vllm/v1/sample/ops/topk_topp_sampler.py` | `register_top_k_top_p_warmups` | 注册内核预热（CUDA 额外预热三个子内核） |
| `vllm/v1/structured_output/backend_types.py` | `StructuredOutputBackend` | 引擎级：编译语法为请求级语法对象 |
| `vllm/v1/structured_output/backend_types.py` | `StructuredOutputGrammar` | 请求级：接受/校验/回退/填位图/终止/重置 |
| `vllm/v1/structured_output/backend_types.py` | `StructuredOutputOptions` | 六种约束形态 |
| `vllm/v1/structured_output/backend_types.py` | `StructuredOutputKey` | 语法缓存键（选项 + 语法串） |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/v1/sample/ops/topk_topp_sampler.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/sample/ops/topk_topp_sampler.py)
- [vllm 源码：vllm/v1/structured_output/backend_types.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/structured_output/backend_types.py)
- [论文：Nucleus Sampling（top-p 采样）](https://arxiv.org/abs/1904.09751)

## 与主核 / AI 的关系

「先过滤再抽样」与主核里「先按条件筛行再聚合」是同一种两段结构：**第一段缩小候选，第二段在候选上做运算**。主核的筛选用索引与谓词，这里的筛选用 top-k/top-p 阈值；两者都有一个共同的实现陷阱——**筛选阈值的作用域必须与候选集对齐**。

**位图约束 vs 主核的 CHECK 约束。** 数据库的 `CHECK` 在写入时逐行判定合法与否；结构化输出的位图在**生成每一步**判定下一个 token 合法与否。前者是事后的门禁（不合法就拒），后者是事中的引导（不合法的直接不给概率）。**判据相同、时机不同，效果差别很大**：事后拒绝会让模型白生成一遍，事中遮蔽则从根上不产生违规。

**两级接口（编译 / 状态）vs 主核的 prepared statement。** 预处理语句把「解析 + 生成计划」与「执行」分开，同一份 SQL 文本可以在多次执行间共享计划；结构化输出把「编译语法」与「推进状态」分开，同一个 schema 可以在多个请求间共享 FSM。**两者都在利用「编译产物可复用」这一性质**，且都需要一个缓存键（SQL 文本 / 语法串）。

**「显式设置就报错」vs 主核的 `SET` 参数。** 数据库对显式设置但无法生效的参数通常直接报错（`unrecognized configuration parameter`）；对未设置的参数用编译默认值。**「用户说了就得算数」是配置系统的基本礼貌**，静默忽略是最差的选择。

对 agent 工程最直接的价值有四条。其一：**结构化输出要在采样前遮蔽而不是生成后校验**，否则浪费 token 且无法保证格式。其二：**语法未终止前要遮住 EOS 与用户停止符**，否则模型会在 JSON 半截时就收尾——这是最容易漏的一条。其三：**「关闭某功能」要有一个明确的哨兵值**（`k == vocab_size`），而不是靠 0 或负数走特殊分支，否则每个实现都要各写一遍特判。其四：**自定义内核消费随机数后必须手动推进生成器状态**，否则采样结果会静默变得可预测。
