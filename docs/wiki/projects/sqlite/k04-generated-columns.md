---
project: sqlite
feature: generated-columns
page_type: feature
schema_version: 1
refs:
  - path: src/insert.c
    symbols: [sqlite3ComputeGeneratedColumns]
  - path: src/expr.c
    symbols: [sqlite3ExprCodeGeneratedColumn]
---

# 生成列

## 一句话职责

生成列负责在写入路径上按其 AS 表达式求出该列的值，并把求值时机与普通列、rowid、触发器的先后关系固定下来。

## 机制是什么

生成列（generated column）是用 `GENERATED ALWAYS AS (expr)` 或 `AS (expr)` 声明的列，取值来自同一行内其它列的表达式。SQLite 把生成列分成两类，用列标志位区分：

- `COLFLAG_VIRTUAL`（值为 `0x0020`）：只在读取时计算，不占数据库文件空间。
- `COLFLAG_STORED`（值为 `0x0040`）：在行写入时计算，结果随行一起存进记录。
- `COLFLAG_GENERATED`（值为 `0x0060`）是前两者的组合掩码，判断「这一列是不是生成列」用它。
- `COLFLAG_NOTAVAIL`（值为 `0x0080`）标记 STORED 列尚未算出；`COLFLAG_BUSY`（值为 `0x0100`）用于阻断生成列之间的递归求值。

表级标志位与列标志位一一对应：`TF_HasVirtual`（`0x00000020`）等于 `COLFLAG_VIRTUAL`，`TF_HasStored`（`0x00000040`）等于 `COLFLAG_STORED`，`TF_HasStored` 与 `TF_HasVirtual` 按位或得到 `TF_HasGenerated`（`0x00000060`）。`sqlite3ComputeGeneratedColumns` 的入口断言就是 `pTab->tabFlags & TF_HasGenerated`，没有该标志的表根本不会进入这条路径。

生成列的表达式存在表默认表达式列表里：`sqlite3ColumnExpr` 依据列的 `iDflt` 下标从 `pTab->u.tab.pDfltList` 里取出表达式，所以生成列与 DEFAULT 子句共用同一套表达式存储与代码生成入口。

物理布局上，`sqlite3TableColumnToStorage` 把 VIRTUAL 列从记录中剔除：它把列序号映射成记录里的存储位置，VIRTUAL 列排在所有非虚拟列之后（`pTab->nNVCol + i - n`），非虚拟列的位置是它前面非虚拟列的个数 `n`。因此记录里只保存普通列与 STORED 列，VIRTUAL 列在读路径上现算。

本页只讲生成列的求值时机与调用顺序，不讲表达式的代码生成本身（归表达式代码生成那一组），也不讲视图。

## 怎么实现的

### 写入路径：`sqlite3ComputeGeneratedColumns`

`sqlite3ComputeGeneratedColumns(Parse *pParse, int iRegStore, Table *pTab)` 的前提是：表的普通列已经放进从 `iRegStore` 开始的连续寄存器，而 STORED 与 VIRTUAL 列对应的寄存器还没有初始化。函数按下列顺序工作：

1. **先补做普通列的 affinity**。调用 `sqlite3TableAffinity(pParse->pVdbe, pTab, iRegStore)`，把普通列按列定义的 affinity 规范化。
2. **如果有 STORED 列，修正刚生成的 affinity / 类型检查指令**。取最后一条指令 `sqlite3VdbeGetLastOp`：
   - 若是 `OP_Affinity`，把其 P4 字符串中对应 STORED 列的字符改写成 `SQLITE_AFF_NONE`。源码注释说明 `@` 是「no-op affinity」，因为这些列还没算出来，先不做 affinity 转换，等算完再单独处理。VIRTUAL 列在这一步直接从字符序列里跳过（`COLFLAG_VIRTUAL` 分支 `continue`），所以字符计数与列序号的计数 `jj` 是分开的。
   - 若是 `OP_TypeCheck`（STRICT 表由 `sqlite3TableAffinity` 生成的就是这一条），把其 P3 操作数置为 `1`，源码注释写明这是表示「生成列不参与类型检查」。
