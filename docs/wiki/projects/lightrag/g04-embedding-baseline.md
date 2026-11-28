---
project: lightrag
feature: embedding-baseline
title: 嵌入空间校验与存储迁移
page_type: feature
schema_version: 1
refs:
  - path: lightrag/vector_space_gate.py
    symbols: [_probe_same_embedding_space, _cosine, StartupEvidence]
  - path: lightrag/vector_space_gate.py
    symbols: [graph_has_nodes, graph_has_edges, _vectors_are_expected]
  - path: lightrag/storage_migrations.py
    symbols: [_StorageMigrationMixin, _migration_chunk_ids]
---

# 嵌入空间校验与存储迁移

## 一句话职责

在启动时做两件**没有单个存储能独立做**的判断：一是**覆盖度**（「索引的数据不为空，但向量容器是空的」是不是故障），二是**归属**（容器里那些没有模型标记的向量，是不是当前这个模型写出来的）——第二个判断靠**抽样重嵌入 + 余弦比较**取证，而不是靠标记的信任。

## 机制是什么

模块 docstring 的第一句就把「为什么这层在 `LightRAG.initialize_storages()` 里而不是某个后端的 `initialize()` 里」说清楚了：

> 两者都问一个跨图谱与向量存储的问题，这就是为什么它们住在上一层。**一个向量存储无法自己判断『我是空的』是不是故障**：那取决于它**索引**的数据，而那份数据在完全另一个存储里。

**覆盖度栅栏**要抓的形状是「跨存储的」：**图里有实体而向量库里没有**。docstring 把能导致这个形状的原因列成了一个家族：

> 一个容器被删了或一个卷没挂上、一次重建跑到一半停了、向量后端换了但数据没迁、一个工作区前缀与写向量时用的那个不匹配——以及在把嵌入模型编码进**容器名**的后端上（Milvus、Qdrant、PostgreSQL），一次模型切换，它会开出一个新的、空的、名字正确的新容器，而**任何标记都永远无法在它上面触发**。

最后那句是关键：模型切换会造出一个「看起来完全正常的空容器」，所以标记类检查对它无效，而这个栅栏恰好通过它的后果（空索引）抓到了它。

而 docstring 明确说这条检查**自清除**：「一次重建通过填充容器回答了这个问题，这就是为什么这个栅栏每次启动都重新问而不是记录任何东西」。

**归属探针**要解决的则是另一个问题：所有早于标记机制的容器都不记模型，而「缺失的证据永不拒绝」，所以这份沉默不会自己结束。它必须靠**采纳（adoption）**结束——但不能盲采：

> 一个在一步里既升级又切到同维度模型的运维者，否则会得到『新模型的名字被盖在旧模型的向量上』，永久地，之后栅栏再也不会触发。所以采纳携带证据：用当前模型重新嵌入几个已存的 `content` 字段，并与存放在它们旁边的向量比较。

**两个检查的目标维度也不同，而 docstring 强调了它们互不替代**：

> 这个栅栏自己的问题仍然是**覆盖度**，两者互不替代。

而探针是**按目标（per target）**跑的，理由是同一条：

> 三个目标共享一个 `embedding_func` 但不共享历史：`lightrag-rebuild-vdb` 分别重建它们，所以一次同维度模型切换后中断的重建可以让实体处于当前空间，而关系或块仍持有上一个模型的向量。**关于一个容器的裁决只是关于那个容器的证据**；从不基于兄弟的裁决采纳或记录基线。

## 怎么实现的

**`StartupEvidence` 是「什么算证据」的显式类型，而它的 docstring 把三种「没有证据」都编码成了 `None`。**

```python
source_populated: dict[str, bool | None] = field(default_factory=dict)
index_empty: dict[str, bool | None] = field(default_factory=dict)
probes: dict[str, bool | None] = field(default_factory=dict)
probe_details: dict[str, str] = field(default_factory=dict)
```

docstring 三条规则：

1. **`source_populated` 回答「这个目标所索引的数据有没有东西」**：`True` / `False`，或 `None`（源读不了，或这一对没被检查）。
2. **`probes` 是采纳探针的裁决**：`True` 表示存着的向量在当前模型下被复现了，`None` 表示探针没跑或答不出（`probe_details` 说为什么）。**负裁决从不存进这里——它抛 `VectorSpaceMismatchError`，因为一次拒绝不是可以记录任何东西的证据。**
3. 「只有正裁决可以为**那个**目标建立一个 `origin=probe` 基线，且只有一个 `False` 的源加上确认也为空的容器（`index_empty`）才能建立一个 `origin=empty` 的。**任何地方的 `None` 都表示『没有证据』，而没有证据就什么都不记录。**」

