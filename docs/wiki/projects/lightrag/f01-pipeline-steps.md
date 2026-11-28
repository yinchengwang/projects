---
project: lightrag
feature: pipeline-steps
title: 流水线步骤与决策
page_type: feature
schema_version: 1
refs:
  - path: lightrag/pipeline.py
    symbols: [PipelineNextStep, PipelineNextDecision, _BatchRunContext]
  - path: lightrag/pipeline.py
    symbols: [_PipelineMixin, _resolve_sidecar_image_path, _rearm_auto_rescan]
---

# 流水线步骤与决策

## 一句话职责

把「处理积压文档」这件事从「一个 `while` 循环」改写成**「决策 → 执行一步 → 再决策」**的状态机：每一步先在一把锁内决定下一步做什么（`PipelineNextStep` 十种取值），再在锁外执行，执行完回来重新决策；而锁内那一步同时承担着「释放忙碌槽」这个原子动作。

## 机制是什么

**这不是一个循环，而是一个反复问同一个问题的驱动器。** 每一轮问的问题是：「现在还有没有待办，且在同一个临界区里决定要不要释放忙碌槽」。答案就是 `PipelineNextStep`。

十种取值可以按「消费了什么」分类：

| 取值 | 消费什么 | 说明 |
|---|---|---|
| `CANCELLED` | **什么都不消费** | 取消标志已在同一锁内设置，绝不落在检查与消费之间 |
| `CONTINUE_MANUAL` | 窥视（不消费） | 最早的粘性手工重试请求 |
| `CONTINUE_SWEEP_PAGE` | 什么都不消费 | 进行中的有界扫描还有下一页 |
| `CONTINUE_AUTO` | **原子消费** | 自动重扫脏标志的唯一消费者 |
| `CONTINUE_DOCUMENT` | 窥视（不消费） | 文档信箱非空 |
| `CONTINUE_DRAIN_WAIT` | 什么都不消费 | 等待冻结前已预留的在途入队完成 |
| `BEGIN_EXCLUSIVE_RESET` | 消费（进入排他相位） | 严格空闲后执行 FAILED → PENDING 重置 |
| `RELEASED` | 释放忙碌槽 | 真的没活了 |

**「消费」与「窥视」的区分是这一页的核心。** 一个被消费的信号只能被处理一次；一个被窥视的信号可以反复看到。选错了就会丢活（消费了却没处理）或者空转（窥视了却永远不处理）。

**而 `RELEASED` 之前的 `pipeline_status.update({"busy": False, "busy_owner": None})` 是唯一在锁内做的写操作** —— 这解决了一个具体的竞态，见下。

## 怎么实现的

### 一、`PipelineNextStep`：每个成员的注释都是一条调度规则

**枚举的注释不是名称解释，而是「这个分支凭什么被选中」：**

```
# A sticky manual retry is pending and the pipeline is IDLE: BEGIN the
# manual protocol (LR2 6.1/7.2) -- freeze new ingress, then drain the AUTO
# backlog to idle. No FAILED is swept here; FAILED is reset only in the
# exclusive phase below. The request stays sticky (ACKed after the reset).
CONTINUE_MANUAL = "continue_manual"
```

```
# The in-progress bounded AUTO sweep has more pages (its keyset cursor is
# not yet CURSOR_END). No wake-up signal is consumed: the run simply
# fetches the next page of the SAME sweep before honouring quiescence
# signals, so a huge PENDING backlog drains page-by-page while cancellation
# and manual retry still preempt at every page boundary (LR2 Phase 2 6.3).
CONTINUE_SWEEP_PAGE = "continue_sweep_page"
```

**第二条注释说明了为什么要分页而不是一次取完**：「巨大的 PENDING 积压按页排空，而取消与手工重试仍然在每个页边界抢占」。**分页在这里不是性能优化，而是抢占点**——不分页的话，一个百万文档的积压会让取消请求等上几十分钟。

**而 `CANCELLED` 的注释说明了它最特殊的地方：**

```
# Cancellation pending: the run must stop WITHOUT consuming any wake-up
# signal -- the ingress is fully retained for the next explicit trigger.
CANCELLED = "cancelled"
```

**「不消费任何唤醒信号」是一条硬约束**：取消之后，所有待办必须原样留给下一次触发。若取消时顺手消费了自动重扫标志，那批待办就再也不会被处理——**因为脏标志已经清了，而没有别的信号会再喊它。**

