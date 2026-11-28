---
project: graphrag
feature: graph-rag-config
title: 总配置结构
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/config/models/graph_rag_config.py
    symbols: [GraphRagConfig]
  - path: packages/graphrag/graphrag/config/defaults.py
    symbols: [ChunkingDefaults, ExtractGraphDefaults, ClusterGraphDefaults]
  - path: packages/graphrag/graphrag/config/load_config.py
    symbols: [load_config]
---

# 总配置结构

## 一句话职责

`GraphRagConfig` 是**一棵只向下的配置树**：27 个顶层字段几乎全是子配置模型（存储、分块、抽取、聚类、社区报告、四类检索各一个），默认值全部来自 `defaults.py` 里那个聚合出来的单例 `graphrag_config_defaults`，而 `load_config` 只是三行委托——它把「找文件、读环境变量、应用命令行覆盖」这些事全交给共享库，自己只提供 `config_initializer=GraphRagConfig` 这一个信息。

## 机制是什么

这套配置的形态由一个事实决定：**graphrag 的索引是一条九步流水线，每一步的参数互不相干。**

分块只管 `size` / `overlap` / `encoding_model`；抽取只管 `entity_types` / `max_gleanings` / 用哪个模型；聚类只管 `max_cluster_size` / `use_lcc` / `seed`；社区报告管 `max_length` / `max_input_length`；嵌入管 `batch_size` 与**要嵌入哪些字段**；四类检索各有自己的 prompt 与 token 预算。若把它们平铺成一个 200 字段的扁平模型，会有两个直接后果：一是「`size` 到底指分块大小还是聚类上限」这类歧义无法靠类型解决，二是**改一处不敢确定没影响别处**。

于是配置被切成「一个聚合根 + N 个分节」。这个选择带来的最重要后果是**工作流的取参方式**：每个 workflow 只 `config.<自己那一节>.<字段>`，从不去看别的节。这就是为什么 `create_communities` 只读 `config.cluster_graph`、`generate_text_embeddings` 只读 `config.embed_text`——**分节边界同时就是 workflow 之间的耦合边界**。

第二个决定性设计是**默认值不在模型里，而在一个 dataclass 单例里**。`GraphRagConfig` 每个字段写的是 `default=graphrag_config_defaults.xxx`，而不是字面量。这换来两件事：所有默认值可以在一个文件里读全；以及**不同层可以共享同一个默认值对象**（`vector_store_defaults` 单独留了一个实例）。代价是「这个字段默认是什么」必须跳一个文件才能看到。

## 怎么实现的

**顶层字段几乎全是子模型，只有 4 个是标量。**

```
class GraphRagConfig(BaseModel):
    completion_models: dict[str, ModelConfig]
    embedding_models: dict[str, ModelConfig]
    concurrent_requests: int
    async_mode: AsyncType

    input: InputConfig
    input_storage: StorageConfig
    chunking: ChunkingConfig
    output_storage: StorageConfig
    update_output_storage: StorageConfig
    table_provider: TableProviderConfig
    cache: CacheConfig
    reporting: ReportingConfig
    vector_store: VectorStoreConfig
    workflows: list[str] | None
    embed_text: EmbedTextConfig
    extract_graph: ExtractGraphConfig
    summarize_descriptions: SummarizeDescriptionsConfig
    extract_graph_nlp: ExtractGraphNLPConfig
    prune_graph: PruneGraphConfig
    cluster_graph: ClusterGraphConfig
    extract_claims: ExtractClaimsConfig
    community_reports: CommunityReportsConfig
    snapshots: SnapshotsConfig
    local_search: LocalSearchConfig
    global_search: GlobalSearchConfig
    drift_search: DRIFTSearchConfig
    basic_search: BasicSearchConfig
```

四个标量里有两个是不属于任何单一 workflow 的**全局执行策略**：`concurrent_requests`（默认 25）与 `async_mode`（默认 `threaded`）。它们被多个 workflow 读取——`extract_graph` 用它俩控制抽取与摘要两条并发线，`create_community_reports` 也读同一对值。**这意味着「并发度」不按阶段可调**，全流水线共用一个数字。

