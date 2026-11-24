---
project: mysql-server
feature: file-io
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/fil/fil0fil.cc
    symbols: [fil_io, fil_flush]
---

# 文件 I/O 封装

## 一句话职责

`storage/innobase/fil/fil0fil.cc` 里的文件 I/O 封装负责把 InnoDB 内部「按 (space_id, page_no) 定位」的页访问请求，翻译成对真实数据文件的字节级读写：入口 `fil_io()` 负责把请求路由到持有该表空间的那个 `Fil_shard`，再由 `Fil_shard::do_io()` 完成校验、定位文件节点、准备文件、计算物理偏移、设置压缩 / 加密 / 打洞标志，最终调用 `os_aio()` 把请求交给 OS 层；入口 `fil_flush()` 负责把某个表空间的数据真正 fsync 落盘，驱动者是 `Fil_shard::space_flush()`。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责 InnoDB 层的文件读写、预读与同步（fsync）的封装；**不负责 OS 层接口（J 组 mysys）**、**不负责 redo 的语义（I01）**。

## 机制是什么

### 两层入口：一个管读写，一个管落盘

`fil_io()` 与 `fil_flush()` 是两个职责完全不同的入口，源码里没有任何一方的调用关系：

| 入口 | 语义 | 内部委托 |
|---|---|---|
| `fil_io()` | 发起一次页级读 / 写，把字节推进到 OS 层（不等于落盘） | `fil_system->shard_by_id(page_id.space())` → `Fil_shard::do_io()` → `os_aio()` |
| `fil_flush()` | 把某个表空间已写入的数据真正刷到盘上（fsync） | `fil_system->shard_by_id(space_id)` → `Fil_shard::space_flush()` → `os_file_flush(file.handle)` |

**写与刷是两件事**：`fil_io()` 成功返回只代表字节已经交给操作系统，落盘与否由 `fil_flush()` 决定。这一分离在 `Fil_shard::write_completed()` 里体现得最清楚——写完成后它只把表空间挂到「未刷列表」上（`add_to_unflushed_list()`），真正的 `os_file_flush()` 要等到刷盘路径才发生。

### 分片：`Fil_system` / `Fil_shard`

InnoDB 的文件层不是单一全局结构，而是分片的。`fil_io()` 的第一行就是：

```
auto shard = fil_system->shard_by_id(page_id.space());
```

`fil_flush()` 同样如此，只是用 `space_id` 而非 `page_id.space()` 定位分片。每个 `Fil_shard` 各自持有：

- `m_spaces`：按 space id 哈希的表空间实例表；
- `m_names`：按表空间名哈希的表空间实例表；
- `m_LRU`：最近使用、可以随时关闭的文件节点链表；
- `m_unflushed_spaces`：已写入但未落盘的表空间链表；
- `m_deleted_spaces`：已删除表空间的记录。

**架构推论：** 分片的作用是把「表空间查找」这类热点锁的粒度切细，让不同表空间的 I/O 与 flush 互不串行。依据是 `Fil_shard::do_io()` 在真正发起 I/O 之前就 `mutex_release()` 释放了分片锁——锁只覆盖「查表空间 → 定位文件 → 准备文件 → 算偏移」这段元数据操作，`os_aio()` 调用发生在锁外。

### 打开文件的 LRU 与两个配额

`Fil_shard::prepare_file_for_io()` 决定文件是否需要现场打开：

```
if (!file->is_open) {
  ut_a(file->n_pending_ios == 0);
  if (!open_file(file)) {
    return false;
  }
}
if (file->n_pending_ios == 0) {
  remove_from_LRU(file);
}
++file->n_pending_ios;
```

打开后立刻从 LRU 链表摘除——**文件在有未完成 I/O 期间不会被关闭**，`Fil_shard::complete_io()` 在 `n_pending_ios` 归零时才调 `add_to_lru_if_needed()` 放回 LRU。

`open_file()` 里有两个配额，用 `acquire_right` lambda 原子地「抢占」：

- `fil_system->m_open_files_limit`：已打开文件总数上限；
- `m_n_files_not_belonging_in_lru`：不能被 LRU 关闭的文件数上限。

`Fil_system::space_belongs_in_LRU()` 决定了哪些表空间的文件参与 LRU：

