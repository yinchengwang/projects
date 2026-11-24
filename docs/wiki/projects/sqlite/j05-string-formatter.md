---
project: sqlite
feature: string-formatter
page_type: feature
schema_version: 1
refs:
  - path: src/printf.c
    symbols: [sqlite3_str_vappendf, sqlite3_str_appendf, sqlite3_str_finish]
---

# 字符串格式化

## 一句话职责

给全库提供一套自带的 printf 实现和可增长的动态字符串累加器：把格式串与变参渲染成字节串，并把结果追加进 `sqlite3_str` 对象里（这个对象可以挂在栈上、用固定缓冲区起步，也可以由 `sqlite3_str_new()` 在堆上分配）。

## 机制是什么

这一层是 SQLite 内部最底层的**文本构造工具**，位于 `src/printf.c`。它有两个东西：

1. **渲染器** `sqlite3_str_vappendf()`：读一条格式串，把每个转换字段渲染成字节，追加进目标累加器 `sqlite3_str`。
2. **累加器** `sqlite3_str`（结构体定义在 `src/sqliteInt.h`）：持有 `zText`（已收集的文本）、`nAlloc`（`zText` 的已分配容量）、`mxAlloc`（允许的最大字节数，`0` 表示永不 malloc）、`nChar`（当前长度）、`accError`（`SQLITE_NOMEM` 或 `SQLITE_TOOBIG`）、`printfFlags`。

累加器有两种用法：

- **固定缓冲区起步**：调用 `sqlite3StrAccumInit(p, db, zBase, n, mx)`，把 `zBase`（可以是一段栈上数组）作为初始缓冲区。`sqlite3VMPrintf()`、`sqlite3_snprintf()`、`sqlite3_vsnprintf()`、`renderLogMsg()` 走的都是这条路。`mx` 传 `0` 时累加器**永远不做堆分配**，写不下就报 `SQLITE_TOOBIG`。
- **堆上动态增长**：调用 `sqlite3_str_new(db)` 拿一个堆分配的累加器，`mx` 取 `db->aLimit[SQLITE_LIMIT_LENGTH]`（`db` 为 NULL 时取 `SQLITE_MAX_LENGTH`）。用完必须调 `sqlite3_str_finish()` 收尾，否则 `sqlite3_str` 自身这块内存会泄漏。

格式串支持 C 标准之外的一批 SQLite 自有转换符（`%q` 转义单引号、`%Q` 转义并加引号、`%w` 转义双引号、`%z` 接管一块已分配内存、`%j` / `%J` 生成 JSON 字符串字面量、`%r` 生成序数词、`%T` / `%S` 输出词法单元与表名），以及 `!` 标志（把宽度与精度按字符数而非字节数计算）和 `,` 标志（千位分隔符）。

## 怎么实现的

### 转换符的查表

`sqlite3_str_vappendf()` 的入口处有一个断言：`assert( pAccum->nChar>0 || (pAccum->printfFlags&SQLITE_PRINTF_MALLOCED)==0 )`，即累加器若是从 malloc 来的空串，不能处于 `SQLITE_PRINTF_MALLOCED` 状态——这是后面 `%z` 优化成立的前提。

格式串扫描是手写状态机。遇到非 `%` 字符时，用 `strchrnul()`（有 `HAVE_STRCHRNUL` 时）一次性跳过整段普通文本，交给 `sqlite3StrAppend64()` 一次性追加；没有 `HAVE_STRCHRNUL` 时退化成 `strchr()` 加一次 `strlen()`。

转换符本身查一张哈希表 `fmtinfo[25]`（`src/printf.c` 的静态常量数组）。查表方式：先用 `idx = ((unsigned)c) % 25` 取桶，再看 `fmtinfo[idx].fmttype==c`，不中就顺着 `fmtinfo[idx].iNxt` 走链，最多比两下。表被手工排成 25 个桶是为了让查表是常数时间；`src/printf.c` 里保留了当初用来生成这个布局的 TCL 脚本（在 `#if 0` 块里）。

编译期定义了 `SQLITE_EBCDIC` 时不走哈希，而是对 `fmtinfo` 做线性查找——注释写明原因是"哈希表只对 ASCII 有效"。

