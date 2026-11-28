---
project: graphrag
feature: config-enums
title: 枚举与索引方法
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/config/enums.py
    symbols: [SearchMethod, IndexingMethod, AsyncType, ModularityMetric]
  - path: packages/graphrag/graphrag/config/enums.py
    symbols: [NounPhraseExtractorType, ReportingType]
---

# 枚举与索引方法

## 一句话职责

六个枚举把「多选一」的配置决定变成类型：`IndexingMethod` 的四个值选出**哪一张工作流图**（标准 LLM 抽取 / NLP 快速抽取，各自再分全量与增量），`SearchMethod` 选检索算法，`AsyncType` 选并发原语，`ModularityMetric` 选评估口径，`NounPhraseExtractorType` 选快速路径的名词短语抽取器，`ReportingType` 选日志落地位置。

## 机制是什么

`IndexingMethod` 是这套系统的核心开关，而它的重要性不在枚举本身，在它的**用法**：这个枚举的值被当成 **`PipelineFactory.pipelines` 字典的键**。

```
PipelineFactory.register_pipeline(IndexingMethod.Standard, ["load_input_documents", *_standard_workflows])
PipelineFactory.register_pipeline(IndexingMethod.Fast, ["load_input_documents", *_fast_workflows])
PipelineFactory.register_pipeline(IndexingMethod.StandardUpdate, ["load_update_documents", *_standard_workflows, *_update_workflows])
PipelineFactory.register_pipeline(IndexingMethod.FastUpdate, ["load_update_documents", *_fast_workflows, *_update_workflows])
```

于是「换索引方式」不是靠 `if` 分支散落在各处，而是**换一个查表键**。这让「索引方法的组合爆炸」被压缩成一张四行的注册表：标准与快速各一套主体工作流，增量在两者后面统一追加八个 `update_*`。

这个结构决定了枚举的**四分法不是正交的**：`Standard` / `Fast` 是一个维度（抽取方式），`StandardUpdate` / `FastUpdate` 是「同一维度 × 增量」。枚举把二维压成一维，代价是出现了两个「派生的」成员——而谁负责从 `Standard` 派生出 `StandardUpdate`，是这个枚举最值得读的地方。

## 怎么实现的

**六个枚举里五个是 `str, Enum`，一个不是。**

```
class ReportingType(str, Enum):
    file = "file"
    blob = "blob"

    def __repr__(self):
        return f'"{self.value}"'


class AsyncType(str, Enum):
    AsyncIO = "asyncio"
    Threaded = "threaded"


class SearchMethod(Enum):
    """The type of search to run."""

    LOCAL = "local"
    GLOBAL = "global"
    DRIFT = "drift"
    BASIC = "basic"

    def __str__(self):
        return self.value


class IndexingMethod(str, Enum):
    Standard = "standard"
    Fast = "fast"
    StandardUpdate = "standard-update"
    FastUpdate = "fast-update"


class NounPhraseExtractorType(str, Enum):
    RegexEnglish = "regex_english"
    Syntactic = "syntactic_parser"
    CFG = "cfg"


class ModularityMetric(str, Enum):
    Graph = "graph"
    LCC = "lcc"
    WeightedComponents = "weighted_components"
```

`SearchMethod` 是唯一的例外：它继承 `Enum` 而不是 `str, Enum`，然后**手写 `__str__` 返回 `self.value` 来补偿**。

这个差别不是风格，是行为：

- `str, Enum` 的成员**本身就是字符串**，于是 `IndexingMethod.Standard == "standard"` 为真，且 `json.dumps({"m": IndexingMethod.Standard})` 直接可行。
- 纯 `Enum` 的成员**不是**字符串。`SearchMethod.LOCAL == "local"` 为 **False**；`f"{SearchMethod.LOCAL}"` 之所以能给出 `"local"`，完全靠那个手写的 `__str__`。而 `str()` 能修的东西有限——`json.dumps` 序列化它的实例会抛 `TypeError`（`Enum` 不可 JSON 序列化，除非成员是 `str` 子类）。

