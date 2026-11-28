---
project: goose
feature: failure-hooks
title: 失败后命令与超时执行
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/agents/retry.rs
    symbols: [execute_success_checks_with_timeout, execute_on_failure_command_with_timeout]
  - path: crates/goose/src/agents/retry.rs
    symbols: [execute_shell_command, capture_tail, MAX_COMMAND_STDERR_BYTES]
---

# 失败后命令与超时执行

## 一句话职责

配方可以声明「成功校验命令」与「失败后命令」，这两个都是**用户提供的 shell 命令**，所以这一层负责用统一的超时执行它们、把输出裁到可控体量、并把结果翻译成两种截然不同的语义——校验命令的**退出码 0 即成功**，失败后命令的**任何非零即失败**。

## 机制是什么

这里的「失败」指**任务没做成**，不是「模型请求失败」。两条命令挂在 `RetryConfig` 上：

```
pub struct RetryConfig {
    pub max_retries: u32,
    pub checks: Vec<SuccessCheck>,        // 成功校验命令
    pub on_failure: Option<String>,       // 失败后命令（清理用）
    pub timeout_seconds: Option<u64>,     // 缩略：默认 300
    pub on_failure_timeout_seconds: Option<u64>,  // 默认 600
}
```

`SuccessCheck` 目前只有一种变体（Shell），且带 `#[serde(alias = "shell")]`——配方文件里可以写 `type: "shell"` 或小写别名：

```
pub enum SuccessCheck {
    #[serde(alias = "shell")]
    Shell { command: String },
}
```

两条命令的执行时机由 `RetryOperation` 决定：一个轮次正常结束（`ends_turn(messages)` 为真）之后先跑校验；校验失败且尚未超过 `max_retries` 才跑失败后命令，然后把对话重置回 kickoff 处、把尝试计数加一，重新开始一轮。

两个默认超时有明显的不对称：校验 300 秒，失败后 600 秒。这个不对称有实际理由——失败后命令常用于清理或收集诊断（可能包含较慢的操作），而校验命令应当快速给出答案。

## 怎么实现的

**`execute_shell_command` 是这三层里唯一的执行器，它的三条设计决定都很硬。**

```
pub(crate) async fn execute_shell_command(command: &str, timeout: Duration) -> Result<std::process::Output> {
    let future = async {
        let mut cmd = if cfg!(target_os = "windows") {
            let mut cmd = Command::new("cmd"); cmd.args(["/C", command]); ...
        } else {
            let mut cmd = Command::new("sh"); cmd.args(["-c", command]); ...
        };
        cmd.set_no_window();
        let mut child = cmd.stdout(Stdio::null()).stderr(Stdio::piped()).stdin(Stdio::null())
                        .kill_on_drop(true).spawn()?;
        ...
    };
    match tokio::time::timeout(timeout, future).await {
        Ok(result) => result,
        Err(_) => { let error_msg = format!("Shell command timed out after {:?}: {}", timeout, command);
                    warn!("{}", error_msg); Err(anyhow::anyhow!("{}", error_msg)) }
    }
}
```

1. **`stdout(Stdio::null())`**：标准输出被**丢弃到空设备**，不是 pipes 到内存。返回的 `Output.stdout` 永远是空 `Vec`——代码里也显式构造了 `stdout: Vec::new()`。这是一个重要决定：校验与失败后命令的语义只依赖退出码与 stderr，捕获 stdout 没有用处，而放任一个会打印大量内容的命令写满管道会造成死锁（写端阻塞等读端）。
2. **`stdin(Stdio::null())`**：命令不能等待输入。若继承 stdin，一个 `read` 就会把整个 agent 卡到超时。
3. **`kill_on_drop(true)`**：超时后 future 被丢弃，`child` 随之被丢弃，于是子进程被杀。没有这一条，超时只是「不再等它」，进程会变成孤儿继续跑——一个清理脚本超时之后还在后台删东西是最糟的结局。

