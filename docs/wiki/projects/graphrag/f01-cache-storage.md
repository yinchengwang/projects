---
project: graphrag
feature: cache-storage
title: 缓存实现与键
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag-cache/graphrag_cache/cache.py
    symbols: [Cache]
  - path: packages/graphrag-cache/graphrag_cache/cache_key.py
    symbols: [CacheKeyCreator]
  - path: packages/graphrag-cache/graphrag_cache/sqlite_cache.py
    symbols: [SQLiteCache]
  - path: packages/graphrag-cache/graphrag_cache/noop_cache.py
    symbols: [NoopCache]
---

# 缓存实现与键

## 一句话职责

定义一个**七个异步方法 + 一个 `child` 分片**的缓存契约，并给出四种实现：JSON 文件、内存、SQLite、以及**什么都不做的 `NoopCache`**——后者不是占位符，而是「关掉缓存」这一配置的零成本实现。

## 机制是什么

`Cache` 的契约只有八个成员，而它们的设计全部围绕两件 graphrag 特有的事：

| 设计 | 原因 |
|---|---|
| **全部异步** | 后端可能是远程存储（Azure Blob），同步接口会阻塞 |
| **`child(name)` 返回同类型实例** | 同一份缓存里要隔离出多个命名空间（`chat` / `embedding` / 各工作流） |
| **`debug_data` 可随值存** | 缓存出来的条目要能被人工检查（存原始提示与元数据） |
| **`get_many` 有默认实现** | 批量是可选优化，不是每个后端都必须原生支持 |

**四种实现覆盖了不同的成本取舍：**

| 实现 | 持久化 | 跨进程 | 成本 | 典型用途 |
|---|---|---|---|---|
| `JsonCache` | 是（每键一个文件） | 是 | 每键一次文件 I/O | 默认，可人工查看 |
| `MemoryCache` | 否 | 否 | 一次字典查找 | 单进程短跑 |
| `SQLiteCache` | 是（单文件） | 是 | 一次 SQL | 大量条目、需要原子性与并发 |
| `NoopCache` | 否 | 否 | **零** | 关掉缓存 |

**而 `NoopCache` 的位置需要单独说。** 它是 `CacheType.Noop` 的实现，而那个枚举值是 `"none"` —— 也就是说**配置里写 `none` 而不是 `noop`**。这暴露了它的真实语义：它不是「一种缓存」，而是「不缓存」这个选项。

## 怎么实现的

### 一、`Cache` 契约：`child` 是最不寻常的一个

**七个抽象方法都很常规（`get` / `set` / `has` / `delete` / `clear` + 抽象构造），但 `child` 是这套设计的枢纽。**

```
@abstractmethod
def child(self, name: str) -> Cache:
    """Create a child cache with the given name."""
```

**注意它是**同步**的（不是 `async def`）** —— 因为它只构造一个新对象，不做 I/O。

**它的语义是「同一个物理缓存里的一个命名空间视图」**：

- `SQLiteCache.child("chat")` 返回一个 `namespace = "{parent}/chat"` 的新实例，共享同一个数据库文件。
- `NoopCache.child(...)` 直接 `return self` —— 因为无操作对子命名空间同样无操作。
- `MemoryCache` / `JsonCache` 各自实现自己的分片方式。

**为什么需要命名空间：** 同一份缓存里要存三类完全不同的东西——补全响应、嵌入向量、以及各工作流的中间结果。它们的键空间（哈希值）可能碰撞（虽然概率极低），更重要的是**它们需要独立清空**（换提示模板只想清 `chat` 那份）。

**而 `child` 的返回类型是 `Cache` 而不是 `Self`** —— 这是一个有意的放宽：子缓存不必与父缓存同类型（虽然现有实现都同类型）。

**`get_many` 有默认实现，而它是「并发逐个取」。**

```
async def get_many(self, keys: Sequence[str]) -> dict[str, Any]:
    """Return existing cache values keyed by cache key.
    Missing or invalid entries are omitted. Duplicate keys appear at most once.
    """
    unique_keys = list(dict.fromkeys(keys))
    values = await asyncio.gather(*(self.get(key) for key in unique_keys))
    return {key: value for key, value in zip(unique_keys, values, strict=True) if value is not None}
```

**四个决定：**

