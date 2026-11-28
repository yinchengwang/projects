---
project: lightrag
feature: metrics-and-prompts
title: 调度指标与提示词配置
page_type: feature
schema_version: 1
refs:
  - path: lightrag/pipeline_metrics.py
    symbols: [increment, observe, snapshot]
  - path: lightrag/prompt.py
    symbols: [EntityExtractionPromptProfile, resolve_entity_extraction_prompt_profile, resolve_user_prompt_prefix_path]
---

# 调度指标与提示词配置

## 一句话职责

两个都面向「运维/使用者可调」的表面：`pipeline_metrics.py`（153 行）用**固定键集的内存计数器**让「分页扫描是在正常分页还是在抖动」可判断，代价是放弃跨 worker 汇总；`prompt.py` 让运维通过环境变量替换抽取提示词的类型指引与示例，代价是一套**文件名沙箱**，因为那些值是用户可控的。

## 机制是什么

**指标模块的核心决定是「固定键集」，而它的理由直接指向它所服务的重构。**

模块 docstring 说清了它为什么存在：

> 有界内存改造把若干决策移出了快路径——一次分页扫描、每次准入一次严格活跃计数、一次排空后重置的冻结——而每一个都可能变慢或有噪声而没有任何东西可见地出错。这些计数器与时长汇总是区分「扫描在分页」与「扫描在抖动」、或「一次冻结是正常的」与「一次冻结卡住了」的**唯一**方式。

所以它不是「通用指标库」，而是**针对三类具体问题的诊断工具**。而它的第一条设计约束就是反向的：

> **固定键集。** 每个指标名都在下面声明。一个未知名字被记录一次并丢弃而不是插入——**一个随基数增长的指标注册表正是整个计划要消除的那种无界结构**。

这句话很有意思：它说这个指标模块自己也要遵守它测量的那个约束。所以「不引入无界结构」不只是被测量的目标，也是工具本身的纪律。

**提示词模块的核心决定是「分层的覆盖」与「沙箱」。**

覆盖分三层，优先级从高到低：

1. `addon_params["entity_types_guidance"]`（代码传入）
2. `ENTITY_TYPE_PROMPT_FILE` 指定的 YAML 文件
3. 内置的 `PROMPTS["default_entity_types_guidance"]`

而沙箱是因为第 2 层与 `USER_PROMPT_PREFIX_FILE` 的值来自环境变量——**用户可控**，所以文件名必须被限制在 `PROMPT_DIR/<subdir>` 之下。

## 怎么实现的

**指标模块用两个元组 + 两个 dict，而「声明」与「存储」是分开的。**

```python
_COUNTERS: tuple[str, ...] = (FREEZE_REJECTS, AUTO_RESCAN_REARMS, MANUAL_RESET_DOCS, MANUAL_RESET_PAGES)
_DURATIONS: tuple[str, ...] = (SCHEDULING_PAGE_SECONDS, ACTIVE_COUNT_SECONDS, MANUAL_DRAIN_SECONDS, MANUAL_RESET_SECONDS)

_lock = threading.Lock()
_counters: Dict[str, int] = {name: 0 for name in _COUNTERS}
_durations: Dict[str, Dict[str, float]] = {name: {...} for name in _DURATIONS}
```

元组是**声明**（允许哪些名字），dict 是**存储**。两者都从元组派生，所以它们不可能不同步。而 `increment` / `observe` 的守卫就是「查 dict 的键」：

```python
if name not in _counters:
    logger.warning(f"Ignoring unknown pipeline metric counter {name!r}")
    return
```

所以「未知名字被记录一次并丢弃」的实现是「警告 + return」——而 docstring 说「记录一次」，实际上是**每次调用都记一条警告**。这是一个小的实现与文档之间的差异，不过在实践上可接受（因为未知名字只会来自代码 bug，不会高频出现）。

**`amount <= 0` 被静默忽略，而这不是防御而是语义。**

```python
if amount <= 0:
    return
```

注释没有解释这一点，但从「计数器」的语义看：`increment(name, -1)` 会让计数器倒退，而一个「拒绝次数」倒退是没有意义的。若某处确实需要减计数，那应该用别的方式（例如 `reset`）。

而这个检查**在锁之外**：

```python
if name not in _counters: ...return
if amount <= 0: return
with _lock:
    _counters[name] += amount
```

两个守卫都在锁外，因为它们只读不可变状态（`dict` 的键集与参数）。而实际的 `+=` 在锁内。这个划分是必要的：`_counters` 的键集永不改变（没有插入），所以「键是否存在」可以在无锁状态下读。

**每个函数都有一层 `except Exception` 兜底，而理由写在模块 docstring 里。**

```python
except Exception as metric_error:  # pragma: no cover - defensive
    logger.debug(f"pipeline metric {name!r} not recorded: {metric_error}")
```

