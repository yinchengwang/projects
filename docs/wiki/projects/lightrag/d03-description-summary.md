---
project: lightrag
feature: description-summary
title: 描述摘要与合并策略
page_type: feature
schema_version: 1
refs:
  - path: lightrag/operate.py
    symbols: [_handle_entity_relation_summary, _summarize_descriptions]
  - path: lightrag/utils_graph.py
    symbols: [validate_relation_weight, apply_relation_weight_floor, relation_evidence_source_ids]
---

# 描述摘要与合并策略

## 一句话职责

一个实体积累了上百条描述后不能全存——这一层用 **map-reduce** 把描述压成一段摘要：先按 token 预算分组、每组调 LLM 汇总、再递归汇总到符合上限；而 `weight >= 不同真实来源数` 这条关系权重契约在这里被拆成两个函数——**一个是「校验并拒绝」（写入路径）、一个是「夹到地板」（读取/修复路径）**。

## 机制是什么

两件事被放在一页，因为它们都在处理「多个碎片压成一个值」：

**描述摘要**要解决的是「无界的增长」。d02 讲过 `_combine_descriptions_dedup` 让描述**不会重复**，但它不限制**长度**——一个被一万个文档提到的实体，描述列表可能远超上下文窗口。所以需要摘要。

摘要的难点是**递归终止**。压一次可能还不够（一百条描述压成十段，十段仍超预算），所以必须能反复压。而「反复压」有两个风险：压到只剩一条还超（无法再压），或压到无限循环。

**权重契约**要解决的是「历史数据的一致性」。`weight` 的设计语义是「有多少个不同来源支持这条关系」，而手工写入（`ainsert_custom_kg`）可以写任意权重。所以需要一个函数在**写入时拒绝**不合理值，另一个在**读取时修复**历史数据。

两者的核心区别在**失败方向**：

| 函数 | 场景 | 不合理时的行为 |
|---|---|---|
| `validate_relation_weight` | 新写入 | **抛错**（让调用方改） |
| `apply_relation_weight_floor` | 读旧数据 | **夹到地板**（容错） |

## 怎么实现的

### 摘要的入口：`_handle_entity_relation_summary`

**三个提前返回覆盖了「不需要 LLM」的三种情形。**

```python
if not description_list:
    return "", False

if len(description_list) == 1:
    return sanitize_text_for_encoding(description_list[0]), False
```

三处：

1. **空列表返回 `("", False)`** —— 返回空串而不是抛错，因为「没有描述」是调用方该处理的情况（d02 里实体给兜底、关系抛错）。

2. **单条描述直接返回，不调 LLM** —— 压一条等于不压。所以省一次调用。

3. **但单条也要 sanitize**，注释说明了理由：「descriptions read back from existing graph nodes (or injected by non-extraction producers) may carry XML-illegal control characters that would crash the GraphML flush downstream」。

   **即：读回来的数据可能没经过抽取期的清洗。** 这与 c05 里 `strip_internal_multimodal_markup_for_extraction` 的「存储不动」形成互补——存储里可能是脏的，所以每个「用」的地方要自己清洗。

而第三个提前返回在循环内，条件更复杂：

```python
if len(current_list) < force_llm_summary_on_merge:
    bare_tokens = sum(len(tokenizer.encode(desc)) for desc in current_list)
    if bare_tokens < summary_max_tokens and (
        bare_tokens <= summary_context_size or len(current_list) <= 2
    ):
        final_description = sanitize_text_for_encoding(separator.join(current_list))
        return final_description if final_description else "", llm_was_used
```

五处：

1. **它在 `while True` 循环的**开头** —— 所以每轮迭代都可能直接返回。这保证了「递归到够小就停」。

2. **`len(current_list) < force_llm_summary_on_merge`** —— 描述条数少到这个阈值以下才可能「不摘要直接拼」。所以这是一个**条数门限**：即使总 token 很小，条数多也要走摘要（因为语义上需要归并）。

3. **`bare_tokens` 用裸文本算** —— 注释解释了这个选择：「A short list whose bare texts fit is stored as their join, with no LLM call and no prompt, so the prompt's JSON overhead must not decide this exit: it is measured on the bare texts, exactly as it is stored」。

   **即：这条出口产出的是「裸文本拼接」，所以判据必须按裸文本量。** 若用 prompt 渲染后的量（含 JSON 包装），就会因为包装开销而误判「放不下」，于是白调一次 LLM。

4. **`bare_tokens < summary_max_tokens` 且（`<= summary_context_size` 或 `len <= 2`）** —— 两个上限：`summary_max_tokens` 是「摘要目标长度」，`summary_context_size` 是「输入预算」。而 `len(current_list) <= 2` 是**兜底**：两条描述即使超 context 也直接拼（因为没有更小的分组了）。

