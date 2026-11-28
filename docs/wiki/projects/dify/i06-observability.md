---
project: dify
feature: observability
title: 日志存储与可观测性
page_type: feature
schema_version: 1
refs:
  - path: api/extensions/logstore/aliyun_logstore.py
    symbols: [AliyunLogStore]
  - path: api/extensions/logstore/sql_escape.py
    symbols: [escape_logstore_query_value, escape_identifier]
---

# 日志存储与可观测性

## 一句话职责

`AliyunLogStore` 把工作流运行日志写进阿里云 SLS，并且**在同一个客户端里实现两套完全不同的查询协议**（PG 协议走 SQL、SDK 协议走 `key: value AND ...`），启动时按「项目是否新建 + 索引是否开启 scan_index」自动选一套；`sql_escape.py` 则是这两套协议各自的转义函数，因为**它们的转义规则完全不同**。

## 机制是什么

把「日志检索」当成一个存储后端，会遇到一个不可回避的事实：**这个后端有两套查询语言，而它们的注入面不一样。**

`sql_escape.py` 的模块 docstring 把这个前提写在第一段：

```
LogStore supports two query modes:
1. PG Protocol Mode: Uses SQL syntax with single quotes for strings
2. SDK Mode: Uses LogStore query syntax (key: value) with double quotes
```

而它的安全关切也写得很明确：

```
Key Security Concerns:
- Prevent tenant A from accessing tenant B's data via injection
- SLS queries are read-only, so we focus on data access control
- Different escaping strategies for SQL vs LogStore query syntax
```

**「SLS queries are read-only, so we focus on data access control」** ——这句划定了威胁模型：**攻击者不能删改数据，只能读到别人的数据。** 于是这次的「注入」不是「破坏」，而是**越权读**。

而两套协议对应两套转义：

- **SQL 模式**：单引号是字符串定界符，转义方式是**双写单引号**（`'` → `''`）；
- **SDK 模式**：双引号包值，**单引号是普通字符**，转义方式是**反斜杠**（`\` → `\\`、`"` → `\"`），且**顺序必须先是反斜杠**。

**这两个函数不能互用**，而一旦用错，后果不是报错而是「看起来正常的查询」——于是越权读会静默成功。

## 怎么实现的

**`escape_sql_string` 只有一行有效代码，而它的 docstring 是本页最完整的一份威胁模型文档。**

```python
def escape_sql_string(value: str) -> str:
    """..."""
    if not value:
        return value

    # Escape single quotes by doubling them (standard SQL escaping)
    # This prevents breaking out of string literals in SQL queries
    return value.replace("'", "''")
```

**四处：**

1. **`if not value: return value`** ——空串与 `None`（虽然类型是 `str`）直接返回。**注意返回的是原值**，所以 `""` 得到 `""`，而 `None` 会得到 `None`（尽管类型标了 `str`）。**调用方在 f-string 里插 `None` 会得到字符串 `"None"`，这是一个潜在的类型缺口。**
2. **双写单引号是标准 SQL 转义** ——对 PostgreSQL 与 MySQL 都成立。
3. **它不做反斜杠转义** ——因为目标方言里 `\` 不是转义字符（PostgreSQL 的 `standard_conforming_strings=on` 是默认值，MySQL 在 `NO_BACKSLASH_ESCAPES` 关闭时会把 `\` 当转义）。**SLS 的 PG 协议行为是否与标准 PostgreSQL 一致，代码没有说明**，这是一个依赖外部行为的假设。
4. **docstring 里的 `Examples` 用了 doctest 格式** ——但它们不是真的可执行 doctest（返回值没加引号，且注释写在表达式后）。**这些例子是给人读的，不是被测试跑的。**

**`escape_identifier` 是 `escape_sql_string` 的别名，而它的名字暗示了一件它做不到的事。**

```python
def escape_identifier(value: str) -> str:
    """
    Escape an identifier (tenant_id, app_id, run_id, etc.) for safe SQL use.
    ...
    Security:
        - Prevents SQL injection via identifiers
        - Stops cross-tenant access attempts
        - Works for UUIDs, alphanumeric IDs, and similar identifiers
    """
    # For identifiers, use the same escaping as strings
    # This is simple and effective for preventing injection
    return escape_sql_string(value)
