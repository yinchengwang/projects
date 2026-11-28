---
project: goose
feature: skills
title: 技能与支撑文件
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/skills/builtin.rs
    symbols: [get_all]
  - path: crates/goose/src/skills/supporting_files.rs
    symbols: [read_supporting_file_with_limit, ReadLimit]
---

# 技能与支撑文件

## 一句话职责

两件与「技能」有关的事：**内置技能在编译期被打进二进制**（`include_dir!` 把 `builtins/` 下的 markdown 焊进可执行文件），而**支撑文件的读取要穿过两道独立的围栏**——体积上限（字符与字节各一道）与路径围栏（只能读到技能目录里、且不被符号链接与硬链接带出去）。

## 机制是什么

| 部件 | 内容 |
|---|---|
| `builtin::get_all()` | 返回内置技能的 markdown 文本（`Vec<&'static str>`），编译期嵌入 |
| `ReadLimit` | 两个单位：`Characters(n)` / `Bytes(n)` |
| `read_supporting_file_with_limit` | 读一个支撑文件，按字符上限 |
| `validated_relative_components` | 路径围栏的第一道：只允许「普通组件」 |
| `directory_traversal_flags` | 路径围栏的第二道：逐组件 `openat` 时的 `O_DIRECTORY \| O_NOFOLLOW` |
| `open_skill_root` / `open_at` | 从 `/` 开始逐段打开，最终校验走到的就是目标目录 |

`supporting_files.rs` 有 1064 行，其中三段是**同一套逻辑的三个平台实现**（unix / windows / 其余），所以真正的机制比行数少得多。

## 怎么实现的

### 内置技能：编译期嵌入

**`builtin.rs` 只有 11 行，但它做的事很彻底。** 核心是那句宏：

```rust
static BUILTIN_SKILLS_DIR: Dir = include_dir!("$CARGO_MANIFEST_DIR/src/skills/builtins");
```

`include_dir!` 在**编译期**把整个目录树写进二进制的只读数据段。于是 `get_all()` 的实现不需要任何文件系统访问：

```
files() → filter(扩展名 == "md") → filter_map(contents_utf8) → collect
```

**返回值是 `&'static str`，这个类型就是「内置」的定义。** 内置技能不可能是运行期产生的、不可能是用户改的、也不占用启动时的 IO——它是二进制的一部分。

**`contents_utf8()` 的 `filter_map` 顺带做了一次编码过滤。** 一个非 UTF-8 的 `.md` 会被静默丢掉。这在编译期嵌入的场景里是合理的：源文件本来就该是 UTF-8。

**内置技能与文件系统技能的合并遵循「文件系统优先」。** 这段逻辑在 `skills/mod.rs` 的发现流程里，而非本文件：先拿到文件系统扫出的 `sources`，收集它们的名字成 `seen`，然后逐个尝试内置技能，**只有名字不在 `seen` 里才加入**。

于是用户可以用一个同名技能覆盖内置技能。这条规则的必要性很明显：内置技能被打进二进制，用户无法删除它；若内置优先，用户就永远改不掉它。而技能名是唯一标识，所以「文件系统优先」等于「用户可用同名文件覆盖」。

`SourceType::BuiltinSkill` 这个枚举值让下游能区分来源，而 `path` 被写成一个人造的 `builtin://skills/{name}`——一个**不是路径的路径**，因为内置技能确实没有文件。这个 URI 形状的选择是对的：它让「有个 path 字段」与「这是内置的」两件事同时成立，且不会让任何代码以为真能 `open` 它。

### 体积：两道上限，两个错误

**`ReadLimit` 只有两个变体，而它们服务的场景不同。** `Characters(n)` 用于支撑文件（上限来自 `agents::max_tool_response_size()`，与 `e03` 的大响应阈值同一个来源）；`Bytes(n)` 用于源文件（`MAX_SOURCE_FILE_BYTES = scheduler::MAX_SCHEDULE_RECIPE_BYTES`，即 1 MiB）。

**支撑文件的字符上限有一个「先扣包装」的步骤，这是本页最细的一处。** `load_supporting_file_with_limit` 先算出包装的字符数：

```
LOADED_FILE_PREFIX + skill_name + LOADED_FILE_SEPARATOR + LOADED_FILE_SUFFIX
```

然后用 `max_characters.checked_sub(wrapper_characters)` 得到内容自己的上限。若减法下溢（上限比包装还小），返回一个 `InvalidInput` 错误并说明「上限小到装不下这个技能名」。

