---
project: lightrag
feature: doc-status
title: 文档状态与游标
page_type: feature
schema_version: 1
refs:
  - path: lightrag/base.py
    symbols: [DocStatus, DocProcessingStatus, DocSchedulingRecord]
  - path: lightrag/base.py
    symbols: [CursorPosition, CursorAfter, _CursorSentinel]
---

# 文档状态与游标

## 一句话职责

`DocStatus` 是文档在四阶段管道里的位置，`DocProcessingStatus` 是那条记录本身（含 `from_stored` 的容错构造），`CursorPosition` 的三态游标让「按 `(created_at, id)` 键集扫描一批文档」这件事有一个**唯一且不可误读**的终止信号；`DocSchedulingRecord` 则是同一条记录的**瘦投影**，被刻意裁到「够排序、够过滤、不够渲染」的程度。

## 机制是什么

这一页的三样东西各自解决一个不同的问题，但都围绕同一句前提：**文档状态既是管道的输入队列，又是 WebUI 的列表数据源，两个消费者的要求相反。**

1. **状态机**要回答「这份文档现在该被哪个 worker 接手」。它是调度面的判据，要求**精确**——少一个状态就有一类文档永远不被捞起。
2. **记录结构**要回答「这条文档长什么样」。它同时被调度面（读状态、读时间戳）和展示面（读摘要、读错误信息、读进度）消费，要求**宽容**——旧版本写的行、第三方写的行都必须在列表里显示出来。
3. **游标**要回答「扫描扫到哪了」。它要求**不可误读**——把「这一页恰好没匹配」当成「扫完了」，就是全量漏处理。

这三种要求互相冲突，冲突的解法就是这一页的全部内容：状态机靠**枚举 + 阶段注释**强制精确；记录结构靠**一个专门的容错构造函数**接纳冗余；游标靠**哨兵对象而非空值**消除歧义。

最值得先看的是那个矛盾：`DocStatus.PREPROCESSED` 的注释写着「Deprecated: use ANALYZING in new pipeline」，**但 `DocProcessingStatus.__post_init__` 至今仍在生产这个状态**。这不是笔误，是一个没做完的迁移被留在了两类消费者之间——下面在「怎么实现的」里展开。

## 怎么实现的

**`DocStatus` 的八个值里，只有四个构成正向流水线。**

```
class DocStatus(str, Enum):
    """Document processing status.
    Pipeline order: PENDING -> PARSING -> ANALYZING (optional) -> PROCESSING -> PROCESSED | FAILED.
    PREPROCESSED is deprecated, kept for backward compatibility.
    """

    PENDING = "pending"
    PARSING = "parsing"       # Phase 1: content extraction (parse_native/mineru/docling)
    ANALYZING = "analyzing"   # Phase 2: multimodal analysis (VLM)
    PROCESSING = "processing" # Phase 3: entity/relation extraction
    PREPROCESSED = "preprocessed"  # Deprecated: use ANALYZING in new pipeline
    PROCESSED = "processed"
    FAILED = "failed"
    DELETING = "deleting"     # User-driven deletion recovery; never ingestion work.
```

四个要点：

1. **每个阶段的状态都注了「Phase N + 谁负责」**。`PARSING` 对应内容抽取（三个引擎任选），`ANALYZING` 多模态分析（VLM），`PROCESSING` 实体关系抽取。这让状态值与 worker 的对应关系**写在枚举里而不是散在调度代码里**——读枚举就能知道有几个 worker 阶段。
2. **`ANALYZING` 是 optional**，`PREPROCESSED` 是它的旧名。两者语义相同，所以老数据里的 `preprocessed` 行必须能被识别为「阶段 2 已完成」。
3. **`FAILED` 与 `DELETING` 不属于流水线**。`FAILED` 是终点之一（`PROCESSED | FAILED`）；`DELETING` 的注释明确说它是「用户驱动的删除恢复，永远不是摄取工作」——它是一个**恢复用的暂存态**，防止删除到一半的记录被调度器当成待处理。
4. **`str` 混入**让它可以与 JSON 里的裸字符串直接比较（`status == "processed"`），因为存储层写的就是 `.value`。

