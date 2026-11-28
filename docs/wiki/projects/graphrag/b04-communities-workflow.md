---
project: graphrag
feature: communities-workflow
title: 社区划分与报告生成
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/index/workflows/create_communities.py
    symbols: [create_communities]
  - path: packages/graphrag/graphrag/index/workflows/create_community_reports.py
    symbols: [create_community_reports, _prep_nodes, _prep_edges]
  - path: packages/graphrag/graphrag/index/operations/finalize_community_reports.py
    symbols: [finalize_community_reports]
---

# 社区划分与报告生成

## 一句话职责

`create_communities` 把聚类结果（`level / community / parent / title` 四元组）与实体表、关系表**拼装成一张社区表**（每个社区带上它的实体 id、**社区内**关系 id、子社区列表、规模、日期）；`create_community_reports` 则为每个社区组装一份「该社区的实体 + 边 + claims」的上下文，让 LLM 写出一份带 `findings` 与 `rating` 的结构化报告。

## 机制是什么

这两步是 graphrag 的**核心产物生成**，而它们的难度都来自同一个事实：**一个社区包含的东西远多于一个提示词窗口能装下的量。**

一个上层社区可能有几百个实体、上千条边。要给它写报告，必须先回答「在这个 token 预算里放什么」。这就是为什么报告生成不是一个 `format(prompt, context)` 调用，而是一条五段的流水线：

1. **`explode_communities`**：把「社区 → 实体 id 列表」爆开成「实体 → 所属社区」，与实体表 merge，得到「每个实体属于哪个社区」的长表。
2. **`_prep_nodes` / `_prep_edges` / `_prep_claims`**：给每类记录造一个 `*_DETAILS` 列（一个 dict 列表，只含需要的字段），这是**提示词渲染的输入格式**。
3. **`build_local_context`**：按社区把实体与边分组，做 token 预算内的裁剪，产出 `context_string`。
4. **`summarize_communities`**：逐层逐社区调 LLM 生成报告；**上层社区的上下文里可以包含下层社区的报告**。
5. **`finalize_community_reports`**：补 `parent` / `children` / `size` / `period`，按 `full_content` 算 sha512 当 id。

第 4 步里的「可以包含下层报告」是关键设计：**层次结构让「上层社区的报告」可以基于「下层社区的报告」而不是原始实体边**。否则上层社区的报告会因为输入太大而被截断成随机的一部分。

## 怎么实现的

**`create_communities` 的第一步是「先算实体映射，再按层算社区内关系」。**

```
clusters = cluster_graph(relationships, max_cluster_size, use_lcc, seed=seed)

title_to_entity_id: dict[str, str] = {}
async for row in entities_table:
    title_to_entity_id[row["title"]] = row["id"]

communities = pd.DataFrame(
    clusters, columns=pd.Index(["level", "community", "parent", "title"])
).explode("title")
communities["community"] = communities["community"].astype(int)
```

**`cluster_graph` 返回的四元组里 `title` 其实是「该社区包含的实体名列表」**（c01 讲过它是重新组织成 `(level, cluster_id, parent_id, nodes)`）。这里 `explode("title")` 把它爆开成一行一个实体，于是 `communities` 变成「社区 × 实体」的长表。

**然后核心的那一段是「按层逐个算，而不是一次算完」。**

```
level_results = []
for level in communities["level"].unique():
    level_comms = communities[communities["level"] == level]
    with_source = relationships.merge(level_comms, left_on="source", right_on="title", how="inner")
    with_both = with_source.merge(level_comms, left_on="target", right_on="title", how="inner")
    intra = with_both[with_both["community_x"] == with_both["community_y"]]
    if intra.empty:
        continue
    grouped = (
        intra
        .explode("text_unit_ids")
        .groupby(["community_x", "parent_x"])
        .agg(
            relationship_ids=("id", list),
            text_unit_ids=("text_unit_ids", list),
        )
        .reset_index()
    )
    grouped["level"] = level
    level_results.append(grouped)
```

**代码里的注释说明了为什么要按层循环：**

