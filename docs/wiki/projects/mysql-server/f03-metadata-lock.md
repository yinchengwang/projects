---
project: mysql-server
feature: metadata-lock
page_type: feature
schema_version: 1
refs:
  - path: sql/mdl.cc
    symbols: [MDL_context::acquire_lock, MDL_context::release_lock]
---

# 元数据锁

## 一句话职责

用一把服务器层的「元数据锁」（Metadata Lock，MDL）把对同一张表/对象元数据的读写请求串成互斥关系：DDL 需要排他（Exclusive）锁才能改元数据，DML 只需共享（Shared）锁就能读元数据，于是 DDL 与 DML 之间自动形成「要么 DDL 先做完、要么 DML 先做完」的秩序，不会在 DDL 改表结构的半途被并发 DML 撞上。

## 机制是什么

MDL 是 **SQL 层**的锁，与 InnoDB 的行锁、意向锁是两套完全独立的机制。`sql/mdl.cc` 把锁分成两类对象：

- **scoped lock（作用域锁）**：作用于 GLOBAL、COMMIT、TABLESPACE、BACKUP_LOCK、SCHEMA 这几个命名空间，锁的是「全局范围」而不是某一个具名对象。源码注释写明这类锁只支持 SHARED、INTENTION EXCLUSIVE 和 EXCLUSIVE 三种模式。
- **object lock（per-object lock，按对象加锁）**：作用于 TABLE、ROUTINE、TRIGGER、EVENT、USER_LEVEL_LOCK、LOCKING_SERVICE、ACL_CACHE 等命名空间，锁的是「某个库里的某个具名对象」，支持全部锁类型。

每种锁类型的强度由两套**位图矩阵**定义，都写在 `sql/mdl.cc` 里：

- `m_granted_incompatible[]`：**已授予**锁的相容矩阵。它回答「要拿 type 这种锁，场上已授予的锁里哪些类型会挡我」。
- `m_waiting_incompatible[]`：**排队中**请求的优先级矩阵。它回答「要拿 type 这种锁，场上排队的人里哪些类型会挡我（且优先级更高）」。

按对象锁的矩阵（源码注释里的表），行是请求类型，列是已授予类型：

```
         | S  SH  SR  SW  SWLP  SU  SRO  SNW  SNRW  X  |
---------+---------------------------------------------+
S        | +   +   +   +    +    +   +    +    +    -  |
SH       | +   +   +   +    +    +   +    +    +    -  |
SR       | +   +   +   +    +    +   +    +    -    -  |
SW       | +   +   +   +    +    +   -    -    -    -  |
SWLP     | +   +   +   +    +    +   -    -    -    -  |
SU       | +   +   +   +    +    -   +    -    -    -  |
SRO      | +   +   +   -    -    +   +    +    -    -  |
SNW      | +   +   +   -    -    -   +    -    -    -  |
SNRW     | +   +   -   -    -    -   -    -    -    -  |
X        | -   -   -   -    -    -   -    -    -    -  |
```

`+` 表示可满足，`-` 表示必须等待。注意最后一行：**X 锁与任何已授予的锁都不相容**，所以 `DROP TABLE` 拿 X 锁时必须等所有 DML 的 S/SW 锁全部释放。

锁还有**持续时间**（duration）维度，由 `MDL_context` 的票据仓库（`m_ticket_store`）按 `MDL_STATEMENT`、`MDL_TRANSACTION`、`MDL_EXPLICIT` 分开存放。同一个 `MDL_context` 在同一对象上可能同时持有多把不同强度的锁，**升级**就是其中的一个动作。

**边界**：本机制只负责元数据锁自身的申请、等待队列、升级与释放；数据行锁是 InnoDB 的事，不在本页范围。

## 怎么实现的

### 1. 三层数据结构

- `MDL_key`：锁的标识，含命名空间、库名、对象名。`MDL_request::init_with_source()` 用它初始化请求，并记录发起请求的源文件名与行号（`m_src_file` / `m_src_line`）。源码注释明确：**MDL 子系统不拥有也不管理请求对象的内存**，请求通常放在语句内存里（例如 `Table_ref` 的成员）。
- `MDL_lock`：全局唯一的锁对象，挂在全局单例 `MDL_map mdl_locks`（源码注释：整个服务器只有一个实例）里。内部有已授予票据链表 `m_granted`、等待队列 `m_waiting`，以及两个位图 `m_granted.bitmap()` / `m_waiting.bitmap()`。
- `MDL_ticket` / `MDL_request`：票据是「一次成功加锁」的凭证，请求是「一次加锁意图」的描述。`MDL_context` 持有一张票据表。

### 2. 申请路径：`MDL_context::acquire_lock()` 完整流程

`MDL_context::acquire_lock(MDL_request *mdl_request, Timeout_type lock_wait_timeout)` 是单把锁的申请入口，返回 `false` 成功、`true` 失败。顺序如下：

**第一步：零超时特判。** 若 `lock_wait_timeout == 0`，直接退到 `try_acquire_lock()`（源码注释：这样可以避免不必要的死锁检测尝试，以及由它产生的「假死锁」）。失败且 `mdl_request->ticket` 为空时，`my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0))` 并返回 `true`。

**第二步：非零超时走正常路径。** 先 `set_timespec(&abs_timeout, lock_wait_timeout)` 算出绝对超时时刻，再调 `try_acquire_lock_impl(mdl_request, &ticket)`。

**第三步：立即拿到锁。** 若 `mdl_request->ticket` 非空，说明 `try_acquire_lock_impl()` 已经授予成功，直接返回 `false`。

