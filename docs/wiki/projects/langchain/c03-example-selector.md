---
project: langchain
feature: example-selector
title: 示例选择
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/example_selectors/length_based.py
    symbols: [LengthBasedExampleSelector]
  - path: libs/core/langchain_core/example_selectors/semantic_similarity.py
    symbols: [SemanticSimilarityExampleSelector, MaxMarginalRelevanceExampleSelector]
---

# 示例选择

## 一句话职责

few-shot 提示里放哪些示例、放几条，由选择器决定。这里有两种判据：**按长度装到装不下为止**（控制上下文预算），以及**按语义相似度排序取前 k 条**（提高与当前输入的贴合度）。

## 机制是什么

示例选择要回答的问题本质上是「上下文预算怎么分配」。同一份示例库，不同输入该配不同的示例集，而判据有两个完全不同的方向：

| 判据 | 关心什么 | 代价 |
|---|---|---|
| 长度 | token / 行数预算 | 选出的示例可能与当前输入毫不相关 |
| 语义相似度 | 与输入的相关性 | 需要嵌入模型与向量库，且**没有预算上限** |

源码把这两条做成两个互不相关的实现，只共享同一个抽象接口：

```python
class BaseExampleSelector(ABC):
    @abstractmethod
    def add_example(self, example: dict[str, str]) -> Any: ...
    @abstractmethod
    def select_examples(self, input_variables: dict[str, str]) -> list[dict[str, Any]]: ...
```

四个方法（两个同步、两个异步）。`add_example` 让选择器可以**成长**——不是一次性配置好的静态清单，而是能随对话追加示例。这个能力把选择器从「过滤器」变成了「可变状态」。

注意基类对返回类型很宽松：`add_example` 返回 `Any`（长度选择器返回 `None`，向量选择器返回 `str` id）。源码没给统一语义——因为「添加」这件事的副产物对两类实现确实不同。

## 怎么实现的

**长度选择器的长度单位是一个「按空白与换行切分的段数」。**

```python
def _get_length_based(text: str) -> int:
    return len(re.split(r"\n| ", text))
```

即 `len(text.split())` 的等价物，但用 `re.split` 保留了 `\n` 作为分隔。所以：

- `"a b c"` → 3
- `"a\nb"` → 2
- 连续空格会产生**空段**并被计数。例如 `"a  b"`（两个空格）→ `['a', '', 'b']` → 3，而不是 2。

第三点是一个真实的偏差：`re.split(r"\n| ")` 与 `str.split()` 不同，后者会把连续空白折叠掉。这里没有折叠，所以**格式对齐的示例（多空格缩进）会被算得更长**。

这个函数名叫 `_get_length_based` 而不是 `_word_count`，且是模块级私有函数——说明它是「默认实现」而非唯一选择：字段 `get_text_length` 是可替换的 `Callable[[str], int]`，注释写「Function to measure prompt length. Defaults to word count」。所以换成 token 计数器是设计时留好的口子。

**长度选择器把「每条示例占多少长度」预先算好并缓存。**

```python
example_text_lengths: list[int] = Field(default_factory=list)
"""Length of each example."""

@model_validator(mode="after")
def post_init(self) -> Self:
    if self.example_text_lengths:
        return self
    string_examples = [self.example_prompt.format(**eg) for eg in self.examples]
    self.example_text_lengths = [self.get_text_length(eg) for eg in string_examples]
    return self
```

三处：

1. **长度是「格式化之后」的长度**，不是原始字典的长度。`self.example_prompt.format(**eg)` 把示例渲染成最终文本，然后才计长度。这是正确的——进上下文的是渲染结果。
2. **缓存到 `example_text_lengths`**，并在 `add_example` 时同步追加。所以选择阶段不必重新渲染，只需累加长度。这对「示例多、调用频繁」的场景有意义。
3. **`if self.example_text_lengths: return self` 是一个「已算过就跳过」的守卫**。它让「用户显式传了 `example_text_lengths`」时不被覆盖。但这也带来一个隐患：**若用户传的列表长度与 `examples` 不一致，校验不会发现**。后续 `select_examples` 按索引访问会 `IndexError` 或越界（取决于哪边更长）。

`add_example` 的实现只有两行：

```python
self.examples.append(example)
string_example = self.example_prompt.format(**example)
self.example_text_lengths.append(self.get_text_length(string_example))
```

