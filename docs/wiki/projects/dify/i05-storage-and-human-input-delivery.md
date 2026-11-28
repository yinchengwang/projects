---
project: dify
feature: storage-and-human-input-delivery
title: 存储后端与人工介入投递
page_type: feature
schema_version: 1
refs:
  - path: api/extensions/storage/base_storage.py
    symbols: [BaseStorage]
  - path: api/core/human_input_v2/im_integration/__init__.py
    symbols: [IMSyncRepository, OpaqueProviderPayload]
---

# 存储后端与人工介入投递

## 一句话职责

`BaseStorage` 是十四个存储后端共享的抽象：**六个必实现方法 + 两个可选方法（默认抛 `NotImplementedError`）+ 一个静态过滤器**；`im_integration` 包则是「人工介入的历史同步」这一子域对外暴露的**唯一接口面**——它不导出任何实现，只导出协议（`IMSyncRepository`）与冻结的值对象。

## 机制是什么

这两个文件回答的是同一个问题的两个极端：**抽象该薄到什么程度，以及抽象该厚到什么程度**。

`BaseStorage` 是**极薄的抽象**：它只做一件事——把「文件存储」收敛成七个动词（`save` / `load_once` / `load_stream` / `download` / `exists` / `delete`，加两个可选）。它**不提供任何默认实现**，除了两个「明确不支持」的占位与一个纯函数过滤器。

而 `im_integration/__init__.py` 是**极厚的边界**：它是一个**纯再导出文件**，`__all__` 里有四十九个名字，全部来自六个内部模块。它不做任何逻辑，只做**收窄可见面**：

```python
"""Infrastructure-free historical synchronization and reconciliation boundary."""
```

**「Infrastructure-free」** ——这个定语是全篇的关键：**这一层承诺它不依赖数据库、HTTP、消息队列**。所有 I/O 都通过 `IMSyncRepository` 这个 `Protocol` 注入，而这个 `Protocol` 自己的 docstring 把「什么不该有」写得很直接：

```python
"""Transaction-oriented persistence ports for IM Control Plane invariants.

Implementations own CAS predicates, Channel row locks, eager loading,
revision-guarded apply, rollback, and append-only result persistence. Generic
table CRUD is intentionally absent.
"""
```

**「Generic table CRUD is intentionally absent」** ——**这个协议故意不提供通用 CRUD**，只有六个业务方法。这是一个「端口设计」的强立场：**端口应当表达用例，而不是表达表。**

## 怎么实现的

**`BaseStorage` 的六个方法全带 `@abstractmethod`，而它们的方法体都是 `raise NotImplementedError`。**

```python
class BaseStorage(ABC):
    """Interface for file storage."""

    @abstractmethod
    def save(self, filename: str, data: bytes):
        raise NotImplementedError

    @abstractmethod
    def load_once(self, filename: str) -> bytes:
        raise NotImplementedError

    @abstractmethod
    def load_stream(self, filename: str) -> Generator:
        raise NotImplementedError

    @abstractmethod
    def download(self, filename: str, target_filepath: str) -> None:
        raise NotImplementedError

    @abstractmethod
    def exists(self, filename: str) -> bool:
        raise NotImplementedError

    @abstractmethod
    def delete(self, filename: str):
        raise NotImplementedError
```

**四处：**

1. **`raise NotImplementedError` 在 `@abstractmethod` 下是冗余的** ——ABC 会在实例化时就拒绝，走不到方法体。**但它是有用的**：IDE 与类型检查器能从方法体看出「这是抽象」，而 `...` 或 `pass` 会让某些工具误判。
2. **`save` / `delete` 没有返回类型标注** ——其余四个有（`bytes` / `Generator` / `None` / `bool`）。**两个无标注的方法在实现里的返回值也不一致**（有的返回 `None`，有的返回 SDK 的响应对象），于是**调用方不能依赖它们的返回值**。
3. **`load_stream` 返回裸 `Generator`（无类型参数）** ——于是「流里是什么」不可知。从实现看应当是 `bytes` 块，但抽象层没约束。**这是一个真实的类型缺口**：消费方必须自己判断。
4. **`download(filename, target_filepath)` 是「复制到本地文件」** ——它与 `load_once` 的区别是「不占内存」。**提供它说明这套抽象考虑了大文件**：一个 GB 级的原始文档不该被完整读进内存。

**两个可选方法用「默认抛 `NotImplementedError`」表达「不是所有后端都支持」。**

