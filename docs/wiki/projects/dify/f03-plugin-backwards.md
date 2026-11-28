---
project: dify
feature: plugin-backwards
title: 反向调用
page_type: feature
schema_version: 1
refs:
  - path: api/core/plugin/backwards_invocation/model.py
    symbols: [PluginModelBackwardsInvocation]
  - path: api/core/plugin/backwards_invocation/tool.py
    symbols: [PluginToolBackwardsInvocation]
---

# 反向调用

## 一句话职责

让**插件反过来调用宿主**：插件在自己的进程里执行时，如果需要「跑一次模型」「调一个别的工具」，就把请求打回 dify 的内网 API，宿主用自己的租户、账号、凭据去代它完成——这样插件永远拿不到任何模型凭据。

## 机制是什么

正向是「宿主 → 插件守护进程 → 插件」，反向是「插件 → 宿主内网 API」。两条链路合起来，插件才能既跑在自己的进程里，又复用宿主的模型与工具。

这个设计解决的是一组**必然冲突的约束**：

| 约束 | 若插件直接持有凭据 | 反向调用 |
|---|---|---|
| 凭据归属 | 每个插件都要拿到租户密钥 | 凭据只留在宿主 |
| 计费与配额 | 插件自己算、无法归一 | 宿主按租户统一结算 |
| 审计 | 每个插件各自记日志 | 宿主是唯一事实源 |
| 模型切换 | 插件要感知配置变化 | 宿主按当前配置解析 |

代价是**插件执行期间必须能访问宿主内网**，且反向往返会叠加延迟。所以反向调用只用于「插件自己做不到」的事（调模型、跑工作流、查别的工具），而不是所有跨边界操作。

反向链路的三个组成：

- 内网控制器（`/inner-api/plugin/invoke/*`）接收插件请求，带 `plugin_inner_api_only` 与 `plugin_data` 两层装饰器；
- `BaseBackwardsInvocation.convert_to_event_stream` 把返回值统一成「一行一个 JSON」的流；
- 具体实现类负责真正干活：`PluginModelBackwardsInvocation` 解析模型实例并调用，`PluginToolBackwardsInvocation` 解析工具运行时并调用。

## 怎么实现的

**统一响应信封只有两个字段，且流式与一次性走同一条通道。**

```
class BaseBackwardsInvocationResponse[...](BaseModel):
    data: T | None = None
    error: str = ""
```

`convert_to_event_stream` 是唯一出口，逻辑很短但覆盖了四种情况：

1. 传入是生成器 → 逐块 `BaseBackwardsInvocationResponse(data=chunk).model_dump_json().encode()`；
2. 生成器**中途抛异常** → 捕获后吐一行 `BaseBackwardsInvocationResponse(error=str(e))`，**不向上抛**；
3. 传入是普通对象（`BaseModel` 或 `Mapping`）→ 只吐一行带 `data` 的 JSON；
4. 非生成器、非 `BaseModel`/`Mapping` 的字符串 → 同样只吐一行。

第 2 条是整段代码里最关键的设计：**错误被降级成流里的一项**。这样做的后果是 HTTP 状态码永远是 200，插件侧必须检查每一行的 `error` 字段才能发现失败。收益是「已经吐出的数据不丢」——如果模型跑到第 3 块才失败，前两块仍然有效。

代价同样明显：**插件必须自己实现「逐行检查 error」的循环**，忘掉就会把错误当成正常数据。而 `except Exception` 的范围包含了 `GeneratorExit`/`KeyboardInterrupt` 之外的一切，意味着**连编程错误（`AttributeError`、`TypeError`）也会被伪装成业务错误**传给插件，堆栈信息在字符串里，插件侧无从分类。

**控制器侧用 `length_prefixed_response(0xF, ...)` 包装。** 这是内网 API 的传输格式：不是标准 SSE，而是「长度前缀 + 帧类型」，`0xF` 是帧类型标识。同一控制器里工具、TTS、摘要等流式接口都用它，与 `convert_to_event_stream` 配合——**先序列化成行，再套一层帧协议**。

**模型侧的核心是「用租户身份重新绑定一个模型实例」。** `_get_bound_model_instance` 是全部模型调用的唯一入口：

```
return ModelManager.for_tenant(
    tenant_id=tenant_id, user_id=user_id, request_metadata=metadata
).get_model_instance(tenant_id=..., provider=..., model_type=..., model=...)
```

三点值得注意：

