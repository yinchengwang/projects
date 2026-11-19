# 主核欠账清偿 —— WAL 前向兼容 + FT 精度 + 测量口径 + ts 页生命周期（工作流 W-D）

> 依据：`docs/superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md` §10（主核欠账台账与裁决）
> 落地日期：2026-11-19
> **执行顺序（唯一权威是设计 §9，本行只是它的摘要）**：
> `W-B P0 → W-A 全部 → W-C C1 → W-C C2~C5 → W-B P1/P2 → W-C C6/C7 → 【本计划 D-0 ~ D-7】 → 文档大改`
>
> ⚠️ **但 D-0 是例外，它是所有工作流的第 0 步** ——
> D-0 要提交的那 4 个文件（`db_ft.c` / `ft_index.c` / `ft_index.h` / `test_ft_reclaim_boot.c`）
> **现在就在工作区里**，不提交就会被 W-A/W-B/W-C 的任何一次 `git add` 卷进去（设计 §9 规则六）。

> **执行进度（2026-11-19）**
>
> | Task | 状态 | 提交 | 说明 |
> |---|---|---|---|
> | D-0 ft 开机页回收 | ✅ 完成 | `ace3a31d7` | 4 个文件单独一个提交；全量门禁 95/95；新集成测试 3 项验收通过 |
> | D-1 WAL 文件头 | ✅ 完成 | `5d1ae2d4a` + `ea26666d8` | 拆成两个提交：先做文件头，再把读写拆出 `wal_hdr.{h,c}`（原实现把 `wal.c` 推到 889 行，破了 800 行硬规则） |
> | D-2 FT 精确页数上界 | ✅ 完成 | `c2def3080` | **做法改了**：不用 `FPH_USED` 累加，改成逐词模拟写入器分页（见 Task D-2 的执行修正） |
> | D-3 测量口径 | ✅ 完成 | `07ad8872` | 拆成两个提交（与 D-1 拆分同批推送）；C7 变三个子判据、新增 `ts_bench_io` |
> | D-4 tombstone 撤销 | ✅ 完成 | `1168c4284` | **歧义不存在**（见 Task D-4 的确认记录）；
> | | | | 但确认过程撞出**运行时 ROLLBACK 不撤 FT** 的真缺陷，一并修了（D-4b） |
> | D-5 geo KNN 二次扫描 | ✅ 完成 | `d66de13ef` | 页读 **739 → 11**（省掉 98.5%）；
> | | | | 两个入口合成一份实现（一份剪枝、一份 tie-break） |
> | D-6.1 ts 时间窗真实路径 | ✅ 完成 | `d6fb60c2e` | SQL 查询分区级跳过；3 分区实测 6 页 → 4/0 页；保留 SPI 单谓词限制并写入验收 | 
> | D-6.2 ~ D-6.3 | ⬜ 未开始 | — | 旧裸写页回收 + 空闲栈 WAL/持久化 | 
> | D-7 | ⬜ 未开始 | — | group commit |
>
> 全量门禁当时的数字随之变成 **100 个测试 100 通过**（D-1 加 `test_wal_header`、D-2 加 `test_ft_merge_pages`、D-4 加 `test_ft_tomb_txn`、D-5 加 `test_geo_knn_points`、D-6.1 加 `test_ts_page_prune`；D-0 的 `test_ft_reclaim_boot` 在“95”那个数字里）。

---

## Goal

把设计 §10 裁决为「**做**」的 11 项主核欠账落地，让主核从「功能基本齐」变成「**行为可证**」：

1. **WAL 文件头 + 版本校验** —— 把「格式不兼容的 WAL 被静默解析」变成**显式报错**（D-1，最高优先）
2. **FT 合并页数上界改精确** —— 不再「宁多勿少」浪费页（D-2）
3. **测量口径有区分力** —— C7 不再是空判据；ts 有一套统一 IO benchmark（D-3）
4. **FT tombstone 撤销精确到事务** —— 同一 doc_id 被重复删除时不撤错（D-4）
5. **geo KNN 去掉第二遍全树扫描** —— 候选直接带坐标与距离（D-5）
6. **ts 页生命周期闭环** —— 旧页真回收 + 空闲栈落盘 + 时间窗接进查询路径（D-6）
7. **group commit** —— README 已经宣称但代码没有（D-7）

---

## Architecture

```
engine/db/
├── src/wal/
│   ├── wal.h            ← 加 WalFileHeader + WAL_FORMAT_VERSION
│   └── wal.c            ← wal_open 校验 magic/version/page_size/endian
├── src/modalities/ft/
│   ├── ft_merge.c       ← ft_merge_count_pages 改用真实 FPH_USED
│   ├── ft_segment.c     ← 暴露段内真实页使用量
│   └── ft_tomb.{c,h}    ← tombstone 记录带 xid，撤销按 (doc_id, xid) 匹配
├── src/modalities/geo/
│   ├── rtree_knn.{c,h}  ← KnHit 带坐标与距离
│   └── db_geo_query.c   ← 不再扫第二遍树
├── src/modalities/ts/
│   ├── ts_dir.{c,h}     ← 空闲页栈落盘
│   ├── ts_am.{c,h}      ← 时间窗接进扫描状态
│   └── ts_wal.c         ← （D-6 不改载荷粒度，见 §10.3）
├── src/wal/wal.c        ← group commit
└── bench/
    ├── ft_bench_a4.c    ← 帧池可配，C7 变有区分力
    └── ts_bench_io.c    ← 【新增】统一 IO 口径 benchmark
```

---

## Global Constraints

1. **中文注释、中文 commit message**；坑用 `M<层>-<序号>` 编号（本计划新增的坑从 M7-01 起）。
2. **每个 Task 独立成一个提交**，提交前跑全量门禁（`test/run.sh` 95 通过 / 0 失败）。
3. **绝不 `git add -A engine/`** —— 工作区可能有无关改动，一律**逐文件列路径**。
4. **不新建 `db_rel.c`、不新建 `doc_am.c`**（设计 §8 不做项）。
5. **不碰 `src/spi/`、`src/query/`、`src/modalities/relational/`** —— 那些是 W-C 的地盘，本计划与 W-C **零文件重叠**（设计 §10.4）。
6. **不碰 `db_internal.h` / `db.c` / `db_instance.c` / `main.c`** —— 组合根是 W-B/W-C 的地盘。
7. 冻结轨 `engine/src/db/` **不存在了**（W-A 已删）—— 本计划所有路径都相对 `engine/db/`。
8. **凡是要改磁盘格式的 Task（D-1 / D-4 / D-6），必须写「旧库怎么活」** ——
   要么提供迁移，要么明确「旧库不支持，报 `E_CORRUPT`」，**不能默默读错**。

---

## Review Focus

审这份计划的人请重点看：

- **D-1 的错误码选择** —— 不支持的版本该报 `E_CORRUPT` 还是新错误码？
- **D-4 的撤销语义** —— 同一 doc_id 被两个事务删除后，撤销其中一个，另一个还在不在？
- **D-6 的空闲栈落盘时机** —— 崩溃在「已改文件未落盘」之间会不会重复发同一页？
- **每个 Task 的「验收判据」是不是机械可判的** —— 不能是「效果更好」这种话。

---

## 文件结构

**新增**
- `engine/db/bench/ts_bench_io.c` —— 统一 IO 口径 benchmark（D-3）
- `engine/db/test/unit/test_wal_header.c` —— WAL 文件头单元测试（D-1）
- `engine/db/test/integration/test_ft_tomb_txn.c` —— tombstone 事务接线测试（D-4）
- `engine/db/src/db_ft_txn.c` —— 连接级 FT 事务接线（D-4b）
- `engine/db/test/unit/test_geo_knn_points.c` —— 带坐标/距离的 KNN 验收（D-5）
- `engine/db/test/integration/test_ts_freelist.c` —— ts 空闲栈落盘集成测试（D-6）

