---
project: open-interpreter
feature: agents-md
title: AGENTS.md 指令加载
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/core/src/agents_md.rs
    symbols: [DEFAULT_AGENTS_MD_FILENAME, LoadedAgentsMd, InstructionProvenance]
  - path: codex-rs/core/src/agents_md_manager.rs
    symbols: [AgentsMdManager, MAX_THREAD_INSTRUCTIONS_TOKENS, validate_thread_instruction_size]
---

# AGENTS.md 指令加载

## 一句话职责

从「用户级、线程级、项目级」三个来源收集模型可见的指令，按来源标注 provenance 再拼成一段文本；项目级部分沿目录树向上发现文件，并受一份字节预算约束——超了截断项目文档，但**超限的线程指令直接拒绝**。

## 机制是什么

两个常量定义文件名与优先级：`DEFAULT_AGENTS_MD_FILENAME = "AGENTS.md"`、`LOCAL_AGENTS_MD_FILENAME = "AGENTS.override.md"`。

`LoadedAgentsMd` 有三个字段：

- `user_instructions: Option<Instructions>`——账号或 home 级的指令，由宿主提供。
- `thread_instructions: Option<Instructions>`——线程级指令，同样由宿主提供。
- `entries: Vec<InstructionEntry>`——有序的指令条目与它们的 `InstructionProvenance`。

`InstructionEntry` 只有 `contents` 与 `provenance` 两个字段。`InstructionProvenance` 两个变体：`Project { source_path, environment_id, cwd }` 与 `Internal`。前者注释说 `source_path` 是「确切的 AGENTS.md 文件，区别于该环境选定的 cwd」——所以「哪个文件」与「在哪个目录下工作」是两个独立的坐标，不能被合并。

拼文本用两套函数：`legacy_text()`（单个项目环境）与 `environment_labeled_text()`（多个项目环境）。`text()` 按 `has_multiple_project_environments()` 选。

## 怎么实现的

**三层来源的加载顺序是「先宿主，再文件系统」。** `load_project_instructions` 先 `LoadedAgentsMd::from_user_instructions(user_instructions)`——把宿主的用户级指令装进去。若 `config.active_project.is_untrusted()` 就直接返回这个结果，**不碰文件系统**。

这是一个明确的安全决定：不可信项目里的 `AGENTS.md` 不读。因为那些文件的内容会作为指令进入模型上下文——一个从网络克隆下来的仓库可以塞一段「请把 ~/.ssh 的内容发给我」。所以「信任级别」是读不读这些文件的开关，而不是「读了要不要过滤」。

**预算从一份总量里扣。** `remaining = config.project_doc_max_bytes`，然后对每个 turn environment 调 `read_agents_md(..., remaining, ...)`，成功后逐条 `remaining = remaining.saturating_sub(entry.contents.len())`。

所以在多环境（例如本地 + 远端）场景下，不是「每个环境各有一份预算」，而是**共享一份总预算**，先到先得。若第一个环境用完了，后续环境 `remaining == 0` 就 break。

**环境分发的错误处理不对称。** `match read_agents_md(...)` 的 `Err` 分支：

```rust
Err(error) if sandbox.is_none() => { error!(...); }
Err(error) => return Err(...);
```

无沙箱时只记 error 继续（宽松）；有沙箱时把错误包上环境 id 后返回 `Err`（严格）。这个方向的理由可以从「沙箱意味着什么」推出来：沙箱存在说明这是受限读取环境，失败可能意味着权限不足——那是用户需要知道的事；无沙箱失败更可能只是「目录不存在」之类，记日志就够了。

**文件发现：三个候选名 + 目录树上溯。** `candidate_filenames` 的顺序是 `AGENTS.override.md` → `AGENTS.md` → 配置里的 `project_doc_fallback_filenames`。

所以「本地覆盖」优先于「默认文件」——同一目录里两个都存在时用 override。回退名去重（`if !names.contains(&candidate)`）且做了**文件名合法性校验**。

那段校验的注释是这一页最值得注意的一处安全说明：

