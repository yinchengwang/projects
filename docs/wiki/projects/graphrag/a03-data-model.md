---
project: graphrag
feature: data-model
title: 索引产物数据模型
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/data_model/entity.py
    symbols: [Entity]
  - path: packages/graphrag/graphrag/data_model/relationship.py
    symbols: [Relationship]
  - path: packages/graphrag/graphrag/data_model/community_report.py
    symbols: [CommunityReport]
  - path: packages/graphrag/graphrag/data_model/text_unit.py
    symbols: [TextUnit]
---

# 索引产物数据模型

## 一句话职责

六个 dataclass 描述索引落盘后的六类产物——文档、文本单元、实体、关系、社区、社区报告——每个都有 `from_dict` 从一行表数据（或一个 dict）还原；它们的继承关系只有两条：**有名字的东西继承 `Named`（id + short_id + title），没名字的东西继承 `Identified`（只有 id + short_id）**，而这条分界线精确地对应「这个产物在提示词里怎么被称呼」。

## 机制是什么

这套模型最重要的特征是**它们不是数据库 schema，而是「读出来之后的视图」**。

真正的落盘格式是 `schemas.py` 里那八个 `ENTITIES_FINAL_COLUMNS` / `TEXT_UNITS_FINAL_COLUMNS` 列表——一张表长什么样由列名列表决定，而列的顺序就是 parquet/CSV 里字段的顺序。dataclass 只在需要「按字段访问」时被构造出来（检索、抽取、报表拼装），它的 `from_dict` 就是把字典键映射成属性。

于是有一个不该混淆的层次：

| 层 | 由什么定义 | 谁在用 |
|---|---|---|
| 磁盘格式 | `schemas.py` 的列名常量与 `ENTITIES_FINAL_COLUMNS` / `TEXT_UNITS_FINAL_COLUMNS` | 表的写入与读取、`row_transformers` 的类型收敛 |
| 内存视图 | `data_model/*.py` 的 dataclass | 检索路径、提示词拼装、增量合并 |
| 类型收敛 | `dfs.py` / `row_transformers.py` | 把弱类型（CSV 字符串、Parquet 提升的 float）压回强类型 |

**所以「模型字段」与「表的列」并不一一对应。** 例如 `Entity` 有 `name_embedding` 字段，但 `ENTITIES_FINAL_COLUMNS` 里没有任何嵌入列——嵌入存在独立的向量库里（b05）。反过来表里有 `frequency` 列，`Entity` 里却叫 `rank`，靠 `from_dict` 的参数默认值把 `rank_key` 映射到 `"degree"`。

第二个特征是**双向引用是冗余存储的**：`Entity.text_unit_ids` 存「这个实体出现在哪些文本单元」，`TextUnit.entity_ids` 存「这个文本单元提到哪些实体」；`Community.entity_ids` 与 `Entity.community_ids` 同理。这不是建模失误，是**为查询期省一次全表扫描**：local search 从实体出发要立刻拿到它所在的文本单元，而从文本单元出发要立刻拿到它提到的实体。

## 怎么实现的

**两个基类只差一个字段，而那个字段决定了这个产物「有没有名字」。**

```
@dataclass
class Identified:
    """A protocol for an item with an ID."""

    id: str
    """The ID of the item."""

    short_id: str | None
    """Human readable ID used to refer to this community in prompts or texts displayed to users, such as in a report text (optional)."""


@dataclass
class Named(Identified):
    """A protocol for an item with a name/title."""

    title: str
    """The name/title of the item."""
```

`short_id` 的 docstring 交代了它存在的全部理由：**「在提示词或展示给用户的文本里指代这个对象」**。真实的 id 是 uuid 或 sha512（`finalize_entities` 用 `uuid4()`，`finalize_community_reports` 用 `gen_sha512_hash(row, ["full_content"])`），长度几十字符，放进提示词会浪费 token 且模型难以引用；`short_id` 是那个小的整数（`human_readable_id`）。

**四个模型里两个继承 `Named`、两个继承 `Identified`。**

