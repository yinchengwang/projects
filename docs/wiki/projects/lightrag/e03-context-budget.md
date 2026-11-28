---
project: lightrag
feature: context-budget
title: 上下文预算与截断
page_type: feature
schema_version: 1
refs:
  - path: lightrag/operate.py
    symbols: [_apply_token_truncation, _fit_kg_context_to_total_budget, _vector_chunk_quota]
  - path: lightrag/operate.py
    symbols: [_truncate_entity_identifier, _truncate_section_context, _truncate_vdb_content]
---

# 上下文预算与截断

## 一句话职责

让三类来源（实体描述、关系描述、文本块）**抢同一份 token 预算**并给出一个确定性的分配结果：先按各自的子预算做一次粗筛，再用「精确重渲染 + 二分搜索」把整体压进 `max_total_tokens`；顺带在三个不同的边界上（标识符长度、面包屑长度、嵌入上限）做与预算无关的独立钳制。

## 机制是什么

**这里有三种性质完全不同的「截断」，混在一起会造成理解困难。它们应该分开记：**

| 机制 | 约束来自 | 触发条件 | 失败方式 |
|---|---|---|---|
| **总预算拟合** | `max_total_tokens`（提示的总容量） | 渲染后的提示超限 | 返回 `None`（不可用） |
| **子预算粗筛** | `max_entity_tokens` / `max_relation_tokens` | 单类记录堆太长 | 丢弃尾部记录 |
| **存储边界钳制** | 后端字段长度、嵌入模型上限 | 单条记录本身超限 | 截断该条的内容 |

第一类是「装不下就少装」；第二类是「先按类分量」；第三类是「单条太大，先把它弄小」。

**而这三类截断的目标互不相同**：总预算保护的是模型调用能成功；子预算保护的是两类记录不互相挤占；存储钳制保护的是写入与嵌入不报错。后两者其实是「为了让总预算这一步有东西可算」。

## 怎么实现的

### 一、`_apply_token_truncation`：一条被写进文档的排序契约

**这个函数的返回值里有两组列表，而它们的关系是整个下游正确性的基础。**

```
Returns ``entities_context`` / ``relations_context`` (the records handed to
the prompt) plus ``filtered_entities`` / ``filtered_relations`` (the matching
original records handed to chunk selection).
```

**`*_context` 是喂给提示的简化记录，`filtered_*` 是交给块选择的原始记录。** 两者条数相同、顺序必须相同——而这条约束被写成了一段很长的注释：

> 任何在这里加入的选择器的排序规则：`*_context` 列表是本阶段的输出，也是下游能看到的唯一重要性排名。`filtered_*` 列表**必须**遵循同一顺序——第 3 阶段把共享块归因给位置更早的记录，并按列表位置分配块配额——所以一个会重排的选择器不得让 `filtered_*` 留在第 1 阶段的检索顺序里。

**这就解释了 `filtered_*` 的重建为什么要走 `*_context` 而不是过滤原始列表：**

```
# Walk the *_context lists (this stage's own output order) and resolve each
# record through the pre-truncation maps. ... Do NOT rebuild these by filtering
# final_entities / final_relations -- that reimposes stage-1 retrieval order and
# silently discards any reordering a stage-2 selector (e.g. a reranker) performed.
for entity_context in entities_context:
    name = entity_context.get("entity")
    if name in filtered_entity_id_to_original: continue
    original = entity_id_to_original.get(name)
    if original is None: unresolved_entities.append(name); continue
    filtered_entities.append(original)
    filtered_entity_id_to_original[name] = original
```

**为什么不直接过滤 `final_entities`？** 因为若将来有人在这里插入一个重排器（比如精排模型），按原始列表过滤会把重排的结果全部丢掉——而**这个错误是静默的**（条数一样、只是顺序回退了）。**注释提前把这条陷阱标出来，是在保护一个将来才会写下的选择器。**

**第二个契约是「可以重排、可以丢弃，不可以发明」。**

```
if original is None:
    unresolved_entities.append(name)
    continue
...
logger.warning(f"Dropping {len(unresolved_entities)} entity records absent from the pre-truncation map: {unresolved_entities[:5]}")
```