> Process one hierarchy level at a time to keep intermediate DataFrames small, then concat the grouped results once at the end.

**这是纯粹的工程考虑**：一次把 `relationships` × 全部 `communities` 做 join，中间表会是「边数 × 社区数」的量级；按层做，每次只有「边数 × 该层社区数」。对深层次聚类（`max_cluster_size=10` 会产出很多层）这个差别能到几个数量级。

而 `merge` 之后 `community_x` / `community_y` 的先后出现依赖 pandas 的列名冲突自动改后缀——**这是一处对 pandas 行为的隐式依赖**，`with_source` 的 `community` 列与 `with_both` 里右表的 `community` 冲突，于是变成 `_x` / `_y`。代码在后面用 `rename` 把 `community_x` 改回 `community`，说明作者知道这个机制。

**`intra = with_both[community_x == community_y]` 这一行是「只保留社区内边」的判定。**

经过两次 inner join，`with_both` 只保留「source 在某个社区 X 里、target 在某个社区 Y 里」的边。取 `X == Y` 就得到社区内边。

**注意这里有个隐含的重复问题**：同一个实体可能属于同一层的多个社区吗？在 Leiden 的划分下不会（同一层是划分），但 `communities` 表是从 `explode("title")` 来的——若同一实体在同一层的两个社区里都出现（数据异常或 `cluster_graph` 的输出有重叠），join 会产出 `X==Y` 之外的多对。代码没有防这个。

**`text_unit_ids` 是靠 `explode` 再 `set` 去重来聚合的。**

```
intra
.explode("text_unit_ids")
.groupby(["community_x", "parent_x"])
.agg(
    relationship_ids=("id", list),
    text_unit_ids=("text_unit_ids", list),
)
...
# deduplicate the lists
all_grouped["relationship_ids"] = all_grouped["relationship_ids"].apply(lambda x: sorted(set(x)))
all_grouped["text_unit_ids"] = all_grouped["text_unit_ids"].apply(lambda x: sorted(set(x)))
```

**`explode("text_unit_ids")` 是必需的**：一条边的 `text_unit_ids` 是列表，不爆开 `agg(list)` 会产出「列表的列表」。爆开之后再 `set` 去重，得到「该社区内所有边涉及的全部文本单元」。

**`sorted(set(x))` 而不是 `list(set(x))`** ——排序让输出可复现（c01 讲的「可复现是硬要求」在这里延续到了社区表）。

**`parent_x` 参与 groupby 键**，于是每个社区行都带上它的父社区 id。这为后面的 `children` 反推留了基础。

**`children` 是从 `parent` 反推的一次 groupby。**

```
final_communities["parent"] = final_communities["parent"].astype(int)
# collect the children so we have a tree going both ways
parent_grouped = cast(
    "pd.DataFrame",
    final_communities.groupby("parent").agg(children=("community", "unique")),
)
final_communities = final_communities.merge(
    parent_grouped,
    left_on="community",
    right_on="parent",
    how="left",
)
final_communities["children"] = final_communities["children"].apply(
    lambda x: x if isinstance(x, np.ndarray) else []
)
```

**这段的逻辑值得逐步追一遍，因为 `merge` 的 `left_on` / `right_on` 交错容易读错：**

- `parent_grouped` 的索引是 `parent`、列是 `children`（该 parent 下所有 community 的数组）。
- `merge(left_on="community", right_on="parent")` 的意思是：**拿本行的 `community` 去对上「作为别人的 parent」这个角色**。于是本行拿到的是「以我为父的那些社区」。
- `how="left"` 让叶子社区（没有子）保留，`children` 为 `NaN`。
- 最后那句 `apply` 把 `NaN` 换成 `[]`，用 `isinstance(x, np.ndarray)` 判定（`agg("unique")` 产出的是 numpy 数组）。

