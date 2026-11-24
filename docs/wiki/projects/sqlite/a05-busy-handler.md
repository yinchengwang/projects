---
project: sqlite
feature: busy-handler
page_type: feature
schema_version: 1
refs:
  - path: src/main.c
    symbols: [sqlite3_busy_handler, sqlite3_busy_timeout, sqlite3InvokeBusyHandler]
---

# 锁冲突忙等待回调

## 一句话职责

`sqlite3_busy_handler()` 把用户注册的忙等待回调挂到连接上，`sqlite3_busy_timeout()` 用内置的默认回调把这套机制退化成「按毫秒数睡眠重试」，`sqlite3InvokeBusyHandler()` 则是锁获取失败后真正驱动回调并维护重试计数的那个内部入口。

## 机制是什么

SQLite 的文件锁（POSIX `fcntl` / Windows `LockFileEx`）在拿不到时会立刻返回 `SQLITE_BUSY`，不会自己排队。忙等待回调就是补在这个缺口上的「重试策略层」：应用可以注册一个函数，SQLite 在遇到锁冲突时反复调用它，直到它返回 0 表示「别再等了」。

回调签名是 `int (*)(void *pArg, int count)`：

- 第一个参数是 `sqlite3_busy_handler()` 第三个参数原样传回的 `void *`，应用自己决定它是什么；
- 第二个参数 `count` 是「同一个锁冲突事件里，回调已经被调用过多少次」。

回调返回值决定走向：返回非 0 表示「继续重试」，SQLite 再次尝试获取锁；返回 0 表示「放弃」，操作以 `SQLITE_BUSY` 失败。

这套机制有三条硬性约束（`src/sqlite.h.in` 的 `CAPI3REF: Register A Callback To Handle SQLITE_BUSY Errors` 一节写明）：

1. **每个连接同时只能有一个忙等待回调**。设新的会清掉旧的——`sqlite3_busy_handler()` 在 `src/main.c` 里无条件写 `db->busyHandler.xBusyHandler = xBusy;` 并把 `db->busyHandler.nBusy` 归零，所以旧回调连「已经等过几轮」的状态都不保留。
2. **回调不可重入**。文档写明回调里「不应做任何修改调用它的那个数据库连接的动作」，否则行为未定义；回调也不许关闭调用它的连接或预编译语句。
3. **有忙等待回调不等于一定被调用**。当 SQLite 判断调用回调会导致死锁时，它会直接返回 `SQLITE_BUSY` 而不调用回调。文档给出的死锁场景是：进程一持读锁想升到 reserved 锁，进程二持 reserved 锁想升到 exclusive 锁，两者互相阻塞，谁都前进不了；SQLite 对进程一直接返回 `SQLITE_BUSY`，指望它先放掉读锁让进程二走。

默认状态是「没有回调」——`db->busyHandler.xBusyHandler` 初始为空，此时遇到锁冲突立即返回 `SQLITE_BUSY`。

边界上要注意：忙等待回调只处理「拿不到锁时怎么办」，它不负责锁本身的获取与释放（那是分页器文件锁那一组机制的职责），也不负责页缓存层面的固定页锁。

## 怎么实现的

### 状态存放在连接的 BusyHandler 结构里

`BusyHandler` 结构定义在 `src/sqliteInt.h`，三个字段：

```c
struct BusyHandler {
  int (*xBusyHandler)(void *,int);  /* The busy callback */
  void *pBusyArg;                   /* First arg to busy callback */
  int nBusy;                        /* Incremented with each busy call */
};
```

`src/sqliteInt.h` 里这组字段上方的注释写明：「Each pager opened via the sqlite handle is passed a pointer to sqlite.busyHandler. The busy callback is currently invoked only from within pager.c.」——连接上只有一个 `busyHandler` 实例，各分页器共享它。连接结构 `sqlite3` 里相应地有两个字段：`BusyHandler busyHandler;`（注释为 `Busy callback`）和 `int busyTimeout;`（注释为 `Busy handler timeout, in msec`）。

