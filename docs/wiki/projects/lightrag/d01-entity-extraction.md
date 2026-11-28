---
project: lightrag
feature: entity-extraction
title: 实体关系抽取
page_type: feature
schema_version: 1
refs:
  - path: lightrag/operate.py
    symbols: [extract_entities]
  - path: lightrag/operate.py
    symbols: [_handle_single_entity_extraction, _handle_single_relationship_extraction, _normalize_and_validate_entity_type]
  - path: lightrag/operate.py
    symbols: [_process_json_extraction_result, _is_gleaning_extract_row, _parse_extract_row]
---

# 实体关系抽取

## 一句话职责

把块文本变成「实体集合 + 关系集合」：先用结构化 prompt 让 LLM 抽取，再用 **gleaning 多轮追问**把漏掉的补上，然后按「描述更长者胜」的策略合并两轮结果；而每一行 LLM 输出都要过一道「四字段/五字段 + 类型白名单 + 描述非空」的严格校验，坏行静默丢弃并记录原因。

## 机制是什么

抽取这一层要同时满足四个互相牵制的要求：

**要求一：LLM 输出格式必错，只是程度不同。** 所以解析器必须能接受三种形态：分隔符记录（文本模式）、JSON（JSON 模式）、以及**两者都可能带瑕疵**（漏字段、多字段、前缀写错、被 token 上限截断）。所以除了解析还有「恢复」。

**要求二：单轮抽取一定会漏。** 这是 lightrag 用 gleaning 的理由——第一轮抽完，把「用户 prompt + 助手回答」放进 history，再追加一句「你还有漏的吗」让模型补充。这是**用多一轮调用换召回率**。

**要求三：两轮的结果会冲突。** 同一个实体在两轮里可能有不同的描述。lightrag 的判据是**「描述更长的胜」**——一个简单但有效的代理指标。

**要求四：抽取失败必须可归因。** 一个解析失败有三种原因：模型没遵守格式、回答被 token 上限截断、或 payload 里本来就没东西。三者的修法完全不同（改 prompt / 提高输出预算 / 不用管），所以错误消息必须能区分。`TruncatedResponse` 与 `_truncation_cause_suffix` 就是为此存在。

## 怎么实现的

### 入口：`extract_entities` 的准备阶段

**它在最开始就检查取消，而不是只在每个块里检查。**

```python
if pipeline_status is not None and pipeline_status_lock is not None:
    async with pipeline_status_lock:
        if pipeline_status.get("cancellation_requested", False):
            raise PipelineCancelledException("User cancelled during entity extraction")
```

**注意后面每个块里还有一次同样的检查**（在 `_process_with_semaphore` 里）。所以是两层：入口拦掉「一开始就取消了」，块级拦掉「跑到一半取消了」。而每块检查在 `async with semaphore` 之**内**——所以等待信号量时不会拦，一拿到许可就拦。

**三个累加器各自负责一类「非致命问题」。**

```python
status_logger = PipelineStatusLogger(pipeline_status)
stage_tally = TokenLimitTruncationTally()
cache_skip_chunks: set[str] = set()
```

三处：

1. **`status_logger` 是「有界环形缓冲」的写入器** —— 注释说明它缓存了共享列表：「the first history write fetches the shared list once; every per-chunk log is then a single extend on the cached handle」。所以它是一次 fetch + N 次 append，而不是 N 次 fetch。

2. **`stage_tally` 是抽取阶段的截断计数**，且文档说明它「publishes the first truncation immediately and one aggregate at the end」——即**首次立即报，之后汇总报**。

3. **`cache_skip_chunks` 是 `set` 而不是计数器**，注释直说理由：「A set, not a tally: there is no per-stage breakdown to report and nothing is persisted」。

**所以三类问题的上报策略不同**：截断有阶段分解（`initial`/`gleaning`）所以要 tally；缓存跳过只有一个原因，只需知道「哪些块」；而两者都遵守「首次全量进日志 + 汇总进状态环」。

**prompt 构建被提到循环外，且有条件地跳过昂贵的 encode。**

```python
gleaning_precheck = (
    entity_extract_max_gleaning > 0
    and extract_tokenizer is not None
    and max_extract_input_tokens > 0
)
```

注释说明了理由：「The encodes stay lazy behind the same condition that gates the gleaning guard, so runs with gleaning off never pay them」。

**即：系统 prompt 里嵌了几千 token 的示例块，对它 encode 是有成本的；而只有 gleaning 开着才需要这个计数。所以关掉 gleaning 就完全不算。**

而 `gleaning_invariant_tokens` 的名字说明了它的用途：这是**每块都相同的那部分**（system prompt + JSON 模式的 continue prompt），所以算一次即可。每块变化的部分（chunk 内容、history）在块内另算。

**JSON 模式与文本模式的 `examples` 处理不同。**

```python
if use_json_extraction:
    examples = "\n".join(prompt_profile["entity_extraction_json_examples"])
    context_base = dict(..., examples=examples, ...)
else:
    examples = "\n".join(prompt_profile["entity_extraction_examples"])
    example_context_base = dict(tuple_delimiter=..., completion_delimiter=..., ...)
    examples = examples.format(**example_context_base)
    context_base = dict(..., examples=examples, ...)
```

**文本模式的 examples 需要先 `format` 一次**（把示例里的分隔符占位符填上），然后放进 `context_base`。而 JSON 模式不需要（示例已是完整 JSON）。

这是一处**两代 prompt 格式的差异**：文本模式用「元组分隔符」描述结构，示例里也要出现分隔符，所以示例本身是被 `format` 的模板；JSON 模式用字段名描述结构，示例是字面 JSON。

**`prompt_profile` 的回退有一个明确的理由。**

```python
prompt_profile = global_config.get("_entity_extraction_prompt_profile")
if prompt_profile is None:
    prompt_profile = resolve_entity_extraction_prompt_profile(addon_params, use_json_extraction)
```

注释：「Fallback for callers that construct `global_config` directly (e.g. tests or custom wiring). Re-run the resolver so behavior matches the cached path that `LightRAG.__post_init__` populates, instead of duplicating guidance/override logic here」。