每个转换符在 `fmtinfo` 里对应一条 `et_info` 记录，含格式字母、进制 `base`、标志位 `flags`、转换范式 `type`（`etRADIX` / `etFLOAT` / `etEXP` / `etGENERIC` / `etSIZE` / `etSTRING` / `etDYNSTRING` / `etPERCENT` / `etCHARX` / `etESCAPE_q` / `etESCAPE_Q` / `etTOKEN` / `etSRCITEM` / `etPOINTER` / `etESCAPE_w` / `etORDINAL` / `etDECIMAL` / `etESCAPE_j` / `etESCAPE_J` 等）、数字字符集偏移、前缀偏移。查不到时 `infop` 退回 `&fmtinfo[0]`、`xtype` 置 `etINVALID`，随后落到 `switch` 的 `default` 分支直接 `return`。

### 标志、宽度与精度

标志解析是一个 `do{...}while(!done)` 循环，认识的标志是 `-`（左对齐）、`+`（正号前缀）、空格、`#`（替代形式）、`!`（`flag_altform2`）、`0`（补零）、`,`（千位分隔符）、`l` 与 `ll`（`flag_long` 取 1 或 2）。

宽度与精度的上界都受 `SQLITE_PRINTF_PRECISION_LIMIT` 约束，但**只在它被定义时**才生效：源码里三处（数字字面量宽度、`*` 宽度、`.` 精度）都包在 `#ifdef SQLITE_PRINTF_PRECISION_LIMIT` 里，未定义该宏时宽度只受 `width = wx & 0x7fffffff` 截断。

浮点精度另有硬上限 `SQLITE_FP_PRECISION_LIMIT`：定义在 `src/printf.c`，取 `SQLITE_PRINTF_PRECISION_LIMIT` 与 `100000000`（一亿）两者的较小者——`SQLITE_PRINTF_PRECISION_LIMIT` 小于一亿时取它，否则取一亿；`SQLITE_PRINTF_PRECISION_LIMIT` 未定义时直接取一亿。`src/util.c` 的 `sqlite3FpDecode()` 断言 `iRound>=-100000000`，正对应这个上限。

### 整数路径

整数转换（`etDECIMAL` / `etRADIX` / `etPOINTER` / `etORDINAL`）先取参数，再按 `infop->flags & FLAG_SIGNED` 分成有符号与无符号两路。有符号且值为负时用 `longvalue = ~v; longvalue++` 取补码绝对值，避免对 `SMALLEST_INT64` 取负溢出。

渲染缓冲区优先用栈上的 `buf[etBUFSIZE]`。`etBUFSIZE` 就是 `SQLITE_PRINT_BUF_SIZE`，在 `src/printf.c` 里默认定义为 `70`，注释说明可以在小栈机器上重新定义成更小的值。走栈缓冲的条件是 `precision < etBUFSIZE-10-etBUFSIZE/3`；不满足则改用 `printfTempBuf()` 申请临时堆缓冲。

`printfTempBuf()` 里有一道防滥用闸门：若请求尺寸 `n` 大于累加器的 `nAlloc` 且大于 `mxAlloc`，**在分配之前**就置 `SQLITE_TOOBIG` 并返回 0——注释写明这是为了防止恶意 SQL 用 `printf()` 的精度或宽度字段请求巨额分配。临时缓冲用完在循环末尾 `sqlite3DbFree(pAccum->db, zExtra)` 释放。

千位分隔符（`,` 标志）是在数字渲染完之后往数字串中间插逗号，插入个数为 `(length-1)/3`。

`%r`（序数词）用一张 6 字符的静态串 `zOrd[] = "thstndrd"`，按个位与十位判断后缀。

### 浮点路径

浮点（`etFLOAT` / `etEXP` / `etGENERIC`）不在 `src/printf.c` 里做十进制转换，而是调 `src/util.c` 的 `sqlite3FpDecode()` 拿到有效数字串 `s.z`、数字个数 `s.n` 与十进制小数点位置 `s.iDP`。有效位数上限是 `flag_altform2 ? 20 : 16`——`!` 标志把上限从 16 位抬到 20 位。

