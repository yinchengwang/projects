# v0.9 执行方案与计划 · SPI 定稿与接线（blob 进 SQL）

> 状态：方案已定，执行计划已排。执行记录见 §3。
> 前置：v0.8（blob 模态）已完成，126 项验收全绿。

---

## 一、方案

### 1.0 这一版真正要证明什么

DESIGN.md §7 的原话：

> **模态放最后**，因为它们是对前面所有抽象的检验。SPI 设计错了，v0.9 会立刻暴露，此时改动成本还小。

所以 v0.9 的**首要产出不是"blob 能被查"** —— 那只是副产品。
首要产出是**"SPI 的设计对不对"**，判据只有一条：

> 把 blob 接到 SQL 上，如果**执行器不需要为 blob 写任何特例代码**，SPI 就成立；
> 如果需要，当场补，然后记录补的过程。

这决定了本版的写法：**先定 SPI，再接 blob，最后回头看执行器里有没有 blob 的味道。**

### 1.1 范围

**做：**

1. 三个 SPI 的**定稿**（DESIGN.md §5.1/5.2/5.3 的草图 → 可编译的头文件）。
2. **Storage AM 的两个实现**：关系模态（heap）、blob 模态。
3. **Index AM 的一个实现**：B+tree。
4. **谓词下推**的两个实现：heap（下推给索引）、blob（下推 blob_id）。
5. **虚拟表机制**：模态可以向 SQL 提供表，SQL 层按名字查到它们。
6. **执行器改造**：SELECT / INSERT / UPDATE / DELETE 全部走 StorageAM。
7. **外部值**：大值不进 SQL 的标量值层，改用"外部引用 + 惰性物化"。

**不做（明确划掉）：**

| 项 | 为什么不做 |
|---|---|
| 全局 latch | 涉及全代码库共享状态，和 SPI 改造叠加会让调试分不清锅。**顺延到 v0.10** |
| 聚合函数 / GROUP BY | v0.9 不需要。blob 的对象级元信息用两张虚拟表表达，不用聚合 |
| 向量模态 / 图模态 | v0.10 / v1.0 |
| 关系模态的大值外置 | DESIGN.md §10.4 保留；本版的外部值机制正是它的前置 |
| 真正的列式执行 / CBO | v1.1 |

### 1.2 §5.1 的草图漏掉了五件事

DESIGN.md §5.1 的 Storage AM 草图是这样的：

```c
struct StorageAM {
    const char *name;
    int (*scan_begin)(...);  int (*scan_next)(...);  int (*scan_end)(...);
    int (*get)(...);
    int (*insert)(...);  int (*update)(...);  int (*remove)(...);
    void *impl;
};
```

它**能表达"扫描"和"读写"，但表达不了一次完整的查询**。逐条对照执行器实际需要的东西：

| # | 漏了什么 | 执行器为什么必须要 | 不补会怎样 |
|---|---|---|---|
| 1 | **schema 访问器** | 要把 `WHERE id = 3` 里的列名绑成列号；要把结果集的列名填对 | 绑定期没有列定义可对 |
| 2 | **解码回调** | 模态的字节串不是列式记录（blob 的帧就是 4KB 裸字节），执行器不能自己解 | 只能把字节串硬塞进 record 格式 —— 那就把所有模态逼成关系模态 |
| 3 | **可见性归属** | heap 的行有 xmin/xmax，要按快照判断；blob 的行没有版本，恒可见 | 只能由执行器统一判 —— 那执行器就得知道每种模态的版本语义 |
| 4 | **能力协商** | blob 是只读的（对象不可变）；B+tree 没有 KNN | 只能靠类型判断 —— 加第 9 个模态就要改执行器 |
| 5 | **写路径的 xid** | 草图里 `insert(am, ctx, tuple, out)` 没有 xid | 又要把 xid 藏进共享对象 —— **v0.7 的血泪教训** |

第 5 条尤其关键：v0.7 已经因为"xid 藏在共享对象里"付过一次代价
（连接 A 的写带上连接 B 的 xid → 未提交数据被判为已提交）。
**SPI 是所有模态写路径的入口，如果它不强制传 xid，这个错误会在每个模态里重犯。**

所以本版要做的不是"把草图抄成代码"，而是**把草图补成能用的契约**。

### 1.3 关键建模问题：blob 怎么变成"一行"

这是 v0.9 的第一个真问题。DESIGN.md §5.1 说：

> **关键约束**：`RID` 是所有模态的统一寻址方式。图的"节点 id"要映射成 `(page_id, slot)`——
> 定长记录天然满足（`slot = 记录序号`）。这条约束保证了不同模态的数据能被同一个执行器扫描。

但 blob 是**字节流**，它没有"行"。三个候选：

