---
project: open-interpreter
feature: sandbox-manager
title: 沙箱管理器与平台选择
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/sandboxing/src/manager.rs
    symbols: [SandboxablePreference, get_platform_sandbox, SandboxCommand]
  - path: codex-rs/sandboxing/src/manager.rs
    symbols: [SandboxExecRequest, SandboxTransformError, with_managed_mitm_ca_readable_root]
---

# 沙箱管理器与平台选择

## 一句话职责

`SandboxManager` 是「这条命令要不要进沙箱、进哪个平台的沙箱、进去之后 argv 变成什么」的唯一决策点。它把一份与平台无关的权限档案，改写成一个带平台前缀的完整命令行。

## 机制是什么

C 组的四篇要回答的核心问题是「为什么需要四套平台实现」。这个模块给出的答案分两层。

**第一层是操作系统能力的不对称。** 三个平台提供了三种完全不同形状的限制原语：

| 平台 | 原语 | 形态 |
|---|---|---|
| macOS | Seatbelt（`sandbox-exec`） | 一份 SBPL 策略文本，声明式地 allow/deny 各类操作 |
| Linux | Landlock + bubblewrap | 内核 LSM 给进程装访问规则，外加用户命名空间重排文件系统视图 |
| Windows | 受限令牌 + 作业/提权后端 | 靠令牌的 SID 做访问检查，能力是「减去」而非「声明」 |

这三种原语连「表达限制」的语法都不一样，所以无法用一套代码实现，只能各写一份。

**第二层是「能不能做到」还要再分一次。** 即便在同一平台上，某些限制根本无法表达。Windows 那侧最能说明问题：受限令牌能减写权限，但它的 `WRITE_RESTRICTED` 令牌**不让 capability SID 的 deny-read ACE 参与读检查**——所以「允许写、但禁止读某个子目录」这种组合在非提权后端上根本做不到。源码的处理不是「尽力而为」，而是返回一条明确的拒绝。

所以这个管理器做的不是「尽量套上沙箱」，而是**「要么套上、要么明确说套不上」**。

## 怎么实现的

**`SandboxablePreference` 是一道三态前置闸门，与「能否提供实现」分开。**

```rust
match pref {
    SandboxablePreference::Forbid => false,
    SandboxablePreference::Require => true,
    SandboxablePreference::Auto => { /* 看权限档案本身是否要求 */ }
}
```

`should_sandbox` 的方法文档写得极清楚：「Returns whether the request needs a sandbox, independently of whether this host can provide a concrete sandbox implementation」。

这句话是整套设计的枢纽。它把问题拆成两个独立的问题：

1. **需要吗** —— 由权限档案 + 偏好决定，与平台无关。
2. **能提供吗** —— 由 `get_platform_sandbox` 决定，与需求无关。

拆开的好处是「我需要沙箱但这个平台给不了」能变成一个可诊断的状态，而不是悄悄降级成不套沙箱。`SandboxablePreference::Auto` 分支再看权限档案里的文件系统与网络策略——即「用户没明确要求，但档案本身写了限制」，那就当作需要。

**`get_platform_sandbox` 是三行的 `cfg!` 分派，但它的参数揭示了 Windows 的特殊地位。**

```rust
if cfg!(target_os = "macos") { Some(SandboxType::MacosSeatbelt) }
else if cfg!(target_os = "linux") { Some(SandboxType::LinuxSeccomp) }
else if cfg!(target_os = "windows") {
    if windows_sandbox_enabled { Some(SandboxType::WindowsRestrictedToken) } else { None }
} else { None }
```

三处值得注意：

- **只有 Windows 需要外部开关**。macOS 与 Linux 有沙箱就给，Windows 要显式启用（因为默认的受限令牌沙箱在早期会影响兼容性）。所以这个函数的签名里只有一个 `windows_sandbox_enabled: bool`，其余平台忽略它。
- **Linux 的类型叫 `LinuxSeccomp`，但实现是 Landlock + bwrap**。名字是历史遗留——两者都是「内核层的系统调用/文件访问限制」这一类，bwrap 负责文件系统视图、Landlock 负责访问规则的兜底。
- **未知平台返回 `None`**，即「没有实现」。这与「不需要」在返回值上是同一个 `None`，区别由前面的 `should_sandbox` 保住——这也正是把两件事拆开的价值。

**`select_initial` 把两道闸门串起来，并给 Windows 的 MXC 变体让路。**

