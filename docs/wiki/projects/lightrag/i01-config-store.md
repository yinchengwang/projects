---
project: lightrag
feature: config-store
title: 配置存储与作用域
page_type: feature
schema_version: 1
refs:
  - path: lightrag/config_store.py
    symbols: [ConfigScope, ConfigKeySpec, config_key]
  - path: lightrag/config_store.py
    symbols: [EmbeddingBaseline, embedding_baseline_key, make_config_row]
---

# 配置存储与作用域

## 一句话职责

给「服务端设置与每个工作区的设置」找一个**不属于任何知识库工作区**的家：一个由代码写死名字的容器，键用 `<workspace>/<suffix>` 编码作用域（元数据用 `$meta/`、空工作区用 `$default/`），每个后缀必须先在注册表里登记才能被写——而容器自身还有一个 UUID 身份，用来防止两个部署误绑同一份配置。

## 机制是什么

模块 docstring 用七条把这一层要守的规则按「调用方遇到的顺序」列了出来。这个顺序本身是关键，因为它同时是阅读顺序与依赖顺序：

1. **容器**是代码里写死的 KV 命名空间 `config`，在 PostgreSQL / MongoDB / OpenSearch 里以 `CONFIG_CONTAINER_TAG` 命名表/集合/索引；JSON 后端在每个工作区一个固定路径的快照，而同一个 `WORKING_DIR` 下的快照组成一个组、共享一个身份。**没有任何 `*_WORKSPACE` 变量能碰到它，没有任何设置能挪动它。**
2. **后端是它自己的类别**：`CONFIG_STORAGE` 只接受 `JsonKVStorage` / `MongoKVStorage` / `PGKVStorage` / `OpenSearchKVStorage`；其他任何东西——向量存储、Redis——**在构造期按名字拒绝**。未设时跟随 `kv_storage`，好让既有部署落在它数据已经在的地方，**除了 Redis 业务 KV 默认用 JSON 配置**。
3. **键**用 `<workspace>/<suffix>`、空工作区用 `$default/<suffix>`、元数据用 `$meta/<suffix>`。业务名不能以 `$` 开头。**键由 `config_key` 构造且从不被重新解析**。
4. **每个键在写之前都要在 `CONFIG_KEY_REGISTRY` 里登记**。「一个未登记的后缀是编程错误，不是运行时状况」。
5. **读是严格的**：一个决定实例能否服务的记录用 `get_by_id_strict` 读；一次完不成的读抛 `ConfigurationStorageError`，**永远不被误认为缺失**。
6. **缺失的基线在键控锁下原子认领**，flush 与严格回读都在锁内。「这个认领覆盖一个 Gunicorn master 的 worker，不会更宽」。
7. **容器有身份**：`$meta/storage_identity` 持一个 UUID，而锚点文件记录这个部署绑定到哪个后端与 UUID。

所以这一层的核心问题是 a01 那页提到过的一句：**配置是元数据，跟着被配置的数据走会导致「配置本身丢了就再也读不到配置」的死循环**。解法是让容器名字在代码里、路径由 `working_dir` 决定而不由任何可配置项决定。

## 怎么实现的

**`ConfigScope` 只有两个值，而它的作用是让「这条键属于谁」在类型层面可判。**

```python
class ConfigScope(str, Enum):
    WORKSPACE = "workspace"
    SERVER = "server"
```

`config_key` 里两条对称的守卫把作用域错配变成构造期错误：

```python
if spec.scope is ConfigScope.SERVER and not is_server:
    raise ValueError(f"{suffix!r} is a server-global key and must be filed under SERVER_SCOPE, not {scope_workspace!r}")
if spec.scope is ConfigScope.WORKSPACE and is_server:
    raise ValueError(f"{suffix!r} is a per-workspace key; SERVER_SCOPE is not a workspace")
```

两条写的是同一个事实的两面。第一条容易理解（全局键不能挂在工作区下）；第二条更微妙：「`SERVER_SCOPE` 不是一个工作区」——所以一个按工作区登记的键不能被当作全局键。若允许这样，那 `embedding/entities` 这条基线就会变成全局的，于是两个工作区共用一条基线。

**`$` 前缀是保留的，而这条保留由两个独立机制保证。**