**注意 `parent_grouped` 的索引在 merge 后会作为一列出现**（名为 `parent`），而 `final_communities` 本来就有 `parent` 列 → 列名冲突 → 又是一个隐式依赖 pandas 后缀的假设。代码没有处理，说明它是「恰好能跑」的（`left_on` / `right_on` 不同名时 pandas 会保留右表的键列，与左表的 `parent` 撞名，实际会生成 `parent_x` / `parent_y`——那么最终的 `parent` 列会是哪一个，取决于后续是否 `rename`。这里没有 rename，所以最终 `output = final_communities.loc[:, COMMUNITIES_FINAL_COLUMNS]` 会**因为找不到 `parent` 列而抛 `KeyError`，或者取到 `parent_x` 的旧值**。这一段是这一页最可疑的一处实现）。

**其余字段的填充都是直白的。**

```
final_communities["id"] = [str(uuid4()) for _ in range(len(final_communities))]
final_communities["human_readable_id"] = final_communities["community"]
final_communities["title"] = "Community " + final_communities["community"].astype(str)
final_communities["period"] = datetime.now(timezone.utc).date().isoformat()
final_communities["size"] = final_communities.loc[:, "entity_ids"].apply(len)
```

四处：

1. **`id` 是 `uuid4()`** ——与实体一样，每次重跑都变。
2. **`human_readable_id` 直接等于 `community`**（聚类给的簇号）。于是它是**每层内唯一但不全局唯一**的整数——不同层可以有同一个 `community` 号。
3. **`title` 是 `"Community " + 簇号`。** 这是给提示词用的名字，也是 a03 里 `CommunityReport` 继承 `Named` 的理由（有 title）。
4. **`period` 是「今天」的日期字符串**，不是内容的时间范围。字段名叫 `period` 容易误解为「这段社区覆盖的时间段」，实际是「这份索引的生成日期」。它存在是为了增量更新的版本标记。

**`_sanitize_row` 把 numpy 类型转成原生类型。**

```
def _sanitize_row(row: dict[str, Any]) -> dict[str, Any]:
    """Convert numpy types to native Python types for table serialization."""
    sanitized = {}
    for key, value in row.items():
        if isinstance(value, np.ndarray):
            sanitized[key] = value.tolist()
        elif isinstance(value, np.integer):
            sanitized[key] = int(value)
        elif isinstance(value, np.floating):
            sanitized[key] = float(value)
        else:
            sanitized[key] = value
    return sanitized
```

**三支分派覆盖 `np.ndarray` / `np.integer` / `np.floating`。** 需要它的原因是 `to_dict("records")` 之后的字典里，从 numpy 数组/标量派生出来的值会保留 numpy 类型，而 CSV/JSON 序列化不认识它们（`ValueError: Object of type int64 is not JSON serializable`）。

**注意它没有处理 `np.bool_`** ——`children` 之类若含布尔会漏网。也没有递归：`sanitized[key].tolist()` 只转一层，若 `numpy` 数组里套 numpy 标量，`tolist()` 会自动降到 Python 原生类型，所以实际上够用。

**`_prep_*` 三个函数做的是「给提示词造它认识的输入形状」。**

```
def _prep_nodes(input: pd.DataFrame) -> pd.DataFrame:
    """Prepare nodes by filtering, filling missing descriptions, and creating NODE_DETAILS."""
    input.loc[:, schemas.DESCRIPTION] = input.loc[:, schemas.DESCRIPTION].fillna("No Description")

    input.loc[:, schemas.NODE_DETAILS] = input.loc[  # type: ignore
        :,
        [
            schemas.SHORT_ID,
            schemas.TITLE,
            schemas.DESCRIPTION,
            schemas.NODE_DEGREE,
        ],
    ].to_dict(orient="records")

    return input


def _prep_edges(input: pd.DataFrame) -> pd.DataFrame:
    input.fillna(value={schemas.DESCRIPTION: "No Description"}, inplace=True)
    input.loc[:, schemas.EDGE_DETAILS] = input.loc[  # type: ignore
        :,
        [
            schemas.SHORT_ID,
            schemas.EDGE_SOURCE,
            schemas.EDGE_TARGET,
            schemas.DESCRIPTION,
            schemas.EDGE_DEGREE,
        ],
    ].to_dict(orient="records")

    return input


def _prep_claims(input: pd.DataFrame) -> pd.DataFrame:
    input.fillna(value={schemas.DESCRIPTION: "No Description"}, inplace=True)
    input.loc[:, schemas.CLAIM_DETAILS] = input.loc[  # type: ignore
        :,
        [
            schemas.SHORT_ID,
            schemas.CLAIM_SUBJECT,
            schemas.TYPE,
            schemas.CLAIM_STATUS,
            schemas.DESCRIPTION,
        ],
    ].to_dict(orient="records")

    return input
```

