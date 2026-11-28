---
project: haystack
feature: document-store-contract
title: 文档存储契约与写入策略
page_type: feature
schema_version: 1
refs:
  - path: haystack/document_stores/types/protocol.py
    symbols: [DocumentStore]
  - path: haystack/document_stores/types/policy.py
    symbols: [DuplicatePolicy]
---

# 文档存储契约与写入策略

## 一句话职责

`DocumentStore` 是一个六方法的 `Protocol`，规定了「存文档的后端」必须能做什么；`DuplicatePolicy` 四个值规定了「写一份已存在的文档时怎么办」。两者一起构成了所有后端（内存、Elasticsearch、Pgvector…）必须遵守的最小契约。

## 机制是什么

这个契约的设计约束来自它要覆盖的后端种类差异极大：

| 后端类型 | 能力 |
|---|---|
| 内存字典 | 什么都能做，但没有持久化 |
| Elasticsearch | 全文检索 + 复杂过滤，写入有 `refresh` 延迟 |
| Pgvector | SQL 过滤 + 向量检索 |
| 各种云服务 | 过滤语法受限、批量写入有配额 |

所以契约必须**只包含所有后端都能实现的操作**。最终只有六个：

```python
def to_dict(self) -> dict[str, Any]: ...
@classmethod
def from_dict(cls, data: dict[str, Any]) -> "DocumentStore": ...
def count_documents(self) -> int: ...
def filter_documents(self, filters: dict[str, Any] | None = None) -> list[Document]: ...
def write_documents(self, documents: list[Document], policy: DuplicatePolicy = DuplicatePolicy.NONE) -> int: ...
def delete_documents(self, document_ids: list[str]) -> None: ...
```

四个业务方法（计数、过滤、写入、删除）+ 两个序列化方法。**没有 `get_by_id`**——要按 id 取文档只能走 `filter_documents`。**没有更新**——更新是「带 `OVERWRITE` 的写入」。**没有向量检索**——那是 `EmbeddingRetriever` 的职责，不属于存储契约。

而 `DuplicatePolicy` 的四个值里，第一个是「不表态」：

```python
class DuplicatePolicy(Enum):
    NONE = "none"
    SKIP = "skip"
    OVERWRITE = "overwrite"
    FAIL = "fail"
```

`NONE` 的语义文档写得很实在：「Default policy, behaviour depends on the Document Store」。所以契约**不规定默认行为**——每个后端自己决定。这是「最小契约」的必然代价：既然不能强制统一，就把它变成「由实现决定」。

## 怎么实现的

**`DocumentStore` 是 `Protocol` 而不是 ABC，且没有 `@runtime_checkable`。**

```python
class DocumentStore(Protocol):
    """..."""
```

三处：

1. **`Protocol` 意味着结构化类型**：任何有这六个方法的类都算 `DocumentStore`，不需要继承。所以第三方后端不必依赖 haystack 的基类（虽然实践上它们通常继承一个 `BaseDocumentStore`）。

2. **没有 `@runtime_checkable`** —— 与 c03 里 `Answer` 协议相反（那个有）。所以不能写 `isinstance(store, DocumentStore)`。

   这是一个有意的省略：`runtime_checkable` 只检查方法名存在（不检查签名），所以它给出的保证很弱。Haystack 选择不提供这个检查，让类型检查器（静态）负责。

3. **六个方法都只写 `...`**，没有默认实现。所以这是个纯声明。

**方法的文档字符串承担了契约的实质内容，而 `filter_documents` 那一段最长。**

```
Filters are defined as nested dictionaries that can be of two types:
- Comparison
- Logic

Comparison dictionaries must contain the keys:
- `field`
- `operator`
- `value`

Logic dictionaries must contain the keys:
- `operator`
- `conditions`

The `conditions` key must be a list of dictionaries, either of type Comparison or Logic.

The `operator` value in Comparison dictionaries must be one of:
- `==` `!=` `>` `>=` `<` `<=` `in` `not in`

The `operator` values in Logic dictionaries must be one of:
- `NOT` `OR` `AND`
```

四处：

1. **两种字典靠「有没有 `conditions`」区分**。所以一个既没有 `field` 也没有 `conditions` 的字典是非法输入——但契约没说会怎样（抛错？返回空？）。这个遗漏在实际实现里由 d02 的 `document_matches_filter` 补上（它抛 `ValueError`）。

2. **`field` 的写法是 `"meta.type"`** —— 即**点号路径**。所以 `meta` 那一层是约定前缀。而 `Document` 的其他字段（`content`/`score` 等）理论上也能作为 `field`？文档没说明，只演示了 `meta.` 前缀。实际实现里 `document_matches_filter` 支持两者（d02 会讲）。

