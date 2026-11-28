---
project: vllm
feature: parallel-state
title: 并行组与通信
page_type: feature
schema_version: 1
refs:
  - path: vllm/distributed/parallel_state.py
    symbols: [GroupCoordinator, init_distributed_environment]
  - path: vllm/distributed/parallel_state.py
    symbols: [all_reduce, reduce_scatter, suspend_device_comms]
---

# 并行组与通信

## 一句话职责

把「一堆 GPU 进程」组织成若干有名字、有固定成员、彼此正交的通信组（张量并行、流水线并行、数据并行、专家并行……），并把每一次集合通信包装成**可按名字查表、可被 torch.compile 追踪、可临时挂起**的操作。

## 机制是什么

一个模型在 8 张卡上跑，实际上同时存在好几套「谁跟谁通信」的关系。以注释里的例子为准：2 路张量并行 × 4 路流水线并行，会建出

- 4 个张量并行组：`[g0,g1]`、`[g2,g3]`、`[g4,g5]`、`[g6,g7]`
- 2 个流水线并行组：`[g0,g2,g4,g6]`、`[g1,g3,g5,g7]`

**同一个 rank 同时属于两个组，而这两个组的语义完全不同**：张量并行组内通信频繁（每个 transformer 层都要 all-reduce），流水线并行组内通信稀疏（只在 stage 边界传激活）。所以「一个 rank 属于哪些组」必须能被精确回答，而且每组的通信方式要能独立配置。

这解释了 vllm 为什么不用 PyTorch 的 `ProcessGroup` 直连，而在外面套一层 `GroupCoordinator`：

1. **每组建两个 `ProcessGroup`**——一个走设备后端（NCCL），一个固定走 `gloo`。代码注释写明后者的用途：「a group with `gloo` backend, to allow direct coordination between processes through the CPU」。**CPU 侧协调（传对象、同步状态）不能走 NCCL**，因为 NCCL 只能搬张量。
2. **每个组有唯一名字**，用作字符串句柄。这是为了让自定义算子能引用组——Dynamo 不支持把 `self` 这样的任意对象传给自定义算子。
3. **组是弱引用注册的**，所以「组被销毁」是一个真实可检测的状态，而不是靠调用方自觉。

## 怎么实现的

**`GroupCoordinator` 的构造要选一条并行建组路径。** 环境变量 `VLLM_DISTRIBUTED_USE_SPLIT_GROUP` 决定走哪条：

- 关（默认）：对每个 `group_ranks` 调两次 `torch.distributed.new_group`——一次用配置的设备后端，一次固定 `gloo`。`gloo` 那次套了 `suppress_stdout()`，因为 PyTorch 建组会打日志、而这里建组数量很多。
- 开：走 `_create_subgroups_split_group`，一次建好一对。

无论哪条路，最后都断言 `cpu_group` 与 `device_group` 非空——**「每个组必须同时有 CPU 与设备两个通道」是硬不变量**。

**设备对象是按平台分支算出来的。** CUDA 系用 `logical_device_id_to_visible_device_id` 映射（因为容器里 `CUDA_VISIBLE_DEVICES` 会重排编号），XPU 直接用 `local_rank`，树外平台用 `device_name:index`，其余退到 `cpu`。**这四条的差别是真实环境差异，不是抽象洁癖**：`local_rank` 是「本机第几张卡」，而 `cuda:N` 里的 N 是「可见列表里的第几个」，两者在有设备遮挡时不相等。

**设备通信器只在 `world_size > 1` 时创建。** `device_communicator` 可以为 `None`，这个 `None` 一路传播：`_all_reduce_out_place` 里判空后抛 `ValueError`，而 `_apply_to_device_comms` 里判空后**跳过**。同一个 `None` 在两处有相反处理——一处当错误、一处当正常情况。原因是：单卡时用户不该调集合通信（是错误），但「遍历所有组做挂起」时单卡组本来就没东西可挂起（是正常）。

