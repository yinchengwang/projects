---
project: open-interpreter
feature: skill-selection
title: 技能提及与隐式调用识别
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/skills/src/mentions.rs
    symbols: [ToolMentions, tool_kind_for_path, SKILL_PATH_PREFIX]
  - path: codex-rs/skills/src/invocation.rs
    symbols: [detect_implicit_skill_invocation_for_command, tokenize_command, detect_skill_script_run]
  - path: codex-rs/skills/src/selection.rs
    symbols: [collect_explicit_skill_mentions, select_skills_from_mentions]
---

# 技能提及与隐式调用识别

## 一句话职责

判断「一次请求用到了哪些技能」，而这里有**两条证据链**：模型**说**它要用某个技能（文本里的 `$skill-name` 或结构化的选择项），与模型**实际**触碰了技能目录（跑了技能里的脚本、读了它的文档）。三条文件分别是这两条链的解析、识别与合并。

## 机制是什么

| 文件 | 在哪条链上 | 产出 |
|---|---|---|
| `mentions.rs` | 文本证据 | 从一段用户文本里抽出 `$name` 提及与 `[$name](路径)` 链接，并按路径前缀判类型 |
| `invocation.rs` | 行为证据 | 从一条 shell 命令里识别「跑了技能脚本」或「读了技能文档」，并解析回技能元数据 |
| `selection.rs` | 合并 | 结构化选择优先，其次文本提及，按路径精确匹配优先、按名字模糊匹配兜底 |

这个三层结构的关键判断是：**「提到」与「用到」是不同的证据，强度也不同**。模型可以在文本里写 `$foo` 而完全不使用它；模型也可能直接 `python .skills/foo/run.py` 而从未提过 foo。

三者的接合点在 `SkillMetadata`：`mentions` 只产出字符串，`selection` 把它匹配到 `SkillMetadata`，而 `invocation` 直接产出 `SkillMetadata`（因为它有索引可查）。

## 怎么实现的

### 一、提及解析：手写扫描器，不靠正则

**`extract_tool_mentions_with_sigil` 是一个逐字节的状态机，不是正则。** 92 行里没有一条正则——这个选择有实际理由：要支持**两种形态**（`$name` 与 `[$name](path)`），且要精确处理「链接形态里的路径」。一条正则要同时表达这两者会变得难以维护，而逐字节扫描能渐进处理每种前缀。

**扫描分两支，且链接形态优先。**

主循环里先判 `byte == b'['`：若紧跟的是 `sigil`（默认 `$`）且能完整解析出一个链接，就**跳过整段**（`index = end_index`）。否则才判「当前位置是不是 sigil」并按名字字符集向后吞。

这个顺序是必要的：`[$foo](skill://foo)` 的第二个字符是 `$`，若不先试链接形态就会把它当成一个裸提及并按名字规则吞到 `[` 为止的前面——结果会得到一个错的提及。

**名字字符集是一个硬编码的字节集合**：`matches!(byte, b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'_' | b'-' | b':')`。

也就是说提及名可以是字母数字、下划线、连字符、**冒号**。冒号的存在说明它要能表达带命名空间的工具名（`mcp:tool` 之类）；而它同时也是为什么「`$PATH` 之类的环境变量会被误认为提及」这个问题必须单独处理。

**`is_common_env_var` 的排除名单是本页最实用的一处工程判断。** 名单是 `PATH`、`HOME`、`USER`、`SHELL`、`PWD`、`TMPDIR`、`TEMP`、`TMP`、`LANG`、`TERM`、`XDG_CONFIG_HOME`，判定时不区分大小写（`name.to_ascii_uppercase()` 后比对）。

理由明显：用户文本里会大量出现 `$HOME/.config`、`$PATH` 这类写法，若都当成技能提及，会产生大量假阳性。而名单只收**最常写的那几个**——这不是穷举，而是「挡住噪声最高的那部分」。

代价是：如果真有人把技能命名为 `home` 或 `path`，它就无法通过裸提及被选中（但通过链接形态仍可，因为链接形态也过这个检查——也就是说这两种形态都被漏过）。这是一个明确接受的小损失。

