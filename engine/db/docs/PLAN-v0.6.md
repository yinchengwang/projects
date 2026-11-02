# v0.6 执行方案与计划 · WAL 与崩溃恢复

> 之前它是个"带索引的文件格式"；这一版之后才叫数据库。
> 验收标准：**提交后崩溃，数据必须在；未提交的，必须不在。**

---

## 一、方案

### 1.1 范围

| 模块 | 文件 | 职责 |
|---|---|---|
| 日志 | `wal/wal.{h,c}` | 记录格式、追加、fsync、checkpoint |
| 恢复 | `wal/recovery.{h,c}` | 分析 / 重放 / 回滚 |
| 页 | `storage/page.c` | 新增 `page_restore`（undo 用）、page_lsn 读写 |
| 堆 | `storage/heap.c` | WAL 钩子传出 LSN，改页后写 page_lsn |
| 接入 | `db.c` | 开日志、跑恢复、每条语句一个事务 |

### 1.2 六个决定

#### A · 行级 undo-redo，不做整页镜像

日志记 `(page_id, slot, before, after)` 的行镜像，不是 4KB 页镜像。

**理由**：整页镜像每改一页写 4KB，插 3000 行就是 12MB 日志。行级只要几十字节。

**代价**：重放要"按 LSN 顺序、从一致状态开始"，靠 page_lsn 保证幂等（见下）。

#### B · LSN 用 u32，存在页头

页头 `H_LSN` 是 u32（4 字节）。所以全系统的 LSN 也用 u32 —— 40 亿条记录，够学。

**不这么做的话**：LSN 用 u64，页头放不下，得扩页头或只存低 32 位（更糟）。

#### C · 幂等靠 page_lsn

重放每条记录前先看：`if (page.page_lsn >= record.lsn) skip`。

**为什么正确**：页被刷盘时 page_lsn 一定等于最后应用到它的记录 LSN。所以 `page_lsn >= lsn` 意味着这条记录已经生效过。从 checkpoint 开始按 LSN 升序重放，页状态单调前进。

#### D · undo 需要 `page_restore`

`page_insert` 只会追加到末尾，没法把一行写回**原来的 slot**。而 undo 删除时必须在原 slot 恢复（否则索引里的 RID 就失效了）。

新增 `page_restore(pg, slot, rec, len)`：slot 存在则覆盖，slot == slot_count 则追加。

**倒序 undo 保证空间够**：后插入的行先被撤销，腾出的空间正好给前面的恢复用。

#### E · 每语句一个事务，提交时 fsync

```
BEGIN → 若干 INSERT/DELETE/UPDATE 记录 → COMMIT → fsync
```

**不做**：多语句事务（`BEGIN; ... COMMIT;` 语法）、group commit（多条事务攒一批 fsync）。

**理由**：先把"崩溃后数据对"做对，事务语法和攒批是独立特性，加进来会掩盖正确性问题。

#### F · checkpoint 只在关闭时做，不截断日志

干净关闭时写一条 CHECKPOINT 记录（含 redo_start_lsn），然后 `pager_close` 刷所有脏页。

恢复时从**日志开头**扫（不截断），靠 page_lsn 快速跳过已生效的页。

**代价**：日志无限增长，恢复时间随历史线性增长。截断是纯优化，v0.7 再做。

### 1.3 日志记录格式

```
偏移  字段          说明
0     lsn     u32   本条记录的 LSN，单调递增
4     prev_lsn u32  同一事务的上一条（undo 链）
8     xid     u32   事务号
12    type    u8    BEGIN/COMMIT/ABORT/INSERT/DELETE/UPDATE/CHECKPOINT
13    pad     u8[3]
16    page_id u32
20    slot    u16
22    blen    u16   before 镜像长度（0 = 无）
24    alen    u16   after 镜像长度（0 = 无）
26    pad     u16
28    payload       before[blen] + after[alen]
      crc32   u32   覆盖 header + payload
```

### 1.4 恢复三阶段

```
阶段 1 · 分析：从头扫日志，收集"哪些 xid 提交了"
阶段 2 · 重放：按 LSN 升序，对每条 INSERT/DELETE/UPDATE
                load page → if page_lsn >= lsn: skip → else 应用 after
阶段 3 · 回滚：对未提交事务的记录，按 LSN 降序应用 before
                （INSERT 的 before 为空 → 删除该 slot）
```

**重放必须包含未提交事务**（ARIES 的核心洞察）：先全部重放到崩溃瞬间的状态，再统一回滚。只重放已提交的会让页状态不一致。

### 1.5 验收

| 项 | 标准 | 状态 |
|---|---|---|
| 正常路径 | 写→关闭→重开，数据在（不依赖恢复）| ✅ |
| 提交后崩溃 | `db_abandon` 不刷页 → 重开后数据必须在 | ✅ |
| 未提交不生效 | 崩溃时未 COMMIT 的事务，重开后看不到 | ✅ |
| 重复重放 | 连续恢复两次，结果一致（幂等）| ✅ |
| 日志损坏 | 末尾半条记录 → 截断到最后一条完整记录，不崩 | ✅ |
| 随机崩溃 | 循环 20 次"写入 N 行→崩溃→重开校验"，每次行数都对 | ✅ |
| WAL 规则 | 页刷盘前，对应日志已落盘（钩子强制）| ✅ |

## 二、执行计划

