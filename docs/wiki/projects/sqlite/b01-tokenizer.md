---
project: sqlite
feature: tokenizer
page_type: feature
schema_version: 1
refs:
  - path: src/tokenize.c
    symbols: [sqlite3GetToken, sqlite3RunParser, sqlite3IsIdChar]
---

# 词法分析器

## 一句话职责

词法分析器把一段 SQL 文本切成一串记号（token），逐个喂给 Lemon 生成的语法分析器；同时负责判定一个词是关键字还是普通标识符。

## 机制是什么

边界（依据条目 `boundary`）：负责把 SQL 文本切分成记号流（标识符、字面量、运算符、注释），以及关键字与标识符的区分；不负责语法树的构造（B02），不负责名称到对象的解析（B03）。

整个机制由 `src/tokenize.c` 一个文件承载，核心是两个对外函数加一个字符分类辅助函数：

- `sqlite3GetToken()`：扫描器本体。输入一个字节指针，输出该记号的长度（字节数），并通过输出参数 `*tokenType` 给出记号类型。它的返回值是「消耗了多少字节」而不是「记号是什么」——类型走输出参数。
- `sqlite3RunParser()`：驱动循环。它反复调用 `sqlite3GetToken()` 取下一个记号，做完空白/注释/长度/中断等预处理后调用 `sqlite3Parser()` 把记号推进 LALR(1) 自动机。
- `sqlite3IsIdChar()`：把文件内的 `IdChar` 宏对外暴露，供 `src/alter.c`、`src/main.c` 与本文件内的归一化代码使用。

记号类型最终都落到 Lemon 文法定义的 `TK_*` 常量上。`src/parse.y` 的 `%token` 声明顺序决定了这些常量的数值大小关系，文法末尾有一条明确约束：

> The TK_SPACE, TK_COMMENT, and TK_ILLEGAL tokens must be the last three tokens. The parser depends on this.

`sqlite3RunParser()` 正是靠这个数值顺序做快速分流：它先判断 `tokenType>=TK_SPACE`（或窗口函数版本下的 `tokenType>=TK_WINDOW`），命中后才去走空白/注释/非法记号的分支，从而把绝大多数普通记号直接推给解析器。

关键字与标识符的区分走两级：

1. **字符类先分流。** 文件顶部用 `CC_*` 常量定义了一套字符类别，`aiClass[]` 是一张 256 字节的查表。表头注释写明了理由：直接对字符做 `switch` 会编译成二分查找，而查表法快得多；为保证编译器一定生成查表，所有类别值必须是小整数且都在同一个 `switch` 里被用到。
2. **再查关键字哈希表。** 落到 `CC_KYWD0` 分支时，函数体调用 `keywordCode()`，该函数来自 `#include "keywordhash.h"`。头文件说明写明这份实现由 `tool/mkkeywordhash.c` 生成，查询失败时返回 `TK_ID`。

## 怎么实现的

### 字符分类表

`aiClass[]` 把每个字节映射到一个 `CC_*` 类别。ASCII 版本下与识别相关的类别有：

