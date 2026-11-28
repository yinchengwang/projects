---
project: dify
feature: text-splitter
title: 递归字符切分与增强
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/splitter/fixed_text_splitter.py
    symbols: [FixedRecursiveCharacterTextSplitter, EnhanceRecursiveCharacterTextSplitter]
---

# 递归字符切分与增强

## 一句话职责

`FixedRecursiveCharacterTextSplitter` 先按一个**用户指定的固定分隔符**把文档切成段，只对**超长段**再做递归细分——于是「段落边界」优先于「大小均匀」。

## 机制是什么

切分器的基类 `TextSplitter`（同目录 `text_splitter.py`）定下三条约定：

1. 长度函数是**批量签名**：`Callable[[list[str]], list[int]]`，一次算一批，而不是一个。
2. 长度单位可替换：缺省是字符数，`from_huggingface_tokenizer` 可以换成 token 数。
3. `_merge_splits` 负责把小块合并到接近 `chunk_size`，并在超限时按 `chunk_overlap` 回退。

`RecursiveCharacterTextSplitter` 在此之上做**逐级降级**：给一串分隔符（缺省 `["\n\n", "\n", " ", ""]`），挑第一个在文本里出现的，切一遍；切出来的块仍超长就带上「剩下的分隔符」递归下去，直到分隔符用完（空串表示逐字符切）。

本页的两个类是在这条链上叠的两层改动：

- `EnhanceRecursiveCharacterTextSplitter`：只加一个类方法 `from_encoder`，把长度函数固定成「字符数」。
- `FixedRecursiveCharacterTextSplitter`：改用「先按固定分隔符分段、超长才递归」的切法，并自带一份与基类**语义不同的递归实现**。

所以真正的机制在第二层：**分段与分块解耦**。固定分隔符（通常是 `\n\n`）表达「这是一个语义段」，`chunk_size` 表达「这是嵌入模型能吃下的上限」。两者冲突时，前者优先。

## 怎么实现的

**`from_encoder` 是一个被掏空的方法。** 签名收了 `embedding_model_instance`、`allowed_special`、`disallowed_special` 三个参数，方法体里**一个都没用**——只定义 `_character_encoder`（`[len(text) for text in texts]`）然后 `cls(length_function=_character_encoder, **kwargs)`。

而基类的 `length_function` 缺省值本来就是 `lambda x: [len(x) for x in x]`。所以这个类方法在当前实现下**等价于直接构造**，三个参数纯属历史遗迹：早期版本显然是从嵌入模型取 tiktoken 编码来算 token 数，改成按字符计后参数留着没删。它的存在价值只剩下一个：**调用方不必再关心长度函数怎么传**。

类 docstring 写「Create recursive splitters that measure chunk limits in characters」，这句在基类缺省行为下是自明的——它是那次「从 token 改成字符」迁移留下的说明。

**固定分隔符先做了一次转义翻转。** 构造函数里：

```python
self._fixed_separator = fixed_separator.encode("raw_unicode_escape").decode("unicode_escape")
```

这是把 `\n\n` 这样的**两字符转义序列**（反斜杠 + n）还原成真正的换行。因为分隔符来自 API 请求体，前端传的是 JSON 字符串，到达时可能仍是转义形态。这一步让「用户填 `\n\n`」与「用户填真换行」等价。

缺省分隔符列表是 `["\n\n", "\n", "。", ". ", " ", ""]`——比基类多了 `。` 与 `. `，即**中英文句末都算一级边界**。

**`split_text` 只切一段，不合并。** 逻辑很短：按固定分隔符切，拿到所有段的长度（**一次批量调用长度函数**，这正是批量签名的用处），然后逐段判断——超 `chunk_size` 的交给 `recursive_split_text`，不超的原样保留。

关键点是**没有合并步骤**。三个各 100 字的短段落、`chunk_size=1000`，仍然产出三个片段，不会合成一个。这与 langchain 的 `RecursiveCharacterTextSplitter` 行为相反（那个总是尽量合并到接近上限）。**这是「固定分隔符」这个名字的真实语义：分隔符就是片段边界。**

