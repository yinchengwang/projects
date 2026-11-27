---
project: smolagents
feature: parsing-and-truncation
title: 解析工具与内容截断
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/utils.py
    symbols: [parse_json_blob, truncate_content, extract_code_from_text]
---

# 解析工具与内容截断

## 一句话职责

把模型输出里「不干净的部分」剥出来：定位并解析夹在散文里的 JSON、按标记对提取代码块、以及在超长内容上做首尾保留中间丢弃的截断。

## 机制是什么

三个函数，三种不同的「不干净」：

`parse_json_blob(json_blob)` 处理「JSON 混在文本里」——返回 `(解析出的字典, JSON 之前的文本)`。
`extract_code_from_text(text, code_block_tags)` 处理「代码被标记对包裹」——返回所有匹配块拼接后的字符串，或 `None`。
`truncate_content(content, max_length)` 处理「内容太长」——首尾各留一半，中间插入一段说明。

三者的共同点是都**不把原始复杂度抛给用户**：解析失败会变成带诊断信息的 `ValueError`，截断直接返回结果。它们服务于「模型输出不可信」这个前提。

## 怎么实现的

**JSON 定位靠首尾花括号。** `parse_json_blob` 的做法是：

1. `json_blob.find("{")` 找第一个左花括号。
2. 用 `re.finditer("}")` 找出所有右花括号的位置，取**最后一个**。
3. 切片取出 `[第一个左花括号 : 最后一个右花括号 + 1]`，用 `json.loads(json_str, strict=False)` 解析。
4. 返回解析结果与 `json_blob[:first_accolade_index]`（JSON 之前的部分）。

`strict=False` 这一项很关键：它允许字符串字面量里出现裸控制字符（包括换行）。模型生成的 JSON 里常有未转义的换行，标准模式会拒，宽松模式能过。

**三种失败各有专门的错误消息。**

- 找不到任何右花括号：`IndexError` 被捕获，抛「模型输出不含任何 JSON blob」。
- `JSONDecodeError`：先检查出错位置前后三个字符是否恰好是 `"},\n"`。若是，抛出「JSON 无效：你可能在同一个动作里提供了多个工具调用。只提供一个工具调用。」——这是把一个具体的高频误用识别成专门的提示。
- 其他解析错误：抛出带三项诊断的 `ValueError`——原始错误、完整的 JSON blob、以及出错位置前后各四个字符的片段。

第三种的诊断设计值得一提：给出「出错点前后一点上下文」，比只报「第 N 字符出错」有用得多，因为模型（与读日志的人）需要看到形状才知道怎么改。

**首尾花括号的粗暴假设。** 「第一个左花括号到最后一个右花括号」这个策略假定：JSON 之外没有花括号。若 JSON 前面的散文里出现花括号（例如「使用某个占位符」这类写法），切片就会从错误位置开始，解析必然失败。

这个假设对「模型输出一段思考然后一个 JSON」这个特定形态是成立的，但它不是通用的 JSON 提取器。代码选择这个实现，是因为目标场景足够窄。

**代码块提取是纯正则。** `extract_code_from_text(text, code_block_tags)` 用开始标记、非贪婪通配、结束标记拼出模式，并开启跨行匹配。

开启跨行匹配让通配符能匹配换行（代码块必然跨行）。非贪婪保证匹配到**最近的**结束标记而不是最后一个。找出所有匹配后，用两个换行拼接并逐块 strip。

返回 `None` 而不是空字符串表示「没找到」——这个区分很重要，因为调用方用「是否为空」来判断是否要尝试下一个策略。

`code_block_tags` 是个元组（开始标记、结束标记），由调用方提供。所以这个函数对「用三个反引号加 python 还是加 py」是中立的。

**截断的对称性。** `truncate_content(content, max_length=20000)`：长度不超就原样返回；超了就取前一半、拼一段提示、再接后一半。

注意 `max_length // 2` 出现两次，所以最终长度是「两倍半长度加提示文本长度」。提示文本本身也有长度，所以结果会**略微超过** `max_length`。这是「近似上限」而非严格上限。

提示文本用了 markdown 的斜体标记，内容是「这段内容已被截断以保持在 N 字符以内」。这是给模型看的：它明确告诉模型中间被删了，避免模型以为内容本来就断了。

首尾各留一半而不是只留开头，是因为很多输出（尤其是表格、日志、代码）的关键信息在结尾——错误堆栈的最后一行、脚本的最后一步。

**三者与错误体系的关系。** `parse_json_blob` 与 `extract_code_from_text` 抛/返回的是 `ValueError` 与 `None`，**不是** `AgentError` 子类。所以在 `utils.py` 里它们与错误继承树是分开的：错误树由 agent 循环包装后再抛，而这两个函数保持纯粹的「字符串工具」，不依赖 logger。

