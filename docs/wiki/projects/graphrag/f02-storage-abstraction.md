---
project: graphrag
feature: storage-abstraction
title: 存储抽象与表提供方
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag-storage/graphrag_storage/storage.py
    symbols: [Storage]
  - path: packages/graphrag-storage/graphrag_storage/storage.py
    symbols: [get_timestamp_formatted_with_local_tz]
---

# 存储抽象与表提供方

## 一句话职责

用一个**十一个方法的接口**（全部异步 + 一个同步 `child` + 一个同步 `find`）把「键值文件存取」抽象成与物理后端无关的操作，并额外提供一个时间戳格式化函数——因为这份存储的用途是**在目录里存索引产物**，而不是存通用数据。

## 机制是什么

`Storage` 的成员可以按「谁在用」分成三组：

| 组 | 成员 | 用途 |
|---|---|---|
| **键值操作** | `get` / `set` / `has` / `delete` / `clear` | 读写一个键（通常是一个文件） |
| **列举与发现** | `keys` / `find` | 列出全部键 / 按正则找 |
| **层级与元数据** | `child` / `get_creation_date` | 子目录视图 / 文件创建时间 |

**三个设计决定值得先记：**

1. **`child(name)` 返回同类型实例并下钻到子目录** —— 这是 graphrag 那套「输出、更新、delta、previous 四份索引目录」的实现基础（见下面 `get_update_table_providers`）。
2. **`find` 是同步的、且接受编译好的正则** —— 而其余方法全是异步。**它是唯一「可能长时间运行」的方法**，异步在这里反而没用（`rglob` 是同步的）。
3. **`get_creation_date` 返回字符串而不是 `datetime`** —— 因为它的值要进 DataFrame 与 parquet，而字符串不会遇到时区/序列化问题。

## 怎么实现的

### 一、`Storage` 契约：十一个方法

**七个异步方法：**

```
async def get(self, key: str, as_bytes: bool | None = None, encoding: str | None = None) -> Any
async def set(self, key: str, value: Any, encoding: str | None = None) -> None
async def has(self, key: str) -> bool
async def delete(self, key: str) -> None
async def clear(self) -> None
async def get_creation_date(self, key: str) -> str
```

**两个同步方法：**

```
def find(self, file_pattern: re.Pattern[str]) -> Iterator[str]
def child(self, name: str | None) -> "Storage"
def keys(self) -> list[str]
```

**`get` 的两个可选参数暴露了这份存储的真实用途。**

```
async def get(self, key, as_bytes: bool | None = None, encoding: str | None = None) -> Any
```

- **`as_bytes`**：parquet 文件是二进制，文本文件是 UTF-8——**同一个接口要同时支持**。
- **`encoding`**：用户的 CSV 可能是 GBK。**让调用方逐次指定**，而不是靠一个全局配置。

**这两个参数的存在说明它不是一个通用的 KV 存储**（通用 KV 存的是字节），而是**「一个带编码概念的目录」**。

**`child(name: str | None)` 的参数可为 `None`。**

**`None` 的语义是「返回自己」** —— 这与 `FileStorage.child` 的实现一致：

```
def child(self, name: str | None) -> "Storage":
    if name is None:
        return self
    child_path = str(self._base_dir / name)
    return FileStorage(base_dir=child_path, encoding=self._encoding)
```

**为什么允许 `None`？** 因为它让「可选的一层子目录」不必在调用点分支。**这是 `NoopCache.child` 返回 `self` 的同一种思路**（见 f01 那页）：用「无操作」代替条件判断。

**`get_creation_date` 的返回值是格式化好的本地时间字符串。**

```
def get_timestamp_formatted_with_local_tz(timestamp: datetime) -> str:
    """Get the formatted timestamp with the local time zone."""
    creation_time_local = timestamp.astimezone()
    return creation_time_local.strftime("%Y-%m-%d %H:%M:%S %z")
```

**三个决定：**

1. **`.astimezone()` 不带参数 = 转成本地时区** —— 于是输出里带的是运行机器的时区，而不是 UTC。**这解释了这个函数为什么叫 `..._with_local_tz`。**
2. **格式是 `%Y-%m-%d %H:%M:%S %z`** —— `%z` 产生 `+0800` 这样的偏移，**是可排序的**（同符号内）。
3. **它是免费函数而不是 `Storage` 的方法** —— 因为它与后端无关（所有后端拿到的都是 `datetime`）。

**为什么需要这个函数：** `FileStorage` 的后端实现是