5. **`final_description if final_description else ""`** —— 全部是控制字符时 sanitize 后为空，返回 `""`。

**主循环的两条路径：能压就压，不能压就分组。**

```python
desc_token_counts = []
for i, desc in enumerate(current_list, start=1):
    tokens = len(tokenizer.encode(_summary_description_line(desc) + "\n"))
    desc_token_counts.append(tokens)
    total_tokens += tokens
    await _cooperative_yield(i, every=32)
```

**注意这里按 `_summary_description_line(desc) + "\n"` 计数，而不是裸文本。**

注释把理由写得很细：

> Count a description as `_summarize_descriptions` renders it (a JSON line plus its newline), because that rendering is what it trims to `summary_context_size`. Counting the bare text undercounts every group, and the trim then silently drops the descriptions at its tail.

四处：

1. **`_summary_description_line` 是 `json.dumps({"Description": description}, ensure_ascii=False)`** —— 所以每条描述被包成一行 JSON。

2. **为什么必须数渲染后的量**：`_summarize_descriptions` 里会用 `atruncate_list_by_token_size` 按**渲染后的量**裁到 `summary_context_size`。若分组时按裸文本算，分出来的组会**比预算大**，然后被那个裁剪悄悄砍掉尾部几条。

   **这是一个「两处使用不同计价单位」导致的静默数据丢失** —— 而它被注释明确指出来了。这也解释了为什么前面的「直接拼接」出口要按裸文本算（因为它不经过那个裁剪）。

3. **`+ "\n"`** —— 分隔符也算进去（`_summarize_descriptions` 里用 `"\n".join`）。

4. **`desc_token_counts` 缓存下来供分组复用** —— 注释：「Tokenize each description once; the chunk-building pass below reuses these counts instead of re-encoding the same strings」。

   所以每个描述**只 encode 一次**（除了「直接拼接」出口那次的 bare 计算）。这在实体有几百条描述时是显著的。

**`while True` 的终止依赖「每次迭代至少减半」。**

```python
if total_tokens <= summary_context_size or len(current_list) <= 2:
    if total_tokens > summary_context_size and len(current_list) <= 2:
        logger.warning(f"Summarizing {entity_or_relation_name}: Oversize description found")
    final_summary = await _summarize_descriptions(...)
    return final_summary, True
```

**「`len(current_list) <= 2` 时无条件做最终摘要」** —— 即使超 context。所以：
- 2 条时：一次 LLM 调用，`_summarize_descriptions` 内部会裁到 context（可能丢一条，并 warning）
- 1 条时：循环开头那个出口已经返回了（因为 `len < force_llm_summary_on_merge` 大概成立，且 `len <= 2` 让第二个条件成立）

**所以循环的实际终止条件是「条数降到 2」，而每次分组至少产生 2 条 → 1 条（因为每组至少 2 条描述），于是必然收缩。**

分组那段的「保证每组至少 2 条」：

```python
if current_tokens + desc_tokens > summary_context_size and current_chunk:
    if len(current_chunk) == 1:
        current_chunk.append(desc)
        chunks.append(current_chunk)
        logger.warning(f"Summarizing {entity_or_relation_name}: Oversize description found")
        current_chunk = []
        current_tokens = 0
    else:
        chunks.append(current_chunk)
        current_chunk = [desc]
        current_tokens = desc_tokens
else:
    current_chunk.append(desc)
    current_tokens += desc_tokens
```

四处：

1. **`len(current_chunk) == 1` 时强制再加一条** —— 注释：「Ensure we have at least 2 descriptions in the chunk (when possible)」。所以一个超大描述会拉着下一条一起进组。

   为什么必须至少 2 条？因为 reduce 阶段对单条组**不调 LLM**（`if len(chunk) == 1: new_summaries.append(chunk[0])`）——若分组产出单条组，那一轮就没压缩，可能无限循环。

   **所以「每组至少 2 条」是终止性的保证。** 而强制加一条会产出「超预算的组」，于是 `_summarize_descriptions` 会裁掉尾部——这就是那条 warning 的来源。

2. **`current_chunk = []` 与 `current_tokens = 0`**（强制加一条后）—— 所以下一个描述开始新组。

3. **否则把当前描述留给下一组**（`current_chunk = [desc]`）。

4. **`current_tokens + desc_tokens > summary_context_size and current_chunk` 的 `and current_chunk`** —— 保证第一个描述总能进组（否则空组会无限循环）。

**reduce 阶段对单条组跳过 LLM。**

```python
for i, chunk in enumerate(chunks, start=1):
    if len(chunk) == 1:
        new_summaries.append(chunk[0])
    else:
        summary = await _summarize_descriptions(...)
        new_summaries.append(summary)
        llm_was_used = True
```

