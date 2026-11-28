---
project: lightrag
feature: keyword-extraction
title: 查询关键词抽取
page_type: feature
schema_version: 1
refs:
  - path: lightrag/operate.py
    symbols: [get_keywords_from_query, extract_keywords_only, _parse_keywords_payload]
  - path: lightrag/operate.py
    symbols: [_normalize_keyword_list, _select_keywords_object, normalize_keyword_extraction_json_mode]
---

# 查询关键词抽取

## 一句话职责

把一句自然语言查询变成**两组关键词**——高层（`hl_keywords`，用来检索关系）与低层（`ll_keywords`，用来检索实体）——并在这个过程中消化掉一个现实：**模型返回的 JSON 有无穷多种坏形态**，而每一种都要么被修好、要么被明确判定为「没有关键词」。

## 机制是什么

**高低层分离是这套检索的核心概念，而它不是一个工程细节。**

- **低层关键词是实体名。** 它们是图谱里节点的名字，用于查出「有哪些实体和这个问题有关」。
- **高层关键词是主题概念。** 它们描述「这个问题在谈什么」，用于查出「有哪些关系和这个主题有关」。

这个分离让 `local` / `global` 两种模式有了不同的检索入口（见 e01 那页），也让同一次抽取能同时支撑两种模式。

**而抽取的工程难点不在提示，在解析。** 模型可能返回：形态正确的 JSON、被 markdown 围栏包住的 JSON、带思维链标签的文本、重复了两次的对象、数组包装、JSON 修复器才能读的残缺 JSON、或者一段 CSV 字符串。`_parse_keywords_payload` 与它的三个辅助函数构成了一整条收敛链。

## 怎么实现的

### 一、入口：预置关键词优先于抽取

**`get_keywords_from_query` 只有两个分支，而第一个可以让整次模型调用不发生。**

```
if query_param.hl_keywords or query_param.ll_keywords:
    return query_param.hl_keywords, query_param.ll_keywords
hl_keywords, ll_keywords = await extract_keywords_only(query, query_param, global_config, hashing_kv)
return hl_keywords, ll_keywords
```

**只要调用方给了任意一侧，就两侧都用调用方的** —— 注意它返回的是 `query_param` 里的**两个字段**，而不是「用给的、补抽的」。于是只提供 `hl_keywords` 会让 `ll_keywords` 也是空列表，而不是「抽一半」。

**这个形态是一个明确的契约**：预置关键词是一次**整体替换**，不是增量补充。理由是两条检索路径的语义互相依赖——高层词描述的「主题」与低层词描述的「实体」若来自不同来源（一个来自调用方、一个来自模型），检索结果会混合两种不协调的口径。

而文档字符串的说法更简单：「检查关键词是否已在查询参数中提供，若不是则用 LLM 抽取」。

### 二、`_parse_keywords_payload`：五层收敛

**这是整页最值得逐层读的函数。它的输入是 `Any`——因为不同 provider 返回的东西根本不同。**

**第 0 层：四种输入类型。**

```
if result is None: return False, [], []
if hasattr(result, "model_dump") and callable(result.model_dump): payload = result.model_dump()
elif isinstance(result, dict): payload = result
elif isinstance(result, str):
    cleaned_result = remove_think_tags(result)
    ...
else:
    logger.error("Unsupported keyword extraction response type: %s", type(result).__name__)
    return False, [], []
```

`model_dump` 那一支是给「结构化输出」用的：某些 provider（或调用方的封装）直接返回 Pydantic 对象。**而它的判定用 `hasattr` + `callable` 而不是 `isinstance`** —— 于是任何长得像 Pydantic 模型的对象都能过，不必继承特定基类。

**第 1 层：剥掉思维链标签。**

```
cleaned_result = remove_think_tags(result)
```

**必须在 JSON 解析之前做**，因为带思维链的模型会在 JSON 前后写推理过程。而顺序不能反：先解析再剥标签就不可能了（解析已经失败）。

**第 2 层：剥掉 markdown 围栏，且只在真的剥掉了时打日志。**

```
unfenced_result = strip_markdown_code_fence(cleaned_result)
if unfenced_result is not cleaned_result:
    logger.debug("Stripped markdown code fence from keyword extraction response")
    cleaned_result = unfenced_result
```