```

**四处：**

1. **名字里的 "identifier" 是误导的** ——SQL 的 identifier 是表名/列名（要双引号包裹），而这里的用法全是**值**（`tenant_id = '...'`）。**它是一个「用于过滤的标识符值」的转义，不是标识符转义。**
2. **它完全等价于 `escape_sql_string`** ——所以它存在只为「在调用点表达意图」。**这有一定价值**（读代码时能看出「这里传的是 id」），代价是**多一个可被误用的名字**。
3. **docstring 说「This function is for PG protocol mode (SQL syntax). For SDK mode, use escape_logstore_query_value() instead」** ——**用交叉引用来防误用**，这是好做法。
4. **`Works for UUIDs, alphanumeric IDs, and similar identifiers`** ——这句是**范围声明**：它暗示「如果你的值不是这类形状，就要小心」。**但它没说「小心什么」**（比如含双引号或反斜杠的 id 在 PG 协议下是否安全）。

**`escape_logstore_query_value` 是本页最精细的一个函数，而它的核心是一个**顺序要求**。**

```python
def escape_logstore_query_value(value: str) -> str:
    """..."""
    if not value:
        return '""'

    # IMPORTANT: Escape backslashes FIRST, then double quotes
    # This prevents double-escaping (e.g., " -> \" -> \\" incorrectly)
    escaped = value.replace("\\", "\\\\")  # \ -> \\
    escaped = escaped.replace('"', '\\"')  # " -> \"

    # Wrap in double quotes to treat as literal string
    # This prevents and/or/not/:/() from being interpreted as operators
    return f'"{escaped}"'
```

**五处：**

1. **先转义反斜杠、再转义双引号** ——顺序颠倒会双重转义。docstring 末尾专门有 `Note: Escape order is critical: backslash first, then double quotes. Otherwise, we'd double-escape the escape character itself.` **这是本页唯一显式记录「顺序敏感」的地方，而这类顺序错误的表现是「某些值查不到」而非报错。**
2. **空值返回 `'""'`（两个引号字符）而不是空串** ——**因为返回值包含定界引号**。于是「空值」在查询里变成 `key: ""`，语义是「该字段为空串」。**这是一个具体的语义决定：空输入不等于「不加过滤」。**
3. **返回值**带引号**（docstring：includes the quotes）** ——调用方**不能**再自己加引号。**这个约定是隐式的**（靠 docstring 的一句话），而一旦调用方误加会得到 `key: ""value""`。
4. **包双引号的目的是让 `and` / `or` / `not` / `:` / `()` 变成字面量** ——docstring 的 `Security` 段列了三条「Prevents injection via ...」，分别对应这三类。**这是一种「不枚举黑名单，而是整体加引号」的防御**：只要定界符正确，内部什么都不需要单独处理（除了转义字符本身）。
5. **它不处理单引号** ——因为 SDK 语法里单引号是普通字符（docstring 规则 2）。**这与 `escape_sql_string` 正好互补**，也是「两个函数不能互用」的根本原因。

**而这两个函数在真实调用点上是**成对出现**的——同一个过滤条件要写两遍。**

```python
# logstore_api_workflow_node_execution_repository.py
sql_filters = " AND ".join(f"{key} = '{escape_identifier(value)}'" for key, value in filter_values.items())
# 同一批 filter_values 再用 SDK 语法渲染一遍：
f"{key}: {escape_logstore_query_value(value)}" for key, value in filter_values.items()
```

**四处：**

1. **同一个 `filter_values` 被渲染两次** ——一次给 SQL、一次给 SDK。**这是「两套协议」在调用层的直接代价：每个查询都要写两份表达式。**
2. **两个连接符的大小写不同** ——SQL 用 `" AND "`（大写），SDK 用 `" and "`（小写）。**功能上等价（SDK 的关键字大小写不敏感，docstring 规则 1），但风格不一致**，说明两处是分别手写的。
3. **`=` 与 `:` 是两种语法的赋值** ——SQL 用 `key = 'value'`，SDK 用 `key: "value"`。
4. **SQL 侧的引号由调用点加（`'{escaped}'`），SDK 侧由函数加** ——**两个函数的返回约定不同**：`escape_identifier` 返回裸值、`escape_logstore_query_value` 返回带引号的值。**这个不对称是最容易出错的地方**：读者需要记住「哪个带引号」。

**而同一文件里对 `triggered_from` 的处理暴露了这个不对称的后果。**

```python
# logstore_api_workflow_run_repository.py
triggered_from_filter = " OR ".join(
    [
        f"triggered_from='{escape_sql_string(tf.value if isinstance(tf, WorkflowRunTriggeredFrom) else tf)}'"
        for tf in triggered_from_list
    ]
)
```

**三处：**

1. **这里用的是 `escape_sql_string` 而不是 `escape_identifier`** ——因为 `triggered_from` 是一个枚举值而非 id。**但这个区分只在作者的心智里**：两个函数实现完全相同，选哪个都不影响结果。
2. **`tf.value if isinstance(tf, WorkflowRunTriggeredFrom) else tf`** ——**兼容枚举与裸字符串两种输入**。于是「调用方传了字符串」与「传了枚举」都能工作，代价是这个判断在多个地方重复。
3. **`'...'` 由 f-string 加** ——与 `escape_identifier` 的调用点一致（都自己加单引号）。

