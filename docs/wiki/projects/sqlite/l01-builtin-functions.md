---
project: sqlite
feature: builtin-functions
page_type: feature
schema_version: 1
refs:
  - path: src/func.c
    symbols: [sqlite3RegisterBuiltinFunctions, sqlite3RegisterPerConnectionBuiltinFunctions, sqlite3QuoteValue]
---

# 内置标量与聚合函数

## 一句话职责

SQLite 把除日期时间与 JSON 之外的内置 SQL 函数集中登记为一张只读的静态函数表，并约定标量函数与聚合函数两类不同的调用契约（前者一次性求值，后者按 step/final/inverse 三段式在分组上累积）。

## 机制是什么

- **注册表**：`sqlite3RegisterBuiltinFunctions` 持有一张 `aBuiltinFunc[]` 静态数组，每个元素是一个 `FuncDef` 结构（`src/sqliteInt.h` 的 `struct FuncDef`），字段包括参数个数 `nArg`、标志位 `funcFlags`、用户数据 `pUserData`、函数指针 `xSFunc`、聚合终结器 `xFinalize`、聚合当前值 `xValue`、聚合逆步 `xInverse` 以及 SQL 名字 `zName`。
- **两段注册**：`sqlite3RegisterBuiltinFunctions` 是**全局**注册（进程内一次，所有连接共享）；`sqlite3RegisterPerConnectionBuiltinFunctions` 是**每连接**注册，只处理不能进全局表的那一个函数。
- **名字解析**：名字解析走 `sqlite3FindFunction`（`src/callback.c`），先查 `db->aFunc`（用户自定义函数），再查全局内置表 `sqlite3BuiltinFunctions`；两者按 `matchQuality` 打分，参数个数精确匹配得 4 分、变长匹配得 1 分，编码完全一致再加 2 分、字节序不同再加 1 分。内置表里的 `nArg` 用负值表达变长：`-1` 表示任意个数，`-3` 表示至少 1 个参数，`-4` 表示至少 2 个参数。
- **聚合函数契约**：聚合函数在 `FuncDef` 里同时登记 `xSFunc`（step）、`xFinalize`、`xValue`、`xInverse` 四个入口；累积状态通过 `sqlite3_aggregate_context`（`src/vdbeapi.c`）拿到，首次调用按 `nByte` 分配、后续返回同一块内存。`xInverse` 是窗口函数 `SQLITE_OMIT_WINDOWFUNC` 未定义时的可选能力。
- **字面量引号**：`sqlite3QuoteValue` 把一个 `sqlite3_value` 渲染成可直接嵌入 SQL 语句的 SQL 字面量文本。

**边界**（依 `sqlite_mechanism_specs.py` 的 `builtin-functions` 条目）：本页只负责内置函数的注册与分派、以及标量/聚合两类函数的执行约定；用户自定义函数属 l02，日期时间函数属 l03（`sqlite3RegisterDateTimeFunctions`）、JSON 函数属 l04（`sqlite3RegisterJsonFunctions`），二者虽然由 `sqlite3RegisterBuiltinFunctions` 一并触发注册，但实现体不在本页范围内。

## 怎么实现的

### 1. 全局注册：`sqlite3RegisterBuiltinFunctions`

该函数体内是一张 `static FuncDef aBuiltinFunc[]` 数组，数组元素由 `src/sqliteInt.h` 的宏展开生成，宏决定了每个函数的标志位组合：

