---
project: goose
feature: recipe-model
title: 配方模型与参数
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/recipe/mod.rs
    symbols: [Recipe, RecipeParameter, RecipeBuilder]
  - path: crates/goose/src/recipe/mod.rs
    symbols: [SubRecipe, RecipeParameterRequirement, RecipeParameterInputType]
---

# 配方模型与参数

## 一句话职责

把「一次会话该怎么跑」写成一份可序列化的文档：`Recipe` 声明标题、描述、指令、要启用的扩展、设置、参数、期望的响应 schema 与子配方；`RecipeParameter` 把「用户要填的东西」建模成**带输入类型与需求级别**的声明。

## 机制是什么

`recipe/mod.rs` 里有六个核心类型：

| 类型 | 字段要点 |
|---|---|
| `Recipe` | 三个必填（`version`、`title`、`description`）+ 九个可选（`instructions`、`prompt`、`extensions`、`settings`、`activities`、`author`、`parameters`、`response`、`sub_recipes`、`retry`） |
| `Settings` | `goose_provider`、`goose_model`、`temperature`、`max_turns`，全部可选 |
| `Author` | `contact`、`metadata`，可选 |
| `Response` | `json_schema: serde_json::Value`，**必填** |
| `SubRecipe` | `name`、`path`（必填）+ `values`、`sequential_when_repeated`、`description` |
| `RecipeParameter` | `key`、`input_type`、`requirement`、`description` 必填 + `default`、`options` 可选 |
| `RecipeParameterRequirement` | `Required` / `Optional` / `UserPrompt` |
| `RecipeParameterInputType` | `String` / `Number` / `Boolean` / `Date` / `File` / `Select` |

## 怎么实现的

**`version` 是必填但带默认值，这个组合是一个声明。** 字段声明是 `#[serde(default = "default_version")] pub version: String`，注释写「version of the file format, sem ver」。

也就是说**线上文件可以不写 version**——它会被填成当前格式版本。于是「老配方没有版本字段」与「新配方显式写了版本」都能解析。这是一个正确的兼容策略：版本字段的作用是**写入方声明**，而不是**读取方要求**。

**「至少要有 instructions 或 prompt 之一」是一条写在注释里的约束，不是类型约束。** 源码注释：`Note: at least one of instructions or prompt need to be set`。两个字段都是 `Option<String>`，所以类型系统允许两个都为 `None`。

这条约束留给校验层（`validate_recipe.rs`）去查。这解释了那个文件为什么有 1340 行——**很多规则是「字段间的组合约束」，无法用单个字段的类型表达**。

**九个可选字段全部带 `skip_serializing_if = "Option::is_none"`。** 这是一致的：一个没设置的字段不会序列化成 `null`。于是序列化出的配方与手写的配方形状一致（与 `e01` 里 `available_tools` 空列表不写出是同一取向）。

**`extensions` 的反序列化被委托给一个适配器。** `deserialize_with = "recipe_extension_adapter::deserialize_recipe_extensions"`——也就是说配方的 `extensions` 字段有两个时代：可能是一个名字列表（老写法），也可能是完整的 `ExtensionConfig`。适配器负责两种都读进来。

这个设计的收益是**配方文件可以只写扩展名**（`extensions: [developer, computercontroller]`），而不必在文件里嵌完整的传输配置。代价是多一层反序列化，且这两种形态在序列化回去时只有一种。

**`SubRecipe.values` 有一个把任意 JSON 值转成字符串的反序列化器。** `deserialize_value_map_as_string` 的实现：先按 `Option<HashMap<String, Value>>` 反序列化，然后逐个：

- 是 `Value::String(s)` 就直接用；
- 其它（数字、布尔、对象、数组）就 `serde_json::to_string(&v)`。

于是配方作者可以写 `values: { count: 3 }` 而不必写 `values: { count: "3" }`。

