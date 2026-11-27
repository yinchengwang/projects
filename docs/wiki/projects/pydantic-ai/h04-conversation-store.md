---
project: pydantic-ai
feature: conversation-store
title: 会话存储与冲突检测
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/conversation_store.py
    symbols: [ConversationStore, InMemoryConversationStore, ConversationConflict]
  - path: pydantic_ai_slim/pydantic_ai/conversation.py
    symbols: [Conversation]
---

# 会话存储与冲突检测

## 一句话职责

把一个多轮对话持久化成**只增不减的日志**：每条消息存一次、带一个 entry id 与父指针，于是「保存」变成「追加尾巴」、「改写历史」变成「从某个旧点开新分支」，并发写入靠一个不透明 revision 令牌检出。

## 机制是什么

`Conversation` 是跨轮携带的单位，四个字段各自回答一个「手工拼 `message_history=` 会丢掉什么」的问题：

- `messages`：历史本身，形式与 `message_history=` 相同。
- `usage`：跨所有运行累计的 `RunUsage`。丢掉它的后果写在文档里：每轮的 `UsageLimits` 预算都从零开始。而且它**不能从消息反推**——每条 `ModelResponse` 上有 token 数与 `requests`，但 `tool_calls` 没有；被裁剪过的历史也不再包含被丢掉轮次的成本。
- `conversation_id`：关联这个对话全部运行的标识，也是存储键。
- `deferred_tool_requests`：上次运行停下来时正在等待的那些工具调用。文档点明这**无法从消息重建**：消息只显示哪些调用没被回答，不显示每种需要什么答案、也不带它延期时附带的元数据。

外加一个 `revision`（来自存储的哪一版）与 `new_message_index`（存储端还没有的那批从哪开始）。

`ConversationStore` 是 ABC，把存储拆成五个原语（读记录、写记录、读 entry、写 entry 相关辅助、删记录），再在其上实现 `load` / `save` / `branch` / `fork` / `list_conversations` / `update` / `delete`。`InMemoryConversationStore` 是参考实现。

`ConversationConflict` 是冲突异常，继承 `UserError`。

## 怎么实现的

**存储形态是「链」而不是「快照」。** `ConversationEntry` 有 `parent_id`，`ConversationRecord` 有 `head_id`（最新一条的 id，无消息时为 `None`）。保存一条新消息就是把一条 entry 的 `parent_id` 指向当前 head，然后把 head 移过去。这个形态让「只追加」天然成立：已写入的 entry 永不需要修改。

**`save` 的两条路径。** 若 conversation 知道自己从哪开始是新的（`new_message_index`，由 `load` 或运行结果设置），就直接从那里截断当作待存。否则退回「与已存历史逐条比对」：用 `==` 比较，从第一条不同开始开分支（把旧尾巴记进 `record.branches`），保留两版共同的那条作为分叉点。

文档里有一句关键的细化：**运行在设 `new_message_index` 之前会先把发给它的历史做「清理」**（例如把存储的一条输出工具结果与它新加的提示合并），所以 `new_message_index` 之前的消息可能既对得上被清理前的那几条、也对得上同样数量的已存消息。这解释了为什么这条路径的判断比「长度差」复杂。

**另一个刻意的精度限制：`==` 比较意味着「消息相等性忽略的字段」的改动不会被存。** 比如文件的 `media_type` 或 `identifier` 改了但其它不变，则这个改动不会进存储。这是用「消息相等」做变更检测的直接后果，文档明确列出以免被当成 bug。

**revision 检查在 `save` 的最前面，且只读一次。** 代码先 `expected_revision = conversation.revision`，再拿 `messages = list(conversation.messages)`，然后读记录比对 revision 与 scope。这三个「先读一次」的动作各有理由：一个并发保存同一个对象会推进它的 revision，而在本次保存等待存储返回期间消息也可能被追加。若不先快照，检查用的值与实际写入的值会不一致。

**冲突的三种来源被列全。** revision 不匹配、从未加载过的会话但存储里已有同 ID、scope 不同。`ConversationConflict` 的文档还给了处置建议：重新加载再决定；**保存冲突后想重跑这一轮，必须先确认重放它的工具调用是安全的**；而 `update` 因 `expected_title` 不匹配而冲突时不要重试。这三条把「冲突后该做什么」变成可执行的动作，而不是笼统的「重试」。

**`branch` 把「改写历史」变成显式操作。** 它接受 `at`（保留前 N 条）或 `from_entry`（从某条 entry 继续，可以是当前历史里的，也可以是它曾离开的分支里的），二者必须**恰好给一个**。被留下的尾巴会记进 `record.branches`，于是之后还能用 `from_entry` 分叉回去。

分支之后有三处一致性处理：`deferred_tool_requests` 只在 head 未变时保留（否则那些等待的调用在分支世界里不存在了）；新 head 若恰好在 `branches` 里则从 branches 移除（不能既是当前头又是离开的分支）；revision 换成新值。

**`fork` 与 `branch` 的区别被文档讲清了。** branch 移动原对话的头（改写它），fork 复制出一个新对话（原对话不动）。这个区分对应两种真实需求：「我要改这一轮」与「我想试试另一种走法但保留原样」。

**`save` 用 `scope` 做强隔离。** 一次保存时若会话已存过，则 scope 必须与已存的一致；若没存过，用传入的 scope。任何读取或修改方法都可传 `scope`，且文档明确：以另一个 scope 存过的会话会被当作「存储里没有」。这不是权限模型，是**分区键**——让一个后端服务多租户而不必自己做 ID 前缀。

**`load` 的 `limit` 只影响读，不影响写。** 文档说明：只读最后 N 条便于只要近期轮的运行，但**保存结果仍会追加到完整存储的会话上**。还有一句提醒：这个窗口可能从一轮中间开始（比如拿到一条工具结果却漏了它的调用），而这种历史在发请求前会被修复。

