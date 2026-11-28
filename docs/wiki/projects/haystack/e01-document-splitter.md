---
project: haystack
feature: document-splitter
title: 文档切分器
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/preprocessors/document_splitter.py
    symbols: [DocumentSplitter]
  - path: haystack/components/preprocessors/document_preprocessor.py
    symbols: [DocumentPreprocessor]
---

# 文档切分器

## 一句话职责

把长文档按「单位」（词/句/段/页/行/token/自定义函数）切成 `split_length` 大小的块、块间留 `split_overlap` 的重叠，**并且把「这块从哪来」写成元数据**——`source_id`、`page_number`、`split_id`、`split_idx_start`，以及双向的 `_split_overlap` 区间记录。

## 机制是什么

三件事被同一个组件承担，而它们的耦合程度决定了这个文件 631 行的结构：

| 关注点 | 实现位置 | 难度来源 |
|---|---|---|
| **切**（按什么单位切） | 四条路径：字符类 / NLTK 句 / tiktoken / 自定义函数 | 四种单位的「重叠」语义各不相同 |
| **合**（把单位攒成块） | `_concatenate_units` | 重叠、阈值、末尾冗余块三者的交互 |
| **溯源**（块与原文的位置关系） | `_create_docs_from_splits` + `_leading_page_breaks` | 分页符 `\f` 的归属问题 |

而 `DocumentPreprocessor` 是它 + `DocumentCleaner` 的 `@super_component` 封装：把所有参数平铺到一层，内部搭一个两节点管道（`splitter → cleaner`）并用 `input_mapping`/`output_mapping` 映射端口。

## 怎么实现的

### 一、四种切分路径：为什么必须是四条而不是一条

**`_split_document` 是一个四路分派，而分派的顺序决定了优先级。**

```
if self.split_by == "sentence" or self.respect_sentence_boundary: return self._split_by_nltk_sentence(doc)
if self.split_by == "function" and self.splitting_function is not None: return self._split_by_function(doc)
if self.split_by == "token": return self._split_by_token(doc)
return self._split_by_character(doc)
```

**第一条把两种配置合并了**：`split_by="sentence"` 与「`split_by="word"` 且 `respect_sentence_boundary=True`」都走 NLTK 句路径。这个合并是必要的——两者都需要句子边界，只是「攒成块时数句还是数词」不同（见下）。

**字符类路径的分隔符来自一张映射表。**

```
_CHARACTER_SPLIT_BY_MAPPING = {"page": "\f", "passage": "\n\n", "period": ".", "word": " ", "line": "\n"}
```

`function` 与 `sentence` 不在这张表里，因为它们不按字符切。

**而 `_split_by_character` 做了一件容易被忽略的事：把分隔符加回每个单位的末尾（最后一个除外）。**

```
units = doc.content.split(split_at)
for i in range(len(units) - 1):
    units[i] += split_at
```

**不这么做的话，把所有块拼回去得不到原文**——分隔符会被全部吞掉。而「最后一个不加」是因为原文末尾没有分隔符（`split` 的最后一个元素是分隔符之后的部分）。

这个「先把分隔符贴回去、之后再按字符数累加偏移」的做法让 `split_idx_start` 能直接是「已处理单位的字符长度累加」，不必重新在原文里搜索。

**token 路径则完全不同：先把整篇编码，再按 token 索引切片，最后解码。**

```
tokens = self._tiktoken_tokenizer.encode_ordinary(doc.content)
full_text, offsets = self._tiktoken_tokenizer.decode_with_offsets(tokens)
offsets.append(len(full_text))
```

**`decode_with_offsets` 是关键**：它给出「第 i 个 token 在解码文本里的起始字符位置」，于是 `chunk_start = offsets[i]`、`chunk_end = offsets[i + chunk_token_count]` 就能拿到精确的字符区间。

这解决了 token 切分的一个固有难题：**按 token 数切与按字符数报位置是两个坐标系**。`offsets` 就是那个转换表。

**而这段代码里有一个针对「末尾冗余块」的专门处理。**

```
if i + chunk_token_count == len(tokens): break
```

没有这个 `break`，当 `step = split_length - split_overlap` 小于 `split_length` 时，最后一次窗口会完全落在已覆盖的 token 里——**产出一个内容完全是上一块后缀的块**。注释在 `_concatenate_units` 里把这个修复称为「与 `_split_by_token` 的 overlap-only trailing chunk fix 同源」。

### 二、`_concatenate_units`：重叠、阈值、冗余三者的裁决

**这是字符类与句子类路径的共同核心，而它的实现里有四个互相制约的机制。**

