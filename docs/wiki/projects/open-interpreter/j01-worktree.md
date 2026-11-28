---
project: open-interpreter
feature: worktree
title: 工作树管理与归属记录
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/worktree/src/lib.rs
    symbols: [WorktreeManager, CreateWorktree, is_safe_worktree_cwd]
  - path: codex-rs/worktree/src/metadata.rs
    symbols: [OwnerRecord, OWNER_FILENAME, bind_thread]
---

# 工作树管理与归属记录

## 一句话职责

把「另开一份工作副本」做成一条可回滚的事务：分配一个四位十六进制桶、`worktree add --detach --no-checkout`、只给目标写配置、`reset --hard` 填充，任何一步失败就把已建的东西删干净；同时在一个版本化的归属记录里声明「这份工作树属于哪个线程」。

## 机制是什么

`WorktreeManager` 只有一个字段 `settings: WorktreeSettings`，构造时做一件事：`settings.root = dunce::simplified(&settings.root).to_path_buf()`。

`CreateWorktree` 两字段：`source_cwd: PathBuf` 与 `base: Option<String>`。`ManagedWorktree` 六字段：`root`、`cwd`、`source_root`、`source_cwd`、`head_sha`、`branch: Option<String>`。`branch` 上的 doc 特意澄清：「可用的 UTF-8 分支标签；缺失**不**意味着 detached HEAD」——因为这个函数产出的是 `--detach` 检出，`branch` 恒为 `None`，所以「没有 branch 字段」不代表「这一定是游离状态」。

归属记录在 `metadata.rs`：`OWNER_FILENAME = "codex-thread.json"`、`OWNER_VERSION: u8 = 1`，`OwnerRecord` 两字段 `version` 与 `owner_thread_id`，且带 `#[serde(rename_all = "camelCase", deny_unknown_fields)]`。

## 怎么实现的

**创建是一条四阶段流水，每阶段失败都回滚。** `create` 的顺序：

1. **前置校验与解析**：`settings.root` 必须是绝对路径；`source_cwd` 走 `dunce::canonicalize`；`repository_root(&source_cwd)` 得到仓库根；`relative_cwd = source_cwd.strip_prefix(&source_root)`；`repository_name = source_root.file_name()`；`revision = "{base 或 HEAD}^{commit}"`；`head_sha = git rev-parse --verify --end-of-options <revision>`。

2. **分配桶**：`allocate_worktree_root(&settings.root, repository_name)`。

3. **`worktree add`**：`--detach --no-checkout <root> <head_sha>`。失败则 `remove_empty_bucket(&root)` 并返回错误。

4. **配置与填充**：先 `rev-parse --path-format=absolute --git-path config.worktree` 拿到目标侧的配置文件路径，然后对它 `git config --file <config> core.worktree <root>`，再 `git --work-tree=. reset --hard --no-recurse-submodules <head_sha>`。失败则 `remove_worktree(&source_root, &root)` 回滚（注意这里用 `?` 传播回滚本身的错误，见下）。

5. **cwd 安全校验**：`cwd = root.join(relative_cwd)`，然后 `is_safe_worktree_cwd(&root, &cwd)`。不通过就 `remove_worktree` 再 `bail!`。

第 5 步的错误消息点明了一个真实场景：「请求的 base 不包含安全的工作目录」。如果用户从 `repo/src/foo` 请求工作树，而 `base` 是一个早期的提交（那时 `src/foo` 还不存在），那么 `root/src/foo` 根本不存在——工作树建成了但 cwd 是无效的。校验加回滚把这个「半成品」清掉。

**`is_safe_worktree_cwd` 是「两个 canonicalize 加一个 starts_with」。** 实现：

```rust
let Ok(root) = dunce::canonicalize(root) else { return false; };
dunce::canonicalize(cwd).is_ok_and(|resolved| resolved.is_dir() && resolved.starts_with(&root))
```

三个条件：两路径都能 canonicalize、cwd 解析后是目录、且**解析后**的 cwd 仍在 root 之下。

