---
project: lightrag
feature: graph-name-rules
title: 图谱名称归一与拒绝规则
page_type: feature
schema_version: 1
refs:
  - path: lightrag/utils_graph.py
    symbols: [resolve_entity_name_for_lookup, _normalize_manual_entity_name, _require_stored_entity_name]
  - path: lightrag/utils_graph.py
    symbols: [_reject_self_loop_relation, _require_non_empty_description, _manual_entity_lock_keys]
---

# 图谱名称归一与拒绝规则

## 一句话职责

把「用户写的实体名」映射成「图谱里那个节点」，而这条路有三条不同规则：读路径可以宽松回退（`resolve_entity_name_for_lookup`），写路径必须在锁内解析成唯一键（`_resolve_manual_entity_name`），**删除类操作则拒绝任何回退**（`_require_stored_entity_name`）——最后一条是因为「重试」会让宽松解析变成删错对象。

## 机制是什么

名称归一化的核心冲突是：**归一化是有损的，而节点身份必须稳定**。

`normalize_entity_name` 会把「Apple 苹果」变成「Apple苹果」、把中文括号换成英文、去引号、去中文之间的空格。所以**多个不同的输入拼写会映射到同一个节点**。这带来三个必须分别处理的问题：

1. **旧节点可能不是归一化后的键**。手工创建实体的能力是后加的，早期节点用原始拼写存。若现在只用归一化键查，那些节点就**查不到**了。
2. **归一化后的键可能为空**。一个只含空格的输入归一化后是 `""`，而 `""` 不能当节点键。
3. **删除类操作不能重试**。这是最微妙的一条。

第三条需要展开。设想「把实体 A 合并掉」这个操作，用户写的是 `"Apple 苹果"`（原始拼写），而归一化后是 `"Apple苹果"`。若两个节点都存在（历史遗留），解析规则是「原始拼写优先」——第一次请求删掉了 `"Apple 苹果"`。用户重试同一个请求时，原始拼写那个节点已经没了，于是解析**回退到归一化键** `"Apple苹果"`，删掉了**另一个实体**。

**所以「可重试」这个性质与「宽松解析」直接冲突。** 而删除类操作的恢复方式恰恰是重试。

源码把这条写成了契约（`normalize_extracted_info` 的文档）：

> A change to what a single-line value normalizes to may only **MERGE** names that used to differ, never **SPLIT** names that used to be equal: rebuilding an entity after a document deletion re-parses the cached LLM output with the current rules and matches it by the stored name, so a split drops the facts of the records that no longer match.

这句话解释的是一条**跨版本的不变量**：删除文档后会「重建实体」——拿缓存里的 LLM 输出用**当前规则**重新解析，再按**已存的名称**去匹配。所以：
- 若规则改动让两个原本不同的名字变成相同（MERGE）→ 匹配仍然成功（更宽松）。
- 若规则改动让两个原本相同的名字变成不同（SPLIT）→ 部分记录匹配不上 → **它们的事实被丢掉**。

## 怎么实现的

### `_normalize_manual_entity_name`：唯一的入口类型检查

```python
def _normalize_manual_entity_name(entity_name: Any) -> str:
    """Apply the extraction naming contract to a manual entity identifier."""
    if not isinstance(entity_name, str):
        raise _InvalidEntityNameError("Entity name must be a string")
    return normalize_entity_name(entity_name)
```

两处：

1. **`isinstance` 检查 + `_InvalidEntityNameError`** —— 而**只在这里**。因为 `normalize_entity_name` 期待 `str`，而手工 API 的参数来自 HTTP/JSON，可能是数字或 `None`。
2. **`_InvalidEntityNameError` 而不是通用 `ValueError`** —— 让上层能把「参数错（400）」与「存储失败（500）」分开。前面那个 `_rejected_deletion` 的文档说明了这个区分：「tell this caller error (400) apart from a storage failure (500)」。

**注意它没做长度上限**。而 `normalize_entity_name` 的文档提到：

> A caller that caps the identifier's length must pass `final_space_pass=False` and run `remove_spaces_next_to_chinese` on the capped result, as the extraction parsers in `lightrag.operate` do.