第 2 条那句「负裁决不在这里」是一个很精确的类型决定：如果负裁决也存成一个布尔，调用方就得再判一次「这个 `False` 是要记录还是不要记录」——而把这个不对称写进类型（负裁决走异常）就消掉了这个判断点。

**「只有跑过并返回负裁决的探针才能拒绝」是全模块的主导不变量，而 docstring 把它写成了一句可检验的规则加一串穷举。**

> 只有跑过并返回负裁决的探针才能拒绝。**每一形式的『跑不了』都回退到升级前的行为。**

然后逐个列举：

> 一个挂掉的嵌入 provider、一次超时、一个读不了的源存储、一个答不出问题的索引、一次被权限拒绝的标记写入——**所有这些都让容器保持未标记、实例继续服务，而那正是这个特性存在之前 LightRAG 拥有的检测能力**。

所以这条不变量的实际作用是**保证这个安全特性永远不会成为可用性风险**。任何「检查失败就拒绝启动」的设计在生产里都会被关掉；而「检查失败就退回旧行为」则可以被长期开着。

**`_cosine` 返回 `None` 而不是 0.0，而理由直接指向这条不变量。**

```python
if left.size == 0 or left.size != right.size:
    return None
denominator = float(np.linalg.norm(left) * np.linalg.norm(right))
if not denominator or not np.isfinite(denominator):
    return None
similarity = float(np.dot(left, right) / denominator)
if not np.isfinite(similarity):
    return None
return similarity
```

docstring：「长度不匹配或零模向量都返回 `None`：两者都意味着比较什么都没说，而**在那里静默返回 0.0 会被读成『模型变了』的最强可能证据**」。

这是一处极易写错的地方：余弦公式在退化输入上的自然结果就是 0（或 `nan`），而 0 在这个判据里恰好是「最不像」。所以退化输入必须走「无信息」这条路而不是「数值」那条路。

**三个阈值是一组，而每个都有明确的语义与理由。**

```python
ADOPT_COSINE = 0.95
REFUSE_COSINE = 0.70
PROBE_ROWS = 8
```

`ADOPT_COSINE`：「采纳需要近乎同一。同一个模型重新嵌入同一段文本落在 ~1.0；到不同模型的差距很宽（通常 0.0-0.5），所以 halfvec 量化与归一化差异都不会把同模型的结果推到这之下」。

`REFUSE_COSINE`：「……而拒绝需要那个差距的另一端，不只是『不是采纳』。**两者之间的带子是裁决两头都不可信的区间**——异常短的 content 字段、一个静默截断了的 provider、同一个基座模型的微调版。上面的不变量让那个带子成为**不确定**而不是拒绝：在一个活的部署上用含糊的证据失败，比它本来就有的未标记容器更糟」。

所以是三值判定：`≤0.70` 拒绝、`≥0.95` 采纳、中间不确定。而「不确定」这个第三值的存在理由被写得很实在：**假阳性（错误拒绝）的代价是停机，而停机比「未标记容器」这个已有状态更糟**。

`PROBE_ROWS = 8` 的理由最长，而它描述了一个可到达的真实场景：

> **一个是不够的**，而证明这一点的场景今天就可到达：一次既升级又切到同维度模型的迁移在嵌入器不可达时以未标记状态开始（不确定），继续服务，然后接受新写入——于是容器最终同时持有上一个模型的向量**和**这个模型的。**从那个混合物里挑出的一行会随便落在哪个上并因此采纳，而盖在混合容器上的标记会永久藏起异己的那一半。**

> 比较几行把那个混合物从「采纳会掩盖的东西」变成「探针会报出的东西」：一行失败而其他行通过，是对这个容器的**正证据**，不是一处含糊。

最后一句是本页最有价值的技术判断之一：**「多数通过」在混合容器上不是「大致通过」，而是「容器不均匀」的确证**。所以 `_probe_same_embedding_space` 的规则是「采纳需要每个被比较的记录都一致」，而实现里那个提前返回正是它：

```python
if similarity <= REFUSE_COSINE:
    if adopted:
        return False, (
            f"cosine {similarity:.3f} against record '{row_id}' while "
            f"{len(adopted)} other sampled record(s) reproduced their "
            f"vectors, so this container holds vectors from more than "
            f"one embedding space"
        )
    return False, f"cosine {similarity:.3f} against record '{row_id}'"
```