注意**每次都重新格式化这一条**——不做增量优化。这也解释了为什么缓存的字段是 `list` 而不是「总长度」：需要逐条判断「下一条放不放得下」。

**选择逻辑是一个「装到装不下为止」的贪心循环，而它的顺序敏感。**

```python
def select_examples(self, input_variables: dict[str, str]) -> list[dict[str, Any]]:
    inputs = " ".join(input_variables.values())
    remaining_length = self.max_length - self.get_text_length(inputs)
    i = 0
    examples = []
    while remaining_length > 0 and i < len(self.examples):
        new_length = remaining_length - self.example_text_lengths[i]
        if new_length < 0:
            break
        examples.append(self.examples[i])
        remaining_length = new_length
        i += 1
    return examples
```

五处：

1. **预算先扣掉输入本身的长度**（`max_length - length(inputs)`）。即 `max_length` 指的是**整个提示**的上限，不是示例部分的配额。这个语义决定了「输入很长时示例会自然减少」。
2. **`inputs = " ".join(input_variables.values())`**：把输入变量的值用空格拼起来算长度。它不区分变量名，也不考虑模板本身的固定文本。所以这是一个**粗略估计**——真实提示还包含模板的框架文本与分隔符，这里都没算。
3. **`if new_length < 0: break` 而不是 `continue`**：装不下就**整个停止**，不尝试后面的示例。所以这是一个前缀贪心：**总是从前面的示例开始装**。这意味着示例列表的顺序就是优先级——排在前面的优先进入。
4. **`remaining_length = new_length`** 只在装成功时才更新。若装不下就 break，不会「跳过这条试下一条」。理由与上一条一致：保持前缀语义。
5. **`while remaining_length > 0`** 而不是 `>= 0`：恰好用完预算时也停。这是避免「0 预算还能再装一条零长度示例」的边界处理。

这个算法的性质值得说明：**它既不看相关性也不看多样性**，只看「顺序 + 长度」。所以示例列表的排列成了唯一的调优手段，而源码对它没有任何约定。

**`aadd_example` 与 `aselect_examples` 直接调用同步版。**

```python
async def aadd_example(self, example):
    self.add_example(example)

async def aselect_examples(self, input_variables):
    return self.select_examples(input_variables)
```

与基类默认实现（`run_in_executor`）不同，这里**没有走线程池**。理由很清楚：长度选择器的全部工作是字符串格式化与整数比较，**没有 I/O**。放到线程池只会多一次调度开销。

这是一处「覆写异步方法」的正确示例——只在该覆写时才覆写。对比 `_VectorStoreExampleSelector` 就没这么做（见下）。

**向量选择器：一个中间基类 + 两个具体实现。**

```python
class _VectorStoreExampleSelector(BaseExampleSelector, BaseModel, ABC):
    vectorstore: VectorStore
    k: int = 4
    example_keys: list[str] | None = None
    input_keys: list[str] | None = None
    vectorstore_kwargs: dict[str, Any] | None = None
```

四个字段的分工，各自对应一个「选择时要决定的事」：

- **`vectorstore`**：示例存在向量库里，`k` 决定取几条。
- **`k = 4`**：默认四条。这是一个经验值——大多数 few-shot 场景取 3-5 条；取太多既费上下文又可能引入噪声示例。
- **`example_keys`**：**输出侧过滤**——检索出来的文档元数据里有很多键，只保留这些键构成示例字典。
- **`input_keys`**：**输入侧过滤**——构造查询文本时只用这些键。
- **`vectorstore_kwargs`**：直接透传给 `similarity_search`，让调用方能传后端的特有参数（如 filter、score_threshold）。

`extra="forbid"` 与 `arbitrary_types_allowed=True` 一起设。后者是必需的（`VectorStore` 是任意类），前者则让拼错字段名报错——这与 `BaseMessage` 的 `extra="allow"` 正好相反（b01 讲过，消息类允许挂额外字段，选择器不允许）。

**文本化的规则是「按键名排序后拼接」，而这是一个纯粹为了确定性。**

```python
def sorted_values(values: dict[str, str]) -> list[str]:
    return [values[val] for val in sorted(values)]

@staticmethod
def _example_to_text(example: dict[str, str], input_keys: list[str] | None) -> str:
    if input_keys:
        return " ".join(sorted_values({key: example[key] for key in input_keys}))
    return " ".join(sorted_values(example))
```

