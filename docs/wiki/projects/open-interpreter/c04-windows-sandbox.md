---
project: open-interpreter
feature: windows-sandbox
title: Windows 受限令牌沙箱
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/sandboxing/src/windows.rs
    symbols: [WindowsSandboxFilesystemOverrides, windows_sandbox_uses_elevated_backend, resolve_windows_restricted_token_filesystem_overrides]
  - path: codex-rs/sandboxing/src/windows.rs
    symbols: [permission_profile_supports_windows_restricted_token_sandbox, has_reopened_writable_descendant]
---

# Windows 受限令牌沙箱

## 一句话职责

Windows 上无法用「策略文本」或「文件系统视图」表达限制，只能靠**进程令牌**。这个模块的职责是把一份权限档案判定成「当前后端能不能表达它」，能表达就翻译成一组覆盖参数，不能表达就返回一条明确的拒绝。

## 机制是什么

Windows 的访问控制模型与前两个平台根本不同：

| 平台 | 限制手段 | 表达什么 |
|---|---|---|
| macOS | SBPL 策略文本 | 按操作类别 + 路径/参数匹配 |
| Linux | bwrap 视图重排 + Landlock | 换一个文件系统视图 |
| Windows | 进程令牌（SID + 权限位）+ ACL | 进程身份决定它能碰什么 |

令牌模型的关键后果是：**限制不是「声明」的，是「减去」的**。你把令牌里的 SID 去掉、把权限位关掉，能做的就少了。但「少做某些事」的能力取决于**检查侧怎么用这些 SID**——而这在不同实现里不一样。

这就是这一页最核心的技术事实，写在源码注释里：

> The restricted-token backend can still enforce split write restrictions, but its `WRITE_RESTRICTED` token does not make capability SID deny-read ACEs participate in read access checks. Read restrictions therefore require the elevated backend, even when the filesystem root remains readable.

即：**受限令牌能减写，但减不了读**。要减读必须换提权后端。所以 Windows 有**两个后端**，且选择依据不是性能偏好，而是「这份策略需要什么表达能力」。

于是这个模块的主线是：判定 → 若当前后端表达不了 → 拒绝（而不是退回不套沙箱）。

## 怎么实现的

**两个后端的选择只由一个布尔量表达。**

```rust
pub fn windows_sandbox_uses_elevated_backend(sandbox_level: WindowsSandboxLevel) -> bool {
    matches!(sandbox_level, WindowsSandboxLevel::Elevated)
}
```

`WindowsSandboxLevel` 是上游协议的类型，这里只关心是不是 `Elevated`。这个函数被 `unsupported_windows_restricted_token_sandbox_reason` 用来分派到两个不同的解析函数：

```rust
if windows_sandbox_level == WindowsSandboxLevel::Elevated {
    resolve_windows_elevated_filesystem_overrides(...).err()
} else {
    resolve_windows_restricted_token_filesystem_overrides(...).err()
}
```

注意它返回的是 `.err()`——这个函数的语义是「**如果不支持，原因是什么**」。所以两个解析函数的设计是「成功返回 `Option<Overrides>`，失败返回 `Err(String)`」，而外层只关心错误那半。这让「能否支持」与「不支持的具体原因」在同一个调用里得到。

**能力判定的第一层：这份权限档案本身能不能被令牌模型表达。**

```rust
pub fn permission_profile_supports_windows_restricted_token_sandbox(permission_profile: &PermissionProfile) -> bool {
    match permission_profile {
        PermissionProfile::Managed { file_system, .. } => !file_system.to_sandbox_policy().has_full_disk_write_access(),
        PermissionProfile::Disabled | PermissionProfile::External { .. } => false,
    }
}
```

三支：

- `Managed` 且**不是全盘可写** → 可能支持（还要看后面的细化判定）。
- `Managed` 且全盘可写 → 不需要沙箱，所以这条判定为假（但注意它并不意味着「不支持」，而是「没东西可限制」，后面会走 `Ok(None)`）。
- `Disabled` / `External` → 假。`Disabled` 是明确关掉沙箱，`External` 是外部代管（策略由外部系统施加），两者都不该由这里再加一层。

**能力判定的第二层：读限制必须换提权后端。**