**这个扣减的意义在于「上限的归属」。** 若直接按上限读内容，再加包装，最终交给模型的文本就会超上限——而那个上限正是 `e03` 里用来决定「是否落盘」的阈值。所以这里必须保证**包装之后的成品不越界**，否则一次「正常读取」会触发大响应落盘，把技能内容变成文件路径。

`checked_sub` 而不是 `saturating_sub` 也是对的：上限真的装不下时应当报错，而不是产出一个空文件。

**字符上限如何落实成字节操作，是这一页技术含量最高的一段。** `max_utf8_bytes` 把字符数乘 4（`checked_mul`）——UTF-8 一个字符最多 4 字节，所以这个乘积是「安全上界」。

然后 `read_utf8_with_limit` 做三步：

1. `reader.take(max_bytes + 1).read_to_end(&mut bytes)`——**多读一个字节**；
2. `if bytes.len() > max_bytes → file_encoding_too_large`;
3. `String::from_utf8(bytes)` 之后 `if content.chars().count() > max_characters → file_too_large`。

第 1 步那个 `+1` 是经典的「探测溢出」技巧：读 `N+1` 字节，若真读满 `N+1` 就说明文件超了。没有它就无法区分「恰好 N 字节」与「N 字节之后还有内容」。

**第 2 与第 3 步的两条错误消息分别是不同的话。** `file_encoding_too_large` 说「超过最大编码大小 N 字节」，`file_too_large` 说「超过最大 N 字符」。也就是说**同一个文件可能因为两个不同的原因被拒**：一段 50 万个 ASCII 字符的文本超字符上限但其字节数远小于 4 倍；而一段 40 万个 4 字节字符（如 emoji）可能字符数没超但字节数超了。

这个双限是必要的，且两个方向都有实际意义：字符限保护上下文（token 预算与字符数相关），字节限保护内存（一个 4 倍膨胀的上界意味着 `max_bytes` 是实际分配的上界）。

**`read_opened_file` 在读完之前还做了一次 metadata 预检。** `if file.metadata()?.len() > max_bytes as u64` 就直接报错——**一个 10 GiB 的文件不会先把 10 GiB 读进内存再判断**。

这道预检与读时的 `+1` 探测是两层保障：预检挡住「文件本来就很大」，读时探测挡住「metadata 说小但内容更多」（例如一个随后被追加的文件，或一个 FIFO）。

**`O_NONBLOCK` 在这个读路径上不是性能选项而是安全选项。** 打开文件时用了 `O_RDONLY | O_NONBLOCK`——一个**命名管道（FIFO）**在没有写者时会让 `read` 永久阻塞，而 `O_NONBLOCK` 让它立刻返回 `EWOULDBLOCK`。一个声称是「支撑文件」的 FIFO 可以因此让整个进程挂住。

### 路径围栏：两道，且第二道是内核级的

**第一道 `validated_relative_components` 只认「普通组件」。** 它逐个看 `path.components()`：

