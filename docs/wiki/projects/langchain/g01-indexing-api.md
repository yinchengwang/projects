---
project: langchain
feature: indexing-api
title: 文档索引与去重清理
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/indexing/api.py
    symbols: [index, IndexingResult, IndexingException]
  - path: libs/core/langchain_core/indexing/api.py
    symbols: [_HashedDocument]
---

# 文档索引与去重清理

## 一句话职责

把一批文档写进向量库时，判断哪些是新增、哪些是内容变了要更新、哪些已经是最新可以跳过，并按指定模式清理掉「源头已不再产出」的旧文档。

## 机制是什么

索引（ingestion 的落库阶段）看起来只是「算嵌入再写入」，实际难点全在**去重**：

- 同一份文档重复投喂不该产生重复向量；
- 内容改了一点点就该替换旧向量；
- 源文件被删了，它对应的向量也该消失，但「删了」这个事实只有靠**比对上一轮的记录**才知道。

langchain 的做法是三层：**内容哈希做文档身份**、**记录管理器记「上一轮每个源有哪些文档」**、**索引函数对比两侧差集并决定增删改**。

## 怎么实现的

**三层哈希得到文档 id。** 当 `key_encoder` 是算法名（`sha1`/`sha256`/`sha512`/`blake2b`）时，代码分别算出**内容哈希**与**元数据哈希**（元数据先 `json.dumps(..., sort_keys=True)` 保证键序稳定），再把两个哈希串拼接后再哈希一次，得到最终 id。

三次哈希而非一次的理由在代码里没写，但两处行为可以解释它：一是 `sort_keys=True` 说明要容忍元数据字典的构造顺序差异；二是拼接后再哈希使「内容相同但元数据不同」与「元数据相同但内容不同」都产生不同 id，而单独用内容哈希就无法区分前者。副作用是**任何元数据变化都会让文档被当成新文档**——这对「来源路径」这类元数据是想要的，对「抓取时间」这类则是灾难。

`key_encoder` 也可以传**可调用对象**，此时完全交给调用方，框架不拼元数据。文档明确写着两条配套要求：默认 SHA-1 **不具备抗碰撞性**，有威胁模型的场景要换成 `sha256`/`blake2b`；换编码器后必须**同时换索引**，否则新旧 id 空间混在一处会产生重复文档。SHA-1 还会触发一次性 `UserWarning`，提示里直接给出替代算法名。

**`_HashedDocument` 是一个只会报错的类。** 它的 `__init__` 无条件抛 `NotImplementedError`，消息里写明「这是 langchain-core 0.3.63 起废弃的内部抽象，本不该被直接使用」。保留这个类是为了让仍在使用它的旧代码拿到**明确的迁移指引**，而不是 `ImportError`。

**索引主流程由 `cleanup` 模式驱动。** 三个取值：

- `None` —— 只增不删。文档里写明「automatically remove documents that it is not aware of」这句话只对 `full`/`scoped_full` 成立，`None` 完全不清理。
- `incremental` —— 只清理**本批文档涉及的源**下面的旧文档。它要求必须给 `source_id_key`（一个元数据键名或可调用对象），因为「哪些源被本批覆盖」只能从本批文档自己推出来。缺省时报错。
- `full` / `scoped_full` —— 清理范围覆盖「上一轮记录过、这一轮没出现」的一切文档。`scoped_full` 是新增的模式，用于把清理范围限定在某个作用域内。

`source_id_key` 的解析由 `_get_source_id_assigner` 做：`None` → 没有 assigner；字符串 → 从元数据里取该键；可调用 → 直接用。**这三种形态都合法但语义不同**，所以校验放在了入口，非字符串非可调用直接报错并带上类型。

**删除要兼容两种目标。** `_delete` 同时接受 `VectorStore` 与 `DocumentIndex`：

- 对 `VectorStore`，`delete(ids)` 返回 `None`（没返回值）或 `bool`，**只在显式返回 `False` 时**判失败。返回 `None` 视为「不知道但没报错」，不抛异常。
- 对 `DocumentIndex`，`delete(ids)` 返回字典，只有 `num_failed > 0` 才判失败。

两种判定都是**保守方向**：不确定时不报错，明确失败才报错。这和「宁可漏删也不要误报」的选择一致——因为清理失败通常意味着下一轮重试即可，而把成功的删除报成失败会让人去查一个不存在的问题。

