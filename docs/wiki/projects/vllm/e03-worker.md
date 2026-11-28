---
project: vllm
feature: worker
title: Worker 与模型执行器
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/worker/worker_base.py
    symbols: [WorkerBase, WorkerWrapperBase]
  - path: vllm/v1/worker/gpu_worker.py
    symbols: [Worker]
---

# Worker 与模型执行器

## 一句话职责

`WorkerBase` 定义「一张卡上要做哪些事」的接口，`WorkerWrapperBase` 是那个「一个进程对应一个 wrapper」的延迟初始化壳，而 `Worker` 是 GPU 上的具体实现——三者叠起来让「多个 worker 进程」看起来像一个可 `collective_rpc` 的对象。

## 机制是什么

这里有**三个不同的抽象层**，容易混：

| 层 | 类 | 存在形态 | 职责 |
|---|---|---|---|
| 接口 | `WorkerBase` | 每 rank 一个实例 | 声明「一张卡能做哪些事」 |
| 进程壳 | `WorkerWrapperBase` | 每进程一个实例 | 延迟构造真 worker、注入扩展、转发属性 |
| 实现 | `Worker`（GPU） | 被 wrapper 持有 | 真正建 device、加载模型、跑前向 |

三层分离的直接原因是**构造时机**。`WorkerWrapperBase` 的 docstring 写得很清楚：

> We first instantiate the WorkerWrapper, which remembers the worker module and class name. Then, when we call `update_environment_variables`, and the real initialization happens in `init_worker`.

**顺序是被环境变量逼出来的**：子进程必须先被创建（拿到 spawn 后的干净进程），再设置 `CUDA_VISIBLE_DEVICES` 之类的环境变量，最后才能构造 worker——因为 worker 在构造时就会读平台与设备信息。所以「创建进程」与「创建 worker」必须分成两步。

`rpc_rank` 与 `global_rank` 的区分是第三个维度。docstring 给了具体例子：**SPMD 式离线推理下，用户启动两个引擎、每个 TP=2，那么两个引擎里的所有 worker 的 `rpc_rank` 都是 0，但它们在 TP 组里的 `global_rank` 不同**。所以「我在这台机器上是第几个被 RPC 找到的」与「我在全局通信组里是第几个」是两件事。

## 怎么实现的

**`WorkerWrapperBase.init_worker` 是一段有固定顺序的启动脚本。** 逐段看：

1. `all_kwargs[self.rpc_rank]` 取出**属于自己那一份**参数。注意是 `rpc_rank` 索引，不是 `global_rank`——每个进程只认自己那份。
2. 断言 `vllm_config` 存在，存到 `self.vllm_config`。
3. `vllm_config.enable_trace_function_call_for_thread()`——为该线程开启调用追踪。
4. `load_general_plugins()`——加载通用插件。
5. `current_platform.register_triton_kernel_overrides()`，注释写「Let the platform replace core Triton kernels before any worker or model code can launch them」——**必须在任何 worker 或模型代码启动内核之前替换**。
6. 解析 `worker_cls`：**只接受字符串**，传类直接抛 `ValueError`，消息要求「把类放到独立模块里并传限定名」。这是为了让子进程能重新 import 它，而不是靠 pickle 传类对象。
7. `worker_extension_cls` 的处理最重：若配了扩展类且它不在 `worker_class.__bases__` 里，就**逐个属性检查冲突**（跳过 dunder），有冲突就 `assert` 失败；没冲突则 `worker_class.__bases__ = worker_class.__bases__ + (worker_extension_cls,)`——**直接改基类元组**。这是一种运行期多重继承注入。
8. `mm_receiver_cache` 从配置造出来，放在 wrapper 上而不是 worker 上（后面解释）。
9. `with set_current_vllm_config(self.vllm_config): self.worker = worker_class(**kwargs)`——**构造被包在配置上下文里**，注释写「To make vLLM config available during worker initialization」。