**即：缓存与自算是同一个解析函数，所以两条路径不可能漂移。** 这是「让快路径与慢路径调同一个函数」的标准做法。

### `_process_single_content`：一块的完整流程

**第一步就剥掉解析器内部标记，而注释强调存储不动。**

```python
content = strip_internal_multimodal_markup_for_extraction(chunk_dp["content"])
```

注释：「Strip parser-internal markup ... The stored chunk content is left intact so query-time citations still resolve」。

这与 c05 里 `strip_internal_multimodal_markup_for_extraction` 的文档要求一致。

**标题上下文是三步：格式化成面包屑 → token 预算 → 套进 prompt 模板。**

```python
heading_path = _truncate_section_context(
    format_heading_context(chunk_dp),
    extract_tokenizer,
    DEFAULT_MAX_SECTION_CONTEXT_TOKENS,
)
heading_context_block = (
    PROMPTS["entity_extraction_section_context"].format(heading_path=heading_path)
    if heading_path
    else ""
)
```

四处：

1. **`format_heading_context`（c05）做「逐层字符截断 + 面包屑拼接」**。
2. **`_truncate_section_context` 再做「整体 token 预算」** —— 注释说明了为什么需要两道：「the joined path is token-budgeted here so heading metadata can never push an otherwise-valid chunk past the provider context window」。

   **两道截断的理由是「字符与 token 不可换算」**：字符截断保证单层不失控，token 预算保证整体不超窗。一道不够。

3. **无标题时 `heading_path` 是 `""`** → `heading_context_block` 也是 `""`。注释：「the block is an empty string so the user prompt stays byte-identical to the no-context form」。

   **「字节完全相同」这个要求很关键**：它保证「没有标题的块」与「旧版本处理的无标题块」prompt 一模一样，于是缓存键也相同——升级不会导致缓存全部失效。

4. **模板里的标记与包装在 `PROMPTS` 里**，这里只产数据。这是「prompt 的可变部分与固定结构分离」的做法，让改格式不用改代码。

**`_report_truncation` 与 `_report_cache_skip` 是两个同步的、不抛错的回调。**

```python
def _report_truncation(result: str, stage: str) -> None:
    if not is_truncated_response(result):
        return
    location = f"chunk {chunk_key} in {file_path}"
    logger.warning(f"Token-limit truncation during {stage} entity extraction for {location}")
    if stage_tally.record(stage, chunk_key):
        status_logger.log(f"Warning: token-limit truncation during {stage} entity extraction for {location}; further occurrences are reported as one summary at the end of extraction")
```

四处：

1. **`is_truncated_response(result)` 是判据** —— 即「这个响应被截断了」，而不是「这个响应超了预算」。所以它是一个**事后判断**（看响应本身有没有截断标记），不是事前估算。

2. **`stage_tally.record(...)` 的返回值决定要不要写状态日志** —— 所以「首次」由 tally 判定（`record` 返回真表示这是这个 stage 的第一次）。这样「首条 + 汇总」的纪律被封装在 tally 里，调用点不用记状态。

3. **两条消息的受众不同**：`logger.warning` 给服务端日志（**每次**都记），`status_logger.log` 给有界状态环（**首次**）。注释说明了这个分工。

4. **`_report_cache_skip` 用 `first = not cache_skip_chunks` 自己判首次**（因为它是 `set` 不是 tally）。所以两个回调的「首次」判定机制不同——一个是 tally 的返回值，一个是集合空判断。

   注释还说明了为什么缓存跳过**不**上报到文档级：「unlike truncation this is not recorded on the document, because the trigger is an unwritable text_chunks storage rather than a property of the document's content」。

   **判据很清晰：能否归因到文档内容。** 截断是内容太长（文档属性），缓存跳过是存储不可写（环境问题）。所以前者进 `doc_status.metadata`，后者只进日志。

**JSON 模式里有一处必须显式绑定局部变量的坑。**

```python
if use_json_extraction:
    # The local must be bound explicitly: the text-mode branch below
    # assigns this name, which makes it local to the whole closure
    # (Python scoping is static), so the gleaning LLM call a few
    # dozen lines down would otherwise read an unbound local here.
    entity_continue_extraction_user_prompt = json_continue_extraction_user_prompt
```

**这是 Python 作用域规则的经典陷阱**：`else` 分支里对 `entity_continue_extraction_user_prompt` 赋值，让这个名字在**整个函数**里都是局部变量（静态作用域）。所以 `if` 分支若不显式绑定，后面 gleaning 那段读到的就是未绑定局部 → `UnboundLocalError`。

注释把这件事写得很完整（「Python scoping is static」）。**这类 bug 在 code review 里极难发现**，因为它只在「JSON 模式 + gleaning 开启」时才触发。

**而文本模式下 continue prompt 每块重建，理由是有条件覆盖。**

```python
entity_continue_extraction_user_prompt = PROMPTS["entity_continue_extraction_user_prompt"].format(
    **{**context_base, "input_text": content}
)
```

注释：「The continue prompt stays per chunk because a custom prompt file may reference `{input_text}`; the default template's placeholders do not include it, but the call site keeps passing the kwarg for that override case」。

**即：默认模板不用 `{input_text}`，但自定义模板可能用，所以仍传。** 代价是每块 `format` 一次（比复用慢，但不贵）。

### gleaning：多轮追问与合并

**gleaning 的触发条件是三重与。**

```python
run_gleaning = entity_extract_max_gleaning > 0
if run_gleaning and extract_tokenizer is not None and max_extract_input_tokens > 0:
    gleaning_token_count = (
        gleaning_invariant_tokens
        + sum(len(extract_tokenizer.encode(msg.get("content", "") or "")) for msg in history)
        + (len(extract_tokenizer.encode(entity_continue_extraction_user_prompt)) if not use_json_extraction else 0)
    )
    if gleaning_token_count > max_extract_input_tokens:
        logger.warning(f"Gleaning stopped for chunk {chunk_key}: Input tokens ({gleaning_token_count}) exceeded limit ({max_extract_input_tokens}).")
        run_gleaning = False
```

