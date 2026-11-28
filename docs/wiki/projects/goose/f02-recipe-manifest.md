---
project: goose
feature: recipe-manifest
title: 配方文件清单与加载
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/recipe/manifest.rs
    symbols: [RecipeFileManifest, list_recipe_file_manifests, load_recipe_by_id]
  - path: crates/goose/src/recipe/manifest.rs
    symbols: [resolve_recipe_sub_recipe_paths, short_id_from_path]
---

# 配方文件清单与加载

## 一句话职责

把磁盘上的一批配方文件扫成一份**可列举、可按 id 取回、且相对路径已被解析**的清单：每个条目带一个由路径哈希出的稳定短 id、文件路径、最后修改时间，以及一份已经把子配方路径换算成绝对路径的 `Recipe`。

## 机制是什么

`manifest.rs` 只有 140 行，四个公开项：

| 项 | 作用 |
|---|---|
| `RecipeFileManifest { id, recipe, file_path, last_modified }` | 清单里的一条 |
| `short_id_from_path(path)` | 用默认哈希器把路径变成一个 16 位十六进制短 id |
| `list_recipe_file_manifests()` | 扫全部本地配方，产出清单，按修改时间**倒序** |
| `get_recipe_file_path_by_id(id)` / `load_recipe_by_id(id)` | 按 id 反查路径 / 直接加载 |
| `load_recipe_from_path(path)` | 从路径加载并解析子配方路径 |
| `resolve_recipe_sub_recipe_paths(recipe, recipe_path)` | 把子配方的相对路径换成绝对路径 |

## 怎么实现的

**`short_id_from_path` 是「路径 → 稳定标识」的最简实现。** 实现用 `std::hash::DefaultHasher` 对路径字符串哈希，格式化成 `{:016x}`（8 字节、定长 16 个十六进制字符）。

三个性质值得拆开：

1. **确定性**。`test_short_id_from_path_is_stable` 断言同一路径两次得到同一个值——`DefaultHasher::new()` 没有随机种子，所以跨运行也稳定。
2. **不保证唯一**。8 字节哈希对「一个用户机器上的配方数量」（几十到几百）来说碰撞概率可以忽略，但这不是密码学哈希，代码也没有处理碰撞。
3. **与路径绑定而非与内容绑定**。**同一份配方换个路径就是一个新 id**。这是一个重要的语义：id 标识的是「那个文件」，不是「那份内容」。

第 3 条解释了为什么改内容不改 id（用户的书签/引用仍然有效），而移动文件就会失效。对于「本地配方列表」这种场景，这是一个比内容哈希更好的选择——内容哈希会让「编辑一次配方」就产生一个新 id。

**排序是按 `last_modified` 字符串倒序，而不是按 `SystemTime`。** `manifests.sort_by(|a, b| b.last_modified.cmp(&a.last_modified))`，而 `last_modified` 是 `to_rfc3339()` 的字符串。

用字符串比较时间在这里是安全的，因为 **RFC 3339 的 UTC 表示是定长且字典序与时间序一致的**（`DateTime::<Utc>::to_rfc3339()` 产出 `2024-01-02T03:04:05+00:00` 这种形式，同一年内长度相同、高位对齐）。但这是一个**隐式依赖**：如果哪天改成 `to_rfc2822()` 或本地时区偏移，排序就会错。

正确之处在于它选对了格式；风险在于这个正确性没有注释撑着。

**取不到修改时间的条目被静默跳过。** 循环里是：

```rust
let Ok(last_modified) = fs::metadata(file_path.clone())
    .and_then(|metadata| metadata.modified().map(...))
else { continue };
```

一个读不了 metadata 的文件（权限、路径失效）不进清单。这里没有日志——与 `e02` 里「读设置失败要告警」形成对照。取舍的差别在于：设置文件是**少数几份用户显式配置的文件**（坏一份值得警告），而配方目录里可能有大量文件（一份坏的不值得刷日志）。

