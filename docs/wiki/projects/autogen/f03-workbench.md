---
project: autogen
feature: workbench
title: 工作台与工具集合调度
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/tools/_workbench.py
    symbols: [Workbench, ToolResult, call_tool]
  - path: python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py
    symbols: [StaticWorkbench, call_tool]
---

# 工作台与工具集合调度

## 一句话职责

把「一组共享状态的工具」当成一个对象来管：列工具、调工具、启停、重置、存状态都由工作台统一负责，调用方只需要一个 `call_tool(name, arguments)` 就能覆盖背后可能是本地函数、也可能是远端会话的整个工具集。

## 机制是什么

`Workbench` 与 `Tool` 的差别不在「能不能执行」，而在**谁掌握生命周期与可见性**。单个工具是不可变的 schema 快照加一个 `run`；工作台则明确承认「工具集合会变」——`list_tools` 的 docstring 写「列表可能是动态的，内容可能在工具执行后改变」。这一句是整个抽象的存在理由：MCP 服务器就是这样，连上之后工具清单取决于服务端，某个工具执行后清单还可能变。

所以工作台的接口比工具宽得多，共七个抽象方法：`list_tools`、`call_tool`、`start`、`stop`、`reset`、`save_state`、`load_state`。另外基类提供了 `__aenter__`/`__aexit__`，分别转调 `start`/`stop`——上下文管理器不是新增能力，只是启停的语法糖。

`StreamWorkbench` 在 `Workbench` 上加一个 `call_tool_stream`。

调用结果用 `ToolResult` 承载，而不是像 `Tool` 那样折成字符串。`ToolResult` 有 `type`（固定 `"ToolResult"`）、`name`、`result`（`List[ResultContent]`）、`is_error`。`result` 的元素是判别联合 `TextResultContent | ImageResultContent`，靠 `type` 字段区分。`to_text(replace_image=None)` 负责把富结果压成字符串：文本直接拼，图片在 `replace_image` 为 `None` 时写成 `[Image: <base64>]`，给了替换串则用替换串。

## 怎么实现的

**抽象层的形态。** `Workbench(ABC, ComponentBase[BaseModel])`，`component_type = "workbench"`。它只声明契约，不给任何默认实现——连 `start`/`stop` 都不给空实现，强制每个后端想清楚自己的生命周期。`call_tool` 的签名固定为 `(name, arguments=None, cancellation_token=None, call_id=None)`，四个参数都可空，所以实现必须自己处理「没给 token」「没给参数」的情况。

**`StaticWorkbench` 的构造。** 接收 `tools: List[BaseTool]` 与可选的 `tool_overrides: Dict[str, ToolOverride]`。构造时遍历 overrides 建一张**反向映射** `_override_name_to_original`（改名后的名字 → 原名），并做两道冲突检查：改名后的名字不能与任何已有工具名冲突（`ValueError`）；两个工具不能改成同一个名字（`ValueError`，文案带上两个原名）。未改名（`override.name` 为空或等于原名）的条目不进反向映射。

**`list_tools` 的覆盖合成。** 遍历工具，若该工具在 overrides 里有条目，就**重新拼一个 `ToolSchema` 字典**：`name` 取 `override.name if 不是 None else 原名`，`description` 同理（此时回落的是 `original_schema.get("description", "")`，注意是 `.get` 带默认空串，而不是直接下标）；`parameters` 与 `strict` 若原 schema 里有就原样复制。没有 override 的条目直接返回原 schema 对象（**不是拷贝**）。

**`call_tool` 的六步。** ①名字先过反向映射还原成原名；②线性查找 `tool.name == original_name`，找不到返回 `ToolResult(name=请求时给的名字, result=[TextResultContent(content=f"Tool {name} not found.")], is_error=True)`——注意返回的是 `ToolResult` 而不是抛异常，错误被**降级成数据**；③补默认 `CancellationToken()`；④`arguments` 为空则换成 `{}`；⑤`asyncio.ensure_future(tool.run_json(...))` 后 `token.link_future`，`await` 得到输出，`tool.return_value_as_string` 转文本；⑥任何 `Exception` 被捕获，走 `_format_errors` 转成字符串并置 `is_error=True`。