**所以手工路径缺少这一步** —— 它用默认的 `final_space_pass=True`。若上层对名字做了截断，就会出现「截断前的归一化」与「截断后的归一化」不一致的问题（见下）。

### `_manual_entity_lock_keys`：锁两个拼写

```python
def _manual_entity_lock_keys(*spellings: tuple[str, str]) -> list[str]:
    """Keyed-lock keys covering every candidate a manual entity name can resolve to.

    Each argument is a ``(requested, normalized)`` pair. Both spellings are
    locked because :func:`_resolve_manual_entity_name` may pick either, and
    the doc-ingest pipeline locks the canonical names in the same
    ``GraphDB`` namespace. An empty normalized name is never a node, so it is
    not locked.
    """
    keys = set()
    for requested, normalized in spellings:
        keys.add(requested)
        if normalized:
            keys.add(normalized)
    return sorted(keys)
```

四处：

1. **两个拼写都要锁** —— 因为解析**可能选任一个**。若只锁归一化键，那么「原始拼写节点」可能被并发改名/删除，而解析已经选了它（解析在锁内，所以顺序是「先锁再解析」，两个都要锁住才能保证「解析结果在操作期间不变」）。

2. **`sorted(keys)`** —— 关键。`get_storage_keyed_lock` 拿一个列表，若两个并发的调用传入的顺序不同（`[A,B]` 与 `[B,A]`），就可能死锁。**排序消除了这个问题**。

   这与 d02 里边锁用 `sorted([edge_key[0], edge_key[1]])` 是同一手法。

3. **`if normalized` 才锁** —— 空归一化名不是节点，锁它无意义（反而会与所有「空名字」的调用者竞争同一把假锁）。

4. **`*spellings` 支持多个对** —— 因为有些操作涉及两个实体（如「合并 A 到 B」），两个名字都要锁。

   **锁多个实体名存在死锁风险**，而 `sorted` + 一次获取整个列表解决了它（原子地拿所有锁，不是逐个拿）。

### `_resolve_manual_entity_name`：写路径的解析规则

```python
async def _resolve_manual_entity_name(chunk_entity_relation_graph, requested_name, normalized_name, *, role="Entity") -> str:
    """Resolve a caller's entity spelling to the node identifier it addresses.

    The rule every manual graph entry point shares: an exact key that differs from
    its normalized form wins when such a node exists, so a node stored before manual
    names were normalized stays reachable; otherwise the extraction-normalized
    identifier that ``acreate_entity`` stores.
    """
    if requested_name != normalized_name and await chunk_entity_relation_graph.has_node(requested_name):
        return requested_name
    if normalized_name:
        return normalized_name
    raise _InvalidEntityNameError(f"{role} name cannot be empty after normalization")
```

四处：

1. **规则是「原始拼写优先，但只在它确实是节点时」** —— 三个条件的与：
   - `requested_name != normalized_name`（有差异才需要判断；相同时跳过一次查询）
   - `has_node(requested_name)`（它真的存在）
   - 满足则返回原始拼写

2. **否则返回归一化键** —— **即使那个节点不存在**。所以这个函数不保证「返回的键一定是已有节点」。调用方的后续操作（`upsert_node`）会创建它。

   **这是一处刻意的宽松**：手工写路径的行为是「upsert」，所以「解析到一个不存在的键」是正确的（它将被创建）。

3. **归一化后为空且无原始节点 → 抛错**。注意条件是「`normalized_name` 为空」——此时若原始拼写恰好是节点（如一个名字就叫 `"  "` 的节点），第一个分支会返回它。所以「空归一化名」只在「原始拼写也不是节点」时才是错误。

   **这是一处边界上的精确性**：一个纯空白名字的节点可以被访问（若它存在），但不能被创建。

4. **文档强调「Call it under a keyed lock taken with `_manual_entity_lock_keys` for the same pair」** —— 因为它在锁内做了「查询 + 决策」，若不在锁内，查询到的节点可能在决策后被改。

   **这是「check-then-act 必须在锁内」的标准形态**，而锁的键必须覆盖所有可能的决策结果。

**而 `resolve_entity_name_for_lookup` 是读路径的包装。**

