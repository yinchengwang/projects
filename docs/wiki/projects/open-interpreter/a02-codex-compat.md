---
project: open-interpreter
feature: codex-compat
title: Codex 协议与 SDK 兼容
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/chat-wire-compat/src/lib.rs
    symbols: [ChatCompletionsCompatClient]
  - path: codex-rs/chat-wire-compat/src/request.rs
    symbols: [ToolKinds, ToolOutputKind]
  - path: codex-rs/acp-server/src/lib.rs
    symbols: [AppServerAcpAgent, CONFIG_ID_HARNESS]
---

# Codex 协议与 SDK 兼容

## 一句话职责

三根「向外兼容」的支柱：`ChatCompletionsCompatClient` 把内部的 Responses API 请求**降级**成 Chat Completions 报文并按上游脾性裁剪字段，`ToolKinds` / `ToolOutputKind` 记住「这个名字的工具其输出该还原成哪种内部调用类型」，`AppServerAcpAgent` 把整套能力按 **ACP（Agent Client Protocol）** 暴露出去，并用 `CONFIG_ID_HARNESS` 把 harness 变成一个会话级可切换的配置项。

## 机制是什么

内部协议与外部协议不是同一个东西，这一层负责翻译，方向分两种：

| 方向 | 组件 | 做什么 |
|---|---|---|
| 出站 | `ChatCompletionsCompatClient` | Responses API 请求 → Chat Completions 报文；SSE 流 → 内部响应事件 |
| 出站 | `ToolKinds` / `ToolOutputKind` | 工具定义被拍平时记下「谁需要被还原成什么」 |
| 入站 | `AppServerAcpAgent` | 把内部能力按 ACP 的 JSON-RPC 形状对外提供 |

三种 wire API（`Responses` / `Chat` / `Messages`）不是同一个协议的三个名字，而是**三种不同的 JSON 结构**。项目内部的对话模型是 Responses 风格（`ResponseItem` 序列），所以只要目标是 Chat 或 Messages，就必须转换。

转换里最难的不是字段改名，而是**两处结构性差异**：

1. **工具调用与结果的配对规则**。Chat Completions 要求「一个 assistant 消息里的全部 tool_calls，其对应的 tool 消息必须紧邻其后，且在同一批里全部出现」。而 Responses 风格的对话历史里，工具执行期间可能夹进上下文更新消息。于是转换时必须**推迟**那些插进来的消息，等整组工具结果齐了再放出去。
2. **工具类型在转换后会丢失**。Responses 里有 `function`、`custom`、`namespace` 三种工具定义，拍平成 Chat Completions 的 `{"type":"function"}` 之后，三种都长得一样。而回程（把上游返回的 tool_call 还原成内部 `ResponseItem`）必须知道它原本是哪种，否则 `custom` 工具的输出会被当成普通函数输出。`ToolKinds` 就是为了补回这个信息而存在的。

## 怎么实现的

**`ChatCompletionsCompatClient` 的公开面只有两个方法，内部却做了四件事。**

```
pub struct ChatCompletionsCompatClient<T: HttpTransport> {
    transport: T,
    provider: Provider,
    auth: SharedAuthProvider,
    request_telemetry: Option<Arc<dyn RequestTelemetry>>,
    sse_telemetry: Option<Arc<dyn SseTelemetry>>,
}
```

泛型参数是 `HttpTransport`，所以测试可以塞一个假的 transport 进去而不碰网络。两个公开方法都是流式入口：

- `stream_request(ResponsesApiRequest, ResponsesOptions)` —— 接受内部请求，内部调 `convert_request` 得到 `(ChatCompletionRequest, ToolKinds)`，再转成 `Value` 交给下面那个；
- `stream_chat_request_value(Value, ToolKinds, ResponsesOptions)` —— 接受已经序列化的报文。**这个方法的单独存在是有意义的**：harness 实现可以直接构造报文（它们本来就在复刻别人的请求体），不必绕道内部模型。

`stream_chat_request_value` 里的四件事，按顺序：

**第一件：上游 URL 覆盖来自一个「永不真正发送」的头部。**

```
pub const CHAT_WIRE_UPSTREAM_URL_HEADER: &str = "x-codex-chat-wire-upstream-url";
```

