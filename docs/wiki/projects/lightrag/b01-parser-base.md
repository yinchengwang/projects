---
project: lightrag
feature: parser-base
title: 解析器契约与原生解析基类
page_type: feature
schema_version: 1
refs:
  - path: lightrag/parser/base.py
    symbols: [BaseParser, ParseContext, ParseResult, ResolvedSource]
  - path: lightrag/parser/native_base.py
    symbols: [NativeParserBase, NativeExtractRuntime]
---

# 解析器契约与原生解析基类

## 一句话职责

`BaseParser` 只声明「一个 `parse(ctx) -> ParseResult` 协程」，把**能力元数据全部赶到注册表的 `ParserSpec` 里**；`ParseContext` 把「解析一份文档所需的一切」包成一个对象（而不是给解析器一个裸的 `rag`），`ParseResult` 用「只输出语义上存在的字段」的 `to_dict` 保住了重构前的返回形状；`NativeParserBase` 则把「本地解析」这条流程的**八个步骤固定成模板方法**，子类只实现 `extract` 与 `build_ir` 两个钩子。

## 机制是什么

解析层要面对的核心张力是：**每个引擎的执行方式不同（本地库调用 / HTTP 服务），但它们对外的契约必须完全一样。**

差异有多大：native 在本进程的线程里解析 docx，mineru 与 docling 把文件发到远端服务再取回。前者的失败是异常，后者的失败是 HTTP 状态码 + 网络超时；前者可以随手 `rmtree` 重建输出目录，后者要处理「上传成功但下载失败」。若这些差异泄漏到调用方，管道层就会长成 `if engine == "native": ... elif engine == "mineru": ...` 的链条——这正是注册表想消灭的东西。

所以分工是这样的：

| 层 | 承担什么 | 不承担什么 |
|---|---|---|
| `BaseParser` | 只有一个 `parse` 协程的签名 | 任何能力信息（后缀、并发组、endpoint 要求） |
| `ParserSpec`（b02） | 全部能力元数据 | 任何行为 |
| `ParseContext` | 每份文档的输入句柄 + 三个便捷方法 | 引擎相关的任何决策 |
| `NativeParserBase` | 本地解析的完整流程与失败语义 | 具体格式的抽取逻辑 |

而 `ParseResult` 面对的是另一类张力：**重构不能改变已持久化的数据形状**。管道 worker 是拿 `.get(...)` 读这些字段的，所以「多出一个 `parse_warnings: None` 键」与「少一个键」在字典比较层面是两件事。

## 怎么实现的

**`BaseParser` 薄到只有一条签名和一个类属性。**

```
class BaseParser(ABC):
    """Abstract base for every parser engine.

    Subclasses set ``engine_name`` and implement :meth:`parse`.  Capability
    metadata (suffixes/queue group/endpoint) is declared in the registry
    ``ParserSpec``, not here.
    """

    engine_name: str

    @abstractmethod
    async def parse(self, ctx: ParseContext) -> ParseResult:
        """Parse one document and return its :class:`ParseResult`."""
        ...
```

模块 docstring 把这条分工的理由写明：

> ``BaseParser`` carries *behaviour only* (the ``parse`` coroutine and any engine-private hooks). Capability metadata (supported suffixes, queue group, endpoint requirements) lives in the registry's lightweight ``ParserSpec`` table so **capability queries never import a parser implementation**.

**「能力查询绝不触发实现导入」这一条是全部设计的支点。** 具体代价很实在：mineru / docling 的实现要 import `httpx`，而「这个后缀支持吗」「哪些引擎可用」这类查询会在**每次上传校验、每次目录扫描**时发生。若能力信息挂在解析器类上，就得先 import 那个类——于是「查一下 .pdf 能不能解析」变成「加载 HTTP 客户端库」。把这个数据搬到注册表（b02 的 `ParserSpec`）之后，能力查询只读一个纯字面量的字典。

**`ResolvedSource` 是「每个引擎都需要的同一段前置解析」的产物。**

```
@dataclass
class ResolvedSource:
    """The common parse preamble shared by every engine."""

    source_path: Path
    document_name: str
    parsed_dir: Path
```

三个字段对应三件事：**磁盘上的源文件在哪**、**文档的规范名是什么**、**解析产物该写到哪个目录**。三者的推导逻辑对所有引擎完全一样（都要做路径规范化、都要拼 `__parsed__/<base>.parsed/`），所以被抽成一个值。