```python
async def resolve_entity_name_for_lookup(chunk_entity_relation_graph, entity_name: str) -> str:
    """Resolve an entity name for a read, the way the manual write paths do.

    Lets a lookup find the node a caller created through ``acreate_entity`` with the
    spelling that call accepted. Takes no lock, so the answer can be stale by the
    time the caller acts on it; write paths must use _resolve_manual_entity_name
    under their keyed lock instead. A lookup answers for a name rather than refusing
    it: a non-string, or a name that normalizes to nothing, is returned unchanged
    and simply finds nothing.
    """
    if not isinstance(entity_name, str):
        return entity_name
    normalized_name = normalize_entity_name(entity_name)
    if not normalized_name:
        return entity_name
    return await _resolve_manual_entity_name(chunk_entity_relation_graph, entity_name, normalized_name)
```

四处：

1. **非字符串原样返回**（`return entity_name`）—— 而参数类型标注是 `str`，所以这是在处理类型标注与实际不符的情形。注释：「A lookup answers for a name rather than refusing it」。

   **即：读路径不抛错，只返回可能查不到的东西。** 这与写路径的 `_normalize_manual_entity_name` 抛 `_InvalidEntityNameError` 相反。

2. **归一化后为空也原样返回** —— 所以 `"   "` 会被当作节点名去查（而它多半不存在）。所以「返回一个不存在的名字」而不是报错。

3. **不加锁** —— 注释明确：「Takes no lock, so the answer can be stale by the time the caller acts on it; write paths must use `_resolve_manual_entity_name` under their keyed lock instead」。

   **即：读路径接受陈旧，写路径必须一致。** 这个区分是对的——读操作没有「check-then-act」问题。

4. **它复用 `_resolve_manual_entity_name`** —— 所以读与写的**解析规则完全一致**，只差锁与错误处理。这是关键的：若读路径自己实现一套规则，就会出现「写进去的节点读不到」。

### `_require_stored_entity_name`：删除类操作必须是存储名

```python
async def _require_stored_entity_name(chunk_entity_relation_graph, requested_name, normalized_name, *, role, operation) -> None:
    """Refuse a node-removing operation unless ``requested_name`` is stored.

    Merging an entity away, renaming it and deleting it remove the node they address,
    and their documented recovery is to retry the same request. A resolved name would
    make that retry unsafe: once the first attempt removed the exact node, the same
    spelling resolves to its normalized form, which can be a DIFFERENT live entity.
    Use this for the removed side of such an operation instead of _resolve_manual_entity_name.
    """
    if await chunk_entity_relation_graph.has_node(requested_name):
        return
    if (normalized_name and normalized_name != requested_name
            and await chunk_entity_relation_graph.has_node(normalized_name)):
        raise ValueError(f"{role} '{requested_name}' does not exist; it is stored as '{normalized_name}'. {operation} it by that name.")
    raise ValueError(f"{role} '{requested_name}' does not exist")
```

五处：

1. **它不返回解析结果** —— 返回 `None`，只做「通过或拒绝」。所以调用方必须用**原始的 `requested_name`** 操作。

   **这是整段的关键**：不给出解析结果，就无法「解析到别的节点」。

2. **第一条 `has_node(requested_name)` 通过** —— 原始拼写是节点，可以操作。

3. **否则不解析、直接拒** —— 但错误消息分两种：
   - 若归一化后是另一个存在节点 → 「it is stored as 'X'. `{operation}` it by that name.」（**给出正确的拼写**）
   - 否则 → 「does not exist」

4. **`{operation}` 是调用方传的动词**（`"Merge"`/`"Rename"`/`"Delete"`）。所以消息形如「it is stored as 'Apple苹果'. Delete it by that name.」

   **这是「拒绝时给出修法」的做法**：不只说「不存在」，还告诉用户「它其实叫这个，用这个名字重试」。

5. **为什么不能解析**（文档那段）：删除类操作的可重试性。**第一次删掉了原始拼写节点，重试时解析会回退到归一化键——那是另一个活着的实体。**

   **所以回退语义与幂等性是互斥的。** 而删除必须幂等（不能「重试一次多删一个」），所以这里禁止回退。

