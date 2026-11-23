---
project: redis
feature: intset-encoding
page_type: feature
schema_version: 1
refs:
  - path: src/intset.c
    symbols: [intsetAdd, intsetSearch]
  - path: src/t_set.c
    symbols: [setTypeMaybeConvert]
---

# 整数集合编码

## 一句话职责

整数集合把一组整数按升序塞进一段定长数组，用「按元素宽度分级」的紧凑表示省内存；元素超出当前宽度就整表升级，元素数量超阈值就整表换编码。

## 机制是什么

`src/intset.h` 里 `intset` 是个极简结构：`uint32_t encoding`、`uint32_t length`、`int8_t contents[]`——8 字节头，后面接一串定长整数。

`src/intset.c` 里宽度分三级，源码注释写明「Note that these encodings are ordered, so: INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64」，三档定义分别是 `sizeof(int16_t)`、`sizeof(int32_t)`、`sizeof(int64_t)`，即 2、4、8 字节。整张表**只有一种宽度，所有元素共用**。

元素按数值**升序**存放，于是查找可以二分、插入位置可以由查找结果直接给出。

宽度选哪一级由元素的取值范围决定：`_intsetValueEncoding` 判「超出 int32 范围 → INT64；超出 int16 范围 → INT32；否则 INT16」。新表由 `intsetNew` 建立，初始宽度是 INT16。

整数集合是 Redis 的**集合类型（SET）的一种内部编码**（`OBJ_ENCODING_INTSET`），不是独立的逻辑类型。它的价值在于：全是整数的集合可以只存数字，不必为每个成员付一份 sds 的开销。

本页只讲整数集合内部的插入、查找与宽度升级，以及集合类型在整数集合与通用编码之间切换的判据。哈希表编码的集合语义在 F 组，字符串编码的一般性选择规则在 d02。

## 怎么实现的

**宽度判定 `_intsetValueEncoding`。** 三档：值落在 int32 之外 → `INTSET_ENC_INT64`；落在 int16 之外 → `INTSET_ENC_INT32`；否则 `INTSET_ENC_INT16`。因为编码有序（16 < 32 < 64），「比当前编码宽」就等价于「装不下」。

**查找 `intsetSearch`。** 注释写明「Search for the position of "value". Return 1 when the value was found and sets "pos" to the position of the value within the intset. Return 0 when the value is not present in the intset and sets "pos" to the position where "value" can be inserted.」流程是：

1. 长度为 0 时直接 `*pos = 0` 返回 0。
2. 否则先做两次边界判断（利用升序）：值大于最大值 → `*pos = length` 返回 0；值小于最小值 → `*pos = 0` 返回 0。注释写明这是「the case where we know we cannot find the value, but do know the insert position」。
3. 然后标准二分：`mid = ((unsigned int)min + (unsigned int)max) >> 1`，按 `value > cur` / `value < cur` 收缩区间。
4. 命中则 `*pos = mid` 返回 1；未命中则 `*pos = min`（应插入位置）返回 0。

**插入 `intsetAdd`。** 注释写明「Insert an integer in the intset」。顺序是：

1. 先算 `valenc = _intsetValueEncoding(value)`，并把 `*success` 预置为 1。
2. **宽度升级分支**：`if (valenc > intrev32ifbe(is->encoding))` 调 `intsetUpgradeAndAdd`。源码注释解释了为什么这时不必先查重：「If we need to upgrade, we know that this value should be either appended (if > 0) or prepended (if < 0), because it lies outside the range of existing values.」并补一句「This always succeeds, so we don't need to curry *success.」
3. **同宽分支**：先 `intsetSearch(is,value,&pos)`；命中说明已存在，把 `*success` 置 0 并原样返回；未命中则 `intsetResize(is, length+1)` 扩容、`intsetMoveTail(is,pos,pos+1)` 把 pos 之后的元素整体后移一格。
4. 最后 `_intsetSet(is,pos,value)` 写入、`is->length` 加一。

**宽度升级 `intsetUpgradeAndAdd`。** 注释是「Upgrades the intset to a larger encoding and inserts the given integer.」步骤：

