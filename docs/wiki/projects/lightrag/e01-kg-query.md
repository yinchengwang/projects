---
project: lightrag
feature: kg-query
title: 图谱查询主流程
page_type: feature
schema_version: 1
refs:
  - path: lightrag/operate.py
    symbols: [kg_query, naive_query]
  - path: lightrag/operate.py
    symbols: [_build_query_context, _build_context_str]
---

# 图谱查询主流程

## 一句话职责

把「用户问了一句话」推进到「一份可发给模型的上下文」，并把五种查询模式（local / global / hybrid / mix / naive）的差异**收敛成「查哪个存储」这一个变量**——其余四步（抽关键词、截断、合并块、渲染）对所有模式共用同一套代码。

## 机制是什么

查询链路是固定的四段，而模式只影响第一段：

| 阶段 | 函数 | 各模式是否相同 |
|---|---|---|
| 1. 搜索 | `_perform_kg_search` | **不同**：决定查实体库、关系库、还是块向量库 |
| 2. 截断 | `_apply_token_truncation` | 相同（同一份 token 预算） |
| 3. 合并块 | `_merge_all_chunks` | 相同（向量块 + 实体块 + 关系块轮转合并） |
| 4. 渲染 | `_build_context_str` | 相同（同一套提示模板与引用编号） |

**模式与存储的对应关系被压成一行：**

```
entity_query   = ll_keywords if mode in ("local",  "hybrid", "mix") else ""
relation_query = hl_keywords if mode in ("global", "hybrid", "mix") else ""
```

`local` 拥有实体库、`global` 拥有关系库、`hybrid` / `mix` 两者都拥有。`naive` 不在这张表里——它走 `naive_query`，完全不碰图谱。

**`mix` 与 `hybrid` 是两种不同的合并。** `hybrid` 是「实体检索 + 关系检索」合并（都在图谱里）；`mix` 在此外**再加一路纯向量块检索**。前者回答的是「图上相关的都拿来」，后者回答的是「图上相关的 + 原文里最像的」。

## 怎么实现的

### 一、`kg_query` 的门面职责：把「没关键词」当一等情形处理

**关键词为空不是异常，而是需要四种不同应对的情形。**

```
if ll_keywords == [] and query_param.mode in ["local", "hybrid", "mix"] and not hl_keywords:
    logger.warning("low_level_keywords is empty")
if hl_keywords == [] and (query_param.mode in ["hybrid", "mix"] or (query_param.mode == "global" and not ll_keywords)):
    logger.warning("high_level_keywords is empty")
if hl_keywords == [] and ll_keywords == []:
    if len(query) < 50:
        logger.warning(f"Forced low_level_keywords to origin query: {query}")
        ll_keywords = [query]
    else:
        return QueryResult(content=PROMPTS["fail_response"], llm_generated=False)
```

三层的意图各不相同：

1. **只缺一侧** —— 只记警告，真正的补偿在 `_resolve_kg_search_queries` 里做（让另一侧顶上），注释写明「一个被搜索兜住的空列表在那里上报，这里只在没有任何东西替代它时才警告」。
2. **两侧都空且查询短** —— **直接把原始 query 当关键词**。这是一个务实的兜底：短查询本身往往就是一个名词短语，把它当实体名去检索是合理的。
3. **两侧都空且查询长（≥50 字符）** —— 返回固定的失败话术。长查询当关键词没有意义，而悄悄用一个长句去向量检索只会得到偏题的结果。

**阈值 50 是一个没有解释的魔数**，但它的位置说明了设计意图：它是「短查询可以当关键词」与「长查询不行」的分界，而不是一个可调参数——这暗示它来自经验而非配置。

**`raw_data` 里存的不是拼接后的字符串，而是两个原始列表。**

```
context_result.raw_data.setdefault("metadata", {})["keywords"] = {
    "high_level": list(hl_keywords) if hl_keywords else [],
    "low_level": list(ll_keywords) if ll_keywords else [],
}
```

