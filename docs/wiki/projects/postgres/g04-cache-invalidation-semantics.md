---
project: postgres
feature: cache-invalidation-semantics
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/cache/inval.c
    symbols: [LocalExecuteInvalidationMessage, InvalidateSystemCaches, CacheInvalidateHeapTuple]
---

# 缓存失效语义

## 一句话职责

缓存失效语义决定“哪一次目录元组改动在什么时刻、以什么粒度让本进程与其它后端丢弃或重建哪些缓存条目”，覆盖消息登记、命令边界本地执行、提交时广播与缓冲区溢出后的整体重置。

## 机制是什么

共享失效消息是对失效对象/范围的声明，不携带新元数据内容：结构按类型仅包含数据库/目录/关系标识、catcache ID 与哈希值、smgr locator 等字段。`CacheInvalidateHeapTuple` 把被改动或已过时的目录元组翻译成失效事件：先按该目录的 catcache 键列登记哈希级失效，再根据目录种类判断这次改动是否牵连某个 relcache 条目（`pg_class` 行改的是它自己的 relid；`pg_attribute`、`pg_index` 用其引用列；`pg_constraint` 仅外键约束参与）。只对系统目录登记：用户表元组既不在 catcache，也不影响 relcache。

登记事件按**命令粒度**与**事务粒度**两级排队：命令结束时在本进程执行（同事务后续命令必须看到自己的改动），事务提交时才将排队消息写入共享失效队列供其它后端接收；事务中止时只本地执行此前命令的事件，不广播（其它后端本来也没看见这些改动）。整体队列溢出重置也只在接收后端本地清理缓存。

## 怎么实现的

1. `CacheInvalidateHeapTuple` 进入 `CacheInvalidateHeapTupleCommon`：bootstrap 模式直接返回（此时没有别的后端依赖该机制）；非系统目录返回；系统目录的 TOAST 表也返回（它们不携带被缓存的目录元组）。
2. 然后分两路。catcache 路：若该目录被标记为“只使目录快照失效”（`RelationInvalidatesSnapshotsOnly`），登记 `SHAREDINVALSNAPSHOT_ID` 事件而不是哈希级事件；否则 `PrepareToInvalidateCacheTuple` 按该目录索引的键列算出每个受影响 cache 的哈希，逐个 `RegisterCatcacheInvalidation`。
3. relcache 路：按目录种类取“被定义的关系”OID——`pg_class` 用行内 oid 且按 `relisshared` 决定 databaseId，`pg_attribute` 用 `attrelid`，`pg_index` 用 `indexrelid`，`pg_constraint` 仅当 `contype == CONSTRAINT_FOREIGN` 且有 `conrelid`；其它目录直接返回。得到 OID 就 `RegisterRelcacheInvalidation`。源码注释明确：这里忽略 `newtuple`，因为假定一次 UPDATE 不会把某元组从“属于某个 relcache 条目”搬到另一个。
4. 登记函数把消息追加到 `CurrentCmdInvalidMsgs` 的 catcache/relcache 子组，并去重。`RegisterRelcacheInvalidation` 还额外 `GetCurrentCommandId(true)` 以保证下一个 `CommandCounterIncrement` 会触发命令末尾处理，并在 relId 为 InvalidOid 或该关系在 relcache init 文件里时把 `RelcacheInitFileInval` 置位，用在提交时先摘 init 文件再广播。
5. 命令边界：`CommandEndInvalidationMessages` 对 `CurrentCmdInvalidMsgs` 调 `LocalExecuteInvalidationMessage`（先 catcache 子组再 relcache 子组），若启用逻辑解码信息还调用 `LogLogicalInvalidations`，最后把本命令消息并入 `PriorCmdInvalidMsgs` 供事务结束时广播。
6. `LocalExecuteInvalidationMessage` 按消息 ID 分派：catcache 消息检查 `dbId` 是否为本库或共享，先 `InvalidateCatalogSnapshot` 再 `SysCacheInvalidate(id, hashValue)` 并回调 syscache 回调；目录级事件走 `CatalogCacheFlushCatalog`；relcache 事件在 relId 有效时 `RelationCacheInvalidateEntry`，否则整体 `RelationCacheInvalidate`，随后触发 relcache 回调；另有 smgr、relmap、snapshot、relsync 分支。它**只影响本进程**，不向其它后端发送。
7. 提交：`AtEOXact_Inval(true)` 把当前命令消息并入 prior 组，经 `SendSharedInvalidMessages` 写入共享失效队列（若需作废 init 文件则写入前后各做一次 `RelationCacheInitFilePre/PostInvalidate`）；接收者之后各自在本地执行消息。中止：`AtEOXact_Inval(false)` 只对 `PriorCmdInvalidMsgs` 本地执行，丢弃 `CurrentCmdInvalidMsgs` 并不广播。子事务提交把两组挂到父级，子事务中止只本地执行 prior 组后弹栈。
8. 溢出与兜底：`AcceptInvalidationMessages` 调 `ReceiveSharedInvalidMessages(LocalExecuteInvalidationMessage, InvalidateSystemCaches)`；当接收端发现共享队列溢出、无法知道具体丢了哪些消息时，仅在该接收进程调用 `InvalidateSystemCaches`。它执行 `InvalidateCatalogSnapshot`、`ResetCatalogCachesExt(false)`、`RelationCacheInvalidate(false)`（注释说明连带 smgr 与 relmap），再通知 syscache/relcache/relsync 回调，作全量失效，而不是广播一条“全局 reset”消息。