`set_no_window()` 在 Windows 上抑制控制台窗口弹出——这是 GUI 应用里执行子进程的必备处理，否则每次校验都会在屏幕上闪一个黑框。

`cmd.env("GOOSE_TERMINAL", "1")` 与 `cmd.env("AGENT", "goose")` 是给脚本的环境标记：脚本可以据此判断「我是在 goose 里跑的」，从而改变输出格式或跳过交互提示。

**`capture_tail` 是这一页最有技巧的一段：只保留 stderr 的末尾 8 KB。**

```
const MAX_COMMAND_STDERR_BYTES: usize = 8 * 1024;

async fn capture_tail<R>(mut reader: R) -> std::io::Result<Vec<u8>> where R: AsyncRead + Unpin {
    let mut tail = Vec::with_capacity(MAX_COMMAND_STDERR_BYTES);
    let mut chunk = [0; 4096];
    loop {
        let count = reader.read(&mut chunk).await?;
        if count == 0 { return Ok(tail); }
        if count >= MAX_COMMAND_STDERR_BYTES {
            tail.clear();
            tail.extend_from_slice(&chunk[count - MAX_COMMAND_STDERR_BYTES..count]);
            continue;
        }
        let overflow = tail.len().saturating_add(count).saturating_sub(MAX_COMMAND_STDERR_BYTES);
        if overflow > 0 { tail.drain(..overflow); }
        tail.extend_from_slice(&chunk[..count]);
    }
}
```

它同时是一个**流式读取器**与一个**环形缓冲**：

- `count >= MAX` 的快速路径直接覆盖整个缓冲，避免了「追加再裁剪」的中间态。这个分支只在 4 KB 的 chunk 读满时才有意义——但 `read` 一次最多返回 4 KB，而 `MAX` 是 8 KB，所以这条分支**实际上永远不会触发**。它不是死代码而是防御：如果将来把 chunk 调大或把 MAX 调小，这段仍然正确。
- 常规路径先算溢出量再用 `drain(..overflow)` 从头部裁掉。`saturating_add` / `saturating_sub` 避免了极端情况下的溢出。

**为什么保留尾部而不是头部？** 因为编译错误、断言失败、异常栈的结论都在**最后几行**。有一个测试正好验证这一点：

```
let command = "i=0; while [ $i -lt 10000 ]; do printf x >&2; i=$((i + 1)); done; printf diagnostic-tail >&2; exit 1";
assert_eq!(output.stderr.len(), MAX_COMMAND_STDERR_BYTES);
assert!(output.stderr.ends_with(b"diagnostic-tail"));
```

一万个 `x` 之后那句 `diagnostic-tail` 被保留下来。另一个测试验证小输出原样返回（`test_capture_tail_preserves_small_diagnostic`），用 `tokio::io::duplex` 造了一对内存流。

`capture_tail` 与 `child.wait()` 是**并发**进行的：

```
let (status, stderr) = tokio::try_join!(child.wait(), capture_tail(stderr))?;
```

`try_join!` 而不是「先 wait 再读」——如果先 wait，一个写满管道缓冲的命令会在 wait 期间阻塞（它想写但没人读），`wait` 永远不返回，超时也永远不会触发。这个顺序问题是子进程编程里最经典的死锁，而这里用一行 `try_join!` 解决了。

**`execute_success_checks_with_timeout` 是「全过才算过」。**

```
pub async fn execute_success_checks_with_timeout(checks: &[SuccessCheck], timeout: Duration) -> Result<bool> {
    for check in checks {
        match check {
            SuccessCheck::Shell { command } => {
                let result = execute_shell_command(command, timeout).await?;
                if !result.status.success() {
                    warn!("Success check failed: command '{}' exited with status {}, stderr: {}", ...);
                    return Ok(false);
                }
                info!("Success check passed: command '{}' completed successfully", command);
            }
        }
    }
    Ok(true)
}
```

三条语义要点：

