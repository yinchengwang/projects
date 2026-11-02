# v0.2 执行方案与计划 · 堆表与元数据

> 对应 DESIGN.md 第 11 节的第二层。目标是让「一张表」这个概念真正存在：
> 能建表、能按 RID 存取行、能全表扫描、元信息重启后还在。
>
> 状态：方案已定，执行中

---

## 一、方案

### 1.1 范围

| 模块 | 文件 | 职责 |
|---|---|---|
| 堆表 AM | `storage/heap.{h,c}` | 把 page/freelist/pager 封装成"表的行集合" |
| 元数据 | `catalog/catalog.{h,c}` | 4 张系统表 + bootstrap，建表/查表 |
| RID 类型 | `common/types.h` 增补 | `(page_id, slot)` 的统一表示 |

**不做**：大值 overflow（超页行直接拒绝，v0.8 再做）、索引元信息（建表结构，v0.3 填内容）、MVCC（v0.6）。

### 1.2 五个需要拍板的点

#### 决策 A · 每表一个 FreeList，非本表页档位为 0

一个全局 freelist 会让 A 表插到 B 表的页里。所以每表独立。

**代价**：每表一个 `npages` 字节的数组。5 张表 × 1 万页 = 50 KB，可接受。

**这要求改 freelist 的默认语义**：现在是"新建即整页空闲"，改成"**默认不可用，必须显式登记**"。否则 `freelist_grow` 会把新页错误地送给所有表。

改动：`freelist_create` / `freelist_grow` 新增的页档位设为 **0（不可用）**，调用方用 `freelist_set(fl, pid, FL_USABLE)` 显式登记自己的页。

#### 决策 B · 页链 + 内存 tail 指针

表 = `root_page` + 用 `page_next` 串起来的页链。

`heap_open` 沿链走到尾，把 tail 缓存在内存。插入需要新页时直接挂到 tail，O(1)。重启后重扫一次链，O(npages)，可接受。

**为什么不用"连续页范围"**：多表交错分配时连续范围不成立。

#### 决策 C · 超页行直接拒绝

一行编码后 > `PAGE_SIZE - HDR - SLOT`（4072）时，`heap_insert` 返回 `E_NOSPACE`，不做 overflow 链。

**理由**：overflow 是 v0.8 的模态（blob 分块）的核心机制，现在做会写两遍。v0.2 只保证"正常大小的行"能存。

#### 决策 D · update 的两种路径

| 情况 | 做法 | RID |
|---|---|---|
| `new_len <= old_len` | `page_update` 原地改 | **不变** |
| `new_len > old_len` | `page_delete` + `heap_insert` | **可能变** |

返回 `new_rid` 让调用方知道。这条对应 DESIGN.md 的 D2（更新 = 插新版本），v0.6 加 MVCC 后第二种路径会变成唯一路径。

#### 决策 E · 现在就埋 WAL 钩子（本轮最重要的设计）

所有修改页的路径都经过一个函数指针：

```c
typedef int (*HeapWalFn)(void *ctx, u32 page_id, u16 slot,
                         const u8 *before, u16 before_len,
                         const u8 *after,  u16 after_len);
```

v0.2 里 `wal_fn == NULL` 时直接跳过。v0.5 写 WAL 时只需注册一个函数，**不用改 heap 的任何逻辑**。

**额外收益**：现在就能在测试里注册一个"记账"钩子，验证 insert / update / delete 三条路径**都**调用了它、次数正确。这等于提前测出了 v0.5 最容易犯的错——漏写日志。

### 1.3 接口

```c
/* common/types.h 增补 */
typedef struct { u32 page; u16 slot; } Rid;
u64  rid_to_u64(Rid r);
Rid  rid_from_u64(u64 v);
bool rid_is_nil(Rid r);

/* storage/heap.h */
typedef struct Heap     Heap;
typedef struct HeapScan HeapScan;

int  heap_create(Pager *pg, FreeList *fl, u32 *root_page_out);
int  heap_open(Pager *pg, FreeList *fl, u32 root_page, Heap **out);
void heap_destroy(Heap *h);

void heap_set_wal(Heap *h, HeapWalFn fn, void *ctx);   /* v0.5 用 */

int  heap_insert(Heap *h, const u8 *rec, u16 len, Rid *rid_out);
int  heap_get(Heap *h, Rid rid, const u8 **rec, u16 *len);
int  heap_update(Heap *h, Rid rid, const u8 *rec, u16 len, Rid *new_rid);
int  heap_delete(Heap *h, Rid rid);

int  heap_scan_begin(Heap *h, HeapScan **out);
int  heap_scan_next(HeapScan *s, Rid *rid, const u8 **rec, u16 *len);
void heap_scan_end(HeapScan *s);
```