| 步 | 任务 | 状态 |
|---|---|---|
| 1 | `page_restore` + `page_restore_at` + page_lsn 读写 | ✅ |
| 2 | `wal.h/c`（含 `wal_reset` / `wal_max_xid` / `wal_flushed_lsn`）| ✅ |
| 3 | `recovery.h/c`（重放用日志里的 slot，不用重算）| ✅ |
| 4 | heap 钩子改造 + catalog meta 页写日志 + db.c 接入 | ✅ |
| 5 | `test/unit/test_wal.c`（10 项）+ `test/integration/test_crash.c` | ✅ |
| 6 | `pager` 的 WAL 规则钩子（页刷盘前先落日志）| ✅ |

## 三、执行记录

### 3.1 最终状态

| 指标 | v0.5 | v0.6 |
|---|---|---|
| 源码行数 | 6655 | **7971** |
| 测试行数 | 2928 | **3616** |
| 测试文件 | 10 | **13** |
| 验收项 | 78 | **98** |

13 个测试文件全部通过。

### 3.2 三个真正的 bug（都是"静默出错"，最值得记）

#### Bug 1 · bootstrap 的 xid=0 记录被当成未提交事务回滚

**现象**：`db_open` 报 `E_CORRUPT`，数据库完全打不开。

**根因**：catalog 建表时写的 WAL 记录 xid=0（还没进入用户事务），
分析阶段找不到对应的 `WAL_COMMIT`，就把它们全当未提交事务回滚掉。
meta 页被回滚成初始值 → magic 变成 0 → `catalog_open` 判定"不是我们的文件"。

**修法**：`xid_committed()` 里把 xid==0 特判为已提交（系统事务）。

**验证**：临时撤销这一行重跑，`test_crash` 立刻 12 项失败 —— 测试有效。

#### Bug 2 · 重放时重算 slot，撤销时用日志里的 slot

**现象**：数据"莫名少一条"，且极难复现。

**根因**：`redo_one` 里用 `page_insert` 自己分配 slot。
如果页状态和预期有偏差，算出来的 slot 和日志里记的不一致，
到了 undo 阶段按日志里的 slot 撤销，就删错了行。

**修法**：新增 `page_insert_at(pg, slot, rec, len)`，
重放时用日志里记的 slot 原地填回。

#### Bug 3 · 事务号跨会话重置

**现象**：未提交的数据留在了库里（隐蔽得多）。

**根因**：每次 `db_open` 都从 xid=1 开始。上次会话做到 xid=5 崩了，
这次会话又从 xid=1 开始 —— 恢复阶段判断"哪些事务提交了"时，
上次那个未提交的 xid=1 会被这次新的 xid=1 提交记录"连累"，判定成已提交。

**修法**：`wal_max_xid()` 从日志里读出出现过的最大 xid，
`db_open` 时用它作为起点。

### 3.3 两个设计层面的决定

#### A · WAL 规则做成"钩子"而不是"断言"

`pager.c` 里原来有个 TODO：页刷盘前要断言日志已落盘。

第一版想做成"检查 + 报错"，后来改成**钩子负责把日志刷下去**：

```c
pager_set_wal_flush(pg, wal_flush_before_page, db);
```

为什么这样更好：钩子返回错误 = 刷盘失败 = 数据库不可用（硬失败）。
而钩子自己去 fsync = 只是慢一点，数据照样安全（软降级）。

而且这个钩子在正常路径下**永远不触发**：db 层每条语句结束都 fsync 日志，
脏页要到更晚（缓冲池逐出或 db_close）才被刷盘，那时日志早就在盘上了。

#### B · xid=0 是"系统事务"

与其在恢复阶段猜"哪些记录是 bootstrap 的"，不如在协议层面说清楚：
**xid=0 专门表示系统事务，永远视为已提交**。

这是从"隐含约定"升级到"显式协议"。同理，`db_open` 检测到
"数据文件是空的但日志里有内容"时，直接 `wal_reset` 清空日志 ——
陈旧日志是孤儿，重放它只会把新库搞脏。

### 3.4 顺手补上的三个安全网

| 位置 | 防的是什么 |
|---|---|
| `db_open` 检测陈旧 WAL → `wal_reset` | 只删 `.db` 留 `.db-wal`，把上一个库的记录重放进新库 |
| `db_open` 用 `wal_max_xid` 作为 xid 起点 | 事务号跨会话重置，未提交数据被误判为已提交 |
| `page_restore_at` 校验 slot 偏移 | 页被 compact 后记录挪位，回写会写坏别的 slot |

### 3.5 还没做的（v0.7 起）

| 项 | 说明 |
|---|---|
| B+tree 不写日志 | 现在靠恢复后 `exec_rebuild_indexes` 重建。数据量大时太慢，v0.7 改成物理日志 |
| 日志不截断 | 恢复要从日志开头扫，靠 page_lsn 跳过。日志无限增长，v0.7 加 checkpoint + 截断 |
| group commit | 现在每条语句一次 fsync。攒批能大幅提升吞吐，v0.7 做 |
| 多语句事务 | 现在 `BEGIN; ... COMMIT;` 语法没有，每条语句一个事务 |
| 页撕裂 | 假设页写入是原子的。真实场景要加 full-page-write 或双写缓冲 |

### 3.6 下一步（v0.7）

MVCC（RC / RR）+ vacuum。

- 引入 tuple header 的 xmin/xmax（现在的 20 B MVCC 头是预留位置）
- 读快照：按 xmin/xmax 判断可见性
- vacuum：回收对所有快照都不可见的版本
- 顺带把 B+tree 的物理日志做掉（有了 MVCC 后索引项也要带版本）