**一个凭空出现或改了名的实体能进提示，但进不了块选择。** 于是它被丢掉并报告——而不是静默消失。这与「伪造的实体名会让块选择找不到对应块」是同一件事的两面：**能拿出来但不该拿出来的数据，要显式丢掉并留痕。**

**去重发生在重建阶段，而 `setdefault` 保证了「首次出现的那份胜出」。**

```
entity_id_to_original.setdefault(entity_name, entity)
...
# First occurrence wins: the filtered rebuild below resolves records
# through this map, and duplicate names must keep the record that was
# retrieved first. Accepted divergence if a name ever repeats: the
# prompt shows the context row a reordering selector picked, while
# filtered_* carries the first-retrieved original. Unreachable today --
# stage 1 already dedups entities by name and relations by sorted pair.
```

**注释承认了一处「已知的不一致」并说明它今天不可达** —— 若同名出现两次，提示里显示的是选择器挑的那一行，而 `filtered_*` 带的是首次检索到的原始记录。**把不可达的分歧写下来，比假装它不存在好**：将来某处改变了去重策略，读这段的人立刻知道会有什么后果。

### 二、粗筛：算 token 时先摘掉不参与的内容

**实体与关系在算 token 之前都被复制并摘掉两个字段。**

```
entities_context_for_truncation = []
for entity in entities_context:
    entity_copy = entity.copy()
    entity_copy.pop("file_path", None)
    entity_copy.pop("created_at", None)
    entities_context_for_truncation.append(entity_copy)

entities_context = await atruncate_list_by_token_size(
    entities_context_for_truncation,
    key=lambda x: json.dumps(x, ensure_ascii=False),
    separator="\n", max_token_size=max_entity_tokens, tokenizer=tokenizer,
)
```

**为什么摘掉 `file_path` 与 `created_at`？** 因为这两个字段**不在被预算保护的那段文本里**——它们是引用与元数据。若把它们算进去，一条记录的 token 成本会被高估，于是一次能装 10 条的地方只装 8 条。

**这是一个具体的「按什么计量」问题**：预算的目的是「模型读到的内容不超限」，所以只应该计模型读到的部分。而 `file_path` 会以引用列表的形式单独出现（`_[1] path`），`created_at` 在 `kg_query_context` 模板里可能根本不渲染。

**代价是 `entities_context` 被替换成了「去掉两字段」的版本。** 注意 `entities_context = await atruncate_list_by_token_size(...)` 覆盖了原列表——于是返回给下游的 `entities_context` 里**没有 `file_path` 与 `created_at`**。而重建 `filtered_*` 时用的是 `entity_id_to_original`（原始记录），所以块选择那边仍有完整数据。

**这个不对称是有效的**：提示不需要 `file_path`（引用另给），块选择可能需要。

**关系标识的构造有一处归一。**

```
if "src_tgt" in relation:
    entity1, entity2 = relation["src_tgt"]
else:
    entity1, entity2 = relation.get("src_id"), relation.get("tgt_id")
```

**两种键名指向同一件事**，于是后续代码只需要处理一种形态。而 `_find_related_text_unit_from_relations` 里更进一步：

```
rel_key = tuple(sorted(relation["src_tgt"]))
...
rel_key = tuple(sorted([relation.get("src_id"), relation.get("tgt_id")]))
```

**关系键故意排序**——于是 `(A, B)` 与 `(B, A)` 是同一个键。这是必需的：无向图里同一条边可能从两个方向被检索到，不排序会产生两条重复记录。

**而 `_apply_token_truncation` 里的 `relation_key` 没有排序。**

```
relation_key = (entity1, entity2)
relation_id_to_original.setdefault(relation_key, relation)
```

**这是一个真实的不一致**：截断层按「有向」存键与重建，而块选择层按「无向」存键。当同一条边以两种方向同时出现在 `final_relations` 里时，截断层会当成两条（各占预算、`*_context` 里出现两行），而块选择层会合并成一条。

