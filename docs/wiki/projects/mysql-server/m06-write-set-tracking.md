---
project: mysql-server
feature: write-set-tracking
page_type: feature
schema_version: 1
refs:
  - path: sql/rpl_trx_tracking.cc
    symbols: [Writeset_trx_dependency_tracker::get_dependency, Transaction_dependency_tracker::step]
---

# 写集跟踪

## 一句话职责

写集跟踪负责**为每个事务算出它的「提交父依赖」（commit parent）**：事务写入过的每一行会被折算成一个 64 位哈希，这些哈希构成该事务的写集（write set）；写集跟踪器把写集与一份「行哈希 → 最近写它的序号」的历史表比对，算出该事务真正依赖的最早那个前驱事务的序号，从而让并行复制可以放行那些**写集互不重叠**的事务。核心实现落在 `sql/rpl_trx_tracking.cc` 的 `Writeset_trx_dependency_tracker::get_dependency`；`Transaction_dependency_tracker::step` 则是为事务分配全局单调递增序号（sequence number）的那一环。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责记录事务写过的行/表以便并行复制判定依赖；**不负责并行复制的调度（M02）**、**不负责 binlog 事件内容（L 组）**。

## 机制是什么

### 写集跟踪要解决的问题

MySQL 的并行复制（MTS）里，每个事务被分配一个单调递增的**序号**（sequence number）。如果事务 T 的提交序号是 100，那么按提交顺序排队时，T 必须等所有序号小于 100 的事务提交完——这是 COMMIT_ORDER 语义，代价是无冲突的事务也必须排队。

写集跟踪的思路是：**只要两个事务没有写同一行，它们之间就没有依赖**。于是可以给 T 算出一个更小的「提交父」——T 只需要等它真正冲突的那些前驱，不必等所有前驱。这个提交父会被写进 binlog 的 GTID 事件里，从库据此决定一个事务能否与其它事务并发执行。

### 三个跟踪器叠在一起

`sql/rpl_trx_tracking.cc` 里实际上有三个跟踪器，`Transaction_dependency_tracker::get_dependency` 按运行模式把它们串起来：

| 跟踪器 | 类 | 职责 |
|---|---|---|
| 提交序跟踪器 | `Commit_order_trx_dependency_tracker` | 分配序号、算「按提交顺序」的父依赖、并识别必须阻塞后续事务的事务 |
| 写集跟踪器 | `Writeset_trx_dependency_tracker` | 用行哈希历史表把父依赖压低到真正冲突的前驱 |
| 会话依赖跟踪器 | `Writeset_session_trx_dependency_tracker` | 保证**同一会话内**的事务按顺序执行（同一 session 的事务天然互相依赖） |

`Transaction_dependency_tracker::get_dependency` 的分派逻辑是逐级叠加的：

```cpp
switch (m_opt_tracking_mode) {
  case DEPENDENCY_TRACKING_COMMIT_ORDER:
    m_commit_order.get_dependency(thd, parallelization_barrier,
                                  sequence_number, commit_parent);
    break;
  case DEPENDENCY_TRACKING_WRITESET:
    m_commit_order.get_dependency(thd, parallelization_barrier,
                                  sequence_number, commit_parent);
    m_writeset.get_dependency(thd, sequence_number, commit_parent);
    break;
  case DEPENDENCY_TRACKING_WRITESET_SESSION:
    m_commit_order.get_dependency(thd, parallelization_barrier,
                                  sequence_number, commit_parent);
    m_writeset.get_dependency(thd, sequence_number, commit_parent);
    m_writeset_session.get_dependency(thd, sequence_number, commit_parent);
    break;
  default:
    assert(0);  // blow up on debug
    /*
      Fallback to commit order on production builds.
     */
    m_commit_order.get_dependency(thd, parallelization_barrier,
                                  sequence_number, commit_parent);
}
```

三点要注意：

- **写集模式永远建立在提交序之上**：`DEPENDENCY_TRACKING_WRITESET` 分支先跑 `m_commit_order.get_dependency`，写集跟踪器拿到的是提交序算出来的父依赖作为**回退值**，只会把它**压低**、不会抬高。
- **`default` 分支是真实存在的回退路径**：非 debug 构建下若模式值超出上述三个枚举，`assert(0)` 不生效，函数退回 `m_commit_order.get_dependency`（调用方为 `Transaction_dependency_tracker::get_dependency`），即退化成纯提交序。
- **三层是递进收紧**：会话依赖跟踪器最后跑，用 `std::max` 收紧父依赖——即会话约束可以把父依赖**推高**到必须等待同会话前一个事务。

### 写集的产生：主键等价键（PKE）与哈希

写集不是「表的粒度」而是「行的粒度」。`sql/rpl_write_set_handler.cc` 的 `add_pke` 负责为每一行被修改的记录抽取**主键等价键（Primary Key Equivalent）**，再哈希成 64 位值。

`add_pke` 的注释把规则写清楚了：主键永远存在 `key_part[0]` 里，所以可以直接从 `table->s->keys` 读取；除了主键，还要抽取**唯一键**的值，因为唯一键约束被破坏的位置也是冲突点。

拼装出的字符串（源码里叫 `pke`）由三段用分隔符拼成，分隔符是 `HASH_STRING_SEPARATOR`，其定义是 `#define HASH_STRING_SEPARATOR "½"`：

1. **索引名**：`pke.append(table->key_info[key_number].name)`，主键就叫 `PRIMARY`。
2. **库名 + 长度**：先 `pke_schema_table.append(HASH_STRING_SEPARATOR)` 起头，再拼 `table->s->db.str` 与 `std::to_string(table->s->db.length)`。
3. **表名 + 长度**：拼 `table->s->table_name.str` 与 `std::to_string(table->s->table_name.length)`。

每个键列的值经过 `field->make_sort_key` 规范化（注释说明是「转换成规范化字符串，这样就能用 memcmp 之类的二进制比较函数排序」），规范化后的字节与 `std::to_string(length)` 一起拼进 `pke`。

只有**唯一索引**参与：`if (!((table->key_info[key_number].flags & (HA_NOSAME)) == HA_NOSAME)) continue;`——非唯一索引直接跳过。注释里的例子说明了这个设计的边界：

