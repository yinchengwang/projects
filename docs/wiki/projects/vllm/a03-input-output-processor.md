---
project: vllm
feature: input-output-processor
title: 输入输出加工
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/engine/input_processor.py
    symbols: [InputProcessor]
  - path: vllm/v1/engine/output_processor.py
    symbols: [OutputProcessor, RequestState, StreamingUpdate]
  - path: vllm/v1/engine/detokenizer.py
    symbols: [IncrementalDetokenizer, FastIncrementalDetokenizer, SlowIncrementalDetokenizer]
---

# 输入输出加工

## 一句话职责

在请求进引擎之前把它变成合法的 `EngineCoreRequest`（`InputProcessor`），在引擎吐 token 之后把它变成文本增量与 `RequestOutput`（`OutputProcessor` + 三个 detokenizer），两端都是**校验密集**的地方。

## 机制是什么

这一层夹在 API 层与引擎核心之间，做的是纯 CPU 的翻译与校验工作。两端各有一个贯穿全程的主题：

**输入侧的主题是「拒绝」。** `InputProcessor.process_inputs` 有十几处 `raise VLLMValidationError`，覆盖参数合法性、LoRA 授权、DP rank 范围、提示词长度、logprob 行数、多模态哈希类型。**在 GPU 上跑一个非法请求的代价远高于在 CPU 上拒绝它**——所以这一层宁可多查。

**输出侧的主题是「不变量守恒」。** 引擎给的是 token id 列表与终止原因，客户端要的是文本增量。中间隔着三件事：增量解码、停止串检测、以及「已发出的文本不能再改」。这三件事每个都可能破坏「客户端看到的内容与最终内容一致」这条不变量。

两端的代价结构完全不同：输入侧每个请求跑一次（便宜、可尽情校验），输出侧**每个 token 跑一次**（昂贵、必须极致精简）。这个差别解释了为什么输出侧有一个反复出现的注释：「vLLM V1 minimizes the number of python loops over the full batch」。

## 怎么实现的

### InputProcessor：一条直线，十几道闸

`process_inputs` 的顺序是这样的：

**先验参数与 LoRA。** `_validate_params(params, supported_tasks)` 返回一个 `watermarking` 布尔值——**校验函数带返回值**，因为水印这项开关既要在校验时确定（是否支持）又要在此后应用。`_validate_lora` 单独校验 LoRA 授权。

**校验 DP rank 范围。** 这里有一处容易错的细节：

```python
num_ranks = dp_local_size if parallel_config.local_engines_only else dp_size
if data_parallel_rank is not None and not (0 <= data_parallel_rank < num_ranks):
    raise VLLMValidationError(...)
```

**上限取决于「是否只用本地引擎」**——本地模式下只有本地 rank 可用，全局模式下才按完整 dp_size 算。这个区分如果搞错，要么会拒绝合法的远端 rank，要么会放行不存在的本地 rank。

**提示词的两种入参形态。** 若 `prompt` 是带 `type` 键的 dict，就认为已经是 `EngineInput`，直接采用（并从中取 `arrival_time`）；否则走「裸 prompt」的兼容路径：打一条 `warning_once` 说明「传裸 prompt 已废弃，应该传 `Renderer.render_cmpl()` / `render_chat()` 的输出」，然后用 `parse_model_prompt` + `renderer.render_cmpl` 现场渲染。

这条兼容路径的存在说明**上层正在从「传原始 prompt」迁移到「传渲染结果」**，而渲染（含 chat 模板套用、多模态占位符替换）已经从引擎里挪到了前端。

**校验失败要给多模态缓存「撤销记录」。** 这一段是全文件最细的：

```python
try:
    current_platform.validate_request(engine_input, params)
    self._validate_model_inputs(encoder_input, decoder_input)
except Exception:
    # Rendering recorded the multimodal items as sent to the engine,
    # which a rejected request never does: resend them next time.
    self.renderer.queue_mm_cache_invalidations([...])
    raise
```