**`AliyunLogStore` 是一个「双检查锁式单例」，而它的实现有一个微妙的缺陷。**

```python
class AliyunLogStore:
    """
    Singleton class for Aliyun SLS LogStore operations.

    Ensures only one instance exists to prevent multiple PG connection pools.
    """

    _instance: AliyunLogStore | None = None
    _initialized: bool = False

    def __new__(cls) -> AliyunLogStore:
        """Implement singleton pattern."""
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __init__(self) -> None:
        # Skip initialization if already initialized (singleton pattern)
        if self.__class__._initialized:
            return
        ...
        self.__class__._initialized = True
```

**五处：**

1. **`__new__` + `__init__` 两道检查是 Python 单例的标准写法** ——`__new__` 保证同一实例，`__init__` 保证只初始化一次。
2. **docstring 说明了单例的动机：「to prevent multiple PG connection pools」** ——**不是「省内存」，而是「避免多个连接池」**。这是一个具体的资源约束，而不是习惯性的模式使用。
3. **`_initialized` 是类属性，用 `self.__class__._initialized` 读写** ——于是子类共享同一个标记。**若有人继承这个类，会得到「父类初始化过所以子类不再初始化」的行为。**
4. **它没有加锁** ——两个线程同时走 `__new__` 可能都看到 `_instance is None`。**在 CPython 的 GIL 下这个窗口极小但不为零**（`super().__new__(cls)` 可能触发 GC 让出）。**代码没有注释说明这一点，所以无法判断是「接受风险」还是「没想到」。**
5. **`__init__` 里调用了 `load_dotenv()`** ——**这意味着一句「读 .env 文件」的副作用发生在一个「构造对象」的动作里。** 而 `load_dotenv()` 在没有 `.env` 时静默无操作。

**`__init__` 里最有价值的一处是「端点连通性预检」。**

```python
# Get timeout configuration
check_timeout = int(os.environ.get("ALIYUN_SLS_CHECK_CONNECTIVITY_TIMEOUT", 30))

# Pre-check endpoint connectivity to prevent indefinite hangs
self._check_endpoint_connectivity(self.endpoint, check_timeout)
```

**而 `_check_endpoint_connectivity` 的 docstring 说明了原因：**

```python
"""Check if the SLS endpoint is reachable before creating LogClient.
Prevents indefinite hangs when the endpoint is unreachable.
```

**「Prevents indefinite hangs」** ——**这是对「第三方 SDK 在端点不可达时会无限阻塞」的处理。** 而它的做法是**在构造客户端之前先自己做一次连通性检查**。

**这是一个值得记的模式**：当第三方客户端没有可靠的超时配置时，**用一次自己的探活把「无限等待」变成「快速失败」**。代价是多一次网络往返（只在启动时发生一次）。

**而 `self.log_enabled` 把两个不同的开关合并成一个。**

```python
self.log_enabled: bool = (
    os.environ.get("SQLALCHEMY_ECHO", "false").lower() == "true"
    or os.environ.get("LOGSTORE_SQL_ECHO", "false").lower() == "true"
)
```

**三处：**

1. **`SQLALCHEMY_ECHO` 被复用来控制日志存储的 SQL 打印** ——因为它们都是「我要看 SQL」。**这是一个合理的复用**（运维心智里它们是同一件事），但它让「只想看数据库 SQL、不想看 SLS SQL」变得不可能。
2. **每个 `if self.log_enabled:` 块里都手写 `logger.info`** ——于是这一个开关控制的是**十几处分散的日志语句**。**没有任何集中的「日志格式化」层**，所以格式（`[LogStore-SDK] PUT_LOG | logstore=%s | ...`）是逐处手写的。
3. **`.lower() == "true"`** ——**只接受 `"true"`（任意大小写）**，`"1"` / `"yes"` / `"TRUE "`（带空格）都不行。**这是一个容易踩的配置坑，而代码没有兜底。**

**`init_project_logstore` 是本页最核心的一段，因为它决定了「用哪套协议」。**