```
CREATE TABLE t1 (c1 INT PRIMARY KEY, c2 INT, KEY(c2));
CREATE TABLE t2 (x1 INT PRIMARY KEY, x2 INT,
                 FOREIGN KEY (x2) REFERENCES t1(c2));

DELETE FROM t1 WHERE c1=1;
  does generate the PKEs:
    PRIMARY½test½4t1½21½1

INSERT INTO t2 VALUES (1,1);
  does generate the PKEs:
    PRIMARY½test½4t2½21½1
```

即 `c2` 上没有唯一约束，所以外键引用它无法用来判定冲突，该外键被跳过。

哈希算法由 `calc_hash` 实现，两个分支：

```cpp
template <class type>
uint64 calc_hash(ulong algorithm, type T, size_t len) {
  if (algorithm == HASH_ALGORITHM_MURMUR32)
    return (murmur3_32((const uchar *)T, len, 0));
  else
    return (MY_XXH64((const uchar *)T, len, 0));
}
```

算法名字表是 `transaction_write_set_hashing_algorithms[] = {"OFF", "MURMUR32", "XXHASH64", nullptr}`，三种取值对应 `HASH_ALGORITHM_OFF` / `HASH_ALGORITHM_MURMUR32` / `HASH_ALGORITHM_XXHASH64`。`get_write_set_algorithm_string` 把这三个枚举映射成字符串，未知值返回 `"UNKNOWN ALGORITHM"`。

### 两种「不能只看行」的例外

写集跟踪在两种情况下**不能只靠行哈希**判定冲突，源码为这两种情况各设了一个标志位：

- **缺主键（missing keys）**：表没有主键或唯一键，行无法被唯一标识。`sql/rpl_write_set_handler.cc` 的 `add_pke` 末尾：`if (!writeset_hashes_added) ws_ctx->set_has_missing_keys();`
- **涉及外键（related foreign keys）**：外键会把行改动传播到别的表。`add_pke` 中：`if (table->s->foreign_key_parents > 0) ws_ctx->set_has_related_foreign_keys();`

这两个标志由 `sql/rpl_transaction_write_set_ctx.cc` 的 `Rpl_transaction_write_set_ctx` 持有，读取入口是 `get_has_missing_keys()` 与 `get_has_related_foreign_keys()`。

## 怎么实现的

### 第一步：行被修改时抽取写集

抽取的触发点在 `sql/handler.cc` 的 `binlog_log_row`。判据是 `thd->variables.transaction_write_set_extraction != HASH_ALGORITHM_OFF`，成立时对 `after_record` 与 `before_record` 两个记录分别调 `add_pke`：

```cpp
std::array<const uchar *, 2> records{after_record, before_record};
for (auto rec : records) {
  if (rec != nullptr) {
    assert(rec == table->record[0] || rec == table->record[1]);
    bool res = add_pke(table, thd, rec);
    if (res) return HA_ERR_RBR_LOGGING_FAILED;
  }
}
```

即 **UPDATE 事件的改前镜像与改后镜像都会进入写集**——因为唯一键约束可能被改前值破坏，也可能被改后值破坏。`add_pke` 返回真表示出错，函数返回 `HA_ERR_RBR_LOGGING_FAILED`；`add_pke` 内部抛 `std::bad_alloc` 时也被外层的 `catch (const std::bad_alloc &)` 捕获，转成 `ER_OUT_OF_RESOURCES` 错误并返回同一个错误码。

写集本身存在 `Rpl_transaction_write_set_ctx` 的 `write_set`（`std::vector<uint64>`）里。写入入口是 `Rpl_transaction_write_set_ctx::add_write_set`，函数体有三道闸门：

1. **写集容量上限**：判据是 `write_set.size() >= binlog_trx_dependency_history_size`，其中 `binlog_trx_dependency_history_size` 取自 `mysql_bin_log.m_dependency_tracker.get_writeset()->m_opt_max_history_size`——**即写集历史表容量的同一个常量**。触顶时置 `m_local_has_reached_write_set_limit = true`、调 `clear_write_set()` 清空写集并返回 `false`。
2. **全局内存上限**：`m_global_write_set_memory_size_limit` 非零且未被 `m_ignore_write_set_memory_limit` 忽略时，判据是 `sizeof(uint64) + write_set_memory_size() > mem_limit`，触顶则报 `ER_WRITE_SET_EXCEEDS_LIMIT` 并返回 `true`。
3. **全局要求完整写集**：`m_global_component_requires_write_sets && !m_local_allow_drop_write_set` 时，容量上限不再生效（不丢弃写集）。

`write_set_memory_size()` 的定义是 `sizeof(uint64) * write_set.size()`。

构造时预分配 12 个元素，注释给了理由：「为了加速小事务的写集抽取，我们预分配 12 个元素。12 这个数量足以容纳单语句事务的写集，即使表上有外键。」

### 第二步：保存点与回滚对写集的裁剪

写集必须随事务回滚一起回退，否则回滚掉的行仍会被当作「写过」。`sql/rpl_transaction_write_set_ctx.cc` 用保存点栈实现这件事：

- `add_savepoint(name)`：同名的旧保存点先被删除，然后记下 `write_set.size()` 作为位置。
- `rollback_to_savepoint(name)`：把 `write_set` 从该位置起的元素全部 `erase`，并顺带删掉位置在它之后的保存点。注释列出的前置条件检查是 `write_set.size() > 0 && position < write_set.size()`。
- `del_savepoint(name)` / `reset_savepoint_list()` / `restore_savepoint_list()`：分别负责删除单个保存点、把当前保存点表压栈、把保存点表出栈恢复。

### 第三步：算提交父依赖——`Writeset_trx_dependency_tracker::get_dependency`

这是本页的核心函数。它的输入输出约定写在注释里：`commit_parent` 是**入参兼出参**，进来时已经由 `Commit_order_trx_dependency_tracker` 填好了提交序父依赖，本函数只会在写集可信时把它**压低**。

函数体分三段。

**第一段：判断本事务能不能用写集。** 判据 `can_use_writesets` 由四个条件与运算而成：

