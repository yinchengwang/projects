---
project: lightrag
feature: parser-routing
title: 解析路由与选项校验
page_type: feature
schema_version: 1
refs:
  - path: lightrag/parser/routing.py
    symbols: [normalize_parser_engine, ProcessOptions, sanitize_process_options]
  - path: lightrag/parser/routing.py
    symbols: [chunk_strategy_key, validate_chunk_token_sizes, ParserRoutingConfigError]
---

# 解析路由与选项校验

## 一句话职责

把「一个文件名 + 一份部署配置」解析成「用哪个引擎 + 哪组处理选项 + 各选项的参数」：文件名里的 `[hint]` 优先级最高，其次 `LIGHTRAG_PARSER` 规则，最后落到默认引擎；同时要保证**启动期的配置错误立刻报出来**（`ParserRoutingConfigError`）、**入队期的畸形 hint 立刻报出来**（`FilenameParserHintError`）、而**运行期的匹配只做尽力而为**——同一套解析逻辑，三种容错口径。

## 机制是什么

这条链路的输入端是两样都很「脏」的东西：**用户起的文件名**和**运维写的一行环境变量**。输出端是一个必须精确的决策：引擎、`i/t/e/!/F/R/V/P/C` 选项串、每个分块策略的参数。

难点在于**同一份解析代码会被三种时刻调用，而这三种时刻对错误的容忍度完全相反**：

| 时刻 | 代表函数 | 出错怎么办 | 理由 |
|---|---|---|---|
| 启动 | `validate_parser_routing_config` | **抛 `ParserRoutingConfigError`** | 配置写错了，服务不该带着错配置起来 |
| 入队 | `resolve_parser_directives` | **抛 `FilenameParserHintError`** | 新文件的名字写错了，应当立刻拒绝并告知怎么改 |
| 处理既有文档 | `resolve_stored_document_parser_engine` | **永不抛** | 已入库的文档必须能续跑，哪怕 hint 已经不合规 |

这个三口径结构解释了 `routing.py` 里为什么存在**两套几乎重复的解析代码**：`_filename_hint_match`（非抛，返回 `None` 表示「不是 hint」）与 `_validate_filename_hint_for_resolution`（抛，收集全部错误）。前者服务「扫描时给文件分组」这种批量场景，后者服务「入队时拒绝坏名字」。想合并它们会立刻撞上「批量场景不能因为一个坏名字中断整批」。

而第二层难点是**语法本身的复用**：`(mineru(page_range=1-3)-iF(chunk_ts=800))` 这样一个 hint 里，有两类括号块——引擎级参数块（在 `-` 之前）和分块策略参数块（紧跟 `F/R/V/P/C` 之后）。解析器必须先**把括号块摘掉**，让「引擎/选项」的老解析逻辑在无参数的字符串上原样运行（`_extract_param_blocks` 的 docstring 明确说这是为了 "legacy behaviour is preserved verbatim"）。

## 怎么实现的

**`normalize_parser_engine` 是所有引擎名比较的唯一入口。**

```
def normalize_parser_engine(engine: Any) -> str:
    """Normalize engine hints such as mineru-iet to mineru.

    Also strips an engine-level parameter block, so a stored/encoded
    ``parse_engine`` like ``mineru(page_range=1-3)`` resolves to the bare
    engine ``mineru`` (the single chokepoint that keeps parser selection and
    engine comparisons working once params are encoded into ``parse_engine``).
    """
    text = str(engine or "").strip()
    paren = text.find("(")
    if paren != -1:
        text = text[:paren]
    return text.split("-", 1)[0].strip().lower()
```

三个动作按顺序：

1. **截到第一个 `(` 之前** ——于是带参数的 `mineru(page_range=1-3)` 归一成 `mineru`。注释称它是 "the single chokepoint"：一旦参数被编码进 `parse_engine` 字段，**所有**「这个引擎是什么」的比较都必须经过这里，否则 `mineru(...) != "mineru"` 会让所有路由判断失效。
2. **按 `-` 切第一段** ——于是 `mineru-iet` 变成 `mineru`。这是历史遗留的方言（`-` 后面曾经是变体名，现在是选项分隔符）。
3. **小写 + 去空白**。

**这三步的顺序不能换**：若先按 `-` 切，`mineru(page_range=1-3)` 里的 `1-3` 会被 `-` 干扰（虽然此时还没截括号，`split("-",1)` 会切在 `page_range=1` 上）。先截括号后切 `-` 才对。

**`ProcessOptions` 是一个「解码视图」，且它自己承认一个字段不可靠。**

