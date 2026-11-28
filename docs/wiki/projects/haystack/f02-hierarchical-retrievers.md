---
project: haystack
feature: hierarchical-retrievers
title: 自动合并与句子窗口检索
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/retrievers/auto_merging_retriever.py
    symbols: [AutoMergingRetriever]
  - path: haystack/components/retrievers/sentence_window_retriever.py
    symbols: [SentenceWindowRetriever]
---

# 自动合并与句子窗口检索

## 一句话职责

两者都实现「小块检索、大块返回」，但走的机制完全不同：`AutoMergingRetriever` 沿**父子关系向上走**，当同一父节点下命中的叶子足够多（超过比例阈值）就把父节点**替换**进来；`SentenceWindowRetriever` 沿**同源文档的 `split_id` 数值窗口**左右扩展，把命中块的前后各 N 块一起取回。

## 机制是什么

| | `AutoMergingRetriever` | `SentenceWindowRetriever` |
|---|---|---|
| 依赖的元数据 | `__parent_id` / `__children_ids` / `__level` / `__block_size` | `source_id` / `split_id` |
| 数据来源 | **第二个 document store**（存父节点） | **同一个 document store** |
| 扩展方向 | 向上（父 → 祖父） | 左右（`split_id ± window_size`） |
| 触发条件 | **比例阈值**（命中数 / 子节点总数 > `threshold`） | 无条件扩展 |
| 输出 | 可能是**混合层级**的文档列表 | 每个命中块一个窗口 + 展平后的文档列表 |
| 输入要求 | **只能喂叶子节点** | 任意带 `source_id`/`split_id` 的块 |

**两者共用同一个前提：切分器写下的元数据。** 自动合并需要 `e02` 那页 `HierarchicalDocumentSplitter` 建的树；句子窗口需要 `e01` 那页 `DocumentSplitter` 写下的 `source_id` 与 `split_id`。**这就是 AGENTS.md 里「复用切分器写下的元数据键」这条规定的实际分量**——去掉那些键，这两个组件都无法工作。

## 怎么实现的

### 一、`AutoMergingRetriever`：双 store 与递归上升

**它的核心前提是一个部署要求：两个 document store。**

类的文档写得很明确：

> - 叶子文档在**上游检索器搜索的那个** document store 里；
> - 父文档在**传给 `AutoMergingRetriever` 的** document store 里。

而 `_check_valid_documents` 的错误消息把这条要求说成了操作指令：

```
AutoMergingRetriever only accepts leaf documents created by HierarchicalDocumentSplitter.
Write only the leaf documents to the document store searched by the upstream retriever,
and the root and parent documents to the document store passed to AutoMergingRetriever.
```

**为什么要分两个 store？** 因为若把根、父、叶都写进同一个被检索的 store，**上游检索器可能直接命中父节点或根节点** —— 那就没有「小块检索」的优势了（父节点内容太长，向量被平均化，匹配精度下降）。分两个 store 是用部署方式强制「只检索叶子」。

**校验的三条各有精确的理由。**

```
parentless_doc = next((doc for doc in matched_leaf_documents if not doc.meta.get("__parent_id")), None)
if parentless_doc is not None: raise ValueError("... do not have the required meta field '__parent_id' ...")
```

第一条第 `__parent_id` 判的是 `not doc.meta.get("__parent_id")` —— 也就是说**空字符串与缺失都被拒**。注释解释了它挡的是什么：「一个没有父的文档要么是根、要么不是 `HierarchicalDocumentSplitter` 建的，也就是说被检索的 store 里有多于叶子文档的东西」。

第二、第三条**只看键是否存在，不看值**：

```
# HierarchicalDocumentSplitter uses __level=0 and __block_size=0 for the root, so these
# fields must be checked for presence rather than truthiness.
if not all("__level" in doc.meta for doc in matched_leaf_documents): raise ValueError(...)
if not all("__block_size" in doc.meta for doc in matched_leaf_documents): raise ValueError(...)
```

**这条注释是这一页最值得记的一处。** `__level=0` 与 `__block_size=0` 对根节点是合法值——于是若用 `doc.meta.get("__level")` 做真值判断，根节点会被误判为「缺字段」。**必须用 `in` 而不是真值。**