```python
if scope_workspace is SERVER_SCOPE: return META_CONFIG_PREFIX     # "$meta"
validate_workspace(scope_workspace)
return scope_workspace or DEFAULT_CONFIG_PREFIX                    # "$default"
```

而 docstring 说「业务工作区名不能以 `$` 开头」。这条约束**不在这个函数里**执行，而是靠 `validate_workspace` —— 所以「`$` 保留」是一个跨模块的不变量。若 `validate_workspace` 允许 `$` 开头，那么一个叫 `$meta` 的工作区就会与元数据前缀撞车。

而 `KEY_SEPARATOR` 那段注释把这条依赖说得很清楚：

> 键在它的作用域与后缀之间只能带这一个分隔符。**承重**：`validate_workspace` 禁止工作区名里有 `/`，所以一个键总能与一个名字区分开——**而它仍然从不被重新解析**。

最后半句是一个重要的设计声明：「能区分」不等于「要去解析」。键是单向构造的，读的时候靠**注册表**分类而不是靠切分字符串。这与「从不重新解析」一起保证了：改键格式不需要写解析器。

**`config_key` 的三重检查顺序也值得看：先查注册表，再查作用域，最后查前缀。**

```python
spec = registry_spec(suffix)          # KeyError 若未登记
...两条作用域守卫...
prefix = scope_prefix(scope_workspace)
if KEY_SEPARATOR in prefix: raise ValueError(...)
return f"{prefix}{KEY_SEPARATOR}{suffix}"
```

`registry_spec` 的错误消息是可操作的：「在 `CONFIG_KEY_REGISTRY` 里声明它（scope、schema、readers/writers、sensitive）**再写它**」。所以「必须登记」这条纪律的成本被降到「复制一个已有的 spec 改四个字段」。

而最后那道 `if KEY_SEPARATOR in prefix` 是双保险——`validate_workspace` 应该已经挡住了，但这里又查一次。理由与 g01 里 `zip(..., strict=True)` 那句一样：**把不变量写成断言**，因为 `config_key` 是公开函数，可能被绕过 `validate_workspace` 的路径间接调用。

**`ConfigKeySpec` 的七个字段正好是「一个键需要被声明的全部事实」。**

```python
@dataclass(frozen=True)
class ConfigKeySpec:
    suffix: str
    scope: ConfigScope
    schema_version: int
    schema: str
    readers: tuple[str, ...]
    writers: tuple[str, ...]
    sensitive: bool
```

docstring 一句：「一个注册表条目：契约要求一个键具备的五个字段」（实际七个——`suffix` 与 `scope` 大概不算「契约要求的」）。而它**冻结**（`frozen=True`）——因为注册表是模块级常量，任何运行时改动都是 bug。

`schema` 是一个**人类可读的字符串**而不是形式化的 schema：

```python
_EMBEDDING_BASELINE_SCHEMA = ("{model: str (unfolded, as configured), dim: int | null, origin: one of probe | empty | rebuild}")
```

所以它服务于**代码审阅者**而不是校验器。这个选择很实在：配置值很小、字段很少，形式化校验的成本大于收益，而一行字符串就能让审阅者看出形状。

而 `readers` / `writers` 两个元组的值是**模块路径字符串**：

```python
UPDATED_BY_STARTUP = "lightrag.initialize_storages"
UPDATED_BY_REBUILD = "lightrag-rebuild-vdb"
DELETED_BY_CLEAR_ENDPOINT = "/documents/clear"
DELETED_BY_CLEAR_TOOL = "lightrag-clear-storage"
UPDATED_BY_MIGRATE = "lightrag-migrate-config"
```

注释说明了为什么删除方也要列：「两个只会**删除**这些行的调用方，在注册表的 `writers` 里点名，**好让声明的列表保持是完整列表**。两者都不打 `updated_by`：一次删除不留下行来承载它」。

所以 `writers` 的语义是「所有改这个键的代码路径」，删除也算。而「删除不留行」这个观察解释了一个细节：诊断字段只在写入时存在。

**`make_config_row` 的字段集有一个按作用域变化的成员。**

```python
return {
    "schema_version": spec.schema_version,
    **({} if scope_workspace is SERVER_SCOPE else {"workspace": scope_workspace}),
    "updated_at": stamp,
    "updated_by": updated_by,
    "value": dict(value),
}
```

docstring：「**只有业务行携带 `workspace`**。元数据由它注册的键识别，不是一个虚构的工作区」。