注意两个 `return False` 的消息不同：**有已采纳行时**消息里明说「这个容器持有来自一个以上嵌入空间的向量」；没有时只说余弦值。所以同一个负裁决被分成了「模型变了」与「容器混合了」两种诊断——而后者是更严重也更容易被误判为前者的情况。

**`context="document"` 这一行是一个容易漏但很实在的细节。**

```python
fresh = await asyncio.wait_for(
    embedding_func([content for _, content, _ in candidates], context="document"),
    timeout=_probe_timeout(),
)
```

注释：「一次调用覆盖它们全部：成本是往返，不是文本数。`context="document"` 与每个后端写入时嵌入的方式一致；**一个对查询加不同前缀的 provider 否则会被拿去与它根本不会写出的向量比较**」。

所以这个参数的选择不是为了「更准确」而是一个**正确性前提**：某些嵌入模型对 query 与 document 用不同前缀（或不同指令），如果探针用默认/query 上下文嵌入，得到的向量天然离已存向量很远，于是「同模型」会被判成「不同模型」。

**探针的采样与「一行可用都没有」的处理。**

```python
for row in rows:
    row_id = row.get("id")
    content = row.get("content")
    if not row_id or not isinstance(content, str) or not content.strip():
        continue
    stored = vectors.get(row_id)
    if stored is None:
        continue
    candidates.append((row_id, content, stored))
    if len(candidates) >= PROBE_ROWS:
        break
if not candidates:
    return None, "no sampled record carried both a content field and a vector"
```

三道过滤：没有 id、content 不是非空字符串、没有对应向量。而「一条都没有」返回 `None` 而不是 `False`——回到那条不变量：**没有可用样本 ≠ 空间不对**。

**超时是一个独立的失败类别，而它的理由与「检查不能拖慢启动」直接相关。**

```python
_PROBE_TIMEOUT_ENV = "LIGHTRAG_VECTOR_SPACE_PROBE_TIMEOUT"
DEFAULT_PROBE_TIMEOUT = 30.0
```

docstring：「有界是因为 provider 会重试：`@retry(stop_after_attempt(3), wait_exponential(min=4, max=10))` 意味着一个不可达端点能烧掉 30-60 秒，而**一个让每次启动都延迟一分钟的安全特性本身就是一种可用性成本**。一次超时只是探针跑不了的另一种方式，所以它和其他情况一样落到不确定」。

而 `_probe_timeout()` 对非法配置的处理也一致：非数字 → 警告并用默认；非正数 → 警告并用默认。**从不因为配置写错而抛**——同样的理由：这是安全检查，不是配置校验器。

**`graph_has_nodes` / `graph_has_edges` 这两个看起来很平常的函数，各自选了一个不同的 API，而理由值得记。**

`graph_has_nodes`：

```python
return bool(await graph.get_popular_labels(limit=1))
```

docstring：「用 `get_popular_labels` 而不是 `iter_labels`：**它在 `BaseGraphStorage` 上是抽象方法，所以每个后端都实现了它**，而 `iter_labels` 在从没实现过它的后端上是 fail-closed 的」。

`graph_has_edges`：

```python
iterator = graph.iter_edges(batch_size=1)
try:
    async for batch in iterator:
        return bool(batch)
    return False
finally:
    aclose = getattr(iterator, "aclose", None)
    if aclose is not None:
        await aclose()
```

docstring：「`iter_edges` 是基类上**唯一有界的边读器**（`get_all_edges` 会物化整张图）。它默认是 fail-closed 的——一个从没实现它的后端抛 `StorageCapabilityError`，调用方把它读作『答不出』并跳过。**所有七个树内图后端都实现了它**」。

两个函数合起来说明同一件事：**「有没有节点/边」这个问题在不同后端上要用不同的原语回答，而选择标准是「这个原语是不是每个后端都必须实现的」**。节点用「大家都实现了的那个」，边用「有界的那个」——因为边上没有既有界又通用的选择。

而它们都带一句「Public, not gate-internal：`lightrag/tools/clear_storage.py` 在它的删除前摘要里显示同样的裁决」。所以这两个判断被两处消费，因此是公开的。

`graph_has_edges` 的 `aclose` 处理也值得记：它在 `finally` 里尽力关闭异步迭代器，且用 `getattr(iterator, "aclose", None)` 容错——因为「返回一个异步迭代器的函数」不保证返回的是真异步生成器。

