---
project: sqlite
feature: unlock-notify
page_type: feature
schema_version: 1
refs:
  - path: src/notify.c
    symbols: [sqlite3_unlock_notify, sqlite3ConnectionBlocked, sqlite3ConnectionUnlocked, sqlite3ConnectionClosed]
---

# 解锁通知

## 一句话职责

在共享缓存（shared-cache）模式下，一个连接因拿不到表锁而收到 `SQLITE_LOCKED` 后，`sqlite3_unlock_notify` 让它登记一个回调；持锁连接结束事务、释放全部表锁时，SQLite 用这个回调把等待方唤醒，并顺带检测并拒绝死锁登记。

## 机制是什么

解锁通知是 SQLite 为**共享缓存模式**准备的「锁等待」替代方案。共享缓存下多个连接共享同一份页缓存，锁的粒度落到**表级**：读锁可多连接共享，写锁同一时刻只能有一个连接持有（见 `https://sqlite.org/sharedcache.html` 的 "Table Level Locking" 一节）。当某连接需要的锁拿不到时，SQLite **不阻塞**，而是直接返回 `SQLITE_LOCKED`（`src/sqlite.h.in` 中定义为 `#define SQLITE_LOCKED 6`），把「让不让等」的决策权交给应用。

整个机制由三个角色构成：

- **被阻塞连接（blocked connection）**：拿锁失败的连接。
- **阻塞连接（blocking connection）**：当前持有所需锁的连接。`src/sqliteInt.h` 的 `sqlite3` 结构里用 `pBlockingConnection` 记录「谁让我拿到了 `SQLITE_LOCKED`」，用 `pUnlockConnection` 记录「我盯着谁，等它解锁」。
- **通知回调（`xUnlockNotify`）**：应用登记的 `void (*)(void **, int)` 函数。签名第二个参数是数组长度，因为 SQLite 会把**多个**等待同一个回调的连接的应用上下文指针**打包成一个数组**一次性传给回调，让应用有机会整体排序优先级。

三条关键语义（均来自 `src/sqlite.h.in` 中 `sqlite3_unlock_notify` 的公开文档注释）：

1. **回调在持锁方的事务终结处触发**，即从终结该事务的 `sqlite3_step()` 或 `sqlite3_close()` 调用内部发出。
2. **回调不可重入**：在回调里调用任何 `sqlite3_xxx` API 会导致崩溃或死锁。
3. **回调触发不代表锁已可用**，只代表「可能可用了」——触发时阻塞连接可能只释放了一部分锁。

死锁检测也是这套 API 的一部分：若登记动作会构成「A 等 B、B 等 A」的环，`sqlite3_unlock_notify` 返回 `SQLITE_LOCKED` 且**不登记**任何回调，调用方据此回滚事务。检测覆盖**任意层数的间接环**。

本机制只管「锁释放时通知等待方」与「等待方被唤醒后的处理」。表锁本身的获取与释放属于共享缓存层（`src/btree.c`），文件锁属于 pager 层，忙等待回调是另一套机制。

## 怎么实现的

### 1. 编译开关与数据结构

整个实现住在独立文件 `src/notify.c`，文件开头就是 `#ifdef SQLITE_ENABLE_UNLOCK_NOTIFY`——未定义该宏时整个文件被编译掉。`src/sqliteInt.h` 里有配套的空桩：`#define sqlite3ConnectionBlocked(x,y)`、`#define sqlite3ConnectionUnlocked(x)`、`#define sqlite3ConnectionClosed(x)`，所以不开这个开关时调用方零成本。

开启后，`sqlite3` 结构体新增五个字段（`src/sqliteInt.h`）：

```c
sqlite3 *pBlockingConnection; /* Connection that caused SQLITE_LOCKED */
sqlite3 *pUnlockConnection;           /* Connection to watch for unlock */
void *pUnlockArg;                     /* Argument to xUnlockNotify */
void (*xUnlockNotify)(void **, int);  /* Unlock notify callback */
sqlite3 *pNextBlocked;        /* Next in list of all blocked connections */
```

源码注释写明：**这些字段由 `SQLITE_MUTEX_STATIC_MAIN` 互斥锁保护，而不是由 `sqlite3.mutex` 保护**（`SQLITE_MUTEX_STATIC_MAIN` 的值为 `2`，定义在 `src/sqlite.h.in`）。这解释了为什么 `notify.c` 里的 `enterMutex()`/`leaveMutex()` 单独管理这个全局锁。

