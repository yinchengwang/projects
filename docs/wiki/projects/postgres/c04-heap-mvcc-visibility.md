---
project: postgres
feature: heap-mvcc-visibility
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/heap/heapam_visibility.c
    symbols: [HeapTupleSatisfiesMVCC, HeapTupleSatisfiesUpdate, HeapTupleSetHintBits]
---

# Heap tuple MVCC 可见性

## 一句话职责

Heap MVCC 可见性逻辑根据 tuple header 中的事务标记与调用方给定的 snapshot，决定版本对读取是否可见或是否可被修改。

## 机制是什么

堆 tuple header 记录 `xmin`、`xmax`、命令 ID 与 `t_infomask` 状态位。`HeapTupleSatisfiesMVCC` 将这些局部信息与传入 snapshot 的 XID 边界/活跃事务集合以及事务提交状态结合，判定插入是否已对该 snapshot 生效，以及删除或更新是否应隐藏旧版本；当前事务还要用 `curcid` 区分本语句前后的修改。该函数消费已构建的 snapshot，不负责构造 snapshot 或维护事务状态存储。

`HeapTupleSatisfiesUpdate` 回答的是更偏修改操作的问题：除可见/不可见外还给出 `TM_SelfModified`、`TM_Updated`、`TM_Deleted`、`TM_BeingModified` 等状态，供更新者决定等待、报并发更新或继续。`HeapTupleSetHintBits` 将已确定的 xmin/xmax 状态缓存回 tuple header，但仅在调用者持有 buffer 排他锁时使用。页内 HOT 链和 VM 标志的管理不由这里定义。

## 怎么实现的

1. `HeapTupleSatisfiesMVCC` 要求 snapshot 已注册或激活，并从 tuple header 的 `HEAP_XMIN_COMMITTED`、`HEAP_XMIN_INVALID` 等 hint bits 开始检查，不会为了所有判断都立即查共享事务结构。
2. 若 xmin 尚未判定，先处理旧式 moved tuple 标记，再判断 xmin 是否属于当前事务；当前事务插入用 `cmin >= snapshot->curcid` 判定“当前语句之后插入”。其他事务则用 `XidInMVCCSnapshot` 判断其是否在该快照的活跃集合中；不在其中时才查询是否提交，并据此设置 committed/invalid hint bit。
3. xmin 已提交但仍可能落在快照活跃集合时，未冻结版本仍按快照视为未提交；否则进入 xmax 阶段。`xmax` 无效或仅表示行锁时版本仍可见；`xmax` 为 MultiXact 时提取真正的更新 XID，分别考虑当前事务命令 ID、快照成员、提交结果与中止情况。
4. 普通 xmax 同样区分当前事务 `cmax`、snapshot 活跃集合与提交状态。当前事务在当前命令之后做的删除/更新不影响本语句看到的旧版本；在本命令前完成的则隐藏它。快照内的并发删除者按仍未提交处理；已提交且不在快照中时旧版本不可见。
5. hint bit 更新由 `SetHintBitsExt` 完成。永久关系上设置“提交”类提示前，会检查 commit LSN 与 buffer LSN 的写出关系；若 WAL 尚未安全写出则暂不落位。批量路径还会用 `BufferBeginSetHintBits` 取得可设置状态，不能设置时在本批余下调用中关闭 hint-bit 写入。
6. `HeapTupleSatisfiesUpdate` 使用传入 `curcid` 并返回 `TM_Result`，对正在更新的事务与锁定 MultiXact 保留 `TM_BeingModified` 等状态。导出的 `HeapTupleSetHintBits` 明确断言 buffer 由当前进程排他锁定，避免对共享页状态无保护修改。

## 关键取舍

- 源码事实：MVCC 判定对快照仍视为活跃的插入/删除事务不主动查询其“当前真实状态”来设置 hint bits；源码注释指出这样可避免高频共享结构和 ProcArrayLock 竞争，而该次可见性结果不会因此改变。架构推论：将缓存更新延后给较新 snapshot 的访问者，换取当前热点读取较少的状态查询；更积极的每次探查只会增加争用且无法让旧 snapshot 改变答案。
- 源码事实：已提交事务的 hint bits 在永久页上受 commit LSN/buffer LSN interlock 约束，LSN 条件不满足时直接不设置。架构推论：接受重复事务状态检查的成本来避免页面 hint 先于对应 WAL 持久化；无此检查虽省一次判断，却可能让崩溃恢复后的页提示与事务结果冲突。
- 源码事实：MVCC 读取返回 bool，而 update 判定返回 `TM_Result` 且接收调用方 `curcid`；更新器需要分辨自己修改、别人更新/删除及仍在修改的版本。架构推论：将读可见性和写冲突分类拆开，避免把可见与可安全写入混为一谈；若复用单一 bool，调用者就得额外查询并发状态，且可能丢失等待/重试所需信息。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/heap/heapam_visibility.c` | `HeapTupleSatisfiesMVCC` | 消费 snapshot，对 xmin、xmax、命令 ID 与 hint bits 做快照相对的可见性判定。 |
| `src/backend/access/heap/heapam_visibility.c` | `HeapTupleSatisfiesUpdate` | 把 tuple 状态分类为更新所需的 `TM_Result`，包括自修改、他事务更新/删除与正在修改。 |
| `src/backend/access/heap/heapam_visibility.c` | `HeapTupleSetHintBits` | 在 buffer 已排他锁定的前提下，为调用方提供更新 tuple 状态提示的导出入口。 |

## 与主核 / AI 的关系

主核事务状态机制并未采用 PostgreSQL 的 `xmin`/`xmax` tuple header MVCC 表示。可对照理解“持久化对象局部状态 + snapshot 判断 + 结果提示缓存”的职责拆分，但不要映射为主核已有的相同可见性算法。

## 参考

- [PostgreSQL 文档：并发控制介绍](https://www.postgresql.org/docs/current/mvcc-intro.html) —— 说明 MVCC 通过多版本与 snapshot 保持并发读取语义。
- [PostgreSQL 官方源码：Heap tuple 可见性判定](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/heapam_visibility.c) —— 支撑 xmin/xmax、命令 ID 和 hint bit 的判断流程。
