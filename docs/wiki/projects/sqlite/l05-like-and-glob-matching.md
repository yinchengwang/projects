---
project: sqlite
feature: like-and-glob-matching
page_type: feature
schema_version: 1
refs:
  - path: src/func.c
    symbols: [sqlite3RegisterLikeFunctions, sqlite3_strlike, sqlite3_strglob, sqlite3IsLikeFunction]
---

# LIKE 与 GLOB 匹配

## 一句话职责

`src/func.c` 用同一套递归匹配引擎 `patternCompare` 支撑 SQL 的 `LIKE` 与 `GLOB` 两个运算符：模式里用到的通配符、大小写折叠规则都收在一个 `compareInfo` 结构里，`LIKE` 与 `GLOB` 各自对应一张常量表；`sqlite3RegisterLikeFunctions` 负责按「是否区分大小写」重新登记 `like` 函数，`sqlite3IsLikeFunction` 则向查询优化器暴露「这个表达式是不是 LIKE 风格函数」的判定能力。

## 机制是什么

**1. 模式描述表 `compareInfo`。** `src/func.c` 里有一个静态结构 `compareInfo`，四个字段依次是 `matchAll`（匹配任意长串的通配符）、`matchOne`（匹配恰好一个字符的通配符）、`matchSet`（字符集起始符，非零表示支持 `[...]` 形式）、`noCase`（是否忽略大小写）。整个机制的核心就是三张常量表：

- `globInfo = { '*', '?', '[', 0 }` —— GLOB：`*` 匹配任意长串、`?` 匹配一个字符、`[` 起始字符集、**不忽略大小写**（`noCase` 为 0）。
- `likeInfoNorm = { '%', '_', 0, 1 }` —— LIKE 默认形态：`%` 匹配任意长串、`_` 匹配一个字符、**不支持字符集**（`matchSet` 为 0）、**忽略大小写**（`noCase` 为 1）。源码注释说明这是 SQL-92 的正确行为：`'a' LIKE 'A'` 应为真。
- `likeInfoAlt = { '%', '_', 0, 0 }` —— LIKE 的区分大小写形态：仅 `noCase` 与 `likeInfoNorm` 不同，其余一致。源码注释指明它在 `SQLITE_CASE_SENSITIVE_LIKE` 编译宏打开时使用。

**2. 匹配结果三态。** `patternCompare` 不返回布尔，而返回三个宏常量：`SQLITE_MATCH`（0，匹配）、`SQLITE_NOMATCH`（1，不匹配）、`SQLITE_NOWILDCARDMATCH`（2，**尽管模式里有 `*` 或 `%`，仍然不匹配**）。第三态是性能优化用的信号：它让调用方能区分「模式里根本没有通配符，纯字面量比较失败」和「通配符存在但整条路径都没命中」，前者可以走更快的短路。

**3. 三个对外入口。** `sqlite3_strlike(zPattern, zStr, esc)` 与 `sqlite3_strglob(zGlobPattern, zString)` 是 C API：两者都是「匹配返回 0，不匹配返回非 0」，与 `strcmp` 语义一致。`sqlite3_strlike` 走 `likeInfoNorm` 并把调用方给的 `esc` 作为 `matchOther` 传入；`sqlite3_strglob` 走 `globInfo` 并把 `'['` 作为 `matchOther` 传入——也就是说 GLOB 的「字符集起始符」在匹配器里复用了 LIKE 的「转义符」槽位。

**4. SQL 层入口 `likeFunc`。** 这是注册到 SQL 层的函数体。`A LIKE B` 在 SQL 层被翻译成 `like(B, A)`，即第一个参数是模式、第二个参数是被匹配串（源码注释明确写出 `A LIKE B` 实现为 `like(B,A)`）。同一个函数体同时服务 `like` 和 `glob`，靠 `sqlite3_user_data(context)` 取出各自绑定的 `compareInfo` 指针来区分。

**5. 优化器契约 `sqlite3IsLikeFunction`。** 它判断一个 `TK_FUNCTION` 表达式是否是 LIKE 风格函数：命中则通过输出参数 `aWc` 输出通配符与转义符，通过 `*pIsNocase` 输出是否忽略大小写，并返回真。

## 怎么实现的

