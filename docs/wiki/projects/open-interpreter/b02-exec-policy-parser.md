---
project: open-interpreter
feature: exec-policy-parser
title: 策略文件解析与校验
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/execpolicy/src/parser.rs
    symbols: [PolicyParser, PolicyBuilder, PendingExampleValidation]
  - path: codex-rs/execpolicy/src/parser.rs
    symbols: [parse_pattern, parse_pattern_token, parse_network_rule_decision]
---

# 策略文件解析与校验

## 一句话职责

策略文件不是 JSON 而是 **Starlark**：`PolicyParser` 建一个带三个内置函数的 Starlark 模块环境，把 `prefix_rule` / `network_rule` / `host_executable` 三个调用逐条累积进 `PolicyBuilder`，并在**整个文件求值完之后**才跑那些附在规则上的正/负例校验。

## 机制是什么

用 Starlark（而不是 JSON/TOML）来做策略文件，换来三件东西：

| 能力 | 例子 |
|---|---|
| 表达式与循环 | 一次生成一批同类规则而不必手抄 |
| 字符串拼接 | `f"..."` 插值 |
| 内建函数带**语义校验** | 参数类型、模式形状、主机归一化都在求值期检查 |

代价是策略文件变成**可执行代码**——这不是安全问题（Starlark 被设计成无副作用、无 I/O），但它意味着「解析」不再是一次纯读取，而是一次求值。

`PolicyParser::parse` 的形状把这件事说得很清楚：

```
pub fn parse(&mut self, policy_identifier: &str, policy_file_contents: &str) -> Result<()> {
    let pending_validation_count = self.builder.borrow().pending_example_validations.len();
    let mut dialect = Dialect::Extended.clone();
    dialect.enable_f_strings = true;
    let ast = AstModule::parse(policy_identifier, policy_file_contents.to_string(), &dialect).map_err(Error::Starlark)?;
    let globals = GlobalsBuilder::standard().with(policy_builtins).build();
    Module::with_temp_heap(|module| {
        let mut eval = Evaluator::new(&module);
        eval.extra = Some(&self.builder);
        eval.eval_module(ast, &globals).map(|_| ()).map_err(Error::Starlark)
    })?;
    self.builder.borrow().validate_pending_examples_from(pending_validation_count)?;
    Ok(())
}
```

四步，其中第三、四步的顺序是这一页的关键：

1. **记下已有的待校验数量**。`PolicyParser` 是「解析多个文件、最后 `build()` 一次」的形态，所以解析第二个文件时，builder 里已经有第一个文件的待校验项了。若不加这个起点，第二个文件解析完会把第一个文件的校验再跑一遍。
2. **`Dialect::Extended` + 打开 `f_strings`**。扩展方言允许较现代的语法；f-string 显式开启（`enable_f_strings = true`），因为生成同类规则时靠它拼名字。
3. **求值**：`eval.extra = Some(&self.builder)` 把一个 `&RefCell<PolicyBuilder>` 塞进求值器，三个内置函数通过 `Evaluation.extra` 拿到它并写入。
4. **只校验「新增的」待校验项**：`validate_pending_examples_from(pending_validation_count)`。

## 怎么实现的

**`PolicyBuilder` 是三个集合加一个待办队列。**

```
struct PolicyBuilder {
    rules_by_program: MultiMap<String, RuleRef>,
    network_rules: Vec<NetworkRule>,
    host_executables_by_name: HashMap<String, Arc<[AbsolutePathBuf]>>,
    pending_example_validations: Vec<PendingExampleValidation>,
}
```

前三项最终对应 `Policy` 的三个字段（`from_parts` 原样搬运）。第四项是一个**延迟队列**：

```
struct PendingExampleValidation { rules: Vec<RuleRef>, matches: Vec<Vec<String>>, not_matches: Vec<Vec<String>>, location: Option<ErrorLocation> }
```

它存「这几条规则 + 它们该匹配/不该匹配的例子 + 在文件里的位置」。延迟的理由在 `validate_pending_examples_from` 的实现里：

