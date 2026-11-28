---
project: goose
feature: extension-runtime
title: 扩展运行时与工具可见性
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/agents/extension.rs
    symbols: [ToolInfo]
  - path: crates/goose/src/agents/extension_config.rs
    symbols: [ExtensionConfig, CLIENT_SECRET_PREFIX]
---

# 扩展运行时与工具可见性

## 一句话职责

把「一个扩展是什么」写成三态枚举（内置 / 平台 / 外部传输），再给每个扩展配一份**工具可见性白名单**——于是同一个工具集合可以根据会话需要被切成不同的子集，而 `ToolInfo` 是这套机制对外汇报的统一形状。

## 机制是什么

两个文件分工明确：

| 文件 | 内容 |
|---|---|
| `extension_config.rs` | `ExtensionConfig` 三态枚举、`ToolInfo` 之外的全部配置逻辑、**进程内密钥暂存**、`check_unique_keys` |
| `extension.rs` | `ToolInfo`（工具描述的统一形状）+ 大量反序列化测试；其余是对 `extension_config` 与 `platform_extensions` 的重导出 |

`ExtensionConfig` 的三个变体：

| 变体 | 语义 | 谁提供工具 |
|---|---|---|
| `Builtin { name, description, display_name, timeout, bundled, available_tools }` | **打包在 goose 自带 MCP 服务器里的内置扩展** | 内置服务器 |
| `Platform { name, description, display_name, bundled, available_tools }` | **平台扩展**：直接访问 agent 等，**跑在 agent 进程里** | 进程内 |
| `External(TransportConfig)` | 来自 oi-kit 的外部传输（stdio / streamable_http） | 独立进程或远端 |

## 怎么实现的

**「保持传输变体可直接匹配」是那个 `#[allow(clippy::large_enum_variant)]` 的理由。** 代码注释：

> 保持传输变体可直接匹配，像 oi-kit 的传输枚举那样；在这里装箱会需要第二个 match 只为了检视传输字段。

也就是说这是一次**明确的 clippy 例外**，理由不是「懒得改」，而是「装箱会让每处读传输字段的地方多一层解包」。取舍很清楚：用一点栈空间换来调用点的简洁。

这种例外值得记，因为 `#[allow(...)]` 通常被当作技术债的标志；这里它带着理由，并且理由是可以被反驳的（如果枚举变得更大，就值得装箱）。

**`External` 用 `#[serde(untagged)]`，而前两个变体用 `#[serde(tag = "type")]`。** 这个组合是刻意的：

- `Builtin` / `Platform` 有线上的 `type` 判别字段（`"builtin"` / `"platform"`），所以由外层 `tag = "type"` 处理；
- `External` 内部已经带自己的标签（`TransportConfig` 是「直接序列化 oi-kit 配置，保留已有的 tagged JSON」——注释原话），所以外层不能再加标签，只能 `untagged`。

`untagged` 的代价是**反序列化要靠试**：serde 会依次尝试每个变体，第一个成功的胜出。这在这里可接受，因为 `External` 是最后一个变体（前面两个有明确 tag，不会与它竞争），而 `External` 内部有自己的 tag 校验。

**`description` 字段有一个专门的反序列化器，因为 YAML 里 `description:` 空的会解析成 `null`。** `deserialize_null_with_default` 的实现是 `Ok(Option::deserialize(deserializer)?.unwrap_or_default())`——把 `null` 变成 `T::default()`。

而 `extension.rs` 的测试里有两个几乎相同的用例（`test_deserialize_missing_description` 与 `test_deserialize_null_description`）专门验证这条：**字段缺失与字段为 null 都得到空串**。这两个用例的存在说明这是被真实配置撞到过的问题。

这个模式的价值在于：**配置文件的「没写」与「写了空」在语义上应当一致**，而 YAML 的 `key:` 写法恰好会产出 `null`。不处理的话，一个手写的扩展配置会因为一个空 `description` 而整体解析失败。

**`available_tools` 的三重属性各自有用。** 声明是：

```rust
#[serde(default)]
#[serde(skip_serializing_if = "Vec::is_empty")]
available_tools: Vec<String>,
```

- `default` 让「不写」等于空列表；
- `skip_serializing_if` 让空列表不出现在序列化输出里；
- 而测试里有两个用例分别验证这两条（`serialization_omits_empty_available_tools` 与 `serialization_preserves_available_tools`）。

「空列表不写出去」不只是省字节：它让**写出去的配置与手写的配置形状一致**——一个默认扩展的 YAML 里不会突然多出一个空数组，也就不会让人误以为那里有什么需要配置的东西。

**工具可见性的判定是「空集即全开」。** `is_tool_available(name)` 对 `Builtin` / `Platform` 的实现是：

```
available_tools.is_empty() || available_tools.iter().any(|tool| tool == name)
```

也就是说 `available_tools: []` 意味着**看见全部工具**，而不是「一个都不看见」。这是一个必须写进文档的默认值取向——因为「空列表 = 全开」与「空列表 = 全关」都有人会这样实现，而两者对用户的影响完全相反。

