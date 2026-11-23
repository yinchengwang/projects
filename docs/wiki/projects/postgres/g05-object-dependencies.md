---
project: postgres
feature: object-dependencies
page_type: feature
schema_version: 1
refs:
  - path: src/backend/catalog/dependency.c
    symbols: [performDeletion, findDependentObjects, deleteOneObject]
  - path: src/backend/catalog/pg_depend.c
    symbols: [recordDependencyOn]
---

# 对象依赖与删除

## 一句话职责

`pg_depend` 把数据库对象间“谁引用谁、引用关系是哪一类”持久化为目录行，DROP 时据此算出删除闭包、检查 RESTRICT/CASCADE 许可并按依赖顺序删除对象。

## 机制是什么

依赖记录以两个 `ObjectAddress` 表示：dependent（依赖者）由 `classid/objid/objsubid` 标识，被引用对象由 `refclassid/refobjid/refobjsubid` 标识，`deptype` 表示 NORMAL、AUTO、INTERNAL、EXTENSION、PARTITION 等关系类型。对象可由 class OID 区分目录类别，也可用 subid 表示列等子对象。`recordDependencyOn` 只是把这些字段写入 `pg_depend`，并不在登记时自动执行级联规则；指向系统 pinned 对象的依赖会被省略，因为它永远不应被删除，且省掉大量无必要目录行。

删除时 `performDeletion` 对目标取得 deletion lock，打开一次 `pg_depend` 并将其沿递归过程传递；`findDependentObjects` 同时沿 depender 索引查目标自身是否属于 owner（如 INTERNAL/EXTENSION），沿 referenced 索引找其所有依赖者。它构建 `ObjectAddresses` 删除清单并记录依赖类型/归属标志，循环依赖用递归 stack 防止无限回环，收集出的依赖者排序后先递归访问，确保依赖者先于被依赖对象删除。最终 `reportDependentObjects` 根据 RESTRICT/CASCADE 检查或报告，`deleteOneObject` 才调用对象类型专用删除函数并清理相应 `pg_depend` 行。

本文聚焦依赖行记录与 DROP 递归图算法，不负责对象名字解析、ACL/ownership 授权、缓存失效传播或各类对象具体物理删除实现。

## 怎么实现的

1. `recordDependencyOn(depender, referenced, behavior)` 转到 `recordMultipleDependencies`。bootstrap 模式直接不写 `pg_depend`（目录可能尚未存在且引导对象通常 pinned）；正常情况下以 RowExclusiveLock 打开 `pg_depend`，跳过 pinned referenced 对象，对其余对象先 `dependencyLockAndCheckObject` 确保引用目标在写依赖时未被并发 DROP。
2. 依赖行按批次组装成 virtual tuple：每行写 `refclassid/refobjid/refobjsubid`、`deptype`、`classid/objid/objsubid`。多个引用对象共用一类 behavior 时批量写入，只有确实有非 pinned 依赖才打开索引状态，最后通过目录 multi-insert 保持 `pg_depend` heap 与索引一致；重复依赖不去重，源码认为无害。
3. `performDeletion` 只打开一次 `pg_depend`，以 RowExclusiveLock 传给整个递归树；先 `AcquireDeletionLock` 锁目标对象，再 `findDependentObjects` 构建 `targetObjects`，随后 `reportDependentObjects` 按 RESTRICT/CASCADE 给出错误或级联提示，最后才对清单逐项 `deleteOneObject`。
4. `findDependentObjects` 首先检查当前递归 stack 与已收集清单，避免循环和重复对象； pinned 目标直接报“系统需要，不能 drop”。它先查 `DependDependerIndexId` 找对象本身依赖的 owner：遇到 INTERNAL/EXTENSION 且当前不是从 owner 递归而来，会释放当前对象锁、锁 owner 并重查依赖行是否仍存在，再反向递归到 owner，避免并发删除竞态与锁序死锁。
5. 接着查 `DependReferenceIndexId` 找所有依赖目标的 dependent，对每个候选先取 deletion lock，再用 `systable_recheck_tuple` 确认等待锁期间依赖行仍有效；若依赖者属于 shared catalog，当前路径不支持删除并报错。普通/AUTO/INTERNAL/partition/extension 依赖被翻成递归 flags，先放进数组再按 `ObjectAddress` 排序，保证 DROP CASCADE 报告与删除顺序确定。
6. `performDeletion` 收集完整闭包后先调用报告/RESTRICT 检查。`deleteOneObject` 每个对象先运行 DROP hook，调用 `doDeletion` 按对象 classId 分派到 `index_drop`、`heap_drop_with_catalog`、`RemoveFunctionById`、`RemoveTypeById` 等对应实现，然后通过 `DependDependerIndexId` 删除从该对象发出的依赖行，清 shared dependency、comments、安全标签和初始权限，最后 `CommandCounterIncrement` 使前一步删除对后续清理可见。
7. Concurrent drop 是窄特例：只有索引 drop 当前支持。`deleteOneObject` 在触发会提交当前事务的 `doDeletion` 前关闭 `pg_depend`，回来再打开；因此不能把它泛化成任意对象都可并发删除。