```python
def generate_presigned_url(
    self,
    filename: str,
    *,
    expires_in: int,
    content_type: str | None = None,
) -> str:
    """Generate a temporary direct-download URL when the backend supports it."""
    raise NotImplementedError("This storage backend doesn't support presigned URLs")

def scan(self, path, files=True, directories=False) -> list[str]:
    """
    Scan files and directories in the given path.
    This method is implemented only in some storage backends.
    If a storage backend doesn't support scanning, it will raise NotImplementedError.
    """
    raise NotImplementedError("This storage backend doesn't support scanning")
```

**五处：**

1. **它们不是 `@abstractmethod`** ——所以子类可以不实现。**这是「可选能力」在 Python 里的标准表达**：做不到就继承默认的「抛错」。
2. **异常消息带了一句解释**（"This storage backend doesn't support presigned URLs"）——**调用方拿到的是一个可读的原因，而不是空白的 `NotImplementedError`。** 这在跨很多后端的代码里很重要：错误消息必须自己说明「这是后端能力缺失」。
3. **`generate_presigned_url` 的关键字参数是 `*` 之后的** ——强制调用方写 `expires_in=`。**对一个「有效期」参数这是必要的**：位置传参容易把秒与内容类型搞混。
4. **`scan` 的参数没有类型标注**（`path` 无类型），而返回标了 `list[str]`。**同一签名里一半有标注一半没有** ——这是本文件的普遍状态。
5. **`scan` 的 docstring 重复了方法体已经表达的信息** ——但它加了一句「implemented only in some storage backends」，说明这是**多后端不一致的显式声明**。

**`_filter_scan_keys` 是抽象层里唯一有实现的函数，而它是一个纯函数。**

```python
@staticmethod
def _filter_scan_keys(keys: Iterable[str], files: bool, directories: bool) -> list[str]:
    """Filter recursive object listings; directory entries end with '/'."""
    if not files and not directories:
        raise ValueError("At least one of files or directories must be True")
    return [key for key in keys if (key.endswith("/") and directories) or (not key.endswith("/") and files)]
```

**四处：**

1. **它是一个 `@staticmethod` 且以下划线开头** ——所以它是**给子类用的工具**，不是接口的一部分。**把「多个后端都要写的过滤逻辑」放进抽象基类，是正确的复用位置。**
2. **「目录以 `/` 结尾」是对象存储的通用约定** ——S3、OSS、COS 都用这个约定表达「目录占位对象」。**这个约定被写进了 docstring，而不是留给每个后端自己猜。**
3. **两个参数都为 `False` 时 `raise ValueError`** ——**而不是返回空列表**。这是一个有意的选择：**「什么都不想要」几乎总是调用方的 bug**，返回空列表会把它静默掉。
4. **实现是一个列表推导，对每个 key 判断两次** ——逻辑正确且可读。

**而 `_filter_scan_keys` 的存在暴露了抽象层的一个真实职责：把「后端差异」压成一条约定。**

**十四个后端实现共享这套抽象，而工厂里的分派方式是「`match` + 函数内 import」。**

```python
@staticmethod
def get_storage_factory(storage_type: str) -> Callable[[], BaseStorage]:
    match storage_type:
        case StorageType.S3:
            from extensions.storage.aws_s3_storage import AwsS3Storage
            return AwsS3Storage
        case StorageType.OPENDAL:
            from extensions.storage.opendal_storage import OpenDALStorage
            return lambda: OpenDALStorage(dify_config.OPENDAL_SCHEME)
        case StorageType.LOCAL:
            from extensions.storage.opendal_storage import OpenDALStorage
            return lambda: OpenDALStorage(scheme="fs", root=dify_config.STORAGE_LOCAL_PATH)
        ...
        case _:
            raise ValueError(f"unsupported storage type {storage_type}")
```

**五处：**

1. **返回的是「工厂」而不是实例** ——`Callable[[], BaseStorage]`。这让 `init_app` 可以在 `app_context` 里才真正构造：

```python
def init_app(self, app: Flask):
    storage_factory = self.get_storage_factory(dify_config.STORAGE_TYPE)
    with app.app_context():
        self.storage_runner = storage_factory()
```

**「先取工厂、进上下文、再构造」的顺序说明某些后端的构造需要 Flask 上下文**（可能读 `current_app.config` 或建连接池）。

