---
project: langchain
feature: retry
title: 重试与退避
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/runnables/retry.py
    symbols: [RunnableRetry, ExponentialJitterParams]
  - path: libs/core/langchain_core/runnables/base.py
    symbols: [RunnableRetry]
---

# 重试与退避

## 一句话职责

`RunnableRetry` 是 `RunnableBindingBase` 的子类，把十多年前成熟的 `tenacity` 重试器套在内部的 `Runnable` 上：由四个字段（异常类型、是否加抖动、抖动参数、最大尝试次数）决定退避曲线，`invoke` / `ainvoke` / `batch` / `abatch` 各有实现，而 `stream` / `transform` 明确不做重试。

## 机制是什么

重试是「装饰器型组合器」里最容易做错的一个。它看起来只是「失败再试」，但一旦落到一个有九种调用方式的 `Runnable` 上，立刻分裂成四个需要分别回答的问题：

1. **哪些失败该重试？** —— 用异常类型白名单，而不是「所有失败」。
2. **每一次尝试在观测上是什么？** —— 必须是上一个尝试的子 run，且标签带尝试序号。
3. **批量的重试粒度是什么？** —— 是整批重来，还是只重试失败的那几条？
4. **流式要不要重试？** —— 这里的答案是**不要**，而且写成了注释与「不实现」两个事实。

四个问题里，第 3 个是这一页最有意思的部分：`RunnableRetry._batch` 做的不是「整批重来」，而是**每一轮只重试上一轮失败的那些输入**，成功的输入立刻被写进结果表、永不再跑。

`RunnableRetry` 没有自己写退避算法，`_kwargs_retrying` 把四个字段翻译成 `tenacity` 的三个参数：

```
if self.max_attempt_number:
    kwargs["stop"] = stop_after_attempt(self.max_attempt_number)
if self.wait_exponential_jitter:
    kwargs["wait"] = wait_exponential_jitter(**(self.exponential_jitter_params or {}))
if callable(self.retry_exception_types):
    kwargs["retry"] = retry_if_exception(self.retry_exception_types)
elif self.retry_exception_types:
    kwargs["retry"] = retry_if_exception_type(self.retry_exception_types)
```

三个 `if` 各有一个容易被忽略的细节：`max_attempt_number` 为 0 或 `None` 时**不设 `stop`**（即无限重试）；`wait_exponential_jitter` 默认 `True`，所以默认就带抖动；`retry_exception_types` 允许是**可调用对象**（`(exception) -> bool`），此时用 `retry_if_exception` 而不是类型白名单。最后这条让「按状态码决定是否重试」成为可能——例如只重试 429 与 5xx。

## 怎么实现的

**四个字段的默认值是「能跑但偏保守」的一档。**

```
retry_exception_types: tuple[type[BaseException], ...] | Callable[[BaseException], bool] = (Exception,)
wait_exponential_jitter: bool = True
exponential_jitter_params: ExponentialJitterParams | None = None
max_attempt_number: int = 3
```

`(Exception,)` 意味着默认**重试一切普通异常**，包括 `ValueError`、`KeyError` 这类确定性错误——它们重试三次只会浪费三次时间。字段注释直说：

> In general you should only retry on exceptions that are likely to be transient, such as network errors. Good exceptions to retry are all server errors (5xx) and selected client errors (4xx) such as 429 Too Many Requests.

也就是说默认值是为了「不用配置也能跑」，而正确用法要求用户显式收窄。这是一个明确把「易用」压在「正确」上的默认值选择，源码用注释补偿。

`ExponentialJitterParams` 是一个 `TypedDict`（`total=False`，四个可选键）：

```
initial: float   # 初始等待
max: float       # 最大等待
exp_base: float  # 指数底数
jitter: float    # 从 random.uniform(0, jitter) 采样的附加等待
```

它只是 `tenacity.wait_exponential_jitter` 的关键字参数搬运，所以这四个键名必须与 tenacity 的签名**逐一对应**。用 `TypedDict` 而不是自定义类的好处是「零转换成本 + 类型提示」；坏处是 tenacity 改签名时这里不会有编译期错误——而 `**` 展开会静默地把不认识的键变成 `TypeError`。

