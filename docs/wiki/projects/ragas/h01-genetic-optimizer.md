---
project: ragas
feature: genetic-optimizer
title: 提示遗传优化
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/optimizers/genetic.py
    symbols: [GeneticOptimizer]
  - path: src/ragas/optimizers/genetic.py
    symbols: [CrossOverPrompt, FeedbackMutationPrompt]
---

# 提示遗传优化

## 一句话职责

用标注数据反推「当初人工判定时遵循的指令」，把每个候选当成一组可交叉的提示文本，再用评测分数当适应度做一轮选择——这是 ragas 从「评测工具」跨到「用评测结果改提示」的那一步。

## 机制是什么

优化的对象是**基因型 = `Dict[str, str]`**：键是某个指标的某个提示名（如 `statement_generator_prompt`），值是那段 instruction 文本。一个候选就是「该指标所有提示的 instruction 的一份完整取值」。

页面上那些 pydantic 模型**不是基因本身**，而是四个遗传算子的入参/出参契约：

| 模型 | 算子身份 | 作用 |
|---|---|---|
| `FormattedExamples` → `OutputInstruction`（`ReverseEngineerPrompt`） | 初始化 | 把若干条「输入 + 期望输出」压成一段能产生该标注的指令 |
| `ParentPrompts` → `OutputInstruction`（`CrossOverPrompt`） | 交叉 | 两个父代指令合成一条语义覆盖两者的子代指令 |
| `FeedbackMutationInput` → `FeedbackMutationOutput` | 变异（诊断） | 给定指令 + 判错的样例，产出最多 3 条「指令该怎么改」的意见 |
| `FeedbackMutationPromptInput` → `OutputInstruction`（`FeedbackMutationPromptGeneration`） | 变异（实施） | 拿指令 + 意见，产出改进后的指令 |

所以「提示变成可交叉的结构」这件事的实现方式是：**提示本来就是字符串，而交叉被实现成「让模型做语义合并」**。遗传算法在这里只提供三个壳——候选群体、适应度排序、配对选择——真正的算子全部由 LLM 执行。

一次 `optimize` 固定四阶段：`Initializing Population` → `Feedback Mutation` → `Cross-over Mutation` → `Fitness Evaluation`，每个阶段都包一层 `new_group` 以便在追踪树里可见，并用 `tqdm` 按 stage 步数估算总进度。

## 怎么实现的

**入口有三道硬校验，其中一条是样本量门槛。** `optimize` 先检查 `metric`/`llm` 非空，再检查 `len(dataset) >= MIN_ANNOTATIONS`（10 条），不足时报错文案直接告诉你还差几条：「Please annotate {N} more samples」。10 条这个数写死成模块常量，不可配置。

三个配置项从 `config` 字典取，各有默认：`population_size=3`、`num_demonstrations=3`、`sample_size=12`。

**群体初始化：从标注里反推指令，再把指标自带的指令当种子。** `initialize_population` 只保留 `is_accepted` 的样本，用 `stratified_batches(batch_size=num_demonstrations, stratify_key="metric_output", replace=False, drop_last_batch=False)` 按指标输出分层切批，取前 `population_size - 1` 批，每批交给 `_reverse_engineer_instruction`。

`_reverse_engineer_instruction` 的实质是「逐提示名拼样例」：遍历批内每个样本的 `prompts` 字典，把 `prompt_input` 里**值为 None 的键剔掉**，输出优先取 `edited_output`、退而取 `prompt_output`，累积成 `{input, output}` 列表，然后对每个提示名各跑一次 `ReverseEngineerPrompt`。一次调用产出一条候选的该字段。

随后有一个关键动作：把 `metric.get_prompts()` 里**名字与候选键集有交集**的 instruction 取出来，追加到群体末尾当种子。注释写的是「get the default prompt used in the metric as seed prompt」。因为初始化只造了 `population_size - 1` 个，加上种子正好凑满。种子存在的意义是保证「原有提示」也在候选池里——遗传算法不该把现有最优解排除在外。

**变异阶段是「先评测、再诊断、再改写」，且每个候选独享一份抽样。** `feedback_mutation` 对每个候选单独 `dataset.sample(sample_size, stratify_key="metric_output")`（`sample_size` 会先被 min 到数据集长度），然后在 `_feedback_mutation` 里三步：

1. `_get_evaluation_dataset` 把标注转成 `EvaluationDataset` 与 `y_true`；
2. `evaluate_candidate` 里 `_set_instructions(candidate)` **就地改写指标对象上的提示**，再调 `evaluate(...)` 得到 `EvaluationResult`；
3. `_get_feedbacks` 找「预测 ≠ 真值」的样本下标，从 `results.traces` 里取这些样本对应提示的输入输出，拼成 `FeedbackExample` 交给诊断提示；`_implement_feedbacks` 再把意见喂给实施提示。