四处：

1. **三重条件**：配置要求 gleaning、有 tokenizer、有上限。任一不满足就不做**预算预检**（但仍可能按配置跑 gleaning）。

   **注意这里的行为不对称**：`entity_extract_max_gleaning > 0` 是「要不要 glean」，而另外两个条件是「要不要做预算预检」。所以 `extract_tokenizer is None` 时 gleaning **照跑但不预检**。

2. **代价估算 = 不变量 + history 全部内容 + （文本模式的）continue prompt**。而 JSON 模式下 continue prompt 加 0，因为它不含 chunk 内容（在 `gleaning_invariant_tokens` 里已算）。

   这个区分是对的：JSON 模式的 continue prompt 是 prompt 级固定的，文本模式的嵌了 `input_text` 所以每块不同。

3. **超限就 `run_gleaning = False`** —— **跳过而不是让调用失败**。注释：「Pre-check here and skip rather than fail」。因为超限的后果是 provider 的 `context_length_exceeded`，那会让整个块失败；而跳过 gleaning 只是召回率略降。

   **这是一处正确的降级判断**：用「召回率下降」换「任务不失败」。

4. **`max_extract_input_tokens = 0` 表示禁用这个检查**（注释：「`0` disables the gleaning guard (gleaning always runs regardless of size)」）。

**合并策略是「描述更长的胜」，而它依据的是列表第一个元素。**

```python
for i, (entity_name, glean_entities) in enumerate(glean_nodes.items(), start=1):
    if entity_name in maybe_nodes:
        original_desc_len = len(maybe_nodes[entity_name][0].get("description", "") or "")
        glean_desc_len = len(glean_entities[0].get("description", "") or "")
        if glean_desc_len > original_desc_len:
            maybe_nodes[entity_name] = list(glean_entities)
    else:
        maybe_nodes[entity_name] = list(glean_entities)
    await _cooperative_yield(i, every=8)
```

四处：

1. **`[0]` 只看第一个** —— 而 `maybe_nodes[name]` 是**列表**（同一实体在一个块里可能被抽出多次）。所以合并时比较的是「第一个出现」的描述长度，若 glean 胜则**整个列表替换**。

   **这意味着「替换整个列表」而不是「逐条合并」**：若初始轮抽了 3 条、glean 轮抽了 1 条但更长，结果就剩 1 条。丢掉的那 2 条描述从此消失。

   这与 `_merge_chunk_records`（跨块合并用的函数，见下）的策略不同——那里是列表拼接。

2. **`list(glean_entities)` 复制** —— 因为后面还要用原列表？实际上没有。这个复制是防御性的（避免两个字典共享同一列表对象）。

3. **`_cooperative_yield(i, every=8)`** —— 每 8 项让出一次事件循环。注释说这是为了让 API 协程能恢复。所以即使在纯 CPU 的合并循环里也不长时间独占事件循环。

   注意 `i` 是 `enumerate(start=1)` 的计数，所以第 8、16、24… 次让出。

4. **实体与关系用完全同构的两段循环**（只差字典与日志文案）。这是重复代码，但保持对称便于对照。

**while 循环里没有「再多轮」** —— 注释说「Process additional gleaning results only 1 time when `entity_extract_max_gleaning` is greater than zero」。

**所以 `entity_extract_max_gleaning` 实际上是一个布尔开关，不是轮数。** 这是一个命名与实现不一致的地方：配置名暗示「最多 N 轮」，实际只跑 1 轮。

### 校验：三道路径汇聚到同一组函数

**`_normalize_and_validate_entity_type` 是文本与 JSON 两条路径共享的类型校验，而它先列了一份「保留名」黑名单。**

```python
_RESERVED_ENTITY_TYPES = frozenset({"__proto__", "constructor", "prototype"})
```

注释把这条的来龙去脉说得很具体：

> JavaScript Object.prototype member names. When such a value is stored as an entity_type it crashes the WebUI knowledge-graph view: the type-color resolver indexes a plain object with it and the Legend later calls a string method on the resulting non-string map key (see graphColor.ts in the WebUI). They are never valid entity types, so reject them at the extraction source.

**这是「下游的原型污染漏洞在数据源头堵住」的做法**：`__proto__`/`constructor`/`prototype` 在 JS 里是 `Object.prototype` 的成员，用它们当字典键会拿到函数而不是颜色值，于是后续 `.toUpperCase()` 之类的调用崩掉。

`_proto__` 这类名字作为实体类型显然没有语义价值，所以「在源头拒绝」是对的——比在 WebUI 里逐个防御便宜得多。

**校验函数的四段，每段一种拒绝理由。**

```python
if not entity_type.strip() or any(char in entity_type for char in ["'", "(", ")", "<", ">", "|", "/", "\\"]):
    logger.warning(f"Entity extraction error: invalid entity type in {context}")
    return None
```

**八个禁用字符**：单引号、四种括号/尖括号、竖线、两种斜杠。它们都是**分隔符或标记字符**——因为实体类型会进 prompt、进存储、进 WebUI 的 HTML 渲染，这些字符会破坏某一层。

```python
if "," in entity_type:
    original = entity_type
    tokens = [t.strip() for t in entity_type.split(",")]
    non_empty = [t for t in tokens if t]
    if not non_empty:
        return None
    entity_type = non_empty[0]
    logger.warning(f"Entity type contains comma, taking first non-empty token ...")
```

**逗号的处理是「取第一个非空 token」而不是拒绝** —— 因为模型常输出 `"person, organization"` 这类多类型。取第一个是**有损但宽容**的选择，并记 warning。

注意 `if not non_empty: return None`——所以 `",,,"` 这种全空会被拒。

```python
entity_type = entity_type.replace(" ", "").lower()
```

**去空格 + 小写**。所以 `"Person"`/`"person"`/`"Per son"` 都归一成 `"person"`。这保证了「同一类型的不同写法」在图谱里是同一个值。