`RunnableRetry` 被定义为 `RunnableBindingBase[Input, Output]` 的子类，`bound` 字段来自父类。它与 `RunnableBinding` 的区别在**方法是否被覆盖**：`RunnableBinding` 的九个方法全是转发，`RunnableRetry` 覆盖了 `invoke` / `ainvoke` / `batch` / `abatch`，其余（`stream` / `transform` / `astream` / `atransform`）仍由父类转发——于是「流式不重试」不是靠返回错误，而是靠**不覆盖**：默认的转发行为天然就是「不重试」。

**每次尝试的观测标签是这里最细的一处设计。**

```
@staticmethod
def _patch_config(config, run_manager, retry_state):
    attempt = retry_state.attempt_number
    tag = f"retry:attempt:{attempt}" if attempt > 1 else None
    return patch_config(config, callbacks=run_manager.get_child(tag))
```

**第一次尝试不加标签，只有第二次及以后才加 `retry:attempt:N`。** 理由大概是「首次尝试不该在观测面板上看起来像重试」——若一律加标签，正常路径上每条 run 都会带一个无意义的 `retry:attempt:1`，让「有重试」与「没重试」无法区分。

`_patch_config_list` 是它的批量版本，逐条 `zip` 应用。

**单条路径用 tenacity 的 `for ... with attempt:` 语法。**

```
def _invoke(self, input_, run_manager, config, **kwargs):
    for attempt in self._sync_retrying(reraise=True):
        with attempt:
            result = super().invoke(
                input_,
                self._patch_config(config, run_manager, attempt.retry_state),
                **kwargs,
            )
        if attempt.retry_state.outcome and not attempt.retry_state.outcome.failed:
            attempt.retry_state.set_result(result)
    return result
```

三处值得看：

1. **`reraise=True`** —— 重试耗尽后抛**最后一次的原始异常**，而不是 tenacity 的 `RetryError` 包装。于是调用方仍能 `except ValueError`，不必去拆 `RetryError`。这是一个很实用的默认，也正是单条路径与批量路径的处理差异所在（下面会看到批量路径会捕获 `RetryError`）。
2. **`super().invoke(...)` 而不是 `self.bound.invoke(...)`** —— 走父类 `RunnableBindingBase` 的转发，于是 kwargs 与 config 的合并逻辑（`_merge_configs` 与 `{**self.kwargs, **kwargs}`）一并生效。若直接调 `self.bound`，绑定在 `RunnableRetry` 上的 kwargs 就会丢失。
3. **`attempt.retry_state.set_result(result)`** 只在没有失败时被调用。这行是必要的：tenacity 的 `with attempt:` 块正常退出即视为成功，但**返回值本身不会被 tenacity 记录**，于是 `set_result` 显式写进去，才能让 tenacity 的行为（例如后续的 `wait` 计算、`before_sleep` 回调）看到这次尝试是成功的。

`ainvoke` 是同一段逻辑用 `async for attempt in self._async_retrying(reraise=True)` 与 `await super().ainvoke(...)`。

**批量的实现是这一页的核心，它的目标是「只重试失败的那些」。**

```
def _batch(self, inputs, run_manager, config, **kwargs):
    results_map: dict[int, Output] = {}
    not_set: list[Output] = []
    result = not_set
    try:
        for attempt in self._sync_retrying():
            with attempt:
                remaining_indices = [i for i in range(len(inputs)) if i not in results_map]
                if not remaining_indices:
                    break
                pending_inputs = [inputs[i] for i in remaining_indices]
                pending_configs = [config[i] for i in remaining_indices]
                pending_run_managers = [run_manager[i] for i in remaining_indices]
                result = super().batch(
                    pending_inputs,
                    self._patch_config_list(pending_configs, pending_run_managers, attempt.retry_state),
                    return_exceptions=True,
                    **kwargs,
                )
                first_exception = None
                for offset, r in enumerate(result):
                    if isinstance(r, Exception):
                        if not first_exception:
                            first_exception = r
                        continue
                    orig_idx = remaining_indices[offset]
                    results_map[orig_idx] = r
                if first_exception:
                    raise first_exception
            if attempt.retry_state.outcome and not attempt.retry_state.outcome.failed:
                attempt.retry_state.set_result(result)
    except RetryError as e:
        if result is not_set:
            result = cast("list[Output]", [e] * len(inputs))
```

逐段读：

**`results_map` 是「已经成功的结果，按下标」。** 每一轮开头算出 `remaining_indices`（还没成功的下标），只把这三样东西挑出来交给内层 `batch`：`pending_inputs`、`pending_configs`、`pending_run_managers`。于是多个字段必须用同一组下标切片——这是批量实现里最容易写错的地方，写错就出现「配置与输入错位」的静默错误。

