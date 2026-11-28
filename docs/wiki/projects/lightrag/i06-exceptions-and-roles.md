---
project: lightrag
feature: exceptions-and-roles
title: 异常分类与角色运行时
page_type: feature
schema_version: 1
refs:
  - path: lightrag/exceptions.py
    symbols: [APIStatusError, RateLimitError, EmptyTruncatedResponseError, ReasoningOnlyResponseError]
  - path: lightrag/llm_roles.py
    symbols: [RoleSpec, RoleLLMConfig, _RoleLLMMixin]
---

# 异常分类与角色运行时

## 一句话职责

两个文件解决两类「同一件事的不同分支必须能被区分」的问题：`exceptions.py`（1165 行）用**类型**承载「这一次失败意味着什么」，使调用方能按类型分支而不是解析消息；`llm_roles.py`（732 行）用**注册表**承载「有哪些 LLM 角色」，使加一个角色是一行改动而不是七处硬编码。

## 机制是什么

`exceptions.py` 里的类型不是「按来源分类」（网络错误 / 解析错误 / 存储错误），而是**按「调用方该做什么」分类**。几个例子把这一点说明白了：

| 类型 | 调用方该做什么 |
|---|---|
| `RateLimitError` | 等一等重试 |
| `EmptyTruncatedResponseError` | **不要重试**（同一份预算再买一次同样的失败） |
| `PipelineBackpressureError` | 429，并告诉客户端还有多少空位 |
| `PipelineReservationConflictError` | 409，重试同一个请求 |
| `ReferencesIntactFlushError` | 不必隔离缓存 |
| `CommitBookkeepingError` | **当作写成功了**，只是可见性延迟 |

所以这个文件的本质是一份**处置协议**，而每个类的 docstring 都在回答「收到我之后你应该做什么」。这也是它为什么这么长的原因——不是过度设计，而是「每种可区分的情形都值得一个名字」。

而 `llm_roles.py` 的注册表机制在 `RoleSpec` 的 docstring 里被写成了可检查的承诺：

> 在 LightRAG 任何地方加一个新角色都是**一行改动**：往 `ROLES` 追加一个 `RoleSpec`。其他每个组件（`api/config.py` 里的环境变量循环、队列可观测性、角色配置更新流程）都迭代这个注册表而不是硬编码角色名。

## 怎么实现的

**`APIStatusError` 把「HTTP 响应」这个事实提升为异常状态的一部分，而子类只用 `Literal` 固化状态码。**

```python
class APIStatusError(Exception):
    response: httpx.Response
    status_code: int
    request_id: str | None
    def __init__(self, message, *, response, body):
        super().__init__(message)
        self.request = response.request
        self.body = body
        self.response = response
        self.status_code = response.status_code
        self.request_id = response.headers.get("x-request-id")
```

三个细节：

1. **`request_id` 从 `x-request-id` 头取** ——这是排障时唯一能把「我这边的失败」对上「服务端那条日志」的键。它被提升为字段而不是留在 message 里。
2. **`body` 被显式保存** ——因为服务端的错误详情常常只在 body 里。
3. **`self.request` 从 response 派生** ——调用方不需要传两次。

而八个子类各自只有一行：

```python
class RateLimitError(APIStatusError):
    status_code: Literal[429] = 429  # pyright: ignore[reportIncompatibleVariableOverride]
```

用 `Literal[429]` 而不是 `int = 429` 让**类型检查器**知道「`RateLimitError` 的 `status_code` 恒为 429」，于是调用方可以据此收窄。而 `# pyright: ignore` 注释表明作者知道这是「故意收紧父类的类型」——这是一个正确但会被类型检查器误报的写法。

`APITimeoutError` 是唯一不按状态码分类的（它属于 `APIConnectionError`），因为「超时」在 HTTP 层面可能根本没有响应。它的构造函数也不需要 `response`：

```python
class APITimeoutError(APIConnectionError):
    def __init__(self, request): super().__init__(message="Request timed out.", request=request)
```

所以这个继承体系按「有没有 HTTP 响应」分为两支：有响应的按状态码细分，没响应的按原因细分。

**`EmptyTruncatedResponseError` 的 docstring 解释了「为什么不重试」，而这是这个文件里最有说服力的一段。**

> 两个抛出点共享它：provider 绑定（OpenAI/Gemini/Anthropic），当响应为空且结束原因是输出 token 上限时；以及 `use_llm_func_with_cache`，针对没有绑定能看见的情形：一个思考模型在推理轨迹里耗尽了预算，返回 `<think>...</think>` 而后面没有答案——在绑定自己的检查里是**非空**的，只有在 `remove_think_tags` 之后才可见地空。

> **故意不在任何绑定的重试谓词里**，与那些可重试的响应有效性错误不同：撞上 token 上限是**提示词与配置输出预算的属性**，所以重跑同一个调用只是**再买一次同样满预算的生成，为了以同样方式失败**。什么都没生成而且生成被截断——**没有什么可挽救的**。失败（一次）正是阻止一个空知识图谱被索引并报告为成功的那个东西。

这段包含了三个层次的推理：

