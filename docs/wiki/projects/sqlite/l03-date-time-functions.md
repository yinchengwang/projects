---
project: sqlite
feature: date-time-functions
page_type: feature
schema_version: 1
refs:
  - path: src/date.c
    symbols: [sqlite3RegisterDateTimeFunctions]
---

# 日期时间函数

## 一句话职责

`src/date.c` 用一个内部结构 `DateTime` 把「文本时间戳 / 儒略日数 / Unix 时间戳」三种外部表示统一换算成同一个内部刻度（儒略日 × 86400000 毫秒），再据此注册 `date`、`time`、`datetime`、`julianday`、`unixepoch`、`strftime`、`timediff` 与 `current_*` 这批 SQL 函数。

## 机制是什么

整套机制围绕一个不导出的结构体 `DateTime` 展开，定义在 `src/date.c`：

- `iJD` 是核心字段，语义是「儒略日数 × 86400000」，即以毫秒为单位的内部刻度；其余 `Y/M/D/h/m/s/tz` 都是它的派生视图。
- 三个 `valid*` 标志（`validJD`、`validYMD`、`validHMS`）与 `rawS`、`isError`、`useSubsec`、`isUtc`、`isLocal` 一起构成一份「哪些字段当前可信」的账本。任何时刻至多有一类字段被显式填充，其余靠 `computeJD` / `computeYMD` / `computeHMS` 惰性推导。

外部输入经 `parseDateOrTime` 分派到三条互斥的解析路径：先试 `parseYyyyMmDd`（`YYYY-MM-DD [HH:MM[:SS[.FFF]]]` 及其时区后缀），再试 `parseHhMmSs`（纯 `HH:MM[:SS[.FFF]]`），再依次尝试关键字 `now`、纯数字（`setRawDateNumber`）、关键字 `subsec`/`subsecond`。修饰符（`parseModifier`）在解析后逐个叠加。

`strftime` 的格式串不在本文件内定义，而是由 `strftimeFunc` 内的 `switch` 逐字符匹配 `%` 后的转换符实现；输出走 `sqlite3_str` 动态字符串累加器（`sqlite3_str_new` / `sqlite3_str_appendf` / `sqlite3_result_str`）。

本模块**不负责**：通用内置函数的注册分派（那是 `src/func.c` 的 `sqlite3RegisterBuiltinFunctions`）、用户自定义函数、随机数。注册入口只有 `sqlite3RegisterDateTimeFunctions`，它被 `src/func.c` 的 `sqlite3RegisterBuiltinFunctions` 在注册 `aBuiltinFunc` 之前调用。

## 怎么实现的

**1. 统一的内部刻度与两条换算公式。** `computeJD` 用 Meeus 公式把年月日换算成 `iJD`：先对 `M<=2` 的月份做 `Y--; M+=12` 的调整，再按 `A=(Y+4800)/100`、`B=38-A+A/4`、`X1=36525*(Y+4716)/100`、`X2=306001*(M+1)/10000` 拼出 `(X1+X2+D+B-1524.5)*86400000`。反向的 `computeYMD` 用另一套中间量（`alpha`、`A`、`B`、`C`、`D`、`E`、`X1`）把 `iJD` 拆回年月日。两者都以 `+43200000`（半天）做舍入偏移，把儒略日的正午锚点对齐到整数日。

**2. 有界性由三处硬检查保证。** `computeJD` 在 `Y<-4713 || Y>9999 || p->rawS` 时调用 `datetimeError` 置错误位；`validJulianDay` 用 `INT_464269060799999`（该宏写成 `(((i64)0x1a640)<<32)|0x1072fdff`，注释说明是为了绕开老编译器处理大整数字面量的问题）作为 `iJD` 上界；`setRawDateNumber` 只在 `r>=0.0 && r<5373484.5` 时把裸数字当作儒略日安装，超出即退化为 `rawS`（Unix 时间戳候选）。最终 `isDate` 统一复查 `p->isError || !validJulianDay(p->iJD)`。

