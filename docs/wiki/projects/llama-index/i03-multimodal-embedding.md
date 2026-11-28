---
project: llama-index
feature: multimodal-embedding
title: 多模态嵌入与稀疏嵌入
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/embeddings/multi_modal_base.py
    symbols: [MultiModalEmbedding]
  - path: llama-index-core/llama_index/core/base/embeddings/base_sparse.py
    symbols: [BaseSparseEmbedding, sparse_similarity]
  - path: llama-index-core/llama_index/core/embeddings/pooling.py
    symbols: [Pooling]
---

# 多模态嵌入与稀疏嵌入

## 一句话职责

在稠密嵌入之外补两种向量形态：**图像嵌入**（把图片当成另一种输入，走完全平行的一套方法）与**稀疏嵌入**（用「下标 → 权重」的字典表示向量，检索走倒排而不是全维点积），再加一个小到只有一个枚举的 `Pooling`——它决定「一层 token 向量怎么压成一个句向量」，而这个决定直接影响嵌入质量。

## 机制是什么

`MultiModalEmbedding(BaseEmbedding)` 继承 i01 那套稠密嵌入，只加两个 `@abstractmethod`：`_get_image_embedding(img_file_path)` 与 `_aget_image_embedding(...)`，以及围绕它们的公开方法 `get_image_embedding` / `aget_image_embedding` / `_get_image_embeddings` / `_aget_image_embeddings` / `get_image_embedding_batch` / `aget_image_embedding_batch`。

所以这一层不是「新抽象」，而是**把 i01 的整套方法矩阵复制一份，把「文本」换成「图像」**。`ImageType` 是路径类型。

`SparseEmbedding = Dict[int, float]` ——一行类型别名，就是整个稀疏表示。`BaseSparseEmbedding(BaseModel, DispatcherSpanMixin)` 与 `BaseEmbedding` 同构但不继承它（不同基类、不同追踪事件、不同 `to_payload`）。

`sparse_similarity(embedding1, embedding2) -> float` 是稀疏版的余弦相似度。

`Pooling(str, Enum)` 两值 `CLS = "cls"` 与 `MEAN = "mean"`，且**枚举实例自己可调用**：`pooling(array)` → 分派到 `cls_pooling` 或 `mean_pooling`。

## 怎么实现的

**图像嵌入与文本嵌入在方法矩阵上完全对称，但**有一处不对称值得看。对比 i01 的稠密版：`_aget_text_embedding` 的默认实现是「同步回退」（`return self._get_text_embedding(text)`），而 `MultiModalEmbedding` 里 `_aget_image_embedding` 是 **`@abstractmethod`** ——必须实现。

所以「异步图像嵌入」不能自动从同步版本推导出来。这是一个刻意的收紧：图像嵌入通常是真的 I/O（读文件、调视觉模型），把它做成必选能避免「伪异步阻塞事件循环」。而文本嵌入有时是纯计算的（本地小模型），同步回退可接受。

同理 `_get_image_embeddings`（批量）是**可覆盖但非必选**，默认逐条循环；`_aget_image_embeddings` 默认 `asyncio.gather`。这与 i01 的文本批量策略完全一致。

**`get_image_embedding_batch` 的同步版本在循环内开 callback 事件，异步版本分两阶段。** 同步实现边攒批边 flush：

```python
for idx, img_file_path in queue_with_progress:
    cur_batch.append(img_file_path)
    if idx == len(img_file_paths) - 1 or len(cur_batch) == self.embed_batch_size:
        with self.callback_manager.event(CBEventType.EMBEDDING, payload=...) as event:
            embeddings = self._get_image_embeddings(cur_batch)
            result_embeddings.extend(embeddings)
            event.on_end(payload={CHUNKS: cur_batch, EMBEDDINGS: embeddings})
        cur_batch = []
```

而异步实现**先攒完所有批次再一次性 gather**：

