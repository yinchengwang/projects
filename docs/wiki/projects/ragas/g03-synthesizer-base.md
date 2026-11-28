---
project: ragas
feature: synthesizer-base
title: 合成器契约与场景
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/testset/synthesizers/base.py
    symbols: [BaseSynthesizer, BaseScenario, QueryLength, QueryStyle]
  - path: src/ragas/testset/synthesizers/single_hop/base.py
    symbols: [SingleHopScenario, SingleHopQuerySynthesizer]
  - path: src/ragas/testset/synthesizers/multi_hop/base.py
    symbols: [MultiHopScenario, MultiHopQuerySynthesizer]
---

# 合成器契约与场景

## 一句话职责

合成器把「场景」变成「评测样本」，分两步走：先只凭知识图谱与角色造出**场景**（还没有问题），再把每个场景落成**样本**（含问题、参考答案、参考上下文）——这个两段结构决定了难度档与风格维度能被穷举与采样。

## 机制是什么

`BaseSynthesizer` 是一个 `@dataclass` 的抽象基类，字段只有三个：`name`（空则由类名填）、`llm`（缺省用 `_default_llm_factory()` 造的 gpt-4o-mini）、`llm_context`（可选的额外语境）。

它对外只暴露两个方法，都是「包一层回调追踪再调抽象方法」：

- `generate_scenarios(n, kg, persona_list, callbacks)` → `List[Scenario]`，内部调 `_generate_scenarios`；
- `generate_sample(scenario, callbacks)` → `BaseSample`，内部调 `_generate_sample`。

`BaseScenario` 是四个必填字段：`nodes`（参与这个场景的节点列表）、`style`（`QueryStyle`）、`length`（`QueryLength`）、`persona`（提问角色）。

两个枚举定义的是**风格维度**：

- `QueryLength`：`LONG` / `MEDIUM` / `SHORT`。
- `QueryStyle`：`MISSPELLED` / `PERFECT_GRAMMAR` / `POOR_GRAMMAR` / `WEB_SEARCH_LIKE`，但它们的**取值是中英文混排的**——`"Misspelled queries"`、`"Perfect grammar"`、`"Poor grammar"`、`"Web search like queries"`。枚举值会被 `.value` 直接填进提示里，所以这些取值是**提示文案的一部分**，不是标识符。

两个具体合成器各加一个字段：单跳加 `term`（这条片段对应的主题词），多跳加 `combinations`（这一组多跳涉及的主题词列表）。

## 怎么实现的

**两段式的第一个成果是「组合空间」。** 两个合成器都实现了一对方法：`prepare_combinations` 造候选、`sample_combinations`（多跳叫 `sample_diverse_combinations`）从中抽取。这是本页的核心机制——**把「难度与风格」变成可枚举的笛卡尔积，再按既定策略采样**。

**单跳的组合与采样。** `prepare_combinations` 拿一个节点、它的词表、角色列表与「角色→概念」映射，产出**一条**候选 dict：`{terms, node, personas, styles, lengths}`，其中 `styles` 与 `lengths` 直接取两个枚举的全部成员。

`sample_combinations` 把这条 dict 展开成完整笛卡尔积（term × persona × style × length），`random.shuffle` 后逐个尝试加入，用 `(node, term)` 作去重键：第一次见到这个组合就无条件收，再见到就只在「数量还不够」时收。所以去重是**优先级而非硬约束**——不够数就允许重复。

**多跳的组合与采样，以及「多样性」的显式计数。** `prepare_combinations` 遍历每个主题组合，为每个组合算出「哪些角色相关」「哪些节点相关」：角色侧按「概念是否出现在组合里」筛，节点侧按「节点属性里的主题是否出现在组合里」筛。于是每个组合自带一个**可用的叉积规模**（角色 × 风格 × 长度），且节点是这一组共享的。

`sample_diverse_combinations` 用一个三级优先规则选样本：

1. 若「这个组合 + 这个角色」还没出现过 → 收。
2. 否则若该风格的计数不超过「当前最大计数 + 1」→ 收。
3. 否则若该长度的计数不超过「当前最大计数 + 1」→ 收。

