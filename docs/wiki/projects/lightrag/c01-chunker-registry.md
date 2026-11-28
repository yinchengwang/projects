---
project: lightrag
feature: chunker-registry
title: 分块器注册与上下文传递
page_type: feature
schema_version: 1
refs:
  - path: lightrag/chunker/registry.py
    symbols: [ChunkerSpec, register_chunker, invoke_chunker]
  - path: lightrag/chunker/registry.py
    symbols: [accepts_chunking_context, selectable_chunker_names]
  - path: lightrag/chunker/plugins.py
    symbols: [load_and_resolve_chunker]
---

# 分块器注册与上下文传递

## 一句话职责

让第三方分块器能在**不被 import 的前提下**登记自己（只登记 `module:attribute` 字符串），把「选哪个分块器」收窄成一个封闭集合里的名字而不是任意 import 路径，并在调用时用**显式标记**（而非试探调用）决定要不要传 `context=`。

## 机制是什么

分块器插件这一层要同时处理四件事，而它们彼此牵制：

1. **启动成本**。分块器实现可能 import 重型依赖（torch、sentence-transformers）。若插件发现阶段就把实现 import 进来，服务启动时间会被无关插件拖垮。所以 `ChunkerSpec.impl` 是一个**惰性引用**——`"module:attribute"` 字符串，只有真正被选中时才 import。
2. **选择面必须是封闭集合**。`CUSTOM_CHUNKER` 若允许写 import 路径（`"mypkg.mod:func"`），那它就变成了任意代码执行的入口。所以注册名必须匹配 `[a-z0-9][a-z0-9_-]{0,63}`，且显式拒绝含 `:`/`/`/`\`/`.` 的名字。
3. **同名冲突不可静默**。两个插件注册同一个名字时，后来的覆盖前面的。若就这样覆盖，用户会拿到一个来源不明的实现。所以冲突被记为 `_DUPLICATES`，且**冲突名不可选**。
4. **上下文是可选能力**。分块器可能需要文档标题、上一块尾部这类信息才能切好。但既有的六参数回调不能改签名——那会破坏所有现存插件。所以「要不要传 `context=`」必须是一个显式声明，而不是运行时探测。

第四条是这一页最有意思的地方：`invoke_chunker` 的文档明确写了**为什么不做签名探测**：

> Signature probing followed by a retry is deliberately avoided: a `TypeError` raised inside a callback is a real callback failure and must never cause the implementation to run a second time.

即「先试不传 context，捕获 `TypeError` 再传」看起来兼容性最好，但它会让**回调内部的真实 `TypeError`** 被误读成「这个回调不接受 context」，于是实现被**执行两次**。分块是有副作用的（可能消耗 LLM 调用、可能写缓存），重跑一次不能容忍。

## 怎么实现的

**`accepts_chunking_context` 用一个函数属性当标记，而这是整个机制的关键。**

```python
_CHUNKING_CONTEXT_MARKER = "__lightrag_accepts_chunking_context__"

def accepts_chunking_context(callback: Callable[..., Any]) -> Callable[..., Any]:
    """Mark a constructor-supplied callback as accepting ``context=``."""
    if not callable(callback):
        raise TypeError("chunking callback must be callable")
    setattr(callback, _CHUNKING_CONTEXT_MARKER, True)
    return callback

def callback_supports_context(callback: Callable[..., Any]) -> bool:
    return bool(
        getattr(callback, _CHUNKING_CONTEXT_MARKER, False)
        or getattr(callback, "supports_chunking_context", False)
        or getattr(callback, "accepts_chunking_context", False)
    )
```

四处：

1. **标记写在函数对象上**（`setattr`），所以它是**运行期属性**而不是类型信息。这意味着任何能拿到回调对象的人都能打这个标记——包括不想遵守约定的人。但这里的目标不是防护，是**表达能力**。

2. **文档强调「legacy six-argument callbacks remain indistinguishable from their historical invocation」** —— 不打标记就等于旧行为，一个字节都不变。所以这是纯增量：老插件不需要改，也不会有行为变化。

3. **文档还说：「The marker is read at dispatch time and is not inferred from a callback failure」** —— 明确排除探测。

4. **`callback_supports_context` 查三个属性**：`__lightrag_accepts_chunking_context__`（由装饰器打）、`supports_chunking_context`、`accepts_chunking_context`（供不想用装饰器的实现直接用类属性/函数属性声明）。所以有两条路：调装饰器，或自己设一个同名属性。

   注意后两个名字**没有前后下划线**，所以它们可能与业务的属性撞名。`__lightrag_...` 那个带双下划线前后缀，防撞名——但因为不是类内的 name mangling 场景（函数属性没有 mangling），所以它只是个命名约定。

**`invoke_chunker` 的三分支只有一个目的：让每个回调被调用恰好一次。**

```python
def invoke_chunker(callback, *args, context: ChunkingContext) -> Any:
    if isinstance(callback, ChunkerBinding):
        return callback(*args, context=context)
    if callback_supports_context(callback):
        return callback(*args, context=context)
    return callback(*args)
