---
project: graphrag
feature: context-building
title: 上下文构建与动态社区选择
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/query/context_builder/dynamic_community_selection.py
    symbols: [DynamicCommunitySelection]
  - path: packages/graphrag/graphrag/query/context_builder/community_context.py
    symbols: [build_community_context]
  - path: packages/graphrag/graphrag/query/context_builder/local_context.py
    symbols: [build_entity_context, build_relationship_context]
  - path: packages/graphrag/graphrag/query/context_builder/source_context.py
    symbols: [build_text_unit_context]
---

# 上下文构建与动态社区选择

## 一句话职责

四种检索模式共用的「内容 → 表格」渲染层，加上一条只在 global search 启用的路径：让 LLM 从根社区开始**逐层给报告打分**，只展开评分达标的社区，从而用少量调用换掉「全量报告 → 大量 map 批次」。

## 机制是什么

这一层里有两个互不相干的东西，被放在同一个包下：

**一是渲染器**（`build_community_context` / `build_entity_context` / `build_relationship_context` / `build_text_unit_context`）。四者的形状高度一致：先写 `-----ContextName-----` 标题行，再写表头，再逐条拼 `分隔符.join(...)` 行，每行按 token 计数累加，超预算就 `break`。它们返回 `(text, dict[str, DataFrame])` 的元组——**同一个内容的两种形态**（给模型看的字符串 + 给审计用的结构化记录）。

**二是动态社区选择**（`DynamicCommunitySelection`），它解决的是 global search 的成本问题。

要理解第二件事的价值，得先回到 d03：global search 会把所有社区报告切成 N 批，每批一次 map 调用。一个万级实体的图有几千份报告，那就是几十次 LLM 调用——**而且大部分报告与问题无关**。

`DynamicCommunitySelection` 的设想是：先用廉价的方式判断「哪些社区值得读」，只把那些送去 map。而它选择的方式是**分层下降**：

- 从 `level 0`（最粗的社区）开始，逐个让 LLM 打分（0-10）。
- 某个社区达标（`rating >= threshold`）→ 收下它，并把它的**子社区**加入下一轮。
- 不达标 → 它的子树整枝跳过。
- **但有个兜底**：若某一层的「待评分队列」为空**且**目前一个相关社区都没有，就把下一层的全部社区加入队列重试。

最后一条是关键。它处理的是「阈值定得太高导致什么都不选」这个失败模式——**空结果比低质量结果更糟**，因为空结果会让下游彻底没有上下文。

## 怎么实现的

### 渲染器：五个函数共享一套骨架

四个渲染函数的形状完全一致，以 `build_entity_context` 最典型：

```python
current_context_text = f"-----{context_name}-----" + "\n"
header = ["id", "entity", "description"]
if include_entity_rank:
    header.append(rank_description)
attribute_cols = (
    list(selected_entities[0].attributes.keys())
    if selected_entities[0].attributes
    else []
)
header.extend(attribute_cols)
current_context_text += column_delimiter.join(header) + "\n"
current_tokens = tokenizer.num_tokens(current_context_text)
```

五处：

1. **`-----{context_name}-----` 这个五横线包裹的标题行** —— 它是渲染进 prompt 的文本的一部分，所以模型能看到「这一段是 Entities，那一段是 Relationships」。**这是「一张 prompt 里塞多张表」的分隔机制**。

2. **`attribute_cols` 取自 `selected_entities[0].attributes`** —— 即**只看第一个实体的属性键**，并假设所有实体有相同的属性集。

   **这是一个明确的近似**：若第一个实体没有某个属性而其他实体有，那些属性会被漏掉；反之若第一个有而其他没有，那些格会填 `""`。而 `attributes` 本来就是「可选补充字段」，所以这个假设在实践上成立。

3. **表头也计入 token 预算**（`current_tokens = tokenizer.num_tokens(current_context_text)` 在写第一行数据之前）。

4. **`rank_description` 作为列名**（默认 `"number of relationships"`）—— 所以表头里会出现一个很长的人类可读列名，而不是 `rank`。

   **这是一处刻意的设计**：让模型看到「这一列的含义」而不是一个缩写。代价是表头更长。

5. **`entity.short_id if entity.short_id else ""`** —— 三元的必要性在于 `short_id` 可以是 `None`（d01 的适配层用 `.get(short_id_key)`）。

**而 `build_relationship_context` 多一步「去重列名」：**

```python
attribute_cols = [col for col in attribute_cols if col not in header]
```

**因为关系表的固定列里已有 `source`/`target`/`description`/`weight`，而关系的 `attributes` 里可能也有同名键。** 不去重会让 `pd.DataFrame(records, columns=header)` 的列数不匹配（因为 `new_context` 只按 `attribute_cols` 追加了非重名的字段）。

