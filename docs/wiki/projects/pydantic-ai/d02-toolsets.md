---
project: pydantic-ai
feature: toolsets
title: 工具集抽象与装饰型工具集
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/toolsets/abstract.py
    symbols: [AbstractToolset, ToolsetTool]
  - path: pydantic_ai_slim/pydantic_ai/toolsets/function.py
    symbols: [FunctionToolset]
  - path: pydantic_ai_slim/pydantic_ai/toolsets/filtered.py
    symbols: [FilteredToolset]
  - path: pydantic_ai_slim/pydantic_ai/toolsets/prefixed.py
    symbols: [PrefixedToolset]
---

# 工具集抽象与装饰型工具集

## 一句话职责

工具集是「一批工具的容器 + 三条生命线（列工具、调工具、给指令）」，而装饰型工具集用包装代替修改：`PrefixedToolset` 改名、`FilteredToolset` 过滤、`RenamedToolset` 重命名、`SetMetadataToolset` 补元数据——每一个都不碰底层实现，只在列工具/调工具时改写。

## 机制是什么

`AbstractToolset` 定义三件必须实现的事与四件可覆写的事：

- **必须**：`id`（同名工具集之间唯一，耐久执行按它命名活动）、`get_tools(ctx) -> dict[str, ToolsetTool]`、`call_tool(name, args, ctx, tool)`。
- **可覆写**：`get_instructions(ctx)`（简介，支持 `str`、`InstructionPart`、序列或其函数）、`for_run(ctx)` / `for_run_step(ctx)`（逐运行/逐步返回新实例）、`__aenter__` / `__aexit__`（连接生命周期）。

`ToolsetTool` 是「一条可调用的工具」的完整记录：它来自哪个工具集（用于报错文案）、`tool_def`、`max_retries`、`args_validator`，外加一个可选的 `args_validator_func`（schema 校验通过后、执行前运行的自定义校验器，抛 `ModelRetry` 或 `ToolFailed`）。

容器的两种形态分明：`CombinedToolset` 合并多个工具集，`WrapperToolset` 包裹一个。所有装饰型工具集都继承后者，且默认实现是**完全透传**——`get_tools`、`call_tool`、`get_instructions`、两个 `for_run*`、`__aenter__`/`__aexit__` 全部转发给 `wrapped`。要加行为，只需覆写其中一两个。

## 怎么实现的

**`WrapperToolset` 的四个透传约定。** 每个都有一处刻意设计：

- `id` 恒为 `None`，`label` 拼成 `类名(内层 label)`。这让报错文案能显示包装层次。
- `for_run` / `for_run_step` 只在 `wrapped` 真的换了实例时才 `replace(self, wrapped=new_wrapped)`，否则返回 `self`。这个「不变则返回自身」的习惯在下游到处被依赖：`CombinedToolset.for_run_step` 用 `all(new is old ...)` 判断是否需要重建整个容器。
- `get_instructions` 不直接转发，而是收集子贡献再摊平（`_collect_child_instruction_contributions` + `flatten_instruction_contributions`）。子类若覆写了 `get_instructions`，`_authors_own_instructions()` 会返回真，于是它自己的文本被当作「作者本人说的」而不是转述——这条区分在指令归属（谁说的）上很关键。
- `apply` / `visit_and_replace` 只作用在 `wrapped` 上，所以「遍历工具集树」不会把包装器本身当成一个节点。

**`FunctionToolset`：工具集自己的默认值体系。** 它在构造时持有一整套默认（`max_retries`、`timeout`、`strict`、`durable`、`ui_visibility`、`sequential`、`requires_approval`、`defer_loading`、`metadata`、`include_return_schema`、`schema_generator`、`docstring_format`、`require_parameter_descriptions`），添加工具时按一套明确规则向下传递：

- `strict` / `include_return_schema` / `durable` / `ui_visibility`：工具未设置（`None`）时用工具集的值。
- `sequential` / `requires_approval` / `defer_loading`：工具集为真时**强制**置真（不能把已置真的改回假）。
- `metadata`：工具集的值与工具自身的合并，工具自身优先（`self.metadata | (tool.metadata or {})`）。
- `max_retries`：工具没设才继承工具集；都没设则运行期取 `ctx.max_retries`。

`add_function` 里有一段很细的补丁：它先调 `add_tool`（后者会施加上面那套「为真则强制」的规则），然后把 `sequential` / `requires_approval` / `defer_loading` **按本次调用传入的值原样恢复**，并把 `_sequential_from_toolset` / `_defer_loading_from_toolset` 清零。注释解释了原因：`Tool` 上这三个是普通布尔，无法区分「显式写 False」与「没写」，所以只有走 `add_function` 这个带 `None` 三态参数的入口才可能表达「显式关掉工具集的默认」。

**列工具时的三层关注点。** `get_tools` 并不只是 `return self.tools`：