```rust
if !self.should_sandbox(...) { return SandboxType::None; }
if cfg!(windows) && windows_sandbox_type == SandboxType::WindowsMxc { return SandboxType::WindowsMxc; }
get_platform_sandbox(windows_sandbox_type != SandboxType::None).unwrap_or(SandboxType::None)
```

注意第三行那个参数：`windows_sandbox_type != SandboxType::None`。也就是说「是否启用 Windows 沙箱」这个布尔量，是由调用方传入的 `windows_sandbox_type` 反推出来的——非 `None` 即视为启用。这让调用方只需给一个枚举，不必再单独维护一个布尔。

还有一行 `crate::windows_mxc::record_availability_once()`，带 `#[cfg(windows)]`。这是在选择时刻记一次「MXC 后端在本机是否可用」，供后续的遥测或诊断使用——选择本身不依赖它。

**`transform` 是主函数，它的输出是一个完整的 argv 而不是一个「已沙箱化」的标记。**

```rust
let mut argv = Vec::with_capacity(1 + command.args.len());
argv.push(os_string_to_command_component(command.program));
argv.extend(command.args);
```

然后按 `sandbox` 分派，每个分支返回三元组 `(argv, arg0_override, pending_sandboxed_request)`。三个平台的做法在形态上差别很大：

- **macOS**：把 `argv` 塞进 `create_seatbelt_command_args_with_profile`，得到策略参数，再在最前面加 `/usr/bin/sandbox-exec`。即 `sandbox-exec -p <policy> -D<key>=<value>... -- <原命令>`。
- **Linux**：把 `argv` 塞进 `create_linux_sandbox_command_args_for_permission_profile`，再把一个独立的 helper 可执行文件放到最前面，同时给出一个 `arg0` 覆盖值。
- **Windows**：`argv` 原文不动，只标记「需要沙箱」，实际的包装发生在 `transform_for_direct_spawn` 里。

这些差异不是风格问题，而是各平台原语的接口决定的：Seatbelt 是一个命令包装器，bwrap 也是命令包装器（但要自己的 helper 来做挂载与降权），Windows 的受限令牌需要在**创建进程时**设置令牌——所以无法通过 argv 表达，必须走 spawn 路径。

**`arg0` 这个字段是 Linux 侧的一个特别机制。** `linux_sandbox_arg0_override(exe)` 的返回值决定了被沙箱化的进程看到自己的 `argv[0]` 是什么：

```rust
if exe.file_name().and_then(|name| name.to_str()) == Some(CODEX_LINUX_SANDBOX_ARG0) {
    CODEX_LINUX_SANDBOX_ARG0.to_string()
} else {
    exe.to_string_lossy().to_string()
}
```

即「如果可执行文件已叫那个名字就用名字，否则用完整路径」。这个看起来很琐碎的分支其实是在处理一个真实场景：helper 既可能是单独安装的二进制（名字对得上），也可能是一个名字不同的打包产物（需要完整路径才能执行）。而 `arg0` 的用途是让 helper 在启动时识别自己是「被调用」而不是「被执行」——这是 Unix 里「同一二进制多用途」的经典手法（busybox 式 arg0 分派）。

**Linux 分支里有一道「先检查再动手」的前置门禁。**

```rust
ensure_linux_bubblewrap_is_supported(&pending.effective_permission_profile.file_system_sandbox_policy(), use_legacy_landlock, managed_network.is_some(), is_wsl1())?;
```

这一步在任何 argv 组装之前。它要确认的是「这台机器能不能真的用 bwrap」——容器里可能没有用户命名空间、WSL1 根本没有。失败就返回 `SandboxTransformError`，而不是组装出一个跑不起来的命令。

**错误类型把「准备失败」细分成六种，每一种对应一个平台或一类前置条件。** 从源码里出现过的变体看：

| 变体 | 含义 |
|---|---|
| `SeatbeltUnavailable` | 非 macOS 平台上请求了 Seatbelt |
| `SeatbeltPreparation` | macOS 策略准备失败（文件系统） |
| `MissingLinuxSandboxExecutable` | Linux 分支缺少 helper 路径 |
| `WindowsSandboxPreparation` | Windows 准备失败（含提权后端缺失） |
| `WindowsMxcPreparation` | MXC 后端不可用或缺少代理 |
| `EnvironmentNetworkProxy` | 代理环境准备失败 |

