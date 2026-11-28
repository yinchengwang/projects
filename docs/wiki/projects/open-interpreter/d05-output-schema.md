---
project: open-interpreter
feature: output-schema
title: 工具输出 schema 与存储
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/tools/src/output_schema.rs
    symbols: [ToolOutputSchema, OutputSchemaStorage]
  - path: codex-rs/tools/src/tool_definition.rs
    symbols: [ToolDefinition]
---

# 工具输出 schema 与存储

## 一句话职责

`ToolOutputSchema` 用两种存储形态表达「一个工具的输出 schema」：直接持有一份共享的 JSON，或者只持有 MCP 结构化结果的那一小块、等真正需要时再拼成完整信封。它省的是「每个工具都预先拼一遍信封」的开销。

## 机制是什么

这个类型只有 83 行，但解决的是一个很具体的问题。

MCP 工具的输出 schema 需要拼成信封（见 d04），而信封的骨架部分（`content`、`isError`、`_meta` 三个属性加上 `type`/`required`/`additionalProperties`）**对所有 MCP 工具完全相同**。若每个工具都预先拼一遍：

- 每个工具持有一份结构几乎一样的 JSON 树；
- 图省事还可以共享骨架，但那需要更复杂的设计（骨架的 `Arc` + 每次克隆拼装）。

于是这里选了第三种：**根本不拼**。只存「服务器给的那一小块 `structuredContent`」，需要时再拼。

两种存储形态：

```rust
enum OutputSchemaStorage {
    Json(Arc<Value>),
    McpCallToolResult(Option<Arc<Map<String, Value>>>),
}
```

这样就得到三条性质：

| 形态 | 存什么 | 物化成本 |
|---|---|---|
| `Json` | 完整 schema 的共享引用 | `Arc::clone` |
| `McpCallToolResult(Some(map))` | 只有结构化结果那一块 | 拼一次信封 |
| `McpCallToolResult(None)` | 什么都没有 | 拼一次空信封 |

注意第二个的 `Option` 嵌在变体里，而不是把「有没有结构化结果」做成第三个变体。这个选择有理由——见下面关于 `PartialEq` 的部分。

## 怎么实现的

**两种构造函数，一个来自 `Value`，一个来自 MCP。**

```rust
impl From<Value> for ToolOutputSchema {
    fn from(value: Value) -> Self { Self(OutputSchemaStorage::Json(Arc::new(value))) }
}

impl ToolOutputSchema {
    pub(crate) fn from_mcp_output_schema(structured_content: Option<Arc<Map<String, Value>>>) -> Self {
        Self(OutputSchemaStorage::McpCallToolResult(structured_content))
    }
}
```

`From<Value>` 是公开的（`impl From` 自动给出 `Into`），供非 MCP 工具用。`from_mcp_output_schema` 是 `pub(crate)` 的——即只有 crate 内能用，具体是 d04 那个 `parse_mcp_tool_with_description_limit` 在调。

这个可见性划分说明设计意图：**信封形态是 MCP 的内部细节**，不该被外部直接构造。外部只能给一份完整 JSON，或通过 MCP 解析路径构造。

`from_mcp_output_schema` 的参数类型值得注意：`Option<Arc<Map<String, Value>>>`。接收的是**已经 `Arc` 化的 map**，而不是 `Map` 本身。所以「共享」发生在调用方——d04 传的是 `tool.output_schema.clone()`，而 `tool.output_schema` 本身就是 `Option<Arc<Map>>`（`rmcp` 的类型）。也就是说**这个类型不复制服务器的原始 map**，直接持同一份 `Arc`。

这是整份实现的关键：`McpCallToolResult` 变体里没有 `Map` 的副本，只有 `Arc`。所以构造是零拷贝的。

**`to_value` 是「按需求物化」。**

```rust
pub fn to_value(&self) -> Value {
    match &self.0 {
        OutputSchemaStorage::Json(schema) => schema.as_ref().clone(),
        OutputSchemaStorage::McpCallToolResult(structured_content) => {
            mcp_call_tool_result_output_schema(Value::Object(
                structured_content.as_deref().cloned().unwrap_or_default(),
            ))
        }
    }
}
```