第三个条件是关键：它比的是 canonicalize 之后的路径，所以任何符号链接把 cwd 引到 root 之外都会被拒。若只比原始路径（`root.join(relative_cwd) == cwd` 之类），一个 `relative_cwd` 里的符号链接就能带出边界。所以这是「先解析再判包含」，而不是「先判包含再解析」。

**`reset --hard` 前必须拿到 `config.worktree` 路径。** 那两条 git 命令的顺序与内容都很讲究：

```rust
git rev-parse --path-format=absolute --git-path config.worktree
git config --file <config> core.worktree <root>
git --work-tree=. reset --hard --no-recurse-submodules <head_sha>
```

源码里那句注释解释了这一组：「只写目标的配置，不改共享设置，也不为源仓库启用 `worktreeConfig`」。

所以这里是**避免污染源仓库**的一种手法：正常的 `worktree add` 配 `--no-checkout` 之后，若要手动填充文件，通常需要设置 `extensions.worktreeConfig = true`，而那是源仓库的共享配置——改它会影响源仓库里所有工作树的行为。这里改成直接把 `core.worktree` 写进目标侧的 `config.worktree` 文件，绕开了共享配置。

而 `git --work-tree=. reset --hard` 里的 `--work-tree=.` 是为「每工作树配置被禁用」的情况准备的——注释写「即使每工作树配置被禁用，也把工作树钉住」。所以即使 `core.worktree` 那一步没生效，这条命令仍然能正确填充。

`--no-recurse-submodules` 说明子模块不会被初始化。这是一个明确的取舍：初始化子模块慢且可能失败，而 agent 通常先只需要主仓库的文件。

**`list` 用 porcelain -z 加六道过滤。** 它的流程：canonicalize `source_cwd` → `repository_root` → `relative_cwd` → `managed_root` 的 canonicalize（失败就用原值）→ `source_common_dir = resolve_git_path(&source_root, "--git-common-dir")` → `git worktree list --porcelain -z`。

然后按 `\0` 切字段、按空字段再切组（一组 = 一个工作树），每组里取 `worktree <path>`、`HEAD <sha>`、`branch <refs/heads/...>`。

取完之后是一串 `continue` 过滤，每一道都对应一个真实的不该列出的情形：

1. **没有 `worktree` 字段** → 跳过（porcelain 的某些记录形态）。
2. **prefix 必须都是目录** ——注释：「让别名不能改变哪个注册提供了元数据」。用 `fs::symlink_metadata` 而不是 `metadata`，所以比的是路径本身的类型而不是它指向的目标。这一条防的是「一个符号链接让同一个工作树以两个不同路径出现」。
3. **`has_managed_layout(&managed_root, &canonical_root)`** ——必须落在 `<managed_root>/<四位十六进制>/<repository_name>` 这个形状里。
4. **`.git` 必须是常规文件**（`file_type().is_file()`）——链接工作树的 `.git` 是一个文件（内容是 `gitdir: ...`），而不是目录。这一条把「主工作树」与「链接工作树」分开。
5. **`linked_worktree_common_dir` 必须等于源仓库的 common dir** ——否则这个工作树属于另一个仓库。
6. **backlink 必须指回当前 checkout** ——注释：「另一个链接检出可能占据一个陈旧注册的路径。要求它的管理目录指回这个 checkout」。

第 6 步的实现值得细看：读 `git_dir/gitdir` 文件内容，去掉尾部的 `\n`（Windows 上再去掉 `\r`），解析成路径，canonicalize 后必须等于 `canonical_root/.git` 的 canonicalize 结果。

这一条防的是 git 的「陈旧注册」问题：一个工作树被 `rm -rf` 掉之后，它的记录还留在主仓库的 `worktrees/` 目录里；若之后在那个路径上新建了一个**不同**的工作树，`worktree list` 会显示两条指向同一路径的记录，而其中一条的管理目录指向已删除的旧内容。要求 backlink 指回自己，就能把那条陈旧的排除掉。

7. **cwd 安全校验** ——`is_safe_worktree_cwd(&canonical_root, &cwd)`，不通过跳过（因为工作树建于一个不含该目录的 base 时会出现）。