1. 取旧编码 `curenc`、新编码 `newenc`、旧长度，并按值的正负算 `prepend = value < 0 ? 1 : 0`。
2. **先把 `is->encoding` 换成新编码，再 `intsetResize(is, length+1)`**——顺序不能反，搬迁期间读写必须用同一套宽度。
3. 搬迁**从后往前**：`while(length--) _intsetSet(is, length+prepend, _intsetGetEncoded(is, length, curenc));`。源码注释写明「Upgrade back-to-front so we don't overwrite values.」
4. 新值落位：`prepend` 为真写在下标 0，否则写在扩容后的末位。

**集合层的切换判据 `setTypeMaybeConvert`。** `src/t_set.c` 里它的注释是「Check if the existing set should be converted to another encoding based off the size hint.」判据只有两条：

- 集合当前是 `OBJ_ENCODING_LISTPACK` 或 `OBJ_ENCODING_LISTPACK_EX`，且 `size_hint > server.set_max_listpack_entries` → 转 `OBJ_ENCODING_HT`。
- 集合当前是 `OBJ_ENCODING_INTSET`，且 `size_hint > server.set_max_intset_entries` → 转 `OBJ_ENCODING_HT`。

两条都走 `setTypeConvertAndExpand(set, OBJ_ENCODING_HT, size_hint, 1)`。两个阈值都是运行时可配置项：`src/config.c` 里 `set-max-intset-entries` 默认 512、`set-max-listpack-entries` 默认 128。

**真实 caller。** `setTypeMaybeConvert` 的 caller 是 `src/t_set.c` 的 `saddCommand`——且只在键已存在时调用（新建集合走 `setTypeCreate`，由工厂方法按 `size_hint` 直接选初始编码），传进去的 `size_hint` 是 `c->argc - 2`，即这次 SADD 携带的成员个数。

**另外三条与整数集合相关的退化路径。**

- **插入后越界转哈希表。** `src/t_set.c` 的 `setTypeAddAuxInline` 在插入成功后判 `if (added && set->encoding == OBJ_ENCODING_INTSET) maybeConvertIntset(set);`，注释写明「Post-insert overflow check: an intset that grew past the configured limit converts to a hash table.」`maybeConvertIntset` 判 `intsetLen(subject->ptr) > intsetMaxEntries()` 就调 `setTypeConvert(subject, OBJ_ENCODING_HT)`；`intsetMaxEntries` 取 `server.set_max_intset_entries`，并额外钳到 `1<<30`，注释写「limit to 1G entries due to intset internals」。
- **装不进整数的成员直接换编码。** `src/t_set.c` 的 `isRawAdd` 在字符串形式无法转成整数时返回 -1，并把目标编码写进 `target_enc`（由 `isResolveEncodingForAdd` 决定是 LISTPACK 还是 HT）；`setTypeAddAuxInline` 收到 -1 后调 `setTypeConvertAndExpand(set, target_enc, ops->size(set) + 1, 1)`，转换后重试。
- **整数集合不能由通用转换接口产生。** `src/t_set.c` 的 `setTypeConvertAndExpand` 只接受 `OBJ_ENCODING_HT` / `OBJ_ENCODING_LISTPACK` / `OBJ_ENCODING_LISTPACK_EX`，其它目标 `serverPanic("Unsupported set conversion")`；`setTypeOpsIntset` 表项里的 `.convertFrom` 注释写明「intentionally left NULL: setTypeConvertAndExpand() never targets intset (see maybeConvertToIntset())」。真正把集合转成整数集合的是 `maybeConvertToIntset`，它的前置条件是「已知所有成员都是整数」——`src/t_set.c` 的集合运算落盘路径上就有 `if (only_integers && !setTypeHasExpireSupport(dstset)) maybeConvertToIntset(dstset);` 这样一处调用。

**初值的选定。** `src/t_set.c` 的 `setTypeCreate` 是工厂方法：值能表示为整数且 `size_hint <= server.set_max_intset_entries` 时返回整数集合；否则 `size_hint <= server.set_max_listpack_entries` 时返回 listpack；再否则返回哈希表（并按 `size_hint` 预扩容）。

