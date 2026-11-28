---
project: haystack
feature: semantic-splitter
title: 基于嵌入的语义切分
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/preprocessors/embedding_based_document_splitter.py
    symbols: [EmbeddingBasedDocumentSplitter]
  - path: haystack/components/preprocessors/document_splitter.py
    symbols: [DocumentSplitter]
---

# 基于嵌入的语义切分

## 一句话职责

不按分隔符、不按句子数，而是**按语义漂移断点**切分：把文本按「每组 N 句」嵌入成向量，算相邻组的余弦距离，**把距离超过「本文件自身距离分布的某个百分位」的位置当作切分点**——于是阈值不是一个绝对常数，而是相对于这份文档自己算出来的。

## 机制是什么

它与 `DocumentSplitter` 的差别不是「精度更高」，而是**判定依据换了坐标系**：

| 维度 | `DocumentSplitter` | `EmbeddingBasedDocumentSplitter` |
|---|---|---|
| 边界依据 | 分隔符 / 句子 / token 数 | **相邻块嵌入的余弦距离** |
| 阈值 | `split_length`（绝对数值） | `percentile`（**相对**本文件的距离分布） |
| 成本 | 零模型调用 | **每个句子组一次嵌入** |
| 长度控制 | 主判据 | **事后修正**（`min_length` 合、`max_length` 递归再切） |

这个「主判据换成语义、长度退为事后修正」的次序，是整页所有细节的来源。

## 怎么实现的

### 一、五步流水线

`_split_document` 只有五行，而每一行是一个阶段：

```
splits = self._split_text(text=doc.content)          # 1. 语义切分
merged_splits = self._merge_small_splits(splits)     # 2. 合掉太短的
final_splits = self._split_large_splits(merged_splits)  # 3. 递归切掉太长的
return _create_documents_from_splits(final_splits, doc)  # 4. 造 Document 并写元数据
```

而 `_split_text` 内部又是三步：句子分组 → 算嵌入 → 找切分点。

**注意第 2、3 步的顺序是不能交换的**：先合短再切长。若先切长，一个被切开的长块可能产生更短的碎片，再合并又会跨过原块边界；而先合短再切长，最终结果里「短块」可能是切长时新产生的，但那种情形由递归的终止条件处理（见下）。

### 二、句子分组与一个「空白不能丢」的修补

**`_prepare_sentence_groups` 里有一段注释解释了一个具体的坑。**

```
rstripped_text = text.rstrip()
trailing_whitespaces = text[len(rstripped_text):]
sentences_result = self.sentence_splitter.split_sentences(rstripped_text)
if sentences_result and trailing_whitespaces:
    sentences_result[-1]["sentence"] += trailing_whitespaces
    sentences_result[-1]["end"] += len(trailing_whitespaces)
```

注释原文：`split_sentences` **会把文本末尾的所有空白（换行、分页符等）剥掉**。为了不丢它们，先 `rstrip` 记下尾随空白，切完之后**贴回最后一句**。

**为什么必须贴回？** 因为下游的偏移与页号是累加出来的：

```
current_char_pos += len(split_text)
current_page += page_breaks_in_split
```

**注释明确写了这个前提：「块是原文的连续子串（所有拼接都是纯字符串连接），所以字符偏移就是累加每块的长度」。** 一旦末尾的 `\f` 被丢掉，后面所有块的偏移都会错位、页号也会少算一页。

这与 `e01` 那页 `_split_by_token` 里 `decode_with_offsets` 的用途是同一个：**为了让偏移可累加，必须保证拼接能还原原文**。

**分组本身是朴素的切片。**

```
if self.sentences_per_group == 1: return sentences
groups = ["".join(sentences[i:i + self.sentences_per_group]) for i in range(0, len(sentences), self.sentences_per_group)]
```

**`sentences_per_group` 是一个「噪声与分辨率的折中」**：默认 3。取 1 时每个句子一个向量，边界最精细但单句向量的语义不稳定（短文本的嵌入方差大）；取得太大则一个组内可能已经跨过主题边界，反而切不出来。

而 `_group_sentences` 里对 `sentences_per_group == 1` 有个特判——不是必须的（`range(0, n, 1)` 也会得到相同结果），只是省掉一层列表构造。

### 三、找切分点：百分位阈值与 `+1`

**`_find_split_points` 的实现只有三步，而每一步都有一个值得记的选择。**

```
if len(embeddings) <= 1: return []
distances = [self._cosine_distance(embeddings[i], embeddings[i + 1]) for i in range(len(embeddings) - 1)]
threshold = np.percentile(distances, self.percentile * 100)
split_points = [i + 1 for i, distance in enumerate(distances) if distance > threshold]
```