### 2. 全局阻塞链表

`src/notify.c` 用一个静态全局链表 `static sqlite3 *SQLITE_WSD sqlite3BlockedList = 0;` 串起所有「有 `pBlockingConnection` 或 `pUnlockConnection` 非空」的连接。链表按 `xUnlockNotify` 函数指针**分组排列**——`addToBlockedList` 的插入逻辑是「沿链表找到第一个 `xUnlockNotify` 不同的节点，把 db 插在它前面」，这正是后面能**批量打包回调**的分组依据。

链表操作有两条硬规则：

- `removeFromBlockedList(db)`：线性遍历，把 db 从链上摘掉；若不在链上则什么都不做。
- `addToBlockedList(db)`：按 `xUnlockNotify` 分组插入；调用方需保证 db 此前不在链上。

`checkListProperties` 是**仅在 `NDEBUG` 未定义时存在**的复杂断言（`#ifndef NDEBUG` 分支），它验证三条不变式：链表每一项至少有一个非空指针；所有 `xUnlockNotify` 相同的项必须相邻；关闭某连接时链上不得有它的引用。`NDEBUG` 定义时它退化为空宏 `# define checkListProperties(x)`。

### 3. 登记回调：`sqlite3_unlock_notify`

`src/notify.c` 的 `sqlite3_unlock_notify(db, xNotify, pArg)` 执行顺序：

1. `SQLITE_ENABLE_API_ARMOR` 打开时先 `sqlite3SafetyCheckOk(db)`，失败返回 `SQLITE_MISUSE_BKPT`。
2. `sqlite3_mutex_enter(db->mutex)`，再 `enterMutex()`（拿 `SQLITE_MUTEX_STATIC_MAIN`）。
3. 分三种情形：
   - **`xNotify==0`（取消登记）**：调用 `removeFromBlockedList(db)`，然后把 `pBlockingConnection`、`pUnlockConnection`、`xUnlockNotify`、`pUnlockArg` 四个字段**全部清零**。
   - **`db->pBlockingConnection==0`（已无阻塞方）**：直接 `xNotify(&pArg, 1);`——**在函数返回之前同步调用回调**，参数是「指向单个 `pArg` 的指针」与「元素个数 1」。这就是公开文档说的「多线程下阻塞连接可能已经结束事务，回调立即触发」。
   - **否则（正常登记）**：进入死锁检测循环
     ```c
     for(p=db->pBlockingConnection; p && p!=db; p=p->pUnlockConnection){}
     ```
     从阻塞连接出发，沿 `pUnlockConnection` 链一路走，看能否绕回 `db` 自己。**能绕回**说明存在环，置 `rc = SQLITE_LOCKED`（源码注释写 `/* Deadlock detected. */`），**不登记任何东西**。
     **不能绕回**则登记：把 `db->pUnlockConnection` 设为 `db->pBlockingConnection`，`db->xUnlockNotify = xNotify`，`db->pUnlockArg = pArg`，再 `removeFromBlockedList(db)` + `addToBlockedList(db)`（后两步是为了让 db 落到正确回调的分组里）。
4. `leaveMutex()`，断言 `!db->mallocFailed`，然后 `sqlite3ErrorWithMsg(db, rc, (rc?"database is deadlocked":0))`——**只有 `rc` 非零（即死锁）时才带上 "database is deadlocked" 消息**；`sqlite3ErrorWithMsg`（`src/util.c`）会设置 `db->errCode` 并把消息格式化进 `db->pErr`。
5. `sqlite3_mutex_leave(db->mutex)`，返回 `rc`。

关键点：**每次登记都会覆盖同一连接上的旧回调**（因为是直接赋值 `db->xUnlockNotify = xNotify`），这与公开文档「最多一个回调，新回调替换旧回调」一致。

### 4. 记录阻塞关系：`sqlite3ConnectionBlocked`

`src/notify.c` 的 `sqlite3ConnectionBlocked(db, pBlocker)` 是**内部**函数（`src/sqliteInt.h` 里声明在 `SQLITE_ENABLE_UNLOCK_NOTIFY` 分支中，不是公开 API）。它做三件事：

