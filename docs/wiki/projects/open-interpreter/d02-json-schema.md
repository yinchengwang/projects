---
project: open-interpreter
feature: json-schema
title: 工具输入 schema 的清洗
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/tools/src/json_schema.rs
    symbols: [parse_tool_input_schema, sanitize_json_schema, prepare_tool_input_schema]
  - path: codex-rs/tools/src/json_schema.rs
    symbols: [DEFINITION_TABLE_KEYS, has_composition_keyword, sanitize_schema_table]
---

# 工具输入 schema 的清洗

## 一句话职责

MCP 服务器给出的 JSON Schema 是任意合法的 JSON Schema，而这个项目只能处理其中一个受限子集。这个模块负责把前者**降级**成后者——补上缺的、塌掉不认识的、裁掉用不到的——让注册不因一份奇怪的 schema 而整体失败。

## 机制是什么

问题的来源是一次很具体的冲突：MCP 工具的 `input_schema` 由**外部服务器**提供，可能是任意合法的 JSON Schema（含 `$ref`、`anyOf`、`const`、布尔 schema、顶层 `type` 为数组、嵌套的 `$defs`…）；而这个项目内部用的是自己定义的 `JsonSchema` 类型，只能表达一个子集。

三种可能的应对：

| 策略 | 后果 |
|---|---|
| 严格校验，不认识的直接拒绝 | 大量 MCP 服务器无法接入 |
| 原样透传，运行期出错再说 | 模型拿到无效 schema，调用必错 |
| **清洗成可表达的近似** | 能接入，且语义大体正确 |

这里选的是第三种。它把「兼容性」交给一次彻底的重写，而不是交给调用方去猜。

而这个选择也定下了整页的语义基调：**这些清洗规则不是「正确的 JSON Schema 语义」，而是「在这个受限类型下最接近原意的近似」**。所以必须逐条理解每条规则在做什么近似，因为每一条都在某处放弃了精度。

## 怎么实现的

**入口是两级：带压缩与不带压缩。**

```rust
pub fn parse_tool_input_schema(input_schema: &JsonValue) -> Result<JsonSchema, serde_json::Error> {
    let mut input_schema = prepare_tool_input_schema(input_schema);
    compact_large_tool_schema(&mut input_schema);
    deserialize_tool_input_schema(input_schema)
}

pub fn parse_tool_input_schema_without_compaction(input_schema: &JsonValue) -> Result<JsonSchema, serde_json::Error> {
    deserialize_tool_input_schema(prepare_tool_input_schema(input_schema))
}
```

而 `prepare_tool_input_schema` 只做两件事：

```rust
sanitize_json_schema(&mut input_schema);
prune_unreachable_definitions(&mut input_schema);
```

即：**先清洗（让每个节点都能被表达），再裁剪（把用不到的 definition 删掉）**。顺序不能反——若先裁剪，被裁掉的可能是「清洗之后才变得可达」的引用。带压缩的版本多一步「大 schema 压缩」，那是体积优化，与正确性无关。

注意 `parse_tool_input_schema_without_compaction` 的文档：「Parse a **trusted** tool `input_schema` without running large-schema compaction」。即压缩是给不可信/超大 schema 的保护，自己代码里写死的 schema 不必付这个代价。

**`sanitize_json_schema` 是主体，递归处理四类输入。**

第一类：**布尔 schema 直接塌成字符串。**

```rust
JsonValue::Bool(_) => { *value = json!({ "type": "string" }); }
```

JSON Schema 允许整个 schema 写成 `true`（接受任何值）或 `false`（拒绝任何值）。而内部类型只表达「带 `type` 的对象」。于是两者都被改成 `{"type": "string"}`。

注意这里是**两个语义相反的值被映射成同一个结果**。注释写的是「Coerce to an accept-all string」——即按「接受任何值」这一侧处理。对 `true` 这是合理的（本来就接受任何东西，收窄成字符串会改变语义，但至少不会拒绝合法的字符串输入）；对 `false` 则**彻底改变了语义**（本该拒绝一切，现在接受字符串）。这是一个刻意的失准：宁可放宽也不拒绝注册。