1. **为什么重试没用**：输入没变（提示词与预算），所以输出必然一样。
2. **为什么重试有害**：一次满预算的生成是真实成本，而「再买一次同样的失败」是纯浪费。
3. **为什么必须抛**：若不抛，一次空的抽取结果会被写进图里，然后被报告成「成功但没抽到东西」——那是最难发现的错误类型。

第 3 点把「异常的作用」讲清了：**它存在不是为了报告失败，而是为了阻止「成功」被误报**。

而 `EmptyTruncatedResponseError` 与 `ReasoningOnlyResponseError` 的分工也被精确划出：

| 类型 | 触发条件 | 谁抛 |
|---|---|---|
| `EmptyTruncatedResponseError` | token 上限截断导致空 | 绑定层 + `use_llm_func_with_cache` |
| `ReasoningOnlyResponseError` | 有推理轨迹无答案（**非空**字符串） | `reject_reasoning_only_answer` |

而 `ReasoningOnlyResponseError` 的 docstring 点出了为什么它需要单独一个类型：

> 一个后面什么都没有的 `<think>...</think>` 轨迹是一个**非空**字符串，所以它否则会被返回、并被**缓存**，当作答案。

「并被缓存」三个字是关键：一个非空的推理轨迹会被当成有效答案存进 LLM 缓存，于是**此后每次相同查询都命中这条缓存**，错误被永久化。所以这个类型的存在同时保护了「本次返回」与「缓存内容」。

而 `EmptySummaryResponseError` 把它推到第三个场景：摘要。它的 docstring 有一句最狠的：

> 摘要替换它被要求合并的每一段描述，所以存一个空的会**把它们抹掉**。

所以同一个「空响应」在三个位置需要三个类型，因为后果不同：抽取位置 → 空图；查询位置 → 错误答案被缓存；摘要位置 → 已有描述被删除。

**`PipelineBackpressureError` 的 docstring 明确要求调用方**按字段**而不是消息分支。**

```python
def __init__(self, *, current, requested, capacity, reason=""):
    self.current = current; self.requested = requested; self.capacity = capacity
    self.reason = reason or "pipeline document capacity reached"
```

docstring：

> 字段是**故意结构化**的（LR2 §9.1）：一个 SDK 调用方必须能告诉客户端还有多少空位，而不是解析一个字符串，而 API **只把这个错误**映射到 429。一次互斥拒绝（manual freeze / `scanning_exclusive` / `destructive_busy`）是 `PipelineReservationConflict` → 409，而一次存储/控制面故障是 503——**绝不被伪装成「没有容量」**。

最后那句是这一层最重要的纪律：**三种截然不同的状况必须映射到三个不同的 HTTP 状态码**，因为客户端对它们的正确反应完全不同（退避重试 / 立即重试 / 报警）。

而 `PipelineReservationConflictError.recovery_required` 属性把这条纪律编码成代码：

```python
@property
def recovery_required(self) -> bool:
    """True when the workspace is fenced pending recovery (→503), as opposed to a bounded mutual-exclusion window (→409)."""
    return getattr(self.conflict, "value", self.conflict) == "recovery_required"
```

`getattr(self.conflict, "value", self.conflict)` 这个写法在解一个具体问题：`conflict` 可能是枚举成员（用 `.value`）也可能是裸字符串（直接比）。而 docstring 说明了为什么参数类型是 `Any`：

> `conflict` 是 `PipelineReservationConflict` 成员（**保持为纯字符串值的枚举成员，好让 import 这个模块永远不拉进共享存储层**）

所以这是为了打破 import 环而做的类型弱化，而 `getattr(..., "value", ...)` 是对弱化的补偿。

**`AdminWriteGateRefusedError` 用「稳定的消息前缀」代替机器可读的代码。**

```python
ADMIN_WRITE_LOCK_BUSY_PREFIX = "Another knowledge graph edit is in progress"
ADMIN_WRITE_PIPELINE_BUSY_PREFIX = "Pipeline is busy with another operation"
```

docstring：

> 在恰好两种情形下，每种都有自己的**稳定开头短语**，好让一个客户端能仅从 `detail` 文本分辨它们（**文本，而不是机器可读代码，才是契约**）

「文本才是契约」这句很诚实——它承认这里没有结构化字段，而客户端只能靠前缀匹配。而它同时把这个弱点变成了一条显式契约：前缀被提取成常量，所以「稳定」是可保证的（改常量就是改契约）。

两种情形的区别在 `fence` 字段里：

- 锁忙 → `fence = "admin_lock"`，`conflict = None`（**没有 `pipeline_status` 标志拒绝**）
- 管道忙 → `fence` 与 `conflict` 都携带拒绝的 `pipeline_status` 标志

所以 `fence` 的 `None` 本身有含义：「不是由 pipeline_status 拒绝的」。

**`AdminWriteHoldExceededError` 继承 `TimeoutError`，而理由不是分类而是**内置语义**。**

```python
class AdminWriteHoldExceededError(TimeoutError):
```

docstring：「当一次 admin 图写跑过 `admin_write_max_hold_seconds` 并被停止时抛。**在 admin 写持有管道 `busy` 预留期间，它延迟它工作区里每一次管道启动，所以持有是有界的**。过时是一次响亮的失败（通过图路由 HTTP 500），**绝不是一次静默释放**」。