```cpp
bool can_use_writesets =
    // empty writeset implies DDL or similar, except if there are missing keys
    (writeset->size() != 0 || write_set_ctx->get_has_missing_keys() ||
     /*
       The empty transactions do not need to clear the writeset history, since
       they can be executed in parallel.
     */
     is_empty_transaction_in_binlog_cache(thd)) &&
    // hashing algorithm for the session must be the same as used by other
    // rows in history
    (global_system_variables.transaction_write_set_extraction ==
     thd->variables.transaction_write_set_extraction) &&
    // must not use foreign keys
    !write_set_ctx->get_has_related_foreign_keys() &&
    // it did not broke past the capacity already
    !write_set_ctx->was_write_set_limit_reached();
```

逐条拆解：

- **写集非空**，或者**有缺主键的表**，或者**本身是空事务**（`is_empty_transaction_in_binlog_cache(thd)`）——前两者说明「这是个有写入的事务」，第三者说明「这是个可以并行执行的空事务」，两者都不成立时说明是 DDL 之类没有行写入的事务，写集对它没有信息量。
- **会话级哈希算法必须与全局一致**：`global_system_variables.transaction_write_set_extraction == thd->variables.transaction_write_set_extraction`。注释解释了原因：「会话的哈希算法必须与历史中其它行使用的算法相同」——历史表里存的是用全局算法算出的哈希，会话换了算法，算出的哈希与历史里的对不上，冲突检测会失效。
- **不得涉及外键**：`!write_set_ctx->get_has_related_foreign_keys()`。
- **此前没有触过容量上限**：`!write_set_ctx->was_write_set_limit_reached()`。

**第二段：查历史表，算冲突父依赖。** 先判容量：

```cpp
exceeds_capacity =
    m_writeset_history.size() + writeset->size() > m_opt_max_history_size;
```

`m_opt_max_history_size` 就是上一节里 `add_write_set` 用来做容量判据的同一个常量——**写集历史表的容量上限**。注意比较用的是 `>` 而不是 `>=`：写集条数恰好等于上限时不超限。

然后遍历本事务的每一个行哈希，查历史表 `m_writeset_history`（哈希 → 最近一次写它的序号）：

```cpp
int64 last_parent = m_writeset_history_start;
for (std::vector<uint64>::iterator it = writeset->begin();
     it != writeset->end(); ++it) {
  Writeset_history::iterator hst = m_writeset_history.find(*it);
  if (hst != m_writeset_history.end()) {
    if (hst->second > last_parent && hst->second < sequence_number)
      last_parent = hst->second;

    hst->second = sequence_number;
  } else {
    if (!exceeds_capacity)
      m_writeset_history.insert(
          std::pair<uint64, int64>(*it, sequence_number));
  }
}
```

这个循环做三件事：

1. **取冲突父依赖**：若哈希命中历史表，且历史里记的序号 `hst->second` 大于当前候选 `last_parent` 且小于本事务序号 `sequence_number`，就把 `last_parent` 抬到它。**`hst->second < sequence_number` 这个条件很关键**——它排除了「自己」以及更晚的事务，保证父依赖严格早于本事务。
2. **更新历史**：命中项的序号被改写成 `sequence_number`——**历史表记的永远是「最近一次写这行的序号」**。
3. **插入新项**：未命中的哈希在**未超容量**时才插入。若已超容量，新哈希不进历史表（但本事务自身的冲突检测仍然做了，因为查找那一步已经完成）。

**第三段：压低父依赖，并处理降级。** 缺主键的事务不能压低父依赖：

```cpp
if (!write_set_ctx->get_has_missing_keys()) {
  /*
   The WRITESET commit_parent then becomes the minimum of largest parent
   found using the hashes of the row touched by the transaction and the
   commit parent calculated with COMMIT_ORDER.
  */
  commit_parent = std::min(last_parent, commit_parent);
}
```

注释说明了「有缺主键的表时回退到 COMMIT_ORDER，但**更新而不重置**历史」的理由：任何引用这张表的事务同样会回退到 COMMIT_ORDER，所以历史没必要重置。

降级与清空：

```cpp
if (exceeds_capacity || !can_use_writesets) {
  m_writeset_history_start = sequence_number;
  m_writeset_history.clear();
}
```

**两种情形会清空整个历史表并把 `m_writeset_history_start` 推到当前序号**：超容量，或本事务根本用不了写集。源码注释把理由写清楚了：「如果发生这些情况，就退回使用 COMMIT_ORDER 并清空历史以保持数据一致性。」

`m_writeset_history_start` 是历史表的**水位线**——它的初始值就是 `last_parent` 的起点，表示「历史表里没有比这个序号更早的信息」。

### 第四步：写集不可信时的回退与降级调用方

当写集不可信时，父依赖保持为提交序算出的值——**回退到 `Commit_order_trx_dependency_tracker::get_dependency`**，它正是 `Transaction_dependency_tracker::get_dependency` 在 `DEPENDENCY_TRACKING_WRITESET` / `DEPENDENCY_TRACKING_WRITESET_SESSION` 分支里**先调用**的那一个，其返回值就是写集跟踪器的入参初值。因此「写集不可信」不是一条独立的降级代码路径，而是「保留上游算出的值」。

启动期还有一道硬校验，在 `sql/mysqld.cc`：

```cpp
if (global_system_variables.transaction_write_set_extraction ==
        HASH_ALGORITHM_OFF &&
    mysql_bin_log.m_dependency_tracker.m_opt_tracking_mode !=
        DEPENDENCY_TRACKING_COMMIT_ORDER) {
  LogErr(ERROR_LEVEL,
         ER_TX_EXTRACTION_ALGORITHM_FOR_BINLOG_TX_DEPEDENCY_TRACKING,
         "XXHASH64 or MURMUR32", "WRITESET or WRITESET_SESSION");
  return 1;
} else
  mysql_bin_log.m_dependency_tracker.tracking_mode_changed();
```

即：**抽取算法为 OFF 时不允许用写集跟踪模式**，否则启动失败（返回 1，错误号 `ER_TX_EXTRACTION_ALGORITHM_FOR_BINLOG_TX_DEPEDENCY_TRACKING`）。校验通过则调 `Transaction_dependency_tracker::tracking_mode_changed()`。