注释解释了原因：「报告关键词列表本身：把拼好的搜索字符串再按 `, ` 切开，会拆坏一个含逗号的关键词（`Washington, D.C.`）」。**这是一个「不要在信息损失之后试图恢复它」的典型例子**——拼字符串是给人读的，下游要的是结构化数据，两者必须在分叉点之前各自留存。

**四种返回形态由两个开关决定。**

```
if query_param.only_need_context and not query_param.only_need_prompt:
    return QueryResult(content=context_result.context, raw_data=..., llm_generated=False)
```

配合文档字符串里的表：

| 开关 | 返回什么 |
|---|---|
| `only_need_context=True` | 上下文串（不调模型） |
| `only_need_prompt=True` | 完整提示（不调模型） |
| `stream=True` | 流式迭代器 + 完整 raw_data |
| 默认 | 模型回答文本 + 完整 raw_data |

**`llm_generated` 这个字段是有意暴露的**：它让调用方能区分「这段文本是模型说的」与「这段文本是我们拼的上下文」。一个把它当答案展示的界面会在前一种情况下显示给用户、后一种情况下不该显示。

### 二、`_build_query_context`：四阶段，以及三处「返回 None」

**这是整条链路的骨架，它的价值在于把「没结果」分成三种不同的情况。**

```
search_result = await _perform_kg_search(...)
if not search_result["final_entities"] and not search_result["final_relations"]:
    if query_param.mode != "mix":
        return None
    else:
        if not search_result["chunk_tracking"]:
            return None
```

**第一处 None：搜不到任何东西。** 但对 `mix` 放宽——因为 `mix` 还有纯向量那一支，即使图谱空着也可能有块。`chunk_tracking` 就是向量支的产物。

```
truncation_result = await _apply_token_truncation(...)
if truncation_result.get("token_budget_exceeded"):
    return None
```

**第二处 None：预算连「空上下文 + 必需提示」都装不下。** 这与「搜不到」是两件事，但出口相同——因为对调用方来说，两种情况下都拿不到可用上下文。区别在日志：截断侧会打 "max_total_tokens cannot fit the required query prompt"。

```
if not merged_chunks and not truncation_result["entities_context"] and not truncation_result["relations_context"]:
    return None
```

**第三处 None：三路来源全空。** 注意它检查的是 `entities_context`（放进提示的那份）而不是 `filtered_entities`（交给块选择的那份）——**判据是「提示里将出现什么」，而不是「我们手里有什么」**。

**一处刻意的配置分裂值得单独记。**

```
# Prompt rendering uses the live query config, while chunk selection must
# keep the storage snapshot. Override only the prefix for token budgeting.
budget_config = text_chunks_db.global_config
if user_prompt_prefix is not None:
    budget_config = {**budget_config, "user_prompt_prefix": user_prompt_prefix}
```

**提示渲染要用「当前查询的配置」，而块选择要用「存储初始化时快照的配置」** —— 于是这里用存储的快照做基底，只把 `user_prompt_prefix` 换成本次请求的值。

理由是这两件事的时效性要求相反：提示是这次请求的，必须用最新的用户提示；而块选择依赖 `kg_chunk_pick_method`、`related_chunk_number` 这类建库期参数，中途改了会让同一次查询里不同块用不同规则挑。

**`token_memo` 是「一次请求内共享的 token 计数缓存」。**

```
# One exact-text token-count memo per request, shared by the three fitter
# calls below (stage 2 and both stage-4 fits). It dies with this request;
# the fitter only reads/writes it inside the tokenizer executor.
token_memo: dict[str, int] = {}
```

**为什么需要它：** 后面的拟合器会反复渲染同一个提示尝试不同的记录条数（二分查找），而 `tokenizer.encode` 是纯函数——同一段文本算两次是浪费。在这个请求内共用一份 memo，把它从 O(候选数 × 编码成本) 降到接近 O(唯一文本数)。

**而「只活一次请求」这个约束被写进了注释与拟合器的文档**：它不能让多个请求共享，否则会无限增长；也不能在事件循环里碰（见 e03 那页的执行器约束）。

**最后写进 `raw_data` 的 `processing_info` 是一份「漏斗图」。**

