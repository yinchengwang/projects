---
project: dify
page_type: overview
display_name: Dify
repo: https://github.com/langgenius/dify
license: NOASSERTION
language: Python
stars: 161641
stars_as_of: 2026-11-22
refs:
  - path: api/core/workflow/node_factory.py
    symbols: [DifyNodeFactory, resolve_workflow_node_class]
  - path: api/core/app/apps/base_app_runner.py
    symbols: [AppRunner]
  - path: api/core/rag/index_processor/index_processor.py
    symbols: [IndexProcessorFactory]
  - path: api/core/plugin/plugin_service.py
    symbols: [PluginService]
  - path: api/core/mcp/mcp_client.py
    symbols: [MCPClient]
---
# Dify

## 一句话职责

一个 LLM 应用平台：把「工作流编排、Agent、RAG 摄取、插件、模型接入、MCP、人工介入、评测」全部做进同一个产品，并且这些能力通过「应用（App）」这一个概念互相耦合。

## 机制是什么

dify 与本文库前七个 RAG 项目的差别是**它同时是框架和产品，而产品的形状决定了框架的形状**。

langchain / llama-index / haystack 是「给别人搭 RAG 的库」；lightrag / graphrag / ragas 是「解决单一问题的工具」；ragflow 是「Go 写的完整服务端」，规模大一档但形态是传统分层服务；**dify 是「应用平台」**——它没有把所有能力做成可独立使用的库，而是围绕「应用」这个概念把它们缝在一起。

这个定位带来两个可观察的后果，也是读这个仓最该先知道的两件事：

**第一，它的图执行内核不在这个仓里。** `api/pyproject.toml` 依赖 `graphon`（`langgenius/graphon`，pin 到具体 commit `4fa22141`，并注释说明是为了「pin 0.8.0 的 container-type 修复直到上游发版」）。因此 `api/core/workflow/` 只负责「节点工厂 + 运行时装配 + 入口适配」，真正的超步执行、通道、检查点都在外部包。**这与 ragflow 把 langgraph 移植进 `internal/harness/` 是相反的架构决策**：一个内联（可改但锁死在本仓），一个外置（可独立升级但要跨仓调试）。

**第二，它有一个独立的 Agent 运行时。** `dify-agent/` 是一个可 `pip install` 的独立包（`api` 用 `path = "../dify-agent"` 依赖它），有自己的 `pyproject.toml`、自己的测试、自己的运行时循环。Agent 能力没有内嵌在 `api` 里，而是做成了一个**可单独部署的进程**。

规模：`api/` 下 4969 个 py 文件、135 万行，其中 `tests/` 占 78 万行——**测试占 58%**，这在本文库所有项目里是最高的。生产代码的实际分布是 `services` 17.4 万行、`core` 11.8 万行、`controllers` 9.4 万行、`providers` 4.1 万行。

本文库按「用户能看到什么 → 它底下靠什么」组织：**9 个机制领域、共 53 篇机制页**。

## 怎么实现的

下表的 9 个领域按下列顺序排列：A 应用形态 → B 工作流图与节点 → C 工作流生成器 → D RAG 摄取与索引 → E RAG 检索与后处理 → F 插件与工具 → G Agent 与 MCP → H 服务与仓储 → I 运行时支撑与部署。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`api/` 与 `dify-agent/` 两个包。`web/`（Next.js 前端）、`sdks/`、`e2e/`、`docker/`、`docs/`、`packages/` 不建页。

**一次请求的一句话概览**（细节见 A、B、H 组）：请求进四类控制器之一（console / openapi / service_api / inner_api）→ 按 app_id 取出应用配置 → 对应形态的 `AppRunner` 起一次执行 → 工作流形态把 DSL 交给 `graphon` 图引擎（节点类由本仓的 `node_factory` 提供）→ 事件流经 `WorkflowResponseConverter` 转成 SSE → 由 Celery 任务或同步路径推给客户端。

## 机制导航（53 篇 / 9 组）

### A-应用形态与编排（7 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-app-runner.md`](a01-app-runner.md) | 应用执行器骨架 |
| [`a02-app-generator.md`](a02-app-generator.md) | 应用响应生成与流式转换 |
| [`a03-workflow-app.md`](a03-workflow-app.md) | 工作流应用形态 |
| [`a04-chat-apps.md`](a04-chat-apps.md) | 对话与补全形态 |
| [`a05-agent-app.md`](a05-agent-app.md) | Agent 应用形态 |
| [`a06-app-config.md`](a06-app-config.md) | 应用配置与模型参数 |
| [`a07-workflow-response.md`](a07-workflow-response.md) | 工作流响应转换 |