注释解释了逻辑：渲染阶段已经把多模态项「登记为已发送」，而被拒的请求其实从未发送——所以要把这些哈希登记为「下次需要重发」，否则下次同样的图片会命中一个空的缓存条目。**这是「乐观写入 + 失败回滚」的典型形态，且回滚点必须放在异常路径上而不是成功路径上。**

**采样参数的每一处修改都发生在 `clone()` 之后。** 注释写明理由：「`params` may be shared across prompts, requests and even engines by the caller」——调用方可能复用同一个 `SamplingParams` 对象，所以水印、`max_tokens` 补全、`update_from_generation_config` 全都作用在 request-local 的副本上。

**`max_tokens` 的补全有一条隐含的校验顺序**：

```python
if sampling_params.max_tokens is None:
    sampling_params.max_tokens = self.model_config.max_model_len - prompt_len
    # min_tokens is not checked while max_tokens is unset.
    if sampling_params.min_tokens > sampling_params.max_tokens:
        raise VLLMValidationError(...)
```

注释说得很清楚：`min_tokens` 的校验**只在 `max_tokens` 原本未设时才在这里做**。若用户显式给了 `max_tokens`，这项校验由别处负责。把校验分散在两处是常见但危险的写法——**它要求两处的条件互斥且穷尽，而这只能靠人记住。**

**`_validate_prompt_len` 的错误消息分两版。** 支持多模态时提示「`max_model_len` 不能小于文本 token 数加上多模态 token 数，图像 token 数还取决于图像数量甚至宽高比」；不支持时只提文本。**同一处错误按上下文给不同建议**，这是可运维性上的细节。

### OutputProcessor：唯一允许遍历整批的函数

`process_outputs` 的 docstring 里有一段全大写的开发者须知：

> vLLM V1 minimizes the number of python loops over the full batch to ensure system overheads are minimized. This is the only function that should loop over EngineCoreOutputs. If you need to touch every element of the batch, do it from within the loop below.

**这是一条架构级的纪律，用注释而不是机制来保证。** 它存在的原因是：每步都要遍历几百个请求的输出，Python 的循环开销会直接吃掉吞吐。

循环体里的分支顺序经过设计：

**第一道：请求状态找不到就跳过。** 注释「Ignore output for already-aborted request」。**取消的请求会继续产出 token**（引擎可能还没处理完取消），所以这里必须能安静丢弃。

**第二道：错误携带在输出上。** 若 `engine_core_output.error` 非空且请求有队列，就把异常塞进队列并 `_finish_request`。注释「Rejected on arrival, e.g. by a paused engine; nothing was run」——所以这个错误在**引擎侧就被拒了**，不是执行中失败。流式输入请求还要额外加入 `reqs_to_abort`，因为「此后发来的输入 chunk 可能重建会话」。

**第三道：统计与预填观察。** `is_prefilling` 为真时调 `observe_prefill`，然后立刻置假。`observe_prefill` 里有一处覆盖逻辑：远程预填的缓存命中数**优先于** decode 阶段的缓存命中数（注释「The prefill worker's count overrides decode cache hits」）。

**第四道：增量解码与停止串。** 这是核心：

```python
num_prev_tokens = req_state.detokenizer.num_output_tokens()
stop_string = req_state.detokenizer.update(new_token_ids, finish_reason == FinishReason.STOP)
if stop_string:
    finish_reason = FinishReason.STOP
    stop_reason = stop_string
    self._trim_surplus_tokens(req_state, engine_core_output, num_prev_tokens)
```

注意**`finish_reason` 在这里被就地改写**：引擎说「长度到了」或者「正常结束」，但若解码器发现文本里出现了停止串，就改成 `STOP`。**文本层的判定覆盖了 token 层的判定**，因为「停在哪」对用户而言是文本语义。

`_trim_surplus_tokens` 处理一步内产出多个 token 的情形：停止串可能落在中间某个 token 上，后面的 token 要丢掉。它的实现是「解码器保留了多少 token」减去「进来多少 token」，差值为正就截断。

**第五道：流式输入的块边界。** 这是全函数最绕的一处：