**链接形态的解析是严格的，任何一步不符就退回裸提及路径。** `parse_linked_tool_mention` 的检查序列是：`[` 后必须是 sigil → sigil 后必须是名字字符 → 名字字符直到 `]` → `]` 后跳过空白 → 必须是 `(` → 直到 `)`。

每一处都用 `text_bytes.get(i)` 加 `?`，于是越界或类型不符都返回 `None`。**路径为空（`path.is_empty()`）也返回 `None`**。

这是一个「全或无」的解析：一个不完整的 `[$foo](` 不会被当成链接，于是它退化成「一个裸提及 `$foo` 后面跟着些普通字符」。这个降级方向是对的——半对的链接若被接受，会得到一个错误路径。

**路径捕获与名字捕获并存，这是链接形态的核心价值。** 代码里分支是：

```
if !matches!(tool_kind_for_path(path), App | Mcp | Plugin) {
    mentioned_names.insert(name);
}
mentioned_paths.insert(path);
```

也就是说**对 App / Mcp / Plugin 三类，只记路径不记名字**；其余（包括 Skill 与 Other）两者都记。

这个分支解释了两件事：一是「name 集合」的用途是**按名字兜底匹配**，而 App/Mcp/Plugin 不是技能，不该进这个名字集合；二是 Path 集合无条件记录，因为它是**精确匹配**的依据。

**`ToolMentions` 有三个集合而不是两个，第三个 `plain_names` 是新旧形态的分水岭。** 三个字段：`names`、`paths`、`plain_names`。

`names` 与 `plain_names` 的差别在于：**只有裸提及会进 `plain_names`**，链接形态的提及只进 `names`。这从代码能读出来——链接分支只 `mentioned_names.insert(name)`，裸提及分支两个都插。

于是 `contains_plain_name(name)` 的语义是「这个名字以裸形式出现过」。而 `selection.rs` 用它是为了**歧义安全**：一个裸名字（`$foo`）可能指的是技能也可能指的是别的同名工具，所以只在无歧义时才按名字匹配；而带路径的提及（`[$foo](skill://foo)`）有精确路径，不需要这层保守。

**这就是「名字证据比路径证据弱」在数据结构层面的落地**：`plain_names` 与 `paths` 分开存，正是为了让消费方能按证据强度分级使用。

**`ToolMentionKind` 的判定有两条路径，第二条容易被忽略。** `tool_kind_for_path` 先看四个前缀（`app://`、`mcp://`、`plugin://`、`skill://`），而**最后一条是 `is_skill_filename(path)`**——只要路径的文件名是 `SKILL.md`（不区分大小写），就判成 Skill。

这意味着**一个裸的相对路径 `./.skills/foo/SKILL.md` 也会被判成技能路径**，不必带 `skill://` 前缀。判定用 `path.rsplit(['/', '\\']).next()` 同时处理两种分隔符——Windows 路径也能识别。

**`normalize_skill_path` 只剥 `skill://` 前缀，不做别的归一。** 而 `selection.rs` 里包了一层 `normalize_host_skill_path`：`normalize_skill_path(path).replace('\\', "/")`。**反斜杠统一成正斜杠**，于是同一个技能在 Windows 与 Unix 式的写法下都能匹配。

两层归一分开写是有道理的：`mentions.rs` 只关心「这是不是技能路径」，而 `selection.rs` 关心「两条路径是不是指同一个东西」。

**`plugin_config_name_from_path` 处理了一个真实的界面细节。** 注释：**桌面 app/browser 的提及会附加 `?app=...` 或 `?browserFamily=...`。匹配插件 ID 时忽略这些目标参数**。实现是 `strip_prefix` 后 `.split('?').next()`。

这是一个「同一条路径可能带查询参数」的兼容：如果不剥，同一个插件在两个界面入口下会得到两个不同 ID。

### 二、隐式调用：从命令 token 反推技能

**`detect_implicit_skill_invocation_for_command` 的顺序是「先脚本、后文档」。**

```
1. 按平台选分词器
2. detect_skill_script_run(...) 若命中则返回
3. 否则 detect_skill_doc_read(...)
```