精度默认值为 `6`（`precision<0` 时置 6），随后按 `etFLOAT` / `etGENERIC` / `etEXP` 分别算出舍入位数 `iRound`（分别是 `-precision` / `precision` / `precision+1`）。`etGENERIC` 会先根据指数 `exp` 是否落在 `-4` 到 `precision` 之间决定降级成 `etFLOAT` 还是 `etEXP`。

特殊值处理：`s.isSpecial==2` 是 NaN——有 `0` 标志时输出 `"null"`（长度 4），否则输出 `"NaN"`（长度 3）；`s.isSpecial` 为其他特殊值时，正无穷按 `0` 标志走 `s.z[0]='9'; s.iDP=1000; s.n=1` 的分支，负无穷输出 `"-Inf"`，正无穷输出 `"Inf"`。

缓冲区不够时的降级路径：当 `szBufNeeded + pAccum->nChar >= pAccum->nAlloc` 且 `pAccum->mxAlloc==0`（即累加器是 `sqlite3_snprintf()` 这类固定缓冲模式），改用 `sqlite3_malloc64(szBufNeeded)` 申请临时空间渲染，之后再 `memcpy` 回目标缓冲区；申请失败则 `sqlite3StrAccumSetError(pAccum, SQLITE_NOMEM)` 并返回。若 `mxAlloc!=0`，则调 `sqlite3StrAccumEnlarge()` 扩容；扩容返回的字节数不足 `szBufNeeded` 时，把 `width` 与 `length` 双双置 0 并 `break`，本次转换退化为输出空内容。

### 字符串路径

`%s` / `%z`：`%z` 的参数是已由 SQLite 分配器分配好的字符串，渲染器不复制它，而是把 `zExtra = bufpt` 记下，最后由 `sqlite3DbFree()` 释放。

`%z` 有一条专门的优化：当 `pAccum->nChar==0 && pAccum->mxAlloc && width==0 && precision<0 && pAccum->accError==0` 五个条件同时成立时，直接把传入的那块内存接管为累加器的 `zText`，把 `nAlloc` 设为 `sqlite3DbMallocSize(pAccum->db, bufpt)`，把 `nChar` 设为 `0x7fffffff & (int)strlen(bufpt)`，并置上 `SQLITE_PRINTF_MALLOCED` 标志。源码注释点名这是给 `sqlite3_mprintf("%z...")` 用的。`src/shell.c.in` 里 `sqlite3_mprintf("%z%sname GLOB '%q'", z1, zSep2, z)` 这类调用正是吃这条路径。

精度语义：带 `!` 标志时精度按**字符数**算（循环里用 `SQLITE_SKIP_UTF8(z)` 跳过 UTF-8 续接字节），不带时按**字节数**算。宽度对齐在 `adjust_width_for_utf8` 标签处统一处理：带 `!` 标志且 `width>0` 时，扫描结果里每个 `0x80` 续接字节都会让 `width` 加一，保证补空格数按字符算。

`%c`：`bArgList` 模式下取文本参数并按 UTF-8 续接字节规则最多取 4 字节；否则取 `unsigned int` 码点，交给 `src/utf.c` 的 `sqlite3AppendOneUtf8Character()` 编码成 UTF-8。`%c` 还支持一个 SQLite 特有的用法：精度大于 1 时把字符按 2 的幂次重复（`nPrior *= 2`）。

### 转义类转换

`%q` / `%Q` / `%w` 共用一段实现。`%q` 把单引号翻倍；`%Q` 额外把整个串包在单引号里；`%w` 把双引号翻倍且强制清掉 `flag_alternateform`。参数为 NULL 时，`%Q` 替换成字面量 `"NULL"`，`%q` 与 `%w` 替换成 `"(NULL)"`。

带 `#`（`flag_alternateform`）时做 unistr 风格的反斜杠转义：反斜杠自身写成 `\\`，控制字符写成 `\u00XX`（用 `aHex` 查表）。`%Q` 的 `#` 形式只在串里**确实存在控制字符**时才启用（`nCtrl` 非零），否则把 `flag_alternateform` 清零退回普通形式。

