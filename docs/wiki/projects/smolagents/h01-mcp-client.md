---
project: smolagents
feature: mcp-client
title: MCP 客户端与工具接入
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/mcp_client.py
    symbols: [MCPClient]
---

# MCP 客户端与工具接入

## 一句话职责

把 MCP 服务器的工具接进框架：校验传入的传输配置、按需装 `mcpadapt` 依赖、在构造时就建立连接，然后把远端工具列表直接当作本地 `Tool` 列表交给 agent。

## 机制是什么

`MCPClient` 不是自己实现 MCP 协议，而是**桥接**：它持有 `mcpadapt.MCPAdapt`（连接管理）与 `SmolAgentsAdapter`（格式适配），构造时 `self.connect()` 立即建立连接。

构造参数三样：

- `server_parameters`：可以是 `StdioServerParameters` 实例（走子进程 stdin/stdout）、`dict`（走 HTTP 类传输）、或这两者的**列表**（一次连多个 MCP 服务器）。
- `adapter_kwargs`：原样转发给 `MCPAdapt`。
- `structured_output`：是否启用结构化输出支持（`outputSchema`、`structuredContent`、JSON 解析兜底）。

关键设计是**「一次调用连上，工具列表从此固定在内存里」**。`get_tools()` 的 docstring 写得诚实：「目前总是返回会话创建时已有的工具，将来版本才会返回调用时刻服务端新增的工具」。

## 怎么实现的

**传输协议的校验与默认值。** 若 `server_parameters` 是 dict，先取 `transport`。没有就默认 `"streamable-http"` **并写回字典**（`server_parameters["transport"] = transport`）——注意这是就地修改调用方传入的对象。

然后校验它是否在 `{"sse", "streamable-http"}` 里，不在则 `ValueError`，消息列出两个支持值。

`sse` 在 docstring 里被标注为「legacy HTTP+SSE transport (deprecated)」，但仍在白名单里——保留了兼容但明确标注了 deprecated。

`StdioServerParameters` 的分支不做任何校验，直接透传。这是合理的：它是 mcp SDK 的类型化参数对象，构造时已经校验过。

**依赖的延迟导入。** `from mcpadapt.core import MCPAdapt` 与 `from mcpadapt.smolagents_adapter import SmolAgentsAdapter` 放在 `try` 里，`ModuleNotFoundError` 转成「请安装 `smolagents[mcp]` extra」的提示。

这与 e03 的本地模型、g02 的远程后端是同一套模式：可选依赖在构造期检查，错误消息给出可执行的安装命令，而不是让 `ImportError` 裸奔到用户面前。

**`structured_output` 的三态与强制选择。** 参数类型是 `bool | None`，默认 `None`。为 `None` 时：

1. 发一条 `FutureWarning`，说明「当前默认 `False`，1.25 版本会改成 `True`」，并给出两个选项（显式 `True` 用新行为、显式 `False` 用旧行为），还附了文档链接。
2. **然后把它设成 `False`**。

所以 `None` 的语义是「你还没选，我先按旧的来，但警告你」——这是「默认值变更」的标准过渡手法：先让现存的隐式依赖暴露出来，几个版本后再翻转默认。

**连接即构造。** `__init__` 最后调 `self.connect()`，后者只有一行：`self._tools = self._adapter.__enter__()`。

所以「构造对象」与「建立连接」是同一个动作。docstring 因此专门提示：「若不用上下文管理器，强烈建议用 `try ... finally` 确保连接被清理」。

这个选择的后果是：`MCPClient(...)` 可能因连接失败而抛异常（构造失败），而失败后没有任何对象可以清理——需要 `MCPAdapt` 自己在 `__enter__` 失败时回滚。

**上下文管理器返回工具列表而不是自身。** `__enter__` 的实现是 `return self._tools`。所以 `with MCPClient(...) as tools:` 里 `tools` 直接就是 `list[Tool]`，不是 client。

这是一个便利但有代价的设计：用户拿不到 client 对象，若想在 `with` 块内 `disconnect()` 或访问其他成员就不可能。注释也承认了这一点：「因为 init 里已经 connect 过，此时 mcp_client 已经是连接状态」。

**`__exit__` 的三参透传。** `__exit__(exc_type, exc_value, exc_traceback)` 把三个参数原样转给 `disconnect()`，后者再原样转给 `self._adapter.__exit__(...)`。所以异常上下文能传到 MCPAdapt 内部——这让适配层有机会区分「正常退出」与「异常退出」并做不同清理。

**`get_tools()` 的防御。** 若 `self._tools is None` 抛 `ValueError`，消息是「无法从 MCP 服务器取到工具，在访问 `tools` 前先跑 `mcp_client.connect()`」。

但按构造逻辑，`_tools` 在 `__init__` 结束时必定已赋值（除非 `connect()` 抛了异常，那种情况下对象根本没构造完）。所以这个检查实际上防的是「有人手动把 `_tools` 置空」或「子类化了但没调 connect」。它是一个防御性检查，不是正常路径的一部分。