**清单在返回之前做了一次「改写」：`resolve_recipe_sub_recipe_paths`。** 这一步是这一页最实质的逻辑：

```rust
let Some(recipe_dir) = recipe_path.parent() else { return };
let Some(ref mut sub_recipes) = recipe.sub_recipes else { return };
for sub_recipe in sub_recipes.iter_mut() {
    if let Ok(resolved) = resolve_sub_recipe_path(&sub_recipe.path, recipe_dir) {
        sub_recipe.path = resolved;
    }
}
```

也就是说：**清单里的 `Recipe` 已经是「子配方路径解析过」的版本**，调用方不需要自己再做一次。这是一个「加载时就把它变成可用的形状」的选择。

**解析失败时保留原路径而不是报错。** `if let Ok(resolved)`——`resolve_sub_recipe_path` 出错（例如路径指到不存在的地方）就不改动，保留相对路径。

这个方向是对的：一份配方的可用性不该因为它引用的某个子配方暂时不可用而整体失败。相对路径原样保留至少让界面能显示「它想引用什么」，而一次失败就整份丢弃会让用户完全看不到这份配方。

**同一个解析在两个入口都被调用，这是必要的重复。** `list_recipe_file_manifests` 里调一次，`load_recipe_from_path` 里也调一次。因为 `load_recipe_by_id` 走的是 `get_recipe_file_path_by_id` → `load_recipe_from_path` 这条路径（**不经过清单**）。

这看起来是重复，但它避免了一个更糟的选择：让 `load_recipe_by_id` 复用清单（那样会为了加载一份配方而扫全部配方）。而这层重复的代价是「两处必须记得解析」——一个靠测试守住的约定，`load_recipe_from_path_resolves_sub_recipe_paths` 就是那个测试。

**`load_recipe_by_id` 的实现是一个「先找路径、再加载」的两步。** `get_recipe_file_path_by_id` 里 `list_recipe_file_manifests()?.into_iter().find(|m| m.id == id)`——也就是说**按 id 加载会扫一遍全部配方**。

这是本页最明显的性能特征：一次 `load_recipe_by_id` 的复杂度是 O(配方数量) 次文件读 + 解析。对本地几十份配方可接受，但它意味着**调用方不该在循环里按 id 加载**。而 `get_recipe_file_path_by_id` 的错误文案是 `Recipe not found: {id}`——直接、可诊断。

**`RecipeFileManifest` 里的 `recipe` 是完整解析后的结构，而不是路径引用。** 也就是说清单本身携带全部内容。这让「列配方」这一步变重（要解析每份文件），换来的是列出来之后**不必再按需加载**。

这是一个可以和 `d02`（会话类型）对照的取舍：那里清单只带元数据、内容按需拉取；这里清单直接带内容。选择依据是**规模**——本地配方是几十份小 YAML，一次全解析比 N 次往返便宜。

**`resolve_recipe_sub_recipe_paths` 用 `recipe_path.parent()` 而不是当前工作目录。** 这是子配方解析的正确基准点：子配方的相对路径是相对**引用它的那份配方文件**，而不是进程 cwd。这也解释了为什么这个函数要接收 `recipe_path`——它需要的是「这份文件在哪」，而不是「这个配方是什么」。

**两个 `let Some(...) else { return }` 是静默的空操作。** 一个没有父目录的路径（理论上 root）或一份没有子配方的配方，都直接返回。后者是常态（绝大多数配方没有子配方），所以不告警是对的。

## 关键取舍

**源码事实：** 短 id 是路径的 8 字节哈希、定长 16 个十六进制字符；与路径绑定而非内容绑定，所以编辑内容不改 id、移动文件改 id。没有碰撞处理。

**源码事实：** 清单按 `last_modified` 字符串倒序，这在 RFC 3339 UTC 下与时间序一致，但该依赖没有注释说明。

