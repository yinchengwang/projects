---
project: dify
feature: celery-and-blueprints
title: Celery 与蓝图装配
page_type: feature
schema_version: 1
refs:
  - path: api/extensions/ext_celery.py
    symbols: [CelerySSLOptionsDict]
  - path: api/extensions/ext_blueprints.py
    symbols: [init_app]
---

# Celery 与蓝图装配

## 一句话职责

`ext_celery.init_app` 把一个裸 `Celery` 实例装配成「带 Flask 上下文的执行器 + 按配置开关生成的任务表与定时表」；`ext_blueprints.init_app` 把九个蓝图按各自的路由前缀与**逐路径不同的 CORS 策略**注册到同一个 Flask 应用上。

## 机制是什么

这两个文件都是**装配点**（composition root）：它们不实现任何业务，只把「配置 + 清单」翻译成「运行时对象」。而它们的复杂度全部来自同一个原因：**装配阶段的代码只能靠 if 分支表达，且这些分支分散在两个不同的层**。

对 Celery 而言，分支来自三层：

1. **进程级配置**（broker 是不是 rediss、要不要 Sentinel、日志格式）；
2. **功能开关**（三十多个 `ENABLE_*` 决定哪些任务模块被 import、哪些定时任务被登记）；
3. **依赖就绪状态**（`get_configured_knowledge_fs_lifecycle_worker_readiness().ready` 决定是否加三个任务模块与四个定时任务）。

对蓝图而言，分支来自**同一路径下不同端点的安全语义**：同一个 `web` 蓝图里，`/chat-messages` 是**未认证的嵌入端**（允许跨域但不要 cookie），而 `/*` 其余是**认证端点**（要 cookie 但只允许白名单来源）。于是 CORS 不是「一个蓝图一套策略」，而是**一个蓝图里按路径分三套**。

## 怎么实现的

**`FlaskTask` 是 Celery 与 Flask 之间唯一的接缝。**

```python
def init_app(app: DifyApp) -> Celery:
    class FlaskTask(Task):
        def __call__(self, *args: object, **kwargs: object) -> object:
            from core.logging.context import init_request_context

            with app.app_context():
                # Initialize logging context for this task (similar to before_request in Flask)
                init_request_context()
                return self.run(*args, **kwargs)
```

**五处：**

1. **类定义在函数内部** ——于是它闭包捕获 `app`。**这解决了「task_cls 需要在 import 期指定，但 app 要在运行期才有」的矛盾。**
2. **`with app.app_context()`** ——每个任务都自带一个应用上下文。**这是 `current_app`、`db.session`、`g` 在 worker 里能工作的前提**，也是 h/i03 页里 `_AppRunner` 仍要再套一层 `app_context` 的原因（**可以嵌套**）。
3. **`init_request_context()` 模拟 `before_request`** ——注释点明了：「similar to before_request in Flask」。**这是为了让日志带上 trace id 之类的上下文**，否则 worker 日志里所有任务的行都没有关联标识。
4. **`return self.run(*args, **kwargs)`** ——不重写 `run`，只在外面加壳。**这是继承 `Task` 而不是实现「基类 + mixin」的最小做法。**
5. **`from core.logging.context import init_request_context` 在函数内 import** ——避免循环依赖（logging 模块可能 import 配置，配置可能 import extensions）。

**`CelerySSLOptionsDict` 是一个 `TypedDict`，而它的作用是「把 Redis 的字符串配置映射成 ssl 常量」。**

```python
class CelerySSLOptionsDict(TypedDict):
    ssl_cert_reqs: int
    ssl_ca_certs: str | None
    ssl_certfile: str | None
    ssl_keyfile: str | None
```

```python
def get_celery_ssl_options() -> CelerySSLOptionsDict | None:
    """Get SSL configuration for Celery broker/backend connections."""
    # Only apply SSL if we're using Redis as broker/backend
    if not dify_config.BROKER_USE_SSL:
        return None

    # Check if Celery is actually using Redis
    broker_is_redis = dify_config.CELERY_BROKER_URL and (
        dify_config.CELERY_BROKER_URL.startswith("redis://") or dify_config.CELERY_BROKER_URL.startswith("rediss://")
    )

    if not broker_is_redis:
        return None

    cert_reqs_map = {
        "CERT_NONE": ssl.CERT_NONE,
        "CERT_OPTIONAL": ssl.CERT_OPTIONAL,
        "CERT_REQUIRED": ssl.CERT_REQUIRED,
    }

    ssl_cert_reqs = cert_reqs_map.get(dify_config.REDIS_SSL_CERT_REQS, ssl.CERT_NONE)

    return CelerySSLOptionsDict(
        ssl_cert_reqs=ssl_cert_reqs,
        ssl_ca_certs=dify_config.REDIS_SSL_CA_CERTS,
        ssl_certfile=dify_config.REDIS_SSL_CERTFILE,
        ssl_keyfile=dify_config.REDIS_SSL_KEYFILE,
    )
```

**六处：**

