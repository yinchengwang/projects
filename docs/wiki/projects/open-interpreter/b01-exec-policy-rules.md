---
project: open-interpreter
feature: exec-policy-rules
title: 执行策略规则与匹配
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/execpolicy/src/rule.rs
    symbols: [PrefixRule, NetworkRule, Decision]
  - path: codex-rs/execpolicy/src/rule.rs
    symbols: [PrefixPattern, PatternToken, normalize_network_rule_host]
---

# 执行策略规则与匹配

## 一句话职责

`rule.rs` 定义「一条策略规则长什么样」以及「它怎么判一条命令」：命令规则是**前缀模式**（首个 token 固定、其余每个位置是一个固定串或一组候选），网络规则是（主机、协议、决策）三元组，两种规则都只产出三种决策之一——允许、询问、禁止。

## 机制是什么

这套策略只回答一个问题：**这条命令（或这个网络目标）该怎么处置**。答案只有三档，且这三档的强弱是有序的：

```
pub enum Decision {
    /// Command may run without further approval.
    Allow,
    /// Request explicit user approval; rejected outright when running with `approval_policy="never"`.
    Prompt,
    /// Command is blocked without further consideration.
    Forbidden,
}
```

`Decision` 派生了 `Ord`（`#[derive(..., Ord, PartialOrd)]`），于是 `Allow < Prompt < Forbidden`。这个顺序不是随手加的——求值层用 `.max()` 把多条匹配的决策聚合成一个（见 b03）。也就是说**同一批匹配里只要有一条 `Forbidden`，整体就是 `Forbidden`**。「更严格者胜」这条聚合规则就编码在枚举的派生顺序里。

`Prompt` 的注释还交代了它在极端配置下的行为：在 `approval_policy="never"` 时它被直接拒绝。于是 `Prompt` 不是一个「安全档」，而是一个「需要人来决定」的档——当没有人可以问时，它就退化成拒绝。

**命令规则的形状是「前缀」，而且首个 token 特殊对待。**

```
pub struct PrefixPattern {
    pub first: Arc<str>,
    pub rest: Arc<[PatternToken]>,
}

pub enum PatternToken {
    Single(String),
    Alts(Vec<String>),
}
```

注释写着「首个 token 固定，因为我们按首个 token 在策略里建索引」。这是**为了索引而牺牲表达力**：首 token 不能是候选集合，因为策略表是以首 token 为键的 `MultiMap`。于是 `prefix_rule(pattern=["rm", "‑rf"], ...)` 里的 `"rm"` 必须是单个字符串，而 `pattern=["git", ["push", "pull"]]` 里的第二位置可以是候选组。

`PatternToken::Alts` 的存在解释了为什么需要 `alternatives()`：

```
pub fn alternatives(&self) -> &[String] {
    match self {
        Self::Single(expected) => std::slice::from_ref(expected),
        Self::Alts(alternatives) => alternatives,
    }
}
```

一个 `Single` 被视作「只有一个候选」。这个归一化让解析层可以**对首 token 统一展开**——见 b02 里 `prefix_rule` 内建的 `first_token.alternatives().iter().map(...)`：若首 token 写成 `["git"]` 这样的单元素列表，解析后是 `Single`，展开成一个规则；若写成多元素列表，会被 `parse_pattern_token` 折成 `Alts`，但**因为在首位，它其实不该出现**——那是索引带来的约束。

## 怎么实现的

**前缀匹配是「长度先卡、逐位比对」，返回命中的那一段前缀。**

```
impl PrefixPattern {
    pub fn matches_prefix(&self, cmd: &[String]) -> Option<Vec<String>> {
        let pattern_length = self.rest.len() + 1;
        if cmd.len() < pattern_length || cmd[0] != self.first.as_ref() {
            return None;
        }
        for (pattern_token, cmd_token) in self.rest.iter().zip(&cmd[1..pattern_length]) {
            if !pattern_token.matches(cmd_token) {
                return None;
            }
        }
        Some(cmd[..pattern_length].to_vec())
    }
}
```

