---
project: vllm
feature: serve-routers
title: 服务路由分层
page_type: feature
schema_version: 1
refs:
  - path: vllm/entrypoints/launchers/api_server/entry.py
    symbols: [build_and_serve, run_server]
---

# 服务路由分层

## 一句话职责

`entry.py` 是启动器的装配层：先在引擎之前抢下监听套接字，再把引擎客户端、应用壳、uvicorn 服务三者按正确顺序接起来，最后把关停任务交回调用方等待。

## 机制是什么

一个 vllm 服务的启动要跨越三个**生命周期不同**的对象：

| 对象 | 生命周期 | 谁创建 |
|---|---|---|
| 监听套接字 | 最长（先于引擎、长于 uvicorn） | `setup_server` |
| 引擎客户端 | 中等（需要优雅关停） | `build_async_engine_client` |
| uvicorn 服务 | 最短（随请求处理而启停） | `serve_http` |

三个生命周期必须嵌套且次序固定：**套接字先绑，引擎后建，服务最后起**。这个次序不是代码风格，而是有两个具体理由写在注释里：

```python
# workaround to make sure that we bind the port before the engine is set up.
# This avoids race conditions with ray.
```

**先绑端口是为了避开与 Ray 的竞态**——Ray 集群里多个进程可能同时尝试初始化，若端口在引擎建好之后才绑，可能已经被别人占了，而那时引擎已经吃掉了大块显存。

第二个理由是同一条链上的另一半：`set_ulimit()` 的注释写「workaround to avoid footguns where uvicorn drops requests with too many concurrent requests active」。**先调高文件描述符上限，再让 uvicorn 接管**，否则高并发下 uvicorn 会静默丢连接。

## 怎么实现的

**`run_server` 的第一件事是抢先占住 SIGTERM。**

```python
def _interrupt_init(*_) -> None:
    raise KeyboardInterrupt("terminated")
signal.signal(signal.SIGTERM, _interrupt_init)
```

注释解释了为什么：「Interrupt initialization if SIGTERM arrives before uvicorn installs its own signal handlers. Once uvicorn is running it replaces this.」——**引擎初始化可能要几十秒（加载权重、profile 显存），这段时间里用户按 Ctrl-C 或容器收到 SIGTERM 都必须能中断**。而 uvicorn 的信号处理器要到 `serve_http` 才装上，中间这段是空窗。

之后 `decorate_logs("APIServer", skip_if_decorated=True)` 给日志加前缀，然后 `setup_server(args, reuse_port=False)` 返回 `(listen_address, sock)`。

**`setup_server` 做四件有顺序的事。** 逐条看：

1. `log_version_and_model` + `log_non_default_args`——**先打日志再校验**。这样即使 `validate_api_server_args` 报错退出，日志里也有版本与非默认参数，排障时能看到「它到底想用什么配置启动」。
2. `init_parser_plugin(args)`——初始化解析器插件。
3. `validate_api_server_args(args)`——校验。
4. 绑套接字。

第 4 步有两个分支：`--uds` 走 `AF_UNIX`（`create_server_unix_socket`），否则走 `create_server_socket`。

**`create_server_socket` 里三个 `setsockopt` 各有明确用途。**

```python
family = socket.AF_INET
if is_valid_ipv6_address(addr[0]):
    family = socket.AF_INET6
sock = socket.socket(family=family, type=socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
if reuse_port:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
sock.bind(addr)
```

`SO_REUSEADDR` 无条件开——避免重启时 `TIME_WAIT` 占用端口。「`SO_REUSEPORT` 只在 `reuse_port` 为真时开」这条是刻意的：`run_server` 传的是 `reuse_port=False`，表示**单 worker 场景不允许同端口多进程**（会静默分摊连接，难以诊断）；多 API worker 场景才需要它做内核级负载均衡。

IPv6 判定只看地址是否合法，**没有单独的开关**——传 `::` 就自动走 `AF_INET6`。这省了一个配置项，代价是地址解析错误会在这一层表现为「绑错协议族」。

**`listen_address` 的构造只是为了打日志好看。**

