---
project: goose
feature: config-migrations
title: 配置迁移与破坏性边界
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/config/migrations.rs
    symbols: [run_migrations, migrate_provider_config, cleanup_legacy_provider_keys]
  - path: crates/goose/src/config/migrations.rs
    symbols: [migrate_platform_extensions, read_enabled_field]
---

# 配置迁移与破坏性边界

## 一句话职责

把旧版平铺式配置键升级成结构化块，同时区分两类迁移——只补字段的（可在只读路径上跑）与会删键的（只能在真正的写路径上跑），并用这个区分防止「读一次配置就把别人的数据删掉」。

## 机制是什么

配置是一个 `serde_yaml::Mapping`（即一份 YAML 字典）。有三个顶层键贯穿全篇：`extensions`、`providers`、`active_provider`。

`run_migrations(config) -> bool` 是全量入口，依次跑两个迁移并汇总「是否有改动」：

```rust
changed |= migrate_platform_extensions(config);
changed |= migrate_provider_config(config);
```

`run_read_migrations(config)` 只跑第一个。注释给了理由：「provider 迁移被排除，因为它会移除平铺键，而 `get_param()` 的调用方可能仍在直接查这些键」。

所以**两个迁移的性质不同**：扩展迁移只「补/改写 `extensions` 块」，不删任何东西；provider 迁移会**删除** `GOOSE_PROVIDER`、`GOOSE_MODEL` 与一批 `*_configured` 键。前者可以在只读路径上重复跑，后者不行。

## 怎么实现的

**`read_enabled_field` 是三层嵌套的 `?`。** 实现是一串 `Option` 组合：`value.as_mapping()?` → `get(&"enabled")?` → `as_bool()`。任一环不成立就返回 `None`。

这个函数的存在说明「取一个嵌套字段」在这类无类型 YAML 数据上要写三层判空。返回值是 `Option` 而不是 `bool` 默认 `false`，因为「没有这个字段」与「字段为 false」在迁移里语义不同——前者应当用默认值，后者应当保留用户的选择。

**平台扩展迁移的判据。** `migrate_platform_extensions` 遍历编译期的 `PLATFORM_EXTENSIONS` 表，对每一项判断是否需要迁移，判据是三段的：

1. 配置里没有这个名字 → 需要迁移（要新建）。
2. 有，但反序列化成 `ExtensionEntry` 失败 → 需要迁移（数据坏了，重建）。
3. 有且能解析，但 `description` 与 `display_name` 与表里的不一致 → 需要迁移（元信息过时）。
4. 有且能解析，但类型不是 `Platform` 也不是 `Builtin` → 需要迁移（类型不对）。

第 3 条是这套迁移的核心动机：当内置扩展的描述文案更新时，用户的配置里存的是旧文案。若不做迁移，界面上会一直显示旧描述。把文案纳入「需要重建」的条件，让描述之类的元信息能随版本更新。

**重建时保留用户状态。** 确定要重建后，代码做四件事保状态：

- `enabled`：从旧的 `enabled` 字段读，取不到用表里的 `default_enabled`。
- `available_tools`：从旧条目里取（若是 `Builtin` 或 `Platform` 类型），否则空。
- **类型保持**：若旧条目本来是 `Builtin`，新条目也建成 `Builtin`；否则建成 `Platform`。注释写「若扩展已存在为 builtin 类型，保留该类型」。
- 其余字段（`name`、`description`、`display_name`、`bundled: Some(true)`）用表里的新值。

所以重建是「元信息用新的、用户状态用旧的、类型用旧的」。这三条分别对应三类字段，区分得很清楚。

`timeout` 字段在新建 `Builtin` 时设为 `None`——也就是重建会**丢掉用户设的超时**。这是一个未被保留的状态，值得注意。

**`needs_save` 与写回。** 只有真的改了才 `config.insert(extensions_key, ...)` 并返回 `true`。所以「表与配置已经一致」时不着痕迹地返回 `false`，上层据此知道「不需要写盘」。

