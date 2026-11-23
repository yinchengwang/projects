---
project: postgres
feature: generic-xlog-api
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/generic_xlog.c
    symbols: [GenericXLogStart, GenericXLogRegisterBuffer, GenericXLogFinish]
---

# Generic WAL API

## 一句话职责

`GenericXLog` 为索引等不想维护专属 redo payload 的代码提供一层页变更记录门面：调用者改一份私有页副本，finish 时框架比较原页与新页、生成可重放 delta 并统一提交 WAL。

## 机制是什么

常规 rmgr 需要为每种页面修改定义自己的 record 结构、插入/redo 代码与兼容逻辑；对小型或扩展型 AM 来说，这笔开发成本可能大于其记录量。Generic WAL API 把工作形式化为一组页副本变更：调用者开始一个 `GenericXLogState`，为需要改的 buffer 注册工作副本，在副本上执行页面操作，然后 finish。框架比较原 buffer 与副本，编码「哪些 offset 区域要覆盖」的 fragments；redo 把 fragments 应用回页面，因此调用者不必自行设计一套 rmgr payload schema。

它是**专用 rmgr 之外的通用封装门面**，不是 WAL record 插入流程的通用替代，也不改变 buffer 锁/pin 的责任：调用者仍需按页面访问规则持锁、pin，并在失败时释放它们。本文不讲通用 XLog insert 公共入口（b01）或 rmgr 表注册（b15）；GenericXLog 本身使用 `RM_GENERIC_ID` 并提供固定的 `generic_redo`。是否适合读者/扩展使用取决于工作负载：没有真实的 generic_xlog caller，源码本身也只展示 API 定义与实现，清单把它作为「确有读者需求时保留」的有条件页是合理的边界提醒。

核心状态 `GenericXLogState` 持对齐的 `images[MAX_GENERIC_XLOG_PAGES]`、对应 `GenericXLogPageData pages[]` 与 `isLogged`；每个 page data 记录 buffer、flags、deltaLen、私有修改 image 与 delta 缓冲。`MAX_GENERIC_XLOG_PAGES` 在公共头中等于 `XLR_NORMAL_MAX_BLOCK_ID`，不是一个随业务页数无限增长的列表。API 不替 caller 获取/释放 buffer 锁 pin；`GenericXLogAbort` 的注释明确要求 caller 自行 release pins/locks。

## 怎么实现的

1. **开始并判断关系是否 WAL-logged。** `GenericXLogStart(Relation relation)` 以 `PG_IO_ALIGN_SIZE` 对齐分配 state，把 `RelationNeedsWAL(relation)` 缓存成 `isLogged`，并将各 pageData 的 image 指向对齐 images 区、buffer 设成 invalid。这个标志决定 finish 走 logged 或 unlogged 分支。
2. **注册 buffer 并复制页面。** `GenericXLogRegisterBuffer(state, buffer, flags)` 在线性扫描固定页数组时先复用已注册同一 buffer 的 image；遇到第一个空槽，就记录 buffer/flags 并把 `BufferGetPage(buffer)` 的 `BLCKSZ` 字节复制到私有 image，返回该 image 给 caller 修改。若槽位用尽则报错，不会悄悄覆盖已有 page。
3. **logged 分支先算差异、再把新 image 应用到实际 buffer。** `GenericXLogFinish` 对 WAL-logged relation 先 `XLogBeginInsert` 并进入 critical section；每个已注册页在原 buffer 仍未被改写时计算 delta（除 `GENERIC_XLOG_FULL_IMAGE` 外），然后将 image 的 lower 部分复制到真实页、把 `pd_lower..pd_upper` 的 hole 清零、再复制 upper 部分并标脏。full-image 模式注册 `REGBUF_FORCE_IMAGE | REGBUF_STANDARD`；delta 模式注册 standard buffer 与 `XLogRegisterBufData` 的 fragments。
4. **统一插入并回填页面 LSN。** 所有 buffer refs 与 delta 都准备完后插入单条 `RM_GENERIC_ID` record，得到 LSN，再将同一 LSN 写到全部实际页面，结束 critical section。这样同一 API 调用涉及的多页变更处在一个 WAL record 的 redo 单元里。
5. **delta 编码和回放规则。** 每个 fragment 由 `OffsetNumber offset`、`OffsetNumber length` 和 length 字节数据组成；未变化区域不写。`MATCH_THRESHOLD` 等于 fragment header（两个 OffsetNumber 的大小），当未改区域不大于 header 时把相邻差异合并，避免为省下少量 payload 反而多存一个 header。计算分 lower 与 upper 两段，不跨中间 hole 合并。redo 对每个 block ref 取 buffer，只有 `BLK_NEEDS_REDO` 时读取 delta 并逐 fragment memcpy；然后同样把 hole 置零并设置 record end LSN。
6. **unlogged 路径不发 WAL。** `isLogged == false` 时 finish 仍 critical-section 保护地把 image 全页复制回 buffer 并标脏，但不生成 WAL、也不设置页面 LSN，返回 `InvalidXLogRecPtr`。成功 finish 会释放 state；若构造中止，`GenericXLogAbort` 只释放 state，不应用任何 image，并把锁/pin 清理责任留给调用者。

