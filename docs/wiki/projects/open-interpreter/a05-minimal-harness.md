---
project: open-interpreter
feature: minimal-harness
title: 最小 harness 与消息转换
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/core/src/harness/minimal.rs
    symbols: [MINIMAL_SYSTEM_PROMPT, build_request, build_tools]
  - path: codex-rs/core/src/harness/minimal.rs
    symbols: [flush_pending_tool_calls, convert_message_content, minimal_tool_output_content]
---

# 最小 harness 与消息转换

## 一句话职责

`minimal` 是这批壳里最省的一个：它不带任何上游工具的个性特征，只做「把内部 Responses 风格的对话历史改写成一份干净的 Chat Completions 报文」这一件事——两句话的系统提示、一个拍平的工具清单、一套把三种工具调用统一成 `tool_calls` 的装配规则，以及一条把内部内容项映射成 chat 内容项的转换函数。

## 机制是什么

要看清它的定位，得先区分两个极端：

| | 其他壳（如 `claude-code`） | `minimal` |
|---|---|---|
| 系统提示 | 复刻上游的完整提示（数千行常量） | 两句话 |
| 工具集 | 复刻上游的工具名与描述 | 用内部工具的现成定义 |
| 请求头 | 复刻 beta/计费/版本等一批硬编码头 | 不加额外头 |
| 消息形态 | 复刻上游的历史形态 | 内部历史的标准改写 |

也就是说 `minimal` 是**「不模拟任何人的形状，只做一次干净的协议转换」**。它是这条主张的对照组：如果「复刻形状」是提升效果的手段，那 `minimal` 就是「不复刻」时的基线。这也解释了它在目录表里那句描述——`"Use a minimal shell-oriented tool harness."`（shell 导向是最小可行集的自然结果：只有一个 `bash` 类工具能覆盖大部分操作）。

系统提示的全文就是两个句子，没有分节、没有规则清单：

```
const MINIMAL_SYSTEM_PROMPT: &str = "You are an expert software engineer working in the user's current directory.\nUse the tools to investigate the codebase and complete the task. Read code before changing it, match existing conventions, and verify your work when possible.";
```

三句指令分别是「先看再改」「跟随既有约定」「尽量验证」——这是模型在没有详细行为规范时也大概率会遵守的三条。省掉其余部分带来的不是「效果略降」而是**形状最干净**：模型的上下文里除了用户任务几乎没有别的。

## 怎么实现的

**`build_request` 的骨架是四步，且顺序固定。**

```
pub(crate) fn build_request(prompt: &Prompt, model_info: &ModelInfo, effort: Option<ReasoningEffortConfig>)
    -> Result<(Value, ToolKinds), serde_json::Error>
{
    let mut messages = vec![json!({ "role": "system", "content": MINIMAL_SYSTEM_PROMPT })];
    messages.extend(build_messages(prompt.get_formatted_input())?);
    let tools = build_tools(&prompt.tools)?;
    let tool_kinds = prompt.tools.iter().map(|tool| (tool.name().to_string(), ToolOutputKind::Function)).collect();

    let mut request = json!({
        "model": model_info.slug,
        "messages": messages,
        "stream": true,
        "stream_options": { "include_usage": true },
        "tools": tools,
    });
    ...
}
```

逐项看：

1. **系统提示是手工塞进 `messages` 的第一条**，不是某个 `instructions` 字段。Chat Completions 没有独立的 system 槽位，所以它必须变成一条消息；而内部模型（Responses 风格）本来是把它放在请求的 `instructions` 里的。这一步是两种协议之间最基础的不对称。
2. **`prompt.get_formatted_input()` 而不是原始 input**。这说明拿到的是「已经格式化过的」条目序列——格式化（把技能、上下文片段注入到消息里）发生在更早的层，`minimal` 只负责协议转换，不参与提示构造。这是它与那些「复刻提示」的壳的又一处分工。
3. **`tool_kinds` 在这里是全 `Function`**：`build_tools` 只保留 `ToolSpec::Function`，所以映射表里自然只有一种输出种类。对比通用 compat 层的三态映射，`minimal` 之所以能退化成单态，是因为它在 `build_tools` 里就把别的类型丢掉了——后面会看到这个取舍的代价。
4. **`stream: true` 与 `include_usage: true` 是写死的**。前者让上游返回 SSE，后者要求末尾带上用量块。硬编码而不是可选，说明这个壳只按一种模式工作；非流式的场景由通用层合成 SSE 兜住（见兼容那一页），`minimal` 不参与。

