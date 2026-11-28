---
project: vllm
feature: api-server
title: HTTP 服务与路由
page_type: feature
schema_version: 1
refs:
  - path: vllm/entrypoints/launchers/app.py
    symbols: [build_app]
  - path: vllm/entrypoints/launchers/api_server/app_state.py
    symbols: [init_app_state]
  - path: vllm/entrypoints/launchers/api_server/routers.py
    symbols: [register_api_routers]
---

# HTTP 服务与路由

## 一句话职责

`build_app` 造一个 FastAPI 应用并按能力挂路由，`init_app_state` 把引擎客户端与一堆渲染器塞进 `app.state`——两者合起来把「一个推理引擎」变成一个可以被 OpenAI 客户端调用的 HTTP 服务。

## 机制是什么

这里有一条容易被忽略的历史线索：**老路径 `vllm/entrypoints/openai/api_server.py` 已经退化成 53 行的弃用转发层**，真实实现搬到了 `vllm/entrypoints/launchers/`。所以读代码时若在老文件里找不到逻辑，不要以为被删了——它只是搬走了，留下一层 `DeprecationWarning` 保住 import 路径。

新结构把启动拆成**两相**，且有明确顺序约束：

| 阶段 | 函数 | 输入 | 产出 |
|---|---|---|---|
| 造壳 | `build_app` | `args` + `supported_tasks` + `model_config` | 一个已挂好路由的 `FastAPI` |
| 填状态 | `init_app_state` | `engine_client` + `state` + `args` | 可直接服务的 `app.state` |

**为什么必须是两相**：`build_app` 在引擎客户端存在之前就要能跑（路由本身不依赖引擎），而 `init_app_state` 需要引擎已就绪。这个拆分让「创建 HTTP 应用」与「创建引擎」可以并行、也可以由不同的启动器做（`api_server/entry.py` 与 `render/entry.py` 都复用它）。

**路由挂载的判据是 `supported_tasks`，不是配置文件。** 一个模型是否支持 `generate` / `transcription` / `realtime` / `pooling` / `render`，由引擎在启动时探测出来，然后作为**元组**传进来。这比「按配置项猜模型能力」可靠得多。

## 怎么实现的

**`build_app` 的第一步是一个兼容性警告。**

```python
if supported_tasks is None:
    warnings.warn("The 'supported_tasks' parameter was not provided ... Defaulting to ('generate',).", DeprecationWarning)
    supported_tasks = FALLBACK_SUPPORTED_TASKS
```

`FALLBACK_SUPPORTED_TASKS` 是「只支持生成」的兜底。也就是说这个参数**正在从可选变成必填**：现在给默认值 + 警告，将来会直接报错。这是渐进式收紧接口的标准做法。

**FastAPI 的三个构造分支只管文档端点。**

- `--disable-fastapi-docs`：`openapi_url=None, docs_url=None, redoc_url=None`，连 OpenAPI schema 都不生成；
- `--enable-offline-docs`：`docs_url=None, redoc_url=None` 但保留 `openapi_url`——用于离线拉 schema；
- 默认：全开。

**这三个开关影响的是「是否暴露接口描述」，而不是「是否服务请求」。** 生产上关文档是为了不泄露 API 形状，但它同时会关掉 OpenAPI schema，如果你有基于 schema 的客户端生成器就会失效。

`app.state.args = args` 与 `app.root_path = args.root_path` 在构造后立刻设——**`args` 整包塞进 `state`**，后续所有 handler 都从这里读配置。这是一种「不做配置对象、直接传 argparse 结果」的务实选择，代价是任何 handler 都能读到任何参数，无法做接口级约束。

**路由注册的顺序有一段注释解释为什么这样排。**

```python
register_api_routers(args, app, supported_tasks, model_config)

# Endpoint plugin hooks run after core routers so plugins can inspect them.
# This runs even for the CPU only render server. A plugin eligible for
# the `render` task still gets its routes registered. It receives
# `engine_client=None` at Phase B.
attach_endpoint_plugins(app, supported_tasks)
```

**插件必须在内置路由之后挂**，因为插件要能「看到」核心路由（比如覆盖或包一层）。而 `engine_client=None` 那句指向 `init_app_state` 阶段的补充：插件分两相初始化，挂路由时还没有引擎。