### 二、`PipelineNextDecision`：把「哪个分支」和「附带信息」分开

```
@dataclass(frozen=True)
class PipelineNextDecision:
    step: PipelineNextStep
    # Set only for CONTINUE_MANUAL: the sticky request this continuation
    # serves; ACKed after the FAILED->PENDING reset persists.
    manual_request_id: str | None = None
    # Set only for CANCELLED: True when the cancellation is an internal-error
    # abort, whose exit path surfaces the actionable halt message.
    internal_error: bool = False
```

**两个字段各自只在一个分支上有值，而注释明写了这个约束。**

**`frozen=True` 是必要的**：一个决策被作出之后就不该被改写。若可变，下游的一步执行可能顺手改掉它——那时「决策与执行分离」这个结构就失效了。

**而 `internal_error` 的存在说明「取消」有两种来源**：外部请求的取消（正常停机）与内部错误触发的取消（需要把可操作的错误消息带出去）。**两者走同一个 `CANCELLED` 分支，但退出路径不同**——这正是「把差异放在数据里而不是放在分支里」的做法。

### 三、`_decide_pipeline_next_step`：优先级是硬编码的顺序

**文档把优先级列成了一条链：**

```
Fixed priority (LR2 6.2): cancellation -> advancement of the CURRENT
manual drain/reset -> earliest (new) manual request -> auto-rescan ->
document mailbox -> release. The lock covers only this decision (mailbox
calls are single in-memory/Manager RPCs; storage queries happen strictly
outside, in _refetch_for_decision).
```

**六项优先级，而每一项的位置都能问出「为什么不在更前面/更后面」。**

**第一，取消在最前且不消费。** 这是唯一无须论证的——停机优先。

**第二，「推进当前的手工排空」优先于「开始新的手工请求」。** 代码里的分叉：

```
if pipeline_status.get("manual_phase", MANUAL_PHASE_IDLE) == MANUAL_PHASE_DRAIN_TO_IDLE:
    # Advance the manual drain we already own (freeze is set). ...
    # Do NOT peek a new manual -- a second request waits FIFO (LR2 7.5).
    if sweep_cursor is not CURSOR_END: return PipelineNextDecision(PipelineNextStep.CONTINUE_SWEEP_PAGE)
    if ingress.consume_auto_rescan(): return PipelineNextDecision(PipelineNextStep.CONTINUE_AUTO)
    if ingress.counts().get("documents"): return PipelineNextDecision(PipelineNextStep.CONTINUE_DOCUMENT)
    if pipeline_status.get("pending_enqueues", 0) > 0: return PipelineNextDecision(PipelineNextStep.CONTINUE_DRAIN_WAIT)
    return PipelineNextDecision(PipelineNextStep.BEGIN_EXCLUSIVE_RESET)
manual_msg = ingress.peek_next_manual_retry()
if manual_msg is not None:
    return PipelineNextDecision(PipelineNextStep.CONTINUE_MANUAL, manual_request_id=manual_msg.request_id)
```

**「正在排空时不再窥视新的手工请求」是一条公平性保证** —— 第二个请求按 FIFO 等第一个 ACK，而不是插队。若允许插队，两个手工重试会互相打断对方的排空，永远到不了 `BEGIN_EXCLUSIVE_RESET`。

**第三，`CONTINUE_SWEEP_PAGE` 在 `IDLE` 相位排在手工重试之后、自动重扫之前。**

```
# The in-progress bounded sweep has more pages: finish draining the KNOWN
# backlog before consuming any quiescence signal. Preempted by cancellation
# and manual retry above ... consumes nothing, so auto-rescan/document stay
# pending for when the cursor reaches CURSOR_END.
if sweep_cursor is not CURSOR_END:
    return PipelineNextDecision(PipelineNextStep.CONTINUE_SWEEP_PAGE)
```

**「先排空已知积压，再消费任何静默信号」** —— 因为静默信号（自动重扫、文档信箱）代表的是**新的**工作，而已知积压是**旧的**。旧的处理完了再接受新的，否则旧的永远排在后面。

**而它「不消费」是有意的**：`auto-rescan` 与 `documents` 保持待处理状态，等游标到达 `CURSOR_END` 时它们仍然在。

**第四，`CONTINUE_AUTO` 是唯一的「原子消费」，而注释强调这一点。**

```
# 3. **Auto rescan** (consumed atomically): sole consumer of the dirty flag;
# re-armed on a failed follow-up query in the refetch.
```