```python
for idx, img_file_path in enumerate(img_file_paths):
    cur_batch.append(img_file_path)
    if ...:
        event_id = self.callback_manager.on_event_start(...)
        callback_payloads.append((event_id, cur_batch))
        embeddings_coroutines.append(self._aget_image_embeddings(cur_batch))
        cur_batch = []

nested_embeddings = await asyncio.gather(*embeddings_coroutines)   # 或 tqdm_asyncio.gather
result_embeddings = [embedding for embeddings in nested_embeddings for embedding in embeddings]
```

三处值得记：

1. **异步版把所有批次的协程都先建出来再一起 await** ——所以所有批次**并发**跑，而不是「一批等一批」。这是异步能带来的真实收益（`asyncio.gather` 让 N 个批同时飞）。
2. **callback 事件被拆成 `on_event_start` / `on_event_end` 两半**，因为事件的生命周期跨越了 await。`callback_payloads: List[Tuple[str, List[ImageType]]]` 存的是 `(event_id, 该批的输入)` ——所以 `event_id` 必须显式保存，回复时才能配对。
3. `nested_embeddings` 是「列表的列表」，最后用双层推导展平。那句注释写着「展平 `asyncio.gather` 的结果，它是一个嵌入列表的列表」。

**`show_progress` 分支里有一个 try/except ImportError。** 有进度时尝试 `from tqdm.asyncio import tqdm_asyncio` 并用 `tqdm_asyncio.gather`，导入失败就退回 `asyncio.gather`。所以「进度条」是可选依赖，缺失时功能不受影响、只是没有进度显示。这个模式在稀疏版里也重复出现。

**稀疏嵌入与稠密嵌入是两套平行的基类，不继承。** `BaseSparseEmbedding(BaseModel, DispatcherSpanMixin)` 而 `BaseEmbedding(TransformComponent, DispatcherSpanMixin)`。所以稀疏版**不是 `TransformComponent`** ——它没有 `__call__` 把节点批量转换那套接口。

字段也少：只有 `model_name` / `embed_batch_size` / `num_workers`，**没有** `callback_manager`（改用 `dispatcher` 事件的追踪机制）、没有 `embeddings_cache`、没有 `rate_limiter`。

所以「稀疏嵌入」在这个框架里是一个**更轻的公民**：能被构造、能被调用、有追踪，但不参与「节点转换」那条链路。

**`BaseSparseEmbedding` 的四个嵌入方法全都要实现，没有同步回退。** `_get_query_embedding` / `_aget_query_embedding` / `_get_text_embedding` / `_aget_text_embedding` 全是 `@abstractmethod`。

对比稠密版的 `_aget_text_embedding` 有同步回退——这个差别的理由是：稀疏嵌入**完全没提供**「异步从同步推导」的通道。所以实现方必须分别写同步与异步版本。这比稠密版严格，也意味着稀疏嵌入的后端实现成本更高。

**`custom_model_dump` 会主动删掉 `api_key`。** 这是一个 `@model_serializer(mode="wrap")`：

```python
data = handler(self)
data["class_name"] = self.class_name()
data.pop("api_key", None)
return data
```

所以序列化时会**剥掉 `api_key` 字段**。这与 i01 的 `to_payload` 那条「绝不能包含凭据」的 docstring 是同一件事的两种实现：那边靠「白名单三个字段」，这边靠「全量 dump 后删掉敏感键」。

后者的方向更危险——它是**减法**（默认全导出，删已知敏感的），而前者是**加法**（默认不导出，显式列安全的）。任何新加入的敏感字段（例如 `api_token`、`auth_header`）都会被漏掉。这是一个真实的取舍点：`pop("api_key")` 只防住了那一个名字。

**`get_agg_embedding_from_queries` 在稀疏版里有了自己的 `mean_agg`。** 两个 `mean_agg` 同名但不同实现：

- 稠密版（i01）：`np.array(embeddings).mean(axis=0).tolist()`，空输入 `raise ValueError`。
- 稀疏版：`defaultdict(float)` 累加每个下标，再按 `len(embeddings)` 除，**空输入返回 `{}`**。

所以稀疏版的空输入语义是「返回空字典」而不是报错。而且它的均值逻辑是「按下标累加再除以数量」——所以「只有部分嵌入出现的下标」也会被除以总数，等于把缺失当成 0。对于稀疏向量这是正确的（缺失本来就意味着该维度权重为 0）。