**核心手法是 `.loc[:, [列名列表]].to_dict(orient="records")` ——把选中的几列变成一个 dict 列表，装进一个新列。**

于是每一行除了原有字段，还多了一个 `node_details`（或 `edge_details` / `claim_details`）列，它的值是一个 dict 列表（每行只有一个 dict，因为是从单行选列）。**这个设计让它能直接被拼进提示词的 JSON 渲染。**

三处细节：

1. **选的字段都是「短名 + 内容 + 一个量化指标」**：节点是 `short_id / title / description / degree`，边是 `short_id / source / target / description / combined_degree`，claim 是 `short_id / subject / type / status / description`。
2. **`degree` / `combined_degree` 被放进详情** ——这是让 LLM 知道「哪个实体/边更重要」的唯一依据。b03 算的度数在这里被消费。
3. **`description` 缺失填 `"No Description"` 而不是空串。** 空串会让提示词里出现 `"description": ""`，模型可能理解为「描述就是空的」；`"No Description"` 是明确的自然语言信号。

**`_prep_nodes` 与另两个的 fillna 写法不同。**

```
# _prep_nodes：只填 description 一列，赋值回去
input.loc[:, schemas.DESCRIPTION] = input.loc[:, schemas.DESCRIPTION].fillna("No Description")

# _prep_edges / _prep_claims：填整个表的所有列，inplace
input.fillna(value={schemas.DESCRIPTION: "No Description"}, inplace=True)
```

**`_prep_edges` 那个 `fillna(value={...})` 看上去像只填 `description`，实际 `value` 是 dict 时 pandas 只填指定的键**——所以行为确实等价。但 `inplace=True` 与 `_prep_nodes` 的赋值式写法不一致，两处都在改传入的 DataFrame（副作用），只是路径不同。

**`explode_communities` 是一个「把社区归属铺平到实体上」的 merge。**

```
def explode_communities(communities: pd.DataFrame, entities: pd.DataFrame) -> pd.DataFrame:
    """Explode a list of communities into nodes for filtering."""
    community_join = communities.explode("entity_ids").loc[
        :, ["community", "level", "entity_ids"]
    ]
    nodes = entities.merge(
        community_join, left_on="id", right_on="entity_ids", how="left"
    )
    return nodes.loc[nodes.loc[:, COMMUNITY_ID] != -1]
```

三步：

1. **`explode("entity_ids")`** 把「一个社区一行、`entity_ids` 是列表」变成「一个 (社区, 实体) 一行」。
2. **`merge(left_on="id", right_on="entity_ids")`** 用实体 id 对上。
3. **`!= -1` 过滤掉没有社区的实体。**

第 3 步里的 `-1` 是**哨兵值**：`how="left"` 之后没有归属任何社区的实体 `community` 为 `NaN`，而 `NaN != -1` 为真——所以这一行过滤不掉 `NaN`。

**这是个真实的细节问题**：`nodes.loc[nodes[COMMUNITY_ID] != -1]` 想过滤「不属于任何社区」的实体，但 `NaN != -1` 是 `True`，于是这些实体会被保留，后续 `_prep_nodes` 时它们的 `community` 是 `NaN`，分组时会出问题（或得到一组 `community=NaN` 的「社区」）。

**如果本意是过滤 `NaN`，写法应该是 `dropna(subset=[COMMUNITY_ID])`。** 这里的 `!= -1` 只对「显式写了 -1」的情况有效——而 `-1` 正是 c01 讲的「顶层社区的父指针」。所以这行代码**看起来像在过滤父指针哨兵，实际想过滤无归属**。

**一个实体可能属于多个社区（不同层各一个），于是 `nodes` 会长于 `entities`。**

