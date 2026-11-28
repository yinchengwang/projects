---
project: lightrag
feature: storage-contracts
title: 四类存储契约与命名空间
page_type: feature
schema_version: 1
refs:
  - path: lightrag/base.py
    symbols: [StorageNameSpace, BaseKVStorage, BaseGraphStorage, BaseVectorStorage]
  - path: lightrag/namespace.py
    symbols: [NameSpace, is_namespace]
  - path: lightrag/kg/factory.py
    symbols: [get_storage_class]
---

# 四类存储契约与命名空间

## 一句话职责

定义键值、向量、图、文档状态四类存储的公共契约（初始化、提交、丢弃、清空），并用「命名空间 + 工作区」两个正交维度把一份逻辑数据映射到任意物理后端上。

## 机制是什么

lightrag 没有事务。它把一份文档拆成记录写进四个互相独立的存储：文本块进键值存储、实体与关系的嵌入进向量存储、图谱结构进图存储、处理进度进文档状态存储。**这四处写入之间没有任何原子性保证**——这是整套契约的根本前提，也是理解每个方法签名为什么要长成这样子的钥匙。

既然没有事务，契约就必须承担两件平时由事务承担的事：

1. **让残留在已知位置。** 写入顺序、缓冲、提交回调（`index_done_callback`）都是为了让「一半写成功」的状态是可预测的、能被下一次运行修好的。所以基类里有 `drop_pending_index_ops`、`drop_pending_upserts`、`has_pending_index_ops` 这一类「你在丢弃前必须回答」的方法。
2. **让「不知道」和「没有」可区分。** 大量方法有两个版本：宽松版与严格版（`get_docs_by_statuses(strict=...)`、`get_by_id` 与 `get_by_id_strict`）。严格版把「结果完整」当成契约的一部分，因为调用方一旦消费了唤醒信号，就再也不会重读。

四类存储共享一个基类 `StorageNameSpace`（`namespace` + `workspace` + `global_config` 三个字段），这是它们唯一的共同点。往下就分家：各自有完全不同的方法集与不变量。

## 怎么实现的

**`StorageNameSpace` 只定义四件事。** `initialize` / `finalize` 是空实现的生命周期钩子；`index_done_callback` 与 `drop` 是抽象方法。其余方法（`drop_pending_*`、`has_pending_index_ops`）都有默认实现——默认值本身就是契约声明：基类默认「无缓冲、无待提交」，所以**只有真正会缓冲的后端才需要覆盖**。

`index_done_callback` 的文档字符串值得逐句读，它要求的是一个**异常分类义务**：抛出异常时必须说明「是否有持久引用可能已经丢了」。能证明这次失败什么都没丢的后端应抛 `ReferencesIntactFlushError`；其他任何异常都让调用方假定有操作被丢掉，从而进入「隔离所有指向它的记录」这条更昂贵的路径。它还补了一句更严格的：必须为**整个 flush** 证明，因为「丢了一个永久拒绝的操作、同时保留了一个可重试的」就已经丢了引用。

`has_pending_index_ops` 的默认返回值 `False` 被文档明确标注为「对延迟写入后端是未实现的答案而不是真相」，并要求在这种后端上先实现再调用——「在非空缓冲上给出一个自信的 False 比没有这个方法更糟」。

**`drop` 返回字典而非抛异常。** 返回 `{"status": ..., "message": ...}` 而不是抛，是因为「清空」这个操作要通知其他进程、要删文件、要跨多个后端的容器，逐个失败点都抛出会让调用方无法继续清理剩下的。代价是**失败被降级成返回值**，调用方必须逐条检查。

**`BaseVectorStorage` 多两个类级契约。** `requires_embedding_func = True` 是能力位：不物化向量的后端可以置 False，`_validate_embedding_func` 据此决定是否在 `__post_init__` 里报错。`meta_fields` 决定哪些元数据要随向量一起被过滤。