**这是一处「必须做否则崩」的过滤**，而它与 `build_community_context` 的 `_get_header` 里的同名字段过滤是同一手法：

```python
def _get_header(attributes: list[str]) -> list[str]:
    header = ["id", "title"]
    attributes = [col for col in attributes if col not in header]
    if not include_community_weight:
        attributes = [col for col in attributes if col != community_weight_name]
    header.extend(attributes)
    header.append("summary" if use_community_summary else "content")
    if include_community_rank:
        header.append(community_rank_name)
    return header
```

**四处**：

1. **第一处过滤**：属性里若有 `id` 或 `title` 就去掉（否则重复列）。
2. **第二处过滤**：`include_community_weight=False` 时去掉权重列。**注意这是「按列名过滤」而不是「不计算」** —— 权重仍会被算进 `attributes`，只是不渲染。
3. **`summary` 或 `content` 二选一** —— 由 `use_community_summary` 决定。
4. **`rank` 最后追加** —— 所以它是最后一列。

**而最后两列的「谁先谁后」在不同调用点上不一致**：`_rank_report_context` 里排序用的是 `[weight, rank]`（权重优先），而表头里 `content` 在 `rank` 之前。所以列的物理顺序（`id, title, ..., content, rank`）与排序优先级（`weight, rank`）不同。**这是可以的**，因为排序用的是 DataFrame 的列名而不是位置。

### 分批逻辑：一个 `single_batch` 布尔改变整个控制流

```python
for report in selected_reports:
    new_context_text, new_context = _report_context_text(report, attributes)
    new_tokens = tokenizer.num_tokens(new_context_text)

    if batch_tokens + new_tokens > max_context_tokens:
        _cut_batch()
        if single_batch:
            break
        _init_batch()

    batch_text += new_context_text
    batch_tokens += new_tokens
    batch_records.append(new_context)
```

四处：

1. **`single_batch=True` 时超预算就 `break`** —— 只产出一批。**这是 local search 的路径**（d02）。
2. **`single_batch=False` 时切批并重开** —— 产出多批。**这是 global search 的路径**（d03）。
3. **`_cut_batch()` 在两种情况都调用** —— 它把当前累积的 `batch_records` 转成 DataFrame、排序、追加到 `all_context_text`/`all_context_records`。
4. **注意 `_init_batch()` 只在非单批时调用** —— 单批模式下 `break` 之后不再重开，所以循环结束。

**然后是一段不太直观的「最后一批要不要提交」的判断：**

```python
current_batch_ids = {record[0] for record in batch_records}
existing_ids_sets = [set(record["id"].to_list()) for record in all_context_records]

if current_batch_ids not in existing_ids_sets:
    _cut_batch()
```

三处：

1. **为什么需要这个判断？** 因为切批发生在「下一条放不下」时——那时 `_cut_batch()` 已经把上一批提交了。而循环正常结束后，**最后一批还留在 `batch_records` 里没提交**。所以需要补一次。

2. **为什么要比较 id 集合而不是直接提交？** 因为若最后一批恰好已经被提交过（例如上一条刚好触发切批、之后没有新条目进入），再提交会产生重复。

   **这个场景是可能的**：`_cut_batch()` 提交的是**切批之前**的累积。切批后 `_init_batch()` 清空了 `batch_records`，然后当前这条被加入。所以循环结束时 `batch_records` 至少有 1 条。**但如果循环在切批后立即结束（即被切掉的那一批是最后一个非空批，新批为空）**——那 `current_batch_ids` 是空集，而空集不在 `existing_ids_sets` 里（除非之前提交过空批），于是 `_cut_batch()` 被调用。而 `_cut_batch` 里第一句就是 `if len(record_df) == 0: return`，所以**空批被安全跳过**。

   **所以这个判断的作用是「空批时不重复提交」**，而实际的守卫在 `_cut_batch` 内部。**两者功能重叠**。

3. **它的比较方式有性能问题**：`current_batch_ids not in existing_ids_sets` 是对**列表**做成员判断（O(n)），且每次比较都是集合相等。而 `existing_ids_sets` 在循环外只算一次——但它是在**循环结束后**算的，所以只含已经提交的批。

**`_cut_batch` 里的单批标题重写：**

```python
def _cut_batch() -> None:
    record_df = _convert_report_context_to_df(
        context_records=batch_records, header=header,
        weight_column=(community_weight_name if entities and include_community_weight else None),
        rank_column=community_rank_name if include_community_rank else None,
    )
    if len(record_df) == 0:
        return
    current_context_text = record_df.to_csv(index=False, sep=column_delimiter)
    if not all_context_text and single_batch:
        current_context_text = f"-----{context_name}-----\n{current_context_text}"

    all_context_text.append(current_context_text)
    all_context_records.append(record_df)
```

