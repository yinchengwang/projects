# P3 D/E 抽检复核报告

> 资料边界：本报告根据 P3 全库验收报告及抽检记录中可取得的 D/E 发现整理，并逐项复核页面和指定 PostgreSQL 源码。没有把未提供的抽样分数或缺失的逐页检查项补写成事实。
> 抽检页面：D 组 d08-gin-write-and-pending-list；E 组 f07-index-and-bitmap-scans。
> 源码仓：`C:\code\projects\.cache\repos\postgres`（PostgreSQL 提交 `9b5d4e3331753b3399e84965093109cac8f364bf`）。

## 摘要

| 页 | 复核结论 | 重点 |
|---|---|---|
| `d08-gin-write-and-pending-list.md` | 页面当前有源码归属错误，早先预警与源码事实相反 | 源码定义在 `gininsert.c`，而页面误写为定义在 `ginfast.c`；`ginfast.c` 实际是 cleanup caller。 |
| `f07-index-and-bitmap-scans.md` | 核心执行叙述与源码一致，但“免回表”实现映射的 refs 覆盖不足 | `IndexOnlyNext` 调用 TableAM 接口；heap AM 的 all-visible 判定与回退实现在未列入该页 refs 的 `heapam_indexscan.c`。因此内容可读，但按源码归属/refs 可追溯性应扣分。 |

## 逐项源码证据

### D08：GIN 写入与 pending list

**早先预警与当前源码事实相反，须明确更正。**先前有关 `ginEntryInsert` 在 `ginfast.c` 定义的预警不成立。实际定义位于 `src/backend/access/gin/gininsert.c` 的 `ginEntryInsert`：函数头注释说明它把一个键相关联的一个或多个 heap TID 插入 GIN entry，可新增单条键记录或扩展既有记录；函数体初始化 entry scan、定位叶页，查到既有 entry 后按 posting tree 等格式执行后续写入。

证据链：

1. 打开 `gininsert.c` 的函数定义可见完整函数签名和职责注释；正文中 `ginEntryInsert` 是真实定义。源码路径：`src/backend/access/gin/gininsert.c`，符号 `ginEntryInsert`。
2. 对 `ginfast.c` 做全文件符号搜索，只发现两处 `ginEntryInsert(...)` 调用，均在 pending-list cleanup 把聚合后的键/TID 写入主索引时调用；没有函数体定义。源码路径：`src/backend/access/gin/ginfast.c`，调用位置属于 `ginInsertCleanup` 的处理流程。
3. [d08 页面](projects/postgres/d08-gin-write-and-pending-list.md) 的正文“机制是什么”只写“这里的 `ginEntryInsert` 实际定义在 `ginfast.c`”吗？复读页面后确认：该段写的是“这里的 `ginEntryInsert` 实际定义在 `ginfast.c`”，源码落点表也把 `gininsert.c` 行标成“该文件含其调用；真实定义在 `ginfast.c`”。这两处均与源码相反：真实定义应为 `gininsert.c`，`ginfast.c` 是 cleanup caller 的调用点。
4. 因而结论是：**源码事实与早先预警相反，但 d08 页面当前仍存在同一归属错误**；正确事实是定义在 `gininsert.c`、`ginfast.c` 有调用。请修订页面两处错误，并保留 spec refs 原样；不要把“预警错了”误写成“页面已正确”。

对正文的相关事实作源码对照：pending-list cleanup 位于 `ginfast.c`，会收集 pending tuples 并调用 `ginEntryInsert`；entry 插入定义则位于 `gininsert.c`。两个角色必须区分：前者是 caller/批量清理，后者是主索引 entry 写入实现。

> 注：本报告只要求形成抽检报告、不修改被审页面；上列发现是待主代理/页面作者处理的事实问题。故报告不擅自改动 d08 页面。

### F07：索引与位图扫描执行

**扣分理由：index-only “免回表”的实现归属映射超出本页 refs，不能把 TableAM 的实现归到 executor 源码。**

1. [f07 页面](projects/postgres/f07-index-and-bitmap-scans.md) 第 32 行将 `IndexOnlyNext` 描述为“若 `xs_recheck` 为真则重检”，这是执行节点本身的行为；refs 所列 `src/backend/executor/nodeIndexonlyscan.c` 确有 `IndexOnlyNext`，并通过 `table_index_getnext_slot` 取得下一个索引扫描结果。
2. 页面第 33 行进一步描述真正免回表：调用 `heapam_index_getnext_slot`，检查 `VM_ALL_VISIBLE`，全可见时返回索引 tuple，否则回退 `heapam_index_only_heap_fetch`；这些实现细节并不在 f07 refs 内。
3. 源码实际归属是 `src/backend/access/heap/heapam_indexscan.c`：`heapam_index_only_tuple_getnext_slot` 调用 `heapam_index_getnext_slot(..., true)`；公共实现检查 `VM_ALL_VISIBLE`，不全可见时调用 `heapam_index_only_heap_fetch`。这些函数属于 heap TableAM，不属于 `nodeIndexonlyscan.c`。
4. 因此页面虽用“由 table AM 决定”正确划清责任边界，但实施细节没有对应 refs 支撑。按“实现映射/来源可追溯性”应扣分；处理方向是保留清单 refs 不变，明确标出 table AM 的职责边界，并将未列 refs 的具体 helper 降为带路径说明的旁证，或提请清单维护者在后续机制清单调整中补齐 refs。不可把 `heapam_index_getnext_slot` 说成 `IndexOnlyNext` 内部函数。

## 抽检范围与评分口径说明

全库验收报告记录 D/E/F 批次合计抽检 15 页，并概述 D08 的符号定义归属与 F07 的 index-only 映射问题；但本次可读取的材料中没有 D/E 抽样清单、逐页六项评分表或评分数值。本报告因此只给上述两页的可复核源码证据和定性结论，不虚构总分、逐项分数或其它抽样页的结论。

## 建议后续处理

- D08：更正页面中 `ginEntryInsert` 的定义路径为 `gininsert.c`；注明 `ginfast.c` 是 pending cleanup 的调用点。
- F07：保留节点层和 TableAM 层边界；如果保留 heap-only helper 的实现细节，应明示其源码文件不在当前 refs 中，并按清单维护流程决定是否调整规格。
- 对外引用的具体性、各页取舍分数等未能从现有材料复核的项，待取得原抽检记录后再补，不在本报告推断。