**`is not` 而不是 `!=`** —— 判的是「有没有产生新对象」而不是「内容是否相同」。若 `strip_markdown_code_fence` 在无事可做时返回原串（常见实现），`is` 能准确区分；而 `!=` 会把「有围栏但剥完一样」也算作剥过。这是一个细但正确的选择。

**第 3 层：严格 JSON 失败则走修复，而修复有两条不同的路。**

```
try:
    payload = json.loads(cleaned_result)
except json.JSONDecodeError as strict_error:
    try:
        payload = json_repair.loads(cleaned_result)
        logger.warning("Keyword extraction response required JSON repair: %s; response: %r", strict_error, cleaned_result[:500])
        if isinstance(payload, list):
            payload = _select_keywords_object(cleaned_result, payload)
        elif not isinstance(payload, dict):
            # json_repair reads '#' as a comment to the end of the line,
            # so a "Result #1: {...}" lead-in repairs to ''. Recover the
            # first object instead; keep the scalar (and so the
            # rejection below) when there is none.
            payload = tolerant_load_json_dict(cleaned_result) or payload
    except Exception as repair_error:
        logger.error("JSON parsing error: %s; repair failed: %s; response: %r", ...)
        return False, [], []
```

三点：

1. **严格解析失败是 `warning` 而不是 `error`** —— 因为它还能被修复，属于「降级但成功」。
2. **修复结果若是数组，交给 `_select_keywords_object` 挑一个。**
3. **修复结果既不是数组也不是字典时，再试 `tolerant_load_json_dict`。** 注释给了一个非常具体的成因：`json_repair` **把 `#` 当行注释**，于是 `"Result #1: {...}"` 这样的前缀会让整段修复成空字符串。这是一个只有踩过才知道的坑，而它被写进了注释。

**注意 `or payload()` 这个写法**：`tolerant_load_json_dict(cleaned_result) or payload` —— 若容错解析也拿不到字典（返回 `None` 或 `{}`），保留原来的标量，于是下面的「不是字典」拒绝照常生效。**这是一个「有更好的才替换」的保守升级**。

**第 4 层：非字典一律拒绝。**

```
if not isinstance(payload, dict):
    logger.error("Keyword extraction payload is not a JSON object: %s", type(payload).__name__)
    return False, [], []
```

**这一条把「修不出来」变成了显式的失败**，而不是返回空关键词。区别在缓存：见下面第五段。

**第 5 层：两个字段各自归一。**

```
hl_keywords = _normalize_keyword_list(payload.get("high_level_keywords"), "high_level_keywords")
ll_keywords = _normalize_keyword_list(payload.get("low_level_keywords"), "low_level_keywords")
return True, hl_keywords, ll_keywords
```

**返回三元组的第一个值是「这次解析是否有效」**，而它与「关键词是否为空」是两件事：一个合法的空清单（模型认为没有关键词）返回 `(True, [], [])`；一个坏 JSON 返回 `(False, [], [])`。**这个区分完全用于缓存决策。**

### 三、`_normalize_keyword_list`：字符串形态与列表形态

**同一个字段可能是字符串（模型返回 CSV），也可能是列表，而两种的处理必须不同。**

```
if isinstance(raw_values, str):
    raw_values = [part.strip() for part in re.split(r"[\n,;]+", raw_values) if part and part.strip()]
if not isinstance(raw_values, list):
    logger.warning("Keyword extraction field '%s' is not a list: %r", field_name, raw_values)
    return []
```

**字符串形态按换行、逗号、分号切分**；而列表形态**逐项保留**——文档字符串给的理由很关键：「多词短语合法地包含逗号时不会被拆开」。

**这就是为什么不能统一切成字符串再切分**：`["Washington, D.C."]` 若先拼成字符串再按逗号切，会变成两个无意义的词。

**而「回显的模板占位符」被专门剔除。**

```
if cleaned and not _KEYWORD_TEMPLATE_PLACEHOLDER.fullmatch(cleaned):
    normalized.append(cleaned)
```

正则：

