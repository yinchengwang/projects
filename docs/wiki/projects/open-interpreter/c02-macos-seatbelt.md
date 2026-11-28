---
project: open-interpreter
feature: macos-seatbelt
title: macOS Seatbelt 沙箱
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/sandboxing/src/seatbelt.rs
    symbols: [MacosSeatbeltProfile, MACOS_SEATBELT_BASE_POLICY, MACOS_SEATBELT_NETWORK_POLICY]
  - path: codex-rs/sandboxing/src/seatbelt.rs
    symbols: [is_loopback_host, proxy_loopback_ports_from_env, SeatbeltPreparationError]
---

# macOS Seatbelt 沙箱

## 一句话职责

把一份权限档案翻译成一段 SBPL（Sandbox Profile Language）策略文本，拼成 `sandbox-exec -p <策略> -D<参数>... -- <命令>`——macOS 上唯一能做「按路径、按操作类型」细粒度限制的用户态手段。

## 机制是什么

Seatbelt 是 Apple 的沙箱框架，通过 `/usr/bin/sandbox-exec` 喂一份策略文件给内核。策略是**声明式**的：写一堆 `(allow ...)` 与 `(deny ...)` 表达式，每条针对某个操作类别（`file-read*`、`file-write*`、`network-outbound`、`mach-lookup`、`sysctl-read`…）加一组匹配条件。

这个模块的形态完全由这个原语决定：它不做「设置限制」，而是**拼字符串**——把权限档案里的路径集合渲染成 `(allow file-read* (subpath "/x"))` 这样的表达式，再拼成一份完整策略。

所以它的复杂度集中在四件事：

| 问题 | 处理方式 |
|---|---|
| 从哪起手 | 四份 `.sbpl` 文件 `include_str!` 进二进制，作为模板与基线 |
| 路径怎么写进策略 | `-D<key>=<path>` 参数化，策略里用 `(param "key")` 引用 |
| 代理怎么放行 | 从环境变量里解析回环端口，为每个端口写一条 `network-outbound` |
| 拼装顺序 | 后写的表达式覆盖先写的，所以顺序即优先级 |

第三点用一个具体的例子说清为什么需要它：受管代理运行时，被沙箱化的进程只能连本地代理端口，不能连外网。而「本地代理端口」是运行时才知道的（写在 `HTTP_PROXY` 之类的环境变量里），所以必须在拼策略时**从环境里读出来**再写进去。

## 怎么实现的

**四份模板常量，全部编译期嵌入。**

```rust
const MACOS_SEATBELT_BASE_POLICY: &str = include_str!("seatbelt_base_policy.sbpl");
const MACOS_SEATBELT_NETWORK_POLICY: &str = include_str!("seatbelt_network_policy.sbpl");
const MACOS_SEATBELT_PREFERENCES_POLICY: &str = include_str!("seatbelt_preferences_policy.sbpl");
const MACOS_RESTRICTED_READ_ONLY_PLATFORM_DEFAULTS: &str = include_str!("seatbelt_read_only_platform_defaults.sbpl");
```

用 `include_str!` 而不是运行时读文件，有两个直接后果：二进制自带策略、不需要安装时随附数据文件；以及策略文本改动必须重新编译（无法被外部替换）。后一点对安全机制很重要——**策略文件不该是运行时可被篡改的资产**。

基线那份只有 116 行，第一句就是 `(deny default)`。注释写明它是「inspired by Chrome's sandbox policy」并给了三个 Chromium 源码链接。这个出处说明它不是凭经验写的，而是从一份被大量攻击面验证过的策略移植来的。

`(deny default)` 是整套设计的语义基础：**默认全拒，只开明确列出的**。之后的每一条 `(allow ...)` 都是在缩小拒绝面。基线里那些看起来琐碎的条目，每一条都有具体用途注释：