```
@dataclass(frozen=True)
class ProcessOptions:
    """Decoded view of a ``process_options`` string.

    The ``raw`` string is preserved verbatim (with duplicates and ordering)
    for storage / audit purposes; boolean flags reflect the deduped logical
    state used by the pipeline.
    """

    raw: str = ""
    images: bool = False
    tables: bool = False
    equations: bool = False
    skip_kg: bool = False
    chunking: ProcessChunkingOption = PROCESS_OPTION_CHUNK_FIXED
```

**两个表示并存：`raw` 原样保留，五个字段是解码结果。** 为什么两者都要？因为：

- `raw` 要落盘（`full_docs` 里有 `process_options`），必须保留用户原样输入以便审计与再现；去重会掩盖「用户写了两次 `i`」这种事实。
- 五个字段服务运行期判断，需要的是**逻辑状态**——`images=True` 比「字符串里有没有 `i`」更直接。

但 `chunking` 这个字段有一个诚实标注的缺陷：

```
@property
def chunking_explicit(self) -> bool:
    """True iff ``raw`` actually contains a chunking selector char.

    Distinguishes "user explicitly opted into a chunking strategy"
    from "no chunking selector supplied — pipeline used the default".
    ``chunking`` itself is unreliable for this question because it
    falls back to :data:`PROCESS_OPTION_CHUNK_FIXED` in both cases.
    Used by ``process_single_document`` to decide whether to
    dispatch via the new file-chunker contract or to honor the
    legacy externally-supplied :attr:`LightRAG.chunking_func`.
    """
    return any(c in PROCESS_OPTION_CHUNK_CHARS for c in self.raw)
```

**「默认值是 `F`，但 `F` 也可能是用户明确选的」这个歧义，导致 `chunking` 无法回答「用户有没有指定」。** 解法是加一个**能区分意图**的派生属性：只有 `raw` 里真的出现了分块字符，才算「显式指定」。

这个区分有实际后果——它决定了走哪条分块路径：

- `chunking_explicit == False` → 保留旧的 `LightRAG.chunking_func` 外部注入路径（向后兼容）。
- `chunking_explicit == True` → 走新的 file-chunker 契约。

**这是一个非常好的模式：当一个字段的「值」无法表达「意图」时，额外暴露一个只回答意图的判据**，而不是让每个调用方自己去 `raw` 里找字符。

**`sanitize_process_options` 与 `validate_process_options` 的分工是「净化 vs 报错」。**

```
def sanitize_process_options(options: Any) -> str:
    """Strip non-supported characters / hyphen / whitespace from an options string.

    Returns the raw token sequence as-is (no dedup, no reorder) so the
    canonical user intent is preserved on disk.  Invalid characters are
    silently dropped — the caller is expected to have already validated.
    """
    if not options:
        return ""
    return "".join(ch for ch in str(options) if ch in SUPPORTED_PROCESS_OPTIONS)
```

**注意它的 docstring 里有一句与实际不符的话**：「Strip non-supported characters / hyphen / whitespace」——而实现只是过滤掉不在 `SUPPORTED_PROCESS_OPTIONS` 里的字符。效果上空格与连字符确实被去掉了（它们不在支持集合里），但没有专门处理它们的逻辑。实际行为是对的，描述的实现路径不精确。

**「静默丢弃非法字符」这个选择很关键**：注释说 "the caller is expected to have already validated"。于是：

- 校验通过时，`sanitize` 是恒等变换（幂等）。
- 校验被跳过时，`sanitize` 起到兜底作用，但**丢弃不报错**。

把一个「净化」函数与一个「校验」函数分开，是这个模块的整体风格：**净化的语义是「尽力留下能用的」，校验的语义是「报告所有不对的」**，两者各自单一职责。

`validate_process_options` 则做两件事：

```
def validate_process_options(options: str, *, label: str = "process options") -> list[str]:
    """Return a list of error messages for an options string; empty if valid."""
    errors: list[str] = []
    if not options:
        return errors
    seen_chunkers: list[str] = []
    for ch in options:
        if ch in (" ", "-"):
            continue
        if ch not in SUPPORTED_PROCESS_OPTIONS:
            errors.append(f"{label} contains unsupported character {ch!r}")
            continue
        if ch in PROCESS_OPTION_CHUNK_CHARS and ch not in seen_chunkers:
            seen_chunkers.append(ch)
    if len(seen_chunkers) > 1:
        errors.append(
            f"{label} specifies multiple chunking modes "
            f"({'/'.join(seen_chunkers)}); pick one of "
            f"{PROCESS_OPTION_CHUNK_FIXED}/{PROCESS_OPTION_CHUNK_RECURSIVE}/"
            f"{PROCESS_OPTION_CHUNK_VECTOR}/{PROCESS_OPTION_CHUNK_PARAGRAH}/{PROCESS_OPTION_CHUNK_CUSTOM}"
        )
    return errors
```

三处：