1. **两道前置判断** ——`BROKER_USE_SSL`（来自 `rediss://` 前缀）与「broker 确实是 redis」双重确认。**第二道是多余的但无害**：`BROKER_USE_SSL` 本身就是靠 `rediss://` 前缀算出来的（i02 页里 `BROKER_USE_SSL` 的实现）。**两处判断同一个事实，说明这段代码对「配置可能不一致」有防御性。**
2. **`cert_reqs_map` 把字符串映射成 `ssl` 模块常量** ——因为环境变量只能是字符串，而 Celery 要整数。**三个键名与 Python 的 `ssl.CERT_*` 常量同名**，于是映射是可直接核对的。
3. **`cert_reqs_map.get(..., ssl.CERT_NONE)` 的兜底是 `CERT_NONE`** ——**这是一个不安全方向上的兜底**：拼错配置项会静默变成「不校验证书」。**更安全的方向应当是 `CERT_REQUIRED` 或直接报错。**
4. **四个字段全量返回，允许 `None`** ——`ssl_ca_certs` / `ssl_certfile` / `ssl_keyfile` 都是可选路径。**`None` 在 Celery/redis-py 里表示「不用这个配置」**，而不是「空字符串」。
5. **`TypedDict` 而不是普通 dict** ——于是四个键名拼错会被类型检查器发现。**这与 i02 的 `SQLAlchemyEngineOptionsDict` 是同一手法：用 `TypedDict` 约束「由字符串键组成的结构」。**
6. **函数名是 `get_` 而不是 `build_`** ——但它每次调用都构造新对象。`get_` 在此暗示「读取配置」，而实际上有映射与兜底逻辑。

**`get_celery_broker_transport_options` 里有本页唯一一处「配置校验即启动门禁」。**

```python
def get_celery_broker_transport_options() -> CelerySentinelTransportDict | dict[str, Any]:
    """Get broker transport options (e.g. Redis Sentinel) for Celery connections."""
    transport_options: CelerySentinelTransportDict | dict[str, Any]
    if dify_config.CELERY_USE_SENTINEL:
        transport_options = CelerySentinelTransportDict(
            master_name=dify_config.CELERY_SENTINEL_MASTER_NAME,
            sentinel_kwargs=_CelerySentinelKwargsDict(
                socket_timeout=dify_config.CELERY_SENTINEL_SOCKET_TIMEOUT,
                password=dify_config.CELERY_SENTINEL_PASSWORD,
            ),
        )
    else:
        transport_options = {}

    global_keyprefix = get_celery_redis_global_keyprefix()
    if global_keyprefix:
        transport_options["global_keyprefix"] = global_keyprefix

    visibility = dify_config.CELERY_BROKER_VISIBILITY_TIMEOUT
    if isinstance(visibility, int) and not isinstance(visibility, bool):
        transport_options["visibility_timeout"] = visibility
    if dify_config.KNOWLEDGE_FS_BACKGROUND_WORKER_ENABLED is True:
        broker = dify_config.CELERY_BROKER_URL or ""
        if not broker:
            raise ValueError("KnowledgeFS workers require an explicit CELERY_BROKER_URL")
        if broker.startswith(("redis://", "rediss://", "sentinel://")) and (
            not isinstance(visibility, int)
            or visibility <= dify_config.KNOWLEDGE_FS_BACKGROUND_TASK_TIMEOUT_SECONDS + 30
        ):
            raise ValueError("KnowledgeFS workers require CELERY_BROKER_VISIBILITY_TIMEOUT above their hard task limit")
```

**七处：**

1. **`CelerySentinelTransportDict` 声明为 `total=False`** ——所以四个键都可缺失。**这与 `CelerySSLOptionsDict`（全必填）相反**，因为 Sentinel 相关项是可选叠加的。
2. **`global_keyprefix` 与 `visibility_timeout` 在 Sentinel 之外单独追加** ——于是两种配置形态（Sentinel / 普通 Redis）都能带这两项。**代码用一个可变 dict 累积，而不是构造两个不同的对象。**
3. **`isinstance(visibility, int) and not isinstance(visibility, bool)`** ——**排除 `bool` 是必需的**：Python 里 `True` 是 `int` 的子类，若配置用了 `CELERY_BROKER_VISIBILITY_TIMEOUT=true` 会被当成 1 秒。**这个判据在 h06 的 `_usage.count` 与 `_seconds` 里也出现过，是这套代码里一致的防御习惯。**
4. **`KNOWLEDGE_FS_BACKGROUND_WORKER_ENABLED is True` 用 `is True`** ——**严格相等**，所以 `1` 或 `"true"` 都不会触发。对一个本来已经是 `bool` 的配置项来说这是冗余的，但它把「必须显式开启」写进了代码。
5. **第一条 `raise`：没有 broker URL 就拒绝启动** ——因为知识文件系统 worker 全程依赖消息队列。
6. **第二条 `raise`：`visibility_timeout` 必须大于「硬任务上限 + 30 秒」** ——**这是 Redis 队列的一个真相**：消息在 `visibility_timeout` 内没被 ack 会重新投递，而如果一个任务本身要跑 N 秒，`visibility_timeout < N` 就会导致**任务被重复执行**。**这行代码把一个「只在长时间运行后才显现」的重复执行 bug 变成了启动期错误。**
7. **只对 `redis://` / `rediss://` / `sentinel://` 检查** ——其它 broker（如 AMQP）没有这套 visibility 语义。