选择「全开」的理由可以从字段的存在目的推出：它叫 `available_tools` 而不是 `blocked_tools`，即**限制项**；没有限制就是不限制。

**`External` 变体把可见性判定委托给 oi-kit。** `config.is_tool_available(name)`——也就是说白名单语义在传输层那一侧也有定义，这里只是转发。而 `ExtensionConfig` 自身的方法（`name`、`key`、`is_tool_available`、`resolve`、`external`）都是**对三种变体的统一门面**：`External` 转发给 oi-kit，另外两个直接读字段。

**`resolve` 只有 `External` 需要做事。** `ExtensionConfig::resolve(self, resolver)` 对 `External` 调 `config.resolve(resolver)`（解析环境变量占位等），对 `Builtin` / `Platform` 原样返回。

这反映了一个差异：外部传输的配置里可能有 `envs` 这类需要运行时展开的东西（`Envs` 类型与 `ExtensionConfigResolver` 都从 oi-kit 重导出），而内置/平台扩展的配置是纯字面量。

**`name_to_key` 让「名字」与「键」分离，而唯一性检查用的是键。** `check_unique_keys` 遍历配置，用 `config.key()` 插入 `HashSet`，重复就报错：`extension '{name}' appears twice in the selection`。

也就是说 `id` 层面的唯一性用规范化后的 key，而报错文案用原始 name——**用 key 判重（防止两个拼写变体绕过检查），用 name 提示（用户认得的那个）**。这是一个正确的分工。

### 进程内密钥暂存

**`CLIENT_SECRET_PREFIX` 与那两个函数是这一页最有意思的机制。** 常量是 `"GOOSE_CLIENT_SECRET_"`（前缀），而 `hold_client_secret` 把真实密钥存进一个进程级 `LazyLock<Mutex<HashMap<String, String>>>`，返回一个形如 `GOOSE_CLIENT_SECRET_<uuid>` 的**键**。

`client_secret(key)` 反向查找：若键不以该前缀开头就返回 `None`（不管），否则从表里取，取不到就返回一个带说明的错误：

> secret '{key}' was sent by an ACP client and is only held in memory by the goose process it was sent to; send this MCP server again when loading the session

**这套机制解决的问题是「一个密钥要穿过配置序列化层，但不该出现在序列化输出里」。** 当一个 ACP 客户端把带密钥的 MCP 服务器配置发给 goose 时，配置要经过 goose 的配置结构——如果密钥原样落在结构里，它就可能被写进磁盘、被日志打印、被序列化到别处。所以这里把真值留在进程内存，只交出一个**不可用的引用**。

**幂等性由内容查找实现，而不是由调用方保证。** `hold_client_secret` 先遍历已有表项，**如果同一个值已经存在就复用它的键**（`find_map(|(key, held)| (*held == value).then(|| key.clone()))`），否则才生成新键。于是同一个密钥被重复交两次不会产生两个表项——这对「同一会话里反复加载配置」的情形是必要的，否则表会无限增长。

**错误消息的第二半是给人看的行为指引。** `send this MCP server again when loading the session`——它告诉用户：这个键只在**收到它的那个进程**里有效，进程换了就得重新发一次。这解释了为什么这个表是进程内的而不是持久的：**密钥的生命周期与「它被发送到哪个进程」绑定**。

**`Mutex` 的加锁写法处理了中毒。** 两处都是 `.lock().unwrap_or_else(|poisoned| poisoned.into_inner())`——也就是说一个在持锁期间 panic 的线程不会让整个密钥表永久不可用。对于一个纯内存映射来说这是对的选择：数据本身不会因 panic 而不一致（只是插入/查找）。

**这个表没有任何上限，也没有清理。** 会话结束不会清除，进程退出才消失。这在「密钥数量 = 会话里配置的 MCP 服务器数量」的前提下是可接受的（几十个量级），但它隐含依赖「会话数不会在一个进程里无限累积」。

**`ToolInfo` 是这套机制对外的汇报形状。** 五个字段：`name`、`description`、`parameters: Vec<String>`、`permission: Option<PermissionLevel>`、`input_schema: Option<serde_json::Value>`。

`input_schema` 带 `#[serde(skip_serializing_if = "Option::is_none")]`——**一个没有 schema 的工具不会在 JSON 里出现 `"input_schema": null`**。这与 `available_tools` 的空列表处理是同一手法：可选就是可选，不要用 null 占位。

而构造函数 `ToolInfo::new(name, description, parameters, permission)` 把 `input_schema` 设为 `None`，另有一个 `with_input_schema`  builder 方法。这是一个「基础构造函数 + 可选附加」的形状，让绝大多数调用点不必传一个 `None`。

