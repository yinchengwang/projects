---
project: goose
feature: plugin-discovery
title: 插件发现与作用域
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/plugins/discovery.rs
    symbols: [DiscoveredPlugin, PluginScope, PluginSettings]
  - path: crates/goose/src/plugins/discovery.rs
    symbols: [discover_enabled_plugins, plugin_scope_rank, filter_by_config]
---

# 插件发现与作用域

## 一句话职责

把「这台机器上哪些插件该被激活」算成一个**按作用域分层的覆盖链**：项目级 `.agents/plugins/` 与用户级安装目录各自扫一遍，再用三层设置文件（local → project → user）逐层判定启用/禁用，最后按作用域排序并按名字去重。

## 机制是什么

| 类型 | 作用 |
|---|---|
| `DiscoveredPlugin { name, root, scope }` | 一个在磁盘上找到、且未被任何设置禁用的插件 |
| `PluginScope::{User, Project}` | 插件所在的作用域 |
| `PluginSettings { enabled, disabled }` | 一份设置文件里的两个名单（线上键是 `enabledPlugins` / `disabledPlugins`） |
| `SettingsScope::{Local, Project, User}` | 设置文件本身的三个层级 |
| `PluginConfigEntry { enabled }` | `config.yaml` 里 `plugins` 映射的一项，**按插件的文件系统路径作键** |

入口 `discover_enabled_plugins(project_root: Option<&Path>)`：给 `project_root` 才会启用项目级与本地级查询。

## 怎么实现的

**「发现」与「启用」是两个阶段，且中间夹了一次写盘。** 第一阶段是纯文件系统扫描：列项目插件目录（若有 `project_root`）与用户插件目录的子目录，每个子目录算一个插件。第二阶段是两层过滤：

1. `filter_by_config`——应用 `config.yaml` 里的 `plugins` 映射；
2. `is_enabled`——应用三层设置文件的名单。

**第二阶段的第一层有副作用，这是本页最需要记住的一点。** `filter_by_config` 的行为是：

| 情形 | 结果 |
|---|---|
| 映射里有这个路径且 `enabled: true` | 保留 |
| 映射里有这个路径且 `enabled: false` | 丢弃 |
| 映射里**没有**这个路径 | **插入 `{ enabled: true }` 并保留** |

插入时置 `dirty = true`，最后若有改动就 `config.set_param(PLUGINS_CONFIG_KEY, entries)`，失败只 `tracing::warn!`。

也就是说**「发现」这个动作会写配置文件**：第一次扫到的插件被自动登记为启用。这是一个显式的取舍——它让「我有哪些插件」在配置里可见可编辑（用户可以直接把某个改成 `false` 而不用记住路径），代价是**一个读路径带了写副作用**，且失败时只警告不报错（于是下一次扫描会再试一次）。

**`config.yaml` 里的键是文件系统路径，不是插件名。** 注释写明了：`keyed by the plugin's filesystem path`。这与最后那步「按名字去重」形成对照——**同名不同路径是两个独立条目，但最终只会激活一个**。

为什么用路径作键？因为插件名来自目录名（`list_dir_children` 取 `file_name`），而两个不同作用域下完全可能有同名目录。用路径作键能保证「用户明确禁用的那个」指的就是那一个，不会因为同名而误禁另一个。

**三层设置文件的判定顺序是 Local → Project → User，且第一个「指名」的层级胜出。** `is_enabled` 的实现是依次查三个 scope 的设置，对每个：

- 若 `disabled` 名单里有它 → **立刻返回 `false`**；
- 否则若 `enabled` 名单里有它 → **立刻返回 `true`**；
- 两者都没有 → 继续下一层。

三层都没提名 → 默认 `true`。

这条链有几个值得拆开的性质：

1. **`disabled` 在同一层内优先于 `enabled`。** 一个文件同时列出同一个名字时，禁用获胜。这是安全侧取向。
2. **更具体的层级优先。** `settings.local.json`（本地，通常不入版本库）能覆盖项目设置，项目设置能覆盖用户设置。
3. **默认启用。** 一个被发现的插件若三层都没提，就是启用的——与 `filter_by_config` 里「新发现即启用」是同一个默认值。

**`PluginSettings` 的字段名是从外部规范来的。** 结构体的注释直接给了出处：`https://open-plugins.com/plugin-builders/installation`，而两个字段用 `#[serde(rename = "enabledPlugins")]` / `#[serde(rename = "disabledPlugins")]` 转成驼峰。