**错误格式化。** `_format_errors` 递归展开 `ExceptionGroup`（Python 3.11+，代码里用 `hasattr(builtins, "ExceptionGroup")` 做兼容判断），把每个子异常各自格式化后拼接；普通异常写成 `f"{str(error)}\n"`。最后 `strip()` 去尾换行。

**状态存取。** 状态模型类名是 `StateicWorkbenchState`（源码里就是这个拼写，`Static` 拼错成 `Stateic`），字段是 `type` 与 `tools: Dict[str, Mapping[str, Any]]`。`save_state` 遍历工具调 `tool.save_state_json()`，按工具名存；`load_state` 解析后按名字匹配回去，只对**状态里存在的工具**调 `load_state_json`。

**启停与重置。** 三个都是 `return None` 的空实现——静态工作台的「资源」就是那几个工具的实例，没有外部句柄要开。

**流式变体。** `StaticStreamWorkbench(StaticWorkbench, StreamWorkbench)` 只覆写 `call_tool_stream`。它的做法很值得注意：若工具实现了 `StreamTool`，就逐项 `yield`，但**总是滞后一项**（`if previous_result is not None: yield previous_result; previous_result = result`），流结束后 `previous_result` 才是最终结果，用来算 `return_value_as_string` 并作为最后一个 `ToolResult` 产出。这个「滞后一项」的设计让最终结果不被中间 `yield` 提前发出去。若工具不是 `StreamTool`，就退化成普通 `run_json` 一次拿到结果。流中途抛异常时，先把已攒下的 `previous_result`（若存在）yield 出去，再 yield 一条错误 `ToolResult` 并 `return`。

**配置往返。** `StaticWorkbenchConfig` 是 `tools: List[ComponentModel]` 加 `tool_overrides`。`_to_config` 对每个工具 `dump_component`；`_from_config` 用 `BaseTool.load_component(tool)` 逐个装回。于是工作台的配置是一棵嵌套配置树，加载时递归走白名单校验。

## 关键取舍

**源码事实：** `call_tool` 把「工具不存在」和「执行失败」都变成 `is_error=True` 的 `ToolResult`，不抛异常。所以调用方无法用 `try/except` 区分「名字错了」与「工具内部炸了」——两者都只能靠读 `result` 里的文本。

**源码事实：** `list_tools` 在无 override 时返回**原 schema 对象的引用**。调用方若原地改动这个 dict，会污染工具自身的 schema。有 override 时才是新字典（且 `parameters`、`strict` 仍是浅引用）。

**源码事实：** 冲突检查只在 `__init__` 里做一次。构造之后再 `append` 到 `tools` 列表（`_tools` 是公开可改的）可以绕过所有校验，得到重名工具或多个工具映射到同一 override 名的状态。

**源码事实：** `StateicWorkbenchState` 的拼写错误让这个名字既不在文档里也不好搜。它同时影响 `save_state` 产出的 `type` 字段值（`"StaticWorkbenchState"` 是**正确拼写**，写在 `Literal` 里；类名拼错但字面量对）。

**源码事实：** `StaticStreamWorkbench` 的「滞后一项」意味着流式调用里**每个 `yield` 都不是最新结果**，而是上一项。对只想拿最终结果的调用方没影响，但对想看进度的一方，进度会晚一拍。

**源码事实：** `call_tool_stream` 里工具查找**不做 override 反向映射**——它直接用 `tool.name == name` 找原始名字。所以对 `StaticStreamWorkbench` 用了 `tool_overrides` 改名后，用改名去调流式接口会找不到工具。这是与 `call_tool` 的实质不一致。

**架构推论：** 工作台把「工具集合」与「工具调用」拆成两个时点：`list_tools` 在每轮模型调用前取一次清单，`call_tool` 在模型给出调用后才发生。这正好匹配 MCP 这类动态工具源——清单可以随时刷新，而某次调用只需要名字和参数。代价是两次调用之间工具可能消失，所以 `call_tool` 必须把「找不到」当正常路径而非异常路径处理，这正是它降级成 `ToolResult` 的原因。

**架构推论：** `ToolResult` 用判别联合而非单一字符串，是为了让工作台能返回图片。但代价是每个消费者都要自己调 `to_text()` 才能进模型的 `FunctionExecutionResultMessage`——类型富了，接线累一点。