**`init_app` 的 `Celery(...)` 构造里有三个参数与一处不一致。**

```python
celery_app = Celery(
    app.name,
    task_cls=FlaskTask,
    broker=dify_config.CELERY_BROKER_URL,
    backend=dify_config.CELERY_BACKEND,
)
```

**注意 `backend=dify_config.CELERY_BACKEND`** ——而 `CELERY_BACKEND` 的值域是 `"database"` / `"redis"` / `"rabbitmq"`。**把它当 `backend` 传给 `Celery` 是错的**（Celery 期望的是一个 URL 或注册的后端名）。而紧接着的 `conf.update` 里又设了正确的 `result_backend=dify_config.CELERY_RESULT_BACKEND`。**于是构造期的错误值被覆盖掉。** 这是一个「靠后面的赋值修正前面」的写法，功能上无害（`conf.update` 在构造之后、使用之前），但它让 `Celery(...)` 那一行的 `backend` 参数完全是干扰。

**`conf.update` 里有两处注释值得记。**

```python
if dify_config.CELERY_BACKEND == "redis":
    celery_app.conf.update(
        result_backend_transport_options=broker_transport_options,
        # Celery keeps the result backend per thread unless it is declared thread safe. The
        # gevent pool runs every task in a new greenlet, so each task would build a new
        # backend, and building one scans the entry points of every installed package.
        # The Redis backend works through a connection pool and can be shared.
        result_backend_thread_safe=True,
    )
```

**四处：**

1. **`result_backend_transport_options=broker_transport_options`** ——**结果后端也复用 broker 的 transport 选项**（含 Sentinel 与 keyprefix）。这与 `redis_backend_use_ssl` 的做法一致（同类配置两处应用）。
2. **`result_backend_thread_safe=True` 的注释完整讲了一个性能陷阱**：Celery 默认把结果后端**按线程缓存**，而 gevent pool 每个任务跑在新 greenlet 里 → 每个任务都新建一个后端 → 而**新建后端要扫描所有已安装包的 entry points**。**这是一个「看起来只是一个对象创建，实际是一次全包扫描」的典型。**
3. **注释也解释了为什么这么修是安全的**：「The Redis backend works through a connection pool and can be shared」。**即「能共享」不是猜的，是因为底层是连接池。**
4. **只在 `CELERY_BACKEND == "redis"` 时设** ——其它后端（数据库）不一定线程安全。

第二处是：

```python
if "visibility_timeout" in broker_transport_options:
    celery_app.conf.visibility_timeout = broker_transport_options["visibility_timeout"]
```

**`visibility_timeout` 要同时出现在 `broker_transport_options` 与顶层 `conf.visibility_timeout` 两处** ——**这是 Celery 自己的一个不直观之处**：传输层选项控制 Redis 的可见性超时，而顶层配置控制 Celery 侧的 `task_time_limit` 相关行为。**「同一个值写两个地方」在这里是必需的，而不是冗余。**

**而 `beat_schedule` 的构造是「三十多个 if」的清单，其中一处带了一段值得读的注释。**

```python
if dify_config.KNOWLEDGE_FS_BACKGROUND_WORKER_ENABLED is True:
    from services.knowledge_fs.background_contract import OPERATION_QUEUES, OPERATION_TASK

    interval = dify_config.KNOWLEDGE_FS_BACKGROUND_POLL_INTERVAL_SECONDS
    for operation, queue in OPERATION_QUEUES.items():
        beat_schedule[f"knowledge_fs_background_{operation}"] = {
            "task": OPERATION_TASK,
            "schedule": timedelta(seconds=interval),
            "kwargs": {"operation": operation},
            # These are recoverable database sweeps, not document messages. Expired wakeups
            # must not replay a backlog of timer ticks after an extended worker outage.
            "options": {"queue": queue, "expires": max(30, interval * 2)},
        }
```

**四处：**

1. **这是唯一一处「循环生成多个定时任务」的地方** ——其余全是手写条目。因为这里的 operation 列表是**数据**（`OPERATION_QUEUES` 字典），不是代码。
2. **`kwargs={"operation": operation}`** ——同一个任务函数被 N 个定时条目复用，靠参数区分。**这是一个「用一个任务实现 N 个扫描」的设计**，省了 N 个函数。
3. **`options["expires"] = max(30, interval * 2)` 的注释解释了一个真实故障**：「These are recoverable database sweeps, not document messages. Expired wakeups must not replay a backlog of timer ticks after an extended worker outage.」——**worker 停机恢复后，积压的定时消息会一次性涌入**。`expires` 让过期的消息被丢弃。**这是一个「定时任务必须能丢」的原则**：**可恢复的扫描任务丢一次无所谓，下一轮会补上；而不可丢的消息（用户请求）不能加 `expires`。**
4. **`max(30, interval * 2)` 的下限 30 秒** ——给极短的轮询间隔留出余量，避免正常抖动导致消息被丢。