| 宏 | 展开出的标志位 | 语义 |
|---|---|---|
| `FUNCTION` | `SQLITE_FUNC_BUILTIN｜SQLITE_FUNC_CONSTANT｜SQLITE_UTF8`（`bNC` 为真时再加 `SQLITE_FUNC_NEEDCOLL`） | 常规标量函数 |
| `VFUNCTION` | `SQLITE_FUNC_BUILTIN｜SQLITE_UTF8`（`bNC` 为真时加 `SQLITE_FUNC_NEEDCOLL`） | 无常量性保证的函数，如 `random()`、`changes()` |
| `SFUNCTION` | `SQLITE_FUNC_BUILTIN｜SQLITE_UTF8｜SQLITE_DIRECTONLY｜SQLITE_FUNC_UNSAFE` | 有副作用且只能在受信上下文调用，如 `load_extension()`、`sqlite_log()` |
| `DFUNCTION` | `SQLITE_FUNC_BUILTIN｜SQLITE_FUNC_SLOCHNG｜SQLITE_UTF8` | 结果随连接状态变化的函数，如 `sqlite_version()`、`sqlite_source_id()` |
| `MFUNCTION` | `SQLITE_FUNC_BUILTIN｜SQLITE_FUNC_CONSTANT｜SQLITE_UTF8` | 数学函数，`pUserData` 直接存 C 库函数指针（`xCeil`、`exp`、`pow`、`fmod` 等） |
| `INLINE_FUNC` | `SQLITE_FUNC_BUILTIN｜SQLITE_UTF8｜SQLITE_FUNC_INLINE｜SQLITE_FUNC_CONSTANT` | 由代码生成器展开成 VDBE 指令，不走函数调用 |
| `LIKEFUNC` | `SQLITE_FUNC_BUILTIN｜SQLITE_FUNC_CONSTANT｜SQLITE_UTF8｜flags` | `like()` / `glob()`，`pUserData` 指向 `likeInfoNorm` / `likeInfoAlt` / `globInfo` |
| `WAGGREGATE` | `SQLITE_FUNC_BUILTIN｜SQLITE_UTF8`（`nc` 为真时加 `SQLITE_FUNC_NEEDCOLL`） | 聚合函数，同时填入 `xSFunc`/`xFinalize`/`xValue`/`xInverse` 四个入口 |
| `TEST_FUNC` | `SQLITE_FUNC_BUILTIN｜SQLITE_UTF8｜SQLITE_FUNC_INTERNAL｜SQLITE_FUNC_TEST｜SQLITE_FUNC_INLINE｜SQLITE_FUNC_CONSTANT` | 仅在未定义 `SQLITE_UNTESTABLE` 时编译的测试函数 |

函数体末尾的调用顺序是固定的：

1. `#ifndef SQLITE_OMIT_ALTERTABLE` 下调用 `sqlite3AlterFunctions()`；
2. 调用 `sqlite3WindowFunctions()`；
3. 调用 `sqlite3RegisterDateTimeFunctions()`；
4. 调用 `sqlite3RegisterJsonFunctions()`；
5. 调用 `sqlite3InsertBuiltinFuncs(aBuiltinFunc, ArraySize(aBuiltinFunc))`。

`sqlite3InsertBuiltinFuncs`（`src/callback.c`）把每个 `FuncDef` 插入全局哈希表 `sqlite3BuiltinFunctions`：哈希下标由 `SQLITE_FUNC_HASH(zName[0], nName)` 计算，宏定义为 `(((C)+(L))%SQLITE_FUNC_HASH_SZ)`，桶数 `SQLITE_FUNC_HASH_SZ` 定义为 23（`src/sqliteInt.h`）。若同桶已有同名函数，新条目挂到旧条目的 `pNext` 链上（`aDef[i].pNext = pOther->pNext; pOther->pNext = &aDef[i]`），因此同名不同元数的重载靠链式串起来，由 `matchQuality` 挑选。

`sqlite3RegisterBuiltinFunctions` 由 `sqlite3_initialize` 路径调用：`src/main.c` 的初始化函数在 `sqlite3GlobalConfig.isInit==0 && sqlite3GlobalConfig.inProgress==0` 时先 `memset(&sqlite3BuiltinFunctions, 0, sizeof(sqlite3BuiltinFunctions))`，再调用 `sqlite3RegisterBuiltinFunctions()`——所以全局表是「清零后重建」而非增量插入。

### 2. 每连接注册：`sqlite3RegisterPerConnectionBuiltinFunctions`

函数体只有一件事：

```
int rc = sqlite3_overload_function(db, "MATCH", 2);
assert( rc==SQLITE_NOMEM || rc==SQLITE_OK );
if( rc==SQLITE_NOMEM ){
  sqlite3OomFault(db);
}
```

