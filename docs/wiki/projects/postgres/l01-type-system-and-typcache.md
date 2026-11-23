---
project: postgres
feature: type-system-and-typcache
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/cache/typcache.c
    symbols: [lookup_type_cache, lookup_rowtype_tupdesc]
  - path: src/backend/utils/cache/lsyscache.c
    symbols: [getTypeInputInfo, getTypeOutputInfo]
---

# 类型系统与 typcache

## 一句话职责

类型系统以类型 OID 为稳定身份，将存储表示、文本 I/O、复合行描述和操作符支持函数连接为可查询的元数据契约。

## 机制是什么

`pg_type` 保存类型定义的基础信息：长度、按值/按引用传递、对齐、存储策略、类型类别、元素/数组关联、复合类型 relation OID、输入输出函数等。`typcache.c` 把被频繁使用的类型元数据及按需解析的 btree/hash 操作符族信息放入 `TypeCacheEntry`；`lsyscache.c` 则从系统缓存取出 I/O 函数 OID 与 I/O 参数，供类型输入输出调用方使用。

typcache 不负责执行输入字符串解析或输出文本格式化，也不定义比较函数本身的语义；它缓存或解析元数据，具体类型函数实现由类型模块负责。复合类型的 `TupleDesc` 具有引用生命周期，调用者必须 release；缓存失效回调会依据系统目录变化清除或重置派生信息，避免继续使用陈旧的类型/操作符契约。

## 怎么实现的

1. `lookup_type_cache(type_id, flags)` 首次使用时建立类型缓存 hash、复合类型 relid 映射 hash，并注册 relcache、TYPEOID、CLAOID、CONSTRAINTOID invalidation callbacks。
2. 若 TypeCacheEntry 不存在，先通过 `SearchSysCache1(TYPEOID, type_id)` 验证 `pg_type` 行及 `typisdefined`，再建立缓存项，复制 typlen、typbyval、typalign、typstorage、typtype、typrelid、typelem、typarray、typcollation 等基础字段。
3. 后续按 `flags` 惰性填充所需信息：如 btree/hash opclass、等值/大小比较操作符、比较/哈希函数及其 `FmgrInfo`。缓存项带检查标志，避免重复查找；目录失效后相关标志和派生项可被重建。
4. 对复合类型，`lookup_rowtype_tupdesc(type_id, typmod)` 查找已知 row descriptor 并 `PinTupleDesc`；引用被记录到 `CurrentResourceOwner`，调用者用完必须 `ReleaseTupleDesc`。动态 `record` 类型依 typmod 与记录结构匹配缓存描述符。
5. `getTypeInputInfo` / `getTypeOutputInfo` 分别读取类型目录行，检查类型已定义且 input/output 函数 OID 有效，返回函数 OID 及 typioparam 或 varlena 标志；随后调用者才经 fmgr 调用类型 I/O 函数。
6. 若类型 OID 不存在、仍是 shell type、缺少必要 I/O 函数或复合类型 descriptor 未注册，相关入口给出错误或按 noerror 变体返回 NULL；目录缓存引用在正常路径释放，row descriptor 则由资源所有者与显式 release 生命周期管理。

## 关键取舍

- **源码事实：**TypeCacheEntry 按 flags 惰性加载 opclass、比较/哈希过程及 FmgrInfo，并注册系统缓存失效回调。**架构推论：**只为调用者真正需要的能力付出目录查找与初始化成本，代价是 entry 有分阶段有效性标记和失效逻辑；若一次缓存所有可能信息，冷类型访问会做大量无用工作，且目录变化后需更复杂地统一重建。
- **源码事实：**类型输入输出函数 OID 由 `getTypeInputInfo` / `getTypeOutputInfo` 提取，执行时由调用点调用；元数据层对未定义类型或缺失函数明确报错。**架构推论：**将类型元数据和解析/格式化算法分离，让自定义类型可注册自己的 I/O 实现；若把通用解析器写死在类型系统里，每种类型格式演进都需要改核心分派逻辑。
- **源码事实：**`lookup_rowtype_tupdesc` 对返回描述符 pin 并由 `CurrentResourceOwner` 跟踪，要求调用者 release。**架构推论：**缓存 descriptor 可跨调用复用，同时通过引用计数保护使用期间生命周期；若返回未受管理的裸指针，缓存重建/资源清理时可能造成悬垂访问。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/cache/typcache.c` | `lookup_type_cache` | 按类型 OID 创建/更新缓存项，并依请求惰性解析操作符与函数信息。 |
| `src/backend/utils/cache/typcache.c` | `lookup_rowtype_tupdesc` | 获取并 pin 复合类型 TupleDesc，接入资源所有者生命周期。 |
| `src/backend/utils/cache/lsyscache.c` | `getTypeInputInfo` | 从 `pg_type` 读取输入函数 OID 与 I/O 参数并验证定义。 |
| `src/backend/utils/cache/lsyscache.c` | `getTypeOutputInfo` | 读取输出函数 OID 与 varlena 属性并验证可输出性。 |

## 参考

- [PostgreSQL 文档：系统目录 pg_type](https://www.postgresql.org/docs/current/catalog-pg-type.html) —— 说明类型元数据字段与类型输入输出函数登记。
- [PostgreSQL 官方源码：typcache](https://github.com/postgres/postgres/blob/master/src/backend/utils/cache/typcache.c) —— 对照类型缓存、目录失效与 row descriptor 生命周期。

## 与主核 / AI 的关系

暂无直接对应。主核可以参照将类型元信息、I/O 函数和比较契约分离的方式；类型缓存失效与 TupleDesc 引用规则需要根据主核自己的目录和资源管理协议实现。