```python
if entity_type in _RESERVED_ENTITY_TYPES:
    logger.warning(f"Entity extraction error: reserved entity type '{entity_type}' rejected in {context}")
    return None
return entity_type
```

**注意保留名检查在归一之后** —— 所以 `"__PROTO__"` 会被 `lower()` 变成 `"__proto__"` 然后被拒。顺序是对的。

**`_handle_single_entity_extraction` 的字段数与前缀双校验。**

```python
if len(record_attributes) != 4 or "entity" not in record_attributes[0]:
    if len(record_attributes) > 1 and "entity" in record_attributes[0]:
        logger.warning(f"{chunk_key}: LLM output format error; found {len(record_attributes)}/4 fields on ENTITY ...")
        logger.debug(record_attributes)
    return None
```

三处：

1. **`!= 4` 是精确等于** —— 少字段多字段都拒。所以无法「宽容地取前 4 个」。

2. **`"entity" not in record_attributes[0]`** —— 用子串而非相等。所以 `"entity"`、`"ENTITIES"`（大写不算，因为区分大小写）、`"(entity"` 都通过。这给了容错，但也可能误收。

   注意这与 `_normalize_text_extraction_record_attributes` 的修复逻辑配合：那里用 `"entity" in prefix and "relation" in prefix` 的反向判断来识别「关系行用了 entity 前缀」。

3. **内层 `if` 只在「前缀像但字段数不对」时告警** —— 所以「完全不像记录的行」静默跳过（比如模型输出的解释文字）。这是一个**噪音控制**：只有「看起来是该是实体行但格式错」才值得记。

   而 `logger.debug(record_attributes)` 把原始字段打到 debug 级——生产环境不输出，排查时打开就有。

**字段清洗用同一条流水线，但参数不同。**

```python
entity_name = normalize_entity_name(record_attributes[1], final_space_pass=False)
entity_type = sanitize_and_normalize_extracted_text(record_attributes[2], remove_inner_quotes=True)
entity_type = _normalize_and_validate_entity_type(entity_type, f"record {record_attributes}")
entity_description = sanitize_and_normalize_extracted_text(record_attributes[3], multiline=True)
```

四处：

1. **名字用 `normalize_entity_name(..., final_space_pass=False)`** —— `final_space_pass=False` 说明「最后一遍空格处理」被关掉。这与 d04 的名称归一契约（只能合不能分）有关：多一遍空格处理可能把名字拆开。

2. **类型用 `remove_inner_quotes=True`** —— 去掉内部的引号，因为模型常写 `"Person"`。

3. **描述用 `multiline=True`** —— 允许换行（描述可以是段落）。

4. **名字为空、类型非法、描述为空 → 分别返回 `None`**，且**每种都有独立的日志**。所以一条坏记录能定位到具体哪一步失败。

**异常处理分两类，且日志级别不同。**

```python
except ValueError as e:
    logger.error(f"Entity extraction failed due to encoding issues in chunk {chunk_key}: {e}")
    return None
except Exception as e:
    logger.error(f"Entity extraction failed with unexpected error in chunk {chunk_key}: {e}")
    return None
```

**`ValueError` 被专门识别为「编码问题」** —— 因为 `normalize_entity_name` 内部可能就是抛 `ValueError`。而关系的处理里这两个 except 是 `logger.warning` 而不是 `error`。

**实体用 `error`、关系用 `warning`** —— 这个级别差异说明「丢一个实体比丢一条关系严重」。

**关系抽取多两道检查。**

```python
if source == target:
    logger.debug(f"Relationship source and target are the same in: {record_attributes}")
    return None
```

**自环被拒，且只记 debug** —— 因为「模型抽出一个自环」是常见但不严重的问题，不值得 warning。

```python
edge_keywords = sanitize_and_normalize_extracted_text(record_attributes[3], remove_inner_quotes=True)
edge_keywords = edge_keywords.replace("，", ",")
```

**中文逗号被归一成英文逗号** —— 因为关键词是逗号分隔的列表，中文逗号会让拆分失效。这是一处**针对中文输出的具体修补**。

```python
# Prompt text rows are 5 fields with no weight; keep default 1.0.
weight = 1.0
```

**权重硬编码 1.0**，注释说明「prompt 里的行是 5 字段、没有权重」。所以关系权重的计算不在这里（见 d03），这里只是占位。

### JSON 路径：容错解析与两处修补

**`_process_json_extraction_result` 用 `tolerant_load_json_dict` 兜住一切。**

```python
if parsed is None:
    parsed = tolerant_load_json_dict(result)
if not parsed:
    logger.warning(f"{chunk_key}: JSON extraction result is empty or unrecoverable{_truncation_cause_suffix(result)}")
    return dict(maybe_nodes), dict(maybe_edges)
```

注释说明了这个 helper 的能力与代价：

> `tolerant_load_json_dict` strips fences, tolerates leading/trailing prose (including trailing braces), and repairs the malformed-object slips json_repair fixes. It never raises and returns `{}` for unparseable input or a top-level array (callers require exactly one object). Note: because the helper absorbs the underlying parse exception, only this single "empty or unrecoverable" warning is logged — the low-level decode error is no longer surfaced.

**「吸收了底层异常」是一个明确的代价**：排查时看不到「第几个字符解析失败」。换来的是「永不说谎的返回类型」（总是 dict）。

**顶层数组被拒**（返回 `{}`）—— 因为调用方要求恰好一个对象。而模型很可能输出 `[{...}]`，所以这是一个常见的失败路径，被静默归入「unrecoverable」。

**`_truncation_cause_suffix` 是那条「三种原因要能区分」的具体实现。**

```python
logger.warning(f"{chunk_key}: JSON extraction result is empty or unrecoverable{_truncation_cause_suffix(result)}")
```

它的文档说：

> An undelimited / unparseable extraction response has two very different causes — a model that ignored the output contract, and a model that was cut off mid-answer — and only the second is fixed by raising the output budget. `TruncatedResponse` survives `remove_think_tags`, so the parsers can tell them apart instead of leaving the operator to correlate this warning with a separate truncation line by chunk id.