**同一个文件里还有另外两处 `expires`，数值风格不同。**

```python
beat_schedule["reconcile_application_traces"] = {
    "task": "tasks.application_trace_task.reconcile_application_traces",
    "schedule": timedelta(seconds=30),
    "options": {"expires": 25},
}
```

**`expires=25` 小于 `schedule=30`** ——**即「消息还没到下一轮就该过期了」**。这是一个刻意的选择：只要调度稍有延迟就丢弃，保证「任何时刻最多有一个待执行的 reconcile」。**这与 `max(30, interval*2)` 的「留两倍余量」策略相反**，说明 `expires` 的选择取决于「能不能容忍堆积」而不是某个统一公式。

**`imports` 与 `beat_schedule` 的耦合是隐式的，而代码里有一条注释提醒。**

```python
# if you add a new task, please add the switch to CeleryScheduleTasksConfig
```

**这条注释的位置在 `beat_schedule` 初始化之前** ——它要求「新增定时任务时，配置开关要加到 `CeleryScheduleTasksConfig` 里」。**也就是说：每个 `beat_schedule` 条目都应当有一个 `ENABLE_*` 开关**，而开关的声明在 `configs/feature` 里。

**对照代码，绝大多数条目确实遵循了这个约定**（`ENABLE_EVALUATION_RUN_RECONCILER`、`ENABLE_TRACE_PAYLOAD_SWEEP_TASK`、`OPS_AGENT_TRACE_ENABLED`……）。**而 `knowledge_fs_lifecycle_worker` 那四个条目用的是 `get_configured_knowledge_fs_lifecycle_worker_readiness().ready`** ——**一个运行期求值的就绪状态，而不是一个布尔开关。**

```python
from services.knowledge_fs.lifecycle_readiness import get_configured_knowledge_fs_lifecycle_worker_readiness

if get_configured_knowledge_fs_lifecycle_worker_readiness().ready:
    imports.extend([
        "tasks.knowledge_fs_initial_source_tasks",
        "tasks.knowledge_fs_lifecycle_tasks",
        "tasks.knowledge_fs_source_import_tasks",
    ])
    beat_schedule["knowledge_fs_lifecycle_worker"] = {...}
    ...
```

**三处：**

1. **`imports` 与 `beat_schedule` 被同一个条件一起改** ——**这是必需的**：若只 import 不登记定时，任务永不被调度；若只登记不 import，Celery 找不到任务名会报错。**两者必须在同一个分支里，而这一点没有任何机制保证**（它们只是两个相邻的语句）。
2. **`readiness.ready` 是「多项配置是否互相满足」的判定** ——所以它不是一个简单的开关，而是**一组约束的合取**。
3. **同一个 `readiness` 在 `i01` 的模型提供方页里也出现过类似概念（`_ProviderSummaryState`）** ——但那里的「状态」是数据驱动，这里的「就绪」是配置驱动。**两种「准备好了吗」的判据不同，而都叫 ready，容易混淆。**

**`imports` 列表里有几条带注释的「历史条目」，它们存在的原因值得单列。**

```python
"tasks.collect_agent_resources_task",  # retired Agent resource collection
"tasks.ops_trace_task",  # retired endpoint; old workers must drain pre-upgrade tasks
```

**「retired」** ——模块已经不再产生新任务，但**仍然被 import**。理由写在第二条注释里：「old workers must drain pre-upgrade tasks」。**即滚动升级期间，旧 worker 已经在队列里投递的任务必须仍能被新 worker 识别执行。**

**这是分布式升级的一个真实约束**：任务模块的移除必须比「最后一个产生该任务的进程下线」更晚。**注释把这个约束写在了 import 列表里，而不是写在某个迁移文档里** ——位置正确，因为改这行的人一定是想删它的人。

**`ext_blueprints.init_app` 的结构是「九个 register + 六处 CORS」，而 CORS 头列表是分层的。**

```python
BASE_CORS_HEADERS: tuple[str, ...] = ("Content-Type", HEADER_NAME_APP_CODE, HEADER_NAME_PASSPORT)
SERVICE_API_HEADERS: tuple[str, ...] = (*BASE_CORS_HEADERS, "Authorization")
AUTHENTICATED_HEADERS: tuple[str, ...] = (
    *SERVICE_API_HEADERS,
    HEADER_NAME_CSRF_TOKEN,
    HEADER_NAME_IDEMPOTENCY_KEY,
    HEADER_NAME_REQUEST_ID,
    "X-Trace-ID",
)
FILES_HEADERS: tuple[str, ...] = (*BASE_CORS_HEADERS, HEADER_NAME_CSRF_TOKEN)
EMBED_HEADERS: tuple[str, ...] = ("Content-Type", HEADER_NAME_APP_CODE)
EXPOSED_HEADERS: tuple[str, ...] = ("X-Version", "X-Env", "X-Trace-Id", "Content-Disposition", HEADER_NAME_CATALOG)
OPENAPI_HEADERS: tuple[str, ...] = ("Authorization", "Content-Type", HEADER_NAME_CSRF_TOKEN, HEADER_NAME_CATALOG)
```

**五处：**

