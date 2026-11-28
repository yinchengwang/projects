---
project: llama-index
feature: ingestion-pipeline
title: 摄取流水线与文档去重
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/ingestion/pipeline.py
    symbols: [IngestionPipeline, DocstoreStrategy, run_transformations]
  - path: llama-index-core/llama_index/core/ingestion/pipeline.py
    symbols: [get_transformation_hash, remove_unstable_values]
---

# 摄取流水线与文档去重

## 一句话职责

把「原始文档 → 切分 → 嵌入 → 写入向量库」串成一条可重跑的流水线，并解决两个增量问题：**同一个文档重跑时不该重复处理**（变换级缓存）、**源文档变了或消失了时该更新或删除**（docstore 级 upsert/delete）。

## 机制是什么

两层去重，判据完全不同：

| 层 | 判据 | 存在哪 | 解决什么 |
|---|---|---|---|
| **变换缓存** | `get_transformation_hash(nodes, transform)` | `IngestionCache` | 「同一批节点 + 同一个变换」已经跑过 → 直接拿上次的输出 |
| **文档去重** | `node.hash` / `ref_doc_id` | `docstore` | 「这份文档」（按 hash）是否已在库中 → 决定跳过、更新、或删除 |

`DocstoreStrategy` 三个取值把第二层分成三档：

| 取值 | 判断依据 | 是否删除 |
|---|---|---|
| `UPSERTS` | 按 `ref_doc_id` 查已有 hash | 只删「被更新的那份」 |
| `DUPLICATES_ONLY` | 按 `node.hash` 全局比对 | 不删 |
| `UPSERTS_AND_DELETE` | 同 `UPSERTS` + 额外算差集 | **还删除本次未出现的所有旧文档** |

## 怎么实现的

### 一、变换缓存的 key：一段字符串哈希，而它必须排除「不稳定值」

**`get_transformation_hash` 的实现只有四步。**

```
nodes_str = "".join([str(node.get_content(metadata_mode=MetadataMode.ALL)) for node in nodes])
transformation_dict = transformation.to_dict()
transform_string = remove_unstable_values(str(transformation_dict))
return sha256((nodes_str + transform_string).encode("utf-8")).hexdigest()
```

1. **节点内容用 `metadata_mode=ALL`** ——即**正文 + 全部元数据**都参与哈希。于是元数据变化会让缓存失效——这与 `g01`（另一个项目的索引去重）里「元数据算进身份」是同一取向。
2. **变换用 `to_dict()` 再 `str()`** ——也就是**整个变换的序列化形态**参与哈希。于是换一个 `chunk_size`、换一个模型名，哈希都变。
3. **关键的一步是 `remove_unstable_values`**。
4. 拼接后 `sha256`。

**`remove_unstable_values` 是做这一页最重要的一件事，而它的实现是一条正则。**

```
pattern = r"<[\w\s_\. ]+ at 0x[a-z0-9]+>"
return re.sub(pattern, "", s)
```

**它删掉的是 Python 对象 `repr` 里的内存地址**，文档给了两个真实例子：

> `<__main__.Test object at 0x7fb9f3793f50>`
> `<function test_fn at 0x7fb9f37a8900>`

**为什么必须删？** 因为 `to_dict()` 的结果里可能包含**没有实现自定义序列化的对象**（比如一个自定义的 `TransformComponent` 里挂着的回调、客户端、锁）。它们的 `repr` 带内存地址，而内存地址**每次进程启动都不同**。

若不删，后果是：**同一份文档、同一个变换，在两个不同进程里算出两个不同的哈希**——于是缓存**永远不命中**。而这类失效不会报错，只会让人以为「缓存没生效是正常的」。

这条正则揭示的正是「哪些字段必须排除」这个问题的答案：**不是字段名，而是「repr 里带地址」这个特征**。也就是说这里用的是**特征匹配**而不是白名单——因为它无法穷举所有不稳定的字段名。

代价也明显：

- 它只匹配 `<... at 0x...>` 这一种形态。若某个对象的 `repr` 是 `Model(id=..., client=<openai.OpenAI object at 0x...>)` 中的一部分，能删掉；但若 `repr` 里嵌的是 `uuid4()` 之类，就删不掉。
- `[a-z0-9]+` 这个字符类**没有大写字母与超出 f 的十六进制**——而 Python 的地址是小写十六进制的，所以对当前 Python 是准的。这是一个和具体版本绑定的假设。

### 二、`run_transformations`：逐变换查缓存

**它的实现是一个简单的循环，而缓存的处理嵌在循环里。**

