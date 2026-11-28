---
project: dify
feature: tool-manager
title: 工具管理器与凭据解析
page_type: feature
schema_version: 1
refs:
  - path: api/core/tools/tool_manager.py
    symbols: [ToolManager, ToolCredentialResolver, WorkflowToolRuntimeSpec]
  - path: api/core/tools/entities/tool_entities.py
    symbols: [ToolProviderType, ApiProviderSchemaType]
---

# 工具管理器与凭据解析

## 一句话职责

把「七种来源的工具」收成一条统一解析路径：给定 `(provider_type, provider_id, tool_name)`，返回一个带好凭据、绑好运行时参数的 `Tool` 实例——而凭据怎么取，由外部注入的一个无状态函数决定，`ToolManager` 自己不碰数据库。

## 机制是什么

`ToolProviderType` 是这个模块的类型基石，七个取值对应七条完全不同的解析链：

| 类型 | 来源 | 凭据从哪来 |
|---|---|---|
| `PLUGIN` | 插件守护进程 | 凭据解析器（租户级凭据记录） |
| `BUILT_IN` | `core/tools/builtin_tool/providers` 目录下的子类 | 同上；但 `need_credentials` 为假时给空字典 |
| `WORKFLOW` | 数据库里的 `WorkflowToolProvider` | 无凭据，但需要运行时参数解密 |
| `API` | 数据库里的 API 工具提供者 | 加密存储，取用时解密 |
| `APP` | 未实现 | — |
| `DATASET_RETRIEVAL` | 已废弃（抛「not found」） | — |
| `MCP` | 数据库里的 MCP 提供者 | 由 MCP 控制器自己处理 |

`ApiProviderSchemaType` 是另一条轴——它只描述「一份 API 工具定义是用什么格式描述的」：`OPENAPI` / `SWAGGER` / `OPENAI_PLUGIN` / `OPENAI_ACTIONS`。**这两个枚举的差别很说明问题**：前者是「工具从哪来」（决定解析路径），后者是「定义长什么样」（决定解析器）。把两者分开，是这套设计能容纳四种 API 描述格式而不让主分派爆炸的原因。

`ToolManager` 是个纯 `@classmethod` 的静态门面，状态全在类变量里：一个硬编码提供者字典、一个外部提供者类列表、一个标签缓存、一个凭据解析器。**它没有实例，也没有构造参数**——所有调用方都用 `ToolManager.get_tool_runtime(...)` 这样的静态形式。

## 怎么实现的

**主分派的形态是一段 `match provider_type`，七条 `case`，其中两条是显式放弃。**

```
case ToolProviderType.BUILT_IN:   ...
case ToolProviderType.API:        ...
case ToolProviderType.WORKFLOW:   ...
case ToolProviderType.APP:            raise NotImplementedError("app provider not implemented")
case ToolProviderType.PLUGIN:     ...
case ToolProviderType.MCP:        ...
case ToolProviderType.DATASET_RETRIEVAL: raise ToolProviderNotFoundError(...)
case _:                                  raise ToolProviderNotFoundError(...)
```

`APP` 抛 `NotImplementedError`（枚举存在、语义预留），`DATASET_RETRIEVAL` 抛 `ToolProviderNotFoundError`（曾存在、已退役）。**同一个枚举里的两个「不存在」，用两种异常表达**——前者是「将来会有」，后者是「曾经有过但从现在起不存在」。这个区分对调用方有意义：前者说明是配置错误，后者说明是老数据。

**`BUILT_IN` 分支最能说明这套抽象的分层。** 它的步骤是：

1. `get_builtin_provider(provider_id, tenant_id)` —— 先查硬编码字典，查不到再退到插件提供者（**内置名与插件名共享一个命名空间**）；
2. `provider_controller.get_tool(tool_name)`，拿不到就 `ToolProviderNotFoundError`；
3. **若 `provider_controller.need_credentials` 为假**，直接 `fork_tool_runtime` 一个凭据为空的运行时并返回——**不走凭据解析器**；
4. 需要凭据时，先算 `selected_id`：**只有当控制器是插件提供者、且 `credential_id` 是合法 UUID 时才用它**，否则传 `None`；
5. `_credential_resolver is None` 则抛 `RuntimeError("Tool credential resolver is not configured")`；
6. 调解析器拿 `(credentials, credential_type)`，再 `fork_tool_runtime`。