1. **`dict.fromkeys(keys)` 去重且保序** —— 于是「重复键最多出现一次」这个契约由框架保证，后端不必操心。
2. **`asyncio.gather` 并发** —— 对远程后端意义重大（100 个键的串行往返不可接受）。
3. **`zip(..., strict=True)`** —— `values` 与 `unique_keys` 长度必然相同（`gather` 与去重后的列表一一对应），`strict=True` 把这个不变量变成断言。
4. **`if value is not None` 过滤** —— 缺失的键**不出现在结果里**（而不是映射到 `None`）。于是调用方用 `key in result` 判断命中。

**这三条契约（去重、省略缺失、并发）写在一个默认实现里，就保证了四种后端的行为一致。** 而 `SQLiteCache` 与 `MemoryCache` 都覆盖了它做原生批量——**默认实现是「正确但慢」的兜底，不是唯一路径。**

**`set` 有一个 `debug_data` 参数。**

```
@abstractmethod
async def set(self, key: str, value: Any, debug_data: dict | None = None) -> None:
```

**它让缓存条目附带可选的诊断信息** —— `SQLiteCache` 把它合并进存储的 JSON（见下），于是可以事后查出「这条缓存是从什么提示、什么参数产生的」。**这是一个「为可调试性预留的通道」**，而它在默认的 `JsonCache` 里未必用到。

### 二、`SQLiteCache`：并发安全靠 WAL，正确性靠「条件删除」

**它是最讲究的实现，而构造期有三重校验。**

```
if storage is None:
    msg = "SQLiteCache requires either a Storage instance to be provided or a StorageConfig to create one."
    raise ValueError(msg)
if not isinstance(storage, Storage):
    storage = create_storage(StorageConfig(**storage))
if not isinstance(storage, FileStorage) or isinstance(storage, MemoryStorage):
    msg = "SQLiteCache only supports FileStorage."
    raise TypeError(msg)
```

**第一条拒 `None`、第二条把 dict 配置就地转成 `Storage`、第三条拒非文件存储。**

**第三条的写法值得注意：`not isinstance(storage, FileStorage) or isinstance(storage, MemoryStorage)`。** 若 `MemoryStorage` 是 `FileStorage` 的子类，则后半句把「内存存储」排除；若不是子类，则前半句已经排除。**这个双重判断暗示了两者的继承关系可能是前者**，而写法的意图是「无论继承关系如何，内存存储都不接受」。

**而 `database_name` 的路径校验非常严格。**

```
database_paths = (PurePosixPath(database_name), PureWindowsPath(database_name))
if (not database_name or database_name == ".."
    or any(path.is_absolute() or len(path.parts) != 1 or path.name != database_name for path in database_paths)):
    msg = "SQLiteCache database_name must be a file name without a directory."
    raise ValueError(msg)
```

**它用 POSIX 与 Windows 两套路径语义各解析一次，要求两边都满足「单段、非绝对、名字即原值」。** 这防的是「在一个平台上合法在另一个平台上逃逸」的路径（如 Windows 的 `C:\x` 在 POSIX 下是一个普通文件名）。**这是跨平台路径校验的正确写法** —— 而它与 `FileTemplateManager` 完全不校验模板名形成对照（见 e04 那页）：**差别在于这里的 `database_name` 来自配置文件（可能被用户随意填），而模板名是代码常量。**

**并发安全靠两条 PRAGMA。**

```
@contextmanager
def _connect(self) -> Generator[sqlite3.Connection]:
    connection = sqlite3.connect(self._database_path, timeout=30)
    connection.execute("PRAGMA busy_timeout = 30000")
    try:
        with connection:
            yield connection
    finally:
        connection.close()

def _initialize(self) -> None:
    with self._connect() as connection:
        connection.execute("PRAGMA journal_mode = WAL")
        connection.execute("PRAGMA synchronous = NORMAL")
        connection.execute("""
            CREATE TABLE IF NOT EXISTS cache_entries (
                namespace TEXT NOT NULL, key TEXT NOT NULL, value_json TEXT NOT NULL,
                PRIMARY KEY (namespace, key)
            )
        """)
```

**四个细节：**