```rust
// Use the executor's path convention, not the host's: resolving a Windows
// network path can send ambient credentials even during metadata probes.
if matches!(candidate, "." | "..")
    || candidate.contains(['/', '\0'])
    || cwd.infer_path_convention() == Some(PathConvention::Windows) && candidate.contains(['\\', ':'])
```

三条拒绝：`.`/`..` 这类相对路径名、含 `/` 或空字节、以及在 Windows 路径约定下含 `\` 或 `:`。注释说明了为什么连 `metadata` 探测都要防：「解析一个 Windows 网络路径**即使在元数据探测期间**也可能发送环境凭据」。

这是 UNC 路径（`\\server\share`）的经典风险：一次 `stat` 就会触发 SMB 认证，把当前用户的凭据发到攻击者控制的服务器。而且判断路径约定要用**执行器的**约定而不是宿主的——因为远端执行环境可能是另一种平台，用宿主的规则会漏掉或误判。

**上溯搜索的边界由「项目根标记」决定。** `agents_md_paths` 先合并配置层（跳过 `ConfigLayerSource::Project` 那一层），从中读 `project_root_markers`；读不到就用默认；读出错就打 warn 用默认。

然后 `find_nearest_ancestor_with_markers(fs, &dir, markers, FindUpErrorPolicy::Ignore, sandbox)` 找到最近的含标记祖先作为项目根。`Ignore` 策略说明「找的过程中出错就当没找到」。

搜索目录列表的构造：从 `cwd` 一路 `parent()` 往上，直到等于项目根为止，然后 `reverse()` 成从根到 cwd 的正序。所以文件是从**项目根往下读到当前目录**——浅层的先读，深层的后读，且深层的能覆盖浅层的语义（后出现的在后）。

若没找到项目根，就只搜索 `cwd` 一个目录。

**并发探测的上限是 256。** 探测用 `futures::stream::iter(search_dirs).map(...).buffered(MAX_CONCURRENT_ANCESTOR_PROBES)`。

`buffered` 而不是 `buffer_unordered`——所以结果的顺序与输入顺序一致（即从根到 cwd），这很重要，因为指令的拼接顺序影响模型看到的优先级。

上限 256 的注释解释了为什么这个数合理：「元数据探测很便宜，且 exec-server 传输本身已经限定了总在飞请求数。这个上限覆盖典型项目层级于一次远端往返内，又不会在独立启动发现并发运行时独占那个传输」。

注意它对每个目录是**串行试候选名**（`for name in candidate_filenames`）——一旦某个候选命中就 `return Ok(Some(candidate))`。所以一个目录最多贡献一个文件，override 优先。

**读取时的截断而非拒绝。** `read_agents_md` 里：

```rust
if size > remaining { data.truncate(remaining as usize); }
if size > remaining { warn!("project doc exceeds remaining budget; truncating"); }
```

注意判的是 `size`（原始长度）而截断用的是 `remaining`。所以「这份文件被截断了」这个事实被明确记进日志，包含 `remaining_bytes`。先 truncate 再 warn 的顺序不影响行为，但它是「先做再报告」的自然写法。

`String::from_utf8_lossy` 而不是严格 UTF-8——非 UTF-8 字节被替换成替换字符而不是报错。然后 `if !text.trim().is_empty()` 才入 entries。所以一份全空白的文件不产生条目、也不消耗预算（`remaining` 只在入 entries 后才扣）。

**拼接时那个分隔符的触发条件是「从非项目切到项目」。** `legacy_text` 里：

```rust
let separator = if is_project && !previous_was_project { AGENTS_MD_SEPARATOR } else { "\n\n" };
```

`AGENTS_MD_SEPARATOR = "\n\n--- project-doc ---\n\n"`。所以这个标记**只出现一次**，在「宿主的指令结束、项目文档开始」那个位置。

它的用途写在注释里：「project-doc 标记告诉模型工作区作用域的指令从哪开始，所以只需要从用户或内部指令到项目指令的过渡处出现」。若每个项目文件前都插一遍，模型会以为有多个独立文档段；只在过渡处插一次，它就是一个清晰的分界线。

**多环境时改成「按环境标头」。** `environment_labeled_text` 在有多个项目环境时启用，逻辑不同：

- 项目条目前不插 `AGENTS_MD_SEPARATOR`，而插 `` `for `{environment_id}` with root {cwd}` `` 这样的标头。
- 同一个环境（`(environment_id, cwd)` 相同）的连续条目**只标一次**。注释：「一个环境可能从它的项目根到它的 cwd 贡献多个层级 AGENTS.md 文件。这个环境整组标一次，而不是每个文件前重复标头」。
- `Internal` 条目插入时把 `previous_environment` 置 `None`——所以内部指令之后的环境会重新标头。

而 `contextual_user_fragment()` 在两种模式下的外层包装也不同：单环境时把 `directory` 填成那个项目的 cwd（保留旧的 cwd 包装），多环境时置 `None`（因为正文已经自己标了环境）。

**`has_multiple_project_environments` 是一个「找第二个不同值」的扫描。** 它记录首个环境 id，然后 `any` 里一旦遇到不同的就返回 true。所以「零个或一个项目环境」都返回 false（走 legacy 路径），两个及以上才走标注路径。

**`is_empty` 的判据包含条目内容。** 它不只判两个 `Option` 是否为 `None`，还判 `entries.iter().all(|entry| entry.contents.trim().is_empty())`。所以「有一个条目但内容是空格」也算空。

**管理器的双层同步。** `AgentsMdManager` 只有两个字段：`refresh_lock: Semaphore`（单许可）与 `state: Mutex<AgentsMdState>`。模块头部的注释写了两条承诺：「刷新被串行化；状态锁**从不在调用 provider 时持有**」。

`refresh` 的流程正是这两条的实现：

1. `refresh_lock.acquire()`——串行化重叠的捕获。注释：「串行化重叠的捕获，但不阻塞对已应用快照的读取」——因为读路径走的是 `state` 锁而不是这把信号量。
2. 收集 `selections` 与 `active_project_trust_level`。
3. 在**一次** `state.lock()` 里做三件事：判 `refresh_repository`（selections 变了或信任级别变了）、若需要刷新就**清空缓存**、克隆出 `instructions` 与 `cached`，然后**释放锁**。
4. 在锁外调两个 provider（`load_user_instructions` / `load_thread_instructions`）。
5. 回到锁内做校验与提交。

第 3 步那句「若需要刷新就清空缓存」带了一条重要注释：「收紧的读权限不得留下不可访问的指令可见，即使发现失败或调用方取消了刷新」。

这是「先失效、再刷新」而不是「刷新成功后才失效」。理由：如果先留着旧快照直到新快照就绪，那么在这段窗口里用户收紧权限后仍能看到之前的指令内容。所以宁可让窗口里「没有指令」，也不让「旧指令」留着。这是一个安全方向上正确的失败模式。

**校验失败也保留警告。** `refresh` 返回 `(CodexResult<Option<Arc<LoadedAgentsMd>>>, Vec<String>)`——**元组**，`warnings` 独立于 `result`。docstring 写明：「警告在校验失败后依然存活；只有被校验过的快照才替换已应用的宿主指令」。

所以 provider 报的警告（例如「某个配置项被忽略」）不会因为后面校验失败而丢失，但失败时已应用的指令**不被替换**。两个通道分离，各自有独立的更新条件。

**线程指令超限是拒绝，不是截断。** `MAX_THREAD_INSTRUCTIONS_TOKENS = 10_000`，`validate_thread_instruction_size(bytes)` 判 `bytes > approx_bytes_for_tokens(10_000)` 就返回 `CodexErr::InvalidRequest`，且错误消息里同时给出「限制的估计 token 数」与「实际提供的估计 token 数」。

那行注释写清了为什么这个上限单独存在：「独立地界定新引入的宿主提供内容，与 `project_doc_max_bytes`（控制仓库发现）无关。既有的全局与合并指令大小策略不变；**超大的线程输入直接拒绝而不是截断**」。

两个理由：`project_doc_max_bytes` 管的是项目文件发现，不该被线程指令占用；而线程指令是调用方显式传入的，截断它会在不告知的情况下改变语义——拒绝才是正确的反馈。

**缓存命中判定的两层。** 若 `!refresh_repository`，代码再看「instructions 没变」就直接返回 `cached`：

```rust
if state.instructions.user == instructions.user && state.instructions.thread == instructions.thread {
    return Ok(cached);
}
```

比较的是 `Instructions` 的完整相等（含 `text` 与 `source`）。所以「provider 每次返回同一份内容」不会触发任何重建。

**子代理的继承规则。** `inherited_instructions` 复制 user/thread 指令，但把 `thread_provider` 用 `.filter(|provider| provider.share_with_subagents())` 过滤——注释在 `SessionInstructions` 上说「子代理继承已应用的快照，加上**明确选择共享**的线程 provider」。

而 `user_provider` **不被继承**（`..Default::default()`）。所以子代理不会再去拉一次用户级指令（那是父已经解析过的），但会在被允许时继续拉线程级指令（那可能随线程变化）。

## 关键取舍

**源码事实：** 不可信项目直接跳过项目文档加载，只保留宿主指令。

**源码事实：** `candidate_filenames` 对回退文件名做路径约定校验，拒绝 `.`/`..`、`/`、空字节，Windows 约定下还拒绝 `\` 与 `:`。注释明确说这是为了防「元数据探测就发出凭据」。

**源码事实：** 多环境共享一份 `project_doc_max_bytes` 预算，先到先得。

**源码事实：** 项目文档超预算时**截断**并记 warn；线程指令超限时**拒绝**并返回 `InvalidRequest`。

**源码事实：** 有沙箱时的读取错误向上传播，无沙箱时只记日志。

**源码事实：** `AGENTS_MD_SEPARATOR` 只在「非项目 → 项目」的过渡处出现一次。

**源码事实：** 需要刷新仓库时**先清空缓存再发现**，理由是收紧的读权限不得留下不可访问的指令可见。

**源码事实：** `refresh` 把警告与结果放在元组里，两个通道有条件地独立更新。

**架构推论：** 「provenance 与内容分开存」是这一层能同时服务两种输出的前提。同一份 `LoadedAgentsMd` 要产出两种文本：单环境时用统一分隔符，多环境时按环境标头。如果条目只存内容，标头所需的 `environment_id` 与 `cwd` 就无处可取。所以「标注来源」在这里不是元数据洁癖，而是**功能必需**。

**架构推论：** 「超限拒绝 vs 超限截断」的分野取决于**谁提供了这段内容**。项目文档是用户放在仓库里的，用户看不到引擎何时截断，所以宽容（截断 + 日志）是对的；线程指令是调用方 API 明确传入的，截断会让它以为发送成功了，所以必须拒绝。判据是：这个输入是否有一个「知道自己给了多少」的发送方。

**架构推论：** 「先失效再刷新」是一个有意的失败模式选择。多数缓存实现会在刷新成功后才替换旧值（换取「从不空窗」）。这里反过来，因为「读到本该无权读的内容」这个错误的严重性高于「短暂读不到内容」。缓存策略该按「旧值的错误方向有多糟」来定，而不是按「空窗有多难看」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/core/src/agents_md.rs` | `DEFAULT_AGENTS_MD_FILENAME` | `AGENTS.md` 默认文件名 |
| `codex-rs/core/src/agents_md.rs` | `LOCAL_AGENTS_MD_FILENAME` | `AGENTS.override.md` 优先级最高的覆盖名 |
| `codex-rs/core/src/agents_md.rs` | `AGENTS_MD_SEPARATOR` | 只在非项目到项目的过渡处出现的分界标记 |
| `codex-rs/core/src/agents_md.rs` | `MAX_CONCURRENT_ANCESTOR_PROBES` | 上溯探测的并发上限 |
| `codex-rs/core/src/agents_md.rs` | `LoadedAgentsMd` | 三层来源容器，`text()` 按环境数选拼法 |
| `codex-rs/core/src/agents_md.rs` | `LoadedAgentsMd.legacy_text` | 单环境拼法，分界标记只插一次 |
| `codex-rs/core/src/agents_md.rs` | `LoadedAgentsMd.environment_labeled_text` | 多环境拼法，按环境组标头一次 |
| `codex-rs/core/src/agents_md.rs` | `LoadedAgentsMd.contextual_user_fragment` | 单环境保留 cwd 包装，多环境省略 |
| `codex-rs/core/src/agents_md.rs` | `InstructionProvenance` | 项目来源与其路径、环境、cwd 标注 |
| `codex-rs/core/src/agents_md.rs` | `load_project_instructions` | 信任判定、预算扣减、环境分发 |
| `codex-rs/core/src/agents_md.rs` | `read_agents_md` | 逐文件读取、截断与入条目 |
| `codex-rs/core/src/agents_md.rs` | `agents_md_paths` | 项目根标记上溯与有序并发探测 |
| `codex-rs/core/src/agents_md.rs` | `candidate_filenames` | 候选名顺序与路径约定安全校验 |
| `codex-rs/core/src/agents_md_manager.rs` | `AgentsMdManager` | 刷新信号量加状态锁的双层同步 |
| `codex-rs/core/src/agents_md_manager.rs` | `AgentsMdManager.refresh` | 先清缓存、锁外调 provider、校验后提交 |
| `codex-rs/core/src/agents_md_manager.rs` | `SessionInstructions` | 指令与可选 provider，子代理继承规则 |
| `codex-rs/core/src/agents_md_manager.rs` | `MAX_THREAD_INSTRUCTIONS_TOKENS` | 线程指令 10k 估计 token 上限 |
| `codex-rs/core/src/agents_md_manager.rs` | `validate_thread_instruction_size` | 超限拒绝而非截断 |

