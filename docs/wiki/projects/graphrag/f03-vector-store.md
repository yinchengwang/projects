---
project: graphrag
feature: vector-store
title: 向量存储抽象
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag-vectors/graphrag_vectors/vector_store.py
    symbols: [VectorStore, VectorStoreDocument, VectorStoreSearchResult]
---

# 向量存储抽象

## 一句话职责

定义「向量库」的最小公共契约（四个抽象方法 + 四个具体方法）与两个数据类，并在基类里承担一件**所有向量后端都必须做但各自实现容易不一致**的事：**把时间戳炸成可过滤的分量字段**。

## 机制是什么

这个抽象比另外几个存储接口都**厚**，而厚度全部用来解决一个具体问题：**时间范围查询在向量库里没有通用语法。**

| 想要的查询 | 向量库的原生支持 |
|---|---|
| 「找与这段文本相似的文档」 | 有（相似度搜索） |
| 「找 2024 年第三季度的文档」 | **没有** —— 时间是一个字符串字段，各后端的过滤语法不同 |

**graphrag 的解法是把时间「预先炸开」：** 一条 `create_date = "2024-03-15T14:30:00"` 的文档，额外写入七个派生字段 `create_date_year=2024`、`create_date_month=3`、`create_date_month_name="March"`、`create_date_day=15`、`create_date_day_of_week="Friday"`、`create_date_hour=14`、`create_date_quarter=1`。

**于是「找 Q3 2024」变成 `create_date_year == 2024 AND create_date_quarter == 3`** —— 一个所有后端都能执行的等值/范围过滤。

**而这个炸开逻辑放在基类里**，正是因为**它必须是统一的**：如果每个后端各自决定「quarter 怎么算」，跨后端的查询结果就会不同。

## 怎么实现的

### 一、两个数据类：三段信息

```
@dataclass
class VectorStoreDocument:
    id: str | int
    vector: list[float] | None
    data: dict[str, Any] = field(default_factory=dict)
    create_date: str | None = None
    update_date: str | None = None

@dataclass
class VectorStoreSearchResult:
    document: VectorStoreDocument
    score: float
```

**`VectorStoreDocument` 的五个字段分成三段：**

| 段 | 字段 | 说明 |
|---|---|---|
| 身份 | `id` | `str | int`（不同后端的原生 id 类型不同） |
| 载荷 | `vector`、`data` | 向量**可为 `None`**；元数据是一个任意字典 |
| 时间 | `create_date`、`update_date` | ISO 8601 字符串，**均可为 `None`** |

**`vector` 可为 `None` 是一个关键的宽松**：它让同一个类型既能表达「要写入的文档（可能还需要嵌入）」也能表达「从库里读出来但没带向量的文档」（见 `similarity_search_by_vector` 的 `include_vectors=False`）。

**`SearchResult.score` 的文档写明了值域与方向：**

```
score: float
"""Similarity score between -1 and 1. Higher is more similar."""
```

**「-1 到 1，越高越相似」是余弦相似度的值域。** 而把这个契约写在文档里意味着**每个后端都必须把原生分数换算到这个口径**——否则调用方的阈值判断无法跨后端一致。

**这是一个「抽象层声明语义、后端负责换算」的典型例子。**

### 二、`__init__`：字段注册与「date 类型」的改写

**构造函数有八个参数，而其中三个的默认值是「字段名」，四个是行为开关。**

```
def __init__(self, index_name="vector_index", id_field="id", vector_field="vector",
             create_date_field="create_date", update_date_field="update_date",
             vector_size=3072, fields=None, timestamp_exploder=explode_timestamp, **kwargs):
```

**注意 `__init__` 不是抽象的** —— 它是这个接口里唯一有实现的方法之一，因为「记录配置 + 展开字段」这件事对所有后端都一样。

**而 `fields` 里声明为 `"date"` 的字段会被就地改写。**

```
self.date_fields: list[str] = [name for name, ftype in self.fields.items() if ftype == "date"]
for name in self.date_fields:
    self.fields[name] = "str"
    self.fields.update(_timestamp_fields_for(name))
```

