---
project: open-interpreter
feature: provider-caps
title: 提供方能力与账号状态
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/model-provider/src/provider.rs
    symbols: [ProviderCapabilities, ProviderAccountState, ProviderUnauthorizedRecovery]
  - path: codex-rs/model-provider/src/provider.rs
    symbols: [RemoteCompactionSupport, enforce_managed_residency, ProviderAuthRecoveryMessages]
---

# 提供方能力与账号状态

## 一句话职责

用一个 trait 把「这个后端能做什么」与「这个后端现在是什么状态」两件事收敛到 provider 自己身上：能力是一份可被上层收紧的上界，账号状态是一个可失败的结果，而认证恢复是一个带用户可见话术的两态过程。

## 机制是什么

`ProviderCapabilities` 有五个字段：

- `namespace_tools: bool`
- `image_generation: bool`
- `web_search: bool`
- `external_web_access: bool`
- `remote_compaction: RemoteCompactionSupport`

doc comment 定下了这个结构的地位：「这些能力是**provider 拥有的上界**。调用方可以通过普通配置进一步关闭更多功能，但不应当暴露一个活跃 provider 在这里标记为不支持的功能」。

所以它的方向是单向的：provider 说不支持就是真不支持；provider 说支持只是「允许」，调用方仍可再关。

`RemoteCompactionSupport` 两值：`Unsupported` 与 `V2`。后者的注释说明具体形态：「provider 通过 Responses 端点支持 `compaction_trigger` 条目」。

`ProviderCapabilities::default()` 的五个值很值得注意：前四个都是 `true`，只有 `remote_compaction` 是 `Unsupported`。所以「默认全开、远端压缩默认关」——远端压缩是一个需要显式声明的能力。

`ProviderAccountState` 两字段：`account: Option<ProviderAccount>` 与 `requires_openai_auth: bool`。它承载的是「app-visible」的账号状态——即 UI 要展示的那一份。

`ProviderUnauthorizedRecovery` 两值：`NotConfigured`（「provider 没有配置 provider 专属的认证恢复」）与 `Recovered`（「provider 恢复了它的认证状态，请求可以重试」）。

`ProviderAuthRecoveryMessages` 两字段：`started: &'static str` 与 `succeeded: &'static str`——恢复过程中给用户看的生命周期文案。

`ProviderAccountError` 两值：`MissingChatgptAccountDetails` 与 `UnsupportedBedrockApiKeyAuth`，且实现了 `Display` 与 `std::error::Error`。类型别名 `ProviderAccountResult = Result<ProviderAccountState, ProviderAccountError>`。

## 怎么实现的

**`ModelProvider` trait 上的能力方法是「默认全开」。** `capabilities()` 返回 `ProviderCapabilities::default()`。所以不覆盖这个方法的 provider 声明「除了远端压缩之外什么都能做」。

这个默认值的取舍值得讲清：若默认是「全关」，那么每个 provider 都必须显式列一遍自己支持什么，漏一个就少一个功能（表现为「功能悄悄不工作」）。默认全开则相反——漏写一个覆盖只是「功能可能不工作」，而真正的错误会被后端拒绝。诊断路径不同：全关的漏写是「静默缺失」，全开的漏写是「显式报错」。后者更容易排查。

**三个「首选模型」常量是给辅助任务用的。** `approval_review_preferred_model` / `memory_extraction_preferred_model` / `memory_consolidation_preferred_model` 各有默认值（`codex-auto-review`、`gpt-5.6-luna`、`gpt-5.6-terra`），都由同名常量提供。每个方法的 doc 都写同一句话：「需要后端专属模型 id 的 provider 应当覆盖它」。

所以「自动审批复核」「记忆抽取」「记忆整合」这三类辅助推理不必由调用方传模型名，而是问 provider「你的这类任务该用哪个模型」。这个设计解决的是「模型 id 空间按后端而异」的问题：同一个逻辑任务在不同后端的模型名完全不同。

注意 `API_KEY_APPROVAL_REVIEW_PREFERRED_MODEL = "gpt-5.6-luna"` 是 `pub(crate)` 的私有常量，而其他的都是 `pub`。所以「用 API key 认证时，审批复核该用哪个模型」这个知识被封在 crate 内部，不由外部指定。

**`enforce_managed_residency` 是「无条件注入一个头」。** 实现只有几行：

```rust
if let Some(requirement) = read_default_client_residency_requirement() {
    let value = match requirement {
        ResidencyRequirement::Us => HeaderValue::from_static("us"),
    };
    provider.headers.insert(RESIDENCY_HEADER_NAME, value);
}
```

