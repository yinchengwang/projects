---
project: open-interpreter
feature: policy-evaluation
title: 策略求值与规则修订
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/execpolicy/src/policy.rs
    symbols: [Policy, Evaluation, MatchOptions, HeuristicsFallback]
  - path: codex-rs/execpolicy/src/amend.rs
    symbols: [blocking_append_allow_prefix_rule, blocking_append_network_rule]
---

# 策略求值与规则修订

## 一句话职责

`Policy` 是规则集的持有者与求值器：它按首 token 取候选、按路径解析做二次尝试、在无命中时调用启发式回退，最后把多条匹配聚合出唯一决策；`amend.rs` 则是反向的一条路——把「用户刚刚批准的那条命令」写回策略文件，并且写得幂等、加锁、不产生重复行。

## 机制是什么

求值回答的问题是「这条命令该怎么办」，而它有三条**按优先级排列的命中来源**：

1. **精确规则命中**：按命令首 token 在 `MultiMap` 里取一组规则，逐条试匹配；
2. **按路径解析命中**：命令首 token 是一个绝对路径时，取它的 basename 再做一次；
3. **启发式回退**：前两者都没命中时，调用一个外部函数给出决策。

这三条是「或」的关系但有顺序：第 1 条只要有命中就**不再**尝试第 2 条，第 2 条要有命中才不进入第 3 条。原文：

```
let matched_rules = self.match_exact_rules(cmd)
    .filter(|matched_rules| !matched_rules.is_empty())
    .or_else(|| options.resolve_host_executables.then(|| self.match_host_executable_rules(cmd)).filter(|m| !m.is_empty()))
    .unwrap_or_default();

if matched_rules.is_empty() && let Some(heuristics_fallback) = heuristics_fallback {
    vec![RuleMatch::HeuristicsRuleMatch { command: cmd.to_vec(), decision: heuristics_fallback(cmd) }]
} else {
    matched_rules
}
```

`or_else` + `filter(!is_empty())` 的组合就是「有命中才算数」的准确表达：一个空 Vec 与「没命中」在类型上不同、在语义上应当相同，所以在每一层都把空转成 `None` 再往下走。

## 怎么实现的

**为什么需要「按路径解析」这第二次尝试？**

因为策略是按**裸程序名**索引的（`prefix_rule(pattern=["git", "push"])` 的键是 `git`），而用户（或模型）完全可能把命令写成绝对路径：

- `git push origin main` → 首 token `git`，直接命中；
- `/usr/bin/git push origin main` → 首 token `/usr/bin/git`，在 `MultiMap` 里查不到 `git` 那组规则，于是**看起来策略没生效**。

第 2 条就是为了消掉这个不对称。它把首 token 当路径、取 basename、用 basename 再查一次，并对命中结果打上 `resolved_program`（「这条规则是因为解析到某个真实路径才命中的」）。

这条机制的开关是 `MatchOptions`：

```
pub struct MatchOptions { pub resolve_host_executables: bool }
```

默认 `false`（`#[derive(Default)]`），只在需要时打开。**这个默认是保守的**：路径解析要碰文件系统，代价比纯字符串比较高，还可能因为平台差异（符号链接、`PATH` 顺序）引入不确定性。所以默认关、由调用方按场景打开——校验（b02 里的例子校验）就显式打开，因为它需要「裸名与绝对路径结论一致」这个性质。

**`host_executable` 那张表把路径解析收窄成一个白名单。**

`match_host_executable_rules` 里有一段额外检查，它是这一层最有意思的地方：

```
let Some(paths) = self.host_executables_by_name.get(&basename) else { ... };
if let Some(paths) = self.host_executables_by_name.get(&basename)
    && !paths.iter().any(|path| path == &program)
{
    return Vec::new();
}
```

也就是说：如果策略文件里为某个名字声明了**允许的绝对路径集合**，那么路径解析只在「这个具体路径在集合里」时才继续。若集合里没有这条路径，返回「没命中」而不是「命中一个裸名规则」。

这条设计解决了路径解析的固有风险：**同名不同物**。`/tmp/malicious/git` 的 basename 也叫 `git`，若只按 basename 匹配，它就继承了「允许 `git push`」这条规则。`host_executable` 声明「名叫 git 的东西只可是 /usr/bin/git 或 /opt/homebrew/bin/git」，于是那个伪造的路径被挡在门外。

