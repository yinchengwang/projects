---
project: llama-index
feature: response-synthesizers
title: 回答合成策略
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/response_synthesizers/base.py
    symbols: [BaseSynthesizer]
  - path: llama-index-core/llama_index/core/response_synthesizers/refine.py
    symbols: [Refine, DefaultRefineProgram, StructuredRefineResponse]
  - path: llama-index-core/llama_index/core/response_synthesizers/tree_summarize.py
    symbols: [TreeSummarize]
---

# 回答合成策略

## 一句话职责

把「一批文本块 + 一个问题」变成「一个回答」——而这里的核心问题是**上下文装不下**：`Refine` 用逐块精炼（每次带着已有答案看下一块），`TreeSummarize` 用树状归约（先各自摘要再摘要的摘要），`CompactAndRefine` 则是 `Refine` 前面加一次「先打包压实」。

## 机制是什么

| 模式 | 归约形状 | LLM 调用次数 | 质量特征 |
|---|---|---|---|
| `Refine` | **线性链**：块1 → 答案1 → 块2 → 答案2 → … | ≈ 块数 | 后块能修正前块的答案，但会「遗忘」早先内容 |
| `TreeSummarize` | **树**：同时摘要 n 块 → 得 n/k 个摘要 → 递归 | ≈ 总块数的对数级层数 × 每层并行 | 每块都被独立看到一次，但跨块的关联只能靠摘要保留 |
| `CompactAndRefine` | 先 `repack` 压实，再走 `Refine` 的链 | 略少于 `Refine` | 用更少的调用次数吃到同样的上下文 |

`Refine` 与 `CompactAndRefine` 是**继承关系**（`CompactAndRefine(Refine)`），差别只在「进去之前要不要先压实」这一步。

## 怎么实现的

### 一、`BaseSynthesizer`：契约与输出形态

**它要求子类实现两个方法：`get_response` 与 `aget_response`；另有两个「可选」的消息版（`get_response_from_messages` / `aget_response_from_messages`）默认抛 `NotImplementedError`。**

```
def get_response_from_messages(...) -> RESPONSE_TEXT_TYPE: raise NotImplementedError
```

这与查询引擎那页的结构一致：**必需用 `abstractmethod`，可选用「默认抛错」**。这里的可选是「多模态/消息形态的合成」——只有部分合成器支持。

**`BaseSynthesizer.synthesize` 是一个统一的门面，它做四件事。**

1. **空输入的专门处理**：

```
if len(nodes) == 0:
    if self._streaming:
        empty_response_stream = StreamingResponse(response_gen=self._empty_response_generator())
        ... return empty_response_stream
    else:
        empty_response = Response(self._empty_response)
        ... return empty_response
```

`_empty_response` 默认 `"Empty Response"`，且**两个分支都发了一对 start/end 仪表事件**——一个空查询在追踪里也应当是完整的。

2. **`str` → `QueryBundle`**。
3. **按 `multimodal` 分叉取文本还是内容块**：

```
if self._multimodal:
    response_str = self.get_response_from_messages(query_str=..., message_chunks=[ChatMessage(blocks=n.node.get_content_blocks(metadata_mode=MetadataMode.LLM)) for n in nodes], ...)
else:
    response_str = self.get_response(query_str=..., text_chunks=[n.node.get_content(metadata_mode=MetadataMode.LLM) for n in nodes], ...)
```

注意 `get_content(metadata_mode=MetadataMode.LLM)`——**取节点内容时会带上「给 LLM 看」的那份元数据**（而不是嵌入用的那份）。这个细节决定了「节点元数据是否出现在提示里」。

4. **把响应文本包装成响应对象，并把 `additional_source_nodes` 拼到来源列表尾部。**

```
source_nodes = list(nodes) + list(additional_source_nodes)
response = self._prepare_response_output(response_str, source_nodes)
```

`additional_source_nodes` 是一个容易被忽略的能力：它让调用方能**在合成之后追加来源**（比如把「工具调用结果」也标成来源），而不用改合成过程。

**`_prepare_response_output` 的类型分派是这一页最繁的一段，而它的顺序有讲究。**