`match` 只有一个分支 `ResidencyRequirement::Us`——所以这个枚举目前只有一个值。`from_static` 而不是 `from_str` 说明这个值是编译期已知的。

它被 `api_provider()` 调用：在 `self.info().to_api_provider(...)` 之后 `enforce_managed_residency(&mut provider)`。所以**每一个**通过 `api_provider()` 得到的 provider 配置都会带上这个头，而不是只在特定路径上。

「data residency」在这类产品里通常意味着「数据必须留在指定区域」。把这个约束实现成「在 client 上强制注入头」，而不是「与服务端协商」，是一个明确的选择：客户端不能选择退出，也无法通过自定义头覆盖它（因为 `insert` 覆盖同名键）。

**`responses_api_provider` 是路由的汇聚点。** 它的 doc 一句话概括了职责：「为 Responses HTTP、压缩与 WebSocket 握手解析路由」。

流程是：`api_provider()` 拿基础配置 → `redirect_policy` 初始为 `Default` → 若四个条件同时成立才尝试工作区路由：

1. `provider_uses_first_party_auth_path(info)`
2. `info.supports_codex_backend_routes()`
3. `self.auth()` 有值且 `is_chatgpt_auth()`
4. `self.auth_manager()` 有值

四者皆真才 `auth_manager.workspace_routing(...)` 请求一个 `WorkspaceRouting`；若有结果就 `apply_workspace_routing(&mut provider, routing)`，并把 `redirect_policy` 改成 `Reject`，同时把 `previously_routed` 置真。

`redirect_policy = Reject` 这个联动是最容易被忽略但最重要的一处：一旦路由到了某个工作区后端，就不能再跟随 HTTP 重定向跳出那个后端。`ResolvedResponsesProvider` 的 doc comment 说得直接：「工作区路由即使其路由覆盖没有提供头，也会拒绝重定向」。所以「路由」与「不跟随重定向」是绑在一起的。

那段 `#[expect(clippy::await_holding_invalid_type, reason = "serialize discovery and the session's first successful routing transition")]` 说明了 `previously_routed.lock()` 为什么在 `.await` 期间被持有：要让「发现路由」与「本会话的第一次成功路由切换」串行化。注释是显式地为 clippy 的顾虑提供理由，而不是简单 `#[allow]`。

**账号状态与认证恢复是分开的两组方法。** 与账号/认证有关的方法有六个：

- `auth_manager()` —— 必需，返回 `Option<Arc<AuthManager>>`。上面那条 `TODO` 注释说明了它的现状：「让 auth manager 的访问成为本 crate 内部，从而调用方只通过 `ModelProvider` 解析 provider 专属认证。我们得先想清 Codex 是否该在全局有统一的 provider 专属 auth manager；那是个比这次改动更大的重构」。所以这是一个**已知未收敛**的接口。
- `gateway_auth_manager()` —— 默认 `Ok(None)`；doc 说「宿主用它做显式登录；配置好的 setup 失败仍是错误」。
- `is_recoverable_auth_error(error)` —— 默认只认 HTTP 401；doc 说「默认保留既有的未授权响应处理。认证失败形态不同的 provider 可以识别额外的响应或请求签名错误」。
- `auth_recovery_messages()` —— 默认 `None`。
- `recover_from_unauthorized()` —— 默认返回 `NotConfigured`。
- `auth()` 与 `account_state()` —— 必需。

这六个方法配合起来是一个完整的两级恢复流程：**先问 provider 能不能自己恢复（`recover_from_unauthorized`），恢复不了再走 auth manager**。方法 doc 正是这么写的：「在动用 auth manager 之前，尝试 provider 拥有的认证恢复」。

`ProviderUnauthorizedRecovery::Recovered` 的注释说「provider 恢复了它的认证状态且请求可以重试」——所以它是一个「可以重试」的许可，而不是「已经成功」。这个区别决定了调用方该做什么：`Recovered` 意味着重试，`NotConfigured` 意味着换路径。

**`ProviderAuthRecoveryMessages` 是「文案作为返回值」。** 两个字段都是 `&'static str`，`ProviderAuthRecoveryMessages` 本身是 `Copy`。所以 provider 不是「执行恢复并打日志」，而是「把这套流程该显示的两句话交出来」。

这个设计的动机可以推：恢复过程往往要等几秒（重新登录、刷新 token），用户需要看到「正在恢复认证……」而不是一个卡住的界面。把这个文案从 provider 里提出来，让 UI 层能自己决定何时显示——provider 只提供词，不控制时机。