第 7 步那段值得细看。它做的是「让 `collective_rpc` 能调到用户自定义方法」：用户写一个扩展类，把方法名传给 `collective_rpc`，worker 因为动态多继承了它就能响应。**代价是它修改了 `worker_class` 这个类对象本身**——而 `worker_class` 是从模块里 import 来的，所以这个修改对整个进程里所有该类的实例生效，且不可撤销。

冲突检查用 `assert` 而不是抛 `ValueError`，意味着 `python -O` 下会失效——不过在生产里不会跑 `-O`。

**`WorkerWrapperBase.execute_model` 比 worker 的同名方法多做一件事。**

```python
def execute_model(self, scheduler_output):
    self._apply_mm_cache(scheduler_output)
    return self.worker.execute_model(scheduler_output)
```

`_apply_mm_cache` 遍历 `scheduler_output.scheduled_new_reqs`，把每个请求的 `mm_features` 用 `mm_cache.get_and_update_features(...)` 换一遍。**多模态特征的解析被放在 wrapper 层而非 worker 层**，理由是它需要在「进入模型之前」发生，且与具体硬件无关——任何 worker 实现都需要这一步。这也解释了为什么 `mm_receiver_cache` 存在 wrapper 上。

**`__getattr__` 让 wrapper 看起来像 worker。**

```python
def __getattr__(self, attr: str):
    return getattr(self.worker, attr)
```

这一行是「透明代理」的全部。它让 `collective_rpc("some_method")` 能直接在 wrapper 上 `getattr` 到 worker 的方法。风险在于 `self.worker` 本身也是属性——若 `init_worker` 还没跑，访问任何属性都会递归进 `__getattr__` 再访问 `self.worker`，而 `self.worker` 不存在。**当前实现没有防这个递归的保护**，依赖「没人会在初始化前访问属性」这个约定。

`shutdown` 里有一个针对扩展类的清理钩子：先调 `destroy_worker_extension`（若存在），异常只记日志、**不向上抛**，注释写「Don't raise: callers still need to run their own teardown.」——**清理阶段的异常不该阻断后续清理**。

**`WorkerBase` 的默认实现密度很高，这本身就是信息。** 大量方法有真实默认体而非 `raise NotImplementedError`：

- `set_mamba_block_size`：直接写进 `cache_config`。
- `get_supported_kv_cache_layouts`：从注意力后端查支持的布局。
- `set_kv_cache_layout` / `disable_extensible_kv_cache`：写配置。
- `check_health`：`return`（空）。
- `notify_kv_cache_ready`：若有 KV 传输组则先 `synchronize_device()` 再 `on_kv_cache_ready()`。
- `synchronize_device`：`torch.accelerator.synchronize()`，注释提醒「backends outside `torch.accelerator` must override」。
- `reset_mm_cache` / `release_late_interaction_query_cache`：用 `getattr(self.model_runner, name, None)` 探测后调用——**模型执行器不一定实现这些，所以是软调用**。
- `apply_model`：`fn(self.get_model())`。
- `get_model_inspection`：调 `format_model_inspection`。
- `vocab_size`：从 `model_config` 读。
- `supports_draft_weight_updates` / `has_incomplete_weight_update`：默认 `False`。
- `extensible_kv_cache_unsupported_reason`：**默认返回一句「XXX 不支持可扩展 KV cache」**，而不是抛错。

真正 `raise NotImplementedError` 的只有：`get_kv_cache_layer_configs`、`compile_or_warm_up_model`、`init_device`、`get_model`、`load_model`、`execute_model`、`sample_tokens`、四个 LoRA 方法。

这个划分有清晰的标准：**「所有硬件都该有的固有能力」给默认实现，「某类硬件特有的动作」才要求实现**。`extensible_kv_cache_unsupported_reason` 默认返回一句话最能说明这个思路——「不支持」是一种合法的答案，不需要抛错。

**`execute_model` 的返回值有三种可能，`sample_tokens` 的注释锁定了契约。**