| 候选 | 描述 | 问题 |
|---|---|---|
| (a) 一行 = 一个对象 | 列：blob_id / nframes / frame_size / total_len | 只能看到元信息；对象内部的帧**无法被寻址**（RID 指不到具体内容） |
| (b) 一行 = 一帧 | 列：blob_id / frame_no / data_len / data_off / data | 帧是 blob 唯一的可寻址单位 ✓；但一帧 4076 字节，塞不进 SQL 的标量值（文本上限 255） |
| (c) 一行 = 一段变长字节 | 行大小由数据决定 | 行没有稳定身份 → **RID 表达不了**；SPI 的核心约束直接崩 |

**决定：(b) 一行 = 一帧。** 三条理由：

1. **帧是 blob 唯一的"可寻址单位"**。对象是帧的集合，RID 要能定位到最小可读单位，帧正好是。
2. **粒度对齐**。图模态的一行 = 一个节点，向量模态的一行 = 一个向量，
   blob 的一行 = 一帧 —— 三个模态的行粒度都是**该模态的最小可寻址单元**。
   这正是 SPI 要求的统一，而不是"都长得像关系表"。
3. **可组合**。帧行带着 blob_id，就能定位回对象；对象的元信息用聚合从帧上算出来。

(b) 的"帧太大塞不进值"这个障碍不是 bug，是**发现**——它逼出了 §1.4 的外部值机制。

#### RID 怎么定

```
blob_id      = 头页页号 + 1                （v0.8 已定）
base_page    = 头页页号 + 1 = blob_id       （页连续分配）
帧 i 的页号   = base_page + i = blob_id + i
```

于是：

```
blob_frames 的 RID = (page = blob_id + frame_no, slot = 0)
```

- **slot 恒为 0**：一帧独占一页，一页一行 —— 和 §5.1 说的"定长记录天然满足 slot = 记录序号"是同一回事。
- **唯一性**：页号全局唯一 ✓。
- **RID 空间是表内的事**：blob_objects 的 RID = (头页页号, 0)，和 blob_frames 不同空间，不冲突 ✓。

**反解（RID → 行）**：给一个 RID，要找回 (blob_id, frame_no)，得知道这个页属于哪个对象。
办法是遍历 blob 自己的对象目录，找帧页区间包含该页的对象。
代价 O(对象数) —— v0.9 接受，**记为已知限制**（将来的修法：blob 自己维护一张"页号 → 对象"的反查表）。
注意这又一次印证了 v0.8 的结论：**blob 不依赖 catalog**，反查用的是 blob 自己的目录。

#### 两张虚拟表，不是一张

| 表 | 一行 = | 列 |
|---|---|---|
| `blob_frames` | 一帧 | blob_id / frame_no / data_len / data_off / data |
| `blob_objects` | 一个对象 | blob_id / nframes / frame_size / total_len |

为什么两张：对象级元信息（帧数、帧大小、总长）放在帧表里是**每行冗余一遍**；
一张表要么冗余要么缺。两张表各自语义干净，还顺带证明了一件事：
**一个模态可以提供多张表**（注册表按名字索引，不是一对一）。

`data_off` 是这一帧数据在数据文件里的字节偏移 = `页号 * 4096 + 20`。
它就是业界说的**外部引用**（Parquet 的 column chunk offset、ClickHouse 的外部字符串引用都是这个形态）：
拿到 offset + length，任何人都能直接去文件里定位，不用问数据库。

### 1.4 外部值：大值不进值层

一帧 4076 字节；`DbValue` 的文本上限是 255 字节。三条路：

| 路 | 做法 | 判决 |
|---|---|---|
| 1. 截断 | 只留前 255 字节 | **否决**。SQL 返回的值和实际内容不符 = 静默的数据错误，比报错更坏 |
| 2. 加大值类型 | `DbValue.s` 改成 4KB | **否决**。每值 4KB、每行 16 列 = 64KB；关系模态的行本来就放不进一页。更根本的是：**值变大并不会让扫描变便宜** —— 扫描时把每帧内容读出来，1GB 对象就要读 1GB。而"只查帧号和长度"本该只读对象头 |
| 3. 外部值 | 值里存"句柄 + 取回方法"，内容**被需要时**才读 | **采纳** |

第 3 条是业界通例，不是我们的发明：

| 系统 | 做法 |
|---|---|
| PostgreSQL | TOAST：大值存进 TOAST 表，行里留 18 字节指针，读时 detoast |
| ClickHouse | 大字符串单独存成"外部表"，值是一个 16 字节引用 |
| Parquet / Lance | 列块单独存，行里是 offset + length；读某一列才去取那几页 |
| S3 / MinIO | 对象体在对象存储里，键 + 长度就是引用 |

所以 **blob 帧的内容不进 SQL 的值，而是以"外部引用"的形式挂在值上**：