四点：

1. **`cmd.len() < pattern_length` 先判**，避免后面的切片越界。注意它允许命令比模式更长——前缀匹配的语义就是「开头对上即可」，`["git", "push"]` 规则能匹配 `git push origin main`。
2. **`cmd[0] != self.first.as_ref()`**：首 token 直接比较字符串，不经过 `PatternToken`。这与「首 token 固定以便建索引」一致。
3. **`zip(&cmd[1..pattern_length])`** 逐位比对 `rest`，长度已经保证不会越界。
4. **返回值是 `cmd[..pattern_length].to_vec()`**，即命令里实际匹配的那一段。它不是模式本身——而是「命令的前 N 个 token 原文」。这个区别在界面与审计上有用：用户可以被告知「就是这几个词触发了规则」。

`PatternToken::matches` 的两支很直白：`Single` 相等比较，`Alts` 任一相等即命中。

**`PrefixRule` 只是把模式、决策、理由捆在一起。**

```
pub struct PrefixRule { pub pattern: PrefixPattern, pub decision: Decision, pub justification: Option<String> }
```

`justification` 的注释说明它「可供给任何决策，并可能在不同语境里浮现（例如提示理由或拒绝消息）」。也就是说它既用于给用户看的解释，也可能用于给模型的解释——一份文本两处使用。

`impl Rule for PrefixRule` 把匹配结果包成 `RuleMatch::PrefixRuleMatch`：

```
fn matches(&self, cmd: &[String]) -> Option<RuleMatch> {
    self.pattern.matches_prefix(cmd).map(|matched_prefix| RuleMatch::PrefixRuleMatch {
        matched_prefix, decision: self.decision, resolved_program: None, justification: self.justification.clone(),
    })
}
```

`resolved_program: None` 是一个占位：真正的路径在「按路径解析」那条匹配路径上才被填（见 `with_resolved_program`）。于是同一个规则可以产出两种匹配结果——一种基于命令里写的名字，一种基于解析出的绝对路径。

**`RuleMatch` 是两种来源的联合，不只是两种规则。**

```
pub enum RuleMatch {
    PrefixRuleMatch { matched_prefix, decision, resolved_program: Option<AbsolutePathBuf>, justification },
    HeuristicsRuleMatch { command: Vec<String>, decision },
}
```

`PrefixRuleMatch` 来自策略文件里的规则；`HeuristicsRuleMatch` 来自**启发式回退**——没有规则命中时，由一个外部函数给出一个决策（见 b03 的 `HeuristicsFallback`）。两者放进同一个枚举，于是求值层不必关心决策来自哪里，只要「有个决策」。

`RuleMatch::decision()` 与 `with_resolved_program()` 都是对这两支的适配：

```
pub fn with_resolved_program(self, resolved_program: &AbsolutePathBuf) -> Self {
    match self {
        Self::PrefixRuleMatch { matched_prefix, decision, justification, .. } =>
            Self::PrefixRuleMatch { matched_prefix, decision, resolved_program: Some(resolved_program.clone()), justification },
        other => other,
    }
}
```

`other => other` 让 `HeuristicsRuleMatch` 原样返回——启发式匹配没有程序路径的概念，所以这个操作对它是恒等。这类「只对一支有效的变换」用 `other => other` 落地是合适的，比强行给启发式匹配也加一个 `Option` 字段要好。

`Rule` 这个 trait 的层面值得一提：

```
pub trait Rule: Any + Debug + Send + Sync {
    fn program(&self) -> &str;
    fn matches(&self, cmd: &[String]) -> Option<RuleMatch>;
    fn as_any(&self) -> &dyn Any;
}
```

`as_any` 加 `Any` 是为了**向下转型**：`Policy::get_allowed_prefixes` 里用 `rule.as_any().downcast_ref::<PrefixRule>()` 去取具体类型，因为要读 `pattern` 而 trait 上没有这个方法。这是一个明确的设计取舍——trait 保持窄（只有 `program` 与 `matches`），具体能力靠 downcast 拿。代价是任何新增规则类型都要在需要它的地方加一个 downcast 分支。

