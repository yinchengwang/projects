---
project: postgres
feature: namespace-resolution
page_type: feature
schema_version: 1
refs:
  - path: src/backend/catalog/namespace.c
    symbols: [RangeVarGetRelidExtended, LookupCreationNamespace, recomputeNamespacePath]
---

# 命名空间解析

## 一句话职责

命名空间解析把未限定或限定的 SQL 名称按当前用户、search_path 和权限规则映射为对象 OID，并为新对象选择允许创建的 schema。

## 机制是什么

未限定关系名不是一个固定 OID：`RelnameGetRelid` 按当前 `activeSearchPath` 的顺序逐个 namespace 查 `pg_class`，第一个同名 relation 赢得解析；`RangeVarGetRelidExtended` 负责把 SQL parser 产生的 `RangeVar` 路由到该查找，并在需要时对目标 relation 加锁、处理“查找过程中并发 DDL 改名/替换”的重试。若名字显式带 schema，则必须精确解析该 schema；调用 `LookupExplicitNamespace` 会先看 `pg_namespace`，再检查当前用户对 schema 的 `USAGE` 权限并调用 namespace search hook。

创建目标的路由规则不完全等于对象查找规则：`LookupCreationNamespace` 对普通 schema 检查 `CREATE`，对 `pg_temp` 别名可按需初始化本 backend 的临时 schema；未限定 CREATE 则使用 search_path 的第一个可创建 namespace（或延迟待建的临时 schema）。search_path 本身被缓存为 OID 路径：按 `(namespace_search_path 字符串, roleid)` 键缓存解析后的候选列表与权限结果，再加入隐式 `pg_catalog`、当前 backend 的 `pg_temp`，过滤重复项并经 hook 决定最终顺序。

本页负责“名字 -> namespace/OID”以及 search_path 展开，不负责对象权限 ACL 的通用判断/属主逻辑（见权限页），也不负责 syscache 如何存取 `pg_namespace`/`pg_class` 元组。

## 怎么实现的

1. `recomputeNamespacePath` 若 `baseSearchPathValid` 且 `namespaceUser == GetUserId()` 就直接返回，否则以当前 GUC `namespace_search_path` 和 role ID 调 `cachedNamespacePath`。缓存未命中时 `preprocessNamespacePath` 拆分逗号列表，把普通 schema 名查成 OID，只有 `USAGE` 通过的才保留；`$user` 替换为当前 role 同名 schema；`pg_temp` 替换本 session temp namespace，不存在且处于首位时记录 `temp_missing`。
2. `finalNamespacePath` 去重、逐个调用 `InvokeNamespaceSearchHook(namespaceId, false)`；记录显式路径第一个 namespace 作为 `firstNS`（供默认创建 namespace 使用），再把不在显式路径中的 `pg_catalog` 和本 backend temp namespace 前置加入最终搜索路径。若存在 `object_access_hook`，最终路径每次都重算，因为 hook 可能改变可见顺序。
3. `RangeVarGetRelidExtended` 先拒绝 catalogname 不是当前数据库的跨库引用；带 `RELPERSISTENCE_TEMP` 时要求 current temp namespace 已初始化，显式 schema 若不是该 temp namespace 就报错；一般显式 schema 走 `LookupExplicitNamespace`，未限定名走 `RelnameGetRelid`，它按 `activeSearchPath` 顺序返回首个命中 OID。
4. `RangeVarGetRelidExtended` 在查找前记录 `SharedInvalidMessageCounter`。如请求锁且结果有效，先对刚解析出的 OID 加锁（NOWAIT/SKIP_LOCKED 则走条件锁）；若锁过程中处理了失效消息，再按名字重查一次。重查得到同 OID 即答案稳定；若 OID 改变就解锁旧 OID 并对新结果重试，避免名称刚被并发 DDL 替换时锁错对象。`NoLock` 路径明确接受可能变化并返回首次答案。
5. 查不到且 `RVR_MISSING_OK` 未设时返回 InvalidOid 并报 undefined relation；若 missing-ok，同样仍处理 pending invalidation，以清理负 catcache 条目。callback 在取得 relation lock 之前执行，所以可在锁前检查调用者权限或获取其他锁；如果 OID 因并发 DDL 改变，callback 会对新 OID 再执行。
6. `LookupCreationNamespace` 对 `pg_temp` 调 `AccessTempTableNamespace(false)` 后返回临时 schema；普通名称用 `get_namespace_oid` 并检查 `ACL_CREATE`。调用该路径可能创建或清理临时 namespace，并推进 command counter。`LookupExplicitNamespace` 则区别地检查 `ACL_USAGE`；对象查找与对象创建因此使用不同权限。
7. `AccessTempTableNamespace` 记录当前事务访问了临时 schema；创建前检查数据库 `ACL_CREATE_TEMP`，拒绝 recovery 与 parallel worker，按 `pg_temp_<procno>` 创建或清理当前后端 namespace，并建立相应 toast-temp schema；设置 `MyProc->tempNamespaceId` 防止别的后端并发清除正在使用的临时对象。