### B-工作流图与节点（8 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-workflow-entry.md`](b01-workflow-entry.md) | 工作流入口与图引擎边界 |
| [`b02-node-factory.md`](b02-node-factory.md) | 节点工厂与类型注册 |
| [`b03-llm-node.md`](b03-llm-node.md) | LLM 节点与模型环境变量 |
| [`b04-agent-nodes.md`](b04-agent-nodes.md) | 两代 Agent 节点 |
| [`b05-knowledge-nodes.md`](b05-knowledge-nodes.md) | 知识检索与知识索引节点 |
| [`b06-human-input.md`](b06-human-input.md) | 人工介入：两代实现与表单编译 |
| [`b07-trigger-nodes.md`](b07-trigger-nodes.md) | 触发器节点 |
| [`b08-node-runtime.md`](b08-node-runtime.md) | 节点运行时装配与变量池 |

### C-工作流生成器（3 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-workflow-generator.md`](c01-workflow-generator.md) | 工作流自动生成器 |
| [`c02-tool-catalogue.md`](c02-tool-catalogue.md) | 工具候选与能力查询 |
| [`c03-app-pipeline.md`](c03-app-pipeline.md) | 流水线应用与 DSL 校验 |

### D-RAG 摄取与索引（6 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-extract-processor.md`](d01-extract-processor.md) | 文档抽取分派 |
| [`d02-text-splitter.md`](d02-text-splitter.md) | 递归字符切分与增强 |
| [`d03-index-processor.md`](d03-index-processor.md) | 索引处理器工厂与基类 |
| [`d04-paragraph-index.md`](d04-paragraph-index.md) | 段落索引处理器 |
| [`d05-parent-child-index.md`](d05-parent-child-index.md) | 父子分块索引处理器 |
| [`d06-qa-index.md`](d06-qa-index.md) | 问答对索引处理器 |

### E-RAG 检索与后处理（5 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-vector-store.md`](e01-vector-store.md) | 向量库工厂与懒加载 |
| [`e02-embedding-cache.md`](e02-embedding-cache.md) | 嵌入缓存 |
| [`e03-dataset-retrieval.md`](e03-dataset-retrieval.md) | 知识库检索编排 |
| [`e04-retrieval-service.md`](e04-retrieval-service.md) | 单库检索服务 |
| [`e05-rerank-and-post.md`](e05-rerank-and-post.md) | 重排与后处理 |

### F-插件与工具（5 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-plugin-service.md`](f01-plugin-service.md) | 插件服务与守护进程通信 |
| [`f02-plugin-capabilities.md`](f02-plugin-capabilities.md) | 插件提供的四类能力 |
| [`f03-plugin-backwards.md`](f03-plugin-backwards.md) | 反向调用 |
| [`f04-tool-manager.md`](f04-tool-manager.md) | 工具管理器与凭据解析 |
| [`f05-workflow-as-tool.md`](f05-workflow-as-tool.md) | 工作流即工具 |

### G-Agent 与 MCP（6 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-agent-runners.md`](g01-agent-runners.md) | Agent 执行器族 |
| [`g02-dify-agent-runtime.md`](g02-dify-agent-runtime.md) | 外部 Agent 运行时 |
| [`g03-agent-layers.md`](g03-agent-layers.md) | Agent 能力分层 |
| [`g04-mcp-client.md`](g04-mcp-client.md) | MCP 客户端与传输 |
| [`g05-mcp-auth.md`](g05-mcp-auth.md) | MCP 授权流程 |
| [`g06-mcp-server.md`](g06-mcp-server.md) | MCP 服务端 |

### H-服务与仓储（6 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-knowledge-service.md`](h01-knowledge-service.md) | 知识库与文档服务 |
| [`h02-knowledge-fs.md`](h02-knowledge-fs.md) | 知识文件系统与后台引擎 |
| [`h03-rag-pipeline-service.md`](h03-rag-pipeline-service.md) | RAG 流水线服务 |
| [`h04-rbac-and-tools-service.md`](h04-rbac-and-tools-service.md) | 权限与工具管理服务 |
| [`h05-repositories.md`](h05-repositories.md) | 仓储工厂与持久化模型 |
| [`h06-tracing-and-evaluation.md`](h06-tracing-and-evaluation.md) | 追踪与评测 |

