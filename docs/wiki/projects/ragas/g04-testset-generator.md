---
project: ragas
feature: testset-generator
title: 测试集总装
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/testset/synthesizers/generate.py
    symbols: [TestsetGenerator]
  - path: src/ragas/testset/synthesizers/testset_schema.py
    symbols: [TestsetSample, Testset]
---

# 测试集总装

## 一句话职责

`TestsetGenerator` 把「文档 → 知识图谱 → 角色 → 场景 → 样本」这五段接成一次调用：前三段由 transform 与 persona 完成，后两段按一份**配额表**分发到各个合成器，最后拼成一个可转成评测数据集的 `Testset`。

## 机制是什么

它是 `@dataclass`，五个字段：`llm`、`embedding_model`、`knowledge_graph`（缺省空图）、`persona_list`（可选）、`llm_context`（可选）。

三个入口方法对应三种输入形态，差别只在「怎么把输入变成图谱」：

| 方法 | 输入 | 节点类型 | 额外处理 |
|---|---|---|---|
| `generate_with_langchain_docs` | langchain `Document` 序列 | `DOCUMENT` | 无 |
| `generate_with_llamaindex_docs` | llama-index `Document` 序列 | `DOCUMENT` | 跳过空文本；不支持 `return_executor` |
| `generate_with_chunks` | langchain `Document` 或裸字符串 | `CHUNK` | 用「已分块」版默认 transform |

三者之后都汇到 `generate(testset_size, query_distribution, num_personas, ...)`——这个方法才是总装。

配额来自 `query_distribution`：一个 `[(合成器实例, 概率), ...]` 的列表。缺省用 `default_query_distribution(...)` 按图谱与 llm 现造一份。每个合成器拿到的题数是 `ceil(testset_size × 概率)`。

## 怎么实现的

**入口方法的第一件事是拒绝默认模型。** 三个入口都有成对的同一段校验：`if not self.llm and not transforms_llm` 抛 `ValueError`，`if not self.embedding_model and not transforms_embedding_model` 再抛一个。两处错误文案都写明了原因之外的两条补救路径（在实例化时提供、或自带 transforms）。

这段校验的存在是可推导的：`BaseSynthesizer` 的 `llm` 字段有 `default_factory`，会静默造一个 OpenAI 客户端；`TestsetGenerator` 不愿意沿袭这个行为，于是用「对象上必须有值」硬挡住。**「有默认值」与「必须有值」是两件事，前者由 dataclass 提供，后者必须自己检查。**

**图是重建而非累积。** 每个入口都新建 `KnowledgeGraph(nodes=nodes)` 再 `apply_transforms`，然后覆盖 `self.knowledge_graph`。所以重复调用会丢掉上一次的图——这是「一次调用一条流水线」的语义，不是增量接口。

**两个入口各有一份文档适配细节。** llamaindex 那条在转节点时**跳过空文本**（`doc.text is not None and doc.text.strip() != ""`），而 langchain 那条不跳（不检查 `page_content`）。chunks 那条也跳空，并且把裸字符串的 `metadata` 设成空字典。同一个「造节点」的动作在三条路径里做了三种程度的清洗——这不是设计上的抽象，而是三次独立添加留下的差异。

**`generate` 的前置处理有顺序依赖。** 按代码顺序：

1. `run_config` 存在时**只对 `BaseRagasLLM` 调 `set_run_config`**——代码注释直说「`InstructorBaseRagasLLM` 没有这个方法」。所以新式 LLM 无法通过这条路径拿到超时与重试配置。
2. 缺省 `query_distribution`。
3. `token_usage_parser` 若给了，造一个 `CostCallbackHandler` 并塞进 callbacks（兼容 `BaseCallbackManager` 与 list 两种形态）。
4. 开一个 `new_group`（名字固定为 `"ragas testset generation"`），后续每个阶段都挂在这个组下。
5. 角色：`persona_list` 为空就现造；已有则 `random.shuffle` 打散。
6. 配额：`calculate_split_values([prob...], testset_size)`。

**配额是向上取整，总量可能超过请求量。** `calculate_split_values` 的实现是 `splits = [ceil(n * prob) for prob in probs]`。概率之和期望为 1，但每项各自向上取整，所以**总和可能大于 `testset_size`**。`testset_size` 因此是「下限」而不是精确值——想精确控制行数不能靠这个参数。

