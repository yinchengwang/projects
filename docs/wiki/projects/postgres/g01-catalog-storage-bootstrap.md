---
project: postgres
feature: catalog-storage-bootstrap
page_type: feature
schema_version: 1
refs:
  - path: src/backend/catalog/catalog.c
    symbols: [IsCatalogRelation, GetNewOidWithIndex]
  - path: src/backend/bootstrap/bootstrap.c
    symbols: [BootstrapModeMain, boot_openrel]
  - path: src/backend/catalog/indexing.c
    symbols: [CatalogTupleInsert]
---

# 系统目录持久化与引导

## 一句话职责

系统目录就是一组用普通 heap 关系保存的元数据表；引导阶段在没有完整目录与索引的状态下先把它们创建并填入初始行，正常运行期再通过固定约定与索引维护读写这些表。

## 机制是什么

catalog 的“特殊”不在于另一种存储引擎：它们在磁盘上同样是 heap relation，靠 `pg_class` 行、`pg_attribute` 行与相应索引描述自己。真正特殊的是硬编码约定：哪些 OID 属于不可删除的 pinned 区间、哪些关系由 initdb 的 bootstrap 阶段创建、哪些关系允许就地更新、以及更新目录时必须同步维护目录索引。`IsCatalogRelation` 只回答“这个关系是否属于 bootstrap 期创建的系统目录集合”，判定依据是 relid 是否小于 `FirstUnpinnedObjectId` 这个 pinned OID 边界（边界值本身不包含在内），不做任何目录访问，因此它可以在 relcache 构建、syscache 填充等极早或递归敏感路径上安全调用。

引导阶段是另一套运行形态：`BootstrapModeMain` 进入 bootstrap processing mode 并置 `IgnoreSystemIndexes = true`，这使普通系统目录扫描（如 `systable_beginscan`）不会打开索引，而改走 heap scan；它与 `GetNewOidWithIndex` 在 bootstrap mode 下直接分配 OID、不做索引查重，是相关但不同的机制。目录索引按 BKI 的定义在引导事务中建立，初始化后首个正常启动会补齐/验证其余结构。

本页只讲目录作为普通关系的持久化约定与 bootstrap 初始化/写入路径。缓存策略（syscache/relcache 的命中与失效）不是这里的职责；目录内容的语义约束（依赖、权限、命名空间可见性）也在其余页。

## 怎么实现的

1. `BootstrapModeMain` 先解析 `--boot`/`--check` 及 `-D`、`-B`、`-c` 等选项并加载配置，`checkDataDir` 与 `ChangeToDataDir` 校验并进入数据目录，建立数据目录锁文件；随后 `SetProcessingMode(BootstrapProcessing)` 并设 `IgnoreSystemIndexes = true`。后者使 `systable_beginscan` 在 bootstrap 期间不打开系统索引、改用 heap scan；这不是 `GetNewOidWithIndex` 直接分配 OID 的原因，后者独立依据 `IsBootstrapProcessingMode()` 分支。
2. 引导进程初始化共享内存与信号处理，`BootStrapXLOG` 建立初始 WAL，`InitPostgres` 以 bootstrap 身份建立最小数据库环境；之后初始化 `attrtypes`、`Nulls` 数组和 BKI 词法扫描器，进入一次 `StartTransactionCommand` / `boot_yyparse` / `CommitTransactionCommand` 的引导事务，BKI 脚本即在此事务内建表、填初始行、建索引与视图。
3. `boot_openrel` 执行 BKI 的 `OPEN` 命令：先保证 `pg_type` 已填充（`Typ` 为空则 `populate_typ_list`），关闭先前打开的引导关系，再用 `makeRangeVar(NULL, relname, -1)` + `table_openrv` 打开目标关系，把其 `rd_att` 中每个属性的固定字段拷进 `attrtypes`，供后续 `INSERT` 按列类型编码。`DEFINEATTR` 反向构造属性，靠 `gettype` 拿到类型 OID 与 typlen/typbyval/typalign/typstorage。
4. 引导结束时 `boot_yyparse` 返回，外层提交事务，随后 `RelationMapFinishBootstrap` 依据已登记的映射关系写出初始 relmap 文件，最后清理退出。引导后的第一次正常启动会再次补齐目录索引等结构。
5. 正常运行期写目录用 `CatalogTupleInsert`（及其 WithInfo/Multi 变体）：它先做（断言构建下的）目录约束检查，`CatalogOpenIndexes` 打开该目录的全部索引，`simple_heap_insert` 插入 heap 行，`CatalogIndexInsert` 为每个可插入索引构造索引项并 `index_insert`，最后关闭索引状态。目录索引不支持部分/表达式索引、排他约束与延迟唯一性，因此这里不建 EState，直接按索引定义取列。
6. catalog 中需要新 OID 的地方调用 `GetNewOidWithIndex`：bootstrap 模式下没有索引可用，直接 `GetNewObjectId()`；正常模式下循环 `GetNewObjectId()` 并借助目录索引以 `SnapshotAny` 查重，直到未命中为止，用 `SnapshotAny` 是为了看见未提交行、避免同事务内重复。重试次数超过阈值时按指数退避打日志，避免日志被刷爆。
7. `IsCatalogRelation` 只需 relid 比较；其配套的 `IsPinnedObject` 用同一 OID 边界回答“该对象是否因系统需要而不可删除”，而 `IsSystemRelation` 在此基础上放宽到用户表的 TOAST 表等，用于 `allow_system_table_mods` 一类保护判断。这些函数都刻意不访问目录，调用者可以依赖这一点。