`%Q` 的 `#` 形式还有一种特殊产出：前缀写成 `unistr('`（8 字节）且 `needQuote` 置 2，末尾补 `')`。

渲染缓冲不够时同样走 `printfTempBuf()`：`n` 超过 `etBUFSIZE` 才申请临时堆缓冲。

`%j` / `%J` 生成 JSON 字符串字面量：`%j` 不带外层引号，`%J` 带外层双引号。控制字符（`<=0x1f`）、双引号、反斜杠都要转义；能被 `((1u<<ch)&0x3700)` 命中的控制字符映射成 `btn?fr` 里的单字母短转义（`\b` `\t` `\n` `\f` `\r`），其余写成 `\u00XX`。参数为 NULL 时 `%J` 输出 `null`。这个分支不走"渲染到临时缓冲再追加"的统一路径，而是直接往累加器里逐段 `sqlite3_str_append()` / `sqlite3_str_appendchar()`，最后自己做宽度对齐。

### 内部专用转换符

`%T`（词法单元）与 `%S`（表源项）都先检查 `pAccum->printfFlags & SQLITE_PRINTF_INTERNAL`，没置位就直接 `return`——这两个转换符只对内部调用开放。

`%T` 带 `#` 时取 `Expr*` 并用 `Expr.u.zToken`，否则取 `Token*` 用 `pToken->z` 与 `pToken->n`。`%S` 输出 `SrcItem`：有别名且没有 `!` 标志时输出别名；否则输出 `数据库名.表名`（当 `fg.fixedSchema==0 && fg.isSubquery==0 && u4.zDatabase!=0` 时才补库名）；都不是时如果是子查询，按 `pSel->selFlags` 分别输出 `(join-%u)`、`%u-ROW VALUES CLAUSE` 或 `(subquery-%u)`——这三个子格式又调 `sqlite3_str_appendf()`。

### 累加器的增长

缓冲区增长由 `sqlite3StrAccumEnlarge()` 负责：

- 若 `p->accError` 非零，直接返回 0（已出错就不再尝试）。
- 若 `p->mxAlloc==0`（永不 malloc 模式），置 `SQLITE_TOOBIG` 并返回 `p->nAlloc - p->nChar - 1`。
- 否则计算 `szNew = p->nChar + N + 1`；若 `szNew+p->nChar<=p->mxAlloc` 则再额外加上 `p->nChar`，即**指数增长**（注释写明是为了避免频繁调用本例程）。
- 若 `szNew > p->mxAlloc`，先 `sqlite3_str_reset(p)` 再置 `SQLITE_TOOBIG` 并返回 0。
- 分配走 `sqlite3DbRealloc(p->db, zOld, p->nAlloc)`（`p->db` 非空时）或 `sqlite3Realloc(zOld, p->nAlloc)`（`p->db` 为空时）。成功后 `p->nAlloc` 被回写为 `sqlite3DbMallocSize(p->db, zNew)`——即真实占用而不是申请值。

追加接口 `sqlite3_str_append()` / `sqlite3StrAppend64()` 走快路径：空间够就直接 `memcpy` 并推进 `nChar`；不够则调 `SQLITE_NOINLINE` 标注的 `enlargeAndAppend()`。这个辅助函数放在这里是为了让 `sqlite3_str_append()` 保持"快调用"语义（尾递归改成了普通调用）。

### 三个公开入口的实际关系

`sqlite3_str_appendf(StrAccum *p, const char *zFormat, ...)` 是最薄的包装：起 `va_list`，调 `sqlite3_str_vappendf(p, zFormat, ap)`，收尾。函数上方的注释说 `bFlags` 参数能含 `SQLITE_PRINTF_INTERNAL` 标志位——但函数签名里实际并没有 `bFlags` 参数，标志是通过 `p->printfFlags` 字段传递的，源码注释与签名不一致。

`sqlite3_str_finish(sqlite3_str *p)` 收尾一条由 `sqlite3_str_new()` 创建的动态字符串：先调 `sqlite3StrAccumFinish(p)` 拿到文本，再 `sqlite3_free(p)` 释放累加器自身。**它有两个特殊分支**：若 `p==0` 或 `p==(sqlite3_str*)&sqlite3OomStr`，返回 0 且不做任何事。

