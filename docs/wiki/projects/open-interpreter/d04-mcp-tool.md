---
project: open-interpreter
feature: mcp-tool
title: MCP 工具的转换与上限
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/tools/src/mcp_tool.rs
    symbols: [MAX_MCP_TOOL_DESCRIPTION_BYTES, parse_mcp_tool, parse_mcp_tool_with_description_limit]
  - path: codex-rs/tools/src/mcp_tool.rs
    symbols: [parse_agent_plugin_mcp_tool, mcp_call_tool_result_output_schema]
---

# MCP 工具的转换与上限

## 一句话职责

把 MCP 服务器发来的 `rmcp::model::Tool` 转成宿主内部的 `ToolDefinition`：补上游 API 硬要求的字段、走 d02 的 schema 清洗、给描述加字节上限、并构造「调用结果信封」的输出 schema。

## 机制是什么

这个模块只有 86 行，但它处理三个跨界问题，每一个都对应一个具体的「两边不一样」：

| 两边 | 差异 | 处理 |
|---|---|---|
| MCP 的 `Tool` → 内部 `ToolDefinition` | MCP 工具可能不带 `properties`，而 OpenAI API 强制要求 | 补一个空对象 |
| MCP 的输入 schema → 内部受限类型 | 任意合法 JSON Schema | 走 `parse_tool_input_schema`（见 d02） |
| MCP 的输出 schema → 内部输出类型 | MCP 给的是 `structuredContent` 的形状，而实际返回的是**信封**（含 `content` / `isError`） | 构造信封 schema，把 MCP 的那份塞进 `structuredContent` 属性 |

第三个差异是这一页最需要说清的一处，因为它是「MCP 协议与宿主模型的语义不重合」的典型表现。

## 怎么实现的

**两个公开入口，差异只有一个字节上限。**

```rust
const MAX_MCP_TOOL_DESCRIPTION_BYTES: usize = 1_000;

pub fn parse_mcp_tool(tool: &rmcp::model::Tool) -> Result<ToolDefinition, serde_json::Error> {
    parse_mcp_tool_with_description_limit(tool, /*description_limit*/ None)
}

pub fn parse_agent_plugin_mcp_tool(tool: &rmcp::model::Tool) -> Result<ToolDefinition, serde_json::Error> {
    parse_mcp_tool_with_description_limit(tool, Some(MAX_MCP_TOOL_DESCRIPTION_BYTES))
}
```

即：普通 MCP 工具的描述**不限长度**，而「agent 插件提供的 MCP 工具」描述被截到 1000 字节。

这个不对称值得想一想。为什么插件工具要限？因为插件是**第三方内容**，其描述会直接进入模型上下文；若不限，一个插件可以塞进几万字的描述，既挤占上下文又是注入的载体。而普通 MCP 工具（用户自己配置的服务器）被视为可信度更高的一类，不额外限制。

注意 1000 字节这个量级：它大约是 250 个英文单词或几百个汉字——足够说清一个工具的用途，但不足以塞进一篇文档。这是一个「够用但不占地方」的阈值。

**`take_bytes_at_char_boundary` 而不是简单截断。**

```rust
description: tool.description.as_deref()
    .map(|description| match description_limit {
        Some(limit) => take_bytes_at_char_boundary(description, limit).to_string(),
        None => description.to_string(),
    })
    .unwrap_or_default(),
```

用 `take_bytes_at_char_boundary` 而不是 `&description[..limit]`：后者在 UTF-8 边界不对时会 panic。而 MCP 工具的描述几乎必然含非 ASCII（中文、emoji、非拉丁文字），所以按字节截断必须做边界修正。这个辅助函数来自 `codex_utils_string`，语义是「截到不超过 limit 的那个字符边界」。

`unwrap_or_default()` 让「没有描述」变成空字符串而不是 `None`——因为 `ToolDefinition.description` 是非空字段（`String` 而非 `Option<String>`）。这一个细节决定了调用方不必处理缺失描述。

**补 `properties`：注释直接点出了上游要求。**

