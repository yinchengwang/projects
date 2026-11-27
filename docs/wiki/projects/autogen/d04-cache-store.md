---
project: autogen
feature: cache-store
title: 缓存存储抽象与内存实现
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_cache_store.py
    symbols: [CacheStore, InMemoryStore, InMemoryStoreConfig]
---

# 缓存存储抽象与内存实现

## 一句话职责

给出 `get`/`set` 两个方法的极简键值缓存契约，并提供一个零依赖的内存实现，让上层（例如模型响应缓存）可以在不改调用方代码的前提下换后端。

## 机制是什么

这个抽象刻意做得非常薄：只有 `get(key, default=None)` 与 `set(key, value)` 两个抽象方法。没有删除、没有 TTL、没有批量、没有迭代、没有大小限制，也没有并发语义声明。之所以敢这么薄，是因为它承载的不是通用 KV 存储需求，而是「同一次调用参数 → 上一次结果」这一种用法——命中就拿走，没中就算，过期与被淘汰交给具体后端自己判断。

唯一的结构性设计在继承关系上：`CacheStore` 同时继承 `ABC` 与 `ComponentBase[BaseModel]`，并声明 `component_type = "cache_store"`。这意味着任何缓存后端天然是**组件**：可以用 `ComponentModel` 描述、dump 成配置、再被 `load_component` 装回。所以「换后端」不只是换一个类，而是换一份配置——这正是抽象层要达成的事。

泛型参数 `T` 只是类型标注，运行期不做任何检查。`InMemoryStore[T]` 的 `store` 就是一个 `Dict[str, T]`，`get` 直接转调 `dict.get(key, default)`，`set` 直接赋值。所以「键不存在返回默认值」与「重复 set 覆盖旧值」这两个行为不是实现出来的，而是 dict 语义的直接投影。

## 怎么实现的

**抽象侧。** `CacheStore.get` 与 `CacheStore.set` 都是 `@abstractmethod`，返回 `Optional[T]`。docstring 里一句关键约定：**子类负责底层存储的生命周期**。基类不做 `close`/`__aenter__`，因为一旦基类声明生命周期方法，所有后端都必须实现，而有些后端（如内存 dict）根本没有生命周期。

**内存实现的配置。** `InMemoryStoreConfig` 是个**空**的 pydantic 模型——没有任何字段。这不是遗漏，而是必然：内存存储的全部状态就是进程内的一个 dict，没有可序列化的构造参数。于是 `_to_config` 返回 `InMemoryStoreConfig()`，`_from_config` 忽略 config 直接 `return cls()`。空配置让 dump 出来的 `ComponentModel.config` 是 `{}`，也让「从配置恢复」这件事只能是「恢复一个空缓存」，无法恢复缓存内容。

**组件标识。** `InMemoryStore.component_provider_override = "autogen_core.InMemoryStore"`，`component_config_schema = InMemoryStoreConfig`。provider 被显式覆盖而非按模块路径自动推导，源码注释在基类里说明了理由：避免内部模块名（`autogen_core._cache_store`）成为线上配置的一部分。

**泛型与运行期。** `InMemoryStore(CacheStore[T], Component[InMemoryStoreConfig])` —— 泛型参数在实例化时常被写出来（`InMemoryStore[int]()`），但 `get`/`set` 不校验值类型。类型检查止步于静态分析。

**对外入口。** 包根 `autogen_core.__init__` 直接导出 `CacheStore` 与 `InMemoryStore`，不导出 `InMemoryStoreConfig`，也不导出 `InMemoryStore` 的 `Dict` 内部字段。于是使用者只能通过 `get`/`set` 访问缓存，无法旁路。

**与上层的接法。** 本仓只负责抽象与内存实现，`autogen-ext` 侧才实现磁盘与 Redis 后端（`DiskCacheStore`、`RedisStore`），并把它们包进 `ChatCompletionCache` 以缓存模型响应。本页只讲本仓机制：核心包提供一个默认实现，保证「不装任何扩展也能跑通缓存路径」。

## 关键取舍

**源码事实：** `InMemoryStore` 的 `store` 是公开属性（`self.store: Dict[str, T] = {}`），下划线缺失。这意味着「只能通过 get/set 访问」不是强制的——外部代码可以直接 `store["k"] = v`。抽象层的封装在内存实现上是自愿遵守的。