**`recursive_split_text` 与基类 `_split_text` 有四处实质差异。**

| 处 | 基类 | 固定切分器 |
|---|---|---|
| 找分隔符 | `re.search(_s, text)`（正则） | `_s in text`（字面包含） |
| 空格处理 | 按分隔符原样切 | `separator == " "` 时 `re.split(r" +", text)` 折叠连续空格 |
| 空片段过滤 | 只丢 `""` | 分隔符是 `""`/`"\n"` 时只丢 `""`，否则**连 `"\n"` 一起丢** |
| 分隔符为空 | 走基类的 `_split_text_with_regex` + `_merge_splits` | 走一段手写的字符累积循环，自己维护 overlap |

第一处差异影响最大。基类把分隔符当正则，而缺省列表里有 `. `——在正则里 `.` 匹配任意字符，于是「任意字符 + 空格」都会成为切点。本类改成字面匹配，`. ` 就严格表示「句号加空格」。**这是修正，不是新增能力。**

第三处是清理：递归细分时如果分隔符是 `. ` 这类非换行符，切出来的片段可能残留纯换行串，直接丢掉比留一个空段落好。

第四处是空分隔符分支。基类在这一路上会调用 `_split_text_with_regex` 把文本拆成单字符，再交给 `_merge_splits` 按 `chunk_size` 与 `chunk_overlap` 合并。本类的手写循环用一个三段判断完成同样的事：

- 若「当前长度 + 新字符」仍不超过 `chunk_size - chunk_overlap`，直接追加；
- 否则若不超过 `chunk_size`，追加并**同时把它记进 overlap 缓冲**；
- 否则收束当前片段，新片段以「overlap 缓冲 + 当前字符」开头，缓冲清空。

即 overlap 不是靠事后回退切片算出来的，而是**在填充阶段就同步累积**。这比基类的「先填满再往回弹」更省一次遍历，代价是这个循环是独立实现、与基类行为未必逐字一致。

**有一处回退破坏了「字面匹配」的一致性。** `recursive_split_text` 里当前分隔符切不开时，调的是：

```python
other_info = self._split_text(s, new_separators)
```

`_split_text` 是**基类**的方法，它用 `re.search` 找下一级分隔符。所以第一层用字面匹配、第二层起又回到正则匹配。对 `. ` 这种在正则与字面下有歧义的分隔符，两者行为不同——这种「两层语义」是当前实现里最容易读漏的地方。

## 关键取舍

**源码事实：** `FixedRecursiveCharacterTextSplitter.split_text` 不合并相邻短段，固定分隔符即片段边界；只有超 `chunk_size` 的段才进 `recursive_split_text`。

**源码事实：** `recursive_split_text` 用字面 `in` 找分隔符（基类用 `re.search`），但对下一级递归调用的是基类的 `_split_text`，于是第二层起又变回正则。

**源码事实：** 分隔符为 `" "` 时用 `re.split(r" +", text)` 折叠连续空格；分隔符非 `""`/`"\n"` 时会把纯 `"\n"` 片段一并过滤掉。

**源码事实：** `_fixed_separator` 经 `encode("raw_unicode_escape").decode("unicode_escape")` 翻转，把 `\n\n` 这类转义序列还原成真实字符。

**源码事实：** `EnhanceRecursiveCharacterTextSplitter.from_encoder` 收了三个参数但一个都不用，只把长度函数设成字符数——与基类缺省行为等价。

**源码事实：** 长度函数是批量签名 `list[str] -> list[int]`，`split_text` 里一次性算出所有段的长度。

**架构推论：** 「分段优先、合并取消」这条规则把 `chunk_size` 从「目标大小」降级成「上限」。对 RAG 的直接后果是**片段数由段落数决定，而不是由字数决定**：一篇 `\n\n` 密集的文档会产出很多小片段，即使它们本可以合并。这个方向在语义上更干净（一段一个片段），在检索上则利弊参半——小片段召回的上下文不完整，大片段会稀释相似度。