**这是 `explode_communities` 最关键的性质**：同一个实体会以「属于层 0 的社区 3」「属于层 1 的社区 1」等多行出现。后续 `build_local_context` 按 `(community, level)` 分组，于是**每个社区拿到的实体集合是「该层该社区内的实体」**。层次结构在这里才真正体现出来。

**`summarize_communities` 的循环是「逐层、层内并发」。**

```
levels = get_levels(nodes)

level_contexts = []
for level in levels:
    level_context = level_context_builder(
        pd.DataFrame(reports),
        community_hierarchy_df=community_hierarchy,
        local_context_df=local_contexts,
        level=level,
        tokenizer=tokenizer,
        max_context_tokens=max_input_length,
    )
    level_contexts.append(level_context)

for i, level_context in enumerate(level_contexts):
    local_reports = await derive_from_rows(
        level_context, run_generate, callbacks=NoopWorkflowCallbacks(),
        num_threads=num_threads, async_type=async_type,
        progress_msg=f"level {levels[i]} summarize communities progress: ",
    )
    reports.extend([lr for lr in local_reports if lr is not None])
```

**注意两个循环是分开的：先一次性算完所有层的上下文，再逐层生成报告。**

这个顺序不是笔误，是必需的：`level_context_builder` 的第一个参数是 `pd.DataFrame(reports)` ——**当前已生成的那些报告**。逐层处理时，上层的上下文可以包含下层的报告。而「先算完所有层的上下文」这个写法……**实际上破坏了这一点**。

因为第一个循环里 `reports` 还是空列表（`reports: list[CommunityReport | None] = []`），所以 `pd.DataFrame([])` 是空表。**每层的上下文都是在「还没有任何下层报告」的假设下构建的**。真正的「用下层报告当上层输入」需要「算一层上下文 → 生成该层报告 → 用新报告算下一层上下文」的交错顺序，而代码先把所有上下文算完再统一生成。

**这是一处真实的实现与意图不符**：`build_level_context` 接收 `reports` 参数正是为了这个目的，但调用顺序让它恒为空。

**`community_hierarchy` 是「父子关系」的显式化。**

```
community_hierarchy = (
    communities
    .explode("children")
    .rename({"children": "sub_community"}, axis=1)
    .loc[:, ["community", "level", "sub_community"]]
).dropna()
```

**注意 `rename({"children": "sub_community"}, axis=1)`** ——pandas 的 `rename` 这里用的是位置参数风格的 `axis=1`，而不是 `columns=`。两者等价，但这种写法在新版 pandas 里已不推荐。

`dropna()` 去掉叶子社区（`children` 为空数组时 explode 产出 `NaN`）。

**`reports.extend([lr for lr in local_reports if lr is not None])`** ——LLM 调用失败的社区被静默跳过。于是**最终报告表可能比社区表少行**，而下游（检索）会以为「这个社区没有报告」。这是又一处「静默跳过」。

**`finalize_community_reports` 是三步：merge 社区字段、造 id、裁列。**

```
def finalize_community_reports(reports: pd.DataFrame, communities: pd.DataFrame) -> pd.DataFrame:
    """All the steps to transform final community reports."""
    community_reports = reports.merge(
        communities.loc[:, ["community", "parent", "children", "size", "period"]],
        on="community",
        how="left",
    )

    community_reports["community"] = community_reports["community"].astype(int)
    community_reports["human_readable_id"] = community_reports["community"]
    community_reports["id"] = community_reports.apply(
        lambda row: gen_sha512_hash(row, ["full_content"]), axis=1
    )

    return community_reports.loc[
        :,
        COMMUNITY_REPORTS_FINAL_COLUMNS,
    ]
```

三处：

