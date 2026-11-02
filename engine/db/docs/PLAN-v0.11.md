# v0.11 执行方案与计划 · 模态 2：图（定长记录 + 邻接链）

> 状态：计划已生成，未执行。执行记录见 §3。
> 前置：v0.10（全局 latch）已完成，152 项验收全绿。

---

## 一、方案

### 1.0 这一版要证明什么

DESIGN.md §7 路线图的原话：

> **v0.11** | 模态 2：图 | 10 万节点/100 万边，3 跳 p99 < 10ms

和 blob（v0.8）一样的逻辑：**这一版的首要产出不是"图能查"，而是"SPI 能不能再吃下一个模态"**。
判据只有一条：

> 图模态接进 SQL 后，**执行器一行都不改**（`grep graph src/query/` 零命中）。
> 如果要改，说明 v0.9 定的 SPI 有缺口 —— 当场补，然后记录补的过程。

blob 已经验证过一遍 SPI 的"读 + 能力协商 + 虚拟表"路径；
图要验证的是 SPI 的**另一半**：**高频随机点查（`get` by RID）+ 定长写路径**。
blob 一帧独占一页、对象不可变；图是**一页几十个节点、就地更新** ——
这是两种相反的访问形态，SPI 如果撑得住，设计才算真正站住。

### 1.1 范围

**做：**

1. `src/modalities/graph/` —— 图存储：定长记录 + 双向邻接链（对齐 Neo4j 的记录布局）。
2. 图的 `StorageAM` 适配（复用 v0.9 的 SPI，`spi/graph_am.{h,c}`）。
3. 两张虚拟表接进 SQL：`graph_nodes` / `graph_edges`（复用 v0.9 的注册表）。
4. 邻居遍历的 C API：`db_graph_neighbors` / `db_graph_hop`（SQL 表达不了递归，遍历走 API）。
5. 崩溃安全：复用 `WAL_PAGEDATA` 整页镜像（blob 的先例，恢复逻辑零改动）。
6. 性能验收：10 万节点 / 100 万边 / 3 跳 p99 < 10ms。

**不做（明确划掉）：**

| 项 | 为什么 |
|---|---|
| 变长属性存储（Neo4j 的 PropertyRecord） | 属性系统是独立的第三个子系统；本版用"固定列 schema"表达节点属性，验证的是图拓扑机制 |
| 图算法（最短路、PageRank…） | 遍历原语（邻接跳转）是它们的地基，原语没验收之前算法都是空中楼阁 |
| MVCC 版本链 | 见 §1.4 的判决 —— 图模态走"锁 + WAL"，对齐 Neo4j 的真实选择 |
| Cypher / 图查询语言 | SQL 虚拟表 + 遍历 API 已够验收；查询语言对理解内核没有增量价值 |
| 细粒度 latch | 全局 latch 覆盖图的所有入口（v0.10 的第 8 条硬规则：新入口必须加锁） |

### 1.2 关键建模问题：节点和边怎么变成"行"

blob 的教训（PLAN-v0.9 §1.3）还适用：**先定行模型，再写一行代码**。

DESIGN.md §5.1 已给的约定：

> 图模态的"节点 id"要映射成 `(page_id, slot)` —— 定长记录天然满足（`slot = 记录序号`）。

三个候选：

| 候选 | 做法 | 判决 |
|---|---|---|
| (a) 边内联在节点记录里 | 节点记录尾部放邻接数组 | **否决** —— 内联数组让记录变长，"定长"这个前提就没了 |
| (b) 边存普通堆表，图只管节点 | 复用 heap | **否决** —— 3 跳 = 沿边链跳，边记录若藏在 B+tree/slotted page 里，跳一次 = 一次页内 slot 查找 + 可能在任意页；定长记录的直接算术定位正是本版的理由 |
| (c) **节点和边都是定长记录，边挂双向邻接链** | Neo4j 式：节点记录存"第一条边的位置"，边记录存 prev/next 双向链 | **采纳** |

#### (c) 的记录布局（对齐 Neo4j，逐字段注明用途）

