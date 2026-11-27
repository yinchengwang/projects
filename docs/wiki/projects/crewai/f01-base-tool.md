---
project: crewai
feature: base-tool
title: 工具基类与参数模式
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/tools/base_tool.py
    symbols: [BaseTool, Tool, _serialize_args_schema]
---

# 工具基类与参数模式

## 一句话职责

把「工具」定义成一个 pydantic 模型而不是普通类：名字、描述、参数模式、缓存策略、使用配额、失败策略全是字段，于是工具本身可校验、可序列化、可随检查点往返，而真正的执行体只是一个必须实现的 `_run`。

## 机制是什么

`BaseTool` 同时是 `BaseModel` 和 `ABC`。这个组合决定了它的全部形态——字段走 pydantic 校验，执行体走抽象方法。它自己声明五个核心字段：`name`、`description`、`env_vars`、`args_schema`、`result_schema`，再加四个行为开关：`cache_function`、`result_as_answer`、`max_usage_count`、`tool_failure_policy`，以及一个运行期计数器 `current_usage_count`。

最值得注意的设计是**类注册表**：`__init_subclass__` 把每个子类按 `f"{模块}.{限定名}"` 注册进 `_TOOL_TYPE_REGISTRY`。这个注册表不是为了工厂分发，而是为了**检查点反序列化**——检查点里存的是 `{"tool_type": "some.module.SomeTool", ...}` 这样的字典，加载时必须知道该用哪个具体的 `BaseTool` 子类去校验。

参数模式有两条来源：显式给 `args_schema`，或由 `_default_args_schema` 从 `_run` 的签名自动推导。推导逻辑先扫 `_run` 的参数，若一个字段都没有则退而扫 `_arun`——这让「同步体只是占位、真正实现在异步侧」的工具也能拿到参数模式。

## 怎么实现的

**模式推导。** `_default_args_schema` 是 `args_schema` 字段的 `mode="before"` 校验器。三种输入：`dict` 走 `_deserialize_schema` 反序列化回模型类（检查点往返路径）；`None` 或占位类继续往下推导；`type` 直接返回。推导时 `signature(cls._run)` 遍历参数，跳过 `self`、`return`、`*args`、`**kwargs`，无注解补 `Any`，无默认值写 `...`（必填），有默认值写默认值本身，最后 `create_model(f"{cls.__name__}Schema", ...)`。

`result_schema` 的推导简单得多：非空就用，空则由 `_infer_result_schema_from_callable(cls._run)` 看返回注解是不是 `BaseModel` 子类，不是就 `None`。

**两个字段序列化器。** `_serialize_args_schema` 与 `_serialize_result_schema` 都是 `when_used="json"`，把模型类转成 `model_json_schema()` 的字典。于是一次 `model_dump(mode="json")` 就能得到完整可落盘的检查点片段。注意它们在**非 JSON 模式**下不生效——`model_dump()` 拿到的仍是类对象本身。

**使用配额。** `_claim_usage` 是唯一线程安全的入口：持 `_usage_lock`（pydantic `PrivateAttr` 的 `threading.Lock`），若 `max_usage_count` 非空且 `current_usage_count` 已达上限，返回一个 `ToolFailure(reason=USAGE_LIMIT)`；否则自增并返回 `None`。`run` 与 `arun` 都在**执行前**调它，超限时直接把 `ToolFailure` 当返回值交出去，不抛异常。

`max_usage_count` 有独立校验器：非空且小于等于 0 直接 `ValueError`。

**两条执行路径。** `run`：有位置参数就跳过校验，否则 `_validate_kwargs`（用 `args_schema.model_validate` 校验并把结果 `.model_dump()` 回成字典，失败时把 `build_schema_hint` 的期望参数提示拼进 `ValueError`）；`_claim_usage`；`self._run(...)`；若返回协程则 `asyncio.run` 掉。`arun` 同构，最后一跳是 `await self._arun(...)`，而 `_arun` 的默认实现直接 `NotImplementedError` 并提示「用 run 走同步」。

