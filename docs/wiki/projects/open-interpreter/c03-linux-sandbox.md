---
project: open-interpreter
feature: linux-sandbox
title: Linux Landlock 与 bwrap 沙箱
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/linux-sandbox/src/bwrap.rs
    symbols: [BwrapOptions, BwrapArgs, BwrapNetworkMode]
  - path: codex-rs/linux-sandbox/src/bwrap.rs
    symbols: [LINUX_PLATFORM_DEFAULT_READ_ROOTS, SyntheticMountTarget, FileIdentity]
---

# Linux Landlock 与 bwrap 沙箱

## 一句话职责

在 Linux 上把「一份路径白名单」翻译成一串 `bubblewrap` 参数，构造出一个只挂载被允许路径的新文件系统视图，并负责在沙箱退出后清理它为了构造视图而在宿主上临时创建的那些空文件与空目录。

## 机制是什么

Linux 这边没有一份「策略文本」可以写。它有两个原语：

- **bubblewrap（bwrap）** 是一个命令包装器，靠 `--ro-bind` / `--tmpfs` / `--bind` 这类参数**重排文件系统视图**。它构造的是「另一个视角的根目录」，而不是「给同一个根目录加限制」。
- **Landlock** 是内核的 LSM，给进程装一组按路径的访问规则，作为 bwrap 之外的兜底。

之所以要两个，是因为 bwrap 构造的视图在「宿主路径不存在」时就无能为力——`--ro-bind /nonexistent /nonexistent` 会直接失败（bwrap 要求 bind 的目标存在）。而权限档案里的「禁止读某路径」常常指向一个**知道要防、但当前不存在**的路径。这里就出现了这个模块最核心的一组构造：

| 场景 | 处理方式 |
|---|---|
| 要挂载的路径不存在 | **在宿主上先创建**一个空文件/空目录，挂载之后再删掉 |
| 想在空文件上「遮住」原内容 | `--bind-data` 一个空字节流 |
| 想遮住整个目录 | `--tmpfs` 挂一个空内存文件系统 |
| 退出后要清理 | 靠 `FileIdentity`（dev + ino）确认「这确实是当初我建的那个」 |

也就是说，**为了让沙箱生效，它必须先修改宿主文件系统**。这个动作本身就是危险的：若清理失败，用户的工作区会多出一些莫名其妙的空文件；若清理时误删了用户的真实文件，那是数据丢失。整页后面的机制有相当一部分是在处理这个风险。

## 怎么实现的

**`BwrapOptions` 五个字段，每个都对应一个环境差异。**

| 字段 | 默认 | 为什么需要 |
|---|---|---|
| `mount_proc` | `true` | 挂一份新的 `/proc`。注释：「This is the secure default, but some restrictive container environments deny `--proc /proc`」 |
| `network_mode` | `FullAccess` | 三态（见下） |
| `mask_wsl_interop` | `false` | WSL 有 Windows 互操作 socket，可借它启动不受限的 Windows 进程 |
| `mask_wslg_distro` | `false` | WSLg 会暴露一份重复的 distro 视图 |
| `glob_scan_max_depth` | `None` | 展开「禁止读」的 glob 时的最大深度，默认不封顶 |

`mount_proc` 那一句注释是这一页里最能体现「默认值是折中产物」的一处：安全上应该挂新的 `/proc`（否则能看到宿主进程），但某些容器不允许。于是默认开、可关。

**`BwrapNetworkMode` 三态，但三态只产生两个 bwrap 参数。**

```rust
enum BwrapNetworkMode { FullAccess, Isolated, ProxyOnly }
fn should_unshare_network(self) -> bool { !matches!(self, Self::FullAccess) }
```

即 `Isolated` 与 `ProxyOnly` 在 bwrap 层面做同一件事——`--unshare-net`。差别在于 `ProxyOnly` 之后要由 helper 进程建立代理路由桥：

> Bubblewrap enforces this by unsharing the network namespace. The proxy-routing bridge is established by the helper process after startup.

这个设计说明「沙箱只负责把网络摘掉，不管怎么接回来」。代理桥是运行期的事，不在 bwrap 参数里表达。

**空策略的短路与「全盘可写」两条捷径。**

```rust
if file_system_sandbox_policy.has_full_disk_write_access() && unreadable_globs.is_empty() {
    return if options.network_mode == BwrapNetworkMode::FullAccess {
        Ok(BwrapArgs { args: command, ... })        // 完全不套
    } else {
        Ok(create_bwrap_flags_full_filesystem(command, options))
    };
}
```

