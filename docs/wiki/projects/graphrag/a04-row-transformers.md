---
project: graphrag
feature: row-transformers
title: DataFrame 行转换与强类型视图
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/data_model/row_transformers.py
    symbols: [transform_entity_row, transform_relationship_row, transform_community_report_row]
  - path: packages/graphrag/graphrag/data_model/dfs.py
    symbols: [entities_typed, relationships_typed, community_reports_typed]
  - path: packages/graphrag/graphrag/data_model/dfs.py
    symbols: [split_list_column]
---

# DataFrame 行转换与强类型视图

## 一句话职责

把「弱类型表格行」压回强类型：`split_list_column` 负责把 CSV 里那个「看起来像列表的字符串」切回真正的列表（两种历史编码都要认），`dfs.py` 的 `*_typed` 对整列做这件事并顺手收敛数值列，`row_transformers.py` 的 `transform_*_row` 对**单行**做同一件事——因为它是作为 `transformer` 参数传给 `TableProvider.open()` 的，而流式读表没有 DataFrame 可操作。

## 机制是什么

这套代码存在两套几乎重复的实现，而重复的原因是**读表有两种姿势**：

| 姿势 | 入口 | 何时用 |
|---|---|---|
| 整表读进 DataFrame | `read_dataframe` + `*_typed` | 需要 pandas 操作（merge/groupby/排序）时 |
| 逐行流式读 | `Table.open(transformer=...)` | 只要遍历一遍就写出去时 |

graphrag 的索引流水线在往流式方向重构：`create_final_text_units`、`finalize_graph`、`create_final_documents`、`generate_text_embeddings` 全都是 `async for row in table` 的形态，因为它们要做的事是「逐行加工再写出」，不需要把整表装进内存。于是 `row_transformers.py` 的模块 docstring 把自己的定位说得很清楚：

> Each transformer converts a raw `dict[str, Any]` row (as produced by `csv.DictReader`, where every value is a string) into a dict with properly typed fields. They serve the same purpose as the DataFrame-based `*_typed` helpers in `dfs.py`, but operate on single rows so they can be passed as the *transformer* argument to `TableProvider.open()`.

**这句话里最重要的是「where every value is a string」。** 从 CSV 读出来的一切都是字符串：`"5"` 是字符串、`"['a', 'b']"` 是字符串、`""` 是空值。从 Parquet 读出来的一切又可能被提升成 `numpy.float64(nan)`。所以「类型收敛」不是可选的整洁工作，而是**在读路径上把两种存储格式的差异抹平**。

而 `split_list_column` 处理的是一个更具体的历史包袱：**同一个「列表」在 CSV 里有两种编码，都必须能读**。

## 怎么实现的

**`split_list_column` 用一次归一化把两种编码变成一种。**

```
def split_list_column(value: Any) -> list[Any]:
    r"""Split a column containing a list string into an actual list.

    Handles two CSV serialization formats:
    - Comma-separated (standard ``str(list)``): ``"['a', 'b']"``
    - Newline-separated (pandas ``to_csv`` of numpy arrays):
      ``"['a'\\n 'b']"``

    Both formats are stripped of brackets, quotes, and whitespace so
    that existing indexes produced by the old pandas-based indexer
    remain readable.
    """
    if isinstance(value, str):
        if not value:
            return []
        normalized = value.replace("\n", ",")
        items = [item.strip("[] '\"") for item in normalized.split(",")]
        return [item for item in items if item]
    return value
```

实现只有五行，但每一行都在处理一个具体的历史事实：