`completion_models` / `embedding_models` 是**命名模型表**（`dict[str, ModelConfig]`），而不是单个模型配置。这个结构决定了下游取模型的形状：

```
extraction_model_config = config.get_completion_model_config(config.extract_graph.completion_model_id)
```

**「用哪个模型」被拆成两跳：字段里存的是 id，id 到 `ModelConfig` 的解析由 getter 完成。** 两个 getter（`get_completion_model_config` / `get_embedding_model_config`）做同一件事：查表，查不到就抛 `ValueError`，消息里带「Please rerun `graphrag init`」。

两跳的好处是**同一模型可被多节复用**（抽取与摘要想用同一个模型，就让两节的 `completion_model_id` 指同一个 key），坏处是**配置里出现悬空 id 时，错误延迟到 workflow 运行时才发生**——第 40 行的 `get_completion_model_config` 要等 `extract_graph` 真正开始跑才被调用。配置在加载时**没有**「所有 `*_model_id` 都指向存在的 key」这类交叉校验。

**`graphrag_config_defaults` 是一个由 22 个分节 dataclass 聚合出来的单例。**

```
@dataclass
class GraphRagConfigDefaults:
    completion_models: dict = field(default_factory=dict)
    concurrent_requests: int = 25
    async_mode: AsyncType = AsyncType.Threaded
    input_storage: InputStorageDefaults = field(default_factory=InputStorageDefaults)
    output_storage: OutputStorageDefaults = field(default_factory=OutputStorageDefaults)
    chunking: ChunkingDefaults = field(default_factory=ChunkingDefaults)
    extract_graph: ExtractGraphDefaults = field(default_factory=ExtractGraphDefaults)
    cluster_graph: ClusterGraphDefaults = field(default_factory=ClusterGraphDefaults)
    ...
    workflows: None = None

graphrag_config_defaults = GraphRagConfigDefaults()
```

三个值得单独记的点：

**第一，每个分节默认值自己是 dataclass，于是「默认值」这一层也是有结构的。**

```
@dataclass
class ChunkingDefaults:
    type: str = ChunkerType.Tokens
    size: int = 1200
    overlap: int = 100
    encoding_model: str = ENCODING_MODEL
    prepend_metadata: None = None

@dataclass
class ExtractGraphDefaults:
    prompt: None = None
    entity_types: list[str] = field(default_factory=lambda: ["organization", "person", "geo", "event"])
    max_gleanings: int = 1
    completion_model_id: str = DEFAULT_COMPLETION_MODEL_ID
    model_instance_name: str = "extract_graph"

@dataclass
class ClusterGraphDefaults:
    max_cluster_size: int = 10
    use_lcc: bool = True
    seed: int = 0xDEADBEEF
```

三组默认值恰好覆盖了三种「默认值类型」：`ChunkingDefaults` 是**纯标量调参**；`ExtractGraphDefaults` 里有一个 **LLM 成本旋钮**（`max_gleanings=1`，即抽取后额外追问一轮）；`ClusterGraphDefaults` 里那个 `0xDEADBEEF` 是**可复现性承诺**（c01 讲过它在 Leiden 调用里的作用）。

**第二，`None` 被当作「用内部默认」的显式标记。** `prompt: None = None`、`prepend_metadata: None = None`、`workflows: None = None`、`graph_prompt: None = None`——这类字段的默认值类型标注是 `None` 而不是 `str | None`。这个写法在类型上是有意收窄的：它声明「默认情况下这个字段不是字符串」，于是下游必须处理 `None` 分支（例如 `config.workflows or cls.pipelines.get(method)`）。

**第三，`ClassVar` 与普通字段混用，导致部分默认值不出现在 `asdict()` 里。**

