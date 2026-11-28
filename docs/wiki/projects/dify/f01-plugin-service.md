---
project: dify
feature: plugin-service
title: 插件服务与守护进程通信
page_type: feature
schema_version: 1
refs:
  - path: api/core/plugin/plugin_service.py
    symbols: [PluginService, _RedisLock]
  - path: api/core/plugin/impl/base.py
    symbols: [BasePluginClient]
---

# 插件服务与守护进程通信

## 一句话职责

把「插件跑在另一个进程里」这件事收成一套 HTTP 内网接口：`BasePluginClient` 负责把调用打成请求、把回包拆成数据或异常，`PluginService` 负责在这层之上管理插件生命周期与租户级缓存——**插件不是本进程的扩展，而是必须通过网络协商的外部服务**。

## 机制是什么

dify 的插件架构与「同进程 import 一个包」有本质区别：

| 维度 | 同进程插件 | dify 的做法 |
|---|---|---|
| 运行位置 | 宿主进程内 | 独立守护进程（plugin daemon） |
| 调用方式 | 函数调用 | 内网 HTTP，带 `X-Api-Key` |
| 能力来源 | 直接 import 类 | 守护进程按 `plugin_unique_identifier` 分发 |
| 故障形态 | 异常/崩溃 | 连接失败、超时、非零 `code`、空 `data` |
| 版本共存 | 装两个版本会冲突 | 各租户各插件各自解析 |

所以这层的核心工作不是「调用插件」，而是**把远程调用的不确定性翻译成本进程能处理的异常类型**。`BasePluginClient` 的 472 行里有三分之二在做这件事：路径校验、超时协商、错误类型映射、包封解包。

`PluginService` 站在更上一层，管三件事：插件安装/卸载/升级的编排、插件市场元数据的短 TTL 缓存、以及**租户级模型提供者元数据的带代际号的分布式缓存**。模块 docstring 明确写了所有权：「all cache mutations for plugin-owned provider metadata stay tenant-scoped and in one place」——缓存失效必须和插件生命周期变更在同一个类里，否则装完插件后别人还在读旧列表。

## 怎么实现的

**路径校验是防注入的第一道，而且防的是「编码绕过」。** `_prepare_request` 里三个动作依次做：

1. 长度上限 `PLUGIN_DAEMON_MAX_PATH_LENGTH`（4096）；
2. **最多解码 `PLUGIN_DAEMON_MAX_PATH_DECODE_DEPTH`（8）次**，用 `for ... else` 表达：每次 `unquote`，直到解码结果与输入相同才 `break`；若 8 次仍未收敛则 `for` 正常走完、进 `else` 抛「path is too deeply encoded」；
3. 在**解码后**的路径上按 `/` 切段，任何一段等于 `..` 就拒绝。

这个顺序是对的：先解码再判 `..`，否则 `%2e%2e` 能绕过。而限制解码次数是为了防「超长多层编码」这种 CPU 消耗型攻击。**这是把路径安全做成可判定条件，而不是靠黑名单正则。**

**超时配置接受三种形态，用 `match` 展开。** `PLUGIN_DAEMON_TIMEOUT` 允许 `None`（不超时）、`httpx.Timeout` 对象（细粒度控制）、或数值（统一超时）。模块导入时用 `match` 语句一次性归一成 `httpx.Timeout | None`。之上还叠了一个 `ContextVar` 覆盖机制：`use_plugin_daemon_request_timeout` 是 contextmanager，临时把当前上下文的超时换短（用于「这个调用必须在 N 秒内返回」的场景），退出时 `reset(token)` 恢复。

**用 ContextVar 而不是参数传递，是为了不改所有调用点的签名**——插件客户端的方法都在深层调用栈里，逐个加 `timeout` 参数会污染整条链路。代价是**超时变成隐式上下文**：读代码时看不到它从哪来。