**三步：先揪出所有 `type == "date"` 的字段名，再把它们自己的类型改成 `"str"`，最后为每个补上七个派生字段。**

**这是一个「类型声明被降级成物理类型」的转换**：`date` 是**给用户看的语义类型**（可过滤的日期），而实际存储的是 `str`（ISO 字符串）+ 七个 `int`/`str` 分量。

**为什么不直接存 `date` 类型？** 因为不同向量后端对日期类型的支持差别很大（有的只支持有限的标量类型）。**降级成 `str` + 分量是「用最保守的类型表达最丰富的语义」。**

**而内置的两个时间字段也自动注册：**

```
self.fields.update(TIMESTAMP_FIELDS)
```

**`TIMESTAMP_FIELDS` 是 `create_date` 与 `update_date` 各自七个分量的合集（14 个字段）。** 于是**即使不声明任何 `date` 字段，库表结构里也总有这 14 个** —— 因为基类会自动填它们（见下）。

**`timestamp_exploder` 是一个可注入的函数。**

```
TimestampExploder = Callable[[str, str], dict[str, str | int]]
```

**这允许换掉「怎么炸」的实现** —— 比如不用 `strftime("%B")`（依赖 locale）而用固定英文月份名。**而它是构造参数而不是类属性，说明「炸法」被视为配置而非行为。**

### 三、`_prepare_document`：写入路径上的唯一必调方法

**它的文档说得很直接：「在 insert 时、提取字段值之前调用」。**

```
def _prepare_document(self, document: VectorStoreDocument) -> None:
    if document.data is None:
        document.data = {}
    if not document.create_date:
        document.create_date = self._now_iso()
    document.data.update(self.timestamp_exploder(document.create_date, "create_date"))
    if document.update_date:
        document.data.update(self.timestamp_exploder(document.update_date, "update_date"))

    for name in self.date_fields:
        value = document.data.get(name)
        if value:
            document.data.update(self.timestamp_exploder(value, name))
```

**四个决定：**

1. **`create_date` 缺失时自动设为「现在」** —— 于是「创建时间」总有值，不必让每个调用点填。
2. **`update_date` 缺失时**不做任何事** —— 它保持 `None`，于是「从未更新」与「创建时间」可区分。**这是一个正确的默认：新文档没有更新时间。**
3. **用户声明的 `date` 字段只在有值时炸开** —— `if value:` 让缺失的日期不产生一堆空分量字段。
4. **`data.update(...)` 是合并而非替换** —— 用户原有的元数据保留。

**注意它修改的是传入的 `document` 对象（原地）而不是返回新对象。** 于是调用方在 `insert` 之后会看到 `data` 里多出十四个字段。**这是「预处理器修改输入」的形态**，也是为什么文档要求「在提取字段值之前调用」——若先取了字段再 `_prepare_document`，派生字段就写不进去。

**而 `_prepare_update` 是它的瘦身版：**

```
def _prepare_update(self, document: VectorStoreDocument) -> None:
    if document.data is None:
        document.data = {}
    if not document.update_date:
        document.update_date = self._now_iso()
    document.data.update(self.timestamp_exploder(document.update_date, "update_date"))
```

**它不碰 `create_date`** —— 因为更新不该改创建时间。**也不炸用户声明的 `date` 字段** —— 那是一个不对称：若用户更新了某个自定义日期字段，它的分量不会被重算。**这是一个真实的缺陷**（除非更新路径不负责改这些字段）。

### 四、`explode_timestamp`：七个分量的具体算法

```
if not iso_timestamp:
    return {}
dt = datetime.fromisoformat(iso_timestamp)
return {
    f"{prefix}_year": dt.year,
    f"{prefix}_month": dt.month,
    f"{prefix}_month_name": dt.strftime("%B"),
    f"{prefix}_day": dt.day,
    f"{prefix}_day_of_week": dt.strftime("%A"),
    f"{prefix}_hour": dt.hour,
    f"{prefix}_quarter": (dt.month - 1) // 3 + 1,
}
```

