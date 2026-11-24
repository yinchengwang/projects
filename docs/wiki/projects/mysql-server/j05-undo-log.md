---
project: mysql-server
feature: undo-log
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/trx/trx0undo.cc
    symbols: [trx_undo_page_init, trx_undo_seg_create, trx_undo_write_xid]
---

# undo 日志

## 一句话职责

InnoDB 的 undo 日志由 `storage/innobase/trx/trx0undo.cc` 独占实现：它把「回滚所需的旧版本信息」写成独立于数据页的 undo 记录，按 `TRX_UNDO_INSERT` / `TRX_UNDO_UPDATE` 两类分装在 undo 段（undo segment）里，undo 段占用回滚段（rollback segment）的一个槽位（slot）；事务提交后 update undo 进入历史链表（history list）等待 purge，insert undo 立即丢弃。本页只讲 undo 段自身的组织、分配、复用与截断，**不负责行锁判定（J02）**、**不负责 redo 日志（I 组）**。

**边界（按 MECHANISM_SPECS 条目）：** 负责记录回滚所需的旧版本信息、undo 链的遍历与 purge 时的物理删除；不负责行锁（J02）、不负责 redo 日志（I 组）。

## 机制是什么

### undo 的三层载体

undo 的物理载体分三层，每层都由 `trx0undo.cc` 里的具体函数维护：

| 层 | 概念 | 谁维护 |
|---|---|---|
| 表空间层 | undo 表空间（undo tablespace），文件后缀 `.ibu` | `trx_undo_truncate_tablespace()` 负责整表空间重建 |
| 回滚段层 | 回滚段（rollback segment），undo 表空间里的一个段 | `trx_rsegf_undo_find_free()` 找空槽，`trx_undo_seg_create()` 建段 |
| undo 段层 | undo 段（undo segment），回滚段的一个槽位里的段 | `trx_undo_seg_create()` + `trx_undo_header_create()` |

回滚段的头页上有一张槽位表，`trx_undo_seg_create()` 用 `trx_rsegf_undo_find_free(rseg_hdr, mtr)` 在其中找一个空槽位：

```
slot_no = trx_rsegf_undo_find_free(rseg_hdr, mtr);
if (slot_no == ULINT_UNDEFINED) {
    ib::error(ER_IB_MSG_1212)
        << "Cannot find a free slot for an undo log."
           " You may have too many active transactions running concurrently."
           " Please add more rollback segments or undo tablespaces.";
    return (DB_TOO_MANY_CONCURRENT_TRXS);
}
```

找不到槽位时的错误码是 `DB_TOO_MANY_CONCURRENT_TRXS`，错误消息里点名要「加回滚段或 undo 表空间」。

### 两种 undo 类型

源码里 undo 类型只有两种，取值 `TRX_UNDO_INSERT` 与 `TRX_UNDO_UPDATE`：

- **`TRX_UNDO_INSERT`**：记录插入的行。事务提交或回滚后即可丢弃。
- **`TRX_UNDO_UPDATE`**：记录更新/删除前的旧版本。提交后不能立刻丢——一致性读要靠它重建旧版本，要等 purge 线程清掉历史链表里不再被任何读视图需要的段。

类型写在页头的 `TRX_UNDO_PAGE_TYPE` 偏移上，`trx_undo_page_init()` 用 `mach_write_to_2(page_hdr + TRX_UNDO_PAGE_TYPE, type)` 写入 2 字节。

### undo 段在页上的四段式布局

一个 undo 段的头页从低地址到高地址分四块，偏移量由 `trx0undo.cc` 里的常量拼出来：

```
undo_page + TRX_UNDO_PAGE_HDR      ← 页头：类型、start、free
undo_page + TRX_UNDO_SEG_HDR       ← 段头：状态、last_log、page_list、fseg header
undo_page + free                  ← undo log 头（trx_undo_header_create 写入）
undo_page + new_free              ← undo 记录区
```

`trx_undo_page_init()` 初始化页头三件事：

```
page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

mach_write_to_2(page_hdr + TRX_UNDO_PAGE_TYPE, type);
mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START,
                TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE,
                TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);

fil_page_set_type(undo_page, FIL_PAGE_UNDO_LOG);
```

- `TRX_UNDO_PAGE_START` 与 `TRX_UNDO_PAGE_FREE` 初值相同，都等于 `TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE`——**页头之后第一个可用字节**。
- 页类型被设成 `FIL_PAGE_UNDO_LOG`，缓冲池据此知道这一页是 undo 页。
- 页头里的 `type` 之后还要通过 `trx_undo_page_init_log()` 写一条 redo 记录：`mlog_write_initial_log_record(undo_page, MLOG_UNDO_INIT, mtr)` 再 `mlog_catenate_ulint_compressed(mtr, type)`。对应解析函数是 `trx_undo_parse_page_init()`。

### undo log 头的字段

`trx_undo_header_create()` 在段的空闲处写入一个 undo log 头，写入的字段有：

| 偏移常量 | 宽度 | 含义 |
|---|---|---|
| `TRX_UNDO_DEL_MARKS` | 2 字节 | 本 undo 是否含 delete mark |
| `TRX_UNDO_TRX_ID` | 8 字节 | 产生本 undo 的事务号 |
| `TRX_UNDO_LOG_START` | 2 字节 | undo 记录区起始偏移 |
| `TRX_UNDO_FLAGS` | 1 字节 | 标志位（XID / GTID 等） |
| `TRX_UNDO_DICT_TRANS` | 1 字节 | 是否为数据字典操作事务 |
| `TRX_UNDO_NEXT_LOG` | 2 字节 | 下一条 undo log 头的偏移，0 表示无 |
| `TRX_UNDO_PREV_LOG` | 2 字节 | 上一条 undo log 头的偏移 |

写头的顺序就是 undo 链的构造顺序：

```
free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);
log_hdr = undo_page + free;
new_free = free + TRX_UNDO_LOG_OLD_HDR_SIZE;

mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);
mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);
mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

prev_log = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
if (prev_log != 0) {
    prev_log_hdr = undo_page + prev_log;
    mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, free);
}
mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, free);
```

**`TRX_UNDO_LAST_LOG` 指向最新写的 log 头；新头的 `TRX_UNDO_PREV_LOG` 指向旧头，旧头的 `TRX_UNDO_NEXT_LOG` 指向新头——这就是 undo 链。** 同一条链上多个 undo log 头共享同一个头页的不同偏移，因此 `trx_undo_header_create()` 每次只在 `free` 处追加一个头，而不是新开一页。

页空间不够的断言来自这里：

