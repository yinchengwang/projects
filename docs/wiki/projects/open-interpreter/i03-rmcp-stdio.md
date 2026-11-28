---
project: open-interpreter
feature: rmcp-stdio
title: MCP 客户端 stdio 传输
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/rmcp-client/src/local_stdio_transport.rs
    symbols: [LocalStdioTransport, StdioTransport]
  - path: codex-rs/rmcp-client/src/bounded_stdio_transport.rs
    symbols: [BoundedStdioTransport, MAX_MCP_STDIO_LINE_BYTES]
---

# MCP 客户端 stdio 传输

## 一句话职责

给本地起的 MCP 服务器准备两套 stdio 传输：老协议沿用上游库的行框架，新协议换上自己实现的「单行 8MB 上限」版本——因为 stdio 的对端是**任意第三方程序**，它的输出长度不受本进程控制。

## 机制是什么

`LocalStdioTransport` 两字段：`child: Box<Child>`（子进程句柄）与 `transport: StdioTransport`。

`StdioTransport` 是一个**只有两变体的枚举**，每个变体注释都说明了适用场景：

- `Legacy(AsyncRwTransport<RoleClient, ChildStdout, ChildStdin>)` —— 「为使用 initialize 握手的服务器保留 rmcp 既有的框架」。
- `V20260728(BoundedStdioTransport)` —— 「在 2026-07-28 发现流程期间限制帧并跳过客户端不认识的消息」。

所以「用哪套传输」由 `McpProtocolMode` 决定，而两套传输被一个枚举并行装着，`Transport` trait 的四个方法（`send` / `receive` / `close` 与生命周期）都写成 match 双分支转发。

`BoundedStdioTransport` 四字段：`stdin: Arc<Mutex<Option<ChildStdin>>>`、`stdout: BufReader<ChildStdout>`、`pending_line: Vec<u8>`、`program_name: String`。

`MAX_MCP_STDIO_LINE_BYTES = 8 * 1024 * 1024`——注释说「与既有的执行器 stdio 传输和生产 MCP 上限一致」。

## 怎么实现的

**为什么 stdio 需要单独的行长上限。** 这一页的核心问题。HTTP 传输的响应长度受服务端框架与客户端库控制，双方都会设限；而 stdio 传输的对端是一个**任意子进程**：

- 它可能是第三方写的、可能是别的语言、可能行为异常。
- 它可能因为 bug 在 stdout 上打了一行无穷长的内容。
- 它可能把调试日志混进协议流，而那些日志一行可能几 MB。

若不加限制，`BufReader::fill_buf` 与逐行拼接会让 `pending_line` 无上界增长——一个行为异常的子进程能直接吃掉整个进程的内存。而这一层是客户端，它没法「要求对端改正」。

所以这里选择了「读的时候判长度、超了就关闭传输」这条硬策略：注释与代码都写明 `closing transport`，返回 `None` 结束流。

**长度判据是「含已有缓冲」的。** 实现里那句：

```rust
if content_len > MAX_MCP_STDIO_LINE_BYTES.saturating_sub(self.pending_line.len()) {
```

判的是「本次新增的字节数」是否超过「上限减去已攒的字节数」。所以它限制的是**一条完整行的总长**，而不是单次读取量。若只判本次读取量，一条被分成很多小块的超长行就能绕过——因为每块都不超限。

`saturating_sub` 而不是普通减法：当 `pending_line` 已超过上限时（理论上不该发生，但防御性地处理）减法不会下溢成巨大的数。

超限后的动作是 `self.pending_line.clear()` 再 `return None`——清掉已攒的数据并终止流。所以「超长」是一个**不可恢复的传输级错误**，处理方式是断开而非跳过。

**逐块读取与换行定位。** `receive_message` 的主循环：

1. `self.stdout.fill_buf().await` 取当前可用的块（`fill_buf` 不消费，只是借出缓冲）。
2. `bytes.is_empty()` —— EOF。若 `pending_line` 也空就返回 `None`（正常结束）；否则 `decode_pending_message()` 交出最后一条没有换行收尾的记录。
3. `memchr(b'\n', bytes)` 找块内第一个换行——**用 `memchr` 而不是 `iter().position()`**，因为这是热路径且 `memchr` 是 SIMD 优化的。
4. `content_len = newline.unwrap_or(bytes.len())`——没有换行就吃整块。
5. 长度检查（见上）。
6. `extend_from_slice(&bytes[..content_len])` 攒起来。
7. `consumed = content_len + usize::from(newline.is_some())` —— 有换行就多消费那个字节。
8. `self.stdout.consume(consumed)` 从缓冲里去掉已消费部分。
9. 若有换行且 `decode_pending_message()` 成功就返回；否则循环继续。