1. **`timeout=30` 与 `busy_timeout = 30000` 都设了** —— 前者是 Python 层的重试超时，后者是 SQLite 层的。**两个都要设**是因为它们在不同层面生效。
2. **`journal_mode = WAL`** —— 写不阻塞读，这是多进程并发读缓存的必要条件。
3. **`synchronous = NORMAL`** —— **这是正确性上的放宽**：NORMAL 模式下 WAL 的提交不保证落盘（崩溃可能丢最后几个事务）。对一个缓存来说这是合理的（丢了就重算），但这是一个有意识的取舍而不是疏忽。
4. **`PRIMARY KEY (namespace, key)`** —— 命名空间是主键的一部分，于是 `child()` 出来的实例**共享同一张表**而互不干扰。

**`_connect` 里 `with connection:` 的位置很关键。**

```
try:
    with connection:
        yield connection
finally:
    connection.close()
```

**`with connection:` 是 SQLite 的事务上下文** —— 正常退出时 `commit`，异常时 `rollback`。而它包着 `yield`，于是**调用方在 `with self._connect() as conn:` 块内执行的所有语句属于同一个事务**。

**若把 `with connection:` 写成 `with connection as conn:` 就错了**（那会返回 connection 而不是事务）。当前写法让 `yield` 的仍是 `connection`，同时借它的上下文管理器管事务。

**而每次操作都新建连接** —— `_get` / `_set` / `_has` / `_delete` 各自 `with self._connect()`。**没有连接池**，于是每次开销是一次 `sqlite3.connect`（对本地文件很便宜）。**这个选择换来的是线程安全**：连接不能跨线程共享，而每次新建就没有共享。

**异步包装全部是 `asyncio.to_thread`。**

```
async def get(self, key: str) -> Any | None:
    payload = await asyncio.to_thread(self._get, key)
    ...
```

**这暴露了「异步契约 + 同步后端」的适配方式**：真正的 I/O 在默认的线程池里跑，事件循环不被阻塞。**代价是每次操作一次线程调度**（对 SQLite 的微秒级查询来说，线程调度可能比查询本身更贵）。

**`get` 的无效条目的处理是这一页最讲究的一处。**

```
if payload is None:
    return None
try:
    data = json.loads(payload)
except json.JSONDecodeError:
    await asyncio.to_thread(self._delete_if_unchanged, key, payload)
    return None
if not isinstance(data, dict) or "result" not in data:
    await asyncio.to_thread(self._delete_if_unchanged, key, payload)
    return None
return data["result"]
```

**两类无效：JSON 解析失败，以及「解析出来但不是预期的 `{"result": ...}` 结构」。两者都**删除该条目**并返回 `None`。**

**为什么是「条件删除」（`_delete_if_unchanged`）而不是直接 `delete`？**

```
def _delete_if_unchanged(self, key: str, payload: str) -> None:
    with self._connect() as connection:
        connection.execute(
            "DELETE FROM cache_entries WHERE namespace = ? AND key = ? AND value_json = ?",
            (self._namespace, key, payload),
        )
```

**`WHERE ... AND value_json = ?` 让删除只在「值仍是我们读到的那个」时才生效。**

**这防的是一个具体的竞态**：线程 A 读到一条坏数据，准备删除；与此同时线程 B 已经用正确的数据覆盖了这个键。若 A 无条件删除，就会**删掉 B 刚写的好数据**。**条件删除让「删坏数据」变成一次带版本检查的 CAS。**

**这是一个很少见但非常正确的细节** —— 大多数缓存实现会直接 `delete(key)`，而它在「读坏数据 → 删除」这个路径上引入了写-写冲突的可能。

**而 `get_many` 的批处理受 SQLite 参数上限约束。**

```
# This stays below SQLite's legacy 999-parameter limit, including the namespace.
SQLITE_BATCH_SIZE = 500
...
for start in range(0, len(keys), SQLITE_BATCH_SIZE):
    batch = keys[start : start + SQLITE_BATCH_SIZE]
    placeholders = ", ".join("?" for _ in batch)
    rows = connection.execute(f"SELECT key, value_json FROM cache_entries WHERE namespace = ? AND key IN ({placeholders})", (self._namespace, *batch)).fetchall()
```

**500 而不是 999 是因为还要算上 `namespace` 那一个参数** —— 注释明写「包括命名空间在内保持在 SQLite 旧的 999 参数限制之下」。

