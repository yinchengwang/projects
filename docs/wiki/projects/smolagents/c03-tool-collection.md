---
project: smolagents
feature: tool-collection
title: 工具集合与流水线工具
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/tools.py
    symbols: [ToolCollection, PipelineTool]
---

# 工具集合与流水线工具

## 一句话职责

`ToolCollection` 把「一批工具」当成一个可整体加载的单元（来自 Hub 收藏夹或 MCP 服务器），`PipelineTool` 则把「预处理器 → 模型 → 后处理器」这条三段流水线包装成一个普通工具——两者分别代表「工具的批量来源」与「工具的一种特殊形态」。

## 机制是什么

`ToolCollection` 极简：只有 `tools: list[Tool]` 一个字段和两个类方法。

- `from_hub(collection_slug, token, trust_remote_code)`：取一个 Hub 收藏夹，**只筛 `item_type == "space"`** 的条目，逐个走 `Tool.from_hub`。注释解释了为什么只取 Space：收藏夹里也可以放模型与数据集，但它们不是工具。
- `from_mcp(server_parameters, trust_remote_code, structured_output)`：这是一个 `@contextmanager`，用 `MCPAdapt` + `SmolAgentsAdapter` 把 MCP 服务器的工具集适配成 smolagents 工具，`yield` 出集合，退出时由上下文管理器负责关闭连接（注释点明会另起一个线程跑 asyncio 事件循环）。

两个入口都要求 `trust_remote_code=True`，理由与 `from_hub` 一致：加载工具等于在本地执行远端代码。

`PipelineTool` 是 `Tool` 的子类，但重构了调用链。它把「一次工具调用」拆成三个可覆写的方法：`encode(raw_inputs)` → `forward(inputs)` → `decode(outputs)`；并覆写 `__call__` 亲自编排这三步，而不是让基类调 `forward` 了事。

## 怎么实现的

**`ToolCollection` 的两个入口各有各的难点。**

`from_hub` 的难点是「收藏夹里混着非工具条目」。实现用集合推导 `{item.item_id for item in _collection.items if item.item_type == "space"}` 过滤，然后列表推导逐个加载。注意它没有并发也没有重试：一个 Space 加载失败会让整个集合失败。这是刻意的简单——工具集通常很小。

`from_mcp` 的难点是生命周期与默认值变更：

- **上下文管理器而非普通构造**：MCP 连接必须显式关闭，所以返回的是 `@contextmanager`，用户必须 `with`。这是 API 层面的强制。
- **`structured_output` 的三态**：参数类型是 `bool | None`，默认 `None`。为 `None` 时发 `FutureWarning` 并回退到 `False`，警告文案写明了「1.25 版本默认会变成 True」并给出抑制方法。这是一个正在进行的迁移，用「默认值 + 弃用警告」而不是直接改默认，避免破坏既有行为。
- **依赖缺失即报错**：`mcpadapt` 未安装时抛 `ImportError` 并给出 `pip install 'smolagents[mcp]'`。
- **传输协议白名单**：`server_parameters` 为 dict 时，`transport` 缺省补成 `"streamable-http"`，且只接受 `{"sse", "streamable-http"}`，其他值抛 `ValueError` 并列出支持项。`sse` 在 docstring 里被标为 deprecated。
- **信任门槛**：与 `from_hub` 同样的 `trust_remote_code` 检查，但文案更具体——「会在你本地机器上执行代码」。

**`PipelineTool` 的四段式结构。** 它的 `__call__` 与基类完全不同，按顺序做：

1. 惰性 `setup()`（若未初始化）。
2. 若 `sanitize_inputs_outputs`，过 `handle_agent_input_types`。
3. `encoded_inputs = self.encode(*args, **kwargs)`——注意这里把原始 `args/kwargs` 直接给 `encode`，而不是 `forward`。
4. **张量/非张量分离**：把 `encoded_inputs` 拆成两组（`isinstance(v, torch.Tensor)`），张量组用 `send_to_device(..., self.device)` 搬到设备，非张量组原样，最后 `{**encoded_inputs, **non_tensor_inputs}` 合并回去喂 `forward`。分离的理由很实际：只有张量能被搬设备，字符串/attention mask 之类的非张量对象若一起送会报错。
5. `outputs = self.forward(...)`，再用 `send_to_device(outputs, "cpu")` **搬回 CPU**。
6. `decoded_outputs = self.decode(outputs)`，可选再过 `handle_agent_output_types`。

`setup()` 里是真正的重量级工作，顺序是：`pre_processor` 若是字符串就用 `pre_processor_class.from_pretrained`（默认 `AutoProcessor`）；`model` 同理（默认无，必须在子类给 `model_class`）；`post_processor` 为 `None` 时复用 `pre_processor`，是字符串时同样 `from_pretrained`。然后确定设备：显式 `device` 优先，否则若有 `device_map` 就取 `self.model.hf_device_map` 的**第一个值**，都没有就用 `accelerate.PartialState().default_device`。最后，只有在 `device_map` 为 `None` 时才 `self.model.to(self.device)`——即有 `device_map` 时模型已由 transformers/accelerate 分片放置，不能再整体搬。