```
for validation in &self.pending_example_validations[start..] {
    let mut rules_by_program = MultiMap::new();
    for rule in &validation.rules { rules_by_program.insert(rule.program().to_string(), rule.clone()); }
    let policy = Policy::from_parts(rules_by_program, Vec::new(), self.host_executables_by_name.clone());
    validate_not_match_examples(&policy, &validation.rules, &validation.not_matches)
        .map_err(|error| attach_validation_location(error, validation.location.clone()))?;
    validate_match_examples(&policy, &validation.rules, &validation.matches)
        .map_err(|error| attach_validation_location(error, validation.location.clone()))?;
}
```

三点：

1. **每条校验用「只含该批规则」的临时 `Policy`**，而不是整个 builder 的规则集。这样「这条规则该匹配这个例子」是**该规则自己的断言**，不会被别的规则顺带满足。若用全量策略，一条写坏的规则可能因为另一条规则恰好匹配而通过校验——校验就失去意义了。
2. **临时 Policy 的 `network_rules` 传空**。正负例只针对命令规则，网络规则没有「例子」的概念。
3. **`host_executables_by_name` 从 builder 克隆**。这是唯一带入的外部上下文：因为校验时 `resolve_host_executables: true`（见 b01），路径解析需要这张表。

`validate_pending_examples_from(start)` 的 `start` 切片就是这个「增量」机制的全部实现：解析 N 个文件时，第 N 次只校验第 N 次新增的那些。

**`prefix_rule` 是三个内置函数里最复杂的一个，它把一条 Starlark 调用展开成一批规则。**

签名有六个参数，其中四个可选：

```
fn prefix_rule<'v>(
    pattern: UnpackList<Value<'v>>,
    decision: Option<&'v str>,
    r#match: Option<UnpackList<Value<'v>>>,
    not_match: Option<UnpackList<Value<'v>>>,
    justification: Option<&'v str>,
    eval: &mut Evaluator<'v, '_, '_>,
) -> anyhow::Result<NoneType>
```

`r#match` 的原始名转义是必须的（`match` 是 Rust 关键字），而 Starlark 侧的参数名仍是 `match`。

处理顺序：

```
let decision = match decision { Some(raw) => Decision::parse(raw)?, None => Decision::Allow };
let justification = match justification {
    Some(raw) if raw.trim().is_empty() => return Err(Error::InvalidRule("justification cannot be empty".to_string()).into()),
    Some(raw) => Some(raw.to_string()),
    None => None,
};
let pattern_tokens = parse_pattern(pattern)?;
let matches: Vec<Vec<String>> = r#match.map(parse_examples).transpose()?.unwrap_or_default();
let not_matches: Vec<Vec<String>> = not_match.map(parse_examples).transpose()?.unwrap_or_default();
let location = eval.call_stack_top_location().map(error_location_from_file_span);
```

三处取舍：

- **`decision` 缺省是 `Allow`**。于是最简写法 `prefix_rule(pattern=["ls"])` 就是「允许 ls」。这条默认值是有风险的——一条忘写 `decision` 的规则会静默地成为允许项。但考虑到策略文件的主要用途就是放宽权限，这个默认方向是合理的。
- **空的 `justification` 报错而不是归一成 `None`**。`"justification": ""` 与「没写」在语义上应当不同：前者是「我写了但写空了」，后者是「我不打算解释」。把它报错迫使作者要么写一句有用的话，要么删掉这个参数。同一判断在 `network_rule` 里重复出现，在 `Policy::add_network_rule` 与 `blocking_append_network_rule` 里也各有一份——四处一致。
- **`location` 取自 `eval.call_stack_top_location()`**。也就是「这次内建函数调用的位置」，随后的错误会带上它（`attach_validation_location`）。

**首 token 的展开是「一条规则变多条」的关键一步。**