而这三条校验的**顺序**也有含义：先查最能说明问题的那一个（`__parent_id` 缺失 → 你写错了 store），再查两个「协议字段」（缺失 → 切分器版本不对）。

**`_get_parent_doc` 的两条断言都是「不该发生」的。**

```
parent_docs = self.document_store.filter_documents({"field": "id", "operator": "==", "value": parent_id})
if len(parent_docs) != 1: raise ValueError(f"Expected 1 parent document with id {parent_id}, found {len(parent_docs)}")
parent_doc = parent_docs[0]
if not parent_doc.meta.get("__children_ids"): raise ValueError(f"Parent document with id {parent_id} does not have any children.")
```

第一条处理「父文档不在 store 里」（两个 store 没同步写入）。第二条处理「父文档存在但没有 `__children_ids`」（可能是旧版本切分器的产物，或根节点的特例）。

**第二条之所以重要**：`_try_merge_level` 会用 `len(parent_doc.meta["__children_ids"])` 当分母，若它为空则除零。这条守卫把 `ZeroDivisionError` 换成了一个说得清的错误。

**递归上升的核心是 `_try_merge_level`，而它先按层级分流。**

```
deepest_level = max((doc.meta.get("__level", 0) for doc in docs_to_merge), default=0)
pending_docs = []

for doc in docs_to_merge:
    # Shorter branches can have shallower leaves whose siblings have not been merged yet.
    if doc.meta.get("__level", 0) < deepest_level:
        pending_docs.append(doc)
    elif doc.meta.get("__parent_id"):
        parent_doc_id_to_child_docs[doc.meta["__parent_id"]].append(doc)
    else:
        docs_to_return.append(doc)
```

**三路分流，而第一路的注释说明了它的必要性：「较短的树枝会有更浅的叶子，它们的兄弟还没被合并」。**

具体情形：一棵树里深度不一致（某些分支因内容太短而少切了一层）。若这一轮只处理最深层的节点，浅层节点会被跳过——而浅层节点的父节点可能还有机会合并（若它的兄弟也命中了）。把它们放进 `pending_docs` 带到下一轮，能让它们与「被晋升上来的兄弟」一起参与。

**第二路只处理有 `__parent_id` 的**——没有父的（理论上不该有，因为入口校验过；但递归过程中「已经到根的」会走到这里）直接进结果。

**合并判据是一个比例。**

```
score = len(child_docs) / len(parent_doc.meta["__children_ids"])
if score > self.threshold: merged_docs.append(parent_doc)
else: docs_to_return.extend(child_docs)
```

**`>` 而不是 `>=`** —— 命中恰好等于阈值时不合并。而构造期校验是 `0 < threshold < 1`：

```
if not 0 < threshold < 1: raise ValueError("The threshold parameter must be between 0 and 1.")
```

**两端都开区间**——`threshold=0` 会让「命中 1 个就合并」（等于没有小块检索的优势），`threshold=1` 会让「必须命中全部才合并」（几乎永不合并）。两个极端都被排除。

**文档示例给出的场景很具体：**

> 因为它有 3 个子节点且 `threshold=0.5`，而我们检索到 2 个子节点（2/3 > 0.66）

**注意它合并后的分数是父文档自己的 `score`** —— 而父文档是从 store 里取出来的，它的 `score` 是 `None`（没经过检索打分的文档）。**这是一个信息损失**：合并后的文档失去了「命中得有多好」这个信息。而这也是为什么它的返回是「混合层级」的列表——叶子带分数、父节点不带。

**递归的继续条件是「这一轮有合并或有 pending」。**

```
if not merged_docs and not pending_docs: return docs_to_return
return _try_merge_level(pending_docs + merged_docs, docs_to_return)
```

**两处必须同时为空才停。** 而进入下一轮的输入是 `pending_docs + merged_docs` —— 也就是**被晋升的父节点会再次参与合并**（它可以被它的父节点吸收），这正是「递归上升」。

**终止的保证**：每一轮要么层数减一（因为合并到了一层更浅的节点），要么 pending 被消化。**层级是有限的**，所以递归必然终止。

### 二、`SentenceWindowRetriever`：一次查询、多窗口切分

