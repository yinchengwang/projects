---
project: redis
feature: hash-type
page_type: feature
schema_version: 1
refs:
  - path: src/t_hash.c
    symbols: [hashTypeSet, hashTypeGetValue, hashTypeDelete, hashTypeLength, hashTypeTryConversion, hashTypeInitIterator]
---

# 哈希类型

## 一句话职责

`src/t_hash.c` 负责哈希类型的字段级语义：写入字段、取值、删字段、算长度、迭代字段，并在字段变多或值变大时把紧凑编码升级为通用编码。

## 机制是什么

哈希类型是「字段名 → 值」的映射。命令层对外只有一套语义（HSET/HGET/HDEL/HLEN/HSCAN/HGETALL），内部却有多套表示：

- **listpack**：字段与值交替排在一个紧凑数组里，适合字段少、值短的哈希。
- **listpack_ex**：在 listpack 的「字段、值」二元组后面多挂一个**字段级过期时间**槽位，变成三元组。
- **HT**：真正的哈希表（dict），字段多或值长时使用。
- **TMPL_LP / TMPL_ARRAY**：模板编码——多个键共享同一份字段名副本，值各存一份。

哈希类型还支持**字段级 TTL**（HEXPIRE/HPERSIST 族），这是它比普通 KV 更复杂的地方：过期时间挂在字段上而不是键上，因此需要一套独立于键空间过期机制的元数据（listpack_ex 的第三个槽位、HT 的 `ebuckets`）。

底层 dict 的实现属于 E 组（e01 页），键空间的读写与删除属于 C 组（c01/c02 页），本页都不重复。

## 怎么实现的

**转换判据：`hashTypeTryConversion`。** 它是「要不要换编码」的集中决策点，参数是 `(db, o, argv, start, end)`——`argv` 是即将写入的字段/值数组，`start`/`end` 是其中的范围。

第一步先看当前编码：只有 `OBJ_ENCODING_LISTPACK`、`OBJ_ENCODING_LISTPACK_EX`、`OBJ_ENCODING_TMPL_LP` 三种需要检查，其余（HT、TMPL_ARRAY）**直接返回**。TMPL_LP 的目标编码是 TMPL_ARRAY，其余的目标是 HT。

第二步查**字段数**：`new_fields = (end - start + 1) / 2`——参数是字段/值成对出现的，所以除以 2。若 `new_fields > server.hash_max_listpack_entries` 就 `hashTypeConvert(db, o, OBJ_ENCODING_HT)` 并立刻 `dictExpand(o->ptr, new_fields)` 预扩容哈希表。

第三步查**值长度**：逐个参数遍历，**只统计字符串编码对象**（源码注释：只有字符串编码对象的长度能 O(1) 拿到），若某个长度超过 `server.hash_max_listpack_value` 就转目标编码；否则把长度累加到 `sum`。

第四步查累计字节：`lpSafeToAdd(lp, sum)` 不成立就转目标编码（listpack_ex 编码要先取 `((listpackEx*)o->ptr)->lp` 拿到底层 listpack）。

两个阈值来自配置项，源码默认值在 `src/config.c`：`hash-max-listpack-entries` 默认 **512**，`hash-max-listpack-value` 默认 **64**。

**写入：`hashTypeSet`。** 按编码分派，标志位用 `HASH_SET_*` 一族表达：

- **listpack**：`lpFind` 以步长 1 找字段。找到就 `lpReplace` 替换值并置 `update = 1`；没找到就 `lpBatchAppend` 一次追加 field/value 两项。追加之后若 `hashTypeLength(o, 0) > server.hash_max_listpack_entries`，就地 `hashTypeConvert(db, o, OBJ_ENCODING_HT)`。
- **listpack_ex**：找字段的步长是 2（因为多了 TTL 槽位）。替换时若旧字段带 TTL 且调用方没给 `HASH_SET_KEEP_TTL`，就调 `listpackExUpdateExpiry(o, field, fptr, vptr, HASH_LP_NO_TTL)` 把 TTL 清掉（`HASH_LP_NO_TTL` 在 `src/t_hash.c` 里定义为 0）；新字段走 `listpackExAddNew`。
- **HT**：`dictFindLink` 一次拿到桶与链接。字段不存在 → `hEntryCreate` 后 `dictSetKeyAtLink(ht, hNewEntry, &bucket, 1)`；字段已存在 → 先 `hfieldPersist(o, hOldEntry)` 把旧 TTL 从哈希私有的 ebuckets 里摘掉（源码注释给的理由：必须先摘，因为接下来的 `hEntryUpdate` 可能重分配并释放这个 entry），再 `hEntryUpdate`；若 entry 被重分配过则用 `dictSetKeyAtLink` 把 dict 的 key 换成新 entry；只有调用方要求 `HASH_SET_KEEP_TTL` 时才把旧 TTL 重新 `ebAdd` 回 ebuckets。
- **模板编码**：用 `hashTemplateFieldIndex` 查字段在模板里的下标。已存在 → `hashTypeTmplWriteValueAt` 原地改值；不存在 → `hashTypeTmplAddFields` 把它并进模板。