1. **它返回错误列表而不抛** ——于是调用方可以一次性收集多条错误（`_validate_filename_hint_for_resolution` 正是这么用的：把所有问题拼进一个 `FilenameParserHintError`）。这比「遇到第一个错误就抛」体验好得多。
2. **空格与 `-` 被显式跳过** ——它们是分隔符，不是选项。
3. **分块策略必须恰好一个（或不选）** ——`seen_chunkers` 去重后若多于一个就报错，消息里列出**全部合法选项**。这是「五选一」这种约束的正确报错方式。

**`parse_process_options` 用「第一个分块字符」定策略，注释说明了依据。**

```
chars = set(raw)
chunking: ProcessChunkingOption = PROCESS_OPTION_CHUNK_FIXED
# Pick the first chunking selector encountered; validate_process_options
# already filters duplicates upstream.
for ch in raw:
    if ch in PROCESS_OPTION_CHUNK_CHARS:
        chunking = ch
        break
```

**「取第一个」而不是「取最后一个」或「报错」**，依据是「上游校验已经挡住了多选」。也就是说这个函数的正确性**依赖调用顺序**：必须先 `validate` 再 `parse`。若有人跳过校验直接 parse，多个策略会出现「谁先出现谁生效」这种静默行为。

**`chunk_strategy_key` 把策略字符映射到 `chunk_options` 的子字典键。**

```
_CHUNK_STRATEGY_KEYS: dict[str, str] = {
    PROCESS_OPTION_CHUNK_FIXED: "fixed_token",
    PROCESS_OPTION_CHUNK_RECURSIVE: "recursive_character",
    PROCESS_OPTION_CHUNK_VECTOR: "semantic_vector",
    PROCESS_OPTION_CHUNK_PARAGRAH: "paragraph_semantic",
    # C deliberately reuses the fixed-token snapshot: those values map
    # one-for-one to the six-argument legacy chunking_func contract.
    PROCESS_OPTION_CHUNK_CUSTOM: "fixed_token",
}

def chunk_strategy_key(process_options: Any) -> str:
    """Return the ``chunk_options`` sub-dict key for ``process_options``.

    Accepts a raw options string or a :class:`ProcessOptions` value.
    Falls back to ``"fixed_token"`` when no chunking selector is
    present ...
    """
    if isinstance(process_options, ProcessOptions):
        strategy = process_options.chunking
    else:
        strategy = parse_process_options(process_options).chunking
    return _CHUNK_STRATEGY_KEYS.get(strategy, "fixed_token")
```

三个设计点：

1. **`C` 与 `F` 共用 `"fixed_token"` 子字典**，注释给了理由：自定义分块器的六个参数与 `F` 的参数一一对应（都是 `chunk_token_size` / `overlap` 那套）。**「两个策略共用一个参数形状」用映射表表达比用 `if` 表达更清楚**。
2. **接受两种输入**（原始字符串或已解析的 `ProcessOptions`），于是调用方不必先决定用哪种形态。
3. **两处兜底都落到 `"fixed_token"`**：`.get(strategy, "fixed_token")` 与「无分块选择器时 `ProcessOptions.chunking` 的默认值」。两处兜底方向一致，不会出现「一个函数认为是 F、另一个认为没有」的分歧。

**`validate_chunk_token_sizes` 是「入口处校验、出口处不校验」的典型。**

```
def validate_chunk_token_sizes(chunk_config: Mapping[str, Any] | None) -> None:
    """Reject any ``chunk_token_size`` below :data:`MIN_CHUNK_TOKEN_SIZE`.

    Checks the top-level slot and every strategy sub-dict present in
    ``chunk_config`` (a full ``addon_params['chunker']`` dict or a slim
    per-document snapshot). Absent and ``None`` slots are skipped: they
    inherit a value that is checked where it is set.

    Call it where configuration ENTERS the system (``LightRAG`` init, the
    ``addon_params`` setter, enqueue). Do not call it when processing an
    already persisted snapshot: a document accepted under an older rule
    must stay processable on retry.

    Raises:
        ValueError: naming the offending slot and value.
    """
    if not chunk_config:
        return
    slots: list[tuple[str, Any]] = [("chunker", chunk_config)]
    for key in dict.fromkeys(_CHUNK_STRATEGY_KEYS.values()):
        sub = chunk_config.get(key)
        if isinstance(sub, Mapping):
            slots.append((f"chunker.{key}", sub))
    for label, slot in slots:
        value = slot.get("chunk_token_size")
        if value is None:
            continue
        if int(value) < MIN_CHUNK_TOKEN_SIZE:
            raise ValueError(
                f"{label}.chunk_token_size must be >= {MIN_CHUNK_TOKEN_SIZE}, "
                f"got {value!r}; a chunk this small is a misconfiguration "
                "(check CHUNK_SIZE / CHUNK_F_SIZE / CHUNK_R_SIZE / "
                "CHUNK_V_SIZE / CHUNK_P_SIZE, LightRAG(chunk_token_size=...) "
                "and addon_params['chunker'])"
            )
```