两点：

1. **为什么要有界**：持有 `busy` 会挡住所有摄取，所以无限持有一个 admin 请求就能让整个工作区停摆。
2. **为什么过时要抛而不是静默释放**：静默释放会让调用方以为写成功了，而实际上它被中断了——于是「图里应该有但可能没有」这个不确定性被藏起来了。

**`ReferencesIntactFlushError` 与 `flush_may_have_lost_reference` 是这一页最有意思的一对，因为它把「一个布尔答案」编码成了一个异常类型。**

类的 docstring 先说清问题：

> 回答 `index_done_callback` 的调用方在一次提交抛异常时必须回答的**那一个**问题：*这次抛出可能丢了一个持久引用吗？* 抛这个类型是后端在说**不**——没有任何缓冲的东西被丢弃，也没有任何已持久的被反写。**其他每个异常都意味着『是，假设可能丢了一个』**，那是一个什么都不说的后端保持的答案。

所以这是一个「**默认不安全**」的设计：只有后端**证明**没丢，才能让调用方跳过隔离。而「证明」的标准很高：

> 两种情况合格，而一个后端必须能对**整个** flush 证明其中之一，而不是对单个操作：
>
> - 每个缓冲的操作仍在缓冲且会在下次 flush 重放（批量调用的传输错误）——调用方推迟；
> - 提交本身落地了而只有它之后的一步失败了（一次 refresh）——引用已经持久。
>
> 一次混合了永久与可重试的逐项失败的 flush **两者都不算**：它丢了东西，所以诚实的答案仍然是「是」。

而最后那句解释了为什么值得一个类型：

> 为什么这个区分值一个类型：一行 `extract` 的 LLM 缓存**只能通过它 chunk 的 `llm_cache_list` 到达**，所以一个无法分辨这些的调用方必须为了安全隔离进程里**每一行**缓冲的 extract 记录——丢弃那些不可能被孤儿化的已完成 LLM 调用。

所以「不知道」的代价是**丢掉所有已完成的 LLM 调用**（真实成本：token 钱）。而这个类型让「知道的」后端能避免那个代价。

`flush_may_have_lost_reference` 的实现里有两处细心的处理：

```python
if error is None: return False
# Bounded rather than recursive: __cause__ is attacker-free here, but a self-referential chain must not turn a diagnostic into a hang.
for _ in range(8):
    if not isinstance(error, IndexFlushError): break
    if error.__cause__ is None: return True
    error = error.__cause__
return not isinstance(error, ReferencesIntactFlushError)
```

第一处：**有界循环而不是递归**，理由是「一个自引用的链不能把一次诊断变成一次挂死」。这是防御性的，但写得很明确。

第二处：**只解包 `IndexFlushError`，别的都不解包**：

> `IndexFlushError` 被解包进它的 `__cause__`，因为它是 `LightRAG._flush_storages` 套在后端抛出的东西外面的传输包装；一个没有 cause 的它回答 `True`。**别的一概不解包。** 一个 `from` 一个「完好」异常重抛的异常已经做了它自己的新声明，而 `OpenSearchKVStorage.finalize` 正是那种情形：它把「这些写已经丢了」的 `RuntimeError` 链到一个缓冲完好的 flush 上，**因为它随后释放客户端而没有任何东西会再重放它们**。

这段的逻辑很精确：`from` 链在 Python 里通常表示「因果关系」，但因果不传递「结论」。所以这里只解包「已知的、纯粹是包装的」那一层，其他情况一律按新的声明处理。

**`CommitBookkeepingError` 的 docstring 记录了一次「两个结果不可区分」的 bug。**

```python
"""The offloaded write LANDED; the bookkeeping that had to follow it did not.
...
它存在是因为那两个结果过去无法区分：钩子自己的异常被原样重抛，落进调用点的 except Exception，
并被只有「一次从未发生的写」才成立的推理处理（把内存状态回滚到文件，报告失败）。
每个调用方都继承了那个谎言——utils_graph 里的删除路径跳过了它们仍欠的 chunk 追踪退役，
而那些对象已经持久地消失；而 _insert_done 把一个写已经在磁盘上的文档标成了 FAILED。
```

而它的「处置契约」被写成了两句话：

> 处理契约：**把这次写当作已提交**。记录延迟的可见性……然后继续。**把它报告成一次失败的写是被禁止的；静默吞掉它也是被禁止的。**

「两个禁止」很值得注意：不能报失败（会引发错误的回滚），也不能沉默（运维需要知道有延迟）。所以唯一正确的动作是「记日志并继续」。

而 `result` 字段的存在解决了一个具体的重复劳动：「`result` 携带写调用返回的任何东西，好让一个需要写自己答案的处理方不必重跑它」。

**`llm_roles.py` 的 `RoleSpec` 的三个字段恰好是「三处需要角色名的地方」。**

```python
@dataclass(frozen=True)
class RoleSpec:
    name: str        # 规范小写键，用于 role_llm_configs 与 CLI/日志输出
    env_prefix: str  # API 环境变量层用的大写前缀
    queue_name: str  # 传给 priority_limit_async_func_call 的显示名
```