## 关键取舍

**源码事实：** 构造时就连接，因此「构造失败」与「连接失败」是同一件事，对象生命周期与连接生命周期完全重合。

**源码事实：** `server_parameters` 为 dict 且缺 `transport` 时，代码会**就地写入**这个字典。所以同一个 dict 对象被用于构造多个 `MCPClient` 时，第二次会看到第一次写入的 `transport`。

**源码事实：** `with MCPClient(...) as tools` 得到的是工具列表而不是 client 对象，所以上下文管理器内无法访问 client 的其他方法。

**源码事实：** `structured_output=None` 既不启用新行为也不报错，而是发警告后按 `False` 执行。这是「默认值翻转」的过渡期方案。

**源码事实：** 工具列表在连接时一次性取回并缓存，`get_tools()` 不刷新。所以服务端新增工具在本次会话内不可见——docstring 明确说这是待改进项。

**架构推论：** 这个类的全部价值在于「把 MCP 的工具模型翻译成框架的工具模型」。它没有一行协议代码，因为协议由 `mcpadapt` + `mcp` SDK 承担，格式适配由 `SmolAgentsAdapter` 承担。这类「薄桥接」类的正确形态就是这样：只做参数校验、依赖检查、生命周期转发，不重复实现下层能力。

**架构推论：** 「构造即连接」在简单用法下体验最好（三行代码就能用上 MCP 工具），但它把「资源获取」藏进了构造函数，违反了「构造函数不该做 I/O」的常规建议。代价是用户必须记住 `try/finally` 或用 `with`——docstring 里那句强调正是对这个代价的补偿。

**架构推论：** 支持「列表形式一次连多个服务器」意味着 `MCPClient` 是「多路复用」的：一个对象对应 N 条连接，工具列表是 N 个服务器的并集。这让「同时用多个 MCP 服务」的代码非常短，但也带来了工具重名的隐患——代码里没有任何重名检测或改名逻辑。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/mcp_client.py` | `MCPClient` | MCP 连接与工具列表桥接 |
| `src/smolagents/mcp_client.py` | `MCPClient.__init__` | 校验传输、检查依赖、构造即连接 |
| `src/smolagents/mcp_client.py` | `MCPClient.connect` | 调用适配层 `__enter__` 取工具列表 |
| `src/smolagents/mcp_client.py` | `MCPClient.disconnect` | 透传异常上下文给适配层 `__exit__` |
| `src/smolagents/mcp_client.py` | `MCPClient.get_tools` | 返回缓存的工具列表，空时抛错 |
| `src/smolagents/mcp_client.py` | `MCPClient.__enter__` | 上下文管理器直接返回工具列表 |

## 参考

- [smolagents 源码：mcp_client.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/mcp_client.py)
- [MCPAdapt 文档](https://github.com/grll/mcpadapt)
- [Model Context Protocol 规范](https://modelcontextprotocol.io/introduction)

## 与主核 / AI 的关系

主核里最接近的是「外部数据源适配器」：连接一个外部系统，把它暴露的表与函数映射成本地可用的对象，连接的生命周期由适配器管理。这里做的是同一件事，但被适配的不是表结构而是「工具」——带名称、描述、输入 schema 的远端函数。

与 CrewAI 的 MCP 客户端相比（crewai 的 g01 页），两边的复杂度分布完全不同。CrewAI 自己实现了 MCP 客户端：要处理传输、事件循环、超时分类、重试退避、凭据脱敏，代码近八百行。smolagents 把这一切推给 `mcpadapt`，自己的代码只有 170 行。前者的收益是「能精细控制协议层行为」，后者是「维护成本极低」。这个对比很说明问题：同一个协议，可以「自己实现」也可以「薄桥接」，取决于你是否需要控制协议细节。

与 g02 的远程执行器对照，两种「让远端能力本地可用」的机制方向相反：远程执行器把**本地代码送出去**，MCP 客户端把**远端工具接进来**。前者的安全关注点是沙箱能否被逃逸，后者是「远端工具的描述与返回值能否被信任」。

对 AI 工程最实用的一条是那个 `structured_output=None` 的过渡方案。做默认值变更时，最糟的做法是直接改默认——依赖旧行为的代码会静默改变。这里的做法是「发 FutureWarning、说明新旧两个选项、给出目标版本、仍按旧值执行」，让用户有时间显式选择。这个模式值得在任何公共 API 上复用。

另一条是 `__enter__` 返回工具列表而非 self 的权衡。它让最常见的使用方式短到一行，但代价是「进了 with 块就够不到 client」。若一个对象的**主要产物**是某个集合而不是对象本身，返回产物是合理的；但这时应该保留一个方法（如 `get_tools()`）让不带 with 的用法也能取到——代码里正是这么做的，两条路并存。