3. **第一遍：把所有生成列标记为尚不可用**。遍历 `pTab->aCol`，凡是带 `COLFLAG_GENERATED` 的列都加上 `COLFLAG_NOTAVAIL`。
4. **第二遍：反复迭代直到没有进展**。设置 `pParse->iSelfTab = -iRegStore`，这个负值告诉表达式代码生成器「本行的列值在寄存器里，列 `i` 对应寄存器 `sqlite3TableColumnToStorage(pTab, i) + iRegStore`」，而不是从游标读。对每个带 `COLFLAG_NOTAVAIL` 的列：
   - 先给它打上 `COLFLAG_BUSY`，然后用 walker `exprColumnFlagUnion` 遍历该列的表达式，把表达式里引用到的所有列的 `colFlags` 按位或到 `w.eCode` 上。
   - 如果 `w.eCode` 里带了 `COLFLAG_NOTAVAIL`，说明这个生成列依赖的另一个生成列还没算出来，本轮跳过，记下 `pRedo` 待重试。
   - 否则算出目标寄存器 `x = sqlite3TableColumnToStorage(pTab, i) + iRegStore`，调用 `sqlite3ExprCodeGeneratedColumn` 生成求值代码，并清掉该列的 `COLFLAG_NOTAVAIL`。
   - 循环条件是 `while( pRedo && eProgress )`：只要本轮有进展就继续重试那些被推迟的列；如果一整轮下来 `eProgress` 仍为 0，说明剩下的生成列互相依赖形成环，此时 `pRedo` 非空，函数调用 `sqlite3ErrorMsg(pParse, "generated column loop on \"%s\"", pRedo->zCnName)` 报错。
5. **收尾**把 `pParse->iSelfTab` 复位为 `0`。

**调用方与求值时机**：

- `sqlite3Insert` 有两处调用。第一处在把新行数据拷到 `regCols+1` 之后立刻调用 `sqlite3ComputeGeneratedColumns(pParse, regCols+1, pTab)`，源码注释说明这一步在算 rowid 之前，是为了让「某个生成列引用 ROWID」的场景有值可用。
- 第二处在 `sqlite3Insert` 算出 rowid 之后再次调用 `sqlite3ComputeGeneratedColumns(pParse, regRowid+1, pTab)`。这一处的注释写明：如果某个生成列是从 INTEGER PRIMARY KEY 派生的，必须在 rowid 算出来之后才算。两次调用之间隔着 rowid 的计算逻辑（`OP_NewRowid` 等）与 `autoIncStep`。
- `sqlite3GenerateConstraintChecks` 在 NOT NULL 约束的两遍处理里还有第三次调用：`sqlite3ComputeGeneratedColumns(pParse, regNewData+1, pTab)`。触发条件是 `nSeenReplace>0` 且表带 `TF_HasGenerated` 标志。源码注释解释了原因——第一遍如果有 `NOT NULL ON CONFLICT REPLACE` 约束触发了 REPLACE，被替换掉的行会影响生成列依赖的列，所以生成列必须重算一遍。源码里还有 `if( b2ndPass ) break;  /* Never need more than 2 passes */`，即这个重算最多做两遍。
- `sqlite3Update` 在组装好新行寄存器之后调用 `sqlite3ComputeGeneratedColumns(pParse, regNew, pTab)`；如果存在 BEFORE UPDATE 触发器，触发器跑完、重新读回被触发器改动的普通列之后，再调用一次 `sqlite3ComputeGeneratedColumns(pParse, regNew, pTab)`。

### 读路径：`sqlite3ExprCodeGetColumnOfTable` 里的 VIRTUAL 列

VIRTUAL 列的值不在记录里，读取时现场计算。`sqlite3ExprCodeGetColumnOfTable` 判断 `pCol->colFlags & COLFLAG_VIRTUAL` 后：
- 若该列已带 `COLFLAG_BUSY`，说明正在递归求值，直接 `sqlite3ErrorMsg(pParse, "generated column loop on \"%s\"", pCol->zCnName)` 报错。
- 否则给它打上 `COLFLAG_BUSY`，把 `pParse->iSelfTab` 设为 `iTabCur+1`（正值，含义是「从游标 `iTabCur` 读列」），调用 `sqlite3ExprCodeGeneratedColumn`，然后恢复 `iSelfTab` 并清掉 `COLFLAG_BUSY`。

### 求值代码：`sqlite3ExprCodeGeneratedColumn`

`sqlite3ExprCodeGeneratedColumn(Parse *pParse, Table *pTab, Column *pCol, int regOut)` 负责把「这个生成列的表达式求到 `regOut`」翻译成 VDBE 指令：

1. 断言 `pParse->iSelfTab != 0`。若 `pParse->iSelfTab > 0`（读路径：列值来自游标），先发一条 `OP_IfNullRow`，跳到 `iAddr`；这条指令在 LEFT JOIN 的右表为空行时把结果置空。
2. 调用 `sqlite3ExprCodeCopy(pParse, sqlite3ColumnExpr(pTab,pCol), regOut)` 生成表达式求值代码。`sqlite3ExprCodeCopy` 的做法是 `sqlite3ExprDup` 复制一份表达式、对其 `sqlite3ExprCode`、再 `sqlite3ExprDelete` 释放副本——复制是为了保证原表达式不被改写。
3. 结果后处理分两种情况：
   - 该列是 VIRTUAL 列且表是 STRICT 表（`TF_Strict`），发 `OP_TypeCheck`，P3 取 `2 + (int)(pCol - pTab->aCol)`（列序号从 2 起算，因为 0 与 1 保留给表本身和 rowid），P4 是 `pTab` 且类型为 `P4_TABLE`。
   - 否则，若 `pCol->affinity >= SQLITE_AFF_TEXT`，发 `OP_Affinity` 把结果转成该列声明的 affinity。
