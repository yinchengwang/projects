---
project: browser-use
feature: downloads-and-proxy
title: 下载与代理轮换
page_type: feature
schema_version: 1
refs:
  - path: browser_use/browser/browser.py
    symbols: [list_downloads, download_files, download_records]
  - path: browser_use/browser/browser.py
    symbols: [rotate_proxy, can_rotate_proxy]
---

# 下载与代理轮换

## 一句话职责

两件「浏览器之外但属于会话」的能力：把云端浏览器下载的文件搬回本地工作区（并识别出哪些「下载」其实只是把页面另存了），以及在不丢标签与登录的前提下把云端浏览器的出口 IP 换一个。

## 机制是什么

这两件事看起来不相关，但共享同一个前提：**它们都只对云端浏览器完整可用**。

| 能力 | 云端浏览器 | 本启 Chrome | 外部 `cdp_url` |
|---|---|---|---|
| `list_downloads` | 走 API 列表 | 读本地目录 | 读本地目录（若已设置） |
| `download_files` | 走 API + HTTP 下载 | 无需搬运（文件已在本机） | 同本启 Chrome |
| `rotate_proxy` | 完整可用 | 不可用 | 仅当 URL 指向 browser-use.com 且（CDP 路径已发布 或 有 key） |

这个差异决定了代码里大量的 `if not self.use_cloud:` 分支——它们不是可选优化，而是「这个能力在这里不存在」的显式表达。

另一个共享前提是：**两者都需要在计费与超时之间做取舍**。云端浏览器按时间计费，所以下载不能无限等（`DOWNLOADS_TIMEOUT_S = 300`）；而 `rotate_proxy` 要等云端真的换上了新出口 IP，也不能无限等（`PROXY_WAIT_S = 10.0`）。

## 怎么实现的

### 下载：三条记录通道

先把「下载」这件事的三份记录分清，否则代码读不通：

| 记录 | 形态 | 谁写 |
|---|---|---|
| `downloads.jsonl` | 每行一条 JSON 记录 | REPL worker（单元格里发生的下载） |
| `_download_records` | 上面所有行的累积（按 run 顺序） | `note_downloads` |
| `json_page_copies` | 一组文件**名** | `json_page_copies()` 算出，`note_downloads` 更新 |

`download_records(lines)` 是最朴素的解析：`json.loads` 每行，`ValueError` 静默跳过，只收 dict。docstring 一句话「unreadable lines are skipped」——因为这是跨进程、可能被中途杀掉写了一半的文件，宽容是必须的。

`note_downloads(log)` 是增量读取的：

```python
self._download_records += download_records(lines[seen:])
self.json_page_copies |= json_page_copies(self._download_records)
```

它用 `self._download_lines[path]` 记住已读行数，只解析新增部分。注释解释了为什么必须累积全部记录：「the cloud numbers a repeated name（'cookies (1).json'）across the whole session, not per workspace」——即云端在整个会话里对重名文件统一编号，所以判断「某个下载是不是页面副本」必须看全会话，不能只看当前工作区。

**`json_page_copies()` 是这一页最有意思的一段。** 它解决的问题很具体：云端浏览器会把「顶层是 JSON 的页面」也当成下载存下来。docstring 描述得很细：

> a cloud browser saves `https://host/data?q=1`, shown as a page, as the download `https://host/data.json`（application/json）or `https://host/data`（+json types）, the query dropped, with any cookies the page echoed. Real downloads keep their own URL.

于是判断规则是：若某条记录的 `url` **不含 `?`** 且（去掉 `.json` 后缀、去掉尾部 `%20(N)` 编号后）落在「本会话显示过的 JSON 页面集合」里，它就只是一个副本，应当从下载列表里排除。

三处细节值得记：

- **重名编号的模拟**：`for i in range(1, 10_000)` 循环，若 `listed` 已被 `taken` 就试 `stem (i) suffix`。这是在**复现云端的编号规则**，因为要判断的是「云端会把这个副本列成什么名字」。注释说明了为什么必须精确：「A name used twice is listed as 'name (1).ext' for the later download, so the real file that shares a copy's name is kept, not dropped with it」。即若不做编号模拟，一个真实下载可能与某个副本同名，被一起误删。
- **URL 上的编号也要剥**：`base = re.sub(r'%20\(\d+\)$', '', url.removesuffix('.json'))`，注释「the cloud numbers a repeated copy's URL too」。
- **`%20` 而不是空格**：说明云端在 URL 里用的是百分号编码的括号编号。

所以这套逻辑的核心是**双向模拟云端的命名规则**——既要在文件名上模拟，也要在 URL 上模拟，否则同名判定会错。这类「复现对方的命名规则」在做跨系统对账时很常见。

### 下载：读列表与搬文件

`list_downloads()` 分两支：