```
ut_a(free + TRX_UNDO_LOG_GTID_XA_HDR_SIZE < UNIV_PAGE_SIZE - 100);
```

**头部预留的上限是 `UNIV_PAGE_SIZE - 100`**，减去的是 `TRX_UNDO_LOG_GTID_XA_HDR_SIZE`（同时容纳 GTID 与 XA 的最大头尺寸）。

### undo 段的三种状态

`TRX_UNDO_STATE` 偏移上的段状态有五种取值，源码里被写/读的有：

- `TRX_UNDO_ACTIVE`：段在使用中。`trx_undo_header_create()` 建头时写入这个值。
- `TRX_UNDO_CACHED`：段已释放但留在回滚段里等待复用。
- `TRX_UNDO_TO_FREE`：段可以整段删掉（insert undo 的典型归宿）。
- `TRX_UNDO_TO_PURGE`：段要进历史链表等 purge（update undo 的典型归宿）。
- `TRX_UNDO_PREPARED` / `TRX_UNDO_PREPARED_80028`：XA prepare 态。`trx_undo_set_prepared_in_tc()` 的断言 `ut_ad(undo->state == TRX_UNDO_PREPARED_80028 || undo->state == TRX_UNDO_PREPARED)` 同时接受这两个值。

### 三种 undo 头尺寸

头尺寸按「需要存什么」分三档，`trx_undo_header_add_space_for_xid()` 用一个 switch 决定：

```
ulint new_limit = TRX_UNDO_LOG_XA_HDR_SIZE;

switch (gtid_storage) {
  case trx_undo_t::Gtid_storage::COMMIT:
    new_limit = TRX_UNDO_LOG_GTID_HDR_SIZE;
    break;
  case trx_undo_t::Gtid_storage::PREPARE_AND_COMMIT:
    new_limit = TRX_UNDO_LOG_GTID_XA_HDR_SIZE;
    break;
  case trx_undo_t::Gtid_storage::NONE:
    new_limit = TRX_UNDO_LOG_XA_HDR_SIZE;
    break;
  default:
    ut_d(ut_error);
}

ulint new_free = free + (new_limit - TRX_UNDO_LOG_OLD_HDR_SIZE);
```

| GTID 存储形态 | 头尺寸常量 | 含义 |
|---|---|---|
| `Gtid_storage::NONE` | `TRX_UNDO_LOG_XA_HDR_SIZE` | 只放 XA 事务标识 |
| `Gtid_storage::COMMIT` | `TRX_UNDO_LOG_GTID_HDR_SIZE` | 只放提交期 GTID |
| `Gtid_storage::PREPARE_AND_COMMIT` | `TRX_UNDO_LOG_GTID_XA_HDR_SIZE` | prepare 与 commit 两份 GTID |

`trx_undo_header_create()` 先写最小尺寸的旧式头（`TRX_UNDO_LOG_OLD_HDR_SIZE`），`trx_undo_header_add_space_for_xid()` 再按需「扩容」——把 `TRX_UNDO_PAGE_START`、`TRX_UNDO_PAGE_FREE`、`TRX_UNDO_LOG_START` 三个偏移一起往后推，差额正好是 `new_limit - TRX_UNDO_LOG_OLD_HDR_SIZE`。扩容前有断言确认当前状态确实是旧式头的尾部：

```
ut_a(free == (ulint)(log_hdr - undo_page) + TRX_UNDO_LOG_OLD_HDR_SIZE);
```

**GTID 空间只给 update undo 留。** `trx_undo_create()` 里：

```
/* GTID storage is needed only for update undo log. */
if (type != TRX_UNDO_UPDATE) {
  gtid_storage = trx_undo_t::Gtid_storage::NONE;
}
```

`trx_undo_reuse_cached()` 里 insert 分支同样强制置 `NONE`。

## 怎么实现的

### 分配：先复用缓存，再新建

`trx_undo_assign_undo()` 是对外的分配入口，调用方是 `trx_undo_gtid_add_update_undo()`（在 `trx_undo.cc` 内调用 `trx_undo_assign_undo(trx, undo_ptr, TRX_UNDO_UPDATE)`）以及 InnoDB 行操作路径。

它先做 GTID 存储形态的判定，然后进入 mtr：

```
trx_undo_t::Gtid_storage gtid_storage{trx_undo_t::Gtid_storage::NONE};

if (!no_redo) {
    auto &gtid_persistor = clone_sys->get_gtid_persistor();
    if (is_first) {
        gtid_persistor.set_persist_gtid(trx, true);
    }
    gtid_storage = gtid_persistor.persists_gtid(trx);
}

mtr.start();
if (no_redo) {
    mtr.set_log_mode(MTR_LOG_NO_REDO);
}
```

**临时表空间的 undo 不写 redo**（`MTR_LOG_NO_REDO`），持久 undo 写 redo。判据是 `no_redo = (&trx->rsegs.m_noredo == undo_ptr)`。

拿到 rseg 的闩锁后，先试缓存：

```
undo = trx_undo_reuse_cached(rseg, type, trx->id, trx->xid, gtid_storage, &mtr);

if (undo == nullptr) {
    err = trx_undo_create(rseg, type, trx->id, trx->xid, gtid_storage, &undo, &mtr);
    if (err != DB_SUCCESS) {
        goto func_exit;
    }
}
```

`trx_undo_reuse_cached()` 从 `rseg->insert_undo_cached` 或 `rseg->update_undo_cached` 取链表头，取到就 `UT_LIST_REMOVE` 并 `MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED)`；取不到返回 `nullptr`，**回退到 `trx_undo_create()` 新建**。复用时 update undo 走 `trx_undo_header_create()`（在原头页追加一个新 log 头），insert undo 走 `trx_undo_insert_header_reuse()`。

复用 insert 头页时，源码把整页剩余空间当作可回收：

```
free = TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE;
...
new_free = free + TRX_UNDO_LOG_OLD_HDR_SIZE;

/* Insert undo data is not needed after commit: we may free all
the space on the page */
```

并且断言这一页确实是 insert undo 页：

```
ut_a(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) ==
     TRX_UNDO_INSERT);
```

### 建段：`trx_undo_seg_create()` 的三段失败路径

`trx_undo_seg_create()` 是本页 refs 里点名的核心符号。它按顺序做三件事，每步都有独立的失败出口：

**第一步，找空槽位**——失败返回 `DB_TOO_MANY_CONCURRENT_TRXS`（见上文）。

**第二步，预留 extent**：