`tracking_mode_changed` 的作用是把写集历史表清空并把水位线对到当前最大已提交时间戳：

```cpp
void Transaction_dependency_tracker::tracking_mode_changed() {
  Logical_clock max_committed_transaction =
      m_commit_order.get_max_committed_transaction();
  int64 timestamp = max_committed_transaction.get_timestamp() -
                    max_committed_transaction.get_offset();

  m_writeset.rotate(timestamp);
}
```

`m_writeset.rotate(int64 start)` 就是 `m_writeset_history_start = start;` 加 `m_writeset_history.clear();`。

### 第五步：会话级依赖——`Writeset_session_trx_dependency_tracker::get_dependency`

同一 session 内的事务必须串行执行，这由会话依赖跟踪器保证：

```cpp
void Writeset_session_trx_dependency_tracker::get_dependency(
    THD *thd, int64 &sequence_number, int64 &commit_parent) {
  int64 session_parent = thd->rpl_thd_ctx.dependency_tracker_ctx()
                             .get_last_session_sequence_number();

  if (session_parent != 0 && session_parent < sequence_number)
    commit_parent = std::max(commit_parent, session_parent);

  thd->rpl_thd_ctx.dependency_tracker_ctx().set_last_session_sequence_number(
      sequence_number);
}
```

逻辑是：取出同会话上一个事务的序号 `session_parent`，若它非 0 且小于本事务序号，就把父依赖**抬高**到 `max(commit_parent, session_parent)`——**注意是 `std::max` 而不是 `std::min`，方向与写集跟踪器相反**。写集跟踪器负责「压低」父依赖以争取并行度，会话跟踪器负责「抬高」父依赖以保证同会话顺序，两者都作用在同一个 `commit_parent` 上。

会话序号存在 THD 私有的 `rpl_thd_ctx.dependency_tracker_ctx()` 里，而不是全局表——**同一会话的事务由同一个 THD 处理，天然不需要全局协调**。

### 第六步：序号分配——`Transaction_dependency_tracker::step`

`Transaction_dependency_tracker::step` 是给事务分配序号的入口，函数体只有一行：

```cpp
int64 Transaction_dependency_tracker::step() { return m_commit_order.step(); }
```

它转发给 `Commit_order_trx_dependency_tracker::step`，后者转发给 `Logical_clock::step`：

```cpp
inline int64 Logical_clock::step() {
  static_assert(SEQ_UNINIT == 0, "");
  DBUG_EXECUTE_IF("logical_clock_step_2", ++state;);
  return ++state;
}
```

**`static_assert(SEQ_UNINIT == 0, "")` 把「未初始化序号」钉死为 0**——这是整个机制的哨兵值约定。`Logical_clock` 的构造函数是 `Logical_clock() : state(SEQ_UNINIT), offset(0) {}`，即新时钟的绝对值为 0。

`step()` 的注释写明调用约束：「调用方必须确保在无并发执行上下文中调用该方法，以便 offset 与 state 不会同时变化。」

调用点在 `sql/binlog.cc` 的缓存刷新路径：

```cpp
trn_ctx->sequence_number = mysql_bin_log.m_dependency_tracker.step();
```

紧接着是一段关于「一个事务被拆成两个缓存」的补偿：

```cpp
if (trn_ctx->last_committed == SEQ_UNINIT)
  trn_ctx->last_committed = trn_ctx->sequence_number - 1;
```

注释解释：事务缓存与语句缓存同时非空时会被刷成两个事务，第二个事务必须等第一个提交，所以第二个事务的 `last_committed` 要等于第一个的；注释还说明了这条路径的代价——「由于检测当前是不是第二个被刷出的缓存用的是简单方法，最初的若干个事务会被顺序记录（后一个被打上像是前一个是其提交父的标记）」。

### 第七步：提交序父依赖——`Commit_order_trx_dependency_tracker::get_dependency`

写集跟踪的输入就是这里算出来的。函数体做的事：

```cpp
sequence_number =
    trn_ctx->sequence_number - m_max_committed_transaction.get_offset();

if (trn_ctx->last_committed <= m_max_committed_transaction.get_offset())
  commit_parent = SEQ_UNINIT;
else
  commit_parent =
      std::max(trn_ctx->last_committed, m_last_blocking_transaction) -
      m_max_committed_transaction.get_offset();
```

- **减去 binlog 的时钟偏移**：注释说明这是为了把序号与父依赖「相对当前 binlog 定位」。
- **父依赖落到上一个 binlog 时置 `SEQ_UNINIT`**：注释写得很清楚——「binlog 轮转之后提交的事务，其提交父可能在上一个 binlog 里。这种情况下减去偏移后序号变成负数，提交父依赖就丢了。因此这种情况我们记 SEQ_UNINIT 值。」
- **`std::max(..., m_last_blocking_transaction)`**：与「最后一个阻塞事务」取最大值。

`m_last_blocking_transaction` 由 `is_trx_unsafe_for_parallel_slave` 决定：

```cpp
static bool is_trx_unsafe_for_parallel_slave(const THD *thd) {
  switch (thd->lex->sql_command) {
    case SQLCOM_ANALYZE:
    case SQLCOM_REPAIR:
    case SQLCOM_OPTIMIZE:
    case SQLCOM_CREATE_DB:
    case SQLCOM_ALTER_DB:
    case SQLCOM_DROP_DB:
      return true;
    case SQLCOM_ALTER_TABLE:
      return thd->lex->alter_info->flags & Alter_info::ALTER_ADMIN_PARTITION;
    default:
      return false;
  }
  return false;
}
```

这六类语句被判定为「对并行从库不安全」，函数上方的注释给了原因：「管理语句会更早释放元数据锁，这破坏了基于锁的逻辑时钟。这个函数识别这些语句。」`SQLCOM_ALTER_TABLE` 则按 `Alter_info::ALTER_ADMIN_PARTITION` 标志判定。

判定为不安全、或本次事务是并行化屏障（`parallelization_barrier`）时：

```cpp
if (is_trx_unsafe_for_parallel_slave(thd) || parallelization_barrier)
  m_last_blocking_transaction = trn_ctx->sequence_number;
```