两处：

1. **按键名排序再取值**。为什么要排序？因为字典的插入顺序取决于构造方式，而**同一个语义示例用不同顺序构造会产生不同的查询文本**，进而产生不同的嵌入向量。排序让「同一份示例必得同一段文本」。这对「示例入库一次、查询多次」的场景是必需的。
2. **先按 `input_keys` 过滤键，再排序**。顺序不能反——排序后再过滤需要另一趟。过滤本身的意义是：有些键（如 `output`）不该参与**查询**的语义匹配（它只该出现在示例的答案里）。

注意两个 `sorted_values` 的路径产出同样的结果风格（空格拼接），所以 `add_example` 入库时的文本与 `select_examples` 查询时的文本是一致的拼法。这是必要的：**入库文本与查询文本必须同构，否则相似度无意义**。

**入库时元数据就是整个示例字典。**

```python
def add_example(self, example: dict[str, str]) -> str:
    ids = self.vectorstore.add_texts([self._example_to_text(example, self.input_keys)], metadatas=[example])
    return ids[0]
```

即**文本是「向量化的那部分」，元数据是「原样的完整字典」**。这解释了 `example_keys` 的存在：检索回来时从元数据恢复示例，若不设 `example_keys` 就是整个字典，若设了就只取那几键。

而 `example_keys` 的典型用途是**排除只用于向量化的键**——例如示例里有 `input` 与 `output`，用两个都参与向量化（不设 `input_keys`），但返回时只想给模板 `input` 与 `output` 两个键（这恰好是全部），或者只想给部分。

`_documents_to_examples` 的实现：

```python
examples = [dict(e.metadata) for e in documents]
if self.example_keys:
    examples = [{k: eg[k] for k in self.example_keys} for eg in examples]
return examples
```

注意 `dict(e.metadata)` 而**不是** `e.page_content`。所以检索结果的**文本被完全丢弃**，只从元数据恢复示例。这是一种「向量库只当索引、真数据放元数据」的用法。它要求元数据里带全了示例的所有键——若示例含非字符串值，元数据可能丢失类型（取决于后端的序列化）。

**`SemanticSimilarityExampleSelector` 与 `MaxMarginalRelevanceExampleSelector` 的差别是一个参数与一个检索方法。**

```python
# SemanticSimilarity
example_docs = self.vectorstore.similarity_search(
    self._example_to_text(input_variables, self.input_keys), k=self.k, **vectorstore_kwargs
)
# MMR
fetch_k: int = 20
example_docs = self.vectorstore.max_marginal_relevance_search(
    self._example_to_text(input_variables, self.input_keys), k=self.k, fetch_k=self.fetch_k
)
```

两处差异：

1. **MMR 多一个 `fetch_k = 20`**：先抓 20 条候选，再从中挑出 k 条既相关又互不相似的。这个 `fetch_k` 与 `k` 的比值（20 : 4 = 5 : 1）是一种经验配置——候选池太小则多样性无从谈起，太大则代价上升。
2. **MMR 的 `select_examples` 没有把 `vectorstore_kwargs` 传下去**，而 `SemanticSimilarity` 传了。这是一个**实现上的不一致**：`_VectorStoreExampleSelector` 定义了 `vectorstore_kwargs` 字段，但只有 `SemanticSimilarityExampleSelector` 用它。所以对一个 `MaxMarginalRelevanceExampleSelector` 设 `vectorstore_kwargs` 会被静默忽略。

MMR 的文档给了论文引用：「This was shown to improve performance in this paper: https://arxiv.org/pdf/2211.13892.pdf」。即这个选择不是凭感觉，而是基于「示例多样性对 few-shot 效果有正收益」这一实证结论。

MMR 的实际效果对 few-shot 特别有意义：**纯相似度会选出好几条几乎一样的示例**（因为它们彼此最像），而示例的价值恰恰在于「覆盖不同的情况」。MMR 用「与已选示例的相似度」做惩罚项，把重复的候选压下去。

`similarity_search` 与 `max_marginal_relevance_search` 的**返回类型相同**（`list[Document]`），所以 `_documents_to_examples` 能共用。这是两个实现能共享中间基类的前提。

**`from_examples` 是便利构造器，它把「示例字典」变成「向量库里有数据的索引」。**