```
@dataclass
class TextAnalyzerDefaults:
    extractor_type: ClassVar[NounPhraseExtractorType] = NounPhraseExtractorType.RegexEnglish
    model_name: str = "en_core_web_md"
    ...

@dataclass
class InputDefaults:
    type: ClassVar[InputType] = InputType.Text
    ...

@dataclass
class ReportingDefaults:
    type: ClassVar[ReportingType] = ReportingType.file
    ...

@dataclass
class VectorStoreDefaults:
    type: ClassVar[str] = VectorStoreType.LanceDB.value
    db_uri: str = str(Path(DEFAULT_OUTPUT_BASE_DIR) / "lancedb")
```

而 `ChunkingDefaults.type` 却是**普通字段**（`type: str = ChunkerType.Tokens`）。同一个名字在五个 dataclass 里有两种性质：`ClassVar`（类常量，`asdict()` 不含）与普通字段（含）。任何「把 defaults 展开成字典」的代码都会看到不一致的形状——`chunking.type` 出现，`input.type` / `reporting.type` 不出现。

`data_model` 里也有同样的问题要处理——`_validate_vector_store` 之类的方法必须**显式读 `ClassVar`**（例如 `self.reporting.type == ReportingType.file`），而不能指望从 defaults 字典里拿到。**这是「用 dataclass 承载配置默认值」时的一个固定陷阱：`ClassVar` 看着像字段，实际不是。**

**校验是「一个 after-validator 串起五个私有方法」，且它会改写配置。**

```
@model_validator(mode="after")
def _validate_model(self):
    """Validate the model configuration."""
    self._validate_input_base_dir()
    self._validate_reporting_base_dir()
    self._validate_output_base_dir()
    self._validate_update_output_storage_base_dir()
    self._validate_vector_store()
    return self
```

前四个是**同一个模式重复四遍**：

```
def _validate_input_base_dir(self) -> None:
    if self.input_storage.type == StorageType.File:
        if not self.input_storage.base_dir:
            msg = "input storage base directory is required for file input storage. Please rerun `graphrag init` and set the input storage configuration."
            raise ValueError(msg)
        self.input_storage.base_dir = str(Path(self.input_storage.base_dir).resolve())
```

四处几乎逐字相同（只是换成 output / update_output / reporting 与对应的错误文案）。重复的理由是**错误消息必须指名是哪一节**——四个目录各自的报错要能定位到自己的配置块。但换个角度看，这四段可以合成一个带参数的方法；没有合，说明作者选择了「四段显式代码 + 四条独立消息」而不是「一个循环 + 拼接的消息」。

**这里最值得注意的是它做的事不只是校验——它还改写。**

```
self.input_storage.base_dir = str(Path(self.input_storage.base_dir).resolve())
```

同一个方法里既检查「必须非空」，又把相对路径**解析成绝对路径**。于是 `load_config` 返回的配置里所有目录字段都已经是绝对路径。这个副作用有三层后果：

1. **下游不需要再关心 cwd。** 所有 workflow 拿到的 `base_dir` 都是绝对路径，即使之后有人改了工作目录也不会错。
2. **配置对象不再是「输入的回声」。** `config.input_storage.base_dir` 与 yaml 里写的值可能不同（`"input"` → `"/abs/path/input"`）。任何「把 config 再序列化回去」的代码都会写出绝对路径。
3. **校验与归一化被绑在一个方法里。** 想「只校验不改写」做不到——这两个职责没有分开的入口。

`_validate_vector_store` 是唯一做另一类事的：

```
def _validate_vector_store(self) -> None:
    """Validate the vector store configuration specifically in the GraphRAG context. This checks and sets required dynamic defaults for the embeddings we require."""
    self._validate_vector_store_db_uri()
    # check and insert/overlay schemas for all of the core embeddings
    # note that this does not require that they are used, only that they have a schema
    # the embed_text block has the list of actual embeddings
    if not self.vector_store.index_schema:
        self.vector_store.index_schema = {}
    default_vector_size = self.vector_store.vector_size
    for embedding in all_embeddings:
        if embedding not in self.vector_store.index_schema:
            self.vector_store.index_schema[embedding] = IndexSchema(
                index_name=embedding,
                vector_size=default_vector_size,
            )
```