第 9 步那个 `&& let Some(message) = ...` 是一个细节：解码失败时 `decode_pending_message` 返回 `None`，此时循环**继续**而不是返回 `None`。所以「一条坏消息」被跳过，流仍可用。

这正是枚举变体注释里那句「跳过客户端不认识的消息」的实现——新协议的发现阶段会收到一些客户端不认识的 JSON-RPC 消息类型，`serde_json::from_slice::<RxJsonRpcMessage>` 会失败，此时跳过比断开合理。

**`decode_pending_message` 三件事。** 它先 `std::mem::take(&mut self.pending_line)` 把缓冲取走（留下空 Vec，省一次清空），再 `line.strip_suffix(b"\r").unwrap_or(&line)` 去掉可能的 CR（兼容 CRLF 输出的服务端），最后 `serde_json::from_slice` 解析。

失败时打一条 **debug** 级别（不是 warn）的日志并返回 `None`。级别选择有讲究：坏消息在不认识的协议版本下是常态，用 warn 会把日志刷满；debug 让它在需要排查时才出现。而超长与读失败用的是 warn——那两种是真的异常。

`strip_suffix` 那个 `unwrap_or(&line)` 是在借用上做文章：`strip_suffix` 返回 `Option<&[u8]>`，`unwrap_or` 借回原切片，所以后面可以直接 `from_slice`。若写成 `if let Some(...)` 就要多一层缩进。

**读失败也算终止。** `fill_buf` 返回 `Err` 时打 warn 并 `return None`。所以「读 stdout 出错」与「EOF」在这层看起来一样（都结束流）——因为 `Transport::receive` 的返回类型是 `Option<...>`，没有错误通道。上游 trait 的这个签名决定了「错误只能表现为结束」。

**发送侧要跨写操作持锁。** `send` 的实现把 `stdin` 克隆出来（`Arc::clone`）再在 async 块里：

```rust
let mut guard = stdin.lock().await;
let stdin = guard.as_mut().ok_or_else(|| io::Error::new(io::ErrorKind::BrokenPipe, "MCP stdin closed"))?;
stdin.write_all(&message).await?;
stdin.flush().await
```

三处值得看：

1. `Arc::clone(&self.stdin)` 是因为 `await_holding_invalid_type` 那条 `#[expect]` ——注释解释：「完整的 JSON-RPC 帧必须在写入期间持有 stdin 锁」。`send` 返回 `'static` future（trait 要求），而 `&mut self` 的引用活不到 `'static`，所以必须用 `Arc` 把句柄带进 future。

2. `guard.as_mut()` 的 `None` 分支返回 `BrokenPipe` 错误。所以 `close()` 之后再 `send` 会得到明确的错误，而不是 panic。

3. `write_all` 后必须 `flush`。stdio 的写是带缓冲的，不 flush 会让帧停在客户端缓冲里——对端永远等不到。这是「为什么 send 里要 flush 而 receive 不涉及」的答案。

**`close()` 的实现是「先丢弃 stdin 再等子进程」。** `BoundedStdioTransport::close` 只有一行：`self.stdin.lock().await.take()`——把 `Option<ChildStdin>` 取走，于是句柄被 drop，管道关闭。对端读到 EOF 后会自行退出。

而外层 `LocalStdioTransport::close` 做三件事：

1. 转发给内层 `transport.close().await`（两变体都转发，老协议那边是上游库的关闭）。
2. `tokio::time::timeout(Duration::from_secs(3), self.child.wait())` —— 最多等 3 秒。
3. 超时就 `self.child.kill().await`。

所以关闭策略是「先温柔（关管道等退出）、3 秒不退就强杀」。这个 3 秒的界限必要：有些 MCP 服务器在 EOF 后会卡住（例如在等某个网络请求）。若不设超时，关闭流程会永久挂起。

注意 `Ok(status) => status.map(|_| ())` ——即使子进程以非零状态退出也算关闭成功（`map` 丢弃状态值）。所以「子进程退出码非零」不构成关闭失败。

**两套传输共存，由模式决定。** `LocalStdioTransport::spawn` 做的是：