第 2、3 步用的是 `max(style_count.values(), default=0) + 1`，也就是**让每个维度上的分布尽量拉平**：只有当某个风格明显落后于最多的那个时才补它。这是一个「软均衡」策略，不是严格配额。

**`make_contexts` 把节点拼成带编号的语境。** 多跳样本的参考上下文是**多条**（`reference_contexts` 是列表），每条前面加 `<1-hop>`、`<2-hop>` 这样的标签。这个标签有实际作用：提示里要能区分「这是第几跳的片段」，否则模型无法表达「先看 A 再看 B」的顺序。单跳那边只有一个节点，`reference_contexts` 是单元素列表。

**生成样本时把场景字段整体拷进提示，再把答案的结构化字段拆回样本。** 单跳的输入模型是 `QueryCondition`（persona、term、context、query_length、query_style、llm_context），多跳是 `QueryConditions`（把 term 换成一串 themes）。两者的输出模型都含 `query` 与 `answer` 两个字段，分别落到 `SingleTurnSample` 的 `user_input` 与 `reference`。

样本还额外记三个**溯源字段**：`persona_name`、`query_style`、`query_length`，取值都用 `getattr(..., "name", None)` 取枚举的**成员名**（如 `"MISSPELLED"`）而不是 `.value`。这是刻意的：`.value` 是给模型看的提示文案，`.name` 是给分析脚本看的分组键。提示与统计两条路用同一个枚举的两个不同属性，是本页一个值得记住的细节。

**类型检查是运行期的、用 `isinstance` + `TypeError`。** 两个 `_generate_sample` 开头都判 `isinstance(scenario, SingleHopScenario)` / `MultiHopScenario`，不匹配就抛 `TypeError("scenario type should be ...")`。基类的泛型参数 `Scenario` 只在类型层面约束，运行期靠这两句。

**回调追踪是「包一层 group」而非侵入实现。** `generate_scenarios` 与 `generate_sample` 都调 `new_group(name=self.name, inputs=..., callbacks=...)` 拿到一个 run manager 与子回调列表，把子回调传给抽象方法，结束后 `on_chain_end(outputs=...)`。实现类完全不需要知道追踪存在。

**缺省 LLM 是「延迟构造」的。** `_default_llm_factory` 在函数体内部才 `from openai import OpenAI`，然后 `OpenAI()` + `llm_factory("gpt-4o-mini", client=client)`。写成 `field(default_factory=...)` 使「不传 llm 时才会真的需要 OpenAI 包与 API key」——只要显式传了 llm，这条路径完全不执行。这对测试与离线环境很重要。

`llm_context` 是横切字段：它在基类上，被两个实现分别塞进各自的输入模型。它的用途是给「问题该怎么问」追加一段全局约束（例如「面向中文用户」）。

## 关键取舍

**源码事实：** 场景（`BaseScenario`）不含问题文本，只含「用哪些节点、什么风格、多长、谁问」。问题在第二阶段才生成。这个拆分是全部多样性机制的前提。

**源码事实：** `QueryStyle` 的枚举值是提示文案（`"Misspelled queries"` 等），`QueryLength` 的值是纯标识符（`"long"`）。两者风格不统一，但都被 `.value` 直接填进提示。

**源码事实：** 样本的溯源字段用枚举 `.name` 而非 `.value`，与提示用 `.value` 形成两套用法。

**源码事实：** 单跳去重键是 `(node, term)`，重复允许在数量不足时发生；多跳用「组合+角色」唯一 + 风格/长度软均衡的三级规则。

**源码事实：** 缺省 LLM 在 `default_factory` 里延迟 import OpenAI，显式传 llm 时不会触发。

**源码事实：** 单跳的 `reference_contexts` 只有一条；多跳是多条且每条带 `<n-hop>` 标签。

**架构推论：** 两段式的真正价值在于**第一段不花钱**（不调模型），于是可以在第一段里做任意的组合穷举与去重、用纯 Python 决定「要造哪些题」，只在第二段才为选中的场景各调一次模型。如果合成器是「输入什么就直接生成什么」，多样性就只能靠随机，成本会随试验次数上升。