也就是说这份设置文件的格式**不是 goose 自定义的**，而是一个跨工具的约定。这解释了为什么字段名是驼峰（Rust 侧习惯是蛇形，这里刻意迁就文件格式），也解释了为什么它只有两个名单字段、没有任何 goose 特有配置。

**设置文件的路径有三处，各自对应一个层级。** `user_settings_path` 是 `Paths::path_root()/.config/goose/settings.json`（拿不到 path root 就退回 `home/.config/goose/settings.json`）；`project_settings_path` 是 `{project_root}/.config/goose/settings.json`，`local` 变体是 `settings.local.json`。

注意这两个路径的**不对称**：用户级在 `path_root/.config/goose/`，项目级在 `{project_root}/.config/goose/`。也就是说项目级设置不在项目根的某个 goose 专属目录里，而是在项目内复刻了同样的 `.config/goose` 结构。这让用户级路径（前置于 `PATH_ROOT`）与项目级路径（前置于项目根）可以用同一段后半段。

**读设置失败是警告而不是失败。** `load_all_settings` 的 `filter_map` 里，`Err(e)` 分支 `tracing::warn!(path, error, "Failed to read plugin settings")` 然后返回 `None`（跳过这一层）。

于是**一个语法损坏的设置文件会让那一层失效，但不会阻止其它层生效**，也不会阻止插件被发现。这个方向是对的：插件系统的可用性不该被一份坏配置完全阻断。代价是「我把某个插件禁用了但它还在」这种情况需要看日志才能发现。

**`read_settings` 区分「文件不存在」与「读失败」。** `Ok(None)` 用于不存在，`Err` 用于读或解析失败——于是「没这个文件」是正常路径（不告警），而「有文件但坏了」才告警。

**作用域排序靠一个显式的 rank 函数。** `plugin_scope_rank(Project) = 0`、`User = 1`，排序键是 `(rank, name, root)`。于是项目级插件排在用户级之前。

而排序之后紧接着去重：

```rust
let mut seen_names = HashSet::new();
enabled_plugins.into_iter().filter(|p| seen_names.insert(p.name.clone())).collect()
```

**排序的作用正是让去重保留项目级的那一个。** 这是一个把「优先级」编码成「顺序」的经典手法——先排好序，再用「按序遍历 + 见过就跳过」实现「先到者胜」。它比「按名字分组再挑」要简单，且不需要额外数据结构。

**「项目插件目录与用户插件目录可能是同一个」时跳过前者。** `if !equivalent_paths(&project_plugins_dir, &user_plugins_dir)` 才扫项目目录。而 `equivalent_paths` 先比字符串相等，再比 `canonicalize()` 的结果——所以一个符号链接或一个相对路径写法的差异也能被识别。

这条判断防止的是「同一批插件被扫两遍、产生两条同路径但不同 scope 的记录」。若不跳过，去重会按名字去掉一个，但 `filter_by_config` 会为同一个路径写两次（幂等，无害）——真正的问题是那个被去掉的记录作用域是错的。

**`list_dir_children` 只收目录、且名字必须能转成 UTF-8。** 三处过滤：`read_dir` 失败返回空 vec（注释无，但行为是「目录不存在就当作没有插件」）、非目录跳过、`file_name()?.to_str()?` 失败的跳过。

第三条意味着**一个非 UTF-8 名字的目录会被静默忽略**。这是「插件名要出现在设置文件与界面里」的直接结果——一个无法用 UTF-8 表示的名字没有可用的书面形式。

**`project_plugin_dir` 的路径是 `{root}/.agents/plugins`，而设置文件在 `{root}/.config/goose`。** 两个不同的隐藏目录，各管一件事：`.agents/plugins` 是插件本体（由外部约定决定，与 `.agents/` 这个共享 agent 目录约定一致），`.config/goose` 是 goose 自己的设置。

这个分离不是随意的：`.agents/` 是一个跨工具共享的约定目录（goose 只在里面读 `plugins/`），而 `.config/goose/` 是 goose 私有的。

## 关键取舍

**源码事实：** 发现动作会写 `config.yaml`——新扫到的插件被自动登记为 `enabled: true`。读路径带写副作用，失败只警告。

**源码事实：** 配置映射的键是**文件系统路径**而非插件名，所以「禁用某一个同名插件」是精确的；名字去重发生在最后。

**源码事实：** 设置判定顺序是 local → project → user，第一个提名的层级胜出；同一层内 `disabled` 优先于 `enabled`；三层都没提名则默认启用。