**1. `patternCompare` 的骨架。** 它按 `compareInfo` 解出 `matchOne`、`matchAll`、`noCase` 三个局部量，然后逐字符推进模式串 `zPattern` 与输入串 `zString`。两个字符串都用 `Utf8Read` 读取：非 EBCDIC 平台下 `Utf8Read` 宏展开为 `(A[0]<0x80?*(A++):sqlite3Utf8Read(&A))`，即 ASCII 走单字节快路径，非 ASCII 才调 `sqlite3Utf8Read`（`src/utf.c` 里按 `sqlite3Utf8Trans1[c-0xc0]` 表解码，遇到非法序列返回 `0xFFFD`）。函数末尾返回 `*zString==0 ? SQLITE_MATCH : SQLITE_NOMATCH`——模式用完时输入串也必须刚好用完。

**2. 通配符 `matchAll`（`*` 或 `%`）的处理。** 命中 `c==matchAll` 时，先用一个内层循环跳过模式里连续的 `matchAll`；如果 `matchOne` 非零（即 LIKE 的 `_` 或 GLOB 的 `?`），跳过的每个 `matchOne` 要从输入串里消费掉一个字符，若此时输入串已到头则立刻返回 `SQLITE_NOWILDCARDMATCH`。跳过后若模式串已结束（`c==0`），直接返回 `SQLITE_MATCH`（模式末尾的 `*` 匹配剩余全部）。

若跳过后遇到 `c==matchOther`：对 LIKE（`matchSet==0`）而言这是转义符，读出下一个字符，若模式串已结束则返回 `SQLITE_NOWILDCARDMATCH`；对 GLOB（`matchSet!=0`）而言这是 `[`，源码注释说明此时必须做慢速递归搜索——它用一个 `while( *zString )` 循环，对每个候选起点递归调用 `patternCompare(&zPattern[-1], zString, pInfo, matchOther)`，注意递归入口是 `&zPattern[-1]` 即退回 `[` 之前一个字节，让递归重新走字符集分支；一旦某次递归返回值不是 `SQLITE_NOMATCH` 就直接返回该值，全部失败则返回 `SQLITE_NOWILDCARDMATCH`。

**3. 通配符后的定位优化。** 若跳过后得到的 `c` 是 ASCII 字符（`c<0x80`），走快速路径：构造一个两字节的停止字符集 `zStop`（区分大小写时 `zStop[0]=c`；忽略大小写时 `zStop[0]=sqlite3Toupper(c)`、`zStop[1]=sqlite3Tolower(c)`），然后用 `strcspn((const char*)zString, zStop)` 一次性跳过所有无法匹配的字符。源码注释说明这里额外算了一个 `nx`/`ny` 预检查：`nx` 取模式里通配符之后的第一个字符，若它不是 ASCII 或恰好是某个通配符（`matchAll`/`matchOne`/`matchOther`）则置 0 关闭该检查；忽略大小写时 `nx` 取 `sqlite3Toupper(nx)`、`ny` 取 `sqlite3Tolower(nx)`。命中候选位置后先做一次行内比较 `if( nx && zString[0]!=nx && zString[0]!=ny ) continue;`，通过才递归调用 `patternCompare`。

若 `c` 不是 ASCII（`c>=0x80`），走朴素路径：逐字符读输入串，遇到 `c2==c` 才递归调用 `patternCompare`。整段循环结束后统一返回 `SQLITE_NOWILDCARDMATCH`。

**4. 字符集分支（仅 GLOB）。** `c==matchOther` 且 `matchSet!=0` 时：先从输入串读一个字符 `c`，输入串已结束则返回 `SQLITE_NOMATCH`；再读模式里的第二个字符 `c2`，若为 `'^'` 则置 `invert=1`（取反集）并再读一个字符；若为 `']'` 则做特判——只有输入字符本身就是 `']'` 时才置 `seen=1`，再读下一个模式字符。随后主循环 `while( c2 && c2!=']' )`：遇到 `'-'` 且模式里下一个字符不是 `']'`、不是 0、且 `prior_c>0` 时，按区间判断 `if( c>=prior_c && c<=c2 ) seen = 1;`，否则按单字符相等判断 `if( c==c2 ) seen = 1;`。循环结束时若 `c2==0`（字符集没有闭合的 `]`）或 `(seen ^ invert)==0`，返回 `SQLITE_NOMATCH`，否则 `continue` 继续匹配下一个模式字符。

