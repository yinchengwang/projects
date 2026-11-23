---
project: postgres
feature: text-and-varlena
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/adt/varlena.c
    symbols: [cstring_to_text, text_to_cstring, text_catenate, text_length]
  - path: src/backend/utils/adt/varchar.c
    symbols: [varchar, bpchar]
---

# text 与 varlena

## 一句话职责

text 作为变长字符串的规范表示，在 C 字符串与 varlena 值之间转换，并按声明长度实施截断与补空语义。

## 机制是什么

text 与 varchar、char(n)、bpchar 都是 varlena 家族：头部用 `VARSIZE` 系列宏描述总长度，正文是原始字节。`VARHDRSZ` 与短头部的差别通过 `VARSIZE_ANY_EXHDR`、`VARDATA_ANY`、`PG_DETOAST_DATUM` 等宏屏蔽，因此同一份代码可处理短头部、压缩或已 toast 的值。字符数长度需按数据库编码计算，只有在编码最大字节长度为 1 时才等价于字节数。

本机制负责字符串表示、长度计算、拼接与长度约束强制，不负责正则匹配、排序比较或字符串索引策略；`varchar.c` 中 `varchar` 与 `bpchar` 是两个 typmod 长度强制函数，它们不解析文本、不新建独立存储格式，而是复用 text 的二进制布局。varlena 本身也不承诺正文是否已解 TOAST 或压缩，调用者需按各函数文档使用相应解包宏。

## 怎么实现的

1. `cstring_to_text` 以 `strlen` 取长度后转调 `cstring_to_text_with_len`；后者 `palloc(len + VARHDRSZ)`、`SET_VARSIZE` 写入总长度并 `memcpy` 正文，产生一个全新分配的完整头部 text 值。
2. `text_to_cstring` 用 `pg_detoast_datum_packed` 解包压缩/外置值，按 `VARSIZE_ANY_EXHDR` 取正文字节数，分配 `len + 1` 字节、复制正文并显式置 `'\0'`；若解包产生了新副本则 `pfree` 释放，避免调用者拿到内部临时块。
3. `text_length` 走两条路径：`pg_database_encoding_max_length() == 1` 时直接用 `toast_raw_datum_size(str) - VARHDRSZ` 得到字节长度，无需解包；多字节编码才 `DatumGetTextPP` 后以 `pg_mbstrlen_with_len` 统计字符。
4. `text_catenate` 假定入参为短头部但未压缩、未外置的 text，取两侧 `VARSIZE_ANY_EXHDR`，分配 `len1 + len2 + VARHDRSZ`，按偏移分别 `memcpy` 两段正文。它对负长度做了防御性归零，并注明此处更合理的做法是报错。
5. `bpchar` 先按 typmod 减去 `VARHDRSZ` 得到目标字符数 `maxlen`；用 `pg_mbstrlen_with_len` 求实际字符数。字符数超限时以 `pg_mbcharcliplen` 得到安全字节截断点，非显式转换还要求被截掉的部分全是空格，否则报右截断错误；字符数不足时把差额字节补空格，从而保持定长语义。
6. `varchar` 先按字节长度判断是否已满足 `maxlen`；超限时同样按多字节边界截断，隐式转换要求多余字符均为空格，然后用 `cstring_to_text_with_len` 构造结果。两函数都通过 escontext 支持软错误返回。

## 关键取舍

- **源码事实：**`text_length` 在单字节编码数据库下直接读原始 datum 尺寸而不解包，多字节编码才逐字符扫描。**架构推论：**以分支换取常见编码下的常数时间长度查询，代价是长度语义依赖数据库编码；若统一走 `pg_mbstrlen_with_len`，单字节场景也要解包并扫描全部字节。
- **源码事实：**`bpchar` 对不足长度的值补空格、对超长值仅在显式转换时静默截断，隐式转换要求多余字符为空格。**架构推论：**定长 char(n) 的填充与截断规则与 SQL 语义对齐，避免隐式赋值悄悄丢弃非空格数据；若隐式转换也静默截断，列赋值会丢失用户未察觉的字符。
- **源码事实：**`text_to_cstring` 对解包副本做 `pfree`，`text_catenate` 要求入参未被压缩或外置。**架构推论：**把解包生命周期收在原函数内部，避免调用者误释放缓存值；若把未解包或已压缩的 text 直接传入 `text_catenate` 并假定布局，正文偏移会算错。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/adt/varlena.c` | `cstring_to_text` | 由 C 字符串分配带完整 varlena 头部的 text 值。 |
| `src/backend/utils/adt/varlena.c` | `text_to_cstring` | 解包 text 并复制为以 NUL 结尾的 C 字符串。 |
| `src/backend/utils/adt/varlena.c` | `text_catenate` | 按两侧正文字节数分配并拼接为新的 text 值。 |
| `src/backend/utils/adt/varlena.c` | `text_length` | 按编码特征选择字节长度或逐字符计数。 |
| `src/backend/utils/adt/varchar.c` | `varchar` | 按 typmod 截断 varchar 值，区分隐式与显式转换。 |
| `src/backend/utils/adt/varchar.c` | `bpchar` | 按 typmod 截断或补空格，实现定长字符语义。 |

## 参考

- [PostgreSQL 文档：字符类型](https://www.postgresql.org/docs/current/datatype-character.html) —— 说明 char(n)/varchar(n)/text 的长度与填充差异。
- [PostgreSQL 官方源码：varlena 字符串函数](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/varlena.c) —— 对照 text 构造、解包与长度计算。

## 与主核 / AI 的关系

暂无直接对应。主核可采用“变长头部 + 统一解包宏”的表示，但长度语义与定长填充规则需结合主核字符编码与列类型约束确定。