```rust
let mut child = command.spawn()?;
let stdin = child.stdin.take().ok_or_else(|| io::Error::other("MCP server stdin was not piped"))?;
let stdout = child.stdout.take().ok_or_else(|| io::Error::other("MCP server stdout was not piped"))?;
let stderr = child.stderr.take();
let transport = match protocol_mode { ... };
```

三个 `take()` 里两个「必须有」，`stderr` 是 `Option`。这两个 `ok_or_else` 的失败消息说明了一个前提：「pipe 配置错了」。若调用方没给子进程配 `Stdio::piped()`，`take()` 会返回 `None`——那是一个编程错误而非运行时错误，所以用 `io::Error::other`。

`stderr` 被单独返回（`(Self, Option<ChildStderr>)`）而不是交给传输层。所以错误输出走上层自己的处理路径（通常是收集日志），协议流只有 stdout/stdin。这是一个必要的隔离：MCP 服务器的 stderr 往往满是调试输出，混进协议流会直接破坏 JSON-RPC。

**`StdioTransport::Legacy` 用的是 `AsyncRwTransport::new(stdout, stdin)`。** 注意参数顺序是 `stdout` 在前、`stdin` 在后——对应「读」与「写」。而 `BoundedStdioTransport::new(stdin, stdout, program_name)` 是 `stdin` 在前。两个构造函数的参数顺序相反，这是两个不同 API 的既有约定，不是笔误。

`program_name` 只给了 bounded 版，用来在日志里标明「是哪个 MCP 服务器出的问题」。所以老协议那边的 warn 缺少程序名——因为上游库自己管理日志。

**为什么老协议不换新传输。** 枚举变体的注释给了答案：「为使用 initialize 握手的服务器保留 rmcp 既有的框架」。

新传输的「跳过不认识的消息」这个行为会与旧协议的握手语义冲突：旧协议里客户端收到不认识的消息通常意味着版本不匹配，应当报错而不是静默跳过。所以两套传输不是「新替代旧」，而是「按协议模式选一套」。

`LocalStdioTransport` 的模块 doc 把这个分层讲得更准：「进程创建是平台相关的；协议选择与关闭是共享的」。所以这个文件的职责是「共享的那部分」——两条不同的传输路径、一个统一的关闭策略、一个统一的子进程持有。

**`id()` 只有一个转发。** `pub(super) fn id(&self) -> Option<u32> { self.child.id() }` ——拿到子进程的 OS 进程 id。这通常用于日志或诊断（「这条警告来自 pid 1234 的 MCP 服务器」）。

## 关键取舍

**源码事实：** 协议模式决定用上游库的传输还是自己实现的有界传输；两套并存于一个枚举。

**源码事实：** `MAX_MCP_STDIO_LINE_BYTES` 是 8MB，注释明确对齐既有的执行器 stdio 传输与生产 MCP 上限。

**源码事实：** 长度判据是「本次新增 + 已攒」是否超过上限，不是单次读取量。

**源码事实：** 超长时清空缓冲并返回 `None`，即关闭传输。

**源码事实：** 单条消息解析失败只打 debug 并**继续**读下一条。

**源码事实：** 读失败（`fill_buf` 出错）打 warn 并返回 `None`，与 EOF 表现相同。

**源码事实：** `send` 跨写与 flush 持有 stdin 锁，并用 `Arc::clone` 让 future 满足 `'static`。

**源码事实：** 关闭是「关管道 → 等 3 秒 → 强杀」，且非零退出码不算失败。

**源码事实：** `stderr` 被交给调用方而不是传输层。

**架构推论：** 「对端不可控」是这一层需要自定义传输的唯一理由。任何与外部进程/外部输入交互的通道都需要一个「单条消息大小上限」，且这个上限必须按**累积**长度判，否则分块读取会让检查形同虚设。这是缓冲区溢出防御在流式协议上的对应形态：不是防越界写，而是防无界增长。

**架构推论：** 「坏消息跳过、超长消息断开」这个不对称处理很有讲究。单条坏消息是**局部**问题，跳过它能保住后续消息；超长是**结构性**问题（意味着对端行为异常或协议不匹配），继续读下去没有意义。判据可以总结成：错误是否只影响当前这一条？是就跳过，不是就断开。