`sqlite3_overload_function`（`src/main.c`）的语义是：若该名字已作为常规全局函数存在则什么都不做；若不存在，则创建一个永远抛错的函数 `sqlite3InvalidFunction`，错误消息为 `unable to use function %s in the requested context`。这样虚拟表才能通过 `xFindFunction` 覆盖 `MATCH`，同时全局名字解析仍然能找到这个占位符。

调用点在 `src/main.c` 的连接打开路径中：`sqlite3RegisterPerConnectionBuiltinFunctions(db)` 之后紧接 `rc = sqlite3_errcode(db)`，再之后是编译内扩展（`sqlite3BuiltinExtensions`）与自动扩展（`sqlite3AutoLoadExtensions`）的加载。注释明确说明此处注册内置函数但不读取数据库 schema，schema 读取被推迟到首次访问数据库时。

### 3. 标量函数与聚合函数的执行契约

**标量函数**：`xSFunc(sqlite3_context*, int argc, sqlite3_value** argv)` 一次性被调用，参数个数由 `sqlite3FindFunction` 按 `nArg` 匹配选定。参数个数超过 `SQLITE_MAX_FUNCTION_ARG` 的注册在 `src/main.c` 被拒（`nArg<-1 || nArg>SQLITE_MAX_FUNCTION_ARG` 返回错误），`SQLITE_MAX_FUNCTION_ARG` 默认 1000（`src/sqliteLimit.h`）。

**聚合函数**：`FuncDef` 里 `xSFunc` 充当 step 入口，`xFinalize` 负责产出最终值，`xValue` 产出分组内当前值，`xInverse` 用于窗口帧的逆序撤销。`sqlite3_aggregate_context(p, nByte)`（`src/vdbeapi.c`）在 `p->pMem->flags & MEM_Agg` 为 0 时调 `createAggContext` 首次分配，否则直接返回 `p->pMem->z` 这块已分配内存。

以几个真实实现为例：

- **`sum()` / `total()` / `avg()`** 共用 `sumStep` 与 `SumCtx`（含 `rSum`、`rErr`、`iSum`、`cnt`、`approx`、`ovrfl` 六个字段）。`sumStep` 用 `sqlite3_value_numeric_type` 判断输入类型：`SQLITE_NULL` 直接跳过；整数走 `sqlite3AddInt64` 精确累加，溢出则置 `p->ovrfl=1`、切到浮点近似；一旦遇到非整数输入就置 `p->approx=1`，转入 Kahan-Babushka-Neumaier 补偿求和（`kahanBabuskaNeumaierStep`，用 `rErr` 存误差项）。`sumFinalize` 在 `p->cnt>0` 时：若 `approx` 且 `ovrfl` 则 `sqlite3_result_error(context, "integer overflow", -1)`；否则浮点结果为 `p->rSum+p->rErr`（`rErr` 未溢出时）或 `p->rSum`，整数结果为 `p->iSum`。`totalFinalize` 永远返回浮点，空输入返回 0.0；`avgFinalize` 返回 `r/(double)p->cnt`。
- **`count()`** 用 `countStep`/`CountCtx`（字段 `n`）：`argc==0`（即 `count(*)`）或参数非 NULL 时 `p->n++`；`countFinalize` 用 `sqlite3_result_int64(context, p ? p->n : 0)`。
- **`min()` / `max()`** 共用 `minmaxStep`，靠 `sqlite3_user_data(context)` 区分方向：`max()` 的 user data 是 `(void*)-1`，`min()` 是 `db` 指针，所以 `max = sqlite3_user_data(context)!=0`。比较用 `sqlite3MemCompare(pBest, pArg, pColl)`，`pColl` 由 `sqlite3GetFuncCollSeq(context)` 得到。
- **`group_concat()` / `string_agg()`** 共用 `groupConcatStep` 与 `GroupConcatCtx`（含 `StrAccum str`）。`groupConcatStep` 把 `pGCC->str.mxAlloc` 设为 `db->aLimit[SQLITE_LIMIT_LENGTH]` 作为累积上限（该 limit 的默认值来自 `SQLITE_MAX_LENGTH`，默认 1000000000，见 `src/sqliteLimit.h` 与 `src/main.c` 的 `aHardLimit`）。分隔符在**值之前**追加，首个值不加分隔符——源码注释明确记录这是历史行为，为避免破坏兼容性不改。`groupConcatFinalize` 用 `sqlite3_result_str(context, &pGCC->str, SQLITE_XFER)` 交出缓冲区所有权。

