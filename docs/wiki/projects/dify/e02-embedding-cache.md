---
project: dify
feature: embedding-cache
title: 嵌入缓存
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/embedding/cached_embedding.py
    symbols: [CacheEmbedding]
---

# 嵌入缓存

## 一句话职责

`CacheEmbedding` 在「调模型拿向量」与「调用方」之间插一层两级缓存：**文档向量落数据库表、查询向量落 Redis**，并在写缓存之前统一做 L2 归一化。

## 机制是什么

四个入口，缓存策略各不相同：

| 方法 | 缓存介质 | 键 | TTL |
|---|---|---|---|
| `embed_documents` | 数据库 `embeddings` 表 | `(provider, model_name, 文本哈希)` | 永久 |
| `embed_query` | Redis | `provider_model_文本哈希` | 600 秒 |
| `embed_multimodal_documents` | 数据库 `embeddings` 表 | `(provider, model_name, 文件 id)` | 永久 |
| `embed_multimodal_query` | Redis | `provider_model_文件 id` | 600 秒 |

**为什么两种介质不同**：文档向量会被反复用到（重新索引、不同检索任务），且量大、能接受占库；查询向量每条通常只用一次，量还随流量增长，放数据库会迅速膨胀——所以走 Redis 加短 TTL。

**为什么嵌入文本与文件的键不同**：文本的键是内容哈希，所以「内容相同即命中」，两个不同片段只要正文一样就共享向量；文件的键是 `file_id`，所以同一张图必然命中、内容变了也不会（图片无法便宜地算内容哈希）。

## 怎么实现的

**`embed_documents` 的两阶段：先查缓存、再批量算、最后回写。**

第一阶段在**一个 session 里逐条查**：对每个文本算哈希，按三元组（模型名、哈希、provider）查 `Embedding` 行，命中就把 `get_embedding()` 的结果填进结果数组的同一位置，未命中则把**下标**记进 `embedding_queue_indices`。

第二阶段只对未命中的下标对应文本分批调模型。批大小来自模型自身的 `MAX_CHUNKS` 属性（拿不到就退化成 1）——**这是一个很实际的细节**：不同嵌入模型单次能吃的条数不同，硬编码批大小会让某些模型直接报错。

第三阶段回写。这里有一段注释值得逐字读：

> Each vector belongs to a specific text. A vector that cannot be used is dropped where it was produced, so collecting the usable vectors in a separate list and zipping that list with the indices afterwards would pair every later text with its neighbour's vector.

也就是说：**失败的向量必须在原位丢弃，不能事后压缩**。如果先把可用向量收集成一个新列表、再和下标列表 `zip`，一旦中间某个向量被丢掉，后面所有文本都会配上相邻文本的向量。这类错配不会报错，只会让检索结果莫名其妙——所以它用 `batch_indices = embedding_queue_indices[i : i + len(batch_texts)]` 精确对齐，然后 `zip(..., strict=False)` 逐个填回**原始位置**。

**归一化与 NaN 检查是这个方法的核心质量措施。**

```python
normalized_embedding = (vector / np.linalg.norm(vector)).tolist()
if np.isnan(normalized_embedding).any():
    continue
```

L2 归一化让向量库可以用内积直接算余弦相似度；`np.isnan` 检查针对的是 issue #11827——**某些 provider 会返回 NaN，而 NaN 不是合法 JSON**，写进向量库或序列化时会炸。检查放在归一化之后，因为归一化本身可能把 0 向量变成 NaN（0 除以 0）。

被跳过的向量会让那个位置保持 `None`。所以 `embed_documents` 的返回类型实际上是「`list[list[float]]` 但可能含 `None`」——回写阶段有 `if n_embedding is None: continue` 兜住，但**调用方拿到的数组里可能真有 `None`**，这是当前实现的真实语义。

**回写是「先收集、后提交、IntegrityError 就回滚」。** 它用 `cache_embeddings` 这个列表做**批内去重**（同一批里相同哈希只插一次），然后在 `try` 里 `session.commit()`，`except IntegrityError` 则 `rollback()`。