**这三个函数（`_resolve_manual_entity_name` / `resolve_entity_name_for_lookup` / `_require_stored_entity_name`）构成了一张按「操作类型」分派的表：**

| 操作 | 函数 | 回退? | 锁? | 错误处理 |
|---|---|---|---|---|
| 读 | `resolve_entity_name_for_lookup` | 是 | 否 | 不抛，返回原名 |
| 写（创建/更新） | `_resolve_manual_entity_name` | 是 | 必须 | 空名抛错 |
| 删/合并/改名 | `_require_stored_entity_name` | **否** | 必须 | 抛错并给出存储名 |

**判据是「操作是否可重放」。** 可重放的操作（upsert）能容忍回退；不可重放的操作（删除）不能。

### 名称归一本体：`normalize_entity_name` 与 `final_space_pass`

**它是 `sanitize_and_normalize_extracted_text` 的薄封装。**

```python
def normalize_entity_name(input_text: str, *, final_space_pass: bool = True) -> str:
    """Normalize an entity identifier using the extraction naming contract."""
    return sanitize_and_normalize_extracted_text(input_text, remove_inner_quotes=True, final_space_pass=final_space_pass)
```

**注意它硬编码 `remove_inner_quotes=True`** —— 所以实体名总是去掉内部引号。这是「名字」与「描述」的区别（描述不适用）。

而 `final_space_pass` 的语义是**顺序问题**，注释说得很精确：

> `final_space_pass`: ``False`` leaves out the last rule above (the second run of the space rules). A caller that cuts the value to a length limit passes ``False`` and runs `remove_spaces_next_to_chinese` on the cut value: the pass **shortens** a value, so running it before the cut lets two spellings that used to be cut to the same key be cut to **different** ones.

**这里的推理链值得完整复述**：

1. `remove_spaces_next_to_chinese` 会**缩短**字符串（删掉空格）。
2. 若先做这一步再截断到 N 字符：两个原本「截断后相同」的拼写，可能因为一个被缩短了而**截断后不同**。
3. 所以要**先截断、再做最后一遍空格处理**（`final_space_pass=False` + 截断后自己调）。

**而这直接关系到那条「只能 MERGE 不能 SPLIT」的不变量**：若顺序错了，同一批数据在不同版本下会被切成不同的键 → SPLIT → 事实丢失。

实现里那一步是条件执行的：

```python
if final_space_pass and removed_inner_quotes and not multiline:
    name = remove_spaces_next_to_chinese(name)
```

三处：

1. **`final_space_pass` 关闭时不做**（供截断调用方）。
2. **`removed_inner_quotes` 为假时也跳过** —— 注释：「either keep the characters on both sides of a space, or (the outer quotes) leave it at an edge for the strip. So the pass is skipped when no inner quote was removed, which gives the same result」。

   **即：这一步只在「刚去掉了内部引号」时才可能改变结果。** 所以跳过是等价优化。

3. **`not multiline`** —— 描述（多行）不走这条规则（描述里的换行有意义）。

**这三重条件是一处「精确的惰性」**：它保证「不必要时绝不动字符串」，从而让「不改变拼写」成为默认行为。

### 拒绝规则：自环与空描述

**`_reject_self_loop_relation` 的文档把「为什么」与「其他入口在哪」都写全了。**

```python
def _reject_self_loop_relation(source_entity, target_entity, *, operation) -> None:
    """Refuse a relation whose two endpoints are the same entity.

    One of the ingresses enforcing a STORE-level invariant: a graph must not contain
    a self-loop (see ``BaseGraphStorage.node_degree``, which is why degree is not
    defined on one). A self-loop carries no connectivity for graph retrieval -- it
    only widens the surface where the rest of the code has to reason about ``src == tgt``.
    """
    if source_entity == target_entity:
        raise ValueError(...)
```

三处：

1. **不变量的理由是「度数在自环上没有定义」** —— 一个自环算 1 度还是 2 度？没有公认答案，而图检索的度数计算（d02 的 `node_degree`）必须有个确定值。所以干脆禁止。

