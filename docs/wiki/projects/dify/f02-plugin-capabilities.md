---
project: dify
feature: plugin-capabilities
title: 插件提供的四类能力
page_type: feature
schema_version: 1
refs:
  - path: api/core/plugin/impl/model.py
    symbols: [PluginModelClient]
  - path: api/core/plugin/impl/tool.py
    symbols: [PluginToolManager]
  - path: api/core/plugin/impl/datasource.py
    symbols: [PluginDatasourceManager]
  - path: api/core/plugin/impl/agent.py
    symbols: [PluginAgentClient]
---

# 插件提供的四类能力

## 一句话职责

一个插件包可以声明四类可被调用的能力——模型提供者、工具、数据源、Agent 策略——而客户端侧是四个薄子类，每个只做三件事：拼路径、定请求形状、把回包摊成强类型对象。

## 机制是什么

四个客户端共享 `BasePluginClient`，差异集中在「URL 路径」与「消息类型」两处：

| 客户端 | 能力 | 主要路径 | 回包类型 |
|---|---|---|---|
| `PluginModelClient` | 模型提供者 | `management/models`、`dispatch/model/*`、`dispatch/llm/invoke` 等 | 声明/绑定实体、`LLMResultChunk` |
| `PluginToolManager` | 工具 | `management/tools`、`dispatch/tool/invoke` | `PluginToolProviderEntity`、`ToolInvokeMessage` |
| `PluginDatasourceManager` | 数据源 | `management/datasources`、`dispatch/datasource/*` | `PluginDatasourceProviderEntity`、文档页实体 |
| `PluginAgentClient` | Agent 策略 | `management/agent_strategies`、`dispatch/agent_strategy/invoke` | `PluginAgentProviderEntity`、`AgentInvokeMessage` |

两层路径语义很清楚：

- `management/*` = **读元数据**（有哪些提供者、schema 长什么样），走普通请求，回包可整体反序列化；
- `dispatch/*` = **执行能力**（真的去调模型/工具），多数走**流式**，因为要边跑边吐结果。

这个二分不是命名习惯，而是两类调用的失败与代价不同：元数据可以缓存、可以整体校验；执行必须流式、必须有超时、必须能把中途错误当成流里的一项传出来。

## 怎么实现的

**每个客户端都往路径里塞 `tenant_id`，并把身份额外放进请求头。**

`management` 路径形如 `plugin/{tenant_id}/management/models`、`plugin/{tenant_id}/management/datasources`；`dispatch` 路径形如 `plugin/{tenant_id}/dispatch/llm/invoke`、`plugin/{tenant_id}/dispatch/tool/invoke`。

`tenant_id` 出现在**路径**而不是 body 里，意味着它在守护进程侧就是一个路由段——多租户隔离靠 URL 前缀实现。而具体是哪个插件实现的，则另外用请求头声明：

- 模型侧：`plugin_dispatch_headers(plugin_id, plugin_unique_identifier)`，来自 `services.plugin.dispatch_identity`；
- 工具侧与 Agent 侧：直接构造 `{"X-Plugin-ID": provider_id.plugin_id, "Content-Type": "application/json"}`。

注意工具/Agent 用的是 `X-Plugin-ID`（只有插件标识），模型侧还带 `plugin_unique_identifier`（精确到版本）。**这个差异是必要的**：同一个插件可能有多个版本共存，模型调用必须精确到版本；工具调用按 id 解析到当前安装版本即可。

**`_dispatch_payload` 是模型侧唯一的请求包装约定。** 它把请求体固定成两段：

```
{"data": {...业务字段...}, "user_id": ..., "app_id": ...}
```

业务字段全进 `data`，而 `user_id` 与 `app_id` 提到同一层——**因为它们不是模型调用的语义参数，而是调用归属信息**，守护进程要用它们做配额、审计、凭据查找。两个字段都是可选的，`None` 就不放进去（而不是放 `null`）。

工具侧与 Agent 侧没有这个包装：它们直接把 `user_id`、`conversation_id`、`app_id`、`message_id`（Agent 还有 `context`）与 `data` 平铺在同一层。**三种形状并存说明这是三套并行演进的协议，没有统一请求信封。**

**模型侧的九个 dispatch 方法形态高度一致，但**有两处反直觉的写法**。

第一处：`get_model_schema` 与 `validate_provider_credentials` 用的是**流式请求**（`_request_with_plugin_daemon_response_stream`），却只取第一条就返回：

```
for resp in response:
    return resp.model_schema
return None
```