**`_vectors_are_expected` 是覆盖度栅栏的核心判据，而它的 docstring 覆盖了「两种情况都表示没有未完成工作」以及一个重要的实现选择。**

第一种：「**每个文档都完成了**。PROCESSED 是流水线对『它写完了那些文档产出的一切，包括向量』的声称」。

第二种更微妙：「**根本没有文档**。`acreate_entity` 与 `ainsert_custom_kg` 写图谱实体**和**它们的向量，且**不写 doc-status 行**。要求一个 PROCESSED 文档会让这样的工作区永远被豁免——而**那是唯一一处『后续流水线运行会修好它』这句话干脆为假的地方**，因为没有任何流水线运行会重新创建运维者手工创建的对象」。

第二种情况是这一页最实在的一个发现：手工创建的对象没有归属文档，所以「等下次运行修」这个通用兜底对它们完全无效，因此必须**连同文档计数一起**判——「没有任何未完成文档」而不是「有已完成的文档」。

而实现里那个**从枚举推导**的写法值得单独看：

```python
unfinished_statuses = [status for status in DocStatus if status is not DocStatus.PROCESSED]
```

注释：「这个列表是**从 `DocStatus` 推导**而不是写出来的，对后来新增的状态而言那是安全的方向：新成员算作未完成，只能**抑制**一次拒绝，永远不会**使能**一次」。

这条是「fail-safe 的默认方向」的范例：用「排除法」而不是「穷举法」表达「未完成」，于是未来新增状态自动落入安全侧。若写成 `[PENDING, PARSING, ANALYZING, ...]` 的显式列表，新状态会默认「不算未完成」，从而可能使一次错误的拒绝。

而 `count_docs_by_statuses(strict=True)` 的选择同样带理由：

> 用 `count_docs_by_statuses(strict=True)` 计数，绝不用 `get_status_counts()`：后者有文档说明会**吞掉它的错误**并报告它设法收集到的部分，所以一次中途失败的 doc-status 读可能显示一行 PROCESSED 而漏掉解释一切的 PENDING 行——**并拒绝那次本会治好它的重启**。严格计数会抛，而一个抛回答『不要拒绝』，与本模块查询的其他每个读不出的东西一样。

所以这里有一条贯穿全模块的映射：**「读不出」→「不要拒绝」**。而 `get_status_counts()` 的「吞错」让「读不出」伪装成「读到了，且没有 PENDING」——那是最危险的一种伪装。

**`_source_is_populated` 把 `None` 与 `False` 的区分保下来，理由一半是日志一半是基线。**

```python
try:
    return await probe()
except Exception as e:
    logger.warning(f"Skipping the coverage check for {name}: the source data could not be read ({type(e).__name__}: {e})")
    return None
```

docstring：「`None` 与 `False` 被调用方同样处理，但在这里被分开保存，好让日志说清是哪一个。两者都不能拒绝：一个空的源不提问题，一个读不了的源不提供证据」。

「但区分对裁决之外也重要，因为那个裁决同时喂给嵌入基线：**`False` 把配置的模型记成那个目标的 `origin=empty` 基线，`None` 什么都不记**。所以一个探针**必须只在一次失败会抛的读上回答 `False`**。`BaseKVStorage.is_empty` 不符合这一点——它捕获自己的错误并回答 `True`——这就是为什么块源要通过 `chunk_source_is_populated` 的严格模式读」。

这段指出了一个跨页的坑：`is_empty` 在 KV 上的实现是「吞错返回 True」，所以在覆盖度栅栏里用它会让「存储挂了」被读成「源是空的」，从而**跳过整个检查**（因为空的源不提问题）。这正是 `_index_is_empty` 那段 docstring 里说的同一件事的镜像：

> 与这些后端上的其他每个读不同，`is_empty` 被规定为**在后端失败时抛**而不是报告未命中，而这正是它存在的原因：`get_by_ids` 捕获它的传输错误并返回空列表，所以一次故障与一个空容器以同一个值抵达。**这里它们不。**

所以同一组 API 里有两个 `is_empty`：KV 的吞错（因为调用方大多想「没数据就继续」），向量存储的抛错（因为覆盖度栅栏需要区分）。**同名不同契约，用途不同。**

**`_PairingGate.check` 是裁决的最终装配处，而它的分支顺序是一条优先级链。**