**`all_reduce` 有三层分流，每一层都有明确理由。**

第一层：`world_size == 1` 直接返回入参。注释写「Bypass the function if we are using only 1 GPU」。

第二层：`use_custom_op_call` 为真时走 `torch.ops.vllm.all_reduce(input_, group_name=self.unique_name)`。docstring 把两个约束写全了：

> We need this because Dynamo does not support passing an arbitrary object (`self` in this case) to a custom op. [...] In addition, PyTorch custom ops do not support mutation or returning a new tensor in the same op. So we always make the all-reduce operation out-of-place.

**「不能传对象」导致必须用字符串名字查表，「不能既改又返」导致必须做成 out-of-place。** 这两条是整段代码形态的根因，而不是风格选择。

第三层是特殊路径：`if self is not _TP: return device_communicator.all_reduce(input_)`——**只有张量并行组的 all-reduce 走「通信与计算重叠」**：

```python
dbo_switch_to_comm_sync("all-reduce-overlap")
out = self.device_communicator.all_reduce(input_)
dbo_yield_and_switch_from_comm_to_compute("all-reduce-overlap")
return out
```

注释说明：每个 TP 的 all-reduce 是一次「交接」，在通信流上跑，同时另一个微批在计算。**只有 TP 组值得这么做**，因为它的 all-reduce 频率最高（每层一次）。

**模块级的 `all_reduce(tensor, group_name)` 是给自定义算子用的查表入口。** 它先 `assert group_name in _groups`，再 `_groups[group_name]()` 取弱引用——取到 `None` 就抛 `ValueError(f"Group {group_name} is destroyed.")`。这个设计的好处是自定义算子只需要一个字符串；坏处是**组被回收后调用会失败而不是静默返回**，这在长跑服务里反而更安全（能立刻发现生命周期错误）。

配套还有 `all_reduce_fake` / `reduce_scatter_fake` / `all_gather_fake`——它们不通信，直接 `torch.empty_like(tensor)` 或按 `world_size` 缩放对应维度后造空张量。**这是给 torch.compile 的假张量追踪用的**：编译期只需要知道输出形状与 dtype，不需要真值。

**`suspend_device_comms` / `resume_device_comms` 是成对的、集合式的、要求通信空闲的。** 它们遍历注册表：

```python
for group_ref in _groups.values():
    group = group_ref()
    if group is None: continue
    dc = group.device_communicator
    if dc is None: continue
    comms.append(dc)
for dc in comms:
    action(dc)
```

**两段循环是刻意的**：先收集再执行，而不是边遍历边调用。因为 `_groups` 是弱引用字典，若 `action` 的副作用触发 GC，边遍历边改字典会抛 `RuntimeError: dictionary changed size during iteration`。先把引用提升成强引用列表，遍历期间字典怎么变都安全。

docstring 里两个限定词值得记住：`(collective; comms must be idle)`。**释放通信器内存是集合操作**——所有 rank 必须同时做，否则剩下的 rank 会等在集合通信里；而且必须在通信空闲时做，否则会把进行中的传输打断。

**`graph_capture` 是一个「同时捕获三种通信器」的上下文。** 它创建一个独立 CUDA stream，在 yield 之前：

1. 若用了自定义 all-reduce 通信器（`ca_comm`），进它的 capture 上下文；
2. 若是 CUDA 通信器且配了 Fi PCIe IPC all-reduce（`fi_pcie_ipc_ar_comm`），也进它的；
3. 取 `aiter_ar_comm`（若有）并进它的。

然后 `stream.wait_stream(curr_stream)`——**做一次流同步再切流**，注释写「ensure all initialization operations complete before attempting to capture the graph on another stream」。

其中有一句注释很关键：「Capture each group's own comm. A global lookup would double-capture」——如果图捕获时去全局查表拿通信器，同一个通信器可能被多个组捕获两次。所以必须从 `self.device_communicator` 上取。

**`init_distributed_environment` 里最绕的一段是为数据并行改编 rank 与 world_size。**

