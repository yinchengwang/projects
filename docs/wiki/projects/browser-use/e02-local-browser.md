---
project: browser-use
feature: local-browser
title: 本地浏览器进程与用户目录
page_type: feature
schema_version: 1
refs:
  - path: browser_use/browser/local.py
    symbols: [LocalBrowser, start, find_binary]
  - path: browser_use/browser/local.py
    symbols: [resolve_profile, require_debug_port, chrome_args]
---

# 本地浏览器进程与用户目录

## 一句话职责

`LocalBrowser` 负责「自己起一个 Chrome」这一件事的全部脏活：找一个可用的二进制、组出正确的启动参数、处理 `user_data_dir` 与 profile 副本、等它把 DevTools 端口写进文件、以及在停止时按正确顺序收尾——外加一个守护进程，保证本进程被强杀时 Chrome 与临时 profile 都会被清掉。

## 机制是什么

这个模块的存在理由写在它的模块 docstring 里：「Local browsers `Browser` launches itself: `Browser('chromium')`（a fresh profile）and `Browser('chrome', profile=...)`（a temporary copy of one of your Chrome profiles）. Attaching to the Chrome you have open stays in browser-harness.」

也就是说它只管**自己启动**的浏览器，附着到用户已开 Chrome 的逻辑在 browser-harness 里。这条边界让职责非常清楚。

它要解决的问题可以归成四类：

| 问题 | 手段 |
|---|---|
| 从哪找 Chrome | `find_binary` 的优先链 + Snap 排除 |
| 用什么 profile | 临时目录（`Browser('chromium')`）或真实 profile 的副本 |
| 怎么知道它起来了 | 轮询 `DevToolsActivePort` 文件与 stderr |
| 怎么保证不留垃圾 | 临时目录 + 所有者锁 + 守护进程 |

第四类里最不显眼但最重要的一点：**临时目录里装的是用户的 cookie 副本**。所以清理不只是整洁问题，而是安全问题——注释在 `sweep_stale_dirs` 里写得很直接：「a profile copy holds the user's cookies and saved passwords」。

## 怎么实现的

**`LocalBrowser` 的四件状态。** `__init__` 先 `sweep_stale_dirs()`（清掉死进程的残留），然后：

```python
self.tmp = Path(tempfile.mkdtemp(prefix=TMP_PREFIX))
self._owner = open(self.tmp / OWNER, 'wb')
_try_lock(self._owner)
```

`_owner` 是一个**每进程持有的独占锁文件**。`_try_lock` 分平台用 `msvcrt.locking(LK_NBLCK)` 与 `fcntl.flock(LOCK_EX | LOCK_NB)`，docstring 一句话点明性质：「the OS drops it when the process dies, however it dies」。所以这把锁是一个**无需清理的存活标记**——进程怎么死的都不重要，锁一定会被内核释放。

`sweep_stale_dirs()` 靠这把锁判断目录是否可删：遍历 `browser-use-chrome-*` 目录，跳过「创建不到 60 秒」的（注释「its owner may not hold the lock yet」——这是一处竞态防护，`mkdtemp` 与 `_try_lock` 之间有窗口），然后尝试加锁：加到就说明原主已死，`shutil.rmtree`。

这个「跳过 60 秒内新建的」判断很关键：没有它，两个进程同时启动时，前者刚建的目录可能被后者立刻删掉。

**`start()` 与 `_start()` 的分工是「兜住取消」。** `start` 只包一层：

```python
try:
    return self._start(...)
except BaseException:
    if self._stopped:  # stop() ran mid-start: a profile copy still running in this thread recreated the dir
        self._remove_tmp()
    raise
```

注释说明了那个具体场景：`stop()` 在启动过程中被调用（例如 `Browser.start()` 被取消），此时正在这个线程里跑的 profile 拷贝可能已经把删掉的临时目录重新创建出来，所以要再删一次。类 docstring 也重申了这个保证：「`stop()` kills it and deletes everything it created; safe while `launch()` runs in another thread（a cancelled `Browser.start`）, so a Chrome is never left behind.」

