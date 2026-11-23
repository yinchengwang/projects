---
project: postgres
feature: syscache
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/cache/syscache.c
    symbols: [SearchSysCache, InitCatalogCache, ReleaseSysCache]
  - path: src/backend/utils/cache/catcache.c
    symbols: [SearchCatCache, InitCatCache]
---

# 系统缓存

## 一句话职责

syscache 是每个后端进程私有的 catalog 元组缓存；它按目录索引键查表，命中就返回缓存内的元组，避免每次读 `pg_class`、`pg_type` 这类高频系统表都真正扫描页面。

## 机制是什么

syscache 本身不实现缓存算法，它只是把 `syscache.h` 里的枚举 ID 映射到一组预先声明的 `catcache`：每个 catcache 绑定一个 catalog 关系 OID、一个索引 OID、固定数量的键列与初始桶数。`InitCatalogCache` 按枚举逐个 `InitCatCache`，同时把涉及的目录关系 OID 及其索引 OID 收集成两个有序数组，供后续失效判定做二分查找。

缓存内容是**本进程内存中的 `CatCTup`**，不是共享的，也不与 MVCC 快照绑定：`SearchCatCache` 命中时返回缓存副本并加引用计数，未命中时用目录索引做一次 `systable_beginscan` 把行读回并插入缓存，查不到则放入**负条目**以避免反复扫空。缓存条目只有在失效消息到达或重建缓存时才作废；缓存值本身不可被调用者修改。

本页只讲按索引键的命中、装载、引用计数与条目作废路径。失效消息如何产生、在 shared inval 队列里如何传输、以及各对象语义如何落到本地缓存，属于另一页；relcache 是另一套关系级缓存，也不在本文范围。

## 怎么实现的

1. `InitCatalogCache` 是纯内存初始化：断言每个枚举的 reloid/indoid 已填、`nbuckets` 为 2 的幂，逐个 `InitCatCache`；再对 `SysCacheRelationOid` 与 `SysCacheSupportingRelOid`（目录关系 + 对应索引）排序去重，使关系级失效消息能以二分判断“这个 OID 是否影响 syscache”。源码还断言 syscache 绑定的关系不满足 `RelationInvalidatesSnapshotsOnly`。
2. `InitCatCache` 在 `CacheMemoryContext` 中分配 `CatCache`，按 cacheline 对齐并清零，分配 `nbuckets` 个 dlist 桶头，`cc_lbucket` 延迟到第一次列表查询才分配；记录 `cc_reloid`、`cc_indexoid`、`cc_nkeys`、`cc_keyno[]`，但**不打开关系**。每个新 cache 挂进全局 `CatCacheHeader` 的 slist 链，桶数不足时按倍增加倍 rehash。
3. `SearchSysCache`/`SearchSysCache1..4` 只是把 ID 换成 `SysCache[cacheId]` 并断言键数与调用形态匹配，然后转调 `SearchCatCache`；后者的 `SearchCatCacheInternal` 先用 `CatalogCacheComputeHashValue` 计算键哈希，取桶后遍历，跳过 `dead` 条目、先比哈希再逐键比较，命中就 `dlist_move_head` 把它移到桶首以加速后续热点访问。
4. 命中且为正条目时，先 `ResourceOwnerEnlarge` 再 `refcount++` 并 `ResourceOwnerRememberCatCacheRef`，然后返回元组指针；命中为负条目则直接返回 NULL 而不加引用。这一引用计数是调用方必须配对的：`ReleaseSysCache`/`ReleaseCatCache` 递减并撤销 ResourceOwner 记录，计数归零且条目已 dead 才真正摘除。
5. 未命中进入 `SearchCatCacheMiss`：`table_open` 目标 catalog，拷贝模板 scan key 并填入本次参数，用 `systable_beginscan`（是否可用索引由 `IndexScanOK` 决定）扫一行，交给 `CatalogCacheCreateEntry` 反 toast 后建条目。若建条目期间因 `AcceptInvalidationMessages` 导致该行已过期，函数丢弃本轮并**重新扫描**，以免返回过期条目。
6. 若扫描无结果：bootstrap 模式直接返回 NULL（此时失效机制尚未建立，负条目无人清理）；正常模式则建立键值正确、其余列为 null 的负条目，并把条目引用计数留为 0，不返回给调用方。
7. 条目作废由 `CatCacheInvalidate` 接收“哈希值”粒度的事件：先把这个 cache 的所有 `CatCList` 标记 dead 或摘除，再在对应桶内把哈希相同的正/负条目 dead 化或摘除，并标记正在构建中的条目。源码明确不按 TID 匹配，因为 `VACUUM FULL` 可能让事件排队后才处理的元组换了 TID；代价是可能因哈希碰撞误伤无关条目。

## 关键取舍

- 源码事实：`InitCatCache` 只分配结构、不打开目录关系，真正装载延迟到第一次查询。架构推论：进程启动只付结构分配成本，冷门 catcache 永不读目录；若在初始化阶段一次性打开并预读全部缓存，启动快路径的 I/O 与 relcache 递归风险都会上升。
- 源码事实：命中按哈希值比较，而失效也按哈希值而非 TID 匹配，源码注释说明这是为了避开 `VACUUM FULL` 后 TID 已变导致失效落空。架构推论：接受哈希碰撞带来的“误失效”，换取失效事件与物理位置解耦；改成按 TID 匹配会精确但可能在重写后失效不到目标，留下脏条目。
- 源码事实：负条目只在非 bootstrap 模式建立，且引用计数保持 0；`SearchCatCacheMiss` 在建条目期间若检测到过期会整轮重扫。架构推论：负缓存把“不存在”这一高频结果也变成 O(1) 命中，代价是必须有可靠失效来清理；重扫逻辑接受偶发重复扫描，以免把一个已经失效的副本返回给调用者。
- 源码事实：每个 `SearchCatCache` 命中都要 `refcount++` 并登记到 `CurrentResourceOwner`，`ReleaseSysCache` 必须配对。架构推论：用资源跟踪器兜住异常路径下的漏释放，代价是每次查找多一次 ResourceOwner 记账；若不做引用计数，失效时就无法安全摘除仍被调用者持有的条目。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/cache/syscache.c` | `InitCatalogCache` | 按枚举建立全部 catcache，并把目录/索引 OID 汇总成可二分查找的集合。 |
| `src/backend/utils/cache/syscache.c` | `SearchSysCache` | 把 syscache ID 解析成 `CatCache`，校验键数后转调通用查找。 |
| `src/backend/utils/cache/syscache.c` | `ReleaseSysCache` | 递减条目引用计数并撤销 ResourceOwner 记账，使条目可被安全回收。 |
| `src/backend/utils/cache/catcache.c` | `InitCatCache` | 分配缓存结构、桶头与键列定义，延迟关系打开与列表桶分配。 |
| `src/backend/utils/cache/catcache.c` | `SearchCatCache` | 哈希定位与桶内比较的命中路径，未命中转交目录扫描与条目建立。 |

## 与主核 / AI 的关系

主核的元数据缓存不采用“每进程一份 catcache + 按目录索引键负缓存”的模型。可借鉴的是把“元数据只读副本 + 引用计数 + 显式失效”三者成组设计；不要把 syscache 命中等同于事务快照可见性，也不要把它描述成跨进程共享缓存。

## 参考

- [PostgreSQL 文档：系统目录](https://www.postgresql.org/docs/current/catalogs.html) —— 说明 syscache 缓存的系统目录表角色。
- [PostgreSQL 官方源码：catcache 实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/cache/catcache.c) —— 支撑桶结构、负条目与条目作废逻辑。
