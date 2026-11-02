# v0.3 执行方案与计划 · B+tree

> 整个项目最容易翻车的一块。DESIGN.md 里列的 6 个经典 bug 全在这里。
>
> 状态：方案已定，执行中

---

## 一、方案

### 1.1 最关键的一个决定：不复用 slotted page

**B+tree 用自己的一套页格式。**

理由：slotted page 的 slot 编号必须**稳定**（RID 依赖它），而 B+tree 需要 slot **按 key 有序**——插入时要在中间位置插 slot，后面的编号全变。两套语义无法共存。

这也是真实引擎的做法：PG 的 nbtree 页和 heap 页格式完全不同。

### 1.2 B+tree 页格式

```
偏移 0    header 28B
 0  u8   type          0x03 inner / 0x04 leaf
 1  u8   flags
 2  u16  nitems
 4  u16  free_off      条目区起点（从页尾往上）
 6  u16  free_bytes
 8  u32  right         leaf: 右兄弟页；inner: 未用
12  u32  leftmost      inner: 最左孩子；leaf: 未用
16  u32  level         0 = leaf
20  u32  page_lsn
24  u16  cksum
26  u16  reserved

28        slot[0] u16
30        slot[1] u16
 ...      （按 key 升序，逻辑顺序由 slot 数组决定）
          ──── free ────
          条目区（从页尾往上，物理位置与逻辑顺序无关）
4096
```

条目：
```
leaf : [klen u16][key klen][rid u64]
inner: [klen u16][key klen][child u32]
```

**为什么需要 slot 数组**：二分查找要 O(1) 随机访问第 i 项。物理位置无关，逻辑顺序由 slot 决定，compact 只改 offset 不改顺序。

### 1.3 内部节点的 key 语义

存 `leftmost` + n 个 `(sep, child)`，其中 `sep[i]` 是第 i+1 个孩子的最小 key。

```
        leftmost   sep[0]   sep[1]
           ↓         ↓        ↓
        [child0]  [child1] [child2]
```

查找用 **upper_bound**（第一个 `sep > target` 的位置）：

```
pos = upper_bound(key)
child = (pos == 0) ? leftmost : item[pos-1].child
```

**为什么是 upper_bound 不是 lower_bound**：`sep[i]` 是"右边孩子的最小值"，等于它的 key 应该去右边。

### 1.4 重复 key 的处理

索引允许同一个 key 对应多个 RID（非唯一索引）。做法：

| 操作 | 位置 |
|---|---|
| 查找 | `lower_bound(key)` → 从该位置向后扫所有 `key == target` |
| 插入 | `upper_bound(key)` → 插到所有相同 key 之后 |
| 删除 | `lower_bound` 找到匹配的 RID |

这样相同 key 聚在一起，顺序稳定。

### 1.5 分裂：叶子和内部节点规则不同（最经典的 bug）

```
叶子分裂（存实际数据）
  [k1 k2 k3 k4 k5] 满 → 插 k3.5
  左 [k1 k2 k3]   右 [k3.5 k4 k5]
  提升 k3.5  ← 两边都保留副本

内部节点分裂（只存路由）
  [p0 k1 p1 k2 p2] 满 → 插 k1.5
  左 [p0 k1 p1]   右 [p2]
  提升 k1.5  ← 不在任何一边，被移走
```

搞反了会导致范围扫描漏数据或多数据，**而且只在随机 key 下暴露**——顺序 key 永远测不出来。

### 1.6 删除：本版只做"删项 + 空页回收"，不做借位合并

| 做 | 不做 |
|---|---|
| 删除项 | 从兄弟借位（redistribute） |
| 页变空时从父节点移除、页还给 freelist | 半满合并（merge） |
| 根只剩一个孩子时降高 | fillfactor 预留 |

**为什么不完整做**：借位 + 合并约 200 行，且是 bug 重灾区。而它的收益是"回收空间"，v0.3 阶段没有实际需求——删除不会让文件增长，只是浪费空间。

**代价（明确记录）**：删一半数据后，页利用率会掉到 30% 左右，树会偏高。等 v0.7 的 vacuum 一起做。

### 1.7 接口

```c
typedef struct BTree BTree;

int  btree_create(Pager *pg, FreeList *fl, u32 *root_out);
int  btree_open(Pager *pg, FreeList *fl, u32 root, BTree **out);
void btree_destroy(BTree *bt);

int  btree_insert(BTree *bt, const void *key, u16 klen, Rid rid);
int  btree_delete(BTree *bt, const void *key, u16 klen, Rid rid);

/* 精确查找：把同一 key 的所有 RID 写进 out[]，*n 返回实际个数 */
int  btree_search(BTree *bt, const void *key, u16 klen, Rid *out, int max, int *n);

/* 范围扫描：[lo, hi]，lo/hi 为 NULL 表示无界 */
typedef struct BtCursor BtCursor;
int  btree_scan(BTree *bt, const void *lo, u16 lolen,
                const void *hi, u16 hilen, BtCursor **out);
int  btree_cursor_next(BtCursor *c, Rid *rid);
void btree_cursor_close(BtCursor *c);

/* 观察用 */
u32  btree_height(const BTree *bt);
u32  btree_npages(const BTree *bt);
```

### 1.8 验收标准

