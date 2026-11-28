---
project: langchain
feature: document-transformers
title: 文档压缩与嵌入
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/documents/compressor.py
    symbols: [BaseDocumentCompressor]
  - path: libs/core/langchain_core/embeddings/embeddings.py
    symbols: [Embeddings]
  - path: libs/core/langchain_core/embeddings/fake.py
    symbols: [DeterministicFakeEmbedding]
---

# 文档压缩与嵌入

## 一句话职责

定义 RAG 流水线两端的两个最小契约：压缩器负责「检索之后再把结果筛一遍」，嵌入器负责「把文本变成向量」；两者都只有两三个方法、都把异步默认实现成同步套壳，而测试用的嵌入实现则靠「文本哈希当随机种子」换取确定性。

## 机制是什么

`BaseDocumentCompressor(BaseModel, ABC)` 只有一个抽象方法 `compress_documents(documents, query, callbacks=None) -> Sequence[Document]`，加上一个默认的 `acompress_documents` 用 `run_in_executor` 套同步版。

`Embeddings(ABC)` 两个抽象方法：`embed_documents(texts) -> list[list[float]]` 与 `embed_query(text) -> list[float]`，同样带两个 async 默认实现，也都是 `run_in_executor`。

`DeterministicFakeEmbedding(Embeddings, BaseModel)` 是测试用实现，只有一个字段 `size: int`。

## 怎么实现的

**压缩器被定位成「后处理」，而且明确建议不要继承它。** 类 docstring 的措辞值得完整读：

「这个抽象主要用于检索到的文档的**后处理**。先检索到与给定查询匹配的 `Document` 对象。然后这个文档列表可以被进一步处理。例如，可以用 LLM 对检索到的文档重排序」。

后面紧跟一条 `!!! note`：「**使用者应当偏向使用 `RunnableLambda` 而不是继承这个接口**」。

所以这份契约的自我定位是**偏保守的**：它承认自己存在的必要性（有些组件按这个形状实现、需要被识别为「压缩器」），但推荐新代码改用 runnable。这与 g03（向量存储契约）里那套「只强求一个必选、其余报错」的宽松策略相反——那里是在扩大兼容面，这里是在劝退新使用者。

**压缩的三个参数各有分工，`query` 是它的核心增量。** 签名 `(documents, query, callbacks=None)`：文档列表 + 查询 + 可选回调。

与「直接过滤文档」相比，`query` 这个参数是关键：它让压缩器能做**查询相关**的筛选（例如「只保留回答了这个问题的那几句」），而不只是「按长度截断」。这也解释了为什么不叫 `filter` 或 `truncate`——名字里的「compress」强调的是「针对这个查询，把这一堆文档压成更少/更短的」。

`callbacks` 位置在最后且默认 `None`：所以这个抽象预期被接到回调体系（追踪、token 计数），但压缩器可以不理会它。

**返回类型是 `Sequence[Document]` 而不是 `list[Document]`。** 输入也是 `Sequence`。用 `Sequence` 而不是 `list` 是在放宽实现：实现方可以返回自己的序列类型（甚至 tuple），不必构造 list。而 `async` 版的默认实现直接把 `self.compress_documents` 塞进 `run_in_executor` ——所以「异步 = 线程池包同步」在这里是**唯一**路径，实现方想真正异步就得覆盖 `acompress_documents`。

**`alist` 的默认是「每个子类都要自己决定」。** 这一点在 `BaseLLM` / `BaseChatModel` 那套里做法不同（那边有 `_agenerate` 的默认回退到 `_generate`）；压缩器这里连一个 `_acompress_documents` 的钩子都没留——只有一个公有的 `acompress_documents` 默认实现。所以子类要异步就得整体覆盖公有方法，粒度更粗。

**嵌入接口的两件事被明确写成「可以独立」。** `Embeddings` 的 docstring 里有三句关键说明：

1. 「这个抽象包含一个嵌入文档列表的方法和一个嵌入查询文本的方法。查询文本的嵌入预期是**单个**向量，而文档列表的嵌入预期是一个向量列表」。
2. 「通常查询嵌入与文档嵌入是相同的，但**这个抽象允许独立地处理它们**」。
3. 「除了同步方法，这个接口还提供异步版本。默认情况下，异步方法用同步方法实现；不过实现方可以选择用原生异步实现覆盖异步方法以提升性能」。

第 2 句解释了一个常被问到的设计问题：为什么不用一个方法？因为有些嵌入模型对「查询」与「文档」走不同的路径（例如某些检索模型用非对称编码，查询侧与文档侧的指令前缀不同）。所以拆两个方法是为了**给非对称模型留位置**，即使大多数模型两侧相同。

