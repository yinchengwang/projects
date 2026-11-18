# 八模态对称化 —— 第四个 SPI 契约 + 关系模态归位 + blob 归位 + 文档模态进 SQL（工作流 W-C）

> 依据：`docs/superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md` §4.5 / §6.1 / §6.3
> 实施依据：`engine/db/docs/PLAN-v1.6.md`（文档模态的原始方案，本计划是它的落地切片）
> 落地日期：2026-11-18
>
> **执行顺序（硬约束）**：
> ```
> W-B P0（CI 门禁） → W-A 全部 → 【本计划 C-1】 → W-B P1 / P2
> ```
> C-1 **必须**先于 W-B P1 —— 两者都改 `db_internal.h` / `db.c` / `db_instance.c`，
> 顺序反了必然冲突。
> C-2 / C-3 / C-4 / C-5 与 W-B P1 无文件重叠，**但 C-2 / C-3 改动了模态的文件归属**，
> 所以必须在 W-B P1 **之后**同步 `tools/mm_profiles.mk`（见 C-6）。

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
| R1 | 执行器真的不认识模态了吗 | `grep -rniE 'heap\|relational\|blob\|graph\|vector\|json' src/query/exec*.c` 只命中注释；`exec_internal.h` 不再 include `spi/heap_am.h` |
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
| `engine/db/tools/mm_profiles.mk` | 随 C-2 / C-3 的搬家更新文件归属 |

---

### Task C1：第四个 SPI 契约 `TableOpener` + 执行器脱钩

**Files**
- 新建：`engine/db/src/spi/table_opener.h`、`engine/db/src/spi/table_opener.c`、
  `engine/db/src/modalities/relational/rel_opener.h`、`engine/db/src/modalities/relational/rel_opener.c`
