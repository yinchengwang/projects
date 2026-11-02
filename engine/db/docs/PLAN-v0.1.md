# v0.1 执行方案与计划 · 存储原语层

> 对应 DESIGN.md 第 11 节的第一层。目标是让「一行数据能存进去、能读回来、能删掉、空间能回收」。
>
> 状态：方案已定，执行中

---

## 一、方案

### 1.1 范围

v0.1 只做三件事，**不做**索引、不做事务、不做 SQL：

| 模块 | 文件 | 职责 |
|---|---|---|
| slotted page | `storage/page.{h,c}` | 页内变长记录的增删改查、碎片整理 |
| tuple 编解码 | `storage/record.{h,c}` | 把一行（多列）序列化成字节串，反之亦然 |
| 页分配 | `storage/freelist.{h,c}` | 哪些页有空位、哪些页整页空闲 |

依赖关系：`page` 只依赖 `pager` 提供的 `u8[4096]`；`record` 只依赖 `page` 的 slot 概念；`freelist` 依赖 `pager`。

### 1.2 页内布局（已定，见 DESIGN.md §3）

```
偏移      内容
0x0000    header                     20 B
0x0014    slot[0]                     4 B  ┐
0x0018    slot[1]                     4 B  │ 向下增长 ↓
  ...     ...                              │
          ───────── free space ─────────
          ...                              │ 向上增长 ↑
          record #1                        │
0x0FFF    (页尾)
```

- slot = `offset: u16 + length: u16`，`length == 0` 表示已删除。
- 记录区从页尾往下长，`free_off` 指向当前最低的记录起点。

### 1.3 四个需要拍板的设计点

#### 决策 A · slot 永不复用（已定）

删除只把 `length` 置 0，**slot 编号永久保留**。

**为什么**：索引里存的是 `RID = (page_id, slot)`。slot 一旦被复用，索引就指向一条完全无关的新记录——不报错，只返回错数据。这种 bug 极难定位。

**代价**：删除多、插入多的页，slot 数组会一直长，浪费 `4 B × 删除次数`。缓解手段是后续 vacuum 时整页重建（v0.7），v0.1 不做。

**已量化的硬上限**：`4 B/slot`，一页 slot 数组最多约 1000 个，**即一页总共只能承受约 1000 次插入**（不管删掉多少）。测试实测：250 个 slot 用满时，页就再也插不进新记录了。真实 heap 层遇到这种情况就分配新页——这个策略属于 v0.2 的 `freelist`，不属于 page 层。

**备选方案（否决）**：删除即回收 slot，用"世代号"（generation）防悬空。复杂度翻倍，收益在 v0.1 阶段为零。

#### 决策 B · compact 只在必要时触发

`page_compact` 不自动调用。插入空间不足时，**先看碎片率**：碎片率 > 0 才 compact，再重试。

**为什么**：compact 要搬所有记录，是 O(页内记录数)。每次插入都 compact 会让写放大到不可接受。而"有碎片但不 compact 也插得下"是常态。

**代价**：调用方（heap.c）要知道什么时候该 compact。所以暴露 `page_fragmentation()` 给它做决策。

#### 决策 C · 更新只允许缩小或等长

`page_update` 在 `new_len > old_len` 时直接返回 `E_NOSPACE`，调用方必须 delete + insert（**RID 会变**）。

**为什么**：变长更新要在页内挪动后续记录，且可能触发跨页迁移——那是 heap 层的活，不该塞进 page 层。而且 DESIGN.md 的 D2 已经定了「更新 = 插新版本」，RID 本来就会变。

**代价**：非 MVCC 场景下的原地更新会退化。接受。

#### 决策 D · checksum 覆盖整页

`page_set_checksum` 先把 `H_CKSUM` 字段清零，再对 4096 字节算 crc16，最后写回。

**为什么**：只算已用区要处理"空闲区边界"，边界一错就漏检。整页算简单且正确，代价是每次修改都要重算 4KB 的 crc（约 2μs，可接受）。

### 1.4 接口