**`thinking` 只在一种模型配置下出现，且默认是「开」。**

```
if model_info.reasoning_control == ReasoningControl::ThinkingToggle {
    request["thinking"] = json!({
        "type": if matches!(effort, Some(ReasoningEffortConfig::None)) { "disabled" } else { "enabled" },
    });
}
```

三个细节：

- **判据是模型的 `reasoning_control` 而不是用户显式要求**。只有声明为 `ThinkingToggle` 的模型才会收到这个字段，其他模型根本不出现该键——这是「按模型能力裁剪请求」而不是「按用户偏好」。
- **默认是 `enabled`**：`effort` 为 `None`（用户没指定）时走 `else` 分支。有一条测试守着这个方向：`thinking_toggle_model_sends_enabled_by_default` 断言无 effort 时得到 `{"type": "enabled"}`，而 `thinking_toggle_model_sends_disabled_for_none_effort` 断言 `Some(ReasoningEffortConfig::None)` 时得到 `disabled`。
- 这个 `"thinking"` 键是 Kimi/Anthropic 风格的扩展名，不是 OpenAI 的 `reasoning_effort`。于是同一个请求打到 OpenAI 兼容但不认这个键的上游时必须被删掉——通用兼容层确实按 host 做这类删除。**换句话说：`minimal` 负责「按模型能力加字段」，兼容层负责「按上游脾气删字段」，两者是分工而非重复。**

**`build_messages` 是这一页最核心的一段，它有三个并行的「待发」缓冲。**

```
fn build_messages(items: &[ResponseItem]) -> Result<impl Iterator<Item = Value>, serde_json::Error> {
    let mut messages = Vec::new();
    let mut pending_tool_calls = Vec::new();
    let mut pending_tool_call_content = String::new();
    let mut pending_reasoning_content = String::new();
    ...
}
```

为什么需要三个缓冲而不是「逐条转换、逐条 push」？因为 Chat Completions 的形状约束：**一条 assistant 消息既承载文本也承载 `tool_calls`，且它必须排在对应的 tool 结果消息之前**。而内部的 Responses 历史把这些拆成了独立的条目：

- `ResponseItem::Message{role:"assistant"}` —— 模型的文本
- `ResponseItem::FunctionCall` / `CustomToolCall` / `LocalShellCall` —— 各个工具调用
- `ResponseItem::Reasoning` —— 推理内容（某些上游要回显）
- `ResponseItem::FunctionCallOutput` 等 —— 工具结果

于是装配逻辑是：**遇到工具调用就先攒着，遇到第一条「不是工具调用」的条目时才把它们连同攒下的文本与推理一起 flush 成一条 assistant 消息**。三个缓冲各有归属：

- `pending_tool_calls` → 最终消息的 `tool_calls` 数组；
- `pending_tool_call_content` → 最终消息的 `content`；
- `pending_reasoning_content` → 最终消息的 `reasoning_content`。

**「文本被并进工具调用」这条规则有一个具体的必要性。**

```
"assistant" => {
    if let Some(message_content) = convert_message_content(content) {
        if message_content.as_str().is_some_and(str::is_empty) { continue; }
        if !pending_tool_calls.is_empty() {
            append_message_text(&mut pending_tool_call_content, &message_content);
            continue;
        }
        let mut message = json!({ "role": "assistant", "content": message_content });
        attach_pending_reasoning_content(&mut message, &mut pending_reasoning_content);
        messages.push(message);
    }
},
```

