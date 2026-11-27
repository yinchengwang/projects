---
project: browser-use
feature: repl-worker
title: 工作进程与单元执行
page_type: feature
schema_version: 1
refs:
  - path: browser_use/repl/worker.py
    symbols: [CellStats, _compile, _run]
  - path: browser_use/repl/worker.py
    symbols: [_watch_parent, _redact_between_cells, main]
---

# 工作进程与单元执行

## 一句话职责

工作进程是那个真正跑模型写的 Python 的地方：它把一个命名空间搭好（browser-harness 助手 + 截图 / 敏感值 / 检查点等自建函数）、把每个单元格的 stdout/stderr 重定向进可脱敏的管道、把执行结果连同统计一起回给父进程——而它自己随时可能被父进程连整棵树杀掉。

## 机制是什么

一个「单元格」（cell）在这里不是 `exec()` 一下那么简单。围绕它的有三层，缺一层都会漏掉一类问题：

| 层 | 做什么 | 不做的后果 |
|---|---|---|
| 命名空间 | 助手函数、`cdp()` 包装、`type_secret`、`checkpoint`、`screenshot` | 模型得自己拼 CDP，且敏感值无处安放 |
| 输出通道 | stdout/stderr 经脱敏泵写入每单元格文件 | 单元格被杀时输出全丢；敏感值落到磁盘 |
| 生命周期 | 父进程看护、输出重定向的保存与恢复、`atexit` 排水 | 父进程死后单元格继续操作浏览器 |

关键在于**输出必须先落盘再回传**。父进程从不直接接收单元格的输出，而是读那个文件：这样「单元格超时被杀」也能拿到它已经打印的内容。这是这套设计里最实用的一处结构选择。

## 怎么实现的

**入口 `main()` 的顺序几乎每一步都有原因。** 按顺序：

1. **Windows 上先给自己的进程建作业对象**（`_OWN_JOB = _job_for(os.getpid())`）。注释给了非常具体的理由：「a venv python.exe launcher starts the real interpreter before the parent assigns it」——父进程在 `Popen` 之后才调 `_job_for`，而 venv 的 `python.exe` 是个启动器，它会先起真正的解释器，那时父进程还没来得及把作业对象分配下来。所以 worker 自己也得建一个，否则在父进程分配前就失联的那部分进程不会被收起。
2. **起 `_watch_parent` 线程**（`daemon=True`），pid 来自 `BU_REPL_PARENT_PID`（读后即 `os.environ.pop`，不留给单元格看到）。
3. **回连父进程**：`socket.create_connection(('127.0.0.1', int(os.environ.pop('BU_REPL_PORT'))))`，写出令牌行，读一行 init。
4. **配置输出编码**：`sys.stdout.reconfigure(encoding='utf-8', line_buffering=True)`，stderr 同样。
5. **`_redact_between_cells(secrets)`**——注意它的位置在 `reconfigure` **之后**，注释明确写了原因：「after `reconfigure()`: it seeks, which a pipe refuses」。`reconfigure` 会 seek，而管道不可 seek；若先做重定向就会失败。
6. **建命名空间**：先放 `{'__name__': '__main__', '__bu_display': _display}`，再 `_install(ns, secrets)`，再按 init 里的四个开关决定加不加 `search` / `downloads` / `rotate_proxy` / `report_issue`。
7. **`sys.path.append(os.getcwd())`**，注释：「after the imports（`-P` kept it off `sys.path[0]`）: modules the agent writes still import」。即父进程用 `-P` 把工作区踢出 `sys.path[0]` 防标准库被遮蔽，但模型写的模块仍要能 `import`——所以等自己所有 import 完成后再把它加到末尾。
8. 建事件循环、`send({'type': 'ready'})`，然后进入逐行读消息的主循环。

**`_compile` 做 REPL 式「显示最后一个表达式」。** 两趟编译：

```python
tree = compile(code, f'<cell {n}>', 'exec', flags=ast.PyCF_ONLY_AST | ast.PyCF_ALLOW_TOP_LEVEL_AWAIT)
if tree.body and isinstance(tree.body[-1], ast.Expr):
    last = tree.body[-1]
    call = ast.copy_location(ast.Call(ast.Name('__bu_display', ast.Load()), [last.value], []), last.value)
    tree.body[-1] = ast.copy_location(ast.Expr(call), last)
    ast.fix_missing_locations(tree)
return compile(tree, f'<cell {n}>', 'exec', flags=ast.PyCF_ALLOW_TOP_LEVEL_AWAIT)
```