```python
def init_project_logstore(self):
    """Initialize project, logstore, index, and PG connection."""
    # Step 1: Ensure project and logstore exist
    project_is_new = False
    if not self.is_project_exist():
        self.create_project()
        project_is_new = True

    self.create_logstore_if_not_exist()

    # Step 2: Initialize PG client and connection (if enabled)
    if not self.pg_mode_enabled:
        logger.info("PG mode is disabled. Will use SDK mode.")
        return
    ...
    # Step 3: Establish PG connection based on project status
    if project_is_new:
        # For newly created projects, schedule delayed PG connection
        self._use_pg_protocol = False
        logger.info("Using SDK mode for project %s (newly created)", self.project_name)
        if self.__class__._pg_connection_timer is not None:
            self.__class__._pg_connection_timer.cancel()
        self.__class__._pg_connection_timer = threading.Timer(
            self.__class__._pg_connection_delay,
            self._delayed_pg_connection_init,
        )
        self.__class__._pg_connection_timer.daemon = True  # Don't block app shutdown
        self.__class__._pg_connection_timer.start()
    else:
        # For existing projects, attempt PG connection immediately
        self._attempt_pg_connection_init()
```

**七处：**

1. **「新建项目」与「已有项目」走两条完全不同的路** ——新建的**先用 SDK 模式，90 秒后再试 PG**；已有的**立刻试 PG**。
2. **为什么要延迟 90 秒？** 注释只写了「delayed PG connection for newly created projects」，而 `_delayed_pg_connection_init` 的 docstring 说「3 minutes after project creation」（**与 `_pg_connection_delay = 90` 不符**）。**代码是 90 秒、注释写 3 分钟，两者矛盾。** 合理的解释是「新建项目的 PG 端点需要一段时间才生效」，但真实值只能从代码看。
3. **`_pg_connection_timer` 是**类属性**（`self.__class__._pg_connection_timer`）** ——于是**全局只有一个定时器**。新建项目时会先 `cancel()` 旧的。**对单例来说这等价，但它说明作者考虑过「多次调用 `init_project_logstore`」的情况。**
4. **`daemon = True` 的注释是「Don't block app shutdown」** ——**这是必需的**：非 daemon 线程会让 Python 进程在退出时挂住 90 秒。
5. **`_use_pg_protocol = False` 在调度定时器之前显式设置** ——于是新建项目期间的所有查询走 SDK。
6. **`pg_mode_enabled` 为假时提前 `return`** ——**注意这一句在 `create_logstore_if_not_exist()` 之后**，所以「项目与库的创建」不受 PG 开关影响。
7. **`_attempt_pg_connection_init` 里失败只打日志**（`logger.info("Using SDK mode ...")` + `logger.debug("PG connection details", exc_info=True)`）——**不抛异常**。理由明确：PG 协议是**性能优化**而非功能前提。**这是一个正确的降级方向。**

**`_check_and_disable_pg_if_scan_index_disabled` 揭示了 PG 协议的一个硬依赖。**

```python
def _check_and_disable_pg_if_scan_index_disabled(self) -> None:
    """Check if scan_index is enabled for all logstores.
    If any logstore has scan_index=false, disable PG protocol.

    This is necessary because PG protocol requires scan_index to be enabled.
    """
    logstore_name_list = [
        AliyunLogStore.workflow_execution_logstore,
        AliyunLogStore.workflow_node_execution_logstore,
    ]

    for logstore_name in logstore_name_list:
        existing_config = self.get_existing_index_config(logstore_name)
        if existing_config and not existing_config.scan_index:
            logger.info("Logstore %s requires scan_index enabled, using SDK mode for project %s", ...)
            self._use_pg_protocol = False
            if self._pg_client:
                self._pg_client.close()
                self._pg_client = None
            return
```

**四处：**

1. **它把「PG 连接已建立成功」这个结果**撤销**掉** ——先连上、再检查、不合格就关掉并把客户端置 `None`。**这是「先乐观后回退」的顺序**，比「先检查再连」简单（检查本身可能要客户端）。
2. **检查对象是**硬编码的两个库名**（`workflow_execution` / `workflow_node_execution`）** ——于是新增一个 logstore 必须回来加。**这是一个「注册表写死在函数里」的例子。**
3. **`existing_config and not existing_config.scan_index`** ——**`existing_config` 为 `None`（索引还没建）时不算失败**。于是「没索引」会放行 PG，而实际查询会在遇到索引缺失时另外失败。
4. **`return` 而不是 `break`** ——找到一个不合格的就整体放弃，不再检查其余。**语义正确**（一个不够就全不用 PG）。

**`get_logs` 与 `execute_sql` 的分派是这一层的运行时核心。**

```python
def put_log(self, logstore: str, contents: Sequence[tuple[str, str]]) -> None:
    # Route to PG or SDK based on protocol availability
    if self._use_pg_protocol and self._pg_client:
        self._pg_client.put_log(logstore, contents, self.log_enabled)
    else:
        log_item = LogItem(contents=contents)
        request = PutLogsRequest(project=self.project_name, logstore=logstore, logitems=[log_item])
        ...
```