docstring 的第四条约束：「**永不致命。** 一个指标失败不能让它测量的操作失败；每个入口点吞掉自己的错误」。

而 `# pragma: no cover` 标记说明这条分支预期不被测试覆盖——因为它理论上不可达（`int` 加法不会抛）。这是一个「防御性但不期待执行」的分支，而它用 `logger.debug` 而不是 `warning`：一个不该发生的错误不该污染日志。

**`observe` 的四个字段选择解释了什么不存。**

```python
summary["count"] += 1
summary["total_seconds"] += value
summary["last_seconds"] = value
if value > summary["max_seconds"]:
    summary["max_seconds"] = value
```

四个：# 样本数、总和、最后一个、最大值。而 `snapshot` 在输出时才算 `mean_seconds`（`total / count`）。

docstring 解释了为什么**没有百分位**：

> **每指标 O(1)。** 时长只保留 count / total / max / last，不是样本。**没有百分位**：一个样本环会是每个指标都要选、辩护和测试的边界，而上面的问题由 max 加均值就能回答。

这段话的逻辑是：百分位需要一个**保留多少样本**的参数，而那个参数是一个新的承诺（多大？怎么测？边界在哪？）。而它服务的两个问题——「分页 vs 抖动」「正常冻结 vs 卡住」——用 max 就能回答：抖动会让 `max` 远超 `mean`，卡住会让 `max` 非常高。

所以这是一个「**够用即止**」的判断，而它避免引入一个新的可调参数与一套新的测试。

**`snapshot` 的取整与锁范围值得看。**

```python
with _lock:
    counters = dict(_counters)
    durations = {name: {...round(...)...} for name, summary in _durations.items()}
return {"counters": counters, "durations": durations}
```

**整个构造过程都在锁内**，包括取整与算平均。所以 `snapshot` 返回的是一个**一致快照**——不会出现「counter 是新的但 duration 是旧的」这种混合。

代价是锁持有时间包含 Python 层的取整运算（8 个键 × 5 个字段）。而这对一个「调用频率是每次 `/health`」的函数完全可接受。

而 `round(x, 4)` 的选择让输出稳定（避免浮点尾数导致的 diff 噪声）。

**`reset` 只在测试里用，而 docstring 明确它是测试专用。**

```python
def reset() -> None:
    """Zero everything (tests only; there is no runtime reset endpoint)."""
```

「没有运行时重置端点」这条声明很重要，因为一个可重置的指标会被运维用来「清掉告警」——而那会让问题消失而不是被解决。

**「故意不复制的东西」是这一页最有意思的一节。**

docstring 最后：

> 这里**故意不复制**：扫描分类计数、源冲突目击、废弃的扫描任务。那些在扫描任务记录里已经有一个有界的、按任务的表面（`GET /documents/scan/status/{track_id}`）——**把它们数两遍会邀请两个数字互相不一致**。

所以「不加指标」也是一个决定，而理由是「同一个事实有两个来源时它们会不一致」。这与 h03 里「占位符预留与展开共用一个正则」、i07 里「三个布尔解析器共用一个词表」是同一条原则的反面应用：**能共用就共用，不能共用就不要复制**。

**指标名与它服务的诊断问题的映射被写成注释，每一条都是一句话的诊断手册。**

```python
# Ingress refused because a manual retry froze new work (LR2 §6.1/§7.2). A handful during a reset is
# normal; a steadily climbing number means clients are retrying into a freeze that is not ending.
FREEZE_REJECTS = "freeze_rejects"
```

```python
# Auto-rescan dirty flag re-armed: a processing request that could not name doc ids (busy-refused,
# partial commit, recovery). Each one costs a later strict sweep, so a high rate explains sweep load
# that no upload seems to account for.
AUTO_RESCAN_REARMS = "auto_rescan_rearms"
```

```python
# DRAIN_TO_IDLE: from freezing ingress to confirmed quiescence. This is the window during which
# uploads are refused, so it is the one an operator feels.
MANUAL_DRAIN_SECONDS = "manual_drain_seconds"
```

最后这条的措辞值得注意：「uploads 被拒绝的那个窗口，**所以它是运维会感觉到的那一个**」。这是一个「从用户视角选指标」的例子——`MANUAL_RESET_SECONDS` 也在测，但它是「没有 worker 运行时」的时长，所以用户不会感到。

**「in-process, per worker」这个约束在 docstring 里与另外两处并列，形成一条一致的架构声明。**

> **进程内、每 worker。** 递增一个共享（Manager 托管的）计数器会给「每次扫描要跑几千次」的路径增加一次 RPC，而那正是调度重构要避免的成本。在 gunicorn N worker 下 `/health` 因此报告的是**回答那个请求的 worker** 的数字——与登录限速器和键控锁表同一条有文档的边界。管道监督者是单所有者的（谁持有 `busy` 就是谁），所以它的测量本来就不跨 worker 分散。