**网络规则的主机归一化是一段独立的、值得细读的逻辑。**

```
pub(crate) fn normalize_network_rule_host(raw: &str) -> Result<String> {
    let mut host = raw.trim();
    if host.is_empty() { return Err(Error::InvalidRule("network_rule host cannot be empty".to_string())); }
    if host.contains("://") || host.contains('/') || host.contains('?') || host.contains('#') {
        return Err(Error::InvalidRule("network_rule host must be a hostname or IP literal (without scheme or path)".to_string()));
    }
    ...
}
```

它按顺序做了六件事：

1. **非空**；
2. **拒绝携带 scheme 或路径**（`://`、`/`、`?`、`#`）——注释说「必须是主机名或 IP 字面量（不带 scheme 或路径）」。这条很关键：`https://example.com/api` 不是主机，是 URL；把它存成规则会导致匹配永远失败（且失败得很安静）。
3. **方括号 IPv6**：若是 `[` 开头，则在 `]` 处分割，并允许 `]` 后跟 `:端口`（端口必须全数字且非空），其余后缀一律报错，然后 host 取方括号里的内容；
4. **单冒号的 `host:port`**：只在冒号恰好一个、两侧都非空、右侧全数字时才剥离端口。用 `host.matches(':').count() == 1` 判断——这是为了不误伤裸 IPv6（它有多个冒号）。
5. **去尾点、小写**：`host.trim_end_matches('.').trim().to_ascii_lowercase()`。DNS 的完全限定名可以带尾点（`example.com.`），而域名大小写不敏感。
6. **拒绝通配与空白**：`*` 一律不许（注释「必须是具体主机；不允许通配」），任何空白不许。

第 6 条的两项各防一类问题。**通配**会让「允许 `*.example.com`」变成一个无法审计的宽口子——而这个策略库的定位是精确的、可以逐条说明的规则集，所以它宁可让调用方写多条具体主机。**空白**则是因为主机名里不可能有空白，出现它说明输入被污染。

归一化之后的规则对象保存的就是这个规范化字符串：

```
pub struct NetworkRule { pub host: String, pub protocol: NetworkRuleProtocol, pub decision: Decision, pub justification: Option<String> }
```

`NetworkRuleProtocol` 有一个额外的兼容注意点：

```
pub fn parse(raw: &str) -> Result<Self> {
    match raw {
        "http" => Ok(Self::Http),
        "https" | "https_connect" | "http-connect" => Ok(Self::Https),
        "socks5_tcp" => Ok(Self::Socks5Tcp),
        "socks5_udp" => Ok(Self::Socks5Udp),
        other => Err(Error::InvalidRule(format!("network_rule protocol must be one of http, https, socks5_tcp, socks5_udp (got {other})"))),
    }
}
```

三个输入拼写映射到同一个 `Https`：`https`、`https_connect`、`http-connect`。前两个是同一概念的两种写法（后者是 CONNECT 通道的语义），第三个是历史拼法。而 `as_policy_string` 只回吐规范名：

```
Self::Https => "https"
```

于是**解析宽、序列化窄**：读的时候接受三种写法，写回策略文件时只写一种。这个不对称是做兼容的正确姿势——它让旧文件还能用，同时让新写出的文件逐步收敛到一种拼法。

**`NetworkRuleProtocol` 与 `parse_network_rule_decision` 是两处不同的词表。**

`Decision::parse` 只接受 `"allow"` / `"prompt"` / `"forbidden"`：

```
pub fn parse(raw: &str) -> Result<Self> {
    match raw { "allow" => Ok(Self::Allow), "prompt" => Ok(Self::Prompt), "forbidden" => Ok(Self::Forbidden),
                other => Err(Error::InvalidDecision(other.to_string())) }
}
```

而网络规则的决策在解析层另有一层包装（见 b02 的 `parse_network_rule_decision`），把 `"deny"` 映射成 `Forbidden`：

