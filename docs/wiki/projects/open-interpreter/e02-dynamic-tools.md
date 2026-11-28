---
project: open-interpreter
feature: dynamic-tools
title: 动态工具规格
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/protocol/src/dynamic_tools.rs
    symbols: [DynamicToolSpec, DynamicToolNamespaceSpec, normalize_dynamic_tool_specs]
  - path: codex-rs/protocol/src/dynamic_tools.rs
    symbols: [DynamicToolCallRequest, DynamicToolResponse, LegacyDynamicToolSpec]
---

# 动态工具规格

## 一句话职责

让上层（app-server 的客户端）在**会话中途**给 agent 下发新的工具：工具规格可以带命名空间分组，调用由客户端而不是 agent 执行；而这个模块同时负责把**两种历史线格式归一成一种**，并拒绝「两种格式混用」的文件。

## 机制是什么

175 行，四组东西：

| 组 | 内容 |
|---|---|
| 下发规格 | `DynamicToolSpec::{Function, Namespace}`；`DynamicToolFunctionSpec`、`DynamicToolNamespaceSpec`、`DynamicToolNamespaceTool` |
| 调用回执 | `DynamicToolCallRequest`（谁调什么、什么参数）与 `DynamicToolResponse`（结果内容块 + 成功位） |
| 内容块 | `DynamicToolCallOutputContentItem::{InputText, InputImage, InputAudio}` |
| 归一 | `normalize_dynamic_tool_specs`、`group_dynamic_tools_by_namespace`、`deserialize_dynamic_tool_specs` |

「动态」的含义是具体的：这些工具**不出现在系统提示的工具清单里、也不由 agent 的运行时执行**——agent 只负责发出调用请求，真正的执行在客户端那一侧。

## 怎么实现的

**`DynamicToolSpec` 的两个变体对应「平铺」与「成组」两种工具形态。**

```
Function(DynamicToolFunctionSpec)   →  type: "function"
Namespace(DynamicToolNamespaceSpec) →  type: "namespace"
```

`DynamicToolFunctionSpec` 的字段是 `name`、`description`、`input_schema`（一个裸 `JsonValue`，不做 schema 校验）、以及 `defer_loading`。

`defer_loading` 的 serde 声明是本页第一个值得看的细节：

```
#[serde(default, skip_serializing_if = "std::ops::Not::not")]
pub defer_loading: bool,
```

也就是说**这个布尔默认 `false`，且值为 `false` 时不出现在线格式里**。这正是那个项目 `AGENTS.md` 里明文写着的约定：当「省略即意味着 `false`」时，用这个组合而不是 `Option<bool>`。

收益是线格式干净（绝大多数工具不延迟加载，于是绝大多数条目里没有这个字段），且**语义是确定的**：缺字段 = 不延迟。

**`DynamicToolNamespaceSpec` 带一个 `tools: Vec<DynamicToolNamespaceTool>`，而后者只有一个变体 `Function`。** 单变体枚举在这里不是留位，而是一个**判别标签的载体**：分组里的每个工具在线格式上也带 `type` 字段，与顶层的 `DynamicToolSpec` 形态一致。这样消费方递归处理时不必区分「顶层」与「组内」。

**`DynamicToolNamespaceSpec` 的 `description` 在归一化时会被填成空串。** 看 `group_dynamic_tools_by_namespace` 里构造它那段：`description: String::new()`。也就是说 legacy 格式的扁平条目没有「命名空间描述」这个概念，所以重新分组时只能给空串。

这个空串是这一页最值得注意的一处**信息损失**：从 legacy 形状归一出来的命名空间永远没有描述。若上层需要描述，就必须在 canonical 格式里显式提供。

**`DynamicToolCallRequest` 有五个必填 + 两个可选。** `call_id`、`turn_id`、`tool`、`arguments` 必填；`started_at_ms`（`#[serde(default)]` 的 `i64`）与 `namespace: Option<String>` 可选。

