---
project: vllm
feature: executor
title: 执行器抽象
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/executor/abstract.py
    symbols: [Executor]
---

# 执行器抽象

## 一句话职责

`Executor` 把「在多少张卡上跑模型」这件事收成一个接口：上层只调 `execute_model` / `sample_tokens`，它负责把调用翻译成「对所有 worker 的一次 RPC」并取回结果。

## 机制是什么

一个引擎核心里有个字段叫 `model_executor`，它的类型是 `Executor`。上层看到的是「跑一步模型」这一个动作，而这个动作在单卡、多进程、Ray 集群上完全不是一回事：

| 形态 | 实现类 | 进程结构 |
|---|---|---|
| `uni` | `UniProcExecutor` | 进程内直接调 worker |
| `mp` | `MultiprocExecutor` | 每 rank 一个子进程 |
| `ray` | `RayExecutorV2` | Ray actor 集群 |
| `external_launcher` | `ExecutorWithExternalLauncher` | 由外部启动器管理进程 |
| 任意类名 | `resolve_obj_by_qualname(...)` | 插件 |

**全部差异被压缩成一个方法：`collective_rpc`。** 它是唯一的抽象方法（除了 `_init_executor` 与 `check_health`），签名是 `(method, timeout, args, kwargs, non_block)`。所有具体能力都是它的**薄封装**：

```python
def execute_model(self, scheduler_output, non_block=False):
    output = self.collective_rpc("execute_model", args=(scheduler_output,), non_block=non_block)
    return output[0]
```

注意 `output[0]`——**`collective_rpc` 永远返回「每个 worker 一份结果」的列表，而 `Executor` 只把第 0 份交出去**。在多卡上这不是省事，而是准确：同一个 `SchedulerOutput` 在所有 rank 上跑出来的 `ModelRunnerOutput` 里，只有输出 rank 的那份是完整的（其余 rank 只有部分张量或不产 token）。

这套设计的关键前提是「**控制面走 RPC，数据面不走**」。docstring 里有一条 Note 把它写明了：「It is recommended to use this API to only pass control messages, and set up data-plane communication to pass data.」——张量通过 `parallel_state` 里的通信组走 NCCL，`collective_rpc` 只传控制参数与小型结果。

## 怎么实现的

**`get_class` 是一条纯字符串分派，但它接受三种形态。** 传类就用类（校验是 `Executor` 子类）、传内置字符串走四条已知分支、传其它字符串走 `resolve_obj_by_qualname` 当插件。**「未知 backend」只在既不是类也不是合法字符串时才抛**，所以拼错模块名会在 import 时报错而不是提前拦截。

**`collective_rpc` 用 `@overload` 把 `non_block` 的类型固化到返回值上。** 三个重载：`non_block=False` → `list[_R]`；`non_block=True` → `Future[list[_R]]`；实现签名是联合。这是**用类型系统表达「同步/异步同接口」**的常见手法——调用方传字面量 `True`/`False` 时能拿到精确类型，传普通 `bool` 时退化成联合类型。

同一个模式也用在 `execute_model` 与 `sample_tokens` 上。所以「异步执行」不是另一套 API，而是同一个方法的返回类型变化。这点与引擎核心那页的 `non_block=True` 正好呼应。

**大量方法是「RPC 的命名包装」，但有些不是无脑转发。**

- `compile_or_warm_up_model`：收到各 worker 的编译耗时后，**取 `max`** 写回主进程配置，注释解释原因：「With TP>1, compilation happens in worker processes, so the main process config is never updated. Use max across workers since they compile in parallel.」——并行编译的总耗时是最大值而不是求和。
- `extensible_kv_cache_unsupported_reason`：先在自己这一侧做一次早退——若传入的 `layer_configs` 全为空就返回「the model has no KV cache」；否则调 worker 拿各 rank 的原因，**取第一个非空的**返回。这是「或者」语义：任一 rank 不支持，整体就不可用。注意它把「模型根本没有 KV cache」这个本应已知的判断放在本地，避免了为一件已知的事去发一次 RPC。
- `supported_tasks`：用 `@cached_property` 缓存。注释写「Avoid unnecessary RPC calls」——这个值在启动后不会变，而它会被频繁查询。
- `supports_draft_weight_updates` / `add_lora` / `remove_lora` / `pin_lora`：用 `all(...)` 把「各 rank 的布尔」压成一个。**「所有 rank 都成功」才是成功**。三个 LoRA 方法还都先断言 `lora_id > 0`——把「非法 id」这类调用方错误挡在 RPC 之前。
- `list_loras`：不仅取第 0 份，还断言**所有 worker 返回的集合相同**，注释写「All workers should have the same LORAs.」。这是把「跨 rank 一致性」变成一条运行期校验。
- `probe_forward`：与 `execute_dummy_batch` 不同，它会遍历所有 rank 的返回并逐个检查错误。注释解释了差别：「Unlike execute_dummy_batch, wait for every rank: PP stages other than the output rank fail independently.」——**输出 rank 成功不代表其它 PP stage 成功**，因为它们的失败是独立的。两个方法的返回值处理正好相反：`execute_dummy_batch` 取 `[0]`，`probe_forward` 遍历全部。

