---
project: lightrag
feature: parser-registry
title: 解析引擎注册与插件
page_type: feature
schema_version: 1
refs:
  - path: lightrag/parser/registry.py
    symbols: [ParserSpec, register_parser, get_parser]
  - path: lightrag/parser/registry.py
    symbols: [parser_specs_snapshot, supported_parser_engines]
  - path: lightrag/parser/plugins.py
    symbols: [load_third_party_parsers]
---

# 解析引擎注册与插件

## 一句话职责

`registry.py` 用一个**模块级字面量字典** `_REGISTRY` 集中存放全部引擎的轻量元数据（`ParserSpec`：后缀、并发组、endpoint 判据、实现路径），让「这个后缀能不能解析」这类最热的查询**不必 import 任何解析器实现**；`plugins.py` 用 `importlib.metadata` 的 entry point 组 `lightrag.parsers` 让第三方包把自己的 `ParserSpec` 注册进来，**一个插件坏掉不能拖垮服务启动**。

## 机制是什么

注册表要同时满足三个互相拉扯的要求，而它的形状就是这三个要求折出来的：

1. **能力查询必须便宜**。上传时的后缀白名单、目录扫描的 `supported_extensions`、`LIGHTRAG_PARSER` 的启动校验——这些都发生在每次请求/每次启动，而且要在**一个文档都还没解析**的时候就能回答。若能力信息挂在解析器类上，回答「`.pdf` 支持吗」就要先 import 一个会拉进 `httpx` 的模块。
2. **实现必须延迟加载**。mineru / docling 是 HTTP 客户端，只在真有文档要走那条路时才该付费。
3. **第三方引擎必须能接入**，且接入方式不能要求改动本仓代码。

于是分工是：`ParserSpec` 只装**数据 + 两个零参数闭包**（`endpoint_configured` / `endpoint_requirement`，只读环境变量、不发网络请求），`impl` 是一个 `"module:Class"` **字符串**，只有 `get_parser` 才会 `importlib` 它。

这个结构带来一个必须付的代价：**能力数据是「一份需要手工与实现保持一致的第二真相」**。声明 `.docx` 支持 docling，但如果实际部署的 docling-serve 没装 LibreOffice，那份声明就是假的。源码对这个问题的答案是「声明保守基线 + 部署侧用环境变量扩展」——这就是 `extra_suffixes_env` 与那个描述符 `_EnvExtendedSuffixes` 的由来。

## 怎么实现的

**`ParserSpec` 的字段分成三类：身份、能力、闸门。**

```
@dataclass(frozen=True)
class ParserSpec:
    """Lightweight, import-cheap metadata for one parser engine.

    Holds everything the pipeline needs to *route* and *gate* a document
    without importing the parser implementation.  ``impl`` is a
    ``"module:Class"`` string imported lazily by :func:`get_parser`.
    """

    engine_name: str
    impl: str
    suffixes: frozenset[str] = _EnvExtendedSuffixes()
    user_selectable: bool = True
    queue_group: str = PARSER_ENGINE_NATIVE
    concurrency: int | None = None
    endpoint_configured: Callable[[], bool] = field(default=lambda: True)
    endpoint_requirement: Callable[[], str | None] = field(default=lambda: None)
    extra_suffixes_env: str | None = None
```

逐项看几个不那么直白的：

- **`impl` 是字符串而不是类**。这一个选择就实现了「延迟加载」：注册表里存的只是文本，`get_parser` 才 `importlib.import_module`。
- **`user_selectable`** 让内部处理器（`reuse` / `passthrough`）存在但不可被用户选为引擎。它们没有后缀、不参与能力集合，只在 `full_docs` 行的 `parse_format` 已经解析过时被路由到。
- **`queue_group`** 决定「这个引擎的解析任务排进哪个队列」。`legacy` 显式复用 `native` 的池（注释：`# shares native pool (local, no network)`）——因为它们都是本地无网络的，共享池是合理的。
- **`concurrency` 默认 `None` 的含义写在注释里**：「``None`` means this spec does not own its group's concurrency (built-in groups are sized by the LightRAG instance field instead)」。也就是说内建引擎的并发由 LightRAG 的 `max_parallel_parse_*` 字段控制（支持构造器覆盖），只有第三方引擎才需要在 spec 里烘死一个数。
- **`endpoint_configured` / `endpoint_requirement` 是一对闭包**，默认分别是「恒真」与「无要求」。有要求的引擎给出两样：**怎么判**、**缺什么的名字**。后者用于错误消息——见下面的 `_mineru_endpoint_requirement`。