**5. 普通字符比较与转义。** 若 `c` 既不是 `matchAll` 也不是 `matchOther`，则读输入串的下一个字符 `c2`：`c==c2` 直接继续；若 `noCase` 且 `sqlite3Tolower(c)==sqlite3Tolower(c2)` 且两者都 `<0x80`，也继续（这就是 ASCII 范围内的忽略大小写折叠）；若 `c==matchOne` 且 `zPattern!=zEscaped` 且 `c2!=0`，继续（单字符通配符命中）。否则返回 `SQLITE_NOMATCH`。

转义的处理在 `c==matchOther && pInfo->matchSet==0` 分支：读出转义后的字符 `c`，若模式串已结束返回 `SQLITE_NOMATCH`，否则把 `zEscaped = zPattern`（指向转义字符之后的位置）。后面普通字符比较里的 `zPattern!=zEscaped` 条件正是用来排除「被转义的那个字符本身不该被当作通配符」这一情形。

**6. 大小写折叠的边界。** 折叠只在 ASCII 范围内生效：折叠判定要求 `c<0x80 && c2<0x80`。折叠函数 `sqlite3Toupper`/`sqlite3Tolower` 定义在 `src/sqliteInt.h`：`SQLITE_ASCII` 打开时 `sqlite3Toupper(x)` 是 `(x)&~(sqlite3CtypeMap[(unsigned char)(x)]&0x20)`（即清掉 ASCII 大小写位），`sqlite3Tolower(x)` 是查表 `sqlite3UpperToLower[(unsigned char)(x)]`；`SQLITE_ASCII` 未打开时退化为 C 库的 `toupper`/`tolower`（带 `(unsigned char)` 强转）。源码注释明确说 SQLite 自己的这版「无论 locale 都只对 ASCII 字符生效」。

**7. `likeFunc` 的前置检查与错误分支。** 顺序如下：

- 若定义了 `SQLITE_LIKE_DOESNT_MATCH_BLOBS`，则任一参数是 `SQLITE_BLOB` 时直接 `sqlite3_result_int(context, 0)` 返回。
- 取 `nPat = sqlite3_value_bytes(argv[0])`（`src/vdbeapi.c` 里 `sqlite3_value_bytes` 转发给 `sqlite3ValueBytes(pVal, SQLITE_UTF8)`，返回的是**字节数**不是字符数）。若 `nPat > db->aLimit[SQLITE_LIMIT_LIKE_PATTERN_LENGTH]`，则 `sqlite3_result_error(context, "LIKE or GLOB pattern too complex", -1)` 并返回。这条上限的编译期硬上限是 `SQLITE_MAX_LIKE_PATTERN_LENGTH`（`src/sqliteLimit.h` 里定义为 `50000`），`src/main.c` 的 `aHardLimit` 数组把 `SQLITE_LIMIT_LIKE_PATTERN_LENGTH` 这一项设为该宏，并在 `sqlite3_limit` 里用 `assert( aHardLimit[SQLITE_LIMIT_LIKE_PATTERN_LENGTH]==SQLITE_MAX_LIKE_PATTERN_LENGTH )` 校验二者一致；连接初始化时 `memcpy(db->aLimit, aHardLimit, sizeof(db->aLimit))` 把硬上限复制成连接级默认值。源码注释说明这个限制的用意是「避免 `patternCompare` 的深递归与 N*N 行为」。

- 三参数形式（`argc==3`）时校验转义字符：`zEsc = sqlite3_value_text(argv[2])`，若 `sqlite3Utf8CharLen((char*)zEsc, -1)!=1` 则报错 `"ESCAPE expression must be a single character"`（`sqlite3Utf8CharLen` 在 `src/utf.c` 中逐字符 `SQLITE_SKIP_UTF8` 计数，负长度表示数到第一个 0 字节为止）。通过后 `escape = sqlite3Utf8Read(&zEsc)` 取出转义字符本身；若该字符恰好等于 `pInfo->matchAll` 或 `pInfo->matchOne`，则把 `pInfo` 复制到栈上的 `backupInfo`，再把对应的 `matchAll`/`matchOne` 置 0——即「转义符本身不能同时是通配符」。
- 两参数形式（`argc!=3`）时 `escape = pInfo->matchSet`（对 LIKE 是 0，表示无转义；对 GLOB 是 `'['`）。
- 最后 `zB = sqlite3_value_text(argv[0])`（模式）、`zA = sqlite3_value_text(argv[1])`（被匹配串），两者都非空时才调 `sqlite3_result_int(context, patternCompare(zB, zA, pInfo, escape)==SQLITE_MATCH)`。

