---
project: ragas
feature: topic-and-summary
title: 主题一致性与摘要质量
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/topic_adherence/metric.py
    symbols: [TopicAdherence]
  - path: src/ragas/metrics/collections/summary_score/metric.py
    symbols: [SummaryScore]
---

# 主题一致性与摘要质量

## 一句话职责

两条不比较「回答与参考答案」的指标：**主题一致性**衡量多轮对话有没有跑题——先抽出对话谈到的主题，再判每个主题是否被回答（而非拒答）、是否属于允许的主题集；**摘要质量**衡量摘要是否抓住了原文要点——把原文抽成关键词、转成是非问句，再用摘要作答，答得上的比例就是质量分。

## 机制是什么

两条指标都绕开了「参考答案」这个依赖，各自换了一种间接度量：

| 指标 | 输入 | 中间产物 | 判据 |
|---|---|---|---|
| `TopicAdherence` | 多轮消息列表 + 允许主题列表 | 对话中的主题、每个主题是否被回答、是否属于允许集 | 主题级精确率 / 召回率 / F1 |
| `SummaryScore` | 原始上下文 + 摘要 | 关键词、是非问句、摘要对这些问句的回答 | 问句答对比例（可选简洁度惩罚） |

**主题一致性是两段式。** 第一步不问「有没有跑题」，而问「这段对话都谈了些什么」——把对话压成一串主题名。第二步对每个主题做两件事：查它有没有被拒答（`refused_to_answer`，回答为真则说明没答），以及查它是否落在允许主题集内（`classifications`）。前者与后者交叉得到四格，于是又能算精确率/召回率/F1。

这个设计的价值在于它能处理**多轮**：一条回答可能同时涉及几个主题，逐个主题判「答了没答、允不允许」比整体判「这段对话切题吗」更可控。代价是判定次数随主题数增长——每个主题一次拒答判定，再加一次批量分类。

**摘要质量是三步反问链路。** 它不问「这份摘要好不好」，而是构造一套「原文能回答的问题」，再看摘要能回答几个：

1. 从原文抽关键词（限定类型：人名、组织、地点、时间、金额、百分比）。
2. 由关键词 + 原文生成**封闭式问句**，提示词明确要求「这些问题基于原文应该always回答 1」。
3. 用摘要逐个作答（只能答 `'1'` 或 `'0'`）。
4. 答 `'1'` 的比例即问答得分。

这是「反向提问」手法在摘要场景的变体：与答案相关性那里的「从回答生成问题」同源，但方向不同——那里是「从回答反推它在答什么」，这里是「从原文出题，考摘要」。

## 怎么实现的

**`TopicAdherence.ascore` 的四步。** `ascore(user_input, reference_topics)`：

1. 入参校验：`user_input` 必须是消息列表（`HumanMessage` / `AIMessage` / `ToolMessage` 的联合），`reference_topics` 必须是非空列表。
2. `_format_conversation`：对每条消息调 `pretty_repr()` 拼成多行文本。
3. `_extract_topics`：`TopicExtractionPrompt` 渲染后调 `llm.agenerate(prompt_str, TopicExtractionOutput)`，取 `result.topics`。**主题为空直接返回 NaN**——与上下文召回率的取向一致（无数据不是差评）。
4. 后续三步：`_check_topics_answered` 逐主题判拒答、`_classify_topics` 批量分类、`_compute_score` 按模式算分。

**拒答判定是逐主题串行的一次调用。** `_check_topics_answered` 里 `for topic in topics` 每个主题渲染一次 `TopicRefusedInput(user_input=conversation, topic=topic)`，调一次 LLM，取 `refused_to_answer`，随后**取反**存入布尔数组——`answered.append(not result.refused_to_answer)`。因此「已答」是用「非拒答」定义的：只要没被判为拒答就算答了。这让「答了一半」这类中间态被归到「已答」。

**分类判定是一次批量，且带长度对齐。** `_classify_topics(reference_topics, topics)` 一次性把两个列表交给 `TopicClassificationPrompt`，得 `classifications`（布尔数组）。然后：

- `_safe_bool_conversion` 把结果转成布尔数组：已经是布尔就用；整数（多种位宽）就 `astype(bool)`；字符串/对象类型就逐项判定，**字符串只认 `"true"` / `"1"` / `"yes"` 三种小写形式**；其他一律 `bool(item)`。
- 若长度与 `topics` 不一致：短了就补 `False`（`np.zeros` + `concatenate`），长了就截断。

**补 `False` 的语义是「不属于允许集」**，也就是说模型少给一条分类，会被当成「这条主题越界」。配合精确率的计算，这会把分数往下拉。这是一处**失败方向不对称**的选择：宁可判越界，不判合规。