也就是说这两个「一次给一个结果」的接口走的是流式通道，然后只消费第一个元素就退出。后果是**生成器没有被耗尽**——后面的行不会被读取，连接由生成器销毁时回收。源码注释没有解释为什么，但从协议一致性看，很可能是因为守护进程那边这几个接口都挂在同一个流式分发入口上。

`validate_provider_credentials` 还在返回前做了一件有副作用的事：若回包里带了 `credentials` 且是 dict，就 `credentials.update(resp.credentials)`——**把守护进程补全的凭据写回调用方传进来的字典**。这是「校验顺便补全」的复用，调用方拿到的 `credentials` 会多出服务端派生出来的字段。这种「入参被修改」的写法在批量校验时很省事，但如果调用方共享同一个 dict，就容易串。

第二处：`invoke_llm` 对错误的处理是**把结构化异常压扁成 `ValueError`**：

```
try:
    yield from response
except PluginDaemonInnerError as e:
    raise ValueError(e.message + str(e.code)) from e
```

其它方法都让 `PluginDaemonInnerError` 原样冒泡（由 `_handle_plugin_daemon_error` 产生），只有 `invoke_llm` 把它转成 `ValueError` 并把 code 拼进消息字符串。**这是不一致的**：调用方若想按异常类型区分「模型限流」与「插件崩了」，在 LLM 这条路上拿不到类型，只能解析字符串。

`start_llm_polling` / `check_llm_polling` 是异步长任务的两段式协议：先拿一个请求 id 启动，再轮询结果。`_raise_typed_polling_unsupported_error` 是专门的错误提升函数，注释说明「current plugin sdk / plugin daemon does not raise exception with type `PluginLLMPollingUnsupportedError`」——**能力已经在客户端实现，但对端还没跟上**，属于预留。

**工具侧的复杂度不在请求，而在「把流拼回文件」。** `invoke` 最后一行是

```
return merge_blob_chunks(response, max_file_size=dify_config.PLUGIN_MAX_FILE_SIZE)
```

`merge_blob_chunks` 是一个独立的 113 行模块，做的是：流里可能出现 `BLOB_CHUNK` 类型的消息（大文件被切片传输），把同一 `id` 的切片按序累进一个 `bytearray`，等 `end` 为真时合成一条完整的 `BLOB` 消息吐出去。它有三道限制：

1. **分配前先校验声明大小** —— `total_length > max_file_size` 就抛错。源码注释写清了理由：「`total_length` comes from the plugin and the whole buffer is pre-allocated, so an oversized claim would exhaust memory (or raise an unhandled MemoryError/OverflowError) before the size limit below is ever checked」。**这是先校验再分配，而不是分配后再检查**——顺序反了就是一个由插件控制的内存耗尽漏洞；
2. 累计偏移超限就抛错并**从缓冲表里删除该项**；
3. 单块超过 `max_chunk_size`（默认 8 KB）就抛错。

还有一个 `require_complete` 开关：开启时要求分片有序、声明大小一致、末尾块完整。**默认关闭**——说明常规路径容忍乱序/缺失，只有明确需要完整文件时才开。

**数据源侧最特殊：它有一个不来自插件的本地提供者。** `fetch_datasource_providers` 在拿到守护进程的列表后，无条件插入一条 `_get_local_file_datasource_provider()` 返回的字典（硬编码的 `langgenius/file/file`，`provider_type` 为 `local_file`），然后对**合并后的全体**统一做一件事：

```
provider.declaration.identity.name = f"{provider.plugin_id}/{declaration.identity.name}"
for tool in provider.declaration.datasources:
    tool.identity.provider = provider.declaration.identity.name
```

**把名字改写成「插件 id / 提供者名」的复合形式**，并把每个子项的 `provider` 字段指向这个复合名。原因很实际：不同插件可以有同名提供者（都叫 `file`），而下游要用这个名字做唯一寻址。**改写发生在客户端而不是守护进程侧**，意味着守护进程返回的是「局部名」、客户端负责「命名空间化」——这类职责划分在跨进程边界上很常见，但代价是每个消费这两类实体的地方都必须用改写后的名字。

**元数据列表的请求都带固定分页参数** `{"page": 1, "page_size": 256}`：模型、工具、数据源、Agent 策略一律如此。**256 是硬编码的一次性上限**，没有任何分页循环。也就是说某租户装了超过 256 个提供者，多出来的会被静默漏掉。这不是「分页接口」，而是「假装分页的定长拉取」。

**多数列表方法带一个 `transformer` 回调做就地修补。** `transformer` 在 `_request_with_plugin_daemon_response` 里、在 `PluginDaemonBasicResponse` 校验**之前**执行。三处用法各有目的：

