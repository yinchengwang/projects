---
project: ragas
feature: cost
title: token 用量与成本回调
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/cost.py
    symbols: [TokenUsage, CostCallbackHandler]
  - path: src/ragas/cost.py
    symbols: [get_token_usage_for_openai]
---

# token 用量与成本回调

## 一句话职责

用 LangChain 的回调接口在每次模型调用结束时抓一份 token 用量，按模型名聚合后按单价折算成钱——因为计价字段各家不同，所以「怎么从响应里抠出 token」被抽成一个可替换的解析函数。

## 机制是什么

三件东西组成这条链路：

| 组件 | 职责 |
|---|---|
| `TokenUsage` | 用量与金额的最小单位：`input_tokens`、`output_tokens`、`model`，带 `__add__` 与 `cost()` |
| `TokenUsageParser` | 类型别名 `(LLMResult \| ChatResult) -> TokenUsage`，即「从响应里抠用量」的可替换函数 |
| `CostCallbackHandler` | 继承 `BaseCallbackHandler`，在 `on_llm_end` 里逐次调用解析器并累积 |

**为什么用回调而不是包装模型。** 这是这一页的核心设计判断。ragas 的评测把同一个模型实例交给多个指标复用（`evaluate` 里会把 `metrics[i].llm` 临时替换再在 `finally` 里还原），如果用量统计靠包装 LLM 对象，就会出现「谁包谁」的归属问题：外层包装再被指标替换掉，统计就断了。而回调是 LangChain 传遍整条调用链的旁路——只要挂在 `evaluate(callbacks=...)` 上，任何一次模型调用（不管经过哪一层包装）都会触发 `on_llm_end`。所以统计与调用方的对象结构解耦，代价是必须依赖 LangChain 的回调协议。

解析器可替换是因为**各家响应结构不同**：OpenAI 把用量放在 `llm_output.token_usage` 且字段叫 `prompt_tokens`/`completion_tokens`；Anthropic 放在每条 generation 的 `message.response_metadata.usage` 且叫 `input_tokens`/`output_tokens`；Azure AI 用 `token_usage.input_tokens`；Bedrock 又叫回 `usage.prompt_tokens` 并带 `model_id`。

## 怎么实现的

**`TokenUsage.__add__` 拒绝跨模型相加。** 只有在 `self.model == y.model`（或两边都是 None）时才返回新实例，否则抛 `ValueError("Cannot add TokenUsage objects with different models")`。这条约束是「一个 `TokenUsage` 只代表一个模型」的不变式，聚合的前提就靠它。

**`cost()` 把两种单价都收，缺省则回落到单一价。** `cost_per_output_token is None` 时用输入单价顶替，然后 `input * in_price + output * out_price`。注意价格单位由调用方负责（这里不假设是「每百万 token」还是「每 token」）。

**`__eq__` 自己实现，且把模型纳入比较。** `isinstance` 检查 + 三个字段（用 `is_same_model` 判模型）。这看似正常，但配合 `__add__` 就形成了一个可用性细节：**两个不同模型的用量永远不相等**，即使用量数字相同。

**四个解析器读的是同一类对象的不同角落。** 两个「扁平型」（OpenAI/Azure AI）读 `llm_result.llm_output`；两个「按代次型」（Anthropic/Bedrock）遍历 `llm_result.generations` 的每个 `ChatGeneration` 读 `message.response_metadata`。全部取值都走 `get_from_dict`（按点号路径逐段下走、中途缺失返回 default 0），所以字段缺失不报错、只是算成 0。

**按代次型的两个解析器有一个共同的结构问题。** 它们的 `return` 写在 `for gs in llm_result.generations` 这个循环**体内**（缩进 8 列，与 `for gs` 同层的 `if` 之后），所以**只处理第一组 generation 就返回了**。多组候选（`n>1`）时后续组不会被统计。我按源码结构核对过缩进：`model = next(...)` 与 `return sum(...)` 都在循环体内。

