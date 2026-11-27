---
project: browser-use
feature: agent-entry
title: Agent 门面与运行生命周期
page_type: feature
schema_version: 1
refs:
  - path: browser_use/agent/service.py
    symbols: [Agent, run, build]
  - path: browser_use/agent/service.py
    symbols: [close, __aenter__, _shutdown]
---

# Agent 门面与运行生命周期

## 一句话职责

`Agent` 是 0.x 那套 `Agent(task).run() -> result.final_result()` 的门面：它把「解析模型 → 起浏览器 → 起 REPL → 跑 Pydantic AI 循环 → 收尾、判分、遥测」串成一次 `run()`，并保证浏览器（会按分钟计费）在正确的时刻启动、在正确的时刻停掉。

## 机制是什么

`service.py` 的模块 docstring 只有一句，但把定位交代完了：**「0.x API 跑在 Pydantic AI 的 REPL 引擎上」**。这句话决定了这个类的全部职责——它自己不实现 agent 循环（那在 `loop.py`），只做四件事：

1. **把 0.x 的调用形态翻译成 1.0 的引擎参数**（含大量已移除参数的告警与拒绝）。
2. **管理三份资源的生命周期**：浏览器、REPL 工作进程、`run_sync()` 的事件循环。三者都昂贵，且浏览器按时间计费，所以「什么时候起、什么时候停」是有商业含义的。
3. **把一次 run 的边界画出来**：预检 → 起浏览器 → 跑循环 → 停浏览器 → 判分 → 上报。顺序不能随意换。
4. **持有跨 run 的状态**：`_history`（续跑用）、`_compacted`（检查点）、`_preflighted`（只预检一次）、`_repl`（工作进程活着的就复用）。

它的公开面只有 `run()`、`run_sync()`、`close()`、`close_sync()`、`add_new_task()`、`workspace` 与 `__aenter__`/`__aexit__`。而 `build` 是私有 `_build()`——注意名字上没有下划线的那个 `build` 在 `loop.py` 里是 `build_agent`，两者不是一回事。

## 怎么实现的

**`run()` 只做参数校验与重入保护，真正的逻辑在 `_run()`。**

```
async def run(self, max_steps: int = 100, timeout: float = 1700, **kwargs) -> AgentHistoryList:
    if kwargs: raise _unsupported(kwargs, 'Agent.run')
    if isinstance(max_steps, bool) or not isinstance(max_steps, int) or max_steps < 1:
        raise ValueError(...)
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not timeout > 0:
        raise ValueError(...)
    if self._running:
        raise RuntimeError('This Agent is already running; await that run() first, ...')
    if self.browser._user is not None:
        raise RuntimeError('This Browser is in use by another running Agent. ...')
    self._running, self.browser._user = True, self
    try:
        return await self._run(max_steps, timeout)
    finally:
        self._running, self.browser._user = False, None
```

两处细节值得记住。第一，`isinstance(max_steps, bool)` 被单独拦——因为 `True` 是 `int` 的子类，不拦就会把 `max_steps=True` 当成 1 步。第二，重入保护是**双向**的：Agent 侧用 `_running` 防止同一个 Agent 两个并发 run（注释说「两个 run 会各起一个 REPL，一个泄漏，还会互相关掉对方的浏览器」），浏览器侧用 `browser._user` 防止两个 Agent 抢同一个浏览器（注释：「两个 Agent 在一个浏览器上会驱动同一个标签页，先结束的那个会把它关掉」）。`_user` 在 `finally` 里清空，于是异常路径也解锁。

`run()` 的 docstring 还把 `max_steps` 的语义钉成 0.x 行为：「最后一次是只投递的一轮（`max_steps=1` 就只投递），之后可能还有一次投递修复」。以及 `timeout` 的边界：「只约束 agent 循环（模型请求与单元格）；起浏览器在此之前、停浏览器（含云端下载）在此之后，都不计入」。

**`_run()` 是一段长事务，它的骨架是「预算之外」与「预算之内」两段。**

预算之外（不受 `timeout` 约束）依次是：`_check_task()`、`raise_fd_limit()`、`_build()`（首次）、预检、建工作区目录、`browser.start()`。每一段注释都在解释「为什么必须在这里」：

- `_build()` 被包在 `try/except MissingAPIKeyError` 里，并在捕获时调 `_capture_gate`——因为「缺失密钥的 run 也得被统计到」。
- 预检放在起浏览器**之前**：「一个死掉的网关、一个被拒的密钥、一个错误的模型或缺失的搜索端点，都在任何浏览器启动之前失败」。预检本身走 `asyncio.to_thread`，因为它是同步 HTTP。
- 工作区目录 `.browser-use` 与 `downloads` 的创建也在起浏览器之前，注释：「不可写的工作区在这里失败，早于浏览器开始计费」。

预算之内是 `session = Session(...)` 与 `result = await run(...)`。注意 `Session` 的构造把 `**compacted` 展开进去：

```
history, self._continue = (self._history if self._continue else None), False
compacted = self._compacted if history else {}
session = Session(repl=self._repl, timeout_s=timeout, max_steps=max_steps, vision=self.use_vision, rec=rec, **compacted)
```