三处细节：`ast.copy_location` 用了两次（构造 `Call` 时与包成 `Expr` 时），是为了让异常回溯指向原始行号而不是 `<unknown>`；`ast.fix_missing_locations` 补全新节点缺失的位置信息；`PyCF_ALLOW_TOP_LEVEL_AWAIT` 让单元格能直接写 `await ...`——对一个要操作浏览器异步 CDP 的 REPL 来说这是必需的。

`_run` 根据 `co_flags & inspect.CO_COROUTINE` 决定 `loop.run_until_complete(eval(...))` 还是 `exec(...)`。注意用的是**同一个持久 event loop**（`asyncio.new_event_loop()` 建一次），所以单元格里定义的 async 状态能跨单元格存活。

`_display` 会跳过 `None` 与 `ax.Printed`（注释：`bu` already printed it），其余用 `pprint.pformat(value, width=120, sort_dicts=False)`。`sort_dicts=False` 是为保持插入顺序——对模型读字典来说顺序有意义。

**输出重定向：每个单元格一组 `os.pipe()`。** 主循环里每次迭代都新建管道与脱敏泵线程：

```python
rfd, wfd = os.pipe()
pump = threading.Thread(target=_pump, args=(rfd, out_path, secrets), daemon=True)
pump.start()
saved = os.dup(1), os.dup(2)
os.dup2(wfd, 1); os.dup2(wfd, 2); os.close(wfd)
```

于是**单元格里起的子进程也继承 fd 1/2**，它们的输出同样进管道。`finally` 里依次恢复（`os.dup2(saved[0], 1)` 等并关掉保存的 fd），再 `pump.join(5)`——注释写「a background subprocess may hold the pipe open; its later output still lands in the file」，即容忍泵线程才等 5 秒，因为单元格留下的后台进程可能一直握着写端。

读完文件头（`MAX_OUTPUT_CHARS + 1` 字节）后 `clip`，再 `stats.take()`，最后 `send` 一份完整的 result。

`_pump` 是脱敏落盘的核心，值得逐行看：

```python
forms = secret_forms(secrets)
keep = max((len(f) for f, _ in forms), default=1) - 1
by_line = not any('\n' in f for f, _ in forms)
```

三处设计：

- **`keep`**：每个秘密值最长形态的长度减一。写入前**扣住尾部这么多字符**不写，因为它可能是某个秘密值的前缀——下一块数据来了才判完整。
- **`by_line`**：若没有任何秘密形态含换行，就可以「最多写到最后一个换行」，进一步减少扣留量，降低把输出切成两半的概率。反之若秘密里可能有换行，就不能按行截断。
- **`decoder = codecs.getincrementaldecoder('utf-8')('replace')`**：增量解码。按字节块读就不可能保证 UTF-8 边界，所以必须用增量解码器而不是每块 `decode()`。`errors='replace'` 保证非法字节不会抛错。

`newline=''` 也有注释：「Windows: the cell already wrote `\r\n`; no `\r\r\n`」。

这段的意义可以用 docstring 一句话概括：「so secret values never reach the disk even when the cell is killed」——**脱敏发生在写入侧而不是回传侧**。所以即使单元格被杀、输出只剩磁盘上那半截，那半截里也没有明文秘密。

**`_redact_between_cells` 覆盖「单元格之外」的输出。** 单元格之间若有遗留线程、子进程或警告往 stdout 写，那也带着潜在的敏感值，且直通 `worker.log`（父进程起的那个 fd）。所以它做的是**常驻重定向**：开一对管道、起泵写 `worker.log`（`'a'` 追加模式）、`os.dup2` 到 1/2，并注册 `atexit` 的 `drain()` 把 1/2 换回去、`pump.join(2)`。

`drain` 的注释解释了为什么必须显式排水：「close the pipe so the pump writes what it holds（a crash traceback）and ends」——退出时若不 flush 并关写端，泵不知道流已结束，最后扣留的那 `keep` 个字符就永远不落盘，一次崩溃的 traceback 可能就此丢失。

**看护线程 `_watch_parent` 是「父死子亡」的实现。** 分平台：

