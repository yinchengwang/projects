---
project: dify
feature: async-tasks
title: 异步任务与工作流执行任务
page_type: feature
schema_version: 1
refs:
  - path: api/tasks/app_generate/workflow_execute_task.py
    symbols: [_publish_failed_workflow_terminal_events, _resolve_user_for_run]
  - path: api/tasks/async_workflow_tasks.py
    symbols: [execute_workflow_professional, resume_workflow_execution]
---

# 异步任务与工作流执行任务

## 一句话职责

两个文件都是 Celery 任务入口：`workflow_execute_task.py` 负责「一次应用执行」——它**把用户身份降级成可序列化的 id、在 worker 里重建 Flask 上下文、并在任何提前失败时补发一对合成的终止事件**；`async_workflow_tasks.py` 负责「恢复一条暂停的工作流」——它从数据库的暂停记录里读回执行上下文并接着跑。

## 机制是什么

Celery worker 与 Web 进程之间只有一条通道：**序列化后的任务参数**。这条通道有三个硬约束，而这两个文件里几乎每一处设计都在应对它们：

1. **参数必须是可序列化的纯数据。** 一个 `Account` ORM 对象带着 session 引用，无法传。于是有 `_Account` / `_EndUser` 两个只含 id 的 Pydantic 模型，加一个带判别器的联合类型：

```python
type User = Annotated[
    (Annotated[_Account, Tag(_UserType.ACCOUNT)] | Annotated[_EndUser, Tag(_UserType.END_USER)]),
    Discriminator(_get_user_type_descriminator),
]
```

2. **worker 里没有请求上下文。** 而应用执行器（`WorkflowAppGenerator` 等）到处要用 `current_app`、`g` 之类。于是 `_AppRunner` 有一个自造的上下文管理器：

```python
@contextlib.contextmanager
def _setup_flask_context(self, user: Account | EndUser):
    flask_app = current_app._get_current_object()  # type: ignore
    with flask_app.app_context():
        set_login_user(user)
        yield
```

3. **SSE 消费者在另一个进程里等事件。** 于是 worker 必须保证「无论如何都会有一个终止事件」，否则消费者的状态机永远挂着。这就产生了本页最有意思的函数 `_publish_failed_workflow_terminal_events`。

## 怎么实现的

**用户身份的降级是「用判别器联合 + 显式转换函数」完成的，而判别器要同时接受对象与字典两种形态。**

```python
def _get_user_type_descriminator(value: Any):
    match value:
        case _Account() | _EndUser():
            return value.TYPE
        case dict():
            user_type_str = value.get("TYPE")
            if user_type_str is None:
                return None
            try:
                user_type = _UserType(user_type_str)
            except ValueError:
                return None
            return user_type
        case _:
            # return None if the discriminator value isn't found
            return None
```

**四处：**

1. **两种输入形态都要支持** ——Pydantic 在**序列化再反序列化**时传的是 `dict`，而在**直接构造**（`AppExecutionParams.new`）时传的是对象实例。判别器必须同时处理。
2. **函数名里 `descriminator` 是拼错的**（正确拼写 `discriminator`）。**这是一个无害但会误导的拼写错误**，而它被 `Discriminator(...)` 引用，所以改名要改两处。
3. **三处 `return None`** ——判别器拿不到类型时返回 `None`，Pydantic 会报「无法确定联合成员」。**这是一个正确的失败方向**（拒绝而不是猜）。
4. **`_UserType` 是 `StrEnum`**，于是 `value.TYPE` 在 JSON 里就是 `"account"` / `"end_user"` 字符串。

**而转换是单向且显式的：**

```python
user_params: _Account | _EndUser
match user:
    case Account():
        user_params = _Account(user_id=user.id)
    case EndUser():
        user_params = _EndUser(end_user_id=user.id)
    case _:
        raise AssertionError("this statement should be unreachable.")
```

**「只传 id」这个决定有一个直接后果：worker 必须重新查库拿用户。** 而在两个文件里，这个「重新查」有两种实现——这正是本页要对比的地方。

**`_resolve_user`（`_AppRunner` 的方法）与 `_resolve_user_for_run`（模块函数）解决同一个问题，但取的用户来源不同。**

```python
def _resolve_user(self) -> Account | EndUser:
    user_params = self._exec_params.user
    match user_params:
        case _EndUser():
            with self._session() as session:
                return session.get(EndUser, user_params.end_user_id)
        case _Account():
            with self._session() as session:
                user: Account = session.get(Account, user_params.user_id)
            return user
        case _:
            raise AssertionError(f"user should only be _Account or _EndUser, got {type(user_params)}")
```

```python
def _resolve_user_for_run(session: Session, workflow_run: WorkflowRun) -> Account | EndUser | None:
    role = CreatorUserRole(workflow_run.created_by_role)
    if role == CreatorUserRole.ACCOUNT:
        return session.get(Account, workflow_run.created_by)

    return session.get(EndUser, workflow_run.created_by)
```

**对比出四处差异：**