三支的含义：

- **空文本被跳过**（`is_empty` 后 `continue`）。一条只有空串的 assistant 消息在 Chat 里没有意义，而某些模型会产出一条。
- **若已有待发工具调用，文本并入那个缓冲**，不单独成条。这正是「一条 assistant 消息承载文本 + tool_calls」的落实——否则模型会看到「说了一句话」和「调了工具」两条互相独立的 assistant 消息，而它自己从没那样说过。
- **否则独立成条**，并顺手把待发的推理内容挂上（`attach_pending_reasoning_content`）。

`append_message_text` 的细节值得一提：

```
fn append_message_text(output: &mut String, content: &Value) {
    let text = content.as_str().map(str::to_string).unwrap_or_else(|| content.to_string());
    if text.is_empty() { return; }
    if !output.is_empty() { output.push('\n'); }
    output.push_str(&text);
}
```

两处：**当内容不是字符串时用 `content.to_string()`**（因为 `convert_message_content` 对多段内容返回的是数组），以及**已有内容时用换行连接**。后者意味着如果历史里有两条 assistant 文本被并进同一条工具调用消息，它们会用换行拼起来而不是互相覆盖。

**`user` 与 `developer` 的角色处理是两条几乎相同的支，但有一条关键的跳过。**

```
"user" => {
    if is_contextual_user_message_content(content) { continue; }
    flush_pending_tool_calls(...);
    if let Some(message_content) = convert_message_content(content) {
        messages.push(json!({ "role": "user", "content": message_content }));
    }
},
"developer" => {
    flush_pending_tool_calls(...);
    if let Some(message_content) = convert_message_content(content) {
        messages.push(json!({ "role": "user", "content": message_content }));
    }
},
```

两点：

1. **`developer` 被降级成 `user`**。Chat Completions 没有 developer 角色（那是 Responses 的特性），而降级而不是丢弃是保内容的选择。有一条测试专门守它：`developer_messages_are_preserved_as_user_messages`，输入一条 `role: "developer"` 的 `<skills_instructions>` 文本，断言它作为 user 消息出现。
2. **`user` 有两分支不同：跳过「上下文类」内容**。`is_contextual_user_message_content` 判定的是那些内部注入的上下文片段（工作目录、时间、提示附加段等）。它们在内部模型里是独立条目，而在 `minimal` 里**整条丢弃**。

第 2 点是一个很容易被误读的取舍：这不是「漏了」，而是「这个壳的信息论立场」——`minimal` 要把上下文压到最小，所以把上下文注入整块排除。它也解释了为什么这个壳最适合用来做基线对照：它的报文里除了系统提示、用户消息、工具调用与工具结果，几乎没有第四类东西。

顺带一个必然的后果：`is_contextual_user_message_content` 是这里唯一的「内容分类」依赖。若上游某天改了这个判定，`minimal` 的报文会随之多出或少掉若干条消息，而它自己毫无察觉。

**三种工具调用被统一成 `tool_calls` 条目，其中 `LocalShellCall` 需要手工翻译。**

```
ResponseItem::FunctionCall { name, arguments, call_id, .. } => pending_tool_calls.push(json!({
    "type": "function", "id": call_id,
    "function": { "name": name, "arguments": arguments },
}),
```

`FunctionCall` 几乎零转换——名字与参数原样搬，加一个 `"type": "function"` 与 `"id"`。

`CustomToolCall` 则需要把自由格式的 `input` 包进一个对象：

```
"arguments": json!({ "input": input }).to_string(),
```

因为 Chat 的 `arguments` 必须是字符串化的 JSON，而 custom 工具的输入本来不是 JSON 对象。

`LocalShellCall` 最复杂，两个细节值得记：