**而 `f-string` 拼 SQL 用了 `# noqa: S608`** —— 那是「可能的 SQL 注入」告警。**这里是安全的**：拼进去的只有 `?` 占位符的数量（`", ".join("?" for _ in batch)`），值全部通过参数传递。

**而 `get_many` 的删除也批量做。**

```
invalid_entries: list[tuple[str, str]] = []
...
self._delete_many_if_unchanged(connection, invalid_entries)
```

**注意它在同一个 `with self._connect()` 块内做删除** —— 于是查询与删除在同一事务里。**这与 `get` 的单条路径（先查、`await`、再删，两次连接）不同**：批量版把「读-判定-删」收在一个事务里，从而完全避免了中间被改写的窗口。**这是批量版比逐条版更严格的地方。**

**`set` 拒绝 `None` 值。**

```
async def set(self, key: str, value: Any, debug_data: dict | None = None) -> None:
    if value is None:
        return
    data = {"result": value, **(debug_data or {})}
    payload = json.dumps(data, ensure_ascii=False)
    await asyncio.to_thread(self._set, key, payload)
```

**三个决定：**

1. **`value is None` 直接返回** —— 于是「缓存 None」是不可能的。这与 `get` 用 `None` 表示「未命中」一致（否则无法区分「缓存了 None」与「没有这条」）。
2. **`{"result": value, **(debug_data or {})}`** —— `debug_data` 被合并进同一个 dict，**而 `result` 在前**。若 `debug_data` 里也有 `result` 键，会覆盖真值——这是一个未加保护的冲突。
3. **`ensure_ascii=False`** —— 中文直接存 UTF-8 而不是 `\uXXXX`，于是文件可读、体积更小。

**写入用 upsert：**

```
INSERT INTO cache_entries(namespace, key, value_json) VALUES (?, ?, ?)
ON CONFLICT(namespace, key) DO UPDATE SET value_json = excluded.value_json
```

**而 `clear` 只清当前命名空间（`DELETE ... WHERE namespace = ?`），不是清空整表。** 这与 `child` 的语义配套：清掉 `chat` 不该清掉 `embedding`。

**`child` 的命名空间拼接值得记。**

```
def child(self, name: str) -> Cache:
    namespace = f"{self._namespace}/{name}" if self._namespace else name
    return SQLiteCache(storage=self._storage, database_name=self._database_name, namespace=namespace)
```

**用 `/` 分隔且处理了空父命名空间，于是形成 `a/b/c` 的路径式层级。** 但 `clear` 用的是**精确匹配** `namespace = ?`，所以**清 `a` 不会清 `a/b`** —— 命名空间是平铺的字符串，不是树。

### 三、`NoopCache`：一次 `return self` 的价值

**它的全部实现在于「什么都不做，且返回类型正确」。**

```
class NoopCache(Cache):
    """A no-op implementation of Cache, usually useful for testing."""

    async def get(self, key: str) -> Any: return None
    async def get_many(self, keys: Sequence[str]) -> dict[str, Any]: return {}
    async def set(self, key, value, debug_data=None) -> None: ...
    async def has(self, key: str) -> bool: return False
    async def delete(self, key: str) -> None: ...
    async def clear(self) -> None: ...
    def child(self, name: str) -> "Cache": return self
```

**文档字符串说「通常对测试有用」，但它的实际角色更重要：它是配置里 `cache.type = none` 时的实现**（`CacheType.Noop = "none"`）。**于是「关掉缓存」不需要在调用点写 `if cache is not None`** —— 传一个 `NoopCache` 即可，代码路径完全一致。

**`child` 返回 `self` 而不是新建实例** —— 因为无操作对任何命名空间都相同，共享一个实例是正确且最省的。

**而 `get_many` 覆盖了默认实现。**

**这不是性能优化，而是语义修复**：默认实现会 `gather` N 个 `get`（每个返回 `None`）再过滤，结果是 `{}`。**结果相同但白做了 N 次协程调度。** `NoopCache` 直接返回 `{}`。

**这是「空实现也要优化」的一个好例子**：既然它的全部价值就是零成本，那么一个会做 N 次无效调用的空实现就兑现不了这个价值。

**而 `has` 返回 `False`** —— 于是「缓存里有这条」永远为假，任何「先 `has` 再 `get`」的逻辑都会走未命中的路径。