```
fn parse_network_rule_decision(raw: &str) -> Result<Decision> {
    match raw { "deny" => Ok(Decision::Forbidden), other => Decision::parse(other) }
}
```

也就是说网络规则接受 `deny` 这个额外的拼法。这不是不一致，而是**按上下文放宽**：网络策略的通行写法是 allow/deny，命令策略的通行写法是 allow/prompt/forbidden。解析时各自迁就各自的习惯，内部只保留一种枚举。

**`validate_match_examples` / `validate_not_match_examples` 是规则的「自带测试」。**

策略文件可以给一条规则附上正例与负例（`match=[...]` / `not_match=[...]`）。这两个函数在加载时把它们跑一遍：

```
pub(crate) fn validate_not_match_examples(policy: &Policy, _rules: &[RuleRef], not_matches: &[Vec<String>]) -> Result<()> {
    let options = MatchOptions { resolve_host_executables: true };
    for example in not_matches {
        if let Some(rule) = policy.matches_for_command_with_options(example, /*heuristics_fallback*/ None, &options).first() {
            return Err(Error::ExampleDidMatch { rule: format!("{rule:?}"), example: try_join(...), location: None });
        }
    }
    Ok(())
}
```

两个函数各守一个方向：`not_match` 里的例子**不允许命中任何规则**（第一条命中就报错），`match` 里的例子**必须至少命中一条**（否则收集起来一并报错）。

三处细节：

- **`heuristics_fallback` 传 `None`**。这很重要：负例校验必须只看策略规则，不能把启发式的结果算进来，否则一条负例可能因为启发式判了 `Allow` 而被误认为「命中」并报错。传入 `None` 也让 `matches_for_command_with_options` 的返回值可以为空——这正是 `not_match` 需要的。
- **`resolve_host_executables: true`**。校验时把可执行文件的路径解析也打开，于是「用绝对路径写的命令」与「用裸名写的命令」在同一套规则下得到一致结论。若不打开，一条 `not_match=["/usr/bin/git", "push"]` 的负例可能因为没匹配到（因为规则按裸名 `git` 索引）而**错误地通过**校验。
- **`_rules` 参数在 `validate_not_match_examples` 里未被使用**（下划线就是标记），只在 `validate_match_examples` 的错误消息里用来列出规则。这是两处签名对齐留下的痕迹。

**`Location: None`** 在两处错误里都填 `None`，而实际的位置由解析层补上（`attach_validation_location`）。于是同一个错误类型既可用于「有位置的解析期校验」也可用于「无位置的独立调用」。

## 关键取舍

**源码事实：** `Decision` 派生 `Ord`，顺序是 `Allow < Prompt < Forbidden`；求值层用 `.max()` 聚合，于是「更严格者胜」。

**源码事实：** `Prompt` 在 `approval_policy="never"` 时被直接拒绝——它不是安全档，而是「需要人来决定」的档。

**源码事实：** `PrefixPattern` 的首 token 是固定的 `Arc<str>` 而非 `PatternToken`，注释明确原因是「按首 token 建索引」；`PatternToken::alternatives()` 把单值归一成单元素切片，供解析层统一展开。

**源码事实：** `Rule` trait 只有 `program` / `matches` / `as_any`，具体能力靠 `downcast_ref` 取；`get_allowed_prefixes` 就依赖这个 downcast 去读 `pattern`。

**源码事实：** `normalize_network_rule_host` 拒绝 scheme/路径/通配/空白，剥端口（方括号 IPv6 与单冒号两种形态），去尾点并小写；端口只能全数字。

**源码事实：** 协议解析接受 `https`、`https_connect`、`http-connect` 三种写法，`as_policy_string` 只回 `https`——解析宽、序列化窄。网络规则的决策另接受 `deny`。

**源码事实：** `validate_not_match_examples` 以 `heuristics_fallback = None` 调用，因为负例校验必须只看策略规则；两处校验都用 `resolve_host_executables: true` 使裸名与绝对路径得到一致结论。

