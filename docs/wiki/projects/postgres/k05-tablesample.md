---
project: postgres
feature: tablesample
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/tablesample/tablesample.c
    symbols: [GetTsmRoutine]
  - path: src/backend/access/tablesample/system.c
    symbols: [tsm_system_handler, system_nextsampleblock]
  - path: src/backend/access/tablesample/bernoulli.c
    symbols: [tsm_bernoulli_handler, bernoulli_nextsampletuple]
---

# TABLESAMPLE 抽样

## 一句话职责

TABLESAMPLE 通过可插拔的抽样方法决定访问哪些数据页与哪些元组，并把抽样比例换算成规划期的代价估算。

## 机制是什么

`TABLESAMPLE method(参数) [REPEATABLE(seed)]` 在语法层绑定一个抽样方法 handler 函数；`GetTsmRoutine` 调用该 handler 得到 `TsmRoutine` 描述符，其中包含参数类型列表、可重复性标志，以及规划与执行阶段的回调：`SampleScanGetSampleSize`、`InitSampleScan`、`BeginSampleScan`、`NextSampleBlock`、`NextSampleTuple`、`EndSampleScan`。

抽样方法只决定"访问哪些页"和"页内取哪些 offset"，并不解释元组的 MVCC 可见性，也不做条件过滤；`SampleScan` 节点对返回的 offset 仍走正常的 table AM 读取与可见性判断，因此不可见或已被 HOT 更新的槽位会被跳过。该方法也不负责随机数发生器本身，可重复性由 seed 与哈希决定。

## 怎么实现的

1. 解析阶段把方法名解析为 handler OID；`GetTsmRoutine` 以 `OidFunctionCall1` 调用 handler（传 NULL 参数），检查返回值非 NULL 且 `IsA(routine, TsmRoutine)`，否则报 handler 未返回有效结构。handler 用 `makeNode(TsmRoutine)` 初始化，未用字段保持 NULL。
2. 规划阶段执行 `SampleScanGetSampleSize`，把抽样比例参数与基表信息换算成估算页数与元组数。system 与 bernoulli 的换算逻辑相同：`estimate_expression_value` 求参数值，只接受 `[0,100]` 内的非 NaN 常量，非法或非常量时回退到源码中的默认比例 0.1。
3. 页数估算两法不同：system 按 `baserel->pages * samplefract` 估计会访问的页数；bernoulli 返回全部 `baserel->pages`，因为每个页都要被逐元组检查。元组数两者都按 `baserel->tuples * samplefract` 估算，均经 `clamp_row_est` 收紧。
4. `InitSampleScan` 为方法分配状态结构（`SystemSamplerData` / `BernoulliSamplerData`）；`BeginSampleScan` 校验百分比范围并据 seed 计算 cutoff 阈值写入状态，`nextblock` 或 `lt` 位置字段初始化为未开始。
5. system 方法的 `NextSampleBlock` 对块号与 seed 组成 2 个 `uint32` 输入做 `hash_bytes`，哈希小于 `cutoff` 的块被选中；找到后把游标推进到下一块，到关系末尾返回 `InvalidBlockNumber`。其 `NextSampleTuple` 则连续返回该块内所有 offset，直到块末。
6. bernoulli 方法把 `NextSampleBlock` 置 NULL（即访问全部页），由 `NextSampleTuple` 对 `(blockno, tupoffset, seed)` 组成 3 个 `uint32` 输入做哈希判定，逐 offset 决定是否抽样。因为逐元组独立判定，其抽样单元是元组而非整页。

## 关键取舍

- **源码事实：**system 方法对块号哈希决定整块是否入选，块内元组全部返回；bernoulli 不改页选择、逐元组哈希。**架构推论：**system 以整页为单位抽样，I/O 顺序性更好且每页只需一次判定，但同块元组相关性会使统计方差偏大；bernoulli 抽样单元更细、统计更均匀，代价是要访问全部页并逐元组哈希。
- **源码事实：**两法的规划估算在参数非常量、越界或 NaN 时统一回退到源码默认比例，而不是报错。**架构推论：**规划阶段不因参数不确定而失败，保留可执行的保守估算；若在规划期直接报错，`REPEATABLE` 之类的表达式参数就无法在运行时才定值。
- **源码事实：**`NextSampleBlock` / `NextSampleTuple` 只返回页号与 offset，注释明确"不保证元组可见甚至存在，由 SampleScan 处理"。**架构推论：**把抽样与可见性彻底解耦，使方法实现不必理解 MVCC；若抽样方法试图自行跳过不可见元组，就要重复实现快照判断并可能与 HOT 链语义冲突。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/tablesample/tablesample.c` | `GetTsmRoutine` | 调用方法 handler 并校验其返回 TsmRoutine 结构。 |
| `src/backend/access/tablesample/system.c` | `tsm_system_handler` | 装配 system 方法参数类型、可重复性与各阶段回调。 |
| `src/backend/access/tablesample/system.c` | `system_nextsampleblock` | 按块号与 seed 哈希选取整块并推进块游标。 |
| `src/backend/access/tablesample/bernoulli.c` | `tsm_bernoulli_handler` | 装配 bernoulli 回调，并将 NextSampleBlock 留空以访问全部页。 |
| `src/backend/access/tablesample/bernoulli.c` | `bernoulli_nextsampletuple` | 逐 offset 哈希判定是否抽样该元组。 |

## 参考

- [PostgreSQL 文档：TABLESAMPLE 子句](https://www.postgresql.org/docs/current/sql-select.html) —— 说明抽样方法与 REPEATABLE 的语法位置。
- [PostgreSQL 文档：SYSTEM_ROWS 抽样方法](https://www.postgresql.org/docs/current/tsm-system-rows.html) —— 说明以固定行数为抽样单元的方法，与本文按比例抽样的 system/bernoulli 形成对照。
- [PostgreSQL 官方源码：TSM 接口定义](https://github.com/postgres/postgres/blob/master/src/include/access/tsmapi.h) —— 对照 TsmRoutine 各回调的契约。

## 与主核 / AI 的关系

暂无直接对应。主核若提供抽样扫描，可借鉴"只决定页与 offset、可见性交回扫描节点"的边界；抽样方法注册与可重复性语义需按主核插件模型定义。