**注释说明了截断层依赖上游已去重**：`# Unreachable today -- stage 1 already dedups entities by name and relations by sorted pair.`。所以这条不一致今天不可达——但它依赖于「第 1 阶段按排序对去重」这个前提，而那个前提是**另一个函数**的局部行为。

### 三、`_fit_kg_context_to_total_budget`：精确拟合的核心

**这是整页最精巧的一段。它的做法是「真的把提示渲染出来数一遍」，而不是估算。**

**第一步：先探一次空上下文能不能装下。**

```
if not render([], [], [])[0]:
    logger.warning("max_total_tokens cannot fit the required query prompt")
    return None
```

**某次记录为空时仍装不下 → 整个请求无解。** 而返回 `None`（而不是空上下文的字符串）让调用方走「无结果」路径——这比给模型一个只有模板没有材料的提示好得多。

**第二步：把图记录按排名交错，但保持各自的前缀顺序。**

```
ranked = [
    (kind, records[index])
    for index in range(max(len(entities), len(relations)))
    for kind, records in (("entity", entities), ("relation", relations))
    if index < len(records)
]
```

**`for index ... for kind, records in (("entity", ...), ("relation", ...))`** 这个双层推导式产生的是 `e0, r0, e1, r1, ...` —— **按排名交错，每类内部保持原顺序**。

**为什么这样排而不是「先实体全部、再关系全部」？** 因为二分搜索取的是 `ranked[:count]` 这个**前缀**。交错排列让前缀天然是「两类各取前 k 条」，于是截断对两类公平。这与 `_perform_kg_search` 里实体轮转合并是同一个思路（见 e01）。

**第三步：三段式的二分搜索与两次独立延伸。**

```
if not render(entities, relations, [])[0]:
    all_entities, all_relations = entities, relations
    entities, relations, _, _, _ = fit_prefix(len(ranked), lambda count: render(*graph_prefix(count), []))
    # An oversized row in one source must not block the other source's
    # smaller records. Extend each remaining source prefix independently.
    entities, relations, _, _, _ = fit_prefix(len(all_entities), lambda count: render(all_entities[:count], relations, []))
    entities, relations, _, _, _ = fit_prefix(len(all_relations), lambda count: render(entities, all_relations[:count], []))
```

**三步的语义：**

1. **按交错排名整体缩** —— 得到一个两类都缩过的起点。
2. **再单独延伸实体** —— 从当前关系列表固定，尽量多装实体。
3. **再单独延伸关系** —— 从当前实体列表固定，尽量多装关系。

**注释给的理由是关键**：「一个来源里的一条超大记录不得阻挡另一来源里更小的记录」。

具体场景：关系记录平均很长（描述多），实体记录很短。交错缩放的二分会把关系砍到只剩 1 条，而那 1 条仍然很大——于是整体缩放停在很少的条数上，实体也被无辜砍掉。**独立延伸让「关系只能装 1 条」与「实体能装 20 条」共存。**

**而 `fit_prefix` 是一个手写的二分搜索，带一个「非单调」的后处理。**

```
def fit_prefix(count, candidate):
    low, high = 0, count
    while low < high:
        mid = (low + high + 1) // 2
        if candidate(mid)[0]: low = mid
        else: high = mid - 1
    fits, result = candidate(low)
    while not fits and low:
        low -= 1
        fits, result = candidate(low)
    return result
```

**注释说明了为什么要那个 `while`**：「搜索前缀，然后验证精确的幸存渲染。**tokenization 在记录或模板边界上不必严格单调**」。

**这是本页最值得记的一个技术判断。** 二分搜索要求谓词单调（「装得下 k 条」⇒「装得下 k-1 条」）。而 tokenization **不保证**这个性质：

- 两条记录拼在一起时的分隔符可能让 token 数**少于**各自之和（BPE 合并）。
- 模板边界上的字符也可能产生合并或分裂。

于是理论上有「装得下 5 条、装不下 4 条」的可能。二分结束后再用线性回退确认，**是承认这个理论缺口并堵上它**——代价是最坏情况 O(k) 次渲染，而实际几乎不会触发。