```
switch (space->purpose) {
  case FIL_TYPE_TABLESPACE:
    return !fsp_is_system_tablespace(space->id) &&
           !fsp_is_undo_tablespace(space->id);
  case FIL_TYPE_TEMPORARY:
  case FIL_TYPE_IMPORT:
    return true;
}
```

即**系统表空间与 undo 表空间的文件永远不进 LRU**，因此它们要单独占用第二个配额。源码注释解释了原因：系统表空间的文件必须一直保持打开，否则读完成回调里再读 insert buffer 会死锁。

### 刷盘的触发条件：`modification_counter` 与 `flush_counter`

`Fil_shard::write_completed()` 每次写完成都会推进文件自己的修改计数：

```
++m_modification_counter;
file->modification_counter = m_modification_counter;
```

而 `Fil_shard::space_flush()` 里判定「这个文件要不要刷」的判据是这两个计数的比较：

```
int64_t old_mod_counter = file.modification_counter;
...
if (!disable_flush && (old_mod_counter <= file.flush_counter)) {
  continue;
}
```

**含义：只要自上次刷盘以来没有新的修改，就跳过 fsync。** 这就是 InnoDB 刷盘的节流机制——页被反复写但没有新变化时不会反复触发 fsync。

刷完盘后把 `flush_counter` 推进到本次的 `old_mod_counter`：

```
if (file.flush_counter < old_mod_counter) {
  file.flush_counter = old_mod_counter;
  remove_from_unflushed_list(space);
}
```

`remove_from_unflushed_list()` 里又有一道校验 `space_is_flushed(space)`——只有确认表空间整体都已刷干净才真正摘除。

### `fil_disable_space_flushing()`：两种禁用刷盘的场景

```
static inline bool fil_disable_space_flushing(const fil_space_t *space) {
#ifndef _WIN32
  if (space->purpose == FIL_TYPE_TABLESPACE &&
      srv_unix_file_flush_method == SRV_UNIX_O_DIRECT_NO_FSYNC) {
    return true;
  }
#endif /* !_WIN32 */
  if (space->purpose == FIL_TYPE_TEMPORARY) {
    return true;
  }
  return false;
}
```

两种场景：

1. **Unix 上 `innodb_flush_method` 设为 `O_DIRECT_NO_FSYNC`**：此时数据已经由 O_DIRECT 直写设备，InnoDB 不再单独 fsync。
2. **临时表空间（`FIL_TYPE_TEMPORARY`）**：临时数据不需要持久性。

禁用刷盘时，`write_completed()` 走的是另一条分支——直接把文件标记为已刷（`file->set_flushed()`），不挂到未刷列表。

### `space_flush()` 的跳过条件与并发保护

`Fil_shard::space_flush()` 遍历表空间下所有文件时有三层跳过：

**第一层，文件级。** `if (!file->is_open) continue;` 跳过未打开的文件。

**第二层，禁用刷盘时的文件大小检查。** 禁用刷盘时只有在文件大小变化时才需要刷（注释原文：flush should be called if the file size changes to keep OЅ metadata in sync）：

```
if (disable_flush && (file.flush_size == file.size)) {
  continue;
}
```

**第三层，同一文件的并发 flush 互斥。** `space_flush()` 用 `file.n_pending_flushes` 与 `file.sync_event` 防止对同一个文件并发调用 `os_file_flush()`：

```
while (file.n_pending_flushes > 0 && !skip_flush) {
  int64_t sig_count = os_event_reset(file.sync_event);
  mutex_release();
  os_event_wait_low(file.sync_event, sig_count);
  mutex_acquire();
  if (file.flush_counter >= old_mod_counter) {
    skip_flush |= true;
  }
  skip_flush |= is_fast_shutdown();
}
```

源码注释给出的理由是：不想对同一个文件同时调用两次 `os_file_flush()`，因为不确定操作系统在文件 I/O 上是否含有 bug。等完之后如果发现 `file.flush_counter >= old_mod_counter`（说明别人已经刷过了），就直接跳过。

`skip_flush` 初始值是 `is_fast_shutdown()`，Windows 上若 `file.is_raw_disk` 也会置真——即**极快关机（`innodb_fast_shutdown = 2`）与裸磁盘设备上都会跳过刷盘**。

### 停机与快速关机的判定

`is_fast_shutdown()` 的判据是两个条件同时成立：

```
return srv_shutdown_state >= SRV_SHUTDOWN_LAST_PHASE &&
       srv_fast_shutdown >= 2;
```

