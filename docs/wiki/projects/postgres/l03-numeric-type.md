---
project: postgres
feature: numeric-type
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/adt/numeric.c
    symbols: [numeric_in, numeric_out, numeric_add, numeric_cmp]
---

# numeric 与 decimal

## 一句话职责

numeric 以变长十进制数字序列表示任意精度数值，并提供受 typmod 约束的解析、舍入、算术与排序语义。

## 机制是什么

`Numeric` 是 varlena 值，其内容承载符号、display scale、weight 与以 `NBASE` 为基的数字序列；内部运算统一转为 `NumericVar`（`weight`、`sign`、`dscale`、`digits`、`ndigits`）以便对齐位数与进位。精度和标度通过 typmod 传入，超出 precision 时按规则报错，scale 决定显示与舍入的小数位。

numeric 负责数值本身的十进制语义与显示精度，不负责类型目录登记、文本缓冲区管理之外的 I/O 分派，也不处理 SQL 表达式的类型推导。`NaN`、`±Infinity` 作为特殊值参与比较与算术：`cmp_numerics` 明确把 NaN 排在所有非 NaN 之后、NaN 与 NaN 相等，这是为得到一致排序所做的取舍，而非 IEEE 浮点语义的照搬。

## 怎么实现的

1. `numeric_in` 先跳过前导空白、解析正负号；若符号后不是数字或小数点，则按 `NaN` / `Infinity` / `inf` 识别特殊值，并检查尾随内容只有空白。普通数值则交内部字符串转换构造 `NumericVar`，再依 typmod 舍入与校验。
2. typmod 由 `make_numeric_typmod` 打包：高 16 位存 precision，低 16 位中取 11 位存 scale，并加 `VARHDRSZ`。`apply_typmod` 先用 `round_var` 舍入到目标 scale，再按 `(weight + 1) * DEC_DIGITS` 估算整数位，必要时逐位扣除高位的十进制前导零以判断真实位数，超限则报 numeric field overflow。
3. `numeric_out` 对特殊值直接输出 `NaN`/`Infinity`/`-Infinity`；其余由 `init_var_from_num` 还原 `NumericVar`，`get_str_from_var` 依 weight、dscale 与基数字组装十进制文本，保证显示精度遵循 dscale。
4. `numeric_add` 转调 `numeric_add_safe`：先处理特殊值组合（NaN 传染，Inf 与相反 Inf 得 NaN），再 `init_var_from_num` 拆出两个操作数，由 `add_var` 按符号选择加绝对值或比较绝对值后相减，最后由 `make_result_safe` 打包回 Numeric。
5. `add_var` 内部以 `add_abs`/`sub_abs` 处理对齐与进位借位，并在相等抵消时用 `zero_var` 并把 `dscale` 取两者较大值，避免结果丢失显示精度信息。
6. `numeric_cmp` 直接调用 `cmp_numerics`；后者先按特殊值序给出结果，普通值再走 `cmp_var_common` 比较符号、weight 与数位，从而对任意精度数值给出三值比较。

## 关键取舍

- **源码事实：**内部以 `NBASE` 为基、`DEC_DIGITS` 位一组存储，`get_str_from_var` 在输出时按 dscale 补零/截断。**架构推论：**以十进制基存储换取精确的十进制舍入与显示，避免二进制浮点表示误差，代价是位运算不能直接复用、算术需处理按组进位。
- **源码事实：**`cmp_numerics` 注释说明把所有 NaN 视为相等且大于任何非 NaN（含 Infinity），理由是保持排序一致。**架构推论：**牺牲与 IEEE 比较规则一致的做法，换取索引与排序可用的全序；若让 NaN 比较结果不可比，btree 排序与哈希分组都会缺少确定的序关系。
- **源码事实：**`numeric_in` 在应用 typmod 前就检查尾随垃圾，注释说明希望语法错误优先于 typmod 语义错误；`apply_typmod` 在舍入之后才判溢出，因为舍入可能抬高 weight。**架构推论：**把校验顺序固定下来，使错误报告可预测且不误判边界值；若先按未舍入值判溢出，会把本可舍入进范围的数误报为溢出。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/adt/numeric.c` | `numeric_in` | 解析符号、特殊值与十进制文本，并按 typmod 校验。 |
| `src/backend/utils/adt/numeric.c` | `numeric_out` | 由内部表示按 dscale 生成十进制显示文本。 |
| `src/backend/utils/adt/numeric.c` | `numeric_add` | 处理特殊值后调用 `add_var` 完成任意精度加法。 |
| `src/backend/utils/adt/numeric.c` | `numeric_cmp` | 通过 `cmp_numerics` 给出含 NaN/Infinity 的全序比较结果。 |

## 参考

- [PostgreSQL 文档：numeric 类型](https://www.postgresql.org/docs/current/datatype-numeric.html) —— 说明任意精度数值的精度、标度与特殊值语义。
- [PostgreSQL 官方源码：numeric 实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/numeric.c) —— 对照十进制基存储、舍入与比较实现。

## 与主核 / AI 的关系

暂无直接对应。主核若需要精确十进制语义，可借鉴“按组十进制存储 + 显式 scale”与 NaN 全序约定；具体基数和 typmod 打包方式需按主核类型系统自行定义。