三个字段对应三种「命名约定」：字典键用小写、环境变量用大写、日志用可读短语。若没有这个 dataclass，这三种约定会散在三个地方并且必然不一致。

四个角色：

```python
ROLES = (RoleSpec("extract", "EXTRACT", "extract LLM func"),
         RoleSpec("keyword", "KEYWORD", "keyword LLM func"),
         RoleSpec("query", "QUERY", "query LLM func"),
         RoleSpec("vlm", "VLM", "vlm LLM func"))
```

三个派生结构都从它算出来：

```python
ROLE_NAMES = frozenset(spec.name for spec in ROLES)
ROLES_BY_NAME = {spec.name: spec for spec in ROLES}
```

注意 `ROLE_NAMES` 是 `frozenset` 用于成员判断（O(1)），`ROLES_BY_NAME` 是 dict 用于取 spec。两者都从同一个元组派生，所以不可能不一致。

**`_ROLE_LLM_TIMEOUT` 这个 `ContextVar` 解决的是「provider 包装器怎么知道当前角色的超时」。**

```python
_ROLE_LLM_TIMEOUT: ContextVar[Any] = ContextVar("lightrag_role_llm_timeout")
_UNSET = object()
```

注释：

> 这个上下文里正在运行的角色调用的有效 LLM 超时（它的 `RoleLLMConfig(timeout=...)`，否则 `default_llm_timeout`）：角色队列的尺寸从它来。角色包装器在它执行的每次调用周围设置它；`apply_llm_timeout` 读它，好让一个打包的 provider 包装器以同样方式尺寸它的客户端，**即使它是通过一个 partial 或一个自定义函数到达的**。

所以问题是：一个 provider 包装器（如 `openai_complete_if_cache`）需要知道「这次调用的超时是多少」来配置它的 HTTP 客户端。而它可能是被 `partial` 包装过的、或直接由用户提供的自定义函数——**没有途径从参数里拿到这个信息**。

解法是用 `ContextVar`：

```python
async def call_with_role_timeout(*args, **kwargs):
    token = _ROLE_LLM_TIMEOUT.set(timeout)
    try:
        return await bound(*args, **kwargs)
    finally:
        _ROLE_LLM_TIMEOUT.reset(token)
```

注释：「在执行的任务内部设置，所以 `apply_llm_timeout` 看到这个角色的超时，无论 provider 包装器坐得多深」。

这是一个「用上下文传递隐式参数」的标准用例，而它的正确性依赖两点：

1. **`reset(token)` 在 `finally` 里** ——所以嵌套调用或异常不会泄漏超时值。
2. **只在角色调用内部有效** ——`apply_llm_timeout` 在 `_UNSET` 时回退到 `global_config["default_llm_timeout"]`。

而 `apply_llm_timeout` 里有一段关于优先级的检查：

```python
if "timeout" in kwargs: return
for key in client_config_keys:
    config = kwargs.get(key)
    if isinstance(config, Mapping) and "timeout" in config: return
```

所以显式传入的 `timeout`（无论是顶层 kwarg 还是某个客户端配置映射里的）**赢过**上下文变量。而 docstring 说明了这是刻意的：「一个已经在 `kwargs` 里的 `timeout`，或任何 `client_config_keys` 命名的 `kwargs[key]` 映射里的一个 `timeout` 键（一个包装器转发的客户端配置），是一次显式选择并胜出」。

第二处检查（`client_config_keys`）的原因很实在：某些 provider 把超时放在嵌套的客户端配置 dict 里（例如 `httpx.Timeout` 的形状），而顶层没有 `timeout` 键。若只看顶层，那个显式配置会被上下文变量覆盖。

**`_wrap_llm_role_func` 里有一处「这个检查是运行时更新的后盾」的注释。**

```python
if max_async < 1:
    raise ValueError(f"max_async for LLM role {role_name!r} must be >= 1 (a non-positive limit dispatches no workers, so calls through this role would never run); got {max_async}")
```

注释：

> 每个角色队列都汇流到这里，这让它成为**运行时配置更新**的检查点（一次被拒的更新会回滚）。构造期的种子（`role_llm_configs`、`{ROLE}_MAX_ASYNC_LLM`）在 `__post_init__` 里更早被按设置的名字拒绝；**这是它们的后盾**。

所以「校验放在最窄的必流经点」+「更早的地方也校验以便给出更好的错误消息」。两层是互补的：

- `__post_init__` 的校验能说「`role_llm_configs['extract'].max_async` 非法」。
- `_wrap_llm_role_func` 的校验能覆盖「运行时热更新」这条不经过 `__post_init__` 的路径。

而错误消息解释了为什么 `max_async < 1` 是致命的：「一个非正的限制不派发任何 worker，所以通过这个角色的调用永远不会运行」——不是「会慢」，是「永远不会完成」。

**`_apply_llm_role_config_update` 的快照/回滚模式，而深拷贝的选择很有针对性。**

```python
snapshot = _RoleLLMState(
    raw_func=state.raw_func,
    kwargs=deepcopy(state.kwargs),
    max_async=state.max_async,
    timeout=state.timeout,
    metadata=deepcopy(state.metadata),
    wrapped=state.wrapped,
)
try:
    ...
except Exception:
    state.raw_func = snapshot.raw_func
    state.kwargs = snapshot.kwargs
    state.max_async = snapshot.max_async
    state.timeout = snapshot.timeout
    state.metadata = snapshot.metadata
    state.wrapped = snapshot.wrapped
    raise
```