`sqlite3OomStr` 是 `src/global.c` 里的一个全局常量单例，注释写明它是"`sqlite3_malloc()` 失败时用来顶替真实对象的那个 sqlite3_str"，它的 `accError` 字段直接初始化为 `SQLITE_NOMEM`。`sqlite3_str_new()` 在自身 malloc 失败时返回的就是这个单例的地址（`p = (sqlite3_str*)&sqlite3OomStr`），因此调用方拿到的指针永远不为 NULL，`sqlite3_str_new()` 的失败是通过 `sqlite3_str_errcode()` 报出来的而不是通过空指针。

`sqlite3StrAccumFinish()` 本身做两件事：把 `p->zText[p->nChar]` 写成 0 终止符；若 `p->mxAlloc>0` 且当前不是 `SQLITE_PRINTF_MALLOCED` 状态（即还在用调用方提供的静态缓冲区），则走 `strAccumFinishRealloc()` 把内容 `sqlite3DbMallocRaw()` 到一块新堆内存上并置上 `SQLITE_PRINTF_MALLOCED` 标志，把原缓冲区内容 `memcpy` 过去。

### 三个调用方的真实用法

- `src/func.c` 的 `printfFunc()`：SQL 函数 `printf()` / `format()` 的实现。它构造一个 `PrintfArguments` 结构（`x.nArg = argc-1`、`x.nUsed = 0`、`x.apArg = argv+1`），调 `sqlite3StrAccumInit(&str, db, 0, 0, db->aLimit[SQLITE_LIMIT_LENGTH])`，把 `str.printfFlags` 置为 `SQLITE_PRINTF_SQLFUNC`，再调 `sqlite3_str_appendf(&str, zFormat, &x)`。因为置了 `SQLITE_PRINTF_SQLFUNC`，`sqlite3_str_vappendf()` 入口会额外从 `va_list` 里取出一个 `PrintfArguments*`（`pArgList = va_arg(ap, PrintfArguments*)`），之后所有参数都通过 `getIntArg()` / `getDoubleArg()` / `getTextArg()` 从这个参数列表里取，而不是从 `va_arg` 取。这三个取值函数在参数用尽时返回 0 / 0.0 / NULL。
- `src/btree.c` 的完整性检查：把 `errMsg` 这个 `sqlite3_str` 字段嵌在检查上下文里，`sqlite3StrAccumInit(&sCheck.errMsg, 0, zErr, sizeof(zErr), SQLITE_MAX_LENGTH)`，并置 `sCheck.errMsg.printfFlags = SQLITE_PRINTF_INTERNAL`（`%T` / `%S` 因此可用）。报错时先 `sqlite3_str_append(&pCheck->errMsg, "\n", 1)` 分隔，再调 `sqlite3_str_vappendf(&pCheck->errMsg, zFormat, ap)`。这条路径直接用了 `sqlite3_str_vappendf()` 而不是 `sqlite3_str_appendf()`，因为它本身已经是变参的。
- `src/tokenize.c` 的 `sqlite3CreateFunc` 类名改写路径：`sqlite3_str_new(db)` 拿累加器，用 `sqlite3_str_appendf(pStr, "\"%w\"", zId)` 生成带双引号且内部双引号被翻倍的标识符，最后 `sqlite3_str_finish(pStr)` 交出字符串。

### SQL 函数结果的所有权转移

`src/func.c` 的 `printfFunc()` 结束时调 `sqlite3_result_str(context, &str, SQLITE_XFER)`。`src/vdbeapi.c` 的 `sqlite3_result_str()` 在 `SQLITE_XFER` 语义下把内容以 `SQLITE_DYNAMIC` 交给结果槽，然后用 `sqlite3StrAccumInit(pStr, pStr->db, 0, 0, pStr->mxAlloc)` 把累加器重置为空壳但保留原有的 `mxAlloc` 上限。`SQLITE_TOOBIG` 错误映射到 `sqlite3_result_error_toobig()`，`SQLITE_NOMEM` 映射到 `sqlite3_result_error_nomem()`。

## 关键取舍