**「唯一的消费者」意味着没有别的地方会清那个脏标志。** 于是「谁清了但没处理」就等价于「丢了那批待办」。**这是把「消费与处理必须成对」的约束集中到一处。**

**而「失败时在 refetch 里重新武装」是配套的**：消费之后若后续查询失败，必须把标志重新置脏，否则那批待办就丢了。

**第五，`RELEASED` 是唯一在锁内写状态的出口。**

```
if ingress.counts().get("documents"): return PipelineNextDecision(PipelineNextStep.CONTINUE_DOCUMENT)
pipeline_status.update({"busy": False, "busy_owner": None})
return PipelineNextDecision(PipelineNextStep.RELEASED)
```

**为什么必须在这个临界区里释放？** 文档开头就写了：

> 关掉了「循环退出交接」竞态：一个在处理循环**走向出口的路上**落下信号（粘性手工请求、自动重扫标志、或一条文档消息）的发布者，会在**同一个**愿意释放 `busy` 的临界区里被观察到，于是循环重新取数而不是把新的工作遗落。

**具体机制**：`busy` 槽是「当前有处理循环在跑」的标志。一个 enqueue 看到 `busy=True` 就只是把文档投进信箱并返回。若循环在检查完信箱之后、释放 `busy` 之前落地了一个 enqueue，那个 enqueue 会看到 `busy=True` 而把活留给「当前的循环」——但那个循环马上就要退出了。

**把「检查信箱」与「释放 busy」放进同一个临界区就消除了这个窗口**：enqueue 要么在检查之前发布（循环看到并继续），要么在释放之后发布（它看到 `busy=False` 并自己启动一个新循环）。**中间没有第三种可能。**

**这是本页最重要的技术点，也是「决策与执行分离」这个结构存在的理由之一。**

### 四、`_BatchRunContext`：一个批次的全部横切状态

**它的文档解释了为什么存在：**

> 把横切句柄（`pipeline_status`、锁、队列、信号量）捆在一起，于是 worker 方法接受单个 `ctx` 参数，而不是约 8 个逐个穿线的参数。

**这是纯粹的参数打包，而它带来一个额外的好处：显式记录哪些状态是「批次级」的。**

**几个字段的注释比字段本身重要得多。**

**`parser_specs` 的不可变性：**

```
# Parse queues are dynamic: one per ParserSpec.queue_group (always at
# least "native"). ``parser_specs`` is the batch snapshot threaded through
# routing + the parse workers so a mid-batch register_parser cannot change
# the engine set for this run.
```

**「批次快照」——于是批次运行期间注册新解析器不会影响正在跑的批次。** 这与 `initialize_storages` 那页的「配置快照」是同一模式（见 f04）：**运行中的工作使用一致快照，新配置下次生效。**

**`inflight_doc_ids` 与 `routing_doc_ids` 两个集合的差别：**

```
#  * ``inflight_doc_ids`` -- every doc_id currently routed into or moving
#    through the queues. The initial batch is pre-registered before the
#    feeder starts; the feeder adds an id right after its parse-queue put
#    succeeds.
#  * ``routing_doc_ids`` -- ids being admitted right now (added before the
#    ``put``, moved to inflight after it with no ``await`` between the two
#    mutations) so the feeder's dedup sees an id the instant admission
#    begins -- there is never a window where an admitting id is in neither
#    set.
```

**「两次修改之间没有 `await`」是这段的关键。** 若中间有一个 `await`，事件循环可能在那一刻切走，于是另一个任务会看到「这个 id 既不在 inflight 也不在 routing」——它会以为这是个新文档于是重复接纳。

**这是一个「用一个额外的集合消除 await 窗口」的经典做法**：不是靠锁，而是靠「让两个修改在同一个同步块里完成」。

**`run_owner_token` 是一个所有权凭证：**

```
# Reservation token of the run that owns ``busy`` for this batch (LR2 7.7
# items 3/4/7). Every worker status write verifies it is still the current
# ``busy_owner`` before touching doc_status, so a run whose ownership was
# reclaimed (dead-process reaper, Manager generation change) discards its
# late results instead of writing a final status over the new owner's work.
# ``None`` disables the check and exists only for call paths that construct
# a context outside a reservation (tests).
```

**它防的是一个具体情形**：进程 A 拿着 `busy` 在跑，被判死（`dead-process reaper`）；进程 B 接管并开始处理同一批文档；此时 A 醒了，正准备写最终状态——**若没有令牌检查，A 会用自己的过期结果覆盖 B 的正确结果**。