**内层 `batch` 固定 `return_exceptions=True`。** 于是无论内部有几个失败，它都会返回一个与 `pending_inputs` 等长的列表，失败位置是异常对象。这是「部分成功」能被看见的前提：若传 `False`，一次失败就会抛异常，那一轮里其他成功的输入结果也丢了。

**成功项立刻按下标写回，失败项只记第一个。**

```
for offset, r in enumerate(result):
    if isinstance(r, Exception):
        if not first_exception:
            first_exception = r
        continue
    orig_idx = remaining_indices[offset]
    results_map[orig_idx] = r
if first_exception:
    raise first_exception
```

`orig_idx = remaining_indices[offset]` 这个映射是必须的：内层返回的顺序对应 `pending_inputs` 的顺序，而下标是原始输入表里的位置。

**然后只抛第一个异常，用来触发 tenacity 的重试。** 这里是这套实现的关键机制：tenacity 只能感知「块内抛了异常」，无法知道「第 3 条失败但第 1、2 条成功了」。于是实现的做法是——**把成功的结果先落袋（`results_map`），再用一个异常告诉 tenacity「这一轮没全成功」**。重试发生后，下一轮的 `remaining_indices` 会自然排除已成功项，于是「只重试失败的」这个语义就成立了。

这个技巧的代价是**成功的结果必须能在多轮之间存活**，所以 `results_map` 定义在循环外。任何把它放进 `with attempt:` 块内的重构都会把整个语义破坏掉。

**`result` 的初值是一个哨兵列表，用来分辨「一次都没跑过」与「跑过但全失败」。**

```
not_set: list[Output] = []
result = not_set
```

`not_set` 是一个**新建的空列表**，用 `result is not_set` 比较身份。于是收尾时能区分两种情况：

```
except RetryError as e:
    if result is not_set:
        result = cast("list[Output]", [e] * len(inputs))
```

- 若 `result is not_set`（第一轮就在 `super().batch` 里炸了，`remaining_indices` 之前就已抛出），用 `RetryError` 填满与输入等长的列表；
- 否则 `result` 是最后一轮的部分结果，后面按 `results_map` 补位。

**收尾的补位逻辑有一个容易踩的坑。**

```
outputs: list[Output | Exception] = []
for idx in range(len(inputs)):
    if idx in results_map:
        outputs.append(results_map[idx])
    else:
        outputs.append(result.pop(0))
return outputs
```

对每个下标：在 `results_map` 里就用成功值，否则从 `result` 头部弹一个。`result` 是**最后一轮仍失败的项**（按 `pending_inputs` 顺序排列），所以「弹头部」这个做法成立的前提是：`result` 里非异常项都已经被写进 `results_map` 了（第一段循环保证了这点），剩下在 `result` 里的只有异常与「最后一轮失败后填充的项」。

但这个前提有个边界：若 `result is not_set` 被替换成 `[e] * len(inputs)`，那么 `result` 的长度是 `len(inputs)` 而不是「剩余失败数」——此时 `pop(0)` 会为每个不在 `results_map` 里的下标弹一个 `RetryError`，恰好正确（因为这种情况下一轮成功项为 0，所有下标都不在 `results_map`）。所以两种路径都自洽，但**它依赖「`len(result)` 与「待填位数」恰好相等」这个不变量**，而这个不变量在两处不同代码里分别成立，任何一侧改动都可能破坏它。

`_abatch` 与 `_batch` 逐行对应，只是 `async for` 与 `await`。

**`batch` / `abatch` 的公开入口用父类的 `_batch_with_config` / `_abatch_with_config` 包装。**

```
def batch(self, inputs, config=None, *, return_exceptions=False, **kwargs):
    return self._batch_with_config(self._batch, inputs, config, return_exceptions=return_exceptions, **kwargs)
```

这层包装负责把「一份 config」扩成 N 份、给每个输入开一个根 run、以及 `return_exceptions` 的最终处理。于是 `_batch` 只需关心「重试」，不必重复处理回调与配置展开。

**注意批量路径的 `reraise` 是缺省的（即 `False`），与单条路径相反。**

