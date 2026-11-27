---
project: browser-use
feature: cli-mcp-server
title: CLI MCP 服务端
page_type: feature
schema_version: 1
refs:
  - path: browser_use/mcp/cli_mcp.py
    symbols: [CLIMCPServer, run, _tool_definitions]
  - path: browser_use/mcp/cli_mcp.py
    symbols: [_Cell, _Login, _run_cell]
---

# CLI MCP 服务端

## 一句话职责

把「模型在一段持久 Python 命名空间里驱动浏览器」这件事装成一个 stdio MCP 服务端：`browser_exec` 跑代码、`browser_screenshot` 出图、`browser_login` 走 OAuth、`browser_feedback` 收反馈，而执行模型必须绕开 MCP 客户端的 60 秒调用超时——于是有了「单元格活得比调用久」这套机制。

## 机制是什么

`CLIMCPServer` 是一个有状态的 stdio 服务端，暴露四个工具：

| 工具 | 作用 |
|---|---|
| `browser_exec` | 在本服务器的持久命名空间里执行 Python，返回它打印的内容 |
| `browser_screenshot` | 截当前页并作为图片返回 |
| `browser_login` | 走浏览器 OAuth 登录 Browser Use Cloud，供云浏览器使用 |
| `browser_feedback` | 收反馈（与 CLI 的 `browser-use feedback` 同一条消息） |

支撑它的几个内部类型：`_Cell`（一次 `browser_exec` 的执行单元，在独立线程上跑）、`_CellStream`（per-thread 的 stdout/stderr 替身）、`_Login`（跨工具调用完成的登录流程）、`_Cell.done`（完成事件）。

执行模型的关键词是**「调用与执行解耦」**：一次 MCP 工具调用只是一个观察窗口，代码在它自己的线程上继续跑。

## 怎么实现的

**核心矛盾是「客户端的 60 秒超时 vs 浏览器操作可能很久」。** `CALL_SECONDS = 50.0` 旁边的注释把约束写明了：**Cursor 以及其他用 MCP TypeScript SDK 的客户端会在 60 秒时取消工具调用，于是模型从「确实跑过的代码」那里什么都没拿到**。所以服务端主动在 50 秒回复，留 10 秒余量。

**于是有了 `_Cell` 这套解耦机制。** 类 docstring 定义得很清楚：**一个 `browser_exec` 单元格在它自己的线程上，没有调用能停掉它；活过自己那次调用的单元格继续跑，之后的调用返回它从那时起打印的内容，以及它的结束**。

具体分工是：`_exec_reply` 起线程跑 `_run_cell`，然后在 `asyncio.to_thread` 里等 `cell.done`，最多等到 deadline。若到点还没完成，就追加一条 `STILL_RUNNING` 说明（含已运行秒数），并告知**用空 code 调用一次可以等它、拿到剩余输出**。空 code 的语义就是「等所有还在跑的单元格」，全都结束了则回 `NOTHING_RUNNING`。

**`_Cell.new_output` 用游标而不是清空缓冲。** 它保存 `_shown` 偏移量，每次返回 `text[_shown:]` 并更新偏移。这样多次调用看到的是**增量**而不是重复：第一次返回前 3000 字，第二次只返回之后的。而 `_clipped` 再对每次增量施加 `MAX_OUTPUT_CHARS` 上限，超出部分**落到临时文件**（`browser_exec_*.txt`），并在返回文本里给出路径。

`_clipped` 的两条编码处理值得记：写临时文件时用 `encoding='utf-8', errors='replace', newline=''`，注释解释了两次事故——**Windows 的 locale 编码（cp1252）会因为一个 emoji 让整个调用在单元格已经跑完之后失败**；`newline=''` 是因为 Windows 会把每个 `\n` 写成 `\r\n`，破坏「文件里就是捕获到的那份输出」这个约定。

**per-thread 的 stdout 捕获是为了隔离「跑飞的单元格」。** `_CellStream` 的 docstring：**它代替 `sys.stdout`/`sys.stderr`，单元格的线程打印进自己的缓冲，其它线程打印进原 stream（服务期间是 stderr）**。注释给出了两条理由：一是「单元格启动的线程打印的内容不在它的结果里」（这一点在 `browser_exec` 的工具描述里对模型明说了）；二是「per-thread 捕获是让一个卡住的单元格的打印永不落到下一次调用的结果里」的手段。