```c
/* 取回函数：把外部值的内容读进 buf */
typedef int (*DetoastFn)(void *ctx, u64 key, u64 off, u8 *buf, u16 cap, u16 *len_out);

typedef struct {
    bool      is_null;
    u8        type;
    i64       i;
    u16       len;
    char      s[VAL_TEXT_MAX];
    /* 外部值：内容不在 s[] 里，由 ext_fn 按 (ext_key, ext_off) 取回 */
    bool      external;
    u64       ext_key;      /* 模态内部的对象标识 */
    u64       ext_off;      /* 模态内部的位置 */
    DetoastFn ext_fn;
    void     *ext_ctx;
} DbValue;
```

**三条纪律：**

1. **外部值只在输出路径物化**。过滤、投影的中间过程**不**物化。
   这正是"扫描 blob_frames 不读帧页"的实现方式 —— 不是靠优化器，是靠值的形态。
2. **比较时必须物化**（`val_cmp` 遇到外部值先物化再比）。
   要么对，要么报错，**不能"近似比较"**。给一个错的结果比给一个错还严重。
3. **物化失败要报错，不能给空**。空的 TEXT 和"内容是空串"是两回事。

**可测量的收益**：`blob_stats()->page_reads` 在 v0.8 里已经存在，正好用来量：

- `SELECT blob_id, frame_no FROM blob_frames` → page_reads **不变**（不读帧页）
- `SELECT data FROM blob_frames` → page_reads **按帧数增长**

### 1.5 三个 SPI 的定稿

#### Storage AM（`src/spi/storage_am.h`）

```c
typedef struct StorageAM StorageAM;
typedef struct AmScan    AmScan;

/* 能力位。能力协商靠返回值，不靠类型判断（DESIGN.md §5.2） */
#define AM_CAP_SCAN     0x01u
#define AM_CAP_INSERT   0x02u
#define AM_CAP_UPDATE   0x04u
#define AM_CAP_REMOVE   0x08u
#define AM_CAP_EXTERNAL 0x10u   /* 支持外部值 */
#define AM_CAP_PUSHDOWN 0x20u

/* 可见性上下文：调用方显式传，不存在 AM 里（硬规则 5） */
typedef struct {
    const Snapshot *snap;
    const TxnTable *txns;
} AmVisCtx;

struct StorageAM {
    const char *name;
    u32         caps;

    /* schema：模态自己的列定义 */
    int  (*columns)(StorageAM *am, const ColDesc **cols, int *ncols);

    /* 扫描 */
    int  (*scan_begin)(StorageAM *am, AmScan **out);
    int  (*scan_next) (StorageAM *am, AmScan *s, Rid *rid, u8 *buf, u16 cap, u16 *len);
    int  (*scan_end)  (StorageAM *am, AmScan *s);

    /* 按 RID 取一行 */
    int  (*get)(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len);

    /* 模态自己的字节串 → 值 */
    int  (*decode)(StorageAM *am, const u8 *buf, u16 len, DbValue *out);

    /* 这一行对当前快照可见吗 */
    bool (*visible)(StorageAM *am, const u8 *buf, u16 len, const AmVisCtx *vis);

    /* 写路径。xid 必须显式传 —— 硬规则 5 */
    int  (*insert)(StorageAM *am, u32 xid, const DbValue *vals, Rid *out);
    int  (*update)(StorageAM *am, u32 xid, Rid rid, const DbValue *vals, Rid *new_rid);
    int  (*remove)(StorageAM *am, u32 xid, Rid rid);

    void *impl;      /* 模态私有状态 */
};
```

**每个字段的"为什么"（这些是接口的说明书，不是注释）：**

| 字段 | 为什么必须这样 |
|---|---|
| `columns` | schema 是模态的**私有知识**。blob 的列定义跟 catalog 无关；图模态的"节点 schema"也不是关系表 |
| `scan_next` 返回**字节串**而不是值 | 值的形态由模态决定（blob 的一帧不是列式记录）。字节串 + `decode` 才是能同时容纳两种模态的最小公共表示 |
| `visible` 收 `AmVisCtx` | 可见性是**连接私有**的（快照）和**实例共享**的（clog）的组合。它必须由调用方显式传进来 —— 存在 AM 里就是共享状态 |
| `visible` 归 AM 而不是执行器 | heap 的行有 xmin/xmax，blob 的行没有版本。**每种模态的版本语义不同**，统一到执行器就等于让执行器知道所有模态 |
| `insert/update/remove` 带 `xid` | 见 §1.2 第 5 条 |
| `caps` | 只读的模态（blob）把写接口置 NULL 或返回 `E_NOTSUP`，执行器据此报"这个表不支持写" |

#### Index AM（`src/spi/index_am.h`）

沿用 §5.2 的草图，做两处调整：