**它的目标是「只查一次 document store」，做法是把所有窗口合并成一个 `OR` 过滤器。**

```
windows = self._get_windows(retrieved_documents)
filters = self._build_filters(windows, window_size)
fetched_documents = self.document_store.filter_documents(filters) if filters else []
return self._assemble_context(retrieved_documents, windows, window_size, fetched_documents)
```

**注意它不用 `retriever` 而是用 `filter_documents`** —— 因为它要的不是「相似文档」而是「满足 `split_id` 区间的同源文档」。用元数据过滤比再检索一次更精确也更便宜。

**`_get_windows` 对缺元数据的文档返回 `None`，而不是抛错。**

```
source_ids = [doc.meta.get(field) for field in self._source_id_meta_fields]
split_id = doc.meta.get(self.split_id_meta_field)
if any(source_id is None for source_id in source_ids) or split_id is None:
    logger.warning("Document {doc_id} is missing required metadata fields ...", ...)
    windows.append(None)
else:
    windows.append((source_ids, split_id))
```

**而 `_assemble_context` 对 `None` 窗口的处理是「原样返回这个文档」。**

```
if window is None:
    context_text.append(doc.content or "")
    context_documents.append(doc)
    continue
```

**于是一个缺元数据的文档拿到「没有扩展的窗口」而不是被丢弃** —— 这与 `raise_on_missing_meta_fields=False` 的文档说明一致（"will still include the original document in the results"）。

**而 `_raise_if_documents_do_not_have_expected_metadata` 在 `raise_on_missing_meta_fields=True`（默认）时先抛错**，所以上面那条路径只在显式关闭校验时才会走到。**两条路径并存是必需的**：默认严格（早失败），可选宽松（降级）。

**`source_id_meta_field` 支持列表，这一点的语义是同组判定。**

```
self._source_id_meta_fields = source_id_meta_field if isinstance(source_id_meta_field, list) else [source_id_meta_field]
```

文档写着「若提供多个字段，则**所有**字段都匹配才算同一来源」。而实现里用 `zip(..., strict=True)` 逐字段比较：

```
all(doc.meta.get(field) == source_id for field, source_id in zip(self._source_id_meta_fields, source_ids, strict=True))
```

**这个能力对应一个实际场景**：一个 store 里混了多个语料库，只靠 `source_id` 可能撞（不同库的同名文件）。加上 `source_id + version` 两个字段才唯一。

**`_build_filter_conditions` 把「窗口」表达成一个 AND 条件组。**

```
conditions = [
    {"field": f"meta.{self.split_id_meta_field}", "operator": ">=", "value": split_id - window_size},
    {"field": f"meta.{self.split_id_meta_field}", "operator": "<=", "value": split_id + window_size},
    *source_id_filters,
]
return {"operator": "AND", "conditions": conditions}
```

**注意字段名前缀 `meta.`** —— 这是 haystack 过滤器语法里「元数据字段」的标记。

**而 `_build_filters` 做了一次去重。**

```
if condition not in conditions: conditions.append(condition)
if not conditions: return None
return {"operator": "OR", "conditions": conditions}
```

**为什么需要去重？** 因为两个不同的命中块可能落在同一个窗口里（`split_id` 相邻且同源）——它们的过滤条件是**完全相同**的字典，重复加入会让 `OR` 列表里出现重复项。去重只能省一点查询负担（多数的 `OR` 实现已经幂等），但它是正确的做法。

**而 `if not conditions: return None` 让「所有文档都缺元数据」时不查 store** —— `run` 里也配合了 `if filters else []`。

**`_assemble_context` 把候选按窗口重新切分，而它靠 `_is_in_window` 逐个判断。**

```
context_docs = [fetched for fetched in fetched_documents if self._is_in_window(fetched, source_ids, split_id - window_size, split_id + window_size)]
context_docs_sorted = sorted(context_docs, key=lambda d: d.meta[self.split_id_meta_field])
context_text.append(self.merge_documents_text(context_docs_sorted))
context_documents.extend(context_docs_sorted)
```

**这里有一个值得记的性能选择**：候选是「所有窗口的并集」（一次查询的结果），然后**对每个命中块各扫一遍**做窗口切分。于是复杂度是 `O(命中数 × 候选数)`。