```rust
if !windows_policy_has_root_read_access(&file_system_sandbox_policy, sandbox_policy_cwd) {
    return Err("windows unelevated restricted-token sandbox cannot enforce split filesystem read restrictions directly; refusing to run unsandboxed".to_string());
}
```

`windows_policy_has_root_read_access` 判的是「这份策略还能读到文件系统根吗」——它拿 `cwd` 的**最顶层祖代**作为根，问策略能不能读它。

判据很巧妙：**若连根都读不到，说明这份策略里有「禁止读某些路径」的意图**（比如「允许读工作区，不允许读 `/etc`」会让根不可读）。而受限令牌减不了读，所以这种策略它表达不了。

紧接着还有第二道更直接的检查：

```rust
let additional_deny_read_paths = codex_windows_sandbox::resolve_windows_deny_read_paths(&file_system_sandbox_policy, sandbox_policy_cwd)?;
if !additional_deny_read_paths.is_empty() {
    return Err("windows unelevated restricted-token sandbox cannot enforce deny-read restrictions directly; refusing to run unsandboxed".to_string());
}
```

即：**只要算出了任何「需要额外 deny 的读路径」，就拒绝**。这与前一道是同一件事的两个角度——前者判「根不可读」，后者判「是否存在具体的 deny 读条目」。两者都命中同一条结论：「我减不了读」。

注意哪怕根仍可读，只要有具体 deny 读条目也会被第二道拦下。第一道是快速路径（根不可读时不必再算），第二道是精确路径。

**第三层：可写根集合必须与「旧投影」一致。**

```rust
let legacy_projection = compatibility_sandbox_policy_for_permission_profile(permission_profile, sandbox_policy_cwd.as_path());
let legacy_writable_roots = legacy_projection.get_writable_roots_with_cwd(sandbox_policy_cwd);
let split_writable_roots = file_system_sandbox_policy.get_writable_roots_with_cwd(sandbox_policy_cwd);

let legacy_root_paths: BTreeSet<PathBuf> = legacy_writable_roots.iter().map(|r| normalize_windows_override_path(r.root.as_path())).collect::<Result<_, _>>()?;
let split_root_paths: BTreeSet<PathBuf> = split_writable_roots.iter().map(|r| normalize_windows_override_path(r.root.as_path())).collect::<Result<_, _>>()?;

if legacy_root_paths != split_root_paths {
    return Err("windows unelevated restricted-token sandbox cannot enforce split writable root sets directly; refusing to run unsandboxed".to_string());
}
```

这里出现了一个重要概念：**旧投影（legacy projection）**。`compatibility_sandbox_policy_for_permission_profile` 把权限档案投影成一套旧的、较粗的沙箱策略——注释在别处解释过，Windows 的已有机制（`legacy writable root projection`）保护的是这套形态。

所以判定的逻辑是：**若新的细分策略能投影到旧的形态且根集合一致，就用旧的机制承载它**；否则拒绝。这是一条「用能力较弱但已经存在的机制承载新策略」的典型手法——能让就用，不能让就明说。