```python
@classmethod
def from_examples(cls, examples, embeddings, vectorstore_cls, k=4, input_keys=None, *, example_keys=None, vectorstore_kwargs=None, **vectorstore_cls_kwargs):
    string_examples = [cls._example_to_text(eg, input_keys) for eg in examples]
    vectorstore = vectorstore_cls.from_texts(string_examples, embeddings, metadatas=examples, **vectorstore_cls_kwargs)
    return cls(vectorstore=vectorstore, k=k, input_keys=input_keys, example_keys=example_keys, vectorstore_kwargs=vectorstore_kwargs)
```

三处：

- **`string_examples` 与 `metadatas=examples` 一一对应**：入库时的文本用 `_example_to_text`，元数据用原字典。所以「向量化的内容」与「恢复出来的内容」由两套规则决定，而 `from_examples` 是唯一让它们保持一致的地方（两处都传 `input_keys`）。
- **`**vectorstore_cls_kwargs` 收集所有剩余关键字，直接传给 `from_texts`**。这让调用方能传后端的连接参数。
- **`k`、`input_keys` 是位置参数，`example_keys`、`vectorstore_kwargs` 是关键字参数**（`*` 之后）。这个划分说明前者更常用。仓库 AGENTS.md 里也要求「新参数用 keyword-only」，这里遵守了。

`afrom_examples` 完全对称，只是 `await vectorstore_cls.afrom_texts(...)`。

**注意 `_VectorStoreExampleSelector` 没有覆写异步方法，而 `LengthBasedExampleSelector` 覆写了。** 这个差异的原因正是前面说的判据：向量选择器的同步方法要访问向量库（可能涉及网络），所以基类的 `run_in_executor` 默认实现是合理的；而长度选择器纯 CPU 工作，覆写掉更省。

## 关键取舍

**源码事实：** `BaseExampleSelector` 只有两个抽象方法（`add_example` / `select_examples`），两个异步默认实现走 `run_in_executor`，返回类型对 `add_example` 是 `Any`（两类实现语义不同）。

**源码事实：** 默认长度度量是 `len(re.split(r"\n| ", text))`，不折叠连续空格，所以对齐格式的示例会被算得更长；该函数可通过 `get_text_length` 字段替换为 token 计数器。

**源码事实：** 示例长度在初始化时按**格式化后**的文本预先计算并缓存（`example_text_lengths`），`add_example` 时同步追加。

**源码事实：** 长度选择是**前缀贪心**：预算先扣掉输入长度，然后从第一条开始装，装不下就 `break`（不跳过重试）。所以示例的排列顺序就是优先级。

**源码事实：** 长度选择器的异步方法直接调同步版，不经过线程池；向量选择器则沿用基类的 `run_in_executor` 默认实现。

**源码事实：** 向量选择器的文本化是「按键名排序后空格拼接」，且入库与查询用同一规则，保证同一示例得到同一段文本。

**源码事实：** 入库的元数据就是完整示例字典；检索回来只从 `e.metadata` 恢复示例（丢弃 `page_content`），`example_keys` 用于输出侧过滤。

**源码事实：** `SemanticSimilarityExampleSelector` 把 `vectorstore_kwargs` 透传给 `similarity_search`，而 `MaxMarginalRelevanceExampleSelector` 不透传——同一个基类字段在两者上的行为不一致。

**源码事实：** MMR 多一个 `fetch_k = 20`（候选池），默认 `k = 4`，即约 5:1 的候选与选取比，并引用论文说明多样性带来的收益。

**架构推论：** 两种选择器暴露出「示例选择」其实有两个不可兼得的目标：**预算控制**与**相关内容**。长度选择器能保证不超预算但选择是盲的；向量选择器能保证相关但完全不看预算（`k` 是条数而非长度）。要兼得就必须组合两者——先按相似度排序再按长度截断。源码没提供这个组合，两套实现互不相识，这是设计上留给使用者的缝。

**架构推论：** `add_example` 这个接口暗示了选择器的「可变状态」定位。这对动态 few-shot 有价值（对话中积累示例），但代价是**选择器的行为随时间变化**——同一份输入在不同时刻可能选出不同示例。对需要可复现的评测，这是必须知道的副作用。

