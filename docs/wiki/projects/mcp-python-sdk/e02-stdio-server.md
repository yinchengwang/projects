---
project: mcp-python-sdk
feature: stdio-server
title: stdio 服务端与文件描述符占用
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/stdio.py
    symbols: [_claim_fd, _open_stdin_diversion, _restore_fd]
---

# stdio 服务端与文件描述符占用

## 一句话职责

stdio 传输要把整个标准输出让给协议，于是这个模块不只「读写 stdin/stdout」，而是**接管这两个文件描述符**：服务期间 fd 0 指向空设备、fd 1 指向 stderr，协议数据走一份私有的重复描述符——这样任何意外的 `print` 都不会污染帧流，任何 handler 想读 stdin 也只会读到 EOF。

## 机制是什么

问题很具体。MCP over stdio 用「一行一个 JSON-RPC 消息」的帧格式，写端只有 stdout 一条路。于是：

- 任何第三方库、任何调试 `print`、任何子进程继承的 stdout，只要往 fd 1 写一个字节，就会在流里插入垃圾，客户端立刻解析失败。
- handler 若读 stdin（例如「问用户一个问题」），会把协议的下一条消息吃掉。

光靠「约定不要 print」解决不了——依赖库不受控制。所以这个模块的做法是：**把 fd 0 和 fd 1 从协议手里挪开，让它们变成无害的**。

- `fd 0` 被 `dup2` 到 `os.devnull`（只读打开）→ 任何人读 stdin 得到 EOF。
- `fd 1` 被 `dup2` 到 `stderr`（`os.dup(2)`）→ 任何人的 `print` 都进了 stderr，而 stderr 不是协议通道。若 stderr 不可用则退回 `devnull`（写端）。
- 协议自己用一份**私有重复描述符**（原 fd 1 的副本），经 `os.fdopen(..., closefd=False)` 包成文件对象。

三者合起来就是「接管」：fd 号还在，但它指向的东西被换掉了；协议握着一个不受打扰的副本。

## 怎么实现的

**先看契约注释，它把设计原则写成了四条。** 源码在 `_claims` 定义前有一段注释，标题是「Stream-claim contract」，逐条是：

1. `_claims` 是 fd 0/1 归属的**唯一权威**，只在锁内变更，只由 acquire 的插入与 release 的注销改动。
2. `private_fd` 在**重复描述符被创建的那一刻**就记下来——在 fd 被移动之前——且在该 claim 注册期间**永不关闭**。
3. release **只在 `dup2(private_fd, fd)` 把线恢复之后**才注销；release 失败就保留 claim，于是后继者被拒绝，而不是被喂一个已被改道的描述符。
4. 每次失败都落在安全的那一侧。

这四条不是描述代码，而是**先定下不变量再实现**。第 2 与第 3 条合起来保证了一个关键性质：任何时刻，fd 要么指向协议、要么指向骗局设备，且总有办法恢复；绝不会出现「fd 指向骗局设备、而恢复所需的副本已被关掉」这种不可恢复态。

**`_StreamClaim` 只有两个字段**：`fd` 与 `private_fd`（初始为 `None`，因为「还没创建副本」与「副本已创建」在失败路径上是不同状态）。`_claims` 是进程级 `dict[int, _StreamClaim]`，配一把 `threading.Lock`——用线程锁而不是 anyio 锁，因为接管的建立与恢复可能发生在不同任务甚至同步上下文里。

**`_claim_fd` 的六段式流程，每段失败都有明确的退路。**

1. **确认这条流真的背靠这个 fd**。`_is_backed_by_fd(stream, fd)` 读 `stream.buffer.fileno()` 比对，捕获 `(AttributeError, OSError, ValueError)` 返回假。不背靠就直接返回 `(stream.buffer, None)`——例如测试里传了 `StringIO`，此时既不需要也不该接管。
2. **登记 claim**。锁内检查重复：同一 fd 已被占用就 `RuntimeError("another stdio_server() in this process has already claimed fd {fd}")`。这是「一个进程只能有一个 stdio 服务端」的强制点，注释在 `stdio_server` 的 docstring 里也重复了一遍。
3. **`release` 闭包先定义好**，它的逻辑是：若 `private_fd` 还是 `None`（副本没建成）**或** `_restore_fd` 成功，才在锁内注销 claim。换句话说**恢复失败就不注销**——这话反过来读就是契约第 3 条。
4. **造私有副本**：`_dup_above_std(fd)`。POSIX 用 `fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 3)`——`F_DUPFD_CLOEXEC` 保证新 fd ≥ 3（不落在标准范围内）且带 `CLOEXEC`（不会被 exec 出去）；Windows 用 `os.dup` 后检查结果 ≤ 2，落在标准范围就关掉并抛 `OSError`。这一步失败 → 返回原地缓冲区 + `release`。
5. **打开骗局目标并 `dup2` 到 fd 上**。stdin 的骗局是 `os.open(os.devnull, os.O_RDONLY)`；stdout 的骗局是 `os.dup(2)`，`OSError` 时退回 `os.open(os.devnull, os.O_WRONLY)`。
6. **`dup2` 自身失败的处理**最细：先关掉骗局 fd，再 `_restore_fd(fd, private_fd)` 把线恢复，然后**返回原地缓冲区**并附上 `release`。注释给了两条理由——「Windows 的 `dup2` 可能在失败前就关掉目标」，所以要确保 fd 上仍有线；以及「同一管道上两个写者会撕裂帧」，所以不能在 fd 已被改道时还用原地缓冲区写。

