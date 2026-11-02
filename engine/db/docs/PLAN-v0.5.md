# v0.5 执行方案与计划 · 索引接入

> btree 已经写好并测过 8 项，但**没有任何代码路径用它**——`WHERE id = 1` 现在也是全表扫。
> 这一版把"索引"从一堆代码变成真能用上的东西。
>
> WAL/recovery 推到 v0.6：两件事都做会顾此失彼。

---

## 一、方案

### 1.1 范围

| 模块 | 改动 |
|---|---|
| `catalog` | 新增索引元数据：sys_index / sys_index_col 的读写、内存结构、加载 |
| `query/parser` | 新增 `CREATE INDEX name ON table (col)` |
| `query/exec` | 谓词提取 → 选索引 → IndexScan；写路径维护索引 |
| `query/index_key` | key 编码（大端 + 符号位翻转）|

### 1.2 四个决定

#### A · 只做单列索引

`CREATE INDEX idx ON t (col)`。复合索引留到以后。

**理由**：复合索引的 key 拼接、前缀匹配、范围边界都要单独设计，而它带来的收益（覆盖更多查询）在当前阶段看不到。先把单列走通，架构上留好位置（`sys_index_col` 已经有 `seq` 字段）。

#### B · key 编码：大端 + 符号位翻转

`memcmp` 比较 key，所以整数必须编成"字节序 = 数值序"：

```c
/* i32 → 4 字节，翻转符号位让负数排在正数前面 */
u32 u = (u32)v ^ 0x80000000u;
b[0] = u >> 24; b[1] = u >> 16; b[2] = u >> 8; b[3] = u;
```

**不翻转符号位的话，`-1`（`FF FF FF FF`）会比 `1`（`00 00 00 01`）大**——索引区间查询全错。

TEXT 直接用原字节，变长 key 的 memcmp 天然符合字典序。

#### C · 谓词提取只认"列 与 常量"的比较

能用的形式：
- `col = const` → 精确查找 `btree_search`
- `col > const` / `>=` / `<` / `<=` → 范围扫描 `btree_scan`

`A AND B` 时取第一个能用的做索引访问，**剩下的仍然当过滤器**（不做索引合并）。

`IS NULL` 不走索引（索引只存非 NULL 值）。

**理由**：索引合并、多条件联合索引是优化器的活，当前没有 CBO。先用"索引缩范围 + 剩余条件过滤"，正确性有保证，收益也已经很大。

#### D · 写路径必须维护所有索引

`INSERT` / `UPDATE` / `DELETE` 都要同步索引，否则索引和堆表不一致——**而且查询会静默返回错数据**（比崩溃更难查）。

- INSERT：`heap_insert` 拿到 RID 后，对每个索引 `btree_insert(key, rid)`
- DELETE：先按旧行算出 key，`btree_delete(key, rid)`，再 `heap_delete`
- UPDATE：一律"删旧 + 插新"（RID 可能变，key 也可能变）

### 1.3 执行路径

```
SELECT ... WHERE id = 5
   ↓ 谓词提取
IndexChoice{col_idx=0, op='=', val=5, rest=NULL}
   ↓ 找到 id 上的索引
encode_key(5) → 4 字节大端
   ↓
btree_search(key) → RID[] → heap_get(RID) → 行
   ↓ rest 过滤（这里为空）
project
```

范围查询走 `btree_scan(lo, hi)`，游标吐 RID。

### 1.4 验收

| 项 | 标准 |
|---|---|
| CREATE INDEX | 建完能查到索引元数据，重开还在 |
| 建索引时已有数据 | 建索引后能立刻查到旧行（说明 build 正确）|
| 等值查询 | `WHERE id = N` 结果正确 |
| 范围查询 | `WHERE id > N` / `BETWEEN`（用 >= 和 <=）结果正确 |
| 索引 + 剩余条件 | `WHERE id > 10 AND name = 'x'` 正确 |
| 负数 key | `WHERE id = -5` 正确（符号位翻转验证）|
| TEXT 索引 | 字符串等值与范围正确 |
| 写路径一致 | INSERT/UPDATE/DELETE 后索引仍能查到最新数据 |
| 无索引时 | 仍然走全表扫，结果不变 |

## 二、执行计划