**修改**
- `engine/db/src/wal/wal.h` / `wal.c` —— 文件头 + group commit（D-1 / D-7）
- `engine/db/src/modalities/ft/ft_merge.c` / `ft_segment.c` / `ft_tomb.c` / `ft_tomb.h`（D-2 / D-4）
- `engine/db/src/modalities/geo/rtree_knn.c` / `rtree_knn.h` / `db_geo_query.c`（D-5）
- `engine/db/src/modalities/ts/ts_dir.c` / `ts_dir.h` / `ts_am.c` / `ts_am.h`（D-6）
- `engine/db/bench/ft_bench_a4.c`（D-3）
- `engine/db/README.md` —— group commit 的描述要与实现一致（D-7）

---

## Task D-0：提交 ft 开机页回收（**所有工作流的第 0 步**）

> **为什么它排第 0**：设计 §9 规则六要求「开工前先处理与本轮无关的脏工作区」，
> 而这 4 个文件**不是无关的** —— 它们是 §10 裁决为「做」的欠账 #1 的实现。
> 不先提交，W-A/W-B/W-C 的任何一次 `git add` 都会把它们卷进去。

**Files**
- 提交（不修改）：`engine/db/src/db_ft.c`、`engine/db/src/modalities/ft/ft_index.c`、
  `engine/db/src/modalities/ft/ft_index.h`、`engine/db/test/integration/test_ft_reclaim_boot.c`

**Interfaces**
- 新增（`ft_index.h`）：`FtPagePool *ft_index_seg_pool(FtIndex *ix);`
- 已有（`ft_segment.h`）：`u32 ft_segment_reclaimable_pages(Pager *, FtSegment *const *, u32, u32 *, u32);`
- 已有：`int ft_pool_add(FtPagePool *, u32);`

- [ ] **D-0.1 审查当前 diff（不是重写，是确认它真的对）**

  ```powershell
  cd C:\code\projects
  git diff engine/db/src/db_ft.c engine/db/src/modalities/ft/ft_index.c engine/db/src/modalities/ft/ft_index.h
  git status --porcelain engine/db/test/integration/test_ft_reclaim_boot.c
  ```

  ⚠️ **审查要盯的四点**（每一条都是「错了不报错」的那类错误）：

  | 审查点 | 为什么危险 | 怎么确认 |
  |---|---|---|
  | 回收在 `ft_index_recover()` **之后** | 顺序反了 ⇒ 把还在用的页判成垃圾 ⇒ 垃圾页发给新段 ⇒ 两份数据互相踩且不报错 | diff 里那段代码在 `dft_root_sync(inst);` 之后 |
  | 判据来自段层、装池在索引层 | 两层知识缝在一起 ⇒ 以后段布局一变就得改两层 | `ft_segment_reclaimable_pages` 找、`ft_index_seg_pool` + `ft_pool_add` 装 |
  | 回收上限 `npages / 2` | 上限算错 ⇒ 要么漏收（浪费空间），要么超收（发重复页） | 容量检查那段 |
  | 分配失败就放弃这轮 | 分配失败若导致开库失败 ⇒ 一个可选优化能拖垮开库 | `calloc` 返回 NULL 的分支 |

- [ ] **D-0.2 clean build（`build/engine/CMakeCache.txt` 是 2026-11-03 的陈旧缓存，必须先重建）**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean
  mingw32-make all 2>&1 | Select-String -Pattern 'error|warning'
  ```

  预期：**0 行**输出（既没有 error 也没有 warning）。

  ⚠️ `engine/db/` 用的是**自己的 Makefile**（不在 CMake 图里），
  不要去 `build/engine` 里 `cmake --build` —— 那是冻结轨/SDK 的构建。

- [ ] **D-0.3 跑全量门禁**

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：**95 通过 / 0 失败**。

  ⚠️ 若门禁红了：**先怀疑这次改动**，不要先怀疑门禁。
  回收逻辑的典型症状是「重开库后数据读不出来」——
  因为垃圾页被误判成可用页、发给了新段。

- [ ] **D-0.4 单独跑新增的集成测试**

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh 2>&1 | Select-String -Pattern 'ft_reclaim'
  ```

  预期：3 项验收全部通过（合并后池大小 = 6 / 重开后池大小 = 6 / 变异禁用回收后重开池大小 = 0）。

- [ ] **D-0.5 提交（一个独立提交）**

  ```powershell
  cd C:\code\projects
  git add engine/db/src/db_ft.c
  git add engine/db/src/modalities/ft/ft_index.c
  git add engine/db/src/modalities/ft/ft_index.h
  git add engine/db/test/integration/test_ft_reclaim_boot.c

  # 提交前确认暂存区里只有这 4 个文件
  git diff --cached --name-only
  ```

  预期输出**恰好 4 行**：
  ```
  engine/db/src/db_ft.c
  engine/db/src/modalities/ft/ft_index.c
  engine/db/src/modalities/ft/ft_index.h
  engine/db/test/integration/test_ft_reclaim_boot.c
  ```

  ```powershell
  git commit -m "feat(ft): 开机页回收 —— 合并后的老段页重开库后重新可用"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

## Task D-1：WAL 文件头与版本前向兼容（**优先级最高**）

> ⚠️ **（执行修正，2026-11-19）** 本 Task 落地时拆成了**两个提交**，原因见下：
>
> 1. `5d1ae2d4a` —— 文件头 + 五项校验（`magic` / `format_version` / `page_size` /
>    `endian_marker` / `hdr_size`）+ `test_wal_header.c` 7 项验收。
> 2. `ea26666d8` —— 把文件头三个函数拆出 `src/wal/wal_hdr.{h,c}`。
>
> **为什么必须拆**：第一版把三个函数直接塞进 `wal.c`，把它从 760 行推到 **889 行**，
> 破了 `README.md:158` 的硬规则 4（单文件不超过 800 行）。这是执行者自己造成的越界，
> 所以单独一个提交修它。拆完 `wal.c` 793 行 + `wal_hdr.c` 129 行。
>
> **为什么“打开时的三个分支”放进 `wal_hdr_load` 而不是留在 `wal_open`**：
> `0 字节`（全新库，写头）/ `1..31 字节`（半个头，报错）/ `>= 32 字节`（逐字段校验）
> 这三条最容易被人“顺手合并”，而合并掉的恰好是中间那条 —— 于是“半个头”会被
> 当成“全新库”补写，把一个截断的日志**洗成**一个干净的。

> ⚠️ **（执行修正，2026-11-19）第 2 处**：正文里“预期 95 通过”现在应是 **96 通过**
> （95 + `test_wal_header` 一个文件）。另外 `test_wal.c` 的 t5 手工改第 3 条记录的
> crc 时偏移写死从 0 开始，加头之后那个偏移改到了第 4 条 ⇒ 截断点后移 ⇒ `n` 从 2 变 3。
> 已改成 `WAL_FILE_HDR_SIZE + 原偏移`。**这是测试被正确地拦住了，不是测试坏了。**

> **为什么第一**：这是唯一一个「不做就可能**静默损坏数据**」的欠账。
> `wal.c:202 wal_open` 今天直接 `fopen(path, "r+b")` ——
> 一个格式不兼容的 WAL 被打开后，恢复逻辑会**按自己的格式去解析它**，
> 最坏情况不是报错，而是**恢复出错误的数据**。

**Files**
- 修改：`engine/db/src/wal/wal.h`、`engine/db/src/wal/wal.c`
- 新增：`engine/db/test/unit/test_wal_header.c`

**Interfaces**
- 新增（`wal.h`）：
  ```c
  #define WAL_MAGIC          0x57414C31u   /* "WAL1" */
  #define WAL_FORMAT_VERSION 1u
  #define WAL_FILE_HDR_SIZE  32u

  typedef struct {
      u32 magic;             /* WAL_MAGIC */
      u32 format_version;    /* WAL_FORMAT_VERSION */
      u32 page_size;         /* 写日志时的 PAGE_SIZE */
      u32 endian_marker;     /* 0x01020304 —— 字节序探针 */
      u32 hdr_size;          /* WAL_FILE_HDR_SIZE */
      u32 reserved[2];       /* 预留，必须为 0 */
  } WalFileHeader;
  ```
- 修改：`int wal_open(const char *path, Wal **out);`
  —— 语义扩展：**打开时校验文件头**，不兼容则返回错误。

- [ ] **D-1.1 先写测试（跑红）**

  在 `engine/db/test/unit/test_wal_header.c` 写 5 个用例：

  | 用例 | 构造 | 期望 |
  |---|---|---|
  | 合法新版本 | 写一个带正确头的 WAL | `wal_open` 成功 |
  | 旧版本 WAL | 头里 `format_version = 0` | **明确报错**（不是成功） |
  | 错误 magic | `magic = 0xDEADBEEF` | `E_CORRUPT` |
  | 截断 header | 文件只有 10 字节 | `E_CORRUPT` |
  | 未知 version | `format_version = 999` | `E_CORRUPT` |

  ⚠️ **"旧版本 WAL"这一条的期望值要先定清楚**（设计 §10.3 的遗留决策）：
  - 选项 A：旧版本 WAL **明确报 `E_CORRUPT`**（"这个文件是我不认识的格式"）；
  - 选项 B：旧版本 WAL **当作 v0 兼容读取**（但 v0 的格式定义必须同时冻结）。

  ⇒ **本计划取选项 A**，理由：v0 的记录格式没有版本标记，
  **无法证明**「这份 WAL 是 v0 而不是被改坏了的 v1」——
  选项 B 的「兼容」是假的兼容。
  ⇒ 这条决策写进代码注释，并在设计 §10.3 补一句说明。

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh 2>&1 | Select-String -Pattern 'wal_header'
  ```

  预期：**红**（`wal_open` 现在根本不校验）。