两个分支的成本差异很大：

- **`Json` 变体**：`schema.as_ref().clone()`——**深拷贝整份 JSON**。这是 `to_value(&self)` 无法避免的：它只借到 `&self`，要给出 owned `Value` 就必须克隆。注释在模块文档里点了这一点：「materializes private JSON for consumers」——「private」正是「给你一份自己的副本，改它不影响别人」。
- **MCP 变体**：`structured_content.as_deref().cloned().unwrap_or_default()` 先克隆（或给空 Map），再交给 `mcp_call_tool_result_output_schema` 拼装。注意这里也是克隆，但克隆的是**结构化结果那一小块**，而信封骨架是每次新建的——总成本比克隆整份信封低。

那么 `to_value` 到底省了什么？省的是**存储**，不是取用。即：不使用时（绝大多数时间）只持有一小块；需要时才拼。这与 c01 的 `ConversationHistory` 延迟物化是同一条经济学——**存储与物化分离**。

**`into_value` 则在可能时省掉克隆。**

```rust
pub fn into_value(self) -> Value {
    match self.0 {
        OutputSchemaStorage::Json(schema) => Arc::unwrap_or_clone(schema),
        OutputSchemaStorage::McpCallToolResult(structured_content) => {
            mcp_call_tool_result_output_schema(Value::Object(
                structured_content.map(Arc::unwrap_or_clone).unwrap_or_default(),
            ))
        }
    }
}
```

`Arc::unwrap_or_clone` 是这里最重要的一个 API：**若 `Arc` 的唯一引用（strong count == 1），就取出内部值而不克隆；否则克隆**。语义上等价于 `Arc::try_unwrap(arc).unwrap_or_else(|arc| (*arc).clone())`，但表达更简洁。

于是：

- `Json` 变体：若这个 `ToolOutputSchema` 是最后一份引用（既没有别处 clone 过 `ToolOutputSchema`，也没有别的地方持 `Arc`），就**直接移出 JSON**，零克隆。
- MCP 变体：同理，但只省掉那块 `Map` 的克隆；信封骨架永远要新建。

这两个方法并存的设计很清楚：**`to_value` 给「我要看一下」，`into_value` 给「我要拿走」**。前者只能用克隆，后者在所有权允许时能省。

而 `unwrap_or_clone` 的语义还揭示了一个隐含的事实：`Arc` 的共享可能来自**别处也持有同一份** `Arc`（不只是 `ToolOutputSchema` 的多个 clone）。例如 d04 里 `tool.output_schema` 是 `rmcp` 给的，若那个 `Tool` 还活着，`Arc` 的计数就不止一，`unwrap_or_clone` 会退回克隆。这是正确的行为——不能从别人手里抢数据。

**`PartialEq` 的手写实现是这份代码里最长的一段，也是最值得读的。**

```rust
impl PartialEq for ToolOutputSchema {
    fn eq(&self, other: &Self) -> bool {
        match (&self.0, &other.0) {
            (Json(left), Json(right)) => left == right,
            (McpCallToolResult(left), McpCallToolResult(right)) => match (left, right) {
                (Some(left), Some(right)) => left == right,
                (None, None) => true,
                (Some(schema), None) | (None, Some(schema)) => schema.is_empty(),
            },
            (Json(left), McpCallToolResult(_)) => left.as_ref() == &other.to_value(),
            (McpCallToolResult(_), Json(right)) => &self.to_value() == right.as_ref(),
        }
    }
}
```

四个分支：

