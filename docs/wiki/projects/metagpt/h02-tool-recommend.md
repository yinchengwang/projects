---
project: metagpt
feature: tool-recommend
title: 工具推荐与检索策略
page_type: feature
schema_version: 1
refs:
  - path: metagpt/tools/tool_recommend.py
    symbols: [ToolRecommender, BM25ToolRecommender, EmbeddingToolRecommender]
---

# 工具推荐与检索策略

## 一句话职责

工具推荐器把「角色被允许使用的工具集合」在每一轮任务开始前裁剪成一个更小的相关集合：先按某种廉价策略**召回**一批候选，再让模型从候选里**挑选**最终的几个，最后把它们的 schema 拼成提示词段落。

## 机制是什么

一个 agent 可能配了几十个工具，但一轮任务真正用得上的是两三个。把所有 schema 全塞进提示词既费 token 又稀释注意力。所以推荐被拆成两段，并且只有第一段是可替换的：

| 阶段 | 职责 | 默认实现 | 可替换 |
|---|---|---|---|
| recall | 从全量里粗筛出 `recall_topk` 个候选 | 抽象方法，子类实现 | 是 |
| rank | 从候选里让模型选出 `topk` 个 | LLM 挑选 | 否（子类共用） |

四个实现分配在两种召回策略上：基类 `ToolRecommender` 的 `recall_tools` 抛 `NotImplementedError`；`TypeMatchToolRecommender` 用「任务类型等于工具标签」做精确匹配；`BM25ToolRecommender` 用 BM25 对工具描述做关键词检索；`EmbeddingToolRecommender` 是空壳，`recall_tools` 只有一句 `pass`（源码 docstring 自己写着 `NOTE: To be implemented.`）。

## 怎么实现的

**工具集合的三条来源，在字段校验期就归一。** `tools` 字段挂了 `field_validator(mode="before")`：已经是字典就原样返回（反序列化场景）；等于 `["<all>"]` 这个特殊标记就返回 `TOOL_REGISTRY.get_all_tools()`；否则调 `validate_tool_names(v)` 解析「名字 / 标签 / 路径」混合列表。因此推荐器拿到的永远是 `dict[name, Tool]`，构造之后不再关心用户是怎么写的。

**`recommend_tools` 的三段短路。** 第一段：`tools` 为空直接返回空列表。第二段：`force` 为真，或 `context` 与 `plan` 都为空时，直接返回全部工具——源码注释写明了意图：「强制推荐就用用户指定的全量」「没有任何有用信息时也用全量」。第三段：召回为空则返回空列表，不再进入 rank。

只有通过三段短路的调用才会真正花一次模型调用。这条设计的实际效果是：**在没拿到任务信息时，推荐器退化成「全量透传」而不是「返回空」**，避免因为缺上下文而让 agent 无工具可用。

**`rank_tools` 的提示词与解析。** 它从 `plan.current_task.instruction` 取当前任务（无 plan 时退到 `context`），把候选工具压成 `{name: description}`（只取 schema 的 `description`，不传完整 schema），填进 `TOOL_RECOMMENDATION_PROMPT` 后调 `LLM().aask`。

解析部分源码自称是「临时方案」，实情是三层兜底：

1. 先 `CodeParser.parse_code(lang="json")` 从响应里抠出 JSON 代码块，再用 `repair_llm_raw_output(repair_type=JSON)` 修补常见格式问题，然后 `json.loads`。
2. 抛 `JSONDecodeError` 时，把原始响应塞进 `JSON_REPAIR_PROMPT` 再问一次模型，从第二次的回答里解析。
3. 其他异常只打印 traceback——此时 `ranked_tools` 可能**未定义**，随后的 `isinstance(ranked_tools, dict)` 会抛 `UnboundLocalError` 而不是按预期降级。

解析成功后还有一步容错：若结果是 dict 而非 list，就取 `list(ranked_tools.values())[0]`——应对模型输出 `{"tools": [...]}` 这种包了一层的形态。再判一次不是 list 就记 warning 并回落到「全部候选」，最后 `validate_tool_names` 过滤掉模型编造的名字，取前 `topk` 个。

**`get_recommended_tool_info` 是给提示词用的出口。** 它先 `recommend_tools`，再可选地并上 `fixed` 里点名的工具（这些不经过推荐、强制包含），然后把 `{name: schemas}` 填进 `TOOL_INFO_PROMPT`——那段模板交代了「工具以 Python 类或函数形式可用」以及「调用时先按路径 import」。没有任何工具时返回空字符串。

**`BM25ToolRecommender` 的语料构造。** 构造期 `_init_corpus` 把每个工具拼成一条文档：`f"{tool.name} {tool.tags}: {tool.schemas['description']}"`。注意 `tool.tags` 是列表，这个 f-string 会把列表的 `repr` 直接拼进去（形如 `['code', 'file']`），而不是展开成词——分词器随后按空格切，于是这些标签会变成带方括号和引号的词元，与查询里的普通单词对不上。这是一个真实存在的匹配损耗。

分词就是 `text.split()`，源码注释 `FIXME: needs more sophisticated tokenization` 直接承认了。