### sqlite3_busy_handler()：登记回调

`src/main.c` 里 `sqlite3_busy_handler()` 的函数体很短，顺序是：

1. 编译了 `SQLITE_ENABLE_API_ARMOR` 时先 `sqlite3SafetyCheckOk(db)`，不通过返回 `SQLITE_MISUSE_BKPT`；
2. `sqlite3_mutex_enter(db->mutex)` 拿连接互斥量；
3. 写 `db->busyHandler.xBusyHandler = xBusy`、写 `db->busyHandler.pBusyArg = pArg`、写 `db->busyHandler.nBusy = 0`；
4. 写 `db->busyTimeout = 0`——**注意这一步会顺手把之前设的超时清零**，即使新回调就是超时回调本身（`sqlite3_busy_timeout()` 会在调用本函数之后重新把 `busyTimeout` 设回 `ms`，所以那条路径不受影响）；
5. 编译了 `SQLITE_ENABLE_SETLK_TIMEOUT` 时额外写 `db->setlkTimeout = 0`（`src/sqliteInt.h` 里该字段注释为 `Blocking lock timeout, in msec. -1 -> inf.`）；
6. `sqlite3_mutex_leave(db->mutex)` 后返回 `SQLITE_OK`。

函数体没有任何错误分支——除了 API armor 下的 misuse 检查，它总是返回 `SQLITE_OK`。

### sqlite3InvokeBusyHandler()：驱动回调并计数

`src/main.c` 里 `sqlite3InvokeBusyHandler(BusyHandler *p)` 是整个机制的驱动点，函数体只有一个分支：

```c
int sqlite3InvokeBusyHandler(BusyHandler *p){
  int rc;
  if( p->xBusyHandler==0 || p->nBusy<0 ) return 0;
  rc = p->xBusyHandler(p->pBusyArg, p->nBusy);
  if( rc==0 ){
    p->nBusy = -1;
  }else{
    p->nBusy++;
  }
  return rc;
}
```

三个行为要点：

- **没注册回调**（`xBusyHandler == 0`）直接返回 0，调用方据此放弃重试。
- **`nBusy < 0` 是一个「已经放弃」的粘性标记**：回调返回 0 时把 `nBusy` 写成 -1，此后每次再进来都因 `p->nBusy<0` 直接返回 0，不再调用用户回调。注释在 `src/sqliteInt.h` 里对 `nBusy` 的描述是 `Incremented with each busy call`，而 -1 这个取值就是「终止信号」。
- **计数是跨调用累加的**：回调返回非 0 时 `nBusy++`，所以回调拿到的 `count` 参数是「此前已被调用过多少次」。

### sqlite3_busy_timeout()：把机制退化成睡眠重试

`src/main.c` 里 `sqlite3_busy_timeout(sqlite3 *db, int ms)` 的函数体：

- API armor 检查后 `sqlite3_mutex_enter(db->mutex)`；
- **`ms > 0` 时**：调 `sqlite3_busy_handler(db, (int(*)(void*,int))sqliteDefaultBusyCallback, (void*)db)` 把默认回调装上（回调参数就是连接本身），然后写 `db->busyTimeout = ms`；编译了 `SQLITE_ENABLE_SETLK_TIMEOUT` 时再写 `db->setlkTimeout = ms`。
- **`ms <= 0` 时**：调 `sqlite3_busy_handler(db, 0, 0)`，即清空回调——这与 `src/sqlite.h.in` 里「参数小于等于零会关闭所有忙等待处理器」的文档一致。
- 最后 `sqlite3_mutex_leave(db->mutex)`，返回 `SQLITE_OK`。

注意 `sqlite3_busy_handler()` 会把 `db->busyTimeout` 清零，所以顺序上必须先装回调再写 `db->busyTimeout = ms`——源码正是这个顺序。