**为什么单独把 `SearchMethod` 留在纯 `Enum`？** 从代码里能看到的唯一后果是：任何「把它当字符串用」的地方都必须显式 `.value` 或 `str()`。而 CLI 层恰好就是这么写的：

```
method: SearchMethod = typer.Option(
    SearchMethod.GLOBAL.value,
    "--method",
    "-m",
    help="The query algorithm to use.",
)
```

**注意这里默认值给的是 `.value`（字符串 `"global"`），而类型标注写的是 `SearchMethod`。** 同一个写法也出现在 `IndexingMethod` 的 CLI 选项上。这在 typer 下能工作（枚举注解会生成 choice，默认值按值匹配），但语义上是「默认值不是该类型的实例」——一个类型检查器会报错、而运行期正确的组合。

**`ReportingType.__repr__` 的引号是给 YAML 用的。**

```
def __repr__(self):
    return f'"{self.value}"'
```

它让 `repr(ReportingType.file)` 变成 `"file"`（带引号）而不是 `<ReportingType.file: 'file'>`。这个 `__repr__` 只在**被嵌进另一个对象的表示**时才有价值——而配置对象恰恰这样用：`GraphRagConfig.__repr__` 走 `devtools.pformat`，其中每个字段用 `repr`。于是打印配置时会得到 `type: "file"`，**可以直接粘回 YAML**。

**六个枚举里只有三个的注释解释了「选这个意味着什么」，而这三个正好是最难从名字猜到的。**

```
class IndexingMethod(str, Enum):
    Standard = "standard"
    """Traditional GraphRAG indexing, with all graph construction and summarization performed by a language model."""
    Fast = "fast"
    """Fast indexing, using NLP for graph construction and language model for summarization."""
    StandardUpdate = "standard-update"
    """Incremental update with standard indexing."""
    FastUpdate = "fast-update"
    """Incremental update with fast indexing."""


class NounPhraseExtractorType(str, Enum):
    RegexEnglish = "regex_english"
    """Standard extractor using regex. Fastest, but limited to English."""
    Syntactic = "syntactic_parser"
    """Noun phrase extractor based on dependency parsing and NER using SpaCy."""
    CFG = "cfg"
    """Noun phrase extractor combining CFG-based noun-chunk extraction and NER."""


class ModularityMetric(str, Enum):
    Graph = "graph"
    """Graph modularity metric."""
    LCC = "lcc"
    WeightedComponents = "weighted_components"
    """Weighted components modularity metric."""
```

三处注释分别解决了三种「名字不足以表达」的情况：

1. **`Fast` 的注释说明分工边界**：「NLP 做图构建、LLM 做摘要」——这是 `Fast` 与 `Standard` 的**唯一实质差别**。没有这句，`Fast` 容易被读成「跳过摘要」。
2. **`NounPhraseExtractorType` 的注释说明代价**：`RegexEnglish` 是「最快，但限于英语」；`Syntactic` 与 `CFG` 依赖 SpaCy 模型。**这三者的选择同时是「语言覆盖」与「依赖重量」的权衡**，而枚举值本身只说了实现方式。
3. **`ModularityMetric` 的注释最弱**：三个都是「X modularity metric」的同义反复，只说出了一个事实——三者的实现不同（`calculate_graph_modularity` / `calculate_lcc_modularity` / `calculate_weighted_modularity`）。**要看懂该选哪个，必须去读那三个函数**，枚举这里不提供判据。

**`AsyncType` 的两个值不只是两个名字，而是两条代码路径。**

```
async_type: AsyncType = AsyncType.AsyncIO
...
match async_type:
    case AsyncType.AsyncIO:
        ...
    case AsyncType.Threaded:
        ...
```

`derive_from_rows` 里这个 `match` 是唯一的消费者。于是这个枚举的语义完全由那个函数的两支定义：`AsyncIO` 走 `asyncio.gather`（真并发，受事件循环调度），`Threaded` 走线程池（能跑同步函数，受 GIL 与线程数限制）。

