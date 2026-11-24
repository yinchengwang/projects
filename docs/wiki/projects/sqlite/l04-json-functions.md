---
project: sqlite
feature: json-functions
page_type: feature
schema_version: 1
refs:
  - path: src/json.c
    symbols: [sqlite3RegisterJsonFunctions]
---

# JSON 函数

## 一句话职责

`src/json.c` 用一套内部的二进制解析树表示（JSONB）承载 JSON，把外部的 JSON 文本解析成这棵树、在树上按路径查找与改写、再从树渲染回文本或 BLOB，并在此基础上注册 `json`/`jsonb`/`json_extract`/`json_set`/`json_patch`/`json_group_array` 等一整组 SQL 函数与 `->`/`->>` 两个运算符。

## 机制是什么

整套机制围绕三个不导出的结构体展开，全部定义在 `src/json.c`：

- `JsonParse` 是解析树句柄。字段 `aBlob`/`nBlob` 是 JSONB 字节，`zJson`/`nJson` 是原始文本，`iErr` 记录错误在 `zJson` 中的字节偏移，`iDepth` 是当前嵌套深度，`eEdit` 记录本次要做的编辑类型，`nErr`、`oom`、`hasNonstd` 分别是错误计数、内存耗尽标志与「输入用了非标准扩展」标志。注释在结构体上方明确写出四步生命周期：文本解析成 JSONB → 按路径查找 → 就地修改 `aBlob[]` → 从 `aBlob[]` 重新生成输出文本（`jsonb_*` 变体跳过最后一步）。
- `JsonString` 是输出用的字符串累加器。`zBuf`/`nAlloc`/`nUsed` 描述缓冲区，`bStatic` 标记当前是否还在用栈上的 `zSpace[100]`，`eErr` 是错误标志位（`JSTRING_OOM`、`JSTRING_MALFORMED`、`JSTRING_TOODEEP`、`JSTRING_ERR` 四个值）。
- `JsonCache` 是文本到 JSONB 的映射缓存，挂在 `sqlite3_get_auxdata()` 上，魔数是 `JSON_CACHE_ID`（`-429938`），条目数上限是 `JSON_CACHE_SIZE`（`4`）。

JSONB 的元素类型由一组常量定义：`JSONB_NULL`(0)、`JSONB_TRUE`(1)、`JSONB_FALSE`(2)、`JSONB_INT`(3)、`JSONB_INT5`(4)、`JSONB_FLOAT`(5)、`JSONB_FLOAT5`(6)、`JSONB_TEXT`(7)、`JSONB_TEXTJ`(8)、`JSONB_TEXT5`(9)、`JSONB_TEXTRAW`(10)、`JSONB_ARRAY`(11)、`JSONB_OBJECT`(12)。低 4 位存类型码，高 4 位存长度信息。人类可读的类型名由 `jsonbType[]` 表提供，`json_type()` 直接用它索引。

函数标志位定义在 `src/json.c`：`JSON_JSON`（结果恒为 JSON）、`JSON_SQL`（结果恒为 SQL 值）、`JSON_ABPATH`（允许缩写路径）、`JSON_ISSET`（是 `json_set()` 而非 `json_insert()`）、`JSON_AINS`（是 `json_array_insert()`）、`JSON_BLOB`（用 BLOB 输出格式）。`JSON_INSERT_TYPE(X)` 宏取 `X & 0xC` 的第 2–3 位，用于把 `JSON_ISSET`/`JSON_AINS` 两个组合位翻译成编辑类型。

本模块**不负责**：通用内置函数的注册分派（那是 `src/func.c` 的 `sqlite3RegisterBuiltinFunctions`）、SQL 层字符串函数、JSON 表值函数 `json_each`/`json_tree` 的模块对象注册（后者由同文件的 `sqlite3JsonVtabRegister` 承担，被 `src/build.c` 的 `sqlite3VtabCreateModule` 调用链使用）。

## 怎么实现的