所以「一个元数据行属于哪个工作区」这个问题的答案是「不属于」——而不是「属于一个叫 `$meta` 的假工作区」。差别在查询上：若元数据行带 `workspace="$meta"`，那任何「列出某工作区的所有配置行」的查询都得特殊处理这个值；不带字段则「没有 `workspace` 字段的行就是元数据」，一个条件就够了。

而它先调 `config_key(...)` 再取 `registry_spec(...)`：

```python
config_key(scope_workspace, suffix)     # 返回值被丢弃，只为触发校验
spec = registry_spec(suffix)
```

第一行只为**副作用**：它执行了那两条作用域守卫。写成一个显式的校验调用会更清楚，但这里用「调用会抛的函数」当校验，是一个常见但需要注释的写法（此处无注释，读者需要回看 `config_key` 才知道为什么要调它）。

而 `updated_at` 用 `datetime.now(timezone.utc).isoformat()` 且可注入 `now` 参数——注入是为了测试确定性。

**`EmbeddingBaseline` 是这一层第一个真正的内容类型，而模块 docstring 对它的语义有一段精确的限定。**

> 头几个按工作区的键是三个按目标的嵌入基线。**一条基线意味着什么**——那个目标被采纳的空间，**不是它的向量被写进的空间**——以及为什么有三个，在契约文档里；`origin` 字段记录每次认领是如何建立的，**且从不进入裁决**。

这三小句各纠正一种误解：

1. **基线 = 「采纳的空间」而不是「写入的空间」**。所以一条基线可以在没有任何向量的情况下存在（`origin=empty`），也可以在有向量但未验证的情况下不存在。
2. **三个而不是一个**，因为三个容器有各自独立的历史（g04 那页讲过：`lightrag-rebuild-vdb` 分别重建它们，所以中断的重建会让三者处于不同的空间）。
3. **`origin` 只是诊断**，从不参与判定。这是一个重要的分层：判定用 `model` 与 `dim`，而 `origin` 记录「这个结论是怎么来的」（probe / empty / rebuild），供人排查。

`BaselineOrigin` 的三个值正好对应三种建立方式：

```python
PROBE = "probe"     # 抽样重嵌入验证过（g04 的探针）
EMPTY = "empty"     # 源与容器都为空
REBUILD = "rebuild" # 显式重建后
```

而 `ALL = frozenset({PROBE, EMPTY, REBUILD})` 供校验用——一个「只允许这三个值」的集合。

**`embedding_baseline_suffix` 有一个前置校验，而它的错误消息列出全部合法值。**

```python
if target not in EMBEDDING_TARGETS:
    raise ValueError(f"{target!r} is not a vector target; expected one of {EMBEDDING_TARGETS}")
return f"embedding{KEY_SEPARATOR}{target}"
```

所以 `embedding/entities` 这类后缀也是**动态构造**的，但受 `EMBEDDING_TARGETS` 这个白名单约束。而 `EMBEDDING_TARGETS` 的顺序有语义：

```python
EMBEDDING_TARGETS: tuple[str, ...] = (
    NameSpace.VECTOR_STORE_ENTITIES, NameSpace.VECTOR_STORE_RELATIONSHIPS, NameSpace.VECTOR_STORE_CHUNKS,
)
```

注释：「三个向量目标，**按重建工具重建它们的顺序**」。所以这个元组的顺序与 `lightrag-rebuild-vdb` 的执行顺序绑定——若两者不一致，重建期间的中间状态就会与基线的记录顺序不符。

**「读是严格的」这条纪律不是风格而是语义：它区分「没有这条记录」与「读不出来」。**

docstring 写：「一个决定实例能否服务的记录用 `get_by_id_strict` 读；一次完不成的读抛 `ConfigurationStorageError`，**永远不被误认为缺失**」。

这与 g04 那页「`is_empty` 在某些后端会吞错」是同一类问题的反面。而这里的判据是「这条记录是否决定实例能否服务」——所以并非所有读都严格，只有**决策输入**才严格。这与 a01 里「控制面 vs 展示面的完整性要求相反」是同一条分层。

**「缺失的基线在键控锁下原子认领」这条有个明确的边界声明。**

docstring：「这个认领覆盖**一个 Gunicorn master 的 worker，不会更宽**」。

