---
project: open-interpreter
feature: code-mode
title: 代码模式下的工具定义
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/tools/src/code_mode.rs
    symbols: [augment_tool_spec_for_code_mode, collect_code_mode_tool_definitions, code_mode_name_for_tool_name]
  - path: codex-rs/tools/src/code_mode.rs
    symbols: [tool_spec_to_code_mode_tool_definition, augmented_description_for_spec]
---

# 代码模式下的工具定义

## 一句话职责

把「给模型看的工具集」翻译成「给代码运行时用的工具集」：生成一套代码模式下的函数名、附上执行样例到描述里、过滤掉不能嵌套调用的工具、并按名字排序去重。

## 机制是什么

代码模式（Code Mode）是一种执行形态：模型不再逐个发出工具调用，而是**写一段代码**，在运行时里调用工具函数。这带来一个命名问题——两个环境里的名字空间不一样：

| 环境 | 工具标识 | 例子 |
|---|---|---|
| 模型侧（Responses API） | 命名空间 + 工具名，两层结构 | namespace `fs`，tool `read` |
| 代码运行时 | 一个扁平标识符，必须能写成函数名 | `fs__read` |

模型 API 允许把工具分组到命名空间（`ToolSpec::Namespace`），而运行时里没有命名空间概念——只有一个标识符。所以需要一个**把两层压成一层**的编码规则，且这个编码必须是无损的（能从扁平名恢复出命名空间与工具名，用于把运行时的调用路由回正确的工具）。

这个模块做的全部工作就是这次翻译，外加三件配套的事：

- **给描述追加执行样例**（让模型知道这个工具在代码里怎么用）
- **过滤不能嵌套的工具**（有些工具在代码模式里没有意义或不可用）
- **排序去重**（同一批里可能有重名）

## 怎么实现的

**命名规则是这一页最需要精确理解的一段。**

```rust
pub fn code_mode_name_for_tool_name(tool_name: &ToolName) -> String {
    if tool_name.is_default_namespace() {
        return tool_name.name.clone();
    }

    match tool_name.namespace.as_deref() {
        Some(namespace) if namespace.ends_with('_') || tool_name.name.starts_with('_') => {
            format!("{namespace}{}", tool_name.name)
        }
        Some(namespace) => format!("{namespace}__{}", tool_name.name),
        None => tool_name.name.clone(),
    }
}
```

四支，逐个说：

1. **默认命名空间 → 直接用短名**。`is_default_namespace()` 为真意味着这个工具属于「无命名空间」那类，扁平名就是工具名本身。这样最常用的工具在代码里写起来最短（`read(...)` 而不是 `default__read(...)`）。

2. **命名空间以 `_` 结尾，或工具名以 `_` 开头 → 直接拼接，不加分隔符**。这一支是为了**避免双下划线**。若 namespace 是 `fs_` 而 tool 是 `read`，直接拼成 `fs_read` 而不是 `fs___read`；若 namespace 是 `fs` 而 tool 是 `_read`，拼成 `fs_read`。两种情况下用户已经在用下划线当分隔，再加 `__` 只会让名字难看且难解析。

3. **其余 → `{namespace}__{tool}`**。双下划线是常规分隔符。

4. **无命名空间 → 短名**。与第 1 支等效但不依赖 `is_default_namespace` 的判定——即「namespace 字段为 `None`」这一具体情形。

这个规则的关键后果是：**分隔符的形态不唯一**。`fs__read`、`fs_read`、`read` 三种形态都可能出现，取决于 namespace 与 tool 各自是否已经带下划线。这意味着**从扁平名反解回 (namespace, tool) 不能只靠 split**——必须在生成侧维护映射。

而 `CodeModeToolDefinition` 恰好同时带两个名字：

```rust
CodeModeToolDefinition {
    name: code_mode_name_for_tool_name(&tool_name),   // 扁平名，给运行时
    tool_name,                                        // 两层名，给宿主路由
    ...
}
```

