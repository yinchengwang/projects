---
project: goose
feature: recipe-validation
title: 配方校验与斜杠命令
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/recipe/validate_recipe.rs
    symbols: [convert_recipe_from_value, RecipeFileFormat]
  - path: crates/goose/src/slash_commands/recipe_slash_command.rs
    symbols: [SlashCommandMapping, set_recipe_slash_command]
---

# 配方校验与斜杠命令

## 一句话职责

把「一份配方文件是否可用」拆成**两条路径**：一条是给调度器用的严格校验（文件格式先判、再把 YAML 值转成 `Recipe`、失败时给出**定位到字段**的错误），一条是界面/命令行用的加载校验（多解析一层模板、再查字段间关系）；而斜杠命令把「一个个配方文件」变成**可调用命令**，命令名的绑定存在配置里。

## 机制是什么

两块，各自的内部分层是本页的重点。

**`validate_recipe.rs`（1340 行）的主干：**

| 层 | 内容 |
|---|---|
| 格式判定 | `RecipeFileFormat::{Json, Yaml}` + `recipe_file_format(path)`：**扩展名是 json 就按 JSON，其余一律按 YAML** |
| 值 → 结构 | `convert_recipe_from_value(value, format, coerce_scalars)`：`serde_path_to_error` 反序列化，失败时 `classify_conversion_error` |
| 错误分类 | `SchedulerRecipeError::{GenericParse(格式), InvalidSchema(定位信息)}` |
| 字段定位 | `SchemaNode` / `SafeSchemaPath` / `schema_field` / `safe_schema_path` + 从错误消息里回捞字段名的两个解析函数 |
| 模板层 | `validate_recipe_template`：解析模板 → 校验参数 → 校验非参数不变式；`ValidatedRecipeTemplate` 还能 `render` 后**再校验一遍** |
| 参数层 | `validate_parameter_limits`（三道上限）、`validate_optional_parameters`、`validate_parameters_in_template`（双向差集） |
| 单项不变式 | `validate_prompt_or_instructions`、`validate_retry_config`、`validate_json_schema` |

**`recipe_slash_command.rs` 的主干：** `SlashCommandMapping { command, recipe_path }`、`list_commands` / `save_slash_commands` / `set_recipe_slash_command`、`runnable_recipes`、`recipe_entry`（从配方文件推出描述与参数提示）、`build_recipe`（把命令行参数拼成配方）。

## 怎么实现的

### 格式判定与转换

**`recipe_file_format` 的默认方向是「不是 json 就当 yaml」。** 实现只检查 `extension().eq_ignore_ascii_case("json")`，其余（`.yaml`、`.yml`、**无扩展名**、以及任何别的扩展名）都返回 `Yaml`。

这个默认是对的，因为 `serde_yaml::from_str` 能解析 JSON（JSON 是 YAML 的子集）。于是「认不出来就按 YAML 试」不会比「报错」更差，反而多接受了一类输入。

**`convert_recipe_from_value` 的第三个参数 `coerce_scalars` 是一个「宽容度开关」。** 实现是：

```rust
let result = if coerce_scalars {
    serde_path_to_error::deserialize(RecipeValueDeserializer::new(value))
} else {
    serde_path_to_error::deserialize(value)
};
```

也就是说：**只有直接加载（`nested_recipe.is_none()`）时才用那个会做标量强转的反序列化器**。而 `validate_recipe_for_scheduling` 里这一段是关键：

```rust
let nested_recipe = document.get("recipe");
let recipe_value = nested_recipe.unwrap_or(&document);
let mut recipe = convert_recipe_from_value(recipe_value, format, nested_recipe.is_none())?;
```

于是：

- **文档是「裸配方」**（顶层就是配方的字段）→ `coerce_scalars = true` → 标量强转打开；
- **文档是「包里包配方」**（顶层有 `recipe` 键）→ `coerce_scalars = false` → 不加强转。

这是一个**由文件形状决定的宽容度**。合理的解释是：裸配方是最常见的手写形态，作者的意图就是写配方，所以「`max_turns: "30"` 这种字符串化数字」应当被容忍；而嵌套形态（`recipe:` 下）更像是由工具生成的包装，那里应当严格。

我不确定这个解释是不是作者的原意——但它至少说明**宽容度不是全局常量，而是根据输入的可信度分级**。

### 错误分类：把「解析失败」与「字段不对」分开