**`ParseContext` 的价值在于「不给解析器裸 rag」。**

```
@dataclass
class ParseContext:
    """Inputs handed to :meth:`BaseParser.parse`.

    Wraps the LightRAG instance plus the per-document handles so a parser
    does not receive a bare ``self``.  Convenience helpers lazily import
    pipeline-layer functions at call time, keeping this module import-cheap
    and free of import cycles.
    """

    rag: Any
    doc_id: str
    file_path: str
    content_data: dict[str, Any]
    # Set by the active pipeline batch when the user requests cancellation.
    # Native parsers pass it to their synchronous LLM bridge so a worker thread
    # does not remain blocked on a slow title-block judgment.
    pipeline_cancel_event: threading.Event | None = None
```

四点的含义：

1. **`rag` 用 `Any` 而不是真实类型** ——因为真实类型在 `lightrag.py` 里，而那个模块会 import 解析层，形成循环。用 `Any` 切断，同时把「解析器需要 rag 的什么」这件事变成隐式契约（只有 `_persist_parsed_full_docs`、`_get_parse_native_executor`、`_parser_shutdown_event` 这几个实际被用到的成员）。
2. **`doc_id` / `file_path` / `content_data` 是每份文档的输入句柄**，打包后解析器不需要知道它们从哪来。
3. **`pipeline_cancel_event` 是可选字段**，默认 `None`。注释说明它存在的目的不是「让解析器优雅退出」，而是**让 worker 线程不要卡在一次慢的 LLM 等待上**——一个同步等待的线程无法响应取消，只能靠事件轮询。
4. **三个便捷方法是方法而不是自由函数**，且**都在调用时才 import 上层模块**：

```
def source_path(self, parser_engine: str) -> Path:
    """Resolve the on-disk source file for this document."""
    from lightrag.pipeline import (
        _call_source_file_resolver,
        _read_source_file,
    )
    return Path(
        _call_source_file_resolver(
            self.rag, self.file_path,
            source_file=_read_source_file(self.content_data),
            parser_engine=parser_engine,
        )
    )
```

**这里的「函数内 import」不是随意的，注释明确说了理由：保持本模块 import-cheap 且无循环依赖。** 这是一个很值得记的手法——把「为了不循环依赖而延迟导入」集中到少数几个方法里，而不是让整个模块牺牲类型。

**`archive_source` 的 docstring 讲了一件测试兼容性的事。**

```
async def archive_source(self, source_path: str) -> str | None:
    """Archive the source after a successful parse + full_docs sync.

    Resolved through the pipeline module's namespace (where the function
    is imported) so existing tests that patch
    ``lightrag.pipeline.archive_docx_source_after_full_docs_sync`` keep
    intercepting it now that the call site lives in the parser layer.
    """
    import lightrag.pipeline as _pipeline
    return await _pipeline.archive_docx_source_after_full_docs_sync(source_path)
```

**它没有 `from ... import archive_docx_source_after_full_docs_sync` 再调用，而是 `import lightrag.pipeline as _pipeline` 后经模块属性调用。** 这两种写法功能相同，但**可被 patch 的能力不同**：`import module; module.func()` 走的是模块属性查找，于是 `patch("lightrag.pipeline.func")` 能拦到；而 `from module import func` 在导入时就把引用绑死了，patch 原模块对已绑定的引用无效。

这是一条**为了不破坏既有测试而选的调用形式**——它把一个「实现细节」变成了「契约」：调用点必须留在 pipeline 的命名空间里。

**`ParseResult.to_dict` 只输出「语义上存在」的字段。**

```
def to_dict(self) -> dict[str, Any]:
    out: dict[str, Any] = {
        "doc_id": self.doc_id,
        "file_path": self.file_path,
        "parse_format": self.parse_format,
        "content": self.content,
        "blocks_path": self.blocks_path,
    }
    if self.parse_engine is not None:
        out["parse_engine"] = self.parse_engine
    if self.parse_stage_skipped:
        out["parse_stage_skipped"] = True
    if self.parse_warnings:
        out["parse_warnings"] = self.parse_warnings
    if self.smartheading_llm_cache_ids:
        out["smartheading_llm_cache_ids"] = self.smartheading_llm_cache_ids
    return out
```