```
_KEYWORD_TEMPLATE_PLACEHOLDER = re.compile(r"<(?:high|low)_level_keyword(?:_\d+)?>")
```

**注释解释了为什么必须剔**：「回显的模板占位符不是关键词：留着它，它会被嵌入用于检索，并为之后每次运行缓存下来」。

这是一个很具体的失败模式：提示里的示例写了 `<high_level_keyword_1>` 这样的占位符，弱模型有时会**原样回显**。若不过滤，这个字符串会被当真关键词嵌入、检索（必然无意义），而且**进入缓存**，于是以后每次同样的查询都复用它。

**而正则上方的注释说明了为什么按名字匹配而不是按形状**：

> 匹配 `<high|low>_level_keyword` 加可选数字后缀……**按名字匹配，绝不按形状：`<div>` 是 HTML 查询的合法关键词。**

**这句「绝不按形状」是一个重要的设计判断**：如果用 `<[^>]+>` 这样的通用形状，一个问 HTML 的查询会把自己的关键词 `<div>` 剔掉。**过滤规则必须窄到只杀掉自己认识的东西。**

**非字符串元素是「跳过并逐个报告」。**

```
logger.warning("Keyword extraction field '%s' contains non-string element at index %d: %r", field_name, idx, value)
```

**带下标** —— 因为问题出在模型输出，报告位置比报告值更有用（值可能很长）。

### 四、`_select_keywords_object`：从「重复的回答」里挑出真的那份

**它的存在前提是 `json_repair` 在回答重复时会返回列表。**

```
objects = [candidate for candidate in candidates if isinstance(candidate, dict)]
for candidate in objects:
    if any(_normalize_keyword_list(candidate.get(field_name), field_name)
           for field_name in ("high_level_keywords", "low_level_keywords")):
        return candidate
return objects[0] if objects else candidates
```

**三档选择，而第一档是「第一个真正带关键词的对象」。**

文档给出的场景很具体：「一个围栏里的对象、一句『让我修正我的格式』、然后是第二个对象」。前两个是坏的（第一个可能格式残缺、第二个是空示例），第三个才是答案。

**第二档：都不带关键词时取第一个对象。** 理由写在文档里——「于是全空的回答仍然是一个合法的『没有关键词』答案」。**空清单是合法答案，不是失败**，所以不能因为「一个都没找到关键词」就报错。

**第三档：完全没有对象时返回原 `candidates`（由调用方拒绝）。** 文档写明：「回答是顶层数组或根本不含对象时……一个数组元素绝不会被当作整个回答」。

**而开头的结构判定是一道必须先过的闸。**

```
opener, _ = _first_structural_opener(text)
if opener != "{":
    return candidates
```

**只有「第一个结构字符是 `{`」才认为这是一个对象型回答。** 若回答以 `[` 开头（顶层数组），直接返回候选让调用方拒绝——因为「数组里挑一个元素当整体」是猜，而猜错了会把一个错误的关键词集合缓存下来。

### 五、缓存：三处刻意的取舍

**第一，缓存命中后仍然解析，解析失败则重新抽取。**

```
cached_response, _ = cached_result
is_valid_payload, hl_keywords, ll_keywords = _parse_keywords_payload(cached_response)
if is_valid_payload:
    return hl_keywords, ll_keywords
else:
    logger.warning("Invalid cache format for keywords, proceeding with extraction")
```

**缓存里存的是原始回答而不是解析结果** —— 于是解析逻辑演进后，旧缓存能被重新解释。代价是每次命中都要重新解析（便宜），收益是**解析修复对历史缓存立即生效**。

**第二，只缓存「解析有效且未被截断」的结果。**

```
if (hl_keywords or ll_keywords) and not is_truncated_response(result):
    cache_data = {"high_level_keywords": hl_keywords, "low_level_keywords": ll_keywords}
    ...
```

两个条件：

- **`hl_keywords or ll_keywords`** —— 两侧都空就不缓。这与「空清单是合法答案」的立场看似矛盾，实际不矛盾：空清单可以作为**本次**答案返回，但不值得占据一个缓存条目（下次调用模型可能就抽出来了）。
- **`not is_truncated_response(result)`** —— 注释写得很直接：「被 token 限制截断的 LLM 回答不缓存：解析出的关键词集可能不完整，缓存它会在之后每次运行都重放那个部分结果」。