```python
streaming_chunk_boundary = req_state.streaming_input and (
    finish_reason not in (FinishReason.ABORT, FinishReason.ERROR)
)

if request_output := req_state.make_request_output(...):
    if streaming_chunk_boundary:
        request_output.finished = False
    ...

if finish_reason is not None:
    if streaming_chunk_boundary:
        if req_state.input_chunk_queue:
            update = req_state.input_chunk_queue.popleft()
            req_state.apply_streaming_update(update)
        else:
            req_state.input_chunk_queue = None
    else:
        self._finish_request(req_state)
        if req_state.streaming_input or not engine_core_output.finished:
            reqs_to_abort.append(req_id)
```

**流式输入下「结束」有两种含义**：这一块的生成结束了（还要继续喂新块），和整个会话结束了。代码用 `streaming_chunk_boundary` 区分：若是块边界，就把 `finished` 强行改回 `False`（**对客户端而言没结束**），然后从队列里取下一条 `StreamingUpdate` 应用，重置 `is_prefilling`。

若不是块边界（真的结束了），才 `_finish_request`，并且——关键的一句——**若引擎侧没有说已结束（`not engine_core_output.finished`），把请求加入待取消列表**。因为停止串是这里检测出来的，引擎还在继续生成，必须通知它停。

### 三个 detokenizer：快、慢、抽象

`IncrementalDetokenizer` 是抽象基类但**几乎不实现任何东西**（四个方法都是 `raise NotImplementedError`）。真正的公共逻辑在 `BaseIncrementalDetokenizer` 里，两个具体实现继承它：

| 类 | 依赖 | 解码方式 |
|---|---|---|
| `FastIncrementalDetokenizer` | `TokenizersBackend`（HuggingFace tokenizers 的 Rust 后端） | `DecodeStream.step` 增量流式解码 |
| `SlowIncrementalDetokenizer` | `TokenizerLike`（任意 Python tokenizer） | `detokenize_incrementally` 带偏移量重解 |

**「快」的实质是原生增量解码器。** `FastIncrementalDetokenizer` 构造时建一个 `DecodeStream`，并用 `get_decode_stream_seed(...)` 把**提示词尾部**喂进去做种子：

```python
# Use native prefill to prime the decode stream with the prompt tail.
self.stream = tokenizers.decoders.DecodeStream(
    ids=get_decode_stream_seed(self.tokenizer, request.prompt_token_ids, self.skip_special_tokens),
    skip_special_tokens=self.skip_special_tokens,
)
```

**为什么必须喂提示词尾部**：BPE 的字节级解码在边界处依赖前文。例如某些 token 只在特定前驱下才产生空格。不喂种子，第一个输出 token 的解码就会错。这个种子逻辑是一处容易漏掉的正确性细节。

`_protected_step` 是容错层，有两个 `except`：

- `(OverflowError, TypeError)` → 记 exception 并返回 `None`（对应 issue 21951 的罕见溢出）。该 token 的文本被丢弃但不中断流。
- 其他异常，若消息以 `INVALID_PREFIX_ERR_MSG` 开头 → **重建 `DecodeStream` 并重试一次**。注释解释原因：「tokenizer 可能产生非单调、非法 UTF-8 的输出，破坏 tokenizers 的 DecodeStream 内部状态」（issue 17448）。

第二条尤其值得注意：**它的恢复方式是「丢弃解码器状态、从零开始」**，代价是后续解码可能重新产生已经发出去的文本。这个代价由 `get_next_output_text` 的偏移量机制承担——因为它是按 `_last_output_text_offset` 切片发送的，重建后 `output_text` 变短会让切片返回空串（`last_offset < length` 不成立），于是客户端不会收到重复内容。

`SlowIncrementalDetokenizer` 用「保留全部 token id + 三个偏移量（`tokens` / `prefix_offset` / `read_offset`）」的做法，每次 `detokenize_incrementally` 重新解码一小段。注释说明 `tokens` 会「Grows by one string per output token, in place」，所以做了 `gc_untrack`——**一个每 token 都在增长的列表如果在 GC 追踪范围内，每次扫描的成本会随生成长度线性上升。**

### 停止串的三处细节