**前五个键无条件写入，后四个只在「有内容」时写入。** 模块 docstring 说明了理由：

> ``ParseResult.to_dict()`` emits only semantically-present fields so the returned dict stays byte-for-byte compatible with the pre-refactor ``parse_native``/``parse_mineru``/``parse_docling`` return shapes (the worker reads these by ``.get(...)``).

三处细节：

1. **`blocks_path` 默认是 `""` 而不是 `None`，且无条件写入。** 这与它的消费方式有关：worker 用 `result.get("blocks_path")` 拿到的是空串而不是缺键，两者在下游行为上等价但字典比较不等价。
2. **`parse_stage_skipped` 用的是真值判断 `if self.parse_stage_skipped:`** ——默认 `False`，于是「跳过了」这个事实存在时写入 `True`，「没跳过」时**整个键不出现**。这是一个「用键的存在性表达布尔」的模式，好处是与旧形状一致，坏处是读端不能写 `result["parse_stage_skipped"]`。
3. **`smartheading_llm_cache_ids` 的字段注释解释了为什么它必须是一等字段**：

```
# LLM cache keys minted during the parse stage (docx smart_heading).
# Fixed, first-class field: the pipeline mirrors it into
# doc_status.metadata so adelete_by_doc_id(delete_llm_cache=True) can
# purge parse-stage cache — there is no chunk llm_cache_list at parse
# time to carry them.
```

**「解析阶段没有 chunk 的 `llm_cache_list` 可以挂这些 key」** ——所以必须单独有一条通道把它们传出去，否则删除文档时这些 LLM 缓存行就成了孤儿（无法被定位、无法被清除）。这是 a01 里「purge 必须能回答『这份文档贡献了什么』」那条不变量在解析层的落点。

**`NativeParserBase.parse` 是一条固定的八步流程。**

模块 docstring 直接把它写成流水线：

```
resolve + validate source → compute parsed_dir/asset_dir
→ pre-clean (rmtree parsed_dir + mkdir + mkdir asset_dir, with rollback)
→ extract() in a thread → build_ir() → write_sidecar(clean_parsed_dir=False)
→ persist full_docs (lightrag) → archive source
```

逐步看几个关键点。

**第一步：参数先解码，且「指名别的引擎」要主动报错。**

```
_engine, engine_params, decode_errs = decode_parse_engine(ctx.content_data.get("parse_engine") ...)
if decode_errs:
    raise ValueError(f"{self.engine_name}: invalid parse_engine for doc_id={ctx.doc_id}: " + "; ".join(decode_errs))
if _engine and _engine != self.engine_name:
    raise ValueError(f"{self.engine_name}: parse_engine names a different engine {_engine!r} for doc_id={ctx.doc_id}")
```

两处注释说明了取舍：

- **畸形指令让这份文档大声失败，而不是「无参数静默解析」**（"A malformed/corrupt directive fails this doc loudly rather than silently parsing with no params (same contract as the external engines)"）。
- **指名别的引擎说明这一行是坏行或错路由**，「失败而不是把别人的参数当成自己的重新贴牌持久化」。

第二条尤其重要：`parse_engine` 这个字段是**会被回写的**（后面第 7 步重新编码），所以「用别人的参数去运行自己的引擎，再把结果以自己的名义存回去」会造成一次静默的数据污染。

**第二步：取消事件是三元组，且 shutdown 事件必须在进入 worker 之前捕获。**

```
cancel_event = threading.Event()
# Captured ONCE here, deliberately: _shutdown_parser_executor() sets the
# current event and then replaces the attribute with a fresh, unset one,
# so an in-flight parse must keep its reference to the event that was
# live when it started. Re-reading ctx.rag._parser_shutdown_event later
# would observe the replacement and miss the shutdown entirely.
shutdown_event = getattr(ctx.rag, "_parser_shutdown_event", None)
cancel_events = (
    cancel_event,
    (ctx.pipeline_cancel_event, ParsePipelineCancelled),
    (shutdown_event, ParseShutdown),
)
```

**「只读一次」这条注释是全篇最精细的一处**：`_shutdown_parser_executor()` 会先 `set()` 当前事件、然后**换上一个新的未置位事件**。若解析过程中反复去读 `rag._parser_shutdown_event`，第二次读到的就是那个新的、未置位的事件——于是**关闭信号被完全错过**，worker 会在存储正在拆除时继续往里写。