```c
/* storage/page.h —— 所有函数都在 u8[PAGE_SIZE] 上操作，不碰文件 */

int  page_init(u8 *pg, u8 page_type);

/* 插入。成功返回 slot 编号（>= 0）；空间不足返回 E_NOSPACE。
 * 注意：失败时不保证页内容不变（可能已经 compact 过）。 */
int  page_insert(u8 *pg, const u8 *rec, u16 len, u16 *slot_out);

/* 读取。*rec 指向页内，调用方不得修改，且页被逐出后失效。 */
int  page_get(const u8 *pg, u16 slot, const u8 **rec, u16 *len);

/* 原地改写，要求 len <= 原长度。 */
int  page_update(u8 *pg, u16 slot, const u8 *rec, u16 len);

/* 删除。slot 编号永久保留（决策 A）。 */
int  page_delete(u8 *pg, u16 slot);

/* 碎片整理：把活记录压到页尾，消除空洞。slot 编号不变。 */
void page_compact(u8 *pg);

/* 统计 */
u16  page_free_bytes(const u8 *pg);     /* 可用的连续空间 */
u16  page_used_bytes(const u8 *pg);     /* 记录区已占字节（含空洞） */
u16  page_live_bytes(const u8 *pg);     /* 活记录总长 */
u16  page_fragmentation(const u8 *pg);  /* (used - live) / used，百分比 */
u16  page_slot_count(const u8 *pg);     /* 已分配 slot 数（含空洞） */
bool page_slot_alive(const u8 *pg, u16 slot);

/* 叶子链（heap 页链、overflow 链都用） */
u32  page_next(const u8 *pg);
void page_set_next(u8 *pg, u32 next_page);

/* 校验 */
void page_set_checksum(u8 *pg);
bool page_verify_checksum(const u8 *pg);
```

### 1.5 关键算法

**插入**

```
need = len + SLOT_SIZE
if free_bytes < need:
    if fragmentation > 0: page_compact()
    if free_bytes < need: return E_NOSPACE
slot  = slot_count++
free_off -= len
memcpy(pg + free_off, rec, len)
free_bytes -= need
slot[slot] = { free_off, len }
```

**compact —— 必须按 slot 升序（= offset 降序）**

```
dst = PAGE_SIZE
for i = 0 .. slot_count-1:              ← 升序！见下方说明
    if alive(i):
        dst -= len(i)
        memmove(pg + dst, pg + off(i), len(i))
        off(i) = dst
free_off = dst
free_bytes = free_off - HDR_SIZE - slot_count*4
```

**为什么是升序（这条一开始写反了，靠测试纠正）**

记录总是从页尾往下长，所以 **slot 号越大，offset 越小**。打包到页尾时要让高地址的记录先归位，也就是按 offset 降序处理——对应 slot 升序。

这样每条记录的目的地址 `dst >= 原 offset`，只向上移动；还没处理的记录全在更低的地址，绝不会被覆盖。

反例（slot 倒序会踩）：`slot0 R1@3996(100)`、`slot1 R3@3796(100)`、`slot2 R4@3596(200)`。倒序先搬 R4 到 3896..4095，正好盖掉还没搬的 R1。

这个例子已写成测试用例 `t6_compact_order`——它是唯一能抓到这个 bug 的用例。

### 1.6 验收标准

| 项 | 标准 |
|---|---|
| 基本读写 | 插入 → 读取，内容逐字节一致 |
| 容量 | 插到满，每个 slot 都能正确读回（64B 记录应正好 59 条） |
| 删除 | 删除后读取返回 `E_NOTFOUND`，其余 slot 不受影响 |
| **RID 稳定** | 删除 + compact 后，所有活记录的 slot 编号不变 |
| **slot 不复用** | 删 slot 3 后再插入，新 slot 编号 > 原最大编号 |
| 碎片回收 | 随机增删 10 万次后 compact，可用空间**精确等于**理论值 |
| **整理彻底性** | 每次 compact 后碎片率**精确为 0**（原定"峰值 < 20%"已作废，理由见下） |
| 不变量 | 任意时刻 `HDR + slots×4 + used + free == PAGE_SIZE` |
| 校验 | 改任意一字节，`page_verify_checksum` 返回 false |

> **为什么废掉"峰值碎片 < 20%"**：碎片率 = (已用 − 有效) / 已用。当一页的记录被删空时，已用仍 > 0、有效为 0，碎片率必然 100%——但这完全无害，下次插入会自动整理。这个指标单独看没有意义。
>
> 真正有意义的是**整理彻底性**：compact 后必须精确为 0，且可用空间精确恢复到 `PAGE_SIZE - HDR - slots×4 - live`。这条已经在测试里逐次断言（211 个页世代、每次整理都验）。

### 1.7 风险

| 风险 | 表现 | 缓解 |
|---|---|---|
| compact 处理顺序错 | 记录被覆盖，读到别人的数据 | 用 `slot0 R1@3996 / slot1 R3@3796 / slot2 R4@3596` 这个用例专测（`t6_compact_order`） |
| 碎片率算错 | 明明有空间却报 E_NOSPACE | 单测里断言 `free + used + slots*4 + HDR == PAGE_SIZE` |
| slot 偏移越界 | 读到页外 | `page_get` 校验 `off >= free_off && off + len <= PAGE_SIZE` |
| 未初始化内存 | 已踩过（pager 那轮） | 比较前 memset，测试里显式填满整页 |

---

## 二、执行计划