**3. 数字输入的双重身份。** `setRawDateNumber` 把数值同时塞进 `p->s` 并置 `rawS=1`；随后 `autoAdjustDate`（由修饰符 `auto` 触发）在 `p->s` 落在 `-21086676*(i64)10000`（对应 `-4713-11-24 12:00:00`）到 `25340230*(i64)10000)+799`（对应 `9999-12-31 23:59:59`）之间时，把它按 Unix 时间戳换算成 `iJD`；超出这个范围则 `rawS` 被清零、值原样保留为 `p->s`。修饰符 `unixepoch` 走另一条强制路径：`r = p->s*1000.0 + 210866760000000.0`，并在 `r>=0.0 && r<464269060800000.0` 时安装为 `iJD`。修饰符 `julianday` 则相反，只接受已经是 `validJD && rawS` 的值。

**4. 修饰符分派。** `parseModifier` 以 `sqlite3UpperToLower[(u8)z[0]]` 做一级 switch，再在每个分支内用 `sqlite3_stricmp` 精确匹配完整关键字（`auto`、`ceiling`、`floor`、`julianday`、`localtime`、`unixepoch`、`utc`、`subsec`/`subsecond`），或用 `sqlite3_strnicmp` 匹配前缀族（`"end of "` / `"start of "` / `"weekday "`）。数量型修饰符 `NNN days` 之类由 `aXformType` 表驱动：表中给出单位名长度、名字、量程上限 `rLimit` 与换算系数 `rXform`，例如 `day` 项是 `{3, "day", 5373485.0, 86400.0}`；判定条件是单位名长度与名字匹配且 `r>-rLimit && r<rLimit`，`month` 与 `year` 两项另有专门处理（先落到年月日再由 `computeFloor` 处理日溢出）。`weekday N` 要求 `r>=-6.0 && r<=6.0` 且 `sqlite3RealSameAsInt` 成立，日期推进量按 `Z=((p->iJD+129600000)/86400000)%7` 与目标星期比较后以 `(n-Z)*86400000` 毫秒调整，带负号的写法额外再减 `7*86400000` 毫秒。

**5. 时区与本地时间。** `parseTimezone` 接受 `(+/-)HH:MM` 与 `Z`/`z` 两种后缀，把偏移分钟写入 `p->tz`；偏移为 0 时同样标记 `isUtc=1`、`isLocal=0`。`computeJD` 在 `p->tz` 非零时把毫秒数减去 `p->tz*60000`，随即作废 `validYMD`/`validHMS` 并清空 `tz`。`localtime` 修饰符调用 `toLocaltime`，后者把 `iJD` 换算成 `time_t`（减去 `21086676*(i64)10000` 的纪元偏移），当日期落在 `[2108667600*(i64)100000, 2130141456*(i64)100000]` 之外时，先把年份映射到等价的 1970–2037 年区间计算再映射回来（源码注释给出证据号 R-55269-29598），最终由 `osLocaltime` 调用 `localtime_r`/`localtime_s`。`utc` 修饰符反向迭代求解，用最多 `cnt++<3` 次迭代收敛本地时间与 UTC 的偏移量。

**6. 输出格式化。** `dateFunc`、`timeFunc`、`datetimeFunc` 都用定长栈缓冲 `zBuf`（分别为 `char zBuf[16]`、`char zBuf[16]`、`char zBuf[32]`）逐位填 `'0' + (x/10)%10` 形式的数字；`useSubsec` 为真时 `datetimeFunc` 输出到下标 24（形如 `YYYY-MM-DD HH:MM:SS.mmm`），为假时输出到下标 20（`YYYY-MM-DD HH:MM:SS`）。年份为负时前置 `'-'` 并输出 11 字节，否则跳过第 0 字节输出 10 字节。`unixepochFunc` 在 `useSubsec` 时输出 `(x.iJD - 21086676*(i64)10000000)/1000.0` 的浮点秒，否则输出整数 `x.iJD/1000 - 21086676*(i64)10000`。`juliandayFunc` 输出 `x.iJD/86400000.0`。

**7. `strftime` 的转换符集合。** `strftimeFunc` 的 `switch` 覆盖 `%d %e %f %F %G %g %H %k %I %j %J %l %m %M %p %P %R %s %S %T %u %w %U %V %W %Y %%`。其中 `%G`/`%g`/`%V` 三者都先把日期挪到「同一周的星期四」（偏移量 `(3 - daysAfterMonday(&x))*86400000` 毫秒）再重算年月日；`%u`/`%w` 共用 `daysAfterSunday` 的结果，仅当 `%u` 遇到 `'0'` 时改成 `'7'`；`%U` 用 `daysAfterJan01` 与 `daysAfterSunday` 之差，`%W` 改用 `daysAfterMonday`。遇到未知转换符时 `sqlite3_str_free(pRes)` 后直接返回，整个函数不产出结果。