五个字段中只有 `kwargs` 与 `metadata` 被 `deepcopy`——因为它们是被**原地修改**的 dict（`state.metadata["binding"] = binding`），而浅拷贝会让快照与状态共享同一个 dict，于是回滚无效。`raw_func` / `max_async` / `timeout` / `wrapped` 是不可变赋值，所以直接存引用就够。

这个区分很精确：**只有会被原地改的东西需要深拷贝**。全都深拷贝是安全的但浪费（`raw_func` 可能不可深拷贝）。

**`_schedule_retired_llm_queue_cleanup` 的降级路径，而它的理由是关于事件循环的所有权。**

```python
try:
    loop = asyncio.get_running_loop()
except RuntimeError:
    # The retired wrapper's queue and worker tasks are tied to the event loop that first used them.
    # Spinning up a fresh loop via asyncio.run would either hang on queue.join() or touch primitives
    # bound to a closed loop. Skip cleanup with a warning -- call aupdate_llm_role_config() from an
    # async context for deterministic shutdown.
    logger.warning("update_llm_role_config: skipping retired LLM queue cleanup because no event loop is running; call aupdate_llm_role_config() from an async context for deterministic shutdown")
    return
```

所以「没有运行中的循环」时**跳过清理并警告**，而不是用 `asyncio.run` 起一个新循环。理由是两个具体的失败模式：

1. **`queue.join()` 会挂死** ——新循环里的 worker 任务是新的，而队列里的项目属于旧循环。
2. **会碰到绑定到已关闭循环的原语** ——`asyncio.Queue` / `Event` 在早期 Python 版本上绑定循环。

而警告里给出了正确用法（用 `aupdate_llm_role_config`），所以这不是「静默跳过」。

**`aupdate_llm_role_config` 的 docstring 给了一个可算的时间上界。**

> 这个等待被退役队列的 `max_task_duration` 限制，它被算作 `llm_timeout * 2 + 15` 秒（默认 `180 * 2 + 15 = 375` 秒，约 6 分 15 秒）。当这个界限到达时，排空超时，关闭落到强制取消：待定的 future 被取消、队列被清空、worker 被停止。**所以这个方法永远不会无限阻塞**，但面对深积压的慢 LLM 调用它可能要花满那个界限才返回，而超过界限的在飞调用会被取消。

这个公式的来源是「一次调用可能用满 `llm_timeout` 的重试两次再加开销」——而它给出的是**可预期的**上界，这对一个会在关闭路径上被调用的方法很重要。

**而同步变体与异步变体的区别被明确写出来了。**

```python
def update_llm_role_config(...) -> None:
    old_wrapped = self._apply_llm_role_config_update(...)
    self._schedule_retired_llm_queue_cleanup(old_wrapped)
```

同步版本把清理**调度成后台任务**并立即返回；异步版本 `await self._shutdown_llm_wrapper(old_wrapped)` 等它完成。而 docstring 给出了第三种用法：

> 如果你需要一个非阻塞的切换，用同步的 `update_llm_role_config()`（它把清理调度成后台任务）并在你想确认旧队列消失时单独 await `wait_for_retired_llm_queues()`。

所以三个 API 覆盖三种需求：非阻塞切换、阻塞切换、切换+稍后确认。

**`_shutdown_llm_and_rerank_queues` 与 `_shutdown_embedding_queue` 的顺序约束是这一页最实用的部分。**

```python
async def _shutdown_llm_and_rerank_queues(self) -> None:
    """... Finalization must call this BEFORE tearing down storages: role LLM calls can still use
    the response cache while their queues drain. The embedding queue is not included, because
    storages need it to finalize; see _shutdown_embedding_queue."""
```

而 `_shutdown_embedding_queue`：

> Finalization must call this AFTER the storages are finalized: a vector storage's `finalize()` flushes its pending upserts, and that flush embeds first. **Closing the queue earlier makes the flush fail and the buffered vectors are dropped.**

> 在一次**被中断的** finalize 之后（从 `finally`、一个 `except`）也不要调它：关闭是永久的，所以重试的 finalize 再也不能 flush 被中断那次留下的缓冲向量。`finalize_storages` 记录了它接受的残留。

所以关闭顺序是：

1. 先关 LLM 与 rerank 队列（此时存储还在，LLM 缓存还能读）。
2. 再 finalize 存储（需要嵌入队列还活着）。
3. 最后关嵌入队列。

而第 3 步之后**不能再 finalize**，因为关闭是永久操作。这是一个「顺序 + 幂等性」的双重约束，而代码用两个独立方法 + docstring 警告来实现，而不是用状态机——这是一个可以商量的取舍，但 docstring 把两个方向的错误都写清了：

- 早了 → flush 失败、缓冲向量丢失。
- 中断后重试 → 再也 flush 不了。

**元数据脱敏用「按标记子串匹配」而不是白名单，而它有一个显式的例外集。**