```
1. StructuredLLM → PydanticResponse(model_validate_json(...))
2. str          → Response
3. Generator    → StreamingResponse
4. AsyncGenerator → AsyncStreamingResponse
5. output_cls 且 response_str 已是该类型 → PydanticResponse
6. 其余 → ValueError("Response must be a string or a generator. Found ...")
```

第 5 条排在第 2 条之后不会出问题（一个 `BaseModel` 不是 `str`），但**第 5 条实际上是一个「兜底的结构化分支」**：`StructuredLLM` 已经把输出转成了对象、而某些合成器可能直接产出对象。

而**所有分支都调用 `_get_metadata_for_response`**，它返回 `{node_id: node.metadata}` ——也就是说**响应对象里带一份「来源节点的元数据快照」**。这是「引用/溯源」的基础（下一节 `f04` 的引用引擎会用到类似的机制）。

**`callback_manager` 的 setter 里有一处重复赋值 + 一个 `TODO`。**

```
@callback_manager.setter
def callback_manager(self, callback_manager: CallbackManager) -> None:
    self._callback_manager = callback_manager
    # TODO: please fix this later
    self._callback_manager = callback_manager
    self._llm.callback_manager = callback_manager
```

`self._callback_manager = callback_manager` **写了两遍**，中间夹着 `# TODO: please fix this later`。这是一个明显的复制粘贴残留，而 TODO 没有说明要修什么。

它实际做的第二件重要事是 `self._llm.callback_manager = callback_manager`——**把 manager 写进 LLM**。与查询引擎那页把 manager 写进后处理器是同一个手法：**让内部调用的事件归属同一条 trace**。

**`__init__` 里也有同样的接线。**

```
if callback_manager: self._llm.callback_manager = callback_manager
```

注意它是**条件执行**的——只在显式传入时才覆盖 LLM 已有的 manager。于是「传了 manager」与「没传」在 LLM 这一侧的行为不同：没传时 LLM 保留它自己构造时的 manager（可能与合成器的不是同一个）。这是一个隐含的不一致。

**`multimodal` 有一道显式校验。**

```
if multimodal:
    if not is_chat_model(self._llm):
        raise ValueError("Multimodal synthesis requires a chat LLM.")
```

理由直接：多模态内容块只能走 chat 消息接口。

**`prompt_helper` 与 `chat_prompt_helper` 的解析是一条三级链。**

```
prompt_helper or Settings._prompt_helper or PromptHelper.from_llm_metadata(self._llm.metadata)
```

而这两个 helper 的用途不同：**文本走 `prompt_helper`、多模态走 `chat_prompt_helper`**。它们负责 `repack`——把若干文本块重新打包进提示预算，这在下文 `Refine` 里是关键。

### 二、`Refine`：线性精炼链

**它的核心循环 `_run_refine_loop` 值得逐段读，因为所有「装不下怎么办」的智慧都在这里。**

**第一步是「先 repack 再进循环」。**

```
chunks_deque: deque = deque([tc for chunk in chunks for tc in prompt_helper.repack(max_prompt, [chunk], llm=self._llm, padding=self._response_padding_size)])
```

注意它**对每个块单独 repack 然后拼成一个 deque**。`max_prompt = get_biggest_prompt([qa_template, refine_template])` ——取两个模板里**更大的那个**作为预算基准。

`padding=self._response_padding_size`（默认 **500**）——为回答预留的空间。

**第二步是循环体：第一次用 QA 模板、之后用 refine 模板。**

```
if response is None:
    prompt_template = qa_template.partial_format(query_str=query_str)
    prompt_kwargs = make_qa_prompt_kwargs(chunk)
else:
    prompt_template = refine_template.partial_format(query_str=query_str, existing_answer=response)
    ...
```

也就是说**只有第一块走「问答」，其余都走「带着已有答案精炼」**。`existing_answer=response` 被 `partial_format` 预填进模板——这是整条链传递上下文的唯一通道。

**第三步是一个很细的「重新压实」逻辑，而它解释了为什么 `existing_answer` 会改变预算。**