即**后续事务的父依赖被抬高到这个阻塞事务上**——这类事务之后，从库必须恢复串行执行。

### 第八步：提交时更新最大已提交序号

`Transaction_dependency_tracker::update_max_committed` 在提交时推进水位：

```cpp
void Transaction_dependency_tracker::update_max_committed(THD *thd) {
  Transaction_ctx *trn_ctx = thd->get_transaction();
  m_commit_order.update_max_committed(trn_ctx->sequence_number);
  /*
    sequence_number timestamp isn't needed anymore, so it's cleared off.
  */
  trn_ctx->sequence_number = SEQ_UNINIT;

  assert(trn_ctx->last_committed == SEQ_UNINIT ||
         thd->commit_error == THD::CE_FLUSH_ERROR ||
         thd->commit_error == THD::CE_GNO_EXHAUSTED_ERROR);
}
```

提交后把 `sequence_number` 清回 `SEQ_UNINIT`（该值已无用途）。`Commit_order_trx_dependency_tracker::update_max_committed` 用 `set_if_greater` 推进最大已提交值，且要求持有 `LOCK_replica_trans_dep_tracker`：

```cpp
void Commit_order_trx_dependency_tracker::update_max_committed(
    int64 sequence_number) {
  mysql_mutex_assert_owner(&LOCK_replica_trans_dep_tracker);
  m_max_committed_transaction.set_if_greater(sequence_number);
}
```

`Logical_clock::set_if_greater` 用 CAS 循环实现「只向前」：`state = max(state, new_value)`；若 `new_val <= offset`（即新值属于上一个 binlog），直接返回 `SEQ_UNINIT` 且不改时钟——注释解释这是为了处理「本函数的调用可能与事务的刷盘相隔了几次轮转」的情况。

调用点在 `sql/binlog.cc` 的提交路径：

```cpp
if (thd->get_transaction()->sequence_number != SEQ_UNINIT) {
  mysql_mutex_lock(&LOCK_replica_trans_dep_tracker);
  m_dependency_tracker.update_max_committed(thd);
  mysql_mutex_unlock(&LOCK_replica_trans_dep_tracker);
}
```

### 第九步：binlog 轮转时重置历史

`Transaction_dependency_tracker::rotate` 做了三件事：

```cpp
void Transaction_dependency_tracker::rotate() {
  m_commit_order.rotate();
  /*
    To make slave appliers be able to execute transactions in parallel
    after rotation, set the minimum commit_parent to 1 after rotation.
  */
  m_writeset.rotate(1);
  if (current_thd) current_thd->get_transaction()->sequence_number = 2;
}
```

- `m_commit_order.rotate()`：把 `m_max_committed_transaction` 与 `m_transaction_counter` 的偏移都推到当前时间戳。
- `m_writeset.rotate(1)`：**清空历史表并把水位线设为 1**，注释解释了原因——「为了让从库 applier 在轮转之后仍能并行执行事务，轮转之后把最小提交父设为 1。」
- 把当前 THD 的序号设为 2（因为 0 是 `SEQ_UNINIT` 哨兵，1 已被当作最小父依赖）。

`sql/binlog.cc` 的 `MYSQL_BIN_LOG` 轮转路径调 `m_dependency_tracker.rotate()`。

### 第十步：结果写进 binlog 事件

写集跟踪的产物最终变成 binlog GTID 事件里的两个字段。`sql/binlog.cc` 的 `MYSQL_BIN_LOG::write_transaction` 是连接点：

```cpp
int64 sequence_number, last_committed;
/* Generate logical timestamps for MTS */
m_dependency_tracker.get_dependency(thd, parallelization_barrier,
                                    sequence_number, last_committed);
```

随后 `thd->get_transaction()->last_committed = SEQ_UNINIT;`——注释说明这是为了处理「事务缓存与语句缓存同时非空、会被刷成两个事务」的情况，第二个事务必须继承第一个的 `last_committed`。

这两个值最终写进 GTID 事件。`sql/log_event.cc` 的 `Gtid_log_event` 构造函数里有一条断言，规定了二者的关系：

```cpp
assert((sequence_number == 0 && last_committed == 0) ||
       (sequence_number > last_committed));
```

即**序号必须严格大于提交父**，否则断言失败。

### 从库一侧如何消费

写集跟踪算出的提交父被从库用于判定「这个事务能否与其它事务并发执行」：

- `sql/rpl_mta_submode.cc` 的 `Mts_submode_logical_clock::schedule_next_event` 从 GTID 事件里取出这两个值：`ptr_group->sequence_number = sequence_number = static_cast<Gtid_log_event *>(ev)->sequence_number;`、`ptr_group->last_committed = last_committed = static_cast<Gtid_log_event *>(ev)->last_committed;`
- `Mts_submode_logical_clock::wait_for_last_committed_trx` 实现等待条件：**`lwm < last_committed`**（`lwm` 是已提交事务的最小逻辑时间戳）。`last_committed_arg == SEQ_UNINIT` 时直接返回 false（不等待任何人）。等待期间线程状态被设为 `stage_worker_waiting_for_commit_parent`（定义在 `sql/mysqld.cc`，描述为 "Waiting for dependent transaction to commit"）。
- `sql/rpl_rli_pdb.cc` 的 `get_last_committed` / `get_sequence_number` 从 GTID 事件里取值，`Slave_worker::slave_worker_exec_event` 用它们做断言校验：注释写明「提交时间戳的等待条件 `lwm_estimate < last_committed` 必须已经由 Coordinator 满足」。

**父依赖越小，从库等待的时间越短，可并行的事务就越多**——这就是写集跟踪的全部收益所在。

## 关键取舍

### 取舍一：写集只压低父依赖，绝不抬高

写集跟踪器用的是 `commit_parent = std::min(last_parent, commit_parent)`——只会把父依赖**压低**。上游 `Commit_order_trx_dependency_tracker::get_dependency` 算出的值始终是上界。

**取舍：并行度只增不减，安全边界由提交序保证。** 代价是写集跟踪在提交序之外**没有任何额外的依赖约束能力**——它只能证明「不冲突」，不能证明「必须等待」。会话级依赖是唯一的例外，且它用的是 `std::max`（抬高方向），说明两个跟踪器的方向是刻意相反的。