即服务器已进入停机最后阶段，且 `innodb_fast_shutdown` 取值不小于 2。

## 怎么实现的

### `fil_io()`：薄封装 + 分片路由

`fil_io()` 的函数体很短，它做三件事：

```
dberr_t fil_io(const IORequest &type, bool sync, const page_id_t &page_id,
               const page_size_t &page_size, ulint byte_offset, ulint len,
               void *buf, void *message) {
  auto shard = fil_system->shard_by_id(page_id.space());
  ...
  auto const err = shard->do_io(type, sync, page_id, page_size, byte_offset,
                                len, buf, message);
  ...
  return err;
}
```

1. 按 space id 找到分片；
2. 把请求原样转交 `Fil_shard::do_io()`；
3. 返回错误码。

**同步 / 异步的 IO 责任移交**在 `UNIV_DEBUG` 下有专门的跟踪：异步时 `fil_io()` 立刻把 IO 责任从调用方转走（`release_io_responsibility()`），若 `do_io()` 返回错误且非同步，说明责任实际上没有移交出去，此时 `fil_io()` 把它要回来（`take_io_responsibility()`）。

### `Fil_shard::do_io()`：九步执行序列

这是本页的核心函数。它的执行顺序如下。

**第一步，构造请求并做前置断言。**

```
IORequest req_type(type);
ut_ad(req_type.validate());
ut_a(!req_type.is_log());
ut_ad(len > 0);
ut_ad(byte_offset < UNIV_PAGE_SIZE);
ut_ad(!page_size.is_compressed() || byte_offset == 0);
ut_ad(UNIV_PAGE_SIZE == (ulong)(1 << UNIV_PAGE_SIZE_SHIFT));
```

注意 `ut_a(!req_type.is_log())`——**redo 日志的 I/O 不走这条路径**，redo 由 I01 组的 `log0write.cc` 自己处理。

**第二步，决定 AIO 模式。**

```
auto aio_mode = get_AIO_mode(req_type, sync);
```

`Fil_shard::get_AIO_mode()` 的实现很直接：`sync` 为真返回 `AIO_mode::SYNC`，否则返回 `AIO_mode::NORMAL`。

**第三步，统计与 ibuf 特例。** 读请求累加 `srv_stats.data_read.add(len)`，写请求累加 `srv_stats.data_written.add(len)`。读请求若命中 insert buffer 页面（`ibuf_page()`），且当前不是恢复期（`!recv_no_ibuf_operations`），则把 AIO 模式改为 `AIO_mode::IBUF`，并调用 `req_type.clear_do_not_wake()`——源码注释解释这是为了「降低与 ibuf 相关的死锁 bug 的概率」，不让 ibuf 的 I/O 处理线程睡眠。

**第四步，查找表空间并处理「表空间不存在」。**

```
mutex_acquire();
auto space = get_space_by_id(page_id.space());

if (space == nullptr ||
    (req_type.is_read() && !sync && space->stop_new_ops)) {
  ...
  return DB_TABLESPACE_DELETED;
}
```

**关键规则：表空间正在被删除时（`stop_new_ops`），只禁止异步读，同步读与写仍允许。** 源码注释原文：If we are deleting a tablespace we don't allow async read operations on that. However, we do allow write operations and sync read operations.

这一分支里有三种返回：

- 写请求且页面已失效（`bpage->is_stale()`）→ 返回 `DB_PAGE_IS_STALE`；
- 请求带 `ignore_missing` 标志 → 静默返回 `DB_TABLESPACE_DELETED`，不打印错误；
- 否则打印 `ER_IB_MSG_330`（表空间不存在）或 `ER_IB_MSG_331`（对正在删除的表空间做异步读）。

**第五步，定位文件节点。**

```
fil_node_t *file;
auto page_no = page_id.page_no();
auto err = get_file_for_io(space, &page_no, file);
```

`get_file_for_io()` 内部是 `space->get_file_node(page_no)`，找不到时 `file == nullptr` 且返回 `DB_ERROR`。此时若请求带 `ignore_missing` 就返回 `DB_ERROR` 让调用方决定；否则调 `fil_report_invalid_page_access()`——这是一个硬错误。

**第六步，准备文件（可能现场打开）。**

```
if (!prepare_file_for_io(file)) {
  ...
  if (fsp_is_ibd_tablespace(space->id)) {
    ...
    return DB_TABLESPACE_DELETED;
  }
  ...
  ut_error;
}
```