**「闸门」这对闭包的设计值得单独看，因为它们把「错误消息」也变成了数据。**

```
def _mineru_endpoint_configured() -> bool:
    mode = os.getenv("MINERU_API_MODE", "local").strip().lower()
    if mode == "official":
        return bool(os.getenv("MINERU_API_TOKEN", "").strip())
    if mode == "local":
        return bool(os.getenv("MINERU_LOCAL_ENDPOINT", "").strip())
    return False

def _mineru_endpoint_requirement() -> str | None:
    mode = os.getenv("MINERU_API_MODE", "local").strip().lower()
    if mode == "official":
        return "MINERU_API_TOKEN"
    if mode == "local":
        return "MINERU_LOCAL_ENDPOINT"
    allowed = ", ".join(sorted(_VALID_MINERU_API_MODES))
    return f"valid MINERU_API_MODE ({allowed})"
```

三个要点：

1. **两个函数各自独立读环境，没有共享状态**。这意味着它们随时可以答，不依赖初始化顺序。
2. **`requirement` 的返回值是给人读的**——它要么是一个环境变量名，要么是「合法的 `MINERU_API_MODE`（local, official）」。于是上层可以拼出「rule 3 需要 MINERU_LOCAL_ENDPOINT 已配置」这种**直接可行动**的错误。
3. **`MINERU_API_MODE` 非法时 `configured` 返回假、`requirement` 指出该变量本身有问题**。这是一个很细的设计：不是「不知道要配什么」，而是「你配的这个模式不认识」。

**`suffixes` 用一个描述符实现「每次访问都现读环境」。**

```
class _EnvExtendedSuffixes:
    """Descriptor-typed ``ParserSpec.suffixes``: declared baseline ∪ env extras.

    A spec that names an ``extra_suffixes_env`` reads back its declared
    baseline unioned with that variable's suffixes, resolved **at every
    access** rather than at construction.  ``_REGISTRY`` is a module-level
    literal built at import time, which for the parser debug CLI happens
    inside ``_build_parser()`` — before ``_run()`` imports ``lightrag.utils``
    and triggers ``load_dotenv``.  Baking the env in at construction would
    therefore permanently capture an empty set for anyone configuring the
    variable in ``.env`` rather than the parent shell.
    """
```

**这段注释描述的是一个真实存在的初始化顺序 bug，以及为什么不能用「构造时求值」这种更简单的做法**：

- `_REGISTRY` 是模块级字面量，在 **import 时**构造。
- 解析器 debug CLI 的路径是：`_build_parser()` 里 import 注册表（为了填 `--engine` 的 choices）→ **之后** `_run()` 才 import `lightrag.utils`，而 `load_dotenv()` 是在那里触发的。
- 所以构造 `_REGISTRY` 的时刻，`.env` **还没被加载**。
- 若构造时就把 `os.getenv("MINERU_ADDITIONAL_SUFFIXES")` 求值，那些写在 `.env` 里的后缀会被永久捕获成空集——而用户明明配了。

于是答案是把求值点挪到**每次访问**：

```
def __get__(self, obj, objtype=None) -> frozenset[str]:
    if obj is None:
        raise AttributeError(self._attr)
    base: frozenset[str] = getattr(obj, self._attr)
    env_name = getattr(obj, "extra_suffixes_env", None)
    return base | _env_suffixes(env_name) if env_name else base
```