**`_compute_score` 的三档。** 把两个布尔数组交叉：

- TP = 已答 **且** 属允许集；
- FP = 已答 **但** 不属允许集（回答了不该答的）；
- FN = **未答但**属允许集（该答的没答）。

分母都加 `1e-10` 防除零。三档分别是 `TP/(TP+FP)`、`TP/(TP+FN)`、以及两者的调和平均。

这个交叉方式定义很清晰：精确率惩罚「越界回答」，召回率惩罚「该答不答」，F1 是折中。**它同时把「拒答正确」计入召回**：一个属允许集的主题被拒答，会算作 FN，从而拉低召回——这正是「该答而没答」的惩罚。

**`SummaryScore.ascore` 的五步。** `ascore(reference_contexts, response)`：

1. 入参校验：上下文不能为空或全空白，摘要不能为空或全空白，各自 `ValueError`。
2. `text = "\n".join(reference_contexts)` 拼接原文，`_extract_keyphrases(text)` 抽关键词。
3. `_generate_questions(text, keyphrases)` 出题。
4. `_generate_answers(response, questions)` 用摘要作答。
5. `_compute_qa_score(answers)` 算比例；若 `length_penalty` 为真，再算简洁度并按 `coeff` 加权合并。

**空结果的处理是「记日志并继续」。** 关键词为空 → `logging.error("No keyphrases generated...")`，置 `keyphrases = []` 继续；问题为空同样处理。注释写的是「Match legacy behavior」。

于是流程会继续往下走，最终在 `_compute_qa_score` 里遇到 `correct / len(answers)`——**答案列表为空时抛 `ZeroDivisionError`**。源码注释明确承认这点：`# Will raise ZeroDivisionError if answers is empty (legacy behavior)`。

这是新式指标里少见的**主动保留崩溃行为**：为了让新旧分数可比，选择与旧实现一致地报错，而不是改成 NaN 或 0。它同时也说明「问题为空 → 答案为空 → 除零」是一条真实可达的路径，不是理论情形。

**问答得分是精确字符串匹配。** `_compute_qa_score` 里 `sum([1 for a in answers if a.lower() == "1"])`——只认字面 `"1"`（大小写无关）。答 `"yes"` 或 `"true"` 都被算错。提示词里强调「Answers should STRICTLY be either '1' or '0'」，这个实现是对提示词的硬要求。

**简洁度惩罚是个线性比例。** `_compute_conciseness_score(text, summary)` 返回 `1 - min(len(summary), len(text)) / (len(text) + 1e-10)`。也就是：摘要越短分越高，短到 0 长度得 1.0，与原文等长得约 0。注意它用 `min(...)`，所以摘要比原文还长时按原文长度算（恰好得 0，不会为负）。

**加权合并是线性的。** `final = qa_score * (1 - coeff) + conciseness_score * coeff`。`coeff` 默认 0.5，构造期校验必须在 0 到 1 之间。当 `coeff = 0` 时只用问答分，`coeff = 1` 时只用简洁度——后者显然荒谬（一份空摘要得满分），但配置上允许。

**三个提示词对象的装配在 `__init__` 里。** `SummaryScore.__init__` 显式赋 `self.llm`、`length_penalty`、`coeff`，并创建三个提示词实例，然后校验 `coeff`，最后才 `super().__init__(name=name, **kwargs)`——**不把 `llm` 传进 kwargs**，因为基类靠 `hasattr` 去找它（这是新式基类的时序契约）。

**每步都是「提示词对象 → 字符串 → 结构化输出」。** 例如 `_extract_keyphrases`：`ExtractedKeyphrasesInput(text=text)` → `prompt.to_string(input_data)` → `await llm.agenerate(prompt_str, ExtractedKeyphrases)` → `result.keyphrases`。全程没有文本解析。

**两条指标共享同一个结构骨架。** 都是「抽取 → 逐项判定 → 聚合」，都带 `BasePrompt` 子类的提示词对象，都在入参校验里给修复指引，都有空值分支。差别只在中间判定的形态：主题一致性是「主题 × 两个布尔维度」，摘要质量是「问句 × 答对与否」。

## 关键取舍

**源码事实：** 主题一致性把「已答」定义为「非拒答」，所以「答了一半」归为「已答」；分类结果长度不足时补 `False`（判越界），失败方向不对称。

**源码事实：** 主题抽取为空返回 NaN（无数据不判分），而分类长度不足补 False（缺数据判越界）。同一条指标里对两类缺失给了相反处理。

**源码事实：** 摘要质量的问答得分只认字面 `"1"`；关键词或问题为空时继续执行，最终在答案为空处抛 `ZeroDivisionError`，且注释说明这是为对齐旧行为。