最后 `head_sha` 用 `with_context` 要求存在（「managed worktree {path} 没有 HEAD」）。所以「有 HEAD 字段」是列出的必要条件。

`worktree list --porcelain -z` 用 `-z` 而不是默认的换行分隔，是为了让**路径里的空格与换行**不再有歧义。porcelain 的换行格式在遇到含空格的路径时会出现引号转义问题，`-z` 用 NUL 分隔就彻底避免了。这是一个很实用的细节。

**bucket（四位十六进制）与 `has_managed_layout` 的判据。** `allocate_worktree_root` 取一个 v4 UUID 的 simple 形式（`Uuid::new_v4().simple().to_string()`，即无连字符的十六进制），取**前四个字符**作为桶名，`create_dir` 成功就返回 `bucket.join(repository_name)`。

所以最终布局是 `<root>/<4-hex>/<repo-name>`。`has_managed_layout` 正是反向验证这个形状：

```rust
bucket.len() == 4 && bucket.bytes().all(|byte| byte.is_ascii_hexdigit())
&& matches!(components.next(), Some(Component::Normal(_)))
&& components.next().is_none()
```

四字符、全 hex、然后再有正好**一个**普通组件、没有更多。所以「四字符 + 名字」两层且无剩余，才算受管布局。

这个「四位十六进制」的选择看起来随意，实际信息量是 65536 个桶——而循环上限是 `0..=u16::MAX`，正好对应。所以「桶用尽」的判据与桶空间严格一致：`bail!("all managed worktree identifiers are in use")` 只在 65536 次 `AlreadyExists` 之后才出现。

用 `create_dir`（不是 `create_dir_all`）作为「占位」手段，是因为它原子地表达「这个桶名归我了」。若只用 `exists()` 判断再建，两个并发创建会撞在同一个桶上。所以 `AlreadyExists` 被当作「继续试下一个」，而不是错误。

**macOS 上写 `.metadata_never_index`。** 那一段 `#[cfg(target_os = "macos")]` 在根目录写一个空文件，注释说「禁用 Spotlight 索引」。这个标记文件的命名约定来自 Spotlight：`.metadata_never_index` 让该目录不被索引。理由很实际——工作树目录会被频繁创建销毁，每份都是完整的代码副本；让 Spotlight 去索引它们会持续消耗 I/O 与 CPU，且结果没有价值。

这一段在循环外（只写一次），所以每次 `create` 都会尝试重写一遍（`fs::write` 覆盖）。幂等。

**`remove_empty_bucket` 只删空的。** 用 `fs::remove_dir`（不是 `remove_dir_all`）——若桶里还有别的东西（比如另一个仓库的工作树），删除会失败并被 `let _ =` 静默忽略。所以「清理桶」是尽力而为，且天然安全：它只删空目录。

**归属记录是「原子发布 + 不覆盖」。** `bind_thread` 的实现四步：

1. `thread_id` 非空校验。
2. `if let Some(existing) = owner(checkout)?` —— 已有记录时：同 id 就返回 `Ok(())`（幂等），不同 id 就 `bail!("worktree already belongs to thread {existing}")`。
3. 建 `NamedTempFile::new_in(directory)` 写 JSON 并 `flush`。
4. `temporary.persist_noclobber(&path)` —— 原子发布且**不覆盖**。

第 4 步的 `AlreadyExists` 分支是竞态的收尾：「并发的另一个绑定先发布了」。此时重新 `owner(checkout)`，若已是同一个线程就 `Ok(())`（说明两个请求恰好都来自同一个线程，结果正确），否则 `bail!("worktree was concurrently assigned to another thread")`。

所以整个流程是「先查、再原子写、写完发现被别人抢先、再查一遍确认」——这是无锁文件系统的标准乐观并发写法。第 2 步的预查只是为了给出友好错误（省掉一次临时文件的创建），真正的正确性来自 `persist_noclobber` 的原子性。

模块 doc 把这个契约写得更清楚：「绑定以原子方式发布记录，不替换另一个所有者；再次绑定同一个线程时保持既有记录不变」。