### 默认回调 sqliteDefaultBusyCallback()：递增式退避表

`src/main.c` 里 `sqliteDefaultBusyCallback()` 是 `static` 函数，通过强制类型转换 `(int(*)(void*,int))` 适配成 `sqlite3_busy_handler()` 要求的签名。函数体分两支，由 `#if SQLITE_OS_WIN || !defined(HAVE_NANOSLEEP) || HAVE_NANOSLEEP` 选择。

**主分支（支持亚秒级睡眠的系统，源码注释举例「All windows systems, unix systems with nanosleep()」）** 用两张静态表：

```c
static const u8 delays[] =
   { 1, 2, 5, 10, 15, 20, 25, 25,  25,  50,  50, 100 };
static const u8 totals[] =
   { 0, 1, 3,  8, 18, 33, 53, 78, 103, 128, 178, 228 };
```

`delays` 是每次要睡的毫秒数，`totals` 是「睡到这一轮为止的累计毫秒数」。表长由 `ArraySize(delays)` 得到，值 12；`ArraySize` 宏定义在 `src/sqliteInt.h`，是 `((int)(sizeof(X)/sizeof(X[0])))`。

取值的逻辑是：

- `count < NDELAY`（即 `count < 12`）时，`delay = delays[count]`、`prior = totals[count]`；
- 否则（`count >= 12`）取表尾一档：`delay = delays[NDELAY-1]`（即 `delays[11]` = 100 毫秒），`prior = totals[NDELAY-1] + delay*(count-(NDELAY-1))`，即累计时间线性外推。

然后做超时裁剪：`if( prior + delay > tmout )`（`tmout` 取自 `db->busyTimeout`）就把 `delay` 改成 `tmout - prior`；若裁剪后 `delay <= 0` 就 `return 0`——这正是「睡眠总时长已累计到 `ms` 毫秒，于是回调放弃，让操作以 `SQLITE_BUSY` 收场」的实现点。最后调 `sqlite3OsSleep(db->pVfs, delay*1000)`（`delay` 毫秒换算成微秒），返回 1 表示继续重试。

`sqlite3OsSleep()` 在 `src/os.c` 里只有一行实质内容：`return pVfs->xSleep(pVfs, nMicro);`，即转给 VFS 的 `xSleep` 回调。

**备分支（缺少 `usleep` 支持的 unix 系统，源码注释写明「Sleeping must be done in increments of whole seconds」）** 只能按整秒睡：条件是 `(count+1)*1000 > tmout` 就 `return 0`；否则 `sqlite3OsSleep(db->pVfs, 1000000)`（即 1 秒）后返回 1。

### 调用链：谁真的会触发回调

从锁失败到回调被调，中间隔着两层转发：

1. `src/pager.c` 的 `sqlite3PagerSetBusyHandler(Pager *pPager, int (*xBusyHandler)(void *), void *pBusyHandlerArg)` 把回调登记到分页器上：写 `pPager->xBusyHandler` 与 `pPager->pBusyHandlerArg`，然后调 `sqlite3OsFileControlHint(pPager->fd, SQLITE_FCNTL_BUSYHANDLER, (void *)ap)`，把这两个指针通过文件控制提示传给 VFS 层。
2. `src/pager.c` 的 `pager_wait_on_lock(Pager *pPager, int locktype)` 是重试循环本体：

```c
do {
  rc = pagerLockDb(pPager, locktype);
}while( rc==SQLITE_BUSY && pPager->xBusyHandler(pPager->pBusyHandlerArg) );
```

即：先调 `pagerLockDb()` 尝试加锁，若返回 `SQLITE_BUSY` 就调分页器上登记的回调，回调返回非 0 则再循环一次。`pagerLockDb()` 内部真正落锁的地方是 `sqlite3OsLock(pPager->fd, eLock)`（`pPager->noLock` 为真时直接返回 `SQLITE_OK`，跳过 OS 层加锁）。