### 1.4 关键算法

**insert**

```
if (len + SLOT_SIZE > PAGE_SIZE - HDR_SIZE) return E_NOSPACE   // 决策 C
pid = freelist_find(fl, len + SLOT_SIZE)
if (pid == NIL):
    pager_alloc → page_init(PT_HEAP) → 挂到 tail → freelist_grow + set(USABLE)
    pid = 新页
pager_get(pid)
page_insert → slot
WAL(after = rec)                        ← 钩子
freelist_set(fl, pid, page_free_bytes)
pager_unpin(pid, dirty = true)
```

**scan**

```
沿页链逐页遍历；每页扫 slot 0..slot_count-1，跳过 length == 0。
遍历时 pin 一页、吐完所有行、unpin —— 不要同时 pin 多页（池子会爆）。
```

### 1.5 验收标准

| 项 | 标准 |
|---|---|
| 插入 | 3000 行，RID 全局唯一，逐行读回一致 |
| 跨页 | 确认使用了 > 1 个页，且页链完整 |
| 删除 | 删除后 `heap_get` 返回 `E_NOTFOUND`，其余行不受影响 |
| 更新（缩小） | RID 不变，内容更新 |
| 更新（变大） | 返回新 RID，旧 RID 失效 |
| 扫描 | 遍历出的行数 == 活行数，且与插入集合完全相同（顺序无关） |
| 超页行 | 4072 字节以上返回 `E_NOSPACE`，不崩溃 |
| **WAL 钩子** | 注册计数钩子：insert N 次 → 触发 N 次；update/delete 同理 |
| 重启 | 关闭重开后，行、页链、扫描全部正常 |

### 1.6 风险

| 风险 | 表现 | 缓解 |
|---|---|---|
| 扫描时同时 pin 多页 | 池子被 pin 死，报 E_NOSPACE | 扫描器每页用完立即 unpin |
| 页链断裂 | 扫描少行，但不报错 | 扫描时断言 `next != 自己的 page_id`，并统计页数 |
| freelist 语义改动波及 v0.1 | page/freelist 测试失败 | 先改 freelist + 修测试，再动 heap |
| WAL 钩子漏调用某条路径 | v0.5 才发现，要重构 | 本轮就用计数钩子测三条路径 |
| update 变大后旧 RID 悬空 | 索引指向空 slot | 旧 RID 返回 E_NOTFOUND 即可，v0.6 vacuum 清理 |

---

## 二、执行计划

| 步 | 任务 | 产出 | 验收 |
|---|---|---|---|
| 0 | 改 freelist 默认语义（新页 = 不可用）+ 修测试 | 改 2 处、测试 +1 项 | v0.1 全部测试仍过 |
| 1 | `common/types.h` 增补 Rid | 20 行 | 编译通过 |
| 2 | `storage/heap.h` / `heap.c` | 约 300 行 | 编译零警告 |
| 3 | `test/unit/test_heap.c` | 约 280 行 | 9 项验收全过 |
| 4 | `catalog` + 系统表 | 约 350 行 | 建表后重启元信息还在 |

**本轮执行第 0–3 步**（堆表闭环）。第 4 步 catalog 依赖 heap 稳定，且要改 bootstrap，单独一轮更稳。

---

## 三、执行记录

### 第 0–3 步 ✅ 完成（堆表闭环）

| 文件 | 行数 | 验收 |
|---|---|---|
| `common/types.h`（+Rid） | +40 | — |
| `common/error.h/c`（+E_EOF） | +3 | — |
| `storage/heap.h` / `heap.c` | 72 + 372 | 9 项全过 |
| `test/unit/test_heap.c` | 380 | |

实测：2000 行 / 77 页；重启后 14 页 / 存活 333 行（小池子 16 帧强制逐出，全部读回正确）。

### 执行中发现的三件事

**① freelist 语义改动会波及所有调用方**

把「新建页默认整页空闲」改成「默认不可用、必须显式登记」后，`test_freelist` 的 4 个用例、`test_v01` 的建表路径都要跟着改。

**这个改动是必要的**：多张表各持一个 FreeList 时，如果新页默认 255，A 表会把 B 表刚分配的页当成空闲页。改动后语义变成「我只管我登记过的页」，从根上消除了串页。

