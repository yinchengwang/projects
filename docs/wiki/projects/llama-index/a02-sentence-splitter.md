---
project: llama-index
feature: sentence-splitter
title: 句子切分与窗口
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/node_parser/text/sentence.py
    symbols: [SentenceSplitter, _Split]
  - path: llama-index-core/llama_index/core/node_parser/text/sentence_window.py
    symbols: [SentenceWindowNodeParser]
---

# 句子切分与窗口

## 一句话职责

按「段落 → 句子 → 分句 → 词 → 字符」的降级顺序把文本切成尽量完整句子的片段，再按 token 预算合并成块并用重叠保证跨块衔接；同时提供「小块检索、大窗口喂给模型」的句子窗口机制。

## 机制是什么

这是 llama-index 最常用的切分器，也是 RAG 里最容易被低估的一层。它的核心矛盾是：**按 token 数切能精确控制长度但会切断句子；按句子切能保住语义但长度不可控**。

`SentenceSplitter` 的答案是**双阶段**：先按语义边界降级切分成「片段」（每个片段带 `is_sentence` 标记），再按 token 预算把这些片段**合并**成块，合并时允许跨块重叠。

`sentence_window.py` 走得更远：它切出极小的句子级节点作为检索单位（检索精度高），但在**喂给模型时把每条命中节点的前后各 N 句一起取来**（上下文完整）。检索单位与生成单位被故意分开。

## 怎么实现的

**双阶段的具体形态。** `_split()` 负责降级切分，`_merge()` 负责合并，`_split_text()` 串起来并包了一个 `CHUNKING` 回调事件。

**降级切分有两组函数。** `_split_fns` 是「看起来像句子」的一组：先按段落分隔符（默认 `\n\n\n`）切，再按句子分词器切。`_sub_sentence_split_fns` 是「句子切不动时的下位替代」：先按次级正则（默认 `[^,.;。？！]+[,.;。？！]?|[,.;。？！]`，注意包含了中文标点），再按空格，最后按字符。

`_get_splits_by_fns` 的实现有个关键细节：**每一组里只要某个函数切出了多于一个片段就立即返回**，并且第一组命中就标记 `is_sentence=True`。所以「这句话是不是完整句子」这个标记来自**哪一组切成功的**，而不是内容本身的性质。

**合并阶段有一个反直觉的保护。** `_split()` 里有一段专门处理「切不动的情况」：如果某个片段 token 数仍超预算，且它已经是唯一片段（`len(text_splits_by_fns) == 1`），就**保留它为一个超长片段**而不是递归。注释把理由写得很具体：那可能是「小 chunk_size 配上多 token 的 CJK/emoji 单字符」，递归会无限循环直到 `RecursionError`。保留后由 `_merge` 抛出意图明确的 `ValueError("Single token exceeded chunk size")`——**把崩溃换成可读的错误**。

**合并循环有四个状态。** `cur_chunk` 是「当前块已有的（文本, 长度）列表」，`last_chunk` 是「上一个已关闭的块」，`new_chunk` 标记「当前块是否还是空的」，`cur_chunk_len` 是累计长度。

`close_chunk()` 里做了一件容易被误解的事：它把 `cur_chunk` 移给 `last_chunk`，然后**从 `last_chunk` 的尾部往前取片段塞进新的空 `cur_chunk`，直到累计长度逼近 `chunk_overlap`**。所以重叠不是「复制上一块的末尾文本」，而是「复用上一块的末尾片段作为下一块的开头」。这意味着**实际重叠量小于等于 `chunk_overlap`**（按片段粒度取整），而不是精确等于。

主循环的判断顺序值得逐条读：

- 当前片段本身超预算 → 直接抛错（超长片段无法再细分了）。
- 「加入会超预算 **且** 当前块非空」→ 关闭当前块。
- 否则进入 else 分支，其中又有一层：**如果这是新块（带重叠）而加入会超预算，就从块头部不断弹出片段腾空间**。这一段是重叠与预算冲突时的裁决——重叠让位给预算。
- 最后还有一层三选一：`is_sentence` 或「加入后不超预算」或「这是新块」→ 加入；否则关闭块。「这是新块就至少加一个」保证了空块不会因预算不足而死循环。

**末尾处理与清洗。** 循环结束后若 `new_chunk` 为假（说明还有未关闭的块）就收尾追加。`_postprocess_chunks` 做两件事：丢弃纯空白块、strip 每块首尾空白。**strip 会让块的字符偏移与原文不再一一对应**——如果下游需要原文位置，这一步是信息损失点。

**元数据感知切分。** `MetadataAwareTextSplitter` 多一个 `split_text_metadata_aware(text, metadata_str)`：先算元数据的 token 数，**从 `chunk_size` 里扣掉**得到有效预算。这里有两道防线：有效预算 ≤ 0 抛 `ValueError`（元数据比块预算还长）；有效预算 < 50 打一条 print 警告（块会过短）。注意警告用的是 `print` 而非 `logging`，且 rejection 理由是「块太短会伤害检索质量」。