**8. `timediff` 的按月/年分解。** `timediffFunc` 声明为固定两个参数（注册表里 `PURE_DATE(timediff, 2, 0, 0, timediffFunc)`）。它先按年、再按月对齐两个 `DateTime`，用 `while` 循环逐月推进/回退直到 `d1.iJD` 与 `d2.iJD` 的先后关系翻转，最后把差值加上 `(u64)1486995408 * (u64)100000` 这个纪元对齐偏移，再用 `sqlite3_str_appendf` 格式化成 `±YYYY-MM-DD HH:MM:SS.SSS`。源码注释给出的不变式是 `datetime(A) == (datetime(B, timediff(A,B))`。

**9. 注册与确定性标记。** `sqlite3RegisterDateTimeFunctions` 维护一张静态 `aDateTimeFuncs` 表，用 `PURE_DATE` 宏登记带日期语义的函数（该宏展开后带 `SQLITE_FUNC_SLOCHNG` 与 `SQLITE_FUNC_CONSTANT`，且 user-data 指向 `&sqlite3Config`），用 `DFUNCTION` 宏登记 `current_time`/`current_timestamp`/`current_date`（只带 `SQLITE_FUNC_SLOCHNG`），最后调用 `sqlite3InsertBuiltinFuncs` 插入全局函数哈希表。`SQLITE_OMIT_DATETIME_FUNCS` 打开时走另一分支：只保留三个 `STR_FUNCTION` 形式的 `current_*`，其 `currentTimeFunc` 直接调用 C 库的 `gmtime`/`gmtime_r` 与 `strftime`，格式串 `"%H:%M:%S"`、`"%Y-%m-%d"`、`"%Y-%m-%d %H:%M:%S"` 作为 user-data 传入。

**10. 非常规路径的守卫。** `now`、`subsec`、`localtime`、`utc` 这些依赖当前时间或本地时区的特性都先经过 `sqlite3NotPureFunc` 检查（实现在 `src/vdbeaux.c`）：当调用点是 `OP_PureFunc` 时，函数报「non-deterministic use of ...」错误并返回失败；因此 `date('now')` 出现在 CHECK 约束、生成列或索引表达式中会被拒绝。`isDate` 在 `argc==0` 时同样先过 `sqlite3NotPureFunc`，通过后才用 `setDateTimeToCurrent` 取当前时间。当前时间取自 `sqlite3StmtCurrentTime`（`src/vdbeapi.c`），同一条 prepared statement 的多次调用返回同一时刻。

## 关键取舍