**而 `render` 内部用「切片」代替「逐条序列化」。**

```
entity_jsons = [json.dumps(record, ensure_ascii=False) for record in entities]
...
entities_str="\n".join(entity_jsons[: len(entity_records)])
```

注释解释了为什么合法：「`render()` 拼接这些列表的切片；它收到的每个图参数都是输入列表的前缀（`graph_prefix` 与各来源的延伸只做切片），所以记录数索引匹配的 JSON 切片，渲染出的提示与逐条序列化逐字节相同」。

**这是一个「先序列化一次、之后只切片」的优化**：二分搜索要渲染 O(log n) 次，若每次都重新序列化全部记录，成本是 O(n log n) 次 `json.dumps`。预先序列化把它降到 O(n)。

**代价是一个隐藏前提**：渲染函数只能放**前缀**。代码里确实如此（`graph_prefix` 只切片、延伸也只切片），但**这个前提是隐式的**——若将来有人传入一个非前缀的子集，渲染结果会与实际记录不符，而且不会报错。注释把它写下来了，但它是一个应该被断言的东西。

**最后一步是块：先试全装，不行再二分。**

```
fits, result = render(entities, relations, chunks)
if fits: return result
return fit_prefix(len(chunks), lambda count: render(entities, relations, chunks[:count]))
```

**注意此时实体与关系已经固定**——于是块只能吃「剩下的容量」。这是一个明确的优先级：**图上检索到的记录优先于文本块**。

理由在结构上：`mix` 模式里实体/关系来自精确的图谱查询（高置信），而块来自向量近似（低置信、易重复）。同时这也意味着 `naive` 模式（只有块）走的是完全不同的路径，不需要这个优先级。

**`count_tokens` 用 `token_memo` 做记忆化，而它的线程约束被写成了一条 INVARIANT。**

```
# INVARIANT: the memo dict is only read/written inside the single-worker
# tokenizer executor. All fitter invocations go through
# run_in_tokenizer_executor, which serializes access. Never touch the memo
# from event-loop code.
def count_tokens(text: str) -> int:
    if token_memo is not None:
        cached = token_memo.get(text)
        if cached is not None: return cached
    count = len(tokenizer.encode(text))
    if token_memo is not None: token_memo[text] = count
    return count
```

**为什么要有这条不变量：** 这个拟合器在一个**单工作线程**的 tokenizer 执行器里跑，所以 dict 不需要锁。而这个优惠是「因为它只在一个线程里」——一旦有人从事件循环里调用，dict 会有数据竞争，且 tokenizer 本身也可能不是线程安全的。

**用注释大写 INVARIANT 标出来，是在用一种「代码之外的约束」换一个锁的开销。** 这类优化的正确性完全依赖约定被遵守，所以它必须在最显眼的位置说明。

### 四、存储边界钳制：三个与预算无关的截断

**第一个是标识符的长度与字节双限。**

```
def _truncate_entity_identifier(identifier, limit, chunk_key, identifier_role, byte_limit=DEFAULT_ENTITY_NAME_MAX_BYTES):
    char_len = len(identifier)
    byte_len = len(identifier.encode("utf-8"))
    if char_len <= limit and byte_len <= byte_limit: return identifier
    display_value = identifier[:limit]
    encoded = display_value.encode("utf-8")
    if len(encoded) > byte_limit:
        display_value = encoded[:byte_limit].decode("utf-8", errors="ignore")
```

**文档给了一个精确的例子：「256 个中文字符 ≈ 694 字节 > 512」。** 而注释说明为什么两个限制都要：

> Milvus 校验 VARCHAR 的 `max_length` 用的是**字节**而不是字符，所以一个在字符限制内的 CJK 标识符仍然可能溢出字段。

**这是「字符数与字节数不是一回事」的经典坑** —— 而它的解（先按字符截、再按字节截、用 `errors="ignore"` 丢掉半个多字节字符）是标准做法。

**`errors="ignore"` 那一行值得单说**：`encoded[:byte_limit]` 可能切在一个多字节字符中间，直接 `decode` 会抛 `UnicodeDecodeError`。用 `ignore` 丢掉残片，保证输出是合法 UTF-8。