1. **四层递进**：`BASE` → `SERVICE_API`（加 `Authorization`）→ `AUTHENTICATED`（再加 CSRF、幂等键、请求 id、trace id）。**每一层都是上一层加上几个头**，用 `*` 展开表达继承关系。**这是「CORS 允许头」这种列表最该有的写法**——否则六个列表会各自漂移。
2. **`EMBED_HEADERS` 与 `FILES_HEADERS` 是独立的两个小集合** ——嵌入端只需要 `Content-Type` + app code（不要凭据）；文件端点需要 CSRF 但不要 `Authorization`（因为用 cookie）。**这两个不继承 `BASE_CORS_HEADERS` 的 `HEADER_NAME_PASSPORT`**，而是 `EMBED_HEADERS` 干脆只列两个。**所以「分层」并不完整，有两个旁支。**
3. **`EXPOSED_HEADERS` 是「允许前端读取的响应头」** ——与 `allow_headers`（允许前端发送的请求头）是**方向相反的两个配置**。**代码里所有 CORS 块都设了 `expose_headers`（或通过共享元组），说明作者理解这个区别。**
4. **`OPENAPI_HEADERS` 不含 `X-Trace-ID` 与幂等键** ——但含 `HEADER_NAME_CATALOG`。**同一份头列表在 openapi 上比 console 上窄**，这是一个有意的差异（openapi 面向第三方，不给内部 trace 头）。
5. **`OPENAPI_MAX_AGE_SECONDS = 600`** ——预检请求缓存十分钟。**这个值只给了 openapi，console 与 web 没设**，于是它们用 flask-cors 的默认值。

**`_apply_cors_once` 的 docstring 说明了它为什么存在。**

```python
def _apply_cors_once(bp, /, **cors_kwargs):
    """Make CORS idempotent so blueprints can be reused across multiple app instances."""

    if getattr(bp, "_dify_cors_applied", False):
        return

    from flask_cors import CORS

    CORS(bp, **cors_kwargs)
    bp._dify_cors_applied = True
```

**五处：**

1. **`bp._dify_cors_applied` 是往蓝图对象上挂私有属性** ——一个「用属性当幂等标记」的做法。**`getattr(..., False)` 让首次调用不报错。**
2. **`/` 之后的 `*` 是「位置参数到此为止」** ——Python 3.8+ 的 positional-only 标记。**这是一个风格选择，对单参数函数没有实际作用。**
3. **函数内 `from flask_cors import CORS`** ——避免在不需要 CORS 的场景下加载这个依赖。
4. **docstring 说的是「blueprints can be reused across multiple app instances」** ——**这解释了一个真实场景**：测试里会创建多个 app，而蓝图是模块级单例。第二次 `init_app` 时若重复应用 CORS，flask-cors 会叠加处理器，导致响应头重复且难以排查。
5. **代价是标记挂在全局对象上** ——两个测试用例用**不同的** CORS 配置注册同一个蓝图时，第二个会静默沿用第一个的配置。**这是一个「幂等与灵活性」的固有冲突**，而代码选择了幂等。

**`web` 蓝图的 CORS 是「按路径三条规则」，而顺序有意义。**

```python
_apply_cors_once(
    web_bp,
    resources={
        # Embedded bot endpoints (unauthenticated, cross-origin safe)
        r"^/chat-messages$": {
            "origins": dify_config.WEB_API_CORS_ALLOW_ORIGINS,
            "supports_credentials": False,
            "allow_headers": list(EMBED_HEADERS),
            "methods": ["GET", "POST", "OPTIONS"],
        },
        r"^/chat-messages/.*": { ... },
        # Default web application endpoints (authenticated)
        r"/*": {
            "origins": dify_config.WEB_API_CORS_ALLOW_ORIGINS,
            "supports_credentials": True,
            "allow_headers": list(AUTHENTICATED_HEADERS),
            "methods": ["GET", "PUT", "POST", "DELETE", "OPTIONS", "PATCH"],
        },
    },
    expose_headers=list(EXPOSED_HEADERS),
)
```

**五处：**

1. **三条规则的关键差别是 `supports_credentials`**：嵌入端 `False`（不要 cookie）、其余 `True`（要 cookie）。**这不是权限判断，而是 CORS 协议要求**：`Access-Control-Allow-Credentials: true` 与 `Access-Control-Allow-Origin: *` 不能共存，所以「要 cookie」的端点必须给出精确来源。
2. **`^/chat-messages$` 与 `^/chat-messages/.*` 分成两条** ——**因为 flask-cors 的 `resources` 是正则匹配，而 `^/chat-messages$` 不会匹配 `/chat-messages/xxx`**。分成两条是必需的，不是冗余。
3. **`/*` 兜底规则排在最后** ——**但 flask-cors 不按顺序取第一个匹配，而是对所有匹配的规则合并**。所以兜底规则的存在意味着 `/chat-messages` 会同时匹配两条。**这里靠的是「更具体的规则后应用并覆盖」**，而这是一个依赖库实现细节的行为（注释没提这一点）。
4. **两条嵌入端规则的 `origins` 都读 `WEB_API_CORS_ALLOW_ORIGINS`** ——与认证端同一个配置源。**于是「嵌入端可跨域的来源白名单」与主应用共用一份**，无法单独收紧。
5. **`methods` 在嵌入端只有三种** ——`GET` / `POST` / `OPTIONS`。**这是一个真正的收紧**（嵌入的聊天框不需要 DELETE/PATCH）。