`&'static str` 的必要性与 `RenderedFragment.role` 一致：这类用户可见文案应当来自编译期常量，不该是运行时拼接的。

**`ProviderAccountError` 的两个变体各对应一个真实的后端约束。** `Display` 实现里的文案说明了它们：

- `MissingChatgptAccountDetails` → `"plan type is required for chatgpt authentication"`。
- `UnsupportedBedrockApiKeyAuth` → `"Bedrock API key auth is only supported by the Amazon Bedrock model provider"`。

而 `resolve_provider_auth_without_bearer` 里那段检查把第二个变体实践了：

```rust
if matches!(auth, Some(CodexAuth::BedrockApiKey(_) | CodexAuth::BedrockAccessKeys(_))) {
    return Err(CodexErr::UnsupportedOperation(BEDROCK_API_KEY_UNSUPPORTED_MESSAGE.to_string()));
}
```

`BEDROCK_API_KEY_UNSUPPORTED_MESSAGE` 与 `ProviderAccountError` 的 `Display` 文案是**两份独立的字符串常量**，内容一致。这说明这里的错误有两层：一层是 `ProviderAccountError`（喂给 app 的账号状态），一层是 `CodexErr::UnsupportedOperation`（喂给推理路径的）。同一个约束在两个出口都表达了。

`account_state()` 返回 `ProviderAccountResult`——所以「拿不到账号状态」是一个**可能的正常结果**，不是一个该抛的错误。这与 `auth()` 返回 `Option<CodexAuth>` 的宽容度一致：认证可以没有，账号状态可以取不到，都不意味着请求不能发。

**`map_api_error` 是一个单行转发。** 默认实现 `codex_api::map_api_error(error)`。留着这个方法的意义是「provider 可以把 API 客户端错误映射成它自己的用户可见错误表示」。默认转发意味着只有需要特殊映射的后端才覆盖。

**`runtime_base_url` 与 `api_provider` 的关系。** `runtime_base_url()` 的默认实现直接返回 `info().base_url`（同步地包在 future 里）。而 `api_provider()` 走的是 `info().to_api_provider(auth_mode)` 加 residency 注入。

两个方法都表达「请求时会用哪个 base URL」，但 `runtime_base_url` 是给「查询」用的（可能早于认证解析），`api_provider` 是给「真发请求」用的（已注入 residency）。所以前者可能不包含路由后的地址，后者可能包含。这个差异是接口分工，不是不一致。

## 关键取舍

**源码事实：** `ProviderCapabilities` 的默认前四项全 `true`、`remote_compaction` 为 `Unsupported`。文档明确它是**上界**，调用方只能在此基础上再收紧。

**源码事实：** `enforce_managed_residency` 的 `match` 只有一个分支（`ResidencyRequirement::Us`），且用 `insert` 无条件覆盖同名头。

**源码事实：** 工作区路由需要四个条件同时成立；一旦生效，`redirect_policy` 从 `Default` 变成 `Reject`。

**源码事实：** 认证恢复有两级：先 `recover_from_unauthorized`，`NotConfigured` 才走 auth manager。

**源码事实：** 恢复话术是 `&'static str` 的 `Copy` 结构，provider 给词不给时机。

**源码事实：** Bedrock API key 的限制在种子层（`resolve_provider_auth_without_bearer`）就拒绝，而 `ProviderAccountError::UnsupportedBedrockApiKeyAuth` 在账号状态层又表达一次。

**源码事实：** `auth_manager()` 上的 `TODO` 明确记着「让调用方只通过 `ModelProvider` 解析认证」是一个尚未完成的更大重构。

**架构推论：** 「能力是上界」这条约定让能力协商变成**单向**的：provider 只能否决，不能授予。这比双向协商简单得多，也避免了「provider 说支持但配置说不支持」谁优先的争议——配置永远能再关，provider 永远能再否。

**架构推论：** `ProviderUnauthorizedRecovery` 用两个变体表达「我试过了」与「我没有这条路」，而不是用 `bool`。调用方拿到布尔时会自然地问「那我要不要重试」，而两个变体直接回答了下一步动作。同理 `NotConfigured` 这个措辞强调的是「provider 没有配置它」，而不是「恢复失败了」——后者会让调用方误以为该再试一次。