**源码事实：** 简洁度 `1 - min(len(summary), len(text)) / (len(text)+1e-10)`，摘要为空时得 1.0；`coeff` 允许 0 与 1 两个端点，端点含义分别是「只看问答」与「只看简洁度」。

**架构推论：** 两条指标都不需要参考答案，所以能用在「没有标注数据」的线上场景。但代价是判据都变成了代理：主题一致性的分数实际衡量「话题覆盖与越界」，摘要质量的分数实际衡量「关键词层面的可问答性」——**它抓不住摘要的组织逻辑与行文质量**。用它们做门禁可以，做质量排名要谨慎。

**架构推论：** 摘要质量的三步链路里，每一步的质量都会向下传递：关键词抽少了 → 问题少了 → 分母小了。分母变小是双刃的：答对比例可能因样本变少而虚高。这也解释了为什么关键词为空时会记 error 而不是静默：「没有关键词」不是「摘要差」，而是「这一轮判定失效」。

**架构推论：** `coeff` 的线性加权让两个尺度不同的量直接相加。问答分是比例（0 到 1），简洁度也是比例（0 到 1），形式上可比，但语义上并不等价：一份内容完整但冗长的摘要，其「简洁度低」会被当作质量差，而实际上「完整」才是摘要的第一要求。默认 0.5 的权重意味着**冗长被罚得相当重**，这个默认值是有主张的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/topic_adherence/metric.py` | `TopicAdherence` | 主题一致性：抽取 → 拒答判定 → 分类 → 三档计分 |
| `src/ragas/metrics/collections/topic_adherence/metric.py` | `TopicAdherence.ascore` | 消息格式校验、主题为空返回 NaN |
| `src/ragas/metrics/collections/topic_adherence/metric.py` | `_check_topics_answered` | 逐主题一次调用，非拒答即已答 |
| `src/ragas/metrics/collections/topic_adherence/metric.py` | `_classify_topics` | 批量分类 + 长度对齐（不足补 False） |
| `src/ragas/metrics/collections/topic_adherence/metric.py` | `_safe_bool_conversion` | 布尔/整数/字符串三种形态归一 |
| `src/ragas/metrics/collections/topic_adherence/metric.py` | `_compute_score` | TP/FP/FN 交叉，三档与 eps 防零 |
| `src/ragas/metrics/collections/summary_score/metric.py` | `SummaryScore` | 摘要质量：五步链路 + 简洁度加权 |
| `src/ragas/metrics/collections/summary_score/metric.py` | `SummaryScore.__init__` | 组件先装配、`coeff` 区间校验、再调基类 |
| `src/ragas/metrics/collections/summary_score/metric.py` | `_compute_qa_score` | 只认字面 `"1"`，空答案抛除零 |
| `src/ragas/metrics/collections/summary_score/metric.py` | `_compute_conciseness_score` | 长度比线性惩罚 |
| `src/ragas/metrics/collections/summary_score/util.py` | `GenerateQuestionsPrompt` | 出题提示：要求基于原文恒答 1 |
| `src/ragas/metrics/collections/topic_adherence/util.py` | `TopicRefusedPrompt` | 判「是否拒答」的提示词 |

## 参考

- [ragas 源码：collections/topic_adherence/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/topic_adherence/metric.py)
- [ragas 源码：collections/summary_score/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/summary_score/metric.py)
- [ragas 文档：主题一致性](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/topic_adherence/)
- [ragas 文档：摘要质量](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/summary_score/)

## 与主核 / AI 的关系

主题一致性的「允许集 + 越界判定」与主核里「访问控制白名单」是同一种结构：都有一个明确的允许集合，判定的是「请求落不落在集合内」，而不是「请求好不好」。差别是主核的白名单判定是确定比较（字符串/角色相等），这里的判定是模型分类——所以它必须额外做长度对齐、类型归一这些数据库里不存在的脏活。**判定器不确定时，判定结果的结构必须被防御。**

摘要质量的「出题再作答」与主核里「用约束检查代替直接比较」同源：与其衡量结果本身，不如构造一组必须成立的性质再逐条验证。约束检查的优点是每一条都可解释、可定位；缺点是**它只覆盖被写出来的性质**——摘要里那些没被关键词覆盖的缺陷，这条链路永远看不见。

对 agent 工程的三条实际结论：其一，`TopicAdherence` 的「非拒答即已答」让拒答成为唯一负面信号，若系统倾向于含糊作答（既不拒答也不正面回答），分数会虚高。其二，`SummaryScore` 的空关键词路径会一路走到除零，**必须在调用点用 try 包住**，否则一条缺少关键词的样本会中断整个评测批次。其三，`coeff=0` 是「只用问答分」的正规做法；把 `coeff` 调大来鼓励简洁，会同时奖励「信息更少」的摘要。