**`_start()` 的四步。**

1. **`find_binary(executable_path, chrome_only=profile is not None)`**——`chrome_only` 的取值在这里就定下了：要拷真实 Chrome profile 时必须用真 Chrome。
2. **`headless = not has_display() if headless is None else headless`**——默认值是「没显示器就无头」。
3. **决定 `user_data_dir`**：有 `profile` 就从真实目录拷一份到 `self.tmp / 'profile'`；否则用传入的目录（保留），或 `self.tmp / 'profile'`（临时）。后者还会 `(udd / 'DevToolsActivePort').unlink(missing_ok=True)`——清掉上一次留下的文件，否则会读到过期端口。
4. **`chrome_args(...)` 组参数后 `_launch`**，并对「No usable sandbox」做一次重试：注释「Linux without user namespaces or a setuid sandbox helper（some CI runners, VMs）」——只在错误文本匹配且参数里还没有 `--no-sandbox` 时，才在 `about:blank` 之前插入它重试一次。

**`chrome_args()` 是一份「每条都有出处」的参数清单。** 除了两个必需的（`--user-data-dir`、`--remote-debugging-port=0`）与两个降噪的（`--no-first-run`、`--no-default-browser-check`），其余每条都带注释：

- **`--remote-debugging-port=0`**：让 Chrome 自己选端口，然后写进 `DevToolsActivePort`。这是避免端口冲突的关键——不指定 9222，就不会与用户自己的调试端口撞车。
- **`--allow-pre-commit-input`**：注释「0.x and Playwright: keys and clicks reach a tab that has not painted yet」。即不等渲染就允许输入，否则快速操作会被丢。
- **三个 `--disable-*-throttling`/`-backgrounding`**：注释「a page the agent is not looking at（a background tab; any tab of a headed window that is covered or behind a locked screen）keeps running its timers and rendering, instead of ~1 timer tick a second」。这条对 agent 至关重要：它经手的标签页常常不是前台可见的。
- **`fresh` 分支的 keychain 参数**：`--use-mock-keychain`（macOS）/`--password-store=basic`（Linux），注释「a profile of our own never asks for（or reads）the OS keychain; a copied profile needs the real key」。即临时 profile 不该碰钥匙串（会弹窗、也无意义）。
- **`else` 分支的 `--disable-sync`**：注释写得很重：「a copy is signed in to the user's Google account: sync would write the agent's history, tabs and saved passwords to the account and from there into the real profile」。这是一条真实的数据污染路径——副本里登录着真实账号，同步会把 agent 的操作写回账号。
- **`--headless=new` + `--window-size=1440,900`**：无头时的窗口尺寸，与云端浏览器请求的尺寸一致（`_start_cloud` 里传的是 1440×900）。
- **容器/root 的 `--no-sandbox`**：判据是 `os.geteuid() == 0` 或存在 `/.dockerenv` / `/run/.containerenv`。
- **`--profile-directory=<dir>`**：拷副本时要告诉 Chrome 用哪个子目录。
- 最后一定追加 `about:blank`。

**`find_binary()` 是一条六段的优先链，每段都有理由。**

1. `executable_path`——用户显式指定，且校验是文件；若是 Snap 则直接报错（`_snap_error`）。
2. `BH_CHROME_PATH` / `CHROME_PATH`——browser-harness 的变量名。注释「it silently fell back to another browser」解释为什么这些缺失时会 warning：用户以为指定了，实际用了别的。
3. 平台特定路径：macOS 看 `/Applications` 与 `~/Applications` 下的四个 `.app`；Windows 看三个环境变量根下的 Chrome/Chromium/Edge；Linux 用 `shutil.which` 找 `google-chrome-stable` 等。
4. `playwright_chromium()`——`browser-use install` 装的 Chromium，注释「last: an installed Chrome wins」。它会按构建号**从新到旧**遍历 Playwright 缓存目录。
5. **Snap 收集与拒绝**：遍历时遇到 Snap 的路径先攒着，找不到别的再抛 Snap 的错误。
6. 报错带完整 tried 列表，并给出三个具体出路（安装、`browser-use install`、`executable_path` / `BH_CHROME_PATH`）。