**而 `console` 蓝图只给一条规则，却设了 `supports_credentials=True`。**

```python
_apply_cors_once(
    console_app_bp,
    resources={r"/*": {"origins": dify_config.CONSOLE_CORS_ALLOW_ORIGINS}},
    supports_credentials=True,
    allow_headers=list(AUTHENTICATED_HEADERS),
    methods=["GET", "PUT", "POST", "DELETE", "OPTIONS", "PATCH"],
    expose_headers=list(EXPOSED_HEADERS),
)
```

**三处：**

1. **`supports_credentials=True` 在 `resources` 外层** ——**这是 flask-cors 的两种写法**：内层（逐 resource）与外层（全局默认）。上层已在外层给了 `expose_headers`，所以这里是混合用法。**同一个文件里三种写法并存**（外层、内层、内外混用），读者必须知道 flask-cors 的合并规则才能判断实际生效值。
2. **`console` 的来源白名单是全局一个值（`CONSOLE_CORS_ALLOW_ORIGINS`）** ——而它正是「有状态控制台」的正确做法：控制台只允许自家前端。
3. **没有 `max_age`** ——于是预检缓存用默认值（通常 21600 秒）。

**另外三个蓝图没有 CORS 或只有最小配置。**

```python
app.register_blueprint(inner_api_bp)
app.register_blueprint(mcp_bp)
# OAuth discovery for the knowledge MCP endpoint lives at the API root (`/.well-known/...`).
app.register_blueprint(well_known_bp)
```

**`inner_api` 与 `mcp` 完全不配 CORS** ——**这是最安全的默认**：浏览器发起的跨域请求会被拒绝，而服务间调用（用服务端 HTTP 客户端）不受 CORS 约束。**注释解释了 `well_known` 为什么挂在根路径**：OAuth 发现必须从根开始找。

**而 `trigger` 蓝图的 CORS 头列表是唯一内联写死的。**

```python
_apply_cors_once(
    trigger_bp,
    allow_headers=["Content-Type", "Authorization", "X-App-Code"],
    methods=["GET", "PUT", "POST", "DELETE", "OPTIONS", "PATCH", "HEAD"],
    expose_headers=list(EXPOSED_HEADERS),
)
```

**三处：**

1. **`allow_headers` 不复用任何常量** ——而 `"X-App-Code"` 实际上就是 `HEADER_NAME_APP_CODE`（在 `BASE_CORS_HEADERS` 里已出现过）。**同一字符串在这里被硬编码，而其它六处都用常量**。**这是一个会漂移的点**：改常量名时这一处会被漏掉。
2. **`methods` 多了 `HEAD`** ——webhook 接收端常被探活请求打，`HEAD` 需要放行。
3. **没有 `resources`、没有 `origins`** ——**于是允许任何来源**。对一个「接收第三方 webhook」的端点这是必须的（无法预知来源）。**但这也意味着任何网页都能向它发请求**，安全性全靠 `X-App-Code` + `Authorization` 的校验。

**`init_app` 的第一段是唯一一处「按配置决定是否注册蓝图」的地方。**

```python
from services.human_input_v2.im_webhook_runtime import configure_im_webhook_runtime

configure_im_webhook_runtime(app)
if dify_config.HUMAN_INPUT_IM_EVENT_TRANSPORT_MODE is IMEventTransportMode.WEBHOOK:
    from controllers.web.human_input_v2_im_webhook import bp as im_provider_webhook_bp

    app.register_blueprint(im_provider_webhook_bp)
```

**三处：**

1. **`configure_im_webhook_runtime(app)` 无条件调用** ——它在注册蓝图之前，说明它做的是「运行时就绪」而不是「路由注册」。
2. **`is IMEventTransportMode.WEBHOOK` 用 `is`** ——`IMEventTransportMode` 是枚举（`StrEnum`），所以 `is` 与 `==` 等价。**用 `is` 表明「这是一个单例比较」。**
3. **import 在 if 内部** ——所以不启用 webhook 模式下，`controllers.web.human_input_v2_im_webhook` 根本不会被加载。**这是一个有效的「不加载即不暴露」**（虽然 Flask 的路由不会因为模块被 import 就生效，少一次 import 主要是省依赖）。

## 关键取舍

**源码事实：** `FlaskTask` 定义在 `init_app` 内部以闭包捕获 `app`，每个任务在 `app.app_context()` 里执行并调用 `init_request_context()`（注释：similar to before_request in Flask），随后 `self.run(...)`。

**源码事实：** `Celery(...)` 构造时传了 `backend=dify_config.CELERY_BACKEND`（值为 `"database"`/`"redis"`/`"rabbitmq"`，不是 URL），而紧接着的 `conf.update` 用 `result_backend=dify_config.CELERY_RESULT_BACKEND` 覆盖成正确值。

