---
project: dify
feature: app-config
title: 应用配置与模型参数
page_type: feature
schema_version: 1
refs:
  - path: api/core/app/app_config/entities.py
    symbols: [ModelConfigEntity]
  - path: api/models/model.py
    symbols: [EnabledConfig, EmbeddingModelInfo]
  - path: api/models/model.py
    symbols: [AnnotationReplyDisabledConfig]
---

# 应用配置与模型参数

## 一句话职责

`app_config` 这一层把数据库里存的那份 `app_model_config` 字典，翻译成运行期可用的强类型实体：一半是 pydantic 的 `BaseModel`（`core/app/app_config/entities.py`），一半是 `TypedDict`（`models/model.py`）。前者承载「应用怎么跑」，后者承载「原始配置的形状契约」。

## 机制是什么

同一个「应用配置」在 dify 里有两种类型表示，分工明确：

| 表示 | 位置 | 性质 | 用途 |
|---|---|---|---|
| pydantic `BaseModel` | `core/app/app_config/entities.py` | 校验 + 默认值 + 嵌套对象 | 运行期读取、传递 |
| `TypedDict` | `models/model.py` | 只有静态形状，零运行期开销 | 描述数据库字段的 JSON 结构 |

这是一次**有意的分家**：`TypedDict` 几乎全是「数据库里那一列长什么样」的描述，而 pydantic 实体是「代码里怎么用」。同一份数据在两处各有一份定义，一致性靠人工维护。

`ModelConfigEntity` 是这条链上的枢纽：它是**唯一被 runner 直接拿来做模型调用的实体**。五个字段里没有任何校验逻辑，`parameters` 与 `stop` 都是宽松容器——因为它承载的是用户任意填写的模型参数，无法类型化。

## 怎么实现的

### ModelConfigEntity：五个字段，零约束

```python
class ModelConfigEntity(BaseModel):
    provider: str
    model: str
    mode: str | None = None
    parameters: dict[str, Any] = Field(default_factory=dict)
    stop: list[str] = Field(default_factory=list)
```

三处设计值得注意：

1. **`mode` 是 `str | None` 而不是 `LLMMode` 枚举**。同一个文件里的 `ModelConfig`（另一个类）用的是 `mode: LLMMode`，而 `ModelConfigEntity` 用裸字符串。原因是 `ModelConfigEntity` 会被反序列化到「模型可能不声明 mode」的场景，用枚举会在缺省时报错。
2. **`parameters` 用 `default_factory=dict`**。这避免了可变默认值共享的经典 bug（用 `{}` 直接作默认值会让所有实例共享同一个 dict）。`stop` 同理用 `default_factory=list`。
3. **`parameters` 是「模型调用的唯一参数源」**。a01 那页讲的 `recalc_llm_max_tokens` 直接往这个 dict 里写 `max_tokens`——也就是说这个字段是**被上游改写的**。它不是一个只读快照，而是一个会被 mutate 的活配置。

第 3 点有一个实际后果：**同一个 `ModelConfigEntity` 被多个请求复用时，`max_tokens` 的改写会互相影响**。代码里的规避方式是每个请求都从配置里重新转换出一个新实体（`ModelConfigConverter.convert`），而不是共享实例。

### 提示词模板的三态结构

`PromptTemplateEntity` 用一个 `prompt_type` 加三个可选字段表达两种形态：

- `prompt_type = SIMPLE` → 只用 `simple_prompt_template`（一个字符串）。
- `prompt_type = ADVANCED` → 用 `advanced_chat_prompt_template`（消息列表）或 `advanced_completion_prompt_template`（带 `role_prefix` 的补全模板），**具体用哪个取决于模型的 mode**。

所以「提示词模板」实际是 1 + 2 三种形状，而类型上只有 3 个可选字段。这种「判别字段 + 可选载荷」的写法比联合类型省事，代价是**类型系统无法保证「选了 advanced 就一定有对应载荷」**——校验只能靠运行期（a01 那页看到的 `if not ...: raise InvokeBadRequestError` 正是这个缺口的填补）。

`AdvancedCompletionPromptTemplateEntity` 内部还嵌一个 `RolePrefixEntity(user, assistant)`——补全模型没有消息角色，只能用字符串前缀模拟，所以这两个字段是「伪角色」。

### DatasetRetrieveConfigEntity：一个高度可选的检索配置