这个分层是必要的：`parse_json_blob` 被 `get_tool_call_from_text` 调用，那条路径不应该被迫传 logger。

## 关键取舍

**源码事实：** `parse_json_blob` 用「第一个左花括号到最后一个右花括号」定位，所以 JSON 之前的散文若含花括号会失败。这是对特定输出形态的适配，不是通用提取器。

**源码事实：** 多重工具调用有专门的错误消息（识别右花括号紧跟逗号与换行的形状），说明这是被观测到的高频误用。

**源码事实：** `json.loads(..., strict=False)` 允许字符串里含裸控制字符，这对模型生成的未转义换行是必需的。

**源码事实：** `extract_code_from_text` 返回 `None` 表示未找到，但「找到且内容为空」也会拼出空字符串——两者在调用方用「是否为空」判断时无法区分。实际影响很小，因为它只是回退链的一环。

**源码事实：** `truncate_content` 的结果长度会略微超过 `max_length`，因为提示文本不计入预算。

**架构推论：** 这三个函数的存在本身就是「模型输出不可信」的证据。传统程序里「解析 JSON」只需一次库调用；有模型参与后，必须先「定位」再「宽松解析」再「给出可读的错误诊断」。一个函数的复杂度从一行涨到二十行，涨的全是与不可信输入的对抗。

**架构推论：** 错误消息里嵌诊断片段（出错点前后几字符、识别出的误用模式）是「写给模型看的错误」这一思路的体现。在 agent 循环里，解析错误会被渲染成 TOOL_RESPONSE 消息喂回模型（见 f01），所以错误文本的措辞直接影响模型下一轮能否修正。这与传统程序「错误给人看」的定位不同，是 AI 工程里的一个新约束。

**架构推论：** 截断保留首尾这个选择，反映的是「模型输出与工具输出的信息分布」。日志类输出的关键在尾部（错误在哪），结构化输出的关键在头部（字段名与第一批数据）。只留头会丢结论，只留尾会丢结构。各留一半是唯一能兼顾的简单方案，代价是中间被删后语义可能不连续——所以提示文本里必须明说。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/utils.py` | `parse_json_blob` | 定位并宽松解析夹在文本里的 JSON |
| `src/smolagents/utils.py` | `extract_code_from_text` | 按标记对提取所有代码块 |
| `src/smolagents/utils.py` | `parse_code_blobs` | 多策略回退的代码提取入口 |
| `src/smolagents/utils.py` | `truncate_content` | 首尾保留的对称截断 |
| `src/smolagents/utils.py` | `MAX_LENGTH_TRUNCATE_CONTENT` | 截断默认上限常量 |

## 参考

- [smolagents 源码：utils.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/utils.py)
- [Python 文档：json 模块的 strict 参数](https://docs.python.org/3/library/json.html#json.loads)
- [smolagents 文档：Code agents](https://huggingface.co/docs/smolagents/conceptual_guides/intro_agents)

## 与主核 / AI 的关系

主核里最接近的是「查询文本的容错解析」：用户输入可能有语法错误，解析器要给出「第几个字符附近出错」这样的定位信息，而不是一句「语法错误」。`parse_json_blob` 的三种错误消息正是同一职能——尤其那条「你可能提供了多个工具调用」，相当于把一类语法错误识别成了语义提示。

与 CrewAI 的四级降级解析（`json.loads` → `ast.literal_eval` → `json5` → `json_repair`）相比，smolagents 只有一级：定位切片后 `json.loads(strict=False)`。差别在于双方的容错策略方向不同：CrewAI 靠**多个解析器**尝试不同语法宽容度，smolagents 靠**精确定位**缩小待解析范围。前者能救回更多畸形输入（单引号、尾逗号），后者在 JSON 干净但散文含噪时更可靠。两者可以叠加，但 CrewAI 已经引入了两个第三方依赖，smolagents 保持零依赖。

对 AI 工程最实用的一条是错误消息要「写给模型看」。当解析失败的消息会被喂回模型时，它就不再只是日志，而是一条提示。所以措辞要包含：错在哪（定位片段）、为什么错（识别出的模式）、怎么改（正确的形状）。`parse_code_blobs` 就是这方面的样板——它有多段精心构造的提示，其中一段专门针对「你想返回最终答案但写法不对」这个场景，直接给出正确示例。任何会被回灌给模型的错误，都值得按这个标准写。

另一条是截断必须自报。`truncate_content` 在中间插入的那句说明看似只是注释，实际防止了一整类误判：模型拿到首尾拼起来的内容，会以为中间本就没有东西，于是基于残缺数据给出结论。明确标注「这里被删了」让模型至少有机会选择「不基于此下结论」。这个做法在所有「上下文预算受限」的场景里都适用。