1. **前者从**任务参数**取 id，后者从**数据库记录**取 id** ——因为恢复路径拿到的只有 `workflow_run`，原始参数早已丢失。
2. **前者查不到时返回 `None` 且不报错**（`session.get` 的语义），**类型标注却不含 `None`** ——所以调用方（`_setup_flask_context(user)`）可能拿到 `None` 然后在一个要求 `Account | EndUser` 的位置炸掉。**这是一个真实的类型谎言。**
3. **后者显式标了 `| None`** ——同一个「可能查不到」的事实，一处承认一处不承认。
4. **`_AppRunner._resolve_user` 的 `match` 有 `case _: raise AssertionError`** ——因为联合类型是封闭的；而 `_resolve_user_for_run` 用 `if/else` 两分支，**任何非 `ACCOUNT` 的 `created_by_role` 都会被当成 `END_USER` 处理**（含未来的新角色）。**同一个「二选一」在一处封闭、在另一处开放。**

**`_resolve_user_for_run` 还多做了一件前者不做的事：它相信 `workflow_run.created_by_role` 这个字符串。**

```python
role = CreatorUserRole(workflow_run.created_by_role)
```

**这行会在 `created_by_role` 是垃圾值时抛 `ValueError`** ——而调用方（`resume_workflow_execution`）在更外层没有接住它。**于是「数据库里有一行 role 值不合法的 workflow_run」会让恢复任务直接失败**，而这个失败是「任务异常」而不是「业务错误」。

**`_publish_failed_workflow_terminal_events` 的 docstring 把整个存在理由讲完了。**

```python
"""Publish synthetic workflow lifecycle events for pre-runtime failures.

Early failures can happen before the app generator creates a task entity or
emits any workflow queue events. In that window SSE consumers still need a
normal terminal event to close their state machines, so we synthesize a
minimal `workflow_started -> workflow_finished(failed)` sequence here.

`workflow_run_id` is reused as a synthetic `task_id` because no application
task id exists yet on this failure path.
"""
```

**三句对应三件事：**

1. **「Early failures can happen before the app generator creates a task entity or emits any workflow queue events」** ——故障窗口的定义：**生成器还没开始产出事件之前**。
2. **「SSE consumers still need a normal terminal event to close their state machines」** ——**这是全部动机**：消费者不是「等结果」，而是「等终止信号」。没有终止信号，前端会一直显示「运行中」。
3. **「`workflow_run_id` is reused as a synthetic `task_id`」** ——因为真正的 `task_id` 由生成器创建，此刻还不存在。**用 `workflow_run_id` 冒充是一个有意的妥协**，而它成立的前提是「消费者只用 `task_id` 做关联，不假设它是某种格式」。

**事件序列是「一对」而不是「一个终止」：**

```python
topic = MessageBasedAppGenerator.get_response_topic(exec_params.app_mode, exec_params.workflow_run_id)
started_payload = WorkflowStartStreamResponse(...)
topic.publish(json.dumps(started_payload.model_dump(mode="json"), ensure_ascii=False).encode())

finished_payload = WorkflowFinishStreamResponse(
    task_id=exec_params.workflow_run_id,
    workflow_run_id=exec_params.workflow_run_id,
    data=WorkflowFinishStreamResponse.Data(
        id=exec_params.workflow_run_id,
        workflow_id=exec_params.workflow_id,
        status=WorkflowExecutionStatus.FAILED,
        outputs=None,
        error=str(exc),
        elapsed_time=0.0,
        total_tokens=0,
        total_steps=0,
        created_by={},
        created_at=timestamp,
        finished_at=timestamp,
        exceptions_count=1,
        files=[],
    ),
)
topic.publish(json.dumps(finished_payload.model_dump(mode="json"), ensure_ascii=False).encode())
```

**六处：**

1. **先发 `workflow_started` 再发 `workflow_finished`** ——**即使是从没开始过的运行也补一个 started**。因为消费者的状态机是「未开始 → 运行中 → 结束」，只发 finished 会让它收到一个不合法的转移。
2. **`error=str(exc)`** ——把异常字符串直接给用户。**这是一个信息暴露面的判断**：`str(exc)` 可能含内部路径或 SQL 片段。代码没有做脱敏。
3. **`elapsed_time=0.0` / `total_tokens=0` / `total_steps=0` / `exceptions_count=1`** ——合成的数值必须**合法但无意义**。`exceptions_count=1` 说得通（确实有一个异常），其余三个 0 是「没跑过所以没有量」。
4. **`created_by={}` 空字典** ——这是一个需要「用户信息对象」的字段，此处无用户，用空对象占位。**消费方必须容忍空对象。**
5. **`timestamp` 只算一次，started 与 finished 共用** ——于是这两个事件的 `created_at` / `finished_at` 完全相同。**对消费者意味着「耗时 0」，与 `elapsed_time=0.0` 自洽。**
6. **`ensure_ascii=False`** ——中文错误消息不转义，减少体积。

**而这条合成路径在 `run()` 里的触发条件是「流式且抛了异常」：**

```python
try:
    response = self._run_app(...)
except Exception as exc:
    if exec_params.streaming:
        _publish_failed_workflow_terminal_events(exc=exc, exec_params=exec_params)
    raise
```

**三处：**

1. **只在 `streaming` 时补发** ——非流式调用没有 SSE 消费者在等，补发是多余的。
2. **`raise` 保留异常** ——所以 Celery 仍然记录任务失败并可能重试。**「补发事件」与「任务失败」是两个正交的结果，都要发生。**
3. **它只覆盖「`_run_app` 抛异常」这一种情况** ——若 `_run_app` 正常返回但内部生成器在中途抛错（生成器是懒的），这个 `except` 抓不到。**那种情况由 `_publish_streaming_response` 的 `except` 负责**（两次捕获针对两个阶段，见下）。

**`_publish_streaming_response` 的 docstring 是本页最密的契约声明。**