四处：

1. **`to_csv` 重新渲染，丢弃了 `batch_text`** —— 注意 `batch_text` 在 `_init_batch` 里拼了标题与表头，但 `_cut_batch` **不用它**，而是用 DataFrame 的 `to_csv`。

   **所以 `batch_text` 与 `batch_tokens` 实际上只用于「预算判断与累积」，从不被输出。** 而真正的输出是 `to_csv` 的结果（不含 `-----ContextName-----` 标题、但含表头）。

2. **单批时手工补标题** —— `if not all_context_text and single_batch`（即「这是第一次提交且是单批模式」）时，给 CSV 前面加 `-----{context_name}-----\n`。

   **多批模式不加标题** —— 因为 global search 的每一批前面已经有了别的东西（对话历史前缀，见 d03），再加标题会重复。

3. **`record_df` 的排序发生在 `to_csv` 之前**（`_convert_report_context_to_df` 内部调 `_rank_report_context`）—— 所以**输出顺序是按权重降序、再按 rank 降序**，而不是报告在 `selected_reports` 里的顺序。

   **这意味着 `shuffle_data=True` 的打散在效果上被排序抵消了**（在每一批内部）。打散的作用只能体现在「**哪些报告被分到同一批**」上，而不是「批内顺序」。

   **这是一处重要的交互**：d03 里说打散是为了「让各批质量均匀」，而这里批内又按权重排序。两者不矛盾——打散决定了分组，排序决定了组内呈现。

4. **`len(record_df) == 0` 的早退** —— 空批不提交。

**权重计算有一个「只算一次」的守卫：**

```python
compute_community_weights = (
    entities
    and len(community_reports) > 0
    and include_community_weight
    and (
        community_reports[0].attributes is None
        or community_weight_name not in community_reports[0].attributes
    )
)
```

**最后一个条件是「第一个报告还没有这个属性」** —— 因为 `_compute_community_weights` **就地修改** 报告的 `attributes`。所以第二次调用时第一个报告已有该属性，就跳过重算。

**这是一处「用数据本身当缓存标记」的做法**。它的好处是不需要额外的状态；坏处是：

- **若 `normalize_community_weight` 改了**，缓存的归一化结果不会被重算。
- **`community_reports[0]` 是代表**——若第一个报告被替换（如动态选择筛掉了它），判断可能失效。
- **就地修改共享对象**，与 d02 的 `attributes["matches"]` 同一类副作用。

**`_compute_community_weights` 的定义：**

```python
community_text_units = {}
for entity in entities:
    if entity.community_ids:
        for community_id in entity.community_ids:
            if community_id not in community_text_units:
                community_text_units[community_id] = []
            community_text_units[community_id].extend(entity.text_unit_ids)
for report in community_reports:
    if not report.attributes:
        report.attributes = {}
    report.attributes[weight_attribute] = len(
        set(community_text_units.get(report.community_id, []))
    )
```

三处：

1. **权重 = 「该社区所有实体关联的文本单元的去重数」** —— 所以它衡量的是「这个社区覆盖了多少原始文本」。大社区权重高。

2. **`report.community_id` 与 `entity.community_ids` 必须同类型** —— 而 d01 讲过 `read_indexer_entities` 把社区 id 转成**字符串**（`[str(int(i)) for i in x]`）。所以这里也是字符串比较。

3. **`entity.text_unit_ids` 可能是 `None`** —— 而 `[...].extend(None)` 会 `TypeError`。**这是一处未防御的路径**（`community_ids` 判了，`text_unit_ids` 没判）。

   实际上 `entity.text_unit_ids` 在 `Entity` 模型里是 `list[str] | None = None`，所以**真的可能为 `None`**。若某个实体有 `community_ids` 但没有 `text_unit_ids`，这里会崩。

**归一化那段：**

```python
if normalize:
    all_weights = [
        report.attributes[weight_attribute]
        for report in community_reports
        if report.attributes
    ]
    max_weight = max(all_weights)
    for report in community_reports:
        if report.attributes:
            report.attributes[weight_attribute] = (
                report.attributes[weight_attribute] / max_weight
            )
```

**三处**：

1. **`max(all_weights)` 若列表为空会 `ValueError`** —— 但此时 `community_reports` 非空（前面判过），且循环里给每个报告都设了 `attributes`，所以 `all_weights` 非空。

2. **除以 `max_weight`** —— 所以归一化后最大值是 1.0。而 `max_weight` 可能是 0（若所有社区的文本单元数都是 0）→ **除零**。

   **这是一处真实的除零风险**：若传入的实体都没有 `text_unit_ids`（或都是空列表），则所有 `community_text_units` 都是空，所有权重都是 0，`max_weight = 0`，然后 `0 / 0` → `ZeroDivisionError`。