**标志位与优化器**：`count`、`min`、`max` 在注册时带 `SQLITE_FUNC_ANYORDER`，`min`/`max` 另带 `SQLITE_FUNC_MINMAX`。`src/resolve.c` 的名字解析阶段读取 `pDef->funcFlags` 并把对应位翻译成 `NC_OrderAgg` / `NC_MinMaxAgg` 写进 `pNC2->ncFlags`，从而允许查询规划器打散这些聚合的输入顺序。

### 4. `sqlite3QuoteValue`

该函数按 `sqlite3_value_type(pValue)` 分派，产出 SQL 字面量文本：

- `SQLITE_FLOAT`：用 `sqlite3_str_appendf(pStr, "%!0.17g", ...)`，源码注释说明 17 位精度保证往返（round-trip）无损。
- `SQLITE_INTEGER`：用 `sqlite3_str_appendf(pStr, "%lld", sqlite3_value_int64(pValue))`。
- `SQLITE_BLOB`：先 `sqlite3StrAccumEnlarge(pStr, nBlob*2 + 4)`，再按字节展开成大写十六进制（查表 `hexdigits`，取值 `'0'`–`'9'`、`'A'`–`'F'`），输出形如 `X'...'`；成功后 `pStr->nChar` 置为 `nBlob*2 + 3`。
- `SQLITE_TEXT`：按 `bEscape` 选 `"%#Q"` 或 `"%Q"` 格式。
- 其他（`SQLITE_NULL`）：`sqlite3_str_append(pStr, "NULL", 4)`。

函数开头有 `assert( pStr!=0 && pStr->nChar==0 )`——当前实现要求累加器初始为空。

调用方是 `quoteFunc`（`src/func.c`）：它用 `sqlite3StrAccumInit(&str, db, 0, 0, db->aLimit[SQLITE_LIMIT_LENGTH])` 建累加器，再调 `sqlite3QuoteValue(&str, argv[0], SQLITE_PTR_TO_INT(sqlite3_user_data(context)))`，最后 `sqlite3_result_str(context, &str, SQLITE_XFER)`。`quote()` 的 user data 为 0（`FUNCTION(quote, 1, 0, 0, quoteFunc)`），`unistr_quote()` 的 user data 为 1（`FUNCTION(unistr_quote, 1, 1, 0, quoteFunc)`），因此 `bEscape` 为真时走 `%#Q`，即用 UNISTR 转义控制字符。

## 关键取舍