**`SchedulerRecipeError` 只有两个变体，而这个二分是全篇最重要的设计。** `GenericParse(格式)` 表示「这份文件根本不是合法 JSON/YAML」，`InvalidSchema(定位信息)` 表示「能解析，但某个字段不对」。而 `Display` 实现把前者渲染成 `Invalid JSON recipe` / `Invalid YAML recipe`，后者渲染成 `Invalid recipe: {message}`。

这个二分对**调度器**这种自动调用方是必要的：前者意味着「文件坏了，别重试」，后者意味着「字段写错了，可以指给用户看」。

**`classify_conversion_error` 是那个「把反序列化错误翻译成可读定位」的函数，逻辑相当细。** 四步：

1. **先判路径是否安全**：`safe_schema_path(error.path())`——若拿不到（`None`），直接退回 `GenericParse`。
2. **把原始路径字符串与错误消息做规整**：`error.path().to_string()` 在根路径时是 `"."`，转成空串；message 里再 `strip_path_prefix` 去掉路径前缀（`strip_error_location` 先剥 serde 的位置信息）。
3. **回捞字段名**：若路径未被截断（`!safe_path.truncated`），用 `schema_field_from_message` 从消息里找 `missing field \`X\`` 或 `duplicate field \`X\`` 这两种前缀，然后**用 `schema_field(safe_path.node, field)` 校验这个字段名在 schema 里真的存在**。
4. **生成最终消息**：字段确认存在时给出 `{路径}: missing field \`X\``（路径为空时只给后半段）；否则若路径非空给 `{路径} is invalid`；路径也空就退回 `GenericParse`。

第 3 步里那句「校验这个字段名在 schema 里真的存在」是本页最讲究的一处。理由可以从它的必要性推出：**消息里的字段名可能是从用户数据里回显的**（一个未知字段的名字会被 serde 变体或 serde_yaml 带进消息），而一个由数据构造的诊断文案会带来两个问题——用户可以注入任意文本进错误消息；以及「不存在的字段」被当成真实字段名报告，会误导。

所以这里做了一次**白名单校验**：只有 schema 里真有的字段名才被写进诊断。

**`SafeSchemaPath` 带一个 `truncated` 标记。** 路径被截断（例如数组下标过多或层级过深）时不做字段回捞，因为「半截路径 + 字段名」组合出来的定位可能指向错误的位置。这是一个「宁可少给信息也不给错信息」的判断。

**`SchemaNode` 与 `schema_field` / `schema_sequence_item` 是一套手写的 schema 漫游。** 它们让「路径 → 节点」的推进不依赖 serde 的类型信息，而是按 `SchemaNode` 的变体逐段前进。这也解释了为什么需要 `safe_schema_path`：路径里的段可能是数据驱动的（数组下标），只有 schema 声明为序列的才能推进。

### 参数校验：发散在三个方向

**三道上限是硬编码常量，且三道各自不同。**

| 常量 | 值 | 量纲 |
|---|---|---|
| `MAX_PARAMETERS` | 32 | 参数个数 |
| `MAX_TOTAL_SELECT_OPTIONS` | 200 | 所有参数的可选项总数 |
| `MAX_PARAMETERS_SIZE_BYTES` | 128 KiB | 序列化后的参数体积 |

第三道的实现方式值得记：`serde_yaml::to_string(&parameters)` 之后比 `len()`——**用一个序列化动作来量体积**。这比手工估算字段开销更准（它包含键名、缩进、引号），代价是多一次序列化。

而三个常量各自防的东西不同：个数防「界面被撑爆」，选项总数防「下拉框有几千项」，字节数防「参数里塞了巨量文本」。**三者都不是另一个的推论**——32 个参数每个带 6 个选项远不到 128 KiB，而 2 个参数也可以各带一个长默认值把体积顶上去。这段的分层是对的。

**`validate_optional_parameters` 把三条规则捆在一起，且第二条是安全规则。** 它做三件事：

1. 先跑三道上限；
2. **`File` 类型的参数不许有 `default`**，错误文案说明了意图：`to avoid importing sensitive user files`，并把违规的 key 列出来；
3. **`Optional` 类型的参数必须有 `default`**，错误文案是 `Please provide defaults.`

第 2 条就是 `f01` 里那条「`File` 参数不许有默认值」的安全约束的实际执行点。而它的**报告方式**值得注意：它**收集全部违规项再一次性报**（`file_params_with_defaults.join(", ")`），而不是遇到第一个就返回。

一次报全对配置类错误是必要的：用户改一处再重试十次，比一次看到十处要差得多。第 3 条同样收集全部（`optional_params_without_default_values`）。