`prepare_file_for_io()` 失败有三种原因，处理方式各不相同：

- 表空间已删除 → 返回 `DB_TABLESPACE_DELETED`，异步时还会补一次 `buf_page_io_complete(bpage, false)`；
- `.ibd` 文件存在但数据文件缺失（`fsp_is_ibd_tablespace()`）→ 打印 `ER_IB_MSG_332` 后返回 `DB_TABLESPACE_DELETED`；
- 其他情况 → `ut_error`，直接崩溃。

源码注释说明了为什么第三种是致命错误：无法打开文件且不是 IBD 文件（IBD 文件可能在等待期间被删掉），这就是硬故障；并且要求「日志必须在 `prepare_file_for_io()` 内部打印」，因为只有那里知道具体失败原因。

**第七步，边界检查：页号必须在表空间范围内。**

```
if (file->size <= page_no && space->id != TRX_SYS_SPACE) {
  ...
  if (req_type.ignore_missing()) {
    complete_io(file, req_type);
    mutex_release();
    return DB_ERROR;
  }
  fil_report_invalid_page_access(...);
}
```

系统表空间（`TRX_SYS_SPACE`）被显式排除在检查之外。

**第八步，计算物理偏移并设置 I/O 属性。**

```
auto offset = (os_offset_t)page_no * page_size.physical();
offset += byte_offset;
```

即「页号 × 物理页大小 + 页内字节偏移」。压缩页的物理大小与逻辑大小不同，所以必须用 `page_size.physical()`。

随后是四条对齐断言：

```
ut_a(len % OS_FILE_LOG_BLOCK_SIZE == 0);
ut_a(byte_offset % OS_FILE_LOG_BLOCK_SIZE == 0);
```

以及范围断言（`file->size - page_no` 必须足够容纳本次访问）。

打洞（punch hole）与压缩的设置条件：

```
if (req_type.is_write() && !page_size.is_compressed() &&
    page_id.page_no() > 0 && IORequest::is_punch_hole_supported() &&
    file->punch_hole) {
  req_type.set_punch_hole();
  req_type.compression_algorithm(space->compression_type);
} else {
  req_type.clear_compressed();
}
```

**四个条件必须同时成立**才启用打洞：写请求、页未被压缩、页号大于 0（**第 0 页永不压缩**）、平台支持打洞且文件允许打洞。加密信息由 `fil_io_set_encryption()` 设置：

```
if ((space->encryption_op_in_progress == Encryption::Progress::DECRYPTION &&
     req_type.is_write()) ||
    !space->can_encrypt() || page_id.page_no() == 0) {
  req_type.clear_encrypted();
  return;
}
```

即**解密进行中的写请求、表空间不可加密、第 0 页**这三种情况都不加密。undo 表空间另有条件：写 undo 日志且 `srv_undo_log_encrypt` 为假时跳过加密。

**第九步，调用 OS 层并处理完成。**

```
err = os_aio(
    req_type, aio_mode, file->name, file->handle, buf, offset, len,
    fsp_is_system_temporary(page_id.space()) ? false : srv_read_only_mode,
    file, message);
```

`UNIV_HOTBACKUP`（mysqlbackup）编译分支下不走 `os_aio()`，而是直接调 `os_file_read()` / `os_file_write()`——源码注释说明在 mysqlbackup 里做普通 I/O 而非 AIO。

打洞失败的处理：

```
if (err == DB_IO_NO_PUNCH_HOLE) {
  err = DB_SUCCESS;
  if (file->punch_hole) {
    ib::warn(ER_IB_MSG_333) << "Punch hole failed for '" << file->name << "'";
  }
  fil_no_punch_hole(file);
}
```

**打洞失败被降级为成功**，但会打印警告 `ER_IB_MSG_333`，并调 `fil_no_punch_hole(file)` 永久关闭该文件的打洞能力（`fil_no_punch_hole()` 的实现就是 `file->punch_hole = false;`）。

最后，同步请求当场完成：

```
if (sync) {
  mutex_acquire();
  complete_io(file, req_type);
  mutex_release();
}
```

`Fil_shard::complete_io()` 递减 `n_pending_ios`，写请求则调 `write_completed()`。

### `fil_flush()`：入口只做加锁