```
Entity(Named)         # 有 title：实体名
CommunityReport(Named) # 有 title：「Community 5」
Relationship(Identified)  # 无 title：它的身份是 (source, target)
TextUnit(Identified)      # 无 title：一段文本没有名字
```

**`Relationship` 不继承 `Named` 是这一页最能说明设计意图的一处。** 一条边没有名字——它由两端定义。模型里确实有 `source` / `target` 两个必填字段，但它们**不是 title**：

```
@dataclass
class Relationship(Identified):
    """A relationship between two entities. ..."""

    source: str
    """The source entity name."""

    target: str
    """The target entity name."""

    weight: float | None = 1.0
    """The edge weight."""
    ...
```

注意 `source` / `target` 存的是**实体名称（title）而不是实体 id**。`from_dict` 默认的 `source_key="source"` 对应 `RELATIONSHIPS_FINAL_COLUMNS` 里的 `source` 列，而那列里装的是实体标题。这意味着**关系与实体之间的连接靠字符串名字，不靠 id**——只要有人改了实体标题而没同步改关系的两端，连接就断了。这是「用名字当外键」的经典代价，而它在 `update_entities_relationships` 里靠 `_group_and_resolve_entities` 产出的 `entity_id_mapping` 来修补（b06）。

**四个模型的必填/可选策略并不一致，而这种不一致能读出各自的来源。**

```
# Entity：除了继承来的三个字段，其余全有默认值
type: str | None = None
description: str | None = None
description_embedding: list[float] | None = None
name_embedding: list[float] | None = None
community_ids: list[str] | None = None
text_unit_ids: list[str] | None = None
rank: int | None = 1
attributes: dict[str, Any] | None = None

# Relationship：source/target 必填，其余有默认值
weight: float | None = 1.0
...

# CommunityReport：community_id 必填（无默认），summary/full_content 有 "" 默认
community_id: str
summary: str = ""
full_content: str = ""
rank: float | None = 1.0
...

# TextUnit：text 必填（无默认）
text: str
...
```

三处可对照的细节：

**第一，`CommunityReport.community_id: str` 没有默认值。** 一份社区报告若不知道属于哪个社区就毫无意义，所以它是必填。而 `summary` / `full_content` 默认是**空字符串而不是 `None`**——因为它们要被拼进提示词，空串参与 `+` 运算不会炸，`None` 会。

**第二，`Entity.rank` 与 `Relationship.rank` 是 `int`，`CommunityReport.rank` 是 `float`。** 前者来自图上的度数（整数），后者来自 LLM 打的评分（浮点）。**同名字段不同类型**是这里刻意保留的语义差别，但如果有人写一个泛化的「取 rank 比较大小」的函数就会遇到混合类型。

**第三，`Entity.rank` 的默认值是 `1`，而 `from_dict` 的 `rank_key` 默认指向 `"degree"`。**

```
rank_key: str = "degree",
...
rank=d.get(rank_key, 1),
```

**`Entity` 里叫 `rank`，表里叫 `degree`。** 两个名字都对：表里的 `degree` 是**怎么算出来的**（图上的节点度数），模型里的 `rank` 是**它拿来干什么**（排序/重要性）。`from_dict` 的参数默认值就是这个翻译层，而那些参数全部可覆盖（`id_key` / `short_id_key` / `title_key` …）——于是同一个模型也能读别的表，只要改键名。

**`TextUnit.covariate_ids` 是唯一一个 `dict[str, list[str]]` 而不是 `list[str]`。**

```
covariate_ids: dict[str, list[str]] | None = None
"Dictionary of different types of covariates related to the text unit (optional)."
```

**因为 covariate（claim）有多个子类型，需要按类型分组。** 对比 `entity_ids` / `relationship_ids` 都是扁平列表——它们没有子类型。这个形状差异直接反映在 `schemas.py` 的 `COVARIATE_IDS = "covariate_ids"` 与 `dfs.py` 里对它的处理上：`text_units_typed` 对 `covariate_ids` 用的也是 `_split_list_column`（把它当成扁平列表切开），也就是说**模型声明 dict、类型收敛函数按 list 处理**，两者不一致。

