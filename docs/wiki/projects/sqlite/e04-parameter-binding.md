---
project: sqlite
feature: parameter-binding
page_type: feature
schema_version: 1
refs:
  - path: src/vdbeapi.c
    symbols: [sqlite3_bind_int64, sqlite3_bind_text, sqlite3_bind_blob, sqlite3_bind_parameter_index, sqlite3_bind_parameter_count]
---

# 参数绑定

## 一句话职责

把调用方准备好的值写进已编译语句的变量槽位（`Vdbe.aVar[]`），并提供「参数名 → 参数序号」的解析入口，使占位符与宿主语言的数据之间只通过一条受控通道相连。

## 机制是什么

一条带占位符的 SQL 文本在 prepare 阶段被编译成 VDBE 程序，程序里的占位符位置被替换成 `OP_Variable` 操作码。占位符本身不携带值——值由调用方在语句执行前通过 `sqlite3_bind_*` 系列 API 逐个写入语句对象内部的变量数组。

这个机制要解决三件事：

1. **写入**：把一个整型、浮点数、文本、Blob 或 NULL 值放进第 `i` 个变量槽位。
2. **计数**：告诉调用方这条语句一共有多少个可绑定的槽位（`sqlite3_bind_parameter_count`）。
3. **按名解析**：把 `:name` / `@name` / `$name` 这类带名占位符翻译成序号（`sqlite3_bind_parameter_index`）。

边界上，本机制**不负责**参数在表达式中的求值——那属于 C 组的表达式编译与运行时求值；也**不负责**结果列的读取与类型转换——那是 E03 的职责。本机制管的是「值进入语句」这一段，不涉及「值如何被解释」。

### 变量槽位是怎么来的

编译期（`src/expr.c` 的变量编号解析逻辑）决定了槽位的编号规则：

- 形如 `?NNN` 的序号占位符，其序号必须落在 1 到 `db->aLimit[SQLITE_LIMIT_VARIABLE_NUMBER]` 之间，否则报 "variable number must be between ?1 and ?N"。`SQLITE_LIMIT_VARIABLE_NUMBER` 的编译期硬上限常量是 `SQLITE_MAX_VARIABLE_NUMBER`，定义在 `src/sqliteLimit.h`，默认值 **32766**；连接建立时 `src/main.c` 的 `sqlite3_open_v2` 用 `memcpy(db->aLimit, aHardLimit, sizeof(db->aLimit))` 把全部运行期上限初始化为这批硬上限常量，因此默认运行期值同样是 32766。
- 形如 `:aaa` / `$aaa` / `@aaa` 的带名占位符，同名重复出现**复用同一个序号**（源码用 `sqlite3VListNameToNum(pParse->pVList, z, n)` 查已有映射），首次出现则 `++pParse->nVar` 分配新序号。
- `pParse->nVar` 是槽位总数，在 `src/vdbeaux.c` 的 `sqlite3VdbeMakeReady` 中被搬进语句对象：`p->pVList = pParse->pVList;` 移交变量名表，`p->nVar = (ynVar)nVar;` 写入槽位数。

也就是说，**带名占位符与序号占位符共享同一个 1 起始的序号空间**，这是 `sqlite3_bind_parameter_index` 能返回与 `sqlite3_bind_text` 的 `i` 参数同一坐标系的前提。

## 怎么实现的

### 两条不同的写入路径

绑定 API 并不是一条统一的实现路径，而是分成两类：

- **走 `vdbeUnbind` 的路径**：`sqlite3_bind_text`、`sqlite3_bind_blob`、`sqlite3_bind_null`、`sqlite3_bind_double`、`sqlite3_bind_zeroblob`、`sqlite3_bind_pointer` 都先调 `vdbeUnbind(p, (u32)(i-1))`。
- **不走 `vdbeUnbind` 的路径**：`sqlite3_bind_int64` 自己完整实现了状态检查、范围检查和过期判断。

`vdbeUnbind` 是 `src/vdbeapi.c` 中的静态函数，语义是「把第 `i` 个变量槽位清成 NULL」：