```python
host_part = f"[{addr}]" if is_valid_ipv6_address(addr) else addr or "0.0.0.0"
listen_address = f"http{'s' if is_ssl else ''}://{host_part}:{port}"
```

`addr or "0.0.0.0"` 处理「host 为空串」——空串表示绑所有接口，但打印成 `0.0.0.0` 更直观。IPv6 地址要加方括号才是合法 URL。`is_ssl` 只看 `ssl_keyfile` 与 `ssl_certfile` 都给了。

**`run_server_worker` 是生命周期嵌套最清晰的一段。**

```python
async with build_async_engine_client(args, client_config=client_config) as engine_client:
    shutdown_task = await build_and_serve(engine_client, listen_address, sock, args, peer_loads, **uvicorn_kwargs)
# NB: Await server shutdown only after the backend context is exited
try:
    await shutdown_task
finally:
    sock.close()
```

**`await shutdown_task` 被刻意放在 `async with` 之外**，注释写「Await server shutdown only after the backend context is exited」。这句的顺序很关键：`serve_http` 返回的是一个**任务**（不是已完成的结果），而退出 `async with` 会触发引擎关停。若先等 uvicorn 关停再关引擎，那么关停期间到达的请求会打到已经死了的引擎；反过来，先把引擎关掉、再等 uvicorn 收尾，剩下的连接会得到明确错误。

`finally: sock.close()` 保证无论怎么退出都释放套接字。

**真正的关停顺序藏在 `serve_http` 的 `handle_shutdown` 里，分三步且有明确先后。**

```python
await loop.run_in_executor(None, partial(engine_client.shutdown, timeout=timeout))
...
server.should_exit = True
server_task.cancel()
```

第一步把引擎关停丢进**线程池**执行——因为 `engine_client.shutdown` 是同步阻塞的（要释放显存、结束子进程），直接 `await` 会卡住整个事件循环。第二步才置 `server.should_exit = True` 并 `cancel` 服务任务。**先关引擎、再关 HTTP**，与 `run_server_worker` 里「`await shutdown_task` 在引擎上下文之外」是同一个次序的两次表达。

`mode = "abort" if timeout == 0 else "drain"` 这行把「超时为 0」解释成「立即中断」而不是「无限等待」——所以 `shutdown_timeout=0` 是一个明确的语义（硬杀），不是一个退化情形。

`signal_handler` 里有 `if shutdown_event.is_set(): return` 的短路，并且用 `logger.info_once` 而不是 `info`——**重复的 Ctrl-C 不会打出重复日志**。随后在 `except asyncio.CancelledError` 分支里，它会调 `find_process_using_port(port)` 并打印占用端口的进程命令行——**这是一条针对「端口被占」的诊断日志**，说明这个失败模式在实际使用中足够常见。

`watchdog_loop` 是另一条独立的关停触发路径：每 5 秒检查引擎是否 `errored and not is_running`，配合 `terminate_if_errored` 决定是否 `server.should_exit = True`。它的 docstring 说明了存在理由：「Needed to trigger shutdown if an exception arises in `StreamingResponse()` generator」——**流式响应里的异常不会传播到 uvicorn 的主循环**，因为它已经在生成器内部被吞掉了，所以需要一个旁路监视。

**`build_and_serve` 才是「造壳 + 填状态 + 起服务」三合一。**

```python
supported_tasks = await engine_client.get_supported_tasks()
model_config = engine_client.model_config
logger.info("Supported tasks: %s", supported_tasks)
app = build_app(args, supported_tasks, model_config)
await init_app_state(engine_client, app.state, args, supported_tasks)
return await serve_http(app, sock=sock, ...)
```

`supported_tasks` 是 **`await` 出来的**——它不是配置读出来的常量，而是引擎探测的结果。这解释了上一页为什么整个路由集合能按能力挂：能力信息在这一刻才可用。

`logger.info("Supported tasks: %s", supported_tasks)` 这一行**正是上一页提到的「缺什么」**——它至少把任务集打出来了，虽然没打实际挂载的路由清单。

`serve_http` 的实参列表很长，可以按用途分组：