这个类有十一个字段，`query_variable` 的注释直接写着「Only when app mode is completion」——**同一个实体承载了只有一种应用形态才用的字段**。这是「一份配置覆盖多种形态」的必然产物。

几个字段的默认值有含义：

- `score_threshold: float | None = 0.0`——默认 0 而不是 `None`，意味着**默认不过滤**（任何分数都收）。
- `reranking_enabled: bool | None = True`——默认开启重排。这个默认值是产品主张：宁可多花一次重排调用，也要结果更好。
- `metadata_filtering_mode: Literal["disabled","automatic","manual"] | None = "disabled"`——三个值的字面量类型而不是枚举，默认关闭。

`metadata_filtering_mode` 用 `Literal` 而 `retrieve_strategy` 用 `StrEnum`，**同一个类里两种写法并存**。这是代码演化的痕迹，不是设计。

`RetrieveStrategy` 与 `PromptType` 都有 `value_of` 类方法，实现都是「遍历枚举找匹配，找不到抛 `ValueError`」——**用 `auto()` 定义的枚举其 `value` 是自增整数，但 `value_of` 拿字符串去比**。这里能正常工作是因为 `StrEnum` + `auto()` 在 Python 3.11+ 会把 `value` 设成成员名的小写形式。这是一个隐式依赖语言版本行为的写法。

### TypedDict 三例：形状契约的三种典型

`EnabledConfig` 是最简形态：`{"enabled": bool}` 一个键。它被用作许多「功能开关」的基形状。

`EmbeddingModelInfo` 只有两个键：`embedding_provider_name` 与 `embedding_model_name`。它描述的是「标注回复功能用了哪个嵌入模型」——注意它**不含任何凭证或连接信息**，只是名字对。这意味着实际调用时要靠这两个名字去模型注册表里查。

`AnnotationReplyDisabledConfig` 与 `AnnotationReplyEnabledConfig` 构成一个**判别联合**：

```python
class AnnotationReplyDisabledConfig(TypedDict):
    enabled: Literal[False]

class AnnotationReplyEnabledConfig(TypedDict):
    id: str
    enabled: Literal[True]
    score_threshold: float
    embedding_model: EmbeddingModelInfo

AnnotationReplyConfig = AnnotationReplyEnabledConfig | AnnotationReplyDisabledConfig
```

这是整份文件里类型表达最精确的一处：`enabled` 用 `Literal[True]` / `Literal[False]` 做判别字段，于是**「关闭」态只有 enabled，而「开启」态的其余四个字段全部必填**。对比 `PromptTemplateEntity` 那种「三个可选字段」的写法，这里选了对的那个做法——因为关闭态确实不需要那些字段，而开启态确实全都要。

`score_threshold` 与 `embedding_model` 只在开启时存在，说明标注回复的匹配是**基于嵌入相似度 + 阈值**的检索：给定用户问题，在标注库里找相似度超过阈值的已有标注直接复用答案。

## 关键取舍

**源码事实：** 应用配置有两种表示（pydantic 实体与 `TypedDict`），前者运行期使用、后者描述存储形状，一致性靠人工维护。

**源码事实：** `ModelConfigEntity.mode` 是 `str | None`，而同文件的 `ModelConfig.mode` 是 `LLMMode` 枚举。

**源码事实：** `ModelConfigEntity.parameters` 会被 `recalc_llm_max_tokens` 原地改写，不是只读快照。

**源码事实：** `PromptTemplateEntity` 用「判别字段 + 三个可选载荷」表达三态，类型系统不保证载荷存在，校验留给运行期。

**源码事实：** `DatasetRetrieveConfigEntity` 含仅补全形态使用的 `query_variable`，且同类内混用 `StrEnum` 与 `Literal`。

**源码事实：** `AnnotationReplyConfig` 是可辨识联合，用 `Literal[True]` / `Literal[False]` 判别，开启态四个字段全必填。

**架构推论：** 「两套类型表示并存」是这类平台型项目的常态。`TypedDict` 的价值在于零运行期开销（它就是 `dict`），适合描述「数据库里的 JSON 列」；pydantic 实体的价值在校验与默认值。**代价是两者漂移时没人报错**——数据库里多了一个键，pydantic 默认会忽略它（除非配了 `extra="forbid"`），于是「配置写了但没生效」这类问题极难排查。