2. **「it only widens the surface where the rest of the code has to reason about `src == tgt`」** —— 即自环不只是无价值，而是**有维护成本**。

3. **比较用 `==` 而不是 `is`** —— 因为两个参数是字符串（同内容不同对象）。而名字已经归一化过（文档说「`ainsert_custom_kg` refuses one after name normalization」），所以 `==` 能捕获「不同拼写但归一化后相同」的自环。

**`_require_non_empty_description` 有一处必须显式判 `None` 的坑。**

```python
def _require_non_empty_description(description, *, operation, object_type) -> None:
    if description is None or not str(description).strip():
        raise ValueError(f"{object_type.capitalize()} description cannot be empty for {operation} operation")
```

**`description is None or not str(description).strip()`** —— 两个条件：

1. **`is None` 单独判**：因为 `str(None)` 是 `"None"`（非空白），所以只写后半句会让 `None` 通过并被存成字符串 `"None"`。

   **这是一个非常具体的字符串化陷阱**：`str()` 几乎永不为空，所以「用 `str()` 后判空」对 `None` 完全失效。

2. **`str(description).strip()`** 兜住「纯空白字符串」与「数字 0 等转成 `"0"` 后非空」。

**为什么数字能通过？** 因为 `str(0).strip()` 是 `"0"`，非空。所以「描述是数字 0」会通过检查并被存成 `"0"`。这大概是有意的宽容（有些描述确实是数字）。

### 「只能 MERGE 不能 SPLIT」这条契约的完整含义

这是本页最重要的一条，值得单独展开。它出现在 `normalize_extracted_info` 的文档里：

> A change to what a single-line value normalizes to may only MERGE names that used to differ, never SPLIT names that used to be equal: rebuilding an entity after a document deletion re-parses the cached LLM output with the current rules and matches it by the stored name, so a split drops the facts of the records that no longer match.

**为什么会有「用当前规则重解析缓存」这件事？** 因为 d05 的图谱重建：删除一个文档后，需要把该文档贡献的实体/关系从图谱里摘出来。而图谱里存的是**合并后的**实体（一份 `description`、一个 `source_id` 列表），无法直接「减掉某个文档的贡献」。

所以重建的做法是：拿剩下的文档的**缓存抽取结果**（d05 的 `_get_cached_extraction_results`），用当前规则重新解析，重建受影响实体。

**而「按已存名称匹配」意味着：**

- 若新规则把「A」和「a」归一化成同一个 → 两者都会匹配到「a」那个节点 → 它们的贡献合并（**MERGE，安全**）。
- 若新规则把原本都归一化成「a」的「A」和「a」分开 → 「A」匹配不到任何已存节点 → **它的事实被丢掉**（**SPLIT，数据丢失**）。

**所以归一化规则的演化方向被约束成单向的：只能越来越宽松，不能越来越严格。**

这是一条很硬的架构约束，而它是**从「重建机制」反推出来的**——不是设计者一开始就想到的，而是重建机制的实现方式（按名匹配）决定了规则演化必须单调。

**它同时解释了 `final_space_pass` 那个顺序要求**：若顺序错，某些值会被切成不同的键——那就是 SPLIT。

## 关键取舍

**源码事实：** `_normalize_manual_entity_name` 是唯一做 `isinstance(str)` 检查的地方（抛 `_InvalidEntityNameError`，让上层区分 400 与 500）；它用默认 `final_space_pass=True`，**没做长度上限**，而 `normalize_entity_name` 的文档要求截断调用方必须传 `False` 并在截断后自行处理空格。

**源码事实：** `_manual_entity_lock_keys` 对每个 `(requested, normalized)` 对**两个拼写都上锁**（因为解析可能选任一），跳过空归一化名，并 `sorted()` ——排序避免「同一组键不同顺序」造成死锁。

**源码事实：** `_resolve_manual_entity_name` 的规则是「原始拼写是已存节点则用它，否则用归一化键」——**后者不要求节点存在**（因为写路径是 upsert）。归一化后为空且原始拼写也不是节点时抛错。必须在 `_manual_entity_lock_keys` 的锁内调用（check-then-act）。

