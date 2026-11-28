---
project: ragas
feature: string-metrics
title: 字符串类指标
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/_string.py
    symbols: [DistanceMeasure, ExactMatch, NonLLMStringSimilarity]
  - path: src/ragas/metrics/collections/_bleu_score.py
    symbols: [BleuScore]
  - path: src/ragas/metrics/collections/_rouge_score.py
    symbols: [RougeScore]
  - path: src/ragas/metrics/collections/chrf_score/metric.py
    symbols: [CHRFScore]
---

# 字符串类指标

## 一句话职责

完全不调用模型的五个指标：完全匹配、子串存在、编辑距离相似度、BLEU、ROUGE、ChrF——它们只衡量字面重合，成本为零、结果可复现，代价是任何改述都判为差异。

## 机制是什么

这一类的共同点不是算法，而是**没有模型**。它们在 `BaseMetric` 的继承体系里做了同一件事：不定义 `llm` 字段、也不定义 `embeddings` 字段。基类构造时的两段校验是「如果实例有 `llm` 属性就校验它」，所以没有这两个属性就什么都不校验——这是「无模型指标」在架构上真正的实现方式，而不是靠一个 `requires_llm=False` 这样的标志位。

这六个实现（`ExactMatch`、`StringPresence`、`NonLLMStringSimilarity`、`BleuScore`、`RougeScore`、`CHRFScore`）的输入永远是同一对：`reference` 与 `response`。没有问题、没有上下文、没有真值答案之外的任何东西。所以它们只能回答一个问题：**生成的文本与参考文本在字面上有多接近**。

三个层次从硬到软：

| 指标 | 判据 | 输出粒度 |
|---|---|---|
| `ExactMatch` | 全字符串逐字节相等 | 0 / 1 |
| `StringPresence` | 参考是响应的子串 | 0 / 1 |
| `NonLLMStringSimilarity` | 四种编辑距离之一归一化后取补 | 连续 |
| `BleuScore` / `RougeScore` / `CHRFScore` | n-gram 或字符 n-gram 重合 | 连续 |

## 怎么实现的

**类结构的统一。** 五个类都只做三件事：`__init__` 把配置存成实例属性并调 `super().__init__(name=..., **base_kwargs)`；`ascore(reference, response)` 做一次纯计算；返回 `MetricResult(value=float(...))`。没有骨架、没有提示、没有中间模型。这是 collections 体系里最薄的一层。

**完全匹配与子串存在。** `ExactMatch` 就是 `float(reference == response)`。Python 字符串相等是逐码点比较，因此空白、全半角、大小写差异全都算不同。`StringPresence` 是 `float(reference in response)`——注意**方向**：判断的是「参考出现在响应里」，不是反过来。这个方向决定了它的用途：当参考是短答案（如「Paris」）而响应是长句时成立；反过来（参考是长段落、响应是其中一句）会判 0。

两者都先 `assert isinstance(..., str)` 断言类型。`assert` 在 `-O` 优化模式下会被剥掉，所以这类检查只在调试构建里生效——这是全库少见的用断言而非抛异常的地方。

**编辑距离的四种度量。** `NonLLMStringSimilarity` 的构造期做两件事：**动态导入** `rapidfuzz`（缺库就抛带安装指引的 `ImportError`），并把 `DistanceMeasure` 枚举的四个值映射到 rapidfuzz 的四个模块对象（Levenshtein、Hamming、Jaro、JaroWinkler）。

计算时是 `1 - normalized_distance(reference, response)`。用 `normalized_distance` 而不是原始距离，是因为原始距离随字符串长度增长，无法作为 0–1 分；归一化后取补即相似度。四种度量的语义差异很大：

- **Levenshtein**（编辑距离）：允许插入/删除/替换，衡量任意改写的字符级成本；
- **Hamming**：只允许替换，**要求两串等长**，否则 rapidfuzz 的实现会给出按长度差处理的结果；
- **Jaro / Jaro-Winkler**：基于匹配窗口与换位计数，对短串与拼写错误更敏感，后者的「Winkler」变体额外奖励共同前缀。

