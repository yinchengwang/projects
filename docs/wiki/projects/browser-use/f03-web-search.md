---
project: browser-use
feature: web-search
title: 网页搜索接入
page_type: feature
schema_version: 1
refs:
  - path: browser_use/search.py
    symbols: [WebSearch, SearchError]
  - path: browser_use/agent/service.py
    symbols: [_web_search]
---

# 网页搜索接入

## 一句话职责

给 REPL 里的 `search()` 提供一个不经浏览器的搜索通道：worker 通过控制通道把查询交给父进程，父进程用 Browser Use 网关的 API key 发一个 `POST /api/v4/search`，再把结果整理成每个命中一条文本。**密钥永不进入 worker 进程。**

## 机制是什么

`WebSearch` 是一个可调用对象，一次调用一次搜索：

| 成员 | 作用 |
|---|---|
| `__call__(query, num_results=None, category=None)` | 发请求、解析响应、把命中规范化成列表，返回原始响应加上替换后的 `results` |
| `charges: list[float]` | 每次响应里若带 `browser_use.charge_usd` 就记一笔，供运行结果累加成本 |
| `preflight()` | 浏览器启动**之前**的一次免费探测，只判断「端点存在且密钥可用」 |
| `search_url(base_url)` | 从 API base URL 推出搜索端点（取 scheme + netloc + 固定路径） |
| `parse_usd(value)` | 把「十进制字符串或数字」的计费解析成 USD，非法则 `None` |

`SearchError` 是唯一的失败类型，所有失败都被翻译成**给模型看的可行动文案**。

## 怎么实现的

**密钥不进 worker 是这个模块存在的首要原因。** 模块 docstring 第一段就说清了链路：**REPL worker 的 `search()` 通过控制通道问父进程（`repl/session.py`），父进程在这里发请求——API key 从不进入 worker**。`Repl.__init__` 的 `search` 参数注释也重申了这一点：`search: defines search() in the worker, answered here (the API key stays in this process)`。

这条设计与 `c03-secret-redaction` 里「环境白名单」的思路一致但更强：那一条是「尽量不让密钥出现在 worker 环境里」，这一条是「把需要密钥的操作整个搬到父进程，worker 只拿到结果」。

**超时被刻意压在一个更大的截止时间之下。** `SEARCH_TIMEOUT_S = 25.0`，注释给出理由：**它在 120 秒的单元格截止时间之下，于是一个卡住的端点会让这次调用失败，而不是整个单元格失败**。这是一个正确的层次判断——搜索是辅助动作，不该拖垮让它发生的那一步。

**命中分隔符的正则带一个前瞻，因为 `---` 会出现在正文里。** `HIT_SEPARATOR` 是 `r'\n\n---\n\n(?=Title: )'`。注释说明：**端点把命中渲染成一段文本，而 `---` 也会出现在高亮片段里**。加分隔后的 `(?=Title: )` 前瞻，才让它只在真正的命中边界处切分。

**响应有两种形态，都要支持。** `results` 是列表就直接用；是**非空字符串**就按分隔符切；其余（含空串）当空列表。这解释了为什么需要一个字符串分支：端点的渲染形态可能是「一段文本」，而模型需要的是「每个命中一条」，因为一段长文本会被它当成一整段内容读。

**HTTP 错误被翻译成带状态码的可读消息，402 单列。** `_clean(e.read()...[:300])` 取响应体前 300 字符，且对 402 额外拼上 `(run out of budget)`。这直接对应 `SEARCH_PROMPT` 里告诉模型的那句「402 表示账户预算用尽」——**错误文案与提示词是配套的**，模型看到 402 时已经有先验知识知道该怎么做。

**错误分类的粒度值得记。** 四类：`HTTPError`（HTTP 状态错误）、`TimeoutError` / `URLError`（并进一步区分「是不是超时」）、`OSError`（注释：读响应中途连接被重置）、以及 JSON 解析失败。每类都产出不同的文案，且都经 `_clean` 过一遍。

**`_clean` 只做一件事：把 API key 从文本里删掉。** `text.replace(self.api_key, '<api key>')`。这是必要的，因为错误体、异常消息、甚至非 JSON 的响应原文都可能回显请求头或 URL。父子进程中任何一段这类文本都会进模型上下文。

**`preflight()` 是一次「免费的、能区分错误类型」的探测。** 它发 `POST {}`（空对象），docstring 三句话讲清了设计：