**第二个是面包屑的 token 预算。**

```
if len(tokenizer.encode(heading_path)) <= max_tokens: return heading_path
levels = heading_path.split(HEADING_BREADCRUMB_SEP)
if len(levels) >= 3:
    heading_path = f"{levels[0]}{HEADING_BREADCRUMB_SEP}…{HEADING_BREADCRUMB_SEP}{levels[-1]}"
```

**超预算时先折叠成「首级 … 末级」** —— 保留「这份文档在哪」与「这段在哪个小节」，丢掉中间层级。

**而折叠后仍可能超预算，所以有一个硬上限。**

```
tokens = tokenizer.encode(heading_path)
if len(tokens) > max_tokens:
    ellipsis = "…"
    ellipsis_token_count = len(tokenizer.encode(ellipsis))
    if ellipsis_token_count <= max_tokens:
        for keep in range(max_tokens - ellipsis_token_count, -1, -1):
            candidate = tokenizer.decode(tokens[:keep]).rstrip() + ellipsis
            if len(tokenizer.encode(candidate)) <= max_tokens: return candidate
    for keep in range(max_tokens, -1, -1):
        candidate = tokenizer.decode(tokens[:keep]).rstrip()
        if len(tokenizer.encode(candidate)) <= max_tokens: return candidate
    return ""
```

**为什么需要这个「回退循环」而不是直接 `decode(tokens[:max_tokens-1])`？** 注释说明了：

> 优先在装得下时用尾部省略号，但要**对每个候选重新编码**，因为自定义/BPE tokenizer 可能在把后缀附加到解码文本时以不同方式切分它。

**这是本页第二个「tokenization 不单调」的证据** —— 解码再编码不保证回到原来的 token 数。于是「取前 k 个 token」不能保证「结果 ≤ k 个 token」，必须实测。

**第三个是嵌入前的块内容钳制。**

```
threshold = int(embedding_token_limit)
if threshold <= 0: return content
try:
    span = tokenizer.truncate_by_token_limit(content, threshold)
except TokenBudgetError as e:
    logger.error(...); raise
if span.end == len(content): return content
truncated_content = content[span.start : span.end]
logger.warning(...)
```

**它用 `Tokenizer.truncate_by_token_limit` 的「安全契约」**，而文档说明了为什么不再需要旧的启发式余量：

> 用的是安全的 `Tokenizer.truncate_by_token_limit` 契约：结果会被独立重新编码并验证确实装得下 `threshold`，所以这里不再需要（也不再施加）一个固定的启发式安全余量——这里旧的 `decode(tokens[:k])` 没有这种验证，并且在所有 tokenizer/内容组合上都不保证能往返回到 ≤ `threshold` 个 token。

**这句话是全页的定性结论**：`decode(encode(x)[:k])` 这个看似显然的写法**在一般 tokenizer 上不成立**。

而 `TokenBudgetError` 的处理是**重新抛出**：

```
# Only possible if even the first Unicode code point can't fit the
# embedding model's own context limit -- not a real-world budget.
```

**只有「连一个码点都装不下」才抛** —— 那不是预算问题而是配置错误，所以让它冒出来。

### 五、`_vector_chunk_quota`：一段「为什么不能删掉这个 1」的文档

**这个函数只有两行，但它有一整页的文档字符串。**

```
if max_related_chunks <= 0 or group_count <= 0: return 0
return max(1, int(max_related_chunks * group_count / 2))
```

**文档逐条解释了每个部分的必要性：**

> **不要移除那个下限 1**：它恢复了与 WEIGHT 路径的对等——后者通过 `pick_by_weighted_polling(..., min_related_chunks=1)` 保证每组至少一块。这个配额与 WEIGHT 的线性梯度预算 `n * (max + 1) / 2` 是同一个，只差 `n / 2` 那一项，所以 `(max_related_chunks=1, group_count=1)` 是 VECTOR 会截断到 0 并低于那个下限的唯一输入。

**这段话做了三件事**：指出删掉 1 的后果、给出 WEIGHT 侧的对应机制、并**精确指出唯一会触发的输入组合**。

