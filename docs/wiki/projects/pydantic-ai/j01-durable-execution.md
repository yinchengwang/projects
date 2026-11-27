---
project: pydantic-ai
feature: durable-execution
title: 持久执行与可重放单元
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/durable_exec/_base.py
    symbols: [_RestrictedRunContext, _ModelRequestCacheIdentity, conversation_id_from_run_id]
  - path: pydantic_ai_slim/pydantic_ai/durable_exec/_spec.py
    symbols: [DurabilityEngineSpec]
---

# 持久执行与可重放单元

## 一句话职责

把一次 agent 运行里的每一次模型请求与每一次工具调用都切成一个**可重放单元**：单元的执行结果被引擎记住，重跑时不再真的调模型/工具，而是直接回放结论——于是崩溃恢复、进程迁移、长时间等待都退化成「把没做过的那部分做完」。

## 机制是什么

三个抽象层：

- `DurabilityEngineSpec` 是**声明式配置**：一个引擎叫什么、它的可重放单元叫什么（activity / step / task）、容器叫什么（workflow / flow）、序列化用哪种 codec、哪些工具集类型要被包成单元、每类工具集的进入生命周期、以及若干开关。
- `DurableOperation` 是**一个可重放单元的完整声明**：`operation_id`（类型化身份）、`handler`（语义体）、`parameter_transport`（参数进出边界的编解码）、`cache_identity`（给按哈希去重的引擎用的投影）、`result_codec`（结果编解码）、`config_role`（粗粒度配置类别）。
- `BaseDurabilityCapability` 是**每种引擎的实现基类**：它拥有模型注册表、把运行路由进引擎、并承担「跨边界传什么」的全部责任。

`_RestrictedRunContext` 是一个 `Protocol`，只有一个方法 `_expose_field(name)`——它的存在说明运行上下文里有些字段需要被显式「开放」才能跨边界，而不是整份序列化出去。

`conversation_id_from_run_id(run_id)` 用固定命名空间做 UUID5：`uuid5(NAMESPACE, run_id)`。文档写明用途：在持久上下文里从 `get_default_conversation_id()` 返回它，于是**重放或恢复的运行会得到同一个 `conversation_id`，而不必抽取随机数**。

## 怎么实现的

**「只有字符串过边界」是这套设计的铁律。** `BaseDurabilityCapability` 的类 docstring 把理由写得很直白：`Model` 实例没法序列化进 activity/step/task，所以请求携带的是一个 `model_id` 字符串（`None` 表示 agent 默认模型、`models=` 注册表的键、或模型名字符串），到了对面再重建模型——重建时走 agent 完整的 `resolve_model_id` 能力链（因此是 deps-aware 的），注册表只作兜底。**未注册的 `Model` 实例在工作流侧直接被拒**，因为按它的 `model_id` 重建会悄悄连到另一个端点、用另一套凭据。

**`_bind_models` 的三条规则值得逐条记住。**

其一，`'default'` 是保留键。一个「具体的」默认模型（`Model` 实例，或用户用 `models=` 显式映射成实例的字符串）会被注册为 `'default'`；`models=` 里的字符串还会额外按原始字符串登记一次，使运行时的默认解析得到同一个实例。

其二，**纯字符串默认模型刻意不在这里解析**。文档解释了原因：过早构造可能建出错误的 provider，并且**伴随认证/配置副作用**，而此时同级的一个 `ResolveModelId` 能力还没机会重新解释这个字符串。于是干脆不注册 `'default'`，每个默认请求都带原始字符串，在 worker 上重新走能力链（或 `infer_model`）。

其三，agent 没有自己的模型时仍可绑定，前提是某个能力能提供模型（例如 `ModelFallback` 或 `SelectModel`）。此时同样不注册 `'default'`。

**`_find_model_id` 的剥壳规则是一个精细的匹配协议。** 它从请求的模型出发，一层层剥掉 `WrapperModel`：每一层先尝试「按原样匹配已注册实例」，命中即返回，且**优先最浅的匹配**。文档给出两条边界：一个已注册的「会改变行为的包装」即使外面又套了未注册的包装（例如一个 `InstrumentedModel` 套在它外面），仍保持自己的 ID；而一个套在默认模型外面的未注册包装仍走默认模型的快路径。同时，**已注册的那一侧永不被剥开**——一个已注册包装的身份只在其注册深度成立，它里面那个裸模型不继承这个 ID。

匹配不到任何东西的实例会被**拒绝**而不是按自身 `model_id` 往返。理由与上文一致：重建会得到「同名但不同端点」的模型。

**`_resolve_model_for_request` 在 worker 侧重建，且必须 deps-aware。** 它先跑 agent 的完整 `resolve_model_id` 能力链（用 `ModelResolutionContext(agent=..., deps=run_context.deps)`），只有当链上没人认领时才退回 `infer_model(model_id)`。注释点明目的：一个 provider 依赖运行 deps 的模型，应该在 worker 上用**真实的 deps** 重建，而不是「deps 盲」地重建。用户解析器抛的异常原样传播，只有 `infer_model` 的兜底错误被翻译成指引性文案。