### I-运行时支撑与部署（7 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-model-provider.md`](i01-model-provider.md) | 模型提供方服务 |
| [`i02-configs.md`](i02-configs.md) | 配置装配 |
| [`i03-async-tasks.md`](i03-async-tasks.md) | 异步任务与工作流执行任务 |
| [`i04-celery-and-blueprints.md`](i04-celery-and-blueprints.md) | Celery 与蓝图装配 |
| [`i05-storage-and-human-input-delivery.md`](i05-storage-and-human-input-delivery.md) | 存储后端与人工介入投递 |
| [`i06-observability.md`](i06-observability.md) | 日志存储与可观测性 |
| [`i07-controllers.md`](i07-controllers.md) | 控制器分层与鉴权包装 |

## 关键取舍

**源码事实：** 图执行内核外置为 `graphon` 依赖（pin 到 commit `4fa22141774dff80e90e4dfa98ea3e6fbf0421bb`），`api/core/workflow/` 只做节点工厂、运行时装配与入口适配。

**源码事实：** Agent 运行时是独立的可安装包 `dify-agent`（`api` 用 path 依赖），有自己的运行时循环、协议定义与分层能力。

**源码事实：** `api/` 下 4969 个 py 文件、135 万行，`tests/` 占 78 万行（约 58%）。

**源码事实：** 节点注册靠 `pkgutil.walk_packages` 遍历三个包，导入即注册，`register_nodes` 用 `lru_cache(maxsize=1)` 缓存；docstring 明写「导入副作用必须留在工作流层，不能下沉到图原语层」。

**源码事实：** `resolve_workflow_node_class` 里有显式兜底：`AGENT` 节点 `version == "2"` 且无 kind 标记时改判成 `"1"`——因为旧 Agent 节点的 `version=2` 表示工具参数格式，与新 Agent 节点 v2 同号不同义。

**源码事实：** 版本解析的降级是「精确匹配 → 退到 `latest` 哨兵 → 都没有才报错」，即存量工作流在旧版本实现被删后会静默按 latest 运行。

**源码事实：** 应用形态有七种（workflow / chat / completion / advanced_chat / agent / pipeline / 及共用的 base），每种各有 `app_runner` + `app_generator` + `app_config_manager` 三个文件。

**源码事实：** 索引处理器有四种（段落 / 父子 / 问答 / 摘要），共用 `BaseIndexProcessor`，由 `IndexProcessorFactory` 分派。

**源码事实：** 插件通过独立守护进程通信（`BasePluginClient`），模型/工具/数据源/Agent 四类能力各有客户端，另有「反向调用」让插件回调宿主模型。

**架构推论：** 「图引擎外置」这个决策的收益是**内核可以独立演进**（`graphon` 有自己的 bug 修复与发版节奏，注释里那条 pin 就是为了拿一个 container-type 修复），代价是**跨仓调试**——工作流的执行细节要在两个仓之间跳。ragflow 反过来选内联，收益是「一份代码库看全」，代价是内核跟着产品走、不能独立发版。**这两个都是合理选择，但混用会很难受**：如果内核外置却又要频繁打补丁，就会陷入「每次都要等上游」的困境，而 dify 那条注释正说明它确实在等。

**架构推论：** 「Agent 运行时独立成包」是**把重依赖隔开**的典型手法。Agent 循环需要沙箱、文件系统、命令执行这些很重的东西，做成独立进程后可以单独扩容、单独限制资源、单独崩溃重启。代价是**协议成了新的契约面**——`dify-agent/src/dify_agent/protocol/` 与 `api` 侧的应用接入必须保持兼容，而这个契约没有编译期检查（两个包都只是 Python）。