**HTTP 客户端是模块级单例，且 `trust_env=False`。** `get_pooled_http_client("plugin_daemon", ...)` 里 `max_keepalive_connections=50`、`max_connections=100`，并且**明确禁用环境变量代理**。这一条很关键：插件守护进程是内网地址，若继承 `HTTP_PROXY` 就会被代理转发出去，既影响性能也可能是信息泄露。**内网调用必须绕开环境代理，这是默认值最容易出错的地方。**

**回包处理分四层，每层失败都有专属异常。**

`_request_with_plugin_daemon_response` 的结构是：

1. `raise_for_status()` 失败 → 按 `status_code < 500` 分成 `PluginDaemonClientSideError` 与 `PluginDaemonInternalServerError`；
2. 其它异常 → 统一 `ValueError`（带 url，不带原始堆栈语义）；
3. 解析失败 → `ValueError`（带 type 名与 url）；
4. **`rep.code != 0` → 走错误分派**；`rep.data is None` → `ValueError`「got empty data from plugin daemon」。

第 4 层里的 `rep.data is None` 判断值得单看：它说明守护进程会用 `code=0` 配 `data=null` 表示「成功但没有内容」，而客户端把这当成错误。这是**一个接口约定被收紧成不变式**——调用方因此永远不需要判 `None`。

**错误分派是按「类名字符串」做的注册表。** `_handle_plugin_daemon_error(error_type, message)` 用一个两层 `match`：

- 外层按 `error_type`（字符串）匹配 `PluginDaemonInnerError`、`PluginInvokeError`、`PluginDaemonInternalServerError`、`PluginDaemoninvalid_request_error`、`PluginDaemonNotFoundError`、`PluginUniqueIdentifierError`、`PluginNotFoundError`、`PluginDaemonUnauthorizedError`、`PluginPermissionDeniedError` 等**类名字符串**；
- 若 `error_type` 是 `PluginInvokeError`，再解一层 JSON 取 `error_type`，按**模型运行时异常**继续匹配（`InvokeRateLimitError`、`InvokeAuthorizationError`、`InvokeConnectionError`、`InvokeServerUnavailableError`、`CredentialsValidateFailedError`、`EndpointSetupFailedError`、触发器相关三类、`EventIgnoreError`、`PluginRuntimeError`）。

**靠类名字符串而不是数字码，是因为两侧是独立部署的两个代码库**——数字码要维护一张共享表，类名则天然可读、且与两边的代码同源。代价是**改名即破坏协议**：任何一侧重构类名，另一侧就落到 `case _` 兜底（外层抛裸 `Exception`，内层抛 `PluginInvokeError`）。

`PluginRuntimeError` 那个分支多做了两件事：从 `args.request_id` 取 `lambda_request_id`（用于把一次 Lambda 运行和插件调用对上），并对非字符串的 `message` 做兜底。**这是唯一一个携带结构化上下文的异常**，因为它是唯一需要跨系统定位的。

**流式接口用同一个错误协议，但只在 `code == -500` 时尝试解析。** `_request_with_plugin_daemon_response_stream` 逐行解析 SSE 风格的 `data:` 前缀，每行都当 `PluginDaemonBasicResponse` 校验。非零 `code` 时：**只有 `-500` 才尝试按 `PluginDaemonError` 解**，解析失败就退成 `PluginDaemonInnerError`；其它非零码直接 `ValueError`。

**这个不对称是刻意的**：流式响应里可能已经有部分数据被消费，此时能给出的信息越多越好，但不能为了拿错误详情把已经吐出的行搞乱。

`-500` 在这个协议里是「守护进程内部错误」的专用码，且是唯一允许携带嵌套错误 JSON 的码。

**响应形状有一处兼容垫片。** `_normalize_plugin_daemon_response_for_type` 只对 `PluginListResponse` 生效：如果守护进程直接回了一个 list 或回了 `{"data": [...]}`，就把它包成 `{"code": 0, "message": "", "data": {"list": ..., "total": len(...)}}`。**这是为「不同版本守护进程返回形状不一致」准备的**，且只在一种类型上做——说明历史上只有这个接口变过形。