3. **归一化后权重变成 float** —— 而 `_rank_report_context` 里 `report_df[weight_column].astype(float)` 正好需要它是可转 float 的。所以两步是配套的。

### `DynamicCommunitySelection`：分层下降 + 三个兜底

**构造时建两张表与一层索引：**

```python
self.reports = {report.community_id: report for report in community_reports}
self.communities = {community.short_id: community for community in communities}

self.levels: dict[str, list[str]] = {}
for community in communities:
    if community.level not in self.levels:
        self.levels[community.level] = []
    if community.short_id in self.reports:
        self.levels[community.level].append(community.short_id)

self.starting_communities = self.levels["0"]
```

五处：

1. **`reports` 用 `community_id` 当键，`communities` 用 `short_id` 当键** —— 两张表的键**不是同一个东西**！

   而 `levels` 里存的是 `community.short_id`。所以 `self.reports[community]` 里那个 `community` 变量是 **short_id**，但 `self.reports` 的键是 **community_id**。

   **这意味着 `self.reports[short_id]` 只在 `short_id == community_id` 时成立。** 而 d01 讲过 `CommunityReport.community_id` 来自 `d["community"]`（即社区 id），`short_id` 来自 `d.get("human_readable_id")`。**两者通常不同**（一个是哈希/序号，一个是人类可读编号）。

   **所以 `levels` 里那些 id 很可能在 `self.reports` 里查不到** —— 这会让 `rate_relevancy` 的 `description=self.reports[community].summary` 直接 `KeyError`。

   **这是一处需要实证的疑点**。除非上游让 `community.short_id` 与 `report.community_id` 相等。而 `read_indexer_reports`（d01）调 `read_community_reports(df=reports_df, id_col="id", short_id_col="community")`——**注意 `short_id_col="community"`**！所以报告的 `short_id` 取自 `community` 列，而 `community_id` 也取自 `community` 列（`from_dict` 的 `community_id_key="community"`）。

   **所以 `report.short_id == report.community_id`。** 而 `Community` 的 `short_id` 也取自 `human_readable_id`——那与 `community` 列是否相同取决于索引产物。

   从代码的自洽性推断：**这里假设 `community.short_id == report.community_id`**。而 `TODO check why some sub_communities are NOT in report_df` 这个注释说明作者也知道报告集不全。

2. **`if community.short_id in self.reports`** —— 只把「有报告」的社区加入 `levels`。所以「社区有但报告没有」的被排除。

3. **`self.starting_communities = self.levels["0"]`** —— **直接用下标 `["0"]`**（键是字符串 `"0"`）。若 `levels` 里没有 `"0"`（例如所有社区都是 level 1）会 `KeyError`。

   **这是一处硬编码依赖**：必须有 level 0 的社区。而 c01 讲过 §`level == 0` 是最粗一层，顶层簇的父是 `-1`。若索引产物里没有 level 0（罕见但可能），这里会崩。

4. **`self.levels[community.level]`** —— `Community.level` 的类型是 `str`（模型里 `level: str`）。所以键是字符串 `"0"`、`"1"`、`"2"`。

5. **`self.levels` 的类型标注是 `dict[str, list[str]]`** —— 与 `Community.level: str` 一致。

**主循环：逐层下降。**

```python
queue = deepcopy(self.starting_communities)
level = 0
ratings = {}
relevant_communities = set()

while queue:
    gather_results = await asyncio.gather(*[
        rate_relevancy(
            query=query,
            description=(self.reports[community].summary if self.use_summary else self.reports[community].full_content),
            model=self.model, tokenizer=self.tokenizer, rate_query=self.rate_query,
            num_repeats=self.num_repeats, semaphore=self.semaphore, **self.model_params,
        )
        for community in queue
    ])
```

六处：

1. **`deepcopy(self.starting_communities)`** —— 因为后面 `queue = self.levels[str(level)]` 会**直接引用** `levels` 里的列表。而 `deepcopy` 只保护第一次赋值，兜底分支的赋值仍是引用（但那时不再改 `queue` 的内容，只读，所以安全）。

   **为什么第一次要 deepcopy？** 其实不需要——`queue` 从未被就地修改。**这是一处多余的防御**。

2. **`asyncio.gather(*[...])` 一次性提交整层** —— 由 `rate_relevancy` 内部的 `semaphore` 限流。所以并发上限是 `concurrent_coroutines`（默认 8）。

3. **`description` 在 `use_summary` 时为 `summary`，否则 `full_content`** —— 所以「用摘要」能省 token 但精度低。默认 `use_summary=False`（构造器），而 `factory.py` 传的是 `gs_config.dynamic_search_use_summary`。