1. **先把换行统一成逗号。** 这是两种编码的**唯一差别**：`str(["a","b"])` 得到 `"['a', 'b']"`（逗号分隔），而 pandas 把 numpy 数组写进 CSV 时会把数组的 `repr` 拆成多行，于是得到 `"['a'\n 'b']"`。`replace("\n", ",")` 之后两者结构一致。
2. **`split(",")` 后逐项 `strip("[] '\"")`。** 注意 `strip` 的参数是**字符集合**而不是子串——它会去掉两端任意组合的 `[`、`]`、空格、单引号、双引号。所以 `"['a'"` → `"a"`，`"[ 'b' ]"` → `"b"`。
3. **空项被过滤。** `"[ ]"`（空列表）会切出 `[""]`，过滤后回到 `[]`。
4. **非字符串原样返回。** `return value` 而不是 `list(value)`——因为它可能是已经是列表、或者可能是 `None`。**返回值类型标注是 `list[Any]`，但实际可能返回 `None` 或任意对象**，这是又一个「注解不生效」的地方。

**这个实现有一个必须指出来的边界：元素内容含逗号就会切坏。**

`"['a, b', 'c']"` 会被切成 `["a", "b'", "'c'"]` 之类的东西。实际数据里成立是因为**实体名/关系描述里的逗号会被上游清洗掉或替换**，而列表元素是 id、标题这类短标识。但它是「用字符串切分解析结构化数据」的经典脆弱点——同一份逻辑若用在自由文本上就会静默产出错误的列表。

**`_safe_int` 在两处有完全不同的实现，且默认值不同。**

```
# dfs.py —— 对整列操作
def _safe_int(series: pd.Series, fill: int = -1) -> pd.Series:
    """Convert a series to int, filling NaN values first."""
    return series.fillna(fill).astype(int)


# row_transformers.py —— 对单个值操作
def _safe_int(value: Any, fill: int = -1) -> int:
    """Coerce a value to int, returning *fill* when missing or empty.

    Handles NaN from Parquet float-promoted nullable int columns
    (``row.to_dict()`` yields ``numpy.float64(nan)``) and any other
    non-convertible type.
    """
    if value is None or value == "":
        return fill
    try:
        return int(value)
    except (ValueError, TypeError):
        return fill
```

**同名不同实现、默认值恰好相同（都是 `-1`），但语义不同**：DataFrame 版只能处理「列里是数字或 NaN」，遇到 `""` 会抛（`astype(int)` 不接受空串）；单值版显式把 `""` 当缺失。这也是为什么单值版必须存在——**CSV 里的空字段是空字符串，不是 NaN**。

单值版 docstring 里那句「numpy.float64(nan)」解释了另一半：Parquet 的 nullable 整数列若有一个 null，整列会被提升成 float，于是 `row.to_dict()` 给出 `nan`。`int(nan)` 会抛 `ValueError`，所以必须走 `except` 分支。

调用点对 `fill` 的选择揭示了两类字段：

```
transform_entity_row:
    human_readable_id → _safe_int(...)          # fill=-1
    frequency         → _safe_int(..., 0)       # fill=0
    degree            → _safe_int(..., 0)       # fill=0
```

**`human_readable_id` 用 `-1`，而 `frequency` / `degree` 用 `0`。** 理由不同：`human_readable_id` 是标识符，缺失时 `-1` 能与任何真实 id 区分；`frequency` / `degree` 是计数，缺失时按 0 处理（一个实体没出现过的次数就是 0），且 0 参与加法不会污染。

**`_safe_float` 里有一行刻意不用 `math` 的 NaN 检测。**

```
def _safe_float(value: Any, fill: float = 0.0) -> float:
    """... Also applies *fill* when the value is NaN (e.g. from Parquet
    nullable columns), keeping behavior consistent with the
    DataFrame-level ``fillna(fill).astype(float)`` pattern."""
    if value is None or value == "":
        return fill
    try:
        result = float(value)
    except (ValueError, TypeError):
        return fill
    # math.isnan without importing math
    if result != result:
        return fill
    return result
```

`result != result` 是 NaN 的标准判定（NaN 不等于自身），注释坦白说明了这是为了不 import `math`。**代价是这一行对任何看到它的人都需要一次额外的认知跳跃**，而省下的只是一个标准库 import。这是「小优化牺牲可读性」的一个轻微实例。