**两阶段、两个 Executor、两次配额计算。** 场景阶段与样本阶段各起一个 `Executor`：

- 场景阶段 `keep_progress_bar=False`，对每个合成器 `exec.submit(scenario.generate_scenarios, n=splits[i], ...)`，然后 `exec.results()` 拿到「每个合成器 → 场景列表」的嵌套列表。
- 样本阶段 `keep_progress_bar=True`，双层循环把「每个合成器的每个场景」逐个 submit，再 `exec.results()`。

配额被算了两次：第一次的结果只用于追踪的 `inputs={"splits": splits}`，第二次才用于实际 `n=splits[i]`。两次调用参数完全相同，这是纯粹的重复计算（`calculate_split_values` 是纯函数），但读代码时容易被第一次的出现误导成「splits 是共享的」。

**`additional_testset_info` 依赖「结果按提交顺序返回」这条契约。** 提交样本时同步往一个列表里 push `{"synthesizer_name": synthesizer.name}`，最后用 `zip(eval_samples, additional_testset_info)` 配对。两者能对上，完全依赖 `Executor.results()` 按提交顺序返回（其 docstring 明确承诺这一点）。**一旦有一天结果按完成时间返回，这里会静默错配——样本会被贴上别人的合成器名字。**

**`return_executor` 只覆盖第二阶段。** 若为 `True`，函数在样本 submit 完就 `return exec`——此时场景阶段已经跑完（花了钱与时间），而 `Testset` 尚未组装、`cost_cb` 也没接上。调用方拿到的 executor 只能自己 `results()` 拿到样本列表，`Testset` 的构造得自己来。llamaindex 那个入口硬写 `return_executor=False`，所以它没有这个半成品出口。

**样本与测试集是两层容器。** `TestsetSample` 只有两个字段：`eval_sample`（真正的评测样本，`SingleTurnSample` 或 `MultiTurnSample`）与 `synthesizer_name`（溯源）。`Testset` 是 `RagasDataset[TestsetSample]` 的子类，带 `run_id`（自动生成的 UUID，`repr=False, compare=False`）与 `cost_cb`。

`to_evaluation_dataset` 只取 `eval_sample` 丢掉溯源——**评测只要样本，溯源只在造题侧有意义**。`to_list` 反过来把 `synthesizer_name` 拍平进每个字典，与 `eval_sample` 的字段同级。

**`from_list` 会推断样本种类，且会改动入参。** 判定规则是「有 `user_input` 且不是 list → 单轮，否则多轮」。它先 `sample.pop("synthesizer_name")`——**直接修改调用方传进来的字典**，然后才构造。这种「就地消费入参」的写法让同一个 list 不能被反序列化两次。

`from_annotated` 是人工标注产物的入口：读 JSON，只收 `approval_status == "approved"` 的条目。**人工审核过的才进测试集**，这条过滤是它存在的全部理由。

**成本是可选的、并且只能事后查。** `total_tokens` / `total_cost` 在 `cost_cb is None` 时抛 `ValueError`，文案指明「给 `TestsetGenerator` 传 `token_usage_parser` 才能算成本」。也就是说成本统计不在默认路径上——不传那个 parser，就永远不知道这次造题花了多少。

## 关键取舍

**源码事实：** 三个入口都强制要求 llm 与 embedding（实例上有值，或本次调用传入），拒绝 `BaseSynthesizer` 那种静默造默认模型的行为。

**源码事实：** 每个入口都新建并覆盖 `self.knowledge_graph`，不累积。

**源码事实：** 三条入口的节点清洗程度不同：langchain 路径不检查空内容，llamaindex 与 chunks 路径跳过空内容。

**源码事实：** `set_run_config` 只对 `BaseRagasLLM` 生效（注释明说新式 LLM 没这个方法）。

**源码事实：** `calculate_split_values` 每项 `ceil(n * prob)`，配额总和可能大于 `testset_size`；该函数在 `generate` 里被调用两次，参数相同。

**源码事实：** `additional_testset_info` 与 `eval_samples` 的配对依赖 `Executor.results()` 按提交顺序返回。

**源码事实：** `return_executor=True` 时场景阶段已执行、`Testset` 未组装、`cost_cb` 未接上；llamaindex 入口不支持该参数。

**源码事实：** `Testset.from_list` 用 `sample.pop(...)` 就地修改入参来剥离 `synthesizer_name`。