三点：

1. **为什么不共享** ——共享计数器要给热路径加 RPC，而热路径正是被优化的对象。
2. **文档化的边界** ——`/health` 报的是「回答这个请求的 worker」的数字，所以需要多 worker 汇总的运维要知道这一点（并列引用了另两处同样的边界，i05 那页讲的 `LoginRateLimiter` 就是其中之一）。
3. **一个例外** ——管道监督者是单所有者的，所以它的测量不受这个影响。

第 3 点是一个很好的补充：它区分了「受影响的指标」（扫描、活跃计数，可能在任一 worker 上）与「不受影响的指标」（冻结/重置，由持有 `busy` 的那个进程测量）。

**提示词模块的沙箱 `_resolve_prompt_file` 是这一页最硬的一段校验。**

```python
file_name = str(prompt_file_name).strip()
if not file_name:
    raise ValueError(f"{env_key} must be a file name such as '{example_name}'.")
if "\\" in file_name:
    raise ValueError(f"{env_key} must not contain directory separators. Only file names inside PROMPT_DIR/{subdir} are allowed.")

candidate = Path(file_name)
if candidate.is_absolute() or candidate.name != file_name or ".." in candidate.parts:
    raise ValueError(f"{env_key} must be a file name only. Files are loaded from PROMPT_DIR/{subdir} (PROMPT_DIR defaults to ./prompts).")
if candidate.suffix.lower() not in suffixes:
    raise ValueError(f"{env_key} must use a {suffix_hint} extension.")

return directory / candidate.name
```

五道检查：

1. **非空**（且给一个例子文件名）。
2. **无 `\`** ——手工检查反斜杠，因为 `Path` 在 POSIX 上不把 `\` 当分隔符。所以若不显式查，一个 `..\..\etc\passwd` 在 Linux 上会被当成一个合法文件名（只是读不到），而在 Windows 上会穿越出去。
3. **`candidate.is_absolute()`** ——拒绝绝对路径。
4. **`candidate.name != file_name`** ——拒绝任何会使 `.name` 与原文不同的输入，这覆盖了 `/` 分隔符与尾随内容。这是一个很巧的判据：`Path("a/b").name == "b" != "a/b"`，所以任何含分隔符的输入都被拒。
5. **`".." in candidate.parts`** ——拒绝 `..` 组件（虽然第 4 条已经挡住了 `../x`，但 `Path("..").name == ".."` 会通过第 4 条，所以需要第 5 条）。

第 5 条的必要性值得单独说明：`Path("..").name` 是 `".."`（因为 `..` 的「文件名」就是它自己），所以 `candidate.name != file_name` 为假，检查通过。于是单独一个 `..` 会逃过第 4 条——而它作为文件名会指向上级目录。第 5 条正是补这个洞。

**而最后返回 `directory / candidate.name` 而不是 `directory / file_name`。**

用 `candidate.name` 而不是原文，意味着返回值里的文件名是 `Path` 解析后的结果。这一步在当前校验下等价（因为第 4 条保证了 `name == file_name`），但写 `candidate.name` 是一个「防御性地使用已验证的值」的习惯。

**扩展名白名单用 `frozenset` 且分两类。**

```python
_ALLOWED_PROMPT_SUFFIXES = frozenset({".yml", ".yaml"})          # 提示词配置文件
_ALLOWED_USER_PROMPT_SUFFIXES = frozenset({".md", ".txt"})       # 用户前缀文件
```

两类不同的原因很实际：YAML 是**结构化配置**（有 schema 要校验），MD/TXT 是**纯文本**（原样拼接）。所以后者不需要解析，前者需要。

而 `candidate.suffix.lower()` 做了小写化——所以 `.YML` 也通过。

**`load_user_prompt_prefix_source` 是这一页最精细的一段，因为它在一个「不能抛」的位置上做五件可能失败的事。**

docstring 先说清约束：

> **从不抛**：它在 `LightRAG()` 里通过一个 dataclass `default_factory` 运行，那里一个异常会浮现在离它的原因很远的地方。任何失败被记录并降级为无前缀。

所以这个函数必须在**所有**失败路径上返回字符串。而它处理了五种：

第一，**两个环境变量都设时警告并选文件**：

```python
if inline:
    logger.warning("Both USER_PROMPT_PREFIX_FILE and USER_PROMPT_PREFIX are set; using the file '%s' and ignoring the inline value.", file_name)