- **套接字**：`sock=sock`（传入已绑好的 socket，uvicorn 不再自己绑）。
- **回退地址**：`host=args.host, port=args.port`——只在 `sock` 未被使用时才用。
- **SSL**：`ssl_keyfile` / `ssl_certfile` / `ssl_ca_certs` / `ssl_cert_reqs` / `ssl_ciphers` 五个全量透传，加 `enable_ssl_refresh`。
- **HTTP 解析限制**：`h11_max_incomplete_event_size` / `h11_max_header_count`。
- **日志与超时**：`log_level`、`access_log=not args.disable_uvicorn_access_log`、`timeout_keep_alive=envs.VLLM_HTTP_TIMEOUT_KEEP_ALIVE`。
- **负载信息**：`peer_loads`。

`access_log` 那里又出现一次「配置是 disable、状态是 enable」的取反，与上一页 `log_stats = not disable_log_stats` 是同一个模式。

**`peer_loads` 是一对「共享数组 + 自己的下标」。** `run_server_worker` 里这样构造：

```python
if client_config and "api_server_loads" in client_config:
    loads = client_config.pop("api_server_loads")
    peer_loads = (loads, client_config["client_index"])
```

`pop` 而非 `get`——**取出即移除**，因为这个键不能继续往 `AsyncLLM.from_vllm_config` 传（那里只认引擎级配置）。而 `client_index` 是 `get`，因为后面还要用。这个细节说明 `client_config` 这个字典是**多个消费者共享的传递包**，每个消费者只取自己认识的键。

`peer_loads` 的用途在 `launcher.py` 的 `NoSignalServer` docstring 里写得很清楚：多个 API worker 共享同一个 socket 时，**每个 worker 把自己的连接数写进共享数组，待处理的连接由负载最低的 worker 接**。注释解释了为什么不能用默认行为：「The event loop's own server would accept every queued connection in whichever worker wakes first.」

配套两个常量说明这套机制的精度：`_ACCEPT_DEFER_CHECKS = 5`（每 1 毫秒重查一次、最多 5 毫秒）、`_NOT_ACCEPTING = 2**31 - 1`（不接连接的 worker 写这个值，保证它永远不会被选为最低负载）。

**`NoSignalServer` 的另一个动机是信号所有权。** docstring 写「Callers register their own handlers on the event loop for graceful shutdown; uvicorn's would race with and override them」。所以这里**不要 uvicorn 装的处理器**，由调用方统一管。与 `run_server` 开头那个抢占 SIGTERM 的 `_interrupt_init` 是同一套思路：**信号只能有一个所有者，而这个所有者必须是启动器。**

**引擎客户端的两种形态由 `build_async_engine_client_from_engine_args` 决定。** docstring 写：

> Create EngineClient, either: in-process using the AsyncLLMEngine Directly, or multiprocess using AsyncLLMEngine RPC

具体走哪条对调用方透明——它只知道拿到了一个 `EngineClient`。这个 docstring 的表述里还留了一点历史痕迹：「Returns the Client or None if the creation failed」，但实现里 `yield async_llm` 前有 `assert async_llm is not None`，所以实际不会返回 `None`。

**`finally: async_llm.shutdown(timeout=vllm_config.shutdown_timeout)` 是唯一的关停点。** 无论正常退出还是异常，引擎都会被关。超时来自配置，说明「关停也要有时间预算」——大模型关停要释放显存、结束子进程，可能卡住。

**`reset_mm_cache` 在 `yield` 之前调用，注释写「Don't keep the dummy data in memory」。** 引擎构造时可能跑过 dummy 前向（为了 profile 或 warmup），那些多模态数据要清掉，否则白占显存。

**`forkserver` 预导入是启动期的一处性能优化。**

```python
if os.getenv("VLLM_WORKER_MULTIPROC_METHOD") == "forkserver":
    multiprocessing.set_start_method("forkserver")
    multiprocessing.set_forkserver_preload(["vllm.v1.engine.async_llm"])
    forkserver.ensure_running()
```

注释写「Pre-import heavy modules in the forkserver process」。`forkserver` 模式下子进程从 forkserver 进程 fork，所以**在 forkserver 里预先 import 的重模块会被所有子进程继承**——避免每个 worker 各自 import 一遍。

