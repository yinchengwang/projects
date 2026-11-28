---
project: goose
feature: declarative-providers
title: 声明式提供方与自定义接入
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/config/declarative_providers.rs
    symbols: [LoadedProvider, expand_env_vars, persist_custom_provider]
  - path: crates/goose/src/config/declarative_providers.rs
    symbols: [create_custom_provider, update_custom_provider, remove_custom_provider]
---

# 声明式提供方与自定义接入

## 一句话职责

让「接入一个新模型服务」变成一份 JSON 配置：内置的随包发布、用户建的落到配置目录，两类共用同一套加载与注册路径，但只有一个可编辑；同时把密钥与占位符替换这两件敏感事都收在这一层。

## 机制是什么

`DeclarativeProviderConfig` 是一份描述文件的内容：名字、引擎（`ProviderEngine`）、显示名、`api_key_env`（存密钥的键名而非密钥本身）、`base_url`、模型清单、请求头、超时、是否流式、是否需要认证、`env_vars` 列表、`auth`、`dynamic_models`、`toolshim`、`preserves_thinking` 等。

`LoadedProvider` 是两个字段的包装：`config` 与 `is_editable`。这个布尔就是这一页的分界线——

- 用户在 `custom_providers/` 目录下写的 JSON → `is_editable: true`。
- 随包发布的内置定义（`fixed_provider_configs`）→ `is_editable: false`。

所以「内置」与「自定义」不是两种类型，而是同一个类型的两种来源，靠一个布尔区分可写性。`load_provider` 的查找顺序是「先本地文件、再内置」——若用户用同一个 id 覆盖了内置定义，本地文件赢。

## 怎么实现的

**密钥从不落进配置文件。** `create_custom_provider` 里，当需要认证且用 `api_key` 形态时，它做三件事：

1. 用 `generate_api_key_name(&id)` 算出键名（`format!("{}_API_KEY", id.to_uppercase())`）。
2. `config.set_secret(&api_key_name, &api_key)?` 把密钥写进密钥库。
3. 配置文件里的 `api_key_env` 字段存的是**这个键名**，不是密钥。

所以磁盘上的 JSON 永远不含明文密钥。读的时候按 `api_key_env` 去密钥库取值。这个设计让「配置文件可以自由查看、复制、提交」而不泄漏凭据。

**`auth` 与 `api_key` 互斥。** 两个参数都非空时直接 `bail!("cannot set both apiKey and auth.command")`。这条检查在 `create` 与 `update` 里都做了一遍——因为它涉及外部命令执行（`auth` 是一个命令式认证配置），同时给两种凭据来源会造成不确定行为。

选了 `auth` 时 `api_key_env` 置为空字符串；选了 `api_key` 时 `auth` 为 `None`。二者是替换关系而非叠加。

**更新时的密钥生命周期。** `update_custom_provider` 的密钥处理分三支，每一支都带清理：

- 改用 `auth`：若旧的 `api_key_env` 恰是自动生成的那个键名，**删掉密钥库里的旧密钥**，然后置空。
- 需要认证且带新密钥：键名沿用旧的（若为空则重新生成），写入新值。
- 需要认证但没带新密钥：若密钥库里也取不到，报错「auth 启用且无已存密钥时必须提供 apiKey」——不让「改成需要认证但忘了给密钥」静默通过。
- 都不需要认证：同样清掉自动生成的旧密钥。

「只删自动生成的键名」这个条件是关键：用户可能自定义过别的键名，那种不该被系统自动删除。

**非可编辑的定义会静默成功。** `update_custom_provider` 在 `editable` 为假时**什么都不做，直接返回 `Ok(())`**。所以对内置 provider 调更新接口不报错也不改任何东西——这是一个容易被误用的行为，值得在页面上点明。

不过它前置的密钥处理仍会执行——所以「更新内置 provider 且传了 requires_auth」理论上能改密钥库而不改配置文件。这是一个潜在的不一致。

**模型条目的合并是逐字段 `or`。** 更新时对每个新模型条目，若在旧配置里找到同名条目，就逐字段 `.or(...)`：`resolved_model`、`context_limit`、输入输出与缓存单价、`currency`、`supports_cache_control`、`supports_vision`、`thinking_preservation_format`、`request_params`。

