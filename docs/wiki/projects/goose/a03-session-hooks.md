---
project: goose
feature: session-hooks
title: 会话钩子文件与规则选择
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/hooks/mod.rs
    symbols: [HookEvent, HooksFile, RawHookRule, LoadedRule]
  - path: crates/goose/src/hooks/mod.rs
    symbols: [select_action, HookDecision, OnFailure, ActionSelection]
---

# 会话钩子文件与规则选择

## 一句话职责

`hooks/mod.rs` 把插件目录里的 `hooks/hooks.json` 解析成「事件 → 规则 → 命令动作」的三层结构：宽容地忽略不认识的事件与动作类型，把匹配器编译成正则，把每个动作绑上一个超时与 `on_failure` 处置策略，然后提供「只记录」与「可拦截」两种触发方式。

## 机制是什么

钩子文件遵循 Open Plugins 的 hooks 规格，位置固定为**某个已启用插件的** `<plugin-root>/hooks/hooks.json`。形状是事件名映射到规则数组，每条规则有自己的匹配器与动作数组：

```json
{
  "hooks": {
    "PostToolUse": [
      { "matcher": "developer__shell|developer__text_editor",
        "hooks": [ { "type": "command", "command": "${PLUGIN_ROOT}/scripts/log.sh" } ] }
    ]
  }
}
```

模块 docstring 把三条契约写在了最前面：goose 目前只支持 `type: "command"`；**未知事件名与未知动作类型按规格忽略**；钩子脚本从 stdin 收到 JSON 事件上下文，成功时应以 0 退出。

「忽略」这个词在这里有两种截然不同的实现，而它们的区别就是这一页的主线：

| 情形 | 处置 |
|---|---|
| 未知的**事件名** | `debug!` 后跳过该事件 |
| 未知的**动作类型**（如 `"type": "webhook"`） | `debug!` 后标记 `Ignored`，跳过该动作 |
| 动作**结构错误**（`type` 不是字符串、`command` 不是字符串） | `anyhow::bail!`，整个文件加载失败 |
| 匹配器正则编译失败 | `warn!` 后**跳过整条规则**，其余照常 |
| 单个插件文件读取/解析失败 | `warn!` 后跳过该插件，其余插件照常 |

也就是说宽容度是分层的：**未来的扩展点要宽容**（未知事件/类型），**写错了要严格**（结构不合法），**坏了一部分不要拖垮全部**（正则、单插件）。这三条判据合起来，恰好覆盖了一个用户可编辑配置文件的全部失败模式。

## 怎么实现的

**`HookEvent` 的字符串映射是双向手写的。**

```
impl HookEvent {
    fn name(&self) -> &'static str { match self { HookEvent::PreToolUse => "PreToolUse", ... } }
    fn from_name(name: &str) -> Option<Self> { match name { "PreToolUse" => Some(HookEvent::PreToolUse), ... , _ => return None } }
}
impl std::fmt::Display for HookEvent { fn fmt(&self, f) { f.write_str(self.name()) } }
```

十二个事件、两张 `match`，没有用 `strum` 之类的派生。这是刻意的保守：这些字符串是**外部配置文件里的协议名**，不能用内部命名习惯去自动推导。一旦 `Display` 与 `from_name` 由同一个过程生成，一个重命名就会同时改变读写两端，而磁盘上的旧文件立刻失配；手写两张表则强制你在改的时候看一眼两边。

`from_name` 返回 `Option` 而不是错误，因为「不认识的事件」按规格就是要忽略。

**`HooksFile` 用 `Value` 而不是具体类型，是有原因的。**

```
#[derive(Debug, Default, Deserialize)]
struct HooksFile {
    #[serde(default)]
    hooks: HashMap<String, Value>,
}
```

`hooks` 的值是 `serde_json::Value` 而不是 `Vec<RawHookRule>`。原因是**逐事件分别反序列化**：先看事件名认不认识，认了才把 payload 解析成 `Vec<RawHookRule>`。若直接在结构上声明具体类型，一个不认识的事件里只要有一条规则结构不对，整个文件的解析就会失败——而规格要求忽略那个事件。

```
let Some(event) = HookEvent::from_name(&event_name) else {
    debug!(plugin = plugin_name, event = %event_name, "Ignoring unknown hook event");
    continue;
};
let raw_rules: Vec<RawHookRule> = serde_json::from_value(raw_payload)
    .with_context(|| format!("reading `{event_name}` rules in {}", path.display()))?;
```