```
repacked = prompt_helper.repack(prompt_template, [chunk], llm=self._llm)
if len(repacked) > 1:
    chunks_deque.extendleft(repacked)
    continue
chunk = repacked[0]
```

注释写得很清楚：

> 因为 existing answer 那部分在持续更新，可能有必要用新模板重新打包这个块以确保它装得下。由于模板已经用实际回答做了部分格式化，不需要额外 padding。

两个要点：

1. **回答越长，留给新块的预算越小**。所以同一个块在链的前段能装下、到后段可能装不下。这一步就是「用当前的实际回答重新算一次预算」。
2. **`repack` 若返回多块，则把它们塞回 deque 头部**（`extendleft`）并 `continue`——也就是**跳过这次 LLM 调用**，让更小的块先被处理。

而 `extendleft` 有一个容易忽略的行为：**它按逆序插入**。`deque.extendleft([a, b])` 的结果是 `[b, a, ...]`。于是 repack 出来的两个块会被**倒序**处理。这个顺序差异在大多数情况下无害（它们本就来自同一个块），但它确实改变了「先看哪部分」的顺序。

**`_update_response` 是那个「要不要接受这次精炼结果」的判定，而它的核心是 `query_satisfied`。**

**非流式路径：**

```
structured_response = program(**program_kwargs, **response_kwargs)
query_satisfied = structured_response.query_satisfied
if query_satisfied: return structured_response.answer
```

**流式路径有两支**，其中第一支是这一页最细的优化：

```
if isinstance(program, DefaultRefineProgram) and self._output_cls is None:
    return (cast(StructuredRefineResponse, sr).answer for sr in structured_response_gen)
```

注释解释：**每个 yield 的 answer 是一个 token delta（增量），不是逐渐完整的对象——所以应当边到边转发，而不是排空成一个值。**

也就是说在「纯文本流式 + 默认程序」这个最常见的情形里，token 是**逐字转发**的。而在其它情形（结构化输出、或自定义程序）里，它必须**排空整个生成器**才敢返回：

```
structured_response = None
for sr in structured_response_gen:
    assert not isinstance(sr, list)
    structured_response = sr
    if sr is not None:
        query_satisfied = getattr(sr, "query_satisfied", None)
        if query_satisfied is not None: break
if query_satisfied:
    return self._get_attribute_from_object_generator(structured_response_gen, structured_response, "answer")
```

而 `_get_attribute_from_object_generator` 的实现与它的文档说明了为什么必须排空：

> 对象生成器可能 yield 多个对象，但因为**我们无法保证对象属性的生成顺序**，需要等它完全生成才能确信该属性既存在又完整。

这是一个关于流式 JSON 的硬约束：**JSON 的字段顺序取决于模型，`answer` 可能出现在 `query_satisfied` 之后**。

### 三、`StructuredRefineResponse`：字段顺序是一个性能设计

**这个只有两个字段的模型有一段很长的注释，而它说明了为什么 `query_satisfied` 必须排在前面。**

> 这个变量**排在第一位很重要**，因为 LLM **几乎总是**按这里列出的顺序流式输出 JSON（因而也按这个顺序序列化）。这让流式时可以**非常早地放弃不相关的来源**。因为这个变量在前 10 来个 token 内就被流出了，若它是 `False`，我们不必等剩下的响应，可以直接换下一个来源。

这是一个把「序列化顺序」当成性能手段的设计：**两个字段的声明顺序决定了能否提前终止**。`query_satisfied` 在前 → 大约 10 个 token 就能判断 → 若为假则整块的其余生成全部作废，省下几百到上千 token。

这个优化的前提是「模型按字段声明顺序生成」——而注释用了「almost always」，说明这是一个**经验规律而非保证**。若模型不按序生成，优化失效（会多等一会儿）但正确性不受影响。

**`DefaultRefineProgram` 的定位是「不做过滤」。**

文档一句话：**在 LLM 上正常跑查询，并总是返回 `query_satisfied=True` 的答案。实际上不做任何答案过滤。**