顺序有意义：**跑脚本是比读文档更强的证据**——读 `SKILL.md` 可能只是在了解它，而执行技能目录里的脚本意味着真的在用它。

**分词器按平台切换，判据是 `PathConvention::native() == PathConvention::Windows`。** Windows 走 `tokenize_powershell_command`，其余走 `tokenize_command`（实现是 `shlex::split`，失败则退 `split_whitespace`）。

这个分支不是可选优化：PowerShell 的引号与转义规则与 POSIX shell 完全不同，用 `shlex` 解析 PowerShell 会得到错的 token。

而 `shlex::split` 的**失败兜底是 `split_whitespace`**：一条语法不完整的命令（未闭合引号）会让 `shlex` 返回 `None`，此时退化成按空白切分。这是一个合理的降级——我们只需要认出「哪个 token 像脚本路径」，不需要精确解析。

**`script_run_token` 用两张白名单表。**

```
RUNNERS: [python, python3, bash, zsh, sh, node, deno, ruby, perl, pwsh]   // 10 个
SCRIPT_EXTENSIONS: [.py, .sh, .js, .ts, .rb, .pl, .ps1]                   // 7 个
```

判定三步：

1. 取第一个 token 的 basename、小写、剥掉 `.exe` 后缀，**必须在 RUNNERS 里**；
2. 从第二个 token 起，**跳过以 `-` 开头的选项与 `--` 分隔符**，取第一个非选项 token 作为候选脚本；
3. 候选脚本的**小写必须以某个后缀结尾**。

第 1 步那个 `.exe` 剥离很关键：Windows 上 `python.exe` 是最常见的写法。

第 2 步跳过以 `-` 开头的 token，是为了处理 `python -u script.py` 这类带选项的调用；而 `token == "--"` 单独列出来，因为 `--` 本身以 `-` 开头（会被上一个条件覆盖），显式写出让意图清楚。

第 3 步要求扩展名，是为了**避免把 `python -m http.server` 里的 `http.server` 当成脚本**——它不以任何脚本扩展名结尾，于是整条命令不被认作「跑技能脚本」。这是一个有效的噪声过滤器，代价是「一个无扩展名的可执行脚本」无法被识别。

**`detect_skill_script_run` 的匹配方式是「沿祖先目录向上找」。** 它把脚本路径拼到 workdir 上、规范化，然后：

```
for path in script_path.ancestors() {
    if let Some(candidate) = outcome.implicit_skill_for_scripts_dir(&path) { return Some(candidate.clone()) }
}
```

于是「这个脚本属于哪个技能」由**索引反查**决定，而这个索引的键是**目录**（`implicit_skill_for_scripts_dir`），不是文件。向上遍历祖先意味着「脚本可以在技能目录的任意子目录里」。

**`detect_skill_doc_read` 复用了通用的命令解析器。** 它对每条 `ParsedCommand::Read { path, .. }` 拼上 workdir、规范化，再用 `implicit_skill_for_doc_path` 反查。也就是说「读文档」的识别交给 `parse_command_impl`（能识别 `cat`、`less` 等多种读文件命令），而技能归属靠索引。

**`canonicalize_if_exists` 是这两条识别的共同前提。** 实现是 `path.canonicalize().unwrap_or_else(|_| path.clone())`——**能规范就规范，不能就原样**。

第三个参数名（`_if_exists`）说明了它的用途：一个不存在的路径无法 canonicalize（会报错），此时保留原样让后续索引查找失败，而不是报错。这个方向对：识别是「尽力而为」的，不该因为路径不存在而失败。

而规范化本身是必要的——索引里的键是规范路径，而命令里的路径可能是 `../` 或符号链接形式。

**`implicit_skill_accesses_for_command` 是「不看文件系统」的版本。** 它的文档注释写明：**解析静态可识别的技能访问，不咨询宿主文件系统**。

它与上面那个函数的三处差别值得逐一对比：

| | `detect_implicit_skill_invocation_for_command` | `implicit_skill_accesses_for_command` |
|---|---|---|
| 输入 | `command` + `workdir: &AbsolutePathBuf` | `command` + `workdir: &PathUri` |
| 平台判定 | `PathConvention::native()` | `workdir.infer_path_convention()` |
| 输出 | `Option<SkillMetadata>`（直接给技能） | `Vec<ImplicitSkillAccess>`（给路径与种类） |
| 规范化 | 会 canonicalize（碰文件系统） | 不 canonicalize |
| 索引 | 查 `ImplicitSkillLookup` | 不查 |

