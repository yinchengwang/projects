---
project: lightrag
feature: query-param
title: 查询参数与基础类型
page_type: feature
schema_version: 1
refs:
  - path: lightrag/base.py
    symbols: [QueryParam, TextChunkSchema]
  - path: lightrag/types.py
    symbols: [ExtractedEntity, ExtractedRelationship, EntityExtractionResult]
---

# 查询参数与基础类型

## 一句话职责

`QueryParam` 把一次查询的全部可变面收进一个**普通 dataclass**（17 个字段，其中 6 个的默认值取自环境变量），`TextChunkSchema` 用 `TypedDict` 固定文本块在存储里的四个键；而 `lightrag/types.py` 里那三个 pydantic 模型，是**抽取结果线上格式的一份声明式文档——它没有被任何代码用 `model_validate` 消费过**。

## 机制是什么

这一页的三样东西性质完全不同，混在一起看反而能看出一个共同问题：**「有类型声明」与「有类型检查」在这套代码里是两件事。**

1. `QueryParam` 是 dataclass 而不是 pydantic 模型。它**不做任何校验**——`top_k=-5`、`mode="typo"`、`max_total_tokens=0` 都能构造成功。字段类型注解只服务编辑器与类型检查器，运行期一律放行。这个选择是有意的：它要能被低成本地、按位置地构造（SDK 调用方会这么用），而 pydantic 的校验成本与「只认关键字」的约束都不合适。
2. `TextChunkSchema` 是 `TypedDict`，意味着它**在运行期完全不存在**——它只让 mypy 知道「一个文本块有这四个键」，而真正决定块里有什么的是各个写入点。
3. `types.py` 的三个 pydantic 模型**有完整的运行期校验能力，却没有任何调用点**。全文只在它们自己的定义处出现，另一处出现是在一个测试模块的 docstring 里。

第三种情况最值得先看清。它揭示的是：**pydantic 模型在这里被当成「规格说明」而不是「校验关卡」**。真实的抽取结果解析走的是手写的宽容路径（`operate.py` 里的 `_process_json_extraction_result`），它读 `parsed.get("entities")`、`entity_data.get("name")`，逐个字段做清洗与容错。为什么不直接用 `EntityExtractionResult.model_validate(...)`？因为**真实的 LLM 输出会脏**——多一层校验会大量失败，而手写路径可以对每个字段单独降级。

## 怎么实现的

**`QueryParam` 的默认值是「环境变量的快照」，不是常量。**

```
@dataclass
class QueryParam:
    mode: Literal["local", "global", "hybrid", "naive", "mix", "bypass"] = "mix"
    only_need_context: bool = False
    only_need_prompt: bool = False
    response_type: str = "Multiple Paragraphs"
    stream: bool = False
    top_k: int = get_env_value("TOP_K", DEFAULT_TOP_K, int)
    chunk_top_k: int = get_env_value("CHUNK_TOP_K", DEFAULT_CHUNK_TOP_K, int)
    max_entity_tokens: int = get_env_value("MAX_ENTITY_TOKENS", DEFAULT_MAX_ENTITY_TOKENS, int)
    max_relation_tokens: int = get_env_value("MAX_RELATION_TOKENS", DEFAULT_MAX_RELATION_TOKENS, int)
    max_total_tokens: int = get_env_value("MAX_TOTAL_TOKENS", DEFAULT_MAX_TOTAL_TOKENS, int)
    hl_keywords: list[str] = field(default_factory=list)
    ll_keywords: list[str] = field(default_factory=list)
    conversation_history: list[dict[str, str]] = field(default_factory=list)
    user_prompt: str | None = None
    enable_rerank: bool = get_env_value("RERANK_BY_DEFAULT", True, bool)
    include_references: bool = False
    disable_user_prompt_prefix: bool = False
```

三个要点：

1. **`get_env_value(...)` 在类体里就被求值一次，是模块导入时刻的环境快照。** 也就是说：导入 `lightrag.base` 之后再改 `os.environ["TOP_K"]`，对新建的 `QueryParam` **没有任何影响**。这个行为与 `lightrag.py` 里那些 `Field(default=get_env_value(...))` 一致（同样的求值时机），是这套代码的统一约定——但也意味着**测试里不能用「改环境变量 + 新建对象」的方式验证默认值**，必须 `monkeypatch` + `reload`。
2. **五个 token 预算参数各有独立环境变量**（`TOP_K` / `CHUNK_TOP_K` / `MAX_ENTITY_TOKENS` / `MAX_RELATION_TOKENS` / `MAX_TOTAL_TOKENS`）。五个数的关系是有意区分的：`top_k` 管实体/关系条数、`chunk_top_k` 管文本块条数、后三个管传给 LLM 的 token 上限（实体 / 关系 / 总量）。**「条数」与「token 数」是两把不同的尺子**，同时存在是因为：条数控制候选集的规模，token 数控制最终进 prompt 的体积——同一条数在不同文档上可以是几十 token 也可以是几千。
3. **`mode` 的 `Literal` 有六个值，但 docstring 只列了五个。**