这四个枚举值不是「同一个距离的四种算法」，而是**四种不同的相似度定义**，选错会得到完全不同的分数分布。源码只提供枚举与映射，不做语义推荐。

**BLEU 的分句处理。** `BleuScore.ascore` 先按 `". "`（句点加空格）把参考与响应各自切成句子，然后构造 `corpus_bleu` 需要的形状：参考侧是 `[[句1], [句2], ...]`（每句一个「参考列表」），响应侧是 `[句1, 句2, ...]`。最后取 `.score / 100`——sacrebleu 返回的是 0–100 的百分制，这里除以 100 归一到 0–1。

这个切分是个隐含约束：**句子边界定义被写死成「句点 + 空格」**。中文文本通常用全角句号且后面不加空格，于是整段中文会被当成一个句子——BLEU 退化成「整段作为一个超长句子的 n-gram 重合」。这是刻意简化还是疏漏难以从代码判断，但它确实让这个指标对中文不再有意义。

**ROUGE 的模式选择。** `RougeScore` 构造时接受 `rouge_type`（`rouge1` 或 `rougeL`）与 `mode`（`fmeasure` / `precision` / `recall`），计算时 `rouge_scorer.RougeScorer([rouge_type], use_stemmer=True)` 后按 `getattr(scores[rouge_type], mode)` 取值。`rouge1` 是单词 unigram 重合，`rougeL` 是最长公共子序列。`use_stemmer=True` 打开词干化，所以「run」「running」会被视作同词——注意这是全组里唯一一处做了形态归一的实现。

三个 mode 各自含义不同：`recall` 关心「参考有多少被覆盖」，`precision` 关心「响应里有多少是参考里的内容」，`fmeasure` 是调和均值。摘要类任务通常看 recall（是否漏了要点），问答类任务看 precision（是否多说了没用的）。

**ChrF 与其他三个的差异。** `CHRFScore` 用 sacrebleu 的 `corpus_chrf`，作用在**字符 n-gram** 而非词 n-gram。这让它对形态变化与拼写差异更宽容（「booked」与「booking」共享大量字符 n-gram），代码注释解释的理由是「更适合形态丰富的语言」。

它的输入校验也是全组最完整的：先判两个参数是否都是字符串（非法则返回 0.0 加 reason），再判去空白后是否为空（空则返回 0.0 加 reason），最后才调用。**与 `ExactMatch`/`StringPresence` 用断言、`BleuScore` 用断言的做法不同**——三个实现的校验风格三种，说明这组指标是在不同时间点陆续补齐的。

**取值范围的一致性。** 六个指标都声明默认 `allowed_values=(0.0, 1.0)`，其中五个天然落在这个区间内（0/1 或归一化相似度）。BLEU 与 ChrF 靠除以 100 落回区间。值得注意的是 sacrebleu 的分数在极端情况下可能略超 100（语料级平滑的副作用），除以 100 后可能得到略大于 1 的值——而数值校验是「声明范围」而非「强制裁剪」，这种越界值会被原样记录。

**返回值的 reason 缺失。** 除了 `CHRFScore` 在错误分支给出了 `reason`，其他五个的成功路径都是裸 `MetricResult(value=...)`。所以这组指标的分数无法自解释——不知道命中了几个 n-gram、不知道编辑距离的具体值。这与 B 组指标「reason 里带判定理由」的惯例相反，是「零模型指标没有判定过程可解释」的直接后果。

## 关键取舍

**源码事实：** 「无模型」是通过不定义 `llm`/`embeddings` 属性实现的，基类只在属性存在时才校验。这比显式标志位更省，但也更隐晦——读者必须知道基类那段 `hasattr` 判断才能理解为什么这些类能跳过校验。

**源码事实：** 三个实现用了三种输入校验风格：`ExactMatch`/`StringPresence` 用 `assert`（优化模式下失效），`BleuScore` 用 `assert`，`CHRFScore` 返回带 reason 的 0.0，`NonLLMStringSimilarity` 用 `assert`。风格不统一说明它们各自独立补齐，没有统一约定。