- [ ] **D-1.2 加文件头定义（`wal.h`）**

  把上面的 `WAL_MAGIC` / `WAL_FORMAT_VERSION` / `WAL_FILE_HDR_SIZE` / `WalFileHeader` 加到 `wal.h`，
  放在 `WAL_HDR`（L103）附近 —— 那个常量是**记录头**（28 字节），
  新加的是**文件头**（32 字节），两者不是一回事，命名上要能一眼分开。

  ⚠️ 文件头要**单独一套名字**（`WalFileHeader` vs 现有的 `WalRec`），
  否则半年后有人会把它们当成同一个东西。

- [ ] **D-1.3 `wal_open` 校验（`wal.c`）**

  在 `wal_open` 拿到 `FILE *f` 之后、开始用之前：

  ```c
  /* ---------- 文件头校验 ---------- */
  u64 fsize = <文件当前大小>;
  if (fsize == 0) {
      /* 空文件 = 全新日志 ⇒ 写一个文件头 */
      <写 WalFileHeader>;
  } else {
      if (fsize < WAL_FILE_HDR_SIZE) return E_CORRUPT;   /* 截断 */
      <读 32 字节>;
      if (hdr.magic != WAL_MAGIC)            return E_CORRUPT;
      if (hdr.format_version != WAL_FORMAT_VERSION) return E_CORRUPT;
      if (hdr.page_size != PAGE_SIZE)        return E_CORRUPT;
      if (hdr.endian_marker != 0x01020304u)  return E_CORRUPT;
      if (hdr.hdr_size != WAL_FILE_HDR_SIZE) return E_CORRUPT;
  }
  ```

  ⚠️ **三个必须注意的点**：
  1. **空文件与"文件存在但只有半个头"是两回事** —— 前者要写头，后者要报错。
     用 `fsize == 0` 判「全新」，用 `fsize < WAL_FILE_HDR_SIZE` 判「截断」，
     **两者不能合并**。
  2. **校验必须在 `w->scan_pos = 0` 之前、且要把文件位置移到头之后** ——
     否则后续 append 会从头写、覆盖掉文件头。
  3. `page_size` 与 `endian_marker` 也要校验：
     `page_size` 变了 ⇒ 载荷里的页内偏移全错；
     字节序反了 ⇒ 载荷全错。**两者都比版本号更容易造成静默损坏**。

- [ ] **D-1.4 确认所有读写路径都跳过了文件头**

  ⚠️ 这是本 Task **最容易漏**的地方：`wal_scan_begin` / `wal_scan_next` / `wal_reset` /
  `wal_truncate` 都直接操作文件偏移。文件头加在文件**最前面**，
  所以**每一个**从文件偏移读记录的路径都要从 `WAL_FILE_HDR_SIZE` 开始。

  ```powershell
  cd C:\code\projects\engine\db
  Select-String -Path src/wal/wal.c -Pattern 'scan_pos|seek|FSEEK|ftell|FTELL|read_at|wal_read_payload' |
      ForEach-Object { "L{0}: {1}" -f $_.LineNumber, $_.Line.Trim() }
  ```

  ⇒ 逐个核对：**每处偏移都要加上文件头长度**。
  ⇒ `wal_truncate`（截短文件）也要考虑：它保留「keep_lsn 之前的记录」，
  **文件头必须保留在截短结果的最前面** —— 截到 0 字节以下会毁掉头，
  下次开库就变成「截断 header」。

- [ ] **D-1.5 跑测试（转绿）+ 全量门禁**

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：95 + 5 通过 / 0 失败。

  ⚠️ **旧测试用的 WAL fixture 全部要更新** ——
  测试里手工构造 WAL 文件的地方现在必须先写文件头，
  否则会命中新的 `E_CORRUPT` 分支。
  **这不是「测试坏了」，是「测试现在被正确地拦住了」。**

- [ ] **D-1.6 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/src/wal/wal.h
  git add engine/db/src/wal/wal.c
  git add engine/db/test/unit/test_wal_header.c
  git diff --cached --name-only
  git commit -m "feat(wal): 加文件头与格式版本校验 —— 不兼容的 WAL 明确报错而不是静默解析"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

## Task D-2：FT 合并的精确页数上界

> ⚠️ **（执行修正，2026-11-19）本 Task 的做法整个换了 —— 原计划不可用**
>
> 原计划是「用源段倒排页的 `FPH_USED`（`ft_internal.h:185`，u16）累加出真实已用字节，
> 再按 `ceil(字节 ÷ 页容量)` 算上界」。落地前实测发现这个思路**两头都不成立**：
>
> | 方向 | 为什么会错 |
> |---|---|
> | 高估 | 源的字节 ≠ 输出的字节。合并时同一个词在几个段里的 posting 会**归并成一条 run**，
> | | 而每个源段各自带一个 12 字节 run 头 —— 累加源段 `FPH_USED` 会把本该省掉的 run 头也算进去 |
> | 低估 | 「总字节 ÷ 页容量取整」本身就会低估：posting run 不能跨页任意拼接，
> | | 页尾不足「12 字节 run 头 + 12 字节 posting」时那一页就用不下去了。
> | | 源码里那个 `容量 - 35` 正是为这种最坏浪费留的余量 —— 它不是乱写的 |
>
> ⇒ 换成**第三种做法**：照写入器的真实分页规则**逐词模拟**。
> 这个做法既不需要读源段页（也就绕开了原计划 D-2.2 里“统计读不能污染逻辑页读”那个坑），
> 又能得到**精确值**而不是“再猜一个余量”。