四处值得逐个看：

**第一，`dict.fromkeys(_CHUNK_STRATEGY_KEYS.values())` 是对映射值去重后遍历。** 因为 `F` 与 `C` 都映射到 `"fixed_token"`，不去重就会检查同一个子字典两次。**用 `fromkeys` 而不是 `set()`** 还保住了顺序（`dict` 有序），于是报错顺序稳定。

**第二，`None` 槽被跳过，理由是「它们继承一个在设置处已被检查的值」。** 这是一条很精确的边界：`None` 表示「没配，用默认」，而默认值本身在定义处已经合规（`DEFAULT_CHUNK_P_SIZE = 2000`）。检查 `None` 会把「没配」误判成「配了 0」。

**第三，错误消息同时给出**：合法下界、实际值、**所有可能设这个值的入口**（五个环境变量 + 构造函数参数 + `addon_params` 键）。这是我在这些页面里见过的最完整的配置错误消息——它回答的不是「哪里错了」，而是「你从哪个入口设的」。

**第四，`MIN_CHUNK_TOKEN_SIZE = 64` 的注释解释了为什么需要这个下界**：

> A chunk this small carries no retrievable context and multiplies the extraction LLM calls, so a smaller value is treated as a misconfiguration and rejected rather than run. Chunker functions called directly only reject values below 1, so tests and SDK callers can still use tiny sizes.

**两套下界并存**：入口处 `>= 64`，直接调用分块器 `>= 1`。理由很实在——**SDK 与测试需要小值来构造可预测的用例，而生产入口的小值是纯粹的配置错误**。这是一条「校验强度按入口分级」的样本。

**`slim_chunk_options` 是「裁剪快照」的唯一瓶颈点。**

```
def slim_chunk_options(snapshot, process_options="") -> dict[str, Any]:
    """Project a (possibly full) chunker snapshot down to the active strategy.

    Keeps the top-level ``chunk_token_size`` and the one strategy
    sub-dict picked by :func:`chunk_strategy_key`; everything else is
    discarded.  Idempotent: a slim snapshot whose key already matches
    ``process_options`` passes through unchanged (deep-copied for
    isolation).  When the matching strategy block is absent from the
    input, an empty dict is used so downstream consumers always see a
    dict-shaped slot.
    """
```

存储形状的设计在文件里的注释里写得很明确：

> Storage shape: the per-document snapshot persisted to ``full_docs[doc_id]['chunk_options']`` carries ONLY the sub-dict of the chunking strategy selected by ``process_options`` — the other strategies' parameters are dropped because they are never consumed during processing. Reparsing a document overwrites both ``process_options`` and ``chunk_options`` together.

**「只存被选中策略的参数」这个决定很重要**：它让「一份文档的分块参数」与「它实际被怎么分块」绑定成一对，避免「存了五套参数，实际只用了其中一套，重跑时不知道该用哪套」。代价是**丢弃了其他策略的配置**——若之后想把策略从 R 改成 P，P 的参数已经没了，只能回到全局配置。

**`slim_chunk_options` 里有一处策略专属的默认回填，注释解释了为什么必须在这里做。**

```
if key == "paragraph_semantic":
    if "chunk_token_size" not in result[key]:
        result[key]["chunk_token_size"] = _chunk_env_int("CHUNK_P_SIZE", DEFAULT_CHUNK_P_SIZE)
    if os.getenv("CHUNK_P_DROP_REFERENCES") is not None:
        result[key].setdefault("drop_references", _env_bool("CHUNK_P_DROP_REFERENCES"))
```

理由（注释原文）：

> This is the single chokepoint that every enqueue path runs through — both the ``resolve_chunk_options`` path (built from addon_params) AND the direct ``chunk_options=`` kwarg path (caller supplies the dict) flow through here, so the backfill cannot be bypassed by runtime addon_params mutation or by passing an explicit ``chunk_options`` that omits the P slot. P must NOT inherit the top-level ``chunk_token_size`` (global ``CHUNK_SIZE`` / legacy ctor) — paragraph-semantic merging needs more headroom than the global default.

**两条约束的冲突在这里解决**：

1. 「所有入队路径都必须经过一个瓶颈点」——否则总有一条路能绕过默认值回填。
2. 「P 不能继承顶层 `chunk_token_size`」——因为段落语义合并需要比全局默认（1200）更大的空间，所以 `DEFAULT_CHUNK_P_SIZE = 2000`。

把回填放在瓶颈点、并对 P 特殊处理，同时满足了这两条。**`setdefault` 而非赋值**保证了「调用方显式给的值优先」。