**`OWNER_VERSION` 与 `deny_unknown_fields` 是两个不同的兼容方向。** `OwnerRecord` 带 `deny_unknown_fields` ——所以「记录里多出字段」会被拒绝（因为 `version` 明确标了 1，多出的字段来自一个更新的版本，旧代码不该猜它的语义）。而 `owner()` 里的 `if record.version != OWNER_VERSION { bail!(...) }` 是同一道检查的另一半：版本号不匹配直接报错。

所以这份记录是**严格版本化**的：读不懂就报错，不做猜测性解析。这与 `ModelsCacheEntry` 那些 `#[serde(default)]` 的宽容做法（见 h03）方向相反——因为那两处的后果不同：缓存读不懂可以当未命中，而归属记录读不懂意味着「无法确认这份工作树属于谁」，那必须停下。

`OwnerRecord` 的字段记得很清楚：`owner_thread_id: String` 而不是某种 id 类型。所以这层不解析 id 的语义，只把它当作不透明字符串保存——跨版本时不会因为 id 类型演化而失效。

**`metadata_path` 把记录放进 Git 元数据目录。** 实现：

```rust
let path = git_path(checkout, ["rev-parse", "--git-path", OWNER_FILENAME])?;
Ok(checkout.join(path))
```

用 `--git-path` 而不是直接拼 `.git/` 下——因为链接工作树的 `.git` 是文件，实际的元数据目录在别处（`git-dir`）。让 git 自己报出路径，是唯一正确处理这个布局的方式。

这也意味着记录**不在工作区里**（不在 `root` 的可见文件树里），所以 `git status` 不会看到它，用户的 `git add .` 也不会误提交它。这是一个必要的选择：归属记录是工具的元数据，不该污染被管理的仓库。

## 关键取舍

**源码事实：** 创建工作树是四阶段流水，第 3、4、5 阶段各自失败都有对应的回滚（删桶、`worktree remove --force`、先删再报错）。

**源码事实：** 只有目标的 `config.worktree` 被写 `core.worktree`，源仓库的共享配置与 `extensions.worktreeConfig` 不被改。

**源码事实：** `reset --hard` 带 `--work-tree=.` 与 `--no-recurse-submodules`。

**源码事实：** 桶名是 UUID v4 前四位十六进制，用 `create_dir` 的 `AlreadyExists` 做占位，上限 `u16::MAX` 与桶空间一致。

**源码事实：** `list` 用 `--porcelain -z`（NUL 分隔），并对每条记录做六道过滤，含 backlink 回指。

**源码事实：** `is_safe_worktree_cwd` 比的是 canonicalize 之后的路径，所以符号链接逃逸会被拒。

**源码事实：** 归属记录用 `NamedTempFile` + `persist_noclobber` 原子发布，`AlreadyExists` 时重读确认。

**源码事实：** `OwnerRecord` 严格版本化（版本号不符即报错）且 `deny_unknown_fields`；`owner_thread_id` 是不透明字符串。

**架构推论：** 「创建多步资源」的正确形状是「每步都有对应的撤销动作」，而不是「最后统一清理」。这里三个阶段有各自的回滚，且回滚的粒度精确（删桶 vs `worktree remove`）。统一清理往往删多了或删漏了——因为它不知道「哪些东西已经真的建立了」。

**架构推论：** 「不改共享配置」这条约束的重要性一直被低估。`worktreeConfig` 是源仓库级的开关，改动它会静默改变源仓库里**所有**工作树的行为。绕开它、只写目标侧配置，是在多租户（多工作树）环境下不引入交叉影响的唯一办法。