即「续跑时把上一轮的检查点（`checkpoint` / `covered` / `usage_after`）带进新 Session」。检查点跨 run 存活，靠的是 Service 持有 `_compacted` 字典而不是 Session 持有。

**REPL 的复用与重建判定。**

```
if self._repl is None or env != self._repl.env:   # 或浏览器被停掉又重新起过
    if self._repl is None:
        browser_s = time.monotonic() - started
    else:
        await self._repl.close()
    self._repl = Repl(self.workspace, env, mode=..., secrets=..., reattach=..., after_cell=..., search=..., downloads=..., rotate_proxy=..., rotate_proxy_cdp=..., highlight_actions=..., feedback=...)
```

判定条件是「没有 REPL」或「浏览器给了不同的环境」。注释补充：「或自那以后浏览器被停掉又起过」。这条设计让 `Browser(keep_alive=True)` 场景下的多次 `run()` 复用同一个工作进程（Python 变量因此跨 run 存活），而一旦浏览器换了（新端口、新会话），REPL 必须重建。

`Repl(...)` 的十三个参数值得当作「父进程要传给工作进程的全部东西」来读：工作区、环境、模式、敏感值、是否重连、每单元格回调、搜索器、下载列举器、两条代理轮换开关、高亮开关、反馈通道。这些都是**行为**而非配置——它们决定了工作进程里能做什么。

**`finally` 是这一页最该细读的地方。**

```
finally:
    _ACTIVE[0] -= 1
    t, loop_phase = now(), phase
    try:
        phase = 'shutdown'
        self.browser.note_downloads(...)
        if not self.browser.keep_alive:
            await self._shutdown()
        else:
            await asyncio.to_thread(self.browser.remember_tab)
            cancelled = (task := asyncio.current_task()) is not None and task.cancelling() > 0
            if self.browser.use_cloud and self.browser.id:
                deadline = time.monotonic() + browser_module.CANCEL_DOWNLOADS_TIMEOUT_S if cancelled else None
                await self.browser._fetch_downloads(...)
        if result is not None:
            result.downloads = sorted(p for p in set((self.workspace / 'downloads').glob('*')) - before if p.suffix != '.crdownload')
        phase = loop_phase
    finally:
        rec.phase('shutdown', now() - t)
        try:
            if error_type is None:
                await self._judge(result)
        finally:
            self._capture(...)
```

四层嵌套，每层的顺序都有理由：

- **先停浏览器，再判分**。注释写明：「在浏览器停掉之后（云端浏览器到那时仍在计费）；被取消的判分仍会发出事件」。
- `keep_alive=True` 时不停浏览器，改为 `remember_tab`——注释解释：「守护进程在下一轮之前死掉时，后续要能重新接上」。
- 被取消时给云端下载一个截止时间（`CANCEL_DOWNLOADS_TIMEOUT_S`），注释：「仍在拷贝的文件留给下一轮或 `browser.stop()`」。
- `result.downloads` 用「运行前快照的差集」算出，并排除 `.crdownload`（未完成的下载）。这是一个朴素但正确的做法：不信任浏览器报告，而是看文件系统。
- `_capture` 放在最内层 `finally`，所以即使判分抛错，遥测也会发出去。

**`_shutdown` 的两步顺序是有代价考量的。**

```
async def _shutdown(self) -> None:
    try:
        if self._repl:
            repl, self._repl = self._repl, None
            await repl.close()
    finally:
        self._downloads = await self.browser.stop(downloads_dir=self.workspace / 'downloads') or []
```

docstring 写明：「浏览器即使 REPL 的 close 失败或被取消也要停（云端浏览器到那时仍在计费）；取消仍然会抛出」。`try/finally` 而不是「失败就跳过」，再加一句「取消仍然会抛出」——即不吞异常，但保证浏览器停掉。这是「钱的优先级高于异常的整洁」的直白表达。

**`close()` 与 `close_sync()` 处理的是事件循环归属问题。**

```
async def close(self) -> None:
    try:
        if self.browser.keep_alive:
            if self._repl:
                await self._repl.close()
                self._repl = None
            return
        await self._shutdown()
    finally:
        runner = self._runner
        if runner is not None and runner.get_loop() is not asyncio.get_running_loop():
            self._runner = None
            runner.get_loop().close()  # 空闲：它的 REPL 已经没了；Runner.close() 不能在另一个循环里跑
```

`keep_alive` 时只关 REPL 不关浏览器。而 `finally` 里那句判定的意思是：如果 `run_sync()` 那个循环不是当前循环，就可以安全地直接 `close()` 它（因为 REPL 已经没了，循环是空闲的）；如果是同一个循环，就不能关（会把自己的循环关掉）。

`close_sync()` 反过来：「在 `run_sync()` 的事件循环上跑 `close()`（REPL 工作进程属于它），然后关掉那个循环」。没有 runner 时退化成 `asyncio.run(self.close())`。

这个「谁拥有事件循环」的问题根源在 `run_sync` 的 docstring 里：「一个事件循环服务每一次 `run_sync()`，只要 REPL 还活着（`Browser(keep_alive=True)`），因为 REPL 工作进程的管道属于启动它的那个循环」。管道绑循环，循环就不能随意关——于是需要两个 `close`。