```python
def execute_model(self, scheduler_output) -> ModelRunnerOutput | AsyncModelRunnerOutput | None:
    """If this method returns None, sample_tokens should be called immediately after..."""
```

三种返回：`ModelRunnerOutput`（本步就出结果）、`AsyncModelRunnerOutput`（异步形态）、`None`（需要紧跟 `sample_tokens`）。docstring 还留了一句「Note that this design may be changed in future if/when structured outputs parallelism is re-architected」——**承认这是临时设计**。这与引擎核心那页的「执行与采样分离」是同一条链。

`sample_tokens` 的 docstring 只有一行，却是一条硬契约：「Should be called immediately after execute_model iff it returned None.」——**`iff` 明确了条件**，不是「可以调」，而是「当且仅当返回 None 时才调」。

**`probe_forward` 返回错误而不是抛错误。** docstring 写：

> Errors are returned, not raised, so the executor reads every worker's reply and its reply queues stay in step.

这与 `Executor.probe_forward` 遍历所有 rank 检查错误正好配对：**worker 侧不抛，因为一抛就会让 RPC 管道里留未读回复**。这条与上一页 `profile` 的前置校验是同一个约束的两个表现。

**`Worker`（GPU 实现）的构造期做了一批一次性设置。**

- `HAS_TRITON` 检查：没有 Triton 且平台不是独立 CPU model runner 就抛错——**依赖在构造期就校验，而不是等到第一次前向**。
- `torch.set_float32_matmul_precision(envs.VLLM_FLOAT32_MATMUL_PRECISION)`：全局精度设置，进程级副作用。
- `elastic_ep_executor`：延迟构造，注释写「so platform workers can opt out」——**子类可以不建它**。
- `worker_sentinel`：只有开了容错才建。
- `weight_transfer_engine` 故意留成 `None`，注释写「created in `load_model` once the model is available, since the engine needs a reference to the model」。
- 三个权重更新的状态字段：`_weight_update_active`、`_incomplete_weight_updates`（按 `is_draft` 区分的集合）、`_weight_update_is_draft`。
- `_weight_update_kv_cache_loan = ExitStack()`，注释写「Keeps the KV cache lent to the active weight update until it ends」——**用 `ExitStack` 管理「借出的资源」**，权重更新结束才归还。
- `_kv_cache_reclaimable`，注释写「Set by the engine while a pause has emptied the KV cache」。
- `_pp_send_work: list[Handle]`：上一步流水线并行发送的句柄，注释「Device handles of the previous step's PP intermediate-tensor send」——这就是 e01 里 `_RetainedHandle` 幂等 `wait` 的服务对象。
- `_pp_transport` 与 `_pp_recv_specs`：`VLLM_PP_TRANSPORT` 选的传输方式，`None` 表示走 NCCL。

**`sleep` 分两级，level 2 会丢 draft 权重。** `_discardable_parameters` 的实现值得读：

1. 取目标模型的全部命名参数；
2. 若存在 draft 模型，把 draft 的独立参数（`id` 不在目标参数集合里的）以 `draft.` 前缀加进去——**共享参数不重复计**；
3. 对 `sleep_preserve_parameter_names` 里每个模式做 `fnmatch_filter`，把匹配到的参数**记进 preserved**；
4. 若某个模式一条都没匹配到且模式不是 `draft.*`，抛 `ValueError`——**配置写错要立刻报**；
5. 若存在 draft 参数但没有任何保留模式覆盖它，`logger.warning_once` 提示「level 2 睡眠会丢弃 draft 权重，而 `reload_weights` 只恢复 target」。

第 4、5 两步合起来说明：**「保留清单」是一个需要被校验的配置**，而不是一个可选的提示。写错模式名会失败，漏掉 draft 会警告。

**`sleep` 本体先让模型加载器知情。** `if loader := _bound_model_loader(self.model_runner): loader.on_sleep(level)`——**加载器可能持有自己的资源（如 weight transfer 的 staging buffer），睡眠前要它先放手**。然后 `torch.accelerator.synchronize()` 再读显存，保证读数是稳定的。