```
节点记录（定长 GR_NODE_SIZE，预估 ~40B）：
  in_use        1B    在用标记（0 = 空闲，挂在空闲链上）
  next_free     4B    空闲链：下一个空闲节点（in_use=0 时有效）
  first_out     8B    第一条出边的 (page,slot)   ← 邻接链的头
  first_in      8B    第一条入边的 (page,slot)   ← 反向链的头
  prop[...]     定长  本版固定列的属性（见下）

边记录（定长 GR_EDGE_SIZE，预估 ~48B）：
  in_use        1B
  next_free     4B
  src           8B    起点节点 (page,slot)
  dst           8B    终点节点 (page,slot)
  out_prev      8B    起点出边链的.prev   ← 双向链：删除 O(1)
  out_next      8B    起点出边链的.next
  in_prev       8B    终点入边链的.prev
  in_next       8B    终点入边链的.next
```

**为什么边要两条链（出链 + 入链）**：
3 跳查询走的是**有向**遍历（沿出边）；
但"谁指向我"的反向查询（入边）在图应用里同样高频（Neo4j 两种都维护）。
没有入边链，反向查询 = 全表扫边 —— 100 万边的全扫每次几毫秒，p99 必炸。

**RID 的语义**：图的行 = 一个节点或一条边。
`graph_nodes` 的行 RID = 节点记录的 `(page, slot)`；
`graph_edges` 的行 RID = 边记录的 `(page, slot)`。
**边记录里存的 src/dst 就是节点 RID** —— 指针跳转 = `am->get(am, rid, ...)`
直接命中，没有任何间接层。这是定长记录送的礼物：blob 需要纯算术换算，
图连换算都不用。

#### 属性怎么办（本版的最简裁决）

节点属性用**固定列 schema**：建图时声明列（复用 `ColDesc`），
属性内联在节点记录的 prop 区。
**列类型限 INT / FLOAT / TEXT(定长)** —— 可变长属性本版不做（见 1.1 不做表）。

理由：本版要验证的是**拓扑机制**（邻接链、指针跳转、定长定位），
不是属性系统。属性系统的真实复杂度（变长、嵌套、索引）值得单独一版，
和图算法一起放 v1.2+。

### 1.3 存储格式：不用 slotted page，用"定长槽页"

现有 `page.c` 是 slotted page（变长记录 + slot 数组）。
图的记录是定长的，用 slotted page 是自缚手脚：

| 候选 | 判决 |
|---|---|
| (a) 复用 slotted page，每记录一个 slot | **否决** —— slot 数组是给变长记录定位用的；定长记录的位置纯算术可得，slot 数组纯属浪费一倍空间 |
| (b) **定长槽页**：一页 = 固定 N 个槽，`slot i` 的位置 = `页头 + i × rec_size` | **采纳** |

```
图页头（16B）：
  magic/type    页类型（PT_GRAPH_NODE / PT_GRAPH_EDGE）
  rec_size      记录大小（节点页 / 边页各自固定）
  nslots        本页槽数 = (4096 - 16) / rec_size
  free_head     本页空闲链头（slot 序号，-1 = 无）

RID = (page_id, slot)。记录物理位置 = 页内偏移，纯算术，零查找。
```

**删除**：`in_use = 0`，`next_free` 指向页空闲链头，页头 `free_head` 指向它。
**分配**：先看页空闲链，再开新页。
空闲记录**立即复用**（Neo4j 同款）—— 这里有个必须记录的语义代价，见 §1.4。

**页类型**：新增 `PT_GRAPH_NODE` / `PT_GRAPH_EDGE`，
不进任何表的 freelist（对齐 v0.8 硬规则 6：模态各自管自己的页）。

### 1.4 事务语义：图不做 MVCC，走"锁 + WAL"

这是本版**最重要的设计判决**，因为图记录是**可变**的（blob 对象不可变），
SPI 的 `visible()` 回调必须有人回答"图行怎么判可见"。

三个候选：

| 候选 | 做法 | 判决 |
|---|---|---|
| (a) 图也上版本链（UPDATE = 插新版本） | 与 heap 同构 | **否决** —— 邻接链里的指针全要跟版本走（Neo4j 不这么做的原因就是指针稳定性），复杂度爆炸 |
| (b) **就地更新 + 恒可见 + 语句级提交** | `visible()` 恒返回 true；写走 WAL 整页镜像 | **采纳** |
| (c) 图模态只读 | 写路径 `AM_CAP_UPDATE = 0` | **否决** —— 图的写路径（边链双端维护）正是本版要练的东西 |