这个 `IntegrityError` 分支的含义是：**并发写同一条哈希**。两个任务同时算同一段文本时，后插入的那个撞唯一约束，回滚即可——数据已经在了，不需要重试。这比「先查后写」的加锁方案简单，代价是靠数据库约束兜底而不是靠应用层协调。

**第四阶段的异常处理分两层。** 内层每个向量单独 `try/except`，归一化或 NaN 检查出错只记日志 `Failed transform embedding` 并继续下一个；外层包住整批，异常记 `Failed to embed documents` 后**原样重抛**。所以「单个向量坏了」是降级，「整批调用失败」是上抛。

**`embed_query` 在 `get_cached_query_embedding` 上叠了一层冗余。** 它自己算了一遍 `embedding_cache_key`，又调 `get_cached_query_embedding(provider, model_name, text)`——后者内部**再算一次同样的哈希与键**。这是重复计算，没有功能影响，但读的时候会疑惑「为什么算两遍」。合理推测是：静态方法是为了让不需要 `ModelInstance` 的调用方（见 E 组第一页的懒代理）也能查缓存，而 `embed_query` 保留了自己的键是为了后面 `setex` 用。

**查询缓存命中时会刷新 TTL。** `get_cached_query_embedding` 命中后 `redis_client.expire(key, TTL)`——所以热查询的向量会一直留在缓存里，冷查询到期即淘汰。这是 LRU-ish 效果，靠「访问即续期」实现。

**Redis 里存的是 base64 编码的原始 float 字节，不是 JSON。**

```python
vector_bytes = np.array(embedding_results).tobytes()
encoded_str = base64.b64encode(vector_bytes).decode("utf-8")
```

读回时 `np.frombuffer(base64.b64decode(embedding), dtype="float")`。这比 JSON 省得多（float 文本序列化会损失精度还翻倍占用），代价是**依赖 `dtype="float"` 与平台一致**——`np.frombuffer` 的 `"float"` 是平台相关的（通常 64 位），若模型返回 float32，这里的 `tobytes()` 与 `frombuffer(dtype="float")` 就读错了字节数。这是一个真实的类型假设，代码里没有显式指定 float32/float64。

**多模态那两条几乎是文本版的复制粘贴。** 结构、注释、NaN 检查、回写逻辑都一样，差别只有三点：键用 `file_id` 而非内容哈希、调 `invoke_multimodal_embedding`、以及 `embed_multimodal_documents` 多调了一次 `embedding_cache.set_embedding(n_embedding)`（文本那路没调，但已经在构造时传了 `embedding=pickle.dumps(...)`）。也就是说多模态这条路**同时设了 `embedding` 字段和调用 setter**，文本路只设字段——两处写法不一致。

**`QUERY_CACHE_TTL` 只在 `embed_query` 路径生效。** `embed_multimodal_query` 里写死 `600`，两次用了字面量而不是类常量。改 TTL 时容易只改一处。

## 关键取舍

**源码事实：** 文档向量存数据库表（永久），查询向量存 Redis（600 秒，命中即续期）。

**源码事实：** 文本缓存的键是内容哈希，多模态的键是 `file_id`；因此文本「内容相同即共享」，图片「同一文件即共享」。

**源码事实：** 批大小取模型 schema 的 `MAX_CHUNKS`，缺失时退化为 1。

**源码事实：** 失败或 NaN 的向量在**原位**留下 `None`，不允许事后压缩，否则会整体错位（代码注释明确解释了这个陷阱）。

**源码事实：** 归一化是 L2 归一化，NaN 检查在归一化之后；`np.isnan` 之后 `continue`。

**源码事实：** 返回数组可能含 `None`；回写阶段跳过 `None`，但调用方拿到的仍是含 `None` 的数组。

**源码事实：** 回写用 `IntegrityError → rollback` 处理并发写同一哈希，不重试。

**源码事实：** Redis 里存 base64 后的原始 float 字节，读回用 `np.frombuffer(..., dtype="float")`。