2. **函数内 import** ——于是未使用的后端模块不被加载。**在十四个后端的情况下这不只是风格，而是启动时间与依赖的实际节省**（比如不装 `azure-sdk` 也能跑 S3 部署）。
3. **`case StorageType.LOCAL` 复用 `OpenDALStorage`** ——**「本地存储」不是独立实现，而是 OpenDAL 的 `fs` scheme**：

```python
return lambda: OpenDALStorage(scheme="fs", root=dify_config.STORAGE_LOCAL_PATH)
```

**这与 i02 里 `STORAGE_TYPE` 把 `local` 标为 deprecated、`STORAGE_LOCAL_PATH` 带 `deprecated=True` 是同一件事的两端** ——本地存储正在被吸收进 OpenDAL。

4. **`case StorageType.OPENDAL` 传的是 `dify_config.OPENDAL_SCHEME`** ——于是「用 OpenDAL 的哪个后端」是另一层配置。**这意味着 `STORAGE_TYPE` 与 `OPENDAL_SCHEME` 是两个正交的开关，而只有 `STORAGE_TYPE=opendal` 时后者才有效。**
5. **`case _: raise ValueError`** ——未知类型立即失败。**这是对的**：配置错应当在启动时报，而不是在第一次上传文件时。

**`Storage` 这个门面类把「当前后端」藏在 `self.storage_runner` 里，而它有一个 `load` 方法带三个 `@overload`。**

```python
@overload
def load(self, filename: str, /, *, stream: Literal[False] = False) -> bytes: ...

@overload
def load(self, filename: str, /, *, stream: Literal[True]) -> Generator: ...

# Keep a bool fallback overload for callers that forward a runtime bool flag.
@overload
def load(self, filename: str, /, *, stream: bool = False) -> bytes | Generator: ...

def load(self, filename: str, /, *, stream: bool = False) -> bytes | Generator:
    if stream:
        return self.load_stream(filename)
    else:
        return self.load_once(filename)
```

**四处：**

1. **三个 `@overload` 是为了让类型检查器能推出「`stream=True` 得到 `Generator`，否则得到 `bytes`」** ——这是 Python 里表达「返回类型依赖参数值」的标准手法。
2. **第三个 overload 是给「运行时 bool」的** ——注释说得很清楚：「for callers that forward a runtime bool flag」。**没有它，那些调用方会被类型检查器拒绝。** 代价是「任何 `bool` 都能通过」，弱化了前两个的精度。
3. **实际实现只有 5 行** ——把 `stream` 折成二选一。**于是 `load` 与 `load_once` / `load_stream` 是两套并存的门**：调用方可以按需选方便的那个。
4. **`/` 之后的 `*` 同时用了 positional-only 与 keyword-only** ——`filename` 只能位置传、`stream` 只能关键字传。**这是一个刻意的签名收紧**，避免 `load("f", True)` 这种不可读的调用。

**而其余方法都是「一行转发」。**

```python
def save(self, filename: str, data: bytes):
    self.storage_runner.save(filename, data)

def exists(self, filename):
    return self.storage_runner.exists(filename)

def delete(self, filename: str):
    return self.storage_runner.delete(filename)
```

**一处细节**：`delete` 有 `return` 而 `save` 没有——**同一个类里两种风格**，反映「`delete` 的返回值可能有用（某些 SDK 返回被删对象信息）而 `save` 的不被使用」。**但抽象层没约定这一点**，所以调用方不该依赖 `delete` 的返回值。

**`im_integration/__init__.py` 的四十九个导出名是这一层唯一的对外承诺。**

```python
__all__ = [
    "ActiveRunDecision",
    "ActiveRunDecisionKind",
    ...
    "IMSyncRepository",
    ...
    "OpaqueProviderPayload",
    ...
]
```

**四处：**

1. **`__all__` 是**手工维护**的四十九项列表** ——**加一个公开类型就要回来补一项**，漏掉的表现是 `from ... import X` 对 IDE 与静态检查不可见（运行时仍能导入，因为模块级有那个名字）。**这是一个「约定强于机制」的地方。**
2. **导出的名字全部来自六个内部模块**（`binding_commands` / `change_log` / `ports` / `records` / `sync_reconciliation` / `sync_records`），**且顺序与 import 语句的顺序一致** ——于是这个文件读起来像一份目录。
3. **`adapters` 子目录没有被导出** ——**实现留在包内，协议与值对象对外。** 这是六边形架构的标准做法。
4. **没有 `__version__`、没有工厂函数、没有任何可执行语句** ——除了六条 import 与 `__all__`。**这个文件是纯声明。**