**判决 (b) 的完整语义**（对齐 Neo4j 的真实选择——它也是锁 + WAL，不做 MVCC）：

| 方面 | 语义 |
|---|---|
| 可见性 | `visible()` **恒真**（记录要么在用要么空闲，没有中间态给读者看） |
| 隔离 | **写者互斥**（全局 latch 保证）+ **读不加锁**。并发读写一个节点：读到旧值或新值，不会读到半写的值（WAL 整页镜像 + 8B 对齐写） |
| 提交 | 语句级（每条语句一个隐式事务）；图操作不参与 BEGIN/COMMIT 的多语句事务 |
| WAL | 每次改页 → `WAL_PAGEDATA` 整页镜像（blob 先例，恢复零改动）；**xid 传 0**（系统事务，永远视为已提交）—— SPI 的写接口强制带 xid，图模态传 0 是**合法值**而非绕过 |
| 崩溃 | 已写的图操作全保留（页镜像重放），不存在"未提交图数据要回滚"的问题（语句级粒度） |

**这是"模态各自管自己的语义"（硬规则 6）的又一次体现**：
heap 管 MVCC、blob 管不可变、graph 管"锁 + WAL"。
执行器三种都照单全收 —— 因为可见性判断本来就在 `am->visible()` 里。

**必须记录的代价**（写进 DESIGN.md 的风险清单）：
空闲记录立即复用 + 无版本链 = **读游标跨语句使用时可能看到记录被复用**
（旧游标拿着 RID，记录被删又建）。缓解：图游标只在单语句内有效（文档写死），
和 heap 游标跨语句失效是同一性质的约束。

### 1.5 遍历 API：3 跳怎么查

SQL 表达不了递归（§10.1 决定不做递归 CTE）。
遍历走 C API，**原语只有一个**：

```c
/* 沿出边走一步：给节点 RID，返回它的出边邻居 RID 数组 */
int db_graph_hop(DB *conn, db_graph_rid node,
                 db_graph_rid **out, uint32_t *n_out);

/* 反向：入边邻居（复用同一条链机制，方向参数区分） */
int db_graph_hop_in(DB *conn, db_graph_rid node,
                    db_graph_rid **out, uint32_t *n_out);
```

- 内部实现：读节点记录 → 沿 `first_out` 的边链逐个 `next` →
  每跳一次 = **1 次节点页读 + K 次边页读**（K = 出度，边记录命中同一页时免费）。
- 加锁：`db_graph_*` 是新入口路径 → **必须 latch**（硬规则 8）。
- 性能模型：3 跳 = 3 轮 hop，每轮邻居数 × 每邻居 1 页读（最坏）。
  100 万边 / 10 万节点 = 平均出度 10 → 3 跳最坏 1 + 10 + 100 + 1000 = 1111 次页读
  ≈ 全部命中缓冲池（256 帧，热点子图）→ **p99 < 10ms 可达**，
  但冷启动（缓冲池全 miss）单跳就是毫秒级 —— 基准要先"预热遍历一遍"再测 p99（写进验收方法）。

**SQL 侧**：`graph_nodes` / `graph_edges` 两张虚拟表照 blob 的模式
（`blob_frames` / `blob_objects`）注册，支持标量谓词下推：
- `graph_nodes`：按属性列下推（列值就在记录里，AM 逐行自滤 → 超集语义）。
- `graph_edges`：按 `src` / `dst` 等值下推（真下推：只枚举匹配的边链，不是全扫）。

### 1.6 验收

| # | 验收项 | 怎么验 |
|---|---|---|
| A1 | 图存储单元测试：建/删/遍历、空闲链复用、双向链一致性（沿出链走的集合 == 沿入链走的集合） | 单元测试 |
| A2 | **指针稳定性**：删中间节点后，其余节点的邻接链仍完整（删除只摘链 + 挂空闲链，不动别的边） | 单元测试 |
| A3 | 执行器零改动：`grep graph src/query/` 零命中；`grep blob src/query/` 仍然只有注释 | 机械检查 |
| A4 | 虚拟表：`SELECT ... FROM graph_nodes WHERE prop = X` 走下推；`graph_edges WHERE src = R` 只枚举对应链 | 集成测试 |
| A5 | 崩溃恢复：装载 10 万节点后模拟断电 → 恢复后节点数、边数、3 跳结果全对 | 集成测试（复用 test_crash 模式） |
| A6 | **性能**：10 万节点 / 100 万边，随机起点 3 跳 × 1000 次，p99 < 10ms（预热后） | bench/graph_bench.c |
| A7 | 旧的 152+ 项验收全绿 | `make test` |
| A8 | 依赖方向正确；新文件 < 800 行 | `make deps` + wc 检查 |
| A9 | 能力协商：图不支持的操作（如 `UPDATE graph_edges`）报 `E_NOTSUP` 系明确错误，不是崩溃 | 集成测试 |