```
let mut builder = policy_builder(eval);
let (first_token, remaining_tokens) = pattern_tokens.split_first()
    .ok_or_else(|| Error::InvalidPattern("pattern cannot be empty".to_string()))?;
let rest: Arc<[PatternToken]> = remaining_tokens.to_vec().into();
let rules: Vec<RuleRef> = first_token.alternatives().iter().map(|head| {
    Arc::new(PrefixRule {
        pattern: PrefixPattern { first: Arc::from(head.as_str()), rest: rest.clone() },
        decision, justification: justification.clone(),
    }) as RuleRef
}).collect();
builder.add_pending_example_validation(rules.clone(), matches, not_matches, location);
rules.into_iter().for_each(|rule| builder.add_rule(rule));
```

`first_token.alternatives()` 让 `Single("git")` 与 `Alts(vec!["git","hg"])` 都变成一组字符串，于是写法统一。`rest.clone()` 是 `Arc<[PatternToken]>` 的廉价共享——多条规则共用同一份「其余位置」模式。

这解释了 b01 里那个「首 token 固定」的约束是怎么被绕开的：**策略文件里首 token 可以写成候选组，但解析后会被展开成 N 条独立的规则，每条的首 token 都是单个字符串**。索引的前提（首 token 唯一）得到了保持，而表达力没有损失。

**注意 `rules.clone()` 与 `rules.into_iter()` 的两次消费。**

先克隆一份给「待校验」（校验需要规则对象本身），再把原 Vec 逐条 `add_rule` 进索引。这是一个很实际的小事：校验队列需要持有规则以便事后构造临时 Policy，而索引需要拿走规则。

**`parse_pattern` 与 `parse_pattern_token` 是两层。**

```
fn parse_pattern<'v>(pattern: UnpackList<Value<'v>>) -> Result<Vec<PatternToken>> {
    let tokens: Vec<PatternToken> = pattern.items.into_iter().map(parse_pattern_token).collect::<Result<_>>()?;
    if tokens.is_empty() { Err(Error::InvalidPattern("pattern cannot be empty".to_string())) } else { Ok(tokens) }
}

fn parse_pattern_token<'v>(value: Value<'v>) -> Result<PatternToken> {
    if let Some(s) = value.unpack_str() { Ok(PatternToken::Single(s.to_string())) }
    else if let Some(list) = ListRef::from_value(value) {
        let tokens: Vec<String> = list.content().iter().map(|value| {
            value.unpack_str().ok_or_else(|| Error::InvalidPattern(format!("pattern alternative must be a string (got {})", value.get_type())))
                .map(str::to_string)
        }).collect::<Result<_>>()?;
        match tokens.as_slice() {
            [] => Err(Error::InvalidPattern("pattern alternatives cannot be empty".to_string())),
            [single] => Ok(PatternToken::Single(single.clone())),
            _ => Ok(PatternToken::Alts(tokens)),
        }
    } else {
        Err(Error::InvalidPattern(format!("pattern element must be a string or list of strings (got {})", value.get_type())))
    }
}
```

`parse_pattern_token` 的三支与三条归一化：

- 字符串 → `Single`；
- 列表 → 逐元素取字符串（非字符串报错，消息里带实际类型 `value.get_type()`），再按长度归并：**空列表报错**、**单元素列表折成 `Single`**、多元素成 `Alts`；
- 其它类型 → 报错并给出实际类型。

「单元素列表折成 `Single`」很重要：`pattern=["git", ["push"]]` 与 `pattern=["git", "push"]` 得到**同一个模式**。若不折叠，同一个语义会有两种内存表示，而 `get_allowed_prefixes` 的 `render_pattern_token` 会把 `Alts(["push"])` 渲染成 `[push]`——一个语义上正确但形状上意外的前缀。

**`network_rule` 比 `prefix_rule` 简单，但它把归一化放在解析期。**

```
fn network_rule<'v>(host: &'v str, protocol: &'v str, decision: &'v str, justification: Option<&'v str>, eval: &mut Evaluator<'v, '_, '_>) -> anyhow::Result<NoneType> {
    let protocol = NetworkRuleProtocol::parse(protocol)?;
    let decision = parse_network_rule_decision(decision)?;
    let justification = /* 同上：空串报错 */;
    let mut builder = policy_builder(eval);
    builder.add_network_rule(NetworkRule { host: crate::rule::normalize_network_rule_host(host)?, protocol, decision, justification });
    Ok(NoneType)
}
```

