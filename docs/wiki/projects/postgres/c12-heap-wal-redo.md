---
project: postgres
feature: heap-wal-redo
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/heap/heapam_xlog.c
    symbols: [heap_redo, heap2_redo, heap_mask]
---

# Heap WAL redo

## 一句话职责

Heap redo 按 WAL 中的 heap/heap2 操作重建堆页状态，并为一致性检查屏蔽允许存在差异的非持久提示字节。

## 机制是什么

`heap_redo` 与 `heap2_redo` 分别分派 heap 与 heap2 resource manager 的记录到具体页面重放程序。执行器依据 block reference 读取 redo buffer；若页面 LSN 已足够新则跳过 heap page 修改，若需要 redo 则按记录提供的字段、tuple 数据和 offset 更新页面，再设置页面 LSN 并标脏。部分操作还要维护 visibility map、FSM 或 hot standby 冲突语义。

本页只讲恢复侧对记录的应用和 page consistency mask，不讲主事务路径如何组装、插入或写出 WAL 记录（那属于 B 组 WAL 生成机制）。它也不替代资源管理器通用恢复框架：XLog 读取记录、处理 FPI/redo action、调用 rmgr 回调；heap redo 解释本 AM 的操作码及数据。对无法识别的操作码或记录与页面状态不相容，代码使用 PANIC 防止带着错误页状态继续恢复。

## 怎么实现的

1. `rmgrlist.h` 将 `RM_HEAP_ID` / `RM_HEAP2_ID` 分别关联到 `heap_redo` / `heap2_redo`，并共享 `heap_mask`。恢复框架提供 `XLogReaderState`；分派器屏蔽通用 info bits 后，以 heap 操作码选择具体处理函数。
2. 单页路径通过 `XLogReadBufferForRedo` 或其 extended 变体获取 buffer 与 `XLogRedoAction`。若 FPI 已应用或页面 LSN 已越过该记录，heap 主页可跳过修改；需要 redo 时持 buffer 内容锁修改。异常 offset、非 normal line pointer、插入失败等以 PANIC 报告记录/页面不一致。
3. `heap_xlog_insert` 从 record header 与 block data 重建 tuple header/data，按记录给出的 offset 用 page API 插入，设置 `t_xmin`、`t_ctid`、可剪枝提示、清理 all-visible 状态，并设置页面 LSN/dirty。multi-insert 与 update 分别按其 compact 数据格式重建多个 tuple 或新版本；delete/lock/in-place 等路径按记录字段更新既存 tuple。
4. heap2 的 prune/freeze redo 反序列化 line-pointer 变更和 freeze plans，执行页面整理/tuple 冻结；multi-insert 复用批量插入逻辑，lock-updated 更新锁定版本。带 snapshot conflict horizon 的记录在 hot standby 上先调用冲突处理，避免仍需旧版本的 standby 查询越过恢复修改。
5. heap 页操作与 VM 并非总能一起按普通 buffer LSN 决定。相关记录按记录 flags 对应重放/清除 VM bits；代码遵守 VM 与页头 all-visible 标志的顺序约束，并在适当时更新 FSM 提示。heap buffer 释放后再做可能的 VM/FSM I/O，避免持 heap 排他锁做 I/O。
6. WAL consistency checking 调用 `heap_mask` 处理 primary image 与 replay image 后比较。它屏蔽 LSN/checksum、hint bits、未用空间、命令 ID，以及未冻结 tuple 的可变事务 hint；但不屏蔽有恢复语义的 moved-partition `ctid` 差异。

## 关键取舍

- **以页面 LSN 和 redo action 跳过已完成主页修改，而不是无条件重复应用。** 这保留 WAL 重放幂等性并支持 FPI；代价是 VM 相关更新有专门分支，因为共享 VM 页上其他记录可能改变不同 bits，不能仅因 heap 页已新就一概跳过。源码事实见 `heap_xlog_vm_clear` 与 multi-insert 的 VM 注释。
- **用 resource-manager 专用 mask 做一致性比较，而非逐字节要求完全相同。** hint bits、LSN/checksum 和空闲区本来就可能不同，逐字节比较会将正常差异报成恢复损坏；过度屏蔽又会掩盖真实 WAL 漏写，因此 mask 仍保留 tuple 的关键事务/链信息。恢复框架 caller 是 `xlogrecovery.c` 的 `verifyBackupPageConsistency`，它对 replay 与 backup image 都套用 rmgr mask 再比较。
- **redo 中结构性不变量违例用 PANIC，而不是跳过记录继续。** 若 offset 越界或 line pointer 类型非法，后续 WAL 可能基于该页状态继续；继续恢复会扩散不一致。代价是恢复会中断，需要调查 WAL/存储；对无法识别操作码同样 panic，避免静默丢失操作。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/heap/heapam_xlog.c` | `heap_redo` | 把 heap record 操作码分派到 insert/delete/update/lock 等页面重放逻辑 |
| `src/backend/access/heap/heapam_xlog.c` | `heap2_redo` | 分派 prune/freeze、multi-insert、lock-updated 与 rewrite 类 heap2 记录 |
| `src/backend/access/heap/heapam_xlog.c` | `heap_mask` | 规范化恢复前后允许变化的页面字段，供 WAL consistency 比较使用 |
| `src/include/access/rmgrlist.h` | `RM_HEAP_ID`、`RM_HEAP2_ID` | 将两类 resource manager 接到对应 redo callback，并登记共享 heap mask |
| `src/backend/access/transam/xlogrecovery.c` | `verifyBackupPageConsistency` | redo 后对页副本应用 rmgr mask 并比较一致性 |

## 与主核 / AI 的关系

暂无直接对应。区分 redo 与 WAL 生成尤其重要：redo 消费已编码记录并更改数据页，生成侧才决定何时、如何把变更描述写入日志；二者不能互相替代。

## 参考

- [PostgreSQL 官方源码：heap WAL redo](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/heapam_xlog.c) —— 支撑 heap/heap2 分派、页面变更和 mask 行为
- [PostgreSQL 官方源码：WAL 恢复与一致性检查](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xlogrecovery.c) —— 支撑 redo 框架及 backup page consistency 比较
- [PostgreSQL 官方源码：resource manager 列表](https://github.com/postgres/postgres/blob/master/src/include/access/rmgrlist.h) —— 支撑 redo/mask 回调接线