1. **同态 `Json` vs `Json`**：比 `Arc<Value>` 的内容（`left == right` 比较的是 `Arc<Value>`，`Arc` 的 `PartialEq` 委托给内部值）。不等 `Arc::ptr_eq`——**语义相等而非引用相等**。
2. **同态 MCP vs MCP**：`Some/Some` 比 map 内容；`None/None` 为真。
3. **`Some(schema)` vs `None` → 判 `schema.is_empty()`**。这一支是整段里最有意思的：**空 map 与「没有 map」被视为相等**。理由：`mcp_call_tool_result_output_schema` 对两者产生同一个结果——`Value::Object(structured_content.as_deref().cloned().unwrap_or_default())`，`None` 走 `unwrap_or_default()` 得到空 Map，`Some(空 Map)` 也是空 Map。所以「物化出来的 JSON 相同」这一语义下，两者确实等价。

   这正是前面提到的「为什么 `Option` 嵌在变体里而不做成第三个变体」的理由：**把 `None` 与空 Map 做成两个变体会需要额外一条等价规则，而现在它们天然在同一个变体的两个取值上**。

4. **异构：`Json` vs MCP** → 把 MCP 那侧物化成 JSON 再比（反之亦然）。

   这一支有成本：每次比较都要**物化一次信封**（含克隆）。所以跨形态比较是 O(schema 大小) 的深比较，而不是 O(1) 的结构比较。这个选择是为了**语义正确**：一个工具的输出 schema 无论是「直接给的 JSON」还是「MCP 信封 + 结构化块」形式构造的，只要物化出来相同，就该被视为相等。

为什么这段要手写而不是 `derive(PartialEq)`？因为派生会按「变体是否相同 + 字段逐个比较」判等，于是「用 `Json` 构造的信封」与「用 MCP 构造的同一个信封」会被判为**不等**——而这在语义上是错的。手写才能实现「跨形态按物化结果比」。

这个取舍的代价也很清楚：**`PartialEq` 不再是纯结构比较，而是会构造 JSON 的比较**。若在热路径上比较大量 schema，这个成本不可忽略。不过 `PartialEq` 在这类场景（缓存键、快照测试、断言）里通常只出现一次，所以可以接受。

**`Clone` 是派生的，而 `Debug` 也是。**

```rust
#[derive(Debug, Clone)]
pub struct ToolOutputSchema(OutputSchemaStorage);
```

`OutputSchemaStorage` 也需要 `Debug, Clone`，它的两个变体都能派生：`Arc<Value>` 与 `Option<Arc<Map>>` 都支持。所以 `clone` 是**引用计数加一**，不是深拷贝——这一点很重要：`ToolOutputSchema` 的克隆成本是 O(1)，物化成本才与 schema 大小相关。

**元组结构体 `ToolOutputSchema(OutputSchemaStorage)` 而不是「直接 enum」。** 用一个 tuple struct 包住 enum，好处是：

- `ToolOutputSchema` 有一个**不透明的**单字段，外部无法直接匹配到 `OutputSchemaStorage`（后者是私有 enum，但若直接做成 `pub struct ToolOutputSchema { storage: ... }` 也一样）。
- 用 tuple struct 让内部字段无名，配合方法（`to_value`/`into_value`）形成完整封装。

`OutputSchemaStorage` 是私有的（没有 `pub`），所以外部只能通过 `From<Value>` 与 MCP 解析路径构造。这个封装程度与前面「信封形态是 MCP 内部细节」的设计意图一致。

**`into_value` 的名字选择值得记。** Rust 命名惯例里 `into_*` 表示消耗 `self`，而 `to_*` 表示借 `&self`。这里严格遵循了，且**语义上的差异恰好就是「能不能省克隆」**——于是命名不仅是所有权约定，还预示了性能差异。调用方看到 `into_value` 就知道「我这里可以放弃这个 schema」。

**`ToolDefinition` 是持有这个 schema 的容器，而它用 `Option` 表达「有没有输出 schema」。**

```rust
pub struct ToolDefinition {
    pub name: String,
    pub description: String,
    pub input_schema: JsonSchema,
    pub output_schema: Option<ToolOutputSchema>,
    pub defer_loading: bool,
}
```

这个五字段结构回答了「为什么 `ToolOutputSchema` 要为物化成本操心」：**每一个工具都持有一个**。输入 schema 只有一份形态（`JsonSchema`，d02 清洗后的类型），而输出 schema 有两种形态（本页的 `OutputSchemaStorage`）且很多工具根本没有（`Option`）。于是工具数量乘以每份 schema 的体积，就是常驻内存的规模——这正是延迟物化的动机所在。