三件事：协议与决策在解析期就校验（错误立刻带位置），主机在入表前归一化。**注意 `decision` 在这里是必填**——与 `prefix_rule` 的缺省 `Allow` 不对称。合理的解释是：网络规则的默认值若取 `Allow`，一条忘写决策的规则会把某个主机全面放开，而这个风险比命令规则更大（网络访问的爆炸半径不是一条命令）。

`parse_network_rule_decision` 的额外词表已在 b01 讲过：在 `Decision::parse` 之前先接受 `"deny"`。

**`host_executable` 把「名字」与「路径」绑在一起校验。**

```
fn host_executable<'v>(name: &'v str, paths: UnpackList<Value<'v>>, eval: &mut Evaluator<'v, '_, '_>) -> anyhow::Result<NoneType> {
    validate_host_executable_name(name)?;
    let mut parsed_paths = Vec::new();
    for value in paths.items {
        let raw = value.unpack_str().ok_or_else(...)?;
        let path = parse_literal_absolute_path(raw)?;
        let Some(path_name) = executable_path_lookup_key(path.as_path()) else {
            return Err(Error::InvalidRule(format!("host_executable path `{raw}` must have basename `{name}`")).into());
        };
        if path_name != executable_lookup_key(name) {
            return Err(Error::InvalidRule(format!("host_executable path `{raw}` must have basename `{name}`")).into());
        }
        if !parsed_paths.iter().any(|existing| existing == &path) { parsed_paths.push(path); }
    }
    policy_builder(eval).add_host_executable(executable_lookup_key(name), parsed_paths);
}
```

四条纪律：

- **名字必须是裸名**（`validate_host_executable_name`）：非空、只有一个路径分量、且 `file_name()` 等于它本身。于是 `"git"` 合法，`"/usr/bin/git"` 与 `"./git"` 都不合法——名字是索引键，不能带路径。
- **路径必须绝对**（`parse_literal_absolute_path` 先查 `Path::is_absolute()`，再转 `AbsolutePathBuf`）。
- **路径的 basename 必须与名字匹配**。这条是这层的核心不变量：`host_executable(name="git", paths=["/usr/bin/git", "/opt/homebrew/bin/git"])` 合法，而混进 `/usr/bin/python3` 会报错。
- **同一路径只留一份**（去重）。

匹配时用两个不同的「lookup key」函数（`executable_path_lookup_key` 与 `executable_lookup_key`）比较，而不是直接用字符串相等。这两个函数来自 `executable_name` 模块，作用大概是抹平平台差异（Windows 的 `.exe` 后缀、大小写不敏感的文件系统）。**用同一套 key 函数生成索引与比较**，才能保证「写进去的」与「查出来的」对得上——这正是 b01 里说的「归一化必须单点」。

**`policy_builder(eval)` 的三层解包。**

```
fn policy_builder<'v, 'a>(eval: &Evaluator<'v, 'a, '_>) -> RefMut<'a, PolicyBuilder> {
    eval.extra.as_ref()
        .expect("policy_builder requires Evaluator.extra to be populated")
        .downcast_ref::<RefCell<PolicyBuilder>>()
        .expect("Evaluator.extra must contain a PolicyBuilder")
        .borrow_mut()
}
```

`extra` 是 `Option<&dyn Any>`，所以「有没有」「是不是那个类型」「能不能借到可变引用」三件事各要一次判断，三次都用 `expect` 而不是返回 `Result`——因为这三条在构造期就保证了（`parse` 里 `eval.extra = Some(&self.builder)`）。这里用 `expect` 是合理的：如果它失败，那是代码错误而不是输入错误。

**例子的解析有两条入口，两种形态。**

