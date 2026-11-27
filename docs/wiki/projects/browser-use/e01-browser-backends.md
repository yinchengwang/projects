---
project: browser-use
feature: browser-backends
title: 浏览器后端选择与连接
page_type: feature
schema_version: 1
refs:
  - path: browser_use/browser/browser.py
    symbols: [Browser, start, reattach]
  - path: browser_use/browser/browser.py
    symbols: [is_cloud_cdp, _kind, _ensure_daemon]
---

# 浏览器后端选择与连接

## 一句话职责

`Browser` 是「一个可供 agent 操作的浏览器」的统一门面：它从四种后端里选一种（云端浏览器 / 本地新起 Chromium / 已开的 Chrome / 给定的 `cdp_url`），为选中的后端起一个 browser-harness 守护进程，再把守护进程需要的环境变量交给 REPL worker。

## 机制是什么

「后端」这个词在这里有两层含义，容易混淆，先分清：

| 层 | 是什么 | 谁管 |
|---|---|---|
| 浏览器本身 | 云端实例、新起的 Chromium、用户的 Chrome、外部 CDP 端点 | `Browser`（本页）或 `LocalBrowser`（见 e02） |
| 协议接入层 | browser-harness 的**守护进程**（daemon），提供 `_send` 式 IPC + 事件缓冲 | 始终由 `Browser` 管 |

所以无论后端是哪种，模型侧的代码都只跟守护进程说话。这解释了为什么 `start()` 返回的是**环境变量字典**而不是连接对象——worker 进程要靠 `BU_NAME` 找到同一个守护进程，靠 `BU_CDP_WS` 之类的变量决定行为。

四种后端的判定集中在 `_kind()` 里，它把一堆 0.x 风格的开关收敛成一个字符串：

```python
if kind is not None and kind not in KINDS:
    raise ValueError(...)
if sum(map(bool, (kind, local, cdp_url, use_cloud))) > 1 or (use_cloud is False and kind == 'cloud'):
    raise ValueError("Pass one of Browser('chromium' | 'chrome' | 'cloud'), local=True, cdp_url=... or use_cloud=True")
if cdp_url:
    return 'cdp'
return kind or ('chromium' if local or use_cloud is False else 'cloud')
```

三处值得注意：

- **`cdp_url` 优先于 `kind`**：给了 URL 就是 `cdp`，因为 URL 是更具体的指定。
- **互斥检查用 `sum(map(bool, ...))`**：`local=True` 与 `kind='chromium'` 同时给会报错，因为它们矛盾的风险低但语义重叠。
- **默认是 `cloud`**：`kind` 缺省且没有 `local` / `use_cloud=False` 时走云端。这是一个有商业含义的默认值，但源码在注释与错误文案里都把它写明（`'cloud' (default)`）。

`KINDS = ('cloud', 'chromium', 'chrome')` 不含 `'cdp'`——因为 `'cdp'` 不是用户传的 `kind`，而是 `cdp_url` 的产物。这是一处类型上的诚实：`kind` 参数的类型标注是 `Literal['cloud', 'chromium', 'chrome'] | None`。

## 怎么实现的

**构造期：按后端做参数门禁。** `__init__` 的 `kwargs` 先查 0.x 的 `REMOVED` 表（每个旧选项对应一个 1.0 替代），然后做四组数值/类型校验，再逐个检查「这个选项对这个后端有没有意义」：

```python
launches = self.kind == 'chromium' or (self.kind == 'chrome' and profile is not None)
for option, value, ok in (
    ('headless', headless is not None, launches),
    ('executable_path', executable_path, launches),
    ('user_data_dir', user_data_dir, self.kind == 'chromium'),
    ('profile', profile is not None, self.kind == 'chrome'),
    ('cloud_profile_id', cloud_profile_id, self.kind == 'cloud'),
    ('cloud_proxy_country_code', not isinstance(cloud_proxy_country_code, _Default), self.kind == 'cloud'),
    ('cloud_timeout', cloud_timeout is not None, self.kind == 'cloud'),
    ('live_view', live_view, self.kind == 'cloud'),
    ('captcha_solver', captcha_solver is False, self.kind == 'cloud'),
):
    if value and not ok:
        fix = {...}.get(option, "Browser('chromium', ...) launches a local browser with it")
        raise ValueError(f'{where} does not take {option}=: {fix}')
```

