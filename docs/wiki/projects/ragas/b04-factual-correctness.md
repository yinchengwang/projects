---
project: ragas
feature: factual-correctness
title: 事实正确性与声索分解
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/factual_correctness/metric.py
    symbols: [FactualCorrectness]
  - path: src/ragas/metrics/_factual_correctness.py
    symbols: [ClaimDecompositionPrompt, DecompositionType]
---

# 事实正确性与声索分解

## 一句话职责

把「回答在事实层面对不对」变成一次集合并运算：先把回答与参考答案各自拆成原子声索，再**双向**做 NLI 判定（回答的声索能否被参考答案支持、参考答案的声索能否被回答覆盖），于是得到 TP / FP / FN 三个计数，最后由它们算出精确率、召回率或 F-beta。

## 机制是什么

这条指标与忠实度共享「分解 → 判定 → 聚合」的骨架，但有两点根本不同：

**一，判定方向是双向的。** 忠实度只判一次：答案的陈述能否被上下文支持——所以它只有一个比例。事实正确性要判两次，因为「回答对」既要求「回答说的都对」（精确率视角）也要求「参考答案说的都说了」（召回率视角）。两次判定得到两个独立的布尔数组，它们构成 TP/FP/FN：

| 符号 | 含义 | 从哪来 |
|---|---|---|
| TP | 回答的声索被参考答案支持 | 第一次判定的真值计数 |
| FP | 回答的声索不被参考答案支持（编造） | 第一次判定的假值计数 |
| FN | 参考答案的声索没被回答覆盖（遗漏） | 第二次判定的假值计数 |

**二，声索分解的粒度是可配置的。** `atomicity`（原子性：低/高）与 `coverage`（覆盖度：低/高）两个维度组合成四种分解风格，每种在提示词里配有专门的少样本示例。低原子性把多个事实合并成一条宽泛声索，高原子性把每个事实切成独立声索；低覆盖度只取要点，高覆盖度要求穷尽。

粒度会直接改变分数：声索拆得越细，TP/FP/FN 的计数越分散，同样的内容会得到不同的精确率与召回率。所以**这条指标的分数字义必须连同 `atomicity` / `coverage` 一起记录**，与 `beta` 和 `mode` 一样都是定义的一部分。

## 怎么实现的

**`ascore` 的两趟判定。** 新式实现（`collections/factual_correctness/metric.py`）的 `ascore(response, reference)`：

1. 入参校验：`response` 与 `reference` 都非空，否则 `ValueError`，消息带「请把该字段加进测试样本」的指引。
2. 第一趟必做：`_decompose_and_verify_claims(response, reference)`——拆 `response` 的声索、用 `reference` 作为前提做 NLI。
3. 第二趟按模式：`mode != "precision"` 时做 `_decompose_and_verify_claims(reference, response)`（拆 `reference` 的声索、用 `response` 作前提）；`precision` 模式下直接给一个**空布尔数组** `np.array([], dtype=bool)`，不做第二趟。
4. 计数：`tp = sum(第一趟)`、`fp = sum(~第一趟)`、`fn = sum(~第二趟)`（`precision` 模式下 `fn = 0`）。
5. 按模式算分：精确率 `tp / (tp + fp + 1e-8)`、召回率 `tp / (tp + fn + 1e-8)`、其余走 `fbeta_score(tp, fp, fn, beta)`。
6. **`np.round(score, 2)`**——分数被截到两位小数。

第 6 步是这条指标一个不显眼但重要的特性：它把连续值量化成百分位，因此「0.67 与 0.68」之间的差异不可见。对单条样本影响不大，但跨版本做细粒度对比时会丢失分辨率。

**`_decompose_and_verify_claims` 的两段短路。** 它先拆声索；**拆不出来就返回空布尔数组**（不是 NaN，也不是满分）。再判定；**判定结果为空也返回空数组**。

于是「声索为空」会被下游当成「TP=0、FP=0」，精确率得 `0 / 1e-8 = 0`，召回率同样 0，F-beta 因为 `precision == 0 and recall == 0` 返回 0.0。**换句话说：拆不出声索等同于「全错」，而不是「无法判」**。这与忠实度在同类情形下返回 NaN 的取向相反。

如果只有一侧为空而另一侧正常，结果会更微妙：例如 `response` 拆出了声索但 `reference` 拆不出来（`fn` 用空数组算得 0），召回率会得满分（`tp / (tp + 0)`），而精确率仍按第一次判定算。**一侧失败会把分数推向另一个方向的极端**，这是双向判定在空值处理上不统一的后果。