```python
if vdb is None: return None
if not getattr(vdb, "persists_vectors", True): return None    # 设计如此，不是故障
populated = await _source_is_populated(name, source_probe)
if populated is not True: return populated                    # 源空或读不出 → 不问
if await _index_is_empty(name, vdb) is not True: return True   # 索引非空或读不出 → 不拒绝
if self._expect_empty: return True                            # 调用方即将重建
if not await self._are_vectors_expected(): ...                # 有未完成文档 → 再查「有没有东西治不好」
if vectorless_by_design_probe is not None and (await vectorless_by_design_probe()): return True
raise VectorStorageEmptyError(...)
```

五道豁免，每道都有独立理由：

1. **`persists_vectors = False`**：「`NoopVectorDBStorage` 和其他声明不保留向量的东西：空是它的设计而不是故障。**只跑图摄取是一个受支持的配置**，没有这一条栅栏会在它的第一个文档之后拒绝每次重启」。而且注释点明「`rebuilding_vector_storage` 在那里不是答案：那样的部署没有在重建任何东西」。
2. **`expect_empty`**：「调用方拥有随后的重新填充——一次进程内重建，或从 `NoopVectorDBStorage` 切到真向量后端的受支持切换。**在这里拒绝会挡住唯一能清除该条件的东西**」。
3. **有未完成文档**：「一个索引落后于它的源、而某个文档还在飞，是**会自愈**的残留……`AGENTS.md` 的 *Consistency without transactions* 明确说这种状态不能被升级——而在这里升级意味着**拒绝启动那个下一次运行就会修好它的进程**」。
4. **但有未完成文档还不够**，还要过 `_nothing_will_heal`：「那个豁免是**工作区级**的，而它所开脱的证据是**按容器**的：一个未完成的文档解释的是那个文档产出的对象的缺失向量，别的什么都不是。当源里还持有没有任何文档产出的对象时，没有重试会重新创建它们，所以豁免到不了它们那里，拒绝成立」。注释里还点出了错误的形状：**工作区级豁免** 加 **容器级证据**，是一个可被利用的不对称。
5. **`vectorless_by_design_probe`**：「最后一个查，只在拒绝否则已决定时：`True` 意味着源里的对象从来没有向量，所以空索引不是损失」。

而它给的日志都很具体。第 4 道豁免成功时：「这个工作区有未完成的文档。继续服务：一次尚未写完向量的摄取由下一次流水线运行修复，而不是由一次拒绝」。失败时：「这个工作区有未完成文档，但它也持有没有任何未完成文档会重写的对象，所以没有流水线运行能重建它们的向量。**拒绝**」。

**`_nothing_will_heal` 的采样预算有一条注释解释为什么「采样」在这里是可接受的。**

```python
DOCUMENTLESS_SAMPLE_SIZE = 32
```

注释：「有多少图对象要追溯回它们的所有者文档。有界因为每个都要读载荷而不只是名字，而**在这里采样是可靠的：一次未命中只会让检查不拒绝**（见 `_nothing_will_heal`）。只在那个本来就打算豁免的分支上付出，从不在健康启动上付」。

所以这里的采样不像 `PROBE_ROWS` 那样要求「全部一致」——因为它的失败方向是「不拒绝」，而漏掉一个「无主对象」的代价只是少抓一次故障，不是错误拒绝。**采样密度取决于「漏掉一个的代价」。**

**`_probe_target` 与覆盖度栅栏分开跑，而理由是两个问题问的是不同的东西。**

它接一个 `force` 参数，与 `baseline_targets` 相连：为了建立一个持久基线，**即使容器自己的标记不需要采纳也要跑探针**（在那些没有标记机制的后端上，这是探针唯一会跑的方式）。而在 `check_vector_space_at_startup` 里，`baseline_targets` 还联动另外两件事：

- 「它的源被**严格**读，好让『空』不能来自一次失败的读」；
- 「它的容器被问是否为空」。

而不在 `baseline_targets` 里的目标不为这些付任何代价——「这就是让此后每次启动都留在栅栏一直用的那次读上的原因」。

所以这个参数的设计是「**只为将要写持久声明的东西付取证成本**」。持久声明（基线）可能存活很多次启动，所以它必须建立在严格证据上；而一次性的覆盖度检查可以接受宽松读数。

**`storage_migrations.py` 的两件事与嵌入空间无关，但同属「启动时的数据一致性检查」。**

模块 docstring 列了两条迁移：

- 当 `full_entities` / `full_relations` 为空时，从 doc_status 历史与每个对象的 chunk 追踪**回填**它们（图 `source_id` 只在追踪缺失时用）。
- 当 `entity_chunks` / `relation_chunks` 为空时，通过遍历图节点/边**重建**这两个索引。