```
for transform in transformations_with_progress:
    if cache is not None:
        hash = get_transformation_hash(nodes, transform)
        cached_nodes = cache.get(hash, collection=cache_collection)
        if cached_nodes is not None:
            nodes = cached_nodes
        else:
            nodes = transform(nodes, **kwargs)
            cache.put(hash, nodes, collection=cache_collection)
    else:
        nodes = transform(nodes, **kwargs)
```

要点：

1. **哈希是「当前这批节点 + 这个变换」的**——所以**变换在链中的位置也被隐式编码了**（因为前面变换的输出是后面变换的输入）。若把两个变换交换顺序，第二个变换的哈希就变了。
2. **命中的是整批节点而不是单个节点。** 也就是说**只要批里有一个节点变了，整个批的缓存都失效**——于是「给一千个文档中的一个加了一行」会让这一千个文档全部重新跑。这是粒度问题：缓存粒度 = 批粒度。
3. **`in_place=True` 时用 `nodes = list(nodes)` 不复制？** 代码是 `if not in_place: nodes = list(nodes)` ——也就是说**默认 `in_place=True` 时不复制输入序列**。于是变换可能修改调用方传进来的对象（若变换自身是原地修改）。这是一个「默认可变」的选择。

**`arun_transformations` 是完整镜像，只把 `transform(nodes, ...)` 换成 `await transform.acall(...)`。**

**而 `arun_transformations_wrapper` 的存在解释了为什么需要两份：**

> Wrapper for async run_transformation. To be used in loop.run_in_executor within a ProcessPoolExecutor.

也就是说并行执行时，每个工作进程需要一个**同步签名的入口**去跑异步变换——于是这个 wrapper 内部大概是「在新的 event loop 里跑 `arun_transformations`」。这是「异步实现 + 多进程并行」两个需求交叉时的必要妥协。

### 三、`DocstoreStrategy` 三档的语义差别

**`DUPLICATES_ONLY` 的实现简单到只有十几行，而它的判据是全局 hash 集合。**

```
existing_hashes = self.docstore.get_all_document_hashes()
current_hashes: set[str] = set()
for node in nodes:
    if node.hash not in existing_hashes and node.hash not in current_hashes:
        self.docstore.set_document_hash(node.id_, node.hash)
        nodes_to_run.append(node)
        current_hashes.add(node.hash)
    return nodes_to_run
```

两个集合各管一件事：

- **`existing_hashes`** 挡「库里已经有了」——**按内容哈希**，所以「同一份内容出现在两个不同 doc_id 下」也会被挡（这正是 duplicates 的字面含义）。
- **`current_hashes`** 挡「本次批内重复」——若两个节点内容相同，第二个也会被跳过。

**注意它边判断边写入**（`set_document_hash` 在循环内），于是同一个 `run` 里后面的节点能看到前面刚写的记录。

**`UPSERTS` 的判据换成了「按 `ref_doc_id` 查」。**

```
ref_doc_id = node.ref_doc_id if node.ref_doc_id else node.id_
doc_ids_from_nodes.add(ref_doc_id)
existing_hash = self.docstore.get_document_hash(ref_doc_id)
if not existing_hash:
    deduped_nodes_to_run.append(node)                    # 新文档
elif existing_hash and existing_hash != node.hash:
    self.docstore.delete_ref_doc(ref_doc_id, raise_error=False)
    if self.vector_store is not None: self.vector_store.delete(ref_doc_id)
    deduped_nodes_to_run.append(node)                    # 变了，先删后加
else:
    continue                                             # 没变，跳过
```

三处值得记：

1. **`ref_doc_id` 缺省时回退到 `node.id_`** ——于是「一个节点自己就是一份文档」（未经切分的 `Document`）也能正确处理。
2. **变了的情况是「先删后加」** ——`delete_ref_doc` + `vector_store.delete` 再重新入列。这不是 `UPDATE`，而是 `DELETE` + `INSERT`。代价是向量库里的 id 会变（若 id 是按节点新生成的），收益是不必处理「部分更新的中间状态」。
3. **`raise_error=False`** ——删除不存在的引用文档不算失败。

**`UPSERTS_AND_DELETE` 多算一个差集，而它的实现有一个具体的缺陷。**

```
existing_doc_ids_before = set(self.docstore.get_all_document_hashes().values())
doc_ids_to_delete = existing_doc_ids_before - doc_ids_from_nodes
for ref_doc_id in doc_ids_to_delete:
    self.docstore.delete_document(ref_doc_id)
    if self.vector_store is not None: self.vector_store.delete(ref_doc_id)
```

**`get_all_document_hashes()` 的返回值在两处被当成两样东西用**：`_handle_duplicates` 里取 `.keys()` 隐含（`node.hash not in existing_hashes` 比的是哈希值），而这里取 `.values()` 当作「doc id」。