两处判断：

- **全盘写 + 全网络 + 无禁止读 → 完全不套 bwrap**。注释说「so we avoid unnecessary sandboxing overhead」。这是一个纯粹的性能优化：套 bwrap 会多一次 namespace 创建与挂载，而既然什么都不限制，就不必付这个代价。
- **全盘写但需要隔离网络 → 仍套，但走 `create_bwrap_flags_full_filesystem`**：`--bind / /`（保留全盘写）、`--dev /dev`、`--bind-try /dev/shm /dev/shm`、`--unshare-*` 三条、按需 `--unshare-net`、按需 `--proc /proc`、`--cap-drop ALL`。

注意「全盘写 + 有 glob 禁止读」这一支**不会**走捷径（因为 `unreadable_globs.is_empty()` 为假）。注释解释了：「Bubblewrap can only mask concrete paths. Expand unreadable glob patterns to the existing matches we can see before constructing the mount overlay」。即要让 glob 生效，必须先把它展开成具体路径。

`create_bwrap_flags_full_filesystem` 里有两条带注释的细节：

- **`--unshare-user` 是显式加的**。注释：「Always enter a fresh user namespace so root inside a container does not need ambient `CAP_SYS_ADMIN` to create the remaining namespaces」。即先降到一个无特权的用户命名空间，后续的 pid/ipc/net 命名空间就没那么高的权限门槛。
- **`--dev /dev` 与 `--bind-try /dev/shm /dev/shm`**：`--dev` 给一份最小的设备树（注释后面在别处列明 null/zero/full/random/urandom/tty）；`/dev/shm` 要单独 bind 回来，否则共享内存没了——注释「Restore shared memory without exposing other host devices」。

**挂载顺序在 `create_filesystem_args` 里被写成了六条编号规则。**

那段文档注释是这一页最重要的阅读材料，因为它解释了**为什么顺序不能乱**：

1. 全读策略，或显式读 `/` 的受限策略，用 `--ro-bind / /`；其余受限策略从 `--tmpfs /` 起手，再逐个 `--ro-bind` 挂允许的根。
2. `--dev /dev` 挂最小可写 `/dev`（含 `/dev/urandom`）。
3. **先把「可写根的不可读祖先」遮住**，再挂那些子挂载——否则嵌套的可写 carveout 无法安全重开。
4. `--bind <root> <root>` 打开允许的根的写权限（包括 `/dev/shm` 这类 `/dev` 下的可写子路径）。
5. `--ro-bind <subpath> <subpath>` 在那些可写根之下重新施加只读保护，让「受保护子路径」赢。
6. 可写根之下的嵌套不可读 carveout在该根被 bind **之后**遮住；无关的不可读根最后遮。

这个顺序的关键在第 3 与第 6 条：**遮罩必须相对于「重开」的正确层级发生**。若先遮了祖先再挂子根，子根会重新露出被遮的内容；若先挂了子根再遮祖先，祖先的遮罩会把子根一起遮掉。所以「遮蔽祖先 → 挂可写根 → 遮子 carveout」这个交错是唯一正确的序列。

而 `--dev /dev` 必须排在可写根之前——注释「`/dev` must be mounted before writable roots so explicit `/dev/*` writable binds remain visible」。

**`SyntheticMountTarget` 与 `FileIdentity` 是「临时修改宿主」的记账结构。**

```rust
struct FileIdentity { dev: u64, ino: u64 }
struct SyntheticMountTarget { path: PathBuf, kind: SyntheticMountTargetKind, pre_existing_path: Option<FileIdentity> }
```

`FileIdentity` 只有两个字段但足以唯一标识一个 inode（同一文件系统内）。它的用途写在 `pre_existing_path` 的字段注释里：

> If an empty metadata path was already present, remember its inode so cleanup does not delete a real pre-existing file or directory.

即：**若那个路径本来就存在（一个空文件）**，就记下它的 inode；清理时若 inode 变了，说明它不是当初那个，不要删。

`should_remove_after_bwrap` 把这个判断实现得很细：

```rust
match self.kind {
    EmptyFile => { if !metadata.file_type().is_file() || metadata.len() != 0 { return false; } }
    EmptyDirectory => { if !metadata.file_type().is_dir() { return false; } }
}
match self.pre_existing_path {
    Some(pre_existing_path) => pre_existing_path != FileIdentity::from_metadata(metadata),
    None => true,
}
```

分两层判：

