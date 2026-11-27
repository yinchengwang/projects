# -*- coding: utf-8 -*-
"""metagpt 机制清单（阶段 2A 第四个 AI / Agent 项目）。

盘点范围：`metagpt/` 主包。这是一个「SOP 驱动」的 agent 框架：角色（Role）按
标准流程串起动作（Action），团队（Team）按预算与成本约束投资，环境
（Environment）承载消息与共享记忆。它与其他 agent 框架的最大差别是
**把软件公司的岗位分工写成了代码结构**。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析），再按包内目录
骨架归并成机制簇。篇数是归并结果，不是按文件数折算。

`rag/engines/flare.py` 只是把 llama-index 的 FLAREInstructQueryEngine 转出，
没有本仓机制，因此不含独立页面。

源码锚点：`metagpt@11cdf466d042aece04fc6cfd13b28e1a70341b1f`。
"""

GROUP_ORDER = [
    "A-角色与团队",
    "B-环境与消息",
    "C-动作与动作图",
    "D-策略与规划",
    "E-记忆与经验池",
    "F-RAG 检索链路",
    "G-模型提供方",
    "H-工具与技能",
]

MECHANISM_SPECS = [
    # ============ A 角色与团队 ============
    {
        "feature_group": "A-角色与团队",
        "feature": "base-role-contract",
        "title": "BaseRole 抽象与三类生命周期钩子",
        "page_file": "a01-base-role-contract.md",
        "boundary": "负责角色的最小契约：空闲判定、思考、行动、反应、运行与记忆读取六个抽象点；"
                    "不负责具体反应模式（a02）、不负责团队编排（a04）。",
        "refs": [
            {"path": "metagpt/base/base_role.py",
             "symbols": ["BaseRole", "is_idle", "react"]},
        ],
    },
    {
        "feature_group": "A-角色与团队",
        "feature": "role-react-modes",
        "title": "Role 的三种反应模式",
        "page_file": "a02-role-react-modes.md",
        "boundary": "负责 Role 如何按 react / by_order / plan_and_act 三种模式推进一轮状态机，"
                    "含 watch 观测与 todo 驱动；不负责动作内部逻辑（C 组）、不负责规划器（d02）。",
        "refs": [
            {"path": "metagpt/roles/role.py",
             "symbols": ["Role", "RoleReactMode", "_react", "_plan_and_act"]},
            {"path": "metagpt/roles/role.py",
             "symbols": ["think", "act", "_observe"]},
        ],
    },
    {
        "feature_group": "A-角色与团队",
        "feature": "role-context",
        "title": "角色上下文与状态注入",
        "page_file": "a03-role-context.md",
        "boundary": "负责 RoleContext 承载的运行时状态：消息历史、重要记忆、当前 todo、环境引用；"
                    "不负责反应模式（a02）、不负责记忆存储（e01）。",
        "refs": [
            {"path": "metagpt/roles/role.py",
             "symbols": ["RoleContext", "important_memory", "history"]},
            {"path": "metagpt/roles/role.py",
             "symbols": ["set_env", "_set_state", "publish_message"]},
        ],
    },
    {
        "feature_group": "A-角色与团队",
        "feature": "team-investment",
        "title": "团队投资与成本控制",
        "page_file": "a04-team-investment.md",
        "boundary": "负责 Team 如何雇佣角色、按预算投资、检查余额并在超支时停止；"
                    "不负责角色内部推进（a02）、不负责环境运行循环（b03）。",
        "refs": [
            {"path": "metagpt/team.py",
             "symbols": ["Team", "hire", "invest", "_check_balance"]},
            {"path": "metagpt/team.py",
             "symbols": ["run_project", "start_project"]},
        ],
    },

    # ============ B 环境与消息 ============
    {
        "feature_group": "B-环境与消息",
        "feature": "base-environment",
        "title": "BaseEnvironment 抽象与角色注册",
        "page_file": "b01-base-environment.md",
        "boundary": "负责环境契约：恢复、观测、步进、发布、运行五个抽象点；"
                    "不负责具体环境的地址分配（b02）、不负责消息路由细节（b03）。",
        "refs": [
            {"path": "metagpt/base/base_env.py",
             "symbols": ["BaseEnvironment", "publish_message", "step"]},
        ],
    },
    {
        "feature_group": "B-环境与消息",
        "feature": "environment-and-addresses",
        "title": "Environment 的角色管理与地址分配",
        "page_file": "b02-environment-and-addresses.md",
        "boundary": "负责 Environment 如何登记角色、分配可读可写地址、判定空闲并使用消息池；"
                    "不负责外部 API 环境（b04）、不负责消息发送语义（b03）。",
        "refs": [
            {"path": "metagpt/environment/base_env.py",
             "symbols": ["Environment", "add_role", "get_addresses"]},
            {"path": "metagpt/environment/base_env.py",
             "symbols": ["is_idle", "run", "archive"]},
        ],
    },
    {
        "feature_group": "B-环境与消息",
        "feature": "message-exchange",
        "title": "消息对象与收发路径",
        "page_file": "b03-message-exchange.md",
        "boundary": "负责消息的结构、角色间收发与共享池的存取规则；"
                    "不负责环境（b01）、不负责记忆（e01）。",
        "refs": [
            {"path": "metagpt/schema.py", "symbols": ["Message", "UserMessage", "SystemMessage"]},
            {"path": "metagpt/environment/base_env.py",
             "symbols": ["publish_message", "get_role"]},
        ],
    },
    {
        "feature_group": "B-环境与消息",
        "feature": "ext-environment",
        "title": "外部 API 环境与可读可写标注",
        "page_file": "b04-ext-environment.md",
        "boundary": "负责把外部 OpenAPI 服务包装成环境：API 探测、读写标注与观测/步进转换；"
                    "不负责内部地址分配（b02）、不负责工具注册（h01）。",
        "refs": [
            {"path": "metagpt/environment/base_env.py",
             "symbols": ["ExtEnv", "get_all_available_apis", "read_from_api"]},
            {"path": "metagpt/environment/base_env.py",
             "symbols": ["EnvType", "mark_as_readable", "mark_as_writeable"]},
        ],
    },

    # ============ C-动作与动作图 ============
    {
        "feature_group": "C-动作与动作图",
        "feature": "action-contract",
        "title": "Action 基类与提示装配",
        "page_file": "c01-action-contract.md",
        "boundary": "负责动作契约：前缀与指令装配、私有模型、提示模式、上下文覆盖与运行入口；"
                    "不负责动作图调度（c03）、不负责输出解析（c02）。",
        "refs": [
            {"path": "metagpt/actions/action.py",
             "symbols": ["Action", "set_prefix", "_aask", "run"]},
        ],
    },
    {
        "feature_group": "C-动作与动作图",
        "feature": "action-node",
        "title": "ActionNode 与结构化输出",
        "page_file": "c02-action-node.md",
        "boundary": "负责用节点描述一次结构化输出：字段填充、评审、修订与输出类注册；"
                    "不负责动作基类（c01）、不负责动作图（c03）。",
        "refs": [
            {"path": "metagpt/actions/action_node.py",
             "symbols": ["ActionNode", "ReviewMode", "FillMode"]},
            {"path": "metagpt/actions/action_outcls_registry.py",
             "symbols": ["register_action_outcls"]},
        ],
    },
    {
        "feature_group": "C-动作与动作图",
        "feature": "action-graph",
        "title": "动作图与依赖执行",
        "page_file": "c03-action-graph.md",
        "boundary": "负责把多个动作按输入依赖组成有向图并依次求解；"
                    "不负责单个动作（c01）、不负责角色反应模式（a02）。",
        "refs": [
            {"path": "metagpt/actions/action_graph.py",
             "symbols": ["ActionGraph"]},
            {"path": "metagpt/actions/action.py", "symbols": ["_run_action_node"]},
        ],
    },
    {
        "feature_group": "C-动作与动作图",
        "feature": "sop-actions",
        "title": "SOP 动作序列：需求到代码",
        "page_file": "c04-sop-actions.md",
        "boundary": "负责软件公司 SOP 里几个核心动作各自承担什么、产出什么，以及它们如何串成"
                    "「需求分析 → 设计 → 任务分解 → 编码 → 运行」；不负责动作契约（c01）、"
                    "不负责角色分工（a02）。",
        "refs": [
            {"path": "metagpt/actions/analyze_requirements.py",
             "symbols": ["AnalyzeRequirementsRestrictions"]},
            {"path": "metagpt/actions/design_api.py", "symbols": ["WriteDesign"]},
            {"path": "metagpt/actions/project_management.py", "symbols": ["WriteTasks"]},
            {"path": "metagpt/actions/write_code.py", "symbols": ["WriteCode"]},
            {"path": "metagpt/actions/run_code.py", "symbols": ["RunCode"]},
        ],
    },

    # ============ D-策略与规划 ============
    {
        "feature_group": "D-策略与规划",
        "feature": "solver-family",
        "title": "求解器家族与策略抽象",
        "page_file": "d01-solver-family.md",
        "boundary": "负责 BaseSolver 的契约与几种实现（朴素、ReAct、思维链、思维树、"
                    "数据解释器）各自的推进方式；不负责规划器（d02）、不负责搜索树（d04）。",
        "refs": [
            {"path": "metagpt/strategy/solver.py",
             "symbols": ["BaseSolver", "ReActSolver", "COTSolver"]},
            {"path": "metagpt/strategy/solver.py", "symbols": ["NaiveSolver", "TOTSolver"]},
        ],
    },
    {
        "feature_group": "D-策略与规划",
        "feature": "planner",
        "title": "任务规划器与人工确认",
        "page_file": "d02-planner.md",
        "boundary": "负责规划器如何维护任务栈、更新计划、处理任务结果并请求人工确认；"
                    "不负责求解器（d01）、不负责动作执行（c01）。",
        "refs": [
            {"path": "metagpt/strategy/planner.py",
             "symbols": ["Planner", "update_plan", "process_task_result"]},
            {"path": "metagpt/strategy/planner.py", "symbols": ["ask_review", "confirm_task"]},
        ],
    },
    {
        "feature_group": "D-策略与规划",
        "feature": "experience-retriever",
        "title": "经验检索器与策略注入",
        "page_file": "d03-experience-retriever.md",
        "boundary": "负责按策略名检索历史经验并注入提示，含若干检索实现；"
                    "不负责经验池存储（e05）、不负责规划（d02）。",
        "refs": [
            {"path": "metagpt/strategy/experience_retriever.py",
             "symbols": ["ExpRetriever", "TRDExpRetriever", "SimpleExpRetriever"]},
        ],
    },
    {
        "feature_group": "D-策略与规划",
        "feature": "tree-of-thought",
        "title": "思维树与搜索空间",
        "page_file": "d04-tree-of-thought.md",
        "boundary": "负责思维树的节点评估、搜索空间与 BFS/DFS/MCTS 三种遍历；"
                    "不负责求解器上层（d01）、不负责经验检索（d03）。",
        "refs": [
            {"path": "metagpt/strategy/base.py",
             "symbols": ["ThoughtNode", "ThoughtTree", "BaseEvaluator"]},
            {"path": "metagpt/strategy/tot.py",
             "symbols": ["BFSSolver", "DFSSolver", "MCTSSolver"]},
            {"path": "metagpt/strategy/search_space.py", "symbols": ["SearchSpace"]},
        ],
    },

    # ============ E-记忆与经验池 ============
    {
        "feature_group": "E-记忆与经验池",
        "feature": "role-memory",
        "title": "角色记忆与消息归档",
        "page_file": "e01-role-memory.md",
        "boundary": "负责角色级记忆的增删查与按角色/内容/动作检索，以及消息归档规则；"
                    "不负责脑记忆（e03）、不负责长期记忆（e04）。",
        "refs": [
            {"path": "metagpt/memory/memory.py",
             "symbols": ["Memory", "add", "try_remember"]},
            {"path": "metagpt/memory/memory.py",
             "symbols": ["get_by_role", "get_by_actions", "delete_newest"]},
        ],
    },
    {
        "feature_group": "E-记忆与经验池",
        "feature": "brain-memory",
        "title": "脑记忆与短期上下文压缩",
        "page_file": "e02-brain-memory.md",
        "boundary": "负责 BrainMemory 如何保存消息历史并在超限时压缩摘要；"
                    "不负责角色记忆（e01）、不负责长期检索（e04）。",
        "refs": [
            {"path": "metagpt/memory/brain_memory.py",
             "symbols": ["BrainMemory", "Config"]},
        ],
    },
    {
        "feature_group": "E-记忆与经验池",
        "feature": "longterm-memory",
        "title": "长期记忆与向量检索",
        "page_file": "e03-longterm-memory.md",
        "boundary": "负责长期记忆的写入与检索路径，以及在角色中的挂载方式；"
                    "不负责脑记忆（e02）、不负责文档存储后端（e04）。",
        "refs": [
            {"path": "metagpt/memory/longterm_memory.py", "symbols": ["LongTermMemory"]},
            {"path": "metagpt/memory/role_zero_memory.py",
             "symbols": ["RoleZeroLongTermMemory"]},
        ],
    },
    {
        "feature_group": "E-记忆与经验池",
        "feature": "document-store",
        "title": "文档存储后端抽象",
        "page_file": "e04-document-store.md",
        "boundary": "负责向量/文档存储的统一契约与多个后端实现（FAISS、Chroma、Qdrant、"
                    "Milvus、LanceDB）；不负责记忆门面（e01）、不负责 RAG 引擎（f01）。",
        "refs": [
            {"path": "metagpt/document_store/base_store.py",
             "symbols": ["BaseStore", "LocalStore", "search"]},
        ],
    },
    {
        "feature_group": "E-记忆与经验池",
        "feature": "experience-pool",
        "title": "经验池与经验生命周期",
        "page_file": "e05-experience-pool.md",
        "boundary": "负责经验池的存储解析、经验创建与查询、以及 BM25/向量两种后端选择；"
                    "不负责检索器（d03）、不负责记忆（e01）。",
        "refs": [
            {"path": "metagpt/exp_pool/manager.py",
             "symbols": ["ExperienceManager", "create_exps", "query_exps"]},
            {"path": "metagpt/exp_pool/schema.py",
             "symbols": ["Experience", "ExperienceType", "Trajectory"]},
        ],
    },

    # ============ F-RAG 检索链路 ============
    {
        "feature_group": "F-RAG 检索链路",
        "feature": "simple-engine",
        "title": "SimpleEngine 与检索器能力校验",
        "page_file": "f01-simple-engine.md",
        "boundary": "负责 RAG 引擎的构建与查询、增删文档、持久化，以及按能力校验检索器类型；"
                    "不负责检索器实现（f02）、不负责重排（f03）。",
        "refs": [
            {"path": "metagpt/rag/engines/simple.py",
             "symbols": ["SimpleEngine", "from_docs", "retrieve"]},
            {"path": "metagpt/rag/engines/simple.py",
             "symbols": ["_ensure_retriever_queryable", "add_docs", "persist"]},
        ],
    },
    {
        "feature_group": "F-RAG 检索链路",
        "feature": "retriever-factory",
        "title": "检索器工厂与混合检索",
        "page_file": "f02-retriever-factory.md",
        "boundary": "负责按类型创建检索器、以及混合检索如何合并多个来源的结果；"
                    "不负责引擎门面（f01）、不负责重排（f03）。",
        "refs": [
            {"path": "metagpt/rag/factories/retriever.py",
             "symbols": ["RetrieverFactory"]},
            {"path": "metagpt/rag/retrievers/hybrid_retriever.py",
             "symbols": ["SimpleHybridRetriever"]},
        ],
    },
    {
        "feature_group": "F-RAG 检索链路",
        "feature": "rerank-postprocessor",
        "title": "重排后处理与对象还原",
        "page_file": "f03-rerank-postprocessor.md",
        "boundary": "负责检索结果的重排与从节点还原对象；不负责检索（f02）、"
                    "不负责引擎构建（f01）。",
        "refs": [
            {"path": "metagpt/rag/rankers/object_ranker.py",
             "symbols": ["ObjectSortPostprocessor"]},
            {"path": "metagpt/rag/engines/simple.py",
             "symbols": ["_try_reconstruct_obj", "get_obj_nodes"]},
        ],
    },

    # ============ G-模型提供方 ============
    {
        "feature_group": "G-模型提供方",
        "feature": "base-llm-provider",
        "title": "模型基类与成本计费",
        "page_file": "g01-base-llm-provider.md",
        "boundary": "负责模型基类的调用契约、重试、成本累计与缓存；"
                    "不负责提供方注册表（g02）、不负责具体厂商实现。",
        "refs": [
            {"path": "metagpt/provider/base_llm.py",
             "symbols": ["BaseLLM"]},
        ],
    },
    {
        "feature_group": "G-模型提供方",
        "feature": "llm-provider-registry",
        "title": "提供方注册与选择",
        "page_file": "g02-llm-provider-registry.md",
        "boundary": "负责按配置查找并实例化模型提供方，含注册与默认回退；"
                    "不负责模型基类（g01）、不负责成本（a04）。",
        "refs": [
            {"path": "metagpt/provider/llm_provider_registry.py",
             "symbols": ["LLMProviderRegistry"]},
        ],
    },

    # ============ H-工具与技能 ============
    {
        "feature_group": "H-工具与技能",
        "feature": "tool-registry",
        "title": "工具注册表与 schema 生成",
        "page_file": "h01-tool-registry.md",
        "boundary": "负责工具的注册、按标签查询、从代码/路径批量注册，以及 schema 生成；"
                    "不负责工具推荐（h02）、不负责代码转 schema 的 AST 细节（h03）。",
        "refs": [
            {"path": "metagpt/tools/tool_registry.py",
             "symbols": ["ToolRegistry", "register_tool", "make_schema"]},
            {"path": "metagpt/tools/tool_registry.py",
             "symbols": ["register_tools_from_path", "get_tools_by_tag"]},
        ],
    },
    {
        "feature_group": "H-工具与技能",
        "feature": "tool-recommend",
        "title": "工具推荐与检索策略",
        "page_file": "h02-tool-recommend.md",
        "boundary": "负责按任务推荐工具：类型匹配、BM25 与向量三种策略；"
                    "不负责注册表（h01）、不负责代码解析（h03）。",
        "refs": [
            {"path": "metagpt/tools/tool_recommend.py",
             "symbols": ["ToolRecommender", "BM25ToolRecommender", "EmbeddingToolRecommender"]},
        ],
    },
    {
        "feature_group": "H-工具与技能",
        "feature": "code-to-tool-schema",
        "title": "代码转工具 schema 的 AST 分析",
        "page_file": "h03-code-to-tool-schema.md",
        "boundary": "负责用 AST 把函数/类方法的签名与文档字符串转成工具 schema；"
                    "不负责注册表（h01）、不负责推荐（h02）。",
        "refs": [
            {"path": "metagpt/tools/tool_convert.py",
             "symbols": ["convert_code_to_tool_schema", "convert_code_to_tool_schema_ast",
                         "CodeVisitor"]},
        ],
    },
    {
        "feature_group": "H-工具与技能",
        "feature": "skill-system",
        "title": "技能声明与技能动作",
        "page_file": "h04-skill-system.md",
        "boundary": "负责技能的声明结构、加载与调用（技能动作、技能管理器）；"
                    "不负责工具注册表（h01）、不负责推荐（h02）。",
        "refs": [
            {"path": "metagpt/learn/skill_loader.py",
             "symbols": ["Skill", "SkillsDeclaration", "Components"]},
            {"path": "metagpt/actions/skill_action.py",
             "symbols": ["SkillAction", "ArgumentsParingAction"]},
            {"path": "metagpt/management/skill_manager.py", "symbols": ["SkillManager"]},
        ],
    },
]