```
creation_timestamp = file_path.stat().st_ctime
creation_time_utc = datetime.fromtimestamp(creation_timestamp, tz=timezone.utc)
return get_timestamp_formatted_with_local_tz(creation_time_utc)
```

**注意它用了 `st_ctime`** —— 在 POSIX 上那是 **inode 变更时间**（不是创建时间；创建时间要靠 `st_birthtime`，且不是所有文件系统都有）；在 Windows 上 `st_ctime` 才是真正的创建时间。**所以这个「creation_date」在 Linux 上实际是「元数据最后一次变化的时间」，而 `set` 覆盖写会更新它。**

**这是一个跨平台语义不一致的真实问题** —— 而调用方（`CSVFileReader` 给每个 `TextDocument` 填 `creation_date`，见 f04 那页）会把它当成「文档创建时间」用。**在 Linux 上，重新处理一份文件会让它的「创建时间」变成现在。**

**而 `%z` 的本地时区也让这个值不可跨机器比较** —— 同一批数据在两台不同时区的机器上处理，会得到带不同偏移的时间字符串。

### 二、`FileStorage`：路径处理是最讲究的一处

**文件存储的全部复杂度集中在「键与路径的转换」上。**

```
def _join_path(file_path: Path, file_name: str) -> Path:
    """Join a path and a file. Independent of the OS."""
    return (file_path / Path(file_name).parent / Path(file_name).name).resolve()
```

**这个三段的写法看起来绕，而它解决的是「键里带路径分隔符」的问题：**

**若直接写 `file_path / file_name`**，在 POSIX 上 `key = "a/b"` 会得到 `base/a/b`，符合预期；但在 Windows 上 `Path("a/b")` 也能工作（`/` 被接受），于是也行。**那为什么不用直接拼接？**

**因为 `Path(file_name).parent / Path(file_name).name` 做了一次「规范化往返」**：它把键解析成（父目录, 文件名）再拼回去。这在多数情况下等价，但**对 `file_name` 是绝对路径时的行为不同** —— `Path("/etc/passwd").parent / .name` 得到 `/etc/passwd`，于是 `base / /etc/passwd` 在 pathlib 里会丢弃 `base`（绝对路径覆盖前缀）。**所以这个写法并不能防绝对路径逃逸**，它只是显式地表达意图。

**而最后的 `.resolve()` 是关键的一步** —— 它把 `..` 折掉、把符号链接解开。**于是 `key = "../../etc/passwd"` 会被解析成 `base/../../etc/passwd` 的真实路径，仍然逃出了 base。**

**结论：`_join_path` 没有路径穿越防护。** 而它的 docstring 写的是「Independent of the OS」（与操作系统无关），指的是**路径语义**而非安全。**这与 f01 那页 `SQLiteCache` 对 `database_name` 的严格校验形成鲜明对照** —— 差别在于：那里的名字来自用户配置，这里的键来自 graphrag 自己的代码（文件名常量 + 内容哈希）。

**`find` 的实现有三个细节。**

```
all_files = list(self._base_dir.rglob("**/*"))
for file in all_files:
    match = file_pattern.search(f"{file}")
    if match:
        filename = f"{file}".replace(str(self._base_dir), "", 1)
        if filename.startswith(os.sep):
            filename = filename[1:]
        yield filename
```

1. **`rglob("**/*")` 递归列出全部**，包括目录（后续没有过滤 `is_file()`）。
2. **匹配的是**绝对路径字符串**而不是相对键** —— 于是模式里可以写 `/data/` 这样的绝对片段。这是一个可疑的设计（模式的行为依赖 base_dir 的位置），但它让「按目录名过滤」成为可能。
3. **`replace(str(self._base_dir), "", 1)` 去掉前缀** —— `count=1` 只替换第一次出现。**若 base_dir 是 `/data/out` 而文件是 `/data/out/out/x.csv`，`replace` 得到 `/out/x.csv`，然后去掉前导分隔符得 `out/x.csv`，正确。** 但如果 base_dir 是 `/data` 而文件名恰好包含 `/data` 子串，`replace` 会切错——**不过因为它替换的是**第一个**出现且前缀确实在开头，所以实际安全。**

**注意 `find` 会把目录也返回** —— 若模式匹配到一个目录名，那个「键」是个目录。下游 `get` 它时会失败（`aiofiles.open` 一个目录抛 `IsADirectoryError`）。**这是一个未处理的边界**，而 typings 里 `Iterator[str]` 无法表达这个区别。

**`clear` 会真的删掉子目录。**