```
fn parse_example<'v>(value: Value<'v>) -> Result<Vec<String>> {
    if let Some(raw) = value.unpack_str() { parse_string_example(raw) }
    else if let Some(list) = ListRef::from_value(value) { parse_list_example(list) }
    else { Err(Error::InvalidExample(format!("example must be a string or list of strings (got {})", value.get_type()))) }
}

fn parse_string_example(raw: &str) -> Result<Vec<String>> {
    let tokens = shlex::split(raw).ok_or_else(|| Error::InvalidExample("example string has invalid shell syntax".to_string()))?;
    if tokens.is_empty() { Err(Error::InvalidExample("example cannot be an empty string".to_string())) } else { Ok(tokens) }
}
```

**字符串形态用 `shlex::split` 做分词**——于是 `match="git push origin main"` 会被切成 4 个 token，与命令规则的分词完全一致。这一步是必要的：策略规则匹配的是 **token 序列**，而例子若按空格简单切分，一个带引号的参数（`"git commit -m 'fix a bug'"`）就会被切成错误的 token 数，从而让校验得出错误结论。

`shlex::split` 返回 `Option`，`None` 表示语法错误（未闭合引号等）。空结果也报错——一个空例子里没有任何 token，无法表达任何断言。

**列表形态则要求每个元素是字符串**，且空列表报错。两条形态各有用途：字符串形态贴合用户实际输入的样子，列表形态便于逐 token 精确指定（避免引号与转义的歧义）。

**错误位置的补上有两层。**

`error_location_from_file_span` 把 Starlark 的 `FileSpan`（0-based）转成自己的 `ErrorLocation`（1-based）：

```
ErrorLocation {
    path: span.filename().to_string(),
    range: TextRange {
        start: TextPosition { line: resolved.begin.line + 1, column: resolved.begin.column + 1 },
        end: TextPosition { line: resolved.end.line + 1, column: resolved.end.column + 1 },
    },
}
```

两次 `+ 1` 是这里最容易被忽略的正确性细节：行号从 0 基转 1 基，列号同理。若漏了，用户看到的「第 0 行」会让人以为是文件开头。

`attach_validation_location` 在错误本没有位置时才补上：

```
fn attach_validation_location(error: Error, location: Option<ErrorLocation>) -> Error {
    match location { Some(location) => error.with_location(location), None => error }
}
```

于是 `validate_*_examples` 产生的位置为 `None` 的错误在这里被补成「`prefix_rule` 那一行的位置」。

## 关键取舍

**源码事实：** 策略文件是 Starlark，`Dialect::Extended` 且显式开启 `f_strings`；三个内置函数通过 `Evaluator.extra` 里的 `RefCell<PolicyBuilder>` 写入 builder。

**源码事实：** 正/负例校验**延迟**到文件求值完成后，且只校验本次新增的部分（靠 `pending_validation_count` 切片实现）；每条校验用「只含该批规则」的临时 `Policy`，网络规则传空，`host_executables` 从 builder 克隆。

**源码事实：** `prefix_rule` 的 `decision` 缺省为 `Allow`，而 `network_rule` 的 `decision` 必填。

**源码事实：** `justification` 传空串报错（而非归一成缺失），这一判断在四处一致。

**源码事实：** 首 token 的候选组在解析期被展开成多条独立规则（`first_token.alternatives()`），于是索引所需的「首 token 唯一」得到保持。

**源码事实：** `parse_pattern_token` 对单元素列表折成 `Single`、空列表报错、类型错误报错并附带实际类型；pattern 整体为空也报错。

**源码事实：** 字符串形态的例子走 `shlex::split` 分词，与命令分词一致；列表形态逐元素取字符串；两种形态空值都报错。

**源码事实：** 错误位置由 Starlark `FileSpan` 转成 1-based 的 `ErrorLocation`（行列各 `+ 1`）；校验期产生的位置为 `None` 的错误在 `attach_validation_location` 里补上调用点位置。

**架构推论：** 「延迟校验 + 只看该批规则」是让「规则自带例子」这件事真正有意义的关键。若校验时用全量策略，「该匹配」的例子可能被别的规则满足、「不该匹配」的例子里可能被别的规则命中——两种误判都让断言失效，而且失败方式是「校验通过但规则其实是错的」，最难发现。