**架构推论：** 「决策三档 + 枚举派生的顺序」把聚合规则编进了类型。这比在聚合点手写一个 `if 有 Forbidden { .. } else if 有 Prompt { .. }` 好：新增一档只需插在合适的位置，而所有 `.max()` 的调用点自动得到正确行为。

**架构推论：** 「首 token 固定」是为了索引而牺牲表达力的典型。它换来的是 O(1) 的规则查找（`MultiMap` 按首 token 直接取一组候选）；代价是 `pattern=[["git","hg"], "push"]` 这种「两种工具同一条规则」必须写成两条。在策略条目数量级（几十到几百）下这个取舍是值的。

**架构推论：** 归一化函数把「什么样的输入算合法主机」的判断集中在一处，而不是散在解析与匹配两端。于是「存进去的 host 与匹配时比较的 host 是同一个规范形态」成为结构上的保证，不需要两端各写一遍同样的规则。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/execpolicy/src/decision.rs` | `Decision` | `Allow` / `Prompt` / `Forbidden`，派生了 `Ord` 以支持 `.max()` 聚合 |
| `codex-rs/execpolicy/src/rule.rs` | `PatternToken` | 单值或候选组；`alternatives()` 归一成切片 |
| `codex-rs/execpolicy/src/rule.rs` | `PrefixPattern` | 首 token 固定 + 其余逐位模式；`matches_prefix` 返回命令的命中段 |
| `codex-rs/execpolicy/src/rule.rs` | `PrefixRule` | 模式 + 决策 + 可选的 `justification` |
| `codex-rs/execpolicy/src/rule.rs` | `RuleMatch` | 策略匹配与启发式匹配的联合，各带自己的决策 |
| `codex-rs/execpolicy/src/rule.rs` | `Rule` / `RuleRef` | 窄 trait + `as_any` 下转型；引用是 `Arc<dyn Rule>` |
| `codex-rs/execpolicy/src/rule.rs` | `NetworkRule` | 归一化主机 + 协议 + 决策 + 理由 |
| `codex-rs/execpolicy/src/rule.rs` | `normalize_network_rule_host` | 拒绝 scheme/路径/通配/空白，剥端口，去尾点并小写 |
| `codex-rs/execpolicy/src/rule.rs` | `validate_match_examples` / `validate_not_match_examples` | 规则自带正/负例的加载期校验 |

## 参考

- [open-interpreter 源码：execpolicy/src/rule.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/execpolicy/src/rule.rs)
- [open-interpreter 文档：沙箱与审批](https://www.openinterpreter.com/docs/terminal/sandbox)
- [Rust 文档：`Ord` 与 `max`](https://doc.rust-lang.org/std/cmp/trait.Ord.html)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)

## 与主核 / AI 的关系

「策略是一组前缀规则 + 三档决策」在主核里对应的是权限与资源策略：主核也要回答「这次操作该放行、该确认、还是该拒」。这里有三条可直接借用的经验。

第一，**把聚合规则编进类型的顺序**。三档决策派生了 `Ord`，于是「更严格者胜」不需要在聚合点写分支，而是 `max()` 的自然结果。主核里若有多条策略同时命中（例如配额策略与权限策略），同样可以让「更严格」=「枚序列更大」，从而把聚合变成一次 `.max()`。这样做的好处是：新增档位时，只要插对位置，所有聚合点自动正确。

第二，**归一化必须是单点的**。`normalize_network_rule_host` 把「什么算合法主机」的判断集中在解析入口，于是「写入的形态」与「比较的形态」不可能分叉。主核里同样的陷阱是「表名/路径的规范化」——若写入端存原始字符串、读取端比较规范化字符串，就会出现「规则明明写了却不生效」这类极难查的问题。

第三，**规则可以自带正负例并在加载时校验**。这是把「我以为这条规则会匹配什么」变成可执行的断言。`not_match` 用 `heuristics_fallback = None` 调用这一点尤其值得记：**校验负例时必须关掉所有兜底**，否则兜底给出的决策会让负例「命中」并误报。