**`IMSyncRepository` 是一个 `Protocol` 而不是 ABC，六个方法全用 `...` 作方法体。**

```python
class IMSyncRepository(Protocol):
    """Transport-neutral command and query persistence required by IM sync."""

    def create_or_get_active_run(
        self,
        channel_revision: IMChannelRevision,
        *,
        organization_scope: DirectoryScope,
        sync_run_id: IMSyncRunId,
        started_by: IMSyncInitiator,
        now: NaiveDatetime,
    ) -> ActiveRunDecision:
        """Lock Channel and create or reuse a run, preserving its original initiator."""
        ...

    def load_sync_run(self, sync_run_id: IMSyncRunId) -> IMSyncRun | None:
        """Load one persisted run without acquiring the Organization write lock."""
        ...
```

**六处：**

1. **`Protocol` 意味着「结构化子类型」** ——实现者**不需要继承**这个类，只要方法签名匹配就算实现。**这对「适配器在其他包里」的场景是必需的**（不能让 repositories 层依赖 core 层的类）。
2. **方法体是 `...`** ——而不是 `raise NotImplementedError`。**因为 `Protocol` 不会被实例化**，方法体永远不会执行。
3. **每个方法的 docstring 都在说「副作用边界」** ——「Lock Channel and create or reuse a run, preserving its original initiator」/「without acquiring the Organization write lock」/「Page one required product-result bucket for one exact run」/「without exposing raw payload or ORM state」。**这是端口设计的关键：签名只表达「做什么」，docstring 表达「加了什么锁、暴露了什么」。**
4. **`create_or_get_active_run` 有五个关键字参数（`*` 之后）** ——强制命名调用。**对一个「语义重的命令方法」这是对的**：位置参数会让 `organization_scope` 与 `sync_run_id` 混淆。
5. **`now: NaiveDatetime` 是显式传入的** ——**不读系统时钟**。这是可测试性与确定性的关键：**时间是一个输入，不是环境**。而 `NaiveDatetime`（Pydantic 的类型）说明这个域**故意不用时区感知时间**（与 h05 里 `ensure_naive_utc` 的处理形成呼应）。
6. **`load_sync_run` 返回 `IMSyncRun | None` 而不是抛错** ——「找不到」是一个正常结果。**而 `page_sync_results` 返回非 Optional 的 `SyncResultPage`** ——因为「空页」是一个合法结果，不需要用 `None` 表达。**两个方法的失败表达方式不同，且理由充分。**

**`ActiveRunDecision` 是一个三分支的判别联合的「手工实现」。**

```python
class ActiveRunDecisionKind(StrEnum):
    """Outcome of Channel-locked sync run creation."""

    # No active run existed and a new revision-bound synchronization run was committed.
    CREATED = "created"

    # An active run already exists, so the repository returns it instead of creating a duplicate.
    EXISTING_ACTIVE = "existing_active"

    # The captured Channel revision is no longer current and no new run may be created from it.
    STALE_REVISION = "stale_revision"


@dataclass(frozen=True, slots=True)
class ActiveRunDecision:
    """New or existing active run, or a stable stale-revision rejection."""

    kind: ActiveRunDecisionKind
    run: IMSyncRun | None
    stale_revision: StaleRevision | None = None
```

**五处：**

1. **三个枚举成员各配一条注释说明「什么情况下产生它」** ——**这是「枚举值即状态转移」的正确文档方式**：读者不用去实现里找。
2. **`run: IMSyncRun | None` 与 `stale_revision: StaleRevision | None = None`** ——**两个可选字段对应三个 kind**，而它们的合法组合是：

| `kind` | `run` | `stale_revision` |
|---|---|---|
| `CREATED` | 非 None（刚建的） | `None` |
| `EXISTING_ACTIVE` | 非 None（已有的） | `None` |
| `STALE_REVISION` | `None` | 非 None |

**代码没有在 `__post_init__` 里校验这个组合** ——于是「三分支判别联合」是靠**约定**而非类型保证的。README 级的注释已经说了（"New or existing active run, or a stable stale-revision rejection"），但没有可执行的约束。

3. **`frozen=True, slots=True`** ——不可变 + 省内存。**对一个「函数返回值」这是正确的**：它不该被修改，且数量可能很多。
4. **`stale_revision` 有默认值而 `run` 没有** ——因为 `STALE_REVISION` 分支里 `run` 恒为 `None` 但必须显式传 `None`。**这个默认值让「只关心 stale 的分支」少写一个参数**，但也让「忘记传 run」不会被类型检查器发现（`run` 没有默认值，所以其实会）。**实际上两个字段都必须传，默认值只对 `stale_revision` 有用。**
5. **`ActiveRunDecisionKind` 用 `StrEnum`** ——于是可以直接序列化进 JSON（这对「同步状态要展示给前端」的场景必要）。