`chrome_only` 分支的报错文案单独一句：「a copied Chrome profile needs real Chrome: Playwright's Chromium cannot decrypt its cookies」。

Snap 的判定（`is_snap`）值得单看：它在 Linux 上读文件头 4KB，判 `'/snap/' in real` 或 shebang 里含 `/snap/`——后者覆盖 `/usr/bin/chromium-browser` 这种 wrapper 脚本。拒绝理由在 `_snap_error` 里：「Snap Chromium, which cannot open DevTools for a browser it did not start itself」；而 `is_snap` 的 docstring 给了更底层的机制：「Its confinement hides our `/tmp` profile from it, so it never reports a DevTools port」。

**profile 的解析与拷贝。**

`resolve_profile(root, name)` 读 `Local State` 的 `profile.info_cache`，得到「目录名 → 显示名」映射，然后**两轮匹配**：先精确比 目录名或显示名，再 casefold 比。命不唯一或没命中时，错误里列出所有 profile（`'显示名' (目录名)` 形式），让用户看到可选值。

`copy_profile(root, directory, dest)` 只拷两样：`Local State`（注释「it holds the cookie key on Windows and Linux」）与那个 profile 目录，忽略 `SKIP_COPY` 一类缓存。它的错误处理是这一段里最实用的：

```python
except shutil.Error as e:  # copytree copies everything it can, then reports the files it could not read
    skipped = sorted({Path(err[0]).name for err in e.args[0] if isinstance(err, tuple) and err})
    if 'Cookies' in skipped:
        raise RuntimeError('Could not copy the cookies ... Chrome holds them locked (Windows does while Chrome runs). Quit Chrome and run again.') from None
    logger.warning('Copied Chrome profile %r without files Chrome holds open: %s', ...)
```

即区分「关键文件拷不到」与「其他文件拷不到」：`Cookies` 缺失意味着副本里没有任何登录态（那就失去了拷 profile 的意义），所以硬失败并要求退出 Chrome；其他文件（日志、锁）缺失只警告。这是一处对**目标**的判断——拷 profile 的目的就是带登录态，少了它就等于没拷。

**等待 Chrome 起来：两个信号源。** `_ws_url(user_data_dir, log)` 先读 `DevToolsActivePort`（Chrome 在那里写两行：端口与 `/devtools/browser/<id>`），再退回正则扫 stderr 里的 `DevTools listening on (ws://\S+)`。docstring 说「Chrome writes its browser websocket to `DevToolsActivePort` and stderr once the DevTools port is open」——两个来源互为兜底。

`_launch` 的主循环把这三件事并行判：

```python
while time.monotonic() < deadline and not self._stopped:
    if ws := _ws_url(...): return ws
    if (code := proc.poll()) is not None:
        tail = log.read_bytes()[-600:].decode(errors='replace').strip()
        lock = '' if self.user_data_dir.is_relative_to(self.tmp) else f' is another Chrome using {self.user_data_dir}?'
        raise RuntimeError(f'{Path(binary).name} exited at start (code {code});{lock} {tail}')
    time.sleep(0.05)
```

三处细节：**进程早退立即失败**（不等满 30 秒）；失败时附上日志尾 600 字节；并且只在「`user_data_dir` 是保留目录而非临时目录」时才猜测「是不是另一个 Chrome 占着它」——注释「a temp or copied profile is new to this run」，即临时目录不可能是别人占的，猜这句只会误导。

`_launch` 里还有一处竞态处理：

```python
with self._lock:
    if self._stopped: raise RuntimeError('the browser was stopped while it started')
    self.proc = subprocess.Popen(...)
```

锁内检查 `_stopped` 再起进程，与 `stop()` 的锁内互斥，保证「不会在停止之后起一个新进程」。这是与前面 `start()` 里那句「safe while launch() runs in another thread」配套的实现。