**第四步：进入等待队列。** `try_acquire_lock_impl()` 失败时已经把 `ticket->m_lock` 指向了对应的 `MDL_lock`，且该 `MDL_lock` 的 `m_rwlock` 处于写锁定状态。于是 `lock->m_waiting.add_ticket(ticket)` 把票据挂进等待队列；接着 `m_wait.reset_status()` 清空等待槽；再 `lock->needs_notification(ticket)` 为真时调 `lock->notify_conflicting_locks(this)` 通知冲突锁的持有者；最后 `mysql_prlock_unlock(&lock->m_rwlock)` 释放 `m_rwlock`。

**第五步：死锁检测。** 这里有一个刻意的例外：

```
if (lock->key.mdl_namespace() != MDL_key::ACL_CACHE ||
    ticket->m_type != MDL_SHARED ||
    get_owner()->might_have_commit_order_waiters()) {
  find_deadlock();
} else if (has_locks()) {
  delayed_find_deadlock = true;
}
```

即：请求 `ACL_CACHE` 命名空间上的 SHARED 锁、且本上下文**没有** commit order 等待者时，跳过立即死锁检测；若本上下文已持锁，则把死锁检测**推迟 1 秒**（源码注释：希望这期间请求就被授予，从而完全跳过死锁检测）。源码注释解释 `ACL_CACHE` 是单例锁、每个连接甚至每条语句都要取 S 锁，连接数多时死锁检测开销会非常大。反过来，带 commit order 等待者的复制应用线程**不做**这个优化，因为这类场景本来就应该尽快检测出死锁。

**第六步：等待。** 若 `lock->needs_notification(ticket)` 或 `lock->needs_connection_check()` 为真，则进入「短等待循环」：每次 `set_timespec(&abs_shortwait, 1)` 等待 1 秒，醒来后依次检查——

- `lock->needs_connection_check()` 且 `!m_owner->is_connected()`：把等待状态置为 `MDL_wait::KILLED` 并跳出。源码注释解释这是用户级锁的语义：客户端断开就当作语句被 kill（源码注释说这与 5.7 之前「当作超时」的处理不同，更合理）。这个检查只对 `USER_LEVEL_LOCK`、`LOCKING_SERVICE`、`ACL_CACHE` 三个命名空间生效（见 `object_lock_needs_connection_check()`）。
- `lock->needs_notification(ticket)`：重新 `notify_conflicting_locks(this)`。
- `delayed_find_deadlock` 为真：补做一次 `find_deadlock()`，然后清零该标志。

短等待循环结束后若仍未拿到锁（`wait_status == MDL_wait::WS_EMPTY`），改为一次长等待：`m_wait.timed_wait(m_owner, &abs_timeout, true, mdl_request->key.get_wait_state_name())`。不需要通知也不需要连接检查时，直接一次长等待。

**第七步：失败处理。** `wait_status != MDL_wait::GRANTED` 时，先 `lock->remove_ticket(this, m_pins, &MDL_lock::m_waiting, ticket)` 把票据从等待队列摘掉；若 `ticket->m_hton_notified` 为真，补一次 `notify_hton_post_release_exclusive()`（源码注释：已通知 SE 即将获取锁，失败时必须做与释放等价的通知）；`MDL_ticket::destroy(ticket)` 销毁票据；然后按状态分派错误码：

| 等待状态 | 错误 |
|---|---|
| `MDL_wait::VICTIM` | `ER_LOCK_DEADLOCK` |
| `MDL_wait::TIMEOUT` | `ER_LOCK_WAIT_TIMEOUT` |
| `MDL_wait::KILLED` | 若 `get_owner()->is_killed() == ER_QUERY_TIMEOUT` 报 `ER_QUERY_TIMEOUT`，否则报 `ER_QUERY_INTERRUPTED` |

**第八步：授予。** `wait_status == MDL_wait::GRANTED` 时，`m_ticket_store.push_front(mdl_request->duration, ticket)` 存入票据表并把 `mdl_request->ticket` 指回票据，PSI 状态置 `MDL_ticket::GRANTED`，返回 `false`。源码注释说明此时 `MDL_lock` 的状态已由并发线程（`MDL_lock::reschedule_waiters()`）更新好了，这里只更新 `MDL_context` 与 `MDL_request`。

### 3. 快慢两条路径：`try_acquire_lock_impl()`

`try_acquire_lock_impl(MDL_request *mdl_request, MDL_ticket **out_ticket)` 是不等待的加锁尝试，返回 `false` 表示「可能没拿到，看 `mdl_request->ticket` 是否为空」。

**复用已有票据。** 函数开头先 `find_ticket(mdl_request, &found_duration)`。`find_ticket()` 的实现是 `m_ticket_store.find(*mdl_request)`——票据仓库按（对象，强度）建索引。命中时直接复用：若 `found_duration != mdl_request->duration` 或 `mdl_request->duration == MDL_EXPLICIT`，则调 `clone_ticket()` 复制一张票据。源码注释解释克隆的三个理由：事务锁与 HANDLER 锁不能共用同一张票据，否则 `HANDLER CLOSE` 会误释放事务锁、`COMMIT` 会误释放 HANDLER 锁；`HANDLER CLOSE` 关一个别名也不该释放另一个别名打开的表。

**决定走快路径还是慢路径。** `unobtrusive_lock_increment = MDL_lock::get_unobtrusive_lock_increment(mdl_request)` 返回该类型的「快路径增量」；`force_slow = !unobtrusive_lock_increment || m_needs_thr_lock_abort`——即「obtrusive（强势）类型」或「本上下文有打开的 HANDLER」时强制走慢路径。源码注释说明后者是因为 `MDL_object_lock::notify_conflicting_locks()` 需要正确的票据列表才能工作。