```python
"""Publish app stream events in order and terminate successful or broken streams.

Accept runtime events for the given app mode and run. A normally exhausted Advanced Chat
stream with a published message end, no workflow start, and no errors completes through an
internal transport signal after all output is published. It must not invent a workflow run.
Other streams require a workflow terminal event; missing terminals produce a failed fallback.
Iteration and publish errors also produce a fallback when possible, then propagate.
"""
```

**四句，而每句都对应代码里的一个分支：**

1. **「A normally exhausted Advanced Chat stream with a published message end, no workflow start, and no errors completes through an internal transport signal」** → 代码里的四条件判断：

```python
if (
    app_mode == AppMode.ADVANCED_CHAT
    and message_end_published
    and not started_published
    and not terminal_published
    and not stream_failed
):
    topic.publish(STREAM_COMPLETE_SIGNAL)
    return
```

2. **「It must not invent a workflow run」** ——**这是上面那个分支的关键**：对话型应用可能走了「不经过工作流」的路径（直接调模型），此时**不能因为「没收到终止事件」就补一个假的 workflow_finished**——那会让前端显示一个不存在的工作流运行。**所以这里发的是一个内部传输信号（`STREAM_COMPLETE_SIGNAL`）而不是工作流事件。**

3. **「Other streams require a workflow terminal event; missing terminals produce a failed fallback」** → 循环结束后的 `if not terminal_published:` 分支。

4. **「Iteration and publish errors also produce a fallback when possible, then propagate」** → `except Exception` 分支里的「补发后 `raise`」。

**这段 docstring 的价值在于它把「四种收尾路径」说清了，而读者不用从代码反推。** 而它也是本页最好的一个「为什么这里不能统一处理」的例证：**对话型与工作流型对「结束」的定义不同。**

**`_publish_streaming_response` 里的四个状态位逐 event 更新。**

```python
terminal_events = {"workflow_finished", "workflow_paused"}
unexpected_stream_end_message = "Workflow stream ended without a terminal event"
topic = MessageBasedAppGenerator.get_response_topic(app_mode, normalized_workflow_run_id)
started_published = False
terminal_published = False
message_end_published = False
stream_failed = False
last_task_id = normalized_workflow_run_id
stream_error_message: str | None = None
```

```python
for event in response_stream:
    event_name = _get_event_name(event)
    task_id = _get_task_id(event)
    if task_id is not None:
        last_task_id = task_id

    try:
        if isinstance(event, BaseModel):
            payload = json.dumps(event.model_dump(mode="json"), ensure_ascii=False)
        else:
            payload = json.dumps(event, ensure_ascii=False, default=str)
    except (TypeError, ValueError):
        stream_failed = True
        logger.exception("error while encoding event")
        continue

    topic.publish(payload.encode())

    if event_name == "workflow_started":
        started_published = True
    elif event_name in terminal_events:
        terminal_published = True
    elif event_name == "message_end":
        message_end_published = True
    elif event_name == "error":
        stream_failed = True
        stream_error_message = _get_error_message(event) or stream_error_message
```

**七处：**

1. **`terminal_events` 包含 `workflow_paused`** ——**「暂停」是一个终止事件**（对本次执行而言），否则一个暂停的运行会被补一个假的 failed 终止。**这是一个容易漏掉的语义**：暂停不是「没结束」，而是「这次执行结束了，等外部输入」。
2. **`last_task_id` 会被每个事件刷新** ——于是补发终止事件时用的是**最后一个见到的事件里的 task_id**，而不是最初的那个。**这解决了「生成器在运行中创建了 task 实体」的情况**：早先不知道 task_id，后来事件里带了。
3. **编码失败只 `continue`** ——该事件被丢弃但流继续。**这是一个明确的取舍：单个坏事件不该中断整条流**，代价是消费者可能少一条消息。
4. **`default=str` 在非 `BaseModel` 分支上** ——让不可 JSON 化的值退化成字符串而不是抛错。**而 `BaseModel` 分支用 `model_dump(mode="json")`，由 Pydantic 负责。**
5. **`topic.publish(payload.encode())` 在状态更新之前** ——**先发后记**。若 publish 抛错，状态位就没更新，于是外层 `except` 会补发终止事件。**顺序是对的：先做可能失败的事，再记状态。**
6. **`stream_error_message = _get_error_message(event) or stream_error_message`** ——**首次出现的错误消息会被保留**（`or` 让后面的空值不覆盖前面的）。注释没写，但这个「保留第一个」的选择是对的：第一个错误通常是根因。
7. **`_get_*` 系列三个函数都先调 `_get_event_data`** ——于是「字符串事件」被统一处理成 `None`（`_get_event_data` 对 `str` 返回 `None`），这几个辅助函数对它一律返回 `None`。**`_get_event_data` 的注释说得很实在**：

```python
if isinstance(event, BaseModel):
    # Temporary compatibility for legacy BaseModel stream events; remove after confirming generators always emit
    # str / Mapping responses.
    return event.model_dump()
```

**「Temporary compatibility ... remove after confirming」** ——一个带退出条件的兼容层。**这类注释比 TODO 有用：它说明了「什么条件下可以删」。**

**`workflow_based_app_execution_task` 本体只有五行。**