第二类：**数组、对象、其它标量**。数组逐元素递归；对象走下面一整套；其它标量不动。

对象的处理顺序值得逐行读，因为**顺序本身就是语义**：

**（1）先递归子节点。** 按固定的子键集合：

```rust
if let Some(properties) = map.get_mut("properties") { /* 递归每个值 */ }
if let Some(items) = map.get_mut("items") { sanitize_json_schema(items); }
if let Some(additional_properties) = map.get_mut("additionalProperties")
    && !matches!(additional_properties, JsonValue::Bool(_)) { sanitize_json_schema(additional_properties); }
if let Some(value) = map.get_mut("prefixItems") { sanitize_json_schema(value); }
for key in COMPOSITION_SCHEMA_KEYS { if let Some(value) = map.get_mut(key) { sanitize_json_schema(value); } }
for table in DEFINITION_TABLE_KEYS { sanitize_schema_table(map, table); }
```

六处：`properties` 的每个值、`items`、`additionalProperties`（**布尔除外**）、`prefixItems`、三个组合关键字（`anyOf`/`oneOf`/`allOf`）、两个 definition 表。

`additionalProperties` 跳过布尔是有意的：JSON Schema 里 `additionalProperties: false` 是「不许额外属性」，`true` 是「随便加」。它作为布尔是有语义的，不该被递归清洗掉。

**（2）`const` 塌成单值 `enum`。**

```rust
if let Some(const_value) = map.remove("const") {
    map.insert("enum".to_string(), JsonValue::Array(vec![const_value]));
}
```

`const: 5` 与 `enum: [5]` 语义等价，而内部类型只认 `enum`。这是全函数里唯一一处**精确等价**的改写，其余都是近似。用 `remove` 而不是 `get` 是必要的：若同时留着 `const` 与 `enum`，反序列化时可能冲突。

**（3）决定 `type` 的取值，而这一段包含了整份实现里信息量最大的判断。**

```rust
let mut schema_types = normalized_schema_types(map);

if schema_types.is_empty() && (map.contains_key("$ref") || has_composition_keyword(map)) {
    return;
}

if schema_types.is_empty() {
    if map.contains_key("properties") || map.contains_key("required") || map.contains_key("additionalProperties") {
        schema_types.push(JsonSchemaPrimitiveType::Object);
    } else if map.contains_key("items") || map.contains_key("prefixItems") {
        schema_types.push(JsonSchemaPrimitiveType::Array);
    } else if map.contains_key("enum") || map.contains_key("format") {
        schema_types.push(JsonSchemaPrimitiveType::String);
    } else if map.contains_key("minimum") || map.contains_key("maximum") || map.contains_key("exclusiveMinimum")
        || map.contains_key("exclusiveMaximum") || map.contains_key("multipleOf") {
        schema_types.push(JsonSchemaPrimitiveType::Number);
    } else {
        map.clear();
        return;
    }
}
```

三个分支：

- **有 `$ref` 或有组合关键字，且没有 `type`** → **直接返回，不清洗、不补 `type`**。这是最重要的一条：`$ref` 节点本身不带类型（类型在被引用的定义里），`anyOf` 节点也不该被强行指派一个类型。若在这里补 `{"type": "object"}`，就破坏了引用语义。
- **没有 `type` 也没有 `$ref`/组合关键字** → 按「线索」推断类型，顺序是 object → array → string → number。这套推断规则不是 JSON Schema 标准的一部分，是这里的**启发式**。它按「最能解释现有字段」的顺序试。
- **什么线索都没有** → `map.clear()`，整个 schema 塌成 `{}`。

最后一个分支是全函数里最激进的一处。一个只有 `"description": "..."` 的 schema，或一个只有 `"title"` 的 schema，会被清空成 `{}`。注释在函数文档里写着「Coerces object schemas with no recognized schema hints into `{}`」。`{}` 在 JSON Schema 里含义是「接受任何东西」，所以这也是放宽。