即**不需要反解**——两个名字同时存在，路由时用 `tool_name`，运行时用 `name`。这个设计避开了「从字符串猜结构」的整类问题。这一点很值得记：当一个标识符要跨两个命名空间时，与其想办法编码成可解析的字符串，不如**两侧都带上**。

**`augment_tool_spec_for_code_mode` 只改描述，不改结构。**

```rust
pub fn augment_tool_spec_for_code_mode(spec: ToolSpec) -> ToolSpec {
    match spec {
        ToolSpec::Function(mut tool) => {
            let Some(description) = augmented_description_for_spec(&ToolSpec::Function(tool.clone())) else {
                return ToolSpec::Function(tool);
            };
            tool.description = description;
            ToolSpec::Function(tool)
        }
        // Freeform 同样
        ToolSpec::Namespace(mut namespace) => { /* 逐个工具改描述 */ }
        other => other,
    }
}
```

三处：

- **`other => other`**：不能进代码模式的工具原样返回，不改。所以这个函数对任意 `ToolSpec` 都安全。
- **`tool.clone()` 传进 `augmented_description_for_spec`**：那里需要完整 spec 来生成 `CodeModeToolDefinition`（含 schema），但只取 `.description`。克隆一次 spec 是这里的代价。
- **`None` 分支原样返回**：若直接把工具名不当成能嵌套的工具（`is_code_mode_nested_tool` 为假），拿不到增强描述，就不改。

`augmented_description_for_spec` 的实现很短：

```rust
code_mode_tool_definition_for_spec(spec)
    .map(codex_code_mode::augment_tool_definition)
    .map(|definition| definition.description)
```

即「构造定义 → 增强 → 取描述」。真正的描述改写发生在 `codex_code_mode::augment_tool_definition`，那是另一个 crate。这里只负责把 spec 转成它能接受的输入。所以**样例文本的具体内容不在这份代码里**——这一点对理解职责边界很重要：`tools/src/code_mode.rs` 是适配层，不是样例的生成者。

`Namespace` 分支里对每个工具都重新构造 `tool_name`：

```rust
let tool_name = ToolName::namespaced(namespace.name.clone(), tool.name.clone());
let definition = CodeModeToolDefinition {
    name: code_mode_name_for_tool_name(&tool_name),
    tool_name,
    description: tool.description.clone(),
    kind: CodeModeToolKind::Function,
    input_schema: serde_json::to_value(&tool.parameters).ok(),
    output_schema: tool.output_schema.as_ref().map(ToolOutputSchema::to_value),
};
tool.description = codex_code_mode::augment_tool_definition(definition).description;
```

注意两个 schema 的处理：

- **`input_schema: serde_json::to_value(&tool.parameters).ok()`**：转成 JSON 值，失败时是 `None`。用 `.ok()` 而不是传播错误，是因为**代码模式的描述增强不需要 schema**——schema 只是构造定义时的一个字段，缺了不影响描述生成。这是一处「尽力而为」。
- **`output_schema: tool.output_schema.as_ref().map(ToolOutputSchema::to_value)`**：只在有输出 schema 时才物化。与 d05 讲的那套共享存储一致——这里调用 `to_value()` 会**克隆**一份 JSON，所以只在确实需要时才做。

**`tool_spec_to_code_mode_tool_definition` 有一层「名字必须合格」的过滤。**

```rust
pub fn tool_spec_to_code_mode_tool_definition(spec: &ToolSpec) -> Option<CodeModeToolDefinition> {
    let definition = code_mode_tool_definition_for_spec(spec)?;
    codex_code_mode::is_code_mode_nested_tool(&definition.name)
        .then(|| codex_code_mode::augment_tool_definition(definition))
}
```

两处：