**1. 注册入口与函数表。** `sqlite3RegisterJsonFunctions` 内部有一张静态表 `aJsonFunc`，每项用 `JFUNCTION` 宏（定义在 `src/sqliteInt.h`）展开成 `FuncDef`。该宏的参数是 `(zName, nArg, bUseCache, bWS, bRS, bJsonB, iArg, xFunc)`，展开后固定带上 `SQLITE_FUNC_BUILTIN|SQLITE_DETERMINISTIC|SQLITE_FUNC_CONSTANT|SQLITE_UTF8`，并按 `bUseCache` 追加 `SQLITE_FUNC_RUNONLY`、按 `bRS` 追加 `SQLITE_SUBTYPE`、按 `bWS` 追加 `SQLITE_RESULT_SUBTYPE`；user-data 是 `iArg | (bJsonB * JSON_BLOB)`。注意 `bJsonB` 实际是通过 `JSON_BLOB` 掩码位传给函数体的，不是独立的标志位。整张表最终由 `sqlite3InsertBuiltinFuncs(aJsonFunc, ArraySize(aJsonFunc))` 插入全局函数哈希表。整个函数体被 `#ifndef SQLITE_OMIT_JSON` 包住。

**2. 三条实现路线复用同一批骨架函数。** 标量函数共两类实现：一类是「读入 → 改写 → 输出」，由 `jsonRemoveFunc`、`jsonReplaceFunc`、`jsonSetFunc` 走 `jsonInsertIntoBlob`；另一类是「读入 → 渲染」，由 `jsonExtractFunc`、`jsonTypeFunc`、`jsonArrayLengthFunc`、`jsonQuoteFunc`、`jsonArrayFunc`、`jsonObjectFunc`、`jsonPrettyFunc` 走 `jsonParseFuncArg` 加各自的输出逻辑。`jsonSetFunc` 用 `JSON_INSERT_TYPE(flags)` 算出 `eInsType`，再查两张静态表：`azInsType[]` 给出错误信息里的函数名（`"insert"`、`"set"`、`"array_insert"`），`aEditType[]` 给出对应的编辑码（`JEDIT_INS`、`JEDIT_SET`、`JEDIT_AINS`）。

**3. 统一的参数入口 `jsonParseFuncArg`。** 它按 `flgs` 决定是否查缓存、是否生成可写对象。流程是：参数类型是 `SQLITE_NULL` 时直接返回 `NULL`（不设错误信息，让上层函数返回 SQL NULL）；否则先查 `jsonCacheSearch`；命中缓存且不需要可写时直接复用引用计数加一的缓存对象；命中缓存但需要可写时把 `aBlob` 复制一份到新分配的 `JsonParse`；未命中则走文本路径：取 `sqlite3_value_text` 得到 `zJson`，长度为零视为 malformed，然后调 `jsonConvertTextToBlob` 完成文本到 JSONB 的翻译。翻译失败时，若带了 `JSON_KEEPERROR` 则置 `nErr = 1` 并返回对象，否则释放并返回 `NULL`。翻译成功后，若原文本不是 `sqlite3RCStrUnref` 类的 `RCStr`，则复制一份进 `RCStr` 以便缓存。

**4. BLOB 输入的两条分支。** `jsonArgIsJsonb` 判断 BLOB 是否「看起来像」JSONB：首字节低 4 位必须 `<= JSONB_OBJECT`，`jsonbPayloadSize` 算出的元素长度必须与 BLOB 总长相等；若载荷长度不超过 7 字节则再做一次完整校验 `jsonbValidityCheck`，以免把文本 JSON 的 BLOB 误判。判定失败时 `jsonArgIsJsonb` 返回假，`jsonParseFuncArg` 落到文本路径——源码注释明确说明这是为兼容历史 bug（标签 `tag-20240123-a`）而保留的行为：非 JSONB 的 BLOB 会被转成文本再按 JSON 解析。

**5. 缓存机制。** `jsonCacheSearch` 先按指针相等 `p->a[i]->zJson == zJson` 匹配，再退化为长度相等加 `memcmp` 的内容匹配；命中后把该条目移到数组末尾实现 LRU。缓存条目有四个约束（注释在 `JsonCache` 结构体上方）：`bReadOnly` 必须置位、`aBlob[]` 必须由 `JsonParse` 自己拥有（即 `nBlobAlloc` 非零）、`eEdit` 与 `delta` 必须为零、`zJson` 必须是 `RCStr`。反向路径是 `jsonCacheInsert`。