```
raw_data["metadata"]["processing_info"] = {
    "total_entities_found": ..., "total_relations_found": ...,
    "entities_after_truncation": ..., "relations_after_truncation": ...,
    "merged_chunks_count": ..., "final_chunks_count": ...,
}
```

六个数字构成「搜到多少 → 截断后剩多少 → 合并出多少 → 最后进提示多少」。**这套指标存在的理由是：调 RAG 效果时最常问的问题是「我的上下文为什么这么短」，而单看最终长度答不了它**——必须知道是检索少了、截断狠了、还是合并去重掉了。

### 三、模式分叉的实现位置：`_perform_kg_search`

**它先把三种要嵌入的文本合并成一次批量调用。**

```
if query and (kg_chunk_pick_method == "VECTOR" or chunks_vdb):
    texts_to_embed.append(query); text_purposes.append("query")
if need_ll:
    texts_to_embed.append(entity_query); text_purposes.append("ll")
share_keyword_embedding = need_ll and relation_query == entity_query
if need_hl and not share_keyword_embedding:
    texts_to_embed.append(relation_query); text_purposes.append("hl")
```

注释说明：「批处理避免 2-3 次串行的 API 往返」。而 `share_keyword_embedding` 这个判断处理的是一个具体情形——**当兜底规则把同一个关键词串送进两个存储时，它只需要嵌入一次**。

**「只嵌入活着的检索分支会真正用到的文本」这条也被写明了**：`need_ll = bool(entity_query)`、`need_hl = bool(relation_query)`。于是 `local` 模式不会白白为关系检索算一次嵌入。

**批嵌入失败只是警告，不中断。**

```
except Exception as e:
    logger.warning(f"Failed to batch pre-compute embeddings: {e}")
```

三个 embedding 变量保持 `None`，后续检索各自退化成「让存储自己嵌入」。**这是一个正确的降级**：预计算是优化，不是前提。

**而 `_get_vector_context` 里有一段相反取向的注释，两者对照很有意思。**

> 这里没有宽泛的 try/except——镜像 `_get_node_data` / `_get_edge_data`，它们的 `entities_vdb` / `relationships_vdb` 查询也被留作向上传播。**一个后端的查询失败不是「零个相关块」**：在这里吞掉它，会让一次瞬时的向量库错误表现为一个自信的「没有结果」答案（而在 mix 模式下会静默丢掉向量检索那一支，同时 KG 结果照常流出）。

**同一个文件里，嵌入预计算失败可以吞、检索失败不能吞。** 判据是「失败后还能不能诚实地继续」：预计算失败的补偿是重新算一次（结果等价）；检索失败的补偿只能是「当作没找到」（结果是错的）。

**实体与关系的轮转合并是同一段代码的两个实例。**

```
final_entities = []; seen_entities = set()
max_len = max(len(local_entities), len(global_entities))
for i in range(max_len):
    if i < len(local_entities):
        entity = local_entities[i]; name = entity.get("entity_name")
        if name and name not in seen_entities:
            final_entities.append(entity); seen_entities.add(name)
    if i < len(global_entities):
        entity = global_entities[i]; name = entity.get("entity_name")
        if name and name not in seen_entities:
            final_entities.append(entity); seen_entities.add(name)
```

**轮转（round-robin）而不是「先 local 全部、再 global 全部」** —— 于是当截断只保留前 N 条时，两个来源**按比例**各留一部分，而不是「local 全留、global 全丢」。

**这是配额公平性的最简实现形式**：不需要显式分配配额，交错排列天然让后缀截断对两个来源公平。

而 `seen_entities` 用的是 `entity_name`（不是 id）——因为同一个实体可能从两条路径被检索到（一条命中它作为头实体、一条作为尾实体），必须按名字去重。

### 四、`naive_query`：预算算法与图谱路径不同

**它完全不经过四个阶段的框架，而是自己算一遍预算。**

```
sys_prompt_tokens = await acount_tokens(tokenizer, pre_sys_prompt)
query_tokens = await acount_tokens(tokenizer, query)
buffer_tokens = 200  # reserved for reference list and safety buffer
available_chunk_tokens = max_total_tokens - (sys_prompt_tokens + query_tokens + buffer_tokens)
```