- `(allow process-exec)` / `(allow process-fork)`：注释「child processes inherit the policy of their parent」——即沙箱是继承的，不需要为子进程重写策略。
- `sysctl-read` 那一大串（`hw.ncpu`、`hw.memsize`、`kern.osversion`…）：让程序能探测硬件与系统信息。其中 `kern.sysv.semmns` 的注释直接点名「Python's ProcessPoolExecutor queries this through `sysconf(_SC_SEM_NSEMS_MAX)`」——**Python 多进程要读它，否则起不来**。
- `(allow ipc-posix-sem)`：注释「Needed for python multiprocessing on MacOS for the SemLock」。
- `ipc-posix-shm-*` 加 `(ipc-posix-name-regex #"^/__KMP_REGISTERED_LIB_[0-9]+$")`：注释「Needed for PyTorch/libomp on macOS to register OpenMP runtimes」。
- `pseudo-tty` 与 `/dev/ptmx`、`/dev/ttys*`：注释「allow openpty()」，并额外补了一条「PTYs created before entering seatbelt may lack the extension; allow ioctl on those slave ttys so interactive shells detect a TTY and remain functional」。

这一串条目合起来说明一件事：**基线策略不是「最小权限」，而是「最小权限 + 让常见工具链能跑」**。若不放开 `semmns`，Python 的进程池直接失败；若不放开 PTY 的 ioctl，交互式 shell 会以为自己没有终端。这些不是在放宽安全，而是在补齐「程序正常启动」的必要条件。

**网络策略是独立一份，且只在特定条件下才拼进来。** `seatbelt_network_policy.sbpl` 只有 31 行，允许三类：`AF_SYSTEM` 域的一种 socket（`(socket-domain AF_SYSTEM)` + `(socket-protocol 2)`）、若干 `mach-lookup` 全局名（`com.apple.SecurityServer`、`com.apple.trustd.agent`、`com.apple.SystemConfiguration.DNSConfiguration` 等，注释分别说明是「TLS 证书信息」「读网络配置」）、以及 `net.routetable` 的 `sysctl`。

**代理放行：从环境变量解析出回环端口。** `proxy_loopback_ports_from_env` 是这一页最实用的机制：

```rust
for key in PROXY_URL_ENV_KEYS {
    let Some(proxy_url) = proxy_url_env_value(env, key) else { continue; };
    let candidate = if trimmed.contains("://") { trimmed.to_string() } else { format!("http://{trimmed}") };
    let Ok(parsed) = Url::parse(&candidate) else { continue; };
    let Some(host) = parsed.host_str() else { continue; };
    if !is_loopback_host(host) { continue; }
    let port = parsed.port().unwrap_or_else(|| proxy_scheme_default_port(scheme.as_str()));
    ports.insert(port);
}
```

逐处读：

- **`contains("://")` 的分支**：很多环境里代理写成 `127.0.0.1:8080` 而不带 scheme，直接喂 `Url::parse` 会失败。所以缺 scheme 时补一个 `http://` 再解析。
- **`is_loopback_host`** 只认三种：`localhost`（大小写不敏感）、`127.0.0.1`、`::1`。这里刻意**不认** `0.0.0.0`、也不认局域网地址——因为沙箱放行的意思是「这个进程可以连到这台机器上的代理」，不是「可以连任何主机」。
- **`proxy_scheme_default_port`** 补默认端口：`https` → 443、socks 家族 → 1080、其余 → 80。这是为了让「只写了 host 没写端口」的代理配置也能被正确解析。
- **`BTreeSet`** 收集端口，天然去重且有序——**输出确定**，这对策略文本的可测试性很重要（同样的环境必须产生同样的策略文本，否则快照测试没法做）。

**`dynamic_network_policy_for_network` 是策略拼装的决策核心，它的分支顺序就是安全语义。**

```
should_use_restricted_network_policy =
    !proxy.ports.is_empty() || proxy.has_proxy_config
    || enforce_managed_network
    || (!network_policy.is_enabled() && has_some_unix_socket_access)
```

只要满足任一条就走「受限网络策略」分支：

1. 若 `allow_local_binding`：放行本地绑定、回环入站、回环出站（三条 `localhost:*` 的规则）。
2. 若 `allow_local_binding` 且已知代理端口：额外放行 `*:53` 的 DNS 查询。注释写「allow DNS lookups while application traffic remains proxy-routed」——即 DNS 可以直连，但业务流量仍走代理。
3. 为每个代理端口写一条 `(allow network-outbound (remote ip "localhost:{port}"))`。
4. 追加 Unix domain socket 策略（若配置了）。

然后 `format!("{policy}{MACOS_SEATBELT_NETWORK_POLICY}")`——把动态部分拼在网络模板**之前**。

