# 八模态对称化 —— 第四个 SPI 契约 + 关系模态归位 + blob 归位 + 文档模态进 SQL（工作流 W-C）

> 依据：`docs/superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md` §4.5 / §6.1 / §6.3
> 实施依据：`engine/db/docs/PLAN-v1.6.md`（文档模态的原始方案，本计划是它的落地切片）
> 落地日期：2026-11-18
>
> **执行顺序（硬约束）—— 唯一权威是设计 §9，本块只是它的副本**：
>
> ```
> W-B 的 P0（CI 兜底）
>   → W-A 全部（冻结轨退役，含 §5.6 的 memctx 移植）
>   → 【本计划 Task C1】（执行器脱钩，此时 CI 已是绿的，能立刻发现回归）
>   → 【本计划 Task C2 / C3 / C4 / C5】
>   → W-B 的 P1 / P2（MM 开关与 smoke —— 此时文件归属已定，`tools/mm_profiles.mk` 一次建对）
>   → 【本计划 Task C6】（8 profile + 8 smoke 重跑）
>   → 【本计划 Task C7】（文档收官 + 父仓指针）
> ```
>
> ⚠️ **（可执行性修正）本块原来与设计 §9 冲突**：原文写「→ 本计划 C-1 → W-B P1 / P2」，
> 把 **C-2/C-3/C-4/C-5 漏在了 P1 之后**；紧接着的下一段又自己说「C-2 ~ C-5 必须全部排在 P1 之前」
> —— **同一段文字前后矛盾**，照任一顺序执行都会让 `MM_FILES_blob` 的文件归属失效。
> ⇒ 现按设计 §9 逐字对齐，**以设计 §9 为准**；本块若与之不符，改本块。
>
> **为什么 C1 ~ C5 全部要在 P1 之前**：P1 的 B1 会**第一次创建** `tools/mm_profiles.mk`，
> 那时文件归属必须已经定下来（C2/C3 搬完、C4/C5 建好 `src/repl_blob.c` / `src/repl_graph.c` /
> `src/db_json.c` / `src/repl_json.c`），才能**一次写对**。
> 反过来做（先 P1 再搬家）会让 B1 写下的 `MM_FILES_blob := src/storage/blob.c …` 全部失效
> ⇒ `MM=vector` 裁不掉 blob ⇒ P1 的验收必失败。
>
> **本计划 Task 编号 vs 设计 §9 的 `C-x` 编号**（两套编号，别混）：
>
> | 设计 §9 | 本计划 Task | 内容 |
> |---|---|---|
> | （无） | **C0** | 开工前处理无关脏文件（设计 §9 规则六） |
> | C-1 | **C1** | S1 + S2 —— `TableOpener` 契约 + 执行器脱钩 |
> | C-2 | **C2** | S3 + S6 —— 关系模态 / B+tree 适配器归位 |
> | C-3 | **C3** | S5 —— blob 归位 |
> | C-6 | **C4** | S10 —— blob / graph 的 REPL 归位 |
> | C-4 | **C5** | PLAN-v1.6 步骤 3~6 —— 文档模态接 SQL（含 S7 / S8） |
> | C-5 | **C6** | S9 门禁 + 8 profile + 8 smoke 重跑 |
> | （无） | **C7** | 文档收官 + 父仓指针 |

---

## Goal

让八个模态在主核里的**结构对称**、在公开 API 上**形态一致**、并且**全部接进 SQL**：

1. **补上第四个 SPI 契约 `TableOpener`** —— 执行器从此不认识任何模态（硬规则 7 补漏）。
2. **关系模态的 SPI 适配器归位** `src/modalities/relational/` —— 八模态目录对称。
3. **blob 模态归位** `src/modalities/blob/` —— 八模态目录对称。
4. **文档模态接进 SQL** —— JSON 列 + 路径索引 + `json_match`（PLAN-v1.6 的步骤 3/4/5）。
5. **门禁 + 8 个 profile + 8 个 smoke 全部重跑** —— 证明对称化没破坏任何东西。

## Architecture

```
engine/db/src/modalities/          ← 改造后：8 个模态，形态一致
├── relational/   ← 【新】关系模态的 SPI 适配器（从 src/spi/ 搬）
│   ├── rel_opener.c/h     关系模态的 TableOpener 实现（catalog → heap → heap_am_build）
│   ├── heap_am.c/h        关系模态的 StorageAM
│   ├── heap_am_batch.c
│   ├── heap_am_internal.h
│   └── btree_am.c/h       关系模态的索引适配器
├── blob/         ← 【新】blob 模态整体（从 src/storage/ + src/spi/ 搬）
│   ├── blob.c/h
│   ├── blob_hdr.c/h
│   └── blob_am.c/h
├── doc/          ← 文档模态（已有 3,787 行）+ 【新】json_func.c / json_match.c
├── ft/  geo/  graph/  ts/  vector/     ← 不动
```

**八模态的公开 API 形态**（对称性的判据）：

| 模态 | 公开 API 前缀 | 存储 | 事务语义 | SQL 入口 |
|---|---|---|---|---|
| relational | `db_exec` / `db_*` | 堆表 | MVCC | catalog 真表 |
| blob | `db_blob_*` | 对象 → 帧 → 页 | 不可变对象（xid=0） | `blob_frames` / `blob_objects` |
| graph | `db_graph_*` | 定长槽页 | 锁 + WAL | `graph_nodes` / `graph_edges` |
| vector | `db_vec_*` | 列式向量页 | 锁 + WAL | `vec_items` + `db_vec_knn` |
| ft | `db_ft_*` | 不可变 segment | 锁 + WAL | `ft_docs` + `db_ft_search` |
| geo | `db_geo_*` | MBR 嵌套树 | 锁 + WAL | `geo_points` + `db_geo_window` |
| ts | `db_ts_*` | 封窗列式压缩 | 锁 + WAL | `ts_readings` / `ts_partitions` |
| doc | `db_json_*` | **无**（寄生：heap 行上的列类型） | 继承 heap 的 MVCC | JSON 列 + 路径索引 + `json_match` |

**第四个 SPI 契约 `TableOpener`**（`spi/table_opener.h`）：

```
现在：执行器自己分两支 ── catalog 命中 → heap_am_build（执行器认得 heap！）
                        └─ 注册表命中 → spi_registry_find（执行器认得虚拟表）

之后：执行器只按顺序问两个 opener ── 谁先答上就用谁
        opener_catalog  （关系模态提供）
        opener_virtual  （SPI 注册表提供）
        执行器不再提 heap / Heap / HeapAmCtx
```

## Tech Stack

- C11、GNU Make（主核自带 Makefile）
- 既有机制：SPI（`StorageAM` / `IndexAM` / `Pushdown`）、B+tree 索引、record 变长列、外部值（`ext_fn`）
- SQLite JSON1 作为 JSON 语义的**裁判**（对照测试用，只读不进产物）

## Global Constraints

1. **提交信息用中文**；每个 Task 结束立即 `git push origin main`。
2. **主核硬规则**：`-Wall -Wextra -Werror` 零警告；新文件 ≤ 800 行；注释用中文；坑用 `M<层>-<序号>`。
3. **分层门禁每次改完必跑**：`sh tools/check_deps.sh` ⇒ `✓ 依赖方向正确`。
4. **执行器不许知道模态**（硬规则 7）—— C-1 的验收是**机械检查**：
   `grep -rniE 'heap|relational|blob|graph|vector|ft_|geo|ts_|json' engine/db/src/query/exec*.c`
   只允许命中**注释**。
5. **A7 审计清单先写后做** —— C-5 动执行器之前，先把"允许的落点"写进
   `engine/db/docs/MODALITIES.md`，实现完逐行对账。
6. **不新建 `db_rel.c`** —— 关系模态的公开接口就是 SQL。
   **不新建 `doc_am.c`** —— 文档模态是寄生模态，没有虚拟表、没有 `StorageAM`。
7. **不动 `engine/src/` 与 `engine/include/`** —— 那是 W-A 的地盘。

## Review Focus

| # | 关注点 | 判据 |
|---|---|---|
| R1 | 执行器真的不认识模态了吗 | `grep -rniE 'heap\|relational\|blob\|graph\|vector\|json' src/query/exec*.c` **零命中（连注释都不该有）**；`exec_internal.h` 不再 include `spi/heap_am.h` |
| R2 | 第四个契约是不是"活"的 | `TableOpener` 有**两个**实现（catalog 侧 + 注册表侧），执行器通过函数指针调用，**没有**任何 `if (is_virtual)` 之外的模态分支 |
| R3 | blob 搬家有没有漏 | `grep -rn 'storage/blob.h\|spi/blob_am.h' engine/db/src/` 应指向新位置；旧位置文件不存在 |
| R4 | 关系模态搬家有没有漏 | `grep -rn 'spi/heap_am' engine/db/src/` 只剩新位置 |
| R5 | JSON 语义对不对 | 对照 SQLite JSON1 的用例全等（转义 / Unicode / 嵌套 / 数字精度 / 重复键） |
| R6 | A7 有没有滑坡 | `grep -i json src/query/` 的命中**逐行**列在执行记录里，且**都在**允许落点内 |
| R7 | 两路全等 | 同一谓词走索引 / 不走索引，结果集完全相同 |
| R8 | MVCC 没被破坏 | `test_mvcc*` / `test_concurrent*` 全绿；JSON 文档替换在 RR 快照下不撕裂 |

---

## 文件结构

### 新增

| 文件 | 作用 |
|---|---|
| `engine/db/src/spi/table_opener.h` | **第四个 SPI 契约**：表打开器 |
| `engine/db/src/repl_blob.c` / `repl_blob.h` | blob 模态的 REPL（从 `main.c` 搬出，C-4） |
| `engine/db/src/repl_graph.c` / `repl_graph.h` | graph 模态的 REPL（从 `main.c` 搬出，C-4） |
| `engine/db/src/spi/table_opener.c` | 注册表侧 opener（虚拟表） |
| `engine/db/src/modalities/relational/rel_opener.c` | 关系模态的 opener（catalog → heap → StorageAM） |
| `engine/db/src/modalities/relational/rel_opener.h` | 同上，头 |
| `engine/db/src/db_json.c` | 文档模态的**公开 API 实现**（`db_json_encode` / `db_json_decode` / `db_json_match`）+ `db_json_init()`（S7） |
| `engine/db/src/repl_json.c` / `repl_json.h` | 文档模态的 **REPL 族**（`DOC` 动词族，S8） |
| `engine/db/src/modalities/doc/json_func.c` | 通用值层函数：`json_match` / `json_extract` 的注册与实现 |
| `engine/db/src/modalities/doc/json_match.c` | 匹配规格解析（`'$.a.b = 5'` → path + op + 值） |
| `engine/db/src/modalities/doc/json_match.h` | 同上，头 |
| `engine/db/test/unit/test_json_match.c` | 匹配规格解析与求值的单元测试 |
| `engine/db/test/integration/test_json_sql.c` | JSON 列 + 路径索引 + `json_match` 的端到端测试 |
| `engine/db/test/integration/test_json_index_equiv.c` | A3「两路全等」的专门测试 |

### 移动（git mv，保留历史）

| 从 | 到 |
|---|---|
| `engine/db/src/spi/heap_am.c` | `engine/db/src/modalities/relational/heap_am.c` |
| `engine/db/src/spi/heap_am.h` | `engine/db/src/modalities/relational/heap_am.h` |
| `engine/db/src/spi/heap_am_batch.c` | `engine/db/src/modalities/relational/heap_am_batch.c` |
| `engine/db/src/spi/heap_am_internal.h` | `engine/db/src/modalities/relational/heap_am_internal.h` |
| `engine/db/src/spi/btree_am.c` | `engine/db/src/modalities/relational/btree_am.c` |
| `engine/db/src/spi/btree_am.h` | `engine/db/src/modalities/relational/btree_am.h` |
| `engine/db/src/storage/blob.c` | `engine/db/src/modalities/blob/blob.c` |
| `engine/db/src/storage/blob.h` | `engine/db/src/modalities/blob/blob.h` |
| `engine/db/src/storage/blob_hdr.c` | `engine/db/src/modalities/blob/blob_hdr.c` |
| `engine/db/src/storage/blob_hdr.h` | `engine/db/src/modalities/blob/blob_hdr.h` |
| `engine/db/src/spi/blob_am.c` | `engine/db/src/modalities/blob/blob_am.c` |
| `engine/db/src/spi/blob_am.h` | `engine/db/src/modalities/blob/blob_am.h` |

### 修改

| 文件 | 改什么 |
|---|---|
| `engine/db/src/query/exec.h` | `Db` 增加两个 opener 指针 |
| `engine/db/src/query/exec_internal.h` | `TableCtx` 去掉 `Heap*` / `HeapAmCtx` / `am_storage`，改成不透明的 opener 缓冲 |
| `engine/db/src/query/exec_scan.c` | `resolve_table` / `resolve_table_id` 改走 opener |
| `engine/db/src/query/exec_eval.c` | 新增 `EX_FUNC` 求值入口（通用函数表，零模态知识） |
| `engine/db/src/query/parser.h` / `parser.c` / `parser_priv.h` / `parser_stmt.c` | 新增 `EX_FUNC` 节点 + 路径字面量 |
| `engine/db/src/storage/value.h` / `value.c` | 新增**通用值层函数表** + JSON 值变体 |
| `engine/db/src/storage/record.h` / `record.c` | `T_JSON` 列的编解码接线（部分已就绪，见 C-4.2） |
| `engine/db/src/db_internal.h` | include 路径随搬家调整；opener 的安装点 |
| `engine/db/src/db.c` | 安装两个 opener；注册 doc 模态的通用函数 |
| `engine/db/src/index/*` | 路径索引（表达式索引）的支持 |
| `engine/db/include/db.h` | 新增 `db_json_*` 公开 API |
| `engine/db/docs/MODALITIES.md` | 补 A7 审计清单 + 八模态盘点表 |
| `engine/db/tools/mm_profiles.mk` | **W-B 的 B1 创建**（在本计划 C-1~C-5 之后）；本计划 C-6 只做核对/校准 |

---