两个细节：

1. **`obj is None` 时抛 `AttributeError`**，注释说明这是刻意的：「Deliberately not a dataclass default: ``suffixes`` stays a required argument, so a registrant that forgets it still gets a `TypeError` instead of an engine that silently matches nothing.」——即**访问类属性（而非实例属性）时让它看起来像「没有这个属性」**，这样 dataclass 就没法给 `suffixes` 一个隐式默认值，忘记传的注册者会立刻拿到 `TypeError`，而不是得到一个「什么都不匹配」的引擎。
2. **`__set__` 绕过 frozen 并强制转 `frozenset`**：

```
def __set__(self, obj, value) -> None:
    # frozen dataclass: bypass the instance-assignment guard, and coerce so
    # a registrant passing a plain ``set`` cannot have it mutated in place.
    object.__setattr__(obj, self._attr, frozenset(value))
```

**「强制转 frozenset」是为了防一个具体的 footgun**：注册者若传一个 `set`，之后任何人对它的原地修改都会悄悄改变引擎能力。

docstring 还诚实地点出了这个设计的代价：

> The cost is that ``__eq__`` / ``__hash__`` of a spec carrying ``extra_suffixes_env`` shift with the environment; specs are values in ``_REGISTRY`` and are never used as dict keys or set members.

**「正因为 spec 从不被用作键或集合成员，这个代价才可以接受」** ——这是一条把「不变量」与「实现自由」绑起来的说明。

**环境后缀的解析是宽容归一 + 严格拒绝的混合。**

```
_SUFFIX_TOKEN = re.compile(r"^[a-z0-9]+$")

def _parse_env_suffixes(env_name: str) -> tuple[frozenset[str], tuple[str, ...]]:
    """Split ``env_name``'s comma-separated list into (valid, malformed) tokens.

    Normalisation per entry: strip surrounding whitespace, lowercase, drop
    leading dots — so ``" .PPT "`` and ``"ppt"`` are the same suffix and a
    trailing comma is harmless.  Anything else that is not ``[a-z0-9]+`` is
    *malformed*, not a suffix: it can never equal ``Path.suffix``, so silently
    admitting it would leave the operator's actual intent unmet with no
    diagnostic.
    """
```

**归一与拒绝的界线画在「能不能成为一个真实的 `Path.suffix`」上**：

- 空 token（末尾逗号、连续逗号）→ 直接跳过，无害。
- 大小写、空白、前导点 → 归一掉，因为 `" .PPT "` 与 `"ppt"` 显然是同一个意图。
- 其余（`*.doc` 这种 glob 习惯、分号分隔）→ **归入 malformed 并上报**，理由是不上报就等于「静默地什么都没做」。

这个 split 返回 `(valid, malformed)` 两元组，而不是只返回 valid 然后打条日志——因为**上报要能变成一个启动错误**，见下面的 `malformed_env_suffixes()`。

**`_REGISTRY` 是纯字面量，五个内建引擎的差异全在字段里。**

```
_REGISTRY: dict[str, ParserSpec] = {
    PARSER_ENGINE_NATIVE: ParserSpec(
        engine_name=PARSER_ENGINE_NATIVE,
        # Single ``native`` engine; the dispatcher picks docx vs markdown by
        # source suffix (see lightrag.parser.native_dispatch).
        impl="lightrag.parser.native_dispatch:NativeParser",
        suffixes=frozenset({"docx", "md", "textpack"}),
        queue_group=PARSER_ENGINE_NATIVE,
        # Built-in groups are sized by the LightRAG ``max_parallel_parse_*``
        # instance field (supports constructor override), so no spec-level
        # ``concurrency`` here.
    ),
    PARSER_ENGINE_LEGACY: ParserSpec(..., suffixes=_LEGACY_SUFFIXES, queue_group=PARSER_ENGINE_NATIVE),
    PARSER_ENGINE_MINERU: ParserSpec(..., endpoint_configured=_mineru_endpoint_configured,
        endpoint_requirement=_mineru_endpoint_requirement, extra_suffixes_env="MINERU_ADDITIONAL_SUFFIXES"),
    PARSER_ENGINE_DOCLING: ParserSpec(..., endpoint_configured=_env_endpoint_configured("DOCLING_ENDPOINT"),
        endpoint_requirement=lambda: "DOCLING_ENDPOINT", extra_suffixes_env="DOCLING_ADDITIONAL_SUFFIXES"),
    PARSER_ENGINE_REUSE: ParserSpec(..., suffixes=frozenset(), user_selectable=False),
    PARSER_ENGINE_PASSTHROUGH: ParserSpec(..., suffixes=frozenset(), user_selectable=False),
}
```