**`ApplyReconciliationStatus` 有九个成员，而它的注释把「哪些是正常、哪些是异常」分开了。**

```python
class ApplyReconciliationStatus(StrEnum):
    """Stable outcome of one idempotent revision-guarded apply."""

    # The plan was applied to current state and its append-only result facts were committed.
    APPLIED = "applied"

    # This completed run was applied earlier; persisted facts are returned for idempotent replay.
    ALREADY_APPLIED = "already_applied"

    # Channel configuration changed after capture, so the plan cannot mutate current state.
    STALE_REVISION = "stale_revision"

    LOCK_UNAVAILABLE = "lock_unavailable"
    LOCK_LOST = "lock_lost"
    PRECONDITION_FAILED = "precondition_failed"
    DIRECTORY_READ_FAILED = "directory_read_failed"
    PLAN_BLOCKED = "plan_blocked"
    UNEXPECTED_APPLY_FAILURE = "unexpected_apply_failure"
```

**五处：**

1. **九个值里只有前三个有注释** ——**这本身是一个信号**：前三个是「业务语义」（应用成功 / 幂等重放 / 版本过期），后六个是「技术性失败」。**注释的分布反映了作者认为哪些值需要解释。**
2. **`APPLIED` 与 `ALREADY_APPLIED` 分开** ——**这是幂等设计的核心区分**：调用方需要知道「这次真的改了」还是「上次已经改过」。**若合并成一个 `SUCCESS`，审计日志就无法回答「哪一次是真的写」。**
3. **`LOCK_UNAVAILABLE` 与 `LOCK_LOST` 分开** ——前者是「一开始就拿不到」，后者是「拿到了但中途丢了」。**这两种情况的处理不同**（前者可立即重试，后者必须重新读状态）。
4. **`PLAN_BLOCKED` 与 `PRECONDITION_FAILED` 分开** ——前者是「计划本身有阻塞项」（业务原因），后者是「执行时前置条件不满足」（并发原因）。
5. **`UNEXPECTED_APPLY_FAILURE` 作为兜底** ——**有一个「其它」值意味着所有异常都被分类，不会漏出到异常通道。** 这与 i03 里「能由重投递解释的静默返回、不能的上抛」是两种不同的策略：**这里把所有失败都变成状态值**。

**`ApplyReconciliationResult` 把「变更计数」与「结果计数」分开。**

```python
@dataclass(frozen=True, slots=True)
class ApplyReconciliationResult:
    """Transport-neutral persisted outcome of one conditional plan apply."""

    status: ApplyReconciliationStatus
    sync_run_id: IMSyncRunId
    committed_at: NaiveDatetime | None
    result_count: int
    change_count: int
    warnings: tuple[ResolvedReconciliationWarning, ...]
```

**四处：**

1. **`result_count` 与 `change_count` 是两个不同的量** ——前者是「追加的结果事实数」，后者是「修改的当前状态数」。**混为一谈会让「什么都没改但记了很多诊断」与「改了很多但没记诊断」不可区分。**
2. **`committed_at: NaiveDatetime | None`** ——**未提交时为 `None`**。于是「状态 + 是否提交」是两个字段，而不是用「`status == APPLIED` 且 `committed_at 非 None`」推导。
3. **`warnings` 是 `tuple` 而不是 `list`** ——不可变容器，与 `frozen=True` 匹配。**这是这套代码的一致风格**：所有值对象里的集合都是 tuple。
4. **`ResolvedReconciliationWarning` 里带了 `identity_ids` 与 `contact_ids` 两个 tuple** ——**一个警告可以关联多个对象**，于是「同一原因导致的十个身份问题」是一条警告而不是十条。

**`OpaqueProviderPayload` 是一个 `RootModel`，而它的「不透明」是显式的。**

```python
class OpaqueProviderPayload(RootModel[dict[str, JsonValue]]):
    """Opaque JSON retained by historical synchronization facts."""

    model_config = ConfigDict(frozen=True, strict=True, validate_default=True)

    # TODO(QuantumGhost): Remove from_mapping and to_mapping.
    @classmethod
    def from_mapping(cls, values: Mapping[str, JsonValue]) -> OpaqueProviderPayload:
        return cls(dict(values))

    def to_mapping(self) -> dict[str, JsonValue]:
        return dict(self.root)
```

