---
project: pydantic-ai
feature: workspaces
title: 工作区后端与权限分层
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/workspaces/workspace.py
    symbols: [Workspace, WorkspacePaths]
  - path: pydantic_ai_slim/pydantic_ai/workspaces/local.py
    symbols: [LocalWorkspaceBackend]
  - path: pydantic_ai_slim/pydantic_ai/workspaces/authority.py
    symbols: [AuthorityWorkspace]
---

# 工作区后端与权限分层

## 一句话职责

给 agent 一个「环境」抽象：命令在哪跑、文件在哪读写、路径用哪种方言；并在其上加一层**内容权威分级**——同一份工作区里，不同文件的文本到达模型时可以说成是应用写的、用户写的、还是纯粹的数据。

## 机制是什么

三层：

- `WorkspaceBackend` 是一个 `Protocol`（不是 ABC）：任何具备那组成员的对象都算后端。它从配置和可选的 `WorkspaceRef` **不做 I/O 地构造**，第一次操作才创建环境或附着到 ref 指名的环境；后端**永不销毁**环境，销毁归 ref 持有者。
- `Workspace` 是业务层包装：持有 backend，提供相对路径解析、文本化读取，并且是 `ctx.workspace` 的类型。
- `WrapperWorkspace` / `AuthorityWorkspace` 是包装层：前者是通用装饰器基类，后者专门降低权威等级。

配套类型：`WorkspacePaths` 是一个 `Protocol`，暴露 `sep`、`isabs`、`join`、`normpath`、`normcase`、`relpath`、`dirname`、`basename`、`splitdrive`——也就是说**路径方言是可替换的**，`posixpath` 或 `ntpath`，取决于工作区标称的平台。

错误类型分五种，每一种都对应不同的处置：`WorkspaceError`（层主动失败）、`WorkspaceOutputLimitError`（输出超限，带 `limit` 与被截断的开头）、`WorkspaceUnavailableError`（环境没了或不可用，**重试不会成功**）、`WorkspaceTimeoutError`（命令超时，带部分输出）、`WorkspaceReadOnlyError`（只读工作区拒绝变更）。

## 怎么实现的

**`Workspace.backend` 会一直往内剥。** 它检查 `self._backend` 是否也是 `Workspace`，是就继续往下，最终返回真正的 provider 后端。这让「取 provider 特有功能」不受包装层数影响。

**`read_only` 是继承而非自身状态。** 只有内层是 `Workspace` 且它为只读时才为真——也就是说只读是**底层**的性质，包装层无法凭空造出只读，也无法取消只读。这一点与 `authority` 的处理形成对照。

**权威等级的合成规则是「取最低」。** `authority` 属性取所有包装层声明的等级中的最低者，缺省是 `'operator'`。默认值在 `Workspace` 基类里就是 operator，即「这文件是应用/运维控制的」；`AuthorityWorkspace` 只能降不能升。

**`authority_of(path)` 比 `authority` 复杂得多，也更有用。** 它的规则分三层：

1. 工作目录内的文件默认取 `authority`，除非某个包装层的 `path_authority` 为包含它的目录或文件设了别的等级——**最长匹配优先**，多层包装取最低。
2. 工作目录外且不被任何 `path_authority` 包含的路径算「工作区之外」，等级为 `'none'`。
3. 最终等级取「书写位置」与「实际指向」两者中较低的那个。

第三条是为了防符号链接：**工作区内的软链会取自身等级与目标等级中较低者**，所以链接不能抬高目标的等级，也不能把一个更低的等级洗白。源码还写了这条保护的边界：链式链接中「只作为另一条链接的目标而经过」的那一环看不到，因为 `realpath` 一次解析整条链；在不支持 `realpath` 的后端上，只有书写路径算数。