1. **去掉 `create`**。索引的创建是 catalog 层的事（要写元数据），不是索引 AM 的事。
   Index AM 只负责"一个已经存在的索引"的读写与查询。
2. **写接口不带 xid**。因为**索引不写日志**（v0.6 的决定：索引崩溃后重建）。
   所以索引里没有"属于哪个事务"的概念，自然不需要 xid。
   ——这一条要写进注释，否则后人一定会以为漏了。

`knn` 保留：向量模态用；B+tree 返回 `E_NOTSUP`（§5.2 的先例）。

#### Pushdown（`src/spi/pushdown.h`）

```c
/* 谓词：列号 + 比较符 + 常量 */
typedef struct {
    int     col_idx;
    int     op;        /* '=' '<' '>' 'L'(<=) 'G'(>=) 'N'(<>) */
    DbValue val;
} Predicate;
```

**下推的结果是一个行源，和全表扫描同构：**

```c
int (*can_push)(StorageAM *am, const Predicate *p);
int (*push)   (StorageAM *am, const Predicate *p, AmScan **out);
```

这条设计有两个好处：

1. **执行器不需要为下推新增算子类型**。它只会问两种行源：全表扫、下推扫。
   两者都是 `(rid, bytes)`，处理路径完全一样。
   这和 §5.2 的"能力协商靠返回值"是同一个思路。
2. **下推不改变正确性**。执行器拿到行之后**仍然完整求值一遍 WHERE**。
   下推只是"少读一些行"，不是"替你判断"。
   于是下推实现错了**最多是慢，不会是错** —— 除非它漏行。
   所以下推的实现必须**保守**：宁可多给行，不能少给。

**两个实现：**

- **heap**：谓词落在某个有索引的列上 → 打开 B+tree 游标 → 回表取行。
  （和 v0.5 的索引扫描是同一件事，只是判断逻辑从执行器挪到了 AM。）
- **blob**：谓词是 `blob_id` 上的等值/范围 → 扫描时**只枚举符合条件的对象**。
  效果可测：扫全部对象要读 N 个对象头页，只扫一个对象只读 1 个。

### 1.6 虚拟表：模态怎么向 SQL 提供表

模态不能碰 catalog（硬规则 6：模态各自管自己的元数据）。
所以需要一个**注册表**，放在**实例级**（所有连接共享）：

```
SpiRegistry:  名字 → StorageAM*
```

解析 `FROM <名字>` 的顺序：

1. 先查 **catalog**（用户建的表优先 —— 用户显式建的东西应该赢）
2. 再查**注册表**（模态提供的虚拟表）
3. 都没有 → "表不存在"

注册表在 `db_open` 时建立，blob 的两张表在这里注册。
**注册表不知道 blob 是什么** —— 它只知道"名字 → 一个 StorageAM"，
所以第 9 个模态要接进来，只是多注册两条记录，执行器一行不改。

### 1.7 验收

**A. SPI 成立（这是本版的主验收）**

| # | 验收项 | 怎么验 |
|---|---|---|
| A1 | 执行器里**没有一行 blob 特判** | `grep -n blob src/query/exec*.c` 必须为空 |
| A2 | 加一个新模态不需要改执行器 | 代码审查 + 依赖方向检查通过 |
| A3 | 依赖方向正确 | `make deps` 通过 |

**B. blob 能被 SQL 扫**

| # | 验收项 | 怎么验 |
|---|---|---|
| B1 | `SELECT * FROM blob_frames` 返回帧行，列名/类型正确 | 集成测试逐列比对 |
| B2 | `SELECT * FROM blob_objects` 返回对象行 | 集成测试 |
| B3 | `SELECT data FROM blob_frames WHERE blob_id = N` 的字节和 blob API 读出来的一致 | 集成测试逐字节比对 |
| B4 | 对 blob 表 INSERT/UPDATE/DELETE → **明确报错**，不静默成功也不崩 | 集成测试 |

**C. 外部值真的惰性（这是 §1.4 的核心验收）**

| # | 验收项 | 怎么验 |
|---|---|---|
| C1 | 扫 blob_frames（不取 data 列）→ `blob_stats()->page_reads` **不变** | IO 量测试 |
| C2 | 取 data 列 → page_reads 按帧数增长 | IO 量测试 |
| C3 | 外部值参与比较 → 结果正确（物化后比） | 单元测试 |

**D. 能力协商与下推真的生效**

| # | 验收项 | 怎么验 |
|---|---|---|
| D1 | heap 的等值谓词走索引（不是全表扫） | 与 v0.5 同款判定：返回顺序 + 页读次数 |
| D2 | blob 的 `blob_id = N` 只读 N 那一个对象的头页 | 页读计数 |
| D3 | 下推漏行 → 测试必须变红（反向验证） | 注入 |