用 `BTreeSet` 而不是逐个比较，是为了让「顺序不同但内容相同」不被误判。而 `normalize_windows_override_path` 里用了 `dunce::simplified`——Windows 路径规范化容易出问题（`\\?\` 前缀、大小写、`/` 与 `\`），`dunce` 处理这些。

**第四层：`has_reopened_writable_descendant`——一个函数名本身就是攻击面描述。**

这个函数在两个后端里都被调用，且在受限令牌后端里是**内联展开**的等价逻辑（那里直接写了三重循环）。函数体是：

```rust
writable_roots.iter().any(|writable_root| {
    writable_root.read_only_subpaths.iter().any(|read_only_subpath| {
        writable_roots.iter().any(|candidate| {
            candidate.root.as_path() != writable_root.root.as_path()
                && candidate.root.as_path().starts_with(read_only_subpath.as_path())
        })
    })
})
```

读成一句话：**存在某个可写根 A，它的某个只读子路径 R 之下，还压着另一个可写根 B**。

为什么这是问题？因为 Windows 的机制是按「可写根 + 其下的只读子路径」建模的。若 A 可写、A/R 只读，那么 B（在 A/R 之下）**本该只读**，但它被单独列为可写根。于是同一块地方有两条互相矛盾的规则，而 Windows 的 ACL 施加方式是「先设可写根、再设只读子路径」，顺序决定结果——若先把 B 设为可写、再把 A/R 设为只读，B 就被盖住了；反之 B 就重开了 A/R。

源码的选择不是「挑一个顺序」而是**拒绝这个策略**：

```
windows unelevated restricted-token sandbox cannot reopen writable descendants under read-only carveouts directly; refusing to run unsandboxed
```

这条拒绝的理由值得单想：**宏展开成 ACL 时，嵌套的可写 carveout 无法在不产生歧义的前提下表达**。所以在提权后端的版本里，同样的判定被保留（`has_reopened_writable_descendant`），只是文案换了：

```
windows elevated sandbox cannot reopen writable descendants under read-only carveouts directly; refusing to run unsandboxed
```

注意两个后端的拒绝理由**只差后端的名字**，说明这条限制是 ACL 表达能力的限制，而不是受限令牌特有的限制。这是一个很有信息量的细节：提权后端能减读，但仍然不能表达「只读 carveout 里再开一个可写洞」。

**第五层（仅受限令牌后端）：把「细分的只读子路径」降级成 deny 写路径。**

这一段的做法是：把新策略的每个可写根，与旧投影里的对应根配对；对新策略里「旧投影没覆盖到」的只读子路径，收集成 `additional_deny_write_paths`：

```rust
for read_only_subpath in &split_root.read_only_subpaths {
    if !legacy_root.read_only_subpaths.iter().any(|c| c == read_only_subpath) {
        additional_deny_write_paths.insert(normalize_windows_override_path(read_only_subpath.as_path())?);
    }
}
```

即**用「拒绝写这个路径」作为「只读」的近似**。这在令牌模型里是可行的（减写能减），所以这是唯一一处「降级但语义等价」的地方。其余降级都是拒绝。

**提权后端的路径则是另一套判定。** `resolve_windows_elevated_filesystem_overrides` 的流程与受限令牌相似，但关键分支不同：

1. 仍先 `remove_skip_missing_path_entries()`，注释与受限令牌那边一字不差：「Windows protects existing metadata paths through the legacy writable root projection. Do not turn skip-missing entries into newly-created deny-write sentinels」。即**别把「跳过不存在的路径」变成「创建一个 deny 写哨兵」**——那会在宿主上凭空造出文件，与 c03 里 bwrap 必须建挂载点的做法形成对照（那边是必要的，这边是有害的）。
2. `has_reopened_writable_descendant` 仍然拦。
3. **不再拦 deny 读**——因为提权后端能减读。
4. 计算三个输出：`read_roots_override`、`write_roots_override`、`additional_deny_write_paths`，各按「与旧投影是否一致」决定要不要给覆盖。

`read_roots_override` 的判据有一段很值得读的注释：

> `has_full_disk_read_access()` is intentionally false when deny-read entries exist. For Windows setup overrides, the important question is whether the baseline still reads from the filesystem root and only needs additional deny ACLs layered on top.

即：不要问 `has_full_disk_read_access()`（它会因为存在 deny 读而返回假），而要问「基线是否仍从根开始读，只是额外叠 deny ACL」。这就是 `windows_policy_has_root_read_access` 的用途。若根仍可读，`read_roots_override` 为 `None`（沿用默认的根）；否则给出具体可读根清单。

`write_roots_override` 与受限令牌后端相反：这里若根集合**一致**就给 `None`（沿用旧投影），不一致才给具体集合。而受限令牌后端是「不一致就拒绝」。这个差异的根源就在能力——提权后端能改变可写根集合，受限令牌不能。

**`additional_deny_write_paths` 的取舍比受限令牌版更细。** 提权版只在两种情况下才算这批路径：

```rust
if needs_direct_runtime_enforcement || has_explicit_write_carveouts { ... }
```

`needs_direct_runtime_enforcement` 是「策略里有些东西必须运行期直接施加」；`has_explicit_write_carveouts` 是「某个只读子路径在策略里被显式地标成非写」。两个都没有，就完全不算（返回空集）。

算的时候还多一层「按后缀比较」的规范化：

```rust
let read_only_subpath_suffix = read_only_subpath.as_path().strip_prefix(writable_root.root.as_path()).ok();
let already_denied_by_legacy = legacy_root.is_some_and(|lg| lg.read_only_subpaths.iter().any(|c| {
    c.as_path().strip_prefix(lg.root.as_path()).ok() == read_only_subpath_suffix
}));
```

即比较的是**相对根的后缀**而不是绝对路径。为什么要这样？因为新旧投影的根路径在规范化后可能不完全相等（符号链接、大小写、`\\?\` 前缀），但「根目录下第几层是什么」是稳定的。用后缀比较能容忍这些差异。

然后判据是 `explicitly_configured || !already_denied_by_legacy`：**显式配置的一定要加，旧投影已经拒绝的不用重复加**。

**返回值的形状在两个后端共用。**

```rust
pub struct WindowsSandboxFilesystemOverrides {
    pub read_roots_override: Option<Vec<PathBuf>>,
    pub read_roots_include_platform_defaults: bool,
    pub write_roots_override: Option<Vec<PathBuf>>,
    pub additional_deny_read_paths: Vec<AbsolutePathBuf>,
    pub additional_deny_write_paths: Vec<AbsolutePathBuf>,
}
```

五个字段里三个是 `Option`，语义统一：**`None` 表示「沿用旧投影的默认值」，`Some` 表示「覆盖它」**。这个约定让「不需要覆盖」与「要覆盖成空」能区分开——后者是一个合法状态（什么都不许读）。

字段类型的差异也值得注意：根的覆盖用 `Vec<PathBuf>`（相对/绝对都可），而 deny 路径用 `AbsolutePathBuf`（强制绝对）。因为 deny 要喂给 ACL 设置，必须绝对；而根可以从旧投影的形状继承。

`read_roots_include_platform_defaults` 是唯一的布尔字段，注释在类型文档里说明：「split policies that opt into platform defaults carry that explicitly with the override」。即「是否加平台默认读根」这件事不能靠猜，必须显式携带。

**与 `manager.rs` 的接线处还有一层 Windows 专用的环境白名单**（见 c01），以及直接 spawn 路径的额外包装。这一篇只讲它自己的判定，接线细节留给 c01。

## 关键取舍

**源码事实：** Windows 有两个后端——受限令牌（非提权）与提权。选择由 `WindowsSandboxLevel::Elevated` 单一判定，而理由不是性能而是**表达能力**：`WRITE_RESTRICTED` 令牌不让 capability SID 的 deny-read ACE 参与读检查，所以「减写」可行、「减读」必须换提权后端。

**源码事实：** 受限令牌后端对四类情形一律拒绝而不是降级：根不可读、存在具体 deny 读路径、可写根集合与旧投影不一致、存在「只读 carveout 之下再开可写洞」的嵌套。

**源码事实：** 唯一被允许的「降级」是：把新策略里旧投影未覆盖的只读子路径，转成 `additional_deny_write_paths`（拒绝写 ≈ 只读）。

**源码事实：** `has_reopened_writable_descendant` 在两个后端里都拦同一条：某个可写根的只读子路径之下还压着另一个可写根。两处错误文案只差后端名，说明这是 ACL 表达能力的限制而非令牌特有。

**源码事实：** 两个后端都调 `remove_skip_missing_path_entries()`，理由相同：Windows 已有机制保护既有元数据路径，不该把「跳过不存在的路径」变成「在宿主上新建 deny 写哨兵」。

**源码事实：** 提权后端按「根是否仍可读」而非 `has_full_disk_read_access()`（后者因存在 deny 读而返回假）来决定是否需要 `read_roots_override`。

**源码事实：** 提权后端比较只读子路径时用**相对根的后缀**而非绝对路径，以容忍规范化差异。

**架构推论：** 这一页最值得带走的一条是「**能力不足时拒绝，不做近似降级**」。四类拒绝都在做同一件事：把「我做不到」变成一条可诊断的错误，而不是一个看起来在工作但实际更宽松的沙箱。唯一的例外（deny 写代替只读）之所以被允许，是因为它在令牌模型下**语义等价**，而不是「差不多」。

**架构推论：** 「旧投影」这个概念揭示了一种常见的迁移结构：新策略比旧机制表达得更细，代码的工作是**判定新策略能否投影到旧机制**，能就投影、不能就拒绝。这让新策略的引入不必等旧机制升级，代价是一张清单——哪些组合能投影、哪些不能。

**架构推论：** `has_reopened_writable_descendant` 这个函数名本身是一份攻击面说明书。它的命名没有用「is_nested_writable」「check_write_carveout_overlap」这类中性的名字，而是直接说明**这是一个「重开」动作**——「重开可写后代」意味着绕过了一个本该生效的只读限制。这种命名方式让代码审查时能立刻意识到这是安全相关的判定。

**架构推论：** 用「后缀比较」而不是绝对路径比较，是这个模块里最不显眼但很实用的一处。Windows 路径在规范化后可能出现多种合法表示（大小写、分隔符、`\\?\` 前缀、符号链接解析差异），直接比较绝对路径会误判「不一致」从而触发拒绝。而比较后缀能容忍这些，因为「根之下的相对位置」是稳定的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/sandboxing/src/windows.rs` | `WindowsSandboxFilesystemOverrides` | 覆盖参数：三个 `None` 表示沿用默认的字段 + 两类 deny 路径 |
| `codex-rs/sandboxing/src/windows.rs` | `windows_sandbox_uses_elevated_backend` | 由 `WindowsSandboxLevel` 判定后端 |
| `codex-rs/sandboxing/src/windows.rs` | `permission_profile_supports_windows_restricted_token_sandbox` | 能力第一层：档案本身能否被令牌表达 |
| `codex-rs/sandboxing/src/windows.rs` | `unsupported_windows_restricted_token_sandbox_reason` | 分派到两个后端解析函数并只取错误 |
| `codex-rs/sandboxing/src/windows.rs` | `resolve_windows_restricted_token_filesystem_overrides` | 四类拒绝 + 一处语义等价的降级 |
| `codex-rs/sandboxing/src/windows.rs` | `resolve_windows_elevated_filesystem_overrides` | 提权后端：可改可写根集合，按根可读性决定读覆盖 |
| `codex-rs/sandboxing/src/windows.rs` | `has_reopened_writable_descendant` | 「只读 carveout 之下再开可写洞」的判定 |
| `codex-rs/sandboxing/src/windows.rs` | `windows_policy_has_root_read_access` | 判「基线是否仍从根读」，替代被 deny 读污染的 `has_full_disk_read_access` |
| `codex-rs/sandboxing/src/windows.rs` | `normalize_windows_override_path` | `dunce::simplified` + 绝对化的路径规范化 |
| `codex-rs/sandboxing/src/windows.rs` | `permission_profile_display_name` | 拒绝文案里的档案类型名 |