结合 `_update_docstore` 里那句 `set_document_hashes({n.id_: n.hash for n in nodes})` 可以推断：**这个映射是 `hash → doc_id` 还是 `doc_id → hash`？** `set_document_hashes` 传的是 `{id: hash}`，而 `get_document_hash(ref_doc_id)` 按 id 查 hash——所以底层映射是 `id → hash`，`.values()` 拿到的是 hash 而不是 id。

于是这里的差集算的是「库里所有 hash 减去本次的 doc id」——**两个不同的键空间**。这是一个真实的 bug：`doc_ids_to_delete` 里混的是哈希值，随后被当成 id 传给 `delete_document`。

这个错误的后果是「要删的没删（因为查不到那些 id）、该留的可能被误删」。而在有 `raise_error=False` 之类的宽容处理下，它可能表现为「删除静默无效」。

### 四、策略降级：缺向量库时的警告

**`run` 里有一段「策略不可用时降级并警告」的逻辑，而它写得相当小心。**

```
effective_strategy = self.docstore_strategy
if (self.docstore is not None and self.vector_store is None and self.docstore_strategy in (UPSERTS, UPSERTS_AND_DELETE)):
    warnings.warn(f"docstore_strategy='{self.docstore_strategy.value}' requires a vector store to apply upsert/delete semantics; falling back to 'duplicates_only' for this run. pipeline.docstore_strategy is unchanged.", UserWarning, stacklevel=3)
    effective_strategy = DocstoreStrategy.DUPLICATES_ONLY
```

三个细节：

1. **它引入一个 `effective_strategy` 局部变量**而不改 `self.docstore_strategy`——警告文案明确写了 `pipeline.docstore_strategy is unchanged`。于是**降级只影响这一次运行**，配置保持原样。
2. **`stacklevel=3`** ——让警告指向调用方而不是这个文件内部。对库代码这是个正确的选择。
3. **降级方向是「更保守的那个」**：`UPSERTS`/`UPSERTS_AND_DELETE` 都会删东西，而 `DUPLICATES_ONLY` 只跳过、不删。所以「没有向量库时」不会误删。

**注意这个条件包含了 `self.docstore is not None`** ——也就是说**只有 docstore 存在时才检查**。而后面真正的去重条件是 `if self.docstore is not None and self.vector_store is not None:`——**要求两者都在**。

于是有一个未覆盖的缺口：**`docstore` 与 `vector_store` 都没有时，既不降级也不去重**（`effective_strategy` 保持了原值但没被用到）。这不算错，但意味着「想只缓存变换、不落库」的用法下，策略配置是无效的。

### 五、`_update_docstore` 与批处理

**`_update_docstore` 按策略分支写记录。**

```
if effective_strategy in (UPSERTS, UPSERTS_AND_DELETE):
    self.docstore.set_document_hashes({n.id_: n.hash for n in nodes})
    self.docstore.add_documents(nodes, store_text=store_doc_text)
elif effective_strategy == DUPLICATES_ONLY:
    self.docstore.add_documents(nodes, store_text=store_doc_text)
else:
    raise ValueError(f"Invalid docstore strategy: {effective_strategy}")
```

**两种策略的差别精确地是「要不要写 hash 映射」**：`UPSERTS` 需要它（因为下一次按 `ref_doc_id` 查 hash），而 `DUPLICATES_ONLY` 不需要（它每次全量取 `get_all_document_hashes`，那些 hash 是从已存文档里算出来的）。

`store_doc_text` 是一个**存储体积的开关**：关掉时 docstore 只存 id 与 hash、不存正文。这让 docstore 变薄（去重仍可用），代价是「按 id 取回文档正文」不可能。

**`_node_batcher` 是给多进程并行用的分批。**

```
batch_size = max(1, int(len(nodes) / num_batches))
for i in range(0, len(nodes), batch_size):
    yield nodes[i: i + batch_size]
```

`max(1, ...)` 防住「节点数少于批数」时的零批大小。而**分批的数量不精确等于 `num_batches`**——因为 `int(len/num_batches)` 会向下取整，实际批数可能多出一批。这是一个小偏差，只影响并行度而不影响正确性。

## 关键取舍

**源码事实：** 变换缓存的 key 是「节点内容（`metadata_mode=ALL`）+ 变换的 `to_dict()` 序列化」的 sha256，中间必须过 `remove_unstable_values`。

**源码事实：** `remove_unstable_values` 用一条正则删掉 `<... at 0x...>` 形态的内存地址，而不是维护字段白名单——因为不稳定的不是字段名，而是「repr 里带地址」这个特征。这条正则只覆盖这一种形态，且字符类绑定于 Python 地址的形态。

**源码事实：** 缓存命中是**整批粒度**：一个节点的变化让整批重新跑。