所以「更新时只传部分字段」不会把旧值抹掉，而是保留。唯一例外是 `reasoning`，用的是 `|=`（位或，布尔或），因为它是非 `Option` 的布尔字段，缺省即 `false`，不能用 `or` 区分「没传」与「传了 false」。

**请求头的三态归一。** `headers` 的处理写成三条 match 分支：`Some(空 map)` → `None`（表示「清空」，而不是存一个空对象）；`Some(非空)` → 用它；`None` → 沿用旧值。

这是一个典型的 PATCH 语义实现：显式空值表示「删除」，缺省表示「不改」。用一个 `Option` 无法表达三态，只能靠「空 map 等价于 None」这个约定。

**`expand_env_vars` 的四级回落。** 它对 `template` 里每个 `${NAME}` 占位符依次处理：

1. `var.secret` 为真 → `config.get_secret`，否则 `config.get_param`。
2. 取到值就用。
3. 取不到 → 用 `var.default`（若声明了）。
4. 没有默认值且 `var.required` → 报错「必需的环境变量未设置」。
5. 没有默认值且非必需 → **跳过**（占位符原样留着）。

第五条的「原样留着」值得注意：它不会替换成空串，也不会删除占位符。所以缺一个可选变量时，URL 里会留一个 `${FOO}` 字样——那多半会导致请求失败，但错误来自 HTTP 层而不是配置层。

还有一个早退：`if !result.contains(&placeholder) { continue; }`——值只在该占位符真的出现在串里时才去读配置。这省掉了一堆无关的密钥库查询。

**惰性解析。** `resolve_config` 的 docstring 写：「在 provider 实例化时惰性调用，这样启动后通过 UI 配置的值也能被取到」。所以占位符替换不发生在加载配置文件时，而发生在真正要用的时候。这让「改了配置立刻生效」成为可能，不必重启。

同一段里还有「流式覆盖」的处理，注释说明配置或环境可能把值存成字符串 `"true"` 或原生布尔，所以**先试 String 再退回 bool**。这是配置文件与 UI 两条写入路径产生的形态差异。

**id 生成的锁与唯一性。** `generate_id` 从显示名派生：小写、非 `[a-z0-9_-]` 一律替换成下划线、去掉首尾下划线，前缀 `custom_`。

然后它在一个**静态 Mutex** 内做「检查文件是否存在、冲突则加序号」的循环。锁是为了防两个并发的创建请求都算出同一个 id 然后都通过存在性检查。这是一个「检查-然后-写」竞态的经典处理方式——用锁把检查与生成包在一起（虽然真正的写发生在锁外，但 id 已被占位判断过）。

`validate_provider_id` 的要求：首字符必须是 `[a-z0-9_]`，其余只能是 `[a-z0-9_-]`。所以不允许大写、不允许点号。