接着是 `init_exception_handler` → `init_entrypoints_middleware` → `sagemaker_standards_bootstrap`。顺序也有依赖：**中间件在异常处理器之后注册**，因为中间件可能要捕获异常处理器抛出的响应；而 Sagemaker 的 bootstrap 放在最后，因为它会**包住整个 app**（返回一个新的 app 对象）。

**`register_api_routers` 是一条「按能力位挂路由」的长流水线。** 它可以分成四组：

**第一组：无条件挂。** `register_vllm_serve_api_routers`（vllm 自己的服务路由）、`register_models_api_router`（模型列表）、`register_structured_decisions_api_router`、`register_decisions_api_router`、`register_sagemaker_api_router`（带 `supported_tasks` 与 `model_config` 参数，因为它要按任务决定行为）。

**第二组：只在开发模式挂。** `envs.VLLM_SERVER_DEV_MODE` 为真时才挂 `register_vllm_dev_api_routers` 与 `register_rl_api_routers`。**这是环境变量而非命令行参数**——意味着它不能由单个部署的配置决定，只能由进程环境决定。

**第三组：按 `supported_tasks` 挂。**

- `"generate"` 在任务里 → `register_generate_api_routers` + `elastic_ep_attach_router`；若 `model_config.enable_sleep_mode` 还额外挂 checkpoint 路由。
- `"generate"` 或 `"render"` → `register_scale_out_api_routers`。
- `"transcription"` 或 `"realtime"` → `register_speech_to_text_api_routers`。
- 任一 `POOLING_TASKS` 在任务里 → `register_pooling_api_routers`。

注意 **checkpoint 路由的双重条件**：既要支持生成，又要开了睡眠模式。因为 checkpoint 的前提是「能睡着再醒来」，而睡眠模式是可选的。

**第四组：按配置项挂。** `args.enable_fault_tolerance` 为真才挂容错路由。

这四组叠起来说明：**路由集合是「能力 × 模式 × 配置」三者的乘积**，而每一组的判据来源不同（引擎探测 / 环境变量 / 命令行参数）。这是启动配置复杂度的真实来源，也解释了为什么路由注册要做成函数而不是静态列表——**静态列表无法表达条件**。

**`init_app_state` 是一段很长的「构造状态」脚本，顺序同样有依赖。**

前两段是准备：

1. 若配了 `--tool-call-parser`，**先**初始化 parser 指标（`init_parser_metrics`）。放在最前是因为后续构造的渲染器会用到 parser。
2. `supported_tasks` 缺省时**再发一次**同样的 `DeprecationWarning`——与 `build_app` 里的那条是重复的。也就是说这个参数在两个函数里各有一套兜底逻辑，将来收紧要改两处。

然后是「基础状态」的填充：served 模型名（给了 `--served-model-name` 用它，否则用 `--model`）、请求日志器（只有 `--enable-log-requests` 才建，否则 `None`）、`BaseModelPath` 列表、以及六个直接赋值：`engine_client`、`log_stats`（取反 `--disable-log-stats`）、`vllm_config`、`args`。

**注意 `log_stats = not args.disable_log_stats`**——配置项是「禁用」语义，状态是「启用」语义，这里做了一次取反。这类反向命名是配置项里最常见的坑。

接着是三个渲染相关组件，它们**共享一整套相同参数**：

- `OnlineRenderer`：请求方向，带 `chat_template`、内容格式、信任开关、工具解析器、reasoning 解析器、`reasoning_effort_map`、`default_chat_template_kwargs`。
- `OnlineDerenderer`：响应方向，参数几乎相同但多一个 `response_role` 与 `return_tokens_as_token_ids`。
- `ServingTokenization`：分词服务，复用 `openai_serving_models` 与 `online_renderer`。

**`state.online_renderer.warmup()` 是唯一带副作用的构造后动作。** 它把模板编译等一次性工作提前做掉，避免第一个请求承担冷启动。

`OnlineDerenderer` 的参数与 `OnlineRenderer` 高度重合（约 10 个），但没有被抽成共享配置对象。**这是「显式优于抽象」的取舍**：抽一个 `RenderContext` 会让两个组件的参数来源变模糊，而复制参数列表让「谁用了什么」一目了然，代价是加参数要改两处（实际是三处，含 `ServingTokenization`）。

最后是按能力的四段条件初始化，与路由注册的判据**一一对应**：