| 项 | 标准 |
|---|---|
| 随机插入 10 万 | 树高 = 2，页利用率 65%–80%（实测 74%）|
| 顺序插入 10 万 | 页利用率 ≈ 50%（实测 49%）|
| 全量查找 | 20 万 key 逐个查到，RID 正确 |
| 重复 key | 同一 key 10 个 RID，全部能查到 |
| 范围扫描 | 结果有序、边界正确、无遗漏 |
| 删除一半 | 剩余的都能查到，删掉的查不到 |
| 删除全部 | 树回到空（height 0，无残留页） |
| 内部节点分裂 | 用随机 key，验证中间 key 不在任何子节点里 |

### 1.9 风险

| 风险 | 表现 | 缓解 |
|---|---|---|
| 内部节点分裂时 key 没移走 | 范围扫描重复 | 专测：分裂后检查中间 key 只出现一次 |
| compact 顺序错 | 条目被覆盖 | 用 scratch buffer 重排，不做原地搬 |
| 借位/合并（本版不做） | — | 明确记录为已知简化 |
| 根降高漏了 | 树越来越深 | 删除全部后断言 height == 0 |
| 空页没还给 freelist | 文件不缩小 | 删除全部后断言 npages 回到 1 |

---

## 二、执行计划

| 步 | 任务 | 产出 | 验收 |
|---|---|---|---|
| 1 | `index/btree.h` / `btree.c`（页格式 + 查找 + 插入 + 分裂） | 约 550 行 | 编译零警告 |
| 2 | `btree.c`（删除 + 空页回收 + 扫描器） | 约 250 行 | — |
| 3 | `test/unit/test_btree.c` | 约 400 行 | 8 项全过 |

**本轮执行全部三步**——B+tree 拆开做没有意义，分裂和删除共用同一套页操作。

---

## 三、执行记录

### 第 1–3 步 ✅ 完成

| 文件 | 行数 | 验收 |
|---|---|---|
| `index/btree.h` / `btree.c` | 78 + 760 | 8 项全过 |
| `test/unit/test_btree.c` | 400 | |

实测：

```
随机 100000：高度 2，533 页（530 叶 / 3 内），填充率 74%
顺序 100000：高度 2，792 页（787 叶 / 5 内），填充率 49%
```

### 踩了四个坑，两个是硬 bug

**① 叶子分裂搬条目时顺序反了（硬 bug）**

第一版从高索引往低索引遍历、往新页**追加**：

```c
for (u16 i = n; i-- > mid; ) {
    insert_at(nf, bt_nitems(nf), ...);   /* 追加 */
    remove_at(pg, i);
}
```

结果新页里的 key 是**降序**。叶子区间互相重叠（leaf[0] 是 0..892，leaf[1] 是 637..893），提升的 key 全错，1000 个 key 查出 637 个失败。

修法：反复读下标 `mid`（删掉后后面的项自动补位），顺序追加 → 升序。

**② 索引 key 用了小端（硬 bug，而且我自己写过这条）**

DESIGN.md §3.2 和 `record.h` 里我都写了"索引 key 必须大端"，测试里还是用了 `put_u32`（小端）。

后果：`memcmp(63, 256)` 返回正数（63 的字节是 `3F 00 00 00`，256 是 `00 01 00 00`，第一个字节就分出大小）。二分查找直接把 256 判成"比所有项都小"，插到位置 1。

这个 bug 的可怕之处：**小 key（< 256）完全正常**，只有跨越字节边界才错。追踪打出来才看见：

```
[ub] search=256 lo=0 hi=127 mid=63 item=63 c=1   ← 63 被判为 > 256
```

修法：测试改用大端编码；并在 `btree.h` 顶部把这条契约写成显式警告。

**③ 分裂后没 compact（导致填充率虚高）**

`remove_at` 留下的是洞，`free_bytes` 只算 slot 区和条目区之间的连续空隙，不含洞。分裂后页里有 127 项，但 `free_off` 还停在原位置，于是 `free_bytes` 很小 → 这一页"看起来几乎满" → 后续插入误判要再分裂。

表现：顺序插入填充率算出 93%（实际 787 叶 × 127 项 = 50%，树是对的，指标错了）。

修法：`split_leaf` 末尾补 `bt_compact`。

**④ 填充率统计本身也不该只看 free_bytes**

`used = cap - free_bytes` 把洞算成已用。改成按条目实际字节累加：

```c
for (i = 0; i < nit; i++) used += item_len(pg, i) + 2;
```

### 一个数值修正

PLAN §1.8 原写"100 万 key 树高 3"，**错了**。

叶扇出 = 4068 / (14+2) = 254，内扇出 = 4068 / (10+2) = 339。

- 10 万 key → 约 530 叶 → 根直接装得下 → **高度 2**
- 树高 3 需要叶数 > 339² = 114,921，按 69% 填充约 **2000 万 key**

所以 10 万 key 高度 2 是正确的，已改测试断言。

### 已知简化（明确记录）

**删除只做"删项 + 空页回收"，不做借位合并。**

删一半数据后页利用率会掉到 30% 左右，树会偏高。这是 v0.3 的主动取舍（见 §1.6），等 v0.7 的 vacuum 一起做。

### v0.3 完成情况

**8 项验收全过。加上 v0.1 的 33 项、v0.2 的 17 项，累计 58 项验收 + 2 项端到端全部通过。**

下一步 v0.4：`query` 层（lexer → parser → plan → exec），把 SQL 文本变成能跑的计划。