三个可对照的观察：

1. **`native` 只有三个后缀，`legacy` 有四十多个，但 `legacy` 复用 `native` 的队列组。** 这个组合是对的：`legacy` 是「什么都能当纯文本读」的兜底，而后缀多的引擎不一定慢，队列分组应当按**资源性质**（本地 / 有网络）而不是按**能力范围**。注释把这一点写在 `legacy` 那一行（`# shares native pool (local, no network)`）。
2. **`native` 注释说明它是「单引擎 + 按后缀二次分派」**：`native` 名下其实是 docx 与 markdown 两套实现，由 `native_dispatch` 按后缀挑。也就是说「引擎」这个概念的粒度是**队列与配置单位**，不是实现单位。
3. **`reuse` / `passthrough` 的后缀是空集且 `user_selectable=False`**，于是它们既不进 `supported_parser_engines()`、也不给能力集合贡献任何后缀——但 `get_parser("reuse")` 仍然能拿到实例。**「可路由」与「可选」是两件事**，这个字段把两者分开。

**`get_parser` 是唯一的实现加载点，缓存键是 `(engine, impl)`。**

```
_INSTANCE_CACHE: dict[tuple[str, str], "BaseParser"] = {}

def get_parser(engine: str, *, specs: dict[str, ParserSpec] | None = None):
    """Return a (cached) parser instance for ``engine`` or ``None``.

    Imports the implementation lazily via ``importlib`` — only here does the
    heavy engine package (and e.g. ``httpx``) get pulled in.
    """
    spec = _table(specs).get(engine)
    if spec is None:
        return None
    cache_key = (engine, spec.impl)
    inst = _INSTANCE_CACHE.get(cache_key)
    if inst is None:
        module_path, _, cls_name = spec.impl.partition(":")
        cls = getattr(importlib.import_module(module_path), cls_name)
        inst = cls()
        _INSTANCE_CACHE[cache_key] = inst
    return inst
```

关键在缓存键**包含 `impl`**，注释说明了理由：「``(engine_name, impl) -> instance``. Keyed on impl so a re-registration with a different implementation is not served a stale cached instance.」

**这是一个「注册表可变、缓存不能过期」的经典问题**：`register_parser` 允许同名覆盖，若缓存只按 `engine_name` 建键，覆盖后仍会返回旧实例——而症状是「改了插件但行为没变」，排查成本极高。把 `impl` 并进键里，就让「换实现」自动变成「换缓存条目」。

反面是**旧实例不会被清掉**（键不同就留着），所以反复覆盖会积累垃圾实例。对一个进程内注册的引擎表来说这是可接受的——注册在启动阶段发生，次数是个位数。

**`parser_specs_snapshot` 解决的是「批处理中途注册表被改」的问题。**

```
def parser_specs_snapshot() -> dict[str, ParserSpec]:
    """Return a shallow snapshot of the registry.

    The pipeline takes one snapshot at batch start and threads it through
    queue construction, routing and the parse workers so a concurrent
    ``register_parser`` cannot change the engine set mid-batch.
    """
    return dict(_REGISTRY)
```

**注意它是「浅」快照**：值仍是同一批 `ParserSpec` 对象（`frozen=True`，所以共享是安全的），只是键集合被冻结在那一刻。这足以挡住「批处理进行到一半有人在注册新引擎」，而成本只是一个字典拷贝。

