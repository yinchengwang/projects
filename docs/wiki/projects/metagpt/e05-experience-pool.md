---
project: metagpt
feature: experience-pool
title: 经验池与经验生命周期
page_type: feature
schema_version: 1
refs:
  - path: metagpt/exp_pool/manager.py
    symbols: [ExperienceManager, create_exps, query_exps]
  - path: metagpt/exp_pool/schema.py
    symbols: [Experience, ExperienceType, Trajectory]
---

# 经验池与经验生命周期

## 一句话职责

经验池把「一次成功的调用」存成可检索的记录，下次遇到相同或相近的请求时先查池子：命中一条满分经验就直接复用结果，没命中才真跑，跑完把结果连同评分写回池子。

## 机制是什么

一个请求-响应对，加上它有多好，构成一条 `Experience`。池子就是这些经验的向量库或关键词库。整个机制围绕「复用」与「自评」两个动作：

- **复用**：查池子、逐条判「是否足够好且足够像」，命中则跳过真实执行。
- **自评**：真实执行之后，用一次模型调用给结果打分（1 到 10），连同请求、响应、标签一起入库。

驱动这一切的是 `exp_cache` 装饰器。它把「查池子 → 判完美 → 执行 → 评分 → 入库」串成一条流水线，被装饰的函数只需要遵守一条约定：`req` 必须作为关键字参数传入。

`ExperienceManager` 负责池子的读写与存储后端选择，`Experience` 负责单条经验的形状以及「用什么文本做检索键」。

## 怎么实现的

**`Experience` 的字段取舍。** 必填两个：`req`（请求文本）与 `resp`（响应文本，可以是字符串、JSON 或代码）。可选的有 `metric`（耗时、费用、评分）、`exp_type`（成功 / 失败 / 洞察，默认成功）、`entry_type`（自动 / 人工，默认自动）、`tag`（归类标签）、`traj`（轨迹：plan / action / observation / reward 四段）。时间戳与 uuid 都用 `default_factory` 自动生成，因此新经验天然具备唯一标识与排序依据。

`rag_key()` 只返回 `req`。也就是说**检索键是请求本身，响应与评分都不参与匹配**。这决定了池子的语义：按「问什么」找答案，而不是按「答得好不好」找答案。

`Score` 的 `val` 声明为 1 到 10 的整数（`MAX_SCORE = 10`），`reason` 是自然语言理由。`Metric` 除了 score 还记录 `time_cost`（毫秒）与 `money_cost`（美元）。

**`ExperienceManager` 的四个门控属性。** `is_readable` 是 `enabled and enable_read`，`is_writable` 是 `enabled and enable_write`。两个 setter 都有一个附带行为：设成 `True` 时顺手把 `enabled` 也置 `True`——避免出现「读了但总开关关着」的无效组合。

`create_exps` 与 `query_exps` 各自的第一行就是门控判断：不可写直接返回、不可读直接返回空列表。两者都挂了 `handle_exception`，所以池子故障不会打断主流程——写失败只记日志，读失败返回空。

**写入路径的 `persist` 时机。** `create_exps` 在 `add_objs` 之后立刻 `persist(self.config.exp_pool.persist_path)`。也就是说每条经验入库都伴随一次落盘，而不是攒批。这是「宁可慢也不丢」的选择。

**读取路径的三段过滤。** `query_exps` 先 `await self.storage.aretrieve(req)` 拿向量或 BM25 召回结果，从每个节点的 `metadata["obj"]` 取回 `Experience`；然后按 `tag` 做一次等值过滤（源码注释写着 `TODO: filter by metadata`，说明当前是在内存里筛而不是在存储层筛）；最后若 `query_type == QueryType.EXACT`，再按 `req` 精确等值过滤。因此 SEMANTIC 只做召回，EXACT 在召回之上再做一层精确匹配。

**存储后端的两条装配路径。** `_resolve_storage` 用一张字典把 `retrieval_type` 映射到两个创建函数：

- BM25：先看 `persist_path/docstore.json` 是否存在。不存在就**塞一条占位经验** `Experience(req="req", resp="resp")` 并把 `create_index=True`，让 BM25 有一个非空语料才能建索引；存在则从已有索引加载。两条路径都把召回条数固定为 `DEFAULT_SIMILARITY_TOP_K`（2）。占位经验是这里最值得注意的实现细节：它不是 bug，而是「BM25 不接受空语料」这个约束的变通。
- Chroma：按 `persist_path` 与 `collection_name` 建持久化客户端，同样固定召回条数。

两条路径都可能追加一个 LLM 重排器（`use_llm_ranker` 为真时加 `LLMRankerConfig(top_n=2)`）。重排器由 `rag` 的 ranker 工厂装配，与检索器解耦。

**`get_exps_count` 不做门控。** 它直接 `self.storage.count()`，在不可读时也会去初始化存储。这是四个对外方法里唯一绕过门控的读操作。

**装饰器的流水线。** `exp_cache` 的 `get_or_create` 分四步：总开关关着就直接执行原函数（并兼容协程与普通函数两种返回）；否则构造 `ExpCacheHandler`、`fetch_experiences` 取候选、`get_one_perfect_exp` 逐条判完美（命中即反序列化返回，**不执行原函数**）、未命中则 `execute_function` 真跑、可写时 `process_experience` 评分入库。最后返回原始响应而不是序列化后的字符串。