第 3 步是性能与正确性的双重优化：**不需要凭据的工具不该触发一次凭据查询**（那可能是一次数据库读加一次 Redis 锁）。第 4 步那个条件判断更微妙：`credential_id` 只有在插件提供者场景下才可信为 UUID，内置工具的路径上即使传了一个 UUID 字符串也不采纳——**因为内置工具不按 id 选凭据，只按提供者取唯一一份**。

第 5 步的 `RuntimeError` 是「组装期缺失」的典型表达：解析器必须在应用启动时通过 `configure_credential_resolver` 注入，没注入就是装配错误，不是运行时业务错误。

**`_credential_resolver` 是一个 `Protocol`，而不是一个具体类。**

```
class ToolCredentialResolver(Protocol):
    def __call__(self, *, controller, provider_id, workspace_id, credential_id) -> tuple[Mapping[str, object], CredentialType]: ...
```

`configure_credential_resolver` 的 docstring 把约束写死了：「Register stateless composition; it must not capture an app, database or provider.」——**注入的函数必须无状态，不能捕获 app、数据库连接或提供者对象**。原因是 `ToolManager` 是类级静态状态，一旦捕获了某个请求的 session，就会跨请求泄漏。

实际注入的实现在 `api/extensions/application_services/tools.py`：`resolve_tool_credentials` 每次调用都新建 `ProviderToolCredentialRuntime`、`ToolCredentialService`、`CredentialQueryRepository`（用 `get_session_maker()` 取 session 工厂而不是 session 本身）、`RedisCredentialRefreshLock`。**这就是「无状态组合」的具体样子：函数本身不持有任何东西，每次调用把依赖现装一遍。**

它还处理了插件提供者与内置提供者的命名差异：

```
if isinstance(controller, PluginToolProviderController):
    provider = ToolProviderID(provider_id)
    providers = (str(provider), provider.provider_name)
else:
    providers = (provider_id,)
```

插件工具的凭据可能按「完整 id」或「短名」两种键存过，所以查询时把两个都作为候选；内置工具只有一种键。

**`WORKFLOW` 分支是唯一自己开数据库会话的分支。**

```
with Session(db.engine, expire_on_commit=False) as session, session.begin():
    workflow_provider = session.scalar(workflow_provider_stmt)
```

查不到就 `ToolProviderNotFoundError`；查到后转成控制器、取第一个 `WorkflowTool`、`fork_tool_runtime` 一个空凭据运行时，再**把 `execution_driver` 挂到工具实例上**。最后一行是关键：工作流工具在执行时需要回调上层执行器（上报进度、产出结果），而这个能力无法通过运行时参数传，只能直接赋属性。

`expire_on_commit=False` 是必要的：会话退出后还要用 `workflow_provider` 的属性（`tenant_id`），默认的 `expire_on_commit=True` 会让它在会话关闭后失效。

**`PLUGIN` 与 `MCP` 两个分支的写法几乎一样，都在「事后改运行时」。**

```
plugin_tool = cls.get_plugin_provider(provider_id, tenant_id).get_tool(tool_name)
runtime = getattr(plugin_tool, "runtime", None)
if runtime is not None:
    runtime.user_id = user_id
    runtime.invoke_from = invoke_from
    runtime.tool_invoke_from = tool_invoke_from
return plugin_tool
```

`getattr(..., None)` 加 `if` 说明 `runtime` 是可选的——有些工具实例不带运行时。这两个分支**没有 `fork_tool_runtime`**，而是就地改已存在运行时的字段。这与内置/API/工作流三条分支形成对比：那三条都是「造一个新运行时并 fork」。**同一个方法里两种模式并存，意味着插件工具与 MCP 工具的 `get_tool` 内部已经建好了运行时**，`ToolManager` 只负责补上调用上下文。

**插件提供者控制器有进程级缓存，且用了双重检查。** `get_plugin_provider` 先从 `contexts.plugin_tool_providers` 这个 contextvar 里取字典，命中就返回；未命中则**加锁后再查一次**，避免并发重复构造。缓存键只有 `provider` 字符串，**不带 `tenant_id`**——这是个有趣的取舍：同名插件提供者在不同租户下是同一份声明（声明不含凭据），所以可以共享；凭据在运行时里、按租户解析。

构造控制器时还做了一次**异常翻译**：