**注释说这是「Optimization: single description chunks don't need LLM summarization」。** 但结合上一条，它同时是终止性的一部分：若单条组也调 LLM，那轮压缩可能「越压越长」（LLM 生成的摘要可能比原文长），于是永远不收敛。

**而 `llm_was_used` 只在 reduce 阶段或最终阶段被设为 `True`** —— 所以返回值 `(description, llm_was_used)` 里的 `bool` 精确表示「是否真的调了 LLM」。d02 用它区分日志前缀（`LLMmrg:` vs `Merged:`）。

### `_summarize_descriptions`：一次实际的 LLM 汇总

**它先用 `atruncate_list_by_token_size` 裁，再判丢了什么。**

```python
truncated_descriptions = await atruncate_list_by_token_size(
    description_list, key=_summary_description_line, separator="\n",
    max_token_size=summary_context_size, tokenizer=tokenizer,
)
dropped = len(description_list) - len(truncated_descriptions)
if dropped:
    logger.warning(f"Summarizing {description_type}:{description_name}: {dropped} of {len(description_list)} descriptions exceed summary_context_size ({summary_context_size} tokens) and are left out of the summary")
```

三处：

1. **`key=_summary_description_line`** —— 裁剪按**渲染后**的量算（与前面「分组也按渲染量」一致）。

2. **`dropped` 的 warning** —— 注释说明了它的定位：「The map phase sizes groups to fit, so a trim here means an oversize description (a forced pair, or a final list of two): say what is lost」。

   **即：正常情况下不会丢**（分组已经保证放得下），所以这里的丢说明是「强制配对」或「最终两条」这两种无法避免的情形。所以记 warning 让人知道。

3. **「left out of the summary」** —— 措辞准确：这些描述不会进摘要，但因为 d02 的描述列表是**累积**的，它们仍在存储里。所以「丢出摘要」不等于「丢数据」——下一轮合并时它们还会在列表里。

   **这是一处重要的可靠性性质**：摘要只影响「当前这份摘要基于哪些碎片」，不影响碎片本身。所以重复摘要（比如调整参数后重算）不会永久丢信息。

**prompt 用 JSONL 格式，而每条是一行 JSON。**

```python
joined_descriptions = "\n".join(_summary_description_line(desc) for desc in truncated_descriptions)
context_base = dict(description_type=..., description_name=..., description_list=joined_descriptions,
                    summary_length=summary_length_recommended, language=language)
use_prompt = prompt_template.format(**context_base)
```

**为什么用 JSONL 而不是纯文本列表？** 因为描述可能含换行、引号、markdown，纯文本拼接会让边界模糊（LLM 无法确定一条描述在哪结束）。JSON 行把每条包成 `{"Description": "..."}`，边界明确。

而 `ensure_ascii=False` 让中文不被转义成 `\uXXXX`——否则 token 数会暴增且 LLM 理解变差。

**LLM 调用带 `_priority` 偏置。**

```python
use_llm_func = partial(use_llm_func, _priority=DEFAULT_SUMMARY_PRIORITY)
```

注释：「Apply higher priority (8) to entity/relation summary tasks」。

**即摘要比抽取优先级高** —— 因为摘要在合并阶段（已经等过抽取了），让它在队列里优先能缩短总时延。这是**调度优先级按阶段设置**的做法。

**截断检测必须在 sanitize 之前，而注释解释了为什么。**

```python
# Check BEFORE sanitizing: sanitize_text_for_encoding rebuilds a plain str
# and drops the TruncatedResponse marker that remove_think_tags preserved.
if is_truncated_response(summary):
    subject = f"{description_type}:{description_name}"
    logger.warning(f"Token-limit truncation while summarizing descriptions for {subject}")
    if truncation_tally is not None:
        truncation_tally.record("summary", subject)
        if truncation_write_ahead is not None:
            await truncation_write_ahead(truncation_tally)

summary = sanitize_text_for_encoding(summary)
```

四处：

1. **`TruncatedResponse` 是响应的一个**子类型**（带「被截断」标记），而 `remove_think_tags` 会保留它**（不像普通的 str 变换会丢）。

2. **`sanitize_text_for_encoding` 会把它重建成普通 `str`** —— 所以标记丢失。于是必须在 sanitize 之前判。

   **这是「标记附着在类型上」的一般问题**：任何把子类型转成父类型的操作都会丢标记。所以所有「读标记」的代码必须排在这类转换之前。

3. **`truncation_write_ahead` 在 `record` 之后立即 `await`** —— 文档说明：「awaited right after a truncation is recorded, BEFORE the summary is returned to the caller that will write it into the graph」。

   即：**记录必须先于它所警告的变更。** d02 里解释了为什么只有累积式恢复的调用方需要它。