3. **比较运算符 8 个**，含 `in`/`not in`（这两个需要 `value` 是列表）。**逻辑运算符 3 个**，全部大写（与比较运算符的符号形式形成对照）。

4. **示例里嵌套了两层**（顶层 `AND` 里含一个 `OR`）。所以「嵌套任意深度」是这个格式的能力，而文档用一个例子就表达了。

**`write_documents` 的返回值语义按策略不同，而文档把这条写清楚了。**

```
:returns: The number of Documents written.
    If `DuplicatePolicy.OVERWRITE` is used, this number is always equal to the number of documents in input.
    If `DuplicatePolicy.SKIP` is used, this number can be lower than the number of documents in the input list.
```

三处：

1. **`OVERWRITE` 时返回值恒等于输入数**（因为每条都写）。
2. **`SKIP` 时可能小于输入数**（跳过的没写）。
3. **`FAIL` 与 `NONE` 的返回值没说明**。`FAIL` 的逻辑是「有重复就抛错」，所以要么全成功（返回值 = 输入数）要么抛错；`NONE` 则是「由后端决定」，所以返回值也可能因后端而变。

这个「返回值随策略变化」的设计有一个实际后果：**调用方不能把返回值当作「处理了多少条」的可靠指标**。它只能当「写进去了多少条」——而 `SKIP` 时那正好是「新增了多少」。

**`delete_documents` 的失败语义是一次全失败。**

```
Fails with `MissingDocumentError` if no document with this id is present in the DocumentStore.
```

注意措辞是「no document **with this id**」——即**任何一个 id 不存在就抛错**。所以删除不是幂等的。而 d03 的 `InMemoryDocumentStore` 的实现是否遵守这一点，是验证契约是否被真正执行的一个具体点。

**`DuplicatePolicy` 是 `Enum` 而不是 `str, Enum`。**

```python
class DuplicatePolicy(Enum):
    NONE = "none"
    SKIP = "skip"
    OVERWRITE = "overwrite"
    FAIL = "fail"
```

**没有继承 `str`**。这与 b01 的 `ConversionStrategy`（也是纯 `Enum`）一致，但与 haystack 里 `ChatRole`（`str, Enum`）、`MetadataMode` 那类可比较的枚举不同。

后果是：`DuplicatePolicy.SKIP == "skip"` 为 **`False`**。所以它**不能**从字符串直接比较，必须走 `DuplicatePolicy("skip")` 或 `DuplicatePolicy["SKIP"]`。

而这正是 `DocumentWriter.from_dict` 做的事：

```python
init_params = data.get("init_parameters", {})
if "policy" in init_params:
    init_params["policy"] = DuplicatePolicy[init_params["policy"]]
```

注意用的是 **`DuplicatePolicy[...]`（按名字查）而不是 `DuplicatePolicy(...)`（按值查）**。而序列化侧写的是名字：

```python
return default_to_dict(self, document_store=self.document_store, policy=self.policy.name)
```

即 `self.policy.name` → `"SKIP"`（大写名字），反序列化用 `DuplicatePolicy["SKIP"]`。**所以存的是枚举成员名，不是值。** 这个选择让存档里出现 `"SKIP"` 而不是 `"skip"`——与值的定义不一致，但往返正确。

**为什么要绕这一圈？** 因为 b02 的 `default_from_dict` 只处理「带 `to_dict` 的对象」与「带 `.` 的类名」，不认识枚举。所以 `DocumentWriter` 必须自己做转换。而 `DuplicatePolicy` 不继承 `str` 意味着**不能直接作为 `init_parameters` 里的值存进 YAML**（`SafeDumper` 会拒绝非基础类型）——所以必须转成字符串。

这里有一处**顺序上的细节**：`from_dict` 先手工转 `policy`，然后调 `default_from_dict`。而 `default_from_dict` 会校验 `type` 与参数名。所以顺序是「先修补参数，再走标准流程」——这依赖 `default_from_dict` 不复制 `data`（它复制的是 `init_parameters` 的浅拷贝，而这里的修改发生在调用之前，所以安全）。

**`NONE` 的语义在具体后端里被解析成 `FAIL`。**

d03 的 `InMemoryDocumentStore.write_documents`：

```python
if policy == DuplicatePolicy.NONE:
    policy = DuplicatePolicy.FAIL
```

所以内存后端把「不表态」理解成「严格」。而另一个后端完全可能把它理解成 `OVERWRITE`（这在 Elasticsearch 上更自然，因为 `index` 操作默认就是覆盖）。

**这就是 `NONE` 的全部内容：它是一个「让后端发挥」的占位值。** 契约不规定它，文档也不承诺它。所以跨后端的代码若依赖 `NONE` 的行为，就是不可移植的。