**`_resolve_path_authority` 把每个 key 展开成两处。** 一个 key 写在某位置、可能指向另一个位置，两个位置都要有等级约束。若 key 是链接且其书写位置在工作区内，则目标处的等级被「书写位置本可达到的等级」封顶；若链接指向工作区外，就没有这个封顶。这段逻辑是「防洗白」的核心实现。

**`authority_of` 有一个明确的使用时机要求。** 文档说：答案反映**当前**文件系统状态，因此应该在**读取文件时**调用，并把等级随文本一起保存，而不是等到发送文本时再问一次——因为那时文件系统可能已经变了。

**`AuthorityWorkspace` 的 `path_authority` 不展开 `~`。** 构造时若发现任何 key 以 `~` 开头就直接 `UserError`，理由写得很清楚：`~/x` 会被当相对路径读成 `<working_dir>/~/x`，而工作区里的文件**可以创建**这个名字——于是路径被劫持。它给出的替代方案是传绝对路径，或用 `LocalWorkspace(path_authority=...)`（那个版本会在这台机器上展开 `~`）。

**`LocalWorkspaceBackend` 的 docstring 第一句就是「它什么都隔离不了」。** 它把命令作为宿主子进程跑、用宿主文件系统；命令与绝对路径能到达这个进程能到达的任何地方。这是刻意不隐藏的事实，而不是待修的限制。

**本地后端的两个具体设计。** 它只继承 `PATH`、`HOME`、`LANG`、`LC_ALL`、`LC_CTYPE` 五项环境变量——注释说明它们「不是秘密，但没有它们命令就找不到宿主的工具与用户配置」；其余环境变量一律不继承，避免把任意机密带进子进程。超时靠**杀整个进程组**（`start_new_session=True` + `os.killpg`），这也是它拒绝非 POSIX 平台的原因（docstring 明说：其超时契约要求杀整个进程组，Windows 请用 `WindowsWorkspaceBackend`）。

**`LocalWorkspaceBackend` 用 `O_NONBLOCK` 打开待读文件。** 注释解释：这样才能在不阻塞的情况下探测 FIFO 与设备文件，否则一次读取会永久挂住。

**工作区目录就是环境。** 文档说明：目录从构造时就存在（`ref` 立即可用），第一次操作时若目录不见了则抛 `WorkspaceUnavailableError`；目录的创建与删除归调用方。

**后台任务会活得比 `run()` 久。** 文档提醒：后台作业超出 `run()` 的生命周期，要重定向它们的输出，否则会等继承的输出管道最多两秒；这些作业的清理归宿主退出时管理。这条提醒很短，但它是一个常见的「为什么我的命令跑完还卡住」的原因。

## 关键取舍

**源码事实：** `WorkspaceBackend` 是 `Protocol` 而不是 `ABC`。任何具备那组成员的对象都算后端，这让「用自定义对象替换后端」不需要继承框架类。

**源码事实：** 后端永不销毁环境，销毁权归 ref 持有者。这是明确的所有权约定，避免「谁最后退出谁清理」这类不确定行为。

**源码事实：** `read_only` 只能来自底层，包装层既不能造也不能取消；`authority` 可由包装层降低，且多包装取最低。两个属性的自由度被刻意设成不同。

**源码事实：** `authority_of` 取「书写位置」与「真实指向」的较低者，并封顶链接对目标的提升。这防的是「把低权威文件链接进高权威目录」与「把高权威文件藏到低权威目录」两个方向的洗白。

**源码事实：** `path_authority` 不展开 `~`，因为工作区内的文件可以创建名为 `~` 的目录，从而劫持路径解析。

**源码事实：** 本地后端只继承五项环境变量、用进程组杀超时、拒绝非 POSIX。这三条都由具体失败模式驱动，而不是风格选择。

**源码事实：** `LocalWorkspaceBackend` 明说不隔离任何东西。这意味着它适合「让 agent 在本机做真实工作」的场景（例如 CLI agent），不适合「执行不可信代码」。