注意这段用 `os.getenv` 直接读环境变量，而不是 `envs.VLLM_WORKER_MULTIPROC_METHOD`。**绕过统一的 envs 模块意味着这个开关不会出现在配置文档里**，也不会被参数校验覆盖。

**`main` 里有三处值得记的细节。**

1. `import uvloop` 放在函数内——因为它是可选依赖，缺失时应该给出 import 错误而不是让整个模块加载失败。
2. 注释写「This section should be in sync with `vllm/entrypoints/cli/main.py` for CLI entrypoints」——**承认存在手工同步点**。
3. `uvloop.run(run_server(args))` 是唯一的顶层驱动。`uvloop` 替换默认事件循环以获得更高吞吐。

另外 `logger = init_logger("vllm.entrypoints.launchers.api_server.entry")` 那行上方有一条注释：「Cannot use `__name__` (https://github.com/vllm-project/vllm/pull/4765)」。**硬编码名字而非用 `__name__`**，说明这里有一个已修复的具体 bug 导致不能用模块名。

## 关键取舍

**源码事实：** 启动顺序是「先绑套接字 → 再建引擎 → 最后起 uvicorn」，注释给出两个理由：避开与 Ray 的端口竞态、避免 uvicorn 在低 fd 上限下丢连接。

**源码事实：** `run_server` 在引擎初始化前就把 SIGTERM 改成抛 `KeyboardInterrupt`，注释说明这是为了填补「uvicorn 装自己的处理器之前」的空窗。

**源码事实：** `setup_server` 先打版本与非默认参数日志，再校验参数，最后绑套接字。

**源码事实：** `SO_REUSEADDR` 无条件开启，`SO_REUSEPORT` 只在 `reuse_port=True` 时开；`run_server` 传的是 `reuse_port=False`。

**源码事实：** `await shutdown_task` 在 `async with build_async_engine_client(...)` 之外，注释写「only after the backend context is exited」。

**源码事实：** `supported_tasks` 通过 `await engine_client.get_supported_tasks()` 获得，并在起服务前打一条 info 日志。

**源码事实：** `peer_loads` 由 `client_config.pop("api_server_loads")` 与 `client_config["client_index"]` 组成；`pop` 是因为该键不能再传给引擎。

**源码事实：** `NoSignalServer` 不安装 uvicorn 自己的信号处理器（避免与调用方竞争），并在有 `peer_loads` 时用共享数组实现「最低负载 worker 接新连接」。

**源码事实：** `forkserver` 分支用 `os.getenv` 直接读环境变量，并在 forkserver 进程里预导入 `vllm.v1.engine.async_llm`。

**源码事实：** 模块级 logger 名硬编码为字符串，注释指向一个具体 PR 解释为什么不能用 `__name__`。

**架构推论：** 「先绑端口再建引擎」这条次序揭示了一个通用原则：**独占资源要在最贵的准备之前落地**。端口是独占的、绑定是廉价的；引擎初始化是昂贵的、但不独占端口。把廉价且独占的动作提前，就把「资源冲突」这个最可能失败的因素移到了失败代价最低的阶段。

**架构推论：** 信号所有权被严格收归启动器，是长启动流程的必然要求。**任何跨越数秒的初始化都需要一个能在中途被中断的入口**，否则容器编排系统发来的 SIGTERM 会等到初始化完成才生效，超时后变成 SIGKILL——那就会留下未释放的显存与孤儿进程。

**架构推论：** `await shutdown_task` 放在引擎上下文之外，实现的是「先停后端、再收连接」。这会让关停窗口里新到的请求得到错误而不是挂起——**明确失败优于静默等待**，因为客户端能立刻重试到别的实例。

**架构推论：** `peer_loads` 那套共享数组 + 最低负载接连接，是在**内核级 `SO_REUSEPORT` 负载均衡不够精确**时的补丁。`SO_REUSEPORT` 的哈希按四元组分摊，不反映实际连接数；而共享数组让每个 worker 主动发布自己的负载。代价是需要进程间共享内存与一颗 1ms 粒度的重试循环。**这类「我自己的调度器」通常只在测量到默认行为确实不均时才值得引入。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/entrypoints/launchers/api_server/entry.py` | `run_server` | 单 worker 启动：占信号、绑端口、进 worker 循环 |
| `vllm/entrypoints/launchers/api_server/entry.py` | `run_server_worker` | 生命周期嵌套：引擎 → 服务 → 关停 → 释放套接字 |
| `vllm/entrypoints/launchers/api_server/entry.py` | `build_and_serve` | 取任务集、造壳、填状态、交给 uvicorn |
| `vllm/entrypoints/launchers/api_server/entry.py` | `build_async_engine_client` | 引擎客户端的上下文管理器（含 forkserver 预导入） |
| `vllm/entrypoints/launchers/api_server/entry.py` | `build_async_engine_client_from_engine_args` | 单/多进程形态的统一入口，含关停超时 |
| `vllm/entrypoints/launchers/launcher.py` | `setup_server` | 打日志、校验参数、绑套接字、调高 fd 上限 |
| `vllm/entrypoints/launchers/launcher.py` | `create_server_socket` | IPv4/IPv6 选择与 `SO_REUSEADDR`/`SO_REUSEPORT` |
| `vllm/entrypoints/launchers/launcher.py` | `create_server_unix_socket` | UDS 分支 |
| `vllm/entrypoints/launchers/launcher.py` | `NoSignalServer` | 不接管信号的 uvicorn 服务，含最低负载接连接 |
| `vllm/entrypoints/launchers/launcher.py` | `terminate_if_errored` | 引擎死亡时决定是否退出服务 |
| `vllm/entrypoints/launchers/utils/server_utils.py` | `get_uvicorn_log_config` | uvicorn 日志配置（含端点过滤） |
| `vllm/entrypoints/launchers/launcher.py` | `serve_http` | 起 uvicorn 服务、装信号处理器、返回关停任务 |
| `vllm/entrypoints/launchers/launcher.py` | `watchdog_loop` | 周期检查引擎错误状态以触发关停 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/entrypoints/launchers/api_server/entry.py](https://github.com/vllm-project/vllm/blob/main/vllm/entrypoints/launchers/api_server/entry.py)
- [vllm 源码：vllm/entrypoints/launchers/launcher.py](https://github.com/vllm-project/vllm/blob/main/vllm/entrypoints/launchers/launcher.py)
- [uvicorn 文档：Server 与信号处理](https://www.uvicorn.org/deployment/)

## 与主核 / AI 的关系

「先绑端口再建引擎」与主核里「先 `listen()` 再启动后台进程」是同一个次序。数据库在 `pg_ctl start` 时先占住 5432，再去恢复 WAL、加载共享缓冲——**若端口被占，你在最便宜的阶段就知道，而不是等十分钟恢复完才发现绑不上。**

**信号所有权 vs 主核的 postmaster 信号循环。** 数据库的 postmaster 是唯一收 SIGTERM 的进程，其余进程由它转发。vllm 的启动器也是唯一所有者，uvicorn 被明确禁止装自己的处理器。**共同理由：信号处理必须是单点，否则「谁先收到」变成竞态**——而竞态在关停路径上会导致资源泄漏而不是数据错乱，更难发现。

**`await shutdown_task` 的位置 vs 主核的「先停止接受新连接」。** 数据库关停的第一阶段是「拒绝新连接、等待现存事务」；vllm 是「先关引擎、再等 uvicorn 收尾」。两者都是**先切断服务能力、再优雅收尾**，方向一致。

**`SO_REUSEPORT` 与最低负载接连接 vs 主核的连接池。** 数据库用连接池把「连接建立」的成本前置并复用；vllm 用共享数组把「连接分配」的决策精确化。**两者都在解决「谁来处理下一个请求」，但一个在连接层、一个在分发层。**

对 agent 工程最直接的价值有四条。其一：**长启动流程必须有一个能在中途被中断的入口**，否则编排系统的 SIGTERM 会被吞掉。其二：**独占资源要在最贵的准备之前抢下来**，端口、锁文件、临时目录都属于这一类。其三：**关停顺序要「先断服务能力、再收尾」**，让关停窗口里的请求得到明确错误而不是挂起。其四：**多个消费者共享一个配置字典时用 `pop` 而不是 `get`**，取走即移除，避免把上层键误传给下层。