**七个分量的选择揭示了「人们会怎么查时间」：**

| 分量 | 支持什么查询 |
|---|---|
| `year` / `month` / `day` | 范围（「2024 年 3 月」） |
| `quarter` | 「Q3」（`(month-1)//3+1`） |
| `month_name` / `day_of_week` | **字符串等值**（「找三月的」「找周五的」） |
| `hour` | 「下午发的」（`hour >= 12`） |

**`month_name` 与 `day_of_week` 用 `strftime("%B")` / `"%A"`。** 而这两个指令**依赖 locale** —— 在默认 C locale 下是英文（`March` / `Friday`），但进程若设了 `LC_TIME=zh_CN`，就会得到「三月」「星期五」。

**这是一个跨环境不一致的来源**：同一个查询 `day_of_week == "Monday"` 在中文 locale 的进程里永远不命中。**而修法需要在 `strftime` 之前设 locale 或改用固定表**——代码没有做。

**没有 `minute` / `second` 分量** —— 说明这个体系的时间粒度是**小时**。**这是一个有意的粗化**：更细的分量会让字段数爆炸而查询价值有限（没人按分钟检索文档）。

**而 `quarter` 的公式 `(month - 1) // 3 + 1`** 对 `month=1,2,3` 给 1、`4,5,6` 给 2，正确。

### 五、抽象方法：六个里有两个「单条包装批量」

**四个真正的抽象方法：**

```
connect() -> None
create_index() -> None
load_documents(documents: list[VectorStoreDocument]) -> None
similarity_search_by_vector(query_embedding, k=10, select=None, filters=None, include_vectors=True) -> list[VectorStoreSearchResult]
search_by_id(id, select=None, include_vectors=True) -> VectorStoreDocument
count() -> int
remove(ids: list[str]) -> None
update(document: VectorStoreDocument) -> None
```

**而 `insert` 不是抽象的 —— 它委托给 `load_documents`。**

```
def insert(self, document: VectorStoreDocument) -> None:
    """Insert a single document by delegating to load_documents."""
    self.load_documents([document])
```

**这是一个「只要求后端实现批量、单条免费得到」的设计。** 好处是后端只需写一个批量接口；**代价是单条插入要走一遍批量路径**（若批量接口有固定开销，单条插入会被拖慢）。

**而 `similarity_search_by_text` 是「先嵌入再搜索」的便利方法。**

```
def similarity_search_by_text(self, text, text_embedder, k=10, select=None, filters=None, include_vectors=True):
    query_embedding = text_embedder(text)
    if query_embedding:
        return self.similarity_search_by_vector(query_embedding=query_embedding, ...)
    return []
```

**三个决定：**

1. **`text_embedder` 是一个注入的函数**（`Callable[[str], list[float]]`），不是依赖嵌入存储。**于是向量库不必知道怎么调模型。**
2. **`if query_embedding:` 用真值判断** —— 空列表与 `None` 都退化为「无结果」。
3. **嵌入失败返回 `[]` 而不是抛错。** 这是一个取向：**空结果与「嵌入模型对这段文本给不出向量」在调用方看来都是「没有匹配」**。

**这与 `graphrag-llm` 那边的取向相反** —— 那里「检索失败要传播、不当成零结果」（lightrag 那套的注释）。**差别在于这里的 `text_embedder` 是调用方提供的、失败原因已知（返回了空），而不是一个可能瞬时故障的后端。**

**`select` 参数** 让调用方只取需要的字段（减少传输），**`include_vectors`** 让调用方跳过向量（通常不需要，因为向量很大）。**两个都是「按用途裁剪返回体积」的开关。**

**而 `filters: FilterExpr | None`** 是那套过滤表达式（`AndExpr` / `OrExpr` / `NotExpr` / `Condition`，见 `filtering.py`）。**它正是「炸开时间戳」的收益兑现处** —— 有了分量字段，任何后端都能执行这个表达式。

### 六、`IndexSchema` 与配置：字段名的安全校验

**`IndexSchema` 是一个 pydantic 模型，而它的校验只查两个字段名。**