启动参数：`stdin=DEVNULL`、`stdout=DEVNULL`、`stderr=<文件>`、`start_new_session=True`（非 Windows）/ `CREATE_NEW_PROCESS_GROUP`（Windows）。注释「its own process group: `_kill` reaches every helper」。

**停止的顺序是这一页最精细的部分。** `stop()` 依次：

1. 先杀 watchdog（注释「first: a normal stop must not race it」）——否则正常停止与看护进程的清理会撞车。
2. 锁内置 `_stopped = True` 并取出 `proc`。
3. `_kill(proc, self.ws)`。
4. `_remove_tmp()`——先 `self._owner.close()` 放锁，再最多重试 20 次删目录（每次间隔 100 毫秒），注释「Windows keeps files locked for a moment after the processes exit」。

`_kill` 的三段式本身就是一篇小论文：

```python
if proc.poll() is None:
    if not (ws and _close_over_cdp(ws)) and not win:
        with contextlib.suppress(OSError): proc.send_signal(signal.SIGTERM)
    try: proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill() if win else send(signal.SIGKILL)
        ...
if not win:
    send(signal.SIGKILL)  # helpers still in the group after the browser process exited
```

docstring 解释了为什么不用「SIGTERM 整组」：「A SIGTERM to the whole group killed the network service with the browser, before it wrote the cookies of a user_data_dir profile.」这是关键——**先礼后兵必须只针对主进程**，因为 Chrome 需要网络进程还活着才能把 cookie 落盘。优雅路径是 `_close_over_cdp(ws)` 发 `Browser.close`，docstring 说它「shuts down as when its last window closes, writing cookies and storage to the profile first」。只有在 CDP 关闭失败（且非 Windows）时才退回对单进程发 SIGTERM。最后才对整组 SIGKILL 清理残留 helper。

Windows 那两处细节也各有一句注释：`SIGTERM` 对 Windows 就是 `TerminateProcess`，「no cleanup at all」；所以优先走 CDP 关闭。

**看护进程（watchdog）是「本进程被强杀」时的最后一道。** 它的机制很巧妙，写在 `WATCHDOG` 常量前的注释里：**stdin 是一个只有启动它的 Python 进程持有的管道**，所以 `sys.stdin.buffer.read()` 会在那个进程死掉时返回 EOF——「however it dies（SIGKILL, OOM）」。醒来后它做四件事：停云浏览器（否则计费到超时）、`_ipc.identify` 找当前守护进程、杀 Chrome 进程组、删临时目录。

两处防误杀的措施值得记：

- **`admin._process_start_time(pid)` 校验**：注释「never a process that reused the number」。即 pid 会被复用，所以要用「进程启动时间」做身份确认；不顺带解释了 Chrome 的情况：「one that exited first（crashed, quit by the user）left its pid free, and the group that reused it was killed」——所以先校验再杀。
- **`if chrome:  # never 0: killpg(0) is this watchdog's own group`**：一个非常具体的防御——`chrome` 变量可能被上一段置 0，而 `killpg(0, ...)` 会杀掉看护自己的进程组。

`start_watchdog` 还有个安全细节：云浏览器的 id 放进**环境变量**而不是 argv，注释「which only this user can read, never argv（any local user's `ps`）」。因为那个 id 能无 key 驱动浏览器。

`start_new_session=True` 也带注释：「a Ctrl-C to our process group does not end it first」——看护进程必须活得比父进程久，所以不能被同一次 Ctrl-C 带走。

**`require_debug_port()` 是一段安全防护，不是便利函数。** 它的 docstring 交代了一个真实漏洞：「With none, browser-harness 0.1.13 attached to whatever answered on 127.0.0.1:9222 / 9223, which any local user or process can open（another user's Chrome, a fake one）: the agent then typed your secrets into their browser.」所以这里的策略是**绝不猜端口**：