**这是「把两个独立告警的关联工作交给代码而不是运维」的实例**：截断信息直接嵌进解析失败的告警里，不用人去按 chunk id 对照两条日志。

**`parsed` 参数存在的理由值得记。**

```python
def _process_json_extraction_result(result, chunk_key, timestamp, file_path="unknown_source", parsed=None):
    """...
    parsed: Payload already recovered by :func:`tolerant_load_json_dict`, for
        callers that need the parse outcome themselves rather than only the
        extracted entities -- an empty result does not say whether the payload
        was unrecoverable or merely carried nothing, and cache rebuild has to
        tell those apart. ``None`` means "not supplied" and is unambiguous: the
        helper always returns a dict, so an unrecoverable payload still arrives
        here as ``{}``.
    """
```

**这解决的是「空结果有歧义」问题**：返回两个空字典既可能是「payload 无法解析」也可能是「payload 合法但没有实体」。d05 的缓存重建需要区分这两者（前者不能缓存，后者可以），所以让它自己解析一次再把结果传进来。

而 `None` 作为哨兵是安全的——因为 `tolerant_load_json_dict` 永远不返回 `None`。

**LaTeX 反斜杠的修复必须在清洗之前。**

```python
# Models quoting LaTeX in descriptions routinely under-escape backslashes
# ("\frac" is valid JSON meaning form feed + "rac"); restore the zero-risk
# cases before sanitization would otherwise delete the control characters
# and leave a maimed formula.
parsed = repair_vlm_json_escape_damage_nested(parsed, context=chunk_key)
```

四处：

1. **问题机制**：JSON 里 `"\frac"` 是合法转义（`\f` = form feed），所以解析后得到「换页符 + rac」。模型本意是 `\frac`（LaTeX 命令），但写 JSON 时忘了写成 `\\frac`。

2. **为什么必须在清洗前**：清洗会把控制字符（`\f` 是 `Cc`，见 c05）剥掉，于是公式变成 `"rac"`——彻底不可恢复。

3. **「zero-risk cases」** —— 只修那些明确无歧义的（如 `\f` 后跟字母构成已知 LaTeX 命令）。有歧义的不动。

4. **`_nested` 说明它递归进嵌套结构**（实体列表、关系列表里每一项的描述）。

**`_json_extraction_field_text` 处理「字段可能不是字符串」。**

从前面读到的签名看，它接受 `(value, joiner, context)`，其中 `joiner` 在实体类型处传 `", "`、描述处传 `"\n"`、名字处传 `None`。所以：
- 名字：`joiner=None` → 非字符串标量转 `str`，列表则丢弃（`_json_extraction_field_text` 里那条 `logger.warning(f"{context}: JSON identifier is not a single name, dropped")` + `return ""`）。
- 类型/描述：用 joiner 把列表拼成一个字符串。

**这是一个「同一个 helper 三种用法」的设计**：靠 `joiner` 的取值决定「要求标量」还是「允许列表」。

### 输出校验与阶段汇总

**`_is_gleaning_extract_row` 是一个「靠 prompt 尾部字符串识别缓存行类型」的判断。**

```python
_GLEANING_HISTORY_PREFIX = '[{"role": '

def _is_gleaning_extract_row(row):
    prompt = row.get("original_prompt")
    if not isinstance(prompt, str) or "\n" not in prompt:
        return False
    tail = prompt.rsplit("\n", 1)[1]
    if not tail.startswith(_GLEANING_HISTORY_PREFIX):
        return False
    try:
        history = json.loads(tail)
    except ValueError:
        return False
    return (isinstance(history, list) and bool(history)
            and all(isinstance(msg, dict) and "role" in msg for msg in history))
```

五处：

1. **背景**：`use_llm_func_with_cache` 把 prompt、system prompt、序列化 history 用换行拼起来存进 `original_prompt`。而**只有 gleaning 调用带 history**。

2. **`json.dumps` 会转义消息内部的换行**，所以 history 恰好是 prompt 的**最后一行**。所以 `rsplit("\n", 1)[1]` 就能取到它。

3. **`startswith('[{"role": ')` 是一个廉价前置筛**（默认 `separators` 会产生这个前缀），再真正 `json.loads` 确认。

4. **三重校验**：是 list、非空、每项是带 `role` 的 dict。所以一个畸形的 JSON 数组不会被误认。

5. **`row.get("original_prompt")` 缺失时返回 `False`**（当作初始抽取）。这是一处**没有更好判据时的保守假设**：宁可按初始处理，也不误判为 gleaning。

   **这个函数的脆弱点很明显**：它依赖「prompt 拼接格式」与「`json.dumps` 必须在最后一行」这两个实现细节。同一份数据若换个后端存储（丢掉 `original_prompt`），这个判断就永远返回 `False`。

**`_parse_extract_row` 是「必须成功否则抛」的薄封装。**

```python
async def _parse_extract_row(text_chunks_storage, row, chunk_id, chunk_data):
    result = row.get("return")
    if not isinstance(result, str) or not result.strip():
        raise ValueError("cache row holds no extraction text")
    return await _rebuild_from_extraction_result(..., extraction_result=result, timestamp=row.get("create_time", 0), ...)
```

**它抛而不返回空** —— 文档说「parse one extract cache row for `chunk_id`; raise if it is unusable」。所以调用方（d05 的缓存重建）能区分「这行坏了」与「这行没内容」。

而 `timestamp=row.get("create_time", 0)` —— 用缓存行的创建时间当抽取时间戳。这是一个**语义选择**：重建时用「当时抽取的时间」而不是「现在」，保证时间戳稳定（否则每次重建都会把所有描述的时间刷新，破坏「按时间排序」的稳定序）。

**阶段汇总放在 `finally` 里，而注释解释了三处曾经的漏洞。**