```python
def execute_sql(self, sql, logstore=None, query="*", from_time=None, to_time=None, power_sql=False) -> list[dict]:
    ...
    # Route to PG or SDK based on protocol availability
    if self._use_pg_protocol and self._pg_client:
        # PG mode: execute SQL directly (ignore query parameter)
        return self._pg_client.execute_sql(sql, logstore, self.log_enabled)
    else:
        # SDK mode: combine query and sql as "query | sql"
        full_query = f"{query} | {sql}"

        # Provide default time range if not specified
        if from_time is None:
            from_time = 0

        if to_time is None:
            to_time = int(time.time())  # now
        ...
```

**六处：**

1. **`self._use_pg_protocol and self._pg_client` 是双条件** ——**即使标志为真，客户端也可能被 `_check_and_disable...` 置 `None`。** 这个双检查防的正是那个状态。
2. **PG 模式**忽略** `query` 参数** ——docstring 明确写了：「Only used in SDK mode. PG mode ignores this parameter.」**于是调用方传了的过滤条件在 PG 模式下会消失。** 调用方必须把过滤写进 `sql` 里，否则在两种模式下行为不同。
3. **SDK 模式把 `query | sql` 拼成一个字符串** ——**`|` 是 SLS 的「先过滤再聚合」语法**。而 `query` 默认是 `"*"`（全部），于是默认行为是「对全部日志执行 SQL」。
4. **`from_time` 默认 0、`to_time` 默认 `int(time.time())`** ——**即「从最早到现在」。** 这是一个危险的默认值：**一次没带时间范围的查询会扫描整个保留期**（`logstore_ttl` 默认 365 天）。而 docstring 只是「Provide default time range if not specified」。
5. **`power_sql` 参数被接受但**从未使用**** ——函数签名里有，函数体里没有任何引用。**这是一个死参数**：调用方传了也没有效果。
6. **错误处理统一是「记完整上下文 + `raise`」** ——`logstore` / `query` / `errorCode` / `errorMessage` / `requestId` 五个字段。**`requestId` 是关键：它是找阿里云支持的唯一凭据。**

**`execute_sql` 的 `logstore` 参数是 `str | None` 但立刻校验非空。**

```python
if not logstore:
    raise ValueError("logstore parameter is required for execute_sql")
```

**「类型允许 `None` 但运行期必填」是一个可避免的签名问题** ——把参数标成 `str` 就能让类型检查器拦住。**而保留 `None` 可能只有一个理由：让调用方能延迟求值再传，但这不是类型系统的语义。**

**索引配置的生成是这一层最有意思的「自动化」部分。**

```python
@staticmethod
def _generate_index_keys_from_model(model_class: type[DeclarativeBase]) -> dict[str, IndexKeyConfig]:
    """Automatically generate LogStore field index configuration from SQLAlchemy model.

    This method introspects the SQLAlchemy model's column definitions and creates
    corresponding LogStore index configurations. When the PG schema is updated via
    Flask-Migrate, this method will automatically pick up the new fields on next startup.
    """
```

**docstring 的最后一句是这套设计的关键承诺：**

> When the PG schema is updated via Flask-Migrate, this method will automatically pick up the new fields on next startup.

**即「数据库迁移改了模型，logstore 的索引配置会在下次启动时自动跟上」。**

**这是一个「用模型作为唯一真相源」的做法**，收益是「不必手写两份 schema」，代价是**类型映射必须是保守的**：

```python
# Integer types -> long
if isinstance(column_type, (sa.Integer, sa.BigInteger, sa.SmallInteger)):
    return "long"
# Float types -> double
if isinstance(column_type, (sa.Float, sa.Numeric)):
    return "double"
# String and Text types -> text
if isinstance(column_type, (sa.String, sa.Text)):
    return "text"
# DateTime -> text (stored as ISO format string in logstore)
if isinstance(column_type, sa.DateTime):
    return "text"
# Boolean -> long (stored as 0/1)
if isinstance(column_type, sa.Boolean):
    return "long"
# JSON -> json
if isinstance(column_type, sa.JSON):
    return "json"
# Default to text for unknown types
return "text"
```

**四处：**

1. **`Integer` 全部映射成 `long`** ——包括 `SmallInteger`。**这意味着「用一个 int 当布尔」的列同样能工作。**
2. **`DateTime -> text` 带注释「stored as ISO format string」** ——**这是一个关键约束**：时间在 logstore 里是字符串，所以**时间比较是字符串比较**。而 `logstore_api_workflow_run_repository` 里有一句对应的注释：

```python
# started_at is stored as naive UTC isoformat, so string comparison orders it chronologically.
since = escape_sql_string(created_since.isoformat())
```