**E. 旧的不变**

| # | 验收项 | 怎么验 |
|---|---|---|
| E1 | v0.1–v0.8 的 126 项验收**全绿** | `make test` |

### 1.8 风险清单

| # | 风险 | 应对 |
|---|---|---|
| R1 | **执行器大改，回归面极大**（126 项旧验收） | 改造后先跑全量测试；出红就定位到"哪一步改造引入的" |
| R2 | 外部值让 `DbValue` 变大（+40 字节），到处按值传 | `DbValue` 现在 272 字节 → 312 字节；一行 16 列 = 5KB，可接受 |
| R3 | 外部值的生命周期（谁分配、谁释放） | **外部值不持有堆内存** —— 它只持句柄 + 函数指针；物化出来的缓冲由结果集自己管（`b_push` 里 free） |
| R4 | 值类型从 `query/` 下沉到 `storage/` 引起 include 变更 | 纯路径替换，编译器会全部报出来 |
| R5 | `exec.c` 已经 1183 行，超过"单文件 ≤ 800 行"的硬规则 | 改造后如果仍超线，拆成 `exec_scan.c` / `exec_write.c` |
| R6 | blob 的 RID 反解是 O(对象数) | v0.9 接受，记为已知限制 |
| R7 | 下推实现漏行 → 静默少数据 | **下推后仍然完整求值 WHERE**；且下推必须保守 |
| R8 | 反向验证时注入的代码编译不过 → 跑的是旧二进制 → 结论不可信 | 注入后**先确认编译通过**再跑（v0.7/v0.8 都踩过这个坑） |

---

## 二、执行计划

分九步，每步都有可验证的产出。**上一步不绿不做下一步。**

### 步骤 1 · 值类型下沉 + 外部值机制

1. `git mv src/query/value.{h,c} src/storage/value.{h,c}`（L1，值是存储层与查询层共用的表示）。
2. `git mv src/query/index_key.{h,c} src/index/index_key.{h,c}`（L2，索引键编码是索引层的事）。
3. 改所有 `#include "query/value.h"` → `"storage/value.h"`（编译器会全部报出来）。
4. `DbValue` 加外部值字段 + `DetoastFn`。
5. 新增 `val_external_text(v, key, off, fn, ctx)`、`val_detoast(v, &buf, &len)`（分配缓冲）。
6. `val_cmp` 遇外部值先物化；`val_to_str` 遇外部值先物化；`val_text` 保持原语义。
7. 检查依赖方向：`storage/` 不许依赖 `query/` ✓。

**验证**：编译通过；`make deps` 通过；旧的 value 相关单元测试全绿。

### 步骤 2 · SPI 三个头文件定稿

1. 写 `src/spi/storage_am.h`（§1.5 的定稿版 + 每字段的"为什么"）。
2. 写 `src/spi/index_am.h`。
3. 写 `src/spi/pushdown.h`。
4. 写 `src/spi/registry.{h,c}`（名字 → StorageAM*）。

**验证**：头文件自洽（能单独编译）；`make deps` 通过。

### 步骤 3 · heap AM（关系模态）

1. `src/spi/heap_am.c`：`StorageAM` 的六个读接口 + 三个写接口。
   - `columns` → 转发 catalog 的列定义
   - `scan_*` → 包 `heap_scan_*`
   - `get` → 包 `heap_get`
   - `decode` → `val_from_record`
   - `visible` → `mvcc_visible`
   - `insert/update/remove` → 编码 + 打版本号 + 维护索引
2. `src/spi/btree_am.c`：`IndexAM` 包 `BTree`；`knn` 返回 `E_NOTSUP`。
3. `src/spi/heap_am.c` 的 `can_push/push`：谓词落在有索引的列上 → 开 B+tree 游标。

**验证**：单元测试直接对 AM 测（不经过 SQL）—— 扫描、get、写、可见性、下推各一项。

### 步骤 4 · blob AM（blob 模态）

1. `src/spi/blob_am.c`：两张虚拟表。
   - `blob_objects`：一行 = 一个对象；RID = (头页页号, 0)
   - `blob_frames`：一行 = 一帧；RID = (blob_id + frame_no, 0)
2. `blob_frames` 的 `decode` 产出外部值（`ext_key = blob_id`，`ext_off = frame_no`）。
3. `blob_frames` 的 `can_push/push`：`blob_id` 上的等值/范围。
4. 写接口返回 `E_NOTSUP`（blob 只读）。
5. `visible` 恒真。

**验证**：单元测试直接对 AM 测。

### 步骤 5 · 执行器改造