**Files**（实际改动的）
- 修改：`engine/db/src/modalities/ft/ft_merge.c` —— `ft_merge_count_pages` 改用模拟器
- 修改：`engine/db/src/modalities/ft/ft_writer.h` —— 新增 `FtPageSim` 与三个函数声明
- 新增：`engine/db/src/modalities/ft/ft_pagesim.c` —— 模拟器实现（95 行，纯算术、不碰 pager）
- 新增：`engine/db/test/unit/test_ft_merge_pages.c` —— 5 项边界测试
- ~~`ft_segment.c` / `ft_segment.h`~~ —— **不再需要**（不读源段页了）

**Interfaces**（实际新增的，不是原计划的 `ft_segment_used_bytes`）
- `ft_pagesim_reset(FtPageSim *s)` —— 清零（空输入 ⇒ 0 页，不多占一页）
- `ft_pagesim_add_postings(FtPageSim *s, u32 n)` —— 照着 `ft_post_writer_put` 的规则累加
- `ft_pagesim_add_dict_entry(FtPageSim *s, u32 term_len)` —— 照着 `ft_dict_writer_put` 的规则累加

- [x] **D-2.1 先量出今天的浪费有多大（基线）** —— 已做，数字记在提交 `c2def3080` 里

  临时探针（已在提交前回滚，工作区不留痕）跑 A4（10 万文档 / 批 1000 / 92 次合并）：

  | 口径 | 旧上界累计 | 实际累计 | 多要 | 每轮平均 |
  |---|---|---|---|---|
  | 倒排页 | 18895 | 18776 | **+119** | +1.3 页 |
  | 字典页 | 2926 | 2850 | **+76** | +0.8 页 |

  单轮更直观（第 92 次合并）：倒排上界 521 / 实际 518，字典上界 32 / 实际 31。

  ⚠️ 多要页的后果不是“浪费点内存”那么轻：**字典区必须连号**，上界决定要往空闲页池里
  借多长的一段。多要 1 页就可能让池里最长那段不够长 ⇒ 退回“文件尾预留” ⇒ 文件每轮都在长。

- [x] **D-2.2 暴露段内真实页使用量** —— ❌ **取消**（见上面的执行修正，思路不成立）

- [x] **D-2.3 改用逐词模拟算上界** —— 已做

  `ft_merge_count_pages` 里两个 `FtPageSim`（倒排 / 字典）各累加一遍，
  其余一行不动：这一趟仍然只读源段**字典页**（不读 posting），df 仍是**归并前**的
  （还没扣 tombstone），所以算出来的仍然是**上界**（可能偏大，不会偏小）✓ 安全。

  ⚠️ 模拟规则**必须与 `ft_writer.c` 的两处 put 逐字对应** —— 改了一边不改另一边，
  算出来的上界就会与实际不符（小于实际 ⇒ 合并时 `E_NOSPACE`）。
  这条一致性由 D-2.4 的边界测试盯着。

  事后检查原样保留：`ft_merge.c:499` 的 `if (ft_post_writer_count(pw) > post_pages)` ——
  它现在是「模拟器算错了就立刻报错」的守卫，而不是“事后补救”。

- [x] **D-2.4 加边界测试** —— 已做，`test/unit/test_ft_merge_pages.c`（5 项，纯算术、不开库）

  | 用例 | 断言（机械可判） |
  |---|---|
  | 空段 | `pages == 0`（不是“0 或 1” —— 空段真的不占页） |
  | 满页 | 倒排 `POST_PER_PG` 条 = 1 页；各多 1 条就变 2 页，且新页 `used == 24` |
  | 单条大词 | df = `POST_PER_PG * 3 + 7` ⇒ `pages == 4`（精确值，不是“≥”） |
  | 跨页 posting | 同词续接的第 2 页 `used == 12 + 12`（**续接 run 头照样占 12 字节**） |
  | 单调性 | df 递增时 `pages` 不减 |

  ⚠️ **变异验证抓出一个测试漏洞**：把 `ft_pagesim_add_postings` 里换页后的 `continue`
  改成 `break`（=「换页之后不再继续写」，一个很容易犯的错），第一版测试**仍然 PASS**
  —— 因为只断言了 `pages == 2`，而那个错法的页数也是 2。补上 `used == 24` 之后才真正抓住。
  ⇒ **这条经验写进了测试文件里**：`pages` 对不等于内容对。

- [x] **D-2.5 跑 FT 全量测试 + 全量门禁，提交** —— 已做

  实测：`test/run.sh` **97 个测试 97 通过**；`tools/check_deps.sh` ✓ 依赖方向正确；
  clean build 零诊断；无文件超 800 行。

  新上界 vs 实际（同规模）：倒排 **18776 = 18776**、字典 **2850 = 2850**，**差 0 页**。

---
## Task D-3：测量口径有区分力（C7 不再空判据 + ts 统一 IO benchmark）

> 这两项都**只改 `bench/`**，不碰内核行为，是整个计划里风险最低的。

**Files**
- 修改：`engine/db/bench/ft_bench_a4.c`
- 新增：`engine/db/bench/ts_bench_io.c`

**Interfaces**
- 新增环境变量：`FT_BENCH_FRAMES` —— 帧池帧数（默认 4096，与今天的 `A4_BP_FRAMES` 一致）

- [ ] **D-3.1 帧池可配（`ft_bench_a4.c`）**

  今天 `ft_bench_a4.c:74` 是 `#define A4_BP_FRAMES 4096u`，
  `:182` 是 `pager_open(DB_PATH_A4, A4_BP_FRAMES, &pg)`。
  ⇒ 改成读环境变量 `FT_BENCH_FRAMES`，读不到就用 4096。

  ```c
  /* 帧池大小可配：默认 4096 帧。
   * 想让「物理读盘」这类判据有区分力，必须把帧池调到装不下数据页 ——
   * 见 PLAN-v1.3.md「仍然欠着的」第 5 条。 */
  static u32 a4_bp_frames(void)
  {
      const char *s = getenv("FT_BENCH_FRAMES");
      if (s == NULL || *s == '\0')
          return 4096u;
      long v = strtol(s, NULL, 10);
      return (v > 0) ? (u32)v : 4096u;
  }
  ```

- [ ] **D-3.2 让 C7 有区分力**

  C7 今天的分支（`ft_bench_a4.c:752`）是
  `if (pg_unique_total > 0) { ... } else { printf("[SKIP] ..."); }`。
  判据「物理读盘 ≤ 唯一页数」在「唯一页数 = 0」时是 `0 <= 0` —— **空判据**。

  ⇒ 跑法改两处：
  1. `bench/ft_bench_run.sh` 增加一个「小帧池」档位；
  2. bench 的文档字符串里写明：
     「要 C7 有意义，必须 `FT_BENCH_FRAMES < 数据页数`；
     10 万文档约 3335 页 ⇒ 帧池取 512 足够」。

  ```powershell
  cd C:\code\projects\engine\db
  $env:FT_BENCH_FRAMES = "512"
  & "C:\Program Files\Git\usr\bin\sh.exe" bench/ft_bench_run.sh a4 100000 2>&1 |
      Select-String -Pattern 'C7'
  ```

  ⇒ 期望：**不再出现 `[SKIP]`**，而是 `[PASS]` 或 `[FAIL]` 且**带真实数值**。

  ⚠️ **判据要重新表述**。今天是「物理读盘次数 ≤ 被读过的唯一页数」。
  小帧池下这个判据**可能真的 FAIL** —— 因为同一页被逐出后又读回是**正常的**
  （缓存不够时必然发生），不是 bug。
  ⇒ 所以要分两个判据：
  - **判据 C7-a**：物理读盘次数 ≤ 唯一页数 × **重读系数**（系数 > 1，允许缓存抖动）；
  - **判据 C7-b**：物理读盘次数的**增长**与「数据页数」成正比，而不是与「词数」成正比
    —— 这才是 C7 真正要证明的那句话（「真实 IO 是 O(页数)，不是 O(词数)」）。

  ⇒ **不要为了让 C7 变绿去改判据**。C7 的目的是抓「IO 复杂度错了」，
  如果小帧池下它红了，那说明 IO 复杂度确实有问题 —— 那就是要修的真问题。