### Task C0：开工前先处理无关脏文件（**本计划的前置，不可跳过**）

  ⚠️ **（可执行性修正）这一步是必需的，不是可选的卫生工作。**
  工作区里现在有**与本计划完全无关**的未提交改动：

  | 状态 | 文件 |
  |---|---|
  | `M` | `engine/db/src/db_ft.c` |
  | `M` | `engine/db/src/modalities/ft/ft_index.c` |
  | `M` | `engine/db/src/modalities/ft/ft_index.h` |
  | `??` | `engine/db/test/integration/test_ft_reclaim_boot.c` |

  这 4 个文件**会被本计划后面几条目录级 `git add` 卷进去**
  （`git add engine/db/src/` / `git add engine/db/`），
  ⇒ 一次提交里混进别人的改动 ⇒ 提交历史被污染，回滚粒度也坏了。

  ⇒ **开工前先处理掉**，二选一（**不要带进本计划的任何一次提交**）：

  ```powershell
  cd C:\code\projects
  # 方案 A：单独提交（推荐 —— 保留工作成果，且与本计划的提交彻底分开）
  git add engine/db/src/db_ft.c engine/db/src/modalities/ft/ft_index.c `
          engine/db/src/modalities/ft/ft_index.h `
          engine/db/test/integration/test_ft_reclaim_boot.c
  git commit -m "fix(ft): 全文检索的回收与索引修正（与八模态对称化无关）"

  # 方案 B：先 stash（改动留在 stash 里，本计划做完再 pop）
  git stash push -m "ft 改动：与八模态对称化无关" -- `
      engine/db/src/db_ft.c engine/db/src/modalities/ft/ft_index.c `
      engine/db/src/modalities/ft/ft_index.h `
      engine/db/test/integration/test_ft_reclaim_boot.c
  ```

  ⚠️ **处理完必须确认工作区干净**（否则后面的显式路径 `git add` 也会把它们带上）：

  ```powershell
  git status --porcelain
  ```

  预期：**无输出**。

---

### Task C1：第四个 SPI 契约 `TableOpener` + 执行器脱钩

**Files**
- 新建：`engine/db/src/spi/table_opener.h`、`engine/db/src/spi/table_opener.c`、
  `engine/db/src/modalities/relational/rel_opener.h`、`engine/db/src/modalities/relational/rel_opener.c`、
  **`engine/db/test/unit/test_table_opener.c`**（C1.5b 新增，见下）
- 修改：`engine/db/src/query/exec.h`、`engine/db/src/query/exec_internal.h`、
  `engine/db/src/query/exec_scan.c`、**`engine/db/src/query/parser.h`**、
  **`engine/db/src/common/types.h`**（`SQL_MAX_COLS` 的新家，见 C1.3）、
  `engine/db/src/db_internal.h`、`engine/db/src/db.c`

**Interfaces**
- 新增契约（`spi/table_opener.h`）：

  ```c
  #ifndef DB_SPI_TABLE_OPENER_H
  #define DB_SPI_TABLE_OPENER_H

  #include <stddef.h>

  #include "catalog/catalog.h"
  #include "spi/storage_am.h"

  /*
   * 表打开器 —— SPI 的第 4 个契约。
   *
   * 【它补的是哪个洞】
   * 硬规则 7「执行器不许知道模态」。执行器解析表名时（query/exec_scan.c::resolve_table）
   * 现在自己分两支：
   *   - catalog 命中 → catalog_heap + heap_am_build  ← **执行器认得 heap**
   *   - 注册表命中 → spi_registry_find + am->columns  ← 执行器只认 StorageAM ✓
   * 第二支合规，第一支破窗。
   *
   * 【本契约怎么补】
   * 把"怎么把一张表变成一个 StorageAM"抽成接口，两个实现：
   *   - 关系模态的 opener（catalog → heap → heap_am_build）
   *   - 注册表的 opener（名字 → 已注册的 StorageAM）
   * 执行器只按顺序问这两个，谁先答上就用谁 —— **执行器不再提 heap**。
   *
   * 【为什么不能把 heap_am_build 塞进 StorageAM】
   * StorageAM 回答的是"拿到一行怎么办"（读 / 写 / 可见性 / 下推），
   * opener 回答的是"这张表从哪儿来"（找到它、给它一个 AM）。
   * 两个问题混在一个结构体里，虚拟表就得填一堆用不上的字段。
   */

  /*
   * 打开器的私有状态缓冲上限。
   *
   * 打开器可能需要为这张表准备一些 per-query 的私有状态
   * （关系模态要放 HeapAmCtx + 一个 StorageAM）。
   *
   * 【为什么是"调用方给缓冲"而不是"打开器返回指针"]
   * TableCtx 已经在栈上了（每条查询一个）。让打开器往调用方的缓冲里写，
   * 就少一次堆分配，也就不用管它的生命周期。
   *
   * 【为什么不把这些字段直接塞进 TableOpenResult】
   * 各模态的私有状态大小不同（关系模态约 200 字节，虚拟表为 0）。
   * 塞进公共结构体就是"给最胖的那个留位置，全家都付这份内存"。
   */
  /*
   * 打开器私有状态缓冲的字节上限。
   *
   * ⚠️ **（复审修正）原值 256 不够** —— 关系模态的私有状态实测是：
   *   `Heap*` 8 + `HeapAmCtx` 184 + `StorageAM` 152 = **344 字节** > 256。
   * （原值只按 `HeapAmCtx` 单件的 184 字节定，漏算了 `Heap*` 与 `StorageAM`。）
   * ⇒ 提到 **512**，并给下一个模态留一档余量。
   *
   * 【为什么改名】原名 `SPI_AM_CTX_MAX` 里的 "CTX" 在别处被当成
   * "执行器的上下文缓冲"，同一个名字两种含义。改名 `SPI_OPENER_PRIV_MAX`
   * 后语义唯一：**打开器私有状态的字节上限**。
   */
  #define SPI_OPENER_PRIV_MAX 512

  /* 打开器的产物。调用方持有，必须活到本次查询结束。 */
  typedef struct {
      StorageAM      *am;        /* 这张表的 StorageAM */
      const ColDesc  *cols;      /* 列定义（归调用方所有） */
      int             ncols;
      const IndexInfo *idxs;      /* 索引列表（归调用方所有） */
      int             nidx;
      u32             tid;       /* catalog 表 id；虚拟表为 0 */
      bool            is_virtual;
  } TableOpenResult;

  typedef struct TableOpener TableOpener;

  /*
   * 第四个 SPI 契约：表打开器。
   *
   * ⚠️ **（复审修正）`open` / `open_by_id` 都带两个"调用方给的缓冲"参数** ——
   * 起因是 `catalog_table_cols` **写进调用方给的数组、不返回指针**
   * （现有调用：`catalog_table_cols(db->cat, tid, tc->cols_buf, SQL_MAX_COLS, &ncols)`），
   * 所以 opener 光返回指针不够：列定义得由**调用方**备好缓冲，
   * `out->cols` / `out->idxs` 指回它 —— 这样"自持副本"纪律才继续成立。
   *
   * ⚠️ **本块是契约的唯一权威副本**（原文件里有初版与修正版两份并存，
   * 签名不一致、谁照抄谁编译不过；初版已删）。
   */
  struct TableOpener {
      const char *kind;          /* "catalog" / "virtual" —— 仅诊断用 */

      /*
       * 按表名打开。表不存在返回 E_NOTFOUND。
       *
       * owner      : 打开器的宿主（catalog / SpiRegistry / ...）
       * name       : 表名
       * out        : 产物（调用方持有）
       * cols_buf   : 调用方给的列定义缓冲（至少 SQL_MAX_COLS 项 ColDesc）
       * idxs_buf   : 调用方给的索引列表缓冲（至少 SQL_MAX_COLS 项 IndexInfo）
       * priv       : 调用方给的私有状态缓冲（至少 priv_bytes 字节）
       * priv_bytes : priv 的字节数
       *
       * out->cols / out->idxs 会指向 cols_buf / idxs_buf（不是打开器自己的内存）。
       */
      int (*open)(const TableOpener *op, void *owner, const char *name,
                  TableOpenResult *out,
                  ColDesc *cols_buf, IndexInfo *idxs_buf,
                  void *priv, size_t priv_bytes);

      /* 按表 id 打开（只有 catalog opener 支持；虚拟表返回 E_NOTSUP）。参数同上。 */
      int (*open_by_id)(const TableOpener *op, void *owner, u32 tid,
                        TableOpenResult *out,
                        ColDesc *cols_buf, IndexInfo *idxs_buf,
                        void *priv, size_t priv_bytes);

      /* 私有状态需要多少字节。 */
      size_t priv_bytes;
  };

  #endif /* DB_SPI_TABLE_OPENER_H */
  ```

- 新增公开符号：`db_json_encode` / `db_json_decode` / `db_json_match`（C-5 才有，本 Task 先不加）

**前置**：W-B P0 已完成（否则没有红灯可看）。

- [ ] **C1.1 新建 `src/spi/table_opener.h`**（上面的契约全文）

- [ ] **C1.2 新建 `src/modalities/relational/rel_opener.{h,c}`：关系模态的 opener**

  ```c
  /* rel_opener.h */
  #ifndef DB_MODALITIES_RELATIONAL_REL_OPENER_H
  #define DB_MODALITIES_RELATIONAL_REL_OPENER_H

  #include "spi/table_opener.h"

  /* 关系模态的表打开器（catalog → heap → StorageAM）。 */
  extern const TableOpener rel_table_opener;

  #endif
  ```

  ```c
  /* rel_opener.c */
  #include "rel_opener.h"

  #include <string.h>

  /*
   * ⚠️ **（可执行性修正）C-1 期间用现路径 `spi/heap_am.h`** ——
   * `modalities/relational/heap_am.h` 要到 **C-2** 才 `git mv` 过来。
   * 现在就写新路径 ⇒ **C-1 单独编不过**（"零警告 + 全绿"在 C-1 不可达）。
   * ⇒ **C-2 搬完家后必须同步改这一行**（已写进 C2.1 的清单）。
   */
  #include "spi/heap_am.h"
  #include "storage/heap.h"

  /*
   * 关系模态的私有状态：堆句柄 + AM 上下文 + AM 本体。
   *
   * 【为什么 AM 本体也在私有状态里】
   * heap 的 AM 是**每条查询现构造**的（它要绑定到具体的堆句柄 + 列定义 + 索引列表，
   * 而表是运行期动态创建的）。所以 AM 与它的上下文同生共死，
   * 一起放进调用方给的缓冲里。
   */
  typedef struct {
      Heap       *heap;
      HeapAmCtx   am_ctx;
      StorageAM   am_storage;
  } RelPriv;

  /*
   * 私有状态的大小契约 —— 越界就是编译期错误，不是运行期崩溃。
   */
  _Static_assert(sizeof(RelPriv) <= SPI_OPENER_PRIV_MAX,
                 "关系模态的私有状态超出 SPI_OPENER_PRIV_MAX");

  /*
   * 实测 sizeof(RelPriv) = Heap*(8) + HeapAmCtx(184) + StorageAM(152) = **344 字节**
   * ⇒ 原上限 256 装不下（这正是原 `_Static_assert` 必然炸的原因）。
   * 上限已提到 SPI_OPENER_PRIV_MAX = 512（见 C1.1 的契约）。
   */

  /* 把 AM 装进 TableOpenResult。 */
  static int rel_fill(const RelPriv *rp, const ColDesc *cols, int ncols,
                       const IndexInfo *idxs, int nidx, u32 tid,
                       TableOpenResult *out)
  {
      out->am        = &rp->am_storage;
      out->cols      = cols;
      out->ncols     = ncols;
      out->idxs      = idxs;
      out->nidx      = nidx;
      out->tid       = tid;
      out->is_virtual = false;
      return OK;
  }

  /*
   * ⚠️ **（复审修正）签名必须与 C1.1 的契约逐字一致** ——
   * 原样例是**修正前**的签名（少 `cols_buf` / `idxs_buf`），
   * 照抄会与函数指针类型不匹配，编译报错。
   */
  static int rel_open_by_id(const TableOpener *op, void *owner, u32 tid,
                            TableOpenResult *out,
                            ColDesc *cols_buf, IndexInfo *idxs_buf,
                            void *priv, size_t priv_bytes)
  {
      (void)op;
      Catalog *cat = (Catalog *)owner;
      RelPriv *rp = (RelPriv *)priv;

      if (priv_bytes < sizeof(RelPriv))
          return E_INVAL;

      Heap *heap = catalog_heap(cat, tid);
      if (!heap)
          return E_CORRUPT;

      /*
       * 列定义：catalog 写进**调用方给的数组**，不返回指针。
       * （catalog.h 的两段式用法：先问要多少，再拷。）
       */
      int ncols = 0;
      int rc = catalog_table_cols(cat, tid, cols_buf, SQL_MAX_COLS, &ncols);
      if (rc != OK)
          return rc;

      /*
       * 索引：同样拷进调用方给的数组。
       *
       * ⚠️ **（可执行性修正）这一步原来整段被 `...` 占掉了** ——
       * 没有索引列表，`heap_am_build` 拿到的 nidx = 0，
       * 建出来的 AM **不认任何索引** ⇒ 索引扫描静默失效
       * （查出来的结果看着永远对，但实际走的全是全扫，EXPLAIN 的理由还会误导人）。
       */
      int nidx = 0;
      rc = catalog_table_indexes(cat, tid, idxs_buf, SQL_MAX_COLS, &nidx);
      if (rc != OK)
          return rc;

      /*
       * 构造关系模态的 StorageAM。
       * AM 本体与它的上下文都住在**调用方给的 priv 缓冲**里
       * （AM 每条查询现构造 —— 见 heap_am.h 的文件头注释）。
       */
      rc = heap_am_build(heap, cols_buf, ncols, idxs_buf, nidx,
                         &rp->am_storage, &rp->am_ctx);
      if (rc != OK)
          return rc;

      rp->heap = heap;
      return rel_fill(rp, cols_buf, ncols, idxs_buf, nidx, tid, out);
  }

  /*
   * ⚠️ **（可执行性修正）按名打开 —— 契约里的 `open` 是主入口，原来整个没写**。
   *
   * 【为什么它比 open_by_id 更重要】
   * `resolve_table` 走的就是它（SQL 里的表名解析）；
   * `open_by_id` 只在"按表 id 打开"时用（少数内部路径）。
   * 少了它，C-1 就只是"契约已定"，执行器并没有真的脱钩。
   *
   * 【查不到表 ⇒ 返回 E_NOTFOUND】
   * E_NOTFOUND 是契约里约定的"我没这个表" ——
   * 执行器据此去问下一个 opener（虚拟表那边）。
   * 其它错误码一律原样透传（执行器不会再问下一个 opener）。
   */
  static int rel_open(const TableOpener *op, void *owner, const char *name,
                      TableOpenResult *out,
                      ColDesc *cols_buf, IndexInfo *idxs_buf,
                      void *priv, size_t priv_bytes)
  {
      Catalog *cat = (Catalog *)owner;

      u32 tid = 0;
      int rc = catalog_find_table(cat, name, &tid);
      if (rc != OK)
          return E_NOTFOUND;     /* 我没这张表 —— 让执行器去问下一个 opener */

      /* 找到之后与按 id 打开完全同一条路 */
      return rel_open_by_id(op, owner, tid, out, cols_buf, idxs_buf,
                            priv, priv_bytes);
  }

  /*
   * ⚠️ **（可执行性修正）opener 的初始化式 —— 契约里的 `priv_bytes` 原来没有来源**。
   *
   * `priv_bytes` 是"打开器的私有状态要多少字节"。
   * 不填它（或者填 0），执行器就按 0 字节给缓冲 ⇒
   * `rel_open_by_id` 里的 `priv_bytes < sizeof(RelPriv)` 恒真 ⇒
   * **每一次 opener 调用都返回 E_INVAL** ⇒ 表永远打不开。
   *
   * ⇒ 这里必须填 `sizeof(RelPriv)`，与 C1.1 契约里的字段一一对应。
   */
  const TableOpener rel_table_opener = {
      "catalog",
      rel_open,
      rel_open_by_id,
      sizeof(RelPriv)
  };
  ```

  ⚠️ **`catalog_table_cols` 的真实签名要照抄现有代码**：
  现有 `resolve_table` 调的是 `catalog_table_cols(db->cat, tid, tc->cols_buf, SQL_MAX_COLS, &ncols)` ——
  它**写进调用方给的数组**，不返回指针。

  ⇒ **因此 opener 不能只返回指针**，得把列定义也拷进 `TableOpenResult` 背后的缓冲。
  这就要求 `TableOpenResult` 里的 `cols` 指向**调用方的缓冲区**。

  ⇒ 修正契约：`TableOpenResult` 里的 `cols` / `idxs` 指针由**打开器**填，
  但指向哪里由**调用方**决定 —— 所以契约里要加两个"调用方给的缓冲"参数。

  > ⚠️ **（复审修正）本块原来重复定义了 `struct TableOpener`** ——
  > 与 C1.1 那份**初版契约**并存，两份签名不一致（初版没有 `cols_buf` / `idxs_buf`），
  > 谁照抄谁编译不过。
  > ⇒ **初版已删**；契约的**唯一权威副本在 C1.1**（`src/spi/table_opener.h`），
  > 本节只保留**为什么这么设计**的论证，不再重复定义。

  ```c
  /* 契约全文见 C1.1 —— 此处不重复定义，避免两份签名打架 */
  int (*open)(const TableOpener *op, void *owner, const char *name,
              TableOpenResult *out,
              ColDesc *cols_buf, IndexInfo *idxs_buf,
              void *priv, size_t priv_bytes);
  ```

  ⚠️ **`SQL_MAX_COLS` 是 32，不是 16**（`query/parser.h:36`）。
  ⚠️ **`SQL_MAX_COLS` 的归属要动**（设计 §2.13 R-15）：
  它现在住在 `src/query/parser.h:36`（值 **32**，注释仍写 16），
  但 `spi/table_opener.h` 的契约要用它（缓冲至少 `SQL_MAX_COLS` 项）——
  **L3 的 spi 头不能依赖 L3 的 query 头**。

  ⇒ **本 Task 一并把 `SQL_MAX_COLS` 挪到 `src/common/types.h`**，
  `query/parser.h` 改成 include 它（不改值、不改语义）。

  ⚠️ **改完必须查一遍所有引用点**：

  ```powershell
  Select-String -Path src/ -Pattern 'SQL_MAX_COLS'
  ```

  预期：**每一处都指向新家**（`common/types.h` 定义，其余 include 它）。


  ⚠️ 顺带记一笔（**本 Task 不改，只提醒**）：`parser.h` 与 `exec_internal.h:69-72`
  的注释仍写"=16"，与实际值 32 不符 —— 属既有文档债。

- [ ] **C1.3 新建 `src/spi/table_opener.c`：注册表侧的 opener**

  ```c
  #include "spi/table_opener.h"

  #include <string.h>

  #include "spi/registry.h"

  /*
   * 虚拟表打开器：从 SPI 注册表里按名字取出已注册的 StorageAM。
   *
   * 【为什么不复用 registry.c 里的 spi_registry_find】
   * 就是复用 —— 这里只是把它包装成 TableOpener 契约的形状。
   * 包装层存在的意义是**让执行器只认一种打开方式**。
   */
  static int virt_open(const TableOpener *op, void *owner, const char *name,
                       TableOpenResult *out,
                       ColDesc *cols_buf, IndexInfo *idxs_buf,
                       void *priv, size_t priv_bytes)
  {
      (void)op;
      (void)idxs_buf;
      (void)priv;
      (void)priv_bytes;

      SpiRegistry *reg = (SpiRegistry *)owner;
      StorageAM *am = spi_registry_find(reg, name);
      if (!am)
          return E_NOTFOUND;

      const ColDesc *vcols = NULL;
      int vn = 0;
      int rc = am->columns(am, &vcols, &vn);
      if (rc != OK)
          return rc;
      if (vn > SQL_MAX_COLS)
          return E_NOSPACE;
      if (vn > 0 && vcols)
          memcpy(cols_buf, vcols, (size_t)vn * sizeof(ColDesc));

      out->am         = am;
      out->cols       = cols_buf;
      out->ncols      = vn;
      out->idxs       = NULL;
      out->nidx       = 0;
      out->tid        = 0;
      out->is_virtual = true;
      return OK;
  }

  static int virt_open_by_id(const TableOpener *op, void *owner, u32 tid,
                             TableOpenResult *out,
                             ColDesc *cols_buf, IndexInfo *idxs_buf,
                             void *priv, size_t priv_bytes)
  {
      (void)op; (void)owner; (void)tid; (void)out;
      (void)cols_buf; (void)idxs_buf; (void)priv; (void)priv_bytes;
      /* 虚拟表没有 id —— 按 id 打开只对 catalog 里的用户表有意义。 */
      return E_NOTSUP;
  }

  const TableOpener spi_table_opener = {
      "virtual",
      virt_open,
      virt_open_by_id,
      0
  };
  ```

  ⚠️ `SQL_MAX_COLS` 定义在 `query/parser.h` —— 但 `spi/` 不该依赖 `query/`（分层是 L3 同层，
  但方向不对）。

  ⚠️ **（可执行性修正）落点定死，不留二选一** ——
  **`SQL_MAX_COLS` 挪到 `src/common/types.h`**（L0 层，谁都能 include），
  `query/parser.h` 改成 include 它（它本来就已经 include 了 `common/types.h`，零新增依赖）。
  **不要**挪到 `spi/table_opener.h` ——
  那样 `query/` 与 `common/` 就都得反过来依赖 `spi/`，方向比现在更糟。
  ⇒ 与 C1.2 里那段「本 Task 一并把 `SQL_MAX_COLS` 挪到 `src/common/types.h`」是同一件事，
  两个地方口径必须一致。
  ⚠️ 这会牵动 `query/parser.h` 与 `exec_internal.h` 的 include ——
  **改完跑一遍分层门禁**（`check_deps.sh` 里 `forbid src/spi ... query/` **不存在**，
  所以 spi→query 现在不违规；但方向上是反的，仍然要挪）。

- [ ] **C1.4 `src/query/exec.h`：`Db` 加两个 opener**

  ```c
  #include "spi/table_opener.h"

  struct Db {
      Pager    *pager;
      Catalog  *cat;
      TxnTable *txns;       /* ⚠️ 指针，不是副本 —— 本步**不改**它 */
      SpiRegistry *reg;
      Snapshot  snap;
      u32       cur_xid;

      /*
       * 表打开器 —— 执行器只认这两个函数指针，不认任何模态。
       *
       * opener_catalog：catalog 里的用户表（关系模态提供）
       * opener_virtual：模态注册的虚拟表（SPI 注册表提供）
       *
       * 解析顺序：先 catalog，后注册表（用户显式建的东西优先）。
       */
      const TableOpener *opener_catalog;
      const TableOpener *opener_virtual;
  };
  ```

  ⚠️ **`txns` 是指针不是值**（`exec.h:16/43` 原文写着「指向**共享的**事务表（指针，不是副本！）」，
  赋值点 `db.c:715` / `db.c:741` 都是 `= &inst->txns`）。
  本步**只加两个 opener 字段**，`txns` 一行都不许动 ——
  改成值就是复制了一份事务表，xid 分配器会分裂成两个。


- [ ] **C1.5 `src/query/exec_internal.h`：`TableCtx` 去模态化**

  删掉：

  ```c
  #include "spi/heap_am.h"
  ...
      /* 关系模态专用 */
      Heap         *heap;
      HeapAmCtx     am_ctx;
      StorageAM     am_storage;
  ```

  换成：

  ```c
  #include "spi/table_opener.h"

  typedef struct {
      StorageAM    *am;
      const ColDesc *cols;
      int           ncols;
      u32           tid;
      bool          is_virtual;

      /* 自持的副本（v1.2 步骤 1） */
      ColDesc       cols_buf[SQL_MAX_COLS];
      IndexInfo     idxs_buf[SQL_MAX_COLS];
      const IndexInfo *idxs;
      int           nidx;

      /*
       * 打开器的产物与私有状态。
       *
       * priv 是**不透明**的 —— 执行器不知道里面是什么，
       * 只负责给它一块够大的地方并交给打开器。
       */
      TableOpenResult open_result;

      /*
       * 打开器的私有状态缓冲。
       *
       * ⚠️ **（可执行性修正）常量名必须是 `SPI_OPENER_PRIV_MAX`** ——
       * C1.1 已经把常量**改名**了（原名 `SPI_AM_CTX_MAX` 已不存在）。
       * 写旧名 ⇒ **编译不过**（未定义标识符）。
       * **同名同值**：与 C1.1 的契约一致，`SPI_OPENER_PRIV_MAX = 512`
       * （原 256 装不下 344 字节的 `RelPriv`）。
       */
      unsigned char    priv[SPI_OPENER_PRIV_MAX];
  } TableCtx;
  ```

- [ ] **C1.6 `src/query/exec_scan.c`：`resolve_table` / `resolve_table_id` 改走 opener**

  ```c
  int resolve_table(Db *db, const char *name, TableCtx *tc, char *err, int errcap)
  {
      memset(tc, 0, sizeof(*tc));

      /*
       * 1. catalog 里的用户表（关系模态的 opener）。
       * 2. 模态注册的虚拟表（SPI 注册表的 opener）。
       *
       * 顺序与旧实现一致：用户显式建的东西优先。
       * 执行器**不知道**这两个 opener 分别是谁提供的 ——
       * 它只按顺序问，谁先答上就用谁。
       */
      const TableOpener *ops[2];
      ops[0] = db->opener_catalog;
      ops[1] = db->opener_virtual;

      for (int i = 0; i < 2; i++) {
          if (!ops[i])
              continue;
          int rc = ops[i]->open(ops[i], (i == 0) ? (void *)db->cat : (void *)db->reg,
                                name, &tc->open_result,
                                tc->cols_buf, tc->idxs_buf,
                                tc->priv, sizeof(tc->priv));
          if (rc == OK) {
              tc->am         = tc->open_result.am;
              tc->cols       = tc->open_result.cols;
              tc->ncols      = tc->open_result.ncols;
              tc->idxs       = tc->open_result.idxs;
              tc->nidx       = tc->open_result.nidx;
              tc->tid        = tc->open_result.tid;
              tc->is_virtual = tc->open_result.is_virtual;
              return OK;
          }
          if (rc != E_NOTFOUND)
              return rc;
      }

      snprintf(err, (size_t)errcap, "表不存在: %s", name);
      return E_NOTFOUND;
  }
  ```

  `resolve_table_id` 同理，改调 `opener_catalog->open_by_id`。

- [ ] **C1.7 `src/db.c`：**两个**入口都要安装 opener**

  ⚠️ **（复审修正）只装 `db_open` 会漏掉第二个连接** ——
  `db.c:728` 的 `db_connect()` **另建一个连接**并逐字段填 `conn->db.*`
  （`conn->db.txns = &db->inst->txns` 就在那儿）。
  只在 `db_open` 装 ⇒ `db_connect` 出来的连接两个 opener 都是 NULL
  ⇒ **第二个连接起所有表都报「表不存在」**。

  ⇒ **两处都要装**：

  ```c