**`validate_parameters_in_template` 的核心是双向差集。** 它先删掉一个内置模板变量（`BUILT_IN_RECIPE_DIR_PARAM`，即配方目录本身），然后：

- 收集定义的 key，重复定义直接报 `Duplicate parameter definition`；
- `missing_keys = template_variables - param_keys`——**模板里用了但没定义**；
- `extra_keys = param_keys - template_variables`——**定义了但模板没用**；
- 两者都为空才算通过，否则把两者拼进同一条消息（各自一行）。

「定义了但没用」也算错误，这个严格程度值得注意：它会让「留一个以后要用的参数」变成校验失败。而理由可以从对称性推出——两个方向的失调都说明配方与它的参数声明不同步。

消息拼接的细节：missing 在前、extra 在后，各自只有非空时才追加，最后 `trim_end()` 去掉可能的尾换行。于是当只有 extra 时，消息以 `\n` 开头——一个小的瑕疵，但 `trim_end` 处理的是尾巴。

**`validate_prompt_or_instructions` 用 `trim().is_empty()` 而不是 `is_none()`。** 也就是说**一个只含空白的 `instructions` 不算有指令**。这修正了一类实际输入：模板里写 `instructions: {{ maybe_empty }}`，渲染后得到一串空格。

### 两条加载路径的区别

**校验有两个入口，两条路径做的事不同档。**

| 入口 | 走什么 |
|---|---|
| `validate_recipe_for_scheduling(content, dir, format)` | 先 `prepare_recipe_template` 拿模板变量 → `serde_yaml` 转 Value → `convert_recipe_from_value`（带格式判断与错误分类）→ `ensure_analyze_for_developer` / `ensure_summon_for_subrecipes` → 逐项校验 → 返回 `Recipe`，错误是 `SchedulerRecipeError` |
| `validate_recipe_template(content, dir)` → `validate_recipe_template_from_content` / `..._from_file` | `parse_recipe_template` → `validate_recipe_parameters`（含双向差集）→ `validate_recipe_non_parameter_invariants`，错误是 `anyhow` |

差异有三处值得记：

1. **调度路径做隐式修正**（两个 `ensure_*`），加载路径不做。也就是说调度器拿到的是「已被修正成可执行形态」的配方。
2. **调度路径的错误类型是结构化的**（能区分「文件坏了」与「字段不对」），加载路径统一 `anyhow`。
3. **调度路径额外做一次 `validate_json_schema` 的错误映射**：只有 `JSON schema must be an object` 与 `Empty JSON schema is not allowed` 两条具体消息被直传，其余都归成 `response.json_schema is invalid`。这是又一次「白名单化诊断文案」——`validate_json_schema` 内部可能回显 schema 内容。

### 渲染后再校验一次

**`ValidatedRecipeTemplate::render` 会在渲染之后重跑校验。** 实现是：`render(params)` 得到新的内容与模板变量 → `Recipe::from_content` → `validate_recipe_parameters` → `validate_recipe_non_parameter_invariants`。

**这是本页最容易被漏掉的一处设计。** 校验通过的模板，在渲染后可能与校验时完全不同：参数值可以改变结构（例如一个参数值里含换行、含 `---`、或含另一个模板变量）。所以「模板合法」不保证「渲染结果合法」——必须两道门都过。

也就是说这道机制的正确理解是：**校验的时点是「最终产物」，而模板校验只是提前拦截一部分错误**。.

### 斜杠命令：把文件变成命令

**映射的存储是「配置文件里的一个键」，而键的名字与内容都很朴素。** `SLASH_COMMANDS_CONFIG_KEY = "slash_commands"`，值是一个 `Vec<SlashCommandMapping>`，每项只有 `command` 与 `recipe_path`。

`list_commands` 的失败方向是空列表 + `warn!`：一个读不出来的配置被当成「没有自定义命令」，命令列表不会因此崩。这与 `e02` 里「读设置失败告警并跳过该层」是同一取向。

**`set_recipe_slash_command` 的写法是「先删后加」，这正是幂等的实现。** 实现：

1. `commands.retain(|m| m.recipe_path != recipe_path_str)`——**先把这份配方原有的映射全部删掉**；
2. 若给了新命令，`trim_start_matches('/')` 去前导斜杠再 `to_lowercase()`；
3. 归一化后非空才插入。

于是同一个调用既是「设置」也是「取消」（`command: None` 时只删不加），且**重复设置不会产生两条**。

归一化的三个动作（去前导斜杠、转小写、空则忽略）说明命令名是**不区分大小写且不带斜杠的归一形态**——但只归一「写入」这一侧。读取侧 `runnable_recipes` 又调了 `normalize_command_name`，也就是说写入过一次之后还可能被别处改过（用户手改配置文件），所以读取时也要归一。