```

三处：

1. **`ChunkerBinding` 优先**：插件绑定对象自己知道 `spec.accepts_context`，所以直接把 `context` 传给它（它内部按 spec 决定是否继续传下去）。
2. **普通回调查标记**。
3. **都没有则只传六参数**。

`context` 在这个函数里是**必填关键字参数**（没有默认值）。所以调用方必须准备好 context——即使最终不被使用。这是一处刻意的浪费：让分派逻辑简单（不需要判断「有没有 context」），代价是即便 legacy 路径也要构造一个 `ChunkingContext`。

**`ChunkingContext` 六个字段，而 `sidecar_location` 的解析被单独抽成函数且有大量拒绝逻辑。**

```python
@dataclass(frozen=True, kw_only=True)
class ChunkingContext:
    doc_id: str
    file_path: str
    sidecar_location: str | None
    parse_format: str
    parse_engine: str | None
    process_options: str
```

文档里有一句很硬的约束：「Resolve `sidecar_location` with `resolve_sidecar_location`; never cast the URI to a `Path` by hand」。

`resolve_sidecar_location` 是一个**跨平台 URI 解析器**，而它的每个拒绝分支都对应一种真实的错误情形：

```python
if not location or location == utils_pipeline.SIDECAR_LOCATION_UNKNOWN:
    return None
parts = urlsplit(location)
if parts.scheme != "file":
    raise _sidecar_location_error(location, "only local file:// URIs can be resolved")
```

前半段：`None`/空串/未知哨兵都返回 `None`（「这份文档没有 sidecar」）。后半段：只接受 `file://`。

然后是四种平台错配的检查：

```python
windows = os.name == "nt"
if parts.netloc.lower() not in ("", "localhost"):
    if not windows:
        legacy = utils_pipeline._LEGACY_WINDOWS_AUTHORITY_RE.match(unquote(parts.netloc))
        raise _sidecar_location_error(location, _FOREIGN if legacy
            else f"file:// URI names remote host {parts.netloc!r}")
elif not windows and _URI_LITERAL_DRIVE.match(parts.path):
    raise _sidecar_location_error(location, _FOREIGN)
```

四处：

1. **`netloc` 非空非 `localhost` 时**：在 POSIX 上要么是「别的平台写的 Windows 盘符 URI」（判为 `_FOREIGN`），要么是真的远程主机（报主机的名字）。在 Windows 上则放行给下游的 reader 处理（注释：「the Windows reader accepts only the legacy drive form and returns None for any other authority」）。

2. **`_URI_LITERAL_DRIVE = re.compile(r"^/[A-Za-z]:")`**：路径里出现字面的 `/C:` 说明这是 Windows 写出来的 URI。注释解释了为什么 POSIX 写入者不会产生这种形态：「A POSIX writer percent-encodes ':' (``/C:`` becomes ``/C%3A``)」。**所以一个未编码的盘符标记就是「这份数据是别的平台写的」的指纹**。

3. **`_FOREIGN = "it is not an absolute local path on this platform"`** 是一句**统一的拒绝理由**，被用在四处。它的价值在于：这不是「URI 格式错」，而是「这条数据不属于这台机器」——用户看到它应该想到「我是不是在错误的平台上读这份数据」。

4. **解析之后还有两道**：`resolved is None` → 拒绝；`not resolved.is_absolute()` → 拒绝；Windows 上盘符以 `\\`/`//` 开头（UNC）→ 拒绝。

注意**解析完全委托给 `utils_pipeline.resolve_sidecar_uri`**：

```python
resolved = utils_pipeline.resolve_sidecar_uri(location)
```

文档说明理由：「Decoding is delegated to the pipeline's own sidecar reader, so a location never resolves to a different directory here than in the pipeline; this function only adds the refusals」。

**这是一处重要的设计**：`resolve_sidecar_location` 只负责「拒绝不该接受的」，不负责「解码」。所以它与管道的 reader **不可能出现分叉**——两者的解码逻辑是同一份代码，这里只在其上加一层守卫。若它自己实现一套 URI 解码，就必然会有「这里解出来的路径与管道解出来的不同」的 bug。

而 `from lightrag import utils_pipeline` 是**函数内 import**（注释：「Deferred: the pipeline helpers are heavy, and this module must stay cheap」）——因为本模块的定位是「import-cheap」，顶层引入重型依赖会破坏这个定位。

**`ChunkerSpec` 八个字段里两个是能力位，而默认值都在保守一侧。**

```python
@dataclass(frozen=True)
class ChunkerSpec:
    name: str
    impl: str
    version: str
    description: str
    executor_safe: bool = False
    accepts_context: bool = False
```

文档说明了两个能力位的含义与默认值理由：

- **`executor_safe`**：「opts a synchronous, thread-safe implementation into the bounded chunking executor」。默认 `False` 意味着**不走线程池**，就在事件循环里跑。所以默认是「假设实现很快」——一个慢实现会阻塞事件循环。用户必须显式声明「我线程安全」才获得卸载。

  注意这里把「线程安全」与「要不要卸载」绑成一个位。这是合理的：卸载到线程池的前提是线程安全。

- **`accepts_context`**：「opts into the keyword-only `ChunkingContext` argument」。默认 `False` 保持六参数契约。

而文档最后一句是扩展性承诺：「Future optional fields can extend this spec without changing entry points」——因为 spec 是 `kw_only` 之外的普通 dataclass，新增带默认值的字段不破坏既有构造调用。