- [ ] **D-3.3 新建 `bench/ts_bench_io.c`**

  现在的 `bench/ts_bench.c` 是**压缩比**基准（只测编码后的字节数），
  **一个 IO 指标都没有**（实测：`bench/` 22 个文件里只有 `ft_bench_a4/a5` 有 logical/physical）。

  ⇒ 新文件输出**统一口径**的 6 个指标：

  | 指标 | 来源 |
  |---|---|
  | rows | 实际读出的行数 |
  | partitions scanned | 分区目录里被裁剪后**实际访问**的分区数 |
  | logical reads | `pager` 的逻辑读计数（`pager_get` 次数） |
  | physical reads | `pager_physical_reads` |
  | bytes read | `pager` 的字节读计数 |
  | elapsed ms | 墙钟 |

  ⚠️ **两条路径都要跑**（这才是「A8 IO 可证」）：
  - **全表扫描**（无时间谓词）；
  - **分区裁剪**（有时间窗）。

  ⇒ 两行的 `partitions scanned` 与 `logical reads` 必须**显著不同** ——
  这是裁剪生效的证据。差异小于 20% ⇒ 裁剪没生效 ⇒ 判 FAIL。

  ⚠️ **帧池大小也要作为变量**：同一组数据用 512 / 4096 两档帧池各跑一遍，
  验证 `physical reads` 随帧池变小而上升、`logical reads` 不变。

- [ ] **D-3.4 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/bench/ft_bench_a4.c
  git add engine/db/bench/ft_bench_run.sh
  git add engine/db/bench/ts_bench_io.c
  git diff --cached --name-only
  git commit -m "bench(ft,ts): C7 判据不再空判据（帧池可配）+ ts 统一 IO 口径基准"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

## Task D-4：FT tombstone 撤销 —— **原计划的歧义不存在，但撞出另一个真缺陷**

> ⚠️ **（执行修正，2026-11-19）本 Task 的结论与原计划相反，且多修了一个原计划没有的东西**
>
> 原计划的判断是「同一 doc_id 被不同事务重复删除 ⇒ 恢复期按值撤销可能撤错事务」，
> 解法是把 tombstone 页条目从 `u32 doc_id` 改成 `(doc_id, xid)` 双字段。
>
> 按 D-4.1 的要求先做确认，实测结论是**这个歧义不可能发生**，
> 而确认过程里撞出一个原计划**只字未提**的真缺陷。

### D-4.1 的确认结果（结论决定 Task 规模，所以这一步不能跳）

三个探针（跑完即删，工作区不留痕）：

**① 重复删除写不写第二条 WAL 记录？—— 不写。**

`ft_index.c:479` 的 `if (ft_tomb_is(ix, doc_id)) return OK;` 在调
`ft_tomb_append` **之前**就返回了 ⇒ 内存检查先于写盘。实测：

```
删除前 WAL 记录数 = 37
第一次删除后 = 41（增量 4）
第二次删除后 = 41（增量 0）   ← 重复删除一条记录都不写
```

⇒ 同一页里**永远不会出现两条相同的 doc_id** ⇒ `ft_tomb_page_find` 按值查必然
唯一命中 ⇒ **原计划担心的「撤错事务」不可能发生**。
⇒ 原计划 D-4.2（页格式加 `xid`）**不需要做**；`ft_segment.{c,h}` 也不用动。

**② 事务内删除后 ROLLBACK，文档会不会恢复？—— 会话内不会，重开后才会。**

```
初始：检索 alpha 命中 1 篇
txn 内删除后：命中 0 篇
ROLLBACK 后（本会话）：命中 0 篇     ← 期望 1，实际 0
checkpoint 后（本会话）：命中 0 篇
关库重开：命中 1 篇                  ← 又对了
```

⇒ **真缺陷**：同一个会话里先查不到、关一下库又查得到。

### 根因

运行时 ROLLBACK（`db_stmt.c` 的 `txn_end_abort`）只动 txn 表（clog 标 ABORTED），
从来没接到 FT 上。FT 的两份状态都没人撤：内存集合 `ix->tomb`、tombstone 页里那一条。
崩溃恢复那一路是对的（WAL 钩子按 `REC_HOOK_UNDO` 调 `ft_tomb_page_remove`），
但运行时这一路**零调用方**。重开之所以对，正是因为走的是恢复那一路。

**Files**（实际改动的）
- 新增：`engine/db/src/db_ft_txn.c` —— 连接级事务接线（163 行）
- 新增：`engine/db/test/integration/test_ft_tomb_txn.c` —— 4 项验收（245 行）
- 修改：`engine/db/src/db_ft.c` —— `db_ft_delete` 退化成转发薄壳（796 → 773 行）
- 修改：`engine/db/src/db_stmt.c` —— `txn_end_commit` 清、`txn_end_abort` 撤
- 修改：`engine/db/src/db_instance.c` —— `db_close` / `db_abandon` 也清
- 修改：`engine/db/src/db_internal.h` —— `struct DB` 加 `ft_del` 字段 + 4 个声明
- 修改：`engine/db/src/modalities/ft/ft_index.{c,h}` —— 新增 `ft_index_undelete`
- 修改：`engine/db/src/modalities/ft/ft_tomb.c` + `ft_index_internal.h` —— 新增 `ft_tomb_remove_doc`
- ~~`ft_segment.c` / `ft_segment.h`~~ —— **不再需要**（不改页格式）

**Interfaces**（实际新增的，不是原计划的 `(doc_id, xid)` 双字段）
- `db_ft_txn_track(DB *, u32 doc_id)` —— 记「本事务删了这一篇」（内部判重）
- `db_ft_txn_rollback(DB *)` —— 回滚时逐个撤（页 + 内存集合）
- `db_ft_txn_clear(DB *)` —— 提交 / 关连接时清
- `ft_index_undelete(FtIndex *, u32 doc_id)` —— 撤销一次删除（合法空操作幂等）
- `ft_tomb_remove_doc(FtIndex *, u32 doc_id)` —— 页那一半（找页号 + 只撤那一条）

- [x] **D-4.1 先把歧义具体化** —— 已做，结论：**歧义不存在**（证据见上）

- [x] **D-4.2 若歧义存在：页格式加 xid** —— ❌ **取消**（前提不成立）

  ⚠️ 这一条的取消不是「先不做」，是「做了反而有害」：
  改页格式要写迁移或明确报 `E_CORRUPT`，代价真实，收益为零。
  前提被钉在 D-4.3 的第 1 项测试上 —— 哪天那个提前返回被挪走，测试立刻红。

- [x] **D-4b 补上「运行时 ROLLBACK 撤 FT」**（原计划没有这一项，新增）

  | 取舍 | 为什么 |
  |---|---|
  | 记在**连接**上，不记在实例上 | 实例是多连接共享的，记在那里会让 A 的回滚撤掉 B 的删除 —— 比原缺陷更严重的数据丢失 |
  | **先撤页，再撤内存集合** | 与 `ft_index_delete` 的「先落盘再进内存」相反，但同一个理由：页是权威存储。页撤成功才动内存集合，否则内存说「没删」、页里还留着 ⇒ 下次开库那篇又被删掉 |
  | **不写新的 WAL 记录** | 那条 `WAL_FT_TOMB_APPEND` 本来就在日志里，它所属的事务马上被打成 ABORTED，崩溃恢复按 clog 判定未提交 → 自动 undo。运行时先撤一遍只是「提前做同样的事」。代价是运行时撤销改了页但没有日志记录；崩溃在「已改页未落盘」之间时重开按 WAL 重放再 undo，结果仍正确 |
  | 记录失败不报错 | 报错会让调用方以为删除没生效，而实际页与日志都已经写了。只放弃回滚能力 |