- **非云端**：读 `self._downloads_dir`（即 `workspace/downloads`），过滤掉 `*.crdownload`（Chrome 的未完成标记），返回 `{'name', 'size'}`。
- **云端**：分页调 `/browsers/{id}/downloads?limit=100`，跳过 `json_page_copies` 里的名字，`nextCursor`/`hasMore` 控制翻页。docstring 提了一句时序：「about a second after each completes」——即 API 有约一秒延迟，刚下完的文件可能还没出现在列表里。

`download_files()` 是搬运主体，它的注释列了五条契约：

> `skip`: remote paths to leave out; each copied path is added to it. A file that fails to copy is logged and left out of `skip`, so a later call retries it. A name already in `dest` gets a ' (1)' suffix, never overwritten. Names in `json_page_copies` are left out. `deadline`: `time.monotonic()` value at which transfers stop.

五条各有后果：

1. **`skip` 是「已处理」的账本**，且**失败项不进 `skip`**——这样下一次调用会重试。这个设计让 `stop()` 可以被多次触发而不重复搬运。
2. **遇到 `json_page_copies` 里的也要 `skip.add`**，注释「listed, so stop() stops waiting for it」。即不搬但也不等——否则 `stop()` 会一直等一个永远不会被搬的文件。
3. **不覆盖**：靠 `_download` 里的 `open(path, 'xb')` 实现，`FileExistsError` 就试下一个候选名。
4. **名字兜底**：`name if name not in ('', '.', '..') else 'download'`——防路径异常。

`_download(url, dest, name, deadline)` 是单个文件的搬运，它的 docstring 强调「Stops at the monotonic deadline, so the thread ends then: `run_sync`'s loop and interpreter exit never wait on a slow transfer」。实现里三处检查 deadline：

- 入口：`if (left := deadline - time.monotonic()) <= 0: raise TimeoutError`。
- 循环内每写一块：`r.read1(65536)` 后判——注释「read1: returns what arrived, so the deadline is checked as bytes drip in」。用 `read1` 而不是 `read` 是为了**不被一次性阻塞住**，这样 deadline 才有意义。
- EOF 后再判一次：注释「EOF came after it: the caller already gave up and never records the file」。

还有一条完整性校验：

```python
expected = (getattr(r, 'headers', None) or {}).get('Content-Length')
if expected is not None and expected.isdigit() and size != int(expected):  # EOF before the end: truncated
    raise ConnectionError(...)
```

失败时 `path.unlink(missing_ok=True)`——**不留半个文件**。

**`_fetch_downloads()` 是搬运的调度层**，它把 `download_files` 包在 `asyncio.timeout` 里，且异常只 warning：

```python
except Exception as e:  # - the result is already delivered
    logger.warning('Could not fetch cloud browser downloads: ...')
```

这句注释是整段的关键：**下载搬不回来不影响结果交付**。所以这里容忍任何异常。

### 下载：本地浏览器的目录设置

`_send_downloads(downloads)` 只在本地启动的 Chrome 上用（云端走 API）。它用 `behavior='allowAndName'` + `downloadPath` + `eventsEnabled`：

注释解释了 `allowAndName` 的选择：「Saved as `<guid>` and renamed once finished（`utils.name_download`）: with 'allow' a repeated name overwrote the earlier file」。即先存成 GUID，完成后再改名——这样同名下载不会互相覆盖。

还有一段关于 CDP session 的细节很值得记：

```python
# On a browser session: the daemon sends other calls on its tab's session, and Chrome then reports
# Browser.download* only for that tab, so a download in the agent's second new_tab() stayed 'inProgress'
# and stop() waited DOWNLOADS_TIMEOUT_S. One session per daemon: each one would repeat every event.
with contextlib.suppress(RuntimeError):
    self._downloads_session = cdp('Target.attachToBrowserTarget')['sessionId']
    cdp('Browser.setDownloadBehavior', self._downloads_session, **params)
```

三件事：必须用**浏览器级 session**（否则事件只覆盖一个标签页）；每个守护进程只能有一个这样的 session（否则事件重复）；且失败只 warning（注释「downloads then go where Chrome puts them」——退回 Chrome 默认位置）。

### 代理轮换：判定能不能

`can_rotate_proxy()` 是一段三档判断，docstring 把每档的条件都写了：

```python
if self.use_cloud:
    return True
if not is_cloud_cdp(self.cdp_url):
    return False
return CDP_ROTATE_SHIPPED or (self.has_rotate_key() and bool(self.cloud_id))
```

读法：

- 自己创建的云浏览器 → 恒可。
- `cdp_url` 不指向 browser-use.com → 恒不可。
- `cdp_url` 指向云端 → 若 CDP 路径已发布（`CDP_ROTATE_SHIPPED = True`）则可（**不需要 key**）；否则需要有 key 且有浏览器 id。