**`runnable_recipes` 的三重过滤把「配置里写着」与「能用」分开。** 条件是：

```
!name.is_empty() && !is_builtin_command(&name) && recipe_path.exists()
```

- 名字归一后非空；
- **不与内置命令冲突**；
- **文件还在**。

第三条是本页最实用的一条：映射里存的是路径字符串，而配方文件可能被删或移动。一个指向不存在文件的映射会被**静默跳过**，而不是在命令列表里显示一个点了就失败的命令。

第二条（不与内置命令冲突）说明命令空间是共享的：`is_builtin_command(&name)` 让内置命令优先，用户的自定义映射不能遮蔽它们。

**`commands_from_mappings` 与 `runnable_recipes` 共享同样的前两步过滤，但多了第三步。** 它还要 `recipe_entry(...)?`——也就是说**取不到配方元数据（读文件失败、校验失败）的映射也不出现在列表里**。

而 `recipe_entry` 的实现是：

```rust
let recipe_content = std::fs::read_to_string(recipe_path).ok()?;
let recipe_dir = recipe_path.parent()?;
let validation_result = validate_recipe_template_from_content(...).ok()?;
```

**`.ok()?` 两处**——读失败或校验失败都返回 `None`，于是那个命令从列表里消失。这意味着**一个写坏的配方会让它的斜杠命令从列表里静默消失**，而不是显示成一条错误。

这是一个产品判断：命令列表是为了「能用的命令」，而不是「配置过的命令」。代价是用户改坏配方后找不到命令、也不知道为什么——除非他自己去看配置。

**`input_hint_for_recipe` 把参数声明渲染成命令行的用法提示。** 它把参数分两类：

- `Required` 与 `UserPrompt` → `<key>`
- `Optional` → `[--<key> <key>]`

然后按「必填全部在前、可选全部在后」拼接。于是「用户需要什么」直接出现在命令补全里，不必打开文件看。

注意 `UserPrompt` 被归进必填——这与 `f01` 里把它作为独立变体并列的语义有细微差别：在界面上它是必填的，在模型侧它可能是要主动追问的。

**`build_recipe` 的错误前缀把命令名带上。** 所有失败都过 `invalid_recipe_msg(command, e)` → `Recipe /{command} is not valid: {reason}`。这是一个正确的诊断取向：用户在命令行里只知道自己敲了 `/foo`，错误消息必须提到 `/foo` 而不只是路径。

而它调用的校验是 **`validate_recipe_template_from_content`**（加载路径），不是调度路径——也就是说通过斜杠命令运行的配方走的是「不加强转、不做隐式修正」的那条。

## 关键取舍

**源码事实：** 格式只在扩展名是 `.json` 时判成 JSON，其余（含无扩展名）一律按 YAML——因为 JSON 是 YAML 的子集，认不出就按 YAML try 不会更差。

**源码事实：** 标量强转只在「裸配方」（顶层无 `recipe` 键）时开启；嵌套形态走严格反序列化。宽容度按输入形状分级。

**源码事实：** 错误分成 `GenericParse`（文件根本不可解析）与 `InvalidSchema`（能解析但字段不对）两类，因为调度器需要区分「别重试」与「可以指给用户看」。

**源码事实：** 诊断里的字段名要过 schema 白名单才写进消息；路径被截断时不做字段回捞。这是「不给错信息优先于多给信息」。

**源码事实：** 参数三道上限分属三个不同量纲（个数 32、选项总数 200、序列化体积 128 KiB），互不推出。

**源码事实：** `File` 参数不许有默认值、`Optional` 参数必须有默认值——两条都把违规项**收集全再一次性报**，而不是遇错即返。

**源码事实：** 「模板里用了但没定义」与「定义了但没用」都是错误，两个方向都查。

**源码事实：** 调度路径做隐式修正（`ensure_analyze_for_developer` / `ensure_summon_for_subrecipes`）且错误结构化；加载路径不做修正、错误统一 `anyhow`。斜杠命令走加载路径。

**源码事实：** `ValidatedRecipeTemplate::render` 在渲染后**重新校验一次**，因为模板合法不保证渲染结果合法。

**源码事实：** 斜杠命令映射按「先删后加」写入，天然幂等；命令名归一化在读写两侧都做。

**源码事实：** 引用不存在文件的映射、以及配方校验失败的映射，都从命令列表里**静默跳过**。

