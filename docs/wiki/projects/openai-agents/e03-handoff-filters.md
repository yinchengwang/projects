---
project: openai-agents
feature: handoff-filters
title: 交接过滤器与提示扩展
page_type: feature
schema_version: 1
refs:
  - path: src/agents/extensions/handoff_filters.py
    symbols: [remove_all_tools, _remove_tool_types_from_input]
  - path: src/agents/extensions/handoff_prompt.py
    symbols: [RECOMMENDED_PROMPT_PREFIX, prompt_with_handoff_instructions]
---

# 交接过滤器与提示扩展

## 一句话职责

在交接发生的那一刻改写「接手方看到的历史」：要么把工具痕迹整段剔掉（`remove_all_tools`），要么把整段旧对话压成一条摘要消息（`nest_handoff_history`）；另附一份推荐提示前缀，让模型知道交接是后台行为、不该在对话里提及。

## 机制是什么

过滤器有两处来源：一是应用自定义的 `input_filter`，二是运行级开关 `nest_handoff_history`。SDK 内置的过滤器只有一个 `remove_all_tools`，它同时被 `extensions/handoff_filters` 导出；`nest_handoff_history` 与 `default_handoff_history_mapper` 也一并导出，因为它们常被组合使用。

`remove_all_tools` 对 `HandoffInputData` 的四个字段分别处理：`input_history`（若为 tuple）按类型名剔除；`pre_handoff_items` 与 `new_items` 按运行项类型剔除；`input_items`（若存在）同样剔除。剔除后的结果通过 `clone` 写回，不修改原对象。

`nest_handoff_history` 的工作方式不同：它不删东西，而是把「会被摘要的内容」和「必须原样转发的项」分开——前者进摘要，后者直接放在摘要之后，保持顺序。摘要是一条 `role="assistant"` 的消息，内容是若干编号行的转写，用 `<CONVERSATION HISTORY>` 与 `</CONVERSATION HISTORY>` 包裹，前面加一句固定前言。

第三件东西在 `extensions/handoff_prompt`：`RECOMMENDED_PROMPT_PREFIX` 是一段固定的系统提示前缀，说明「你是一个多 agent 系统的一部分，有 Agents 与 Handoffs 两种抽象，转交通过调用 `transfer_to_<agent_name>` 这类函数完成，转交在后台无缝处理，**不要在对话里提及或强调这些转交**」。`prompt_with_handoff_instructions(prompt)` 把它用两个换行接在给定提示之前。它不参与运行时，只影响模型的行为倾向。

## 怎么实现的

**`remove_all_tools` 的两套类型清单。** 运行项侧用的是「排除列表」，列了十一种：`HandoffCallItem`、`HandoffOutputItem`、`ToolSearchCallItem`、`ToolSearchOutputItem`、`ToolCallItem`、`ToolCallOutputItem`、`ReasoningItem`、`MCPListToolsItem`、`MCPApprovalRequestItem`、`MCPApprovalResponseItem`、`ToolApprovalItem`。注意连**交接项自身**也在剔除之列——所以经过这个过滤器后，接手方看不到「谁把我叫过来的」。

`input_history` 侧用的是「类型名字符串列表」，共二十六种：`function_call`、`function_call_output`、`computer_call`、`computer_call_output`、`file_search_call`、`tool_search_call`、`tool_search_output`、`web_search_call`、`mcp_call`、`mcp_list_tools`、`mcp_approval_request`、`mcp_approval_response`、`reasoning`、`code_interpreter_call`、`image_generation_call`、`local_shell_call`、`local_shell_call_output`、`shell_call`、`shell_call_output`、`apply_patch_call`、`apply_patch_call_output`、`custom_tool_call`、`custom_tool_call_output`、`hosted_tool_call`、`program`、`program_output`。

两套清单必须手工同步：一处按类，一处按 wire 层类型名。这就是为什么 `program`/`program_output` 与 `caller.type == "program"` 的项在两处都被单独处理——它们是「不可分割的托管程序转写」，删掉一半会留下无法解析的残留。

**`remove_all_tools` 也过滤 `input_items`。** 注释明确写了理由：保持链式过滤（例如先 `nest_handoff_history` 再 `remove_all_tools`）时，「不要丢弃也不要重新引入工具项」。如果只过滤 `new_items` 而放过 `input_items`，第二个过滤器就会把第一个已经裁掉的东西又带回来（因为 `input_items` 会覆盖模型输入）。

**`nest_handoff_history` 的分区算法。** 先把 `input_history` 摊平成列表并剥掉元数据；再对 `pre_handoff_items` 与 `new_items` 逐项判定「是否原样转发」，判定用的是两个不同的谓词：

- `_should_forward_pre_item`：托管程序项不转发；`role == "assistant"` 不转发；类型在「只进摘要」（`function_call`、`function_call_output`、`reasoning`）里不转发。其余转发。
- `_should_forward_new_item`：托管程序项不转发；**有任何 role 就转发**；否则按「只进摘要」类型判定。