所以「原子」在单 master 范围内成立，跨 master 不成立。这是一个诚实的能力限定——而它的必要性在于：多 master 部署下两个进程可能同时发现基线缺失并各自建立。若两个建立的结论不同（例如一个 probe 成功、一个 probe 超时得 `empty`），后写的会覆盖先写的。

而「flush 与严格回读都在锁内」是这条保证的两个必要部分：只有 flush 在锁内的话，读到的是别人还没落盘的状态；只有回读在锁内的话，中间别人可能改动。

**身份绑定是这一层最复杂的部分，而它的存在理由是一句非常现实的话。**

docstring：「`bind_configuration_identity` 在任何基线被读之前检查——或（在没有锚点时）建立——那个绑定；**这一行一旦有效就永不被覆盖，永不被工作区维护删除**」。

而 `IDENTITY_BACKEND_MISMATCH`、`IDENTITY_UUID_MISMATCH`、`IDENTITY_UUID_MISSING`、`IDENTITY_MEMBER_MISSING`、`IDENTITY_MEMBER_UNREGISTERED`、`IDENTITY_ANCHOR_APPEARED`、`IDENTITY_SHARD_INVALID`、`IDENTITY_ROW_INVALID`、`IDENTITY_WRITE_FAILED`、`IDENTITY_ANCHOR_WRITE_FAILED` 这一长串错误码说明了它要处理的失败模式有多细。

这些错误码每一个对应一种「两个部署的关系不对」的情形。它们的价值在于**诊断可区分**：运维看到 `IDENTITY_UUID_MISMATCH` 就知道是「锚点与容器记录的 UUID 不一致」（可能是把锚点文件拷到了另一个目录），看到 `IDENTITY_BACKEND_MISMATCH` 就知道是「换了后端但锚点没换」。

而 `canonical_storage_uuid` / `new_storage_uuid` 这一对说明 UUID 有规范形式——所以「两个 UUID 是否相等」不是字符串比较，而是规范形式比较。这一点在 i03（存储锚点）那页会展开。

**JSON 后端的身份问题比数据库多一层，因为它有多个文件。**

docstring 那条说：「在 JSON 上，每个快照还携带一个所有者行命名它的工作区，而锚点列出已注册的快照：**一次启动在 bind 锁下注册它的工作区一次**」。

所以数据库后端只有「一个容器一个 UUID」，而 JSON 后端有「一个目录 N 个快照文件，每个属于一个工作区」。这就需要两份额外的记录：每个快照的 owner 行、以及锚点里的已注册列表。

而 `JSON_SHARD_SUFFIX = "json_shard"` 那条注释明确它的地位：

> 一个 JSON 快照的所有者：它所属的工作区。**布局元数据，不是配置**——它只存在于 JSON 快照里，并在配置迁移到数据库时被丢弃。

所以这是一条「后端特定」的键，而它被标记为「布局元数据」——因此迁移时不需要搬它。这是一个很干净的分层：配置内容跨后端不变，而**配置的存放布局**是后端特定的。

**`configuration_storage_implementations` / `default_configuration_storage` / `resolve_configuration_storage` 三件套处理「选择哪个后端」的默认规则。**

`FILE_BACKED_CONFIG_STORAGES = frozenset({"JsonKVStorage"})` ——一个单元素集合，但用 `frozenset` 而不是直接比较字符串。理由是它可能扩展，而调用方写 `storage in FILE_BACKED_CONFIG_STORAGES` 就不必改。

而对 Redis 业务 KV 的特例：

docstring 说「未设时，选择跟随 `kv_storage`，好让既有部署落在它的行已经在的地方，**除了 Redis 业务 KV 默认用 JSON 配置**」。

所以默认规则是「跟随业务 KV」——这样升级后配置就落在数据已经在的那个后端，不需要迁移。而 Redis 是例外，因为「配置是元数据，不该与业务数据放在一起」这条原则在 Redis 上更重要（Redis 常被当作缓存）。当然这个理由是我的推论——docstring 只说了「除了」，没给理由，所以这一点值得标注为推论而不是源码事实。

## 关键取舍

**源码事实：** 配置容器的名字在**代码**里（`CONFIG_CONTAINER_TAG`），路径由 `working_dir` 决定，「没有任何 `*_WORKSPACE` 变量能碰到它，没有任何设置能挪动它」。