**源码事实：** BLEU 的分句用 `". "` 硬切。中文文本因此退化为单句，指标失去原有语义。

**源码事实：** 只有 ROUGE 打开了词干化，其余指标都做字面比较。同一对文本在不同指标下可能得到差异很大的相似度。

**源码事实：** 无引用片段时（不适用于这组，但类似情形）——这组的空值处理是：BLEU/ChrF 空串会得到极低分（分母为零或 n-gram 无匹配），`ExactMatch` 两个空串会得到 1.0。

**架构推论：** 这组指标的价值定位是**回归门禁**而不是质量度量。零成本意味着可以每次提交都跑全部样本；可复现意味着分数变化必然对应文本变化，不会被模型随机性干扰。反过来，它们的分数**不能用来比较两个语义等价但措辞不同的系统**——改述会被判为全错，这是字面指标的固有边界，不是缺陷。

**架构推论：** 把 BLEU/ROUGE/ChrF 放在「非 LLM 指标」组而不是单独一类，体现了 ragas 的分类依据是**是否调用模型**，而不是**算法族**。这个分类对使用者更实用：想看「哪些指标能离线跑、不花钱」，答案就在这一组。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/_string.py` | `DistanceMeasure` | 四种距离度量的枚举 |
| `src/ragas/metrics/collections/_string.py` | `ExactMatch` | 逐码点完全匹配，输出 0/1 |
| `src/ragas/metrics/collections/_string.py` | `StringPresence` | 参考是否为响应子串 |
| `src/ragas/metrics/collections/_string.py` | `NonLLMStringSimilarity` | 归一化距离取补，构造期动态导入 rapidfuzz |
| `src/ragas/metrics/collections/_bleu_score.py` | `BleuScore` | 按句点分句后算语料级 BLEU，除以 100 |
| `src/ragas/metrics/collections/_rouge_score.py` | `RougeScore` | ROUGE-1 / ROUGE-L 三种聚合模式，开词干化 |
| `src/ragas/metrics/collections/chrf_score/metric.py` | `CHRFScore` | 字符 n-gram F 分数，输入校验最完整 |

## 参考

- [ragas 源码：collections/_string.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/_string.py)
- [ragas 源码：collections/_bleu_score.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/_bleu_score.py)
- [ragas 源码：chrf_score/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/chrf_score/metric.py)
- [BLEU 原始论文（Papineni et al., 2002）](https://aclanthology.org/P02-1040/)
- [ChrF 论文（Popović, 2017）](https://aclanthology.org/W17-4770/)

## 与主核 / AI 的关系

**零成本指标的定位等同于主核里的「格式校验」。** 主核里 `CHECK` 约束与类型校验不查语义，只查形式，因此可以放在每次写入路径上；这组字符串指标同样不查语义、只查字面，因此可以放在每次评测路径上。两者的共同前提是：**便宜的检查应该覆盖全量，贵的检查只能抽样。**

**「可复现」是这类指标最大的工程价值。** 主核的性能基准之所以可信，是因为它不引入随机性；这组指标之所以能当门禁用，同样因为它不引入随机性。任何带模型的指标在两个版本之间的分数波动，都无法区分「系统变差了」与「模型今天心情不同」——纯字面指标没有这个问题，所以它给出的回归信号是干净的。

**归一化与越界的处理。** BLEU/ChrF 除以 100 把百分制拉回 0–1，与主核把不同单位的指标映射到统一量纲是同一动作。而「声明了范围但不裁剪」这一点又与主核一致：约束是描述性的，不是强制的。

对 AI 工程最实用的四条：其一，**字面指标不能跨系统比较**，只能同系统跨版本比较；换了措辞风格的两版系统，BLEU 差异没有意义。其二，**中文场景慎用 BLEU**，分句规则失效会让分数退化成整段比较。其三，**ROUGE 要选对 mode**，摘要看 recall、问答看 precision，选错会把「啰嗦」和「漏要点」混为一谈。其四，**这组指标的 reason 是空的**，分数异常时无法从结果里定位原因，需要自己拿文本做一次手算比对。