**`write_documents` 的 `policy` 参数有默认值，而这个默认值把「选择权」留给了后端。**

```python
def write_documents(self, documents: list[Document], policy: DuplicatePolicy = DuplicatePolicy.NONE) -> int:
```

所以**最简单的调用（不传 policy）的行为是完全不确定的**。这与「最小契约」的目标一致（不能强制），但它让「写文档」这个最常用的操作成为最不可预测的。

对比 `DocumentWriter` 组件：它的 `policy` 默认也是 `NONE`，但文档说「relies on the DocumentStore settings」——即**组件层把责任明确地转给了存储层**。

**六个方法里缺了两个在别处存在的操作，这个缺失值得记。**

| 缺失的操作 | 替代方式 | 代价 |
|---|---|---|
| 按 id 取单个文档 | `filter_documents({"field": "id", "operator": "==", "value": id})` | 需要构造过滤器 + 返回列表 |
| 更新文档 | `write_documents([doc], DuplicatePolicy.OVERWRITE)` | 会重写全部字段（无法部分更新） |

**没有 `get_by_id`** 是这组约束里最有意思的一条：它意味着**「按主键查」和「按任意字段查」被统一了**。好处是契约更小；坏处是每个后端都要让 `id` 成为一个可过滤字段（而不能只维护内部主键索引）。d02 的 `document_matches_filter` 里确实特判了 `id` 字段。

**没有 `update`** 则意味着「部分更新」无法表达。所以「只改一个 meta 键」必须先把文档读出来、改、再整条写回——这在并发场景下会丢更新（没有版本检查，没有 CAS）。

## 关键取舍

**源码事实：** `DocumentStore` 是六方法 `Protocol`（`to_dict`/`from_dict`/`count_documents`/`filter_documents`/`write_documents`/`delete_documents`），**不带 `@runtime_checkable`**（与 c03 的 `Answer` 协议相反）。

**源码事实：** 契约里**没有**按 id 取单个文档、没有部分更新、没有向量检索——前者用 `filter_documents` 代替，更新用 `OVERWRITE` 写入代替，向量检索属 retriever 职责。

**源码事实：** 过滤格式的完整规范写在 `filter_documents` 的 docstring 里：两种字典（比较 4 键 / 逻辑 2 键）、8 个比较运算符（含 `in`/`not in`）、3 个大写逻辑运算符、`field` 演示为 `meta.xxx` 点号路径。

**源码事实：** `write_documents` 的返回值语义**随策略变化**：`OVERWRITE` 恒等于输入数，`SKIP` 可能更小；`FAIL`/`NONE` 未说明。

**源码事实：** `delete_documents` 声明「任何一个 id 不存在就抛 `MissingDocumentError`」，即**非幂等**。

**源码事实：** `DuplicatePolicy` 是纯 `Enum`（**不继承 `str`**），故 `DuplicatePolicy.SKIP == "skip"` 为 `False`；也正因如此它不能直接进 YAML，必须序列化成字符串。

**源码事实：** `DocumentWriter` 序列化用 `self.policy.name`（枚举成员名，如 `"SKIP"`），反序列化用 `DuplicatePolicy[...]`（按名查）——所以存档里存的是名字而非值，与 `"none"`/`"skip"` 小写值不一致。

**源码事实：** `DuplicatePolicy.NONE` 的语义契约不规定（「behaviour depends on the Document Store」）；d03 的 `InMemoryDocumentStore` 把它解析成 `FAIL`。

**源码事实：** `write_documents` 的 `policy` 默认 `NONE`，所以「不传 policy 的写入」行为跨后端不可移植。

**架构推论：** 「最小契约」这个方向是对的——后端差异极大时，契约每多一条就多一批实现不了的后端。但它带来一个具体后果：**最常用的操作（写）反而最不可预测**。更稳的设计是让 `NONE` 有一个契约级默认（例如强制等价于 `FAIL`），但那样会打破「内存后端可以宽容、云后端可以覆盖」的既有行为。所以这里的选择是把不确定性显式化（文档写「depends on the Document Store」），而不是假装没有。

**架构推论：** 没有 `get_by_id` 是一个**有意的能力收窄**，它强迫每个后端把 `id` 当作普通可过滤字段。这消除了「主键查询快、过滤查询慢」的不对称，也让「按 id 查」与「按 meta 查」用同一条路径。代价是内存后端也得走一遍过滤器构建——牺牲常数性能换接口统一，在「文档存储」这个量级上是划算的。

**架构推论：** 「没有部分更新」是这套契约里最实质的限制。它让「改一个 meta 键」变成读-改-整写，而中间任何并发写入都会被后写覆盖（lost update）。这是「文档存储」与「数据库」的分界线：后者有事务与版本号，前者只有整条替换。判据是**并发写入的语义要求**：若同一份文档可能被两个管道同时更新，这个契约就不够。

