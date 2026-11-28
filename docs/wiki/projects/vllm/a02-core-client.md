---
project: vllm
feature: core-client
title: 核心客户端与进程传输
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/engine/core_client.py
    symbols: [EngineCoreClient, InprocClient, MPClient]
  - path: vllm/v1/engine/core_client.py
    symbols: [SyncMPClient, AsyncMPClient, DPAsyncMPClient]
---

# 核心客户端与进程传输

## 一句话职责

`EngineCoreClient` 是前端进程与引擎核心之间的**唯一接口**：它把「加请求、取输出、调运维方法」这三类操作，翻译成三种传输形态之一——同进程直调、ZMQ + 后台线程、ZMQ + asyncio。

## 机制是什么

引擎核心可以跑在同一个进程里，也可以跑在子进程里，还可以跑在多进程 + 数据并行里。这三种部署形态在**语义上完全相同**（都是「推请求、拉输出」），但在**阻塞行为上根本不同**：

| 形态 | 类 | 传输 | 调用方阻塞方式 |
|---|---|---|---|
| 进程内 | `InprocClient` | 直接方法调用 | 同步，`step()` 就地推进 |
| 多进程同步 | `SyncMPClient` | ZMQ + 后台收包线程 | 阻塞在 `Queue.get()` |
| 多进程异步 | `AsyncMPClient` | ZMQ + asyncio 收包任务 | `await Queue.get()` |

`EngineCoreClient` 是一个纯抽象类：二十多个方法全部 `raise NotImplementedError`，只有 `shutdown` 是 `@abstractmethod`。**这个组合不是笔误**——它意味着子类只要实现了 `shutdown` 就能实例化，其余方法的缺失只会在调用时暴露。这是为了让「不同形态支持不同能力」成为可能，而不是逼所有子类实现全部方法。

形态选择在 `make_client` 里做，四条分支，其中一条是硬拒绝：

```
asyncio_mode and not multiprocess_mode → NotImplementedError
multiprocess_mode and asyncio_mode     → make_async_mp_client
multiprocess_mode and not asyncio_mode → SyncMPClient
否则                                   → InprocClient
```

**「asyncio 但不用多进程」被明确禁止**。理由不难推：进程内形态没有并发吞吐可言，而 asyncio 的全部价值在于「等 ZMQ 时可以处理别的请求」；两者组合既拿不到性能又增加了复杂度。这是一条**否掉无意义组合**的校验。

## 怎么实现的

### InprocClient：薄到几乎没有形态

`InprocClient` 的每个方法都是三行以内的转发。`get_output` 是唯一有点内容的一个：

```python
outputs, model_executed = self.engine_core.step_fn()
self.engine_core.post_step(model_executed=model_executed)
self.last_step_model_executed = model_executed
return outputs and outputs.get(0) or EngineCoreOutputs()
```

三件事：**推进引擎一步**、**跑后置钩子**、**记下本步是否真跑了模型**。最后那个 `outputs and outputs.get(0) or EngineCoreOutputs()` 是个惯用的空值兜底——空的 `outputs` 字典会退化成空的 `EngineCoreOutputs` 对象而不是 `None`，保证调用方不用判空。

`last_step_model_executed` 的注释写着「Read by LLMEngine.step() to keep the DP ranks' collectives aligned」——**数据并行下各 rank 必须知道彼此是否都执行了模型**，否则集合通信会错位。这个字段就是那条同步信息的载体。

**`__init__` 里有个值得注意的防 GC 细节**：

```python
self._finalizer = weakref.finalize(self, self.engine_core.shutdown)
```

注释解释了两点：一，要在客户端被回收时释放执行器 worker，否则它们会占着 GPU 内存活到进程结束；二，**回调不能引用 `self`**，否则 `self` 永远可达、finalizer 永不触发。所以这里绑的是 `self.engine_core.shutdown` 这个**实例方法本身**而不是 lambda。这是一个容易写错的地方——`weakref.finalize(self, lambda: self.engine_core.shutdown())` 会造成永久泄漏。

### MPClient：ZMQ 骨架与启动握手

`MPClient` 是所有多进程客户端的共同基类，它做四件事。

**一，建 ZMQ 上下文与资源容器。** 先建同步 `zmq.Context(io_threads=2)`，asyncio 模式下再包一层 `zmq.asyncio.Context(sync_ctx)`。然后：

```python
self.resources = BackgroundResources(ctx=sync_ctx)
self._finalizer = weakref.finalize(self, self.resources)
```