**机制一：`windowed(elements, n=split_length, step=step)`。** 用滑动窗口生成「单位组」，`step = split_length - split_overlap`。

**机制二：跳过「不带来新文本」的窗口。**

```
new_units = current_units[max(0, covered_unit_count - seg_index * step):]
if split_overlap > 0 and text_splits and not "".join(new_units): continue
covered_unit_count = max(covered_unit_count, seg_index * step + len(current_units))
```

`covered_unit_count` 记录「已被之前窗口覆盖的单位数」。第 `seg_index` 个窗口从单位 `seg_index * step` 开始，所以「这个窗口里第一个未被覆盖的单位」下标是 `covered_unit_count - seg_index * step`。

若这些新单位的拼接是空字符串，就**跳过这个窗口**——否则会产出一个完全包含在前一块里的块。

**这一段就是重叠机制与「不重复产出」的冲突裁决点**。重叠必然会让窗口有一部分落在已覆盖区域，而滑到最后时可能**全部**落在已覆盖区域。

**机制三：低于阈值的段并入上一块，但只并入「重叠之外」的部分。**

```
if len(current_units) < split_threshold and len(text_splits) > 0:
    text_splits[-1] += "".join(current_units[split_overlap:])
```

**`current_units[split_overlap:]` 这个切片是本页最值得记的一行。** 注释解释了原因：这个段的前 `split_overlap` 个单位**已经是上一块的一部分**（那就是重叠的含义），若整段拼上去，重叠文本会被重复一次，**合并后的块就不再是原文的连续子串**。

也就是说：合并时要扣掉重叠，否则「块是原文的子串」这个不变量被破坏。

**机制四：页号随「已处理单位」推进，而「已处理」明确定义为前 `split_length - split_overlap` 个。**

```
processed_units = current_units[: split_length - split_overlap]
cur_start_idx += len("".join(processed_units))
if self.split_by == "page":
    num_page_breaks = len(processed_units)
else:
    num_page_breaks = sum(processed_unit.count("\f") for processed_unit in processed_units)
```

两个要点：

1. **页号推进只看 `step` 个单位，不看整个窗口**——因为下一个窗口正是从那里开始。用整个窗口长度会重复计数。
2. **`split_by == "page"` 时一个单位就是一个分页符**，所以数单位个数而不是数 `\f`。这个特判必需：`units` 里每个都已经被贴回了 `\f`，数 `count("\f")` 会得到相同结果，但直接用 `len()` 更准确（且避免了「原文里本来就有额外 `\f`」的干扰）。

### 三、`page_number` 的归属规则：一个独立模块专门定义它

**所有切分器共用 `_page_numbers.py` 里的 `_leading_page_breaks`，而那个模块的文档把规则写得很清楚。**

> 切分器把块来自的页记在 `page_number` 里，通过数源文档里的分页符（form feed，`\f`）。**报告的页是「块的第一个非分页符字符」所在的那一页**：跨页的块属于它起始的那一页，而块开头的分页符属于这个块自己的页，而不是它被切出来的那一页。

**这是一个「空洞但必要」的规则，而 `_leading_page_breaks` 的实现解释了为什么需要它。**

```
offset = 0
while text.startswith(page_break_character, offset): offset += len(page_break_character)
if offset >= len(text): return 0        # 全是分页符 = 一个空页，留在起始页
return offset // len(page_break_character)
```

函数返回「这个块开头连续有几个分页符」，然后 `page_number = splits_pages[i] + _leading_page_breaks(txt)`。

**要解决的具体问题：** 当 `split_by="page"` 时，切出来的每个单位是「页内容 + 末尾的 `\f`」——于是**下一个块总是以 `\f` 开头**。若不修正，第二块的 `page_number` 会是 1（它被切出来的页），而它实际内容在第 2 页。

**而「全是分页符」的那个特例是专门为 `split_by="page"` 的空白页准备的。** 一个只含 `\f` 的块代表一个空页，它**属于它起始的那一页**而不是下一页。注释里明写了「`split_by="page"` 会为每个空白页产出这样的块」。

这个规则的正确性依赖一件事：**`_leading_page_breaks` 只看开头**。所以「块中间的 `\f`」不影响归属——那正是「跨页的块属于它起始的那一页」。

### 四、元数据的四个键与一个特殊结构

**`_create_docs_from_splits` 是唯一产出 `Document` 的地方，它写四个键。**

