---
project: goose
feature: config-experiments
title: 实验开关与目录布局
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/config/experiments.rs
    symbols: [ExperimentManager]
  - path: crates/goose/src/config/paths.rs
    symbols: [Paths, DirType]
---

# 实验开关与目录布局

## 一句话职责

用一份编译期常量表把「有哪些实验开关」定死，用户的实验列表每次都按它对齐（多的删、少的补默认值）；同时把所有落盘位置收敛到六个逻辑目录，并允许用一个环境变量整体重定向。

## 机制是什么

`ExperimentManager` 是个纯命名空间结构体（`pub struct ExperimentManager;`，无字段），三个静态方法：`get_all()`、`set_enabled(name, enabled)`、`is_enabled(name)`。

开关的权威来源是 `const ALL_EXPERIMENTS: &[(&str, bool)]`——**当前是空数组**。注释写明了它的语义：这是「初始化实验的基准真相」；用户列表里有但基准表里没有的实验会被**移除**；基准表里有但用户列表里没有的会被**加上**并附默认值。

`Paths` 也是无字段结构体，六个逻辑目录：`Config`、`Data`、`State`、`Plugins`、`Agents`、`AgentsHome`，都有对应的公开访问函数，外加四个 `in_*_dir(subpath)` 便捷方法。

## 怎么实现的

**实验列表的每次读写都先对齐。** 三个方法都调 `refresh_experiments`：

- `get_all()` 读 `config.get_param("experiments")`（失败用 `unwrap_or_default()` 得到空 map），对齐后收集成 `Vec<(String, bool)>`。
- `set_enabled` 同样先读、先对齐，然后 `insert`，再写回。
- `is_enabled` 干脆直接转成 `get_all()` 再查 map——所以它也会对齐。

`refresh_experiments` 做两件事：`entry(key).or_insert(default_value)` 补上缺失的，`retain(|key, _| ALL_EXPERIMENTS 里存在)` 删掉多余的。

**这套「基准表驱动」的意义在于开关的下线。** 当一个实验功能被移除时，只需从 `ALL_EXPERIMENTS` 删掉那一行，用户配置里对应的键会在下一次读或写时被自动清掉。不需要写迁移脚本，也不需要在代码里留一段「清理旧开关」的逻辑。这是把「配置清理」变成**声明式**的：表是什么，配置就是什么。

代价是这张表现在是空的——所以 `retain` 会把用户配置里**所有** `experiments` 键都删掉。也就是说当前版本不承认任何实验开关，任何残留在配置里的实验名都会被清空。

**`is_enabled` 的默认是 `false`。** `unwrap_or(&false)`。加上 `refresh_experiments` 只补基准表里有的键，所以「查一个不在表里的实验名」永远得到 `false`，且不会把那个名字写进配置。这个行为的反面是：无法通过 `is_enabled` 探测「某个未登记的开关是否被用户手动打开过」。

**`set_enabled` 不校验名字。** 它会 `insert(name, enabled)` 并把结果写回——即使 `name` 不在 `ALL_EXPERIMENTS` 里。但因为写之前调过 `refresh_experiments`，所以本次 write 的 map 只含基准表里的键，然后 insert 一个表外的键。于是在「基准表为空」的当下，`set_enabled` 写进去的键会在下次读时被 `refresh_experiments` 删掉。这是一个「写入能成功但不持久」的怪状态。

**`Paths` 的双策略。** `get_dir(dir_type)` 先看 `path_root()`：若 `GOOSE_PATH_ROOT` 存在**且是绝对路径**，就用它做基座，六个目录按固定子路径拼上去（`config`、`data`、`state`、`.agents/plugins`、`.agents/agents`、`.agents`）。

否则退回 `etcetera` 的平台策略：`choose_app_strategy` 用 `top_level_domain: "Block"`、`author: "Block"`、`app_name: "goose"`，然后按目录类型取平台对应的位置（macOS 上是 `~/Library/Application Support/Block/goose/`）。

那段 `NOTE` 注释是关键：保留 `"Block"` 是为了**向后兼容**已存在的用户目录，改名会让现有安装的配置与数据全部失联。所以这个看似可以「正名」的值，实际是迁移成本的一部分。

`expect("goose requires a home dir")` 说明没有 home 目录时直接 panic——这是一个「不可能发生」的假设，而不是可恢复错误。

**绝对路径校验。** `validated_path_root` 只做两件事：把 `OsString` 转成 `PathBuf`，然后 `filter(|path| path.is_absolute())`。所以相对路径的 `GOOSE_PATH_ROOT` 被静默忽略（退回平台策略），而不是报错。测试里明确断言了这个行为：`None`、空串、相对路径都得到 `None`，绝对路径得到 `Some`。

静默忽略而不是报错的取舍：若报错，一个写错的环境变量会让程序起不来；若静默忽略，用户会看到「配置写到意料之外的地方」但程序能跑。后者更宽容，也更难诊断。

**`State` 的回退。** 平台策略里 `State` 是 `strategy.state_dir().unwrap_or(strategy.data_dir())`——某些平台没有独立的 state 目录，就退到 data。这让六个逻辑目录在每个平台上都有值，不会出现 `Option` 一路传下去。

**六个目录的分工。** `Config` 存配置（包括 `custom_providers/` 子目录，见 g04）；`Data` 存数据（包括模型目录缓存，见 g02）；`State` 存运行时状态；`Plugins`、`Agents`、`AgentsHome` 三个都在 `~/.agents` 之下，分别对应插件、agent 定义与那个共享根。