注意 `map.clear()` 会连 `description` 一起清掉——**注释文本一并丢失**。这是这份实现的一个真实代价：一个带丰富描述的 schema 若没有 `type` 与任何结构线索，它的描述会在这一行被抹掉，于是模型看到的是一个无说明的 `{}`。

**（4）把 `type` 写回去，并补齐必需子字段。**

```rust
write_schema_types(map, &schema_types);
ensure_default_children_for_schema_types(map, &schema_types);
```

`write_schema_types` 有三分支：空则删 `type`；单值写字符串；多值写成数组。第三步对应 `type: ["string", "null"]` 这类可空联合。

`ensure_default_children_for_schema_types`：

```rust
if schema_types.contains(&Object) && !map.contains_key("properties") {
    map.insert("properties".to_string(), JsonValue::Object(serde_json::Map::new()));
}
if schema_types.contains(&Array) && !map.contains_key("items") {
    map.insert("items".to_string(), json!({ "type": "string" }));
}
```

即：对象必须有 `properties`（缺则空对象）、数组必须有 `items`（缺则 `{"type": "string"}`）。

这两条是**为了满足下游模型的硬要求**。d04 那篇会看到，MCP 工具解析里也有同类补丁：

> OpenAI models mandate the `properties` field in the schema. Some MCP servers omit it（or set it to null）, so we insert an empty object to match the behavior of the Agents SDK.

即「对象的 schema 必须带 `properties`」不是这个项目的要求，是**上游 API 的要求**。而这个模块替所有调用方统一做了这件事。

而数组默认 `items: {"type": "string"}` 是一个更随意的近似——JSON Schema 里 `items` 缺省意味着「元素类型任意」，这里收窄成字符串。对一个实际装数字的数组，这会给出错误的提示。

**`normalized_schema_types` 只认七种类型名字。**

```rust
match schema_type {
    "string" => String, "number" => Number, "boolean" => Boolean,
    "integer" => Integer, "object" => Object, "array" => Array, "null" => Null,
    _ => None,
}
```

未知的名字被**静默丢弃**（`filter_map`）。所以 `{"type": "date"}` 这种非标准写法会变成「没有类型」，然后走上一段的启发式推断；若也没有别的线索，就被 `map.clear()` 清空。

而 `write_schema_types` 会把结果写回成字符串或数组，所以「丢弃未知类型」产生的「类型集合为空」会变成「删掉 `type` 字段」。这一步让 schema 更宽松（少了约束），但也让下游少了一条信息。

**单例 null 是一个特例错误。**

```rust
fn deserialize_tool_input_schema(input_schema: JsonValue) -> Result<JsonSchema, serde_json::Error> {
    let schema: JsonSchema = serde_json::from_value(input_schema)?;
    if matches!(schema.schema_type, Some(JsonSchemaType::Single(JsonSchemaPrimitiveType::Null))) {
        return Err(singleton_null_schema_error());
    }
    Ok(schema)
}
```

即**反序列化成功之后**再检查一次：若这个 schema 的类型就是「单个 `null`」，报错。

为什么这一条是错误而不是清洗？因为一个「参数必须是 null」的工具意味着它**不接受任何参数**，而工具调用机制总要传点什么。所以这种 schema 无法被有意义地使用——报错比放进上下文更好。

注意检查发生在**反序列化之后**：也就是说 `{"type": "null"}` 能通过 JSON → `JsonSchema` 的转换，是被额外一道守卫拦下的。这暗示内部类型其实允许 `null` 作为类型（`type: ["string", "null"]` 是合法的可空联合），但「只有 null」被禁止。

**`sanitize_schema_table` 与 `DEFINITION_TABLE_KEYS`。**

```rust
const DEFINITION_TABLE_KEYS: [&str; 2] = ["$defs", "definitions"];
```

两个键代表两个世代的写法：`definitions` 是 draft-07 的，`$defs` 是 2019-09 起的。两者都被支持是必要的——MCP 服务器可能用任一版本。

`sanitize_schema_table` 的规则很干脆：