1. `vdbeSafetyNotNull(p)` 为真（语句为空或已 finalize）时返回 `SQLITE_MISUSE_BKPT`。
2. `sqlite3_mutex_enter(p->db->mutex)` 持连接互斥锁。
3. `p->eVdbeState != VDBE_READY_STATE` 时置 `SQLITE_MISUSE_BKPT`，`sqlite3_log(SQLITE_MISUSE, "bind on a busy prepared statement: [%s]", p->zSql)` 后返回——**在正在执行的语句上绑定是非法操作**。
4. `i >= (unsigned int)p->nVar` 时置 `SQLITE_RANGE` 并返回——**越界绑定返回 `SQLITE_RANGE`**。注意这里用的是无符号比较，因此 `i==0` 会在 `(u32)(i-1)` 之后变成 `0xffffffff`，同样落进 `SQLITE_RANGE` 分支。
5. **计划失效判断**：`p->expmask` 是「绑定这些变量会让当前查询计划失效」的位掩码，`p->smimask` 是其子集（含义是「只有当值在 0 与 1 之间来回变化时才失效」，`src/vdbeInt.h` 的注释写明 "smimask is always a subset of expmask"）。掩码位通过 `i>=31 ? 0x80000000 : (u32)1<<i` 计算——**第 32 个及以后的变量共用最高位 0x80000000**，因为 `expmask` 是 32 位。
   - 当 `p->smimask==0`，或该变量位不在 `smimask` 中，或旧值是 0/1 的整数（`(pVar->flags & MEM_Int)!=0 && pVar->u.i>=0 && pVar->u.i<=1`）时，置 `p->expired = 1`。
6. `sqlite3VdbeMemRelease(pVar)` 释放旧值，`pVar->flags = MEM_Null`（`MEM_Null` 常量值为 `0x0001`，定义在 `src/vdbeInt.h`），`p->db->errCode = SQLITE_OK`，返回 `SQLITE_OK`。

源码注释里为 `vdbeUnbind` 标注了需求编号 `R-57496-20354`：若绑定到 WHERE 子句的宿主参数会影响查询计划选择，则在该参数绑定值变化后的第一次 `sqlite3_step()` 时，语句会像发生 schema 变更一样被自动重新编译。

### `sqlite3_bind_int64`：唯一自己写全流程的绑定函数

`sqlite3_bind_int64` 是本页 `refs` 里唯一一个不经过 `vdbeUnbind` 的绑定函数（`sqlite3_bind_int` 是它的转发：`return sqlite3_bind_int64(p, i, (i64)iValue);`）。它的流程是：

1. `vdbeSafetyNotNull(p)` 为真时返回 `SQLITE_MISUSE_BKPT`。
2. `sqlite3_mutex_enter(p->db->mutex)`。
3. `p->eVdbeState != VDBE_READY_STATE` 时置 `SQLITE_MISUSE_BKPT`，`sqlite3_log(SQLITE_MISUSE, "bind on a busy prepared statement: [%s]", p->zSql)` 后返回。
4. 范围检查：`if( i<=0 || (--i)>=p->nVar )` 则置 `SQLITE_RANGE` 并返回。**注意这里的做法与 `vdbeUnbind` 不同**——它先把 `i` 减 1 变成 0 基下标再与 `nVar` 比较，并用 `i<=0` 单独挡掉 0 与负数。
5. 计划失效判断：`p->expmask!=0` 时算出 `expireMask = i>=32 ? 0x80000000 : (u32)1 << i`。
   - 若该位在 `smimask` 中，则只有「旧值是 0/1 整数」与「新值是 0/1」两者不一致时才 `p->expired = 1`（源码注释："only expire the prepared statement if the value is changing to or from (0,1) and something else"）。
   - 若该位不在 `smimask` 中，则当旧值不是整数类型或新旧整数值不等时 `p->expired = 1`（源码注释："Always expire if the smimask bit is clear"）。
6. `sqlite3VdbeMemSetInt64(pVar, iValue)` 写入值。
7. `sqlite3_mutex_leave(p->db->mutex)`，返回 `SQLITE_OK`。

`sqlite3VdbeMemSetInt64` 实现在 `src/vdbemem.c`：若内存单元持有动态内存（`VdbeMemDynamic(pMem)`）则走 `vdbeReleaseAndSetInt64(pMem, val)`，否则直接 `pMem->u.i = val; pMem->flags = MEM_Int;`。`MEM_Int` 常量值为 `0x0004`（`src/vdbeInt.h`）。