1. **解析调用名**。对每个工具算 `exposed = _exposed_tool_name(original_name, ctx._tool_name_path)`，即把外层 `PrefixedToolset` 与 `RenamedToolset` 压下的层从内向外依次套用。然后用这个名字去 `ctx.retries` 里取这条调用名对应的重试计数。源码注释在这里罕见地列出了已知不精确之处：若工具自己的 `prepare` 改名，或有别的包装器改名，这个算出来的名字可能是错的，`prepare` 就会读到不相干工具的重试计数。
2. **构造工具级上下文**。用 `replace(ctx, tool_name=original_name, retry=..., max_retries=..., _tool_name_path=(), _dispatched_tool_name=None)`。最后一项的理由写在旁边：嵌套在一次工具调用里的列工具不得继承那次调用的工具管理器名字。
3. **跑 `prepare` 并查冲突**。`await tool.prepare_tool_def(run_context)`；返回 `None` 表示本次不注册。返回的名字若与已收的重名，按「是 prepare 改的名」还是「本来就重名」给出两种不同报错。

**`FunctionToolsetTool` 记下三样运行期要用的东西**：`call_func`（即 `FunctionSchema.call`）、`is_async`、`timeout`。`original_name` 记「工具集内部用的名字」，因为 `prepare` 可能改了 `tool_def.name`；`tool_for_tool_def` 正是靠这个重建工具（耐久执行在活动内重建工具时用，避免第二次跑 `prepare`）。

**超时在执行处读 def 而不是读构造值。** `call_tool` 里 `timeout = tool.tool_def.timeout if tool.tool_def.timeout is not None else self.timeout`——注释说明 `prepare` 可能在 `_tool_for` 之后改过它。超时到点抛的是 `ModelRetry('Timed out after N seconds.')`，即**当作可重试的模型反馈**，而不是运行错误。这与 `_HookEntry.timeout` 抛 `HookTimeoutError` 分属两套语义（见 c04）。

**装饰型工具集的四种改写手法。** 它们分别示范了「改写键」「改写内容」「改写名字」「改写调用」：

| 工具集 | 改写点 | 手法 |
|---|---|---|
| `FilteredToolset` | `get_tools` 的返回字典 | 对每条调 `filter_func(ctx, tool_def)`（可同步可异步），保留为真的 |
| `PrefixedToolset` | 键与 `tool_def.name` | `get_tools` 加前缀并在 ctx 压入一层；`call_tool` 反向 `removeprefix` 还原 |
| `RenamedToolset` | 键与 `tool_def.name` | 用 `name_map`（新名 → 原名）双向映射，重名冲突分两种文案报错 |
| `SetMetadataToolset` / `DeferredLoadingToolset` | `tool_def` 的其他字段 | 继承 `PreparedToolset`，把改写包成一个 `prepare_func` |

几个细节值得单列：

- `FilteredToolset` 调过滤函数前用 `without_tool_name_path(ctx)` 清掉调用路径层，使过滤函数看到的是一个「干净」的上下文，不受外层前缀影响。
- `PrefixedToolset` 在 `call_tool` 里不仅还原工具名，还 `replace(ctx, tool_name=original_name)` 与 `replace(tool, tool_def=replace(..., name=original_name))`——三者必须同时还原，否则执行阶段看到的名字与列工具时不一致，重试计数与钩子过滤就会对不上。
- `PrefixedToolset` 与 `RenamedToolset` 都通过 `with_tool_name_layer(ctx, ...)` 在上下文里压一层，供 `FunctionToolset.get_tools` 反推暴露名。这是一个**跨两层的隐式契约**：改名的事实只存在于 ctx 里，而不在工具定义上。
- `SetMetadataToolset` 与 `DeferredLoadingToolset` 都继承 `PreparedToolset` 并用闭包构造 `prepare_func`，构造函数里只保留数据字段。`PreparedToolset` 本身有一道校验：`prepare_func` 不得新增或重命名工具（`len(prepared - original) > 0` 直接抛错并建议改用 `FunctionToolset.add_function()` 或 `RenamedToolset`），并强制把 `capability_id` 还原为原值——理由是「工具归哪个能力所有在这个运行里是固定的，其内容受该能力的权限约束」，若 `prepare` 用新对象而不是 `replace()` 就会丢掉它。
- `DeferredLoadingToolset` 只是把 `defer_loading=True` 打上去；`tool_names` 为 `None` 表示全部工具。它复用 `PreparedToolset` 而不是另写一个包装器，说明「打标记」与「准备定义」在实现上是同一件事。

**组合与遍历。** `CombinedToolset` 的 `_merge_tools` 按顺序合并各工具集的工具，重名时抛错并附上*对方*的 `tool_name_conflict_hint`（每个工具集可以自定义提示，`PrefixedToolset` 的就提示改前缀）。它还把每个工具的 `toolset_id` 只填「显式声明过的 id」（`_explicit_id`），源码注释说明原因：由主机名推导出的 id 会改变工具定义，而耐久引擎会哈希与序列化这些定义。`call_tool` 特别从**外传的 `tool_def`** 派发而不是用 `source_tool` 缓存的那份，因为外层包装（如 `PreparedToolset`）可能已经改过它。

## 关键取舍