/* (可执行性修正) db_open() 里**没有** `conn` 这个变量 —— 它用 `db->db.*`
 *
 * db.c 的两个入口名字不一样：
 *   L218  db_err db_open(const char *path, DB **out)   -> 内部变量是 db，字段是 db->db.*
 *   L728  db_err db_connect(DB *db, DB **out)         -> L734 DB *conn = calloc(...)，字段是 conn->db.*
 *
 * 照抄成 conn->db.* 会直接编译不过（未定义标识符 conn）。
 */
db->db.opener_catalog = &rel_table_opener;
db->db.opener_virtual = &spi_table_opener;

  ```c
  /* db_connect() 里（db.c:728 起），逐字段填完 conn->db.* 之后 */
  conn->db.opener_catalog = &rel_table_opener;
  conn->db.opener_virtual = &spi_table_opener;
  ```

  ⚠️ `db.c` 要 include `modalities/relational/rel_opener.h` 与 `spi/table_opener.h`。

  ⚠️ **与 W-B B3.3/B3.4 的冲突**：`db.c` 的模态 `#if` 守卫是 W-B P1 才加的，
  而本步（W-C C-1）在 W-B P1 **之前**执行 ⇒ 此时 `db.c` 里还没有 `#if DB_HAS_*`，
  两边不打架。**但** C5.7 要往 `db.c` 加 `db_json_init()` 时，
  那个调用点必须**同时**被 W-B 的裁剪守卫覆盖 —— 见 C5.7 的说明。

- [ ] **C1.8 机械验收：执行器不认识模态了**

⚠️ **（可执行性修正）原来写「零命中（连注释都不该有）」这条判据不可达** ——
`src/query/` 下实测还有下面这些命中，而且**大部分不该删**：

| 位置 | 内容 | 能不能删 |
|---|---|---|
| `exec_scan.c:199/200/222/268/269/286` | `catalog_heap(...)` / `heap_am_build(...)` **真实调用** | **本 Task 就是要消掉它们** |
| `exec_internal.h:5` | `#include "spi/heap_am.h"` | **本 Task 就是要删掉它** |
| `exec_internal.h:79` | `Heap *heap;` 字段 | 要改成 `const TableOpenResult *opened;` 之类 |
| `exec.c:6` | 注释「改造前：执行器直接用 heap_* / btree_*…」 | 可以留（这句是**历史说明**，不是残留） |
| `exec_batch.c:405` / `:579` | 注释「可见性由模态判断（heap 走 MVCC…）」 | 可以留（讲的是语义） |
| `exec_internal.h:42` | 注释「【为什么关系模态要单独放 heap / am_ctx / am_storage】」 | 要改（设计已改用 TableOpener） |
| `exec_cost.c:82` | **宏名** `CBO_HEAP_FETCH` | **重命名**成 `CBO_REL_FETCH`，不是删 |

⇒ 判据拆成**三层**，每层都能机械判定：

**第 1 层 — 标识符零命中（这是硬判据）**

```powershell
cd C:\code\projects\engine\db
Get-ChildItem src/query -Recurse -File -Include '*.c','*.h' |
  Select-String -Pattern '\bheap_am_build\b|\bHeapAmCtx\b|\bcatalog_heap\b|\bHeap\s*\*'
```

预期：**零命中**。（`exec_scan.c` 那 6 处、`exec_internal.h:79` 那个字段都在这条判据里。）

**第 2 层 — 不再 include 模态头**

```powershell
Get-ChildItem src/query -Recurse -File -Include '*.c','*.h' |
  Select-String -Pattern '#include\s+"spi/heap_am\.h"'
```

预期：**零命中**。

**第 3 层 — 剩余的 `heap` 字样必须都在「已知例外」清单里**

```powershell
Get-ChildItem src/query -Recurse -File -Include '*.c','*.h' |
  Select-String -Pattern 'heap|Heap' |
  ForEach-Object { "{0}:{1}" -f $_.Filename, $_.LineNumber }
```

允许残留的**只有**这 5 处（执行时逐条核对，多一处就要处置）：
`exec.c:6`（历史注释）、`exec_batch.c:405`、`exec_batch.c:579`（语义注释）、
`exec_cost.c:82`（宏名，**要重命名为 `CBO_REL_FETCH`**）、`exec_internal.h:42`（**要改写成 TableOpener 的说法**）。

⇒ **宏重命名与注释改写都算在本步内**；剩下的 3 处历史/语义注释**明确保留**。