而后半段更关键：

> 配额为 0 会让 `pick_by_vector_similarity` 返回 `[]`，而**调用点把那个读作「向量选择失败」并静默降级到 WEIGHT**。

**这是「0 是一个有哨兵含义的值」带来的坑**：0 在这里不是「选 0 个」，而是「没得选」的信号。于是 `max_related_chunks=1` 这个合法配置会被当成失败。

**而两个守卫的语义也不同。**

> `max_related_chunks <= 0` 的守卫是让 `related_chunk_number=0` 成为一个**真正的关闭开关**而不是静默的 1；下限只在两个输入都为正时才施加。
>
> `group_count <= 0` 分支**纯属防御**：两个调用点都已在到达这里之前守卫了组列表非空。

**「0 是关闭开关」与「0 是失败信号」的矛盾，就是这整段文档存在的原因。** 同一个值在两种语境下含义相反，只能靠提前把边界算对来避免。

## 关键取舍

**源码事实：** `_apply_token_truncation` 返回两组列表（提示用的 `*_context` 与块选择用的 `filtered_*`），并规定**顺序必须一致**——下游按列表位置分配块配额，重排的选择器不得让 `filtered_*` 回到检索顺序。

**源码事实：** `filtered_*` 通过 `*_context` 反查原始记录重建（而不是过滤 `final_entities` / `final_relations`），因为按原始列表过滤会静默丢弃任何重排结果。反查失败（记录被发明或改名）时丢弃并警告。

**源码事实：** 同名实体的映射用 `setdefault`（首次出现胜出），注释记录了一处「已知但今天不可达」的分歧：提示显示选择器挑的行、`filtered_*` 带首次检索的原始记录。

**源码事实：** 粗略筛 token 时先复制并摘掉 `file_path` / `created_at`——它们不被预算保护（引用列表另给、时间戳可能不渲染），算进去会高估成本。于是返回的 `entities_context` 不含这两个字段。

**源码事实：** 关系在块选择层用 `tuple(sorted(...))`（无向键），而截断层用 `(entity1, entity2)`（有向键）——不一致，但今天不可达，因为它依赖第 1 阶段已按排序对去重。

**源码事实：** 总预算拟合采用「真实渲染 + 数 token」而非估算，先探空上下文能否装下（不能则整个请求返回 `None`）。

**源码事实：** 图记录按排名**交错**（`e0, r0, e1, r1, ...`）排列，于是取前缀对两类公平；缩放后还各自独立延伸一次，因为「一个来源的超大记录不得阻挡另一来源的小记录」。

**源码事实：** `fit_prefix` 是二分搜索 + 线性回退，注释说明原因是「tokenization 在记录或模板边界上不必严格单调」——这是对二分搜索单调性前提的显式补丁。

**源码事实：** 渲染用「预先序列化一次 + 之后只切片前缀」，把 O(n log n) 次 `json.dumps` 降到 O(n)。它依赖一个隐式前提（渲染函数只收前缀），该前提写在注释里但没有断言。

**源码事实：** `token_memo` 的读写被一条大写 INVARIANT 限制在单工作线程的 tokenizer 执行器内——用约定换掉锁。

**源码事实：** 标识符长度同时受字符数与 UTF-8 字节数限制，因为 Milvus 按字节校验 VARCHAR；字节截断后 `errors="ignore"` 丢掉半个多字节字符。

**源码事实：** 面包屑超预算时先折叠成「首级 … 末级」，若仍超则用回退循环逐个缩短并**重新编码验证**——因为「取前 k 个 token」不保证结果 ≤ k 个 token。

**源码事实：** 块内容嵌入上限用 `truncate_by_token_limit` 的「结果被独立重编码验证」契约，替代旧的 `decode(tokens[:k])` 启发式；`TokenBudgetError` 重新抛出，因为它只会在「连一个码点都装不下」时发生。

**源码事实：** `_vector_chunk_quota` 的下限 1 不可删：配额 0 会被调用点读作「向量选择失败」并静默降级到 WEIGHT，而 `(max=1, group=1)` 是唯一会算出 0 的合法输入。