**架构推论：** 这 1340 行的分布本身就是一条结论：**校验的成本集中在「错误怎么说」，而不是「规则是什么」**。真正的规则（三道上限、两条参数约束、双向差集、三个不变式）加起来不到一百行；剩下的是 `SchemaNode` 漫游、字段名白名单回捞、路径规整、以及两套错误类型。

这个比例是对的，因为校验器的用户价值几乎全在「这条错误能不能让我改对」。而它最难的部分是**在不信任输入的前提下产出可信的诊断**——这正是「用户数据可能被回显进错误消息」这一条带来的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/recipe/validate_recipe.rs` | `RecipeFileFormat` / `recipe_file_format` | 扩展名判格式，非 json 一律 yaml |
| `crates/goose/src/recipe/validate_recipe.rs` | `convert_recipe_from_value` | 值 → `Recipe`，带 `coerce_scalars` 开关 |
| `crates/goose/src/recipe/validate_recipe.rs` | `classify_conversion_error` | 把反序列化错误翻译成可读定位 |
| `crates/goose/src/recipe/validate_recipe.rs` | `SchedulerRecipeError` | 两个变体：解析失败 / 字段不对 |
| `crates/goose/src/recipe/validate_recipe.rs` | `SafeSchemaPath` / `SchemaNode` / `schema_field` | schema 漫游与「路径是否可安全展示」 |
| `crates/goose/src/recipe/validate_recipe.rs` | `validate_recipe_for_scheduling` | 调度路径：格式判定 + 转换 + 隐式修正 + 逐项校验 |
| `crates/goose/src/recipe/validate_recipe.rs` | `ValidatedRecipeTemplate::render` | 渲染后重跑校验 |
| `crates/goose/src/recipe/validate_recipe.rs` | `validate_parameter_limits` | 三道上限（个数 / 选项总数 / 序列化体积） |
| `crates/goose/src/recipe/validate_recipe.rs` | `validate_optional_parameters` | `File` 禁默认值、`Optional` 必须有默认值，全收集再报 |
| `crates/goose/src/recipe/validate_recipe.rs` | `validate_parameters_in_template` | 双向差集 + 重复定义 |
| `crates/goose/src/recipe/validate_recipe.rs` | `validate_prompt_or_instructions` | 用 `trim().is_empty()` 判「有指令」 |
| `crates/goose/src/slash_commands/recipe_slash_command.rs` | `SlashCommandMapping` | 命令名与配方路径的一对映射 |
| `crates/goose/src/slash_commands/recipe_slash_command.rs` | `set_recipe_slash_command` | 先删后加，幂等；命令名归一 |
| `crates/goose/src/slash_commands/recipe_slash_command.rs` | `runnable_recipes` | 三重过滤：非空、非内置、文件存在 |
| `crates/goose/src/slash_commands/recipe_slash_command.rs` | `recipe_entry` | 从配方推出描述与参数提示，失败返回 `None` |
| `crates/goose/src/slash_commands/recipe_slash_command.rs` | `input_hint_for_recipe` | 参数声明 → 命令行用法提示 |
| `crates/goose/src/slash_commands/recipe_slash_command.rs` | `build_recipe` | 把命令行参数拼成配方，错误带命令名 |

## 参考

- [goose 源码：recipe/validate_recipe.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/recipe/validate_recipe.rs)
- [goose 源码：slash_commands/recipe_slash_command.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/slash_commands/recipe_slash_command.rs)
- [serde_path_to_error 文档](https://docs.rs/serde_path_to_error/latest/serde_path_to_error/)
- [goose 文档：配方参考](https://block.github.io/goose/docs/guides/recipes/recipe-reference/)

## 与主核 / AI 的关系

这一层与主核里的「DDL 校验」几乎逐条对应：数据库也把「语法错」（解析失败，别重试）与「语义错」（列不存在、类型不匹配，可以指给用户看）分成两级错误码；也有一套「多少个列、多少个子分区、多大的元组」的量纲不同的上限；也在诊断消息里遵守「不把用户数据原样回显」。

对 agent 工程的直接价值有三条。其一，**宽容度应该按输入形状分级，而不是全局统一**。裸配方容忍字符串化数字，嵌套包装则严格——这比「一律严格」或「一律宽松」都更贴近真实用法。其二，**诊断里的用户数据要过白名单**。把「缺失字段名」回捞出来再校验它真的在 schema 里，既防注入又防误导；这是校验器里最容易被跳过的安全细节。其三，**模板校验不等于产物校验**。参数值可以改变结构，所以渲染后必须再跑一遍——否则「校验通过」会变成一种错觉。