1. **`merge` 只取社区的五个字段**（`community` / `parent` / `children` / `size` / `period`），`title` / `entity_ids` 等不带。报告不需要知道社区的实体列表（那是检索时按 `community_id` 去查的）。
2. **`id` 是 `gen_sha512_hash(row, ["full_content"])`** ——按内容算。于是**重跑时若报告内容不变，id 就不变**。这与实体/社区的 `uuid4()` 形成对比，也是一个有意义的差别：报告是「派生的文本产物」，内容相同即同一份；实体是「从文本里抽出的东西」，重抽就是新的。
3. **`how="left"` 保留报告行**（以报告为准）。若某社区没有报告（LLM 失败被跳过），它不会出现在报告表里；反过来若报告存在但社区表里找不到（不该发生），那行的 `parent` 等为 `NaN`。

`human_readable_id = community` 又是「簇号当短名」，与社区表一致。

## 关键取舍

**源码事实：** `create_communities` 按层循环计算社区内关系（每层 `relationships` × 该层社区两次 inner join，取 `community_x == community_y`），注释说明原因是「keep intermediate DataFrames small」；中间表靠 pandas 的 `_x` / `_y` 列名后缀消歧，后续 `rename` 回原名。

**源码事实：** 社区表的 `relationship_ids` 只含**社区内**边（两端在同一社区）；`text_unit_ids` 由 `explode` + `sorted(set(...))` 得到，排序是为了可复现。

**源码事实：** `children` 由 `groupby("parent")` 反推后 `merge(left_on="community", right_on="parent")` 生成，缺失填 `[]`（用 `isinstance(x, np.ndarray)` 判定）；`parent` 与 merge 进来的右表键列同名，代码未做 rename 处理。

**源码事实：** 社区 `id` 是 `uuid4()`（每次重跑变），`human_readable_id` 直接等于簇号 `community`（每层内唯一、跨层不唯一），`title` 是 `"Community " + 簇号`，`period` 是当天 UTC 日期（索引生成日期，不是内容时间范围），`size` 是 `entity_ids` 长度。

**源码事实：** `_sanitize_row` 把 `np.ndarray` / `np.integer` / `np.floating` 转成原生类型供序列化；未处理 `np.bool_`，不递归。

**源码事实：** `explode_communities` 用 `!= -1` 过滤「无社区归属」的实体，但 `how="left"` 产生的 `NaN != -1` 为真，因此这些实体**不会**被过滤掉——若本意是排除它们，应使用 `dropna`。

**源码事实：** `_prep_nodes` / `_prep_edges` / `_prep_claims` 用 `.loc[:, [短名, 内容, 量化指标]].to_dict(orient="records")` 造 `node_details` / `edge_details` / `claim_details` 列；`degree` / `combined_degree` 进入详情，是 LLM 判断重要度的唯一依据；缺失描述填 `"No Description"` 而非空串。

**源码事实：** `summarize_communities` 先把**所有层**的上下文一次性构建完（此期间 `reports` 恒为空列表），再逐层生成报告；因此 `level_context_builder` 收到的「已有报告」数据帧恒为空，上层报告无法利用下层报告作为输入。

**源码事实：** 生成失败的社区（`_generate_report` 返回 `None`）被 `[lr for lr in local_reports if lr is not None]` 静默丢弃，最终报告表可能少于社区表。

**源码事实：** `finalize_community_reports` 从社区表只取 `community` / `parent` / `children` / `size` / `period` 五列 merge；`id` 用 `gen_sha512_hash(row, ["full_content"])`（内容决定，重跑稳定），`human_readable_id` 等于簇号。

**架构推论：** 社区的 `relationship_ids` 只含内部边，是一个**有意的信息裁剪**：跨社区的边不属于任何单个社区，若硬塞进去会让两个社区都声称拥有它。代价是「这个社区与外界怎么连」这个问题的答案不在社区表里——需要时得现算。

**架构推论：** 「先算完所有上下文再逐层生成」这个顺序让层次结构的最大优势落空了。层次设计的价值在于**上层可以在下层的结论上做归纳**，而当前实现让每层都从原始实体边重建上下文。这不影响正确性，但直接推高了上层报告的成本（输入规模与下层无关地增长）与质量风险（预算内只能塞下部分实体）。**交错顺序（算一层 → 生成一层 → 再算下一层）才能兑现层次的价值**，而代码的表结构（`level_contexts` 先全量后使用）把这个可能封死了。