```python
@shared_task(queue=WORKFLOW_BASED_APP_EXECUTION_QUEUE)
def workflow_based_app_execution_task(payload: str) -> Mapping[str, Any] | None:
    exec_params = AppExecutionParams.model_validate_json(payload)
    logger.info("workflow_based_app_execution_task run with params: %s", exec_params)
    runner = _AppRunner(db.engine, exec_params=exec_params)
    return runner.run()
```

**四处：**

1. **任务是同步函数，接收 `payload: str`（JSON 字符串）而不是 dict** ——**字符串是最稳的序列化形式**（不受 Celery 序列化器配置影响）。与 `async_workflow_tasks.py` 里收 `dict` 的任务形成对比。
2. **`model_validate_json` 立即校验** ——坏 payload 在任务开始的第一行就失败。
3. **`logger.info(... run with params: %s", exec_params)`** ——**把完整参数（含 `args` 里的用户输入）打进日志**。这是一个**隐私风险**：用户输入可能含敏感内容。
4. **`_AppRunner(db.engine, ...)` 传的是 `Engine` 而不是 `sessionmaker`** ——而 `_AppRunner.__init__` 有对应的兼容处理：

```python
if isinstance(session_factory, Engine):
    session_factory = sessionmaker(bind=session_factory)
```

**这个重载让「传 engine 或传 maker」都成立**，代价是构造器里有一个 `isinstance` 分支。

**`_AppRunner._session()` 是一个「总是开事务」的上下文管理器。**

```python
@contextlib.contextmanager
def _session(self):
    with self._session_factory(expire_on_commit=False) as session, session.begin():
        yield session
```

**`session.begin()` 意味着「with 块正常退出即提交，异常即回滚」** ——所以每一个 `with self._session() as session:` 都是一个完整事务。**而 `run()` 里有三次这样的调用**（查 workflow/app、解析用户、执行应用体），**各自是独立事务**。这与 h01 里「三段式」是同一个思路：**把长流程切成短事务，避免在等待 LLM 时持有事务。**

**`run()` 里那份「先查 workflow 与 app，再离开会话」的写法值得单独指出。**

```python
with self._session() as session:
    workflow = session.get(Workflow, exec_params.workflow_id)
    if workflow is None:
        logger.warning("Workflow %s not found for execution", exec_params.workflow_id)
        return None
    app = session.get(App, workflow.app_id)
    if app is None:
        logger.warning("App %s not found for workflow %s", app.workflow_id if False else workflow.app_id, ...)
        return None
```

（实际代码里 `logger.warning("App %s not found for workflow %s", workflow.app_id, exec_params.workflow_id)`。）

**关键点是 `with` 块结束后 `workflow` 与 `app` 仍是可用对象** ——因为 `expire_on_commit=False`。**若忘了这个参数，`with` 退出后访问 `workflow.created_by` 就会触发「已被 detach」错误。** 而后面的代码正是这么用的：

```python
pause_config = PauseStateLayerConfig(
    session_factory=self._session_factory,
    state_owner_user_id=workflow.created_by,
)
```

**这是一个「用会话外对象」的模式，它的正确性完全依赖 `expire_on_commit=False`** ——而这个参数在两处（`_session()` 与 `run()` 里的 `self._session_factory(expire_on_commit=False)`）都显式写了。**同一参数写两遍说明作者知道它关键。**

**`run()` 的 `with` 语句把 Flask 上下文与数据库会话并列：**

```python
with (
    self._setup_flask_context(user),
    self._session_factory(expire_on_commit=False) as session,
):
```

**两个上下文管理器的退出顺序是「后进先出」** ——所以会话先关、Flask 上下文后散。**这个顺序有影响**：若会话关闭时还在用 `current_app`（比如连接池的 teardown），顺序反了会炸。**代码正确但依赖读者理解 `with (a, b)` 的语义。**

**`async_workflow_tasks.py` 的三个 `execute_workflow_*` 是「同一实现的三个队列版本」。**

```python
@shared_task(queue=AsyncWorkflowQueue.PROFESSIONAL_QUEUE)
def execute_workflow_professional(task_data_dict: dict[str, Any]) -> None:
    task_data = WorkflowTaskData.model_validate(task_data_dict)
    application_services().triggers.executions.execute(task_data.workflow_trigger_log_id)

@shared_task(queue=AsyncWorkflowQueue.TEAM_QUEUE)
def execute_workflow_team(task_data_dict: dict[str, Any]) -> None:
    task_data = WorkflowTaskData.model_validate(task_data_dict)
    application_services().triggers.executions.execute(task_data.workflow_trigger_log_id)

@shared_task(queue=AsyncWorkflowQueue.SANDBOX_QUEUE)
def execute_workflow_sandbox(task_data_dict: dict[str, Any]) -> None:
    ...
```

**三处：**

1. **三个函数体逐字相同，差异只在 `queue=`** ——因为 Celery 的队列是**装饰器参数**，不能由运行时值决定。**于是「按套餐分队列」这件事必须复制三份。**
2. **`execute` 的入参只是 `workflow_trigger_log_id`** ——**一个 id，不是完整上下文**。所有状态从库里读。这与 `AppExecutionParams` 把 id 与参数一起传形成对比。**传 id 的好处是「重试时读到的总是最新状态」，坏处是「队列延迟期间状态可能已变」。**
3. **任务名默认取函数名** ——`execute_workflow_professional` 这个名字成了队列里的路由键，于是**重命名函数会破坏已在队列里的消息**。而下面那个 `resume_workflow_execution` **显式写了 `name=`**：