**为什么不按窗口分组一次？** 因为一个候选可能同时属于多个重叠的窗口——它必须被**每个**窗口各算一次（文档里写着 "Documents shared by overlapping windows appear once per window"）。所以「一次分组」不成立，逐个扫是对的。

**排序用 `d.meta[self.split_id_meta_field]` 直接索引而不是 `.get`** —— 因为 `_is_in_window` 已经保证了 `split_id` 非 `None`，所以此时必然存在。

**而 `context_documents` 是「所有窗口展平后首尾相接」** —— 于是**同一个文档可能出现多次**（若它落在两个窗口里）。这是有意的，文档里明确写了。

**`merge_documents_text` 是一个静态方法，可以直接当工具用。**

把它做成 `@staticmethod` 让「把窗口拼成一个字符串」这件事可以被外部单独调用（不必构造组件）。而它的实现大概是 `"\n".join` 之类。

### 三、两者的共同点与根本差别

**共同点：都要求上游检索器返回的是「带特定元数据的小块」，都用额外一次 store 访问换上下文。**

**差别在于「扩展的依据」**：

- 自动合并依据的是**结构关系**（`__parent_id`）——它是**语义正确**的扩展：父节点包含子节点的全部内容，所以「用父替换子」不引入任何无关文本。
- 句子窗口依据的是**序号相邻**（`split_id ± N`）——它是**启发式**的扩展：相邻块可能换主题了，于是可能引入无关文本。

于是两者的风险也不同：自动合并的风险是「合并后块太大」（父节点可能很长）、句子窗口的风险是「窗口里混进不相关内容」。

**而它们的成本结构相反**：自动合并可能做**多次** store 查询（每层每父一次），句子窗口**只做一次**（把所有窗口合并成一个 OR 查询）。

## 关键取舍

**源码事实：** `AutoMergingRetriever` 要求两个 document store（叶子在被检索的那个、父节点在传给它的那个），错误消息把它写成了操作指令；分 store 的目的是**用部署方式强制上游只检索叶子**。

**源码事实：** 入口校验对 `__level` / `__block_size` 用 `in` 而不是真值判断——因为 `HierarchicalDocumentSplitter` 对根节点写的就是 `__level=0` 与 `__block_size=0`，真值判断会误判为缺失。这条写在注释里。

**源码事实：** `_get_parent_doc` 校验「恰好一个父文档」与「父文档有 `__children_ids`」——后者防的是除零（合并判据用 `len(__children_ids)` 当分母）。

**源码事实：** `_try_merge_level` 按层级三路分流，其中「浅于最深层的进 `pending_docs`」是为了让**深度不一致的树枝**能在下一轮与晋升上来的兄弟一起合并。

**源码事实：** 合并判据是 `len(命中子节点) / len(全部子节点) > threshold`，用 `>` 而非 `>=`；`threshold` 构造期校验为开区间 `(0, 1)`。

**源码事实：** 合并后返回的是**父文档**，而父文档的 `score` 是 `None`（没经过检索打分）——所以输出是「混合层级 + 部分无分数」的列表。

**源码事实：** 递归继续条件是「这一轮有合并**或**有 pending」，进入下一轮的输入是 `pending_docs + merged_docs`（晋升的父节点会再次参与合并），层级有限所以必然终止。

**源码事实：** `SentenceWindowRetriever` 用 `filter_documents` 而不是再检索一次——它要的是「同源且 `split_id` 在区间内」，用元数据过滤更精确也更便宜。

**源码事实：** 它把所有窗口合并成一个 `OR` 过滤器，**只查一次** store；窗口条件内部是 `AND`（两个 `split_id` 边界 + 每个 `source_id` 字段）。

**源码事实：** `source_id_meta_field` 支持列表，语义是「所有字段都匹配才算同一来源」——用于区分多个语料库里的同名文件。

**源码事实：** 缺元数据的文档在 `raise_on_missing_meta_fields=False` 时**仍被保留在结果里**（只是没有扩展窗口），而不是被丢弃。

**源码事实：** `_assemble_context` 对每个命中块各扫一遍候选（`O(命中数 × 候选数)`），因为重叠窗口里的同一文档必须被每个窗口各算一次——「一次分组」不成立。