**`register_chunker` 是一个七段校验的函数，而每段校验都对应一种真实的误注册。**

```python
if not isinstance(spec.name, str) or not _NAME.fullmatch(spec.name):
    raise ValueError("chunker name must match [a-z0-9][a-z0-9_-]{0,63}")
if spec.name in _RESERVED:
    raise ValueError(f"reserved chunker name: {spec.name!r}")
if not isinstance(spec.impl, str) or not spec.impl.strip():
    raise ValueError(f"chunker {spec.name!r} requires a lazy implementation reference")
if not isinstance(spec.version, str):
    raise ValueError(f"chunker {spec.name!r} version must be a string")
if (not isinstance(spec.description, str) or not spec.description.strip()
    or any(c in spec.description for c in "\r\n")):
    raise ValueError(f"chunker {spec.name!r} requires a one-line description")
if not isinstance(spec.executor_safe, bool):
    raise ValueError(f"chunker {spec.name!r} executor_safe must be a bool")
if not isinstance(spec.accepts_context, bool):
    raise ValueError(f"chunker {spec.name!r} accepts_context must be a bool")
```

四处值得单独看：

1. **`_RESERVED` 九个名字**：

```python
_RESERVED = frozenset({
    "fixed_token", "recursive_character", "semantic_vector", "paragraph_semantic",
    "f", "r", "v", "p", "c",
})
```

   前四个是四个内置分块器的完整名，后五个是它们的**单字母别名**。所以内置分块器占住了两个名字空间（全名与单字母）。这条规则防的是「插件注册一个与内置同名的分块器」——若允许，`CUSTOM_CHUNKER=recursive_character` 的行为会依赖插件安装顺序。

2. **`description` 必须单行**（拒绝 `\r`/`\n`）。理由不难推：描述会进启动日志的一行摘要（见 `log_chunker_selection`），多行会破坏日志的可读性与 grep 能力。

3. **`executor_safe` 与 `accepts_context` 必须是真 `bool`**，不接受「真值」（如 `1`、`"yes"`）。这与 Python 的宽松惯例相反，但它防的是「插件作者写了 `"false"` 结果是真」这类错误。

4. **`version` 只校验是字符串，不校验格式**。文档说「`version` is an opaque author-supplied observation」——即框架不用它做比较，只当诊断信息。所以不解析。

**冲突处理：记下但不拒绝，且让冲突名不可选。**

```python
resolved_origin = _registration_origin or origin or spec.impl
previous = _REGISTRY.get(spec.name)
if previous is not None:
    _DUPLICATES.add(spec.name)
    logger.error("[chunker-plugins] duplicate chunker %r: %s -> %s",
                 spec.name, previous.origin, resolved_origin)
_REGISTRY[spec.name] = _Registration(spec, resolved_origin)
```

两处：

1. **后来的覆盖前面的**（`_REGISTRY[spec.name] = ...`），但 `_DUPLICATES` 记住了这个名字。所以注册表里存的是**后注册者**，而这个名字的**可选择性被永久关闭**。
2. **`logger.error` 而不是 `warning`**——因为这是插件冲突，需要运维注意。

而 `selectable_chunker_names` 与 `registered_chunker_names` 的差别就是这一点的体现：

```python
def registered_chunker_names() -> tuple[str, ...]:
    return tuple(sorted(_REGISTRY))

def selectable_chunker_names() -> tuple[str, ...]:
    return tuple(sorted(name for name in _REGISTRY if name not in _DUPLICATES))
```

文档把两者的分工写清楚了：「`registered_chunker_names` includes names that collided, so the startup summary can show what a provider actually registered. Use `selectable_chunker_names` for anything that answers 'what may CUSTOM_CHUNKER be set to'」，而 `selectable_chunker_names` 的理由是：

> A duplicated name is registered but unselectable -- `resolve_chunker` refuses it -- so offering it as a choice sends the operator to a value that is guaranteed to fail startup.

**这是一处很好的「诊断面 vs 操作面」分离**：日志要显示「实际发生了什么」（含冲突），而给用户的候选清单只显示「设了能用的」。

**`_plugin_registration` 是一个「部分失败全部回滚」的上下文管理器。**

```python
@contextmanager
def _plugin_registration(origin: str) -> Iterator[None]:
    global _registration_origin
    previous_origin = _registration_origin
    previous = _REGISTRY.copy()
    previous_duplicates = _DUPLICATES.copy()
    _registration_origin = origin
    try:
        yield
    except Exception:
        _REGISTRY.clear()
        _REGISTRY.update(previous)
        _DUPLICATES.clear()
        _DUPLICATES.update(previous_duplicates)
        raise
    finally:
        _registration_origin = previous_origin
```

四处：

1. **异常时恢复两个全局结构的快照**。所以「一个插件注册了三个名字，第三个时抛错」的结果是**三个都不留**。

   为什么需要这个？因为一个插件的注册函数通常是「注册 A、注册 B、注册 C」顺序执行。若 C 抛错，A 与 B 已经进了注册表——于是这个插件处于「半注册」状态，而它的导入错误又被记进了 `_failures`。用户看到「插件失败」但注册表里还有它的两条。回滚让「失败」的语义干净。