- **形状必须仍然匹配**：文件必须还是文件且长度仍为 0；目录必须还是目录。若文件已被写入了内容、或已被换成别的东西，不删。
- **身份必须不是原来那个**：若当初就存在，只有 inode 变了才删（说明中间有人把它替换掉了，那现在这个就是我们的临时产物）；若当初不存在，直接删。

这个两层判断合起来的效果是：**清理只删「确定是自己建的、且仍是空壳」的东西**。任何不确定的情况都放过——留下一个空文件比删掉用户文件好。

`SyntheticMountTarget` 有三个构造函数，对应三种情形：

- `missing(path)`：路径不存在，建一个空文件。
- `missing_empty_directory(path)`：路径不存在，建一个空目录。
- `existing_empty_file(path, metadata)`：路径已存在且是空文件，记下 inode。

**在空文件上遮内容用 `--bind-data`，在目录上遮内容用 `--tmpfs`。** 这解释了为什么需要区分 `EmptyFile` 与 `EmptyDirectory`：两种遮罩手法不同，对应的清理对象也不同。

**glob 展开：`MAX_UNREADABLE_GLOB_MATCHES` 是一道拒绝线而不是静默截断。**

```rust
if expanded_paths.len() > MAX_UNREADABLE_GLOB_MATCHES {
    return Err(CodexErr::Fatal(format!(
        "unreadable glob expansion for {} matched more than {MAX_UNREADABLE_GLOB_MATCHES} paths", ...)));
}
```

能拿到禁读结论，就必须读返回值全部生效：**匹配超过 8192 就直接失败，而不是截断到 8192**。若截断，用户会以为某个目录被遮住了，而实际只遮了前 8192 个匹配——一个静默的安全缺口。

与之配套的是 `split_pattern_for_ripgrep` 的两条拒绝：

```rust
if static_prefix.is_empty() || static_prefix == "/" { return None; }
```

上层对此的处理是 `Err` 而不是跳过：

> unreadable glob `{pattern}` cannot be safely expanded; use a pattern with a non-root directory prefix

即**以根开头的 glob 一律拒绝**（因为它会把整个文件系统扫一遍，在沙箱构造阶段不可接受），并给出可操作的提示。

`expand_unreadable_globs_with_ripgrep` 还有一处优化写进了注释：「Group each pattern by the static path prefix before its first glob metacharacter. That keeps scans narrow, avoids searching from `/`, and lets one `rg --files` call handle all patterns under the same root」。即按「首个通配符前的静态前缀」分组，一个搜索根只跑一次 `rg`。

还有一处「逻辑路径与真实路径都要记录」：

```rust
if let Some(target) = canonical_target_if_symlinked_path(path.as_path()) {
    expanded_paths.insert(...target...);
}
expanded_paths.insert(path);
```

注释：「The bwrap overlay needs the resolved target to prevent a readable symlink path from bypassing an unreadable glob match」。即**一个符号链接既能通过逻辑路径访问、也能通过真实路径访问**，所以要遮两个。这是符号链接在沙箱里的经典漏洞面。

`escape_unclosed_glob_classes` 处理的是一处两边语义不一致：文件系统策略把未闭合的 `[` 当字面量，而 ripgrep 视为语法错误。所以展开前只把**未闭合的**类开符转义成 `\[`，闭合的保持原样。

**`FileIdentity` 与「宿主文件系统时间窗」的另一面：`protected_create_targets`。** `BwrapArgs` 里有四个集合：

```rust
pub struct BwrapArgs {
    pub args: Vec<String>,
    pub preserved_files: Vec<File>,
    pub synthetic_mount_targets: Vec<SyntheticMountTarget>,
    pub protected_create_targets: Vec<ProtectedCreateTarget>,
}
```

`preserved_files` 是打开的文件句柄（权限 000 的临时文件需要保持 fd 才能后续 unlink 与重建），`synthetic_mount_targets` 是上面讲的自建挂载点，`protected_create_targets` 是「不许被创建」的路径。四者共同构成「bwrap 之外还要做的宿主侧动作」——bwrap 参数只能表达视图，这些动作必须在视图之外执行。

**`normalize_command_cwd_for_bwrap` 与 `--chdir` 处理一个符号链接陷阱。**

```rust
if normalized_command_cwd.as_path() != command_cwd {
    args.push("--chdir".to_string());
    args.push(path_to_string(normalized_command_cwd.as_path()));
}
```

注释：「Bubblewrap otherwise inherits the helper's logical cwd, which can be a symlink alias that disappears once the sandbox only mounts canonical roots. Enter the canonical command cwd explicitly so relative paths stay aligned with the mounted filesystem view」。