**它给「所有核心嵌入字段」都补上一个 schema，哪怕其中大部分这次根本不生成。** 注释把理由写得很清楚：「note that this does not require that they are used, only that they have a schema」——真正的选择在哪几个字段要嵌入，是 `embed_text.names` 决定的（b05），而 schema 表必须先齐。

这个「先给全部、再由别的字段裁掉」的顺序是必要的：`generate_text_embeddings` 里是 `config.vector_store.index_schema[field_config.name]`（**直接下标，没有 `.get`**）。若这里不预先铺满，一个「用户显式写了 `embed_text.names` 但没写 `index_schema`」的配置会在**运行时**撞 `KeyError`。所以这段是「把运行期可能缺的东西在加载期铺平」。

而 `_validate_vector_store_db_uri` 里那处有条件覆盖也值得记：

```
if store.type == VectorStoreType.LanceDB:
    if not store.db_uri or store.db_uri.strip() == "":
        store.db_uri = graphrag_config_defaults.vector_store.db_uri
    store.db_uri = str(Path(store.db_uri).resolve())
```

**注意它是「空串才回退默认值」，然后无条件 resolve。** 所以用户写 `db_uri: ""` 会得到默认路径（并被 resolve），而写相对路径会被转绝对——与四个 `base_dir` 的处理是同一模式，只是多了一层「空值回退」。

**`load_config` 只有三行，它的全部信息量在签名和 docstring 里。**

```
def load_config(
    root_dir: str | Path,
    cli_overrides: dict[str, Any] | None = None,
) -> GraphRagConfig:
    """Load configuration from a file.

    ...
    root_dir : str | Path
        The root directory of the project.
        Searches for settings.[yaml|yml|json] config files.
    cli_overrides : dict[str, Any] | None
        A nested dictionary of cli overrides.
        Example: {'output': {'base_dir': 'override_value'}}
    ...
    :raises ConfigParsingError: If there was an error parsing the config file or its environment variables.
    """
    return lc(
        config_initializer=GraphRagConfig,
        config_path=root_dir,
        overrides=cli_overrides,
    )
```

四处可读出的信息：

1. **`root_dir` 是目录而不是文件路径**，文件名是约定好的三种候选（`settings.yaml` / `settings.yml` / `settings.json`）。于是「换配置文件」不是靠参数，而是靠换目录。
2. **`cli_overrides` 是嵌套 dict 而非扁平键值。** 例子 `{'output': {'base_dir': 'override_value'}}` 说明它按配置树的层级嵌套——这正是「配置是树」这个设计在覆盖机制上的延续。扁平写法（`{"output.base_dir": ...}`）会让覆盖层无法与模型结构对照。
3. **docstring 的 `raises` 段里提到了 environment variables**，说明共享库实现了环境变量插值/覆盖。但**这一层完全不在本仓库的这三个文件里**——优先级、插值语法、是否支持 `${VAR:-default}`，都要去 `graphrag_common.config.load_config` 读。这是这套配置最需要外部知识的一处。
4. **`config_initializer=GraphRagConfig` 是唯一的注入点。** 换个根模型就换一套配置，其余逻辑全复用。这让「共享配置加载」与「各包自己的配置结构」解耦。

**`__repr__` 与 `__str__` 的分工也是刻意的。**

```
def __repr__(self) -> str:
    return pformat(self, highlight=False)

def __str__(self):
    return self.model_dump_json(indent=4)
```

`repr` 用 `devtools.pformat`（带类型与字段名，适合读结构），`str` 用 JSON（适合喂给日志或落盘）。**两个表示服务两个场景**，而不是让 `str` 复用 `repr`——因为配置对象最常被打印的地方是启动日志，那里需要的是**能直接复制进 yaml 的形态**。

## 关键取舍

**源码事实：** `GraphRagConfig` 有 27 个顶层字段，其中只有 `completion_models` / `embedding_models` / `concurrent_requests` / `async_mode` 四个不是子配置模型；`concurrent_requests`（默认 25）与 `async_mode`（默认 `threaded`）被多个 workflow 共同读取，不按阶段可调。

