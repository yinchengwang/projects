---
project: ragas
feature: run-config
title: 运行配置与重试
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/run_config.py
    symbols: [RunConfig, add_retry]
  - path: src/ragas/run_config.py
    symbols: [add_async_retry]
---

# 运行配置与重试

## 一句话职责

`RunConfig` 是一个承载超时、重试次数、退避上限、并发度与异常类型的值对象；两个装饰器函数在调用时按它现场构造重试器——也就是说，重试策略不是配置在模型上的属性，而是**每次调用时按当前配置重新组装的包装**。

## 机制是什么

这里有一个反直觉的实现选择：`add_retry` / `add_async_retry` 不是「配置一次、永久生效」的装饰器，而是**每次调用模型方法时才现包一层 tenacity 重试器**。

看调用点就明白：`BaseRagasLLM.generate` 内部有 `add_async_retry(self.agenerate_text, self.run_config)`，然后立即调用；`BaseRagasEmbedding.embed_texts` 同理。所以真正的语义是：

- `RunConfig` 是**可变的值对象**，谁持有它、改哪个字段，下一次调用立刻生效；
- 重试包装是**调用期的临时产物**，用完即弃，没有任何缓存。

这个设计让「改运行配置」不需要重建模型对象，也让「多个模型共享一份配置」成为可能——但它们同时带来一个代价：无法在包装层做任何跨调用的状态累积（比如「连续失败三次就降级」这类策略无处安放）。

`RunConfig` 的六个字段覆盖了重试与并发的全部参数：`timeout`（单次操作超时，默认 180 秒）、`max_retries`（最大尝试次数，默认 10）、`max_wait`（退避上限，默认 60 秒）、`max_workers`（并发上限，默认 16）、`exception_types`（可重试的异常类型元组，默认捕获所有 `Exception`）、`log_tenacity`（是否把重试过程写进调试日志，默认关闭）。另有一个 `seed`（默认 42），在 `__post_init__` 里被用来初始化一个 `numpy` 随机数生成器并挂在 `rng` 属性上。

## 怎么实现的

**退避策略：指数 + 抖动，封顶。** 两个装饰器都用 `wait_random_exponential(multiplier=1, max=run_config.max_wait)`。这是 tenacity 的随机指数退避：等待时间随尝试次数指数增长，每次叠加随机分量避免多个并发任务同时重试形成尖峰，超过 `max_wait` 就截断在 `max_wait` 附近。

这里没有使用固定的 `wait_fixed` 或线性增长，是有理由的：评测场景往往是几十个任务并发打同一个接口，固定间隔重试会让所有任务在同一时刻再次冲撞。抖动是防止「重试风暴」的必要手段，而封顶防止单次等待长到超过整个评测的超时预算。

**终止条件：按尝试次数。** 两个都用 `stop_after_attempt(run_config.max_retries)`。注意字段名叫 `max_retries`，而语义是**最大尝试次数**（`Retrying` 的语义是「尝试 N 次」而非「重试 N 次」），所以默认值 10 意味着最多调 10 次、失败 9 次后才放弃。这个命名与语义的偏差是容易误读的地方。

**异常过滤：按类型。** 两个都用 `retry_if_exception_type(run_config.exception_types)`，默认值是 `(Exception,)`，也就是**捕获一切异常**。这是一个很宽的默认：包括那些重试毫无意义的错误（参数写错、模型名不存在、鉴权失败）。副作用是这类错误会被重试 10 次、每次间隔指数退避，一个本该立刻失败的配置问题会变成几十秒的等待加 10 条日志。

**是否抛出：`reraise=True`。** 两个都设了 `reraise=True`，所以耗尽重试后**抛出最后一次的原始异常**而不是 tenacity 自己的 `RetryError`。这对调用方友好——能直接看到真实的错误类型（比如限流错误还是解析错误），而不是一个包了一层的通用重试异常。

**日志的开关判断有一处不对称。** 两个装饰器都用 `if run_config.log_tenacity is not None:` 决定是否挂日志回调。字段类型是 `bool`、默认 `False`，而 `False is not None` 为真——**所以默认就会挂日志回调**。这与字段文档「默认关闭」的描述不符：默认情况下时间歇日志是开着的，只是因为是 DEBUG 级别所以平时看不见。要让日志真的不挂，必须显式把该字段设成 `None`，而类型声明里它是 `bool`，这属于「用 None 表示关闭」的隐式约定。

**日志对象按函数名分。** 同步版用 `ragas.retry.<函数名>`，异步版用 `TENACITYRetry[<函数名>]`。同一个被包装的方法在两个装饰器下会落到两个不同的 logger 名字上，所以按 logger 过滤时行为不一致。

**`__post_init__` 的副作用。** 唯一的初始化动作是建随机数生成器：`np.random.default_rng(seed=self.seed)`。注意这个 `rng` 是**实例属性**而不是 dataclass 字段（没有类型声明），所以它不会进 `repr`、不会参与相等比较、也不会被 `dataclasses.replace` 保留。它存在的意义是给需要可复现随机性的下游（比如测试集合成里的采样）提供统一的种子源。

**挂载方式：`set_run_config` 逐层下发。** 三个持有位置都有这个方法：`BaseRagasLLM.set_run_config` 直接赋值；`BaseRagasEmbedding.set_run_config` 同样；`MetricWithLLM.init` 在调用时若发现 llm 是旧式 `BaseRagasLLM` 就下发配置，而**新式 `InstructorBaseRagasLLM` 没有这个方法，因此不下发**——源码注释写明了这一差异。`MetricWithEmbeddings.init` 则用 `hasattr` 检查后再下发。

