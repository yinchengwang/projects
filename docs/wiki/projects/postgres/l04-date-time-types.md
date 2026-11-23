---
project: postgres
feature: date-time-types
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/adt/timestamp.c
    symbols: [timestamp_in, timestamp_out, timestamptz_in, interval_in]
  - path: src/backend/utils/adt/datetime.c
    symbols: [DecodeDateTime, ParseDateTime]
  - path: src/backend/utils/adt/date.c
    symbols: [date_in, date_out]
---

# 日期时间类型

## 一句话职责

日期时间类型把外部文本解析成内部时间点或时间跨度，并在输出时按 DateStyle 与显示精度还原文本。

## 机制是什么

`date`、`timestamp`、`timestamptz`、`interval` 使用共享的文本解析前端：`ParseDateTime` 把输入切成字段并标注字段类别，`DecodeDateTime` 将这些字段合成为 `struct pg_tm`、秒以下小数与可选时区偏移。timestamp 系列再经 `tm2timestamp` 转换成以儒略日与日内时间为基准的内部整数，date 用 `DateADT` 直接表达日序，interval 用月/日/微秒三段分别保存。

这些函数负责类型的文本 I/O 与 typmod 精度约束，不负责时区数据库规则本身的维护、时间运算语义（如加减、跨时区比较）也不在本文范围。`timestamptz_in` 与 `timestamp_in` 共享解析流程，差别在于是否读取并应用时区；输出时 timestamp 不带时区，而 timestamptz 的显示依赖会话时区与 DateStyle。

## 怎么实现的

1. `ParseDateTime` 跳过字段间空白，按首字符判断字段类型：数字可能是时间、日期或纯数字；小数点后接数字视为小数秒；字母串可能是月名、星期、特殊词或时区名。它把规范化后的字段指针与类型数组返回，超出字段上限或缓冲区不足即返回格式错误。
2. `DecodeDateTime` 按字段类型逐个消费并累积 `fmask`：识别日期分量、时间分量、时区说明与 `AD/BC`、儒略日、ISO 周日期等前缀形式；已经出现月与日时，后续数字字段可能被当作时区复合字段拆分。缺少必需日期分量、字段重复或矛盾时返回对应的 `DTERR` 错误码。
3. `timestamp_in` / `timestamptz_in` 调用上述两步，`dtype` 为普通日期时用 `tm2timestamp` 转换，`DTK_EPOCH` 取纪元时间，`DTK_LATE`/`DTK_EARLY` 生成无穷边界值。`timestamptz_in` 把解析出的时区偏移传入 `tm2timestamp`，从而把本地时间转为 UTC 时间点。
4. 解析成功后再由 `AdjustTimestampForTypmod` 按类型精度舍入，超出可表示范围时报告越界；`date_in` 只接受日期类别并借助 `IS_VALID_JULIAN` 检查转换范围。
5. `interval_in` 用同一前端切字段，但改由 `DecodeInterval` 解释年月日与时分秒；若该路径报格式错误，则再尝试 `DecodeISO8601Interval` 解析 ISO 8601 形式，最后由 `itmin2interval` 填充分段字段并按 typmod 调整。
6. 输出侧：`date_out` 对有限值把 `DateADT` 经 `j2date` 还原为年月日再 `EncodeDateOnly`；`timestamp_out` 对有限值 `timestamp2tm` 得到 `pg_tm` 与 fsec 后 `EncodeDateTime`，不传时区；无穷值统一走 `EncodeSpecialTimestamp` 等特殊文本路径。

## 关键取舍

- **源码事实：**文本解析被拆成 `ParseDateTime`（词法切分与字段分类）与 `DecodeDateTime`（语义合成与校验）两层，date/timestamp/timestamptz/interval 各自复用前端。**架构推论：**以一处共享切分规则保证多类型输入语法一致，代价是各类型需自行解释字段组合；若每类型独立实现词法，格式扩展会出现不一致的接受范围。
- **源码事实：**timestamp 的内部表示是时间点，timestamptz 在输入时即时应用时区偏移；输出时 `timestamp_out` 的 `timestamp2tm` 传 NULL 时区参数。**架构推论：**把时区归一化放在输入边界，使内部比较与存储不受会话时区影响，显示再现则由输出路径决定；若存储本地时间并随时区换算，同一时间点在不同会话中会得到不同的存储值。
- **源码事实：**`interval_in` 在 `DecodeInterval` 返回 `DTERR_BAD_FORMAT` 时才回退尝试 `DecodeISO8601Interval`，其他错误码直接报错。**架构推论：**仅在语法不被传统格式接受时启用 ISO 8601 路径，避免两种语法语义冲突时静默择一，代价是部分歧义输入仍按传统规则解释。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/adt/timestamp.c` | `timestamp_in` | 解析文本、选择 dtype 分支并转换为时间点后应用 typmod。 |
| `src/backend/utils/adt/timestamp.c` | `timestamp_out` | 把内部时间点还原为 `pg_tm` 并按 DateStyle 格式化。 |
| `src/backend/utils/adt/timestamp.c` | `timestamptz_in` | 在共享解析流程中读取时区偏移并归一化为 UTC 时间点。 |
| `src/backend/utils/adt/timestamp.c` | `interval_in` | 解析时间跨度字段，含传统格式与 ISO 8601 回退路径。 |
| `src/backend/utils/adt/datetime.c` | `DecodeDateTime` | 将已分类字段合成为日期时间分量并校验一致性。 |
| `src/backend/utils/adt/datetime.c` | `ParseDateTime` | 切分输入字段并标注类型，供各日期时间输入函数复用。 |
| `src/backend/utils/adt/date.c` | `date_in` | 复用解析前端并限定只接受日期类别，构造 `DateADT`。 |
| `src/backend/utils/adt/date.c` | `date_out` | 由日序还原年月日并按 DateStyle 输出日期文本。 |

## 参考

- [PostgreSQL 文档：日期时间类型](https://www.postgresql.org/docs/current/datatype-datetime.html) —— 说明 timestamp、timestamptz、date 与 interval 的语义差异。
- [PostgreSQL 官方源码：日期时间解析](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/datetime.c) —— 对照字段切分与日期时间合成规则。

## 与主核 / AI 的关系

暂无直接对应。可借鉴“共享词法前端 + 各类型语义解释”的划分；时区数据库与时区归一化策略需由主核自行引入与验证。
