---
project: ragas
feature: dynamic-few-shot
title: 动态少样本
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/prompt/dynamic_few_shot.py
    symbols: [DynamicFewShotPrompt, SimpleExampleStore]
  - path: src/ragas/prompt/dynamic_few_shot.py
    symbols: [SimpleInMemoryExampleStore]
---

# 动态少样本

## 一句话职责

`DynamicFewShotPrompt` 把「示例」从提示对象里的一个固定列表，换成一个按当前输入相似度取 top-k 的检索步骤——于是每次调用看到的 few-shot 都可能是不同的一组。

## 机制是什么

静态 few-shot 的问题是明确的：示例固定，那么对「与示例相似」的输入效果极好，对长尾输入毫无帮助，甚至可能把模型带偏——它会照着不相关的示例样例去编。动态 few-shot 的解法是**在每次调用时，从示例库中挑出与当前输入最相近的若干条**。

结构上分两层：

- `SimpleExampleStore` 是抽象接口，两个方法：`get_examples(data, top_k)` 与 `add_example(input_dict, output_dict)`。注意它交换的数据是**普通的 dict**，不是 pydantic 模型。
- `SimpleInMemoryExampleStore` 是内存实现，持有 `_examples`（dict 对）与 `_embeddings_list`（每条输入对应的向量）。
- `DynamicFewShotPrompt` 组合一个 store，并在 `format(**kwargs)` 时把检索结果拼进提示。

`DynamicFewShotPrompt` 继承的是 `Prompt`（`simple_prompt.py` 里那个轻量的、非 pydantic 的提示类），**不是** `PydanticPrompt`。这是本页最容易被忽略的一点：这条动态少样本路线走的是「instruction 字符串 + dict 示例」的老形态，产出的是一段纯文本，不产出结构化模型。

同一目录下还有另一条并行的实现（`few_shot_pydantic_prompt.py` 里的 `FewShotPydanticPrompt`），它继承 `PydanticPrompt`、示例是 `(InputModel, OutputModel)` 模型对、用嵌入检索后覆写 `generate_multiple`。两条路线共享「按相似度挑示例」这个思路，但接入点不同：一条在 `format` 层，一条在 `generate_multiple` 层。

## 怎么实现的

**Embedding 是可选的，缺失时退化为「取最近的」。** `SimpleInMemoryExampleStore._get_embedding` 在 `embedding_model is None` 时返回空列表；`get_examples` 里先判 `if not self.embedding_model or not self._embeddings_list: return self._examples[-top_k:]`——没有嵌入就返回**最近加入的 k 条**。

这条降级路径有实际价值：它让「动态少样本」在无嵌入模型时仍能工作（退化成「最新示例优先」），也使单元测试不必接模型。但它同时意味着**行为会随配置静默改变**：同一份代码，有嵌入与没嵌入走的是完全不同的选择逻辑，而调用方拿不到任何提示。

**嵌入的文本是「把 dict 拆成 key: value 行」。** `_get_embedding` 用 `"\n".join([f"{k}: {v}" for k, v in data.items()])` 序列化。所以进入向量的不只是值，还有键名——这一点对相似度判断有实质影响：两个键名相同、值不同的输入会被判为比键名不同但值相同的更相似。

**相似度用余弦，阈值与 top-k 双重过滤。** `_get_nearest_examples` 先算 `dot(embed_matrix, query) / (norm(embed_matrix) * norm(query) + 1e-8)`——分母加 `1e-8` 是防零向量除零。然后 `np.where(similarities >= threshold)[0]` 取超阈值的，再按相似度排序取最后 top_k 个（`np.argsort(...)[-top_k:]`）。

注意顺序细节：它**先按阈值过滤再取 top-k**，所以阈值是硬门槛而非软加权。若一条示例相似度 0.69（阈值 0.7），即使库里只有它一条，也不会被选中——然后走后面那条兜底分支。

**兜底分支也是「取最近的」。** 若没有任何示例过阈值，`_get_nearest_examples` 返回 `list(range(max(0, len(embeddings) - top_k), len(embeddings)))`——即最后 top_k 条的索引。这与「无嵌入模型」分支的兜底策略一致：**宁可给不相关的示例，也不给空**。

这个选择值得单独记住：它意味着「示例总会出现在提示里」，模型永远不会看到零示例的状态。好处是提示形状稳定；坏处是输入与库内容完全无关时，模型仍会被塞进几条毫不相干的示例，可能比没有更糟。

**拼接发生在 `format` 里，格式极简。** 每条形如 `Example i:\nInput:\nkey: value\nOutput:\nkey: value`，全部用 `\n\n` 与 instruction 段连接。这里没有 JSON、没有 schema 说明——因为整条链是纯文本，不存在结构化解析。

**`add_example` 有去重。** `DynamicFewShotPrompt.add_example` 先判 `if (input, output) not in self.example_store._examples` 再添加。它直接读了 store 的私有字段 `_examples`，没有走接口方法——`SimpleExampleStore` 没有「是否已存在」的方法。

**构造顺序是刻意安排的。** `__init__` 里先建 `example_store` 与两个配置字段，再用 `super().__init__(instruction, [], response_model)` 传**空列表**，最后手工把 examples 喂进 store。源码注释写明原因：避免父类构造期间调用 `self.add_example` 而那时 store 还不存在。这是「父类在构造期会调用可覆写方法」这一经典陷阱的规避。