**源码事实：** `get_celery_ssl_options` 先判 `BROKER_USE_SSL`，再判 broker 是否 redis，两者重叠（`BROKER_USE_SSL` 本身由 `rediss://` 前缀算出）；`cert_reqs_map` 拼错时兜底 `ssl.CERT_NONE`（不安全方向）；返回 `CelerySSLOptionsDict`（四个键，三个可为 `None`）。

**源码事实：** `get_celery_broker_transport_options` 用 `total=False` 的 `CelerySentinelTransportDict` 累积 Sentinel / `global_keyprefix` / `visibility_timeout`；`visibility` 判据排除 `bool`；`KNOWLEDGE_FS_BACKGROUND_WORKER_ENABLED is True` 时强制要求显式 `CELERY_BROKER_URL`，且对 redis/sentinel broker 要求 `visibility_timeout > 硬任务上限 + 30`，否则 `raise ValueError`。

**源码事实：** `visibility_timeout` 同时写入 `broker_transport_options` 与顶层 `conf.visibility_timeout`；`result_backend_thread_safe=True` 的注释说明 gevent pool 下每个任务新建结果后端会触发「扫描所有已安装包的 entry points」。

**源码事实：** `beat_schedule` 由三十多个 `if` 开关构造；`KNOWLEDGE_FS_BACKGROUND_WORKER_ENABLED` 分支用 `OPERATION_QUEUES` 循环生成条目并设 `expires=max(30, interval*2)`，注释说明「可恢复的数据库扫描，停机后不应重放积压的定时 tick」；`knowledge_fs_lifecycle_worker` 等四个条目由 `get_configured_knowledge_fs_lifecycle_worker_readiness().ready` 决定，且 `imports` 与 `beat_schedule` 在同一分支内一起修改。

**源码事实：** `reconcile_application_traces` 的 `expires=25` 小于其 `schedule=30`，与 `max(30, interval*2)` 的策略相反。

**源码事实：** `imports` 列表含两个已退役模块（注释：retired Agent resource collection / retired endpoint; old workers must drain pre-upgrade tasks），以及一条「if you add a new task, please add the switch to CeleryScheduleTasksConfig」的注释。

**源码事实：** CORS 头列表用 `*` 展开分层（`BASE` → `SERVICE_API` → `AUTHENTICATED`），但 `EMBED_HEADERS` 与 `FILES_HEADERS` 是独立旁支不继承 `BASE`；`EXPOSED_HEADERS` 与 `allow_headers` 是方向相反的两个配置。

**源码事实：** `_apply_cors_once` 用蓝图上的 `_dify_cors_applied` 属性做幂等（docstring：so blueprints can be reused across multiple app instances），函数内 import `flask_cors`。

**源码事实：** `web` 蓝图用三条按路径的正则规则区分嵌入端（`supports_credentials=False`、只 `GET/POST/OPTIONS`）与认证端（`supports_credentials=True`、六种方法）；`console` 用单条 `/*` + 外层 `supports_credentials=True` + `CONSOLE_CORS_ALLOW_ORIGINS`；`trigger` 的 `allow_headers` 内联写死（含硬编码的 `"X-App-Code"`）且不设 `origins`；`inner_api` / `mcp` / `well_known` 不配 CORS。

**源码事实：** `OPENAPI_MAX_AGE_SECONDS = 600` 只用于 openapi 蓝图；openapi 的 `allow_headers` 不含 `X-Trace-ID` 与幂等键。

**源码事实：** `init_app` 在注册蓝图前无条件调用 `configure_im_webhook_runtime(app)`，并仅在 `HUMAN_INPUT_IM_EVENT_TRANSPORT_MODE is IMEventTransportMode.WEBHOOK` 时 import 并注册 `im_provider_webhook_bp`。

**架构推论：** 「装配点的复杂度 = 分支数」是一条可靠的度量。这两个文件共约 570 行，其中绝大部分是 `if` 与字面量清单，而它们**没有单元测试能覆盖「所有开关组合」**（三十多个布尔的两两组合就是天文数字）。**正确的加固不是测组合，而是让每个开关的语义独立且可观察**：启动时打印「已登记 N 个定时任务 + M 个 import 模块」，并把开关状态一并输出。这样才能在「某个定时任务没跑」的故障里快速定位是「开关没开」还是「登记漏了」。

**架构推论：** `visibility_timeout` 必须大于任务时长，这条约束属于「不满足就会静默重复执行」的类型——**它不会报错，只会让某些任务跑两遍**。这里把它做成启动期 `raise` 是正确做法。主核里凡是有「超时后会重试」的配置（lease、lock TTL、heartbeat），都应当与「任务最长时长」做启动期交叉校验，因为**重复执行的 bug 与被重复执行的任务在时间上相隔很久，事后几乎无法归因**。

**架构推论：** 「退役模块仍要 import 以 drain 旧任务」揭示了一个升级纪律：**生产者的下线必须晚于消费者的升级**。这条约束最适合写在 import 列表的注释里（改这行的人正好要看），而不适合写在发布文档里（没人会在改代码时去查）。主核里删函数、删配置项、删 GUC 参数时同理。