### 1.7 风险清单

| # | 风险 | 应对 |
|---|---|---|
| R1 | **双向链维护出 bug**（摘链时 prev/next 指针漏改一边） | 单元测试 A2 的"双向一致性"断言：出链集合 == 入链集合，每次增删后都验 |
| R2 | **页内槽算术错**（rec_size 对齐、页头大小算错 → 越界写） | 页头大小进 `config.h` 单点定义；ASan 全程开 |
| R3 | **`visible()` 恒真被滥用**（把"还没写完的记录"放出去） | 分配记录时先 `in_use=1` + 全字段写完才挂链；WAL 先写后改页（纪律已有） |
| R4 | **3 跳 p99 不达标**（出度方差大，长尾节点邻居上千） | 预热 + 缓冲池命中是前提（写进验收方法）；不达标先查 IO 计数再谈优化 |
| R5 | 注入反向验证时编译不过 → 跑旧二进制 | 老规矩：先确认编译通过再跑（v0.7 起每次都踩） |
| R6 | graph_am 的 `decode` 要把定长记录转成 `DbValue[]` —— 列 offset 算错 | schema 声明时就算好 offset 表（和 heap 的 ColDesc 同构），单测覆盖 |

---

## 二、执行计划

分七步，每步可验证。**上一步不绿不做下一步**。

### 步骤 1 · 定长槽页原语

1. `src/modalities/graph/gstore.{h,c}`：页头、记录寻址算术、空闲链、`get/put` 记录。
2. 页类型 `PT_GRAPH_NODE` / `PT_GRAPH_EDGE` 进 `page.h` 枚举。
3. WAL：页改写走 `WAL_PAGEDATA` 钩子（blob 同款）。

**验证**：`test/unit/test_gstore.c` —— 寻址算术、空闲链复用、页满开新页。

### 步骤 2 · 图记录与邻接链

1. 节点/边记录布局（§1.2）、`graph_create_node / add_edge / remove_edge / remove_node`。
2. 双向链维护（出链 + 入链），删除摘链。

**验证**：A2 的双向一致性断言 + 指针稳定性断言（进 test_gstore）。

### 步骤 3 · graph_am（SPI 适配）

1. `spi/graph_am.{h,c}`：两张虚拟表 `graph_nodes` / `graph_edges`，
   `caps = AM_CAP_SCAN | AM_CAP_EXTERNAL?`（按实际能力定），
   `visible` 恒真，`insert` 支持节点/边，
   `update/remove` 返回 `E_NOTSUP`（本版只读语义先站住，写走专用 API）。
2. 下推：`graph_edges` 按 src/dst 等值（真下推），`graph_nodes` 按属性列（自滤超集）。

**验证**：`test/unit/test_graph_am.c` —— 扫描、下推行数 == 全扫过滤行数（超集断言）。

### 步骤 4 · 公开 API + 接线

1. `db_graph.{h,c}`：`db_graph_create/store/neighbors/hop*`，全部 latch。
2. REPL 命令：`GRAPH NODES / EDGES / HOP <rid>`（REPL 占位阶段简单实现）。
3. 注册表接入（blob 同款）。

**验证**：编译 + 152 项旧验收全绿（接线不能碰坏任何旧路径）。

### 步骤 5 · SQL 集成测试

`test/integration/test_graph_it.c`：
A3 / A4 / A9 三项 + 能力协商错误信息检查。

### 步骤 6 · 崩溃恢复 + 性能基准

1. `test/integration/test_graph_crash.c`：装载 → 断电 → 恢复 → 全量校验（A5）。
2. `bench/graph_bench.c`：10 万节点 / 100 万边装载，预热，随机 3 跳 × 1000 次 → p99（A6）。

### 步骤 7 · 反向验证 + 文档