### `bindText`：文本与 Blob 的公共实现

`sqlite3_bind_text` 与 `sqlite3_bind_blob` 都是 `bindText` 的薄封装：

- `sqlite3_bind_text` 调 `bindText(pStmt, i, zData, nData, xDel, SQLITE_UTF8)`。
- `sqlite3_bind_blob` 调 `bindText(pStmt, i, zData, nData, xDel, 0)`——**编码参数传 0**。`bindText` 内部的分支是：编码为 `SQLITE_UTF8` 时走 `sqlite3VdbeMemSetText`；为 `SQLITE_UTF8_ZT` 时走 `sqlite3VdbeMemSetText` 并额外置 `pVar->flags |= MEM_Term`（`MEM_Term` 值 `0x0200`）；其余（含 Blob 的 0）走 `sqlite3VdbeMemSetStr(pVar, zData, nData, encoding, xDel)`，且编码为 0 时补一句 `pVar->enc = ENC(p->db)`。这就是 Blob 与 Text 在实现上的分岔点：**Blob 不带编码**。

`bindText` 的完整流程：

1. `rc = vdbeUnbind(p, (u32)(i-1))`。
2. 若 `rc==SQLITE_OK`：断言 `p!=0 && p->aVar!=0 && i>0 && i<=p->nVar`（源码标注 `tag-20240917-01`），然后按上面的编码分支写入内存单元。
3. 编码非 0 时，若写入成功，再调 `sqlite3VdbeMemChangeEncoding(pVar, ENC(p->db))` 把值转成连接的原生编码。
4. 写入失败时 `sqlite3Error(p->db, rc)` 记录错误，再 `sqlite3ApiExit(p->db, rc)` 走统一的错误闸门。
5. `sqlite3_mutex_leave(p->db->mutex)`。
6. **失败时的析构责任**：若 `rc != SQLITE_OK` 且 `xDel` 既不是 `SQLITE_STATIC` 也不是 `SQLITE_TRANSIENT`，则 `xDel((void*)zData)`——**绑定失败时由 SQLite 代为调用调用方的析构函数**，避免内存泄漏。这条责任转移是 `sqlite3_bind_blob64` 与 `sqlite3_bind_text64` 里那句 `assert( xDel!=SQLITE_DYNAMIC );` 的原因：`SQLITE_DYNAMIC` 语义与 `SQLITE_TRANSIENT` 冲突，所以被排除在外。

长度限制由 `sqlite3VdbeMemSetText` 负责：若 `nByte > (i64)pMem->db->aLimit[SQLITE_LIMIT_LENGTH]`，则按 `xDel` 释放数据、把内存单元置 NULL，并返回 `SQLITE_TOOBIG`。`SQLITE_LIMIT_LENGTH` 的枚举值是 **0**（`src/sqlite.h.in`），其编译期硬上限常量是 `SQLITE_MAX_LENGTH`，定义在 `src/sqliteLimit.h`，默认值 **1000000000**。

### `sqlite3_bind_parameter_count` 与 `sqlite3_bind_parameter_index`

- `sqlite3_bind_parameter_count(pStmt)` 实现只有一行有效逻辑：`Vdbe *p = (Vdbe*)pStmt; return p ? p->nVar : 0;`——**返回的是槽位总数 `p->nVar`，语句为空时返回 0**。源码注释说明这个接口是「为支持 DBD::SQLite 而加的」。
- `sqlite3_bind_parameter_index(pStmt, zName)` 调 `sqlite3VdbeParameterIndex((Vdbe*)pStmt, zName, sqlite3Strlen30(zName))`，后者在 `p==0 || zName==0` 时返回 0，否则调 `sqlite3VListNameToNum(p->pVList, zName, nName)`。
- `sqlite3VListNameToNum` 实现在 `src/util.c`：遍历变量名表 `VList`（一个 `int` 数组，布局为「长度槽 / 已用长度槽 / (序号, 记录长度, 名字字节) 若干条」），匹配条件是 `strncmp(z,zName,nName)==0 && z[nName]==0`，命中返回该变量的序号，未命中返回 0。**因此 `sqlite3_bind_parameter_index` 返回 0 表示「没有这个名字」，而合法的参数序号从 1 开始。**
- 反向接口 `sqlite3_bind_parameter_name(pStmt, i)` 调 `sqlite3VListNumToName(p->pVList, i)`，后者同样遍历 `VList` 比对序号字段，命中返回名字指针，未命中返回 NULL。