```
let call_id = call_id.clone().or_else(|| id.as_ref().map(ToString::to_string)).ok_or_else(|| {
    serde_json::Error::io(std::io::Error::new(std::io::ErrorKind::InvalidData, "local_shell history item missing call id"))
})?;
let arguments = match action {
    LocalShellAction::Exec(exec) => json!({
        "command": exec.command,
        "timeout": exec.timeout_ms.map(|timeout_ms| timeout_ms / 1000),
    }).to_string(),
};
pending_tool_calls.push(json!({
    "type": "function", "id": call_id,
    "function": { "name": "bash", "arguments": arguments },
}));
```

- **`call_id` 有两条来源**：显式的 `call_id` 优先，缺失时退回 `id`。两者都缺才报错，且这是整个 `build_messages` **唯一的错误路径**——`Result` 的存在主要就是为了这一处。
- **工具名被硬编码成 `"bash"`**，`timeout_ms` 被除以 1000 换成秒。

第二点是这个壳最「有主张」的一处转换：内部的 `local_shell` 调用在 chat 报文里必须变成某个具名函数，而它选了 `bash`。这个选择与 `MINIMAL_SYSTEM_PROMPT` 里「shell-oriented」的定位一致，也与 `build_tools` 只发出内部工具定义这件事互补——内部工具集里本来就有一个 `bash` 类工具，于是历史里的 `local_shell` 调用与新的 `bash` 工具调用在报文里同名，模型看到的是一贯的。

**工具结果的装配点在「工具结果之前」先 flush。**

```
ResponseItem::FunctionCallOutput { call_id, output, .. } => {
    flush_pending_tool_calls(&mut messages, &mut pending_tool_calls, &mut pending_tool_call_content, &mut pending_reasoning_content);
    messages.push(json!({ "role": "tool", "content": minimal_tool_output_content(output), "tool_call_id": call_id }));
}
```

顺序是「先 flush 那条带着 tool_calls 的 assistant，再 push tool 消息」。这是 Chat 协议对 adjacency 的要求；若顺序反了，上游会拒收或行为异常。`CustomToolCallOutput` 走同样的两步。

一个必然的细节：**同一组多个工具调用只会被 flush 一次**。因为 `flush_pending_tool_calls` 在开头就 `if pending_tool_calls.is_empty() { return; }`，第一个结果消息触发 flush 后缓冲被 `take()` 清空，后续结果消息调用 flush 时是空操作。于是「一个 assistant 消息带 N 个 tool_calls + N 条 tool 消息」这个正确形状自动成立。

**`flush_pending_tool_calls` 与推理的挂载。**

```
fn flush_pending_tool_calls(messages, pending_tool_calls, pending_tool_call_content, pending_reasoning_content) {
    if pending_tool_calls.is_empty() { return; }
    let mut message = json!({
        "role": "assistant",
        "content": std::mem::take(pending_tool_call_content),
        "tool_calls": std::mem::take(pending_tool_calls),
    });
    attach_pending_reasoning_content(&mut message, pending_reasoning_content);
    messages.push(message);
}

fn attach_pending_reasoning_content(message: &mut Value, pending_reasoning_content: &mut String) {
    if pending_reasoning_content.is_empty() { return; }
    message["reasoning_content"] = json!(std::mem::take(pending_reasoning_content));
}
```

三处 `std::mem::take` 把缓冲取走并置空——这既是「清空以便下次」也是「所有权转移而不复制」。而 `content` 取走后可能是空串：此时消息里会有一个 `"content": ""`。

**推理内容有两条汇聚路径。**

`ResponseItem::Reasoning` 分支把多段文本拼进缓冲：

```
for entry in content {
    let text = match entry { ReasoningItemContent::ReasoningText { text } | ReasoningItemContent::Text { text } => text };
    pending_reasoning_content.push_str(text);
}
```