**`DocProcessingStatus.__post_init__` 是一次「向后兼容的反向改写」。**

```
def __post_init__(self):
    """
    Handle status conversion based on multimodal_processed field.

    Business rules:
    - If multimodal_processed is False and status is PROCESSED,
      then change status to PREPROCESSED
    - The multimodal_processed field is kept (with repr=False) for internal use and debugging
    """
    if self.multimodal_processed is not None:
        if (self.multimodal_processed is False and self.status == DocStatus.PROCESSED):
            self.status = DocStatus.PREPROCESSED
```

**这段代码把已经完成的新状态 `PROCESSED` 改回已废弃的 `PREPROCESSED`。**

它的来源很清楚：`multimodal_processed=False` 表示「这份文档不需要/没有做多模态分析」。在旧管道里，那意味着它停在 `PREPROCESSED`（阶段 2 未做）。新管道把「阶段 2 跳过」和「阶段 2 做完」都归入 `PROCESSED`，但为了让**旧的读端**（按 `PREPROCESSED` 判断「还能不能再往下走」的代码、以及按状态列过滤的 UI）仍然工作，构造时把状态翻译回旧值。

这带来一个真实的代价，值得明确写下来：

- **`PROCESSED` 不是「全部结束」的唯一表示**。任何读端如果要问「这份文档处理完了吗」，必须同时接受 `PROCESSED` 与 `PREPROCESSED`。
- **枚举注释与构造行为不一致**。枚举说 `PREPROCESSED` 已废弃，构造函数却仍在产出它。读代码的人若只信枚举注释，会认为这个值只会出现在历史数据里。
- **`multimodal_processed` 是一个「影响构造结果」的字段**，所以它不能在读路径上被随意补默认值——它的 `None`（未知）与 `False`（明确没做）行为不同：`None` 不触发改写，`False` 触发。

**`from_stored` 是为一次真实事故写的容错构造。**

```
@classmethod
def from_stored(cls, data: dict[str, Any]) -> "DocProcessingStatus":
    """Construct from a stored doc_status row, ignoring undeclared fields.

    Producers evolve independently of this schema: RAG-Anything writes
    ``scheme_name``/``multimodal_content``, other LightRAG versions write
    fields an installed release does not declare yet (or no longer does).
    ``DocProcessingStatus(**row)`` raises ``TypeError`` on any such field,
    which makes every read path treat the row as malformed and drop it
    from listings — the document silently disappears from the WebUI and
    the API even though its record is intact (HKUDS/RAG-Anything#73).

    Extra fields are ignored for construction only; the stored row is
    not modified. A *missing required* field still raises ``TypeError``
    — tolerance is strictly for extras, not for malformed rows.
    """
    known = {f.name for f in fields(cls)}
    return cls(**{k: v for k, v in data.items() if k in known})
```

**这是一份写得极好的 docstring，它把「为什么」写到了可以直接复述的程度**：

- 生产方（第三方包、其他版本的 LightRAG）**独立演化**，会写进本版本没声明的字段。
- 一个多余字段让 `DocProcessingStatus(**row)` 抛 `TypeError`。
- 调用方的读路径把 `TypeError` 当成「这行坏了」，于是把它从列表里丢掉。
- 结果：**记录完好无损，文档却从 UI 与 API 里消失了**。

docstring 结尾那句「Extra fields are ignored for construction only; the stored row is not modified」和「A *missing required* field still raises」把容错的**边界**画得很清楚：**只容忍多余，不容忍残缺**。这个边界很重要——若连缺字段也容忍，「行坏了」这个信号就消失了，损坏会被静默地当成默认值。

**实现只有三行，且用的是 `dataclasses.fields` 而不是手写白名单**：

```
known = {f.name for f in fields(cls)}
return cls(**{k: v for k, v in data.items() if k in known})
```

用 `fields(cls)` 的好处是**新增字段时不需要同步改白名单**——白名单一旦手写就会漂移，而漂移的方向正好是「新字段被静默丢弃」。这里把「声明」当成唯一真相来源。

代价是**未知字段被真正丢掉了**：读完再写回去，第三方字段就消失了。docstring 明确说这是有意的（"the stored row is not modified"），也就是说**读路径不该回写**——任何「读出来改一下再存」的路径都会抹掉别人的字段。