```
def _validate_schema(self) -> None:
    for field in [self.id_field, self.vector_field]:
        if not is_valid_field_name(field):
            msg = f"Unsafe or invalid field name: {field}"
            raise ValueError(msg)
```

**`is_valid_field_name` 用 `^[A-Za-z_][A-Za-z0-9_]*$`。**

**而它的 docstring 说明了这条规则的来源：「检查字段名对 CosmosDB 是否合法」。**

**这是一个「按最严后端设约束」的做法**：文件后端不在乎字段名叫什么，但 Cosmos 要求标识符必须是合法 JS 标识符（且不能用保留字）。**在配置层统一校验，就不必让每个后端各自拒绝。**

**而代价是「最严后端限制了所有后端」** —— 一个只在 LanceDB 上跑的部署也不能用 `my-field` 这样的字段名。**但这是正确的方向：跨后端可移植比单后端的表达力更重要。**

**注意 `fields` 字典里的**键**（用户声明的字段名）没有校验** —— 只有 `id_field` 与 `vector_field` 被查。**这是一个缺口**：一个 `fields = {"my-field": "str"}` 的配置能通过 `IndexSchema` 校验，然后在 Cosmos 后端上失败。

**而 `create_date_field` / `update_date_field` 也不在 `__init__` 的校验范围里**（它们甚至不在 `IndexSchema` 里）—— 它们是 `VectorStore.__init__` 的参数，而 `IndexSchema` 只覆盖 `index_name` / `id_field` / `vector_field` / `vector_size` / `fields`。

**`vector_size` 的默认值是 `DEFAULT_VECTOR_SIZE = 3072`** —— 这个数字是 `text-embedding-3-large` 的维度。**把它当默认值说明默认嵌入模型是它**；而 `VectorStoreConfig` 也允许按 index 覆盖（`index_schema: dict[str, IndexSchema]`）。

**`VectorStoreConfig` 的 `db_uri` / `url` / `api_key` / `audience` / `connection_string` / `database_name` 六个可选字段** 对应三种后端：

| 后端 | 用到的字段 |
|---|---|
| `lancedb`（默认） | `db_uri` |
| `azure_ai_search` | `url`、`api_key`、`audience` |
| `cosmosdb` | `url`、`connection_string`、`database_name` |

**六个字段全部可为 `None`，而「哪个后端需要哪些」由各字段的 docstring 声明** —— 没有跨字段的 `model_validator`（对比 `CacheConfig` 有一个「sqlite 必配 file」的校验，见 f01 那页）。**于是「选了 azure_ai_search 却忘了 `api_key`」会在构造后端时失败，而不是在配置加载时。**

**这是一个不一致**：`CacheConfig` 把组合校验做在配置层，`VectorStoreConfig` 没做。**前者的错误更早暴露。**

### 七、字段的「注册」这件事只影响过滤

**这里需要澄清一个容易误解的地方：`self.fields` 与 `self.date_fields` 只被 `_prepare_document` 使用，而 `_prepare_document` 是**后端调用的**。**

**基类自己不调它** —— 它只提供方法，各后端在 `load_documents` / `update` 里调。于是：

- **若某后端忘了调 `_prepare_document`，那条文档就没有任何时间分量**，时间查询会漏掉它。
- **而 `self.fields` 也不用于建表** —— 建表由后端的 `create_index` 负责，它可能读 `self.fields` 也可能不读。

**这意味着「字段声明」在基类层面只是一个**约定**，不是强制。** 这个接口里没有一个方法强制「注册的字段必须真的被创建」。

## 关键取舍

**源码事实：** 时间范围查询在向量库里没有通用语法，于是把 ISO 时间戳**预先炸成七个分量字段**（`year`/`month`/`month_name`/`day`/`day_of_week`/`hour`/`quarter`）写在基类里，以保证跨后端一致。

**源码事实：** 炸开是「七个分量、小时粒度」，没有 `minute`/`second`——更细的分量价值低而字段数会爆炸。

**源码事实：** `month_name` / `day_of_week` 用 `strftime("%B")` / `"%A"`，**依赖 locale**，于是在非英文 locale 的进程里 `day_of_week == "Monday"` 永不命中。