```

第二，**解析路径失败**：

```python
except (ValueError, OSError, RuntimeError) as exc:
    # ValueError is the sandbox's own rejection. The other two come from resolving PROMPT_DIR itself:
    # Path.expanduser() raises RuntimeError when a home directory cannot be determined, and Path.resolve()
    # raises RuntimeError (<=3.12) or OSError on a symlink loop. Letting either escape would abort every
    # LightRAG() construction, since this runs as a dataclass default_factory -- the exact opposite of
    # the degrade-and-log contract documented above.
    logger.error("Cannot resolve USER_PROMPT_PREFIX_FILE '%s': %s. No prefix applied.", file_name, exc)
    return ""
```

这段注释是这一页最有价值的技术细节之一。它逐个说明了三种异常的来源：

- `ValueError` ——沙箱自己的拒绝。
- **`Path.expanduser()` 在无法确定 home 目录时抛 `RuntimeError`** ——这是 `~` 展开的失败，例如容器里没有 `HOME` 环境变量且 `pwd` 查不到。
- **`Path.resolve()` 在符号链接环上抛 `RuntimeError`（≤3.12）或 `OSError`** ——版本相关的差异。

而「让任一个逃出去会中止每一次 `LightRAG()` 构造」这句话把严重性讲清了：因为这个函数是 `default_factory`，所以它的异常会在**对象构造**时爆，而调用栈会指向 dataclass 的字段初始化——离 `PROMPT_DIR` 那个环境变量隔着好几层。

第三，**读文件失败**：

```python
except (OSError, UnicodeDecodeError) as exc:
    logger.error("Failed to read USER_PROMPT_PREFIX_FILE '%s': %s. No prefix applied.", prefix_path, exc)
    return ""
```

第四，**日志记录 resolved path 而不是配置的名字**：

```python
# Log the resolved path, not the configured name: it is what an operator checks to confirm which file actually won.
# An empty file logs too -- "0 chars" is itself the answer to "why is my prefix not applied?".
logger.info("User prompt prefix loaded from USER_PROMPT_PREFIX_FILE '%s' (%d chars)", prefix_path, len(prefix))
```

两点：

1. **记 resolved 路径** ——因为运维要确认的是「哪个文件真的赢了」，而配置的名字与解析后的路径可能因为 `PROMPT_DIR` 的默认值而不同。
2. **空文件也记日志**（`0 chars`）——因为「前缀没生效」的最常见原因之一就是「文件是空的」，而这条日志直接回答了它。

第五，**内容原样返回，不去空、不加分隔符**：

> 值**原样**返回——不 strip、不追加分隔符。格式化由运维拥有，他们用自己的换行结束它（文件天然以换行结束）。

所以 `load_user_prompt_prefix_source` 是一个**只读不解释**的函数。而 docstring 指向 `lightrag.utils.resolve_user_prompt` 处理后续的拼接。

这个分工的理由是：一个前缀可能是 Markdown 段落、可能是列表项、可能需要在末尾有两个换行——而任何自动格式化都会在某个用法上错。所以把格式化交给写文件的人。

**`_get_prompt_subdir` 用 `expanduser()` + `resolve()`，而这两个正是上面提到的两个失败源。**

```python
configured = os.getenv("PROMPT_DIR", "").strip() or _DEFAULT_PROMPT_DIR
return (Path(configured).expanduser() / subdir).resolve()
```

`expanduser()` 处理 `~/prompts`，`resolve()` 把相对路径转绝对并解析符号链接。所以 `PROMPT_DIR` 与 `INPUT_DIR` / `WORKING_DIR` 一样是「相对于当前工作目录」的（docstring 明说「`PROMPT_DIR` 默认 `./prompts`，相对于当前工作目录，镜像 `INPUT_DIR` / `WORKING_DIR`」）。

**而 `resolve()` 在这里有第二个作用：它是沙箱的边界基准。**

因为 `_resolve_prompt_file` 最后返回 `directory / candidate.name`，而 `directory` 已经是 `resolve()` 过的绝对路径。所以沙箱的边界是一个**真实路径**而不是一个可能含 `..` 或符号链接的路径。若 `PROMPT_DIR` 是 `./prompts/../prompts`，`resolve()` 会把它规范成 `./prompts`，于是「逃逸检查」在正确的基准上做。

**`EntityExtractionPromptProfile` 是 `TypedDict`，所以它的三个键在类型层面被固定。**

```python
class EntityExtractionPromptProfile(TypedDict):
    entity_types_guidance: str
    entity_extraction_examples: list[str]
    entity_extraction_json_examples: list[str]
```

三个键各自对应提示词模板里的一处插值：类型指引（一段文本）、文本模式示例（字符串列表）、JSON 模式示例（字符串列表）。

**为什么示例分文本与 JSON 两套？** 因为实体抽取支持两种输出格式，而**示例必须与被要求输出的格式一致**——给一个「输出 JSON」的指令配文本格式的示例会让模型困惑。所以两套示例是必要的，而 `use_json` 参数决定用哪套。

**`resolve_entity_extraction_prompt_profile` 的合并顺序是「字段级」而不是「整体级」。**

```python
guidance = addon_params.get("entity_types_guidance")
if guidance is None:
    guidance = file_profile.get("entity_types_guidance", default_profile["entity_types_guidance"])