它是**可选**的：不声明 `host_executable` 时，按 basename 的匹配无限制（这是宽松档）；声明之后就变成白名单。这个「默认宽松、声明即收紧」的方向与「策略文件的主要用途是放宽权限」一致。

**聚合是把决策取最大值，其余信息全部保留。**

```
impl Evaluation {
    fn from_matches(matched_rules: Vec<RuleMatch>) -> Self {
        let decision = matched_rules.iter().map(RuleMatch::decision).max();
        let decision = decision.expect("invariant failed: matched_rules must be non-empty");
        Self { decision, matched_rules }
    }
}
```

两点：

1. **`max()` 就是「更严格者胜」**（因为 `Decision` 派生了 `Ord` 且 `Allow < Prompt < Forbidden`）。一条命令同时命中「允许 `git`」与「禁止 `git push`」两条规则时，结果是 `Forbidden`。这是安全策略必须的性质——**宽规则不能覆盖严规则**。
2. **`matched_rules` 被原样保留**。`Evaluation` 同时带「结论」与「依据」，于是上层既能决定怎么做（看 `decision`），也能解释为什么（看匹配到的规则与它们的 `justification`）。

`expect` 那句注释是 `Caller is responsible for ensuring that matched_rules is non-empty`——因为 `check` / `check_multiple` / `matches_for_command` 的所有路径都保证了非空（要么有命中，要么有启发式兜底）。唯一的例外是 `matches_for_command_with_options` 传 `heuristics_fallback = None` 且无命中——那种调用**不该**去构造 `Evaluation`，而它确实没有（b02 的负例校验直接用 `.first()` 与 `.is_empty()`）。

`is_match()` 在同一个结构上界定另一件事：

```
pub fn is_match(&self) -> bool {
    self.matched_rules.iter().any(|rule_match| !matches!(rule_match, RuleMatch::HeuristicsRuleMatch { .. }))
}
```

它回答的是「**真的有条规则命中了吗**」，而不是「有决策吗」。启发式回退也会产出决策，但那不算「匹配」。这个区分让上层能区分「策略明确允许」与「策略没说话，只好按启发式放行」——后者往往需要在界面上以不同方式呈现（例如提示「无匹配规则」）。

**批量求值只是把单条的匹配拼起来。**

```
pub fn check_multiple_with_options<Commands, F>(&self, commands: Commands, heuristics_fallback: &F, options: &MatchOptions) -> Evaluation
where Commands: IntoIterator, Commands::Item: AsRef<[String]>, F: Fn(&[String]) -> Decision
{
    let matched_rules: Vec<RuleMatch> = commands.into_iter()
        .flat_map(|command| self.matches_for_command_with_options(command.as_ref(), Some(heuristics_fallback), options))
        .collect();
    Evaluation::from_matches(matched_rules)
}
```

用 `flat_map` 把每条命令的匹配展开拼进一个 Vec，再一次 `from_matches`。于是「一条命令列表里有 ANY 一条被禁止」→ 整体 `Forbidden`。这是正确的语义：一条 shell 命令里可能串了多条子命令（`a && b` 之类），其中任一被禁就不能放行整条。

**网络规则有一份独立的编译，且「相反决策会互相移除」。**

```
pub fn compiled_network_domains(&self) -> (Vec<String>, Vec<String>) {
    let mut allowed = Vec::new();
    let mut denied = Vec::new();
    for rule in &self.network_rules {
        match rule.decision {
            Decision::Allow => { denied.retain(|entry| entry != &rule.host); upsert_domain(&mut allowed, &rule.host); }
            Decision::Forbidden => { allowed.retain(|entry| entry != &rule.host); upsert_domain(&mut denied, &rule.host); }
            Decision::Prompt => {}
        }
    }
    (allowed, denied)
}
```

三件事：

- **后写的规则覆盖先写的**：`Allow` 会把同主机从 `denied` 里移除，`Forbidden` 反过来。于是规则顺序有意义（后条胜），这与命令规则的「取最大」是不同的聚合方式。差异有理由：命令规则是「多条同时命中」的合取式聚合，网络规则是「同一主机只能有一个结论」的覆盖式聚合。
- **`upsert_domain` 把主机移到末尾并去重**：
  ```
  fn upsert_domain(entries: &mut Vec<String>, host: &str) { entries.retain(|entry| entry != host); entries.push(host.to_string()); }
  ```
  顺序因此反映「最后声明次序」，便于上层按顺序展示（或后续按顺序取第一个）。