这也解释了 `else:` 的存在（Bedrock 那个是裸 `return`）：`for-else` 的 `else` 只在循环**正常走完**时执行，而循环体内有 `return`，所以这个 `else` 分支实际上不可达——只有 `generations` 为空列表时才会走到。Anthropic 版本用 `for...else` + 无缩进 `return`，Bedrock 版本用裸 `return`，两者行为一致但写法不同。

**`sum(token_usages, TokenUsage(0,0,model))` 的初值带模型名。** 因为 `__add__` 要求模型相同，所以初值的 `model` 必须与实际用量的模型一致，否则第一次相加就抛错。源码用 `next((usage.model for usage in token_usages if usage.model), "")` 取第一个非空模型名当初值。我实测确认：两个不同模型的用量相加会抛 `ValueError`，同模型才会得到 `TokenUsage(30, 12, 'claude-3')`。也就是说**一次响应里若两条 generation 报告了不同模型名，解析器直接崩**——而不是忽略差异。

**`CostCallbackHandler.on_llm_end` 只有两行。** 把解析结果 append 进 `usage_data`，不聚合、不去重。所有聚合都推迟到 `total_tokens()`/`total_cost()` 被显式调用时。

**`total_tokens()` 先按模型分桶再合并。** 用 `first_usage.model` 建初始表，遍历其余项：模型已在表里就 `+=`，否则新建桶。最后若只有一个桶返回该 `TokenUsage`，多个桶返回列表。**空 `usage_data` 会在 `usage_data[0]` 处抛 `IndexError`**（实测确认），源码没有处理「一次模型都没调用」这种情况。

**`total_cost()` 的两条分支不对称，其中一条会静默漏算。** 开头先校验「全都没给就报错」；然后分桶（同 `total_tokens`）。分叉：

- **单模型**：若给了 `per_model_costs`，模型名不在表里就抛 `ValueError`；否则用 `cost_per_token`。两条路都会真正算钱。
- **多模型**：`total_cost = 0.0`，遍历所有桶，**只有 `model in per_model_costs` 的桶才累加**——不在表里的模型被跳过，不报错。

我实测确认了这个静默漏算：两个模型各有用量、成本表只配了一个，返回的是那一个的金额（0.12），另一个被完全忽略。这与单模型分支「不在表里就抛错」形成尖锐对比：**配错成本表时，单模型会报错、多模型会少算钱**。

## 关键取舍

**源码事实：** 用 LangChain 回调而不是包装 LLM。理由是回调能穿透多层包装，而 ragas 的评测会临时替换 `metrics[i].llm` 再还原。

**源码事实：** `TokenUsage.__add__` 在模型不同时抛 `ValueError`；解析器用第一个非空模型名当 `sum()` 初值，因此一次响应里出现多个模型名会直接失败（实测确认）。

**源码事实：** Anthropic 与 Bedrock 两个解析器的 `return` 在 `for gs in generations` 循环体内，只处理第一组候选；`else`/尾部 `return` 分支实际只在 `generations` 为空时可达（按源码缩进核对）。

**源码事实：** 全部字段取值走 `get_from_dict` 点号路径，缺失即 0，不报错。

**源码事实：** `total_tokens()` 与 `total_cost()` 都以 `usage_data[0]` 起始，空列表抛 `IndexError`（实测确认）。

**源码事实：** `total_cost()` 单模型分支缺表项抛错、多模型分支缺表项静默跳过（实测确认：只得到已配置模型的金额）。

**架构推论：** 「用量统计靠回调」是正确的方向，但也划定了边界：**只有走 LangChain 回调协议的调用才被统计**。若某个 provider 的直接 SDK 调用绕过了回调，用量会静默缺失——不是算错，是不算。这与「包装模型」相反：包装至少会强制经过统计对象。

**架构推论：** 把「解析响应」抽成可替换函数，等于承认**没有统一口径**。这是不可消除的差异：`prompt_tokens` vs `input_tokens`、`llm_output` vs `response_metadata` 都是各家的既成事实。抽成插件后，新增 provider 只写一个函数，不改聚合逻辑；代价是解析器的正确性无法由框架保证。