**源码事实：** `resolve_entity_name_for_lookup` 是读路径：非字符串原样返回、归一化后为空也原样返回（**不抛错，只返回可能查不到的东西**）、**不加锁**（接受陈旧），但**复用 `_resolve_manual_entity_name`** 保证读与写解析规则一致。

**源码事实：** `_require_stored_entity_name` **不返回解析结果**（只做通过/拒绝），因此调用方只能按原始拼写操作；当原始拼写不是节点但归一化后是另一个节点时，错误消息**给出存储名与重试方式**。理由是删除类操作的可重试性——一旦第一次删掉了原始拼写节点，重试的宽松解析会指向**另一个活着的实体**。

**源码事实：** 三个函数构成按操作类型分派的表：读（回退 + 无锁 + 不抛）、写（回退 + 必须锁 + 空名抛）、删/合并/改名（**禁止回退** + 必须锁 + 抛并给出存储名）。判据是「操作是否可重放」。

**源码事实：** `normalize_entity_name` 硬编码 `remove_inner_quotes=True`（名字与描述的区别），`final_space_pass` 控制最后一遍空格处理。

**源码事实：** `final_space_pass=False` 的必要性来自顺序：`remove_spaces_next_to_chinese` 会**缩短**字符串，若在截断之前做，两个原本「截断后相同」的拼写会被切成**不同**的键 → SPLIT。实现里这一步有三重条件（`final_space_pass and removed_inner_quotes and not multiline`），其中 `removed_inner_quotes` 是等价优化（只有刚去掉内部引号时才可能改变结果）。

**源码事实：** 归一化规则演化受一条硬约束：**只能 MERGE，不能 SPLIT**。理由是删除文档后的重建拿缓存 LLM 输出用**当前规则**重解析、再按**已存名称**匹配——SPLIT 会让部分记录匹配不上，其事实被丢弃。这是从「重建机制按名匹配」反推出的架构约束。

**源码事实：** 自环被定义为存储级不变量，理由是 `node_degree` 在自环上没有定义（1 还是 2），且自环会「widen the surface where the rest of the code has to reason about `src == tgt`」；比较用 `==`（字符串同内容不同对象），且因名字已归一化，能捕获「不同拼写但归一化后相同」的自环。

**源码事实：** `_require_non_empty_description` **必须显式判 `description is None`**，因为 `str(None).strip()` 是 `"None"`（非空），只写后半句会让 `None` 通过并存成字符串 `"None"`；而数字（如 `0`）会通过并被存成 `"0"`（有意的宽容）。

**架构推论：** 这一页的核心是「**同一个名称有三个解析严格度，按操作的可重放性选择**」。这不只是接口设计，而是由幂等性需求反推出来的：upsert 天然幂等（重复执行结果相同），所以能容忍「有时命中 A、有时命中 B」；而 delete 不幂等（重复执行会多删），所以必须锁死到「用户写的那个拼写」。**任何「宽松解析 + 可重试」的组合都藏着这个 bug**，而它只在「两个相似对象都存在」时触发——所以测试很难覆盖。

**架构推论：** 「只能 MERGE 不能 SPLIT」是一条**由存储格式反推的演化约束**。它的根源不是名称归一化本身，而是「重建按名匹配」这个实现选择。若重建改成按稳定 id 匹配，这条约束就不需要了。**判据是「匹配依据是否稳定」**：按名称匹配（会变）就必须约束归一化演化；按 id 匹配（不变）就可以自由改规则。这是一个很好的例子：`d05` 的实现方式给 `d04` 加了一条隐含约束，而两边都不知道对方。

**架构推论：** `final_space_pass` 那个顺序问题值得当成通用模式记：**任何「先归一化再截断」的处理都有这个坑**，因为归一化可能改变长度。正确顺序永远是「截断 → 归一化」或者「归一化时不做那些会变短的操作，截断后再补」。前者要求归一化不依赖上下文（这里不成立，因为它需要看两侧字符），所以只能选后者——而它要求把归一化拆成两步。