这个顺序是刻意的：模板里的规则是「本地平台服务」的白名单，放在后面意味着它们**覆盖**前面的动态规则？不是——SBPL 的规则是叠加的，allow 与 allow 之间没有覆盖关系。真正需要后置的是 `deny`。这一点在下面「拼装顺序」那一段会看到。

**三条「fail closed」分支是这一页最值得记的安全设计。**

```rust
if proxy.has_proxy_config {
    // Proxy configuration is present but we could not infer any valid loopback endpoints.
    // Fail closed to avoid silently widening network access in proxy-enforced sessions.
    return String::new();
}
if enforce_managed_network {
    // Managed network requirements are active but no usable proxy endpoints are available.
    return String::new();
}
```

即：**配置了代理但解析不出回环端口时，返回空字符串（零网络权限），而不是退回「全放行」**。

这个选择的方向很关键。天真的写法是「解析不出来就当作没有代理，给全网络」，因为那能让程序跑起来。但那样做的后果是：一个本该只走代理的会话突然拿到了直连外网的能力——**代理强制可言被绕过**。返回空策略则意味着程序连不上任何东西，会立刻失败。

第三条 fail-closed 是「`network_policy` 未启用但配了 Unix socket」的镜像情形：不给网络但给 socket 访问时，也必须走受限分支，否则会得到一个「没有网络规则但有 socket 规则」的半成品策略。

**只有 `network_policy.is_enabled()` 且没有代理配置时，才给全网络。**

```rust
let mut policy = String::from("(allow network-outbound)\n(allow network-inbound)\n");
```

注意这两条是**无条件的 allow**（不带任何匹配条件），即放开全部出站与入站。注释写「No proxy env is configured: retain the existing full-network behavior」——「retain the existing」说明这是对既有行为的有意保留，而非新决定的。

**拼装顺序在最后的 `create_seatbelt_command_args_with_profile` 里。**

```rust
let mut policy_sections = vec![
    MACOS_SEATBELT_BASE_POLICY.to_string(),
    file_read_policy,
    file_write_policy,
    network_policy,
];
```

五段之后还有条件追加与一批**刻意放在最后**的 deny：

- `MACOS_SEATBELT_PREFERENCES_POLICY`——只在有全盘读权限时加。
- `MACOS_RESTRICTED_READ_ONLY_PLATFORM_DEFAULTS` 与 `(allow file-read* (subpath "/Applications"))`——`include_platform_defaults` 时加，且后者只在 `Process` profile 下加。
- **守护进程 socket 保护策略**——只在**没有**全盘写权限时加，注释写得很重：「Network grants and Unix-socket allowlists must never reopen the privileged app-server RPC transport to filesystem-restricted commands」。
- `(deny mach-lookup (xpc-service-name-prefix ""))`——把整个 XPC 名字空间关掉。
- `deny_read_policy`——由 `build_seatbelt_unreadable_glob_policy` 生成。
- **一批 `(deny file-write-unlink (require-all (vnode-type DIRECTORY) (literal (param "key"))))`**，注释解释了为什么必须放在最后：「Renaming an allowed ancestor relocates its protected descendants past their pathname carveouts. Keep these denies last so no broader allowance can reopen the unlink operation used by rename」。
- **`(deny system-fcntl (fcntl-command 80 110))`**，注释：「These fcntls mutate files through read-only descriptors, bypassing `file-write*` and `file-ioctl`. Even deny-default needs this explicit deny.」（`F_MAKECOMPRESSED = 80`、`F_TRANSFEREXTENTS = 110`）。

后面这两条是这一页技术含量最高的部分，值得展开：

1. **rename 会绕过路径限制**。策略是按路径写的（`(subpath "/work")`）。若允许重命名一个祖先目录，被保护的子路径会跟着搬走，于是它的路径变了、原来的 carveout 就管不到它了。所以要在所有 allow 之后补一条「目录的 unlink 操作禁止」，且必须放在最后——SBPL 里 allow 与 deny 的**求值顺序**决定了后写的能拦住先写的。
2. **fcntl 能在只读描述符上改文件**。`F_MAKECOMPRESSED`（在 macOS 上会给文件打压缩标记并可能重写数据）、`F_TRANSFEREXTENTS`（把另一个文件的数据块转移进来）都是「通过一个只读 fd 产生写效果」的操作。它们不走 `file-write*`，所以必须单独 deny——注释甚至说「Even deny-default needs this explicit deny」。