**批量删除有 `cleanup_batch_size`（默认 1000）。** 大批量删除对某些后端是不友好的，分批是为了避免一次打爆。

## 关键取舍

**源码事实：** 文档 id 由「内容哈希 + 元数据哈希」拼接后再哈希，元数据用 `sort_keys=True` 序列化。任何元数据变化都会导致文档被判为新文档。

**源码事实：** 默认 SHA-1 并主动发出一次性 `UserWarning`，提示里写明「不抗碰撞」并给出替代算法。文档要求更换编码器时**同时更换索引**。

**源码事实：** `_HashedDocument` 被保留为一个无条件抛错的类，用来给旧代码明确的迁移错误。

**源码事实：** `cleanup` 有四个取值（含 `None`），`incremental` 强制要求 `source_id_key`；`_delete` 只在明确失败时抛 `IndexingException`。

**架构推论：** 「内容哈希做身份」这件事的隐含前提是**同一份内容在任何地方都该有同一个 id**。这让索引具备幂等性（重跑不产生重复），但把「文档身份」绑定到了内容本身。工业界有另一种做法（用源路径 + 版本号做身份），好处是元数据变化不触发重写，坏处是内容不变时无法去重。两种都不是错，取决于「同一内容是否可能出现在多个源」。

**架构推论：** 清理模式的设计暴露了一个现实：**「删除了什么」无法从当前批次推断，只能靠记录上一轮**。所以索引必须有 `RecordManager` 这个额外状态。这也是增量索引比全量重建麻烦的根本原因——它需要外部持久化状态，而这个状态本身也有失效风险（记录丢了就会漏删）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/indexing/api.py` | `index` | 索引主流程，按 `cleanup` 模式决定清理 |
| `libs/core/langchain_core/indexing/api.py` | `IndexingResult` | 增删改跳四项计数 |
| `libs/core/langchain_core/indexing/api.py` | `IndexingException` | 删除失败时的统一异常 |
| `libs/core/langchain_core/indexing/api.py` | `_HashedDocument` | 已废弃的占位类，实例化即报错 |
| `libs/core/langchain_core/indexing/api.py` | `_get_source_id_assigner` | `source_id_key` 三种形态的归一 |
| `libs/core/langchain_core/indexing/api.py` | `_delete` | 兼容两种目标、只在明确失败时报错 |
| `libs/core/langchain_core/indexing/api.py` | `NAMESPACE_UUID` | 生成确定性 UUID 的命名空间常量 |

## 参考

- [langchain 源码：indexing/api.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/indexing/api.py)
- [langchain 文档：如何索引文档并处理去重](https://python.langchain.com/docs/how_to/indexing/)
- [langchain 文档：记录管理器](https://python.langchain.com/api_reference/core/indexing/langchain_core.indexing.base.RecordManager.html)
- [OWASP：哈希算法选择](https://cheatsheetseries.owasp.org/cheatsheets/Cryptographic_Storage_Cheat_Sheet.html)

## 与主核 / AI 的关系

RAG 索引去重与主核（多模态数据库内核）的**主键与唯一约束**在目标上一致，但机制不同：

主核用**显式主键**表达身份：用户声明哪一列是主键，然后插入时按主键判断冲突。RAG 索引没有主键概念，只能用**内容哈希**当身份——因为向量库通常只接受任意 id，而语义上「同一份内容」才该是同一个条目。两种做法的差别在于**谁承担去重责任**：主核让用户声明身份，索引让框架推导身份。

**内容哈希当身份的三个后果**值得对比主核的实现：

- 幂等重跑：同一个文档投两次只留一份。主核里对应「INSERT ... ON CONFLICT DO NOTHING」。
- 元数据变化等于新文档：主核里主键不含所有列，所以改非主键列不会换身份。RAG 索引反过来，把元数据算进了身份。
- 需要额外的「上一轮记录」才能删：主核里删除靠显式 `DELETE`，不需要历史状态。

最后一条最能说明增量索引的复杂度来源：它必须**持久化一份外部状态**（记录管理器）来表达「上次见过什么」，而这个状态本身可能失效、可能不一致。主核的 MVCC 与索引维护也在处理同一类问题——只不过它把状态放在数据库内部，由事务保证一致性；RAG 索引把状态放在框架层，由调用方自己承担丢记录的后果。