| 组件 | 处理 |
|---|---|
| `Normal` | 收进列表 |
| `CurDir`（`.`） | **跳过**（等价于不存在） |
| `ParentDir`（`..`） | 拒绝 |
| `RootDir`（`/`） | 拒绝 |
| `Prefix`（Windows 的 `C:\` 等） | 拒绝 |

拒绝时的消息是 `supporting file path must stay within the skill directory`——一句直接说明意图的话。而组件列表为空（例如路径是 `.` 或空）时另报一条 `must name a file`，说明「空路径不是读不到文件，而是参数错了」。

**允许 `CurDir` 而拒绝 `ParentDir` 的选择是有道理的。** `./` 前缀与路径中间的 `./` 不改变解析结果，所以拒绝它只会让合理输入失败；`..` 则会离开目录，必须拒绝。Windows 的 `Prefix` 也要拒绝，否则 `C:\Windows\...` 会绕过一切——注意在 Windows 上 `Prefix` 不只出现在开头，`Component` 的解析会把它单独拎出来。

**但第一道是不够的：字符串层面的检查挡不住符号链接。** 一个 `skill_dir/link → /etc/passwd` 的符号链接完全由普通组件构成（`link` 是 `Normal`），通过第一道检查。所以第二道是**逐组件 `openat`**。

**`open_skill_root` 从 `/` 开始，一段一段 `openat`。** 实现是：

1. 打开 `/`，旗标是 `directory_traversal_flags()`，路径变量从 `/` 起。
2. 遍历 `skill_dir.components()`：`RootDir` 只允许出现在最前（置 `saw_root`）；`Normal` 且已见过 root 才调 `open_at` 并把组件推入 `opened_path`；`CurDir` 忽略；其余一律报 `skill path must be an absolute normalized path`。
3. 结束后校验两点：`saw_root` 为真且 `opened_path == skill_dir`（**走到了就是要的目录**）、且 `metadata()?.is_dir()`。

第 3 点的第一个校验返回的是同一句错误——也就是说**任何非绝对、非规范化的 `skill_dir` 都被拒绝**。这条很重要的：`open_skill_root` 不接受 `..` 或相对路径，所以它的「从 `/` 逐段走」不会因为路径本身就有 `..` 而失效。

**`directory_traversal_flags()` 有三套，因为 `O_PATH` / `O_SEARCH` / `O_RDONLY` 各有平台限制。** 三个 cfg 变体：

| 平台 | 旗标 |
|---|---|
| Linux / Android | `O_PATH \| O_DIRECTORY \| O_NOFOLLOW \| O_CLOEXEC` |
| macOS / AIX / *BSD / illumos / Solaris | `O_SEARCH \| O_DIRECTORY \| O_NOFOLLOW \| O_CLOEXEC` |
| 其余 unix | `O_RDONLY \| O_DIRECTORY \| O_NOFOLLOW \| O_CLOEXEC` |

**三个变体的共同项才是重点**：`O_DIRECTORY`（打开的不是目录就失败，挡住「某个组件其实是普通文件」）、`O_NOFOLLOW`（**该组件是符号链接就失败**，这是围栏的核心）、`O_CLOEXEC`（不把 fd 泄漏给子进程）。

而前三者的差别只在于「用什么方式打开一个只用来当作 `openat` 基准的目录 fd」：Linux 用 `O_PATH`（不产生真实的文件访问权限检查，最轻）；有 `O_SEARCH` 的平台用它（POSIX 的「只为搜索」语义）；其余退回 `O_RDONLY`。

**这份三套实现说明作者查过各平台的语义，而不是「挑一个能编译的」。** 一个只写 `O_RDONLY|O_DIRECTORY|O_NOFOLLOW` 的实现在 Linux 上也能跑，但会要求目录有读权限（`O_PATH` 不需要）；在 macOS 上则不是最优。

**`open_at` 用 `CString::new` 挡 NUL 字节。** 一个含 NUL 的名字会让 C 的 `openat` 静默截断——`a\0b` 变成 `a`。Rust 的 `Path` 允许 NUL（`OsStr` 可以是任意字节），所以这里必须显式拒绝，错误消息是 `supporting file path contains a NUL byte`。

**最后一段的打开旗标又加了两项：`O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC`。** 注意这里也带 `O_NOFOLLOW`——**即使前面的目录都是通过 `openat` 逐个验证过的，最后这个文件名仍然要挡符号链接**。这是必要的：验证目录与打开文件之间有时间窗（TOCTOU），而 `O_NOFOLLOW` 让这个窗口对最终组件无效。

`O_NONBLOCK` 在这里同时起到了「挡 FIFO」的作用（见上文）。

**打开之后还有一道类型检查。** `if !file.metadata()?.is_file()` → `supporting file is not a regular file`。于是设备文件、目录、FIFO 都被挡在外面。`O_NOFOLLOW` 只挡符号链接，挡不住 `/dev/zero`——`is_file()` 补上这一格。

**`after_opened_component` 这个钩子是给测试用的，而它是这套设计的证明。** 所有 `read_confined_file_with_hook` / `open_skill_root` 都接受一个 `impl FnMut(&Path)`，**每打开一个组件就回调一次**。生产代码传入的是 `|_| {}`（`read_supporting_file_with_limit` 就是这么调用的）。

它的用途只能是测试：在第 N 个组件已打开、第 N+1 个尚未打开的那一刻把某个目录换成符号链接，然后验证读取仍然被拒绝。**这正是「目录 fd 走查」免疫 TOCTOU 的证明**——因为后续的 `openat` 是相对已打开的 fd，不受「路径上的某个中间目录被换掉」影响。

一个为了漏洞验证而存在的注入点是罕见的，而它在这里位置正确：回调在每次成功 `openat` 之后触发，恰好是竞态窗口的位置。

### 写入路径的三道额外检查

**写与读共用同一套目录走查，但最终的打开旗标与检查更严。** 写用的旗标是 `O_WRONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC`，`create_new` 时再加 `O_EXCL`，模式 `0o666`。

然后三道检查：

1. `metadata.is_file()`——同读路径。
2. **`ensure_source_file_has_single_link`：`metadata.nlink() != 1` 就拒绝**，错误是 `source path must have exactly one hard link`。
3. 非 `create_new` 时 `file.set_len(0)` 截断再写。

**第二道是硬链接防御，而它的理由是读路径没有的那类风险。** 一个硬链接（`skill_dir/x` 与 `/home/user/.ssh/authorized_keys` 是同一个 inode）完全由普通组件构成、`O_NOFOLLOW` 也挡不住（硬链接不是符号链接，它本身就是那个文件）。`nlink == 1` 这条把「这个文件被别处也指着」这个事实当成危险信号。

对**写**来说这个信号确实危险：写入会改掉那个 inode，也就是改掉了技能目录之外的另一个名字指向的文件。对**读**来说风险小得多（只会读到自己本来也能读的内容），所以读路径**没有**这条检查——这是一个按方向区分的取舍，而不是漏写。

**`O_EXCL` 与「先检查后截断」的顺序也值得注意。** 新建文件用 `O_EXCL` 保证「要么新建、要么失败」，避免覆盖已存在的文件（一个指向目录外的硬链接不能靠这个绕过，因为 `nlink` 检查在写入之前）。而更新文件走「打开 → 检查 → `set_len(0)`」——**先确认是个符合要求的普通文件，再清空**，而不是先清空再检查。

### 其他平台直接拒绝

**`#[cfg(not(any(unix, windows)))]` 的实现只做一件事：跑一遍路径检查，然后返回 `Unsupported`。** 两条消息是 `secure supporting file reads are not supported on this platform` 与 `secure source file writes are not supported on this platform`。