`namespace` 可选说明**平铺工具与分组工具的调用请求是同一个结构**：分组工具只是多填一个命名空间名。`turn_id` 的存在让一次调用能与具体回合对应——这是「会话中途下发」这个前提下必要的，因为同一套工具在不同回合里可能被换掉。

**`DynamicToolResponse` 的形状是「内容块列表 + 成功位」。** `content_items: Vec<DynamicToolCallOutputContentItem>` 与 `success: bool`。

注意它**不是** `Result`：失败也走同一个结构，靠 `success` 区分。而内容块三种（文本、图片、音频）说明这个通道不只送文本——客户端可以回一张图或一段音频。

`success: bool` 而非 `Option<error>` 的选择是有代价的：**失败时没有结构化的错误字段**，只能在 `content_items` 里放一段文本。这与 `TurnItem` 里 `FunctionCallOutputItem` 用 `FunctionCallOutputBody`（能表达成功/失败两种）不同——两处的建模精度不一致。

### 归一：三态判定

**`normalize_dynamic_tool_specs` 是这一页的核心，它要处理三种输入：全 canonical、全 legacy、以及混用。**

判定靠两个辅助闭包。第一个判「单个值是不是 legacy」：

```
has_legacy_fields = value.get("namespace").is_some()
                 || value.get("exposeToContext").is_some()
                 || value.get("type").is_none()
```

三个条件任一成立就算 legacy。注意第三项：**没有 `type` 字段就算 legacy**——因为 canonical 格式的两个变体都带 `tag = "type"`，缺它就不可能是 canonical。

第二个判「整批里有没有 legacy」，且**递归看一层**：

```
has_legacy_format = values.iter().any(|v|
    has_legacy_fields(v)
    || v.get("tools").as_array().is_some_and(|tools| tools.iter().any(&has_legacy_fields)))
```

也就是说一个 canonical 形状的 `Namespace` 条目，若它**组内的**工具是 legacy 形状，整批也算 legacy。这是必要的：`Namespace` 条目的外壳可以是 canonical（有 `type`），而内部工具仍是老写法。

**混用被显式拒绝，而不是「容忍并逐个判断」。** 判定是：

```
if has_legacy_format && has_canonical_format { return Err(custom("dynamic tools must use either canonical or legacy format consistently")) }
```

而 `has_canonical_format` 的定义只是 `values.iter().any(|v| v.get("type").is_some())`——**任何一个条目有 `type` 就算有 canonical 格式**。

于是「全 legacy」时 `has_canonical_format` 为假（legacy 条目没有 `type`），「全 canonical」时 `has_legacy_format` 为假。而「一半 legacy 一半 canonical」两种情况都真，直接报错。

**为什么拒绝混用而不是逐条归一？** 因为归一化的产物结构不同：legacy 走「解成扁平条目再按 namespace 重组」，canonical 走「直接反序列化」。若混用，两条路径各自的假设都会被破坏——而错误消息说「必须一致地使用」，把决定权交回给调用方。

这个取舍的代价很明确：一个客户端不能「渐进迁移」（先给新工具用 canonical、老工具保持 legacy）。收益是归一逻辑简单且**不会产出半对半错的结果**。

**legacy 路径的重组是「收集 (namespace, function) 对，再按 namespace 归组」。** 三条：

1. 每个 legacy 条目先转成 `DynamicToolFunctionSpec`，其中 `defer_loading` 有三级来源：

```
tool.defer_loading.unwrap_or_else(|| tool.expose_to_context.map(|visible| !visible).unwrap_or(false))
```

也就是**优先用新字段 `defer_loading`，没有就用老字段 `exposeToContext` 取反，两者都没有则 `false`**。