**源码事实：** 所有字段默认值来自模块级单例 `graphrag_config_defaults`（`GraphRagConfigDefaults()` 实例），该 dataclass 由 22 个分节 `*Defaults` dataclass 聚合而成；另有一个独立的 `vector_store_defaults` 实例。

**源码事实：** `input` / `input_storage` / `output_storage` / `update_output_storage` / `reporting` / `vector_store` 的 `type` 字段在各自的 `*Defaults` 里被声明为 `ClassVar`（`asdict()` 不含），而 `ChunkingDefaults.type` 是普通字段——同名不同性质。

**源码事实：** `_validate_model` 是一个 `@model_validator(mode="after")`，按固定顺序调用五个私有方法；前四个（input / reporting / output / update_output）是同一模式的四次重复（检查 File 类型下 `base_dir` 非空 + resolve 成绝对路径），错误消息各自指名所属配置节。

**源码事实：** 四个 base_dir 校验**会改写配置**（把相对路径 resolve 成绝对路径），因此 `load_config` 返回的配置对象与文件里的字面值可能不同，且配置对象不能被当作「输入的回声」再序列化回去。

**源码事实：** `_validate_vector_store` 为 `all_embeddings` 里的**全部**核心嵌入字段补默认 `IndexSchema`，即使这次只有一部分会被生成（实际选择由 `embed_text.names` 决定）；注释明确「does not require that they are used, only that they have a schema」。这是为了让 `generate_text_embeddings` 里的 `index_schema[name]` 直接下标不会 `KeyError`。

**源码事实：** 模型 id 的解析走两跳（字段存 id → `get_completion_model_config` / `get_embedding_model_config` 查命名表），查不到时抛 `ValueError` 且消息提示「rerun `graphrag init`」。配置加载期**不做**「所有 `*_model_id` 都指向存在的 key」这类交叉校验。

**源码事实：** `load_config` 只是把 `config_initializer=GraphRagConfig` / `config_path=root_dir` / `overrides=cli_overrides` 传给共享库；`root_dir` 是目录（搜索 `settings.{yaml,yml,json}`），`cli_overrides` 是按配置树嵌套的 dict；环境变量处理在共享库内（docstring 的 `raises` 提到 ConfigParsingError 覆盖「config file or its environment variables」）。

**源码事实：** 多个字段用 `None` 作为「用内部默认」的显式标记，且类型标注就是 `None`（如 `prompt: None = None`、`workflows: None = None`），迫使下游必须处理该分支。

**架构推论：** 「配置树的分节边界 = workflow 的耦合边界」是这个结构最大的收益。每个 workflow 只读自己那一节，于是改一个参数的影响面在阅读时就能确定。代价是**跨节约束无处安放**：模型 id 是否悬空、`embed_text.names` 与 `index_schema` 是否一致、`chunking.size` 与 `community_reports.max_input_length` 是否匹配——这些跨节问题只能靠运行期各自报错。

**架构推论：** 校验器同时做归一化（resolve 路径），是把「便宜且总要做」的变换放进了唯一必然被执行的钩子。收益是下游无需关心 cwd；代价是「配置对象」不再是纯数据——它承载了「在哪个 cwd 下解析」这个隐含信息，而该信息没有出现在任何字段里。若同一份 yaml 在两个不同目录加载，得到的两个配置**不相等**，而这在 diff 时会表现为「莫名其妙的全字段变化」。