**这四个返回值的组合保证了一件事：`NoopCache` 在语义上等价于「一个永远空的缓存」**，而不是「一个坏掉的缓存」。**差别在于前者不会让任何代码分支出错。**

### 四、`CacheKeyCreator`：一个协议而非函数

**它是一个 `@runtime_checkable` 的 `Protocol`。**

```
@runtime_checkable
class CacheKeyCreator(Protocol):
    def __call__(self, input_args: dict[str, Any]) -> str: ...
```

**它是 Protocol 而不是 `Callable[[dict], str]` 类型别名**，原因是：

1. **可被 `isinstance` 检查**（`runtime_checkable`）。
2. **有名字，能被文档引用**（`graphrag_llm` 与 `graphrag_cache` 两个包都用它）。
3. **可被第三方实现并注册**。

**而默认实现只有一行：**

```
def create_cache_key(input_args: dict[str, Any]) -> str:
    return hash_data(input_args)
```

**`hash_data` 来自 `graphrag_common.hasher`，它对 dict 做规范化后再哈希。** 规范化的关键是**键顺序无关** —— 若两个内容相同但键插入顺序不同的 dict 得到不同哈希，缓存命中率会莫名其妙地低。**这一层保证是 `hash_data` 的责任，而缓存的正确性依赖它。**

**注意这个函数是**同步**的，而 `Cache` 的全部方法都是异步的。** 这个不对称是合理的：哈希是纯计算，没有 I/O。

### 五、工厂与配置：一条「配置类型必须匹配后端要求」的校验

**`CacheType` 用 `StrEnum`，而 `Noop` 的值是 `"none"`。**

```
class CacheType(StrEnum):
    Json = "json"
    Memory = "memory"
    Noop = "none"
    Sqlite = "sqlite"
```

**`Noop = "none"` 这个不一致值得记** —— 枚举**成员名**是 `Noop`、**值**是 `"none"`。于是配置文件里写 `type: none`，而代码里引用 `CacheType.Noop`。**这是「用户词汇」与「实现词汇」的差别被保留在同一个枚举里** —— 优点是对用户友好（`none` 比 `noop` 直观），缺点是 `CacheType("noop")` 会抛 `ValueError`。

**`CacheConfig` 有一个跨字段校验器。**

```
@model_validator(mode="after")
def _validate_sqlite_storage(self) -> "CacheConfig":
    if self.type == CacheType.Sqlite and (self.storage is None or self.storage.type != StorageType.File):
        msg = "Cache type 'sqlite' requires storage type 'file'."
        raise ValueError(msg)
    return self
```

**它保证「SQLite 缓存必须配文件存储」在**配置加载时**就失败**，而不是等到 `SQLiteCache.__init__` 抛 `TypeError`。

**这两个校验位置的分工很清晰**：配置层校验「组合是否合法」（`sqlite` + `file`），构造层校验「参数是否合法」（`database_name` 是纯文件名）。**前者是用户的错误，后者是程序员的错误。**

**而 `CacheConfig` 用 `extra="allow"`。**

```
model_config = ConfigDict(extra="allow")
"""Allow extra fields to support custom cache implementations."""
```

**于是配置文件里多出的字段不会报错，而是被收进 `model_extra` 并传给构造。** 这是「让第三方实现能带自己的配置项」的必要放宽 —— **代价是拼错字段名不会报错**（`storag` 而不是 `storage` 会静默地用一个默认存储）。

**`storage` 的默认值直接是一个 `StorageConfig(type=File, base_dir="cache")`。**

```
storage: StorageConfig | None = Field(..., default_factory=lambda: StorageConfig(type=StorageType.File, base_dir="cache"))
```

**用 `default_factory` 而非可变默认值** —— 因为 `StorageConfig` 是 pydantic 模型（可变的），共享一个实例会让修改互相影响。

## 关键取舍

**源码事实：** `Cache` 契约是七个异步方法 + 一个同步 `child(name)`；`child` 同步因为只构造对象不做 I/O，返回类型放宽为 `Cache`（不要求同类型）。

**源码事实：** `get_many` 有默认实现（`dict.fromkeys` 去重保序 + `gather` 并发 + `zip(strict=True)` + 过滤 `None`），于是「去重、省略缺失、并发」三条契约由框架保证；`SQLiteCache` / `MemoryCache` / `NoopCache` 都覆盖它做原生批量。