第 3 句则是对「异步是兼容层」这件事的公开承认：与 g03 里那句「临时变通」同源，但这里的措辞更中性——「实现方可以选择覆盖」。所以这条既不是「我们会改」，也不是「你必须实现」，而是「默认可用、鼓励覆盖」。

**两个方法都是 `@abstractmethod`，没有默认实现。** 这一点值得注意：`embed_documents` 与 `embed_query` 都无法由基类推导——一个确实需要模型，另一个确实需要模型。所以嵌入接口的必选方法是 2 个（对比 `VectorStore` 的 1 个）。异步 2 个是可选覆盖。

**`DeterministicFakeEmbedding` 用文本哈希做随机种子。** 实现三步：

```python
def _get_embedding(self, seed: int) -> list[float]:
    rng = np.random.default_rng(seed)
    return list(rng.normal(size=self.size))

@staticmethod
def _get_seed(text: str) -> int:
    return int(hashlib.sha256(text.encode("utf-8")).hexdigest(), 16) % 10**8
```

所以「同一段文本永远得到同一个向量」，而「不同文本得到互不相关的向量」（正态分布采样）。`% 10**8` 把 256 位哈希截成八位十进制量级的种子——这是 numpy 种子的合理范围，也保证「不同文本的种子高概率不同」。

这个设计的实际价值是：任何依赖「向量稳定性」的逻辑都能被无成本地测试。例如 `InMemoryVectorStore` 的整条检索路径、MMR 的多样性选择、索引去重的 id 计算——只要用 `DeterministicFakeEmbedding`，它们就从「需要真实嵌入服务」变成「纯本地可复现」。

注意它与 `FakeEmbeddings` 的差别只在一处：`FakeEmbeddings._get_embedding()` 不收种子，用 `np.random.default_rng().normal(...)` 每次采新随机数。所以同名文本两次调用得到不同向量。两个类的 docstring 示例输出**同一组数字**（`[-0.700234640213188, ...]`）——那显然是从某一处复制过来的，因为 `FakeEmbeddings` 根本不可能稳定产出这个值。这是文档里的一个小瑕疵，但它恰好说明了两个类的区别所在。

两个类都带 `!!! danger "Toy model"` 的警告块：「不要在测试之外使用，因为它不是真实的嵌入模型」。而文件顶部还有一条注释：「**请不要往这里添加更多的 fake embedding 实现**」。所以这个文件是一个封闭的、被明确标注为测试专用的白名单。

**两个 fake 类都继承 `BaseModel`。** 所以它们的 `size` 是 Pydantic 字段，带类型校验——传 `size="100"` 会被拒（或转换，取决于配置），传负数不会（这只是 numpy 的 `size`，负数会报错）。而继承 `BaseModel` 还带来 `model_copy` / 序列化等能力，使它们能直接塞进 pydantic 模型的字段里（例如 `InMemoryVectorStore` 若做成 pydantic 模型）。

**numpy 的导入方式不同：`fake.py` 用 `contextlib.suppress`，而 `vectorstores/utils.py` 用 try/except 设标志位。** 前者：

```python
with contextlib.suppress(ImportError):
    import numpy as np
```

后者是 `_HAS_NUMPY = True/False` 加显式检查。差别在于「能否在缺 numpy 时给出更好的错误」：`utils.py` 能说「cosine_similarity 需要 numpy，请 pip install numpy」；`fake.py` 则是「用了 fake 嵌入而没装 numpy 时得到一个 `NameError`」。前者是库的公共工具，后者是测试辅助，所以宽松度的差别有道理。

## 关键取舍

**源码事实：** 压缩器只有一个抽象方法，异步默认是 `run_in_executor` 包同步；docstring 明确写「使用者应当偏向 `RunnableLambda` 而不是继承这个接口」。

**源码事实：** 压缩器的 `query` 参数让它能做查询相关的筛选，而不只是长度截断。

**源码事实：** `Embeddings` 有两个必选抽象方法（文档与查询分开），docstring 明确说「通常相同，但抽象允许独立处理」。

**源码事实：** 嵌入的异步默认实现也是 `run_in_executor`，docstring 承认「实现方可以选择原生异步覆盖以提升性能」。

**源码事实：** `DeterministicFakeEmbedding` 用 `sha256(text) % 10**8` 做种子，同文本得同向量。

**源码事实：** `FakeEmbeddings` 与 `DeterministicFakeEmbedding` 的 docstring 示例输出了同一组数字，而前者不可能稳定产出该值。

**源码事实：** fake.py 顶部写了「请不要往这里添加更多的 fake embedding 实现」。

**架构推论：** 「压缩器建议用 `RunnableLambda` 替代」这条自我劝退，反映的是框架演化中一类常见的收束：早期每个概念都配一个 ABC，后来发现 runnable 的组合能力已经覆盖了绝大多数需求，于是旧 ABC 保留兼容、新代码被引导走别的路径。承认自己的接口在退场，比强行推广它更诚实，也让迁移变得可预期。

