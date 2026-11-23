---
project: postgres
feature: array-types
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/adt/arrayfuncs.c
    symbols: [array_in, array_out, construct_md_array, deconstruct_array]
---

# 数组类型

## 一句话职责

数组类型把同类型元素连同维度与下标下界打包成单一 varlena 值，并提供文本构造、元素解构与外部格式输出。

## 机制是什么

`ArrayType` 头部记录维度数、数据偏移与元素类型 OID，随后是每维长度与下界数组、可选 NULL bitmap 和按元素类型对齐的数据区。数组本身是一维连续存储，多维只以维度/下界表达，元素位置由下标换算而来；元素类型信息（长度、按值传递、对齐、定界符、I/O 函数）在首次调用时缓存进 `ArrayMetaState`。

本机制负责数组容器与元素读写契约，不负责元素类型自身的比较、排序或运算，也不负责 expanded array 的延迟物化策略细节（`ExpandedArrayHeader` 属于表示变体）。数组维度上限由 `MAXDIM` 约束；空数组不保留维度信息，`construct_md_array` 对零元素直接返回空数组。

## 怎么实现的

1. `array_in` 从字符串与元素类型 OID 出发，先在 `fn_extra` 中缓存 `ArrayMetaState`（含元素 I/O 函数 `FmgrInfo`），避免同一 I/O 函数反复查目录。
2. 若输入以 `[n]` 或 `[m:n]` 开头，`ReadArrayDimensions` 解析显式维度与下界，并校验维度数不超过 `MAXDIM`、上界不小于下界、上界不得为 `INT_MAX`；否则 `dim[]` 保持 -1 由嵌套花括号推断。
3. `ReadArrayStr` 逐 token 解析花括号层级与元素文本，按元素类型的分隔符切分，调用元素输入函数得到 `Datum` 与 NULL 标记，同时累计元素数与各层长度。
4. 解析完成后按元素值大小、对齐与 NULL 情况计算总字节数，添加数组头与 bitmap 开销，`palloc0` 分配并填写 `ndim`、`dataoffset`、`elemtype`、维度与下界，再按对齐规则写入元素。`construct_md_array` 是同一构造逻辑的数组入口，并拒绝维度为负、超过 `MAXDIM` 或总尺寸超过分配上限的请求。
5. `array_out` 依 `AARR_DIMS` / `AARR_LBOUND` 取维度与下界；若任一维下界不为 1，则必须在输出前加 `[lb:ub]` 显式维度说明，否则文本无法还原下标。它先用元素输出函数把每个值转为字符串，统计需加引号或转义的情况与总长度，再按嵌套花括号布局写出。
6. `deconstruct_array` 用 `ArrayGetNItems` 算元素数，分配 `Datum[]` 与可选 `bool[]`，读取 bitmap 判断 NULL，按元素类型长度与对齐前进，返回扁平元素数组供上层运算；调用者提供元素类型信息，函数只断言传入 OID 与数组头一致。

## 关键取舍

- **源码事实：**多维数组仍以单一连续数据区存储，维度与下界只出现在头部；元素访问需根据下标与维度换算线性位置。**架构推论：**这使数组可作为单个 varlena 值参与存储、传输与快照，并把维度信息开销压到头部；若按嵌套子数组分配，嵌套层级会带来额外指针与非连续内存，但对元素寻址规则并无额外语义收益。
- **源码事实：**`array_out` 仅在下界不为 1 时输出显式维度前缀，否则省略。**架构推论：**牺牲“输出总带维度”的可读一致性，换取常见 1 基数组的紧凑文本；若总是输出维度前缀，默认数组的外部表示会显著冗长，且与历史输入格式的简洁惯例不符。
- **源码事实：**`array_in` 用 `fn_extra` 缓存元素类型 I/O 信息，元素类型变化时重新获取；`construct_md_array` 对 `elmlen == -1` 的元素先 detoast。**架构推论：**批量解析时避免逐元素查系统目录，并保证存储的元素不依赖外部 TOAST 指针；若每元素都重新查类型或保留外置引用，解析成本与生命周期风险都会上升。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/adt/arrayfuncs.c` | `array_in` | 解析外部数组文本、推断或读取维度并构造 ArrayType。 |
| `src/backend/utils/adt/arrayfuncs.c` | `array_out` | 依据维度/下界把数组元素格式化为带花括号的外部文本。 |
| `src/backend/utils/adt/arrayfuncs.c` | `construct_md_array` | 由 Datum/NULL 数组和维度信息组装扁平多维 ArrayType。 |
| `src/backend/utils/adt/arrayfuncs.c` | `deconstruct_array` | 按元素类型布局与 NULL bitmap 拆出扁平 Datum 数组。 |

## 参考

- [PostgreSQL 文档：数组类型](https://www.postgresql.org/docs/current/arrays.html) —— 说明数组下标、维度与文本表示。
- [PostgreSQL 官方源码：数组函数](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/arrayfuncs.c) —— 对照数组构造、输出与解构实现。

## 与主核 / AI 的关系

暂无直接对应。主核若实现数组值，可借鉴“扁平数据区 + 头部维度/下界”的表示；元素对齐、NULL bitmap 与文本转义规则需按主核存储格式定义。
