---
project: llama-index
feature: semantic-splitter
title: 语义切分
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/node_parser/text/semantic_splitter.py
    symbols: [SemanticSplitterNodeParser, SentenceCombination]
  - path: llama-index-core/llama_index/core/node_parser/text/semantic_double_merging_splitter.py
    symbols: [SemanticDoubleMergingSplitterNodeParser]
---

# 语义切分

## 一句话职责

两个把「边界该切在哪」从长度/标点交给**相邻文本的向量相似度**的切分器：`SemanticSplitterNodeParser` 先算每对相邻句子的余弦距离，再取距离分布的**分位数**作阈值——高于阈值的那些位置就是切点；`SemanticDoubleMergingSplitterNodeParser` 则完全不看分布，改用三个绝对相似度阈值（新建块 / 追加 / 合并）两轮贪心地把句子拼成块，并受一个字符上限约束。

## 机制是什么

这两者回答的是同一个问题：**如果没有可用的结构标记（不是 Markdown、没有标题、标点也不可靠），边界还能靠什么决定？**

答案只能是内容本身。而「内容相近的地方不该切开、内容突变的地方应该切开」这件事，可以用相邻片段的向量相似度做代理。于是两个切分器都变成「先算相似度序列，再在上面找切点」——差别全在**怎么把相似度变成决策**：

| | `SemanticSplitterNodeParser` | `SemanticDoubleMergingSplitterNodeParser` |
|---|---|---|
| 相似度由谁来算 | 必须给 `embed_model`（`from_defaults` 缺省拉 OpenAI 嵌入） | 可给 `embed_model`，也可用 spaCy 词向量 |
| 阈值来源 | 距离序列的**分位数**（相对） | 三个固定值（绝对） |
| 长度约束 | **无** | `max_chunk_size`（字符数） |
| 决策次数 | 一次线性扫描 | 两轮（先建初始块，再合并块） |
| 相似度的比较对象 | 相邻两个「句子窗口」的嵌入 | 逐对文本（块 vs 句子 / 块 vs 块） |

第一列那个「无长度约束」是它最重要的特性，也是最危险的特性：分位数阈值**保证一定会有切点**（默认 95 分位，即距离最大的那 5% 一定被切开），但**不保证块不超过任何预算**。一份 60 句、句子都很相似的文档，可能只切出 2 个块，每块 30 句——远超任何模型的上下文。

第二列的设计正是为修补这一点：它的每一次合并都要过 `len(current) + len(next) + 1 <= max_chunk_size` 这道门。代价是它引入了**绝对阈值**，而绝对阈值在不同语料上表现差异很大（一个 0.8 的相似度在技术文档里意味着「高度相关」，在闲聊文本里可能只是「都用了常见词」）。

## 怎么实现的

### 第一路：分位数断点

**`SentenceCombination` 是这条流水线的数据单位。**

```
class SentenceCombination(TypedDict):
    sentence: str
    index: int
    combined_sentence: str
    combined_sentence_embedding: List[float]
```

注意它同时存了 **`sentence`（原句）与 `combined_sentence`（窗口文本）**：**嵌入是对窗口算的，切点是对原句定的**。这个双份存储是全篇第一个关键设计。

**窗口的构造是「前后各 `buffer_size` 句」拼起来。**

```
for i in range(len(sentences)):
    combined_sentence = ""
    for j in range(i - self.buffer_size, i):
        if j >= 0:
            combined_sentence += sentences[j]["sentence"]
    combined_sentence += sentences[i]["sentence"]
    for j in range(i + 1, i + 1 + self.buffer_size):
        if j < len(sentences):
            combined_sentence += sentences[j]["sentence"]
    sentences[i]["combined_sentence"] = combined_sentence
```

于是 `buffer_size=1` 时窗口是「上一句 + 本句 + 下一句」，默认就是 1。

**为什么需要窗口？** 因为单句嵌入太短、太不稳定：一句 8 个词的话，其向量几乎由词频决定，两句话因为都用「the」就可能算出高相似度。把一个句子放进上下文中算嵌入，向量才携带「这段话在讲什么」的信息。这与 a02 的句子窗口（检索单位 vs 生成单位）是不同的动机——这里的窗口是**为了稳定度量**，不是为了提供上下文。