这两条共同揭示一个模式：**在声明式沙箱里，必须逐个堵住「能产生副作用但不属于常规操作类别」的系统调用**。这不是理论完备性，而是逐个发现、逐个补上的过程。

**`MacosSeatbeltProfile` 只有两个变体，代表两种用途。**

```rust
pub(crate) enum MacosSeatbeltProfile { #[default] Process, FileSystemHelper }
```

`Process` 是给普通命令用的；`FileSystemHelper` 是「narrower runtime profile」——注释在 `SandboxManager::for_file_system_helpers` 里说的。差别体现在两处：`FileSystemHelper` 不加 `/Applications` 的读权限、且只有 `Process` 才用完整的平台默认读根。也就是说 helper 拿到的权限比普通命令更窄。

**`MACOS_PATH_TO_SEATBELT_EXECUTABLE` 是一个绝对路径常量，注释直接说明了理由：**

> When working with `sandbox-exec`, only consider `sandbox-exec` in `/usr/bin` to defend against an attacker trying to inject a malicious version on the PATH. If `/usr/bin/sandbox-exec` has been tampered with, then the attacker already has root access.

即**绝不用 `PATH` 查找**。这是安全工具启动时的一条通用纪律：把一个用于施加限制的工具从可被用户控制的查找路径上取出来。若攻击者能替换 `/usr/bin/sandbox-exec`，那他已经是 root 了——这句补充让这条纪律的边界很清楚。

**路径参数化：`-D<key>=<value>` 与 `(param "key")`。** 拼装完策略文本后，尾部是这样组装的：

```rust
let mut seatbelt_args: Vec<String> = vec!["-p".to_string(), full_policy];
let definition_args = dir_params.into_iter()
    .map(|(key, value)| format!("-D{key}={value}", value = value.to_string_lossy()));
seatbelt_args.extend(definition_args);
seatbelt_args.push("--".to_string());
seatbelt_args.extend(command);
```

用 `-D` 而不是把路径直接内插进策略文本，是为了避免**路径里的特殊字符破坏策略语法**。一条路径若含引号或括号，直接拼进 SBPL 会让整份策略解析失败（最坏的情况是解析成一个意外宽松的策略）。参数化把「数据」放在参数通道，表达式里只留一个键名。

注意那四个参数集合被 `concat()` 到一起：`file_read_dir_params`、`file_write_dir_params`、`protected_ancestor_params`、`unix_socket_dir_params(&proxy)`。也就是说**只有路径类的东西走参数，策略类的东西（allow/deny 表达式）走文本**。

**`SeatbeltPreparationError` 只有两个变体，这个划分反映它的失败来源。**

```rust
pub(crate) enum SeatbeltPreparationError { FileSystem(String), EnvironmentNetworkProxy(String) }
```

`FileSystem` 来自路径规范化（符号链接、顶层别名等）；`EnvironmentNetworkProxy` 来自代理环境解析。两类的修补动作完全不同——前者是配置里的路径有问题，后者是代理环境变量有问题。`Display` 实现让两者都直接写消息，不额外加前缀，因为 `manager.rs` 那一层已经把类型翻译成 `SandboxTransformError::SeatbeltPreparation` / `EnvironmentNetworkProxy`，前缀在那里。

**符号链接处理：一个 `nested_symlink_component` 的判据。**

```rust
path.ancestors().find(|ancestor| {
    let Ok(metadata) = std::fs::symlink_metadata(ancestor) else { return false; };
    metadata.file_type().is_symlink() && ancestor.parent().and_then(Path::parent).is_some()
})
```

注释：`Keep top-level macOS aliases such as /tmp -> /private/tmp compatible, but reject symlinks in user-controlled path components.`

读法：`ancestor.parent().and_then(Path::parent).is_some()` 意味着「这个祖先至少是第三层」（有祖父以上的层级）。即**顶层符号链接放行、深层符号链接拒绝**。

这个区分有实际理由：`/tmp`、`/var` 这类 macOS 顶层别名是系统既有的、可信的（它们指向 `/private/...`）；而工作区里的符号链接是用户/程序可创建的，可能指向策略之外的位置。若不对后者做处理，一条 `(subpath "/work/evil-link")` 就能把一个本不该给的范围包进来。