**obtrusive 请求要先物化快路径票据。** `if (!unobtrusive_lock_increment) materialize_fast_path_locks();`。`materialize_fast_path_locks()` 遍历各 duration 的票据链，把 `m_is_fast_path` 为真的票据逐个从 `MDL_lock::m_fast_path_state` 的打包计数器里减掉、置 `HAS_SLOW_PATH` 标志，并加入 `lock->m_granted` 链表。源码注释说明原因：这样 `can_grant_lock()` 才能安全假定「所有已授予的快路径锁都属于不同的上下文」。

**快路径增量表。** 按对象锁的增量数组在 `m_object_lock_strategy` 里：

```
{0, 1, 1, 1ULL << 20, 1ULL << 40, 1ULL << 40, 0, 0, 0, 0, 0}
```

顺序对应 `S, SH, SR, SW, SWLP, SU, SRO, SNW, SNRW, X`。即：按对象锁的「unobtrusive」类型是 **S、SH、SR、SW、SWLP**，「obtrusive」类型是 **SU、SRO、SNW、SNRW、X**。源码注释给出位编码：bits 0..19 存 S 与 SH（不区分），bits 20..39 存 SR，bits 40..59 存 SW 与 SWLP。注释还说明不会溢出，因为「可预见的将来不太可能支持超过 2^20 - 1 个并发连接」。作用域锁的增量数组是 `{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}`——只有 IX 是 unobtrusive。

**快路径的具体步骤。** `mdl_locks.find_or_insert(m_pins, key, &pinned)` 找到或插入 `MDL_lock`（源码注释：GLOBAL、COMMIT、ACL_CACHE 命名空间的对象是单例，不加 pin）。然后在 `m_fast_path_state` 上做原子 CAS 循环：先查 `IS_DESTROYED` 标志（命中则 `goto retry` 重新查找），再查 `HAS_OBTRUSIVE`（命中则 `goto slow_path`），最后把 `old_state + unobtrusive_lock_increment` CAS 回去。成功后置 `ticket->m_is_fast_path = true` 并返回。源码注释说：这里不做互斥检查，因为快路径锁类型之间**互相全部相容**，且场上没有 obtrusive 锁。

**慢路径的具体步骤。** `mysql_prlock_wrlock(&lock->m_rwlock)` 拿写锁后：查 `IS_DESTROYED`（命中则解锁、unpin、`goto retry`）；计算 `first_obtrusive_lock = (unobtrusive_lock_increment == 0) && ((lock->m_obtrusive_locks_granted_waiting_count++) == 0)`——源码注释说明必须在调 `can_grant_lock()` **之前**原子设置 `HAS_OBTRUSIVE` 标志，否则并发的快路径加锁会破坏 `can_grant_lock()` 的结果；然后 `lock->can_grant_lock(mdl_request->type, this)` 判断可否授予，可授予就 `lock->m_granted.add_ticket(ticket)`，否则 `*out_ticket = ticket`（票据留给等待队列用）。

**`can_grant_lock()` 的判定条件。** 函数读两张矩阵：`incompatible_waiting_types_bitmap()[type_arg]` 与 `incompatible_granted_types_bitmap()[type_arg]`。可授予的充要条件是：排队位图与等待相容位图无交集，且快路径已授予位图与已授予相容位图无交集，且 `m_granted` 位图与已授予相容位图无交集；若 `m_granted` 里有冲突票据，则逐个遍历，**只有属于其它上下文**的冲突票据才算冲突（`ticket->get_ctx() != requestor_ctx && ticket->is_incompatible_when_granted(type_arg)`）。源码注释写明这与「本上下文已持有更强类型的锁会自动被授予」是配套的。

### 4. 等待队列的调度：`MDL_lock::reschedule_waiters()`

`reschedule_waiters()` 从等待队列头开始遍历，对每个票据调 `can_grant_lock(ticket->get_type(), ticket->get_ctx())`，能授予就 `m_wait.set_status(MDL_wait::GRANTED)`，然后把票据从 `m_waiting` 移到 `m_granted`。源码注释指出：**唤醒等待者之后再改锁对象状态是安全的**，因为任何要查这个锁状态的会话都必须先拿 `MDL_lock::m_rwlock`，等它拿到时看到的已经是更新后的状态。

如果 `m_wait.set_status()` 返回假（源码注释：等待者的连接/语句已被 kill，或已超时，等待槽非空），就把它留在等待队列里继续找下一个可授予的请求。

**优先级矩阵的切换。** 同一函数末尾有一段按 `max_write_lock_count` 切换优先级的逻辑：

- `MDL_OBJECT_HOG_LOCK_TYPES` 是「hog」类型位图：`MDL_BIT(MDL_SHARED_NO_WRITE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) | MDL_BIT(MDL_EXCLUSIVE)`。源码注释说这类锁容易把低优先级锁饿死，所以「有冲突锁在等时，只连续授予它们 max_write_lock_count 次」。
- `MDL_lock::get_incompatible_waiting_types_bitmap_idx()` 计算当前该用哪张优先级矩阵：`idx = 0`；`m_piglet_lock_count >= max_write_lock_count` 则 `idx += 1`；`m_hog_lock_count >= max_write_lock_count` 则 `idx += 2`。因此共有 4 张矩阵：默认（0）、piglet 超限（1）、hog 超限（2）、两者都超限（3）。
- `count_piglets_and_hogs(type)` 负责计数：若授予的类型属于 `MDL_OBJECT_HOG_LOCK_TYPES` 且队列里有非 hog 的等待者，`m_hog_lock_count++`；否则若 `type == MDL_SHARED_WRITE` 且队列里有 `MDL_SHARED_READ_ONLY` 等待者，`m_piglet_lock_count++`。计数越过阈值时调 `switch_incompatible_waiting_types_bitmap_if_needed()` 换矩阵并返回真，调用方随即 `reschedule_waiters()` 重排。
- 4 张矩阵的具体内容在 `m_object_lock_strategy` 里。第 0 张（默认）中，请求类型为 S 时只有 X 会挡（`MDL_BIT(MDL_EXCLUSIVE)`）；请求类型为 X 时没有任何排队类型会挡（`0`）。第 1 张把 SW 降到 SRO 之下。第 2 张让 S/SH/SR/SW/SNRW/SRO/SU 全部优先于所有 hog 类型。第 3 张在第 2 张基础上把 SRO 也提到 SW/SWLP 之上。
- `is_affected_by_max_write_lock_count()` 对 `ACL_CACHE` 命名空间**显式关闭**这套机制（源码注释：目的是保留「对 ACL_CACHE 跳过 `find_deadlock()`」的优化）。