```

所以 `guidance` 单独解析，允许「代码传 guidance + 文件提供 examples」这种混合。而两个 examples 键各自独立：

```python
"entity_extraction_examples": list(file_profile.get("entity_extraction_examples", default_profile["entity_extraction_examples"])),
```

所以是**逐字段**的「代码 → 文件 → 内置」。而 `list(...)` 是为了避免把内置列表的引用交出去（一个调用方修改它会污染默认值）。

**而 YAML 文件在 JSON 模式下必须提供 JSON 示例，这是一条模式相关的校验。**

```python
required_examples_key = "entity_extraction_json_examples" if use_json else "entity_extraction_examples"
if required_examples_key not in file_profile:
    mode_name = "json" if use_json else "text"
    raise ValueError(f"ENTITY_TYPE_PROMPT_FILE '{prompt_file}' must define '{required_examples_key}' when entity extraction runs in {mode_name} mode.")
```

注意这条检查**只在文件被加载时**做（`if prompt_file:` 块内），而不是对合并后的 profile 做。所以「内置的 JSON 示例」被认为总是存在（它确实存在），而**用户提供的文件**必须在它要服务的模式下提供对应示例。

`validate_entity_extraction_prompt_profile_for_mode` 是同一检查的独立版本，作用在**已合并的** profile 上：

```python
if required_examples_key not in prompt_profile or not prompt_profile[required_examples_key]:
    ...
    raise ValueError(f"{source} must define '{required_examples_key}' when entity extraction runs in {mode_name} mode.")
```

而它比 `resolve_...` 里那条多一个 `or not prompt_profile[required_examples_key]` ——检查**非空**而不只是存在。所以两道检查的强度不同：

- `resolve_...` 里：文件里**有没有**这个键。
- `validate_..._for_mode` 里：最终 profile 里这个键**有没有且非空**。

第二道更严，且它能捕捉一个第一道漏掉的情形：文件提供了 `entity_extraction_json_examples: []`（空列表）。第一道只检查键存在，所以通过；第二道检查非空，所以拒绝。而「空示例列表」意味着提示词里没有任何示例——那会让抽取质量显著下降而不报错。

而 `validate_...` 里那个 `source` 变量的措辞很有意思：

```python
source = f"ENTITY_TYPE_PROMPT_FILE '{prompt_file_name}'" if prompt_file_name else "the resolved prompt profile"
```

所以同一个函数能报两种来源：具体文件（当调用方知道文件名时）或「解析后的 profile」（当它只知道最终值时）。这让错误消息尽可能具体。

**`load_entity_extraction_prompt_profile` 的校验是「每个字段独立可选，但给了就必须合法」。**

```python
guidance = raw_profile.get("entity_types_guidance")
if guidance is not None:
    if not isinstance(guidance, str) or not guidance.strip():
        raise ValueError(f"ENTITY_TYPE_PROMPT_FILE '{profile_path}' field 'entity_types_guidance' must be a non-empty string.")
    profile["entity_types_guidance"] = guidance.rstrip()

for field_name in ("entity_extraction_examples", "entity_extraction_json_examples"):
    if field_name in raw_profile:
        profile[field_name] = _normalize_prompt_examples(raw_profile[field_name], field_name, profile_path)
```

所以文件可以只提供其中一个字段——其余从内置取。而「给了就必须合法」体现在 `not isinstance(...) or not guidance.strip()` 与 `_normalize_prompt_examples` 的逐项检查。

注意 `if guidance is not None` 与 `if field_name in raw_profile` 用的是**不同的判据**：前者 `is not None`（YAML 里 `field:` 会解析成 `None`），后者 `in`（列表字段不存在时的语义不同）。

**`_normalize_prompt_examples` 逐项检查非空字符串，而这是必要的。**

```python
for index, item in enumerate(value):
    if not isinstance(item, str) or not item.strip():
        raise ValueError(f"... field '{field_name}' item {index} must be a non-empty string.")
    normalized.append(item.rstrip())
```

错误消息带 `index` ——所以运维能定位到 YAML 列表里的第几项。而 `item.rstrip()` 是唯一的内容修改：**只去尾部空白**，因为示例是多行文本而尾随空白无意义、且会占用 token。

而 `not item.strip()` 拒绝「只有空白」的示例——因为一个空示例会被插进提示词，在模型看来是「示例是空的」，那比没有示例更糟。

**`yaml.safe_load` 而不是 `yaml.load`，而空文件被特殊处理。**

```python
try:
    raw_profile = yaml.safe_load(content)