```python
rank = parallel_config.data_parallel_rank * world_size + rank
world_size = parallel_config.world_size_across_dp
```

也就是说 DP 下每个 DP rank 的进程组各自独立编号，再平移到全局空间。**这让「每个 DP 副本」看到的是一个从 0 开始的连续 rank 空间**，而不是「DP0 用 0-7、DP1 用 8-15」这种需要各自偏移的视角。

多节点与 DP 的地址来源也不同：多节点用显式配置的 `master_addr`/`master_port`，单机多 DP 则要**协商端口**。协商用了两段式：先由协调 store 分配一个端口发布 `world_pg_port`，然后 rank 0 让 `TCPStore` 自己 bind 到 0（由内核分配），再把真实端口写回协调 store，其他 rank 从那里读。

```python
if rank == 0:
    world_pg_store = create_tcp_store(ip, 0, ...)
    port = world_pg_store.port
    coord_store.set("world_pg_port", str(port).encode())
else:
    port = int(coord_store.get("world_pg_port").decode())
```

注释解释了为什么协调 store 要「一次性客户端」而不是缓存复用：**缓存的 store 会跨 fork 被继承并与兄弟 worker 共享**，那样端口协商就串了。

**后端不可用会降级到 gloo 并打 warning。** `if not torch.distributed.is_backend_available(backend)` → 记 warning → 断言 gloo 可用 → `backend = "gloo"`。**这是为了让「本机调试」不需要装 NCCL**，代价是单机调试与生产用了不同的通信后端，有些时序问题只在其中一边出现。

**`_split_tensor_dict` 把张量字典拆成「元数据 + 张量列表」。** 元数据里记的是 `value.device.type`（只要类型，如 `"cuda"`），不是 `value.device`。注释写明了原因：「because it contains not only the device type but also the device index (e.g. `cuda:0`). We only need the device type. receiving side will set the device index.」——**发送方的设备编号对接收方无意义**，接收方按自己的 `local_rank` 建张量。

**`_RetainedHandle` 解决了一个 gloo 的具体陷阱。** 它有 `_waited` 标志，`wait` 第一次调用才真等，后续调用直接返回。docstring 写得很坦白：

> gloo `Work.wait()` is single-shot for point-to-point operations — calling it a second time on an already completed send blocks forever (verified empirically). Both the worker's top-of-step drain of `_pp_send_work` and `_reap_completed_isends` may wait the same entry's metadata handle, so the second caller must not re-wait the underlying works.

**这不是防御式编程，是一个实测出来的挂死**。所以「句柄的 wait 必须幂等」成了一条隐式契约。

**几个横切设计值得单独列出：**

- **`make_sibling_device_group`**：建一个成员相同但通信器独立的新组，让两组上的操作可以并发。它是集合调用（每个 world rank 都必须调）。
- **`dead_dp_ranks`**：容错缩容后记录「已死的 DP 槽位」，DP all-reduce 读它来中和那些空列。**故障被建模成数据，而不是分支**。
- **`_pending_isends`**：`isend_tensor_dict` 的在途句柄 FIFO。它的存在是为了让「发完就不管」的调用方也能保证发送缓冲不被提前释放；条目在下次 `isend` 时惰性回收。注释还解释了为什么它不能在 `__init__` 里直接初始化——**有些子类复制了 `__init__` 但没调 `super().__init__`**。
- **`single_pp_stage()` 上下文**：临时把流水线并行当成单 stage，代价是期间所有 PP 组操作被重定向（`in_single_pp_stage` 供别处查询）。
- **`checkpoint_prepare_distributed_state` / `checkpoint_restore_distributed_state`**：把并行状态存下来再恢复，用于「先建组再加载权重」这类需要重入的场景。

## 关键取舍

**源码事实：** 每个 `GroupCoordinator` 同时持有一个设备后端 `ProcessGroup` 与一个固定 `gloo` 的 `ProcessGroup`。