**一，`stop_buffer_length` 的算法是 `max(len(s) for s in stop) - 1`。** 为什么要减一：流式输出时不能把可能是停止串**前缀**的字符发出去，否则一旦后面真的匹配上，已经发的字符没法收回。缓冲区长度取「最长停止串减一」，因为只要保留这么多尾部字符，任何潜在的停止串起点都不会被提前泄露。

而 `include_stop_str_in_output` 为真时缓冲为 0——因为停止串本身要显示，没什么可藏的。

**二，`get_next_output_text` 用 `finished` 绕过缓冲。**

```python
buffer_length = 0 if finished else self.stop_buffer_length
```

**完成了就不再缓冲，一次性把尾巴也吐出去。** 否则用户会看到一段「少了最后几个字符」的文本，然后流就结束了。

**三，`min_tokens` 影响停止串的检查起点。**

```python
stop_check_offset = token_offsets[max(self.min_tokens - num_prev_tokens, 0)]
stop = check_stop_strings(output_text=self.output_text, new_char_count=..., ...)
```

`token_offsets` 是「每个 token 边界处的文本偏移」，只在有停止串时才构建（`token_offsets = [len(self.output_text)] if self.stop else None`）。用 `min_tokens` 之前的偏移作检查起点，意味着**`min_tokens` 以内的文本不参与停止串匹配**。注释引用了对应 PR——这是为支持「强制生成至少 N 个 token」而加的。

## 关键取舍

**源码事实：** `InputProcessor` 的每一处采样参数修改都在 `clone()` 之后，因为调用方可能跨请求、跨引擎共享同一个 `SamplingParams`。

**源码事实：** 提示词校验失败时要把多模态哈希登记为「待重发」，因为渲染阶段已把它们记为「已发送」。

**源码事实：** `max_tokens` 未设时在此处补成 `max_model_len - prompt_len`，并且 `min_tokens` 的校验**只在这个分支里**做。

**源码事实：** `process_outputs` 的 docstring 明确规定「这是唯一应该遍历整批的函数」，靠注释而非机制约束。

**源码事实：** 文本层的停止串判定会覆盖引擎层的 `finish_reason`，并在引擎未结束时把请求加入待取消列表。

**源码事实：** 流式输入下块边界会把 `finished` 强行改回 `False`，并立即应用下一条 `StreamingUpdate`。

**源码事实：** `FastIncrementalDetokenizer` 用提示词尾部做 `DecodeStream` 种子；解码器内部状态损坏时整体重建并重试。

**源码事实：** 停止串缓冲长度是「最长停止串减一」，`finished` 时缓冲归零。

**架构推论：** 输入侧「尽情校验」与输出侧「只循环一次」的对比，是**同一层里两种完全不同的成本模型**并存。这提示一个判断标准：**如果一个操作每请求一次，就把它写清楚写全；如果每 token 一次，就把它压缩到极致。** 混淆这两者的代码会在一端低效、在另一端脆弱。

**架构推论：** 「渲染阶段乐观登记、校验失败后回滚」这个模式在任何分层缓存里都会出现。它的脆弱点是**回滚必须覆盖所有失败路径**——这里用 `try/except` 包住两个校验调用，一旦将来有人在校验之外加了一步会失败的逻辑，回滚就会漏。

**架构推论：** `max_tokens` 与 `min_tokens` 的校验分散在两处，是因为两者有依赖关系又分属不同层。**这是「校验的依赖顺序」问题**：`min_tokens <= max_tokens` 只有在 `max_tokens` 确定之后才能查，而 `max_tokens` 可能在别处被设。这类校验最好收敛到「参数定稿之后统一校验」的一个点上，代价是要延迟校验时机。