except yaml.YAMLError as exc:
    raise ValueError(f"ENTITY_TYPE_PROMPT_FILE '{profile_path}' contains invalid YAML: {exc}") from exc

if raw_profile is None:
    raw_profile = {}
```

`safe_load` 不构造任意 Python 对象（防 YAML 反序列化攻击）——而这里的数据源是用户提供的文件，所以这一条是必要的。

`raw_profile is None` 是「空文件」或「全是注释」的结果——而把它当成空 dict 让后续的「每个字段独立可选」逻辑自然生效（所有字段都从内置取）。

**而 `if not profile_path.is_file()` 与 `if not profile_path.exists()` 是两道检查而不是一道。**

```python
if not profile_path.exists():
    raise FileNotFoundError(f"ENTITY_TYPE_PROMPT_FILE '{profile_path}' does not exist.")
if not profile_path.is_file():
    raise ValueError(f"ENTITY_TYPE_PROMPT_FILE '{profile_path}' must point to a file.")
```

分开的理由是给不同的错误类型：文件不存在是 `FileNotFoundError`（可能是路径写错），而「存在但是目录」是 `ValueError`（配置错）。这个区分让调用方（或运维读日志）能判断「我是该检查路径还是该检查挂载」。

而 `read_text(encoding="utf-8")` 的 `OSError` 被重新包装成带文件名的消息：

```python
except OSError as exc:
    raise OSError(f"Failed to read ENTITY_TYPE_PROMPT_FILE '{profile_path}': {exc}") from exc
```

`from exc` 保留原始异常（可能是权限不足、I/O 错误），而外层消息加上了「哪个文件」——因为 `PermissionError` 的默认消息只有路径，而这个包装让它与 `ENTITY_TYPE_PROMPT_FILE` 这个配置项关联起来。

## 关键取舍

**源码事实：** 指标模块存在是因为「有界内存改造把若干决策移出了快路径，而每一个都可能变慢或有噪声而没有任何东西可见地出错」；它要区分「扫描在分页 vs 抖动」「冻结正常 vs 卡住」。

**源码事实：** **固定键集**是第一条设计约束——「一个未知名字被记录一次并丢弃而不是插入，因为一个随基数增长的指标注册表正是整个计划要消除的那种无界结构」。声明（`_COUNTERS` / `_DURATIONS` 元组）与存储（dict）都从元组派生，所以不可能不同步。

**源码事实：** 每个入口点都有一层 `except Exception` 兜底（`# pragma: no cover - defensive`，`logger.debug`），因为「一个指标失败不能让它测量的操作失败」。

**源码事实：** 时长只保留 count / total / max / last，**没有百分位**，因为「一个样本环会是每个指标都要选、辩护和测试的边界，而上面的问题由 max 加均值就能回答」。

**源码事实：** `observe` 的 `last_seconds` 与 `max_seconds` 是两种不同的事实：前者「最后那次多慢」（用于看趋势），后者「最坏多慢」（用于判断抖动）。

**源码事实：** `snapshot` 的**整个构造过程都在锁内**（含取整与算平均），所以返回的是一致快照——不会出现「counter 新、duration 旧」的混合。

**源码事实：** `reset` 标注为「仅测试；没有运行时重置端点」——一个可重置的指标会被用来「清掉告警」而使问题消失而非被解决。

**源码事实：** 「故意不复制」的三类（扫描分类计数、源冲突目击、废弃扫描任务）已经有「有界的、按任务的表面」——「把它们数两遍会邀请两个数字互相不一致」。

**源码事实：** 每个指标名旁边的注释是一句话的诊断手册，而 `MANUAL_DRAIN_SECONDS` 的理由是「uploads 被拒绝的那个窗口，**所以它是运维会感觉到的那一个**」——按用户感知选指标。

**源码事实：** 指标是**进程内、每 worker** 的，因为共享计数器会给「每次扫描跑几千次」的热路径加 RPC；`/health` 因此报「回答这个请求的 worker」的数字，与登录限速器和键控锁表同一条文档化边界。例外是管道监督者（单所有者，谁持 `busy` 是谁）。

**源码事实：** 提示词覆盖三层：`addon_params` → `ENTITY_TYPE_PROMPT_FILE` 的 YAML → 内置 `PROMPTS`，且合并是**字段级**的（可以代码传 guidance + 文件提供 examples）；`list(...)` 避免把内置列表的引用交出去。