这也是为什么前面每个查询函数都带 `specs: dict[str, ParserSpec] | None = None` 参数、并统一走 `_table(specs)`：

```
def _table(specs: dict[str, ParserSpec] | None) -> dict[str, ParserSpec]:
    return specs if specs is not None else _REGISTRY
```

**「可选传入快照」而不是「全局变量」**，让批处理的一致性不需要靠锁——每个调用点自己决定用快照还是实时表。

**`supported_parser_engines` 与 `available_engine_suffixes` 差了一个闸门。**

```
def supported_parser_engines(specs=None) -> frozenset[str]:
    """User-selectable engine names (replaces SUPPORTED_PARSER_ENGINES)."""
    return frozenset(name for name, spec in _table(specs).items() if spec.user_selectable)

def available_engine_suffixes(specs=None) -> frozenset[str]:
    """Suffixes (lowercase, no dot) parseable by a *currently usable* engine.

    Union over user-selectable engines whose ``endpoint_configured()`` gate
    passes. This is the single source for the API upload allowlist and the
    input-directory scan (``DocumentManager.supported_extensions``) ...
    """
    out: set[str] = set()
    for spec in _table(specs).values():
        if spec.user_selectable and spec.endpoint_configured():
            out |= spec.suffixes
    return frozenset(out)
```

**两个集合的差别正是「配了吗」这一个条件**：

- `supported_parser_engines()` 回答「**语法上**哪些引擎名合法」——用于校验 `LIGHTRAG_PARSER` 规则里的引擎名、校验文件名 hint 里的引擎名。这里**不能**要求 endpoint 已配，否则「你写了 docling 规则但没配 endpoint」会被报成「不支持的引擎」，把配置错误说成拼写错误。
- `available_engine_suffixes()` 回答「**现在**能处理哪些后缀」——用于上传白名单与目录扫描。这里必须过闸门，否则用户能上传一个 `.png`，然后发现没有引擎能处理它。

docstring 还把默认部署的行为写清了：「in a default deployment (no external endpoints) it equals the local engines' suffixes (legacy ∪ native)」——这是一个很有用的自检：**没配任何 endpoint 时，可上传后缀就等于本地两个引擎能吃的那些**。

**`malformed_env_suffixes` 与 `validate_parser_suffix_env_vars` 把「宽容解析」与「启动报错」分开。**

```
def malformed_env_suffixes(specs=None) -> dict[str, tuple[str, ...]]:
    """Env var name -> its malformed entries, over specs declaring one.

    Empty when every ``extra_suffixes_env`` holds a well-formed list (the
    common case, including "unset"). Consumed by the startup check
    ``routing.validate_parser_suffix_env_vars``; kept here so the parsing rule
    and the reporting share one implementation.
    """
```

**「让解析规则与上报共享一份实现」是关键**：如果 `_parse_env_suffixes` 与校验函数各写一遍判断，两处必然漂移，而漂移的方向是「运行时接受、启动时拒绝」或相反。这里的设计是：**注册表侧只负责解析并返回 valid/malformed，是否把它升级成错误由调用方决定**。

对应在 `routing.py` 侧：

```
def validate_parser_suffix_env_vars() -> None:
    errors = [f"{env_name} has malformed entries: " + ", ".join(repr(t) for t in tokens)
              + "; use bare lowercase suffixes separated by ',' (e.g. 'doc,ppt,xls')"
              for env_name, tokens in sorted(malformed_env_suffixes().items())]
    if errors:
        raise ParserRoutingConfigError("; ".join(errors))
```

错误消息给出**正确写法示例**（`'doc,ppt,xls'`），这是这类配置校验的标准做法。

**容错与拒绝的边界在「unset 不报错」上**：`_parse_env_suffixes` 对未设置的环境变量得到空字符串 → 无 token → 无 valid 也无 malformed → 校验通过。所以「大部分部署什么都没配」是干净的零成本路径。