两处设计值得学：

- **`launches` 把 `'chrome'` 分成两种**：`Browser('chrome')` 只是**附着**到用户已开的 Chrome（不启动进程），所以 `headless` 与 `executable_path` 对它无意义；而 `Browser('chrome', profile=...)` 会**启动一个新的 Chrome** 跑那个 profile 的副本，所以是可启动的。
- **`cloud_proxy_country_code` 用 `_Default` 哨兵**：`US = _Default('us')` 是一个 `str` 子类，于是「用户没传」与「用户传了 `'us'`」可以区分开——前者不触发门禁，后者对非云端后端会报错。这是用一个 `str` 子类当哨兵的技巧，让默认值本身携带「这是默认的」这一信息。

报错文案里的 `where` 变量也在做同一件事：`Browser() (a cloud browser)` / `Browser('chrome') (your open Chrome)` / `Browser('chromium')`——同一条错误在不同后端下说不同的话。

`user_data_dir` 还有一条单独的硬拒绝：若它是浏览器的**默认** profile 目录，就报错，原因是「Chrome 136+ refuses remote debugging on it」。这就是 e02 里 `is_default_user_data_dir` 的用途。

**`start()` 的三层结构。** 公开的 `start()` 只做一件事：拿锁后调 `_start_locked(replace_gone_cloud=True)`。`_start_locked` 才是主体，它的分支按「当前处于什么状态」分：

1. **首次启动**（`_env is None or not self.name`）：调 `_start()` 做真正的启动。
2. **本地 Chrome 死了**（`self._local.proc.poll()` 非空）：先 `_stop()` 再递归重启。注释写「the Chrome this Browser launched died（crash, OOM, kill）: launch a new one」。
3. **其他（已启动过）**：调 `_reattach()` 让守护进程重新连上同一个浏览器。若它抛 `RuntimeError` 且允许替换云浏览器，就 `_stop()` 后重启；否则抛。

第 2、3 条的区别是关键：**本地 Chrome 死了就换一个新的，云浏览器死了则要看情况**。`replace_gone_cloud` 这个参数就是那个「情况」——`start()` 传 `True`（新的一轮运行，可以换），而 `reattach()`（运行中途）不传（中途换浏览器会丢掉所有标签与登录，见下）。

还有个细节：`_start_locked` 在末尾若发现 `self._local is not None and self._downloads_dir`，会重新发一次下载目录设置。注释写「Chrome forgets it when the connection that set it closes」——即这个设置绑定在 CDP 连接上，重连后必须重发。

**`_start()` 是启动的真正实现，顺序几乎每步都有前置理由。**

1. `require_harness_dirs()`——先确认 browser-harness 的目录可用。
2. **`'chrome'` 且无 profile 的分支**：这是最特殊的一条，因为它要附着到用户的 Chrome。三步：
   - `_check_runtime_dir()`，注释「before anything connects to the default daemon's socket in it」。
   - `_check_one_chrome()`——检查只有一个 Chrome 在跑（否则不知道连哪个）。
   - `require_debug_port_unless_attached()` + `admin.ensure_daemon(wait=LOCAL_START_WAIT_S, env={'BH_TAB_MARKER': '0'})`。
   
   注释解释了 `wait` 的必要性：「without a wait, browser-harness waits forever on Chrome's "Allow remote debugging?" prompt, and `run(timeout=)` only starts counting after the browser started」。即用户在 Chrome 弹窗上不点「允许」的话，会永久挂住——而且 `run(timeout=)` 的计时在浏览器启动之后才开始，救不了。

   这条分支的错误处理分两种：`permission-blocked` 开头 → 给出「点 Allow（macOS 可跑 `browser-use mac-approve`）」的具体指引；其他 → 建议改用 `Browser('chromium')`，「a fresh browser that needs no setup」。

   注意它返回的是 `{'BU_SHARED_DAEMON': '1'}` 而不是完整的 env——因为它用的是**共享**的默认守护进程，那个进程可能是 `browser-use` CLI、MCP 服务端或更早版本启动的，它自己的 `BH_TAB_MARKER` 会保留（worker 会负责剥掉马形前缀）。
