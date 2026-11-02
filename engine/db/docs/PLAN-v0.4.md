# v0.4 执行方案与计划 · 查询层

> 目标：`db.exe` 打开一个文件，能建表、插数据、按条件查、改、删。
> 这是第一次让用户真的输入 SQL。
>
> 状态：方案已定，执行中

---

## 一、方案

### 1.1 范围

| 模块 | 文件 | 职责 |
|---|---|---|
| 值类型 | `query/value.{h,c}` | 自包含的运行时值 |
| 词法 | `query/lexer.{h,c}` | 字符流 → Token |
| 语法 | `query/parser.{h,c}` | Token → AST（递归下降）|
| 执行 | `query/exec.{h,c}` | AST + catalog → 结果集 |
| 公开 API | `db.c` | 实现 `include/db.h` |
| REPL | `main.c` | 读 SQL、打印结果 |

**SQL 子集**（DESIGN.md §10.1）：`CREATE TABLE` / `INSERT` / `SELECT ... WHERE ... LIMIT` / `UPDATE` / `DELETE`。

**不做**：JOIN、聚合、ORDER BY、子查询、索引（v0.5 接 btree）。

### 1.2 五个决定

#### A · 结果集物化，不做流式

`SELECT` 一次扫完、过滤、投影，把所有行放进内存，再交给上层打印。

**理由**：单表查询、有 LIMIT 兜底，内存可控。而且 ORDER BY 以后加进来时，物化后排序是两行代码；流式就得引入外部排序。

**代价**：`SELECT * FROM 大表` 会吃内存。v0.4 不解决，加 LIMIT 就是。

#### B · 值类型自包含，不用 malloc

```c
typedef struct {
    bool is_null;
    u8   type;
    i64  i;              /* INT4 / INT8 / BOOL 共用 */
    u16  len;
    char s[256];         /* TEXT 内联 */
} DbValue;
```

**理由**：值在表达式求值、结果集、临时数组之间到处传。用指针 + malloc 就得管生命周期，一处漏 free 就是内存泄漏或野指针。定长内联，`memcpy` 即拷贝，零心智负担。

**代价**：每值 272 字节，TEXT 最长 255。够用。

#### C · 不做计划树，直接"绑定 + 执行"

单表查询不需要 Join 顺序、不需要算子树。绑定只做一件事：**把列名换成列号**（写回 `Expr.col_idx`）。

**理由**：计划树是给多表连接和代价优化用的。现在建树，等加 JOIN 时会发现树的设计是错的，白写。

#### D · UPDATE / DELETE 先收集 RID 再改

扫描时不能边扫边改：`heap_update` 变大时会换 RID，`heap_delete` 会让 slot 失效，都会打乱扫描器状态。

做法：扫一遍把 `(RID, 新记录)` 收集到数组，扫完再逐个应用。

#### E · NULL 用三值逻辑

`WHERE` 只接受 `TRUE`，`FALSE` 和 `NULL` 都当不匹配。

比较运算任一操作数为 NULL → 结果为 NULL。`AND`/`OR` 按 SQL 三值表。

---

## 二、执行计划

| 步 | 任务 | 产出 |
|---|---|---|
| 1 | `value.h/c` + `lexer.h/c` | 约 320 行 |
| 2 | `parser.h/c` | 约 450 行 |
| 3 | `exec.h/c` | 约 500 行 |
| 4 | `db.c` + `main.c`（REPL） | 约 250 行 |
| 5 | `test/unit/test_query.c` | 约 350 行 |

## 三、验收

| 项 | 标准 |
|---|---|
| 建表 | `CREATE TABLE` 后重开还在 |
| 插入 | 单行 / 多行 `VALUES` |
| 查询 | 投影、`WHERE` 比较、`AND/OR/NOT`、`LIMIT` |
| NULL | `IS NULL` / 比较返回 NULL / 三值逻辑 |
| 更新 | `UPDATE ... SET ... WHERE` |
| 删除 | `DELETE ... WHERE` |
| 错误 | 表不存在 / 列不存在 / 语法错 / 类型不匹配都返回错误码不崩溃 |
| 对照 | 与 SQLite 跑同一批语句结果一致 |

## 四、执行记录

### 全部完成 ✅

| 文件 | 行数 |
|---|---|
| `query/value.h/c` | 52 + 190 |
| `query/lexer.h/c` | 40 + 200 |
| `query/parser.h/c` | 86 + 560 |
| `query/exec.h/c` | 38 + 620 |
| `db.c`（公开 API 实现） | 210 |
| `main.c`（REPL） | 130 |
| `test/unit/test_query.c` | 210 |

**9 项验收全过。加上前三个版本，累计 67 项验收 + 2 项端到端全部通过。**

### 现在能干什么

```
$ ./db.exe test.db < script.sql

CREATE TABLE users (id INT NOT NULL, name TEXT, age INT);
INSERT INTO users VALUES (1, 'alice', 30), (2, 'bob', 25), (3, NULL, 40);
SELECT * FROM users;
id  name   age
--  -----  ---
1   alice  30
2   bob    25
3   NULL   40
(3 行)

SELECT id, name FROM users WHERE age > 28 AND id < 3;
id  name
--  -----
1   alice
(1 行)
```

### 踩的坑

**① `LIMIT 0` 还是吐了一行（真 bug）**

检查写在了 `b_push` 之后：

```c
rc = b_push(&b, proj);
if (st->limit >= 0 && b.r->nrows >= st->limit) stopped = 1;   /* 太晚 */
```

`limit=0` 时第一行已经 push 进去了才判断。改成 push **之前**检查。

**② 两处测试期望写错（不是代码问题）**

- `UPDATE ... WHERE age > 45` 我写期望 3 行，实际是 4 行（99/50/60/70）——自己数错了。
- 测 NOT NULL 约束时写了 `VALUES (9, 'x', NULL)`，但 `id` 传的是 9 不是 NULL，这条本来就该成功。

**教训**：测试失败先分清"代码错"还是"期望错"。这次 4 个失败里有 3 个是期望错——如果直接去改代码，会把正确的逻辑改坏。

### 明确没做的

| 不做 | 原因 |
|---|---|
| JOIN | 需要连接顺序，属 CBO 范畴（DESIGN.md §10.1）|
| 聚合 / GROUP BY | 单独一版，要引入累加器和 hash 分组 |
| ORDER BY | 物化后排序很简单，但想和聚合一起做 |
| **用索引** | 优化器还不选索引，`WHERE id = 1` 也是全表扫 —— v0.5 接 btree |
| 事务 | 每条语句自动提交，`BEGIN/COMMIT` 还没实现 |

**其中"用索引"是下一步最该补的**：btree 已经写好并测过，但还没有任何路径把 `WHERE` 谓词转成 `btree_search`。这是 v0.5 的主线。

### 一个设计上的观察

执行器里 `collect_matches` 被 UPDATE 和 DELETE 共用，都是"先扫一遍收集 RID，再逐个改"。这个模式在加索引后会变成"先收集候选 RID，再回表"，结构不用变——算是提前押对了。