**可重放单元的身份是「名字 + 参数哈希」。** `_ModelRequestCacheIdentity.project` 把一次模型请求投影成五元组：`model_id`、`messages`、`model_settings`、`model_request_parameters`、`run_context`。这是给**按哈希去重的引擎**（journal 型）用的：相同参数即视为同一单元，直接回放。`DurableOperation` 里 `cache_identity` 与 `parameter_transport` 是两个独立关注点——前者回答「这两次调用是不是同一件事」，后者回答「参数怎么过去」。

**每种单元都有自己的一份投影。** 模型请求用五元组；取消挂起响应用 `(model_id, response, run_context)`；消息压缩用 `(model_id, request_context, instructions, run_context)`；事件流处理器只用 `(event,)`；工具集发现只用 `(ctx,)`；函数工具调用用 `(name, tool_args, ctx, tool)`；动态工具集调用用 `(name, tool_args, ctx, tool_def)`。粒度差异是有意义的：请求侧要全量参数才能区分，而「取工具列表」只需要上下文。

**`operation_id` 是类型化身份而不是字符串。** `ModelRequestId(model_id, streaming, model_name)` 三者都在身份里——所以流式与非流式是**两个不同的单元**，不同注册模型的请求也是不同单元。`DurableOperationId` 是一个联合类型，文档明确「这个联合会在 minor 版本增加变体，因此匹配必须带默认分支」。

**`DurabilityEngineSpec` 的每个字段都是「必须显式声明」的。** 字段注释里有一句关键的话：工具集生命周期「被强制显式，因为两个真实 bug 都来自有默认值的门控」。这条注释比字段本身更有价值——它说明这类配置项的默认值是个陷阱。

**`codec` 区分两种引擎族。** 对象传递型（Temporal / DBOS / Prefect）用 `IDENTITY_CODEC`，值原样过边界；journal 型（Restate / Lambda / Absurd）用 JSON codec。`serialization_failure` 则把确定性的编码失败映射成引擎的终止错误类型，让引擎**不去重试那些永远编不出来的值**。

**`wrapped_toolset_kinds` 的缺省是 `{'function', 'mcp', 'dynamic'}`，但 DBOS 少了 `'function'`**（函数工具通过 `@DBOS.step` 内联跑）。这说明「哪些东西该被包成单元」是引擎相关的，不是普适规则。

**`toolset_lifecycles` 有四档语义。** `enter-always`（容器围着运行进它）；`enter-outside-durable`（只在容器之外进，因为 Temporal 的 activity 可能在另一个进程跑）；`enter-never`（谁也不进，Restate 用这个把函数工具排除在进入之外）；`enter-in-durable-unit`（运行持有一个已进入的工具集，第一个需要它的单元进去，于是连接被单元的重试覆盖，且 `MCPToolset` 整个运行只维持一个会话与其 `cache_tools`）。最后一档的适用条件写得很具体：**只适用于单元跑在容器自己进程里的引擎**。

**`tool_call_result_upgrade_lenient` 默认关闭，且文档劝阻打开。** 它用于解码「控制流异常被包成值之前」记录的旧结果；新引擎必须保持 `False`，因为宽松解码无法区分「早期的原始记录」与「损坏/误解码的载荷」，严格解码才能让损坏暴露出来，而不是把垃圾喂给模型。

**「一个 agent 只能有一个引擎」。** `_one_per_agent` 类变量设为 `'durable execution engine'`，注释说明理由：每个引擎都把每次模型请求与工具调用包成自己的单元，所以第二个引擎不会增加持久性，只会**在不说明的情况下接管第一个的分发**。

**绑定需要一个唯一的 agent 名字。** `_bind_for_agent` 在 `self.name` 与 `agent.name` 都为空时抛 `UserError`，并解释这个名字用来识别该 agent 的持久单元。引擎还可以通过 `name=` 显式提供。

**注册型引擎与可调用型引擎的绑定时机不同。** 注册型后端（`RegisteredOperationBackend`）在绑定时就注册全部单元，因为 worker 启动前需要完整注册集；可调用型引擎则**逐请求绑定**，因为它的单元名可能依赖那次请求的 `model_id`。

**模型的包装要在 worker 侧重新套一次。** `_base.py` 里有一段注释：工作流侧已经把能力的模型包装套在了它自己那份不真正发请求的模型上，所以到了会发请求的那个模型上要**再套一次**。这解释了为什么 worker 侧重建后还要过 `get_wrapper_model`。

**上下文里有字段需要显式「开洞」。** `_RestrictedRunContext._expose_field('model')` 的调用点说明：运行上下文不是整体跨边界，而是按需把某些字段放到可序列化视图里。这是一种白名单式的边界控制。

## 关键取舍

**源码事实：** 只有字符串能跨边界携带「哪个模型」。未注册的 `Model` 实例被拒而不是尽力序列化。这是**安全取舍压过便利**：按名字重建会连到别的端点、用别的凭据，静默走错比直接报错危险得多。