1. 先查「有没有残留的 `DevToolsActivePort` 而其端口已被别的进程占着」——这种情况 browser-harness 会连到那个进程，所以明确要求删文件。
2. 有活跃的调试目录 → 通过。
3. 没有显示器（服务端/容器）→ 报错并给出两条出路（`BU_CDP_URL` 或云端）。
4. 尝试启动浏览器并等最多 `TOGGLE_BOOT_GRACE`（或 3 秒）。
5. 都不行 → 打开 `chrome://inspect` 并要求用户勾选，错误里明确写「Ports 9222/9223 are never guessed」。

`_left_over` 与 `_holder_wrote_it` 是这套判断的技术核心。前者比「文件存在」更细：`DevToolsActivePort` 的 mtime 早于 `SingletonLock` 的 mtime，或没有浏览器真的运行在那个 profile 上，就说明这个文件是**上一次会话留下的**（Chrome 退出时不删它）。后者是更进一步的证明：「文件里写的端口上，服务的是不是文件里写的那个 path」——若是，说明写文件的那个浏览器仍握着端口，即使它跑在另一个 PID namespace 里（注释给的例子是共享 home 与 localhost 的 sidecar 容器）。

`require_debug_port_unless_attached()` 是它的宽容版：若指定名字的守护进程已经能连上它的浏览器（`admin.require_existing_daemon`），就跳过全部检查——因为 browser-harness 会复用它而不做发现。

## 关键取舍

**源码事实：** 每个 `LocalBrowser` 的临时目录里放一个 `.owner` 锁文件，进程存活期间持有独占锁；`sweep_stale_dirs` 靠「能否加锁」判断目录是否可以删，并跳过创建不足 60 秒的目录以避开竞态。

**源码事实：** 临时目录里可能装着用户 profile 的副本（含 cookie 与保存的密码），所以清理是安全要求而不只是整洁要求。

**源码事实：** Chrome 用 `--remote-debugging-port=0` 自选端口并写进 `DevToolsActivePort`，`_ws_url` 同时从该文件与 stderr 读取，互为兜底。

**源码事实：** 复制 profile 时 `Cookies` 拷不到就硬失败（要求退出 Chrome），其他文件拷不到只警告。

**源码事实：** 停止时优先走 CDP `Browser.close`；SIGTERM 只发给浏览器主进程而**不是整组**，因为整组会先杀掉网络进程、导致 cookie 未落盘；最后才对整组 SIGKILL 清残留 helper。

**源码事实：** 看护进程用「stdin 管道的 EOF」感知父进程死亡，并用「进程启动时间」校验 pid 身份，避免杀掉复用了同一 pid 的无关进程。

**源码事实：** `require_debug_port()` 绝不猜 9222/9223，理由是 browser-harness 曾因此连到别的本地进程，导致敏感值被打进别人的浏览器。

**架构推论：** 这个模块的复杂度几乎全部来自「进程与文件系统的不确定性」。Chromium 启动是异步的（要等它写文件）、它的退出是分阶段的（cookie 需要网络进程活着）、它的 pid 会被复用、它的临时文件会被别的进程占着、它可能根本不在本 PID namespace 里。所有那些「校验启动时间」「等列表刷新」「重试删目录 20 次」的代码，都是对这类不确定性的具体回应。

**架构推论：** 「用锁文件标记存活」是一个跨平台的经典技巧，但它的价值在这里被放大了：因为清理的对象是**含凭证的目录**，所以「无法证明原主已死就不能删」与「能证明就一定要删」这两个方向都必须对。60 秒的宽限期与 `_try_lock` 的原子性共同保证了这两点。