**6. 路径求值 `jsonLookupStep`。** 它是递归下降的路径匹配器，返回值有五种：`JSON_LOOKUP_ERROR`(0xffffffff)、`JSON_LOOKUP_NOTFOUND`(0xfffffffe)、`JSON_LOOKUP_NOTARRAY`(0xfffffffd)、`JSON_LOOKUP_TOODEEP`(0xfffffffc)、`JSON_LOOKUP_PATHERROR`(0xfffffffb)，`JSON_LOOKUP_ISERROR(x)` 判定 `x >= JSON_LOOKUP_PATHERROR`。匹配对象标签时逐个比较对象的成对元素，找到标签后递归下降，下降前判断 `if( ++pParse->iDepth >= JSON_MAX_DEPTH ) return JSON_LOOKUP_TOODEEP;`。嵌套上限是 `JSON_MAX_DEPTH`：未定义 `SQLITE_JSON_MAX_DEPTH` 时取 `1000`，否则取该宏值；源码注释说明这个上限是为了避免递归下降解析器栈溢出，且 3.42.0 之前该值是 2000。

**7. 深度上限的其它落点。** 文本解析器 `jsonTranslateTextToBlob` 在进入对象与数组时各有一处 `if( ++pParse->iDepth > JSON_MAX_DEPTH )` 判定并记录错误位置；`jsonTranslateBlobToPrettyText` 在缩进时用 `if( pPretty->nIndent >= JSON_MAX_DEPTH )` 判定；`jsonCreateEditSubstructure` 用 `if( pIns->iDepth >= JSON_MAX_DEPTH )` 判定；`jsonMergePatch` 的两处递归点用 `if( iDepth>=JSON_MAX_DEPTH ) return JSON_MERGE_TOODEEP;`。四处判定分别覆盖解析、渲染、创建子结构与 RFC 7396 合并四种路径。

**8. 编辑类型与 `jsonInsertIntoBlob`。** `JsonParse.eEdit` 的五个取值是 `JEDIT_DEL`(1)、`JEDIT_REPL`(2)、`JEDIT_INS`(3)、`JEDIT_SET`(4)、`JEDIT_AINS`(5)。`jsonInsertIntoBlob` 按 `(path, value)` 成对遍历参数：路径为 `SQLITE_NULL` 时跳过；路径首字符不是 `'$'` 走 `jsonInsertIntoBlob_patherror`；值参数由 `jsonFunctionArgToBlob` 转成 JSONB，其中 `SQLITE_BLOB` 且不是合法 JSONB 时报 `"JSON cannot hold BLOB values"`，`SQLITE_TEXT` 且 subtype 是 `JSON_SUBTYPE` 时按 JSON 解析，否则按 `JSONB_TEXTRAW` 当作普通字符串。路径是 `'$'`（根）时，`JEDIT_REPL`/`JEDIT_SET` 直接用 `jsonBlobEdit` 覆盖整棵树，`JEDIT_AINS` 报 `JSON_LOOKUP_NOTARRAY`，`JEDIT_INS` 不做修改。返回码为 `JSON_LOOKUP_NOTFOUND` 时跳过该对继续下一对。

**9. `jsonFunctionArgToBlob` 的类型映射。** `SQLITE_NULL` 与其他未特判类型映射成 `JSONB_NULL`（单字节 `{0x00}`）；`SQLITE_INTEGER` 映射成 `JSONB_INT`；`SQLITE_FLOAT` 在 NaN 时映射成 `JSONB_NULL`，文本以 `'I'` 开头时映射成 `JSONB_FLOAT` 载荷 `"9e999"`，以 `'-'` 加 `'I'` 开头时映射成载荷 `"-9e999"`，否则按原文本存 `JSONB_FLOAT`；`SQLITE_TEXT` 按上文处理。