两个细节值得单独记：`_set_instructions` 是**写回指标对象**（`prompts[key].instruction = val` 后 `set_prompts(**prompts)`），所以优化过程是有副作用的，中途异常会让指标停在某个候选的提示上。而 `_get_feedbacks` 若一个错样本都没有（预测全对），只打一条 warning 并返回空意见表；`_implement_feedbacks` 见到空意见就**原样保留该提示**并再打一条 warning——也就是说「已经全对」的候选会原样进入下一代，不会被无谓改写。

**`y_true` 对二元指标做了标签翻转。** `_get_evaluation_dataset` 里，被接受样本直接用 `metric_output`；**未被接受且指标 `output_type` 是 `BINARY` 的样本反转标签**（`int(not sample.metric_output)`）——因为标注只标「这条被接受了（是/否）」，对二元指标而言「未接受」本身就等于「真值是相反的标签」。非二元指标的被拒样本直接丢弃。

**交叉用的是汉明距离，而距离矩阵只填了上三角。** `cross_over_mutation` 先对每个候选评测一遍，把 `y_pred` 与 `y_true` 逐项比对成 `int(pred == true)` 向量，然后 `hamming_distance(prediction_vectors)` 得到距离矩阵。接着对每个候选取 `parent_y = candidates[np.argmin(distance_matrix[idx])]`，即「与自己差异最小」的那个当配偶，走 `_cross_over_chain`（要求两个父代键集相同，逐提示名跑 `CrossOverPrompt`）。

这里有一处必须点明的事实：`hamming_distance` 只对 `j > i` 填值，**下三角与对角线都是 0**。于是每行最小值恒落在下标 0（或对角线上的自己）。用同样代码实测：四个向量 `[1,0,1] [1,0,1] [0,1,0] [0,0,1]` 得到矩阵 `[[0,0,3,1],[0,0,3,1],[0,0,0,2],[0,0,0,0]]`，每行 `argmin` 全是 0。也就是说「按距离选最相似配偶」的意图没有落地，实际效果是**所有候选都去找第 0 号（或自己）配对**。

**适应度阶段用损失函数取值，选择却用 `argmax`。** `evaluate_fitness` 逐候选评测后算 `loss_fn(y_true, y_pred)`，收集成 `losses` 返回。`optimize` 最后一步是 `best_candidate = improved_prompts[np.argmax(fitness_scores)]`——变量名叫 fitness，装的其实是 loss。这个方向对不对取决于用哪个 `Loss`：

- `MSELoss` 返回误差（越小越好）→ `argmax` 选中的是**最差**的那个；
- `BinaryMetricLoss` 返回 accuracy 或 F1（越大越好）→ `argmax` 选中的是最好的那个。

同一个代码路径，换个损失函数就选出相反的答案。这是这一页最重要的一条使用风险。

**候选的键必须对得上指标的提示集合。** 交叉阶段要求父子键集一致（否则 `ValueError`）；`_set_instructions` 只设置候选里出现的键，没出现的保留原值。而初始化从 `batch[0]["prompts"].keys()` 建骨架，所以只要标注采集时覆盖了指标的每个提示，键集就是完整的。

## 关键取舍

**源码事实：** 基因型是普通 `Dict[str, str]`，不是 pydantic 模型。那 8 个 pydantic 类全部是四个 LLM 算子的入参/出参形状（`FormattedExamples`、`OutputInstruction`、`ParentPrompts`、`FeedbackExample`、`FeedbackMutationInput`、`FeedbackMutationOutput`、`FeedbackMutationPromptInput`），不参与候选的表示。

**源码事实：** `hamming_distance` 只填上三角，对角线为 0，所以 `np.argmin` 恒返回 0（或对角自身）。交叉阶段的「选最相似配偶」实际未生效。

**源码事实：** `optimize` 用 `np.argmax` 在 `losses` 上选最优。`MSELoss` 越小越好、`BinaryMetricLoss` 越大越好，两者会让同一份代码选出相反候选。

**源码事实：** 至少 10 条标注是硬门槛（`MIN_ANNOTATIONS`）；`population_size`/`num_demonstrations`/`sample_size` 有默认值 3/3/12，走 `config` 字典。

**源码事实：** 种子提示来自 `metric.get_prompts()`，只保留名字与候选键集有交集的那些，追加在生成群体之后——保证原始提示参与竞争。

**源码事实：** `_set_instructions` 就地改写指标对象上的提示实例；优化中途异常会让指标停留在某个候选的状态。

**源码事实：** 二元指标下未被接受的样本会被反转标签计入 `y_true`（`int(not metric_output)`），非二元指标下直接被丢弃。

**源码事实：** `feedback_mutation` 在成功路径上把 `on_chain_end` 调了**两次**——一次在 `else` 分支里（`outputs={"improved_candidate": ...}`），一次在函数末尾（`outputs={"improved candidates": ...}`）。所以每个候选的变异链会向追踪器发两条结束事件，键名还不同（单数 `improved_candidate` 与复数 `improved candidates`）。`initialize_population`、`cross_over_mutation`、`evaluate_fitness` 的对应位置都只调一次。