**「naive UTC isoformat 的字符串比较等价于时间比较」** ——这成立是因为 ISO 8601 的零填充让字典序等于时间序，而**「naive」保证了不会有 `+00:00` 这样的后缀破坏字典序**。**这是一个正确但脆弱的前提**：一旦某个写入方存了带时区的 isoformat，字符串比较就会乱序。

3. **`Boolean -> long（0/1）`** ——不用 `bool` 索引类型（SLS 可能没有），而用整数编码。
4. **`default: text`** ——**未知类型默认成 `text` 而不是报错**。这保证了「模型加了新列不会让启动失败」，代价是「一个本该是数值的列被当文本索引，范围查询会按字符串比较」。**这是本页最值得记的一个取舍。**

**而两个 logstore 的索引键各有专门方法覆盖。**

```python
def _get_workflow_execution_index_keys(self) -> dict[str, IndexKeyConfig]:
def _get_workflow_node_execution_index_keys(self) -> dict[str, IndexKeyConfig]:
```

**为什么要两者？** 因为**自动生成只覆盖「与模型同名的列」**，而 logstore 里的记录还有额外字段（`log_version` 之类由写入方加的），以及某些列需要特殊的 tokenizer 或 `scan_index` 设置。**于是「自动生成 + 手工覆盖」是这个文件的实际形态。**

**`_merge_index_configs` 与 `ensure_index_config` 承担「已有索引的增量更新」。**

那一段（561–740 行）的核心逻辑是：**读现有配置、与需要的配置比对、只对缺失或类型不符的键做更新、全对则什么都不做。** 而它的日志把结论说得很清楚：

```python
logger.info(
    "Logstore %s: Index already contains all %d Dify-required fields with correct types, no update needed",
    logstore_name,
    len(required_keys),
)
```

**「no update needed」** ——**说明索引更新是一件「可能破坏线上查询」的操作**（改索引类型可能触发重建），所以「能不动就不动」。这是一个正确的保守方向。

**最后值得指出：`DEFAULT_TOKEN_LIST` 里有两个看起来相同的字符。**

```python
DEFAULT_TOKEN_LIST = [
    ",",
    " ",
    '"',
    '"',
    ...
]
```

**前两个引号（`"` 与 `"`）在源码里看起来一样，但它们大概率是 ASCII 双引号与全角引号（U+201C/U+201D）之一。** 从注释「Common delimiters: comma, space, quotes, punctuation...」看，意图是**同时支持半角与全角引号**（中文日志里常见全角）。**这类「视觉上不可分辨」的字符在代码里必须靠注释或转义说明**，否则下一个编辑者会以为是重复项而删掉一个。

## 关键取舍

**源码事实：** `sql_escape.py` 的模块 docstring 声明 SLS 有两种查询模式（PG 协议用 SQL 单引号、SDK 模式用 `key: value` 双引号），安全关切是「防止租户 A 注入读到租户 B 的数据」，并说明「SLS queries are read-only, so we focus on data access control」。

**源码事实：** `escape_sql_string` 用双写单引号（`'` → `''`），空值原样返回（包括 `None`，尽管类型标 `str`）；`escape_identifier` 完全等价于它，仅提供语义命名，docstring 交叉引用 SDK 模式应改用 `escape_logstore_query_value`；`escape_logstore_query_value` 先转义反斜杠再转义双引号（顺序敏感，docstring 专门 `Note` 说明），空值返回两个引号字符，**返回值自带双引号**。

**源码事实：** 两个转义函数的返回约定不对称：`escape_identifier` / `escape_sql_string` 返回裸值（调用点自己加 `'...'`），`escape_logstore_query_value` 返回自带 `"..."` 的值；同一过滤条件在 repository 里被渲染两遍（SQL 用 `" AND "` + `= '...'`，SDK 用 `" and "` + `: "..."`）。

**源码事实：** `triggered_from` 的过滤用 `escape_sql_string` 且用 `tf.value if isinstance(tf, WorkflowRunTriggeredFrom) else tf` 兼容枚举与字符串两种输入。

**源码事实：** `AliyunLogStore` 是 `__new__` + `__init__` 双检单例，动机（docstring）是「prevent multiple PG connection pools」，无锁；`_initialized` 是类属性且通过 `self.__class__` 读写。

**源码事实：** `__init__` 里调用 `load_dotenv()`，从 `os.environ` 读九项配置；`log_enabled` 由 `SQLALCHEMY_ECHO` 或 `LOGSTORE_SQL_ECHO` 任一为 `"true"`（大小写不敏感、不接受 `1`/`yes`）决定；构造 `LogClient` 前先 `_check_endpoint_connectivity`（docstring：Prevents indefinite hangs when the endpoint is unreachable），并在 user agent 里拼入 `Dify-<version>`。