**矩阵退档。** `reschedule_waiters()` 末尾还有退档逻辑：若当前是矩阵 3 且队列里除 hog/SW/SWLP 外没有别的等待者，则清零 `m_piglet_lock_count` 与 `m_hog_lock_count` 并直接退回矩阵 0；否则分别判断「队列里没有非 hog 等待者」（清 `m_hog_lock_count`，`m_current_waiting_incompatible_idx &= ~2`）与「队列里没有 `MDL_SHARED_READ_ONLY` 等待者」（清 `m_piglet_lock_count`，`&= ~1`）。源码注释解释为什么矩阵 3 不能先降到矩阵 2：那会让并发的 SW 与 SRO 请求流把 hog 锁饿死。

### 5. 升级：`MDL_context::upgrade_shared_lock()`

`upgrade_shared_lock(MDL_ticket *mdl_ticket, enum_mdl_type new_type, Timeout_type lock_wait_timeout)` 用于 ALTER TABLE 与 CREATE TABLE。流程：

1. 若 `mdl_ticket->has_stronger_or_equal_type(new_type)` 则直接返回 `false`（源码注释：FLUSH TABLE under LOCK TABLES 时同一张表在 LOCK TABLES 列表里出现两次会走到这里）。
2. 用同一把 `MDL_lock` 的 key 构造一个新的 `MDL_request`，类型为 `new_type`，duration 为 `MDL_TRANSACTION`，调 `acquire_lock(&mdl_new_lock_request, lock_wait_timeout)`。**失败即升级失败**，原共享锁保持不变（源码注释明确写了这个保证）。
3. `is_new_ticket = !has_lock(mdl_svp, mdl_new_lock_request.ticket)` 判断拿到的是不是一张新票据。
4. 拿 `lock->m_rwlock` 写锁：若 `is_new_ticket` 则把新票据从 `m_granted` 摘掉并 `--lock->m_obtrusive_locks_granted_waiting_count`；把旧票据按快/慢路径分别处理（快路径则减计数器并清 `m_is_fast_path`，慢路径则 `m_granted.remove_ticket()`）；然后 `mdl_ticket->m_type = new_type;`，`lock->m_granted.add_ticket(mdl_ticket);`，最后 `++lock->m_obtrusive_locks_granted_waiting_count`。

源码注释指出升级的前提：**一把锁只能有一个升级者**，否则必然死锁；这个不变式由「SU、SNW、SNRW 在 ALTER TABLE 操作中互不相容」保证；但在 CREATE TABLE 场景下存在死锁可能，因为 S 与 S 是相容的，源码说这种情况由「回退重试」机制化解。

**强度比较**由 `MDL_ticket::has_stronger_or_equal_type(type)` 完成，实现是位运算：

```
return !(granted_incompat_map[type] & ~(granted_incompat_map[m_type]));
```

即「type 类型所冲突的锁集合」是「当前类型所冲突的锁集合」的子集，则当前票据至少和 type 一样强。

### 6. 释放：`MDL_context::release_lock()`

`MDL_context::release_lock(enum_mdl_duration duration, MDL_ticket *ticket)` 是释放的主实现；`release_lock(MDL_ticket *ticket)` 是只接受 EXPLICIT 时长的重载（`assert(ticket->m_duration == MDL_EXPLICIT)` 后转调前者）。

流程：

1. `m_ticket_store.remove(duration, ticket)`——**先把票据从票据表摘掉，再真正释放锁**。源码注释说明原因：摘除过程要引用 `MDL_lock::m_key`，若票据表用哈希二级索引，先摘才能安全引用。
2. 若 `ticket->m_hton_notified`，先把 `lock->key` 拷到栈上的 `key_for_hton`（源码注释：释放后 `MDL_lock` 对象可能已被释放，需要栈上留一份 key 才能通知 SE）。
3. **快路径票据的释放**（`ticket->m_is_fast_path` 为真）：算出 `unobtrusive_lock_increment = lock->get_unobtrusive_lock_increment(ticket->get_type())`，并 `assert(unobtrusive_lock_increment != 0)`（源码注释：快路径票据不应代表 obtrusive 类型）。然后在 `m_fast_path_state` 上做 CAS 循环：若 `old_state & MDL_lock::HAS_OBTRUSIVE`，则先拿 `m_rwlock` 写锁再 `fast_path_state_add(-unobtrusive_lock_increment)`，并在此期间若 `lock->m_obtrusive_locks_granted_waiting_count` 非零则 `lock->reschedule_waiters()`（源码注释：可能有等待者在等这张锁让开）；否则直接 CAS 把 `old_state - unobtrusive_lock_increment` 写回。最后若 `last_use && !is_singleton` 则 `mdl_locks.lock_object_unused(this, m_pins)`（源码注释：单例对象不计入「未使用」计数）。
4. **慢路径票据的释放**：`lock->remove_ticket(this, m_pins, &MDL_lock::m_granted, ticket)`。`remove_ticket()` 内部会：把票据从指定链表摘掉；若摘的是 obtrusive 类型则 `--m_obtrusive_locks_granted_waiting_count`，且归零时要清 `HAS_OBTRUSIVE` 标志；若 `m_granted` 与 `m_waiting` 都空了则清 `HAS_SLOW_PATH` 标志。
5. 若 `ticket->m_hton_notified`，做 `notify_hton_post_release_exclusive(&key_for_hton)`。
6. `MDL_ticket::destroy(ticket)` 销毁票据。