- **自带的 printf 而不是 libc 的 printf**：理由写在 `sqlite3_snprintf()` 的注释里——"它忽略当前的 locale 设置。这对 SQLite 很重要，因为我们不能在某些 locale 下用 `,` 代替 `.` 作为小数点"。标准 `printf` 的浮点输出受 locale 影响，而 SQLite 的输出必须是可重现的、跨平台一致的。自带的代价是要自己实现浮点十进制转换（`src/util.c` 的 `sqlite3FpDecode()`）和 UTF-8 长度计算。
- **栈缓冲 + 临时堆缓冲两级**：`etBUFSIZE`（默认 `SQLITE_PRINT_BUF_SIZE` 为 70）的栈缓冲覆盖绝大多数转换，超出时才申请临时堆缓冲。`printfTempBuf()` 把"申请临时缓冲"和"申请输出空间"这两件事合并成同一道闸门：`n>pAccum->nAlloc && n>pAccum->mxAlloc` 触发 `SQLITE_TOOBIG`，防止用宽度/精度字段撑爆内存。
- **`mxAlloc==0` 表示"永不 malloc"，而不是"无上限"**：`sqlite3StrAccumInit()` 的注释明确写了 `mx==0` 时 "no memory allocations will ever occur"。这一个语义差别让 `sqlite3_snprintf()` 与 `sqlite3_str_new()` 走同一套渲染代码，却有完全不同的溢出行为。
- **上限检查发生在分配之前**：`printfTempBuf()` 的注释把这条写成了显式设计意图——"在内存分配之前做尺寸检查，以防止恶意 SQL 使用 printf() 函数的精度或宽度字段请求巨额分配"。这是把安全检查放在资源获取路径上的典型做法。
- **增长用指数策略**：`sqlite3StrAccumEnlarge()` 里 `szNew += p->nChar` 的条件是 `szNew+p->nChar<=p->mxAlloc`，注释写明是为了"避免不得不太频繁地调用本例程"。代价是一次分配出的缓冲区尺寸会超过当前所需，但换来了摊还后的 O(n) 总拷贝量。
- **内部转换符用标志位隔离**：`%T`（词法单元）与 `%S`（表源项）会泄露内部对象指针，因此在 `sqlite3_str_vappendf()` 里用 `SQLITE_PRINTF_INTERNAL` 标志位做闸门。`sqlite3VMPrintf()` 置位这个标志，`sqlite3_vmprintf()`（公开 API）不置——同一份渲染代码，用途不同可用的转换符集合也不同。`src/loadext.c` 的 `sqlite3_api_routines` 数组把 `sqlite3_str_new` / `sqlite3_str_finish` / `sqlite3_str_appendf` 暴露给扩展。
- **`%z` 用"接管"代替"复制"**：`%z` 的优化路径把调用方已经分配好的字符串直接当作累加器的初始内容，避免一次额外的拷贝。但这条路径要求五个条件同时成立（累加器为空、`mxAlloc` 非零、无宽度、无精度、无错误），任何一条不满足就退回普通路径。
- **OOM 用单例对象表达**：`sqlite3_str_new()` 分配失败时不返回 NULL，而是返回全局常量 `sqlite3OomStr`（其 `accError` 预置为 `SQLITE_NOMEM`）。这让所有调用方都不必判空指针，错误统一通过 `sqlite3_str_errcode()` 暴露。代价是调用方必须知道 `sqlite3OomStr` 这个特殊值——`sqlite3_str_finish()` 与 `sqlite3_str_free()` 里都要显式判它。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/printf.c | sqlite3_str_vappendf | 核心渲染器：解析格式串、查 `fmtinfo` 哈希表、按转换类型渲染进 `sqlite3_str` 累加器 |
| src/printf.c | sqlite3_str_appendf | `sqlite3_str_vappendf` 的变参包装，起止 `va_list` 后直接转发 |
| src/printf.c | sqlite3_str_finish | 收尾由 `sqlite3_str_new()` 创建的动态字符串，交出文本并释放累加器自身；对 `sqlite3OomStr` 与 NULL 返回 0 |
| src/printf.c | sqlite3StrAccumInit | 初始化累加器：设定 `zText` / `nAlloc` / `mxAlloc` / `nChar` / `accError` / `printfFlags` |
| src/printf.c | sqlite3StrAccumEnlarge | 累加器扩容：指数增长策略、超 `mxAlloc` 置 `SQLITE_TOOBIG`、走 `sqlite3DbRealloc` 分配 |
| src/printf.c | sqlite3StrAccumFinish | 给文本补 0 终止符；静态缓冲区模式下转成堆分配并置 `SQLITE_PRINTF_MALLOCED` |
| src/printf.c | printfTempBuf | 临时渲染缓冲的申请闸门：超 `mxAlloc` 时在分配前置 `SQLITE_TOOBIG` |
| src/printf.c | sqlite3StrAccumSetError | 置错误码（`SQLITE_NOMEM` / `SQLITE_TOOBIG`）并按需 `sqlite3_str_reset` |
| src/printf.c | fmtinfo | 25 项的静态转换符描述表，按 `c % 25` 分桶哈希查找 |
| src/printf.c | sqlite3VMPrintf | 内部格式化入口：置 `SQLITE_PRINTF_INTERNAL`，走 `sqlite3VMPrintf` 的 `sqlite3MPrintf` 包装 |
| src/printf.c | sqlite3_str_new | 堆上分配累加器，`mxAlloc` 取 `db->aLimit[SQLITE_LIMIT_LENGTH]` 或 `SQLITE_MAX_LENGTH` |
| src/global.c | sqlite3OomStr | `sqlite3_str_new()` 分配失败时返回的单例对象，`accError` 预置 `SQLITE_NOMEM` |
| src/sqliteInt.h | struct sqlite3_str | 累加器结构定义与 `SQLITE_PRINTF_INTERNAL` / `SQLITE_PRINTF_SQLFUNC` / `SQLITE_PRINTF_MALLOCED` 三个标志位 |
| src/func.c | printfFunc | SQL 函数 `printf()` / `format()` 的实现，走 `SQLITE_PRINTF_SQLFUNC` 的参数列表模式 |
| src/vdbeapi.c | sqlite3_result_str | 接收 `sqlite3_str` 作为 SQL 函数结果；`SQLITE_XFER` 语义下转移内容所有权 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 有自己的一套字符串工具，与 SQLite 这一层不是同一套机制：主核在 `engine/db/` 内自建字符串与缓冲区管理，不引入 SQLite 的 `sqlite3_str`。SQLite 这一层是一个**不依赖 SQL 语义**的纯字节拼接器，它的格式串语法、宽度/精度语义、错误码约定自成体系，不与 SQL 层的字符串函数（L 组）共享任何逻辑。