**`sparse_similarity` 是余弦相似度，且有一处性能优化。** 完整流程：

```python
if not embedding1 or not embedding2: return 0.0
if len(embedding1) > len(embedding2): embedding1, embedding2 = embedding2, embedding1

norm1 = norm2 = dot_product = 0.0
common_indices = set(embedding1.keys()) & set(embedding2.keys())
for idx, value in embedding1.items():
    norm1 += value**2
    if idx in common_indices:
        dot_product += value * embedding2[idx]
for value in embedding2.values():
    norm2 += value**2

if norm1 == 0.0 or norm2 == 0.0: return 0.0
return dot_product / (math.sqrt(norm1) * math.sqrt(norm2))
```

四步各有理由：

1. **空向量直接返回 0.0**（不是报错）。所以「没嵌入过」被当成「不相似」，而不是异常。这与稠密版 `mean_agg` 报错的方向相反，但这里合理——相似度是个度量，返回 0 有意义。
2. **交换让 `embedding1` 是更小的那个** ——所以 `common_indices` 的构造与第一层循环都作用在小集合上。注释写着「用较小的嵌入作为主迭代集」。
3. **先算 `common_indices` 再循环**，所以 `if idx in common_indices` 是集合查找（O(1)）而不是字典查找。注意它**没用** `embedding2.keys()` 直接查——虽然字典查找也是 O(1)，但用集合先把交集算出来，避免了在循环里反复访问 `embedding2`。
4. **两个范数分别算**：小集合那个在循环里顺便累加 `norm1`，大集合那个单独一遍。所以两次循环覆盖全部值，没有多余遍历。

零点判据 `if norm1 == 0.0 or norm2 == 0.0: return 0.0` ——所以「全是零权重」的向量被判为不相似，不会除零。

`BaseSparseEmbedding.similarity` 方法只是转发到这个模块函数——与稠密版一样的形态。

**`Pooling` 的两级 shape 判断是这一页最实在的细节。** `cls_pooling`：

```python
if len(array.shape) == 3: return array[:, 0]     # (batch, seq, dim) → 取每个句子的第一个 token
if len(array.shape) == 2: return array[0]        # (seq, dim) → 取第一个 token
raise NotImplementedError(f"Unhandled shape {array.shape}.")
```

`mean_pooling`：

```python
if len(array.shape) == 3: return array.mean(axis=1)   # 对 seq 维取均值
if len(array.shape) == 2: return array.mean(axis=0)
raise NotImplementedError(...)
```

三维 = 带 batch，二维 = 单句。两个方法都支持两种输入形状，这是必要的：嵌入一条与嵌入一批都要能用。

`cls_pooling` 取的是**第一个 token**（`[CLS]`，或位置 0）。这与 BERT 类模型的惯例一致：预训练时分类任务就用 `[CLS]` 位的输出。而 `mean_pooling` 对全部 token 取均值——需要 `attention_mask` 才能正确排除 padding，但这里只收数组，**所以带 padding 的批量输入做 mean pooling 会把 padding 也算进去**。

这是一个真实的隐含要求：调用方必须先自己按 mask 处理（或保证批内等长且无 padding），再调 `mean_pooling`。而签名里没有 mask 参数，所以这个要求只能靠文档传达（而 docstring 只写了「Enum of possible pooling choices with pooling behaviors」）。

`Pooling.__call__` 判的是 `self == self.CLS` ——注意是 `self.CLS` 而不是 `Pooling.CLS`（两者等价，但写法上用了实例访问类属性）。若不等就一律走 `mean_pooling`，所以「未来新增第三种 Pooling」会静默落到均值上——除非同时改 `__call__`。

`cls_pooling` 有两种 `@overload`（`np.ndarray` 与 `torch.Tensor`），带一条注释解释那个 `# type: ignore`：「等 mypy 修掉这个误报问题后可以删掉」。所以这个文件是**同时支持 numpy 与 torch** 的——`torch` 的导入被 `if TYPE_CHECKING` 包住且标了 `# pants: no-infer-dep`，所以运行时不需要装 torch。而 `mean_pooling` **只有** `np.ndarray` 的签名（没有 torch 重载），但 `array.mean(axis=1)` 在 torch 张量上同样成立——所以实际是兼容的，只是类型标注没有覆盖。