## 关键取舍

**记 page delta，而不是对所有修改一律记整页 image。** `computeRegionDelta` 仅保留变化 fragment，变化稀疏时记录能比 full page image 小；代价是 finish 要扫描页面比较，并在 redo 时重放多个变长 fragment。源码明确 `MAX_DELTA_SIZE` 上界为 `BLCKSZ + 2 * FRAGMENT_HEADER_SIZE`，最坏时两段各一个 header 加完整页数据，所以它不承诺 delta 总是更小。对于页面改动范围大或 caller 指定 `GENERIC_XLOG_FULL_IMAGE` 的情况，直接强制 full image，放弃压缩以避免计算 delta 与小幅压缩收益不成比例。

**caller 改私有 copy，而不是直接原地改 buffer 后再尝试撤销。** 放弃的是额外分配与一次页复制；换来 abort 时不会留下未记录的部分修改，且框架能对比原页与目标页计算 redo 数据。`GenericXLogAbort` 因此只需 free state，但它不替 caller unlock/unpin。替代方案要在每一个错误点保存 undo copy 或标记 buffer 状态，事务异常路径更复杂。

**`pd_lower` 与 `pd_upper` 之间的页内 hole 一律归零，不尝试在 delta 中保留其原值。** delta 不携带 hole 内容，redo 和本地 finish 都把 hole 清零以保证两边结果逐字节一致。放弃的是保留 hole 中可能存在的旧字节，换来不让未定义/已删除的页内残留内容导致 replay 与原 buffer 分叉；这种 canonicalization 也避免 WAL 泄露无关旧内容。

**固定最大页数，不动态扩容。** `MAX_GENERIC_XLOG_PAGES` 沿用 XLog record 可引用的 block 上限，state 数组静态包含每页 image 和最大 delta。放弃的是一次操作可登记任意多页，代价是超限时报 `ERROR`；换来 record block-id 与 API 状态数组边界一致、无 finish 期动态扩容。真实 fail path 是 `GenericXLogRegisterBuffer` 找不到空槽后直接 `elog(ERROR)`，caller 必须把操作分批或改用专属 rmgr。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/generic_xlog.c` | `GenericXLogStart` | 创建对齐的状态对象、初始化页槽与 `RelationNeedsWAL` 分支标志，为一次事务式页面修改提供私有副本容器 |
| `src/backend/access/transam/generic_xlog.c` | `GenericXLogRegisterBuffer` | 将 buffer 放入固定槽并快照到可修改 image；重复 buffer 返回原 image，超出页槽上限时报错 |
| `src/backend/access/transam/generic_xlog.c` | `GenericXLogFinish` | 对 logged relation 计算 delta、应用 image、登记 buffers/data、插入 generic record 并设置页 LSN；unlogged relation 则只写回脏页并返回 invalid LSN |

## 参考

- [PostgreSQL 官方源码：generic_xlog.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/generic_xlog.c) —— GenericXLog 页副本、delta 与 redo 实现
- [PostgreSQL 官方源码：generic_xlog.h](https://github.com/postgres/postgres/blob/master/src/include/access/generic_xlog.h) —— 扩展可见 API 与最大 page 数边界
- [PostgreSQL 官方源码：genericdesc.c](https://github.com/postgres/postgres/blob/master/src/backend/access/rmgrdesc/genericdesc.c) —— generic record 的 block delta 人类可读描述

## 与主核 / AI 的关系

暂无直接对应。