**源码事实：** 取不到 metadata 的条目静默跳过（无日志），与 `e02` 里「设置文件读失败要告警」形成对照——依据是文件数量级不同。

**源码事实：** 清单里的 `Recipe` 已解析子配方路径，调用方不必再做；解析失败保留原相对路径而不报错。

**源码事实：** 子配方的解析基准是「引用它的那份配方文件所在目录」，不是 cwd。

**源码事实：** 解析在「扫清单」与「从路径加载」两个入口分别发生，因为按 id 加载不经过清单——这层重复由测试守住。

**源码事实：** 按 id 加载会扫全部配方（O(数量) 次文件读与解析），所以不适合在循环里用。

**架构推论：** 这个模块是**「文件系统即数据库」**的最小形态：索引（清单）、主键（短 id）、排序（修改时间）、关联（子配方路径）。它与真正的数据库差在两件事上：没有持久索引（每次重扫）、没有引用完整性（子配方缺失不报错）。

而「扫描即重算」这个选择在本地配方这个规模下是合理的——它免除了「索引什么时候失效」这个永远答不清的问题。代价是可预期的时间随文件数线性增长，以及**每次列举都重新解析全部 YAML**。若配方数量涨到几百上千，第一个该动的地方就是这里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/recipe/manifest.rs` | `RecipeFileManifest` | 清单条目：短 id、配方内容、路径、修改时间 |
| `crates/goose/src/recipe/manifest.rs` | `list_recipe_file_manifests` | 扫全部本地配方、解析子配方路径、按时间倒序 |
| `crates/goose/src/recipe/manifest.rs` | `short_id_from_path` | 路径的 8 字节稳定哈希，定长 16 位十六进制 |
| `crates/goose/src/recipe/manifest.rs` | `get_recipe_file_path_by_id` | 按 id 反查路径，未找到报 `Recipe not found` |
| `crates/goose/src/recipe/manifest.rs` | `load_recipe_by_id` | 两步加载：先找路径再解析 |
| `crates/goose/src/recipe/manifest.rs` | `load_recipe_from_path` | 从路径加载并解析子配方路径 |
| `crates/goose/src/recipe/manifest.rs` | `resolve_recipe_sub_recipe_paths` | 子配方相对路径 → 绝对路径，失败保留原值 |
| `crates/goose/src/recipe/manifest.rs` | `test_short_id_from_path_is_stable` | 钉住「同一路径同一 id」这个性质 |

## 参考

- [goose 源码：recipe/manifest.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/recipe/manifest.rs)
- [goose 文档：配方参考](https://block.github.io/goose/docs/guides/recipes/recipe-reference/)
- [RFC 3339：日期与时间的互联网表示](https://www.rfc-editor.org/rfc/rfc3339)
- [Rust 文档：std::hash::DefaultHasher](https://doc.rust-lang.org/std/collections/hash_map/struct.DefaultHasher.html)

## 与主核 / AI 的关系

这套「扫描目录 → 生成清单 → 短 id → 按需加载」与主核里的「表目录缓存」同构：数据库也需要一个「有哪些表、各自在哪、何时改过」的索引。区别在于数据库把这个索引持久化并做失效管理（`pg_class`、`relcache` 失效消息），而这个模块每次重扫。

对 agent 工程的直接价值有三条。其一，**id 绑什么决定了它的语义**。绑路径意味着「编辑不换 id、移动换 id」，绑内容则相反；选哪个取决于「用户引用的是这份配方还是这个文件」。其二，**加载入口有几条，解析就要发生几次**。清单入口与路径入口都做路径解析不是冗余，而是因为二者不共用代码路径——这类重复要用测试钉住，否则新增第三个入口时会漏。其三，**静默跳过的门槛要按数量级定**。少数几份用户显式配置的文件坏了该告警；一个可能有大量文件的目录里，单份坏掉不值得刷日志。