```
void fil_flush(space_id_t space_id) {
  auto shard = fil_system->shard_by_id(space_id);
  shard->mutex_acquire();
  /* Note: Will release and reacquire the Fil_shard::mutex. */
  shard->space_flush(space_id);
  shard->mutex_release();
}
```

`fil_flush()` 本身极薄，真正的逻辑在 `Fil_shard::space_flush()`，它要持有分片锁（`ut_ad(mutex_owned())`）进入，并在内部释放再重新获取（等 `file.sync_event` 时）。

### `Fil_shard::space_flush()`：逐文件刷盘

`space_flush()` 的完整流程：

**第一步，取表空间并早退。**

```
fil_space_t *space = get_space_by_id(space_id);

if (space == nullptr || space->purpose == FIL_TYPE_TEMPORARY ||
    space->stop_new_ops) {
  return;
}
```

**第二步，判断是否禁用刷盘。** `fil_disable_space_flushing(space)`。

禁用刷盘时，只在文件大小变化时才刷（保持文件系统元数据同步），全部文件大小都没变就直接返回。

**第三步，增加表空间级待刷计数。** `++space->n_pending_flushes;`，防止刷盘期间表空间被丢弃。

**第四步，遍历文件并逐个刷。** 对每个已打开的文件：

- 记录 `old_mod_counter = file.modification_counter`；
- 若禁用刷盘且 `file.flush_size == file.size`，跳过；
- 若未禁用刷盘且 `old_mod_counter <= file.flush_counter`，跳过；
- 对 `FIL_TYPE_TABLESPACE` / `FIL_TYPE_IMPORT` 类型的空间，递增 `fil_n_pending_tablespace_flushes`。

**第五步，等待同文件的并发 flush 结束**（前述 `file.n_pending_flushes` / `file.sync_event` 循环）。

**第六步，真正刷盘。**

```
if (!skip_flush) {
  ut_a(file.is_open);
  ++file.n_pending_flushes;
  mutex_release();
  os_file_flush(file.handle);
  file.flush_size = file.size;
  mutex_acquire();
  os_event_set(file.sync_event);
  --file.n_pending_flushes;
}
```

**注意：刷盘期间分片锁被释放**（`mutex_release()` / `mutex_acquire()`），所以 `os_file_flush()` 不在锁内执行。

**第七步，推进 flush 计数并摘除未刷列表。**

```
if (file.flush_counter < old_mod_counter) {
  file.flush_counter = old_mod_counter;
  remove_from_unflushed_list(space);
}
```

### `fil_flush_file_spaces()`：批量刷所有表空间

`fil_flush_file_spaces()` 是 `fil_flush()` 的批量版本，两层委托：

```
void fil_flush_file_spaces() { fil_system->flush_file_spaces(); }
```

`Fil_system::flush_file_spaces()` 遍历所有分片调 `Fil_shard::flush_file_spaces()`。后者先收集未刷列表中符合条件的空间 id：

```
for (auto space : m_unflushed_spaces) {
  if ((to_int(space->purpose) & FIL_TYPE_TABLESPACE) &&
      !space->stop_new_ops) {
    space_ids.push_back(space->id);
  }
}
```

**只收集 `FIL_TYPE_TABLESPACE` 且未在删除中的表空间**，然后逐个调 `space_flush(space_id)`。源码注释说明：对不存在的 space id 调 `fil_flush()` 不会有害，所以这里不需要额外校验。

### 真实调用方

`fil_io()` 的调用方（均已读过函数体）：

| 调用方 | 场景 | 关键参数 |
|---|---|---|
| `fil_read()`（`fil0fil.cc` 内的静态辅助函数） | 内部读表空间页 | `fil_io(IORequestRead, true, ...)`，`message` 传 `nullptr` |
| `fil_write()`（`fil0fil.cc` 内的静态辅助函数） | 内部写表空间页 | `fil_io(IORequestWrite, true, ...)`，前置断言 `ut_ad(!srv_read_only_mode)` |
| `buf_read_page_low()`（`buf0rea.cc`） | 缓冲池读页 | `sync` 由调用方决定；失败时按 `IORequest::ignore_missing(type)` 或 `DB_TABLESPACE_DELETED` 走 `buf_read_page_handle_error(bpage)` |
| `Double_write::single_write()`（`buf0dblwr.cc`） | 写 doublewrite 文件 | 断言结果只能是 `DB_SUCCESS`、`DB_TABLESPACE_DELETED` 或 `DB_PAGE_IS_STALE` |
| `recv_recover_page()`（`log0recv.cc`） | 崩溃恢复读页 / 写回页 | 读写都用 `sync = true`；失败 `ib::fatal(ER_IB_MSG_712)` / `ib::fatal(ER_IB_MSG_713)` |