## 关键取舍

**源码事实：** 三层抽象（`WorkerBase` 接口 / `WorkerWrapperBase` 进程壳 / `Worker` 实现）分离，因为「建进程 → 设环境变量 → 建 worker」必须有序。

**源码事实：** `rpc_rank` 用于索引参数列表与定位自己的那份 env/kwargs；`global_rank` 用于 KV cache 配置列表索引（`initialize_from_config` 用 `self.global_rank` 取）。两者在 SPMD 式多引擎部署下不同。

**源码事实：** `init_worker` 的执行顺序是「取参数 → 断言配置 → 开追踪 → 载插件 → 注册 Triton 覆盖 → 解析 worker_cls → 注入扩展类 → 建多模态缓存 → 在配置上下文里构造 worker」。

**源码事实：** `worker_cls` 只接受字符串限定名，传类对象直接抛 `ValueError`。

**源码事实：** 扩展类通过 `worker_class.__bases__ = worker_class.__bases__ + (worker_extension_cls,)` 动态注入，注入前逐属性检查冲突（跳过 dunder）。

**源码事实：** `WorkerWrapperBase.execute_model` 先把每个新请求的多模态特征过一遍接收缓存，再调 worker。

**源码事实：** `WorkerBase.execute_model` 可返回 `None`，此时必须紧跟 `sample_tokens`；`probe_forward` 返回错误字符串而不抛。

**源码事实：** `WorkerBase.extensible_kv_cache_unsupported_reason` 的默认实现返回一句「不支持」而不是抛错。

**源码事实：** `Worker.__init__` 直接改全局 matmul 精度；`weight_transfer_engine` 延迟到 `load_model` 才建；`_weight_update_kv_cache_loan` 是 `ExitStack`。

**源码事实：** `_discardable_parameters` 对未匹配的保留模式抛 `ValueError`（`draft.*` 除外），对未保留的 draft 参数发一次 warning。

**架构推论：** 「worker_cls 必须是字符串」这条限制的根因是**子进程要能自己 import 它**。传类对象就得序列化类，而类对象跨进程传递会丢失模块身份（同一个类在两个进程里是两个不同对象），`isinstance` 与注册表都会失效。**用字符串限定名让每个进程各自 import，得到的是同一个模块对象**——这是分布式系统里传递「类型」的通用做法。

**架构推论：** 动态改 `__bases__` 注入扩展，是把「用户自定义方法」纳入 RPC 命名空间的最省手段，但它有两个代价：一是**修改全局类对象**（同进程所有实例共享），二是**绕过了静态检查**（扩展方法不会出现在任何类型的属性列表里）。用它的前提是「注入只发生一次、且在 worker 构造之前」，代码靠 `worker_extension_cls not in worker_class.__bases__` 这个判重来保证。

**架构推论：** 「默认实现密度高」说明 `WorkerBase` 不是纯接口而是**带默认行为的骨架**。这让「写一个最小 worker」变得可行——只实现 6 个真正必需的方法就能跑。代价是「不实现」与「实现成空」在类型上无法区分：`check_health` 默认什么都不做，与「健康检查通过」在行为上等价。