**架构推论：** 多模型成本静默漏算，是「宽松默认」在这类统计代码里的典型危害。成本数字会被拿去做预算决策，而**偏低的成本比偏高的成本更危险**——没人会因为账单比预期低而去追查。同一份代码里单模型分支选了「严格」、多模型分支选了「宽松」，这种不一致本身就是缺陷来源。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/cost.py` | `TokenUsage` | 用量与金额单位，含 `__add__`、`cost`、`__eq__` |
| `src/ragas/cost.py` | `TokenUsage.__add__` | 拒绝跨模型相加的不变式 |
| `src/ragas/cost.py` | `TokenUsage.cost` | 输入/输出双价折算，缺省回落到单值 |
| `src/ragas/cost.py` | `TokenUsageParser` | 「从响应抠用量」的函数类型别名 |
| `src/ragas/cost.py` | `get_token_usage_for_openai` | 读 `llm_output.token_usage`，字段为 prompt/completion |
| `src/ragas/cost.py` | `get_token_usage_for_anthropic` | 读每条 generation 的 `response_metadata`，字段为 input/output |
| `src/ragas/cost.py` | `get_token_usage_for_bedrock` | 同代次型但字段为 prompt/completion 且带 `model_id` |
| `src/ragas/cost.py` | `get_token_usage_for_azure_ai` | 读 `llm_output.token_usage`，字段为 input/output |
| `src/ragas/cost.py` | `CostCallbackHandler` | 在 `on_llm_end` 里累积解析结果 |
| `src/ragas/cost.py` | `CostCallbackHandler.total_tokens` | 按模型分桶，单个返回对象、多个返回列表 |
| `src/ragas/cost.py` | `CostCallbackHandler.total_cost` | 单/多模型两条分支，缺表项处置不同 |
| `src/ragas/utils.py` | `get_from_dict` | 点号路径取值，缺失返回默认值 |
| `src/ragas/evaluation.py` | `token_usage_parser` 参数 | 只有提供解析器才挂 `CostCallbackHandler` |
| `src/ragas/dataset_schema.py` | `EvaluationResult.total_tokens` | 对外入口，未配置时抛 `ValueError` |
| `src/ragas/dataset_schema.py` | `EvaluationResult.total_cost` | 对外入口，转发到 `cost_cb.total_cost` |

## 参考

- [ragas 源码：cost.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/cost.py)
- [ragas 源码仓库](https://github.com/explodinggradients/ragas)
- [LangChain 集成说明](https://docs.ragas.io/en/stable/howtos/integrations/langchain/)
- [Anthropic Messages API](https://docs.anthropic.com/en/api/messages)

## 与主核 / AI 的关系

这套机制与主核里的「执行统计 / 慢查询日志」是同一层：都在一个可观测的旁路上收集运行时事实，且都面临同一个问题——**统计口径必须与真实执行路径一一对应**。主核的统计挂在执行器的固定钩子上（算子进入/退出），漏算意味着「某条路径绕过了执行器」；这里的统计挂在 LangChain 回调上，漏算意味着「某次模型调用没走回调协议」。两者的失败都是静默的，都需要专门的对照手段（主核可以用「扫描行数 vs 返回行数」自校，这里可以拿 provider 账单对照）。

三条对 AI 工程的结论。第一，**成本统计要按模型分桶**，因为单价不同；把不同模型混在一起算出的金额没有意义。第二，**缺表项必须报错而不是跳过**：这里单模型分支报错、多模型分支跳过的对比，正好说明「宽松默认」在钱的问题上是不可接受的。第三，**解析器要按「代次」完整遍历**：只读第一组候选的写法在 `n>1` 时会少算，而少算的账单没人会发现。

最后一条关于实现选择：用回调而不是包装模型，在大规模评测里是对的（穿透包装），但代价是「统计依赖协议」而非「统计依赖对象」。如果一个系统能同时做到两件事——把统计塞进每次调用的必经路径，又让它与被统计对象的生命周期解耦——那才是真正稳的方案；只做到后者（回调），就必须接受「绕路的调用永远不被统计」。