- **空 `checks` 返回 `true`**——「没有任何校验」即视为成功。这很重要，因为配方通常不声明校验，而一个默认失败的行为会让所有配方都跑不通。
- **遇到第一个失败就 `return Ok(false)`**，短路，不打后续命令。
- **每个命令用同一个 `timeout`**，而不是共享一个总预算。三个命令各 300 秒意味着最坏 900 秒——这是一个明确的取舍：单命令超时是「防卡死」，不是「总时长限制」。
- **`Result<bool>` 里的 `Err` 与 `Ok(false)` 不同**：`Err` 是执行器层面的失败（超时、spawn 失败），`Ok(false)` 是命令正常跑完但退出非零。调用方对两者的处理也不同（见下）。

调用点：

```
let success = execute_success_checks_with_timeout(&retry_config.checks, retry_timeout).await;
let success = match success {
    Ok(success) => success,
    Err(error) => { let message = emit.message(retry_error(&error.to_string())); return applied([message.into()]); }
};
if success {
    if messages.last().is_some_and(|message| message.content.is_empty()) { return yielded(); }
    return not_applicable();
}
```

`Err` 分支把执行失败转成一条**对话里的错误消息**（`Retry logic encountered an error: …`）并应用它——也就是说一次校验超时不会静默重试，而是变成用户可见的一条错误。而 `Ok(false)` 则继续走重试路径。

`Ok(true)` 之后还有一层判断：如果最后一条消息**内容为空**（只有工具调用、没有文本），则 `yielded()` 而不是 `not_applicable()`。也就是「校验通过但模型没说话」这一情形需要再跑一轮让它说话。

**`execute_on_failure_command_with_timeout` 的错误区分把「超时」单独拎出来。**

```
let output = match execute_shell_command(command, timeout).await {
    Ok(output) => output,
    Err(e) => {
        if e.to_string().contains("timed out") {
            let error_msg = format!("On_failure command timed out after {:?}: {}", timeout, command);
            warn!("{}", error_msg); return Err(anyhow::anyhow!(error_msg));
        } else { warn!("On_failure command execution error: {}", e); return Err(e); }
    }
};
```

它通过**字符串包含 `"timed out"`** 来识别超时。这个做法确实脆弱（依赖上游错误消息措辞），但源码里就是这样的，值得如实记录：`execute_shell_command` 在超时分支里构造的正是 `"Shell command timed out after {:?}"`，所以这条判断与上游是配套的。唯一的差别是把它重写成「On_failure command timed out」——把「哪个命令超时」这个上下文补全了。

非零退出则直接算失败：

```
if !output.status.success() {
    let error_msg = format!("On_failure command failed: command '{}' exited with status {}, stderr: {}", command, output.status,
                            String::from_utf8_lossy(&output.stderr));
    warn!("{}", error_msg); return Err(anyhow::anyhow!(error_msg));
}
```

与校验命令形成对照：**同一个退出码，两条命令的语义相反**。校验命令 `exit 1` = 任务没成（应当重试）；失败后命令 `exit 1` = 清理没做成（不重试，直接把这条命令的错误报出来）。这个反转是整页最需要记住的一点：它不是设计上的疏漏，而是两种命令的用途完全不同。

**调用侧的次序与记账。**

```
let attempts = self.attempts(messages);
if attempts >= retry_config.max_retries { ... "Maximum retry attempts ({}) exceeded …" ; return applied([message.into()]); }
if let Some(command) = &retry_config.on_failure {
    let timeout = retry_config.on_failure_timeout_seconds.map(Duration::from_secs).unwrap_or(self.on_failure_timeout);
    if let Err(error) = execute_on_failure_command_with_timeout(command, timeout).await {
        let message = emit.message(retry_error(&error.to_string())); return applied([message.into()]);
    }
}
let mut reset = Self::reset_conversation(conversation)?;
if let Some(kickoff) = reset.messages_mut().last_mut() {
    self.set_message_meta(kickoff, ATTEMPTS, serde_json::json!(attempts + 1));
}
applied([reset.into()])
```

顺序是先判上限、再跑失败后命令、最后重置对话。失败后命令的错误**中止重试**（`return applied(...)`）——一个清理失败的命令不该被忽略着继续跑下一轮，因为环境可能已被它弄坏了一半。