**架构推论：** 这一页反复出现的主题是**「token 不是可以自由算术的量」**。二分搜索要用线性回退兜住非单调、面包屑要重新编码验证、嵌入截断要靠库提供的可验证契约、序列化切片要证明前缀等价——四处都在处理同一个事实：**字符、token、字节是三个不同的度量空间，它们之间的转换不保持任何简单的代数性质**。

**架构推论：** 预算分配的本质是**优先级声明**。交错排列 + 独立延伸 = 「两类都要有，但各自尽量多」；块放最后 = 「图谱记录优先于向量块」。这些优先级没有写在参数里（没有 `entity_priority` 这样的开关），而是写进了循环结构。**当优先级太多样时，参数化会让配置爆炸；此时把优先级编码进结构反而更清晰。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/operate.py` | `_apply_token_truncation` | 两组列表的产出与「顺序一致」契约 |
| `lightrag/operate.py` | `_fit_kg_context_to_total_budget` | 精确渲染拟合，交错排名 + 独立延伸 |
| `lightrag/operate.py` | `fit_prefix` | 二分搜索 + 线性回退（非单调兜底） |
| `lightrag/operate.py` | `render` / `graph_prefix` | 前缀渲染与预序列化切片 |
| `lightrag/operate.py` | `_vector_chunk_quota` | 向量配额的下限 1 与两种 0 的含义 |
| `lightrag/operate.py` | `_truncate_entity_identifier` | 字符 + UTF-8 字节双限 |
| `lightrag/operate.py` | `_truncate_section_context` | 面包屑折叠 + 重新编码验证的硬上限 |
| `lightrag/operate.py` | `_truncate_vdb_content` | 嵌入上限，依赖可验证的截断契约 |
| `lightrag/operate.py` | `token_memo` 不变量 | 用注释约定换掉锁 |

## 参考

- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)
- [tiktoken 项目：BPE 编码](https://github.com/openai/tiktoken)
- [Milvus 文档：VARCHAR 长度限制](https://milvus.io/docs/v2.4.x/schema.md)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

上下文预算与主核的**缓冲区管理**是同一类问题：一块固定容量的空间（buffer pool / context window）要被多个竞争者瓜分，而分配策略决定了系统的行为。主核用 LRU/时钟算法驱逐页，这里用交错前缀 + 独立延伸驱逐记录。

**差别在于「代价是否可估」。** 主核能估算一页的 I/O 代价，于是可以把预算分配做成一个优化问题；这里每条记录的「价值」只有排名一个信号，没有可估算的收益，于是只能做「公平分配」——交错排列正是「不做取舍时最公平的做法」。

**「tokenization 非单调」在主核里对应的是「压缩后的大小不可预测」。** 数据库的页压缩（LZ4、zstd）也有同样的性质：压缩两条记录后的大小不等于各自压缩之和，甚至可能更大。所以凡是依赖「单调性」的算法（二分查找容量、估算剩余空间）都必须留验证步骤。lightrag 的 `fit_prefix` 回退与数据库的「压缩失败则回退到未压缩页」是同一个补丁。

**「字符 / token / 字节三个度量空间」与主核的字符集与排序规则问题同构。** 数据库里 `VARCHAR(255)` 到底按字符还是字节算、排序按码点还是按区域规则，是一整类移植性 bug 的来源。`_truncate_entity_identifier` 同时限制两者，正是承认「上层按字符思考、后端按字节存储」这个不可避免的错位。

对 agent 工程的直接价值有三条。其一，**预算要按「模型真正读到的」计量**。把不渲染的字段算进成本，会让每次都能装的内容被无辜砍掉。其二，**任何基于单调性假设的搜索都要有验证回退**。二分搜索找「能装多少条」这个优化本身没错，但必须承认它可能不成立并留后手。其三，**同一个数值在两种语境下可能有相反含义**。配额 0 既是「关闭」也是「失败」，这个冲突只能靠把边界算对来避免——而它值得一句专门的注释，否则下一个「简化」它的人会引入静默降级。
