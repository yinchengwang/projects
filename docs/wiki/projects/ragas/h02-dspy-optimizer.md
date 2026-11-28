---
project: ragas
feature: dspy-optimizer
title: DSPy 编译优化
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/optimizers/base.py
    symbols: [Optimizer]
  - path: src/ragas/optimizers/dspy_optimizer.py
    symbols: [DSPyOptimizer]
---

# DSPy 编译优化

## 一句话职责

把 ragas 的提示对象翻译成 DSPy 的 Signature，交给 DSPy 的 MIPROv2 去编译，再把编译出的指令文本翻回 ragas 的格式——整条路径的价值在于**引入一个可选的第三方优化器，而不让它成为硬依赖**。

## 机制是什么

两个优化器共享同一个抽象基类 `Optimizer`，但走的路完全不同：

| | `GeneticOptimizer` | `DSPyOptimizer` |
|---|---|---|
| 优化算法 | 自研遗传算法（LLM 做算子） | DSPy MIPROv2 |
| 依赖 | 只有 ragas 自身 | 需要 `dspy-ai` |
| 提示表示 | `Dict[str, str]` | 转成 DSPy `Signature` 类 |
| 适应度 | 自算 `Loss` | 转成 DSPy 的 `metric(example, prediction)` |
| 搜索空间 | 指令 + 用反馈改写 | 指令 + 演示（few-shot）联合搜索 |

`Optimizer` 本身极薄：两个可选字段 `metric`、`llm`，加一个抽象方法 `optimize(dataset, loss, config, ...) -> Dict[str, str]`。它把「优化什么」和「产出什么」钉死，把「怎么搜」完全交给子类。

## 怎么实现的

**可选依赖的处理分三层，缺一层就会在错误的地方失败。**

第一层在包导出：`optimizers/__init__.py` 用 `try: from ... import DSPyOptimizer ... except ImportError:` 决定 `__all__` 是否包含这个名字。所以 `from ragas.optimizers import DSPyOptimizer` 在没装 dspy 的机器上会直接 `ImportError`,而 `GeneticOptimizer` 照常可用。

第二层在构造：`DSPyOptimizer.__post_init__` 里 `import dspy`，成功就把模块对象存进 `self._dspy`（一个 `init=False` 的字段），失败则抛一个**带安装指引的** `ImportError`——错误消息里直接写了 `uv add 'ragas[dspy]'` 与 `pip install 'ragas[dspy]'` 两种写法。注意 `__init__.py` 的 try/except 只是让导入不炸，真正构造时仍然会抛——**这个异常的文案质量决定了用户能否自己走出来**。

第三层在适配器：`dspy_adapter.py` 里每个函数（`pydantic_prompt_to_dspy_signature`、`ragas_dataset_to_dspy_examples`、`create_dspy_metric`）都自己再写一遍 `try: import dspy except ImportError: raise ImportError(...)`。所以即使有人绕过前两层直接 import 适配器，拿到的也是同一句指引，而不是 `ModuleNotFoundError`。

另有一处间接依赖：`setup_dspy_llm` 内部再 `from ragas.optimizers.dspy_llm_wrapper import RagasDSPyLM`。这个 wrapper 类的模块本身不 import dspy，只在 `__call__` 里 `import asyncio`——它靠鸭子类型对接 DSPy，所以那一层不会额外失败。

**`optimize` 的六步流水线。** docstring 把步骤写出来了，实现严格对应：

1. `setup_dspy_llm(self._dspy, self.llm)` → 造一个 `RagasDSPyLM` 并 `dspy.settings.configure(lm=lm)`。这是**全局状态**：配置一次之后所有 DSPy 调用都走这个 LM。
2. `self.metric.get_prompts()` 拿提示字典；对每个 `prompt_name` 逐个优化。
3. `pydantic_prompt_to_dspy_signature(prompt)` → 遍历 `input_model.model_fields` 建 `dspy.InputField`、`output_model.model_fields` 建 `dspy.OutputField`，再用 `type(...)` 动态造一个继承 `dspy.Signature` 的类，`__doc__` 塞 `prompt.instruction`。**提示的 instruction 就这样变成了 Signature 的文档字符串**——DSPy 的优化器改的正是这个位置。
4. `ragas_dataset_to_dspy_examples(dataset, prompt_name)` → 只取 `is_accepted` 的样本，把 `prompt_input` 的键值摊进 example dict、把 `edited_output or prompt_output` 也摊进去，最后 `with_inputs(*input_keys)` 标记哪些是输入。非 dict 的输出会被塞进 `output` 一个键。
5. `create_dspy_metric(loss, dataset.name)` → 包成 `dspy_metric(example, prediction)`：从两者按指标名取属性，任一缺失返回 0.0，否则返回 `-float(loss([predicted], [ground_truth]))`。**取负号是因为 DSPy 要求「越大越好」而 `Loss` 是「越小越好」**——这是整条适配链里唯一一处方向修正。
6. `MIPROv2(...).compile(module, trainset=examples, metric=metric_fn)` → 然后 `_extract_instruction` 从编译产物里挖出指令。