**架构推论：** 静默跳过失败项（LLM 返回 `None` 就不写这行报告）在这里是危险的：报告的缺失在检索期表现为「这个社区没有信息」，而不是「这次索引有残缺」。主核里同类场景（批量构建索引项、部分失败）都应当把失败**记入产物**（一个 `error` 列或一张失败表），让「缺」与「空」可区分。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/index/workflows/create_communities.py` | `create_communities` | 聚类结果 + 实体 + 关系 → 社区表（按层算社区内边、反推 children、造 id/title/period/size） |
| `packages/graphrag/graphrag/index/workflows/create_communities.py` | `_sanitize_row` | numpy 标量/数组 → 原生类型（供表序列化） |
| `packages/graphrag/graphrag/index/workflows/create_community_reports.py` | `create_community_reports` | 组装五段流水线：explode → prep → local context → summarize → finalize |
| `packages/graphrag/graphrag/index/workflows/create_community_reports.py` | `_prep_nodes` / `_prep_edges` / `_prep_claims` | 造 `*_details` 列（短名 + 内容 + 度数为提示词输入） |
| `packages/graphrag/graphrag/index/operations/summarize_communities/explode_communities.py` | `explode_communities` | 社区实体列表 → 实体所属社区长表；`!= -1` 过滤（对 NaN 无效） |
| `packages/graphrag/graphrag/index/operations/summarize_communities/summarize_communities.py` | `summarize_communities` | 逐层构建上下文 + 层内并发生成报告；失败项静默丢弃 |
| `packages/graphrag/graphrag/index/operations/summarize_communities/graph_context/context_builder.py` | `build_local_context` / `build_level_context` | 按 token 预算裁剪社区上下文 |
| `packages/graphrag/graphrag/index/operations/finalize_community_reports.py` | `finalize_community_reports` | merge 社区字段 + 按 `full_content` 算 sha512 + 白名单裁列 |

## 参考

- [graphrag 源码：create_communities.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/workflows/create_communities.py)
- [graphrag 源码：create_community_reports.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/workflows/create_community_reports.py)
- [graphrag 文档：索引输出——社区报告](https://microsoft.github.io/graphrag/index/outputs/)
- [graphrag 源码仓库](https://github.com/microsoft/graphrag)

## 与主核 / AI 的关系

社区报告生成在主核（多模态数据库内核）里对应的是**物化视图与统计信息的构建**：都是「把底层数据汇总成一个可直接查询的派生结构」。四条经验。

**第一，「汇总单元」必须明确它含什么、不含什么。** 社区只收内部边，跨社区边不属于任何社区。这个裁剪是对的，但必须写下来——主核做物化视图时同理：**每个物化视图的过滤条件就是它的语义边界**，边界不清会让使用者以为「查这个视图就等于查了全部」。任何「有意的信息裁剪」都应当能回答「被裁掉的那部分去哪查」。

**第二，层次的复利依赖交错执行，而不是先全量准备。** 这里的层次结构建好了（`parent` / `children` / `level` 都有），但报告生成没有利用它——因为上下文是在「所有报告都还没生成」时算的。主核里做多级汇总（日 → 月 → 年、页 → 区 → 表）时同理：**上层必须基于下层的产出增量构建，否则层次只是多了一层元数据，没有省任何计算也没有提升任何质量。**

**第三，`NaN` 与哨兵值不是一回事，过滤条件写错会静默失效。** `nodes[community != -1]` 对 `NaN` 为真，于是「不属于任何社区」的实体被放行。主核里凡是「用哨兵值（-1 / 0 / 空串）表示缺失」的地方，都要同时处理真正的 `NULL`——**两者在比较语义上完全不同**，`x != -1` 永远无法筛掉 `NULL`。

**第四，批量生成中的单项失败必须落盘。** LLM 返回 `None` 时那行报告被丢掉，于是产物表里「没有这个社区」与「这个社区的报告生成失败了」不可区分。主核做批量索引构建、批量统计刷新时同理：**失败要么让整批回滚，要么写进一张可查的失败清单**；静默跳过会让「数据不全」伪装成「数据本来就这么少」。