三元组第二个元素的形状（`(event, exception_type)` 对）说明「同一个取消语义，来源不同要区分」：管道取消抛 `ParsePipelineCancelled`，关闭抛 `ParseShutdown`，而本地那次（`cancel_event`，无异常类型）由调用方自行决定。

**第三步：LLM 桥只在参数需要时才建。**

```
if self.wants_llm_bridge(engine_params):
    submit, smartheading_cache_keys, i4_cache_disabled = self._build_llm_submit(ctx)
    if submit is not None:
        llm_invoke = SyncLLMBridge(asyncio.get_running_loop(), submit, cancel_events=cancel_events)
runtime = NativeExtractRuntime(engine_params=engine_params, llm_invoke=llm_invoke, cancel_events=cancel_events)
```

`wants_llm_bridge` 的默认实现返回 `False`，注释说明理由：**「base 保持引擎无关——只有具体的后缀实现才知道它的哪些参数隐含 LLM 工作」**（docx 为 `smart_heading` 覆盖它）。

`NativeExtractRuntime` 的 docstring 交代了它为什么把三样东西打包：

> Bundles the per-file state the async template resolves BEFORE entering the worker thread: the decoded ``parse_engine`` params, an optional synchronous LLM callable ..., and the cancellation events any blocking work in the worker thread should poll. **They travel together** — a subclass that consumes none of them simply ignores the argument.

**「它们一起旅行，用不到的引擎就忽略整个参数」** ——这比「给 `extract` 加三个可选 kwargs」更好：签名稳定，新增一个运行时资源不需要改所有子类。

**第四步：读级校验被专门从事件循环里挪出去。**

```
def validate_source(self, source: Path, file_path: str) -> None:
    """Validate the resolved source (default: must be an existing file).

    Runs inline in :meth:`parse`, i.e. ON THE EVENT LOOP, so it must stay
    stat-level. Anything that reads the file belongs in
    :meth:`validate_source_blocking`.
    """
    if not (source.exists() and source.is_file()):
        raise FileNotFoundError(...)

def validate_source_blocking(self, source: Path, file_path: str) -> None:
    """Validate what cannot be checked without reading the source.

    Runs at the top of ``_extract_sync`` — inside the parser executor, and
    BEFORE the ``parsed_dir`` cleanup — so that:
    * a check whose cost scales with the file (opening the archive of a
      .docx with a huge central directory takes seconds) cannot stall the
      event loop and every unrelated request with it, and
    * a refusal has not already deleted the previous attempt's artifacts.
    """
    return None
```

**「`stat` 级 vs 读级」的划分不是洁癖，而是两个具体后果**：一是读级校验可能花数秒（打开中央目录很大的 docx），在事件循环上会让所有并发请求一起卡住；二是它必须**排在上一步（`rmtree`）之前**，这样一次拒绝不会先毁掉上一次成功解析的产物。

**第五步：`cleanup_started` + `cleanup_lock` 处理「future 已取消但线程还在跑」的竞态。**

```
cleanup_started = False
cleanup_lock = threading.Lock()

def _extract_sync():
    nonlocal cleanup_started
    self.validate_source_blocking(source, ctx.file_path)
    ...
    pipeline_cancelled = ctx.pipeline_cancel_event
    with cleanup_lock:
        if shutdown_event is not None and shutdown_event.is_set():
            raise ParseShutdown(...)
        if cancel_event.is_set() or (pipeline_cancelled is not None and pipeline_cancelled.is_set()):
            raise ParsePipelineCancelled(...)
        cleanup_started = True
    if parsed_dir.exists():
        shutil.rmtree(parsed_dir)
    parsed_dir.mkdir(parents=True, exist_ok=True)
    asset_dir.mkdir(parents=True, exist_ok=True)
    return self.extract(...)
```

上层对应的回滚逻辑：

```
except BaseException:
    with cleanup_lock:
        cancel_event.set()
        should_roll_back = cleanup_started
    if should_roll_back and parsed_dir.exists():
        shutil.rmtree(parsed_dir, ignore_errors=True)
    raise
```

**这段是全篇最复杂的并发推理，而注释把它逐层拆开了。** 核心事实是：**取消 `run_in_executor` 的 future 只会取消 future，线程继续跑**。于是出现一个窗口——协程已经从 `await` 返回（拿到了 `CancelledError`）并去做回滚，而线程还没到取消检查点。两者都在动 `parsed_dir`。