4. **`_priority` 与 `truncation` 两条无关的关注点都在这个函数里** —— 说明这个函数是「摘要」这件事的唯一入口，所有横切关注点都堆在这里。

**空摘要抛错，而不是返回空串。**

```python
if not summary.strip():
    message = (
        f"Received empty summary for {description_type}:{description_name} "
        f"({len(description_list)} descriptions): the model returned no answer text (empty, or reasoning only); "
        "if it is a thinking model, consider disabling thinking mode for the extract role"
    )
    logger.error(message)
    raise EmptySummaryResponseError(message)
```

四处：

1. **为什么不返回空串**，注释说得很清楚：「The summary replaces every fragment it merges, so storing an empty one would erase them. Fail instead: the stored description survives and the document can be retried」。

   **这是一处关键的可靠性推理**：摘要是**替换**而不是追加。所以空摘要会「用空覆盖掉所有碎片」。而抛错让存储里的旧描述保持有效，文档可重试。

2. **「An answer that is empty after think-tag removal is never cached (save_to_cache skips falsy content), so the retry asks the LLM again」** —— 即重试有效（因为空的没被缓存）。

3. **错误消息给了可操作的建议**：「if it is a thinking model, consider disabling thinking mode for the extract role」。这是从「思考模型把内容全花在推理上、正式回答为空」这个具体故障现象反推的诊断。

4. **注释还记录了一处已知残留**：

> Accepted residue: an answer of control characters only is cached (it is non-empty until `sanitize_text_for_encoding` above runs), so a retry replays it and fails again, loudly, until the LLM cache is cleared.

   即：「只含控制字符」的响应在 sanitize 之前是非空的，所以**被缓存了**；重试会命中这个坏缓存，于是**一直失败**直到清缓存。

   **这是一个真实且会卡住文档的缺陷**，而注释选择「记录而不修」——因为修法是「在缓存前判 sanitize 后是否为空」，那需要改缓存层。**把已知缺陷连同它的表现与解法一起写进注释**，是很负责的做法。

**摘要长度超过嵌入上限时只警告。**

```python
embedding_token_limit = global_config.get("embedding_token_limit")
if embedding_token_limit is not None and summary:
    summary_token_count = len(tokenizer.encode(summary))
    threshold = int(embedding_token_limit)
    if summary_token_count > threshold:
        logger.warning(f"Summary tokens({summary_token_count}) exceeds embedding_token_limit({embedding_token_limit}) for {description_type}: {description_name}")
```

**只 warning 不裁** —— 因为摘要已经被 LLM 生成了，裁掉会更糟（把语义切断）。而超限的后果是嵌入时被截断（嵌入模型自己处理），所以「警告 + 交给下游」是合理的。

**注意这里的判据是「嵌入上限」而不是「摘要目标长度」** —— 两个不同的约束：`summary_max_tokens` 是**提示 LLM 写多长**（软），`embedding_token_limit` 是**嵌入模型能吃多少**（硬）。所以前者用于生成，后者用于校验。

### 摘要与 `d02` 的接口

**摘要在 d02 的合并流程里被调用两次（实体、关系），而返回值带 `llm_was_used`。** d02 已详述它的用途（日志前缀）。

**一个关键不变量：摘要结果替换整个描述列表。**

在 d02 里：

```python
node_data = dict(
    entity_id=entity_name, entity_type=entity_type,
    description=description,   # ← 摘要结果
    source_id=source_id, file_path=file_path, ...
)
```

**即：存储里只有一份 `description` 字符串**（多条碎片用 `GRAPH_FIELD_SEP` 拼接，或已被摘要替换）。所以「碎片列表」是**合并过程中的临时态**，不是持久态。

**这解释了为什么 d02 要做「跨已存+新去重」**：因为读出来的 `already_description` 是 `description.split(GRAPH_FIELD_SEP)`——若上一轮是「直接拼接」（没走 LLM），拆开就是原始碎片；若上一轮走了摘要，拆开就只有一条（摘要本身）。

**所以 `description` 字段有两种可能的内容：碎片拼接或摘要。** 这带来一处语义模糊：下一轮合并无法区分「这是一条原始碎片」与「这是一条别人写的摘要」，于是把它们同等对待。若一条摘要被当成碎片参与下一次摘要，信息会被二次压缩。

**这是一个真实的渐进退化**：每次合并都压一次，早期细节逐渐消失。而代码没有标记区分两者。

### 权重契约：两个函数，两个方向

**`relation_evidence_source_ids` 是契约的唯一解析器。**