```python
finally:
    # On EVERY exit — mirrors merge_nodes_and_edges. The success and
    # failure paths used to publish explicitly, but an external
    # cancellation landing on the asyncio.wait above bypassed both,
    # and the caller's failure bookkeeping (custom chunks) then
    # persisted FAILED with no llm_truncation for responses that had
    # already been recorded. Await-free, called exactly once.
    _publish_truncation_summary()
    _publish_cache_skip_summary()
```

四处：

1. **「曾经的成功路径与失败路径分别显式调用」** —— 于是「外部取消落在 `asyncio.wait` 上」这一条路**两条都没走到**。
2. **后果很具体**：调用方的失败记账会 persist `FAILED` 但没有 `llm_truncation`，尽管有些截断已经被记进 tally 了。
3. **「Await-free」是关键要求** —— 因为在取消 unwind 里不能再 `await`。
4. **`_publish_truncation_summary` 里 `truncation_tally.absorb(stage_tally)`** —— 把阶段计数交给调用方（文档级），因为「the condition outlives the bounded status ring」（状态环会被后来的日志挤掉，而文档元数据不会）。

**任务编排用 `asyncio.wait(FIRST_EXCEPTION)` + 两次遍历 + `done` 的顺序陷阱。**

```python
done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_EXCEPTION)

for task in done:
    try:
        exception = task.exception()
        if exception is not None and first_exception is None:
            first_exception = exception
    except Exception as e:
        if first_exception is None:
            first_exception = e

if first_exception is None:
    chunk_results = [task.result() for task in tasks]
```

四处，而注释里那段分析是本页最值得记的工程推理：

1. **`done` 是 `set`，迭代顺序来自对象的 `__hash__`（即内存地址）** —— 所以「既不是完成顺序也不是创建顺序，且跨进程不稳定」。

2. **后果具体**：从 `done` 收集结果会让 `chunk_results` 每次是不同排列。而那个排列会通过 `merge_nodes_and_edges` 进入持久化状态——因为 `source_id` 与 `file_path` 是「保序去重」构造的，没有排序兜底，于是 `apply_source_ids_limit` 会截断**不同的子集**。

   **这是本页最严重的隐患的完整链条**：set 迭代顺序 → 结果排列 → 保序去重 → 截断子集 → 持久化字段不同。而且它是**跨进程不稳定**的，所以同一个文档重跑两次会得到不同的 `source_id` 列表。

3. **修法：结果从 `tasks`（`ordered_chunks` 顺序构造的 list）取** —— 所以 `chunk_results[i]` 恒对应 `ordered_chunks[i]`。

4. **两遍不能合成一遍**：异常路径上 `tasks` 里还有 pending 项，对它们调 `.exception()` 会抛 `InvalidStateError`，而下面的 `except Exception` 会把它当成 `first_exception`，**掩盖真正的失败**。

   **这是一处非常具体的「合并循环会引入新 bug」的例子。**

**取消处理与 `asyncio.wait` 的一个非显然性质。**

```python
except (Exception, asyncio.CancelledError):
    # asyncio.wait never cancels the tasks it waits on. Without this, an
    # external cancellation of this coroutine (pipeline cancel, server
    # shutdown) leaves the chunk tasks running: still calling the LLM for
    # a document already abandoned, and failing later with nobody left to
    # retrieve the exception ("Task exception was never retrieved").
    await cancel_and_drain_tasks(tasks)
    raise
```

四处：

1. **`asyncio.wait` 不取消它等待的任务** —— 这是与 `gather` 的重要差别（`gather` 在外部取消时会取消子任务）。
2. **不处理的后果**：块任务继续跑，还在为已放弃的文档调 LLM，并且稍后失败时没人取异常（"Task exception was never retrieved"）。
3. **`cancel_and_drain_tasks` 既取消又收全结果** —— 注释说「Same rule as `wait_tasks_with_drain`: cancel what still runs and collect every outcome before unwinding」。
4. **`except (Exception, asyncio.CancelledError)` 两个都捕** —— 因为 `CancelledError` 在 Python 3.8+ 继承 `BaseException` 而非 `Exception`，所以必须显式列出。

**`_process_with_semaphore` 里有一处 `await asyncio.sleep(0)`。**

```python
result = await _process_single_content(chunk)
await asyncio.sleep(0)
return result
```

**注释解释了为什么**：「Yield once between chunk completions so API coroutines can resume even when many chunk tasks are hitting cache and finishing quickly」。

**即：命中缓存的块几乎不耗时，于是 `async` 函数可能一路同步执行到底，不给事件循环机会让出。** 加一个 `sleep(0)` 保证每块结束后至少让出一次。

这是一处**针对「异步函数但实际无 await」的补救** —— 比在每个 `if` 分支里插 `await` 更简单。

## 关键取舍

**源码事实：** 抽取有两代 prompt 路径（文本分隔符 / JSON），共享类型校验与 sanitize 流水线；两代路径的 `examples` 预处理不同（文本模式的示例本身要 `format` 一次填分隔符）。

**源码事实：** `prompt_profile` 缺失时**重新调用同一个 resolver**，让「缓存路径」与「自算路径」不可能漂移。

**源码事实：** gleaning 的 token 预检用 `gleaning_invariant_tokens`（每块相同的 system prompt + JSON continue prompt，只在三个条件同时满足时才 encode），再加上 history 与文本模式的 continue prompt；超 `MAX_EXTRACT_INPUT_TOKENS` 时**跳过 gleaning 而不是让块失败**；`0` 表示禁用该检查。

**源码事实：** `entity_extract_max_gleaning` 实际上是**布尔开关**——代码只跑一轮 gleaning，与名字暗示的「最多 N 轮」不符。

**源码事实：** 两轮结果的合并判据是**「描述更长者胜」**，且只看列表的 `[0]`（第一个出现）并**整体替换列表**——所以 glean 轮胜出时初始轮的其余描述会丢。

**源码事实：** `_normalize_and_validate_entity_type` 的三段：八个禁用字符（单引号 + 四种括号/尖括号 + 竖线 + 两斜杠）直接拒；逗号取第一个非空 token（有损但宽容，记 warning）；去空格 + 小写后检查保留名。