**`__aenter__` / `__aexit__` 只是把 `close()` 挂到上下文管理器上。**

```
async def __aenter__(self) -> Agent: return self
async def __aexit__(self, *exc: object) -> None: await self.close()
```

`__aexit__` 不吞异常（返回 `None`），所以 `async with Agent(...) as agent:` 退出时一定会关资源，而异常照常传播。

**满意度判定的顺带事实。** `_run` 里 `self._history = session.messages` 的注释是「原始的是它：`result.messages` 里的敏感值已脱敏」——即续跑要用未脱敏的原文（因为「提供方会拒绝被改过的签名思考块」），而对外暴露的结果用脱敏版。这条区分在 b03 页会再出现一次。

## 关键取舍

**源码事实：** `run()` 只做参数校验与双向重入保护（Agent 的 `_running` + 浏览器的 `_user`），业务逻辑全在 `_run()`；两层保护都在 `finally` 里清空。

**源码事实：** 所有「会花钱」的动作都被推到预检之后：`_build()`、预检、工作区目录创建、`browser.start()`。注释逐处点明「早于浏览器开始计费」。

**源码事实：** `finally` 的顺序是「停浏览器 → 判分 → 遥测」，且遥测在最内层 `finally`，所以判分失败也会发事件。

**源码事实：** `_shutdown` 保证浏览器在 REPL close 失败或被取消时仍被停掉，且不吞异常。

**源码事实：** 下载结果由「运行前后目录快照的差集」算出并排除 `.crdownload`，不依赖浏览器的自述。

**源码事实：** `close()` 与 `close_sync()` 并存，是因为 REPL 工作进程的管道绑定在启动它的事件循环上。

**架构推论：** 「门面 + 引擎」的切法让 0.x 的调用形态完整保留，而引擎可以整体替换（这里就是整体换成了 Pydantic AI 的循环）。代价是门面必须承担全部兼容性负担——`REMOVED` 那张六十多条的映射表就是证据。

**架构推论：** 把「资源生命周期」与「一次 run 的语义」写在同一个方法里（`_run` 加它的 `finally`），好处是顺序约束看得见；坏处是这个方法长到需要靠 `phase` 变量标注当前阶段（遥测里也确实上报 `error_phase`）。这是为了可观测性做出的让步：阶段名不是给读者看的装饰，而是故障排查时的第一手信息。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/agent/service.py` | `Agent` | 0.x 门面：统一入口、状态持有、资源编排 |
| `browser_use/agent/service.py` | `Agent.run` | 参数校验 + 双向重入保护，转发 `_run` |
| `browser_use/agent/service.py` | `Agent._run` | 一次 run 的长事务：预检 → 起浏览器 → 循环 → 收尾 → 判分 → 遥测 |
| `browser_use/agent/service.py` | `Agent._build` | 解析模型、组装指令、构建 Pydantic AI agent（懒执行，首次 run 时） |
| `browser_use/agent/service.py` | `Agent.close` | 关 REPL（`keep_alive` 时只关这个），并按需关闭外部事件循环 |
| `browser_use/agent/service.py` | `Agent._shutdown` | 停 REPL 与浏览器，浏览器优先保证被停 |
| `browser_use/agent/service.py` | `Agent.__aenter__` / `__aexit__` | 上下文管理只挂 `close()`，不吞异常 |

## 参考

- [browser-use 源码：agent/service.py](https://github.com/browser-use/browser-use/blob/main/browser_use/agent/service.py)
- [browser-use 文档：入门](https://docs.browser-use.com/quickstart)
- [browser-use 仓库：MIGRATING.md](https://github.com/browser-use/browser-use/blob/main/MIGRATING.md)
- [Pydantic AI 文档：Agent](https://ai.pydantic.dev/agent/)

## 与主核 / AI 的关系

「门面持有资源、引擎只跑逻辑」这条分法与主核里「连接管理器持有连接池、执行器只跑计划」完全同构。主核里同样有「什么时候开事务、什么时候提交」这类顺序约束，写错不会报错但会泄漏资源；这里写错的代价更直观——云端浏览器按时间计费，所以 `finally` 里每个位置都被注释解释了。

对 AI 工程的实际意义集中在两点。第一，**钱的位置决定代码的顺序**。`_build`、预检、目录创建全被推到 `browser.start()` 之前，而停浏览器被放在判分之前，两处注释都用「到那时仍在计费」说明理由。一个 agent 框架最容易出的事故就是「参数写错了，先起了浏览器，再在第一次模型请求上 400」——这里把能静态检查的东西全提前到 `__init__`（见 a02），把需要网络的检查全提前到浏览器之前。

第二，**取消不等于放弃清理**。`_shutdown` 的 `try/finally` 加「取消仍然抛出」，是拆除代码的标准姿态：不吞异常（否则上层看不到取消），但保证最贵的资源被释放。这一条在长时任务里尤其重要，因为「被取消的 run 留下一个计费中的浏览器」比一次正常失败要贵得多。