**10. 值的语义：`json_extract` 与 `->`/`->>`。** `jsonExtractFunc` 的返回形态由 user-data 里的标志位决定：带 `JSON_JSON` 时恒返回 JSON 文本（`->` 运算符在注册表里带此标志），带 `JSON_SQL` 时恒返回 SQL 值（`->>` 带此标志），两者都不带且只有两个参数时，命中的是数组或对象（`(p->aBlob[j]&0x0f) >= JSONB_ARRAY`）就返回 JSON 文本，其他类型返回 SQL 值。参数多于两个时，返回一个 JSON 数组，每个未命中的路径在数组里写成 `null`。`jsonExtractFunc` 支持 `JSON_ABPATH` 缩写路径：整数类型参数被翻译成 `$[N]` 或 `$[#N]`，全字母数字加下划线的字符串翻译成 `$.LABEL`，形如 `[N]` 的直接照搬，其余翻译成 `$."..."`。缩写路径的翻译只在 `JSON_ABPATH` 标志置位时启用，注册表里 `->`/`->>` 都没带这个标志，但函数体注释与 `jsonExtractFunc` 的实现说明该能力是为兼容 PostgreSQL 保留的。

**11. `json_type` 的类型名来源。** `jsonTypeFunc` 取出命中节点的 `p->aBlob[i] & 0x0f`，用 `jsonbType[]` 索引出字符串并以 `SQLITE_STATIC` 返回。因为 `jsonbType[]` 表里 `JSONB_INT` 与 `JSONB_INT5` 都映射为 `"integer"`，`JSONB_FLOAT` 与 `JSONB_FLOAT5` 都映射为 `"real"`，所以对外的类型名只有 8 种。

**12. `json_valid` 的 FLAGS 语义。** `jsonValidFunc` 的第二个参数必须落在 1 到 15 之间，超出报 `"FLAGS parameter to json_valid() must be between 1 and 15"`；随后只取低 4 位 `flags = f & 0x0f`。四个位分别是：`0x01` 严格 RFC-8259 文本、`0x02` 允许 JSON-5 扩展、`0x04` 表面像 JSONB、`0x08` 严格合法 JSONB。参数缺省时 `flags` 初始化为 `1`。BLOB 输入且 `jsonArgIsJsonb` 判定为真时，带 `0x04` 直接返回 1，带 `0x08` 则用 `jsonbValidityCheck` 严格校验后返回结果；两个位都不带则 `res` 保持 0。BLOB 不是 JSONB 时会贯穿到文本分支。文本分支用 `jsonParseFuncArg` 带 `JSON_KEEPERROR|JSON_NOCACHE` 解析，若 `p->hasNonstd` 为零或 `flags` 带 `0x02` 则判定为合法。

**13. `json_error_position`。** `jsonErrorFunc` 对 BLOB 输入先判 `jsonArgIsJsonb`，是则用 `jsonbValidityCheck` 返回首个错误位置；否则把参数当文本（即使是数值型也当文本）用 `jsonConvertTextToBlob` 解析，失败时把 `s.iErr` 这个字节偏移换算成字符偏移——换算方式是遍历字节并统计 `(s.zJson[k] & 0xc0)!=0x80` 的次数（即非 UTF-8 续接字节的个数），最后加一得到 1 基的字符位置。

**14. `json_patch` 的 RFC 7396 实现。** `jsonMergePatch` 就地改写目标 `pTarget`，补丁 `pPatch` 只读。返回值常量是 `JSON_MERGE_OK`(0)、`JSON_MERGE_BADTARGET`(1)、`JSON_MERGE_BADPATCH`(2)、`JSON_MERGE_OOM`(3)、`JSON_MERGE_TOODEEP`(4)。算法本身在函数注释里被改写成带编号的行（01 到 15），与 RFC 原文一一对应。补丁值为 `JSONB_NULL` 时删除目标中的同名键；补丁值为对象时递归合并；目标里找不到同名键且补丁值非 `NULL` 时按 `JSON_MERGE_OOM` 检查后追加。