与之配套的 `normalize_top_level_alias_for_sandbox` 专门处理顶层别名：找到「父目录存在但祖父目录不存在」的那一层（即第一层），若它是符号链接就 `canonicalize` 并把前缀替换掉。

**`UnixDomainSocketPolicy` 用两个变体避免「带了被忽略状态的 allow-all」。**

```rust
enum UnixDomainSocketPolicy { AllowAll, Restricted { allowed: Vec<AbsolutePathBuf> } }
```

注释直接说了动机：「Keep allow-all and allowlist modes disjoint so we don't carry ignored state」。若做成 `{ allow_all: bool, allowed: Vec<...> }`，那么在 allow-all 为真时 `allowed` 就是无意义的残留数据，容易导致「以为在限制其实没限制」的误读。两个互斥变体让这种情况不可能出现。

而 `from_allowlist` 在归一化失败时的处理是**丢弃该条并 warning**，不是报错——注释写「ignoring ... because it could not be normalized」。

**返回值是完整的 argv。** 最终返回：

```rust
Ok(seatbelt_args)
```

而 `manager.rs` 会在前面加 `/usr/bin/sandbox-exec`。所以最终执行形态是：`/usr/bin/sandbox-exec -p <策略> -D<key>=<路径>... -- <原命令>`。

## 关键取舍

**源码事实：** 四份 `.sbpl` 模板通过 `include_str!` 编译进二进制，策略文本无法被运行时替换。

**源码事实：** 基线策略以 `(deny default)` 起手，随后逐条 allow，包含为 Python 多进程（`kern.sysv.semmns`、`ipc-posix-sem`）、PyTorch/libomp（`__KMP_REGISTERED_LIB_*` 共享内存）、PTY（`/dev/ptmx`、`/dev/ttys*` 的 ioctl）准备的必要权限。

**源码事实：** 代理端口从 `PROXY_URL_ENV_KEYS` 对应的环境变量解析，只认 `localhost` / `127.0.0.1` / `::1` 三种回环主机，缺 scheme 时补 `http://`，缺端口时按 scheme 补默认端口，结果用 `BTreeSet` 保证确定性。

**源码事实：** 配了代理却解析不出回环端口时**返回空网络策略**（零网络权限）而非退回全放行；受管网络无可用端点时同样返回空策略。

**源码事实：** 两条「非路径型」的 deny 必须显式写：`(deny file-write-unlink ...)` 防 rename 搬走被保护子路径且必须置于最后；`(deny system-fcntl (fcntl-command 80 110))` 防通过只读 fd 的 `F_MAKECOMPRESSED` / `F_TRANSFEREXTENTS` 产生写副作用。

**源码事实：** `sandbox-exec` 只用绝对路径 `/usr/bin/sandbox-exec`，绝不从 `PATH` 解析。

**源码事实：** 路径经 `-D<key>=<value>` 参数化，策略文本里只留 `(param "key")`，避免路径里的特殊字符破坏语法。

**源码事实：** 符号链接规则是「顶层别名放行、深层拒绝」，因为顶层别名是系统既有的、可控性低。

**架构推论：** `(deny default)` 加逐条 allow 的方向，决定了这个模块的维护模式必然是「**遇到问题补一条**」。基线里那些针对 Python 进程池、OpenMP、PTY 的条目都是这么来的。这与「设计一份完备策略」的理想相反，但符合现实——用户态沙箱的失败几乎总是以「某个工具莫名其妙起不来」的形式出现，只能逐例补。

**架构推论：** 那两条「非路径型 deny」揭示了一个更普遍的教训：**按路径写的策略无法覆盖不按路径产生的副作用**。rename 绕过路径 carveout、fcntl 绕过 write 类别，都属于「操作的效果落在策略的盲区」。这类盲区只能靠对系统调用语义的理解逐个补，无法靠路径枚举穷尽。