副作用也很实在：**窗口有 3 句，相邻两个窗口共享 2 句**，于是它们的向量天然接近——**距离序列会被系统性压低且平滑**。这解释了为什么阈值必须用分位数而不是绝对值：绝对值在平滑后的分布上没有意义。

**距离是「1 − 相似度」，一个向量只算一次。**

```
combined_sentence_embeddings = self.embed_model.get_text_embedding_batch(
    [s["combined_sentence"] for s in sentences], show_progress=show_progress)
for i, embedding in enumerate(combined_sentence_embeddings):
    sentences[i]["combined_sentence_embedding"] = embedding

distances = self._calculate_distances_between_sentence_groups(sentences)
```

```
similarity = self.embed_model.similarity(embedding_current, embedding_next)
distance = 1 - similarity
```

**`get_text_embedding_batch` 是唯一的一次嵌入调用**——N 句话 N 次嵌入，而不是 N 个相邻对 N−1 次（那会重复嵌入同一句两次）。这个批量化很重要：嵌入是这套机制里唯一的外部开销。

`1 - similarity` 的取值依赖 `similarity` 的定义（`BaseEmbedding.similarity` 默认是余弦相似度，值域 [−1, 1]）。于是**距离可能大于 1**（相似度为负时）。这一点在分位数方案里不影响正确性（阈值也按同一分布算），但在绝对阈值方案里就意味着「阈值必须落在 [0, 1] 区间」这个隐含假设。

**切点由分位数决定，这一步是全篇最需要细看的地方。**

```
breakpoint_distance_threshold = np.percentile(distances, self.breakpoint_percentile_threshold)
indices_above_threshold = [i for i, x in enumerate(distances) if x > breakpoint_distance_threshold]
```

`distances` 有 N−1 项，`distances[i]` 是「句子 i 与句子 i+1 的距离」。**分位数阈值落在距离分布上，而不是固定值。** 默认 95 意味着「取第 95 百分位的距离值作为门槛」——大约 5% 的位置（距离最大的那些）会被切开。

三个必须理解的性质：

1. **切点数量不由内容决定，由参数决定**。`breakpoint_percentile_threshold=95` 在任何文档上都倾向于切出「距离前 5%」的位置。一份语义连贯的文档也会有 5% 的位置被切（因为它们相对距离更大），一份语义跳跃的文档也只切 5% 的位置（即使绝对距离都很大）。**阈值是相对的，切点数与文档长度成正比**。
2. **`>` 而不是 `>=`**：等于阈值的那个位置不切。由于阈值本身取自某个样本值，用 `>` 就把「恰好等于阈值的那一个」排除掉，于是**切点数严格少于 5%**（可能少一个）。这个细节避免了「所有距离相等时全部切开」的退化（因为分位数会等于那个相等值，而 `>` 让它们全都不满足）。
3. **`np.percentile` 对「只有 1 个距离」的处理**：N=2 时 `distances` 只有 1 项，分位数返回那一个值，于是 `x > threshold` 为假——**两句话永不切开**。这正好落进下一段的兜底分支之外（`len(distances) > 0` 为真），于是产出 1 个块。

**组块是从头扫一遍切点。**

```
start_index = 0
for index in indices_above_threshold:
    group = sentences[start_index : index + 1]
    combined_text = "".join([d["sentence"] for d in group])
    chunks.append(combined_text)
    start_index = index + 1

if start_index < len(sentences):
    combined_text = "".join([d["sentence"] for d in sentences[start_index:]])
    chunks.append(combined_text)
```

**`index + 1` 是关键**：距离 `distances[i]` 衡量的是「句子 i 与 i+1 之间」，所以切点落在两者**之间**。此块包含 `sentences[start : i+1]`（即含句子 i），下一块从 `i+1` 开始。若写成 `index` 而不加 1，句子 i 就会被丢进下一块，而它的距离明明是「与 i+1 的」——切点会偏一位。

**用 `"".join` 而不是 `" ".join`** ——这一处值得记：句子切分器（`split_by_sentence_tokenizer`）通常**保留尾随空格**，所以直接拼接不会把词粘在一起。若换成别的 `sentence_splitter`（用户可传任意 `Callable[[str], List[str]]`）且它 strip 了句子，`""` 拼接就会让词连在一起（`"Hello world" + "How are you"` → `"Hello worldHow are you"`）。这是一个依赖上游实现的隐含约定。

**零距离的兜底分支。**