`_capture` 用 `contextvars` 的 `_cell.buffer` 做「当前线程属于哪个单元格」的绑定，并在 `finally` 里清空。这是个很干净的实现：不需要真的换 `sys.stdout` 对象，只在首次进入时换上 `_CellStream` 包装，之后靠 contextvar 决定往哪写。

**并发控制与「取消不该阻塞后续调用」是一对矛盾，解法是 `_locked`。** docstring 说：**一个被客户端取消（超时）的调用会立刻放开锁**——它的线程停不下来、会继续往自己的缓冲里打印，但一个卡住的单元格永不阻塞它之后的调用（包括 `stop_remote_daemon`）。**在拿到锁之前就被取消的调用根本不会运行**。

`_exec_call` 的写法就是这条契约：先用 `asyncio.timeout` 限时抢 `_exec_lock`，抢不到就回 `BUSY`；`finally` 里 `release()`。于是锁的持有时间是「抢到锁到 `_exec_reply` 返回」——而 `_exec_reply` 在 deadline 到点就返回，不等代码结束。

**浏览器选择有三级回退，且每一级的来源都被注释解释。** `_attach` 的顺序：云浏览器（本服务器启动过）→ 自己的 Chromium（`--mcp`，或 `--cli-mcp` 下用户 Chrome 不可用）→ 用户的 Chrome。`__init__` 里的注释补了背景：**`--mcp`（0.x 的配置形态）用一个临时 profile 的独立 Chromium，带着配置里的 `allowed_domains`/`prohibited_domains`；`--cli-mcp` 用用户的 Chrome，在没有可用 Chrome 的地方用自己的 Chromium**。

**「用户 Chrome 变得可用了」会主动提示，而不是静默切换。** `_attach` 里 `_user_chrome_back()` 为真时，若自己的临时浏览器里**没有 agent 的页面**（`_own_pages()`）就直接 `_leave_own()` 离开；若还有页面则设一条 note 提示模型「可以调 `use_my_chrome()` 切过去（带标签页与登录态），那会关掉本服务器的临时 Chromium 和里面的页面」。这是一个正确的取舍：切换有破坏性（关掉临时浏览器），所以让模型知情决定。

**域名规则在 MCP 侧被重新包了两层，理由都是「错误来得太晚」。** `_refusing` 的 docstring：**`fn(url, ...)` 对规则禁止的 URL 抛出与 agent 辅助函数同样的一行拒绝；浏览器本来也会拦，但 `new_tab()` 会先返回成功，然后下一次调用才失败且不指名任何东西**。也就是说这一层存在的价值是**让错误在正确的调用上以正确的文案出现**。

第二层是 `_refuse_cloud`：对有域名规则的服务器，**云浏览器的管理函数被替换成一律拒绝**，理由是注释那句「cloud browser has no Guard: it would go anywhere」。`NO_CLOUD_WITH_DOMAINS` 的文案也说清了原因：**域名规则只在它自己的 Chromium 上成立，云浏览器会打开任何站点**。这与 `f01` 里「Guard 是自有 CDP 连接上的拦截」是同一件事的推论——云端不受这套 CDP 连接覆盖。

**`http_get` 的实现说明了两处真实漏洞。** `_local_http_get` 的 docstring 直接列出来：**browser-harness 的实现会在设置了 `BROWSER_USE_API_KEY` 时把 URL 与 headers（含 Authorization、cookies）发给 fetch-use 代理，并且允许从被允许的主机重定向到任意主机**。所以这里换成了自己的 opener：**每一跳重定向都过一遍规则检查**，且**不调用 `install_opener`**——注释理由是「本进程的云、登录与遥测调用也用 urllib」，全局替换会波及它们。

**登录是跨调用完成的，因为用户要在他自己的浏览器里点批准。** `_Login` 的 docstring：**一次浏览器 OAuth 登录（browser-harness 的流程与密钥存储），在一个跨工具调用的线程里完成**。它构造时先做三件准备：设 `BH_DEVICE_NAME`、`harden_login_callback()`、`set_aside_corrupt_store()`——最后一项的注释说明了必要性：**一个损坏的密钥存储会让保存新密钥失败，而那发生在用户已经批准之后**。

