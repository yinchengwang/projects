---
project: goose
feature: state-machine
title: 状态机式智能体循环
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/agents/state_machine_builder.rs
    symbols: [typesafe_provider]
  - path: crates/goose/src/agents/active_run.rs
    symbols: [ActiveRun, SessionRunState]
---

# 状态机式智能体循环

## 一句话职责

goose 的 agent 循环不是「模型 → 工具 → 模型」的 `while` 循环，而是一台**按固定顺序反复尝试的操作流水线**：每次 `step()` 从第一个操作开始逐个问「你要不要应用」，第一个说「要」的就应用它产生的效果并结束这一步，全都不要则 `Yielded`、需要继续则回到开头——`state_machine_builder` 负责把二十多个操作按顺序装进去，`active_run` 负责保证同一个会话同时只有一个这样的循环在跑。

## 机制是什么

`oi_kit_agent::machine` 的模块 docstring 把这个模型讲清了：

> 在一份持久化的会话状态之上运行一条有序、可重入的流水线。调用方持久化入站消息、用自己的操作构造 `Step`，并选择调 `StateMachine::step`、`StateMachine::apply` 还是 `StateMachine::run`。

三句话各对应一个核心概念：

- **有序**：`Step` 是一个 `Vec`，顺序是语义的一部分。
- **可重入**：每一步都重新加载会话（`runtime.load(session_id)`），从第一步重来；没有跨步的「当前操作指针」。
- **持久化在前**：`step()` 只产出 `effects`，`apply()` 把它们落到存储里。两者分离，于是「算」与「写」可以分别测试与重放。

`Step` 只有两种形态：

```
pub enum Step<'a, S, E = ConversationEffect> {
    Operation(Arc<dyn Operation<S, E> + 'a>),
    Inference(Arc<dyn Inference<S, E> + 'a>),
}
```

`Inference` 也被当作「一种操作」来处理（`Step::operation()` 对它调用 `inference.as_ref()` 转成 `&dyn Operation`），所以流水线里的每一步都能回答 `name()`、`commands()`、`prompt_parts()`。这是它能统一遍历的前提。

`state_machine_builder` 里的 `operations` 是一个 `Vec<Arc<dyn Operation<Session, GooseEffect>>>`，按注释所述顺序大致是：

1. **Bang 命令**（以 `!` 开头的 shell 直通，永不到模型）；
2. **上下文相关性判定 + 压缩**（`manages_own_context` 为真的 provider 跳过）；
3. **最终输出、工具审批、技能、配方、平台扩展、子目录提示、MCP 提示、过期工具、扩展、工具延迟、未处理工具调用、子智能体、steer、工具对压缩、doctor、项目、重试、空响应、思考恢复、Stop 钩子、错误退出、最大轮次、状态**；
4. 最后是 `Step::Inference`。

有两个位置被源码注释点名是有意为之的：

- **压缩排在所有「对助手回复有反应」的操作之前**，因为它要「在工具执行、steering 或轮次结束逻辑看到它之前收获带内摘要」。
- **Bang 命令在最前**，因为「bang-shell 轮次永不到达模型」。

## 怎么实现的

**`step()` 的核心是一个 `for` 加三种结局。**

```
for (index, step) in self.steps.iter().enumerate() {
    let name = step.operation().name();
    if self.cancel.is_cancelled() { return Ok(empty_result(RunStatus::Cancelled)); }
    let result = tokio::select! { biased; result = execution => Some(result), _ = self.cancel.cancelled() => None };
    ...
    match result {
        OperationResult::NotApplicable => {}
        OperationResult::Applied(mut result) => { ...; return Ok(result); }
    }
}
Ok(empty_result(RunStatus::Yielded))
```

三个结局正好对应 `OperationResult` 的两态加上「没有一个应用」：

| 结果 | 含义 |
|---|---|
| `NotApplicable` | 这一步管不着，继续下一个 |
| `Applied(effects)` | 我处理了；写回效果，本步结束 |
| 循环走完 | `Yielded`——没有操作认领，交给调用方 |

`applied_step = Some(name)` 这一行让效果知道自己是哪个操作产生的——日志与测试都能定位。`for effect in &mut result.effects { effect.ensure_message_ids(); }` 则在返回前给每个效果的消息补齐了 id。这两件事都放在**返回前**统一做，意味着每个操作都不必自己操心。