**六处：**

1. **`RootModel[dict[str, JsonValue]]`** ——**包装一个裸 dict 而不是定义字段**。于是这个类型**不能被「按字段访问」**，只能整体存取。**这正是「不透明」的类型级表达**：你拿不到 `payload["user_id"]`，除非显式转成 dict。
2. **`frozen=True`** ——不可变。**于是「保留的原始 payload」不会被下游代码意外改动。**
3. **`strict=True`** ——**Pydantic 的严格模式**：不做隐式类型转换（`"1"` 不会被转成 `1`）。**对一个「原样保留」的容器这是必须的**——任何转换都会破坏「原样」。
4. **`validate_default=True`** ——默认值也要校验。对一个没有默认字段的 `RootModel` 这可能是防御性的。
5. **`from_mapping` / `to_mapping` 带 TODO 要求删除** ——**因为 `RootModel` 本身就支持 `cls(values)` 与 `.root`**，这两个包装是多余的。**TODO 里带了作者名（`QuantumGhost`）**，这是一种「认领待办」的做法。
6. **`dict(values)` 与 `dict(self.root)` 都做了浅拷贝** ——于是外部持有的 dict 与本对象不共享最外层。**注意是浅拷贝**：嵌套的 list/dict 仍共享。**对一个 `frozen=True` 的对象，浅拷贝不足以防住「通过嵌套列表修改内容」**，但 `strict=True` 加 Pydantic 的验证会在构造时生成新结构（实际上 `RootModel` 会对嵌套值做校验复制）。**这里的保证程度需要读 Pydantic 的行为才能确定，而代码没有说明。**

**`__init__.py` 的模块 docstring 只有一行，而它是这一层的设计宣言。**

```python
"""Infrastructure-free historical synchronization and reconciliation boundary."""
```

**三个词：**

- **`Infrastructure-free`** ——不依赖具体基础设施（DB/HTTP/MQ）；
- **`historical synchronization`** ——同步的是「历史」数据（IM 的历史联系人与身份）；
- **`reconciliation boundary`** ——它是「对账」的边界。

**而 `ports.py` 的 docstring 补上了「谁负责什么」：**

```python
"""Transaction-oriented persistence ports for IM Control Plane invariants.

Implementations own CAS predicates, Channel row locks, eager loading,
revision-guarded apply, rollback, and append-only result persistence. Generic
table CRUD is intentionally absent.
"""
```

**「Implementations own ...」这一句列出了实现的全部责任：CAS 谓词、Channel 行锁、预加载、版本守卫的应用、回滚、只追加的结果持久化。** 这是一份**责任移交清单**：**核心域不关心这些，全部由适配器承担。**

**而「Generic table CRUD is intentionally absent」是本页最值得记的一句设计判断**——见「关键取舍」。

## 关键取舍

**源码事实：** `BaseStorage` 六个必实现方法（`save` / `load_once` / `load_stream` / `download` / `exists` / `delete`）全为 `@abstractmethod` 且方法体 `raise NotImplementedError`；两个可选方法（`generate_presigned_url` / `scan`）不是抽象方法，默认抛带解释消息的 `NotImplementedError`。

**源码事实：** `BaseStorage` 的 `save` 与 `delete` 没有返回类型标注，其余四个有；`load_stream` 返回裸 `Generator`（无类型参数）；`scan` 的 `path` 参数无标注而返回标 `list[str]`。

**源码事实：** `_filter_scan_keys` 是唯一有实现的成员，`@staticmethod` + 下划线开头（供子类使用），以「key 是否以 `/` 结尾」区分目录与文件，`files` 与 `directories` 都为 `False` 时抛 `ValueError` 而非返回空列表。

**源码事实：** `Storage.get_storage_factory` 用 `match` + 函数内 import 返回工厂 `Callable[[], BaseStorage]`；`local` 复用 `OpenDALStorage(scheme="fs", root=STORAGE_LOCAL_PATH)`；`opendal` 用 `OpenDALStorage(dify_config.OPENDAL_SCHEME)`；未知类型 `raise ValueError`。`init_app` 先在 `app_context` 内构造实例。

**源码事实：** `Storage.load` 有三个 `@overload`（`stream: Literal[False]` → `bytes`、`Literal[True]` → `Generator`、运行时 `bool` → 联合），实现按 `stream` 二选一；签名同时用 `/` 与 `*` 收紧位置与关键字参数。