## 关键取舍

- 源码事实：系统目录用与用户表相同的 heap 存储，靠 pinned OID 区间做硬编码分类（`IsCatalogRelation` 判 `relid < FirstUnpinnedObjectId`）。架构推论：选择“同一套存储 + 约定”而非独立的目录存储引擎，换来目录与普通表共用缓冲、WAL、元组与扫描代码；代价是分类知识散落在若干硬编码函数中，新增目录或改变边界要同步多处。
- 源码事实：`IgnoreSystemIndexes` 在 bootstrap 置真，`systable_beginscan` 因此不打开系统索引；`GetNewOidWithIndex` 则因 bootstrap mode 单独直接调用 `GetNewObjectId()`，不是因该 GUC/标志造成。BKI 初始化脚本为目录初始对象提供定义与 OID。架构推论：引导期既允许在系统索引不参与扫描时继续进行目录访问，也避免依赖索引进行 OID 查重；若把两条分支混为因果，会误以为关闭系统索引本身决定 OID 策略。
- 源码事实：`CatalogTupleInsert` 每次都 `CatalogOpenIndexes`/`CatalogCloseIndexes`，并为此单独提供 `CatalogTupleInsertWithInfo` 与 multi-insert 变体。架构推论：单行便利接口把“heap 插入 + 索引维护”封装成不可分割的一步，避免调用者漏更新索引；代价是批量插入要学会改用带状态的变体，否则索引信息构建开销按行重复。
- 源码事实：`GetNewOidWithIndex` 用 `SnapshotAny` 而非普通 MVCC snapshot 查重，并在 bootstrap 下不查重。架构推论：接受“理论上的 OID 环回竞态”以换取一次扫描即可看全所有行；改用需提交才能结束竞态的快照会把风险推得更高，源码注释明确点出这一点。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/catalog/catalog.c` | `IsCatalogRelation` | 以 pinned OID 上界判定关系是否属于 bootstrap 创建的系统目录集合，全程不访问目录，供极早路径安全使用。 |
| `src/backend/catalog/catalog.c` | `GetNewOidWithIndex` | 正常模式下用目录索引查重分配 OID，bootstrap 模式下退化为直接分配。 |
| `src/backend/bootstrap/bootstrap.c` | `BootstrapModeMain` | 引导进程主流程：切换 bootstrap mode、置 `IgnoreSystemIndexes` 使系统目录扫描避开索引、跑 BKI 脚本并提交。 |
| `src/backend/bootstrap/bootstrap.c` | `boot_openrel` | 处理 BKI `OPEN`，从已打开关系的 TupleDesc 抽出属性定义供后续行插入使用。 |
| `src/backend/catalog/indexing.c` | `CatalogTupleInsert` | 目录单行写入的合成入口：约束检查、heap 插入与逐索引项插入。 |

## 与主核 / AI 的关系

主核的对象与模态元数据没有 PostgreSQL 这样“目录本身就是 heap 表 + pinned OID 约定 + BKI 引导脚本”的结构。可借鉴的是把引导期的受限写入路径与正常运行期路径显式分开；不要把 `IsCatalogRelation` 的 OID 区间判定当作主核的类型/对象分类依据。

## 参考

- [PostgreSQL 文档：系统目录](https://www.postgresql.org/docs/current/catalogs.html) —— 说明系统目录表与用户表同构的关系。
- [PostgreSQL 文档：initdb](https://www.postgresql.org/docs/current/app-initdb.html) —— 说明数据库簇初始化与引导文件的作用。
- [PostgreSQL 官方源码：bootstrap 主流程](https://github.com/postgres/postgres/blob/master/src/backend/bootstrap/bootstrap.c) —— 支撑引导模式切换与 BKI 处理顺序。