```
space = page_get_space_id(page_align(rseg_hdr));

success = fsp_reserve_free_extents(&n_reserved, space, 2, FSP_UNDO, mtr);
if (!success) {
    return (DB_OUT_OF_FILE_SPACE);
}
```

**预留的 extent 数是 2**，类型 `FSP_UNDO`。预留失败返回 `DB_OUT_OF_FILE_SPACE`。

**第三步，建文件段**：

```
block = fseg_create_general(space, 0, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
                            true, mtr);

fil_space_release_free_extents(space, n_reserved);

if (block == nullptr) {
    /* No space left */
    return (DB_OUT_OF_FILE_SPACE);
}
```

`TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER` 是段头加文件段头占的空间。段头页随后被初始化：

```
page_hdr = *undo_page + TRX_UNDO_PAGE_HDR;
seg_hdr = *undo_page + TRX_UNDO_SEG_HDR;

trx_undo_page_init(*undo_page, type, mtr);

mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_FREE,
                 TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE, MLOG_2BYTES, mtr);

mlog_write_ulint(seg_hdr + TRX_UNDO_LAST_LOG, 0, MLOG_2BYTES, mtr);

flst_init(seg_hdr + TRX_UNDO_PAGE_LIST, mtr);

flst_add_last(seg_hdr + TRX_UNDO_PAGE_LIST, page_hdr + TRX_UNDO_PAGE_NODE, mtr);

trx_rsegf_set_nth_undo(rseg_hdr, slot_no, page_get_page_no(*undo_page), mtr);
*id = slot_no;

MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);
```

注意 `TRX_UNDO_PAGE_FREE` 的初值被直接写成 `TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE`——**undo 记录区从段头之后开始**，绕过了 `trx_undo_page_init()` 设的 `TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE`。段头页同时是 undo 段头页和回滚段槽位所在页。

### 写 XA 事务标识：`trx_undo_write_xid()`

`trx_undo_write_xid()` 是本页 refs 里点名的第三个符号，把 X/Open XA 事务标识写进 undo log 头：

```
mlog_write_ulint(log_hdr + TRX_UNDO_XA_FORMAT,
                 static_cast<ulint>(xid->get_format_id()), MLOG_4BYTES, mtr);

mlog_write_ulint(log_hdr + TRX_UNDO_XA_TRID_LEN,
                 static_cast<ulint>(xid->get_gtrid_length()), MLOG_4BYTES, mtr);

mlog_write_ulint(log_hdr + TRX_UNDO_XA_BQUAL_LEN,
                 static_cast<ulint>(xid->get_bqual_length()), MLOG_4BYTES, mtr);

mlog_write_string(log_hdr + TRX_UNDO_XA_XID,
                  reinterpret_cast<const byte *>(xid->get_data()),
                  XIDDATASIZE, mtr);
```

**四个字段：格式 ID（4 字节）、全局事务 ID 长度（4 字节）、分支限定符长度（4 字节）、事务数据（`XIDDATASIZE` 字节，定长）。**

它的唯一调用方是 `trx_undo_set_state_at_prepare()`，且只在 **XA PREPARE** 路径上（`rollback == false`）调用：

```
if (rollback) {
    /* XA ROLLBACK：状态改回 TRX_UNDO_ACTIVE */
    mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE, MLOG_2BYTES, mtr);
    return (undo_page);
}

undo->set_prepared(trx->xid);
mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, undo->state, MLOG_2BYTES, mtr);
mlog_write_ulint(undo_header + TRX_UNDO_FLAGS, undo->flag, MLOG_1BYTE, mtr);

trx_undo_write_xid(undo_header, &undo->xid, mtr);
```

**XA ROLLBACK 分支不写 XID，只把段状态改回 `TRX_UNDO_ACTIVE`**——源码注释解释了原因：这样系统被 kill 后恢复过程能执行回滚。

读回 XID 的对应函数是 `trx_undo_read_xid()`，用 `mach_read_from_4` 读三个长度字段、`xid->set_data(log_hdr + TRX_UNDO_XA_XID, XIDDATASIZE)` 读数据。

### 分配时的副作用：标记数据字典事务

`trx_undo_assign_undo()` 在挂上 undo 之后做一次检查：

```
if (trx->ddl_operation || trx_get_dict_operation(trx) != TRX_DICT_OP_NONE) {
    trx_undo_mark_as_dict_operation(undo, &mtr);
}
```

`trx_undo_mark_as_dict_operation()` 把 undo log 头的 `TRX_UNDO_DICT_TRANS` 偏移写成 1，并置 `undo->dict_operation = true`。恢复时靠这个标记识别出改过数据字典的事务。

### 分配时的另一个分支：为 GTID 补一条 update undo

`trx_undo_gtid_add_update_undo()` 是一个独立的补齐入口，被 `trx_commit_for_mysql()`（在 `trx0trx.cc` 中调用 `trx_undo_gtid_add_update_undo(trx, false, false)`）与回滚路径调用。它在 `trx->undo_mutex` 保护下调 `trx_undo_assign_undo(trx, undo_ptr, TRX_UNDO_UPDATE)` 补一条 update undo；补不到时打印 `ER_IB_CLONE_GTID_PERSIST`，并把 `trx->persists_gtid` 置回 `false`：

```
if (db_err != DB_SUCCESS) {
    trx->persists_gtid = false;
    ib::error(ER_IB_CLONE_GTID_PERSIST)
        << "Could not allocate undo segment"
        << " slot for persisting GTID. DB Error: " << db_err;
    ut_d(ut_error);
}
```

源码注释说明了为什么不能更早报这个错：GTID 信息在 prepare 之前还不知道。

### 段状态落定：`trx_undo_set_state_at_finish()`

事务结束时由 `trx_undo_set_state_at_finish()` 决定段的终态：

```
if (undo->size == 1 && mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE) <
                       TRX_UNDO_PAGE_REUSE_LIMIT) {
    state = TRX_UNDO_CACHED;
} else if (undo->type == TRX_UNDO_INSERT) {
    state = TRX_UNDO_TO_FREE;
} else {
    state = TRX_UNDO_TO_PURGE;
}
```

**三个条件依次判：单页（`undo->size == 1`）且该页空闲空间低于 `TRX_UNDO_PAGE_REUSE_LIMIT` → `TRX_UNDO_CACHED`（留在回滚段里复用）；否则 insert undo → `TRX_UNDO_TO_FREE`；否则（update undo）→ `TRX_UNDO_TO_PURGE`。**

### 三条收尾路径

**insert undo：`trx_undo_insert_cleanup()`** —— 事务提交或回滚后立刻处理：