`fil_flush()` / `fil_flush_file_spaces()` 的调用方：

| 调用方 | 场景 |
|---|---|
| `buf_flush_sync_datafiles()`（`buf0flu.cc`） | 先 `os_aio_wait_until_no_pending_writes()` 等所有异步写提交给 OS，再 `fil_flush_file_spaces()` 落盘 |
| `buf_LRU_flush_or_remove_pages()` 的 `BUF_REMOVE_FLUSH_WRITE` 分支（`buf0lru.cc`） | 先 `os_aio_wait_until_no_pending_writes()`，再 `fil_flush(id)` 刷单个表空间 |
| `Double_write` 相关路径（`buf0dblwr.cc`） | `fil_flush(bpage->id.space())` 刷单空间，`fil_flush_file_spaces()` 刷全部 |
| `fil0fil.cc` 内的 redo 应用路径 | 表空间扩展后 `fil_flush(space->id)` 再 `fil_space_close(space->id)` |

### 异步完成路径：`fil_aio_wait()`

异步 I/O 的完成由 `fil_aio_wait(ulint segment)` 处理：它调 `os_aio_handler(segment, &m1, &m2, &type)` 取出已完成请求，然后拿分片锁调 `Fil_shard::complete_io(file, type)`。

完成后的处理按表空间类型分派：

```
switch (file->space->purpose) {
  case FIL_TYPE_IMPORT:
  case FIL_TYPE_TEMPORARY:
  case FIL_TYPE_TABLESPACE:
    srv_set_io_thread_op_info(segment, "complete io for buf page");
    if (m2 != nullptr) {
      auto bpage = static_cast<buf_page_t *>(m2);
      ut_d(bpage->take_io_responsibility());
      buf_page_io_complete(bpage, false);
    }
    return;
}
```

源码注释再次强调了死锁风险：读完成处理会连带读取系统表空间 0 里的 insert buffer，所以系统表空间的数据文件必须一直保持打开，并用专门的 I/O 线程服务 insert buffer 请求。

## 关键取舍

### 用「修改计数」代替「脏标记」来决定刷盘

InnoDB 没有用「这个文件脏了」这样的布尔标记，而是用 `modification_counter` / `flush_counter` 两个单调递增的整数比较。**取舍：多占几个字节的成员空间，换来「自上次刷盘以来没变过就完全不 fsync」的精确节流。** 判定逻辑在 `space_flush()` 里是 `old_mod_counter <= file.flush_counter` 时 `continue`。

### 表空间被删除时，只禁异步读

`do_io()` 的分支条件是 `space == nullptr || (req_type.is_read() && !sync && space->stop_new_ops)`。**取舍：删除期间牺牲一部分异步读的并发度，换来写路径与同步读路径不被删除流程阻塞。** 源码注释明确写了这个取舍：写操作与同步读操作都允许。

### 打洞失败降级为成功，而非报错

`DB_IO_NO_PUNCH_HOLE` 被改写成 `DB_SUCCESS`，代价是打洞带来的空间回收收益永久丢失（`fil_no_punch_hole(file)` 关掉该文件的打洞能力），收益是打洞不被支持的文件系统上 InnoDB 仍能正常工作。

### 「同步完成」在锁外做，「元数据」在锁内做

`do_io()` 在 `os_aio()` 之前 `mutex_release()`，在 `os_aio()` 之后若为同步请求则重新加锁调 `complete_io()`。**取舍：真正的系统调用不在分片锁内，锁内只做元数据维护，因此锁持有时间与磁盘延迟无关。** 代价是 `complete_io()` 需要重新加锁，而此时表空间可能已被删除——所以代码里有多处 `space->stop_new_ops` / `is_deleted()` 的复查。

### 刷盘期间释放分片锁

`space_flush()` 在 `os_file_flush(file.handle)` 前后做 `mutex_release()` / `mutex_acquire()`。**取舍：fsync 的延迟不阻塞同一分片上的其他 I/O 元数据操作，代价是刷盘期间表空间可能被并发摘除，所以用 `space->n_pending_flushes` 与 `file.n_pending_flushes` 两层计数兜底。**