**这是一个「宁可不做也不做错」的选择**：在没有 `openat` 一类原语的平台上，用「先 `canonicalize` 再 `open`」的退化实现会重新引入 TOCTOU 窗口。作者选择让功能不可用，而不是让围栏变成建议。

## 关键取舍

**源码事实：** 内置技能在编译期由 `include_dir!` 打进二进制，返回 `&'static str`；与文件系统技能合并时**文件系统优先**（同名内置技能可被用户覆盖），内置技能的 `path` 是人造的 `builtin://skills/{name}`。

**源码事实：** 支撑文件的上限要先减掉包装（前缀 + 技能名 + 分隔符 + 后缀）的字符数，保证成品不越 `e03` 的大响应阈值。

**源码事实：** 字符上限通过「乘 4 得上界」+「多读 1 字节探测溢出」实现，并与字节上限各有独立的错误消息。

**源码事实：** `read_opened_file` 先按 metadata 长度预检，再读；所以一个超大文件不会被整体读进内存。

**源码事实：** 打开文件带 `O_NONBLOCK`，这是为了挡住「名为支撑文件的 FIFO」让读永久阻塞，不是性能考虑。

**源码事实：** 路径围栏两道：组件级拒绝 `ParentDir`/`RootDir`/`Prefix`（允许 `CurDir`），以及逐组件 `openat` 带 `O_NOFOLLOW`。三套平台旗标只差「用哪种方式开目录 fd」，共同项是 `O_DIRECTORY`/`O_NOFOLLOW`/`O_CLOEXEC`。

**源码事实：** 最终组件也带 `O_NOFOLLOW`，所以「验证目录 → 打开文件」之间被换掉的窗口对最终组件无效；`is_file()` 再补上「非符号链接但也不是普通文件」的那一格。

**源码事实：** `open_at` 用 `CString::new` 拒绝含 NUL 的路径名，否则 C 层会静默截断到 NUL 之前。

**源码事实：** 写路径多一道 `nlink == 1` 的硬链接检查（读路径没有），因为写会改到技能目录之外的另一个名字；更新走「打开 → 检查 → 截断」而不是反过来。

**源码事实：** 不支持的平台返回 `Unsupported` 而不是退化实现——避免用「先 canonicalize 再 open」把围栏变成建议。

**源码事实：** 每个组件打开后都触发 `after_opened_component` 回调，生产代码传空闭包；它存在是为了测试能模拟「中途被换成符号链接」。