1. 注入 1：摘链时**漏改入链的 prev** → A2 必须红。
2. 注入 2：下推的边枚举**漏掉边链最后一跳** → 下推行数 < 全扫行数 → A4 必须红。
3. `DESIGN.md`（§5.8 图模态、路线图、模块表）、`README.md`、`PLAN-v0.11.md` §3、workspace 记忆。

---

## 三、执行记录

### 步骤 1 · 定长槽页原语 ✓

`src/modalities/graph/gstore.{h,c}`。页头 20B、定长槽纯算术寻址、空闲链、
`get/put/free`、WAL_PAGEDATA 整页镜像、链头标志位 + 扫全文件恢复。

**验收**：`test/unit/test_gstore.c` —— 8 项全过。

**顺带修的既有 bug**：`gs_log_page` 硬编码 page_id=0（已修）。

### 步骤 2 · 图记录与邻接链 ✓

`src/modalities/graph/graph.{h,c}` + `graph_prop.c`。
节点/边记录布局、双向链的挂链/摘链、属性定长编解码。

**验收**：`test/unit/test_graph.c` —— 8 项全过（含 A1 双向一致性、A2 指针稳定性）。

**顺带修的既有 bug**（3 个）：

| # | bug | 后果 |
|---|---|---|
| 1 | `gr_calc_schema` 里 `bitmap_bytes` 被重复计入列偏移 | 列之间多出空洞、记录总长虚高，合法 schema 可能被 `GR_PROP_MAX` 误拒 |
| 2 | `gr_calc_schema` 未限制 TEXT 宽度 ≤ 255 | 值层装不下（`VAL_TEXT_MAX`），写入时静默截断 = 数据错误 |
| 3 | `gstore_get` 未校验页类型 | 拿边页 RID 去节点 gstore 读，会按节点的记录大小解释边页字节 → 读出垃圾且不报错 |

### 步骤 3 · graph_am（SPI 适配）✓

**落盘位置偏离了 PLAN 原文**：PLAN 写 `spi/graph_am.{h,c}`，实际放在
`src/modalities/graph/graph_am.{h,c}`。

原因：spi/ 是 L3，modalities/ 是 L4。`spi/` 依赖 `modalities/` 是**反向依赖**
（storage_am.h 的依赖方向一节明写"spi/ 也不许依赖 modalities/ 里的任何东西"）。
blob_am 之所以能住在 spi/，是因为 blob 的存储在 storage/（L1），spi/ → storage/ 合法；
图的存储在 modalities/graph/，所以图的适配器必须跟着住在 modalities/graph/。

**文件**：

| 文件 | 行数 | 内容 |
|---|---|---|
| `graph_am.h` | 210 | AM 私有状态、行格式、扫描模式、内部结构 |
| `graph_am.c` | 581 | 构造 / 读路径 / 写路径 |
| `graph_am_push.c` | 300 | 下推（can_push / push / ga_match） |
| `gstore_cursor.c` | 123 | 拉式游标（gstore.h 早有声明，一直没实现） |
| `graph_cursor.c` | 107 | 邻接链增量游标（graph.h 早有声明，一直没实现） |

**两张虚拟表**：

- `graph_nodes`：`node_id`（INT8，可空 —— 由存储层分配）+ 动态属性列
- `graph_edges`：`src` / `dst`（INT8，都是**节点 id** = RID 的 u64 编码）

`src`/`dst` 用节点 id 而不是槽号，是为了让 `WHERE src = X` 能反解出节点 RID
→ 沿它的出链走 —— 这是"真下推"的前提。

**行格式**（模态私有，不复用 record 格式）：
- 节点行 = `[u64 node_id][节点记录原样]`
- 边行 = `[u64 src][u64 dst]`

**能力位**：`SCAN | INSERT | PUSHDOWN`。update/remove/vacuum/reindex → `E_NOTSUP`。
写路径的 WAL xid 传 0（系统事务，图走语句级提交，不参与 BEGIN/COMMIT）。

**下推**：
- `graph_edges` 的 src/dst 等值 → **真下推**（沿邻接链，只读链上那几条）
- `graph_nodes` 的 node_id 等值 → 直接定位一个节点
- `graph_nodes` 的属性列 → **自滤**（比较语义用 `ga_match` 逐字复刻执行器的 `eval_expr`）

**验收**：