## 关键取舍

- 源码事实：事件按命令与事务两级分别执行：命令末本地执行，提交才广播，中止只本地执行不广播。架构推论：同事务后续命令必须立即看到自己的元数据改动，而其它后端不应看到未提交改动；若命令末就广播，并发后端会按未提交状态重建缓存并读到不该看到的结果；若提交才首次本地执行，同事务内后续语句会用到过期缓存。
- 源码事实：`RegisterRelcacheInvalidation` 用 `GetCurrentCommandId(true)` 主动推进“命令需要处理”的标志，并只对“关系在 init 文件里或整体作废”设置 `RelcacheInitFileInval`。架构推论：relcache init 文件是提交时才能安全摘除的加速结构，把它只绑定到真正相关的失效事件；若每次任何失效都摘，提交路径会频繁做文件删除而失去该优化意义。
- 源码事实：`CacheInvalidateHeapTuple` 只取 `tuple` 不取 `newtuple` 来判断 relcache 关联，源码注释说明这是基于“UPDATE 不会换归属”假设。架构推论：该假设让每条事件只需一次判断、省去对两版本的比较；若某类目录将来允许改 `conrelid`/`attrelid`，就必须补处理，否则旧关系的缓存会漏失效。
- 源码事实：接收端一旦发现消息丢失就调 `InvalidateSystemCaches` 整体作废，而不是尝试补齐。架构推论：细粒度补发需要可回放的完整历史，与固定容量环冲突；整体重置把正确性责任从“追平消息”降为“全量重建”，代价是一次昂贵的冷启动式重载。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/cache/inval.c` | `CacheInvalidateHeapTuple` | 把一次目录元组变更（含 update 的旧版本）翻译成 catcache 哈希事件与可能的一条 relcache 事件。 |
| `src/backend/utils/cache/inval.c` | `LocalExecuteInvalidationMessage` | 单条消息在**本进程**内的对象级分派：目录快照、syscache、目录整体、relcache、smgr、relmap、snapshot、relsync。 |
| `src/backend/utils/cache/inval.c` | `InvalidateSystemCaches` | 接收端队列溢出时本地全量失效：目录快照、catcache、relcache（包含 smgr/relmap）及注册回调；不广播 reset 消息。 |
| `src/backend/utils/cache/inval.c` | `AtEOXact_Inval` | 命令/事务消息在提交广播或中止本地执行的边界处理。 |
| `src/backend/utils/cache/inval.c` | `SendSharedInvalidMessages` | 将提交事务的失效消息写入共享失效队列，供各接收后端分别处理。 |

## 与主核 / AI 的关系

主核的缓存失效不采用“目录元组 → 哈希事件 → 命令/事务两级执行”的语义模型。可借鉴的是把“谁在什么时刻看到失效”显式分成命令边界与事务边界；不要把这页讲成共享环传输，也不要把“消息已登记”说成“所有后端缓存已失效”。

## 参考

- [PostgreSQL 文档：系统目录](https://www.postgresql.org/docs/current/catalogs.html) —— 说明被缓存与失效的系统目录表。
- [PostgreSQL 文档：开发者选项](https://www.postgresql.org/docs/current/runtime-config-developer.html) —— 说明强制丢弃缓存的调试选项，用于验证失效路径。
- [PostgreSQL 官方源码：缓存失效实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/cache/inval.c) —— 支撑消息登记、命令边界本地执行与整体重置。