**provider 迁移的两段结构。** `migrate_provider_config` 先判 `providers` 块是否已存在：

- **已存在**：只做「若 `active_provider` 缺失则从 `GOOSE_PROVIDER` 回填」，然后调 `cleanup_legacy_provider_keys` 删掉残留的平铺键。
- **不存在**：走完整迁移。

这个分支很重要：它是「半迁移状态」的修补逻辑。可能某个版本已经建了 `providers` 块但漏了新键，或者用户手改过。先补后清，保证幂等。

**完整迁移的四步。** 当 `providers` 不存在时：

1. 读 `GOOSE_PROVIDER`（当前 provider 名）与 `GOOSE_MODEL`（当前模型，缺省空串）。
2. **扫描 `*_configured` 后缀的键**发现所有「曾经用过」的 provider——把后缀剥掉得到名字。
3. 若当前 provider 不在发现列表里，补进去。
4. 若发现列表空且当前 provider 也空 → 早退返回 `false`。

然后为每个发现的 provider 建一个 `ProviderEntry`：`enabled: true`、`model`（只有当前 provider 才填真实模型，其余空串）、`configured: true`。

接着写入三个东西：`providers` 映射、顶层 `active_provider`，并 `shift_remove` 掉 `GOOSE_PROVIDER`、`GOOSE_MODEL` 与所有 `*_configured` 键。

**`shift_remove` 而非 `remove`。** `serde_yaml::Mapping::shift_remove` 在删除时会**保持剩余键的顺序**（不同于 `swap_remove` 会把末项挪过来填补空位）。对配置文件来说，键顺序影响人工阅读与 diff 的可读性，所以用 `shift_remove` 是正确的选择。

**`is_active` 只影响一个字段。** 每个 `ProviderEntry` 里只有 `model` 字段取决于是否当前 provider。`enabled` 与 `configured` 一律 `true`——所以迁移后「所有历史上用过的 provider 都被标记为启用且已配置」。这可能不是用户原意（某个 provider 只是试过就弃了），但旧数据里没有「是否启用」这个信息，只能这么填。

**`cleanup_legacy_provider_keys` 的范围比迁移更宽。** 它删的条件是「`GOOSE_PROVIDER` 或 `GOOSE_MODEL` 或以 `_configured` 结尾」，而不是「发现的那些 provider 对应的键」。所以即使某个 `*_configured` 键对应的 provider 从没被迁移进 `providers` 块，它也会被删掉。这是清理逻辑，不是迁移逻辑——它的假设是「`providers` 块已经是权威，这些平铺键全是垃圾」。

## 关键取舍

**源码事实：** `run_migrations` 跑两个迁移，`run_read_migrations` 只跑第一个，理由是后者不删键、`get_param()` 调用方可能还在查被删的键。

**源码事实：** 扩展迁移重建条目时保留 `enabled`、`available_tools` 与类型（`Builtin` 保持 `Builtin`），但**不保留 `timeout`**。

**源码事实：** `*_configured` 后缀是旧版「这个 provider 配置过」的标记。迁移把标记剥成名字，全部标 `enabled: true` 与 `configured: true`。

**源码事实：** 只有当前 provider 的 `model` 字段填真实值，其余 provider 全填空串。

**源码事实：** 删除用 `shift_remove`，保持剩余键顺序。

**架构推论：** 「可重复跑」与「会删数据」这两个属性必须分开建模。若只有一个 `run_migrations`，那么任何「读配置」的地方调它都会产生副作用——一次读取可能把磁盘上的键删掉（若之后又被写回）。拆成两个入口，让只读路径显式选择「不删东西」的那个，是把破坏性从「隐式」变成「显式」的做法。

**架构推论：** 扩展迁移的判据把「文案不一致」也算作需要迁移，这实际上把配置当成了「缓存」而非「权威」。用户的 `enabled` 是权威的（保留），而 `description` / `display_name` 是缓存的（跟着代码更新）。这个划分很清晰：凡是用户能改的保留，凡是代码里写死的就覆盖。