**架构推论：** `require_debug_port` 那段是一次「防御性拒绝设计」的范例：它宁可让用户手动开一个开关、宁可多给三种出路，也不肯自动去试一个固定端口。这类决定通常只有在出过真实事故之后才会被写进代码——docstring 里那句「the agent then typed your secrets into their browser」就是事故记录。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/browser/local.py` | `LocalBrowser` | 一个自启 Chrome 的完整生命周期：临时目录、锁、启动、看护、停止 |
| `browser_use/browser/local.py` | `LocalBrowser.start` / `_start` | 取消安全的启动包装；二进制、headless 默认、profile 目录、参数组装 |
| `browser_use/browser/local.py` | `LocalBrowser._launch` | `Popen` 与等待 `DevToolsActivePort`，含早退失败与日志尾 |
| `browser_use/browser/local.py` | `LocalBrowser.stop` / `_remove_tmp` | 先杀看护、再杀进程、最后重试删目录 |
| `browser_use/browser/local.py` | `find_binary` | 六段优先链，含 Snap 拒绝与带出路的报错 |
| `browser_use/browser/local.py` | `chrome_args` | 每条参数都带机制注释（输入、节流、keychain、sync、headless） |
| `browser_use/browser/local.py` | `resolve_profile` / `copy_profile` | 两种匹配的 profile 解析；只拷 `Local State` 与目标目录 |
| `browser_use/browser/local.py` | `require_debug_port` | 绝不猜端口的调试端口前置检查与用户指引 |
| `browser_use/browser/local.py` | `_left_over` / `_holder_wrote_it` | 用文件时序与端口实际应答判断「残留 vs 活跃」 |
| `browser_use/browser/local.py` | `WATCHDOG` / `start_watchdog` | 用 stdin EOF 感知父进程死亡的清理进程 |
| `browser_use/browser/local.py` | `_kill` / `_close_over_cdp` | 先 CDP 关闭、再单进程 SIGTERM、最后整组 SIGKILL |

## 参考

- [browser-use 源码：browser/local.py](https://github.com/browser-use/browser-use/blob/main/browser_use/browser/local.py)
- [Chrome 文档：DevToolsActivePort 与远程调试](https://developer.chrome.com/blog/remote-debugging-port)
- [browser-harness 仓库](https://github.com/browser-use/browser-harness)
- [Python 文档：tempfile 与 shutil.ignore_patterns](https://docs.python.org/3/library/tempfile.html)
- [Python 文档：fcntl.flock 与 msvcrt.locking](https://docs.python.org/3/library/fcntl.html)

## 与主核 / AI 的关系

「自己起一个外部进程，并保证它一定被收掉」与主核里的**外部进程管理**完全同构：主核启动 worker 进程、以及调用外部程序（`COPY ... PROGRAM`、扩展里的 shell 调用）时，都必须回答同一组问题——怎么找到可执行文件、怎么传参、怎么判断它真的起来了、怎么在父进程异常死亡时清理。这份实现给出的答案（临时目录 + 存活锁 + 用 stdin EOF 做死亡感知）可以直接照搬。

「停止顺序决定数据完整性」这一条尤其值得对照主核。主核写 WAL 与数据页时也讲究顺序：先落盘再改状态。这里对应的是「必须先让 Chrome 自己优雅关闭（写好 cookie），再考虑强杀」。而它踩过的坑——「对整组发 SIGTERM 会先杀掉网络进程，导致 cookie 没写进去」——与主核里「先关掉 writer 再 flush 会丢数据」是同一类错误：**杀一个多进程协作体时，顺序错了就等于跳过了它内部的落盘步骤**。

`require_debug_port` 那段与主核的安全配置（`pg_hba.conf`、监听地址）同类：都是「默认不猜、必须显式确认」。主核不会自动监听所有网卡，这里不会自动试 9222——两者的理由相同：**宽松的默认值一旦被踩到，后果是数据/凭证外泄**，而代价只是用户多一步配置。

对 AI 工程的实际意义在于：用「起一个浏览器」当作 agent 的执行环境，会把大量操作系统层面的细节拉进框架。这份实现给出的可借鉴做法有四条：把临时目录的所有权用锁文件表达（进程怎么死都能回收）、把 CPU 降频相关的 chrome flag 全部关掉（agent 经手的标签常常不在前台）、把「这个参数为什么在这里」逐条写进注释（否则半年后没人敢删）、以及绝不为了「省一步用户操作」去猜一个调试端口。