**架构推论：** `list` 那六道过滤的共同主题是「不要相信 git 的记录，要验证它」。陈旧注册、符号链接别名、跨仓库的同路径、被替换的 checkout——这些都不是 git 的 bug，而是「记录与当前文件系统状态可能不一致」这个基本事实的各种表现。读一份可能过期的索引时，必须用当前状态反验它。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/worktree/src/lib.rs` | `WorktreeManager` | 工作树创建、列举与释放的主体 |
| `codex-rs/worktree/src/lib.rs` | `CreateWorktree` | 源目录与可选 base 的创建请求 |
| `codex-rs/worktree/src/lib.rs` | `ManagedWorktree` | 受管工作树的六字段描述 |
| `codex-rs/worktree/src/lib.rs` | `WorktreeManager.create` | 四阶段流水与逐阶段回滚 |
| `codex-rs/worktree/src/lib.rs` | `WorktreeManager.list` | porcelain -z 解析与六道过滤 |
| `codex-rs/worktree/src/lib.rs` | `is_safe_worktree_cwd` | 解析后再判包含，防符号链接逃逸 |
| `codex-rs/worktree/src/lib.rs` | `has_managed_layout` | 四位十六进制桶加仓库名的形状验证 |
| `codex-rs/worktree/src/lib.rs` | `remove_worktree` | `worktree remove --force` 加清空桶 |
| `codex-rs/worktree/src/lib.rs` | `linked_worktree_common_dir` | 校验 git-dir 与 common-dir 不同 |
| `codex-rs/worktree/src/paths.rs` | `allocate_worktree_root` | UUID 前四位做桶，create_dir 占位 |
| `codex-rs/worktree/src/paths.rs` | `remove_empty_bucket` | 只删空桶的尽力清理 |
| `codex-rs/worktree/src/metadata.rs` | `OWNER_FILENAME` | `codex-thread.json` 记录名 |
| `codex-rs/worktree/src/metadata.rs` | `OwnerRecord` | 版本加线程 id，拒未知字段 |
| `codex-rs/worktree/src/metadata.rs` | `bind_thread` | 原子不覆盖发布与并发重查 |
| `codex-rs/worktree/src/metadata.rs` | `owner` | 读记录并校验版本与非空 id |
| `codex-rs/worktree/src/metadata.rs` | `metadata_path` | 用 `--git-path` 定位记录位置 |

## 参考

- [open-interpreter 源码：worktree/src/lib.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/worktree/src/lib.rs)
- [git 文档：git-worktree](https://git-scm.com/docs/git-worktree)
- [open-interpreter 文档](https://www.openinterpreter.com/docs)

## 与主核 / AI 的关系

主核里最接近的是「副本目录的分配与回收」：测试库、临时表空间、快照目录都需要「分配一个唯一位置、初始化、失败则清理、并有归属标记防止被别的实例占用」。这一页的桶分配加归属记录与那个模式同构，连「用一次原子创建表达占领」的做法都一样。

与 g01（会话滚动记录）的 `WriterLockCoordinator` 对照很有意思：那边用文件锁协调同一线程的多个写入者，这里用「原子不覆盖的元数据文件」协调同一工作树的多个归属者。两者都是无锁文件系统的乐观并发：一个靠 `try_lock` 的 `WouldBlock`，一个靠 `persist_noclobber` 的 `AlreadyExists`。

与 goose 的 j02（调度器的配方文件复制）也有对应：那边把外部配方复制到托管目录再改写 source，本质上也是「建立一个受管副本并记录它的来源」。这一页做得更彻底——连归属都写进了元数据目录，且带版本号。

对 AI 工程最实用的一条是「用原子创建表达占领」。任何「分配唯一名字」的需求（桶、会话 id、临时目录），都别用「先查存在再创建」——两种操作之间有窗口。正确做法是直接创建并接受失败：`create_dir` 返回 `AlreadyExists` 就换名字重试。这条规则简单、无锁、且不需要清理「检查过了但没建」的残留。

另一条是「每阶段回滚」而不是「最后统一清理」。多步资源创建时，每一步知道自己新建了什么、哪些是已有的；只有逐阶段的回滚才能精确地只删自己建的那部分。统一清理会在「第二步失败」时试图去删第三步的东西（不存在），或在「第五步失败」时只删一部分。

最后一条是「读索引要用当前状态反验」。`git worktree list` 给出的是注册记录，而注册记录可能与文件系统不一致（陈旧条目、路径被复用）。做这类读操作时，必须逐条验证「这个记录说的东西现在真的在那儿、且真的属于我」。这里用 backlink（管理目录里的 `gitdir` 指回 checkout）做归属验证，是一个很聪明的手法——它利用了 git 自己的双向引用，成本一次读文件。
