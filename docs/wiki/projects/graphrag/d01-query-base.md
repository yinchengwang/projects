---
project: graphrag
feature: query-base
title: 查询引擎契约与适配
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/query/structured_search/base.py
    symbols: [BaseSearch, SearchResult]
  - path: packages/graphrag/graphrag/query/factory.py
    symbols: [get_local_search_engine, get_global_search_engine]
  - path: packages/graphrag/graphrag/query/indexer_adapters.py
    symbols: [read_indexer_entities, read_indexer_reports, read_indexer_text_units]
---

# 查询引擎契约与适配

## 一句话职责

`BaseSearch` 定下四种检索模式共同的两个方法（`search` 与 `stream_search`）与一个统一的结果信封 `SearchResult`；`factory.py` 把「配置 + 数据对象」装配成引擎；`indexer_adapters.py` 则是磁盘上那些 DataFrame 与检索期对象之间的唯一通道——包括一处**在读取时就完成的社区层级裁剪**。

## 机制是什么

graphrag 的索引产物是一堆 parquet/DataFrame：`entities`、`relationships`、`text_units`、`communities`、`community_reports`、`covariates`。而检索期要用的是 Python 对象（`Entity`、`Relationship`……）。中间这一层看起来只是「读文件」，实际要解决三件事：

1. **同一个实体属于多个社区**。`entities` 表里没有社区列，`communities` 表里有 `entity_ids` 列表。所以「实体 → 社区」是**多对多**，要靠 explode + groupby 反向聚合。

2. **社区有多层，而查询只该看某一层以上**。c01 讲过层次 Leiden 产出的父子嵌套结构。若把全部层级的报告都塞给检索，等于把同一话题的多个粒度重复喂给模型。所以适配层要按 `community_level` 过滤。

3. **「该给哪些社区的报告」有两种完全不同的答案**。默认是「每个实体所属的**最深**社区」（roll up），另一种是「让 LLM 逐层挑」（d06 的动态选择）。`read_indexer_reports` 用一个 `dynamic_community_selection` 布尔在这两者之间切换。

第三条是这一页最有意思的地方：**它把「用不用 LLM 选社区」这件事编码成了「读数据时的一个分支」**，而不是查询期的一个可选步骤。

## 怎么实现的

### `SearchResult`：一个已经很厚的信封

```python
@dataclass
class SearchResult:
    response: str | dict[str, Any] | list[dict[str, Any]]
    context_data: str | list[pd.DataFrame] | dict[str, pd.DataFrame]
    # actual text strings that are in the context window, built from context_data
    context_text: str | list[str] | dict[str, str]
    completion_time: float
    # total LLM calls and token usage
    llm_calls: int
    prompt_tokens: int
    output_tokens: int
    # breakdown of LLM calls and token usage
    llm_calls_categories: dict[str, int] | None = None
    prompt_tokens_categories: dict[str, int] | None = None
    output_tokens_categories: dict[str, int] | None = None
```

四处：

1. **三个字段都是「标量或容器」的联合类型**。`response` 可以是字符串、字典或字典列表——因为 global 的 map-reduce 会产生多个中间答案。`context_data` 同理（local 是 `dict[str, DataFrame]`，global 是 `list[DataFrame]`）。

2. **`context_data` 与 `context_text` 是同一份内容的两种形态**：前者是结构化记录（用于审计/调试），后者是**真正进上下文窗口的字符串**。注释明确写了 `context_text` 是「built from `context_data`」。所以两者可能不一致——截断发生在渲染成文本时。

3. **六个统计字段分两层：总量 + 分类明细**。而 `_categories` 三个是可选的（`None` 默认）。分类键是**阶段名**：local search 用 `build_context` / `response`，global search 用 `map` / `reduce`。

4. **`llm_calls` 是整数而 `_categories` 是字典** —— 所以「总量」是明细之和。这要求每个调用点都记得记录明细，否则总量与明细会不一致。**而 local search 的异常路径就违反了这一点**（见下）。

