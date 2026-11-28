---
project: goose
feature: large-response
title: 大响应的落盘处理
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/agents/large_response_handler.rs
    symbols: [process_tool_response, write_large_text_to_file]
---

# 大响应的落盘处理

## 一句话职责

在工具响应回到模型之前拦一道：**单个文本块超过阈值就写进临时文件，只把一个「文件在哪、多大」的引用交回模型**，让模型自己用别的工具去读——因为一个 200 万字符的工具输出会直接把上下文窗口撑爆。

## 机制是什么

模块很小（265 行，其中一半是测试），三个东西：

| 项 | 值 / 作用 |
|---|---|
| `DEFAULT_LARGE_TEXT_THRESHOLD` | `200_000` 字符 |
| `max_tool_response_size()` | 读配置项 `GOOSE_MAX_TOOL_RESPONSE_SIZE`，缺省用上面的常量 |
| `process_tool_response(response)` | 唯一的处理入口，逐内容块判断 |
| `write_large_text_to_file(content)` | 落盘并返回路径 |

处理入口的签名就很说明问题：`Result<CallToolResult, ErrorData> -> Result<CallToolResult, ErrorData>`——它**不改错误路径**，只改成功响应里的内容块。

## 怎么实现的

**阈值是可配置的，但配置项是「读取失败即回落」而不是「必须配置」。** `Config::global().get_param::<usize>("GOOSE_MAX_TOOL_RESPONSE_SIZE").unwrap_or(DEFAULT_LARGE_TEXT_THRESHOLD)`——一个 `unwrap_or` 把「没配」与「配错类型」都归到默认值。于是在没有这个环境变量的机器上，行为是确定的 20 万字符。

**计数用的是 `chars().count()` 而不是 `len()`。** 这一点在两个地方都出现：判断是否超阈值时、以及写给模型的消息里（`was larger ({} characters)`）。用字符数而不是字节数，是因为这条限制的目的是**控制上下文 token 量**——而 token 大致与字符数成正比，与字节数不成比例（一段中文的字节数约是字符数的三倍）。用 `len()` 会让同样的「字符预算」在非 ASCII 文本上提前触发三倍。

这是一个常被忽略的细节：**限制单位要与被保护资源（上下文窗口）的单位一致**。

**逐块处理而不是整体替换，这是本页的关键设计。** `process_tool_response` 遍历 `result.content`，对每一块：

- `content.as_text()` 返回 `Some` 且超阈值 → 落盘 + 换成引用消息；
- `content.as_text()` 返回 `Some` 但未超阈值 → **原块透传**；
- `as_text()` 返回 `None`（图片等）→ **原块透传**。

也就是说同一份响应里可以同时有「小文本保留」「大文本落盘」「图片不动」。测试 `test_mixed_content_handled_correctly` 正是验证这个组合：三块（小文本、大文本、图片）处理后仍是三块，只有中间那块变成了消息。

**为什么必须逐块？** 因为一个工具可以返回多个内容块（`CallToolResult::success(vec![...])`），而其中可能既有需要给模型看的短摘要、又有几兆的原始数据。整体替换会把短摘要也一起丢掉，整体透传则会撑爆上下文。

**落盘失败时不丢内容，而是换一条警告并附上全文。** `Err(e)` 分支构造的消息是：

> `Warning: Failed to write large response to file: {e}. Showing full content instead.`

然后**把原始全文附在消息后面**。

这个降级方向是有代价的：它把上下文撑爆的风险放了回来。但替代方案（静默丢弃）更糟——用户会看到模型「什么都没拿到」而不知道为什么。也就是说这里的选择是**「宁可撑爆也要让内容可追责」**，并且用 `Warning` 前缀让模型与日志都知道发生了什么。

**文件名有固定的前缀与后缀，且不加密、不可预测性来自 `tempfile`。** `tempfile::Builder::new().prefix("goose_mcp_response_").suffix(".txt").tempfile()`。

- 前缀 `goose_mcp_response_` 让这些文件在临时目录里可被识别与清理；
- 后缀 `.txt` 让模型知道这是文本（它可能会用 `cat` / `grep` 之类的工具去读）；
- 唯一性由 `tempfile` 保证（`test_large_response_files_have_unique_paths` 验证两次调用路径不同）。

**`keep()` 这个调用决定了文件的生命周期。** `let (_, file_path) = file.keep()?;`——`tempfile` 的默认行为是**作用域结束时删除**，而 `keep()` 明确放弃删除责任，把文件留在磁盘上。

这是一条必要但要紧的取舍：**文件必须在返回后仍然存在**，否则模型拿到路径去读时已经没了。代价是**没有任何人负责删除**——这些文件会一直留在系统临时目录里，直到 OS 清理或用户手动清。源码里没有清理逻辑，也没有对「累计落盘了多少」做任何限制。

从测试可以看到团队对这个问题的态度：测试自己 `fs::remove_file`，而生产代码没有。也就是说「这些文件是临时文件系统的一部分，交给 OS 回收」是接受的设计。

**权限是 owner-only，而且有专门的测试守着。** `test_large_response_file_is_owner_only` 断言 `mode & 0o077 == 0`——即 group 与 other 位全为 0。

这条测试的存在说明团队意识到**落盘的内容可能是敏感数据**（工具输出可能含密钥、个人数据、内部日志）。而实现上这个权限不是本模块设置的，而是 `tempfile` 在 Unix 上以 `O_EXCL` + `0600` 创建文件的默认行为。测试的作用是**把这个隐含依赖固定下来**——如果哪天换成别的落盘方式，测试会失败。