1. `enterMutex()`；
2. 若 `db->pBlockingConnection==0 && db->pUnlockConnection==0`（此前既没被阻塞也没在等解锁），则 `addToBlockedList(db)` 把 db 挂上链表；
3. 赋值 `db->pBlockingConnection = pBlocker;`，然后 `leaveMutex()`。

**注意它只记录「谁挡了我」，不记录「我要盯谁」**——`pUnlockConnection` 要等应用调用 `sqlite3_unlock_notify` 才被填上。这条分界是理解本机制的关键：SQLite 主动告知「是它挡的」，应用主动决定「要不要等它」。

真实调用方在 `src/btree.c`，共三处：

- `sqlite3BtreeLockTable`（共享缓存下取表锁）：当 `pBt->pWriter!=p` 且 `(pBt->btsFlags & BTS_EXCLUSIVE)!=0`（`BTS_EXCLUSIVE` 值为 `0x0040`，定义在 `src/btreeInt.h`，注释 "pWriter has an exclusive lock"）时，调用 `sqlite3ConnectionBlocked(p->db, pBt->pWriter->db)` 并返回 `SQLITE_LOCKED_SHAREDCACHE`。
- 同一函数稍后遍历 `pBt->pLock` 链表时，若发现 `pIter->pBtree!=p && pIter->iTable==iTab && pIter->eLock!=eLock`（别的连接在同表上持着不同类型的锁），同样 `sqlite3ConnectionBlocked(p->db, pIter->pBtree->db)`；若请求的是写锁，还额外置 `pBt->btsFlags |= BTS_PENDING`（`BTS_PENDING` 值为 `0x0080`，注释 "Waiting for read-locks to clear"），然后返回 `SQLITE_LOCKED_SHAREDCACHE`。
- `sqlite3BtreeBeginTrans`（开启事务）：若已有写事务在跑（`pBt->inTransaction==TRANS_WRITE`）或 `BTS_PENDING` 已置位，或 `wrflag>1` 且别的 Btree 持锁，则取 `pBlock` 后调用 `sqlite3ConnectionBlocked(p->db, pBlock)` 并置 `rc = SQLITE_LOCKED_SHAREDCACHE`。

`SQLITE_LOCKED_SHAREDCACHE` 定义在 `src/sqlite.h.in` 为 `(SQLITE_LOCKED | (1<<8))`，即 `SQLITE_LOCKED` 加一个扩展位——公开文档正是靠这个扩展错误码把「真的有阻塞连接」与「DROP TABLE/INDEX 那种没有阻塞连接」的 `SQLITE_LOCKED` 区分开。

### 5. 释放锁时通知：`sqlite3ConnectionUnlocked`

`src/notify.c` 的 `sqlite3ConnectionUnlocked(db)` 在**阻塞连接的事务终结、锁全部释放**时被调用。真实调用方在 `src/vdbeaux.c`：VDBE 停机路径里 `if( db->autoCommit ){ sqlite3ConnectionUnlocked(db); }`——**只在自动提交模式下**（此时连接确实已经不再持有任何锁）才触发。

它遍历整条 `sqlite3BlockedList`，对每一项 `p` 做三步：

**第 1 步**：`if( p->pBlockingConnection==db ) p->pBlockingConnection = 0;`——把「我被谁挡」的记录清掉。

**第 2 步**：`if( p->pUnlockConnection==db )`——这个连接正等着 db 解锁：

- 先做**回调分组切换**：若当前累积的回调 `xUnlockNotify` 与本项的 `p->xUnlockNotify` **不同**，且已有累积（`nArg!=0`），就先把已累积的那一批 `xUnlockNotify(aArg, nArg)` 发出并清零 `nArg`。这保证了**只有同一个回调函数的上下文才会被打包在一起**。
- 然后处理参数数组扩容。栈上预分配了 `void *aStatic[16]`（**固定 16 个槽位**），并配 `void **aDyn = 0` 作为动态扩容缓冲。扩容触发条件是「`nArg` 已经等于当前数组容量」——栈数组满（`nArg==ArraySize(aStatic)`）或动态数组满（`nArg == sqlite3MallocSize(aDyn)/sizeof(void*)`，`ArraySize` 宏定义为 `((int)(sizeof(X)/sizeof(X[0])))`）。扩容方式是 `sqlite3Malloc(nArg*sizeof(void *)*2)`——**新容量是旧容量的 2 倍**，并 `memcpy` 旧内容、`sqlite3_free(aDyn)` 释放旧动态缓冲。
- 扩容失败时（`sqlite3Malloc` 返回 NULL）走**降级路径**：源码注释长段说明——此时若返回错误码也没用，因为阻塞连接的事务照样会结束、回调照样发不出去，应用程序可能永远等一个不会到的回调。所以选择**把当前已累积的那一批先发出去**（`xUnlockNotify(aArg, nArg); nArg = 0;`），然后继续累积，但不再需要动态内存。源码明确说这是「次优」方案：应用会收到**多次**小批次回调而不是一次大批量回调，降低了应用按连接排优先级的能力。
- 累积本项的上下文：`aArg[nArg++] = p->pUnlockArg;`，并记下 `xUnlockNotify = p->xUnlockNotify;`，然后清空本项的 `pUnlockConnection`、`xUnlockNotify`、`pUnlockArg`。
- 整段累积过程包在 `sqlite3BeginBenignMalloc()` / `sqlite3EndBenignMalloc()` 之间——这段区间内的内存分配失败不触发 OOM 处理（良性 malloc 区间）。