**文件名 hint 的两种解析：非抛的 `_filename_hint_match` 与抛的 `_validate_filename_hint_for_resolution`。**

非抛版本的关键在于它**返回「是不是 hint」而不是「hint 对不对」**：

```
def _filename_hint_match(file_path):
    """Locate a supported ``[hint]`` segment in a basename.

    ...
    This low-level helper stays non-throwing because scan grouping and
    basename canonicalization need a best-effort classifier.  Ingestion
    entrypoints must call :func:`resolve_parser_directives`, which validates
    malformed hints and raises instead of falling back.
    """
```

它对每个失败点都返回 `None`，且**每个返回 `None` 之前都打一条 warning**（除了两个明显的形态判断）：

- 括号不配对 → `struct_errors` → warning + `None`。
- 选项串非法 → warning + `None`。
- 参数块解析失败 → warning + `None`。
- 引擎名不认识**且没有选项** → 直接 `None`，不打 warning。

最后一条有个精细的判断：

```
if engine in supported_parser_engines():
    return m, engine, options, chunk_params, engine_params
# Without a recognised engine only the ``-OPTIONS`` form is a hint, and
# ``split_engine_and_options`` returns options for nothing else. A chunk
# parameter block alone must not qualify: ``xyz-R(...)`` stays
# unrecognised just like ``xyz-R``.
if engine is None and options:
    return m, "", options, chunk_params, engine_params
return None
```

**「`xyz-R(...)` 和不带参数的 `xyz-R` 一样不被识别」** ——这个一致性很重要：若带参数块就能让它变成合法 hint，那么「加一个参数块」会意外地把一个无效引擎名变成一个有效 hint，行为极难预测。

抛错版本 `_validate_filename_hint_for_resolution` 则是**收集全部错误再一次抛出**，且有一处专门优化错误消息：

```
else:
    engine = normalize_parser_engine(inner)
    if engine not in supported_parser_engines():
        supported = ", ".join(sorted(supported_parser_engines()))
        message = (f"filename hint {m.group(0)!r} uses unsupported parser engine "
                   f"{inner.strip()!r}; supported engines: {supported}")
        if all(ch in SUPPORTED_PROCESS_OPTIONS or ch == " " for ch in inner):
            message += ("; options-only filename hints must start with '-' "
                        f"(use '[-{inner.strip()}]' instead)")
        errors.append(message)
```

**「用户写了 `[i]` 想表示『只要图片』，但没加前导 `-`，于是它被当成引擎名」** ——这是这个语法最容易犯的错。检测方法是「整个 inner 都由合法选项字符组成」，命中就给出一条**带具体修正写法**的消息（`use '[-i]' instead`）。

**这条消息是全页最值得学的一处**：它把「一个语法约定的坑」变成了「一条自解释的修法」。没有它，用户看到的是「不支持的引擎 `i`」，而 supported engines 列表里当然没有 `i`——他会以为自己要的是某个引擎名。

**规则（`LIGHTRAG_PARSER`）的解析与校验各有一套切分器。**

规则格式是 `<suffix-pattern>:<engine>[-options]`，合法例子 `pdf:mineru(page_range=1-3)FF(chunk_ts=800)`。三个切分点：

```
def _iter_parser_rule_items(rules: str) -> list[tuple[int, str]]:
    # Parenthesis-aware split: ';' (preferred) or ',' (legacy) separate rules
    # at paren depth 0, so commas inside a chunk-parameter block such as
    # ``R(chunk_ts=800,chunk_ol=80)`` never split the surrounding rule.
    return [(index, item.strip())
            for index, item in enumerate(split_top_level(rules, ";,"), start=1)
            if item.strip()]
```

**「括号感知的切分」是必须的**：规则里允许写 `R(chunk_ts=800,chunk_ol=80)`，那个逗号是参数分隔符而不是规则分隔符。若用 `rules.split(";")` 与 `rules.split(",")` 直接切，参数块会被从中间劈开。

而 `_rule_engine_and_options` 与 `split_engine_and_options` 的差异也有注释说明：

```
def _rule_engine_and_options(engine_hint: str) -> tuple[str, str]:
    """Split a ``LIGHTRAG_PARSER`` rule's RHS (``engine[-options]``).

    Returns ``(normalized_engine, options_str)``.  Unlike the filename hint
    splitter this always treats the first ``-`` as the engine/options
    boundary, since ``LIGHTRAG_PARSER`` rules cannot be options-only.
    """
    head, _, tail = engine_hint.partition("-")
    return normalize_parser_engine(head), tail.strip()
```

**「规则不能只有选项」这个限制让切分简化成「第一个 `-` 就是边界」**，不需要像文件名 hint 那样检查「引擎名是否在支持集合里」来消歧。而 `_filename_hint_match` 里 `split_engine_and_options` 的两次 `supported_parser_engines()` 查询，正是因为 hint 允许 `-OPTIONS` 形态。