`_batch` 用的是 `self._sync_retrying()`（没有 `reraise=True`），于是耗尽后抛的是 `RetryError`，由 `except RetryError` 分支接住并转换成「用 `RetryError` 填满的输出列表」。这与单条路径的设计意图一致：批量路径本来就以「逐项结果（含异常）」为返回形态，把 `RetryError` 作为一个结果项传出去，比抛出去更符合批量的语义。

**流式不重试这件事被写在了两个地方。**

代码末尾的注释：

```
# stream() and transform() are not retried because retrying a stream
# is not very intuitive.
```

以及**方法表里根本没有 `stream` / `transform` / `astream` / `atransform` 的实现**——它们由 `RunnableBindingBase` 转发给内层。

注释用的词是 "not very intuitive"，这是一个很诚实的措辞。真实的技术原因更硬：一次流式调用可能在已经吐出若干个块之后失败，此时「重试」意味着从头再吐一遍，调用方已经收到的块无法撤回。若下游把这些块展示了（打字机效果），重试就会出现「前几个字重复」。要在流式上正确实现重试，必须改成「把块缓冲到确认成功后再吐」——那就不是流式了。

**`base.py` 里的 `RunnableRetry` 只出现在一个局部导入位置。**

`Runnable.with_retry(...)` 的实现结尾是：

```
# Import locally to prevent circular import
from langchain_core.runnables.retry import RunnableRetry  # noqa: PLC0415

return RunnableRetry(
    bound=self,
    kwargs={},
    config={},
    retry_exception_types=retry_if_exception_type,
    wait_exponential_jitter=wait_exponential_jitter,
    max_attempt_number=stop_after_attempt,
    exponential_jitter_params=exponential_jitter_params,
)
```

`RunnableRetry` 的**定义**在 `retry.py`，`base.py` 只是局部导入它以避免循环依赖（`retry.py` 需要 `RunnableBindingBase`，而它在 `base.py` 里）。这个导入被放在函数体内部，于是模块加载顺序不冲突。

`with_retry` 的参数名与字段名不同，这是刻意的：对外用 tenacity 风格的语义化名字（`retry_if_exception_type`、`stop_after_attempt`、`wait_exponential_jitter`），对内用结构化字段名（`retry_exception_types`、`max_attempt_number`）。`kwargs={}` 与 `config={}` 显式传空，表示「不为重试额外绑定参数或配置」。

`with_retry` 的 docstring 里有一段实例化的对照，说明「方法调用与直接构造等价」，并给出**一条重要建议**：

```
# Good
chain = template | model.with_retry()

# Bad
chain = template | model
retryable_chain = chain.with_retry()
```

以及一句直白的判断：*it's best practice to keep the scope of the retry as small as possible*。理由很实在：整个链重试意味着提示构造、检索、解析全部重跑一遍，而这些步骤要么是确定性的（重跑浪费）、要么有副作用（重跑可能重复写数据）。**只把重试用在那些真正会瞬时失败的组件上**，通常是模型调用与网络请求。

## 关键取舍

**源码事实：** 四个字段的默认值是 `(Exception,)` / `wait_exponential_jitter=True` / `None` / `max_attempt_number=3`；默认重试一切普通异常，字段注释明确指出应当收窄到瞬时错误（5xx、429）。

**源码事实：** `retry_exception_types` 允许是可调用对象，此时用 `retry_if_exception`（按异常对象判定）而不是 `retry_if_exception_type`（按类型匹配）。

**源码事实：** `_kwargs_retrying` 在 `max_attempt_number` 为假值时不设 `stop`，即无限重试。

**源码事实：** `_patch_config` 只在 `attempt > 1` 时加 `retry:attempt:N` 标签，首次尝试不加。

**源码事实：** 单条路径用 `reraise=True`，重试耗尽后抛原始异常；每条尝试通过 `super().invoke(...)` 走父类转发，保住绑定的 kwargs 与 config 合并。

**源码事实：** `_batch` / `_abatch` 用 `results_map`（原下标 → 成功结果）跨轮存活，每轮只把未成功的下标子集交给内层 `batch`（固定 `return_exceptions=True`），并只抛第一个异常以触发 tenacity 重试。批量路径不带 `reraise`，于是耗尽后由 `except RetryError` 把 `RetryError` 填进结果列表。

**源码事实：** `result` 以新建空列表 `not_set` 作哨兵、用身份比较分辨「一次都没跑」；收尾按原下标补位，中间结果从 `result` 头部弹出。