| 测试 | 项数 | 验什么 |
|---|---|---|
| `test/unit/test_graph_am.c` | 8 | 正确性：schema / 扫描 / get 往返 / **下推行数 == 全扫过滤行数** / 锚点不存在 → 0 行 / 能力协商 / insert |
| `test/unit/test_graph_am_push.c` | 1 | **效率**：下推只读了 1 条记录，全扫读了 39 条 |

**为什么效率要单独一个测试文件**：

"下推行数 == 全扫过滤行数"这条断言**验不出真下推** —— 一个写成"全扫 + 过滤"的
实现返回的行集和真下推完全一样，断言照样通过。所以效率必须用另一个可观测量
来验：**读了多少条记录**。为此在 `GStore` 里加了 `rec_reads` 计数器
（`gstore_read_count()`）。

判据从紧带来的一个直接后果：自滤的**比较语义必须和执行器逐字一致**，
否则就是静默漏数据（`ga_match` 就是照 `exec_scan.c` 的 `eval_expr` 抄的）。

**验收结果**：39 个测试文件全 PASS / 0 FAIL；`make deps` 通过；
`grep -rn graph src/query/` 零命中（执行器零改动，A3 ✓）；所有新文件 < 800 行。

### 步骤 4 · 公开 API + 接线 ✓

**落盘的文件**：

| 文件 | 行数 | 内容 |
|---|---|---|
| `include/db.h` | +150 | 图模态的公开 API（类型 + 13 个函数） |
| `src/db_graph.c` | 593 | 公开 API 实现（建图 / 节点 / 边 / 枚举 / 统计 / schema） |
| `src/db_graph_walk.c` | 316 | 遍历原语（跳一步 + BFS 多跳 + 开放寻址哈希集） |
| `src/db_graph_internal.h` | 72 | 两个文件共享的内部定义（RidVec / dg_hop_locked） |
| `src/modalities/graph/graph_schema.c` | 241 | **属性 schema 的落盘 / 读回**（新增） |
| `src/modalities/graph/graph_edge.c` | 300 | 边链的挂链 / 摘链（从 graph.c 拆出） |
| `src/modalities/graph/graph_walk.c` | 85 | 沿链读一遍（从 graph.c 拆出） |
| `src/main.c` | +330 | REPL 的 GRAPH 命令 |
| `test/integration/test_graph_api.c` | 380 | 8 项公开 API 端到端验收（新增） |

**关键决策 1 · 属性 schema 必须落盘（PLAN 原文没写，是这一版最大的缺口）**

节点记录是**定长**的：长度 = 公共头 + NULL 位图 + 各列宽度之和。
记录里**没有**"这一列多宽"的信息 —— 宽度只存在于 schema 里。

所以：不落盘 = 重启后整张图变成一堆看不懂的字节，
连节点数都数不出来（`gstore_recover` 会先因为 `rec_size` 对不上而 E_CORRUPT）。
A5（崩溃恢复）的前提就是 schema 能被找回来。

落盘方式选的是**第三个 gstore**（页类型 `PT_GRAPH_SCHEMA`），理由是零新代码：
页分配 / WAL 整页镜像 / 重启接管（扫全文件找 `GS_FLAG_DIR` 链头）
全是 gstore 现成的能力。

排除的两条路：
- 塞进节点记录 → 每条记录存一份 schema = 纯浪费，且改 schema 会改所有记录长度
- 塞进 catalog → 硬规则 6：catalog 不知道任何模态

**schema 是图的身份证，不是可改的元数据**：
磁盘上已有 schema 且与传入的**逐列相同** → 接管这张图；
不同 → E_INVAL（记录布局不同，旧记录没法解释）。
只比 `node_size` 是不够的 —— `INT4+INT4` 和 `INT8` 长度一样，布局完全不同。

**关键决策 2 · 公开 API 的形态**

PLAN §1.5 写的是 `db_graph_hop(conn, node, db_graph_rid **out, uint32_t *n_out)`，
即**堆上分配**。采纳 PLAN 的写法，另配 `db_graph_free()` 释放。

理由：出度事先未知。给调用方一个固定缓冲意味着"要么先问一次度数，
要么给个猜的容量然后重试" —— 后者把重试的复杂度推给了每一个调用方。