源码给的解法是把两个动作各自做成临界区：

- 线程侧：`{读事件, 置 cleanup_started}` 在锁内；
- 协程侧：`{置 cancel_event, 读 cleanup_started}` 在锁内。

于是**只可能有两种结局**：协程先看到 `cleanup_started=False`（线程还没提交 rmtree）→ 回滚跳过，线程随后在检查点看到 cancel 事件、自己抛出，什么都没破坏；或者线程先提交了标志 → 协程回滚，而线程已不可能再动目录。

注释还补了两个关键判断：

- **为什么抛 `ParsePipelineCancelled` 而不是 `asyncio.CancelledError`**：管道级取消只 `set` 事件、从不取消 worker task，所以在这里抛 `CancelledError` 会落到一个**活着且未被取消的 future** 上，再被 `_parse_worker` 当成 `BaseException` 重抛——**既匹配不上它的两个 `except` 分支（文档会滞留在 `PARSING`），又可能让整个批卡在 `q.join()` 且 `busy=True`**。而 `ParsePipelineCancelled` 是仓库范围内「阻塞的解析等待被取消」的统一信号，worker 会捕获它并把文档标成取消。
- **为什么 shutdown 走 `ParseShutdown` 而不是取消类异常**：`_shutdown_parser_executor()` 只 `set` 事件后 `shutdown(wait=False)`，**不等在跑的解析**——它的契约就是「在飞的工作靠事件退出」。而 shutdown 在审计上算「通用解析失败」而不是「取消」，所以抛一个不同的异常类型，worker 就会走失败路径。

**第六步：执行器优先用 rag 自己的池。**

```
executor_getter = getattr(ctx.rag, "_get_parse_native_executor", None)
try:
    if executor_getter is not None:
        blocks, warnings, metadata = await asyncio.get_running_loop().run_in_executor(
            executor_getter(), contextvars.copy_context().run, _extract_sync)
    else:
        blocks, warnings, metadata = await asyncio.to_thread(_extract_sync)
```

注释说明了为什么不能用进程默认的 `to_thread` 池：**解析可能要等 LLM 数秒，占住线程；默认池被占满会饿死所有其他 `to_thread` 用户**。而 rag 自有的池大小跟着 `max_parallel_parse_native` 走，与并发配置一致。

`contextvars.copy_context().run` 的显式使用是为了**保住 `contextvars` 传播**——`to_thread` 会自动做这件事，而 `run_in_executor` 不会。所以两条分支都要等价的行为。

**第七步：侧车写入与 `clean_parsed_dir=False`。**

```
parsed_data = write_sidecar(
    ir, parsed_dir=parsed_dir, doc_id=ctx.doc_id, engine=self.engine_name,
    clean_parsed_dir=False,  # asset dir pre-populated above
    block_drawing_path_style=self.sidecar_path_style,
)
```

**`clean_parsed_dir=False` 是必须的**：模板在前面已经把 `asset_dir` 建好、`extract` 可能往里写了图片字节，若 `write_sidecar` 再清一次目录，这些资产就没了。这一条把「谁的职责」划得很清楚：**目录清理只发生在模板的 pre-clean 那一次**。

**第八步：重新编码 `parse_engine` 再持久化，注释说明了一个会踩的坑。**

```
await ctx.rag._persist_parsed_full_docs(ctx.doc_id, {
    "content": make_lightrag_doc_content(parsed_data["content"]),
    ...
    "parse_engine": encode_parse_engine(self.engine_name, engine_params or None),
    ...
})
```

注释：

> Re-encode the engine + params so the persisted directive keeps the per-file params (the `{**existing, **record}` merge in `_persist_parsed_full_docs` would otherwise revert it to the bare engine name). No params encodes back to the bare name.

**持久化用的是「现有记录 + 本次记录」的合并，而现有记录里存的是裸引擎名**——所以若不在这里重新编码带上参数，合并后参数就丢了。这是一个非常具体的陷阱：**读取时解码、写入时必须重新编码，否则合并语义会把解码后的信息吃掉**。

**`ParseResult` 里 `blocks_path` 的默认 `""` 与 `parse_stage_skipped` 的默认 `False` 也是同一类考虑**：重构后字段用 dataclass 表达，但序列化出去必须是旧形状，所以默认值的选择要匹配「旧形状里那个键不存在时下游会怎么读」。