```
except (PluginNotFoundError, PluginDaemonNotFoundError) as exc:
    raise ToolProviderNotFoundError(f"plugin provider {provider} not found") from exc
```

注释写明动机：「Translate to the console's domain error so the API returns a 4xx instead of leaking the daemon's `PluginNotFoundError` as a 500.」——**跨进程的异常类型不能直接冒泡到 HTTP 层**，否则会把「用户填错名字」变成「服务器错误」。这是 f01 那套错误分派的下游：`BasePluginClient` 把守护进程的错误翻成领域异常，`ToolManager` 再把领域异常翻成控制台可用的 4xx。

**硬编码提供者靠「目录遍历 + 动态加载子类」发现。**

`_list_hardcoded_providers` 用 `listdir` 遍历 `builtin_tool/providers` 下的每个目录，对每个 `<name>/<name>.py` 调 `load_single_subclass_from_source(module_name=..., parent_type=BuiltinToolProviderController)`，实例化后 `_cache_hardcoded_provider`。

三点值得注意：

1. **每个文件独立 try/except 并 `logger.exception` 后 `continue`** —— 一个坏的内置提供者不会让全部加载失败，但也意味着**它会静默消失**；
2. `_cache_hardcoded_provider` 除了存提供者，还把每个工具的 `identity.label` 记进 `_builtin_tools_labels`，供 `get_tool_label` 用；
3. 遍历完还追加 `_external_hardcoded_provider_classes` —— 这是给**仓外定义的提供者**准备的注册口，`register_hardcoded_provider_class` 负责加入并**把 `_builtin_providers_loaded` 置假**以触发重载。

实际上 `init_app` 里就有一处条件注册：

```
if get_knowledge_fs_features().enabled:
    ToolManager.register_hardcoded_provider_class(KnowledgeFSProvider)
```

注释写的是「The Agent Knowledge Base tool cannot run without KnowledgeFS, so keep it out of tool lists when disabled」——**功能开关决定一个工具是否存在**。这是「能力由部署形态决定」的正确做法：不是在工具内部判断「有没有 KnowledgeFS」，而是根本不注册它。

**`_builtin_providers_loaded` 这个布尔有隐患。** 它被声明并且在 `_list_hardcoded_providers` 末尾置真、在 `register_hardcoded_provider_class` 与 `clear_hardcoded_providers_cache` 里置假，但**判定加载与否用的都是 `len(cls._hardcoded_providers) == 0`**，不是这个布尔。于是「注册了外部提供者但已有缓存」这种路径下，`_builtin_providers_loaded` 变成假、而实际判定仍然是「字典非空 → 不重载」，新注册的外部提供者就不会出现。**两个表示同一状态的标志并用，是这类缓存的典型不一致源。**

**运行时参数的类型转换分两条路，由「有没有变量池」决定。**

`_convert_tool_parameters_type` 的签名带 `typ: Literal["agent", "workflow", "tool"]` 与两个开关，只处理 `form == FORM` 的参数（非 FORM 的由别处处理）。核心分叉：

- **有 `variable_pool`（工作流内）**：参数配置是一个 `ToolNodeData.ToolInput`，其 `type` 决定取值方式——`variable` 走变量选择器（`variable_pool.get(selector)`，取不到就 `ToolParameterError`）、`constant` 直接用值、`mixed` 走 `convert_template` 拼模板取文本；其它类型抛 `ToolParameterError`；
- **没有 `variable_pool`（Agent 场景）**：直接从配置取标量，走 `parameter.init_frontend_parameter(...)`。

在取值之前还有一段**选择器值的特判**：`_extract_runtime_selector_value` 只对 `MODEL_SELECTOR` 与 `APP_SELECTOR` 两种参数生效，返回一个结构化字典而不是标量，注释写明「Selector parameters carry structured dictionaries, not scalar ToolInput values」。`_is_selector_value` 用「有没有 `provider`/`model`/`model_type` 三个字段」或「有没有 `app_id`」来判断——**这是鸭子类型式的结构校验，不是严格的类型校验**。

函数开头还有一条与类型无关的硬约束：

```
if parameter.type in {SYSTEM_FILES, FILE, FILES} and parameter.required and typ == "agent" and not allow_file_parameters:
    raise ValueError(f"file type parameter {parameter.name} not supported in agent")
```

**Agent 场景默认不支持必填的文件参数**，必须显式 `allow_file_parameters=True` 才能开。这解释了为什么 `get_agent_tool_runtime` 有一个同名参数：能力默认关闭、按调用方需要逐个打开。