**架构推论：** `ModelConfigEntity.parameters` 被上游改写这件事，说明这个实体在生命周期里**不是不可变值**。这与 a04 那页看到的高级对话 pipeline 把 ORM 对象压成 `frozen` 快照形成对比：同一个系统里，有的配置被冻结，有的被改写。判断标准是「这个东西会不会被跨请求共享」——`ModelConfigEntity` 每请求新建，所以改写安全；工作流快照要跨线程，所以必须冻结。

**架构推论：** `AnnotationReplyConfig` 用可辨识联合是正确的做法，而 `PromptTemplateEntity` 用可选字段是不那么好的做法。两者的差别是「变量之间有没有耦合」：标注回复的 `score_threshold` 只在开启时有意义（强耦合，适合联合类型），而提示词模板的三种载荷在类型上并列（弱耦合，可选字段够用）。**选哪种写法取决于字段之间是否存在前提关系。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/app/app_config/entities.py` | `ModelConfigEntity` | 模型调用的参数载体，可被上游改写 |
| `api/core/app/app_config/entities.py` | `PromptTemplateEntity` | 简单/高级提示词的判别字段 + 可选载荷 |
| `api/core/app/app_config/entities.py` | `AdvancedChatPromptTemplateEntity` | 高级对话的消息模板 |
| `api/core/app/app_config/entities.py` | `AdvancedCompletionPromptTemplateEntity` | 高级补全模板与伪角色前缀 |
| `api/core/app/app_config/entities.py` | `ModelConfig` | 完整模型配置（含 provider/name/mode） |
| `api/core/app/app_config/entities.py` | `DatasetRetrieveConfigEntity` | 十一字段的检索配置，含形态专属字段 |
| `api/core/app/app_config/entities.py` | `DatasetEntity` | 知识库 ID 列表 + 检索配置 |
| `api/core/app/app_config/entities.py` | `AppAdditionalFeatures` | 文件上传、开场白、推荐问题等外围开关 |
| `api/core/app/app_config/entities.py` | `EasyUIBasedAppConfig` | EasyUI 形态的完整配置 |
| `api/models/model.py` | `EnabledConfig` | 最简开关形状 |
| `api/models/model.py` | `EmbeddingModelInfo` | 嵌入模型的名字对，不含凭证 |
| `api/models/model.py` | `AnnotationReplyDisabledConfig` | 标注回复关闭态 |
| `api/models/model.py` | `AnnotationReplyEnabledConfig` | 标注回复开启态，四字段全必填 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/app/app_config/entities.py](https://github.com/langgenius/dify/blob/main/api/core/app/app_config/entities.py)
- [dify 源码：models/model.py](https://github.com/langgenius/dify/blob/main/api/models/model.py)
- [Pydantic 文档：模型与字段校验](https://docs.pydantic.dev/latest/concepts/models/)

## 与主核 / AI 的关系

「配置实体 vs 存储形状」的双表示与主核里「系统目录里的表定义 vs 内存里的 relcache 条目」同构：数据库也维护两份——磁盘上的 `pg_class`/`pg_attribute` 行，与内存里预解析的结构体。主核用**失效机制**保证两者一致（`AcceptInvalidationMessages`）。dify 这里没有任何失效机制，全靠「每次请求重新转换」来绕过不一致——**开销换一致性，是小规模系统最省事的选择。**

`AnnotationReplyConfig` 的判别联合与主核里「堆元组 vs 索引元组」的类型分发是同一种做法：用一个字段区分变体，只在有效的变体上定义字段。数据库的元组头有 `infomask` 位标记，正是判别字段的底层形态。

`ModelConfigEntity.parameters` 被改写这件事与主核的「查询计划被参数化改写」有可比性：计划生成后会因为参数绑定被调整（比如把通用计划切成定制计划）。差别是主核的改写有明确的所有权（计划属于本次查询），这里的改写依赖「实体每请求新建」这个纪律。**前者是结构保证，后者是约定保证。**

对 agent / RAG 工程的实际含义有四条。其一，`score_threshold` 默认 0 表示不过滤，想让低分片段被剔除必须显式抬高阈值。其二，`reranking_enabled` 默认开启，关掉它能省一次调用但会明显降低检索质量。其三，`metadata_filtering_mode` 三态里 `automatic` 会额外调一次模型来生成过滤条件——它是隐性成本。其四，标注回复存的是嵌入模型的名字对而不是向量本身，换嵌入模型等于让历史标注失效。
