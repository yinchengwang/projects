---
project: ragas
feature: embedding-wrappers
title: 嵌入包装与工厂
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/embeddings/base.py
    symbols: [BaseRagasEmbeddings, LangchainEmbeddingsWrapper, HuggingfaceEmbeddings]
  - path: src/ragas/embeddings/base.py
    symbols: [embedding_factory]
---

# 嵌入包装与工厂

## 一句话职责

这一层把「各种来源的嵌入模型」收成两个基类和三个包装器，而 `embedding_factory` 用一个**形状探测**来判断调用方想走哪一代接口——因为同一个参数位既可能填 provider 名，也可能填模型名。

## 机制是什么

与 LLM 那一层同构，嵌入层也是两代并存：

| | `BaseRagasEmbeddings`（旧式） | `BaseRagasEmbedding`（新式） |
|---|---|---|
| 继承 | langchain 的 `Embeddings`（必须实现 `embed_query` / `embed_documents`） | 纯 `ABC` |
| 契约 | 查询与文档两条独立方法 | `embed_text` / `aembed_text` 单条，批量由基类默认组合 |
| 缓存 | 构造时把四个方法全部包上 `cacher` | 只包 `embed_text` 与 `aembed_text` |
| 工厂 | `LangchainEmbeddingsWrapper` / `HuggingfaceEmbeddings` / `LlamaIndexEmbeddingsWrapper` | provider 类（`_from_factory` 造） |

`BaseRagasEmbeddings` 自己补了一个 `embed_text`：它是 `async` 方法但默认 `is_async=True`，内部逻辑得读代码才知道——它在同步上下文中会走 `else` 分支返回同步结果。这个名字与 langchain 的 `embed_query` 并存，使同一个对象上出现两套动词（`embed_query` / `embed_text`），是新旧过渡期的典型痕迹。

它还有一段 `__get_pydantic_core_schema__`：注册一个 `is_instance_schema` 校验器，让这个类能作为 pydantic 字段的类型标注被校验。没有这段，把嵌入模型放进 pydantic 模型（例如 `TestsetGenerator` 的字段）会失败。

## 怎么实现的

**工厂的第一件事是判「这是不是一次旧式调用」。** `_is_legacy_embedding_call` 的判定只有两行逻辑：

1. 若 `interface` 明确是 `"legacy"` 或 `"modern"`，直接按它来（显式选择优先）。
2. 否则 `auto` 判定：**没有 client 且（`provider` 看起来像模型名 或 `provider` 就是 `"openai"`）**——满足即旧式。

`_looks_like_model_name` 靠一张很短的子串表：`text-embedding`、`ada`、`davinci`、`gpt`、`curie`、`babbage`。所以 `embedding_factory("text-embedding-3-small")` 这种「第一个位置填了模型名」的老写法会被认出来，而 `embedding_factory("huggingface")` 不会。

**三张表拼出一个兼容层。** 这套判定之所以存在，是因为历史接口的第一个位置参数含义变了：过去它是模型名（且默认走 OpenAI），现在它是 provider 名。两代写法无法靠类型区分，只能靠「这个字符串长得像什么」+「有没有给 client」。这是**没有版本号可用时唯一可行的兼容手段**——代价是 `_LEGACY_MODEL_PATTERNS` 里任何一个子串出现在 provider 名里都会误判（例如某天出现一个叫 `ada` 的 provider）。

**旧式分支硬编码 OpenAI。** 命中旧式后，代码做四件事：发 `DeprecationWarning`（文案给出两种迁移方式）；用 `_looks_like_model_name(provider)` 决定模型名是「provider 那个字符串」还是「`model` 参数 / `text-embedding-ada-002`」；构造 `OpenAIEmbeddings(model=..., base_url=base_url)`，并把 `run_config.timeout` 写进它的 `request_timeout`；最后包成 `LangchainEmbeddingsWrapper`。

也就是说旧式路径**根本没有 provider 概念**——不管你传什么，出来的都是 OpenAI 嵌入。这与「旧接口的第一个位置参数是模型名」的假设一致，也是它被弃用的真正原因。

