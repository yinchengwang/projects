---
project: ragas
feature: noise-sensitive
title: 噪声敏感度与引用片段
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/noise_sensitivity/metric.py
    symbols: [NoiseSensitivity]
  - path: src/ragas/metrics/collections/quoted_spans/metric.py
    symbols: [QuotedSpansAlignment]
---

# 噪声敏感度与引用片段

## 一句话职责

噪声敏感度衡量「检索结果里混进干扰文档时，答案被带偏了多少」；引用对齐衡量「答案里用引号标出的片段，到底有没有在原文里出现过」。前者是检索质量的抗干扰度量，后者是引用的字面核对。

## 机制是什么

两个指标都围绕「上下文与答案之间的对应关系」，但问的问题方向相反：

- `NoiseSensitivity` 问：**错误的那些结论，是被哪一类上下文带出来的**。它需要对照真值答案，把答案里错的陈述归因到「相关上下文」「无关上下文」或「真值本身」。
- `QuotedSpansAlignment` 问：**答案声称引用原文的那些片段，真的在原文里吗**。它不需要模型，纯字符串匹配，也不需要真值答案。

噪声敏感度的取值方向与其他指标相反：**越低越好**。0 表示答案里的错误没有一个是被检索上下文带偏的，1 表示所有错误都源于上下文干扰。

## 怎么实现的

**噪声敏感度的三段分解。** `ascore` 收四个参数：问题、答案、真值答案、检索上下文列表。流程是：

1. 把**真值答案**与**答案**各自分解成原子陈述（同一次超级步里调用两次陈述生成）。
2. 对**每一条**检索上下文，分别把真值陈述与答案陈述送去 NLI 判定，得到两个「陈述 × 上下文」的布尔矩阵；再把答案陈述对**真值答案本身**判一次，得到 `ground_truth2answer` 的一维向量。
3. 用矩阵运算合成三种归因。

第二步的循环是 O(上下文数) 次模型调用（每次调用判一批陈述），这是全项目里最贵的指标之一。之所以要逐上下文判，是因为归因必须知道「这条错误陈述被哪块上下文支持过」——合并上下文会丢掉这个信息，那正是指标要的答案。这与 b01 忠实度「合并上下文换单次调用」的取舍正好相反，理由也正好相反。

**矩阵形状的拼接。** 三个键分别是：

- `retrieved2ground_truth`：真值陈述 × 上下文的判定矩阵（每个上下文一列，转置后成「陈述 × 上下文」）。
- `retrieved2answer`：答案陈述 × 上下文的判定矩阵。
- `ground_truth2answer`：答案陈述能否被真值支持的一维结果，外面又套了一层数组以对齐形状。

三者最后统一 `astype(bool)`。这一步不是美化，`numpy` 的按位与在整数与布尔上语义不同，先转布尔才能让后面的 `&`、`~` 按逻辑运算解释。

**三种归因的算式。** `incorrect = ~ground_truth2answer`——只关心「真值不支持的那些答案陈述」。

「相关上下文」的判定是两步：先按列取 `max`，得到「这条真值陈述是否被任何一条上下文支持」；再把这个行向量与 `retrieved2answer` 按位与后按行取 `max`，得到「这条答案陈述是否被某条**与真值相关**的上下文支持」。

`relevant` 模式直接返回 `mean(relevant_faithful & incorrect)`：错误陈述里，有多少被相关上下文支持过。

`irrelevant` 模式先取 `~relevant_retrieved` 得到无关上下文的列掩码，同样算出「被无关上下文支持」的布尔向量，然后**再与 `~relevant_faithful` 相与**——这一步是为了让两类互斥，防止一条同时被相关与无关上下文支持的陈述被两边都计一次。最后返回它与 `incorrect` 的合取均值。

**模式在构造期定死。** `mode` 参数（`relevant` / `irrelevant`）在 `__init__` 里校验取值并把提示对象建好，之后 `ascore` 只读不写。所以同一个指标实例只能算一种归因；要两种都看就建两个实例，各自跑一遍全套模型调用——**矩阵是逐上下文现算的，没有缓存**，这是重复成本的来源。

**引用对齐的三步纯字符串。** `QuotedSpansAlignment.ascore`：

1. 用一条正则从答案里抠出引号包住的片段。正则的引号字符集覆盖了直引号、中文/欧式双引号、单引号、反引号与重音符，左引号与右引号是同一个集合（不配对检查左右类型），所以「用中文左引号开头、英文右引号结尾」也能匹配。
2. 按词数过滤：`len(span.split()) >= min_span_words`（默认 3）。短片段被丢弃，注释写明是为了避免误命中——「AI」「0.5」这类短串在任何长文本里都能找到。
3. 把所有源文档**用空格拼接成一整段**，归一化（折叠空白 + 转小写，可关闭），然后逐片段做子串包含判断。

分数是命中数 / 总片段数；**没有片段时返回 1.0**（注释「No quoted spans found in response」），理由是这个指标只评「引对了没有」，不评「有没有引用」。

**输入类型的软校验。** 这个指标对 `response` 非字符串、`retrieved_contexts` 非列表**不抛异常**，而是返回 `value=0.0` 加一条 reason 说明。这与同一组里其他指标的硬失败取向不同，是刻意的：它是零成本的纯字符串指标，调用方可能把整批数据直接灌进来，一条脏数据不该炸掉整次评测。