### `BaseSearch`：泛型绑在上下文构建器上

```python
T = TypeVar(
    "T",
    GlobalContextBuilder,
    LocalContextBuilder,
    DRIFTContextBuilder,
    BasicContextBuilder,
)

class BaseSearch(ABC, Generic[T]):
    def __init__(self, model, context_builder: T, tokenizer=None,
                 model_params=None, context_builder_params=None):
        self.model = model
        self.context_builder = context_builder
        self.tokenizer = tokenizer or model.tokenizer
        self.model_params = model_params or {}
        self.context_builder_params = context_builder_params or {}
```

四处：

1. **`TypeVar` 用四个具体类型做上界**（而不是 `ContextBuilder` 基类）。所以静态检查器知道 `GlobalSearch` 只能配 `GlobalContextBuilder`。这比宽松的基类上界更严，代价是新增模式时要改这个 `TypeVar`。

2. **`self.tokenizer = tokenizer or model.tokenizer`** —— tokenizer 默认从模型取。所以「模型与分词器必须是同一家」这个约束是靠默认值保证的，但可以被显式覆盖。而**覆盖会导致 token 计数与模型实际分词不一致**——统计字段就不可信了。

3. **`model_params` 与 `context_builder_params` 分开放**，且都在构造时固定。所以「同一个引擎多次查询用同一套参数」是默认语义；要变参数得新建引擎或走 `search(**kwargs)`。

4. **两个抽象方法都是 `async`**，且 `stream_search` 是**异步生成器**：

```python
@abstractmethod
async def stream_search(self, query, conversation_history=None) -> AsyncGenerator[str, None]:
    """Stream search for the given query."""
    yield ""  # This makes it an async generator.
    msg = "Subclasses must implement this method"
    raise NotImplementedError(msg)
```

**`yield ""` 那一行的存在理由值得记**：一个函数要成为**异步生成器**，函数体里必须有 `yield`。而 `raise NotImplementedError` 本身不会让函数变成生成器——所以若只写 `raise`，子类实现 `async def stream_search` 与基类签名的 `AsyncGenerator` 返回类型就不匹配（基类会变成协程）。

**这是「用一句无副作用的 `yield` 来满足语法」的实例**，注释也写明了这一点。

### 四个工厂函数：装配的差异

`factory.py` 有四个 `get_*_search_engine`，签名本身就能看出四种模式的输入差异：

| 工厂 | 输入 | 关键 |
|---|---|---|
| `get_local_search_engine` | reports + text_units + entities + relationships + covariates + 描述嵌入 | 五类数据全要 |
| `get_global_search_engine` | reports + entities + communities | **不要 text_units、不要 relationships** |
| `get_drift_search_engine` | reports + text_units + entities + relationships + 描述嵌入 | 与 local 相同（因为它内部包了 local） |
| `get_basic_search_engine` | 只要 text_units + 其嵌入 | **完全不用图** |

**这张表就是 D 组的主线**：global 只用「图的高层产物」（社区报告），local 要「图 + 原文」，basic 只要原文。

而 `get_local_search_engine` 的 `context_builder_params` 是一份很长的显式清单：

```python
context_builder_params={
    "text_unit_prop": ls_config.text_unit_prop,
    "community_prop": ls_config.community_prop,
    "conversation_history_max_turns": ls_config.conversation_history_max_turns,
    "conversation_history_user_turns_only": True,
    "top_k_mapped_entities": ls_config.top_k_entities,
    "top_k_relationships": ls_config.top_k_relationships,
    "include_entity_rank": True,
    "include_relationship_weight": True,
    "include_community_rank": False,
    "return_candidate_context": False,
    "embedding_vectorstore_key": EntityVectorStoreKey.ID,
    "max_context_tokens": ls_config.max_context_tokens,
}
```

四处：

1. **两个比例来自配置**（`text_unit_prop` / `community_prop`），而**第三个是从它们算出来的**（`1 - community_prop - text_unit_prop`，在 `build_context` 里）。