**源码事实：** `_RESERVED_ENTITY_TYPES = {"__proto__", "constructor", "prototype"}`，注释说明这些值作为 entity_type 会让 WebUI 的图谱视图崩溃（颜色解析器用它索引普通对象，Legend 再对非字符串键调字符串方法）——所以在抽取源头拒绝，而不是在 WebUI 逐处防御。

**源码事实：** 保留名检查在 `lower()` **之后**，所以 `"__PROTO__"` 也会被拒。

**源码事实：** 记录校验要求**精确的字段数**（实体 4、关系 5）与首字段含 `"entity"`/`"relation"` 子串；只有「前缀像但字段数不对」才 warning，完全不像的行静默跳过（噪音控制）；原始字段打 debug 级。

**源码事实：** 名字用 `normalize_entity_name(..., final_space_pass=False)`、类型用 `remove_inner_quotes=True`、描述用 `multiline=True` —— 同一条 sanitize 流水线三种参数。三种失败（名字空 / 类型非法 / 描述空）各有独立日志。

**源码事实：** 异常分 `ValueError`（识别为编码问题）与 `Exception`，实体路径用 `logger.error`、关系路径用 `logger.warning`——「丢实体比丢关系严重」。

**源码事实：** 自环关系被拒且只记 `debug`；`edge_keywords` 把中文逗号 `，` 归一成英文 `,`（否则拆分失效）；`weight = 1.0` 硬编码（注释：「Prompt text rows are 5 fields with no weight」）。

**源码事实：** `tolerant_load_json_dict` **吸收底层解析异常**，所以只有一条「empty or unrecoverable」告警，看不到具体解码错误；顶层数组被拒（返回 `{}`）因为它要求恰好一个对象。

**源码事实：** `parsed` 参数让调用方把已解析的 payload 传进来，为的是区分「无法解析」与「解析成功但没东西」（缓存重建需要这个区分）；`None` 作哨兵是安全的，因为 helper 永不返回 `None`。

**源码事实：** `repair_vlm_json_escape_damage_nested` **必须在 sanitize 之前**——因为 `"\frac"` 是合法 JSON（form feed + `rac`），而清洗会剥掉 `\f` 让公式变成 `"rac"` 不可恢复；只修无歧义的「零风险」情形。

**源码事实：** `_json_extraction_field_text(value, joiner, context)` 靠 `joiner` 决定「要求标量」（`None`，名字）还是「用 joiner 拼列表」（类型用 `", "`、描述用 `"\n"`）。

**源码事实：** `_is_gleaning_extract_row` 靠「`original_prompt` 最后一行以 `[{"role": ` 开头且能 `json.loads` 成非空 dict 列表」识别 gleaning 行——依赖 prompt 拼接格式与 `json.dumps` 不换行两个实现细节；`original_prompt` 缺失时保守地按初始抽取处理。

**源码事实：** `_parse_extract_row` 对不可用缓存行**抛 `ValueError` 而不是返回空**；`timestamp` 用行的 `create_time`（保时间戳稳定，否则每次重建会刷新描述排序）。

**源码事实：** 阶段汇总放在 `finally`：注释记录了三处历史漏洞——成功与失败路径分别显式调用，导致「外部取消落在 `asyncio.wait` 上」时两条都没走，调用方 persist `FAILED` 但缺 `llm_truncation`。汇总函数必须 **await-free**（要能在取消 unwind 里运行）且恰好调用一次。

**源码事实：** 任务编排用 `asyncio.wait(FIRST_EXCEPTION)`，**结果必须从 `tasks`（有序 list）取而不是 `done`（set）**——注释详细说明 `done` 的迭代顺序来自内存地址、跨进程不稳定，会让 `chunk_results` 每次不同排列，最终通过保序去重与 `apply_source_ids_limit` 让持久化的 `source_id`/`file_path` 截断不同子集。

**源码事实：** 「取异常」与「取结果」不能合成一遍循环：异常路径上 `tasks` 仍含 pending 项，对其调 `.exception()` 抛 `InvalidStateError`，会被 `except Exception` 当成 `first_exception` 从而掩盖真实失败。

**源码事实：** `asyncio.wait` **不取消**它等待的任务，所以 `except (Exception, asyncio.CancelledError)` 里必须 `await cancel_and_drain_tasks(tasks)`，否则块任务继续为已放弃的文档调 LLM 并产生「Task exception was never retrieved」。

**源码事实：** `_process_with_semaphore` 在每块结束后 `await asyncio.sleep(0)`——因为命中缓存的块几乎不耗时，`async` 函数可能一路同步跑到底，不给事件循环让出机会。

**源码事实：** 抽取的三类非致命问题上报策略不同：截断用 `TokenLimitTruncationTally`（有阶段分解、进文档元数据、首次+汇总）、缓存跳过用 `set`（无分解、不进文档，因为原因是存储不可写而非文档属性）、两者都「每次进服务端日志、首次+汇总进有界状态环」。

**架构推论：** 这一层的形状是「**用多一次 LLM 调用换取召回率，再用一套严格校验把多出来的噪声挡住**」。gleaning 提高召回但也提高噪音（模型被追问时会编造），所以校验必须严格——而校验的严格性体现在「精确字段数」与「三种失败各有日志」。判据是**代价的不对称**：漏抽一个实体会丢失整条知识边（不可恢复），多抽一个坏实体会被校验挡掉（可恢复）。所以宁可偏向「多抽 + 严校验」。

**架构推论：** `_RESERVED_ENTITY_TYPES` 这条最能说明「数据源头 vs 消费端防御」的取舍。JS 原型污染本可以在 WebUI 里修（用 `Map` 而不是对象、或者 `Object.create(null)`），但那样要改的是**每一处**用 entity_type 当键的地方。在抽取源头拒绝只需一次判断，且语义上 `__proto__` 作为实体类型本就没有价值。**判据是「这个取值是否可能有合法用途」**——不可能，就在源头拒。