4. **`num_repeats` 交给 `rate_relevancy`** —— 它会重复调用 N 次然后**取众数**（见下）。

5. **`**self.model_params`** —— 直接展开，所以 `temperature` 等会影响评分。

6. **`self.reports[community]`** —— 如前所述，`community` 是 `short_id`，键是 `community_id`。

**然后处理结果：**

```python
communities_to_rate = []
for community, result in zip(queue, gather_results, strict=True):
    rating = result["rating"]
    ratings[community] = rating
    llm_info["llm_calls"] += result["llm_calls"]
    llm_info["prompt_tokens"] += result["prompt_tokens"]
    llm_info["output_tokens"] += result["output_tokens"]
    if rating >= self.threshold:
        relevant_communities.add(community)
        if community in self.communities:
            for child in self.communities[community].children:
                child_str = str(child)
                if child_str in self.reports:
                    communities_to_rate.append(child_str)
                else:
                    logger.debug("dynamic community selection: cannot find community %s in reports", child)
        if not self.keep_parent and community in self.communities:
            relevant_communities.discard(self.communities[community].parent)
```

七处：

1. **`zip(..., strict=True)`** —— 因为 `gather` 保证顺序与输入一致，且长度相同。`strict=True` 让长度不匹配时报错（防未来改动引入错位）。

2. **`rating >= self.threshold`** —— 文档说「EQUAL or ABOVE」，与实现一致。

3. **只有达标才展开子社区** —— 所以不达标的子树被整枝跳过。**这是分层的全部价值**。

4. **`if community in self.communities`** —— 因为 `communities` 用 `short_id` 索引，而这个判断是「这个 short_id 有对应的 Community 对象」。

5. **`str(child)` 转换** —— 因为 `Community.children` 的类型标注是 `list[str]`，但注释说「Convert child to string to match self.reports key type」——说明实际可能不是字符串（可能来自 pandas 的 numpy 类型）。

6. **`if child_str in self.reports`** —— 子社区必须有报告才加入队列。TODO 注释承认了「some sub_communities are NOT in report_df」。

7. **`relevant_communities.discard(self.communities[community].parent)`** —— **加入子社区时移除父社区**（除非 `keep_parent`）。

   **这个逻辑值得细想**：`discard` 而不是 `remove`（父本来可能不在集合里）。而「父被移除」意味着**最终选中的是「最细的相关层」而不是「所有相关层」**。

   理由能推出来：父报告的 `full_content` 是子报告的概括，两者都送进 map 会重复。所以「选最细的」避免了冗余。

   但这也带来一个问题：**若父社区达标而子社区全部不达标**，那父会被保留（因为没有子社区触发 `discard`）。所以「最细的相关层」实际是「最早不再细化的那一层」。

**队列推进与兜底：**

```python
queue = communities_to_rate
level += 1
if (
    (len(queue) == 0)
    and (len(relevant_communities) == 0)
    and (str(level) in self.levels)
    and (level <= self.max_level)
):
    logger.debug("dynamic community selection: no relevant community reports, adding all reports at level %s to rate.", level)
    queue = self.levels[str(level)]
```

四处：

1. **`queue = communities_to_rate`** —— 无条件替换。所以「没达标的社区的子树」不会进入下一轮。

2. **`level += 1` 无条件递增** —— 即使 `queue` 为空。所以进入兜底分支时 `level` 已经是下一层。

3. **兜底的四个条件**：队列空 **且** 目前零相关社区 **且** 下一层存在 **且** `level <= max_level`。

   **「目前零相关」这个条件是关键**：它保证了兜底只在「完全失败」时触发，不会在「部分成功」时降低精度。

4. **`queue = self.levels[str(level)]`** —— 把**整层**加入队列（不只是某个父的子社区）。所以兜底是「这一层全评一遍」。

   **这与正常路径的不对称值得注意**：正常路径只评「达标父的子社区」，兜底评「整层」。所以兜底的开销可能很大（一层可能有几百个社区）。

**循环的终止**：`while queue`。所以队列空就退出。而兜底会重新填满队列——但 `level <= max_level` 限制了兜底次数。

**若兜底后仍然零相关**：下一轮循环 `queue` 非空，评完分，若仍全不达标则 `relevant_communities` 仍空，`queue` 为空，`level` 再 +1，再兜底……直到 `level > max_level`。此时 `queue` 保持为空，循环退出，**返回空列表**。

**所以「零相关」是可能的最终结果** —— 而 `GlobalCommunityContext` 会拿这个空列表去 `build_community_context`，后者 `if len(selected_reports) == 0: return ([], {})`，于是 `context_chunks` 是空列表 → **零个 map 任务** → reduce 阶段所有 key_points 为空 → 返回 `NO_DATA_ANSWER`（d03）。