- 修改：`engine/db/src/query/exec.h`、`engine/db/src/query/exec_internal.h`、
  `engine/db/src/query/exec_scan.c`、`engine/db/src/db_internal.h`、`engine/db/src/db.c`

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
  #define SPI_AM_CTX_MAX 256

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

  struct TableOpener {
      const char *kind;          /* "catalog" / "virtual" —— 仅诊断用 */

      /* 按表名打开。表不存在返回 E_NOTFOUND。 */
      int (*open)(const TableOpener *op, void *owner, const char *name,
                  TableOpenResult *out, void *priv, size_t priv_bytes);

      /* 按表 id 打开（只有 catalog opener 支持；虚拟表返回 E_NOTSUP）。 */
      int (*open_by_id)(const TableOpener *op, void *owner, u32 tid,
                        TableOpenResult *out, void *priv, size_t priv_bytes);

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

  #include "modalities/relational/heap_am.h"
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
  _Static_assert(sizeof(RelPriv) <= SPI_AM_CTX_MAX,
                 "关系模态的私有状态超出 SPI_AM_CTX_MAX");

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

  static int rel_open_by_id(const TableOpener *op, void *owner, u32 tid,
                            TableOpenResult *out, void *priv, size_t priv_bytes)
  {
      (void)op;
      Catalog *cat = (Catalog *)owner;
      RelPriv *rp = (RelPriv *)priv;

      if (priv_bytes < sizeof(RelPriv))
          return E_INVAL;

      Heap *heap = catalog_heap(cat, tid);
      if (!heap)
          return E_CORRUPT;

      const ColDesc *cols = NULL;
      int ncols = 0;
      int rc = catalog_table_cols(cat, tid, &cols, ncols);   /* 按实际签名改 */
      ...
  }
  ```

  ⚠️ **`catalog_table_cols` 的真实签名要照抄现有代码**：
  现有 `resolve_table` 调的是 `catalog_table_cols(db->cat, tid, tc->cols_buf, SQL_MAX_COLS, &ncols)` ——
  它**写进调用方给的数组**，不返回指针。

  ⇒ **因此 opener 不能只返回指针**，得把列定义也拷进 `TableOpenResult` 背后的缓冲。
  这就要求 `TableOpenResult` 里的 `cols` 指向**调用方的缓冲区**。

  ⇒ 修正契约：`TableOpenResult` 里的 `cols` / `idxs` 指针由**打开器**填，
  但指向哪里由**调用方**决定 —— 所以契约里要加两个"调用方给的缓冲"参数：

  ```c
  struct TableOpener {
      const char *kind;

      /*
       * 打开一张表。
       *
       * owner      : 打开器的宿主（catalog / SpiRegistry / ...）
       * name       : 表名
       * out        : 产物（调用方持有）
       * cols_buf   : 调用方给的列定义缓冲（至少 SQL_MAX_COLS 项 ColDesc）
       * idxs_buf   : 调用方给的索引列表缓冲（至少 SQL_MAX_COLS 项 IndexInfo）
       * priv       : 调用方给的私有状态缓冲
       * priv_bytes : priv 的字节数
       *
       * out->cols / out->idxs 会指向 cols_buf / idxs_buf（不是打开器自己的内存）——
       * 这样调用方的"自持副本"纪律（v1.2 步骤 1）继续成立。
       */
      int (*open)(const TableOpener *op, void *owner, const char *name,
                  TableOpenResult *out,
                  ColDesc *cols_buf, IndexInfo *idxs_buf,
                  void *priv, size_t priv_bytes);

      int (*open_by_id)(const TableOpener *op, void *owner, u32 tid,
                        TableOpenResult *out,
                        ColDesc *cols_buf, IndexInfo *idxs_buf,
                        void *priv, size_t priv_bytes);

      size_t priv_bytes;
  };
  ```

  ⚠️ **`SQL_MAX_COLS` 是 32，不是 16**（`query/parser.h:36`）。
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
  但方向不对）。**解决**：把 `SQL_MAX_COLS` 挪到 `spi/table_opener.h` 或
  `common/types.h`，`query/parser.h` 改为 include 它。
  ⚠️ 这会牵动 `query/parser.h` 与 `exec_internal.h` 的 include ——
  **改完跑一遍分层门禁**（`check_deps.sh` 里 `forbid src/spi ... query/` **不存在**，
  所以 spi→query 现在不违规；但方向上是反的，仍然要挪）。

- [ ] **C1.4 `src/query/exec.h`：`Db` 加两个 opener**

  ```c
  #include "spi/table_opener.h"

  struct Db {
      Pager    *pager;
      Catalog  *cat;
      TxnTable  txns;
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
      unsigned char    priv[SPI_AM_CTX_MAX];
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

- [ ] **C1.7 `src/db.c`：安装两个 opener**

  在 `db_open` 里给连接装上：

  ```c
  conn->db.opener_catalog = &rel_table_opener;
  conn->db.opener_virtual = &spi_table_opener;
  ```

  ⚠️ `db.c` 要 include `modalities/relational/rel_opener.h` 与 `spi/table_opener.h`。

- [ ] **C1.8 机械验收：执行器不认识模态了**

  ```powershell
  cd C:\code\projects\engine\db
  Select-String -Path src/query/exec*.c,src/query/exec_internal.h `
    -Pattern 'heap|Heap|relational|blob_am'
  ```

  预期：**零命中**（连注释都不该有）。

  ```powershell
  Select-String -Path src/query/exec*.c,src/query/exec_internal.h -Pattern 'include "spi/heap_am.h"'
  ```

  预期：**零命中**。

- [ ] **C1.9 构建 + 全量测试**

  ```powershell
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：零警告；`✓ 依赖方向正确`；**95 个测试全绿**。

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
  Select-String -Path engine/db/src/**/*.c,engine/db/src/**/*.h `
    -Pattern 'spi/heap_am|spi/btree_am'
  ```

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
  git add engine/db/src/
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
  Select-String -Path engine/db/src/**/*.c,engine/db/src/**/*.h `
    -Pattern 'storage/blob|spi/blob_am'
  ```

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
  git add engine/db/src/
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
  W-B 的 `tools/mm_profiles.mk` 里，`MM_FILES_blob` 要加 `src/repl_blob.c`，
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
执行器里允许出现的落点**只有两处** ——
1. **通用值层**（`src/storage/value.{h,c}`）：值变体 + 函数表
2. **求值入口**（`src/query/exec_eval.c` 的 `eval_expr`）：`EX_FUNC` 的分发

**不允许**：任何 `if (strcmp(col_type, "json"))` 式的模态特判。

- [ ] **C5.1 先写 A7 审计清单（写进 `engine/db/docs/MODALITIES.md`）**

  新增一节：

  ```markdown
  ## v1.6 · 执行器改动审计清单（A7）

  文档模态接进 SQL 时，执行器**只允许**在下面两处出现与文档有关的代码：

  | # | 落点 | 文件 | 允许的内容 | 禁止的内容 |
  |---|---|---|---|---|
  | 1 | 通用值层 | `src/storage/value.h` / `value.c` | `T_JSON` 值变体；通用函数表（`val_func_register` / `val_func_find`）；函数签名为 `int (*)(DbValue *args, int nargs, DbValue *out)` | 出现 "json" 字样以外的模态名；按列类型分支的求值逻辑 |
  | 2 | 求值入口 | `src/query/exec_eval.c::eval_expr` | `EX_FUNC` 的一次函数表查表 + 调用 | 任何 `strcmp(..., "json")`；任何路径解析；任何索引判断 |

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
  但 `value.h` 里的 `DbValue.type` 用的是 `ValType`。

  ⚠️ **先查清楚 `ValType` 与 `ColType` 是两套还是一套**：
  `record.h` 定义 `ColType {T_TEXT=4, T_REAL=5, T_JSON=6}`，
  `value.h` 定义 `DbValue.type`（字段名也是 `type`）。
  如果是两套枚举，需要在值层加 `T_JSON` 并写清映射。

  JSON 值的表示：
  - 小文档：内联进 `DbValue.s[256]`（`VAL_TEXT_MAX` = 256）—— **装不下**。
    ⇒ JSONB 必须走**外部值**（`ext_key` / `ext_fn` / `ext_ctx` 那套既有机制）。
  - 大文档：外部值（PLAN-v1.6 判决 3）。

  ⚠️ **`DbValue.s` 只有 256 字节** ⇒ JSONB 一律走外部值，
  除非 JSONB ≤ 256 字节且**解码后**仍能装下。
  ⇒ **决定**：JSONB 在 `DbValue` 里**一律用外部值**表示
  （`is_external = true`，`ext_fn` = 读页链）。
  这样 `DbValue` 的结构一个字都不用改 —— 只多一个 `ext_fn` 的实现。

  ```c
  /*
   * 外部 JSONB 的读函数（签名照抄 storage/value.h 里 DetoastFn 的定义）。
   *
   * 【它读什么】
   * 一条 JSONB 字节流。来源有两种：
   *   - heap 行内联（小文档）：ext_key = 行内偏移
   *   - 外置页链（大文档）：ext_key = 首页号
   *
   * 【为什么不改 DbValue 的结构】
   * DbValue.s 只有 256 字节。JSONB 随便一个文档都超。
   * 外部值机制就是为这个准备的（v0.9 设计 foresight）——
   * 契约一行不改，换个 ext_fn 即可。
   */
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

  ⚠️ **`T_JSON` 定义在 `storage/record.h`**（`ColType`），而 `parser.h` 用的是 `ValType`。
  **先确认两套枚举的对应关系**（C5.3 已经查过一次），别猜。

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

  `src/query/exec_eval.c` 的 `eval_expr`（L42）加一个 case：

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

  ⚠️ **`fn_json_match` 的第一个实参是 JSONB** ——
  它可能是外部值（`ext_fn`），要 `val_detoast` 出来再解析。
  `val_detoast` 已有（`storage/value.h`）。

- [ ] **C5.8 索引：路径索引（写时维护）**

  **这是 C-5 里最大的一块。** PLAN-v1.6 判决 2 已把路线定死：
  **写时同步维护**（文档更新 = 整替换 → 删旧键、插新键），不是"快照 + 兜底"。

  需要做的：

  1. **索引定义**：catalog 里多存"这个索引是路径索引，路径是 `$.a.b`"。
     现在 `IndexInfo` 只存列号。加一个字段：
     ```c
     typedef struct {
         u16  col;          /* 普通索引的列号 */
         bool is_path;      /* true = 路径索引 */
         u8   path_len;
         char path[64];     /* 路径文本（is_path 时有效） */
     } IndexInfo;
     ```
     ⚠️ **`IndexInfo` 加字段会影响 `exec_internal.h` 的 `idxs_buf[SQL_MAX_COLS]`** ——
     内存变大但没关系（32 项 × ~72 字节 = 2.3 KB，在栈上，可接受）。
     ⚠️ **catalog 的索引元数据要能存下这个字段** —— 落盘格式要加一段。
     这是**有代价的**（要改 catalog 的序列化）。

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

- [ ] **C5.9 A3 验收：两路全等**

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

  **逐行**核对：命中必须全部落在 A7 清单的"落点 1"（值层）与"落点 2"（求值入口）里。
  写进 `engine/db/docs/MODALITIES.md` 的执行记录。

  ```powershell
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：零警告；`✓ 依赖方向正确`；**全部测试绿**（95 + 新增）。

- [ ] **C5.14 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/
  git commit -m "feat(kernel): 文档模态接进 SQL（JSON 列 + 路径索引 + json_match）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task C6：门禁 + 8 个 profile + 8 个 smoke 重跑

**Files**
- 修改：`engine/db/tools/mm_profiles.mk`（随 C-2 / C-3 的搬家更新）
- 修改：`engine/db/tools/check_deps.sh`（加"模态不许直接依赖 L1 之外的东西"之类的规则，若需要）
- 新建：`engine/db/test/smoke/smoke_doc.c`（W-B 已建的话改造它）

**Interfaces**
- `MM_FILES_blob` 的文件归属要更新（blob 搬家了）。
- `MM_FILES_relational` 是新的（关系模态的适配器搬到了 `modalities/relational/`）。

- [ ] **C6.1 更新 `tools/mm_profiles.mk`**

  ```make
  # blob 的实现搬进了 modalities/blob/（C-3）；REPL 也归位了（C-4）
  MM_FILES_blob := $(wildcard src/modalities/blob/*.c) src/db_blob.c src/repl_blob.c

  # graph 的 REPL 归位了（C-4）
  MM_FILES_graph := $(wildcard src/modalities/graph/*.c) src/db_graph.c \
                    src/db_graph_walk.c src/repl_graph.c

  # 关系模态的 SPI 适配器搬进了 modalities/relational/（C-2）
  # 关系模态是**恒常驻**的底座型模态，所以这行只用于文档，
  # 不参与裁剪（裁剪的是"可变模态"）。
  MM_FILES_relational := $(wildcard src/modalities/relational/*.c) \
                         $(wildcard src/storage/heap*.c) \
                         src/storage/record.c src/storage/value.c
  ```

  ⚠️ **`MM_FILES_relational` 不参与 `MM_CROP_SRCS`** —— 关系模态恒常驻，
  所以 `MM_CROPPABLE` 里没有它。✓

  ⚠️ **`repl_blob.c` / `repl_graph.c` 必须跟着搬家一起进裁剪清单** ——
  它们是模态的实现，不裁的话 `MM=vector` 的二进制里会**残留 blob / graph 的 REPL 符号**，
  链接期证明就作废了。

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
  git add engine/db/
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