**插件加载是「幂等 + 单个失败不致命」。**

```
_loaded = False

def load_third_party_parsers(*, force: bool = False) -> list[str]:
    """Discover and run all ``lightrag.parsers`` entry points.

    Idempotent per process (``force=True`` re-runs, for tests).  Returns the
    names of the entry points that loaded successfully.  A failing plugin is
    logged with its origin and skipped — one broken third-party package must
    not take down server startup or the debug CLI; the built-in engines are
    registered statically and are never affected.
    """
    global _loaded
    if _loaded and not force:
        return []
    _loaded = True

    loaded: list[str] = []
    for ep in entry_points(group=ENTRY_POINT_GROUP):
        try:
            register = ep.load()
            register()
        except Exception as e:
            logger.error("[parser-plugins] failed to load parser plugin %r (%s): %s", ep.name, ep.value, e)
            continue
        loaded.append(ep.name)
        logger.info("[parser-plugins] loaded parser plugin %r (%s)", ep.name, ep.value)
    return loaded
```

五点：

1. **`_loaded` 全局标志让它每进程只跑一次**。这不是性能优化而**语义要求**：插件注册会覆盖 `_REGISTRY`，重复跑没有意义（幂等），但更重要的是 `force=True` 只在测试里用。
2. **`except Exception` 包住 `ep.load()` 和 `register()` 两步**。也就是说**插件导入失败**与**插件注册逻辑抛异常**被同等对待——两者都不该拖垮启动。
3. **错误日志带 `ep.name` 与 `ep.value`**（后者是 `"module:callable"` 路径），于是运维能定位到底是哪个包、哪个入口点坏了。
4. **`continue` 而不是 `raise`**，且返回值只包含成功的名字——**调用方可以据此知道哪些插件生效了**。
5. **`return []` 在幂等短路时返回空列表**，而不是返回上次的结果。这有点反直觉（调用方无法区分「没加载」与「已加载过」），但因为返回值主要用于日志/测试，这个简化是合理的。

**加载时机是硬性的，而且注释在调用点解释了原因。**

```
# Discover third-party parser engines (``lightrag.parsers`` entry points)
# BEFORE validating routing rules, so LIGHTRAG_PARSER may reference them.
load_third_party_parsers()
...
validate_parser_routing_config()
```

**顺序不能反**：`validate_parser_routing_config` 会用 `supported_parser_engines()` 校验规则里的引擎名；若插件还没注册，一条「`myfmt:myengine`」的规则会被判成「不支持的引擎」，而实际上插件是好的。

插件的文档字符串还交代了「每个入口点必须是一个零参数可调用对象，自己完成 `register_parser` 调用」，以及导入要便宜：「The callable should be import-cheap: defer the parser implementation import to the ``ParserSpec.impl`` string (the registry already loads it lazily).」——**插件的注册函数不该 import 它自己的解析器实现**，否则「查一下有哪些引擎」就会把第三方重依赖拉进来，正好破坏 `ParserSpec` 存在的全部意义。

## 关键取舍

**源码事实：** `ParserSpec` 只装数据 + 两个零参数闭包（`endpoint_configured` / `endpoint_requirement`，仅读环境变量、不发网络请求），`impl` 是 `"module:Class"` **字符串**，只在 `get_parser` 里经 `importlib` 加载。

**源码事实：** `supported_parser_engines()` 只按 `user_selectable` 过滤（回答「哪些引擎名合法」），`available_engine_suffixes()` 还要求 `endpoint_configured()` 通过（回答「现在能处理什么后缀」）；后者是上传白名单与目录扫描的唯一来源。

**源码事实：** `suffixes` 用描述符 `_EnvExtendedSuffixes` 实现「每次访问现读环境」，理由是 `_REGISTRY` 在 import 时构造，而 `.env` 要到之后 `load_dotenv()` 才被加载（解析器 debug CLI 的路径就是这样）；代价是携带 `extra_suffixes_env` 的 spec 其 `__eq__`/`__hash__` 随环境变化，但 spec 从不被用作键或集合成员。