4. 跳到 `iAddr` 收尾；若 `pParse->nErr` 增加了，把 `pParse->db->errByteOffset` 置为 `-1`，抹掉字节偏移以便报出整条语句级错误。

### 写入路径上的表达式引用：`TK_COLUMN` 分支

表达式里引用同表其它列时，`sqlite3ExprCodeTarget` 的 `TK_COLUMN` 分支在 `pParse->iSelfTab < 0` 时（即写入路径，行已在寄存器里）计算源寄存器 `iSrc = sqlite3TableColumnToStorage(pTab, iCol) - pParse->iSelfTab`。若被引用的列本身是生成列：
- 已带 `COLFLAG_BUSY` 时报 `generated column loop` 错误；
- 否则打上 `COLFLAG_BUSY`，若该列还带 `COLFLAG_NOTAVAIL`（说明尚未算出），就地调用 `sqlite3ExprCodeGeneratedColumn` 把它算出来，然后清掉 `COLFLAG_BUSY` 与 `COLFLAG_NOTAVAIL`，返回寄存器 `iSrc`。

这就是写入路径上生成列可以引用另一个生成列的实现方式：被依赖者在被引用处现场补算，而不是等到扫描到它自己时。

## 关键取舍

- **依赖顺序用「多趟重试」而不是拓扑排序（源码事实）**：`sqlite3ComputeGeneratedColumns` 第一遍把生成列全标成 `COLFLAG_NOTAVAIL`，第二遍反复迭代直到 `eProgress` 为 0。代价是同一批生成列可能被反复扫描。替代方案是在建表时算出拓扑序，但那样把依赖信息持久化进 schema，代价是 schema 格式要加字段、且 ALTER TABLE 改依赖后要重算顺序。
- **环依赖靠「无进展」判定而非图算法（源码事实）**：循环退出条件 `while( pRedo && eProgress )` 里，`eProgress` 表示本轮是否至少算出一个生成列。如果一整轮没有任何进展，剩下的生成列只能是互相等待，函数报 `generated column loop on "列名"`。这不需要额外维护环检测结构，代价是环的判定延迟到编码期而不是建表期。
- **递归防护用列上的 `COLFLAG_BUSY` 位（源码事实）**：`sqlite3ComputeGeneratedColumns` 在遍历某列表达式前打 `COLFLAG_BUSY`，`sqlite3ExprCodeGetColumnOfTable` 与 `TK_COLUMN` 分支也都检查这一位并在递归进入时置位。同一列不能被两个地方同时展开，因此不需要栈或深度计数。
- **STORED 列的 affinity 延后到算完之后（源码事实）**：`sqlite3ComputeGeneratedColumns` 把 `OP_Affinity` 里 STORED 列的字符改写成 `SQLITE_AFF_NONE`，源码注释说明这是因为这些列「尚未计算」。代价是多一轮指令改写（直接改写已生成的 P4 字符串，而不是延后生成指令），收益是普通列的 affinity 转换可以一次批量完成。
- **STRICT 表的类型检查要显式排除生成列（源码事实）**：STRICT 表走的是 `OP_TypeCheck` 而不是 `OP_Affinity` 分支，此时 `sqlite3ComputeGeneratedColumns` 把 `OP_TypeCheck` 的 P3 置为 `1`；而读路径上 VIRTUAL 列在 STRICT 表中则单独发一条带 `P4_TABLE` 的 `OP_TypeCheck`。这说明「生成列的类型检查」在读、写两条路径上是两套代码。
- **UPDATE 路径要算两次生成列（源码事实）**：`sqlite3Update` 在 BEFORE UPDATE 触发器之后重算一次生成列。源码注释说明触发器可能改动了行里的列，被改动的只有未被本次 UPDATE 修改的列（`aXRef[i]<0` 的列会被重新读入），生成列依赖这些列，所以必须重算。代价是 UPDATE 的编码多一段。
- **REPLACE 冲突也要重算生成列（源码事实）**：`sqlite3GenerateConstraintChecks` 里 `nSeenReplace>0` 时重算生成列，源码注释说明生成列的值「可能依赖于被 REPLACE 影响的列」。同一处注释 `Never need more than 2 passes` 把重算次数限定在两遍。
- **架构推论：** 生成列的求值完全落在写入路径与表达式代码生成器之间，没有单独的「生成列模块」。`sqlite3ComputeGeneratedColumns` 负责「什么时候算」，`sqlite3ExprCodeGeneratedColumn` 负责「怎么算成指令」，`sqlite3ExprCodeGetColumnOfTable` 负责「读的时候现算」。这三条路径共享 `COLFLAG_BUSY` / `COLFLAG_NOTAVAIL` 这套状态位作为唯一的协调机制。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/insert.c` | `sqlite3ComputeGeneratedColumns` | 写入路径上生成列的求值编排：补 affinity、标记 `COLFLAG_NOTAVAIL`、多趟重试算依赖、检出环依赖报错 |
| `src/insert.c` | `exprColumnFlagUnion` | 遍历生成列表达式，把被引用列的 `colFlags` 按位或到 walker 上，用于判定依赖是否已就绪 |
| `src/insert.c` | `sqlite3TableAffinity` | 生成列求值前为普通列生成 `OP_Affinity`（或 STRICT 表的 `OP_TypeCheck`）指令 |
| `src/insert.c` | `sqlite3Insert` | 调用 `sqlite3ComputeGeneratedColumns` 求生成列：算 rowid 前一次、算 rowid 后一次 |
| `src/insert.c` | `sqlite3GenerateConstraintChecks` | NOT NULL 约束两遍处理中，因 REPLACE 冲突而重算生成列 |
| `src/update.c` | `sqlite3Update` | UPDATE 路径上求生成列，并在 BEFORE UPDATE 触发器之后重算一次 |
| `src/expr.c` | `sqlite3ExprCodeGeneratedColumn` | 把生成列的 AS 表达式翻译成 VDBE 指令并写入目标寄存器，附带 STRICT 类型检查与 affinity 转换 |
| `src/expr.c` | `sqlite3ExprCodeGetColumnOfTable` | 读路径：遇到 VIRTUAL 列时现场调用 `sqlite3ExprCodeGeneratedColumn` 计算 |
| `src/expr.c` | `sqlite3ExprCodeCopy` | 生成列求值代码的落地手段：复制表达式、代码生成、释放副本，保证原表达式不被改写 |
| `src/expr.c` | `sqlite3ExprCodeTarget` | `TK_COLUMN` 分支在 `iSelfTab<0` 时解析同表列引用，被引用的是未算出的生成列则就地补算 |
| `src/build.c` | `sqlite3TableColumnToStorage` | 列序号到记录存储位置的映射，把 VIRTUAL 列排到非虚拟列之后 |
| `src/build.c` | `sqlite3ColumnExpr` | 按列的 `iDflt` 下标从 `pTab->u.tab.pDfltList` 取出生成列的 AS 表达式 |
| `src/sqliteInt.h` | `COLFLAG_VIRTUAL` / `COLFLAG_STORED` / `COLFLAG_GENERATED` / `COLFLAG_NOTAVAIL` / `COLFLAG_BUSY` | 生成列的列级状态位，取值分别为 `0x0020` / `0x0040` / `0x0060` / `0x0080` / `0x0100` |
| `src/sqliteInt.h` | `TF_HasVirtual` / `TF_HasStored` / `TF_HasGenerated` | 表级标志位，取值分别为 `0x00000020` / `0x00000040` / `0x00000060`，与列级标志位一一对应 |

## 与主核 / AI 的关系

暂无直接对应实现。本页的生成列机制是 SQLite 的写入路径能力，与本仓主核（`engine/db/`）的列式存储没有可直接映射的关系：主核是否有派生列、派生列在写入时物化还是读取时计算，都不能由本页的源码事实推定。若要借鉴，可参考的是「依赖顺序由重试而非拓扑排序解决」与「递归防护用列状态位而非栈」这两条设计取舍本身，而不是 SQLite 的具体指令序列。

## 参考

- [SQLite 官方文档：Generated Columns](https://www.sqlite.org/gencol.html) —— 支撑 VIRTUAL 与 STORED 的语义差别、生成列可引用的表达式范围、以及生成列自 3.31.0 起支持这一版本背景。
- [GitHub 官方源码：src/insert.c](https://github.com/sqlite/sqlite/blob/master/src/insert.c) —— 支撑 `sqlite3ComputeGeneratedColumns` 的多趟重试、affinity 改写、依赖重算与调用顺序。
- [GitHub 官方源码：src/expr.c](https://github.com/sqlite/sqlite/blob/master/src/expr.c) —— 支撑 `sqlite3ExprCodeGeneratedColumn` 的指令生成、`sqlite3ExprCodeGetColumnOfTable` 的 VIRTUAL 列读取路径与 `TK_COLUMN` 的就地补算。
- [SQLite 官方文档：Query Planner](https://sqlite.org/optoverview.html) —— 供对照表达式与索引选择背景，生成列的具体机制以源码为准。