```
if (undo->state == TRX_UNDO_CACHED) {
    UT_LIST_ADD_FIRST(rseg->insert_undo_cached, undo);
    MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
} else {
    ut_ad(undo->state == TRX_UNDO_TO_FREE);
    /* Delete first the undo log segment in the file */
    rseg->unlatch();
    DEBUG_SYNC_C("innodb_commit_wait_for_truncate");
    trx_undo_seg_free(undo, noredo);
    rseg->latch();
    trx_undo_mem_free(undo);
}
```

**段是 `TRX_UNDO_CACHED` 就进缓存链表等着复用；否则先调 `trx_undo_seg_free()` 删掉磁盘上的段，再调 `trx_undo_mem_free()` 释放内存对象。** 注意这里会先 `unlatch` 再删段——删段要反复 `fseg_free_step()`，不能在持有回滚段闩锁时长时间做。

`trx_undo_seg_free()` 的循环体每次做一个 `fseg_free_step()`，直到 `finished` 为真：

```
finished = fseg_free_step(file_seg, false, &mtr);

if (finished) {
    auto rseg_header = trx_rsegf_get(rseg->space_id, rseg->page_no, rseg->page_size, &mtr);
    trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL, &mtr);
    rseg->decr_curr_size(undo->size);
    MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);
}
```

**只有最后一步才把回滚段头页上的槽位写回 `FIL_NULL`（槽位重新变空）并递减回滚段占用。**

**update undo：`trx_undo_update_cleanup()`** —— 提交时把段交给 purge：

```
trx_purge_add_update_undo_to_history(
    trx, undo_ptr, undo_page, update_rseg_history_len, n_added_logs, mtr);

UT_LIST_REMOVE(rseg->update_undo_list, undo);
undo_ptr->update_undo = nullptr;

if (undo->state == TRX_UNDO_CACHED) {
    UT_LIST_ADD_FIRST(rseg->update_undo_cached, undo);
    MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
} else {
    ut_ad(undo->state == TRX_UNDO_TO_PURGE);
    trx_undo_mem_free(undo);
}
```

**`trx_purge_add_update_undo_to_history()` 在 `trx0purge.cc` 里**，它把 update undo log 头挂到回滚段历史链表的头部（`flst_add_first(rseg_header + TRX_RSEG_HISTORY, undo_header + TRX_UNDO_HISTORY_NODE, mtr)`），并把 `TRX_RSEG_HISTORY_SIZE` 增加 `undo->size`；段不可复用时用 `trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL, mtr)` 把槽位释放。

**关库清理：`trx_undo_free_trx_with_prepared_or_active_logs()`** —— 关库时把 prepared / 恢复态事务的 undo 内存对象摘链释放，不删磁盘段：

```
if (trx->rsegs.m_redo.update_undo) {
    ut_a(trx->rsegs.m_redo.update_undo->is_prepared() == prepared);
    UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->update_undo_list,
                   trx->rsegs.m_redo.update_undo);
    trx_undo_mem_free(trx->rsegs.m_redo.update_undo);
    trx->rsegs.m_redo.update_undo = nullptr;
}
```

它对 `m_redo` 与 `m_noredo` 两个 undo 指针的 insert / update 四个组合各做一遍相同处理。

### undo 链的遍历

undo 链的遍历在 `trx0undo.cc` 里有三个方向的入口，跨页时靠页链表 `TRX_UNDO_PAGE_LIST` 串起来：

- `trx_undo_get_next_rec(rec, page_no, offset, mtr)`：取下一条。页内没有下一条时调 `trx_undo_get_next_rec_from_next_page()`，后者用 `flst_get_next_addr(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr)` 找下一页，**取不到页号（`FIL_NULL`）就返回 `nullptr`**；跨页读取时按 `mode` 决定用 `trx_undo_page_get_s_latched()` 还是 `trx_undo_page_get()`（x-latch）。
- `trx_undo_get_prev_rec(rec, page_no, offset, shared, mtr)`：取上一条，跨页走 `trx_undo_get_prev_rec_from_prev_page()`，同样用页链表的 `flst_get_prev_addr()` 找上一页。
- `trx_undo_get_first_rec(modifier_trx_id, space, page_size, page_no, offset, mode, mtr)`：取第一条。`modifier_trx_id` 非空时，**顺手从 undo log 头的 `TRX_UNDO_TRX_ID` 偏移读出 8 字节事务号返回给调用方**——这是 undo 链上「这条 undo 属于哪个事务」的唯一权威来源。

### 截断：两端各有一套

**尾部截断 `trx_undo_truncate_end_func()`** —— 回滚时用来释放 undo 段尾部的空间：

```
const page_id_t page_id(undo->space, undo->last_page_no);
auto undo_page = trx_undo_page_get(page_id, undo->page_size, &mtr);

int trunc_offset = trx_undo_page_truncate_offset(undo, undo_page, limit);

/* If offset is within the page, truncate part of the page and quit.*/
if (trunc_offset > 0) {
    mlog_write_ulint(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE,
                     trunc_offset, MLOG_2BYTES, &mtr);
    break;
}

/* If all recs are < limit, don't truncate anything. */
if (trunc_offset < 0) {
    break;
}

/* Free the last page and move on to the next. */
ut_ad(undo->last_page_no != undo->hdr_page_no);
trx_undo_free_last_page_func(IF_DEBUG(trx, ) undo, &mtr);
```

`trx_undo_page_truncate_offset()` 返回三态：**大于 0 是页内偏移，小于 0 是不截断，0 是整页截掉**。判定依据是 undo 号与 limit 的比较：

```
while (rec != nullptr) {
    if (trx_undo_rec_get_undo_no(rec) < limit) {
        if (trunc_rec == nullptr) {
            return (-1);
        }
        return (trunc_rec - undo_page);
    }
    trunc_rec = rec;
    rec = trx_undo_page_get_prev_rec(rec, undo->hdr_page_no, undo->hdr_offset);
}
```

**头页永远不整页释放**——若最后一条记录就在头页上，返回的是页内偏移而不是 0。

**头部截断 `trx_undo_truncate_start()`** —— purge 侧丢弃历史链表头部的旧 undo：

```
rec = trx_undo_get_first_rec(nullptr, rseg->space_id, rseg->page_size,
                             hdr_page_no, hdr_offset, RW_X_LATCH, &mtr);
if (rec == nullptr) {
    /* Already empty */
    mtr.commit();
    return;
}
...
if (page_no == hdr_page_no) {
    trx_undo_empty_header_page(rseg->space_id, rseg->page_size, hdr_page_no,
                               hdr_offset, &mtr);
} else {
    trx_undo_free_page(rseg, true, rseg->space_id, hdr_page_no, page_no, &mtr);
}
```