**参数是逐字段透传的，一个没漏也没加。** `num_candidates`、`max_bootstrapped_demos`、`max_labeled_demos`、`init_temperature`、`auto`、`num_threads`、`max_errors`、`seed`、`verbose`、`track_stats`、`log_dir`、`metric_threshold` 全部按名字传给 `MIPROv2`。这些都在 `__post_init__` 的 `_validate_parameters` 里做了范围校验：`num_candidates > 0`、两个 demo 数 ≥ 0、`init_temperature > 0`、`auto ∈ {light, medium, heavy, None}`、`num_threads > 0`（若给）、`max_errors ≥ 0`（若给）、`metric_threshold ∈ [0, 1]`（若给）。

**缓存键是「参数快照 + 数据集哈希」的二次哈希。** `_generate_cache_key` 把 `metric.name`、`dataset.model_dump()` 的 SHA256、`loss.__class__.__name__`、全部可配参数、以及外部 `config` 一起 `json.dumps(sort_keys=True, default=str)` 后再 SHA256。它用 `default=str` 兜住不可序列化的对象，`sort_keys=True` 保证同参同键。缓存命中时直接返回先前的结果字典，并把这次运行标记为「Cache hit」。

注意缓存键里装的是 **`loss` 的类名**而不是实例状态——`MSELoss` 的 `reduction` 字段、`BinaryMetricLoss` 的 `metric` 字段都不进键。改了这些字段但类名不变，会拿到旧缓存。

**`_extract_instruction` 是三段式降级取文本。** 优先 `optimized_module.signature.instructions`；次之 `signature.__doc__`；再之 `extended_signature` 转字符串；全都不存在则返回空串。**兜底返回 `""` 而不是抛错**：意味着一次编译若结构不匹配，会把空指令写进优化结果——调用方拿到的是一个「指令被清空」的提示，而不是一个明确的失败。

**`RagasDSPyLM` 是一个最小适配壳。** 它只实现 DSPy 需要的调用形态：`__call__(prompt=None, messages=None, **kwargs)` 把 prompt 包成 `[{"role": "user", "content": prompt}]`，然后 `asyncio.run(self._generate(...))`，返回 `[result]`（DSPy 期望列表）。内部 `_generate` 把 messages 塞进 `PromptValue(prompt_str="", messages=messages)` 交给 ragas 的 `generate`，再按 `generations[0][0].text` 取文本。

这里有两处需要注意：`asyncio.run` 在已有事件循环的上下文里会直接抛 `RuntimeError`（与 i04 里 `apply_nest_asyncio` 要解决的问题同源，但这里没有做任何保护）；而 `from ragas.llms.prompt import PromptValue` 这个导入路径在当前锚点里**并不存在**——`PromptValue` 实际来自 `langchain_core.prompt_values`（在 `ragas/llms/base.py` 里被 import），且 `src/ragas/llms/` 下没有 `prompt.py`。也就是说这个 wrapper 的生成路径在运行时会先在 import 那一行失败。`history` 字段只有 `inspect_history` 读它，但没有任何地方往里写，恒为空列表。

## 关键取舍

**源码事实：** dspy 是可选依赖，处理分三层：`optimizers/__init__.py` 的 try/except 改 `__all__`、`DSPyOptimizer.__post_init__` 的 import 带安装指引、`dspy_adapter.py` 每个函数各自再 try/import 一次。

**源码事实：** `create_dspy_metric` 返回 `-float(loss_value)`。这是把 ragas 的「越小越好」翻成 DSPy 的「越大越好」的唯一一处修正。

**源码事实：** `_generate_cache_key` 用 `loss.__class__.__name__`，不包含损失实例的字段值；`json.dumps` 用 `default=str` 兜不可序列化对象。

**源码事实：** `_extract_instruction` 找不到任何文本时返回空字符串而非抛错。

**源码事实：** `RagasDSPyLM.__call__` 用 `asyncio.run(...)`，在已有事件循环里会抛 `RuntimeError`；其 `_generate` 从 `ragas.llms.prompt` 导入 `PromptValue`，而该模块在此锚点不存在（`PromptValue` 实际来自 `langchain_core.prompt_values`）。

**源码事实：** `optimize` 里每个提示名独立编译一次，每次新建一个 `Predict` 模块与一个 `MIPROv2` 实例；所有提示共用一个由 `dspy.settings.configure` 设置的全局 LM。

**架构推论：** 这条适配链的本质是「把两套提示抽象对齐」：ragas 的提示 = instruction + 输入/输出 pydantic 模型；DSPy 的 Signature = docstring（指令）+ InputField/OutputField。对齐后，DSPy 的优化器就能在**它自己的搜索空间**（指令 + 演示）里工作，而 ragas 只需要把结果文本搬回来。代价是这套映射是有损的：ragas 提示里的 few-shot `examples`、`name` 等字段都不进 Signature（`name` 只在类名上留痕），所以 DSPy 优化得到的指令被写回 ragas 提示时，其余部分仍是原样。