**这是一条完整的退化链**：动态选择什么都没选 → 没有批次 → 没有要点 → 「我不知道」。**而这条链是静默的**（只有 `logger.debug`）。

### `rate_relevancy`：投票 + 容错

```python
for _ in range(num_repeats):
    async with semaphore if semaphore is not None else nullcontext():
        model_response = await model.completion_async(
            messages=messages_builder.build(), response_format_json_object=True, **model_params,
        )
        response = await gather_completion_response_async(model_response)
    try:
        _, parsed_response = try_parse_json_object(response)
        ratings.append(parsed_response["rating"])
    except KeyError:
        logger.warning("Error parsing json response, defaulting to rating 1")
        ratings.append(1)
    llm_calls += 1
    prompt_tokens += tokenizer.num_prompt_tokens(messages_builder.build())
    output_tokens += tokenizer.num_tokens(response)
options, counts = np.unique(ratings, return_counts=True)
rating = int(options[np.argmax(counts)])
```

七处：

1. **`async with semaphore if semaphore is not None else nullcontext()`** —— 用一个三元表达式在「有信号量」与「`nullcontext()`」之间选。**这是「可选限流」的简洁写法**，比 `if semaphore: async with ...` 重复代码好。

2. **`messages_builder.build()` 被调用两次** —— 一次传给模型，一次传给 `num_prompt_tokens`。若 `build()` 是纯函数则无害，但每次调用都重建列表。

3. **`try_parse_json_object` 的返回值只用了第二个** —— `_, parsed_response`。所以清洗后的文本被丢弃。

4. **只捕 `KeyError`** —— 即「JSON 解析成功但没有 `rating` 键」。而 `try_parse_json_object` 本身不抛（返回 `{}`），所以 `parsed_response["rating"]` 抛 `KeyError`。

   **但若 `rating` 的值是非数字字符串**（如 `"high"`），`parsed_response["rating"]` 能取到，`np.unique` 也能处理（当字符串），而最后 `int(options[np.argmax(counts)])` 会 `ValueError`。**这是一处未覆盖的路径**。

5. **解析失败默认 1** —— 注释说「so the report is kept」。**这个默认值是「保底通过」而不是「保底拒绝」**：`threshold` 默认 1，所以 rating=1 刚好达标。

   **这是一处「宁可多读也不漏读」的偏置**：解析失败时倾向保留。理由能推出来——漏掉一个相关社区会让答案缺信息（不可恢复），多读一个只是多花 token。

6. **`num_repeats` 次调用后取众数** —— `np.unique(ratings, return_counts=True)` 然后 `argmax(counts)`。

   **平票时 `argmax` 返回第一个** —— 而 `np.unique` 返回的 `options` 是**排序后**的。所以平票时取**较小**的 rating。

   **这是一处保守的平票规则**：两个 rating 票数相同，取低的。所以「不确定时倾向不展开」。

   而 `num_repeats=1`（默认）时投票没意义——`np.unique` 单元素，直接返回它。

7. **`prompt_tokens` 用 `tokenizer.num_prompt_tokens(messages_builder.build())`** —— 这是**含 system + user 的完整 prompt**。而 d02/d05 的 `prompt_tokens` 只算 system prompt。

   **所以这里的统计更准确**。

## 关键取舍

**源码事实：** 四个渲染函数共享同一套骨架：`-----{context_name}-----` 标题行 → 表头 → 逐行 `分隔符.join(...)` → 按 `tokenizer.num_tokens` 累加 → 超预算 `break`；返回 `(text, dict[str, DataFrame])` 两形态。

**源码事实：** `attribute_cols` 取自**第一条记录的 `attributes` 键**，假设所有记录属性集相同；关系的 `attribute_cols` 与社区的 `_get_header` 都要过滤掉与固定列重名的键（否则 `pd.DataFrame(records, columns=header)` 列数不匹配）。

**源码事实：** `build_community_context` 里 `single_batch` 一个布尔改变控制流：`True` 时超预算 `break`（只一批，local 用）；`False` 时切批重开（多批，global 用）。

**源码事实：** `batch_text` / `batch_tokens` 只用于预算判断与累积，**从不输出**——真正输出的是 `_cut_batch` 里的 `to_csv()`，它不含标题行（单批模式才手工补 `-----{context_name}-----\n`），且**含表头**。

**源码事实：** `_cut_batch` 的输出顺序由 `_rank_report_context` 决定（先权重降序、再 rank 降序），所以 `shuffle_data=True` 的打散只影响「哪些报告被分到同一批」，不影响批内顺序——两者不矛盾（打散决定分组，排序决定组内呈现）。