**15. 输出格式的选择。** `jsonReturnParse` 按 user-data 的 `JSON_BLOB` 位决定返回 `sqlite3_result_blob` 还是文本；返回文本时额外调用 `sqlite3_result_subtype(ctx, JSON_SUBTYPE)`，这个 subtype 值是 `74`（ASCII 码 `J`）。`jsonReturnString` 同样按 `JSON_BLOB` 位分流，错误码按 `eErr` 分派：`JSTRING_OOM` 报 OOM，`JSTRING_TOODEEP` 保留此前已设的错误，`JSTRING_MALFORMED` 报 `"malformed JSON"`。`jsonReturnFromBlob` 把节点转成 SQL 值：`JSONB_NULL` 转 NULL，`JSONB_TRUE`/`JSONB_FALSE` 转 1/0，整数走 `jsonConvertInteger`，超出 64 位的十进制整数按 `CAST(x AS REAL)` 的方式折成浮点（先 `sqlite3DbStrNDup` 再 `sqlite3AtoF`），浮点用 `sqlite3AtoF` 转换并要求返回值大于 0。

**16. `json_pretty` 的缩进默认值。** `jsonPrettyFunc` 只给一个参数时把缩进设为 `x.zIndent = "    "`、`x.szIndent = 4`；给了第二个参数则用它的实际长度覆盖。源码注释说明这个四空格默认值与 PostgreSQL 一致。

**17. 聚合函数。** `json_group_array`/`jsonb_group_array`/`json_group_object`/`jsonb_group_object` 四个通过 `WAGGREGATE` 宏注册，`jsonb_` 变体在注册时把 `JSON_BLOB` 掩码放进 user-data 位。`jsonArrayStep` 在聚合上下文里逐个追加元素值，追加前先补 `'['` 或 `','`；`jsonArrayCompute` 收尾时补 `']'` 再用 `jsonStringTrimOneChar` 去掉。聚合上下文为空时，`jsonb_` 变体返回单字节 `{0x0b}`（即 `JSONB_ARRAY` 类型码），文本变体返回 `"[]"`。

**18. `json_object` 的参数检查。** `jsonObjectFunc` 在参数个数为奇数时报 `"json_object() requires an even number of arguments"`；标签参数类型不是 `SQLITE_TEXT` 时报 `"json_object() labels must be TEXT"`。这两个检查都是直接调 `sqlite3_result_error` 并返回。

**19. 参数个数错误的统一提示。** `jsonWrongNumArgs` 生成的错误信息格式是 `"json_%s() needs an odd number of arguments"`，调用方有 `jsonReplaceFunc`（传 `"replace"`）与 `jsonSetFunc`（传 `azInsType[eInsType]`）。

**20. 路径错误的统一提示。** `jsonBadPathError` 按返回码选择消息：`JSON_LOOKUP_NOTARRAY` 给 `"not an array element: %Q"`，`JSON_LOOKUP_ERROR` 给 `"malformed JSON"`，`JSON_LOOKUP_TOODEEP` 给 `"JSON path too deep"`，其余给 `"bad JSON path: %Q"`。它有两种用法：`ctx` 为 `NULL` 时只返回消息字符串（供上层自行使用），`ctx` 非 `NULL` 时用 `sqlite3_result_error` 压入上下文。

## 关键取舍