三处设计值得看：

1. **`input_schema` 是裸值 `JsonSchema`，`output_schema` 是 `Option<ToolOutputSchema>`**。不对称的理由不在「输入必输出可」这种表面解释，而在**物化时机**：输入 schema 每次工具调用都要用（模型要据此生成参数），所以必须已物化；输出 schema 只在需要校验返回值时才用，所以值得延迟。这个差异精确地反映在类型上——一个是不透明的具体类型，一个是被封装的存储形态。

2. **`into_deferred` 把两个字段一起改**：

```rust
pub fn into_deferred(mut self) -> Self {
    self.output_schema = None;
    self.defer_loading = true;
    self
}
```

即「延迟加载 = 丢掉输出 schema + 置标记」。这两件事必须同时发生：一个 `defer_loading: true` 但仍带 `output_schema` 的定义是自相矛盾的（既然延后注册，为什么还准备校验？），而一个 `defer_loading: false` 却没有 `output_schema` 的定义则意味着工具不校验返回值。

   这个一变二的耦合是这份代码里的一处隐患：两个字段表达同一个事实的两个侧面。更紧的写法是把它们合成一个带载荷的枚举（`Deferred` 无 schema / `Eager(OutputSchema)`），让不一致在类型层面不可能发生。d04 那篇提到的 `defer_loading: false` 与 `output_schema: Some(...)` 的自洽配对，正是依赖这条隐含约定才能成立。

3. **`renamed` 只改名字，不动 schema**：

```rust
pub fn renamed(mut self, name: String) -> Self {
    self.name = name;
    self
}
```

   这是给「同一个工具在不同 harness 下用不同名字」准备的（a04 讲的 harness 复刻：Claude Code 与 Kimi CLI 对同一能力的工具名不同）。注意它**不重建 schema**——因为 schema 与名字无关，改名不影响参数形状。这个「只改能改的」的做法避免了不必要的物化。

**这个容器与 `ToolOutputSchema` 的职责分界很干净。** `ToolDefinition` 负责「一个工具的元数据是什么」，`ToolOutputSchema` 负责「这份 schema 怎么存最省」。前者不关心物化成本，后者不知道自己是某个工具的一部分。这类分界让 d04 的 MCP 解析可以只做「填字段」，把存储策略完全交给本页的类型。

## 关键取舍

**源码事实：** 两种存储形态：`Json(Arc<Value>)` 存完整 schema，`McpCallToolResult(Option<Arc<Map>>)` 只存 MCP 结构化结果那一小块，信封在物化时才拼。

**源码事实：** 构造是零拷贝的——`from_mcp_output_schema` 接收已经 `Arc` 化的 map 并直接持有，不复制。

**源码事实：** `to_value(&self)` 必须克隆（只借到 `&self`），`into_value(self)` 用 `Arc::unwrap_or_clone` 在唯一引用时零克隆。两个方法并存的唯一理由就是这一个差异。

**源码事实：** `PartialEq` 手写而非派生，因为要支持跨形态比较：把 MCP 侧物化成 JSON 与 `Json` 侧比较，语义是「物化结果相同即相等」。

**源码事实：** `McpCallToolResult(Some(空 map))` 与 `McpCallToolResult(None)` 被判相等，因为两者物化出的信封 JSON 完全相同。

**源码事实：** `from_mcp_output_schema` 是 `pub(crate)`，`From<Value>` 是公开的——信封形态属于 MCP 解析的内部细节，外部只能给完整 JSON。

**架构推论：** 这个类型解决的问题是「同一个常量骨架的重建成本」。若每个 MCP 工具都预先拼信封，就产生 N 份结构几乎相同的 JSON 树；若共享骨架又需要拼接机制。选择「根本不拼」是最省的——代价是每次物化多一步拼装，但物化本身不频繁。

**架构推论：** 「`Option` 嵌在变体里」这个细节体现了「**把等价性提前到结构上**」的思路。若把 `None` 做成独立变体，`PartialEq` 就要多写一条「`None` 变体与 `Some(空 map)` 相等」的规则，而且这条规则的理由（物化结果相同）需要靠注释传达。现在这条等价性天然落在同一个分支里，只需判 `schema.is_empty()`。