**这里同样是为了切断对 `self` 的引用**——`BackgroundResources` 是个 `@dataclass(slots=True)`，装着 ctx、engine_manager、coordinator、各个 socket、后台 task。注释说明它是「Used as a finalizer for clean shutdown, avoiding circular reference back to the client object」。也就是说：资源清理逻辑需要的所有东西被搬进一个独立对象，客户端本身可以被回收。

`success = False` + `try/finally` 的写法是构造期异常安全：构造中途失败就主动调 `self._finalizer()` 把已建资源释放掉。

**二，两种 socket 归属。** 有 `client_addresses` 时是「引擎由外部管理」：用传入的地址建 ROUTER（收请求）与 PULL（发输出），**并且立刻启动多模态预热**（因为没有 fork 竞态）。没有时是「客户端自己管引擎」：先建 socket、绑地址，再 `launch_core_engines(...)` 起引擎进程，**在进程 fork 完成之后才启动预热**。

注释把原因讲得很细：「it must not run while fork() is in flight (a live thread holding a lock would deadlock the forked child), but the engine-core model load (minutes) that follows is exactly what the warmup should overlap with.」——**fork 期间不能有活线程持锁**（子进程会死锁），而紧随其后的权重加载要几分钟，正是预热该并行的窗口。这个「既不能早也不能晚」的时机判断是这份代码里最细的一处。

**三，串行化器与张量 IPC。** `MsgpackEncoder(oob_tensor_consumer=tensor_ipc_sender)` 与 `MsgpackDecoder(EngineCoreOutputs)`。张量 IPC 只在 `mm_tensor_ipc == "torch_shm"` 且拿到了 `tensor_queue` 时才建 `TensorIpcSender`——**多模态张量走共享内存而不是塞进 msgpack**，避免大张量被序列化成字节。

**四，就绪握手。** 每个引擎的 ZMQ identity 是 `rank.to_bytes(2, "little")`（两字节小端）。客户端建一个这些 identity 的集合，然后在一个**同步影子 socket**（`zmq.Socket.shadow`）上循环收 ready 消息，每收到一个就从集合里删一个，直到集合空。

超时用 `VLLM_ENGINE_READY_TIMEOUT_S`，超时抛 `TimeoutError` 且**错误消息写得极其具体**：告诉用户「通常是权重大导致加载慢」，并直接给出环境变量名与设置方法。这是可运维性上的一处好实践——错误消息本身就是排查手册。

`zmq.Socket.shadow` 的用法值得注意：把一个 asyncio socket 包装成同步 socket 来阻塞收包，绕开「构造函数里不能 await」的限制。

**`validate_alive` 是收包路径上的健康检查。** 它接 `frames` 参数——每帧检查一次，一旦发现引擎死了就抛出。`_format_exception` 则把任意异常在有引擎死掉时替换成 `EngineDeadError(suppress_context=True)`——**根因清楚时不要让上层去猜**。

### SyncMPClient：后台线程 + 队列

同步版在 `MPClient` 之上加了一层「收包线程 → `queue.Queue` → 调用方阻塞取」。

`process_outputs_socket` 是这个线程的主体，两个设计点：

**用一个 `zmq.PAIR` socket 做退出信号。** 线程里 bind 一个 inproc 路径，`Poller` 同时注册 shutdown socket 与 output socket。轮询返回时若 `len(socks) == 2` 或第一个是 shutdown socket，就 `break`。**用 socket 而不是 Event 是因为要同时等两件事**——`Poller` 天然支持多路等待，而 `Event` 与 socket 无法一起等待。

**线程不持有客户端引用。** 构造时把 `ctx`、`out_socket`、`decoder`、`utility_results`、`outputs_queue` 全部**先赋给局部变量**再进闭包，然后 `self.resources.output_socket = None`（注释：线程接管了关闭 socket 的责任）。这是一套刻意切断引用的写法，用意同前——避免客户端无法被回收。

异常经队列转交：`outputs_queue.put_nowait(e)`，`get_output` 里 `isinstance(outputs, Exception)` 就 `raise self._format_exception(outputs) from None`。**线程里的异常不能直接抛**（抛了没人接），只能经由队列搬到调用线程抛。

`_send_input` 里 `msg = (self.core_engine, request_type.value, *self.encoder.encode(request))`——三元组：目标 identity、请求类型、序列化载荷。注释说明 `copy=False` 的零拷贝张量帧由 zmq 自己保活，有从 memoryview 回指原张量的引用链——**这是「零拷贝」能成立的前提，说清它就是防止别人误加 copy。**

`call_utility` 用一个 `Future` + `uuid.uuid1().int >> 64` 的 call_id 实现「同步 RPC」：把 future 存进 `utility_results`，把 `(engine_index, call_id, method, args)` 发出去，然后 `future.result()` 阻塞等。收包线程收到 `utility_output` 时填 future。**这就是在消息队列上手工实现请求-响应关联。**