### 取舍二：哈希算法必须会话与全局一致，否则整表作废

判据 `global_system_variables.transaction_write_set_extraction == thd->variables.transaction_write_set_extraction` 是一个**全局一刀切**的条件：任何会话改了哈希算法，该事务就完全用不了写集。

**取舍：保证历史表里的哈希与新算出的哈希可比。** 代价是一个改了 `transaction_write_set_extraction` 的会话会退化成提交序，且该事务会触发历史表清空（`!can_use_writesets` 分支），连带影响后续事务的并行度。

### 取舍三：容量上限触发时清空整张历史表

超容量的处理是 `m_writeset_history_start = sequence_number; m_writeset_history.clear();`——**整张表清空**，而不是淘汰最老的项。

**取舍：实现简单，且清空后所有事务退回提交序、行为完全正确。** 代价是清空之后的一批事务会集中失去并行度——源码注释明确写了这是为了「保持数据一致性」而采取的保守动作。容量判据 `m_writeset_history.size() + writeset->size() > m_opt_max_history_size` 用的是 `>`，即**恰好填满不算超限**。

### 取舍四：缺主键的事务不压低父依赖，但保留历史

`if (!write_set_ctx->get_has_missing_keys())` 才执行 `std::min`。源码注释说明了为什么缺主键时**不重置**历史：「任何引用这张表的事务同样会回退到 COMMIT_ORDER。」

**取舍：缺主键的事务按提交序走，但不清空历史影响其它表的事务。** 代价是同一张缺主键表上的事务之间完全没有并行度，而其它表的事务仍保留并行度。

### 取舍五：外键涉及的事务完全不用写集

`!write_set_ctx->get_has_related_foreign_keys()` 是 `can_use_writesets` 的一个必要条件——一旦事务涉及有外键的表（`table->s->foreign_key_parents > 0`），写集跟踪**整笔放弃**，退回提交序。

**取舍：外键的级联影响无法用行哈希表达，与其猜不如退回保守模式。** 代价是外键密集的库几乎享受不到写集跟踪的收益。

### 取舍六：只抽取唯一索引的值

`if (!((table->key_info[key_number].flags & (HA_NOSAME)) == HA_NOSAME)) continue;`——非唯一索引一律跳过。

**取舍：唯一约束是唯一能由「行的值」唯一确定冲突点的地方。** 代价是只建了非唯一索引的表完全没有写集信息，只能标记为缺主键。

### 取舍七：改前镜像与改后镜像都进写集

`binlog_log_row` 对 `after_record` 与 `before_record` 都调 `add_pke`。

**取舍：唯一约束既可能被改后值破坏，也可能被改前值破坏，两边都要覆盖。** 代价是 UPDATE 的写集条目数翻倍，直接吃掉了写集历史表的容量预算。

### 取舍八：会话依赖用 THD 私有状态而不是全局表

`Writeset_session_trx_dependency_tracker::get_dependency` 把上一个会话序号存在 `thd->rpl_thd_ctx.dependency_tracker_ctx()` 里。

**取舍：同一会话的事务由同一个 THD 串行处理，用 THD 私有状态天然正确、零锁。** 代价是这个约束**只在单个 THD 内成立**——会话一旦被迁移到另一个 THD，约束就断了。

### 取舍九：写集抽取失败降级而不是报错

`add_write_set` 触顶容量上限时，置 `m_local_has_reached_write_set_limit = true`、清空写集、返回 `false`（**不是错误**）；只有内存上限触发才报 `ER_WRITE_SET_EXCEEDS_LIMIT`。

**取舍：容量问题降级为「本事务没有写集」，后续由 `can_use_writesets` 的第四个条件把它挡在写集模式之外。** 代价是超容量事务的写集信息被完全丢弃，该事务退回提交序。

### 取舍十：模式值越界时回退到提交序

`Transaction_dependency_tracker::get_dependency` 的 `default` 分支：`assert(0);` 之后调 `m_commit_order.get_dependency(...)`，注释写明「在生产构建上回退到提交序」。

**取舍：非法的模式值不会让服务器崩溃，而是退化成最保守的模式。** 代价是 debug 构建会直接断言失败、行为与生产构建不一致。

### 取舍十一：启动期强制「抽取算法 OFF ⇒ 不用写集跟踪」

`sql/mysqld.cc` 的自检在 `transaction_write_set_extraction == HASH_ALGORITHM_OFF` 且模式不是 `DEPENDENCY_TRACKING_COMMIT_ORDER` 时报错并 `return 1`。