- **源码事实：**内置函数表是 `static FuncDef aBuiltinFunc[]`，元素在初始化完成后只读；`sqlite3InsertBuiltinFuncs` 用 `SQLITE_FUNC_HASH` 定位桶、`SQLITE_FUNC_HASH_SZ` 为 23。**架构推论：**用「一次注册、全连接共享」的全局表换取查找开销恒定，代价是没法给内置函数做每连接的差异化配置；因此只有确实需要连接级差异的函数（`MATCH`）才走 `sqlite3RegisterPerConnectionBuiltinFunctions` 这条旁路。
- **源码事实：**`sqlite3RegisterBuiltinFunctions` 在注册内置表之后，还依次调用 `sqlite3AlterFunctions`、`sqlite3WindowFunctions`、`sqlite3RegisterDateTimeFunctions`、`sqlite3RegisterJsonFunctions`。**架构推论：**把各功能域的注册入口收拢到一个总入口，让 `sqlite3_initialize` 只需一处调用即可完成全部函数注册；代价是函数归属被摊薄到多个文件，追踪某个函数来源必须回到各自的注册表。
- **源码事实：**`FUNCTION` 宏带 `SQLITE_FUNC_CONSTANT`（在 `src/sqliteInt.h` 的注释里等价于 `SQLITE_DETERMINISTIC`），而 `VFUNCTION` 不带；`SFUNCTION` 带 `SQLITE_FUNC_UNSAFE` 与 `SQLITE_DIRECTONLY`。**架构推论：**常量性标志让查询规划器能把函数调用从循环里提出来、把 `unlikely()`/`likely()` 这类提示函数优化成零成本；副作用函数必须显式标记为 unsafe 并限制在受信上下文调用，否则「常量折叠」会改变程序语义。
- **源码事实：**`INLINE_FUNC` 注册的函数把 `xSFunc` 填成 `noopFunc`，而 `noopFunc` 是 `#define noopFunc versionFunc`，源码注释写明「noopFunc will never be called」。**架构推论：**这类函数（`coalesce`、`ifnull`、`iif`、`if`、`unlikely`、`likely`、`likelihood`）由代码生成器展开成 VDBE 指令而非真正的函数调用，因此短路的未被使用参数根本不会被求值；代价是这些函数没有通用的 C 侧执行路径。
- **源码事实：**`sum()` 精确整数累加与浮点累加并存，溢出时 `sumFinalize` 抛 `"integer overflow"` 错误，而 `total()` 用同一个 `sumStep` 但在 `totalFinalize` 里永远返回浮点且空输入返回 0.0。**架构推论：**用「同一套累积状态 + 不同终结器」实现 `sum`/`total`/`avg` 三种语义，避免重复三份累积逻辑；代价是 `total`/`avg` 即使输入全整数也走浮点路径，精度表现与 `sum` 不一致。
- **源码事实：**`groupConcatStep` 把分隔符追加在值之前，源码注释称这是「tragic」的历史行为，且顺序未公开、理论上可改但因兼容性风险不改。**架构推论：**为兼容既有数据生成逻辑而保留反直觉的输出顺序，是「行为兼容优先于实现优雅」的取舍；也直接导致 `groupConcatInverse` 实现复杂——需要用 `pnSepLengths` 数组记录每个分隔符的实际长度才能精确回退。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/func.c` | `sqlite3RegisterBuiltinFunctions` | 持有 `aBuiltinFunc[]` 静态函数表，在初始化时依次触发 alter/window/datetime/json 的注册，并把内置表插入全局哈希。 |
| `src/func.c` | `sqlite3RegisterPerConnectionBuiltinFunctions` | 每连接注册：调用 `sqlite3_overload_function(db, "MATCH", 2)` 建立 `MATCH` 占位函数，OOM 时调 `sqlite3OomFault`。 |
| `src/func.c` | `sqlite3QuoteValue` | 按值类型把 `sqlite3_value` 渲染为 SQL 字面量：浮点用 `%!0.17g`、整数用 `%lld`、BLOB 转大写十六进制 `X'...'`、文本用 `%Q`/`%#Q`、NULL 输出 `NULL`。 |

## 与主核 / AI 的关系

主核的函数层可参照三点：**一是把「注册」与「执行」分成静态表和分派回调两步**，让新增函数只需往一张表里加条目而不改动分派逻辑；**二是用标志位把确定性、副作用、是否需要 collation 这些属性显式声明出来**，供规划器消费；**三是聚合函数采用 step/final/value/inverse 四入口契约**，配合 `sqlite3_aggregate_context` 式的「首次分配、后续复用」累积状态，能自然支撑窗口帧的逆序撤销。

## 参考

- [SQLite 官方文档：Built-In Scalar SQL Functions](https://sqlite.org/lang_corefunc.html) —— 标量函数的语义与参数个数约定。
- [SQLite 官方文档：Built-In Aggregate Functions](https://sqlite.org/lang_aggfunc.html) —— `sum()` / `total()` / `avg()` / `count()` / `min()` / `max()` / `group_concat()` 的返回值与 NULL 处理规则。
- [GitHub 官方镜像源码：src/func.c](https://github.com/sqlite/sqlite/blob/master/src/func.c) —— 对照内置函数注册表与各函数实现体。
- [SQLite 官方文档：Compiler Options](https://sqlite.org/compile.html) —— `SQLITE_ENABLE_MATH_FUNCTIONS`、`SQLITE_SOUNDEX`、`SQLITE_UNTESTABLE` 等决定内置函数是否进入注册表的编译期开关。