**新式分支走注册表。** `_create_modern_embedding` 支持 `provider/model` 的斜杠写法（`provider` 里的 `/` 且 `model` 为空时，按第一个斜杠拆成 provider 与 model）。然后 `_get_provider_registry()` 拿一张 `{PROVIDER_NAME: class}` 的映射——注意它**不是自动发现**，四个 provider 类是硬写在函数体里的 import 加上一个列表（函数名 `_get_provider_registry` 与 docstring 里的「Auto-discover」并不完全相符）。找不到就抛 `ValueError` 并把可用名单列出来。

找到类之后调 `provider_cls._from_factory(model=..., client=..., cache=... , **kwargs)`。`_from_factory` 是基类上的模板方法：按类属性 `REQUIRES_CLIENT` / `REQUIRES_MODEL` / `DEFAULT_MODEL` 做校验与补默认，然后按需把 `model` / `client` 塞进构造参数。**校验规则由类自己声明，工厂不判断。**

**三个包装器各自的适配点。**

`LangchainEmbeddingsWrapper` 是最薄的一层：四个方法直接转发给内部的 langchain `Embeddings`，每次调用后发一条 `EmbeddingUsageEvent`（`embedding_type="legacy"`，`num_requests` 在批量时取 `len(texts)`）。构造时发弃用告警并 `set_run_config`。

`HuggingfaceEmbeddings` 是唯一一个**真正加载模型**的，用 pydantic dataclass 声明字段（`model_name` 默认 `BAAI/bge-small-en-v1.5`、`cache_folder`、`model_kwargs`、`encode_kwargs`、`cache`）。`__post_init__` 先调父类初始化（为了套上缓存装饰），再 import `sentence_transformers` 与 `transformers`（缺任一就抛带安装指引的 `ImportError`）。接着用 `AutoConfig.from_pretrained` 读模型的 `architectures`，与 `MODEL_FOR_SEQUENCE_CLASSIFICATION_MAPPING_NAMES` 求交，**非空即判定为 cross-encoder**。两类模型的构造与用途完全不同：

- cross-encoder → `CrossEncoder`，用于 `predict`（成对打分的重排场景）；
- 其余 → `SentenceTransformer`，用于 `embed_query` / `embed_documents`。

`encode_kwargs` 里若没给 `convert_to_tensor` 就补成 `True`——保证输出是张量而非 list，方便后续转 numpy。缓存只包在 `predict` 上，因为向量生成自身的缓存由更上层的调用点决定。

`LlamaIndexEmbeddingsWrapper` 与 langchain 那个对称：四个方法转发到 `get_query_embedding` / `get_text_embedding_batch` 及它们的异步版本，构造时发弃用告警。

**异步能力的探测被下沉成工具。** 新式基类的 `_check_client_async` 按点分路径（默认 `"embeddings.create"`）逐级 `getattr` 到方法，再 `inspect.iscoroutinefunction`；任何一步 `AttributeError` / `TypeError` 都返回 `False`。`_run_async_in_current_loop` 则直接委托给 `ragas.embeddings.utils` 里的同名函数——LLM 那边有一份自己的实现（逻辑更复杂，要处理闭包与线程），嵌入这边选择了复用工具函数。

## 关键取舍

**源码事实：** `_is_legacy_embedding_call` 的 `auto` 规则是「无 client 且（provider 像模型名 或 provider 就是 openai）」。`interface` 显式取值优先于一切启发式。

**源码事实：** 旧式分支固定构造 OpenAI 嵌入（`text-embedding-ada-002` 为默认模型），完全不看 `provider` 是否是别的服务。

**源码事实：** 模型名判定是一张 6 个元素的子串表，不是正则也不是白名单——任何含这些子串的 provider 名都会被误判成旧式。

**源码事实：** `_get_provider_registry` 里的 provider 列表是硬写的四个类，不是运行期扫描。

**源码事实：** `HuggingfaceEmbeddings` 靠 `AutoConfig` 的 `architectures` 与序列分类映射表求交来判定 cross-encoder，判定结果决定「用 CrossEncoder 还是 SentenceTransformer」，两者方法面不同。

**架构推论：** 兼容层只能靠「形状探测」，因为旧接口的第一个位置参数语义漂移了，而 Python 的位置参数没有重载可用。这类兼容的固有代价是**永远存在误判空间**：判错的后果不是报错，而是静默走了另一条路径（要么被当成旧式固定用 OpenAI，要么被当成 provider 名去注册表里找不到而报错）。前者更危险，因为它不会立刻失败。