注意 `sqlite3_bind_parameter_name` 的返回值是**连接内存储的指针**，生命周期随语句结束，不是调用方自己的一份拷贝。

### 与绑定相关的其它入口

- `sqlite3_bind_null(pStmt, i)` 走 `vdbeUnbind`——把槽位置空即等价于绑定 NULL，源码注释明说 "This is the same as binding a NULL value to the column"。
- `sqlite3_bind_value(pStmt, i, pValue)` 按 `sqlite3_value_type` 分派到各绑定函数：`SQLITE_INTEGER` → `sqlite3_bind_int64`；`SQLITE_FLOAT` → `sqlite3_bind_double`；`SQLITE_BLOB` → 若带 `MEM_Zero` 标志则 `sqlite3_bind_zeroblob`，否则 `sqlite3_bind_blob(pStmt, i, pValue->z, pValue->n, SQLITE_TRANSIENT)`；`SQLITE_TEXT` → 直接调 `bindText` 并传 `SQLITE_TRANSIENT`；其余 → `sqlite3_bind_null`。
- `sqlite3_bind_zeroblob64(pStmt, i, n)` 先查 `n > (u64)p->db->aLimit[SQLITE_LIMIT_LENGTH]`，超出则返回 `SQLITE_TOOBIG`，否则转 `sqlite3_bind_zeroblob`。
- `sqlite3_clear_bindings(pStmt)` 把 `p->nVar` 个槽位全部清成 NULL：遍历时若某槽位当前是 0/1 整数就把它记进局部位掩码 `maskSmallInt`（同样用 `i>=31 ? 0x80000000 : ((u32)1)<<i` 编码），然后 `sqlite3VdbeMemRelease(pVar); pVar->flags = MEM_Null;`。清空之后若 `(p->expmask&~p->smimask)!=0 || (p->smimask&maskSmallInt)!=0`，则 `p->expired = 1`——**清空绑定同样会让语句计划失效**。
- `sqlite3TransferBindings(pFromStmt, pToStmt)` 在语句重编译时把绑定搬过去：循环 `for(i=0; i<pFrom->nVar; i++) sqlite3VdbeMemMove(&pTo->aVar[i], &pFrom->aVar[i]);`。它的调用方是 `src/prepare.c` 的 `sqlite3Reprepare`（由 `src/vdbeapi.c` 的 `sqlite3_step` 在遇到 `SQLITE_SCHEMA` 时触发，见 E02）。

## 关键取舍

- **绑定与编译分离**：值不进入 SQL 文本，只进入语句对象的变量数组。好处是值可以自由更换而无需重新编译；代价是调用方必须记住「哪些槽位还没绑」——未绑定的槽位保持 `MEM_Null`，SQL 语义上等价于绑了 NULL。
- **绑定受语句状态约束**：所有绑定入口都要求语句处于 `VDBE_READY_STATE`（常量值 **1**，定义在 `src/vdbeInt.h`，含义是 "Ready to run but not yet started"），否则返回 `SQLITE_MISUSE_BKPT`。这条约束保证了绑定不会与正在执行的语句竞争同一批内存单元。
- **越界统一报 `SQLITE_RANGE`**：两个入口的检查写法不同（`vdbeUnbind` 用无符号比较，`sqlite3_bind_int64` 先减一再比较），但对外结果一致。这让调用方可以用同一个分支处理「序号不合法」。
- **用位掩码表达「影响计划」**：`expmask` / `smimask` 是一对 32 位掩码，把「哪些参数的值会影响查询计划」这件事压缩到语句对象里，代价是**第 32 个及以后的变量共享同一个掩码位 0x80000000**，因此这些变量只要有一个被重新绑定，就会触发整条语句重新编译。
- **`smimask` 子集语义带来的精度提升**：只关心 0/1 布尔值的参数，用 `smimask` 标记后，只有当值真的在 0 与 1 之间来回切换时才让语句过期。这避免了 `WHERE flag = ?` 这类语句在重复绑定同一个布尔值时反复重编译。
- **绑定失败时析构责任转移到 SQLite**：`bindText` 在失败路径上调 `xDel`，保证调用方传入的缓冲区不会泄漏。代价是这个所有权转移规则不直观，源码用 `assert( xDel!=SQLITE_DYNAMIC );` 把冲突情形挡在编译期。
- **`SQLITE_TRANSIENT` 是安全的默认选择**：它在 `sqlite3VdbeMemSetText` 里触发一次拷贝（分配 `nByte+1` 字节并 `memcpy`），使 SQLite 立即拥有自己的副本，调用方可以随即释放原缓冲区。
- **同一坐标系下的两种寻址**：`sqlite3_bind_parameter_index` 与 `sqlite3_bind_text` 的 `i` 参数共用一套 1 起始序号（由 `sqlite3VListNameToNum` 与 `p->nVar` 保证），代价是「序号 0」被占作「无效」标记，无法绑定。