**`PluginService` 的缓存用「代际号」而不是「删一批键」。** 四个 Redis 前缀：

- `plugin_model_providers:tenant_id:` （数据本体，可能按代际分键）
- `plugin_model_providers_generation:tenant_id:` （代际号）
- `plugin_model_providers_refresh_lock:tenant_id:` （刷新锁）
- `plugin_model_providers_remote_debug:tenant_id:` （远端插件标记）

失效时**不删数据键，而是 `incr` 代际号**：`invalidate_plugin_model_providers_cache` 用一条非事务 pipeline 同时 `delete` 无代际的旧键并 `incr` 代际。**旧代际的键自然变成垃圾、靠 TTL 过期**。这样做的收益是失效操作恒定代价、且不会与正在写的并发者互相踩；代价是 Redis 里会短暂堆积无用键。

**读路径是「双检锁 + 代际重试」的循环。** `fetch_plugin_model_providers` 的骨架：

1. 读代际号 → 按代际读缓存；命中就返回；
2. 代际不存在或缓存不可用 → 直接抓取并按当前代际写回；
3. 否则算剩余等待预算（`deadline = monotonic() + LOCK_WAIT_TIMEOUT`），超预算就直接抓取；
4. 拿刷新锁；拿不到就直接抓取；
5. **拿到锁后再读一次代际**：若变了就 `continue` 重来，若缓存已出现就返回，否则抓取并写回。

第 2 步与第 4 步的「直接抓取」是**降级到无缓存路径**，而不是让请求失败。这也是 `_plugin_model_providers_refresh_lock` 那个 contextmanager 返回 `bool` 的原因：**锁只是优化，不是正确性前提**。Redis 不可用时整个缓存层自动失效、每次都打守护进程。

**解析结果还套了一层进程内 LRU。** `_parsed_plugin_model_providers_cache` 是按「payload 长度 + payload 字节」做键的 `OrderedDict`，上限 `PLUGIN_MODEL_PROVIDERS_PARSED_CACHE_MAX_ENTRIES`（8），配一把 `threading.Lock`。源码注释把理由写全了：提供者声明是租户级但不含凭据，可以安全共享；按内容缓存解析结果，避免每次请求都付一次 Pydantic 校验成本；内容变了键自然变；小 LRU 限制单进程内存。注释还特意声明「consumers must treat them as read-only」——**共享缓存对象必须约定只读，这是没有冻结机制时的替代方案**。

**压缩是有版本前缀的。** `PLUGIN_MODEL_PROVIDERS_CACHE_COMPRESSION_PREFIX` 是一个字节前缀（`zstd-v1`），只在 payload 超过 `PLUGIN_MODEL_PROVIDERS_CACHE_COMPRESSION_MIN_BYTES`（64 KB）时才压。模块 docstring 说明**读侧同时接受旧的纯 JSON payload**，为的是滚动升级期间新旧实例共存。

给压缩数据加自描述前缀、而不是靠「试着解压失败就回退」，是更稳的做法：**判定靠标记，不靠异常**。

**插件市场的元数据缓存是另一套、简单得多。** `LatestPluginCache` 是 pydantic 模型（含 `status: "active" | "deleted"`、`deprecated_reason`、`alternative_plugin_id`），键前缀 `plugin_service:latest_plugin:`，TTL 300 秒。`fetch_latest_plugin_version` 先逐 id 读 Redis，未命中的打成一个列表；若 `MARKETPLACE_ENABLED` 关着就直接填 `None`，否则批量拉 manifest 再逐条写回。

**它没有代际号也没有锁**——因为它是「尽力而为的提示信息」（有没有新版），不是「决定能不能用」的数据。