- 工具：补 `identity.provider`（把工具挂到其提供者名下），再 `resolve_dify_schema_refs` 展开参数 schema 里的 `$ref`；
- 数据源：同样补 `identity.provider`，并对 `output_schema` 展开 `$ref`；
- Agent：只补 `identity.provider`，且 `fetch_agent_strategy_provider` 里先判 `data` 或 `declaration` 为 `None` 就原样返回（**避免对错误响应做修补**）。

**`$ref` 展开这一步放在客户端，说明守护进程返回的 schema 是「带引用的紧凑形式」**，而本进程内需要「自包含形式」才能直接喂给前端或 JSON Schema 校验。这类「传输用紧凑、使用用展开」的分工是合理的，但要保证展开逻辑只有一处——这里就是 `resolve_dify_schema_refs`。

**四个客户端都不是无状态的**：它们要么带 `plugin_unique_identifier`（模型侧用于精确版本解析），要么在方法里显式接收 `plugin_id`。而 `PluginToolManager`、`PluginDatasourceManager`、`PluginAgentClient` 的构造不需要参数——它们的身份来自每个调用参数里的 `provider_id`。

**这是一处不对称**：模型侧的「哪个插件」是构造期绑定（`plugin_unique_identifier`），其余三类是调用期传入（`plugin_id` / `provider_id`）。原因是模型的 dispatch 头需要版本级精确标识，而这个标识在一批调用里通常不变；工具的 `X-Plugin-ID` 只到 plugin 级别，可以从 provider 名里解出来。

## 关键取舍

**源码事实：** 四个客户端都继承 `BasePluginClient`，路径分 `management/*`（读元数据、普通请求）与 `dispatch/*`（执行、多数流式）两类，`tenant_id` 在路径段里。

**源码事实：** 请求信封有三套形状：模型侧 `{"data": {...}, "user_id", "app_id"}`（由 `_dispatch_payload` 统一）；工具侧与 Agent 侧把 `user_id`/`conversation_id`/`app_id`/`message_id` 与 `data` 平铺。

**源码事实：** `get_model_schema` 与 `validate_provider_credentials` 走流式接口但只取第一条；后者还会把回包里的 `credentials` 写回入参字典。

**源码事实：** `invoke_llm` 把 `PluginDaemonInnerError` 转成 `ValueError(e.message + str(e.code))`，是四个客户端里唯一这样做的。

**源码事实：** 工具与 Agent 的 dispatch 头是 `X-Plugin-ID` + `Content-Type`，模型侧用 `plugin_dispatch_headers(plugin_id, plugin_unique_identifier)`。

**源码事实：** 所有列表请求固定 `page=1, page_size=256`，没有分页循环。

**源码事实：** `merge_blob_chunks` 在分配缓冲**之前**校验 `total_length`，单块上限 8 KB，`require_complete` 默认关闭。

**源码事实：** 数据源列表无条件并入硬编码的 `langgenius/file/file` 本地文件提供者，并把所有提供者名改写成 `plugin_id/name` 复合形式。

**架构推论：** 「四类能力」的划分不是抽象出来的分类，而是**插件 SDK 的实际接口面**：一个插件可以实现其中任意子集，守护进程按 `plugin_unique_identifier` 把 dispatch 请求路由到对应实现。客户端侧四个类之所以都很薄，是因为复杂度全在守护进程与插件 SDK 那边——**本进程只需要知道「怎么把请求打成对的形状」**。

**架构推论：** `management/*` 与 `dispatch/*` 的分野应该被当成设计原则而非命名约定。凡是「读元数据」都该可缓存、可整体校验；凡是「执行」都必须流式、可中断、能把中途错误作为流的一部分传出。把执行接口做成一次性请求/响应，会让长任务在超时边界上极其脆弱。

**架构推论：** 三套请求信封并存是这个架构最明显的技术债。每次新增能力都要重新决定「身份信息放哪、业务参数放哪」，而守护进程侧要同时理解三种形状。统一信封的代价是一次性改造 + 兼容期；不统一的代价是永久性的认知负担。