**`None` 只用于测试** —— 这个说明很重要，因为它标出了「这条检查在生产路径上永远开启」。

### 五、`_PipelineMixin`：按「知道什么」而不是「做什么」切分

**它的文档说明了 mixin 的边界：**

> 设计为只能与 LightRAG 一起作为基类组合。依赖 LightRAG 提供的属性（`self.full_docs`、`self.doc_status`、`self.tokenizer`、`self.parser_*`、`self.workspace`……）以及留在主类、通过 MRO 解析的共享方法 `self._insert_done` / `self._process_extract_entities`。

**这是一段不寻常的坦白**：mixin 依赖宿主提供属性，且**依赖另外两个仍在主类里的方法**。于是 `_PipelineMixin` 不是一个可独立使用的类，而是一个「用来拆分巨型类」的产物。

**为什么这么切**：`lightrag.py` 有 9902 行，`pipeline.py` 有 8342 行。按「做哪一类事」切成两个文件是唯一可行的拆分——**但切完之后两者的边界不是接口而是「谁持有哪个属性」**。文档把这个代价说清楚了。

**`_rearm_auto_rescan` 是一个两行函数，而它的文档解释了为什么不是一行。**

```
def _rearm_auto_rescan(ingress: Any) -> None:
    """Set the auto-rescan dirty flag and count the re-arm.

    Every re-arm costs a later strict sweep, so the rate explains sweep load that
    no upload accounts for. Counted here rather than inside the ingress because in
    multiprocess mode the mailbox lives in the Manager process, where a counter
    would be invisible to the worker answering /health.
    """
    ingress.request_auto_rescan()
    pipeline_metrics.increment(pipeline_metrics.AUTO_RESCAN_REARMS)
```

**「重新武装的速率解释了没有上传能解释的扫描负载」** —— 这句话点出了这个指标的价值：它不是「有多少次重扫」这么直白，而是「排查『为什么这么闲还在扫』的唯一证据」。

**而「计数放在这里而非 ingress 内部」的理由是跨进程可见性**：多进程模式下信箱活在 Manager 进程里，在那里计数的话，回答 `/health` 的 worker 看不到它。

**这是一个「指标必须放在能被读取的那一侧」的具体例子** —— 位置选择由「谁读它」决定，而不是由「谁产生它」决定。

### 六、`_resolve_sidecar_image_path`：一个把攻击面收在汇点的解析器

**它的文档说明了这个函数为什么存在：**

> `<doc>.drawings.json` 的 `path` 字段是**静置状态下受攻击者影响的数据**：sidecar 可能由旧版本写、由外部引擎写、或从备份恢复，而里面的值源自一个上传的文档。**这个解析器是把那个值变成「一个会被读取并送给 VLM 的文件」的汇点，所以遏制属于这里**，而不是指望每个生产者都行为正确（GHSA）。

**「汇点」这个概念是这段话的核心** —— 安全性应该收在「数据被真正使用的那一处」，而不是「每个可能产生数据的地方」。因为生产者会越来越多（旧版本、外部引擎、备份），而消费点只有一个。

**三个结果状态把「拒绝」与「不存在」分开。**

```
class _SidecarPathOutcome(str, Enum):
    RESOLVED = "resolved"  # a real file, contained to the sidecar dir
    REFUSED = "refused"    # containment / malformed / non-string -- never read
    MISSING = "missing"    # contained but no such file on disk (or a dir)
```

**文档给了这条区分的操作价值：**

> 结果让调用方把真正的「没有这个文件」（`MISSING`）与遏制/格式拒绝（`REFUSED`）分开：一个旧的 sidecar 的 `path` 是绝对的 RAG-Anything/MinerU 值，指向一个**确实存在但在文档目录之外**的文件，把它报告成「未找到」会让运维在排查「消失的 VLM 分析」时被引向错误的原因。

**这是一个「错误消息影响可运维性」的实例**：同一个「拿不到文件」的结果，两种原因的排查方向完全相反——一个是数据问题（文件真丢了）、一个是安全拒绝（文件在那里但被拒绝访问）。

**而遏制本身委托给共享原语。**