展开说：沙箱内挂的是**规范路径**（如 `/private/tmp/...`），而 helper 的逻辑 cwd 可能是别名（`/tmp/...`）。若不显式 `--chdir` 到规范路径，进程会停在一个在沙箱里**不存在**的目录上，于是所有相对路径都错。

**`LINUX_PLATFORM_DEFAULT_READ_ROOTS` 的注释明确划了边界。**

```rust
const LINUX_PLATFORM_DEFAULT_READ_ROOTS: &[&str] = &[
    "/bin", "/sbin", "/usr", "/etc", "/lib", "/lib64", "/nix/store", "/run/current-system/sw",
];
```

注释：「These are intentionally system-level paths only（plus Nix store roots）so `include_platform_defaults` does not silently widen access to user data」。即这份清单只含系统路径与 Nix 的 store 根（后者是 NixOS 上动态库的真实位置），**不含任何用户目录**。这是「平台默认值不得静默扩大用户数据访问面」的具体落实。

而它们在加入时还过一道 `path.exists()` 过滤——注释在别处说明：混合平台配置里可能带着别的平台才有用的路径（如 macOS 的 `/Applications`），Linux 上不存在就跳过，而不是让 bwrap 因 bind 失败而崩。

**WSL 的两处特殊处理值得单独看。**

- **`mask_wsl_interop`**：用 `--tmpfs` 把 `/run/WSL` 遮掉。注释：「WSL's Windows-process bridge uses Unix sockets under `/run/WSL`。Mask them after all filesystem binds so network access cannot bypass the Linux filesystem policy by launching an unrestricted Windows process」。即 WSL 的互操作 socket 是一个**逃逸通道**——通过它能启动不受 Linux 沙箱限制的 Windows 进程。所以必须在所有 bind 之后遮掉。
- **`mask_wslg_distro`**：WSLg 暴露 `/mnt/wslg/distro`，是 distro 根目录的重复视图。注释：「An already hidden ancestor cannot accommodate a new child mount」——若祖先已被遮，子挂载无法建立，所以要特殊处理。

两者合起来还有一条：若 `mask_*` 为真且**没有**挂新 `/proc`，就要用 `--tmpfs /proc` 遮宿主 procfs——「Without a fresh procfs, the root bind retains the host procfs. Hide it so host process roots cannot restore the masked WSL views」。这条揭示了一个微妙的牵连：遮了文件系统视图但留着宿主 procfs，进程仍能通过 `/proc/<pid>/root` 拿到被遮的内容。

**`reject_daemon_mount_aliases` 是启动前的一道检查。** 它在 `create_filesystem_args` 最开头调，参数是共享守护进程的 socket 目录与（可能有的）WSLg distro 根。注释没在这段写理由，但从名字与位置可以读出：若这两个目录与守护进程 socket 目录构成别名关系，后面那套遮蔽与重开就会互相干扰。

## 关键取舍

**源码事实：** 全盘写 + 全网络 + 无禁读 glob 时完全不套 bwrap（纯性能优化）；全盘写但需隔离网络时仍套，但走「全盘 bind + 只拆命名空间」的轻量路径。

**源码事实：** 挂载顺序被写成六条编号规则，核心是「遮祖先 → 挂可写根 → 遮子 carveout」的交替，「`--dev` 必须在可写根之前」。

**源码事实：** bwrap 要求 bind 目标存在，所以失效的挂载点要在宿主上**先创建**，退出后再删；`SyntheticMountTarget` 与 `FileIdentity` 为此记账。

**源码事实：** 清理的判据是双重：形状必须仍是空文件/空目录，且 inode 必须不是「当初就存在的那个」。任何不确定都放过。

**源码事实：** glob 展开超过 `MAX_UNREADABLE_GLOB_MATCHES`（8192）直接失败而非截断；以根开头的 glob 直接拒绝并给出提示。

**源码事实：** glob 展开同时记录逻辑路径与真实符号链接目标，防止通过链接绕过遮罩。

**源码事实：** `LINUX_PLATFORM_DEFAULT_READ_ROOTS` 只含系统路径与 Nix store 根，且逐个过 `path.exists()`。

**源码事实：** WSL 的互操作 socket（`/run/WSL`）与 WSLg 的重复 distro 视图都要遮，且遮在**所有 bind 之后**；没有新 `/proc` 时还要遮宿主 procfs。