注意它**不插入分隔符**，与 `append_message_text` 的换行策略不同——因为推理内容是要「回显」给上游的连续块，而 assistant 文本是多条消息合并（需要分隔）。这个不对称是有依据的。

挂载点有两处：独立的 assistant 文本消息、以及 flush 出的工具调用消息。有一条测试 `reasoning_content_is_passed_back_on_assistant_tool_call_messages` 守第二条——即「带 tool_calls 的那条 assistant 消息也必须带上 `reasoning_content`」。这条测试的存在说明了这个字段在某些上游（DeepSeek 一类）上是**必需的回显**，而不只是可选信息。

**被丢弃的条目是一个长长的显式清单。**

```
ResponseItem::ToolSearchCall { .. } | ResponseItem::ToolSearchOutput { .. } | ResponseItem::AgentMessage { .. }
| ResponseItem::WebSearchCall { .. } | ResponseItem::ImageGenerationCall { .. } | ResponseItem::Compaction { .. }
| ResponseItem::CompactionTrigger { .. } | ResponseItem::ContextCompaction { .. } | ResponseItem::AdditionalTools { .. }
| ResponseItem::ConfigurationUpdate { .. } | ResponseItem::Other => {}
```

十一种。写成一个空分支而不是 `_ => {}`，是这个仓库的一条硬规矩（AGENTS.md 里明确要求「能穷尽就穷尽、避免通配臂」）。实际收益也很具体：`ResponseItem` 将来加一个变体时，这里会**编译报错**，强迫作者决定「它该被转换还是该被丢弃」。若用 `_`，新变体会静默消失——而静默消失在这个壳里意味着「模型看不到某类历史」。

**`build_tools` 只保留 `Function`，其余静默丢弃。**

```
for tool in tools {
    let ToolSpec::Function(ResponsesApiTool { name, description, parameters, .. }) = tool else { continue; };
    converted.push(json!({ "type": "function", "function": { "name": name, "description": description, "parameters": parameters } }));
}
```

`let ... else { continue }` 而不是错误：一个非函数工具在这个壳下就是「不存在」，不构成失败。这与 `build_messages` 里 `LocalShellCall` 缺 id 时的行为形成对照——那里缺 id 是**数据损坏**（无法表达一个必须有 id 的东西），这里只是「类型不适用于本协议」。

**`convert_message_content` 的三条返回形态。**

```
fn convert_message_content(content: &[ContentItem]) -> Option<Value> {
    let parts = content.iter().map(|item| match item {
        ContentItem::InputText { text } | ContentItem::OutputText { text } => json!({ "type": "text", "text": text }),
        ContentItem::InputImage { image, detail } => chat_image_content_part(image, *detail),
        ContentItem::InputAudio { .. } => json!({ "type": "text", "text": "[audio content omitted]" }),
    }).collect::<Vec<_>>();

    match parts.as_slice() {
        [] => None,
        [single] if single.get("type").and_then(Value::as_str).is_some_and(|kind| kind == "text") => single.get("text").cloned(),
        _ => Some(Value::Array(parts)),
    }
}
```

三个结果：

- **空内容 → `None`**，调用方据此「不产生这条消息」；
- **单段文本 → 直接返回那个字符串**（不是 `[{type:text,text}]` 数组）；
- **其余 → 内容项数组**。

单段文本折叠成裸字符串是一个兼容性决定：很多 OpenAI 兼容上游对 `content` 的类型是宽松的，但**字符串形态是接受面最广的一种**。用最窄的形状发出去，才能让最严格的端点也收。

`InputAudio` 被替换成文字占位而不是丢弃或报错：`"[audio content omitted]"`。这样模型知道「这里原本有音频」而不是看到一条空消息——一个信息更完整的降级。

**`minimal_tool_output_content` 与它形状同构，但多两个变体。**

```
fn minimal_tool_output_content(output: &FunctionCallOutputPayload) -> Value {
    if let Some(text) = output.text_content() { return json!(text); }
    if let Some(content_items) = output.content_items() { /* 与上面同构：InputText / InputImage / InputAudio / InputVideo / EncryptedContent */ }
    json!(output.to_string())
}
```