**第一，距离是「相邻两组的余弦距离」，不是与全局中心的距离。**

这一点决定了它只能发现**局部**的主题转移。一段先讲 A、再讲 B、最后讲回 A 的文字，在 A→B 与 B→A 两个边界都有高距离——这是对的。但「整篇都在讲 A，中间夹一段与 A 相似度中等但其实是另一主题」的内容可能切不出来。

**第二，`percentile` 是相对本文件的。**

`percentile=0.95` 意味着「把本文件的距离分布排序，取第 95 百分位」。于是**总是有大约 5% 的位置被切**——哪怕整篇文章语义完全均匀。

这个性质对「切分」来说其实是优点：**极端均匀的文本也需要被切开**（否则会得到一个超长块，然后由 `max_length` 强行切开，那个切点更差）。而对「边界是否真实」的判断交给了后续的长度修正。

代价是**不可比**：`percentile=0.95` 在两份语义结构完全不同的文档上切出的块数占总位置的比例相同，而各自的块数规模可能差很多。

**第三，`np.percentile` 的插值行为意味着「超过阈值」可能切出比 `(1 - percentile)` 更少的位置。**

`np.percentile` 默认用线性插值，返回的不一定是距离数组里的某个实际值。于是 `distance > threshold` 的命中数不严格等于 `(1 - percentile) × 样本数`。这是一个统计上的细节，但对「块数是不是符合预期」有实际影响。

**第四，`i + 1` 那个加一是「在距离 i 之后切」。** `distances[i]` 是「第 i 组与第 i+1 组」的距离，所以切分点（下一块的第一组的下标）是 `i + 1`。

**`_cosine_distance` 有一个零向量的兜底。**

```
if norm1 == 0 or norm2 == 0: return 1.0
```

返回 1.0（最大距离）而不是 0 或抛错——**零向量意味着「这一组没有有效内容」（例如纯空白组），把它当作一个强边界是合理的**。而这行兜底也避免了除零。

### 四、`_merge_small_splits`：只向前合并，然后补一个向后合并

**正向合并的主循环只朝一个方向看。**

```
current_split = splits[0]
for split in splits[1:]:
    if len(current_split) < self.min_length and len(current_split) + len(split) < self.max_length:
        current_split += split
    else:
        merged.append(current_split); current_split = split
merged.append(current_split)
```

**两个条件同时成立才合并**：当前块还不够长，**而且**合并后不会超上限。第二个条件注释写明了理由——避免把一个短块与一个长块合并成一个超上限的块（那样又要进递归切分）。

**而末尾需要一个单独的向后合并，注释直接点出了原因。**

> 循环只向前合并，所以最后一块仍可能低于 `min_length`。把它向后合并，受与正向合并相同的 `max_length` 限制。

```
if len(merged) > 1 and len(merged[-1]) < self.min_length and len(merged[-2]) + len(merged[-1]) < self.max_length:
    trailing_split = merged.pop(); merged[-1] += trailing_split
```

**「只向前」为什么留下了尾巴？** 因为判断「当前块太短」用的是**已经合并过的当前块**，而最后一块从没被当作「当前块」去检查与下一块的关系——它只是被 append 了。于是若最后一块本身很短，没有任何后续块能吸收它，只能往后并给前一块。

**这也说明一个不对称**：向前合并时考虑的是「当前太短就吃掉下一个」，向后合并时是「最后太短就并进前一个」。前者可能让块超过 `min_length` 很多（连续吃几个）、后者只并一次。

### 五、`_split_large_splits`：递归，以及为什么重跑能切得更细

**这是本页最需要解释的一处：为什么对一个大块再调一次 `_split_text` 就能切开？**

```
if len(split) <= self.max_length: final_splits.append(split)
else:
    sub_splits = self._split_text(text=split)
    if len(sub_splits) == 1:
        logger.warning("Could not split a chunk further below max_length={max_length}. Returning chunk of length {length}.", ...)
        final_splits.append(split)
    else:
        final_splits.extend(self._split_large_splits(splits=sub_splits))
```

**代码注释给了答案：「阈值是根据 `split` 的嵌入动态算出来的」。**

具体机制是：`percentile` 是**相对**的。第一次切分时，阈值来自全文的距离分布；一个大块之所以没被切开，是因为**它的内部距离都没有超过全文的第 95 百分位**——那些高距离点在别的块边界上。

而当只给这一个块重算时，距离分布变成了「这个块内部的距离」，第 95 百分位相应降低——**原本排不进全文前 5% 的距离，在块内可能就排得进**。于是同一个 `percentile` 值在小范围内切得更细。