`_sign_in` 用 `_login_lock` 保护 `self._login`，等待最多 `LOGIN_WAIT_SECONDS = 45`，返回 `(文本, 状态)`。状态里区分 `failed` / `start_failed` / `signed_in`，而 `call_tool` 里的注释记了一个 bug：**`is_error` 早先是恒 `ok=True`，让一次失败的登录变成不可见**。

**遥测与工具调用解耦，且绝不拖垮调用。** `_record_call` 的 docstring：**每次工具调用一条 `mcp_server_event`，入队、由遥测线程发送；绝不让调用失败**。实现上它在 `finally` 里跑，并用 `_call_errors` 这个 contextvar 收集本调用内发生的错误分类（`_error_class`）。

**未实现的 0.x 工具名有专门的处理。** `OLD_TOOLS` 列出 0.x `--mcp` 服务端的工具集（它跑的是被移除的引擎），对其中名字返回 `removed(...)` 生成的迁移文案而不是「未知工具」。注释说明 `browser_screenshot` 被保留了下来——它是唯一从 0.x 活到 1.0 的工具名。

**`stdout` 在服务期间被整体换成 `stderr`。** `run()` 里那句 `stdout, sys.stdout = sys.stdout, sys.stderr`，注释给出理由：**模型代码启动的线程在调用之外 `print()`，会留在 `sys.stdout` 的缓冲里，在进程退出时被刷到已经恢复的协议流上，把它弄坏**。所以服务期间一律写到 stderr。

**Windows 与 POSIX 的云浏览器清理路径不同，注释解释了为什么。** `run()` 的 `finally` 里：**Windows 上客户端关掉会话后要在退出前停掉云浏览器，不能只靠 reaper**（注释：客户端会在服务器退出后立刻杀掉它的 Job Object，连带杀掉一个 WMI 可能没起来过的 reaper）；**POSIX 上不做**，因为那里 reaper 已经负责，而这次 stop 会等它 spawn 的 daemon 最多 15 秒。

顶部那段 `_REAPER` 的长注释讲了清理进程的设计：POSIX 靠「只有本服务器持有的 stdin 管道在 EOF 时返回」，Windows 靠「服务器在客户端创建的 Job Object 里，所以 reaper 要在 Job 之外启动（`_spawn_outside_job`）并等服务器进程」。还有一条安全细节：**浏览器 id 走环境变量而不是 argv，因为它无需密钥就能驱动浏览器，而 argv 对任何本地用户的 `ps` 都可见**。

**`main()` 设了三个环境变量，每个都有具体理由。** `set_entry('mcp')`（locating 登录入口）、`BH_CLIENT='browser-use-mcp'`、以及 `BU_MODEL_CODE='1'`——最后一项注释：**`browser_exec` 跑的是客户端模型的代码，所以它的 `browser_use.feedback()` 不算用户同意**。这与 `c03` 里那条 `BU_MODEL_CODE` 是同一条纪律。

**异常组的收尾处理。** `main()` 里 `except* OSError as group` 并用 `group.split(_client_gone)[1]` 拆出「客户端已断开」的那部分再抛出——注释说明 mcp 库的 task group 会把失败写入包进 `ExceptionGroup`，所以这里要把「客户端走了」这个正常终止与其它 OSError 分开。

## 关键取舍

**源码事实：** `CALL_SECONDS = 50` 是为了赶在 MCP 客户端的 60 秒取消之前回复；代码在独立线程上继续跑，超时不是失败。

**源码事实：** 单元格输出用游标做增量返回，单次返回受 `MAX_OUTPUT_CHARS` 限制，超出落盘并在文本里给出路径。

**源码事实：** stdout 捕获是 per-thread 的（`_CellStream` + contextvar），所以单元格启动的线程打印不进结果，且卡住的单元格的打印不会污染下一次调用。

**源码事实：** 被取消的调用立刻释放锁，线程继续跑但不再阻塞后续调用；没抢到锁的调用根本不会运行。

**源码事实：** 域名规则在 MCP 侧被重新包一层（让拒绝发生在正确的调用上），并且**云浏览器管理函数在设了域名规则时被禁用**，因为云浏览器没有 Guard。

**源码事实：** `http_get` 不沿用 browser-harness 的实现，因为它会外发带凭据的 headers 并把重定向跟到任意主机；这里每一跳都检查规则，且不污染全局 opener。