**8. `sqlite3_strlike` / `sqlite3_strglob` 的空指针契约。** 两个函数都先判空：`sqlite3_strglob` 中 `zString==0` 时返回 `zGlobPattern!=0`（即模式非空则不匹配），`zGlobPattern==0` 时返回 1（不匹配）；`sqlite3_strlike` 的结构完全对称（`zStr==0` 返回 `zPattern!=0`，`zPattern==0` 返回 1）。

**9. 注册路径 `sqlite3RegisterLikeFunctions`。** 它按 `caseSensitive` 参数二选一：区分大小写时 `pInfo = &likeInfoAlt` 且 `flags = SQLITE_FUNC_LIKE | SQLITE_FUNC_CASE`，否则 `pInfo = &likeInfoNorm` 且 `flags = SQLITE_FUNC_LIKE`。然后对 `nArg` 从 2 到 3 各调一次 `sqlite3CreateFunc(db, "like", nArg, SQLITE_UTF8, pInfo, likeFunc, 0, 0, 0, 0, 0)`，再用 `sqlite3FindFunction(db, "like", nArg, SQLITE_UTF8, 0)` 取回 `FuncDef`（源码注释说明这个调用不会失败，因为 `like` SQL 函数本来就在），然后 `pDef->funcFlags |= flags` 并 `pDef->funcFlags &= ~SQLITE_FUNC_UNSAFE`。

这个函数的唯一调用方是 `src/pragma.c` 里 `PragTyp_CASE_SENSITIVE_LIKE` 分支中的 `sqlite3RegisterLikeFunctions(db, sqlite3GetBoolean(zRight, 0))`，即 `PRAGMA case_sensitive_like` 会重装 LIKE 函数。

**10. 内建注册表里的初始形态。** `src/sqliteInt.h` 定义了 `LIKEFUNC(zName, nArg, arg, flags)` 宏，展开为一条 `FuncDef` 记录：`{nArg, SQLITE_FUNC_BUILTIN|SQLITE_FUNC_CONSTANT|SQLITE_UTF8|flags, (void*)arg, 0, likeFunc, 0, 0, 0, #zName, {0}}`——user-data 位填的就是 `compareInfo` 指针。`src/func.c` 的内建函数表里登记了：`LIKEFUNC(glob, 2, &globInfo, SQLITE_FUNC_LIKE|SQLITE_FUNC_CASE)`；`like` 则按 `SQLITE_CASE_SENSITIVE_LIKE` 宏二选一——打开时登记 `LIKEFUNC(like, 2, &likeInfoAlt, SQLITE_FUNC_LIKE|SQLITE_FUNC_CASE)` 与 `LIKEFUNC(like, 3, &likeInfoAlt, ...)`，关闭时登记 `LIKEFUNC(like, 2, &likeInfoNorm, SQLITE_FUNC_LIKE)` 与 `LIKEFUNC(like, 3, &likeInfoNorm, SQLITE_FUNC_LIKE)`。

注意 GLOB 恒带 `SQLITE_FUNC_CASE`，因此 GLOB 永远区分大小写，且与 `case_sensitive_like` pragma 无关——`sqlite3RegisterLikeFunctions` 只重装 `like`，不重装 `glob`。

**11. `sqlite3IsLikeFunction` 的判定细节。** 它先断言 `pExpr->op==TK_FUNCTION`，若 `!pExpr->x.pList` 直接返回 0；取 `nExpr = pExpr->x.pList->nExpr`，用 `sqlite3FindFunction(db, pExpr->u.zToken, nExpr, SQLITE_UTF8, 0)` 找 `FuncDef`，若 `pDef==0` 或 `(pDef->funcFlags & SQLITE_FUNC_LIKE)==0` 则返回 0。

命中后 `memcpy(aWc, pDef->pUserData, 3)` 把三个通配符字节拷进 `aWc`——源码注释说明这依赖 `compareInfo` 的布局：前三个字段依次是 `matchAll`、`matchOne`、`matchSet`，并用三条 `assert` 固化这一假设（`&((char*)&likeInfoAlt)[0] == (char*)&likeInfoAlt.matchAll` 等）。