注释写得很直白：「这是内部额外头部的键，携带每次请求的 chat-completions 上游 URL 覆盖。它从不作为真实头部离开进程；兼容客户端读它以便在构造 provider 请求之前选好上游」。

于是它被读出来之后就不再出现在出站头部里：

```
let provider_base_url = extra_headers.get(CHAT_WIRE_UPSTREAM_URL_HEADER)
    .and_then(|value| value.to_str().ok())
    .unwrap_or(self.provider.base_url.as_str());
```

用「额外头」当作**带内控制通道**是个务实的选择：`extra_headers` 已经从上层一路传下来了，为「这次请求打到哪个上游」再加一个参数会穿透多层签名。代价是这个键必须被显式移除，否则一个内部标记会漏给上游——而注释正是在提醒这件事。

**第二件：按上游删字段，这是「兼容」的具体含义。**

```
sanitize_chat_body_for_provider(&mut body, provider_base_url);
```

它先按 host 判断三个目标（`api.deepseek.com`、`openrouter.ai`、`api.groq.com`），然后各自处理：

- **DeepSeek** 把 `role: "developer"` 的消息改成 `"user"`。这是**语义降级**：DeepSeek 的 chat 接口没有 developer 角色，而内部把 developer 消息用于技能/系统级指令；改成 user 至少保住内容，若直接发出去会被拒。
- **OpenRouter** 双向调 token 上限：`cap_chat_token_limits(body, 1024)` 把超过 1024 的 `max_completion_tokens` / `max_tokens` 压到 1024，`ensure_chat_token_limit(body, 1024)` 在两者都缺时补上 1024。先压后补，于是**无论原报文有没有这个字段，结果都是不超过 1024**。
- **Groq** 最严格：压到 32768，并逐个删掉 `prompt_cache_key`（Kimi/OpenAI 扩展）、`thinking`（Kimi/Anthropic 风格开关）、`parallel_tool_calls` / `response_format` / `reasoning_effort` / `service_tier` / `store` / `verbosity`，还要从每条 message 里删掉 `reasoning_content`（DeepSeek 扩展）。

这一段的注释点名了三个具体的壳：`kimi-cli` 的 `prompt_cache_key`、`deepseek-tui` 的 `reasoning_content`、`kimi-cli` 在 thinking-toggle 模型上的 `thinking`。也就是说**别的 harness 加的字段，在这里被按目标上游删掉**——同一份报文要同时满足「复刻形状」与「目标上游能接受」两个要求，而这两者会冲突，冲突点就在这里被解开。

`reasoning_content` 的处理尤其能说明问题：它要遍历所有消息删掉，因为 DeepSeek 把它回显在 assistant 消息上，而 Groq 在 assistant tool_call 上会拒绝它。

**第三件：非流式响应被合成为一条 SSE，于是下游只有一条路径。**

```
let streaming = body.get("stream").and_then(Value::as_bool).unwrap_or(false);
if !streaming {
    let response = ...;
    let sse = synthetic_sse_from_chat_completion_response(&response.body)?;
    let bytes = stream::once(async move { Ok(Bytes::from(sse)) });
    return Ok(spawn_chat_stream(Box::pin(bytes), ..., tool_kinds));
}
```

`synthetic_sse_from_chat_completion_response` 把一个普通的 `{id, model, usage, choices}` 响应逐个 choice 改写成 SSE 的 `chat.completion.chunk` 事件，末尾补一条 usage 块。这样做的好处是**调用方不必区分流式与非流式**：两种情况都返回同一种 `ResponseStream`，同一套解析器处理。

代价是多了一次 JSON 解析与重序列化，以及必须手工补齐 `id`（响应里没有 `id` 时用 `"chatcmpl-compat"`）。但换来的是「一条路径通吃」，这在兼容层里很值——否则每个 harness 都要处理两种返回形态。

**第四件：OpenRouter 的 402 被翻译成一次带新上限的重试。**

```
fn openrouter_affordable_token_limit(is_openrouter: bool, error: &ApiError) -> Option<u64> {
    if !is_openrouter { return None; }
    let ApiError::Transport(TransportError::Http { status, body, .. }) = error else { return None; };
    if *status != http::StatusCode::PAYMENT_REQUIRED { return None; }
    let body = body.as_deref()?;
    parse_openrouter_affordable_token_limit(body)
}
```