**架构推论：** 「遗传算法」在这里的价值不在交叉/变异的具体实现（它们是 LLM 语义操作，不可复现），而在**它给「改提示」提供了一套可计数的外部循环**：固定代数、可配群体、有评测驱动的适应度、最后能选出单一赢家。真正让它区别于一般调参工具的是「用评测分数反过来改提示」这个闭环，而不是算子本身。

**架构推论：** 由于算子是 LLM 调用，一次 `optimize` 的开销量级是 `population × (sample_size + dataset_size)` 次评测再加若干次改写调用。`MIN_ANNOTATIONS=10` 与 `population_size=3` 这些小默认值不是随手写的——它们是让「跑一次优化」保持在分钟级而非小时级的手段。

**架构推论：** 把「反推指令」当初始化算子，是这个设计里最巧的一步：它让搜索从「能解释标注的指令」附近起步，而不是从随机字符串起步。代价是初始化质量强依赖标注质量——标注错了，反推出的指令会把整个群体带偏，而且后续变异只会在这个偏差附近微调。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer` | 优化器主体：四阶段流程与全部算子调用 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer.optimize` | 入口：三道校验、四阶段调度、`argmax` 选优 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer.initialize_population` | 分层切批 + 反推指令 + 追加种子提示 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer._reverse_engineer_instruction` | 从「输入 + 期望输出」样例反推指令 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer.feedback_mutation` | 逐候选抽样评测的诊断阶段 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer._feedback_mutation` | 单候选的三步：评测、取意见、实施意见 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer._get_feedbacks` | 从判错样本与 traces 构造诊断输入 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer._implement_feedbacks` | 有意见才改写，无意见原样保留 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer.cross_over_mutation` | 评测成向量、汉明距离、配对交叉 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer._cross_over_chain` | 逐提示名交叉，要求父子键集一致 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer.evaluate_fitness` | 逐候选算损失，供 `argmax` 选择 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer.evaluate_candidate` | 写回提示并调 `evaluate` 得到评测结果 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer._set_instructions` | 就地把候选写进指标对象的提示实例 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer._get_evaluation_dataset` | 标注转评测集，含二元指标的标签翻转 |
| `src/ragas/optimizers/genetic.py` | `ReverseEngineerPrompt` | 初始化算子：样例 → 指令 |
| `src/ragas/optimizers/genetic.py` | `CrossOverPrompt` | 交叉算子：两父代 → 子代指令，自带一个示例 |
| `src/ragas/optimizers/genetic.py` | `FeedbackMutationPrompt` | 变异诊断算子：指令 + 样例 → 最多 3 条意见 |
| `src/ragas/optimizers/genetic.py` | `FeedbackMutationPromptGeneration` | 变异实施算子：指令 + 意见 → 新指令 |
| `src/ragas/optimizers/genetic.py` | `FormattedExamples` | 把样例压成「逐字段换行」的文本块 |
| `src/ragas/optimizers/utils.py` | `hamming_distance` | 只填上三角的距离矩阵 |
| `src/ragas/optimizers/base.py` | `Optimizer` | 抽象基类：`metric` + `llm` + 抽象 `optimize` |
| `src/ragas/losses.py` | `MSELoss` | 越小越好的损失（与 `argmax` 方向相反） |
| `src/ragas/losses.py` | `BinaryMetricLoss` | 返回 accuracy/F1，越大越好（与 `argmax` 同向） |

## 参考

- [ragas 源码仓库](https://github.com/explodinggradients/ragas)
- [ragas 源码：optimizers/genetic.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/optimizers/genetic.py)
- [ragas 文档：指标总览](https://docs.ragas.io/en/latest/concepts/metrics/)
- [ragas 文档首页](https://docs.ragas.io/en/stable/)

## 与主核 / AI 的关系

「用评测结果反过来改提示」与主核里「用统计信息反过来选索引/改计划」是同一种反馈环：都是先有可度量的目标函数，再让某个搜索过程在解空间里走一步、量一次、留下更好的那个。主核的搜索是确定性的（代价模型 + 枚举/DP），这里的搜索算子由 LLM 执行——于是「可复现」这个前提不成立，只能靠**多次评测取平均 + 标注分层抽样**来压低方差。

三条可直接用的判断。第一，**优化器的选择判据必须与损失方向绑定**：同一个 `argmax` 在 `MSELoss` 下选最差、在 `BinaryMetricLoss` 下选最好，这在工程上是致命的；任何「损失 + 选择」的组合都该把方向写进同一个函数里，而不是靠调用方自觉。第二，**原始解必须进候选池**：把指标自带提示追加为种子，是防止「优化后比不改还差」的最低成本保险。第三，**优化是有副作用的操作**：就地改写指标对象意味着失败会留下中间态，生产上该在改写前保存原始提示，或在克隆出的指标上优化。

另有一条关于算法的诚实结论：`hamming_distance` 只填上三角导致配对失效这件事说明，**遗传算法里「距离」与「配对策略」是两个容易脱节的点**——写测试时只看「有没有选出配偶」不够，得看「选的是不是预期那个」。这类「看起来运行正常、实际逻辑退化」的缺陷，比抛异常的缺陷难发现得多。