**这是最值得记的一条**：截断的响应是**部分正确**的，而部分正确的缓存会永久固化。同类判断在 `_select_keywords_object` 的「数组不猜」里也出现——**宁可不缓存，不可缓存错的**。

**第三，缓存键里包含 LLM 身份，而 `user_prompt` 只在元数据里。**

```
llm_cache_identity = get_llm_cache_identity(global_config, "keyword")
args_hash = compute_args_hash(param.mode, text, language, "\n<llm_identity>\n", serialize_llm_cache_identity(llm_cache_identity))
```

**`mode` 在键里** —— 因为不同模式的提示不同（虽然关键词抽取的提示其实相同，但 `mode` 影响了 `handle_cache` 的命名空间）。

**而 `user_prompt` 被明确排除在键外，注释解释了原因**：

> 仅元数据。关键词抽取没有 `{user_prompt}` 槽位，所以它和服务器侧前缀都不影响它，也都不属于上面那个关键词缓存键。

**这是一个精确的「按影响面选择键」的例子**：`user_prompt` 只影响最终回答的提示，不影响关键词抽取的提示，所以放进键里会让「只改用户提示」也导致关键词缓存全部失效——那是无谓的重算。

### 六、JSON 模式策略：`auto` / `json_object` / `none`

**`normalize_keyword_extraction_json_mode` 是一个严格的白名单校验。**

```
KEYWORD_EXTRACTION_JSON_MODES = ("auto", "json_object", "none")
mode = str(value).strip().lower()
if mode not in KEYWORD_EXTRACTION_JSON_MODES:
    raise ValueError("keyword_extraction_json_mode (KEYWORD_EXTRACTION_JSON_MODE) must be 'auto', 'json_object' or 'none', got {value!r}")
return mode
```

**它做三件事：`str()` 容错（可能是枚举或数字）、`strip().lower()` 归一、白名单校验。** 而错误消息同时给出参数名与**环境变量名**——因为这个值主要从环境变量来。

**`auto` 的行为是一段值得读的注释：**

> JSON 模式策略：`auto` 首次尝试保留 API 强制的 JSON 约束——受支持的 provider 绝不能被降级成仅靠提示解析，那会静默退化成空关键词——**并且只在 provider 拒绝该字段之后才去掉它**。

也就是说 `auto` 的意思是「**先硬来，被拒绝再软来**」，而不是「看情况」。理由：API 强制的 JSON 输出比提示约束可靠得多，主动降级等于自找空关键词。

**而重试的判定条件非常窄。**

```
except Exception as error:
    if not (json_mode == "auto" and llm_kwargs and _is_unsupported_response_format_error(error)):
        raise
    logger.warning("Keyword extraction: provider rejected response_format=json_object (%s); retrying once without it", error)
    result = await use_model_func(kw_prompt)
```

**必须同时满足三个条件**：`auto` 模式、确实带了 `llm_kwargs`、且异常是「provider 拒绝了这个字段」。第三个条件由 `_is_unsupported_response_format_error` 实现，而它的判定逻辑本身值得记：

```
message = str(error).lower().replace("invalid_request_error", "")
if "response_format" not in message and "json_object" not in message:
    return False
rejection_markers = ("unsupported", "not supported", "does not support", "not allowed", "not accept", "invalid", "must be", "only accept", "only support")
return any(marker in message for marker in rejection_markers)
```

**两段式判定：先要求消息里出现字段名，再要求出现拒绝标记。** 注释解释了第一句 `replace` 的必要性：「OpenAI SDK 把每个 400 的 `type` 都印成 `invalid_request_error`；它没有说明是哪个字段被拒」——若不去掉它，`invalid` 这个标记会对**所有** 400 命中。

**于是「一个通用的 400、超时、传输错误永远不会合格」**（文档原文）——只有这个兼容性失败才能在不带约束的情况下重试一次，其余必须传播。

**这是一条「重试必须精确限定范围」的范例**：无条件重试会把一次真实的配置错误重试一遍（浪费时间且掩盖问题），而条件过窄会让兼容性场景失败。判据的关键在于**用多个独立证据（字段名 + 拒绝动词）共同限定**。