**`sleep` / `wake_up` / `discard` 用一个 `sleeping_tags` 集合维护状态机。** 标签常量 `SLEEP_TAGS = frozenset(("weights", "kv_cache"))`。细节值得逐条看：

- `sleep` 前判的是 `if "weights" in self.sleeping_tags`——**只查 `weights` 这一个标签**就认定「已在睡」，因为权重是最终要释放的东西；是则 warning 并返回（**幂等**）。
- `sleep` 成功后 `self.sleeping_tags |= SLEEP_TAGS`——**无条件把两个标签都置上**，不管 `level` 传了什么。所以「部分睡眠」在本地状态记录上表达不出来，`level` 只影响 worker 侧的真实行为。
- `wake_up` 要求「必须先睡」；若传了具体 `tags`，逐个校验都在 `sleeping_tags` 里，**有一个不在就整体返回**（不是部分唤醒）。成功后才按 `tags` 逐个 `remove`，或整体 `clear`。
- `discard` 先算 `tags_to_discard = set(tags) - self.sleeping_tags`，**已经睡着的标签不重复丢弃**；若差集为空则 warning 并返回。真正丢弃的 RPC 用 `try/finally` 包住，`finally` 里把差集并进 `sleeping_tags`。也就是**即使 RPC 抛错，本地状态也照样记成「已丢弃」**。这个反直觉的选择有理由：`discard` 是「把这块显存扔掉」，如果 worker 侧已经执行了一半，本地记成「还在用」比记成「已丢」更危险。

**`profile` 有一处「先校验再分发」的保护。** 若传了按次覆盖的参数（`delay_iterations` / `max_iterations` / `profiler_kwargs`），先调 `_check_worker_profile_kwargs` 用 `inspect.signature` 检查 worker 的 `profile` 是否接受这些参数。注释给出了根因：「Reject before dispatch: a failing collective_rpc may leave unread worker replies queued.」——**一个失败的集合 RPC 会在管道里留下未读的回复**，后续调用会读错。所以能提前判的错必须提前判。

`_check_worker_profile_kwargs` 自己也有个细节：若 worker 的 `profile` 有 `**kwargs`（`VAR_KEYWORD`），直接放行——因为任何参数都能接。

**`disable_extensible_kv_cache` 在关掉之前先挡一个前提。** 它检查 `kv_transfer_config.kv_cache_access()` 是否包含 `KVCacheAccess.EXPORTS_FD`，是则抛 `RuntimeError` 并点明「某个 connector（例：LMCacheMPConnector 用 vmm API）通过 POSIX 文件描述符导入 KV cache，需要可扩展 KV cache」。也就是说**这个能力关不掉**，因为已经有人依赖它的一个特定性质。这是「配置项被下游语义绑死」的典型形态。

**`initialize_from_config` / `extend_kv_cache` / `set_kv_cache_layout` / `set_mamba_block_size` 是一组「把决议下发给 worker」的方法。** 它们的共同形态是「主进程算出一个值，然后广播给所有 worker」：

- `determine_available_memory` 反向——从 worker 收集；
- `get_kv_cache_layer_configs` / `get_supported_kv_cache_layouts` 也是收集；
- `set_kv_cache_layout` / `set_mamba_block_size` 是下发。