2. **`_registration_origin` 是模块级全局**，用 `previous_origin` 保存与 `finally` 恢复——所以支持嵌套。

3. **`_REGISTRY.clear()` + `update(previous)`** 而不是 `_REGISTRY = previous`。这是必要的：`register_chunker` 里持有的是模块级名字的引用，若重新绑定，`_DUPLICATES` 那边（或任何局部缓存）会与新的字典脱钩。用 in-place 修改保证同一对象。

   注意 `previous = _REGISTRY.copy()` 是**浅拷贝**，但 `_Registration` 是 `frozen=True`，所以浅拷贝足够安全。

4. **`origin` 参数会被存进 `_Registration`**，所以每条注册都记得自己来自哪个发行包。这对冲突错误消息（「see both origins in the error log」）是必需的。

**`resolve_chunker` 是「名字 → 可调用对象」的唯一入口，而它的校验顺序是「先便宜后昂贵」。**

```python
if name is None or name == "":
    return None
if not isinstance(name, str):
    raise ValueError("CUSTOM_CHUNKER must be a registered chunker name")
if any(c in name for c in ":/\\."):
    raise ValueError("CUSTOM_CHUNKER: import paths are not supported; install a plugin using lightrag.chunkers entry points and select its registered name")
if not _NAME.fullmatch(name):
    raise ValueError("CUSTOM_CHUNKER must match [a-z0-9][a-z0-9_-]{0,63}")
if name in _DUPLICATES:
    raise ValueError(f"CUSTOM_CHUNKER selects duplicate name {name!r}; remove the conflicting registration (see both origins in the error log)")
registration = _REGISTRY.get(name)
if registration is None:
    raise ValueError(f"CUSTOM_CHUNKER: unknown chunker {name!r}; selectable names: {', '.join(selectable_chunker_names()) or '(none)'}. Check plugin discovery errors for a failed provider.")
```

五处：

1. **`None` 或 `""` 返回 `None`**，表示「不用自定义分块器」。注意空串与 `None` 等价——所以配置里写空值等于没配。

2. **显式拒绝 import 路径**（含 `:`、`/`、`\`、`.` 的名字）。这是**安全边界的核心一句**：`CUSTOM_CHUNKER` 若接受 `"os:system"` 就是任意代码执行。错误消息直接给出替代方案（用 entry point 注册再选名字）。这与 b03（haystack 的反序列化白名单）是同一类纪律——**按名字查找可以，按路径 import 不行**。

3. **`_DUPLICATES` 检查在 `_REGISTRY.get` 之前**。顺序是必需的：冲突名在注册表里是存在的（后注册者赢了），若先查存在性就会放行。所以「存在」与「可选」是两件事，必须先判后者。

4. **错误消息列出所有可选名字**，且 `or '(none)'` 兜住空集合。末尾还说「Check plugin discovery errors for a failed provider」——把用户导向另一个诊断入口。

5. **注意 `.` 被禁是双重保险**：`_NAME` 的正则本身不允许 `.`，所以那句 `any(c in name for c in ":/\\.")` 只用来看**是不是在尝试写 import 路径**，以便给出更准确的错误消息。这个区分很实用：「你写了个路径」比「你的名字不合法」信息量大得多。

**import 与签名校验是最后两步，而它们都在 `try` 里包着。**

```python
spec = registration.spec
try:
    module, separator, attribute = spec.impl.partition(":")
    if not module or not separator or not attribute:
        raise ValueError("expected module:attribute")
    implementation = importlib.import_module(module)
    for part in attribute.split("."):
        implementation = getattr(implementation, part)
except Exception as exc:
    raise ValueError(f"cannot load selected chunker {name!r} ({spec.impl}, {registration.origin}): {exc}") from exc
```

三处：

1. **`partition(":")` 而不是 `split(":")`** —— 只切第一个冒号，且返回三元组（`separator` 为空表示没有冒号）。所以 `"mod:Attr"` 与 `"mod:"` 与 `"mod"` 能被区分。

2. **`attribute.split(".")` 逐段 `getattr`** —— 所以支持 `"module:Class.method"` 这样的嵌套属性。这与 b04（haystack 的 `deserialize_callable`）同样的手法。

3. **`except Exception` 包住整个 import + getattr**，并重抛成 `ValueError`，消息里带 `spec.impl` 与 `registration.origin`。**这是关键**：错误消息同时给出「想加载什么」与「谁注册的」，所以运维能直接定位到该卸载哪个包。

   注意这里是**唯一执行 import 的地方**，所以整个「不 import 实现」的承诺在这里兑现：注册阶段只存字符串，选择阶段才 import 且只 import 被选中的那一个。

**签名校验用 `signature.bind` 而不是数参数。**

```python
try:
    signature = inspect.signature(implementation)
except (ValueError, TypeError):
    pass  # Some native callables expose no signature; do not invent a fence.
else:
    if spec.accepts_context:
        try:
            signature.bind(*([None] * 6), context=None)
        except TypeError as exc:
            raise ValueError(f"selected context-aware chunker {name!r} must accept six positional arguments plus keyword-only context: {exc}") from exc
    else:
        try:
            signature.bind(*([None] * 6))
        except TypeError as exc:
            raise ValueError(f"selected chunker {name!r} must accept six positional arguments: {exc}") from exc