### 七、提示与语言

**示例来自 `PROMPTS["keywords_extraction_examples"]`，而语言有二级回退。**

```
addon_params = global_config.get("addon_params") or {}
language = global_config.get("_resolved_summary_language")
if language is None:
    language = addon_params.get("language", DEFAULT_SUMMARY_LANGUAGE)
```

**`_resolved_summary_language` 优先**（带下划线前缀说明它是内部解析结果），回退到 `addon_params["language"]`，再回退到默认值。**三级配置**的存在说明语言在别处被解析过一次（可能与摘要语言统一），这里只是消费。

**而模型调用用了角色分离的接口。**

```
use_model_func = partial(global_config["role_llm_funcs"]["keyword"], _priority=DEFAULT_QUERY_PRIORITY)
```

**`role_llm_funcs["keyword"]` 是一个命名角色**，不是「全局那个模型」。这让关键词抽取可以配一个更便宜快、更擅长遵循 JSON 格式的模型。而 `_priority=DEFAULT_QUERY_PRIORITY` 把它标记为在线查询路径（相对离线建库有更高的调度优先级）。

## 关键取舍

**源码事实：** 高低层关键词的语义分工是「低层=实体名（检索实体库）、高层=主题概念（检索关系库）」，这个分离让 `local` / `global` 两种模式共用一次抽取。

**源码事实：** `get_keywords_from_query` 只要调用方给了任意一侧，就**两侧都用调用方的**——预置关键词是整体替换而非增量补充。

**源码事实：** 解析链五层：剥思维链标签 → 剥 markdown 围栏（用 `is not` 判「是否真的剥了」）→ 严格 `json.loads` → `json_repair` 修复 → `tolerant_load_json_dict` 兜底；非字典一律拒绝并返回无效。

**源码事实：** 注释记录了一个具体的坑：`json_repair` 把 `#` 当行注释，于是 `"Result #1: {...}"` 这类前缀会让修复结果为空串，需要 `tolerant_load_json_dict` 救回。

**源码事实：** 字符串形态的字段按换行/逗号/分号切分，列表形态逐项保留——因为多词短语可能合法地包含逗号。

**源码事实：** 回显的模板占位符（`<high_level_keyword_1>` 之类）会被剔除，否则它会被嵌入、检索并缓存，导致之后每次相同查询都复用它。匹配按**名字**而非形状，注释明确「`<div>` 是 HTML 查询的合法关键词」。

**源码事实：** `_select_keywords_object` 三档选择（第一个真正带关键词的对象 → 第一个对象 → 原样返回让调用方拒绝），且**只在本体以 `{` 开头时生效**——顶层数组不会被当成整体回答。

**源码事实：** 缓存里存的是原始回答而非解析结果，命中后重新解析；解析无效则重新抽取。于是解析逻辑的修复对历史缓存立即生效。

**源码事实：** 只缓存「解析有效、至少一侧非空、且未被 token 截断」的结果。截断的响应是部分正确的，缓存它会永久固化那个部分结果。

**源码事实：** 缓存键含 LLM 身份但不含 `user_prompt`——关键词抽取的提示没有 `{user_prompt}` 槽位，把它放进键会让「只改用户提示」导致关键词缓存全失效。

**源码事实：** `keyword_extraction_json_mode` 三值白名单，`auto` 的含义是「首次保留 API 强制的 JSON 约束，只在 provider 拒绝该字段后才降级」——主动降级会静默退化成空关键词。

**源码事实：** 降级重试的判定要求三个条件同时成立（`auto` 模式、带了 `llm_kwargs`、异常确为「拒绝该字段」），而最后一条由「消息含字段名 **且** 含拒绝动词」两段共同判定；并先把 OpenAI SDK 的 `invalid_request_error` 抹掉，否则 `invalid` 会对所有 400 命中。

**源码事实：** 模型调用走命名角色 `role_llm_funcs["keyword"]`，可配一个专门的模型；语言有「已解析值 → addon 参数 → 默认」三级回退。