1. **`for_tenant` 是关键**：不是复用某个全局实例，而是按租户构造一个 `ModelManager`。插件传来的 `tenant_id` 只用于解析，不携带任何凭据；
2. 它被 `@with_credit_usage_created_by(CreditUsageCreatedBy.PLUGIN_API)` 装饰，**把计费来源标成「插件 API」**，于是同一个模型调用能区分「用户直接跑」与「插件代跑」；
3. 注释写明「Bind explicit caller attribution before the decorator exits. Streaming invocations settle quota later, outside this context manager's lifetime」——**流式调用的额度结算发生在 contextmanager 之外**，所以归属信息必须在装饰器退出前显式绑进 metadata。这是一处「作用域与生命周期不匹配」的显式补偿。

**`invoke_llm` 统一了流式与非流式的返回形状，都是生成器。**

- 底层返回生成器 → 包一层 `handle()`，**每块把 `chunk.prompt_messages` 清空**再 yield；
- 底层返回 `LLMResult` → 包成**单块**的 `LLMResultChunk`（`index=0`、`finish_reason=""`、`delta` 填 `message` 与 `usage`）。

清空 `prompt_messages` 是必要的：每块都带完整 prompt 会让流式响应体积翻几十倍，而插件那边本来就知道 prompt。

把非流式结果伪装成单块流，换来的是**插件侧只需要写一种消费逻辑**——不必区分「这次调用是流式还是非流式」。这是把分支收敛到生产者而不是消费者。

`invoke_llm_with_structured_output` 用完全相同的两层包装，只是块类型换成 `LLMResultChunkWithStructuredOutput`、并多带 `structured_output` 字段。

**`list_models` 是唯一一个「不调模型、只读配置」的反向接口，而它的实现里有一条强不变量。**

流程是：`create_plugin_provider_manager(tenant_id, user_id)` → `get_configurations(tenant_id).get_models(model_type=..., only_active=True)` → 按 `provider`/`model` 过滤 → 排序 → 按 `offset`/`limit` 切页。

中间对 `PluginService.list(tenant_id)` 做了一次映射构造 `installed_identities: plugin_id -> plugin_unique_identifier`，并在两个地方**主动抛错**：

- 同一 `plugin_id` 出现两个不同的 `plugin_unique_identifier` → `ValueError("Ambiguous installed identity for model plugin ...")`；
- 某个活跃模型对应的 `plugin_id` 不在已安装列表里 → `ValueError("Installed identity not found for active model plugin ...")`。

**这两条把「插件身份必须唯一可解析」写成了运行期不变式**：插件侧的模型目录里每一项都要带 `plugin_unique_identifier`，因为后续 dispatch 需要它精确定位版本；一旦解析不出来，宁可报错也不返回一个身份不明的条目。

**Token 上限的解析比看起来讲究。** `_model_token_limits` 做四件事：

1. 拿模型实例的 schema；拿不到返回 `None`（不报错）；
2. `positive_limit` 校验：拒绝 `bool`（Python 里 `bool` 是 `int` 子类，这是个真实陷阱）、拒绝非数字、要求 `0 < v <= 1_000_000_000` 且为整数值；
3. 从 `parameter_rules` 里挑出**名字在四个别名集合**（`max_tokens` / `max_output_tokens` / `max_completion_tokens` / `maxtoken`）**或 `use_template == "max_tokens"`** 的规则；
4. **多个别名时取最小值**：`min(bounded_rules, key=itemgetter(1))`，注释写的是「Multiple declared aliases must not let the caller bypass the tighter bound」。

第 4 条是不变式的正确写法：**别名越多越危险，因为调用方可以挑最宽松的那个来申请**。取 min 保证「无论声明几个别名，实际生效的都是最紧的那个」。返回结构里还带 `output_parameter`——告诉调用方该把值填到哪个参数名上，因为不同提供者的参数名不同。

**`list_models` 只对「查询了具体模型且类型是 LLM」的条目算 token 上限**（`if payload.model and model.model_type == ModelType.LLM`）。这是显式的成本控制：`_model_token_limits` 要为每个模型解析一次实例与 schema，若列表里几十个模型都算，就是几十次 schema 解析。函数 docstring 也写了同一条约束：「Resolve the selected model only; listing a catalog must not cause N schema RPCs」。

**工具侧的反向调用短得多，但它揭示了一件事：反向调用可以再套反向调用。**

`PluginToolBackwardsInvocation.invoke_tool` 的步骤是：

