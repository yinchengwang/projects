> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Gap Analysis 子报告 — Agent（多智能体 / ReAct / Pipeline 编排）

> 系列：自研多模态数据库 + RAG「自研 vs 业界」差距分析（13 篇子报告之十三，本系列最后 1 篇）
> 本篇：多 Agent 编排与 RAG Pipeline（agent 三件套 + 5 类 Pipeline + DB 内核 RAG 算子 + 记忆 / 工具 / 人机协作）
> 时间：2026-10-16
> 状态：初稿
> 上游依赖：`archive/.superpowers/sdd/gap-task-1-inventory.md` §13 agent
> 对照 wiki：
> - `docs/agent-wiki/langgraph.md`（LangGraph b292a33a6 前 release，StateGraph + Pregel BSP + Checkpoint + Interrupt + Send API）
> - `docs/agent-wiki/crewai.md`（CrewAI 0.130+，Role / Goal / Backstory / Crew / Process + Flow + Memory + MCP）
> - `docs/agent-wiki/autogen.md`（AutoGen 0.4+，Actor Model + RoutedAgent / ClosureAgent + GroupChat / Magentic-One + Subscription）
> 自身参考：`2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层报告，本篇不替代，仅作纵向对照）

## 摘要（TL;DR）

自研 Agent 栈已经具备 **「Agent 三件套（agent/memory/tool）+ 5 类 Pipeline（ReAct/Iterative/Recursive/Self-RAG/Corrective）+ DB 内核 RAG 算子（chunk/embed/generate/retrieve/rag_executor/rag_pipeline/graphrag_* 4 件套）+ GraphRAG 实体/关系/混合搜索 + Agent 状态机（IDLE/THINKING/ACTING/OBSERVING/FINISHED/FAILED）+ ReAct 步骤（thought/action/action_input/observation）+ 多 Agent 协作（通过 Pipeline 串联与上下文传递）」**，覆盖度评级 **A**——本篇是 13 篇子报告里**自研 Agent 模块与 Pipeline 编排方向唯一的一篇**，且为系列最后 1 篇。与 LangGraph 的差距集中在 **「StateGraph + Pregel BSP 模型 + Channel 抽象（LastValue/Topic/BinaryOperator/NamedBarrier/EphemeralValue）+ Checkpoint 持久化（MemorySaver/SqliteSaver/PostgresSaver）+ Store 跨线程记忆 + Interrupt / Resume HITL + Command API + Send API Map-Reduce + Streaming 5 模式（values/updates/messages/events/debug）+ LangGraph Studio + LangSmith 集成」**；与 CrewAI 的差距集中在 **「Agent 四件套（Role/Goal/Backstory/Tools）+ Task 三件套（description/expected_output/context）+ Crew Process 三件套（Sequential / Hierarchical / Consensus）+ Flow 事件驱动（@start / @listen / @router / @and_ / @or_）+ Memory 四层（ShortTerm/LongTerm/Entity/Contextual）+ Knowledge Source RAG + MCP 集成 + Skills Marketplace + Hooks + CrewAI 5 类 Agent Marketplace」**；与 AutoGen 的差距集中在 **「Actor Model（RoutedAgent / ClosureAgent / SingleThreadedAgentRuntime / DistributedAgentRuntime）+ Subscription 路由（TopicType / TypeSubscription / DefaultSubscription）+ GroupChat（RoundRobin / Selector / Swarm / Magentic）+ 终止条件（MaxMessage / TokenUsage / Timeout / Handoff）+ State Snapshot（save_state / load_state）+ Pause / Resume / Replay + Code Executor 沙箱 + MCP Workbench + ComputerUse + Distributed Runtime gRPC/WebSocket」**。

简言之，自研 agent 是 **「Agent 三件套（agent / memory / tool）+ 5 类学术 Pipeline（ReAct/Iterative/Recursive/Self-RAG/Corrective）+ DB 内核 RAG 算子端到端」的 C/C++ 应用层**，但 **无** 「StateGraph 图编排 + Pregel BSP 模型 + Channel 抽象 + Checkpoint 持久化 + Interrupt HITL + Actor Model + Subscription 路由 + GroupChat 协调 + Crew Process 协作 + Flow 事件驱动 + MCP 集成 + Skills Marketplace + Code Executor 沙箱 + ComputerUse + 多语言 SDK」。LangGraph 是「图编排 + 状态机 + 持久化 + HITL + 流式」标杆，CrewAI 是「角色化 + 任务化 + 流程化 + 工具化」+ 25k star 上升势头猛，AutoGen 是「事件驱动 + GroupChat + Magentic-One + AutoBuild + 35k star + 学术研究底座」标杆。扩展方向应以 **「引入 StateGraph + Channel 抽象 + Checkpoint 持久化 + Interrupt HITL + Process Sequential/Hierarchical/Consensus + Flow @start/@listen/@router 事件驱动 + Memory ShortTerm/LongTerm/Entity/Contextual 四层 + Actor Model + Subscription 路由 + GroupChat RoundRobin/Selector/Magentic + Code Executor 沙箱 + MCP Workbench + Skills Marketplace + 多语言 SDK」** 为最高优先级，因为它直接决定了 `engine/apps/multimodal_rag/src/agent/{agent.cpp, memory.cpp, tool.cpp}` + `engine/apps/multimodal_rag/src/pipeline/{react,iterative,recursive,self_rag,corrective}_pipeline.cpp` 能否从「端到端 C/C++ Agent + Pipeline」跨越到「图编排 + 状态机 + 多 Agent 协作 + 持久化 + 人在回路 + 流式 + 商业 SaaS + 多语言生态」。

---

## 1. 自研现状

> 引用 `archive/.superpowers/sdd/gap-task-1-inventory.md` §13 agent（行 137-144 索引），并补充定位说明。覆盖度 **A**（Agent 三件套 + 5 类 Pipeline + DB 内核 RAG 算子 + GraphRAG 模块齐备）。

1. **Agent 三件套（覆盖度 A）**：
   - `engine/apps/multimodal_rag/src/agent/agent.cpp`（243 行）+ `engine/apps/multimodal_rag/include/mmrag/modular/agent/agent.h`（124 行）实现核心 `Agent` 类（ReAct 推理代理）+ `AgentConfig` 配置（max_iterations / max_retries / temperature / verbose）+ `AgentState` 枚举（IDLE / THINKING / ACTING / OBSERVING / FINISHED / FAILED）+ `ReActStep` 步骤结构（thought / action / action_input / observation）+ `AgentResponse` 响应结构。
   - `engine/apps/multimodal_rag/src/agent/memory.cpp`（88 行）+ `engine/apps/multimodal_rag/include/mmrag/modular/agent/memory.h`（79 行）实现 `Memory` 抽象层（短期/长期/上下文三类记忆的接口契约）。
   - `engine/apps/multimodal_rag/src/agent/tool.cpp`（366 行）+ `engine/apps/multimodal_rag/include/mmrag/modular/agent/tool.h`（189 行）实现 `Tool` 抽象（Function Tool 注册/调用协议 + 多 Tool 工具集组合 + JSON Schema 描述）。

2. **5 类 Pipeline（覆盖度 A，5 文件）**：
   - `engine/apps/multimodal_rag/src/pipeline/react_pipeline.cpp`（544 行，ReAct 循环：Reason → Action → Observation → Loop）；
   - `engine/apps/multimodal_rag/src/pipeline/iterative_pipeline.cpp`（418 行，迭代式检索增强：每次迭代重检索 + 重写 query + 累积证据）；
   - `engine/apps/multimodal_rag/src/pipeline/recursive_pipeline.cpp`（489 行，递归检索：树状查询分解 + 子节点结果聚合）；
   - `engine/apps/multimodal_rag/src/pipeline/self_rag.cpp`（Self-RAG 反思式检索 + 引用 token [Retrieval]/[IsRel]/[IsSup]/[IsUse] 学术协议）；
   - `engine/apps/multimodal_rag/src/pipeline/corrective_pipeline.cpp`（421 行，CRAG Corrective RAG：检索评估 + Web 检索 fallback + 知识精炼 + 答案过滤）；

3. **公共头与接口（覆盖度 A）**：
   - `engine/apps/multimodal_rag/include/mmrag/rag.h`（统一 RAG 公共头）；
   - `engine/apps/multimodal_rag/include/mmrag/pipeline.h`（Pipeline 基类与公共协议）；
   - `engine/apps/multimodal_rag/include/mmrag/self_rag.h`（Self-RAG 反思 token 协议头）；

4. **DB 内核 RAG 算子（覆盖度 A，9 文件）**：
   - `engine/src/db/executor/rag/chunk_op.c`（文档切分算子：按字符 / Token / Sentence / Semantic 四种策略）；
   - `engine/src/db/executor/rag/embed_op.c`（嵌入算子：调用 Embedder 服务 + 批量向量化）；
   - `engine/src/db/executor/rag/generate_op.c`（生成算子：调用 LLM 服务 + 流式输出）；
   - `engine/src/db/executor/rag/retrieve_op.c`（检索算子：Vector + BM25 + Hybrid 三路检索）；
   - `engine/src/db/executor/rag/rag_executor.c`（RAG 算子执行器：算子调度 + 状态机 + 错误处理）；
   - `engine/src/db/executor/rag/rag_pipeline.c`（RAG Pipeline 执行器：端到端编排）；
   - `engine/src/db/executor/rag/graphrag_context.c`（GraphRAG 上下文检索算子：图节点 + 邻居 + 子图）；
   - `engine/src/db/executor/rag/graphrag_entity.c`（GraphRAG 实体抽取算子：NER + 实体归一化）；
   - `engine/src/db/executor/rag/graphrag_relation.c`（GraphRAG 关系抽取算子：实体间关系识别）；
   - `engine/src/db/executor/rag/graphrag_hybrid_search.c`（GraphRAG 混合检索算子：向量 + 图遍历 + BM25 三路融合）。

5. **缺失模块（明确标注）**：与 LangGraph / CrewAI / AutoGen 对比，自研 Agent **无** LangGraph StateGraph 节点 / 边 / 编译 + Pregel BSP 模型 + Channel 抽象（LastValue / Topic / BinaryOperator / NamedBarrier / EphemeralValue）+ Checkpoint 持久化（Memory / SQLite / Postgres）+ Store 跨线程记忆 + Interrupt / Command / Send API + `compile()` + `stream` 5 模式 + LangGraph Studio，**无** CrewAI Agent 四件套（Role / Goal / Backstory / Tools）+ Task 三件套（description / expected_output / agent / context / async_execution）+ Crew Process 三件套（Sequential / Hierarchical / Consensus）+ Flow @start / @listen / @router / @and_ / @or_ 装饰器 + Memory 四层（ShortTerm / LongTerm / Entity / Contextual）+ Knowledge Source RAG + MCP 集成 + Skills Marketplace + Hooks + CrewAI Studio + AMP Cloud，**无** AutoGen Actor Model（RoutedAgent / ClosureAgent / SingleThreadedAgentRuntime / DistributedAgentRuntime）+ Subscription 路由（TopicType / TypeSubscription / DefaultSubscription）+ GroupChat（RoundRobin / Selector / Swarm）+ Magentic-One Ledger / FactSheet / Replan + 终止条件（MaxMessage / TokenUsage / Timeout / Handoff）+ State Snapshot（save_state / load_state）+ Pause / Resume / Replay + Code Executor 沙箱（Local / Docker / E2B）+ MCP Workbench + ComputerUse（OpenAI CUA / Anthropic Computer Use）+ AutoGen Studio，**无** Tool Calling Function / OpenAI Tools / Anthropic Tools / Gemini Tools / Cohere Tools 多 LLM 协议适配，**无** Reflection / Self-Critique / Plan-and-Execute / Structured Outputs Pydantic 风格协议，**无** 商业 Tracing（LangSmith SaaS / Langfuse / Phoenix / TruLens）+ 公开 Bench（BEIR / RAGAS / HotpotQA / TREC-COVID）+ 多语言 SDK（Python / JS / Java / Go / Rust）+ 工具市场（Skills Marketplace / Agent Marketplace / Flow Marketplace）。

**关键路径速查**（与本报告后续引用一致）：

- Agent 三件套：`engine/apps/multimodal_rag/src/agent/{agent.cpp, memory.cpp, tool.cpp}`
- Agent 头：`engine/apps/multimodal_rag/include/mmrag/modular/agent/{agent.h, memory.h, tool.h}`
- 5 类 Pipeline：`engine/apps/multimodal_rag/src/pipeline/{react_pipeline.cpp, iterative_pipeline.cpp, recursive_pipeline.cpp, self_rag.cpp, corrective_pipeline.cpp}`
- DB RAG 算子：`engine/src/db/executor/rag/{chunk_op.c, embed_op.c, generate_op.c, retrieve_op.c, rag_executor.c, rag_pipeline.c, graphrag_context.c, graphrag_entity.c, graphrag_relation.c, graphrag_hybrid_search.c}`
- 公共头：`engine/apps/multimodal_rag/include/mmrag/{rag.h, pipeline.h, self_rag.h}`
- 父模块：`engine/apps/multimodal_rag/`（14 子模块 RAG 服务）
- RAG 报告：`docs/analysis/rag.md`（同期完成的 12 号子报告，14 子模块覆盖）
- 文档报告：`docs/analysis/document.md`（同期完成的 3 号子报告，文档存储对标）

---

## 2. 业界主流做法

下面三篇对照项目分别代表「图编排 + 状态机 + 持久化 + HITL」「角色化 + 任务化 + 流程化 + 工具化 + Flow + Memory + MCP」「事件驱动（Actor Model）+ GroupChat + Magentic-One + AutoBuild」三条路线，重点对比「Agent 三件套 / Pipeline 编排 / 多 Agent 通信 / 记忆系统 / 工具调用协议 / 人机协作 / 状态管理 / 多模态工具 / 商业生态」九个维度。

### 2.1 LangGraph（图编排 + StateGraph + Pregel BSP + Checkpoint + Interrupt HITL）

详细：`docs/agent-wiki/langgraph.md`

- **整体定位**：LangChain Inc.（Harrison Chase）2024-06 开源的「图编排 + 状态化 + 持久化 + 流式」Agent 框架——定位「Production-grade orchestration for long-running, stateful agents」，填补 LangChain LCEL 缺少显式图 + 持久化 + 人在回路的工程空白；13k+ star，500 万+月下载量，LangGraph Studio 桌面/Web IDE + LangGraph Platform 商业矩阵。
- **StateGraph 节点 / 边 / 条件边**：`builder.add_node("node_name", func)` + `add_edge("node_a", "node_b")` + `add_conditional_edges("node_a", routing_func, {"path1": "node_b"})` + `builder.add_edge(START, "node_a")` + `builder.add_edge("node_z", END)` + `graph = builder.compile(...)`；实现位置 `libs/langgraph/langgraph/graph/state.py`。
- **Pregel BSP 执行模型**：Bulk Synchronous Parallel 模型——super-step 内并行执行所有活跃节点 → barrier → 下一 super-step；Channel 触发（节点声明读取的 Channel 有值才触发）；实现位置 `libs/langgraph/langgraph/pregel/_algo.py` + `_loop.py` + `_executor.py`。
- **Channel 抽象（5 类）**：LastValue（保留最后一个值，`current_step` / `agent_name`）+ Topic（保留所有 value list，`messages`）+ BinaryOperatorAggregate（用 add / multiply / union 二元算子累积，`accumulator` / `set_union` / `dict_merge`）+ NamedBarrier（等待多个 Channel 都有值，`reduce` 同步 barrier）+ EphemeralValue（单次 round 用完即清空，中间计算结果）；实现位置 `libs/langgraph/langgraph/channels/`。
- **Checkpoint 持久化**：抽象 `libs/checkpoint/langgraph/checkpoint/base/`（`Checkpoint` + `CheckpointMetadata`）+ 序列化 `serde/`（JSON / pickle / ormsgpack）+ 后端 `MemorySaver` / `SqliteSaver` / `PostgresSaver` / `RedisSaver`；线程（thread_id）+ Checkpoint 链（每个 super-step 一个）+ 时间旅行（time-travel）。
- **Store 跨线程记忆**：`libs/checkpoint/langgraph/store/`（`InMemoryStore` / `PostgresStore`）+ 命名空间（`("users", user_id, "memories")`）+ 向量记忆（`store.search(query="...")` 语义检索）+ Pydantic Schema（结构化记忆）。
- **Interrupt / Command / Send API**：Interrupt（`graph.interrupt(value)` 暂停）+ Resume（`graph.invoke(Command(resume="answer"))`）+ Approval Workflow（删除文件 / 转账 / 邮件审批）+ Edit State（`Command(update={"messages": [HumanMessage(...)]})`）+ Checkpoint Fork（A/B 测试）+ Send API（`Send("worker_node", {"input": item})` Map-Reduce 动态扇出）+ Command（`Command(goto="next_node", update={"key": value})`）。
- **Streaming API（5 模式）**：`graph.stream()` / `astream()` + `mode="values"` / `"updates"` / `"messages"` / `"events"` / `"debug"` / `"custom"`；流式 token-by-token 输出；LangSmith 自动 trace + LangGraph Studio 可视化 step-through。
- **Subgraph 嵌套**：`add_node("sub", subgraph.compile())` + 父子图通信通过父图 state channel 映射；每个 subgraph 独立 state schema。
- **API 与生态**：`langgraph.graph.StateGraph` / `langgraph.pregel.Pregel` / `langgraph.checkpoint.{MemorySaver, SqliteSaver, PostgresSaver}` / `langgraph.store.{InMemoryStore, PostgresStore}` / `langgraph.types.{Command, Interrupt, Send}` / `langgraph.func.{entrypoint, task}` + LangGraph SDK（Python + JS）+ LangGraph CLI（`langgraph dev / up`）+ LangGraph Studio + LangGraph Platform 托管 + LangSmith SaaS 可观测性 + LangChain VectorStore + LangChain Tools。

### 2.2 CrewAI（角色化 + 任务化 + 流程化 + 工具化 + Flow + Memory + RAG + MCP）

详细：`docs/agent-wiki/crewai.md`

- **整体定位**：João Moura（巴西开发者）2023-11 开源的「多 Agent 协作框架」——以「Crew（团队）+ Agent（角色）+ Process（流程）+ Tools（工具）」四要素为核心，让用户用最少的代码组装出可生产部署的多 Agent 系统；25k+ star，800 万+月下载量，AMP Cloud + CrewAI Enterprise 商业矩阵；2025-08 起 Crews 2.0 / Agents 2.0 + Flow + Memory + RAG + MCP + Skills + Hooks。
- **Agent 四件套（Role + Goal + Backstory + Tools）**：`Agent(role="...", goal="...", backstory="...", tools=[...], llm=..., allow_delegation=...)` 实现位置 `lib/crewai/src/crewai/agent.py`；`LiteAgent`（轻量）+ `Agents 2.0`（多 Agent 实现于 `lib/crewai/src/crewai/agents/`）+ `reflection=True` 反思 + `output_pydantic=MyModel` 结构化输出。
- **Task 三件套（description / expected_output / context）**：`Task(description="...", expected_output="...", agent=..., context=[other_task1, other_task2], tools=[...], async_execution=True, human_input=True)` 实现位置 `lib/crewai/src/crewai/task.py`；上下文自动注入（上游 output → 下游 context）+ Async Execution 并行执行。
- **Crew Process 三件套**：Sequential Process（任务按顺序依次执行，下游 Task context 是上游 output）+ Hierarchical Process（Manager Agent 委派 Worker，Manager LLM 自动选择）+ Consensus Process（多个 Agent 投票达成共识）；实现位置 `lib/crewai/src/crewai/process.py`。
- **Flow 事件驱动**：`@start()` / `@listen()` / `@router()` / `@and_()` / `@or_()` 装饰器；`flows/`（Flow 引擎 / State / Events / 状态持久化）；`flow.state()` 访问状态；自动 pickle / JSON 保存到 `flow.state` 文件；典型场景：CRM / ERP / 营销自动化工作流。
- **Memory 四层**：Short-term Memory（最近 N 个任务输出，默认 5 个，内存 deque）+ Long-term Memory（Chroma / SQLite / Redis，OpenAI embeddings，长期）+ Entity Memory（spaCy NER 抽取实体 + 去重）+ Contextual Memory（任务上下文 + RAG 检索）；实现位置 `lib/crewai/src/crewai/memory/`。
- **Knowledge Source RAG**：`knowledge/`（CrewAI Knowledge Source）+ PDF / Web / DB / Confluence / Notion / GitHub / Slack 源 + `Chroma` 向量库；`crew = Crew(..., knowledge_sources=[pdf_source, web_source])`；PyPDF 解析 + RecursiveCharacterTextSplitter / SemanticSplitter + 默认 chunk_size 500-1500。
- **MCP（Model Context Protocol）**：`mcp/`（MCP 客户端 / 服务器）+ `MCPServer` 类支持 SSE / Stdio 模式；连接外部 MCP 服务器（filesystem / GitHub / Slack / Google Drive / Notion）。
- **Skills Marketplace**：`skills/`（预制 Agent 技能）+ 100+ Skills（搜索 / 摘要 / 翻译 / 校对）+ Agent Marketplace（社区 Agent 模板，销售 / 客服 / 营销 / 财务 / HR）+ Flow Marketplace（社区 Flow 模板）。
- **Hooks（Pre / Post）**：`hooks/`（Pre / Post 钩子）+ `@before_step / @after_step` 装饰器 + 自定义 Agent 加 `before_execute / after_execute` 钩子 + Custom HITL（用户自定义审批）。
- **MCP / AMP Cloud / Enterprise**：`mcp/` MCP 集成 + `amp.crewai.com` 商业托管（部署 + 监控 + Scaling + Approval UI）+ `enterprise.crewai.com` 企业版（合规 SOC2 / HIPAA / GDPR + 安全 SSO / RBAC / Audit + 私有部署）。
- **CLI**：`crewai create my_crew`（创建项目）+ `crewai run / test / train / deploy / chat / flow` + CrewAI Studio Web IDE + AMP Cloud 托管。

### 2.3 AutoGen（事件驱动 Actor Model + GroupChat + Magentic-One + AutoBuild）

详细：`docs/agent-wiki/autogen.md`

- **整体定位**：Microsoft Research（Chi Wang 团队）2023-08 开源的「事件驱动 + 可对话 + 可自定义」多 Agent 协作框架——以「Agent + Chat + Event + Runtime」四要素为核心，支持 GroupChat / Magentic-One / Swarm / AutoBuild 四种对话模式；35k+ star，200 万+月下载量；AutoGen Studio Web IDE + Magentic-One CLI。
- **Actor Model（RoutedAgent + ClosureAgent + Runtime）**：`autogen-core/src/autogen_core/base.py` Agent 抽象 + `RoutedAgent` 事件驱动 Agent + `ClosureAgent` 闭包 Agent + `SingleThreadedAgentRuntime` 单进程运行时 + `DistributedAgentRuntime` 分布式运行时（gRPC + Redis）。
- **Subscription 路由（事件驱动核心）**：`TopicType`（消息主题，如 `"task_started"` / `"agent_response"`）+ `TypeSubscription`（Agent 订阅特定 Type 消息）+ `DefaultSubscription`（默认全部消息）+ Message Router（根据 Subscription 路由消息到对应 Agent）+ Message Queue（每 Agent 一个 in-memory queue）；实现位置 `autogen-core/src/autogen_core/base.py` + `_agent_runtime.py` + `_agent_instantiation.py`。
- **GroupChat 群聊协调**：`RoundRobinGroupChat`（轮询 Speaker Selection）+ `SelectorGroupChat`（LLM-based Speaker Selection）+ `SwarmGroupChat`（Handoff 风格）+ `MagenticOneGroupChat`（自适应规划 + 自适应重规划）；实现位置 `autogen-agentchat/src/autogen_agentchat/teams/`；状态共享（所有 Agent 共享 Team Context）。
- **Magentic-One（复杂任务自适应编排）**：`autogen-magentic-one/`（Magentic-One Orchestrator + Coder / WebSurfer / FileSurfer / Executor / ComputerTerminal）+ Ledger（进度跟踪）+ FactSheet（计划）+ Replan（自适应重规划）；典型场景：软件开发 / 数据分析 / 网络研究。
- **ConversableAgent + Reply**：`AssistantAgent`（默认 ReAct 循环：Reason → Action → Observation → Reason）+ `UserProxyAgent`（人类作为 Agent 参与对话，`human_input_mode="ALWAYS" / "TERMINATE" / "NEVER"`）+ `CodeExecutorAgent`（LLM 生成代码 + Local / Docker / E2B 沙箱执行）。
- **终止条件**：`MaxMessageTermination(10)`（最大消息数）+ `TokenUsageTermination`（token 上限）+ `TimeoutTermination`（超时）+ `HandoffTermination`（handoff 终止）；实现位置 `autogen-agentchat/src/autogen_agentchat/conditions/`。
- **State Snapshot + Pause / Resume / Replay / Cancellation**：`state = await team.save_state()` / `load_state(state)` / `await team.pause() / resume()` / `team.run_stream(..., replay_state=state)` / `await task.cancel()`；实现位置 `autogen-agentchat/src/autogen_agentchat/state/`。
- **MCP Workbench + Tool Error Handling**：`autogen-ext/src/autogen_ext/mcp/`（`McpWorkbench` / `McpToolAdapter`，连接外部 MCP 服务器）+ `tool_call_max_consecutive_failure=3` 连续失败上限 + `reflect_on_tool_use=True` 双模型自反思。
- **ComputerUse（v0.5+）**：`autogen-ext-computer-use` + `ComputerUseAgent`（OpenAI CUA / Anthropic Computer Use）+ 自动操作系统 + 多模态（图像 / 视频 / 音频 / 文本）。
- **API 与生态**：`autogen_agentchat.agents.{AssistantAgent, UserProxyAgent, CodeExecutorAgent}` + `autogen_agentchat.teams.{RoundRobinGroupChat, SelectorGroupChat, Swarm, MagenticOneGroupChat}` + `autogen_agentchat.conditions.{MaxMessageTermination, TokenUsageTermination, TimeoutTermination, HandoffTermination}` + `autogen_core.{RoutedAgent, ClosureAgent, AgentRuntime, TopicType, TypeSubscription, DefaultSubscription}` + `autogen_core.models.{OpenAIChatCompletionClient, AnthropicChatCompletionClient}` + `autogen_core.tools.FunctionTool` + `autogen-ext-{openai, anthropic, azure, ollama, litellm, cache-disk, cache-redis, tools-*, magentic-one}` + AutoGen Studio Web IDE + Magentic-One CLI + RPC（gRPC / WebSocket / Redis / Kafka）。

---

## 3. 24 维度对照矩阵

> 等级：A=工业级，缺失≤1 个版本号差距 / B=教学+工程可用，关键能力缺失 / C=占位，仅 API / D=空。
> 本表覆盖自研 vs LangGraph vs CrewAI vs AutoGen 四方对照。

| # | 维度 | 自研（book multimodal_rag/agent + pipeline + DB RAG 算子） | LangGraph | CrewAI | AutoGen | 等级 | 关键说明 |
|---|------|-------------------------------------------------------|-----------|--------|---------|------|------|
| 1 | 项目定位 | C/C++ Agent 三件套 + 5 类 Pipeline + DB RAG 算子端到端应用层 | 图编排 + 状态机 + 持久化 + 人在回路 + 流式（Python 13k star） | 角色化 + 任务化 + 流程化 + 工具化 + Flow + Memory + RAG + MCP（Python 25k star） | 事件驱动 Actor Model + GroupChat + Magentic-One + AutoBuild（Python 35k star） | A | 自研 Agent + Pipeline 学术方法实现最齐 |
| 2 | 发展历史 | 单一维护者内部迭代 | 2024-06 起，2 年 | 2023-11 起，3 年 | 2023-08 起，3 年 | C | 历史沉淀薄，但迭代节奏自定 |
| 3 | 许可限制 | 内部使用 | MIT + LangSmith/LangGraph Platform 商业 | MIT + CrewAI Enterprise/AMP 商业 | MIT + AutoGen Enterprise 规划中 | A | 自研无开源义务 |
| 4 | 使用方案例 | 内部测试 + 多模态 demo | Uber / LinkedIn / Klarna / Replit / Elastic | Oracle / DocuSign / KPMG / PwC / IBM | Microsoft Research + 学术研究多 + 企业 | C | 缺外部生产案例 |
| 5 | 对比定位 | 「Agent 三件套 + 5 类学术 Pipeline + DB 内核 RAG 算子」 | 「StateGraph + Pregel BSP + Checkpoint + Interrupt + Send API」 | 「Crew + Agent + Process + Tool + Flow + Memory + RAG + MCP + Skills + Hooks」 | 「Actor Model + RoutedAgent + Subscription + GroupChat + Magentic-One + AutoBuild + Code Executor + MCP」 | A | 自研 5 类 Pipeline 是亮点 |
| 6 | **整体架构** | **Agent 三件套（agent/memory/tool）+ 5 类 Pipeline（ReAct/Iterative/Recursive/Self-RAG/Corrective）+ DB 内核 10 个 RAG 算子（chunk/embed/generate/retrieve/rag_executor/rag_pipeline/graphrag_* 4 件套）+ cpp-httplib Server** | **StateGraph 节点 / 边 / 编译 + Pregel BSP 模型 + 5 类 Channel（LastValue/Topic/BinaryOperator/NamedBarrier/EphemeralValue）+ Checkpoint 持久化 + Store 跨线程记忆 + Interrupt/Command/Send API + LangGraph Studio + LangSmith SaaS** | **Agent 4 件套（Role/Goal/Backstory/Tools）+ Task 3 件套（description/expected_output/context）+ Crew Process 3 件套（Sequential/Hierarchical/Consensus）+ Flow 5 装饰器（@start/@listen/@router/@and_/@or_）+ Memory 4 层（ShortTerm/LongTerm/Entity/Contextual）+ Knowledge Source RAG + MCP + Skills + Hooks + CrewAI Studio + AMP Cloud** | **Actor Model（RoutedAgent/ClosureAgent）+ Subscription 路由（TopicType/TypeSubscription/DefaultSubscription）+ GroupChat（RoundRobin/Selector/Swarm/Magentic）+ Termination（MaxMessage/TokenUsage/Timeout/Handoff）+ State Snapshot + Pause/Resume/Replay + Code Executor 沙箱 + MCP Workbench + Computer Use + AutoGen Studio** | **B+** | **自研 Agent + 5 Pipeline + DB RAG 算子端到端；缺 StateGraph 图编排 + Channel 抽象 + Pregel BSP + Checkpoint 持久化 + Store 跨线程记忆 + Interrupt HITL + Crew Process + Flow 事件驱动 + Memory 4 层 + Actor Model + Subscription + GroupChat + Code Executor + MCP + Skills Marketplace** |
| 7 | Agent 抽象 | `Agent` 类（AgentConfig + AgentState 6 枚举 + ReActStep 步骤 + AgentResponse）+ `Memory` 接口 + `Tool` 抽象（366 行） | `langgraph.graph.StateGraph` / `langgraph.pregel.Pregel` + `RoutedAgent` 风格节点 + `langgraph.func.entrypoint / task` 函数式 API | `Agent(role, goal, backstory, tools, llm, allow_delegation)` + `LiteAgent` 轻量 + `Agents 2.0` 多 Agent + `reflection=True` | `RoutedAgent`（事件驱动） + `ClosureAgent`（闭包） + `AssistantAgent`（默认 ReAct） + `UserProxyAgent` + `CodeExecutorAgent` + `LiteAgent` | B+ | 自研 Agent 三件套有；缺 LangGraph RAG 节点化抽象 + CrewAI 4 件套 + AutoGen RoutedAgent 事件驱动 |
| 8 | Memory 系统 | `Memory` 接口层（短期/长期/上下文三类接口契约）+ `memory.h`（79 行）+ `memory.cpp`（88 行） | Channel Topic（`messages: Annotated[list, add_messages]`）+ `InMemoryStore` / `PostgresStore` 跨线程 + 向量记忆 + Pydantic Schema + `MemorySaver / SqliteSaver / PostgresSaver` Checkpoint | Short-term Memory（最近 N 默认 5 内存 deque）+ Long-term Memory（Chroma / SQLite / Redis）+ Entity Memory（spaCy NER 去重）+ Contextual Memory（RAG over memory） | Team Context（Team 内 Agent 共享）+ Model Context（LLM Messages/Tools）+ Disk/Redis Cache Store + RAG over Memory（Chroma） | B | 自研 Memory 接口层简洁；缺 LangGraph Channel + Store + Checkpoint 多层 + CrewAI 4 层 + AutoGen Team Context + Cache Store + Chroma 集成 |
| 9 | Tool Calling 协议 | `Tool` 抽象（189 行）+ Tool 注册/调用协议 + JSON Schema 描述 + 多 Tool 工具集组合 | `ToolNode` + `tools_condition` + ReAct `create_react_agent(llm, tools)` + CodeAct `PythonREPL` + E2B / Docker sandbox + Tool Error Handling `handle_tool_call_error` + RetryPolicy | `Tool` / `BaseTool` / `@tool("my_tool")` + `Toolkit`（100+ 内置）+ Agent-as-Tool + CodeInterpreterTool（本地 / E2B）+ MCP `MCPServer` + `human_input=True` + Tool 错误处理 | `FunctionTool` / `Tool` / `AgentTool` + `McpWorkbench` + CodeExecutor（本地 / Docker / E2B）+ `tool_call_max_consecutive_failure=3` + `reflect_on_tool_use=True` | B | 自研 Tool 抽象简洁；缺 LangGraph ToolNode + CrewAI 100+ Tool + MCP 集成 + AutoGen FunctionTool + E2B / Docker sandbox + 错误处理/重试 |
| 10 | Human-in-the-Loop（HITL） | `human_input=True` Task 风格未直接实现；Agent 通过 `verbose` 配置间接暴露 | `Interrupt`（`graph.interrupt(value)`）+ `interrupt_before / interrupt_after` + `Command(resume="...")` 恢复 + Approval Workflow（删除 / 转账 / 邮件）+ Dynamic Interrupt（LLM 输出触发）+ Edit State + Checkpoint Fork | `HumanInput` Tool + `Task(human_input=True)` + Flow `@router(after_commit)` 等待审批 + CLI `crewai chat` 交互 + AMP Approval UI | `UserProxyAgent(human_input_mode="ALWAYS/TERMINATE/NEVER")` + `agent.human_input("Continue? [y/n]:")` + Studio HITL UI + Magentic-One Human Approval | D | 自研无显式 HITL 接口；缺 LangGraph Interrupt / Command / Approval / Resume + CrewAI HumanInput Tool / Flow 审批 + AutoGen UserProxyAgent |
| 11 | **图执行引擎（Graph Execution Engine）** | **5 类 Pipeline 顺序 / 循环 / 递归执行 + Agent 单点 ReAct 循环** | **Pregel BSP 模型（super-step 并行执行 → barrier → 下一 super-step）+ Channel 触发 + Subgraph 嵌套 + Concurrent Executor（同步 / 异步 / ConcurrentExecutor / AsyncConcurrentExecutor）+ Streaming API（astream values/updates/messages/events/debug/custom）+ LangGraph Studio 可视化 step-through + time-travel** | **Crew.kickoff() / kickoff_async() / kickoff_for_each() + Process Sequential / Hierarchical / Consensus + Async / Parallel + Task async_execution + Flow @start/@listen/@router 事件驱动 + Flow State 自动 pickle 持久化** | **Team.run_stream() 异步流式 + GroupChat 协调（RoundRobin/Selector/Swarm/Magentic）+ Termination Conditions + Cancellation + State Snapshot + Reentrant** | **C** | **自研 5 类 Pipeline 是序贯循环；缺 Pregel BSP + Channel 触发模型 + ConcurrentExecutor 并行调度 + Subgraph 嵌套 + Streaming 5 模式 + LangGraph Studio 可视化 + Crew Process Sequential/Hierarchical + Flow 装饰器 + AutoGen GroupChat 协调** |
| 12 | 状态管理（State Management） | `AgentState` 6 枚举（IDLE/THINKING/ACTING/OBSERVING/FINISHED/FAILED）+ `ReActStep` 步骤历史 | StateGraph State schema（Pydantic / TypedDict）+ Channel 状态（LastValue / Topic / BinaryOperatorAggregate / NamedBarrier / EphemeralValue）+ Checkpoint 链（每 super-step 一个）+ Thread（thread_id）+ Run（run_id）+ Time-travel | Crew State（任务 pending/running/completed/failed）+ Output（每 Task output）+ Context（上下 Task output 自动注入）+ Flow State 自动 pickle 持久化 + flow.state() + Run（kickoff/async/for_each） | Team State（Team 内所有 Agent state + Messages）+ Agent State + Pause/Resume + Replay + Cancellation + Subscription State | B- | 自研 AgentState 6 枚举基本够用；缺 Channel 5 类 + Checkpoint 持久化 + Thread/Run + Time-travel + Flow State 持久化 + Team State |
| 13 | **多 Agent 通信（Multi-Agent Communication）** | **通过 Pipeline 串联 + 上下文传递 + Agent 单点 ReAct 无多 Agent 协议** | **共享 State（blackboard）+ Channel 消息总线（messages channel）+ A2A 协议（RemoteGraph 跨进程通信）+ Send API 异步分发 + HTTP API（RemoteGraph 客户端）** | **共享 State（blackboard）+ 消息总线（messages channel = ShortTerm Memory）+ Hierarchical Manager 委派（Manager LLM 自动选 Worker）+ Handoff（Handoffs）+ A2A 协议（Remote Crew MCP Server / HTTP Server）** | **Message Bus（Subscription 发布订阅）+ Shared State（Agent 间共享）+ Handoff（SwarmGroupChat）+ A2A 协议（Runtime RPC）+ Remote Agent（RpcAgentClient / RpcAgentServer）+ gRPC / WebSocket / Redis / Kafka** | **C** | **自研 Pipeline 串联 + 上下文传递非真正多 Agent 通信；缺 LangGraph State 共享 + Channel + RemoteGraph；CrewAI Manager 委派 + Handoff + Remote Crew；AutoGen Message Bus + Subscription + Runtime RPC + gRPC/WebSocket** |
| 14 | ReAct / Reasoning 循环 | ReAct 主循环（react_pipeline.cpp 544 行，Thought → Action → Observation）+ Agent 类内置 ReAct 状态机 | `create_react_agent(llm, tools)` helper（LangGraph 内置）+ ReAct StateGraph + 自定义 StateGraph 节点 + `ToolNode` | Agent 默认 ReAct 循环（Thought → Action → Observation）+ `reasoning` 参数 + `reflection=True` | `AssistantAgent` 默认 ReAct 循环 + `reflect_on_tool_use=True` 双模型自反思 | A | 自研 ReAct 主实现完整；缺 LangGraph create_react_agent helper + CrewAI reflection + AutoGen 双模型反思 |
| 15 | Self-RAG / Reflection | `self_rag.cpp`（Self-RAG 反思式检索 + `[Retrieval]` / `[IsRel]` / `[IsSup]` / `[IsUse]` token 学术协议） | 自定义 StateGraph 节点加 `reflect` 节点（评估工具结果）+ retry / continue / fallback | `Agent(reflection=True)` 完成任务后反思 + `summarize_output=True` + RAG over Memory | `reflect_on_tool_use=True`（双模型自反思）+ `AssistantAgent` default ReAct + Magentic-One Replan | A- | 自研 Self-RAG 学术实现完整；缺 LangGraph reflect 节点 + CrewAI reflection 默认 + AutoGen 双模型反思 |
| 16 | Corrective / CRAG | `corrective_pipeline.cpp`（421 行，检索评估 + Web 检索 fallback + 知识精炼 + 答案过滤） | 自定义 Conditional Edge（`add_conditional_edges`）+ `Interrupt` 决策 + RetryPolicy | `Task` 加 fallback Task + 自定义 Process + Hook 后置 | `SelectorGroupChat` 自适应选择 + Magentic-One Replan + AutoBuild 重新生成 Agent | A- | 自研 CRAG 学术实现完整；缺 LangGraph Conditional Edge + CrewAI Fallback + AutoGen Adaptive |
| 17 | Recursive / Iterative | `iterative_pipeline.cpp`（418 行，迭代检索增强）+ `recursive_pipeline.cpp`（489 行，递归检索树状查询分解 + 子节点聚合） | 自定义循环 Node + StateGraph 循环边（`add_edge` 自环）+ Subgraph 嵌套循环 | `async_execution=True` 并行 Task + `Kickoff for_each` 批量 + Flow `and_` / `or_` 并行 | `MagenticOneGroupChat` Ledger 跟踪 + 多 Agent 并行 + Replan 重新规划 | A- | 自研 Iterative + Recursive 学术实现完整；缺 LangGraph 循环边 + CrewAI async_execution + AutoGen Magentic-One Replan |
| 18 | Knowledge Graph / GraphRAG | `engine/src/db/executor/rag/graphrag_{entity,relation,context,hybrid_search}.c` 4 件套图算子 + 自研 KG（`engine/apps/multimodal_rag/src/graph/` 5 文件）+ `graph_pipeline.cpp` | 自定义 Node 加 Graph RAG + Knowledge Graph Index + Store + 向量记忆 | Knowledge Source + RAG Tool + Vector Knowledge | `autogen-ext-tools-rag-*` + `RagTool` / `RagAgent` + Chroma / FAISS | A- | 自研 GraphRAG 算子 + KG 三件套；缺 LangGraph Store + CrewAI Knowledge Source + AutoGen RAG Tool |
| 19 | Code Execution / Sandbox | 无（Agent 三件套无代码执行） | `langchain_experimental.utilities.PythonREPL` + E2B / Docker / Modal sandbox + 自定义 CodeAct Node | `CodeInterpreterTool`（本地 PythonREPL）+ `E2BCodeInterpreterTool`（E2B sandbox） | `CodeExecutorAgent`（LLM 生成代码）+ `LocalCommandLineCodeExecutor`（本地）+ `DockerCommandLineCodeExecutor`（Docker）+ E2B | D | 自研无代码执行；缺 LangGraph PythonREPL + E2B + CrewAI CodeInterpreterTool + AutoGen CodeExecutorAgent + Docker 沙箱 |
| 20 | MCP（Model Context Protocol） | 未集成（Tool 抽象可扩展但 MCP 客户端缺） | LangGraph 通过 LangChain 集成 MCP 服务器 + RemoteGraph 远程 | `mcp/`（MCPServer SSE / Stdio）+ 连接外部 MCP 服务器（filesystem / GitHub / Slack / Google Drive） | `autogen-ext/src/autogen_ext/mcp/`（McpWorkbench / McpToolAdapter） | D | 自研无 MCP 集成；缺 LangGraph LangChain MCP + CrewAI MCPServer + AutoGen McpWorkbench |
| 21 | Skills / Hooks / Marketplace | 无（Tool 抽象可注册但 Skills Marketplace 缺） | LangChain Hub（Prompt 仓库）+ CallbackHandler / BaseCallbackManager + 自定义 Hook | Skills Marketplace（100+ Skills 搜索 / 摘要 / 翻译 / 校对）+ Hooks（@before_step / @after_step）+ Agent Marketplace + Flow Marketplace + Custom Hooks | 自定义 Tool / Agent / Subscription + Magentic-One CLI 一键脚本 + AutoBuild 自动生成 Agent | D | 自研无 Skills Marketplace；缺 LangChain Hub + CrewAI 100+ Skills + Agent Marketplace + AutoGen AutoBuild |
| 22 | Streaming / Observability | `observability/` 子模块（logger.cpp + metrics.cpp 共 2 文件）+ `verbose` Agent 配置 | `stream()` / `astream()` 5 模式 + `astream_events` 细粒度事件 + LangSmith 自动 trace + LangGraph Studio 可视化 step-through + OpenTelemetry | `streaming_callback=True` Generator + `kickoff_async()` 流式 + Langfuse + OpenTelemetry + CrewAI Observability + AMP Cloud 监控 | `team.run_stream()` 异步流式 + OpenTelemetry + AutoGen Studio Web UI + Magentic-One CLI 实时输出 | B- | 自研 logger + metrics 2 文件；缺 LangGraph 5 模式 + LangSmith + LangGraph Studio + OpenTelemetry + Streaming + Langfuse |
| 23 | 核心论文 / 协议 | ReAct（Yao 2022，自研实现）+ Self-RAG（Asai 2023）+ CRAG（Yan 2024） | StateGraph + Pregel BSP 模型 + Send API + Interrupt / Resume | Multi-Agent Collaboration: A Survey + Process Sequential / Hierarchical | Multi-Agent LLM Collaboration（Microsoft Research 2023）+ Magentic-One（2024） | A | 自研追踪全部论文，但缺独立 StateGraph / Pregel BSP / Actor Model 学术研究 |
| 24 | 源码路径（对照） | `engine/apps/multimodal_rag/src/agent/{agent.cpp, memory.cpp, tool.cpp}` + `pipeline/{react,iterative,recursive,self_rag,corrective}_pipeline.cpp` + `engine/src/db/executor/rag/*.c`（10 文件） | `libs/langgraph/langgraph/{graph/state.py, pregel/{_algo.py,_loop.py,_executor.py}, channels/{last_value.py,topic.py,binop.py,named_barrier.py,ephemeral_value.py}, types.py}` + `libs/checkpoint/langgraph/checkpoint/{base/, memory/, serde/}` | `lib/crewai/src/crewai/{agent.py, task.py, crew.py, process.py, flow/, memory/, knowledge/, rag/, mcp/, skills/, hooks/, telemetry/}` | `python/packages/autogen-core/src/autogen_core/{base.py, _agent_runtime.py, _agent_instantiation.py, cache_store.py}` + `autogen-agentchat/src/autogen_agentchat/{agents/, teams/, conditions/, state/, messages/}` + `autogen-magentic-one/` + `autogen-ext/` | A | 自研 3+5+10 文件 + 公开头清晰，路径一一对应可对照 wiki 学习 |
| 25 | 与本项目关联 | 即本报告，Agent + Pipeline + DB RAG 算子 | 「图编排 + 状态机 + 持久化 + HITL + 流式」是 Agent 编排主流 | 「角色化 + 流程化 + Flow + Memory + RAG + MCP + Skills」25k star 上升势头猛 | 「事件驱动 + GroupChat + Magentic-One + AutoBuild + 学术研究」最学术 | A | 5 Pipeline + DB RAG 算子端到端对齐 |

> 注：上表实际为 25 行（含「与本项目关联」一行扩展），24 维度主表保持不变。

**矩阵读法**：

- **等级 A**：维度 1 / 5 / 14 / 15 / 16 / 17 / 18 / 23 / 24 / 25（共 10 项是 A / A-）—— 本篇是 13 子报告里维度 14（ReAct）+ 15（Self-RAG）+ 16（Corrective/CRAG）+ 17（Iterative/Recursive）四件 A 的旗舰篇，5 类学术 Pipeline 自研实现最完整
- **等级 B**：维度 2 / 3 / 7 / 8 / 9（共 5 项）
- **等级 B-**：维度 12 / 22（共 2 项）
- **等级 C**：维度 11 / 13（共 2 项）
- **等级 D**：维度 10 / 19 / 20 / 21（共 4 项）

**整体评级：B+**——**Agent 三件套 + 5 类学术 Pipeline（ReAct/Iterative/Recursive/Self-RAG/Corrective）+ DB 内核 10 个 RAG 算子端到端齐备**，但**缺**维度 10（LangGraph Interrupt/Command/Approval/Resume + CrewAI HumanInput Tool + AutoGen UserProxyAgent HITL）+ 维度 11（Pregel BSP + Channel 触发 + ConcurrentExecutor 并行调度 + Streaming 5 模式 + LangGraph Studio + Subgraph 嵌套 + Crew Process Sequential/Hierarchical + Flow 装饰器 + AutoGen GroupChat 协调）+ 维度 19（Code Executor 沙箱）+ 维度 20（MCP）+ 维度 21（Skills Marketplace + Hooks + Agent Marketplace + AutoBuild）五块 D 级补丁，以及维度 13（多 Agent 通信：State 共享 + Channel + Manager 委派 + Handoff + Message Bus + Subscription + Runtime RPC）一块 C 级补丁；建议下月把 P0 集中在维度 10（HITL Interrupt/Command/Resume）+ 维度 11（图执行引擎 Pregel BSP + Channel + ConcurrentExecutor + Streaming 5 模式）+ 维度 13（多 Agent 通信 State 共享 + Manager 委派 + Message Bus）这三个等级 D/C 的联动。

---

## 4. 关键差距 Top-5

每条都按「自研代码路径 ↔ 业界做法 ↔ 差距量级 ↔ 影响」四段式给出，并附仓库路径与 wiki 路径两个引用，方便后续任务直接卡条。

### 4.1 StateGraph 图编排 + Pregel BSP 模型 + Channel 抽象 + Checkpoint 持久化 + Interrupt HITL 协议标准化全链路空白（等级 C → A）

- **自研现状**：`engine/apps/multimodal_rag/src/agent/agent.cpp`（243 行）+ `memory.cpp`（88 行）+ `tool.cpp`（366 行）实现 Agent 三件套 + `AgentState` 6 枚举（IDLE/THINKING/ACTING/OBSERVING/FINISHED/FAILED）+ `ReActStep` 步骤；`engine/apps/multimodal_rag/src/pipeline/`（5 文件）实现 5 类 Pipeline（react/iterative/recursive/self_rag/corrective）+ `engine/src/db/executor/rag/`（10 文件）DB 内核 RAG 算子；但**无** StateGraph 节点 / 边 / 条件边（`add_node / add_edge / add_conditional_edges / START / END`）+ Pregel BSP 模型（Bulk Synchronous Parallel）+ Channel 抽象（LastValue / Topic / BinaryOperatorAggregate / NamedBarrier / EphemeralValue）+ Checkpoint 持久化（MemorySaver / SqliteSaver / PostgresSaver）+ Store 跨线程记忆（InMemoryStore / PostgresStore）+ Interrupt / Command / Send API + 5 模式 Streaming（values / updates / messages / events / debug）+ LangGraph Studio + LangSmith SaaS。
- **业界做法**：LangGraph `libs/langgraph/langgraph/graph/state.py`（StateGraph + `add_node / add_edge / add_conditional_edges / compile`）+ `libs/langgraph/langgraph/pregel/{_algo.py, _loop.py, _executor.py}`（Pregel BSP 模型 super-step 并行 → barrier → 下一 super-step）+ `libs/langgraph/langgraph/channels/{last_value.py, topic.py, binop.py, named_barrier.py, ephemeral_value.py}`（5 类 Channel）+ `libs/checkpoint/langgraph/checkpoint/{base/, memory/, serde/}`（Checkpoint 抽象 + Memory / SQLite / Postgres 后端）+ `libs/langgraph/langgraph/types.py`（`Command` / `Interrupt` / `Send`）+ LangGraph Studio（桌面 / Web IDE）+ LangSmith（商业 SaaS 自动 trace）。
- **差距量级**：约 12-18 个文件 / 12000-20000 行 C；包含 StateGraph 节点 / 边 / 编译（`state_graph.{h,c}`）+ Pregel BSP 主循环（`pregel_loop.{h,c}` super-step 并行 → barrier）+ Channel 5 类（`channel_last_value.{h,c}` + `channel_topic.{h,c}` + `channel_binop.{h,c}` + `channel_named_barrier.{h,c}` + `channel_ephemeral_value.{h,c}`）+ Checkpoint 抽象 + SqliteSaver / PostgresSaver + Store 跨线程 + Interrupt / Command / Send API + Streaming 5 模式（`stream_values / updates / messages / events / debug`）+ LangGraph Studio Web IDE + LangSmith SaaS 客户端。
- **影响**：Pipeline 之间**无**统一 `compile()` 接口（每次必须 `new Pipeline()` 显式实例化）；**无**Channel 触发模型（节点只能被显式调用，不能按 channel 是否可用触发）；**无**Checkpoint 持久化（任务中断后状态丢失，无法 time-travel）；**无**Interrupt HITL（删除文件 / 转账 / 邮件等高风险操作无审批流）；**无**Send API Map-Reduce（无法动态扇出 worker）；**无**Streaming 5 模式（生产中只能 pull，不能 stream values/updates/messages/events）；与 LangGraph Studio 可视化 + LangSmith SaaS trace 完全脱钩；这是「从 C/C++ Agent 应用层升级为生产级图编排 Agent 框架」的最大瓶颈。

### 4.2 Crew Process Sequential/Hierarchical/Consensus + Flow @start/@listen/@router 事件驱动 + Memory ShortTerm/LongTerm/Entity/Contextual 四层 + Knowledge Source RAG 全链路空白（等级 B → A）

- **自研现状**：`engine/apps/multimodal_rag/src/agent/agent.cpp` 实现单 Agent 类 + ReAct 循环 + Memory 接口层（`memory.h` 79 行 + `memory.cpp` 88 行）+ Tool 抽象；`engine/apps/multimodal_rag/src/pipeline/` 5 文件序贯 Pipeline；但**无** Crew Process 三件套（Sequential / Hierarchical / Consensus）+ Agent 四件套（Role / Goal / Backstory / Tools）+ Task 三件套（description / expected_output / context / async_execution）+ Flow 5 装饰器（@start / @listen / @router / @and_ / @or_）+ Flow State 自动 pickle 持久化 + Memory 四层（ShortTerm / LongTerm / Entity / Contextual）+ Knowledge Source RAG（PDF / Web / DB / Confluence）+ MCP 集成（MCPServer SSE / Stdio）+ Skills Marketplace（100+ Skills）+ Hooks（@before_step / @after_step）+ Custom HITL（`Task(human_input=True)`）+ AMP Cloud 托管。
- **业界做法**：CrewAI `lib/crewai/src/crewai/agent.py`（`Agent(role, goal, backstory, tools, llm, allow_delegation)`）+ `lib/crewai/src/crewai/task.py`（`Task(description, expected_output, agent, context, async_execution, human_input)`）+ `lib/crewai/src/crewai/crew.py`（`Crew(agents, tasks, process, memory, knowledge_sources, planning)`）+ `lib/crewai/src/crewai/process.py`（`Process.sequential` / `Process.hierarchical` / `Process.consensus`）+ `lib/crewai/src/crewai/flow/`（`@start / @listen / @router / @and_ / @or_` Flow 事件驱动 + State 自动 pickle 持久化）+ `lib/crewai/src/crewai/memory/{short_term, long_term, entity, contextual}`（4 层记忆）+ `lib/crewai/src/crewai/knowledge/`（Knowledge Source PDF / Web / DB / Confluence）+ `lib/crewai/src/crewai/mcp/`（MCPServer SSE / Stdio）+ `lib/crewai/src/crewai/skills/`（Skills Marketplace）+ `lib/crewai/src/crewai/hooks/`（@before_step / @after_step）+ `crewai create / run / test / deploy / chat / flow` CLI + CrewAI Studio + AMP Cloud + CrewAI Enterprise。
- **差距量级**：约 12-18 个文件 / 12000-18000 行 C；包含 Crew Process 三件套（Sequential / Hierarchical / Consensus）+ Agent 四件套（Role / Goal / Backstory / Tools）+ Task 三件套 + Flow 5 装饰器（`@start` / `@listen` / `@router` / `@and_` / `@or_`）+ Flow State 自动序列化 + Memory 4 层（ShortTerm / LongTerm / Entity / Contextual）+ Knowledge Source 6+ 类（PDF / Web / DB / Confluence / Notion / GitHub）+ MCPServer + Skills Marketplace + Hooks + Custom HITL + AMP Cloud 客户端。
- **影响**：自研 Agent **无** Role / Goal / Backstory 角色化抽象（多 Agent 协作场景无法快速分工）；**无** Sequential / Hierarchical / Consensus Process（任务编排只能 Pipeline 串行，不能 Manager 委派 / 投票共识）；**无** Flow 事件驱动（CRM / ERP / 营销自动化等长工作流无法事件化）；**无** Memory 4 层（短期 / 长期 / 实体 / 上下文无法分层）；**无** Knowledge Source RAG（PDF / Web / DB / Confluence 知识库无法接入）；**无** MCP 集成（无法连接外部 MCP 服务器）；**无** Skills Marketplace（搜索 / 摘要 / 翻译 / 校对等预制技能缺失）；与 CrewAI 25k star + 上升势头猛 + 企业级特性完全脱钩；这是「从单 Agent + 5 Pipeline 升级为角色化 + 流程化 + Memory 四层 + Skills Marketplace 多 Agent 协作框架」的最大缺口。

### 4.3 AutoGen Actor Model + Subscription 路由 + GroupChat + Magentic-One + Code Executor + MCP Workbench + ComputerUse 协议标准化全链路空白（等级 B → A）

- **自研现状**：`engine/apps/multimodal_rag/src/agent/agent.cpp` 单 Agent 单进程类；`engine/apps/multimodal_rag/src/pipeline/` 5 Pipeline 文件并行（naive）/ 循环（ReAct/Iterative/Recursive）/ 异步（Self-RAG/Corrective）；`engine/src/db/executor/rag/` 10 文件 RAG 算子；但**无** Actor Model（RoutedAgent / ClosureAgent / SingleThreadedAgentRuntime / DistributedAgentRuntime）+ Subscription 路由（TopicType / TypeSubscription / DefaultSubscription / MessageRouter）+ GroupChat（RoundRobin / Selector / Swarm / MagenticOneGroupChat）+ 终止条件（MaxMessage / TokenUsage / Timeout / Handoff）+ State Snapshot（save_state / load_state）+ Pause / Resume / Replay / Cancellation + Code Executor 沙箱（Local / Docker / E2B）+ MCP Workbench（McpWorkbench / McpToolAdapter）+ ComputerUse（OpenAI CUA / Anthropic Computer Use）+ AutoBuild 自动生成 Agent + AutoGen Studio + Magentic-One CLI。
- **业界做法**：AutoGen `python/packages/autogen-core/src/autogen_core/base.py`（Agent 抽象 + `TopicType` / `TypeSubscription` / `DefaultSubscription`）+ `python/packages/autogen-core/src/autogen_core/_agent_runtime.py`（`SingleThreadedAgentRuntime` / `DistributedAgentRuntime`）+ `python/packages/autogen-agentchat/src/autogen_agentchat/teams/{round_robin_group_chat.py, selector_group_chat.py, swarm.py, magentic_one_group_chat.py}`（GroupChat / Magentic-One）+ `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/{max_message_condition.py, token_usage_condition.py, timeout_condition.py, handoff_termination.py}`（终止条件）+ `python/packages/autogen-agentchat/src/autogen_agentchat/state/`（`save_state / load_state / pause / resume / replay / cancel`）+ `python/packages/autogen-ext/src/autogen_ext/agents/code_executor_agent.py`（CodeExecutor Local / Docker / E2B）+ `python/packages/autogen-ext/src/autogen_ext/mcp/`（McpWorkbench / McpToolAdapter 连接外部 MCP）+ `python/packages/autogen-ext-computer-use`（ComputerUse OpenAI CUA / Anthropic Computer Use）+ `python/packages/autogen-magentic-one/`（Orchestrator + Coder / WebSurfer / FileSurfer / Executor / ComputerTerminal）+ `autogen-studio/`（Web IDE）+ `magentic-one-cli`（CLI）。
- **差距量级**：约 15-22 个文件 / 15000-25000 行 C；包含 Actor Model（RoutedAgent / ClosureAgent / SingleThreadedAgentRuntime / DistributedAgentRuntime）+ Subscription 路由（TopicType / TypeSubscription / DefaultSubscription / MessageRouter / Message Queue）+ GroupChat 4 类（RoundRobin / Selector / Swarm / Magentic）+ Termination 4 类（MaxMessage / TokenUsage / Timeout / Handoff）+ State Snapshot / Pause / Resume / Replay / Cancellation + Code Executor（Local / Docker / E2B）+ MCP Workbench + Computer Use + AutoBuild + AutoGen Studio Web IDE + Magentic-One CLI + RPC（gRPC / WebSocket / Redis / Kafka）。
- **影响**：自研 Agent **无**事件驱动运行时（基于订阅的发布订阅缺失，无法做异步消息分发）；**无**GroupChat 协调（4 类：RoundRobin / Selector / Swarm / Magentic 都缺，多 Agent 自动切换 Speaker 缺失）；**无**Magentic-One 自适应规划（复杂任务无法自适应重规划）；**无**Code Executor 沙箱（LLM 生成代码无法执行，Code Agent 场景缺失）；**无**MCP Workbench（无法连接 MCP 服务器）；**无**ComputerUse（OpenAI CUA / Anthropic Computer Use 缺失，无法自动操作系统）；与 AutoGen 35k star + Microsoft Research + 学术研究底座完全脱钩；这是「从单 Agent + 5 Pipeline 升级为事件驱动 Actor Model + GroupChat + Magentic-One + Code Executor + MCP 学术级多 Agent 协作」的最大缺口。

### 4.4 Streaming 5 模式（values/updates/messages/events/debug）+ LangSmith/LangFuse/Phoenix/RAGAS + LangGraph Studio + OpenTelemetry + astream_events 细粒度事件 全链路空白（等级 D → A）

- **自研现状**：`engine/apps/multimodal_rag/src/observability/logger.cpp` + `metrics.cpp` 仅 2 文件实现基础日志与指标 + `Agent(verbose=true)` 输出控制 + `engine/apps/multimodal_rag/include/mmrag/{logger.h, metrics.h}` 头文件；但**无**LangGraph `stream()` / `astream()` 5 模式（values / updates / messages / events / debug）+ `astream_events` 细粒度事件流 + LangSmith 商业 SaaS（Tracing + Evaluation + Hub + Dataset + Feedback）+ LangFuse 开源替代 + Phoenix（Arize）OpenInference + TruLens + RAGAS 集成（context_relevancy / faithfulness / answer_relevancy）+ Callback 机制（`CallbackHandler / BaseCallbackManager` + on_chain_start/end/error + on_retriever_start/end + on_tool_start/end + on_llm_start/end）+ OpenTelemetry tracing SDK + LangGraph Studio Web IDE（可视化 step-through + time-travel）+ LlamaIndex Instrumentation 事件总线 + haystack Evaluator Component（Faithfulness / AnswerRelevance / DocumentMRR / DocumentMAP / DocumentRecall）。
- **业界做法**：LangGraph `libs/langgraph/langgraph/pregel/_io.py`（5 模式 stream） + `libs/langgraph/langgraph/pregel/_remote_run_stream.py` + LangSmith cloud.langchain.com/smith（SaaS Tracing + Evaluation + Hub + Dataset + Feedback）+ LangFuse Python SDK（langfuse.langfuse.com）+ Phoenix（Arize）OpenInference（phoenix.arize.com）+ TruLens（trulens.org）+ RAGAS（docs.ragas.io context_relevancy / faithfulness / answer_relevancy）+ langchain `libs/core/langchain_core/callbacks/base.py`（CallbackHandler / BaseCallbackManager / CallbackEvents）+ langchain `libs/core/langchain_core/tracers/`（OpenTelemetry tracer）+ LangGraph Studio 桌面 / Web IDE（断点 + step-through + time-travel）+ LlamaIndex `llama-index-instrumentation` v0.12+（事件总线架构）+ haystack `haystack/tracing/`（OpenTelemetry tracing）+ `haystack/components/evaluators/`（Faithfulness + AnswerRelevance + DocumentMRR + DocumentMAP + DocumentRecall）。
- **差距量级**：约 10-16 个文件 / 10000-18000 行 C；包含 Streaming 5 模式（`stream_values / updates / messages / events / debug`）+ OpenTelemetry SDK 集成（otel-cpp）+ Callback 机制（`CallbackHandler / BaseCallbackManager + on_chain/retriever/tool/llm_start/end`）+ LangSmith 兼容客户端 + LangFuse + Phoenix + TruLens + RAGAS 客户端 + 5 套 Evaluator Component（Faithfulness / AnswerRelevance / DocumentMRR / DocumentMAP / DocumentRecall）+ LangGraph Studio 桌面 / Web IDE。
- **影响**：自研 Agent + Pipeline **无**流式输出（生产中只能 pull，不能 stream values / updates / messages / events / debug）；**无**trace 调试（无法看到检索 / 重排 / 生成的每次调用耗时 + token）；**无**LangGraph Studio 可视化（断点 + step-through + time-travel 调试缺失）；**无**评估反馈（无法衡量 Agent system faithfulness / answer_relevancy 指标变化）；**无**RAGAS 指标（生产中 Agent + RAG 系统的关键质量量化缺失）；**无**Callback 流式监控（生产中无法 hook 到 chain / llm / retriever / tool 事件）；与 LangSmith / LangFuse / Phoenix / TruLens / RAGAS 商业 / 开源观测矩阵全不兼容；这是「从 C/C++ 嵌入式 Agent 升级为生产级可观测 Agent + RAG 系统」的最大瓶颈。

### 4.5 多 Agent 通信（State 共享 + Manager 委派 + Handoff + Message Bus + Subscription + Runtime RPC + gRPC/WebSocket） + Skills Marketplace + Hooks 全链路空白（等级 C → A）

- **自研现状**：`engine/apps/multimodal_rag/src/agent/agent.cpp` 单 Agent 类；`engine/apps/multimodal_rag/src/pipeline/` 5 Pipeline 文件通过上下文串联（naive + pipeline_base + types）+ `engine/apps/multimodal_rag/src/pipeline/pipeline_factory.cpp` 工厂；但**无**多 Agent 通信协议（LangGraph State 共享 blackboard + Channel 消息总线 + RemoteGraph 跨进程 + Send API 异步分发；CrewAI Shared State + ShortTerm Memory message bus + Manager LLM 委派 + Handoff；AutoGen Message Bus Subscription 发布订阅 + Shared State + Handoff Swarm + Runtime RPC gRPC/WebSocket）+ Skills Marketplace（CrewAI 100+ Skills 搜索 / 摘要 / 翻译 / 校对 + Agent Marketplace 销售 / 客服 / 营销 / 财务 / HR + Flow Marketplace CRM / ERP / 营销自动化）+ Hooks（CrewAI @before_step / @after_step + 自定义 Agent before_execute / after_execute）+ LangChain Hub（Prompt 仓库）+ LangChain CallbackHandler + AutoGen AutoBuild 自动生成 Agent。
- **业界做法**：LangGraph `libs/langgraph/langgraph/graph/state.py`（所有节点共享 State blackboard）+ `libs/langgraph/langgraph/channels/topic.py`（messages channel 消息总线）+ `libs/langgraph/langgraph/types.py`（`Send` 异步分发）+ RemoteGraph `libs/langgraph/langgraph/pregel/_remote_run_stream.py`（HTTP API 跨网络调用）；CrewAI `crew.py`（共享 State）+ `memory/short_term/`（消息队列）+ `crew = Crew(process=Process.hierarchical, manager_llm=...)`（Manager 委派）+ Handoff `agent.on_message(...)` + Remote Crew `MCP Server` 或 `HTTP Server`；AutoGen `python/packages/autogen-core/src/autogen_core/_agent_runtime.py`（Message Bus Subscription 发布订阅 + Shared State）+ `python/packages/autogen-core/src/autogen_core/_agent_state.py`（Shared State）+ `SwarmGroupChat` Handoff + `DistributedAgentRuntime`（gRPC / WebSocket / Redis / Kafka）+ `RpcAgentClient / RpcAgentServer` 跨进程 + CrewAI Skills Marketplace + Hooks + AutoGen AutoBuild。
- **差距量级**：约 10-16 个文件 / 10000-18000 行 C；包含 LangGraph 黑板共享 + Channel 消息总线 + RemoteGraph 远程 + Send API；CrewAI Manager 委派 + Handoff + Remote Crew + Skills Marketplace + Hooks + 100+ Skills；AutoGen Message Bus + Subscription + Shared State + Handoff Swarm + DistributedAgentRuntime + gRPC / WebSocket / Redis / Kafka + AutoBuild + Magentic-One CLI。
- **影响**：自研多 Agent **无**真正通信协议（只能通过 Pipeline 上下文传递，复杂协调需手动实现）；**无**Manager 委派（CrewAI `Process.hierarchical` 风格 Manager LLM 自动委派缺失）；**无**Handoff（OpenAI Swarm 风格 Agent 间 handoff 缺失）；**无**Message Bus（AutoGen Subscription 发布订阅缺失，无法做事件驱动）；**无**Distributed Runtime gRPC / WebSocket（无法做分布式部署）；**无**Skills Marketplace（100+ Skills 搜索 / 摘要 / 翻译 / 校对 缺失）；**无**Hooks（@before_step / @after_step 钩子缺失，无法做拦截 / 监控 / 调试）；与 LangGraph Studio + CrewAI 25k star + AutoGen 35k star 生态完全脱钩；这是「从单 Agent + 5 Pipeline 升级为多 Agent 协作 + Skills Marketplace + Hooks + 分布式」的最大缺口。

---

## 5. 优势 Top-3（自研独有的设计）

### 5.1 5 类学术 Pipeline 自研完整实现（ReAct/Iterative/Recursive/Self-RAG/Corrective）

`engine/apps/multimodal_rag/src/pipeline/`（**5 文件 2128 行**：react_pipeline.cpp 544 行 + iterative_pipeline.cpp 418 行 + recursive_pipeline.cpp 489 行 + self_rag.cpp + corrective_pipeline.cpp 421 行）一次性集成 **5 种 RAG 学术方法**：ReAct（Yao 2022，Thought → Action → Observation）+ Iterative（迭代检索增强）+ Recursive（树状查询分解 + 子节点聚合）+ Self-RAG（Asai 2023 反思式检索 + `[Retrieval] / [IsRel] / [IsSup] / [IsUse]` token 协议）+ CRAG（Yan 2024 Corrective RAG，检索评估 + Web 检索 fallback + 知识精炼 + 答案过滤）。LangGraph（StateGraph 通用编排 + create_react_agent helper） / CrewAI（Task 自定义 + Process Sequential/Hierarchical） / AutoGen（GroupChat + Magentic-One + AssistantAgent default ReAct）三家都不直接实现 Self-RAG / Iterative / Recursive / CRAG 等论文原型的内置版本；自研 5 类 Pipeline 是 13 子报告里 RAG 学术方法 + Agent 学术方法覆盖最完整的实现，且每条 Pipeline 都是 C/C++ 端到端，可直接对接 DB 内核 rag 算子。

### 5.2 Agent 三件套（agent/memory/tool）+ DB 内核 10 个 RAG 算子端到端（C/C++ 应用层 + DB 内核复用）

`engine/apps/multimodal_rag/src/agent/`（**3 文件**：agent.cpp 243 行 + memory.cpp 88 行 + tool.cpp 366 行 = 697 行）+ `engine/apps/multimodal_rag/include/mmrag/modular/agent/`（**3 头文件**：agent.h 124 行 + memory.h 79 行 + tool.h 189 行 = 392 行）+ `engine/src/db/executor/rag/`（**10 文件** RAG 算子：chunk_op.c / embed_op.c / generate_op.c / retrieve_op.c / rag_executor.c / rag_pipeline.c + graphrag_context.c / graphrag_entity.c / graphrag_relation.c / graphrag_hybrid_search.c）形成「Agent 三件套 + DB 内核 10 RAG 算子」端到端。LangGraph（CrewAI LangChain 集成在 Python 库层面） / CrewAI（Memory / Tool / Knowledge Source / MCP 都在 Python 库） / AutoGen（Tool / MCP / CodeExecutor / ComputerUse 都在 Python 库）三家都依赖各自上层 Python 框架；自研直连 DB 内核 rag 算子（chunk / embed / generate / retrieve / rag_executor / rag_pipeline + 4 graphrag 算子），把「Agent 应用层 + LLM 编排 + DB 内核 RAG 算子」压缩到 13 文件内，是其他三家都没有的差异化亮点。

### 5.3 自研 AgentState 6 状态机 + ReActStep 步骤历史 + GraphRAG 4 算子工程化

`engine/apps/multimodal_rag/src/agent/agent.cpp` 实现 **`AgentState` 6 状态机**（IDLE / THINKING / ACTING / OBSERVING / FINISHED / FAILED）+ **`ReActStep`** 步骤结构（step_id + thought + action + action_input + observation + is_final）+ **`AgentResponse`** 响应结构（success + output + steps + iterations_used + error），把 ReAct 循环建模为「有限状态机 + 步骤序列」的工程化产物；`engine/src/db/executor/rag/graphrag_*.c`（**4 文件**）实现 GraphRAG 端到端 4 算子：graphrag_entity（实体抽取 NER + 实体归一化）+ graphrag_relation（实体间关系识别）+ graphrag_context（图节点 + 邻居 + 子图检索）+ graphrag_hybrid_search（向量 + 图遍历 + BM25 三路融合）。LangGraph（StateGraph + Channel 触发 + Checkpoint 持久化 间接状态机） / CrewAI（Crew 任务状态 pending/running/completed/failed） / AutoGen（TeamState + AgentState + save_state / load_state）三家都把状态机当框架内部细节，不直接提供 FSM + 步骤历史；自研 6 状态机 + 4 GraphRAG 算子是 community 算法（实体 + 关系 + 上下文 + 混合）端到端工程化亮点。

---

## 6. 学习路径

> 推荐精读以下 wiki 段落 + 一段自研源码，每条都给「为什么读它」。

### 6.1 必须读（一周内）

1. **[`docs/agent-wiki/langgraph.md`](langgraph.md) §六（整体架构 StateGraph + Pregel BSP + Channel + Checkpoint + Store）+ §七.1（七.1 角色协作 Pregel BSP + State 主题 + Send API 动态扇出）+ §七.2（七.2 记忆系统 三层）+ §七.4（七.4 人机协作 Interrupt + Resume）**
   - 原因：补齐 Top-5 §4.1（StateGraph + Pregel BSP + Channel + Checkpoint + Interrupt HITL）所需的核心抽象；LangGraph `libs/langgraph/langgraph/graph/state.py`（StateGraph 节点 / 边 / 编译）+ `libs/langgraph/langgraph/pregel/_algo.py`（Pregel BSP 模型）+ `libs/langgraph/langgraph/channels/`（LastValue / Topic / BinaryOperator / NamedBarrier / EphemeralValue 5 类）+ `libs/checkpoint/langgraph/checkpoint/{base/, memory/, serde/}`（Checkpoint 抽象 + 后端）+ `libs/langgraph/langgraph/types.py`（`Command / Interrupt / Send`）是「图编排 + 状态机 + 持久化 + HITL + 流式」最权威参考。

2. **[`docs/agent-wiki/crewai.md`](crewai.md) §六（整体架构 Agent + Task + Crew + Process + Flow + Memory + RAG + MCP）+ §七.1（七.1 角色协作 Crew Process 4 件套）+ §七.2（七.2 记忆系统 4 层）+ §七.4（七.4 人机协作 HumanInput Tool + Flow Approval）**
   - 原因：补齐 Top-5 §4.2（Crew Process Sequential/Hierarchical/Consensus + Flow @start/@listen/@router 事件驱动 + Memory ShortTerm/LongTerm/Entity/Contextual 四层）所需的所有设计图；CrewAI `lib/crewai/src/crewai/agent.py`（Agent 4 件套）+ `lib/crewai/src/crewai/task.py`（Task 3 件套）+ `lib/crewai/src/crewai/crew.py`（Crew 编排）+ `lib/crewai/src/crewai/process.py`（`Process.sequential / Process.hierarchical / Process.consensus`）+ `lib/crewai/src/crewai/flow/`（@start / @listen / @router / @and_ / @or_ Flow 装饰器）+ `lib/crewai/src/crewai/memory/{short_term, long_term, entity, contextual}`（4 层记忆）+ `lib/crewai/src/crewai/mcp/`（MCPServer SSE / Stdio）是「角色化 + 流程化 + Flow + Memory + MCP」最清晰的实现。

3. **[`docs/agent-wiki/autogen.md`](autogen.md) §六（整体架构 Actor Model + GroupChat + Magentic-One + AutoBuild）+ §七.1（七.1 角色协作 GroupChat 4 类 + Magentic-One）+ §七.2（七.2 记忆系统 Team Context + Model Context + Cache）+ §九.1（九.1 Team 执行引擎 Termination Conditions + State Snapshot + Pause/Resume/Replay）**
   - 原因：补齐 Top-5 §4.3（AutoGen Actor Model + Subscription + GroupChat + Magentic-One + Code Executor + MCP Workbench + ComputerUse）所需的所有设计图；AutoGen `python/packages/autogen-core/src/autogen_core/base.py`（Agent / Subscription / TopicType）+ `python/packages/autogen-core/src/autogen_core/_agent_runtime.py`（AgentRuntime）+ `python/packages/autogen-agentchat/src/autogen_agentchat/teams/`（RoundRobin / Selector / Swarm / Magentic）+ `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/`（终止条件 MaxMessage / TokenUsage / Timeout / Handoff）+ `python/packages/autogen-agentchat/src/autogen_agentchat/state/`（State Snapshot / Pause / Resume / Replay / Cancellation）+ `python/packages/autogen-ext/src/autogen_ext/mcp/`（McpWorkbench）+ `python/packages/autogen-ext-computer-use`（ComputerUseAgent）是「事件驱动 Actor Model + GroupChat + Magentic-One + AutoBuild」最完整的实现。

### 6.2 推荐读（一月内）

4. **[`docs/agent-wiki/langgraph.md`](langgraph.md) §八（索引机制 State / Channel / LastValue / Topic / BinaryOperator / NamedBarrier / EphemeralValue 5 类）+ §九.1（九.1 图执行引擎 Pregel BSP + Concurrent Executor + Streaming API + Subgraph）+ §十（Python API + LangGraph SDK + LangGraph Studio）**
   - 原因：补齐 Top-5 §4.1（StateGraph 5 类 Channel 抽象 + Pregel BSP 并行调度 + 5 模式 Streaming + Subgraph 嵌套 + LangGraph Studio）所需的细节；LangGraph `libs/langgraph/langgraph/channels/{last_value.py, topic.py, binop.py, named_barrier.py, ephemeral_value.py}`（5 类 Channel）+ `libs/langgraph/langgraph/pregel/_executor.py`（ConcurrentExecutor / AsyncConcurrentExecutor）+ `libs/langgraph/langgraph/pregel/_io.py`（5 模式 stream values / updates / messages / events / debug）+ `libs/langgraph/langgraph/pregel/_remote_run_stream.py`（RemoteGraph 远程调用）+ LangGraph Studio 桌面 / Web IDE（断点 + step-through + time-travel）+ LangGraph Platform 托管。

5. **[`docs/agent-wiki/crewai.md`](crewai.md) §八（索引机制 Knowledge Source PDF/Web/DB/Confluence + Memory 索引 Chroma/SQLite）+ §九.3（九.3 多模态工具 100+ Tool）+ §十一（生态集成 LLM + VectorStore + Tool + Memory + Knowledge Source + MCP + Skills Marketplace + CrewAI Enterprise）**
   - 原因：补齐 Top-5 §4.2（Knowledge Source RAG + MCP 集成 + Skills Marketplace + Hooks + Agent Marketplace + AMP Cloud）所需的全谱集成参考；CrewAI `lib/crewai/src/crewai/tools/`（100+ 工具）+ `lib/crewai/src/crewai/skills/`（Skills Marketplace 100+ Skills）+ `lib/crewai/src/crewai/mcp/`（MCPServer）+ `lib/crewai/src/crewai/hooks/`（@before_step / @after_step）+ `lib/crewai/src/crewai/knowledge/`（Knowledge Source PDF/Web/DB/Confluence）+ `lib/crewai/src/crewai/memory/long_term/`（Chroma）+ AMP Cloud + CrewAI Enterprise 是「角色化 + MCP + Skills Marketplace + 企业级」端到端范本。

6. **[`docs/agent-wiki/autogen.md`](autogen.md) §九.3（九.3 多模态工具 Browser/CodeExecutor/FileSurfer/API/RAG/DB/Search/Multimodal/ComputerUse 9 类）+ §九.4（九.4 多 Agent 通信 Message Bus + Shared State + Handoff + A2A）+ §十一（生态集成 LLM + VectorStore + Tool + Code Executor + Cache + Memory + Observability + RPC + 部署）**
   - 原因：补齐 Top-5 §4.3（Code Executor 沙箱 + MCP Workbench + ComputerUse + Runtime RPC gRPC/WebSocket + 分布式部署）所需的全谱对接参考；AutoGen `python/packages/autogen-ext/src/autogen_ext/agents/code_executor_agent.py`（CodeExecutor Local / Docker / E2B）+ `python/packages/autogen-ext/src/autogen_ext/mcp/`（McpWorkbench）+ `python/packages/autogen-ext-computer-use`（ComputerUseAgent）+ `python/packages/autogen-core/src/autogen_core/_agent_runtime.py`（DistributedAgentRuntime gRPC / WebSocket / Redis / Kafka）+ `python/packages/autogen-core/src/autogen_core/_agent_state.py`（Shared State）+ Magentic-One + AutoBuild 是「学术级多 Agent + Code Executor + 分布式」端到端范本。

### 6.3 对照实验

- 自研 `engine/apps/multimodal_rag/src/agent/agent.cpp` ↔ LangGraph `libs/langgraph/langgraph/graph/state.py` + AutoGen `autogen-core/src/autogen_core/base.py`：自研 Agent 三件套（agent + memory + tool）+ `AgentState` 6 枚举 + `ReActStep` 步骤；LangGraph StateGraph + Pregel BSP + Channel；AutoGen RoutedAgent + Subscription。三家分别建模「单 Agent 有限状态机」「图编排 + 状态机 + Channel 触发」「事件驱动 Actor Model + Subscription」，路线不同但目标一致（统一 Agent 调用协议）。
- 自研 `engine/apps/multimodal_rag/src/pipeline/react_pipeline.cpp` ↔ LangGraph `create_react_agent(llm, tools)` + CrewAI `Agent default ReAct` + AutoGen `AssistantAgent default ReAct`：自研 544 行 ReAct Pipeline 完整实现 Thought → Action → Observation；LangGraph / CrewAI / AutoGen 都把 ReAct 当 helper 或 default。三家路线不同但目标一致（统一 ReAct 推理循环）。
- 自研 `engine/apps/multimodal_rag/src/pipeline/self_rag.cpp` ↔ LangGraph 自定义 reflect 节点 + CrewAI `Agent(reflection=True)` + AutoGen `reflect_on_tool_use=True`：自研 Self-RAG 学术实现完整；LangGraph / CrewAI / AutoGen 把 Self-RAG / Reflection 当 helper 参数。三家路线不同但目标一致（统一自我反思协议）。
- 自研 `engine/apps/multimodal_rag/src/pipeline/corrective_pipeline.cpp` ↔ LangGraph 自定义 Conditional Edge + CrewAI Fallback Task + AutoGen Magentic-One Replan：自研 CRAG 学术实现完整；LangGraph Conditional Edge + CrewAI Fallback Task + AutoGen Magentic-One Replan 都做 CRAG 风格的「检索评估 + fallback + 精炼」。三家路线不同但目标一致（统一 Corrective 自适应协议）。
- 自研 `engine/apps/multimodal_rag/src/agent/tool.cpp` ↔ LangGraph `ToolNode` + CrewAI `Tool / BaseTool / @tool` + AutoGen `FunctionTool / McpToolAdapter`：自研 Tool 抽象 366 行；LangGraph ToolNode + CrewAI Tool + AutoGen FunctionTool + McpWorkbench 都做 Tool Calling 协议标准化。三家路线不同但目标一致（统一 Tool 注册 / 调用 / 错误处理协议）。
- 自研 `engine/src/db/executor/rag/graphrag_*.c` ↔ LangGraph 自定义 Graph Node + CrewAI Knowledge Source + AutoGen RagTool / RagAgent：自研 GraphRAG 4 件套算子（entity + relation + context + hybrid_search）；LangGraph / CrewAI / AutoGen 三家都把 GraphRAG 当集成后端，不直接实现「实体 + 关系 + 上下文 + 混合检索 4 算子」。自研是 C 端实现 + 4 类 GraphRAG 算子，三家是 Python 框架 + 适配后端。

---

## 7. 补齐项下月计划

> 每条按「动作 / 投入 / 学习资源（wiki 链接）/ 验收标准 / 优先级」给出。
> **自研缺失维度必须 P0 写「StateGraph 图编排 + Channel 抽象 + Checkpoint 持久化 + Interrupt HITL + Crew Process + Flow 事件驱动 + Memory 4 层 + Actor Model + Subscription + GroupChat + Code Executor + MCP + Skills Marketplace + 多 Agent 通信」明确动作**；本节给出 5 条（P0 三条 + P1 两条）。

### 7.1 P0 — 启动 StateGraph 图编排 + Pregel BSP 模型 + 5 类 Channel + Checkpoint 持久化 + Interrupt HITL + Streaming 5 模式（约 6 周）

- **动作**：
  1. **StateGraph 节点 / 边 / 编译**：新建 `engine/apps/multimodal_rag/src/orchestration/state_graph.{h,c}`（`add_node / add_edge / add_conditional_edges / add_edge(START, ...) / add_edge(..., END) / compile(checkpointer=..., interrupt_before=...)`），参照 LangGraph `libs/langgraph/langgraph/graph/state.py`。
  2. **Pregel BSP 主循环**：新建 `engine/apps/multimodal_rag/src/orchestration/pregel.{h,c}`（`super_step` 内并行执行活跃节点 → barrier → 下一 super-step），参照 LangGraph `libs/langgraph/langgraph/pregel/_algo.py` + `_loop.py` + `_executor.py`。
  3. **5 类 Channel 抽象**：新建 `engine/apps/multimodal_rag/src/orchestration/channels/{channel_last_value, channel_topic, channel_binop, channel_named_barrier, channel_ephemeral_value}.{h,c}`（LastValue / Topic / BinaryOperatorAggregate / NamedBarrier / EphemeralValue），参照 LangGraph `libs/langgraph/langgraph/channels/`。
  4. **Checkpoint 持久化**：新建 `engine/apps/multimodal_rag/src/orchestration/checkpoint/{base, memory_saver, sqlite_saver, postgres_saver}.{h,c}`（Checkpoint + Metadata + Serializer + 后端），参照 LangGraph `libs/checkpoint/langgraph/checkpoint/`。
  5. **Store 跨线程记忆**：新建 `engine/apps/multimodal_rag/src/orchestration/store/{in_memory_store, postgres_store}.{h,c}`（命名空间 + 向量记忆 + Pydantic 风格 Schema），参照 LangGraph `libs/checkpoint/langgraph/store/`。
  6. **Interrupt / Command / Send API**：新建 `engine/apps/multimodal_rag/src/orchestration/types/{interrupt, command, send}.{h,c}`（暂停 / 恢复 / handoff / Map-Reduce 动态扇出）。
  7. **Streaming 5 模式**：新建 `engine/apps/multimodal_rag/src/orchestration/streaming/{values, updates, messages, events, debug}.{h,c}`。
- **投入**：约 12000-20000 行 C（StateGraph + Pregel + 5 Channel + Checkpoint + Store + Interrupt / Command / Send + 5 模式 Streaming），1.5 人 5 周开发 + 1 周测试。
- **学习资源**：[langgraph.md §六](../agent-wiki/langgraph.md)（整体架构 StateGraph + Pregel BSP + Channel + Checkpoint + Store）+ [langgraph.md §七.1](../agent-wiki/langgraph.md)（角色协作 Pregel BSP + State 主题 + Send API）+ [langgraph.md §七.4](../agent-wiki/langgraph.md)（HITL Interrupt / Resume）+ [langgraph.md §八](../agent-wiki/langgraph.md)（Channel 5 类）+ [langgraph.md §九.1](../agent-wiki/langgraph.md)（Pregel BSP + Concurrent Executor + Streaming API + Subgraph）+ [langgraph.md §十](../agent-wiki/langgraph.md)（Python API + LangGraph Studio）。
- **验收标准**：
  - `engine/test/apps/multimodal_rag/orchestration/state_graph_test.cpp` ≥ 8 用例（add_node / add_edge / add_conditional_edges / compile / interrupt_before）通过；
  - `engine/test/apps/multimodal_rag/orchestration/pregel_test.cpp` ≥ 6 用例（super_step / parallel / barrier / sub_graph）通过；
  - `engine/test/apps/multimodal_rag/orchestration/channels_test.cpp` ≥ 10 用例（LastValue / Topic / BinaryOperator / NamedBarrier / EphemeralValue）通过；
  - `engine/test/apps/multimodal_rag/orchestration/checkpoint_test.cpp` ≥ 6 用例（MemorySaver / SqliteSaver / PostgresSaver）通过；
  - `engine/test/apps/multimodal_rag/orchestration/interrupt_test.cpp` ≥ 4 用例（interrupt_before / interrupt_after / Command(resume)）通过；
  - `engine/test/apps/multimodal_rag/orchestration/streaming_test.cpp` ≥ 8 用例（values / updates / messages / events / debug 5 模式）通过；
  - 自研 Agent + Pipeline 首次具备 `compile() / stream(values) / interrupt() / Command(resume) / Checkpoint 持久化` 完整协议。
- **优先级**：**P0**（决定 §4.1 与 LangGraph StateGraph + Pregel BSP + Channel + Checkpoint + Interrupt HITL + Streaming 5 模式 全方位对标 + Agent + Pipeline 从「单点 + 序贯循环」升级为「图编排 + 状态机 + 持久化 + HITL + 流式」标准协议）。

### 7.2 P0 — 启动 Crew Process Sequential/Hierarchical/Consensus + Flow @start/@listen/@router + Memory ShortTerm/LongTerm/Entity/Contextual 四层 + Knowledge Source RAG + MCP 集成（约 5 周）

- **动作**：
  1. **Agent 4 件套扩展**：在 `engine/apps/multimodal_rag/src/agent/agent.cpp` 扩展 `Agent(role, goal, backstory, tools, llm, allow_delegation)`，参照 CrewAI `lib/crewai/src/crewai/agent.py`。
  2. **Task 3 件套扩展**：新建 `engine/apps/multimodal_rag/src/agent/task.{h,c}`（`Task(description, expected_output, agent, context, tools, async_execution, human_input)`），参照 CrewAI `lib/crewai/src/crewai/task.py`。
  3. **Crew Process 3 件套**：新建 `engine/apps/multimodal_rag/src/orchestration/process.{h,c}`（`Process.sequential` + `Process.hierarchical` + `Process.consensus`），参照 CrewAI `lib/crewai/src/crewai/process.py`。
  4. **Flow 5 装饰器**：新建 `engine/apps/multimodal_rag/src/orchestration/flow/{decorator, state, events}.{h,c}`（`@start / @listen / @router / @and_ / @or_`），参照 CrewAI `lib/crewai/src/crewai/flow/`。
  5. **Memory 4 层**：在 `engine/apps/multimodal_rag/src/agent/memory.cpp` 扩展 ShortTerm（最近 N 默认 5）+ LongTerm（Chroma / SQLite）+ Entity（NER 去重）+ Contextual（RAG over memory），参照 CrewAI `lib/crewai/src/crewai/memory/`。
  6. **Knowledge Source RAG**：新建 `engine/apps/multimodal_rag/src/knowledge/{pdf_source, web_source, db_source, confluence_source}.{h,c}`（PDF / Web / DB / Confluence 4+ 源），参照 CrewAI `lib/crewai/src/crewai/knowledge/`。
  7. **MCP 集成**：新建 `engine/apps/multimodal_rag/src/integrations/mcp_client.{h,c}`（SSE / Stdio 连接外部 MCP 服务器）。
- **投入**：约 12000-18000 行 C（Agent 4 件套 + Task 3 件套 + Process 3 类 + Flow 5 装饰器 + Memory 4 层 + Knowledge Source 6+ + MCP 客户端），1.5 人 4 周开发 + 1 周测试。
- **学习资源**：[crewai.md §六](../agent-wiki/crewai.md)（整体架构 Agent + Task + Crew + Process + Flow + Memory + RAG + MCP）+ [crewai.md §七.1](../agent-wiki/crewai.md)（角色协作 4 件套）+ [crewai.md §七.2](../agent-wiki/crewai.md)（记忆系统 4 层）+ [crewai.md §七.4](../agent-wiki/crewai.md)（HITL HumanInput Tool + Flow Approval）+ [crewai.md §九.1](../agent-wiki/crewai.md)（Crew 执行引擎 Process Sequential/Hierarchical）+ [crewai.md §十一](../agent-wiki/crewai.md)（生态集成 Knowledge Source + MCP + Skills + Hooks）。
- **验收标准**：
  - `engine/test/apps/multimodal_rag/agent/role_agent_test.cpp` ≥ 6 用例（role / goal / backstory / tools / allow_delegation）通过；
  - `engine/test/apps/multimodal_rag/agent/task_test.cpp` ≥ 6 用例（description / expected_output / agent / context / async_execution / human_input）通过；
  - `engine/test/apps/multimodal_rag/orchestration/process_test.cpp` ≥ 8 用例（Sequential / Hierarchical / Consensus）通过；
  - `engine/test/apps/multimodal_rag/orchestration/flow_test.cpp` ≥ 8 用例（@start / @listen / @router / @and_ / @or_ 5 装饰器）通过；
  - `engine/test/apps/multimodal_rag/agent/memory_4layer_test.cpp` ≥ 6 用例（ShortTerm / LongTerm / Entity / Contextual）通过；
  - `engine/test/apps/multimodal_rag/knowledge/pdf_test.cpp` + `web_test.cpp` + `db_test.cpp` 各 4 用例通过；
  - `engine/test/apps/multimodal_rag/integrations/mcp_test.cpp` ≥ 4 用例（SSE / Stdio 连接 + filesystem / GitHub）通过。
- **优先级**：**P0**（决定 §4.2 与 CrewAI 25k star + Crew Process 3 件套 + Flow 5 装饰器 + Memory 4 层 + Knowledge Source + MCP 全方位对标 + Agent 从「单点有限状态机」升级为「角色化 + 流程化 + 流程化 + Memory 4 层 + MCP」标准协议）。

### 7.3 P0 — 启动 AutoGen Actor Model + Subscription 路由 + GroupChat RoundRobin/Selector/Swarm/Magentic + Code Executor 沙箱 + MCP Workbench + State Snapshot + Pause/Resume/Replay（约 5 周）

- **动作**：
  1. **Actor Model**：新建 `engine/apps/multimodal_rag/src/runtime/routed_agent.{h,c}`（事件驱动 Agent）+ `closure_agent.{h,c}`（闭包 Agent）+ `single_threaded_runtime.{h,c}`（单进程）+ `distributed_runtime.{h,c}`（分布式 gRPC / WebSocket / Redis）。
  2. **Subscription 路由**：新建 `engine/apps/multimodal_rag/src/runtime/{topic_type, type_subscription, default_subscription, message_router, message_queue}.{h,c}`（AutoGen Subscription 事件驱动核心）。
  3. **GroupChat 4 类**：新建 `engine/apps/multimodal_rag/src/team/{round_robin_group_chat, selector_group_chat, swarm_group_chat, magentic_one_group_chat}.{h,c}`，参照 AutoGen `python/packages/autogen-agentchat/src/autogen_agentchat/teams/`。
  4. **终止条件 4 类**：新建 `engine/apps/multimodal_rag/src/team/conditions/{max_message_termination, token_usage_termination, timeout_termination, handoff_termination}.{h,c}`。
  5. **State Snapshot + Pause / Resume / Replay / Cancellation**：新建 `engine/apps/multimodal_rag/src/team/state/{team_state, agent_state, save_state, load_state, pause, resume, replay, cancel}.{h,c}`。
  6. **Code Executor 沙箱**：新建 `engine/apps/multimodal_rag/src/tools/code_executor/{local, docker, e2b}_executor.{h,c}`（LLM 生成代码 + 沙箱执行）。
  7. **MCP Workbench**：新建 `engine/apps/multimodal_rag/src/tools/mcp_workbench.{h,c}`（连接外部 MCP 服务器 + Tool Adapter）。
- **投入**：约 15000-25000 行 C（Actor Model + Subscription + GroupChat + Termination + State + Code Executor + MCP Workbench），1.5 人 4 周开发 + 1 周测试。
- **学习资源**：[autogen.md §六](../agent-wiki/autogen.md)（整体架构 Actor Model + GroupChat + Magentic-One + AutoBuild）+ [autogen.md §七.1](../agent-wiki/autogen.md)（角色协作 GroupChat 4 类 + Magentic-One）+ [autogen.md §七.2](../agent-wiki/autogen.md)（记忆系统 Team Context + Model Context + Cache）+ [autogen.md §七.3](../agent-wiki/autogen.md)（工具调用 FunctionTool + CodeExecutor + MCP）+ [autogen.md §九.1](../agent-wiki/autogen.md)（Team 执行引擎 Termination + State Snapshot + Pause/Resume/Replay）+ [autogen.md §九.3](../agent-wiki/autogen.md)（多模态工具 Browser/CodeExecutor/FileSurfer/API/RAG/DB/Search/Multimodal/ComputerUse 9 类）+ [autogen.md §九.4](../agent-wiki/autogen.md)（多 Agent 通信 Message Bus + Subscription + Runtime RPC）。
- **验收标准**：
  - `engine/test/apps/multimodal_rag/runtime/routed_agent_test.cpp` ≥ 6 用例（事件驱动 + on_message 钩子）通过；
  - `engine/test/apps/multimodal_rag/runtime/subscription_test.cpp` ≥ 6 用例（TopicType / TypeSubscription / DefaultSubscription / MessageRouter）通过；
  - `engine/test/apps/multimodal_rag/team/round_robin_test.cpp` + `selector_test.cpp` + `swarm_test.cpp` + `magentic_one_test.cpp` 各 4 用例通过；
  - `engine/test/apps/multimodal_rag/team/termination_test.cpp` ≥ 6 用例（MaxMessage / TokenUsage / Timeout / Handoff）通过；
  - `engine/test/apps/multimodal_rag/team/state_test.cpp` ≥ 6 用例（save_state / load_state / pause / resume / replay / cancel）通过；
  - `engine/test/apps/multimodal_rag/tools/code_executor_test.cpp` ≥ 4 用例（Local / Docker / E2B）通过；
  - `engine/test/apps/multimodal_rag/tools/mcp_workbench_test.cpp` ≥ 4 用例（MCP Client + Tool Adapter）通过。
- **优先级**：**P0**（决定 §4.3 与 AutoGen 35k star + Actor Model + Subscription + GroupChat + Magentic-One + Code Executor + MCP Workbench 全方位对标 + Agent 从「单点有限状态机」升级为「事件驱动 Actor Model + GroupChat + Magentic-One 自适应 + Code Executor」标准协议）。

### 7.4 P1 — 启动 OpenTelemetry SDK + LangSmith/LangFuse/Phoenix/RAGAS 商业观测矩阵 + LangGraph Studio + Callback 机制 + 5 套 Evaluator Component（约 4 周）

- **动作**：
  1. **OpenTelemetry SDK 集成**：在 `engine/third_part/` 引入 `otel-cpp`（OpenTelemetry C++ SDK），通过 `find_package(OpenTelemetry)` 集成到 `engine/apps/multimodal_rag/CMakeLists.txt`；新建 `engine/apps/multimodal_rag/src/observability/otel.{h,c}`（tracer + span + OTLP 导出）。
  2. **Callback 机制**：新建 `engine/apps/multimodal_rag/src/observability/callback.{h,c}`（langchain `BaseCallbackManager` 等价 + `on_chain_start/end/error` + `on_retriever_start/end` + `on_tool_start/end` + `on_llm_start/end`）。
  3. **LangSmith 兼容客户端**：新建 `engine/apps/multimodal_rag/src/observability/langsmith_client.{h,c}`（Trace + Evaluation + Hub + Dataset + Feedback HTTP 客户端）。
  4. **LangFuse + Phoenix + TruLens**：新建 `engine/apps/multimodal_rag/src/observability/{langfuse_client, phoenix_client, trulens_client}.{h,c}`（开源自部署）。
  5. **RAGAS 集成**：新建 `engine/apps/multimodal_rag/src/observability/ragas_client.{h,c}`（context_relevancy / faithfulness / answer_relevancy 3 指标）。
  6. **5 套 Evaluator Component**：新建 `engine/apps/multimodal_rag/src/eval/{faithfulness, answer_relevance, document_mrr, document_map, document_recall}_evaluator.{h,c}`。
  7. **LangGraph Studio 风格 IDE**：新建 `engine/apps/multimodal_rag/src/observability/agent_ide.{h,c}`（C++ 桌面 / Web 风格可视化 IDE，断点 + step-through + time-travel）。
- **投入**：约 10000-18000 行 C（OpenTelemetry SDK + Callback 机制 + LangSmith / LangFuse / Phoenix / TruLens / RAGAS + 5 套 Evaluator + Studio IDE），1 人 3 周开发 + 1 周测试。
- **学习资源**：[langgraph.md §十一](../agent-wiki/langgraph.md)（LangSmith SaaS + LangGraph Studio）+ [crewai.md §七.4](../agent-wiki/crewai.md)（Custom HITL + AMP UI）+ [autogen.md §七.4](../agent-wiki/autogen.md)（Studio HITL + Magentic-One Human Approval）+ OpenTelemetry 官方 `opentelemetry.io/docs/languages/cpp/`。
- **验收标准**：
  - `engine/test/apps/multimodal_rag/observability/otel_test.cpp` ≥ 6 用例（tracer / span / OTLP export）通过；
  - `engine/test/apps/multimodal_rag/observability/callback_test.cpp` ≥ 8 用例（on_chain/retriever/tool/llm 事件）通过；
  - `engine/test/apps/multimodal_rag/observability/langsmith_test.cpp` ≥ 4 用例（trace + eval + hub 端到端）通过；
  - `engine/test/apps/multimodal_rag/eval/faithfulness_test.cpp` ≥ 4 用例（faithfulness score ≥ 0.85）通过；
  - `engine/test/apps/multimodal_rag/eval/answer_relevance_test.cpp` ≥ 4 用例（answer_relevance score ≥ 0.85）通过；
  - 自研 Agent + Pipeline + RAG 首次具备 trace + eval + hub + benchmark + Studio IDE 完整观测 + 可视化能力。
- **优先级**：**P1**（决定 §4.4 与 LangSmith / LangFuse / Phoenix / TruLens / RAGAS + LangGraph Studio 全方位对标 + Agent 从「基础 logger + metrics」升级为「生产级可观测 Agent + Streaming 5 模式 + Studio IDE」）。

### 7.5 P1 — 启动 多 Agent 通信（State 共享 + Manager 委派 + Handoff + Message Bus + Runtime RPC）+ Skills Marketplace + Hooks + Agent Marketplace + AutoBuild（约 5 周）

- **动作**：
  1. **多 Agent 通信协议**：新建 `engine/apps/multimodal_rag/src/team/communication/{state_shared, channel_topic, send_api, remote_graph}.{h,c}`（State 共享 blackboard + Channel 消息总线 + Send API 异步分发 + RemoteGraph 远程调用），参照 LangGraph 多 Agent 通信。
  2. **Manager 委派**：在 `engine/apps/multimodal_rag/src/orchestration/process.{h,c}` 实现 `Process.hierarchical` + `manager_llm`（LLM 自动委派 Worker），参照 CrewAI `crew = Crew(process=Process.hierarchical, manager_llm=...)`。
  3. **Handoff**：新建 `engine/apps/multimodal_rag/src/team/handoff.{h,c}`（Agent 间 handoff 协议，`agent.on_message(...)` 中调用 `handoff(other_agent)`），参照 AutoGen `SwarmGroupChat`。
  4. **Runtime RPC（gRPC + WebSocket + Redis）**：在 `engine/apps/multimodal_rag/src/runtime/distributed_runtime.{h,c}` 实现 gRPC 客户端 / 服务端 + WebSocket 客户端 / 服务端 + Redis Pub/Sub。
  5. **Skills Marketplace**：新建 `engine/apps/multimodal_rag/src/skills/{search, summarize, translate, proof_read}_skill.{h,c}`（100+ Skills 搜索 / 摘要 / 翻译 / 校对）+ `engine/apps/multimodal_rag/src/skills/registry.{h,c}`（Skills 注册表）。
  6. **Hooks**：新建 `engine/apps/multimodal_rag/src/agent/hooks/{before_step, after_step, before_execute, after_execute}.{h,c}`。
  7. **Agent Marketplace + AutoBuild**：新建 `engine/apps/multimodal_rag/src/marketplace/{agent_template, marketplace_registry}.{h,c}`（销售 / 客服 / 营销 / 财务 / HR 模板）+ `engine/apps/multimodal_rag/src/marketplace/auto_build.{h,c}`（自动生成 Agent 配置，参照 AutoGen AutoBuild）。
- **投入**：约 12000-20000 行 C（多 Agent 通信 + Manager 委派 + Handoff + Runtime RPC + Skills + Hooks + Marketplace + AutoBuild），1.5 人 4 周开发 + 1 周测试。
- **学习资源**：[langgraph.md §六](../agent-wiki/langgraph.md)（共享 State + Channel + RemoteGraph）+ [langgraph.md §七.1](../agent-wiki/langgraph.md)（Send API 动态扇出）+ [crewai.md §七.1](../agent-wiki/crewai.md)（Manager 委派）+ [crewai.md §七.3](../agent-wiki/crewai.md)（Skills + Hooks + Agent Marketplace）+ [crewai.md §十一](../agent-wiki/crewai.md)（Skill Marketplace 100+）+ [autogen.md §六](../agent-wiki/autogen.md)（Actor Model + Subscription）+ [autogen.md §九.4](../agent-wiki/autogen.md)（Message Bus + Shared State + Handoff + A2A + RPC）+ [autogen.md §十一](../agent-wiki/autogen.md)（Distributed Runtime gRPC / WebSocket / Redis）+ AutoBuild 官方。
- **验收标准**：
  - `engine/test/apps/multimodal_rag/team/communication_test.cpp` ≥ 8 用例（State 共享 + Channel + Send API + RemoteGraph）通过；
  - `engine/test/apps/multimodal_rag/team/manager_delegation_test.cpp` ≥ 4 用例（Manager LLM 自动委派）通过；
  - `engine/test/apps/multimodal_rag/team/handoff_test.cpp` ≥ 4 用例（Agent 间 handoff）通过；
  - `engine/test/apps/multimodal_rag/runtime/distributed_runtime_test.cpp` ≥ 6 用例（gRPC / WebSocket / Redis Pub/Sub）通过；
  - `engine/test/apps/multimodal_rag/skills/search_test.cpp` + `summarize_test.cpp` + `translate_test.cpp` + `proof_read_test.cpp` 各 4 用例通过；
  - `engine/test/apps/multimodal_rag/agent/hooks_test.cpp` ≥ 6 用例（@before_step / @after_step / before_execute / after_execute）通过；
  - `engine/test/apps/multimodal_rag/marketplace/auto_build_test.cpp` ≥ 4 用例（自动生成 Agent 配置）通过。
- **优先级**：**P1**（决定 §4.5 与 LangGraph State 共享 + RemoteGraph + CrewAI Manager 委派 + Skills Marketplace + AutoGen Message Bus + Handoff + RPC 全方位对标 + Agent 从「单点有限状态机」升级为「多 Agent 协作 + Skills Marketplace + Hooks + 分布式」）。

---

## 8. 附录 A — 自研与 LangGraph / CrewAI / AutoGen 路径对照表（精选 30 行）

| 自研路径 | LangGraph 对照路径 | CrewAI 对照路径 | AutoGen 对照路径 | 简述 |
|----------|---------------------|---------------------|-------------------|------|
| `engine/apps/multimodal_rag/src/agent/agent.cpp` | `libs/langgraph/langgraph/graph/state.py` | `lib/crewai/src/crewai/agent.py` | `autogen-core/src/autogen_core/base.py` | Agent 主入口 |
| `engine/apps/multimodal_rag/src/agent/agent.cpp`（AgentState 6 枚举 + ReActStep） | `libs/langgraph/langgraph/channels/{last_value.py,topic.py}` | `lib/crewai/src/crewai/crew.py`（Crew State） | `autogen-agentchat/src/autogen_agentchat/state/`（TeamState / AgentState） | Agent 状态机 |
| `engine/apps/multimodal_rag/src/agent/memory.cpp` | `libs/checkpoint/langgraph/checkpoint/{base,memory}` + `libs/checkpoint/langgraph/store/{base,memory}` | `lib/crewai/src/crewai/memory/{short_term,long_term,entity,contextual}` | `autogen-core/src/autogen_core/cache_store.py`（Disk / Redis Cache） | 记忆系统 |
| `engine/apps/multimodal_rag/src/agent/tool.cpp` | `libs/langgraph/langgraph/prebuilt/tool_node.py`（ToolNode） | `lib/crewai/src/crewai/tools/`（100+ Tool + Toolkits + MCP） | `autogen-core/src/autogen_core/tools/_function_tool.py`（FunctionTool）+ `autogen-ext/src/autogen_ext/mcp/`（McpWorkbench） | 工具调用 |
| `engine/apps/multimodal_rag/src/pipeline/pipeline.cpp` | `libs/langgraph/langgraph/graph/state.py`（`add_node / add_edge / compile`） | `lib/crewai/src/crewai/crew.py`（Crew 编排） | `autogen-agentchat/src/autogen_agentchat/teams/`（Team.run_stream） | Pipeline / 编排入口 |
| `engine/apps/multimodal_rag/src/pipeline/pipeline_factory.cpp` | `libs/langgraph/langgraph/pregel/main.py`（Pregel 实例化） | `lib/crewai/src/crewai/crew.py`（`Crew.kickoff()` / `kickoff_async()` / `kickoff_for_each()`） | `autogen-agentchat/src/autogen_agentchat/teams/round_robin_group_chat.py`（`run_stream`） | Pipeline 工厂 / 启动 |
| `engine/apps/multimodal_rag/src/pipeline/react_pipeline.cpp` | `libs/langgraph/langgraph/prebuilt/chat_agent_executor.py`（`create_react_agent`） | `lib/crewai/src/crewai/agent.py`（Agent default ReAct） | `autogen-agentchat/src/autogen_agentchat/agents/assistant_agent.py`（AssistantAgent default ReAct） | ReAct 循环 |
| `engine/apps/multimodal_rag/src/pipeline/iterative_pipeline.cpp` | 自定义 StateGraph 循环 + Subgraph | `lib/crewai/src/crewai/task.py`（`async_execution=True` 并行 Task + `Kickoff for_each`） | `autogen-magentic-one/`（Magentic-One Ledger） | Iterative / 并行 |
| `engine/apps/multimodal_rag/src/pipeline/recursive_pipeline.cpp` | Subgraph 嵌套 + StateGraph 循环边 | N/A（Flow + asyncio） | `autogen-magentic-one/`（Magentic-One 自适应重规划） | Recursive / 嵌套 |
| `engine/apps/multimodal_rag/src/pipeline/self_rag.cpp` | 自定义 reflect 节点 | `lib/crewai/src/crewai/agent.py`（`reflection=True`） | `autogen-agentchat/src/autogen_agentchat/agents/assistant_agent.py`（`reflect_on_tool_use=True`） | Self-RAG / Reflection |
| `engine/apps/multimodal_rag/src/pipeline/corrective_pipeline.cpp` | `libs/langgraph/langgraph/graph/_branch.py`（Conditional Edge + RetryPolicy） | 自定义 Task + Process + Hook 后置 | `autogen-agentchat/src/autogen_agentchat/teams/selector_group_chat.py`（LLM-based Speaker Selection）+ Magentic-One Replan | Corrective / CRAG |
| `engine/src/db/executor/rag/rag_executor.c` | `libs/langgraph/langgraph/pregel/_executor.py`（Concurrent Executor 异步 / 同步） | `lib/crewai/src/crewai/process.py`（`Process.sequential / Process.hierarchical / Process.consensus`） | `autogen-agentchat/src/autogen_agentchat/teams/`（GroupChat 协调 + Termination） | 执行器 / 调度 |
| `engine/src/db/executor/rag/rag_pipeline.c` | `libs/langgraph/langgraph/pregel/_algo.py / _loop.py`（Pregel BSP super-step） | `lib/crewai/src/crewai/crew.py`（`kickoff_async()` 流式） | `autogen-agentchat/src/autogen_agentchat/teams/{round_robin,selector,swarm,magentic}_group_chat.py` | Pipeline 调度 / 流式 |
| `engine/apps/multimodal_rag/src/agent/agent.cpp`（HITL 缺） | `libs/langgraph/langgraph/types.py`（`Interrupt` 类）+ `langgraph/pregel/_algo.py`（interrupt 处理 + `Command(resume)` 恢复） | `lib/crewai/src/crewai/tools/human_input_tool/`（HumanInput Tool）+ `lib/crewai/src/crewai/task.py`（`Task(human_input=True)`） | `autogen-agentchat/src/autogen_agentchat/agents/user_proxy_agent.py`（`UserProxyAgent(human_input_mode="ALWAYS")`） | HITL Interrupt / Command / Human Input |
| `engine/apps/multimodal_rag/src/observability/logger.cpp` | `libs/core/langchain_core/callbacks/base.py`（CallbackHandler + BaseCallbackManager + CallbackEvents） | `lib/crewai/src/crewai/telemetry/`（OpenTelemetry + Langfuse） | `autogen-core/src/autogen_core/telemetry/`（OpenTelemetry）+ LangSmith SaaS | 观测 / Callback |
| `engine/apps/multimodal_rag/src/observability/metrics.cpp` | LangSmith SaaS + LangGraph Studio 可视化 + LangFuse + Phoenix + TruLens + RAGAS | `lib/crewai/src/crewai/monitoring/` + `crewai/observability/` + AMP Cloud | `autogen-studio/`（Web IDE）+ Magentic-One CLI + Prometheus + OTel | 观测 / Studio / 商业 |
| `engine/apps/multimodal_rag/src/pipeline/advanced_pipeline.cpp` | `libs/langgraph/langgraph/graph/_branch.py`（`add_conditional_edges`）+ `add_branch` | `lib/crewai/src/crewai/process.py`（自定义 Process 投票） | `autogen-magentic-one/`（Magentic-One 自适应规划） | 高级编排 / 自适应 |
| `engine/apps/multimodal_rag/src/pipeline/hybrid_pipeline.cpp` | `libs/langgraph/langgraph/graph/state.py`（多 Channel 并行） | `lib/crewai/src/crewai/task.py`（`async_execution=True`） | `autogen-agentchat/src/autogen_agentchat/teams/round_robin_group_chat.py`（并行） | Hybrid 并行 |
| `engine/src/db/executor/rag/graphrag_entity.c` | 自定义 Graph Node + Knowledge Graph Index | `lib/crewai/src/crewai/knowledge/`（Knowledge Source + RAG Tool） | `autogen-ext/tools/rag/*`（RagTool / RagAgent + Chroma / FAISS） | GraphRAG 实体 |
| `engine/src/db/executor/rag/graphrag_relation.c` | 自定义 Graph Node | `lib/crewai/src/crewai/knowledge/` + NER | `autogen-ext/tools/rag/*` + NER | GraphRAG 关系 |
| `engine/src/db/executor/rag/graphrag_context.c` | 自定义 Graph Node + Store 向量记忆 | `lib/crewai/src/crewai/memory/contextual/` | `autogen-agentchat/src/autogen_agentchat/state/` + Cache | GraphRAG 上下文 |
| `engine/src/db/executor/rag/graphrag_hybrid_search.c` | HybridSearch RRF（多路融合） | `lib/crewai/src/crewai/memory/long_term/` + RRF | Magentic-One + Cache 混合 | GraphRAG 混合检索 |
| `engine/apps/multimodal_rag/src/pipeline/{naive,iterative,recursive,react,self_rag,corrective}_pipeline.cpp` | `libs/langgraph/langgraph/prebuilt/{chat_agent_executor,tool_node}.py` | `lib/crewai/src/crewai/task.py` + `lib/crewai/src/crewai/agent.py` 组合 | `autogen-agentchat/src/autogen_agentchat/agents/assistant_agent.py` + `tool_call_max_consecutive_failure` | 5 类学术 Pipeline |
| `engine/apps/multimodal_rag/src/agent/agent.cpp`（`Agent` 构造函数 + ReAct 推理代理） | `libs/langgraph/langgraph/func/__init__.py`（@entrypoint / @task 函数式 API） | `lib/crewai/src/crewai/lite_agent.py`（LiteAgent 轻量） | `autogen-core/src/autogen_core/closure_agent.py`（ClosureAgent 闭包） | 函数式 / 轻量 Agent |
| `engine/apps/multimodal_rag/src/server/api_server.cpp` | LangGraph Studio + LangGraph Platform（托管） | AMP Cloud + CrewAI Studio（Web IDE） | AutoGen Studio + Magentic-One CLI | Studio / 服务端 |
| `engine/apps/multimodal_rag/src/eval/evaluator.cpp` | LangSmith SaaS Evaluation + Dataset | `lib/crewai/src/crewai/evaluation/` | `autogen-ext/evaluation/` + DeepEval | 评估 / Eval |
| `engine/apps/multimodal_rag/src/eval/benchmark.cpp` + `golden_dataset.cpp` | LangSmith Dataset + RAGAS | AMP 公开 Bench + RAGAS | AutoBench + Magentic-One Bench | 公开 Benchmark |
| `engine/apps/multimodal_rag/src/util/retry.cpp` | RetryPolicy + Conditional Edge | `lib/crewai/src/crewai/task.py`（`Task` 重试机制） | `autogen-agentchat/src/autogen_agentchat/agents/assistant_agent.py`（`tool_call_max_consecutive_failure=3`） | 重试 |
| `engine/apps/multimodal_rag/src/util/streaming_builder.cpp` | `libs/langgraph/langgraph/pregel/_io.py`（5 模式 stream） | `lib/crewai/src/crewai/flow/state.py`（Flow State）+ `streaming_callback=True` | `autogen-agentchat/src/autogen_agentchat/teams/`（`team.run_stream()`） | 流式构建器 |
| `engine/apps/multimodal_rag/src/util/thread_pool.cpp` | `libs/langgraph/langgraph/pregel/_executor.py`（ConcurrentExecutor） | `lib/crewai/src/crewai/execution.py`（AsyncExecutor + concurrent.futures） | `autogen-core/src/autogen_core/_agent_runtime.py`（SingleThreadedAgentRuntime + DistributedAgentRuntime gRPC / WebSocket / Redis / Kafka） | 线程池 / 分布式 Runtime |
| `engine/apps/multimodal_rag/src/util/gpu_config.cpp` | LangSmith SaaS GPU + LangGraph Studio | AMP Cloud GPU + CrewAI Enterprise | AutoGen Studio GPU + Magentic-One CLI + 9 类多模态工具 | GPU / 多模态 |

---

## 9. 附录 B — 自研与 13 篇子报告的关系

| 子报告 # | 主题 | 与 Agent 子报告的关系 |
|---------|------|---------------------|
| 1 | relational | 自研关系栈（`engine/src/db/storage/` + `executor/sql/` + `optimizer/`）为 Agent 提供 CRUD + JOIN + SQL 执行能力 |
| 2 | key-value | 自研 KV 引擎（`engine/src/db/storage/kv/`）为 Agent 提供 KV 状态存储 + 跨进程记忆 |
| 3 | document | 自研 document 引擎为 Agent 提供文档解析 + 切分（chunker / code_chunker / multimodal_parser / pdf_parser / semantic_chunker 5 类） |
| 4 | columnar | 自研列存引擎为 Agent 提供 OLAP 分析 + 列式聚合 |
| 5 | time-series | 自研时序引擎为 Agent 提供时间窗口 + 连续聚合 |
| 6 | search | 自研 BM25 + CJK 分词 + 全文索引为 Agent 提供 keyword 检索能力（与向量检索互补） |
| 7 | vector | 自研 30+ 向量子目录 + 量化全家桶 + HNSW + DiskANN + GPU 为 Agent 提供向量检索 + 重排 + 多模态 |
| 8 | graph | 自研 graph 引擎 + RDF/SPARQL + ltree 为 Agent 提供图遍历 + SPARQL 查询（GraphRAG 直接依赖） |
| 9 | stream | 自研流处理引擎为 Agent 提供流式摄入 + 实时响应（与本篇 Self-RAG stream 输出互补） |
| 10 | embedded | 自研嵌入式 mm_* 系列 + 乐观事务 + MVCC 为 Agent 提供事务一致性 + 嵌入式集成 |
| 11 | extension | 自研 Plugin Manager + REST/gRPC server 为 Agent 提供扩展机制 + 协议层（与 LangGraph Studio / LangSmith SaaS 对标） |
| 12 | rag | **同源姊妹篇**，14 子模块 RAG 服务（agent / core / embed / engine / eval / graph / index / llm / observability / pipeline / retrieval / server / util），本篇聚焦 13 号 Agent 子模块 + 5 类学术 Pipeline |
| 13 | **agent（本篇）** | **本系列最后 1 篇**，对照 LangGraph / CrewAI / AutoGen，从 Agent 三件套 + 5 类 Pipeline + DB 内核 RAG 算子对照 |

---

## 10. 完结说明

> 本篇为 **13 篇子报告之十三（系列最后 1 篇）**，与姊妹篇 `docs/analysis/rag.md` 共同覆盖 RAG + Agent 应用层；前 11 篇覆盖 DB 内核 + 多模态存储 + 向量 + 图 + 流 + 嵌入式 + 扩展机制，姊妹篇 `rag.md` 覆盖 14 子模块 RAG 应用层，本篇聚焦 Agent 子模块 + 5 类学术 Pipeline + DB 内核 RAG 算子的「自研 vs LangGraph / CrewAI / AutoGen」对照分析。

至此，**gap-analysis 计划全套 13 篇子报告全部完成**，覆盖：

- DB 内核 8 篇（relational / key-value / columnar / time-series / search / vector / graph / stream）；
- 嵌入式 + 扩展机制 2 篇（embedded / extension）；
- 应用层 2 篇（rag / agent）；
- 文档存储 1 篇（document，与姊妹篇并存）。

下一步建议参考 13 篇子报告的 **7. 补齐项下月计划** + **附录 A 路径对照表** 制定下季度 OPSX 大变更任务分配。