**源码事实：** `CONFIG_STORAGE` 只接受四个 KV 后端，其他任何存储「在构造期按名字拒绝」；未设时跟随 `kv_storage`，但 Redis 业务 KV 默认用 JSON 配置。

**源码事实：** 键格式 `<workspace>/<suffix>`、`$default/<suffix>`、`$meta/<suffix>`；业务名不能以 `$` 开头（由 `validate_workspace` 保证，不在 `config_key` 里）；`/` 是唯一分隔符且 `validate_workspace` 禁止工作区名含 `/`，**而键仍然从不被重新解析**。

**源码事实：** `config_key` 有两条对称的作用域守卫，其中一条的理由是「`SERVER_SCOPE` 不是一个工作区」——防止按工作区登记的键被当全局键。

**源码事实：** 每个后缀必须先登记在 `CONFIG_KEY_REGISTRY`，未登记是「编程错误而不是运行时状况」；`registry_spec` 的错误消息给出声明所需的字段。

**源码事实：** `ConfigKeySpec` 是 `frozen=True`，七个字段；`schema` 是**人类可读字符串**而不是形式化 schema，服务于审阅而不是校验。

**源码事实：** `writers` 里包含**只删除**的调用方（`/documents/clear`、`lightrag-clear-storage`），「好让声明的列表保持是完整列表」；两者不打 `updated_by`，因为删除不留下行。

**源码事实：** `make_config_row` 只让业务行携带 `workspace` 字段——「元数据由它注册的键识别，不是一个虚构的工作区」。

**源码事实：** 一条基线记录的是**采纳的空间**而不是写入的空间；三个目标各一条因为「它们是独立容器、有独立历史」；`origin` 只作诊断，**从不进入裁决**。

**源码事实：** `EMBEDDING_TARGETS` 的顺序与重建工具的执行顺序绑定。

**源码事实：** 「读是严格的」只适用于**决定实例能否服务**的记录；一次完不成的读抛 `ConfigurationStorageError`，「永远不被误认为缺失」。

**源码事实：** 基线的原子认领「覆盖一个 Gunicorn master 的 worker，不会更宽」——跨 master 不保证；flush 与严格回读都在键控锁内。

**源码事实：** 容器身份行「一旦有效就永不被覆盖，永不被工作区维护删除」；十个 `IDENTITY_*` 错误码分别对应一种「两个部署关系不对」的情形，让诊断可区分。

**源码事实：** JSON 后端除了容器 UUID 还有两份额外记录：每个快照的所有者行（`json_shard` 后缀）与锚点里的已注册快照列表；`json_shard` 被明确标记为「布局元数据，不是配置」，迁移到数据库时丢弃。

**架构推论：** 「配置容器名在代码里」这条是整个设计的支柱。若容器名可配置，那么「配置在哪里」这个问题就需要配置来回答——而那份配置本身也在某个容器里。这个环没有干净的解法，所以唯一正确的做法是**用不可配置的东西当根**：代码里写死的名字，加上一个由 `working_dir` 决定的路径。

**架构推论：** 「键从不被重新解析」与「每个后缀都要登记」是一对互补的决定。前者说明读路径不需要解析逻辑（于是改键格式是安全的）；后者说明「这个键是什么」由注册表回答而不是由字符串形状推断。两者合起来把「键的语义」集中到一个 dict 里，而不是散在解析代码里。**判据是：这个格式的消费方需要理解它吗？** 需要的话，就用结构化载体；不需要的话，就只单向构造、绝不反向解析。