- **`Prompt` 不进任何一个列表**。它既不是「域名被允许」也不是「域名被拒绝」，而是「每次访问都要问」——所以它被排除在编译产物之外，由另一条路径处理。

`(Vec<String>, Vec<String>)` 这个返回类型是**位置语义**的：先是 allowed 再是 denied。用元组而不是具名结构，说明调用点很近、不需要自解释——但这确实是一个容易搞反的地方，值得记一笔。

**修订路径写的是「把批准写回文件」。**

`blocking_append_allow_prefix_rule` 的产物是一行 Starlark：

```
let tokens = prefix.iter().map(serde_json::to_string).collect::<Result<Vec<_>, _>>()?;
let pattern = format!("[{}]", tokens.join(", "));
let rule = format!(r#"prefix_rule(pattern={pattern}, decision="allow")"#);
append_rule_line(policy_path, &rule)
```

**用 `serde_json::to_string` 对每个 token 做转义**，而不是手写引号拼接。于是含空格、引号、反斜杠、非 ASCII 的命令参数都能生成合法的 Starlark 字符串字面量——一条测试就用 `"Hello, world!"` 验证了这点，产出 `prefix_rule(pattern=["echo", "Hello, world!"], decision="allow")`。这是「生成代码」时的必备纪律：**永远用序列化器而不是字符串拼接**。

`blocking_append_network_rule` 走同样的路数，四个字段各自序列化后拼进 `network_rule(host=..., protocol=..., decision=..., justification=...)`，`justification` 只在有时才加。

**追加的实现有三道保护：文件锁、去重、目录自建。**

```
fn append_locked_line(policy_path: &Path, line: &str) -> Result<(), AmendError> {
    let mut file = OpenOptions::new().create(true).read(true).append(true).open(policy_path)...;
    file.lock()...;
    file.seek(SeekFrom::Start(0))...;
    let mut contents = String::new();
    file.read_to_string(&mut contents)...;
    if contents.lines().any(|existing| existing == line) { return Ok(()); }
    if !contents.is_empty() && !contents.ends_with('\n') { file.write_all(b"\n")...; }
    file.write_all(format!("{line}\n").as_bytes())...;
    Ok(())
}
```

逐条：

1. **`create(true).read(true).append(true)`** 三个标志的组合是刻意的：需要 `append` 以便原子追加，需要 `read` 以便在追加前读全文去重（这解释了为什么还要 `seek(0)`——打开时文件指针在末尾）。
2. **`file.lock()`** 是**劝告式文件锁**（`File::lock` 在新版 Rust 标准库里可用）。`AmendError` 里专门有 `LockPolicyFile` 变体。这是必要的：两个并发的「批准」动作同时追加，会写进去两行一模一样的规则；锁把「读全文 + 判断 + 追加」变成一个临界区。
3. **去重按「整行相等」**（`contents.lines().any(|existing| existing == line)`）。于是同一条规则被批准两次只留一行——**幂等**。这个判断的前提是生成的文本对同一输入稳定：`serde_json::to_string` + 固定的参数顺序保证了这一点。
4. **缺尾换行时先补一个**（`if !contents.is_empty() && !contents.ends_with('\n')`）。这个细节防的是「文件被手工编辑过、最后一行没换行」的情况：不补的话新规则会接在最后一行后面变成 `... decision="allow"` 的一部分，整份文件语法错误。
5. 最后**总是写 `{line}\n`**，保证每次追加后文件以换行结尾。

`append_rule_line` 在调它之前先确保目录存在，且把「目录已存在」当成正常：

```
match std::fs::create_dir(dir) {
    Ok(()) => {}
    Err(ref source) if source.kind() == std::io::ErrorKind::AlreadyExists => {}
    Err(source) => return Err(AmendError::CreatePolicyDir { dir: dir.to_path_buf(), source }),
}
```

用 `create_dir` 而不是 `create_dir_all`，且显式忽略 `AlreadyExists`。理由大概是：父目录应当是已存在的（策略目录就在配置目录下），只需补一层。而 `AlreadyExists` 必须单独当成成功——否则第二次批准就会失败。

**两个函数的名字前缀 `blocking_` 与它们的 docstring 连在一起说明了一条使用纪律。**

```
/// Note this thread uses advisory file locking and performs blocking I/O, so it should be used with
/// [`tokio::task::spawn_blocking`] when called from an async context.
```

也就是说：这是同步 I/O + 文件锁，在异步上下文里必须包进 `spawn_blocking`，否则会阻塞运行时线程。把这一点写进 docstring 而不是靠类型标记，是因为「同步函数在异步环境里调用」在 Rust 里完全合法——编译器不会阻止你。