**插件列表的端点计数会被事后校正。** 模块 docstring 提到某些守护进程版本的 `management/list` 会返回过期的 `endpoints_*` 聚合值。于是 `_reconcile_endpoint_counts` 在返回前按 `ENDPOINT_RECONCILIATION_PAGE_SIZE`（100）逐插件查真实端点并改正，注释说明这个页大小刻意与详情面板一致，让「列表校正」和「用户看到的抽屉」走同一条分页路径。**这是在客户端修上游数据不一致**，属于没有选择时的选择：上游修复需要版本协调，而 API 层必须立刻给出正确数字。

## 关键取舍

**源码事实：** `BasePluginClient` 所有请求都打向 `PLUGIN_DAEMON_URL`，带 `X-Api-Key`（来自 `dify_config.PLUGIN_DAEMON_KEY`），用模块级池化 `httpx.Client` 且 `trust_env=False`。

**源码事实：** 路径校验为「长度 ≤ 4096 → 最多 8 轮 `unquote` → 解码后按段拒绝 `..`」，超深编码与遍历都直接 `ValueError`。

**源码事实：** 守护进程回包统一包封 `PluginDaemonBasicResponse`（`code`/`message`/`data`）；`code != 0` 时把 `message` 当 JSON 解出 `error_type` 再按类名字符串分派；`data is None` 视为错误。

**源码事实：** 流式响应只在 `code == -500` 时尝试解析嵌套错误 JSON，其它非零码直接 `ValueError`；`-500` 是「守护进程内部错误」的专用码。

**源码事实：** `_normalize_plugin_daemon_response_for_type` 只对 `PluginListResponse` 做形状兼容（list 或 `{"data": [...]}` 都包成带 `list`/`total` 的标准形）。

**源码事实：** 提供者元数据缓存失效用 `incr` 代际号 + `delete` 无代际旧键，读路径是双检锁 + 代际变化重试；Redis 任何环节失败都降级为直接打守护进程，不失败。

**源码事实：** 缓存 payload 超过 64 KB 才用 zstd 压缩，并带 `\x00dify-plugin-model-providers-zstd-v1:` 前缀；读侧兼容旧纯 JSON。解析结果另有一层容量 8 的进程内 LRU，键为 payload 内容。

**源码事实：** `_redis` 锁类型用 `_RedisLock` Protocol 声明（只要求 `acquire`/`release`），没有导入具体的 redis 锁类。

**架构推论：** 「插件是独立进程」这个决定的代价全部压在这一层：每一次能力调用都要序列化、走网络、处理超时、解包、映射异常。收益是**隔离与可独立升级**——插件崩了不会带走主进程，插件可以有独立的依赖树（甚至独立语言实现）。

**架构推论：** 错误分派靠类名字符串，本质是**把两侧的代码约定当成协议**。它比错误码表更易读、更少维护，但缺少编译期校验；一旦任一侧改名，故障表现为「错误类型变成了兜底的那个」，而不是「协议不匹配」。想要前者又不要后者的做法是在协议里加一个稳定的错误码字段，代价是维护一张两边都要同步的表。

**架构推论：** 「锁只是优化，不是正确性前提」是这层最值得学的判断。所有 Redis 失败分支都 `yield False` 并降级为直接调用，于是**Redis 挂掉只影响性能，不影响可用性**。这与「拿不到锁就抛错」相反，后者的可用性上限等于最不可靠的那个依赖。