```
"""Specifies the retrieval mode:
- "local": Focuses on context-dependent information.
- "global": Utilizes global knowledge.
- "hybrid": Combines local and global retrieval methods.
- "naive": Performs a basic search without advanced techniques.
- "mix": Integrates knowledge graph and vector retrieval.
"""
```

`bypass` 在 `Literal` 里、在默认值逻辑里存在，却不在这个枚举式 docstring 里——它在 `user_prompt` 的文档里被顺带提到（「``bypass`` mode ignores this field entirely」）。**同一份信息被拆到两处，且主文档不全**，这类遗漏在「字段级 docstring」的风格里很常见。

**`chunk_top_k` 的 docstring 说它可以是 `None`，但类型注解不是。**

```
chunk_top_k: int = get_env_value("CHUNK_TOP_K", DEFAULT_CHUNK_TOP_K, int)
"""Number of text chunks to retrieve initially from vector search and keep after reranking.
If None, defaults to top_k value.
"""
```

**注解写 `int`，文档说「If None」。** 这是一处真实的不一致，但**行为是对的**——因为消费点用的是真值回退：

```
search_top_k = query_param.chunk_top_k or query_param.top_k
```

`or` 让它同时接受 `None` 与 `0`，两者都回退到 `top_k`。所以在 dataclass 不做校验的前提下，`QueryParam(chunk_top_k=None)` 是能工作的，尽管类型检查器会报错。**这是「没有运行期校验」带来的意外柔性**：错误的类型注解没能拦住任何东西，反而让一个文档承诺的行为真的能跑。

**`disable_user_prompt_prefix` 被刻意放在最后一个字段，注释写得非常具体。**

```
# Declared last on purpose. QueryParam is a plain dataclass, so SDK callers
# may construct it positionally; inserting a field beside `user_prompt`
# would silently rebind every positional argument after it (a positional
# `False` meant for `enable_rerank` would land here instead). New fields
# belong at the end.
disable_user_prompt_prefix: bool = False
```

**这段注释是整页最有价值的一处**，它把一个只在「用位置参数构造」时才暴露的破坏性变更讲清了：

- 普通 dataclass 的字段顺序**就是** `__init__` 的签名顺序。
- 在中间插入一个字段，会让它之后所有位置参数**静默错位**。
- 而错位的后果不是异常，是「一个本该给 `enable_rerank` 的 `False` 落到了新字段上」——两个布尔互换，没有任何报错。

所以「新字段一律加在末尾」不是风格偏好，而是**这条 API 的兼容性规则**。而这个规则之所以必须靠注释维护，是因为**dataclass 没有机制强制它**（不像 `kw_only=True` 可以从根上消除位置构造）。

顺带一个观察：这段注释自己也在论证「为什么当初没用 `kw_only=True`」——因为 SDK 调用方已经在用位置参数了。

**`user_prompt` 的语义比字段名复杂得多，docstring 用了一半篇幅讲「它什么时候不生效」。**

```
The server may prepend a global prefix (``USER_PROMPT_PREFIX`` /
``LightRAG.user_prompt_prefix``) to this value. **If this field is empty,
the prefix alone becomes the instructions sent to the LLM** -- an empty
``user_prompt`` does not disable the prefix. Set
``disable_user_prompt_prefix`` (declared at the end of this class) to opt
out instead.

Two paths do not receive the prefix: ``bypass`` mode ignores this field
entirely (empty or not), and a caller-supplied ``system_prompt`` without a
``{user_prompt}`` placeholder silently drops it.
```

这里有三个独立事实，每一个都是「直觉会猜错」的地方：