- **Windows**：`OpenProcess(0x00100000, False, parent)`（`SYNCHRONIZE`）后 `WaitForSingleObject(handle, INFINITE)`。拿到句柄就直接阻塞等父进程退出——比轮询精确。注释说句柄为 `None` 表示父进程已不在，那就往下走 `os._exit(1)`。
- **POSIX**：`while os.getppid() == parent: time.sleep(1)`。注释解释「a dead parent's children are reparented」——`getppid()` 变化就是父死的信号。发现父死后，若自己确实是进程组长（`os.getpgrp() == os.getpid()`，因为父进程启动时用了 `start_new_session`），就 `os.killpg(os.getpid(), signal.SIGKILL)` 把自己整组杀掉。

最后统一 `os._exit(1)`（不是 `sys.exit`）——要立即终止，不跑 `atexit`，因为此时可能已经有失控的单元格残留。

**`_install` 是命名空间搭建的主体，也是最长的一段。** 它的开头有一处很讲究的处理：

```python
env = dict(os.environ)                      # the allowlisted env the parent gave us
from browser_harness import helpers          # loads .env files and agent_helpers.py into this process
os.environ.clear()                           # whatever those added (DATABASE_URL, ...) does not survive
os.environ.update(env)
```

即：**先快照当前（白名单化过的）环境，再 import browser-harness（它会读 `.env` 并往 `os.environ` 里塞东西），然后清空并恢复快照**。这样 `.env` 里的密钥不会通过 `os.environ` 泄漏给单元格代码。这是一段只有三行但很关键的清理。

然后是一系列包装：

- **`send` 包装 `helpers._send`**：所有 CDP 请求的必经点。三件事：请求体超 `MAX_IPC_REQUEST`（`65536 - 128`）就抛带建议的 `ValueError`（注释说 browser-harness 的守护进程按 asyncio 默认 64 KiB 读一行，超了就 `BrokenPipeError`）；捕获连接类异常与守护进程的 websocket 关闭文本（`BROWSER_GONE` 正则）转成 `BrowserConnectionLost`；对 `set_session` 与 `drain_events` 两种 meta 做记账。
- **`cdp` 包装**：这是这一页最精巧的部分，专门解决「JS 对话框冻结页面」这个真实坑。docstring 写得很具体：「js() then took 20 s, click_at_xy 20 s and screenshot() 65 s, each with a timeout that did not say why」。做法是三层：若守护进程已知道有对话框打开（`pending_dialog`），把这次调用的 `_response_timeout` 压到 `DIALOG_PROBE_S = 2.0` 并标记 `_frozen`；若这次调用**自己可能打开**对话框（`HELD_BY_DIALOG` 里的方法或 `Input.*`）就走 `raced`；两条路都在超时/错误时把 `dialog_open(...)` 的解释拼进错误。
- **`raced`**：把调用放进后台线程，主线程每 `DIALOG_POLL_S = 0.25` 秒问一次守护进程有没有对话框；一发现就抛解释性错误（注释说「the call stays queued: handling the dialog completes it」）。
- **`timed`**：所有 CDP 调用的遥测计时，按方法名聚合。

**敏感值输入 `type_secret` 与 `_type_keys`。** `type_secret(name, id=None)` 从 `secrets` 里按名字找候选（注意可能有多个同名不同域的），域作用域用 `domain_matches` 判当前 frame 的 URL；名字以 `bu_2fa_code` 结尾时值被当作 TOTP 种子，现场算出 6 位码（`totp()` 是 RFC 6238 的 SHA-1/30 秒实现）。`_type_keys` 逐字符发按键事件，注释给了三条理由：有些表单监听 `keydown/keyup` 才启用提交按钮；`Input.insertText` 不触发按键事件；而换行要发 Enter、其他控制字符必须走 `Input.insertText`，否则 Tab 会把焦点移走、剩下的字符落到下一个输入框。

**`refuse_placeholder` 拦一个很具体的失败模式。** docstring 描述得极为具体：「the model typed that text into the form, read it back（redacted the same way）and reported "Wrong credentials"」。即模型看到输出里是 `<secret:name>`，就以为那是要填的值，直接把它打进了表单。所以任何 CDP 参数里出现已知名字的占位符就报错，并提示正确做法（`type_secret(...)`）。注释还强调「Nothing was sent」——报错发生在请求发出之前。

