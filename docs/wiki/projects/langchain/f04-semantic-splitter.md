---
project: langchain
feature: semantic-splitter
title: 语义与句法分块
page_type: feature
schema_version: 1
refs:
  - path: libs/text-splitters/langchain_text_splitters/sentence_transformers.py
    symbols: [SentenceTransformersTokenTextSplitter]
  - path: libs/text-splitters/langchain_text_splitters/nltk.py
    symbols: [NLTKTextSplitter]
  - path: libs/text-splitters/langchain_text_splitters/spacy.py
    symbols: [SpacyTextSplitter]
---

# 语义与句法分块

## 一句话职责

前两个分块器（字符、标记语言）都靠**符号**（分隔符、标题标签）判断边界；这三个靠**语言模型与句法分析**——它们都只做一件事：**找出句子的边界**，然后交给基类的 `_merge_splits` 合并成不超过上限的块。

## 机制是什么

| 类 | 边界来源 | 让「合并」做的事 |
|---|---|---|
| `NLTKTextSplitter` | NLTK 的 `sent_tokenize`（或 `span_tokenize`） | 把句子合进块 |
| `SpacyTextSplitter` | spaCy 管道的 `Doc.sents` | 把句子合进块 |
| `SentenceTransformersTokenTextSplitter` | 该模型的 tokenizer（**不是句子**） | 按 token 索引切，**不走 `_merge_splits`** |

前两个是「**句子 → 块**」的两级结构，第三个是「**token → 块**」的一级结构。这个差别决定了它们与基类的关系：前两者复用 `_merge_splits`，后者绕开它。

## 怎么实现的

### 一、`NLTKTextSplitter`：两种切法，一个约束

**构造参数里有一个互斥校验，而它的理由不是「等价」而是「必然」。**

```
if use_span_tokenize and self._separator:
    raise ValueError("When use_span_tokenize is True, separator should be ''")
```

为什么 `span_tokenize` 要求分隔符为空？因为 **`span_tokenize` 的目的就是「保留原始文本里的空白与标点」**——它返回的是 `(start, end)` 的字符区间，而下面的实现会把**句子之间的空隙**（上一句的 `end` 到本句的 `start`）也带上。若再用一个非空分隔符去 join，那些空隙就被重复了。

**`_span_tokenizer` 的实现正是那个「带上空隙」的版本。**

```
spans = list(tokenizer.span_tokenize(text))
for i, (start, end) in enumerate(spans):
    if i > 0:
        prev_end = spans[i - 1][1]
        sentence = text[prev_end:start] + text[start:end]
    else:
        sentence = text[start:end]
    splits.append(sentence)
```

也就是说第 i 句的片段 = **「上一句结束到本句开始的空白」+「本句本身」**。于是把所有片段按空分隔符拼回去，得到的就是原文。这与 `sent_tokenize` 的差别是本质的：后者只返回句子文本，**句间的空格与换行被丢弃**。

这就是这个参数的用途：**需要保留原文形态（例如要回填 `start_index`、要在块里保留段落结构）时用 `span_tokenize`**。

**它用了 NLTK 的一个私有函数。**

```
tokenizer = nltk.tokenize._get_punkt_tokenizer(language)  # noqa: SLF001
```

`_get_punkt_tokenizer` 是下划线开头的私有 API，而代码用 `# noqa: SLF001` 显式抑制「访问私有成员」的 lint。理由很实际：`sent_tokenize` 只给文本，`span_tokenize` 是 `PunktTokenizer` 的方法，而**公开 API 里没有一个「按语言拿 Punkt 分词器」的入口**。所以只能取私有的。

这是一个「依赖上游未公开 API」的已知代价——它会在 NLTK 改内部结构时失效，但没有替代方案。

**`_sent_tokenizer` 用 lambda 包了一层，且 `cast` 了返回类型。**

```
return lambda text: cast("list[str]", nltk.tokenize.sent_tokenize(text, language))
```

`cast` 的存在说明 NLTK 的类型标注与实际的 `list[str]` 对不上。这是类型检查器与上游库标注不一致的常见妥协。

**`split_text` 只有两句，并且注释写着「先朴素地切成小片，再交给合并」。**

```
splits = self._tokenizer(text)
return self._merge_splits(splits, self._separator)
```

**「朴素」在这里是准确的说法**：句子是「可能超长」的片段（一个句子可以有一千个字符），而 `_merge_splits` **不会把一个句子再切细**——它只在句子之间做合并与回退。所以若单个句子超过 `chunk_size`，输出会超限（基类会 `warn`）。