3. 分页器上登记的回调不是用户回调，而是 `src/btree.c` 的 `btreeInvokeBusyHandler(void *pArg)`——它在 `src/btree.c` 的 `sqlite3BtreeOpen()` 里被装上去：`sqlite3PagerSetBusyHandler(pBt->pPager, btreeInvokeBusyHandler, pBt);`。`btreeInvokeBusyHandler()` 的函数体是取出 `BtShared *pBt`、断言 `sqlite3_mutex_held(pBt->db->mutex)`，然后 `return sqlite3InvokeBusyHandler(&pBt->db->busyHandler);`——这才是把分页器层的「拿不到锁」翻译成连接层「调用用户回调」的那一步。

所以完整链条是：`pager_wait_on_lock()` → `pPager->xBusyHandler`（即 `btreeInvokeBusyHandler`）→ `sqlite3InvokeBusyHandler()` → 用户的 `xBusyHandler(pBusyArg, nBusy)`。

### 哪些锁转换会触发回调

`src/pager.c` 里 `sqlite3PagerSetBusyHandler()` 上方的注释用表格写清了触发条件：

| Transition | Invokes xBusyHandler |
|---|---|
| `NO_LOCK` → `SHARED_LOCK` | Yes |
| `SHARED_LOCK` → `RESERVED_LOCK` | No |
| `SHARED_LOCK` → `EXCLUSIVE_LOCK` | No |
| `RESERVED_LOCK` → `EXCLUSIVE_LOCK` | Yes |

同一段注释给出的理由是：`sqlite3OsLock()` 返回 `SQLITE_BUSY` 只发生在「从无锁升到共享锁」和「从 reserved 升到 exclusive」这两种转换上；升 SHARED→RESERVED、以及热日志回滚期间的 SHARED→EXCLUSIVE 都不产生 `SQLITE_BUSY`。

`pager_wait_on_lock()` 里的断言 `assert( (pPager->eLock>=locktype) || (pPager->eLock==NO_LOCK && locktype==SHARED_LOCK) || (pPager->eLock==RESERVED_LOCK && locktype==EXCLUSIVE_LOCK) )` 与这张表一一对应——只有这三种情况才可能进入重试循环。

### PRAGMA busy_timeout 走的是同一条路

`src/pragma.c` 的 `PragTyp_BUSY_TIMEOUT` 分支里，若 `zRight && sqlite3GetInt32(zRight,&n)` 成立就调 `sqlite3_busy_timeout(db, n)`，然后 `returnSingleInt(v, db->busyTimeout)` 把当前超时值回给应用。源码注释写明：设置了超时且用的是默认回调时回读得到该值；如果装的是别的忙等待处理器则返回 0。

### WAL 模式下的旁路

`src/pager.c` 里 WAL 检查点路径也用到了同一个回调槽：`sqlite3PagerCheckpoint()` 相关的调用在选择检查点模式时把 `pPager->xBusyHandler` 作为参数传下去（`eMode<=SQLITE_CHECKPOINT_PASSIVE ? 0 : pPager->xBusyHandler`）——即被动检查点不调用忙等待回调，阻塞式/全量检查点才会。

## 关键取舍

**回调只有一个，且设新的就清旧的——换来「状态简单」。** `sqlite3_busy_handler()` 直接覆盖 `xBusyHandler` 并把 `nBusy` 归零，不保留任何历史状态。代价是 `sqlite3_busy_timeout()` 和 `PRAGMA busy_timeout` 会把用户先前装的回调顶掉，反过来用户再调 `sqlite3_busy_handler()` 又会把设好的超时清零（因为该函数无条件写 `db->busyTimeout = 0`）。这是 `src/sqlite.h.in` 文档里专门加了一段「Note that calling sqlite3_busy_timeout() or evaluating PRAGMA busy_timeout=N will change the busy handler and thus clear any previously set busy handler」的原因。