3. **云端分支**：`self._key(auto_login=True)` 先确保有 key（可能触发登录），然后停掉「上一个还在跑或正在创建」的浏览器。不成功就报错要求重试 `browser.stop()`。
   
   注释里有两处与线程相关的交代：`_key` 要在**循环线程**上调用而不是工作线程，因为只有主线程收 Ctrl-C（登录要能被中断）；而它只在「本次运行是该循环的唯一任务」时才真登录（否则服务端或 `gather` 的其他任务会被卡住）。
4. `_install_cleanup()`、`_name_daemon_files()`、`self.name = f'bu{uuid.uuid4().hex[:8]}'`。
5. **`_check_socket_path(self.name)` 在创建云端浏览器之前**：注释「before a cloud browser is created and billed」。这是本页反复出现的一条纪律——**所有可能失败的本地前置检查都放在计费动作之前**。失败时要把 `self.name` 复位成 `None`，否则留下一个无主的名字。
6. `_LIVE.add(self)`——进程级的待停清单。
7. **按后端拿 websocket**，三路：
   - `cdp_url`：`_ws_url(cdp_url)` 把 http 端点转成 ws（内部会请求 `/json/version` 拿 `webSocketDebuggerUrl`）。失败时用 `_unreachable()` 包成可读错误。
   - 云端：`_start_cloud()`。
   - 其他（`'chromium'` 或带 profile 的 `'chrome'`）：`_local.LocalBrowser().start(...)`。
8. **组守护进程环境**：`{'BU_CDP_WS': ws, 'BH_TAB_MARKER': '0', **({'BU_BROWSER_ID': id} if id else {})}`。注释解释了 `BU_BROWSER_ID` 的作用：「the daemon knows the cloud browser is its own, so it attaches to its visible tab（a background tab is hidden, and a cloud browser never answers a hidden tab's screenshot）and stops it when it shuts down」。即两件事：附着到可见标签（否则截图会挂），以及守护进程自己退出时顺手停掉云浏览器。
9. `_ensure_daemon()`，然后 `ipc.identify` 拿守护进程 pid。
10. 按后端做收尾：本地 → `self._local.watch(...)`（起看护进程）+ `_close_launch_tab()`（关掉 Chrome 启动时那个 `about:blank`）；`cdp_url` / 云端 → `_attach_open_page()` 与 `start_watchdog(...)`。
11. 若有域名规则，`Guard(ws, self.domains).start()`。若云端且关了验证码求解，发 `BrowserUse.setCaptchaSolverEnabled(enabled=False)`。
12. 整段包在 `except BaseException: await self._stop(); raise` 里——**任何失败都要回收**，包括取消。这一条对云端尤其重要（否则一个已创建的浏览器会一直计费到超时）。
13. 最后组 worker 的 env：`{'BU_NAME': name, 'BU_OWN_BROWSER': '1', **({'BU_REMOTE_BROWSER': '1'} if cloud)}`，云端且关了求解器再加 `BU_CAPTCHA_SOLVER='off'`（注释说这个标记让 `solve_captcha()` 不能把它重新打开）。

`_close_launch_tab()` 有一段「等它真的关掉」的逻辑，注释给了实测：「Linux Chrome still lists a tab for a moment after `closeTarget` answered: `list_tabs()` right after `start()` gave 2.」所以它关掉之后还要轮询最多 2 秒，确认那个 targetId 从列表里消失了。