**三态游标是一个密封类层次，而不是 `str | None`。**

```
class CursorPosition:
    """Sealed three-state cursor for stable keyset sweeps.

    Exactly three shapes exist: the CURSOR_START singleton, the
    CURSOR_END singleton, and CursorAfter (an opaque, backend-defined
    continuation token). ``page.next_position is CURSOR_END``
    is the ONLY termination signal — an empty ``docs`` dict is not (a page
    may be fully filtered yet not exhausted).
    """

    __slots__ = ()


class _CursorSentinel(CursorPosition):
    __slots__ = ("_name",)

    def __init__(self, name: str) -> None:
        self._name = name

    def __repr__(self) -> str:  # pragma: no cover - trivial
        return self._name


CURSOR_START = _CursorSentinel("CURSOR_START")
"""Begin a sweep from the smallest ``(created_at, id)`` key."""

CURSOR_END = _CursorSentinel("CURSOR_END")
"""The sweep is exhausted; no further page exists."""
```

五处设计点：

**第一，`__slots__ = ()` 让基类无法携带状态。** `CursorPosition` 本身不能被实例化出有意义的值——它只是一个类型标签。子类要么是单例哨兵（带一个 `_name` 仅供 `repr`），要么是 `CursorAfter`（带一个 opaque 字符串）。**「恰好三种形态」是被类型结构保证的，不是被文档保证的。**

**第二，不变量用 `is` 而不是 `==` 表达。** docstring 写的是 `page.next_position is CURSOR_END`，是**身份比较**。这也解释了为什么哨兵要设计成模块级单例而不是每次新建：`is` 只有对单例才成立。

**第三，终止信号唯一，且 docstring 明确排除了那个诱人的替代品。** 「an empty `docs` dict is not」——因为**过滤后的页可以是空的但没扫完**。这正是键集分页的经典陷阱：`WHERE status IN (...) LIMIT n` 在按状态过滤时，某一页可能全部被过滤掉，`docs` 为空，但后面还有数据。用空结果判终止就会漏处理。

**第四，`CursorAfter` 是 `frozen=True` 的 dataclass，且字段名就叫 `opaque`。**

```
@dataclass(frozen=True)
class CursorAfter(CursorPosition):
    """Opaque continuation token: resume strictly after the last CONSUMED
    underlying record (see the consumed-position contract on
    :meth:`DocStatusStorage.get_docs_by_statuses_page`)."""

    opaque: str
```

**字段名 `opaque` 是一个声明**：调用方不得解析它、不得构造它，只能原样回传。这比用一个 `str` 好——一个裸 `str` 会被误当成主键，而 `CursorAfter("doc-123")` 看起来就像「从 doc-123 继续」，实际语义是「从它**之后**继续」，且只对写它的那个后端有效。

**第五，「consumed-position」这个契约决定了该推进到哪。** `get_docs_by_statuses_page` 的 docstring 把这条讲得极细：

> **Consumed-position advance**: `next_position` advances past the last CONSUMED underlying record — records returned to the caller plus records read and dropped by filtering are consumed; a record prefetched for merging but NOT returned because the page filled is NOT consumed (or must be encoded into the opaque cursor and returned first on the next page).

也就是说「消费」的定义不是「返回给调用方」，而是「读过并丢弃也算」。这解决了多状态合并扫描（k-way merge）时的真问题：为了合并两个状态流，实现可能预取了一条下一个才该返回的记录；若游标越过它，那条记录就永久丢了。契约给了两条出路——**要么别越过它，要么把它编进 opaque 游标并在下一页第一个返回**。

**配套的地基是「排序键不可变」。**

```
* **Immutable sort key**: ``created_at`` is written once at record
  creation and preserved by every later transition
  (``update_doc_status_fields`` refuses it) so a record can never move
  underneath a sweep.
```

底层的强制点落在 `DocStatusStorage.update_doc_status_fields`：

```
if "created_at" in fields:
    raise ValueError(
        "created_at is an immutable scheduling sort key and cannot "
        "be changed via update_doc_status_fields"
    )
```