## 关键取舍

**源码事实：** `MultiModalEmbedding` 的图像方法与稠密文本方法在矩阵上完全对称，但 `_aget_image_embedding` 是 `@abstractmethod`（而稠密版的 `_aget_text_embedding` 有同步回退）。

**源码事实：** 图像批量的同步版边攒边 flush，异步版**先建好所有批次的协程再一次性 gather**，且 callback 事件被拆成 `on_event_start` / `on_event_end` 两半。

**源码事实：** `show_progress` 的进度条依赖 `tqdm.asyncio`，导入失败静默退回 `asyncio.gather`。

**源码事实：** `BaseSparseEmbedding` 与 `BaseEmbedding` 是两套平行基类，字段更少（无 callback manager、无 cache、无 rate limiter），且四个嵌入方法**全部必选**。

**源码事实：** `custom_model_dump` 用「全量 dump 后 `pop("api_key")`」的减法方式剥凭据，而 `to_payload` 用三字段白名单的加法方式。

**源码事实：** 两个 `mean_agg` 同名不同实现：稠密版空输入报错，稀疏版返回 `{}`。

**源码事实：** `sparse_similarity` 交换使小集合成为主迭代集，先算交集再循环，两个范数分别在两遍里累加。

**源码事实：** `Pooling` 支持二维与三维两种输入形状；`cls_pooling` 取位置 0，`mean_pooling` 对 seq 维取均值且**没有 mask 参数**。

**源码事实：** `Pooling.__call__` 只在 `self == self.CLS` 时走 CLS 分支，其余一律均值。

**架构推论：** 「图像嵌入与文本嵌入对称」这个做法看似省事（复制一套方法），实际暴露了一个设计局限：图像与文本在这个抽象里**只是两种并列的输入类型**，无法表达「一张图与一段文字拼成同一个向量」——那才是真正的多模态嵌入（CLIP 那一类）。所以 `MultiModalEmbedding` 的真实含义是「这个模型既能嵌图也能嵌文，两个空间可能相同也可能不同」，而不是「图文联合嵌入」。任何需要后者的场景，都得在更高层组合，或者干脆自己实现。

**架构推论：** 稀疏嵌入另建基类而不复用稠密基类，反映的是「这两种表示的生命周期不同」。稠密嵌入是 `TransformComponent`（要参与节点转换流水线），稀疏嵌入不是——因为它通常只用于查询侧（BM25 那种「查询稀疏、文档稀疏」的倒排检索），不需要「把一批节点转成向量存进去」这个动作。所以抽象的分裂不是遗漏，而是职责本来就不同。