**架构推论：** 把「风格」与「长度」做成枚举而不是自由字符串，使它们能进笛卡尔积、能被计数、能被打平。代价是枚举值同时承担「提示文案」职责——想改文案就得改枚举，而改枚举会改变历史样本里记录的值。这是「枚举值当文案用」的固有耦合。

**架构推论：** 多跳的采样规则里那三条 `elif` 实际上是「先保证覆盖（组合×角色），再保证风格均衡，最后保证长度均衡」的优先级序列。它不是严格分层抽样，而是「软均衡」：某一维落后太多才被补。这种写法在样本量小时会退化（`max(...)+1` 的容差在小样本下几乎总是成立），在样本量大时才会真正生效。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/testset/synthesizers/base.py` | `BaseSynthesizer` | 合成器基类：两段式入口 + 回调包装 |
| `src/ragas/testset/synthesizers/base.py` | `BaseSynthesizer.generate_scenarios` | 包 group 后调抽象的场景生成 |
| `src/ragas/testset/synthesizers/base.py` | `BaseSynthesizer.generate_sample` | 包 group 后调抽象的样本生成 |
| `src/ragas/testset/synthesizers/base.py` | `BaseScenario` | 场景四字段：节点、风格、长度、角色 |
| `src/ragas/testset/synthesizers/base.py` | `QueryLength` | 长度维度三档 |
| `src/ragas/testset/synthesizers/base.py` | `QueryStyle` | 风格维度四档，枚举值即提示文案 |
| `src/ragas/testset/synthesizers/base.py` | `_default_llm_factory` | 延迟构造的 gpt-4o-mini 缺省模型 |
| `src/ragas/testset/synthesizers/single_hop/base.py` | `SingleHopScenario` | 单跳场景：多一个 `term` |
| `src/ragas/testset/synthesizers/single_hop/base.py` | `SingleHopQuerySynthesizer.prepare_combinations` | 单节点候选与全枚举展开 |
| `src/ragas/testset/synthesizers/single_hop/base.py` | `SingleHopQuerySynthesizer.sample_combinations` | 笛卡尔积 + `(node, term)` 优先去重 |
| `src/ragas/testset/synthesizers/multi_hop/base.py` | `MultiHopScenario` | 多跳场景：`combinations` 主题列表 |
| `src/ragas/testset/synthesizers/multi_hop/base.py` | `MultiHopQuerySynthesizer.prepare_combinations` | 按主题组合筛角色与节点 |
| `src/ragas/testset/synthesizers/multi_hop/base.py` | `MultiHopQuerySynthesizer.sample_diverse_combinations` | 三级优先规则打平风格与长度 |
| `src/ragas/testset/synthesizers/multi_hop/base.py` | `MultiHopQuerySynthesizer.make_contexts` | 多跳语境加 `<n-hop>` 标签 |

## 参考

- [ragas 源码：testset/synthesizers/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/testset/synthesizers/base.py)
- [ragas 源码：testset/synthesizers/multi_hop/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/testset/synthesizers/multi_hop/base.py)
- [ragas 文档：测试集生成与查询合成](https://docs.ragas.io/en/stable/concepts/test_data_generation/)
- [RAGAS 论文（Es et al., 2023）](https://arxiv.org/abs/2309.15217)

## 与主核 / AI 的关系

「先生成组合空间、再按策略采样、最后才付生成成本」与主核里「先估算代价、再选执行计划」是同一个次序原则：**把免费的决策放在付费的动作之前**。主核用统计信息估算，这里用纯 Python 的穷举与去重。

对 agent 工程最实用的四条。其一：造评测集时把「场景」与「样本」分开，你可以先生成几百个场景再决定只生成其中 20 个问题，试错成本几乎为零。其二：**风格枚举值会被原样塞进提示**，所以想要「错别字风格」的题就必须让枚举值写成英文文案；若直接改 `.value`，历史样本里记录的 `query_style` 不变（记的是 `.name`），但提示行为会变。其三：多跳的多样性规则是软均衡——小样本时等于没生效，别指望造 10 条题就能自动覆盖四种风格 × 三档长度。其四：`<n-hop>` 标签是多跳能成立的关键，模型必须能分辨「第几跳的片段」；自己写合成器时若只把多段上下文拼成一坨，模型会退化成回答其中一段。