**头页走 `trx_undo_empty_header_page()`（只清记录区，头页本身保留），非头页走 `trx_undo_free_page()` 整页释放。** 源码注释明确写了这条约束：「only whole pages are freed; the header page is not freed, but emptied」。

`trx_undo_free_page()` 在 `in_history` 为真时会同步递减回滚段历史长度：

```
if (in_history) {
    rseg_header = trx_rsegf_get(space, rseg->page_no, rseg->page_size, mtr);
    hist_size = mtr_read_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, mtr);
    ut_ad(hist_size > 0);
    mlog_write_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, hist_size - 1,
                     MLOG_4BYTES, mtr);
}
```

**历史链表长度是页数而不是条数**——每次释放一页就减 1。

### 段扩展：undo 段不够用时加页

`trx_undo_add_page()` 给 undo 段追加一页：

```
if (rseg->get_curr_size() == rseg->max_size) {
    return (nullptr);
}
...
new_block = fseg_alloc_free_page_general(
    TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER + header_page,
    undo->top_page_no + 1, FSP_UP, true, mtr, mtr);
...
undo->last_page_no = new_block->page.id.page_no();
new_page = buf_block_get_frame(new_block);
trx_undo_page_init(new_page, undo->type, mtr);
flst_add_last(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
              new_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);
undo->size++;
rseg->incr_curr_size();
```

**回滚段占用达到 `rseg->max_size` 时直接返回 `nullptr`（加页失败）。** 新页用 `FSP_UP` 标志向上分配，`undo->size` 与 `rseg->curr_size` 各加 1。

### 启动时重建内存对象

`trx_undo_mem_init()` 从磁盘段头重建内存对象 `trx_undo_t`：

```
state = mach_read_from_2(seg_header + TRX_UNDO_STATE);
offset = mach_read_from_2(seg_header + TRX_UNDO_LAST_LOG);
undo_header = undo_page + offset;
trx_id = mach_read_from_8(undo_header + TRX_UNDO_TRX_ID);
auto flag = mtr_read_ulint(undo_header + TRX_UNDO_FLAGS, MLOG_1BYTE, mtr);

bool xid_exists = ((flag & TRX_UNDO_FLAG_XID) != 0);
```

**恢复时靠 `TRX_UNDO_FLAGS` 里的 `TRX_UNDO_FLAG_XID` 位判断有没有 XID**，有则 `trx_undo_read_xid()` 读回来。

GTID 存储形态也是从 flags 反推的：

```
if ((flag & TRX_UNDO_FLAG_XA_PREPARE_GTID) != 0) {
    undo->m_gtid_storage = trx_undo_t::Gtid_storage::PREPARE_AND_COMMIT;
} else if ((flag & TRX_UNDO_FLAG_GTID) != 0) {
    undo->m_gtid_storage = trx_undo_t::Gtid_storage::COMMIT;
}
```

**prepare 位蕴含「prepare 与 commit 两份 GTID 的空间都已分配」。**

段状态 `TRX_UNDO_TO_FREE` 时页链表已不一致，源码直接 `goto add_to_list` 跳过页链表重建：

```
if (state == TRX_UNDO_TO_FREE) {
    goto add_to_list;
}
```

### 表空间级截断

`trx_undo_truncate_tablespace()` 做的是**整个 undo 表空间的重建**，分五步（源码注释里的 Step-1 到 Step-5）：

1. `fil_delete_tablespace(old_space_id, BUF_REMOVE_NONE)` 删旧文件。**若返回非 `DB_SUCCESS` 就直接 `return false`**；旧空间已经不在（`space == nullptr`）时，改调 `os_file_delete_if_exists()` 再试一次，因为 Windows 上文件被占用时删除会失败。
2. `fil_ibd_create(new_space_id, ..., flags, n_pages)` 用新的 space id 重建。
3. `fsp_header_init(new_space_id, n_pages, &mtr)` 初始化表空间头。
4. `trx_rseg_array_create(new_space_id, &mtr)` 建 RSEG_ARRAY 页。
5. 逐个回滚段重建头页：`trx_rseg_header_create()` + `trx_rsegf_get_new()`，并把 `rseg->space_id` 换成新的。

新表空间的页数来自：

```
auto n_pages = UNDO_INITIAL_SIZE_IN_PAGES;

if (space != nullptr && space->m_undo_extend > UNDO_INITIAL_SIZE_IN_PAGES &&
    space->m_last_extended.elapsed() < 1000) {
    /* UNDO is being extended aggressively, don't reduce size to default. */
    n_pages = fil_space_get_size(old_space_id) / 4;
}
```

**默认页数是 `UNDO_INITIAL_SIZE_IN_PAGES`；只有当「扩展量大于默认页数」且「距上次扩展不到 1000 毫秒」这两个条件同时成立，才改用旧表空间大小的四分之一。** 两个条件的逻辑与取自源码。

重建回滚段前有断言保证没有活跃事务：

```
ut_a(UT_LIST_GET_LEN(rseg->update_undo_list) == 0);
ut_a(UT_LIST_GET_LEN(rseg->insert_undo_list) == 0);
```

## 关键取舍

**1. undo 与 redo 彻底分离，undo 自己有 redo。** undo 页有自己的 redo 记录类型：`MLOG_UNDO_INIT`（页初始化）、`MLOG_UNDO_HDR_CREATE`（头创建）、`MLOG_UNDO_HDR_REUSE`（头复用）。解析入口是 `trx_undo_parse_page_init()` 与 `trx_undo_parse_page_header()`。源码注释强调「`MLOG_UNDO_HDR_CREATE` 和 `MLOG_UNDO_HDR_REUSE` 各自有专属日志类型，不许改这两个函数的行为」——**undo 的 redo 格式被显式冻结**，因为恢复路径依赖它。代价是 undo 的写放大要自己承担。

**2. 同一个页上串多个 undo log 头，而不是一个 undo 独占一页。** 文件头注释写明了动机：「If each transaction is given a whole page for its update undo log, file space consumption can be 10 times higher than necessary」。因此 `trx_undo_header_create()` 在 `free` 处追加头，用 `TRX_UNDO_LAST_LOG` 串成链。代价是 undo 段的截断必须区分「页内截断」和「整页释放」两种粒度。