而 `check_and_migrate_data` 的门控逻辑很值得读，因为它两处都用了 `strict=True` 并写明理由。

**第一处：**

```python
processed_docs = await self.doc_status.get_docs_by_statuses([DocStatus.PROCESSED], strict=True)
```

注释：「`strict=True`：**这个映射是唯一决定哪些文档得到恢复锚点的输入**。一行解析不了必须中止迁移而不是被跳过——一个部分映射会为它的兄弟们写锚点，之后『锚点已存在』的检查会短路此后每次启动，于是被省略的那个文档永远拿不到锚点，而对它的每次清除都永久 fail closed（409）」。

这段描述了一个**错误如何被固化**的完整链条：部分成功 → 存在性检查短路 → 缺失部分永远补不上 → 相关操作永久失败。而关键在于「存在性检查」这种常见的幂等优化正是固化的机制：它把「部分完成」误判为「已完成」。

**第二处（`_migration_chunk_ids`）：**

```python
if storage is not None:
    if not getattr(storage, "supports_strict_point_reads", False):
        raise StorageCapabilityError("Legacy recovery-anchor migration requires strict chunk-tracking reads")
    row = await storage.get_by_id_strict(key)
    if has_chunk_tracking_row(row):
        return row["chunk_ids"]
return (graph_sources or "").split(GRAPH_FIELD_SEP)
```

docstring 三条：

1. 「**先读权威追踪再读图的有限溯源视图**」。
2. 「一个存在但为空的追踪行也是权威的」。
3. 「**一次读失败必须在中止在任何恢复锚点被写出之前**；相信一次尽力而为的未命中可能持久化不完整的锚点，而那之后会授权一次不完整的清除」。

第 3 条是这一页最严重的一条：不完整的锚点会**授权不完整的清除**——意思是一次「删除某文档」的操作会因为锚点少列了一些块而留下孤儿数据。所以这里的失败方向必须是「中止」而不是「用图的视图兜底」。

而第 1 条的道具是「图的 `source_id` 是**有限的视图**」——这与 g02 里 `_create_tool_span` 那类「有界采样」是同一件事：图里的溯源字段可能被截断（有上限），所以它不能当权威。

**`check_and_migrate_data` 的短路本身也很讲究：只查 5 个文档。**

```python
max_check = min(5, len(processed_docs))
for doc_id in list(processed_docs.keys())[:max_check]:
    checked_count += 1
    entity_data = await self.full_entities.get_by_id(doc_id)
    relation_data = await self.full_relations.get_by_id(doc_id)
    if entity_data or relation_data:
        migration_needed = False
        break
```

所以判据是「**前 5 个已处理文档里有任意一个有数据就认为不需要迁移**」。这是一个刻意便宜的启发式：全量检查的代价是 N 次读，而「有没有迁移过」这个问题在实践中是全局一致的（要么全迁了要么全没迁）。

代价是理论上可能误判（若前 5 个恰好都没有而后面有），但那是「多迁一次」——而迁移本身是幂等回填，所以误判的代价可接受。**与前面 `strict=True` 形成对照：决定「迁不迁」可以便宜，决定「给谁写锚点」必须严格。** 这个分层很清晰：进入迁移与否是性能决定，迁移的具体内容正确性决定。

## 关键取舍

**源码事实：** 覆盖度栅栏与归属探针都在 `LightRAG.initialize_storages()` 而非后端 `initialize()` 里，因为「一个向量存储无法自己判断『我是空的』是不是故障」。

**源码事实：** 主导不变量「只有跑过并返回负裁决的探针才能拒绝，每一形式的『跑不了』都回退到升级前的行为」，逐项列出挂掉的 provider、超时、读不了的源、答不出的索引、被拒的标记写。

**源码事实：** `_cosine` 对长度不匹配与零模向量返回 `None` 而非 `0.0`，因为「静默返回 0.0 会被读成『模型变了』的最强可能证据」。

**源码事实：** 三个阈值：`ADOPT_COSINE = 0.95`、`REFUSE_COSINE = 0.70`，中间带子是「不确定」而非拒绝——「在一个活的部署上用含糊证据失败，比它本来就有的未标记容器更糟」。

**源码事实：** `PROBE_ROWS = 8` 而非 1，因为混合容器（同维度模型切换 + 嵌入器不可达期间的新写入）里挑一行会「随便落在哪个上」，而比较几行把混合物从「被采纳掩盖」变成「被探针报出」。