**源码事实：** `embed_query` 里重复计算了一次缓存键；`embed_multimodal_query` 的 TTL 写死 `600`，未用类常量。

**架构推论：** 「宁可返回 `None` 也不返回错位的向量」是这个类最重要的正确性决定。错位向量的危害远超缺失向量：缺失会让某条片段检索不到（可观测、可重试），错位会让 A 片段检索到 B 片段的内容（不可观测、只能表现为"答非所问"）。**在批处理里，任何「过滤后重组」都要先问一句：位置还对不对得上？**

**架构推论：** 文档向量与查询向量用不同介质，本质上是对「写入成本 vs 存储成本」的不同权衡。文档是写多读多但总量可控；查询是写多读一次且总量随流量无限增长。用同一套策略必然有一边不合适。**这也是为什么两者的 TTL 差了两个数量级（永久 vs 10 分钟）。**

**架构推论：** NaN 检查的存在说明上游 provider 的返回质量不可控。把这类校验放在**写缓存之前**而不是「用之前」，是因为一旦脏数据落进缓存就永久留存（文档向量没有 TTL）——**缓存的清洗必须发生在写入侧，读侧兜底只能救急。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/embedding/cached_embedding.py` | `CacheEmbedding` | 嵌入缓存门面，四入口两种介质 |
| `api/core/rag/embedding/cached_embedding.py` | `CacheEmbedding.embed_documents` | 查库命中、按 `MAX_CHUNKS` 分批、原位回填、回写 |
| `api/core/rag/embedding/cached_embedding.py` | `CacheEmbedding.embed_query` | Redis 查询缓存 + 归一化 + base64 存字节 |
| `api/core/rag/embedding/cached_embedding.py` | `CacheEmbedding.get_cached_query_embedding` | 静态查缓存，命中续期，无需模型实例 |
| `api/core/rag/embedding/cached_embedding.py` | `CacheEmbedding.embed_multimodal_documents` | 以文件 id 为键的多模态文档缓存 |
| `api/core/rag/embedding/cached_embedding.py` | `CacheEmbedding.embed_multimodal_query` | 多模态查询缓存，TTL 写死 |
| `api/core/rag/embedding/cached_embedding.py` | `CacheEmbedding._query_cache_key` | 统一缓存键构造（provider_模型_哈希） |
| `api/core/rag/embedding/cached_embedding.py` | `CacheEmbedding.QUERY_CACHE_TTL` | 查询缓存有效期常量（600 秒） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/rag/embedding/cached_embedding.py](https://github.com/langgenius/dify/blob/main/api/core/rag/embedding/cached_embedding.py)
- [dify 文档：知识库与嵌入模型](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents)
- [numpy 文档：frombuffer 与 dtype](https://numpy.org/doc/stable/reference/generated/numpy.frombuffer.html)

## 与主核 / AI 的关系

「内容哈希做键的永久缓存 + 短 TTL 的热点缓存」与主核里「页面缓存 + 缓冲池」是同一层意思：都在昂贵操作前面挡一层，都靠「命中即复用」省成本。区别在于主核的缓冲池有淘汰策略（LRU/时钟）保证有界，而这里的文档向量缓存**没有淘汰**——它靠「嵌入表就是个普通业务表」这个假设，缺失的清理只能靠外部任务。

对 RAG 工程最实用的四条。其一：**同一个嵌入模型下，内容相同的片段只算一次向量**——所以建库前先去重正文能显著省钱，而改模型（provider 或模型名）会让缓存全部失效、必须重算。其二：**返回数组可能含 `None`**，直接 `zip(texts, vectors)` 会拿到空位；写下游代码时必须过滤或显式处理。其三：**NaN 检查发生在归一化之后**，说明 0 向量会导致该条被静默跳过——如果某个模型的空文本返回 0 向量，你会看到"向量数少于片段数"而不报错。其四：**缓存清洗在写入侧**，一旦脏向量落库（文档向量无 TTL）就永久留存，升级 provider 或修 bug 后要记得清表。