## 与主核 / AI 的关系

本机制是 SQLite 里「数据从宿主语言进入查询引擎」的唯一入口，也是 SQL 注入防线在机制层面的落点：值走 `Mem` 槽位，永远不会被拼回 SQL 文本，因此不需要转义。

**架构推论：** 主数据库内核（`engine/db/`）的 SPI 层若要对齐这套形状，关键约束是「绑定值与查询计划解耦，但计划失效必须可判定」——SQLite 用 `expmask`/`smimask` 这对位掩码把「哪些参数影响计划」这件信息随语句一起保存，而不是在每次执行时重新比较全部绑定值。若主核不引入同类机制，就只能在每次执行前重算计划，或者退化为「一条语句只有一种绑定形态」的接口，二者都比 SQLite 的方案差。

**架构推论：** `sqlite3_bind_int64` 单独实现一遍过期判断（而不是复用 `vdbeUnbind`）说明：整型绑定的过期条件更精细——它要比较「旧值是不是 0/1 整数」与「新值是不是 0/1」。主核的 SPI 层若要对齐这一点，等价要求是绑定入口必须能拿到旧值的类型信息，而不只是旧值的字节。

**架构推论：** 从 AI 系统角度看，这套「参数化绑定」是让模型生成的 SQL 具备安全性的前提：模型只负责产出带占位符的 SQL 模板，值由宿主系统通过 `sqlite3_bind_*` 注入。若主核要支持「LLM 直出 SQL + 结构化参数」的形态，绑定通道必须是显式的、可静态校验的，而不是让模型把值内联进 SQL 字符串。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/vdbeapi.c | `sqlite3_bind_int64` | 整型绑定：自查空指针（`vdbeSafetyNotNull`）、查语句状态（`VDBE_READY_STATE`）、查范围（`i<=0 || (--i)>=p->nVar` 报 `SQLITE_RANGE`），按 `expmask`/`smimask` 决定是否 `p->expired = 1`，再 `sqlite3VdbeMemSetInt64` 写入 |
| src/vdbeapi.c | `sqlite3_bind_text` | 文本绑定：`bindText(pStmt, i, zData, nData, xDel, SQLITE_UTF8)`，编码参数使 `bindText` 走 `sqlite3VdbeMemSetText` 分支 |
| src/vdbeapi.c | `sqlite3_bind_blob` | Blob 绑定：`bindText(pStmt, i, zData, nData, xDel, 0)`，编码参数为 0 使 `bindText` 走 `sqlite3VdbeMemSetStr` 分支且不带编码 |
| src/vdbeapi.c | `sqlite3_bind_parameter_index` | 参数名 → 序号：`sqlite3VdbeParameterIndex((Vdbe*)pStmt, zName, sqlite3Strlen30(zName))`，无此名返回 0 |
| src/vdbeapi.c | `sqlite3_bind_parameter_count` | 参数总数：返回 `p->nVar`，语句为空返回 0 |
| src/vdbeapi.c | `vdbeUnbind` | 把第 `i` 个槽位置为 NULL 的公共前置：查 `VDBE_READY_STATE`、查 `i>=p->nVar` 报 `SQLITE_RANGE`、按 `expmask`/`smimask` 置 `p->expired`、`sqlite3VdbeMemRelease` + `pVar->flags = MEM_Null` |
| src/vdbeapi.c | `bindText` | 文本/Blob 公共实现：按编码分派到 `sqlite3VdbeMemSetText`（UTF8）、`sqlite3VdbeMemSetText` + `MEM_Term`（UTF8_ZT）、`sqlite3VdbeMemSetStr`（其余）；编码非 0 时 `sqlite3VdbeMemChangeEncoding` 转连接原生编码；失败时 `sqlite3Error` + `sqlite3ApiExit`，且在 `rc!=SQLITE_OK` 且 `xDel` 非 `SQLITE_STATIC`/`SQLITE_TRANSIENT` 时调 `xDel` 释放数据 |
| src/vdbeapi.c | `sqlite3_bind_null` | 绑定 NULL：`vdbeUnbind` 后置槽位为空即等价于 NULL |
| src/vdbeapi.c | `sqlite3_bind_double` | 浮点绑定：`vdbeUnbind` 后 `sqlite3VdbeMemSetDouble(&p->aVar[i-1], rValue)` |
| src/vdbeapi.c | `sqlite3_bind_value` | 按 `sqlite3_value_type` 分派：`SQLITE_INTEGER`→`sqlite3_bind_int64`、`SQLITE_FLOAT`→`sqlite3_bind_double`、`SQLITE_BLOB`→`sqlite3_bind_zeroblob`（带 `MEM_Zero`）或 `sqlite3_bind_blob`、`SQLITE_TEXT`→`bindText`、其余→`sqlite3_bind_null` |
| src/vdbeapi.c | `sqlite3_bind_zeroblob64` | 零填充 Blob：先比 `n > db->aLimit[SQLITE_LIMIT_LENGTH]` 报 `SQLITE_TOOBIG`，否则转 `sqlite3_bind_zeroblob` |
| src/vdbeapi.c | `sqlite3_clear_bindings` | 清空全部绑定：逐槽 `sqlite3VdbeMemRelease` + `flags = MEM_Null`，并用 `maskSmallInt` 记录原为 0/1 整数的槽位，据此决定是否 `p->expired = 1` |
| src/vdbeapi.c | `sqlite3VdbeParameterIndex` | 参数名解析：`p==0 \|\| zName==0` 返回 0，否则 `sqlite3VListNameToNum(p->pVList, zName, nName)` |
| src/vdbeapi.c | `sqlite3_bind_parameter_name` | 序号 → 参数名：`sqlite3VListNumToName(p->pVList, i)`，未命中返回 NULL；返回的是连接内存储的指针 |
| src/vdbeapi.c | `sqlite3TransferBindings` | 语句重编译时搬迁绑定：`for(i=0; i<pFrom->nVar; i++) sqlite3VdbeMemMove(&pTo->aVar[i], &pFrom->aVar[i]);` |
| src/vdbemem.c | `sqlite3VdbeMemSetInt64` | 写整型：`VdbeMemDynamic(pMem)` 为真走 `vdbeReleaseAndSetInt64`，否则 `pMem->u.i = val; pMem->flags = MEM_Int;` |
| src/vdbemem.c | `sqlite3VdbeMemSetText` | 写文本：长度超过 `db->aLimit[SQLITE_LIMIT_LENGTH]` 时按 `xDel` 释放数据、置 NULL 并返回 `SQLITE_TOOBIG`；`xDel==SQLITE_TRANSIENT` 时拷贝一份并追加结尾零字节 |
| src/util.c | `sqlite3VListNameToNum` | 在变量名表 `VList` 中按 `strncmp(z,zName,nName)==0 && z[nName]==0` 匹配，返回该变量序号，未命中返回 0 |
| src/util.c | `sqlite3VListNumToName` | 在变量名表 `VList` 中按序号字段匹配，返回名字指针，未命中返回 NULL |
| src/util.c | `sqlite3VListAdd` | 变量名表插入：按 `nName/4 + 3` 个 `int` 的记录长度写入 `(序号, 长度, 名字)` 三元组，必要时用 `sqlite3DbRealloc` 扩容 |
| src/expr.c | 变量编号解析 | 编译期决定槽位编号：`?NNN` 校验 1..`db->aLimit[SQLITE_LIMIT_VARIABLE_NUMBER]`；`:aaa`/`$aaa`/`@aaa` 经 `sqlite3VListNameToNum` 复用同名序号，首次出现则 `++pParse->nVar` |
| src/vdbeaux.c | `sqlite3VdbeMakeReady` | 语句就绪时固化绑定结构：`p->pVList = pParse->pVList;` 移交变量名表，`p->nVar = (ynVar)nVar;` 写入槽位数 |
| src/vdbeInt.h | `MEM_Null` / `MEM_Int` / `MEM_Term` / `MEM_Zero` | 内存单元类型标志，取值分别为 `0x0001` / `0x0004` / `0x0200` / 用于判零填充 Blob 的标志位 |
| src/vdbeInt.h | `VDBE_READY_STATE` | 语句「已就绪未开始」的状态常量，取值 1；所有绑定入口要求语句处于此状态 |
| src/vdbeInt.h | `expmask` / `smimask` | 语句级位掩码：前者标「绑定这些变量会让计划失效」，后者是其子集，标「只在值于 0/1 间变化时失效」；注释写明 "smimask is always a subset of expmask" |
| src/vdbeInt.h | `nVar` / `aVar` / `pVList` | 语句对象的绑定结构：槽位数、变量内存单元数组、变量名表指针 |
| src/sqliteLimit.h | `SQLITE_MAX_VARIABLE_NUMBER` | 变量个数上限常量，默认 32766（`#ifndef` 保护，可编译期覆盖） |
| src/sqliteLimit.h | `SQLITE_MAX_LENGTH` | 字符串/Blob 长度上限常量，默认 1000000000（`#ifndef` 保护，可编译期覆盖） |
| src/sqlite.h.in | `SQLITE_LIMIT_LENGTH` | 长度上限的 `sqlite3_limit` 类别枚举值，取值 0 |
| src/sqlite.h.in | `SQLITE_LIMIT_VARIABLE_NUMBER` | 变量个数上限的 `sqlite3_limit` 类别枚举值 |
| src/main.c | `sqlite3_open_v2` | 连接建立时用 `memcpy(db->aLimit, aHardLimit, sizeof(db->aLimit))` 把运行期上限初始化为编译期硬上限常量 |
| src/main.c | `aHardLimit` | 各上限类别的硬上界数组，首项为 `SQLITE_MAX_LENGTH`，变量个数项为 `SQLITE_MAX_VARIABLE_NUMBER` |
| src/sqliteInt.h | `SQLITE_MISUSE_BKPT` | 展开为 `sqlite3MisuseError(__LINE__)`，用于 API 不当调用（如对 busy 语句绑定） |
| src/sqliteInt.h | `sqlite3Strlen30NN` | 长度计算宏：`(strlen(C)&0x3fffffff)`，`sqlite3_bind_parameter_index` 用它算参数名长度 |

## 参考

- [Binding Parameters To An SQL Statement（sqlite3_bind_* 官方文档）](https://sqlite.org/c3ref/bind_blob.html)
- [sqlite3_bind_parameter_count（官方文档）](https://sqlite.org/c3ref/bind_parameter_count.html)
- [sqlite3_bind_parameter_index（官方文档）](https://sqlite.org/c3ref/bind_parameter_index.html)
- [sqlite3_bind_parameter_name（官方文档）](https://sqlite.org/c3ref/bind_parameter_name.html)
- [sqlite3_clear_bindings（官方文档）](https://sqlite.org/c3ref/clear_bindings.html)
- [Limits On The Length Of SQL Statements And Strings（sqlite3_limit 官方文档）](https://sqlite.org/c3ref/limit.html)
- [The SQLite Bytecode Engine（VDBE 官方文档）](https://sqlite.org/opcode.html)
- [Architecture of SQLite（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [sqlite/sqlite 仓库中的 src/vdbeapi.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbeapi.c)
- [SQLite 源码浏览：src/vdbeapi.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/vdbeapi.c)