**`save` / `load` 承认了不可序列化的部分。** 两个模型（`response_model` 与 `embedding_model`）都无法序列化，所以 `save` 对它们各发一条 `warnings.warn`，并在 JSON 里只写 `class_name` / `module` / `schema` 这类元信息。`load` 时若文件里记了 `response_model_info` 而调用方没传 `response_model`，直接抛 `ValueError` 并给出正确的调用示例；`embedding_model` 缺失则只告警——因为它**可以缺**（降级为最近 k 条）。

`save` 还支持 `.gz` 后缀（`gzip.open`），且 `include_embeddings=True` 时把向量列表一并写盘。`load` 恢复向量时有一个三重条件：文件里有 `embeddings`、调用方给了 `embedding_model`、且**向量条数与示例条数相等**——最后一条是防止旧文件与新示例列表错位。

**`from_prompt` 是格式迁移入口。** 它从一个 `Prompt` 复制 instruction / examples / response_model，构造出动态版。于是「已有静态 few-shot 提示想升级为动态」只需多传一个嵌入模型。

## 关键取舍

**源码事实：** 无嵌入模型、或所有相似度都低于阈值时，都退化为「取最后 top-k 条」（最近加入的示例）。示例永远不会为空。

**源码事实：** 相似度先按阈值过滤再取 top-k，阈值是硬门槛；分母加 `1e-8` 防零向量。

**源码事实：** 嵌入的输入文本是 dict 拆成的 `key: value` 行，键名参与相似度计算。

**源码事实：** `__init__` 先建 store 再调父类构造，传空 examples，然后手工喂入，以规避父类构造期调用覆写方法而属性未就绪。

**源码事实：** store 接口交换的是普通 dict，`DynamicFewShotPrompt` 继承的是 `Prompt` 而非 `PydanticPrompt`，所以这条路线不产出结构化模型。

**架构推论：** 「示例永不缺失」这条兜底把稳定性置于相关性之上。对一个评测库而言这个方向可以接受（提示形状稳定、行为可预测），但如果用在生产问答上，塞入无关示例会系统性损害长尾输入的质量。要改就需要允许「零示例」状态，而那会让提示形状随输入变化——两者无法兼得。

**架构推论：** 两条动态少样本实现（`DynamicFewShotPrompt` 的 `format` 层注入，`FewShotPydanticPrompt` 的 `generate_multiple` 层覆写）说明「在哪个环节注入」比「怎么检索」更决定适用面：`format` 层只能配纯文本提示，`generate_multiple` 层能保留完整的结构化解析与重试。代价是后者必须先有 `(模型对)` 形式的示例库，而前者用 dict 就够了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/prompt/dynamic_few_shot.py` | `SimpleExampleStore` | 示例库接口：按输入取 top-k、追加一条 |
| `src/ragas/prompt/dynamic_few_shot.py` | `SimpleInMemoryExampleStore` | 内存实现：缓存示例与向量，无嵌入时退化 |
| `src/ragas/prompt/dynamic_few_shot.py` | `SimpleInMemoryExampleStore.get_examples` | 两条降级路径与阈值过滤 |
| `src/ragas/prompt/dynamic_few_shot.py` | `SimpleInMemoryExampleStore._get_nearest_examples` | 余弦相似度计算、阈值过滤、top-k 与兜底 |
| `src/ragas/prompt/dynamic_few_shot.py` | `DynamicFewShotPrompt` | 组合 store，在 format 时注入检索到的示例 |
| `src/ragas/prompt/dynamic_few_shot.py` | `DynamicFewShotPrompt.from_prompt` | 从静态 Prompt 迁移为动态版的入口 |
| `src/ragas/prompt/dynamic_few_shot.py` | `DynamicFewShotPrompt.save` | 存示例与向量，模型只存元信息 |
| `src/ragas/prompt/simple_prompt.py` | `Prompt` | 被组合的基类：instruction + dict 示例 |
| `src/ragas/prompt/few_shot_pydantic_prompt.py` | `FewShotPydanticPrompt` | 并行实现：在 generate_multiple 层注入 |

## 参考

- [ragas 源码：prompt/dynamic_few_shot.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/prompt/dynamic_few_shot.py)
- [ragas 源码：prompt/few_shot_pydantic_prompt.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/prompt/few_shot_pydantic_prompt.py)
- [ragas 文档：提示与评测概念](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/)
- [论文：用检索增强的少样本提示做文本分类](https://arxiv.org/abs/2303.03846)

## 与主核 / AI 的关系

按相似度挑示例，与主核里「按统计信息选择访问路径」是同一种决策：都是**用一小段归纳出来的元数据，在每次执行前选一个具体方案**。主核选索引，这里选示例。两者的共同风险也一致——元数据不准时，选择会静默劣化，而且劣化方向是「挑了个不合适的」，不是「报错」。

对 agent 工程最实用的三条。其一：动态 few-shot 的收益**只在长尾输入上体现**，如果你的输入分布本来就很窄（同一类问题反复出现），固定示例更省更稳，别为了「动态」多接一个嵌入模型。其二：注意那条「永不返回空示例」的兜底——输入与示例库完全无关时，模型仍会被塞进几条不相干的示范，这比不给示例更容易让它编造格式。其三：两条实现路线的分野在于注入层，选 `FewShotPydanticPrompt` 才能同时保留结构化输出与解析重试；`DynamicFewShotPrompt` 产出的只是纯文本，适合「不需要结构化的自由生成」场景。