```

四处：

1. **`bind` 而不是比较参数个数** —— `bind` 会考虑默认值、`*args`、`**kwargs`、keyword-only。所以「六参数 + `**kwargs`」也能通过（因为 bind 接受），而「五参数」不能。

2. **`([None] * 6)` 用 `None` 当占位** —— `bind` 只检查形状不检查类型，所以 `None` 足够。

3. **`accepts_context=True` 时额外要求 `context=None` 能绑上** —— 即必须有 `context` 这个关键字参数（或 `**kwargs`）。

4. **`inspect.signature` 失败时 `pass`**，注释写得很硬：「Some native callables expose no signature; do not invent a fence」——即「无法检查就不检查」，而不是「无法检查就拒绝」。这与 b01 里「`sender is Any → 假`」的保守方向相反，但理由不同：这里的判据是「拒绝一个合法的 native 实现比放行一个参数错的实现更糟」。

**异步实现不允许请求线程池卸载。**

```python
if spec.executor_safe and (
    inspect.iscoroutinefunction(implementation)
    or inspect.iscoroutinefunction(getattr(implementation, "__call__", None))
):
    raise ValueError(f"selected async chunker {name!r} cannot request executor offload")
```

两处：

1. **同时检查函数本身与 `__call__`** —— 后者覆盖可调用对象（实例）。所以一个 `async def __call__` 的类实例也会被拦。

2. **理由**：`executor_safe` 的语义是「同步、线程安全，可以丢到线程池」。而一个 `async def` 丢到线程池只会得到一个**未被 await 的协程**——它不执行任何东西。所以这个组合是**必然错误**，提前拒绝比运行期静默失效好。

这与 C 组其他页面会看到的「同步/异步能力位」是同一类问题：能力声明与实现形态必须一致。

**`ChunkerBinding` 不是 dataclass，而文档解释了为什么。**

```python
class ChunkerBinding:
    """Callable injected at construction, with non-authoritative provenance.

    Not a dataclass: LightRAG's ``asdict`` snapshots must retain a callable,
    rather than recursively turning the callback into a metadata dictionary.
    """
```

**这是一个很具体的理由**：若它是 dataclass，`dataclasses.asdict()` 会递归把 `implementation` 拆成一个（空的）字典——因为函数不是 dataclass 时 `asdict` 会保留它，但 `spec` 是 dataclass 会被展开。所以「不是 dataclass」保证 `asdict` 遇到它时不展开，保留成一个可调用对象。

而 `spec` 与 `implementation` 是**普通属性**（不是 dataclass 字段），所以 `asdict` 完全看不见它们。

`__call__` 的签名值得逐行读：

```python
def __call__(self, tokenizer, content, split_by_character, split_by_character_only,
             chunk_overlap_token_size, chunk_token_size, *,
             context: ChunkingContext | None = None) -> Any:
    args = (tokenizer, content, split_by_character, split_by_character_only,
            chunk_overlap_token_size, chunk_token_size)
    kwargs = {"context": context} if self.spec.accepts_context else {}
    if self.spec.executor_safe:
        from lightrag.utils import run_in_chunking_executor
        return run_in_chunking_executor(self.implementation, *args, **kwargs)
    return self.implementation(*args, **kwargs)
```

四处：

1. **`ChunkerBinding` 自己的签名总是带 `context`**（即使是 legacy 实现），因为它要能被 `invoke_chunker` 无条件地以 `context=` 调用。而它内部再按 `spec.accepts_context` 决定要不要往下传。

   **所以适配发生在这一层**：外部看到的是统一的九参数形态，内部是六或七参数。

2. **`run_in_chunking_executor` 是函数内 import** —— 因为 `lightrag.utils` 很重，而本模块要保持 import-cheap。

3. **`executor_safe` 时返回的是协程**（`run_in_chunking_executor` 是 `async def`）。所以 `__call__` 的返回类型是 `Any`——可能是一个分块结果，也可能是一个可等待对象。**这个不一致由调用方处理**（它必须知道自己在 async 上下文里）。

   而 `resolve_chunker` 已经保证了「`executor_safe` 的实现不是 async」，所以「返回协程」只会是因为走了执行器。

4. **`chunking/executor 的界限来自 `CHUNKING_SUBMIT_LIMIT` 信号量**（在 `run_in_chunking_executor` 里）。文档说明了为什么需要单独的界限：

> Bounded like any other submission: the submitters include public SDK entry points (``ainsert_custom_kg``, ``ainsert_custom_chunks``) gated by neither ``max_parallel_insert`` nor the pipeline's busy flag, so "the pipeline limits concurrency" is not a ceiling that actually holds here.

   即「管道的并发上限」不覆盖 SDK 直调路径，所以分块执行器必须有自己的上限。这是一处「以为有保护但实际没有」的实例——**并发上限必须加在真正的入口处，而不是加在「通常的调用路径」上**。

**`chunker_identity` 只做诊断，且显式标注 `authoritative: False`。**

```python
def chunker_identity(callback) -> dict[str, Any] | None:
    if isinstance(callback, ChunkerBinding):
        return {"name": callback.spec.name, "version": callback.spec.version, "authoritative": False}
    return None
```