```python
def relation_evidence_source_ids(source_id: str) -> list[str]:
    """Return ordered distinct source IDs that count as relation evidence.

    Empty values and historical no-source placeholders are intentionally excluded.
    New manual relations store an omitted source as an empty string; the placeholders
    remain here only so legacy rows keep the same meaning.
    """
    if not isinstance(source_id, str):
        raise ValueError("Relation field 'source_id' must be a string, got " + type(source_id).__name__)
    return list(dict.fromkeys(
        source for source in source_id.split(GRAPH_FIELD_SEP)
        if source and source not in RELATION_NO_EVIDENCE_SOURCE_IDS
    ))
```

四处：

1. **类型校验**：非字符串直接抛。因为 `source_id` 会被 `.split()`，用错类型会抛 `AttributeError`（信息量低），显式检查给出更好消息。

2. **`dict.fromkeys(...)` 去重且保序** —— 这与 d01 里 `_is_gleaning_extract_row` 用 `dict.fromkeys` 去重是同一手法。

3. **两层过滤**：空串（新手工关系的「省略来源」表达）与 `RELATION_NO_EVIDENCE_SOURCE_IDS`（历史占位符）。注释强调：「New manual relations store an omitted source as an empty string; the placeholders remain here only so legacy rows keep the same meaning」。

   **即：新数据用空串表达「无来源」，旧数据用占位符；两者都要排除，但只有空串是新约定。**

4. **`relation_evidence_count` 只是 `len(...)`** —— 所以「证据数」的定义完全集中在一个函数里。

**`validate_relation_weight` 拒绝低于地板的权重。**

```python
def validate_relation_weight(weight, source_id, *, context="Relation") -> float:
    """Normalize a relation weight and enforce its evidence-count floor.

    A relation weight is its evidence count plus an optional manual boost. A caller
    may choose a fractional weight only when the relation has no real source IDs;
    negative weights are therefore always invalid.
    """
    normalized = _sanitize_graph_fields({"weight": weight, "source_id": source_id},
                                        allowed_fields=_RELATION_DATA_FIELDS,
                                        object_type="relation", reject_unknown=True)
    normalized_weight = normalized["weight"]
    evidence_count = relation_evidence_count(normalized["source_id"])
    if normalized_weight < evidence_count:
        raise ValueError(f"{context} weight {normalized_weight} cannot be less than its distinct-source evidence count {evidence_count}")
    return normalized_weight
```

五处：

1. **先过 `_sanitize_graph_fields`** —— 复用通用的字段清洗（类型归一、禁止未知字段）。所以「权重必须是数字」这类检查在那里。

2. **`<` 而不是 `<=`** —— 权重**等于**证据数是合法的（每条来源贡献 1.0）。

3. **文档定义了语义**：「A relation weight is its evidence count plus an optional manual boost」。所以 `weight = count + boost`（`boost >= 0`）。

4. **「A caller may choose a fractional weight only when the relation has no real source IDs; negative weights are therefore always invalid」** —— 两处推论：
   - 无来源时 `evidence_count = 0`，所以 0.5 这类小数合法（没有地板约束）。
   - 负数**永远非法**（因为地板 ≥ 0）。这一条注释只说了结论，实现上是 `weight < evidence_count` 的自然结果（0 < 0 为假，所以 -1 < 0 为真 → 抛）。所以负数是被地板检查顺带挡住的，不是单独检查。

5. **它返回归一后的权重** —— 所以调用方能拿到「清洗过的值」，而不只是通过/不通过。

**`apply_relation_weight_floor` 相同但不抛，直接夹。**

```python
def apply_relation_weight_floor(weight: Any, source_id: str) -> float:
    """Normalize a stored weight and repair it to the evidence-count floor."""
    normalized = _sanitize_graph_fields(...)
    return max(normalized["weight"], float(relation_evidence_count(normalized["source_id"])))
```

**两个函数几乎完全一样，只差最后一步（抛 vs `max`）。**

这个重复是**有意的**，因为它们的语义契约不同：

| | `validate_relation_weight` | `apply_relation_weight_floor` |
|---|---|---|
| 适用 | 写入新数据（手工路径） | 读取/修复旧数据 |
| 坏值 | 让用户改 | 自动修 |
| 代价 | 一次往返 | 静默改变数据 |

**判断该用哪个的判据是「这个值是谁写的」**：用户写的要求他改（否则他会以为写进去了），历史/系统写的只能修（否则整个库不可读）。

**`_reject_self_loop_relation` 的文档列出了全部五处自环入口。**