**源码事实：** 登录跨调用完成，45 秒等待，且登录失败必须反映到 `is_error`（修过一个恒 `ok=True` 的 bug）。

**源码事实：** 服务期间 `sys.stdout` 指向 stderr，防止模型代码在线程里的 `print` 在进程退出时污染协议流。

**源码事实：** 云浏览器的浏览器 id 走环境变量而非 argv，因为 argv 对其他本地用户可见，而这个 id 足以驱动浏览器。

**源码事实：** 0.x 的工具名返回迁移文案；`browser_screenshot` 是唯一保留下来的旧工具名。

**架构推论：** 这个服务端最值得学的是**它对宿主约束的诚实处理**。MCP 客户端的 60 秒超时是外部事实，无法协商，于是执行模型被改成「调用是窗口、代码是主体」；stdio 是协议通道，于是服务期间 stdout 被改道到 stderr；Windows 的 Job Object 会连带杀子进程，于是清理逻辑要在 Job 之外另起。这些都不是优雅的抽象，而是**在具体宿主约束下唯一能工作的形状**。相比之下，「把 timeout 调到 300 秒」或者「让用户别用 Cursor」之类的做法都不成立——前者客户端不认，后者不是工程决策。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer` | 有状态的 stdio MCP 服务端主体 |
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer._tool_definitions` | 四个工具的定义与给模型的说明 |
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer._exec_call` | 限时抢锁 + 到点即回的调用入口 |
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer._exec_reply` | 增量输出、仍在运行说明、早先单元格的结束汇报 |
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer._locked` | 取消即释放锁的串行执行 |
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer._attach` | 云浏览器 → 自有 Chromium → 用户 Chrome 的三级回退 |
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer._ensure_namespace` | 持久命名空间与工具注入，含域名规则的包装 |
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer.run` | stdio 服务循环、stdout 改道、Windows 云清理 |
| `browser_use/mcp/cli_mcp.py` | `_Cell` / `_run_cell` | 一次执行的单元与线程入口，活过调用的载体 |
| `browser_use/mcp/cli_mcp.py` | `_CellStream` / `_capture` | per-thread 输出捕获，隔离跑飞的单元格 |
| `browser_use/mcp/cli_mcp.py` | `_Login` | 跨调用完成的浏览器 OAuth 登录 |
| `browser_use/mcp/cli_mcp.py` | `_refusing` / `_local_http_get` | 域名规则的早期拒绝与逐跳检查的 `http_get` |
| `browser_use/mcp/cli_mcp.py` | `OLD_TOOLS` / `OLD_TOOL_HINT` | 0.x 工具名的迁移文案 |
| `browser_use/mcp/cli_mcp.py` | `_REAPER` / `_spawn_outside_job` | 服务器退出后停掉云浏览器的清理子进程 |

## 参考

- [browser-use 源码：mcp/cli_mcp.py](https://github.com/browser-use/browser-use/blob/main/browser_use/mcp/cli_mcp.py)
- [MCP 规范：stdio 传输](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports#stdio)
- [browser-use 文档：CLI 与 MCP](https://docs.browser-use.com/customize/mcp-server)
- [browser-harness 仓库](https://github.com/browser-use/browser-harness)

## 与主核 / AI 的关系

这个服务端与主核里「把长任务暴露成一个可能超时的接口」是同一类工程问题。数据库的 `COPY`、`VACUUM`、长时间查询都会超过客户端的语句超时；正确处理不是把超时改大，而是**让操作在后台继续、让接口变成可轮询/可续接的**（作业 id、进度查询、结果回取）。这里的 `_Cell` + 空 code 等待就是同一个模式的轻量版本。

对 agent 工程的直接价值有三条。其一，**别和宿主客户端的超时对抗，绕开它**。50 秒回复 + 代码继续跑 + 空调用取剩余输出，这三件加起来才是可用的长任务接口。其二，**per-thread 输出捕获是必须的**。一旦执行是「线程活过调用」，全局的 `sys.stdout` 重定向就会让两次调用的输出互相污染；把「当前线程属于哪个单元」放进 contextvar 才是唯一干净的解法。其三，**安全能力要跟着执行位置走**。域名规则在本地 Chromium 上由 Guard 保证，一旦切到云浏览器就完全失效——所以工具里必须把云浏览器的入口禁掉，而不是期望规则「也适用于云」。