这是一个「宽容输入、统一内部表示」的取舍：内部类型是 `HashMap<String, String>`（因为参数最终都要变成模板里的文本），而外部允许更自然的 YAML 写法。代价是**类型信息在这里被抹掉了**——一个 `true` 与 `"true"` 在内部不可区分。

**`RecipeParameterInputType` 有一个变体带注释说明安全理由。** `File` 上的注释：

> File parameter that imports content from a file path. **Cannot have default values to prevent importing sensitive user files.**

这是一个**在类型层面无法表达、只能在校验层执行**的约束——`default` 是 `Option<String>`，它不区分 `input_type`。所以「`File` 类型的参数不许有默认值」必须由校验器检查。

这条约束的安全含义很清楚：若 `File` 参数能有默认值，一份第三方配方就能指定 `default: ~/.ssh/id_rsa`，从而在用户运行时自动读取敏感文件而不需要用户输入。

**两个枚举都用 `snake_case` 重命名，且都手写了一个 `Display` 实现。** `#[serde(rename_all = "snake_case")]` 让线格式是 `required` / `user_prompt` / `file` / `select` 这类小写形式。

而 `Display` 的实现方式很特别：

```rust
write!(f, "{}", serde_json::to_string(self).unwrap().trim_matches('"'))
```

也就是说**`Display` 不是手写字符串，而是序列化后再去掉引号**。这保证了「显示出来的字」与「序列化出来的字」永不漂移——一个枚举变体改名时，只有 `rename_all` 那处需要改，而 `Display` 自动跟随。

`.unwrap()` 在这里是安全的（一个无字段的枚举序列化不会失败），但这个写法的真正价值是**消除了两处需要同步的字符串**。

**`Recipe` 有一个私有方法做自动注入：`ensure_analyze_for_developer`。** 注释：**当一个配方有老的 builtin developer 扩展但没有 analyze 时，自动注入 analyze**。

这是一个**向后兼容的隐式升级**：老配方里 `developer` 是一个更早的、功能更窄的扩展，而新的 `analyze` 扩展承担了其中一部分。自动注入让老配方不必被改写就能获得新行为。

值得注意的是它是**私有**的而不是公开 API——也就是说这是一个内部修正，不构成对外契约。

**`Response.json_schema` 是必填的，且校验方式是「试着构造一个工具」。** `Response::validate()` 的实现是 `FinalOutputTool::try_new(self.json_schema.clone()).map(|_| ())`。

也就是说**schema 的合法性不是通过 JSON Schema 校验器检查，而是通过「能不能被最终输出工具接受」来检查**。这是一个务实的选择：真正要用这个 schema 的地方就是那个工具，所以「工具能否接受」是唯一的判据，比另写一套 schema 校验更贴近实际。

**`RecipeBuilder` 是给代码构造配方用的，`version` 有默认值而 `title`/`description` 是 `Option`。** 字段列表与 `Recipe` 相同，但必填字段被放松成 `Option`（`version` 除外，它在 builder 里是 `String`）。这符合 builder 的语义：`build()` 时才检查完整性。

**`Settings` 的四个字段都是「覆盖项」而不是「全部配置」。** 没有 `response`、没有 `extensions`——那些在 `Recipe` 顶层。`goose_provider` / `goose_model` 是选择模型，`temperature` / `max_turns` 是行为调整。全部 `Option` 意味着「配方可以只指定它关心的那些，其余用运行时默认」。

## 关键取舍

**源码事实：** `version` 必填但带默认值，于是老配方（无版本字段）也能解析；版本字段是写入方声明而非读取方要求。

**源码事实：** 「至少要有 `instructions` 或 `prompt`」写在注释里而非类型里，留给校验层——这类字段间约束是 `validate_recipe.rs` 体积的主要来源。

**源码事实：** 九个可选字段全部 `skip_serializing_if = "Option::is_none"`，序列化出的形状与手写文件一致。

**源码事实：** `extensions` 走适配器反序列化，支持「只写名字」与「完整配置」两种时代形态。

**源码事实：** `SubRecipe.values` 允许任意 JSON 值并统一转成字符串——宽容输入、统一内部表示，代价是类型信息被抹掉。

