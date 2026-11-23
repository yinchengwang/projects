---
project: postgres
feature: json-jsonb-types
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/adt/jsonb.c
    symbols: [jsonb_in, jsonb_out]
  - path: src/backend/utils/adt/jsonb_util.c
    symbols: [JsonbValueToJsonb, JsonbIteratorNext]
  - path: src/backend/utils/adt/jsonb_gin.c
    symbols: [gin_extract_jsonb]
---

# JSON/JSONB

## 一句话职责

jsonb 把 JSON 文本解析为可规范化存储的二进制容器，并提供遍历、文本还原与 GIN 倒排键抽取。

## 机制是什么

jsonb 的磁盘格式是 `Jsonb`（varlena）包裹 `JsonbContainer`：容器头部记录元素计数与类型标志，随后是 `JEntry` 数组描述每个子项的类型与长度/偏移，最后是按序排列的数据区。对象键经过排序去重，因此同值不同书写顺序的输入会得到相同二进制表示；标量在根层以单元素数组形式包裹。解析中间态是 `JsonbValue`，可在写入前做增量合并与去重。

`jsonb` 与 `json` 文本类型不同：后者保留原始文本与格式，本机制负责的是 jsonb 规范化容器。它不负责 SQL 级的 JSON 路径求值语义、不定义操作符语义，也不管理 GIN 索引的页结构；`jsonb_gin.c` 只把容器内容抽取成 GIN 可索引的键项。

## 怎么实现的

1. `jsonb_in` 取得输入字符串后转调 `jsonb_from_cstring`：构造 `JsonLexContext` 与语义动作表，把 object/array 起止、字段名与标量回调接到 `jsonb_in_*` 系列，再由 `pg_parse_json_or_errsave` 驱动解析。
2. 回调通过 `pushJsonbValue` 累积到 `JsonbInState`；对象字段按唯一的 key 处理，重复键依 unique_keys 设置取舍。解析结束时 `state.result` 是根 `JsonbValue`。
3. `JsonbValueToJsonb` 把结果转成最终值：标量包装成 `rawScalar` 的单元素数组，并用 `pushJsonbValue` 依次发出 BEGIN_ARRAY/ELEM/END_ARRAY 后 `convertToJsonb`；`jbvObject`/`jbvArray` 直接序列化；已是 `jbvBinary` 时仅按长度复制 `val.binary.data`。
4. `convertToJsonb` 保留 varlena 头部空间后调用 `convertJsonbValue` 递归写入，每个节点返回 `JEntry` 描述长度与类型位；对象/数组的递归序列化使子项可以用长度或结束偏移定位，根节点的 `JEntry` 被丢弃，改由容器头表达根类型。
5. `JsonbIteratorNext` 是只前进不递归的游标：`JBI_ARRAY_START`/`JBI_OBJECT_START` 返回 `WJB_BEGIN_*` 并压入子迭代器，随后按 `curIndex` 与 `curDataOffset` 逐个返回 `WJB_ELEM`/`WJB_KEY`/`WJB_VALUE`，元素耗尽时 `freeAndGetParent` 回到父层并返回 END token；`skipNested` 为真时可跳过整段嵌套容器。
6. `jsonb_out` 调 `JsonbToCString(NULL, &jb->root, VARSIZE(jb))` 把容器重新渲染为 JSON 文本。`gin_extract_jsonb` 则用 `JB_ROOT_COUNT` 取根元素数，以 `2 * total` 预分配 `GinEntries`，遍历时把 KEY、数组元素中的字符串、VALUE 分别经 `make_scalar_key` 编码为 GIN 键项后返回。

## 关键取舍

- **源码事实：**对象键在容器中排序去重，标量根值以 `rawScalar` 单元素数组包装，`JEntry` 用长度或结束偏移描述子项。**架构推论：**以写入时的排序与规范化成本换取读取查找和等值比较的确定性，且顶层统一为容器简化遍历代码；若保留输入顺序与原文本，则无法保证同值同表示，索引与比较需额外规范化。
- **源码事实：**`make_text_key` 对超过 `JGIN_MAXLENGTH`（源码宏值 125 字节）的文本改为哈希后写入并置 `JGINFLAG_HASHED`，且数值先经 `numeric_normalize` 转成无尾随零的文本。**架构推论：**以哈希碰撞导致的额外重检为代价限制 GIN 键长度；若不截断，超长字符串会让索引项过大，而数值若不规范化则 `1.0` 与 `1` 会生成不同键。
- **源码事实：**`JsonbIteratorNext` 用显式迭代器状态与父链替代递归，并在元素取尽后释放子迭代器；`gin_extract_jsonb` 忽略结构 token，只收集键与值项。**架构推论：**把深度优先遍历转为可暂停的游标，避免大文档一次性展开为完整对象树；若递归返回整棵 `JsonbValue`，深嵌套文档的内存与栈开销会显著放大。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/adt/jsonb.c` | `jsonb_in` | 接收文本并驱动语义动作回调完成解析。 |
| `src/backend/utils/adt/jsonb.c` | `jsonb_out` | 将二进制容器渲染回 JSON 文本。 |
| `src/backend/utils/adt/jsonb_util.c` | `JsonbValueToJsonb` | 把解析结果 `JsonbValue` 规范化为可存储 Jsonb。 |
| `src/backend/utils/adt/jsonb_util.c` | `JsonbIteratorNext` | 以非递归游标逐项产出容器的结构/键/值 token。 |
| `src/backend/utils/adt/jsonb_gin.c` | `gin_extract_jsonb` | 遍历容器并抽取键与值，生成 GIN 索引项。 |

## 参考

- [PostgreSQL 文档：JSON 类型](https://www.postgresql.org/docs/current/datatype-json.html) —— 说明 json 与 jsonb 的存储差异及 jsonb 的索引能力。
- [PostgreSQL 官方源码：jsonb 容器与迭代](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/jsonb_util.c) —— 对照 JEntry 布局、序列化与游标遍历。

## 与主核 / AI 的关系

暂无直接对应。主核若需要结构化文档类型，可借鉴“解析中间态 + 规范化二进制容器 + 可暂停游标”的分层；GIN 键编码与长度哈希策略需按主核索引实现确定。