> 遏制被委托给 `validate_file_path_security`，那个共享的 `resolve()` + `is_relative_to()` 原语（它会把 `..` 折掉、解析两侧以便 macOS 的符号链接前缀 `/tmp → /private/tmp` 仍然匹配、并把任何畸形路径——内嵌 NUL、符号链接环——变成一个安静的 `None` 而不是会 fail-fast 整个文档的异常）。

**三个细节都属于「路径安全不是 `startswith`」这一类**：

1. **`resolve()` 折掉 `..`** —— 否则 `a/../../etc/passwd` 看起来还在 `a/` 下面。
2. **两侧都解析** —— 否则 `/tmp` 与 `/private/tmp` 在 macOS 上看起来不同。
3. **畸形路径变成 `None` 而不是异常** —— 因为一个坏 sidecar 不该让整个文档处理失败。

第 3 点与「安全拒绝 vs 数据缺失」的三态是一致的：**所有「这个路径不可用」的原因都要能落到三个桶里，而桶的语义决定调用方怎么写日志。**

## 关键取舍

**源码事实：** 流水线是「决策 → 执行一步 → 再决策」的状态机，`PipelineNextStep` 十种取值按「消费了什么」分类：原子消费（`CONTINUE_AUTO`、`BEGIN_EXCLUSIVE_RESET`）、窥视不消费（`CONTINUE_MANUAL`、`CONTINUE_DOCUMENT`）、什么都不消费（`CANCELLED`、`CONTINUE_SWEEP_PAGE`、`CONTINUE_DRAIN_WAIT`）、以及释放（`RELEASED`）。

**源码事实：** `CANCELLED` 明确不消费任何唤醒信号——ingress 完整保留给下一次显式触发；否则被消费的标志会让那批待办永远丢失。

**源码事实：** 固定优先级是「取消 → 推进当前手工排空 → 最早的新手工请求 → 已有扫描的下一页 → 自动重扫 → 文档信箱 → 释放」。正在排空时**不窥视新的手工请求**，第二个按 FIFO 等第一个 ACK（否则两个重试互相打断，永远到不了排空终点）。

**源码事实：** `CONTINUE_SWEEP_PAGE` 排在静默信号之前（「先排空已知积压，再消费新工作」），且它不消费——`auto-rescan` / `documents` 保持待处理直到游标到 `CURSOR_END`。分页的存在意义是**抢占点**：巨量积压按页排空，取消与手工重试仍在每个页边界抢占。

**源码事实：** 释放 `busy` 与「检查信箱为空」在**同一个临界区**内完成，从而关闭「循环退出交接」竞态：并发 enqueue 要么在检查前发布（循环继续），要么在释放后发布（它自己启动新循环），没有中间态。

**源码事实：** `PipelineNextDecision` 是 `frozen=True`，且两个附带字段各只在一个分支上有值（`manual_request_id` 仅 `CONTINUE_MANUAL`、`internal_error` 仅 `CANCELLED`）。

**源码事实：** `_BatchRunContext` 是纯参数打包（把约 8 个横切参数收成一个 `ctx`），但它顺带显式记录了哪些状态是批次级的；`parser_specs` 是批次快照，于是批次中途注册解析器不影响本批次。

**源码事实：** `routing_doc_ids` 在 `put` 之前加入、`put` 之后移入 `inflight_doc_ids`，**两次修改之间没有 `await`**，于是不存在「某个 id 既不在 inflight 也不在 routing」的窗口。

**源码事实：** `run_owner_token` 让每个 worker 的状态写在落盘前验证自己仍是当前 `busy_owner`，于是被判死并已失去所有权的运行会丢弃迟到的结果，而不是覆盖新主人的工作。`None` 只用于测试。

**源码事实：** `_PipelineMixin` 是拆巨型类的产物，依赖宿主提供属性并依赖仍留在主类的两个方法（通过 MRO 解析）——所以它不能独立使用。

**源码事实：** `_rearm_auto_rescan` 把计数放在 pipeline 侧而不是 ingress 内部，因为多进程下信箱在 Manager 进程里，那里的计数对回答 `/health` 的 worker 不可见。

**源码事实：** `_resolve_sidecar_image_path` 把路径遏制收在「数据被使用的汇点」，因为生产者不可穷举（旧版本、外部引擎、备份恢复）而消费者只有一处；三态（`RESOLVED`/`REFUSED`/`MISSING`）把「安全拒绝」与「文件不存在」分开，避免运维被引向错误原因。