**第 3 步**：`if( p->pBlockingConnection==0 && p->pUnlockConnection==0 )` 则把 `p` 从链表摘掉（`*pp = p->pNextBlocked; p->pNextBlocked = 0;`），否则前进到下一个节点。

遍历结束后，若 `nArg!=0` 发出最后一批 `xUnlockNotify(aArg, nArg);`，然后 `sqlite3_free(aDyn)` 释放动态缓冲，`leaveMutex()`。

**重要约束**：整个函数在 `enterMutex()`（`STATIC_MAIN` 锁）与 `leaveMutex()` 之间调用应用回调——**应用回调是在持有 `SQLITE_MUTEX_STATIC_MAIN` 互斥锁的状态下被调用的**。这也是为什么公开文档强调回调不可重入。

### 6. 连接关闭：`sqlite3ConnectionClosed`

`src/notify.c` 的 `sqlite3ConnectionClosed(db)` 做三步：先 `sqlite3ConnectionUnlocked(db)`（把该连接持有的锁释放掉，从而触发所有等它的连接的回调），再 `enterMutex()` + `removeFromBlockedList(db)`（把该连接自己从链表摘除），再 `checkListProperties(db)` 验证链表状态（`checkListProperties` 的第三个不变式就是「关闭 db 时链上不得有 db 的引用」），最后 `leaveMutex()`。

真实调用方在 `src/main.c` 的连接关闭路径 `sqlite3LeaveMutexAndCloseZombie` 中，源码注释写明「告诉 notify.c 中的代码：这个连接不再持有任何锁，也不再需要任何 unlock-notify 回调」，调用点在释放函数与辅助数据库数组之后。

## 关键取舍