2. **几个布尔是硬编码的**：`include_entity_rank=True`、`include_relationship_weight=True`、`include_community_rank=False`、`return_candidate_context=False`。

   **`include_community_rank=False` 值得注意**：local search 的社区上下文不带 rank，而 global search 的 `include_community_rank=True`。所以「社区排名」只在 global 里是一个信号——这符合两者的用途（global 靠排名筛报告，local 靠实体匹配筛报告）。

3. **`conversation_history_user_turns_only=True` 硬编码** —— 所以 local search 的历史上下文只带**用户问句**，不带助手的回答。这与 d07 会讲的「历史打分」有关。

4. **`embedding_vectorstore_key` 出现两次**（构造器一次、参数一次）且注释重复了两遍。这是「同一个值要在两处出现」的冗余——若只传一处，两处会不一致。

5. **注释里有一条很实用的建议**：「change this based on the token limit you have on your model (if you are using a model with 8k limit, a good setting could be 5000)」。**把配置经验值写进代码注释**，因为配置项名称本身不表达「该设多少」。

`get_global_search_engine` 的动态社区选择是条件装配：

```python
dynamic_community_selection_kwargs = {}
if dynamic_community_selection:
    # TODO: Allow for another llm definition only for Global Search to leverage -mini models
    dynamic_community_selection_kwargs.update({
        "model": model,
        "tokenizer": tokenizer,
        "keep_parent": gs_config.dynamic_search_keep_parent,
        "num_repeats": gs_config.dynamic_search_num_repeats,
        "use_summary": gs_config.dynamic_search_use_summary,
        "concurrent_coroutines": config.concurrent_requests,
        "threshold": gs_config.dynamic_search_threshold,
        "max_level": gs_config.dynamic_search_max_level,
        "model_params": {**model_params},
    })
```

三处：

1. **空字典 + 条件更新** —— 所以未启用时传的是 `{}`，`GlobalCommunityContext` 拿到空参就不做动态选择。
2. **`model` 与 `tokenizer` 是同一个** —— 而 TODO 注释说明了这是个已知限制：「Allow for another llm definition only for Global Search to leverage -mini models」。
3. **七个参数**覆盖了动态选择的全部行为（保留父社区、重复次数、用摘要、并发、阈值、最大层级）。

`get_drift_search_engine` 的独特之处是**它把 `config.drift_search` 整个传给 context builder**：

```python
context_builder=DRIFTSearchContextBuilder(
    model=chat_model, text_embedder=embedding_model,
    entities=entities, relationships=relationships, reports=reports,
    entity_text_embeddings=description_embedding_store,
    text_units=text_units,
    local_system_prompt=local_system_prompt, reduce_system_prompt=reduce_system_prompt,
    config=config.drift_search, response_type=response_type,
)
```

**所以 DRIFT 的配置没有被拆成 `context_builder_params` 字典，而是整块传进去。** 这与 local/global 的做法不同——因为它们把参数「摊平」成了 `context_builder_params` 的键，而 DRIFT 的 context builder 直接持有配置对象。

**这是一处风格不一致**，但它反映了实际差异：DRIFT 的 context builder 需要用配置驱动的多轮循环，摊平成字典会让它拿不到结构。

`get_basic_search_engine` 只传三个参数（`k`、`max_context_tokens`、`embedding_vectorstore_key`）——**最简单的那个，因为它不用图**。

### 适配层：`read_indexer_entities` 的多对多反转

```python
def read_indexer_entities(entities, communities, community_level):
    community_join = communities.explode("entity_ids").loc[:, ["community", "level", "entity_ids"]]
    nodes_df = entities.merge(community_join, left_on="id", right_on="entity_ids", how="left")

    if community_level is not None:
        nodes_df = _filter_under_community_level(nodes_df, community_level)

    nodes_df = nodes_df.loc[:, ["id", "community"]]
    nodes_df["community"] = nodes_df["community"].fillna(-1)
    # group entities by id and degree and remove duplicated community IDs
    nodes_df = nodes_df.groupby(["id"]).agg({"community": set}).reset_index()
    nodes_df["community"] = nodes_df["community"].apply(lambda x: [str(int(i)) for i in x])
    final_df = nodes_df.merge(entities, on="id", how="inner").drop_duplicates(subset=["id"])
```