| 键 | 值 | 用途 |
|---|---|---|
| `source_id` | 原文档的 `doc.id` | 追到源文档 |
| `page_number` | 起始页 + 开头分页符数 | 定位到页 |
| `split_id` | 块在本文档内的序号 | 块在原文里的顺序 |
| `split_idx_start` | 块首字符在原文的偏移 | **精确定位到字符** |

**后两个键的组合是「可重建原文」的关键**：`split_id` 给顺序、`split_idx_start` 给位置、`content` 给文本。三者合起来足以把块还原成原文的片段。

**而 `_split_overlap` 是一个双向的列表结构。**

```
doc.meta["_split_overlap"] = []
...
overlapping_range = (current_doc_start_idx - previous_doc_start_idx, len(previous_doc.content))
if overlapping_range[0] < overlapping_range[1]:
    overlapping_str = previous_doc.content[overlapping_range[0]:overlapping_range[1]]
    if current_doc.content.startswith(overlapping_str):
        current_doc.meta["_split_overlap"].append({"doc_id": previous_doc.id, "range": overlapping_range})
        overlapping_range = (0, overlapping_range[1] - overlapping_range[0])
        previous_doc.meta["_split_overlap"].append({"doc_id": current_doc.id, "range": overlapping_range})
```

四层判断，每层都有一个理由：

1. **`overlapping_range[0] < overlapping_range[1]`** ——起点必须在前一块内容范围内，否则没有重叠。
2. **`current_doc.content.startswith(overlapping_str)`** ——**必须实际验证重叠文本真的是当前块的开头**。这是一个校验而非推断：某些切分路径（比如 `_split_by_function` 里函数改写了文本）可能让偏移与内容不对应，此时不写这条记录比写一条错的记录好。
3. **双向记录**：前一块也记一条（指向当前块、range 从 0 到重叠长度）。于是**任意一块都能查出它和前后邻居的重叠量**——这对「按块去重」或「按块拼接」都是必需的。
4. **range 用字符下标而非字符串**——存储体积小，且不依赖内容不变。

**未被写进元数据的只有一点：不重叠时 `_split_overlap` 是空列表而不是缺失。** 这是为了让下游不必判 `None`（但它只在 `split_overlap > 0` 时才被创建——`split_overlap <= 0` 时这个键根本不存在）。

### 五、`respect_sentence_boundary`：一个完全独立的攒块算法

**当 `split_by="word"` 且 `respect_sentence_boundary=True` 时，块是按「词数」攒的，但攒的单位是句子。这需要另一套算法。**

`_concatenate_sentences_based_on_word_amount` 的主循环逻辑是：

```
current_chunk.append(sentence)
chunk_word_count += len(sentence.split())
next_sentence_word_count = len(sentences[sentence_idx + 1].split()) if 还有下一句 else 0

if (chunk_word_count + next_sentence_word_count) > split_length or sentence_idx == len(sentences) - 1:
    # 收块，然后算要保留几句给下一块
```

**判据是「当前累计 + 下一句」是否超限**，而不是「当前累计」——这样**不会把一个句子拆开**（宁可提前收块）。

**而保留几句给下一块由 `_number_of_sentences_to_keep` 决定，它有两个停止条件。**

```
for sent in reversed(sentences[1:]):
    num_words += len(sent.split())
    if num_words > overlap_word_budget: break
    num_sentences_to_keep += 1
    if num_words >= split_overlap: break
```

- **`sentences[1:]` 而不是 `sentences`** —— 注释写着「下一个重叠文档不应与上一个完全相同，所以我们跳过第一句」。于是**重叠永远至少跳过一句**，避免了「下一块开头与上一块开头完全一样」。
- **两个 break**：超预算就停、够了重叠量也停。而 `overlap_word_budget = split_length - next_sentence_word_count` —— 也就是说预算是「块大小减去下一句要占的空间」，因为下一块的第一句已经确定了。

**这个 algo 与 `_concatenate_units` 的差别是本质的**：后者按固定步长滑窗（重叠是精确的 `split_overlap` 个单位），前者按句子边界贪心（重叠是**近似**的 `split_overlap` 个词，因为粒度是句子）。

这就是为什么两条路径不能合并——**一个是「精确数量、可断句」，另一个是「不断句、近似数量」**，两者不可兼得。

### 六、`_split_by_function`：最难得的一部分

**自定义函数切分的问题在于「函数可能改写文本」，于是「块在原文的位置」不再显然。**

```
found_idx = content.find(split, cur_start_idx)
if found_idx == -1 and text_splits:
    found_idx = content.find(split, prev_start_idx + 1)
```

**两级搜索 + 一个回退**：