**`ask` 限制主线程**，注释解释了为什么：单元格遗留的线程若也能读到控制通道，就会**取走下一个单元格的消息**。所以 `search()` / `downloads()` 这类「让父进程代做」的函数有硬约束——只能在主线程顺序调用。这也顺带解释了为什么 `search()` 的 API key 能留在父进程：值从不进入 worker 进程。

**`checkpoint` 与 `partial.json`。** `checkpoint(filename, value, partial=False)` 先把值过一遍 JSON 往返（`json.loads(json.dumps(value, default=_jsonable))`）再 `redact`，注释写「the checkpoint file never holds a secret value」。`_write_atomic` 用「写 `.tmp` → `os.replace`」，注释说「a kill mid-write leaves the old file, never a truncated one」——在一次可能被 `SIGKILL` 打断的进程里，这个原子性不是讲究，而是必需。

`partial=True` 时还会写 `state/partial.json`（含文件路径与值），作为这次运行的兜底结果。

**`CellStats` 是「只记形状不记内容」的遥测容器。** docstring 明确写「Never arguments, URLs, page text or values」。它的字段揭示了几处判断：

- `navigated` / `tabs`：注释说 `Browser('chrome')` 会挂到用户已经打开的标签上，所以「一次页面读取只有在**本 worker 导航过**之后才算数」。否则用户自己的邮件页面会被算成 agent 访问的页面。
- `keys` 与 `record_key` / `end_keys`：逐字符的按键要被拼成一段文字才算「输入过的内容」，因为单个字符太短，脱敏替换不掉。`record_key` 遇到带修饰键（`modifiers & 7`）的按键就结束拼接——快捷键不算输入文字。
- `seen`：URL 的哈希集合，注释写「never leave this process」，用于去重与回访统计。
- `json_pages`：顶层是 JSON 的页面，注释说云端浏览器会把它们各存成一个下载。
- `nav_failed` 只从 `Page.navigate` 的 `errorText` 里抽 Chrome 的 `net::ERR_*` 名字，注释写「never the URL」。

`stats.take()` 在每个单元格开始时被调一次（`__init__` 末尾也调，用来初始化），把累积值取走并清零。

## 关键取舍

**源码事实：** 单元格输出先经脱敏泵写入 `state/cells/NNNN.txt`，父进程再从该文件头部读取回传；因此单元格被杀也不丢已产出的输出，且磁盘上不留明文秘密。

**源码事实：** 泵写入时会扣留尾部若干字符（最长秘密形态长度减一），防止把某个秘密值的前缀先写出去；若不含换行的形态则可按最后一个换行截断。

**源码事实：** 输出重定向用 `os.dup2` 换 fd 1/2，所以单元格起的子进程也被涵盖；`finally` 恢复保存的 fd，`pump.join(5)` 容忍后台进程继续握着管道。

**源码事实：** `main()` 里 Windows 先自建作业对象，因为 venv 的 `python.exe` 启动器会在父进程分配作业对象之前先起真正的解释器。

**源码事实：** `_redact_between_cells` 把单元格之间的输出也纳入脱敏，并在 `atexit` 里 flush + 关写端 + join，确保扣留的尾部（可能是崩溃 traceback）落盘。

**源码事实：** `_install` 在 import browser-harness 前后做一次环境快照与恢复，挡住 `.env` 里新增的变量进入单元格可见环境。

**源码事实：** `cdp()` 包装专门处理「JS 对话框冻结页面」，把原本静默的 20–65 秒超时变成「页面被对话框冻结」的明确错误。

**源码事实：** `refuse_placeholder` 在请求发出前拦掉「把 `<secret:name>` 当成值填进表单」这个由脱敏本身诱发的错误。

**源码事实：** `ask` 只允许主线程调用，因为控制通道只有主线程读，遗留线程会抢走下一个单元格的回复。

**架构推论：** 把输出「先落盘、再回传」而不是「边跑边流式回传」，是与「可强制中断」配套的选择。流式回传在超时被杀时会留下一个残缺的流，而落盘则天然是「最后一次 flush 为止」的完整前缀。代价是每次单元格多一次文件读，并且父进程读到的是稳定的快照而非实时输出。

**架构推论：** 整个 worker 的设计处处以「我随时会死」为前提：原子写文件、`atexit` 排水、`os._exit` 而不 `sys.exit`、看护线程。这与父进程侧的「绝不留下半个 worker」互为镜像——两侧都在解决同一个问题：**一段可能永不返回的代码所依赖的任何东西，都必须在任意时刻可被放弃**。