**架构推论：** 关闭策略里的「3 秒后强杀」是必须的兜底。任何「等子进程自行退出」的设计都会遇到「它就是不退」的情况；而没有超时的等待会把整个关闭流程（进而整个进程退出）挂住。这条规则对所有管理子进程的代码都适用：先礼貌，再有界，最后强制。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/rmcp-client/src/local_stdio_transport.rs` | `LocalStdioTransport` | 子进程与传输的持有者，统一关闭策略 |
| `codex-rs/rmcp-client/src/local_stdio_transport.rs` | `StdioTransport` | 两套传输模式的枚举 |
| `codex-rs/rmcp-client/src/local_stdio_transport.rs` | `LocalStdioTransport.spawn` | 取三个管道、按模式选传输、分离 stderr |
| `codex-rs/rmcp-client/src/local_stdio_transport.rs` | `Transport::close` 实现 | 关管道、3 秒等待、超时强杀 |
| `codex-rs/rmcp-client/src/bounded_stdio_transport.rs` | `MAX_MCP_STDIO_LINE_BYTES` | 8MB 单行上限 |
| `codex-rs/rmcp-client/src/bounded_stdio_transport.rs` | `BoundedStdioTransport` | 有界行读取与持锁发送 |
| `codex-rs/rmcp-client/src/bounded_stdio_transport.rs` | `BoundedStdioTransport.receive_message` | 逐块读、memchr 找换行、累积判长 |
| `codex-rs/rmcp-client/src/bounded_stdio_transport.rs` | `BoundedStdioTransport.decode_pending_message` | 去 CR、解析、失败只记 debug |
| `codex-rs/rmcp-client/src/bounded_stdio_transport.rs` | `Transport::send` 实现 | Arc 句柄、跨写持锁、写后 flush |
| `codex-rs/rmcp-client/src/bounded_stdio_transport.rs` | `Transport::close` 实现 | 丢弃 stdin 触发对端 EOF |

## 参考

- [open-interpreter 源码：rmcp-client/src/bounded_stdio_transport.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/rmcp-client/src/bounded_stdio_transport.rs)
- [open-interpreter 源码：rmcp-client/src/local_stdio_transport.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/rmcp-client/src/local_stdio_transport.rs)
- [JSON-RPC 2.0 规范](https://www.jsonrpc.org/specification)

## 与主核 / AI 的关系

主核里最接近的是「外部进程协议解析的边界防御」：读客户端或工具的输出时必须有长度上限，且要按累积长度判而不是单次读取量。这一页的做法与主核里那些「先判长度再解析」的实现同构，连「超限即断开而不是跳过」的方向都一样——因为超长意味着协议已经不可信。

与 goose 的 i02（ACP 服务端）对照很有意思：goose 那边对**入站** `_meta.tags` 设了三道上限（16 条、128 字节键、1024 字节值），因为它站在服务端、面对不可信客户端；这里对**入站** stdout 设了一道 8MB 行上限，因为它站在客户端、面对不可控子进程。两者都是「我没有权力要求对端改正，只能自己设限并断开」的处境。

与 i02（MCP 服务端）的联系：那一页里 `run_codex_tool_session` 处理内部事件流，事件类型是编译期已知的枚举；这一页处理外部子进程的 JSON-RPC 流，消息类型是运行期才知道的。所以那边用穷举 match，这边用「解析失败就跳过」。差别不在严格度，而在**对端是否是同一份代码**。

对 AI 工程最实用的一条是「上限必须按累积长度判」。流式读取里最容易写错的检查就是「这一块多少字节」，因为攻击面（或 bug）恰恰在于「一条消息被切成很多块」。正确的判据永远是「已攒 + 本次」对比上限，并且要处理「上限减已攒」的下溢。

另一条是「局部错误跳过、结构错误断开」。这一页给坏 JSON 用 debug 级别并继续，给超长用 warn 级别并断开——两条路径的日志级别也不同。这个区分很实用：如果所有异常都用同一个级别并同样处理，日志里就分不清「这条消息有问题」与「这个连接有问题」，而这两种问题的处置方式完全不同。

最后一条是「stderr 必须与协议流分离」。用 stdio 作为协议通道时，子进程的 stderr 会满是调试输出；若与 stdout 混在一起，JSON-RPC 就再也解析不了。把 `stderr` 单独 `take()` 出来交给上层，是让「日志」与「协议」各归其位的唯一办法。这也解释了 i01 里那个 `with_writer(std::io::stderr)`——写端也必须在另一边。