1. **免费**：没有 query，不会真的搜索，不计费。
2. **哪些状态码算错误**：404（端点不存在）、401/403（密钥被拒）、402（无预算）→ 返回错误行；其余（422 表示「端点在且密钥可用」；429/5xx/网络不可达属于可能瞬时）→ 返回 `None`，让每次 `search()` 自己上报，运行继续但无搜索。
3. 实现细节：错误体读取失败时**退回 `e.reason`**（注释：body 断了那就只有状态码是答案）；`(OSError, http.client.HTTPException)` 都被吞掉，因为 **`BadStatusLine`、`IncompleteRead` 不是 `OSError`**。

这套分类的价值在于**区分「配置错」与「暂时不可用」**：前者应当在浏览器启动前就失败（省掉一次可能计费的浏览器启动），后者不该阻止运行——搜索只是辅助能力。

**`preflight` 在 `run` 之前、与模型预热并行调度。** `service.py` 里那句 `check = self._gateway.preflight if self._gateway else (lambda: provider_preflight(model))`，然后 `error = await asyncio.to_thread(lambda: check() or (search and search.preflight()))`。注意它被放在 **`asyncio.to_thread`** 里执行——网络探测不能阻塞事件循环；而 `or` 的短路顺序保证模型检查先跑，只有模型检查通过才探测搜索。注释写明它的位置意义：**在工作区创建之前**，于是「密钥被拒」这类配置错会在一个浏览器被计费之前暴露。

**计费是「响应自报，累加，缺失则不计」。** `charge_usd` 从 `browser_use` 元数据里取，经 `parse_usd` 校验（必须是 `str|int|float`、不是 `bool`、能转 float、有限、非负），通过就追加到 `self.charges`。`charges` 的类 docstring 明确：**只有成功且响应里报了价的搜索才记；没报的什么都不加**——也就是说这个数字是**下界**，不是账单。

**`parse_usd` 的一个细节：显式排除 `bool`。** 因为 `isinstance(True, int)` 为真，不排除的话一个 `True` 会被当成 1 美元。返回 `None` 也是一个统一的「不可用」表示，而不是 0——这与主核里「未知成本与零成本必须区分」的原则相同。

**`charges` 的写入有锁，理由是取消。** `self._lock` 旁边注释：**一次被取消的调用的线程可能在下一次调用运行时才结束**。也就是说父进程侧的搜索调用可能跑在单独线程里，取消不是同步的，所以追加必须加锁。

**`search_url` 只保留 scheme 与 netloc。** 从 `https://api.browser-use.com/api/v4/llm/v1` 推出 `https://api.browser-use.com/api/v4/search`——它不管 base URL 的路径，直接换成本模块的固定路径 `SEARCH_PATH`。这样「网关地址可配」与「搜索端点路径固定」两件事可以独立变化。

**`_web_search` 里有一条明确的配置互斥检查。** `service.py` 的实现：`web_search` 为假直接返回 `None`；否则**如果设了 `allowed_domains` 就抛 `ValueError`**，理由是 `Agent(web_search=True) cannot be combined with allowed_domains: search would reach sites the browser may not open`。

这条互斥是本页最值得记住的设计判断：域名白名单表达的是「这个 agent 只该接触这些站点」，而搜索会**绕过浏览器**直接访问任意站点并把摘要带回来。两者语义冲突，所以框架选择报错而不是让用户以为白名单还在生效。

**密钥来源有两级回退。** `_web_search` 取 `gateway.api_key`（模型跑在 Browser Use 网关上时），否则用 `browser_use_api_key('Agent(web_search=True)', 'web_search', auto_login=True)`。`auto_login=True` 意味着允许走登录流程获取密钥。搜索端点的主机则取网关的 `base_url`，无网关时用默认 `GATEWAY_URL`——也就是说**搜索与模型调用共用同一套凭据与主机**。

**系统提示是条件拼进去的，且注释强调「关掉时默认提示逐字节不变」。** `SEARCH_PROMPT if self._search else ''`，旁边注释：`off: the default prompt stays byte-identical`。同一个注释也出现在域名规则那一行（`'' without restrictions: the default prompt stays byte-identical`）。这条纪律的意义在于**提示词缓存**：开启搜索会改变系统提示（缓存失效一次），但不开启时不能因为拼接方式而改变任何字节。

**`SEARCH_PROMPT` 的内容本身是一份使用说明，值得读完。** 它告诉模型：浏览器里的搜索引擎要走很多步并触发反爬（所以优先用 `search()`）；用自然语言描述你要的页面而不是关键词；`results` 每个命中一条文本（标题、URL、匹配片段）且**页面内容很少，要看内容得在浏览器里打开**；`num_results` 默认 8、最多 20；`category` 可选值；失败会带 HTTP 状态；**每次搜索都计费，所以把工作批成更少更好的查询，而不是每项一次搜索**。

最后一句是产品层面的关键：它把「成本」直接写进提示词，让模型参与成本控制。这是 agent 系统里控制 API 花费的一个有效手段。

## 关键取舍

