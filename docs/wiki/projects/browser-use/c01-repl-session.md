---
project: browser-use
feature: repl-session
title: REPL 父进程侧与会话管理
page_type: feature
schema_version: 1
refs:
  - path: browser_use/repl/session.py
    symbols: [Repl, start, call]
  - path: browser_use/repl/session.py
    symbols: [check_mode, check_loop, _kill_tree, kill_workers]
---

# REPL 父进程侧与会话管理

## 一句话职责

`Repl` 是「一个能杀掉重来的 Python 会话」的父进程侧：它起一个子进程、开一个本地端口等子进程回连、按单元格收发消息、在超时或无响应时杀掉整棵进程树并重启——而**文件、检查点与浏览器状态跨重启存活，只有 Python 变量丢失**。

## 机制是什么

整个设计由一句话需求驱动：模型写的 Python 必须能被**强制中断**。线程杀不掉（Python 没有安全的中断机制），所以只能靠独立进程。于是产生了三个必须解决的工程问题：

| 问题 | 解法 |
|---|---|
| 父子进程怎么通信 | 子进程**回连**父进程监听的 `127.0.0.1` 临时端口，用随机令牌自证 |
| 超时后怎么保证不留残党 | 子进程自成一个进程组（POSIX）/ 作业对象（Windows），杀整棵树 |
| 谁能活多久 | 用普通 `subprocess.Popen` 而非 asyncio 子进程，因为 `keep_alive` 的 Agent 会活过事件循环 |

第一点的动机写在模块 docstring 里，而且点明是**跨平台**诉求：「so it works the same on Windows, macOS and Linux（no inherited file descriptors）」。这是一个明确的技术选择：Unix 的 `socketpair` + 继承 fd 更省事，但 Windows 上把 fd 交给子进程不可移植。

第三点是这一页最容易忽略但影响最大的决定，下面细说。

## 怎么实现的

**回连与令牌自证。** `_start` 先 `token = secrets.token_hex(32)`，再 `asyncio.start_server(on_connect, '127.0.0.1', 0, limit=STREAM_LIMIT)`——端口写 0 表示让内核分配临时端口，随后从 `server.sockets[0].getsockname()[1]` 读出来。

`on_connect` 是握手点，它的逻辑很紧：

```python
line = await asyncio.wait_for(reader.readline(), 10)   # 10s 读一行
if not connected.done() and hmac.compare_digest(line.strip(), token.encode()):
    connected.set_result((reader, writer))
else:
    writer.close()
```

三个细节：

- **10 秒超时**：注释说 `except Exception` 的注释是「- a stray client」，即监听期间任何本地进程都可能连上来搭话，不能让它们挂住握手。
- **`hmac.compare_digest`** 而不是 `==`：即使这里比的是本地环回上的令牌，仍用常量时间比较——这是「不在代码里留下比较捷径」的纪律。
- **`connected.done()` 先行判断**：第一个正确令牌胜出，之后的连接一律关闭。于是监听窗口内不存在「两个客户端都算数」的竞态。

令牌与端口都通过环境变量交给子进程（`BU_REPL_PORT`、`BU_REPL_TOKEN`），子进程用完立刻 `os.environ.pop`。

**`start` 与 `_start` 是两个方法，中间隔着一个上下文隔离。** `start` 只做一件事：把 `_start` 放进一个**全新的空 `contextvars.Context()`** 里跑，并顺手计时（`start_ns`，供遥测的 `ms_repl_start` 用）。注释给了理由：