**「不可变」不是靠约定，而是靠一个显式的 `ValueError`。** 原因是键集扫描的前提：一条记录的排序键如果在扫描过程中被改，它会**在游标已经越过之后移到后面**（或反过来），于是要么被扫两次、要么一次都扫不到。

同一段契约还处理了两个边角，都很实在：

- **NULL 桶必须可由游标恢复**：「Where the backend indexes a missing/NULL `created_at` at all, that bucket must stay reachable across page boundaries via bucket-aware resume rather than becoming permanently unreachable behind a real-timestamp cursor.」——因为游标是「真实时间戳」，一个没有时间戳的桶若排在它之前，就永远够不着了。
- **那种行永远不会被返回**：「a strict page raises on it (complete-or-raise) and a relaxed page consumes-and-skips it so the cursor still advances.」——`DocSchedulingRecord` 要求 `created_at` 必填，所以这种行无法投影；严格页直接抛，宽松页跳过但**推进游标**。两条路都不让它卡住扫描。

**`DocSchedulingRecord` 是被裁剪过的投影，裁剪本身就是设计。**

```
@dataclass(frozen=True)
class DocSchedulingRecord:
    """Lightweight scheduling projection of a doc_status row.

    Deliberately excludes ``chunks_list``, large ``metadata`` blobs and full
    error text so a page of records stays O(page_size × small constant) —
    the pipeline hydrates full records per-document only when it actually
    routes one.
    """

    id: str
    status: DocStatus
    created_at: str
    updated_at: str
    file_path: str | None
    track_id: str | None
    has_custom_chunk_journal: bool
```

三点：

1. **它不含 `chunks_list`**。`DocProcessingStatus.chunks_list` 是一份完整的分块 ID 列表，对一份大文档可以是几千项；一页 100 条就是几十万项。而调度只需要「状态 + 时间戳」。
2. **它不含 `metadata` 与完整 `error_msg`**。这两者都可能很长，且调度不需要正文。
3. **但它保留了一个 `has_custom_chunk_journal` 布尔位**。这是 `metadata` 里**唯一**被调度需要的信息，于是被提成独立字段。docstring 说明用途：「True when doc_status.metadata carries the custom-chunk patch journal — such rows belong to scan/custom-chunk recovery, not ordinary routing.」

**这一条是「裁剪」与「提列」的配合**：整个 `metadata` blob 被丢掉，但其中一个布尔判据被抬到顶层。没有这个字段，调度要么被迫读整个 `metadata`（毁掉 O(page_size) 的承诺），要么无法区分两类行。

配套的读法是两条独立的抽象方法，一个只读投影、一个读全量：

```
async def get_docs_by_ids(...) -> dict[str, DocSchedulingRecord]   # 投影
async def get_full_docs_by_ids(...) -> dict[str, DocProcessingStatus]  # 全量
```

后者的 docstring 把「为什么分两个」写清了：

> The scheduler hydrates the survivors of ONE page into full `DocProcessingStatus` rows through this method right before routing them, so memory stays O(page_size) while the consistency validator and the parse/analyze/process workers still …

**模式是「先粗读一整页，再对幸存者逐个补水」**——这正是「列表页 + 详情页」两级读法在调度器上的应用。

**`DocStatusPage` 把「页」与「下一页位置」绑成一个不可分的值。**

```
@dataclass(frozen=True)
class DocStatusPage:
    """One page of a stable keyset sweep.

    ``next_position is CURSOR_END`` terminates the sweep; any other value
    (including with an empty ``docs``) means "call again".
    """

    docs: dict[str, DocSchedulingRecord]
    next_position: CursorPosition
```

**把两者放进同一个 frozen 值，是防「忘了带游标」的机制**：调用方不可能拿到一批文档却没拿到继续位置。若分成两个返回值（`docs, next_pos = ...`），总有人只接第一个。

## 关键取舍

**源码事实：** `DocStatus` 有 8 个成员，其中 `PENDING→PARSING→ANALYZING(可选)→PROCESSING→PROCESSED|FAILED` 是正向流水线，`PREPROCESSED` 标注为 deprecated（`ANALYZING` 的旧名），`DELETING` 是「用户驱动的删除恢复，永远不是摄取工作」。

