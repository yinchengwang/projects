---
project: browser-use
feature: feedback-and-auth-events
title: 反馈与登录事件
page_type: feature
schema_version: 1
refs:
  - path: browser_use/telemetry/feedback.py
    symbols: [feedback, RunReports]
  - path: browser_use/telemetry/auth_events.py
    symbols: [capture, keyed_success]
---

# 反馈与登录事件

## 一句话职责

两个「除使用统计之外」的遥测通道：`library_feedback` 把用户或 agent 写的反馈发回团队，`library_auth` 记录「没有密钥的人卡在哪一步、后来拿到了没有」——两者各自用不同方式界定「谁在说话」与「什么可以离开本机」。

## 机制是什么

| 通道 | 事件名 | 触发来源 |
|---|---|---|
| `feedback.py` | `library_feedback` | agent 单元格里的 `report_issue()`、Python 的 `browser_use.feedback()`、CLI 的 `browser-use feedback`、MCP 的 `browser_feedback` 工具 |
| `auth_events.py` | `library_auth` | `missing_key` / `key_rejected` 两种卡点、`keyed_success` 一次性的成功回执、登录各阶段 |

两者都复用 `h01` 那套发送管道（`telemetry()` 的线程与出箱），差别全在**准入判定**与**数据边界**上。

## 怎么实现的

### 反馈：谁是「用户」，谁是「用户的工具」

**模块 docstring 把三种身份讲清了，这是本页最重要的内容。** 原文大意：用户自己的调用（`python`、`cli`）**属于同意**，即使用量统计关闭也发；**一个编码 agent 在用户 shell 里跑 `browser-use feedback`（技能让它这么做）是用户的工具在做用户装它来做的事，同样算同意**；而 **agent 的模型与 MCP 客户端的模型不是用户**，统计关闭时什么都不发。

**`BU_MODEL_CODE` 环境变量是这条区分的技术载体。** 代码里的判定是：`if source in ('python', 'cli') and os.environ.get('BU_MODEL_CODE'): return None`——**模型以用户之名写的报告**被拦掉。此时返回的文案不是「已发送」而是 `MODEL_CODE`：`From an agent cell use report_issue(...), from an MCP client the browser_feedback tool: nothing was sent.`。

而 docstring 紧接着有一句自我限定，值得原样记住：**Best effort, not a boundary: that code can unset the variable, and it can reach the network anyway; the flag only keeps the model off the user's path by mistake.** 也就是说这个标志不假装是安全边界，它只防「模型误用了用户的通道」，不防「模型有意绕过」。

**`report_issue()` 是给 agent 单元格的正确入口，而它有不同的隐私含义。** `RunReports` 类承载这条路径：它由 `Repl(feedback=...)` 注入、由 Agent 在每次运行开始时 `start()` 绑上本次运行的 `run_id`、`rec`（`RunRecorder`）与 `stats`（`RunStats`）。这意味着**从 agent 单元格发出的反馈会带上本次运行的上下文**：模型名、`run_id`、当前步数、最后一个失败的单元格错误类型。

而这些信息是**自动附加的、不需要也不允许用户提供**。`RunReports.__call__` 里构造 `capture(...)` 的参数就是这些：`model=self.model`、`run_id=self.run_id`、`step=len(steps)`、`last_error_type=errors[-1]`。副作用是这类反馈天然带诊断价值，而不只是一段文本。

**「每运行最多 3 条」的限额是为了对付循环里的模型。** `MAX_PER_RUN = 3`，注释：**一个卡在循环里的模型只报一次，而不是每轮一次**。三个入口各自实现：`RunReports` 用 `self.sent` 计数（每次 `start()` 清零）、`mcp_call` 用模块级 `_mcp_sent`（注释：**一个 MCP 服务端就是一个客户端会话**）。到限后返回的文案是 `Already reported 3 issues this run: nothing more was sent.`，且 `is_error=False`——**超限不是错误**，否则模型会以为要重试。

**消息先净化再离开。** `capture` 的 `build` 回调里调 `scrub(text, forms, MAX_MESSAGE, tasks or [], typed or ())`。净化范围在 docstring 里列全：**密钥、凭据、邮箱、URL 查询串、家目录**一律处理；**对 agent 来说连 task 文本与输入过的文本也处理**（作为占位符）。

`secret_forms(secrets)` 与 `c03` 用的是同一个函数——把每个敏感值展开成多种可辨识形态（含 HTML 转义、base64 核心），再逐个替换。于是**用户提供的 `sensitive_data` 也会从它自己写的反馈里被抹掉**。

**`MAX_MESSAGE = 2000` 在净化时作为上限传入**，所以截断发生在净化之后而不是之前——这是个正确的顺序：先替换敏感值再截断，否则可能截掉一半的令牌而留下另一半。

**`category` 有白名单兜底。** `kind = category if category in CATEGORIES else 'other'`，`CATEGORIES = ('bug', 'docs', 'missing', 'other')`。三个来源（cell、MCP、Python）都可能传任意字符串，所以归一化在 `capture` 里统一做，而不是各入口自己做。