**`_coerce_list` 处理三种输入，这是三种存储格式的交汇点。**

```
def _coerce_list(value: Any) -> list[Any]:
    """Parse a value into a list, handling CSV string and array types.

    Handles three cases:
    - str: CSV-encoded list (e.g. from CSVTable rows)
    - list: already a Python list (pass-through)
    - array-like with ``tolist`` (e.g. numpy.ndarray from ParquetTable
      ``row.to_dict()``)
    """
    if isinstance(value, str):
        return split_list_column(value)
    if isinstance(value, list):
        return value
    if hasattr(value, "tolist"):
        return value.tolist()
    return []
```

三支分别对应 CSV、已经是列表、Parquet 的 numpy 数组。**第四支 `return []` 是兜底**：`None`、数字、dict 都会走到这里变成空列表。这个兜底的方向是「宁可当空，不要炸」——对列表字段来说是合理的（空列表语义清楚），但如果上游本意是「有值但格式坏了」，就会被静默吞掉。

**注意 `array-like` 用的是 `hasattr(value, "tolist")` 而不是 `isinstance(value, np.ndarray)`。** duck typing 让这段代码不必 import numpy，同时也能接住 `pd.Series` 之类——但也意味着任何恰好有 `tolist` 方法的对象都会被当数组。

**`transform_*_row` 的字段集合与 `*_typed` 一一对应，且注释逐条标出映射。**

```
def transform_entity_row(row: dict[str, Any]) -> dict[str, Any]:
    """Coerce types for an entity row.

    Mirrors ``entities_typed``: ``human_readable_id`` -> int,
    ``text_unit_ids`` -> list, ``frequency`` -> int, ``degree`` -> int.
    """
```

**「Mirrors」这个词在这份文件里出现了六次**（entity / relationship / community / community_report / covariate / text_unit / document）。这是作者在显式标注「这两份实现必须保持同步」——一个靠注释维护的重复。

而它们确实一一对应。对照 `entities_typed` 与 `transform_entity_row`：两者都处理 `SHORT_ID`（→ int）、`TEXT_UNIT_IDS`（→ list）、`NODE_FREQUENCY`（→ int, fill 0）、`NODE_DEGREE`（→ int, fill 0）。命令完全一致。

**但有一处不完全对应，值得单独指出：`entities_typed` 的 `SHORT_ID` 用 `_safe_int(df[SHORT_ID])`（fill=-1），而 `communities_typed` / `community_reports_typed` / `text_units_typed` / `documents_typed` 用的是 `astype(int)`（无填充）。**

```
def communities_typed(df):
    if SHORT_ID in df.columns:
        df[SHORT_ID] = df[SHORT_ID].astype(int)
    df[COMMUNITY_ID] = df[COMMUNITY_ID].astype(int)
    df[COMMUNITY_LEVEL] = df[COMMUNITY_LEVEL].astype(int)
```

`astype(int)` 遇到 NaN 会抛 `ValueError: Cannot convert non-finite values`。而对应的 `transform_community_row` 用的是 `_safe_int`（fill=-1）——**同一个字段，DataFrame 路径会炸而流式路径不会**。这是「两份实现必须同步」这条注释没覆盖到的不对称：字段集合同步了，容错程度没有。

**「有值才处理」与「无条件处理」是两种写法，区分标准是「这个字段是否可能不存在」。**

```
# 有条件：字段可能不在表里（早期索引 / 精简表）
if "human_readable_id" in row:
    row["human_readable_id"] = _safe_int(row["human_readable_id"])
if "text_unit_ids" in row:
    row["text_unit_ids"] = _coerce_list(row["text_unit_ids"])

# 无条件：字段必然存在（否则这行不是这类记录）
row["community"] = _safe_int(row.get("community"))
row["level"] = _safe_int(row.get("level"))
row["children"] = _coerce_list(row.get("children"))
```