函数收尾先按 `HASH_SET_TAKE_FIELD`/`HASH_SET_TAKE_VALUE` 释放没被接管的 SDS，然后做一次**自动转模板**：若没设 `HASH_SET_NO_TEMPLATE_CONVERT`（该标志用于「一次加 N 个字段」的场景，避免每加一个字段就重建一次模板）且 `server.hash_min_template_entries > 0`，就调 `hashTypeTryConvertToTemplate(o, server.hash_min_template_entries, server.hash_max_template_entries, NULL)`。`hash-min-template-entries` 默认 **0**，即默认关闭。

**取值：`hashTypeGetValue`。** 返回值是 `GetFieldRes` 枚举：`GETF_OK`、`GETF_NOT_FOUND`、`GETF_EXPIRED`、`GETF_EXPIRED_HASH`。按编码分派取到字段值后，还有一段**过期处理**是本页最需要点名的地方：

- 若 `server.allow_access_expired` 为真，或字段的过期时间还没到，或带了 `HFE_LAZY_ACCESS_EXPIRED` 标志 → 直接返回 `GETF_OK`。
- 副本上（`server.masterhost` 为真）：带 `CLIENT_MASTER` 标志的客户端（数据导入用的假主客户端）视为有效；普通用户客户端则**当作已过期但不删除**，返回 `GETF_EXPIRED`——源码注释说明副本不能按自己的时钟删字段。
- 载入中（`server.loading`）、或带 `HFE_LAZY_AVOID_FIELD_DEL`、或处于 `PAUSE_ACTION_EXPIRE` 暂停状态 → 返回 `GETF_EXPIRED` 且不删。
- 其余情况走**惰性删除**：`hashTypeDelete(o, field)` 删字段、`propagateHashFieldDeletion(db, key, field, ...)` 把删除传播出去、`server.stat_expired_subkeys++`，并更新键大小直方图。如果这个字段是哈希里的最后一个字段，哈希本身也会被删除。

**删除：`hashTypeDelete`。** listpack 用 `lpDeleteRangeWithEntry(zl, &fptr, 2)` 一次删掉字段与值两项；listpack_ex 用 `lpDeleteRangeWithEntry(lpt->lp, &fptr, 3)` 连 TTL 槽位一起删三项；HT 走 `dictDelete`（释放交给 `dictEntryDestructor`）；模板编码走 `hashTypeTmplDropFields`。返回 1 表示删掉了，0 表示字段不存在。

**长度：`hashTypeLength(o, subtractExpiredFields)`。** 各编码的元素计数方式不同：listpack 是 `lpLength / 2`；listpack_ex 是 `lpLength / 3`（三元组）；HT 是 `dictSize(d)`；TMPL_LP 是 `lpLength - 1`（源码注释：首项是模板 ID）；TMPL_ARRAY 是 `hta->field_count`。

第二个参数控制要不要**扣掉已过期的字段**。listpack_ex 侧只有在 `lpt->meta.trash == 0` 时才调 `listpackExExpireDryRun(o)` 做一次试算；HT 侧只有在字典已注册进全局 HFE 数据结构（`isDictWithMetaHFE(d)`）且 `meta->expireMeta.trash == 0` 时才用 `ebExpireDryRun` 试算。函数注释直接写明 `subtractExpiredFields=1` 在字段很多时「might be pricy」。

**迭代：`hashTypeInitIterator` / `hashTypeNext`。** 初始化按编码分派：listpack 系把 `fptr`/`vptr`（listpack_ex 还有 `tptr`）置空表示「尚未开始」；HT 用 `dictInitIterator`；模板编码把 `field_index` 置为 -1。`hashTypeNext` 推进游标并返回 `C_OK`/`C_ERR`：listpack 系成对前进（`fptr` 走一步、`vptr` 跟着走），listpack_ex 还要多跳一个 TTL 槽位；HT 走 `dictNext` 并在 `skipExpiredFields` 为真时跳过过期条目；模板编码只递增 `field_index`。函数开头统一处理：`server.allow_access_expired` 为真时把 `skipExpiredFields` 强制置 0。