**用「回调返回 0 即终止」而不是「回调抛错」——换来 C API 的简单性。** 回调只有一个返回值，没有错误码通道，也没法告诉 SQLite「换个策略」。终止信号通过把 `nBusy` 写成 -1 变成粘性的，后续调用直接短路。

**用递增退避表而不是固定间隔——换来「等待越久、探测越稀疏」。** `delays` 表从 1 毫秒起步，逐步放大到 100 毫秒；`totals` 表记录累计值，让超时判断精确到「累计睡眠已达 `ms`」。表长固定 12 档（第 12 档后固定按 100 毫秒线性外推累计时间），所以长等待不会因为次数变多而让表无限增长。

**亚秒睡眠与整秒睡眠分成两个编译分支——换来对老 unix 系统的兼容。** 主分支走 `sqlite3OsSleep()` 的微秒接口；备分支因为缺少 `usleep` 只能按 `1000000` 微秒（1 秒）粒度睡，超时判断相应地退化成 `(count+1)*1000 > tmout`。

**忙等待只覆盖两种锁转换——换来「不会制造死锁」。** `NO_LOCK → SHARED_LOCK` 与 `RESERVED_LOCK → EXCLUSIVE_LOCK` 之外的转换不进入重试循环。加上文档写明的「判断会死锁就直接返回 `SQLITE_BUSY`」，SQLite 的策略是宁可失败也不无限互等。

**忙等待回调不看错误类型——换来「回调不需要理解错误码」。** `sqlite3InvokeBusyHandler()` 只区分「回调返回 0 / 非 0」两种情况，`pager_wait_on_lock()` 也只在 `rc==SQLITE_BUSY` 这个具体值上重试，其它错误码（`SQLITE_IOERR` 之类）直接透传。

**回调在锁竞争时可能完全不被调用——这是文档级的显式免责。** `src/sqlite.h.in` 写明「The presence of a busy handler does not guarantee that it will be invoked when there is lock contention」，并给出了两进程互相等待升级锁的具体场景，说明 SQLite 对其中一方直接返回 `SQLITE_BUSY`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/main.c | `sqlite3_busy_handler` | 登记忙等待回调：写 `xBusyHandler`/`pBusyArg`/`nBusy=0`，并把 `db->busyTimeout` 清零、`setlkTimeout` 清零 |
| src/main.c | `sqlite3_busy_timeout` | 装上默认回调并设置 `db->busyTimeout = ms`；`ms <= 0` 时清空回调 |
| src/main.c | `sqlite3InvokeBusyHandler` | 驱动回调并维护 `nBusy` 计数；回调返回 0 时把 `nBusy` 置 -1 作为粘性终止标记 |
| src/main.c | `sqliteDefaultBusyCallback` | 默认回调：按 `delays`/`totals` 两张 12 档递增表退避，按 `db->busyTimeout` 裁剪末次睡眠时长 |
| src/main.c | `sqlite3_setlk_timeout` | 同一族的阻塞锁超时接口，`ms < -1` 返回 `SQLITE_RANGE`；编译了 `SQLITE_ENABLE_SETLK_TIMEOUT` 时写 `db->setlkTimeout` |
| src/sqliteInt.h | `struct BusyHandler` | 忙等待状态三字段定义：`xBusyHandler`、`pBusyArg`、`nBusy` |
| src/sqliteInt.h | `ArraySize` | `((int)(sizeof(X)/sizeof(X[0])))`，退避表长度由此得到，值为 12 |
| src/btree.c | `btreeInvokeBusyHandler` | 分页器回调 → 连接层回调的转发点：调 `sqlite3InvokeBusyHandler(&pBt->db->busyHandler)` |
| src/btree.c | `sqlite3BtreeOpen` | 在此处调 `sqlite3PagerSetBusyHandler(pBt->pPager, btreeInvokeBusyHandler, pBt)` 完成接线 |
| src/pager.c | `sqlite3PagerSetBusyHandler` | 把回调与参数存进 `Pager`，并用 `SQLITE_FCNTL_BUSYHANDLER` 文件控制提示告知 VFS 层 |
| src/pager.c | `pager_wait_on_lock` | 重试循环本体：`pagerLockDb()` 返回 `SQLITE_BUSY` 且回调返回非 0 时继续循环 |
| src/pager.c | `pagerLockDb` | 真正落锁处：`sqlite3OsLock(pPager->fd, eLock)`；`pPager->noLock` 为真时跳过 OS 加锁 |
| src/pragma.c | `PragTyp_BUSY_TIMEOUT` 分支 | `PRAGMA busy_timeout` 的实现：调 `sqlite3_busy_timeout(db, n)`，回读 `db->busyTimeout` |
| src/os.c | `sqlite3OsSleep` | 转调 `pVfs->xSleep(pVfs, nMicro)`，是默认回调真正睡眠的出口 |
| src/sqlite.h.in | `CAPI3REF: Register A Callback To Handle SQLITE_BUSY Errors` | 官方语义：回调签名、单回调约束、不可重入、可能因死锁而不被调用 |