**架构推论：** 「可选依赖」在 Python 生态里的正确做法就是这三层：**导入不炸 → 构造时报错并给安装指引 → 适配层自证依赖**。缺第一层会让 `import ragas` 失败，缺第二层会让用户拿到裸 `ModuleNotFoundError`，缺第三层会让直接 import 适配器的人拿到无法理解的消息。这套三层结构值得直接照抄。

**架构推论：** 把 `dspy.settings.configure` 当配置入口意味着**同一进程里只有一个 DSPy LM**。这对单次优化没问题，但两个并发优化会互相覆盖。同理，`_extract_instruction` 返回空串的兜底是「宁可产出空指令也不中断整轮优化」——这是把可用性放在正确性之前的取舍。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/optimizers/base.py` | `Optimizer` | 抽象基类：`metric`、`llm`、抽象 `optimize` |
| `src/ragas/optimizers/__init__.py` | try/except 导入 | 按 dspy 是否可用决定 `__all__` |
| `src/ragas/optimizers/dspy_optimizer.py` | `DSPyOptimizer` | 优化器主体：参数校验、缓存、MIPROv2 调用 |
| `src/ragas/optimizers/dspy_optimizer.py` | `DSPyOptimizer.__post_init__` | 延迟导入 dspy 并给出安装指引 |
| `src/ragas/optimizers/dspy_optimizer.py` | `DSPyOptimizer._validate_parameters` | 全部可配参数的范围校验 |
| `src/ragas/optimizers/dspy_optimizer.py` | `DSPyOptimizer.optimize` | 六步流水线与逐提示名编译 |
| `src/ragas/optimizers/dspy_optimizer.py` | `DSPyOptimizer._extract_instruction` | 三段式降级取指令，兜底空串 |
| `src/ragas/optimizers/dspy_optimizer.py` | `DSPyOptimizer._generate_cache_key` | 参数快照 + 数据集哈希的缓存键 |
| `src/ragas/optimizers/dspy_adapter.py` | `setup_dspy_llm` | 把 ragas LLM 配成全局 DSPy LM |
| `src/ragas/optimizers/dspy_adapter.py` | `pydantic_prompt_to_dspy_signature` | pydantic 提示 → DSPy Signature 类 |
| `src/ragas/optimizers/dspy_adapter.py` | `ragas_dataset_to_dspy_examples` | 标注 → DSPy Example，含 `with_inputs` 标记 |
| `src/ragas/optimizers/dspy_adapter.py` | `create_dspy_metric` | 损失 → DSPy 指标，方向取负 |
| `src/ragas/optimizers/dspy_llm_wrapper.py` | `RagasDSPyLM` | 最小 DSPy LM 适配壳 |
| `src/ragas/optimizers/dspy_llm_wrapper.py` | `RagasDSPyLM.__call__` | 包消息、`asyncio.run`、返回单元素列表 |
| `src/ragas/cache.py` | `CacheInterface` | 优化结果的缓存后端契约 |
| `src/ragas/losses.py` | `Loss` | 被翻转成 DSPy 指标的原函数 |

## 参考

- [ragas 源码：optimizers/dspy_optimizer.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/optimizers/dspy_optimizer.py)
- [DSPy 源码仓库](https://github.com/stanfordnlp/dspy)
- [ragas 源码仓库](https://github.com/explodinggradients/ragas)
- [ragas 源码：optimizers/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/optimizers/base.py)

## 与主核 / AI 的关系

这个优化器与主核里「可选插件式存储引擎/压缩算法」的架构问题是同一个：**主路径不能因为某个可选组件缺席而失效**，而可选组件缺席时的失败必须发生在「用户明确要用它」的那一刻、并附带修复指引。这里的做法值得对照：主核用编译期宏或运行期动态库探测，Python 用 try/import 三层。共同要求是「错误消息里必须写清怎么补」。

对工程有三条可直接用的结论。第一，**可选依赖的错误消息就是产品**：`uv add 'ragas[dspy]'` 与 `pip install 'ragas[dspy]'` 两种写法都给出，用户不用查文档；反之若只抛裸 `ModuleNotFoundError`，这条路径在实践里就等于不可用。第二，**方向修正必须显式且集中**：`-loss` 这一处负号是两套框架语义的唯一接缝，散落多处就会有一处忘了翻。第三，**降级兜底要能被观测**：`_extract_instruction` 返回空串的兜底让流程不中断，但若不打日志，用户会得到「优化后提示突然变空」这个谜。

另有一条关于适配器的诚实观察：`RagasDSPyLM` 里那句 `from ragas.llms.prompt import PromptValue` 指向不存在的模块，而 `asyncio.run` 在已有事件循环里会直接失败。这说明**适配层是最容易腐化的一层**——它跨两个框架、测试覆盖通常最薄。任何依赖第三方优化器的集成，都该有一条「端到端跑通一次」的冒烟测试，否则这类问题只会在用户机器上出现。