```
for file in self._base_dir.glob("*"):
    if file.is_dir():
        shutil.rmtree(file)
    else:
        file.unlink()
```

**递归删除（`rmtree`）而不是只删文件** —— 因为 `child` 会建子目录（`timestamped_provider.child(timestamp)` 之类），只删顶层文件会留下一堆空目录。

**`keys()` 只返回顶层文件。**

```
def keys(self) -> list[str]:
    return [item.name for item in self._base_dir.iterdir() if item.is_file()]
```

**三个约束：不递归（只当前层）、只文件（不含目录）、返回文件名而不是完整键（不含子目录前缀）。**

**于是 `keys()` 与 `find()` 的语义不一致**：`find` 递归且返回相对路径，`keys` 不递归且返回纯文件名。**「`keys()` 就是全部键」这个直觉是错的** —— 它只是「当前目录下的文件名」。

**`set` / `get` 用 `aiofiles` 做异步文件 I/O。**

```
is_bytes = isinstance(value, bytes)
write_type = "wb" if is_bytes else "w"
encoding = None if is_bytes else encoding or self._encoding
```

**二进制模式必须传 `encoding=None`**（否则 `open` 会抛错），而文本模式必须传编码。**这三行把「按值的类型自动选模式」封装起来了**，于是调用方不必关心。

**`get` 先 `has` 再读** —— 两次系统调用（`exists` + `open`）。**这是一个可优化点**（直接 `open` 并捕获 `FileNotFoundError` 更省），但前者的语义更清楚：不存在时返回 `None` 而不是抛异常。

**而 `child` 每次构造都 `mkdir(parents=True, exist_ok=True)`。**

```
self._base_dir.mkdir(parents=True, exist_ok=True)
```

**于是「得到一个子存储」这个动作有副作用：创建目录。** 好处是 `set` 不必担心目录不存在；坏处是**仅仅构造一个存储实例就会在磁盘上留下痕迹**——而 `child` 会递归调用构造，于是遍历 `timestamp/delta` 与 `timestamp/previous` 会真的创建两个目录。

### 三、表提供方：与 `Storage` 并列的另一套抽象

**`graphrag_storage.tables` 里有另一套接口，而它与 `Storage` 的分工是「行 vs 文件」。**

| | `Storage` | `TableProvider` |
|---|---|---|
| 单位 | 一个键（一个文件/一个 blob） | 一张表（一个 DataFrame/parquet/csv 文件） |
| 操作 | `get` / `set` / `has` / `delete` | 读表 / 写表 / `has` / `child` |
| 格式意识 | 只知道文本或字节 | 知道 parquet / CSV / Cosmos 容器 |
| 后端 | file / memory / blob / cosmos | file / cosmos |

**而 `get_update_table_providers` 展示了 `child` 的层级用法（这是 `Storage.child` 与 `TableProvider.child` 都要支持 `name` 的原因）：**

```
output_table_provider = create_table_provider(config.table_provider, output_storage)
update_storage = create_storage(config.update_output_storage)
update_base_provider = create_table_provider(config.table_provider, update_storage)
timestamped_provider = update_base_provider.child(timestamp)
delta_table_provider = timestamped_provider.child("delta")
previous_table_provider = timestamped_provider.child("previous")
```

**两次 `child` 形成 `update_output/<timestamp>/{delta,previous}` 的目录结构。**

**而注释说明了为什么必须走 `child()` 而不是手工拼路径：**

> 通过 `table_provider.child()` 构建更新提供方，**以便 Cosmos 提供方使用命名空间隔离，而 file/blob 提供方委托给 `Storage.child()` 的文件系统路径。**

**这是 `child` 这个抽象的真实收益**：同一个「下一层」操作，在文件后端是「建子目录」，在 Cosmos 后端是「换容器/分区」。**若调用方手工拼路径，Cosmos 后端就无法工作。**

### 四、四个后端与工厂

**`StorageType` 有四个成员，值都是小写短名。**

```
class StorageType(StrEnum):
    File = "file"
    Memory = "memory"
    AzureBlob = "blob"
    AzureCosmos = "cosmosdb"
```

**工厂的 `match` 与另外几个包同构（懒注册 + 兜底列出已注册类型）：**

```
case StorageType.File: register_storage(StorageType.File, FileStorage)
case StorageType.Memory: register_storage(StorageType.Memory, MemoryStorage)
case StorageType.AzureBlob: register_storage(StorageType.AzureBlob, AzureBlobStorage)
case StorageType.AzureCosmos: register_storage(StorageType.AzureCosmos, AzureCosmosStorage)
case _: raise ValueError(f"StorageConfig.type '{storage_strategy}' is not registered ...")
```