**架构推论：** 这两个组件是「检索粒度 ≠ 生成粒度」这条原则的两种实现，而它们的选择揭示了**一个关键判断：你手上有没有结构信息**。

有树（`__parent_id`）就用结构扩展，扩展是语义安全的，代价是多次查询与可能过大的块。只有顺序（`split_id`）就用数值窗口，一次查询搞定，代价是可能混进无关内容。**而两种元数据的可用性取决于切分器**——这正是 `e01`/`e02` 那两页写下的键在下游被消费的地方。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/retrievers/auto_merging_retriever.py` | `AutoMergingRetriever` | 沿父子关系向上合并，比例阈值 |
| `haystack/components/retrievers/auto_merging_retriever.py` | `_check_valid_documents` | 三条入口校验，`__level`/`__block_size` 只查键存在 |
| `haystack/components/retrievers/auto_merging_retriever.py` | `_get_parent_doc` | 取父文档，校验唯一性与非空子列表 |
| `haystack/components/retrievers/auto_merging_retriever.py` | `_try_merge_level` | 三路分流 + 比例判据 + 递归上升 |
| `haystack/components/retrievers/auto_merging_retriever.py` | `threshold` | 开区间 `(0,1)`，判据用 `>` |
| `haystack/components/retrievers/sentence_window_retriever.py` | `SentenceWindowRetriever` | 按 `split_id ± window_size` 左右扩展 |
| `haystack/components/retrievers/sentence_window_retriever.py` | `_get_windows` | 抽源 ID 与序号，缺失则 `None` |
| `haystack/components/retrievers/sentence_window_retriever.py` | `_build_filters` | 全部窗口合成一个 `OR`，去重，空则 `None` |
| `haystack/components/retrievers/sentence_window_retriever.py` | `_build_filter_conditions` | 单窗口的 `AND` 条件组，字段带 `meta.` 前缀 |
| `haystack/components/retrievers/sentence_window_retriever.py` | `_assemble_context` | 按窗口切分候选，重叠窗口各算一次 |
| `haystack/components/retrievers/sentence_window_retriever.py` | `_is_in_window` | 序号区间 + 全部源字段匹配 |
| `haystack/components/retrievers/sentence_window_retriever.py` | `merge_documents_text` | 静态方法，窗口文本拼接 |

## 参考

- [haystack 源码：auto_merging_retriever.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/retrievers/auto_merging_retriever.py)
- [haystack 源码：sentence_window_retriever.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/retrievers/sentence_window_retriever.py)
- [haystack 文档：自动合并检索](https://docs.haystack.deepset.ai/docs/automerge-retriever)
- [haystack 文档：句子窗口检索](https://docs.haystack.deepset.ai/docs/sentencewindowretriever)

## 与主核 / AI 的关系

这两个组件与主核里的**索引到数据的回表**是同一类操作：先用紧凑的索引条目定位，再按定位结果取更完整的记录。差别在于扩展方式——数据库靠精确的指针（页号 + 槽号），自动合并靠 `__parent_id` 引用（接近指针），句子窗口靠 `split_id ± N`（只有顺序，没有指针）。

**「用 `in` 而不是真值判断」这条在主核里也有对应**：判断一个字段是否 `NULL` 必须用 `IS NULL`，不能用 `= NULL` 或真值判断——因为 `0`、空串这些值在真值语义里都是假，而它们是合法的数据。**「缺失」与「假值」的区分是所有数据系统共有的坑**。

对 agent 工程的直接价值有三条。其一，**结构扩展与序号扩展的风险不同，选哪个取决于元数据**。有父子关系就向上走（语义安全），只有序号就开窗口（便宜但可能混入无关内容）——而这两类元数据都是切分器给的，所以「切分策略」决定了「能做什么样的检索增强」。其二，**比例阈值比绝对数量稳**。`命中数 / 子节点总数 > 0.5` 在子节点数变化时仍然有意义，而「命中 ≥ 2」在小父节点上会让不该合并的也合并。其三，**合并会丢分数**。父文档没经过检索打分，于是输出里有些文档带 `score`、有些是 `None`——下游若按分数排序或过滤，必须处理这个不齐。