**架构推论：** 这套实现的核心不是「检查路径字符串」，而是**把路径解析的权威交给内核的 fd 而不是用户的字符串**。`openat` 相对一个已打开的目录 fd，所以「路径前面的部分被换掉」这件事对它无效——安全边界从「我相信这个字符串」变成了「我手上这个 inode」。

这个方向与主核里「用 `dirname`/`basename` 拼接路径」的思路是对立的，也解释了为什么它要写三段平台实现：`openat` 式的接口在不同 Unix 上的旗标不同、在 Windows 上要换成 `NtCreateFile` 加 reparse point 检查。**跨平台的安全围栏没有捷径，只能逐个平台写。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/skills/builtin.rs` | `get_all` | 返回编译期嵌入的内置技能 markdown |
| `crates/goose/src/skills/builtin.rs` | `BUILTIN_SKILLS_DIR` | `include_dir!` 的静态树，构建期打进来 |
| `crates/goose/src/skills/supporting_files.rs` | `ReadLimit` | 字符 / 字节两种上限单位 |
| `crates/goose/src/skills/supporting_files.rs` | `load_supporting_file_with_limit` | 扣掉包装字符数后读内容 |
| `crates/goose/src/skills/supporting_files.rs` | `read_supporting_file_with_limit` | 支撑文件读取入口（字符上限） |
| `crates/goose/src/skills/supporting_files.rs` | `validated_relative_components` | 路径围栏第一道：只收普通组件、拒绝 `..` |
| `crates/goose/src/skills/supporting_files.rs` | `directory_traversal_flags` | 三套平台旗标，共同项是 `O_DIRECTORY`/`O_NOFOLLOW` |
| `crates/goose/src/skills/supporting_files.rs` | `open_skill_root` | 从 `/` 逐段 `openat`，终局校验走到的就是目标目录 |
| `crates/goose/src/skills/supporting_files.rs` | `open_at` | `openat` 封装，拒绝含 NUL 的名字 |
| `crates/goose/src/skills/supporting_files.rs` | `read_confined_file_with_hook` | 逐组件走查 + 最终组件 `O_NOFOLLOW` + `is_file()` |
| `crates/goose/src/skills/supporting_files.rs` | `after_opened_component` | 供测试注入竞态的钩子，证明目录 fd 走查免疫 TOCTOU |
| `crates/goose/src/skills/supporting_files.rs` | `read_utf8_with_limit` / `max_utf8_bytes` | 乘 4 上界 + 多读 1 字节探测溢出 |
| `crates/goose/src/skills/supporting_files.rs` | `write_confined_file_with_hook` | 写路径：`O_EXCL` / 截断顺序 / 类型检查 |
| `crates/goose/src/skills/supporting_files.rs` | `ensure_source_file_has_single_link` | 写路径的硬链接防御（`nlink == 1`） |
| `crates/goose/src/skills/supporting_files.rs` | `MAX_SOURCE_FILE_BYTES` | 源文件上限，取自调度器的配方字节上限 |

## 参考

- [goose 源码：skills/supporting_files.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/skills/supporting_files.rs)
- [goose 源码：skills/builtin.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/skills/builtin.rs)
- [Linux man-pages：openat 与其旗标](https://man7.org/linux/man-pages/man2/openat.2.html)
- [CWE-59：路径等价与链接解析（Link Following）](https://cwe.mitre.org/data/definitions/59.html)

## 与主核 / AI 的关系

这套围栏与主核里「限制 COPY 只能读数据目录」是同一类机制，而主核通常做得更粗——一次 `realpath` 加前缀比较。`realpath` 的问题正是这里用 `openat` 解决的那个：**检查和打开之间有窗口**。数据库因为通常是单用户本地进程而较少被利用，而一个 agent 技能目录里的文件可能来自用户下载的第三方技能包，风险等级完全不同。

对 agent 工程的直接价值有三条。其一，**信任边界要交给内核，不要交给字符串**。`openat` + `O_NOFOLLOW` 让「路径前段被换掉」失效，而 `realpath` 之后的字符串比较在这个窗口里是假的。其二，**读与写的防护强度应当不同**。硬链接对读几乎无害、对写是致命的（会改到目录外的另一个名字），所以 `nlink == 1` 只在写路径检查——这比「两边都加」更省，也比「都不加」正确。其三，**不支持就拒绝，别退化**。在没有等价原语的平台上用「先规范化再打开」会让安全属性变成尽力而为，而这恰恰是最容易被误认为「已经做了」的那类事。