**`update` 与 `save` 是两条不同的写路径。** `title`、`metadata`、`scope` 这类「记录级」信息只能通过 `update` 改，且文档明确「save 永不改动它们」。`update` 还支持 `expected_title` 校验，冲突时不建议重试——因为它改的是元信息，重试会把别人的改名覆盖掉。

**历史一旦写入就只增。** 文档对 `ConversationStore` 的总述把这条讲得很直白：「存储的会话只会增长：一次保存会追加运行新增的消息，**即使某个能力裁剪或改写了那次运行被发送的历史**」。要重新生成或编辑早先的轮次就 branch。这条设计保证了「你可以随时回到任何旧状态」，代价是存储只会增长。

**顺手记录文件归属。** `Conversation.usage` 之外，entry 上还有 `content_file_digests`（序列化时记录工具结果里的文件）。这暗示「被裁剪/分支后的历史里，文件内容去哪了」是个需要显式记录的问题。

## 关键取舍

**源码事实：** 存储是链式 entry 日志，不是整份快照。这让追加是 O(新增) 而非 O(整份)，也让分支成为「指向旧节点」而不是「复制一段历史」。

**源码事实：** 保存永不删除任何已写内容。裁剪与改写只影响**发送给模型的历史**，不影响到存储；要改历史必须 branch。

**源码事实：** 变更检测用消息 `==`。因此「消息相等性忽略的字段」的改动不会被持久化，这是设计约束而非 bug。

**源码事实：** revision 在 `save` 开头只读一次，消息列表也先快照。这两个动作都是为了在 `await` 期间不被并发修改影响判断。

**源码事实：** branch 时若非 head 变化则清掉 `deferred_tool_requests`。待决调用与具体历史位置绑定，换了位置就不该保留。

**源码事实：** `scope` 是分区键而非权限模型，且「以其它 scope 存过的会话视为不存在」。这让多租户隔离不依赖应用自己拼 ID。

**源码事实：** `load(limit=N)` 只限制读，`save` 仍追加到完整会话。这避免了「为了省 token 读窗口，结果把存储覆盖成窗口」这类事故。

**架构推论：** 这套存储的形状（只增日志 + 父指针 + 分支记录 + revision）与版本控制系统的对象模型同源：提交不可变、分支是引用、并发靠乐观并发控制。代价是它把「怎么改历史」的学习成本转给了使用者——必须理解 branch / fork / update 三种操作的语义边界，才能不把「保存一个被改过的会话」当成「更新历史」用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/conversation.py` | `Conversation` | 跨轮携带的会话单位：消息、累计用量、ID、待决请求、revision |
| `pydantic_ai_slim/pydantic_ai/conversation.py` | `Conversation.usage` | 累计用量；`tool_calls` 与已裁剪轮次的成本无法从消息重建 |
| `pydantic_ai_slim/pydantic_ai/conversation.py` | `Conversation.deferred_tool_requests` | 暂停运行所等待的调用，含元数据，不能从消息重建 |
| `pydantic_ai_slim/pydantic_ai/conversation.py` | `Conversation.revision` | 与存储版本对应的不透明令牌，只用于相等比较 |
| `pydantic_ai_slim/pydantic_ai/conversation_store.py` | `ConversationStore` | 五个存储原语之上实现 load/save/branch/fork/update/delete |
| `pydantic_ai_slim/pydantic_ai/conversation_store.py` | `ConversationStore.save` | 只追加增量；带 revision 与 scope 检查 |
| `pydantic_ai_slim/pydantic_ai/conversation_store.py` | `ConversationStore.branch` | 从旧点继续，留下的尾巴记进 branches |
| `pydantic_ai_slim/pydantic_ai/conversation_store.py` | `ConversationStore.fork` | 复制出新对话，原对话不动 |
| `pydantic_ai_slim/pydantic_ai/conversation_store.py` | `ConversationStore.load` | `limit` 只限制读取，不影响后续保存的完整性 |
| `pydantic_ai_slim/pydantic_ai/conversation_store.py` | `ConversationEntry` | 链式 entry：父指针 + 消息 + 文件摘要记录 |
| `pydantic_ai_slim/pydantic_ai/conversation_store.py` | `ConversationRecord` | 记录级信息：head、用量、待决请求、scope、分支、revision、标题与元数据 |
| `pydantic_ai_slim/pydantic_ai/conversation_store.py` | `ConversationConflict` | 冲突异常，含「什么情况该重试、什么情况不该」的指引 |

## 参考

- [pydantic-ai 源码：conversation_store.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/conversation_store.py)
- [pydantic-ai 源码：conversation.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/conversation.py)
- [Pydantic AI 文档：持久化](https://ai.pydantic.dev/persistence/)

## 与主核 / AI 的关系

这个存储模型与主核的「WAL + 快照」共用一条原则：已写下的内容不可变，新状态靠追加，想回到旧状态靠「从某个旧点继续」。差别是主核的分支是故障恢复产物，这里的 branch 是**面向应用的常规操作**——用户可以主动重生成某一轮。

对 agent 工程的直接价值有三条。其一，**会话必须带用量与待决请求一起存**。只存 `message_history` 会静默失去配额累计与审批上下文，表现为「第二轮的预算从零开始」和「恢复后不知道该批准什么」。其二，**冲突后是否重试取决于动作类型**。追加式保存冲突可以重载再试，但前提是重放那一轮的工具调用安全；元信息更新冲突则不该重试。其三，**改历史要走 branch 而不是覆盖**。所有「我保存一个裁剪过的会话」的尝试都会与「存储只增」的设计相抵触——正确姿势是保留原历史，从想改的那条之前开分支。