```rust
let mut serialized_input_schema = serde_json::Value::Object(tool.input_schema.as_ref().clone());

// OpenAI models mandate the "properties" field in the schema. Some MCP
// servers omit it (or set it to null), so we insert an empty object to
// match the behavior of the Agents SDK.
if let serde_json::Value::Object(obj) = &mut serialized_input_schema
    && obj.get("properties").is_none_or(serde_json::Value::is_null)
{
    obj.insert("properties".to_string(), serde_json::Value::Object(serde_json::Map::new()));
}
```

三处：

- **`.clone()` 一份**：`tool.input_schema` 是 `Arc<Map>`，这里需要可变副本才能插字段。克隆是必要的代价——不能改服务器给的原值（它可能被别处共享）。
- **`is_none_or(Value::is_null)`**：同时处理「字段不存在」与「字段存在但为 `null`」两种情形。后者是 MCP 服务器把可选字段写成显式 null 的常见做法。用 `is_none_or` 一个组合子覆盖两种，比 `if obj.get("properties").map_or(true, Value::is_null)` 更直接。
- **`if let ... && ...`**：Rust 的 let-chain，把「是对象」与「需要补」两条件写在一起，少一层嵌套。这符合仓库 AGENTS.md 里「Always collapse if statements」的要求。

补完这一个字段后立刻交给 d02 的清洗：

```rust
let input_schema = parse_tool_input_schema(&serialized_input_schema)?;
```

注意**错误类型是 `serde_json::Error`**，即这个函数的失败只可能来自 schema 反序列化。所以「补 `properties`」与「清洗」两步都不会失败（除了清洗末尾的单例 null 检查）——这让 `parse_mcp_tool` 的签名很干净。

而 d02 里也有一条 `ensure_default_children_for_schema_types` 会补 `properties`。这意味着**这里补的与那里补的会重叠**。这不是冗余，因为顺序不同：

- 这里补的是**顶层** schema 的 `properties`（在清洗之前），确保 OpenAI API 的要求一定被满足。
- d02 里补的是**每一个** object 类型节点的 `properties`（在清洗过程中）。

两处都在补同一个字段，但作用的层级不同。若只有 d02 那处，顶层 schema 若不是 object 类型就不会被补；若只有这里，嵌套的对象节点仍可能缺 `properties`。

**输出 schema 的构造是这一页的核心机制。**

```rust
pub fn mcp_call_tool_result_output_schema(structured_content_schema: JsonValue) -> JsonValue {
    let mut schema = json!({
        "type": "object",
        "properties": {
            "content": { "type": "array", "items": { "type": "object" } },
            "structuredContent": null,
            "isError": { "type": "boolean" },
            "_meta": { "type": "object" }
        },
        "required": ["content"],
        "additionalProperties": false
    });
    schema["properties"]["structuredContent"] = structured_content_schema;
    schema
}
```

先看这个信封的形状，它对应 MCP 的 `CallToolResult`：

| 属性 | 类型 | 说明 |
|---|---|---|
| `content` | array of object | 内容块列表（文本、图片等），**必填** |
| `structuredContent` | 由服务器提供 | 结构化结果，MCP 新版特性 |
| `isError` | boolean | 是否工具级错误 |
| `_meta` | object | 元数据 |

而 `required` 只有 `["content"]`，`additionalProperties: false`。

三处判断值得读：

1. **`"structuredContent": null` 作为占位，然后再替换。** 注释解释了理由：

   > Replacing the placeholder keeps the existing property order while moving the supplied schema instead of cloning it through `json!`.

   即这个写法有两层目的：一是**保持属性顺序**（`content`、`structuredContent`、`isError`、`_meta`）——若把 `structuredContent` 放到最后再插，顺序会变；二是**移动而不是克隆**（`json!` 宏里若直接嵌入一个 owned 值会做一次转换，而先写 `null` 再赋值是纯移动）。

   属性顺序在 JSON 里语义上不重要，但**对模型看到的文本有影响**：模型读 schema 是按顺序读的，把 `content` 放第一个（它是必填的）比较合理。所以这不是无意义的讲究。

2. **`structuredContent` 直接赋成服务器给的那份 schema**，而不是包一层。即服务器声明的就是「结构化结果本身的形状」，信封只是把它装起来。这与 `required` 的取舍配合——`structuredContent` **不是必填**，因为很多 MCP 服务器不提供它（它是较新的可选特性）。