`get_output` 里还有一句 `if outputs.wave_complete is not None: self.engines_running = False`——**「这一批跑完了」是清除 DP 运行标志的唯一时机**。

### AsyncMPClient：把线程换成 task

异步版把收包线程换成 `asyncio.Task`，其余结构对应：

- `outputs_queue` 从 `queue.Queue` 换成 `asyncio.Queue`。
- 收包循环从 `poller.poll()` 换成 `await output_socket.recv_multipart()`。
- `_ensure_output_queue_task()` 做成惰性启动：构造函数里先试 `asyncio.get_running_loop()`，能拿到就立刻启动 task，拿不到就等第一次用。

那个「先试一下有没有事件循环」的写法有明确理由：「If it is not started here, we could miss EXECUTOR_FAILED messages from engine core if they occur prior to any requests being sent.」——**如果等到第一个请求才启动收包 task，那种在请求之前发生的引擎故障消息会被漏掉。**

收包循环里有三类特殊分支（按 `call_id` 分流）：弹性 EP 通知、容错状态上报、普通 utility 结果。前两类都通过 `_self_ref()` 弱引用取客户端——**再次是为了不阻碍 GC**，取不到就 `return`（客户端已回收，停止循环）。

`except asyncio.CancelledError: outputs_queue.put_nowait(EngineDeadError())` 这一行很关键：task 被取消时不能让调用方永远卡在 `await queue.get()`，所以塞一个「引擎已死」进去。**取消也要有个明确的结束信号。**

`get_output_async` 与同步版同构：`await queue.get()` → 判异常 → 抛。

**utility 调用有三个变体**，这个梯度很能说明 DP 下的需求：

- `call_utility_async`：只发给 `self.core_engine`（第一个引擎）。
- `call_utility_all_async`：`asyncio.gather` 发给所有受管引擎，每个返回一个结果。
- `call_utility_consensus_async`：拿 all 的结果，**若不一致就抛 `RuntimeError`**，一致则返回第一个。

第三个的存在说明「有些配置必须全局一致」——比如支持的任务类型、某些能力开关。用共识判定而不是取自任一个，把「不一致」从静默错误变成显式失败。

### DPAsyncMPClient：数据并行的外层

`DPAsyncMPClient` 在 `AsyncMPClient` 之上加 DP 相关状态：`current_wave`（批次波次）、`lb_engines`（每个引擎的「等待数 / 运行数 / KV 占用」三元组，只有带内部负载均衡的子类用）、以及弹性扩缩的协调器与 socket。

它的替身 `DPLBAsyncMPClient` 才是真正做均衡的那个：维护 `reqs_in_flight`（请求 ID → 引擎 identity，用于把取消路由到正确引擎）、`streaming_reqs`（流式输入请求集合，**取消时要发给所有引擎**）、`engine_inflight`（每个引擎的未完成请求精确计数）。

**取消路由的差别值得单看**：普通请求知道自己在哪个引擎上跑，所以取消发给那一个；流式输入请求在会话建立时可能跨引擎，所以取消要广播。**「知道该发给谁」是一个需要额外维护的状态，不是天然具备的。**

## 关键取舍

**源码事实：** `EngineCoreClient` 只有 `shutdown` 是抽象方法，其余方法默认抛 `NotImplementedError`——能力缺失在调用时暴露而非实例化时。

**源码事实：** 「asyncio 但不用多进程」被 `make_client` 明确拒绝。

**源码事实：** `InprocClient` 与 `MPClient` 都用 `weakref.finalize`，且回调刻意不引用 `self`（`MPClient` 把资源搬进独立 `BackgroundResources` 对象）。

**源码事实：** 多模态预热必须**在 fork 之后**启动（fork 期间有活线程持锁会死锁子进程），且要赶上权重加载那几分钟。

**源码事实：** 引擎 identity 是 `rank.to_bytes(2, "little")`；就绪握手在 `zmq.Socket.shadow` 的同步视图上阻塞收包。

**源码事实：** 同步收包线程用 `zmq.PAIR` socket 做退出信号（与输出 socket 一起 `Poll`），并把所有依赖先提到局部变量以切断对客户端的引用。

**源码事实：** 异步收包 task 被取消时塞一个 `EngineDeadError` 进队列，避免调用方永久等待。

**源码事实：** utility 调用有单引擎 / 全引擎 / 共识三档，共识不一致直接抛错；流式输入请求的取消要广播。