**四个后端分成两类：**

| 后端 | 需要什么 | 适合什么 |
|---|---|---|
| `FileStorage` | 本地目录 | 单机、可检查、默认 |
| `MemoryStorage` | 进程内存 | 测试、短跑 |
| `AzureBlobStorage` | 容器名 + 连接 | 云端、无共享文件系统 |
| `AzureCosmosStorage` | 端点 + 数据库 | 云端、需要按行访问 |

**而 `AzureBlobStorage` 与 `AzureCosmosStorage` 的存在解释了为什么 `Storage` 接口全是异步** —— 它们要做网络 I/O。**若只有文件与内存两个后端，同步接口就够了。**

**这也是 `find` 是同步的那个设计的代价**：一个异步的存储抽象里留了一个同步方法，于是 `AzureBlobStorage` 实现它时必须做某种阻塞或预取。**接口的不一致来自「`rglob` 天然是同步的」这个事实。**

## 关键取舍

**源码事实：** `Storage` 有七个异步方法（`get`/`set`/`has`/`delete`/`clear`/`get_creation_date`）+ 三个同步方法（`find`/`child`/`keys`）；`find` 同步且接编译好的正则，是唯一可能长时间运行的方法。

**源码事实：** `get` 的 `as_bytes` 与 `encoding` 两个可选参数说明这不是通用 KV 存储，而是「一个带编码概念的目录」。

**源码事实：** `child(name: str | None)` 允许 `None` 且语义为「返回自己」，于是「可选的一层子目录」不必在调用点分支。

**源码事实：** `get_timestamp_formatted_with_local_tz` 用无参 `.astimezone()`（转本地时区）与 `%Y-%m-%d %H:%M:%S %z` 格式；它是免费函数而非方法，因为与后端无关。

**源码事实：** `FileStorage.get_creation_date` 用 `st_ctime`——在 POSIX 上是 inode 变更时间（不是创建时间），在 Windows 上才是创建时间。于是 Linux 上「重新处理一份文件」会更新它的「创建时间」；且输出带本地时区偏移，不可跨机器比较。

**源码事实：** `_join_path` 做了一次「父目录 + 文件名」的规范化往返并 `.resolve()`，**没有路径穿越防护**（`../` 会被解析到 base 之外）；docstring 说的「Independent of the OS」指路径语义而非安全。

**源码事实：** `find` 用 `rglob("**/*")` 递归列出（**含目录**），在**绝对路径字符串**上做 `search`（于是模式可含绝对片段），再用 `replace(base_dir, "", 1)` 去前缀。目录会作为「键」被返回，下游 `get` 会抛 `IsADirectoryError`。

**源码事实：** `keys()` 不递归、只返回当前层**文件名**（不含子目录前缀），与 `find` 的递归+相对路径语义不一致——「`keys()` 就是全部键」是错的。

**源码事实：** `clear()` 对子目录用 `shutil.rmtree` 递归删除，因为 `child` 会建子目录。

**源码事实：** `set` 按 `isinstance(value, bytes)` 自动选 `wb`/`w`，且二进制模式必须 `encoding=None`。

**源码事实：** `get` 先 `has` 再读（两次系统调用），语义上让「不存在」返回 `None` 而非抛异常。

**源码事实：** `FileStorage.__init__` 与 `child` 都执行 `mkdir(parents=True, exist_ok=True)`——**构造一个存储实例就有磁盘副作用**，于是遍历 `timestamp/{delta,previous}` 会真的创建目录。

**源码事实：** 更新索引的目录结构通过 `table_provider.child(timestamp).child("delta"/"previous")` 建立，注释明确走 `child()` 而不是拼路径是为了让 Cosmos 提供方用命名空间隔离、文件/Blob 提供方委托给 `Storage.child()`。

**源码事实：** `StorageType` 四个后端（`file`/`memory`/`blob`/`cosmosdb`），其中 Blob 与 Cosmos 是网络后端——**这正是接口全异步的原因**。

**架构推论：** 「异步接口里留一个同步 `find`」这个不一致的根源是 `rglob` 天然同步。**接口的整齐度被宿主语言的标准库能力限制** —— 一个真正的异步接口要求每个后端都把列举实现成异步（云后端通常有分页 API），而文件后端就只能用 `to_thread` 包装。**当前设计选择了「让文件后端简单」而不是「让接口一致」。**