**源码事实：** 文件名沙箱五道检查：非空、无 `\`（**手工查**，因为 `Path` 在 POSIX 上不把 `\` 当分隔符）、`is_absolute()` 拒、`candidate.name != file_name` 拒（覆盖 `/` 与尾随内容）、`".." in candidate.parts` 拒（补 `Path("..").name == ".."` 这个漏洞）。

**源码事实：** 扩展名白名单分两类：提示词配置用 `.yml`/`.yaml`（结构化、要校验），用户前缀用 `.md`/`.txt`（纯文本、原样拼接）；`suffix.lower()` 所以 `.YML` 也过。

**源码事实：** `load_user_prompt_prefix_source` **从不抛**，因为「它在 `LightRAG()` 里通过 dataclass `default_factory` 运行，那里一个异常会浮现在离它的原因很远的地方」；它捕获的三种路径异常各有具体来源——`ValueError` 是沙箱拒绝、**`Path.expanduser()` 无法确定 home 时抛 `RuntimeError`**、**`Path.resolve()` 在符号链接环上抛 `RuntimeError`（≤3.12）或 `OSError`**。

**源码事实：** 前缀内容**原样返回**（不 strip、不追加分隔符），因为「格式化由运维拥有，他们用自己的换行结束它」；两个环境变量都设时警告并选文件。

**源码事实：** 日志记 **resolved path** 而不是配置的名字，因为「那是运维检查以确认哪个文件真的赢了」的；**空文件也记日志**（`0 chars`），因为那是「前缀没生效」的最常见答案。

**源码事实：** YAML 用 `safe_load`（数据源是用户文件，防反序列化攻击）；空文件（`raw_profile is None`）当作空 dict，于是所有字段从内置取。

**源码事实：** 每个字段**独立可选但给了就必须合法**：`entity_types_guidance` 用 `is not None` 判（YAML 里 `field:` 解析成 `None`），列表字段用 `in` 判；示例逐项检查非空字符串且错误消息带 `index`；`rstrip()` 是唯一的内容修改。

**源码事实：** 「文件必须在当前模式下提供对应示例」这条检查有两道且强度不同：`resolve_...` 里检查文件里**有没有**这个键，`validate_..._for_mode` 里检查最终 profile 里**有没有且非空**——后者能捕捉「文件提供了 `entity_extraction_json_examples: []`」这种第一道漏掉的情形。

**源码事实：** 「文件不存在」抛 `FileNotFoundError`、「存在但是目录」抛 `ValueError`——分开以便运维判断是路径写错还是配置错；读失败重新包装成带 `ENTITY_TYPE_PROMPT_FILE` 的消息并用 `from exc` 保留原异常。

**架构推论：** 指标模块示范了「**为已知问题设指标，而不是为通用可观测性设指标**」。四个计数器与四个时长每一项都对应一个具体的诊断问题，而「不加什么」也有理由（已经别处有界的就不重复）。这比「把所有东西都埋点」更难做但更有用——因为一个没有对应问题的指标不会有人看，而它会永久增加维护面。

**架构推论：** 「固定键集」这条约束把「指标名」从运行时值变成了编译期声明。代价是加一个指标要改代码（不能动态注册），收益是**指标集的大小与每项的存储都是静态可知的**——于是「这个模块不会泄漏内存」是一个可以证明的性质，而不是一个需要监控的性质。任何「自己也可能成为问题」的观测工具都该这样约束自己。

**架构推论：** `load_user_prompt_prefix_source` 是「**在不允许失败的位置上做多件可能失败的事**」的范例。它必须返回字符串（因为它在 `default_factory` 里），所以要为每一类失败准备一个降级路径，并且要为每类失败写清「为什么这个异常会出现」。而那段关于 `expanduser` / `resolve` 抛什么异常的注释是这类代码的模板——**跨平台、跨版本的异常行为必须逐个记录**，否则下一个人会把它们删掉。

**架构推论：** 文件名沙箱的五道检查说明了「路径校验必须用**多重独立判据**」。任何单一判据都有绕过：`Path` 的语义在不同平台不同（`\` 在 POSIX 是普通字符）、`..` 的 `.name` 是它自己、绝对路径与相对路径要分开判。所以正确的做法是「列出一个合法输入的全部性质」（非空、无分隔符、非绝对、`name` 等于原文、无 `..` 组件、后缀在白名单），逐条检查——而不是「用一个正则匹配一下」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/pipeline_metrics.py` | `increment` | 声明过的计数器递增，未知名字丢弃 |
| `lightrag/pipeline_metrics.py` | `observe` | 时长样本累进 count/total/max/last |
| `lightrag/pipeline_metrics.py` | `snapshot` | 全程持锁构造一致快照，输出时算 mean |
| `lightrag/pipeline_metrics.py` | `_COUNTERS` / `_DURATIONS` | 固定键集的声明，存储从中派生 |
| `lightrag/pipeline_metrics.py` | `FREEZE_REJECTS` | 流量被冻结拒绝；「持续攀升说明冻结没在结束」 |
| `lightrag/pipeline_metrics.py` | `AUTO_RESCAN_REARMS` | 脏标志重新武装；解释「上传量解释不了的扫描负载」 |
| `lightrag/pipeline_metrics.py` | `MANUAL_DRAIN_SECONDS` | 上传被拒的窗口，「运维会感觉到的那一个」 |
| `lightrag/prompt.py` | `EntityExtractionPromptProfile` | 三个键的 TypedDict，字段级合并的载体 |
| `lightrag/prompt.py` | `resolve_entity_extraction_prompt_profile` | 代码 → YAML → 内置的字段级合并 |
| `lightrag/prompt.py` | `validate_entity_extraction_prompt_profile_for_mode` | 检查当前模式的示例存在且非空 |
| `lightrag/prompt.py` | `load_entity_extraction_prompt_profile` | `safe_load` + 逐字段校验 |
| `lightrag/prompt.py` | `_normalize_prompt_examples` | 逐项非空检查，错误消息带 index |
| `lightrag/prompt.py` | `resolve_user_prompt_prefix_path` | 用户前缀文件路径，沙箱保护 |
| `lightrag/prompt.py` | `resolve_entity_type_prompt_path` | 提示词配置路径，.yml/.yaml 白名单 |
| `lightrag/prompt.py` | `_resolve_prompt_file` | 五道检查的共用沙箱 |
| `lightrag/prompt.py` | `load_user_prompt_prefix_source` | 从不抛，异常来源逐个记录 |
| `lightrag/prompt.py` | `_get_prompt_subdir` | `expanduser()` + `resolve()`，沙箱边界基准 |