`recall_tools` 拿查询（优先 `plan.current_task.instruction`，否则 `context`）分词、`bm25.get_scores`、`np.argsort` 降序取前 `topk`。注意 `argsort` 的结果只是**下标顺序**，取的仍是 `list(self.tools.values())[index]`，因此依赖「字典插入顺序」与「构造语料时的遍历顺序」一致——两者都是 `self.tools.values()`，在字典未被改动的前提下成立。

**`TypeMatchToolRecommender` 的精确匹配。** 无 plan 时返回前 `topk` 个工具（按插入顺序，不排序）。有 plan 时取 `plan.current_task.task_type`，从注册表按标签取候选，再与「允许集合」求交集。交集用集合运算，因此**结果顺序不确定**（`set & set` 后再列表化），这一点与 BM25 版本不同。

**rank 阶段对候选的描述只取 `description`。** 这使提示词保持在数十行量级，代价是模型只能凭一句话判断工具是否合适，看不到参数与签名。真正的调用细节要等 `get_recommended_tool_info` 才补上——也就是说「挑」与「用」两个阶段的上下文不同，模型在挑的时候信息更少。

## 关键取舍

**源码事实：** 召回可替换、排序不可替换。四个实现里三个共用同一套 LLM 排序逻辑，因此换召回策略不会改变「最终由模型定夺」这一点。若想完全不用模型（例如纯关键词流水线），只能继承基类覆写 `rank_tools`。

**源码事实：** 没有上下文时返回全量而不是空。这让「没传 plan 的调用」不会因推荐器而失去工具，但也意味着这条路径完全不省 token——推荐的价值在缺信息时自动消失。

**源码事实：** BM25 语料把 `tool.tags` 以列表 `repr` 形式拼进文档，与查询的普通单词对不上；同时分词只有 `str.split()`。两处叠加使 BM25 召回在「工具名与描述都含目标词」时才有稳定效果。

**源码事实：** rank 阶段的异常兜底不完整：第二层兜底失败时 `ranked_tools` 可能未被赋值，导致后续 `isinstance` 检查抛出 `UnboundLocalError`，而不是按设计回落到「全部候选」。

**架构推论：** 整个推荐器是 pydantic 模型，`tools` 在反序列化时会被重新解析。因此「序列化推荐器配置再恢复」这件事是可行的，但前提是注册表在反序列化时仍然装着那些工具——否则 `validate_tool_names` 会把未知名字记 warning 后丢弃。配置与注册表之间有一条隐式的加载顺序依赖。

**架构推论：** `force` 与「无上下文」走同一条短路，返回顺序是 `list(self.tools.values())` 即字典插入顺序。因此在这条路径上「哪些工具排在前面」由注册顺序决定，而不是由相关性决定——调 prompt 时若发现模型总偏爱前几个工具，原因在这里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/tools/tool_recommend.py` | `ToolRecommender` | 召回抽象 + LLM 排序的公共流程 |
| `metagpt/tools/tool_recommend.py` | `ToolRecommender.recommend_tools` | 三段短路：空集、强制/无信息、召回为空 |
| `metagpt/tools/tool_recommend.py` | `ToolRecommender.rank_tools` | 模型挑选 + 三层 JSON 解析兜底 |
| `metagpt/tools/tool_recommend.py` | `ToolRecommender.get_recommended_tool_info` | 推荐结果 + 强制工具拼成提示词段落 |
| `metagpt/tools/tool_recommend.py` | `validate_tools` | `<all>` 标记与名字/标签/路径的统一解析 |
| `metagpt/tools/tool_recommend.py` | `BM25ToolRecommender` | 关键词召回：语料构造与 BM25 打分 |
| `metagpt/tools/tool_recommend.py` | `BM25ToolRecommender._tokenize` | 按空格切词（源码标注需改进） |
| `metagpt/tools/tool_recommend.py` | `TypeMatchToolRecommender` | 任务类型与标签的精确匹配召回 |
| `metagpt/tools/tool_recommend.py` | `EmbeddingToolRecommender` | 未实现的向量召回占位 |
| `metagpt/tools/tool_recommend.py` | `TOOL_RECOMMENDATION_PROMPT` | 排序阶段提示词模板 |

## 参考

- [MetaGPT 源码：tools/tool_recommend.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/tools/tool_recommend.py)
- [MetaGPT 文档：工具推荐与角色工具配置](https://docs.deepwisdom.ai/main/en/guide/tutorials/customize_tool.html)
- [BM25 排序函数说明（rank_bm25）](https://github.com/dorianbrown/rank_bm25)

## 与主核 / AI 的关系

「先廉价召回、再精确排序」与主核里「先用索引把候选页缩到很小、再逐行校验谓词」是同一种两段式：第一段允许近似、允许漏，第二段才做精确判断。差别在第二段的实现手段——主核是确定性谓词求值，这里是一次模型调用，因此同样的输入可能给出不同结果，且结果无法复现。

对 agent 工程最实际的结论有两条。其一，工具数量增长时先坏的不是成本而是**召回**：BM25 只能匹配字面词，工具描述写得含糊就会召不回来，模型再强也挑不到它。把这层做稳的办法是让工具描述包含用户会用的词，而不是只写实现者视角的术语。其二，`recall_topk` 与 `topk` 的比例决定了模型的选择空间；把 `recall_topk` 调小会让推荐更稳定但更易漏，调大则相反——这是一个需要按工具集规模实调的旋钮。