```rust
let should_remove = match map.get_mut(key) {
    Some(JsonValue::Object(definitions)) => { /* 递归清洗每个定义 */ false }
    Some(_) => true,   // 不是对象 -> 整表删掉
    None => false,
};
if should_remove { map.remove(key); }
```

即**definition 表必须是对象；不是对象就整表删除**。函数文档说明了理由：

> Codex keeps valid definition tables and recursively applies the same compatibility lowering used for inline schemas, but drops malformed tables so `strict: false` tool registration degrades gracefully instead of failing on an unreachable or invalid definition table.

展开：一个畸形的 `$defs`（比如写成数组）若保留，反序列化会失败，整个工具注册失败。而若删掉，被引用的定义就「找不到」了——但那只影响具体引用点，工具本身仍能注册。这是一个**用「少一点信息」换「能注册」**的取舍。

**裁剪：可达性分析。**

`prune_unreachable_definitions` 的目标写在注释里：「Prune unused root definition entries to avoid sending tokens for definitions the tool schema never references」。即**为了省 token**——一份大 schema 里可能有几十个定义，实际只用到几个，其余全是发给模型的浪费。

实现是一个标准的图遍历：

```rust
fn collect_reachable_definitions(value: &JsonValue) -> BTreeSet<DefinitionPointer> {
    let mut reachable = BTreeSet::new();
    let mut pending = Vec::new();
    collect_refs_outside_definitions(value, &mut pending);
    while let Some(pointer) = pending.pop() {
        if !reachable.insert(pointer.clone()) { continue; }
        if let Some(definition) = definition_for_pointer(value, &pointer) {
            collect_refs(definition, &mut pending);
        }
    }
    reachable
}
```

三处关键：

1. **起点只收集「定义表之外」的引用**（`collect_refs_outside_definitions`）。理由：定义表内部的互相引用不该让它们全部变成可达——只有被外部真正引用的定义才是入口。若从整棵树的 `$ref` 开始收集，那些「只被别的定义引用」也会可达，裁剪就失效了。
2. **`reachable.insert` 的返回值用来终止循环**：已访问过就 `continue`，避免循环引用导致无限遍历。JSON Schema 允许 `$ref` 形成环（`A` 引用 `B`，`B` 引用 `A`），这一点必须处理。
3. **`BTreeSet`** 让输出有序，便于测试与确定性。

而遍历子节点时用了 `DefinitionTraversal::Skip`：

```rust
for_each_schema_child(map, DefinitionTraversal::Skip, &mut |value| {
    collect_refs_outside_definitions(value, refs);
});
```

`Skip` 意味着**不进入定义表**——这是与第 1 点配套的：外部遍历时跳过 `$defs`/`definitions` 的内容，只扫 inline 部分。

**`parse_local_definition_ref` 只认本地引用。**

```rust
fn parse_local_definition_ref(schema_ref: &str) -> Option<DefinitionPointer> {
    let fragment = schema_ref.strip_prefix('#')?;
    let pointer = urlencoding::decode(fragment).ok()?;
    let pointer = jsonptr::Pointer::parse(pointer.as_ref()).ok()?;
    let (table_token, pointer) = pointer.split_front()?;
    let table = table_token.decoded();
    let table = DEFINITION_TABLE_KEYS.into_iter().find(|candidate| table.as_ref() == *candidate)?;
    let (name, _) = pointer.split_front()?;
    Some(DefinitionPointer { table, name: name.decoded().into_owned() })
}
```

五步：`#` 前缀（本地）→ URL 解码 → JSON Pointer 解析 → 第一段必须是 `$defs` 或 `definitions` → 第二段是定义名。

三处细节：

- **`strip_prefix('#')` 保证只处理本地引用**。`http://example.com/schema#/defs/X` 这类远程引用返回 `None`——因为无法（也不该）在清洗阶段去解析远程 schema。
- **URL 解码**：JSON Pointer 里的定义名可能被百分号编码（`$ref` 是 URI fragment）。
- **允许嵌套指针**，因为代码只取前两段。注释：「Responses API non-strict mode accepts nested local refs such as `#/$defs/User/properties/name`, so keep the parent definition reachable」。即 `#/$defs/User/properties/name` 会让 `User` 这个定义整体可达。这是一个刻意的保守选择：**引用定义的任何一个部位，就把整个定义保留**。