**架构推论：** `ClassVar` 与普通字段混用，是「用 dataclass 表达配置默认值」的固有陷阱。`ClassVar` 不参与 `__init__`、不参与 `asdict()`，但它读起来的语义与字段一样；写 `self.reporting.type == ReportingType.file` 的代码证明作者自己是知道要显式读的，但任何自动化的「默认值清单」都会漏掉这些项。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/config/models/graph_rag_config.py` | `GraphRagConfig` | 27 字段配置树根（聚合全部子配置） |
| `packages/graphrag/graphrag/config/models/graph_rag_config.py` | `GraphRagConfig._validate_model` | `after` 校验器：串起五个校验/归一化方法 |
| `packages/graphrag/graphrag/config/models/graph_rag_config.py` | `_validate_input_base_dir` / `_validate_output_base_dir` / `_validate_update_output_storage_base_dir` / `_validate_reporting_base_dir` | 四次重复的「File 类型必须给 base_dir」+ resolve 绝对路径 |
| `packages/graphrag/graphrag/config/models/graph_rag_config.py` | `_validate_vector_store` / `_validate_vector_store_db_uri` | 为全部核心嵌入字段补 `IndexSchema`；LanceDB 的空 `db_uri` 回退默认 |
| `packages/graphrag/graphrag/config/models/graph_rag_config.py` | `get_completion_model_config` / `get_embedding_model_config` | 命名模型表的两跳解析（查不到抛 `ValueError`） |
| `packages/graphrag/graphrag/config/defaults.py` | `GraphRagConfigDefaults` / `graphrag_config_defaults` | 22 个分节默认值的聚合与单例 |
| `packages/graphrag/graphrag/config/defaults.py` | `ChunkingDefaults` | 分块默认：`size=1200` / `overlap=100` / `encoding_model=o200k_base` |
| `packages/graphrag/graphrag/config/defaults.py` | `ExtractGraphDefaults` | 抽取默认：四类实体 + `max_gleanings=1` + 复用 `default_completion_model` |
| `packages/graphrag/graphrag/config/defaults.py` | `ClusterGraphDefaults` | 聚类默认：`max_cluster_size=10` / `use_lcc=True` / `seed=0xDEADBEEF` |
| `packages/graphrag/graphrag/config/load_config.py` | `load_config` | 三行委托：注入根模型，其余交给共享加载器 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：配置](https://microsoft.github.io/graphrag/config/overview/)
- [graphrag 源码：graph_rag_config.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/config/models/graph_rag_config.py)
- [pydantic 文档：model validators](https://docs.pydantic.dev/latest/concepts/validators/#model-validators)

## 与主核 / AI 的关系

配置系统在主核（多模态数据库内核）里对应的是**实例级参数文件**（页大小、WAL 模式、缓冲池大小、各存储引擎的连接串）。四条经验。

**第一，配置树的分节应当对齐执行阶段的边界。** 这里每个 workflow 只读自己那一节，于是「改这个参数影响谁」在阅读时即可判定。主核的参数文件若按引擎分节（storage / index / query / modalities），同样能获得这个性质；反之若把所有旋钮平铺，最典型的症状是「调了一个参数不知道会影响哪条路径」。**判断准则：如果两个参数从不被同一段代码读取，它们就该在不同的节里。**

**第二，默认值集中到一个单例，比散落在各模型里更好维护，但会让 `ClassVar` 这类陷阱变得危险。** 这里 `type` 字段在五个 defaults 里两种性质混用，导致「展开默认值」的代码必须显式处理。主核写结构体默认值时同理：**任何「看起来是字段但不是字段」的成员（编译期常量、枚举别名）都必须有明确标注，否则自动生成的配置文档会漏项且无人发现。**

**第三，校验器里夹归一化是省事但会留下隐性信息。** 这里把相对路径 resolve 成绝对路径，于是「配置对象」记住了加载时的 cwd。主核的参数加载若也做路径归一化，必须意识到两个后果：一是同一份配置在不同 cwd 下得到不等的对象（diff 噪音），二是**配置无法原样回写**。若需要「读进来再写回去」的往返（`ALTER SYSTEM RESET` 之类），归一化就必须与校验分开。

**第四，跨节约束无法在分节结构里安放，只能靠运行期报错——所以要尽量把它前移。** 这里「模型 id 悬空」与「`embed_text.names` 不在 `index_schema` 里」都是运行期才炸；而 `_validate_vector_store` 那一段正是把后者前移到了加载期（铺满 schema 让直接下标不会失败）。**凡是「加载期能铺平的东西都不要留给运行期」**——代价只是一个可能用不到的默认项，收益是消掉一整类 `KeyError`。