`nExpr<3` 时 `aWc[3]=0`（无 ESCAPE 子句）。三参数时取 `pExpr->x.pList->a[2].pExpr`，若它的 `op!=TK_STRING` 直接返回 0（ESCAPE 必须是字符串字面量）；再检查 `zEscape[0]==0 || zEscape[1]!=0` 则返回 0（必须恰好一个字节长），且 `zEscape[0]` 不得等于 `aWc[0]` 或 `aWc[1]`（转义符不能是通配符），否则返回 0。全部通过才 `aWc[3] = zEscape[0]`。

最后 `*pIsNocase = (pDef->funcFlags & SQLITE_FUNC_CASE)==0;` —— 没有 `SQLITE_FUNC_CASE` 标志（即 `likeInfoNorm` 那条）才算忽略大小写。

**12. 优化器的实际调用方。** `src/whereexpr.c` 的 `isLikeOrGlob` 是 LIKE 优化的入口，它调用 `sqlite3IsLikeFunction(db, pExpr, pnoCase, (char*)wc)`，成功后才继续取模式串并计算前缀：统计第一个通配符之前的非通配符字节数 `cnt`，遇到 `0xff`、码点 `>=0xfffd`、`0x7ff`、`0xd7ff` 或数据库编码为 `SQLITE_UTF16LE` 时终止计数（源码注释逐条解释这五个条件的理由）。随后 `src/wherecode.c` 的 `codeCursorHintIsOrFunction` 也调用同一个 `sqlite3IsLikeFunction`，用于在 `OP_CursorHint` 语境下把 LIKE 风格表达式标记为不可下推。

**13. 其它 C API 使用点。** `sqlite3_strglob` 与 `sqlite3_strlike` 在内核内部还被多处调用：`src/analyze.c` 用 `sqlite3_strlike("sqlite\\_%", pTab->zName, '\\')` 判断统计表名、用 `sqlite3_strglob` 匹配 `unordered*` / `sz=[0-9]*` / `noskipscan*` / `costmult=[0-9]*` 等索引参数前缀；`src/json.c` 用 `sqlite3_strglob("*]", &zPath[i])` 判断 JSON 路径片段；`src/os_kv.c` 用 `sqlite3_strglob("*-journal", zName)` 识别日志文件；`src/vdbe.c` 用 `sqlite3_strlike("DELETE%", p->zSql, 0)` 判断语句类型；`src/loadext.c` 把 `sqlite3_strglob` 与 `sqlite3_strlike` 列入可被扩展覆盖的 API 名单。

## 关键取舍