**② WAL 钩子的价值在测试阶段就体现了**

`t8_wal` 用一个记账钩子验证三条路径：

```
10 次 insert           → ins = 10
1 次缩小 update        → upd = 1
1 次变大 update        → del = 1, ins = 11   ← 内部走了 delete + insert
5 次 delete            → del = 6
合计                    → 18 次
```

如果 v0.5 写 WAL 时漏了某条路径（比如 update 的变大分支忘了写 before image），这个测试会立刻抓到。**钩子在 v0.2 就位，比 v0.5 才发现要重构便宜得多。**

**③ 集成测试里的重复类型定义**

`test_v01.c` 自己定义了 `typedef struct {u32 page; u16 slot;} Rid;`，和 `types.h` 新增的 `Rid` 冲突。删掉本地定义即可。

这类问题只会出现在「先写测试、后提取公共类型」的路径上——提取类型时记得搜一遍全仓。

### 关于 t2 的一个设计失误

第一版 `t2_cross_page` 插完 2000 行后想"抽查前 200 行"，但没保存 RID，于是写了一段空转的循环（`(void)out;`）。改成保存 `rids[2000]` 后逐行验证——**没保存 RID 就没法验证，这本身说明测试设计不完整**。

### 第 4 步 ✅ 完成（catalog）

| 文件 | 行数 | 验收 |
|---|---|---|
| `catalog/catalog.h` / `catalog.c` | 82 + 560 | 8 项全过 |
| `test/unit/test_catalog.c` | 240 | |
| `test/integration/test_v02.c` | 220 | **端到端通过** |

端到端实测：

```
22 页 users + 6 页 events；重启后 2000 行 / 500 行全部校验通过
```

### 自举的两个必要硬编码

**① 系统表 id 写死 1..4，绝不查表**

否则死锁：要查 sys_table 里的行，得先知道 sys_table 的 root_page；而 root_page 存在 sys_table 里。

所以 meta 页（第 0 页）直接存 `catalog_root`，绕开这个循环。

**② 系统表自己的 schema 也写死在代码里**

同样的循环：`sys_column` 的行要靠 schema 才能解码，而 schema 又存在 `sys_column` 里。

所以 4 张系统表的 `ColDesc[]` 是 `catalog.c` 里的 `static const` 数组，用户表的 schema 才从磁盘读。

**这就是为什么 `sys_cols_of()` 那个 switch 不能删**——它不是优化，是自举的必需条件。

### 三个实现细节

**③ meta 字段从 `HDR_SIZE` 之后开始**

第 0 页也是 slotted page，页头占 0..20 字节。meta 字段如果从 0 开始写，会把 `H_TYPE`、`H_SLOTS` 全冲掉。

```
M_MAGIC   = HDR_SIZE + 0
M_VERSION = HDR_SIZE + 4
M_CATROOT = HDR_SIZE + 8
M_NEXTTID = HDR_SIZE + 12
M_PAGESZ  = HDR_SIZE + 16
```

`M_PAGESZ` 存进去是为了**换页大小后立刻报 E_CORRUPT**，而不是读出一堆垃圾再莫名其妙崩溃。

**④ `ColDesc.name` 从 `const char*` 改成 `char[32]`**

catalog 要深拷贝 schema（调用方传进来的 `ColDesc[]` 可能是栈上的临时数组）。用指针就得管字符串生命周期，用定长数组直接 `memcpy` 整块。

`record.c` 只读 `type` / `nullable`，不受影响；测试里的 `{ "id", T_INT4, false }` 初始化也仍然合法。

**⑤ 加载分两遍**

第一遍扫 `sys_table` 把所有表的 heap 都 open（否则读不到各自的列）；第二遍才对用户表扫 `sys_column` 拼 schema。

一遍做不到：扫到 users 表时，`sys_column` 的 heap 可能还没 open。

### v0.2 完成情况

| 模块 | 状态 |
|---|---|
| `common/types.h`（+Rid） | ✅ |
| `storage/heap` | ✅ 9 项 |
| `catalog` | ✅ 8 项 |
| 端到端 | ✅ 2 表 / 2500 行 / 重启校验 |

**v0.2 合计 17 项验收 + 1 项端到端。加上 v0.1 的 33 项，累计 50 项验收全部通过。**

下一步 v0.3：`index/btree`（B+tree + 迭代器），这是整个项目里最容易翻车的一块。