**`short_id` 的注解是 `str | None` 但它没有默认值。**

```
short_id: str | None
```

在 Python 里，「注解可空」与「有默认值」是两件事：这个字段**必须显式传入**（可以是 `None`），不能省略。四个 `from_dict` 都老老实实传了 `d.get(short_id_key)`。

这个写法在类型上是对的（值可为空），在可用性上是别扭的（不能省略）。而实际数据里 `human_readable_id` 被 `row_transformers` 收敛成 `int`（`_safe_int`），于是 `Entity.short_id` 里装进去的**是一个 int 而不是 str**——dataclass 不做校验，类型注解不会被强制执行。**`str | None` 这个注解在运行时是假的。**

**`from_dict` 分两类，严格程度不同。**

```
# Entity / Relationship / TextUnit：缺字段回退默认值
rank=d.get(rank_key, 1),
weight=d.get(weight_key, 1.0),

# CommunityReport：三个字段直接下标，缺了就 KeyError
summary=d[summary_key],
full_content=d[full_content_key],
rank=d[rank_key],
```

`CommunityReport.from_dict` 对 `summary` / `full_content` / `rank` 用 `d[...]` 而不是 `d.get(...)`。这意味着**读一份不完整的社区报告会直接抛 `KeyError`**，而不是拿到空串。这个严格是有道理的：报告的这三项是它的全部内容，缺了就是坏数据，静默补空串会让下游把「空报告」当成正常结果。而 `Entity` 的字段大多是「有更好、没有也能用」的性质，所以用 `.get` 配默认值。

**`Community.level` 与 `Community.parent` 是 `str`，而表里是整数。**

```
@dataclass
class Community(Named):
    level: str
    """Community level."""

    parent: str
    """Community ID of the parent node of this community."""

    children: list[str]
```

对照 `COMMUNITIES_FINAL_COLUMNS` 与 `transform_community_row`：`community` / `level` 都被 `_safe_int` 收敛成 `int`，`parent` 也被 `astype(int)`。于是**模型声明 `str` 而数据是 `int`**——与 `short_id` 同一个问题，dataclass 不校验。

值得单独说明的是 `parent` 的语义：顶层社区的 `parent` 是 `-1`（c01 讲过层级父指针用 `-1` 而不是 `None`，因为要写进整数列）。所以 `Community.parent: str` 里实际装的是 `int`，而顶层装的是 `-1` 这个哨兵——**每个读 `parent` 的地方都必须知道哨兵语义**。

**`Community.children` 是反向补出来的。**

`create_communities` 里先按 `parent` 分组：

```
parent_grouped = final_communities.groupby("parent").agg(children=("community", "unique"))
final_communities = final_communities.merge(parent_grouped, left_on="community", right_on="parent", how="left")
final_communities["children"] = final_communities["children"].apply(
    lambda x: x if isinstance(x, np.ndarray) else []
)
```

**「父指向子」与「子指向父」两个方向都在表里**，而原始聚类只给出 `parent`。`children` 是从 `parent` 反推的一次 groupby。理由与前面一样：查询期不想做反向查找。

**六类产物的相互引用构成一张有向图，且每条边都以两个方向存储。**

```
Document ──text_unit_ids──→ TextUnit
TextUnit ──document_id────→ Document          （反向，单值）
TextUnit ──entity_ids─────→ Entity
Entity   ──text_unit_ids──→ TextUnit          （反向）
TextUnit ──relationship_ids→ Relationship
Relationship ─text_unit_ids→ TextUnit         （反向）
Community ──entity_ids────→ Entity
Entity   ──community_ids──→ Community         （反向）
Community ──relationship_ids→ Relationship
Community ──text_unit_ids─→ TextUnit
Community ──parent/children─→ Community       （父子双向）
CommunityReport ─community_id→ Community      （单向）
```

**唯一的单向引用是 `CommunityReport.community_id`**：报告指向社区，社区不存报告 id。这是合理的——报告是社区的派生物，社区不需要知道自己的报告在哪（而且报告可能被重新生成）。