## 关键取舍

**源码事实：** `BaseParser` 只有一个抽象协程 `parse(ctx) -> ParseResult` 和类属性 `engine_name`；能力元数据（后缀、并发组、endpoint 要求）全部在注册表的 `ParserSpec` 里，模块 docstring 明确目标是「capability queries never import a parser implementation」（避免为查后缀而加载 httpx）。

**源码事实：** `ParseContext` 用 `Any` 表示 `rag` 以切断循环 import；三个便捷方法（`source_path` / `resolve` / `archive_source`）全部在**调用时**才 import 上层模块，理由写明是「保持本模块 import-cheap 且无循环依赖」。

**源码事实：** `archive_source` 通过 `import lightrag.pipeline as _pipeline` 后走模块属性调用，而不是 `from ... import func`，注释说明这是为了让既有测试 patch `lightrag.pipeline.archive_docx_source_after_full_docs_sync` 仍然有效（`from ... import` 会在导入时绑定引用，patch 原模块无效）。

**源码事实：** `ParseResult.to_dict` 无条件写 5 个键，另外 4 个只在「有值」时写入，目的是与重构前的返回形状逐字节兼容（worker 用 `.get(...)` 读）。

**源码事实：** `smartheading_llm_cache_ids` 是一等字段，因为「解析阶段没有 chunk 的 `llm_cache_list` 可以承载这些 LLM 缓存键」，而删除文档时必须能定位并清除它们。

**源码事实：** `NativeParserBase.parse` 固定八步：解码参数 → 校验（`stat` 级）→ 建运行时（含取消三元组）→ 线程内读级校验 → pre-clean 目录 → `extract` → `build_ir` + `write_sidecar(clean_parsed_dir=False)` → 持久化 + 归档。子类只实现 `extract` / `build_ir`，可选覆盖 `wants_llm_bridge` / `validate_source_blocking` / `finalize_parse_warnings`。

**源码事实：** 畸形 `parse_engine` 指令与「指令指名了别的引擎」都直接 `ValueError`；后者是为避免「用别人的参数运行自己的引擎、再把结果以自己的名义持久化」。

**源码事实：** shutdown 事件必须在进入 worker 前**只读一次**，因为 `_shutdown_parser_executor()` 会先置位当前事件再换上一个新的未置位事件，反复读会错过关闭。

**源码事实：** `cleanup_started` + `cleanup_lock` 让线程侧 `{读事件, 置标志}` 与协程侧 `{置事件, 读标志}` 成为互斥临界区，解决「future 已取消但线程仍在跑」时的目录竞态；线程在检查点抛 `ParsePipelineCancelled` 而不是 `asyncio.CancelledError`，因为后者会落到一个未被取消的 future 上并被当成 `BaseException` 重抛，导致文档滞留 `PARSING` 甚至卡住整批。

**源码事实：** 执行器优先取 `ctx.rag._get_parse_native_executor()`（池大小跟 `max_parallel_parse_native` 走），否则退到 `to_thread`；前者显式传 `contextvars.copy_context().run` 以补回 `run_in_executor` 缺失的上下文传播。

**源码事实：** 持久化时**必须重新 `encode_parse_engine`**，因为 `_persist_parsed_full_docs` 的 `{**existing, **record}` 合并会让裸引擎名覆盖掉带参数的版本。

**架构推论：** 「行为在类里、数据在表里」这个分工的分界线是**「谁需要 import 谁」**。能力查询是最热的路径，所以它的数据必须能在不 import 实现的情况下被读到——这条约束一旦确立，`BaseParser` 就只能薄到一条签名。反之，若把后缀列表放进类属性，`available_engine_suffixes()` 就会变成「import 所有引擎」。**判断抽象该切在哪，先看哪条路径不能被 IO/依赖污染。**

**架构推论：** 这一页最难的部分不是流程，而是**「取消一个在线程里跑的任务」这件事本质上做不到**。所有复杂度（`cleanup_lock`、`cleanup_started`、两种异常类型、`wait=False` 的 shutdown）都是在承认这个事实之后做的正确性兜底。任何「把同步工作丢到线程池」的系统都会遇到同一类竞态；解法不是让取消生效（做不到），而是**让「取消后仍在跑的那段时间」不破坏共享状态**。