**3. insert undo 与 update undo 用不同的回收时机。** insert undo 提交即可丢（`TRX_UNDO_TO_FREE`），update undo 必须等 purge（`TRX_UNDO_TO_PURGE`）。这是 MVCC 的直接要求：一致性读要靠 update undo 重建旧版本。代价是长事务会让 update undo 堆积，官方文档建议定期提交事务正是为了避免回滚段撑爆 undo 表空间。

**4. 空闲页整页复用而不是回收部分空间。** `trx_undo_set_state_at_finish()` 的判据是「单页」且「空闲空间低于 `TRX_UNDO_PAGE_REUSE_LIMIT`」才置 `TRX_UNDO_CACHED`。满足则整段留在回滚段缓存链表里等下一笔事务复用；不满足则整段删掉。**代价是页内剩余空间被浪费**——换来的是复用一个段时不需要处理「段内还残留别的 undo 记录」的复杂情形。

**5. GTID 空间按需分配，insert undo 一律不给。** `trx_undo_create()` 里 `if (type != TRX_UNDO_UPDATE) { gtid_storage = Gtid_storage::NONE; }`，`trx_undo_reuse_cached()` 的 insert 分支同样置 `NONE`。头尺寸因此有三档（`TRX_UNDO_LOG_XA_HDR_SIZE` / `TRX_UNDO_LOG_GTID_HDR_SIZE` / `TRX_UNDO_LOG_GTID_XA_HDR_SIZE`）。代价是头布局不是定长的，恢复时必须靠 `TRX_UNDO_FLAGS` 反推。

**6. 临时表 undo 不写 redo。** `trx_undo_assign_undo()` 里 `mtr.set_log_mode(MTR_LOG_NO_REDO)`（判据 `no_redo = (&trx->rsegs.m_noredo == undo_ptr)`），`trx_undo_seg_free()`、`trx_undo_truncate_end_func()`、`trx_undo_truncate_start()` 里对系统临时表空间也一律 `MTR_LOG_NO_REDO`。官方文档说明这类 undo 「只用于服务器运行期间的回滚，不需要崩溃恢复」。**代价是临时表空间不 redo 记录，崩溃后这部分数据不可恢复**——换来的是省掉 redo 写 I/O。

**7. 删段时先放闩锁再删。** `trx_undo_insert_cleanup()` 在调 `trx_undo_seg_free()` 前先 `rseg->unlatch()`，删完再 `rseg->latch()`。因为 `trx_undo_seg_free()` 要循环 `fseg_free_step()`，长时间持闩锁会卡住回滚段上的其他事务。**代价是这中间存在一段段状态未受闩锁保护的窗口**。

**8. 头页永远保留，只清记录区。** `trx_undo_truncate_start()` 的源码注释写明「only whole pages are freed; the header page is not freed, but emptied」——头页走 `trx_undo_empty_header_page()`，只把 `TRX_UNDO_LOG_START` 推到记录区末尾。代价是每个 undo 段至少占一页不回收。

**9. 段的空间不足时直接失败，而不是排队等待。** `trx_undo_seg_create()` 找不到槽位返回 `DB_TOO_MANY_CONCURRENT_TRXS`，extent 预留失败或 `fseg_create_general()` 返回空返回 `DB_OUT_OF_FILE_SPACE`。`trx_undo_add_page()` 在回滚段占满 `rseg->max_size` 时返回 `nullptr`。**代价是并发事务数有硬上限**，官方文档给的上限估算公式是 `(innodb_page_size / 16) * innodb_rollback_segments * undo 表空间数`。