**架构推论：** `PartialEq` 是「语义等价」而不是「结构等价」，这类实现的代价总是「比较可能比表面上贵」。这里的成本是 O(schema 大小) 的深比较加一次物化。判断是否值得的判据是**比较的频率**：如果 schema 比较只出现在断言与缓存键（各一次），就完全值得；如果它在每次工具调用都比一次，就该改成预计算一个规范化形式。

**架构推论：** 一处值得指出的不对称：`to_value` 对 `Json` 变体克隆整份 JSON，而对 MCP 变体克隆小块。也就是说**同一个 API 的两次调用成本可以差一个量级**，取决于内部形态。外部调用方无法从签名看出这一点。若要更可预测，可以让 `to_value` 也返回 `Arc<Value>`（即 `Json` 变体直接返回 `Arc::clone`），把「要不要深拷贝」的决定权交给调用方。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/tools/src/output_schema.rs` | `ToolOutputSchema` | 单字段封装，对外只暴露构造与两种物化 |
| `codex-rs/tools/src/output_schema.rs` | `OutputSchemaStorage` | 私有 enum：完整 JSON 或 MCP 结构化块 |
| `codex-rs/tools/src/output_schema.rs` | `ToolOutputSchema::from_mcp_output_schema` | `pub(crate)` 构造，零拷贝持有 MCP 的 `Arc<Map>` |
| `codex-rs/tools/src/output_schema.rs` | `ToolOutputSchema::to_value` | 借 `&self` 物化，必然克隆 |
| `codex-rs/tools/src/output_schema.rs` | `ToolOutputSchema::into_value` | 消耗 `self`，唯一引用时用 `Arc::unwrap_or_clone` 零克隆 |
| `codex-rs/tools/src/output_schema.rs` | `ToolOutputSchema::eq` | 手写语义等价，支持跨形态比较 |
| `codex-rs/tools/src/output_schema.rs` | `impl From<Value> for ToolOutputSchema` | 外部唯一公开构造方式 |

## 参考

- [open-interpreter 源码：tools/src/output_schema.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/tools/src/output_schema.rs)
- [Rust 文档：Arc::unwrap_or_clone](https://doc.rust-lang.org/std/sync/struct.Arc.html#method.unwrap_or_clone)
- [Rust 文档：实现 PartialEq](https://doc.rust-lang.org/std/cmp/trait.PartialEq.html)
- [MCP 规范：结构化工具结果](https://modelcontextprotocol.io/specification/2025-06-18/server/tools#structured-content)

## 与主核 / AI 的关系

「共享不可变数据 + 按需求物化」与主核里的**页面共享与写时复制**同源：主核的多个读者持同一份页，只有在要修改时才复制。差别在于主核的物化是「把页解成元组格式」，而这里是「把结构化块拼成信封」——都是「内部表示更紧凑，对外表示更完备」。

「`Arc::unwrap_or_clone` 在唯一引用时省掉复制」这条与主核里**引用计数为一时可原地修改**的优化同理：这需要知道「有没有别人在看这份数据」。判据相同——**唯一所有权时才能安全地改动（或移出）**。

「语义等价而非结构等价」的 `PartialEq`，在主核里对应的是**比较两个计划是否等价**：主核不会比较两个计划树的指针或文本，而是比较其规范化形式。本页的做法（把 MCP 侧物化后比较）与「把计划规范化后比较」是同一条路，代价也相同——每次比较都要做一次转换。

对 AI 工程的实际意义在于：工具定义会随 MCP 服务器数量线性增长，而其中很大一部分是重复的骨架。这份实现给出的三条经验可直接用：**把共享的常量部分延迟构造而不是预先展开**、**把「借出副本」与「移出所有权」拆成两个方法**（让调用方能选择性能），以及**手写等价性以支持跨表示比较**（否则「同一份 schema 用两种方式构造」会被判为不等，导致缓存与断言失效）。