`transform_community_row` 与 `transform_community_report_row` 里，`community` / `level` / `children` / `rank` / `findings` / `size` / `period` 是**无条件**写的（用 `row.get(...)` 取，缺了得到 `None` 再被 `_safe_*` 转成填充值）；而其他所有 transform 都用 `if key in row` 保护。

**这个差别不是随意的：`community` / `level` 是这类记录的身份字段，缺了就不是一条合法记录，所以「补默认值」后再写给下游会更快暴露问题**；而 `text_unit_ids` 这类引用字段在早期版本里可能整列不存在，硬写会凭空造出一个空列表列。

**有一个 transform 与其他所有都不同：它不是收敛类型，而是造一个新列。**

```
def transform_entity_row_for_embedding(row: dict[str, Any]) -> dict[str, Any]:
    """Add a title_description column for embedding generation."""
    title = row.get("title") or ""
    description = row.get("description") or ""
    row["title_description"] = f"{title}:{description}"
    return row
```

**它把 `title` 与 `description` 拼成一个新列 `title_description`，因为嵌入要走 `embed_column="title_description"`。** 这个函数的定位从名字就能看出：`_for_embedding`。它在 `EMBEDDING_FIELDS` 里作为 `entity_description` 的 `row_transform` 被使用（b05）。

为什么要把两列拼起来嵌入？因为**实体名本身是检索质量的关键**——只嵌描述会丢掉「用户查的就是这个名字」这一强信号。冒号分隔让它和描述有明确边界。

**`RowTransformer` 的类型是 `Callable[[dict[str, Any]], Any]`，而表的实现还接受类。**

```
RowTransformer = Callable[[dict[str, Any]], Any]

def _apply_transformer(transformer: RowTransformer, row: dict[str, Any]) -> Any:
    """Apply transformer to row, handling both callables and classes.

    If transformer is a class (e.g., Pydantic model), calls it with **row.
    Otherwise calls it with row as positional argument.
    """
    if inspect.isclass(transformer):
        return transformer(**row)
    return transformer(row)
```

**「传类就用 `**row` 构造、传函数就位置传参」这个分派让 dataclass / pydantic 模型可以直接当 transformer 用。** 这意味着前面 a03 讲的 `Entity.from_dict` 那套模型**本可以直接这样接上**——但实际代码里全是函数（`transform_entity_row` 等），没有一处传模型类。也就是说这条能力目前没有被使用。

而返回类型是 `Any` 而不是 `dict[str, Any]`，正是为「transformer 可以返回模型实例」留的余地。于是 transformer 的语义是「行 → 任意对象」，不保证还是 dict。

## 关键取舍

**源码事实：** `row_transformers.py` 与 `dfs.py` 的 `*_typed` 是两套并行实现，前者逐行（供 `TableProvider.open(transformer=...)` 用）、后者整列（供 `read_dataframe` 后用）；模块 docstring 明确说明前者是为了流式读表，因为 `csv.DictReader` 给出的每个值都是字符串。

**源码事实：** `split_list_column` 先 `replace("\n", ",")` 把两种历史编码（`str(list)` 的逗号分隔、pandas 写 numpy 数组的换行分隔）归一，再 `split(",")`，逐项 `strip("[] '\"")`（按字符集合剥，非子串），最后过滤空项；非字符串输入原样返回（尽管注解声明返回 `list[Any]`）。docstring 明确目的是「让旧 pandas 版索引器产出的索引仍然可读」。

**源码事实：** 两个 `_safe_int` 同名不同实现：`dfs.py` 的按列 `fillna(fill).astype(int)`（遇空串会抛），`row_transformers.py` 的按值 try/except（把 `None` 与 `""` 都当缺失）；两者默认 `fill` 都是 `-1`。

**源码事实：** `split_list_column` 的解析依赖元素不含逗号与引号；这类「用分隔符解析结构化字符串」的做法在元素含分隔符时会静默切错。