它的实现就是把 `structured_predict`（若有 `output_cls`）或 `predict` 的结果包成 `StructuredRefineResponse(query_satisfied=True)`。于是**默认模式下 `query_satisfied` 恒为真**，那个提前终止优化只在 `structured_answer_filtering=True` 时才真正生效。

**而 `program_factory` 与 `structured_answer_filtering` 之间有一道强制关系。**

```
if not self._structured_answer_filtering and program_factory is not None:
    raise ValueError("Program factory not supported without structured answer filtering.")
```

理由：自定义 program 的意义就是改变「怎么判定/产出答案」，而若不做过滤，那就没必要换 program。**这道校验把「自定义 program」绑在了「结构化过滤」这个能力上**，避免了一个「换了 program 但过滤没开」的混乱组合。

### 四、`TreeSummarize`：树状归约

**它的实现短得多，而结构一目了然。**

```
text_chunks = self._prompt_helper.repack(summary_template, text_chunks=text_chunks, llm=self._llm)

if len(text_chunks) == 1:
    ...  # 一次 LLM 调用直接回答
else:
    ...  # 对每块并行/串行摘要，然后递归
    return self.get_response(query_str=query_str, text_chunks=summaries, **response_kwargs)
```

**递归的终止条件是「repack 之后只剩一块」。** 而 `repack` 每次递归都会被调用——**所以树的分叉度由「一块能装下几个输入」决定，而不是由固定参数决定**。这是它与「固定 fanout」的树归约的根本差别：**树形自适应于提示预算**。

**每一层是并行的。** `_use_async=True` 时它把 n 个摘要任务建成 `tasks` 再 `run_async_tasks(tasks)`；否则是列表推导逐条同步跑。

**所以调用次数的量级是 `n / fanout + n / fanout² + …`，也就是 O(log n) 层、每层约 fanout 个并行调用。** 这正是任务的提示里那句「O(log n) 次调用」——准确说法是**层数是 O(log n)，每层并行 fanout 次**。

**它有一个「结构化输出要转成 JSON 字符串再往下传」的处理。**

```
summaries = [summary.model_dump_json() if isinstance(summary, BaseModel) else str(summary) for summary in summary_responses]
```

理由很直接：**下一层的输入是文本块**（要重新 repack 进提示），所以结构化对象必须先序列化回字符串。这也意味着**跨层的结构化信息只能以 JSON 文本形式保留**——一棵树上每一层都做一次 `model_dump_json` 再 `model_validate_json`。

**单块分支里有三选一：流式 / 普通 / 结构化。** 而流式分支直接用 `self._llm.stream(...)` 返回生成器——`BaseSynthesizer._prepare_response_output` 会把它包成 `StreamingResponse`。**也就是说流式只在「最后一次调用」时才可能发生**，因为前面的层需要真实的摘要文本才能继续。

### 五、`CompactAndRefine` 与 `Refine` 的差别

**它就是 `Refine` 加一个前置步骤，而这个步骤是「用 prompt_helper 把输入块压实」。**

```
def get_response(self, query_str, text_chunks, prev_response=None, **response_kwargs):
    new_texts = self._make_compact_text_chunks(query_str, text_chunks)
    return super().get_response(query_str=query_str, text_chunks=new_texts, prev_response=prev_response, **response_kwargs)
```

而 `_make_compact_text_chunks` 的实现是：

```
text_qa_template = self._text_qa_template.partial_format(query_str=query_str)
refine_template = self._refine_template.partial_format(query_str=query_str)
max_prompt = get_biggest_prompt([text_qa_template, refine_template])
return self._prompt_helper.repack(max_prompt, text_chunks, llm=self._llm, padding=self._response_padding_size)
```

**注意 `repack` 的参数差异：`CompactAndRefine` 传的是全部 `text_chunks`（一次打包），而 `Refine._run_refine_loop` 是逐块 repack（`[chunk]`）。**

于是两者的差别变得精确：