## 与主核 / AI 的关系

**架构推论：** 主核（`engine/db/`）的分层规范里 L2 `txn/` 是负责事务与锁的层。若主核要在写事务撞上锁冲突时提供重试，SQLite 这套「连接级单回调 + 返回 0 即终止 + 计数粘性标记」的模型是一个可直接对照的最小契约：重试策略是连接/会话级状态，不是每条语句级状态。

**架构推论：** SQLite 把「锁升级的哪两种转换允许重试」写进 `sqlite3PagerSetBusyHandler()` 的注释表格，而不是靠散落在代码里的隐式假设。主核若要在存储层做重试，同样需要一张显式的「转换 → 是否可重试」对照表，否则开发者无法判断某条路径为什么没有等待。

**架构推论：** `sqlite3InvokeBusyHandler()` 用 `nBusy = -1` 这一粘性负值表达「已放弃」，而不是新增一个布尔字段。主核的等待/退避状态机若要避免多字段不一致，写法上更接近单计数器加哨兵值。

**架构推论：** 嵌入式 SDK（`engine/src/sdk/`）只依赖 SQLite 一个外部库，它若在多线程下共享同一个 SQLite 连接，就要意识到忙等待回调是「每个连接只能有一个」的槽位——后设置的会顶掉先设置的，这与主核分层规范里「共享状态必须收敛到一层」的纪律是同一个问题在不同层面的表现。

## 参考

- [Register A Callback To Handle SQLITE_BUSY Errors — sqlite.org C 接口文档](https://sqlite.org/c3ref/busy_handler.html)：`sqlite3_busy_handler()` 的官方语义，含回调两个参数的含义、返回 0 与非 0 的后果、单回调约束、不可重入约束，以及因死锁而不调用回调的说明。
- [Set A Busy Timeout — sqlite.org C 接口文档](https://sqlite.org/c3ref/busy_timeout.html)：`sqlite3_busy_timeout()` 的官方语义，含「参数小于等于零会关闭所有忙等待处理器」与「会清掉先前设置的其他忙等待处理器」。
- [sqlite/sqlite 仓库 src/main.c — GitHub 源码页](https://github.com/sqlite/sqlite/blob/master/src/main.c)：`sqlite3_busy_handler()`、`sqlite3_busy_timeout()`、`sqlite3InvokeBusyHandler()` 与 `sqliteDefaultBusyCallback()` 的实现原文。
- [sqlite.org Fossil 源码树 src/main.c](https://sqlite.org/src/doc/trunk/src/main.c)：SQLite 官方 Fossil 仓库中 `src/main.c` 的在线浏览版。
- [PRAGMA busy_timeout — sqlite.org](https://sqlite.org/pragma.html#pragma_busy_timeout)：`PRAGMA busy_timeout` 的官方说明及其与 `sqlite3_busy_timeout()` 的关系。