**启动校验把「后缀扩展」排在「规则校验」之前，注释解释了顺序。**

```
def validate_parser_routing_config(parser_rules: str | None = None) -> None:
    """Validate LIGHTRAG_PARSER syntax and required external parser endpoints.

    Deployment suffix extensions are checked first: they feed engine capability
    (``suffix_capabilities``), so a malformed list must be reported as such
    rather than as a downstream "rule does not match any supported suffix".
    """
    validate_parser_suffix_env_vars()
    ...
```

**顺序理由很具体**：规则校验会检查「这条规则的后缀模式能不能匹配到该引擎的某个后缀」（`_rule_pattern_matches_engine_capability`）。而引擎的后缀集合受环境扩展影响。若扩展列表本身有错，先校验规则就会得到一条**误导性的**「你的规则不匹配任何后缀」——真正的原因在别处。

**每条规则会收集多类错误，且用统一的 `rule N (...)` 前缀。**

```
label = f"rule {index} ({item!r})"
if ":" not in item:
    errors.append(f"{label} must use '<suffix-pattern>:<engine>'")
    continue
...
if not pattern:
    errors.append(f"{label} has an empty suffix pattern")
    continue
if "." in pattern:
    errors.append(f"{label} matches suffixes without dots; use 'pdf', not '*.pdf'")
    continue
...
if not _rule_pattern_matches_engine_capability(pattern, engine):
    supported_suffixes = ", ".join(sorted(suffix_capabilities(engine)))
    errors.append(f"{label} does not match any suffix supported by {engine}; supported suffixes: {supported_suffixes}")
endpoint_req = parser_engine_endpoint_requirement(engine)
if endpoint_req and not parser_engine_endpoint_configured(engine):
    errors.append(f"{label} requires {endpoint_req} to be configured")
```

三类错误消息的共同点是**都给出正确写法**：

- 缺冒号 → 给出完整格式 `<suffix-pattern>:<engine>`。
- 带点 → 明确说「匹配不带点的后缀；写 `pdf`，不是 `*.pdf`」（glob 习惯是最高频的错）。
- 模式不匹配 → 列出该引擎实际支持的后缀。
- endpoint 未配 → 说出**具体缺哪个变量**（`parser_engine_endpoint_requirement` 返回变量名，b02 里那对闭包）。

最后一类的实现值得复用：错误消息的内容由被校验对象自己提供（引擎知道该配什么），而不是校验器硬编码一张表。

**最后一点：`resolve_parser_directives` 的优先级与「参数按引擎归属过滤」。**

```
engine = hinted_engine or rule_engine or default_engine or PARSER_ENGINE_LEGACY
options_str = hinted_options or rule_options
```

四级回退：**文件名 hint → 规则 → 按后缀的默认引擎（目前只有 `textpack`）→ `legacy`**。而选项串只有两级（hint → rule），因为「默认引擎」与 `legacy` 都没有选项可继承。

引擎参数的处理更细：

```
# Engine params are flat (one resolved engine per file): keep only the
# params whose attached engine == the resolved engine — rule params first,
# then filename-hint params (filename wins on a shared key); params
# attached to a losing engine are dropped.
engine_params: dict[str, Any] = {}
if rule_engine == engine:
    engine_params.update(rule_engine_params)
if hinted_engine_for_params == engine:
    engine_params.update(hinted_engine_params)
seed_smart_heading_param(engine, engine_params, file_path)
```

**「参数跟着引擎归属走」这条规则是必要的**：若 hint 说 `mineru(page_range=1-3)` 但最终引擎因为某种原因落到了 `legacy`，那么 `page_range` 必须被丢掉——否则它会被写进 `parse_engine` 字段，然后被 legacy 引擎拿到并（正确地）拒绝，或者更糟，被误传给别的引擎。

而 `hinted_engine_for_params` 这个变量必须在「可用性判定之前」捕获，注释说明了原因：

```
# The engine a filename hint's params belong to (captured before the
# usability null-out below); engine params are dropped if this engine does
# not win resolution.  Note: under strict validation the null-out is
# effectively unreachable (the validator raises first), so this normally
# equals the resolved engine when a hint engine is present.
hinted_engine_for_params = hinted_engine
if hinted_engine and not _engine_is_usable(...):
    hinted_engine = None
```

**这里有一处「两条路径的不一致」被注释明确承认了**：严格校验（入队路径）会先抛错，所以那个 null-out 走不到；但宽松路径（处理既有文档）会走到，那时「hint 说的引擎不可用」但「它的参数还在」——必须靠 `== engine` 的比较把它们丢掉。注释把「正常情况下这个变量恰等于解析出的引擎」写出来，等于告诉读者「别指望它总是不同」。

