---
project: postgres
feature: relcache
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/cache/relcache.c
    symbols: [RelationBuildDesc, RelationIdGetRelation, RelationClearRelation]
---

# 关系缓存生命周期

## 一句话职责

relcache 为每个后端保存关系描述符（`Relation`），从系统目录把 `pg_class`、`pg_attribute`、`pg_index`、重写规则、约束等元数据组装成可直接使用的结构，并按引用计数决定何时原地重建或整体销毁。

## 机制是什么

一个 relcache 条目不只是 `pg_class` 行：`RelationBuildDesc` 先按 relid 扫到 `pg_class` 行，复制成 `rd_rel`，再据 relkind 走不同分支填充 `rd_att`（TupleDesc）、索引访问信息、reloptions、规则与触发器、行级安全、分区键、锁信息与物理定位（`rd_locator`），并初始化物理文件句柄为空。因此它是“关系的一次完整元数据快照”，热路径上打开关系只需查哈希表并加引用计数。

条目生命周期由 `rd_refcnt` 与有效性位 `rd_isvalid` 共同表达：引用计数为零且已失效的条目可以整体摘除；被引用中的条目在收到变更通知时必须重建，并且必须原地换内容而不是换地址，因为持有者手里是裸指针。`RelationClearRelation` 只在 refcnt 为零、且该关系不是本事务内新建、且未标记 dropped 时才被允许执行。本页讲条目的内容与装载、引用计数、以及收到 flush 后如何重建/销毁；“哪些目录元组的变化会产生什么失效消息、消息如何在本进程分派执行”属于缓存失效语义页，本页不重复定义消息语义与传输。

## 怎么实现的

1. `RelationIdGetRelation` 先查 `RelationIdCache`。命中且 `rd_droppedSubid` 有效则返回 NULL（关系已在本事务内被删除）；否则先 `RelationIncrementReferenceCount` 再按需重建。未命中则调用 `RelationBuildDesc(relid, true)` 建立并插入哈希，成功后再加一次引用。调用方必须持有至少 AccessShareLock，且最终通过 `RelationClose` 递减计数。
2. `RelationBuildDesc` 在 `in_progress_list` 里登记本次构建（`in_progress_offset`），随后 `ScanPgRelation` 找到 `pg_class` 行；`AllocateRelationDesc` 复制 `Form_pg_class` 到 `rd_rel`，设置 `rd_id`、把 `rd_refcnt`、`rd_isnailed`、`rd_createSubid`、`rd_newRelfilelocatorSubid`、`rd_firstRelfilelocatorSubid`、`rd_droppedSubid` 全部置初值，并按 `relpersistence` 决定 `rd_backend`/`rd_islocaltemp`。
3. 接着 `RelationBuildTupleDesc` 填 `rd_att`；外键、TOAST chunk 类型、分区键/分区描述、partcheck 均延迟到需要时才装载（先置 NIL/NULL 与失效标记）。索引类关系走 `RelationInitIndexAccessInfo`（含 `rd_index`、`rd_indclass`、support 函数与 `rd_indexcxt`），表类关系走 `RelationInitTableAccessMethod` 设置 `rd_tableam`。随后解析 reloptions，并按 `relhasrules`/`relhastriggers`/`relrowsecurity` 装载规则、触发器与行安全描述，最后初始化锁 tag 与 `RelFileLocator`。
4. 构建末尾专门检查“构建期间是否收到针对本 relid 的失效”：`in_progress_list[offset].invalidated` 为真就 `RelationDestroyRelation` 后 `goto retry` 重建。该检查之后到函数结束的代码被约定为“不得读系统目录”，这段区间对 `insertIt=false` 必须无失效处理，否则换内容会与重入失效交错。
5. `insertIt` 为真时 `RelationCacheInsert(relation, true)`：允许覆盖已存在的同 OID 条目，因为装载系统目录（如 `pg_class` 自身索引）可能在构建过程中递归创建同一 relcache 条目；覆盖时若旧条目 refcnt 非零会记 WARNING 并泄漏旧条目。最后置 `rd_isvalid = true`。
6. 收到针对某关系的 flush 请求时（`RelationCacheInvalidateEntry`）走 `RelationFlushRelation`：本事务内新建或刚换过 relfilenumber 的关系只能重建不能丢弃（否则会丢掉“新”状态）；其他关系 refcnt 为零就 `RelationClearRelation` 整体摘除，非事务态下无法读目录则只标 invalid，nailed 且 refcnt==1 的条目只标 invalid，其余走 `RelationRebuildRelation`。
7. `RelationRebuildRelation` 为被引用条目构造一个临时新条目（`RelationBuildDesc(relid, false)`），比较 `rd_att`、规则、行安全描述是否等价，再整体 swap 字段，尽力保留 `rd_att`、`rd_rel`、规则、分区键等子结构地址不变；失败路径保留旧条目且仍为 invalid，下次访问重试。`RelationInvalidateRelation` 会先关掉 smgr 句柄并释放 `rd_amcache`。
8. `RelationCacheInvalidate`（SI 队列溢出等场景）分两阶段：第一遍摘除可删条目并就地更新映射关系的 `rd_locator`，第二遍按“先 `pg_class`、再其索引、再其他 nailed 关系、最后其余”的顺序重建，保证用系统目录重建其他关系时目录已是最新。