3. **`additionalProperties: false`**。即信封里只允许这四个键。这与「MCP 的 `CallToolResult` 就只有这些字段」对应，且能防止服务器塞额外的顶层键。注意这是一个**严格的**选择，与 d02 里「偏向放宽」的整体取向相反——因为这里是宿主自己构造的 schema，不是外部给的。

**`some(schema) | none` 的等价性在 d05 里有个有意思的处理。** 这里 `structured_content_schema` 是一个 `JsonValue`，调用方把「服务器没给」表达成 `Value::Object(空 Map)`：

在 `mcp_tool.rs` 里：

```rust
output_schema: Some(ToolOutputSchema::from_mcp_output_schema(tool.output_schema.clone())),
```

传的是 `tool.output_schema.clone()`，类型是 `Option<Arc<Map<String, Value>>>`。而 `from_mcp_output_schema` 签名接受 `Option<Arc<Map<...>>>`，所以「没有结构化 schema」这一信息被**保留成 `None`** 而不是被压成空对象。

到 d05 那篇会看到 `to_value` 如何把 `None` 变成 `Value::Object(Map::new())`（即 `{}`）——一个空对象 schema 在 JSON Schema 里含义是「接受任何东西」，所以「服务器没声明结构化结果」被表达成「结构化结果是任意形状」。这是合理的：既然没声明，就不能假设它有约束。

**`defer_loading: false` 是唯一一个没有注释的字段。**

```rust
Ok(ToolDefinition {
    name: tool.name.to_string(),
    description: ...,
    input_schema,
    output_schema: Some(...),
    defer_loading: false,
})
```

`ToolDefinition` 的五个字段里，前四个都从 MCP 工具来，只有 `defer_loading` 是**硬编码的**。它的语义从 `ToolDefinition::into_deferred` 可以推出来：

```rust
pub fn into_deferred(mut self) -> Self {
    self.output_schema = None;
    self.defer_loading = true;
    self
}
```

即「延迟加载的工具没有输出 schema 并可被延后注册」。所以 MCP 工具既然**有** `output_schema`，它必然不是延迟加载的——`false` 与 `Some(output_schema)` 是自洽的一对。

这个「自洽但不显眼」的耦合值得一提：两个字段表达同一个事实（是否延迟加载），改动其一必须记得改另一个。更紧的写法是把「延迟」与「输出 schema」合成一个枚举变体（`OutputSchemaStorage` 那种形态），从而不可能不一致。

**`name` 直接取 `tool.name.to_string()`，不做任何改写。** 即 MCP 工具名原样进入内部定义。这与 d03 讲的代码模式命名形成对照——那里必须扁平化两层名，这里没有层级要压（MCP 工具名本身是一个扁平字符串，可能带 `-` 或 `.`）。至于这些字符是否能在代码模式下当函数名，由 d03 的 `is_code_mode_nested_tool` 判定。

## 关键取舍

**源码事实：** 两个入口只差描述字节上限：普通 MCP 工具不限，agent 插件提供的 MCP 工具截到 1000 字节，且截断走 `take_bytes_at_char_boundary` 以避免 UTF-8 边界 panic。

**源码事实：** 顶层 `properties` 在清洗**之前**被补上空对象，覆盖「字段缺失」与「显式为 null」两种情形，理由是 OpenAI API 强制要求该字段。

**源码事实：** 这个函数唯一可能的失败是 `serde_json::Error`，即 schema 反序列化失败。

**源码事实：** 输出 schema 是一个信封：`content`（必填数组）、`structuredContent`（由服务器 schema 填充）、`isError`（布尔）、`_meta`（对象），并设 `additionalProperties: false`。

**源码事实：** 信封里 `structuredContent` 先写成 `null` 占位再替换，以保持属性顺序并避免通过 `json!` 克隆传入的 schema。

**源码事实：** `structuredContent` 不是必填，因为很多 MCP 服务器不提供结构化结果。

**源码事实：** `defer_loading: false` 与 `output_schema: Some(...)` 是自洽的一对——延迟加载的工具按 `into_deferred` 的定义没有输出 schema。