**源码事实：** 描述符在 `obj is None`（访问类属性）时抛 `AttributeError`，刻意让 `suffixes` 无法有 dataclass 隐式默认值——忘记传的注册者会拿到 `TypeError`，而不是一个静默匹配不到任何后缀的引擎。`__set__` 绕过 frozen 并强制 `frozenset`，防止注册者传 `set` 后被原地修改。

**源码事实：** 环境后缀解析先宽容归一（去空白、小写、去前导点、丢弃空 token），再把「永远不可能等于 `Path.suffix`」的条目归入 malformed 并上报；`malformed_env_suffixes()` 返回上报数据，`routing.validate_parser_suffix_env_vars()` 负责把它升级成 `ParserRoutingConfigError`——解析规则与上报共享同一份实现，避免漂移。

**源码事实：** `_INSTANCE_CACHE` 的键是 `(engine, impl)` 而非仅 `engine`，注释说明是为了让「同名重注册但换了实现」不会返回过期的缓存实例；代价是旧实例不会被清理。

**源码事实：** `parser_specs_snapshot()` 返回 `dict(_REGISTRY)` 的**浅**快照，用于在批处理期间冻结引擎集合；所有查询函数都接受可选的 `specs` 参数并经 `_table(specs)` 取表，于是「用快照还是实时表」由调用点决定，不需要锁。

**源码事实：** `_REGISTRY` 是模块级字面量；`legacy` 复用 `native` 的 `queue_group`（注释：本地无网络）；`native` 是「单引擎 + 按后缀二次分派」（docx/markdown 由 `native_dispatch` 挑）；`reuse` / `passthrough` 后缀为空且 `user_selectable=False`，但仍然可以被 `get_parser` 取到。

**源码事实：** `load_third_party_parsers` 用全局 `_loaded` 保证每进程只跑一次（`force=True` 供测试）；`except Exception` 同时覆盖 `ep.load()` 与插件的 `register()`，失败只记 error 日志（带 `ep.name` 与 `ep.value`）并跳过；返回成功加载的入口点名字。

**源码事实：** `load_third_party_parsers()` 必须在 `validate_parser_routing_config()` **之前**调用，否则引用第三方引擎的 `LIGHTRAG_PARSER` 规则会被误判为「不支持的引擎」。

**架构推论：** 注册表这种「一份手工维护的元数据表」，其正确性风险全部集中在**与实现的一致性**上。这里的两个对冲手段很值得记：一是把「部署侧可扩展」的部分做成环境变量（`extra_suffixes_env`），于是声明的基线可以保持保守而不牺牲灵活性；二是把「声明与实现不符」的典型场景（缺少 endpoint）做成闸门，让不一致在**使用点**而不是**声明点**被发现。

**架构推论：** 把「错误消息的内容」也做成数据（`endpoint_requirement()` 返回变量名或期望值），是这个设计里最容易被忽略的好处。它让「哪条规则因为缺什么而不可用」可以逐条拼出来，而不是在深层抛一个「endpoint not configured」然后靠人工回溯是哪个引擎。**任何配置校验，都应该能回答「缺哪一个具体的东西」。**

