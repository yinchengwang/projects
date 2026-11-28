---
project: goose
feature: provider-def
title: 提供方定义与两枚编译期常量
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/providers/base.rs
    symbols: [ProviderDef, ProviderType]
---

# 提供方定义与两枚编译期常量

## 一句话职责

用一个 trait 把「怎么创建某个 provider 的实例」与「这个实例能不能被共享」绑定在一起，并用两枚关联常量让上层在不接触具体类型的前提下决定「要不要缓存实例」与「要不要启动 MCP 服务器」。

## 机制是什么

`ProviderDef` 继承 `ProviderDescriptor`，加 `Send + Sync`，核心是一个关联类型 `type Provider: Provider + 'static`——每个 `ProviderDef` 实现都对应一个具体的 `Provider` 实现类。

它有三个构造函数：`from_env(extensions, tls_config)` 是必须实现的；`from_env_with_working_dir(...)` 与 `from_env_with_default_model(...)` 都有默认实现，直接转发到 `from_env`（注意 `working_dir` 参数在默认实现里以下划线开头，即被忽略）。

两枚关联常量是这一页的核心：

- `SESSION_BOUND: bool = false`——注释说「实例持有远端会话，或由某个会话的扩展与工作目录构造出来，所以不能共享」。
- `RUNS_OWN_TOOL_LOOP: bool = false`——注释说「它拿到会话的 MCP 服务器并自己跑工具循环，那么 goose 就不启动那些服务器」。

两个默认值都是 `false`，而「不共享」与「自己跑工具循环」这两种特殊情况必须由实现者**显式声明**。

`ProviderType` 是个四值枚举：`Preferred`、`Builtin`、`Declarative`、`Custom`，derive 了 `Copy` 与 `Serialize`/`Deserialize`。

## 怎么实现的

**两枚常量表达的是两类不同的「外部副作用」。**

`SESSION_BOUND` 回答的是「这个 provider 实例能否被多个会话复用」。默认 `false` 意味着「可以复用，缓存它是安全的」。设为 `true` 时，创建成本必须每个会话付一次——因为实例里钉着某个会话的远端状态（ACP 会话就属于此类，见 i01）。

`RUNS_OWN_TOOL_LOOP` 回答的是「这个 provider 内部会不会调用 MCP 工具」。默认 `false` 意味着「它只是收发消息，工具循环由 goose 自己的状态机负责」。设为 `true` 时，goose 就必须把 MCP 服务器交给它而不是自己启动——否则会出现两套工具循环或两套服务器实例。

这两个问题在代码里都表现为「某个地方少写一行就会出错」的耦合，把它们提成常量，等于把「隐式约定」变成了「编译期可见的声明」。

**`from_env` 的三个变体是能力叠加而非重载。** 三个方法名都叫 `from_env` 开头，参数差异只有两个：`working_dir` 与「默认模型」。

- `from_env`：基础形态，只有扩展列表与 TLS 配置。
- `from_env_with_working_dir`：多一个工作目录。
- `from_env_with_default_model`：签名与 `from_env` 完全相同。

第三个变体值得注意：它的参数与 `from_env` 一模一样，所以它**不是**「传了额外参数」的变体，而是语义上的区分——「请用该 provider 的默认模型来构造」而不是从配置读。默认实现转发到 `from_env`，意味着基础实现本身就用了默认模型；只有那些「默认模型与配置解析路径不同」的 provider 才需要覆盖它。

前两个的默认实现都忽略新增参数，转发到 `from_env`。所以「不关心工作目录」的 provider 什么都不用做，而 `from_env_with_working_dir` 的默认实现里那个 `_working_dir` 前缀下划线正是 Rust 的「刻意未用」标记。

**返回类型是 `BoxFuture`。** 三个方法都返回 `BoxFuture<'static, Result<Self::Provider>>`。用 boxed future 而不是 `async fn`，是因为 trait 要保持对象安全——`ProviderDef` 本身可能被放进 `Box<dyn ProviderDef>` 之类的容器，而 `async fn` 在 trait 里会产生非对象安全的方法。这与 g05 里 `ProviderRetry` 的处理是同一类手法。

`'static` 生命周期要求意味着 future 不能借用调用方的栈——这也是为什么 `working_dir` 按值传入（`PathBuf` 而不是 `&Path`）。

**`ProviderType` 的四值。** `Builtin` 是编译进二进制的 provider；`Declarative` 是由声明式配置文件描述的（见 g04）；`Custom` 是用户通过配置 API 创建并持久化的自定义 provider；`Preferred` 是「用户偏好的那个」，用于区分「显式选择」与「默认回落」。

derive `Copy` 说明它是纯标签——不携带数据，可以自由复制。这一点很重要：它被当作分类标签在配置与 UI 之间传递，若带数据就会带来所有权的麻烦。