1. `TableCtx` 换成 `{ StorageAM *am; void *am_ctx; const ColDesc *cols; int ncols; u32 tid; int is_virtual; }`。
2. `resolve_table`：先 catalog，后注册表；heap 表现场构造 heap AM。
3. `exec_select`：扫描源改为 `scan_begin` 或 `push`；可见性交给 `am->visible`；解码交给 `am->decode`。
4. `exec_insert/update/delete`：全部走 `am->insert/update/remove`（xid 显式传）。
5. 索引选择逻辑（`pick_index` / `leaf_indexable`）从执行器**删掉**，改成"把谓词翻译成 `Predicate`，问 AM"。
6. `exec_vacuum` / `exec_create_index` 对虚拟表报错。

**验证**：全量测试；`grep -n blob src/query/exec*.c` 必须为空（验收 A1）。

### 步骤 6 · 接线

1. `Db`（exec.h）加 `SpiRegistry *reg`。
2. `db_open` 建注册表 + 注册 blob 的两张表；`db_connect` 共享同一个注册表。
3. REPL：加 `BLOB SQL <sql>` 之类的演示命令（或者直接在 REPL 里能写 `SELECT ... FROM blob_frames`）。

**验证**：REPL 端到端跑一遍。

### 步骤 7 · 测试

1. 单元 `test/unit/test_spi.c`：SPI 契约（能力位、错误码、可见性归属）。
2. 单元 `test/unit/test_blob_am.c`：blob AM（两张表、RID、往返、外部值、下推）。
3. 集成 `test/integration/test_spi_it.c`：blob 进 SQL 的端到端 + IO 量 + 能力协商 + 回归。

**验证**：新增测试全绿 + 旧的 126 项全绿。

### 步骤 8 · 两轮反向验证

1. **验证一：把 heap 的下推改成"漏掉边界行"** → 下推类测试必须变红。
   （证伪：下推测试真的在测下推，不是碰巧对）
2. **验证二：把外部值改成"扫描时就物化"** → IO 量测试必须变红。
   （证伪：惰性测试真的在测惰性）
3. 每次注入后**先确认编译通过**（v0.7/v0.8 的坑）。

### 步骤 9 · 文档与记忆

1. `DESIGN.md`：§5 三个 SPI 定稿、§5.5 blob 的行模型、§5.6 外部值、§7 路线图状态、§9 模块对应。
2. `README.md`：硬规则、目录布局、当前状态、实现顺序。
3. `PLAN-v0.9.md` §3 执行记录。
4. workspace 记忆。

---

## 三、执行记录

### 3.1 最终状态

| 项 | 结果 |
|---|---|
| SPI 三接口 | **定稿**（`storage_am.h` / `index_am.h` / `pushdown.h`） |
| Storage AM 实现 | 2 个：关系模态（heap）、blob 模态（两张虚拟表） |
| Index AM 实现 | 1 个：B+tree（`knn` 返回 `E_NOTSUP`） |
| 谓词下推 | 2 个：heap（下推给索引）、blob（下推 blob_id） |
| 虚拟表机制 | 注册表（名字 → StorageAM），blob_frames / blob_objects 已接进 SQL |
| 执行器 | **模态无关**：`grep blob src/query/exec*.c` 只命中注释 |
| 外部值 | 已实现（句柄 + 惰性物化），IO 量可测 |
| 验收 | 21 个测试文件 / **134 项**，全绿；依赖方向正确；零错误零警告 |
| 源码 / 测试 | 13971 行 / 7061 行 |

**顺延**：全局 latch（v0.9 原计划做）→ v0.10。

### 3.2 方案开头的判断被证实了

方案里写：*「这一版的首要产出不是『blob 能被查』，而是『SPI 的设计对不对』」*。

结果正是如此 —— §5.1 的草图**漏了五件事**，每一件都在接线时暴露：

| 漏掉的 | 接线时撞到的问题 | 补法 |
|---|---|---|
| schema 访问器 | 执行器要绑列名、要填结果集列名，模态的列定义没人给 | `columns()` 回调 |
| 解码回调 | blob 的行是 16 字节裸数据，不是列式记录 | `decode()` 回调 + "行 = 字节串 + 模态自己解" |
| 可见性归属 | heap 的行有 xmin/xmax，blob 的行没有版本 | `visible(am, buf, len, &vis)` —— 归模态 |
| 能力协商 | blob 只读、B+tree 无 KNN | `caps` 位 + `E_NOTSUP` |
| 写路径的 xid | 草图里 `insert()` 没有 xid | 全部写接口显式带 `u32 xid` |

**最值得记的一条是 xid**：SPI 是所有模态写路径的入口。
草图里没带 xid，意味着每个模态都会自己想办法存 ——
而"存进共享对象"就是 v0.7 付过血泪代价的错误。
**接口层强制比每个模态自觉更可靠。**

### 3.3 三个关键设计

#### 帧才是行

方案 §1.3 纠结过：blob 是字节流，没有"行"，怎么映射到 RID 模型？