**`custom_provider_file_path` 是第二道防线。** 它不调 `validate_provider_id`，而是自己检查：非空、不含 `/`、`\`、不含控制字符。然后 `join(format!("{}.json", id))`。

这道检查防的是**路径穿越**——即使某处漏了 `validate_provider_id`，`../foo` 这类 id 也无法逃出目录。但它允许大写与点号，与 `validate_provider_id` 的规则不完全一致。

**注册时的两个类型标签。** `register_declarative_providers` 先注册全部内置定义（标 `ProviderType::Declarative`），再注册用户定义（标 `ProviderType::Custom`）。顺序决定了用户定义能覆盖同名的内置定义。

**`preserves_thinking` 的引擎相关默认。** 创建时若没显式给，用 `should_preserve_thinking_by_default(&engine)` 推断。更新时更细：显式给了就用；没给且**引擎变了**才重新推断；没给且引擎没变就沿用旧值。这个逻辑防的是「换引擎时沿用旧引擎的默认」这个错误。

## 关键取舍

**源码事实：** `LoadedProvider.is_editable` 区分来源。用户目录下的文件可编辑，内置定义不可编辑。

**源码事实：** `update_custom_provider` 对不可编辑的定义直接返回 `Ok(())`，不报错也不改动。

**源码事实：** 密钥存密钥库，配置文件只存键名 `api_key_env`。删除密钥时只删「自动生成的那个键名」，用户自定义的键名不动。

**源码事实：** `expand_env_vars` 对缺失的非必需变量**跳过**（占位符原样保留），不替换成空串。

**源码事实：** `headers` 的三态：空 map 表示清空，缺省表示不改。

**源码事实：** id 生成用静态 Mutex 保护「检查存在性 + 加序号」的循环。

**架构推论：** 「一个类型、两种来源、一个布尔」比「内置类型 + 自定义类型」更省事。若分成两个类型，注册、加载、序列化都要写两套；现在只有一处判断 `is_editable`，其余代码完全共用。代价是这个布尔必须在每个写路径上被检查——而当前实现里有一处漏检（密钥处理在检查之前），这正是「靠布尔分流」这种设计的典型风险点。

**架构推论：** 「配置存键名、密钥存密钥库」这个分离是声明式 provider 能安全存在的前提。若密钥直接进 JSON，则整个功能会变成「请把 API key 写进明文文件」——那在任何共享环境里都不可接受。分离之后，配置文件变成可以自由分发的描述，密钥变成每个安装各自的私有状态。

**架构推论：** 「惰性解析占位符」这个选择让配置具备热更新能力，代价是错误暴露得晚——配置写错时不会在加载时被拒，而会在第一次请求时表现为「URL 里有个奇怪的 `${FOO}`」。若改成加载时解析，则能早发现，但会失去「UI 改完立即生效」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/config/declarative_providers.rs` | `LoadedProvider` | 配置加可编辑性标记 |
| `crates/goose/src/config/declarative_providers.rs` | `expand_env_vars` | 占位符替换与四级回落 |
| `crates/goose/src/config/declarative_providers.rs` | `create_custom_provider` | 生成 id、存密钥、落盘 |
| `crates/goose/src/config/declarative_providers.rs` | `update_custom_provider` | 逐字段保留旧值的 PATCH 式更新 |
| `crates/goose/src/config/declarative_providers.rs` | `remove_custom_provider` | 删文件并清理自动生成的密钥 |
| `crates/goose/src/config/declarative_providers.rs` | `load_provider` | 先本地文件、再内置定义的查找顺序 |
| `crates/goose/src/config/declarative_providers.rs` | `persist_custom_provider` | 序列化并以私有权限写盘 |
| `crates/goose/src/config/declarative_providers.rs` | `generate_id` | 显示名派生 id，锁内解决冲突 |
| `crates/goose/src/config/declarative_providers.rs` | `validate_provider_id` | id 字符集校验 |
| `crates/goose/src/config/declarative_providers.rs` | `custom_provider_file_path` | 拒绝路径分隔符的穿越防线 |
| `crates/goose/src/config/declarative_providers.rs` | `register_declarative_providers` | 内置标 Declarative、用户标 Custom |

## 参考

- [goose 源码：config/declarative_providers.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/config/declarative_providers.rs)
- [goose 文档：自定义提供方](https://block.github.io/goose/docs/getting-started/providers/)
- [goose 文档：密钥管理](https://block.github.io/goose/docs/guides/secrets/)

## 与主核 / AI 的关系

主核里最接近的是「外部数据源定义」：连接串、凭据、表清单都描述在一份配置里，凭据指向外部密钥而非明文，配置可热加载。`DeclarativeProviderConfig` 与这个结构几乎同构，连「配置存引用、密钥存外部」这条原则都一样。

与 smolagents 的自定义工具路径相比，差别在于「接入的对象」。smolagents 自定义的是工具（要传源码），goose 自定义的是 provider（要传引擎与端点）。前者必须能执行代码，后者只需能发 HTTP 请求——所以后者可以用纯 JSON 描述，前者不行。这也是为什么声明式 provider 能做到「随包发布几十个内置定义」，而工具做不到。

对 AI 工程最实用的一条是「配置文件存键名、密钥存密钥库」的强制分离。这不该是「建议做法」而应是结构约束：只要配置文件里出现密钥字段，就一定会有用户把带密钥的文件提交到仓库。把密钥从类型上移走（`api_key_env: String` 而非 `api_key: String`），让「泄漏」在结构层面不可能发生。

另一条是 `update` 里的逐字段 `Option::or` 合并。做 PATCH 式更新时，若直接整体替换，调用方必须先把完整对象读回来再改一个字段——这在并发场景下会丢更新。逐字段 `or` 让「只传想改的字段」成立，代价是**无法显式置空**（`Option` 的 `None` 既表示「没传」也表示「要清空」）。要同时支持这两件事，得用嵌套 `Option<Option<T>>` 或专门的哨兵类型——这也是为什么 `headers` 那里要用「空 map 等于清空」这个约定来补足。