**批量释放**由 `release_locks_stored_before(duration, sentinel)` 提供：沿指定 duration 的票据链遍历，逐个 `release_lock(duration, ticket)`，遇到 `sentinel` 就停。源码注释说明票据按**逆时间序**存放（新的在前），这个设计让同一个函数能服务三种场景——冲突时回退、语句/事务结束时全部释放、回滚到保存点。

**具体调用方：**

- `MDL_context::release_transactional_locks()`：先 `release_locks_stored_before(MDL_STATEMENT, nullptr)` 再 `release_locks_stored_before(MDL_TRANSACTION, nullptr)`。源码注释写明它用于事务结束（显式或隐式 COMMIT、ROLLBACK）的实现。
- `MDL_context::release_statement_locks()`：只调 `release_locks_stored_before(MDL_STATEMENT, nullptr)`。
- `MDL_context::rollback_to_savepoint(mdl_savepoint)`：调 `release_locks_stored_before(MDL_STATEMENT, mdl_savepoint.m_stmt_ticket)` 与 `release_locks_stored_before(MDL_TRANSACTION, mdl_savepoint.m_trans_ticket)`。
- `MDL_context::release_all_locks_for_name(name)`：遍历 `MDL_EXPLICIT` 时长的票据，对 `m_lock == name->m_lock` 的逐个 `release_lock(MDL_EXPLICIT, ticket)`。
- `MDL_context::release_locks(visitor)`：遍历 `MDL_EXPLICIT` 时长的票据，对 `visitor->release(ticket)` 为真的逐个 `release_lock(MDL_EXPLICIT, ticket)`。
- `MDL_ticket::downgrade_lock(new_type)`：把 EXCLUSIVE 或 SHARED_NO_WRITE 降级为共享锁。源码注释限定只允许从 EXCLUSIVE 和 SHARED_NO_WRITE 降级；降级到非 obtrusive 类型时 `--m_obtrusive_locks_granted_waiting_count`，归零则清 `HAS_OBTRUSIVE`；最后 `m_lock->reschedule_waiters()` 重新调度等待者。

### 7. 死锁检测

`MDL_context::find_deadlock()` 是个循环：每轮 new 一个 `Deadlock_detection_visitor dvisitor(this)`（源码注释强调每次搜索都用新实例，选择受害者依赖这一点），调 `visit_subgraph(&dvisitor)` 找环；找到环则取 `dvisitor.get_victim()`，把受害者的等待状态置为 `MDL_wait::VICTIM` 并调 `victim->unlock_deadlock_victim()`；若受害者就是自己则跳出循环，否则继续搜索。源码注释解释：由于移除一条边不能保证消灭所有由新边造成的环，必须重复搜索。

等待中的线程最终通过 `MDL_wait::VICTIM` 状态拿到 `ER_LOCK_DEADLOCK`（见上文错误分派表）。

### 8. 存储引擎的通知钩子

在 `try_acquire_lock_impl()` 里有一条针对存储引擎的路径：若 `mdl_request->type == MDL_EXCLUSIVE` 且 `MDL_lock::needs_hton_notification(key->mdl_namespace())`，则先 `notify_hton_pre_acquire_exclusive(key, &victimized)` 通知 SE；SE 拒绝时 `my_error(victimized ? ER_LOCK_DEADLOCK : ER_LOCK_REFUSED_BY_ENGINE, MYF(0))` 并销毁票据返回 `true`。成功则置 `ticket->m_hton_notified = true`。

对称地，失败路径与释放路径都要做 `notify_hton_post_release_exclusive()`——源码注释解释：既然已经通知过 SE「即将获取锁」，那么获取失败就必须做与「锁被释放」等价的通知。

### 9. 可观测性

`sql/mdl.cc` 为 MDL 子系统注册了 Performance Schema 的插桩点：互斥锁 `MDL_wait::LOCK_wait_status`、读写锁 `MDL_lock::rwlock` 与 `MDL_context::LOCK_waiting_for`、条件变量 `MDL_context::COND_wait_status`、内存 `MDL_context::acquire_locks`（用途注释为「排序锁请求的缓冲」）。

`MDL_key::m_namespace_to_wait_state_name[]` 数组给每个命名空间配一个 Performance Schema 线程状态名，例如 "Waiting for table metadata lock"、"Waiting for global read lock"、"Waiting for backup lock"、"Waiting for acl cache lock" 等，共 18 个命名空间（数组长度 `NAMESPACE_END`）。这些名字会作为 `MDL_wait::timed_wait()` 的第四个参数传下去，因此卡在 MDL 上的线程在 Performance Schema 里能显示它具体在等哪个命名空间。

## 关键取舍

- **快路径 vs 慢路径**：源码把锁类型分成「unobtrusive」（彼此全部相容、常见于 DML）与「obtrusive」（与其它类型或自身相容性受限、不常见于 DML）两档。按对象锁的分类是 S/SH/SR/SW/SWLP 为前者，SU/SRO/SNW/SNRW/X 为后者。快路径用一个打包在 `m_fast_path_state` 里的整数计数器代替对 `m_granted`/`m_waiting` 位图与链表的反复操作，源码注释说这样「减少与 `MDL_lock::m_rwlock` 相关的临界区大小，从而提升可扩展性」。代价是：一旦场上出现 obtrusive 锁，所有快路径请求都必须降级走慢路径，而且必须先 `materialize_fast_path_locks()` 把快路径票据物化进 `m_granted` 链表——这一步是 O(票据数) 的。