```
else:
    # If, for some reason we didn't get any distances (i.e. very, very small documents) just
    # treat the whole document as a single node
    chunks = [" ".join([s["sentence"] for s in sentences])]
```

`len(distances) == 0` 只发生在「0 句或 1 句」的情况下。注意这里用的是 `" ".join`（带空格），与正常路径的 `"".join` 不同——**同一个函数里两种拼接方式**，是「两条路径各写各的」留下的不一致。对一句话的文档两种都等价，对空文档前者返回 `""`、后者返回 `[]`。

**顺带一个会崩的边界**：`_build_sentence_groups` 对空 `text_splits` 返回空列表，`get_text_embedding_batch([])` 的行为取决于实现，而 `_build_node_chunks` 走 else 分支返回 `[""]`。于是**空文档产出一个空字符串块**，而不是零个块。

**`from_defaults` 的一条硬绑定值得记。**

```
if embed_model is None:
    try:
        from llama_index.embeddings.openai import OpenAIEmbedding
        embed_model = embed_model or OpenAIEmbedding()
    except ImportError:
        raise ImportError("`llama-index-embeddings-openai` package not found, please run `pip install llama-index-embeddings-openai`")
```

**没显式给 `embed_model` 时会静默构造一个 `OpenAIEmbedding()`** ——于是「忘了配嵌入模型」的后果是「运行时尝试用一个未配置 API key 的 OpenAI 客户端」。`embed_model` 字段本身是必填（`embed_model: SerializeAsAny[BaseEmbedding] = Field(description=...)`，没有 default），所以这条路只在 `from_defaults` 里存在。

而 `sentence_splitter` 字段用 `default_factory=split_by_sentence_tokenizer` 且 `exclude=True`（不参与序列化）——它是纯行为配置，不是可持久化的数据。

**`from_defaults` 的签名里还有一个实际不存在的参数。** 它接受 `original_text_metadata_key: str = DEFAULT_OG_TEXT_METADATA_KEY` 并原样传给 `cls(...)`，但这个类**没有声明同名字段**（它的字段只有 `sentence_splitter` / `embed_model` / `buffer_size` / `breakpoint_percentile_threshold` 四个，加上父类那四个）。而 pydantic v2 在未显式设置 `extra` 时默认取 `ignore`——**未知关键字参数被静默丢弃，不报错、也不留存**。于是这个参数从签名上看存在、从行为上完全无效。

同一个参数在 `sentence_window.py` 里是**真正的字段**（`SentenceWindowNodeParser` 声明了它并在切分时把原句写进 `node.metadata[self.original_text_metadata_key]`）。也就是说它在这两个语义切分器上属于「从别处抄来的签名」：`from_defaults` 的入参表与 `sentence_window` 对齐了，字段声明却没有跟上。这类缺陷的症状是「传了参数但没有任何效果」——比报错更难发现。

### 第二路：双轮合并

**先建初始块：一段「三种状态」的贪心。**

`_create_initial_chunks` 逐句扫描，维护 `chunk`（当前累积）、`new`（当前块是否只有一句/刚开）、`last_sentences`（块内最后两句的拼接）与 `chunk_sentences`（块内句子列表）。

四个分支：

1. **新块、相似度低于 `initial_threshold`、且插入不超 `max_chunk_size`** → 收尾，当前句起新块（`<` 判「不够像就断」）。
2. **新块、相似度够，但插入会超 `max_chunk_size`** → 同样收尾起新块。
3. **新块、相似度够、长度也够** → 把当前句并入，`new = False`，算 `last_sentences`。
4. **非新块**：若 `last_sentences` 与当前句的相似度 **高于** `appending_threshold` 且不超长度 → 追加；否则收尾、起新块、`new = True`。

注意第 1 支的合取条件：**「不相似」与「放得下」必须同时成立才走这条快路径**。若「不相似」但「放不下」，代码在条件短路后落进下面的 `chunk_sentences = [chunk]` 再检查一次长度，这次必然失败、走其 `else`——**净效果与第 1 支完全相同**（都是「收尾当前块、当前句起新块」）。也就是说「相似度不足」与「长度不够」这同一个结局被写成了两条路径，而长度判断在这段里重复出现三次（第 1 支的合取、第 3 支的判据、以及第 4 支的合取）。这段分支是良性的冗余，不是逻辑错误。