```python
@shared_task(name="resume_workflow_execution")
def resume_workflow_execution(task_data_dict: dict[str, Any]) -> None:
```

**为什么这一个要显式命名？** 因为**它会通过名字被别的进程发消息**（`resume_app_execution` 之类的调用点用字符串指定任务），而**函数名一旦显式写出来，重命名函数就安全了**。这是一个「显式路由键」的正确做法，而同一文件里另外三个任务没这么做。

**`resume_workflow_execution` 的流程是「读暂停记录 → 校验 → 重建上下文 → 恢复 → 删暂停记录」。**

```python
task_data = WorkflowResumeTaskData.model_validate(task_data_dict)
session_factory = sessionmaker(bind=db.engine, expire_on_commit=False)
workflow_run_repo = DifyAPIRepositoryFactory.create_api_workflow_run_repository(session_factory)

pause_entity = workflow_run_repo.get_workflow_pause(task_data.workflow_run_id)
if pause_entity is None:
    logger.warning("No pause state for workflow run %s", task_data.workflow_run_id)
    return
workflow_run = workflow_run_repo.get_workflow_run_by_id_without_tenant(pause_entity.workflow_execution_id)
if workflow_run is None:
    logger.warning("Workflow run not found for pause entity: pause_entity_id=%s", pause_entity.id)
    return

try:
    resumption_context = WorkflowResumptionContext.loads(pause_entity.get_state().decode())
except Exception as exc:
    logger.exception("Failed to load resumption context for workflow run %s", task_data.workflow_run_id)
    raise exc
```

**四处，而失败处理的分裂是重点：**

1. **「没有暂停记录」「找不到 workflow_run」「实体类型不对」三种情况都`return`（静默成功）** ——因为它们都是**幂等重投递的正常结果**：任务被投了两次，第二次找不到记录是对的。
2. **「上下文反序列化失败」却 `raise`** ——因为它不可能是重投递导致的，只能是数据损坏或代码不兼容。**这个分类判据很清晰：能否由「重复投递」解释。**
3. **`raise exc` 而不是裸 `raise`** ——两者在语义上等价（Python 里 `raise exc` 会重置 traceback 起点，实际上会丢失原始帧）。**这是一个细节缺陷**：`raise` 更好。
4. **`get_workflow_run_by_id_without_tenant(...)`** ——**方法名里显式说了「不带租户」**。因为此路径从暂停实体拿 run id，而暂停实体本身已带租户约束。**这个名字是一个安全审计的好帮手**：任何不带租户的查询都能被 grep 出来。

**「旧 task_id 恢复」那段带了一个有到期条件的 TODO。**

```python
# TODO: Remove legacy task recovery 30 days after the last legacy writer is retired.
task_id = workflow_run.task_id
if task_id is None:
    try:
        task_id = str(UUID(generate_entity.task_id))
    except ValueError:
        logger.warning("Invalid saved task ID for workflow run %s", task_data.workflow_run_id)
        return
    AppTaskService.index_workflow_pause_task(...)
    indexed_run = workflow_run_repo.get_workflow_run_by_id(
        workflow_run.tenant_id, workflow_run.app_id, workflow_run.id
    )
    if indexed_run is None or indexed_run.task_id != task_id:
        logger.warning("Cannot recover saved task ownership for workflow run %s", task_data.workflow_run_id)
        return
    workflow_run.task_id = task_id
generate_entity.task_id = task_id
```

**六处：**

1. **TODO 里带了一个**可执行的到期条件**（「最后一个旧写入方下线后 30 天」）**——比「TODO: remove」有用得多。
2. **`str(UUID(generate_entity.task_id))` 是一次规范化** ——把可能存成非标准形式的 UUID 转成标准形式，并在失败时 `return`（而不是用原值）。
3. **`index_workflow_pause_task` 之后**重新查一遍 run，并比对 `task_id`** ——**这是一个「写入后读回验证」的模式**：确认索引真的生效了，而且拿到的是自己写的那个值。
4. **`indexed_run is None or indexed_run.task_id != task_id` 时 `return`** ——**放弃恢复而不是继续**。因为若 task 所有权没建立，后续的事件发布与审计都会落到错误的 task 上。
5. **`workflow_run.task_id = task_id` 只在这条恢复路径上赋值** ——说明 `task_id` 是可以为空的列（旧数据）。
6. **`generate_entity.task_id = task_id` 无条件赋值** ——即使 `workflow_run.task_id` 本来就有值。**这是必要的**：`generate_entity` 来自序列化的上下文，而那个副本可能更旧。

**恢复执行体里有一段「按触发日志决定是否加时间片层」的逻辑。**

```python
trigger_logs = application_services().triggers.execution_logs
trigger_log = trigger_logs.find_for_run(
    tenant_id=workflow_run.tenant_id, app_id=workflow_run.app_id, workflow_run_id=task_data.workflow_run_id
)

if trigger_log:
    cfs_plan_scheduler_entity = AsyncWorkflowCFSPlanEntity(
        queue=AsyncWorkflowQueue(trigger_log.queue_name),
        schedule_strategy=AsyncWorkflowSystemStrategy,
        granularity=dify_config.ASYNC_WORKFLOW_SCHEDULER_GRANULARITY,
    )
    cfs_plan_scheduler = AsyncWorkflowCFSPlanScheduler(plan=cfs_plan_scheduler_entity)

    graph_engine_layers.extend(
        [
            TimeSliceLayer(cfs_plan_scheduler),
            TriggerPostLayer(logs=trigger_logs, log=trigger_log, start_time=start_time),
        ]
    )
```