| 步 | 任务 |
|---|---|
| 1 | catalog 索引元数据（读/写/加载）|
| 2 | parser 支持 `CREATE INDEX` |
| 3 | `index_key` 编码 + 谓词提取 |
| 4 | exec：IndexScan + 写路径维护 |
| 5 | `test/unit/test_index.c` |

## 三、执行记录

### 全部完成 ✅

| 文件 | 改动 |
|---|---|
| `catalog/catalog.{h,c}` | +约 260 行：索引元数据读写、加载、建索引 |
| `query/parser.{h,c}` | +约 50 行：`CREATE INDEX`、一元负号 |
| `query/index_key.{h,c}` | 新增 130 行：key 编码（大端 + 符号位翻转）|
| `query/exec.c` | +约 300 行：谓词提取、索引扫描、写路径维护 |
| `test/unit/test_index.c` | 新增 210 行 |

**9 项验收全过。累计 76 项验收 + 2 项端到端全部通过。**

### 怎么证明"真的走了索引"

不能只看结果对不对——全表扫和索引扫结果一样。**用返回顺序**：

```
插入顺序：5, 3, 1, 4, 2
无索引：SELECT id FROM t WHERE id > 0  →  5|3|1|4|2   （全表扫，插入顺序）
建索引：SELECT id FROM t WHERE id > 0  →  1|2|3|4|5   （索引扫，key 顺序）
```

这个技巧抓出了下面那个最隐蔽的 bug。

### 三个坑

**① 一元负号没支持（真 bug，直接挂掉整条语句）**

`INSERT INTO t VALUES (1,'a',-5)` 整条语句解析失败——`parse_primary` 只处理字面量，遇到 `-` 就报错。

修法：在 `parse_primary` 里处理 `-`，产出 `EX_UN(NEG)` 节点。

**② 负数字面量导致索引被静默绕过（最隐蔽）**

`-10` 在 AST 里不是 `EX_LIT`，而是 `EX_UN(NEG) 包着 EX_LIT(10)`。而谓词提取只认：

```c
if (l->kind != EX_COL || r->kind != EX_LIT) return false;
```

于是**所有带负数的条件都退回了全表扫**——结果完全正确，只是没用上索引。

只有"返回顺序"测试能发现：`WHERE score > -10` 返回 `5|3|4|2`（插入顺序）而不是 `2|3|4|5`（key 顺序）。

修法：加 `as_const()` 做常量折叠，认 `EX_UN(NEG, EX_LIT)`。

**这个坑的教训**：性能退化不会报错。必须有"能观察执行路径"的测试，否则"功能正常"会掩盖"优化没生效"。

**③ 我自己的操作失误**

同一个 SQL 脚本在同一个文件上跑了两遍，`INSERT` 重复执行导致结果翻倍，一度以为是索引返回重复行。教训：测试脚本要么用干净文件，要么脚本本身可重入。

### 现在的查询路径

```
SELECT ... WHERE score > -10
   ↓ 谓词提取（as_const 折叠 -10）
IdxChoice{col=score, op='>', val=-10}
   ↓ index_for_col 找到 idx_score
encode_key(-10) → 7F FF FF F6（大端 + 符号位翻转）
   ↓ btree_scan(lo=7FFFFFF6, hi=NULL)
游标按 key 升序吐 RID
   ↓ 每个 RID 回表 heap_get
重新求值完整 WHERE（闭区间越界的行在这里滤掉）
   ↓
project
```

**边界一律用闭区间，越界的行靠重新求值 WHERE 滤掉。** 这样 `>` 和 `>=` 共用一套代码，不用为开闭区间写两套边界逻辑。

### 明确没做的

| 不做 | 原因 |
|---|---|
| 复合索引 | key 拼接、前缀匹配、范围边界都要单独设计；`sys_index_col.seq` 字段已留好位置 |
| 唯一索引 | `is_unique` 字段写了但没实现约束检查 |
| 索引合并 | 多个条件各走一个索引再求交，属优化器范畴 |
| 覆盖索引 | 只从索引拿 RID，必须回表 |
| 索引统计信息 | 优化器还不会"选"索引——有就用，没有就全表扫 |

**"有就用"是个简化**：即使 `WHERE id > 0` 命中全表 99% 的行，也照样走索引。等 v0.7 有了 `nrows_est` 和选择率估算，才谈得上"选不选"。