**10. undo 表空间截断用「删了重建」，不原地收缩。** `trx_undo_truncate_tablespace()` 的五步是：删旧文件 → 用新 space id 建新文件 → 初始化表空间头 → 建 RSEG_ARRAY 页 → 逐个重建回滚段头页。**回滚段对象本身被复用**（`marked_rsegs` 里的对象不重建，只换 `space_id` 和 `page_no`）。代价是过程复杂且需要额外的空间 id 管理（`undo::unuse_space_id` / `undo::use_next_space_id`）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/trx/trx0undo.cc | `trx_undo_page_init` | 初始化 undo 页头：`TRX_UNDO_PAGE_TYPE` 写 2 字节类型，`TRX_UNDO_PAGE_START` / `TRX_UNDO_PAGE_FREE` 初值设为 `TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE`，页类型设 `FIL_PAGE_UNDO_LOG` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_page_init_log` | 写页初始化的 redo 记录：`MLOG_UNDO_INIT` + 压缩的 type |
| storage/innobase/trx/trx0undo.cc | `trx_undo_parse_page_init` | `MLOG_UNDO_INIT` 的解析入口，回调 `trx_undo_page_init()` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_seg_create` | 在回滚段里建 undo 段：`trx_rsegf_undo_find_free()` 找槽位（失败 `DB_TOO_MANY_CONCURRENT_TRXS`）→ `fsp_reserve_free_extents(..., 2, FSP_UNDO, ...)` 预留 2 个 extent（失败 `DB_OUT_OF_FILE_SPACE`）→ `fseg_create_general()` 建文件段（返回空同样 `DB_OUT_OF_FILE_SPACE`）→ 初始化页头与段头 → `trx_rsegf_set_nth_undo()` 占槽 → `MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED)` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_header_create` | 追加一个 undo log 头：把 `TRX_UNDO_PAGE_START` / `TRX_UNDO_PAGE_FREE` / `TRX_UNDO_LOG_START` 各推进 `TRX_UNDO_LOG_OLD_HDR_SIZE`，段状态置 `TRX_UNDO_ACTIVE`，旧头的 `TRX_UNDO_NEXT_LOG` 指向新头、新头的 `TRX_UNDO_PREV_LOG` 指向旧头，`TRX_UNDO_LAST_LOG` 指向新头；断言 `free + TRX_UNDO_LOG_GTID_XA_HDR_SIZE < UNIV_PAGE_SIZE - 100` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_header_add_space_for_xid` | 按 GTID 存储形态把头从旧式尺寸扩到 `TRX_UNDO_LOG_XA_HDR_SIZE` / `TRX_UNDO_LOG_GTID_HDR_SIZE` / `TRX_UNDO_LOG_GTID_XA_HDR_SIZE`，同步推进页头与 `TRX_UNDO_LOG_START` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_write_xid` | 写 X/Open XA 事务标识：`TRX_UNDO_XA_FORMAT`（格式 ID，4 字节）、`TRX_UNDO_XA_TRID_LEN`（4 字节）、`TRX_UNDO_XA_BQUAL_LEN`（4 字节）、`TRX_UNDO_XA_XID`（`XIDDATASIZE` 字节定长数据） |
| storage/innobase/trx/trx0undo.cc | `trx_undo_read_xid` | 读回 XID：用 `mach_read_from_4` 读三个长度字段，`xid->set_data()` 读 `XIDDATASIZE` 字节数据 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_assign_undo` | 分配入口：判 `no_redo` 决定 `MTR_LOG_NO_REDO`，算 `gtid_storage`，先 `trx_undo_reuse_cached()` 复用缓存，返回 `nullptr` 时回退到 `trx_undo_create()`；随后挂 `rseg->insert_undo_list` / `rseg->update_undo_list`，按 `trx->ddl_operation` 调 `trx_undo_mark_as_dict_operation()`，XA prepared 态调 `undo->set_prepared()` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_create` | 新建 undo 段：检查 `rseg->get_curr_size() == rseg->max_size` 返回 `DB_OUT_OF_FILE_SPACE`，调 `trx_undo_seg_create()`，再 `trx_undo_header_create()` + `trx_undo_header_add_space_for_xid()`；非 update undo 强制 `gtid_storage = NONE`；内存对象创建失败返回 `DB_OUT_OF_MEMORY` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_reuse_cached` | 复用缓存段：从 `rseg->insert_undo_cached` / `rseg->update_undo_cached` 取链表头（取不到返回 `nullptr`），insert 走 `trx_undo_insert_header_reuse()`、update 走 `trx_undo_header_create()`，再统一 `trx_undo_header_add_space_for_xid()` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_insert_header_reuse` | 复用 insert 头页：`free` 设为 `TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE`，断言页类型是 `TRX_UNDO_INSERT`，段状态置 `TRX_UNDO_ACTIVE`，写 `MLOG_UNDO_HDR_REUSE` redo 记录 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_mark_as_dict_operation` | 把 undo log 头的 `TRX_UNDO_DICT_TRANS` 写成 1 并置 `undo->dict_operation = true` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_gtid_add_update_undo` | 为 GTID 持久化补一条 update undo：在 `trx->undo_mutex` 保护下调 `trx_undo_assign_undo(trx, undo_ptr, TRX_UNDO_UPDATE)`；失败时置 `trx->persists_gtid = false` 并报 `ER_IB_CLONE_GTID_PERSIST` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_gtid_write` | 把 GTID 写进 undo 头：写 `TRX_UNDO_LOG_GTID_VERSION` 与定长 GTID（`TRX_UNDO_LOG_GTID_LEN`，`static_assert` 校验等于 `GTID_INFO_SIZE`），最后回写 `TRX_UNDO_FLAGS` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_gtid_read_and_persist` | 恢复时从 undo 头读回 GTID 并加入持久化器 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_set_state_at_finish` | 决定段终态：`undo->size == 1` 且空闲低于 `TRX_UNDO_PAGE_REUSE_LIMIT` → `TRX_UNDO_CACHED`；insert undo → `TRX_UNDO_TO_FREE`；其余 → `TRX_UNDO_TO_PURGE` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_set_state_at_prepare` | XA PREPARE / XA ROLLBACK 时改段状态：rollback 分支把状态改回 `TRX_UNDO_ACTIVE`；prepare 分支写 `TRX_UNDO_FLAGS` 并调 `trx_undo_write_xid()` 落 XID |
| storage/innobase/trx/trx0undo.cc | `trx_undo_set_prepared_in_tc` | XA prepare 落 TC 状态时写 GTID 并改段状态；断言状态是 `TRX_UNDO_PREPARED_80028` 或 `TRX_UNDO_PREPARED` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_update_cleanup` | update undo 收尾：调 `trx_purge_add_update_undo_to_history()` 把段交给历史链表，摘出 `rseg->update_undo_list`；段是 `TRX_UNDO_CACHED` 则进缓存链表，否则 `trx_undo_mem_free()` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_insert_cleanup` | insert undo 收尾：段是 `TRX_UNDO_CACHED` 则进 `rseg->insert_undo_cached`，否则先放闩锁调 `trx_undo_seg_free()` 删磁盘段、再 `trx_undo_mem_free()` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_seg_free` | 删 undo 段：循环 `fseg_free_step()` 直到完成，完成后 `trx_rsegf_set_nth_undo(..., FIL_NULL, ...)` 释放槽位、递减 `rseg->curr_size`、`MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED)` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_free_page` | 释放非头页：从段头页链表摘除、`fseg_free_page()` 释放页、递减 `rseg->curr_size`；`in_history` 为真时把 `TRX_RSEG_HISTORY_SIZE` 减 1 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_free_last_page_func` | 释放 undo 段最后一页并把 `undo->last_page_no` 更新为剩余段的最后页 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_empty_header_page` | 清空头页的 undo 记录区：把 `TRX_UNDO_LOG_START` 推到记录区末尾，头页本身保留 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_page_truncate_offset` | 算截断位置三态：页内偏移（大于 0）、不截断（小于 0）、整页截掉（0）；按 `trx_undo_rec_get_undo_no(rec) < limit` 判定，头页永不全页截断 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_truncate_end_func` | 尾部截断：调 `trx_undo_page_truncate_offset()`，页内偏移则改写 `TRX_UNDO_PAGE_FREE`，整页则调 `trx_undo_free_last_page_func()` 继续 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_truncate_start` | 头部截断：取首条记录，页号等于头页则调 `trx_undo_empty_header_page()`，否则调 `trx_undo_free_page()` 整页释放 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_add_page` | 给 undo 段加页：回滚段占满 `rseg->max_size` 时返回 `nullptr`；`fsp_reserve_free_extents(..., 1, FSP_UNDO, ...)` 预留后 `fseg_alloc_free_page_general(..., FSP_UP, ...)` 分配，新页 `trx_undo_page_init()` 初始化并挂进段页链表，`undo->size` 与 `rseg->curr_size` 各加 1 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_get_next_rec` | 取 undo 链下一条记录；页内取不到时跨页，经 `flst_get_next_addr()` 找下一页，页号 `FIL_NULL` 则返回 `nullptr` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_get_prev_rec` | 取 undo 链上一条记录；跨页经 `flst_get_prev_addr()` 找上一页 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_get_first_rec` | 取 undo 链第一条；`modifier_trx_id` 非空时从 undo 头 `TRX_UNDO_TRX_ID` 读出 8 字节事务号返回 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_mem_init` | 启动时从磁盘段头重建内存对象：读 `TRX_UNDO_STATE`、`TRX_UNDO_LAST_LOG`、`TRX_UNDO_TRX_ID`、`TRX_UNDO_FLAGS`；`TRX_UNDO_FLAG_XID` 位决定是否 `trx_undo_read_xid()`；GTID 位反推 `m_gtid_storage`；段状态 `TRX_UNDO_TO_FREE` 时跳过页链表重建 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_mem_create` | 分配并初始化 `trx_undo_t` 内存对象 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_mem_free` | 释放 undo 内存对象（`ut::free`），前置断言 `undo->id < TRX_RSEG_N_SLOTS` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_lists_init` | 启动或新建回滚段时初始化 undo 链表，返回 undo 段的总页数 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_free_trx_with_prepared_or_active_logs` | 关库时释放 prepared / 恢复态事务的 undo 内存对象；对 `m_redo` 与 `m_noredo` 的 insert / update 四个组合各摘链并 `trx_undo_mem_free()` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_truncate_tablespace` | 整表空间重建五步：`fil_delete_tablespace()` 删旧文件（失败返回 `false`）、`fil_ibd_create()` 用新 space id 重建、`fsp_header_init()` 初始化表空间头、`trx_rseg_array_create()` 建 RSEG_ARRAY 页、逐个回滚段 `trx_rseg_header_create()` 重建头页；默认页数 `UNDO_INITIAL_SIZE_IN_PAGES`，扩展量大于默认页数且距上次扩展不足 1000 毫秒时改用旧空间大小的四分之一 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_parse_page_header` | `MLOG_UNDO_HDR_CREATE` / `MLOG_UNDO_HDR_REUSE` 的解析入口，分别回调 `trx_undo_header_create()` 与 `trx_undo_insert_header_reuse()` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_header_create_log` | 写 `MLOG_UNDO_HDR_CREATE` redo 记录 + 压缩的 trx_id |
| storage/innobase/trx/trx0undo.cc | `trx_undo_insert_header_reuse_log` | 写 `MLOG_UNDO_HDR_REUSE` redo 记录 + 压缩的 trx_id |
| storage/innobase/trx/trx0purge.cc | `trx_purge_add_update_undo_to_history` | purge 侧接盘 update undo：`flst_add_first()` 挂到回滚段历史链表头部，`TRX_RSEG_HISTORY_SIZE` 增加 `undo->size`，不可复用时 `trx_rsegf_set_nth_undo(..., FIL_NULL, ...)` 释放槽位，并写 `TRX_RSEG_MAX_TRX_NO` 与 `TRX_UNDO_TRX_NO` |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己的 WAL 与 pager，与 InnoDB 的 `trx0undo.cc` 没有代码级关联，但机制形态可以对照：