所以 `Executor` 实际上同时是**收集器**与**下发器**，方向由方法名区分（`get_` / `determine_` vs `set_` / `initialize_` / `extend_`）。

**`shutdown` 与 `register_failure_callback` 的关系值得注意。** `register_failure_callback` 的基类实现是**空的**（带 `# noqa: B027`），docstring 说「注册一个在执行器进入永久失败状态时被调用的函数」。也就是说基类不实现失败通知，只有部分子类会做。这是刻意的：单进程执行器没有「永久失败」这个概念，它的失败就是异常。

**文件末尾有一组为了向后兼容的再导出。**

```python
from vllm.v1.executor.uniproc_executor import ExecutorWithExternalLauncher as _ExecutorWithExternalLauncher
UniProcExecutor = _UniProcExecutor
ExecutorWithExternalLauncher = _ExecutorWithExternalLauncher
```

注释写「For backwards compatibility.」。而 `get_class` 里 import 这两个类用的是**延迟导入**（在函数体内 `from ... import`），原因写在内存里：这会造成循环导入——`uniproc_executor` 需要 `Executor` 基类，而基类文件又要引用它。**尾部导入 + 函数内导入**是绕开这个环的两种手段，同时用上了。

**`supported_tasks` 用 `cached_property` 有一个隐含要求：worker 的 `get_supported_tasks` 必须在整个生命周期里稳定。** 若某些 worker 因为加载 LoRA 而改变了支持的任务集，这个缓存就会说谎。当前实现里没有失效机制。

## 关键取舍

**源码事实：** `collective_rpc` 是唯一的通用抽象方法，其余能力都是它的命名封装；统一取 `output[0]`，因为只有输出 rank 的结果是完整的。

**源码事实：** docstring 明确建议「只用这个 API 传控制消息，数据面另设通道」。

**源码事实：** `non_block` 用三个 `@overload` 把返回类型在同步列表与 `Future` 之间切换。

**源码事实：** `compile_or_warm_up_model` 取各 worker 编译耗时的 `max` 写回配置；`extensible_kv_cache_unsupported_reason` 先在本地判断「没有 KV cache」，否则取第一个非空原因；`add_lora` / `remove_lora` / `pin_lora` 先断言 `lora_id > 0` 再用 `all(...)` 汇总；`list_loras` 断言所有 worker 集合相同；`probe_forward` 遍历全部 rank 的错误而 `execute_dummy_batch` 只取第 0 份。

**源码事实：** `sleep` 只以 `"weights"` 是否在集合里判断「已在睡」，成功后无条件置上全部 `SLEEP_TAGS`（`level` 只影响 worker 侧）；`wake_up` 若指定 tag 有一个不在集合里就整体不做，成功后才按 tags 移除或整体清空；`discard` 只处理「尚未睡着的标签」（差集），并在 `finally` 里更新本地状态（RPC 失败也记为已丢弃）。

**源码事实：** `profile` 在 dispatch 前校验 worker 是否支持按次覆盖参数，注释说明「失败的 collective_rpc 会留下未读回复」。

**源码事实：** `disable_extensible_kv_cache` 在 `KVCacheAccess.EXPORTS_FD` 存在时抛 `RuntimeError`，不允许关闭。

**源码事实：** `supported_tasks` 是 `cached_property`，注释写「避免不必要的 RPC」。

**源码事实：** 文件末尾为向后兼容再导出 `UniProcExecutor` / `ExecutorWithExternalLauncher`，而 `get_class` 内部用函数级延迟导入——两者共同绕开循环导入。

**架构推论：** 「一个通用 RPC + 一堆命名封装」是分布式抽象的经典形态，它的价值在于**新增执行器只需实现一个方法**。代价是所有语义都被压进一个 `str` 方法名加 `tuple` 参数——**类型检查在这条边界上完全失效**，写错方法名要到运行期才会发现，而错参数个数要到 worker 侧才发现。

**架构推论：** `output[0]` 这个取值方式隐含一个强假设：**所有 rank 都成功返回，且结果结构一致**。一旦某个 rank 返回 `None` 或抛错，`collective_rpc` 必须先把错误统一成异常，否则 `[0]` 会静默取到别人的结果。这就是为什么「哪些方法用 `all`、哪些只取第一个、哪些要遍历」必须有明确规则——**返回列表里每一份的类型相同，但语义权重不同**。