## 关键取舍

- 源码事实：`RangeVarGetRelidExtended` 在加锁前解析名称、锁 OID 后对比失效消息计数，必要时重查并改锁新 OID；`NoLock` 时不做这轮稳定性检查。架构推论：把“名字稳定”绑定到 relation lock 与失效处理，常见有锁操作可得到稳定对象；无锁调用者必须接受答案变化，若所有调用都强行加锁会增加无意义锁开销并与已持锁调用者形成锁顺序问题。
- 源码事实：search_path preprocess 阶段对普通 schema 过滤 `USAGE`，再将隐式 `pg_catalog` 与 session `pg_temp` 前置；创建 namespace 使用显式路径第一个元素，而不一定是搜索列表中的首个隐式项。架构推论：对象解析与默认 CREATE 目标被拆成两个结果，避免把隐式搜索目录误当成用户选择的创建位置；若共用一个列表首项，默认建表可能落入系统目录或临时目录。
- 源码事实：`LookupExplicitNamespace` 检查 `USAGE`，`LookupCreationNamespace` 检查 `CREATE`，`LookupCreationNamespace` 还会初始化 `pg_temp`。架构推论：把“可以解析/访问 schema”与“可以在 schema 里创建对象”分开，防止只拥有一个权限就同时获得另一个权限；把它们合并会导致读权限被错误提升为建对象权限。
- 源码事实：search_path cache entry 以路径字符串和 roleid 为索引，缓存 OID 列表、finalPath、首个可创建 schema 与 temp-missing 标志；hook 存在时重算 finalPath。架构推论：缓存减少反复 catalog lookup 与 ACL 查询，但把 role 和 hook 也纳入语义一致性条件；若只缓存字符串，role 切换后会沿用前用户权限过滤结果。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/catalog/namespace.c` | `RangeVarGetRelidExtended` | 执行 RangeVar 路由、显式/隐式 namespace 选择、可选 callback 与 relation-lock 后失效重查。 |
| `src/backend/catalog/namespace.c` | `LookupCreationNamespace` | 为 CREATE 选定 schema 并检查 CREATE 权限，支持按需初始化 `pg_temp`。 |
| `src/backend/catalog/namespace.c` | `recomputeNamespacePath` | 从 `(search_path, role)` 缓存取最终 OID 搜索路径与默认创建 namespace，更新 active generation。 |

## 与主核 / AI 的关系

主核的多模态对象定位不使用 PostgreSQL schema/search_path 的名称遮蔽规则。可借鉴把名字解析结果绑定调用身份、把“读/使用 namespace”与“创建对象”权限拆开；不要把 search_path 当成主核的对象发现路径。

## 参考

- [PostgreSQL 文档：模式与搜索路径](https://www.postgresql.org/docs/current/ddl-schemas.html) —— 说明 schema namespace、search_path 与对象名解析规则。
- [PostgreSQL 文档：客户端连接默认值](https://www.postgresql.org/docs/current/runtime-config-client.html) —— 说明 search_path 参数及其配置入口。
- [PostgreSQL 官方源码：namespace 解析实现](https://github.com/postgres/postgres/blob/master/src/backend/catalog/namespace.c) —— 支撑名称解析、search_path cache 与 temp namespace 路由。