- [x] **D-4.3 加崩溃恢复集成测试** —— 已做，`test_ft_tomb_txn.c`（4 项）

  | 场景 | 断言（机械可判） |
  |---|---|
  | 连删两次同一 doc_id | `ft_index_ntombstones(ix) == 1`（**钉住 D-4.1 的前提**） |
  | BEGIN → 删除 → ROLLBACK | 同一会话里 `hits == 1`（修前是 0，红） |
  | BEGIN → 删除 → COMMIT | 同一会话里 `hits == 0`（防「过度撤销」） |
  | 会话内 vs 重开后 | 两条路径给出同一答案（回滚=1、提交=0） |

  ⚠️ **变异验证**：把 `db_ft_txn_rollback` 调用换成 `int ft_rc = OK`
  （= 回滚不撤 FT），测试第 2、4 项立刻红；改回绿。
  这证明四条断言真的在测那件事，不是空转。

- [x] **D-4.4 提交** —— `1168c4284`（10 个文件，+612/−28）

  ⚠️ 踩了一个自己造的坑：`db_stmt.c` 在仓库里是 **CRLF**，我的脚本把它写成了
  裸 LF，diff 一度显示 **901 行变更**。转回 CRLF 之后真实 diff 是 **13 增 4 删**。
  教训：改文件前先看行尾，别整文件重写。

### 如实记账的已知限制（本 Task 明确不修）

**① 两个并发事务删同一篇文档**：A 未提交地删 5，B 删 5（此时 5 已在集合里 ⇒
B 是个空操作、不记录），B 提交，A 回滚 ⇒ 5 被 A 撤掉 ⇒ B 那次删除的意图没了。
这不是本次改动引入的：整个内核「删除是物理删除、不做写冲突检测」。
WAL 那一路的结论与它一致（A 的记录被 undo 撤掉，B 根本没有记录），所以两条路径
**不会互相矛盾** —— 但这确实不是「谁先谁赢」的语义。要真正的并发删除语义
需要行存 + 版本，另立一项。

**② 回滚撤不回已被合并物理丢弃的文档**：若删除之后发生过合并（合并把
tombstone 的文档物理丢弃），撤销只能撤「标记」，撤不回内容。合并走系统事务
（xid=0），与用户事务回滚不相干 ⇒ 这是长期语义，不是缺陷。

---
## Task D-5：geo KNN 去掉第二遍全树扫描

> 执行结果：**页读 739 → 11**（10 万点 / k=10），结果与改前逐位一致。
> 做法与计划基本一致，但有一处**比计划更彻底**：两个入口合成了一份实现。

### D-5.1 的基线（实测，探针跑完即删）

| 口径 | 逻辑页读 |
|---|---|
| `rtree_knn` 单独（k=10） | **11** |
| `db_geo_knn` 改前 | **739** |

⇒ 728 次全是那第二遍摊平。也就是说 KNN 本身很省，
真正贵的是调用方为了拿坐标把整棵树又扫了一遍。

### 根因比计划写的更冤

计划记的是「`rtree_knn` 只返回 row_id，不带距离（坑 M5-13）」。
实情是：`rtree_knn.c` 在两处叶子条目上**都已经算出了真实距离**
（`geo_dist2(qx, qy, m.minx, m.miny)`），**只是拿去排序之后丢掉了**；
坐标也已经在 `rtk_ent_get` 解出来（叶子 MBR 是退化矩形，minx/miny 就是那个点），
同样丢掉。

⇒ 所以补的不是「多读点什么」，而是「**别把手里已有的东西扔了**」。
这也解释了为什么这个优化的收益是 98.5% 而不是「省一点」：
丢掉的恰好是 O(树) 那一遍本来要重新算的东西。

**Files**（实际改动的）
- 修改：`engine/db/src/modalities/geo/rtree_knn.c` —— 合成一份 `rtk_knn_impl` + 两个薄入口
- 修改：`engine/db/src/modalities/geo/rtree_knn.h` —— 新增 `KnPoint` 与 `rtree_knn_points`
- 修改：`engine/db/src/db_geo_query.c` —— `db_geo_knn` 去掉 `db_geo_flatten`
- 新增：`engine/db/test/unit/test_geo_knn_points.c` —— 4 项验收（383 行）

**Interfaces**（与计划一致的）
- `KnPoint { u32 row_id; float dist; float x; float y; }`
  —— `dist` 是**平方距离**（`geo_dist2` 口径，与 `db_geo_hit.dist` / `geo_brute_knn` 一致）
- `rtree_knn_points(...)` —— 带坐标与距离的精确 KNN
- `rtree_knn(...)` **签名一动不动**（`test_geo_knn.c` 有十几处断言依赖它）

- [x] **D-5.1 先量出第二遍扫描的代价（基线）** —— 已做（数字见上表）

- [x] **D-5.2 `rtree_knn` 内部带上坐标与距离** —— 已做，**不增加任何一次页读**

  ⚠️ **比计划多做了一步，也更安全**：计划说「把 `KnHit` 带进结果集」，
  实际做法是把原 `rtree_knn` 的整个函数体（180 行）搬进 `static rtk_knn_impl`，
  让 `rtree_knn` 与 `rtree_knn_points` **都走它**。

  为什么必须合成一份：两套剪枝条件迟早会不一致，然后其中一套**悄悄漏点** ——
  这类错误不报错，只是答案少几个。`geo_brute_knn` 存在的全部理由就是这个。

- [x] **D-5.3 `db_geo_knn` 去掉 `db_geo_flatten`** —— 已做

  原来是：摊平全树拿坐标 + 死条目数 → 要 `k + dead_n + 16` 个候选 →
  滤死条目 → 补距离 → 再排。
  现在是：直接要 k 个 → 拿到现成的 `row_id/dist` → 再排一次（同一个比较器 `dg_hit_cmp`）。

  **顺带解决了「死条目占位」**：旧实现不知道谁是死的，所以要知道死条目数
  就得先摊平全树 —— 那正是第二遍扫描的**动机**。新实现在叶子条目上就过滤
  （`id & RT_ENT_DEAD`），于是 `k + dead_n + 16` 这个补丁连同 `db_geo_flatten`
  的调用一起消失了。

- [x] **D-5.4 验收** —— 已做，`test/unit/test_geo_knn_points.c`（4 项）

  | 判据 | 怎么测（机械可判） |
  |---|---|
  | 与旧实现一致 | 20 次查询（k=1/10/100）：新函数与旧 `rtree_knn` 的 row_id 序列**逐位相等** |
  | 距离 / 坐标正确 | 10 次 × k=50：与 `geo_brute_knn` 逐位相等（断言 `==`，不是「接近」） |
  | **死条目**（D-5.3 的核心） | 3 个点被标死：k=3 必须返回活的 3/4/5（没过滤会拿到 0/1/2）；k=100 只返回 17 条；max_out=5 精确报 `E_NOSPACE` |
  | 一页都不许多读 | 10 次查询 `r_new <= r_old` |

  「死条目」那一条是 D-5.3 的核心 —— 计划里专门警告过：「只在没有死条目的情况下
  对拍，证明不了任何东西」。

- [x] **D-5.5 提交** —— `d66de13ef`（4 个文件，+593/−114）

  实测（同一规模复测）：`db_geo_knn k=10` **11** 次页读（改前 739），
  省 728 次 = **98.5%**；查询结果与改前**逐位一致**（前 3 条 row_id/dist 相同）。
  `bench/geo_bench` 仍 PASS。