## 关键取舍

**源码事实：** 同一套解析逻辑有三种容错口径：启动（`validate_parser_routing_config` 抛 `ParserRoutingConfigError`）、入队（`resolve_parser_directives` 抛 `FilenameParserHintError`）、处理既有文档（永不抛）。因此存在两套 hint 解析代码：非抛的 `_filename_hint_match`（批量分组用，失败打 warning 返回 `None`）与抛的 `_validate_filename_hint_for_resolution`（收集全部错误后一次抛出）。

**源码事实：** `normalize_parser_engine` 按「先截 `(` 之前 → 再按 `-` 切第一段 → 小写去空白」的顺序归一，是整个模块里引擎名比较的唯一入口（注释自称 "single chokepoint"）；顺序不可换，否则参数块里的 `1-3` 会干扰。

**源码事实：** `ProcessOptions` 同时保留 `raw`（原样，供审计/落盘）与五个解码字段；因为 `chunking` 在「用户选了 F」与「用户没选」两种情况下都等于 `F`，另设派生属性 `chunking_explicit` 专门回答「用户是否显式指定」，该判据决定走新 file-chunker 契约还是旧的 `chunking_func` 路径。

**源码事实：** `sanitize_process_options` 静默丢弃不支持字符（注释：调用方应已校验过），`validate_process_options` 返回错误列表而不抛，且要求分块策略五选一（去重后 >1 即报错，消息列出全部合法选项）；`parse_process_options` 取**第一个**分块字符，其正确性依赖上游已校验。

**源码事实：** `_CHUNK_STRATEGY_KEYS` 把 `C` 也映射到 `"fixed_token"`（注释：自定义分块器的参数与 `F` 一一对应）；`chunk_strategy_key` 接受字符串或 `ProcessOptions`，两处兜底都落到 `"fixed_token"`。

**源码事实：** 落盘的 `chunk_options` 只保存被选中策略的子字典（其他策略参数被丢弃），重解析时与 `process_options` 一起覆盖；`slim_chunk_options` 是所有入队路径的唯一瓶颈点，并对 `paragraph_semantic` 强制回填 `chunk_token_size`（`CHUNK_P_SIZE` 或 `DEFAULT_CHUNK_P_SIZE`），**不允许它继承顶层 `chunk_token_size`**，因为段落语义合并需要更大空间。

**源码事实：** `validate_chunk_token_sizes` 检查顶层与每个策略子字典的 `chunk_token_size`（用 `dict.fromkeys(...values())` 去重且保序），跳过 `None` 槽（继承值在设置处已检查），错误消息列出**全部可能设置它的入口**（五个环境变量 + 构造器参数 + `addon_params` 键）。下界 `MIN_CHUNK_TOKEN_SIZE = 64` 只在入口生效，直接调用分块器只拒绝 `< 1`，以便测试与 SDK 使用小值。

**源码事实：** 规则按 `<pattern>:<engine>[-options]` 解析，用括号感知的 `split_top_level(rules, ";,")` 切分（避免参数块里的逗号劈开规则）；`_rule_engine_and_options` 与文件名 hint 的 `split_engine_and_options` 不同，直接把第一个 `-` 当边界，因为规则不允许「只有选项」。

**源码事实：** `validate_parser_routing_config` 先校验后缀环境变量再校验规则，因为扩展列表会影响引擎能力，顺序反了会把「扩展列表格式错」报成「规则不匹配任何后缀」。

**源码事实：** 引擎参数只保留「归属引擎 == 最终解析引擎」的那些（规则参数先、hint 参数后、同名 hint 优先），`hinted_engine_for_params` 在可用性判定**之前**捕获；注释承认严格校验路径下「hint 引擎被置空」不可达，但宽松路径可达。

**架构推论：** 「同一逻辑三种容错口径」是这个模块体积的主要来源，也是它正确的关键。批量分组不能因一个坏名字中断，入队必须立刻拒绝坏名字，续跑必须允许已入库的不合规文档。**如果一个解析器同时被这三种场景调用，它就不该只有一种错误策略**——更不该把三种策略混在一个函数里用参数切换（那会让每条调用路径都携带另外两条的假设）。这里的做法是分成两个函数、由调用点选，代价是约一百五十行的重复。

**架构推论：** `chunking_explicit` 是一个值得抄的模式：**当某个字段的值无法表达意图时，不要改字段语义，而是加一个只回答意图的判据**。替代方案（把 `chunking` 改成 `Optional`）会破坏所有按默认值比较的代码，而新增一个只读属性不影响任何现有调用方。