三处：

1. **`authoritative: False`** 是一个显式字段，文档说「Describe the bound callback, never use persisted identity for selection」。所以它随快照落盘后，读回来也**不能**用来决定「用哪个分块器」——只能用来显示「当时用的是哪个」。

   理由能推出来：插件可能被卸载或改版，所以旧快照里的名字未必还能解析；而若用它来选，就会出现「配置是对的但加载失败」这种难查的问题。

2. **非 `ChunkerBinding` 时返回 `None`**（构造函数直接传的回调没有身份）。

3. **不含 `accepts_context`/`executor_safe`** —— 所以身份只记「谁」不记「能力」，能力永远从当前注册表读。

**`log_chunker_selection` 的启动摘要里有一句很长的话，而它说明了一个用户几乎不会预料到的后果。**

```python
logger.info(
    "[chunker-plugins] registered=%s; selected=%s%s",
    registered,
    selected.spec.name if selected else "(none)",
    "; serves C and no-selector inserts, which therefore forgo source-span"
    " sidecar backfill exactly as a constructor-supplied callback does"
    if selected else "; built-in callback unchanged",
)
```

**「选中的分块器会同时服务 C 和「没有显式选择器的插入」，而这些路径因此放弃 source-span sidecar 回填」** —— 即：一旦设置 `CUSTOM_CHUNKER`，那些本来走内置分块器、能回填原文区间的插入路径，也会改用你的插件，从而失去区间信息。

这是一个**全局副作用**，而且它的方向是「功能变少」。把它写进启动日志（而不是文档）是对的——因为它只在「设了 CUSTOM_CHUNKER」时发生，而那一刻运维正在看日志。

`registered` 那个列表每项含四个字段（`name`/`version`/`origin`/`description`），用 `sorted(_REGISTRY.items())` 保证顺序稳定。

**`plugins.py` 的发现机制与失败处理。**

```python
ENTRY_POINT_GROUP = "lightrag.chunkers"
_loaded = False
_failures: list[tuple[str, str, str]] = []

def _origin(ep) -> str:
    dist = getattr(ep, "dist", None)
    return f"{getattr(dist, 'name', 'unknown distribution')}@{getattr(dist, 'version', '?')} {ep.name}={ep.value}"
```

五处：

1. **entry point group 是 `lightrag.chunkers`**，而每个 entry point 解析到的是**一个零参数函数**（调 `register_chunker`），不是实现本身。文档强调「Each entry point resolves to a zero-argument function calling `register_chunker(ChunkerSpec(...))`, never the implementation itself」。

   所以 import 的只有「注册函数」那个轻量模块——重型实现在 `module:attribute` 里，等到被选中才 import。

2. **`_loaded` 是一次性标志**，且 `_discover()` 在已加载时返回 `[]`。

3. **`_origin(ep)` 拼出「发行包名@版本 ep名=值」**，缺信息时有 `'unknown distribution'` / `'?'` 兜底。这个字符串会进每条注册的 `origin`，也是冲突错误消息里的关键信息。

4. **`sorted(entry_points(...), key=_origin)`** —— 按 origin 排序保证发现顺序确定。这很重要：冲突时「谁覆盖谁」取决于注册顺序，排序让结果可复现。

5. **失败记进 `_failures`（三元组：名字、来源、错误）并 `continue`**，不中断其他插件的发现。所以「一个插件坏了」不会让服务起不来——只会让它的分块器不可用。

`load_and_resolve_chunker` 的两步顺序是关键：

```python
def load_and_resolve_chunker(name: str | None) -> ChunkerBinding | None:
    _discover()
    try:
        selected = resolve_chunker(name)
    except ValueError:
        _report_failures(f"CUSTOM_CHUNKER={name!r} could not be activated; startup aborted")
        raise
    _report_failures(...)
    return selected
```

四处：

1. **先 `_discover()` 再 `resolve_chunker`** —— 必须先注册再解析。

2. **`resolve_chunker` 抛错时先把失败报告出去再重抛**，报告的措辞是「could not be activated; startup aborted」。这个顺序是刻意的：用户需要**同时**看到「选的名字无效」与「哪些插件在发现阶段就失败了」——因为后者往往就是前者原因（选的分块器来自一个加载失败的插件）。

3. **成功时的措辞也区分两种情形**：选中了 → 「selected chunker ... validated; C and no-selector chunking remain available」；没选（`CUSTOM_CHUNKER` 未设）→ 「CUSTOM_CHUNKER is unset; built-in callback and existing C admission/fallback are unchanged」。

4. **`_report_failures` 只打印，不抛** —— 所以「一个无关插件坏了」不会阻止服务启动。

而 `load_third_party_chunkers` 是给 SDK 用户用的（不选回调，只发现）：

```python
def load_third_party_chunkers() -> list[str]:
    if _loaded:
        return []
    loaded = _discover()
    _report_failures("discovery only; no chunker selection has been validated")
    return loaded
```

注意 **`if _loaded: return []`** 在 `_discover()` 之前就判了——所以第二次调用返回空列表（而不是上次的结果）。文档写「Discover once」，所以这个「返回空」的语义是「这次没新加载东西」而不是「没有插件」。这是一个容易误读的返回值。