**架构推论：** `send` 包装里把 `MAX_IPC_REQUEST` 定成 `65536 - 128` 而不是整数 64 KiB，是为了给 Windows 上令牌可能追加的字节留余量。这类「留一点余量」的常量通常来自一次真实的线上断裂，注释里那句「a longer one broke the connection（`BrokenPipeError`）」就是证据。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/repl/worker.py` | `main` | 入口：作业对象、看护线程、回连、编码、命名空间、单元格主循环 |
| `browser_use/repl/worker.py` | `_install` | 命名空间搭建：`send`/`cdp`/`timed` 包装、助手注入、环境快照恢复 |
| `browser_use/repl/worker.py` | `_compile` / `_run` | REPL 式末表达式显示、顶层 await、复用同一事件循环 |
| `browser_use/repl/worker.py` | `_pump` | 输出落盘时的增量脱敏，扣留可能的前缀 |
| `browser_use/repl/worker.py` | `_redact_between_cells` | 单元格之间的输出也脱敏，`atexit` 排水 |
| `browser_use/repl/worker.py` | `_watch_parent` | 父进程死亡探测与整组自杀 |
| `browser_use/repl/worker.py` | `CellStats` | 只记形状的遥测：计数、耗时、事件，不含内容 |
| `browser_use/repl/worker.py` | `_write_atomic` | `.tmp` + `os.replace` 的原子写 |
| `browser_use/repl/worker.py` | `checkpoint` | 脱敏后原子保存、可选写入 `partial.json` |
| `browser_use/repl/worker.py` | `type_secret` / `_type_keys` | 域作用域敏感值输入、逐键事件、TOTP |
| `browser_use/repl/worker.py` | `refuse_placeholder` | 拦截把占位符当真值填入表单 |
| `browser_use/repl/worker.py` | `cdp` / `raced` / `dialog_open` | 对话框冻结的探测与解释性报错 |
| `browser_use/repl/worker.py` | `_ask_parent` / `search` / `downloads` | 主线程专用的父进程代做通道 |
| `browser_use/repl/worker.py` | `_error` / `_message` | 剔除已安装代码帧的回溯、遥测用的单行摘要 |

## 参考

- [browser-use 源码：repl/worker.py](https://github.com/browser-use/browser-use/blob/main/browser_use/repl/worker.py)
- [browser-harness 仓库](https://github.com/browser-use/browser-harness)
- [browser-use 仓库：AGENTS.md 的架构一节](https://github.com/browser-use/browser-use/blob/main/AGENTS.md)
- [Chrome DevTools Protocol 文档](https://chromedevtools.github.io/devtools-protocol/)
- [Python 文档：ast 的 PyCF_ALLOW_TOP_LEVEL_AWAIT](https://docs.python.org/3/library/ast.html#ast.PyCF_ALLOW_TOP_LEVEL_AWAIT)

## 与主核 / AI 的关系

「把输出重定向到可审计的落盘通道」与主核里的**日志与审计写入**是同一种结构：数据在产生侧就被整形（脱敏、截断），而不是在消费侧补救。差别在主核的日志不会被中途杀掉，而这里的输出必须假定「写到一半进程就死了」——所以才有原子写、扣留尾部、`atexit` 排水这些额外动作。

「只记形状不记内容」的遥测容器也值得对照主核的**统计视图**：表大小、访问计数、缓存命中率，都不含行内容。这条界线的价值在于：**遥测数据会被外发到第三方，而业务数据不会**。一旦遥测里混进了页面文本或用户输入，就等于把用户数据送到了遥测后端。

对 AI 工程的实际意义在于：这份实现几乎每一处都在回应「模型写的代码不守规矩」这一现实。它看到模型把脱敏占位符当成真值填进表单，就加了 `refuse_placeholder`；它看到模型被对话框卡住却只收到一个无解释的超时，就把超时改写成解释性错误；它看到逐字符输入太短无法脱敏，就把按键拼成一段再处理。**这些都不是理论设计，而是一条条从实际失败里长出来的防御**。做同类系统时最值得学的不是具体补丁，而是这个态度：把每一次「模型行为意外」都变成一处显式的、带解释的拒绝或重写。