**取消是通过两处检查加一次 `select` 实现的，而不是只靠其中一种。**

```
if self.cancel.is_cancelled() { return Ok(empty_result(RunStatus::Cancelled)); }   // 循环头
let result = match result {
    Some(Ok(result)) => result,
    Some(Err(error)) if !self.cancel.is_cancelled() => return Err(error),
    _ => { *self.interrupted_step.lock().unwrap() = Some(index); return Ok(empty_result(RunStatus::Cancelled)); }
};
```

第二处的第二个分支尤其精细：一个操作返回了错误，但**如果此刻已经被取消，就不把这个错误报出去**——因为取消期间错误是取消的副产物，报出来会掩盖真实原因。而 `interrupted_step` 记下了「取消发生在第几步」，供 `finalize` 用。

`tokio::select! { biased; ... }` 的 `biased` 让「操作完成」优先于「取消触发」——一个已经跑完的操作不被取消打断，于是它产生的效果不会被丢掉。

**`finalize` 是取消路径的收尾，它做三件事。**

```
pub async fn finalize<R>(&self, runtime: &R, session_id: &str, emit: &Emitter) -> Result<S> {
    let mut session = runtime.load(session_id).await?;
    if !self.cancel.is_cancelled() { return Ok(session); }

    let interrupted_step = self.interrupted_step.lock().unwrap().take();
    if let Some(index) = interrupted_step {
        session = self.finalize_step_cancellation(runtime, session_id, session, &self.steps[index], emit).await?;
    }

    let interrupted = session.conversation()
        .and_then(|conversation| messages_since_kickoff(conversation).ok())
        .and_then(interrupted_tool_responses);
    if let Some(tool_responses) = interrupted { ... }

    for (index, step) in self.steps.iter().enumerate() {
        if Some(index) != interrupted_step {
            session = self.finalize_step_cancellation(runtime, session_id, session, step, emit).await?;
        }
    }
    Ok(session)
}
```

1. **被中断的那一步先收尾**（它可能持有部分状态）。
2. **悬挂的工具调用补上「已中断」响应**：`interrupted_tool_responses` 遍历 `pending_tool_requests(messages)`，为每个未完成的调用造一条 `"Tool call was interrupted before completing"` 的工具结果。这一步是必要的——对话历史里一个没有结果的工具调用会让下一次请求被 provider 拒绝，而取消恰恰最容易留下这种半截状态。
3. **所有其它步骤也各收尾一次**（跳过已经处理过的那一步）。

这段代码体现了一个重要判断：**取消不是「什么都不做」**。一个被取消的轮次仍然要把持久化状态修成一个合法的中间态，否则下一次 `reply` 会面对一份坏历史。

**`run()` 是「步进 + 应用」的外层循环。**

```
loop {
    let session = runtime.load(session_id).await?;
    let mut result = self.step(&session, emit).await?;
    self.apply(runtime, &session, &mut result.effects, emit).await?;
    match result.status {
        RunStatus::Continuing => {}
        RunStatus::Yielded | RunStatus::Cancelled => break,
    }
}
self.finalize(runtime, session_id, emit).await
```

`Continuing` 是第三种状态：没有操作认领，但**必须再跑一轮**（因为某个操作已经改了状态，需要从第一步重新评估）。`Yielded` 才是「交回控制权」。这个区分避免了两类错误：把 `Continuing` 当成 `Yielded`（丢掉后续工作），或把 `Yielded` 当成 `Continuing`（死循环）。

`collect_prompt_contributions` 与 `collect_inference_input` 是这条流水线的另一个用途：**同一个操作列表既用于执行，也用于组装请求**。`collect_prompt_contributions` 遍历所有操作收集 `inference_tools` 与 `prompt_parts`，并检查工具名重复：

```
if !tool_names.insert(tool.name.to_string()) {
    anyhow::bail!("multiple operations registered tool '{}'", tool.name);
}
```

这是一个在**组装期**就抓住的重名错误——比运行时发现「同名工具生效了谁」要好得多。`collect_inference_input` 在它的基础上再收集 `moim_parts`，也就是「轮次上下文」那一组片段。