**源码事实：** `DocProcessingStatus.__post_init__` 在 `multimodal_processed is False` 且 `status is PROCESSED` 时把状态**改写回** `PREPROCESSED`——即构造函数仍在产出枚举注释里标为已废弃的值。`multimodal_processed` 为 `None`（未知）时不改写。

**源码事实：** `DocProcessingStatus.from_stored` 用 `fields(cls)` 动态取已知字段名，过滤掉多余字段后构造；多余字段被丢弃（仅构造期，不回写存储），**缺必填字段仍抛 `TypeError`**——容错严格限定在「多余」，不覆盖「残缺」。docstring 点名了触发事故（`HKUDS/RAG-Anything#73`）与完整因果链。

**源码事实：** `CursorPosition` 用 `__slots__ = ()` 密封，恰好三种形态：`CURSOR_START` / `CURSOR_END` 两个模块级单例、`CursorAfter(opaque: str)`。终止判据是 `next_position is CURSOR_END`（身份比较，故哨兵必须是单例），**空 `docs` 不是终止信号**。

**源码事实：** `CursorAfter` 的字段名是 `opaque`，语义是「从最后一条**已消费**记录之后继续」；「已消费」包含「读过后被过滤丢弃」，预取但因页满而没返回的记录不算消费（要么不越过，要么编进游标下一页先返回）。

**源码事实：** `created_at` 是不可变排序键，`update_doc_status_fields` 检测到它出现在 `fields` 里就抛 `ValueError`。NULL `created_at` 的桶必须靠 bucket-aware resume 保持可达，但那种行无法投影成 `DocSchedulingRecord`（严格页抛、宽松页跳过并推进游标）。

**源码事实：** `DocSchedulingRecord` 刻意排除 `chunks_list`、大块 `metadata` 与完整错误文本，只保留调度需要的字段，外加一个从 `metadata` 提列出来的 `has_custom_chunk_journal` 布尔位；全量记录由 `get_full_docs_by_ids` 对「一页的幸存者」按需补水。

**源码事实：** `DocStatusPage` 把 `docs` 与 `next_position` 组成一个 frozen 值，docstring 明确「除 `CURSOR_END` 外的任何值（**包括 `docs` 为空时**）都意味着该再调一次」。

**架构推论：** 「枚举注释说废弃、构造函数却仍在产出」这个矛盾，是**跨版本兼容被压到一个类里**的必然症状。真正的解法是把翻译层放在边界上（读旧数据时翻译一次），而不是放在记录自身的构造函数里——后者会让「记录被创建」与「记录被读取」两条路径都经过翻译，于是同一个值在两种语境下含义不同。这里的做法能工作，但它把「哪些状态算完成」这个问题留给了每一个读端。

**架构推论：** `from_stored` 的容错方向（容忍多余、不容忍缺失）值得当成通则：**多余字段是别人的正常演化，缺字段是本行的真实损坏**。反过来容忍缺失会把「损坏」静默成「默认值」，而这类静默错误的排查成本远高于一次 `TypeError`。docstring 把这条边界明写出来，是这个函数最有价值的部分。

**架构推论：** 用哨兵单例 + `is` 比较表达终止，比 `Optional[str]` 强的地方在于**不可能误判**。`None` 可以来自「没有下一页」「后端没实现」「序列化丢了」三种原因，而 `CURSOR_END` 只有一个含义。代价是它不可序列化传出去（单例身份在跨进程后会丢），所以 `CursorAfter.opaque` 必须是字符串而非哨兵——**可传输的那一段用字符串，不可传输的边界用哨兵**，这个分工是对的。