也就是说第二条是**纯语法层**的：它只说「这条命令碰了某个路径，那像是个技能脚本 / 技能文档」，不做归属判定。这个分离是给「没有文件系统访问能力的场景」（比如远端或沙箱外）用的。

而 `workdir.infer_path_convention()` 与 `PathConvention::native()` 的差别同理：前者从 workdir 的形状推断，后者用编译目标平台。

**`ImplicitSkillAccess` 是两个变体（`Document(PathUri)` / `Script(PathUri)`），而 `PathUri` 是专门为「尚未落到真实文件系统」的路径设计的类型。** 这与 `AbsolutePathBuf` 的区别就是「是否可以被真正打开」——语法层的识别产出前者。

### 三、合并：结构化优先，路径优先于名字

**`collect_explicit_skill_mentions` 的实现结构是两轮独立遍历，不是一轮混合。**

**第一轮：结构化选择（`UserInput::Skill { name, path, .. }`）。** 逐步：

1. `blocked_plain_names.insert(name.clone())`——**把这个名字记进「禁止按裸名字匹配」集合**；
2. `AbsolutePathBuf::relative_to_current_dir(path)`，失败跳过；
3. 在已加载技能里找路径匹配的：**要么 `path_to_skills_md` 相等，要么它的「发现路径」相等**；
4. 若技能被禁用或路径已选过，跳过；
5. 记入 `seen_paths` / `seen_names` 并 push。

第 1 步是这一轮最有意思的设计。**一个显式给了路径的选择，会让同名的裸提及失去资格。** 理由很清楚：用户（或上层客户端）已经精确指出了「是这个路径的技能」，那么文本里出现的 `$同名` 就不该把另一个同名技能也拉进来。它把一个更强的证据当作对弱证据的抑制。

而 `blocked_plain_names` 的作用范围是**整个函数**（跨两轮），所以它同时抑制第二轮的名字匹配。

第 3 步的「两个路径都能匹配」是必要的：`skill_discovery_path_by_path` 那个映射说明真实路径与发现路径可能不同，而结构化输入里的路径**两种都可能**（取决于界面上显示的是哪个）。

**第二轮：文本提及。** 对每个 `UserInput::Text { text, .. }` 抽提及，然后调 `select_skills_from_mentions`。

**`select_skills_from_mentions` 内部又是两遍：先按路径、后按名字。** 这两遍的顺序是这一页最重要的语义：

第一遍遍历所有已加载技能，**只做路径匹配**（canonical 路径或发现路径命中 `mention_skill_paths`）。

第二遍再遍历同一个列表，**只做名字匹配**，且条件是四个：

1. 技能未被禁用、未被选过；
2. 名字不在 `blocked_plain_names` 里；
3. `mentions.contains_plain_name(skill.name)`——**必须是裸提及**；
4. `skill_count == 1 && connector_count == 0`。

第 4 条的两个计数各防一类歧义：

- `skill_name_counts` 来自 `build_skill_name_counts(skills, disabled_paths)`——**同名技能的数量**（禁用的不算）；
- `connector_slug_counts` 是外部传入的 **connector slug 数量**。

也就是说**只有「这个名字在技能里唯一」且「它不是某个 connector 的 slug」时才按名字选中**。文档注释把这条写得很明确：**plain names 只在匹配无歧义时才使用**。

**这个「同名计数」的粒度值得注意：它是按 `skill.name` 精确计数与按小写计数两套。** `build_skill_name_counts` 返回两个 map（exact 与 `to_ascii_lowercase`）。而 `selection.rs` 只用第一个（`.0`），第二个在别处用。

**`selected` 是一个 `Vec<SkillMetadata>` 且带两个 `seen` 集合。** 三个状态量：`seen_paths`（按 `path_to_skills_md`）、`seen_names`、`selected`。