docstring 还补了一句重要的时序说明：「`Browser()`（cloud）qualifies before it starts: the Agent builds its prompt before the browser has an id.」即这个判定必须能在**启动前**回答「能不能」，因为系统提示是在浏览器创建之前拼的。

`has_rotate_key()` 与 `_own_key()` 两级：

```python
def has_rotate_key(self) -> bool:
    if self.use_cloud: return True
    return self._own_key() or bool(self._llm_key)

def _own_key(self) -> bool:
    from browser_use.auth import _stored_api_key
    if (os.environ.get('BROWSER_USE_API_KEY') or '').strip(): return True
    try: return bool(_stored_api_key('Browser Use Cloud', 'cloud_browser'))
    except Exception: return False
```

`_own_key` 的 docstring 有一句限定：「looked up without recording a missing-key auth event（a `cdp_url` cloud browser rotates over CDP without any key）」。即这个查询是**探测性**的，不能因为「查了一下没有」就产生一条「缺 key」的遥测事件——因为多数情况下根本不需要 key。这是一个容易忽略的副作用边界。

### 代理轮换：执行

`rotate_proxy(country)` 的流程是 GET → PATCH → 轮询：

1. **先取浏览器 id**：`self.cloud_id`，失败时报错并说明两条路（`Browser()` 或 `https://<id>.cdp.browser-use.com`，后者走 CDP 不需要 id）。
2. **校验国家码**：`re.fullmatch(r'[A-Za-z]{2}', country.strip())`。
3. **选 key**：
   ```python
   key = self._cloud_key if self.use_cloud else ((self._llm_key,) if self._llm_key and not self._own_key() else ())
   ```
   即自建云浏览器用创建时的 key（注释在 `_cloud_key` 字段那处解释了为什么必须记住它：「a `keep_alive` Browser's next Agent on ChatOpenAI sets `_llm_key` None; a stop without a key never ends billing」）；`cdp_url` 浏览器只有在自己没有 key 时才借用模型的 key。
4. **GET 当前状态**——目的是拿到 `before.exitIp` 用于后续比对。
5. **PATCH `{'rotate': True, 'country': ...}`**，并对 409 做专门翻译：
   > `rotate_proxy()`: this browser's proxy cannot be changed here (409). A browser that a Browser Use Cloud agent session runs rotates its own proxy, and a stopped browser has none.
6. **轮询到真的换了**：
   ```python
   while not after.get('exitIp') or after.get('exitIp') == before.get('exitIp'):
       if time.monotonic() > deadline: raise RuntimeError(...)
       await asyncio.sleep(0.5)
       after = await asyncio.to_thread(_cloud, path, 'GET', None, *key)
   ```
   注意条件是「没有 exitIp **或** 与之前相同」——两者都算没换成功。超时的错误文案还留了余地：「the old one may still be in use」。
7. 返回 `{k: after.get(k) for k in ('mode', 'country', 'exitIp')}`——只给三个字段。

这个「PATCH 之后还要轮询确认」的模式值得注意：**异步操作的成功应答不等于操作完成**。云端的 PATCH 成功只表示「已受理」，实际的出口 IP 变更需要时间。

## 关键取舍

**源码事实：** 云端浏览器的下载与代理能力通过 REST API 实现，本地启动的 Chrome 通过目录与 CDP 实现，`cdp_url` 视指向而定。

**源码事实：** `json_page_copies` 用「URL 无 query 且（剥掉 `.json` 与尾部编号后）落在显示过的 JSON 页面集合里」判定副本，并**双向模拟云端的重名编号规则**（文件名与 URL 上都要模拟）。

**源码事实：** 下载的 `skip` 集只收集成功项，失败项留待下次重试；`json_page_copies` 里的项也进 `skip`，以免 `stop()` 白等。

**源码事实：** `_download` 用 `read1` 逐块读并在每块后检查 deadline，EOF 后再检查一次，且用 `Content-Length` 校验完整性；失败时删除半成品。

**源码事实：** 本地 Chrome 的下载行为用 `allowAndName`（先存 GUID 再改名），且必须用浏览器级 CDP session、每守护进程只建一个。

**源码事实：** `can_rotate_proxy()` 必须在浏览器启动前就能回答，因此 `Browser()` 在还没有 id 时也算「可轮换」。

**源码事实：** `rotate_proxy` 在 PATCH 之后还要轮询确认 `exitIp` 真的变了，超时文案承认「旧的可能仍在用」。

**源码事实：** `_own_key()` 是探测性的，不产生「缺 key」遥测，因为多数 `cdp_url` 场景不需要 key。

**架构推论：** 下载这块的复杂度不在传字节，而在**对账**：云端的列表、本地工作区的文件、REPL worker 记录的事件、以及「哪些其实是页面副本」这四个视角必须能对上。`json_page_copies` 那套双向编号模拟就是典型的对账代码——它必须精确复现对方的命名规则，否则会把真实下载当成副本删掉。