**架构推论：** 这个状态机的价值在于**把「信号消费」变成可审计的**。一个 `while` 循环里，信号在哪里被清掉、失败了会不会补回来，读者只能靠推断；而把它拆成十种具名决策之后，每种决策「消费了什么、不消费什么」都写在枚举成员的注释里。**当并发正确性依赖「某个标志只在某处被清」时，把它显式化成枚举比注释更可靠。**

**架构推论：** 「决策与执行分离」带来一个必须付的代价：**决策时看到的状态在执行时可能已过期**。这里的应对是让执行完成后**重新决策**（而不是继续用旧决策往下走），并在决策函数里只读「不会过时的东西」（信箱计数、标志位）。而需要读存储的判断（`_refetch_for_decision`）被严格移到锁外——**锁里只做单次内存 RPC，绝不做存储查询**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/pipeline.py` | `PipelineNextStep` | 十种调度决策，每种的注释即规则 |
| `lightrag/pipeline.py` | `PipelineNextDecision` | 冻结的决策 + 两个分支专属字段 |
| `lightrag/pipeline.py` | `_decide_pipeline_next_step` | 锁内决策，六项固定优先级 |
| `lightrag/pipeline.py` | `_refetch_for_decision` | 锁外取数，与决策严格分离 |
| `lightrag/pipeline.py` | `_BatchRunContext` | 批次级横切状态（含 `run_owner_token`） |
| `lightrag/pipeline.py` | `_PipelineMixin` | 拆巨型类的产物，依赖宿主属性 |
| `lightrag/pipeline.py` | `_run_pipeline_batch` | 三层队列（parse → analyze → process）的装配 |
| `lightrag/pipeline.py` | `_rearm_auto_rescan` | 重新武装脏标志 + 计数（指标位置由读取方决定） |
| `lightrag/pipeline.py` | `_resolve_sidecar_image_path` | 路径遏制的汇点，三态结果 |
| `lightrag/pipeline.py` | `_SidecarPathOutcome` | `RESOLVED` / `REFUSED` / `MISSING` |
| `lightrag/pipeline.py` | `validate_file_path_security` | 共享的 `resolve()` + `is_relative_to()` 原语 |

## 参考

- [lightrag 源码：pipeline.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/pipeline.py)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [lightrag 文档](https://github.com/HKUDS/LightRAG/blob/main/README.md)
- [Python 文档：asyncio 队列与同步原语](https://docs.python.org/3/library/asyncio-sync.html)

## 与主核 / AI 的关系

这个状态机与主核的**后台任务调度器**（checkpointer、WAL writer、autovacuum launcher）同构：都是一把槽位（`busy`）加一组唤醒条件，在锁内决定下一步、在锁外干活。

**「释放槽位与检查待办在同一临界区」这条对应的正是数据库里的经典 latch 交接（latch handoff）。** 在 buffer pool 里，一个进程释放页锁之前必须重新检查等待队列——否则一个刚入队的请求会看到「锁已释放但没人通知我」，然后等一个已经不存在的事件。**lightrag 的 `RELEASED` 分支就是「检查 + 释放」的原子化**，与 latch handoff 是同一个模式。

**十种决策的显式枚举与数据库的 `ProcState` 对比，是一种「把隐式协议显式化」的常见演进。** 早期的后台任务通常是一个 `while (has_work()) { do_one(); }`，等到并发 bug 出现才被迫拆成状态机。lightrag 这里已经把注释写到「第几号决策为什么排在这个位置」，说明这些 bug 已经被踩过一遍。

**`run_owner_token` 与数据库的**事务 ID 围栏（fencing token）**是同一机制：一个持有过期所有权的主体，写任何东西之前先验证自己还有权。分布式存储里这叫 fencing，这里叫 owner token——**机制一样，因为问题一样**：延迟到达的结果会覆盖新主人的工作。

对 agent 工程的直接价值有三条。其一，**「消费」与「窥视」必须显式区分**。一个被消费的信号只能处理一次，所以「消费之后处理失败」必须把标志重新武装；而一个被窥视的信号可以被反复观察到，所以必须有一个确定的地方负责清除它。把这两种操作混在一起，就会得到「丢活」或「空转」。其二，**取消不能消费任何信号**。停机时必须把所有待办原样留给下一次触发——若停机顺手清了脏标志，那批工作就永久遗失了。其三，**安全校验要收在汇点而不是每个起点**。生产者的数量会无限增长（旧代码、外部工具、备份），而消费点只有一个；把遏制放在消费点，才只需要审查一处。