**源码事实：** 探针用 `context="document"` 嵌入，因为「一个对查询加不同前缀的 provider 否则会被拿去与它根本不会写出的向量比较」——这是正确性前提而不是精度优化。

**源码事实：** 探针超时默认 30 秒（可用 `LIGHTRAG_VECTOR_SPACE_PROBE_TIMEOUT` 覆盖），因为 provider 自带重试会烧 30-60 秒，而「一个让每次启动都延迟一分钟的安全特性本身就是可用性成本」；非法配置一律警告并用默认，从不抛。

**源码事实：** `graph_has_nodes` 用 `get_popular_labels`（抽象方法，人人必须实现），`graph_has_edges` 用 `iter_edges`（基类上唯一的**有界**边读器，fail-closed）。

**源码事实：** `_vectors_are_expected` 的未完成状态列表**从 `DocStatus` 推导**，新状态自动算作未完成，只能抑制拒绝、永不使能；且用 `count_docs_by_statuses(strict=True)` 而非 `get_status_counts()`，因为后者吞错会让「读失败」伪装成「读到了且没 PENDING」。

**源码事实：** 「没有任何文档」也算「没有未完成工作」，因为 `acreate_entity` / `ainsert_custom_kg` 写实体与向量但不写 doc-status 行——「那是唯一一处『后续运行会修好它』为假的地方」。

**源码事实：** `_PairingGate.check` 五道豁免的顺序：不持久化向量（设计如此）→ 源空/读不出 → 索引非空/读不出 → 调用方即将重建 → 有未完成文档且无「无主对象」→ 对象本来就没有向量。

**源码事实：** 「有未完成文档」的豁免是工作区级的，而证据是容器级的，所以还要过 `_nothing_will_heal` 排除「没有文档产出的对象」；那里采样 32 个是可靠的，因为「一次未命中只让检查不拒绝」。

**源码事实：** `baseline_targets` 里的目标额外付三项成本（探针 `force`、源严格读、容器 `is_empty`），因为它们要写**持久**基线声明；不在其中的目标不为这些付费。

**源码事实：** `check_and_migrate_data` 只抽查前 5 个已处理文档判断「是否需要迁移」（便宜且误判代价是幂等重迁），但决定「给谁写锚点」时用 `strict=True`，因为部分映射会被「锚点已存在」的检查短路固化，导致被省略的文档永久 fail closed。

**源码事实：** `_migration_chunk_ids` 先读严格追踪再退回图的 `source_id`，且「存在但为空的追踪行也是权威的」；读失败必须中止，因为「相信一次尽力而为的未命中可能持久化不完整的锚点，而那之后会授权一次不完整的清除」。

**架构推论：** 这个模块的全部复杂度都来自一个不对称：**「什么都没问题」与「我答不出」在观测上只差一个 `None`，而两者的正确处置完全相反。** 所以整个设计围绕「把 `None` 保住」——`_cosine` 返回 `None`、`_source_is_populated` 区分 `None`/`False`、`StartupEvidence` 三态、`_index_is_empty` 承诺抛而不是吞。每处细节都在对抗同一个退化倾向：把「不知道」压成某个具体值。

**架构推论：** 「负裁决走异常、正裁决走返回值」这个类型决定很小但作用大：它把「拒绝」变成控制流而不是数据，于是 `StartupEvidence` 里不可能出现「这个 `False` 到底要不要记录」这种判断点。**把两类不对称语义放进两种机制（异常 vs 值），是消除判断点的常用手法。**