### 极快关机与裸磁盘上跳过刷盘

`is_fast_shutdown()`（`srv_shutdown_state >= SRV_SHUTDOWN_LAST_PHASE && srv_fast_shutdown >= 2`）为真时 `skip_flush` 初始就是真；Windows 上 `file.is_raw_disk` 也置真。**取舍：牺牲关机时的数据落盘保证换取关机速度——这是 `innodb_fast_shutdown = 2` 的语义。**

### 禁用刷盘时仍要同步文件大小

`disable_flush` 为真时，`space_flush()` 仍然会因为 `file.flush_size != file.size` 而执行一次 `os_file_flush()`。源码注释给出的理由是：保持 OЅ metadata 与实际文件大小同步。**取舍：即使明确关闭 fsync 也不放弃元数据一致性。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/fil/fil0fil.cc | `fil_io` | 读写统一入口：按 `page_id.space()` 定位 `Fil_shard` 后转交 `do_io()`；`UNIV_DEBUG` 下跟踪异步 IO 责任的移交与回滚 |
| storage/innobase/fil/fil0fil.cc | `fil_flush` | 单表空间刷盘入口：取分片、加分片锁、调 `Fil_shard::space_flush()` |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::do_io` | 核心执行序列：断言、AIO 模式判定、统计、查表空间、定位文件节点、准备文件、边界检查、算偏移与设压缩/加密/打洞、`os_aio()`、同步完成 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::space_flush` | 逐文件刷盘：三层跳过条件、并发 flush 互斥等待、`os_file_flush()`、推进 `flush_counter`、摘除未刷列表 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::complete_io` | I/O 完成后的元数据维护：递减 `n_pending_ios`、写请求调 `write_completed`、归零时放回 LRU |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::write_completed` | 写完成后推进 `m_modification_counter`；禁用刷盘时直接标记已刷，否则挂到未刷列表 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::prepare_file_for_io` | 现场打开文件（必要时）、从 LRU 摘除、`n_pending_ios` 自增 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::open_file` | 打开文件并抢占两个配额：`m_open_files_limit` 与 `m_n_files_not_belonging_in_lru`；失败时释放已占配额 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::get_AIO_mode` | 同步返回 `AIO_mode::SYNC`，异步返回 `AIO_mode::NORMAL`；hotbackup 编译下强制同步 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::get_file_for_io` | 由页号定位文件节点，找不到返回 `DB_ERROR` 且 `file == nullptr` |
| storage/innobase/fil/fil0fil.cc | `fil_io_set_encryption` | 设置加密信息：解密中写请求、不可加密表空间、第 0 页、undo 表空间关闭加密这四种情况都不加密 |
| storage/innobase/fil/fil0fil.cc | `fil_disable_space_flushing` | 判定是否禁用刷盘：Unix 上 `O_DIRECT_NO_FSYNC` 模式，或临时表空间 |
| storage/innobase/fil/fil0fil.cc | `is_fast_shutdown` | 判定极快关机：`srv_shutdown_state >= SRV_SHUTDOWN_LAST_PHASE` 且 `srv_fast_shutdown >= 2` |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::remove_from_unflushed_list` | 确认表空间整体已刷干净才从未刷链表摘除 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::flush_file_spaces` | 收集未刷列表中 `FIL_TYPE_TABLESPACE` 且未在删除中的空间 id，逐个 `space_flush` |
| storage/innobase/fil/fil0fil.cc | `Fil_system::flush_file_spaces` | 遍历所有分片调 `Fil_shard::flush_file_spaces()` |
| storage/innobase/fil/fil0fil.cc | `fil_flush_file_spaces` | 批量刷盘入口：`fil_system->flush_file_spaces()` |
| storage/innobase/fil/fil0fil.cc | `fil_read` | 内部读辅助：固定 `sync = true` 的 `fil_io(IORequestRead, ...)` |
| storage/innobase/fil/fil0fil.cc | `fil_write` | 内部写辅助：固定 `sync = true` 的 `fil_io(IORequestWrite, ...)`，断言非只读模式 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::complete_io` 之外的 `fil_aio_wait` | 异步 I/O 完成处理：`os_aio_handler` 取完成请求，`complete_io` 维护元数据，按表空间类型分派完成回调 |
| storage/innobase/fil/fil0fil.cc | `fil_no_punch_hole` | 关闭文件的打洞能力（`file->punch_hole = false`） |
| storage/innobase/fil/fil0fil.cc | `Fil_system::space_belongs_in_LRU` | 判定表空间文件是否参与 LRU：系统表空间与 undo 表空间不参与 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::add_to_lru_if_needed` | I/O 全部完成后把文件放回 LRU 链表头 |
| storage/innobase/fil/fil0fil.cc | `Fil_shard::remove_from_LRU` | 有新 I/O 发起时把文件从 LRU 链表摘除 |
| storage/innobase/fil/fil0fil.cc | `fil_report_invalid_page_access_low` | 页号越界时的硬错误报告：打印 `ER_IB_MSG_328` 后 `ut_error` |
| storage/innobase/buf/buf0rea.cc | `buf_read_page_low` | `fil_io()` 的调用方：缓冲池读页，按 `IORequest::ignore_missing` 或 `DB_TABLESPACE_DELETED` 决定是否走 `buf_read_page_handle_error` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_sync_datafiles` | `fil_flush_file_spaces()` 的调用方：先等所有异步写提交给 OS，再批量落盘 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_flush_or_remove_pages` | `fil_flush(id)` 的调用方：`BUF_REMOVE_FLUSH_WRITE` 分支中等异步写结束后刷单个表空间 |
| storage/innobase/buf/buf0dblwr.cc | `Double_write::single_write` | `fil_io()` 的调用方：写 doublewrite 文件，接受 `DB_SUCCESS` / `DB_TABLESPACE_DELETED` / `DB_PAGE_IS_STALE` 三种结果 |
| storage/innobase/log/log0recv.cc | `recv_recover_page` | `fil_io()` 的调用方：崩溃恢复读页并写回，读写都 `sync = true`，失败即 `ib::fatal` |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己的存储层 WAL 与 pager，与 InnoDB 的 `fil0fil.cc` 没有代码级关联，但**机制形态高度同构**，可作对照：