**源码事实：** 哈希包含「当前这批节点」，于是**变换在链中的位置被隐式编码**；交换两个变换的顺序会让后一个的哈希变化。

**源码事实：** `run_transformations` 默认 `in_place=True`，即不复制输入序列。

**源码事实：** `DUPLICATES_ONLY` 用两个集合（库里已有 + 本次已见）挡重复，且**边判断边写**（循环内就 `set_document_hash`）。

**源码事实：** `UPSERTS` 按 `ref_doc_id`（缺省回退 `node.id_`）查 hash；文档变了走「先删后加」而不是更新。

**源码事实：** `UPSERTS_AND_DELETE` 用 `get_all_document_hashes().values()` 算差集，而底层映射是 `id → hash`——**两个键空间混用**，`doc_ids_to_delete` 里其实是 hash 值被当成 id 传下去。

**源码事实：** 策略降级用局部 `effective_strategy` 且警告明确声明 `pipeline.docstore_strategy is unchanged`；降级方向选更保守的 `DUPLICATES_ONLY`；`stacklevel=3` 让警告指向调用方。

**源码事实：** 真正的去重要求 `docstore` 与 `vector_store` **两者都在**；只有 docstore 时会降级警告，两者都没有时策略配置无效但无提示。

**源码事实：** `_update_docstore` 的差别精确到「要不要写 hash 映射」；`store_doc_text=False` 让 docstore 只存 id 与 hash。

**架构推论：** 这一页的核心是**「怎么算出稳定的缓存键」**。一个缓存的价值完全取决于 key 的稳定性：key 不稳定则永不命中（白费一层）；key 太粗则命中错的内容（比不缓存更糟）。

而 `remove_unstable_values` 揭示的方法论值得记：**当稳定性问题来自「序列化里嵌了运行期标识」时，逐字段排除是不可行的**（你不知道下一个第三方对象会带什么）。唯一务实的做法是**用模式匹配清掉那类特征**，并接受它覆盖不全——然后靠「缓存命中率」这个可观测指标去发现遗漏。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `get_transformation_hash` | 节点内容 + 变换序列化 → sha256 缓存键 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `remove_unstable_values` | 正则删掉 `<... at 0x...>` 的内存地址 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `run_transformations` | 逐变换查缓存；整批粒度 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `arun_transformations` / `arun_transformations_wrapper` | 异步镜像与进程池入口 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `DocstoreStrategy` | 三档去重策略枚举 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `IngestionPipeline.run` | 主流程：策略降级 → 去重 → 变换 → 落库 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `_handle_duplicates` | 全局 hash 比对，边判断边写 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `_handle_upserts` | 按 `ref_doc_id` 查 hash，变了先删后加 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `_update_docstore` | 两种策略的差别：要不要写 hash 映射 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `_node_batcher` | 多进程分批，`max(1, ...)` 防零批 |

## 参考

- [llama-index 源码：ingestion/pipeline.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/ingestion/pipeline.py)
- [llama-index 文档：摄取流水线](https://docs.llamaindex.ai/en/stable/module_guides/loading/ingestion_pipeline/)
- [Python 文档：hashlib](https://docs.python.org/3/library/hashlib.html)
- [dejavu：基于内容哈希的增量数据处理思路](https://arxiv.org/abs/2207.05222)

## 与主核 / AI 的关系

这套两层去重与主核里**物化视图的增量刷新**是同构问题：数据库也要判断「哪些基础表变了 → 哪个视图需要重算」，而判据同样是「内容哈希 + 上次记录」。差别在于主核靠事务与日志（WAL）保证这个记录的一致性，而这里把记录放在 docstore 里、由调用方承担记录不一致的后果。

`remove_unstable_values` 那一条则有一个更直接的对照：主核的**查询计划缓存**也要对「计划文本」做哈希，而计划里若嵌了指针地址或临时 id，缓存就永不命中。数据库的做法是**只哈希规范化后的结构**（不含地址），而这里的做法是**哈希全部再事后清理**——两者都可行，后者更省事但对新出现的「不稳定形态」不免疫。

对 agent 工程的直接价值有三条。其一，**缓存键的稳定性是缓存能否生效的唯一前提**。序列化里带了内存地址、时间戳、随机 id 的任何一项，都会让命中率归零——而这类失效是静默的。其二，**缓存粒度决定了失效范围**。这里按整批哈希，于是一个节点的变化让整批重跑；要按节点缓存就得把哈希下沉到节点级（但那样「变换之间依赖批内上下文」的情形会失效）。其三，**降级要选更保守的方向并告知**。缺向量库时降级到不删只跳、且明确声明配置未被修改，这个处理是对的——比静默地按原策略跑（然后删了一堆东西）安全得多。