**源码事实：** `_groups` 存的是 `weakref.ref`，取到 `None` 时模块级 `all_reduce` / `reduce_scatter` / `all_gather` 抛「Group ... is destroyed」。

**源码事实：** `use_custom_op_call` 为真时走 `torch.ops.vllm.*`；原因是 Dynamo 不支持把 `self` 传给自定义算子，且自定义算子不支持「原地改 + 返回新张量」，所以集合通信一律 out-of-place。

**源码事实：** 只有 `_TP` 组的 all-reduce 走 `dbo_switch_to_comm_sync` / `dbo_yield_and_switch_from_comm_to_compute` 的通信-计算重叠。

**源码事实：** `suspend_device_comms` / `resume_device_comms` 先收集通信器再逐个操作，且是集合调用、要求通信空闲。

**源码事实：** `graph_capture` 从 `self.device_communicator` 上取通信器（全局查表会重复捕获），并在切流前 `stream.wait_stream(curr_stream)`。

**源码事实：** `init_distributed_environment` 在 DP 下把 `rank` 改成 `data_parallel_rank * world_size + rank`、把 `world_size` 改成 `world_size_across_dp`。

**源码事实：** 单机多 DP 的端口协商是「rank 0 绑定 0 端口 → 把内核分配的端口写进协调 store → 其余 rank 读取」，协调 store 用一次性客户端以避免被 fork 继承共享。

**源码事实：** 设备后端不可用时降级到 `gloo` 并打 warning。

**源码事实：** `_split_tensor_dict` 的元数据只记 `device.type`，不记索引，注释说明索引由接收方设置。

**源码事实：** `_RetainedHandle.wait` 用 `_waited` 保证幂等，注释说明 gloo 的 p2p `Work.wait()` 是一次性的、二次调用会永久阻塞。

**架构推论：** 「字符串名字 + 模块级查表」这个组合是被编译器限制逼出来的，但它带来一个额外好处：**通信操作变成了可被静态图记录的东西**。自定义算子只依赖一个字符串，图捕获时不需要捕获 Python 对象图。代价是名字成了隐式全局命名空间，重名或未注册只能在运行期发现。

**架构推论：** 弱引用注册是把「组销毁」变成可检测错误的关键。如果存强引用，一个已经不该用的组会一直被字典持有，调用方拿到的是一个「看起来还活着」的对象。**弱引用让生命周期由真正的持有者决定，字典只做索引。**

**架构推论：** 「先收集后操作」这个两段式在遍历弱引用字典时是必需的，但它同时解决了另一个问题：`action` 可能触发的 GC 不会改变正在遍历的集合。**这个模式在所有「遍历容器并执行副作用」的地方都成立**，而 Python 的 `RuntimeError: dictionary changed size during iteration` 只是它最明显的症状。