### 如实记账

`rtree_knn` 的旧签名**没有改**（虽然它现在只是薄壳）：`test_geo_knn.c` 有十几处
断言依赖它，改签名要动别人的测试；而且它确实还有用（不需要距离的调用方用它
少分配一点内存）。`rtree_knn.h` 里给它的注释补了一句「它丢掉自己算出来的距离与
坐标，需要距离就用 `rtree_knn_points`」—— 免得下一个人又踩 M5-13。

---
## Task D-6：ts 页生命周期闭环（旧页回收 + 空闲栈落盘 + 时间窗接线）

> 这三项是**同一个病根**的三条腿，所以合并成一个 Task。
> 光有 freelist 不持久化，重启后还是丢；光有时间窗没接线，谓词根本进不来。
>
> **执行顺序修正**：D-6.1 已先单独完成（见下）；D-6.2 / D-6.3 仍按原顺序推进。

**Files**
- 修改：`engine/db/src/modalities/ts/ts_dir.c`、`ts_dir.h`、`ts_am.c`、`ts_am.h`
- 新增：`engine/db/test/integration/test_ts_freelist.c`

**Interfaces**
- 修改（`ts_dir.h`）：空闲页栈持久化 —— 栈的元数据（头/尾/条目数）登记到
  `PT_TS_META` 页（那页已经记了格式版本、分区窗口秒数、目录链的头/尾、条目数）
- 修改（`ts_am.h`）：新增扫描状态上的时间范围字段

- [x] **D-6.1 时间窗接进真实查询路径** —— 已完成，提交 `d6fb60c2e`

  **先量到的真问题（探针跑完即删）**：3 个分区（每天 1 个）、每天 300 条。

  | SQL | 改前 | 改后 |
  |---|---:|---:|
  | 无 WHERE | 900 行 / 6 页 | 900 行 / 6 页 |
  | `time >= 第 2 天`（2 个分区） | 600 行 / 6 页 | 600 行 / **4 页** |
  | `time < 第 2 天`（1 个分区） | 300 行 / 6 页 | 300 行 / **4 页**（超集边界） |
  | 完全范围之外 | 0 行 / 6 页 | 0 行 / **0 页** |

  根因：原 `ts_am_mat_readings` 是 `ts_store_scan`（全表扫描）+ `ts_am_in_parts`（逐行过滤）；
  `ts_prune_scan` 只在 bench 里直调，真实 SQL 路径从未接入。

  **实际改法**：
  1. `ts_am_mat_readings` 改走 `ts_prune_scan`，命中分区才读页；
  2. 新增 `ts_store_pager()` 取得存储自己的 Pager；
  3. 新增 `ts_store_note_reads()` 回收裁剪路径页读，否则 `db_ts_page_reads` 看不见节省；
  4. 删除 `TsAmMat` 已不需要的 `pstart/pend/nparts/cap_parts`；
  5. 新增 `test/integration/test_ts_page_prune.c`，走真实 SQL 断言 6 项。

  **SPI 结构性限制（明确不越界）**：`PushFn` 一次只能接收一个 `Predicate`，而
  `time >= a AND time < b` 是两个谓词，执行器只下推其中一个，另一边由执行器完整求值。
  因此双边界只能保证行数精确、页读不高于全扫，不能假装保证只读一个分区；要彻底解决需改
  `src/spi/pushdown.h` 的谓词合取契约，但该目录是 W-C 地盘，本 Task 不碰。

  验证：clean build 零诊断；全量 **100 个测试 100 通过**；依赖方向正确；无文件超 800 行。

- [ ] **D-6.2 旧裸写页真回收**

  现状：`ts_seal.c:27-29` 明说「旧裸写页只是**标记不再使用**……
  本轮**不真回收**：本模态没有 freelist（坑 M6-42）」。

  ⇒ 要做：
  1. 明确页生命周期状态（活跃 / 已封窗待回收 / 已回收）；
  2. 回收旧时序页 → 交给全局空闲栈；
  3. **复用前清零**（否则新数据会看到旧页的残留字节）；
  4. 崩溃恢复中断时不重复发放。

  ⚠️ **「不重复发放」是这条最容易错的地方** ——
  页号一旦重复发给两个结构 ⇒ 两处数据互相踩 ⇒ 且不报错。
  ⇒ 空闲栈的「发放」必须是**先从栈里弹出、后使用**，
  且栈的持久化状态必须与实际分配状态**一致**（D-6.3 解决）。

- [ ] **D-6.3 全局空闲栈落盘**

  现状：`ts_ttl.h` 明确「全局空闲栈**只在内存里**，不落盘 ⇒ 关库即丢」。
  ⇒ 把空闲栈的元数据（头/尾/条目数 + 栈本身）持久化。

  ⚠️ **崩溃一致性是这个 Task 的核心难点**：
  - 「页已从栈里弹出但还没用上」崩溃 ⇒ 该页**丢失**（不是重复，是丢失）——
    可接受吗？⇒ **不可接受**：那就是一个永久的空洞。
  - 「页已用上但栈还没更新」崩溃 ⇒ 该页**重复发放** ⇒ **致命**。

  ⇒ 正确顺序：**先写日志（页从栈里弹出），再改页（真正用这个页）** ——
  这正是 WAL 的 write-ahead 规则（`geo_wal.c:7-15` 写得很清楚）。
  ⇒ 所以这一步**依赖 WAL 接线**，不能只改 `ts_dir.c`。

  ⚠️ **这是本 Task 最大的工作量**，不是「把栈写进 meta 页」那么简单。
  ⇒ **拆成两个提交**：D-6.3a 先做「不落盘但崩溃安全」（栈的分配走 WAL），
  D-6.3b 再做「落盘」。

- [ ] **D-6.4 集成测试**

  新建 `engine/db/test/integration/test_ts_freelist.c`：

  | 场景 | 断言 |
  |---|---|
  | 封窗后旧页进空闲栈 | 栈大小 = 预期页数 |
  | 关闭再打开 | 栈大小**不变**（落盘生效） |
  | 分配优先取空闲栈 | 新分配拿到的是栈里的页，不是文件尾 |
  | 崩溃在弹出与使用之间 | 重开后该页**不重复发放** |
  | 复用前清零 | 新页读出来全是 0 |

- [ ] **D-6.5 提交（拆成 3 个）**

  ```powershell
  cd C:\code\projects
  git add engine/db/src/modalities/ts/ts_am.c
  git add engine/db/src/modalities/ts/ts_am.h
  git diff --cached --name-only
  git commit -m "feat(ts): 时间窗口接进查询路径 —— 时间谓词真正参与分区裁剪"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

  ```powershell
  git add engine/db/src/modalities/ts/ts_dir.c
  git add engine/db/src/modalities/ts/ts_dir.h
  git add engine/db/test/integration/test_ts_freelist.c
  git diff --cached --name-only
  git commit -m "feat(ts): 旧时序页真回收 —— 页生命周期状态化 + 复用前清零"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

  ```powershell
  git add engine/db/src/modalities/ts/ts_dir.c
  git add engine/db/src/modalities/ts/ts_dir.h
  git diff --cached --name-only
  git commit -m "feat(ts): 全局空闲页栈落盘 —— 关库再开空间可复用"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

## Task D-7：group commit

> **现状**：`wal.c:478 wal_fsync` 是单次 `WAL_FSYNC`（`fsync` / `_commit`），
> **没有攒批**。而 `engine/db/README.md:51` 已经写着
> 「wal.c/h 日志记录、**group commit**」—— **文档宣称了，代码没有**。

**Files**
- 修改：`engine/db/src/wal/wal.c`、`engine/db/src/wal/wal.h`
- 修改：`engine/db/README.md`（确认描述与实现一致）

**Interfaces**
- 修改（`wal.h`）：新增 group commit 的状态与接口（见 D-7.2）

- [ ] **D-7.1 先量出今天的 fsync 次数（基线）**

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" bench/latch_bench_run.sh 2>&1 |
      Select-String -Pattern 'fsync|commit|tps'
  ```

  ⇒ 记下：N 个事务提交 ⇒ **N 次 fsync**（今天必然是 1:1）。