**注意默认值在这两处不一致**：枚举层面的契约出现在 `derive_from_rows` 的签名默认值 `AsyncType.AsyncIO`，而 `GraphRagConfig` 与 `ExtractGraphNLPDefaults` 的默认值是 `AsyncType.Threaded`。也就是说「函数默认」与「配置默认」是两个不同的值——直接调 `derive_from_rows` 得到 `AsyncIO`，走配置则得到 `Threaded`。

**`ModularityMetric` 被 `match` 逐个分派，它是三个评估函数的选择器。**

```
def calculate_modularity(
    edges: pd.DataFrame,
    max_cluster_size: int = 10,
    random_seed: int = 0xDEADBEEF,
    use_root_modularity: bool = True,
    modularity_metric: ModularityMetric = ModularityMetric.WeightedComponents,
) -> float:
    match modularity_metric:
        case ModularityMetric.Graph:
            return calculate_graph_modularity(...)
        case ModularityMetric.LCC:
            return calculate_lcc_modularity(...)
        case ModularityMetric.WeightedComponents:
            return calculate_weighted_modularity(...)
```

三支接收完全相同的参数、返回完全相同的类型。**这是一个「三个可互换的评分口径」的枚举**，而不是「三种不同的行为」——它的用途是**评估与调参**，不是生产路径。这也解释了为什么它的注释最弱：选哪个不影响产物，只影响那个分数的数值。

**`IndexingMethod` 的第四与第五个成员，实际是由字符串拼接产生的。**

```
def _get_method(method: IndexingMethod | str, is_update_run: bool) -> str:
    m = method.value if isinstance(method, IndexingMethod) else method
    return f"{m}-update" if is_update_run else m
```

**这段七行代码是整页最值得细看的地方**，因为它与枚举的定义有一个真实的错位：

- 枚举里已经有两个「update」成员（`StandardUpdate` / `FastUpdate`），值分别是 `"standard-update"` / `"fast-update"`。
- 但 `_get_method` **不用它们**——它拿基础值（`"standard"` / `"fast"`）**拼字符串**得到同样的结果。
- 两种路径产出的字符串一致，于是查表能命中；**但组合起来会出错**。

具体地：若调用方传入 `IndexingMethod.StandardUpdate`（值 `"standard-update"`）并把 `is_update_run=True`，`_get_method` 返回 `"standard-update-update"`。这个键在 `PipelineFactory.pipelines` 里不存在，而查表用的是 `.get(method, [])`：

```
workflows = config.workflows or cls.pipelines.get(method, [])
logger.info("Creating pipeline with workflows: %s", workflows)
return Pipeline([(name, cls.workflows[name]) for name in workflows])
```

**默认值是空列表 → 生成一个零工作流的 `Pipeline` → 跑完什么都不做，日志还打印「Indexing pipeline complete」。**

这是一个「静默空跑」的失败模式：没有异常、没有警告，只有一条 `Creating pipeline with workflows: []` 的 info 日志。它的成因是三个各自合理的选择叠加：枚举把二维压成一维（于是有冗余成员）、`_get_method` 用拼接而不查表（于是冗余成员被绕过）、`create_pipeline` 用 `.get(..., [])` 而不是 `.get(...)` 加显式报错（于是不认识的键变成空管道）。

**`create_pipeline` 里还有一个优先级：`config.workflows` 永远赢。**

```
workflows = config.workflows or cls.pipelines.get(method, [])
```

`GraphRagConfig.workflows` 的字段注释把这条说死了：「List of workflows to run, in execution order. **This always overrides any built-in workflow methods.**」

于是 `--method fast` 这个参数在配置显式给了 `workflows` 时**完全无效**。这是一个不报错、不警告的优先级覆盖——同样的「静默」家族。

**`SearchMethod` 的四个值在 CLI 里走 `match`，且它是命令级的而非索引级的。**