**架构推论：** `wait` 必须幂等的这条契约，本质是「多个回收点共享同一批句柄」的结果。worker 在步首统一排空、`_reap_completed_isends` 又惰性排空，两个路径可能碰到同一条目。**当资源的释放被多处共管时，释放动作就必须幂等**——否则「谁先释放」变成了时序赌博。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/distributed/parallel_state.py` | `GroupCoordinator` | 一个并行组的完整封装（CPU + 设备双通道） |
| `vllm/distributed/parallel_state.py` | `GroupCoordinator.__init__` | 建组、选设备、建通信器与消息广播 |
| `vllm/distributed/parallel_state.py` | `GroupCoordinator.all_reduce` | 三层分流：单卡旁路、自定义算子、TP 重叠 |
| `vllm/distributed/parallel_state.py` | `GroupCoordinator.graph_capture` | CUDA graph 捕获时的通信器上下文 |
| `vllm/distributed/parallel_state.py` | `GroupCoordinator.make_sibling_device_group` | 同成员但独立通信器的并发组 |
| `vllm/distributed/parallel_state.py` | `init_distributed_environment` | 全局初始化，含 DP 的 rank/world_size 平移与端口协商 |
| `vllm/distributed/parallel_state.py` | `initialize_model_parallel` | 按 TP/PP/DP 规模切出各并行组 |
| `vllm/distributed/parallel_state.py` | `all_reduce` | 按名字查表的模块级通信入口（供自定义算子） |
| `vllm/distributed/parallel_state.py` | `reduce_scatter` | 同上，归约散布 |
| `vllm/distributed/parallel_state.py` | `all_reduce_fake` | 编译期假张量版本（只造形状） |
| `vllm/distributed/parallel_state.py` | `suspend_device_comms` | 挂起全部设备通信器（集合、需空闲） |
| `vllm/distributed/parallel_state.py` | `resume_device_comms` | 恢复全部设备通信器 |
| `vllm/distributed/parallel_state.py` | `_apply_to_device_comms` | 先收集后执行的遍历模板 |
| `vllm/distributed/parallel_state.py` | `GraphCaptureContext` | 图捕获用的流容器 |
| `vllm/distributed/parallel_state.py` | `_split_tensor_dict` | 张量字典拆成元数据 + 张量列表 |
| `vllm/distributed/parallel_state.py` | `_RetainedHandle` | 幂等 wait 的在途句柄，保活发送缓冲 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/distributed/parallel_state.py](https://github.com/vllm-project/vllm/blob/main/vllm/distributed/parallel_state.py)
- [vllm 文档：分布式推理与并行策略](https://docs.vllm.ai/en/latest/serving/distributed_serving.html)
- [PyTorch 文档：分布式通信原语](https://pytorch.org/docs/stable/distributed.html)

## 与主核 / AI 的关系

并行组的组织方式与主核里「多节点集群的分片组」是同一个问题：**同一批节点要按不同维度被切成多套重叠的分组**。数据库的复制组按「副本」切、分片组按「哈希范围」切，一个节点同时属于两者；vllm 的 TP 组按「层内切分」、PP 组按「层间切分」，一个 GPU 同时属于两者。**共同点是：组的语义决定通信频率，通信频率决定优化手段。**

**双通道（设备 + CPU）vs 主核的控制面与数据面。** 主核的协调走控制面（元数据、心跳）、数据传输走数据面；vllm 同理——张量走 NCCL，对象与状态走 gloo。**任何分布式系统都需要这两条通道，因为它们的消息大小、延迟要求、可靠性要求都不同。** 把两者混在一条通道上，要么小消息被大传输阻塞，要么大传输被控制消息拖慢。

**字符串句柄 vs 数据库的 OID。** vllm 用名字查组，主核用 OID 查对象。前者简单但无命名空间管理（重名只能靠约定），后者有分配与回收纪律但因为「OID 永不复用」而在删除后仍占号。**编译器/序列化限制越强的地方，越倾向用名字；需要跨版本引用时，越倾向用数字。**

**「挂起通信器」vs 主核的「静默实例」。** 主核降级一个实例时把它标成只读、拒绝新事务；vllm 挂起通信器时要求「comms must be idle」并把内存释放。**两者都是「让资源在不用时真的不占资源」，但都要求先进入一个可安全操作的状态**。区别是主核的状态可以靠锁渐进达成，通信器不行——集合通信必须所有 rank 同时空闲，所以它只能是个约定。

对 agent 工程最直接的价值有四条。其一：**多维度并行是常态**，agent 系统也会有「按会话分片 + 按模型副本分组 + 按工具集群分组」这类重叠分组，需要同一套「一个实体属于多个组」的建模。其二：**控制通道与数据通道必须分开**，把工具调用的小消息与模型流式输出挤在同一条链路上就会互相拖累。其三：**共享资源的释放动作要幂等**，agent 里的「中止任务」可能被用户、超时、上游取消同时触发。其四：**故障要建模成数据而不是分支**——`dead_dp_ranks` 这种做法（把死掉的槽位记成一个集合、让 all-reduce 读它）比在每个调用点写 `if failed: ...` 更容易维护。