三点差异：

1. **第一条快路径是纯文本**：`text_content()` 有值就直接用，不必走内容项数组。大部分工具结果是纯文本，这条路径省掉一次遍历。
2. **内容项多了两种降级**：`InputVideo` 与 `EncryptedContent` 都变成占位文本（`"[video content omitted]"`、`"[encrypted content omitted]"`）。
3. **最后一档是 `json!(output.to_string())`**：无法解构的输出被整个 `to_string()` 化。这是「宁可给模型一段无法解析的文本，也不丢内容」的选择。

同样是「单段文本折叠成裸字符串」，有测试 `view_image_tool_output_preserves_image_payload` 与 `view_image_tool_output_preserves_file_id` 守图像那条路径——一个 `view_image` 类工具返回图片时，输出必须保留 `image_url`（或 `file_id`）结构而不是被压成文本。

**`chat_image_content_part` 的两支对应两种图像引用。**

```
ImageReference::Inline { image_url } => {
    let mut image_url_value = json!({ "url": image_url });
    if let Some(detail) = detail { image_url_value["detail"] = json!(detail); }
    json!({ "type": "image_url", "image_url": image_url_value })
},
ImageReference::File { file_id } => json!({ "type": "file", "file": { "file_id": file_id } }),
```

`Inline` 走 Chat 的 `image_url` 形态，`detail` 只在有值时才加（不给上游发一个默认值，让它的默认生效）；`File` 走 `file` 形态并携带 `file_id`——即「图像已经在服务端，只给引用」，避免把字节重新上传。

`convert_message_content` 与 `minimal_tool_output_content` **共用**这个函数，于是「用户消息里的图」与「工具输出里的图」用同一种表达。这是正确的一致性：上游看到的两种图都长一样。

## 关键取舍

**源码事实：** 系统提示是两句话的常量（`MINIMAL_SYSTEM_PROMPT`），且被手工塞成 `messages` 的第一条，因为 Chat Completions 没有独立的 system 槽位。

**源码事实：** `build_request` 里 `stream: true` 与 `stream_options.include_usage: true` 是硬编码；`thinking` 字段仅在模型 `reasoning_control == ThinkingToggle` 时出现，且 `effort` 未指定时默认为 `enabled`。

**源码事实：** `tool_kinds` 在这个壳里是单态的（全部 `Function`），因为 `build_tools` 用 `let ... else { continue }` 只保留 `ToolSpec::Function`，其余静默丢弃。

**源码事实：** `build_messages` 用三个并行缓冲（`pending_tool_calls` / `pending_tool_call_content` / `pending_reasoning_content`）把「文本 + 工具调用 + 推理」合成一条 assistant 消息；首条非工具调用条目触发 flush，工具结果消息之前也触发 flush。

**源码事实：** assistant 文本在有待发工具调用时被**并入**而不是独立成条；空文本被跳过；多条文本合并时用换行连接。

**源码事实：** `developer` 角色被降级成 `user`；`user` 里被 `is_contextual_user_message_content` 判为上下文的内容**整条丢弃**——这是这个壳与其它壳最本质的差别（不注入上下文）。

**源码事实：** `LocalShellCall` 被翻译成名为 `"bash"` 的函数调用，`timeout_ms` 除以 1000 转秒；`call_id` 缺失时退回 `id`，两者都缺才报错，且这是 `build_messages` 唯一的错误路径。

**源码事实：** 十一种 `ResponseItem` 变体以显式空分支丢弃（无通配臂），于是新增变体会编译报错而不是静默消失。

**源码事实：** `convert_message_content` 对单段文本折叠成裸字符串、空内容返回 `None`；音频降级为文字占位。`minimal_tool_output_content` 与之同构，另加视频与加密内容的占位，最后兜底 `output.to_string()`。