**源码事实：** 声明为 `"date"` 的用户字段会被就地改写成 `"str"` 并补上七分量——`date` 是语义类型，物理上是 `str` + 分量。内置的 `create_date`/`update_date` 的 14 个分量无条件注册（`TIMESTAMP_FIELDS`）。

**源码事实：** `_prepare_document` 原地修改传入的 document：`create_date` 缺失时填「现在」、`update_date` 缺失时**保持 `None`**（区分「从未更新」）、用户 `date` 字段只在有值时炸开。

**源码事实：** `_prepare_update` **不碰 `create_date`，也不炸用户声明的 `date` 字段**——后者是不对称（更新自定义日期字段时分量不会重算）。

**源码事实：** `timestamp_exploder` 是构造参数（可注入），说明「怎么炸」被视为配置而非行为。

**源码事实：** `insert` 不是抽象的，它委托给 `load_documents([doc])`；于是后端只需实现批量，但单条插入要走一遍批量路径。

**源码事实：** `similarity_search_by_text` 的 `text_embedder` 是注入的函数（向量库不知道如何调模型），且嵌入返回空时**返回 `[]` 而非抛错**——与「检索失败要传播」的取向相反，因为这里的失败原因已知（调用方自己的嵌入函数返回了空）。

**源码事实：** `SearchResult.score` 的文档声明「-1 到 1、越高越相似」，即每个后端必须把原生分数换算到余弦口径。

**源码事实：** `VectorStoreDocument.vector` 可为 `None`，于是同一类型能表达「待嵌入的文档」与「读出来但没带向量的文档」（`include_vectors=False`）。

**源码事实：** `IndexSchema` 只校验 `id_field` 与 `vector_field` 是合法标识符（正则 `^[A-Za-z_][A-Za-z0-9_]*$`，docstring 说明是「对 CosmosDB 合法」）；**`fields` 的键、`create_date_field`、`update_date_field` 都不在校验范围**。

**源码事实：** `VectorStoreConfig` 的六个可选字段（`db_uri`/`url`/`api_key`/`audience`/`connection_string`/`database_name`）用 docstring 声明「哪个后端用哪个」，**没有跨字段 `model_validator`**——与 `CacheConfig` 的「sqlite 必配 file」校验不一致。

**源码事实：** `self.fields` 与 `self.date_fields` 只被 `_prepare_document` 使用，而基类不调用它；后端若忘调，那条文档就没有任何时间分量。字段声明在基类层面只是约定，不强制建表。

**源码事实：** `DEFAULT_VECTOR_SIZE = 3072`（`text-embedding-3-large` 的维度），`VectorStoreConfig.index_schema` 允许按 index 覆盖。

**架构推论：** 「把时间炸成分量」这个手法在数据库里对应的是**物化的派生列**（generated column）。而它在这里是**必要的**而非优化：因为向量后端没有通用的时间函数，派生列是唯一能表达「按季度过滤」的方式。**当底层设施不支持某种语义时，把语义预先物化成它支持的形式** —— 这条规律在 lightrag 的「页码归属」、`graphrag` 的「时间分量」上重复出现。