**架构推论：** 「幂等 CORS」与「按路径分策略」这两个需求在同一处冲突：`_apply_cors_once` 保证只应用一次，而这意味着**同一蓝图无法在不同 app 上用不同策略**。这是一个可接受的取舍（生产只有一个 app），但它把「测试里用不同 CORS 配置」这条路径关掉了。**任何「为了幂等而在全局对象上打标记」的设计，都要检查它的测试可用性代价。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/extensions/ext_celery.py` | `init_app` | Celery 装配入口（任务基类、传输选项、SSL、imports、beat_schedule） |
| `api/extensions/ext_celery.py` | `FlaskTask` | 每个任务外包一层 app 上下文与日志上下文 |
| `api/extensions/ext_celery.py` | `CelerySSLOptionsDict` | Redis SSL 四字段的结构约束（`ssl_cert_reqs` 为 int，其余可为 `None`） |
| `api/extensions/ext_celery.py` | `get_celery_ssl_options` | 由配置构造 SSL 选项（证书校验要求字符串→ssl 常量映射） |
| `api/extensions/ext_celery.py` | `CelerySentinelTransportDict` | Sentinel 传输选项（`total=False`，四键可选） |
| `api/extensions/ext_celery.py` | `get_celery_broker_transport_options` | 传输选项累积 + 知识文件系统 worker 的启动期校验 |
| `api/extensions/ext_celery.py` | `get_celery_redis_global_keyprefix` | Redis 键前缀（命名空间隔离） |
| `api/extensions/ext_celery.py` | `_enqueue_initial_community_telemetry_heartbeat` | beat 启动时补发一次心跳（挂在 `beat_init` 信号上） |
| `api/extensions/ext_celery.py` | `CeleryBeatScheduleEntry` | 定时条目结构（`task` / `schedule` / 可选 `kwargs` / `options`） |
| `api/extensions/ext_blueprints.py` | `init_app` | 九个蓝图注册与六处 CORS 策略 |
| `api/extensions/ext_blueprints.py` | `BASE_CORS_HEADERS` / `SERVICE_API_HEADERS` / `AUTHENTICATED_HEADERS` | 允许请求头的三层递进 |
| `api/extensions/ext_blueprints.py` | `EMBED_HEADERS` / `FILES_HEADERS` / `OPENAPI_HEADERS` | 三条独立旁支头列表 |
| `api/extensions/ext_blueprints.py` | `EXPOSED_HEADERS` | 允许前端读取的响应头（与请求头方向相反） |
| `api/extensions/ext_blueprints.py` | `_apply_cors_once` | 蓝图级 CORS 幂等应用（蓝图对象上打标记） |
| `api/extensions/ext_blueprints.py` | `OPENAPI_MAX_AGE_SECONDS` | openapi 预检缓存 600 秒 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [Celery 文档：配置项与任务基类](https://docs.celeryq.dev/en/stable/userguide/configuration.html)
- [Redis 文档：BRPOPLPUSH 与可见性超时](https://redis.io/docs/latest/commands/blmove/)
- [flask-cors 文档：基于资源的 CORS 配置](https://flask-cors.readthedocs.io/en/latest/api.html)

## 与主核 / AI 的关系

装配层在主核（多模态数据库内核）里对应的是**启动期的 GUC 注册、后台进程启动、扩展加载**。四条经验。

**第一，装配点的分支无法用组合测试覆盖，必须让它可观测。** 这里的三十多个 `if` 决定「哪些任务被登记」，而「某任务没跑」的故障有无数成因。主核里启动后台进程（checkpointer、walwriter、autovacuum launcher）时同理：**启动日志应当明确列出「我启动了哪些、按什么配置」，而不是只在出错时说话**。可观测的装配让「配置没开」与「代码漏了」一眼可分。

**第二，「超时配置必须大于任务时长」这类约束要在启动期检查。** 这里对 `visibility_timeout` 的校验是范本：不满足就 `raise`，因为不满足的后果是**静默的重复执行**。主核里凡是「过期后会重试/重新投递」的机制（锁、租约、心跳），都该与「持有者最长工作时长」做交叉校验——**重复执行的故障在时间上远离根因，事后再也归不了因。**

**第三，退役的注册项要按「消费者晚于生产者下线」的纪律处理。** 这里保留两个 retired 模块的 import，注释写明是让旧 worker 能 drain 升级前投递的任务。主核里删一个 GUC、一个错误码、一个系统函数时同理：**删代码的人必须确认「没有任何在跑的旧实例还会产生它」**，而滚动升级让这件事的窗口比想象的宽。

**第四，「允许发送的头」与「允许读取的头」是相反方向的两个白名单，且必须分层表达。** 这里用 `*` 展开做出四层递进，但仍有两条旁支不继承基类——**说明分层一旦不完整，就会出现「同一字符串在两处」的漂移**（`trigger` 蓝图里硬编码的 `X-App-Code` 就是实例）。主核里做「权限位/能力标志的组合」时同理：**允许「精确列举」与「按层继承」混用，就必须有一处统一校验，否则漏掉的那一层不会报错，只会少一个能力。**