五处：

1. **`explode("entity_ids")`** —— 把「一个社区一行、`entity_ids` 是列表」变成「一个 (社区, 实体) 一行」。这是多对多展开的关键一步。

2. **`merge(..., how="left")`** —— 实体左连接社区。所以**不属于任何社区的实体不会被丢掉**（它的 `community` 会是 `NaN`）。

3. **`fillna(-1)`** —— 用 `-1` 表示「无社区」。这与 c01 里父指针用 `-1` 是同一个约定（同一个哨兵值在整个项目里表示「无」）。

   **但这里有个隐患**：`-1` 进 `set` 后会被 `int(-1)` 转成 `"-1"` 字符串，所以一个「无社区实体」的 `community_ids` 会是 `["-1"]` 而**不是空列表**。下游若判 `if entity.community_ids:` 会得到真。

4. **`agg({"community": set})`** —— 同一个实体在多层里属于多个社区，用 `set` 收集。注释说「group entities by id and degree and remove duplicated community IDs」——但 `by` 里只有 `id`，**没有 `degree`**。这是一处代码与注释的不一致（注释提到 degree，实际没按它分组）。

5. **`[str(int(i)) for i in x]`** —— 转成字符串列表。而 `int()` 会抛错若 `set` 里有 `NaN`——但 `fillna(-1)` 已处理，所以安全。**不过这依赖 `fillna` 在 `groupby` 之前**，顺序不能动。

6. **`drop_duplicates(subset=["id"])`** —— 因为 `merge` 可能产生重复行（一个实体在多行社区里出现多次，虽然已聚合过）。这一步是保险。

### `read_indexer_reports`：两套选社区的策略

```python
def read_indexer_reports(final_community_reports, final_communities,
                         community_level, dynamic_community_selection=False):
    reports_df = final_community_reports
    nodes_df = final_communities.explode("entity_ids")

    if community_level is not None:
        nodes_df = _filter_under_community_level(nodes_df, community_level)
        reports_df = _filter_under_community_level(reports_df, community_level)

    if not dynamic_community_selection:
        # perform community level roll up
        nodes_df.loc[:, "community"] = nodes_df["community"].fillna(-1)
        nodes_df.loc[:, "community"] = nodes_df["community"].astype(int)

        nodes_df = nodes_df.groupby(["title"]).agg({"community": "max"}).reset_index()
        filtered_community_df = nodes_df["community"].drop_duplicates()

        reports_df = reports_df.merge(filtered_community_df, on="community", how="inner")

    return read_community_reports(df=reports_df, id_col="id", short_id_col="community")
```

五处：

1. **`community_level` 过滤同时作用于两个表** —— 社区表与报告表都要按 `level <= community_level` 裁。所以传 `None` 表示「不裁」。

   **`_filter_under_community_level` 用的是 `df[df.level <= community_level]`** —— 注意是 `<=`，所以「层级数字越大越细」。而 c01 讲过 `level == 0` 是最粗那层。所以 `community_level=2` 意味着「取第 0、1、2 层」，即「最多细到第 2 层」。

2. **roll up 是「每个 title 取最大 community 号」** —— `groupby(["title"]).agg({"community": "max"})`。

   **为什么按 `title` 而不是按实体？** 因为 `explode` 后一个 title（社区名）对应多个实体行，而同一 title 在不同层级有不同的 community id。取 `max` 的意思是「取该 title 的最细层级」。

   **这是一个近似**：它假设「同一个 title 的社区 id 随层级单调递增」，而 c01 没有保证这一点（层次 Leiden 的簇 id 是算法分配的序号）。所以 `max` 不一定等于「最深层」——**这是一处依赖数据布局的假设**。