**源码事实：** `stream` / `transform` / `astream` / `atransform` 未被覆盖，因而由父类转发、天然不重试；文件末尾有注释说明「重试流不直观」。

**源码事实：** `base.py` 只在 `Runnable.with_retry` 的方法体内局部导入 `RunnableRetry`（注释写明为避免循环导入），并显式传空的 `kwargs` / `config`。

**架构推论：** 批量重试的「只重试失败项」必须靠「先落袋成功结果、再用异常通知 tenacity」这一手实现，因为 tenacity 的接口只认「块内有无异常」。这类「把外部库的单点语义适配成自己的多粒度语义」的适配层，其正确性完全依赖那份跨轮存活的中间状态放在哪一层——放错一层，语义就变成「整批重试」。

**架构推论：** 「流式不重试」是这一类组合器的普遍困境。任何「可撤回性」缺失的副作用（已发送的块、已写出的字节、已展示的进度）都无法靠重试修复。判定标准应当是「这次尝试的可见输出是否已经被外部消费」——已被消费就不能重试。

**架构推论：** `with_retry` 的 `reraise=True` 让抛出的异常类型保持与内层一致，这比统一包装成 `RetryError` 好用得多：调用方写 `except ValueError` 就够了。代价是「失败了几次」这个信息只在 trace 里可见，异常本身不再携带尝试次数。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/runnables/retry.py` | `RunnableRetry` | `RunnableBindingBase` 子类，把 tenacity 重试套在内部 `Runnable` 上 |
| `libs/core/langchain_core/runnables/retry.py` | `ExponentialJitterParams` | `wait_exponential_jitter` 的四个参数搬运（initial / max / exp_base / jitter） |
| `libs/core/langchain_core/runnables/retry.py` | `_kwargs_retrying` | 把四个字段翻译成 tenacity 的 `stop` / `wait` / `retry` |
| `libs/core/langchain_core/runnables/retry.py` | `_patch_config` | 只对第 2 次及以后的尝试加 `retry:attempt:N` 子 run 标签 |
| `libs/core/langchain_core/runnables/retry.py` | `_invoke` / `_ainvoke` | 单条重试：`reraise=True`，走 `super()` 以保住绑定参数 |
| `libs/core/langchain_core/runnables/retry.py` | `_batch` / `_abatch` | 批量重试：跨轮 `results_map` + 只抛首异常，实现「只重试失败项」 |
| `libs/core/langchain_core/runnables/base.py` | `Runnable.with_retry` | 局部导入 `RunnableRetry`（避免循环依赖）并构造实例，对外用 tenacity 风格参数名 |

## 参考

- [langchain 源码：runnables/retry.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/retry.py)
- [tenacity 文档](https://tenacity.readthedocs.io/en/latest/)
- [langchain API 参考：`RunnableRetry`](https://python.langchain.com/api_reference/core/runnables/langchain_core.runnables.retry.RunnableRetry.html)
- [AWS 架构博客：指数退避与抖动](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/)

## 与主核 / AI 的关系

重试在主核里对应的是「I/O 失败后的重试」——磁盘读失败、连接断开、锁超时。这里有四条可以直接搬的经验。

**第一，批量重试的粒度是设计选择，不是细节。** 这里的答案是「只重试失败项」，实现方式是「成功结果跨轮存活 + 用一个异常通知重试器」。主核里若对一批 I/O 做重试，「整批重来」会把已成功的写入重复执行——对幂等操作只是浪费，对非幂等操作就是数据错误。

**第二，退避必须带抖动。** 默认 `wait_exponential_jitter=True` 不是可选项。没有抖动的指数退避在大量客户端同时失败时会形成同步的「重试风暴」——同一秒所有客户端一起重试，把刚恢复的服务再打挂一次。抖动把这些重试摊开到一段时间上。

**第三，只把重试包在真正会瞬时失败的那一层。** docstring 里 `template | model.with_retry()` 与 `(template | model).with_retry()` 的对比就是这条。主核里同理：不要把一个包含「读配置 + 建连接 + 执行查询 + 写日志」的复合操作整体重试，因为其中确定性的部分重跑只会放大问题。

**第四，流式不可撤回，因此不可重试。** 这条经验对主核里所有「边算边输出」的路径都成立：一旦结果的一部分已经交给下游（网络响应已发出、进度已上报），重试就意味着重复。正确做法不是重试而是**缩短不可撤回之前的窗口**，或者让下游具备去重能力。