docstring 特意说明 `collect_prompt_contributions` 「从会话与对话推导，所以在轮次之外调用也是安全的，可用于预览请求」。于是「预览即将发送的请求」与「真正发送」走的是同一份代码，不会漂移。

**`state_machine_builder` 里最特别的一个符号是 `typesafe_provider`。**

```
fn typesafe_provider(config: &Config) -> anyhow::Result<Arc<dyn DecisionProvider>> {
    let key = config.get_secret::<String>("TYPESAFE_API_KEY")?.trim().to_string();
    anyhow::ensure!(!key.is_empty(), "TYPESAFE_API_KEY is empty");
    let tls = crate::config::tls::provider_tls_config_from_config(config)?;
    let client = ApiClient::with_timeout_and_tls(
        TYPESAFE_DEFAULT_HOST.to_string(), AuthMethod::BearerToken(key),
        ContextRelevanceOperation::timeout(), tls, 
    )?;
    Ok(Arc::new(TypeSafeProvider::new(client)))
}
```

它造的是一个 `DecisionProvider`——用来判定「哪段上下文相关」的外部服务。三条约束值得记：key 必须非空（`ensure!` 而不是让一个空 bearer 发出去）；TLS 配置来自全局 config（与企业环境一致）；**超时取自 `ContextRelevanceOperation::timeout()`**，也就是「用操作自己的超时设置构造客户端」，于是两者不会失配。

调用侧是 `context_relevance_provider`，它的三道门很值得读：

```
pub(crate) fn context_relevance_provider(config: &Config, provider: &dyn Provider) -> Option<Arc<dyn DecisionProvider>> {
    if provider.manages_own_context() || !config.get_param::<bool>("GOOSE_CONTEXT_RELEVANCE_SHADOW").unwrap_or(false) {
        return None;
    }
    Some(typesafe_provider(config).unwrap_or_else(|error| {
        tracing::error!("context relevance shadow is enabled but TypeSafe setup failed: {error}");
        Arc::new(UnavailableDecisions(error.to_string()))
    }))
}
```

- **自己管上下文的 provider 直接跳过**，docstring 给了理由：「它们从不运行那个操作，所以也不该触发一次密钥查询」。
- 实验开关默认关（`unwrap_or(false)`）。
- **构造失败不返回 `None`，而是返回一个 `UnavailableDecisions`**。docstring 解释：「在一次显式 opt-in 之后，setup 失败会被记录并为每条观测打上标记，而不是悄悄把实验关掉」。这是「已选择加入的实验出故障时应当可见」的正确处理——悄悄退化等于让实验数据变得不可信而无人知晓。

**`StateMachineConfig` 是这一层的输入契约，二十多个字段。**

它把「运行一次流水线需要知道的一切」平铺出来：provider 与 model config、上下文上限与压缩阈值、工具调用截断、是否延迟工具与总是加载的工具名、会话管理器、服务、工具快照、审查器、钩子管理器、steer 队列、子智能体运行器、两个超时、stop 钩子的阻塞上限、最大轮次、额外操作、取消令牌、可选的决策 provider。

其中 `extra_operations` 是唯一的「可以由外部追加」的入口，且**追加在末尾**（`operations.extend(extra_operations)`）——排在 `StatusOperation` 之后、`Inference` 之前。这个位置让扩展操作能反应于内置操作都不认领的情形，但不会抢在内置逻辑之前。

`max_concurrency` 的默认值也是从配置读的，且用了 `NonZeroUsize`：

```
.with_max_concurrency(Config::global().get_param::<NonZeroUsize>("GOOSE_MAX_CONCURRENT_SUBAGENTS").unwrap_or(NonZeroUsize::new(5).unwrap()))
```

用 `NonZeroUsize` 把「0 并发」变成类型上不可能——一个配置成 0 的信号量会永远阻塞。

**slash 命令的插入是「重排」而不是「新增」。**

```
if crate::agents::execute_commands::slash_commands_enabled() {
    let mut command_operations: Vec<_> = operations.iter().filter(|op| !Arc::ptr_eq(op, &skills)).cloned().collect();
    command_operations.push(Arc::clone(&skills));
    steps.push(Step::Operation(Arc::new(SlashCommandOperation::new(command_operations)));
}
steps.extend(operations.into_iter().map(Step::Operation));
steps.push(Step::Inference(inference));
```