## 关键取舍

- 源码事实：重建被引用中的条目时采用“另建一份再整体 swap、并尽量保留 rd_att/rd_rel/规则/分区键子结构”的策略，而非销毁重建指针。架构推论：调用者持有裸 `Relation` 指针，换地址会立刻失效；因此宁可付出一次额外构建与比较成本。若允许换地址，就必须把所有持有者改成间接句柄，改动面远大于一次 swap。
- 源码事实：`RelationBuildDesc` 用 `in_progress_list` 标记并支持 `retry`，`RelationCacheInvalidate` 用两阶段遍历并明确说明 `hash_seq_search` 只能容忍删除当前元素。架构推论：装载期间可能因反 toast、目录访问而重入失效处理，必须让构建可被作废重启、让遍历不破坏后续元素；不做这些保护会得到随机脏条目或崩溃。
- 源码事实：`rd_createSubid`/`rd_firstRelfilelocatorSubid`/`rd_droppedSubid` 让本事务内新建、换文件号、已删除的关系走“只标记或只重建、不摘除”分支。架构推论：事务回滚需要这些状态存活，提前摘除会丢失新关系或已删除标记，导致回滚语义错误；代价是多几个子事务 ID 字段与多处判断。
- 源码事实：外键、分区描述、partcheck 等延迟装载并带 `rd_*valid` 标记。架构推论：把冷门元数据的目录读推迟到实际需要，减少打开常见表时的目录 I/O 与内存；若一次性全装，热路径装载成本随关系复杂度上升。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/cache/relcache.c` | `RelationBuildDesc` | 从 `pg_class` 行出发装配完整 `Relation`，含 `rd_att`、AM 信息、reloptions、规则等，并处理构建期失效重试。 |
| `src/backend/utils/cache/relcache.c` | `RelationIdGetRelation` | 缓存命中/未命中的统一入口：命中校验已删除标记并加引用，未命中触发构建。 |
| `src/backend/utils/cache/relcache.c` | `RelationClearRelation` | 在引用为零、非本事务新建且未 dropped 的情况下，先失效再摘哈希并释放存储。 |

## 与主核 / AI 的关系

主核没有“每后端一份关系描述符 + 裸指针 + 引用计数”的 relcache 同构物。可借鉴的是把元数据快照的构建期失效重试、被引用条目原地重建这两点作为并发缓存设计模板；不要把 relcache 说成共享缓存，也不要把它等同于 syscache。

## 参考

- [PostgreSQL 文档：系统目录](https://www.postgresql.org/docs/current/catalogs.html) —— 说明 relcache 所读取的 `pg_class`、`pg_attribute` 等目录来源。
- [PostgreSQL 官方源码：relcache 实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/cache/relcache.c) —— 支撑条目构建、引用计数与重建策略。
- [PostgreSQL 文档：开发者选项](https://www.postgresql.org/docs/current/runtime-config-developer.html) —— 说明调试期强制丢弃缓存的开发选项。