**架构推论：** `discard` 在 `finally` 里更新本地状态，是「本地状态必须与远端可能的部分执行对齐」的体现。这类「不完美的一致性」在资源释放路径上很常见：宁可本地记成「已释放」而远端还占着（下次 `discard` 是幂等的无操作），也不要本地记成「未释放」而远端已释放（下次会拿到 `use-after-free` 式的错误）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/executor/abstract.py` | `Executor` | 执行器抽象基类 |
| `vllm/v1/executor/abstract.py` | `Executor.get_class` | 按配置把 backend 字符串解析成执行器类 |
| `vllm/v1/executor/abstract.py` | `Executor.collective_rpc` | 唯一的通用 RPC 抽象，同步/异步双形态 |
| `vllm/v1/executor/abstract.py` | `Executor.execute_model` | 单步执行，取 `output[0]` |
| `vllm/v1/executor/abstract.py` | `Executor.sample_tokens` | 独立采样入口（与执行分离） |
| `vllm/v1/executor/abstract.py` | `Executor.compile_or_warm_up_model` | 收集各 rank 编译耗时并取最大值回填 |
| `vllm/v1/executor/abstract.py` | `Executor.probe_forward` | 逐 rank 检查前向探针错误 |
| `vllm/v1/executor/abstract.py` | `Executor.disable_extensible_kv_cache` | 关闭前校验无 connector 依赖 FD 导出 |
| `vllm/v1/executor/abstract.py` | `Executor.sleep` / `wake_up` / `discard` | 显存休眠状态机与标签管理 |
| `vllm/v1/executor/abstract.py` | `Executor.profile` | 分发前校验 worker 是否接受覆盖参数 |
| `vllm/v1/executor/abstract.py` | `_check_worker_profile_kwargs` | 用签名反射预检参数兼容性 |
| `vllm/v1/executor/abstract.py` | `SLEEP_TAGS` | 显存池标签常量（权重 / KV cache） |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/v1/executor/abstract.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/executor/abstract.py)
- [vllm 文档：分布式部署与执行后端](https://docs.vllm.ai/en/latest/serving/distributed_serving.html)
- [Python 文档：typing.overload 与类型收窄](https://docs.python.org/3/library/typing.html#typing.overload)

## 与主核 / AI 的关系

`collective_rpc` 与主核里「统一的算子调用接口」是同一层抽象：**把 N 种执行形态压成一个调用点**。主核的执行器把「顺序扫描 / 索引扫描 / 并行扫描」压在同一个元组协议后面；vllm 把「单进程 / 多进程 / Ray」压在同一个方法名加参数后面。**两者都用「字符串标识动作 + 元组传参」换取了新增实现的低门槛，也都因此失去了编译期检查。**

**控制面 RPC 与数据面分离 vs 主核的元数据与数据分离。** 主核把「表结构、统计信息」与「实际数据页」分开；vllm 把「调度参数、控制信号」与「张量」分开。共同理由是**两者的传输特性完全不同**：控制消息小、需要可靠与顺序，张量巨大、只需要带宽。混在一条通道上，要么控制被大传输阻塞，要么大传输被控制消息切碎。

**`sleep` / `wake_up` 状态机 vs 主核的页面换出。** 主核把冷页换到磁盘、需要时读回；vllm 把权重与 KV cache 从显存释放、需要时重新加载。共同点是**「释放」不是「丢弃」**——必须有足够的状态记录来支持恢复。差别在代价：主核换出的是数据（必须完整保存），vllm 睡眠后是靠重新加载权重文件恢复（不需要保存内容）。所以 vllm 的 `sleeping_tags` 只需记「哪些池被释放」，不需要记内容。

对 agent 工程最直接的价值有四条。其一：**多层执行后端应该收敛成一个 RPC 接口**，新增一个后端只要实现一个方法，而不是实现整套生命周期。其二：**「取第一个结果」与「所有结果都要一致」是两种语义**，混用会导致静默错配——确定每个批量调用属于哪一类，是这类抽象必须写清的东西。其三：**资源的释放动作要幂等而状态记录可以保守**，`finally` 里无条件标记「已释放」比精确追踪更安全。其四：**失败的集合调用会留下脏管道**，能前置校验的错误必须在分发前拦住。