只有「是 OpenRouter + 402 + 有响应体」三个条件全满足才进入解析，而解析靠**在错误文本里找 `"can only afford "` 并读出后面的数字**：

```
fn parse_openrouter_affordable_token_limit(body: &str) -> Option<u64> {
    let marker = "can only afford ";
    let start = body.find(marker)? + marker.len();
    let rest = &body[start..];
    let mut digits = String::new();
    for ch in rest.chars() {
        if ch.is_ascii_digit() { digits.push(ch); }
        else if (ch == ',' && !digits.is_empty()) || (digits.is_empty() && ch.is_ascii_whitespace()) { continue; }
        else { break; }
    }
    let limit = digits.parse::<u64>().ok()?;
    (limit > 0).then_some(limit)
}
```

这段是有分寸的：接受数字间与开头的千位分隔逗号（`can only afford 1,000`），遇到其它字符立刻停，最后要求 `limit > 0`（0 会让请求必然失败）。这是「上游没有结构化错误码时，只能解析人话」的现实做法——脆弱，但只在 402 且只有 OpenRouter 时才走。

重试是把原报文克隆一份、按解析出的限额压 token 上限、再发一次：

```
let mut retry_body = original_body.clone();
cap_chat_token_limits(&mut retry_body, affordable_limit);
ensure_chat_token_limit(&mut retry_body, affordable_limit);
```

**`ToolKinds` 是一个名字到「输出种类的映射，三个变体。**

```
pub enum ToolOutputKind {
    Function,
    NamespacedFunction { name: String, namespace: String },
    Custom,
}
pub type ToolKinds = HashMap<String, ToolOutputKind>;
```

`convert_tools` 在做拍平的同时往 `tool_kinds` 里登记：

- `"function"` → `ChatTool` + `ToolOutputKind::Function`；
- `"custom"` → `ToolOutputKind::Custom`；
- `"namespace"` → 把命名空间里的每个子工具拍平成独立的扁平工具名，登记成 `NamespacedFunction { name, namespace }`，因为扁平化之后「属于哪个命名空间」这个信息在报文里没有了。

回程（`stream.rs`）按这个表还原：

```
let item = match tool_kinds.get(&name) {
    Some(ToolOutputKind::Function) => ResponseItem::FunctionCall { ... },
    Some(ToolOutputKind::NamespacedFunction { name, namespace }) => ...,
    Some(ToolOutputKind::Custom) => ...,
};
```

也就是说**转换是有状态的**：一次请求的转换结果必须活到这次流的解析结束。`convert_request` 的返回类型 `(ChatCompletionRequest, ToolKinds)` 就是这份状态的转移方式——它不藏在 client 字段里，而是随流一起传下去（`spawn_chat_stream(..., tool_kinds)`）。这样并发多请求时各自的映射不会互相覆盖。

`Custom` 这一类存在的理由：Responses 的 custom 工具接受自由格式输入（不是 JSON Schema 约束的对象），而 Chat 只认 `function`。拍平时要把输入包成 `{"input": ...}`，回程时再拆开——所以「它是 custom」这件事必须被记住。

**`AppServerAcpAgent` 是一个把 JSON-RPC 桥到内部 app-server 的适配器。**

`run_main` 只做三件事：

```
pub async fn run_main(arg0_paths: Arg0DispatchPaths) -> anyhow::Result<()> {
    init_tracing();
    let agent = Arc::new(AppServerAcpAgent::start(arg0_paths).await?);
    let outgoing = io::stdout().compat_write();
    let incoming = io::stdin().compat();
    agent.serve(ByteStreams::new(outgoing, incoming)).await?;
    Ok(())
}
```

stdout 写、stdin 读、`init_tracing` 把日志定向到 stderr（`with_writer(std::io::stderr)`）。三者都必须如此——ACP 的 stdout 是协议通道，一行日志混进去就会破坏 JSON-RPC 帧。

`AppServerAcpAgent` 自己不实现 agent 逻辑，它 `start` 时加载配置，然后所有动作都以 `self.client.request_typed(ClientRequest::...)` 转发给内部 app-server。于是这一层是**纯协议翻译**：ACP 的方法名、会话模式、配置项，都映射成内部已有的请求。

**`CONFIG_ID_HARNESS` 是三个配置项之一，且它的设置路径最特殊。**