最后选了「一行 = 一帧」，三条理由：帧是 blob 唯一的可寻址单位；
粒度和其他模态对齐（图=节点，向量=向量，blob=帧）；
帧行带着 blob_id 就能定位回对象。

**实现出来的样子**：

```
blob_frames 的 RID = (页号 = base_page + frame_no, slot = 0)
blob_objects 的 RID = (头页页号 = blob_id - 1, slot = 0)
```

`slot` 恒为 0 —— 一帧独占一页。
这和 DESIGN.md §5.1 说的"定长记录天然满足 slot = 记录序号"是同一回事，
**设计早就写好了，只是当时没想到 blob 也是这样**。

**两张虚拟表而不是一张**：对象级元信息（帧数/帧大小/总长）
放在帧表里是每行冗余一遍。一张表要么冗余要么缺。
两张表各自干净，还顺带证明了"一个模态可以提供多张表"。

#### 外部值：塞不进去就换形态

一帧 4076 字节，值的文本上限 255。三条路里否掉了两条：

- **截断** —— SQL 返回的值和实际内容不符 = 静默的数据错误，比报错更坏
- **加大值类型** —— 值撑大了不会让扫描变便宜，只会让每个值都变贵

选第三条：**外部值**（句柄 + 取回函数）。

**关键不是"装得下"，是"惰性"**：
`SELECT blob_id, frame_no FROM blob_frames` 不读帧页，
`SELECT data FROM blob_frames` 才读。
这不是优化器做到的，是**值的形态**做到的 ——
外部值在过滤/投影的中间过程根本不物化。

三条纪律写进了 `storage/value.h`：
只在输出路径物化 / 比较时必须物化（不许近似比较）/ 物化失败要报错（不能给空）。

#### 虚拟表注册表

模态不能碰 catalog（硬规则 6）。那怎么让 `FROM blob_frames` 能跑？

答案是**注册表**：实例级，"名字 → StorageAM*"。

**注册表不知道 blob 是什么** —— 它只知道"名字 → 一个接口"。
所以第 9 个模态接进来，只是多注册两条记录，执行器一行不改。

解析顺序：**catalog 优先**。用户显式建出来的表应该赢 ——
否则"我建的表怎么查不到我的列"会很困惑。

### 3.4 执行器重写：搬走了什么

`exec.c` 从 1183 行改到模态无关，拆成三个文件：

| 文件 | 行数 | 内容 |
|---|---|---|
| `exec.c` | 181 | 入口 + 结果集构造 |
| `exec_scan.c` | 511 | 绑定 + 求值 + 表解析 + 下推 + SELECT |
| `exec_write.c` | 466 | INSERT / UPDATE / DELETE / VACUUM / CREATE |

从执行器里搬走的：索引维护、索引选择、版本号打标、MVCC 更新的两段式。
**搬走之后，执行器里没有一行"堆表"的知识**。

**索引选择从执行器下沉到了 AM**：
以前执行器自己判断"这个列有没有索引"（`pick_index`），
现在执行器只把表达式翻译成 `Predicate`，问模态"你能不能处理"。

### 3.5 踩到的坑

**1. 注释里出现 `/*`**

写 storage_am.h 时写了 `modalities/**`，
在块注释里 `/*` 出现两次 → `-Werror=comment` 编译失败。
**注释里不要写 `/*`**（`/**` 也一样）。

**2. `E_NOTSUP` 在文档里有、代码里没有**

DESIGN.md §5.2 早就约定"能力协商靠 E_NOTSUP"，
但 `common/error.h` 里从来没定义过它 —— 一直没人用到。
接线时第一次用就编译失败。
**文档里写的错误码，要检查代码里有没有。**

**3. 一个返回值不能表达两件事**

`val_cmp` 原本返回比较结果（-1/0/1）。
加上外部值之后，物化失败要报错 —— 但 `E_NOSPACE = 1`，
和"大于"撞了。混用一个返回值必然有一处要靠猜。

改法：加一个 `int *err` 出参。
**返回值的语义空间重叠时，拆开，不要靠约定。**

**4. 断言把"空"当成了"没有"**

单元测试里写了 `CHECK(outlen > 0)`，
但 blob 对象的第 1、2 帧本来就是空的（内容只有 12 字节）。
**断言要区分"空"和"不存在"** —— 前者是合法状态。

**5. 测试不能假设行序**

集成测试里 `SELECT id, name FROM users` 之后检查第 0 行是 "bobby"，
结果失败了 —— 因为 UPDATE 是"插一个新版本"，新版本在堆的末尾。
SQL 没有 ORDER BY 就不保证行序，测试也不能假设。
**测试要查集合，不要查位置。**

**6. 值类型下沉引发的连锁**