## 关键取舍

**源码事实：** `ChunkerSpec.impl` 是惰性 `"module:attribute"` 字符串，注册阶段**不 import 实现**，只有 `resolve_chunker` 选中的那一个才被 import——这是本层的核心承诺。

**源码事实：** `invoke_chunker` 明确避免「签名探测 + 重试」，文档理由是「a `TypeError` raised inside a callback is a real callback failure and must never cause the implementation to run a second time」。

**源码事实：** 上下文能力靠函数属性标记（`__lightrag_accepts_chunking_context__`），`callback_supports_context` 还接受 `supports_chunking_context`/`accepts_chunking_context` 两个属性名作为替代声明；默认（无标记）等于历史六参数行为。

**源码事实：** `resolve_chunker` 显式拒绝含 `:`/`/`/`\`/`.` 的名字（import 路径），并给出「用 entry point 注册再选名字」的替代方案；名字必须匹配 `[a-z0-9][a-z0-9_-]{0,63}`。

**源码事实：** `_RESERVED` 九个名字 = 四个内置分块器全名 + 它们的单字母别名（`f`/`r`/`v`/`p`/`c`）。

**源码事实：** 同名冲突不拒绝注册但记为 `_DUPLICATES`，冲突名**注册表里存在但不可选**；`registered_chunker_names`（含冲突，供诊断）与 `selectable_chunker_names`（排除冲突，供操作）分离，后者文档理由：「offering it as a choice sends the operator to a value that is guaranteed to fail startup」。

**源码事实：** `_DUPLICATES` 检查在 `_REGISTRY.get` 之前——顺序必需，否则冲突名会被存在性检查放行。

**源码事实：** `register_chunker` 七段校验，其中 `description` 必须单行（拒绝 `\r`/`\n`）、两个能力位必须是真 `bool`、`version` 只校验是字符串（不解析格式，仅作诊断）。

**源码事实：** `_plugin_registration` 在插件注册抛错时**回滚两个全局结构的快照**（in-place `clear()`+`update()` 而非重绑定），所以一个插件注册了三个名字、第三个抛错时三个都不留。

**源码事实：** 签名校验用 `signature.bind(*([None]*6), context=None)` 而非比较参数个数，所以 `**kwargs`、默认值、keyword-only 都被正确考虑；`inspect.signature` 失败时**不检查**（注释：「Some native callables expose no signature; do not invent a fence」）。

**源码事实：** `executor_safe` + `async def`（含 `async __call__`）的组合被提前拒绝，因为把协程丢到线程池只会得到一个未 await 的协程。

**源码事实：** `ChunkerBinding` 刻意**不是 dataclass**，理由是会进 `asdict` 快照——dataclass 会被递归展开成元数据字典，而非 dataclass 保留为可调用对象。它的 `__call__` 签名**总是**带 `context=`（适配在这一层完成），`executor_safe` 时返回协程。

**源码事实：** `chunker_identity` 返回 `{"name", "version", "authoritative": False}`——身份只用于诊断，文档明确「never use persisted identity for selection」。

**源码事实：** `log_chunker_selection` 警告一个全局副作用：设置 `CUSTOM_CHUNKER` 后该分块器同时服务「C」与「无选择器的插入」，因此这些路径**放弃 source-span sidecar 回填**。

**源码事实：** `resolve_sidecar_location` 把解码**完全委托**给 `utils_pipeline.resolve_sidecar_uri`，自己只加拒绝：非 `file://` scheme、非本地 authority、POSIX 上出现字面盘符（`^/[A-Za-z]:`，因为 POSIX 写入者会百分号编码冒号）、非绝对路径、Windows 上的 UNC 路径。

**源码事实：** `_FOREIGN = "it is not an absolute local path on this platform"` 作为统一拒绝理由，语义是「这份数据不属于这台机器」而非「URI 格式错」。

**源码事实：** 插件发现按 `_origin` 排序保证「冲突时谁覆盖谁」可复现；发现失败只记 `_failures` 并 `continue`，不阻止启动；`load_third_party_chunkers` 在已加载时返回 `[]`（语义是「这次没新加载」而非「没有插件」）。

**架构推论：** 这一层的三条纪律可以总结成一句话：**把「能执行什么代码」从数据里拿掉**。注册名是封闭集合、`CUSTOM_CHUNKER` 拒绝 import 路径、身份字段标 `authoritative: False`、实现只 import 被选中的那一个——每条都在缩小「配置能触达的代码面」。这与 haystack 反序列化白名单是同一族手法，差别在于这里连「名字」都要求匹配正则且内置名被占住。

**架构推论：** 「不做签名探测」这条是本页最重要的工程判断。探测版看起来更友好（插件不用改），但它把「回调内部抛 `TypeError`」与「回调不接受这个参数」混为一谈，而两者在分块场景里的代价完全不同：前者是 bug 该暴露，后者是兼容性问题。用一次显式标记换掉这个歧义，是**把不可判定的事变成可声明的**。

**架构推论：** 「注册表存在但不可选」这个中间状态值得单独记。通常的做法是「冲突时拒绝后注册者」或「冲突时后者覆盖」，两者都让注册表与可选集一致。这里让它们**故意不一致**，好处是启动摘要能显示「这个插件确实注册了东西」（诊断价值），代价是任何读 `_REGISTRY` 的代码都必须知道还有 `_DUPLICATES` 这一层。两个访问器（`registered_` / `selectable_`）就是为这个不一致提供的两个正确入口。