```
const CONFIG_ID_MODEL: &str = "model";
const CONFIG_ID_REASONING_EFFORT: &str = "reasoning_effort";
const CONFIG_ID_HARNESS: &str = "harness";
```

前两个直接改 `SessionState` 的字段（`session.model = model`、`session.reasoning_effort = reasoning_effort`），第三个不同——它要**fork 线程**：

```
CONFIG_ID_HARNESS => {
    let requested_harness = config_value_id(&request.value)
        .ok_or_else(|| Error::invalid_params().data("missing harness value"))?;
    let selected_harness = self.interpreter_harnesses(&session).await?
        .into_iter()
        .find(|harness| harness.id.as_deref().unwrap_or_default() == requested_harness)
        .map(|harness| harness.id);
    if selected_harness.is_none() && !requested_harness.is_empty() {
        return Err(Error::invalid_params().data("unknown harness"));
    }
    let response: ThreadForkResponse = self.client.request_typed(ClientRequest::ThreadFork {
        params: ThreadForkParams {
            thread_id: session.thread_id.clone(),
            config: Some(HashMap::from([("harness".to_string(), serde_json::json!(selected_harness.clone())])),
            ..ThreadForkParams::default()
        },
    }).await...
}
```

三点值得读：

1. **合法性靠「列表里找得到」**。不是硬编码白名单，而是 `interpreter_harnesses(&session)` 现查——那个列表已经按 wire API 过滤过（见 a01）。于是「这个 provider/模型下不能用的壳」在这里自动不合法。
2. **空串显式允许**。`if selected_harness.is_none() && !requested_harness.is_empty()` 才报错——空串表示「回到 Native」，而 Native 在列表里的 id 是 `None`，所以查找结果为空是正常的。
3. **改 harness 走 fork 而不是改字段**。这与 model / reasoning_effort 的处理完全不同。理由是 harness 影响的是线程级的配置（工具集与提示），已建立的历史不能在中途换壳重解释——必须开一条新线程。`SessionState` 里确实存了 `harness: Option<String>`，但它是在 fork 返回后同步过去的，不是直接写。

对外暴露的选项也是从同一个来源生成：

```
SessionConfigOption::select(SessionConfigId::new(CONFIG_ID_HARNESS), "Harness",
    session.harness.clone().unwrap_or_default(), harness_options)
```

默认值是 `unwrap_or_default()` 即空串，与 `from_config_name` 里「空串 → Native」严丝合缝。而 `harness_options` 来自 `config_options` 里对 `interpreter_harnesses(session)` 的映射——于是「能选什么」与「选了什么会被接受」由同一份数据决定，不存在两处不一致。

## 关键取舍

**源码事实：** 上游 URL 覆盖通过内部头部 `CHAT_WIRE_UPSTREAM_URL_HEADER` 传递，注释明确它「从不作为真实头部离开进程」；取出后即用它选上游，不再透传。

**源码事实：** 字段裁剪按上游分三档：DeepSeek 的 `developer` → `user`（语义降级）；OpenRouter 的 token 上限先压后补到 1024；Groq 压到 32768 并删除 `prompt_cache_key`、`thinking`、`parallel_tool_calls`、`response_format`、`reasoning_effort`、`service_tier`、`store`、`verbosity` 以及消息级 `reasoning_content`。

**源码事实：** 非流式响应被 `synthetic_sse_from_chat_completion_response` 合成成 SSE，使调用方只面对一种返回形态；缺 id 时用 `"chatcmpl-compat"` 兜底。

**源码事实：** OpenRouter 的 402 触发一次重试：从响应文本里找 `"can only afford "` 并解析出新的 token 上限，克隆原报文压上限后再发一次。解析只接受数字与千位逗号，且要求结果为正。

**源码事实：** `ToolOutputKind` 有三态（`Function` / `NamespacedFunction{name,namespace}` / `Custom`），它补偿的是「三种工具定义在 Chat 报文里都长得一样」这个信息丢失；`convert_request` 把它与报文一起返回，随流传递。

**源码事实：** `AppServerAcpAgent` 不实现 agent 逻辑，所有动作以 `request_typed(ClientRequest::...)` 转发内部 app-server；日志定向到 stderr 因为 stdout 是 ACP 协议通道。