**源码事实：** `File` 类型参数不许有默认值，这条安全约束无法在类型层面表达（`default` 不区分 `input_type`），必须由校验层执行。理由是防止第三方配方自动读取用户敏感文件。

**源码事实：** 两个参数枚举的 `Display` 由「序列化后去引号」实现，于是显示的字符串与线格式永不漂移。

**源码事实：** `Response::validate` 的判据是「能否被最终输出工具构造」，而不是另跑一遍 schema 校验。

**源码事实：** `ensure_analyze_for_developer` 是私有的向后兼容隐式升级，让老配方自动获得新扩展。

**架构推论：** 这个模型的形状说明了一件事：**配方的「合法性」分成两层**。一层是**形状**（字段类型、可选性、默认值），由 serde 与 `Option` 表达；另一层是**关系**（至少一个指令、`File` 不许默认值、参数上限、字节上限），必须由校验器检查。

后一层才是这类文档格式的难点：它需要人读懂「这个配方的意图是否成立」，而这类判断永远无法完整地压进类型。于是 `Recipe` 保持宽松（能解析就解析），把严格留给 `validate_recipe.rs`——这样一份大体正确的配方至少能被读出来给人看，而不是在解析阶段就彻底失败。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/recipe/mod.rs` | `Recipe` | 配方主体：三必填 + 九可选 |
| `crates/goose/src/recipe/mod.rs` | `Recipe::ensure_analyze_for_developer` | 老配方的隐式扩展升级 |
| `crates/goose/src/recipe/mod.rs` | `Settings` / `Author` | 运行设置覆盖项与作者信息 |
| `crates/goose/src/recipe/mod.rs` | `Response` / `Response::validate` | 期望 JSON schema，用最终输出工具作判据 |
| `crates/goose/src/recipe/mod.rs` | `SubRecipe` | 子配方的名字、路径与参数值 |
| `crates/goose/src/recipe/mod.rs` | `deserialize_value_map_as_string` | 任意 JSON 值统一转字符串 |
| `crates/goose/src/recipe/mod.rs` | `RecipeParameter` | 参数的键、输入类型、需求级别、默认值与选项 |
| `crates/goose/src/recipe/mod.rs` | `RecipeParameterRequirement` | `Required` / `Optional` / `UserPrompt` |
| `crates/goose/src/recipe/mod.rs` | `RecipeParameterInputType` | 六种输入类型，`File` 带安全约束注释 |
| `crates/goose/src/recipe/mod.rs` | `RecipeBuilder` | 代码构造用的 builder |

## 参考

- [goose 源码：recipe/mod.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/recipe/mod.rs)
- [goose 文档：配方](https://block.github.io/goose/docs/guides/recipes/recipe-reference/)
- [serde 文档：字段属性（default / skip_serializing_if）](https://serde.rs/field-attrs.html)
- [JSON Schema 规范与草案索引](https://json-schema.org/)

## 与主核 / AI 的关系

配方与主核里的「预编译查询模板 / 命名计划」是同构的：都是把「要做什么」序列化成一份文档，让另一台机器（或另一个会话）能原样执行。共同点是**形状层由反序列化保证、关系层必须单独校验**——数据库的执行计划也有「语法正确但语义不合法」的大量情形（列不存在、类型不匹配、索引缺失）。

对 agent 工程的直接价值有三条。其一，**安全约束经常无法在类型里表达**。「`File` 类型不许有默认值」这条如果只靠 `Option<String>` 的类型，任何人都能写出 `default: ~/.ssh/id_rsa`；必须有一处显式检查。其二，**默认值的方向决定了兼容性**。`version` 必填但带默认值，于是老文件能读；反过来要求写方必须写版本，会让所有存量配方一次性失效。其三，**「显示的字符串」与「线格式的字符串」要共享一个真值源**。用「序列化后去引号」实现 `Display`，消除了两处需要同步的字符串——这类同步是最容易在重构时漏掉的。