**架构推论：** 完整 provider 迁移的「扫 `*_configured` 后缀发现 provider」是一个很有时代感的设计——旧版用一个「扁平键 + 后缀标记」表达一对多关系。迁移的工作就是把这种「用键名嵌套」的编码方式翻译成真正的结构。这类迁移的难点永远在「发现」这一步：必须能从旧数据里枚举出所有对象，而旧格式把对象名藏在键名里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/config/migrations.rs` | `run_migrations` | 全量迁移，含破坏性操作 |
| `crates/goose/src/config/migrations.rs` | `run_read_migrations` | 只读路径用的非破坏性迁移 |
| `crates/goose/src/config/migrations.rs` | `migrate_platform_extensions` | 按内置表补齐与重建扩展条目 |
| `crates/goose/src/config/migrations.rs` | `migrate_provider_config` | 平铺键到 `providers` 块的双分支迁移 |
| `crates/goose/src/config/migrations.rs` | `cleanup_legacy_provider_keys` | 删除残留平铺键 |
| `crates/goose/src/config/migrations.rs` | `read_enabled_field` | 三层判空取嵌套布尔 |
| `crates/goose/src/config/migrations.rs` | `EXTENSIONS_CONFIG_KEY` | 扩展块的顶层键名 |
| `crates/goose/src/config/migrations.rs` | `ACTIVE_PROVIDER_KEY` | 当前 provider 的顶层键名 |

## 参考

- [goose 源码：config/migrations.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/config/migrations.rs)
- [goose 文档：配置参考](https://block.github.io/goose/docs/category/configuration/)
- [serde_yaml 文档：Mapping 的 shift_remove](https://docs.rs/serde_yaml/latest/serde_yaml/struct.Mapping.html)

## 与主核 / AI 的关系

主核里最接近的是「系统表升级与在线迁移」：新版内核读旧版元数据时需要转换，而转换分两类——「补字段」（任何路径都能跑）与「改结构或删记录」（只能在真正的升级流程里跑，且有事务保护）。goose 这里没有事务，所以它用「两个入口」来划这条线：`run_read_migrations` 只做安全的，`run_migrations` 才做全部。

与 smolagents 的配置处理对比，差别在于「有没有历史包袱」。smolagents 是 2024 年的框架，配置层没有需要迁移的旧格式；goose 是从「Block 名下、扁平键」一路演进过来的产品，配置里存着多个版本的痕迹。这解释了为什么这一层要处理「`*_configured` 后缀发现 provider」这种编码——那是旧式「用键名表达结构」的直接产物。任何长期存活的产品的配置层最终都会长出这样的迁移代码。

对 AI 工程最实用的一条是「读路径与写路径必须用不同的迁移函数」。这是一个容易被忽略但后果明显的设计：如果只有一个 `run_migrations`，那么「打开设置界面看一眼」这个动作就会触发删键。若随后因为某种原因没写回（进程崩了、用户取消了），磁盘上的数据已经被读过一次却没保存——下次读时看到的是「迁移了一半」的状态。拆成两个入口，读路径永远只做加法。

另一条是「用户可改的保留、代码写死的覆盖」。做配置升级时，最常见的错误是「怕丢用户设置所以什么都保留」，结果描述文案永远是旧的、新字段永远补不上。正确的划分是按「字段的所有者」：用户拥有的是选择（启用与否、超时多久），代码拥有的是常量（描述文本、默认值）。迁移时前者保留、后者覆盖，两类都不会出错。

**顺带记一个真实的缺口：** 扩展迁移重建 `Builtin` 条目时把 `timeout` 设为 `None`，没有从旧条目保留。所以「用户给某个内置扩展设了超时，一次迁移后就丢了」——这正是「用户可改的应当保留」这条原则被违反的地方，且因为它只在特定类型的重建路径上发生，很难被注意到。