成功路径最后还有两件事：Windows 上 `rebind_std_handle_to_fd(fd)`（让 CRT 的 std handle 跟上 fd 的变动，失败静默），以及 `os.fdopen(private_fd, mode, closefd=False)`。`closefd=False` 的理由写在注释里：**工作线程在传输退出后仍可能阻塞在这个描述符上，所以它绝不能被关闭后被回收**。这一行是整段里最容易写错的地方——默认 `closefd=True` 会在文件对象被 GC 时关掉 fd，进而让一个正在读的线程读到别人的 fd。

**`_restore_fd` 的返回值语义比看起来重要。** 它 `dup2(private_fd, fd)`，`OSError` 时返回 `False`；Windows 上的 handle 重绑是尽力而为（`suppress(OSError)`），**不影响返回值**。docstring 明确写「the Windows handle rebind never affects the outcome」——因为返回值决定的是「claim 要不要注销」，而注销的唯一条件是「线真的回来了」。

**`_UnownedTextWrapper` 解决一个历史 bug。** 它继承 `TextIOWrapper`，只覆写 `close()`：`with suppress(ValueError): self.detach()`。docstring 写得很直白：底层缓冲区不是传输的财产——在原地路径里它就是 `sys.stdout` 自己的缓冲区，在 GC 时把它关掉会**永久毁掉整个进程的 `sys.stdout`**（点名 issue #1933）。`detach()` 只是断开文本层与缓冲区的关联，不关缓冲区。将裸的文本缓冲区包成 `_UnownedTextWrapper` 还顺手解决第二个问题：统一指定 `encoding="utf-8"`。注释说明标准句柄的平台编码不可靠——Windows 上可能是 cp1252 之类，而 MCP 帧必须是 UTF-8。stdin 那边还加了 `errors="replace"`。

**`stdio_server` 的编排。** 它是 `@asynccontextmanager`，参数 `stdin` / `stdout` 可选，显式给了就**跳过接管**（docstring 点明）。两个都缺时各调一次 `_claim_fd`，把 `restore_*` 存起来，`anyio.wrap_file` 成异步文件。然后建两对内存流：`read_stream_writer`/`read_stream` 与 `write_stream`/`write_stream_reader`（`create_context_streams[SessionMessage | Exception](0)` 的缓冲参数为 0，即不缓冲）。

两条后台任务：

- `stdin_reader`：逐行读，`types.jsonrpc_message_adapter.validate_json(line, by_name=False)` 解析；解析失败**把异常本身发进读流**而不是中断——上层会话据此回一条 JSON-RPC 错误。成功则包成 `SessionMessage` 发出。`anyio.ClosedResourceError` 时打一个 checkpoint 就退出。
- `stdout_writer`：从写流读，`message.model_dump_json(by_alias=True, exclude_unset=True)` 序列化，**追加换行**写出去并 `flush`。`by_alias=True` 与 `exclude_unset=True` 是两个必须的设置——前者保证线上字段名是协议名（`camelCase`），后者保证没有值的可选字段不出现在帧里。

两者在 `anyio.create_task_group()` 里启动，`yield` 出 (read_stream, write_stream)。`finally` 里**先恢复 stdout 再恢复 stdin**，各调一次 `restore_*`（可能为 `None`，因为跳过接管时它就是 `None`）。

注意 `finally` 的顺序：stdout 在前。这不是随意的——stdout 是协议写端，先恢复它能让退出后的日志立刻走回正常 stdout；而 stdin 恢复晚一点无害（改道目标是 devnull，读它不会产生垃圾）。

## 关键取舍

**源码事实：** 接管是**进程级独占**的：同一 fd 被第二个 `stdio_server()` 抢占时抛 `RuntimeError`。`_claims` 加线程锁是唯一权威。

**源码事实：** 契约把「恢复失败就保留 claim、拒绝后继者」定为规则，理由是「宁可拒绝，也不要喂一个已被改道的描述符」。

**源码事实：** 私有副本用 `F_DUPFD_CLOEXEC` 且下界为 3，既避开标准范围也不会被 exec 继承；文件对象用 `closefd=False` 打开，防止工作线程仍在读时 fd 被回收。