**检查点反序列化的信任边界。** `__get_pydantic_core_schema__` 只在 `cls is _BASE_TOOL_CLS`（即恰好是 `BaseTool` 本身）时安装一个 wrap validator，子类复用默认 schema。这个 validator 处理三种输入：已是工具实例直接返回；含 `tool_type` 的字典走 `_resolve_tool_dict`；其余交给下一层。

`_resolve_tool_dict` 先查注册表；查不到时判断 `tool_type` 是否以 `crewai.` 或 `crewai_tools.` 开头，或环境变量 `CREWAI_DESERIALIZE_CALLBACKS` 已启用——两者都不满足就 `ValueError` 拒绝。放行后 `importlib.import_module(dotted.rpartition(".")[0])` 导入模块（导入的副作用是子类把自己注册进表），再查一次注册表确认导入的确实是 `BaseTool` 子类。这段的设计意图写在注释里：**导入本身安全，因为最终仍要用注册表确认它是工具类**。

**`cache_function` 的失败关闭。** `_resolve_tool_dict` 还处理一个历史坑：若存的 `cache_function` 是 `null`（原本是 lambda 或绑定方法，没有可导入路径），替换成 `_never_cache_function` 而不是默认的「允许缓存」。理由写得很清楚——它可能守着一个会改状态的工具，宁可关掉缓存也不能回落到默认策略。若存的是 `/` 分隔的内置函数名，直接从 `_BUILTIN_CACHE_FUNCTIONS` 取回，不需要信任开关。

**`Tool` 子类。** `Tool[P, R]` 把 `ParamSpec` 与返回类型带上，核心是覆写 `run`：不再调 `_run`，直接 `self.func(**kwargs)`，若结果是协程就 `asyncio.run`。`_run` 只是 `self.func(...)` 的转发，供 `to_structured_tool` 取用。`_arun` 里先取结果，`_is_awaitable` 为真才 `await`，否则抛 `NotImplementedError`——所以给 `Tool` 包同步函数后调 `arun` 会失败，必须走 `run`。

**`tool` 装饰器。** 三种用法（裸装饰、带名字、带选项）由 `len(args)` 与类型分派。它强制两件事：函数必须有 docstring（否则 `ValueError`），参数模式由签名建 `create_model`，类名是工具名去空格后 `title()`。

**`to_structured_tool`。** 把 `BaseTool` 转成 `CrewStructuredTool`，把 `max_usage_count`、`current_usage_count`、`cache_function`、`tool_failure_policy` 全部带过去，并在结构化工具上反挂 `_original_tool = self`。这条反向指针很关键——后面所有「按名字找工具、拿原始工具的策略、同步计数器」都靠它回到 `BaseTool`。

## 关键取舍

**源码事实：** 配额检查是「先查后加」在同一把锁里完成，但 `run`/`arun` 里 `_claim_usage` 在 `_run` 之前——所以**一旦通过检查，即使执行失败也消耗了一次配额**。这不是 bug，而是把「尝试使用」计入配额的语义。

**源码事实：** 超限返回 `ToolFailure` 对象而不是抛异常。注释明确说明理由：让每条执行路径都能记录「用掉了一次配额」，而不是只有认得这句错误文案的路径才认得。

**源码事实：** `run` 只在**没有位置参数**时才校验 kwargs。传位置参数等于绕过 `args_schema` 校验——这是为兼容 `_run(self, x, y)` 这种老式调用留的口子。

**源码事实：** `_generate_description` 现在是空方法，只保留给子类覆写。历史上它会把 LLM 用的三段式描述写回 `description` 字段，现在改由 `formatted_description` 属性按需合成，`description` 保持作者原文。

**架构推论：** 工具是 pydantic 模型，代价是工具实例不能再随便塞任意属性（`__setattr__` 之外没有自由字段），收益是整个工具体系自动获得校验、JSON 序列化与检查点往返。这也解释了为什么配额计数要放在公开字段而非私有属性：它必须随检查点一起保存。

**架构推论：** `tool_type` 是 `computed_field`，因此 `model_dump(mode="json")` 天然带上它——检查点里的「这个工具是什么类」不需要额外维护字段，与类名同源，天然不会漂移。