**架构推论：** 「瘦投影 + 按需补水」是列表类接口的通用形状。它的核心不是省内存，而是**让「一页的成本」与「一条记录的大小」解耦**——否则一条巨大的记录就能让整页超时，而超时是整页失败。这也解释了为什么 `has_custom_chunk_journal` 要提列：**只要有一个判据被调度需要，就不能把整个 blob 留在详情里**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/base.py` | `DocStatus` | 8 个状态：四阶段流水线 + 废弃别名 + 删除恢复态 |
| `lightrag/base.py` | `DocProcessingStatus` | 完整状态记录（摘要、长度、分块列表、错误、多模态标记、内容哈希） |
| `lightrag/base.py` | `DocProcessingStatus.__post_init__` | `multimodal_processed=False` + `PROCESSED` → 改写为 `PREPROCESSED` |
| `lightrag/base.py` | `DocProcessingStatus.from_stored` | 容忍多余字段的构造（`fields(cls)` 过滤）；缺必填仍抛 |
| `lightrag/base.py` | `CursorPosition` | 密封的三态游标基类（`__slots__ = ()`） |
| `lightrag/base.py` | `_CursorSentinel` / `CURSOR_START` / `CURSOR_END` | 两个模块级单例哨兵，供 `is` 比较 |
| `lightrag/base.py` | `CursorAfter` | `frozen` 的 opaque 续扫令牌 |
| `lightrag/base.py` | `DocSchedulingRecord` | 调度投影：无 `chunks_list` / 无 `metadata` blob，但提列 `has_custom_chunk_journal` |
| `lightrag/base.py` | `DocStatusPage` | `docs` + `next_position` 绑定成一个 frozen 值 |
| `lightrag/base.py` | `DocStatusStorage.get_docs_by_statuses_page` | 键集扫描契约：排序、不可变键、consumed-position、严格模式 |
| `lightrag/base.py` | `DocStatusStorage.update_doc_status_fields` | 拒绝写 `created_at`（不可变排序键的强制点） |
| `lightrag/base.py` | `DocStatusStorage.get_docs_by_ids` / `get_full_docs_by_ids` | 投影读与全量补水两条独立路径 |

## 参考

- [lightrag 源码：base.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/base.py)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [lightrag 文档](https://github.com/HKUDS/LightRAG/blob/main/README.md)
- [RAG-Anything 仓库（`from_stored` 容错所针对的第三方生产方）](https://github.com/HKUDS/RAG-Anything)

## 与主核 / AI 的关系

这一页的三样东西在主核（多模态数据库内核）里都能找到对应物：**状态机对应元组的可见性状态、`from_stored` 对应 schema 演进的读兼容、游标对应 B-tree 区间扫描的位置**。四条经验。

**第一，终止条件必须用位置而不是用结果。** `CURSOR_END` 那条契约（「空 `docs` 不是终止信号」）与 B-tree 区间扫的经典陷阱完全同构：一个按谓词过滤的页可能是空的但没扫完。主核做「按状态扫元组」时若用「返回 0 行就停」，会在过滤选择性高时静默漏数据——而漏掉的恰好是没被处理的那批，重跑也不会补。**判断准则：任何「分批处理全量」的接口，其返回类型里必须有一个与数据无关的位置量。**

**第二，排序键不可变是键集分页的前提，而这件事必须强制。** 这里用 `ValueError` 挡住 `created_at` 的更新。主核里同理：任何被用作扫描键的字段（页内 slot、时间戳、自增 ID）一旦允许在扫描期间变更，就会出现「扫两次」或「漏一条」。**能靠类型/存储布局强制的就别靠约定**；这里只能靠运行时检查，已经是次优解。

**第三，读兼容的容错方向只有一个。** `from_stored` 容忍多余、不容忍缺失，这条边界值得直接照抄进主核的元组反序列化：**多余字段是外部系统的正常演化，缺字段是本条记录真实损坏**。把两者都容忍，就等于放弃了「损坏可被发现」这个能力。而 docstring 里那段完整因果（多余字段 → `TypeError` → 读路径当成坏行丢掉 → 记录完好但文档从列表消失）是所有「严格解析 + 宽松丢弃」组合的通用失败模式。

**第四，投影与全量必须分成两条读路径，且投影的裁剪要按「谁需要」而不是按「多大」。** `DocSchedulingRecord` 丢掉整个 `metadata` 却把其中一个布尔位提列，说明裁剪的正确依据是**消费方的判据集合**。主核做「元组头 + 溢出页」时是同一件事：固定在头部的是所有扫描都要用到的字段，其余的进溢出区。若头部塞进一个只有少数场景需要的大字段，所有扫描都要为它付 I/O。