**取反这一步是这次归一化最关键的一次语义换算**：老字段叫「暴露给上下文」（`true` = 模型能看到），新字段叫「延迟加载」（`true` = 不在初始清单里）。两者方向相反，所以必须取反。而 `unwrap_or(false)` 的兜底意味着「两个字段都没写」→ 不延迟 → 可见，与老格式的默认一致。

2. 得到 `(Option<String>, DynamicToolFunctionSpec)`，namespace 为 `None` 就是平铺工具。

3. `group_dynamic_tools_by_namespace` 按 namespace 归组，用一个 `HashMap<String, usize>` 记住「这个 namespace 在结果里的下标」，于是后续同名工具被 push 进已有的 `Namespace` 条目的 `tools` 里。

**归组实现里那个 `unreachable!` 值得看一眼。** 代码是：

```
let DynamicToolSpec::Namespace(namespace) = &mut grouped_tools[index] else {
    unreachable!("namespace index must point to a namespace");
};
```

这是一个**用 `unreachable!` 表达不变量**的做法：`namespace_indices` 里存的下标只会在「插入了一个 `Namespace`」时写入，所以取出来必然是 `Namespace`。而 `let ... else` 加 `unreachable!` 让编译器不必假定，也让读代码的人一眼看到「这里的不变量是什么」。

**平铺工具不经归组，直接 push。** `let Some(namespace) = namespace else { grouped_tools.push(DynamicToolSpec::Function(function)); continue; }`——于是 legacy 里没有 namespace 的工具保持平铺形态，而那些有 namespace 的会被合并成组。

**这条不对称有一个可观察的后果：legacy 输入里的「组」在输出里会出现，即使原输入根本没有组的概念。** 也就是说归一化不是无损的——它把代码的语义（`namespace` 字段）提升成了结构（`Namespace` 变体）。

**`deserialize_dynamic_tool_specs` 是一个自定义反序列化器，它做了「先读成 `Vec<JsonValue>` 再归一」。** 实现是：

```
let Some(values) = Option::<Vec<JsonValue>>::deserialize(deserializer)? else { return Ok(None) };
normalize_dynamic_tool_specs(values).map(Some).map_err(D::Error::custom)
```

`Option::<Vec<JsonValue>>` 让「字段缺失」与「字段是 null」都返回 `None`，与 `Some(空 vec)` 区分。而这里**故意先反序列化成 `JsonValue` 而不是直接解析成 `DynamicToolSpec`**——因为只有拿到原始结构才能做「legacy 还是 canonical」的判定。

这个两步结构是「同一份线数据有两种历史形状」这类问题的标准解法：**先到无类型表示，判定形状，再到类型表示**。直接 `#[derive(Deserialize)]` 一个枚举做不到，因为 serde 的判别标签只认一种形状。

## 关键取舍

**源码事实：** `DynamicToolSpec` 两个变体（`Function` / `Namespace`），组内工具也用同类判别标签，于是递归处理时不必区分顶层与组内。

**源码事实：** `defer_loading` 用 `default` + `skip_serializing_if = "std::ops::Not::not"` 的布尔，省略即 `false`，不用 `Option<bool>`。

**源码事实：** 归一化对 legacy 的 `exposeToContext` 做**取反**映射到 `defer_loading`：老字段是「可见」、新字段是「延迟」，方向相反。两者都缺时兜底 `false`（即不延迟、可见），与老格式默认一致。

**源码事实：** 归一化判 legacy 的三个条件之三是「没有 `type` 字段」，且判定**递归看 `Namespace` 条目的组内工具一层**。

**源码事实：** 混用两种格式被直接报错，不做逐条归一——因为两条归一路径的假设互不兼容。代价是客户端无法渐进迁移。

**源码事实：** legacy 归一后重新分组时，`Namespace` 的描述被填成空串——这是不可恢复的信息损失，因为 legacy 扁平条目没有命名空间描述这个概念。