1. `ToolManager.get_tool_runtime_from_plugin(...)` 解析出工具运行时（这一步可能又回到插件守护进程）；
2. `ToolEngine.generic_invoke(session, tool_runtime, ...)` 执行，**`workflow_call_depth=1`**；
3. `ToolFileMessageTransformer.transform_tool_invoke_messages(...)` 把消息里的文件引用换成可访问的 URL；
4. 返回生成器。

`workflow_call_depth=1` 是**递归深度的起点**：插件调工具 → 工具内部可能又调工作流 → 工作流里可能又有工具。这个参数就是用来切断无限递归的。

而 `get_tool_runtime_from_plugin` 的签名里要 `execution_driver`，控制器传的是 `WorkflowRunAgg.run`——**工具在执行过程中需要的「往上层报告进度/产出」的能力，也是通过回传函数注入的**。所以反向调用不只是「插件能调宿主」，而是「插件能在宿主的一次执行上下文里回调」。

**`invoke_tool` 里的 `try/except` 只是 `raise e`。** 捕获后原样抛出，不做任何加工——这个写法在这里没有语义作用（除了让堆栈多一层）。真正有用的错误包装在 `convert_to_event_stream` 里。

## 关键取舍

**源码事实：** `BaseBackwardsInvocationResponse` 只有 `data` 与 `error` 两个字段；`convert_to_event_stream` 捕获生成器中途异常并转成一行 `error`，HTTP 状态码保持成功。

**源码事实：** 控制器用 `length_prefixed_response(0xF, ...)` 包装，帧类型 `0xF`，不是标准 SSE。

**源码事实：** 所有模型调用都必须经过 `_get_bound_model_instance`，它按 `tenant_id` 构造 `ModelManager`，带 `PLUGIN_API` 计费来源标记，并在装饰器退出前把归属信息绑进 metadata（因为流式结算发生在装饰器作用域之外）。

**源码事实：** `invoke_llm` 与非流式路径统一返回生成器；流式块逐块清空 `prompt_messages`；非流式结果被伪装成单块流。

**源码事实：** `list_models` 在「同 `plugin_id` 有两个唯一标识」与「活跃模型的插件未安装」两种情况下主动抛 `ValueError`。

**源码事实：** token 上限从四个参数名别名 + `use_template` 匹配，多个别名时取**最小值**；`positive_limit` 显式排除 `bool`。

**源码事实：** `invoke_tool` 用 `workflow_call_depth=1` 作为递归起点，并通过 `execution_driver` 注入上层执行上下文。

**架构推论：** 反向调用是「插件沙箱」的必然配套。只要插件不能持有凭据，它就必然需要某种「代我调用」的通道；而这个通道一旦存在，宿主就必须对每一次反向调用做**完整的租户身份重建**——不能用插件传来的任何东西当凭据，只能用 `tenant_id` 去查。`ModelManager.for_tenant` 就是这个重建动作的具体形态。

**架构推论：** 「错误降级成流里的一项」是流式接口的通用契约，但它必须配一条对等义务：**消费方必须逐行检查**。这里把这条义务下推给了插件 SDK，而 SDK 是否忠实执行无法在本仓验证。相比之下，非流式接口用 `code`/`error` 字段（f01 的 `PluginDaemonBasicResponse`）也是同一思路，但两侧都是本仓代码，可验证性强得多。

**架构推论：** `_model_token_limits` 取最小上限，是「多个声明源」场景下的正确默认。同类问题的通用规则是：**当同一语义被声明多次，取最保守的那个**，因为宽松的那个总是更容易被误用。