**源码事实：** `im_integration/__init__.py` 是纯再导出文件，`__all__` 手工列出四十九个名字，全部来自六个内部模块（`binding_commands` / `change_log` / `ports` / `records` / `sync_reconciliation` / `sync_records`），`adapters` 子包不导出；模块 docstring 为 `Infrastructure-free historical synchronization and reconciliation boundary.`。

**源码事实：** `IMSyncRepository` 是 `Protocol`（非 ABC），六个方法体为 `...`，`create_or_get_active_run` 的五个参数全为关键字参数，`now: NaiveDatetime` 由调用方传入（不读系统时钟）；`load_sync_run` 返回 `IMSyncRun | None` 而 `page_sync_results` 返回非 Optional 的 `SyncResultPage`。

**源码事实：** `IMSyncRepository` 的模块 docstring 声明「Implementations own CAS predicates, Channel row locks, eager loading, revision-guarded apply, rollback, and append-only result persistence. Generic table CRUD is intentionally absent.」

**源码事实：** `ActiveRunDecisionKind` 三值（`CREATED` / `EXISTING_ACTIVE` / `STALE_REVISION`）各配条件注释；`ActiveRunDecision` 为 `frozen=True, slots=True` dataclass，含 `kind` + `run: IMSyncRun | None` + `stale_revision: StaleRevision | None = None`，三个 kind 与两个可选字段的合法组合未被 `__post_init__` 校验。

**源码事实：** `ApplyReconciliationStatus` 九值，前三个（`APPLIED` / `ALREADY_APPLIED` / `STALE_REVISION`）有注释、后六个无；`APPLIED` 与 `ALREADY_APPLIED` 分开以区分「本次真的写」与「幂等重放」；`LOCK_UNAVAILABLE` 与 `LOCK_LOST` 分开；`UNEXPECTED_APPLY_FAILURE` 作为兜底把异常也变成状态值。

**源码事实：** `ApplyReconciliationResult` 的 `result_count` 与 `change_count` 分别表示「追加的结果事实数」与「修改的当前状态数」；`committed_at` 可为 `None`；集合字段全为 `tuple`。

**源码事实：** `OpaqueProviderPayload` 是 `RootModel[dict[str, JsonValue]]`，`ConfigDict(frozen=True, strict=True, validate_default=True)`；`from_mapping` / `to_mapping` 带「TODO(QuantumGhost): Remove」注释（因 `RootModel` 原生支持构造与 `.root`）。

**架构推论：** 抽象基类的厚度应当由「后端差异有多大」决定。`BaseStorage` 只声明七个动词加一个纯函数过滤器，**把「所有一致性风险留给实现」**——于是每个后端都要自己处理「流式读取的块大小」「下载的原子性」「删除的不存在语义」。**这是正确的取舍**（十四个云的语义差异太大，抽象层强行统一会造出一个最弱公分母），但它意味着**跨后端的一致行为只能靠测试矩阵保证**，而代码里没有这份矩阵的痕迹。

**架构推论：** 「端口故意不提供通用 CRUD」是一条值得抄的立场。`IMSyncRepository` 六个方法全是业务动作（创建或复用运行、按版本守卫地应用计划、追加诊断事实），**没有任何 `insert` / `update` / `select`**。这让适配器无法「顺手查点别的」，也让核心域无法把业务逻辑漏到 SQL 里。**代价是每加一个用例就要加一个方法**，而这正是想要的：**端口的每一次扩张都应当是一次设计决定，而不是一次便利。**

**架构推论：** 「把所有失败都变成状态值」与「把可解释的失败静默、不可解释的上抛」是两种对立策略，而它们适用于不同的位置。这里是前者（九值状态，含 `UNEXPECTED_APPLY_FAILURE` 兜底），因为**对账是一个批处理流程，单条失败不该中断整批，且失败原因需要被审计**。而 i03 的任务入口用后者，因为**任务重投递的幂等性需要靠「静默返回」实现**。**判据是「失败是否需要被人逐条阅读」。**