这行 `?` 的位置也说明了分层：**已认识的事件，规则数组必须合法**——一个拼错的 `hooks` 键不会静默消失。

`#[serde(default)]` 让整个 `hooks` 键缺失等价于空 map，于是「建了插件目录但还没写钩子」是一个合法状态。

**`select_action` 把「类型判定」与「结构判定」分成两个 `match`。**

```
fn select_action(action: &Value, plugin_name: &str, path: &Path) -> Result<ActionSelection> {
    let Some(obj) = action.as_object() else {
        anyhow::bail!("hook action in {} must be an object", path.display());
    };
    match obj.get("type") {
        None | Some(Value::Null) => {}
        Some(Value::String(action_type)) if action_type == "command" => {}
        Some(Value::String(action_type)) => { debug!(...); return Ok(ActionSelection::Ignored); }
        Some(_) => anyhow::bail!("hook action `type` in {} must be a string", path.display()),
    }
    match obj.get("command") {
        None | Some(Value::Null) => { debug!(...); Ok(ActionSelection::Ignored) }
        Some(Value::String(_)) => Ok(ActionSelection::Selected),
        Some(_) => anyhow::bail!("hook action `command` in {} must be a string", path.display()),
    }
}
```

两处细节值得记：

- **`type` 缺失或为 `null` 都当成 `command`**（第一个空的 match 分支）。规格里 `type` 可能是可选的，缺省即命令；而 `null` 与缺失分开列出来，说明作者不信任「JSON 里没写 null 就等于没有」这个假设——手写文件里出现 `"type": null` 是很常见的。
- **`command` 缺失时是 `Ignored` 而不是错误**。这与 `command` 类型错误时的 `bail!` 形成对比：**缺失**是一种合法的不完整（也许别的东西会补上），**类型错**则是明确的书写错误。

`ActionSelection` 只有 `Selected` / `Ignored` 两个变体，且 derive 了 `PartialEq`（测试要比较）。它把「这个动作要不要采」这件事从布尔里拎出来变成具名枚举——调用点写 `== ActionSelection::Ignored` 比 `if !kept` 之类可读。

**`on_failure` 的自定义反序列化器在区分「缺失」与「存在但为 null」。**

```
#[serde(default, deserialize_with = "deserialize_present_on_failure")]
on_failure: Option<Value>,

fn deserialize_present_on_failure<'de, D>(d: D) -> Result<Option<serde_json::Value>, D::Error>
where D: serde::Deserializer<'de> {
    serde_json::Value::deserialize(d).map(Some)
}
```

`serde_json::Value` 本身能反序列化 `null`（成为 `Value::Null`），所以「字段存在但值是 null」会变成 `Some(Value::Null)`，而字段缺失（配合 `#[serde(default)]`）是 `None`。**如果没有这个包装，两者都是 `None`**，就无从区分。

为什么要区分？因为下一层要把它转成 `OnFailure` 枚举，而 `serde_json::from_value::<OnFailure>(Value::Null)` 会失败：

```
let on_failure = if event == HookEvent::PreToolUse {
    match action.on_failure {
        None => OnFailure::Allow,
        Some(value) => serde_json::from_value::<OnFailure>(value)
            .with_context(|| format!("reading on_failure in {}", path.display()))?,
    }
} else {
    OnFailure::Allow
};
```

于是「写了 `"on_failure": null`」会得到一个带文件路径的错误，而不是被静默当成缺失。这是一条「配置错误要报出来」的选择，与前面 `select_action` 里对 `type: null` 的宽容形成了有趣的对照——差别在于 `type: null` 有明确的默认语义，而 `on_failure: null` 没有。

同一段代码还有另一条**语义上的**取舍：`on_failure` **只在 `PreToolUse` 事件上被读取**，其它事件一律强制成 `Allow`。原因在后面的 `apply_verdict` 里可以看到——只有 `PreToolUse` 才有「拦下来」的意义。也就是说一个在 `PostToolUse` 上写了 `"on_failure": "block"` 的配置不会报错，但也不起作用。这处静默是本页最值得批评的地方之一：它违反了「写错了要严格」的原则，而理由大概是不想让用户为一个无效字段而整份配置失败。

**`OnFailure` 是 `#[serde(rename_all = "lowercase")]` 的两值枚举，默认 `Allow`。**