**架构推论：** 长度函数改批量签名是为了省掉「每个段单独调一次」的开销。若长度函数要做 tokenize（`from_huggingface_tokenizer` 就是这样），批量调用还能让 tokenizer 内部一次编码多条，差别是数量级的。这也是为什么这个签名是**列表进列表出**而不是单值——它是为分词器设计的。

**架构推论：** `from_encoder` 变成空壳，说明长度单位从 token 切到字符是一次**产品决策**而非技术演进：按字符计不依赖具体嵌入模型，于是切分结果与「用哪个模型」解耦，代价是不同模型的实际 token 上限不同、字符数相同而 token 数可能差很多。参数留着不删，是让外部调用点不用改——典型的「签名冻结、实现清空」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/splitter/fixed_text_splitter.py` | `FixedRecursiveCharacterTextSplitter` | 固定分隔符优先的切分器 |
| `api/core/rag/splitter/fixed_text_splitter.py` | `FixedRecursiveCharacterTextSplitter.split_text` | 按固定分隔符分段，超长段转递归 |
| `api/core/rag/splitter/fixed_text_splitter.py` | `FixedRecursiveCharacterTextSplitter.recursive_split_text` | 字面匹配的递归细分与手写 overlap 累积 |
| `api/core/rag/splitter/fixed_text_splitter.py` | `EnhanceRecursiveCharacterTextSplitter` | 增强版基类，只加 `from_encoder` |
| `api/core/rag/splitter/fixed_text_splitter.py` | `EnhanceRecursiveCharacterTextSplitter.from_encoder` | 把长度函数固定成字符数（参数未使用） |
| `api/core/rag/splitter/text_splitter.py` | `TextSplitter` | 切分基类：批量长度函数、合并与回退 |
| `api/core/rag/splitter/text_splitter.py` | `TextSplitter._merge_splits` | 合并到接近上限并按 overlap 回弹 |
| `api/core/rag/splitter/text_splitter.py` | `TextSplitter._join_docs` | 拼接并 trim，空串返回 `None` |
| `api/core/rag/splitter/text_splitter.py` | `RecursiveCharacterTextSplitter` | 基类递归实现（正则找分隔符） |
| `api/core/rag/splitter/text_splitter.py` | `TextSplitter.create_documents` | 切分结果转 `Document`，可记 `start_index` |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/rag/splitter/fixed_text_splitter.py](https://github.com/langgenius/dify/blob/main/api/core/rag/splitter/fixed_text_splitter.py)
- [dify 文档：分段与清洗](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents/chunking-and-cleaning-text)
- [LangChain 文档：递归字符切分器](https://python.langchain.com/docs/concepts/text_splitters/)

## 与主核 / AI 的关系

「先按固定边界切、只对超限部分再细切」与主核里「先按页切、超长行再溢出到新页」是同一种策略：**优先保住语义/物理边界，尺寸作为上限而不是目标**。主核的溢出是为了满足页大小限制，这里的递归细分是为了满足嵌入模型的输入上限，两者都把「边界」放在「均匀」之前。

对 RAG 工程最实用的四条。其一：**`chunk_size` 在固定切分器里是上限不是目标**，所以调它不会让片段变大变小（除非降低到段落长度以下）；想控制片段数应该改「固定分隔符」，而不是改 `chunk_size`。其二：**缺省分隔符列表是 `\n\n`**，意味着 Markdown 与换行分段的文档切得自然，而一坨没有空行的长文本会整段进递归细分——此时行为才退回到类 langchain 的按大小切。其三：**`overlap` 只在递归细分那一路生效**，段落级切分没有重叠，所以「跨段落的语义衔接」这类问题不能靠调大 `chunk_overlap` 解决。其四：若你在别处复用这份代码，注意 **`recursive_split_text` 只在第一层是字面匹配**，深层递归走的是基类正则；对 `. ` 这类分隔符，两层语义不一致会导致结果与预期有细微偏差。