`has_composition_keyword` 是三个键的任一检查：

```rust
const COMPOSITION_SCHEMA_KEYS: [&str; 3] = ["anyOf", "oneOf", "allOf"];
fn has_composition_keyword(map: &serde_json::Map<String, JsonValue>) -> bool {
    COMPOSITION_SCHEMA_KEYS.into_iter().any(|key| map.contains_key(key))
}
```

它在 `sanitize_json_schema` 里只有一个用途：**决定「没有 type 时是否早退」**。有组合关键字就不补 `type`，因为组合关键字的语义是「这些子 schema 之一/全部」，给外层加一个 `type` 会与之冲突。

`SCHEMA_CHILD_KEYS` 虽然在文件里声明了（`["items", "anyOf", "oneOf", "allOf"]`），但 `sanitize_json_schema` 实际逐键处理而非用它——它大概供 `traversal` 模块使用。

## 关键取舍

**源码事实：** 清洗与裁剪的顺序固定：先 `sanitize_json_schema` 让每个节点可表达，再 `prune_unreachable_definitions` 删掉不可达定义。

**源码事实：** 布尔 schema（`true` / `false`）一律塌成 `{"type": "string"}`，即按「接受任何值」那一侧处理，`false` 的语义被改变。

**源码事实：** `const` → 单值 `enum` 是全流程里唯一精确等价的改写。

**源码事实：** 没有 `type` 但有 `$ref` 或组合关键字时直接早退，不补 `type`、不清洗——因为补 `type` 会破坏引用与组合语义。

**源码事实：** 没有 `type` 也没有引用时按线索推断：`properties`/`required`/`additionalProperties` → object；`items`/`prefixItems` → array；`enum`/`format` → string；数值边界关键字 → number；都没有则 `map.clear()` 成 `{}`（连 `description` 一并丢失）。

**源码事实：** 对象补空 `properties`、数组补 `items: {"type": "string"}`——前者是上游 OpenAI API 的硬要求，后者是一个会误伤非字符串数组的近似。

**源码事实：** 只认七个标准类型名，未知类型名静默丢弃导致「无类型」，再走启发式或清空。

**源码事实：** 反序列化成功后再拦「单例 `null` 类型」，因为「参数必须是 null」的工具无法被有意义地调用。

**源码事实：** definition 表必须是对象，不是对象就整表删除，以避免畸形表导致整个工具注册失败。

**源码事实：** 可达性分析从「定义表之外」的引用出发，用 `BTreeSet` 去重以防循环引用，且 `DefinitionTraversal::Skip` 保证外部扫描不进入定义表内部。

**源码事实：** 允许嵌套本地引用（如 `#/$defs/User/properties/name`），只取前两段，因此引用定义的任何部位都会保留整个定义。

**架构推论：** 整套清洗的实质是「**把任意合法 schema 投影到受限类型上，且投影方向统一偏向放宽**」。补 `properties`、塌 `false` 成接受一切、清空无线索对象——每一步都在放宽而非收紧。理由是：放宽的后果是「模型收到的约束比真实更松」，它可能传错参数但有重试机会；收紧的后果是「模型无法表达合法调用」，那是死路。

**架构推论：** 有两条「不该放宽」的地方值得注意，它们揭示了这套机制的边界：一是 `$ref`/组合关键字节点绝不补 `type`（放宽会破坏引用语义），二是「单例 null」直接报错（放宽会得到一个无法调用的工具）。可见「偏向放宽」不是无条件的，它有明确的两处例外。

**架构推论：** 「先清洗再裁剪」这个顺序不只是逻辑上的需要，也影响结果质量。若先裁剪，一个 `$defs` 里的畸形节点会导致裁剪阶段看不到引用（因为解析失败），从而把整个定义删掉；先清洗则畸形节点被塌成可用形状，引用关系还在，裁剪能作对。

