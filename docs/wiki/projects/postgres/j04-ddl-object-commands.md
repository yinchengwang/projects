---
project: postgres
feature: ddl-object-commands
page_type: feature
schema_version: 1
refs:
  - path: src/backend/commands/tablecmds.c
    symbols: [DefineRelation, ATController]
  - path: src/backend/commands/indexcmds.c
    symbols: [DefineIndex]
  - path: src/backend/commands/typecmds.c
    symbols: [DefineType]
---

# 对象 DDL 命令域

## 一句话职责

对象 DDL 命令域把「创建一个数据库对象」落到系统目录上：分配 OID、写入 `pg_class`/`pg_attribute`/`pg_type` 等目录元组、登记依赖与权限，并在语义要求时重写底层存储。

## 机制是什么

这一域覆盖的是**目录状态变更**，不是 SQL 语法解析，也不是执行器计划执行：解析与语义分析在解析阶段完成，语句如何被分派到本域由 utility 分派层负责（见 j03）。本域内部按对象种类分成若干族：表/视图/外表/分区表走 `DefineRelation`，索引走 `DefineIndex`，类型（base type、domain、enum、range、composite）走 `DefineType`；函数、序列各有自己的命令文件，但共享同一套约定——建目录项、记依赖、赋权限、推进命令计数器、返回 `ObjectAddress`。

共同的形状是「先在事务内拿到命名空间与权限，再一次性把对象写进目录」。「一次性」体现在 `DefineRelation` 用单个 `heap_create_with_catalog` 调用同时落 `pg_class`、`pg_attribute`、复合类型的 `pg_type`；「事务内」意味着所有目录变更都随当前事务回滚，跨事务可见性靠命令计数器推进。

本域**不负责**：语句的语法与重写、权限判定算法本身（ACL 判定在 G 组）、依赖图遍历与级联删除（目录依赖页）、索引访问方法内部的算法实现（B-tree/GIN 等在 D 组）、以及 WAL 屏障语义。它向这些机制提供的是「对象已存在且已登记依赖」这一前提。

## 怎么实现的