**架构推论：** 工作台是组件，所以整棵工具树可 dump。`tool_overrides` 单独存在而不是改工具本身，是为了保持工具的**不可变 schema**：同一个工具实例可以被两个工作台以不同名字暴露，底层仍是同一份实现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/tools/_workbench.py` | `Workbench` | 工作台抽象：列工具、调工具、启停、重置、状态 |
| `python/packages/autogen-core/src/autogen_core/tools/_workbench.py` | `Workbench.call_tool` | 统一调用入口，四参数含取消令牌与调用 id |
| `python/packages/autogen-core/src/autogen_core/tools/_workbench.py` | `ToolResult` | 富结果：名字、判别联合内容、错误标志 |
| `python/packages/autogen-core/src/autogen_core/tools/_workbench.py` | `ToolResult.to_text` | 富结果压成文本，图片可替换 |
| `python/packages/autogen-core/src/autogen_core/tools/_workbench.py` | `TextResultContent` / `ImageResultContent` | 两种结果内容，按 `type` 判别 |
| `python/packages/autogen-core/src/autogen_core/tools/_workbench.py` | `StreamWorkbench` | 加流式调用接口 |
| `python/packages/autogen-core/src/autogen_core/tools/_workbench.py` | `Workbench.__aenter__` | 上下文管理器转调 `start` |
| `python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py` | `StaticWorkbench` | 固定工具集实现，支持改名覆盖 |
| `python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py` | `StaticWorkbench.call_tool` | 名字还原、查找、执行、错误降级为结果 |
| `python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py` | `StaticWorkbench.list_tools` | 合成 override 后的 schema 清单 |
| `python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py` | `StaticWorkbench.save_state` | 逐个工具存状态，按名字归档 |
| `python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py` | `StaticWorkbench._format_errors` | 递归展开 `ExceptionGroup` 成文本 |
| `python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py` | `StaticStreamWorkbench` | 流式变体，滞后一项产出中间结果 |
| `python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py` | `StaticWorkbenchConfig` | 嵌套 `ComponentModel` 的工具清单与覆盖 |

## 参考

- [AutoGen 源码：tools/_workbench.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/tools/_workbench.py)
- [AutoGen 文档：Workbench (and MCP)](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/workbench.html)
- [Model Context Protocol 规范](https://modelcontextprotocol.io/introduction)

## 与主核 / AI 的关系

主核的表空间有「动态目录」问题：一张表可以在连接期间被 `DROP`，而另一会话的查询还持有它的 OID。解决办法是快照与失效消息——先按名字解析成稳定对象，对象消失时通过失效广播让所有持有者感知。工作台面临同一类问题但**放弃了这种严谨性**：`list_tools` 拿到名字，`call_tool` 再按名字线性查找，中间没有任何句柄或版本。名字消失只表现为一条「Tool not found」的结果文本。对一个会去调外部服务、且外部服务本身可能挂掉的系统来说，这种「尽力而为」是合适的；但如果工作台后面接的是强一致资源，就该在 `call_tool` 里做一次显式的存在性校验。

对 AI 工程来说，工作台的价值集中在一个词：**动态清单**。单工具模式的 `tools=[...]` 是编译期固定的列表，而模型每轮都要看一遍工具清单；工作台把这个清单变成一次异步调用，于是清单可以按会话状态变（MCP 连上才知道有什么）、按执行结果变（某个工具用了之后才解锁别的）。写 agent 时若发现「工具集需求随对话演进」，这是唯一自然的抽象。

其次要记住 `ToolResult` 与 `Tool.return_value_as_string` 的分工。工作台内部保留富类型，但**通向模型的唯一桥是 `to_text()`**。图片要进模型，正确做法是通过 `ImageResultContent` 加多模态消息，而不是把 base64 塞进 `FunctionExecutionResultMessage` 的字符串字段——后者会让模型看到一大段无法解析的文本，白烧 token。

最后一个实践细节：`call_tool` 的 `arguments` 是 `Mapping[str, Any]`，模型给出的是 JSON 字符串，所以调用方必须先 `json.loads`。这一步没有在工作台里做，因为不同来源的参数形态可能不同（`tool_agent_caller_loop` 走的是另一条 `Tool` 路径，那里由 `ToolAgent` 负责解析）。接线时漏了这一步，得到的是 `TypeError` 而不是友好的错误文本。