`use_default_for_missing_form_parameters` 是另一个开关，开启时缺值会按顺序回退：参数默认值 → 若必填且是 `SELECT` 且有候选，取第一个候选 → 都失败才 `continue` 跳过。**「取第一个候选」是有点冒险的默认**，但对「必填却没人填」的场景比报错更实用。

## 关键取舍

**源码事实：** `ToolManager` 全部是 `@classmethod`，状态在类变量；主分派是七条 `case` 的 `match`，`APP` 抛 `NotImplementedError`、`DATASET_RETRIEVAL` 抛 `ToolProviderNotFoundError`。

**源码事实：** 凭据解析器是 `Protocol` 注入，docstring 要求「无状态、不得捕获 app/数据库/提供者」；未注入时 `get_tool_runtime` 抛 `RuntimeError`。

**源码事实：** `selected_id` 只在「控制器是插件提供者且 `credential_id` 是合法 UUID」时采纳，内置工具路径上即使传 UUID 也不用。

**源码事实：** `need_credentials` 为假的内置工具直接返回凭据为空的运行时，跳过解析器。

**源码事实：** 内置/API/工作流三条分支都用 `fork_tool_runtime` 造新运行时；插件/MCP 两条分支直接就改已存在运行时的字段。

**源码事实：** `get_plugin_provider` 用 contextvar 字典做进程级缓存、加锁双重检查，缓存键不带 `tenant_id`，并把守护进程的 `PluginNotFoundError`/`PluginDaemonNotFoundError` 翻成 `ToolProviderNotFoundError`。

**源码事实：** 内置提供者靠 `listdir` + `load_single_subclass_from_source` 动态加载，单个失败只记日志后 `continue`；外部提供者通过 `register_hardcoded_provider_class` 注入并置 `_builtin_providers_loaded = False`，但实际判定用的是 `len(_hardcoded_providers) == 0`。

**源码事实：** `KnowledgeFSProvider` 只在 `get_knowledge_fs_features().enabled` 为真时注册。

**源码事实：** `_convert_tool_parameters_type` 按有无 `variable_pool` 分成两条取值路径；Agent 场景默认拒绝必填文件参数；`use_default_for_missing_form_parameters` 的末位回退是「必填 SELECT 取第一个候选」。

**架构推论：** 「来源类型枚举 + 单一解析入口」是这套设计的核心价值：调用方永远只问一句「给我这个工具的运行时」，不需要知道它来自插件、内置目录、数据库还是 MCP。代价是**这个函数承担了七种资源的取用策略**，任何一类工具的凭据机制变化都要改这里。

**架构推论：** 把凭据解析做成注入的无状态函数，是为了解耦「工具解析」与「凭据存储」。`ToolManager` 在 `core/` 里、不该依赖 `services/` 的凭据实现；注入让依赖方向反过来。而「必须无状态」这条约束不是洁癖——类级静态状态 + 捕获请求级 session 组合起来就是跨请求数据泄漏。

**架构推论：** 两种运行时构造模式（fork 新建 vs 就地改字段）并存，说明抽象没有完全统一。统一成一种需要插件与 MCP 的控制器也支持「获取不带运行时的工具定义」，而它们的实现是「取工具时顺手建好运行时」。**这是历史演进的痕迹：先有 fork 模式，插件/MCP 后加时选了更省事的路。**