**`mode` 与 `beta` 都参与了定义。** `beta` 的类型校验在新旧两代实现里都是硬性的：新式要求 `isinstance(beta, (int, float))`（接受整数），旧式 `__post_init__` 里用 `type(self.beta) is not float`（**只接受浮点，传 `1` 会被拒绝**）。两代对同一参数的校验严格程度不同。

`beta > 1` 偏向召回（更在意「参考答案说的都被覆盖」），`beta < 1` 偏向精确（更在意「回答说的都对」）。而 `mode` 直接决定用不用 F-beta：选 `precision` 或 `recall` 时 `beta` 完全无效。

**旧式实现的工艺差异。** `metrics/_factual_correctness.py` 里的 `FactualCorrectness` 是 `MetricWithLLM + SingleTurnMetric`，流程与新式对齐但实现手法不同：

- 两趟判定用 `asyncio.gather` 并发；`precision` 模式下第二趟用一个 `_get_passthrough_value` 协程返回空数组，保证 `gather` 的两支都有 Future。新式是串行两趟。
- 声索拆解与 NLI 判定各自是独立方法（`decompose_claims` / `verify_claims`），判定走 `nli_prompt.generate(data, llm, callbacks)`，回调贯穿全程。新式走 `llm.agenerate(prompt_str, OutputModel)`，没有回调。
- 计数用 `sum(...)` 而不是 `np.sum(...)`，结果同。
- 同样 `np.round(score, 2)`。

所以**新式不是旧式的严格超集**：新式少了回调与并发，多了结构化输出的强类型保证。

**`DecompositionType` 是旧式的粒度枚举。** 四个值把两个维度拼成字符串：`low_atomicity_low_coverage`、`low_atomicity_high_coverage`、`high_atomicity_low_coverage`、`high_atomicity_high_coverage`。旧式 `__post_init__` 里做的是：

1. 由 `self.atomicity` 与 `self.coverage` 拼出 `f"{atomicity}_atomicity_{coverage}_coverage"`；
2. **先把 `claim_decomposition_prompt.examples` 置空**（注释写明是为了「隔离到本实例，防止与其他指标交叉污染」）；
3. 遍历 `DecompositionType`，匹配那条就把预设示例 `extend` 进去；
4. 一条都没匹配上就打 warning。

第 2 步及其注释说明这里曾出过真实的跨实例污染 bug：提示词类是共享对象的话，改它的 `examples` 会影响所有指标。置空 + 重建是修法。注意这是**实例级隔离**——它改的是 `self.claim_decomposition_prompt`，而提示词对象本身是该实例私有的。

**新式用另一套手法解决同一问题。** `collections/factual_correctness/util.py` 里的 `ClaimDecompositionPrompt` 把四套示例存进类级字典 `_all_examples[("low","low")]` 等，`examples` 默认取 `("low","low")` 那套。关键在 `to_string`：

1. 由 `input_data.atomicity` 与 `input_data.coverage` 组成键；
2. **暂存**当前 `self.examples`；
3. 临时换成该键对应的示例集；
4. `super().to_string(input_data)` 渲染；
5. `finally` 里恢复原值。

也就是说新式把「切换示例」从构造期挪到**渲染期的临时改动**，并用 `try/finally` 保证恢复。这在并发渲染同一提示词对象时仍有竞态风险（两个协程同时改 `self.examples`），但比构造期污染更局部。两种手法对比起来，旧式是「一个指标一个提示词实例」，新式是「一个提示词类共享，渲染时切」。

**`atomicity` / `coverage` 也进提示词输入。** 新式的 `ClaimDecompositionInput` 除了 `response` 还带 `atomicity` 与 `coverage` 两个字段（各默认 `"low"`）。它们既用于选示例，也会出现在渲染出的提示词里——因为提示词的 `instruction` 明确写着「按示例所示的原子性与覆盖度执行」。

## 关键取舍

**源码事实：** 判定是双向的，两趟各拆一侧、各判一次。`precision` 模式省掉第二趟（`fn = 0`），因此**精确率模式的成本约为召回率与 F1 的一半**。

**源码事实：** 声索拆不出来时返回空布尔数组，于是分数算到 0 而不是 NaN——与忠实度的 NaN 取向相反。

**源码事实：** 两侧都为空时得 0；只有一侧为空时，分数被推向另一个方向的极端（例如 `fn` 为 0 让召回率满分）。