```python
def _reject_self_loop_relation(source_entity, target_entity, *, operation) -> None:
    """Refuse a relation whose two endpoints are the same entity.

    One of the ingresses enforcing a STORE-level invariant: a graph must not
    contain a self-loop (see ``BaseGraphStorage.node_degree``, which is why
    degree is not defined on one). A self-loop carries no connectivity for
    graph retrieval — it only widens the surface where the rest of the code has
    to reason about ``src == tgt``.

    The other ingresses: ``operate.py`` drops ``source == target`` in both
    record parsers and again in ``_merge_edges_then_upsert``,
    ``amerge_entities`` skips any endpoint pair that would collapse into one,
    ``ainsert_custom_kg`` refuses one after name normalization, and
    ``tools/migrate_graph_storage.py`` refuses a source graph that holds one.
    Manual creation is the ingress THIS function covers.

    Rejected at the same layer as an empty description: before the keyed lock,
    so a refusal writes nothing.
    """
```

四处：

1. **不变量是「存储级」的**，而理由具体：「see `BaseGraphStorage.node_degree`, which is why degree is not defined on one」——**度数在自环上没有定义**（是 1 还是 2？）。所以整个图检索的度数计算都假设没有自环。

2. **五处入口各自校验** —— d01 的两个解析器 + d02 的 `_merge_edges_then_upsert` + `amerge_entities` + `ainsert_custom_kg` + 迁移工具。

   **这是「同一个不变量在多个入口重复校验」的做法。** 替代方案是只在存储层校验，但那样「非法输入会在更深处失败」，错误消息与位置都更远。

3. **「A self-loop carries no connectivity for graph retrieval — it only widens the surface where the rest of the code has to reason about `src == tgt`」** —— 即它不只是无价值，而是**有成本**（让所有代码都要考虑两端相同的情形）。

4. **「Rejected at the same layer as an empty description: before the keyed lock, so a refusal writes nothing」** —— **校验位置的选择理由**：在锁之前，所以拒绝时不会持有锁、不会写入任何东西。这与 d02 里「先构造并校验 VDB 载荷再写图」是同一原则。

**`_require_non_empty_description` 与自环检查成对出现。**

```python
def _require_non_empty_description(description, *, operation, object_type) -> None:
    if description is None or not str(description).strip():
        raise ValueError(f"{object_type.capitalize()} description cannot be empty for {operation} operation")
```

**注意 `not str(description).strip()`** —— 所以非字符串值（如数字）只要 `str()` 后非空白就通过。而 `description is None` 单独判（因为 `str(None)` 是 `"None"`，非空）。

**这是一处必须显式判 `None` 的实例**：若只写 `not str(description).strip()`，`description=None` 会得到 `"None"` → 通过 → 存进一个字符串 `"None"`。

## 关键取舍

**源码事实：** `_handle_entity_relation_summary` 四个提前返回：空列表 `("", False)`；单条**直接返回但必须 sanitize**（读回的数据未经抽取期清洗）；循环内「条数 < `force_llm_summary_on_merge` 且裸文本量满足上限」时 `separator.join`（**按裸文本计量**，因为这条出口不构造 prompt）；「总渲染量在预算内或只剩 2 条」时做最终 LLM 摘要。

**源码事实：** 分组与裁剪**都按 `_summary_description_line(desc) + "\n"` 计数**（JSON 行 + 换行），因为 `_summarize_descriptions` 的裁剪就是这个计价单位；按裸文本算会让分组超预算，然后被裁剪**静默丢掉尾部描述**。

**源码事实：** 分组强制「每组至少 2 条」（`len(current_chunk) == 1` 时再拉一条进来），因为 reduce 阶段对单条组**不调 LLM**；这既是优化也是**终止性保证**（否则单条组那一轮无压缩，可能无限循环）。强制配对会产超预算组，于是 `_summarize_descriptions` 内部裁剪并 warning。

**源码事实：** 循环靠「每组至少 2 条 → 条数至少减半」终止；剩 2 条时无条件走最终 LLM 摘要（即使超 context，此时 `_summarize_descriptions` 会裁掉并记「Oversize description found」）。

**源码事实：** `desc_token_counts` 缓存每条的渲染 token 数供分组复用，所以每条描述在压缩流程里只 encode 一次（除了直接拼接出口的 bare 计算）。

**源码事实：** 描述在 prompt 里是**一行一条 JSON**（`{"Description": ...}`、`ensure_ascii=False`），因为纯文本拼接会让描述边界模糊（描述本身可能含换行）。

**源码事实：** 摘要是**替换**而不是追加，所以空摘要会抹掉所有碎片——因此空摘要**抛 `EmptySummaryResponseError`** 而不是返回空串（让旧描述存活、文档可重试）。错误消息给出「思考模型请关闭 thinking」的诊断建议。

