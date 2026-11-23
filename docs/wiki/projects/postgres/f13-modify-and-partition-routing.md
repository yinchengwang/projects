---
project: postgres
feature: modify-and-partition-routing
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/nodeModifyTable.c
    symbols: [ExecModifyTable, ExecInsert, ExecUpdate]
  - path: src/backend/executor/execPartition.c
    symbols: [ExecSetupPartitionTupleRouting, ExecFindPartition]
---

# 修改节点与分区路由

## 一句话职责

ModifyTable 把子计划产出的行应用到目标关系上，并在目标为分区表时按分区键把行分发到正确的叶子分区。

## 机制是什么

ModifyTable 是 INSERT/UPDATE/DELETE/MERGE 的统一执行节点：它从子计划取行，按命令类型调用插入、更新、删除或 merge 动作，处理行触发器和约束，并把 RETURNING 结果逐条返回给调用者。目标可以是普通表、分区叶子、分区根表或外部表；当一次查询涉及多个结果关系时，行携带 `tableoid` 垃圾列来决定当前应作用的关系。

分区路由是修改节点的一部分而不是独立节点：行先按分区键在分区树中逐层下降，找到叶子分区后再沿用该分区自己的 `ResultRelInfo`（包含分区的关系描述、索引、约束与触发器状态）。本机制不负责分区裁剪、分区定义本身、继承展开或执行期分区选择策略；那些属于规划与元数据侧。它也不负责属性映射算法的构建，路由只需要在根行类型与分区行类型不一致时调用已有的映射设施搬运列。

边界上还要区分三件事：分区约束检查在本机制内触发，但约束表达式的语义与构建不属于本机制；索引插入、堆插入由 TableAM 承接，这里只负责按命令语义调用并维护统计；并发更新冲突的 EvalPlanQual 重试属于本节点的控制流，但它复用的是通用的 EPQ 机制，不自行定义可见性规则。

## 怎么实现的

1. `ExecModifyTable` 首次调用先触发 BEFORE STATEMENT 触发器，然后在循环里重置 per-output 与 per-tuple 表达式上下文，再从子计划取一行。若存在 `mt_merge_pending_not_matched`（上一条 MERGE 遗留的待执行动作）就先把它执行完再继续，保证 RETURNING 结果按行顺序逐条返回而不打乱动作次序。
2. 当节点带 `mt_resultOidAttno` 时，从行里取 `tableoid` 垃圾列，与 `mt_lastResultOid` 比较；变化时通过 `ExecLookupResultRelByOid` 重新定位 `ResultRelInfo`。对 UPDATE/DELETE/MERGE，还按 relkind 分别取 ctid 垃圾列或 wholerow 垃圾列：前者用于堆类关系定位待改行，后者用于外部表或视图，同时作为触发器 OLD 值与 UPDATE 未修改列的来源。
3. 分区路由的入口是 `ExecPrepareTupleRouting`：先调 `ExecFindPartition` 得到叶子分区的 `ResultRelInfo`，再按过渡表捕获的需要决定是记录原始未转换行还是转换后的行，最后在存在根到子映射时用 `execute_attr_map_slot` 把槽转换为分区行类型。
4. `ExecSetupPartitionTupleRouting` 只建立最小的 `PartitionTupleRouting` 骨架：记录 `partition_root` 与分配用的内存上下文，并为根表初始化一个 `PartitionDispatch`。每个分区的 `ResultRelInfo` 推迟到真正有行路由过去时才创建，注释说明这是为了让“向分区表插入单行”这类常见情形保持低开销。
5. `ExecFindPartition` 先检查根表自身的分区约束，再从根 `PartitionDispatch` 逐层下降：用 `FormPartitionKeyDatum` 取分区键值，用 `get_partition_for_tuple` 得到分片下标；命中叶子时优先复用 `dispatch->indexes` 中已建好的 `ResultRelInfo`，其次尝试复用 ModifyTable 已持有的同 OID 关系信息（此时校验它允许 INSERT 并初始化路由信息），都没有才新建。中途遇到子分区表则继续下降，并在行类型变化时转换槽。
6. 若命中的是 default 分区，`ExecFindPartition` 会当场做一次分区约束检查，注释给出的理由是分区在并发新增后 default 的约束可能已变化，且不能只依赖 `ExecInsert` 来检查，否则非叶层的 default 分区会被漏掉。
7. `ExecInsert` 先在 `proute` 非空时执行上面的路由与行类型转换，并物化槽；随后按需打开分区索引，依次处理 BEFORE ROW 与 INSTEAD OF 触发器、外部表批次/FDW 插入或本地表的 RLS WITH CHECK、约束、分区约束、ON CONFLICT 的推测插入循环，最后调用 `table_tuple_insert` 与 `ExecInsertIndexTuples`。捕获过渡表时，跨分区 UPDATE 产生的插入被算进 UPDATE 的 NEW TABLE 而不是 INSERT 的过渡表。
8. `ExecUpdate` 由 `ExecUpdatePrologue` 处理 BEFORE ROW UPDATE 触发器并允许其取消更新，INSTEAD OF 与 FDW 路径分流后，本地表进入 `ExecUpdateAct`。当目标行不满足分区约束时，`ExecUpdateAct` 转成 `ExecCrossPartitionUpdate`：先以 `changingPart` 语义删除旧分区中的行（跳过 RETURNING），删除成功后再把转换回根行类型的槽交给从根表开始的 `ExecInsert` 重新路由。删除未成功时，普通 UPDATE 在并发更新场景下会取最新行、重新投影新行并返回重试信号，从而避免 DELETE 失败但 INSERT 成功导致总行数增加；`ON CONFLICT DO UPDATE` 触发跨分区移动则直接报错。