**源码事实：** `CacheType.Noop = "none"` —— 成员名与值不一致，于是配置写 `none`、代码用 `CacheType.Noop`，而 `CacheType("noop")` 会抛。

**源码事实：** `NoopCache.child` 返回 `self`（无操作对任何命名空间相同），且覆盖 `get_many` 直接返回 `{}`——不是为了性能而是为了兑现「零成本」这个存在理由。

**源码事实：** `SQLiteCache` 构造期三重校验，其中 `database_name` 用 POSIX 与 Windows 两套路径语义各解析一遍，要求两边都是「单段、非绝对、名字即原值」，以防跨平台路径逃逸。

**源码事实：** SQLite 走 `journal_mode = WAL`（写不阻塞读）与 `synchronous = NORMAL`（**放弃提交即落盘**，对缓存可接受）；`busy_timeout = 30000` 与 Python 层 `timeout=30` 都设。

**源码事实：** 每次操作新建连接、无连接池——换来线程安全（连接不能跨线程共享）。

**源码事实：** 所有异步方法用 `asyncio.to_thread` 包装同步 SQLite 调用；对微秒级查询，线程调度可能比查询本身更贵。

**源码事实：** `get` 遇到「JSON 解析失败」或「不是 `{"result": ...}` 结构」时用 `_delete_if_unchanged`（带 `AND value_json = ?` 的条件删除）清理，防止删掉别的线程刚写入的好数据（一次带版本检查的 CAS）。

**源码事实：** `get_many` 的 `SQLITE_BATCH_SIZE = 500` 是因为要算上 `namespace` 那一个参数、保持在 SQLite 旧 999 参数上限之下；拼 SQL 的 f-string 只拼 `?` 占位符数量（`# noqa: S608` 是安全的）。

**源码事实：** `get_many` 在**同一个** `with self._connect()` 块内完成查询与无效条目删除（同一事务），而单条 `get` 是「查、`await`、再删」两次连接——**批量版比逐条版更严格**。

**源码事实：** `set` 拒绝 `None` 值（否则无法区分「缓存了 None」与「未命中」）；`debug_data` 用 `{"result": value, **(debug_data or {})}` 合并，`result` 在前，**若 `debug_data` 含 `result` 会覆盖真值（未加保护）**。

**源码事实：** `clear` 只清当前命名空间（精确匹配），所以清 `a` 不清 `a/b`——命名空间是平铺字符串不是树。

**源码事实：** `hash_data` 对 dict 做键顺序无关的规范化，这是「内容相同则哈希相同」的前提，而整个缓存的命中率依赖它。

**源码事实：** `CacheConfig` 用 `model_validator(mode="after")` 保证 `sqlite` 必配 `file` 存储——在配置加载时失败而不是等构造时抛 `TypeError`；同时 `extra="allow"` 让第三方配置项透传，但**拼错字段名不报错**。

**源码事实：** `CacheKeyCreator` 是 `@runtime_checkable` 的 `Protocol` 而非 `Callable` 别名，于是可 `isinstance`、可被两个包共同引用、可被第三方实现；而它是**同步**的（哈希是纯计算）。

**架构推论：** `NoopCache` 是这个设计里最有价值的一个实现。它把「关闭某功能」从**条件分支**改成了**多态** —— 于是所有调用点的代码只有一种形态，而「关了」这个决定只出现在装配处。**这与 e01 那页 `NoopMetricsStore` 是同一个手法，也是主核里「无操作统计收集器」的同构做法。** 判据是：一个功能的关闭不该让调用方多写一行 if。