所以「同一份运行配置」在整条链上的到达程度不一致：旧式模型与嵌入能收到，新式模型收不到；指标层只是转发者，不持有配置。

**`timeout` 的两个去向。** 字段的用途分流了：`evaluate` 提交任务时用 `run_config.timeout` 作为 `executor.submit` 的超时参数（任务级超时）；而某些包装器会把 `request_timeout` 写到被包装的 LangChain 对象上（请求级超时）。两者语义不同但用同一个数值，于是「改 timeout」会同时改变任务超时与请求超时，无法单独调。

**`exception_types` 会被包装器改写。** 有两个包装器在构造时把 `run_config.exception_types` 改成 `RateLimitError`——也就是说，使用这些包装器时「只对限流错误重试」这一策略不是用户在配置里选的，而是包装器替用户决定的。这个改写发生在配置对象上，若同一份配置被多个包装器共享，后构造的会覆盖先构造的。这是「可变配置对象被多处持有」的直接后果。

## 关键取舍

**源码事实：** 重试器在每次调用时现构造，而不是在构造对象时配置一次。改 `RunConfig` 的字段立即生效，无需重建模型。

**源码事实：** `if run_config.log_tenacity is not None:` 的判断让默认（`False`）也走「挂日志」分支，与字段文档「默认关闭」不符；要真关闭得传 `None`，而类型声明是 `bool`。

**源码事实：** `max_retries` 的名字与语义不一致——它是**最大尝试次数**，默认 10 意味着最多调 10 次。

**源码事实：** 默认 `exception_types=(Exception,)` 捕获一切异常，配置类错误（鉴权、模型名错）也会被重试到耗尽。

**源码事实：** 部分包装器会就地改写 `run_config.exception_types` 为 `RateLimitError`。同一份配置被共享时，后构造者覆盖先构造者。

**架构推论：** 「配置对象可变 + 装饰器调用期构造」是一种廉价的动态配置方案，代价是没有跨调用的状态。若想做「连续失败降级」「按任务类型分开重试策略」，必须在调用方包一层，而配置对象本身提供了锚点。

**架构推论：** 把重试策略从模型内部搬到公共函数，使所有模型实现共享同一套退避逻辑。这与主核里把重试与超时统一放在连接池层、而不是每个驱动各自实现，是同一个组织原则。代价是全库只有一套策略，无法针对不同供应商的限流特征定制（除非改配置对象）。

**架构推论：** 新式 `InstructorBaseRagasLLM` 不下发配置，意味着新老两代模型在「运行配置是否真正生效」上有实质差异。这不是遗漏而是接口差异：新式模型的重试多半由底层 instructor 客户端自己处理，框架不再介入。迁移时要意识到「配了 max_retries 但看着没生效」可能正是这个原因。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/run_config.py` | `RunConfig` | 运行配置值对象：超时、重试、并发、异常类型、种子 |
| `src/ragas/run_config.py` | `RunConfig.__post_init__` | 按 seed 初始化随机数生成器 |
| `src/ragas/run_config.py` | `add_retry` | 同步重试包装：随机指数退避、尝试上限、原样抛出 |
| `src/ragas/run_config.py` | `add_async_retry` | 异步重试包装，参数与同步版一致，日志名不同 |
| `src/ragas/llms/base.py` | `BaseRagasLLM.generate` | 调用期现包重试器的位置 |
| `src/ragas/llms/base.py` | `BaseRagasLLM.set_run_config` | 配置下发入口 |
| `src/ragas/embeddings/base.py` | `BaseRagasEmbedding.embed_texts` | 同步与异步两条重试分支 |
| `src/ragas/metrics/base.py` | `MetricWithLLM.init` | 只对旧式模型下发配置，新式跳过 |
| `src/ragas/metrics/base.py` | `MetricWithEmbeddings.init` | 用 `hasattr` 判断后下发 |

## 参考

- [ragas 源码：run_config.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/run_config.py)
- [tenacity 文档：重试策略与退避](https://tenacity.readthedocs.io/en/latest/)
- [ragas 文档：运行配置](https://docs.ragas.io/en/stable/howtos/customizations/)

## 与主核 / AI 的关系

**「调用期构造重试器」与主核里「每次提交时读取超时参数」是同一手法。** 数据库不会在连接建立时把超时固化成连接属性，而是在每个请求上带着超时值；评测这边不把重试次数固化成模型属性，而是每次调用现读配置。两侧的理由相同：**让配置变更的热路径最短**。代价也相同：无法在配置对象以外的层做跨请求累积状态。

**随机指数退避的必要性在两个领域都成立。** 主核的锁竞争与重试、评测的重试风暴，本质都是「大量并发实体撞同一资源」。抖动的作用是把同步化的重试打散，这一点在数据库的重试循环与客户端的退避中都是标准做法。

**「默认捕获一切异常」是一条危险但常见的选择。** 主核对不可重试错误（如约束冲突）与可重试错误（如死锁）有严格分类，因为重试不可重试的错误只会浪费时间且掩盖问题。这里默认 `Exception` 全捕获，把分类责任推给了用户——而多数用户不会改这个默认值。

对 AI 工程最实用的五条：其一，**`max_retries` 是尝试次数**，设 3 就是最多调 3 次。其二，**默认会重试所有异常**，鉴权失败、模型名错这类问题会被重试到耗尽；要精确控制必须自己设 `exception_types`。其三，**包装器可能改写你的 `exception_types`**，共享同一份配置对象时尤其要小心覆盖。其四，**新式模型不下发运行配置**，别指望 `max_retries` 在它身上生效。其五，**`timeout` 同时作用于任务超时与请求超时**，想分开调只能绕开这两个入口。