**架构推论：** `__all__` 手工维护四十九项是「边界声明靠人力」的典型。它的风险是**漏加**（新类型不可见但不报错）与**多留**（删了实现却忘了删导出）。前者可以在 CI 里用「包内所有公开名 ⊆ `__all__`」的检查消掉，而这项检查的成本很低——**任何手工维护的清单都应当有一条自动对拍。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/extensions/storage/base_storage.py` | `BaseStorage` | 存储后端抽象：六个必实现 + 两个可选能力 |
| `api/extensions/storage/base_storage.py` | `BaseStorage.generate_presigned_url` | 可选：临时直链（默认抛出带解释的 `NotImplementedError`） |
| `api/extensions/storage/base_storage.py` | `BaseStorage.scan` | 可选：目录扫描（默认不支持） |
| `api/extensions/storage/base_storage.py` | `BaseStorage._filter_scan_keys` | 供子类复用的「按 `/` 结尾区分目录」过滤器 |
| `api/extensions/storage/base_storage.py` | `BaseStorage.load_once` / `load_stream` / `download` | 三种读取形态（全量 / 流式 / 落地到本地文件） |
| `api/core/human_input_v2/im_integration/__init__.py` | 模块 `__all__` | 四十九个公开名的边界声明（手工维护） |
| `api/core/human_input_v2/im_integration/ports.py` | `IMSyncRepository` | 传输无关的持久化端口（六个业务方法，无通用 CRUD） |
| `api/core/human_input_v2/im_integration/ports.py` | `ActiveRunDecisionKind` / `ActiveRunDecision` | 「建了 / 已存在 / 版本过期」三分支结果 |
| `api/core/human_input_v2/im_integration/ports.py` | `ApplyReconciliationStatus` | 九值应用结果（含 `UNEXPECTED_APPLY_FAILURE` 兜底） |
| `api/core/human_input_v2/im_integration/ports.py` | `ApplyReconciliationResult` | 应用结果值对象（结果数 / 变更数 / 警告 / 提交时刻） |
| `api/core/human_input_v2/im_integration/ports.py` | `ResolvedReconciliationWarning` | 一条警告聚合多个身份与联系人 id |
| `api/core/human_input_v2/im_integration/records.py` | `OpaqueProviderPayload` | 不透明 JSON 值对象（严格模式 + 冻结） |
| `api/extensions/ext_storage.py` | `Storage.get_storage_factory` | 十四种存储后端的工厂分派（函数内 import） |
| `api/extensions/ext_storage.py` | `Storage.load` | 三个 `@overload` 的按需全量/流式读取 |
| `api/extensions/ext_storage.py` | `storage` / `init_app` | 全局门面单例与初始化（含文件运行时） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [Python 文档：抽象基类与 Protocol](https://docs.python.org/3/library/abc.html)
- [Pydantic 文档：RootModel 与严格模式](https://docs.pydantic.dev/latest/concepts/models/#rootmodel-and-custom-root-types)
- [OpenDAL 文档：统一存储访问层](https://opendal.apache.org/docs/)

## 与主核 / AI 的关系

存储抽象与端口设计在主核（多模态数据库内核）里分别对应**抽象存储接口**（smgr / vfd）与**模块边界**（spi / hooks）。四条经验。

**第一，抽象层的厚度应当匹配后端差异的幅度，且缺口要用测试矩阵补。** `BaseStorage` 只声明动词不规定行为，于是「删除不存在的文件」在不同后端有不同表现。主核里 `smgr` 同样只声明 `read` / `write` / `fsync`，而行为契约靠 `regression` 测试固定。**任何只声明动词语义、不声明行为语义的抽象，都必须配一份「跨实现行为一致性」的测试**，否则它只是让编译通过。

**第二，端口要有意地不提供通用 CRUD。** `IMSyncRepository` 的六个方法全是业务动作，注释明说「Generic table CRUD is intentionally absent」。主核里 spi 扩展接口同理：**给扩展一个 `execute_sql()` 就等于把内部 schema 变成公开 API**。**端口每加一个方法都应当是一次设计评审，而通用 CRUD 会让这次评审显得「没必要」。**

**第三，幂等的成功必须与「真的做了什么」分开编码。** `APPLIED` 与 `ALREADY_APPLIED` 分开，`LOCK_UNAVAILABLE` 与 `LOCK_LOST` 分开——**这些区分让审计日志能回答「这一次到底改了什么」**。主核里做重放（WAL 重放、逻辑复制去重、幂等 DDL）时同理：**把「做成了」与「本来就是这样」合并成一个成功状态，会让事后归因变成猜测。**

**第四，手工维护的导出清单必须有自动对拍。** 四十九项 `__all__` 是这一层唯一的对外承诺，而漏加一项不会报错、只会让类型检查看不见。主核里凡是有「符号版本脚本 / 导出表 / ABI 清单」的地方都同理：**清单是人写的，一致性必须由机器验。**