**架构推论：** 「解析宽、归一化早」的组合让下游只面对一种形态。例子先 shlex 再比对、主机先归一化再入表、单元素列表先折叠再匹配——每一处都把「多种输入写法」在入口处收敛成一种内部表示。这样匹配层不必猜测输入来自哪种写法。

**架构推论：** 在可执行配置语言（Starlark）里做语义校验，等于给配置文件加了一层编译期检查。`prefix_rule(pattern=[])` 这种明显错误在求值期就被拒，而不会变成一条「永远匹配不上」的死规则留在策略里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/execpolicy/src/parser.rs` | `PolicyParser` | 建 Starlark 环境、求值、触发增量校验；`build()` 产出 `Policy` |
| `codex-rs/execpolicy/src/parser.rs` | `PolicyBuilder` | 四个集合：规则索引、网络规则、host 路径、待校验队列 |
| `codex-rs/execpolicy/src/parser.rs` | `PendingExampleValidation` | 延迟校验项：规则 + 正例 + 负例 + 位置 |
| `codex-rs/execpolicy/src/parser.rs` | `validate_pending_examples_from` | 用「只含该批规则」的临时 Policy 跑校验 |
| `codex-rs/execpolicy/src/parser.rs` | `policy_builtins` | 三个内置函数：`prefix_rule` / `network_rule` / `host_executable` |
| `codex-rs/execpolicy/src/parser.rs` | `parse_pattern` / `parse_pattern_token` | 模式解析与三种归一化（单元素折叠、空报错、类型报错） |
| `codex-rs/execpolicy/src/parser.rs` | `parse_network_rule_decision` | 额外接受 `deny` 的网络决策词表 |
| `codex-rs/execpolicy/src/parser.rs` | `parse_example` / `parse_string_example` | 例子解析：字符串走 `shlex`，列表逐元素 |
| `codex-rs/execpolicy/src/parser.rs` | `error_location_from_file_span` | Starlark 0-based 位置 → 1-based `ErrorLocation` |

## 参考

- [open-interpreter 源码：execpolicy/src/parser.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/execpolicy/src/parser.rs)
- [Starlark 语言规范](https://github.com/bazelbuild/starlark/blob/master/spec.md)
- [starlark-rust 文档](https://docs.rs/starlark/latest/starlark/)
- [open-interpreter 文档：沙箱与审批](https://www.openinterpreter.com/docs/terminal/sandbox)

## 与主核 / AI 的关系

「策略文件是一段可求值的脚本，而不是一堆死数据」这个选择，在主核里对应的是「用 SQL 或脚本表达权限策略」的路线。主核的权限策略通常是声明式的（谁对哪个对象有什么权限），但一旦需要「按模式批量生成」或「条件组合」，就会遇到表达力瓶颈。这里的做法给出了一个中间答案：**用受限的可执行语言 + 内建函数做语义校验**。

对 AI 工程的实际意义有三条。

第一，**校验必须只看「这条规则」**。`validate_pending_examples_from` 为每批规则造一个只含它们的临时策略，这个细节决定了「自带例子」是有用的还是自欺的。任何「规则自带测试」的机制都要先问：跑测试时的上下文是全局的还是局部的？全局上下文会让断言互相污染。

第二，**在入口处收敛输入的多种写法**。例子走 `shlex`（与命令分词一致）、主机先归一化、单元素列表折叠成单值。三处都是同一个策略：**匹配层只面对一种表示**。若把「字符串例子」在匹配时再分词，它必然与命令分词写在不同地方，早晚会出现两者结论不一致的情形。

第三，**可执行配置的默认值方向要按风险选**。`prefix_rule` 的 `decision` 缺省 `Allow`（因为策略文件的主要用途是放宽），而 `network_rule` 必填（因为网络放开的爆炸半径更大）。这个不对称不是不一致，而是「默认值应当与最坏后果对齐」的体现。