## 关键取舍

- 源码事实：`ExecSetupPartitionTupleRouting` 明确仅在配置根 dispatch，分区的 `ResultRelInfo` 由 `ExecFindPartition` 首次命中时按需分配；注释把原因写作“单行插入分区表必须快”。放弃一次性为所有分区建 `ResultRelInfo` 换来常见路径的低开销，代价是首次路由到某个分区时要临时打开关系、加载索引与触发器信息，这部分成本落在执行期而非准备期。
- 源码事实：`ExecFindPartition` 在 default 分区命中时就地调用 `ExecPartitionCheck`，而不是留给 `ExecInsert`。若把检查只放在插入路径上，非叶层的 default 子分区在继续下降时不会被检查；代价是同一行可能被检查不止一次，源码用显式注释说明这是为了保证不漏检。
- 源码事实：`ExecCrossPartitionUpdate` 把分区键更新实现为“删除加重新路由插入”，并要求删除成功才插入。CTID 链无法跨关系，因此普通 UPDATE 的 EvalPlanQual 语义在此不适用；源码选择用“删除失败即不插入”来有限地模拟这一语义，保证并发更新不会把一行变成两行，但放弃了完整的 EPQ 重检能力。
- 源码事实：跨分区 UPDATE 的插入在过渡表捕获上被记为 UPDATE 的 NEW TABLE，注释写明“被移动的行不属于 INSERT 过渡表”。若按底层动作归类成 INSERT 过渡事件，行会在两个过渡表里重复出现；代价是 `ExecInsert` 必须多带一个“这次插入由更新引起”的分支判断。
- 源码事实：`ExecModifyTable` 在入口处显式拒绝在 `estate->es_epq_active` 非空时被调用并抛错，同时保留在 CTE 子计划中被初始化与关闭的可能。放弃“允许 ModifyTable 出现在 EPQ 子计划里”使得 EPQ 重查路径只能传入子计划树，代价是这一约束要靠运行期检查而非仅靠断言来兜住易漏的测试场景。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/nodeModifyTable.c` | `ExecModifyTable` | 驱动子计划取行、选择结果关系、分发到插入/更新/删除/merge 并返回 RETURNING |
| `src/backend/executor/nodeModifyTable.c` | `ExecInsert` | 先做分区路由与行类型转换，再走触发器、约束、ON CONFLICT、堆插入与索引插入 |
| `src/backend/executor/nodeModifyTable.c` | `ExecUpdate` | 处理更新前触发器与并发重试，并在分区约束失败时转为跨分区移动（删除加重路由插入） |
| `src/backend/executor/execPartition.c` | `ExecSetupPartitionTupleRouting` | 建立根 `PartitionTupleRouting` 骨架，把分区 `ResultRelInfo` 推迟到首次命中时创建 |
| `src/backend/executor/execPartition.c` | `ExecFindPartition` | 逐层计算分区键、下降分区树、复用或新建叶子分区信息并检查 default 约束 |

## 参考

- [PostgreSQL 文档：表分区](https://www.postgresql.org/docs/current/ddl-partitioning.html) —— 支撑分区表与叶子分区、分区约束及 default 分区的语义背景。
- [PostgreSQL 文档：INSERT](https://www.postgresql.org/docs/current/sql-insert.html) —— 支撑插入路径中 ON CONFLICT 与 RETURNING 的行为描述。
- [PostgreSQL 文档：UPDATE](https://www.postgresql.org/docs/current/sql-update.html) —— 支撑更新路径与分区键变化导致行移动的语义。
- [PostgreSQL 官方源码：ModifyTable 节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeModifyTable.c) —— 支撑插入、更新、跨分区更新与 ON CONFLICT 推测插入流程。
- [PostgreSQL 官方源码：分区路由](https://github.com/postgres/postgres/blob/master/src/backend/executor/execPartition.c) —— 支撑按需构建分区信息与逐层分区键下降细节。

## 与主核 / AI 的关系

暂无直接对应。主核若实现分区写入，可借鉴“根关系骨架加按需叶子信息”“删除成功才重路由插入”“命中 default 时即时校验约束”的组织方式；但行类型映射的构建、索引与堆写入协议、并发重试语义必须分别由主核的属性映射层与存储层承接，不能由本节点直接套用。