`ExpCacheHandler` 在 `model_validator(mode="after")` 里补齐默认依赖（manager / scorer / judge / context_builder / serializer），并校验 `req` 必须是关键字参数——装饰器的硬约束就在这里强制。默认 `tag` 由 `_generate_tag` 生成：第一个位置参数是对象时用 `类名.方法名`，否则用函数名。

**上下文注入会改写 `req`。** `_execute_function` 在执行前把 `kwargs["req"]` 替换成 `context_builder.build(...)` 的结果，也就是把召回的经验拼进请求文本。因此**真实执行看到的请求与入库记录的请求不是同一个字符串**：入库的 `_req` 是原始请求的序列化，执行用的是拼接后的上下文。

**评分与完美判定是两套独立策略。** `SimpleScorer` 用模板让模型输出 `{val, reason}` 的 JSON，再从代码块里解析。`SimplePerfectJudge` 完全不调模型：它要求「序列化请求与经验请求全等」**且**「评分等于 10」，两个条件同时满足才算完美。所以默认配置下只有满分且请求字面一致的旧经验会被直接复用。

**同步函数的桥接。** `choose_wrapper` 按被装饰函数是不是协程决定包装方式：协程直接 `await`，普通函数则 `NestAsyncio.apply_once()` 之后用事件循环 `run_until_complete`。这让同步函数也能享受异步流水线，代价是同步路径会创建或复用事件循环。

## 关键取舍

**源码事实：** `rag_key()` 只取 `req`，因此池子的召回粒度是请求相似度。两条响应质量不同但请求相同的经验会互相竞争召回名额，最终由 `top_k` 与重排决定谁能被看到。

**源码事实：** 默认完美判定要求评分恰为 `MAX_SCORE` 且请求字面全等。这条规则把「复用」限制在极窄的范围内——同义改写的请求不会被复用，评分 9 的经验也不会。它保证正确性，牺牲命中率。

**源码事实：** BM25 首次建库要插入一条占位经验。这条经验会永久留在池子里，参与后续检索，直到有人显式清空（`delete_all_exps`）。任何统计「经验条数」的地方都应知道它的存在。

**源码事实：** 每条经验入库都立刻 `persist`，而读取的 tag 过滤发生在内存里。写入一侧选一致性，读取一侧选实现简单，两侧不对称。

**架构推论：** 门控拆成 `enabled` + `enable_read` + `enable_write` 三层，使得「只观察不写入」这种灰度用法可以直接配置出来，无需改代码。代价是三个布尔的所有组合都需要被理解：`enabled=False` 时另外两个布尔完全无效。

**架构推论：** 执行前的上下文注入改写了 `req`，这意味着「同一函数同一请求」在开启/关闭经验池时会产生不同的模型输入，进而可能产生不同的输出与不同的评分。经验池因此不是透明缓存，它会改变被缓存对象的行为。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/exp_pool/schema.py` | `Experience` | 经验记录：req/resp/metric/tag/traj 与检索键 |
| `metagpt/exp_pool/schema.py` | `ExperienceType` | 成功 / 失败 / 洞察三类经验 |
| `metagpt/exp_pool/schema.py` | `Trajectory` | 轨迹：plan / action / observation / reward |
| `metagpt/exp_pool/manager.py` | `ExperienceManager` | 池子读写入口与三个门控 |
| `metagpt/exp_pool/manager.py` | `ExperienceManager.create_exps` | 门控写入、立即落盘 |
| `metagpt/exp_pool/manager.py` | `ExperienceManager.query_exps` | 召回 + tag 过滤 + 可选精确匹配 |
| `metagpt/exp_pool/manager.py` | `_create_bm25_storage` | 首次建库插入占位经验以满足非空语料 |
| `metagpt/exp_pool/decorator.py` | `exp_cache` | 查池子 / 判完美 / 执行 / 评分入库的流水线 |
| `metagpt/exp_pool/decorator.py` | `ExpCacheHandler._execute_function` | 执行前用召回经验改写 `req` |
| `metagpt/exp_pool/perfect_judges/simple.py` | `SimplePerfectJudge` | 请求全等且满分的完美判定 |
| `metagpt/exp_pool/scorers/simple.py` | `SimpleScorer` | 模型打分产出 1 到 10 的评分与理由 |

## 参考

- [MetaGPT 源码：exp_pool/manager.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/exp_pool/manager.py)
- [MetaGPT 源码：exp_pool/decorator.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/exp_pool/decorator.py)
- [MetaGPT 文档：经验池使用说明](https://docs.deepwisdom.ai/main/en/guide/tutorials/customize_agent.html)

## 与主核 / AI 的关系

「把一次执行的结果连同质量评分存起来，下次命中直接返回」与主核的查询结果缓存是同一类机制，但校验维度不同：主核的缓存靠逻辑读的一致性校验（页版本、事务快照）保证不返回过期结果，这里的校验靠「请求字面全等 + 评分满分」。前者是强一致，后者是启发式。这也正是「缓存能不能被信任」的分水岭：经验池的命中是建议，不是保证。

对 agent 工程而言，这一页的价值在于给出可预期的成本与风险。开启经验池后，每个被装饰的调用会多出三笔开销：一次向量或 BM25 检索、未命中时的一次评分模型调用、每条经验的一次落盘。而收益只在「请求高度重复、且第一次就是满分」时才兑现。因此它适合「同一批任务重复跑」的批处理场景，不适合开放式对话——后者的请求几乎不会字面重复，池子只增不进命中率。