1. 从当前位置向后找（正常情况）。
2. 找不到就**从上一块起点之后重找** —— 注释解释了原因：某些切分函数会产出**滑动窗口式的重叠块**，当前块可能开始于上一块的起点之后但在搜索位置之前。只做「向前搜索」会把这种情况误判为「找不到」。
3. 仍然找不到就**沿用累加的偏移**（`cur_start_idx`），并且此时页号靠 `split.count("\f")` 自己推算。

注释明确承认了这个前提：**「切分函数可能变换文本，所以块不保证原样出现在原文里」**。这是这个组件里唯一一处「溯源是尽力而为」的地方——其它三条路径都能给出精确偏移。

### 七、`DocumentPreprocessor`：把两个组件压成一层

**它用 `@super_component` 装饰器，把所有子组件参数平铺到自己的一层构造函数里。**

```
pp = Pipeline()
pp.add_component("splitter", splitter)
pp.add_component("cleaner", cleaner)
pp.connect("splitter.documents", "cleaner.documents")
self.pipeline = pp
self.input_mapping = {"documents": ["splitter.documents"]}
self.output_mapping = {"cleaner.documents": "documents"}
```

**注意顺序：先 split 再 clean**（类的文档第一句就写了 "first splits and then cleans"）。这个顺序不是随意的——**先切再清让 cleaner 面对更小的文本**（`remove_repeated_substrings` 要在页级找重复的页眉页脚，切完之后反而更准），但也意味着 cleaner 改动内容后**`split_idx_start` 与内容的对应关系被破坏**。

这是这个封装的一个内在张力：**切分器精心维护的偏移，在清洗之后不再成立**。

而 `min_content_length`（cleaner 的参数）与 `split_threshold`（splitter 的参数）在功能上重叠——前者按字符丢太短的块、后者按单位并入上一块。

**`if TYPE_CHECKING:` 里有一对「假方法」**：

```
def run(self, *, documents: list[Document]) -> dict[str, list[Document]]: ...
def warm_up(self) -> None: ...
```

文档注释写着「fake method, never executed, but static analyzers will not complain about missing method」。也就是说 `@super_component` 在运行期会动态生成 `run`，但类型检查器看不到——于是手工声明一份签名。

代价是：**这份声明可能与真实生成的签名漂移**，而漂移不会被任何工具发现。

## 关键取舍

**源码事实：** 切分有四条独立路径（字符类 / NLTK 句 / tiktoken / 自定义函数），分派顺序里第一条把 `split_by="sentence"` 与「`split_by="word"` + `respect_sentence_boundary`」合并。

**源码事实：** 字符类路径把分隔符贴回每个单位末尾（最后一个除外），否则块拼不回原文；也让偏移能靠长度累加而不是重新搜索。

**源码事实：** token 路径用 `decode_with_offsets` 得到「token 索引 → 字符位置」的转换表，于是按 token 切的同时能报精确字符偏移。

**源码事实：** `_concatenate_units` 里「跳过不带来新文本的窗口」与「低于阈值并入上一块时只拼 `current_units[split_overlap:]`」是两个互补的裁决：前者防止产出完全包含在前块里的块，后者防止合并时重复重叠文本、破坏「块是原文连续子串」的不变量。

**源码事实：** 页号推进只看前 `split_length - split_overlap` 个单位（`step` 个），因为下一窗口正从那里开始。

**源码事实：** `page_number` 的归属规则（块的第一个非分页符字符所在页、开头分页符算块自己的页、全分页符块留在起始页）被抽到独立模块 `_page_numbers.py` 里由所有切分器共用。

**源码事实：** 元数据写四个键：`source_id` / `page_number` / `split_id` / `split_idx_start`；后两者的组合足以把块还原到原文位置。

**源码事实：** `_split_overlap` 是**双向**记录（当前块指向上一块、上一块指向当前块），且写入前用 `current_doc.content.startswith(overlapping_str)` **验证重叠文本真的在当前块开头**——验证失败就不写。

**源码事实：** `_split_overlap` 只在 `split_overlap > 0` 时被创建，否则这个键不存在（下游要区分「空列表」与「键缺失」）。

**源码事实：** `respect_sentence_boundary` 走独立算法：判据是「当前累计 + 下一句」是否超限（避免断句），保留句数从 `sentences[1:]` 起算（保证下一块开头不与上一块完全相同），且有两个 break（预算与重叠量）。

**源码事实：** `_split_by_function` 是唯一「溯源尽力而为」的路径：函数可能改写文本，于是用「从当前位置找 → 从上一块起点重找 → 沿用累加偏移」三级回退，注释明确承认这个前提。