**这是一个「用相对阈值做递归细化」的自洽设计，而它有一个明确的边界：** 若块内距离全为 0（完全均匀的文本），任何百分位都不会切出东西 → `len(sub_splits) == 1` → 打警告并**返回超限的块**。

**这个失败出口是显式的**（`logger.warning` 带上了实际长度），而不是静默 —— 与 `e01` 那页「切不动就保留超长片段并抛 `ValueError`」的取向不同：这里的超限只是一个质量损失（下游 embedder 可能截断），不是崩溃点。

**而 `_split_large_splits_async` 是逐行镜像，注释说明了两件事：**

> 镜像 `_split_large_splits`，但通过异步路径嵌入，使递归不会用同步 embedder 调用阻塞事件循环。

以及一句维护性要求：**「与 `_split_large_splits` 保持同步」**（另一半也写了同样的注释）。这是一份明确的重复代码，而作者用交叉注释承认了它。

**异步路径的存在意义值得记：** 这类组件的同步实现里嵌入了「每步都要调一次模型」的循环——在事件循环里跑它会直接阻塞。所以 `run_async` 不是「为了好看」，而是**避免阻塞**。这与 `AGENTS.md` 里「只在有真实异步路径时才实现 `run_async`」这条规定的精神一致。

### 六、元数据：与 `e01` 同一套键，但少两个

**`_create_documents_from_splits` 写三个键，而它只有三个（`e01` 有四个）。**

```
metadata["source_id"] = original_doc.id
split_meta["split_id"] = i
split_meta["split_idx_start"] = current_char_pos
split_meta["page_number"] = current_page + _leading_page_breaks(split_text)
...
current_char_pos += len(split_text)
current_page += page_breaks_in_split
```

**没有 `_split_overlap`** —— 因为这个名字里没有「重叠」这个概念：它按语义边界切开，相邻块之间**不共享任何字符**。

而这带来的一个直接后果：**它无法被 `sentence_window` 类组件当成「有重叠的来源」处理**，但反过来，它切出的块天然满足「块之间无冗余」，对嵌入检索反而更干净。

**而 `page_number` 的算法有三处与 `e01` 一致：**

- 用 `_leading_page_breaks(split_text)` 处理「块开头的分页符」（同一个共享模块）。
- `current_page` 累加的是 `split_text.count("\f")`（**全部**分页符，不是开头的那些）。
- 于是「块的页号」是 `current_page + 开头分页符数`，而下一块的 `current_page` 已经把本块的所有分页符算进去了。

**这套递推正确的前提正是「拼接能还原原文」**（前面那个 `trailing_whitespaces` 修补保证的）。若某处的空白被吞掉，`current_char_pos` 与 `current_page` 都会被永久带偏——**而且不会报错，只会静默错位**。

### 七、与 `DocumentSplitter` 的对照：两条不可互换的路

**之所以这一页的 refs 里带上了 `document_splitter.py`，是因为这两个组件的取舍恰好互补。**

| | `DocumentSplitter` | `EmbeddingBasedDocumentSplitter` |
|---|---|---|
| 能否离线 / 无模型 | 能 | **不能**（必须先有 `document_embedder`） |
| 成本 | 线性于文本长度、零调用 | **每组一次嵌入调用**（大致等于句子数 / `sentences_per_group`） |
| 可复现性 | 完全确定 | 依赖模型版本与随机性（模型输出可能不稳定） |
| 边界质量 | 取决于分隔符与文本是否规整 | 取决于语义是否真的有漂移 |
| 长度保证 | 硬约束（`split_length`） | **软约束**（`min_length`/`max_length` 事后修正，可能超限） |

**最关键的一条差别是最后一行。** `DocumentSplitter` 的产出一定不超过 `split_length`（除单片段本身超限）；而这里**可能产出超限的块**，因为长度不是主判据——`max_length` 的递归切分一旦遇上「块内距离全等」就放弃了。

这也解释了为什么它的文档示例里那组参数（`percentile=0.95, min_length=50, max_length=1000`）看上去很「松」——**它的长度控制本来就在事后**，参数的作用是「修正而非规定」。

## 关键取舍

**源码事实：** 切分主判据是「相邻句子组的余弦距离是否超过本文件距离分布的第 `percentile` 百分位」，长度（`min_length` / `max_length`）只做事后修正。

**源码事实：** 阈值是**相对**的，于是大约总有 `1 - percentile` 比例的位置被切——即使文本语义完全均匀。这让它不可跨文档比较，但也保证了均匀文本不会产出单一超长块。

**源码事实：** `_prepare_sentence_groups` 必须把 `split_sentences` 剥掉的**尾随空白贴回最后一句**，否则字符偏移与页号会永久偏错——因为元数据是靠「累加每块长度」算出来的。