**取舍：配置错误在启动时就暴露，而不是运行期悄悄退化成提交序。** 代价是这两个系统变量必须成对配置。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/rpl_trx_tracking.cc | `Writeset_trx_dependency_tracker::get_dependency` | 核心：判断本事务能否用写集（`can_use_writesets` 四条件）、查 `m_writeset_history` 算冲突父依赖 `last_parent`、用 `std::min` 压低 `commit_parent`、超容量或不可信时清空历史并推进 `m_writeset_history_start` |
| sql/rpl_trx_tracking.cc | `Transaction_dependency_tracker::step` | 分配全局单调递增序号，转发给 `Commit_order_trx_dependency_tracker::step` → `Logical_clock::step`（`static_assert(SEQ_UNINIT == 0)`） |
| sql/rpl_trx_tracking.cc | `Transaction_dependency_tracker::get_dependency` | 按 `m_opt_tracking_mode` 分派三层跟踪器；`default` 分支在生产构建上回退到 `m_commit_order.get_dependency` |
| sql/rpl_trx_tracking.cc | `Commit_order_trx_dependency_tracker::get_dependency` | 算出提交序父依赖：序号与父依赖减去 binlog 时钟偏移；父依赖落到上一个 binlog 时置 `SEQ_UNINIT`；与 `m_last_blocking_transaction` 取 `std::max` |
| sql/rpl_trx_tracking.cc | `Commit_order_trx_dependency_tracker::step` | 转发给 `Logical_clock::step` 递增绝对时钟值 |
| sql/rpl_trx_tracking.cc | `Commit_order_trx_dependency_tracker::rotate` | 轮转时把 `m_max_committed_transaction` 与 `m_transaction_counter` 的偏移推到当前时间戳 |
| sql/rpl_trx_tracking.cc | `Commit_order_trx_dependency_tracker::update_max_committed` | 用 `set_if_greater` 推进最大已提交值；断言持有 `LOCK_replica_trans_dep_tracker` |
| sql/rpl_trx_tracking.cc | `Logical_clock::step` | `++state` 返回新绝对值；`static_assert(SEQ_UNINIT == 0, "")` 钉死哨兵值 |
| sql/rpl_trx_tracking.cc | `Logical_clock::set_if_greater` | CAS 循环实现 `state = max(state, new_value)`；`new_val <= offset` 时返回 `SEQ_UNINIT` 且不改时钟 |
| sql/rpl_trx_tracking.cc | `is_trx_unsafe_for_parallel_slave` | 识别对并行从库不安全的语句：`SQLCOM_ANALYZE` / `REPAIR` / `OPTIMIZE` / `CREATE_DB` / `ALTER_DB` / `DROP_DB`，以及带 `Alter_info::ALTER_ADMIN_PARTITION` 的 `SQLCOM_ALTER_TABLE` |
| sql/rpl_trx_tracking.cc | `Writeset_trx_dependency_tracker::rotate` | 清空 `m_writeset_history` 并把 `m_writeset_history_start` 设为给定起点 |
| sql/rpl_trx_tracking.cc | `Writeset_session_trx_dependency_tracker::get_dependency` | 取同会话上一个序号 `session_parent`，用 `std::max` **抬高** `commit_parent`；序号存在 `thd->rpl_thd_ctx.dependency_tracker_ctx()` |
| sql/rpl_trx_tracking.cc | `Transaction_dependency_tracker::tracking_mode_changed` | 模式切换时按最大已提交时间戳调 `m_writeset.rotate(timestamp)` 清空写集历史 |
| sql/rpl_trx_tracking.cc | `Transaction_dependency_tracker::update_max_committed` | 提交时推进最大已提交序号，并把 `trn_ctx->sequence_number` 清回 `SEQ_UNINIT` |
| sql/rpl_trx_tracking.cc | `Transaction_dependency_tracker::rotate` | 轮转时 `m_commit_order.rotate()`、`m_writeset.rotate(1)`（最小提交父设为 1）、当前 THD 序号设为 2 |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::add_write_set` | 写集入库的三道闸门：容量上限（判据 `write_set.size() >= binlog_trx_dependency_history_size`，取自 `m_opt_max_history_size`）、全局内存上限（触发则报 `ER_WRITE_SET_EXCEEDS_LIMIT`）、全局要求完整写集时豁免容量上限 |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::write_set_memory_size` | 写集占用字节数：`sizeof(uint64) * write_set.size()` |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::get_write_set` | 返回写集向量指针 `&write_set` |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::get_has_missing_keys` | 读缺主键标志；由 `set_has_missing_keys` 置位 |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::get_has_related_foreign_keys` | 读涉及外键标志；由 `set_has_related_foreign_keys` 置位 |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::was_write_set_limit_reached` | 读「本次事务已触容量上限」标志 |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::add_savepoint` | 记下 `write_set.size()` 作为保存点位置；同名旧保存点先删除 |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::rollback_to_savepoint` | 把写集从保存点位置起 `erase`，并删除位置更靠后的保存点 |
| sql/rpl_transaction_write_set_ctx.cc | `Rpl_transaction_write_set_ctx::reset_state` | 事务复位：清写集、清两个标志、清容量上限标志 |
| sql/rpl_write_set_handler.cc | `add_pke` | 为一行抽取主键等价键：遍历唯一索引拼 `pke`（索引名 + 库名/长度 + 表名/长度 + 规范化键值/长度），算哈希后写入写集；无哈希产出时置缺主键标志 |
| sql/rpl_write_set_handler.cc | `calc_hash` | 哈希分派：`HASH_ALGORITHM_MURMUR32` 走 `murmur3_32`，否则走 `MY_XXH64` |
| sql/rpl_write_set_handler.cc | `generate_hash_pke` | 对 `pke` 串算哈希并调 `add_write_set`；返回真表示出错 |
| sql/rpl_write_set_handler.cc | `transaction_write_set_hashing_algorithms` | 哈希算法名字表：`{"OFF", "MURMUR32", "XXHASH64", nullptr}` |
| sql/rpl_write_set_handler.cc | `get_write_set_algorithm_string` | 枚举 → 字符串映射；未知值返回 `"UNKNOWN ALGORITHM"` |
| sql/handler.cc | `binlog_log_row` | 写集抽取触发点：`transaction_write_set_extraction != HASH_ALGORITHM_OFF` 时对改后与改前两条记录分别调 `add_pke`；`bad_alloc` 转 `ER_OUT_OF_RESOURCES` |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_transaction` | 调 `m_dependency_tracker.get_dependency(thd, parallelization_barrier, sequence_number, last_committed)` 生成 MTS 逻辑时间戳 |
| sql/binlog.cc | 缓存刷新路径 | `trn_ctx->sequence_number = mysql_bin_log.m_dependency_tracker.step();` 分配序号 |
| sql/binlog.cc | 提交路径 | 持 `LOCK_replica_trans_dep_tracker` 时调 `m_dependency_tracker.update_max_committed(thd)` |
| sql/binlog.cc | binlog 轮转路径 | 调 `m_dependency_tracker.rotate()` |
| sql/mysqld.cc | 启动自检 | 抽取算法为 `HASH_ALGORITHM_OFF` 而模式不是 `DEPENDENCY_TRACKING_COMMIT_ORDER` 时报 `ER_TX_EXTRACTION_ALGORITHM_FOR_BINLOG_TX_DEPEDENCY_TRACKING` 并 `return 1`；否则调 `tracking_mode_changed()` |
| sql/mysqld.cc | `stage_worker_waiting_for_commit_parent` | 从库等待提交父的线程状态名，描述 "Waiting for dependent transaction to commit" |
| sql/log_event.cc | `Gtid_log_event` 构造函数 | 断言 `sequence_number > last_committed`；两个值写入 GTID 事件的 `last_committed` / `sequence_number` 字段 |
| sql/rpl_mta_submode.cc | `Mts_submode_logical_clock::schedule_next_event` | 从 GTID 事件取 `sequence_number` 与 `last_committed` 存入事务组 |
| sql/rpl_mta_submode.cc | `Mts_submode_logical_clock::wait_for_last_committed_trx` | 等待条件 `lwm < last_committed`；`last_committed_arg == SEQ_UNINIT` 时不等待直接返回 false |
| sql/rpl_rli_pdb.cc | `get_last_committed` / `get_sequence_number` | 从 `Gtid_log_event` 取提交父与序号 |
| sql/rpl_rli_pdb.cc | `Slave_worker::slave_worker_exec_event` | 断言 `lwm_estimate < last_committed` 的等待条件已由 Coordinator 满足 |

## 与主核 / AI 的关系

- **行级冲突判定是同一类问题，但粒度与载体不同。** MySQL 把「哪些行被写过」压缩成 64 位哈希放进事务上下文（`Rpl_transaction_write_set_ctx` 的 `write_set`），冲突判定发生在**提交时刻**、用于算出 binlog 里的提交父。**架构推论：** 主核（`engine/db/`）若要做行级并发写冲突判定，需要解决的是同一类问题——但主核的写集载体与冲突判定时机本页没有核对 `engine/db/` 源码，不作断言。
- **写集是「事务级增量状态」，与主核的 WAL 是两类东西。** MySQL 的写集是**内存中的、易失的、随提交而丢弃**的（`clear_write_set` / `reset_state`）；它不进 binlog，只有算出的提交父会进。**架构推论：** 这意味着写集跟踪**不能用于跨实例的重放或恢复**，也不构成持久化的一致性依据——AI 侧若要判断「某行是否被写过」，不能指望从 binlog 里反推写集。
- **对 AI / CDC 链路而言，写集跟踪的产物只有一个数字有意义：`last_committed`。** 写集本身既不落盘也不随 binlog 传输，从库拿到的只有提交父这一个逻辑时间戳。**架构推论：** AI 侧若要自行实现「按冲突分桶并行消费」，其等价物是把行哈希历史表搬到消费端自己维护——但 MySQL 侧的这份历史表是**易失且按容量上限清空**的，因此从库端无法复用源库的历史；AI 侧的自建方案在这一点上与 MySQL 是不同的取舍。
- **写集跟踪与并行复制调度的职责边界正是本页的边界声明。** 写集跟踪只负责「算出依赖」，「怎么调度」属于 M02。**架构推论：** AI 侧若照搬 MySQL 的形态，最容易出错的正是把「算依赖」与「调度」混在一起——`Writeset_trx_dependency_tracker::get_dependency` 里的 `std::min`（压低）与 `Writeset_session_trx_dependency_tracker::get_dependency` 里的 `std::max`（抬高）方向相反，两条路径若被合并成一条，就会丢掉会话级串行约束。

## 参考

- [sql/rpl_trx_tracking.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_trx_tracking.cc) —— 本页 refs 的权威来源：`Writeset_trx_dependency_tracker::get_dependency`（写集历史表查询与父依赖压低）、`Transaction_dependency_tracker::step`（序号分配）、`Commit_order_trx_dependency_tracker::get_dependency`、`Logical_clock::step` / `set_if_greater`、`is_trx_unsafe_for_parallel_slave`、三个跟踪器的分派逻辑。
- [sql/rpl_transaction_write_set_ctx.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_transaction_write_set_ctx.cc) —— 写集上下文的实现：`add_write_set` 的容量与内存两道闸门、`ER_WRITE_SET_EXCEEDS_LIMIT` 报错、缺主键与外键两个标志位、保存点对写集的裁剪。
- [sql/rpl_write_set_handler.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_write_set_handler.cc) —— 主键等价键的抽取：`add_pke` 的拼接规则与唯一索引过滤、`calc_hash` 的 `murmur3_32` / `MY_XXH64` 分派、`transaction_write_set_hashing_algorithms` 名字表、外键键值的构造与「无唯一约束的外键被跳过」的注释。
- [sql/binlog.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/binlog.cc) —— 写集跟踪与 binlog 的连接点：`MYSQL_BIN_LOG::write_transaction` 调 `get_dependency` 生成 MTS 逻辑时间戳、缓存刷新时的 `step()`、提交路径的 `update_max_committed`、轮转路径的 `rotate()`。
- [sql/mysqld.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/mysqld.cc) —— 启动自检：抽取算法 OFF 与写集跟踪模式的互斥校验与 `ER_TX_EXTRACTION_ALGORITHM_FOR_BINLOG_TX_DEPEDENCY_TRACKING` 错误。
- [sql/rpl_mta_submode.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_mta_submode.cc) —— 从库消费侧：`schedule_next_event` 取 `sequence_number` / `last_committed`，`wait_for_last_committed_trx` 实现 `lwm < last_committed` 的等待条件。
- [sql/log_event.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/log_event.cc) —— GTID 事件侧：`Gtid_log_event` 构造函数中 `sequence_number > last_committed` 的断言与两个字段的写入。
- [Replication Options（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-options.html) —— `binlog_transaction_dependency_tracking` 与 `binlog_transaction_dependency_history_size` 两个系统变量的官方定义与取值，对应本页的 `m_opt_tracking_mode` 与 `m_opt_max_history_size`。[Oracle 官方手册镜像（同一手册的官方托管副本）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/replication-options.html)
- [System Variable References（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/server-system-variables.html) —— `transaction_write_set_extraction` 的官方取值（`OFF` / `MURMUR32` / `XXHASH64`），对应本页 `transaction_write_set_hashing_algorithms` 与 `calc_hash` 的算法分派。[Oracle 官方手册镜像](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/server-system-variables.html)
- [Replication with Global Transaction Identifiers（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-gtids.html) —— GTID 与并行复制的事务依赖定位方式，是本页「提交父」语义的上位概念来源。[Oracle 官方手册镜像](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/replication-gtids.html)
- [Replicated Binary Log File-Based Transactions（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-options-binary-log.html) —— 从库如何按依赖关系调度事务的官方说明，对应本页「从库一侧如何消费」一节。[Oracle 官方手册镜像](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/replication-options-binary-log.html)