**架构推论：** 「同一接口三种传输」是这个文件存在的全部理由。它让上层（`LLM` / `AsyncLLM`）完全不必知道引擎在哪个进程。代价是**每个新方法要实现三遍**（`InprocClient` 一遍、`SyncMPClient` 一遍、`AsyncMPClient` 一遍），而漏一遍不会在构造时发现——这正是「只有 `shutdown` 是抽象方法」的副作用。

**架构推论：** 三个地方重复出现「切断对 self 的引用」的手法（`InprocClient` 的 finalizer、`MPClient` 的 `BackgroundResources`、`SyncMPClient` 的闭包局部变量），说明 Python 的 GC + `weakref.finalize` 组合在这里是一个持续的成本。**资源清理与对象生命周期绑定时，任何反向引用都会把清理变成泄漏。**

**架构推论：** 「共识判定」出现在 utility 调用上，说明 DP 下「配置必须全局一致」是一个需要主动检查的假设，而不是架构保证。**分布式系统里，假设一致就要验证一致，否则不一致会在很久以后以奇怪的方式暴露。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/engine/core_client.py` | `EngineCoreClient` | 前端与引擎核心的唯一抽象接口 |
| `vllm/v1/engine/core_client.py` | `EngineCoreClient.make_client` | 四分支形态选择，拒绝 asyncio+单进程 |
| `vllm/v1/engine/core_client.py` | `make_async_mp_client` | 按 DP 与 LB 策略选具体异步客户端 |
| `vllm/v1/engine/core_client.py` | `InprocClient` | 同进程直调，`get_output` 即推进一步 |
| `vllm/v1/engine/core_client.py` | `BackgroundResources` | 独立资源容器，供 finalizer 使用 |
| `vllm/v1/engine/core_client.py` | `MPClient` | ZMQ 骨架、启动握手、就绪等待 |
| `vllm/v1/engine/core_client.py` | `MPClient._start_mm_warmup` | fork 之后才启动的多模态预热 |
| `vllm/v1/engine/core_client.py` | `MPClient.validate_alive` | 每帧健康检查 |
| `vllm/v1/engine/core_client.py` | `SyncMPClient` | 后台收包线程 + `queue.Queue` |
| `vllm/v1/engine/core_client.py` | `SyncMPClient.call_utility` | Future + call_id 手工关联请求响应 |
| `vllm/v1/engine/core_client.py` | `AsyncMPClient` | asyncio task 收包 + 惰性启动 |
| `vllm/v1/engine/core_client.py` | `AsyncMPClient.call_utility_consensus_async` | 全引擎一致才返回 |
| `vllm/v1/engine/core_client.py` | `DPAsyncMPClient` | 数据并行外层状态 |
| `vllm/v1/engine/core_client.py` | `DPLBAsyncMPClient` | 取消路由与每引擎在途计数 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：v1/engine/core_client.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/engine/core_client.py)
- [ZeroMQ 指南：ROUTER/DEALER 与身份](https://zguide.zeromq.org/docs/chapter3/)
- [Python 文档：weakref.finalize](https://docs.python.org/3/library/weakref.html#weakref.finalize)

## 与主核 / AI 的关系

「同一语义接口、多种传输实现」与主核里「同一份页读写接口、多种存储后端（本地文件 / 对象存储 / 内存盘）」是同一层抽象。差别在**失败模式的可见度**：主核的后端切换通常在启动时就验证（打开文件、探测挂载），而这里的能力缺失要等具体方法被调用才暴露。**抽象基类把方法全设成默认抛异常，换来的灵活性正是这个延迟暴露的代价。**

「切断对 self 的引用才能让 finalizer 生效」这件事，在主核里对应「资源句柄不能反向持有管理器」。数据库里页缓冲区的清理依赖引用计数归零，任何循环都会让缓冲区永不释放——Python 的 `weakref.finalize` 是同一个问题的语言级形态。

「共识判定」与主核里「各副本的 catalog 必须一致」的判断相通：主核在读到不一致的元数据时会报错而不是忽略。**分布式系统里，一致性只能靠主动验证，不能靠假设。**

对 agent / RAG 工程的实际含义有四条。其一，本地调试与生产部署走的可能是**完全不同的客户端实现**，本地能跑的形态（`InprocClient`）在生产（`AsyncMPClient`）下的错误语义不同，尤其是取消与超时。其二，「fork 之后才能起后台线程」这条约束意味着任何「在模型加载前就想并行干点什么」的设计都必须挂在正确的时机上。其三，utility 类操作在 DP 下要区分「问一个」「问所有」「必须一致」，选错会在多副本配置下产生静默不一致。其四，流式请求的取消需要广播，而普通请求的取消需要精确定位——如果自己实现路由层，这个差别必须显式处理。