向量存储还有一个独有的问题：**容器名要编码嵌入模型**。`_collection_suffix_for` 把模型名与维度折成 `text_embedding_3_large_3072d` 这样的后缀，`_generate_collection_suffix` 调它。理由是换嵌入模型意味着换向量空间，新旧向量不能共存于同一容器，所以容器名必须能区分。`model_container_name` 与 `drop_sibling_models` / `drop_all_models` 是这条规则的延伸：清空工作区时要连**其他模型**的容器一起清，否则下次启动会被遗留容器「收养」，让旧向量污染新空间。而清兄弟容器要求**所有权证据**（`unvouched_sibling_containers` / `vouched` 参数），「按名字模式猜」被明确禁止。

**`BaseGraphStorage` 的方法集分三档。** 单条查询（`has_node` / `get_node` / `get_edge` / `node_degree`）、批量查询（`get_nodes_batch` / `get_edges_batch` / `node_degrees_batch` / `edge_degrees_batch` / `has_nodes_batch`）、以及**迭代**（`iter_labels` / `iter_edges`，带 `batch_size`）。迭代方法的存在说明「一次性取全部节点」在后端上是不可接受的——大图谱必须流式扫。`get_popular_labels` / `search_labels` 是给前端补全用的，文档写明「空列表表示没有匹配，后端错误必须抛而不是返回空列表」。

**`BaseKVStorage` 的 `filter_keys` 是去重的实现基础。** 传一组键、返回**还不存在**的那些——语义是「差集」而不是「存在性检查」。这样调用方一次往返就能知道要写哪些，而不必逐键查。

**`DocStatusStorage` 用可实例化性当能力保证。** 文档写明：所有后端都必须实现有界调度与严格读取 API（`get_docs_by_statuses_page` / `get_docs_by_ids` / `resolve_doc_source_strict`），它们是抽象方法，所以**缺任何一个的后端根本无法实例化**——没有降级路径、没有能力位。

**命名空间是编译期常量，工作区是运行期参数。** `NameSpace` 里 12 个常量（7 个键值、3 个向量、1 个图、1 个文档状态）加上一个 `config`，注释第一句就是「所有命名空间都不应被改动」——因为它们是**磁盘上的目录名与数据库里的容器名前缀**，改名等于丢数据。`is_namespace` 用 `endswith` 判断，因为后端会把工作区拼在命名空间前后，形成 `kv_store_full_docs_<workspace>` 这类复合名。

**配置存储被刻意排除在数据命名空间之外。** `KV_STORE_CONFIG` 单独一个固定容器（`CONFIG_CONTAINER_TAG = "_lightrag_config"`），注释写明「它不是一个工作区」：没有校验、不能被 `*_WORKSPACE` 环境变量重映射、调用方不能选。前导下划线让它在一堆租户容器里一眼能认出是自己的。这是一个很实在的设计——**配置是元数据，跟着被配置的数据走会导致「配置本身丢了就再也读不到配置」的死循环**。

**`get_storage_class` 是唯一的后端解析入口。** 配置里写的是后端类名字符串，由它解析成类。这条路径是有意收窄的：注册表在 `kg/__init__.py`（`STORAGE_IMPLEMENTATIONS`），工厂只做名字到类的映射，不接受任意 import。

## 关键取舍

**源码事实：** 四类存储无跨存储事务。基类提供 `drop_pending_index_ops` / `drop_pending_upserts` / `has_pending_index_ops` 三个「丢弃前必须回答」的方法，默认实现对应「无缓冲」后端。

**源码事实：** `index_done_callback` 要求异常携带分类信息：能证明没丢引用的后端抛 `ReferencesIntactFlushError`，其他异常让调用方假定有丢失。且必须为整个 flush 证明，不能逐项。

**源码事实：** `has_pending_index_ops` 默认 `False` 被文档标注为「对延迟写入后端是未实现而非真相」，要求先实现再调用。

**源码事实：** `DocStatusStorage` 把「有界调度 + 严格读取」三个方法设为抽象方法，缺一不可实例化——用可实例化性代替能力位。

**源码事实：** 向量存储的容器名编码嵌入模型（`_collection_suffix_for`），清空工作区时须连兄弟模型的容器一起清，且要求所有权证据。

**源码事实：** `NameSpace` 的 12 个常量注释标注「不应改动」，因为它们是物理容器名。配置存储用独立的固定容器 `_lightrag_config`，注释明确「它不是工作区」。