- [ ] **C1.9 构建 + 全量测试**

  ```powershell
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：零警告；`✓ 依赖方向正确`；**95 个测试全绿**。

  ⚠️ **（复审修正）光"全绿"不够**（设计 §7 风险表）：
  TableOpener 改造是**执行器核心路径**的改动 ——
  必须**改造前后的 ctest 用例名集合逐条比对**，确认「不新增、不减少」：

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh   # 跑一次
# (可执行性修正) run.sh 的每行输出形状是 printf '  %-34s [PASS]'（L94）
#    / '[FAIL]'（L97）/ '[COMPILE FAIL]'（L89），所以用例名可以用这个正则抽：
& "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh 2>&1 |
  Select-String -Pattern '^\s*(\S+)\s+\[(PASS|FAIL|COMPILE FAIL)\]' |
  ForEach-Object { $_.Matches[0].Groups[1].Value } |
  Sort-Object | Out-File -Encoding utf8 before.txt

# 改造后同样跑一次，存 after.txt，然后逐条比对：
Compare-Object (Get-Content before.txt) (Get-Content after.txt)

# 预期：**无输出**（集合完全相同 —— 不新增、不减少、不改名）。
# 若只想看数量：(Get-Content before.txt).Count  -> 应为 95
  ⚠️ 只比「用例数相等」不够 —— 数量相同但名字换了，是另一种回归。


  ⚠️ `heap_am_build` 现在只在 `rel_opener.c` 里被调用 ——
  `exec_scan.c` 不再 include `spi/heap_am.h`，所以 `heap_am.c` 的 include 要跟着改。

- [ ] **C1.10 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/src/spi/table_opener.h engine/db/src/spi/table_opener.c `
          engine/db/src/modalities/relational/ engine/db/src/query/ `
          engine/db/src/db.c engine/db/src/db_internal.h
  git commit -m "feat(kernel): 第四个 SPI 契约 TableOpener —— 执行器脱钩模态"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task C2：关系模态归位 `src/modalities/relational/`

**Files**
- 移动：`src/spi/heap_am.{c,h}`、`src/spi/heap_am_batch.c`、`src/spi/heap_am_internal.h`、
  `src/spi/btree_am.{c,h}` → `src/modalities/relational/`
- 修改：所有引用这些头的地方

**Interfaces**
- include 路径变化：`spi/heap_am.h` → `modalities/relational/heap_am.h`；
  `spi/btree_am.h` → `modalities/relational/btree_am.h`。

**⚠️ 为什么"关系模态"也算模态**：它有独立的公开 API（SQL 之外的 `db_exec` 家族）、
独立的存储实现（`storage/heap*`）、独立的 SPI 适配器。
把它放进 `modalities/relational/` 是为了**八模态目录对称**，
但**存储实现仍留在 L1**（`storage/heap*` 不动）——
因为 `catalog.h:8` 与 `vacuum.h:6` 直接持有 `Heap*`，搬到 L4 会造成 L2→L4 的反向依赖。

- [ ] **C2.1 `git mv` 六个文件**

  ```powershell
  cd C:\code\projects
  git mv engine/db/src/spi/heap_am.c        engine/db/src/modalities/relational/heap_am.c
  git mv engine/db/src/spi/heap_am.h        engine/db/src/modalities/relational/heap_am.h
  git mv engine/db/src/spi/heap_am_batch.c  engine/db/src/modalities/relational/heap_am_batch.c
  git mv engine/db/src/spi/heap_am_internal.h engine/db/src/modalities/relational/heap_am_internal.h
  git mv engine/db/src/spi/btree_am.c       engine/db/src/modalities/relational/btree_am.c
  git mv engine/db/src/spi/btree_am.h       engine/db/src/modalities/relational/btree_am.h
  ```

- [ ] **C2.2 改 include 路径**

  ```powershell
Get-ChildItem engine/db/src -Recurse -File -Include '*.c','*.h' |
  Select-String -Pattern 'spi/heap_am|spi/btree_am'
    -Pattern 'spi/heap_am|spi/btree_am'
  ```

⚠️ **（可执行性修正）不能用 `Select-String -Path engine/db/src/**/*.c`** ——
PowerShell 的 `**` **至少要匹配一层目录**，`src/db_internal.h` / `src/db.c` / `src/db_blob.c`
这些**顶层文件一个都搜不到**（实测：`**` 只命中子目录里的文件），
而 `db_internal.h:31/33` 恰恰是最关键的引用点。
⇒ 递归必须交给 `Get-ChildItem -Recurse`。

  逐个改成 `modalities/relational/heap_am` / `modalities/relational/btree_am`。

  ⚠️ 已知引用点：`src/query/exec_internal.h`（C-1.5 已经删掉了）、
  `src/modalities/relational/rel_opener.c`、`src/modalities/relational/heap_am.c` 自身、
  `src/modalities/relational/heap_am_batch.c`、可能还有 `src/index/*`。

- [ ] **C2.3 确认 `src/spi/` 里剩下的东西**

  搬完之后 `src/spi/` 应该是：

  ```
  storage_am.h    契约 1
  index_am.h      契约 2
  pushdown.h      契约 3
  table_opener.h  契约 4
  registry.c/h    虚拟表注册表
  table_opener.c  注册表侧的 opener 实现
  ```

  ⚠️ **`src/spi/` 里不再有任何"某个模态的东西"** —— 这是 SPI 层的 purity。

- [ ] **C2.4 构建 + 门禁 + 全量测试**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：零警告；`✓ 依赖方向正确`；95 全绿。

  ⚠️ **`check_deps.sh` 的模态互斥规则现在会遍历到 `relational/`** ——
  它会检查 `relational/` 里有没有 include 别的模态。应该没有（它只依赖
  `storage/heap.h`、`catalog/catalog.h`、`spi/*`）。若门禁报错，说明搬家引入了反向依赖。

- [ ] **C2.5 提交**

  ```powershell
  cd C:\code\projects
# ⚠️ （可执行性修正）不要用目录级 add —— 工作区里有 4 个与本任务无关的改动：
#   M engine/db/src/db_ft.c
#   M engine/db/src/modalities/ft/ft_index.c
#   M engine/db/src/modalities/ft/ft_index.h
#   ?? engine/db/test/integration/test_ft_reclaim_boot.c
# ⇒ 按本 Task 实际改动的文件逐条列：
git add engine/db/src/spi/heap_am.c engine/db/src/spi/heap_am.h `
        engine/db/src/spi/heap_am_batch.c engine/db/src/spi/heap_am_internal.h `
        engine/db/src/spi/btree_am.c engine/db/src/spi/btree_am.h `
        engine/db/src/modalities/relational `
        engine/db/src/query/exec_scan.c engine/db/src/query/exec_internal.h

# 提交前确认没卷进 ft（输出必须为空）：
git diff --cached --name-only | Select-String -Pattern 'db_ft|ft_index|ft_reclaim'
  git commit -m "refactor(kernel): 关系模态的 SPI 适配器归位 modalities/relational/"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task C3：blob 模态归位 `src/modalities/blob/`

**Files**
- 移动：`src/storage/blob.{c,h}`、`src/storage/blob_hdr.{c,h}`、`src/spi/blob_am.{c,h}`
  → `src/modalities/blob/`
- 修改：引用这些头的文件

**Interfaces**
- include 路径变化：`storage/blob.h` → `modalities/blob/blob.h`；
  `spi/blob_am.h` → `modalities/blob/blob_am.h`。

**⚠️ blob 的存储实现从 L1 搬到 L4**：它现在住在 `src/storage/`，
但它**不是**通用存储设施 —— 它是 blob 模态自己的对象存储。
搬走之后 `src/storage/` 只剩真正通用的东西（page / pager / heap / record / value / freelist / gstore / batch）。

- [ ] **C3.1 `git mv` 六个文件**

  ```powershell
  cd C:\code\projects
  git mv engine/db/src/storage/blob.c      engine/db/src/modalities/blob/blob.c
  git mv engine/db/src/storage/blob.h      engine/db/src/modalities/blob/blob.h
  git mv engine/db/src/storage/blob_hdr.c  engine/db/src/modalities/blob/blob_hdr.c
  git mv engine/db/src/storage/blob_hdr.h  engine/db/src/modalities/blob/blob_hdr.h
  git mv engine/db/src/spi/blob_am.c       engine/db/src/modalities/blob/blob_am.c
  git mv engine/db/src/spi/blob_am.h       engine/db/src/modalities/blob/blob_am.h
  ```

- [ ] **C3.2 改 include 路径**

  ```powershell
Get-ChildItem engine/db/src -Recurse -File -Include '*.c','*.h' |
  Select-String -Pattern 'storage/blob|spi/blob_am'
    -Pattern 'storage/blob|spi/blob_am'
  ```

⚠️ **（可执行性修正）不能用 `Select-String -Path engine/db/src/**/*.c`** ——
PowerShell 的 `**` **至少要匹配一层目录**，`src/db_internal.h` / `src/db.c` / `src/db_blob.c`
这些**顶层文件一个都搜不到**（实测：`**` 只命中子目录里的文件），
而 `db_internal.h:31/33` 恰恰是最关键的引用点。
⇒ 递归必须交给 `Get-ChildItem -Recurse`。

  逐个改成 `modalities/blob/blob` / `modalities/blob/blob_am`。

  ⚠️ 已知引用点：`src/db_internal.h`、`src/db.c`、`src/db_blob.c`、
  `src/modalities/blob/*` 自身、`src/spi/registry.c`（可能没有）。

- [ ] **C3.3 检查 `src/storage/` 里剩下的文件是不是都"通用"**

  搬完之后 `src/storage/` 应含：
  `batch.c/h`、`freelist.c/h`、`gstore.c/h`、`gstore_cursor.c`、`heap*.c/h`、
  `page.c/h`、`pager*.c/h`、`record.c/h`、`value.c/h`。

  ⚠️ **`gstore.c` 仍在 L1** —— 它是 graph 与 vector **共用**的通用磁盘存储
  （`vector_am.c:125` 调 `gstore_cursor_init(vstore_meta(...))`），
  **不属于 graph**，也不能搬进 `modalities/graph/`。

- [ ] **C3.4 构建 + 门禁 + 全量测试**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：零警告；`✓ 依赖方向正确`；95 全绿。

- [ ] **C3.5 提交**

  ```powershell
  cd C:\code\projects
# ⚠️ （可执行性修正）不要用目录级 add —— 工作区里有 4 个与本任务无关的改动：
#   M engine/db/src/db_ft.c
#   M engine/db/src/modalities/ft/ft_index.c
#   M engine/db/src/modalities/ft/ft_index.h
#   ?? engine/db/test/integration/test_ft_reclaim_boot.c
# ⇒ 按本 Task 实际改动的文件逐条列：
git add engine/db/src/spi/heap_am.c engine/db/src/spi/heap_am.h `
        engine/db/src/spi/heap_am_batch.c engine/db/src/spi/heap_am_internal.h `
        engine/db/src/spi/btree_am.c engine/db/src/spi/btree_am.h `
        engine/db/src/modalities/relational `
        engine/db/src/query/exec_scan.c engine/db/src/query/exec_internal.h

# 提交前确认没卷进 ft（输出必须为空）：
git diff --cached --name-only | Select-String -Pattern 'db_ft|ft_index|ft_reclaim'
  git commit -m "refactor(kernel): blob 模态归位 modalities/blob/（src/storage 与 src/spi 不再有模态代码）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task C4：blob / graph 的 REPL 归位 `src/repl_blob.c` / `src/repl_graph.c`

**Files**
- 新建：`engine/db/src/repl_blob.c`、`engine/db/src/repl_blob.h`、
  `engine/db/src/repl_graph.c`、`engine/db/src/repl_graph.h`
- 修改：`engine/db/src/main.c`

**Interfaces**
- 新增：`int handle_blob(DB *db, char *line);` / `int handle_graph(DB *db, char *line);`
  —— 从 `main.c` 里的 `static` 变成外部函数，**签名与返回语义不变**
  （返回 0 表示这一行被消费了）。

**为什么这是对称性的一部分**：设计文档 §6.1 定义「模态的契约面 = 四件套」，
其中 ③ 是「REPL 族 = `src/repl_<m>.c`」。实测 blob 与 graph 的 REPL
**定义在 `src/main.c` 内部**（`static handle_blob` / `static handle_graph`），
其余五个模态才有独立的 `repl_<m>.c` ⇒ 这两个模态在 ③ 上**破了对称**。

**顺带的好处**：搬完之后 `main.c` 里只剩**派发分支**，
W-B 的第 5 处裁剪点（`#if DB_HAS_BLOB` 包住派发）就不用再包整坨 static 函数了。

- [ ] **C4.1 新建 `src/repl_blob.{c,h}`**

  把 `main.c` 里的 `static int handle_blob(DB *db, char *line)`（连同它的 static 辅助函数）
  **逐字**搬过来，改成外部函数：

  ```c
  #ifndef DB_REPL_BLOB_H
  #define DB_REPL_BLOB_H

  #include "db.h"

  /* blob 模态的 REPL 入口。返回 0 表示这一行被消费了。 */
  int handle_blob(DB *db, char *line);

  #endif /* DB_REPL_BLOB_H */
  ```

  ⚠️ **搬运纪律**：除「去掉 `static`」与「调整 include」外，**一个字符都不要改** ——
  注释、错误处理、返回值语义全部原样。这是搬家，不是重写。

- [ ] **C4.2 新建 `src/repl_graph.{c,h}`**（同上，搬 `handle_graph`）

- [ ] **C4.3 改 `src/main.c`**

  - 删掉 `handle_blob` / `handle_graph` 的函数体（以及它们的 static 辅助函数）
  - 加 `#include "repl_blob.h"` / `#include "repl_graph.h"`
  - 派发分支**不动**

⚠️ **`main.c` 会从 670 行掉到大约 240 行**（blob 约 130 + graph 约 300 搬走）。

实测 `engine/db/src/main.c` = **670 行**（`handle_blob` 在 **L113**、`handle_graph` 在 **L393**，
两个都是 `static` 函数）。搬家后这两个函数连同它们的辅助函数一起走，
`main.c` 剩 REPL 主循环 + catalog/heap/txn 那几族动词 ≈ 240 行。
  ⚠️ **`main.c` 的文件头注释要补一句**：
  「blob 与 graph 的 REPL 实现在 `repl_blob.c` / `repl_graph.c`」——
  否则下一个读代码的人会以为它们还在 `main.c` 里。

- [ ] **C4.4 构建 + 门禁 + 全量测试**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：零警告；`✓ 依赖方向正确`；**95 个测试全绿**。

  ⚠️ **`src/repl_*.c` 现在有 6 个**（vec / ft / geo / ts / blob / graph）——
  它们在 `src/` 顶层，`check_deps.sh` 的 `forbid` 规则**不覆盖 `src/` 顶层** ✓。