注意 `AgentsHome` 就是 `.agents` 本身，而 `Plugins` 与 `Agents` 是它下面的两个子目录。所以这三个不是并列的，而是「一个根加两个子目录」。这在 `GOOSE_PATH_ROOT` 模式下也一样——三者都拼在同一个基座上，所以重定向时 `~/.agents` 会变成 `<root>/.agents`。

## 关键取舍

**源码事实：** `ALL_EXPERIMENTS` 当前为空数组。所以任何残留在用户配置里的实验名都会被 `refresh_experiments` 的 `retain` 删除。

**源码事实：** `set_enabled` 不校验名字，但写前的对齐会把表外的旧键删掉；写入表外的名字能成功但下次读时被清掉。

**源码事实：** `is_enabled` 对未登记的名字返回 `false` 且不会写入。

**源码事实：** `GOOSE_PATH_ROOT` 必须是绝对路径，否则静默忽略并退回平台策略。

**源码事实：** 平台策略的 `top_level_domain` 与 `author` 都是 `"Block"`，注释明确说是为兼容既有安装目录。

**源码事实：** `State` 在平台模式下可能退回 `Data`；在 `GOOSE_PATH_ROOT` 模式下则总是 `<root>/state`。

**架构推论：** 「基准表 + 每次对齐」是一种把配置清理声明化的手法。传统的做法是「加迁移函数：把 v1 的键转成 v2 的键」；这里改成「维护一张表，表外的自动消失」。前者需要累积历史迁移，越写越长；后者只需维护当前状态。代价是**无法表达「需要一个迁移过程」的变化**——若某个开关改名并要保留用户的旧选择，这张表就做不到了。

**架构推论：** `Paths` 用一个环境变量做整体重定向，是给测试与多实例部署用的实用设计。没有它，测试就得 monkey-patch 平台策略或污染真实用户目录；有了它，`GOOSE_PATH_ROOT=/tmp/x` 就能把六个目录全部搬走。而「必须绝对路径」这个限制防的是「相对路径的基座会随进程 cwd 变化」，那种情况下配置会跑到意料之外的地方且极难复现。

**架构推论：** 「一个根加两个子目录」这种布局（`AgentsHome` 与其下两个）暗示了一个产品决策：`.agents` 是一个**跨工具共享**的约定目录（而不是 goose 专属），所以它放在 home 下而不是 app data 下。这也解释了为什么 `GOOSE_PATH_ROOT` 模式下它被拼到根目录下而不是独立配置——它本来就不属于「应用私有数据」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/config/experiments.rs` | `ExperimentManager` | 实验开关的读写入口 |
| `crates/goose/src/config/experiments.rs` | `ExperimentManager.get_all` | 读后对齐基准表并返回全量 |
| `crates/goose/src/config/experiments.rs` | `ExperimentManager.set_enabled` | 对齐后写入单个开关 |
| `crates/goose/src/config/experiments.rs` | `ExperimentManager.refresh_experiments` | 补缺失、删多余的对齐实现 |
| `crates/goose/src/config/experiments.rs` | `ALL_EXPERIMENTS` | 编译期基准表，当前为空 |
| `crates/goose/src/config/paths.rs` | `Paths` | 六个逻辑目录的访问入口 |
| `crates/goose/src/config/paths.rs` | `Paths.get_dir` | 环境变量与平台策略的双路径 |
| `crates/goose/src/config/paths.rs` | `Paths.validated_path_root` | 只接受绝对路径的环境变量校验 |
| `crates/goose/src/config/paths.rs` | `DirType` | Config/Data/State/Plugins/Agents/AgentsHome 六值 |

## 参考

- [goose 源码：config/experiments.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/config/experiments.rs)
- [goose 源码：config/paths.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/config/paths.rs)
- [etcetera 文档：跨平台目录策略](https://docs.rs/etcetera/)

## 与主核 / AI 的关系

主核里最接近的是「运行参数与实例目录」：配置文件、数据目录、日志目录、临时目录各自有默认位置，且都允许被环境变量覆盖以支持多实例与测试。`Paths` 的做法与这个模式一致，连「只接受绝对路径」这条防御都相同——因为相对路径的基座会让同一份配置在不同 cwd 下解析出不同位置，这是最难排查的一类问题。

「基准表驱动配置对齐」这个手法在主核里也有对应物：内核参数表定义了「有哪些参数、默认值与合法范围」，启动时读入用户设置后按参数表补齐与裁剪。区别在于主核的参数表是功能性的（缺一个参数就没法启动），这里的实验表是**可选功能的开关**——表空意味着「当前没有任何实验功能」，这是完全合法的状态。

对 AI 工程最实用的一条是「用一张表把配置清理变成声明式的」。当你的产品有大量「灰度开关」「实验特性」时，最烦的不是加开关，而是**下开关**：功能转正后要清理用户配置里残留的键，否则它会一直出现在设置界面上。传统做法是写迁移函数并在版本号上做判断，这套做法的累积成本很高。改成「维护一张当前有效的表 + 每次读写都对齐」，就把清理变成了表的副作用。

另一条是「静默忽略非法的目录覆盖」这个取舍。报错能让问题早暴露，但会让一个写错的环境变量直接导致程序起不来；静默忽略保证程序能跑，代价是配置写到了意料之外的位置且没有任何提示。折中的做法是「静默忽略 + 记一条警告」——这个代码里没有做，是值得借鉴的改进点：既然 `validated_path_root` 已经把非法值过滤掉了，过滤点正是记日志的最佳位置。