- **两者都把「写进文件」与「刷到盘」分成两级**：主核 WAL 的写入与刷盘也是分开推进的，本页 `fil_io()` 与 `fil_flush()` 是一对职责分离的入口，对应 InnoDB 侧 `fil_io()`（交给 OS）与 `fil_flush()`（`os_file_flush`）的分界。
- **两者都有「未刷列表」概念**：主核需要知道哪些数据已写入但未落盘，本页 `Fil_shard::m_unflushed_spaces` 与 `write_completed()` 的 `add_to_unflushed_list()` 承担同样职责。
- **两者都用「计数比较」代替布尔脏标记**来决定刷盘范围，本页是 `modification_counter` 与 `flush_counter` 的比较。

**架构推论：** 主核侧可以借鉴本页的两点设计——(1) 把真正的系统调用（`os_aio()` / `os_file_flush()`）放在锁外、锁内只做元数据维护，避免 fsync 延迟阻塞其他操作；(2) 用「自上次刷盘以来是否有新修改」的计数比较来跳过无效 fsync，而不是靠脏标记位粗略判断。

**架构推论：** 本页的 `fil_io()` / `fil_flush()` 二分，以及 `Fil_shard` 的分片元数据结构，对主核 pager 的接口设计有参考价值——但主核是否已有等价实现、以及是否采用同样的分片粒度，本页不作断言，需另行核对主核源码。

## 参考

- [storage/innobase/fil/fil0fil.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/fil/fil0fil.cc) —— 本页全部主符号（`fil_io`、`fil_flush`、`Fil_shard::do_io`、`Fil_shard::space_flush` 等）的权威来源。
- [File-Per-Table Tablespaces（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-file-per-table-tablespaces.html) —— `.ibd` 数据文件的组织方式与 `innodb_flush_method = O_DIRECT` 对并发写的影响，是本页「文件按表空间分片管理」与「打洞 / 空间回收」机制的上位背景。
- [Doublewrite Buffer（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-doublewrite-buffer.html) —— doublewrite 文件的落盘流程与「单次 fsync」的官方说明，对应本页 `Double_write::single_write()` 调 `fil_io()` 的写入路径。
- [Redo Log（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-redo-log.html) —— redo 日志文件与 LSN 的官方定义；是本页 `do_io()` 中 `ut_a(!req_type.is_log())` 这条「redo 不走通用文件 I/O 路径」断言的对照背景。