**架构推论：** `Pooling` 那个「无 mask 参数」的限制揭示了「池化质量取决于调用方是否预处理」这个常见现象。任何「把序列压成单向量」的函数都会遇到 padding 问题，而提供池化函数的库通常只做数学、不管 mask。判据很清楚：**若这个函数接受的是「批量的二维/三维数组」，那 padding 一定已存在，而 mask 一定在调用方手里**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/embeddings/multi_modal_base.py` | `MultiModalEmbedding` | 图像嵌入的方法矩阵，与文本对称 |
| `llama-index-core/llama_index/core/embeddings/multi_modal_base.py` | `_aget_image_embedding` | 必选抽象，不接受同步回退 |
| `llama-index-core/llama_index/core/embeddings/multi_modal_base.py` | `get_image_embedding_batch` | 边攒边 flush，每次 flush 一个 callback 事件 |
| `llama-index-core/llama_index/core/embeddings/multi_modal_base.py` | `aget_image_embedding_batch` | 所有批次协程先建后 gather，事件分两半 |
| `llama-index-core/llama_index/core/base/embeddings/base_sparse.py` | `BaseSparseEmbedding` | 稀疏嵌入的平行基类，字段更少 |
| `llama-index-core/llama_index/core/base/embeddings/base_sparse.py` | `SparseEmbedding` | `Dict[int, float]` 的别名 |
| `llama-index-core/llama_index/core/base/embeddings/base_sparse.py` | `sparse_similarity` | 余弦相似度，小集合主导遍历 |
| `llama-index-core/llama_index/core/base/embeddings/base_sparse.py` | `mean_agg` | 按下标累加再均分，空输入返回空字典 |
| `llama-index-core/llama_index/core/base/embeddings/base_sparse.py` | `custom_model_dump` | 序列化剥 `api_key` 的减法方式 |
| `llama-index-core/llama_index/core/base/embeddings/base_sparse.py` | `aget_text_embedding_batch` | 可用 `run_jobs` 限并发，展平嵌套结果 |
| `llama-index-core/llama_index/core/embeddings/pooling.py` | `Pooling` | 二值枚举，实例可直接调用 |
| `llama-index-core/llama_index/core/embeddings/pooling.py` | `Pooling.cls_pooling` | 取位置 0，支持 numpy 与 torch |
| `llama-index-core/llama_index/core/embeddings/pooling.py` | `Pooling.mean_pooling` | 对 seq 维取均值，无 mask 参数 |

## 参考

- [llama-index 源码：embeddings/multi_modal_base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/embeddings/multi_modal_base.py)
- [llama-index 源码：base/embeddings/base_sparse.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/base/embeddings/base_sparse.py)
- [llama-index 文档：多模态](https://docs.llamaindex.ai/en/stable/module_guides/models/multi_modal/)

## 与主核 / AI 的关系

主核里最接近的是「两种索引表示」：稠密索引（例如 HNSW，每个向量固定维度）与倒排索引（词项 → 文档列表，天然稀疏）。这一页的稠密与稀疏两套基类，在职责上与那两种索引结构对应——一个用于「按向量近邻找」，一个用于「按词项匹配」。

差别在于**抽象的对称性**。主核的两种索引共用同一套「索引访问方法」骨架，因为它们服务于同一个查询算子；而这里的两个基类不共用骨架，因为稀疏嵌入几乎没有「批量写成节点属性」的需求。所以「看起来同构的两个东西该不该共用一个基类」这件事，取决于它们是否共享**使用流程**而不只是**数据形状**。

与 i01（模型与嵌入抽象）的关系是直接的父类关系：`MultiModalEmbedding` 就是 `BaseEmbedding` 的子类，而稀疏版是与它平级的另一支。

与 h03（向量存储契约）的呼应：`VectorStoreQueryMode` 里有 `SPARSE` 与 `HYBRID` 两个值，而稀疏嵌入正是 feeding 这两个模式的组件。`VectorStoreQuery.alpha` 的注释「0 是 bm25、1 是向量」也直接对应稀疏 vs 稠密。

对 AI 工程最实用的一条是 `Pooling` 那两种池化在**语义上不等价**。`cls_pooling` 取的是模型预训练时被训练成「汇总整个序列」的那一位；`mean_pooling` 是对所有位平均。前者对分类/检索任务通常更好（因为预训练目标就是让它承载句义），后者在 fine-tuned 的句向量模型上常用。所以「用哪种池化」不是一个超参数，而是**要不要信任原模型的训练目标**。而这一点在只接收数组的函数上完全看不出来。

另一条是 `mean_pooling` 无 mask 参数那条限制。任何「对序列维求均值」的池化，在批量输入上都会被 padding 污染。正确做法要么在调用方用 mask 加权（`(array * mask[..., None]).sum(1) / mask.sum(1, keepdims=True)`），要么保证批内等长无填充。判据很简单：**若你要对 seq 维做聚合，先问 mask 在哪**。

最后一条是 `sparse_similarity` 那个「交换使小集合主导」的优化。稀疏向量的长度可能差两个数量级（短查询 vs 长文档），把循环次数绑定在较小的一侧，能让复杂度从「与文档长度成正比」降到「与查询长度成正比」。这与倒排索引「按查询词项去发帖列表里查找」的思路是同一个——**遍历小的一侧，在大的一侧做查找**。