**架构推论：** `map.clear()` 连 `description` 一起丢掉，是这一页最具体的代价。描述是给模型看的最重要信息之一（它决定模型是否知道该传什么），而这里为了「让 schema 可表达」把它牺牲了。更细的做法是保留 `description` 并加一个宽松的 `type`，但那需要内部类型支持「无类型但有描述」的节点——目前不支持。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/tools/src/json_schema.rs` | `DEFINITION_TABLE_KEYS` | `$defs` / `definitions` 两代定义表键名 |
| `codex-rs/tools/src/json_schema.rs` | `has_composition_keyword` | 判是否存在 `anyOf`/`oneOf`/`allOf`，决定是否早退 |
| `codex-rs/tools/src/json_schema.rs` | `sanitize_json_schema` | 主体递归清洗：布尔塌陷、`const`→`enum`、类型推断、补子字段 |
| `codex-rs/tools/src/json_schema.rs` | `sanitize_schema_table` | 定义表必须是对象，否则整表删除 |
| `codex-rs/tools/src/json_schema.rs` | `ensure_default_children_for_schema_types` | 补空 `properties` 与默认 `items` |
| `codex-rs/tools/src/json_schema.rs` | `parse_tool_input_schema` | 清洗 + 裁剪 + 压缩 + 反序列化的完整入口 |
| `codex-rs/tools/src/json_schema.rs` | `parse_tool_input_schema_without_compaction` | 可信 schema 的免压缩入口 |
| `codex-rs/tools/src/json_schema.rs` | `prune_unreachable_definitions` | 可达性裁剪以省 token |
| `codex-rs/tools/src/json_schema.rs` | `collect_reachable_definitions` | 从定义表之外的引用出发的图遍历 |
| `codex-rs/tools/src/json_schema.rs` | `parse_local_definition_ref` | 只处理 `#/` 本地引用，支持嵌套指针 |
| `codex-rs/tools/src/json_schema.rs` | `normalized_schema_types` / `write_schema_types` | 七种类型名的规范化与写回 |
| `codex-rs/tools/src/json_schema.rs` | `singleton_null_schema_error` | 单例 null 类型的拒绝 |

## 参考

- [open-interpreter 源码：tools/src/json_schema.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/tools/src/json_schema.rs)
- [JSON Pointer（RFC 6901）](https://www.rfc-editor.org/rfc/rfc6901)
- [MCP 规范：工具定义](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

「把外部系统给的任意结构投影成内部受限类型」与主核里的**查询规范化**结构相同。主核收到一条 SQL 时也要把它投影成内部计划树，而内部类型只能表达它支持的那些操作。差别在失败策略：主核遇到不支持的结构**报错**（因为执行一个语义不同的计划是灾难），这里选择**降级**（因为注册失败等于该工具完全不可用）。判据是「降级的后果是否会造成错误的结果」——schema 降级只影响模型提出的参数形状（模型会看到错误并重试），不产生错误的数据。

「definition 表畸形就整表删掉」这条与主核里**损坏的统计信息被丢弃**同理：主核在读到不一致的统计时会当作「没有统计」并走默认估算，而不是让整个查询失败。两边都是「局部信息不可信时，丢掉局部而不是丢掉整体」。

「偏向放宽而非收紧」这条取舍，与主核里**权限的默认值**方向相反，原因值得对照。主核在权限上是「默认拒绝」（找不到规则就拒绝连接）；这里在 schema 上是「默认接受」（无法表达就放宽）。两者的判据都是「哪一侧的错误更容易被发现与修复」：权限放宽不会报错，只会静默地让不该通行的通过，所以必须默认拒绝；schema 收紧会让工具直接不可用，用户立刻会看到，所以可以接受放宽。

对 AI 工程的实际意义在于：**MCP 生态的 schema 是异构的**，各家服务器按自己的理解生成 `input_schema`，标准允许的写法远多于任一客户端能处理的。这份实现给出的三条经验可直接用：**把降级做成一次集中清洗而不是散在调用点**、**补上游 API 的硬要求（`properties`）在架构层统一完成**、以及**用可达性裁剪省 token**（大 schema 里真正被引用的定义往往只是少数）。