**架构推论：** `page_size=256` 无分页是**用静默截断换实现简单**。它的失效条件很明确（提供者数 > 256），一旦触发表现为「某些工具突然不见了」且无任何日志。要么做成分页，要么把上限显式化（回包带 `total`，客户端检测 `len(data) == page_size` 时告警）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/plugin/impl/model.py` | `PluginModelClient` | 模型提供者：schema、凭据校验、LLM/embedding/rerank/TTS/STT/moderation 调用 |
| `api/core/plugin/impl/model.py` | `PluginModelClient._dispatch_payload` | 模型侧请求信封：业务字段进 `data`，归属信息提层 |
| `api/core/plugin/impl/model.py` | `PluginModelClient.invoke_llm` | 流式 LLM 调用，异常被压成 `ValueError` |
| `api/core/plugin/impl/model.py` | `PluginModelClient.start_llm_polling` | 异步长任务：启动轮询 |
| `api/core/plugin/impl/model.py` | `PluginModelClient.check_llm_polling` | 异步长任务：查询轮询结果 |
| `api/core/plugin/impl/model.py` | `PluginModelClient._raise_typed_polling_unsupported_error` | 预留的错误提升（对端尚未抛该类型） |
| `api/core/plugin/impl/tool.py` | `PluginToolManager` | 工具提供者与工具调用 |
| `api/core/plugin/impl/tool.py` | `PluginToolManager.fetch_tool_providers` | 补 `identity.provider` 并展开参数 schema 的 `$ref` |
| `api/core/plugin/impl/tool.py` | `PluginToolManager.invoke` | 流式工具调用，回包交给 `merge_blob_chunks` |
| `api/core/plugin/impl/tool.py` | `PluginToolManager.get_runtime_parameters` | 拉取运行时参数定义 |
| `api/core/plugin/impl/datasource.py` | `PluginDatasourceManager` | 数据源能力：抓取、文档解析、云盘浏览 |
| `api/core/plugin/impl/datasource.py` | `PluginDatasourceManager.fetch_datasource_providers` | 并入本地文件提供者并做名字空间化 |
| `api/core/plugin/impl/datasource.py` | `PluginDatasourceManager._get_local_file_datasource_provider` | 硬编码的本地 `file` 提供者声明 |
| `api/core/plugin/impl/agent.py` | `PluginAgentClient` | Agent 策略提供者与策略调用 |
| `api/core/plugin/impl/agent.py` | `PluginAgentClient.invoke` | 流式策略调用，携带 `PluginInvokeContext` |
| `api/core/plugin/utils/chunk_merger.py` | `merge_blob_chunks` | 分片文件重组：分配前校验、单块上限、完整性开关 |
| `api/core/plugin/utils/chunk_merger.py` | `FileChunk` | 单个文件的重组缓冲 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/plugin/impl/model.py](https://github.com/langgenius/dify/blob/main/api/core/plugin/impl/model.py)
- [dify 源码：core/plugin/impl/tool.py](https://github.com/langgenius/dify/blob/main/api/core/plugin/impl/tool.py)
- [dify 文档：插件开发](https://docs.dify.ai/plugins/introduction)

## 与主核 / AI 的关系

四类能力的分法与主核里的「访问方法（AM）能力声明」同源：每种实现声明它支持哪些操作，框架按能力路由。差别是主核的 AM 能力表是**枚举 + 注册**（`amhandler` 声明 `scan`/`insert`/`update`），插件的四类能力是**四个独立的 HTTP 命名空间**——前者可查询、可校验，后者只能靠调用失败来发现「这个插件没实现这类能力」。

**management vs dispatch 与数据库的 catalog vs executor 分层完全对应。** 查系统表（有哪些表、列类型是什么）与执行查询（真的去读数据）在数据库里是两条完全不同的代码路径，前者可以缓存、后者必须流式。dify 把它做成了 `management/*` 与 `dispatch/*`。**任何系统只要同时提供「元数据查询」和「实际执行」，都应该把这两条路分开**，混在一起的后果是元数据查询被迫承担执行的复杂度。

**blob 分片重组与数据库的大字段传输。** PostgreSQL 的 TOAST 会把大字段切片存储、按需重组，且都必须防「声明一个超大长度」的攻击。`merge_blob_chunks` 里那句「先校验声明大小再分配」正是 TOAST 类代码必须遵守的规则——**长度字段来自不可信方时，先校验再分配**。这条在数据库内核里是常识，在应用层经常被忘。

**名字空间化与数据库的 `search_path`。** dify 把提供者名改写成 `plugin_id/name` 来消歧；数据库用 schema 前缀（`public.users`）做同样的事。**跨来源汇集同名对象时，必须有一层命名空间**，而放在哪一侧（生产侧还是消费侧）会影响所有下游——dify 选在客户端，意味着守护进程返回的名字不能直接用作唯一键。

对 agent 工程四条结论。其一：**能力面用独立命名空间而不是能力枚举**，新增能力不需要改路由表，代价是无法静态得知「某插件支持什么」。其二：**执行接口必须流式**，否则长任务在超时边界上不可用。其三：**跨进程传输的不可信长度字段要先校验再分配**，这是内存耗尽漏洞的唯一可靠防线。其四：**硬编码的 `page_size` 上限要么改成分页、要么在触顶时告警**，静默截断是最难排查的一类缺陷。