**MCP 工具的 schema 本身就把「不要写什么」写进了描述。** `MCP_TOOL['description']` 末尾那句：`Not for ordinary website problems; never include secrets, keys, passwords or customer data.`，以及要求第一行写 `kind: integration_report | feature_request | bug`。这是一个把隐私约束前置到模型可见文本的做法——它能减少需要净化的量。

**`RunReports` 刻意不持有 REPL 或会话。** 类 docstring：**它不持有 REPL 或会话，所以一个没有 `close()` 就被丢弃的 Agent 仍能释放它的 worker**。这是一个资源生命周期的细节：反馈对象挂在 Agent 上，若它引用了 REPL，就会阻止 worker 被回收。

### 登录事件：只发枚举、布尔与计数

**模块 docstring 一句话定义了数据边界**：**只发枚举、布尔与计数：绝不含密钥、它的 id、项目、登录 URL、state 或 code**。这句是这一整块的核心约束，其余实现都是它的推论。

**本地状态文件保存三个事实，且只有数字会离开。** `telemetry_dir()/auth_state.json` 记录：**这台设备第一次需要密钥是什么时候**（`first_gate`）、**需要过多少次**（`gates`）、**第一次带密钥的成功运行是否已上报**（`done`）。注释：**只有数字离开**。

**`capture` 与 `keyed_success` 是两个语义不同的入口。** `capture(stage, method, automatic, fields)` 每发生一次就发一个事件；`GATES = ('missing_key', 'key_rejected')` 是**会累加计数**的两个 stage（`_state(gate=stage in GATES)`）。`keyed_success()` 则**每设备只发一次**：它读状态，若没有 `first_gate` 或已有 `done` 就直接返回；否则**先落盘 `done: True` 再发**。

先落盘再发这个顺序值得注意：它保证即使发送失败，也不会在下次运行时重复发同一个「首次成功」事件。代价是一次发送失败就永久丢失该事件——这个取舍与「每设备一次」的语义是匹配的：事件的价值在于「发生过」，而不在于「被收到」。

**`seconds_since_first_gate` 是这条通道真正想回答的问题。** `_send` 里算的是 `max(0, int(time.time() - float(first))) if first else None`——**距这台设备第一次卡在密钥上过了多久**。配合 `gate_count`，这两个数字能回答「用户从第一次遇到密钥要求到成功跑通花了多长时间、失败了几次」。

**`key_source` 把「密钥从哪来」拆成六个值且从不读取密钥本身。** 顺序是：`param`（网关模型的 `api_key=`）→ `env`（`BROWSER_USE_API_KEY`）→ 存储记录的 `source` 映射（`manual` → `login_manual`、`device_code` → `login_device`、其余 → `login_oauth`）→ `none`。docstring 最后一句是：**从环境与 `auth.json` 读；绝不读密钥**。

实现里有一处防护值得记：读取存储记录被包在 `except Exception` 里，注释：**一个损坏或不可读的存储不持有可用密钥**。也就是说 `key_source` 的失败方向是 `'none'` 而不是抛错。

**`entry` 区分三种程序，而它同时控制「要不要打遥测提示」。** `_entry = ['agent']` 是进程级默认，`set_entry()` 由 CLI 与 MCP 服务端调用改成 `cli` / `mcp`。`_send` 里有一句 `if entry != 'agent': telemetry().notice = False`，注释：**CLI 与 MCP 服务端的 stderr 保持自己的用途**。这是一个把「用户看到的那行提示」也算作遥测设计一部分的处理——库使用与命令行使用的输出契约不同。

**写入磁盘的失败被降级为「进程内保存」。** `_save` 在 `OSError` 时把状态存进模块级 `_UNSAVED[path]`，注释：**目录不可写时为本进程保留它：事件仍然发出并计数**。对应地 `_read` 先查 `_UNSAVED`。这让只读 HOME（或 Lambda 之类无家目录环境）下仍能正确计数，而不是每次读都从空状态开始、把 `gate_count` 永远报成 1。

**`_write` 的临时文件名把 pid 与线程 id 都放进去。** `f'{STATE}.{os.getpid()}.{threading.get_ident()}.tmp'` 再 `os.replace`。`_LOCK` 只保护**本进程内**的读-改-写，注释对此诚实：**counts across processes: about**（跨进程的计数只是近似）。这是一个明确的精度声明，而不是一个隐含的缺陷。

**`warning` 级别的日志一处都没有，全部是 `logger.debug`。** 两类都是「遥测失败不该进入用户视野」的一贯做法，`keyed_success` 的 except 后面还写着 `never breaks a run`。

## 关键取舍

**源码事实：** 「用户自己的调用算同意」，用 `consent=True` 传给 `capture`；而模型以用户之名调用被 `BU_MODEL_CODE` 拦掉，返回明确指向正确入口的文案。

**源码事实：** `BU_MODEL_CODE` 被 docstring 明确声明为 **best effort 而非安全边界**：那段代码能清掉变量，也能直接联网。它只防误用。