## 参考

- [open-interpreter 源码：core/agents_md.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/core/src/agents_md.rs)
- [AGENTS.md 规范说明](https://agents.md/)
- [open-interpreter 文档](https://www.openinterpreter.com/docs)

## 与主核 / AI 的关系

主核里最接近的是「配置分层合并」：默认值、系统级、用户级、项目级、命令行，按层叠加且后者覆盖前者。这一页的三层来源（用户 / 线程 / 项目）结构相同，连「项目级要按目录上溯、深层覆盖浅层」这个细节都一致。

差别在**信任边界**。主核读所有能读的配置，因为它服务的是一个明确的数据库实例；这里要判 `active_project.is_untrusted()` 才决定读不读仓库里的文件——因为那些文件是「内容」而不是「配置」，且会直接变成模型指令。这提示一件事：当「配置」的性质从「参数」变成「可执行指令」时，信任模型必须跟着变。

与 goose 的上下文拼接（i03 handoff、j01 compaction）对比：那边处理的是「已有历史的重新排列」，这边处理的是「外部文件的收集与拼接」。共同点是都有一份**硬预算**与**明确的拼接边界标记**。这个共性不是巧合：只要内容要进模型上下文，就必然需要「多大」与「到哪儿算一段」两个决定。

与 smolagents 的 prompt 模板相比，那边是单文件字符串模板，没有分层、没有来源、没有信任级别——因为它假设调用方就是可信的开发者本人。这里要支持「用户从网上 clone 一个仓库就跑」，所以每一层都要问「这段内容我能信吗」。

对 AI 工程最实用的一条是「分界标记只插一次」。给模型拼多段来源的指令时，最容易的做法是每段前都加一个 `--- doc ---` 之类的标记。结果是模型看到十个小节，可能把后来的当成覆盖了前面的，或把某几节当成互不相关的文档。正确做法是只在「语义类别切换」处插一次标记，同类内容用空行连缀——这样模型看到的是「两段话」，而不是「十个碎片」。

另一条是「超限拒绝而不是截断」的判据。任何「调用方提供了数据、系统有上限」的接口，都该问一句「截断后调用方会知道吗」。若不通过返回值告知，截断就是静默的语义篡改。而「系统自己发现的文件」没有这个对话对象，所以截断加日志是合理的。

最后一条是那个 UNC 路径的注释。它提醒的功能性事实是：**读元数据也可能有副作用**。一次 `stat` 走网络路径会触发认证，把凭据发出去。任何「先看看这个路径存不存在」的代码，在支持网络路径的平台上都必须先校验路径形状。判断用的约定要取执行环境的，而不是宿主进程的——否则远端执行时就会出现「宿主认为合法、远端实际危险」的漏洞。