**注意第 4 支比较的是 `last_sentences`（块内最后两句）而不是整个块。** 这是一个有意的局部化：块的「语义重心」应当由它最近的内容决定，否则块越长、与后续句的相似度越被稀释，就会提前断开。副作用是**块的连贯性只对相邻两句有保证**，长块的早期内容可能与后期内容无关——这正是第二轮「合并」想修补的问题。

**第 3 支里 `chunk_sentences` 的初始化有个隐含前提。**

```
chunk_sentences = [chunk]
if len(chunk) + len(sentence) + 1 <= self.max_chunk_size:
    chunk_sentences.append(sentence)
    chunk = self.merging_separator.join(chunk_sentences)
    new = False
else:
    new = True
    initial_chunks.append(chunk)
    chunk = sentence
    continue
```

`chunk_sentences = [chunk]` 把「当前块」当作**一个元素**——但 `chunk` 可能已经是多句拼接的结果（第 3 支进入时 `new` 为真，意味着上一轮刚收尾过，所以 `chunk` 只有一句……**除了第一轮**）。第一轮时 `chunk = sentences[0]`（单句），所以这条也成立。

严格说这个不变式是「`new == True` 时 `chunk` 只有一句」。它由三处维护：初始化、第 1 支收尾后起新块、第 4 支收尾后起新块。若任何一处漏掉，`chunk_sentences` 就不再是「句子列表」而是「块 + 句子」。这是那种「靠不变量而非结构保证」的脆弱点。

`last_sentences = self.merging_separator.join(chunk_sentences[-2:])` ——取最后两句。第 4 支里也这样做，于是它的比较基准与第 3 支初始化后的一致。

**末尾无条件 `initial_chunks.append(chunk)`** ——所以即使循环体从未进过任何分支（单句文档），也产出一个块。注意这行**在 `for` 之外**，而第 1、2、4 支内部各有 `continue`，所以不会重复追加。

**再合并初始块：这是「跳过式」的合并。**

```
skip = 0
current = initial_chunks[0]
for i in range(1, len(initial_chunks)):
    if skip > 0:
        skip -= 1
        continue
    if len(current) >= self.max_chunk_size:          # 满了，收尾
        chunks.append(current); current = initial_chunks[i]
    elif self._similarity(current, initial_chunks[i]) > self.merging_threshold and 长度够:
        current += sep + initial_chunks[i]           # 直接合并下一个
    elif self._similarity(current, initial_chunks[i+1]) > self.merging_threshold and 长度够:
        current += sep + i + sep + i+1               # 跨过 i+1 的先见之明？不 —— 合并 i 与 i+1
        skip = 1
    elif (self.merging_range == 2 and self._similarity(current, initial_chunks[i+2]) > ...):
        current += sep + i + sep + i+1 + sep + i+2
        skip = 2
    else:
        chunks.append(current); current = initial_chunks[i]
```

四支的顺序就是**贪心的优先级**：能合下一个就合；不能但「再下一个」更像就跨一个；`merging_range=2` 时还能跨两个。

**第三支的语义需要想清楚**：`current` 与 `initial_chunks[i+1]` 相似度足够高时，它把 **`i` 和 `i+1` 两个块一起**并入 `current`，并设 `skip = 1` 让循环跳过 `i+1`。

也就是说：**即便 `current` 与 `i` 不相似，只要它与 `i+1` 相似，就把 `i` 当「中间物」一起吞掉。** 这是一个「为了让相似的块相邻而牺牲一次」的策略——它假定 `i` 是 `i+1` 的铺垫（过渡段），吞掉它比留着它更好。

这个策略的风险很具体：**若 `i` 本身是一个完整独立的主题**，它就被强行并进了一个与它无关的块。而 `merging_range=2` 时最坏情况是一次吞掉两个无关块。

`skip` 的维护要注意：`skip -= 1; continue` 在循环开头，于是被跳过的块**完全不参与后续判断**（既不检查相似度，也不检查长度）。长度检查靠的是第三/四支里那个「三/四个块相加 + 2/3 个分隔符 ≤ max」的显式计算——**这是唯一一处保证合并后不超上限的地方**，而它的写法是把所有长度手工加起来。

**`_similarity` 有一个会改变行为的副作用。**