**架构推论：** `done` 是 `set` 那条注释是本页最有价值的工程记录。它把一个**极难复现的 bug**（同一文档重跑得到不同的持久化字段）追到了根因（set 迭代顺序来自内存地址），并说明了它如何穿过保序去重与截断进入磁盘。值得记的是修法本身很轻（从 `tasks` 取结果），但**发现它需要理解整条链路**——这类 bug 无法靠单元测试覆盖，只能靠读代码 + 想清楚「这个集合的顺序有保证吗」。

**架构推论：** 「阶段汇总放 `finally` 且必须 await-free」是一条可推广的纪律。任何「在成功与失败两条路上都要做的收尾」都会漏掉第三条路（取消/异常落在了别人身上）。唯一的修法是 `finally`，而 `finally` 里的代码不能 `await`（因为可能在取消 unwind 中）。这条约束会反过来限制收尾能做什么——所以收尾应该设计成「纯内存操作 + 同步日志」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/operate.py` | `extract_entities` | 抽取入口：取消检查、三类累加器、prompt 构建、任务编排 |
| `lightrag/operate.py` | `extract_entities._process_single_content` | 单块完整流程（标记剥离 → 标题上下文 → LLM → gleaning → 校验 → 多模态注入） |
| `lightrag/operate.py` | `extract_entities._report_truncation` | 截断上报（每次日志 + 首次状态环） |
| `lightrag/operate.py` | `extract_entities._report_cache_skip` | 缓存跳过上报（不进文档元数据） |
| `lightrag/operate.py` | `extract_entities._publish_truncation_summary` | 阶段汇总，`finally` 里调用、await-free |
| `lightrag/operate.py` | `extract_entities._publish_cache_skip_summary` | 缓存跳过汇总 |
| `lightrag/operate.py` | `extract_entities._process_with_semaphore` | 信号量 + 块级取消检查 + `sleep(0)` 让出 |
| `lightrag/operate.py` | `_RESERVED_ENTITY_TYPES` | 三个 JS 原型成员名 |
| `lightrag/operate.py` | `_normalize_and_validate_entity_type` | 类型三段校验（禁用字符 / 逗号 / 保留名） |
| `lightrag/operate.py` | `_handle_single_entity_extraction` | 文本模式实体行（精确 4 字段） |
| `lightrag/operate.py` | `_handle_single_relationship_extraction` | 文本模式关系行（精确 5 字段 + 自环拒绝 + 中文逗号归一） |
| `lightrag/operate.py` | `_normalize_text_extraction_record_attributes` | 修复「关系行用了 entity 前缀」 |
| `lightrag/operate.py` | `_truncation_cause_suffix` | 把截断原因嵌进解析失败告警 |
| `lightrag/operate.py` | `_process_json_extraction_result` | JSON 路径（容错解析 + LaTeX 修复 + 逐字段校验） |
| `lightrag/operate.py` | `_json_extraction_field_text` | 靠 `joiner` 区分「要求标量」与「拼接列表」 |
| `lightrag/operate.py` | `_is_gleaning_extract_row` | 靠 prompt 尾行识别 gleaning 缓存行 |
| `lightrag/operate.py` | `_parse_extract_row` | 不可用则抛；时间戳取 `create_time` |
| `lightrag/operate.py` | `_find_custom_kg_chunks` | 识别 `ainsert_custom_kg` 写入的块（不重抽） |
| `lightrag/chunk_schema.py` | `strip_internal_multimodal_markup_for_extraction` | 抽取前剥解析器内部标记（存储不动） |
| `lightrag/chunk_schema.py` | `format_heading_context` | 标题面包屑（逐层字符截断） |

## 参考

- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)
- [lightrag 论文：Graph-based RAG 与 gleaning 策略](https://arxiv.org/abs/2410.05779)
- [Python 文档：asyncio.wait 与 FIRST_EXCEPTION](https://docs.python.org/3/library/asyncio-task.html#asyncio.wait)
- [OWASP：原型污染](https://owasp.org/www-community/vulnerabilities/Prototype_pollution)

## 与主核 / AI 的关系

抽取这一层与主核（多模态数据库内核）的**写入路径**有一个结构上的相似点：都是「先解析外部输入，再校验，再落库」。但校验的严格度差很远：主核的输入是带类型的二进制（页格式固定、校验和可验），而这里的输入是自然语言模型输出的字符串——**格式必错**，所以校验必须同时承担「解析」与「纠错」。

**最值得对照的是 `done` 是 `set` 那条。** 主核里对应的问题是「并行 worker 的结果顺序」：主核的并行查询用 `Gather` 节点并保证输出顺序（按计划树里的编号），因为**顺序是计划的一部分**。这里的顺序一开始被当成免费（从 `done` 拿），但它通过「保序去重」隐式进入了持久化。判据很清楚：**任何进入持久化的字段，其来源集合必须有序**——若来源集合本身无序，就必须显式指定权威顺序（这里是 `tasks` 的下标）。

第二处是 `_RESERVED_ENTITY_TYPES` 与主核的**标识符保留字**。SQL 的保留字（`SELECT`/`FROM`）不能在无引号时当列名，因为它们在解析层有特殊含义。`__proto__` 在 JS 对象里有特殊含义，所以同样要拒。**共同点是「某一层的语义保留名，必须在更早的层拒绝」**——在消费层逐个防御既贵又易漏。

第三处是「三种失败原因要能区分」与主核的**错误码分层**。主核的 `E_*` 内部码映射到 `DB_E_*` 公开码，就是为了让调用方能按原因分支处理。这里 `_truncation_cause_suffix` 做的是同一件事的轻量版：把「为什么解析失败」塞进同一条消息，让运维不用跨两条日志关联。

对 AI 工程的实际意义在于：图谱抽取是 graph-RAG 相对向量 RAG 的核心增量，而它的鲁棒性几乎全在「解析 + 校验」这两步上。这份实现给出的三条经验可直接用：**多轮追问能提召回，但必须配严格校验**（否则噪音一起进来）、**并行结果必须按权威顺序收集**（set 的顺序会一路漏进持久化）、**收尾动作放 `finally` 且不许 await**（否则第三条退出路径会漏掉）。