**错误类型本身是一份清单，读一遍就能知道这条路径上会出什么事。**

`AmendError` 有十个变体：`EmptyPrefix`、`InvalidNetworkRule`、`MissingParent`、`CreatePolicyDir`、`SerializePrefix`、`SerializeNetworkRule`、`OpenPolicyFile`、`WritePolicyFile`、`LockPolicyFile`、`SeekPolicyFile`、`ReadPolicyFile`、`PolicyMetadata`。每个都带具体的 `path` 与 `source`。

「每个 I/O 动作一个变体」这种做法让错误消息能精确到「哪一步、哪个文件、什么原因」。代价是枚举很长——但这里值得，因为策略文件写坏了的后果是「用户的批准没有生效，或者文件变成了非法语法」。

**`merge_overlay` 是一种「不破坏原策略的叠加」。**

```
pub fn merge_overlay(&self, overlay: &Policy) -> Policy {
    let mut combined_rules = self.rules_by_program.clone();
    for (program, rules) in overlay.rules_by_program.iter_all() {
        for rule in rules { combined_rules.insert(program.clone(), rule.clone()); }
    }
    let mut combined_network_rules = self.network_rules.clone();
    combined_network_rules.extend(overlay.network_rules.iter().cloned());
    ...
}
```

规则按程序名**追加**（`MultiMap` 的 `insert` 允许多值），网络规则**拼接**（于是覆盖式的后写胜自然生效），host 路径表用 `extend`（同名的后写覆盖）。三处的合并语义各自不同，但都符合各自的聚合方式——命令规则是多条并存后取最大，网络规则是同主机覆盖，host 表是一名字一集合。

`RequirementsExecPolicy` 那段还揭示了另一个问题：**策略的比较要顺序无关**。

```
pub fn fingerprint(&self) -> Vec<String> {
    let mut entries = Vec::new();
    for (program, rules) in self.policy.rules().iter_all() {
        for rule in rules { entries.push(format!("{program}:{rule:?}")); }
    }
    entries.sort();
    entries
}
impl PartialEq for RequirementsExecPolicy { fn eq(&self, other: &Self) -> bool { self.fingerprint() == other.fingerprint() } }
```

`MultiMap` 的迭代顺序未必稳定，所以直接比较会因插入顺序不同而判为不等。`fingerprint()` 把每条规则渲染成字符串、**排序**、再比较。这是「语义相等但表示顺序不同」这类问题里最省事的解法——比逐条双向包含判断简单，也不易写错。

代价是 `fingerprint` 是 `Vec<String>`（每次比较都要格式化所有规则）。而 `get_allowed_prefixes` 已经在做类似的事（它把模式渲染成字符串以便排序去重），说明「策略的比较与展示都走文本形态」在这个模块里是一致的做法。

## 关键取舍

**源码事实：** 求值有三条有序来源（精确规则 → 路径解析 → 启发式回退），每层用 `filter(!is_empty())` 把「空」转成「未命中」；`MatchOptions::resolve_host_executables` 默认 `false`。

**源码事实：** 路径解析命中会带 `resolved_program`；若策略里为该名字声明了 `host_executable` 路径集合，则只在该具体路径属于集合时才继续，否则返回未命中。

**源码事实：** `Evaluation::from_matches` 用 `.max()` 聚合决策（`Allow < Prompt < Forbidden`），并保留全部 `matched_rules`；`is_match()` 只认非启发式的匹配。

**源码事实：** 批量求值用 `flat_map` 拼所有匹配再一次聚合，于是「任一被禁 → 整体被禁」。

**源码事实：** 网络规则按「同主机后写覆盖」编译成 allowed/denied 两个列表，`Prompt` 不进任何列表；`upsert_domain` 会把主机移到末尾以保序去重。

**源码事实：** 修订路径用 `serde_json::to_string` 逐 token 转义拼出 Starlark 行；追加时用劝告式文件锁、整行去重、缺尾换行先补、`create_dir` 把 `AlreadyExists` 当成功。

**源码事实：** 两个 `blocking_*` 函数的 docstring 明确要求异步上下文里包 `spawn_blocking`——因为这既是同步 I/O 又用文件锁。

**源码事实：** `RequirementsExecPolicy` 的相等靠 `fingerprint()`（渲染 + 排序）来做到顺序无关。