比 PLAN 多加的：`db_graph_walk(node, depth, ...)` —— BFS 多跳。
"3 跳"是 A6 的验收口径，BFS 必须**去重**（菱形图里同一个节点会从多条路径到达，
不去重则跳的次数被算错）。为此在 `db_graph_walk.c` 里写了一个开放寻址的
u64 哈希集（库里没有现成的，排序数组去重是 O(n²)，比遍历本身还慢）。

**关键决策 3 · 注册表的接入时机**

`db_open` 时扫磁盘：有 schema → 接管 + 注册 `graph_nodes` / `graph_edges`；
没有 → **不注册**，于是查这两张表会得到"表不存在"。

不给空表是有意的：空表会让人以为"图是空的"，
而真相是"这个库压根还没有图" —— 这两件事必须能区分开。

**REPL 命令**（`GRAPH` 前缀，和 `BLOB` 同类）：

```
GRAPH CREATE <列定义>   建图，列定义形如 id:INT4!, name:TEXT(16), score:INT8
GRAPH SCHEMA            看属性 schema
GRAPH NODES / EDGES     列出所有节点 / 边
GRAPH HOP <rid>         跳一步（出邻居）
GRAPH HOPIN <rid>       跳一步（入邻居）
GRAPH STAT              统计
```

**顺带修的 3 个 bug**（都是这一步自己写出来的、当场发现）：

| # | bug | 后果 |
|---|---|---|
| 1 | `gr_schema_read` 把游标结束的 `E_EOF` 原样返回 | `graph_open` 把"没有图"误判成错误 → **库打不开**（`db_open` 直接失败） |
| 2 | schema 记录从第 0 字节开始存 | `gstore_alloc_with` 只拷**载荷区**（跳过 4 字节公共头）→ 前 4 字节被静默丢掉 → 重启后 schema 解不出来，报"数据损坏" |
| 3 | REPL 的类型扫描没把 `\r` / `\n` 当结束符 | 最后一列的类型被读成 `"INT8\n"` → 建图永远失败 |

bug 2 值得单独记一笔：**它写的时候不报错，读的时候才发现**。
定长槽页的"载荷从第 4 字节开始"这个约定写在 gstore.h 里，
但写 schema 的人如果没读那段，就会把它当成"整条记录"。

**验收结果**：40 个测试文件全 PASS / 0 FAIL；`make deps` 通过；
`grep -rn graph src/query/` 零命中（执行器零改动，A3 ✓）；
所有新增 / 改动的文件 < 800 行。

### 步骤 5 · SQL 集成测试 ✓

`test/integration/test_graph_it.c` —— 6 项：

| 项 | 验什么 |
|---|---|
| A3 | 执行器零改动（机械检查：扫 `src/query/*.c` 有没有 "graph"） |
| A4a | `graph_nodes` 的属性列过滤：**下推结果 == 全扫过滤结果**（超集语义） |
| A4b | `graph_nodes` 的 node_id 等值 → 只返回那一个节点 |
| A4c | `graph_edges` 按 src / dst 下推：**只读链上那几条**（读计数证明） |
| A9 | UPDATE / DELETE / VACUUM / CREATE INDEX 明确报错，不是崩溃 |
| — | INSERT：node_id 必须由存储层分配（用户不能指定） |

**A3 为什么用机械检查**：这件事没法用行为断言表达 ——
行为对了不代表执行器没被改。所以只能查源码。

**A4c 是这一版的重点**："下推"光看查询结果**验不出来** ——
一个写成"全扫 + 逐行过滤"的实现，返回的行集和真下推完全一样。

所以必须用**可观测量**区分：`GStore.rec_reads`。
真下推只读匹配链上的那几条（= 锚点的度数），全扫读了全表每一条。

实测（100 条边，hub 的出度 30）：

```
（下推读了 30 条记录，全表 100 条）
```

**新加的一个公开可观测量**：`db_graph_reads(db, &node_reads, &edge_reads)`。
和 `db_blob_page_reads` 同一个思路 —— 用可观测量证明"少读了"。

### 步骤 6 · 崩溃恢复 + 性能基准 ✓

**A5 · `test/integration/test_graph_crash.c`（4 项）**

| 项 | 验什么 |
|---|---|
| 1 | 装载 → 断电 → 重开：节点数、边数全对 |
| 2 | 断电后：每条边的两个端点全对（邻接链指针没丢） |
| 3 | 断电后：**3 跳的结果集和断电前完全一致** |
| 4 | 多轮：每轮加一批再断电，累加后全对 |