```python
_SECRET_MARKERS = ("api_key","api-key","apikey","access_key","access-key","secret","token","credential","password","passphrase","pwd","auth","session")
_SAFE_OPTION_KEYS = {"thinking_token_budget"}

@classmethod
def _is_secret_key(cls, key):
    lowered = key.lower()
    if lowered in cls._SAFE_OPTION_KEYS: return False
    return any(marker in lowered for marker in cls._SECRET_MARKERS)
```

「按子串匹配」是**宁可多删**的方向：`api_key`、`x-api-key`、`openai_api_key`、`aws_secret_access_key` 全部命中，而未来出现的新字段名（`vertex_credentials`、`hf_token`）也会被命中。

代价是误伤，而 `_SAFE_OPTION_KEYS` 就是误伤的例外集：

```python
# Non-secret keys whose names collide with a secret marker substring
# (e.g. "thinking_token_budget" contains "token"). Matched globally at
# every nesting level during metadata scrubbing, not only inside
# provider_options.
_SAFE_OPTION_KEYS = {"thinking_token_budget"}
```

所以这里是一个**保守默认 + 显式例外**的结构。而它的方向选择与 `_sddl_access_mask` 那种「认不出就拒绝」是同一条原则的应用：**面对不确定，选安全的那一侧**。

而脱敏是**删除而不是打码**，理由在 `_scrubbed_llm_metadata` 的 docstring 里：

> 认证相关字段被**整个剥掉——不是打码**——因为一个打码的 `"***"` 对外部消费者不携带任何信息（运维已经能看到 `binding` / `host` 来确认一个角色被配置了）。剥掉让不变量变简单：**出现在这个输出里的任何东西都可以安全地记录、缓存、上线传输**。

这条「删除而不是打码」的推理很好：打码保留了「这里有个字段」这个信息，而它对消费者无用；删除则给出一个可陈述的不变量（输出里的内容都安全）。而**不变量**是可以被测试和被依赖的，而「打码但形态未知」不能。

而 `get_llm_role_config` 的 docstring 补了最后一句：「**没有逃生口**；真正需要原始值的运行时组件直接从 `_role_llm_states[role].metadata` 读」。

所以「谁能看到明文」被划成了两部分：内部组件读私有 dict，外部输出走脱敏函数。**没有第三条路**（没有 `include_secrets=True` 参数）——这是刻意的，因为一个这样的参数迟早会被误用。

## 关键取舍

**源码事实：** `exceptions.py` 的类型按「调用方该做什么」分类而不是按来源分类；`APIStatusError` 子类只用 `Literal[NNN]` 固化状态码（配合 `pyright: ignore` 注释），`APITimeoutError` 属 `APIConnectionError` 因为超时可能没有响应。

**源码事实：** `APIStatusError` 把 `request_id`（从 `x-request-id` 头）与 `body` 提升为字段，`request` 从 `response` 派生。

**源码事实：** `EmptyTruncatedResponseError` **故意不在任何绑定的重试谓词里**，因为「撞上 token 上限是提示词与输出预算的属性，重跑只是再买一次同样满预算的生成，为了以同样方式失败」；而抛它「正是阻止一个空知识图谱被索引并报告为成功的那个东西」。

**源码事实：** 「空响应」在三个位置需要三个类型，因为后果不同：抽取位置 → 空图；查询位置 → 非空的 `<think>` 轨迹会被**缓存**为答案（错误永久化）；摘要位置 → 已有的描述片段被**抹掉**。

**源码事实：** `PipelineBackpressureError` 字段刻意结构化，「SDK 调用方必须能告诉客户端还有多少空位，而不是解析一个字符串」；且 API「**只把这个错误**映射到 429」，互斥拒绝是 409，存储故障是 503，「绝不被伪装成没有容量」。

**源码事实：** `PipelineReservationConflictError.recovery_required` 用 `getattr(self.conflict, "value", self.conflict)` 同时接受枚举与裸字符串，因为 `conflict` 参数类型被弱化成 `Any` 以「让 import 这个模块永远不拉进共享存储层」。

**源码事实：** `AdminWriteGateRefusedError` 用两个稳定的消息**前缀常量**区分两种情况，docstring 承认「文本，而不是机器可读代码，才是契约」；`fence` 为 `None` 本身有含义（「不是由 `pipeline_status` 拒绝的」）。

**源码事实：** `AdminWriteHoldExceededError` 存在是因为「admin 写持有 `busy` 预留期间延迟工作区里每一次管道启动，所以持有必须有界」；过时**抛错而不是静默释放**。

**源码事实：** `ReferencesIntactFlushError` 是「默认不安全」的设计：只有后端**对整个 flush** 证明没丢引用才能抛它，混合了永久与可重试逐项失败的 flush **两者都不算**；「不知道」的代价是隔离进程里每一行缓冲 extract 记录，即丢弃已完成的 LLM 调用。

**源码事实：** `flush_may_have_lost_reference` 用**有界循环**（8 次）而不是递归（「一个自引用的链不能把一次诊断变成一次挂死」），且**只解包 `IndexFlushError`**——「一个 `from` 一个完好异常重抛的异常已经做了它自己的新声明」，`OpenSearchKVStorage.finalize` 就是这种情形。