**架构推论：** 「宽规则不能覆盖严规则」是安全策略的硬要求，而它在这里由「枚举派生顺序 + `.max()`」实现。若聚合写成「先命中者胜」，一条 `allow git` 排在 `forbid git push` 之前就会让后者失效——这正是很多自制策略引擎的真实漏洞。

**架构推论：** `host_executable` 是「可选收紧」的范本。默认按 basename 匹配（宽松、方便），一旦声明具体路径就变成白名单（可防同名伪造）。这种「默认可用、声明即收紧」的设计让安全性与易用性不必二选一。

**架构推论：** 「批准即写回文件」这条路径必须幂等且并发安全。整行去重保证了重复批准不膨胀文件，文件锁保证了并发批准不产生重复行，而序列化器保证了生成的代码永远合法。三件事缺一：没有去重则文件被同一行淹没；没有锁则两条相同规则交错写入；用字符串拼接则含引号的命令会写出语法错误文件。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/execpolicy/src/policy.rs` | `Policy` | 规则集持有者：按程序名索引 + 网络规则 + host 路径表 |
| `codex-rs/execpolicy/src/policy.rs` | `MatchOptions` | 默认关闭的 `resolve_host_executables` 开关 |
| `codex-rs/execpolicy/src/policy.rs` | `matches_for_command_with_options` | 三条有序来源 + 启发式兜底 |
| `codex-rs/execpolicy/src/policy.rs` | `match_host_executable_rules` | basename 再匹配 + `host_executable` 白名单检查 |
| `codex-rs/execpolicy/src/policy.rs` | `Evaluation` / `from_matches` | `.max()` 聚合决策并保留全部匹配 |
| `codex-rs/execpolicy/src/policy.rs` | `compiled_network_domains` | 同主机后写覆盖，输出 allowed/denied 两表 |
| `codex-rs/execpolicy/src/policy.rs` | `merge_overlay` | 三种不同的叠加语义（追加 / 拼接 / 覆盖） |
| `codex-rs/execpolicy/src/amend.rs` | `blocking_append_allow_prefix_rule` | 把批准的命令前缀写成一行 `prefix_rule` |
| `codex-rs/execpolicy/src/amend.rs` | `blocking_append_network_rule` | 同上，写成 `network_rule(...)` |
| `codex-rs/execpolicy/src/amend.rs` | `append_locked_line` | 加锁、整行去重、补尾换行的追加实现 |

## 参考

- [open-interpreter 源码：execpolicy/src/policy.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/execpolicy/src/policy.rs)
- [open-interpreter 源码：execpolicy/src/amend.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/execpolicy/src/amend.rs)
- [Rust 文档：`File::lock` 与劝告式文件锁](https://doc.rust-lang.org/std/fs/struct.File.html#method.lock)
- [open-interpreter 文档：沙箱与审批](https://www.openinterpreter.com/docs/terminal/sandbox)

## 与主核 / AI 的关系

「策略求值」与主核里的「权限判定」几乎逐条对应：多条策略同时命中时取最严格的那个、宽规则不得覆盖严规则、结果要带依据而不只是结论。这里的三条经验可以直接搬。

第一，**聚合必须是「更严格者胜」，而且要靠结构保证**。`Decision` 的 `Ord` 派生 + `.max()` 让这条规则不可能写错；而手写 `if has_forbidden {..} else if has_prompt {..}` 的版本会在「新增一档决策」时静默出错。主核的权限检查同样应当用「枚举顺序即严格性顺序」这种编码。

第二，**路径解析必须配白名单**。只按 basename 匹配会让 `/tmp/x/git` 继承 `git` 的规则——一个真实可用的绕过路径。`host_executable` 的「声明即收紧」把选择权交给策略作者：不声明则宽松，声明则精确。主核里凡是「按名字匹配对象」的地方（表名、扩展名、可执行程序）都有同样的伪造风险。

第三，**「批准即写回」必须幂等且用序列化器**。整行去重 + 文件锁 + `serde_json` 转义三件套缺一不可。主核里任何把用户决定持久化回配置文件的功能都适用：字符串拼接生成的配置在遇到含引号的值时会产生语法错误文件，而那种错误往往在下次启动时才暴露。

另一条更细的：**`fingerprint()` 排序后比较**是「语义相等」的廉价实现。主核在比较两棵执行计划或两套索引定义是否等价时，同样可以先渲染成规范文本再排序比较——比结构化双向比较省事，也不易漏掉顺序差异。