- **快路径计数器的位分配是固定死的**：bits 0..19 存 S/SH，bits 20..39 存 SR，bits 40..59 存 SW/SWLP，每段 20 位。源码注释说不会溢出，因为「不太可能支持超过 2^20 - 1 个并发连接」。**架构推论：** 这个上限是硬编码在策略表里的常量，不是可配置项；源码里没有对应的系统变量。

- **用 `max_write_lock_count` 限制「hog」类型的连发授予**：`MDL_OBJECT_HOG_LOCK_TYPES` 定义了 hog 类型（SNW、SNRW、X），配合 `m_hog_lock_count` / `m_piglet_lock_count` 两个计数器与 4 张优先级矩阵，保证低优先级请求不会被高优先级请求饿死。代价是矩阵切换逻辑相当复杂——`reschedule_waiters()` 末尾有一大段退档逻辑，源码注释逐条解释了为什么某些切换「不需要 reschedule」（因为切换发生时对应的等待队列已空）。

- **`ACL_CACHE` 命名空间被特殊对待**：三处特殊——(1) `is_affected_by_max_write_lock_count()` 对它返回假，关掉 hog/piglet 计数；(2) `acquire_lock()` 里对它的 S 锁请求跳过立即死锁检测，或推迟 1 秒再检测；(3) `object_lock_needs_connection_check()` 把它列入需要检查连接状态的命名空间。源码注释解释理由：它是单例锁，每个连接甚至每条语句都要取 S 锁，连接数多时死锁检测开销不可接受。

- **等待失败时错误码三分**：死锁受害者报 `ER_LOCK_DEADLOCK`，超时报 `ER_LOCK_WAIT_TIMEOUT`，被 kill 报 `ER_QUERY_TIMEOUT`（当 `get_owner()->is_killed() == ER_QUERY_TIMEOUT`）或 `ER_QUERY_INTERRUPTED`。三者在 `acquire_lock()` 的失败分支里由 `switch (wait_status)` 显式分派，不是靠统一的错误映射表。

- **升级失败保持原状**：`upgrade_shared_lock()` 的注释明确写「升级失败（例如升级者被 kill）时，锁保持原来的共享模式」。代价是 DDL 中途失败后需要额外的清理路径，收益是不会出现「锁类型被改了一半」的状态。

- **克隆票据以隔离 HANDLER 与事务的寿命**：`clone_ticket()` 让 HANDLER 锁与事务锁各自持有独立票据，源码注释列出三个具体后果——`HANDLER CLOSE` 不会释放事务锁、`COMMIT` 不会误释放 HANDLER 锁、`HANDLER CLOSE` 关一个别名不会释放另一个别名打开的表。

- **`release_lock()` 先摘票据再放锁**：`m_ticket_store.remove(duration, ticket)` 在 `lock->remove_ticket()` 之前。源码注释解释：摘除过程会引用 `MDL_lock::m_key`，票据表若使用哈希二级索引，先摘才能安全引用。

- **票据按 duration 分链、按逆时间序存放**：`m_ticket_store` 为 `MDL_STATEMENT` / `MDL_TRANSACTION` / `MDL_EXPLICIT` 各维护一条链，新的票据 `push_front` 到链头。源码注释说明这个逆序设计让 `release_locks_stored_before()` 一个函数就能服务「冲突回退」「语句/事务结束全释放」「回滚到保存点」三个场景。