1. **空 `user_prompt` 不等于「不加指令」，而是「只加服务端前缀」。** 直觉上「我没填 prompt」应当意味着「没有额外指令」，实际是「运营配置的全局前缀成为指令」。这是一个安全相关的设计：全局前缀是运营方的约束，请求方只能选择退出，无法读取或替换它。
2. **`disable_user_prompt_prefix` 是唯一的退出开关**，而不是「把 `user_prompt` 清空」。两个字段一个管内容、一个管开关，职责清晰。
3. **还有两条路径绕过前缀**：`bypass` 模式整个忽略 `user_prompt`；调用方自带 `system_prompt` 且其中没有 `{user_prompt}` 占位符时，它被静默丢弃。

第 3 条里的「silently」值得单独标出：**一条静默丢弃路径**。它存在是因为「调用方给了完整 system_prompt」这个场景下没有地方安插前缀，但后果是「我设了 `user_prompt` 却没生效」而没有任何提示。

**`TextChunkSchema` 只有四个键，且不含自己的 id。**

```
class TextChunkSchema(TypedDict):
    tokens: int
    content: str
    full_doc_id: str
    chunk_order_index: int
```

四点的含义：

1. **没有 `id` 字段**，因为 id 是存储里的**键**（`chunks` 表的 key），而不是值里的一个域。这是键值存储的常见形状——把主键同时存在值里会产生两个真相来源。
2. **`tokens` 是预先算好并落盘的**，不是运行时再算。因为分块时已经算过一次（要控制 `chunk_token_size`），存下来避免重复分词——而分词是这套系统里最贵的纯 CPU 操作之一。
3. **`full_doc_id` 是回指**，让「这块来自哪份文档」可以单向查到；反向的 `chunks_list` 存在 `DocProcessingStatus` 里（a02 讲过）。
4. **`chunk_order_index` 是顺序**，用于把检索到的若干块重新排回原文顺序。

**它是 `TypedDict` 而不是 dataclass 或 pydantic，理由很实际**：文本块在存储里就是一个 `dict`，读出来直接当 dict 用；若定义成 dataclass 就要在每次读写时多一层转换。`TypedDict` 的价值纯粹在静态检查侧——让 `chunks: dict[str, TextChunkSchema]` 这种注解能表达「值长什么样」。

**`types.py` 的三个 pydantic 模型与真实线上格式对不上。**

```
class ExtractedEntity(BaseModel):
    """A single entity extracted from text by the LLM."""

    entity_name: str = Field(description="Name of the entity. ...")
    entity_type: str = Field(description="Type/category of the entity.")
    entity_description: str = Field(description="Concise yet comprehensive description ...")


class ExtractedRelationship(BaseModel):
    """A single relationship between two entities extracted from text."""

    source_entity: str = Field(...)
    target_entity: str = Field(...)
    relationship_keywords: str = Field(...)
    relationship_description: str = Field(...)


class EntityExtractionResult(BaseModel):
    """Structured output format for entity and relationship extraction from text."""

    entities: list[ExtractedEntity] = Field(default_factory=list, ...)
    relationships: list[ExtractedRelationship] = Field(default_factory=list, ...)
```

**把这三个模型和真实链路对照，会发现字段名一个都对不上。**

提示词要求模型输出的是（`entity_extraction_json_user_prompt` → `entity_extraction_json_system_prompt`）：

| 实际线上字段 | pydantic 模型字段 |
|---|---|
| `name` / `type` / `description` | `entity_name` / `entity_type` / `entity_description` |
| `source` / `target` / `keywords` / `description` | `source_entity` / `target_entity` / `relationship_keywords` / `relationship_description` |

而解析端读的正是线上字段：

```
entity_data.get("name")        # operate.py
entity_data.get("type")
entity_data.get("description")
rel_data.get("source")
rel_data.get("target")
rel_data.get("keywords")
rel_data.get("description")
```

**只有最外层的 `EntityExtractionResult`（`entities` / `relationships` 两个键）是吻合的。**

这三件事合起来说明：**这三个模型从来没有被喂过真实 LLM 输出**。若 `EntityExtractionResult.model_validate(parsed)` 被调用过，元素层的字段名会全部缺失，`ExtractedEntity` 会因为 `entity_name` 无默认值而直接抛 `ValidationError`——所以它不可能在链路上。全仓检索也确认了这一点：除了定义处，只有一处测试模块的 docstring 提到它（该测试实际检查的是提示词与常量，并不实例化模型）。

**那为什么还留着？** 因为 `EntityExtractionResult` 的外层形状确实描述了 JSON 模式的输出契约，`Field(description=...)` 里的文字也确实在讲「该抽什么」。它是一份**声明式规格**，价值在于「读这个文件就能知道 JSON 模式长什么样」，而不在于运行期把关。代价是：