**源码事实：** `group_dynamic_tools_by_namespace` 用 `HashMap<String, usize>` 记下标，并用 `let ... else unreachable!` 表达「这个下标必然指向 Namespace」的不变量。

**源码事实：** `DynamicToolResponse` 用 `success: bool` 而不是结果类型，失败只能在内容块里放文本——与 `FunctionCallOutputBody` 的结构化程度不同。

**源码事实：** `deserialize_dynamic_tool_specs` 两步走：先 `Vec<JsonValue>` 再判定形状再转类型，因为 serde 的判别标签只认一种形状。

**架构推论：** 这一页示范了**「协议演进怎么落地」**的一个具体答案：老格式不删除、不并存两套解析器，而是**在入口处一次归一到唯一内部表示**。归一化是单向的（legacy → canonical），且归一之后所有下游只面对一种形状。

代价也很清楚：归一化会丢信息（命名空间描述），且「不许混用」这条约束会把一部分迁移成本推给客户端。但换来的是**下游代码里没有第二个形状的分支**——这是长期收益更高的那一侧。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/protocol/src/dynamic_tools.rs` | `DynamicToolSpec` | 下发规格的判别联合：平铺 / 命名空间 |
| `codex-rs/protocol/src/dynamic_tools.rs` | `DynamicToolFunctionSpec` | 单个工具：名字、描述、裸 schema、`defer_loading` |
| `codex-rs/protocol/src/dynamic_tools.rs` | `DynamicToolNamespaceSpec` | 工具分组：名字、描述、组内工具 |
| `codex-rs/protocol/src/dynamic_tools.rs` | `DynamicToolNamespaceTool` | 组内工具的单变体判别标签 |
| `codex-rs/protocol/src/dynamic_tools.rs` | `DynamicToolCallRequest` | 调用请求：call/turn id、命名空间、工具、参数 |
| `codex-rs/protocol/src/dynamic_tools.rs` | `DynamicToolResponse` | 回执：内容块列表 + `success` |
| `codex-rs/protocol/src/dynamic_tools.rs` | `DynamicToolCallOutputContentItem` | 三种回执内容：文本 / 图片 / 音频 |
| `codex-rs/protocol/src/dynamic_tools.rs` | `LegacyDynamicToolSpec` | 只反序列化的老扁平形状，含 `exposeToContext` |
| `codex-rs/protocol/src/dynamic_tools.rs` | `normalize_dynamic_tool_specs` | 三态判定 + legacy 归一，混用即报错 |
| `codex-rs/protocol/src/dynamic_tools.rs` | `group_dynamic_tools_by_namespace` | 按命名空间归组，用下标表合并同名组 |
| `codex-rs/protocol/src/dynamic_tools.rs` | `deserialize_dynamic_tool_specs` | 两步反序列化器，供 serde 字段使用 |

## 参考

- [open-interpreter 源码仓库](https://github.com/OpenInterpreter/open-interpreter)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [serde 文档：为字段指定自定义反序列化函数](https://serde.rs/field-attrs.html#deserialize_with)
- [MCP 规范：工具定义](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

这套「入口归一」与主核里「协议版本降级到统一内部格式」是同构的：数据库允许老客户端用老协议连接，服务端在解析阶段就把它转成内部算子格式，之后的执行路径只有一条。两者的共同判断是**接收端负责归一，执行端只认一种形状**。

对 agent 工程的直接价值有三条。其一，**字段语义翻转时必须显式取反，并写清兜底方向**。`exposeToContext` → `defer_loading` 是「可见」变成「延迟」，取反漏掉就会让工具可见性整体反过来——而这类错误在测试里只表现为「工具没出现」，很难定位。其二，**混用格式要报错而不是尽力解释**。半对半错的归一结果比一个明确错误糟糕得多，因为前者会让调用方以为迁移成功了。其三，**归一化的信息损失要能说清**。命名空间描述被填成空串这件事，如果没人知道，上层就会困惑「为什么我设的描述没了」。