**架构推论：** 信封 schema 的存在揭示了一个协议层的错位。MCP 的 `output_schema` 描述的是 `structuredContent` 的形状，而宿主实际拿到的是一个**完整的 `CallToolResult`**。若不构造信封，模型的输出校验会认为「返回值应该直接是结构化结果」从而对每次都返回信封的实际结果报错。这个修正是必须的，且必须在宿主侧做——服务器无法知道宿主怎么包装返回值。

**架构推论：** 「插件工具限 1000 字节、普通工具不限」这条不对称，本质上是**信任分级**。插件来自第三方分发渠道，描述经由它进入模型上下文；用户自己配的服务器被视为自己选择的可信来源。这类分级在 agent 系统里很常见（远端 MCP、插件、内置工具各有不同待遇），而把它落实成两个函数而不是一个带参数的函数，让调用点必须显式选择信任级别。

**架构推论：** 「补 `properties` 在清洗之前」这个顺序不是随意的。d02 的清洗会在「无类型无线索」时 `map.clear()`，若顶层 schema 恰好是这种情况，补的字段也会一起没掉——但那时整个 schema 已经变成 `{}`（接受任何输入），OpenAI 的 `properties` 要求也就无从满足了。这个顺序暴露了 d02 那处激进清空的一个实际后果。

**架构推论：** `additionalProperties: false` 出现在一个**宿主自建**的 schema 里，与 d02 里对外部 schema 普遍放宽的取向形成对照。判据是「谁在定义这份 schema」：外部给的只能尽量接受，自己造的不妨严格。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/tools/src/mcp_tool.rs` | `parse_mcp_tool` | MCP 工具 → 内部定义，描述不限长 |
| `codex-rs/tools/src/mcp_tool.rs` | `parse_agent_plugin_mcp_tool` | 同一转换但描述截到 1000 字节 |
| `codex-rs/tools/src/mcp_tool.rs` | `parse_mcp_tool_with_description_limit` | 主体：补 `properties`、清洗、截断、构造输出 schema |
| `codex-rs/tools/src/mcp_tool.rs` | `MAX_MCP_TOOL_DESCRIPTION_BYTES` | 插件工具描述的字节上限 |
| `codex-rs/tools/src/mcp_tool.rs` | `mcp_call_tool_result_output_schema` | 构造 `CallToolResult` 信封 schema 并嵌入结构化结果 schema |

## 参考

- [open-interpreter 源码：tools/src/mcp_tool.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/tools/src/mcp_tool.rs)
- [MCP 规范：工具](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)
- [MCP 规范：结构化工具结果](https://modelcontextprotocol.io/specification/2025-06-18/server/tools#structured-content)
- [OpenAI 文档：函数调用与 JSON Schema](https://platform.openai.com/docs/guides/function-calling)

## 与主核 / AI 的关系

「外部系统给的对象要包一层信封才能被内部消费者正确理解」与主核里的**元组格式适配**同源：主核的存储层期望元组按某种布局（含头部信息），而调用方给的是裸值。适配层负责补头部、设标记、保证必填字段存在。

「插件限长、普通工具不限」这条与主核里**不同来源的输入享有不同信任级别**一致：主核对待客户端直连的语句与经扩展加载的语句，在权限检查与资源限额上并不相同。关键是把分级写进**函数签名**而不是一个布尔参数——调 `parse_agent_plugin_mcp_tool` 的人必须意识到自己在处理插件。

「`additionalProperties: false` 只在自己造的 schema 上出现」这条判据很值得带走：**对外部输入放宽、对自产结构严格**。放宽外部输入是为了让它们能被接入；严格自产结构是为了让错误尽早暴露。这两个方向看起来矛盾，但作用于不同的对象，合起来才是完整的策略。

对 AI 工程的实际意义在于：MCP 是 agent 能力扩展的主渠道，而其工具定义与宿主期望之间存在多处错位（字段缺失、schema 表达力、结果信封）。这份实现给出的三条经验可直接用：**把上游 API 的硬要求在适配层统一补齐**（而不是让每个服务器自己猜）、**按来源分级限制第三方内容的体积**、以及**为结果构造信封 schema**（否则输出校验会对每个响应报错）。