**架构推论：** `probe_forward` 返回错误而非抛出，暴露了 P2P/RPC 的一个基本约束：**在管道式的请求-回复协议里，任何一端的异常都会让另一端的回复队列错位**。所以协议层要求「每个请求都必须有一个正常回复」，异常要编码成数据。这是所有自研 RPC 都会遇到的第一个设计问题，而「错误即数据」是最稳的解法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/worker/worker_base.py` | `WorkerBase` | 单卡能力接口 + 大量默认实现 |
| `vllm/v1/worker/worker_base.py` | `WorkerBase.execute_model` | 可返回 `None` 表示需要后续采样 |
| `vllm/v1/worker/worker_base.py` | `WorkerBase.probe_forward` | 就绪探针，错误作为返回值 |
| `vllm/v1/worker/worker_base.py` | `WorkerBase.notify_kv_cache_ready` | 先同步设备再通知 KV 传输组 |
| `vllm/v1/worker/worker_base.py` | `WorkerBase.extensible_kv_cache_unsupported_reason` | 默认返回「不支持」而非抛错 |
| `vllm/v1/worker/worker_base.py` | `WorkerWrapperBase` | 进程壳：延迟构造、注入扩展、属性转发 |
| `vllm/v1/worker/worker_base.py` | `WorkerWrapperBase.init_worker` | 有严格顺序的启动脚本 |
| `vllm/v1/worker/worker_base.py` | `WorkerWrapperBase.execute_model` | 先应用多模态特征缓存再执行 |
| `vllm/v1/worker/worker_base.py` | `WorkerWrapperBase.__getattr__` | 让 wrapper 透明代理 worker 属性 |
| `vllm/v1/worker/worker_base.py` | `CompilationTimes` | 编译耗时与可扩展 KV 块数 |
| `vllm/v1/worker/gpu_worker.py` | `Worker` | GPU 实现（device、模型、PP、睡眠、权重更新） |
| `vllm/v1/worker/gpu_worker.py` | `Worker.__init__` | 一次性设置与状态字段初始化 |
| `vllm/v1/worker/gpu_worker.py` | `Worker._discardable_parameters` | 睡眠二级的可丢弃参数集与保留校验 |
| `vllm/v1/worker/gpu_worker.py` | `Worker.sleep` | 先让加载器知情、同步、再按级别释放 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/v1/worker/worker_base.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/worker/worker_base.py)
- [vllm 源码：vllm/v1/worker/gpu_worker.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/worker/gpu_worker.py)
- [vllm 文档：架构概览与 Worker 角色](https://docs.vllm.ai/en/latest/design/arch_overview.html)

## 与主核 / AI 的关系

三层 worker 抽象与主核里「存储引擎的 `amhandler` → 具体 AM → 其内部状态」是同一种分层：**接口声明能力、工厂负责实例化、实现在具体硬件上干活**。差别在于 vllm 多出一个「进程壳」层，因为它的执行单元是独立进程；主核的 AM 跑在同一进程里，不需要跨进程重建。

**「worker_cls 必须是字符串」vs 主核的注册表。** 主核把 AM 实现注册进系统表（`pg_am`），靠名字引用；vllm 用字符串限定名让每个子进程各自 import。**两者都把「类型」降级成「名字」，原因都是跨边界**（主核跨事务与版本、vllm 跨进程）。名字可以序列化、可以存进配置文件，类对象不行。

**「错误即数据」vs 主核的错误码。** 主核的 C 接口用返回码而不是 `longjmp`/异常，也是为了让「调用方一定要处理」。vllm 的 `probe_forward` 返回错误字符串也是同一动机。**在需要保持通道同步的协议里，异常必须被降级成返回值。**

**睡眠的「保留清单」vs 主核的 `CHECKPOINT`/`fsync` 边界。** 主核决定「哪些必须落盘、哪些可以重建」；vllm 决定「哪些参数必须保留、哪些可以重新加载」。**两者都是把「有价资源」与「可重建资源」分开**，区别在主核的重建靠重放日志，vllm 的重建靠重新读权重文件。这也解释了为什么 vllm 的 `sleep_preserve_parameter_names` 写错会导致**配置直接报错**而不是静默降级——错误的保留清单会让重建假设失效。

对 agent 工程最直接的价值有四条。其一：**「建进程 → 配环境 → 建对象」这个顺序在 agent 的沙箱化执行里完全一样**，把对象构造放在错误的阶段会让环境变量失效。其二：**跨进程传类型要用限定名字符串**，不要 pickle 类对象。其三：**RPC 的每一端都必须有正常回复**，异常要么编码成返回值，要么整个通道作废。其四：**资源释放前先让持有者知情**——vllm 睡眠前先调加载器的 `on_sleep`，agent 中止任务前也应先让工具层交出资源。