## 关键取舍

**源码事实：** 转换用**双阈值**——字段数（`hash-max-listpack-entries`，默认 512）与单值长度（`hash-max-listpack-value`，默认 64），而不是单一阈值。**源码事实：** 长度预估只统计字符串编码对象，整数编码的对象被跳过。**源码事实：** HT 路径上「先摘 TTL 再改值」的顺序是注释明确要求的，因为 entry 可能被重分配释放。**源码事实：** 过期字段在**读路径**上被惰性删除并传播删除；副本上不删只标记过期。**源码事实：** `hashTypeLength` 只有在没有待回收垃圾（`trash == 0`）时才做精确的过期扣减，否则返回未扣减的计数。

**架构推论：** 双阈值对应 listpack 的两种不同开销来源——元素个数带来的每项头开销，和长字符串带来的字节膨胀；只用一个阈值会顾此失彼（字段多但值短的哈希用长度阈值卡不住，字段少但值超长的哈希用个数阈值卡不住）。**架构推论：** 惰性删除把「没人读的过期字段」也清掉，不完全依赖后台扫描；代价是读命令变成了有副作用的写命令——要 `server.dirty++`、要传播删除、要发通知，读延迟与复制流量都会上升。**架构推论：** 副本按自己的时钟删字段会造成主从数据分叉，所以副本只标记不删、让主库来删，这是「单一过期权威」原则的落地。**架构推论：** `hashTypeLength` 的「有垃圾就不精确算」是拿精度换开销——它保证计数的 O(1) 路径不被破坏，但代价是同一时刻不同调用方拿到的长度可能不一致。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/t_hash.c` | `hashTypeSet` | 字段写入：按编码分派替换/追加/建 entry，处理字段级 TTL 的继承与清除，末尾可自动转模板 |
| `src/t_hash.c` | `hashTypeGetValue` | 字段取值：按编码分派查找，并处理字段级过期的「可访问/惰性删除/只标记」三条路径 |
| `src/t_hash.c` | `hashTypeDelete` | 字段删除：listpack 删两项、listpack_ex 删三项、HT 走 dictDelete、模板走 dropFields |
| `src/t_hash.c` | `hashTypeLength` | 字段计数：各编码的计数方式不同，并可选择是否扣减已过期字段 |
| `src/t_hash.c` | `hashTypeTryConversion` | 紧凑编码是否需要升级的集中判据：字段数、值长度、累计字节三条 |
| `src/t_hash.c` | `hashTypeInitIterator` | 字段迭代器的初始化：按编码准备各自的游标状态 |

## 参考

- [Redis 源码：t_hash.c](https://github.com/redis/redis/blob/unstable/src/t_hash.c)
- [Redis 文档：哈希数据类型](https://redis.io/docs/latest/develop/data-types/hashes/)
- [Redis 命令：HSET](https://redis.io/docs/latest/commands/hset/)
- [Redis 命令：HEXPIRE](https://redis.io/docs/latest/commands/hexpire/)

## 与主核 / AI 的关系

「字段级 TTL 与键级 TTL 是两套元数据」是 Redis 哈希里最容易踩坑的设计，主核若要做文档内的字段级过期（向量库里的 chunk 元数据、推理结果里的分项置信度）必须照抄这个分层：子键的过期集合要与主键的过期集合分开，否则主键被删时子键的回收就断了。

Redis 这里的惰性删除策略对主核也有参考价值：读命令顺带做回收能覆盖「没人再访问的过期数据」，但主核是多写者引擎，读路径变成写路径会引入行锁冲突与可见性延迟——主核更适合把这种回收交给后台清理线程，只在读路径上做「标记不可见」。

AI 侧常用哈希存「文档 id → {embedding, metadata}」。这个用法把 embedding 的大字节和 metadata 的小字段混在同一个阈值体系里：`hash-max-listpack-value` 默认 64 字节意味着只要 embedding 超过 64 字节，哈希就会立刻从紧凑编码升级成哈希表——也就是每个字段都要付一份 dict entry 与 SDS 的开销。若要控制内存，必须显式把这个阈值调到 embedding 的实际字节数以上，否则紧凑编码形同虚设。