**架构推论：** 「代理配了但解析不出来就零网络」是 fail-closed 的教科书式应用。它的判据是：**不确定时该偏向哪一侧**。这里的答案是「偏向不给网络」，因为代理配置的存在说明用户期望流量受控，此时给全放行等于绕过用户意图。呼应 d01/c01 讲的「失败方向要偏向保守」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/sandboxing/src/seatbelt.rs` | `MacosSeatbeltProfile` | `Process` / `FileSystemHelper` 两种用途的更窄/更宽档 |
| `codex-rs/sandboxing/src/seatbelt.rs` | `MACOS_SEATBELT_BASE_POLICY` | 编译期嵌入的基线策略（`(deny default)` 起手） |
| `codex-rs/sandboxing/src/seatbelt.rs` | `MACOS_SEATBELT_NETWORK_POLICY` | 本地平台服务的网络白名单模板 |
| `codex-rs/sandboxing/src/seatbelt.rs` | `MACOS_PATH_TO_SEATBELT_EXECUTABLE` | 绝对路径常量，禁用 `PATH` 查找 |
| `codex-rs/sandboxing/src/seatbelt.rs` | `is_loopback_host` / `proxy_scheme_default_port` | 回环主机判定与按 scheme 的默认端口 |
| `codex-rs/sandboxing/src/seatbelt.rs` | `proxy_loopback_ports_from_env` | 从代理环境变量解析回环端口（确定性输出） |
| `codex-rs/sandboxing/src/seatbelt.rs` | `dynamic_network_policy_for_network` | 网络策略拼装与三条 fail-closed 分支 |
| `codex-rs/sandboxing/src/seatbelt.rs` | `SeatbeltPreparationError` | `FileSystem` / `EnvironmentNetworkProxy` 两类失败 |
| `codex-rs/sandboxing/src/seatbelt.rs` | `nested_symlink_component` | 顶层别名放行、深层符号链接拒绝的判据 |
| `codex-rs/sandboxing/src/seatbelt_network_policy.sbpl` | 模板 | 31 行的网络白名单，含 `AF_SYSTEM` socket 与 TLS/网络配置的 `mach-lookup` |
| `codex-rs/sandboxing/src/seatbelt_base_policy.sbpl` | 模板 | 116 行基线，含 Python/OpenMP/PTY 的必要放行 |

## 参考

- [open-interpreter 源码：sandboxing/src/seatbelt.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/sandboxing/src/seatbelt.rs)
- [Apple 文档：Sandbox Profile Language](https://developer.apple.com/library/archive/documentation/Security/Conceptual/AppSandboxDesignGuide/AppSandboxInDepth/AppSandboxInDepth.html)
- [Chromium 源码：macOS 沙箱策略](https://source.chromium.org/chromium/chromium/src/+/main:sandbox/policy/mac/common.sb)
- [`sandbox-exec` 手册页](https://www.unix.com/man-page/osx/1/sandbox-exec/)

## 与主核 / AI 的关系

「把权限档案渲染成一份声明式策略文本」与主核里的**生成式配置**（动态生成 `pg_hba.conf` 规则、生成检查约束表达式）是同一类工作：都是把内部的结构化策略翻译成外部系统能读的文本。关键风险也相同——**拼装出错会静默放宽**。这里的两道防护（路径走 `-D` 参数化、拒绝时 fail-closed）正是针对这一点。

「按路径写的策略有盲区」这条，与主核里**行级安全的表达式无法覆盖副作用**同理：一个 `SELECT` 上的策略可以限制它读哪些行，却管不到它在函数里的副作用。这类问题在两边都只能靠「逐个识别有副作用的原语」处理。

fail-closed 那三条分支与主核的**认证失败处理**同构：主核在 `pg_hba.conf` 匹配不到任何规则时默认拒绝连接，而不是默认放行。判据一致——**配置存在但无法解读时，偏向拒绝**。这与「没有配置」不同：没有配置可以按预设默认走，而配置存在却解析失败，说明有人表达了意图但系统没能实现它。

对 AI 工程的实际意义在于：macOS 是许多 agent 用户的主力平台，而 Seatbelt 是这里唯一可用的用户态沙箱。这份实现给出的三条经验可直接用：**基线策略必须包含「让常见工具链能跑」的条目**（否则用户只会觉得沙箱坏了而关掉它）、**代理端口要在拼策略时从环境解析**（因为策略是静态的而代理端口是动态的）、以及**不确定就给零权限**（代理配置存在但解析失败时，给全放行等于撤销用户意图）。