**源码事实：** `CommitBookkeepingError` 存在是因为「两个结果过去无法区分」，而它的契约是双向禁止：「把它报告成一次失败的写是被禁止的；静默吞掉它也是被禁止的」。

**源码事实：** `RoleSpec` 的三个字段对应三种命名约定（字典键小写 / 环境变量大写 / 日志可读），`ROLE_NAMES`（frozenset）与 `ROLES_BY_NAME`（dict）都从 `ROLES` 派生，所以不可能不一致；加一个角色是「一行改动」。

**源码事实：** `_ROLE_LLM_TIMEOUT` 用 `ContextVar` 传递有效超时，因为 provider 包装器可能通过 `partial` 或用户自定义函数被调用，**没有途径从参数拿到这个信息**；`reset(token)` 在 `finally` 里。

**源码事实：** `apply_llm_timeout` 里显式的 `timeout`（顶层 kwarg **或** `client_config_keys` 命名的嵌套映射里的键）**赢过**上下文变量——后者覆盖「超时藏在客户端配置里」的 provider。

**源码事实：** `max_async < 1` 的校验放在 `_wrap_llm_role_func`（每个角色队列必流经点），因为它是**运行时配置更新**的检查点（被拒的更新会回滚），而构造期的种子在 `__post_init__` 更早被按设置名拒绝——「这是它们的后盾」。

**源码事实：** `_apply_llm_role_config_update` 的快照只对 `kwargs` 与 `metadata` 做 `deepcopy`，因为它们会被**原地修改**；`raw_func` / `max_async` / `timeout` / `wrapped` 直接存引用。

**源码事实：** `_schedule_retired_llm_queue_cleanup` 在「没有运行中的事件循环」时**跳过并警告**而不是 `asyncio.run`，因为退役队列的 worker 任务绑定在首次使用它的事件循环上，「起一个新循环要么在 `queue.join()` 上挂死，要么碰到绑定到已关闭循环的原语」。

**源码事实：** `aupdate_llm_role_config` 的等待上界是 `llm_timeout * 2 + 15` 秒（默认 375 秒），到界后强制取消——「这个方法永远不会无限阻塞」。

**源码事实：** 队列关闭顺序：先 LLM + rerank（此时存储还在，LLM 缓存可用）→ finalize 存储（需要嵌入队列还活着）→ 最后关嵌入队列；而**被中断的 finalize 之后不能再调**最后一个，因为「关闭是永久的，重试的 finalize 再也 flush 不了被中断那次留下的缓冲向量」。

**源码事实：** 元数据脱敏用**标记子串匹配**（`_SECRET_MARKERS`）而非白名单，代价用 `_SAFE_OPTION_KEYS` 显式补偿（`thinking_token_budget` 含 `token`）；且是**删除而不是打码**——「打码的 `***` 对外部消费者不携带任何信息」，删除给出可陈述的不变量「输出里的任何东西都安全」。

**源码事实：** `get_llm_role_config` **没有逃生口**（没有 `include_secrets=True`）；内部组件直接读私有 `_role_llm_states[role].metadata`。

**架构推论：** 这个异常模块之所以长，是因为它把「**处置决定**」也当成了接口的一部分。多数代码库的异常只说「哪里错了」，而调用方需要知道的是「我该重试、该放弃、该报警，还是该当作成功」。把后者编码进类型，就能让每个调用点用 `except` 分支表达策略，而不是用 `if "rate limit" in str(e)`。代价是类型数量多，收益是**每个处理分支都是被类型系统检查过的**。

**架构推论：** 「默认不安全 + 需证明才降级」是一条可以推广的纪律。`flush_may_have_lost_reference` 对未知异常返回 `True`（可能丢了），因为「什么都不说的后端保持它今天已有的 fail-safe 隔离」。这与 `ConfigurationStorageError`（「读不出就是启动失败，绝不当作缺失」）是同一条方向的两次应用：**当系统无法确定时，选择那个「代价可见」的分支**。而这两处都有配套的「证明路径」让知道的后端避免那个代价。