这些变体的价值在于**错误信息能指向具体平台的具体原因**。若压成一个 `SandboxError(String)`，用户只会看到「沙箱失败」，而这类失败的原因在三个平台上完全不同，需要完全不同的修补动作。

**`with_managed_mitm_ca_readable_root` 是一处「因为代理而放宽读权限」的窄口子。**

```rust
let Some(managed_mitm_ca_trust_bundle_path) = managed_mitm_ca_trust_bundle_path else {
    return permission_profile;
};
let file_system_sandbox_policy = file_system_sandbox_policy
    .with_additional_readable_roots(sandbox_policy_cwd, std::slice::from_ref(managed_mitm_ca_trust_bundle_path));
```

它做一件很具体的事：受管网络（MITM 代理）需要读 CA 信任包，而那个文件通常不在工作区内，所以要把它的路径加入可读根。三个设计点：

- **只有 `Some` 时才改档案**，`None` 时原样返回——不给「没有受管代理」的普通运行增加任何读权限。
- **用 `from_ref` 而不是 `clone`**，避免为一个元素复制整个 bundle 路径。
- **只有读，不涉及写**——`with_additional_readable_roots` 的语义决定了这一点。

这是「安全策略要为功能让出一格」的典型形态：让出的范围必须精确到一个文件，并且只在触发条件成立时让。

**`transform_for_direct_spawn` 是 Windows 的专用入口。** 它先判断是不是 `WindowsRestrictedToken`，是的话取 `codex_home` 再走专门路径，否则直接委托给 `transform`。那个专门路径把 `request.transform` 先跑一遍，再调 `wrap_windows_sandbox_exec_request_for_direct_spawn`。

这个分叉的存在理由写在 `SandboxDirectSpawnTransformRequest` 的文档里：「Direct-spawn callers will not run a later platform-specific launcher, so the returned command must encode any sandbox wrapper it needs」。即普通路径假定后面还有一层平台 launcher 会做包装，而直接 spawn 的调用方没有那一层，所以包装必须现在做进去。

**Windows 环境变量的白名单不在 argv 里，在环境里。**

```rust
#[cfg(target_os = "windows")]
const WINDOWS_SANDBOX_WRAPPER_SETUP_ENV_ALLOWLIST: &[&str] = &["USERNAME", "USERPROFILE", "SYSTEMROOT"];
```

注释说明了 `SYSTEMROOT` 的必要性：「ShellExecuteExW needs SystemRoot to elevate the setup helper」。也就是说提权 helper 靠 `ShellExecuteExW` 启动，而这个 API 需要 `SYSTEMROOT`。白名单只有三项，其余环境变量在提权那一刻被剥掉——这是「用最小环境执行提权动作」的实现。

## 关键取舍

**源码事实：** `should_sandbox`（需不需要）与 `get_platform_sandbox`（能不能提供）是两个独立的判断，前者与平台无关，后者与需求无关。

**源码事实：** 只有 Windows 需要外部布尔开关来启用沙箱，macOS 与 Linux 有实现就用；未知平台返回 `None`。

**源码事实：** `select_initial` 用一个枚举反推布尔（`windows_sandbox_type != SandboxType::None`），使调用方不必维护两套状态。

**源码事实：** `transform` 的输出是完整 argv，而三个平台的包装方式不同：macOS 前置 `sandbox-exec`、Linux 前置 helper 并给出 `arg0` 覆盖、Windows 原样保留 argv（包装延后到 spawn 路径）。

**源码事实：** Linux 分支在组装 argv 之前先调 `ensure_linux_bubblewrap_is_supported`，把「用不了」变成一次可诊断的失败而不是一条跑不起来的命令。

**源码事实：** 错误类型把准备失败细分成六个平台相关变体，而不是压成一个字符串。

**源码事实：** `with_managed_mitm_ca_readable_root` 只在有受管 MITM CA 时给它加一条可读根，`None` 时完全不动档案。

**架构推论：** 「需不需要」与「能不能」分开，是这类安全机制里最容易被做错的一处。合成一个布尔会带来两种坏结果：要么「需要但给不了」被静默降级成不套（最危险），要么「不需要但平台有实现」被强行套上（影响可用性且用户无法理解）。拆开之后，前者能成为显式错误，后者能成为 `None`。