**四处：**

1. **`if trigger_log:` 才加层** ——由触发器发起的工作流恢复时要把「时间片调度」与「触发器收尾」两层插回图引擎；而由用户手动暂停/恢复的工作流没有触发日志，就不加。**同一个恢复路径服务两类来源，靠「有没有触发日志」区分。**
2. **`AsyncWorkflowQueue(trigger_log.queue_name)` 从库里读队列名** ——于是**恢复时用的队列与最初执行时一致**（专业版/团队版/沙箱版各有配额）。**这是一个「把调度策略持久化」的做法**，正确性依赖 `queue_name` 被正确写入。
3. **`granularity=dify_config.ASYNC_WORKFLOW_SCHEDULER_GRANULARITY`** ——**唯一的运行时配置注入点**：时间片粒度不由历史决定，而由当前配置决定。**结果是「改了配置后，恢复中的工作流会按新粒度跑」。**
4. **`start_time = datetime.now(UTC)` 在 `TriggerPostLayer` 里** ——它是恢复时刻，不是原始开始时刻。**于是触发器日志里的耗时会把「暂停等待时间」算进去。**

**恢复的最后四步有一个严格的顺序。**

```python
workflow_run_repo.resume_workflow_pause(task_data.workflow_run_id, pause_entity)

generator.resume(...)

workflow_run_repo.delete_workflow_pause(pause_entity)
```

**三处：**

1. **先 `resume_workflow_pause`（标记为已恢复）再执行** ——**防止并发恢复**：两个任务同时进来，只有一个能把状态从 paused 改走。这是「用状态机做互斥」的标准手法。
2. **`generator.resume(...)` 是一个同步调用，内部可能有长时间运行** ——所以「删除暂停记录」在它之后。
3. **`delete_workflow_pause` 在成功后才执行** ——若 `resume` 抛异常，暂停记录保留，任务重投递时还能再恢复一次。**这是一个正确的「保留可重试状态」的选择。**

**`_get_user` 的 docstring 一句话划清了新旧路径。**

```python
def _get_user(session: Session, workflow_run: WorkflowRun) -> Account | EndUser:
    """Legacy HITL resume hydration; trigger task execution uses its scoped repository."""
```

**「Legacy HITL resume hydration」** ——它只服务「人工介入（human input）的旧恢复路径」，而新的触发器任务走自己的作用域仓库。**这解释了为什么它需要 `set_current_tenant_with_session`：**

```python
if workflow_run.created_by_role == CreatorUserRole.ACCOUNT:
    user = session.scalar(select(Account).where(Account.id == workflow_run.created_by))
    if user:
        user.set_current_tenant_with_session(tenant, session=session)
else:  # CreatorUserRole.END_USER
    user = session.scalar(select(EndUser).where(EndUser.id == workflow_run.created_by))
```

**四处：**

1. **先查 `Tenant` 且找不到就抛 `_TenantNotFoundError`** ——而 `_get_user` 返回的是用户，为什么要查租户？因为 `set_current_tenant_with_session` 需要一个 `Tenant` 对象。**于是「取用户」这个函数有一个隐式的「必须能取到租户」的前置条件。**
2. **只有 `Account` 才 `set_current_tenant_with_session`** ——`EndUser` 没有租户概念（它属于应用而非工作区）。
3. **`if user:` 而不是 `if user is not None:`** ——`Account` 若定义了 `__bool__` 或 `__len__`，这里会走错分支。**对 ORM 对象用真值判断是一个习惯性风险**，虽然实践中通常没问题。
4. **两个异常类定义在文件末尾** ——`_TenantNotFoundError` / `_UserNotFoundError` / `_AppNotFoundError` 都在 227–236 行，而使用在 198–224 行。**Python 允许（名字在运行时解析），但读者要跳到最后才能看到定义。**

**而这三个异常都是空类（`pass`）。**

```python
class _TenantNotFoundError(Exception):
    pass
```

**空体意味着它们只提供**

**「类型」而不提供任何行为** ——调用方可以按类型区分三种失败。**而它们是私有的（下划线开头）**，说明只在模块内使用。**这也意味着一件事：`_get_user` 抛出的异常不会被任何共享的错误处理器识别**，只能在本模块里被接住——而代码里没有任何地方接住它们，所以它们会作为「任务异常」冒到 Celery。

**`_get_user` 构造异常时用了 `%s` 占位与多个参数：**

```python
raise _TenantNotFoundError(
    "Tenant not found for WorkflowRun: tenant_id=%s, workflow_run_id=%s",
    workflow_run.tenant_id,
    workflow_run.id,
)
```

**这是一个缺陷**：`Exception` 的构造器只接一个参数，`%s` 不会被替换。**于是 `str(exc)` 会得到一个元组字符串 `("Tenant not found ...%s...", 'tid', 'rid')`。** 正确写法是先 `%` 好再传。**这个缺陷的后果是日志里能拿到信息（三个值都在），但格式难看且不可 grep。**

## 关键取舍

**源码事实：** 用户身份通过 `_Account` / `_EndUser` 两个只含 id 的 Pydantic 模型 + `Tag`/`Discriminator` 的联合类型跨进程传递；判别器函数名 `_get_user_type_descriminator` 被用作 `Discriminator(...)` 的参数（源码里 `discriminator` 拼作 `descriminator`，改名要同改定义与引用两处）。