两者的差别在 `assistant` 的处理上：轮前项里的 assistant 消息已经进摘要（否则重复），而轮内项里的 assistant 消息必须原样保留（那是刚发生的事，摘要里还没写）。

`ToolApprovalItem` 在两边都被直接 `continue` 跳过——审批占位项不参与历史。

**为什么 `reasoning` 也进摘要。** 注释给了原因：一旦其它项被摘要替换，reasoning 项就会变成孤儿（它引用的是已被替换的项），所以必须一起收进摘要，不能原样转发。

**顺序保持。** `_build_ordered_default_history` 顺序遍历规范化后的项：遇「不转发」就攒进 `pending_summary`；遇「转发」且此时 `pending_summary` 非空（或还没有任何历史项）就先产出一条摘要消息，再放这一项。收尾时若还有待摘要内容（或整段历史为空），再补一条摘要。结果形态是「摘要 → 转发项 → 摘要 → 转发项 …」，保证因果顺序不被摘要破坏。

**空历史的兜底。** 摘要内容为空时写 `(no previous turns recorded)`，而不是产出空字符串——接手方至少能看出「这是一条历史摘要，只是没有内容」。

**来源追踪。** 转发项若带可计算的摘要（`digest_input_item` 非空），会登记一条 `NestedHistoryOwnedItem`，记录它来自哪个运行项、落在历史里的第几个位置、以及摘要值。这份清单挂回 `HandoffInputData` 的私有属性 `_nested_history_owned_items`，供会话持久化时把「摘要里的项」与「原始运行项」对齐。`clone` 会保留它，`nest_handoff_history` 在返回前还会把 `input_item` 替换成深拷贝后的那一份，保证追溯指向的是真正下发的内容。

**嵌套后的字段改写。** 返回的 `HandoffInputData` 里 `input_history` 换成摘要+转发项的元组、`pre_handoff_items` 置空、`input_items` 置空（注释：「映射后的历史就是精确的模型输入，new_items 原封不动留给会话历史」）。也就是说嵌套模式走的是 `input_history` 这条通道，而不是 `input_items`。

**自定义映射器。** `default_handoff_history_mapper` 只是「把整段转写压成一条 assistant 消息」的默认实现。传入自定义 `history_mapper` 时，`nest_handoff_history` 会把「摊平后的历史 + 全部规范化项」作为转写交给它，并把 `owned_items` 留空——因为自定义映射器可能重排或丢弃项，来源追踪无法自动建立。

**转写格式的两种。** `_format_transcript_item` 分派：纯文本单行的项走 legacy 格式（`role (name): content`），其它走 JSON（`json.dumps(payload, ensure_ascii=False, default=str)`，剥掉 `provider_data`）。legacy 分支的注释指出一个细节：分隔符必须始终输出，因为裸 role 没有记录分隔符，下一次扁平化时会整条丢失。

**提示前缀的定位。** `RECOMMENDED_PROMPT_PREFIX` 解决的是「交接的用户体验问题」而不是正确性问题。交接在界面上会表现为一条函数调用与其输出，模型如果理解为「我该向用户说明我正在转接」，就会在回答里插入「正在为您转接专员」这类话术，破坏多 agent 的透明性。前缀把这条规则写死：转交在后台无缝处理，不要提及。`prompt_with_handoff_instructions` 只是 `f"{PREFIX}\n\n{prompt}"`，没有其它处理，所以它不改变提示的其余部分，也不会去重（重复调用会得到两段前缀）。

**标记可替换。** `set_conversation_history_wrappers` / `reset_conversation_history_wrappers` / `get_conversation_history_wrappers` 让应用替换 `<CONVERSATION HISTORY>` 这对标记。解析侧同时接受新旧两种前言文本（`_SUPPORTED_CONVERSATION_HISTORY_PREAMBLES`），保证旧摘要仍能被正确解析——因为「多次交接层层嵌套」时，新的嵌套需要能识别出旧的摘要并重新摊平。

**一个必须知道的边界。** `nest_handoff_history` 的文档明确写：它**不做脱敏**。转写是从 `input_history`、`pre_handoff_items`、`new_items` 三处拼的，会忽略 `input_items`；所以工具参数与输出可能留在摘要文本里。要脱敏必须在调用前清洗这三个源字段，或者在返回后清洗 `input_history`。只过滤 `input_items` 完全挡不住。

## 关键取舍

**源码事实：** `remove_all_tools` 连交接项自己都删。结果是接手方看不到「我是被谁、因为哪句话叫来的」，只看到用户与助手的对话。这对「无缝转接」的体验是好的，对「接手方需要知道上下文来源」的场景则可能信息不足。

**源码事实：** 两套类型清单（类名 + wire 类型名）靠人工维护同步。新增一种工具调用项时若漏改一处，会出现「运行项被删但 wire 层残留」或反之的错配，且不会报错。

**源码事实：** 嵌套历史的摘要是**一条 assistant 消息**，不是 system 消息。这直接影响模型的解读：它会认为「上一轮助手说过这段话」，而不是「系统给了我一段背景」。