- `"generate"` → `init_generate_state` + `init_scale_out_state`；
- `"transcription"` 或 `"realtime"` → `init_speech_to_text_state`；
- 任一 `POOLING_TASKS` → `init_pooling_state`；
- **无条件** `init_structured_decisions_state`。

注意 `init_structured_decisions_state` 是**无条件**的，而对应的 `register_structured_decisions_api_router` 在路由侧也是无条件的。两处一致。

末尾三行收尾：`await init_endpoint_plugins_state(engine_client, state, args)`（插件第二相，此时有引擎了）、`state.enable_server_load_tracking = args.enable_server_load_tracking`、`state.server_load_metrics = 0`。

**`tool_server` 的条件是 `"generate" in supported_tasks`。** 因为工具调用只在生成任务下有意义——池化任务（embedding/rerank）不需要工具服务器。这是个值得记住的精度：**不是「有工具配置就建」，而是「任务支持生成才建」**。

**`init_static_loras` 是 `await` 的。** 它在 `OpenAIServingModels` 构造后立刻调，说明 LoRA 的注册需要 IO（读适配器文件），必须异步等待完成才能开始服务。

**两处 `FALLBACK_SUPPORTED_TASKS` 与两处 `DeprecationWarning`，构成一份「双份兼容代码」。** 这类重复是接口演进的常见副产品：`build_app` 与 `init_app_state` 各自对外，各自要能独立兜底。代价是将来收紧要同步改，漏一处就会出现「一边警告一边静默兜底」的不一致。

## 关键取舍

**源码事实：** 启动分两相：`build_app` 造壳挂路由（不依赖引擎），`init_app_state` 填状态（需要引擎）。

**源码事实：** `supported_tasks` 缺省时两处各发一次 `DeprecationWarning` 并退到 `FALLBACK_SUPPORTED_TASKS`（只支持生成）。

**源码事实：** FastAPI 的三个文档开关只影响 `openapi`/`docs`/`redoc` 端点，不影响服务能力。

**源码事实：** `args` 整包塞进 `app.state.args`，是所有 handler 的配置来源。

**源码事实：** 插件路由在内置路由之后挂（注释说明「让插件能 inspect 核心路由」），且在 `build_app` 阶段就挂——即使 CPU-only 的 render server 也挂。

**源码事实：** 路由注册分四组：无条件、仅开发模式（`VLLM_SERVER_DEV_MODE` 环境变量）、按 `supported_tasks`、按命令行配置（容错）。

**源码事实：** checkpoint 路由需要同时满足「支持生成」与「开启睡眠模式」两个条件。

**源码事实：** `init_app_state` 先做 parser 指标，再填基础状态，再构造渲染/反渲染/分词三件套，最后按能力做四段条件初始化；`init_structured_decisions_state` 无条件。

**源码事实：** `tool_server` 只在 `"generate" in supported_tasks` 时创建；`init_static_loras` 是 `await` 的。

**架构推论：** 「造壳」与「填状态」分离的真正价值在于**依赖倒序**。FastAPI 应用本身只依赖路由（编译期就确定），而 handler 需要引擎（运行期才有）。把两者塞进一个函数会强制「先建引擎再建 app」，于是「引擎启动失败」会连带「HTTP 服务起不来」——而实际上你想先有个能返回 503 的壳。

**架构推论：** 用 `supported_tasks` 而不是配置项决定路由，本质是**把「模型能做什么」的判断从人转移到引擎**。同一个 checkpoint 在支持生成时能跑、在只支持池化时不能，人很难凭配置判断；引擎探测出来的结果才是权威。**路由集合成为能力的函数，而不是意愿的函数。**

**架构推论：** 三组判据来源不同（引擎探测 / 环境变量 / 命令行）会让「为什么这个路由没挂上」变成一个需要逐层排查的问题。好在真正起服务的那一层（`serve_http`）会遍历 `app.routes` 把每条路径、方法、以及**是否被标记为禁用**都打进日志，并单独列出没有 `methods` 的挂载式端点。**条件注册的必要补救就是在启动时输出实际清单**——这里做到了，但输出发生在 uvicorn 启动前而不是 `build_app` 内，所以只看 `build_app` 是看不到的。