它把「除技能外的全部操作」加「技能（移到最后）」包成一个子流水线放进第一步。于是 slash 命令解析能看见除技能外的所有命令，而技能排在最后意味着 slash 命令优先于「把输入当技能调用」。`Arc::ptr_eq` 用来排除技能这一项——因为技能是唯一被特意挪位的。

**`ActiveRunRegistry` 守的是「一个会话同时只有一个轮次」。**

```
struct ActiveRun { run_id: String, cancel_token: CancellationToken }
struct SessionRunState { agent_run: Option<ActiveRun>, live_active: bool }
```

`SessionRunState` 的两个字段表达两种「占用」：一个正常的 agent 轮次，或一个 live 语音交互。`start_prompt_run` 的拒绝条件是这两者任一已存在：

```
if let Some(agent_run) = &state.agent_run { return Err(StartRunError::AgentRunExists { run_id: agent_run.run_id.clone() }); }
if state.live_active { return Err(StartRunError::LiveVoiceInteractionExists); }
```

而 `start_live_delegation` 更特殊——它要求 live 已经活跃（`LiveVoiceInteractionMissing`），且**只允许在 live 期间挂上一个 agent 轮次**。这是「语音交互把工作委托给 agent」那条路径。

`remove_agent_run` 用了 `is_none_or` 的守卫：

```
if state.agent_run.as_ref().is_none_or(|run| run.run_id != run_id) { return false; }
state.agent_run = None;
if !state.live_active { runs.remove(session_id); }
```

比对 `run_id` 而不是无条件清空，是因为可能有「旧的轮次结束得比新的晚」这种情况——只清自己那一份，并把返回值定义成「这个会话的轮次曾是 `run_id` 且现在被移除了」。docstring 正是这么写的。

`finish_live` 里还有一处跨模块的调用：

```
drop(runs);
if let Some(services) = self.services.get().and_then(Weak::upgrade) {
    if let Ok(handle) = tokio::runtime::Handle::try_current() {
        handle.spawn(async move { services.trim_sessions().await });
    }
}
```

它先 `drop` 锁再 spawn，避免在持锁期间回调进 `SessionRuntime`（那会走另一把锁）。`services` 存的是 `Weak<StateMachineServices>`，而 `set_services` 用 `OnceLock` 且只能设一次（第二次 `set` 会 panic）。用弱引用的理由很直白：注册表挂在服务里，服务又持有注册表，强引用会成环。

`is_active` 只查「这张表里有没有这个键」，于是它同时覆盖了「有 agent 轮次」与「live 活跃」两种占用——这也正是 `SessionRuntime::is_busy` 用它的原因。

## 关键取舍

**源码事实：** 流水线是「有序、可重入」的：每次 `step()` 都从头遍历，第一个返回 `Applied` 的操作结束本步；全不认领则 `Yielded`；`run()` 遇到 `Continuing` 继续、遇到 `Yielded`/`Cancelled` 停止。

**源码事实：** 压缩被排在所有「对助手回复有反应」的操作之前，目的是在工具执行、steering 与轮次结束逻辑看到带内摘要之前先收获它；bang 命令排在最前因为它永不到达模型。

**源码事实：** 取消有两处检查加一次 `biased` 的 `select`；操作返回错误时若已取消则不报错误，只记 `interrupted_step`。

**源码事实：** `finalize` 在取消后补一条 `"Tool call was interrupted before completing"` 给每个未完成的工具调用，避免留下会让下次请求被拒的半截历史。

**源码事实：** 工具重名在**组装期**就报错（`collect_prompt_contributions` 里 `bail!`），而不是运行时。

**源码事实：** `typesafe_provider` 的客户端超时取自 `ContextRelevanceOperation::timeout()`；`context_relevance_provider` 在自己管上下文的 provider 上跳过、实验开关默认关、构造失败降级成 `UnavailableDecisions` 而非静默关闭。

**源码事实：** `extra_operations` 追加在 `StatusOperation` 之后、`Inference` 之前，是唯一的外部扩展位。