- **两者都把「可回滚信息」与「数据页」分开存放。** InnoDB 把 undo 放在独立 undo 表空间 / 回滚段的 undo 段里，靠页链表 `TRX_UNDO_PAGE_LIST` 串起来；主核的回滚信息放在 WAL 里。两边都面临同一个问题：**回滚信息的存储成本与业务数据的写入成本不成正比**。
- **两者都做「按需分配头部空间」。** InnoDB 用 `TRX_UNDO_LOG_OLD_HDR_SIZE` 先写最小头，再按 GTID / XA 需求扩到对应尺寸；主核的 WAL 记录也有「变长头 + 变长负载」的分层编码思路。
- **两者都有「提交后回收」的两种节奏。** InnoDB 区分 insert undo（提交即丢）与 update undo（等 purge）；主核的 WAL 也区分「提交即可截断」与「需要读者确认后才能截断」。

**架构推论：** InnoDB 的 `trx_undo_seg_create()` 把「找槽位 → 预留 extent → 建文件段」三步拆成三个独立错误码（`DB_TOO_MANY_CONCURRENT_TRXS` / `DB_OUT_OF_FILE_SPACE` / `DB_OUT_OF_MEMORY`），而不是统一成一个失败码，这样上层才能区分「加回滚段能解决」与「加磁盘空间能解决」。主核若要报「undo 空间不足」，可借鉴这种按「缺什么资源」分类返回错误码的做法，而不是笼统报一个「内部错误」。

**架构推论：** `trx_undo_header_create()` 把 undo log 头串成双向链（`TRX_UNDO_NEXT_LOG` / `TRX_UNDO_PREV_LOG`，段头 `TRX_UNDO_LAST_LOG` 指向最新），并让多个 undo log 共享同一个头页的不同偏移。这一设计的收益是「一个 undo 段承载一笔事务的全部 undo 记录」，代价是截断必须区分页内与整页两种粒度（`trx_undo_page_truncate_offset()` 的三态返回值就是为这个代价付的账）。主核若要做多版本存储，可对照这一「同页串链 + 段级截断」的划分。

**架构推论：** `trx_undo_header_add_space_for_xid()` 的做法是「先写最小头，再回头把三个偏移一起推进」——即头部变长靠后补而非预留。这让常规路径（不带 GTID / XA 的事务）只需写最小头，但要求恢复路径能反推实际头尺寸（`trx_undo_mem_init()` 靠 `TRX_UNDO_FLAGS` 反推 `m_gtid_storage` 就是配套手段）。主核若要支持可变的记录头，需要同时设计好「写入时的后补」与「恢复时的反推」两套逻辑。

## 参考

- [storage/innobase/trx/trx0undo.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/trx/trx0undo.cc) —— 本页全部主符号（`trx_undo_page_init`、`trx_undo_seg_create`、`trx_undo_write_xid`）与 undo 段分配、复用、截断全链路的权威来源。
- [storage/innobase/trx/trx0purge.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/trx/trx0purge.cc) —— `trx_purge_add_update_undo_to_history()` 的实现位置，是 update undo 从「事务侧」交接到「purge 侧」的接口点。
- [Undo Logs（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-undo-logs.html) —— 官方对 insert undo 与 update undo 划分（insert undo 提交即可丢、update undo 一致性读还要用）、单事务最多四条 undo 日志、回滚段槽位数等于 `innodb_page_size / 16` 的说明，对照本页的 `TRX_UNDO_INSERT` / `TRX_UNDO_UPDATE` 与槽位机制。
- [Undo Tablespaces（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-undo-tablespaces.html) —— 官方对 undo 表空间、`innodb_rollback_segments` 默认值 128、undo 表空间自动截断流程与 `innodb_max_undo_log_size` / `innodb_undo_log_truncate` 的说明，对照本页 `trx_undo_truncate_tablespace()` 的重建流程。
- [InnoDB Multi-Versioning（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-multi-versioning.html) —— 官方对 `DB_ROLL_PTR` 指向 undo 记录、purge 才是物理删除时机的说明，是本页「update undo 必须等 purge」这一约束的文档依据。