**架构推论：** `child` 用「返回同类新实例」而不是「返回一个句柄」来实现层级，是把**路径拼接**变成**类型操作**。好处是每层都是完整的 `Storage`（能 `get`/`set`/`clear`）；代价是**构造有副作用**（建目录）、且**层数无限制**（可以 `child("a").child("b")...` 直到路径过长）。**数据库的表空间不是这样工作的**——它是有限层级、需要在系统目录里注册。而这里「子存储」是无限且自动的，因为它只是路径前缀。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag-storage/graphrag_storage/storage.py` | `Storage` | 十一方法契约（七异步 + 三同步） |
| `packages/graphrag-storage/graphrag_storage/storage.py` | `get_timestamp_formatted_with_local_tz` | 本地时区格式化 |
| `packages/graphrag-storage/graphrag_storage/file_storage.py` | `FileStorage` | 目录后端，`child` 建子目录 |
| `packages/graphrag-storage/graphrag_storage/file_storage.py` | `_join_path` | 键→路径（无穿越防护） |
| `packages/graphrag-storage/graphrag_storage/file_storage.py` | `find` | `rglob` + 绝对路径正则 + 去前缀 |
| `packages/graphrag-storage/graphrag_storage/memory_storage.py` | `MemoryStorage` | 进程内字典 |
| `packages/graphrag-storage/graphrag_storage/azure_blob_storage.py` | `AzureBlobStorage` | 云 Blob 后端 |
| `packages/graphrag-storage/graphrag_storage/azure_cosmos_storage.py` | `AzureCosmosStorage` | 云 Cosmos 后端 |
| `packages/graphrag-storage/graphrag_storage/storage_factory.py` | `create_storage` | 按 `StorageType` 懒注册 |
| `packages/graphrag-storage/graphrag_storage/tables/table_provider.py` | `TableProvider` | 行级/表级抽象与 `child` |
| `packages/graphrag/graphrag/index/run/utils.py` | `get_update_table_providers` | 两次 `child` 建 `timestamp/{delta,previous}` |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 源码：graphrag_storage](https://github.com/microsoft/graphrag/tree/main/packages/graphrag-storage/graphrag_storage)
- [Python 文档：pathlib](https://docs.python.org/3/library/pathlib.html)
- [aiofiles 文档](https://github.com/Tinche/aiofiles)

## 与主核 / AI 的关系

这个存储抽象与主核的**存储管理器（smgr）** 在抽象层级上相差很远，而差别说明了「用途决定抽象」。

**主核的存储管理器管的是「页」** —— 固定大小的块、buffer pool 里的槽位、文件与块号的映射。它极其讲究（校验和、fsync 顺序、并发 latch），因为**它承载的是数据库的唯一真相**。

**而这里的 `Storage` 管的是「文件」** —— 一个键一份数据、内容是不可变的索引产物。于是它不需要事务、不需要校验和、不需要并发控制（写入是批处理的最后一步）。**它需要的是「能换后端」**（本地文件 / 云 Blob / Cosmos），因为 graphrag 既要在笔记本上跑也要在云上跑。

**「同一个接口服务四个后端」这个要求解释了接口为什么是异步的**，也解释了 `child` 为什么是抽象的：**文件后端的子目录、Cosmos 后端的命名空间，是同一个概念的两种实现。** 这与数据库的「表空间」是对应的——但数据库的表空间需要在系统目录里注册、需要管理员操作，而这里的子存储是「随便拼个前缀」。

**`get_creation_date` 的 `st_ctime` 问题在数据库里不会出现**，因为数据库自己维护元组的时间戳（`xmin` 的提交时间）。**当一个系统依赖文件系统的元数据来表达语义时，「文件系统不保证这个字段的语义」就变成一个隐患。** 这是「用一个通用设施表达特定语义」的典型代价。

对 agent 工程的直接价值有三条。其一，**接口的同步/异步一致性会被宿主语言的标准库限制**。若 `rglob` 只有同步版本，硬要异步就只能 `to_thread` 包装或让文件后端变成异类——两条路都不干净。其二，**`child` 这种「返回同类新实例」的分层方式，收益在第二类后端上兑现**。文件后端的 `child` 只是建目录（没有它也能拼路径），而 Cosmos 后端的 `child` 是换命名空间（没有它根本做不到）。**抽象的价值要等第二个后端出现才被证明。** 其三，**别依赖文件系统的「创建时间」**。POSIX 的 `st_ctime` 是 inode 变更时间，任何覆盖写都会更新它；要真的记录创建时间，必须自己写进元数据。