**源码事实：** 摘要内容不做任何转义或截断。转写按行编号拼接，若某条消息内容里自带换行，JSON 格式会把换行编码进字符串，legacy 格式则可能破坏「一行一条」的结构——这也是为什么 legacy 只用于单行内容。

**源码事实：** 自定义 `history_mapper` 会丢失来源追踪（`owned_items` 为空）。这在使用会话持久化 + 嵌套历史的组合时是个真实缺口：持久化层无法判断哪些会话项是「摘要里已经包含的」。

**源码事实：** 解析侧兼容新旧前言。这说明摘要格式是会被模型侧读到并再次解析的稳定协议，而不是纯内部表示——多层嵌套交接依赖这个可解析性。

**源码事实：** 提示前缀是纯字符串常量，没有版本号、没有条件分支，也不做幂等处理。它在 `dump_component` 之外，属于应用侧拼装系统提示的职责；SDK 不会自动注入。

**架构推论：** 两个内置过滤器代表了两种截然不同的取舍：`remove_all_tools` 是「删减」（信息量下降、结构简单、可预测），`nest_handoff_history` 是「压缩」（信息保留但失真、结构复杂、需要来源追踪）。前者适合「接手方只关心对话目标」的窄任务转交，后者适合「长对话中的层级交接」。选择哪一种，取决于接手方需要的是细节还是脉络。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/extensions/handoff_filters.py` | `remove_all_tools` | 内置过滤器：按四个字段分别剔除工具相关项 |
| `src/agents/extensions/handoff_filters.py` | `_remove_tools_from_items` | 运行项侧的类型排除列表 |
| `src/agents/extensions/handoff_filters.py` | `_remove_tool_types_from_input` | wire 层类型名的排除列表 |
| `src/agents/handoffs/history.py` | `nest_handoff_history` | 摘要化入口：分区、摘要、改写三个字段 |
| `src/agents/handoffs/history.py` | `default_handoff_history_mapper` | 默认映射：整段转写压成一条 assistant 消息 |
| `src/agents/handoffs/history.py` | `_build_ordered_default_history` | 顺序保持的分区算法与来源登记 |
| `src/agents/handoffs/history.py` | `_should_forward_pre_item` / `_should_forward_new_item` | 两个不同谓词：轮前 assistant 入摘要、轮内 assistant 原样转发 |
| `src/agents/handoffs/history.py` | `_build_summary_message` | 摘要消息构造：前言 + 标记 + 编号转写 |
| `src/agents/handoffs/history.py` | `_format_transcript_item` | 单行走 legacy、其余走 JSON 的转写格式分派 |
| `src/agents/handoffs/history.py` | `set_conversation_history_wrappers` / `reset_conversation_history_wrappers` | 摘要标记的替换与还原 |
| `src/agents/handoffs/history.py` | `_SUPPORTED_CONVERSATION_HISTORY_PREAMBLES` | 新旧摘要前言兼容集，支撑多层嵌套的再解析 |
| `src/agents/handoffs/history.py` | `_SUMMARY_ONLY_INPUT_TYPES` | 只进摘要的类型：函数调用、其结果、reasoning |
| `src/agents/extensions/handoff_prompt.py` | `RECOMMENDED_PROMPT_PREFIX` | 推荐提示前缀：说明交接在后台无缝处理、不要提及 |
| `src/agents/extensions/handoff_prompt.py` | `prompt_with_handoff_instructions` | 把前缀拼到给定提示之前，不做去重或其它处理 |

## 参考

- [openai-agents-python 源码：extensions/handoff_filters.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/extensions/handoff_filters.py)
- [openai-agents-python 源码：extensions/handoff_prompt.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/extensions/handoff_prompt.py)
- [openai-agents-python 源码：handoffs/history.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/handoffs/history.py)
- [Agents SDK 文档：交接历史与过滤器](https://openai.github.io/openai-agents-python/handoffs/#input-filters)

## 与主核 / AI 的关系

这两个过滤器对应主核里「视图与摘要」两种不同的数据缩减手段：投影视图不改变信息量只改变可见范围，物化摘要则主动丢弃细节换取体积。主核两者都有（视图与统计表），差别是主核的摘要会明确记录聚合口径，这里的摘要是一段自然语言，口径隐含在转写格式里。

对 agent 工程最实用的三条推论。其一，**过滤器不是安全边界**。`remove_all_tools` 只删「工具项」，对话正文里的敏感内容照旧传下去；`nest_handoff_history` 更会把工具参数写进摘要文本。真正的脱敏必须发生在源字段上，且要注意嵌套历史绕过 `input_items` 这条捷径。其二，**嵌套摘要会累积失真**。多层交接时，每层都把上一层摘要当作普通转写再摘要一次，信息层层衰减；长链路应当限制嵌套层数，或用自定义 `history_mapper` 做结构化摘要。其三，**摘要是模型可读可写的文本**，它进入上下文后模型可能引用、误解甚至复述其中内容。把「历史摘要」当成受信数据是常见误判。