**源码事实：** 余弦距离对零向量返回 `1.0`（最大距离）而不是 0 或抛错——零向量意味着该组没有有效内容，视为强边界，同时避免除零。

**源码事实：** `_merge_small_splits` 的正向合并要求「当前太短」**且**「合并后不超上限」两个条件同时成立；循环只向前看，所以**末尾还要一次单独的向后合并**，否则最后一块可能是短的。

**源码事实：** `_split_large_splits` 对超限块重跑 `_split_text` 之所以能切得更细，是因为阈值相对于**本次输入**的距离分布——块内重算时百分位下降，原本排不进全局前 5% 的距离可能就排得进了。

**源码事实：** 递归切分失败的出口是「块内距离全等 → 无切分点 → `len(sub_splits) == 1` → 打 `logger.warning` 并返回超限块」，而不是抛错。这与 `DocumentSplitter`「切不动就抛 `ValueError`」的取向不同。

**源码事实：** 元数据只写 `source_id` / `split_id` / `split_idx_start` / `page_number`，**没有 `_split_overlap`**——因为块之间不共享字符。

**源码事实：** 同步与异步实现是两份近乎逐行相同的代码，用交叉注释「保持同步」（Keep in sync）互相约束；异步路径的存在理由是「递归里的嵌入调用不能用同步方式阻塞事件循环」。

**架构推论：** 「相对阈值 + 事后长度修正」这个组合是本页的核心方法，而它揭示了一个通用模式：**当判定依据（语义距离）与硬约束（长度上限）不可兼得时，把硬约束降级为事后修正，能同时保住两边的优点**——边界质量由语义决定、极端情形由长度兜底。

代价是**「保证」变成了「倾向」**：`max_length` 不再是一定成立的约束，而是一条「尽力满足、失败时报警告」的目标。而一个依赖「块一定不超限」的下游（比如不做截断的嵌入器）就会在某些输入上出问题。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `EmbeddingBasedDocumentSplitter` | 语义切分主体，五步流水线 |
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `_prepare_sentence_groups` | 句子分组 + 尾随空白贴回 |
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `_group_sentences` | 按 `sentences_per_group` 拼接 |
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `_find_split_points` | 相邻距离 + 百分位阈值 + `i+1` |
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `_cosine_distance` | 余弦距离，零向量返回 1.0 |
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `_merge_small_splits` | 正向合并 + 末尾向后合并 |
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `_split_large_splits` | 递归细化，失败时警告并返回超限块 |
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `_create_documents_from_splits` | 累加偏移与页号，不写 `_split_overlap` |
| `haystack/components/preprocessors/embedding_based_document_splitter.py` | `run_async` / `_split_large_splits_async` | 异步镜像，避免阻塞事件循环 |
| `haystack/components/preprocessors/document_splitter.py` | `DocumentSplitter` | 对照：绝对长度约束、零模型调用的确定性切分 |

## 参考

- [haystack 源码：embedding_based_document_splitter.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/preprocessors/embedding_based_document_splitter.py)
- [haystack 文档：DocumentSplitter](https://docs.haystack.deepset.ai/docs/documentsplitter)
- [numpy 文档：percentile](https://numpy.org/doc/stable/reference/generated/numpy.percentile.html)
- [论文：Text Segmentation by Topic（语义边界检测）](https://aclanthology.org/J97-1003/)

## 与主核 / AI 的关系

「按相对阈值判定」与主核里的**自适应基数估计**同构：数据库也有「按当前数据分布动态决定阈值」的机制（直方图分桶、自适应采样率）。而两者都付出同一个代价——**同一个参数在不同数据上产生不同的绝对效果**。

而「硬约束降级为事后修正」在主核里也有对应：某些存储引擎会「尽力按页边界对齐，遇到超大记录就让那一页溢出」，而不是强行拆分记录。两者都是「宁可局部超限，也不破坏语义边界」。

对 agent 工程的直接价值有三条。其一，**相对阈值让参数可移植但不可比较**。`percentile=0.95` 在任何文档上都能用，但「切出了 7 块」这件事在另一份文档上没有可参照性——调参时要看的是块的质量而不是块的数量。其二，**块的偏移靠累加就必须保证拼接可还原**。这里为一句尾随空白专门写了修补代码，因为丢掉它会让后续所有块的 `split_idx_start` 与 `page_number` 静默偏错。其三，**「保证」与「倾向」要在文档里说清**。`max_length` 在这里是「尽量满足」而不是「一定满足」，失败时只打警告；一个依赖硬上限的下游必须自己再截断一次。