**架构推论：** 「现读环境」与「import 时快照」的冲突，本质是**配置的加载时机不受被配置模块的控制**。`_EnvExtendedSuffixes` 是一个针对性解法：把求值点推到访问时。通用规律是——**任何在 import 期求值的配置，都要先确认它依赖的东西（`.env`、配置文件、命令行）在那个时刻是否已经就绪**；不确定就把求值推迟，并把这个推迟写进注释，否则下一个人会「优化」成构造时求值。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/parser/registry.py` | `ParserSpec` | 单引擎的轻量元数据：身份、能力、闸门、实现路径字符串 |
| `lightrag/parser/registry.py` | `_EnvExtendedSuffixes` | `suffixes` 的描述符：声明基线 ∪ 环境变量扩展，每次访问现算 |
| `lightrag/parser/registry.py` | `_parse_env_suffixes` / `malformed_env_suffixes` | 环境后缀的宽容归一与异常条目收集 |
| `lightrag/parser/registry.py` | `_mineru_endpoint_configured` / `_mineru_endpoint_requirement` | 按 `MINERU_API_MODE` 判定可用性并给出缺什么 |
| `lightrag/parser/registry.py` | `_REGISTRY` | 五个内建引擎的字面量表 |
| `lightrag/parser/registry.py` | `register_parser` | 注册或覆盖一个 spec（同名覆盖） |
| `lightrag/parser/registry.py` | `get_parser` | 唯一的实现加载点：`importlib` + `(engine, impl)` 键的实例缓存 |
| `lightrag/parser/registry.py` | `parser_specs_snapshot` | 批处理用的浅快照，冻结引擎集合 |
| `lightrag/parser/registry.py` | `supported_parser_engines` | 用户可选引擎名集合（不含 endpoint 闸门） |
| `lightrag/parser/registry.py` | `available_engine_suffixes` | 当前可用后缀并集（含 endpoint 闸门），上传白名单的唯一来源 |
| `lightrag/parser/registry.py` | `suffix_capabilities` / `engine_endpoint_configured` / `engine_endpoint_requirement` | 单引擎的能力与闸门查询 |
| `lightrag/parser/plugins.py` | `load_third_party_parsers` | `lightrag.parsers` entry point 的幂等加载，单插件失败不致命 |
| `lightrag/parser/plugins.py` | `ENTRY_POINT_GROUP` | 第三方插件的入口点组名 |

## 参考

- [lightrag 源码：parser/registry.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/parser/registry.py)
- [lightrag 源码：parser/plugins.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/parser/plugins.py)
- [Python 文档：`importlib.metadata` entry points](https://docs.python.org/3/library/importlib.metadata.html#entry-points)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

注册表对应主核里最接近的东西是**存储引擎/编解码器的插件表**。四条经验。

**第一，「能力数据」与「实现」必须分层，且判据是「哪条路径不能承担依赖」。** 这里的硬约束是「查后缀不能 import httpx」——主核里同类的约束是「列出支持的压缩算法不能加载 libzstd」。一旦违反，最热的管理接口会变成最重的接口。判断方法：**找出所有「在还没有真正处理数据之前就要回答」的问题，它们的数据必须能被零依赖读到。**

**第二，手工维护的第二真相必须配两个对冲：可扩展的声明、使用点的闸门。** `ParserSpec` 里的后缀列表就是一份可能与真实能力脱节的声明；解药是 `extra_suffixes_env`（部署侧补）与 `endpoint_configured`（用的时候再验）。主核里凡是「注册时声明能力、运行时才知道是否可用」的场景（插件声明支持的页大小、编解码器声明支持的格式变体）都该照这个结构：**声明保守，扩展靠配置，真实可用性在使用点验。**

**第三，「配置的加载时机不受被配置模块控制」是一个普遍陷阱。** `_REGISTRY` 在 import 时构造，而 `.env` 还没加载——这个 bug 的表现极其难查（配置明明写了，行为却像没配）。主核里凡是「模块级常量读配置」的地方都要问同一个问题。解法就一种：**把求值点推到访问时，并在注释里写下为什么不能提前**。

**第四，插件加载失败必须降级而非中止，但必须留下可定位的痕迹。** 这里的 `except Exception` + 带 `ep.name`/`ep.value` 的 error 日志是正确的形状。主核做插件扫描时同理：一个第三方扩展的导入错误不该让整个服务起不来，但也绝不能让运维看不出是哪个扩展坏了。与之配套的是**顺序约束**——插件必须在「校验引用插件名的配置」之前加载，这条顺序一旦搞反，症状是「插件明明是好的，配置却说引擎不存在」。