**源码事实：** `DocumentPreprocessor` 是 `splitter → cleaner` 的 `@super_component`，用 `input_mapping`/`output_mapping` 映射端口；`if TYPE_CHECKING` 里声明了 `run`/`warm_up` 的假签名给静态分析器看。

**架构推论：** 这个组件的复杂度几乎全部来自**「块必须可还原」这个不变量**。切分本身不难（按字符切、按句切都简单），难的是切完之后仍然能回答「这块从哪里来、和相邻块重叠多少」。

而那个不变量与每一个机制都冲突：重叠破坏「块是原文子串」（所以要扣掉重叠再合并）、分页符破坏「块的起始页」（所以要 `_leading_page_breaks`）、清洗破坏「偏移对应内容」（所以 `DocumentPreprocessor` 里有这个内在张力）。**每加一个便利功能，就得多一条修正规则。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/preprocessors/document_splitter.py` | `DocumentSplitter` | 切分主体：四条路径 + 攒块 + 元数据 |
| `haystack/components/preprocessors/document_splitter.py` | `_CHARACTER_SPLIT_BY_MAPPING` | `split_by` 到字符分隔符的映射 |
| `haystack/components/preprocessors/document_splitter.py` | `_concatenate_units` | 滑窗攒块：重叠、阈值、冗余窗口三重裁决 |
| `haystack/components/preprocessors/document_splitter.py` | `_concatenate_sentences_based_on_word_amount` | 不断句的攒块算法 |
| `haystack/components/preprocessors/document_splitter.py` | `_number_of_sentences_to_keep` | 重叠句数：从 `sentences[1:]` 起算、双 break |
| `haystack/components/preprocessors/document_splitter.py` | `_split_by_token` | tiktoken 路径，`decode_with_offsets` 做坐标转换 |
| `haystack/components/preprocessors/document_splitter.py` | `_split_by_function` | 自定义函数路径，三级偏移回退 |
| `haystack/components/preprocessors/document_splitter.py` | `_create_docs_from_splits` | 唯一产出 `Document` 处，写四个元数据键 |
| `haystack/components/preprocessors/document_splitter.py` | `_add_split_overlap_information` | 双向重叠记录 + 内容验证 |
| `haystack/components/preprocessors/_page_numbers.py` | `_leading_page_breaks` | 分页符归属规则，被所有切分器共用 |
| `haystack/components/preprocessors/document_preprocessor.py` | `DocumentPreprocessor` | `splitter → cleaner` 的 super component 封装 |

## 参考

- [haystack 源码：document_splitter.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/preprocessors/document_splitter.py)
- [haystack 文档：DocumentSplitter](https://docs.haystack.deepset.ai/docs/documentsplitter)
- [haystack 文档：预处理](https://docs.haystack.deepset.ai/docs/preprocessors)
- [Python 文档：itertools 与滑动窗口](https://docs.python.org/3/library/itertools.html)

## 与主核 / AI 的关系

切分与主核里的**页管理与记录边界**是同一类问题：两者都要把一个整体切成固定预算的片段，并让每片都能追溯回原处。主核用（页号，槽内偏移）两元组定位；这里用（`source_id`，`split_idx_start`）加 `split_id` 定位——**结构完全同构**。

而 `_split_overlap` 那个双向记录在主核里没有对应物：数据库的页不重叠，所以不需要「这块与邻居共享多少字节」。RAG 的块需要重叠，因为下游是按「一段连续文本」理解的，块边界处语义会断——于是重叠量本身成了必须记录的信息（否则按块拼接会重复）。

**「块是原文连续子串」这个不变量在主核里对应的是「页内容与文件偏移一一对应」**。两者都被各种便利功能威胁，都需要专门代码维持。`_concatenate_units` 里那个 `current_units[split_overlap:]` 切片，本质上就是在合并时把重叠扣掉以恢复这个不变量。

对 agent 工程的直接价值有三条。其一，**切分元数据是下游能力的前提，不是可选的日志**。`source_id` 让「返回来源」成为可能，`split_idx_start` 让「在原文里高亮」成为可能，`_split_overlap` 让「合并相邻块」成为可能——缺任一键，对应的下游功能就做不了。其二，**重叠与「可还原」是冲突的，冲突点必须显式裁决**。合并时扣掉重叠、跳过冗余窗口、末尾加 `break`，三处都是同一个不变量的守卫。其三，**自定义函数的溯源只能是尽力而为**。「三段回退 + 沿用累加偏移」这个设计说明作者清楚它不可靠——而承认不可靠（在注释里写明）比假装精确好。