> the worker's socket reader holds the context it was opened in while it is open, and the run's (Pydantic AI's `RunContext` → `Session` → this `Repl`) kept an `Agent` dropped without `close()` from being freed

也就是说：`asyncio` 的 socket 读取器会持有它被创建时的 contextvars 上下文快照。若不隔离，这个快照里会有 `RunContext` → `Session` → `Repl` 的引用链，于是**一个被丢弃但没 `close()` 的 `Agent` 会一直无法被回收**。用一个空 context 起任务，等于把这条引用链切断。这是一个非常具体的内存泄漏根因，而不是泛泛的「避免循环引用」。

**`_start` 的失败语义是「绝不留下半个 worker」。** 整段在 `try/except BaseException` 里，`except` 第一件事是 `await self.close()`，然后把 `TimeoutError`/`ConnectionError` 换成带日志路径的 `RuntimeError`（`REPL worker failed to start; see .../worker.log`）。`finally` 里关日志文件与监听 socket。注意捕的是 `BaseException`——**取消也要清理**。

启动流程的顺序值得逐步看：

1. `check_loop()`——在任何东西（尤其是**计费的云端浏览器**）启动之前就检查 Windows 事件循环类型。函数 docstring 写明了原因：子进程在 `SelectorEventLoop` 上会 `NotImplementedError`，所以要在花钱之前给出可操作的错误（提示删掉 `WindowsSelectorEventLoopPolicy`）。
2. `await self.close()`——注释说明理由：「a dead previous worker's cell processes may still run: kill its group/job before replacing it」。即重启前先清场，否则上一轮的单元格子进程可能还在操作浏览器。
3. `self.state.mkdir(parents=True, exist_ok=True)`，并在这里**删掉上一轮的 `captcha.json`**：注释说这是「solver state of an earlier browser session」，而重启要保留它（同一浏览器会话的求解状态有意义）。
4. 起监听、造 `token`、造 `connected` future。
5. 组环境变量并 `Popen`。
6. `_job_for(pid)` 拿 Windows 作业对象，登记进 `_WORKERS`。
7. 装 `weakref.finalize` 兜底与 `_install_cleanup()`。
8. `asyncio.wait({connected, exited}, timeout=60, FIRST_COMPLETED)`——注意它**同时等「连上了」和「进程退了」**，谁先到谁赢。这比只等 connected 好：子进程若立刻崩掉，能马上失败而不是白等 60 秒。
9. 连上后写一行 init（`secrets`、四个能力开关），再等子进程回一行 `ready`。

**环境变量里的几处非默认设置。** 除了 `worker_env` 的白名单与 `BU_REPL_*` 之外，还有几条值得记：

- **`-P` 启动参数**：注释写「the workspace is not `sys.path[0]`: a `json.py` there must not shadow the stdlib（worker.py appends it）」。防止模型在工作区里放一个同名文件把标准库顶掉，是这类「模型可写目录 + 子进程」组合的必要防护。工作目录（`cwd=self.workspace`）确实是模型可写的地方。
- **`BH_TAB_MARKER='0'`**：关闭 browser-harness 的标签页标题标记，否则每个页面加载后标题会被加上一个马形前缀，模型会把前缀当成页面标题读回去。
- **`BU_REPL_PARENT_PID`**：父进程 pid，供子进程的 `_watch_parent` 使用。
- **`BH_TELEMETRY='0'`**：注释说明理由是「A browser-harness CLI call from a cell would send its piped code and output（page text）as `cli_event`」——即子进程侧的遥测会把页面文本发出去，而白名单已经掉了 `ANONYMIZED_TELEMETRY`，所以直接关掉；这次运行在 `agent_event` 里已计过。
- **`PYTHONUTF8='1'` 与 `PYTHONIOENCODING='utf-8'`**：注释一句话点明「page text is Unicode; Windows defaults to cp1252」。

`stdin=subprocess.DEVNULL`、`stdout=log`、`stderr=log`（都指向 `state/worker.log`）。关掉 stdin 是必须的：子进程隔一段时间会读一行（协议），若继承父进程 stdin 就可能与用户终端争抢输入。

**为什么是普通 `Popen` 而不是 asyncio 子进程。** 这是 `__init__` 里注释解释的一处刻意选择：

> a plain `Popen`, not an asyncio subprocess: asyncio's child watcher belongs to the loop that started the worker, which a `keep_alive` Agent outlives（`asyncio.run` per run）; killed after that loop closed, the worker was never reaped on Linux（pidfd watcher）and logged "Loop ... that handles pid ... is closed" on macOS

展开来说：`keep_alive=True` 的 Agent 会跨多次 `asyncio.run` 存活，而 REPL worker 也就跨了多个事件循环。asyncio 的子进程管理器绑定在**创建它的那个 loop** 上；loop 关掉之后再去 kill/wait 那个进程，Linux 上进程永远不会被回收（僵尸），macOS 上则直接报「loop 已经关闭了」。

对应地，`_exited` 自己实现了轮询：

```python
while proc.poll() is None:
    await asyncio.sleep(0.02)
```

并且带了一条 `noqa: ASYNC110`，注释是「polled: an asyncio child watcher would tie it to one event loop」。这条 `noqa` 是必要的说明——项目规范通常禁止 `sleep` 轮询，这里的例外有明确理由。

**杀整棵树的两套机制。** `_kill_tree(pid, job)` 分平台：

- **POSIX**：`os.killpg(pid, signal.SIGKILL)`，因为子进程是 `start_new_session=True` 起的，**自己就是进程组的组长**。注释解释了为什么必须杀整组：「a surviving subprocess could keep driving the browser」。
- **Windows**：先 `TerminateJobObject(job, 1)` 再 `CloseHandle(job)`，然后无条件 `taskkill /F /T /PID`。双保险：作业对象能杀掉「已注册进 job 的进程」，`taskkill /T` 能杀掉「还能从 pid 找到的子树」；先者的价值在**worker 自己已经死了之后**——那时 `taskkill /T` 已经不认这个 pid 的树了。

job 对象由 `_job_for(pid)` 创建，实现全部是 `ctypes` 直调 `kernel32`，四处细节值得记：

- **`CreateJobObjectW.restype = OpenProcess.restype = ctypes.c_void_p`**：注释写「HANDLEs are pointer-sized: the default int restype truncates them on 64-bit Windows」。这是 `ctypes` 最常见的 64 位错误——不设 `restype` 会把句柄截成 32 位，后续调用必然失败。
- **`LimitFlags = 0x2000`（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）**：注释解释这条最妙：「when this process dies, **however it dies**, Windows closes its job handle and kills the worker's whole tree」。即父进程被 `SIGKILL`/任务管理器强杀时，子进程树也会被内核连带清掉——不需要父进程有机会跑清理代码。
- **`OpenProcess(0x0101, ...)`** 即 `PROCESS_SET_QUOTA | PROCESS_TERMINATE`：注释明确列出这两个权限位。
- **整体是 best-effort**：注释说「it cannot be created」时返回 `None`，理由是同一条注释里给的退路——「without it the worker's watchdog still exits when we die（`worker._watch_parent`）」。所以 Windows 上有两条独立的死亡传播路径。

**三层清理，各自覆盖不同的失效场景。**

1. **`weakref.finalize(self, _end_worker, pid, job)`**：兜住「`Repl` 被丢弃但没 `close()`」的情形。注释里给了实测数字：「one dropped without it（a service running many Agents on one Browser）left a ~35 MB worker each until exit」——即每个泄漏的 Agent 留下一个 35 MB 的 worker 进程直到进程退出。这是从真实的服务化使用里发现的。
2. **`self._reaper.atexit = False`**：注释只有四个字「exit: `kill_workers`, before browsers stop」，但它表达了一个**顺序要求**——进程退出时应该先杀 worker 再关浏览器，因为 worker 的单元格可能正在操作浏览器。所以这里主动关掉 finalize 自带的 atexit（`weakref.finalize` 默认在退出时执行），改由 `_install_cleanup()` 注册的 `atexit` + `SIGTERM` 处理器统一走 `kill_workers()`。
3. **`_install_cleanup()`**（从 `browser_use.browser.browser` 导入）：把清理挂到进程边界上。

`_WORKERS` 是模块级 `dict[pid, job]`，`kill_workers()` 循环 `popitem()` 逐个杀——用 `while _WORKERS: _kill_tree(*_WORKERS.popitem())` 的写法保证「即使某个杀失败也不会死循环」（`_kill_tree` 内部吞掉 `ProcessLookupError`/`PermissionError`，且 pop 已发生）。

`_end_worker` 则先 `pop` 再 `_kill_tree`。注意它**总是**调用 `_kill_tree`，即使 worker 已经退出——因为「进程的单元格子进程可能还在跑」。

**单元格编号跨 run 续接。** `__init__` 里有这么一段：

```python
cells = (p.stem for p in (self.state / 'cells').glob('*.txt'))
self.n = max((int(c) for c in cells if c.isdigit()), default=0)
```

注释说明了为什么需要：「a follow-up run's new REPL must not overwrite earlier cells' files, which the retained conversation still points to」。`keep_alive` 场景下同一个工作区会被多次 run 使用，而**保留下来的对话历史里引用着旧单元格的输出文件**（`output_file` 字段）。若从 1 重新编号，新一轮的 `0001.txt` 会覆盖上一轮的，历史消息里的链接就指向了错误内容。

注意 `c.isdigit()` 的过滤——目录里可能有非数字命名的文件，直接 `int()` 会炸。

**锁要跟着事件循环走。** `call` 开头有一段看起来奇怪的重建：

```python
if self._lock_loop is not (loop := asyncio.get_running_loop()):
    self.lock, self._lock_loop = asyncio.Lock(), loop
```

注释是「reused from a new loop（`asyncio.run` per run）」。`asyncio.Lock` 绑定在创建它的事件循环上，跨 `asyncio.run` 复用会出问题（等待者挂在上一个已关闭的循环里）。所以锁必须按循环重建。这类「asyncio 原语与循环同寿命」的约束在 `keep_alive` 场景里会反复出现。

`__init__` 里给锁的注释说明了它的用途：「the model may emit parallel python calls; the worker runs one cell at a time」——模型可能在一轮里发多个 `python` 调用，但 worker 是单线程逐格执行的，所以要在父进程侧串行化。

**`call` 在 `_call` 之上叠了两层。** 第一层是重连：若结果里 `error_type == 'BrowserConnectionLost'` 且配了 `reattach` 回调，就调它拿新的环境变量。这里有一个**关键分叉**：

- 若 `reattach` 返回的 env **不等于**当前 `self.env`（说明连到了**另一个**浏览器守护进程），就更新 env、`close()` 重启 REPL，并在错误文本后追加「浏览器被停了又起了新的：标签页、登录与页面状态都没了，REPL 也重启了」。
- 若 env 相同（同一个浏览器，只是守护进程重启），只追加「daemon 已在同一浏览器上重启：重试，但先看页面状态」。
- 若 `reattach` 抛异常，追加错误并置 `r['browser_gone'] = True`——注释说明这个标记的后果：「the agent's next turn is delivery-only: no cells without a browser」。

区分这两者的理由是：能不能继续干，取决于浏览器状态还在不在。env 相同意味着还是那个浏览器，变量丢了但页面还在；env 不同意味着换了浏览器，页面也没了。

第二层是 `after_cell`（仅 `op == 'exec'`）：它返回 `(notices, blocks)`，notice 被**前置**拼到输出前面。注释给了顺序理由：「the cell's own output after a blocked click（an error page titled with the blocked host）can read as a successful landing」——先说要紧的，否则模型会把被拦的落地页当成成功。`blocks` 则累加进 `stats['events']['domain_blocked']`。

**`_call` 的协议循环。** 写一行 JSON 请求，然后在 `asyncio.timeout(timeout)` 里读行循环。循环要处理两类消息：

- **`type` 为 `search` / `downloads` / `feedback` / `rotate_proxy`**：这是单元格在**执行中途**请求父进程代做一件事（API key 留在父进程）。回答后 `continue` 继续等，不消耗这次调用的结果。
- **`n == n`**：本次调用的结果。注释一句话点明为什么要校验：「never hand one call another call's answer」。虽然 `lock` 已经串行化，但重启、残留消息都可能让编号错位，这个校验是最后一道。

**重启判据有三条，都指向「这个 worker 用不了」。**

```python
moved = self.proc is not None and self.loop is not asyncio.get_running_loop()
if moved or self.proc is None or self.proc.poll() is not None:
    await self.start()
```

第二条（没起过）与第三条（进程死了）是显然的。第一条才是有意思的：**worker 是在上一个（已结束的）事件循环上启动的**，它的 socket 无法在这个循环里驱动。这正是 `keep_alive` 场景的常态——每次 `asyncio.run` 是一个新循环。重启后给输出加前缀 `RESTARTED.format(why="This run uses a new event loop")`，让模型知道变量丢了。

**超时后的返回值是一份「可继续的错误」。** 捕获 `TimeoutError`/`ConnectionError` 后先 `close()`，然后读单元格输出文件的**头部**（`_head`，最多 `MAX_OUTPUT_CHARS + 1`）并用 `clip` 加工，返回一个结构与正常结果完全一致的 dict，只把 `error`/`error_type`/`error_message` 填上。

三处细节：

- `_head` 的 docstring 说明了为什么要只读头部：「a cell may have printed gigabytes before it hung」——超时的单元格可能已经打了几个 GB，全读进内存没有意义。
- 错误文案带一句「Actions may already have happened; inspect before retrying」。这是超时语义的核心：**超时不代表没生效**。
- `error_type` 是 `CellTimeout` 或 `WorkerExited`，与正常结果里模型代码抛出的异常类型区分开。

最后 `CancelledError` 单独处理：`await self.close()` 再 `raise`。注释写「the cell may still be running」——整个 run 被取消时单元格可能还在跑，必须杀掉。

**`close()` 是可重入的，并且每个副作用都吞掉「已经没了」的情况。** 依次做：关 writer（吞 `RuntimeError`，注释说「its event loop is closed（a finished run_sync's）」，且不关会留 `CLOSE_WAIT`）、`_reaper.detach()`、`_end_worker`、若进程还在则 `proc.kill()` 并 `await asyncio.wait_for(_exited(proc), 5)`。`wait_for` 外面吞了 `TimeoutError`，注释写「reaped from any loop: SIGKILL ends it in milliseconds」。

## 关键取舍

**源码事实：** 传输是「子进程回连父进程的临时端口 + 随机令牌 + `hmac.compare_digest` 自证」，模块 docstring 明确给出的理由是跨平台、不依赖继承文件描述符。

**源码事实：** 通信进程用普通 `subprocess.Popen`，不用 asyncio 子进程，因为 `keep_alive` 的 Agent 会活过创建 worker 的那个事件循环，asyncio 的子进程管理器会随之失效。

**源码事实：** 子进程自成进程组（POSIX `start_new_session`）/ 作业对象（Windows `CREATE_NEW_PROCESS_GROUP` + `KILL_ON_JOB_CLOSE`），超时与退出都杀整棵树。

**源码事实：** 三层清理分别兜住「显式 `close()`」「对象被丢弃但没 close」「进程退出 / 收到 SIGTERM」，且 `finalize` 的 atexit 被主动关掉以保证先杀 worker 再关浏览器。

**源码事实：** `Repl.start` 在空 `contextvars.Context()` 里启动，切断 socket 读取器对 `RunContext → Session → Repl → Agent` 的引用，使未 `close()` 的 Agent 能被回收。

**源码事实：** 单元格编号从工作区已有文件的最大值续接，保证跨 run 的历史引用不被覆盖。

**源码事实：** `asyncio.Lock` 在检测到事件循环变化时被重建；worker 若属于旧循环则重启并给输出加「变量已丢失」的前缀。

**架构推论：** 「回连 + 令牌」这套设计在跨平台上胜过 fd 继承，但它引入了一个短暂的监听窗口——任何本地进程都能来敲这个端口。三层防护（10 秒超时、常量时间比较、`connected.done()` 先行判断）说明作者把这个窗口当真对待。代价是端口与令牌必须经环境变量传递，于是「环境变量」成了这套机制的信任根之一。

**架构推论：** 用普通 `Popen` 是**放弃框架协助以换生命周期的自由**。asyncio 的子进程管理带来「`await proc.wait()` 就能回收」的便利，但把生命周期钉在单个循环上；browser-use 需要的是「一个 worker 服侍多个 `asyncio.run`」，所以只能自己轮询 `poll()`。这个取舍的方向很明确：**长活对象不能依赖短活基础设施**。

**架构推论：** 「杀整棵树」不是洁癖，而是功能正确性：单元格里起的子进程可能持有 CDP 连接，一个残留进程能继续操作浏览器，而它的超时监控已经随父进程消失了。这解释了为什么 `kill_workers()` 的 docstring 要写「its timeout gone with us」。

**架构推论：** Windows 作业对象上那句「however it dies」揭示了一类设计原则：**当清理代码本身可能不执行时，要把清理责任交给内核**。这与 `weakref.finalize`（交给 Python 的 GC）和 `atexit`（交给正常退出路径）形成三级递进——三种兜底覆盖三种不同的失效层级。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/repl/session.py` | `Repl` | 父进程侧会话：起停、调用、重连、单元格编号与锁 |
| `browser_use/repl/session.py` | `Repl.start` / `_start` | 空上下文启动、监听与令牌握手、环境组装、`Popen`、init 交换 |
| `browser_use/repl/session.py` | `Repl.call` / `_call` | 串行化、协议收发、循环变更检测、超时降级、`after_cell` |
| `browser_use/repl/session.py` | `Repl.close` | 可重入关闭：writer、reaper、进程树、等待回收 |
| `browser_use/repl/session.py` | `check_mode` / `check_loop` | mode 校验；Windows 事件循环前置检查（在花钱之前） |
| `browser_use/repl/session.py` | `_kill_tree` | 分平台杀整棵进程树 |
| `browser_use/repl/session.py` | `kill_workers` / `_end_worker` | 进程级清理与单 worker 清理 |
| `browser_use/repl/session.py` | `_job_for` | Windows 作业对象（`KILL_ON_JOB_CLOSE`）的 `ctypes` 构造 |
| `browser_use/repl/session.py` | `_exited` | 轮询式回收，刻意不用 asyncio 子进程监视器 |
| `browser_use/repl/session.py` | `_head` | 只读单元格输出头部，供超时后取回部分结果 |

## 参考

- [browser-use 源码：repl/session.py](https://github.com/browser-use/browser-use/blob/main/browser_use/repl/session.py)
- [browser-use 仓库：AGENTS.md 的架构一节](https://github.com/browser-use/browser-use/blob/main/AGENTS.md)
- [Python 文档：subprocess 的 start_new_session 与 creationflags](https://docs.python.org/3/library/subprocess.html#subprocess.Popen)
- [Python 文档：weakref.finalize](https://docs.python.org/3/library/weakref.html#weakref.finalize)
- [Microsoft 文档：Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)

## 与主核 / AI 的关系

「父进程持有子进程的句柄，超时就杀整棵树」与主核里的**后端进程管理**高度同构：主核也为每个连接或每个并行任务起子进程，也必须保证「父进程死后子进程不残留」——尤其是共享内存段与文件锁会被残留进程一直占着。两边的关键机制几乎逐条对应：自成进程组、按组发信号、父死子亡的看护。

差别在「谁来保证」。主核依赖操作系统层面的 postmaster 模式（父进程是被特意保护的，子进程死了重启），而这里反过来——**子进程是被牺牲的一方**，因为它的职责是「执行一段可能永不返回的代码」。所以这里的死亡传播方向是「父死则子必死」，而数据库的方向是「子死则父重启它」。

`keep_alive` Agent 与 asyncio 子进程管理器的冲突，是异步 Python 里一个典型的生命周期错配。主核里对应的问题是「长连接持有的信号处理器属于哪个线程」——都是「基础设施对象绑定在比使用者更短的寿命上」。解法也一致：**要么让基础设施活到最长寿命，要么绕开它自己实现**。这里选了后者。

对 AI 工程的实际意义在于：任何「让模型写代码执行」的系统都会撞上同一个问题——**必须能中断**。而在 Python 里，能中断的唯一可靠方式就是独立进程。这份实现给出的三条经验值得直接抄：回连式握手（比 fd 继承更可移植）、按进程组/作业对象杀树（而不是只 kill 主进程）、以及「超时不代表没生效」这句必须写进错误信息的话。最后一条尤其重要：模型看到「超时」的第一反应是重试，而重试一个已经点过按钮的操作会造成重复提交。