这是三个分块器共同的性质：**它们提供「语义上正确的边界」，但不提供「尺寸上必定合规的块」**。尺寸是否正确取决于原文的句子长度。

**`split_text` 上方的注释解释了这段行为的来源**——它明确说明先朴素切分再合并，而基类的 `_merge_splits` 会「进一步合并」并检查重叠。也就是说这个类的作者把「切」与「合」的职责划分写在了注释里，读的人不必自己去基类推断。

### 二、`SpacyTextSplitter`：管道配置与 `strip_whitespace` 的二次作用

**它构造时不校验分隔符，而是把分隔符存下来供 `_merge_splits` 用。**

```
self._tokenizer = _make_spacy_pipeline_for_splitting(pipeline, max_length=max_length)
self._separator = separator
self._strip_whitespace = strip_whitespace
```

注意 `self._strip_whitespace = strip_whitespace` ——**它覆盖了基类同名属性**。而 `strip_whitespace` 在基类的 `_join_docs` 里也有作用（拼接后 `.strip()`）。也就是说这个参数在这里被用两次：一次在 `split_text` 里选 `s.text` 还是 `s.text_with_ws`，一次在合并时去空白。

**`split_text` 的两行各自体现一件事。**

```
splits = (s.text if self._strip_whitespace else s.text_with_ws for s in self._tokenizer(text).sents)
return self._merge_splits(splits, self._separator)
```

第一行选择句子的文本形态：`s.text` 是**去掉尾随空白**的，`s.text_with_ws` 是**带尾随空白**的。而它是一个**生成器**——意味着句子是惰性产出的，`_merge_splits` 边消费边合并。

第二行把生成器交给基类。**`_merge_splits` 接受 `Iterable[str]`**，所以生成器可以直接传——这是基类签名用 `Iterable` 而非 `list` 的实际价值：**处理超长文档时不必先把所有句子物化进内存**。

**`_make_spacy_pipeline_for_splitting` 的两条分支各有明确取舍。**

```
if pipeline == "sentencizer":
    sentencizer = english_cls()          # 空的英文 Language
    sentencizer.add_pipe("sentencizer")
else:
    sentencizer = spacy.load(pipeline, exclude=["ner", "tagger"])
    sentencizer.max_length = max_length
```

- `"sentencizer"`：只用规则（标点）断句，**不加载任何模型**，极快但准确度低。类文档写着「更快但可能不够准确」。
- 其他（默认 `en_core_web_sm`）：加载模型，但**排除 `ner` 与 `tagger` 两个管道组件**——因为只需要句法边界，不需要命名实体与词性标注。

**`exclude=["ner", "tagger"]` 是一个针对性优化**：这两个组件占了模型推理时间的大头，而断句用不到它们的输出。

**`max_length` 只在这一支被设置，`sentencizer` 分支没设。** 这是一个不对称：`spacy.load` 出来的是完整 `Language`，有一个默认的 `max_length`（文档说默认一百万字符）需要显式调整；而空的 `english_cls()` 可能没有这个限制或不需要调。代码没有注释说明，但从「只在一个分支里设」可以读出这个意图。

**`pipeline` 参数的含义是「管道名或 `sentencizer`」这一对二选一**，而它没有校验——一个拼错的名字会在 `spacy.load` 里报 spaCy 自己的错误。

**`_import` 的失败消息给出了 pip 命令。** `"Spacy is not installed, please install it with `pip install spacy`."` ——两个第三方分块器都这么做，是这类可选依赖的标准处理。

### 三、`SentenceTransformersTokenTextSplitter`：唯一一个不走 `_merge_splits` 的

**它的切分完全靠基类里的 `split_text_on_tokens`，而它自己做的是「准备一个 `Tokenizer`」。**

```
def encode_strip_start_and_stop_token_ids(text: str) -> list[int]:
    return self._encode(text)[1:-1]

tokenizer = Tokenizer(
    chunk_overlap=self._chunk_overlap,
    tokens_per_chunk=self.tokens_per_chunk,
    decode=self.tokenizer.decode,
    encode=encode_strip_start_and_stop_token_ids,
)
return split_text_on_tokens(text=text, tokenizer=tokenizer)
```

**`[1:-1]` 这个切片是这一步的全部要点。** Sentence-Transformers 的 tokenizer 会在序列首尾各加一个特殊 token（`[CLS]` / `[SEP]`）。而滑动窗口是**按 token 索引切**的——若不去掉它们，每个块都会多出这两个 token，且会出现在块内部的任意位置（而不只是首尾）。

**切掉它们之后，`decode` 得到的块是「纯内容」**，与原文没有特殊 token 污染。这是一个很细但必要的处理。