`DbValue` 从 `query/` 挪到 `storage/`（因为 SPI 要用它，
放在 query/ 会让 spi/ 反过来依赖 query/）。
连带 `index_key` 也从 `query/` 挪到 `index/`。

**层次问题拖出来的改动，比预想的多** ——
但编译器把每一处都报了出来，没有一处漏。

**7. `DbValue` 变大了**

加了外部值的 5 个字段 → 272 字节变 312 字节。
一行 16 列 = 5KB。可接受，但记一笔：
**值类型是热路径上的东西，每加一个字段都要算账。**

### 3.6 两轮反向验证

**验证一：把下推改成"漏掉第一行"**

注入：`heap_am.c` 的下推扫描里，跳过游标返回的第一行。

结果：**test_index 24 项失败** + 另 2 个测试文件变红。

这一项证明的是：**下推路径真的被测到了**。
如果测试全都走全表扫描，这个注入会完全 invisible。

**验证二：把外部值改成"扫描时就物化"**

注入：`blob_am.c` 的 `decode` 里，`val_external_text` 之后立刻 `val_detoast`。

结果：**test_blob_am 19 项失败**（包括 `after_scan == before` 那条 IO 断言）
+ **test_spi_it 1 项失败**（`after_meta == before`）。

这一项证明的是：**惰性是真的**，
而不是"恰好在测试里没读到帧页"。

两次注入都先确认编译通过再跑 ——
v0.7/v0.8 都踩过"编译失败 → 链接旧 .o → 结论不可信"的坑。

### 3.7 顺手做对的几件事

**1. 执行器拆分**：`exec.c` 1131 行超过硬规则的 800，拆成三个。

**2. `db.c` 拆分**：它本来就 989 行（v0.8 就超了，一直没人管）。
拆成 `db.c`（生命周期）+ `db_stmt.c`（事务与执行）+ `db_blob.c`（blob API）。

**3. `blob.c` 拆分**：加了 `blob_header` / `blob_frame_info` 之后涨到 830 行。
把「对象头格式」拆成 `blob_hdr.{h,c}`（对齐 `record.c` 之于 tuple 格式的做法），
blob.c 回到 749 行。

拆分之后，v0.9 碰过的文件**全部在 800 行以内**。
还超的是历史遗留的两个：`btree.c`(1038) / `catalog.c`(927) —— v0.9 没碰过。

**4. 错误描述能传到用户面前**：加了 `db_last_error()`。
以前执行器的错误描述写进了 `db->errmsg` 但**没人读**，
用户只看到"执行失败: E_INVAL"。现在 REPL 会说
"表 blob_frames 不支持插入"。

**5. `E_NOTSUP` 加进公共 API**（`DB_E_NOTSUP`），
错误分类从"参数非法"里分出来了 ——
"这个模态没这个能力"和"用户请求非法"是两回事。

### 3.8 还没做的

| 项 | 说明 |
|---|---|
| **全局 latch** | 顺延到 v0.10（见下） |
| 关系模态的大值外置 | DESIGN.md §10.4；v0.9 的外部值机制是它的前置 |
| 聚合 / GROUP BY | 没做；blob 的对象级信息用第二张虚拟表表达 |
| 下推的精确边界 | B+tree 只支持闭区间，开区间返回超集（安全但不够快） |
| blob 的 RID 反查 | O(对象数)；将来的修法是 blob 自己维护"页号 → 对象"反查表 |
| 页内 compact / hint bits | v0.7 起顺延 |
| B+tree 物理日志 / 日志截断 / group commit | v0.6 起顺延 |
| `btree.c`(1038) / `catalog.c`(927) 超 800 行 | 历史遗留，v0.9 没碰这两个文件（v0.9 碰过的都已在 800 行内） |

### 3.9 全局 latch 为什么顺延

v0.9 原计划做全局 latch（DESIGN.md §7 的表格里写着"多线程并发跑"）。

**顺延的理由**：latch 涉及全代码库的共享状态，
和 SPI 改造叠加，一次变红就分不清是 SPI 的锅还是 latch 的锅。
**一次只动一件事，才知道是谁弄坏的。**

v0.9 的验收标准因此改成"SPI 定稿 + blob 能被 SQL 扫"，
并发推到 v0.10 和图模态一起做。

### 3.10 下一步（v0.10）

**全局 latch（真并发）+ 模态 2：图**。

图模态是**定长记录**（对齐 Neo4j 的做法）：
一个节点 = 一条定长记录，天然满足 `slot = 记录序号`，
RID 映射不用做任何妥协 —— 这正是 DESIGN.md §5.1 早就写下的那句。

SPI 已经站住了（图模态可以照着 blob 的模式做），
所以 v0.10 的顺序是：先 latch（把并发语义钉死），再叠图模态。

验收目标：10 万节点 / 100 万边，3 跳 p99 < 10ms。