**源码事实：** `_UnownedTextWrapper.close()` 只 `detach()` 不关缓冲区，注释点名这是修复「GC 时关掉 `sys.stdout` 缓冲区、毁掉整个进程」的历史 bug。

**源码事实：** 接管是**尽力而为**的：`_is_backed_by_fd` 不成立、副本创建失败、`dup2` 失败时都退回「原地使用 `stream.buffer`」，此时靠 v1 的约定（不要 print）而不是靠改道。

**源码事实：** 所有失败路径都返回「原地缓冲区 + release」这一对，没有一条路径返回「已改道但无恢复」。

**架构推论：** 这套机制的本质是**用文件描述符做能力隔离**。在同一个进程里无法阻止第三方库调用 `print`，但可以改变 `print` 作用到的东西——把它从协议通道挪到 stderr。这是 Unix 进程模型给的一种极便宜的隔离：不需要子进程、不需要容器，一次 `dup2` 就够。

**架构推论：** 非对称的骗局目标是有讲究的。stdout 改道到 **stderr** 而不是 devnull，是为了让意外的输出**仍然可见**（进日志）而不是消失——调试价值与协议干净两者兼得。stdin 改道到 devnull 则必须彻底，因为「读到 EOF」是最安全的行为，而把 stdin 改道到任何「有内容」的东西都可能被误当成协议输入。

**架构推论：** 牺牲了「与 stdio 服务端同进程的其他代码可以正常使用 print」这一便利。这是必须的，但它带来一个隐性成本：**测试困难**。所以 `stdio_server` 留了 `stdin` / `stdout` 参数让调用方跳过接管——这不是给生产用的，是给测试用的逃生口。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/stdio.py` | `_claim_fd` | 接管单个标准 fd：登记、造副本、改道、返回私有流与恢复函数 |
| `src/mcp/server/stdio.py` | `_StreamClaim` | claim 记录，`private_fd` 的两种状态区分「未建副本」与「已建」 |
| `src/mcp/server/stdio.py` | `_open_stdin_diversion` / `_open_stdout_diversion` | 骗局目标：devnull（读）/ stderr 优先、devnull 兜底（写） |
| `src/mcp/server/stdio.py` | `_dup_above_std` | 造 ≥3 且 `CLOEXEC` 的私有副本，Windows 上校验不落标准范围 |
| `src/mcp/server/stdio.py` | `_restore_fd` | 把线 `dup2` 回去；返回值决定 claim 是否注销 |
| `src/mcp/server/stdio.py` | `_UnownedTextWrapper` | 只 `detach` 不关缓冲区的文本层，避免毁掉 `sys.stdout` |
| `src/mcp/server/stdio.py` | `_is_backed_by_fd` | 判断流是否真的背靠该 fd，非真则跳过接管 |
| `src/mcp/server/stdio.py` | `stdio_server` | 编排：接管 → 建流 → 两个后台任务 → 退出时恢复 |

## 参考

- [mcp-python-sdk 源码：server/stdio.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/stdio.py)
- [MCP 规范：stdio 传输](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports#stdio)
- [Python 文档：os.dup2](https://docs.python.org/3/library/os.html#os.dup2)
- [Python 文档：io.TextIOWrapper.detach](https://docs.python.org/3/library/io.html#io.IOBase.detach)

## 与主核 / AI 的关系

「用 fd 改道把不该混入的数据挪走」在主核里对应的是**标准文件描述符的进程内管理**：服务端进程启动后会把 stdin/stdout/stderr 重定向到日志或 devnull，避免写 fd 1 的东西污染协议或泄漏。两边用同一套原语（`dup`/`dup2` + 记录原描述符以便恢复），也都必须回答同一个问题：**什么时候能安全恢复**。这里的答案是「恢复失败就不注销 claim」，主核里对应的是「重定向失败就退出启动」——都是把不可恢复的中间态变成显式失败。

契约注释的写法值得单独学。它把四条不变量写在代码前面，然后让每条失败路径都对照着检查。这类「先声明不变量、再逐条保证」的做法在并发与资源管理里几乎是唯一可靠的方式——因为这类 bug 的特征是不可复现：只在特定时序或特定 GC 时显形。

`closefd=False` 那一行也值得记。它对应主核里「文件描述符不能被两个所有者同时关闭」这条纪律。Python 里更隐蔽：`os.fdopen` 默认会在文件对象被 GC 时关 fd，而 GC 时机不可控，所以「谁拥有这个 fd」必须在创建时就决定。

对 AI 工程的实际意义在于：stdio 传输是 MCP 最常用的本地集成方式（几乎所有桌面客户端都这么起服务端），而它最脆的地方恰恰不是协议实现，而是**进程的 stdio 被第三方代码污染**。这套实现的解法很彻底：不靠约定，直接改道。副作用是「服务期间不能正常用 print 调试」——所以它把 stdout 改道到 stderr 而不是丢掉，让调试信息仍然留在可见的地方。这是一个值得直接抄的细节。