**源码事实：** `_safe_float` 用 `result != result` 判 NaN，注释说明是为了不 import `math`。

**源码事实：** `_coerce_list` 按 str / list / 有 `tolist` 的对象 / 其他（返回 `[]`）四支分派，覆盖 CSV 字符串、已解析列表、Parquet 的 numpy 数组；array-like 用 `hasattr(..., "tolist")` 的 duck typing 以免 import numpy。

**源码事实：** `human_readable_id` 的 `fill=-1` 而 `frequency` / `degree` 的 `fill=0`——标识符缺失要与真实 id 可区分，计数缺失按 0。

**源码事实：** `transform_community_row` / `transform_community_report_row` 对身份字段（`community` / `level` / `children` / `rank` / `findings` / `size` / `period`）**无条件**写（缺则补填充值），其余 transform 全都用 `if key in row` 保护。

**源码事实：** `dfs.py` 里 `communities_typed` / `community_reports_typed` / `text_units_typed` / `documents_typed` 对 `SHORT_ID` 用 `astype(int)`（遇 NaN 抛错），而对应的 `transform_*_row` 用 `_safe_int`（容错）——字段集合同步了，容错程度没同步。

**源码事实：** `transform_entity_row_for_embedding` 不是类型收敛而是**造新列**：把 `title` 与 `description` 拼成 `title_description`（冒号分隔），供 `EMBEDDING_FIELDS` 里 `entity_description` 的 `embed_column` 使用。

**源码事实：** `RowTransformer = Callable[[dict[str, Any]], Any]`，表的 `_apply_transformer` 同时支持「传类则 `**row` 构造」与「传函数则位置传参」；返回类型是 `Any`，允许 transformer 产出非 dict 对象。

**架构推论：** 「同一逻辑两套实现」在这里是流式重构的过渡代价，而作者用六处 `Mirrors ...` 注释把它变成了显式契约。这类注释是有价值的（否则没人知道要同步），但它**只能保证字段集合同步，不能保证行为同步**——`astype(int)` 与 `_safe_int` 的差别正是漏网之鱼。**更稳的做法是让 `*_typed` 直接复用 `transform_*_row`（`df.apply` 或 `iterrows`），用一份实现消灭漂移**；代价是 DataFrame 路径变慢，而对小表无所谓、对大表可能不可接受——这大概就是没有复用的原因，但代码里没写。

**架构推论：** `split_list_column` 这种「解析 `str(list)`」的做法是一个技术债标记：**它存在说明某个时点把结构化数据写进了 CSV 单元格**。而它同时要认两种编码，说明这个格式还被改过一次（从 `str(list)` 到 pandas 的多行 repr）。任何新系统都不该重演这个选择——列式/行式表格式（parquet、arrow）能原生存 list 类型，只有 CSV 需要这种字符串编解码。