构造期还有两道前置检查：`accelerate` 与 `torch` 未安装直接 `ModuleNotFoundError`（附 pip extra 名）；`model` 为空且子类没给 `default_checkpoint` 时 `ValueError`（文案是「本工具没有实现默认 checkpoint，你需要传一个」）。

`PipelineTool` 的类属性里 `inputs = {"prompt": str}`、`output_type = str` 是占位值，子类基本都会覆写；`skip_forward_signature_validation = True` 是必需的，因为它的 `forward` 接收一个聚合后的 `inputs` 字典，无法与 `inputs` 的键逐个对齐。

## 关键取舍

**源码事实：** `ToolCollection` 只是 `list[Tool]` 的薄容器，没有去重、没有命名冲突检查、没有延迟加载。命名冲突的检查发生在 `Agent._validate_tools_and_managed_agents`，不在集合里。

**源码事实：** `from_mcp` 是上下文管理器，强制显式关闭；`from_hub` 是普通类方法。这个不对称来自「MCP 有真实连接需要释放，Hub 下载只是一次读取」。

**源码事实：** `from_mcp` 的 `structured_output` 默认 `None` 会发 `FutureWarning`，并预告 1.25 改成 `True`。这是一次带过渡期的默认值迁移。

**源码事实：** `PipelineTool` 只把张量搬到设备、其余原样传递，且输出一定搬回 CPU。有 `device_map` 时跳过 `model.to(device)`。

**源码事实：** `PipelineTool` 必须设 `skip_forward_signature_validation = True`，因为它的 `forward` 签名是「一个聚合 dict」，与 `inputs` 的键不对齐。

**架构推论：** 把「模型加载与设备管理」塞进一个工具类，等于在工具层实现了半个推理运行时。好处是模型工具与其他工具在智能体眼里完全同形（都可以被代码调用或被 tool calling 调用）；代价是这类工具的行为严重依赖环境（torch/accelerate 版本、可用加速器），`setup` 里的分支数量就是这份耦合的度量。

**架构推论：** 两个「批量来源」入口都收敛到 `Tool.from_hub` / MCP 适配器，再统一成 `list[Tool]`。这条收敛是有价值的：下游（智能体、解释器、远程执行器）只需要认识 `Tool`，不必知道工具来自哪里。但它也意味着集合层无法表达「来源特有的能力」——例如 MCP 的工具可能带结构化输出 schema，而集合只把它们当作普通工具的列表。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/tools.py` | `ToolCollection` | 工具批量容器，只有 `tools` 一个字段 |
| `src/smolagents/tools.py` | `ToolCollection.from_hub` | 从 Hub 收藏夹筛 Space 条目并逐个加载 |
| `src/smolagents/tools.py` | `Tool.from_hub` | 单个工具的 Hub 加载（`ToolCollection.from_hub` 的基础） |
| `src/smolagents/tools.py` | `ToolCollection.from_mcp` | MCP 适配上下文管理器，含 transport 白名单与 `structured_output` 迁移 |
| `src/smolagents/tools.py` | `PipelineTool` | 三段式工具：`encode` / `forward` / `decode` |
| `src/smolagents/tools.py` | `PipelineTool.setup` | 加载预处理器与模型、确定设备、按需 `model.to(device)` |
| `src/smolagents/tools.py` | `PipelineTool.__call__` | 张量搬设备 → 前向 → 搬回 CPU → 解码的编排 |

## 参考

- [smolagents 源码：tools.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/tools.py)
- [smolagents 源码：mcp_client.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/mcp_client.py)
- [smolagents 文档：工具集合](https://huggingface.co/docs/smolagents/tutorials/tools#tool-collections)
- [Model Context Protocol 文档](https://modelcontextprotocol.io/)

## 与主核 / AI 的关系

`ToolCollection` 与主核里的**存储引擎插件加载器**是同一种结构：都是「从某个外部来源把一批可调用单元整体导入，并统一成内部表示」。两边都要处理来源异构（Hub 收藏夹里混着非工具条目 / 插件目录里混着元数据文件）、都要面对信任问题（远端代码 / 第三方二进制），且都把信任决策提到调用方（`trust_remote_code` / 显式启用插件）。

`PipelineTool` 值得对照主核里的**UDF 执行器**：用户自定义函数在数据库里也是一个「黑盒可调用单元」，引擎为它准备输入、收集输出、并负责资源（内存、并行度）管理。差别在 smolagents 把设备管理也放进了工具本身——这在数据库里不可想象（引擎不该知道 UDF 想用哪块 GPU）。这恰好说明了 agent 框架与数据库的一个基本差异：**数据库管理的是数据位置的移动，agent 框架管理的是模型所在设备的移动**。

对 AI 工程的实际意义在于：`from_mcp` 用上下文管理器强制生命周期管理这个做法很值得抄。MCP 这类长连接最容易出的问题不是「连不上」，而是「忘了关」——在 notebook 与服务里都会累积成资源泄漏。把它做成 `with` 语句后，泄漏在类型层面就不可能悄悄发生。另一处值得学的是 `structured_output` 的三态迁移手法：`None` 表示「你没表态」，于是发警告并沿用旧默认，把「改默认值」这个破坏性变更加速成一到两个版本的过渡期，而不是一次性打断所有用户。