```
case SearchMethod.LOCAL: ...
case SearchMethod.GLOBAL: ...
case SearchMethod.DRIFT: ...
case SearchMethod.BASIC: ...
```

四支分别对应四类检索，与 `GraphRagConfig` 里的四个 `*_search` 配置节（`local_search` / `global_search` / `drift_search` / `basic_search`）一一对应。**枚举成员与配置节是同步演化的**：加一种检索方式要同时加一个枚举值、一个配置节、一个 `match` 分支、一条注册的工作流——四处缺一不可，而代码里没有任何机制保证它们同步。

## 关键取舍

**源码事实：** 六个枚举里五个是 `str, Enum`（成员即字符串、可直接 JSON 序列化），`SearchMethod` 是纯 `Enum` 并手写 `__str__` 返回 `value`；因此 `SearchMethod.LOCAL == "local"` 为假，而其余五个为真。

**源码事实：** CLI 层把枚举选项的默认值写成 `.value`（字符串）而注解写枚举类型（`method: IndexingMethod = typer.Option(IndexingMethod.Standard.value, ...)`、`method: SearchMethod = typer.Option(SearchMethod.GLOBAL.value, ...)`）。

**源码事实：** `ReportingType` 手写 `__repr__` 返回带引号的 `value`，使配置对象打印（`devtools.pformat`）出来的枚举字段能直接粘贴回 YAML。

**源码事实：** 只有 `IndexingMethod` / `NounPhraseExtractorType` / `ModularityMetric` 三个枚举的成员带 docstring；`AsyncType` / `SearchMethod` / `ReportingType` 没有。

**源码事实：** `IndexingMethod` 的四个值是 `PipelineFactory.pipelines` 的注册键；两条主体路径（`Standard` / `Fast`）各自在 `register_pipeline` 里被展开成工作流名列表，增量的两个键在主体之后追加同一组八个 `update_*` 工作流。

**源码事实：** `_get_method` 用字符串拼接（`f"{m}-update" if is_update_run else m`）产生增量键，而不是使用枚举里已有的 `StandardUpdate` / `FastUpdate` 成员；两者产出一致，但传 `StandardUpdate` + `is_update_run=True` 会得到不存在的键 `"standard-update-update"`。

**源码事实：** `create_pipeline` 用 `config.workflows or cls.pipelines.get(method, [])`——`config.workflows` 优先（字段注释写「always overrides any built-in workflow methods」），且未知 method 键回退为空列表，生成零工作流的管道而不报错。

**源码事实：** `AsyncType` 的默认值在两层不同：`derive_from_rows` 的签名默认是 `AsyncIO`，而 `GraphRagConfig` / `ExtractGraphNLPDefaults` 的默认是 `Threaded`。

**源码事实：** `ModularityMetric` 的三个分支接收相同参数、返回相同类型（`float`），只是三个不同的模块度计算实现——它是评估口径选择器，不改变索引产物。

**架构推论：** 「枚举当注册表键」是这套设计最有价值的一点：它把「换索引方式」从散落的 `if` 变成一次查表，于是新增一种索引方式只需 `register_pipeline` 一行 + 一个枚举成员。代价是**枚举与注册表的同步没有机制保证**——注册了却忘了加成员、或加了成员却忘了注册，都会退化成「查不到 → 空管道」。这个代价本可以用「启动时校验每个枚举成员都有对应注册」消掉，代码里没有这一步。

**架构推论：** 把「基础方式」与「增量方式」压进同一个一维枚举，是这次错位的根源。更稳的结构是两层：`IndexingMethod`（standard / fast）与 `is_update_run`（bool）各自独立，键由两者组合而成且**组合逻辑只有一处**。现在的结构里组合逻辑确实只有一处（`_get_method`），但枚举同时暴露了组合结果，于是**两条入口通向后一个组合器**而其中一条会重复组合。