**源码事实：** `WrapperToolset` 的 `id` 恒为 `None`。这意味着「包装后的工具集」在耐久执行里无法被单独命名，只能以它的内层身份出现。

**源码事实：** `FunctionToolset.add_function` 必须手动恢复三个布尔并清零来源标记。`Tool` 用普通布尔 + 私有来源标记来表达三态，是一处「用最少字段覆盖两种语义」的妥协。

**源码事实：** 工具级超时抛 `ModelRetry`，钩子级超时抛 `HookTimeoutError`。前者是给模型的反馈（可以重试），后者是运行错误。两套语义并存是刻意的。

**源码事实：** `PreparedToolset` 拒绝「新增或重命名工具」，并强制还原 `capability_id`。这两条都是为了防止「用 prepare 绕过正式的注册/改名/归属通道」。

**源码事实：** 外露名（模型调用名）的推导依赖 `ctx._tool_name_path` 这个运行期上下文层，而不是工具定义上的字段。这是为了不把包装细节写进发给模型的 schema。

**架构推论：** 「必须实现三件、可覆写四件」这个最小契约，加上「包装器默认全透传」，使横切行为可以叠加而不必修改任何具体工具集实现。这也解释了 `toolsets/AGENTS.md` 里的那条纪律——优先用包装器而不是改每个具体工具集。

**架构推论：** 把改名、过滤、加元数据都做成「列工具时的投影」而不是「修改存储的工具」，使同一次运行里不同步骤可以有不同的工具面貌（配合 `for_run_step`）。代价是「工具身份」在多处被重建，任何一环没还原（键、`tool_def.name`、`ctx.tool_name`）都会导致静默错位。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/toolsets/abstract.py` | `AbstractToolset` | 工具集契约：id、列工具、调工具、指令、两级生命周期、遍历 |
| `pydantic_ai_slim/pydantic_ai/toolsets/abstract.py` | `ToolsetTool` | 单条可调用工具记录：来源、定义、重试上限、校验器 |
| `pydantic_ai_slim/pydantic_ai/toolsets/wrapper.py` | `WrapperToolset` | 全透传包装器基类，四个转发约定与 label 拼装 |
| `pydantic_ai_slim/pydantic_ai/toolsets/function.py` | `FunctionToolset` | 函数工具集：默认值体系、三态布尔补丁、列工具时解析外露名 |
| `pydantic_ai_slim/pydantic_ai/toolsets/function.py` | `FunctionToolset.add_function` / `add_tool` | 工具集默认向下传递的两种通道与优先级 |
| `pydantic_ai_slim/pydantic_ai/toolsets/function.py` | `FunctionToolset.call_tool` | 执行处读 `tool_def.timeout`，超时转 `ModelRetry` |
| `pydantic_ai_slim/pydantic_ai/toolsets/filtered.py` | `FilteredToolset` | 按上下文+定义过滤，调过滤前清掉调用路径层 |
| `pydantic_ai_slim/pydantic_ai/toolsets/prefixed.py` | `PrefixedToolset` | 加前缀并压入名字层；调用时三重还原 |
| `pydantic_ai_slim/pydantic_ai/toolsets/prepared.py` | `PreparedToolset` | 通用定义改写：拒绝增删改名，强制还原 capability_id |

## 参考

- [pydantic-ai 源码：toolsets/abstract.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/toolsets/abstract.py)
- [pydantic-ai 源码：toolsets/function.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/toolsets/function.py)
- [Pydantic AI 文档：工具集](https://ai.pydantic.dev/toolsets/)
- [Pydantic AI 文档：工具高级用法](https://ai.pydantic.dev/tools-toolsets/tools-advanced/)

## 与主核 / AI 的关系

「容器 + 可透传的包装器」是主核里存储引擎分层的同一思路：`AbstractToolset` 相当于一层存储接口，`WrapperToolset` 相当于「装饰一层而不改实现」的中间件，`CombinedToolset` 相当于多路合并的联合层。两边都靠「最小必需接口 + 默认透传」来避免为每个新行为复制一份实现。

最值得对照的是**身份重建**这件事。主核里一条记录的身份是明确的（键 + 版本），改名和过滤这种操作根本不存在；这里因为工具名对模型可见、而且会被多层包装改写，于是同一个工具在不同层有四个名字：工具集内部的键、`tool_def.name`、外露给模型的名字、以及 `ctx.tool_name`。这四个必须在列工具与调工具时同时对齐，源码为此专门维护了一个 `_tool_name_path` 的上下文层。这种「同一个对象有多重名字且要跨层对齐」的成本，是「让模型看到的名字可以动态变化」的必然代价。

对 AI 工程的实际意义在于：工具集的设计决定了「大规模工具」的可管理性。真正的难点不在「怎么定义一个工具」，而在三件事——命名冲突如何避免（前缀 + 可选自定义提示）、上下文相关的工具可见性（prepare / 过滤）、以及执行阶段能否精确还原身份（三重还原）。这套实现给出的答案是「投影而非修改 + 上下文携带改名事实」，代价是必须严格守约，否则错位是静默的。