**架构推论：** transformer 支持「类」这条未被使用的路径，说明这一层原本的设计目标是「行 → 模型实例」。现在实际用的是「行 → 行」，也就是**把类型收敛留在了 dict 层面而不是升到 dataclass**。这让流式路径比 DataFrame 路径更轻（不构造对象），但也意味着 a03 那套模型的 `from_dict` 与流式读表是两条互不相通的路。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/data_model/dfs.py` | `split_list_column` | 列表字符串 → 列表（兼容逗号与换行两种编码） |
| `packages/graphrag/graphrag/data_model/dfs.py` | `_safe_int`（列版） | `fillna(fill).astype(int)`，遇空串会抛 |
| `packages/graphrag/graphrag/data_model/dfs.py` | `entities_typed` | 实体表整列收敛（`human_readable_id` / `text_unit_ids` / `frequency` / `degree`） |
| `packages/graphrag/graphrag/data_model/dfs.py` | `relationships_typed` | 关系表整列收敛（含 `weight` → float、`combined_degree` → int） |
| `packages/graphrag/graphrag/data_model/dfs.py` | `community_reports_typed` | 报告表整列收敛（`rank` → float、`findings` → list） |
| `packages/graphrag/graphrag/data_model/row_transformers.py` | `_safe_int` / `_safe_float`（值版） | 单值容错转换，处理空串与 NaN |
| `packages/graphrag/graphrag/data_model/row_transformers.py` | `_coerce_list` | 四支分派：CSV 字符串 / 列表 / 有 `tolist` / 兜底空列表 |
| `packages/graphrag/graphrag/data_model/row_transformers.py` | `transform_entity_row` | 实体行收敛（mirrors `entities_typed`） |
| `packages/graphrag/graphrag/data_model/row_transformers.py` | `transform_relationship_row` | 关系行收敛（mirrors `relationships_typed`） |
| `packages/graphrag/graphrag/data_model/row_transformers.py` | `transform_community_report_row` | 报告行收敛，身份字段无条件写（mirrors `community_reports_typed`） |
| `packages/graphrag/graphrag/data_model/row_transformers.py` | `transform_entity_row_for_embedding` | 造 `title_description` 列供嵌入使用 |
| `packages/graphrag-storage/graphrag_storage/tables/table.py` | `RowTransformer` | `Callable[[dict[str, Any]], Any]` |
| `packages/graphrag-storage/graphrag_storage/tables/csv_table.py` | `_apply_transformer` | 传类则 `**row` 构造，传函数则位置传参 |

## 参考

- [graphrag 源码：row_transformers.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/data_model/row_transformers.py)
- [graphrag 源码：dfs.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/data_model/dfs.py)
- [pandas 文档：`to_csv`](https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_csv.html)
- [graphrag 源码仓库](https://github.com/microsoft/graphrag)

## 与主核 / AI 的关系

类型收敛在主核（多模态数据库内核）里对应的是**从磁盘字节到强类型元组的解包**——只不过主核靠固定的页布局与编译期偏移，这里靠运行期逐字段 `try/except`。四条经验。

**第一，两套并行实现必须有一份「同步契约」，且最好让其中一份变成另一份的封装。** 这里六处 `Mirrors ...` 注释就是契约，但它只保证字段集合同步——`astype(int)` 与 `_safe_int` 的容错差异已经漏了。主核里若有「批量 API 与流式 API」两套解码路径，同理：**要么让一套调用另一套，要么把差异点（容错策略）也写进契约**，而不是只对齐字段清单。

**第二，容错的方向必须逐字段决定，不能全局统一。** `human_readable_id` 缺失给 `-1`（可与真实 id 区分），`frequency` / `degree` 缺失给 `0`（计数语义），`""` 与 `None` 都当缺失（CSV 与 Parquet 两种来源）。主核解包元组时同样会遇到「EOF 截断」「列被 drop」「值为 NULL」三种缺失，**每一种的正确默认值都不同**；一刀切给 0 或给 NULL 都会在某些列上产生错值。

**第三，把结构化数据编码进 CSV 单元格是应该被明确标记为技术债的选择。** `split_list_column` 要认两种历史编码，且「元素含分隔符就切错」——这两点都是同一个决定的后果。主核若导出数据给外部消费，**优先选能原生存 list 的列式格式**；一旦落到 CSV，就必须在文档里写清编码规则与元素限制（不得含逗号/引号），否则下游解析只能靠猜。

**第四，`duck typing + hasattr` 能免除 import，但会把「恰好有同名方法」的对象也接住。** `hasattr(value, "tolist")` 接住了 numpy 数组与 pandas Series（意图内），也会接住任何自定义带 `tolist` 的对象（意图外）。主核里做跨后端值适配时同理：**`hasattr` 判据比 `isinstance` 宽，好处是不引入依赖，坏处是语义边界变成「碰巧长得像」**。若适配层要对值做有损转换（如这里把未知类型变成空列表），宽判据的风险会被放大——因为错误类型会被静默吞掉而不是报错。