3. **`drop_duplicates()` 后 `merge(..., how="inner")`** —— 只保留被选中的社区报告。所以「未被任何实体 title 引用的社区」会被丢掉。

4. **`dynamic_community_selection=True` 时完全跳过 roll up** —— 所以**全部层级的报告都返回**，由 d06 的 `DynamicCommunitySelection` 在查询期用 LLM 逐层挑。

   **这就是前面说的「把策略编码成读取分支」**：两条路返回的报告集完全不同（一个是每 title 一份，一个是全量），而下游（`GlobalCommunityContext`）不需要知道它拿到的是哪一种。

5. **`nodes_df.loc[:, "community"] = ...` 用 `.loc`** —— 避免 pandas 的 `SettingWithCopyWarning`。而 `reports_df = final_community_reports` 是**引用**，不是复制——所以后面的 `merge` 会产生新对象，但 `_filter_under_community_level` 返回的也是新对象。所以原来的 DataFrame 不会被改。

   **但 `nodes_df = final_communities.explode(...)` 是新建的**，所以对它的 `.loc` 赋值安全。

### `read_indexer_text_units`：最简单的那个，而它的 `None` 有含义

```python
def read_indexer_text_units(final_text_units: pd.DataFrame) -> list[TextUnit]:
    """Read in the Text Units from the raw indexing outputs."""
    return read_text_units(
        df=final_text_units,
        # expects a covariate map of type -> ids
        covariates_col=None,
    )
```

**`covariates_col=None` 旁边的注释**说明了为什么是 `None`：「expects a covariate map of type -> ids」。即 `read_text_units` 支持「一个文本单元关联多个协变量」的列，而**检索期不需要这个关联**（协变量由 `read_indexer_covariates` 单独读，再按 `text_unit_ids` 反向索引）。

所以这一行的 `None` 是一个**明确的能力裁剪**：`read_text_units` 能做的事比检索需要得多，这里只取需要的那部分。

### 适配层顶部的自我批评

```python
"""Indexing-Engine to Query Read Adapters.

The parts of these functions that do type adaptation, renaming, collating, etc. should
eventually go away. Ideally this is just a straight read-through into the object model.
"""
```

**模块文档直接承认这一层是技术债**：「should eventually go away. Ideally this is just a straight read-through into the object model」。

**这是一个很有信息量的自我评价**：它说明「索引产物形状」与「检索期对象模型」本该一致，而现在需要适配是因为两者独立演化。而适配层的存在价值在于**它把不一致集中在一处**——若没有它，每个检索模式都要自己处理 DataFrame。

## 关键取舍

**源码事实：** `SearchResult` 六个统计字段分两层（总量 + `_categories` 明细），三个 `_categories` 可选且键是阶段名（local 用 `build_context`/`response`，global 用 `map`/`reduce`）；`llm_calls` 是明细之和，所以每个调用点都必须记录明细。

**源码事实：** `context_data`（结构化记录）与 `context_text`（进上下文窗口的字符串）是同一内容的两种形态，注释写明后者「built from」前者，所以两者可能因截断而不一致。

**源码事实：** `BaseSearch` 的 `TypeVar` 用四个具体 builder 类型做上界（而非共同基类），所以模式与 builder 的配对在静态层面被约束到四种。

**源码事实：** `tokenizer` 默认取 `model.tokenizer`，显式覆盖会让 token 计数与模型实际分词不一致，统计字段随之不可信。

**源码事实：** `stream_search` 基类里有 `yield ""`，理由是要让函数成为**异步生成器**（否则返回类型 `AsyncGenerator` 与协程不符），注释写明「This makes it an async generator」。

**源码事实：** 四个工厂的输入差异即四种模式的分工：global 只要 reports + entities + communities（**不要 text_units、不要 relationships**），local 要五类全，drift 与 local 同，basic 只要 text_units + 其嵌入。