**架构推论：** 快解码器依赖原生实现、慢解码器依赖 Python 重解，两者的行为必须一致。而这个一致性没有任何机制保证——`INVALID_PREFIX_ERR_MSG` 这种「靠消息前缀判断错误类型」的恢复逻辑，本质是在给一个外部库的状态机打补丁。**依赖别人的增量状态机，就得准备好自己修它的状态。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/engine/input_processor.py` | `InputProcessor` | 请求入引擎前的唯一校验与转换入口 |
| `vllm/v1/engine/input_processor.py` | `InputProcessor.process_inputs` | 十三条校验与最终 `EngineCoreRequest` 组装 |
| `vllm/v1/engine/input_processor.py` | `_validate_params` | 参数校验并返回水印开关 |
| `vllm/v1/engine/input_processor.py` | `_validate_prompt_len` | 长度校验，错误建议按是否支持多模态分版 |
| `vllm/v1/engine/input_processor.py` | `_validate_model_inputs` | 编解码两侧输入的模型级校验 |
| `vllm/v1/engine/input_processor.py` | `inject_into_mm_cache` | 多模态缓存注入 |
| `vllm/v1/engine/input_processor.py` | `assign_request_id` | 请求 ID 赋值 |
| `vllm/v1/engine/output_processor.py` | `OutputProcessor` | 输出加工门面 |
| `vllm/v1/engine/output_processor.py` | `OutputProcessor.process_outputs` | 唯一遍历整批的函数 |
| `vllm/v1/engine/output_processor.py` | `RequestState` | 单请求的累积状态（token、文本、统计、流式队列） |
| `vllm/v1/engine/output_processor.py` | `RequestState.apply_streaming_update` | 应用流式输入的新块并重置预填标志 |
| `vllm/v1/engine/output_processor.py` | `RequestState.observe_prefill` | 预填阶段的缓存命中统计 |
| `vllm/v1/engine/output_processor.py` | `StreamingUpdate` | 流式输入的增量载荷 |
| `vllm/v1/engine/output_processor.py` | `RequestOutputCollector` | 每请求输出收集器 |
| `vllm/v1/engine/detokenizer.py` | `IncrementalDetokenizer` | 增量解码的抽象契约 |
| `vllm/v1/engine/detokenizer.py` | `FastIncrementalDetokenizer` | 原生流式解码 + 状态损坏恢复 |
| `vllm/v1/engine/detokenizer.py` | `SlowIncrementalDetokenizer` | 带偏移量的 Python 重解 |
| `vllm/v1/engine/detokenizer.py` | `get_next_output_text` | 增量切片与停止串缓冲 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：v1/engine/output_processor.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/engine/output_processor.py)
- [vllm 源码：v1/engine/detokenizer.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/engine/detokenizer.py)
- [HuggingFace tokenizers：DecodeStream](https://huggingface.co/docs/tokenizers/api/decoders)

## 与主核 / AI 的关系

输入侧的「拒绝」与主核里「写路径上的约束校验」位置相同、取向相同：主核在写入时校验类型、约束、唯一性，把非法数据挡在页之外；vllm 在请求进引擎前校验参数与长度，把非法请求挡在 GPU 之外。**两者都在「最贵的动作之前」放校验。**

输出侧的「每 token 只循环一次」与主核里「每次 I/O 只解析一次元组」是同一条性能纪律。数据库解析行头时的字段偏移预计算、vllm 里 `token_offsets` 只在有停止串时构建，都是同一个思路：**把能省的判断移到循环之外，或者在不需要时根本不建。**

停止串缓冲的机制与主核的「延迟写」有一处有趣的对应：主核要把脏页攒起来再刷，因为「已经写下去就改不了」；这里要把可能构成停止串前缀的字符扣住，因为「已经发给客户端就收不回」。**两者都在处理「不可撤销的动作」——只是一个在存储层，一个在输出层。**

对 agent / RAG 工程的实际含义有四条。其一，`max_tokens` 不设时会被补成「上下文长度减提示长度」，所以看到输出突然变短该先怀疑提示变长。其二，停止串会占用「最长停止串减一」个字符的延迟，流式输出的最后几个字符总会晚一拍——这是正确性代价，不是 bug。其三，多模态请求被拒后那张图需要重新上传（缓存登记被回滚），不要假设失败后立即重试能命中缓存。其四，流式输入下「这一块结束」与「整个会话结束」是不同的信号，客户端的 `finished` 标志在块边界处会是 `False`。