- **`Refine`**：每个原始块单独 repack，然后把所有结果按块数依次喂给链。**若一个原始块本身能装下多个块的空间，`Refine` 不会合并它们**——每个块都消耗至少一次 LLM 调用。
- **`CompactAndRefine`**：先让 `repack` 把多个块合并成「尽量满」的若干块，再走同一条链。**于是 LLM 调用次数减少**（因为块数变少了）。

**这就是「compact」的含义：不是压缩文本内容，而是压缩调用次数**——把多个小块拼成一个大块，用一次调用覆盖它们。

这也解释了为什么 `CompactAndRefine` 是默认模式（`ResponseMode.COMPACT`）：**在多数场景下它用更少的调用得到与 `Refine` 相当的质量**，因为一次看到更多上下文总比逐块看要好。

代价是：**`Refine` 的「逐块精炼」能力被削弱了**——一次看到 3 个块的精炼不如分 3 次精炼能逐步修正。当答案需要跨块推理时，`Refine` 反而可能更好。

## 关键取舍

**源码事实：** `Refine` 与 `CompactAndRefine` 是继承关系，唯一差别是「进链之前如何 repack」：`Refine` 逐块 repack（`[chunk]`），`CompactAndRefine` 一次 repack 全部（合并成更少的块）。

**源码事实：** 因此 `CompactAndRefine` 的 LLM 调用次数更少，而 `Refine` 保留了逐块逐步修正的能力。默认模式是 `COMPACT`。

**源码事实：** `Refine` 只有第一块走 QA 模板，其余走 refine 模板，`existing_answer` 用 `partial_format` 预填——这是跨块传递上下文的唯一通道。

**源码事实：** 循环里每次都用**当前的实际回答**重新 repack 这个块（因为回答越长留给新块的预算越小）；若 repack 出多块则 `extendleft` 塞回 deque 头部并跳过本次调用。`extendleft` 会**逆序**插入。

**源码事实：** `response_padding_size`（默认 500）是为回答预留的空间；`max_prompt` 取两个模板里更大的那个作预算基准。

**源码事实：** `StructuredRefineResponse.query_satisfied` 必须声明在前，因为模型几乎总是按字段顺序生成——于是前 10 来个 token 就能判定是否放弃这个来源。这是把序列化顺序当性能手段。

**源码事实：** `DefaultRefineProgram` 恒返回 `query_satisfied=True`，所以提前终止优化只在 `structured_answer_filtering=True` 时真正生效。

**源码事实：** 「自定义 `program_factory` 必须配合 `structured_answer_filtering`」是硬校验，避免「换了 program 但不过滤」的组合。

**源码事实：** 流式路径有两支：默认程序 + 无 `output_cls` 时**逐 token 转发**；其余情形必须**排空整个生成器**才能取 `answer`，因为无法保证属性生成顺序。

**源码事实：** `TreeSummarize` 的终止条件是「repack 后只剩一块」，于是**树的分叉度自适应于提示预算**，层数是 O(log n)、每层并行 fanout 次。

**源码事实：** 结构化输出在 `TreeSummarize` 里每层都要 `model_dump_json` 转回字符串才能进下一层——跨层的结构化信息只能以 JSON 文本保留。

**源码事实：** `_prepare_response_output` 按六种形态分派（`StructuredLLM` / `str` / `Generator` / `AsyncGenerator` / 已是 `output_cls` / 报错），且**每种都带一份「来源节点元数据快照」**。

**源码事实：** `callback_manager` 的 setter 里有一处重复赋值并附 `# TODO: please fix this later`；而 `__init__` 里只在显式传入 manager 时才覆盖 LLM 的 manager（不传则 LLM 保留自己的）。

**架构推论：** 这四种模式的核心分野是**「跨块信息靠什么传递」**：

- `Refine` 靠**答案本身**（`existing_answer`）——于是信息在链上单向流动，后块可以覆盖前块的结论（可能是改进，也可能是遗忘）。
- `TreeSummarize` 靠**摘要**——于是每块都被独立看到一次，但跨块的关联只能在摘要里留存，且每层摘要都是信息损失。
- `Accumulate`（同一目录下的另一个实现）靠**拼接**——不归约，把所有块的回答拼起来。