- **`code_mode_tool_definition_for_spec` 只取第一个定义**（`code_mode_tool_definitions_for_spec(spec).into_iter().next()`）。所以这个函数只适用于「一个 spec 对应一个工具」的形态（Function / Freeform），对 Namespace 会只取第一个并静默丢弃其余。这是它的使用范围，不是 bug——调用方若要处理 Namespace 应该用 `collect_code_mode_tool_definitions`。
- **`is_code_mode_nested_tool(&definition.name)`** 是判定「这个名字在代码模式里能不能被当函数调用」。它由 `codex_code_mode` crate 提供，判据大概是「名字是否是合法的标识符」。若不合格则不增强、不返回。

注意它在**增强之前**判定，用的是扁平名 `definition.name`——这正是需要判定的名字（`tool_name` 是两层结构，不是运行时看到的东西）。

**`collect_code_mode_tool_definitions` 是批量版，多做三件事。**

```rust
let mut tool_definitions = specs
    .into_iter()
    .flat_map(|spec| {
        let mut definitions = code_mode_tool_definitions_for_spec(spec);
        if let ToolSpec::Namespace(namespace) = spec {
            let namespace_description = namespace.description.trim();
            if !namespace_description.is_empty() {
                for definition in &mut definitions {
                    definition.description = format!("{namespace_description}\n\n{}", definition.description);
                }
            }
        }
        definitions
    })
    .filter(|definition| codex_code_mode::is_code_mode_nested_tool(&definition.name))
    .map(codex_code_mode::augment_tool_definition)
    .collect::<Vec<_>>();
tool_definitions.sort_by(|left, right| left.name.cmp(&right.name));
tool_definitions.dedup_by(|left, right| left.name == right.name);
```

三件事：

1. **命名空间的描述被前置到每个成员的描述。** 若 namespace 有非空描述，就用 `format!("{namespace_description}\n\n{}", ...)` 拼在每个成员的描述前面。这处理了一个具体的信息损失问题：扁平化后命名空间层级消失，而 namespace 的描述（「这一组工具用于文件系统操作」）对模型理解成员用途有帮助。于是把它下推到每个成员。

   注意 `.trim()` 后再判空——纯空白的 namespace 描述不该产生一个空段落。

2. **`filter` 在 `augment` 之前。** 顺序有意义：先剔掉不合格的名字，只为留下来的做增强（增强可能涉及字符串拼接，是有的成本的）。

3. **`sort` 后 `dedup`。** 用 `name` 排序再按 `name` 去重——这是 Rust 里 `dedup` 能生效的前提（它只比较相邻元素）。排序的副产物是**输出顺序由名字决定**，与输入顺序无关，这对测试与稳定性都有好处。

去重的必要性来自「同一批 specs 里可能有重名工具」——例如两个扩展都提供了一个叫 `read` 的工具扁平化后撞名。这里的选择是**保留第一个**（`dedup_by` 保留首次出现的），而不是报错或加后缀。这会让「后注册的同名工具」静默消失，是一个值得注意的取舍。

**第二个批量函数少了两步。**

```rust
pub fn collect_code_mode_exec_prompt_tool_definitions<'a>(specs: impl IntoIterator<Item = &'a ToolSpec>) -> Vec<CodeModeToolDefinition> {
    let mut tool_definitions = specs
        .into_iter()
        .flat_map(code_mode_tool_definitions_for_spec)
        .filter(|definition| codex_code_mode::is_code_mode_nested_tool(&definition.name))
        .collect::<Vec<_>>();
    tool_definitions.sort_by(|left, right| left.name.cmp(&right.name));
    tool_definitions.dedup_by(|left, right| left.name == right.name);
    tool_definitions
}
```

与上一个比，缺两样：

- **没有 namespace 描述前置**。
- **没有 `augment_tool_definition`**（没有追加执行样例）。

名字里的 `exec_prompt` 说明了用途：这是给**执行提示**用的清单，而不是给模型看的工具描述。执行提示里不需要「怎么调用这个工具」的样例（运行时自己知道），也不需要 namespace 的说明文字（那是给模型读的）。所以这条路径只做「扁平化 + 过滤 + 排序去重」。