**架构推论：** 反向调用与正向调用构成一个环，因此必须有递归深度控制。`workflow_call_depth=1` 是显式的，但如果某些路径不传或重置它，递归就会出现——**环路的深度控制必须在「进入环」的那一处统一施加**，散落在各节点里迟早漏。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/plugin/backwards_invocation/base.py` | `BaseBackwardsInvocation` | 反向调用基类，`convert_to_event_stream` 统一出口 |
| `api/core/plugin/backwards_invocation/base.py` | `BaseBackwardsInvocationResponse` | `data` + `error` 两字段的响应信封 |
| `api/core/plugin/backwards_invocation/model.py` | `PluginModelBackwardsInvocation` | 反向模型调用：LLM、结构化输出、embedding、rerank、TTS、STT、moderation、摘要 |
| `api/core/plugin/backwards_invocation/model.py` | `PluginModelBackwardsInvocation._get_bound_model_instance` | 按租户重建模型实例并绑定计费归属 |
| `api/core/plugin/backwards_invocation/model.py` | `PluginModelBackwardsInvocation.invoke_llm` | 统一流式/非流式返回，逐块清空 prompt |
| `api/core/plugin/backwards_invocation/model.py` | `PluginModelBackwardsInvocation.invoke_llm_with_structured_output` | 结构化输出的同形包装 |
| `api/core/plugin/backwards_invocation/model.py` | `PluginModelBackwardsInvocation.list_models` | 可调模型目录与分页 |
| `api/core/plugin/backwards_invocation/model.py` | `PluginModelBackwardsInvocation._model_token_limits` | 多别名取最小上限的 token 约束解析 |
| `api/core/plugin/backwards_invocation/tool.py` | `PluginToolBackwardsInvocation` | 反向工具调用 |
| `api/core/plugin/backwards_invocation/tool.py` | `PluginToolBackwardsInvocation.invoke_tool` | 解析运行时 → 执行 → 文件消息转换，`workflow_call_depth=1` |
| `api/core/plugin/entities/request.py` | `RequestInvokeLLM` | 反向模型调用的请求模型 |
| `api/core/plugin/entities/request.py` | `RequestListModels` | 模型目录的请求模型（含 `offset`/`limit`） |
| `api/controllers/inner_api/plugin/plugin.py` | `PluginInvokeLLMApi` | `/inner-api/plugin/invoke/llm` 端点 |
| `api/controllers/inner_api/plugin/plugin.py` | `PluginInvokeToolApi` | `/inner-api/plugin/invoke/tool` 端点 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/plugin/backwards_invocation/model.py](https://github.com/langgenius/dify/blob/main/api/core/plugin/backwards_invocation/model.py)
- [dify 源码：core/plugin/backwards_invocation/base.py](https://github.com/langgenius/dify/blob/main/api/core/plugin/backwards_invocation/base.py)
- [dify 文档：插件开发](https://docs.dify.ai/plugins/introduction)

## 与主核 / AI 的关系

反向调用与主核里的「外部函数 / 回调」机制同构：数据库在内核里执行时，若要调用用户定义的函数（PL/pgSQL、C 扩展、FDW 的回调），也必须「从内核回过身去调外部代码」，同时保持事务、权限、审计的连续性。两者的共同要求一模一样：**外部的身份与权限不能由调用方声称，必须由内核按现有会话重建**。dify 用 `ModelManager.for_tenant`，数据库用当前会话的 `user_id`/`search_path`。

**错误降级成数据 vs 数据库的 `NOTICE`/`WARNING`。** PostgreSQL 允许在函数执行中途发 `NOTICE` 而不中断事务——把「非致命的坏消息」作为流的一部分传出。dify 的 `convert_to_event_stream` 做的是同一件事但更激进：**它把致命错误也降级成数据**。数据库不给这个自由度，因为「查询中途失败」必须让事务回滚；而一次模型调用失败不必然让整个插件执行作废，所以降级是合理的。**同一种手法是否可用，取决于「失败后已产生的结果是否有价值」。**

**取最紧上限 vs 数据库的 `work_mem` 逐层收敛。** 数据库的参数有多层来源（会话、角色、库、集群），生效值取最严格的那个方向要一致；dify 的 token 上限有四个别名，取 `min`。**「同一语义多处声明」时统一取最保守值**是一条可以直接搬的规则。

**递归深度 `workflow_call_depth` vs 数据库的 `max_stack_depth`。** 数据库用递归深度上限防止 SQL 函数自我调用打爆栈；dify 用 `workflow_call_depth=1` 切断「插件 → 工具 → 工作流 → 工具」的环。**任何允许 A 调 B、B 又能调 A 的系统，都必须在进入环的入口处记深度。**

对 agent 工程四条结论。其一：**凭据永不外流**的代价是必须提供反向调用通道，而这个通道必须做完整的租户身份重建，不能信任任何传入的身份声明。其二：**流式接口把错误当数据**是对的设计，但必须同时约定「消费方逐行检查」，否则错误会被静默吞掉。其三：**同一语义的多个参数别名要取最紧值**，否则调用方总能挑到最宽松的那个。其四：**正向 + 反向构成环，深度控制要落在环的入口**；散落在节点里迟早有一处漏掉，表现为偶发的无限递归。