| 类别 | 值 | 含义 |
|---|---|---|
| `CC_X` | 0 | 字母 `x`，或 BLOB 字面量起点 |
| `CC_KYWD0` | 1 | 关键字的首字母 |
| `CC_KYWD` | 2 | 字母或 `_`，可用于关键字 |
| `CC_DIGIT` | 3 | 数字 |
| `CC_DOLLAR` | 4 | `$` |
| `CC_VARALPHA` | 5 | `@`、`#`、`:`，字母型 SQL 变量 |
| `CC_VARNUM` | 6 | `?`，数字型 SQL 变量 |
| `CC_SPACE` | 7 | 空白字符 |
| `CC_QUOTE` | 8 | `"`、`'`、`` ` `` |
| `CC_QUOTE2` | 9 | `[`，`[...]` 风格引用标识符 |
| `CC_PIPE` | 10 | `\|` |
| `CC_MINUS` | 11 | `-` |
| `CC_LT` / `CC_GT` / `CC_EQ` | 12 / 13 / 14 | `<`、`>`、`=` |
| `CC_BANG` | 15 | `!` |
| `CC_SLASH` | 16 | `/` |
| `CC_LP` / `CC_RP` | 17 / 18 | `(`、`)` |
| `CC_SEMI` | 19 | `;` |
| `CC_PLUS` / `CC_STAR` / `CC_PERCENT` | 20 / 21 / 22 | `+`、`*`、`%` |
| `CC_COMMA` | 23 | `,` |
| `CC_AND` | 24 | `&` |
| `CC_TILDA` | 25 | `~` |
| `CC_DOT` | 26 | `.` |
| `CC_ID` | 27 | 可用于标识符的 Unicode 字符 |
| `CC_ILLEGAL` | 28 | 非法字符 |
| `CC_NUL` | 29 | `0x00` |
| `CC_BOM` | 30 | UTF-8 BOM 首字节 `0xEF` |

同一张表在 EBCDIC 构建下由 `src/tokenize.c` 中另一份 `aiClass[]` 初始化式提供（`SQLITE_EBCDIC` 分支），类别值与 ASCII 版一一对应。

### 标识符字符判定

ASCII 构建下 `IdChar` 宏为：

```c
#define IdChar(C)  ((sqlite3CtypeMap[(unsigned char)C]&0x46)!=0)
```

`sqlite3CtypeMap[]` 定义在 `src/global.c`，其上方注释解释了掩码 `0x46` 的含义：位 `0x40` 表示「非字母数字但可用于 SQLite 标识符」，因此标识符判定掩码是 `0x46`（`0x40` 与字母数字位 `0x02`/`0x04` 的组合）。按注释，标识符由字母数字、`_`、`$` 以及任意非 ASCII 的 UTF 字符构成。

`src/tokenize.c` 里 `IdChar` 宏上方的注释还给出一条兼容性说明：SQL 标准不允许 `$` 出现在标识符中间，但许多 SQL 实现允许，SQLite 为兼容性放行；该特性未写进官方文档，对应 Ticket #1066。

`sqlite3IsIdChar()` 的实现只有一行 `return IdChar(c);`，作用是把这份判定对外暴露。

### `sqlite3GetToken()` 的分支

函数体是一个以 `aiClass[*z]` 为分支条件的大 `switch`，各分支行为如下（括号内为该分支实际写入的 `*tokenType`）：

- **空白（`CC_SPACE`）**：用 `sqlite3Isspace()` 循环跳过所有连续空白，返回长度，记号类型为 `TK_SPACE`。
- **`--` 注释（`CC_MINUS`）**：第二个字符是 `-` 时，循环到换行符或字符串结尾为止，记号类型为 `TK_COMMENT`；第二个字符是 `>` 时返回 `TK_PTR`，长度为 `2 + (z[2]=='>')`，即 `->` 与 `->>`。
- **C 风格注释（`CC_SLASH`）**：`z[1]!='*'` 或 `z[2]==0` 时退化为普通除号 `TK_SLASH`；否则循环找 `*/` 结束串，记号类型为 `TK_COMMENT`。若注释未闭合，循环在字符串结尾停下，此时 `c` 为 0，`if( c ) i++;` 不成立，返回的长度止于结尾。
- **引号字符串（`CC_QUOTE`）**：从 `z[0]` 取定界符向后扫描，遇到与定界符相同的字符时若下一个字符也是定界符则跳过（成对转义），否则结束。结束字符是 `'` 时记号类型为 `TK_STRING`；结束字符非 0（即以 `"` 或 `` ` `` 收尾）时为 `TK_ID`；扫到字符串结尾（`c==0`）则为 `TK_ILLEGAL`。
- **`[...]` 引用标识符（`CC_QUOTE2`）**：找到 `]` 则为 `TK_ID`，否则为 `TK_ILLEGAL`。
- **数字（`CC_DIGIT`）**：先看 `0x`/`0X` 开头且 `z[2]` 是十六进制数字，则走十六进制分支（`SQLITE_OMIT_HEX_INTEGER` 未定义时），否则走十进制分支。两条分支都在遇到 `SQLITE_DIGIT_SEPARATOR` 时把记号类型改写为 `TK_QNUMBER`——该分隔符在 `src/sqliteInt.h` 中定义为 `'_'`。小数点部分与指数部分各自独立处理，命中时把 `TK_INTEGER` 升级为 `TK_FLOAT`；指数的判定条件是 `z[i]` 为 `e` 或 `E`，且其后是数字，或是带 `+`/`-` 的数字。数字扫完后还有一个 `while( IdChar(z[i]) )` 循环：一旦数字后面紧跟标识符字符，就把记号类型改为 `TK_ILLEGAL` 并继续前进。
- **变量（`CC_VARNUM` 与 `CC_DOLLAR`/`CC_VARALPHA`）**：`?` 后面跟数字是编号变量，记号类型为 `TK_VARIABLE`；`$`、`@`、`#`、`:` 开头同样是 `TK_VARIABLE`。在 `SQLITE_OMIT_TCL_VARIABLE` 未定义时，`$`/`@`/`#`/`:` 形式还支持括号参数形式：`c=='(' && n>0` 时扫描到匹配的 `)`，扫不到闭合括号则记号类型改为 `TK_ILLEGAL`；`::` 形式（`c==':' && z[i+1]==':'`）会多前进一个字符。整个变量分支结束时若 `n==0`（一个标识符字符都没吃到），记号类型改为 `TK_ILLEGAL`。
- **关键字（`CC_KYWD0`）**：若 `aiClass[z[1]]>CC_KYWD`（第二个字符不是字母或 `_`），则退化为单字符标识符；否则循环收集所有 `CC_KYWD` 类字符，然后若 `IdChar(z[i])` 成立，说明这个记号混进了关键字不允许的字符，于是退化为标识符；否则调 `keywordCode()` 查关键字哈希表并返回其结果。
- **BLOB 字面量（`CC_X`）**：`x`/`X` 后紧跟 `'` 时进入 BLOB 分支（`SQLITE_OMIT_BLOB_LITERAL` 未定义时），扫十六进制数字后要求以 `'` 收尾且十六进制位数为偶数（`i%2` 为 0），否则记号类型改为 `TK_ILLEGAL` 并扫到下一个 `'`。源码注释写明：若不是 BLOB 字面量则必然是标识符，因为没有任何 SQL 关键字以字母 `x` 开头，所以此处 `deliberate_fall_through` 落到通用标识符分支。
- **运算符**：`=` 后面再跟 `=` 时长度为 2（`TK_EQ`）；`<` 的四种形态分别是 `<=`（`TK_LE`）、`<>`（`TK_NE`）、`<<`（`TK_LSHIFT`）、`<`（`TK_LT`）；`>` 对应 `>=`（`TK_GE`）、`>>`（`TK_RSHIFT`）、`>`（`TK_GT`）；`!` 后面不是 `=` 则为 `TK_ILLEGAL`；`|` 单独出现是 `TK_BITOR`，`||` 是 `TK_CONCAT`；`%` 是 `TK_REM`；`&` 是 `TK_BITAND`；`~` 是 `TK_BITNOT`。
- **BOM（`CC_BOM`）**：`z[1]==0xbb && z[2]==0xbf` 时返回 3，记号类型为 `TK_SPACE`。
- **NUL（`CC_NUL`）**：记号类型为 `TK_ILLEGAL`，返回长度 0。
- **兜底 `default`**：记号类型为 `TK_ILLEGAL`，返回长度 1。

### `sqlite3RunParser()` 的驱动循环

主循环每一轮做这些事：

1. `n = sqlite3GetToken((u8*)zSql, &tokenType)` 取下一个记号。
2. `mxSqlLen -= n`，若 `mxSqlLen<0` 则设 `pParse->rc = SQLITE_TOOBIG` 并 `pParse->nErr++` 后退出循环。`mxSqlLen` 的初值是 `db->aLimit[SQLITE_LIMIT_SQL_LENGTH]`。该限制的编译期硬上限由 `src/sqliteLimit.h` 的 `SQLITE_MAX_SQL_LENGTH` 给出，默认值 `1000000000`；运行时默认值由 `src/main.c` 的 `aHardLimit[]` 数组提供同一常量，应用可用 `sqlite3_limit()` 调整。
3. 记号类型落在「尾部的空白/注释/非法类」区间时（窗口函数未裁剪时是 `tokenType>=TK_WINDOW`，裁剪后是 `tokenType>=TK_SPACE`），做四类处理：
   - **中断检查**：`AtomicLoad(&db->u1.isInterrupted)` 为真时设 `pParse->rc = SQLITE_INTERRUPT` 并退出循环。循环开始前，若 `db->nVdbeActive==0`，会用 `AtomicStore(&db->u1.isInterrupted, 0)` 把中断标志清零。
   - **空白**：`tokenType==TK_SPACE` 时直接 `zSql += n` 跳过。
   - **输入结束**：当 `zSql[0]==0` 时，函数会再喂两次记号——第一次喂 `TK_SEMI`，第二次喂 `0`（结束符）。若上一个记号已经是 `TK_SEMI` 就直接喂 0，若上一个已经是 0 则跳出循环。源码注释写明这是「到达输入末尾时，再调解析器两次，依次是 `TK_SEMI` 和 0」。
   - **窗口函数关键字消歧**：`TK_WINDOW`、`TK_OVER`、`TK_FILTER` 三个记号分别交给 `analyzeWindowKeyword()`、`analyzeOverKeyword()`、`analyzeFilterKeyword()` 重新判定，判定结果可能是 `TK_ID`。
4. **注释丢弃**：记号是 `TK_COMMENT` 且（`db->init.busy` 为真，或 `db->flags & SQLITE_Comments` 非零）时跳过该注释。源码注释写明条件是「正在重解析模式」或「`SQLITE_DBCONFIG_ENABLE_COMMENTS` 已开启（默认开启）」。`SQLITE_Comments` 标志位在 `src/sqliteInt.h` 中定义为 `HI(0x00040)`，并在 `src/main.c` 的 `SQLITE_DBCONFIG_ENABLE_COMMENTS` 配置项映射表中对应。
5. **非法记号报错**：既不是注释也不是 `TK_QNUMBER` 时，构造 `Token x` 并用 `sqlite3ErrorMsg(pParse, "unrecognized token: \"%T\"", &x)` 报错后退出循环。`TK_QNUMBER` 是例外——带下划线分隔的数字被词法器标记出来但并不直接送进语法树，而是留给上层处理。
6. **推进解析器**：把 `zSql` 与长度 `n` 存进 `pParse->sLastToken`，调 `sqlite3Parser(pEngine, tokenType, pParse->sLastToken)`，然后 `lastTokenParsed = tokenType; zSql += n;`。若 `pParse->rc!=SQLITE_OK` 则退出循环。

循环结束后做清理与统计：

- 若定义了 `YYTRACKMAXSTACKDEPTH`，用 `sqlite3ParserStackPeak(pEngine)` 配合 `sqlite3StatusHighwater(SQLITE_STATUS_PARSER_STACK, ...)` 记录解析器栈深度的峰值（进 `sqlite3_mutex_enter(sqlite3MallocMutex())` 保护）。
- 释放解析器：`sqlite3ParserFinalize(pEngine)`（栈上引擎）或 `sqlite3ParserFree(pEngine, sqlite3_free)`（堆分配引擎）。堆分配路径下 `sqlite3ParserAlloc(sqlite3Malloc, pParse)` 返回 0 时会调 `sqlite3OomFault(db)` 并返回 `SQLITE_NOMEM_BKPT`。
- 错误汇总：若 `pParse->zErrMsg` 非空，或 `pParse->rc` 既不是 `SQLITE_OK` 也不是 `SQLITE_DONE`，则在没有消息时用 `sqlite3DbStrDup(db, sqlite3ErrStr(pParse->rc))` 补一条，并（在 `SQLITE_PREPARE_DONT_LOG` 未置位时）调 `sqlite3_log(pParse->rc, "%s in \"%s\"", pParse->zErrMsg, pParse->zTail)` 记日志，`nErr++`。
- 收尾：`pParse->zTail = zSql` 指向未解析的剩余文本；释放 `pParse->apVtabLock`；若 `pParse->pNewTable` 非空且不在特殊解析模式下，调 `sqlite3DeleteTable(db, pParse->pNewTable)`；若 `pParse->pNewTrigger` 非空且不在重命名模式下，调 `sqlite3DeleteTrigger(db, pParse->pNewTrigger)`；释放 `pParse->pVList`；恢复 `db->pParse = pParentParse`。返回值是错误计数 `nErr`。

函数入口还有几个前置动作：取 `mxSqlLen`、在 `db->nVdbeActive==0` 时清中断标志、设 `pParse->rc = SQLITE_OK` 与 `pParse->zTail = zSql`、`db->pParse = pParse`（供嵌套解析使用，退出时恢复为 `pParentParse`），并断言 `pParse->pNewTable==0`、`pParse->pNewTrigger==0`、`pParse->nVar==0`、`pParse->pVList==0`。

### 窗口函数关键字的消歧

文件里有一组三个静态函数，专门处理 `WINDOW`、`OVER`、`FILTER` 三个既可能是关键字、也可能是别名的词。上方注释给出了不能用 Lemon `%fallback` 的理由：

> If a "%fallback ID OVER" directive were added to grammar, then SQLite would always treat "OVER" as an alias, making it impossible to call a window-function without a FILTER clause.

判定规则（来自注释与函数体）：

- `analyzeWindowKeyword()`：`WINDOW` 后的第一个记号必须是标识符（或能回退成标识符的关键字），且再下一个记号必须是 `TK_AS`，才判定为 `TK_WINDOW`，否则返回 `TK_ID`。
- `analyzeOverKeyword()`：前一个记号（`lastToken`）必须是 `TK_RP`，且后一个记号是 `TK_LP` 或标识符，才判定为 `TK_OVER`，否则返回 `TK_ID`。
- `analyzeFilterKeyword()`：前一个记号必须是 `TK_RP`，且后一个记号必须是 `TK_LP`，才判定为 `TK_FILTER`，否则返回 `TK_ID`。

三者共用的 `getToken()` 辅助函数会跳过 `TK_SPACE` 与 `TK_COMMENT`，并把 `TK_ID`、`TK_STRING`、`TK_JOIN_KW`、`TK_WINDOW`、`TK_OVER` 以及经 `sqlite3ParserFallback()` 判定可回退为 `TK_ID` 的记号统一改写为 `TK_ID`。

这三个函数整体位于 `#ifndef SQLITE_OMIT_WINDOWFUNC` 之内。

### 关键字回退机制

`src/parse.y` 中有 `%fallback ID` 指令，注释写明其作用是「让 ABORT、AFTER、ASC 等记号在不能按原义解析时回退为 ID」，并说明这样就不需要再单独写一个 `id` 非终结符。它列出的记号包含 `ABORT ACTION AFTER ANALYZE ASC ATTACH BEFORE BEGIN BY CASCADE CAST COLUMNKW` 等一大串。

`sqlite3ParserFallback()` 在 `src/sqliteInt.h` 中有函数声明，由 Lemon 生成的解析器实现。

### 调用方

`sqlite3RunParser()` 的调用方（全部经 grep 在 `src/` 下确认）：

| 调用点 | 用途 |
|---|---|
| `src/prepare.c` 的 `sqlite3Prepare()` | 预编译 SQL 文本的主路径 |
| `src/prepare.c` 的 `sqlite3Prepare()` | 带 `nBytes` 且末字节非 0 时，先 `sqlite3DbStrNDup` 复制一份再解析，随后把 `sParse.zTail` 偏移回原串 |
| `src/build.c` 的 `sqlite3NestedParse()` | 解析由 `sqlite3VMPrintf` 拼出的嵌套 SQL（函数体默认表达式等） |
| `src/alter.c` 的 `sqlite3RenameTokenRemap()` 一侧的重命名路径 | 解析改名用的 SQL，且该路径显式 `db->flags |= SQLITE_Comments` 后再解析、解析完恢复原标志 |
| `src/vtab.c` 的虚表 `CREATE TABLE` 声明路径 | 解析虚拟表模块给出的建表 SQL，`sParse.eParseMode = PARSE_MODE_DECLARE_VTAB` |

`sqlite3GetToken()` 的其他调用方（除 `src/tokenize.c` 内部）：

| 调用点 | 用途 |
|---|---|
| `src/alter.c` 的 `getConstraintToken()` | 解析外键约束子句 |
| `src/alter.c` 的重命名 token 扫描 | 找出需要替换的记号边界 |
| `src/vtab.c` 的虚表参数解析 | 扫描 `CREATE TABLE` 语句里的列定义参数 |
| `src/vdbetrace.c` | 跟踪模式下重新切分 SQL 文本 |

`sqlite3IsIdChar()` 的外部调用方：

| 调用点 | 用途 |
|---|---|
| `src/alter.c` | 判断被替换记号的首字符是否可直接用作标识符 |
| `src/main.c` | 判断编译选项名是否已完整匹配（要求其后不是标识符字符） |

### 归一化路径（编译期选项开启时）

当定义 `SQLITE_ENABLE_NORMALIZE` 时，`src/tokenize.c` 额外提供 `sqlite3Normalize()`，把一条 SQL 规范化成等价形式，供语句缓存比较。它复用 `sqlite3GetToken()`，但产出规则不同：注释与空白被丢弃；字符串、整数、浮点数、变量、BLOB 一律替换成 `?`；标识符小写化；关键字大写化；`IN (...)` 的右括号处会被回填成 `?,?,?`。`addSpaceSeparator()` 辅助函数用 `sqlite3IsIdChar()` 判断末尾字符是否需要补一个空格。

## 关键取舍

**取舍一：查表而不是分支比较。** `aiClass[]` 查表 + 大 `switch` 的组合，是文件顶部注释明确写出的选择：直接 `switch` 字符会被编译成二分查找，查表更快。代价是必须维护一张 256 项的表，并且 EBCDIC 构建要维护一份等价表。

**取舍二：记号长度与记号类型分离。** `sqlite3GetToken()` 返回长度、类型走输出参数。这样调用方（如 `src/alter.c` 的 `getConstraintToken()`）可以只关心字节偏移而不关心类型，避免为不需要类型的地方付出分支成本。

**取舍三：关键字消歧不走 `%fallback`。** `WINDOW`/`OVER`/`FILTER` 之所以要单独写三个静态函数判定，是因为 Lemon 的 `%fallback` 是无条件的：一旦把 `OVER` 声明为可回退，就永远无法表达「带 FILTER 子句的窗口函数调用」。代价是这三个判定逻辑游离在文法之外，靠 `lastTokenParsed` 这种手工状态传递上下文。

**取舍四：非法记号立即报错而非跳过。** 遇到无法识别的记号时，`sqlite3RunParser()` 直接调 `sqlite3ErrorMsg()` 报 `unrecognized token` 并终止整条语句的解析，而不是跳过该记号继续往下走。这让词法错误在最早的时机暴露。

**取舍五：注释默认被丢弃。** `TK_COMMENT` 在 `db->init.busy`（正在重解析模式）或 `SQLITE_Comments` 置位时被跳过。注意 `SQLITE_Comments` 的默认状态由 `src/main.c` 中 `SQLITE_DBCONFIG_ENABLE_COMMENTS` 的默认配置决定，源码注释把它记为「默认开启」。这一路径只影响注释记号本身，不影响其他记号的切分。

**取舍六：`TK_QNUMBER` 不进语法树。** 数字里出现 `SQLITE_DIGIT_SEPARATOR`（定义为 `'_'`）时，记号类型被改写成 `TK_QNUMBER`，在 `sqlite3RunParser()` 的错误分支里被单独放行而不报错。它是「词法层识别出来但语义层另有处理」的记号。

**取舍七：注释未闭合不报错。** C 风格注释若扫到字符串结尾仍未遇到 `*/`，`sqlite3GetToken()` 仍然返回 `TK_COMMENT`，只是长度止于结尾。因为此时 `c` 为 0，`if( c ) i++;` 不执行。注释块未闭合因此不会触发 `unrecognized token`。

**取舍八：长度上限在词法层强制。** `SQLITE_LIMIT_SQL_LENGTH` 的扣减发生在 `sqlite3RunParser()` 的取记号之后立即检查，扣减的是「已消耗字节数」，因此超限报的是 `SQLITE_TOOBIG` 而不是解析错误。`src/prepare.c` 里另有一道前置检查（`nBytes>mxLen` 时报 `statement too long`），两道关卡用的是同一个 `db->aLimit[SQLITE_LIMIT_SQL_LENGTH]`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/tokenize.c | `sqlite3GetToken` | 扫描器本体：按 `aiClass` 分支切出下一个记号，返回消耗字节数，类型写入输出参数 |
| src/tokenize.c | `sqlite3RunParser` | 驱动循环：取记号、处理空白/注释/中断/长度上限、消歧窗口函数关键字，再调 `sqlite3Parser` |
| src/tokenize.c | `sqlite3IsIdChar` | 把 `IdChar` 宏对外暴露，供 `src/alter.c`、`src/main.c` 与归一化代码使用 |
| src/tokenize.c | `aiClass` | 256 字节字符类别查表，ASCII 与 EBCDIC 各一份 |
| src/tokenize.c | `IdChar` | ASCII 下的标识符字符判定宏，掩码 `0x46` 取自 `sqlite3CtypeMap` |
| src/tokenize.c | `getToken` | 供消歧函数使用的辅助取记号，跳过空白与注释并把可回退记号归一为 `TK_ID` |
| src/tokenize.c | `analyzeWindowKeyword` | 判定 `WINDOW` 是关键字还是别名 |
| src/tokenize.c | `analyzeOverKeyword` | 判定 `OVER` 是关键字还是别名 |
| src/tokenize.c | `analyzeFilterKeyword` | 判定 `FILTER` 是关键字还是别名 |
| src/tokenize.c | `sqlite3Normalize` | `SQLITE_ENABLE_NORMALIZE` 下把 SQL 规范化，用于语句缓存比较 |
| src/tokenize.c | `addSpaceSeparator` | 归一化时按 `sqlite3IsIdChar` 决定是否补空格 |
| src/global.c | `sqlite3CtypeMap` | 256 字节字符属性表，`IdChar` 掩码 `0x46` 的来源 |
| src/sqliteInt.h | `SQLITE_Comments` | 注释开关标志位，值为 `HI(0x00040)` |
| src/sqliteInt.h | `SQLITE_DIGIT_SEPARATOR` | 数字内分隔符，值为 `'_'` |
| src/sqliteInt.h | `sqlite3ParserFallback` | Lemon 生成的回退判定函数声明 |
| src/sqliteLimit.h | `SQLITE_MAX_SQL_LENGTH` | SQL 长度编译期硬上限，默认 `1000000000` |
| src/main.c | `aHardLimit` | 各 `SQLITE_LIMIT_*` 的硬上限数组，含 `SQLITE_MAX_SQL_LENGTH` |
| src/main.c | `SQLITE_DBCONFIG_ENABLE_COMMENTS` | 与 `SQLITE_Comments` 标志位的配置项映射 |
| src/prepare.c | `sqlite3Prepare` | 预编译主路径，调用 `sqlite3RunParser` 并回填 `zTail` 偏移 |
| src/build.c | `sqlite3NestedParse` | 解析拼接生成的嵌套 SQL |
| src/alter.c | `sqlite3RenameTokenRemap` 一侧的重命名路径 | 解析改名 SQL，显式开启 `SQLITE_Comments` |
| src/alter.c | `getConstraintToken` | 用 `sqlite3GetToken` 解析外键约束子句 |
| src/vtab.c | 虚表 `CREATE TABLE` 声明路径 | 解析虚拟表建表 SQL |
| src/vdbetrace.c | 跟踪模式 | 重新切分 SQL 文本以打印记号流 |
| src/parse.y | `%fallback ID` | 让一批关键字在无法按原义解析时回退为标识符 |
| src/parse.y | `%token SPACE COMMENT ILLEGAL` | 保证 `TK_SPACE`、`TK_COMMENT`、`TK_ILLEGAL` 是最后三个记号，解析器依赖此顺序 |
| src/complete.c | `IdChar` | 补全逻辑复用同一套标识符字符判定 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层（`storage/`、`wal/`、`index/`、`txn/`、`catalog/`、`query/`、`spi/`）里没有与 SQLite 词法层对位的模块。主核的 `query/` 层直接面向结构化的 API 调用，不承担 SQL 文本的切分职责。SQLite 的词法层是「文本 SQL → 结构化计划」这条链路上的第一段，也是唯一一个必须逐字节理解输入的地方。

**架构推论：** 嵌入式 SDK（`engine/src/sdk/`）只依赖 SQLite 一个外部库，因此 AI 生成 SQL 的解析路径最终必然经过这里的 `sqlite3GetToken()`。这意味着词法层的错误分类会直接决定 AI 侧看到的错误形态：`TK_ILLEGAL` 会以 `unrecognized token: "..."` 的形式冒泡，而 `TK_QNUMBER`、`TK_VARIABLE`、`TK_BLOB` 这些「词法层特殊标记」则需要在更上层有对应处理。

**架构推论：** 若主核未来要引入 SQL 文本入口，SQLite 在这里给出的可移植经验有三条：其一，字符分类用查表而非分支比较；其二，关键字与标识符的区分必须支持上下文回退，而纯上下文无关的文法无法表达 `OVER` 这类既可作关键字又可作别名的词；其三，词法错误应当在最早时机以明确消息暴露，而不是静默跳过。

**架构推论：** AI 侧若要对 SQL 报错做分类，`unrecognized token` 属于「输入生成问题」，与 `SQLITE_TOOBIG` 属于「资源限制问题」、与 `SQLITE_INTERRUPT` 属于「运行时取消问题」是三类不同来源，`sqlite3RunParser()` 把它们分别用 `sqlite3ErrorMsg()`、`pParse->rc = SQLITE_TOOBIG`、`pParse->rc = SQLITE_INTERRUPT` 三条互不相同的路径设置，正是为了让上层能区分。

## 参考

- [SQLite 架构图（词法分析到语法分析的管线位置）](https://sqlite.org/arch.html)
- [SQLite 官方文档：Parsing Of SQL Statements（`sqlite3_prepare` 与解析流程）](https://sqlite.org/lang_parse.html)
- [SQLite 官方文档：Limitation On The Length Of An SQL Statement（`SQLITE_LIMIT_SQL_LENGTH`）](https://sqlite.org/limits.html#max_sql_length)
- [SQLite 官方文档：Tokenizing（词法记号与关键字回退）](https://sqlite.org/tokenize.html)
- [SQLite 官方文档：Lemon 解析器生成器（`%fallback` 指令）](https://sqlite.org/doc/trunk/doc/lemon.html)
- [sqlite/sqlite 源码：src/tokenize.c](https://github.com/sqlite/sqlite/blob/master/src/tokenize.c)
- [sqlite/sqlite 源码：src/parse.y](https://github.com/sqlite/sqlite/blob/master/src/parse.y)