**架构推论：** 嵌入式 SDK `engine/src/sdk/` 直接依赖 SQLite，SQLite 层的 `sqlite3_str` 正是它拼接 SQL 文本与构造诊断信息的现成手段——SDK 不必自己实现变参格式化，只需 `sqlite3_str_new()` / `sqlite3_str_appendf()` / `sqlite3_str_finish()` 三步即可得到可增长的文本缓冲，且能拿到 `sqlite3_str_errcode()` 的错误语义。

**架构推论：** 这一层的 `%q` / `%Q` / `%w` 转义语义与主核的 SQL 构造需求直接对齐：把不可信输入拼进 SQL 语句时必须做引号转义，SQLite 在内部用 `%Q`（包引号并翻倍单引号）承担这个职责，而 `sqlite3_str_appendf()` 是它暴露给上层的入口。

## 参考

- SQLite 动态字符串接口文档：<https://sqlite.org/c3ref/str_append.html>
- SQLite 内置 printf 转换符说明：<https://sqlite.org/printf.html>
- SQLite `printf()` SQL 函数文档：<https://sqlite.org/lang_corefunc.html>
- SQLite 编译期选项 `SQLITE_PRINTF_PRECISION_LIMIT`：<https://sqlite.org/compile.html>
- SQLite 源码镜像 `src/printf.c`：<https://github.com/sqlite/sqlite/blob/master/src/printf.c>
- SQLite 源码镜像 `src/sqliteInt.h`（`struct sqlite3_str` 与 `SQLITE_PRINTF_*` 标志）：<https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h>