**架构推论：** 校验强度按入口分级（入口 `>= 64`、直接调用 `>= 1`）是把「生产正确性」与「测试可构造性」分开的标准做法。主核里同样会遇到：配置入口应当拒绝荒谬值，而库函数不该替调用方定策略。关键是**两个下界都要写下来并说明各自服务谁**，否则后来者会「统一」成一个。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/parser/routing.py` | `normalize_parser_engine` | 引擎名归一的唯一入口：截参数块 → 切 `-` → 小写 |
| `lightrag/parser/routing.py` | `encode_parse_engine` / `decode_parse_engine` | `(engine, params)` 与 `engine(key=value)` 字符串的双向编解码 |
| `lightrag/parser/routing.py` | `ProcessOptions` | 选项解码视图：`raw` 原样 + 五个逻辑字段 |
| `lightrag/parser/routing.py` | `ProcessOptions.chunking_explicit` | 「用户是否显式指定分块策略」的意图判据 |
| `lightrag/parser/routing.py` | `sanitize_process_options` | 静默过滤非法字符（调用方应已校验） |
| `lightrag/parser/routing.py` | `validate_process_options` | 返回错误列表；分块策略五选一 |
| `lightrag/parser/routing.py` | `parse_process_options` | 取第一个分块字符（依赖上游已校验） |
| `lightrag/parser/routing.py` | `_CHUNK_STRATEGY_KEYS` / `chunk_strategy_key` | 策略字符 → `chunk_options` 子键（`C` 复用 `fixed_token`） |
| `lightrag/parser/routing.py` | `slim_chunk_options` | 唯一瓶颈点：裁剪到选中策略 + `P` 专属默认回填 |
| `lightrag/parser/routing.py` | `validate_chunk_token_sizes` | 入口处 `>= 64` 校验，错误消息列出全部设置入口 |
| `lightrag/parser/routing.py` | `_extract_param_blocks` | 括号感知的块摘除：引擎级 vs 分块策略级，保留旧解析行为 |
| `lightrag/parser/routing.py` | `_filename_hint_match` / `_validate_filename_hint_for_resolution` | 非抛（批量分组）与抛（入队校验）两套 hint 解析 |
| `lightrag/parser/routing.py` | `split_engine_and_options` / `_rule_engine_and_options` | hint 与规则的引擎/选项切分（规则不允许只有选项） |
| `lightrag/parser/routing.py` | `validate_parser_routing_config` | 启动校验：先后缀扩展再规则，每条规则收集多类错误 |
| `lightrag/parser/routing.py` | `resolve_parser_directives` | 四级回退 + 参数按引擎归属过滤 + smart_heading 种子 |
| `lightrag/parser/routing.py` | `ParserRoutingConfigError` / `FilenameParserHintError` | 启动错与入队错的两种异常 |

## 参考

- [lightrag 源码：parser/routing.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/parser/routing.py)
- [lightrag 源码：constants.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/constants.py)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [Python 文档：`fnmatch`](https://docs.python.org/3/library/fnmatch.html)

## 与主核 / AI 的关系

这条链路对应主核里最接近的东西是**存储参数与执行选项的解析**（`SET` 语句、连接串参数、文件格式探测）。四条经验。

**第一，同一个解析器被多种时刻调用时，容错口径必须分开写。** 这里三口径（启动抛 / 入队抛 / 续跑不抛）导致了两套 hint 解析代码。主核里同理：一个「解析表选项」的函数会被 `CREATE TABLE`（该严格）、`ALTER TABLE`（该严格）、以及「打开既有表」（必须宽容，否则旧表打不开）调用。**合并成一个函数再用参数切换，是最容易出隐性 bug 的做法**——因为每条调用路径都携带着另外两条的假设。

**第二，语法越宽松，错误消息越要教人写对。** 这里四处错误消息都内嵌正确写法：`<suffix-pattern>:<engine>`、`use 'pdf', not '*.pdf'`、列出引擎实际支持的后缀、`use '[-i]' instead`。最后一条尤其值得抄——它识别出「用户把选项串当引擎名」这个具体误解，并给出改后的完整字符串。**判断标准：用户看到消息后，能否不做任何额外查询就改对。**

**第三，「默认值」与「用户显式选了默认值」必须能区分，否则下游分不清该走兼容路径还是新路径。** `chunking_explicit` 用「`raw` 里有没有那个字符」回答这个问题。主核里类似场景很多：`WITH (fillfactor=100)`（用户显式写了默认值）与「没写」在需要兼容旧行为时是不同的。**用值判断永远分不清，必须用「原文是否存在」判断。**

**第四，配置扩展会影响能力判定，因此校验顺序有语义。** 这里必须先校验后缀环境变量再校验规则，否则会得到误导性错误。主核里任何「配置 A 扩展了能力，配置 B 引用了能力」的场景都有同样的顺序约束：**先校验扩张方，再校验引用方**；顺序反了，用户会去改一份本来正确的配置。