**架构推论：** 角色注册表模式（`ROLES` 单一来源 + 三个派生视图）解决的是「同一个概念在多处有不同名字」这个具体问题。`RoleSpec` 把三种命名约定绑在一起，于是「加一个角色」不可能漏掉任何一处。任何「一个实体需要在 N 个地方各有一个标识符」的系统都该做同样的事——**把 N 个名字放在一个 dataclass 里，让注册表成为唯一来源**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/exceptions.py` | `APIStatusError` | 携带 `request_id` 与 `body`，`request` 从响应派生 |
| `lightrag/exceptions.py` | `RateLimitError` | `Literal[429]` 固化，让调用方可收窄 |
| `lightrag/exceptions.py` | `EmptyTruncatedResponseError` | 截断导致的空响应，**不重试** |
| `lightrag/exceptions.py` | `ReasoningOnlyResponseError` | 只有推理轨迹，防它被当作答案缓存 |
| `lightrag/exceptions.py` | `EmptySummaryResponseError` | 空摘要会抹掉被合并的描述片段 |
| `lightrag/exceptions.py` | `PipelineBackpressureError` | 结构化容量拒绝，只映射 429 |
| `lightrag/exceptions.py` | `PipelineReservationConflictError` | 互斥拒绝 409，`recovery_required` 区分 503 |
| `lightrag/exceptions.py` | `AdminWriteGateRefusedError` | 两个稳定前缀区分锁忙与管道忙 |
| `lightrag/exceptions.py` | `AdminWriteHoldExceededError` | 持有有界，过时抛错不静默释放 |
| `lightrag/exceptions.py` | `ReferencesIntactFlushError` | 后端证明「没丢引用」，免于隔离缓存 |
| `lightrag/exceptions.py` | `flush_may_have_lost_reference` | 有界解包，默认回答「可能丢了」 |
| `lightrag/exceptions.py` | `CommitBookkeepingError` | 写已落地、记账失败；禁报失败也禁沉默 |
| `lightrag/exceptions.py` | `ConfigurationStorageError` | 读不到 ≠ 不存在，是启动失败 |
| `lightrag/exceptions.py` | `ConfigurationIdentityError` | 带 `cause` 与 `anchor_path`，按 `cause` 分支 |
| `lightrag/llm_roles.py` | `RoleSpec` | 三个命名约定绑在一起，加角色是一行 |
| `lightrag/llm_roles.py` | `ROLES` | 四个角色的唯一注册表 |
| `lightrag/llm_roles.py` | `RoleLLMConfig` | 按角色的覆盖，`None` 表示继承基础设置 |
| `lightrag/llm_roles.py` | `_RoleLLMState` | 运行时状态，内部使用 |
| `lightrag/llm_roles.py` | `_ROLE_LLM_TIMEOUT` | 用 `ContextVar` 传有效超时给深层包装器 |
| `lightrag/llm_roles.py` | `apply_llm_timeout` | 填 `timeout`，显式配置优先 |
| `lightrag/llm_roles.py` | `_RoleLLMMixin` | 角色运行时：构建、重建、热更新、关停 |
| `lightrag/llm_roles.py` | `_wrap_llm_role_func` | 每个角色队列的必流经校验点 |
| `lightrag/llm_roles.py` | `_apply_llm_role_config_update` | 快照/回滚，只深拷贝会被原地改的字段 |
| `lightrag/llm_roles.py` | `_shutdown_embedding_queue` | 必须在存储 finalize **之后**，且不可重试 |
| `lightrag/llm_roles.py` | `_scrubbed_llm_metadata` | 删除而非打码，无逃生口 |

## 参考

- [lightrag 源码：exceptions.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/exceptions.py)
- [lightrag 源码：llm_roles.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/llm_roles.py)
- [Python 文档：contextvars](https://docs.python.org/3/library/contextvars.html)
- [Python 文档：异常链](https://docs.python.org/3/reference/simple_stmts.html#the-raise-statement)

## 与主核 / AI 的关系

主核里最接近的是**SQLSTATE 错误码**：一个五字符码把「什么错了」与「该怎么做」编码在一起，而客户端库按类（`23xxx` 完整性约束、`40xxx` 事务回滚、`53xxx` 资源不足）分支。这一页的异常体系是同一件事在 Python 里的形态——用类型层次而不是字符串前缀。

**差别在于「处置」的粒度。** SQLSTATE 的处置主要由标准规定（重试 / 回滚 / 放弃），而这里的处置是本项目自定的，所以每个类都必须写清 docstring。这也是为什么这个文件有 1165 行——**它同时是规范文档**。

与 g01（JSON 键值存储）/ g03（向量存储）的关系：那两页反复出现的 `CommitBookkeepingError` 处理、`CorruptStorageSnapshotError`、`ReferencesIntactFlushError` 都是这个文件定义的类型。所以这一页是那一组页面的**词汇表**：存储层讲「什么时候抛」，这里讲「抛的那个类型意味着什么」。

与 i05（API 安全）的关系：`PipelineBackpressureError` → 429、`PipelineReservationConflictError` → 409、存储故障 → 503 的映射表是 API 层从这些类型派生出来的。所以「HTTP 状态码的语义」在这里被定义，而不是在路由里。

对 AI 工程最实用的一条是 **`EmptyTruncatedResponseError` 那个「不重试」的判断**。多数重试逻辑把「空响应」当成暂时性故障，而实际上「输出被 token 上限截断」是**输入与预算的性质**——重试必然同样失败，只是再花一次钱。判据是：**这次失败的原因在输入里还是环境里？** 在输入里的话，重试无用。而这条判断值得显式编码成异常类型，否则它会散在每个调用点的 `if` 里并且被写错。

另一条是 `ReasoningOnlyResponseError` 那个「非空但无用」的陷阱。思考模型返回 `<think>...</think>` 而后面没有答案时，字符串非空、绑定层检查通过、然后它被**缓存**为答案。这类「格式合法但语义为空」的响应在任何支持推理轨迹的模型上都会出现，而检测点必须在「缓存之前」。做法是先剥标记再看是否为空，而不是相信「非空即可用」。

最后一条是角色注册表。任何「一个实体在多个子系统里各有一个名字」的设计都该把名字集中到一个 frozen dataclass，并让所有子系统迭代注册表。否则加一个实体就要改 N 处，而漏掉一处的症状通常是「配置了但不生效」——最难查的一类。