**架构推论：** 「`origin` 只作诊断，从不进入裁决」这条分层在任何「带来源标记的决策数据」里都适用。判定逻辑只该看事实（`model`、`dim`），而来源（怎么得到的）只该出现在日志与错误消息里。混淆两者的后果是：一个用 `rebuild` 建立的基线会因为「它不是 probe 来的」而被当成弱证据，而实际上它们等价。反过来，一个 `probe` 得到的结论也不该因为来源可靠就被免于校验。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/config_store.py` | `ConfigScope` | 两值作用域枚举，让错配在构造期报错 |
| `lightrag/config_store.py` | `ConfigKeySpec` | 冻结的注册表条目，七个字段 |
| `lightrag/config_store.py` | `CONFIG_KEY_REGISTRY` | 所有合法后缀的单一登记处 |
| `lightrag/config_store.py` | `registry_spec` | 查注册表，未登记时给出声明指引 |
| `lightrag/config_store.py` | `config_key` | 构造键的唯一出口，含两道作用域守卫 |
| `lightrag/config_store.py` | `scope_prefix` | `$meta` / `$default` / 工作区名三路 |
| `lightrag/config_store.py` | `make_config_row` | 建行，元数据行不带 `workspace` 字段 |
| `lightrag/config_store.py` | `EmbeddingBaseline` | 一个目标的已采纳嵌入空间 |
| `lightrag/config_store.py` | `BaselineOrigin` | probe / empty / rebuild 三值，仅诊断 |
| `lightrag/config_store.py` | `embedding_baseline_suffix` | 白名单校验后的动态后缀 |
| `lightrag/config_store.py` | `EMBEDDING_TARGETS` | 三目标，顺序与重建工具一致 |
| `lightrag/config_store.py` | `embedding_baseline_key` | `<workspace>/embedding/<target>` |
| `lightrag/config_store.py` | `STORAGE_IDENTITY_SUFFIX` | 容器 UUID 的键后缀 |
| `lightrag/config_store.py` | `JSON_SHARD_SUFFIX` | JSON 快照所有者行，布局元数据 |
| `lightrag/config_store.py` | `KEY_SEPARATOR` | 唯一分隔符，且键从不被重新解析 |
| `lightrag/config_store.py` | `resolve_configuration_storage` | 后端选择与 Redis 特例 |
| `lightrag/config_store.py` | `precheck_embedding_baselines` | 启动时基线比对，严格读 |
| `lightrag/config_store.py` | `IdentityBinding` | 容器身份与锚点的绑定 |
| `lightrag/config_store.py` | `inspect_shard_rows` | JSON 快照行检查，区分损坏与缺失 |

## 参考

- [lightrag 源码：config_store.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/config_store.py)
- [lightrag 源码：config_shards.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/config_shards.py)
- [UUID 规范（RFC 4122）](https://www.rfc-editor.org/rfc/rfc4122)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

主核里最接近的是**系统目录（system catalog）**：一张记录「数据库里有哪些表、每个表有哪些列、列的类型是什么」的表，而它自己也是一张表。这个自指结构在数据库里是可行的，因为 catalog 的位置是引导阶段（bootstrap）确定的，不依赖任何用户配置。

这一层的处境一样，解法也一样：**用代码里写死的容器名当引导根**。差别在于主核的目录是强类型的（列定义有类型系统），而这里是弱类型的（`schema` 只是一个说明字符串）。这个差别来自两者面对的错误成本：一个错列类型会让查询执行崩，而一个错配置形状最多让启动报错。

与 a01（存储契约）的分工很直接：那一页说「配置存储被刻意排除在数据命名空间之外，用独立的固定容器 `_lightrag_config`」；这一页说那个容器内部长什么样。

与 g04（嵌入基线）的连接最紧：那一页讲**如何取证**（抽样重嵌入 + 余弦），这一页讲**证据存在哪、怎么不丢**（`embedding/<target>` 键、严格读、原子认领）。所以 g04 产出判定，本页持久化判定。

与 i03（存储锚点）的关系是同一套身份机制的下一层：本页的 `IDENTITY_*` 错误码与 `canonical_storage_uuid` 由锚点文件那一层实现。

对 AI 工程最实用的一条是**「配置/元数据不能跟着被配置的数据走」**。把配置存在业务数据旁边看起来很自然（同一个数据库、同一个卷），但它会造出一个不可恢复的环：配置指向数据位置，而位置信息本身也在那个位置。正确做法是让「根位置」由不可配置的东西决定（代码常量 + 工作目录），其余都可以配置。

另一条是「键单向构造、绝不反向解析」。这一条的价值在演进时显现：改键格式（加前缀、换分隔符）只需要改构造端，读端不受影响，因为它从来不看键的形状。反之，任何 `key.split("/")` 都会在格式变化时变成静默错误。判据是：**这个名字需要被理解，还是只需要被比较？** 只需要比较的话，就别解析。

最后一条是「`origin`/来源标记只作诊断」。任何带「这个值是推断出来的还是算出来的」这类元数据的地方，都不该让来源影响判定——否则两类等价的事实会被区别对待。来源该出现在日志与错误消息里，帮人定位「为什么系统认为是这样」。