**架构推论：** `resolve_sidecar_location` 的「只加拒绝、不解码」是一条可以推广的模式：**当同一份数据要被两个模块解析时，让其中一个成为唯一的解码者，另一个只做守卫**。这样两个模块的解析结果永远一致，而守卫逻辑可以独立演进。若各解一次，就会出现「管道能读、这里读不到」或更糟的「两边解出不同目录」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/chunker/registry.py` | `ChunkingContext` | 六个文档级上下文字段（`frozen`、`kw_only`） |
| `lightrag/chunker/registry.py` | `resolve_sidecar_location` | URI → 本地目录，只加拒绝不解码 |
| `lightrag/chunker/registry.py` | `_sidecar_location_error` / `_FOREIGN` | 统一拒绝消息构造 |
| `lightrag/chunker/registry.py` | `accepts_chunking_context` | 给回调打上下文能力标记 |
| `lightrag/chunker/registry.py` | `callback_supports_context` | 查三种属性判能力 |
| `lightrag/chunker/registry.py` | `invoke_chunker` | 三分支分派，保证回调只执行一次 |
| `lightrag/chunker/registry.py` | `ChunkerSpec` | 六个字段的注册元数据（含两个能力位） |
| `lightrag/chunker/registry.py` | `_RESERVED` | 九个被占用的名字 |
| `lightrag/chunker/registry.py` | `register_chunker` | 七段校验 + 冲突记录 |
| `lightrag/chunker/registry.py` | `registered_chunker_names` / `selectable_chunker_names` | 诊断面 vs 操作面 |
| `lightrag/chunker/registry.py` | `_plugin_registration` | 部分失败回滚的上下文管理器 |
| `lightrag/chunker/registry.py` | `ChunkerBinding` | 非 dataclass 的绑定对象，`__call__` 做能力适配 |
| `lightrag/chunker/registry.py` | `resolve_chunker` | 名字 → 可调用对象的唯一入口（拒绝 import 路径） |
| `lightrag/chunker/registry.py` | `chunker_identity` | 非权威身份（`authoritative: False`） |
| `lightrag/chunker/registry.py` | `log_chunker_selection` | 启动摘要 + 回填副作用的警告 |
| `lightrag/chunker/plugins.py` | `ENTRY_POINT_GROUP` | `lightrag.chunkers` |
| `lightrag/chunker/plugins.py` | `_discover` | 排序发现 + 失败隔离 |
| `lightrag/chunker/plugins.py` | `load_third_party_chunkers` | 仅发现（SDK 用） |
| `lightrag/chunker/plugins.py` | `load_and_resolve_chunker` | 先发现再解析，失败时同时报告发现错误 |
| `lightrag/utils.py` | `run_in_chunking_executor` | 带独立信号量上限的线程池卸载 |

## 参考

- [lightrag 源码：chunker/registry.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/chunker/registry.py)
- [lightrag 源码：chunker/plugins.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/chunker/plugins.py)
- [Python 文档：importlib.metadata 的 entry points](https://docs.python.org/3/library/importlib.metadata.html#entry-points)
- [PEP 621：项目元数据中的 entry points](https://peps.python.org/pep-0621/)

## 与主核 / AI 的关系

「按名字注册、按名字选择、拒绝按路径 import」这一套与主核（多模态数据库内核）的**扩展加载**是同一类问题的两种解法。主核的 `LOAD 'path/to/ext.so'` 接受文件路径，靠「超级用户权限 + 限定库目录」防护；这里则从根上不接受路径，只接受一个封闭集合里的名字。差别在威胁模型：主核的调用者是 DBA（可信但需授权），这里的配置可能来自环境变量或被运维脚本生成。

第二处对照是 `resolve_sidecar_location` 的「唯一解码者」原则，与主核的**编码转换**设计一致。主核的字符集转换只在一个地方做（`pg_do_encoding_conversion`），别处只传「源编码/目标编码」两个标识；若某个子系统自己实现一套转换，就会出现「同一个值在两处被解成不同字符串」。这里的 `resolve_sidecar_uri` 就是那个唯一解码者，而 `resolve_sidecar_location` 只加守卫。

第三处是 `_plugin_registration` 的部分失败回滚。主核里对应的是 **DDL 的事务性**：`CREATE TABLE` 要建文件、建 pg_class 行、建索引——失败必须全部回滚，否则会留下「目录里有文件但没有 pg_class 行」这种孤儿。haystack 的 b03 那篇里也讨论过「校验顺序即边界」，而这里是「注册的原子性」，两者都是「多步副作用需要一个原子边界」。

对 AI 工程的实际意义在于：分块器插件这一层的设计决定了「用户能不能安全地换算法」。这份实现给出的三条经验可直接用：**注册只存惰性引用，选中的才 import**（否则一个坏插件拖垮启动）、**能力探测要显式声明不要试探调用**（分块有副作用，重跑一次不可接受）、以及**「存在」与「可选」要分开暴露**（诊断需要前者，操作只能给后者）。