**架构推论：** 自环检查在五个入口重复，而文档把五处都列了出来。这与 d03 里 `_reject_self_loop_relation` 同属一类做法：**存储级不变量必须在每个写入口校验，而不是只在存储层**。因为存储层拒绝时可能已经产生了部分写入，且错误消息离用户远。代价是「新增入口时必须记得加检查」——而把入口清单写进 docstring 是唯一的防漏手段。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/utils_graph.py` | `_normalize_manual_entity_name` | 手工路径的类型检查 + 归一化 |
| `lightrag/utils_graph.py` | `_manual_entity_lock_keys` | 两个拼写都上锁，`sorted` 防死锁 |
| `lightrag/utils_graph.py` | `_resolve_manual_entity_name` | 写路径解析：原始拼写优先，否则归一化键 |
| `lightrag/utils_graph.py` | `resolve_entity_name_for_lookup` | 读路径：不抛、不加锁、返回原名 |
| `lightrag/utils_graph.py` | `_require_stored_entity_name` | 删除类操作：**禁止回退**，只做通过/拒绝 |
| `lightrag/utils_graph.py` | `_reject_self_loop_relation` | 自环拒绝 + 五处入口清单 |
| `lightrag/utils_graph.py` | `_require_non_empty_description` | 空描述拒绝（须显式判 `None`） |
| `lightrag/utils_graph.py` | `_rejected_deletion` | 参数错 → 400 而非 500 |
| `lightrag/utils.py` | `normalize_entity_name` | 归一化入口（硬编码去内部引号） |
| `lightrag/utils.py` | `normalize_extracted_info` | 归一化本体与「只能 MERGE 不能 SPLIT」契约 |
| `lightrag/utils.py` | `remove_spaces_next_to_chinese` | 缩短字符串的最后一遍空格处理 |
| `lightrag/utils.py` | `sanitize_and_normalize_extracted_text` | 归一化的实际实现 |

## 参考

- [lightrag 源码：utils_graph.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/utils_graph.py)
- [lightrag 设计文档：清理恢复契约](https://github.com/HKUDS/LightRAG/blob/main/docs/design/PurgeRecoveryContract.md)
- [Unicode 标准：规范化（NFC/NFKC）与等价性](https://www.unicode.org/reports/tr15/)
- [论文：实体消解与名称归一化](https://aclanthology.org/2020.emnlp-main.510/)

## 与主核 / AI 的关系

「名称归一化只能合不能分」与主核（多模态数据库内核）的**标识符折叠**规则是同一类约束。主核把未加引号的标识符折叠成小写（`SELECT Foo` 与 `select foo` 是同一列），这个折叠是**不可逆**的——但因为它只做「合」（`Foo`/`FOO`/`foo` 都变 `foo`），从不「分」，所以是安全的。SQL 标准明确禁止「某些标识符折叠、某些不折叠」这种不一致，理由与这里完全相同：**折叠规则一旦可能分开原本相同的名字，已有引用就会失效。**

第二处对照是删除类操作「必须用存储名」与主核的 **`DROP TABLE` 不使用搜索路径回退**。主核的 `DROP TABLE foo` 会在 `search_path` 里找 `foo`，找到就删——这与这里的宽松解析同类。但主核的 `DROP` 是幂等改进方向上的（`IF EXISTS` 才有幂等），无 `IF EXISTS` 时不存在就报错。**关键是：数据库从不「找到一个相似的就删」**——而这里的 `_require_stored_entity_name` 正是把这个纪律加回来。

第三处是 `_manual_entity_lock_keys` 的 `sorted` 与主核的**多表锁按 OID 排序加锁**。两者都是「需要同时锁多个对象时，按固定顺序获取以避免死锁」。主核在 `LockRelationOid` 之前会把待锁对象排序；这里把键排序。**这是多对象加锁的唯一正确做法**，而它的必要性在两边都是「两个并发事务以不同顺序请求同一组锁」。

对 AI 工程的实际意义在于：实体名称是图谱的主键，而用户会用手写的方式碰它。这份实现给出的三条经验可直接用：**归一化规则只能合不能分**（否则重建/删除会让部分事实失联）、**「宽松解析 + 可重试」是危险的组合**（删除类操作必须锁死到原拼写）、**先截断再归一化**（否则两拼写会截成不同键）。