**架构推论：** `renderer` / `derenderer` / `tokenization` 三件套共享约 10 个参数但不抽公共对象，是「显式复制优于隐式抽象」的典型。它对读者的好处是每个组件的输入清单完整可见；对维护者的代价是加一个参数要改三处。判断标准是**这三个组件的参数是否真的会同步演化**——如果它们的参数集合长期趋同，就该抽；如果会分化，复制更稳。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/entrypoints/launchers/app.py` | `build_app` | 造 FastAPI 应用：文档开关、路由、插件、异常、中间件 |
| `vllm/entrypoints/launchers/api_server/app_state.py` | `init_app_state` | 填 `app.state`：模型名、渲染器、分词、按能力初始化 |
| `vllm/entrypoints/launchers/api_server/routers.py` | `register_api_routers` | 按能力位与配置挂全部路由 |
| `vllm/entrypoints/launchers/utils/server_utils.py` | `lifespan` | 应用的启动/关闭生命周期钩子 |
| `vllm/entrypoints/serve/exception_handling/register.py` | `init_exception_handler` | 统一异常处理注册 |
| `vllm/entrypoints/serve/middleware/register.py` | `init_entrypoints_middleware` | 中间件注册 |
| `vllm/entrypoints/serve/sagemaker/api_router.py` | `sagemaker_standards_bootstrap` | 包装整个 app 以适配 Sagemaker 协议 |
| `vllm/plugins/endpoint_plugins/interface.py` | `attach_endpoint_plugins` | 在核心路由之后挂插件路由 |
| `vllm/plugins/endpoint_plugins/interface.py` | `init_endpoint_plugins_state` | 插件第二相：有了引擎再填状态 |
| `vllm/entrypoints/openai/models/serving.py` | `OpenAIServingModels` | 模型清单与静态 LoRA 注册 |
| `vllm/renderers/online_renderer.py` | `OnlineRenderer` | 请求侧渲染（模板、工具、reasoning） |
| `vllm/renderers/online_derenderer.py` | `OnlineDerenderer` | 响应侧反渲染（token 到文本/工具调用） |
| `vllm/tasks.py` | `FALLBACK_SUPPORTED_TASKS` | 缺省任务集（只支持生成） |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/entrypoints/launchers/app.py](https://github.com/vllm-project/vllm/blob/main/vllm/entrypoints/launchers/app.py)
- [vllm 文档：OpenAI 兼容服务器](https://docs.vllm.ai/en/latest/serving/openai_compatible_server.html)
- [FastAPI 文档：lifespan 与中间件](https://fastapi.tiangolo.com/advanced/events/)

## 与主核 / AI 的关系

「造壳 / 填状态」两相分离与主核里「先建监听端口、再挂连接处理器」是同一种次序：**先把协议的入口暴露出来，再把后端能力接上**。主核的 `listen()` 在存储引擎就绪之前就能完成，这样客户端连上来时至少能得到一个明确的错误；vllm 的应用壳在引擎就绪之前就能构造，同理。

**按 `supported_tasks` 挂路由 vs 主核的按扩展能力注册 SQL 函数。** 主核加载扩展后才会有某些函数（如 `pgvector` 的 `<=>` 操作符），没加载就报「函数不存在」；vllm 探测到模型支持池化才有池化端点，否则路由根本不存在（404）。**两者都是「能力决定可见接口」，共同效果是「不支持的请求得到的是不存在而不是内部错误」。**

**`args` 整包进 state vs 主核的 GUC 参数。** 主核把配置收进统一的 GUC 命名空间（`pg_settings`，可查询、可会话级覆盖）；vllm 直接把 argparse 的 `Namespace` 塞进 `app.state`。前者可观测、可审计，后者零成本但**无法回答「这个参数当前是什么值」**——除非去读 `app.state.args`。这是「快速迭代」与「可运维性」之间的经典取舍。

对 agent 工程最直接的价值有四条。其一：**把服务壳与后端引擎分两相构造**，让「引擎挂了但 HTTP 还能返回结构化错误」成为可能。其二：**用探测出的能力而不是配置决定暴露哪些端点**，避免「配置说支持、实际跑不了」。其三：**条件注册的路由必须在启动日志里打出实际清单**（`serve_http` 会遍历 `app.routes` 逐条打印，含禁用标记），否则排障只能读代码。其四：**命名语义要统一**：`disable_log_stats` 到 `log_stats` 的取反可读但容易在别处搞错，配置项与状态字段的极性最好一致。