**注意 `Relationship` 没有 text unit 之外的入向引用。** 实体不存「我参与了哪些关系」的列表——那是唯一一处**没有**冗余存储的反向引用。想从实体找到它的边，必须扫描全部关系，或者用外部图结构（`finalize_graph` 产出的 degree map 也只是一张计数表，不是邻接表）。这解释了为什么 local search 的实体到关系那一步需要一次 join 而不是直接查表。

## 关键取舍

**源码事实：** `Identified` 只有 `id` 与 `short_id`（后者注解 `str | None` 但无默认值，因此必填）；`Named` 加 `title`。`Entity` / `CommunityReport` / `Document` / `Community` 继承 `Named`，`Relationship` / `TextUnit` 继承 `Identified`。

**源码事实：** `Relationship.source` / `target` 存的是**实体名称**而非实体 id；关系与实体的连接靠字符串名字。

**源码事实：** `Relationship` 不继承 `Named`（边没有名字）；`TextUnit` 也不继承（一段文本没有名字）。

**源码事实：** 必填策略不一致且各有用意：`CommunityReport.community_id: str` 与 `TextUnit.text: str` 无默认值（缺了就没有意义）；`CommunityReport.summary` / `full_content` 默认 `""`（要拼进提示词，空串不参与算术会炸而 `None` 会）；`Entity` 的多数可选字段默认 `None`。

**源码事实：** `Entity.rank` 与 `Relationship.rank` 是 `int`（来自图上度数），`CommunityReport.rank` 是 `float`（LLM 评分）；`Entity.from_dict` 的 `rank_key` 默认是 `"degree"`——模型名与列名不同，靠 `from_dict` 的参数默认值翻译。

**源码事实：** `from_dict` 的严格程度按模型不同：`Entity` / `Relationship` / `TextUnit` 用 `.get(key, default)` 容错；`CommunityReport` 对 `summary` / `full_content` / `rank` 直接下标，缺字段抛 `KeyError`。

**源码事实：** `TextUnit.covariate_ids` 是唯一 `dict[str, list[str]]` 形状的引用字段（claim 有多个子类型需分组），但 `text_units_typed` 对它也套用 `_split_list_column`（按扁平列表处理），两者不一致。

**源码事实：** 模型声明与数据类型在多处不符：`short_id: str | None` 实际装 `int`（`human_readable_id` 被 `_safe_int` 收敛）、`Community.level` / `Community.parent` 声明 `str` 实际装 `int`（`parent` 顶层为 `-1`）。dataclass 无运行期校验，注解不生效。

**源码事实：** 双向引用被冗余存储（`Entity.text_unit_ids` ↔ `TextUnit.entity_ids`、`Entity.community_ids` ↔ `Community.entity_ids`、`Community.parent` ↔ `Community.children`）；唯一的单向引用是 `CommunityReport.community_id`；实体**没有**「参与哪些关系」的反向列表。

**架构推论：** 「有名字的继承 `Named`、没名字的继承 `Identified`」这条分界线不是形式主义，它精确对应「提示词里怎么称呼这个对象」。`short_id` 的 docstring 已经把用途写在字段上——它就是给模型看的短名。于是继承关系回答了「这个产物会不会出现在提示词里并被引用」，而这决定了它需不需要 `title`。

**架构推论：** 用实体名当关系端点是最脆弱的一处。好处是**可读**（报表里 `source: QUINTARA` 比 `source: 7f3a-...` 更像人话，模型也更容易复述），坏处是**改名即断链**。任何「合并两个同义实体」的操作都必须同时改写所有以名字引用它关系的边——这正是 `update_entities_relationships` 里 `entity_id_mapping` 存在的理由。