**架构推论：** `DuplicatePolicy` 不继承 `str` 这个选择，与它需要「按名字序列化」直接相关。若继承 `str`，`f"{policy}"` 会得到值（`"skip"`），而 `policy.name` 仍是 `"SKIP"`。两个都可序列化，但会得到不同的存档格式。项目选了「按名字」并在 `DocumentWriter` 里显式转换——这让枚举的「值」只用于内部，不进入持久化格式。这个区分是好的（改值不会破坏存档），但它没有被文档说明。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/document_stores/types/protocol.py` | `DocumentStore` | 六方法 `Protocol` |
| `haystack/document_stores/types/protocol.py` | `DocumentStore.filter_documents` | 过滤格式的完整规范（写在 docstring 里） |
| `haystack/document_stores/types/protocol.py` | `DocumentStore.write_documents` | 写入 + 返回值随策略变化 |
| `haystack/document_stores/types/protocol.py` | `DocumentStore.delete_documents` | 非幂等删除（缺失即抛错） |
| `haystack/document_stores/types/policy.py` | `DuplicatePolicy` | 四值纯 `Enum`，`NONE` 语义不约定 |
| `haystack/components/writers/document_writer.py` | `DocumentWriter.to_dict` | 用 `policy.name` 序列化 |
| `haystack/components/writers/document_writer.py` | `DocumentWriter.from_dict` | 用 `DuplicatePolicy[...]` 按名反查 |
| `haystack/components/writers/document_writer.py` | `DocumentWriter.run` | 运行期 policy 可覆盖构造期 |
| `haystack/document_stores/in_memory/document_store.py` | `write_documents` | 把 `NONE` 解析成 `FAIL` |

## 参考

- [haystack 源码：document_stores/types/protocol.py](https://github.com/deepset-ai/haystack/blob/main/haystack/document_stores/types/protocol.py)
- [haystack 文档：文档存储](https://docs.haystack.deepset.ai/docs/document-store)
- [haystack 文档：元数据过滤](https://docs.haystack.deepset.ai/docs/metadata-filtering)
- [Python 文档：typing.Protocol](https://docs.python.org/3/library/typing.html#typing.Protocol)

## 与主核 / AI 的关系

`DocumentStore` 契约与主核（多模态数据库内核）的**访问方法接口**是同一类设计：定义一组最小操作，让不同实现（堆表、索引扫描、外部表）可互换。但两者的「最小」程度差很远——主核的访问方法有 `beginscan`/`rescan`/`getnext`/`endscan` 加上可选的 `markpos`/`restpos`，且**指针语义严格**（`getnext` 返回的下一个元组必须在同一快照里）。

关键差别是**没有快照隔离**。主核的扫描器持有一个快照（`SnapshotNow`、`xmin`/`xmax`），所以扫描过程中别的事务的写入不影响它。`DocumentStore` 的 `filter_documents` 没有任何一致性保证——每次调用都是「现在的状态」。所以一个「先 `count_documents` 再 `filter_documents`」的操作，两次看到的数据可能不同。这是文档存储与数据库的又一条分界。

第二处对照是 **`DuplicatePolicy.NONE` 与主核的 `ON CONFLICT` 缺省**。主核的 `INSERT` 若遇唯一冲突默认报错（无 `DO NOTHING`/`DO UPDATE`），即「缺省是严格」。而 `DuplicatePolicy.NONE` 明确让缺省不确定。主核这样做是因为**它有能力在缺省时给出确定行为**（有唯一约束、有事务）；haystack 让缺省不确定是因为不同后端的缺省不同（Elasticsearch 的 `index` 天然覆盖）。所以「缺省是否确定」反映的是「框架能不能强制统一行为」。

第三处是**过滤格式的嵌套字典与主核的 `WHERE` 树**。`{"operator": "AND", "conditions": [...]}` 与 PostgreSQL 的 `BoolExpr` 节点是同一个结构（`AND_EXPR`/`OR_EXPR`/`NOT_EXPR` 各有子节点列表）。区别在于主核有**类型检查**（每个比较算子只接受可比类型），而这里的 `field`/`operator`/`value` 三个字符串在契约层不做校验——校验推迟到 `document_matches_filter` 的运行时（d02）。

对 AI 工程的实际意义在于：文档存储契约决定了上层能做什么、不能做什么。这份实现给出的三条经验可直接用：**最小契约要显式承认哪些行为不确定**（`NONE` 的文档写法是对的示范）、**去掉主键查询能让接口更统一但要付常数代价**、以及**没有版本检查的整条替换会在并发下丢更新**（需要这部分能力的场景必须换存储）。