**`buffer_tokens = 200` 是一个硬编码的经验值**，注释写明它留给「引用列表 + 安全余量」。

这与图谱路径的做法有本质差别：图谱路径用**二分搜索 + 精确重渲染**（e03 那页的 `_fit_kg_context_to_total_budget`），而这里用**一次减法 + 固定余量**。

**为什么允许这个粗糙度**：`naive` 只有一类内容（块文本），而块的排布是简单的顺序拼接，用固定余量足够。图谱路径要装三种不同形状的记录（实体 JSON、关系 JSON、带引用的块），渲染方式复杂到必须实测。

**它用空 `content_data` 渲染一次提示来算开销。**

```
pre_sys_prompt = sys_prompt_template.format(response_type=..., user_prompt=..., content_data="")
```

**这是一个「先量壳子、再往里装东西」的标准做法**，注释里补了一句重要的说明：「`user_prompt` 即使 `only_need_context` 会提前返回也被计入——**那些开关预览的是真实请求，所以它们的块数必须与真实请求一致**」。

这解释了为什么两个路径都要为「不发给模型的提示」付预算：**预览接口与真实接口的输出必须能对得上**，否则拿 `only_need_context` 调参的人会得到与生产不同的块数。

**`naive_query` 有两个 `@overload` 声明，是给旧调用方留的兼容层。**

```
@overload
async def naive_query(..., return_raw_data: Literal[True] = True) -> dict[str, Any]: ...
@overload
async def naive_query(..., return_raw_data: Literal[False] = False) -> str | AsyncIterator[str]: ...
```

**实现签名里已经没有 `return_raw_data` 了** —— 它统一返回 `QueryResult | None`。overload 只是让类型检查器对老代码继续给对类型。**这是一种「运行期统一、类型层留后门」的迁移手法**：新代码拿到结构化结果，老代码拿到它期望的字符串，两者共用一份实现。

## 关键取舍

**源码事实：** 五种模式压成「查哪个存储」一个变量：`entity_query` 只在 local/hybrid/mix 非空，`relation_query` 只在 global/hybrid/mix 非空；`hybrid` 是图谱内两路合并，`mix` 额外加一路纯向量块检索。

**源码事实：** 三处 `return None` 分别对应「搜不到」（对 mix 放宽，因为向量支可能还有票）、「预算装不下必需提示」、「三路来源全空」。第三处判的是 `entities_context`（将进提示的）而非 `filtered_entities`（手里的）。

**源码事实：** 关键词全空时按查询长度分流：短于 50 字符则把原查询当低层关键词，否则返回固定失败话术。一侧为空只记警告，实际补偿在 `_resolve_kg_search_queries`。

**源码事实：** `raw_data` 里存的是两个关键词**列表**而不是拼接后的字符串，因为反过来拆分会拆坏含逗号的关键词。

**源码事实：** 预算配置以「存储初始化时的快照」为基底、只覆盖 `user_prompt_prefix`——提示渲染要最新配置，块选择要建库期快照。

**源码事实：** `token_memo` 是一次请求内的 token 计数缓存，被三个阶段共用；它必须随请求死亡，且只在 tokenizer 执行器里读写。

**源码事实：** 实体与关系都用轮转（round-robin）合并而非顺序拼接，于是后缀截断对两个来源按比例公平；去重键是 `entity_name` 而非 id。

**源码事实：** 批量预计算嵌入失败只警告（三个 embedding 变量留 `None`，检索退化为各自嵌入），而检索失败必须向上传播——注释明确「后端的查询失败不是零个相关块」。

**源码事实：** `naive_query` 用固定 `buffer_tokens = 200` 的一次减法算块预算，图谱路径用二分搜索 + 精确重渲染。两者都为「不发给模型的预览请求」计费，以保证预览与真实请求的块数一致。

**源码事实：** `naive_query` 保留两个 `@overload` 声明（`return_raw_data` 为真/假），但实现统一返回 `QueryResult | None`——运行期统一、类型层兼容旧调用方。