**架构推论：** Windows 上那套「做不到就拒绝」的处理方式值得单记。它对「允许写但禁止读某个子目录」直接返回错误，理由写得很技术——受限令牌的读检查不参与 capability SID 的 deny ACE。这类「平台的限制表达能力不足」只能靠代码显式登记，无法靠测试发现（因为测试跑在那个平台上也可能「看起来没生效」）。

**架构推论：** 三个平台各写一份实现，看起来是重复，实际是**三种原语的接口差异**导致的必然结果。Seatbelt 是声明式策略文本，bwrap 是命名空间重排，Windows 是令牌 + 提权。把它们强行抽象成一套「沙箱策略 DSL」只会得到三种语言的并集，而每种语言在自己的平台上都表达不出原生能力。

**架构推论：** `arg0` 覆盖这个字段单独存在，说明「同一二进制多用途」这条 Unix 传统在这个 Rust 项目里仍然被沿用。它的好处是不用为「包装」和「执行」两个用途编译两个二进制；代价是启动逻辑里必须有一处「看自己叫什么名字」的分支。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/sandboxing/src/manager.rs` | `SandboxablePreference` | 三态偏好：自动 / 要求 / 禁止 |
| `codex-rs/sandboxing/src/manager.rs` | `get_platform_sandbox` | 按平台返回可用实现，未知名平台返回 `None` |
| `codex-rs/sandboxing/src/manager.rs` | `SandboxManager::should_sandbox` | 判断「需不需要」，与平台无关 |
| `codex-rs/sandboxing/src/manager.rs` | `SandboxManager::select_initial` | 串联两道闸门，并为 Windows MXC 让路 |
| `codex-rs/sandboxing/src/manager.rs` | `SandboxManager::transform` | 主改写：按平台产出完整 argv 与 `arg0` |
| `codex-rs/sandboxing/src/manager.rs` | `SandboxCommand` | 待改写命令的输入形状 |
| `codex-rs/sandboxing/src/manager.rs` | `SandboxExecRequest` | 改写结果：argv、环境、策略快照、`arg0` |
| `codex-rs/sandboxing/src/manager.rs` | `SandboxTransformError` | 六个平台相关变体的失败分类 |
| `codex-rs/sandboxing/src/manager.rs` | `with_managed_mitm_ca_readable_root` | 因受管代理而加一条可读根的窄口子 |
| `codex-rs/sandboxing/src/manager.rs` | `transform_for_direct_spawn` | Windows 直接 spawn 路径的专用包装 |
| `codex-rs/sandboxing/src/manager.rs` | `linux_sandbox_arg0_override` | helper 的 `argv[0]` 覆盖值 |

## 参考

- [open-interpreter 源码：sandboxing/src/manager.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/sandboxing/src/manager.rs)
- [Apple 文档：App Sandbox 与沙箱配置文件语言](https://developer.apple.com/documentation/security/app_sandbox)
- [Linux 内核文档：Landlock](https://docs.kernel.org/userspace-api/landlock.html)
- [Microsoft 文档：受限令牌](https://learn.microsoft.com/en-us/windows/win32/secauthz/restricted-tokens)

## 与主核 / AI 的关系

「把同一份抽象策略翻译成多种平台原语」与主核里的**存储后端抽象**是同一类问题，但结论不同。主核可以把「写一个页」抽象成统一接口，因为所有存储介质都支持这个动作；沙箱做不到，因为 macOS 的 Seatbelt 与 Linux 的 Landlock 限制的**是不同维度**，交集比并集小得多。

「可表达性不足就拒绝」这条与主核里**拒绝不支持的 DDL**一致：主核在遇到某个索引类型不支持的组合时会报错，而不是建一个「看起来像但行为不同」的索引。原因相同——**静默降级的后果比失败严重**。沙箱静默降级意味着用户以为自己在受保护的状态下运行不可信代码。

`with_managed_mitm_ca_readable_root` 那种「只在特定条件成立时放宽一格」的做法，与主核里**为特定操作临时提升权限**的做法同源：放宽的范围必须精确（一个文件、一个操作），且必须与触发条件绑定，不能变成全局配置。

对 AI 工程的实际意义在于：给「模型生成的代码」套沙箱，难点从来不在策略设计，而在于**跨平台一致性**。同一份策略在三个平台上保护力度不同，用户却只有一套配置。这份实现给出的三条经验可直接用：**把「需要」与「能做到」分开判断**（否则静默降级无法被发现）、**把平台差异登记成显式的错误类型**（让失败可诊断）、以及**宁可拒绝也不假装套上**。