- **它是一份会漂移的规格**：字段名已经与实际输出脱节，任何按它去写对接代码的人都会失败。
- **它给人一种「有校验」的错觉**：看到 pydantic 模型会以为链路上有 schema 校验，实际没有——链路上的容错是手写的、逐字段的。

**而手写容错的具体形状解释了为什么不能用校验器。** 真实解析对每个字段单独降级：

- 顶层不是 dict → 记一条 warning 后返回空结果，不抛。
- `entities` 不是 list → 记 warning，当成空列表继续处理 `relationships`。
- 单个 entity 不是 dict → `continue` 跳过这一条。
- 名字清洗后为空 → `continue`。
- 描述为空 → `continue`。
- **每条记录的整段处理包在 `try/except Exception` 里**，失败只记 warning 然后继续下一条。

**这个形状与「`model_validate` 全有或全无」正好相反**：手写路径的语义是「尽最大努力保留能用的部分」，校验器的语义是「有任何一个字段不合规就整体拒绝」。对 LLM 输出这种必然有噪声的输入，前者才是对的——一批 50 个实体里坏 3 个，丢掉 3 个远比丢掉 50 个好。

还有一处更细的修复，同样只能在手写路径里做：模型在描述里写 LaTeX 时经常把反斜杠转义不足（`\frac` 在 JSON 里被读成「换页符 + rac」），代码在清洗前先调 `repair_vlm_json_escape_damage_nested` 把零风险的情况修回来。这类「先修数据再验」的步骤，在严格的 schema 校验下没有位置。

## 关键取舍

**源码事实：** `QueryParam` 是普通 `dataclass`，无任何运行期校验；17 个字段全部有默认值，其中 `top_k` / `chunk_top_k` / `max_entity_tokens` / `max_relation_tokens` / `max_total_tokens` / `enable_rerank` 六个的默认值由 `get_env_value(...)` 在**模块导入时**求值，是环境快照。

**源码事实：** `mode` 的 `Literal` 有六个值（含 `bypass`），但字段 docstring 只列了五个；`bypass` 的实际语义（完全忽略 `user_prompt`）写在另一个字段的文档里。

**源码事实：** `chunk_top_k` 类型注解是 `int`，但 docstring 写「If None, defaults to top_k value」；消费点用 `query_param.chunk_top_k or query_param.top_k`，故 `None` 与 `0` 都真的会回退——注解与行为不一致，但因为无校验所以行为容量比注解更宽。

**源码事实：** `disable_user_prompt_prefix` 被刻意声明在字段列表最后，注释说明理由：dataclass 字段顺序即 `__init__` 位置参数顺序，在中间插入字段会让其后所有位置参数静默错位（举例：本该给 `enable_rerank` 的 `False` 会落到新字段上）。规则是「新字段一律加在末尾」。

**源码事实：** `user_prompt` 的文档记录了三条反直觉行为：空 `user_prompt` 时服务端前缀单独成为指令；退出要用 `disable_user_prompt_prefix` 而非清空字段；`bypass` 模式整个忽略该字段，而调用方自带的 `system_prompt` 若无 `{user_prompt}` 占位符则**静默丢弃**它。

**源码事实：** `TextChunkSchema` 是 `TypedDict`，四个键 `tokens` / `content` / `full_doc_id` / `chunk_order_index`，**不含自身 id**（id 是存储键）；`tokens` 落盘避免重复分词。

**源码事实：** `ExtractedEntity` / `ExtractedRelationship` 的字段名（`entity_name` / `source_entity` / `relationship_keywords` …）与实际提示词要求、与实际解析读的键（`name` / `source` / `keywords` …）**全部不一致**；只有外层 `EntityExtractionResult` 的 `entities` / `relationships` 吻合。全仓检索确认这三个模型除定义处外无任何调用点。

**源码事实：** 真实 JSON 解析路径对每个字段单独降级（顶层非 dict → 空结果；`entities` 非 list → 当空；元素非 dict / 名字空 / 描述空 → `continue`；每条记录整段 `try/except Exception` 只记 warning），并在清洗前先调 `repair_vlm_json_escape_damage_nested` 修复 LaTeX 反斜杠损伤。