注意**去重的主键是路径**（`seen_paths`），而 `seen_names` 只在第二遍的名字匹配里用（`if seen_names.insert(...)`）。这个不对称说明：两条来源（结构化与文本）都按路径去重，而名字匹配额外需要「同名只选一次」——因为一个名字可能匹配到多个技能（在 `skill_count != 1` 时已被拒，但一个名字仍可能已从路径路径选过）。

**`collect_explicit_skill_mentions` 的复杂度注释是本页少见的自说明。** 原文给出 `O(T + (N_s + N_t) * S)` 时间与 `O(S + M)` 空间，并定义每个符号。这个复杂度说明了两件事：提及解析是线性于文本长度（`T`），而匹配是**每个输入都线性扫一遍技能列表**（`(N_s + N_t) * S`）。

也就是说**它不是用哈希索引加速的**，而是直接遍历。技能数量（`S`）通常是几十到几百，输入通常一两条——这个量级下遍历是最简单也足够快的选择。注释把这个量级假设显式化了，于是将来真需要索引时，判据是「S 变大了」。

**`ExplicitSkillLookup` trait 有四个方法，其中一个有默认实现。** `is_skill_enabled` 的默认实现是 `!disabled_paths().contains(&skill.path_to_skills_md)`。

把「禁用」表达成「路径集合」而不是「技能上的布尔字段」，使禁用成为一个**外部状态**而不是技能数据的属性——同一个技能可以在这个会话启用、在那个会话禁用，而 `SkillMetadata` 不必因此变化。

## 关键取舍

**源码事实：** 提及解析是逐字节手写扫描器，链接形态优先于裸提及，任何一处不完整都退回裸提及路径；路径为空也算解析失败。

**源码事实：** 名字字符集含冒号（支持带命名空间的工具名），并有一份常见环境变量排除名单（`PATH`/`HOME`/`USER` 等）挡假阳性。

**源码事实：** `ToolMentions` 的三集合设计让「裸提及」与「链接提及」可分：只有裸提及进 `plain_names`，而按名字匹配只允许用 `plain_names`——名字证据弱于路径证据，在数据结构层就分开了。

**源码事实：** App / Mcp / Plugin 三类路径**只记路径不记名字**；Skill 与 Other 两者都记。

**源码事实：** `tool_kind_for_path` 的第二条判据是「文件名是 `SKILL.md`」，于是裸的相对路径也能被认出是技能，不必带 `skill://` 前缀。

**源码事实：** `normalize_skill_path` 只剥前缀，而 `selection.rs` 额外把反斜杠统一成正斜杠——「类型判断」与「同一性判断」需要不同程度的归一。

**源码事实：** 隐式调用先判脚本后判文档，因为跑脚本是更强的证据。

**源码事实：** 分词器按平台切换（Windows 用 PowerShell 解析器），`shlex` 失败时退化为空白切分。

**源码事实：** `script_run_token` 用两张白名单（10 个 runner、7 个扩展名），跳过以 `-` 开头的选项，并剥 `.exe` 后缀；要求扩展名是为了避免把 `python -m http.server` 当成脚本。

**源码事实：** 脚本归属靠「沿祖先目录向上查索引」，于是脚本可以在技能目录的任意子目录里；文档归属靠通用命令解析器给出的读文件动作。

**源码事实：** `canonicalize_if_exists` 在规范化失败时保留原路径——识别是尽力而为的，不因路径不存在而失败。

**源码事实：** 结构化选择会把名字记进 `blocked_plain_names`，从而抑制同名裸提及——强证据抑制弱证据。

**源码事实：** 名字匹配要求「技能名唯一」且「不是 connector slug」，并用四重条件；路径匹配不受这些限制。

**源码事实：** 复杂度注释明确写出 `O(T + (N_s + N_t) * S)`——匹配是遍历而非索引，注释把「S 很小」这个假设显式化。

**架构推论：** 这三条文件共同呈现了一个**证据强度分级**的完整案例：路径证据（`[$foo](skill://foo)`、`UserInput::Skill`）强于名字证据（裸 `$foo`），行为证据（跑了脚本）强于文本证据（提了一句）。而分级不是靠一个 `Option<f32>` 置信度，而是靠**把不同强度的证据存在不同容器里**（`paths` vs `plain_names`），再让消费方按容器分遍处理。