**架构推论：** `ParseResult.to_dict` 的「只输出存在的字段」是纯粹的**兼容性债务**。它让 dataclass 表达力（有默认值、有类型）与线上形状（键可能缺失）共存，代价是读端不能直接下标访问。这类债务的合理偿还时机是「下一个大版本」，而不是重构当次——因为重构的目标是「行为不变」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/parser/base.py` | `BaseParser` | 唯一的抽象契约：`parse(ctx) -> ParseResult` + `engine_name` |
| `lightrag/parser/base.py` | `ResolvedSource` | 所有引擎共享的前置解析产物：源路径、规范文档名、产物目录 |
| `lightrag/parser/base.py` | `ParseContext` | 每份文档的输入句柄 + 三个延迟导入的便捷方法 |
| `lightrag/parser/base.py` | `ParseContext.source_path` / `resolve` / `archive_source` | 路径解析、产物目录推导、源文件归档（均调用时 import 上层） |
| `lightrag/parser/base.py` | `ParseResult` | 结构化解析输出（`to_dict` 只写语义存在的字段） |
| `lightrag/parser/native_base.py` | `NativeParserBase` | 本地解析模板：八步流程 + 四类可选钩子 |
| `lightrag/parser/native_base.py` | `NativeParserBase.parse` | 模板方法本体（校验、取消三元组、线程执行、侧车写入、持久化、归档） |
| `lightrag/parser/native_base.py` | `validate_source` / `validate_source_blocking` | `stat` 级（事件循环内）与读级（执行器内、pre-clean 前）两层校验 |
| `lightrag/parser/native_base.py` | `wants_llm_bridge` / `_build_llm_submit` | 按引擎参数决定是否搭同步 LLM 桥，并收集缓存键 |
| `lightrag/parser/native_base.py` | `finalize_parse_warnings` / `surface_warnings` | 原始警告 → `parse_warnings` 字段的映射钩子 |
| `lightrag/parser/native_base.py` | `NativeExtractRuntime` | 交给 `extract` 的每文件运行时（参数、LLM 桥、取消事件集合） |

## 参考

- [lightrag 源码：parser/base.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/parser/base.py)
- [lightrag 源码：parser/native_base.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/parser/native_base.py)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [Python 文档：`asyncio` 与线程](https://docs.python.org/3/library/asyncio-dev.html)

## 与主核 / AI 的关系

「把一种耗时、可能失败、且取消不生效的操作封装成统一契约」在主核里对应的是**外部数据装载与插件执行**。四条经验。

**第一，把「不会取消」当成事实来设计，而不是当成 bug 去修。** 这里所有复杂度都在承认「线程不可中断」之后：标志位 + 互斥临界区保证「取消后仍在跑的那段」不破坏共享目录，异常类型的选择保证错误能落到正确的处理分支。主核里做「查询取消」时同理：一个卡在磁盘 I/O 或第三方库调用里的算子不会响应取消，**能做的是保证它回来时发现自己已被放弃、不要写共享状态**。判断准则：凡是有 `cancel()` 的地方，都要问「取消之后那段还在跑的时间会碰什么」。

**第二，「标志只读一次」这类注释，是并发代码里最有价值的资产。** `shutdown_event` 那段注释解释了「为什么不能反复读 `rag._parser_shutdown_event`」——因为持有者会先置位旧事件再换上新的。这类知识无法从代码结构推断出来，只能靠注释保存。主核里任何「读一个可能被替换的共享属性」的地方都有同样的坑，而症状是「关闭信号偶发丢失」。

**第三，能力数据与行为必须分层，判据是「哪条路径不能承担依赖」。** `ParserSpec` 存在的唯一理由是「查后缀不能 import httpx」。主核里若做插件/格式注册，同样要问：**哪个查询是高频且轻量的？它的数据必须能在不加载实现的情况下取到。** 一旦混在一起，加载一个插件就会连带拉进它所有的依赖。

**第四，测试可 patch 性会变成隐性契约。** `archive_source` 之所以用 `import module; module.func()`，是为了让既有测试继续拦得到。这意味着**「调用点所在的模块命名空间」被测试锁住了**——想把它挪到别处就必须同时改测试。主核里同理：使用 `from x import y` 还是 `import x` 不只是风格，它决定了未来能否在不改测试的前提下重构，也决定了 mock 是否生效。写新代码时若预见到「这个调用会被测试拦」，就该用模块属性访问形式。