这三种传递方式对应三种「信息损失的位置」，而选择哪一种取决于「这个问题的答案是否需要在块之间累积」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/response_synthesizers/base.py` | `BaseSynthesizer` | 合成器契约：`get_response` 必需、消息版可选 |
| `llama-index-core/llama_index/core/response_synthesizers/base.py` | `BaseSynthesizer.synthesize` | 门面：空输入特判、`str`→bundle、多模态分叉、拼 `additional_source_nodes` |
| `llama-index-core/llama_index/core/response_synthesizers/base.py` | `_prepare_response_output` | 六种响应形态分派，各带来源元数据快照 |
| `llama-index-core/llama_index/core/response_synthesizers/base.py` | `callback_manager` setter | 写进 `self._llm`，含重复赋值与 TODO |
| `llama-index-core/llama_index/core/response_synthesizers/refine.py` | `Refine` | 线性精炼链 |
| `llama-index-core/llama_index/core/response_synthesizers/refine.py` | `_run_refine_loop` | 首块 QA、其余 refine；每次按实际回答重新 repack |
| `llama-index-core/llama_index/core/response_synthesizers/refine.py` | `_update_response` | 按 `query_satisfied` 决定是否接受；流式有两支 |
| `llama-index-core/llama_index/core/response_synthesizers/refine.py` | `StructuredRefineResponse` | 两个字段，顺序决定能否提前终止 |
| `llama-index-core/llama_index/core/response_synthesizers/refine.py` | `DefaultRefineProgram` | 恒 `query_satisfied=True`，即不做过滤 |
| `llama-index-core/llama_index/core/response_synthesizers/refine.py` | `_get_attribute_from_object_generator` | 必须排空生成器，因为属性生成顺序不可保证 |
| `llama-index-core/llama_index/core/response_synthesizers/tree_summarize.py` | `TreeSummarize` | 树状归约，终止条件是 repack 后只剩一块 |
| `llama-index-core/llama_index/core/response_synthesizers/tree_summarize.py` | `TreeSummarize.get_response` | 每层并行摘要 + 递归 |
| `llama-index-core/llama_index/core/response_synthesizers/compact_and_refine.py` | `CompactAndRefine` | 继承 `Refine`，只多一次全量 repack |
| `llama-index-core/llama_index/core/response_synthesizers/factory.py` | `get_response_synthesizer` | 按 `ResponseMode` 分派到具体实现 |

## 参考

- [llama-index 源码：response_synthesizers/refine.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/response_synthesizers/refine.py)
- [llama-index 源码：response_synthesizers/tree_summarize.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/response_synthesizers/tree_summarize.py)
- [llama-index 文档：响应合成器](https://docs.llamaindex.ai/en/stable/module_guides/querying/response_synthesizers/)
- [论文：map-reduce 与 refine 式长文问答](https://arxiv.org/abs/2212.10496)

## 与主核 / AI 的关系

这三种归约形状与主核里的**并行聚合**同构：数据库的 `SELECT ... FROM` 算子也分「流式归约」（顺序扫描边读边聚合）与「阻塞归约」（先全部读进内存再聚合），而 `Refine` 正是前者的文本版——**每次只保留一个累加器（这里是「当前答案」）**。

主核里这两者的取舍是内存与并发度；这里换成 token 预算与答案质量。而同一个陷阱也存在：**流式归约的累加器可能被后来的输入带偏**，`Refine` 的 `existing_answer` 正是这个累加器。

对 agent 工程的直接价值有三条。其一，**「compact」压的不是文本而是调用次数**。把多个块拼成一次调用，与把文本内容压缩，是完全不同的两件事——前者省调用、后者省 token。其二，**字段声明顺序可以是性能手段**。`query_satisfied` 排第一让流式能提前 10 个 token 就放弃；这类优化依赖「模型按序生成 JSON」这一经验规律，所以要写明它是 almost always 而非保证。其三，**流式与结构化输出的结合必须区分「增量」与「完整对象」**。纯文本流可以逐 token 转发，而带结构体的流必须排空才能取某个字段——把这两种情形混为一谈会让流式退化成「等全部生成完再返回」。