第 3 项最难也最值钱：节点数、边数只是**计数**，
而邻接链指针是**图的结构**（first_out / first_in / prev / next 全在页里）。
"3 个节点 2 条边"看着没问题，但一跳跳到空 —— 这是最容易漏的失败模式。

**A6 · `bench/graph_bench.c`**

10 万节点 / 100 万边 / 3 跳 / 1000 轮：

| 指标 | 结果 | 目标 |
|---|---|---|
| p50 | 0.177 ms | — |
| p90 | 0.305 ms | — |
| **p99** | **0.564 ms** | **< 10 ms** ✓ |
| max | 4.225 ms | — |
| 每次平均读 | 1104 条边 | PLAN 模型 1111 ✓ |

缓冲池的对照实验（1 万节点 / 10 万边）：

| 缓冲池 | 装载 | p99 | 说明 |
|---|---|---|---|
| 256 帧（1MB，库的默认） | 121 s（824 边/秒） | 5.39 ms | 工作集装不下，每次未命中都要回读 |
| 32768 帧（128MB） | 5.4 s（18558 边/秒） | 0.14 ms | 全部命中 |

**两个必须记录的结论**：

1. **基准在"模态层"跑，不挂 WAL 钩子**。
   因为加一条边要改 4 个页 = 4 条 WAL 整页镜像 = 32KB 日志，
   100 万条边 ≈ **32GB**。那不是"慢一点"，是磁盘放不下。
   持久化路径由 `test_graph_crash.c` 单独验。
   公开 API 在这条路径上只多两样东西：每跳一次 malloc + BFS 去重哈希集。

2. **基准测试找到了真正的瓶颈：CRC**。
   `common/crc.c` 原来是逐位算的（每字节 8 次移位异或），
   页校验和 4KB = 32768 次位运算，WAL 记录 8KB = 65536 次。
   而加一条边要改 4 个页 → 每条边约 26 万次位运算。

   crc.c 原来的注释写着"等 bench 显示它成为瓶颈再换成查表版"——
   现在就是那个时候。**换成查表版，装载快 3 倍**（145µs/边 → 48µs/边）。

   换的时候**值一个都不能变**（页校验和在磁盘上，值变了老库全废）。
   所以新加了 `test/unit/test_crc.c`：里面保留一份**逐位参考实现**，
   逐字节比对两者的输出，再加标准检验向量
   （`"123456789"` → crc16 = 0x29B1、crc32 = 0xCBF43926）。
   把"值不变"从"我相信没变"变成"测过没变"。

### 步骤 7 · 反向验证 + 文档 ✓

**注入 1 · 摘链时漏改入链的 prev**

```c
/* graph_edge.c · gr_unlink_edge */
-   gr_rid_set(nbuf2, GR_E_IN_PREV, in_prev);
+   /* 【反向验证 · 注入 1】故意漏改后继的 in_prev */
```

结果：`test_graph` **变红**（`graph_remove_edge` 失败 → A1/A2 双向一致性破）。

**注入 2 · 下推的边枚举漏掉边链最后一跳**

```c
/* graph_cursor.c · graph_edge_cursor_next */
    u64 nxt = get_u64(rec + noff);
+   /* 【反向验证 · 注入 2】故意漏掉链上的最后一条边 */
+   if (nxt == GR_RID_NIL)
+       return E_EOF;
```

结果：**三个测试文件同时变红**：

| 测试 | 失败的断言 |
|---|---|
| `test_graph_am` | `pushed == full`（下推行数 == 全扫行数） |
| `test_graph_am_push` | `pushed == 1`（下推效率） |
| `test_graph_it` | `n == HUB_DEG`（A4） |

两次注入都**先确认编译通过再跑**（v0.7 起每次都踩"跑的是旧二进制"的坑），
撤销后重跑：**43 个测试文件全 PASS / 0 FAIL**。

**文档**：`DESIGN.md`（新增 §5.8 图模态、路线图状态、风险清单 4 条、模块表）、
`README.md`（目录布局、图模态一节、调试记录）、本文件 §3、workspace 记忆。

**最终验收**：43 个测试文件全 PASS / 0 FAIL；`make deps` 通过；
`grep -rn graph src/query/` 零命中（A3 ✓）；所有新增 / 改动的文件 < 800 行。