**持久化路径上的升级。** `src/rdb.c` 载入 `RDB_TYPE_SET_INTSET` 时，若 `intsetLen(o->ptr) > server.set_max_intset_entries` 就 `setTypeConvert(o, OBJ_ENCODING_HT)`——旧快照里的整数集合在载入时同样可能被就地换成哈希表。

## 关键取舍

**源码事实：** 整数集合所有元素共用一种宽度，宽度有 2 / 4 / 8 三档且编码有序。**源码事实：** 元素按升序存放，查找是二分。**源码事实：** 升级时先换编码再扩内存，且从后往前搬迁。**源码事实：** 宽度只升不降。**源码事实：** 转换阈值 `set-max-intset-entries` 默认 512、`set-max-listpack-entries` 默认 128，都是运行时可配置项。**源码事实：** 整数集合只能由 `maybeConvertToIntset` 产生，通用转换接口明确拒绝把它当目标。

**架构推论：** 单一宽度换来的是「按下标直接寻址」与「O(log n) 二分」，代价是只要有一个大整数，整张表就得按最宽的元素付钱——100 个小整数加 1 个 64 位值，内存比全用 16 位宽翻了一倍还多；这正是 Redis 给整数集合设条目数上限、并在超限时整体换编码的原因。**架构推论：** 宽度只升不降，省掉了「降宽要重新排序或搬迁」的复杂度，代价是短暂插入一个大整数会让整表长期停在宽状态。**架构推论：** 「先扩容、再从后往前搬迁」把升级的搬迁压成一次自尾向头的顺序遍历，代价是升级不是原地完成的，中间需要一份新内存。**架构推论：** 把「装不下」（宽度）与「太多元素」（条数）拆成两个正交判据，比单一阈值更贴近真实内存画像——16 位整数哪怕几百个也很便宜，而 64 位整数哪怕几十个也不划算。**架构推论：** 把「反向转换」单独留一个入口（`maybeConvertToIntset`）而不是塞进通用转换接口，是因为「所有成员都是整数」这个前提只有调用方自己知道；主核若做多表示的列，「可否降级」的判定权必须交给掌握语义的那一层。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/intset.c` | `intsetAdd` | 往整数集合插入元素：宽度不够就升级，同宽就二分定位后插入 |
| `src/intset.c` | `intsetSearch` | 在升序整数集合里二分查找，并同时给出应插入的位置 |
| `src/t_set.c` | `setTypeMaybeConvert` | 依据 size_hint 与配置阈值，判断集合是否要从紧凑编码换成哈希表 |

## 参考

- [Redis 源码：intset.c](https://github.com/redis/redis/blob/unstable/src/intset.c)
- [Redis 源码：t_set.c](https://github.com/redis/redis/blob/unstable/src/t_set.c)
- [Redis 文档：SET 命令](https://redis.io/docs/latest/commands/set/)
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/)

## 与主核 / AI 的关系

整数集合这套「按取值范围选宽度、装不下就整表升级」的做法，对主核的直接参考价值在**向量与量化列**：一个 float32 向量列里若混进几个 float64 或高精度向量，同样会出现「一个元素拖垮整列」的问题，主核可以用同一套判据——按列统计最大宽度，超阈值就整列升级或改存 blob。另一处可借鉴的是**「装不下」与「太多元素」分成两个判据**：主核的稀疏向量列同样要分别回答「精度够不够」和「条目太多不多」两个问题，用一个阈值同时管会误伤。不可照搬的是**宽度只升不降**这条：主核的列是后台线程批量重写的，压缩线程完全可以在重写时顺手降宽，把「不降级」这条省事规则换成「重写时重新评估」。AI 侧若用 Redis 存用户画像里的 ID 集合或标签集合，整数集合能让纯数字标签省下每成员一份 sds 的开销，但一旦掺入非数字成员就会触发 `isRawAdd` 那条「装不下就整表换编码」的路径，内存占用会从「几个字节一个成员」跳到哈希表量级，这个跳变必须在容量规划时提前算进去。