- **源码事实：**`JsonParse` 同时保留原始文本 `zJson` 与二进制树 `aBlob`，输出时优先从 `aBlob` 重新渲染。**架构推论：**保留原文是为了 `json_error_position` 能报出输入里的字符位置（JSONB 里没有原始位置信息），代价是每个解析对象要同时持有两份数据，内存占用翻倍。
- **源码事实：**JSONB 用「首字节高 4 位 + 可变长长度头」编码长度，注释明确说明长度不要求最小形式，允许预留空间后回填。**架构推论：**这种非最小编码让「先生成后知道长度」的路径不必二次搬迁缓冲区；代价是同一个 JSONB 可能有多种字节表示，官方文档因此要求应用把 JSONB 当不透明 BLOB 处理。
- **源码事实：**文本 JSON 一律先转成 JSONB 再处理，`jsonb_` 变体则跳过这步。**架构推论：**统一到一种内部表示让查找、编辑、合并这些复杂逻辑只写一遍；代价是 `json_` 变体每次调用都要付一次解析成本，官方文档也据此建议串联时优先用 `jsonb_` 变体。
- **源码事实：**`jsonArgIsJsonb` 对载荷不超过 7 字节的 BLOB 做完整校验，超过 7 字节只查外层长度。**架构推论：**小 BLOB 才可能是「被误转成 BLOB 的文本 JSON」，大 BLOB 不可能是，因此只对小者付出完整校验成本；代价是 `json_valid(X, 0x04)` 的「表面像 JSONB」判定对大 BLOB 是宽松的，只有 `0x08` 位才做严格校验。
- **源码事实：**非 JSONB 的 BLOB 输入会退化成按文本解析，源码注释把它称为「long-standing bug」但明确表示继续支持（标签 `tag-20240123-a`）。**架构推论：**保留这个行为是为了不破坏依赖 `readfile()` 读 JSON 的既有应用；代价是「BLOB 输入必然报错」这条文档承诺不再成立，输入类型到输出类型的判定变复杂。
- **源码事实：**嵌套深度上限 `JSON_MAX_DEPTH` 默认 1000，可用 `SQLITE_JSON_MAX_DEPTH` 覆盖，注释说明 3.42.0 之前是 2000。**架构推论：**递归下降解析器的栈深度必须与 JSON 深度解耦，否则恶意输入能撑爆 C 栈；代价是深度受限这件事没有在解析错误信息里显式说明，只表现为通用的 malformed JSON。
- **源码事实：**`json_valid` 的 FLAGS 默认值是 `1`，且 `1` 表示「严格 RFC-8259 文本」，因此 `json_valid('{x:35}')` 返回 0 而 `json_valid('{x:35}',6)` 返回 1。**架构推论：**默认值取最严的一档是为了与 2015 年只有单一参数的旧实现保持行为一致；代价是「我只想知道这能不能被 JSON 函数接受」必须显式写 `6`。
- **源码事实：**`json_set`/`json_insert`/`json_replace` 三个函数共用 `jsonInsertIntoBlob`，差别只在 `aEditType[]` 表里的一格；`json_array_insert` 复用同一个入口但带 `JSON_AINS` 标志。**架构推论：**用一张编辑码表替代四个独立实现，把「找到路径 → 在原地改字节 → 重新渲染」这条链路的复杂度收敛到一处；代价是四个函数在参数个数错误时的提示文字要靠 `azInsType[]` 表反查才能对上。
- **源码事实：**`json_extract` 单参数与多参数的返回形态不同，多参数时未命中的路径写成 `null` 而单参数时返回 SQL NULL。**架构推论：**单参数走 SQL NULL 便于 `WHERE` 判断，多参数走 JSON `null` 保持数组结构完整；代价是同一批参数在两种调用形式下「未命中」的语义不同，需要使用者记住。
- **源码事实：**`JSON_BLOB` 位既出现在 `JFUNCTION` 宏的 `bJsonB` 形参里，也出现在 `JSON_INSERT_TYPE` 掩码之外的位置——它是通过 `SQLITE_INT_TO_PTR(iArg|((bJsonB)*JSON_BLOB))` 混进 user-data 的。**架构推论：**用一个整数同时承载「编辑类型」与「输出格式」两类互不冲突的标志，让 `FuncDef` 不必为 JSON 单独开一个结构；代价是同一个位在不同函数里含义不同（编辑函数里是输出格式，提取函数里是格式控制），读代码时必须逐函数确认。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/json.c` | `sqlite3RegisterJsonFunctions` | 本文件唯一导出符号，登记全部 JSON 标量函数、运算符与四个聚合函数。 |
| `src/json.c` | `sqlite3JsonVtabRegister` | 注册 `json_each`/`json_tree`/`jsonb_each`/`jsonb_tree` 四个表值函数的虚表模块。 |
| `src/sqliteInt.h` | `JFUNCTION` | 展开成 `FuncDef` 的宏，固定带确定性/常量/UTF8 标志，按形参追加 `SQLITE_FUNC_RUNONLY`、`SQLITE_SUBTYPE`、`SQLITE_RESULT_SUBTYPE`，并把 `JSON_BLOB` 混入 user-data。 |
| `src/json.c` | `JsonParse` | 解析树句柄结构：JSONB 字节、原文、错误位置、深度、编辑类型、缓存与错误标志。 |
| `src/json.c` | `JsonString` | 输出字符串累加器：缓冲区三件套、静态栈缓冲 `zSpace[100]`、错误标志 `eErr`。 |
| `src/json.c` | `JsonCache` | 文本到 JSONB 的映射缓存，条目数上限 `JSON_CACHE_SIZE` 为 4。 |
| `src/json.c` | `jsonbType[]` | JSONB 类型码到人类可读类型名的映射表，`json_type()` 直接索引它。 |
| `src/json.c` | `jsonParseFuncArg` | 统一参数入口：查缓存、判 BLOB、文本转 JSONB、返回可写或只读解析对象。 |
| `src/json.c` | `jsonArgIsJsonb` | 判断 BLOB 是否是 JSONB，小载荷做完整校验，大载荷只查外层长度。 |
| `src/json.c` | `jsonCacheSearch` | 缓存查找：先指针后内容匹配，命中后移至数组末尾实现 LRU。 |
| `src/json.c` | `jsonCacheInsert` | 缓存写入，供 `jsonReturnString` 把渲染结果回填为可复用条目。 |
| `src/json.c` | `jsonTranslateTextToBlob` | 递归下降的 JSON 文本到 JSONB 翻译器，含 JSON-5 扩展识别与深度检查。 |
| `src/json.c` | `jsonConvertTextToBlob` | `jsonTranslateTextToBlob` 的外层包装，负责处理返回值与错误标志。 |
| `src/json.c` | `jsonbValidityCheck` | JSONB 严格合法性校验，返回首个错误位置。 |
| `src/json.c` | `jsonLookupStep` | 路径求值与就地编辑的核心递归函数，返回 `JSON_LOOKUP_*` 系列状态码。 |
| `src/json.c` | `jsonCreateEditSubstructure` | 为不存在的路径创建中间节点，供 `json_set`/`json_insert` 使用。 |
| `src/json.c` | `jsonBlobEdit` | 在 JSONB 字节序列中执行删除/替换/插入并维护长度字段。 |
| `src/json.c` | `jsonInsertIntoBlob` | `json_insert`/`json_replace`/`json_set`/`json_array_insert` 共用的成对参数编辑主流程。 |
| `src/json.c` | `jsonFunctionArgToBlob` | 把 SQL 值（NULL/INTEGER/FLOAT/TEXT/BLOB）映射成 JSONB 节点。 |
| `src/json.c` | `jsonExtractFunc` | `json_extract`/`jsonb_extract`/`->`/`->>` 的共同实现，按标志位决定返回 JSON 还是 SQL 值。 |
| `src/json.c` | `jsonSetFunc` | `json_set`/`json_insert`/`json_array_insert` 的入口，用 `JSON_INSERT_TYPE` 与两张静态表选编辑码。 |
| `src/json.c` | `jsonReplaceFunc` | `json_replace` 的入口，参数个数为偶数时调 `jsonWrongNumArgs`。 |
| `src/json.c` | `jsonRemoveFunc` | `json_remove` 的实现，路径不存在时跳过，`'$'` 根路径返回 NULL。 |
| `src/json.c` | `jsonMergePatch` | RFC 7396 MergePatch 的就地实现，深度超限返回 `JSON_MERGE_TOODEEP`。 |
| `src/json.c` | `jsonPatchFunc` | `json_patch`/`jsonb_patch` 的入口。 |
| `src/json.c` | `jsonTypeFunc` | `json_type` 的实现，用类型码索引 `jsonbType[]`。 |
| `src/json.c` | `jsonValidFunc` | `json_valid` 的实现，FLAGS 取 1 到 15，只用低 4 位。 |
| `src/json.c` | `jsonErrorFunc` | `json_error_position` 的实现，文本输入时把字节偏移换算成字符偏移。 |
| `src/json.c` | `jsonArrayLengthFunc` | `json_array_length` 的实现，非数组返回 0。 |
| `src/json.c` | `jsonArrayFunc` | `json_array` 的实现，逐参数追加后返回 JSON 数组。 |
| `src/json.c` | `jsonObjectFunc` | `json_object` 的实现，检查参数个数为偶数且标签必须是 TEXT。 |
| `src/json.c` | `jsonQuoteFunc` | `json_quote` 的实现，把 SQL 值转成 JSON 表示。 |
| `src/json.c` | `jsonPrettyFunc` | `json_pretty` 的实现，缩进缺省为四个空格。 |
| `src/json.c` | `jsonParseFunc` | 仅在 `SQLITE_DEBUG` 下编译，输出 JSONB 内部结构的可读转储。 |
| `src/json.c` | `jsonReturnParse` | 解析树的统一输出：按 `JSON_BLOB` 位返回 BLOB 或文本，并置 `JSON_SUBTYPE` 标记。 |
| `src/json.c` | `jsonReturnFromBlob` | 把 JSONB 节点转成 SQL 值（NULL、整数、浮点、文本）。 |
| `src/json.c` | `jsonReturnString` | 字符串累加器的统一输出，按 `eErr` 分派 OOM/过深/畸形三类错误。 |
| `src/json.c` | `jsonArrayStep` | `json_group_array` 的聚合步进函数，逐个追加元素值。 |
| `src/json.c` | `jsonArrayCompute` | `json_group_array` 的收尾计算，空聚合时返回 `"[]"` 或单字节 JSONB 数组。 |
| `src/json.c` | `jsonObjectStep` | `json_group_object` 的聚合步进函数，成对收集标签与值。 |
| `src/json.c` | `jsonObjectCompute` | `json_group_object` 的收尾计算。 |
| `src/json.c` | `jsonGroupInverse` | 窗口函数逆聚合支持：从聚合结果里删掉首个元素。 |
| `src/json.c` | `jsonBadPathError` | 路径错误的统一消息生成，按状态码选「not an array element」「malformed JSON」「JSON path too deep」「bad JSON path」。 |
| `src/json.c` | `jsonWrongNumArgs` | 参数个数错误的统一消息生成，格式为 `json_%s() needs an odd number of arguments`。 |
| `src/json.c` | `jsonEachModule` | `json_each`/`json_tree` 四个表值函数共用的虚表模块定义。 |
| `src/func.c` | `sqlite3RegisterBuiltinFunctions` | 调用 `sqlite3RegisterJsonFunctions()` 的实际调用方。 |

## 与主核 / AI 的关系

对主核而言，这套实现有四处形态可借鉴。一是「一种内部表示 + 多个输出格式」的分层：主核的多模态数据若要同时支持文本与二进制两种存储形态，`JsonParse` 这种「内部一套树、出口按标志位分流」的结构可直接照搬。二是 `JSON_SUBTYPE` 这种把「这个文本其实是 JSON」记在值自身的做法——主核若要避免「值参数到底是字符串还是 JSON」的歧义，可以沿用 subtype 标记的思路，而不是靠函数名约定。三是 `JSON_MAX_DEPTH` 这个显式的、有编译期开关的深度上限，以及它在解析、渲染、建结构、合并四条路径上分别设防的做法，主核做递归式结构遍历时同样需要。四是 `json_valid` 的 FLAGS 位掩码设计：把「宽松检查」与「严格检查」做成可选位而不是二选一函数，让调用方按性能需求取舍。

对 AI 侧而言，JSON 函数是模型输出与结构化存储之间的翻译层。模型侧的 JSON 常见两类脏数据——尾随逗号、单引号、无引号键——而 `jsonTranslateTextToBlob` 恰好接受这些 JSON-5 扩展并在输出时归一成严格 RFC-8259 文本，这个「宽松读入 + 严格写出」的组合正是模型结果落库前需要的语义。另一处是 `json_type` 与 `json_valid` 的分离：前者回答「这个值是什么形状」，后者回答「这个值算不算合法」，两者对应模型流水线里的两道不同关卡——先判合法性，再按类型分派到不同解析器。

## 参考

- [SQLite 官方文档：JSON Functions And Operators](https://sqlite.org/json1.html) —— 列出全部 JSON 函数、运算符、四个表值函数与四个聚合函数的官方语义，并说明 JSONB 格式、JSON5 扩展、路径语法与深度限制。
- [SQLite 官方文档：JSONB 的二进制格式说明](https://sqlite.org/jsonb.html) —— JSONB 元素类型码、头部长度编码与合法 BLOB 判定规则的官方描述。
- [GitHub 镜像源码：sqlite/sqlite 的 src/json.c](https://github.com/sqlite/sqlite/blob/master/src/json.c) —— 本文全部符号、常量、调用顺序与错误分支的对照来源。