**归一化的两个后果。** 用空格拼所有源文档，意味着一个跨文档边界的片段会被判为命中——段落 A 的结尾与段落 B 的开头连起来恰好等于引文时，匹配会成功。同时，因为只做「片段是否出现在源中」的单向判定，**引文被截断、篡改、拼接都不会被发现**：只要片段整体出现在源文本里就算对。

## 关键取舍

**源码事实：** 噪声敏感度必须逐上下文判定，因此模型调用次数随上下文数线性增长；代价换来的是「错误陈述被哪一类上下文支持」这个归因信息，合并上下文就拿不到。

**源码事实：** 两种模式共用同一套矩阵计算，但每次 `ascore` 都重算全套。同一个样本要两种归因，必须跑两遍。

**源码事实：** `irrelevant` 模式额外做了一次 `&= ~relevant_faithful` 的互斥处理。没有这一步，一条被相关上下文与无关上下文同时支持的错误陈述会在两种模式里都被计入。

**源码事实：** 引用对齐在「没有引用片段」时返回满分而非 NaN 或 0。这是一种典型的「真空真」：不引用就无所谓引错。

**源码事实：** 引用对齐的源文本是「所有文档空格拼接后的整串」。跨文档边界的伪命中由此产生，且无法从结果里分辨。

**架构推论：** 两个指标代表了评测里两种成本极端：噪声敏感度是**全项目最贵**的指标之一（O(上下文) 次多段 NLI 判定），引用对齐是**最便宜**的一类（无模型、纯正则加子串）。它们面向完全不同的问题频次——前者适合离线深度分析、小样本；后者适合全量数据的每次回归。

**架构推论：** 「真空真给满分」与 b01 里「空陈述给 NaN」形成对照。两者的区别在语义：忠实度的「没有陈述可判」确实是「无数据」（不知道是否忠实）；引用对齐的「没有引用」是「确定没有引错」（这个指标的问题域被限定在已引用的片段）。**判断该给满分还是 NaN，取决于指标的问题域是否覆盖了空集**——这条区分是设计评测指标时最容易搞错的地方。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/noise_sensitivity/metric.py` | `NoiseSensitivity` | 噪声敏感度主体：分解 → 逐上下文 NLI → 归因 |
| `src/ragas/metrics/collections/noise_sensitivity/metric.py` | `_decompose_answer_into_statements` | 把答案或真值拆成原子陈述 |
| `src/ragas/metrics/collections/noise_sensitivity/metric.py` | `_evaluate_statement_faithfulness` | 对单条上下文做批量陈述判定 |
| `src/ragas/metrics/collections/noise_sensitivity/metric.py` | `_compute_score` | 相关/无关两种归因的矩阵合成 |
| `src/ragas/metrics/collections/noise_sensitivity/util.py` | `StatementGeneratorPrompt` | 复用公共陈述生成提示 |
| `src/ragas/metrics/collections/noise_sensitivity/util.py` | `StatementFaithfulnessPrompt` | 复用公共 NLI 判定提示 |
| `src/ragas/metrics/collections/quoted_spans/metric.py` | `QuotedSpansAlignment` | 引用对齐主体，含软校验与真空真返回 |
| `src/ragas/metrics/collections/quoted_spans/util.py` | `extract_quoted_spans` | 正则抠引号片段并按词数过滤 |
| `src/ragas/metrics/collections/quoted_spans/util.py` | `count_matched_spans` | 拼接源文本后逐片段子串匹配 |
| `src/ragas/metrics/collections/quoted_spans/util.py` | `normalize_text` | 空白折叠与大小写归一 |

## 参考

- [ragas 源码：noise_sensitivity/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/noise_sensitivity/metric.py)
- [ragas 源码：quoted_spans/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/quoted_spans/metric.py)
- [ragas 文档：噪声敏感度指标](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/noise_sensitivity/)
- [RAGAS 论文（Es et al., 2023）](https://arxiv.org/abs/2309.15217)

## 与主核 / AI 的关系

**归因需要逐单元记录，聚合会丢掉归因能力。** 噪声敏感度之所以贵，是因为它要求「每条错误陈述 × 每条上下文」的完整矩阵。这与主核里「慢查询分析要靠 per-operator 统计而非全局耗时」是同一个道理：一旦你把上下文合并成一个字符串，就只剩下「有没有被支持」，永远拿不回「被哪块支持」。**可归因性不是免费的，它要求数据在正确的粒度上保留下来。**

**真空真 vs 无数据，这条区分在两个项目里都出现过。** 引用对齐「没有引用 → 满分」、忠实度「没有陈述 → NaN」，对应主核里「空集合的聚合值」该怎么定义。数学上空集的 `max` 与「无定义」是两回事，工程上必须明确选一个——选错的后果是聚合统计被污染。

**纯字符串指标的定位：便宜到可以每次跑。** 引用对齐零模型成本，因此可以作为回归门禁的一部分，每次提交都跑；噪声敏感度贵到只能抽样子跑。这对应主核里「轻量校验进 CI、重量级压测按计划跑」的分层。**把指标按成本分层，比把所有指标都塞进一条流水线更实际。**

对 agent 工程最实用的三条：其一，**抗干扰能力必须单独度量**，答案整体质量高不代表它没被噪声带偏——两者可以同时成立。其二，**归因指标要接受它贵**，任何「把上下文合并省调用」的优化都会让归因失效。其三，**引用对齐只能发现「引错」，发现不了「该引而未引」**；要抓漏引，必须另设一个「答案里的关键结论是否带引用」的指标，用意是覆盖空集以外的那一半。