- **源码事实：**`DateTime` 同时保存 `iJD` 与年月日时分秒，并用三个 `valid*` 标志标记可信来源，惰性换算由 `computeJD`/`computeYMD`/`computeHMS` 承担。**架构推论：**用「一份权威刻度 + 多份带标记的派生视图」换取解析器与格式化器的解耦——解析器只管填字段，格式化器只管读字段，二者互不感知；代价是每个消费点都要先确认对应 `valid*` 标志，否则会读到陈旧值。
- **源码事实：**内部刻度选的是儒略日 × 86400000 毫秒，而不是 Unix 毫秒。**架构推论：**选择儒略日是为了让「纯日期」与「带时刻」共用一条刻度而无需区分整日与小数日的编码；代价是纪元偏移常量 `21086676*(i64)10000` 必须反复出现在换算里，任何新增的刻度转换都要记得带这个偏移。
- **源码事实：**`parseDateOrTime` 的回退链是「`parseYyyyMmDd` → `parseHhMmSs` → `now` → 纯数字 → `subsec`」，每一步失败才试下一步。**架构推论：**单一入口多级回退让多种输入形态共享一套修饰符与换算逻辑；代价是输入的歧义由顺序决定，例如同时能被日期与时间解析的字符串永远先走日期分支。
- **源码事实：**`SQLITE_OMIT_DATETIME_FUNCS` 打开时，整套日期时间函数退化为三个只依赖 C 库 `strftime` 的 `current_*` 实现，注册表由 `PURE_DATE` 切换到 `STR_FUNCTION`。**架构推论：**为减小二进制体积提供开关级裁剪，代价是裁剪后 `date`/`time`/`datetime` 全部消失，仅够支撑 `DEFAULT CURRENT_TIME` 这类列声明。
- **源码事实：**`toLocaltime` 对超出 1970–2037 的日期先映射年份再计算，映射偏移 `iYearDiff = (2000 + x.Y%4) - x.Y`。**架构推论：**这是为了迁就 C 库 `localtime_r` 只在 32 位时间戳范围内可用的现实限制；代价是这些年份的夏令时规则被替换成了等价年份的规则，结果与真实时区表可能不同。
- **源码事实：**`sqlite3NotPureFunc` 让 `now`/`localtime`/`utc`/`subsec` 在 `OP_PureFunc` 语境下直接报错，而不是静默取一个近似值。**架构推论：**把「非确定性」当作硬错误而非降级，让索引与生成列不会建立在一个随时钟漂移的键上；代价是这些函数无法用于确定性索引表达式。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/date.c` | `sqlite3RegisterDateTimeFunctions` | 本文件唯一导出符号，登记全部日期时间 SQL 函数到全局函数哈希表。 |
| `src/date.c` | `isDate` | 统一入口：解析首个参数、逐个施加修饰符，最后做错误与 `validJulianDay` 复查。 |
| `src/date.c` | `parseDateOrTime` | 输入分派：`parseYyyyMmDd` → `parseHhMmSs` → `now` → 纯数字 → `subsec` 的回退链。 |
| `src/date.c` | `parseYyyyMmDd` | 解析 `YYYY-MM-DD`（可带 `T` 或空格分隔的时刻），并处理 `h==24` 的进位。 |
| `src/date.c` | `parseHhMmSs` | 解析 `HH:MM[:SS[.FFF]]`，小数秒上限截到 0.999。 |
| `src/date.c` | `parseTimezone` | 解析 `(+/-)HH:MM` 与 `Z`/`z` 时区后缀，写入 `p->tz`。 |
| `src/date.c` | `getDigits` | 按四位一组的格式串抽取整数，格式串第三位经 `aMx[]` 映射到最大值上限。 |
| `src/date.c` | `computeJD` | 年月日到 `iJD` 的换算，含年份越界检查与时区偏移扣减。 |
| `src/date.c` | `computeYMD` | `iJD` 到年月日的反向换算，含 `validJulianDay` 越界检查。 |
| `src/date.c` | `computeHMS` | `iJD` 到时分秒的换算。 |
| `src/date.c` | `computeFloor` | 依据 `daysOfMonth` 计算日溢出天数 `nFloor`。 |
| `src/date.c` | `daysOfMonth` | 每月天数表，用 `0x15aa` 位掩码识别 31 天月份，2 月按闰年规则取 29 或 28。 |
| `src/date.c` | `parseModifier` | 修饰符分派：`auto`/`ceiling`/`floor`/`julianday`/`localtime`/`unixepoch`/`utc`/`weekday`/`start of`/`end of`/`subsec` 与数量型偏移。 |
| `src/date.c` | `aXformType` | 数量型修饰符的单位表，给出各单位的量程上限 `rLimit` 与换算系数 `rXform`。 |
| `src/date.c` | `autoAdjustDate` | 依据数值大小在儒略日与 Unix 时间戳之间择一解释 `rawS`。 |
| `src/date.c` | `setRawDateNumber` | 把裸数值同时记为 `p->s` 并置 `rawS`，仅在 `r<5373484.5` 时安装为 `iJD`。 |
| `src/date.c` | `toLocaltime` | UTC 到本地时间的换算，跨 1970–2037 时先映射年份再换算。 |
| `src/date.c` | `osLocaltime` | 调用 `localtime_r`/`localtime_s`，并处理 `bLocaltimeFault` 测试注入。 |
| `src/date.c` | `setDateTimeToCurrent` | 取 `sqlite3StmtCurrentTime` 作为当前时刻。 |
| `src/date.c` | `juliandayFunc` | `julianday()` 实现，输出 `iJD/86400000.0`。 |
| `src/date.c` | `unixepochFunc` | `unixepoch()` 实现，按 `useSubsec` 决定输出整数秒还是浮点秒。 |
| `src/date.c` | `datetimeFunc` | `datetime()` 实现，用 `char zBuf[32]` 逐位拼装输出。 |
| `src/date.c` | `timeFunc` | `time()` 实现，用 `char zBuf[16]` 拼装 `HH:MM:SS`。 |
| `src/date.c` | `dateFunc` | `date()` 实现，用 `char zBuf[16]` 拼装 `YYYY-MM-DD`。 |
| `src/date.c` | `strftimeFunc` | `strftime()` 实现，用 `switch` 覆盖全部转换符并走 `sqlite3_str` 累加器。 |
| `src/date.c` | `daysAfterJan01` | 计算当年 1 月 1 日之后的天数，供 `%j`/`%U`/`%W`/`%V` 使用。 |
| `src/date.c` | `daysAfterMonday` | 计算最近一个周一之后的天数（0=周一），供 `%G`/`%g`/`%V`/`%W` 使用。 |
| `src/date.c` | `daysAfterSunday` | 计算最近一个周日之后的天数（0=周日），供 `%u`/`%w`/`%U` 使用。 |
| `src/date.c` | `timediffFunc` | `timediff()` 实现，按年/月分解后输出 `±YYYY-MM-DD HH:MM:SS.SSS`。 |
| `src/date.c` | `ctimeFunc` | `current_time()` 实现，转调 `timeFunc(context, 0, 0)`。 |
| `src/date.c` | `cdateFunc` | `current_date()` 实现，转调 `dateFunc(context, 0, 0)`。 |
| `src/date.c` | `ctimestampFunc` | `current_timestamp()` 实现，转调 `datetimeFunc(context, 0, 0)`。 |
| `src/date.c` | `currentTimeFunc` | `SQLITE_OMIT_DATETIME_FUNCS` 分支下的精简 `current_*`，用 `gmtime` 与 `strftime` 实现。 |
| `src/date.c` | `datedebugFunc` | 仅在 `SQLITE_DEBUG` 下编译，输出 `DateTime` 内部状态的 JSON。 |

## 与主核 / AI 的关系

SQLite 这套日期时间实现对主核有三点可借鉴：一是「单一内部刻度 + 带 `valid*` 标志的派生视图」这种自描述状态结构，主核的多模态时间戳若也需要多种来源（数据库时间、Unix 秒、ISO 文本、模型侧自定义格式），可以照此建一套 `DateTime` 等价物与惰性换算；二是修饰符分派表（`aXformType` 把单位名、量程上限、换算系数集中成一张表），主核做「按 N 天 / 按 N 个 token / 按 N 个 chunk」这类时间窗换算时可用同一形态把魔数收敛到一处；三是 `sqlite3NotPureFunc` 把「依赖当前时间」的调用在确定性语境（索引、生成列、CHECK 约束）下变成硬错误，主核在生成确定性缓存键或物化视图时需要同类守卫。

对 AI 侧而言，本模块暴露的正是模型与存储之间的「时间语义翻译层」：`strftime` 的转换符集合、`unixepoch`/`julianday` 的刻度换算、以及 `timediff` 的按月/年分解，都是把模型生成的自然语言时间（「上个月末」「最近一个工作日」「三周前」）映射到可比较的数值键上的可枚举操作面。`DateTime` 用「分量分解 + 溢出天数 `nFloor`」而非纯毫秒加法来处理「1 月 31 日加 1 个月」这类语义，这与模型侧的日历推理习惯天然对齐。

## 参考

- [SQLite 官方文档：Date And Time Functions](https://www.sqlite.org/lang_datefunc.html) —— 列出 `date`/`time`/`datetime`/`julianday`/`unixepoch`/`strftime`/`timediff` 的官方语义与全部修饰符、`strftime` 转换符。
- [SQLite 官方文档：Built-in Date And Time Functions 的 SQLite Source 页面](https://sqlite.org/src/file?name=src/date.c&ci=trunk) —— 对照 `src/date.c` 中 `DateTime`、`parseModifier`、`strftimeFunc` 的真实实现。
- [GitHub 镜像源码：sqlite/sqlite 的 src/date.c](https://github.com/sqlite/sqlite/blob/master/src/date.c) —— 本文全部符号、常量与调用顺序的对照来源。