**架构推论：** 把「路由」与「拒绝重定向」绑在一起，是一个安全考虑很明确的联动。路由的意义是把请求定向到某个后端；若之后还跟随 HTTP 重定向，一个 302 就能把请求（连同认证头）送到另一个主机。所以「一旦路由就 Reject」不是保守，是路由本身的前提。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/model-provider/src/provider.rs` | `ProviderCapabilities` | 五项能力的 provider 上界 |
| `codex-rs/model-provider/src/provider.rs` | `ProviderCapabilities::default` | 前四项开、远端压缩关 |
| `codex-rs/model-provider/src/provider.rs` | `RemoteCompactionSupport` | 远端压缩支持的两值枚举 |
| `codex-rs/model-provider/src/provider.rs` | `ProviderAccountState` | app 可见的账号状态 |
| `codex-rs/model-provider/src/provider.rs` | `ProviderAccountError` | 账号状态取不到的两个原因 |
| `codex-rs/model-provider/src/provider.rs` | `ProviderUnauthorizedRecovery` | 恢复的两态：未配置 / 已恢复可重试 |
| `codex-rs/model-provider/src/provider.rs` | `ProviderAuthRecoveryMessages` | 恢复过程的两句用户话术 |
| `codex-rs/model-provider/src/provider.rs` | `enforce_managed_residency` | 无条件注入 data residency 头 |
| `codex-rs/model-provider/src/provider.rs` | `ModelProvider` | provider 行为的总契约 |
| `codex-rs/model-provider/src/provider.rs` | `ModelProvider.responses_api_provider` | 四条件收敛的工作区路由与重定向策略 |
| `codex-rs/model-provider/src/provider.rs` | `ModelProvider.is_recoverable_auth_error` | 默认只认 HTTP 401 |
| `codex-rs/model-provider/src/provider.rs` | `DEFAULT_APPROVAL_REVIEW_PREFERRED_MODEL` | 辅助任务的模型默认值 |
| `codex-rs/model-provider/src/auth.rs` | `BEDROCK_API_KEY_UNSUPPORTED_MESSAGE` | Bedrock 限制在推理路径上的错误文案 |

## 参考

- [open-interpreter 源码：model-provider/src/provider.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/model-provider/src/provider.rs)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [open-interpreter 文档：模型提供方](https://www.openinterpreter.com/docs)

## 与主核 / AI 的关系

主核里最接近的是「存储引擎的能力位与运行状态」：一个引擎声明自己支持事务、支持批量读、支持稀疏索引，上层据此决定走哪条路径；同时引擎还会报告「当前是否可写」「副本是否在线」这类运行状态。`ProviderCapabilities` 是能力位，`ProviderAccountState` 是运行状态，两者的分工与主核完全对应。

与 goose 的 g01（`ProviderDef` 的两枚常量）对照很有意思：那边用**关联常量**表达两个二值能力（`SESSION_BOUND`、`RUNS_OWN_TOOL_LOOP`），因为它们是编译期就确定的、且只需一个布尔。这里用**结构体**表达五项能力，因为数量多、且 `remote_compaction` 需要三值。判据是：能力数量与取值形态决定用常量还是结构。

与 smolagents 的 `Model` 基类相比，那边几乎没有「能力协商」——模型就是 `generate` 与 `stream` 两个方法，能力差异靠运行时失败来发现。这里选择「提前声明」，因为一个要支持十几个后端的客户端不能靠试错：每次请求失败一次才发现「这个后端不支持图片生成」，代价是真实的费用与延迟。

对 AI 工程最实用的一条是「能力默认为开」。这个默认值的诊断价值前面讲过，但还有一层：它把「哪些 provider 特殊」这件事变成**可搜索的**。默认全开意味着任何覆盖 `capabilities()` 的实现都在源码里显式写了「我有例外」，于是「谁不支持什么」是一份可枚举的清单，而不是散落在文档里。

另一条是「路由与重定向策略必须联动」。任何形如「把请求定向到特定后端」的机制，都必须同时禁止后续的自动重定向——否则一次 3xx 就能绕过定向。这条规则在 HTTP 客户端、OAuth 流程、代理配置里都适用，而它经常被漏掉，因为「重定向」看起来是传输层细节，与「路由」不在同一个抽象层。

最后一条是「恢复话术从 provider 交出来，而不是 provider 自己显示」。任何有「等待若干秒」的恢复流程，其用户体验都由两件事决定：等待的合法性（能不能看到为什么等）与等待的可控性（能不能取消）。让 provider 交出文案，就是把「合法性」交给它而把「时机与可控性」留给上层。若 provider 自己控制显示，上层就无法为不同的 UI（CLI、GUI、API）适配同一套恢复流程。