**这两个函数并存本身就是一条设计信息**：同一份 spec 集合要产出**两种形态的清单**，差异恰好是「给模型看的描述」与「给运行时的标识符」。把它们拆成两个函数而不是加参数，是因为两者的消费者完全不同，而参数化会让调用点难以看出自己在给谁准备数据。

**`code_mode_tool_definitions_for_spec` 的四支 `match`。**

```rust
match spec {
    ToolSpec::Function(tool) => vec![...],     // 单个，kind = Function，带 input_schema
    ToolSpec::Freeform(tool) => vec![...],     // 单个，kind = Freeform，无 schema
    ToolSpec::Namespace(namespace) => namespace.tools.iter().map(...).collect(),  // 多个
    ToolSpec::ImageGeneration { .. } | ToolSpec::LocalShell { .. }
        | ToolSpec::ToolSearch { .. } | ToolSpec::WebSearch { .. } => Vec::new(),
}
```

最后一支是**明确的排除清单**：四类工具在代码模式下没有定义。它们不是「因为名字不合格被过滤掉」，而是**根本不生成定义**。这个区别在诊断时有意义：一个 `web_search` 工具不会出现在代码模式里，不是命名问题，是这类工具被明确排除。

`Freeform` 与 `Function` 的差别体现在三个字段：

- `kind: CodeModeToolKind::Freeform` vs `Function`
- `input_schema: None` vs `Some(parameters)`——自由形态没有参数 schema
- `output_schema: None`——自由形态也没有输出 schema

用 `CodeModeToolKind` 这个判别位而不是靠「schema 是否为 None」推断，是同一族设计（d02 那种「另加判别位而非拆字段」的手法）。

`Namespace` 分支里 `Function` 与 `Custom` 也各有对应：

- `ResponsesApiNamespaceTool::Function` → `kind: Function`，带 input/output schema。
- `ResponsesApiNamespaceTool::Custom` → `kind: Freeform`，两者皆 `None`。

注意 `Custom` 的 `name` 同样走 `code_mode_name_for_tool_name`——即命名空间编码规则对两种成员类型一视同仁。

## 关键取舍

**源码事实：** 扁平名有四种形态：默认命名空间给短名；namespace 以 `_` 结尾或 tool 以 `_` 开头时直接拼接（避免双下划线）；其余用 `{namespace}__{tool}`；namespace 为 `None` 时给短名。因此**分隔符不唯一**。

**源码事实：** 因为分隔符不唯一，`CodeModeToolDefinition` 同时携带扁平 `name`（给运行时）与两层 `tool_name`（给宿主路由），不靠从字符串反解。

**源码事实：** `augment_tool_spec_for_code_mode` 只改 `description` 字段，对不支持的 spec 原样返回；描述内容由 `codex_code_mode::augment_tool_definition` 生成，本模块只是适配层。

**源码事实：** `code_mode_tool_definitions_for_spec` 明确排除四类工具（`ImageGeneration`、`LocalShell`、`ToolSearch`、`WebSearch`），不生成任何定义，而不是靠名字过滤。

**源码事实：** 批量收集时命名空间的非空描述会被前置到每个成员描述之前（`{ns_desc}\n\n{member_desc}`），以补偿扁平化后丢失的层级说明。

**源码事实：** 两个批量函数的差异是「是否前置 namespace 描述 + 是否追加执行样例」；`exec_prompt` 版两者都不做，因为它是给运行时清单用的。

**源码事实：** 先按名字排序再 `dedup_by`（只比较相邻），因此去重依赖排序；重名时保留首次出现，后来者静默消失。

**架构推论：** 「两侧都带上名字」是这一页最可迁移的手法。跨命名空间的标识符有两条路：编码成可解析的字符串，或两侧各带一份。编码成字符串必然要定一套语法（分隔符、转义、合法性），而语法一旦定下就变成隐式契约；两侧各带则完全没有反解问题。这里的代价是 `CodeModeToolDefinition` 多一个字段。