```
def _similarity(self, text_a: str, text_b: str) -> float:
    if self.embed_model is not None:
        embeddings = self.embed_model.get_text_embedding_batch([text_a, text_b])
        return self.embed_model.similarity(embeddings[0], embeddings[1])
    if self.language_config.nlp is None:
        self.language_config.load_model()
    ...
    return self.language_config.nlp(clean_a).similarity(self.language_config.nlp(clean_b))
```

**每一对比较都要调一次嵌入接口**（`get_text_embedding_batch([a, b])`）。而 `_create_initial_chunks` 与 `_merge_initial_chunks` 里的比较次数是 **O(N)** 量级（每个句子/块一次或两次），**没有任何批量化**。

对比第一路：它只做**一次批量嵌入**（N 句一起），然后在本地算距离。第二路的做法让嵌入调用次数从 1 变成 ~2N。若 `embed_model` 是远程 API（OpenAI 等），这就是 **2N 次网络往返**——一个 200 句的文档要 400 次调用。这是这两个切分器最实际的性能差异，而它完全不出现在参数表上。

spaCy 路径则相反：`nlp(...).similarity(...)` 是**本地计算**（词向量平均后的余弦），没有网络开销，但需要加载 100+MB 的语言模型，且只支持三种语言（`LANGUAGES = ["english", "german", "spanish"]`，注释写着 `# TODO test more languages`）。

`language_config.nlp is None` 时才 `load_model()` ——惰性加载，于是「配置了但没用到」不会付加载代价。而 `_parse_nodes` 一开始就 `if self.embed_model is None: self.language_config.load_model()`，提前加载以避免在循环里反复检查。

**spaCy 路径要先清洗文本，清洗规则是一套固定的 NLP 预处理。**

```
text = text.lower()
text = re.sub(r"http\S+|www\S+|https\S+", "", text, flags=re.MULTILINE)
text = text.translate(str.maketrans("", "", string.punctuation))
tokens = re.findall(r"\w+", text)
filtered_words = [w for w in tokens if w not in self.language_config.stopwords]
return " ".join(filtered_words)
```

四步：小写、去 URL、去标点、去停用词。

**这套清洗只作用在 spaCy 路径上，`embed_model` 路径不做清洗。** 这个不一致有技术理由（现代嵌入模型自己会处理标点与大小写，人工清洗反而伤信息），但它带来一个体验问题：**同一份文档、同样的阈值，换 backend 得到的分块完全不同**。而阈值（0.6/0.8/0.8）是在某个 backend 上调出来的——换 backend 后这些默认值未必合适。

`self.stopwords = set(globals_helper.stopwords)` 那行把 `List` 赋给 `List` 类型的字段（`# type: ignore`），实际存的是 `set`。后面 `w not in self.language_config.stopwords` 用集合查找，复杂度 O(1)——正确，但字段类型声明与实际值不符。

**这个切分器自己建了 PREVIOUS/NEXT 关系。**

```
previous_node: Optional[BaseNode] = None
for split_node in split_nodes:
    if previous_node:
        split_node.relationships[NodeRelationship.PREVIOUS] = previous_node.as_related_node_info()
        previous_node.relationships[NodeRelationship.NEXT] = split_node.as_related_node_info()
    previous_node = split_node
```

**为什么需要它自己建？** 因为 a01 的父类后处理也会建——但只在 `include_prev_next_rel=True` 时。这里无条件建，于是**即使调用方设了 `include_prev_next_rel=False`，关系依然存在**。这是一个行为覆盖：字段名义上可关，实际关不掉。

而 `as_related_node_info()` 每次都会读 `node.hash`（重算 sha256），这里对每对相邻节点各调两次 → 2(N−1) 次哈希计算。a01 里为 `SOURCE` 关系专门做过「只算一次」的优化，这里没有。

三个阈值字段的语义区别值得单独列：

| 字段 | 默认 | 判据方向 | 作用 |
|---|---|---|---|
| `initial_threshold` | 0.6 | `similarity < threshold` 判「不够像」 | 决定「起新块」还是「并入两句」 |
| `appending_threshold` | 0.8 | `similarity > threshold` 判「够像」 | 决定「继续追加」 |
| `merging_threshold` | 0.8 | `similarity > threshold` 判「够像」 | 决定「两块合并」 |