**架构推论：** 那条「导入模块再查注册表」的路径实际上把「能不能反序列化」拆成两道：命名空间白名单管「允许谁」，注册表管「是不是工具」。单靠白名单不足以防住「导入一个不是工具的类」，单靠注册表又无法阻止导入任意模块的副作用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/tools/base_tool.py` | `BaseTool` | 工具基类：pydantic 字段 + 抽象 `_run` + 配额检查 |
| `lib/crewai/src/crewai/tools/base_tool.py` | `BaseTool._claim_usage` | 加锁的「查配额并自增」，超限返回 `ToolFailure` |
| `lib/crewai/src/crewai/tools/base_tool.py` | `BaseTool.run` | 校验参数、占配额、执行，协程结果用 `asyncio.run` 收口 |
| `lib/crewai/src/crewai/tools/base_tool.py` | `BaseTool._default_args_schema` | 从 `_run`（退化时 `_arun`）签名推导参数模型 |
| `lib/crewai/src/crewai/tools/base_tool.py` | `BaseTool._serialize_args_schema` | 把参数模型类序列化成 JSON Schema 字典 |
| `lib/crewai/src/crewai/tools/base_tool.py` | `BaseTool.__get_pydantic_core_schema__` | 仅基类安装检查点反序列化 wrap validator |
| `lib/crewai/src/crewai/tools/base_tool.py` | `_resolve_tool_dict` | 按 `tool_type` 解析具体类，含白名单与缓存函数失败关闭 |
| `lib/crewai/src/crewai/tools/base_tool.py` | `_TOOL_TYPE_REGISTRY` | `__init_subclass__` 填充的类注册表 |
| `lib/crewai/src/crewai/tools/base_tool.py` | `Tool` | 包普通函数的泛型子类，覆写 `run` 与 `_arun` |
| `lib/crewai/src/crewai/tools/base_tool.py` | `tool` | 三种用法的装饰器，强制 docstring |
| `lib/crewai/src/crewai/tools/base_tool.py` | `BaseTool.to_structured_tool` | 转成 `CrewStructuredTool` 并反挂原始工具 |
| `lib/crewai/src/crewai/tools/base_tool.py` | `BaseTool.formatted_description` | 按需合成三段式 LLM 描述 |

## 参考

- [CrewAI 源码：tools/base_tool.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/tools/base_tool.py)
- [CrewAI 文档：Tools](https://docs.crewai.com/en/concepts/tools)
- [Pydantic 文档：自定义核心 schema](https://docs.pydantic.dev/latest/concepts/types/#customizing-validation-with-__get_pydantic_core_schema__)

## 与主核 / AI 的关系

主核里与「工具」最接近的是注册进目录的算子/函数：它也有固定的名字、参数描述、返回类型，也要能被序列化进系统目录从而在重启后复原。差别在于主核的类型是编译期确定的，注册表是一张静态查找表；这里的 `_TOOL_TYPE_REGISTRY` 是**运行期**由 `__init_subclass__` 填充的，同一份检查点在「模块已导入」与「模块未导入」两种环境下走的是两条不同路径——这正是 `_resolve_tool_dict` 需要先尝试 import 再回查的原因。

与 AutoGen 的工具基类相比，这里多出三样东西，也正是它的特色：**使用配额**（`max_usage_count` 加锁计数，超限变成结构化结果而非异常）、**缓存策略字段**（`cache_function` 参与「这次结果要不要写缓存」的判断，且反序列化时对不可导入的旧值是失败关闭而非放行）、**单工具失败策略**（`tool_failure_policy` 可以在最具体的一层覆盖 agent 与 task 的设置）。AutoGen 那边工具是不可变的 schema 快照加一个 `run`，没有这些「运行期状态字段」。

对 AI 工程最实用的一条：配额是**按实例**算的，而实例通常被同一个 agent 复用。所以 `max_usage_count` 事实上限定了「这个工具在整个生命周期里最多被调几次」，跨任务累计。要在每个任务里重置，必须显式调 `reset_usage_count`——否则第二个任务会发现工具已经用尽，而且拿到的是一个看起来像正常返回值的 `ToolFailure`，不会报错。