- **单例对象不计入「未使用」计数**：快慢两条路径在释放时都判断 `mdl_locks.is_lock_object_singleton(&lock->key)`，只有非单例才调 `lock_object_unused()`。源码注释的规则是「用 `pinned == false` 表明是单例」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/mdl.cc | `MDL_context::acquire_lock` | 单锁申请总入口：零超时走 `try_acquire_lock()`；否则先 `try_acquire_lock_impl()`，失败则入等待队列、跑 `find_deadlock()`、长等待，最后按 `MDL_wait` 状态分派 `ER_LOCK_DEADLOCK` / `ER_LOCK_WAIT_TIMEOUT` / `ER_QUERY_TIMEOUT` / `ER_QUERY_INTERRUPTED` |
| sql/mdl.cc | `MDL_context::release_lock` | 释放主实现：先 `m_ticket_store.remove(duration, ticket)` 摘票据，快路径票据在 `m_fast_path_state` 上 CAS 减计数、慢路径票据走 `MDL_lock::remove_ticket()`，最后 `notify_hton_post_release_exclusive()` + `MDL_ticket::destroy()` |
| sql/mdl.cc | `MDL_context::try_acquire_lock` | 不等待的加锁尝试包装；失败时回滚 `HAS_SLOW_PATH` / `HAS_OBTRUSIVE` 标志并销毁票据 |
| sql/mdl.cc | `MDL_context::try_acquire_lock_impl` | 真正的加锁实现：复用已有票据（必要时 `clone_ticket()`）、决定快/慢路径、obtrusive 请求先 `materialize_fast_path_locks()`、X 锁先通知 SE、慢路径用 `can_grant_lock()` 判定 |
| sql/mdl.cc | `MDL_context::materialize_fast_path_locks` | 把快路径票据物化进 `MDL_lock::m_granted` 链表并从 `m_fast_path_state` 计数器中扣除，保证 `can_grant_lock()` 能假定快路径锁属于不同上下文 |
| sql/mdl.cc | `MDL_context::clone_ticket` | 复制一张已授予票据，隔离 HANDLER 锁与事务锁的寿命 |
| sql/mdl.cc | `MDL_context::find_ticket` | 按（对象，强度）在票据表中查找可复用票据，实现 `m_ticket_store.find(*mdl_request)` |
| sql/mdl.cc | `MDL_context::upgrade_shared_lock` | 共享锁升级：先按新类型申请 `MDL_TRANSACTION` 时长的锁，再在 `m_rwlock` 保护下把旧票据的 `m_type` 改掉并重挂 `m_granted`；失败保持原状 |
| sql/mdl.cc | `MDL_context::find_deadlock` | 死锁检测循环：每轮新建 `Deadlock_detection_visitor`，`visit_subgraph()` 找环，取受害者并置 `MDL_wait::VICTIM` |
| sql/mdl.cc | `MDL_context::release_locks_stored_before` | 按 duration 逆时间序释放票据，可指定哨兵提前停止；服务于事务结束、语句结束与回滚到保存点 |
| sql/mdl.cc | `MDL_context::release_transactional_locks` | 事务结束时的释放入口：先释放 `MDL_STATEMENT` 再释放 `MDL_TRANSACTION` 时长的全部票据 |
| sql/mdl.cc | `MDL_context::release_statement_locks` | 语句结束时的释放入口：只释放 `MDL_STATEMENT` 时长的票据 |
| sql/mdl.cc | `MDL_context::rollback_to_savepoint` | 回滚到保存点：分别以保存点里的 `m_stmt_ticket` / `m_trans_ticket` 为哨兵释放之后取得的票据 |
| sql/mdl.cc | `MDL_context::release_all_locks_for_name` | 释放同一 `MDL_lock` 对象下的全部 `MDL_EXPLICIT` 票据 |
| sql/mdl.cc | `MDL_context::release_locks` | 按 `MDL_release_locks_visitor` 的判定释放选中的 `MDL_EXPLICIT` 票据 |
| sql/mdl.cc | `MDL_context::has_locks` | 遍历各 duration 的票据链，判断本上下文是否持有指定命名空间的锁 |
| sql/mdl.cc | `MDL_context::has_locks_waited_for` | 判断本上下文持有的锁是否正在被别人等待 |
| sql/mdl.cc | `MDL_context::find_lock_owner` | 按 key 找到 `MDL_lock` 并调 `visitor->visit_context(owner)` 访问持有者 |
| sql/mdl.cc | `MDL_context::fix_pins` | 为访问 `MDL_map` 分配 `LF_PINS`；分配失败报 `ER_MDL_OUT_OF_RESOURCES` |
| sql/mdl.cc | `MDL_context::destroy` | 断言票据表已空，销毁 `m_LOCK_waiting_for` 并归还 pins |
| sql/mdl.cc | `MDL_context::set_lock_duration` | 把事务锁改到别的 duration（只允许从 `MDL_TRANSACTION` 改出），并同步 Performance Schema 的 `metadata_locks` 表的 DURATION 列 |
| sql/mdl.cc | `MDL_lock::can_grant_lock` | 可授予判定：等待位图/快路径已授予位图/已授予位图三重检查，且冲突票据必须属于其它上下文 |
| sql/mdl.cc | `MDL_lock::reschedule_waiters` | 等待队列调度：逐个 `can_grant_lock()`，授予后把票据从 `m_waiting` 移到 `m_granted`；按 `max_write_lock_count` 切换 4 张优先级矩阵并做退档 |
| sql/mdl.cc | `MDL_lock::remove_ticket` | 从指定链表摘除票据；维护 `m_obtrusive_locks_granted_waiting_count`、`HAS_OBTRUSIVE` 与 `HAS_SLOW_PATH` 标志 |
| sql/mdl.cc | `MDL_lock::get_incompatible_waiting_types_bitmap_idx` | 按 `m_piglet_lock_count` / `m_hog_lock_count` 与 `max_write_lock_count` 算出该用第几张优先级矩阵（0..3） |
| sql/mdl.cc | `MDL_lock::count_piglets_and_hogs` | 授予 hog 类型且队列有非 hog 等待者时 `m_hog_lock_count++`；授予 `MDL_SHARED_WRITE` 且队列有 `MDL_SHARED_READ_ONLY` 等待者时 `m_piglet_lock_count++` |
| sql/mdl.cc | `MDL_lock::is_obtrusive_lock` | 判定某类型是否 obtrusive，实现为 `get_unobtrusive_lock_increment(type) == 0` |
| sql/mdl.cc | `MDL_lock::get_unobtrusive_lock_increment` | 从策略表取该类型的快路径增量，0 表示 obtrusive |
| sql/mdl.cc | `MDL_lock::is_affected_by_max_write_lock_count` | 判断该锁是否受 `max_write_lock_count` 机制约束；对 `ACL_CACHE` 命名空间显式返回假 |
| sql/mdl.cc | `MDL_lock::notify_conflicting_locks` | 通过策略里的 `m_notify_conflicting_locks` 回调通知冲突锁的持有者 |
| sql/mdl.cc | `MDL_lock::needs_connection_check` | 通过策略里的 `m_needs_connection_check` 回调判断等待期间是否需要检查连接状态 |
| sql/mdl.cc | `MDL_lock::has_pending_conflicting_lock` | 检查该票据的锁类型是否有冲突的待授予请求 |
| sql/mdl.cc | `MDL_ticket::has_stronger_or_equal_type` | 强度比较：`!(granted_incompat_map[type] & ~granted_incompat_map[m_type])` |
| sql/mdl.cc | `MDL_ticket::is_incompatible_when_granted` / `is_incompatible_when_waiting` | 分别按已授予相容矩阵与等待相容矩阵判断某类型是否与本票据冲突 |
| sql/mdl.cc | `MDL_ticket::downgrade_lock` | 把 EXCLUSIVE 或 SHARED_NO_WRITE 降级为共享锁，降级后调 `reschedule_waiters()` |
| sql/mdl.cc | `MDL_lock::m_object_lock_strategy` | 按对象锁的策略实例：已授予相容矩阵、4 张等待优先级矩阵、快路径增量数组 `{0, 1, 1, 1ULL << 20, 1ULL << 40, 1ULL << 40, 0, 0, 0, 0, 0}` |
| sql/mdl.cc | `MDL_lock::m_scoped_lock_strategy` | 作用域锁的策略实例：只支持 SHARED/IX/EXCLUSIVE，快路径增量数组 `{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}`，`m_is_affected_by_max_write_lock_count` 为假 |
| sql/mdl.cc | `MDL_OBJECT_HOG_LOCK_TYPES` | hog 类型位图：`MDL_SHARED_NO_WRITE` | `MDL_SHARED_NO_READ_WRITE` | `MDL_EXCLUSIVE` |
| sql/mdl.cc | `object_lock_needs_connection_check` | 需要等待期间检查连接状态的命名空间：`USER_LEVEL_LOCK`、`LOCKING_SERVICE`、`ACL_CACHE` |
| sql/mdl.cc | `MDL_key::m_namespace_to_wait_state_name` | 18 个命名空间对应的 Performance Schema 线程状态名数组，长度 `NAMESPACE_END` |
| sql/mdl.cc | `MDL_request::init_with_source` | 初始化锁请求：填 `MDL_key`、`type`、`duration`，并记录发起请求的源文件与行号；注释明确 MDL 子系统不管理请求对象的内存 |
| sql/mdl.cc | `init_mdl_psi_keys` | 向 Performance Schema 注册 MDL 子系统的互斥锁、读写锁、条件变量与内存插桩点 |