**源码事实：** 最终分数 `np.round(score, 2)`，量化到百分位。

**源码事实：** 新旧两代对 `beta` 的类型校验不同（`isinstance` 接受整数 vs `type(...) is float` 拒绝整数）。

**源码事实：** 粒度切换的实现从构造期（旧式：置空 + 重建实例的 examples）迁到渲染期（新式：临时替换 + `finally` 恢复）。

**架构推论：** 把「正确性」拆成精确率与召回率两个方向是必要的：只看精确率会奖励「少说少错」，只看召回率会奖励「全都说一遍」。F-beta 是把两种失败放进一个数里的手段，`beta` 就是「更怕哪种失败」的旋钮。这也说明**单一的正确性分数是多参数约定，不是绝对真理**。

**架构推论：** 声索粒度改变分数，意味着这条指标的分可比性依赖两个额外约定（`atomicity` 与 `coverage`）。任何跨实验对比必须先确认这三个旋钮（含 `beta`）一致，否则「分数提高」可能只是把声索拆粗了。

**架构推论：** 两代实现各有取舍：旧式并发但带回调、有旧式依赖；新式串行但结构化、无回调。**「新」不等于「全面更好」**——需要回调追踪时旧式仍是唯一选择，这也是迁移不能一刀切的原因。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/factual_correctness/metric.py` | `FactualCorrectness` | 新式实现：两趟双向判定 + 三档计分 |
| `src/ragas/metrics/collections/factual_correctness/metric.py` | `FactualCorrectness.ascore` | 模式分叉、TP/FP/FN 计数、两位小数 |
| `src/ragas/metrics/collections/factual_correctness/metric.py` | `_decompose_and_verify_claims` | 拆分 + 判定，两处空值返回空数组 |
| `src/ragas/metrics/collections/factual_correctness/util.py` | `ClaimDecompositionPrompt` | 四套示例按粒度切换，渲染期临时替换 |
| `src/ragas/metrics/collections/factual_correctness/util.py` | `ClaimDecompositionInput` | 携带 `atomicity` / `coverage` 的输入模型 |
| `src/ragas/metrics/_factual_correctness.py` | `DecompositionType` | 旧式粒度枚举，四值组合 |
| `src/ragas/metrics/_factual_correctness.py` | `ClaimDecompositionPrompt` | 旧式提示词：单 `response` 输入，示例按构造期选 |
| `src/ragas/metrics/_factual_correctness.py` | `FactualCorrectness.__post_init__` | 置空示例后重建，隔离跨实例污染 |
| `src/ragas/metrics/_factual_correctness.py` | `FactualCorrectness._single_turn_ascore` | `asyncio.gather` 并发两趟判定 |
| `src/ragas/metrics/utils.py` | `fbeta_score` | 公共 F-beta，空集返回 0 |

## 参考

- [ragas 源码：collections/factual_correctness/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/factual_correctness/metric.py)
- [ragas 源码：metrics/_factual_correctness.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/_factual_correctness.py)
- [ragas 文档：事实正确性](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/factual_correctness/)
- [ragas 文档：提示词适配与少样本](https://docs.ragas.io/en/stable/concepts/metrics/custom_metrics/)

## 与主核 / AI 的关系

精确率/召回率这套集合运算与主核里「索引命中率」的数学形式一致：TP 是被正确覆盖的部分，FP 是多出来的噪声，FN 是漏掉的部分。差别是主核的集合元素是**确定的行**，这里的集合元素是**模型判定出的声索**——所以声索分解的粒度相当于「以什么为单位统计集合大小」。数据库里这对应「按什么粒度分组聚合」，粒度一变结论就变。

`np.round(score, 2)` 这个细节值得与主核的数值精度取向对照：主核必须避免任何隐式舍入（金额、计数都要精确），评测指标却主动量化到百分位。原因是评测的噪声远大于 0.01——模型判定本身就不稳定，多留几位小数只是把噪声当精度。**在噪声大于分辨率的地方，保留多余位数是自欺。**

对 agent 工程的三条实际结论：其一，`mode="precision"` 便宜一半（只一趟判定），若只关心「回答有没有编」，用它比 F1 更省。其二，声索拆不出来会被判成 0 分而不是「无数据」，所以**批处理里混入短回答会拉低均值**，应先过滤过短样本。其三，`atomicity` / `coverage` / `beta` / `mode` 四个旋钮共同定义分数，记录基线时必须全记。