**架构推论：** 这一页的复杂度几乎全在**解析**而不在**抽取**。提示是一段固定文本，而解析要面对「同一个语义任务的无穷多种表示形式」。这说明一件事：**当输出方是不可靠的外部系统时，接口的复杂度会从「生成侧」整体迁移到「解析侧」**——你在提示里要求的每一条约束，都会在解析里变成一个需要宽容处理的偏差。

**架构推论：** 三个决定共同勾勒出一种「缓存洁癖」：不缓存截断的、不缓存两侧全空的、缓存无效则重抽、数组不猜。四条规则指向同一个原则——**缓存会把一次的偶然变成永久的常态**，所以进入缓存的判断标准必须比「能返回给调用方」更严。返回给调用方错了只影响一次；缓存错了影响所有后续。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/operate.py` | `get_keywords_from_query` | 预置关键词优先的入口，整体替换语义 |
| `lightrag/operate.py` | `extract_keywords_only` | 缓存查询、提示构造、模型调用、缓存写入 |
| `lightrag/operate.py` | `_parse_keywords_payload` | 五层收敛，返回 `(是否有效, hl, ll)` |
| `lightrag/operate.py` | `_normalize_keyword_list` | 字符串按分隔符切、列表逐项保留、剔占位符 |
| `lightrag/operate.py` | `_KEYWORD_TEMPLATE_PLACEHOLDER` | 按名字匹配占位符，绝不按形状 |
| `lightrag/operate.py` | `_select_keywords_object` | 从重复回答里挑第一个真带关键词的对象 |
| `lightrag/operate.py` | `normalize_keyword_extraction_json_mode` | 三值白名单与归一 |
| `lightrag/operate.py` | `_is_unsupported_response_format_error` | 两段式判定「provider 拒绝该字段」 |
| `lightrag/operate.py` | `normalize_kg_chunk_pick_method` | 同族白名单校验（`VECTOR` / `WEIGHT`） |

## 参考

- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)
- [论文：LightRAG 双级关键词检索](https://arxiv.org/abs/2410.05779)
- [json_repair 项目](https://github.com/mangiucugna/json_repair)
- [OpenAI 文档：结构化输出](https://platform.openai.com/docs/guides/structured-outputs)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

关键词抽取与主核的**查询解析（parser）**是同一层：都把用户输入编译成执行器能用的结构。而两者的可靠性标准截然不同。

**SQL 解析器的输入是确定文法，输出是语法树，失败是语法错误。** 这里的输入是自然语言、输出是 JSON、失败有一整类「看起来对但其实错」。于是 SQL 解析器只需要一个错误码，这里需要五层收敛加一个「是否有效」的布尔。**输入的不确定性决定了接口的厚度**——这条规律在 e01（三处 `return None`）、a01（严格读 vs 宽松读）里反复出现。

**`json_repair` 那一条注释（`#` 被当行注释）在主核里对应的是「兼容旧版本文件格式」。** 两者都是「为兼容性引入的宽松解析，其宽松规则本身产生了新的失败模式」。数据库里典型的例子是宽松的日期解析——`2024-1-1` 与 `01/02/2024` 的歧义。**每一项兼容性宽容都带来一个新的、只有特定输入才触发的坑，而这类坑只能靠注释留痕。**

**「缓存键按影响面选择」在主核里对应查询计划缓存的键设计。** 计划缓存必须包含影响计划的一切（表统计信息、参数类型），且只能包含那些——多包含会让缓存无谓失效（性能问题），少包含会让缓存返回错误的计划（正确性问题）。lightrag 把 `user_prompt` 排除在关键词缓存键外、把 LLM 身份包含进去，是同一种权衡的两个方向。

对 agent 工程的直接价值有三条。其一，**提示里要求的每一条约束都会在解析里变成一份宽容代码**。与其反复加强提示（模型总会漂移），不如把解析写成「坏输入也能落地」——但坏输入必须落地成「明确的无效」，而不是「看起来正常的空结果」。其二，**缓存的标准必须比返回值更严**。截断的、两侧全空的、格式可疑的一律不缓存，因为缓存把一次偶然变成永久。其三，**重试必须精确限定范围**。用「字段名 + 拒绝动词」两个独立证据共同判定，才不会把一个真实的配置错误也重试一遍。