## 关键取舍

- 源码事实：`recordMultipleDependencies` 对 pinned referenced 对象不写 pg_depend；普通对象写入前先取得依赖锁，DROP 则反过来锁对象再读取依赖图。架构推论：省略永不删除对象的边可显著压缩目录，锁则确保“建立引用”与“被删除”不会并发穿越；若所有系统对象边都存储，会增加大量永远不会参与 DROP 规划的行。
- 源码事实：依赖者先收集再排序，然后递归，再把当前对象加入删除清单；`performDeletion` 在真正 doDeletion 前才做 RESTRICT/CASCADE 报告。架构推论：先规划完整闭包避免边走边删造成一半删除后才发现 RESTRICT 冲突；排序也让级联结果稳定。若发现一条边就立即删除，会让错误路径难回滚且输出依赖目录物理顺序。
- 源码事实：依赖扫描遇到 INTERNAL/EXTENSION owner 时释放现有对象锁、取得 owner 锁并重验依赖行后再递归；普通依赖者也在等待 deletion lock 后重验依赖 tuple。架构推论：这是以重复一次索引查找换取并发 DDL 下图仍有效；若锁等待后沿用扫描前的旧依赖行，可能递归到已删除或不再拥有它的对象。
- 源码事实：循环通过 stack 与已收集对象集合处理，依赖者排序后递归；`deleteOneObject` 按对象 classId 分派各自删除实现，且 `pg_depend` 仅在对象主体删除后清除 outgoing rows。架构推论：图规则统一、实际销毁保持对象类型专属，能避免依赖模块承载所有物理删除逻辑；若由 dependency.c 直接清每类目录，将强耦合到所有对象目录格式。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/catalog/pg_depend.c` | `recordDependencyOn` | 将 dependent 与 referenced 地址及 deptype 交给批量写入实现，建立持久依赖边。 |
| `src/backend/catalog/dependency.c` | `performDeletion` | 单对象 DROP 外层编排：锁目标、打开 pg_depend、规划、报告，再执行删除闭包。 |
| `src/backend/catalog/dependency.c` | `findDependentObjects` | 双向解析 owner/依赖者，处理递归环、并发锁重验与依赖顺序。 |
| `src/backend/catalog/dependency.c` | `deleteOneObject` | 对计划对象调用类型专属删除，并清理相关依赖、共享依赖、注释与安全标签。 |

## 与主核 / AI 的关系

主核元数据与对象生命周期不采用 PostgreSQL `pg_depend` 对象地址图和 `DROP ... CASCADE` 通用递归器的直接同构实现。可借鉴把引用边类型化、把删除规划与物理删除分离；不能把 PostgreSQL 的 deptype 或级联行为推断为主核已实现能力。

## 参考

- [PostgreSQL 文档：依赖跟踪](https://www.postgresql.org/docs/current/ddl-depend.html) —— 说明对象间依赖及 DROP 时的 RESTRICT/CASCADE 规则。
- [PostgreSQL 文档：系统目录 pg_depend](https://www.postgresql.org/docs/current/catalog-pg-depend.html) —— 说明依赖边的目录字段与类型。
- [PostgreSQL 官方源码：dependency deletion](https://github.com/postgres/postgres/blob/master/src/backend/catalog/dependency.c) —— 支撑递归查找与对象删除顺序。