**源码事实：** `InMemoryStoreConfig` 的 docstring 是空的，类体只有 `pass`。因此从配置加载得到的永远是一个**空缓存**。这是刻意的：缓存内容属于运行时状态，若混进配置就会让「同一份配置」在不同时刻产出不同对象，破坏「配置是可重复盖章的蓝图」这条不变式。

**源码事实：** 只有 `get`/`set` 两个方法，没有 `delete`。淘汰策略如果存在，只能由子类在 `set` 里自行实现，或干脆交给后端（例如 Redis 的 TTL）。基类不预留钩子。

**架构推论：** 缺失的 `delete` 意味着缓存无法被显式失效。要「清空」只能新建一个实例或直接操作内部 dict——对一个响应缓存来说，这在实践中通常可接受（键是参数哈希，参数不变则结果理论上不变），但对多轮对话这类会变的输入就必须依赖别的机制。

**架构推论：** `CacheStore` 继承 `ComponentBase` 而非 `Component`，说明它是**接口**（也可以被实现类单独继承），但 `InMemoryStore` 又额外继承了 `Component[InMemoryStoreConfig]`。这是本仓一贯的双继承模式：接口侧挂 `ComponentBase` 以获得 `load_component`，具体类挂 `Component` 以获得 `_from_config`。漏掉任何一侧，`is_component_class` 的检查都会失败。

**架构推论：** 键是 `str`，不是任意可哈希对象。于是所有键的构造（例如把调用参数压成指纹）都发生在调用方，缓存层不理解语义、也不知道什么算「同一个请求」。这让缓存层保持通用，代价是正确性完全取决于调用方如何拼键——拼宽了会命中不该命中的结果，拼窄了等于永远 miss。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_cache_store.py` | `CacheStore` | 缓存契约：`get` / `set`，标记 `component_type="cache_store"` |
| `python/packages/autogen-core/src/autogen_core/_cache_store.py` | `CacheStore.get` | 抽象读取，未命中返回 `default` |
| `python/packages/autogen-core/src/autogen_core/_cache_store.py` | `CacheStore.set` | 抽象写入，同名键覆盖 |
| `python/packages/autogen-core/src/autogen_core/_cache_store.py` | `InMemoryStoreConfig` | 空配置模型，说明内存实现无构造参数 |
| `python/packages/autogen-core/src/autogen_core/_cache_store.py` | `InMemoryStore` | dict 支撑的内存实现，公开 `store` 字段 |
| `python/packages/autogen-core/src/autogen_core/_cache_store.py` | `InMemoryStore._to_config` | 返回空配置 |
| `python/packages/autogen-core/src/autogen_core/_cache_store.py` | `InMemoryStore._from_config` | 忽略配置，返回新的空实例 |

## 参考

- [AutoGen 源码：_cache_store.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_cache_store.py)
- [AutoGen 文档：Model Clients 与响应缓存](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/model-clients.html)
- [Python 文档：abc 抽象基类](https://docs.python.org/3/library/abc.html)

## 与主核 / AI 的关系

主核的 buffer pool 也是「键 → 页」的缓存，但那里的键是 `(relfilenode, blocknum)` 这样的定长结构，命中率、淘汰、脏页回写、并发保护全部是核心议题，甚至决定事务的正确性。这里的缓存抽象连淘汰都没有，因为它服务的是**幂等的、可重算的**东西：模型响应丢了就再调一次，最坏结果是多花钱多花时间，绝不会让状态错乱。这条界线值得记住——**缓存要不要做复杂，取决于「未命中」的代价是正确性问题还是成本问题**。

对 AI 工程而言，缓存的价值几乎全部落在 token 成本上。把 `create` 的调用参数（消息列表、tools、json_output、extra_create_args）压成键，命中即省一次计费。所以工程上的关键不是缓存实现，而是**键的口径**：扩展侧文档明确提示，改了 `tools` 或 `json_output` 就可能 miss，因为这些参数参与键的构造。反过来，若某参数参与了行为却没进键，就会返回「看起来对但实际错配」的结果——这是缓存最危险的失败模式，比 miss 昂贵得多。

最后一点：缓存命中必须让 `total_usage` 保持不变。也就是说，被缓存的响应仍然要按原始用量记账，否则成本统计会在命中时凭空缩水，把「省了钱」错误地记成「没花钱」。