**源码事实：** `_AppRunner._resolve_user` 从任务参数取 id，返回类型标注不含 `None` 而 `session.get` 可能返回 `None`；`_resolve_user_for_run` 从 `workflow_run.created_by` 取 id，显式标 `| None`，且用 `if/else` 两分支（任何非 ACCOUNT 的角色都被当 END_USER）。

**源码事实：** `_AppRunner` 用 `_setup_flask_context` 进 `app_context` 并 `set_login_user`；`_session()` 总是 `session.begin()`（with 块退出即提交），并统一用 `expire_on_commit=False`，使得会话关闭后仍可读 `workflow.created_by` 等字段。

**源码事实：** `_publish_failed_workflow_terminal_events` 在 `_run_app` 抛异常且 `streaming=True` 时补发一对 `workflow_started → workflow_finished(failed)`，用 `workflow_run_id` 冒充 `task_id`，`elapsed_time`/`total_tokens`/`total_steps` 为 0、`exceptions_count=1`、`created_by={}`，并把 `str(exc)` 直接放进事件的 `error`。

**源码事实：** `_publish_streaming_response` 维护 `started_published` / `terminal_published` / `message_end_published` / `stream_failed` / `last_task_id` / `stream_error_message` 六个状态；`terminal_events = {"workflow_finished", "workflow_paused"}`（暂停算终止）；`last_task_id` 随每个事件刷新；编码失败只 `continue`；错误消息保留首次出现者；publish 在状态更新之前。

**源码事实：** 对话型应用的「正常结束」走四条件分支（ADVANCED_CHAT + 有 message_end + 无 started + 无 terminal + 无 error）并发 `STREAM_COMPLETE_SIGNAL`，docstring 明确「It must not invent a workflow run」；其它流缺终止事件时补发 failed 兜底；迭代/发布异常时先补发再 `raise`。

**源码事实：** `_get_event_data` 对 `BaseModel` 的兼容分支带「Temporary compatibility ... remove after confirming generators always emit str / Mapping responses」注释。

**源码事实：** `workflow_based_app_execution_task` 收 JSON 字符串并用 `model_validate_json` 校验，把完整 `exec_params`（含 `args` 用户输入）以 `logger.info` 记入日志；`_AppRunner` 接受 `Engine` 或 `sessionmaker` 并内部统一。

**源码事实：** `async_workflow_tasks.py` 的 `execute_workflow_professional` / `_team` / `_sandbox` 三函数体逐字相同，仅 `queue=` 不同；它们只传 `workflow_trigger_log_id` 一个 id。`resume_workflow_execution` 显式声明 `name="resume_workflow_execution"`（其余三个未命名）。

**源码事实：** `resume_workflow_execution` 对「无暂停记录」「run 不存在」「实体类型不符」三种情况静默 `return`（可幂等重投递解释），对「上下文反序列化失败」`raise exc`（不可由重复投递解释，但 `raise exc` 不如裸 `raise` 保留 traceback）。

**源码事实：** 旧 `task_id` 恢复段落带 `TODO: Remove legacy task recovery 30 days after the last legacy writer is retired.`，包含 `str(UUID(...))` 规范化、写入后重查并比对 `task_id`、不一致则放弃恢复。

**源码事实：** 恢复时若存在触发日志，则向图引擎插入 `TimeSliceLayer` + `TriggerPostLayer` 两层，队列名从 `trigger_log.queue_name` 读回，时间片粒度取当前配置 `ASYNC_WORKFLOW_SCHEDULER_GRANULARITY`，`start_time` 为恢复时刻（会把暂停等待时长算入触发器耗时）。

**源码事实：** 恢复顺序为 `resume_workflow_pause` → `generator.resume(...)` → `delete_workflow_pause`，用「先标记、成功后删」实现并发互斥与可重试。

**源码事实：** `_get_user` docstring 标明「Legacy HITL resume hydration」，先查 `Tenant`（缺失抛 `_TenantNotFoundError`），`Account` 分支调用 `set_current_tenant_with_session`；三个异常类定义在文件末尾且均为空类；构造时用「格式串 + 多参数」传给 `Exception`，`%s` 不会被替换。

**架构推论：** 「跨进程只传 id」让任务参数不可变、重试时读最新状态，代价是 **worker 必须重新解析身份，而解析失败的处理必须分门别类**：能由重复投递解释的失败静默返回，不能的必须上抛。这两个文件的判据基本清晰，但 `_resolve_user` 返回 `None` 却被标注成非空，属于分类没落到类型上。

**架构推论：** 「事件流的终止语义」不能统一处理，因为不同类型的流对「结束」的定义不同（对话型可能不经过工作流）。`STREAM_COMPLETE_SIGNAL` 这个内部传输信号是正确解法：**它不假装产生一个业务事件，而是产生一个传输层信号**。主核里做「请求-响应流」时同理：**当「业务结束」与「传输结束」可能分离时，必须有两个不同的信号。**

**架构推论：** 「合成事件」是分布式边界上的必要妥协，而它必须做到三件事：**补齐状态机的合法前驱**（所以先发 started）、**每个字段都合法但可识别为无意义**（0 与空对象）、**带上真实原因**（`str(exc)`）。缺第一件会让消费者收到非法转移，缺第二件会让统计把合成事件算进业务量。这里三件都做了，唯一的缺口是 `str(exc)` 直接暴露给终端用户。