尝试计数放在**kickoff 消息的 metadata** 里，注释给了理由：

> 尝试次数记在 kickoff 消息上，因为一次重试会把对话替换成直到并包含它的全部内容——它是唯一存活过自己所属那次尝试的消息。

`reset_conversation` 的实现印证了这一点：

```
let messages = messages_since_kickoff(conversation)?;
let kickoff = conversation.len() - messages.len();
Ok(Conversation::new_unvalidated(conversation.messages()[..=kickoff].to_vec()))
```

切到「kickoff 及其之前」，于是计数写在 kickoff 上就存下来了。这也解释了为什么校验通过后要判 `messages.last()` 是否为空——重置后的对话里最后一条就是 kickoff。

**超时的两层来源。**

`RetryOperation` 持有的两个 `Duration` 是**兜底值**，来自 `TurnSettings`：

```
retry_timeout: std::time::Duration::from_secs(config.get_param::<u64>("GOOSE_RECIPE_RETRY_TIMEOUT_SECONDS").unwrap_or(DEFAULT_RETRY_TIMEOUT_SECONDS)),
on_failure_timeout: std::time::Duration::from_secs(config.get_param::<u64>("GOOSE_RECIPE_ON_FAILURE_TIMEOUT_SECONDS").unwrap_or(DEFAULT_ON_FAILURE_TIMEOUT_SECONDS)),
```

而 `DEFAULT_RETRY_TIMEOUT_SECONDS = 300`、`DEFAULT_ON_FAILURE_TIMEOUT_SECONDS = 600`。实际取值优先级是：**配方里的 `timeout_seconds` / `on_failure_timeout_seconds` → 全局配置项 → 编译期常量**。三层，与 a02 页里 provider 名与 model config 的回退结构同型。

`RetryConfig::validate()` 强制三者：

```
if self.max_retries == 0 { return Err("max_retries must be greater than 0".to_string()); }
if let Some(timeout) = self.timeout_seconds { if timeout == 0 { ... } }
if let Some(on_failure_timeout) = self.on_failure_timeout_seconds { if on_failure_timeout == 0 { ... } }
```

`Some(0)` 被拒绝，而 `None` 被允许（表示用默认）。这一条很关键：零超时会让 `tokio::time::timeout(Duration::ZERO)` 立即触发，命令永远不会真的跑——一处「看起来配了但实际禁用」的静默故障。

## 关键取舍

**源码事实：** `stdout` 被定向到空设备且返回的 `Output.stdout` 恒为空；`stdin` 也置空。语义只依赖退出码与 stderr。

**源码事实：** `kill_on_drop(true)` 保证超时后子进程被杀，而不是变成孤儿继续执行。

**源码事实：** `child.wait()` 与 `capture_tail(stderr)` 用 `try_join!` 并发进行，避免「管道写满 → 子进程阻塞 → wait 永不返回」的死锁。

**源码事实：** stderr 只保留末尾 `MAX_COMMAND_STDERR_BYTES = 8 * 1024`，因为诊断结论在最后几行；有测试用一万个字符 + 尾部标记验证。

**源码事实：** 校验命令与失败后命令对**同一个退出码的解释相反**：校验的 `exit 1` 意味「任务没成」，失败后命令的 `exit 1` 意味「清理失败」。

**源码事实：** 校验命令短路（首个失败即返回 `false`），空 `checks` 返回 `true`，每个命令各自享有完整超时而不是共享总预算。

**源码事实：** 执行器层面的失败（`Err`：超时/spawn 失败）与命令非零退出（`Ok(false)`）被分开：前者变成可见的对话错误，后者进入重试路径。

**源码事实：** 失败后命令的失败会**中止重试**；超时的识别靠错误消息里含 `"timed out"`，与上游构造的文案配套。

**源码事实：** 尝试次数记在 kickoff 消息的 metadata 上，因为重试会把对话截断到「kickoff 及其之前」，那是唯一存活的锚点。