**源码事实：** `ActiveRunRegistry` 用 `run_id` 比对来移除轮次，返回「曾是它且已移除」；`services` 用 `Weak` 避免环，且 `OnceLock` 只能设一次。

**架构推论：** 「操作列表 + 每步重来」比「有状态的循环机」更容易扩展：加一个能力就是往 `Vec` 里插一个操作，不需要在主循环里加分支。代价是顺序本身成了最脆弱的部分——源码不得不用注释解释「压缩为什么必须在前」「bang 为什么必须最前」。这类顺序依赖无法用类型表达。

**架构推论：** 把「算效果」与「写效果」分开，使同一个 `step()` 能在测试里跑而不碰存储，也让 `collect_prompt_contributions` 复用同一批操作做预览。这是「同一份逻辑两种用途」的标准做法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/agents/state_machine_builder.rs` | `build_state_machine` | 按顺序装配二十多个操作与最后一个 inference 步 |
| `crates/goose/src/agents/state_machine_builder.rs` | `StateMachineConfig` | 运行一次流水线所需的全部输入，含唯一扩展位 `extra_operations` |
| `crates/goose/src/agents/state_machine_builder.rs` | `typesafe_provider` | 构造上下文相关性决策客户端，超时取自操作自身 |
| `crates/goose/src/agents/state_machine_builder.rs` | `context_relevance_provider` | 三道门 + 失败降级为 `UnavailableDecisions` |
| `crates/oi-kit-agent/src/machine.rs` | `StateMachine::step` | 有序遍历、三种结局、取消处理与 `interrupted_step` |
| `crates/oi-kit-agent/src/machine.rs` | `StateMachine::run` / `finalize` | 步进循环与取消后的补偿收尾 |
| `crates/oi-kit-agent/src/machine.rs` | `collect_prompt_contributions` | 组装期收集工具与提示片段，并在此处拒绝重名 |
| `crates/goose/src/agents/active_run.rs` | `ActiveRun` | 一个轮次的身份与取消令牌 |
| `crates/goose/src/agents/active_run.rs` | `SessionRunState` | 每会话的占用状态：agent 轮次或 live 交互 |
| `crates/goose/src/agents/active_run.rs` | `remove_agent_run` | 按 `run_id` 比对移除，返回「曾是它且已移除」 |

## 参考

- [goose 源码：agents/state_machine_builder.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/state_machine_builder.rs)
- [goose 源码：agents/active_run.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/active_run.rs)
- [goose 源码：oi-kit-agent 的 machine.rs](https://github.com/aaif-goose/goose/blob/main/crates/oi-kit-agent/src/machine.rs)
- [goose 文档：架构说明](https://block.github.io/goose/docs/)

## 与主核 / AI 的关系

「有序操作流水线 + 每步重来」在主核里对应的是查询执行的阶段序列（解析 → 绑定 → 优化 → 执行 → 收尾），区别在于主核通常用「一个带状态的执行器」而不是「每次从头重算」。这里选择后者是因为**每一步都可能改变会话状态，而改变之后需要重新评估前置条件**——agent 循环里「刚加了一条消息」会让「是否要压缩」「是否该投递」全部需要重算。

对 AI 工程的实际意义有三条。第一，**取消必须留下合法状态**。`finalize` 为每个未完成的工具调用补一条中断响应，这件事看起来像补丁，其实是硬性要求：对话历史里悬空的工具调用会让下一次请求被 provider 拒绝。任何「可以被中断、且中断后还要继续用同一份历史」的系统，都要有这一步。

**第二，顺序依赖要用注释显式化**。「压缩必须在工具执行之前」「bang 命令必须在最前」这两条都无法由类型系统保证，只能靠注释与测试。当你在这样的流水线里加操作时，第一个问题不该是「加在哪」，而是「它相对这些有注释的位置关系如何」。

**第三，实验性功能失败时应当可见地降级**。`context_relevance_provider` 在构造失败时返回 `UnavailableDecisions` 而不是 `None`，于是「开关打开了但没工作」这件事会被记录到每条观测上而不是消失。这是一个通用的选择：**silent off 与 visible degrade 的区别，在于你事后能不能知道实验到底跑了没跑。**