**`initial_threshold` 比另两个低 0.2，方向也相反。** 这个不对称是有道理的：`initial` 判的是「是否必须断开」（所以判据是「不够像」，门槛低意味着更容易断开），而另外两个判的是「是否可以连上」（判据是「够像」，门槛高意味着更保守地连）。**断开宁可松、连接宁可紧**——这是分段算法的一般原则：错误的合并比错误的断开更难补救（断开后还能靠上下文扩展修复，合并后无法拆开）。

## 关键取舍

**源码事实：** `SemanticSplitterNodeParser` 的嵌入是对「前后各 `buffer_size` 句」的窗口算的，而切点是对原句定的；`SentenceCombination` 同时存原句与窗口文本。

**源码事实：** 相邻窗口共享 `2 * buffer_size` 句，因此距离序列被系统性压低且平滑——这正是阈值必须用分位数而非固定值的理由。

**源码事实：** 切点阈值是距离序列的 `np.percentile(distances, breakpoint_percentile_threshold)`（默认 95），用 `>` 比较；于是切点数量由参数决定、与文档长度成正比，且**不保证任何长度上限**。

**源码事实：** 组块时切点 `index` 落在句子 `i` 与 `i+1` 之间，故此块取 `sentences[start : index+1]`、下块从 `index+1` 起。正常路径用 `"".join`（依赖句子切分器保留尾随空格），零距离兜底分支用 `" ".join`。

**源码事实：** `from_defaults` 在未给 `embed_model` 时**静默构造 `OpenAIEmbedding()`**；`embed_model` 字段本身无默认值。

**源码事实：** `SemanticDoubleMergingSplitterNodeParser` 的三个阈值方向不一致：`initial_threshold`（0.6）用 `<` 判断开，`appending_threshold`（0.8）与 `merging_threshold`（0.8）用 `>` 判连接——断开松、连接紧。

**源码事实：** 第一轮追加判据比较的是「块内最后两句」而非整个块；`chunk_sentences` 的正确性依赖「`new == True` 时 `chunk` 只有一句」这个由三处共同维护的不变量。

**源码事实：** 第二轮合并是贪心四支，其中「与 `i+1` 相似就吞掉 `i` 与 `i+1`」会牺牲一个可能独立的块；`merging_range=2` 时最坏一次吞掉两个。长度上限只在合并分支里靠「所有块长度 + 分隔符数量」显式相加保证。

**源码事实：** `_similarity` 每比较一对就调一次 `get_text_embedding_batch([a, b])`，而比较次数是 O(N) 量级且无批量化——与第一路的「一次批量嵌入 + 本地算距离」形成鲜明对比。

**源码事实：** spaCy 路径先做四步清洗（小写、去 URL、去标点、去停用词），`embed_model` 路径不清洗；语言支持限于 `english`/`german`/`spanish`（源码有 `# TODO test more languages`）。`stopwords` 字段声明为 `List[str]` 实际存 `set`（带 `# type: ignore`）。

**源码事实：** 该切分器**无条件**建 PREVIOUS/NEXT 关系，于是 `include_prev_next_rel=False` 无法关闭它们；且每次 `as_related_node_info()` 都会重算节点哈希。

**源码事实：** 两个切分器的 `from_defaults` 都接受 `original_text_metadata_key` 并传给构造器，但**两个类都没有声明这个字段**；pydantic v2 默认 `extra="ignore"`，故该参数被静默丢弃——签名上存在、行为上完全无效（同名的真字段在 `SentenceWindowNodeParser` 里）。

**架构推论：** 「分位数阈值」把「切多少块」交给参数、「切在哪」交给内容。这使它在任何文档上都能产出非退化的结果（既不会全切也不会不切），代价是**块尺寸完全不可控**。对需要硬预算的下游（LLM 上下文），它必须再套一层长度切分——而那一步会破坏语义边界。这是「相对阈值」的固有代价。

**架构推论：** 「绝对阈值 + 长度上限」是相反的选择：块尺寸可控，但阈值的意义依赖语料。三个阈值的方向设计（断开松、连接紧）值得作为通用原则记录——**错误的合并不可修复，错误的断开可以靠上下文扩展补救**。