**源码事实：** `CONFIG_ID_HARNESS` 的合法性检查是「在按 wire API 过滤后的列表里找得到」，空串显式允许；设置它走 `ThreadFork` 而不是直接改写会话字段，而 model 与 reasoning_effort 是直接赋值。

**架构推论：** 兼容层必然要处理「同一份报文，多个上游，要求互斥」这个局面。这里的解法是**按目标上游做减法**：尽可能复刻形状，然后对严格的上游逐字段删除。反过来（按上游做加法）会失去复刻的意义——那些字段之所以存在，正是因为上游工具在发它们。

**架构推论：** 把转换产出的映射当作**请求级状态**而不是客户端字段，使并发安全成为结构性的而不是靠加锁。若把 `ToolKinds` 存在 client 上，两个并发请求会互相覆盖，症状是「偶发地把 custom 工具当普通函数」——极难复现。

**架构推论：** 「改一个配置值要 fork 线程」说明这个配置项不属于「可以中途改」的那一类。区分「能直接改」与「必须新开线程」是好事，但这条区分只能靠人对每个字段的判断，无法由类型强制——所以它需要注释或测试来固化。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/chat-wire-compat/src/lib.rs` | `ChatCompletionsCompatClient` | Responses → Chat 的降级客户端，CRC 重导出三件公开类型 |
| `codex-rs/chat-wire-compat/src/client.rs` | `CHAT_WIRE_UPSTREAM_URL_HEADER` | 内部头，携带单次请求的上游覆盖，不出进程 |
| `codex-rs/chat-wire-compat/src/client.rs` | `sanitize_chat_body_for_provider` | 按上游删字段/压 token/改角色 |
| `codex-rs/chat-wire-compat/src/client.rs` | `synthetic_sse_from_chat_completion_response` | 非流式响应合成 SSE，统一返回形态 |
| `codex-rs/chat-wire-compat/src/client.rs` | `parse_openrouter_affordable_token_limit` | 从 402 文本解析可用 token 上限 |
| `codex-rs/chat-wire-compat/src/request.rs` | `ToolKinds` | 工具名 → 输出种类的映射，请求级状态 |
| `codex-rs/chat-wire-compat/src/request.rs` | `ToolOutputKind` | `Function` / `NamespacedFunction` / `Custom` 三态 |
| `codex-rs/chat-wire-compat/src/request.rs` | `convert_request` | 转换并同时产出 `ToolKinds` |
| `codex-rs/acp-server/src/lib.rs` | `AppServerAcpAgent` | ACP 适配器：stdio JSON-RPC 桥到内部 app-server |
| `codex-rs/acp-server/src/lib.rs` | `CONFIG_ID_HARNESS` | 把 harness 暴露为会话配置项，设置时 fork 线程 |

## 参考

- [open-interpreter 源码：chat-wire-compat/src/request.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/chat-wire-compat/src/request.rs)
- [open-interpreter 源码：acp-server/src/lib.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/acp-server/src/lib.rs)
- [Agent Client Protocol 规范](https://agentclientprotocol.com/)
- [OpenAI Chat Completions API 参考](https://platform.openai.com/docs/api-reference/chat)

## 与主核 / AI 的关系

「内部模型是 A，对外要同时说 B」在主核里对应的是客户端协议兼容层：主核内部用一套执行计划与行格式，但对不同的客户端协议要输出不同的线格式。这里的三条经验可以直接搬。

第一，**兼容的面是「减法」而非「加法」**。这一层从不去给目标上游添加字段，只删除它不认的。主核里同理：兼容性靠「不发送对方不需要的东西」，而不是「猜对方可能需要什么」。

第二，**信息在拍平中丢失，就必须显式补回**。三种工具定义拍成一种报文格式，`custom` 与 `namespace` 的语义就没了；`ToolKinds` 是那份补给。任何有损转换都该问一句「回程需要什么」，并把那部分作为状态带下去——而不是指望从报文的形状里重新推断。

第三，**「改一个设置」是否需要新开上下文，是个必须逐项判断的问题**。model 可以直接改，harness 必须 fork 线程，因为后者改变了历史被解释的方式。这类判断无法由类型系统强制，所以要么写进注释，要么写进测试；留给「实现者凭直觉」是最坏的选择。