**源码事实：** local 的 `context_builder_params` 里几个布尔硬编码（`include_entity_rank=True`、`include_relationship_weight=True`、`include_community_rank=False`、`return_candidate_context=False`、`conversation_history_user_turns_only=True`），而 global 的 `include_community_rank=True`——社区排名只在 global 里是信号。

**源码事实：** `embedding_vectorstore_key` 在 `get_local_search_engine` 里出现两次（构造器 + params）且注释重复两遍，只传一处会导致两处不一致。

**源码事实：** `get_drift_search_engine` 把整个 `config.drift_search` 传给 context builder 而不是摊平成 `context_builder_params`——与 local/global 风格不一致，因为 DRIFT 的 builder 需要结构化的多轮配置。

**源码事实：** `read_indexer_entities` 用 `explode` + `merge(how="left")` + `fillna(-1)` + `agg({"community": set})` 把多对多反转成「每个实体一组社区 id」；`-1` 哨兵会进 `set` 并变成字符串 `"-1"`，所以「无社区实体」的 `community_ids` 是 `["-1"]` 而非空列表。

**源码事实：** `read_indexer_entities` 的注释说「group entities by id and degree」，而 `groupby(["id"])` 里只有 `id`，没有 `degree`——注释与代码不一致。

**源码事实：** `read_indexer_reports` 的两条路：默认做 roll up（`groupby(["title"]).agg({"community": "max"})` 取每个 title 最细层级的社区，再 inner merge 只留被引用的报告）；`dynamic_community_selection=True` 时**完全跳过 roll up**，返回全部层级的报告供查询期 LLM 挑选。

**源码事实：** roll up 用 `"max"` 取社区号，隐含假设「同一 title 的社区 id 随层级单调递增」，而 c01 里簇 id 是算法分配的序号，未保证此性质。

**源码事实：** `_filter_under_community_level` 用 `level <= community_level`，配合「`level == 0` 最粗」的约定，`community_level=2` 意为「最多细到第 2 层」。

**源码事实：** `read_indexer_text_units` 传 `covariates_col=None`，注释说明「expects a covariate map of type -> ids」——检索期不需要文本单元到协变量的关联（协变量单独读、反向按 `text_unit_ids` 索引）。

**源码事实：** 适配层模块文档自认是技术债：「The parts of these functions that do type adaptation, renaming, collating, etc. should eventually go away. Ideally this is just a straight read-through into the object model.」

**架构推论：** `SearchResult` 把 token 统计做成「总量 + 分阶段明细」双份，说明这个框架把**成本可观测性**当成一等需求。而明细的键是阶段名而不是固定枚举——所以不同模式可以有不同的分解粒度。代价是「总量 = 明细之和」这条不变量靠调用点自觉维护，而 local search 的异常路径恰好违反了它（`llm_calls=1` 而 `llm_calls_categories=None`）。

**架构推论：** 「用 `dynamic_community_selection` 一个布尔在**读取时**分叉」是个有争议的设计。好处是下游不必知道报告集是怎么来的；坏处是「读了多少数据」取决于一个查询期策略，从而让「索引加载」这个本该纯读取的操作带上了行为分支。若将来要支持「同一次加载、两种策略都试」，这个设计就不成立了。