**架构推论：** 代际号失效法把「失效一批键」变成「一次 `incr`」。这在缓存键数量随租户线性增长时是唯一可扩展的做法——遍历删键的代价不可控，而 `incr` 恒定。代价是需要所有读路径都携带代际号，且旧键要等 TTL。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/plugin/impl/base.py` | `BasePluginClient` | 守护进程调用基类：路径校验、超时、包封解包、错误分派 |
| `api/core/plugin/impl/base.py` | `use_plugin_daemon_request_timeout` | 用 ContextVar 临时缩短当前上下文的超时 |
| `api/core/plugin/impl/base.py` | `_normalize_plugin_daemon_response_for_type` | 对 `PluginListResponse` 做响应形状兼容 |
| `api/core/plugin/plugin_service.py` | `PluginService` | 插件生命周期编排与租户级缓存所有权 |
| `api/core/plugin/plugin_service.py` | `PluginService.fetch_plugin_model_providers` | 双检锁 + 代际重试的缓存读路径 |
| `api/core/plugin/plugin_service.py` | `PluginService._plugin_model_providers_refresh_lock` | 返回 bool 的锁上下文，失败即降级 |
| `api/core/plugin/plugin_service.py` | `PluginService.invalidate_plugin_model_providers_cache` | 删旧键 + `incr` 代际 |
| `api/core/plugin/plugin_service.py` | `PluginService.fetch_latest_plugin_version` | 市场元数据的短 TTL 缓存 |
| `api/core/plugin/plugin_service.py` | `PluginService.uninstall` | 卸载编排（含凭据保留开关） |
| `api/core/plugin/plugin_service.py` | `PluginService.install_from_marketplace_pkg` | 市场包安装与签名校验链路 |
| `api/core/plugin/plugin_service.py` | `_RedisLock` | 只声明 `acquire`/`release` 的结构化锁类型 |
| `api/core/plugin/plugin_service.py` | `_ModelPluginIdentity` | 模型插件身份的 Protocol |
| `api/core/plugin/entities/plugin_daemon.py` | `PluginDaemonBasicResponse` | `code`/`message`/`data` 三段的包封模型 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/plugin/impl/base.py](https://github.com/langgenius/dify/blob/main/api/core/plugin/impl/base.py)
- [dify 源码：core/plugin/plugin_service.py](https://github.com/langgenius/dify/blob/main/api/core/plugin/plugin_service.py)
- [dify 文档：插件开发](https://docs.dify.ai/en/develop-plugin/getting-started/getting-started-dify-plugin)

## 与主核 / AI 的关系

这层的核心问题与主核里的「跨进程存储访问」完全同构：**把一次函数调用换成一次可能失败的远程调用**。主核里客户端连数据库要处理连接失败、认证、超时、错误码；这里连插件守护进程要处理同样的四件事。两者都不约而同地把「重试与降级」放在比「协议细节」更重要的位置。

**错误分派用类名 vs 数据库的 SQLSTATE。** 数据库用五位 SQLSTATE 码做错误分类，两侧（服务端/驱动）共享一张稳定表，加新码不改旧码；dify 用类名字符串，两侧共享的是**代码里的类名**。SQLSTATE 的好处是名字可以随便改、码不变；类名的好处是不用维护映射表、错误信息天然可读。**若两侧是同一版本发布的 monorepo，类名方案更省；若两侧独立发版（数据库就是这样），必须有稳定码。**

**代际号失效 vs 数据库的 relcache invalidation。** PostgreSQL 改表结构后要广播失效消息让各后端丢掉缓存的 relcache。dify 的 `incr` 代际号是它的简化版：不广播、不通知，靠「新读者用新代际、老读者自然过期」。**代价是失效有延迟窗口**（依赖 TTL），适合「元数据可以短暂陈旧」的场景；数据库不能有这个窗口，所以它选了同步广播。

**`trust_env=False` vs 数据库的 `PGHOST` 语义。** 数据库连接参数通常显式传入、不吃环境变量；dify 显式禁掉了 `httpx` 的环境代理继承。**内网依赖不能继承公网配置**是一条通用规则，两者用不同手段实现同一目标。

对 agent 工程最直接的价值有四条。其一：**插件/工具走独立进程时，错误映射层的成本是必须付的**，与其散落在各调用点，不如像这里集中到一个 `_handle_plugin_daemon_error`。其二：**锁只做优化不做正确性前提**，Redis 不可用时降级为直连，可用性不被最弱依赖拉低。其三：**缓存失效用代际号**，键数随租户增长时是唯一可扩展的做法。其四：**内网 HTTP 客户端要禁环境代理**，这个默认值错了会导致「本地好好的、上生产就慢或失败」。