**源码事实：** 「最后一批要不要提交」用 `current_batch_ids not in existing_ids_sets` 判断，但真正的守卫在 `_cut_batch` 内部的 `if len(record_df) == 0: return`——两者功能重叠。

**源码事实：** 社区权重的计算用「第一个报告是否已有该属性」当缓存标记（`_compute_community_weights` 就地修改 `report.attributes`），所以 `normalize_community_weight` 改了不会重算；且这是对共享对象的副作用（同 d02 的 `attributes["matches"]`）。

**源码事实：** 权重 = 该社区所有实体关联文本单元的**去重数**；`entity.text_unit_ids` 未判 `None` 就 `extend`，而 `Entity.text_unit_ids` 类型是 `list[str] | None`——有 `community_ids` 但无 `text_unit_ids` 的实体会触发 `TypeError`。

**源码事实：** 归一化除以 `max_weight`，若所有社区权重都是 0（实体无 `text_unit_ids`）则 `ZeroDivisionError`。

**源码事实：** `DynamicCommunitySelection` 的 `reports` 用 `community_id` 当键、`communities` 用 `short_id` 当键，而 `levels` 存的是 `short_id`——**依赖 `report.short_id == report.community_id`**（从 `read_indexer_reports` 的 `short_id_col="community"` 与 `community_id_key="community"` 同为 `community` 列可推得）。`TODO` 注释承认「some sub_communities are NOT in report_df」。

**源码事实：** `self.starting_communities = self.levels["0"]` **硬编码要求存在 level 0**，否则 `KeyError`。

**源码事实：** `queue = deepcopy(self.starting_communities)` 的拷贝是多余的（`queue` 从未被就地修改），而兜底分支的 `queue = self.levels[str(level)]` 是**引用**（只读，所以安全）。

**源码事实：** 达标（`rating >= threshold`）才展开子社区，不达标的子树整枝跳过；加入子社区的同时 `relevant_communities.discard(parent)`（除非 `keep_parent`），所以最终选中的是「最早不再细化的那一层」而非所有相关层。

**源码事实：** 兜底触发需四个条件同时成立：队列空 **且** 零相关 **且** 下一层存在 **且** `level <= max_level`；兜底把**整层**加入队列（不是某个父的子社区），所以开销可能远大于正常路径。

**源码事实：** 「零相关」是可能的最终结果，此时 `GlobalCommunityContext` 拿到空列表 → `build_community_context` 返回 `([], {})` → 零个 map 任务 → 全 0 分 → `NO_DATA_ANSWER`。这条退化链是静默的（只有 `logger.debug`）。

**源码事实：** `rate_relevancy` 的 `num_repeats` 次调用取**众数**（`np.unique` + `argmax`），平票时 `argmax` 返回第一个而 `np.unique` 的 `options` 已排序 → **平票取较小值**（保守：倾向不展开）。`num_repeats=1`（默认）时投票无意义。

**源码事实：** 解析失败（`KeyError`）默认 rating = **1**，注释说「so the report is kept」——而 `threshold` 默认 1，所以恰好达标。这是「宁可多读也不漏读」的偏置。只捕 `KeyError`，所以 `rating` 是非数字字符串时会在 `int(options[argmax])` 处 `ValueError`。

**源码事实：** `rate_relevancy` 的 `prompt_tokens` 用 `tokenizer.num_prompt_tokens(messages_builder.build())`（含 system + user），比 d02/d05 只算 system prompt 的统计更准确；`messages_builder.build()` 被调用两次。

**源码事实：** `async with semaphore if semaphore is not None else nullcontext()` 是「可选限流」的简洁写法。

**架构推论：** 「动态社区选择」的核心假设是**「相关性在社区层级上是单调的」**——父社区不相关则子社区也不相关。这个假设让「整枝跳过」成为可能（省掉大量评分调用），而它只在「子社区比父社区更专精于同一话题」时成立。c01 讲过层次 Leiden 产出的嵌套保证「上层簇是下层簇的并集」，所以话题确实包含——但**包含不等于相关**：一个宽泛的父社区可能因为某个子话题相关而整体「有点相关」，此时父会达标、子全部不达标，最终选中父（那是正确的）；反之一个宽泛的父社区因为话题太杂而不达标，但其中一个子社区恰好相关——**此时整枝跳过会漏掉它**。这是这套机制的**真实盲区**，而兜底条件（零相关才降一层）无法覆盖「部分相关但漏了别的」。

**架构推论：** 四个渲染函数都是「字符串拼接 + token 计数」，而不是「用模板引擎渲染」。好处是零依赖、可精确控制每个 token；坏处是每个函数都要重复一遍「累加、判断、break」的循环。而它们的**一致性是这个设计的价值**：四种内容用同一套渲染规则，所以模型看到的表格格式统一，prompt 可以只有一套引用语法。