**架构推论：** 命名规则里那条「避免双下划线」的分支说明这套扁平化是**为了人的可读性**服务的，而不只是为机器。运行时并不在意标识符是否好看，但模型要在代码里写这些名字——`fs_read` 比 `fs___read` 更容易被正确生成。这与 a04 讲的「提示形状影响模型效果」是同一条线。

**架构推论：** 明确排除四类工具（而不是让它们被名字过滤掉）体现了一种区分：**「这类工具在代码模式里没有意义」与「这个名字不能当函数名」是两件不同的事**。前者是设计决定，后者是语法约束。合并处理会让诊断混乱——用户会以为换个名字就能用。

**架构推论：** 重名时静默保留第一个，是这个模块里最值得提出的一点。两个扩展提供同名工具时，用户会看到其中一个消失且没有任何提示。更完整的做法是让上层的工具注册去处理冲突（报错或加命名空间前缀），而不是在扁平化这一步静默吞掉。不过在扁平化层做「确定性去重」至少保证了输出稳定。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/tools/src/code_mode.rs` | `code_mode_name_for_tool_name` | 两层名压成扁平名的四条规则 |
| `codex-rs/tools/src/code_mode.rs` | `augment_tool_spec_for_code_mode` | 只改写描述的适配，其余原样返回 |
| `codex-rs/tools/src/code_mode.rs` | `augmented_description_for_spec` | 构造定义 → 增强 → 取描述 |
| `codex-rs/tools/src/code_mode.rs` | `tool_spec_to_code_mode_tool_definition` | 单个 spec 的定义，含「名字可当函数名」的过滤 |
| `codex-rs/tools/src/code_mode.rs` | `collect_code_mode_tool_definitions` | 批量：namespace 描述前置 + 过滤 + 增强 + 排序去重 |
| `codex-rs/tools/src/code_mode.rs` | `collect_code_mode_exec_prompt_tool_definitions` | 批量但不增强描述，供执行提示使用 |
| `codex-rs/tools/src/code_mode.rs` | `code_mode_tool_definition_for_spec` | 只取第一个定义，用于单工具形态 |
| `codex-rs/tools/src/code_mode.rs` | `code_mode_tool_definitions_for_spec` | 四支 `match`，明确排除四类工具 |

## 参考

- [open-interpreter 源码：tools/src/code_mode.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/tools/src/code_mode.rs)
- [OpenAI 文档：Responses API 工具命名空间](https://platform.openai.com/docs/api-reference/responses)
- [Rust 文档：Vec::dedup_by](https://doc.rust-lang.org/std/vec/struct.Vec.html#method.dedup_by)
- [MCP 规范：工具定义](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

「把两层命名空间压成一层标识符」与主核里的**限定名解析**是同一类问题：主核要把 `schema.table` 解析成内部的对象 id，而 SQL 方言允许多种省略形式（省略 schema、用引号、别名）。主核的解法是**在解析阶段就确定对象 id**，之后的执行计划只带 id，不再回解字符串。本页的做法完全同构：扁平化的同时保留 `tool_name`，后续路由用后者而非前者。

「避免双下划线」那条规则与主核里**标识符规范化**的动机相同：可读性影响正确使用。主核在生成列别名时要避免与已有名字冲突并保持可读；这里避免 `fs___read` 也是为了让模型少写错。

「明确排除四类工具」对应主核里**不支持的操作列表**：某些 SQL 语法在主核里被明确拒绝（例如某些窗口函数组合），而不是「解析失败」。两条路的结果相同（不可用），但错误信息的质量完全不同——前者能说「本引擎不支持」，后者只能说「语法错误」。

对 AI 工程的实际意义在于：代码模式是近年一个重要的 agent 执行形态（模型写代码而非逐个发工具调用），而它的工程难点集中在**两个命名空间的对齐**上。这份实现给出的三条经验可直接用：**两侧各带名字而不是编码成可解析的字符串**、**把命名空间的描述下推到成员以补偿层级丢失**、以及**把「这类工具不支持」与「这个名字不合法」分开处理**。