这个做法比「打分」更好维护：新加一类证据只需加一个容器与一遍遍历，不必调权重。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/skills/src/mentions.rs` | `ToolMentions` | 三个集合：名字、路径、裸名字 |
| `codex-rs/skills/src/mentions.rs` | `extract_tool_mentions` / `_with_sigil` | 逐字节扫描器，链接形态优先 |
| `codex-rs/skills/src/mentions.rs` | `parse_linked_tool_mention` | 严格的全或无链接解析 |
| `codex-rs/skills/src/mentions.rs` | `tool_kind_for_path` | 四个前缀 + `SKILL.md` 文件名的类型判定 |
| `codex-rs/skills/src/mentions.rs` | `SKILL_PATH_PREFIX` / `MCP_PATH_PREFIX` | 路径前缀常量 |
| `codex-rs/skills/src/mentions.rs` | `TOOL_MENTION_SIGIL` | 提及符号 `$` |
| `codex-rs/skills/src/mentions.rs` | `is_common_env_var` | 常见环境变量排除名单 |
| `codex-rs/skills/src/mentions.rs` | `normalize_skill_path` | 只剥 `skill://` 前缀 |
| `codex-rs/skills/src/invocation.rs` | `detect_implicit_skill_invocation_for_command` | 先脚本后文档的识别入口 |
| `codex-rs/skills/src/invocation.rs` | `ImplicitSkillLookup` | 目录与文档路径两种索引查询 |
| `codex-rs/skills/src/invocation.rs` | `tokenize_command` | `shlex` + 空白切分的降级 |
| `codex-rs/skills/src/invocation.rs` | `detect_skill_script_run` | 沿祖先目录查索引定归属 |
| `codex-rs/skills/src/invocation.rs` | `script_run_token` | 两张白名单 + 选项跳过 + `.exe` 剥离 |
| `codex-rs/skills/src/invocation.rs` | `implicit_skill_accesses_for_command` | 纯语法层、不碰文件系统的版本 |
| `codex-rs/skills/src/invocation.rs` | `canonicalize_if_exists` | 尽力而为的规范化 |
| `codex-rs/skills/src/selection.rs` | `collect_explicit_skill_mentions` | 两轮遍历：结构化优先、文本其次 |
| `codex-rs/skills/src/selection.rs` | `select_skills_from_mentions` | 两遍：先路径精确、后名字保守 |
| `codex-rs/skills/src/selection.rs` | `ExplicitSkillLookup` | 四个查询方法，`is_skill_enabled` 有默认实现 |
| `codex-rs/skills/src/selection.rs` | `normalize_host_skill_path` | 反斜杠统一，供跨平台同一性比较 |

## 参考

- [open-interpreter 源码仓库](https://github.com/OpenInterpreter/open-interpreter)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [open-interpreter 文档：技能](https://www.openinterpreter.com/docs/terminal/skills)
- [MCP 规范：工具名与命名空间](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

这套「证据分级 + 保守匹配」与主核里的「名称解析（name resolution）」是同一类问题：数据库解析一个标识符时也要在多层作用域里找，并要处理同名歧义。主核的处理是**按作用域优先级依次尝试、歧义时明确报错**；这里因为在文本语境里不能报错，改成「只在无歧义时才匹配，否则静默不选」。

对 agent 工程的直接价值有三条。其一，**不同强度的证据要存在不同容器里，不要压成一个置信度分数**。`paths` 与 `plain_names` 分开，让消费方能自然地「先精确后模糊」；打分则需要调权重，而权重永远调不对。其二，**强证据应当抑制弱证据，而不是简单叠加**。`blocked_plain_names` 让一个精确的结构化选择阻止同名的裸提及再拉进另一个技能——这避免了「用户已经指明了却还是选错」。其三，**解析器可以尽力而为，但不能假装精确**。`shlex` 失败退化成空白切分、`canonicalize` 失败保留原路径、扫描器遇到半截链接退回裸提及——三处降级都明确知道自己在猜，而猜的结果只会「匹配不到」，不会「匹配错」。