- **整个特性由编译开关包住（源码事实）**：`src/notify.c` 全文在 `#ifdef SQLITE_ENABLE_UNLOCK_NOTIFY` 内，`src/sqliteInt.h` 里的三个内部函数在未开启时退化为空宏。代价是默认构建**没有**这个能力，应用必须显式编译才能用。
- **回调在持有 `STATIC_MAIN` 锁时被调用（源码事实）**：`sqlite3ConnectionUnlocked` 的 `enterMutex()`/`leaveMutex()` 包住了整个遍历与回调派发，因此应用回调运行在全局互斥锁保护下。代价是回调里调用任何 `sqlite3_xxx` API 会死锁——这正是公开文档「回调不可重入」的机制级原因，而不是单纯的约定。
- **回调立即触发而非排队（源码事实）**：`sqlite3_unlock_notify` 在 `db->pBlockingConnection==0` 时直接 `xNotify(&pArg, 1)`，在函数返回前同步调用完。代价是应用必须能容忍「登记与回调同时发生」——`https://sqlite.org/unlock_notify.html` 给出的 pthread 示例专门用 `fired` 标志位 + 条件变量来处理这个竞态。
- **死锁检测用「走环」而非「图搜索」（源码事实）**：检测逻辑是 `for(p=db->pBlockingConnection; p && p!=db; p=p->pUnlockConnection){}`，只沿 `pUnlockConnection` 一条链走。代价是它只能发现**当前已登记的那条等待链**构成的环；若某条边还没建立（应用还没登记），那个环此刻还不存在。
- **回调按函数指针分组合并（源码事实）**：`addToBlockedList` 按 `xUnlockNotify` 分组插入，`sqlite3ConnectionUnlocked` 只把**同一个回调函数**的上下文打包成一个数组。代价是注册了**不同**回调函数的等待方无法共享一次唤醒，批量唤醒只在「回调相同」这一组内生效。
- **参数数组用栈上 16 槽 + 2 倍扩容（源码事实）**：`void *aStatic[16]` 是固定容量，动态扩容走 `sqlite3Malloc(nArg*sizeof(void *)*2)`。代价是等待方超过 16 个时必然走堆分配，而堆分配失败会触发**降级为多次小批次回调**——源码注释明确说这是「在这种情况下能做的最好处理」，并指出代价是「应用会收到两到多次较小批次的回调，降低了应用对多个连接排优先级的能力」。
- **登记只保存「一个」等待目标（源码事实）**：`db->pUnlockConnection` 是单个指针，`sqlite3_unlock_notify` 每次登记直接覆盖旧值。代价是同一连接无法同时等待多个阻塞方。
- **写锁申请被任意一个读锁持有者挡下（源码事实）**：`src/btree.c` 的 `sqlite3BtreeLockTable` 遍历 `pBt->pLock` 链表时取**第一个**满足条件的其它连接作为阻塞方。代价是被唤醒后目标锁**未必已可用**，应用仍需重试并处理再次失败。
- **`BTS_PENDING` 让「申请写锁」升级为「拒绝新读事务」（源码事实）**：`src/btree.c` 在写锁请求失败时置 `pBt->btsFlags |= BTS_PENDING`，`sqlite3BtreeBeginTrans` 见到该标志位就让新事务失败。代价是读事务在有写者排队期间会被系统性拒绝，属于用可用性换避免写者饥饿。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/notify.c` | `sqlite3_unlock_notify` | 登记/取消解锁回调；含走环式死锁检测；无阻塞方时立即同步回调 |
| `src/notify.c` | `sqlite3ConnectionBlocked` | 记录「谁挡住了本连接」并挂上全局阻塞链表 |
| `src/notify.c` | `sqlite3ConnectionUnlocked` | 事务终结时遍历链表，按回调分组打包上下文并派发回调 |
| `src/notify.c` | `sqlite3ConnectionClosed` | 关闭连接时先解锁通知，再把自身从链表摘除并校验不变式 |
| `src/notify.c` | `sqlite3BlockedList` | 全局阻塞链表头，按 `xUnlockNotify` 分组排列 |
| `src/notify.c` | `addToBlockedList` | 按 `xUnlockNotify` 分组把连接插入链表 |
| `src/notify.c` | `removeFromBlockedList` | 把连接从阻塞链表摘除；不在链上则为空操作 |
| `src/notify.c` | `enterMutex` / `leaveMutex` | 获取/释放 `SQLITE_MUTEX_STATIC_MAIN` 互斥锁，并触发链表不变式校验 |
| `src/notify.c` | `checkListProperties` | 仅在 `NDEBUG` 未定义时存在的链表不变式断言 |
| `src/sqliteInt.h` | `sqlite3.pBlockingConnection` | 记录导致 `SQLITE_LOCKED` 的阻塞连接 |
| `src/sqliteInt.h` | `sqlite3.pUnlockConnection` | 记录本连接正在等待其解锁的连接 |
| `src/sqliteInt.h` | `sqlite3.pUnlockArg` | 传给 `xUnlockNotify` 的应用上下文指针 |
| `src/sqliteInt.h` | `sqlite3.xUnlockNotify` | 应用登记的解锁回调函数指针 |
| `src/sqliteInt.h` | `sqlite3.pNextBlocked` | 阻塞链表中的下一个节点指针 |
| `src/sqliteInt.h` | `sqlite3ConnectionBlocked` / `sqlite3ConnectionUnlocked` / `sqlite3ConnectionClosed` | 三个内部函数的声明；未开启编译开关时为空宏 |
| `src/btree.c` | `sqlite3BtreeLockTable` | 共享缓存取表锁失败时调用 `sqlite3ConnectionBlocked` 并返回 `SQLITE_LOCKED_SHAREDCACHE` |
| `src/btree.c` | `sqlite3BtreeBeginTrans` | 开启事务遇已有写事务或 `BTS_PENDING` 时调用 `sqlite3ConnectionBlocked` |
| `src/vdbeaux.c` | `sqlite3VdbeHalt` 路径的自动提交分支 | `db->autoCommit` 为真时调用 `sqlite3ConnectionUnlocked(db)` 触发回调派发 |
| `src/main.c` | `sqlite3LeaveMutexAndCloseZombie` | 关闭连接时调用 `sqlite3ConnectionClosed(db)` |
| `src/btreeInt.h` | `BTS_EXCLUSIVE` / `BTS_PENDING` | 共享缓存 Btree 的独占锁标志与等待读锁清除标志 |
| `src/sqlite.h.in` | `sqlite3_unlock_notify` | 公开 API 声明与完整语义文档（回调触发时机、不可重入、死锁检测、DROP TABLE 例外） |
| `src/sqlite.h.in` | `SQLITE_LOCKED` / `SQLITE_LOCKED_SHAREDCACHE` | 锁错误码及其共享缓存扩展码，用于区分有无阻塞连接 |
| `src/sqlite.h.in` | `SQLITE_MUTEX_STATIC_MAIN` | 保护阻塞链表与五个连接字段的全局静态互斥锁编号 |
| `src/util.c` | `sqlite3ErrorWithMsg` | 设置 `db->errCode` 并格式化 "database is deadlocked" 错误消息 |

## 与主核 / AI 的关系

**架构推论：** 主核若也要提供「等待锁」能力而非直接返回错误，需要同样回答三个问题：阻塞关系由谁记录（引擎主动记还是应用登记时记）、回调在什么锁状态下被调用、等待目标能否多个。SQLite 的答案是「引擎记录阻塞关系 + 应用决定是否等待 + 回调在全局锁下同步调用 + 等待目标唯一」，这套答案的前提是**共享缓存的表级锁粒度**；主核若是页级或更粗的锁粒度，「等待目标唯一」这一约束会更容易撞上，因为一个阻塞方往往同时持有多把锁。

对本项目而言，本页的可复用点是**三个具体设计**：`sqlite3_unlock_notify` 里「无阻塞方则立即同步回调」消除了登记与触发的竞态；`sqlite3ConnectionUnlocked` 里「按回调函数分组打包上下文」让应用能整体排优先级；以及扩容失败时「降级为多次小批次回调而非丢失通知」的处理——源码注释明确指出「返回错误码是不够的」，因为事务照样会结束、回调照样发不出去。主核若有类似的通知队列，这三条都值得对照。

对 AI 协作而言，本页提供的是**读函数体得到的断言**，评审时应回具体分支核对而非凭印象推断：例如「回调在持有 `SQLITE_MUTEX_STATIC_MAIN` 时被调用」这一点，结论来自 `sqlite3ConnectionUnlocked` 中 `enterMutex()` 在前、回调派发在中、`leaveMutex()` 在后的实际顺序；「栈上 16 槽」来自 `void *aStatic[16]` 的字面量，而非任何配置项。

## 参考

- [SQLite 官方文档：Unlock Notification API](https://sqlite.org/c3ref/unlock_notify.html) —— `sqlite3_unlock_notify()` 的接口定义、回调触发时机、回调不可重入约束、死锁检测语义与 DROP TABLE/INDEX 例外。
- [SQLite 官方文档：Using the sqlite3_unlock_notify() API](https://sqlite.org/unlock_notify.html) —— 基于 pthread 条件变量的完整用法示例，含「回调可能先于等待发生」的竞态处理与写者饥饿说明。
- [SQLite 官方文档：SQLite Shared-Cache Mode](https://sqlite.org/sharedcache.html) —— 共享缓存的表级锁模型与 `SQLITE_LOCKED` 的产生条件，是本机制的前置背景。
- [SQLite 官方源码：`src/notify.c`](https://github.com/sqlite/sqlite/blob/master/src/notify.c) —— 支撑本页关于分组链表、立即回调、死锁走环检测、16 槽参数数组与扩容失败降级路径的全部描述。
- [SQLite 官方源码：`src/sqliteInt.h`](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h) —— 支撑五个受 `SQLITE_MUTEX_STATIC_MAIN` 保护的连接字段与三个内部函数的空宏降级。
- [SQLite 官方源码：`src/btree.c`](https://github.com/sqlite/sqlite/blob/master/src/btree.c) —— 支撑 `sqlite3ConnectionBlocked` 的三处真实调用点与 `BTS_PENDING` 的设置条件。