**架构推论：** 「采样密度取决于漏掉的代价」在这里有两个相反的例子：`PROBE_ROWS = 8` 且要求全部一致（因为漏掉一行会让混合容器被误采纳，代价是永久掩盖异己数据），而 `DOCUMENTLESS_SAMPLE_SIZE = 32` 且命中即停（因为漏掉一个对象只是少抓一次故障，代价是不拒绝）。**同一个词「采样」，两个数量级不同的严格度，而判据是可算的。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/vector_space_gate.py` | `check_vector_space_at_startup` | 先跑三对覆盖度栅栏，再按目标跑探针 |
| `lightrag/vector_space_gate.py` | `StartupEvidence` | 三态证据容器，`None` 一律表示「无证据」 |
| `lightrag/vector_space_gate.py` | `_probe_same_embedding_space` | 抽样重嵌入 + 余弦，混合容器会被报出 |
| `lightrag/vector_space_gate.py` | `_cosine` | 退化输入返回 `None` 而非 0.0 |
| `lightrag/vector_space_gate.py` | `ADOPT_COSINE` / `REFUSE_COSINE` | 双阈值，中间带子判为不确定 |
| `lightrag/vector_space_gate.py` | `PROBE_ROWS` | 8 行，防止在混合容器上误采纳 |
| `lightrag/vector_space_gate.py` | `_probe_timeout` | 有界超时，非法配置回退默认且不抛 |
| `lightrag/vector_space_gate.py` | `_PairingGate` | 五道豁免按优先级链装配出裁决 |
| `lightrag/vector_space_gate.py` | `_vectors_are_expected` | 未完成状态从 `DocStatus` 推导，严格计数 |
| `lightrag/vector_space_gate.py` | `_source_is_populated` | 保住 `None` 与 `False` 的区别（基线要用） |
| `lightrag/vector_space_gate.py` | `_index_is_empty` | `is_empty` 承诺抛错，故障不伪装成空 |
| `lightrag/vector_space_gate.py` | `graph_has_nodes` | 用抽象方法 `get_popular_labels`，人人实现 |
| `lightrag/vector_space_gate.py` | `graph_has_edges` | 用唯一的**有界**边读器，fail-closed |
| `lightrag/vector_space_gate.py` | `_nothing_will_heal` | 排除「没有文档产出的对象」 |
| `lightrag/vector_space_gate.py` | `_probe_target` | 按目标跑探针，`force` 用于建基线 |
| `lightrag/storage_migrations.py` | `_StorageMigrationMixin` | 两条一次性数据迁移的宿主 |
| `lightrag/storage_migrations.py` | `_migration_chunk_ids` | 先严格追踪再图的有限视图，失败即中止 |
| `lightrag/storage_migrations.py` | `check_and_migrate_data` | 抽查 5 个文档决定是否迁移，写锚点用严格读 |

## 参考

- [lightrag 源码：vector_space_gate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/vector_space_gate.py)
- [lightrag 源码：storage_migrations.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/storage_migrations.py)
- [余弦相似度（Wikipedia）](https://en.wikipedia.org/wiki/Cosine_similarity)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

主核里最接近的是**恢复时的一致性检查（crash recovery validation）**：启动时检查控制文件、检查点标记与数据文件是否互相矛盾，矛盾则拒绝启动或触发恢复。这一层的覆盖度栅栏是同一类判断——**单看一个文件没问题，矛盾只在跨组件时可见**。

**差别在于「证据的强度」。** 主核的一致性检查用校验和与 LSN，证据是确定的；这里的归属探针用**余弦相似度**，证据是概率的。所以它必须有三值判定（采纳/拒绝/不确定），而主核只需要两值。这套三值逻辑的代价是要为「不确定」定义行为——而这里的行为是「继续服务并留下未标记容器」，正好等于「这个特性存在之前的状态」。

与 g03（向量存储）的连接最直接：那一页的 Faiss 加载路径会读 `.space.json` 标记并调用 `assert_vector_space_matches`，而本页的探针正是「标记缺失时如何产生标记」的机制。两者合起来是一个完整的两级方案：**有标记就信标记（便宜），没标记就抽样取证（贵但只跑一次）**。

与 a01（存储契约）的关系是这一层的定位：那一页讲四类契约，本页讲**契约之上的一层跨组件校验**——它不属于任何一个存储，因为「空索引是不是故障」这个问题需要两个存储才能回答。

对 AI 工程最实用的一条是「**采纳旧容器必须有证据，否则会永久掩盖错误**」。升级 + 换模型是一个很常见的组合操作，而如果只是「给没有标记的容器盖上当前模型的名字」，那么旧模型的向量会永久被当成新模型的，此后所有校验都不会再触发。而解决它的成本并不高：抽 8 条重嵌入比一下余弦即可。**判据是：这个动作会不会让将来的检查失去触发能力？** 会的话，就必须取证。

另一条是「三值而不是两值」。所有基于相似度/概率的判定都该有「不确定」这一档，且它的默认行为应该是**退回旧状态**而不是拒绝。这条在实践中决定了这类特性能不能长期开启：一个「不确定就停机」的检查会在第一次 provider 抖动时被运维关掉，然后永远不再打开。

最后一条是 `storage_migrations.py` 里那个「部分成功被短路固化」的教训。迁移 + 幂等存在性检查是一个危险的组合：部分完成会让存在性检查永久认为「已完成」，从而永远补不上缺的那部分。修法是让「决定给谁写」这一步用严格读并**失败即中止**——宁可整个迁移不跑，也不要留下一个「看起来跑完了」的残缺状态。