1. `DefineRelation` 先做参数一致性检查：`ON COMMIT` 只允许临时表；带分区规格时 relkind 被改成 `RELKIND_PARTITIONED_TABLE`；分区表不能是 unlogged。接着用 `RangeVarGetAndCheckCreationNamespace` 解析目标命名空间、校验 `CREATE` 权限并把命名空间锁住；在 security-restricted 操作中禁止建临时表，避免不可信临时表出现在 `pg_temp` 的搜索路径前面。
2. 父表加锁模式按语义分流：普通继承用 `ShareUpdateExclusiveLock`，源码注释说明原因是两个后端同时给一个尚无子表的父表加子表时都会去改父表的 `relhassubclass`，会撞上 "tuple concurrently updated"，该锁同时挡住并发的 `ANALYZE`；分区 attach 则用 `AccessExclusiveLock`，因为父表的 partition descriptor 会被改。
3. 表空间先按显式指定 → 分区继承父表 → 默认值的顺序解析，非默认表空间要过 `ACL_CREATE`，且任何用户关系都不允许放进 `pg_global`。
4. 核心落盘是 `heap_create_with_catalog`：把 `TupleDesc`、cooked defaults、继承来的 CHECK 约束、relkind、persistence、oncommit、reloptions、ownerId 一次性交给它，由它写 `pg_class`、`pg_attribute` 以及复合类型的 `pg_type`。随后立刻 `CommandCounterIncrement()`，让新元组对自己可见到可以 `relation_open` 的程度。
5. `DefineRelation` 接着 `relation_open(relationId, AccessExclusiveLock)`。源码注释明确说这不是为了挡住别的后端（新关系提交前别人看不见），而是为了让锁管理器不报死锁风险。列默认值与生成表达式必须推迟到这一步才做 parse 变换，因为 `transformExpr` 需要一个已存在的 relation 才能构造命名空间上下文；分区边界同理要先 `transformPartitionSpec` 再 `ComputePartitionAttrs`/`StorePartitionKey`。
6. 继承关系由 `StoreCatalogInheritance` 写 `pg_inherits` 并对父子登记 `DEPENDENCY_NORMAL` 依赖；建分区时还要克隆父表的索引、行级触发器与外键（`CloneRowTriggersToPartition`、`CloneForeignKeyConstraints`），源码说明顺序原因——`DefineIndex` 需要父表的分区键已经落库。
7. `DefineRelation` 返回 `ObjectAddressSet(address, RelationRelationId, relationId)`；`ObjectAddress` 只有 `classId`/`objectId`/`objectSubId` 三个字段，正是 event trigger、依赖登记与权限改写共用的对象标识形式。
8. `ATController` 是 ALTER TABLE 的三段骨架：Phase 1 用 `ATPrepCmd` 做权限、relkind 检查与向子表的递归，生成工作队列 `wqueue`（元素是 `AlteredTableInfo`，含 `relid`/`relkind`/`oldDesc`/`subcmds[AT_NUM_PASSES]`/`rewrite`/`chgPersistence` 等字段）；Phase 1 结束时 `relation_close(rel, NoLock)` 关闭关系但保留锁到提交；Phase 2 的 `ATRewriteCatalogs` 按 `AlterTablePass` 分趟逐表执行 `ATExecCmd`；Phase 3 的 `ATRewriteTables` 在 `tab->rewrite > 0` 时建临时关系拷数据。
9. `DefineIndex` 的入口动作是建立 GUC 嵌套层并 `RestrictSearchPath()`，索引函数被限制在受控搜索路径下求值；`index_create` 落 `pg_class`/`pg_index` 与对列类型的依赖后，用 `AtEOXact_GUC(false, root_save_nestlevel)` 回滚索引函数造成的 GUC 变更。并发建索引被拆成多事务：建出 `indisvalid=false` 的索引并提交、`WaitForLockers` 等到没有事务还持有旧索引列表、`index_concurrently_build` 用新快照灌数据、`validate_index` 补齐并发期间漏掉的元组，最后才把索引标为有效。
10. `DefineType` 先要求 base type 的创建者是 superuser，再用 `QualifiedNameGetCreationNamespace` 解析名字；若同名类型已存在且是自动生成的数组类型，先把它改名腾地方，否则报重复。参数列表为空的 `CREATE TYPE` 只建一个 shell type（`TypeShellMake`）并立刻返回，因为 I/O 函数必须先于完整类型存在；带参数时要求 shell type 已存在。最后由 `TypeCreate` 完成真正的类型与数组类型创建，`TypeCreate` 内部负责 `GenerateTypeDependencies` 登记类型依赖。

## 关键取舍