- [ ] **D-7.2 设计提交协议（先写下来，再写代码）**

  要定的四件事：

  | 问题 | 要定的内容 |
  |---|---|
  | **谁执行 fsync** | 第一个到达的提交者执行；其余线程等同一个 flush generation |
  | **flush 失败怎么通知等待者** | 等待者不能各自去 fsync（那还是 N 次）也不能被当成成功 |
  | **等待者怎么知道要等哪一代** | 用 `flushed_lsn` 水位：**等到 `flushed_lsn >= 我的 lsn`** 即为已落盘 |
  | **abort / close 时怎么处理等待者** | 唤醒全部等待者，让它们各自判定失败 |

  ⚠️ **关键设计点**：现有 `wal_fsync`（`wal.c:478-503`）已经把
  「域锁内 fflush」与「域锁外真正落盘」分开了 ——
  注释明说「两个线程的 fsync 可以**重叠** —— 磁盘队列能流水」。
  ⇒ **group commit 是在这个基础上加「等同一代」**，不是重写 fsync 路径。

  ⇒ 设计写成一段注释放进 `wal.h`，包含状态机：
  ```
  提交者 → 写 COMMIT 记录（拿 lsn）
        → 尝试成为「本代 flush 的发起者」
            是 → 执行 fsync → 推进 flushed_lsn → 唤醒等待者
            否 → 等 flushed_lsn >= 我的 lsn（或等失败信号）
  ```

- [ ] **D-7.3 实现**

  ⚠️ **不要引入新的锁**。现有 `wal_lock` / `wal_unlock` 是域锁，
  `flushed_lsn` 是 `_Atomic u32`。
  ⇒ 「等一代」用**条件变量**或**自旋 + 短暂 sleep**，
  但**等待期间不能持域锁**（否则一个线程的等待会堵住所有 appender）。

  ⚠️ **`flushed_lsn` 只能前进不能后退**（`wal.c:496-501` 已经用 CAS 保证了）——
  group commit 的等待者也依赖这个性质。

- [ ] **D-7.4 验收**

  | 判据 | 怎么测 |
  |---|---|
  | 单事务语义不变 | 单线程提交后立刻崩溃，恢复结果与今天一致 |
  | 多线程多个事务共享 fsync | 4 线程 × 100 事务 ⇒ **fsync 次数 << 400** |
  | 崩溃恢复一致 | 提交后 kill -9，恢复后事务要么全在要么全不在 |
  | **失败 fsync 不让事务假成功** | 让 fsync 失败（把 WAL 目录设成只读）⇒ 事务必须报错 |
  | benchmark 对比 | 与 D-7.1 的基线对比 fsync 次数 |

  ⚠️ **「失败 fsync 不让事务假成功」是最容易漏的一条** ——
  攒批之后，一个事务的成败可能取决于**另一个线程**的 fsync 结果。
  ⇒ 每个等待者必须知道**自己等的这一代**的成败，
  不能只看「flushed_lsn 推进了没有」。

- [ ] **D-7.5 同步 README**

  `engine/db/README.md:51` 的「group commit」描述在实现之后要与事实一致。
  ⇒ 若最终决定「不做真攒批、只做并发 fsync」，**就把 README 改成实话** ——
  这也是一个合法结论。

- [ ] **D-7.6 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/src/wal/wal.c
  git add engine/db/src/wal/wal.h
  git add engine/db/README.md
  git diff --cached --name-only
  git commit -m "feat(wal): group commit —— 多事务共享一次 fsync"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

## 不做项（本计划明确不做）

| 事项 | 原因 |
|---|---|
| R-tree E_NOSPACE 保守误报 | **已修**（`rtree_knn.c:262-280` 坑 M5-11，2026-11-04，commit `e48c4b1b8`） |
| reindex 后 row_id 复用 | `geo_am_reindex` 返回 `E_NOTSUP`，前提不成立；水位线已由 `db_geo.c:178 dg_fix_high_water` 修 |
| geo 开库扫描 O(npages) | 记为已知代价（`geo_state.h:39-46` 坑 M5-9）；解法依赖 W-C 的 `TableOpener`，**排进 W-C 的后续项**而不是 W-D |
| geo WAL 只 redo 不 undo | **待人工裁决**（设计 §10.3）—— agent 不得自行决定事务模型 |
| geo 无第二份行存 | 待人工裁决（与上一条同一批设计） |
| geo WAL 放大 4 倍 | 待人工裁决（与上一条同一批设计） |
| ts 整页镜像 WAL 放大 300 倍 | 待人工裁决（与 geo 三项同一批设计） |
| study-hub P2/P3/P4/P5 | 与本设计无关 |
| 22c-monitoring-deployment | 已作废 |
| gap-analysis-roadmap P0/P1/P2 | 已降级为历史参考 |
| Raft 迁移 | B2 决策「分布式：不做」 |

---

## Self-Review 记录

### Spec 覆盖

对照设计 §10 的裁决表：

| §10 裁决 | 本计划落点 | 状态 |
|---|---|---|
| 做 #1（ft 开机页回收） | D-0 | ✅ |
| 做 #2（精确段页上界） | D-2 | ✅ |
| 做 #3（C7 判据） | D-3.1 / D-3.2 | ✅ |
| 做 #4（WAL 前向兼容） | D-1 | ✅ |
| 做 #5（按值撤销歧义） | D-4 | ✅ |
| 做 #11（KNN 二次扫描） | D-5 | ✅ |
| 做 #13 #14 #15（ts 三项） | D-6 | ✅ |
| 做 #17（ts A8 benchmark） | D-3.3 | ✅ |
| 做 #18（group commit） | D-7 | ✅ |
| 已完 #10 | 不做项 | ✅ |
| 降级 #9 #12 | 不做项 | ✅ |
| 待裁决 #6 #7 #8 #16 | 不做项 | ✅ |

⇒ **§10 裁决为「做」的 11 项，全部有落点**；其余 7 项全部有明确的不做理由。

### 类型一致性

- `WalFileHeader` 用 `u32` × 8 = 32 字节（`wal.h` 的 `u32` = `uint32_t`），
  与 `WAL_FILE_HDR_SIZE 32u` 一致；`WalRec` 保持不变（记录头 28 字节是另一回事）。
- `KnHit` 用 `u32 row_id` + `float dist` + `float x, y` = 16 字节 ——
  与 `geo_brute_knn`（`geo_brute.c:78`）的输出类型对齐，**两边必须用同一个 `float` 口径**，
  否则浮点末位差异会让「结果全等」这条验收被噪声击穿（`rtree_knn.h` 的注释已经警告过这点）。
- `ft_tomb_page_remove` 加 `u32 xid` 参数 ⇒ 签名从 4 个参数变 5 个，
  **所有调用点都要改**（用 `grep -rn ft_tomb_page_remove src/` 找全）。
- 错误码统一用 `common/error.h` 里已有的：`E_CORRUPT` / `E_IO` / `E_NOSPACE` / `E_INVAL` ——
  **不新增错误码**（`E_NOTSUP` 的语义是「调用方换个做法」，不适合「格式不兼容」）。

### 比例

本计划约 900 行 / 设计文档 1484 行 ≈ 0.6:1。
其中约 35% 是「为什么这么做」的注释与坑位引用 ——
它们是**要照着做的指令**，不是背景叙述，删掉会导致执行者踩坑。