**架构推论：** 测试占 58% 这个比例值得单独看。它说明**这个项目的复杂度主要在「已有行为的保持」而不是「新功能的实现」**——一个应用平台最大的风险是改一处破坏另一处，而平台型产品的回归面（七种应用形态 × 四种索引 × 四类控制器 × 多租户权限）组合爆炸。**测试占比高是这类产品的必然，不是勤奋的证明**。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `api/core/app/apps/base_app_runner.py` | `AppRunner` | 应用执行骨架 |
| `api/core/app/apps/workflow/app_runner.py` | `WorkflowAppRunner` | 工作流形态执行 |
| `api/core/workflow/workflow_entry.py` | `WorkflowEntry` | 交给图引擎的入口 |
| `api/core/workflow/node_factory.py` | `DifyNodeFactory` | 节点类型解析与构造 |
| `api/core/workflow/node_runtime.py` | `DifyPreparedLLM` | 节点运行时依赖装配 |
| `api/core/workflow/generator/runner.py` | `WorkflowGenerator` | 工作流自动生成 |
| `api/core/rag/index_processor/index_processor.py` | `IndexProcessorFactory` | 索引处理器分派 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval` | 多库检索编排 |
| `api/core/plugin/plugin_service.py` | `PluginService` | 插件生命周期 |
| `api/core/tools/tool_manager.py` | `ToolManager` | 工具加载与凭据 |
| `api/core/mcp/mcp_client.py` | `MCPClient` | MCP 客户端 |
| `api/core/agent/fc_agent_runner.py` | `FunctionCallAgentRunner` | 旧式函数调用 Agent |
| `dify-agent/src/dify_agent/runtime/runner.py` | `AgentRunRunner` | 独立 Agent 运行时循环 |
| `api/services/model_provider/service.py` | `ModelProviderService` | 模型提供方 |
| `api/controllers/service_api/wraps.py` | `validate_app_token` | 对外 API 鉴权 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 官方文档](https://docs.dify.ai/)
- [graphon：dify 外置的图执行内核](https://github.com/langgenius/graphon)
- [dify 文档：工作流编排](https://docs.dify.ai/guides/workflow)
- [dify 源码：api/core/workflow/node_factory.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/node_factory.py)

## 与主核 / AI 的关系

dify 与主核（多模态数据库内核）的关系是「**平台与内核**」：平台把内核包装成用户可点的东西，而平台的复杂度几乎全部来自包装。

**七种应用形态 vs 数据库的一种查询接口。** 数据库对外只有一个入口（SQL），所有能力通过语法扩展表达；dify 对内有七种应用形态，每种一套 runner/generator/config_manager。**这是「统一抽象」与「贴近场景」的两种组织方式**：数据库能统一是因为查询语义足够同质；dify 不能统一是因为「对话」「补全」「工作流」「Agent」的交互模型本质不同（有状态 vs 无状态、流式 vs 一次性、可中断 vs 不可中断）。**当场景的差异大于共性时，统一抽象会变成一堆 if。**

**图引擎外置 vs 数据库把执行器内联。** 数据库不会把查询执行器做成外部依赖（那等于把心脏放在体外），因为它需要逐版本调整执行器以配合存储格式。dify 把图引擎外置，因为图执行与它的存储格式正交——**判据是「这个组件是否与我的核心数据格式强耦合」**。耦合就内联，不耦合就外置。这条判据在两边都成立，所以两边做了相反的决定而都对。

**插件守护进程 vs 数据库的扩展（extension）。** PostgreSQL 的扩展是**编进同一进程**的（`.so` 动态加载，共享内存、共享事务）；dify 的插件是**独立进程 + RPC**。差别在故障隔离与信任边界：同进程扩展能参与事务但一崩全崩，独立进程崩了只影响自己但不能共享事务。**dify 选独立进程是因为插件来自第三方**——不可信代码不能在宿主进程里跑。数据库的扩展是 DBA 装的、可信的，所以可以同进程。

**人工介入（human input）vs 数据库的两阶段提交。** 两者都是「流程停下来等外部输入」。数据库的 2PC 有超时与协调者，超时后按规则中止；dify 的人工介入有表单、超时单位、默认值，同样要处理「人不来怎么办」。**差别是数据库的等待方是另一个程序（毫秒级、可靠），dify 的等待方是人（小时级、可能永远不来）**——所以后者必须有更丰富的超时策略与更宽容的恢复路径。

**测试占 58% vs 数据库的回归测试套件。** 数据库有庞大的 `regression` 测试（每个 SQL 特性一组期望输出），dify 有庞大的 pytest。两者都是**用测试固定已有行为**，因为改一处破坏另一处的风险与代码量成正比。**判断一个项目是否成熟，看它的测试是不是主要在防回归而不是在验证新功能**——dify 的这个比例说明它已经进入「保持」而非「扩张」阶段。

对 agent 工程最直接的价值有五条。其一：**图引擎该内联还是外置，取决于它与你的核心数据格式是否强耦合**，不取决于「是否好维护」。其二：**Agent 运行时独立成进程能把重依赖隔开**，代价是多一层协议契约且没有编译期检查。其三：**多形态产品里统一抽象会退化成一堆 if**，不如承认差异、每个形态一套文件。其四：**任何「匹配不到就退到最新」的降级都必须告警**，否则删旧版本会在监控上完全无声。其五：**测试占比过半是平台型产品的常态**，不是过度工程。