这是本页最值得学的一处：**安全属性来自依赖库的默认行为，所以要用测试把它钉住**。不写这个测试，任何一次「改用更简单的写文件方式」的重构都会静默地让文件变成 0644。

**错误路径原样返回，不做任何处理。** `Err(e) => Err(e)`——工具调用失败时不会因为「大响应」这条逻辑而改变。这是一个正确的边界：这条机制的职责是控制成功响应的体积，而错误信息的体积由另一套机制管（错误通常短，且截断错误会丢诊断信息）。

**消息文案的结构是为模型设计的。** 三部分：

1. **发生了什么**：`The response returned from the tool call was larger (N characters)`;
2. **东西在哪**：`stored in the file`；
3. **能用它做什么**：`which you can use other tools to examine or search in: <path>`。

第 3 部分是本页最实用的一句。如果一个模型只看到「太长了，存文件了」，它可能就放弃了；明确写出「用其它工具检查或搜索」等于告诉它**下一步该怎么做**。这与 `d01` 里「提示文案带 finding id」是同一类做法：错误/降级消息要包含可行动的信息。

**路径用 `to_string_lossy()` 而不是要求 UTF-8。** `Ok(file_path.to_string_lossy().to_string())`——一个非 UTF-8 的临时目录路径（理论上可能）会被宽松转换而不是让整个流程失败。这是「路径只在消息里出现、不需要往返解析」的前提下的合理选择。

## 关键取舍

**源码事实：** 阈值默认 200_000 **字符**（`chars().count()`），可由 `GOOSE_MAX_TOOL_RESPONSE_SIZE` 覆盖；读取失败或类型不对都回落默认。

**源码事实：** 逐内容块处理：小文本原块透传、大文本换成引用消息、非文本块（图片）不动。一份响应里三种处理可以共存。

**源码事实：** 落盘失败时附上全文并加 `Warning:` 前缀——宁可冒撑爆上下文的风险，也不静默丢内容。

**源码事实：** 文件用 `tempfile` 的固定前缀 `goose_mcp_response_` 与 `.txt` 后缀；`keep()` 放弃自动删除，于是文件在返回后仍存在，但**没有任何人负责清理**。

**源码事实：** Unix 上文件权限是 owner-only，而这来自 `tempfile` 的默认行为，不是本模块设置——`test_large_response_file_is_owner_only` 专门把这个隐含依赖钉住。

**源码事实：** 错误路径原样返回；路径用 `to_string_lossy()` 宽松转换。

**源码事实：** 消息包含「发生了什么 / 在哪 / 能用其它工具检查或搜索」三段，第三段是给模型的行动指引。

**架构推论：** 这条机制的定位是**「上下文的溢出阀」**，而它的设计处处体现「阀」的语义：触发条件是一条与上下文单位（字符）对齐的阈值；动作不是丢弃而是**把数据移到一个模型能主动去取的地方**；并且它愿意在无法移动时退回「撑爆但要可见」。

代价也很清楚：**落盘的文件是无人管理的**。这在单次会话内没问题（模型用完就走），但长跑或大量工具调用会累积。这类「用文件系统当扩展内存」的做法在 agent 系统里很常见，而它隐含的前提是「临时目录会被操作系统回收」——一个在容器里可能不成立的前提。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/agents/large_response_handler.rs` | `process_tool_response` | 逐内容块判断并替换超大文本 |
| `crates/goose/src/agents/large_response_handler.rs` | `write_large_text_to_file` | 落盘到临时文件并 `keep()` 保留 |
| `crates/goose/src/agents/large_response_handler.rs` | `max_tool_response_size` | 配置项读取，失败回落默认阈值 |
| `crates/goose/src/agents/large_response_handler.rs` | `DEFAULT_LARGE_TEXT_THRESHOLD` | 200_000 字符的默认阈值 |
| `crates/goose/src/agents/large_response_handler.rs` | `test_large_response_file_is_owner_only` | 把「所有者独占权限」这个隐含依赖钉住的测试 |

## 参考

- [goose 源码：large_response_handler.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/large_response_handler.rs)
- [tempfile 文档：Builder 与 keep](https://docs.rs/tempfile/latest/tempfile/struct.Builder.html)
- [goose 文档：环境变量与配置](https://block.github.io/goose/docs/guides/environment-variables/)
- [MCP 规范：工具结果内容块](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

这条机制与主核里的「大字段溢出到外部存储」完全同构：数据库在行里放不下一个超大字段时会溢出到单独的页或文件，行里只留一个指针。两者的共同判断是**「主结构要小，大对象另存」**，区别在于数据库的溢出有事务与回收保证，而这里没有。

对 agent 工程的直接价值有三条。其一，**限制单位要与被保护资源对齐**。上下文窗口按 token 计，而 token 与字符数成正比、与字节数不成比例，所以用 `chars().count()` 而不是 `len()`——用错的后果是同样的预算在中文文本上三倍提前触发。其二，**降级要保留可追责性**。落盘失败时附全文加警告，比静默丢弃好；模型至少知道自己拿到了什么，日志也有痕迹。其三，**依赖库提供的安全属性要用测试钉住**。文件权限是 0600 这件事完全来自 `tempfile` 的默认行为，一旦有人换实现就静默消失——而已经依赖这个属性的人是看不到的。