**源码事实：** agent 侧反馈自动附带运行上下文（模型、`run_id`、步数、最后失败类型），不需要用户提供。

**源码事实：** 每运行（或每个 MCP 服务端进程）最多 3 条，超限返回成功语义的文案，避免模型重试。

**源码事实：** 消息先净化再按 2000 字符截断（顺序不能反过来，否则可能截断半个令牌）；`sensitive_data` 也参与净化。

**源码事实：** `RunReports` 刻意不持有 REPL 或会话，以便未 `close()` 的 Agent 仍能释放 worker。

**源码事实：** 登录事件只发枚举、布尔、计数；本地文件只保存「第一次需要密钥的时间、次数、首次成功是否已报」，只有数字离开。

**源码事实：** `keyed_success` 每设备一次，且「先落盘 done 再发」——宁可丢一次事件也不重复发。

**源码事实：** `key_source` 从环境与存储记录推导出六个来源值，绝不读取密钥本身；存储损坏时返回 `none`。

**源码事实：** `entry` 非 `agent` 时关闭遥测提示，让 CLI 与 MCP 的 stderr 保持自己的用途。

**源码事实：** 状态文件不可写时降级为进程内保存，使 `gate_count` 仍然正确；`_LOCK` 只保证进程内一致，跨进程「大致准确」被写进了注释。

**架构推论：** 这两个通道共同示范了一件事：**「遥测」不是一种东西，因为「谁在说话」不同**。使用统计的主体是运行本身，所以只需要开关；反馈的主体是**人**（或代替人的工具），所以需要「同意」这个更细的概念，并且要能识别「模型冒充用户」；登录事件的主体是**设备**，所以它用「每设备一次」而不是「每次运行一次」。三种主体对应三种去重与准入规则，把它们混成一套开关必然在其中两个上出错。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/telemetry/feedback.py` | `capture` | 反馈事件的唯一入队口，含三条准入与净化回调 |
| `browser_use/telemetry/feedback.py` | `feedback` | Python 入口，`consent=True` |
| `browser_use/telemetry/feedback.py` | `RunReports` | agent 单元格的 `report_issue()`，带运行上下文与每运行限额 |
| `browser_use/telemetry/feedback.py` | `RunReports.start` | 每次运行重新绑定 id、记录器与统计 |
| `browser_use/telemetry/feedback.py` | `mcp_call` | `browser_feedback` 工具实现，每进程限额 |
| `browser_use/telemetry/feedback.py` | `MCP_TOOL` | 工具 schema，描述里前置了隐私约束 |
| `browser_use/telemetry/feedback.py` | `CATEGORIES` / `MAX_MESSAGE` / `MAX_PER_RUN` | 白名单、净化上限、限额 |
| `browser_use/telemetry/auth_events.py` | `capture` | 卡点事件的入口，gate 同时累计本地计数 |
| `browser_use/telemetry/auth_events.py` | `keyed_success` | 每设备一次的「首次带密钥成功」 |
| `browser_use/telemetry/auth_events.py` | `key_source` | 六种密钥来源判定，从不读密钥 |
| `browser_use/telemetry/auth_events.py` | `set_entry` | 区分 agent / cli / mcp，并影响遥测提示 |
| `browser_use/telemetry/auth_events.py` | `_state` / `_write` | 本地计数的读-改-写与原子落盘 |
| `browser_use/telemetry/auth_events.py` | `_UNSAVED` | 目录不可写时的进程内兜底 |

## 参考

- [browser-use 源码：telemetry/feedback.py](https://github.com/browser-use/browser-use/blob/main/browser_use/telemetry/feedback.py)
- [browser-use 源码：telemetry/auth_events.py](https://github.com/browser-use/browser-use/blob/main/browser_use/telemetry/auth_events.py)
- [browser-use 文档：遥测与隐私](https://docs.browser-use.com/customize/telemetry)
- [browser-use 文档：反馈](https://docs.browser-use.com/support)

## 与主核 / AI 的关系

这两个通道与主核里的「崩溃报告」和「使用统计」的分野一致：崩溃报告携带诊断上下文（版本、配置、堆栈），使用统计只带计数。主核同样要面对「用户主动提交」与「自动采集」两种同意，并且同样不能把二者混成一个开关。

对 agent 工程的直接价值有三条。其一，**「模型不是用户」需要显式建模**。当一个 agent 框架让模型能执行任意代码时，模型随时可以调用「用户才该调用」的入口（发反馈、改配置、装东西）。这类入口的正确处理不是权限检查（模型与用户在同一进程里），而是**换一条通道**：让模型走 `report_issue()` 或 MCP 工具，让用户走 CLI——并接受「这不是安全边界，只防误用」这个定位。其二，**反馈里的自动上下文比文本更有用**。模型写的反馈往往含糊（「它没工作」），但配上模型名、步数、最后失败的类型就能定位。其三，**每设备一次的事件要先落盘再发**。这类事件的语义是「发生过」，重复发比丢失更糟。