**架构推论：** `_delete_if_unchanged` 这个细节揭示了一种常被忽略的并发问题：**「读坏数据然后删除」本身是一次写操作，它会与别的写操作竞争。** 大多数缓存实现直接删，因为它假设「坏数据一定是坏的」——但坏数据可能只是被读的时候正在被写（读到半个旧值）。**用条件删除把「删」变成「仅当它仍是我读到的那份」**，这是乐观并发控制的教科书应用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag-cache/graphrag_cache/cache.py` | `Cache` | 七异步方法 + 同步 `child`，`get_many` 默认实现 |
| `packages/graphrag-cache/graphrag_cache/cache_key.py` | `CacheKeyCreator` | `runtime_checkable` 协议 |
| `packages/graphrag-cache/graphrag_cache/cache_key.py` | `create_cache_key` | 委托 `hash_data`（键序无关） |
| `packages/graphrag-cache/graphrag_cache/sqlite_cache.py` | `SQLiteCache` | WAL + 条件删除 + 批量分片 |
| `packages/graphrag-cache/graphrag_cache/sqlite_cache.py` | `_delete_if_unchanged` | 带值比较的删除（CAS） |
| `packages/graphrag-cache/graphrag_cache/sqlite_cache.py` | `SQLITE_BATCH_SIZE` | 500，为 999 参数上限留出命名空间 |
| `packages/graphrag-cache/graphrag_cache/noop_cache.py` | `NoopCache` | 零成本空实现，`child` 返回 `self` |
| `packages/graphrag-cache/graphrag_cache/json_cache.py` | `JsonCache` | 每键一个文件，受 `Storage` 抽象 |
| `packages/graphrag-cache/graphrag_cache/memory_cache.py` | `MemoryCache` | 进程内字典，覆盖 `get_many` |
| `packages/graphrag-cache/graphrag_cache/cache_factory.py` | `create_cache` | 按 `CacheType` 懒注册 |
| `packages/graphrag-cache/graphrag_cache/cache_config.py` | `CacheConfig` | `sqlite` 必配 `file` 的跨字段校验 |
| `packages/graphrag-cache/graphrag_cache/cache_type.py` | `CacheType` | `Noop = "none"` |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 源码：graphrag_cache](https://github.com/microsoft/graphrag/tree/main/packages/graphrag-cache/graphrag_cache)
- [SQLite 文档：WAL 模式](https://www.sqlite.org/wal.html)
- [SQLite 文档：PRAGMA synchronous](https://www.sqlite.org/pragma.html#pragma_synchronous)

## 与主核 / AI 的关系

这个缓存层与主核的**缓冲池（buffer pool）** 在词汇上相似而机制完全不同，差别本身很有信息量。

**缓冲池是「热数据的副本」，它的正确性与数据库的一致性绑定** —— 一个脏页何时写回、写回失败怎么办，都是事务协议的一部分。**而这个缓存是「计算结果的副本」，丢了只是重算** —— 所以它可以用 `synchronous = NORMAL` 放弃提交落盘、可以在 `atexit` 里尽力刷盘、可以整片清空。**「能不能丢」这个判据决定了一整层的设计自由度。**

**「条件删除」在数据库里叫 `DELETE ... WHERE version = ?`，是乐观锁的标准写法。** 而它在缓存的清理路径上出现，说明作者意识到「清理坏数据」也是一次写。**大多数缓存实现没有这个意识** —— 它们假设坏数据只会是坏的，没考虑「读到坏数据是因为有人正在写」。

**`child` 的命名空间与主核的表空间/租户隔离是同构的**，但实现简单得多：这里是一个字符串前缀拼在主键里，而 SQLite 用 `PRIMARY KEY (namespace, key)` 让隔离免费得到（索引已经按键排序）。**数据库的隔离要靠 schema 与权限，这里只要一个复合主键** —— 因为这里的隔离需求只有「能独立清空」，没有「能独立授权」。

**而 `clear` 精确匹配命名空间、不清子命名空间这件事，与文件系统的 `rm -rf` 语义相反** —— 它更像 Redis 的 `DEL prefix:*` 需要显式写通配。**「层级用字符串表达」必然导致「层级语义要手工实现」**，这是所有 `a/b/c` 式命名空间的共同代价（包括对象存储的 key 前缀）。

对 agent 工程的直接价值有三条。其一，**用空实现而不是 `None` 来表示「关闭」**。`NoopCache` 让下游代码只有一种形态，而「关了」这个决定集中在装配处——同时也让测试可以传一个真对象而不是 mock。其二，**清理坏数据要带版本检查**。「读到坏的 → 删掉」是一次写操作，会与别的写竞争；用 `WHERE value = 我读到的那份` 把它变成 CAS，才不会有「删掉别人刚写的好数据」的窗口。其三，**批量接口要在同一事务里完成判定与清理**。`get_many` 把查询与删除放进同一个 `with connection`，于是完全没有中间被改写的窗口——**而逐条版有这个窗口**，两版的严格性不同是应该被知道的。