- **源码事实：**`LIKE` 与 `GLOB` 共用同一个 `patternCompare` 函数体，差异全部由 `compareInfo` 常量表承载（`globInfo` / `likeInfoNorm` / `likeInfoAlt`）。**架构推论：**把「通配符集合 + 大小写策略」外置成数据而非代码，可以让两套语义完全不同的匹配规则共享一套递归骨架；代价是 `compareInfo` 的字段顺序变成了隐式 ABI——`sqlite3IsLikeFunction` 里那三条 `assert` 正是在守护这个隐式约定，任何人调整字段顺序都会踩到。
- **源码事实：**`likeFunc` 用 `sqlite3_user_data(context)` 取回 `compareInfo` 指针，同一个函数体服务 `like` 与 `glob`。**架构推论：**把选择逻辑放进函数注册时的 user-data，使 VDBE 侧的调用路径保持单一；代价是无法在调用期切换规则，因此 `PRAGMA case_sensitive_like` 只能通过 `sqlite3RegisterLikeFunctions` 重装函数来生效，而重装过程必须同时覆盖 2 参数与 3 参数两种元数。
- **源码事实：**`patternCompare` 返回三态而非布尔，第三态 `SQLITE_NOWILDCARDMATCH` 专门标记「模式含通配符但仍不匹配」。**架构推论：**区分这两种失败让调用方能跳过无效的继续搜索，把无通配符的纯字面量比较路径彻底短路掉；代价是返回值语义需要靠宏 `SQLITE_MATCH`/`SQLITE_NOMATCH`/`SQLITE_NOWILDCARDMATCH` 三者并列才能读懂，单看 `!=0` 判断会丢掉这层信息。
- **源码事实：**`matchAll` 之后的定位对 ASCII 模式字符走 `strcspn` 快路径并加 `nx`/`ny` 行内预检查，非 ASCII 则退回逐字符扫描。**架构推论：**把快路径限定在 ASCII 是因为 ASCII 的大小写折叠只有两种形态，`strcspn` 能一次跳过多个无法匹配的位置；代价是非 ASCII 通配符后的定位退化为线性扫描，这是 `patternCompare` 最坏情形 O(N²) 的主要来源之一。
- **源码事实：**模式长度上限由 `SQLITE_LIMIT_LIKE_PATTERN_LENGTH` 约束，硬上限 `SQLITE_MAX_LIKE_PATTERN_LENGTH` 为 `50000`，超限报 `"LIKE or GLOB pattern too complex"`。**架构推论：**这是用「拒绝超长模式」代替「实现线性时间匹配算法」的现实选择，把递归深度与 N² 行为的风险转嫁成一条可配置的硬错误；代价是长前缀前缀匹配这类合法用法会被误杀。
- **源码事实：**大小写折叠只在 `c<0x80 && c2<0x80` 时生效。**架构推论：**限定在 ASCII 是为了避免引入完整 Unicode 大小写折叠表（体积与复杂度代价）；代价是 `'É' LIKE 'é'` 这类非 ASCII 的大小写等价不被识别，跨语言数据必须自行归一化。
- **源码事实：**`GLOB` 在内建注册表里恒带 `SQLITE_FUNC_CASE`，而 `sqlite3RegisterLikeFunctions` 只重装 `like`。**架构推论：**大小写敏感性被设计成函数自身的固有属性而非连接级开关，`PRAGMA case_sensitive_like` 只影响 LIKE；代价是用户无法通过 pragma 改变 GLOB 的行为，只能改用 LIKE。
- **源码事实：**`sqlite3IsLikeFunction` 要求 ESCAPE 参数是长度恰好一字节的字符串字面量（`pEscape->op!=TK_STRING` 即返回 0），否则整个表达式不参与 LIKE 优化。**架构推论：**把 ESCAPE 限制为编译期常量，才能让优化器把转义字符写进 `aWc[3]` 并在提取前缀时静态剥离转义；代价是 `ESCAPE` 绑定变量的写法虽然语义正确，却拿不到索引优化。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/func.c` | `patternCompare` | 核心递归匹配器，按 `compareInfo` 处理通配符、字符集、转义与大小写折叠，返回 `SQLITE_MATCH`/`SQLITE_NOMATCH`/`SQLITE_NOWILDCARDMATCH`。 |
| `src/func.c` | `globInfo` | GLOB 的模式描述常量表：`{ '*', '?', '[', 0 }`。 |
| `src/func.c` | `likeInfoNorm` | LIKE 默认模式描述常量表：`{ '%', '_', 0, 1 }`，忽略大小写、不支持字符集。 |
| `src/func.c` | `likeInfoAlt` | LIKE 区分大小写的模式描述常量表：`{ '%', '_', 0, 0 }`。 |
| `src/func.c` | `likeFunc` | `like`/`glob` 的 SQL 层函数体：处理 BLOB 短路、模式长度上限、ESCAPE 校验，最终调 `patternCompare`。 |
| `src/func.c` | `sqlite3_strlike` | C API：按 `likeInfoNorm` 匹配，`esc` 作为转义符传入；空指针按 `strcmp` 语义处理。 |
| `src/func.c` | `sqlite3_strglob` | C API：按 `globInfo` 匹配，`'['` 作为字符集起始符传入。 |
| `src/func.c` | `sqlite3RegisterLikeFunctions` | 按 `caseSensitive` 重装 2 参数与 3 参数的 `like` 函数，设置 `SQLITE_FUNC_LIKE` 与 `SQLITE_FUNC_CASE` 标志并清除 `SQLITE_FUNC_UNSAFE`。 |
| `src/func.c` | `sqlite3IsLikeFunction` | 优化器入口：判定表达式是否 LIKE 风格函数，输出通配符字节、转义字符与是否忽略大小写。 |
| `src/func.c` | `sqlite3RegisterBuiltinFunctions` | 内建函数注册总入口，其函数表中的 `LIKEFUNC(glob, ...)` 与 `LIKEFUNC(like, ...)` 条目携带各自动态的 `compareInfo` 指针。 |
| `src/sqliteInt.h` | `LIKEFUNC` | 定义 `LIKEFUNC(zName, nArg, arg, flags)` 宏，生成以 `likeFunc` 为实现、以 `arg` 为 user-data 的 `FuncDef` 记录。 |
| `src/sqliteInt.h` | `SQLITE_FUNC_LIKE` | `0x0004` 标志位，标记该函数是 LIKE 优化的候选者。 |
| `src/sqliteInt.h` | `SQLITE_FUNC_CASE` | `0x0008` 标志位，标记该 LIKE 型函数区分大小写。 |
| `src/sqliteLimit.h` | `SQLITE_MAX_LIKE_PATTERN_LENGTH` | 模式长度上限的编译期硬上限，定义为 `50000`。 |
| `src/main.c` | `sqlite3_limit` | 调整连接级上限；其中 `assert` 校验 `aHardLimit[SQLITE_LIMIT_LIKE_PATTERN_LENGTH]==SQLITE_MAX_LIKE_PATTERN_LENGTH`。 |
| `src/pragma.c` | `PragTyp_CASE_SENSITIVE_LIKE` | `PRAGMA case_sensitive_like` 分支，调用 `sqlite3RegisterLikeFunctions` 重装 LIKE 函数。 |
| `src/whereexpr.c` | `isLikeOrGlob` | LIKE 优化入口，调用 `sqlite3IsLikeFunction` 后提取模式前缀并剥离转义。 |
| `src/wherecode.c` | `codeCursorHintIsOrFunction` | 在 `OP_CursorHint` 语境下调用 `sqlite3IsLikeFunction`，把 LIKE 风格表达式标记为不可下推。 |
| `src/utf.c` | `sqlite3Utf8Read` | 按 `sqlite3Utf8Trans1` 表解码一个 UTF-8 字符，非法序列返回 `0xFFFD`。 |
| `src/utf.c` | `sqlite3Utf8CharLen` | 统计 UTF-8 字符数，`likeFunc` 用它校验 ESCAPE 必须是单个字符。 |

## 与主核 / AI 的关系

对主核而言，这套机制给出三条可直接迁移的设计：第一是「把语义差异收进常量表、把算法留在单一函数体」的 `compareInfo` 手法——主核的多模态匹配（glob 模式、路径模式、正则片段）同样可以用一张描述表承载各自的通配符集合与折叠策略，取代每个模式一套独立实现。第二是「用长度上限把最坏情形挡在门外」的取舍方式：主核做向量召回的路径前缀匹配、文档路径的 glob 展开时，同样可以用一条可配置的硬上限换取递归深度与 N² 行为的可控。第三是 `sqlite3IsLikeFunction` 这种「匹配器向优化器暴露契约」的接口形态——主核若要把模式匹配下推到存储层或向量化层，就需要一个类似的判定函数把「表达式是否属于可下推的模式匹配」以及「下推需要的通配符与转义参数」一并交出去。

对 AI 侧而言，LIKE/GLOB 的语义边界直接对应模型生成检索条件时的常见歧义：`%` 与 `*`、`_` 与 `?`、`[^...]` 的取反、`ESCAPE` 的转义约定，以及 LIKE 默认忽略大小写而 GLOB 不忽略这一组差异，都是 LLM 拼装检索条件时最容易出错的点。更关键的是 SQLite 把这些差异**显式建模**而非隐式约定：`noCase`、`matchSet` 各自独立成字段，`PRAGMA case_sensitive_like` 提供运行期开关，源码注释还逐条说明 SQL-92 对 LIKE 大小写的要求。AI 应用若要把自然语言意图（「以 abc 开头」「忽略大小写的包含」）翻译成 SQL 条件，这张语义对照表就是校对清单。

## 参考

- [SQLite 官方文档：The LIKE, GLOB, REGEXP, and MATCH Operators](https://www.sqlite.org/lang_expr.html) —— LIKE 与 GLOB 的官方语义、通配符与转义规则。
- [SQLite 官方文档：Built-in Functions](https://www.sqlite.org/lang_builtinfunc.html) —— `like()` 与 `glob()` SQL 函数的参数形式与 `ESCAPE` 子句说明。
- [SQLite 官方文档：PRAGMA case_sensitive_like](https://www.sqlite.org/pragma.html#pragma_case_sensitive_like) —— 连接级 LIKE 大小写敏感性开关的官方说明。
- [SQLite 官方文档：Limits](https://www.sqlite.org/limits.html) —— `SQLITE_MAX_LIKE_PATTERN_LENGTH` 与 `sqlite3_limit()` 的关系。
- [GitHub 镜像源码：sqlite/sqlite 的 src/func.c](https://github.com/sqlite/sqlite/blob/master/src/func.c) —— 本文全部符号、常量与调用顺序的对照来源。
