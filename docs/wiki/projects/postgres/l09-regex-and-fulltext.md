---
project: postgres
feature: regex-and-fulltext
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/adt/regexp.c
    symbols: [RE_compile_and_execute, textregexeq, regexp_matches]
  - path: src/backend/utils/adt/tsvector_op.c
    symbols: [TS_execute, ts_match_tt]
  - path: src/backend/utils/adt/tsquery.c
    symbols: [parse_tsquery]
---

# 正则与全文

## 一句话职责

正则与全文机制把模式或查询表达式编译成可复用的内部形式，再对字符串或词位向量判定是否匹配并可选地返回命中信息。

## 机制是什么

正则路径以 Spencer 正则引擎为后端：模式文本按编码转为宽字符后编译，编译结果按「模式+编译标志+collation」缓存，执行时把目标串也转为宽字符并调用引擎。全文路径是另一套机制：`tsquery` 把查询文本解析为逆波兰形式的 `QueryItem` 序列（AND/OR/NOT/PHRASE 与词位操作数），`tsvector` 保存词位及其位置/权重；`TS_execute` 按查询树递归求值，由回调判断某个词位操作数是否存在。

两者都属于「模式/表达式 + 数据 → 布尔结果（可带附加信息）」，但语义层完全不同：正则匹配字符序列，全文匹配规范化后的词位并支持位置短语与权重。本机制不负责 `to_tsvector` 的分词与词典规范化细节，也不负责 GIN/GiST 的索引页结构；正则函数只调用引擎，不实现字符类、量词的匹配算法。

## 怎么实现的

1. `RE_compile_and_execute` 先按调用者是否需要子匹配设置 `REG_NOSUB`，再 `RE_compile_and_cache` 取已编译模式，最后 `RE_execute` 执行。是否返回捕获组信息由 `nmatch` 决定，因此同一模式在"只判真假"与"要子匹配"两种用途下可能对应不同编译标志。
2. `RE_compile_and_cache` 在缓存数组中按长度、编译标志、collation 与模式字节逐项比较，命中则移到数组前端（自组织策略），未命中才转宽字符并新建独立内存上下文编译，避免编译中途出错泄漏缓存资源。
3. `textregexeq` 是 `~` 操作符的实现：取左操作数为数据、右操作数为模式，固定 `REG_ADVANCED` 与当前 collation，`nmatch = 0` 表示只要布尔结果，因此可走 `REG_NOSUB` 快速路径。
4. `regexp_matches` 是集合返回函数：首次调用时在 `multi_call_memory_ctx` 里解析 flags、把输入串复制进多调用上下文，并由 `setup_regexp_matches` 一次性完成全部匹配（把原串转宽字符、编译模式、记录各次匹配与子表达式位置），后续每次调用只从已算好的位置表构造一个结果数组，直到耗尽。
5. `parse_tsquery` 按 flags 选择 tokenizer（标准、plain 或 websearch 风格），初始化 `TSQueryParserStateData` 与值解析器，`makepol` 生成后序（逆波兰）`QueryItem` 列表；空词位时发 NOTICE 并返回 size 为 0 的查询，过大则由 `TSQUERY_TOO_BIG` 报错，否则按 `COMPUTESIZE` 打包头部、QueryItem 与操作数字符串。
6. `TS_execute` 调 `TS_execute_recurse` 并在结果不等于 `TS_NO` 时返回 true。递归按操作符处理：`OP_NOT` 在 `TS_EXEC_SKIP_NOT` 下直接视为真，`OP_AND`/`OP_OR` 分别短路，`OP_PHRASE` 交给 `TS_phrase_execute`；词位操作数走回调 `chkcond`，并以 `TS_MAYBE` 三值结果表达"仅当提供位置信息时才可确定"。
7. `ts_match_tt` 是文本对文本的便捷入口：先把左侧经 `to_tsvector` 规范化、右侧经 `plainto_tsquery` 解析，再调 `ts_match_vq`；后者把 `tsvector` 的词位数组与操作数字符串放入 `CHKVAL`，交给 `TS_execute` 配 `checkcondition_str` 做二分查找与权重/位置检查。

## 关键取舍

- **源码事实：**正则编译结果按模式、flags、collation 缓存，命中时把条目移到数组前端；`nmatch < 2` 会自动加入 `REG_NOSUB`。**架构推论：**以缓存查找与搬移换取重复查询免编译，代价是同一模式在"要子匹配"与"不要子匹配"场景下会编译成两份；若按模式单一缓存，就无法同时获得 `REG_NOSUB` 的执行收益。
- **源码事实：**`regexp_matches` 首次调用就一次性算出全部匹配位置，后续调用只组装结果。**架构推论：**把匹配成本集中到首调用，使 SRF 每次返回的固定开销很低，代价是即使调用者只取第一行也已付出全量扫描；若逐次增量匹配，则每次都要维护可恢复的引擎状态。
- **源码事实：**`TS_execute` 用三值逻辑，`TS_MAYBE` 只在调用者未提供位置信息（如 `TS_EXEC_PHRASE_NO_POS`）时出现，`TS_execute` 把非 `TS_NO` 都当作真。**架构推论：**以"可能匹配"这一第三值保留短语位置信息缺失时的保守判定，避免误判为不匹配；若强行二值化，需要要么高估命中、要么漏掉本应命中的短语。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/adt/regexp.c` | `RE_compile_and_execute` | 决定是否要子匹配、取编译缓存并执行匹配。 |
| `src/backend/utils/adt/regexp.c` | `textregexeq` | `~` 操作符入口，以固定编译标志与当前 collation 判定匹配。 |
| `src/backend/utils/adt/regexp.c` | `regexp_matches` | 集合返回函数，首调用完成全部匹配、逐次输出结果数组。 |
| `src/backend/utils/adt/tsvector_op.c` | `TS_execute` | 遍历 tsquery 后序操作符树并按回调判定词位是否存在。 |
| `src/backend/utils/adt/tsvector_op.c` | `ts_match_tt` | 文本对文本入口，先经 to_tsvector/plainto_tsquery 再复用向量查询匹配。 |
| `src/backend/utils/adt/tsquery.c` | `parse_tsquery` | 依 flags 选 tokenizer，生成逆波兰 QueryItem 并打包为 TSQuery。 |

## 参考

- [PostgreSQL 文档：正则表达式](https://www.postgresql.org/docs/current/functions-matching.html) —— 说明 `~` 类操作符与 `regexp_matches` 的语义差异。
- [PostgreSQL 文档：文本搜索查询](https://www.postgresql.org/docs/current/datatype-textsearch.html) —— 介绍 tsvector/tsquery 表示与短语、权重语义。
- [PostgreSQL 官方源码：tsquery 解析](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/tsquery.c) —— 对照 tokenizer 选择与逆波兰打包。

## 与主核 / AI 的关系

暂无直接对应。主核可借鉴"编译结果按模式指纹缓存"与"表达式树 + 回调求值"的两层结构；正则引擎与分词词典属于外部能力，需单独选型。