**架构推论：** 适配层那句自我批评（「should eventually go away」）正是**防腐层**的典型处境。它的价值不在于优雅，而在于把「索引产物的形状」与「检索期的对象模型」之间的全部不一致收敛到一个文件里。一旦两者收敛，这个文件就该消失——而它自己在文档里预告了这一点。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/query/structured_search/base.py` | `SearchResult` | 统一结果信封（答案 + 上下文 + 成本明细） |
| `packages/graphrag/graphrag/query/structured_search/base.py` | `BaseSearch` | 两抽象方法的泛型基类 |
| `packages/graphrag/graphrag/query/structured_search/base.py` | `BaseSearch.stream_search` | 用 `yield ""` 满足异步生成器语法 |
| `packages/graphrag/graphrag/query/factory.py` | `get_local_search_engine` | 五类数据全装配 + 比例参数 |
| `packages/graphrag/graphrag/query/factory.py` | `get_global_search_engine` | 报告/实体/社区装配 + 动态选择可选参数 |
| `packages/graphrag/graphrag/query/factory.py` | `get_drift_search_engine` | 整块传 `config.drift_search` |
| `packages/graphrag/graphrag/query/factory.py` | `get_basic_search_engine` | 仅文本单元与其嵌入 |
| `packages/graphrag/graphrag/query/indexer_adapters.py` | `read_indexer_entities` | 多对多社区反转 + `-1` 哨兵 |
| `packages/graphrag/graphrag/query/indexer_adapters.py` | `read_indexer_reports` | 两条选社区路径（roll up / 全量） |
| `packages/graphrag/graphrag/query/indexer_adapters.py` | `read_indexer_text_units` | 直读，裁掉协变量关联 |
| `packages/graphrag/graphrag/query/indexer_adapters.py` | `read_indexer_relationships` | 用 `combined_degree` 当 rank |
| `packages/graphrag/graphrag/query/indexer_adapters.py` | `read_indexer_communities` | 重建社区层级 + 缺报告告警 |
| `packages/graphrag/graphrag/query/indexer_adapters.py` | `_filter_under_community_level` | `level <= community_level` 过滤 |
| `packages/graphrag/graphrag/query/indexer_adapters.py` | `read_indexer_report_embeddings` | 从向量库回填报告嵌入（失败置 `None`） |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：查询](https://microsoft.github.io/graphrag/query/overview/)
- [graphrag 源码：query/factory.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/query/factory.py)
- [Python 文档：typing.Protocol 与 TypeVar 上界](https://docs.python.org/3/library/typing.html#typing.TypeVar)

## 与主核 / AI 的关系

适配层与主核（多模态数据库内核）的**元组解码层**（`heap_getattr` / 反序列化）是同一类角色：把磁盘上的字节变成内存里的结构。但两者的**成本结构完全相反**：主核的解码是查询路径上最热的一段（所以用固定偏移、避免逐字段解析、有 `tupleDesc` 缓存），而这里是一次性的全量加载（`list[Entity]` 把所有实体读进内存）。

**这个差别决定了它们的优化方向**：主核关心「每行几个纳秒」，这里关心「能不能装进内存」。所以 graphrag 的检索是**进程内内存检索**，而主核是**按需读页**。判据是**数据规模与访问模式**：全量随机访问 → 内存；点查 + 范围扫 → 页缓存。

第二处对照是 `read_indexer_reports` 的 roll up，与主核的**视图展开**（`pull_varnos` / 子查询扁平化）。两者都是「把多层结构压成一层」：roll up 是「每个 title 取一个社区」，视图展开是「把嵌套查询拼成一个」。差别在**是否有损**：视图展开是等价的（可证明），roll up 是有损的（丢掉其他层级的报告）。**所以 roll up 需要 `dynamic_community_selection` 这个逃生口**，而视图展开不需要。

第三处是「适配层自认是技术债」这一点与主核的**行格式版本兼容**。主核也有类似的层（`HeapTupleHeader` 的不同版本、`attalign` 补齐），但它永远不会消失——因为磁盘格式与内存结构必须不同（对齐、紧凑）。而 graphrag 的适配层理论上可以消失，因为它两侧都是「Python 对象」这种柔性结构。**判据是「两侧是否真的可以同形」**：同形则该层是临时债，不同形则是永久层。

对 agent 工程的实际意义在于：查询层的设计决定了「加一种检索模式有多贵」。这份实现给出的三条经验可直接用：**结果信封要含成本明细而不只是总量**（否则无法定位是哪个阶段贵）、**把「用不用 LLM 做选择」这类策略显式建模**（哪怕表现成一个读取分支）、以及**适配层要集中不要分散**（每种模式各自处理 DataFrame 会让修一处变成修四处）。