**源码事实：** 搜索请求由父进程发起，worker 只通过控制通道请求结果。密钥不进 worker 进程。

**源码事实：** 搜索超时 25 秒，被刻意压在 120 秒的单元格截止时间之下——让辅助动作失败而不是让整个单元格失败。

**源码事实：** 命中分隔正则带 `(?=Title: )` 前瞻，因为 `---` 会出现在高亮片段里。

**源码事实：** `web_search=True` 与 `allowed_domains` 互斥并抛 `ValueError`，因为搜索会绕过浏览器访问白名单之外的站点。

**源码事实：** `preflight()` 用空 body 做免费探测，且只把 404/401/402/403 当硬错误；422 视为「端点在且密钥可用」，429/5xx/网络问题留给每次调用上报。它在浏览器启动前、工作区创建前执行。

**源码事实：** `parse_usd` 排除 `bool`（否则 `True` 会被算成 1 美元），并把「未知」表示为 `None` 而不是 0。

**源码事实：** `charges` 只记响应自报的金额，因此是下界而非法案。

**源码事实：** 所有错误文案都过 `_clean` 删掉 API key；402 额外标注「预算用尽」，与提示词里的说明配套。

**源码事实：** 系统提示按开关条件拼接，且关闭时保证默认提示逐字节不变（提示词缓存友好）。

**架构推论：** 搜索在这里被定位成一个**有成本、有边界、可失败但不致命**的外部能力。三个设计共同支撑这个定位：把密钥留在父进程（安全边界）、把预检放在浏览器启动前（成本边界）、把超时压在单元格截止之下（可用性边界）。而提示词里那句「每次搜索都计费，请批量查询」是第四层——**把成本模型也交给模型**，因为在这个架构里模型才是决定「搜几次」的那一方。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/search.py` | `WebSearch` | 一次调用一次搜索，含计费累加与线程锁 |
| `browser_use/search.py` | `WebSearch.preflight` | 浏览器启动前的免费探测，区分硬错误与瞬时问题 |
| `browser_use/search.py` | `WebSearch.__call__` | 发请求、四类错误翻译、结果规范化 |
| `browser_use/search.py` | `SearchError` | 唯一失败类型，文案面向模型 |
| `browser_use/search.py` | `search_url` | 从 API base URL 推出固定搜索端点 |
| `browser_use/search.py` | `parse_usd` | 计费解析，排除 `bool`，未知为 `None` |
| `browser_use/search.py` | `HIT_SEPARATOR` | 带 `(?=Title: )` 前瞻的命中分隔 |
| `browser_use/search.py` | `SEARCH_TIMEOUT_S` | 25 秒，压在单元格截止之下 |
| `browser_use/agent/service.py` | `_web_search` | 密钥来源回退与 `allowed_domains` 互斥检查 |
| `browser_use/agent/service.py` | 预检调用点 | `asyncio.to_thread` 中先跑模型检查再跑搜索检查 |
| `browser_use/agent/prompt.py` | `SEARCH_PROMPT` | 使用说明 + 成本提示，条件拼入系统提示 |
| `browser_use/repl/session.py` | `Repl.__init__` 的 `search` 参数 | worker 定义 `search()`、父进程作答的接线点 |

## 参考

- [browser-use 源码：search.py](https://github.com/browser-use/browser-use/blob/main/browser_use/search.py)
- [browser-use 源码：agent/service.py](https://github.com/browser-use/browser-use/blob/main/browser_use/agent/service.py)
- [browser-use 文档：网页搜索](https://docs.browser-use.com/customize/tools)
- [browser-use 定价说明](https://browser-use.com/pricing)

## 与主核 / AI 的关系

这条链路与主核里「扩展过程（如外部函数、远程表）的凭据处理」同构：一个可能不受信任的执行环境需要一个需要凭据的能力，正确做法是把它变成一个**受控的远程调用**，而不是把凭据下发到执行环境。主核里对应的是「外部表由后台进程用服务器凭据读取」，而不是「把文件系统凭据交给 SQL 执行器」。

对 agent 工程的直接价值有三条。其一，**预检要能区分配置错与暂时不可用**。把所有非 2xx 都当致命，会让一次 429 阻止整次运行；把所有都当可忽略，会让一个写错的密钥拖到运行中途才暴露。按状态码分类是唯一能兼顾两者的做法。其二，**带费用提示的能力要把费用写进提示词**。模型是决定调用次数的执行方；如果它不知道每次都要钱，它就会按「每项搜一次」的方式工作。其三，**安全边界与能力边界冲突时要报错**。`web_search` 加 `allowed_domains` 在实现上可以「让搜索照常跑」，但那会让白名单变成一句空话；报错虽然不够友好，但保住了语义。