**架构推论：** 「解析失败默认 1」与「平票取较小」这两个规则方向相反——前者的偏置是「保留」，后者的偏置是「不展开」。它们并不矛盾：前者处理「模型输出坏了」（信息缺失，该保守保留），后者处理「模型摇摆不定」（信息充分但不一致，该保守拒绝）。**判据是「有没有信号」**：没信号时按最坏情况假设但选安全方向，有冲突信号时选低风险方向。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/query/context_builder/dynamic_community_selection.py` | `DynamicCommunitySelection` | 逐层评分的社区筛选 |
| `packages/graphrag/graphrag/query/context_builder/dynamic_community_selection.py` | `DynamicCommunitySelection.select` | 主循环 + 四条件兜底 |
| `packages/graphrag/graphrag/query/context_builder/community_context.py` | `build_community_context` | 社区报告渲染（单批/多批） |
| `packages/graphrag/graphrag/query/context_builder/community_context.py` | `_compute_community_weights` | 权重 = 实体关联文本单元去重数 |
| `packages/graphrag/graphrag/query/context_builder/community_context.py` | `_rank_report_context` | 按权重 + rank 降序排序 |
| `packages/graphrag/graphrag/query/context_builder/community_context.py` | `_convert_report_context_to_df` | 记录列表 → 排序后的 DataFrame |
| `packages/graphrag/graphrag/query/context_builder/local_context.py` | `build_entity_context` | 实体表渲染 |
| `packages/graphrag/graphrag/query/context_builder/local_context.py` | `build_relationship_context` | 关系表渲染（列名去重） |
| `packages/graphrag/graphrag/query/context_builder/local_context.py` | `build_covariates_context` | 协变量表渲染 |
| `packages/graphrag/graphrag/query/context_builder/source_context.py` | `build_text_unit_context` | 文本单元表渲染 |
| `packages/graphrag/graphrag/query/context_builder/source_context.py` | `count_relationships` | 文本单元连接的关系数（两条路径） |
| `packages/graphrag/graphrag/query/context_builder/rate_relevancy.py` | `rate_relevancy` | 0-10 评分 + 多次投票取众数 |
| `packages/graphrag/graphrag/query/context_builder/rate_prompt.py` | `RATE_QUERY` | 评分 prompt 模板 |
| `packages/graphrag/graphrag/query/input/retrieval/entities.py` | `to_entity_dataframe` | 实体 → DataFrame（筛选候选） |
| `packages/graphrag/graphrag/query/input/retrieval/relationships.py` | `get_in_network_relationships` / `get_out_network_relationships` | 一跳邻边分成进出两组 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：动态社区选择](https://microsoft.github.io/graphrag/query/dynamic_community_selection/)
- [graphrag 论文（Edge et al., 2024）](https://arxiv.org/abs/2404.16130)
- [论文：分层摘要与递归概括](https://arxiv.org/abs/2109.10862)

## 与主核 / AI 的关系

「动态社区选择」与主核（多模态数据库内核）的**索引选择**是同一个决策问题：用一次廉价的探测决定「读哪些数据」，而不是全量读。主核的做法是查统计信息（`pg_class.reltuples`、直方图）估算选择性；这里用 LLM 给报告打 0-10 分。

**差别在于代价模型的有无**：主核能算出「走索引扫 200 页 vs 全表扫 2000 页」，所以能选对；这里只能得到「相关性分数」，而**分数与代价无关**——一个 10 分的社区可能有 10000 token，一个 3 分的可能只有 500 token。所以这套机制优化的是「读多少份报告」而不是「读多少 token」。

第二处对照是「父不相关则子树跳过」与主核的**分区裁剪**（partition pruning）。两者都靠「上层元数据能代表下层数据」这个假设。分区的假设是「分区键的值域」，是可证明的；社区层级的假设是「话题包含」，是启发式的。**判据是「上层信息是否对下层有约束力」**：分区键有（值是硬约束），话题没有（同一个子社区可以讨论父话题之外的东西）。

第三处是渲染器里「表头也计入 token 预算」这个细节与主核的**页头开销**。主核的每页有 `PageHeaderData`（24 字节）占用，所以可用空间是 `BLCKSZ - header`。两者都是「先扣固定开销，再分配内容」——**因为固定开销是必然成本，不能假装它不存在**。

对 RAG 工程的实际意义在于：上下文构建是所有 RAG 的公共瓶颈，而它最容易出的问题是「预算算不准」与「顺序丢失」。这份实现给出的三条经验可直接用：**把固定开销（标题行、表头）计入预算**、**渲染与线程结构分离**（同一份内容同时产出文本与 DataFrame，便于审计）、以及**分层筛选要有兜底**（零结果比低质量结果更糟——降一层重试总比什么都不给好）。