**`_ensure_daemon()` 修正一个误导性的错误。** browser-harness 会把任何 `BU_CDP_WS` 失败归咎于「远程浏览器」并建议 `start_remote_daemon()`；这对本地启动的 Chrome 是误导。所以这里先看本地进程是否已退出——若是，报「the local browser exited（code N）: it crashed, was killed or closed」；若 `cdp_url` 的失败文本含 `'CDP WS handshake failed'`，就用 `_unreachable()` 生成端点不可达的错误；其余原样抛。

**`reattach()` 与 `_reattach()` 是一对，语义不同。**

- `reattach()`（公开，async）：给 REPL 用。它在锁内检查 `self._env is None`——若已被停掉就抛错，注释解释了为什么不能重启：「`start()` would create a new browser, billed, with none of the tabs or logins the agent was using」。
- `_reattach()`（内部，同步）：守护进程挂了或丢了浏览器连接时，在**同一个**浏览器上重启守护进程。它先 `admin.daemon_alive(name)`，活着就 `daemon_healthy(name)` 判定；不健康就 `_kill_daemon(name)`。

后者的 docstring 有一条很细的判断：「A daemon that answers but whose browser connection is stuck is killed, never shut down: a cloud daemon's shutdown（and browser-harness's own stale-daemon handling）stops the cloud browser this reattaches to.」即「杀掉」与「优雅关闭」在云端是有区别的——优雅关闭会连带停掉云浏览器，那样 reattach 就没有对象可附着了。

**`can_rotate_proxy()` 的层次也值得一提**（详见 e03）：它先判 `use_cloud`（真的云端 → 可以），再判 `is_cloud_cdp`（URL 指向 browser-use.com），最后看 `CDP_ROTATE_SHIPPED` 或有没有 key。`is_cloud_cdp` 与 `cloud_browser_id` 是两个纯函数：前者只判 host 是否 `browser-use.com` 或其子域；后者从 `https://<id>.cdp.browser-use.com` 里取第一段作为 id，且要求 host 恰好四段。

## 关键取舍

**源码事实：** 四种后端由 `_kind()` 从 `kind` / `local` / `use_cloud` / `cdp_url` 收敛而来，`cdp_url` 优先，默认是 `cloud`。`'cdp'` 是派生值而不在用户可传的 `KINDS` 里。

**源码事实：** 构造期逐项检查「这个选项对这个后端有没有意义」，`launches` 把 `Browser('chrome')`（附着）与 `Browser('chrome', profile=...)`（启动）区分开。

**源码事实：** `cloud_proxy_country_code` 用 `str` 子类哨兵 `_Default`，让「没传」与「传了默认值」可区分。

**源码事实：** 所有可能失败的本地前置检查（socket 路径、唯一的 Chrome、调试端口）都排在「创建云端浏览器并开始计费」之前。

**源码事实：** `start()` 会替换「运行之间死掉的」云浏览器（`replace_gone_cloud=True`），但 `reattach()` 不会——它宁可抛错也不创建一个丢掉全部标签与登录的新浏览器。

**源码事实：** `_reattach()` 对「守护进程活着但连接卡住」的处理是 kill 而不是 shutdown，因为优雅关闭会连带停掉云浏览器。

**源码事实：** `_start()` 整段包在 `except BaseException` 里回收，取消也算失败。

**架构推论：** 「用环境变量而不是连接对象交接」是这个模块的结构核心。worker 是独立进程，无法继承 Python 对象，所以守护进程的名字、CDP 端点、模式开关必须全部走环境。这带来一个副作用：**任何新增的协作行为都要同时改两侧**（`Browser` 组 env、worker 读 env），本页与 c01/c02 的交叉引用大多源于此。

**架构推论：** 「后端选择」被做成一个纯字符串而不是类层次，避免了四种 `Browser` 子类与交叉组合（`local` × `cloud` × `profile`）。代价是所有分支集中在 `_start_locked` 与 `_start` 里，读起来需要一直记住当前的 `self.kind` 是哪一个。