**架构推论：** 没有事务时，契约的职责从「保证正确」变成「让错误可定位、可自愈」。这解释了为什么基类里那么多方法在讨论「丢弃之后会怎样」——**它们不是在管理数据，是在管理不确定性**。这也是它比其他 RAG 框架的存储抽象厚得多的原因：那些框架大多假设「写完就成功了」。

**架构推论：** 「严格读 vs 宽松读」成对出现，是因为控制面与展示面对完整性的要求相反。展示面宁可少显示一条也不能整个页面报错；调度面宁可报错也不能漏掉一条——因为漏掉的那条再也不会被重读（唤醒信号已经被消费）。**同一份数据的两个消费者，可靠性方向相反**，这是分布式系统里典型的双契约需求。

**架构推论：** 容器名编码模型是一道防线的具体形态：它把「向量空间错配」从**运行时语义错误**（检索到无关结果，难发现）转成**命名冲突**（容器不存在，立刻发现）。同类手法在别处也出现——用命名而非校验来防错，因为命名错误是显式的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/base.py` | `StorageNameSpace` | 四类存储的公共基类与生命周期 |
| `lightrag/base.py` | `BaseKVStorage` | 键值存储契约（含 `filter_keys` 差集） |
| `lightrag/base.py` | `BaseVectorStorage` | 向量存储契约与容器命名 |
| `lightrag/base.py` | `BaseGraphStorage` | 图存储契约与批量/迭代接口 |
| `lightrag/base.py` | `DocStatusStorage` | 文档状态存储的调度与严格读 |
| `lightrag/base.py` | `_collection_suffix_for` | 容器后缀折叠（模型名 + 维度） |
| `lightrag/namespace.py` | `NameSpace` | 12 个物理命名空间常量 |
| `lightrag/namespace.py` | `CONFIG_CONTAINER_TAG` | 配置容器的固定标签 |
| `lightrag/namespace.py` | `is_namespace` | 复合名后缀判定 |
| `lightrag/kg/factory.py` | `get_storage_class` | 后端类名解析 |

## 参考

- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [lightrag 文档](https://github.com/HKUDS/LightRAG/blob/main/README.md)
- [lightrag 源码：base.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/base.py)
- [lightrag 源码：namespace.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/namespace.py)

## 与主核 / AI 的关系

lightrag 的存储契约与主核（多模态数据库内核）的存储层在**同一个问题上给出了相反的答案**：多后端可插拔 vs 单一自研内核。

**契约厚度与事务的关系。** 主核有 WAL 与事务，所以它的存储接口可以很简单——「写成功」就是成功了，不需要 `has_pending_index_ops` 这种问题。lightrag 没有事务，于是契约必须把「可能没写成功」这件事显式建模，接口因此厚了三四倍。**接口复杂度是可靠性保证的替代品而不是补充品**：保证越弱，契约必须越啰嗦。这条关系在所有分布式存储里都成立。

**「严格读 vs 宽松读」与数据库的读已提交。** 数据库用隔离级别统一描述读能看到什么；lightrag 没有隔离级别，只能在每个读方法上手工标注两种模式。差别在于数据库把一致性做成**全局属性**（一个事务内所有读一致），lightrag 做成**每个调用点的局部选择**。前者更强、后者更灵活，但后者的正确性依赖每个调用点选对——这是一个把责任从系统下移到调用方的典型取舍。

**容器名编码模型 vs 数据库的表空间隔离。** 数据库换存储格式或编码时需要显式迁移，工具化且可见；lightrag 把模型指纹塞进容器名，于是换模型自动隔离，但也自动积累了孤儿容器（`drop_sibling_models` 就是为了收它们）。**用命名做隔离的代价是清理责任转移给了调用方**，这是主核里不需要处理的负担——因为主核的表空间是管理员显式管理的。

**文档状态存储的游标设计**值得单独比：`CursorPosition` 的三态（`CURSOR_START` / `CursorAfter` / `CURSOR_END`）且文档写明「`next_position is CURSOR_END` 是唯一终止信号，空 `docs` 不是」——这与数据库 keyset 分页的经典陷阱完全一致（部分过滤的页可能是空的但没扫完）。主核的 B-tree 区间扫也面对同一问题，解法相同：**用位置而不是用结果判断结束**。