**`parameters: Vec<String>` 与 `input_schema: Option<Value>` 并存，说明有两套工具描述。** 前者是一个**扁平的参数名列表**（给人看、给权限判定用），后者是**完整的 JSON Schema**（给模型用）。保留两套而不是只留 schema，是因为很多地方只需要「这个工具要几个参数、叫什么」，而不想解析 schema。

## 关键取舍

**源码事实：** `#[allow(clippy::large_enum_variant)]` 带明确理由：装箱会让每处检视传输字段的地方多一次 match 解包。

**源码事实：** 前两个变体用 `tag = "type"`，`External` 用 `untagged`，因为 oi-kit 的配置自带标签，不能叠加。`untagged` 的「靠试探」在这里安全，因为 `External` 排最后且内部有自己的校验。

**源码事实：** `description` 有专门的反序列化器把 `null` 变成空串，并有两条测试分别覆盖「缺失」与「显式 null」。

**源码事实：** `available_tools` 空列表意味着**看见全部工具**（字段是限制项而非开关），且空列表不被序列化出去。

**源码事实：** 唯一性检查用规范化后的 `key`，报错文案用原始 `name`。

**源码事实：** `resolve` 只对 `External` 做事（外部配置可能含需要展开的环境变量），内置/平台原样返回。

**源码事实：** 客户端密钥以「真值留内存、交出带前缀的不可用引用」的方式穿过配置层；同一个值重复持有会复用同一个键；取不到时报错并给出「重新发送这个 MCP 服务器」的行为指引。

**源码事实：** 密钥表的 `Mutex` 在中毒时仍可用（`into_inner()`），但**没有上限也没有清理**。

**架构推论：** 这一层处理的是一个典型的「配置层被当作数据管道」的问题：扩展配置既要能被序列化（写进设置、传给 UI、发给别的进程），又要携带不该被序列化的东西。`CLIENT_SECRET_PREFIX` 那套「引用替换真值」的做法是这类问题的通用解——它让**配置对象本身是安全的**，而真值只在一个明确的、可解释的作用域里存在。

代价是引入了**隐式状态**：一个 `ExtensionConfig` 是否可用，取决于它所在的进程里有没有对应的表项。而错误消息把这件事说出来了，这很关键——否则用户会拿到一个「配置完全正确但用不了」的谜题。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/agents/extension.rs` | `ToolInfo` | 工具描述的统一形状（名字、参数、权限、可选 schema） |
| `crates/goose/src/agents/extension_config.rs` | `ExtensionConfig` | 三态枚举：内置 / 平台 / 外部传输 |
| `crates/goose/src/agents/extension_config.rs` | `ExtensionConfig::is_tool_available` | 工具可见性判定，空列表即全开 |
| `crates/goose/src/agents/extension_config.rs` | `ExtensionConfig::resolve` | 仅外部传输需要解析（环境变量展开） |
| `crates/goose/src/agents/extension_config.rs` | `CLIENT_SECRET_PREFIX` | 进程内密钥引用的前缀 |
| `crates/goose/src/agents/extension_config.rs` | `hold_client_secret` / `client_secret` | 真值留内存、交出引用、反向查找与错误指引 |
| `crates/goose/src/agents/extension_config.rs` | `CLIENT_SECRETS` | 进程级密钥表，中毒仍可用、无上限 |
| `crates/goose/src/agents/extension_config.rs` | `deserialize_null_with_default` | 把 YAML 的空值变成默认值 |
| `crates/goose/src/agents/extension_config.rs` | `check_unique_keys` | 用 key 判重、用 name 报错 |
| `crates/goose/src/agents/extension_config.rs` | `DEFAULT_EXTENSION` / `DEFAULT_DISPLAY_NAME` | 默认扩展与展示名 |

## 参考

- [goose 源码：agents/extension_config.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/extension_config.rs)
- [goose 文档：扩展](https://block.github.io/goose/docs/getting-started/using-extensions/)
- [serde 文档：枚举表示（tagged / untagged）](https://serde.rs/enum-representations.html)
- [MCP 规范：工具列表](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

这一层与主核里「插件/扩展的配置对象」同构：主核的扩展配置也要能被 dump 出来、能被比较、能被传给别的进程，而其中可能含不该外泄的项（连接口令）。主核通常用「引用令牌 + 进程内表」的方式处理，与这里的 `CLIENT_SECRET_PREFIX` 是同一个解法。

对 agent 工程的直接价值有三条。其一，**空列表的语义必须选一个并写清**。`available_tools: []` 是「全开」还是「全关」，两种实现都合理，但猜错会让一个扩展静默失去全部工具或意外获得全部工具。其二，**「配置即数据管道」时要处理不可序列化的部分**。把真值留在进程内存、交出不可用的引用，比让密钥穿过每个序列化点安全得多——但必须给出可诊断的错误，否则会变成「配置正确却用不了」的谜题。其三，**反序列化的宽容度要针对真实文件格式**。YAML 的 `key:` 会产出 `null`，而手写配置里这种写法极常见；不专门处理就会让一个空的 `description` 让整份配置解析失败。