## 参考

- [lightrag 源码：pipeline_metrics.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/pipeline_metrics.py)
- [lightrag 源码：prompt.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/prompt.py)
- [Python 文档：pathlib](https://docs.python.org/3/library/pathlib.html)
- [PyYAML 文档：safe_load](https://pyyaml.org/wiki/PyYAMLDocumentation)

## 与主核 / AI 的关系

主核里最接近的是 **`pg_stat_*` 视图**：一组固定名字的计数器与时长，对应一组已知的诊断问题（`pg_stat_activity` 看谁在跑、`pg_stat_bgwriter` 看检查点压力、`pg_stat_user_tables` 看 autovacuum 需求）。而 `pg_stat_statements` 的正是一个**有基数**的指标（按查询归一化后的 SQL 文本），而它需要一个明确的 `pg_stat_statements.max` 上限——这正好对应这里「固定键集」的立场。

**差别在于汇总范围。** 主核的统计是共享内存里的，所以所有进程看到同一份；这里是每 worker 进程内的，所以 `/health` 报的是「回答这个请求的 worker」的数字。这个取舍在本页 docstring 里被明确承认并与另两处并列成一条「文档化边界」——**承认边界比假装没有边界更有用**，因为知道边界的人会正确地解读数字。

而提示词沙箱与主核的**配置文件名限制**是同一条思路：`postgresql.conf` 里的 `hba_file`、`ssl_cert_file` 等参数只接受文件名而不接受任意路径，因为一个可配置的路径是提权向量。

与 i07（绑定选项）的关系：那一页讲「配置项如何从 dataclass 派生」，这一页的提示词配置是另一种——它不走 dataclass，而是走「环境变量 → 沙箱解析 → YAML 校验」。两条路径并存的原因是：绑定选项是**程序员的参数**（数量多、要生成 CLI），提示词是**运维的文本**（数量少、内容是长文本）。

与 i05（API 安全）的关系：两处都有「用户可控输入必须沙箱」。那边是 HTTP 头与请求体，这边是环境变量指定的文件名。

对 AI 工程最实用的一条是**「为每个指标写一句它回答什么问题」**。这一层的四个计数器与四个时长，每一个的注释都是一句诊断手册（「持续攀升说明什么」「高比率解释了什么的负载」）。而多数指标埋点只写「记录 X 的次数」，于是三个月后没人知道那个数字正常范围是多少。判据是：**这个指标超过某个值时，运维该做什么？** 答不上来的指标不如不加。

另一条是 `load_user_prompt_prefix_source` 那种「在不允许失败的位置上必须列出所有失败源」。任何运行在 `default_factory`、模块导入期、或错误处理器里的函数都有同样的约束——它们抛异常的后果远大于正常位置，而异常的类型往往比 API 文档写的多（`expanduser` 抛 `RuntimeError` 就不在直觉里）。做法是：**列出每个 `except` 分支对应的具体失败场景并写进注释**，而不是写一个笼统的 `except Exception`。

最后一条是路径校验要用多重独立判据。`Path("..").name == ".."` 这个细节说明「用 `Path` 的语义当判据」会漏掉一些情形。正确做法是列出合法输入的全部性质逐条检查——而这也让错误消息能说清「是哪一条不满足」，从而运维能一次改对。