**架构推论：** 两个切分器的性能都不是 O(1) 的设备开销：第一路是 N 次嵌入（一次批量），第二路是 ~2N 次嵌入调用（逐对）。在远程嵌入服务上，这决定了可用规模。**任何把「相似度」放进循环的算法都必须问一句「这次调用是本地计算还是网络往返」**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/node_parser/text/semantic_splitter.py` | `SemanticSplitterNodeParser` | 分位数断点式语义切分：一次批量嵌入 + 本地距离 + 分位阈值 |
| `llama-index-core/llama_index/core/node_parser/text/semantic_splitter.py` | `SentenceCombination` | 原句 + 窗口文本 + 窗口嵌入的四字段载体 |
| `llama-index-core/llama_index/core/node_parser/text/semantic_splitter.py` | `_build_sentence_groups` | 拼出「前后各 buffer_size 句」的窗口文本 |
| `llama-index-core/llama_index/core/node_parser/text/semantic_splitter.py` | `_calculate_distances_between_sentence_groups` | `1 - similarity` 的相邻距离序列 |
| `llama-index-core/llama_index/core/node_parser/text/semantic_splitter.py` | `_build_node_chunks` | 分位数阈值 + `>` 比较 + 切点偏移 +1 的组块 |
| `llama-index-core/llama_index/core/node_parser/text/semantic_double_merging_splitter.py` | `SemanticDoubleMergingSplitterNodeParser` | 三阈值双轮合并；spaCy 或嵌入两种后端 |
| `llama-index-core/llama_index/core/node_parser/text/semantic_double_merging_splitter.py` | `_create_initial_chunks` | 第一轮：四支贪心建初始块，追加判据用「最后两句」 |
| `llama-index-core/llama_index/core/node_parser/text/semantic_double_merging_splitter.py` | `_merge_initial_chunks` | 第二轮：贪心四支合并块，可跳过中间块 |
| `llama-index-core/llama_index/core/node_parser/text/semantic_double_merging_splitter.py` | `LanguageConfig` / `_clean_text_advanced` | spaCy 后端配置与四步文本清洗（仅该后端使用） |
| `llama-index-core/llama_index/core/node_parser/text/semantic_double_merging_splitter.py` | `LANGUAGES` / `LANGUAGE_MODELS` | 支持语言与模型对的硬编码表 |

## 参考

- [llama-index 源码：node_parser/text/semantic_splitter.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/text/semantic_splitter.py)
- [llama-index 源码：node_parser/text/semantic_double_merging_splitter.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/text/semantic_double_merging_splitter.py)
- [llama-index 文档：语义切分器](https://docs.llamaindex.ai/en/stable/module_guides/loading/node_parsers/modules/)
- [NumPy 文档：`percentile`](https://numpy.org/doc/stable/reference/generated/numpy.percentile.html)

## 与主核 / AI 的关系

「按内容相似度找边界」在主核里对应的是**按数据分布决定分区边界**：分片、聚簇、以及在流式摄取里「按语义批次提交」。这里有四条经验。

**第一，相对阈值与绝对阈值不能混用，必须选一个并接受它的代价。** 分位数保证非退化但不可控尺寸；绝对阈值可控尺寸但依赖语料。主核里做「按大小切分 / 按相似度聚类」时同理：分位数让「切多少」变成参数，绝对阈值让「切在哪」变成参数。混用（例如先用分位数定切点再用绝对上限截断）会导致两层都失效——a02 与 a03 里那些「超长片段被保留」的分支正是这种混用的结果。

**第二，比较基准宜局部化。** 第二路的追加判据只用「块内最后两句」，理由是长块的整体向量会稀释后续句的相似度。主核里做「流式聚合」时同样应当用「最近窗口」而不是「全程摘要」——后者的判断会随历史变长而钝化。

**第三，把相似度放进循环之前必须先问「这次调用贵不贵」。** 第一路一次批量嵌入、第二路逐对调用，两者在远程嵌入服务上的成本差了两个数量级。主核里若在循环里调用一个跨进程/跨网络的度量函数，应当先把所有输入攒起来批量算（就像第一路的 `get_text_embedding_batch`），再在本地做比较。

**第四，「断开松、连接紧」是分段算法的一般原则。** 这里用 `initial_threshold=0.6`（判「不够像就断」）与 `appending/merging=0.8`（判「够像才连」）把这条原则编码进了默认值。理由是不可逆性：错误的合并无法在后处理阶段拆开，错误的断开还能靠上下文扩展（a02 的句子窗口、a06 的父子关系）补救。主核里凡是做「合并小对象成大对象」的优化（页合并、段合并、批合并），判据都应当比「拆分」更保守。