- 源码事实：`ATController` 把 ALTER TABLE 拆成准备、目录改写、整表重写三段，`ATRewriteCatalogs` 又按 pass 分趟处理多张表，注释写明原因是「ALTER TYPE 作用在某张表外键所引用的主键上时，必须把重建该外键的工作派发到另一张表，而工作只能传播到后面的 pass」。架构推论：把跨表传播编码进 pass 序号，换取子命令执行顺序与依赖一致；替代方案（一次性顺序执行所有子命令）会让外键在主键列类型已经改变之后才被重建，约束表达式与列类型失配。
- 源码事实：`ATPrepCmd` 里对每张表都执行 `cmd = copyObject(cmd)`，注释解释为不同子表需要不同的 parse 变换（例如同一列在不同子表里列号不同）。架构推论：用一次树拷贝换取每张子表的命令树可独立改写；不拷贝的话，同一棵命令树在递归过程中被就地改写，会让兄弟子表拿到错误的列号。
- 源码事实：`DefineRelation` 在建完目录后仍用 `AccessExclusiveLock` 打开新关系，注释说这不是必需、只是为了「不让锁管理器抱怨死锁风险」。架构推论：放弃省掉一次加锁，换取锁管理器不做保守的顺序推断；这属于纯粹为工具链让路的成本。
- 源码事实：`DefineIndex` 对临时表强制关闭并发模式，判断条件是 `get_rel_persistence(tableId) != RELPERSISTENCE_TEMP`，注释给出的理由是别的后端访问不到临时关系，加更强的锁没有坏处，且非并发 DROP 更高效。架构推论：这是一条显式的退化路径，caller 就是 `DefineIndex` 自己（在 `src/backend/commands/indexcmds.c` 中调用 `get_rel_persistence`）；替代方案（对临时表也走三阶段并发流程）会白白承担两次 `WaitForLockers` 与多次事务提交。
- 源码事实：`DefineType` 遇到无法识别的类型属性时只发 `WARNING` 而不是报错，注释写明是「出于历史向后兼容」。架构推论：放弃严格拒绝未知属性，换取旧脚本与旧 dump 的可恢复性；替代方案（升级为 ERROR）会让包含已废弃属性的历史备份无法恢复。
- 源码事实：`DefineIndex` 用 `NewGUCNestLevel()` + `AtEOXact_GUC(false, root_save_nestlevel)` 把索引函数造成的 GUC 变更整段回滚，注释强调这对 `search_path` 这类影响行为的 GUC 是必需的。架构推论：放弃「索引表达式函数可以改变会话配置」，换取表达式索引求值环境的确定性；不隔离的话，同一会话里先后创建的两个表达式索引可能用不同的搜索路径解析同名对象。
- 源码事实：`DefineType` 对 base type 要求 superuser，注释给出的理由是错误的类型定义有太多破坏系统的方式（表示参数与 C 代码预期不符），且实际上创建 I/O 函数本身也需要 superuser 权限。架构推论：把风险判断前置成权限门槛，代价是普通用户无法定义 base type。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/commands/tablecmds.c` | `DefineRelation` | CREATE TABLE 主流程：命名空间与权限检查、父表加锁、表空间选择、`heap_create_with_catalog` 落目录、延迟做默认值与分区边界的 parse 变换、写继承关系、克隆分区索引与触发器，最后返回 `ObjectAddress`。 |
| `src/backend/commands/tablecmds.c` | `ATController` | ALTER TABLE 三段骨架：Phase 1 准备并生成工作队列、Phase 2 按 pass 改写目录、Phase 3 按需重写整表；Phase 1 结束时关关系留锁。 |
| `src/backend/commands/indexcmds.c` | `DefineIndex` | CREATE INDEX 主流程：受控搜索路径与 GUC 嵌套层、临时表强制非并发、`index_create` 落目录与依赖、回滚索引函数的 GUC 变更、分区表递归建索引、并发建索引的多事务三阶段。 |
| `src/backend/commands/typecmds.c` | `DefineType` | CREATE TYPE 主流程：superuser 门槛、命名空间解析、同名类型与自动数组类型的冲突处理、shell type 的建/查两条路径、类型属性解析（未知属性仅告警）、`TypeCreate` 落类型与数组类型并由其登记类型依赖。 |

## 与主核 / AI 的关系

主核 `engine/db` 的目录层没有 PostgreSQL 这套「三段式 ALTER + 依赖登记 + OID 空间」结构：`catalog_create_table` 是一次性写 `sys_table` 行与逐列 `sys_column` 行并建空堆，只接受列定义清单，没有 ALTER 的分趟与整表重写，也没有 `pg_depend` 式的对象依赖。可借鉴的是 `DefineRelation` 里「原始 parse 树必须等到对象已存在才能做语义变换」这条次序约束，以及 `ATController` 把「准备—改目录—重写物理」切成三段以容纳跨对象传播的做法；不要把 PostgreSQL 的 `ObjectAddress`、pass 序号或 shell type 两步走当作主核已有的目录语义。

## 参考

- [PostgreSQL 文档：CREATE TABLE](https://www.postgresql.org/docs/current/sql-createtable.html) —— 说明 CREATE TABLE 的对象级语义与参数面，与 `DefineRelation` 的输入相对应。
- [PostgreSQL 文档：ALTER TABLE](https://www.postgresql.org/docs/current/sql-altertable.html) —— 说明 ALTER TABLE 的子命令集合，与 `ATController` 的分派面相对应。
- [PostgreSQL 官方源码：表命令实现](https://github.com/postgres/postgres/blob/master/src/backend/commands/tablecmds.c) —— 支撑 `DefineRelation`/`ATController` 三段结构与锁、依赖、权限处理的事实。
- [PostgreSQL 官方源码：索引命令实现](https://github.com/postgres/postgres/blob/master/src/backend/commands/indexcmds.c) —— 支撑并发建索引多事务阶段与 GUC 隔离的事实。