```
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
enum OnFailure {
    #[default]
    Allow,
    Block,
}
```

默认 `Allow` 这个方向很重要：**没写策略 = 钩子挂了也不拦**。若默认是 `Block`，任何一次钩子脚本崩溃都会把用户的所有工具调用锁死——一个可用性问题。

`LoadedAction::on_failure()` 用一个不可失败的 `let ... else` 解构取值：

```
fn on_failure(&self) -> OnFailure {
    let LoadedAction::Command { on_failure, .. } = self;
    *on_failure
}
```

因为目前只有一个变体，这个 `let` 是「永真」的。它的价值在将来：加了第二种动作类型时，编译器会在这里报错，强迫你决定新类型的默认处置。

**`LoadedRule` 与 `RawHookRule` 的差别不只是「解析前/解析后」。**

```
struct RawHookRule { matcher: Option<String>, hooks: Vec<Value> }

struct LoadedRule {
    plugin_name: String,
    plugin_root: PathBuf,
    matcher: Option<Regex>,
    actions: Vec<LoadedAction>,
}
```

`LoadedRule` 多了三样东西：`plugin_name`（用于日志与拦截消息里的归属）、`plugin_root`（用于展开 `${PLUGIN_ROOT}` 与作为 cwd）、以及把 `Option<String>` 编译成的 `Option<Regex>`。也就是说加载阶段完成了三件「之后运行时不该再做的事」：身份绑定、路径绑定、正则编译。

匹配器的编译与失败处置：

```
let matcher = match raw.matcher.as_deref().filter(|s| !s.is_empty()) {
    Some(pattern) => match Regex::new(pattern) {
        Ok(re) => Some(re),
        Err(err) => { warn!(...); continue; }
    },
    None => None,
};
```

`.filter(|s| !s.is_empty())` 让空字符串等价于「没有匹配器」——否则 `""` 会被编译成一个匹配一切的正则，语义就反了。而编译失败用 `continue` 跳过**整条规则**（而不是退化成「不匹配」）：一条匹配器坏了的规则，按「不匹配」处理会让它的钩子静默不执行；跳过并打警告，至少日志里有痕迹。两者结果其实相同（钩子都不会跑），但后者可诊断。

**加载失败的隔离粒度是「一个插件」。**

```
for plugin in plugins {
    let hooks_path = plugin.root.join("hooks").join("hooks.json");
    if !hooks_path.is_file() { continue; }
    match load_hooks_file(&hooks_path, &plugin.name, &plugin.root) {
        Ok(loaded) => { for (event, plugin_rules) in loaded { total += plugin_rules.len(); rules.entry(event).or_default().extend(plugin_rules); } }
        Err(err) => warn!(plugin = %plugin.name, path = %hooks_path.display(), error = %err, "Failed to load plugin hooks; skipping"),
    }
}
```

一个插件文件坏掉只丢它自己的规则，其它插件全部照常。最后若 `total > 0` 打一条 info 日志，带 `rule_count` 与**涉及的事件名列表**——`rules.keys().map(|e| e.name())`。后者对排障很实用：用户能看到「我写的那个事件到底被认出来了没有」。

`HookManager` 的规则表是 `Arc<HashMap<HookEvent, Vec<LoadedRule>>>`，因此 `#[derive(Clone)]` 是廉价的引用克隆——上层可以随意持有副本，不必担心复制成本。

## 关键取舍

**源码事实：** 宽容度是分层的：未知事件名与未知动作类型按规格忽略（`debug` 级），动作结构错误直接失败（`bail!`），正则编译失败只跳过该规则（`warn`），单插件文件失败只跳过该插件（`warn`）。

**源码事实：** 每个事件的规则数组是**逐事件**解析的（`HashMap<String, Value>`），所以一个不认识的事件里写坏规则不会影响整个文件。

**源码事实：** `HookEvent` 的 `name()` 与 `from_name()` 是手写两张 `match`，不用派生——因为这些字符串是磁盘上的协议名，两端必须能独立审视。

**源码事实：** `type` 缺失或 `null` 都算 `command`；`command` 缺失算「忽略」，而 `command` 类型错误算「失败」。缺失与类型错被严格区分。

**源码事实：** `on_failure` 用了自定义反序列化器，使「字段缺失」(`None`) 与「字段为 null」(`Some(Value::Null)`) 可区分；后者会在转换时报错。