**架构推论：** `_builtin_providers_loaded` 与实际判定条件不一致，是「缓存失效标志」这一类问题的通例：**一旦有两个地方都能表达「已加载」，就必须保证它们永远同步**，否则组合路径（注册外部提供者 + 已有缓存）会静默失效。正确的做法是让判定只读一个源。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/tools/tool_manager.py` | `ToolManager` | 七类工具来源的统一解析门面 |
| `api/core/tools/tool_manager.py` | `ToolManager.get_tool_runtime` | 主分派：按 `ToolProviderType` 解析并组装运行时 |
| `api/core/tools/tool_manager.py` | `ToolManager.get_builtin_provider` | 硬编码字典优先、插件提供者兜底 |
| `api/core/tools/tool_manager.py` | `ToolManager.get_plugin_provider` | contextvar 缓存 + 双重检查 + 异常翻译 |
| `api/core/tools/tool_manager.py` | `ToolManager.get_workflow_tool_runtime` | 工作流工具的运行时参数转换与解密 |
| `api/core/tools/tool_manager.py` | `ToolManager.get_tool_runtime_from_plugin` | 反向调用入口，固定 `InvokeFrom.SERVICE_API` |
| `api/core/tools/tool_manager.py` | `ToolManager.configure_credential_resolver` | 注入无状态凭据解析器 |
| `api/core/tools/tool_manager.py` | `ToolManager.register_hardcoded_provider_class` | 仓外提供者的注册口 |
| `api/core/tools/tool_manager.py` | `ToolManager._list_hardcoded_providers` | 目录遍历 + 动态子类加载 |
| `api/core/tools/tool_manager.py` | `ToolManager._convert_tool_parameters_type` | 运行时参数的类型转换（变量池 / 标量两路） |
| `api/core/tools/tool_manager.py` | `ToolManager._extract_runtime_selector_value` | 模型/应用选择器的结构化取值 |
| `api/core/tools/tool_manager.py` | `ToolCredentialResolver` | 凭据解析器的 Protocol 契约 |
| `api/core/tools/tool_manager.py` | `WorkflowToolRuntimeSpec` | 工作流工具的最小接口契约（五个属性） |
| `api/core/tools/entities/tool_entities.py` | `ToolProviderType` | 七类工具来源枚举 |
| `api/core/tools/entities/tool_entities.py` | `ApiProviderSchemaType` | 四种 API 定义格式枚举 |
| `api/core/tools/entities/tool_entities.py` | `ApiProviderAuthType` | 三种 API 鉴权方式，含 `api_key` 的历史别名归一 |
| `api/core/tools/entities/tool_entities.py` | `ToolParameter` | 参数类型与表单形式的定义 |
| `api/extensions/application_services/tools.py` | `resolve_tool_credentials` | 实际注入的无状态凭据解析实现 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/tools/tool_manager.py](https://github.com/langgenius/dify/blob/main/api/core/tools/tool_manager.py)
- [dify 源码：core/tools/entities/tool_entities.py](https://github.com/langgenius/dify/blob/main/api/core/tools/entities/tool_entities.py)
- [dify 文档：工具接入](https://docs.dify.ai/guides/tools)

## 与主核 / AI 的关系

「一个入口解析多种来源」与主核里的「表访问方法（AM）分派」同构：`heap`/`btree`/`columnar` 各有各的存储布局，但上层只调 `amgettuple`；dify 有插件/内置/工作流/API/MCP 五类来源，上层只调 `get_tool_runtime`。两者的共同收益都是**把多样性关在一个函数里**，共同风险都是**这个函数会成为最难改的地方**。

**注入无状态解析器 vs 数据库的钩子注册。** PostgreSQL 的 `fmgr` 里每个函数有 `prosrc`/`proconfig`，调用时按当前会话解析；dify 的凭据解析器按 `tenant_id`+`workspace_id` 现装依赖。两者的共同约束是：**解析过程不能持有跨调用的状态**，否则会话隔离就破了。dify 把这条写进 docstring，数据库靠后端进程模型天然保证。

**异常翻译 vs 数据库的 SQLSTATE 分层。** 守护进程的 `PluginNotFoundError` 必须变成控制台的 `ToolProviderNotFoundError` 才能给出 4xx。数据库也有同样的分层：底层 `E_*` 错误码映射成对外 `DB_E_*`，避免内部错误泄漏成 500。**跨层异常必须显式翻译**，否则用户看到的是「服务器错误」而不是「你填错了」。

**两种运行时构造模式 vs 数据库的两种元组来源。** 数据库读一行可能来自堆表、也可能来自索引回表，路径不同但产出的元组形状一致。dify 的 fork 与就地改字段也是两条路径产出一个 `Tool`——**只要对外形状一致，内部不一致是可以接受的**，但要有测试同时覆盖两条路径。

对 agent 工程四条结论。其一：**工具来源越多，越需要一个统一解析入口**，否则每个调用点都要自己分派，改一处漏三处。其二：**凭据解析要注入且无状态**，绝不把数据库 session 或请求对象塞进全局单例。其三：**跨进程异常必须翻译成领域异常**，否则「名字填错」会表现为 500。其四：**两个标志表示同一状态必然不同步**，缓存失效判定只该读一个源——这里 `_builtin_providers_loaded` 与 `len(_hardcoded_providers)` 并存就是反例。