**源码事实：** 超时取值三层：配方 → 全局配置（`GOOSE_RECIPE_RETRY_TIMEOUT_SECONDS` / `GOOSE_RECIPE_ON_FAILURE_TIMEOUT_SECONDS`）→ 常量（300 / 600）。`validate()` 拒绝 `Some(0)` 但允许 `None`。

**架构推论：** 「只保留 stderr 尾部」与「丢弃 stdout」合起来是一条体量控制策略：外部命令的输出是**不可信体量**，而框架的内存不该被它支配。代价是若脚本把结论写到 stdout，框架看不到——两种命令的文档因此必须写明「用退出码与 stderr 表达结论」。

**架构推论：** 同一个退出码在两条命令上语义相反，这看似危险，但它反映的是两种命令的**目的不同**：校验命令在回答「做成了吗」（非零 = 没做成），失败后命令在回答「清理成功了吗」（非零 = 出问题了）。把它们统一成一种语义反而会出错。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/agents/retry.rs` | `execute_success_checks_with_timeout` | 依次跑校验命令，首个失败即返回 false，空列表为 true |
| `crates/goose/src/agents/retry.rs` | `execute_on_failure_command_with_timeout` | 跑失败后命令，非零或超时都算失败并中止重试 |
| `crates/goose/src/agents/retry.rs` | `execute_shell_command` | 唯一执行器：跨平台 shell、kill-on-drop、stdout 丢弃、超时 |
| `crates/goose/src/agents/retry.rs` | `capture_tail` | 流式读取并只保留 stderr 末尾 8 KB |
| `crates/goose/src/agents/retry.rs` | `MAX_COMMAND_STDERR_BYTES` | stderr 保留上限，也是缓冲容量 |
| `crates/goose/src/agents/types.rs` | `RetryConfig` | 两条命令的声明、两个超时与 `max_retries` |
| `crates/goose/src/agents/types.rs` | `DEFAULT_RETRY_TIMEOUT_SECONDS` / `DEFAULT_ON_FAILURE_TIMEOUT_SECONDS` | 300 / 600 的编译期兜底值 |
| `crates/goose/src/agents/state_machine/ops_retry.rs` | `RetryOperation::run` | 校验 → 上限判断 → 失败后命令 → 重置对话与计数 |

## 参考

- [goose 源码：agents/retry.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/retry.rs)
- [goose 文档：配方的成功校验与重试](https://block.github.io/goose/docs/guides/recipes/)
- [tokio 文档：`process::Command` 与 `kill_on_drop`](https://docs.rs/tokio/latest/tokio/process/struct.Command.html)
- [Rust 标准库：`std::process::ExitStatus`](https://doc.rust-lang.org/std/process/struct.ExitStatus.html)

## 与主核 / AI 的关系

「执行外部命令并用超时兜住」与主核里「调用外部程序处理数据文件」是同一类问题：外部程序的行为不可信、输出量不可控、可能挂住。主核通常的做法是设超时 + 限制输出，这里把这两件事做到了位，而且额外解决了管道死锁这个容易漏掉的坑。

对 AI 工程的实际意义有三条。

第一，**`wait` 与读管道必须并发**。这是子进程编程里最常见的死锁：父进程等子进程退出，子进程等父进程读完它写的输出。`tokio::try_join!(child.wait(), capture_tail(stderr))` 用一个表达式解决了它，而「先 wait 再 read」的写法在很多代码库里仍然存在。

第二，**外部命令的输出量必须被框架约束**。`stdout` 直接丢空设备、`stderr` 只留 8 KB，这两条让「脚本不小心打印了 2 GB」变成无事故。而保留**尾部**是一个有依据的选择：错误结论总在最后。

第三，**同一个退出码在不同命令上可以有相反含义**。校验命令回答「成功了吗」，失败后命令回答「清理成功了吗」。把两者统一成「非零即错误」会让校验永远失败；统一成「非零即正常」会让清理失败被静默吞掉。识破这一点需要看调用侧的用法，只看 `execute_shell_command` 是看不出来的。