**源码事实：** `init_project_logstore` 对「新建项目」先用 SDK 模式并挂一个 `_pg_connection_delay = 90` 秒的 `threading.Timer`（`daemon=True`）延迟再试 PG，对「已有项目」立即 `_attempt_pg_connection_init`；`_delayed_pg_connection_init` 的 docstring 写的是「3 minutes after project creation」，与代码的 90 秒不一致；PG 连接失败只打 `info` + `debug(exc_info=True)`，不抛异常。

**源码事实：** `_check_and_disable_pg_if_scan_index_disabled` 在 PG 连接成功后再检查两个硬编码 logstore 的 `scan_index`，任一为 `False` 则撤销 PG（关闭客户端并置 `None`）；`existing_config is None`（索引不存在）时放行。

**源码事实：** `put_log` / `execute_sql` 都以 `self._use_pg_protocol and self._pg_client` 双条件分派；PG 模式**忽略** `query` 参数（docstring 明确）；SDK 模式把 `query | sql` 拼成一条；`from_time` 默认 0、`to_time` 默认当前时间；`execute_sql` 的 `power_sql` 参数被接受但从未使用；`logstore` 标成 `str | None` 却在开头 `raise ValueError` 校验非空。

**源码事实：** 所有 SLS 调用失败都记 `logstore` / `query` / `errorCode` / `errorMessage` / `requestId` 后 `raise`。

**源码事实：** `_generate_index_keys_from_model` 用 `__mapper__.columns` 内省 SQLAlchemy 模型并映射类型（Integer→long、Float/Numeric→double、String/Text→text、DateTime→text、Boolean→long、JSON→json、未知→text）；docstring 承诺「Flask-Migrate 更新 schema 后，下次启动自动吸收新字段」。

**源码事实：** `DateTime → text` 配合 repository 里的注释「started_at is stored as naive UTC isoformat, so string comparison orders it chronologically」，说明时间过滤依赖 ISO 8601 零填充 + naive 前提下字典序等于时间序。

**源码事实：** `_get_workflow_execution_index_keys` / `_get_workflow_node_execution_index_keys` 为两个 logstore 提供手工覆盖；`_merge_index_configs` / `ensure_index_config` 只对缺失或类型不符的键更新，全对时打「Index already contains all %d Dify-required fields with correct types, no update needed」。

**源码事实：** `DEFAULT_TOKEN_LIST` 中包含两个视觉上几乎相同的引号字符（半角与全角）。

**架构推论：** 「两套查询协议」不是可以被抽象掉的复杂度，因为**它们的转义规则相反**（单引号 vs 双引号、是否用反斜杠）。当前实现用「两个函数 + 调用点各写两遍表达式」来应对，代价是**每个查询都有两条可能走错的路**。这类系统的正确加固是**让「哪套协议」不进入调用方视野**：由一层构造器同时接收结构化条件并渲染成两种字符串。当前代码把渲染下放到 repository 的 f-string 里，于是「新增一个过滤条件要写两遍，且两遍都可能漏转义」。

**架构推论：** 「read-only 后端仍要防注入」是一个具体而常被忽略的威胁模型：攻击者不破坏数据，只读别人的数据。这决定了防御目标是**身份约束不能被扩大**——所以 `tenant_id` 的转义是安全关键，而 `status` / `triggered_from` 的转义只是正确性问题。**区分这两类，才能把测试重点放在对的地方**（`test_sql_escape.py` 里大量用例正是打 `tenant_id`）。

**架构推论：** 「未知 SQLAlchemy 类型默认成 text」是**用正确性换可用性**的决定。它保证「模型加列不会让启动失败」，代价是「一个数值列被当文本索引，范围查询按字典序比较，于是 `10 < 9`」。**这类默认值应当配一条启动期告警**（「以下列的类型未被识别，已按 text 索引」），否则它会以「某些范围查询结果诡异」的形式在很久以后暴露。

