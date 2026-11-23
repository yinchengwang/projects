---
project: postgres
feature: tuple-attribute-mapping-conversion
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/common/attmap.c
    symbols: [make_attrmap, build_attrmap_by_position]
  - path: src/backend/access/common/tupconvert.c
    symbols: [convert_tuples_by_position, convert_tuples_by_name]
---

# Tuple 属性映射与转换

## 一句话职责

属性映射把输入 TupleDesc 的列号对应到输出 TupleDesc，并在两者物理列序不一致时重排 Datum 形成目标 tuple 或 slot。

## 机制是什么

`AttrMap` 的 `attnums[]` 以输出列为索引，保存对应输入列的 1-based attnum；0 表示输出列无对应输入值，转换时产生 NULL。映射器支持按位置或按列名匹配，都会忽略 dropped 列并检查匹配列的类型约束。`TupleConversionMap` 除映射外还保存输入/输出描述符引用与 Datum/NULL 工作数组，供 tuple 运行时转换复用。

本页只负责描述符之间的属性对应及值重排，不改变源/目标描述符，不推导元组物理字段偏移，也不做类型 coercion。位置匹配用于列顺序本来应一致的接口；名称匹配适用于继承/分区等列号可能不同的关系。若映射为一一同序且无需处理 missing attribute，设置函数返回 NULL，调用方可以直接使用原 tuple/slot。

## 怎么实现的

1. `make_attrmap(maplen)` 在当前 memory context 分配 `AttrMap` 及零初始化的 `attnums[]`；长度按目标（输出）描述符属性数计算，dropped 输出列因初始化为 0 而保持空映射。
2. `build_attrmap_by_position` 顺序扫描输出属性，跳过 dropped 项，并在输入端向前寻找下一个非 dropped 属性。每对位置列都检查 type OID 及适用的 typmod；非 dropped 列数量不符或类型不匹配时报 datatype mismatch。
3. `build_attrmap_by_name` 对每个非 dropped 输出列按名称搜索输入描述符，找到后要求 type 与 typmod 精确一致，写入实际输入 attnum。`nextindesc` 从上次匹配附近继续搜索，兼顾常见同序关系；找不到列时依 `missing_ok` 决定置 0 还是报错。
4. 两种路径都经 `check_attrmap_match` 判断是否无需转换；同序对应且没有输入 `atthasmissing` 时释放临时映射并返回 NULL，否则保留 map。dropped 列仅在两端都 dropped 且物理属性约束相容时可视为无需转换。
5. `convert_tuples_by_position` / `convert_tuples_by_name` 包装映射构造。需要转换时分配 `TupleConversionMap` 和可复用工作数组；描述符只被引用不被复制，因此必须比 map 活得更久。
6. 执行 tuple 转换时先按输入描述符解构到 Datum/NULL 数组，再按目标列索引从 `attnums[]` 选值，最后用目标描述符 form 新 tuple。slot 转换走类似的重排；0 输入映射被显式写成 NULL。map 与工作数组由 `free_conversion_map` 释放。

## 关键取舍

- **按位置匹配保持“位置即契约”，按名称匹配允许不同 attnum。** 位置算法不尝试用列名修复错序，而是逐项核对类型；因此上游输出列顺序错了会报类型/列数错误，而非静默重排。名称算法适配继承/分区关系的重排，但付出名称扫描与重复匹配检查成本。选择不同策略能让调用者明确列对应规则。
- **类型不一致时报错，不在此处做隐式转换。** 源码要求 type OID 与 typmod 满足兼容条件，执行期只是 Datum 搬运与重新构造 tuple。放弃“尽量转型”的通用性，避免将类型转换规则、错误语义和隐式 coercion 混入低层列映射；调用方若要类型转换，应在表达式/类型系统层显式完成。
- **返回 NULL 表示物理上不需重排，而非始终分配转换器。** 一一对应时释放 map，可节省逐行解构/重构成本；代价是调用方需识别 NULL 快路径。真实 caller 包括 `tstoreReceiver.c` 的 `convert_tuples_by_position`，它在描述符不兼容时需要转换，而同序时可不创建转换对象。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/common/attmap.c` | `make_attrmap` | 按输出属性数建立零初始化的输出列到输入列映射 |
| `src/backend/access/common/attmap.c` | `build_attrmap_by_position` | 跳过 dropped 列按序配对，并验证类型/列数兼容性 |
| `src/backend/access/common/attmap.c` | `build_attrmap_by_name` | 依属性名重排输入 attnum，检查类型并处理缺失列策略 |
| `src/backend/access/common/tupconvert.c` | `convert_tuples_by_position` | 构建位置映射，并为必要的重排配置可复用执行工作区 |
| `src/backend/access/common/tupconvert.c` | `convert_tuples_by_name` | 构建名称映射，返回名称重排所需的转换描述 |
| `src/backend/access/common/tupconvert.c` | `execute_attr_map_tuple` | 解构输入 tuple、按映射重排，再按目标描述符构造新 tuple |

## 与主核 / AI 的关系

暂无直接对应。主核如需适配不同列序，可借鉴“映射构建与逐行转换分离、同序时走无转换快路径”；此机制本身不提供 schema migration 或类型转换服务。

## 参考

- [PostgreSQL 官方源码：属性映射](https://github.com/postgres/postgres/blob/master/src/backend/access/common/attmap.c) —— 支撑位置/名称匹配、dropped 列和类型兼容规则
- [PostgreSQL 官方源码：Tuple 转换](https://github.com/postgres/postgres/blob/master/src/backend/access/common/tupconvert.c) —— 支撑转换 map 生命周期、执行重排与快路径