**架构推论：** 这个壳的价值一半在「少做什么」。丢掉上下文注入、丢掉非函数工具、丢掉十一种历史条目，换来的是报文里只有四类东西：system、user、assistant（含 tool_calls）、tool。这样的报文对便宜模型最不容易出错，也正是它作为对照基线的原因。

**架构推论：** 「单段文本折叠成裸字符串」是兼容层的通用技巧：能发最窄类型时就发最窄类型。它省掉的是「某些上游对数组形态的 content 处理不同」这一类无法穷举的差异。

**架构推论：** 把协议转换的错误路径压缩到一处（缺 call id），意味着其余所有异常都被「降级或丢弃」处理。这是一个明确的取向：转换层不该因为一条历史条目的形态怪异就整轮失败——因为它面对的是可能由别的版本写下的历史。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/core/src/harness/minimal.rs` | `MINIMAL_SYSTEM_PROMPT` | 两句话的系统提示，手工塞为第一条消息 |
| `codex-rs/core/src/harness/minimal.rs` | `build_request` | 组装报文：system + messages + tools + 流式/用量 + 条件 thinking |
| `codex-rs/core/src/harness/minimal.rs` | `build_tools` | 只保留 `ToolSpec::Function`，其余静默丢弃 |
| `codex-rs/core/src/harness/minimal.rs` | `build_messages` | 三缓冲装配：文本 + tool_calls + reasoning 合成一条 assistant |
| `codex-rs/core/src/harness/minimal.rs` | `flush_pending_tool_calls` | 唯一产出带 `tool_calls` 的 assistant 消息的地方 |
| `codex-rs/core/src/harness/minimal.rs` | `convert_message_content` | 内容项 → chat 内容项；单段文本折叠、空内容返回 `None` |
| `codex-rs/core/src/harness/minimal.rs` | `minimal_tool_output_content` | 工具输出 → chat content，含四种降级占位与文本兜底 |
| `codex-rs/core/src/harness/minimal.rs` | `chat_image_content_part` | 图像引用 → `image_url` 或 `file`，消息与工具输出共用 |
| `codex-rs/core/src/harness/minimal.rs` | `attach_pending_reasoning_content` | 把待发推理挂成 `reasoning_content`，空则不加 |

## 参考

- [open-interpreter 源码：core/src/harness/minimal.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/core/src/harness/minimal.rs)
- [open-interpreter 文档：Harness](https://www.openinterpreter.com/docs/terminal/harness)
- [OpenAI Chat Completions API：消息与工具调用](https://platform.openai.com/docs/api-reference/chat/create)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)

## 与主核 / AI 的关系

「把内部历史改写成外部报文」与主核里「把内部行格式改写成客户端协议行」是同一件事，而这里的核心经验是**装配顺序而不是字段映射**。字段改名是机械的，难的是「协议要求 A 与 B 必须相邻」「一条消息只能承载一类东西」这类结构约束——它们无法从类型定义里读出来，只能靠对上游行为的了解。

对 AI 工程的实际意义有三条。

第一，**协议转换的正确性主要取决于顺序，而不是字段**。三个缓冲、一次 flush、工具结果前先 flush——这套结构的存在就是为了满足「assistant 的 tool_calls 必须紧邻其结果」这条隐含约束。任何跨协议转换都该先问「目标协议对相邻性有什么要求」。

第二，**「丢弃」比「猜测」安全，但必须显式列举**。十一种条目的丢弃写成了穷尽的空分支，于是新增变体会编译报错。若用通配臂，模型会静默地看不到某类历史，而这种缺失在输出质量上只表现为「莫名其妙地不听话」。

第三，**转换层应当只有一个错误路径**。这里只有「缺 call id」会失败，其余全部降级或丢弃。这样转换层面对任何由旧版本写下的历史都不会整轮失败——而如果它动不动就报错，一个兼容层反而会成为最脆弱的一环。