**架构推论：** 「附着型后端」与「启动型后端」的差别远大于「本地」与「远程」的差别。`Browser('chrome')` 借用用户已有的进程，能做的事受限（不能改 headless、不能指定二进制），且要处理「用户没开调试端口」「有多个 Chrome」这类不自控的情况；而 `Browser('chromium')` 与云端都是自己创建的，可以完全支配。`launches` 这个变量的存在就是这条差别的编码。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/browser/browser.py` | `Browser` | 四后端门面：构造期门禁、启动、重连、停止、下载与代理入口 |
| `browser_use/browser/browser.py` | `_kind` | 从 `kind` / `local` / `use_cloud` / `cdp_url` 收敛出后端字符串 |
| `browser_use/browser/browser.py` | `start` / `_start_locked` | 幂等启动；区分首次启动、本地 Chrome 死亡、重连三条路 |
| `browser_use/browser/browser.py` | `_start` | 真正的启动：前置检查、按后端拿 websocket、组守护进程、看护、域名守卫 |
| `browser_use/browser/browser.py` | `_ensure_daemon` | 把 browser-harness 的误导性错误改写成「本地浏览器已退出」等 |
| `browser_use/browser/browser.py` | `reattach` / `_reattach` | 公开的重连（拒绝替换）与内部的守护进程重启（kill 而非 shutdown） |
| `browser_use/browser/browser.py` | `is_cloud_cdp` / `cloud_browser_id` | 从 host 判定云端 CDP 并取出浏览器 id |
| `browser_use/browser/browser.py` | `_close_launch_tab` | 关掉 Chrome 启动自带的 `about:blank`，含「列表延迟」的轮询 |
| `browser_use/browser/browser.py` | `_Default` / `US` | `str` 子类哨兵，区分「没传」与「传了默认值」 |
| `browser_use/browser/browser.py` | `REMOVED` / `REMOVED_METHODS` | 0.x 选项与方法到 1.0 做法的映射表 |

## 参考

- [browser-use 源码：browser/browser.py](https://github.com/browser-use/browser-use/blob/main/browser_use/browser/browser.py)
- [browser-use 文档：浏览器配置](https://docs.browser-use.com/customize/browser/overview)
- [browser-harness 仓库](https://github.com/browser-use/browser-harness)
- [Chrome DevTools Protocol：/json/version 端点](https://chromedevtools.github.io/devtools-protocol/#how-do-i-access-the-browser-target)

## 与主核 / AI 的关系

「用一个门面把多种后端收敛成同一套接口」与主核里的**存储后端抽象**完全同构：主核的存储层可以把堆表放在本地文件、也可以放在对象存储或远端节点上，上层查询计划不关心具体是哪种；差别在于本页的实现把选择结果压缩成一个字符串并保留在一个对象里，而不是分成多个实现类。

「附着型 vs 自建型」这条差别在主核里也有对应：主核可以自己管理数据文件，也可以「附着」到用户提供的既有文件（如 `COPY ... FROM` 的外部文件、`file_fdw` 的外部表）。后者的共同约束是**不能假定可完全支配**：文件可能随时被外部进程改写，就像用户的 Chrome 可能随时被关掉。`Browser('chrome')` 的那套前置检查（唯一实例、调试端口、等待授权）本质上就是在处理「我不能完全控制这个资源」的情况。

「失败要回收、取消也算失败」这条纪律与主核的事务语义一致：任何中途失败都必须把已获取的资源释放，否则会留下锁、临时文件或（这里的）**一个仍在计费的云实例**。本页里所有「前置检查放在计费之前」的注释，与主核里「在加锁之前完成所有可能失败的校验」是同一条原则。

对 AI 工程的实际意义在于：浏览器后端的复杂度来自**环境差异**而不是协议。同一个 CDP，接入方式有四种，每种都有各自的失败模式（云端超时计费、本地崩溃、用户 Chrome 未开调试端口、外部端点不可达）。这份实现给出的可借鉴做法是：把选择收敛成一个字符串、把「这个选项对这个后端有意义吗」在构造期逐项问清、把每一种失败都改写成一句带下一步动作的话（点 Allow、改用 chromium、重试 `stop()`）。