**架构推论：** 「声明字段但基类不强制使用」这个弱契约，反映了这个接口的真实定位：**它不是一个会主动校验的模式（schema），而是一组供后端读取的约定。** 数据库的模式是强制的（DDL 建表、写入时校验），而这里的 `fields` 只影响过滤表达式能不能解析。**代价是错配不会在写入时暴露，只在查询时表现为「漏结果」。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag-vectors/graphrag_vectors/vector_store.py` | `VectorStore` | 抽象基类，八个方法与两个 `_prepare_*` |
| `packages/graphrag-vectors/graphrag_vectors/vector_store.py` | `VectorStoreDocument` | 五字段数据类（`vector` 可 `None`） |
| `packages/graphrag-vectors/graphrag_vectors/vector_store.py` | `VectorStoreSearchResult` | 文档 + 分数（文档声明 -1 到 1） |
| `packages/graphrag-vectors/graphrag_vectors/vector_store.py` | `_prepare_document` / `_prepare_update` | 填时间、炸分量、原地改 `data` |
| `packages/graphrag-vectors/graphrag_vectors/vector_store.py` | `insert` / `similarity_search_by_text` | 两个「免费」的具体方法 |
| `packages/graphrag-vectors/graphrag_vectors/timestamp.py` | `explode_timestamp` | 七分量算法（`strftime` 依赖 locale） |
| `packages/graphrag-vectors/graphrag_vectors/timestamp.py` | `TIMESTAMP_FIELDS` / `_timestamp_fields_for` | 内置与自定义字段的分量定义 |
| `packages/graphrag-vectors/graphrag_vectors/index_schema.py` | `IndexSchema` / `is_valid_field_name` | 只校验 `id_field` 与 `vector_field` |
| `packages/graphrag-vectors/graphrag_vectors/index_schema.py` | `DEFAULT_VECTOR_SIZE` | 3072 |
| `packages/graphrag-vectors/graphrag_vectors/filtering.py` | `FilterExpr` / `AndExpr` / `OrExpr` / `NotExpr` / `Condition` | 跨后端的过滤表达式（分量的消费方） |
| `packages/graphrag-vectors/graphrag_vectors/vector_store_config.py` | `VectorStoreConfig` | 三后端各自的连接字段，无跨字段校验 |
| `packages/graphrag-vectors/graphrag_vectors/vector_store_factory.py` | `create_vector_store` | 按 `VectorStoreType` 懒注册 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 源码：graphrag_vectors](https://github.com/microsoft/graphrag/tree/main/packages/graphrag-vectors/graphrag_vectors)
- [LanceDB 文档](https://lancedb.github.io/lancedb/)
- [Azure AI Search 文档：向量搜索](https://learn.microsoft.com/azure/search/vector-search-overview)

## 与主核 / AI 的关系

向量存储与主核的**二级索引（如 GiST / SP-GiST）** 面对同一个问题：一种特殊的距离度量无法用 B-tree 表达，于是需要专门的访问方法。而两者的抽象边界不同。

**主核的索引访问方法（`amapi`）要求索引提供一套完整的策略操作符**（`consistent` / `union` / `compress` / `penalty`…），而 `VectorStore` 只要「相似度搜索 + 过滤 + 计数」。

**差别在「谁来负责正确性」**：数据库的索引必须保证「索引结果 = 全表扫描结果」，所以 `amapi` 里的每个操作符都有精确语义。而向量库的 ANN 搜索**不保证精确**（近似最近邻），于是「索引结果」与「全表结果」本来就不同。**这解释了为什么 `VectorStore` 的契约能这么薄** —— 它不需要为精确性负责。

**「把时间炸成分量」与数据库的 generated column 是同构的**，而数据库有一样东西这里没有：**数据库会保证生成列永远与源列一致**（写入时计算、不能单独修改）。而这里的 `_prepare_document` 是**约定调用**的，一个忘了调的后端会静默产生不一致。**「物化派生数据」的关键不是怎么算，而是怎么保证它一定被算。**

**分数归一化到 -1..1 这个契约，对应数据库里 `amapi` 要求距离函数满足度量公理。** 两者都是「抽象层声明语义、实现负责满足」。而这里的困难在于**不是所有后端的原生分数都是余弦**（LanceDB 可能是内积或 L2），换算需要后端知道自己的度量。

对 agent 工程的直接价值有三条。其一，**底层不支持某种查询时，把语义预先物化成它支持的形式**。向量库没有时间函数，那就把时间炸成整数分量——这比给每个后端写一遍时间过滤逻辑可靠得多。其二，**物化派生数据必须有强制机制**。如果派生是「后端应该调用基类的某个方法」，那迟早有后端忘掉，而症状是「查询漏结果」这种极其难查的表现。其三，**抽象层的分数口径要写进契约**。「-1 到 1、越高越相似」这一行文档决定了所有后端必须做换算，没有它跨后端比较分数是不可能的。