**架构推论：** 工作区这一层的价值在于把「环境」与「内容权威」拆开：环境决定能不能做（读写、执行、超时、只读），权威决定「从里面读出来的文字该以什么口气到达模型」。这两件事经常被混在一起（例如「沙箱里的输出都可信」），但它们是独立的：一个完全隔离的容器里也可能放着第三方仓库，那些文件的文字不该被当成应用的指令。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/workspaces/workspace.py` | `Workspace` | 业务层包装：相对路径解析、文本读取、`ctx.workspace` 类型 |
| `pydantic_ai_slim/pydantic_ai/workspaces/workspace.py` | `WorkspacePaths` | 可替换的路径方言协议（posix / nt） |
| `pydantic_ai_slim/pydantic_ai/workspaces/workspace.py` | `Workspace.authority` | 包装层等级的合成结果，缺省 operator，取最低 |
| `pydantic_ai_slim/pydantic_ai/workspaces/workspace.py` | `Workspace.authority_of` | 单文件等级：最长匹配、书写与指向取低、链接封顶 |
| `pydantic_ai_slim/pydantic_ai/workspaces/workspace.py` | `Workspace._resolve_path_authority` | 把每个 `path_authority` key 展开到书写位置与真实位置 |
| `pydantic_ai_slim/pydantic_ai/workspaces/workspace.py` | `WrapperWorkspace` | 通用包装基类，聚合各层的权威与只读状态 |
| `pydantic_ai_slim/pydantic_ai/workspaces/local.py` | `LocalWorkspaceBackend` | 宿主子进程与宿主文件系统，明示不隔离 |
| `pydantic_ai_slim/pydantic_ai/workspaces/local.py` | `_INHERITED_ENV` | 只继承五项环境变量，避免带走机密 |
| `pydantic_ai_slim/pydantic_ai/workspaces/local.py` | `_kill_tree` | 超时杀整个进程组，也是拒绝非 POSIX 的原因 |
| `pydantic_ai_slim/pydantic_ai/workspaces/authority.py` | `AuthorityWorkspace` | 只降不升的权威包装，含 `path_authority` 的 `~` 拒绝 |
| `pydantic_ai_slim/pydantic_ai/workspaces/protocol.py` | `WorkspaceUnavailableError` | 环境不可用，重试不会成功 |
| `pydantic_ai_slim/pydantic_ai/workspaces/protocol.py` | `WorkspaceOutputLimitError` / `WorkspaceTimeoutError` | 带部分输出的两类失败 |

## 参考

- [pydantic-ai 源码：workspaces/workspace.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/workspaces/workspace.py)
- [pydantic-ai 源码：workspaces/local.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/workspaces/local.py)
- [Pydantic AI 文档：工作区](https://ai.pydantic.dev/workspace/)
- [Pydantic AI 文档：自定义工作区](https://ai.pydantic.dev/workspace-custom/)

## 与主核 / AI 的关系

工作区与主核的「表空间 / 文件空间」有一处结构呼应：都是「一个具名的存储边界 + 一组操作 + 一套错误分类」，上层不需要知道它是本机目录还是远端容器。差别在于主核关心容量与并发，工作区关心**执行能力与内容权威**。

对 agent 工程的直接价值有三条。其一，**权威分级与沙箱强度是两件事**。把一份第三方仓库克隆进最强沙箱，其中文件的文字仍然只是数据，不该以应用口气到达模型；`AuthorityWorkspace` 正是为此存在。其二，**软链是权威分级的真实攻击面**。`authority_of` 取书写位置与真实指向的较低者并封顶链接，是为了堵住「链接抬权」与「链接洗白」；自己实现类似的读文件逻辑时要照抄这条。其三，**本地后端不隔离**这句话要当真。它只继承五个环境变量、只杀进程组，其余一概不管——`PATH` 里的任何可执行文件、绝对路径能到的任何地方，agent 都能碰。