**架构推论：** 这个模块的复杂度的真正来源是「bwrap 只能构造视图，不能表达『路径不存在但不许读』」。一旦承认这一点，就必须面对「为了表达策略得先改建宿主」这个循环：改宿主本身有风险，于是要为每次改动记账、为每次清理做身份校验。`FileIdentity` 的存在完全是被这个循环逼出来的。

**架构推论：** 挂载顺序那六条规则是无法从 bwrap 文档推出的——它们来自「遮蔽与重开的相对层级必须正确」这条不变量。这类顺序依赖在命令式原语（参数列表即顺序）里只能靠文档注释固定，无法用类型系统保证。这也是为什么那段注释写得像一份算法说明。

**架构推论：** 「超过上限就失败而非截断」这条与 c01 的「做不到就拒绝」是同一条纪律的两种形态。截断会产生**静默的安全缺口**——用户以为遮住了全部，实际只遮了一部分。而在沙箱里，这类缺口比崩溃危险得多，因为它不可见。

**架构推论：** WSL 互操作 socket 那一段值得单独记住，它揭示了一个通用模式：**在给下层操作系统套限制时，要检查同一台机器上是否存在「绕过这条限制的另一条通道」**。WSL 的 Windows 进程桥就是这样一个通道——Linux 侧的沙箱对 Windows 进程无效。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/linux-sandbox/src/bwrap.rs` | `BwrapOptions` | 五个环境差异开关（`/proc`、网络、WSL 两处、glob 深度） |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `BwrapNetworkMode` | 三态网络，其中两态在 bwrap 层面等效 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `BwrapArgs` | argv + 三类宿主侧动作（保留 fd、自建挂载点、保护创建目标） |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `create_bwrap_command_args` | 入口与两条捷径判断 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `create_filesystem_args` | 六条挂载顺序规则与遮蔽/重开交替 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `SyntheticMountTarget` / `FileIdentity` | 临时挂载点的记账与身份校验 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `expand_unreadable_globs_with_ripgrep` | 按静态前缀分组展开，超限即失败，逻辑与真实路径双记 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `split_pattern_for_ripgrep` / `escape_unclosed_glob_classes` | 根开头的 glob 拒绝；未闭合类开符转义 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `LINUX_PLATFORM_DEFAULT_READ_ROOTS` | 仅系统路径与 Nix store 根的平台默认值 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `MAX_UNREADABLE_GLOB_MATCHES` | 8192 条匹配的拒绝线 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `normalize_command_cwd_for_bwrap` | 规范 cwd 与 `--chdir` 的符号链接陷阱 |

## 参考

- [open-interpreter 源码：linux-sandbox/src/bwrap.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/linux-sandbox/src/bwrap.rs)
- [bubblewrap 仓库](https://github.com/containers/bubblewrap)
- [Linux 内核文档：Landlock](https://docs.kernel.org/userspace-api/landlock.html)
- [WSL 文档：Windows 与 Linux 互操作](https://learn.microsoft.com/en-us/windows/wsl/filesystems)

## 与主核 / AI 的关系

「构造一个新视图而不是给原视图加限制」这件事，与主核里的**快照与写时复制**有结构性相似：两者都不修改原始数据，而是给出一个「另一个版本的世界」。差别在于沙箱要的是**更窄**的视图，而快照要的是**某个时刻**的视图。

「为了让限制生效而必须先改宿主」这个循环，在主核里对应的是**为满足约束而创建辅助对象**：要建一个唯一索引，主核得先写索引文件；若中途失败，索引文件会残留。主核的解法是让这些辅助对象登记在事务里，失败即回滚。这里的 `SyntheticMountTarget` 与 `FileIdentity` 就是同一件事的手工版本——没有事务可用，只能用「记录 inode + 两层判据」来近似。

「超过上限就失败而非截断」与主核里**拒绝超出资源限制的操作**一致：主核不会「尽量建一部分索引」，而是直接报错。判据相同——**部分完成的安全设施比没有更危险**，因为它看起来在工作。

对 AI 工程的实际意义在于：Linux 是最常见的 agent 运行环境（容器、CI、服务器），而 bwrap 是这里最可靠的用户态沙箱。这份实现给出的三条经验可直接用：**遮蔽与重开有严格顺序**（顺序错了保护就失效，且不会报错）、**为沙箱而修改宿主时必须记账**（否则清理阶段可能删掉用户文件）、以及**检查绕过通道**（WSL 的 Windows 进程桥让 Linux 沙箱失效，这类跨子系统通道必须单独处理）。