**句子窗口的三个字段。** `SentenceWindowNodeParser` 切出句子级节点，并给每个节点写入 `window` 元数据（前后各 `window_size` 句），另有 `original_text` 之类用于回溯。检索命中后由一个后处理器（`MetadataReplacementPostProcessor`）把节点的 `text` 换成窗口文本——**检索用短文本、生成用长文本**这条分离在检索之后才生效。

## 关键取舍

**源码事实：** 切分是「两组函数、组内先成功者胜」。`is_sentence` 标记来自**哪一组**切成功，不是内容判定。

**源码事实：** 遇到不可再分的超长片段时保留并抛 `ValueError`，而不是递归；注释明确写出递归会 `RecursionError`（CJK/emoji 单字符场景）。

**源码事实：** 重叠实现为「复用上一块的末尾片段」，其代价是实际重叠量按片段粒度取整、可能小于配置值。

**源码事实：** 合并时若新块带重叠且加入会超预算，会**从块头部弹出片段腾空间**——预算优先于重叠。

**源码事实：** `_postprocess_chunks` 会 strip 每块，块的字符串不再与原文偏移对齐。

**源码事实：** `split_text_metadata_aware` 从块预算里扣掉元数据长度，有效预算 ≤ 0 抛错、< 50 打 print 警告（用 print 而非日志）。

**架构推论：** 双阶段设计说明「长度约束」与「语义完整」是**两个可以分别优化的目标**：先按语义切碎（保证不许有切断的句子进入下一步），再按长度合并（保证块尺寸）。单阶段方案必然在其中一侧妥协。

**架构推论：** 句子窗口揭示了 RAG 的一个本质张力：**检索精度与生成上下文长度反相关**。小块匹配更准（噪声少），但模型需要更多上下文才能答对。把两者拆成「检索单位」与「生成单位」，是唯一不需要二选一的做法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/node_parser/text/sentence.py` | `SentenceSplitter` | 双阶段切分主体 |
| `llama-index-core/llama_index/core/node_parser/text/sentence.py` | `_Split` | 片段（文本 + 是否完整句 + token 数） |
| `llama-index-core/llama_index/core/node_parser/text/sentence.py` | `CHUNKING_REGEX` | 次级切分正则（含中文标点） |
| `llama-index-core/llama_index/core/node_parser/text/sentence.py` | `DEFAULT_PARAGRAPH_SEP` | 段落分隔符默认值 |
| `llama-index-core/llama_index/core/node_parser/text/sentence_window.py` | `SentenceWindowNodeParser` | 句子级节点 + 窗口元数据 |
| `llama-index-core/llama_index/core/node_parser/interface.py` | `MetadataAwareTextSplitter` | 元数据感知的预算扣减 |
| `llama-index-core/llama_index/core/node_parser/text/utils.py` | `split_by_sep` | 四级降级切分的基础函数 |

## 参考

- [llama-index 源码仓库](https://github.com/run-llama/llama_index)
- [llama-index 文档：节点与切分器](https://docs.llamaindex.ai/en/stable/module_guides/loading/node_parsers/)
- [llama-index 文档：句子窗口检索](https://docs.llamaindex.ai/en/stable/examples/node_postprocessor/MetadataReplacementDemo/)
- [llama-index 源码：sentence.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/text/sentence.py)

## 与主核 / AI 的关系

分块与主核（多模态数据库内核）的**页管理与记录边界**有一个直接可比的点：两者都在处理「把一个整体切成有固定预算的片段」。

主核的页是**固定大小**的（通常是 8KB），切法是机械的：记录跨页时靠指针链起来。RAG 的块是**可变大小**的（按 token 预算），切法是语义的：必须尽量不切断句子。这个差别来自「片段能否被重新解释」——数据库记录可以靠指针与偏移任意跨页，而模型只能读连续的文本。

**重叠机制**在主核里没有对应物。WAL 的连续段不需要重叠，因为读取方知道确切的起止位置；文本块需要重叠，因为下游是按「一段连续文本」来理解的，块边界处的语义会断。重叠是**为读者服务的冗余**，不是为存储完整性服务的。

**句子窗口**这条「检索单位 ≠ 生成单位」的分离，在主核里对应的是「索引条目 ≠ 数据页」。数据库的 B-tree 条目指向数据位置，检索时用条目（小），读取时用页（大）。RAG 的句子窗口是同一个结构：用句子做索引键，用窗口做读取单位。

`_postprocess_chunks` 的 strip 值得单独记：它是**信息损失点**。主核里对应的是「读出后重新组装」，任何 format 转换都可能引入这类损失。判断准则是一致的——**只要下游需要精确位置，就不该做 normalize**。