**源码事实：** `PluginSettings` 的字段名来自 open-plugins.com 的跨工具约定（驼峰），不是 goose 自定义。

**源码事实：** 读某层设置失败只警告并跳过该层，不阻止其它层或整体发现；「文件不存在」不算失败。

**源码事实：** 作用域优先级被编码成排序（Project rank 0 优先），再靠「按序遍历 + 见名跳过」实现先到者胜。

**源码事实：** 项目插件目录与用户插件目录等价时（含符号链接/相对路径情形）跳过项目扫描，避免同一批插件被记两次不同作用域。

**源码事实：** 只收目录、只收能转 UTF-8 的名字，其余静默跳过。

**架构推论：** 这层的核心是**把「优先级」变成一个可以叠加的覆盖链**，而不是一堆 if。三段链条各自负责一个维度：作用域（项目 vs 用户，决定扫描哪个目录）、配置层（`config.yaml` 的显式开关，决定要不要把它纳入）、设置层（三层名单，决定启用）。三者串行，每段都能独立解释。

而「发现即登记」这个副作用最有争议。它的收益是让配置成为一份**可见的清单**——用户可以打开 `config.yaml` 看到自己有哪些插件并逐条开关，而不必知道目录在哪。代价是「一个纯查询动作需要写权限」，在只读文件系统（容器、CI）上会持续告警，而由于失败被降级为警告，用户可能一直没注意到自己的禁用设置没有生效。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/plugins/discovery.rs` | `discover_enabled_plugins` | 对外入口，按项目根决定作用域范围 |
| `crates/goose/src/plugins/discovery.rs` | `discover_enabled_plugins_with_config` | 扫描 + 两层过滤 + 排序 + 去重的主流程 |
| `crates/goose/src/plugins/discovery.rs` | `DiscoveredPlugin` | 找到的插件：名字、根路径、作用域 |
| `crates/goose/src/plugins/discovery.rs` | `PluginScope` | 两个作用域，配 `plugin_scope_rank` 的 0/1 |
| `crates/goose/src/plugins/discovery.rs` | `PluginSettings` | 跨工具约定的两个名单（驼峰键） |
| `crates/goose/src/plugins/discovery.rs` | `filter_by_config` | 应用 `config.yaml` 映射，新发现即登记启用 |
| `crates/goose/src/plugins/discovery.rs` | `PluginConfigEntry` | 按文件系统路径作键的开关项 |
| `crates/goose/src/plugins/discovery.rs` | `is_enabled` | 三层名单的覆盖链，disabled 优先、默认启用 |
| `crates/goose/src/plugins/discovery.rs` | `load_all_settings` | 读三层设置，失败降级为警告并跳过 |
| `crates/goose/src/plugins/discovery.rs` | `equivalent_paths` | 用 canonicalize 判断两个插件目录是否同一个 |
| `crates/goose/src/plugins/discovery.rs` | `list_dir_children` | 只收目录、只收 UTF-8 名字 |
| `crates/goose/src/plugins/discovery.rs` | `PLUGINS_CONFIG_KEY` | `config.yaml` 里映射的键名 |

## 参考

- [goose 源码：plugins/discovery.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/plugins/discovery.rs)
- [Open Plugins：安装与设置文件格式](https://open-plugins.com/plugin-builders/installation)
- [goose 文档：插件](https://block.github.io/goose/docs/guides/plugins/)
- [Claude Code 文档：`.agents/` 共享目录约定](https://docs.claude.com/en/docs/claude-code/settings)

## 与主核 / AI 的关系

这套覆盖链与主核里「配置的优先层级」是同一种结构：数据库的参数来源也是多层的（编译默认、`postgresql.conf`、`ALTER SYSTEM`、会话级 `SET`），而优先级规则同样是「越具体越优先」。区别在于数据库把优先级放在**读取时刻的动态解析**里，而这里把一部分优先级落到**配置文件的一次性登记**上。

对 agent 工程的直接价值有三条。其一，**优先级可以编码成排序 + 去重，而不必写成嵌套判断**。`plugin_scope_rank` 加「见名跳过」这两步，比「若项目有则用项目、否则若用户有则用用户」的显式分支更容易扩展第三个作用域。其二，**禁用优先于启用，且同一层内不看顺序**。这是安全侧的必要取向——否则一个同时出现在两个名单里的插件，其行为会取决于文件里的书写顺序。其三，**「发现即登记」这类副作用要有清楚的失败语义**。这里失败只警告，于是「我禁用了它但它还在」只能靠看日志发现；在只读文件系统上尤其要留意这条。