**架构推论：** 「入口方法只负责造图，`generate` 只负责发题」这个分工使流程可分段复用：可以自己造好图再调 `generate`，也可以复用同一份 `query_distribution` 在多个图上跑。代价是两条路径的错误校验各写一遍（三处成对的两条 `ValueError`），新增入口必须记得补。

**架构推论：** 把配额做成「概率列表 + 向上取整」而不是精确整数，使 `testset_size` 变成软目标。用评测库的人常常期待「给多少造多少」，这里的实际语义是「至少这么多，通常略多」。这个偏差在测试数小时的出题上不明显，但写断言时必须留意。

**架构推论：** 两阶段各自的 Executor 与各自的 group，使追踪树天然分层（generation → scenario generation → sample generation）。代价是错误处理要写两遍 `try/except`（`on_chain_error` 后再 `raise`），并且第一阶段的失败会直接中断整条链——没有「部分合成器失败、其余继续」的语义，除非把 `raise_exceptions` 关掉。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/testset/synthesizers/generate.py` | `TestsetGenerator` | 总装对象：模型、图、角色、语境五项 |
| `src/ragas/testset/synthesizers/generate.py` | `TestsetGenerator.from_langchain` | 用旧式包装器装配实例 |
| `src/ragas/testset/synthesizers/generate.py` | `TestsetGenerator.from_llama_index` | llama-index 版装配 |
| `src/ragas/testset/synthesizers/generate.py` | `TestsetGenerator.generate_with_langchain_docs` | 文档造图 + 走总装，支持返回 executor |
| `src/ragas/testset/synthesizers/generate.py` | `TestsetGenerator.generate_with_llamaindex_docs` | 跳过空文本，不支持返回 executor |
| `src/ragas/testset/synthesizers/generate.py` | `TestsetGenerator.generate_with_chunks` | 预分块输入直接成 `CHUNK` 节点 |
| `src/ragas/testset/synthesizers/generate.py` | `TestsetGenerator.generate` | 配额、角色、两阶段 Executor、组装 Testset |
| `src/ragas/testset/synthesizers/utils.py` | `calculate_split_values` | 按概率向上取整分配题数 |
| `src/ragas/testset/synthesizers/testset_schema.py` | `TestsetSample` | 样本 + 合成器名，两层容器的内层 |
| `src/ragas/testset/synthesizers/testset_schema.py` | `Testset` | 测试集：样本、run_id、成本回调 |
| `src/ragas/testset/synthesizers/testset_schema.py` | `Testset.to_evaluation_dataset` | 剥掉溯源字段转评测数据集 |
| `src/ragas/testset/synthesizers/testset_schema.py` | `Testset.from_list` | 反序列化并就地剥离合成器名 |
| `src/ragas/testset/synthesizers/testset_schema.py` | `Testset.from_annotated` | 只收已审核的标注样本 |

## 参考

- [ragas 源码：testset/synthesizers/generate.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/testset/synthesizers/generate.py)
- [ragas 源码：testset/synthesizers/testset_schema.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/testset/synthesizers/testset_schema.py)
- [ragas 文档：生成测试集](https://docs.ragas.io/en/stable/getstarted/rag_testset_generation/)
- [RAGAS 论文（Es et al., 2023）](https://arxiv.org/abs/2309.15217)

## 与主核 / AI 的关系

「入口只造数据、核心只负责调度」这种分层，与主核里「连接器负责把外部数据变成页、执行器负责把页变成结果」是同一个结构。差别在于主核的入口有严格的事务边界，这里的入口是「覆盖式重建图」——没有回滚，重复调用就丢上一次的图。

对 agent 工程最实用的四条。其一：`testset_size` 是**软目标**，配额向上取整，实际条数可能偏多；写集成测试时用范围断言而不是等值断言。其二：`additional_testset_info` 与结果的配对依赖提交顺序，所以**不要在样本生成阶段并发改写 `query_distribution`**，否则名字与实际样本会错配。其三：`return_executor=True` 只覆盖第二阶段，此时场景已烧掉全部 token——想「先看场景再决定要不要继续」需要自己在场景阶段之后自行接管，而不是靠这个参数。其四：成本统计必须显式传 `token_usage_parser`，否则 `total_cost()` 只会抛错；造题是这条链上最贵的环节，正式跑批前先把成本回调接上。