**架构推论：** `embed_query` 与 `embed_documents` 分开这件事，单看签名像是冗余，看了 docstring 才知道是为非对称模型留的位置。这提示一个设计细节：**当一个抽象的两个方法在多数实现里行为相同时，它的存在理由往往只体现在少数实现上**。如果一个接口的「分开」有实际用途，就该在文档里点名——否则后来者会以「冗余」为名把它们合并。

**架构推论：** `DeterministicFakeEmbedding` 的设计说明「确定性」在测试里比「真实感」更重要。随机的 fake 嵌入（`FakeEmbeddings`）只能测「代码跑不跑得通」；确定性的那个能测「结果对不对」，因为每次运行的向量相同，所以排序、MMR、去重都能断言具体值。给测试用的替身，第一原则就是可复现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/documents/compressor.py` | `BaseDocumentCompressor` | 检索后处理抽象，建议改用 runnable |
| `libs/core/langchain_core/documents/compressor.py` | `BaseDocumentCompressor.compress_documents` | 唯一必选方法，含查询上下文 |
| `libs/core/langchain_core/documents/compressor.py` | `BaseDocumentCompressor.acompress_documents` | 默认异步即线程池包同步 |
| `libs/core/langchain_core/embeddings/embeddings.py` | `Embeddings` | 文档与查询分离的嵌入契约 |
| `libs/core/langchain_core/embeddings/embeddings.py` | `Embeddings.aembed_documents` | 默认异步实现，允许被原生异步覆盖 |
| `libs/core/langchain_core/embeddings/embeddings.py` | `Embeddings.aembed_query` | 查询侧异步默认实现 |
| `libs/core/langchain_core/embeddings/fake.py` | `DeterministicFakeEmbedding` | 哈希种子驱动、同文本同向量的测试替身 |
| `libs/core/langchain_core/embeddings/fake.py` | `DeterministicFakeEmbedding._get_seed` | 文本 sha256 截成八位种子 |
| `libs/core/langchain_core/embeddings/fake.py` | `FakeEmbeddings` | 每次采新随机数的非确定性替身 |

## 参考

- [langchain 源码：documents/compressor.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/documents/compressor.py)
- [langchain 源码：embeddings/embeddings.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/embeddings/embeddings.py)
- [langchain 文档：检索器与后处理](https://python.langchain.com/docs/concepts/retrievers/)

## 与主核 / AI 的关系

主核里最接近的是「算子接口」：扫描算子、过滤算子、聚合算子都是一两个方法的抽象，上层组合它们。这一页的两个契约在形态上同类——压缩器是「后置过滤算子」，嵌入器是「类型转换算子」。

但有一处根本差别：主核算子的输入输出是类型确定的记录流，不存在「这个分数是什么意思」的问题；而嵌入器的输出是**语义空间里的坐标**，其含义由模型决定、又被归一函数二次改写（见 g03）。所以嵌入契约虽然只有两个方法，它承载的语义不确定性远大于任何数据库算子。

与 g04（内存实现）的连接很直接：`InMemoryVectorStore` 的 `embedding` 字段就是 `Embeddings` 的实现；测试里塞 `DeterministicFakeEmbedding` 就能让整条检索路径可断言。这也是这一页为什么把 fake 实现与契约放在同一篇——它们是一对。

与 goose 的 `ProviderDef`（g01）对照：那边用关联常量声明「能否共享 / 是否自带工具循环」，是**能力声明**；这里 `Embeddings` 连能力声明都没有，只保证「有方法能出向量」。差别在于两边的使用场景：goose 要调度很多异构 provider，必须知道能力差异；`Embeddings` 的调用方只关心「给我向量」，不关心模型是什么。

对 AI 工程最实用的一条是「给测试替身加确定性」。任何依赖向量相似度的逻辑（排序、去重、聚类、MMR）都会在测试里遇到「随机结果不可断言」的问题。用「文本哈希当种子」这么一个几行的实现，就把整条流水线从「需要真实模型 + 不可复现」变成「纯本地 + 可断言具体值」。这个做法值得直接复制到你自己的测试工具里。

另一条是「压缩器的 `query` 参数」。做后处理时，最容易的设计是「给我一批文档、帮我筛」——不带查询。但那样就只能做无查询相关的策略（长度、去重、元数据），而 RAG 里最有价值的后处理恰恰是「这段文字对**这个问题**有没有用」。把 query 放进签名，是从接口层承认「压缩是查询相关的」。

最后一条是「明确写清异步是兼容层」。三个文件里都有类似表述（「临时变通」「实现方可以选择覆盖」）。把这件事写在文档里，比让它成为心照不宣的行规更好：使用者知道「`a*` 方法的性能可能和同步版一样」，实现者也知道「不覆盖异步不算缺陷」。