**源码事实：** 注释记录了一处已知残留缺陷：只含控制字符的响应在 `sanitize_text_for_encoding` 之前是非空的，所以**被缓存**；重试命中坏缓存 → 一直失败直到清缓存。

**源码事实：** 截断检测**必须在 sanitize 之前**——`sanitize_text_for_encoding` 重建普通 `str`，会丢掉 `remove_think_tags` 保留的 `TruncatedResponse` 标记。`truncation_write_ahead` 在 `record` 之后立即 `await`（记录先于它警告的变更）。

**源码事实：** 摘要的 LLM 调用被 `partial(..., _priority=DEFAULT_SUMMARY_PRIORITY)` 提优先级（8），因为摘要在合并阶段、已等过抽取。

**源码事实：** 摘要超 `embedding_token_limit` 时**只 warning 不裁**——摘要是硬约束（嵌入模型能吃多少），`summary_max_tokens` 是软目标（提示 LLM 写多长），两者用途不同。

**源码事实：** 存储里只有一份 `description`（碎片用 `GRAPH_FIELD_SEP` 拼接或已被摘要替换），所以「碎片列表」是合并过程中的临时态。**同一字段可能是「碎片拼接」也可能是「一条摘要」，下一轮无法区分**——所以摘要会被当碎片再次压缩，早期细节渐进消失。

**源码事实：** `relation_evidence_source_ids` 是权重的唯一解析器：非字符串直接抛；`dict.fromkeys` 去重保序；排除空串（新手工约定：省略来源 = 空串）与 `RELATION_NO_EVIDENCE_SOURCE_IDS`（历史占位符，保留只为让旧行保持原义）。

**源码事实：** 权重契约 `weight >= 不同真实来源数` 拆成两个函数：`validate_relation_weight` **抛错**（`weight < evidence_count` 时），`apply_relation_weight_floor` **夹到 `max(weight, count)`**。前者用于写入（让用户改），后者用于读旧数据（容错）。负数被地板检查顺带挡住（不是单独检查）。

**源码事实：** 权重语义是「证据数 + 可选手工加成」，所以无真实来源时允许小数权重（地板为 0）；`<` 而非 `<=`（权重等于证据数合法，每条来源贡献 1.0）。

**源码事实：** `_reject_self_loop_relation` 的文档列出了全部五处自环入口（d01 两个解析器、`_merge_edges_then_upsert`、`amerge_entities`、`ainsert_custom_kg`、迁移工具），并说明不变量是存储级的、理由是 `node_degree` 在自环上没有定义。校验位置与空描述检查相同——**在键控锁之前**，所以拒绝时不持有锁、不写任何东西。

**源码事实：** `_require_non_empty_description` 必须显式判 `description is None`，因为 `str(None).strip() == "None"` 非空，只写 `not str(...).strip()` 会让 `None` 通过并存入字符串 `"None"`。

**架构推论：** 摘要这一层的设计可以概括成「**用两套计价单位区分「存储什么」与「送什么给 LLM」**」。直接拼接出口按裸文本算（因为产出就是裸文本），而分组/裁剪按 JSON 渲染量算（因为 prompt 里就是 JSON）。混用会导致两类错误：按渲染量判拼接出口 → 白调 LLM；按裸文本判分组 → 分组超预算 → 裁剪静默丢数据。**同一份数据在两个环节有不同的体积，所以每个判据必须用它实际处理的那个体积。**

**架构推论：** 「空摘要抛错而不是返回空」是本页最重要的一处可靠性判断。它的前提是「摘要是替换语义」——若摘要是追加（像 d02 的碎片拼接），空结果只是「这次没加到东西」，无害。而替换语义下空结果等于**删除**。所以判据是**产出物的语义是替换还是追加**：替换必须 fail-closed，追加可以忽略空。

**架构推论：** `weight >= 来源数` 拆成「校验」与「夹地板」两个函数，是「同一不变量在写入侧与读取侧需要不同严格度」的标准形态。写入侧严格（否则用户以为写成功了），读取侧宽松（否则历史数据让整个库不可读）。**这类不变量几乎总该成对出现**——只做一侧会出问题：只校验则旧数据读不出，只夹地板则新错误值被静默吞掉。

**架构推论：** 「同一个不变量在五个入口重复校验」看起来违反 DRY，但对**存储级不变量**是合理的。因为存储层校验的位置太深（错误消息离用户远、且可能已产生部分写入），而每层的语境不同（解析器知道是 LLM 输出、`ainsert_custom_kg` 知道是用户输入）。**判据是「失败时能否不写任何东西」**——能在入口拒绝就在入口拒绝。