**架构推论：** 「失败项不进 `skip`」这条小规则解决的是**幂等重试**问题。`stop()` 可能被多次调用（正常停止、超时、`atexit`），若不重试则一次网络抖动就永久丢文件；若进 `skip` 则永远不重试。这条规则让停止过程既能重复执行又有收敛性。

**架构推论：** 代理轮换的「PATCH 后轮询」暴露了云端 API 的异步本质。这类接口最容易被误用成「PATCH 返回 200 就等于换好了」，而实际上下一次请求的出口 IP 可能还是旧的。这套实现选择「确认到变化才返回」，代价是慢（最多 10 秒），收益是调用方拿到的 `exitIp` 一定是新的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/browser/browser.py` | `list_downloads` | 云端分页列表 / 本地目录枚举，排除页面副本与未完成文件 |
| `browser_use/browser/browser.py` | `download_files` | 云端文件搬运，含 `skip` 账本与不覆盖策略 |
| `browser_use/browser/browser.py` | `download_records` | `downloads.jsonl` 的宽容解析 |
| `browser_use/browser/browser.py` | `json_page_copies` | 识别「只是页面另存」的下载，双向模拟重名编号 |
| `browser_use/browser/browser.py` | `_download` | 单文件搬运：`read1` 逐块、deadline、`Content-Length` 校验、失败清理 |
| `browser_use/browser/browser.py` | `note_downloads` | 增量读取下载记录并累积全会话视图 |
| `browser_use/browser/browser.py` | `_send_downloads` | 本地 Chrome 的下载目录设置（`allowAndName` + 浏览器级 session） |
| `browser_use/browser/browser.py` | `_fetch_downloads` | 搬运调度：整体超时且失败只 warning |
| `browser_use/browser/browser.py` | `rotate_proxy` | GET → PATCH → 轮询确认出口 IP 变化 |
| `browser_use/browser/browser.py` | `can_rotate_proxy` | 三档判定，且必须能在启动前回答 |
| `browser_use/browser/browser.py` | `is_cloud_cdp` / `cloud_browser_id` | 从 host 判定云端端点并取出浏览器 id |
| `browser_use/browser/browser.py` | `_cloud` | 云端 REST 调用，把 HTTP 错误翻成带 `detail` 的 `RuntimeError` |

## 参考

- [browser-use 源码：browser/browser.py](https://github.com/browser-use/browser-use/blob/main/browser_use/browser/browser.py)
- [browser-use 云文档：更新浏览器代理](https://docs.browser-use.com/cloud/api-v4/browsers/update-browser-proxy)
- [browser-use 文档：下载文件](https://docs.browser-use.com/customize/browser/downloads)
- [Chrome DevTools Protocol：Browser.setDownloadBehavior](https://chromedevtools.github.io/devtools-protocol/tot/Browser/#method-setDownloadBehavior)
- [Python 文档：io.BufferedIOBase.read1](https://docs.python.org/3/library/io.html#io.BufferedIOBase.read1)

## 与主核 / AI 的关系

「把远端产生的文件搬回本地，并保证不重复、不覆盖、不半途而废」与主核里的**基础备份与 WAL 归档**是同一种工程：都要处理「断续重试」「不覆盖已有文件」「不留下半个文件」。主核的归档会用「写到临时名再 rename」实现原子性；这里用 `open(path, 'xb')`（独占创建）+ 失败即 `unlink` 实现同样的效果。

`json_page_copies` 那套对账逻辑在主核里对应的是**外部表与本地表的行数校验**：两边都必须在相同的命名/编号规则下比较，否则会把「同一件事」当成两件。这里的特殊之处是必须**复现对方（云端）的编号规则**——这在数据库里通常不需要，因为编号规则是自己定的；而在调用第三方 API 的场景里，「对方的命名规则」就是一份必须精确实现的规格。

`rotate_proxy` 的「PATCH 后轮询」与主核里「DDL 提交后要等 catalog 版本推进」是同一类：异步操作的成功应答只表示受理，真正的生效需要观察。区别在重试边界的确定：主核可以等一个明确的版本号递增，这里只能比较 `exitIp` 是否变化——一个更弱但可用的信号。

对 AI 工程的实际意义在于：这两个能力都是「agent 完成实际工作」的必要条件——下载是产出文件的方式，代理轮换是绕过反爬与地域限制的方式。这份实现给出的三条经验值得抄：**下载要用独占创建 + 失败清理**（半成品文件会让下游误判）、**搬不回来不影响主结果交付**（下载失败只 warning，因为结果已在对话里）、以及**能力判定要在资源创建前可回答**（`can_rotate_proxy` 在没有 id 时也要能给答案，因为提示词在浏览器启动前就拼好了）。