**架构推论：** dataclass + `from_dict` 这套组合的价值在于「**键名可覆盖**」：`from_dict` 的十几个 `*_key` 参数让同一个模型能读不同形状的表。代价是**类型不安全**：注解不会被校验、`str` 字段里可能是 `int`、必填与可选靠 `from_dict` 的实现细节而非模型声明来保证。若把 `short_id` 改成 `int`、把 `Community.level` 改成 `int`，这些注解就会变成真的——但那要动所有构造点。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/data_model/identified.py` | `Identified` | 基类：`id` + `short_id`（prompt 里用的短名） |
| `packages/graphrag/graphrag/data_model/named.py` | `Named` | 在 `Identified` 上加 `title` |
| `packages/graphrag/graphrag/data_model/entity.py` | `Entity` | 实体：类型、描述、两组嵌入、所属社区、出现过的文本单元、`rank`（来自 degree） |
| `packages/graphrag/graphrag/data_model/relationship.py` | `Relationship` | 关系：`source`/`target`（存实体名）、weight、描述嵌入、`rank` |
| `packages/graphrag/graphrag/data_model/community_report.py` | `CommunityReport` | 社区报告：`community_id` 必填、summary/full_content、`rank`（float）、size、period |
| `packages/graphrag/graphrag/data_model/text_unit.py` | `TextUnit` | 文本单元：`text` 必填、三个反向 id 列表、`n_tokens`、`document_id` |
| `packages/graphrag/graphrag/data_model/community.py` | `Community` | 社区：`level` / `parent` / `children` 树结构 + 三类 id 列表 |
| `packages/graphrag/graphrag/data_model/document.py` | `Document` | 文档：type、`text_unit_ids`、原始 text |
| `packages/graphrag/graphrag/data_model/schemas.py` | `ENTITIES_FINAL_COLUMNS` / `TEXT_UNITS_FINAL_COLUMNS` | 六类产物的落盘列名与顺序（磁盘格式的真正定义） |

## 参考

- [graphrag 源码：data_model 目录](https://github.com/microsoft/graphrag/tree/main/packages/graphrag/graphrag/data_model)
- [graphrag 文档：索引输出](https://microsoft.github.io/graphrag/index/outputs/)
- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [Python 文档：`dataclasses`](https://docs.python.org/3/library/dataclasses.html)

## 与主核 / AI 的关系

这套数据模型在主核（多模态数据库内核）里对应的是**元组结构与系统目录表**。四条经验。

**第一，磁盘格式与内存视图必须分层，且两者不要求一一对应。** 这里列名列表定磁盘格式、dataclass 定内存视图、`from_dict` 的键名参数做翻译（`rank` ← `degree`）。主核里同理：页内元组布局（定长头 + 变长体）与迭代器返回的强类型结构体是两层，中间有一个「字段偏移 → 结构体成员」的映射。**把这两层混在一起的最常见后果是「为了加一个内存字段而改磁盘格式」。**

**第二，用名字当外键换来可读性，代价是改名即断链。** `Relationship.source` 存实体标题而非 id，好处是报表与提示词都能直接读；代价是任何实体重命名/合并都要连带修复所有引用它的边。主核里若允许「列名/表名/索引名」被下游当作稳定标识，会遇到同一类问题——**要么强制用不可变 id，要么提供一个「改名时批量改写所有名字引用」的事务性入口**。graphrag 选的是后者（增量更新里的 id mapping）。

**第三，双向冗余存储是有意的空间换时间，但要清楚它只覆盖了哪些边。** 这里六类产物之间的引用几乎都双向存了，唯独「实体 → 它参与的关系」没存。**每个「没存的方向」都对应查询期的一次全扫**，而这类缺失通常只在性能分析时才发现。设计时应当把所有边列出来，逐条标注「两个方向都存 / 只存一个方向 + 为什么」。

**第四，注解不校验时，注解就是会漂移的文档。** `short_id: str | None` 里装 int、`Community.level: str` 里装 int、`parent` 里装 `-1` 哨兵——三处都因为 dataclass 不校验而无人发现。主核里若用 C 结构体或 Go struct 表达同类结构，编译期就会拦住这类不一致；**动态语言里补上这一层的唯一办法是在构造点做一次显式转换（`int(...)` / `str(...)`），而不是依赖注解**。