**架构推论：** 「先连上再撤销」的 `scan_index` 检查与「延迟 90 秒重试」的定时器，共同说明**这个后端的就绪状态是随时间变化的**。于是「启动时用的是哪种协议」不是一个可以缓存的静态事实——而代码里 `_use_pg_protocol` 一旦变真就不再回退（除了 `scan_index` 那条撤销路径）。**对「会自愈的依赖」，正确的做法是让「健康状态」显式可查并周期复核**，而不是一次性判定。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/extensions/logstore/sql_escape.py` | `escape_sql_string` | SQL 单引号双写转义（PG 协议模式） |
| `api/extensions/logstore/sql_escape.py` | `escape_identifier` | 语义化别名（等同上一个；用于租户/应用/运行 id） |
| `api/extensions/logstore/sql_escape.py` | `escape_logstore_query_value` | SDK 模式转义（先反斜杠后双引号，返回自带引号的值） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore` | SLS 客户端单例（PG 协议 + SDK 双模式） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore.__new__` / `__init__` | 双检单例与九项环境配置读取（含 `load_dotenv`） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore._check_endpoint_connectivity` | 构造客户端前的探活（防无限阻塞） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore.init_project_logstore` | 项目/库/索引初始化与协议选择（新建延迟 90 秒、已有立即） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore._attempt_pg_connection_init` | 尝试 PG 连接（失败静默降级 SDK） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore._check_and_disable_pg_if_scan_index_disabled` | `scan_index` 缺失时撤销 PG 协议 |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore.put_log` | 写入日志（PG / SDK 分派） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore.get_logs` | SDK 模式取日志（带完整调用与结果日志） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore.execute_sql` | SQL 聚合（PG 直接执行；SDK 拼 `query \| sql`） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore._sqlalchemy_type_to_logstore_type` | SQLAlchemy 类型 → 索引类型（未知默认 text） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore._generate_index_keys_from_model` | 从 ORM 模型内省生成索引配置 |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore._get_workflow_execution_index_keys` / `_get_workflow_node_execution_index_keys` | 两个 logstore 的手工索引覆盖 |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore.ensure_index_config` / `_merge_index_configs` | 索引增量合并（全对则不更新） |
| `api/extensions/logstore/aliyun_logstore.py` | `AliyunLogStore.DEFAULT_TOKEN_LIST` | 默认分词定界符（含半角与全角引号） |
| `api/extensions/logstore/repositories/logstore_api_workflow_run_repository.py` | 双表达式渲染点 | 同一过滤条件分别用 SQL 与 SDK 语法渲染 |
| `api/extensions/logstore/repositories/logstore_api_workflow_node_execution_repository.py` | `sql_filters` / `escape_logstore_query_value` | 两套过滤表达式的成对构造（`sql_filters` 是局部变量，SDK 侧是内联推导式） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [阿里云 SLS 文档：查询语法](https://help.aliyun.com/zh/sls/user-guide/query-syntax)
- [阿里云 SLS 文档：SQL 分析](https://help.aliyun.com/zh/sls/user-guide/sql-analysis)
- [OWASP 防注入速查表](https://cheatsheetseries.owasp.org/cheatsheets/Injection_Prevention_Cheat_Sheet.html)

## 与主核 / AI 的关系

可观测性存储在主核（多模态数据库内核）里对应的是**日志收集与外部查询接口**（`pg_stat_*` 视图、日志文件、外部监控导出）。四条经验。

**第一，同一后端的两套查询语言必须由一层构造器统一渲染，不能下放到调用点。** 这里的每个过滤条件都在 repository 的 f-string 里写两遍，且两个转义函数的返回约定不同（一个带引号一个不带）。主核里凡是有「同一查询要同时服务内部执行与外部协议」（例如 SQL 与某种 RPC 过滤语法）时同理：**让调用方同时正确写出两种语法，等价于让每个新条件都有两次出错机会。**

**第二，「只读后端仍要防注入」这一威胁模型要显式写下来。** 这里的模块 docstring 直接说明「SLS queries are read-only, so we focus on data access control」——于是测试重点落在 `tenant_id` 而不是 `status`。主核里做「只读副本」「审计视图」「指标导出接口」时同理：**只读不意味着安全，它只是把威胁从「破坏」换成「越权读」。**

**第三，「未知类型默认成最宽松的映射」必须配一条启动期告警。** `DateTime → text` 是为了「模型加列不失败」，而它让时间过滤退化成字符串比较，能否正确完全依赖「存的是 naive UTC ISO 8601」这一未强制的前提。主核里做「外部格式映射」（日志字段、指标名、扩展元数据）时同理：**默认值让系统能跑，但会让错误以「某些查询结果诡异」的形式延迟暴露。告警是把延迟暴露变成即时暴露的最低成本手段。**

**第四，「依赖会自愈」时协议选择不能是一次性判定。** 这里用「新建项目延迟 90 秒重试」处理「新端点需要时间生效」，而状态一旦变真就不再回望。主核里对待「备库追上」「扩展加载完成」「缓存预热完成」这类**会随时间改变的就绪状态**时同理：**把就绪做成可查询 + 周期复核的状态，而不是启动时的一次分支**，否则「启动顺序稍有不同就永久降级」会成为一类难以复现的故障。