**架构推论：** 「dataclass + 无校验」与「pydantic + 无调用点」是同一个取舍的两面：**这套代码把校验放在最靠近边界的手写解析里，而不是放在类型声明上**。好处是每个字段可以按自己的方式降级（LLM 输出必须如此）；代价是类型注解会漂移、docstring 里的承诺（「If None」）没有机制保证、以及声明式规格最终会与实现脱节——`types.py` 就是脱节之后的样子。

**架构推论：** 「新字段加在末尾」这条规则必须靠注释维护，说明 `QueryParam` 的公开接口在结构上是不稳定的。`kw_only=True` 能从根上消除这个风险，但它会破坏已有 SDK 的位置调用；这是一个**已付出的兼容成本已经锁死了更安全设计**的典型例子。任何「后期想改成 kw_only」的尝试，都要先确认真实调用方是否用位置参数。

**架构推论：** `TextChunkSchema` 不含 `id` 是好设计，值得当成通则：**主键存在于键里，就不要在值里再存一份**。两个副本必然会有不一致的那一天，而那时没有正确答案。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/base.py` | `QueryParam` | 一次查询的全部可变面（17 字段，6 个默认值取自环境） |
| `lightrag/base.py` | `QueryParam.mode` | 六级检索模式 `Literal`（含未列入 docstring 的 `bypass`） |
| `lightrag/base.py` | `QueryParam.disable_user_prompt_prefix` | 声明在末尾的退出开关（位置参数兼容性规则） |
| `lightrag/base.py` | `TextChunkSchema` | 文本块存储形状：`tokens` / `content` / `full_doc_id` / `chunk_order_index` |
| `lightrag/types.py` | `ExtractedEntity` | 单实体抽取结果模型（字段名与实际线上格式不符，无调用点） |
| `lightrag/types.py` | `ExtractedRelationship` | 单关系抽取结果模型（同上） |
| `lightrag/types.py` | `EntityExtractionResult` | JSON 模式输出契约的声明式规格（外层两键与线上一致） |
| `lightrag/utils.py` | `get_env_value` | 环境变量读取与类型转换；非法值回退默认并告警 |
| `lightrag/operate.py` | `_process_json_extraction_result` | 真实 JSON 解析：逐字段降级 + 逐条 `try/except` + 转义修复 |

## 参考

- [lightrag 源码：base.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/base.py)
- [lightrag 源码：types.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/types.py)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [pydantic 文档：`BaseModel`](https://docs.pydantic.dev/latest/concepts/models/)

## 与主核 / AI 的关系

这一页对应主核里两件很具体的事：**查询参数结构**与**外部输入的 schema 校验边界**。四条经验。

**第一，「声明式规格」与「校验关卡」必须分清名字与位置。** `types.py` 的三个模型看起来是校验器，实际是文档；真实校验在 `operate.py` 的手写解析里。在一个 LLM 抽取类的系统里这是必然的——**上游是非确定性生成器，输入必然脏，全有或全无的校验会大面积拒绝**。主核若对接外部数据源（CSV 导入、第三方 API 落库），同样会遇到「按 schema 严格拒绝」还是「逐字段降级并记账」的选择。对**可重试的数据源**严格校验是对的（让人修数据再重跑）；对**不可重试的一次性流**必须降级（丢掉三个坏实体比丢掉五百个好）。判断依据是「坏了之后能不能重来」。

**第二，逐字段降级要配一个「丢弃记账」。** 这里的降级点各打一条 warning，且区分了「名字空」用 `info`、「描述空」用 `warning`。主核做宽容解析时同样必须让每个丢弃可见——**静默降级比直接失败更危险**，因为它把「数据质量在下降」这件事藏起来了。

**第三，位置参数兼容性一旦付出就无法回收。** `QueryParam` 把新字段钉在末尾、并写了长注释解释原因，这是「已经有人按位置调用」之后的唯一可行做法。主核里凡是会被外部 SDK 按位置调用的结构体（C 结构体初始化、Go 的非命名 struct literal），从第一版起就该考虑 `kw_only` 或「只允许命名初始化」——**这个决定只能在还没人调用时做**。

**第四，环境变量默认值必须在类型定义处可追踪。** 这里的六个字段默认值来自 `get_env_value`，于是「这个参数的默认值是多少」不能用「看代码」回答，要看部署环境。主核做配置化时同理：**默认值来自环境/配置文件的字段，应当有一处集中列出「名字 + 默认值 + 环境变量名」的清单**，否则运维与开发会长期各说一套。这里的代价已经很具体——`chunk_top_k` 的 docstring 承诺了一个类型注解不支持的 `None`，两者不一致却都能跑。