## 关键取舍

**源码事实：** `SESSION_BOUND` 与 `RUNS_OWN_TOOL_LOOP` 的默认值都是 `false`，即「可共享 + 不跑工具循环」。这是「普通 API provider」的默认画像，特殊者必须显式声明。

**源码事实：** `from_env_with_default_model` 的参数与 `from_env` 完全相同，区别纯属语义。所以无法靠签名区分，只能靠调用点的选择。

**源码事实：** 三个构造函数都返回 `BoxFuture<'static, ...>`，`PathBuf` 按值传入。这是为保 trait 对象安全与 `'static` 边界付的代价。

**源码事实：** `ProviderDef` 的 `Provider` 关联类型要求 `'static`，所以 provider 实例不能借用任何栈上数据。

**架构推论：** 把「能否共享」提成常量而不是运行时查询，是一个有意的简化。运行时查询意味着上层要先拿到实例才知道能不能缓存，而那时已经付出了创建成本；常量则在**实例存在之前**就给出答案。代价是它必须由实现者诚实声明——没有机制验证「声明 false 却实际有会话状态」这种违约。

**架构推论：** `RUNS_OWN_TOOL_LOOP` 这个常量的存在揭示了 goose 的一个结构性事实：MCP 服务器由谁启动是一个会冲突的资源决策。如果 provider 内部已经有自己的工具循环（例如 ACP 对端本身就是一个 agent），goose 再启动一套 MCP 服务器就会造成「同一批工具被实例化两次、两套循环各自调用」。把这个决策从「provider 内部行为」提升为「声明式契约」，让主循环能提前分流。

**架构推论：** 三个 `from_env*` 变体而非一个带完整参数的方法，反映的是「不同 provider 需要的构造信息不同」。若统一成一个「大参数」方法，几乎所有实现都要忽略大部分参数——那才是真正的噪音。拆成三个变体，每个 provider只覆盖自己关心的那个，其余靠默认实现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/providers/base.rs` | `ProviderDef` | provider 构造契约与两枚行为常量 |
| `crates/goose/src/providers/base.rs` | `ProviderDef::SESSION_BOUND` | 实例是否绑定会话、不可共享 |
| `crates/goose/src/providers/base.rs` | `ProviderDef::RUNS_OWN_TOOL_LOOP` | 是否自带工具循环、goose 不启动 MCP |
| `crates/goose/src/providers/base.rs` | `ProviderDef::from_env` | 必须实现的基础构造函数 |
| `crates/goose/src/providers/base.rs` | `ProviderDef::from_env_with_working_dir` | 带工作目录的变体，默认转发 |
| `crates/goose/src/providers/base.rs` | `ProviderDef::from_env_with_default_model` | 用默认模型的变体，默认转发 |
| `crates/goose/src/providers/base.rs` | `ProviderType` | Preferred/Builtin/Declarative/Custom 四值标签 |

## 参考

- [goose 源码：providers/base.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/providers/base.rs)
- [goose 文档：支持的提供方](https://block.github.io/goose/docs/getting-started/providers/)
- [Rust 文档：trait 对象安全](https://doc.rust-lang.org/reference/items/traits.html#object-safety)

## 与主核 / AI 的关系

主核里最接近的是「存储引擎注册表」：一个引擎实现要被注册进去，需要提供「怎么打开」的构造入口，并声明若干能力位（是否支持事务、是否支持批量读）。`ProviderDef` 的两个常量正是能力位，只不过它们描述的不是「引擎能做什么」，而是「上层该为它做什么」。

与 smolagents 的 `Model` 基类对比，差别很明显：smolagents 里模型实例就是实例，没有「能否共享」的概念，因为它是单进程单 agent 的框架；goose 是长驻服务，多个会话共享一个进程，实例是否可共享就成了必须回答的问题。这是「库」与「服务」的差别带来的设计差异。

对 AI 工程最实用的一条是「把隐式约定提成常量」。`SESSION_BOUND` 与 `RUNS_OWN_TOOL_LOOP` 这两个属性，如果只是写在文档里，实现者一定会忘；写成关联常量之后，默认 `false` 让普通实现无需关心，而需要特殊行为的实现必须在自己的代码里写一行 `const SESSION_BOUND: bool = true;`——这一行就是提醒，也是可搜索的线索。任何「上层必须知道、下层容易忘」的约定，都值得这样显式化。

另一条是 boxed future 换对象安全这个取舍。`async fn` in trait 在 Rust 里越来越可用，但对象安全约束仍在。当一个 trait 既要被 dyn 使用、又要有异步方法时，`BoxFuture` 返回类型几乎是唯一选择，代价是每次调用多一次堆分配。对「每个会话创建一次 provider」这种低频操作，这个代价可以忽略——但对每个请求都调用的 trait，就要三思。