**源码事实：** `on_failure` 只在 `PreToolUse` 上被读取，其它事件强制成 `Allow`——写在别的事件上不会报错，但也不生效。

**源码事实：** `OnFailure` 的默认值是 `Allow`，于是钩子崩溃默认不拦工具调用。

**源码事实：** 匹配器空字符串等价于「无匹配器」，编译失败则跳过整条规则；`LoadedRule` 在加载期完成了身份、路径与正则的绑定，运行时不再做这些。

**架构推论：** 「宽容度分层」是配置文件解析的通用正确姿势，但这里的两个例外值得警惕：`on_failure` 在非 `PreToolUse` 事件上被静默忽略，属于「宽容过度」。判据可以更简单——**有默认语义的缺失可以宽容，没有默认语义的赋值应当报错**。

**架构推论：** 把 `RawHookRule` 与 `LoadedRule` 分成两个类型，使「解析出来的东西」与「可执行的东西」在类型层面不同。`LoadedRule` 的每个字段都是运行必需，而 `RawHookRule` 的每个字段都可能是脏的。这个二分让运行时代码不必再处理 `Option<String>` 的正则、不必再拼路径。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/hooks/mod.rs` | `HookEvent` | 十二个生命周期事件，手写双向字符串映射 |
| `crates/goose/src/hooks/mod.rs` | `HooksFile` | 顶层文件结构：事件名 → 未解析的 `Value` |
| `crates/goose/src/hooks/mod.rs` | `RawHookRule` | 解析前的规则：字符串匹配器 + 未解析的动作数组 |
| `crates/goose/src/hooks/mod.rs` | `LoadedRule` | 解析后的规则：插件身份、根路径、编译好的正则、动作 |
| `crates/goose/src/hooks/mod.rs` | `select_action` | 判定动作是否采信：未知类型忽略、结构错误失败 |
| `crates/goose/src/hooks/mod.rs` | `ActionSelection` | `Selected` / `Ignored` 两态，让调用点自解释 |
| `crates/goose/src/hooks/mod.rs` | `OnFailure` | `Allow` / `Block`，默认 `Allow`，仅 `PreToolUse` 生效 |
| `crates/goose/src/hooks/mod.rs` | `HookDecision` | `Allow` 或 `Deny { reason, plugin }`，拦截的公开结果 |
| `crates/goose/src/hooks/mod.rs` | `load_hooks_file` | 逐事件、逐规则加载，完成身份/路径/正则绑定 |
| `crates/goose/src/hooks/mod.rs` | `HookManager::from_plugins` | 按插件隔离加载失败，汇总日志带事件名列表 |

## 参考

- [goose 源码：hooks/mod.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/hooks/mod.rs)
- [Open Plugins 规范：hooks 组件](https://open-plugins.com/agent-builders/components/hooks)
- [goose 文档：插件与钩子](https://block.github.io/goose/docs/guides/plugins/)
- [serde 文档：`deserialize_with` 与 `default`](https://serde.rs/field-attrs.html)

## 与主核 / AI 的关系

「可编辑配置的解析要分层宽容」在主核里对应的是目录文件、配置文件的加载：主核读一个损坏的表定义不能让整个实例起不来，但也不能把语法错误当成「没有这张表」。这里的四档处置（忽略 / 跳过 / 警告 / 失败）恰好是一个完整的阶梯，可以直接借用。

对 AI 工程的实际意义有三条。第一，**未来扩展点要宽容，书写错误要严格**。未知的动作类型忽略，是因为插件生态会先于 goose 发布新类型；而 `command` 写成数字，是纯粹的笔误，必须报出来。把这两件事混成一种处置，要么让生态无法演进，要么让用户永远看不到自己的错误。

第二，**`on_failure` 的默认方向决定可用性**。默认 `Allow` 意味着「钩子挂了就放行」，这让钩子变成一个可失败的附加层；默认 `Block` 则会让一次脚本崩溃把整个 agent 锁死。凡是「外部脚本 / 外部服务参与主路径」的设计，都要先回答「它挂了会怎样」，而这个默认值就是答案。

第三，**加载期完成尽可能多的解析工作**。`LoadedRule` 在加载时就把 `plugin_name`、`plugin_root`、`Regex` 都备好了，于是运行时只剩下「匹配、执行、判定」。这样做的好处不只是性能——它把「配置不合法」这一类错误全部收敛到一次加载里，运行时不再有「这个正则能不能编译」这种分支。