**架构推论：** 两个枚举用了两种基类（`str, Enum` 与 `Enum`），说明这套代码没有统一的枚举约定。在一个「值要落 yaml、要进日志、要被 `match`」的系统里，`str, Enum` 是明显更合适的基类；`SearchMethod` 的例外只带来「必须记得 `.value`」这一项负担，没有任何对应收益。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/config/enums.py` | `IndexingMethod` | 四个索引方式，同时是工作流图的注册键 |
| `packages/graphrag/graphrag/config/enums.py` | `SearchMethod` | 四类检索（纯 `Enum` + 手写 `__str__`） |
| `packages/graphrag/graphrag/config/enums.py` | `AsyncType` | 并发原语选择：`asyncio` / `threaded` |
| `packages/graphrag/graphrag/config/enums.py` | `ModularityMetric` | 模块度评估口径：graph / lcc / weighted_components |
| `packages/graphrag/graphrag/config/enums.py` | `NounPhraseExtractorType` | 快速路径的名词短语抽取器（regex / spaCy 依存 / CFG） |
| `packages/graphrag/graphrag/config/enums.py` | `ReportingType` | 日志落地：file / blob，带 YAML 友好的 `__repr__` |
| `packages/graphrag/graphrag/index/workflows/factory.py` | `PipelineFactory.pipelines` / `register_pipeline` | 枚举值 → 工作流名列表的注册表 |
| `packages/graphrag/graphrag/index/workflows/factory.py` | `PipelineFactory.create_pipeline` | 按 method 查表建图；`config.workflows` 优先 |
| `packages/graphrag/graphrag/api/index.py` | `_get_method` | 基础方式 + `is_update_run` → 拼出增量键 |
| `packages/graphrag/graphrag/index/utils/derive_from_rows.py` | `derive_from_rows` | `AsyncType` 的唯一 `match` 消费点 |
| `packages/graphrag/graphrag/graphs/modularity.py` | `calculate_modularity` | `ModularityMetric` 的三分支分派 |

## 参考

- [graphrag 源码：config/enums.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/config/enums.py)
- [graphrag 文档：索引方法](https://microsoft.github.io/graphrag/index/methods/)
- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [Python 文档：`enum`](https://docs.python.org/3/library/enum.html)

## 与主核 / AI 的关系

枚举在主核（多模态数据库内核）里对应的是**存储引擎类型、隔离级别、压缩算法、访问方法**这类「有限选择」。四条经验。

**第一，枚举当注册表键是对的，但注册表必须与枚举同步校验。** 这里的 `PipelineFactory.pipelines` 以枚举值为键，好处是新增方式只需一处注册；坏处是缺注册会静默退化成空管道。主核的访问方法（`USING btree/hash/gin`）同理：注册表与枚举漂移的后果是「语法合法但什么都不做」。**解法是在启动时遍历枚举断言每个成员都已注册**——一个十几行的检查能消掉一整类静默失败。

**第二，永远不要把「派生维度」塞进同一个枚举。** `Standard`/`Fast` 与「是否增量」是两个正交维度，压成一维后必然出现「冗余成员」与「组合逻辑」并存，而两者一旦都被调用方触达就会二次组合。主核里同类风险在「存储后端 × 只读/读写副本」这类组合上：**枚举只放原子维度，组合用配置的两个字段表达，组合逻辑收在一个函数里。**

**第三，同一个概念不要用两种枚举基类。** 五个 `str, Enum` 加一个纯 `Enum` 的混用，代价是「必须记得 `.value`」。判断准则：**只要这个值需要落配置文件、进日志、被 JSON 序列化，就用 `str, Enum`**；纯 `Enum` 只在「绝不出序列化边界」时才有理由。

**第四，函数默认值与配置默认值必须显式对齐。** `derive_from_rows` 默认 `AsyncIO` 而配置默认 `Threaded`，于是「直接调库」与「走流水线」得到不同的并发模型。主核里任何「有配置项、也有独立的库函数入口」的开关（页大小、批量大小、超时）都有同样的风险；**两处默认值不同时，至少要在其中一处注释里说明为什么。**