## 参考

- [open-interpreter 源码：sandboxing/src/windows.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/sandboxing/src/windows.rs)
- [Microsoft 文档：受限令牌](https://learn.microsoft.com/en-us/windows/win32/secauthz/restricted-tokens)
- [Microsoft 文档：访问控制列表与 ACE](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-control-lists)
- [Microsoft 文档：WRITE_RESTRICTED SID](https://learn.microsoft.com/en-us/windows/win32/secauthz/sid-attributes-in-an-access-token)

## 与主核 / AI 的关系

「用能力不足的机制承载新策略时，判定能否投影、否则拒绝」这件事，与主核里的**降级执行**有直接对照。主核有时必须在不支持某特性的副本上执行（例如没有并行查询的旧版本），做法是判定「这个计划能否降级成串行版本」——能就降级、不能就报错，而不是「尽力并行」。

`has_reopened_writable_descendant` 那类「嵌套规则互相矛盾」的问题，与主核里**行级安全策略与约束的交互**同源：一条策略允许写、一条约束禁止写同一个地方，结果取决于施加顺序。主核对此的做法是显式定义优先级；这里的选择是直接拒绝这种配置，因为 ACL 施加顺序产生的歧义无法向用户解释。

「`None` 表示沿用默认、`Some` 表示覆盖」这个约定，与主核里**配置项的三态**（未设置 / 显式设为默认 / 显式设为其他）一致。把「没有意见」与「明确要求默认值」分开，是配置系统的通用要求。

对 AI 工程的实际意义在于：Windows 是三个平台里最难做沙箱的，而它的限制**看起来**很接近「够用」。这份实现的价值在于把「不够用的地方」逐条登记成拒绝：减不了读、改不了可写根集合、表达不了嵌套 carveout、不能把 skip-missing 变成哨兵。做跨平台 agent 时最该学的是这个态度——**平台差异不是配置差异，有些差异必须变成明确的「此处不支持」，否则用户会在一个自以为受保护的沙箱里运行不可信代码**。