四步，每步有产出和验收，**不达标不进下一步**。

| 步 | 任务 | 产出 | 验收 |
|---|---|---|---|
| 1 | 写 `page.h` / `page.c` | 约 320 行 | 编译零警告 |
| 2 | 写 `test/unit/test_page.c` | 约 260 行 | 9 项验收全过 |
| 3 | 写 `record.h` / `record.c` | 约 180 行 | tuple 编解码往返一致 |
| 4 | 写 `freelist.h` / `freelist.c` | 约 120 行 | 页分配/回收不重不漏 |

**本轮执行第 1–2 步**（page 层完整闭环）。第 3–4 步待续——record 依赖 page 的 slot 语义稳定，等 page 测试全绿再动，避免两个模块同时改。

---

## 三、执行记录

### 第 1–2 步 ✅ 完成（page 层闭环）

| 项 | 结果 |
|---|---|
| `storage/page.h` | 62 行 |
| `storage/page.c` | 296 行 |
| `test/unit/test_page.c` | 420 行，9 项验收全过 |
| 编译 | `-Wall -Wextra -Werror` 零警告 |
| 随机压测 | 10 万次 / 211 个页世代，插入成功 52650 次 |

### 执行中发现并修正的三件事

**① compact 方向写反了（方案阶段就错）**

原方案写"从后往前压"。真实规则是**按 slot 升序**——因为记录从页尾往下长，slot 号越大 offset 越小，所以 slot 升序 = offset 降序 = 高地址先归位。

反例（已固化为用例 `t6_compact_order`）：`slot0 R1@3996(100)`、`slot1 R3@3796(100)`、`slot2 R4@3596(200)`。倒序先搬 R4 到 3896..4095，正好盖掉还没搬的 R1。**这个用例是唯一能抓到该 bug 的测试**。

**② slot 不复用的硬上限被量化**

一页约 1000 个 slot × 4 B = 4 KB，即**一页最多承受约 1000 次插入**，与删掉多少无关。原测试写"10 万次随机增删"直接退化：98200 次插入失败、存活归零。改为模拟"页满就换页"（250 slot 一世代，共 211 个世代）后才是有意义的压测。

**③ 验收标准本身设计错了**

原定"碎片率峰值 < 20%"。实测峰值 100%——因为页被删空时已用 > 0、有效 = 0，碎片率必然 100%，但完全无害。

改为**整理彻底性**：每次 compact 后碎片率必须精确为 0，且可用空间精确恢复。这条比峰值指标严格得多，且逐次断言（211 个世代每次整理都验）。

### 第 3–4 步 ✅ 完成（record + freelist）

| 文件 | 行数 | 验收 |
|---|---|---|
| `storage/record.h` / `record.c` | 90 + 236 | 10 项全过 |
| `storage/freelist.h` / `freelist.c` | 56 + 143 | 8 项全过 |
| `test/unit/test_record.c` | 240 | |
| `test/unit/test_freelist.c` | 190 | |
| `test/integration/test_v01.c` | 220 | **端到端通过** |

### 端到端结果（v0.1 的真正验收）

```
3000 行 / 51 页，重开后全部读回一致；freelist 从页内容重建成功
```

四层合起来能用了：`record.encode → freelist.find → pager.get/alloc → page.insert → unpin(dirty) → close → reopen → page.get → record.decode`。

### 执行中又修正了两件事

**④ freelist 的量化方向必须是"保守"**

档位换算 `level = floor(free×255/4076)`，查找用 `need_level = ceil(need×255/4076)`。

这个组合保证 **find 返回的页一定真的放得下**（无假阳性）。反过来不保证——255 档对 4076 字节，粒度约 16 字节，可能漏掉"刚好够"的页。漏掉的代价是调用方新开一页（浪费更多），但如果反过来用 floor，就会返回放不下的页，调用方**重设档位后还是同一个档位，会死循环**。

测试用 4 万组 `(free, need)` 组合穷举验证了这条性质。

**⑤ 测试自己算错了端点**

`for (free = 0; free <= 4076; free += 7)` 的最后一个采样是 **4074 而不是 4076**（除不尽），所以档位是 254 而非 255。改成显式测两个端点。这类"步长除不尽"的错在边界测试里极常见。

### v0.1 完成情况

| 模块 | 状态 |
|---|---|
| `common/*`（types/error/config/crc） | ✅ |
| `storage/pager` | ✅ 6 项 |
| `storage/page` | ✅ 9 项 |
| `storage/record` | ✅ 10 项 |
| `storage/freelist` | ✅ 8 项 |
| 端到端 | ✅ 3000 行重开一致 |

**合计 33 项验收 + 1 项端到端，全部通过。**

下一步是 v0.2：`heap`（堆表 AM）+ `catalog`（4 张系统表）。