**架构推论：** 「队列名在任务装饰器里」导致「按套餐分队列」必须复制函数体。这不是可以靠抽象解决的问题（Celery 的装饰器参数是 import 期的静态值），但**可以用代码生成或 `make_task(queue)` 工厂缓解**。而一旦选择复制，就应当把「三份必须同步」写成测试（比较三个函数体的字节码或源码）——**否则三者迟早漂移，而漂移的表现是「某个套餐的行为与其它两个不同」。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/tasks/app_generate/workflow_execute_task.py` | `workflow_based_app_execution_task` | Celery 入口（收 JSON 字符串，构造 runner 并执行） |
| `api/tasks/app_generate/workflow_execute_task.py` | `AppExecutionParams` | 跨进程执行参数（含判别器联合的用户身份） |
| `api/tasks/app_generate/workflow_execute_task.py` | `_get_user_type_descriminator` | 联合类型判别器（同时接受对象与 dict；源码里拼作 descriminator） |
| `api/tasks/app_generate/workflow_execute_task.py` | `_AppRunner` | worker 侧执行器（会话、Flask 上下文、用户解析、生成器分派） |
| `api/tasks/app_generate/workflow_execute_task.py` | `_AppRunner._session` | 总是开事务的会话上下文（`expire_on_commit=False`） |
| `api/tasks/app_generate/workflow_execute_task.py` | `_AppRunner._setup_flask_context` | 重建 Flask 应用上下文并设置登录用户 |
| `api/tasks/app_generate/workflow_execute_task.py` | `_AppRunner.run` | 短事务查 workflow/app → 解析用户 → 执行 → 失败补发事件 |
| `api/tasks/app_generate/workflow_execute_task.py` | `_resolve_user_for_run` | 按 `created_by_role` 从库重建用户（显式 `| None`） |
| `api/tasks/app_generate/workflow_execute_task.py` | `_publish_failed_workflow_terminal_events` | 运行前失败的合成终止事件（started → finished(failed)） |
| `api/tasks/app_generate/workflow_execute_task.py` | `_publish_streaming_response` | 流式事件转发与四种收尾路径（含 `STREAM_COMPLETE_SIGNAL`） |
| `api/tasks/app_generate/workflow_execute_task.py` | `_get_event_data` / `_get_event_name` / `_get_task_id` / `_get_error_message` | 事件字段的容错读取（字符串事件一律返回 `None`） |
| `api/tasks/app_generate/workflow_execute_task.py` | `_resume_app_execution` / `_resume_workflow` / `resume_app_execution` | 本地恢复路径（从暂停记录重建上下文） |
| `api/tasks/async_workflow_tasks.py` | `execute_workflow_professional` | 专业版队列的触发器执行（三份同名实现之一） |
| `api/tasks/async_workflow_tasks.py` | `resume_workflow_execution` | 暂停工作流的异步恢复（读暂停记录、校验、重建、恢复、删除） |
| `api/tasks/async_workflow_tasks.py` | `_get_user` | 旧人工介入恢复路径的用户装配（需要 `Tenant` 以设置当前租户） |
| `api/tasks/async_workflow_tasks.py` | `_TenantNotFoundError` / `_UserNotFoundError` / `_AppNotFoundError` | 模块私有异常（空类，定义在文件末尾） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [Celery 文档：任务、队列与路由](https://docs.celeryq.dev/en/stable/userguide/routing.html)
- [Pydantic 文档：可判别联合类型](https://docs.pydantic.dev/latest/concepts/unions/#discriminated-unions)
- [Flask 文档：应用上下文](https://flask.palletsprojects.com/en/stable/appcontext/)

## 与主核 / AI 的关系

异步任务层在主核（多模态数据库内核）里对应的是**后台 worker 与恢复协议**（checkpointer、WAL replay、并行 worker 的启动参数）。四条经验。

**第一，跨进程只传 id，但「解析失败」必须分类。** 能由重复投递解释的失败（记录已不存在）应当静默返回以保持幂等；不能解释的失败（数据损坏、格式不兼容）必须上抛。这两个文件的判据基本正确，缺口在「返回 `None` 却被标注成非空」——**类型上的诚实比文档上的诚实更有效**，因为调用方不会去读文档。

**第二，「业务结束」与「传输结束」必须是两个信号。** 对话型应用可能不产生工作流事件，此时只能发一个传输层完成信号，而不能伪造一个业务事件。主核里做「查询结束」时同理：**「游标耗尽」与「事务提交」是两件事**，混为一谈会让上层在「没有更多行」与「已经安全」之间做出错误判断。

**第三，恢复路径的顺序应当是「标记 → 执行 → 清理」，且中间失败要保留可重试状态。** 这里 `resume_workflow_pause` 先行（用状态机做并发互斥）、`delete_workflow_pause` 最后（失败后仍可再次恢复）。主核里做崩溃恢复（`xlog` 重放、checkpoint 推进）时同理：**「标记已开始」与「标记已完成」必须分开写**，只写一次的话要么重复执行要么丢失。

**第四，「按队列/套餐复制函数体」是装饰器静态性导致的结构性重复，必须用测试锁住。** 三个 `execute_workflow_*` 只在 `queue=` 上不同，任何一处逻辑改动都要求同步三份。主核里若有「按后端复制的注册表」（不同编译选项下的同构函数），也应当有一条对拍测试——**因为这类重复不会在代码评审中被发现，只会在「某个后端行为不一样」的故障里被发现。**