- [ ] **C4.5 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/src/main.c engine/db/src/repl_blob.c engine/db/src/repl_blob.h `
          engine/db/src/repl_graph.c engine/db/src/repl_graph.h
  git commit -m "refactor(kernel): blob 与 graph 的 REPL 从 main.c 归位为 repl_blob.c / repl_graph.c"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

  ⚠️ **`repl_blob.c` / `repl_graph.c` 属于 blob / graph 两个模态** ——
⚠️ **（复审修正）本步在 W-B 的 B1 之前** —— `tools/mm_profiles.mk` **还不存在**。
⇒ 正确说法是：**P1 的 B1 建 `mm_profiles.mk` 时，`MM_FILES_blob` 就要含 `src/repl_blob.c`**。
⇒ 在本步的**执行记录**里写下这句提醒，让后续执行者不会去找一个还不存在的文件。
⇒ 并且：`W-B 的 tools/mm_profiles.mk 里，MM_FILES_blob 要加 src/repl_blob.c`
  `MM_FILES_graph` 要加 `src/repl_graph.c`（见 Task C6）。

---

### Task C5：文档模态接进 SQL（PLAN-v1.6 的步骤 3 / 4 / 5）

**Files**
- 新建：`engine/db/src/modalities/doc/json_func.c`、`json_match.{c,h}`、
  `engine/db/test/unit/test_json_match.c`、
  `engine/db/test/integration/test_json_sql.c`、`test_json_index_equiv.c`
- 修改：`engine/db/src/query/parser.h` / `parser.c` / `parser_priv.h` / `parser_stmt.c`、
  `engine/db/src/query/exec_eval.c`、`engine/db/src/storage/value.{h,c}`、
  `engine/db/src/storage/record.{h,c}`、`engine/db/src/index/*`、
  `engine/db/src/db.c`、`engine/db/src/db_internal.h`、`engine/db/include/db.h`、
  `engine/db/docs/MODALITIES.md`

**Interfaces**
- 新的表达式节点：`ExprKind` 增加 `EX_FUNC`（通用函数调用）
- 新的值层接口：`val_func_register` / `val_func_find`（**通用函数表**，零模态知识）
- 新的公开 API：`db_json_encode` / `db_json_decode` / `db_json_match`
- 新的索引形态：**表达式索引**（索引键 = 路径提取的标量）

**⚠️ A7 审计清单必须先写（PLAN-v1.6 步骤 5 的第 1 条）**：
  执行器里允许出现的落点**是五处**（设计 §6.4 已把「原文写『只有两处』是错的」订正过来）——
  1. **通用值层**（`src/storage/value.{h,c}` / `record.h`）：值变体 + 函数表
  2. **求值入口**（`src/query/exec_scan.c::eval_expr` 与 `src/query/exec_eval.c::eval_where`
     —— **两条平行实现都要加**）：`EX_FUNC` 的分发
  3. **lexer**（`src/query/parser.c` 的词法部分）：认识 `JSON` 类型名与 `json` 路径字面量
  4. **parser 表达式**（`src/query/parser.h` / `parser.c` / `parser_priv.h`）：
     新增**通用**函数调用节点 `EX_FUNC`
  5. **parser DDL**（`src/query/parser_stmt.c`）：`CREATE INDEX` 支持 `json '$.a.b'` 的路径列

  **不允许**：任何 `if (strcmp(col_type, "json"))` 式的模态特判。
  落点 3/4/5 的原则是「**加通用能力，不加模态分支**」——
  `EX_FUNC` 是通用函数调用节点（将来 `geo_contains(...)` 也能用），`JSON` 是通用类型名。

- [ ] **C5.1 先写 A7 审计清单（写进 `engine/db/docs/MODALITIES.md`）**

  新增一节：

  ```markdown
  ## v1.6 · 执行器改动审计清单（A7）

  文档模态接进 SQL 时，执行器**只允许**在下面**五处**出现与文档有关的代码：

  | # | 落点 | 文件 | 允许的内容 | 禁止的内容 |
  |---|---|---|---|---|
  | 1 | 通用值层 | `src/storage/value.h` / `value.c` / `record.h` | `T_JSON` 值变体；通用函数表（`val_func_register` / `val_func_find`）；函数签名为 `int (*)(const DbValue *args, int nargs, DbValue *out)` | 出现 "json" 字样以外的模态名；按列类型分支的求值逻辑 |
  | 2 | 求值入口 | `src/query/exec_scan.c::eval_expr` **与** `src/query/exec_eval.c::eval_where`（两条平行实现，**都要加**） | `EX_FUNC` 的一次函数表查表 + 调用 | 任何 `strcmp(..., "json")`；任何路径解析；任何索引判断 |
  | 3 | lexer | `src/query/parser.c` 的词法部分 | 认识 `JSON` 类型名与 `json` 路径字面量（`'$.a.b'` 的词法） | 任何语义判断；任何模态名 |
  | 4 | parser 表达式 | `src/query/parser.h` / `parser.c` / `parser_priv.h` | 新增**通用**函数调用节点 `EX_FUNC` + `json_match` 的参数解析 | 为 `json_match` 特判；任何路径解析 |
  | 5 | parser DDL | `src/query/parser_stmt.c` | `CREATE INDEX` 支持 `json '$.a.b'` 的路径列 | 加模态分支；改其它 DDL 的语义 |

  审计命令（每次改动后跑）：

  ```bash
  grep -in json src/query/
  ```

  命中必须**逐行**列在执行记录里，且每行都能对上表里的某一格。

  **配套的不变量**：
  - 路径解析只在 `src/modalities/doc/` 里
  - 路径索引的匹配只在 `src/modalities/doc/json_match.c` 里
  - 执行器只负责"把 `json_match(doc, '...')` 这个表达式求出来"
  ```

- [ ] **C5.2 值层：通用函数表（零模态知识）**

  `src/storage/value.h` 增加：

  ```c
  /*
   * 通用值层函数表。
   *
   * 【它是什么】
   * 一个"名字 → 实现"的注册表。SQL 里的函数调用表达式（EX_FUNC）
   * 按名字在这里查表执行。
   *
   * 【为什么放在值层而不是 query 层】
   * 因为值层是"通用标量 + 外部值"的地方，是所有模态的公共词汇表。
   * 函数表放这里 ⇒ query 层只做一次查表，**不认识任何具体函数**。
   * （硬规则 7 的同一条纪律，换了个对象。）
   *
   * 【谁注册】
   * 提供这个函数的模态，在自己的初始化入口注册。
   * 例：文档模态在 db_json_init() 里注册 "json_match" 与 "json_extract"。
   */
  typedef int (*ValFunc)(const DbValue *args, int nargs, DbValue *out);

  /* 注册一个函数。同名返回 E_INVAL。 */
  int  val_func_register(const char *name, ValFunc fn);

  /* 查一个函数。找不到返回 NULL。 */
  ValFunc val_func_find(const char *name);
  ```

  `src/storage/value.c` 实现它（一个静态数组 + 线性查找，函数数量是个位数，够了）。

  ⚠️ **注册表是进程级全局** —— 与硬规则 5（共享状态）的关系：
  它**只读**（注册在初始化期，之后不再改），所以并发读是安全的。
  但**注册必须在任何查询开始之前完成** —— 放在 `db_open` 里。

- [ ] **C5.3 值层：JSON 值变体**

  `src/storage/value.h` 的 `DbValue` 已有 `T_JSON` 的类型位（`record.h` 有 `T_JSON = 6`），
⚠️ **（可执行性修正）这句原来是错的** —— `value.h` 里就是 `u8 type; /* ColType */`，
**没有 `ValType` 这个类型**。`T_JSON` 直接来自 `storage/record.h` 的 `ColType`。
  `record.h` 定义 `ColType {T_TEXT=4, T_REAL=5, T_JSON=6}`，
  `value.h` 定义 `DbValue.type`（字段名也是 `type`）。
  如果是两套枚举，需要在值层加 `T_JSON` 并写清映射。

  当前已落地的实现已按用户确认改为三层值表示：≤255B 在 `DbValue.s[]` 内联，256–2048B 用指向当前记录缓冲的通用外部值，>2048B 留待 C5.3b-b 页链生命周期设计。以下原始草图中“一律外部值”仅作为历史方案，不再是实现要求；字段名实际为 `external`（不是 `is_external`）。

  ```c
  /* 历史接口草图：当前不要求 JSONB 一律走页链外部值 */
  ```


- [ ] **C5.4 parser：加 `EX_FUNC` 表达式节点**

  `src/query/parser.h`：

  ```c
  typedef enum {
      EX_LIT,
      EX_COL,
      EX_BIN,
      EX_UN,
      EX_AGG,
      EX_FUNC,          /* v1.6：通用函数调用 */
  } ExprKind;

  struct Expr {
      ExprKind kind;
      int      op;                   /* EX_AGG 时是 AggKind */
      char     name[SQL_NAME_MAX];   /* EX_COL 的列名；EX_AGG / EX_FUNC 的函数名 */
      ...
      /* EX_FUNC 的实参 */
      Expr   **args;
      int      nargs;
  };
  ```

`src/query/parser.c` 的 `p_parse_expr` 增加**函数调用**分支：
识别 `IDENT (` → 解析实参列表（逗号分隔的表达式）→ `EX_FUNC`。

⚠️ **（可执行性修正）加了 `args` / `nargs` 之后，有两处必须同步改，否则一个泄漏、一个跑错**：

1. **`src/query/parser.c` 的 `free_stmt()`** —— 必须**递归**释放 `args` 链。
   现在它按 `kind` 释放 `EX_BIN` 的左右子树、`EX_UN` 的操作数、`EX_AGG` 的子树，
   但**没有 `EX_FUNC` 分支** ⇒ 每个函数调用的实参数组与节点全部泄漏
   （`test/run.sh` 里的 memcheck 用例会抓到）。

   ```c
   case EX_FUNC:
       for (int i = 0; i < e->nargs; i++)
           free_expr(e->args[i]);
       free(e->args);
       break;
   ```

2. **`src/query/exec_internal.h` 的 `bind_expr()`** —— 必须**递归**绑定 `args` 里的列号。
   `bind_expr` 的作用是把 `EX_COL` 的列名解析成列下标；
   如果只绑 `EX_FUNC` 节点本身而**不进 `args`**，那么 `json_match(doc, '$.a')` 的
   `doc` 那一列**永远绑定不上**（列号停在初值，取到错列或空值）⇒ 结果静默错误。

   ```c
   case EX_FUNC:
       for (int i = 0; i < e->nargs; i++) {
           int rc = bind_expr(e->args[i], cols, ncols);
           if (rc != OK)
               return rc;
       }
       break;
   ```

⇒ 这两处与 `EX_FUNC` 的求值入口（C5.6）是**同一个改动的三面**，必须同批提交。

  ⚠️ **与 `EX_AGG` 的区别**：`EX_AGG` 是**聚合**（AVG / COUNT / SUM），
  在 `exec_scan.c` 里有专门的聚合求值路径；`EX_FUNC` 是**标量函数**，
  逐行求值。两者不能混。

  ⚠️ **函数名是保留字吗**：`json_match` 不是 SQL 关键字，`p_accept_kw` 不会吃掉它。
  但 `json` 作为**类型名**（`CREATE TABLE t (doc JSON)`）需要加进 `p_parse_type`。

- [ ] **C5.5 parser：加 JSON 类型 + 路径字面量**

  `src/query/parser_stmt.c` 的 `p_parse_type`（L29-48）增加 `JSON`：

  ```c
  if (p_accept_kw(p, "JSON"))
      st->col_type = T_JSON;
  ```

⚠️ **（可执行性修正）不存在第二套枚举 —— 全树只有 `ColType` 这一套**
（本计划 C5.3b 已经核实过，这里原写的「`parser.h` 用的是 `ValType`」是错的）：

- `engine/db/src/storage/record.h` 的 `ColType` 里 **`T_JSON = 6`**；
- `engine/db/src/storage/value.h:79` 的 `DbValue.type` 就是 `u8   type;   /* ColType */`；
- 全树 grep `ValType` **零命中**（除本计划自己写错的地方）。

⇒ 直接用 `T_JSON`，不要引入第二套类型枚举。

  **路径字面量**（`CREATE INDEX ... ON t (json '$.a.b')`）：
  `p_parse_create_index` 现在的 `p_expect_ident(p, st->index_col)` 只认标识符。
  改成：

  ```c
  /*
   * 索引列可以是：
   *   - 裸列名             ON t (name)
   *   - 路径表达式         ON t (json '$.a.b')
   *
   * 【为什么路径表达式要用 "json '<path>'" 这种写法】
   * 因为路径不是列名，是一个**常量字符串**加一个**类型标记**。
   * 写成 `json '$.a.b'` 而不是 `$.a.b`，是为了：
   *   1. 不给 parser 加新的字面量语法（'$' 开头的 token）；
   *   2. 明确"这是一个路径表达式，不是列名"；
   *   3. 与 v1.6 方案 §1.1 的写法一致。
   */
  ```

  `Stmt` 增加两个字段：`bool index_is_path; char index_path[128];`

  ⚠️ **路径长度上限**：`JSONP_MAX_STEPS 16`（`json_path.h`）限制的是步数，
  路径字符串本身的长度上限自己定（128 够用；`json_path_parse` 里有自己的上限检查）。

- [ ] **C5.6 evaluator：`EX_FUNC` 的求值入口**

  `src/query/exec_scan.c` 的 `eval_expr`（**L42**）加一个 case：

  ⚠️ **（可执行性修正）位置搞错了** —— `eval_expr` 在 **`exec_scan.c:42`**（行式路径）；
  `exec_eval.c` 里那个叫 **`eval_where`（L201）**（列式路径）。两路都要加，见本步末尾。

  ```c
  case EX_FUNC: {
      /*
       * 通用函数调用：按名字查值层的函数表。
       *
       * 【这里为什么可以没有模态知识】
       * 函数表是通用的（value.c），函数名由 AST 带过来。
       * 执行器只做"查表 + 调用"，不知道 json_match 是什么。
       * 这就是 A7 审计清单里的"落点 2"。
       */
      int cap = e->nargs;
      DbValue args[8];
      if (cap > 8)
          return E_NOSPACE;
      for (int i = 0; i < cap; i++) {
          int rc = eval_expr(e->args[i], row, &args[i]);
          if (rc != OK)
              return rc;
      }
      ValFunc fn = val_func_find(e->name);
      if (!fn)
          return E_NOTSUP;
      return fn(args, cap, out);
  }
  ```

  ⚠️ **参数个数上限 8 是硬编码的** —— `DbValue args[8]` 在栈上。
  超了返回 `E_NOSPACE`。够用。

  ⚠️ **`eval_expr` 有两路**（行式 `exec_scan.c::eval_expr` 与列式 `exec_eval.c::eval_where`）——
  **两路都要加**！只加一路 ⇒ A3 的"两路全等"验收必然失败。
⚠️ 具体地：`exec_scan.c::eval_expr` 是行式路径，`exec_eval.c::eval_where` 是列式路径。
`EX_FUNC` 在两处都要能求值。

⚠️ **（可执行性修正）列式路径不能照抄行式的写法** —— 两路签名根本不同：

```c
/* 行式：能拿到 row */
int eval_expr(const Expr *e, const DbValue *row, DbValue *out);

/* 列式：**没有 row**，拿到的是批上下文 + 列描述 + 行号…… */
int eval_where(const Expr *e, BatchCtx *bc, const ColDesc *cols,
               int col, DbValue *out, int flags);   /* exec_eval.c:201 */
```

⇒ 上面那段 `eval_expr(e->args[i], row, &args[i])` 是**行式**的，列式里没有 `row` 可传。
**本计划采用「物化单行」的降级方案**（比给 `BatchCtx` 加取行 API 改动面小）：

1. 在 `src/query/exec_batch.h` 加一个辅助函数（**这是本步要新增的**）：

   ```c
   /*
    * 把批上下文里第 i 行物化成一个 DbValue 行。
    *
    * 【为什么需要它】
    * 列式求值是按列扫的，天然拿不到「一整行」。
    * 但通用函数调用（EX_FUNC）的实参可能是任意表达式，
    * 而值层的函数签名是 (args[], nargs, out) —— 它是**按行**的。
    * 所以这里必须能把一行拼出来。
    *
    * 【代价】
    * EX_FUNC 出现在 WHERE 里时，列式路径会退化成逐行物化。
    * 这是**有意接受**的：通用函数本来就带不走列式向量化的好处；
    * 而 json_match 这类函数占比很小，不值得为它把列式框架重构。
    */
   int batch_row_at(BatchCtx *bc, int i, DbValue *row_out);
   ```

2. `eval_where` 的 `EX_FUNC` case 写成：

   ```c
   case EX_FUNC: {
       DbValue row[SQL_MAX_COLS];
       int rc = batch_row_at(bc, col, row);   /* col 就是当前行号 */
       if (rc != OK)
           return rc;
       /* 之后照行式的写法：逐个求实参、查函数表、调用 */
       return eval_func_common(e, row, out);
   }
   ```

3. **把两路的公共部分抽成 `eval_func_common(const Expr *e, const DbValue *row, DbValue *out)`**
   （放在 `exec_internal.h` 声明、`exec_scan.c` 实现），行式与列式各调用它 ——
   这样 A3「两路全等」在 EX_FUNC 上**结构上就成立**，不靠两处手写对齐。

- [ ] **C5.7 文档模态：注册 `json_match` 与 `json_extract`**

  新建 `src/modalities/doc/json_match.{h,c}`：

  ```c
  /*
   * json_match 的匹配规格解析。
   *
   * 【规格的语法】
   *   <路径> <比较符> <字面量>
   * 例：'$.a.b = 5'、'$.name != "x"'、'$.age >= 18'
   *
   * 【为什么用字符串而不是 SQL 表达式】
   * 因为路径索引的键是"路径 + 标量"，而 WHERE 里要能同时表达
   * "路径 + 比较 + 值"。把整个谓词塞进一个字符串，
   * 索引匹配时只要解析出路径就能对上索引 —— 比在 SQL 层做表达式归一化简单得多。
   *
   * 【语法故意极简】
   * 只支持"路径 比较符 字面量"这一种形状。
   * 更多的形状（AND / OR / 嵌套）留给 SQL 层的组合 ——
   * 多个 json_match 用 AND 连起来，就是 AND 的语义。
   */
  typedef struct {
      JsonPath path;      /* 解析好的路径 */
      int      op;        /* JSON_OP_EQ / NE / LT / LE / GT / GE */
      DbValue  lit;       /* 右边的字面量 */
  } JsonMatchSpec;

  /* 解析匹配规格。失败返回 E_INVAL。 */
  int  json_match_parse(const char *text, JsonMatchSpec *out);

  /* 对一个 JSONB 值求值这个规格。 */
  int  json_match_eval(const u8 *jsonb, u16 len, const JsonMatchSpec *spec, bool *out);
  ```

  新建 `src/modalities/doc/json_func.c`：

  ```c
  /*
   * 文档模态向通用值层注册的两个函数。
   *
   * 【注册在什么时候发生】
   * db_open 里（db.c 调 db_json_init()）。
   * 值层的函数表是进程级全局，注册必须在任何查询之前完成。
   *
   * 【为什么函数表是全局的而不是实例级的】
   * 因为函数表是**纯代码的目录**，不持有任何状态 ——
   * 实例级的表反而会让"函数表"与"实例"的生命周期搅在一起。
   * 代价是：注册只能做一次（重复注册返回 E_INVAL，忽略即可）。
   */

  static int fn_json_match(const DbValue *args, int nargs, DbValue *out)
  {
      /* args[0] = 文档（JSONB 外部值），args[1] = 规格字符串 */
      ...
  }

  static int fn_json_extract(const DbValue *args, int nargs, DbValue *out)
  {
      /* args[0] = 文档，args[1] = 路径字符串；返回标量 */
      ...
  }

  int db_json_init(void)
  {
      val_func_register("json_match",   fn_json_match);
      val_func_register("json_extract", fn_json_extract);
      return OK;
  }
  ```

- [ ] **C5.3b 步骤 3 收尾：大文档外置接 `PT_JSON_OVF` 页链**

  ⚠️ **（复审修正）PLAN-v1.6 的「步骤 3」原来没有独立落点** ——
  它被混在 C5.3 的 `DbValue` 变体里，导致「大文档外置」这条
  只剩一个测试要求、没有实现步骤。

  PLAN-v1.6 步骤 3 的原文要求：**大文档（> 页大小）走页链外置；
  `ext_fn` 换沿链读；`DetoastFn` 契约一行不改**。

  ⇒ 本步要做三件事：

  1. **record 编解码按 `cols[i].type` 分流**：
     - `T_JSON` 且内联长度 ≤ `JSON_INLINE_MAX`（`json_ovf.h` 里是 2048）⇒ 内联存；
     - 超长 ⇒ **外置**：正文落到 `PT_JSON_OVF` 页链，
       `DbValue` 里只留 `external = true` + `(key, off, len)`。

     ⚠️ **字段名是 `external`，不是 `is_external`**（`storage/value.h:95`）——
     写错就编译不过。

  2. **`ext_fn` 沿链读**：外部值的读取函数**沿 `PT_JSON_OVF` 页链**逐页拼出全文，
     **不改 `DetoastFn` 的签名**（`value.h:128`）。

  3. **只取路径字段时不读全文**（A2 的实现侧）：
     路径索引命中时**只读那一段**，不把整条链拉上来 —— 用页读计数断言。

  ⚠️ **`DbValue.type` 只有 `ColType` 一套枚举**（`value.h:79`），
  **不存在 `ValType`** —— 别引入第二套类型枚举。


  ⚠️ **`fn_json_match` 的第一个实参是 JSONB** ——
  它可能是外部值（`ext_fn`），要 `val_detoast` 出来再解析。
  `val_detoast` 已有（`storage/value.h`）。

- [ ] **C5.3b-a：≤2048B JSON 列端到端往返（页链所有权暂缓）**

  **范围裁决（2026-11-22，用户批准）**：本子步先修复 JSON 列目前的假成功，
  不接 `PT_JSON_OVF` 页链。实测当前行为：

      CREATE TABLE t (id INT, doc JSON)       → OK
      INSERT ... '{"a":5}'                   → OK（但原始文本字节被当 JSONB 存）
      SELECT id FROM t                        → OK
      SELECT doc FROM t                       → E_INVAL
      SELECT json_extract(doc, '$.a') FROM t  → E_CORRUPT

  本子步接受的输入仅为 SQL `TEXT`（JSON 文本）或 SQL `NULL`；`INT / REAL / BOOL`
  不做隐式 JSON 标量转换，直接返回类型错误。`TEXT` 由 doc 模态注册的编码器
  解析为 JSONB；编码后长度 `<= 2048` 才允许进入 heap 写路径；大于 2048、JSON
  语法错误或编码器未注册时，必须在 heap 写入前失败，不能产生部分 tuple。
  `NULL` 仍按 SQL NULL 保存，不写 JSONB `null`。

  `JSONB → JSON 文本` 复用 C5.3 的 `jsonb_render` 注册钩子。该阶段的验收是
  小文档与中等文档（包括 256–2048B、运行时 `DbValue.s[]` 放不下的记录内联 JSON）
  可 SELECT、可 `json_extract` / `json_match`，并且内容完整。

  **明确不在本子步实现**：超过 2048B 的页链写入、页链 tuple 所有权、事务 abort、
  WAL/崩溃恢复、DELETE/VACUUM 回收。原因：当前 `json_ovf_write/read/free` 没有 WAL
  或 tuple owner；现有 heap AM / vacuum / abort 接口不能保证资源生命周期。不得仅把
  `json_ovf_write` 塞进 `val_to_record` 或 `heap_am_insert`。这些工作留给独立的
  C5.3b-b 设计与实现，须先定义每个 heap tuple 版本独占页链的发布、补偿、恢复和回收协议。

  本子步的实现边界：执行器 INSERT/UPDATE 在进入关系 Heap AM 前，将 JSON 列 TEXT
  调值层注册的通用编码入口转成 `T_JSON`；值层/记录层不 include doc 模态；关系 AM
  和 record 编码只接收已编码 JSONB。编码上限由 doc 模态自己裁决
  （`JSON_INLINE_MAX`，见 `modalities/doc/json_ovf.h`）—— query 层不写死这个数字
  （A7 审计：查询层不得认识具体模态的表示细节）；转换失败不写 heap。

  **SQL 字符串字面量**上限 8192B（`TK_STR_MAX`），超限明确报错，绝不静默截断；
  `TEXT` 列超过 255B 显式报错（`E_NOSPACE`），不静默截断成 63B 或存成空串。
  详见 `docs/superpowers/specs/2026-11-23-json-column-long-literal-design.md`
  与实施计划 `docs/superpowers/plans/2026-11-23-json-column-long-literal.md`。

- [ ] **C5.3b-b：`PT_JSON_OVF` 页链所有权与 MVCC 生命周期（待独立设计）**

  用户已确认目标所有权模型：每个 heap tuple 版本独占一条页链；运行时 abort 回收本
  事务创建且没有被已提交版本接管的链；DELETE 只标记 `xmax`；只有 VACUUM 在旧版本
  确认对所有活跃快照不可见、并物理删除 tuple 时才释放其链。

  此子步开始前，必须先独立设计并通过用户审阅：
  1. tuple 内 overflow tag、chain head、长度的持久化格式（和 record 的变长区可兼容）；
  2. chain 的写入发布顺序及 INSERT/UPDATE/索引失败后的补偿；
  3. JSON 页链与 WAL / redo / undo / abort 的关系；`json_ovf.c` 当前直接改 Pager 页且明确不写 WAL；
  4. VACUUM 在物理删 tuple 前后如何以可重试、幂等方式释放链，避免半释放 / double-free；
  5. scan/read 两趟 detoast 与并发 VACUUM 的互斥/快照保护；
  6. 启动恢复时如何找到孤儿链、以及 `DetoastFn` 的 `u16` 长度上限 65535 的公开语义。

  在以上契约与故障注入测试获批前，禁止把 JSON 溢出页链接入生产 heap 写路径。

- [ ] **C5.8 索引：路径索引（写时维护）**


  **这是 C-5 里最大的一块。** PLAN-v1.6 判决 2 已把路线定死：
  **写时同步维护**（文档更新 = 整替换 → 删旧键、插新键），不是"快照 + 兜底"。

  需要做的：

   1. **索引定义**：catalog 里多存"这个索引是路径索引，路径是 `$.a.b`"。

      ⚠️ **（复审修正）字段落点不是 `IndexInfo`，是 `IndexMeta`** ——
      现有 `src/catalog/catalog.h:102-106` 的 `IndexInfo` 是**索引句柄**，不是元数据：

      ```c
      typedef struct {
          IndexMeta meta;   /* 元数据 */
          BTree    *bt;     /* 已打开的 B+tree */
          FreeList *fl;     /* 内部用，外部别碰 */
      } IndexInfo;          /* ← 句柄！不能动 */
      ```

      它的使用者有 7 处，跨 3 层（`catalog.c:486/533`、`catalog_load.c:239`、
      `catalog_lock.c:282`、`spi/heap_am.c:36/53/438/507`）——
      给它加字段会**抹掉 B+tree 句柄**，索引扫描全废。

      ⇒ **路径字段加在 `IndexMeta` 上**（`catalog.h` 里 `IndexMeta` 的定义），`IndexInfo` **一个字都不动**：

      ```c
      typedef struct {
          /* ... 现有 IndexMeta 字段 ... */
          u16  col;          /* 普通索引的列号 */
          bool is_path;      /* true = 路径索引 */
          u8   path_len;
          char path[64];     /* 路径文本（is_path 时有效） */
      } IndexMeta;
      ```

      ✅ 好处：`exec_internal.h` 的 `idxs_buf[SQL_MAX_COLS]` **完全不受影响**
      （`IndexInfo` 仍是 100 字节，32 项 = 3.2 KB，在栈上，可接受）。

      ⚠️ **catalog 的索引元数据要能存下这个字段** —— 落盘格式要加一段。
      这是**有代价的**（要改 catalog 的序列化）——
      设计 §8.5 已为此开了**唯一豁免**：只允许改 `IndexMeta` 这三处，
      不动 catalog 的其它结构、其它落盘格式、锁与恢复逻辑。

      ⚠️ **改动面比"加个字段"大**，实际要碰 4 个文件：
      `src/catalog/catalog.h`（`IndexMeta` 定义）、`src/catalog/catalog.c`（序列化）、
      `src/catalog/catalog_load.c`（反序列化）、`src/spi/heap_am.c`（消费元数据）。
      **这 4 个文件必须进本任务的修改清单**（见 C5.8.1）。

     ⚠️ **更省的做法**：路径索引**不进 catalog 的普通索引结构**，
     而是由文档模态**自己管理**（它有自己的元数据 ——
     硬规则 6「模态各自管自己的元数据」）。
     路径索引放在文档模态自己的目录里，catalog 只记"这张表有一个文档模态的路径索引"。

     ⚠️ **但那样就绕过了 B+tree 机制**（PLAN-v1.6 判决 2 说"B+tree 既有能力，零新机制"）。
     复用 B+tree 需要 catalog 侧的索引定义能表达"表达式索引"。

     ⇒ **决定**：扩展 `IndexInfo` + catalog 的索引元数据，加"表达式索引"这一种形态。
     这是**唯一**需要动落盘格式的地方，值得。

  2. **索引键的生成**：路径索引的键 = `json_extract_scalar(doc, path)` 的**通用标量**
     （PLAN-v1.6 判决 1 的映射表：string→TEXT、number→INT/REAL、true/false→INT 1/0、
     null/不存在/类型不匹配→NULL）。
     ⇒ 键的编码复用 `index_key.c` 的现有编码（按标量类型编码）。

  3. **写路径**：
     - INSERT：算出路径值 → 插索引键。
     - UPDATE（整替换）：删旧键（用旧文档算路径值）→ 插新键。
     ⚠️ **UPDATE 的旧值从哪来**：heap 的 MVCC 版本链里有旧版本（`xmin`/`xmax`），
     所以旧键能算出来。

   4. **查询路径（执行器用索引）**：
      - WHERE 里有 `json_match(doc, '$.a.b = 5')` → 解析出 path + op + 值。
      - 在表的索引里找"路径索引且 path 相同且 op 可用"的。
      - 用它做索引扫描 → 拿到候选行 → **执行器重新求值整个 WHERE**（超集契约）。
      - 索引扫描的结果必须**是超集**（索引可能给出不完全匹配的候选）。
      - ⚠️ **谓词下推必须走 `spi/pushdown.h` 的下推契约**（设计 §6.1 的要求）：
        路径谓词的"能不能下推、推给谁"由**下推契约**表达，
        执行器**不**直接问文档模态"这个谓词你认不认"。

⚠️ **（可执行性修正）这里原来重复了一遍「4. 查询路径」，内容与上面完全相同 —— 已删。**
（上面那段含「谓词下推必须走 `spi/pushdown.h` 的下推契约」，是完整版。）

- [ ] **C5.9 A3 验收：两路全等**
  ⚠️ **（复审修正）PLAN-v1.6 的「反向验证 2 处」原来只做了一处** ——
  必须做**两处注入**，证明这套测试**真的会红**（否则测试是摆设）：

  | # | 注入 | 期望 |
  |---|---|---|
  | 1 | **标量映射把 `null` 映成 INT 0**（把 `json_extract_scalar` 里 null 的分支改成返回 0） | NULL 语义的用例必须**红** —— 若仍绿，说明这条用例没真正覆盖 null |
  | 2 | UPDATE 漏删旧键（把写路径里"删旧键"那行注释掉） | A4 的索引对账用例必须**红** —— 若仍绿，说明对账没数对 |

  ⇒ 两个注入各做一次，**每次都要看到红**，然后**回滚注入**、再跑一遍确认全绿。
  两次注入的截图/输出写进执行记录。



  新建 `engine/db/test/integration/test_json_index_equiv.c`：

  ```c
  /*
   * A3：同一谓词，走路径索引 vs 不走索引全扫过滤，结果集全等。
   *
   * 【怎么造出"不走索引"的那一路】
   * 同一张表、同一批数据，跑两遍查询：
   *   1. 建了路径索引 ⇒ 走索引
   *   2. 删掉路径索引 ⇒ 全扫
   * 比对两遍的结果集（按 rowid 排序后逐行比）。
   *
   * 【为什么必须这样比】
   * 因为索引实现可能有 bug（漏行）。超集契约要求的是
   * "索引给的候选 ⊇ 满足条件的行"，所以索引结果必须是全扫结果的**子集**，
   * 且两者的**最终结果**必须完全相同。
   */

  int main(void)
  {
      /* 1. 建表 + 灌 200 行（含各种类型：数字 / 字符串 / 布尔 / null / 缺字段 / 对象 / 数组） */
      /* 2. 建路径索引，跑查询，收集结果 */
      /* 3. 删路径索引，再跑，收集结果 */
      /* 4. 逐行比对 */
      /* 5. 反向验证：故意让索引少给一行 ⇒ 断言必须红 */
  }
  ```

  ⚠️ **反向验证**是 PLAN-v1.6 风险 R6 的要求：
  "反向验证编译不过 → 跑旧二进制"与"UPDATE 漏删旧索引键 → A4 对账红"。

- [ ] **C5.10 A4 验收：索引维护对账**

  ```c
  /*
   * A4：文档更新后旧键不残留（索引项计数对账）、新键可命中。
   *
   * 对账口径：索引里的项数 == 当前活跃文档里该路径有值的文档数。
   */

  int main(void)
  {
      /* 1. 建表 + 灌 100 行，每行 doc = {"a": <i>} */
      /* 2. 建路径索引 */
      /* 3. 断言：索引项数 == 100 */
      /* 4. UPDATE 其中 50 行的 doc = {"a": 1000 + i} */
      /* 5. 断言：索引项数仍然是 100（旧键被删了，新键插进来了） */
      /* 6. 断言：查 a = 1000 + i 能命中，查 a = i（旧的）不能命中 */
  }
  ```

- [ ] **C5.11 A1 / A2 / A5 / A6 验收测试**

  - **A1**（JSONB 编解码对照 SQLite JSON1）：`test/unit/test_jsonb.c` 已存在，
    补齐转义 / Unicode 代理对 / 嵌套 / 数字精度 / 重复键保留首个的用例。
  - **A2**（外部值收官）：大文档走页链；**只取路径字段时不读全文** ——
    页读计数对比（v0.9 IO 断言模式）。
  - **A5**（MVCC）：并发读 + 文档替换，RR 快照下不撕裂（复用 `test_mvcc_it.c` 的骨架）。
  - **A6**（崩溃恢复）：文档页链 + 路径索引（reindex）恢复后一致。

- [ ] **C5.12 公开 API：`db_json_*`**

  `engine/db/include/db.h` 增加：

  ```c
  /*
   * 文档模态的公开 API。
   *
   * 【文档模态是寄生模态】
   * 它没有自己的存储、没有自己的虚拟表 —— JSONB 存关系模态的 heap 行里。
   * 所以它的公开 API 是**值层面的**（编解码 / 取路径），
   * 而不是"打开一个文档存储"。
   */

  /* 文本 → JSONB。out 由调用方 free。 */
  db_err db_json_encode(const char *text, u8 **out, u16 *len);

  /* JSONB → 文本。out 由调用方 free。 */
  db_err db_json_decode(const u8 *jsonb, u16 len, char **out);

  /*
   * 对一个 JSON 文本做匹配规格求值。
   * spec 形如 "$.a.b = 5"。
   * out = true 表示匹配。
   */
  db_err db_json_match(const char *json_text, const char *spec, bool *out);
  ```

- [ ] **C5.13 A7 审计 + 全量回归**

  ```powershell
  cd C:\code\projects\engine\db
  Select-String -Path src/query/*.c,src/query/*.h -Pattern 'json'
  ```

⚠️ **（可执行性修正）这条判据执行时一跑就红** —— 基线里本来就有 2 处 `json` 命中，
而落点表（五处）里没有它们：

| 基线位置 | 内容 | 处置 |
|---|---|---|
| `src/query/lexer.c:81` | 注释「v1.1 加了 REAL 类型（AVG 的结果、地理坐标、时序数值、**JSON** 数字）」 | **保留** —— 这是讲 `REAL` 用途的枚举，与文档模态无关 |
| `src/query/parser_stmt.c:39` | 注释「值层与记录层早就有 T_REAL 了（AVG 的结果、坐标、时序数值、**JSON** 数字）」 | **保留** —— 同上 |

⇒ 把判据改成：**「改动前后对比，新增命中为零」**，而不是「绝对零容忍」：

1. **改 C5 之前**先跑一遍 `grep -in json src/query/`，把结果存成 `before-json.txt`
   （预期恰好上面 2 行）；
2. 每改完一步再跑，结果与 `before-json.txt` 做差集；
3. **差集必须只落在 A7 落点表五格之内** —— 落在五格之外 = 违规。

```powershell
cd C:\code\projects\engine\db
# 改动前（一次性）
Get-ChildItem src/query -Recurse -File -Include '*.c','*.h' |
  Select-String -Pattern 'json' -CaseSensitive:$false |
  ForEach-Object { "{0}:{1}" -f $_.Filename, $_.LineNumber } |
  Sort-Object | Out-File -Encoding utf8 before-json.txt

# 每步之后
Get-ChildItem src/query -Recurse -File -Include '*.c','*.h' |
  Select-String -Pattern 'json' -CaseSensitive:$false |
  ForEach-Object { "{0}:{1}" -f $_.Filename, $_.LineNumber } |
  Sort-Object | Out-File -Encoding utf8 after-json.txt
Compare-Object (Get-Content before-json.txt) (Get-Content after-json.txt)
```

`Compare-Object` 的输出里，**每个 `=>`（SideIndicator = `=>`，新增）的行**都必须能对上 A7 五格之一：
① 值层（`storage/`）② 求值入口（`exec_scan.c` / `exec_eval.c`）③ lexer ④ parser 表达式 ⑤ parser DDL。
`<=`（消失）的行不需要解释（那是被改掉的）。

⇒ **行号会漂移**，所以比对用「文件名:行号」会误报 —— 更稳的做法是同时存**整行内容**，
按内容做差集（`Select-String` 的 `-Raw`，或用 `$_.Line`）。执行时以内容为准。

**原始要求（保留）**：逐行核对，写进 `engine/db/docs/MODALITIES.md` 的执行记录。
  写进 `engine/db/docs/MODALITIES.md` 的执行记录。

  ```powershell
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：零警告；`✓ 依赖方向正确`；**全部测试绿**（95 + 新增）。

  ⚠️ **A8 的第三项「新文件 < 800 行」原来没有验收命令** —— 补上：

  ```powershell
  cd C:\code\projects\engine\db
  Get-ChildItem src/modalities/doc,src/db_json.c,src/repl_json.c -Recurse -File |
    Where-Object { $_.Extension -in '.c','.h' } |
    ForEach-Object {
      $n = ([System.IO.File]::ReadAllLines($_.FullName)).Count
      if ($n -gt 800) { "超限 {0} 行: {1}" -f $n, $_.FullName }
    }
  ```

  预期：**无输出**（零超限）。⚠️ `Measure-Object -Line` 对 UTF-8 源文件会**偏低**，
  必须用 `[System.IO.File]::ReadAllLines().Count`。


- [ ] **C5.14 `db_json_*` 的实现落点：新建 `src/db_json.c`**（S7）

  ⚠️ **（复审修正）原计划只给了 `include/db.h` 的声明，没给实现的落点** ——
  `db_json_encode` / `db_json_decode` / `db_json_match` 三个公开函数
  以及 `db_json_init()` **必须有家**，否则链接不过。

  ⇒ **实现放在 `src/db_json.c`**（与 `db_vec.c` / `db_ft.c` 同级，
  公开 API 的实现文件都住在 `src/` 顶层）：

  | 函数 | 落点 | 说明 |
  |---|---|---|
  | `db_json_encode` | `src/db_json.c` | 文本 → JSONB。**薄封装**，真正的编解码在 `modalities/doc/jsonb.c` |
  | `db_json_decode` | `src/db_json.c` | JSONB → 文本。同样薄封装 |
  | `db_json_match` | `src/db_json.c` | 匹配规格求值；**委托** `modalities/doc/json_match.c` |
  | `db_json_init` | `src/db_json.c` | 注册 `json_match` / `json_extract` 到值层函数表（`val_func_register`） |

  ⚠️ **`db_json.c` 要进 `MM_FILES_doc`**（W-B B1.1 的裁剪清单，
  W-C C6.1 会同步）—— 否则 `MM=vector` 时 `db_json_*` 符号还在、
  `db_json_init()` 也会被调用 → 注册了不存在的函数。

  ⚠️ **`db_json_init()` 的调用点（`db.c`）必须被 W-B 的裁剪守卫覆盖** ——
  它在 `db.c` 的 `db_open` 里，而 W-B B3.3 的守卫是 `MM=vector` 等 profile 才加的。
  ⇒ 在 `db.c` 里写成：

  ```c
  #if DB_HAS_DOC
      db_json_init();
  #endif
  ```

⚠️ **（可执行性修正）`#if DB_HAS_DOC` 在本步执行时**该宏还不存在** —— 这是个死锁**：

- `DB_HAS_DOC` 由 W-B **B2.3 的 `MM_FLAGS`** 引入（`-DDB_HAS_DOC=1`）；
- 而 B2.3 在 **W-B P1** 里；设计 §9 的顺序把 P1 排在 **C-1 ~ C-5 之后**；
- ⇒ 执行 C5.14 时宏未定义 ⇒ **`#if DB_HAS_DOC` 恒假** ⇒ `db_json_init()` **永不调用**
  ⇒ 值层函数表永远注册不上 ⇒ `json_match` 永远返回 `E_NOTSUP` ⇒ C5.9 / C5.10 的验收全绿不了。

⇒ **本计划定死：C5.14 无条件调用，不写 `#if`**：

```c
/* db.c 的 db_open() 里 */
db_json_init();
```

理由与后续：
1. C-5 阶段还没有裁剪机制，「无条件调用」是**唯一能编过又能跑通**的写法；
2. **裁剪守卫交给 W-B 去做**：W-B **B3.3** 的裁剪触点清单里**必须包含**
   `db.c` 里这一行 `db_json_init();` —— 到时候由 B3.3 就地包上 `#if DB_HAS_DOC`；
3. 本步的**执行记录必须写下这句话**：「`db.c` 的 `db_json_init()` 是 W-B B3.3 的裁剪触点之一，勿漏」。

⇒ **本步与 W-B B3.3 的接口就这一句话**，不需要等 P1 先落地。

- [ ] **C5.15 文档模态的 REPL 族：`src/repl_json.c` + `DOC` 动词族**（S8）

  ⚠️ **（复审修正）原计划完全没有 S8** —— 设计的「四件套」是
  「SPI 适配器 + **公开 API** + **REPL 族** + 模态文档」，
  文档模态当时只有前两件，**REPL 族整个缺失**。

  ⇒ **新建** `engine/db/src/repl_json.c` / `engine/db/src/repl_json.h`，
  提供 `DOC` 动词族：

  | 动词 | 作用 |
  |---|---|
  | `DOC ENCODE <json>` | 文本 → JSONB，打印 hex |
  | `DOC DECODE <hex>` | JSONB → 文本 |
  | `DOC MATCH <json> <spec>` | 匹配规格求值，打印 true/false |
  | `DOC PATH <json> <path>` | 取路径值（调试用） |

⚠️ **（可执行性修正）「`repl_xxx_init(...)` 注册动词」这个形状不存在** ——
实测全树**没有任何 `repl_*_init` 函数**，也**没有**「注册动词」这套机制。
真实形状是**最简单的那种**：`repl_xxx.c` 里定义一个 `int handle_xxx(DB *db, char *line)`，
`main.c` 里直接调用它。

```c
/* engine/db/src/repl_vec.c:26 */
int handle_vec(DB *db, char *line)

/* engine/db/src/repl_ft.c:48 */
int handle_ft(DB *db, char *line)

/* engine/db/src/main.c:593-629 的派发 */
if (strncmp(line, "VEC", 3) == 0)
    handle_vec(db, line);
else if (strncmp(line, "FT", 2) == 0)
    handle_ft(db, line);
```

⇒ `repl_json.c` 照这个形状写，**不要发明注册机制**：

**`engine/db/src/repl_json.h`**

```c
#ifndef MMDB_REPL_JSON_H
#define MMDB_REPL_JSON_H

#include "db.h"

/* DOC 动词族的入口。line 是整行输入（已含 "DOC" 前缀）。 */
int handle_json(DB *db, char *line);

#endif /* MMDB_REPL_JSON_H */
```

**`engine/db/src/repl_json.c`**

```c
#include "repl_json.h"
#include "db.h"
#include <stdio.h>
#include <string.h>

/*
 * DOC 动词族。
 *
 * 【为什么动词分派写在这里而不是 main.c】
 * main.c 只按首词（DOC）分派到本文件；DOC 之下再有几个动词
 * 是文档模态自己的事 —— 这样别处加模态时 main.c 不用改。
 */
static int doc_encode(const char *text)
{
    u8  *buf = NULL;
    u16  len = 0;
    db_err rc = db_json_encode(text, &buf, &len);
    if (rc != OK) {
        printf("DOC ENCODE: rc=%d\n", (int)rc);
        return 1;
    }
    for (u16 i = 0; i < len; i++)
        printf("%02x", buf[i]);
    printf("\n");
    free(buf);
    return 0;
}

static int doc_decode(const char *hex)
{
    /* 先把 hex 解成字节，再调 db_json_decode */
    size_t n = strlen(hex) / 2;
    u8 *raw = (u8 *)malloc(n ? n : 1);
    if (!raw)
        return 1;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) { free(raw); return 1; }
        raw[i] = (u8)v;
    }
    char *text = NULL;
    db_err rc = db_json_decode(raw, (u16)n, &text);
    free(raw);
    if (rc != OK) { printf("DOC DECODE: rc=%d\n", (int)rc); return 1; }
    printf("%s\n", text);
    free(text);
    return 0;
}

static int doc_match(const char *json, const char *spec)
{
    bool m = false;
    db_err rc = db_json_match(json, spec, &m);
    if (rc != OK) { printf("DOC MATCH: rc=%d\n", (int)rc); return 1; }
    printf("%s\n", m ? "true" : "false");
    return 0;
}

int handle_json(DB *db, char *line)
{
    (void)db;
    /* 跳过 "DOC" 这 3 个字符 */
    const char *p = line + 3;
    while (*p == ' ' || *p == '\t')
        p++;

    if (strncmp(p, "ENCODE", 6) == 0)
        return doc_encode(p + 6);
    if (strncmp(p, "DECODE", 6) == 0)
        return doc_decode(p + 6);
    if (strncmp(p, "MATCH", 5) == 0) {
        /* MATCH <json> <spec>：按第一个空格切 */
        ...
    }
    printf("用法: DOC ENCODE <json> | DOC DECODE <hex> | DOC MATCH <json> <spec> | DOC PATH <json> <path>\n");
    return 1;
}
```

⚠️ `DOC PATH` 要不要在本步做，取决于 `db_json_*` 是否已经有取路径的公开函数 ——
C5.14 只定义了 `encode` / `decode` / `match` 三个。
⇒ **`DOC PATH` 要么本步补一个 `db_json_extract()` 公开函数，要么从动词表里去掉**。
本计划选**去掉**（`PATH` 只是调试用，等真有需要再加），并在执行记录里写明。

  ⚠️ **接线点有两处**：
  1. `main.c` 的 `#include "repl_json.h"`（在 L62-65 那一组 `repl_*.h` 里）
     与派发（`main.c:593-629` 那一组 `handle_xxx` 调用）
2. ⚠️ **（复审修正）此时 `tools/mm_profiles.mk` 还不存在**（P1 的 B1 在后面才建它）——
   在本步的执行记录里写下：**P1 建 `mm_profiles.mk` 时，`MM_FILES_doc` 必须含 `src/repl_json.c` 与 `src/db_json.c`**。

  ⚠️ **与 W-B B3.6 的 `#if` 守卫的关系**：W-B B3.6 是在 `main.c` 里
  就地包 `#if` 住各模态的 include 与派发。文档模态的 REPL 也要被守卫住
  （`MM=vector` 时不该有 `DOC` 动词）。
  ⇒ **本步的派发代码写成 `#if DB_HAS_DOC` 包裹**，
  W-B B3.6 只需把 `repl_json.h` 的 include 与 `handle_json` 的调用一并纳入。

  ⚠️ **`repl_json.c` 不属于任何 `MM_FILES_*` 的通配**（它在 `src/` 顶层，
  不在 `src/modalities/doc/` 下）—— 必须**显式**加进 `MM_FILES_doc`。

- [ ] **C5.16 提交（C5 整段一个原子提交）**

  ```powershell
  cd C:\code\projects
# ⚠️ （可执行性修正）不要用 `git add engine/db/` —— 会卷进上面那 4 个无关的 ft 改动。
# 逐路径显式 add：
git add engine/db/src/db_json.c engine/db/src/db_json.h `
        engine/db/src/repl_json.c engine/db/src/repl_json.h `
        engine/db/src/modalities/doc `
        engine/db/src/query engine/db/src/storage engine/db/src/catalog `
        engine/db/src/db.c engine/db/src/db_internal.h engine/db/src/main.c `
        engine/db/include/db.h `
        engine/db/test/unit/test_json_match.c `
        engine/db/test/integration/test_json_sql.c `
        engine/db/test/integration/test_json_index_equiv.c `
        engine/db/docs/MODALITIES.md engine/db/Makefile

# 提交前确认没卷进 ft（输出必须为空）：
git diff --cached --name-only | Select-String -Pattern 'db_ft|ft_index|ft_reclaim'
  git commit -m "feat(kernel): 文档模态接进 SQL（JSON 列 + 路径索引 + json_match）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task C6：门禁 + 8 个 profile + 8 个 smoke 重跑

**Files**
- 核对：`engine/db/tools/mm_profiles.mk`（W-B 的 B1 已按最终归属一次建对；本步只**核对/校准**，并补上 C-4/C-5 新增的 `src/db_json.c` / `src/repl_json.c`）
- 修改：`engine/db/tools/check_deps.sh`（加"模态不许直接依赖 L1 之外的东西"之类的规则，若需要）
- 新建：`engine/db/test/smoke/smoke_doc.c`（W-B 已建的话改造它）

**Interfaces**
- `MM_FILES_blob` 的文件归属要更新（blob 搬家了）。
- `MM_FILES_relational` 是新的（关系模态的适配器搬到了 `modalities/relational/`）。

- [ ] **C6.1 校准 `tools/mm_profiles.mk`（不是「核对」—— 是补全）**

⚠️ **（可执行性修正）口径定死：B1 建骨架，C6.1 校准补全。**

原文写「B1 已一次建对，本步只核对」是**错的**，而且与本计划 C5.14 / C5.15
「此时 `mm_profiles.mk` 还不存在」**自相矛盾**。实际情形是：

- W-B 的 **B1** 按设计 §9 排在 C-1~C-5 **之后**，那时 `src/db_json.c` / `src/repl_json.c` /
  `src/repl_blob.c` / `src/repl_graph.c` 都已存在；
- 但 B1 的清单是照 `src/modalities/<m>/` 通配写的，**顶层新文件它不会自动包含**；
- ⇒ B1 建出来是**骨架**，`src/db_json.c` / `src/repl_json.c` 这些**必须由 C6.1 补进去**。

⚠️ **而且原文的片段里根本没有 `MM_FILES_doc`** —— 文档模态是 C-5 的主角，
它的裁剪清单在这步必须写出来（否则 `MM=vector` 时 `db_json_*` 符号还在）。

```make
# blob 的实现搬进了 modalities/blob/（C-3）；REPL 也归位了（C-4）
MM_FILES_blob := $(wildcard src/modalities/blob/*.c) src/db_blob.c src/repl_blob.c

# graph 的 REPL 归位了（C-4）
MM_FILES_graph := $(wildcard src/modalities/graph/*.c) src/db_graph.c \
                  src/db_graph_walk.c src/repl_graph.c

# doc 的公开 API 与 REPL 都在顶层（C-5 新建的），必须显式列出
MM_FILES_doc := $(wildcard src/modalities/doc/*.c) \
                src/db_json.c \
                src/repl_json.c

# 关系模态的 SPI 适配器搬进了 modalities/relational/（C-2）
# 关系模态是**恒常驻**的底座型模态，所以这行只用于文档，
# 不参与裁剪（裁剪的是"可变模态"）。
MM_FILES_relational := $(wildcard src/modalities/relational/*.c) \
                       $(wildcard src/storage/heap*.c) \
                       src/storage/record.c src/storage/value.c
```

⚠️ **`MM_FILES_relational` 不参与 `MM_CROP_SRCS`** —— 关系模态恒常驻，
所以 `MM_CROPPABLE` 里没有它。✓

⚠️ **`repl_blob.c` / `repl_graph.c` / `repl_json.c` 必须跟着搬家一起进裁剪清单** ——
它们是模态的实现，不裁的话 `MM=vector` 的二进制里会**残留这些模态的 REPL 符号**，
链接期证明就作废了。

**本步的硬判据（三条，缺一不可）**：

1. `MM_FILES_doc` 含 `src/db_json.c` 与 `src/repl_json.c`；
2. `MM_FILES_blob` 含 `src/repl_blob.c`，`MM_FILES_graph` 含 `src/repl_graph.c`；
3. 每个 `MM_FILES_<m>` 里列出的文件**全部** `Test-Path` 为真（没有列了不存在的文件 ——
   那会让 `make` 直接报 `No rule to make target`）。

```powershell
cd C:\code\projects\engine\db
# 逐模态核对"列出的文件都存在"
foreach ($m in @('blob','graph','doc','ft','geo','ts','vector')) {
    $files = (& mingw32-make -f tools/mm_profiles.mk --eval "print:" --eval "\t@echo \$(MM_FILES_$m)" print 2>$null)
    foreach ($f in ($files -split '\s+' | Where-Object { $_ })) {
        if (-not (Test-Path $f)) { Write-Output "!! $m 列了不存在的文件: $f" }
    }
}
Write-Output "核对完毕（无 !! 行 = 通过）"
```

- [ ] **C6.2 9 个 profile 全部编译**

  ```powershell
  cd C:\code\projects\engine\db
  foreach ($m in 'full','relational','blob','graph','vector','ft','geo','ts','doc') {
      mingw32-make MM=$m clean
      mingw32-make MM=$m all 2>&1 | Select-String -Pattern 'error|warning' | Measure-Object |
          ForEach-Object { "$m : 问题数 $($_.Count)" }
  }
  ```

  预期：全部 `0`。

- [ ] **C6.3 8 个 smoke 全绿**

  ```powershell
  mingw32-make clean && mingw32-make all
  mingw32-make smoke
  foreach ($m in 'relational','blob','graph','vector','ft','geo','ts','doc') {
      & ".\build\smoke_$m.exe"
  }
  ```

  预期：8 个 `OK`。

  ⚠️ **`smoke_doc.c` 现在可以用上公开 API 了**：
  W-B 里它只能调内部的 `json_parse` / `jsonb_encode`；
  C-5 之后改成调 `db_json_encode` / `db_json_decode` / `db_json_match`。

- [ ] **C6.4 门禁加固：SPI 层的 purity**

  `tools/check_deps.sh` 加一条：

  ```sh
  # SPI 层不许出现任何模态的东西 ——
  # SPI 是"契约层"，它的实现只能通过 SPI 契约暴露出去。
  # （关系模态与 blob 的适配器已搬到 src/modalities/，这里是回归保护。）
  forbid src/spi heap_am btree_am blob_am
  ```

  ⚠️ `forbid` 是按 include 路径前缀匹配的。
  `src/spi/table_opener.c` 里 include 的是 `spi/registry.h`、`spi/storage_am.h` —— 不命中。
  ✓

- [ ] **C6.5 全量回归 + 提交**

  ```powershell
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  ```powershell
  cd C:\code\projects
# ⚠️ （可执行性修正）不要用 `git add engine/db/` —— 会卷进上面那 4 个无关的 ft 改动。
# 逐路径显式 add：
git add engine/db/src/db_json.c engine/db/src/db_json.h `
        engine/db/src/repl_json.c engine/db/src/repl_json.h `
        engine/db/src/modalities/doc `
        engine/db/src/query engine/db/src/storage engine/db/src/catalog `
        engine/db/src/db.c engine/db/src/db_internal.h engine/db/src/main.c `
        engine/db/include/db.h `
        engine/db/test/unit/test_json_match.c `
        engine/db/test/integration/test_json_sql.c `
        engine/db/test/integration/test_json_index_equiv.c `
        engine/db/docs/MODALITIES.md engine/db/Makefile

# 提交前确认没卷进 ft（输出必须为空）：
git diff --cached --name-only | Select-String -Pattern 'db_ft|ft_index|ft_reclaim'
  git commit -m "feat(kernel): 八模态对称化收尾（profile 归属更新 + SPI 层 purity 门禁）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task C7：文档收官 + 父仓指针

**Files**
- 修改：`engine/db/docs/MODALITIES.md`（八模态盘点表 + A7 执行记录 + v1.6 收官复盘）
- 新建：`engine/db/docs/MODALITIES-v1.6.md`（或追加到 MODALITIES.md）

- [ ] **C7.1 八模态盘点表**

  在 `engine/db/docs/MODALITIES.md` 补一节：

  ```markdown
  ## v1.6 收官 · 八模态盘点

  | 模态 | 目录 | 公开 API | SPI 适配 | 事务语义 | SQL 入口 | 独立部署形态 |
  |---|---|---|---|---|---|---|
  | 关系 | `modalities/relational/` + `storage/heap*` | SQL | `heap_am` / `btree_am` | MVCC | catalog 真表 | 恒驻底座 |
  | blob | `modalities/blob/` | `db_blob_*` | `blob_am` | 不可变对象 | `blob_frames` / `blob_objects` | `MM=blob` |
  | 图 | `modalities/graph/` | `db_graph_*` | `graph_am` | 锁 + WAL | `graph_nodes` / `graph_edges` | `MM=graph` |
  | 向量 | `modalities/vector/` | `db_vec_*` | `vector_am` / `hnsw_am` | 锁 + WAL | `vec_items` | `MM=vector` |
  | 全文 | `modalities/ft/` | `db_ft_*` | `ft_am` | 锁 + WAL | `ft_docs` | `MM=ft` |
  | 空间 | `modalities/geo/` | `db_geo_*` | `geo_am` | 锁 + WAL | `geo_points` | `MM=geo` |
  | 时序 | `modalities/ts/` | `db_ts_*` | `ts_am` | 锁 + WAL | `ts_readings` / `ts_partitions` | `MM=ts` |
  | 文档 | `modalities/doc/` | `db_json_*` | **无**（寄生） | 继承 heap | JSON 列 + 路径索引 + `json_match` | `MM=doc` |

  ### 贯穿线索

  - **不变文件 + 合并 ×4**：record 的变长列机制同时承载 TEXT / JSON；
    外部值机制同时承载 BLOB / 大 JSON；B+tree 同时承载普通索引与路径索引；
    谓词下推同时承载普通谓词与路径谓词。
  - **外部值 ×3**：blob 帧 → 向量 → 大 JSON。
  - **下推超集契约 ×7**：七种可写模态 + 路径索引，全部走"AM 给候选、执行器重判"。
  - **事务语义三足鼎立**：MVCC（关系 + 文档）/ 不可变对象（blob）/ 锁 + WAL（其余五个）。
  ```

- [ ] **C7.2 A7 执行记录**

  把 C5.13 的 `grep -i json src/query/` 逐行结果写进
  `engine/db/docs/MODALITIES.md` 的 A7 小节。

- [ ] **C7.3 提交 + 父仓指针**

  ```powershell
  cd C:\code\projects
  git add engine/db/docs/
  git commit -m "docs(kernel): 八模态收官复盘（盘点表 + A7 审计记录）"
  git push origin main 2>&1 | Out-String | Write-Output

  cd C:\code
  git add projects
  git commit -m "chore(submodule): 更新 projects 指针（八模态对称化）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

## 交付说明

1. **完成了什么** —— 第四个 SPI 契约、关系模态与 blob 归位、文档模态接进 SQL、八模态盘点。
2. **变更范围** —— `engine/db/src/`（新增 `spi/table_opener.*`、`modalities/relational/`、
   `modalities/blob/`、`modalities/doc/json_*.c`）、`engine/db/src/query/`、
   `engine/db/src/storage/`、`engine/db/include/db.h`、`engine/db/tools/`。
3. **验证方式** —— C1.8（执行器机械检查）、C1.9 / C2.4 / C3.4（构建 + 门禁 + 95 测试）、
   C5.9（A3 两路全等）、C5.10（A4 对账）、C5.13（A7 审计）、C6.2 / C6.3（9 profile + 8 smoke）。
4. **后续建议** —— 跨模态事务（v1.7+）；`json_match` 的多值索引（当前数组路径返回 NULL）；
   根目录越界文件仍未清理。

## Self-Review 记录

**Spec 覆盖**：§6.1「对称」的定义（先定义再补全）→ C1（第四个 SPI 契约）+ C2/C3（归位）；
§6.2 `TableOpener` → C1.1~C1.9；§6.3 结构对称化动作清单 → C2 / C3 / C4；
§6.4 文档模态功能补全（`PLAN-v1.6` 步骤 3~6、A1~A8）→ C5（含复审补入的
C5.3b「步骤 3 收尾」、C5.14「`db_json_*` 的实现落点」、C5.15「`DOC` 动词族」）；
§6.5 `MM` 开关与 smoke 覆盖 8 个模态 → C6（重跑 W-B 建好的 8 个 smoke）；
§6.6 执行顺序 → 本计划表头已改为**引用设计 §9 的唯一权威顺序块**；
§2.12 文档模态功能缺口 → C5 全覆盖；
§7 风险 → C1.9（改造前后 ctest 用例名集合**逐条**比对，不只看总数）、
C5.9（反向验证**两处**注入）、C5.13（A7 五处审计 + 新文件 < 800 行的验收命令）、
C5.1b（`IndexMeta` 落点，避免动 `IndexInfo` 句柄）、C1.2（`RelPriv` 尺寸断言）。

**类型一致性**：`TableOpener` / `TableOpenResult` / `SPI_OPENER_PRIV_MAX` 在 C1.1 定义、
C1.2 使用、C1.4 装进 `Db`，三处名字一致（常量从 `SPI_AM_CTX_MAX` 改名，
因为原名里的 "CTX" 在别处被当成「执行器的上下文缓冲」，一词两义）；
`RelPriv` 的三个字段 `Heap*` / `HeapAmCtx` / `StorageAM` 在 C1.2 的定义、
C1.2 的 `_Static_assert` 与 C1.4 的安装处三处一致（实测 8+184+152 = 344 字节 ≤ 512）；
`rel_open_by_id` 的签名在 C1.1（契约）与 C1.2（实现样例）**逐字一致**
（含 `cols_buf` / `idxs_buf` 两个「调用方给的缓冲」参数）；
`Db.txns` 保持 `TxnTable *`（`exec.h:16/43` 的原注释「指针，不是副本」），
C1.4 只加两个 opener 字段，一行都不动；
路径索引的三个新字段加在 `IndexMeta` 上，`IndexInfo`（**索引句柄**）不动 ——
七个使用者（`catalog.c:486/533`、`catalog_load.c:239`、`catalog_lock.c:282`、
`spi/heap_am.c:36/53/438/507`）不受影响。名字无漂移。

**比例**：本计划 1787 行 / 设计文档 1192 行 ≈ 1.5:1。
其中约 35% 是两轮复审的「（复审修正）」批注 —— 它们是**要照着做的指令**，
不是背景叙述，删掉会导致执行者踩坑。
**复审来源**：本计划经过两轮独立 subagent 复审 ——
① 规格覆盖复审（对照设计 §1~§9 逐条查落点）与 ② 跨计划一致性复审
（31 个交叉点、契约冲突、文件归属冲突、顺序矛盾、数字口径）。
两轮的 blocking 级发现**已全部并入本计划正文**，每处均以
「⚠️ **（复审修正）**」标出，并写明**为什么错**与**改成什么** ——
这样执行者遇到分歧时能就地判断，而不是回头查聊天记录。