**`_encode` 里那个 `max_length = 2**32` 与 `truncation="do_not_truncate"` 是同一件事的两个保险。**

```
self.tokenizer.encode(text, max_length=self._max_length_equal_32_bit_integer, truncation="do_not_truncate")
```

`encode` 的默认行为可能是按模型最大长度截断——若那样，一个超长文本会被**静默截成模型上限**，于是分块器只看到开头那一段，后面的内容永远进不了块。设置一个巨大上限并显式禁用截断，保证「整段文本都被编码」。

**这个坑很隐蔽**：截断不会报错，只会让输出变短。而分块器的使用者不会立刻发现「我的文档只有前 512 个 token 进了块」。

**`tokens_per_chunk` 的默认值来自模型的 `max_seq_length`，且有两条校验。**

```
self.maximum_tokens_per_chunk = self._model.max_seq_length
if tokens_per_chunk is None:
    if self.maximum_tokens_per_chunk is None: raise ValueError("The model does not have a maximum token limit, and tokens_per_chunk was not provided. ...")
    self.tokens_per_chunk = self.maximum_tokens_per_chunk
else:
    self.tokens_per_chunk = tokens_per_chunk

if self.maximum_tokens_per_chunk is not None and self.tokens_per_chunk > self.maximum_tokens_per_chunk:
    raise ValueError(f"The token limit of the models '{model_name}' is: {maximum}. Argument tokens_per_chunk={tokens_per_chunk} > maximum token limit.")
```

两处的存在理由不同：

1. **模型没有上限且调用方也没给** → 无法决定每块多少 token，只能报错。
2. **调用方给了但超过模型上限** → 报错，并在消息里带上模型名与实际上限。

第二条是这一页最值得记的**保护**：一个 `tokens_per_chunk > max_seq_length` 的配置会让每块都超过嵌入模型能吃下的长度，于是**嵌入时被截断或报错**。在分块阶段拦住它，比让错误出现在嵌入阶段好得多——因为那时它已经是「一堆块里有一半嵌不进去」的形式。

而错误消息的措辞（`The token limit of the models '...' is: ...`）把三个数字都摆出来了，这是一条可以直接定位问题的消息。

**它的 `chunk_overlap` 默认 50，与基类的 200 不同。** 50 个 token 的重叠在句级切分里通常够用，而基类那个 200 的默认值是给字符数准备的——两者量纲不同，所以默认值不能共用。

**它完全不走 `_merge_splits`，所以 `chunk_size` 与 `length_function` 对它无意义。** 它的块大小由 `tokens_per_chunk` 精确控制，重叠由 `chunk_overlap` 精确控制。

这是三个里唯一「尺寸精确」的分块器——代价是它**不懂句子边界**，可能在一个词中间切断（虽然 token 通常代表完整片段，但一个长词的多个 token 会被拆到两个块里）。

## 关键取舍

**源码事实：** `NLTKTextSplitter` 的 `use_span_tokenize` 强制要求 `separator` 为空，因为 span 版本会把「上一句结束到本句开始的空隙」也放进片段——若再叠加分隔符就会重复。

**源码事实：** `_span_tokenizer` 依赖 NLTK 的私有 `_get_punkt_tokenizer`（用 `# noqa: SLF001` 抑制 lint），因为公开 API 里没有「按语言取 Punkt 分词器」的入口。

**源码事实：** 两个句法分块器都只做「找句子边界」，把合并交给 `_merge_splits`；而 `_merge_splits` **不会把单个超长句子再切细**，所以块可能超限（基类会 `warn`）。

**源码事实：** `SpacyTextSplitter.split_text` 传给 `_merge_splits` 的是**生成器**，利用基类签名的 `Iterable` 支持惰性处理。

**源码事实：** spaCy 分支里 `sentencizer` 走空管道（纯规则），其余走 `spacy.load` 且 `exclude=["ner", "tagger"]`；`max_length` 只在前者之外的分支设置。

**源码事实：** `SentenceTransformersTokenTextSplitter` 的 `encode` 包装用 `[1:-1]` 切掉首尾特殊 token，否则每个块都会被污染。

**源码事实：** `_encode` 显式设置巨大 `max_length` 并 `truncation="do_not_truncate"`，防止超长文本被静默截断。

**源码事实：** `tokens_per_chunk` 默认取模型的 `max_seq_length`，并对「模型无上限且未提供」与「提供值超过模型上限」两种情况分别报错，后者消息里带模型名与两个数字。

**源码事实：** 它的 `chunk_overlap` 默认 50（基类 200），且完全不走 `_merge_splits`——`chunk_size` 与 `length_function` 对它无意义。