**架构推论：** 长度选择器的前缀贪心与「按其顺序」这两件事合起来，使**示例列表的排列成为唯一的调优参数**，而源码对它没有任何约定或建议。更精细的实现会引入优先级或排序依据（如按相关性排），但那样就需要额外的信息，而长度选择器的接口里没有——它只拿到 `input_variables`，无从判断相关性。

**架构推论：** 向量选择器把真数据放在元数据里、把 `page_content` 丢弃，是一个实用但脆弱的约定。它假设「元数据能无损往返字符串字典」，而不同后端的元数据序列化能力不同（有些只支持标量）。这处风险源码没提，但它是 `_documents_to_examples` 那句 `dict(e.metadata)` 的隐含前提。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/example_selectors/length_based.py` | `LengthBasedExampleSelector` | 按格式化后长度前缀贪心装入预算 |
| `libs/core/langchain_core/example_selectors/length_based.py` | `_get_length_based` | 默认长度度量：按换行/空格切分的段数 |
| `libs/core/langchain_core/example_selectors/length_based.py` | `post_init` | 初始化时按渲染文本预计算并缓存长度 |
| `libs/core/langchain_core/example_selectors/length_based.py` | `add_example` | 追加示例并同步追加其长度 |
| `libs/core/langchain_core/example_selectors/semantic_similarity.py` | `_VectorStoreExampleSelector` | 向量库选择器的共享字段与文本化规则 |
| `libs/core/langchain_core/example_selectors/semantic_similarity.py` | `sorted_values` / `_example_to_text` | 按键名排序拼接，保证文本可复现 |
| `libs/core/langchain_core/example_selectors/semantic_similarity.py` | `_documents_to_examples` | 从元数据恢复示例并用 `example_keys` 过滤 |
| `libs/core/langchain_core/example_selectors/semantic_similarity.py` | `SemanticSimilarityExampleSelector` | 纯相似度检索，透传 `vectorstore_kwargs` |
| `libs/core/langchain_core/example_selectors/semantic_similarity.py` | `MaxMarginalRelevanceExampleSelector` | 先抓 `fetch_k` 候选再按 MMR 选出 k 条，不透传 `vectorstore_kwargs` |
| `libs/core/langchain_core/example_selectors/semantic_similarity.py` | `from_examples` / `afrom_examples` | 从示例字典构造出带数据的向量库选择器 |
| `libs/core/langchain_core/example_selectors/base.py` | `BaseExampleSelector` | 两个抽象方法 + 两个走线程池的异步默认实现 |

## 参考

- [langchain 源码：example_selectors/length_based.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/example_selectors/length_based.py)
- [langchain 源码：example_selectors/semantic_similarity.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/example_selectors/semantic_similarity.py)
- [langchain 文档：如何选择示例](https://python.langchain.com/docs/how_to/example_selectors/)
- [MMR 论文：多样性重排（Carbonell & Goldstein）](https://arxiv.org/pdf/2211.13892.pdf)

## 与主核 / AI 的关系

「同一份数据按不同判据选出不同子集」与主核里的**采样与截断**结构相同。主核在 `ANALYZE` 取样时会按页/行随机抽样来控制代价，而查询时要按谓词筛选来控制结果集大小。前者是「预算优先」，后者是「相关性优先」，两者在同一个数据库里并存而不试图统一——这正是本页两种选择器的关系。

「按键名排序后拼接以保证文本可复现」这条与主核里**表达式的规范化**同源。主核在做计划缓存时，会把 `a+b` 与 `b+a` 规范化成同一种形式以便命中同一份缓存；本页排序键名做的是同一件事，但目的在**让同一个示例永远得到同一个向量**。判据相同——**可复现性来自确定性的序列化**。

「长度选择器只用前缀贪心」与主核里**按顺序装入缓冲池**的做法类似：两者的共同限制是「没有全局最优的信息，只能按既定顺序做局部决定」。代价都是可能选到次优子集；收益是算法是线性的且行为可预测。

对 AI 工程的实际意义在于：few-shot 是提升小模型效果最廉价的手段之一，而「选哪些示例」直接决定效果与成本。这份实现给出的三条经验可直接用：**长度与相关性是两个独立目标，想兼得必须组合而非替换**、**文本化规则必须确定**（否则同一示例向量不一致）、以及**示例列表的排列顺序就是隐式优先级**（在只按长度装入的实现里，它成了唯一的调优旋钮）。