**源码事实：** 纯字符串默认模型不在绑定期解析，避免「建错 provider 且带认证副作用」。这是把「解析时机」当成正确性问题而不是性能问题处理。

**源码事实：** `'default'` 是保留键，且只有在默认模型是具体实例（或用户显式映射）时才注册。

**源码事实：** 已注册的包装身份止于其注册深度。这防止「里面的裸模型继承外层包装的身份」——那会让一个 `InstrumentedModel` 包着的模型被当成被包模型本身。

**源码事实：** 可重放单元的身份里包含 `streaming`。流式与非流式请求是两个不同的单元，不能互相回放。

**源码事实：** 哈希型引擎靠 `cache_identity` 判「是否同一件事」，而 `project` 的粒度因单元而异。粒度选错会导致「本该复用却重跑」或「本不该复用却回放」。

**源码事实：** 一个 agent 只能挂一个持久执行引擎。第二个不是叠加，而是接管。

**源码事实：** `DurabilityEngineSpec` 的字段被强制显式，注释点名「两个真实 bug 来自有默认值的门控」。这条经验值得直接搬到别处：**决定「哪段代码要不要被包起来」的开关，不该有默认值**。

**源码事实：** 宽松解码旧载荷的开关默认关闭且被劝阻打开。它承认「无法区分早期原始记录与损坏载荷」，于是选择让损坏可见。

**架构推论：** 持久执行的全部难点集中在一句话：**边界两个侧的环境不一样**。同一份代码在 workflow 侧与 activity 侧可能有不同进程、不同事件循环、不同可用 deps、不同凭据。因此跨边界的东西必须满足三条：可序列化（只有字符串与已声明的 codec）、可确定性重建（不能用随机数，所以 `conversation_id` 从 `run_id` 派生）、身份稳定（名字 + 参数哈希，而不是对象 id）。任何一条不满足，重放就会走样，而重放走样在持久执行里是最难查的一类问题——因为它只在恢复路径上出现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/durable_exec/_spec.py` | `DurabilityEngineSpec` | 引擎的声明式配置：单元名词、codec、工具集种类与生命周期 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_spec.py` | `codec` / `serialization_failure` | 对象传递型与 journal 型的两种序列化策略 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_spec.py` | `toolset_lifecycles` | 四档进入语义，决定谁在哪进工具集 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_operation.py` | `DurableOperation` | 一个可重放单元的完整声明 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_operation.py` | `CacheIdentity.project` | 给哈希型引擎用的语义投影 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_operation.py` | `ParameterTransport` | 参数进出边界的编解码，`load` 带引擎侧 runtime |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_operation.py` | `ModelRequestId` | 模型请求单元身份：模型、是否流式、名字 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_base.py` | `BaseDurabilityCapability` | 引擎基类：模型注册表、跨边界责任、单引擎约束 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_base.py` | `_bind_models` | 建注册表；字符串默认模型刻意不解析 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_base.py` | `_find_model_id` | 剥包装层找注册 ID，未注册实例直接拒 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_base.py` | `_resolve_model_for_request` | worker 侧 deps-aware 重建，`infer_model` 兜底 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_base.py` | `_ModelRequestCacheIdentity` | 模型请求的五元组语义投影 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_base.py` | `_RestrictedRunContext` | 运行上下文的按需开洞协议 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_base.py` | `conversation_id_from_run_id` | 从 `run_id` 确定性派生会话 id，供重放复用 |

## 参考

- [pydantic-ai 源码：durable_exec/_base.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/durable_exec/_base.py)
- [pydantic-ai 源码：durable_exec/_spec.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/durable_exec/_spec.py)
- [Pydantic AI 文档：持久执行概览](https://ai.pydantic.dev/durable_execution/overview/)
- [Pydantic AI 文档：持久执行后端](https://ai.pydantic.dev/durable_execution/backends/)

## 与主核 / AI 的关系

持久执行与主核的「预写日志 + 重放恢复」是同一条原则推到应用层：把不可重复的副作用（这里是模型请求与工具调用）变成「有记录的一次性动作」，崩溃后按日志重放已完成的部分。主核的重放靠 LSN 与幂等页写，这里的重放靠单元身份与缓存结论。

对 agent 工程的直接价值有三条。其一，**别把模型实例跨边界带**。持久执行只认字符串标识，因为按名字重建可能落到别的端点；想把特定实例带进持久运行，只有「注册进 `models=`」或「用模型名字符串 + 解析能力」两条路。其二，**默认模型用字符串时要小心**。它在绑定期不解析（避免认证副作用），意味着「默认模型是什么」要到运行期才定——这对依赖「启动时就确定模型」的监控与配额逻辑有影响。其三，**可重放单元的身份决定复用边界**。流式与非流式是两个单元；参数哈希包含 run context；工具调用哈希包含工具定义。任何「看起来一样但实际不同」的调用都不会被误回放，反之任何「实际一样但哈希不同」的调用都会重跑一遍并再付一次钱。