**架构推论：** 三个分块器清晰地分成了两派，而分派的依据是**「边界是否语义化」与「尺寸是否精确」这两个不可兼得的性质**。

| | 边界语义化 | 尺寸精确 |
|---|---|---|
| 字符 / 递归字符 | 否（按符号近似） | 近似 |
| 标记语言 | 是（按结构） | 近似 |
| NLTK / spaCy | 是（按句子） | **否**（超长句子会越界） |
| Sentence-Transformers token | **否** | 是（精确 token 数） |

这张表说明了一个现实：**「按句子切」和「按 token 数切」是两条不能合并的路**。想要语义边界就得接受块大小不均；想要精确大小就得接受从词中间切断。langchain 没有试图做一个「两者兼顾」的切分器——它把选择留给使用者。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/text-splitters/langchain_text_splitters/nltk.py` | `NLTKTextSplitter` | 按句子切，两种分词路径 |
| `libs/text-splitters/langchain_text_splitters/nltk.py` | `NLTKTextSplitter.__init__` | `use_span_tokenize` 与 `separator` 的互斥校验 |
| `libs/text-splitters/langchain_text_splitters/nltk.py` | `_sent_tokenizer` | 包 `sent_tokenize` 并 cast 返回类型 |
| `libs/text-splitters/langchain_text_splitters/nltk.py` | `_span_tokenizer` | 把句间空隙也带上，依赖私有 Punkt 分词器 |
| `libs/text-splitters/langchain_text_splitters/nltk.py` | `NLTKTextSplitter.split_text` | 切句 + 交给 `_merge_splits` |
| `libs/text-splitters/langchain_text_splitters/spacy.py` | `SpacyTextSplitter` | 按 `Doc.sents` 切 |
| `libs/text-splitters/langchain_text_splitters/spacy.py` | `SpacyTextSplitter.split_text` | 生成器 + `s.text` / `s.text_with_ws` 二选 |
| `libs/text-splitters/langchain_text_splitters/spacy.py` | `_make_spacy_pipeline_for_splitting` | `sentencizer` 走规则、其余走模型并排除 `ner`/`tagger` |
| `libs/text-splitters/langchain_text_splitters/sentence_transformers.py` | `SentenceTransformersTokenTextSplitter` | 唯一按精确 token 数切的分块器 |
| `libs/text-splitters/langchain_text_splitters/sentence_transformers.py` | `_initialize_chunk_configuration` | 默认取模型上限 + 两条分别报错的校验 |
| `libs/text-splitters/langchain_text_splitters/sentence_transformers.py` | `_encode` | 巨大 `max_length` + 禁用截断 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `Tokenizer` / `split_text_on_tokens` | 滑动窗口算法，三个分块器里唯一的精确尺寸路径 |

## 参考

- [langchain 源码：sentence_transformers.py](https://github.com/langchain-ai/langchain/blob/master/libs/text-splitters/langchain_text_splitters/sentence_transformers.py)
- [langchain 源码：nltk.py](https://github.com/langchain-ai/langchain/blob/master/libs/text-splitters/langchain_text_splitters/nltk.py)
- [langchain 源码：spacy.py](https://github.com/langchain-ai/langchain/blob/master/libs/text-splitters/langchain_text_splitters/spacy.py)
- [spaCy 文档：sentence segmentation](https://spacy.io/usage/linguistic-features#sbd)
- [NLTK 文档：tokenize](https://www.nltk.org/api/nltk.tokenize.html)

## 与主核 / AI 的关系

这套「按语义边界切」与主核里的**按记录边界切页**是同一个问题的两种版本：数据库不能把一条记录跨页拆开，就像分块器不应该把一句话切开。而两者都必须处理同一个例外——**当最小不可分单元本身就超过上限时怎么办**。

主核的答案通常是「允许跨页 / 溢出到外部存储」，而这里三个分块器的答案是「让它超限并 `warn`」。这不是偷懒，而是因为它们没有「溢出」这个选项——一个句子必须在同一个块里，否则它的语义就断了。

对 agent 工程的直接价值有三条。其一，**「语义边界」与「精确尺寸」是两条互斥的路，选哪条取决于用途**。要检索就去嵌入模型侧，尺寸必须精确（token 数）；要阅读就给模型看，语义完整更重要。其二，**编码时的静默截断是分块器最危险的坑**。`truncation="do_not_truncate"` 与巨大的 `max_length` 这两个设置，是防止「一千页文档只有前 512 token 进了块」的唯一保险。其三，**模型上限必须与配置上限对账**。`tokens_per_chunk > max_seq_length` 的错误在分块时拦下，比留到嵌入阶段变成「一半块嵌不进」要好得多。