**架构推论：** 把「要不要 client / 要不要 model / 默认模型是什么」做成类属性（`REQUIRES_CLIENT`、`REQUIRES_MODEL`、`DEFAULT_MODEL`）而不是工厂里的 if-else，使新增 provider 只需声明属性、实现 `_from_factory` 或继承默认实现。工厂因此保持极薄；代价是这些约束散在各个 provider 类里，看工厂代码无法知道「总共有哪些必填项」。

**架构推论：** `HuggingfaceEmbeddings` 把 cross-encoder 与 bi-encoder 塞进同一个类，是「同一模型仓库里两种用途」的现实妥协。但它使类型层面无法区分「能 embed 的」与「只能 predict 的」——一个 cross-encoder 实例调 `embed_query` 会在运行期出问题，而静态检查不会提醒。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/embeddings/base.py` | `BaseRagasEmbedding` | 新式基类：单条 + 批量（默认组合） |
| `src/ragas/embeddings/base.py` | `BaseRagasEmbedding._from_factory` | 按类属性校验 client/model 并构造实例 |
| `src/ragas/embeddings/base.py` | `BaseRagasEmbeddings` | 旧式基类：继承 langchain `Embeddings`，四方法缓存 |
| `src/ragas/embeddings/base.py` | `BaseRagasEmbeddings.__get_pydantic_core_schema__` | 让嵌入对象可作 pydantic 字段类型 |
| `src/ragas/embeddings/base.py` | `LangchainEmbeddingsWrapper` | langchain 适配：四方法转发 + 用量上报 |
| `src/ragas/embeddings/base.py` | `HuggingfaceEmbeddings` | 本地模型加载：cross-encoder 与 bi-encoder 分流 |
| `src/ragas/embeddings/base.py` | `LlamaIndexEmbeddingsWrapper` | LlamaIndex 适配：查询/文档批量转发 |
| `src/ragas/embeddings/base.py` | `embedding_factory` | 统一工厂：形状探测后分走新旧两条路径 |
| `src/ragas/embeddings/base.py` | `_is_legacy_embedding_call` | 旧式调用判定：显式优先、否则按 client 与名字形状 |
| `src/ragas/embeddings/base.py` | `_looks_like_model_name` | 6 元素子串表判定「像不像模型名」 |
| `src/ragas/embeddings/base.py` | `_get_provider_registry` | 硬写的 provider 名到类的映射 |
| `src/ragas/embeddings/base.py` | `_create_modern_embedding` | 支持 `provider/model` 斜杠写法后按注册表构造 |

## 参考

- [ragas 源码：embeddings/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/embeddings/base.py)
- [ragas 文档：使用自定义嵌入模型](https://docs.ragas.io/en/stable/howtos/customizations/custom_embeddings/)
- [sentence-transformers 文档：CrossEncoder 与 bi-encoder](https://www.sbert.net/docs/pretrained_cross-encoders.html)
- [LangChain 文档：Embeddings 接口](https://python.langchain.com/docs/concepts/embedding_models/)

## 与主核 / AI 的关系

「用形状探测来兼容两代接口」与主核里「按文件头魔数判格式」是同一种手法：都因为缺少显式版本标识，只能从**内容特征**推断该走哪条路径。主核的判错后果是解析失败（立刻可见），这里的旧式误判后果是「悄悄用错了提供方」——直到看到 token 账单或质量异常才发现。**静默误判比显式失败昂贵得多。**

对 agent 工程最实用的三条。其一：迁移到 `embedding_factory(provider, model=..., client=...)` 的三参数写法，能避免被判定成旧式；旧式路径下传什么 provider 都会落到 OpenAI 嵌入。其二：`HuggingfaceEmbeddings` 会自动判 cross-encoder，但**判定靠模型元数据里的 `architectures` 字段**——自训练或裁剪过配置的模型可能判错，宁可显式传参也不要赌自动判定。其三：嵌入缓存只包在 `HuggingfaceEmbeddings.predict` 与基类的四个公开方法上，批量调用若想复用缓存，必须走包装器而不要绕过它直接调底层模型。