**架构推论：** 那条「只含控制字符的响应被缓存导致永久失败」的已知缺陷，是「两个校验的边界不一致」的典型：缓存层判 `if content`，清洗层判 `if content.strip()`。两者的差集就是「看起来有内容但实际没有」的响应。要修只能让缓存层也用清洗后的判据，或者把 `TruncatedResponse` 那类标记与空检查合并成一次判断。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/operate.py` | `_summary_description_line` | 一条描述渲染成一行 JSON |
| `lightrag/operate.py` | `_handle_entity_relation_summary` | map-reduce 主循环与四个提前返回 |
| `lightrag/operate.py` | `_handle_entity_relation_summary` 的裸文本出口 | 条数少且裸文本够小则直接拼接（不调 LLM） |
| `lightrag/operate.py` | `_handle_entity_relation_summary` 的分组段 | 强制每组 ≥ 2 条以保证终止 |
| `lightrag/operate.py` | `_summarize_descriptions` | 一次 LLM 汇总：裁剪、JSONL prompt、截断检测、空检查 |
| `lightrag/operate.py` | `_summarize_descriptions` 的截断检测段 | 在 sanitize 之前读 `TruncatedResponse` 标记 |
| `lightrag/operate.py` | `EmptySummaryResponseError` 抛出点 | 空摘要 fail-closed |
| `lightrag/utils_graph.py` | `relation_evidence_source_ids` | 权重的唯一证据解析器 |
| `lightrag/utils_graph.py` | `relation_evidence_count` | 不同真实来源数 |
| `lightrag/utils_graph.py` | `validate_relation_weight` | 写入侧：低于地板则抛错 |
| `lightrag/utils_graph.py` | `apply_relation_weight_floor` | 读取侧：夹到地板 |
| `lightrag/utils_graph.py` | `_require_non_empty_description` | 空描述拒绝（须显式判 `None`） |
| `lightrag/utils_graph.py` | `_reject_self_loop_relation` | 自环拒绝 + 五处入口的文档 |
| `lightrag/utils.py` | `atruncate_list_by_token_size` | 按渲染量把列表裁到预算 |
| `lightrag/utils.py` | `sanitize_text_for_encoding` | XML 非法字符清洗（会丢子类型标记） |
| `lightrag/utils_graph.py` | `_sanitize_graph_fields` | 图字段的通用归一与未知字段拒绝 |

## 参考

- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)
- [lightrag 源码：utils_graph.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/utils_graph.py)
- [论文：Map-Reduce 式长文本摘要](https://arxiv.org/abs/2005.00661)
- [GraphML 规范（XML 序列化约束）](http://graphml.graphdrawing.org/specification.html)

## 与主核 / AI 的关系

**摘要的 map-reduce** 与主核（多模态数据库内核）的**两阶段聚合**（`Agg` 节点的 partial aggregate → finalize）结构完全对应：都是「先把大集合分成能处理的组、每组算一个中间结果、再把中间结果聚合」。而两者都面对同一个约束：**中间结果必须比输入小，否则不收敛**。

主核保证这一点靠「聚合函数必须单调收缩」（`count`/`sum` 的输出是定长的）；这里靠「每组至少 2 条且单条组不调 LLM」。**判据是「这一轮是否严格减少元素数量」**——若不减少就不收敛。这里的「每条描述渲染成一行 JSON」还有一个副作用：JSON 包装让每条都变大，所以「压一次」的收益要扣掉包装开销。

第二处对照是 `validate_relation_weight` 与 `apply_relation_weight_floor` 的成对结构，与主核的 **`CHECK` 约束 vs `pg_dump` 里的修复**同理：`CHECK` 在写入时拒绝（用户必须改），而读老备份时若遇到历史脏数据只能修或跳过。主核的明确选择是「加载时不校验 CHECK，只在 `ALTER TABLE VALIDATE` 时才查」——把「写入侧严格」与「读取侧宽松」分成了两个显式操作。这里则用两个函数表达了同样的区分。

第三处是「截断标记必须在类型转换之前读取」，与主核的 **`ErrorData` 在 `elog` 展开前保留**同理：错误严重级别、SQLSTATE、附加字段都是结构化信息，一旦被 `format()` 成字符串就全丢了。所以主核的所有错误传播都保持 `ErrorData` 结构直到最后一层。这里是 `TruncatedResponse` 子类——同一类「不要过早降级成字符串」的纪律。

对 AI 工程的实际意义在于：描述摘要是 graph-RAG 里唯一「会主动删信息」的环节（其他环节只增不减），所以它的失败模式最危险。这份实现给出的三条经验可直接用：**替换式产出必须 fail-closed**（空摘要抛错而不是覆盖）、**同一份数据在不同环节有不同的体积，判据必须匹配对应体积**（裸文本 vs JSON 渲染量）、以及**不变量要拆成写入严格 + 读取宽松两个函数**。