**架构推论：** 这条链路的设计核心是**把「模式」降级成一个很窄的分叉点**。五种模式听起来是五条流程，实际只有一处不同。做成这样需要两个前提：检索结果被归一成同一种结构（实体的 `entity_name` / 关系的 `src_tgt`），而后续四个阶段对这些结构的形状有唯一假设。**归一层越薄，后面的复用越多**——代价是归一时必须丢掉各来源的特性（比如哪条是向量找的、哪条是图上找的，只剩 `chunk_tracking` 里的一个 `source` 标记）。

**架构推论：** 三处 `return None` 而不是「返回空上下文」是一个重要的接口选择。空上下文会让模型在没有材料的情况下自由发挥，那是最坏的一种失败（看起来像成功）。返回 `None` 让调用方能明确地说「这次没查到」。**能把「没有答案」和「答案是空的」分开的系统，才敢在无人值守的场景里跑。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/operate.py` | `kg_query` | 图谱查询门面：关键词兜底、预算、返回形态分派 |
| `lightrag/operate.py` | `naive_query` | 纯向量块查询，自带一套预算减法 |
| `lightrag/operate.py` | `_build_query_context` | 四阶段骨架与三处 None 出口 |
| `lightrag/operate.py` | `_build_context_str` | 最终渲染，返回 `(context, raw_data)` |
| `lightrag/operate.py` | `_perform_kg_search` | 模式分叉点：批嵌入、两路检索、轮转合并 |
| `lightrag/operate.py` | `_resolve_kg_search_queries` | 模式 → 存储的映射与缺词兜底 |
| `lightrag/operate.py` | `_kg_keyword_fallback` | 兜底名称（也是答案缓存键的一部分） |
| `lightrag/operate.py` | `_get_vector_context` | 块向量检索，失败向上传播 |
| `lightrag/operate.py` | `_get_node_data` / `_get_edge_data` | 实体 / 关系检索入口 |
| `lightrag/operate.py` | `processing_info` | 六项漏斗指标写入 `raw_data` |

## 参考

- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [lightrag 文档：查询模式](https://github.com/HKUDS/LightRAG/blob/main/README.md)
- [论文：LightRAG 双级检索](https://arxiv.org/abs/2410.05779)

## 与主核 / AI 的关系

查询链路与主核的**查询执行器**同构：都是「解析请求 → 访问若干存储 → 拼装结果」。而 lightrag 的一个特殊之处是它的「解析」产物不是执行计划，而是**检索用的字符串**——高低层关键词。

**模式 → 存储的映射与主核的谓词下推选择同一性质**：都是「根据请求形状决定访问哪几个数据源」。差别是数据库的选择由代价模型驱动、离线可验证；这里的选择由用户显式指定（`mode`），因为「该查图还是该查原文」没有可估算的代价。

**三处 `return None` 与数据库的「空结果集」不同。** 数据库里空结果是正常值；这里 `None` 意味着「连材料都没凑出来」，它与「答案为空」是两回事。这个区分对应主核里**「查询失败」与「查询成功但无匹配行」**的区分——两者在日志、告警、重试策略上完全不同。

**「预览接口与真实请求必须同预算」这条对应数据库的 `EXPLAIN` 与 `EXPLAIN ANALYZE`。** 前者不执行但必须反映真实的计划成本；若 `EXPLAIN` 的成本估算与实际执行走不同的路径，那个估算就没有参考价值。lightrag 为 `only_need_context` 付 `user_prompt` 的预算，正是同一考虑。

对 agent 工程的直接价值有三条。其一，**把模式分叉收敛到一处**。五种模式的代码如果各写一遍，任何一处修复都要改五遍；收敛到「查哪个存储」之后，后面四个阶段只有一份实现。其二，**失败要分「搜不到」与「装不下」**。两者的日志不同、处置不同（改检索 vs 改预算），而合并成一个「无结果」会让调优时无从下手。其三，**不要在信息损失后试图恢复它**。关键词拼成字符串只为给人读，下游要的结构化数据必须在拼接之前就留存。