## 与主核 / AI 的关系

**架构推论：** MDL 的核心价值不在「互斥」本身——互斥用一把大锁就能做到——而在于它把**元数据的读与写分成了两个强度**，让「读元数据」（DML，共享锁）与「改元数据」（DDL，排他锁）落在同一套优先级矩阵里。MySQL 没有为 DDL 单独设一条「DML 停摆」的开关，而是靠锁的相容性天然实现。主核若要复刻这个思路，关键不是学锁类型表，而是学**「按操作性质分档 + 分档之间定义相容矩阵」**这个建模方式。

**架构推论：** 「快路径 / 慢路径」的二分是一个可推广的并发设计：绝大多数请求落在一个**彼此全相容**的子集里，这个子集可以用一个原子计数器表示，只有当出现少数「强势请求」时才退化成完整的队列管理。源码注释明确把「减少 `m_rwlock` 临界区大小、提升可扩展性」作为设计目标。主核的多模态元数据层（catalog/）同样存在「大量并发读、少量写」的模式，这套二分可以直接套用。

**架构推论：** 优先级矩阵的 4 档设计与「hog / piglet」分类，本质是一个**反饿死机制**：把会挤占资源的请求类型命名为 hog，把容易被挤占的命名为 piglet，用计数器 + 矩阵切换限制 hog 的连发次数。这比「先来先服务」多了一层保护，但复杂度明显上升（`reschedule_waiters()` 末尾的退档逻辑就占了函数近三分之一篇幅）。主核若要在 AI 辅助调优的场景下做资源仲裁，这个取舍值得照搬，但要意识到它的维护成本。

**架构推论：** `MDL_context` 把「一次加锁意图」（`MDL_request`）与「一次加锁结果」（`MDL_ticket`）分离，并且明确声明**不拥有请求对象的内存**，是让锁子系统与 SQL 解析层解耦的手法。请求放在语句内存里，锁子系统只持票据。主核的 catalog 层若要让查询规划层参与元数据锁的协商，这个「借而不持」的边界同样适用。

**架构推论：** MDL 的错误语义被拆成三个互斥的终态（死锁 / 超时 / 被中断），而不是一个笼统的「加锁失败」。这让上层可以针对不同失败给出不同恢复策略——死锁可重试、超时通常应回退整个语句、被中断则应立即放弃。主核的并发控制接口若只返回一个布尔值，就丢失了这层信息。

## 参考

- [MySQL 参考手册：Metadata Locking](https://dev.mysql.com/doc/refman/8.0/en/metadata-locking.html) —— 元数据锁的官方语义：MDL 与 InnoDB 行锁的分工、DDL 被 DML 阻塞时的表现、metadata_locks 与 performance_schema 的可观测口径。
- [MySQL 参考手册：INNODB LOCKS 与元数据锁的关系](https://dev.mysql.com/doc/refman/8.0/en/innodb-locking.html) —— InnoDB 行锁/意向锁的官方描述，用于划清与本页元数据锁的边界。
- [MySQL 参考手册：INFORMATION_SCHEMA `METADATA_LOCK_INFO` 与 `PERFORMANCE_SCHEMA.metadata_locks`](https://dev.mysql.com/doc/refman/8.0/en/information-schema-metadata-lock-info-table.html) —— MDL 在系统视图里的官方可观测口径。
- [mysql/mysql-server 源码：sql/mdl.cc（mysql-8.0.40 tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/mdl.cc) —— `MDL_context::acquire_lock()` / `MDL_context::release_lock()` 的实现、快慢路径、优先级矩阵与死锁检测。
- [mysql/mysql-server 源码：sql/mdl.h（mysql-8.0.40 tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/mdl.h) —— `MDL_request` / `MDL_ticket` / `MDL_key` / `MDL_context` 的声明与锁类型、时长枚举定义。
