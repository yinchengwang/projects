# -*- coding: utf-8 -*-
"""ragas 机制清单（阶段 2C 第七个项目）。

盘点范围：`src/ragas/` 下 238 个 py 文件。`tests/`、`docs/`、`examples/`、`cli` 之外的
脚本不建页；`integrations/` 只做一页汇总而不是每个适配器一页。

ragas 与前六个项目的差别是**它卖的不是管道，是尺子**：
- langchain / llama-index / haystack 解决「怎么把 RAG 搭起来」；
- lightrag / graphrag / ragflow 解决「怎么把 RAG 做成一个产品」；
- **ragas 解决「搭起来的 RAG 到底好不好」**。

因此它的机制核心不是「组件如何连接」，而是「怎么把一个模糊的质量问题变成一个可执行的
评分过程」：把「答案有没有幻觉」拆成「抽陈述 → 逐条判定是否被上下文支持 → 算比例」，
把「检索准不准」拆成「每个上下文块是否有用 → 加权平均」。**每一条指标都是一次
「先分解、再让 LLM 判定、最后聚合」的固定套路**，而这一套路的可复用性正是这个库的价值。

它还有一条值得单独读的弧线：**同一个指标存在两代实现**——`metrics/_*.py` 里的旧式
`Metric` 子类，与 `metrics/collections/*/` 里的新式 `BaseMetric` 组件。两代并存不是为了
兼容，而是在迁移中途；理解两代的差别才能理解这个库当前的形态。

组划分按「抽象 → 指标 → 执行 → 支撑」切：A 指标抽象 → B LLM 判定类指标 →
C 非 LLM 指标 → D 数据集与执行 → E 提示层 → F 模型接入 → G 测试集合成 →
H 优化器 → I 运行时支撑。

源码锚点：`ragas@8c63f10d500c44efbf24fdb9e2920de2104c321c`。
"""

GROUP_ORDER = [
    "A-指标抽象",
    "B-LLM 判定类指标",
    "C-非 LLM 指标",
    "D-数据集与执行",
    "E-提示层",
    "F-模型接入",
    "G-测试集合成",
    "H-优化器",
    "I-运行时支撑",
]

MECHANISM_SPECS = [
    # ============ A-指标抽象 ============
    {
        "feature_group": "A-指标抽象",
        "feature": "legacy-metric",
        "title": "旧式指标契约",
        "page_file": "a01-legacy-metric.md",
        "boundary": "负责第一代 `Metric` 家族的抽象契约（单轮/多轮、是否需要 LLM/嵌入）；"
                    "不负责新式组件指标（a02）、不负责装饰器形态（a03）。",
        "refs": [
            {"path": "src/ragas/metrics/base.py",
             "symbols": ["Metric", "MetricType", "MetricOutputType"]},
            {"path": "src/ragas/metrics/base.py",
             "symbols": ["MetricWithLLM", "MetricWithEmbeddings"]},
            {"path": "src/ragas/metrics/base.py",
             "symbols": ["SingleTurnMetric", "MultiTurnMetric"]},
        ],
    },
    {
        "feature_group": "A-指标抽象",
        "feature": "modern-metric",
        "title": "新式组件指标",
        "page_file": "a02-modern-metric.md",
        "boundary": "负责第二代 `BaseMetric` 组件的契约与两代并存的状态；"
                    "不负责旧式契约（a01）、不负责装饰器（a03）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/base.py", "symbols": ["BaseMetric"]},
            {"path": "src/ragas/metrics/collections/__init__.py",
             "symbols": ["Faithfulness", "AnswerRelevancy"]},
            {"path": "src/ragas/metrics/collections/summary_score/metric.py",
             "symbols": ["SummaryScore"]},
        ],
    },
    {
        "feature_group": "A-指标抽象",
        "feature": "metric-decorator",
        "title": "装饰器式自定义指标",
        "page_file": "a03-metric-decorator.md",
        "boundary": "负责用装饰器声明自定义指标的三种输出形态（数值/离散/排序）；"
                    "不负责类式契约（a01/a02）、不负责结果与校验（a04）。",
        "refs": [
            {"path": "src/ragas/metrics/decorator.py",
             "symbols": ["create_metric_decorator", "NumericMetricProtocol",
                         "DiscreteMetricProtocol"]},
            {"path": "src/ragas/metrics/numeric.py",
             "symbols": ["NumericMetric", "numeric_metric"]},
            {"path": "src/ragas/metrics/discrete.py",
             "symbols": ["DiscreteMetric", "discrete_metric"]},
            {"path": "src/ragas/metrics/ranking.py",
             "symbols": ["RankingMetric", "ranking_metric"]},
        ],
    },
    {
        "feature_group": "A-指标抽象",
        "feature": "metric-result",
        "title": "结果结构与输出校验",
        "page_file": "a04-metric-result.md",
        "boundary": "负责指标结果的承载结构与输出取值校验；"
                    "不负责指标契约（a01/a02）、不负责执行编排（d02）。",
        "refs": [
            {"path": "src/ragas/metrics/result.py", "symbols": ["MetricResult"]},
            {"path": "src/ragas/metrics/validators.py",
             "symbols": ["BaseValidator", "get_validator_for_allowed_values",
                         "get_metric_type_name"]},
            {"path": "src/ragas/metrics/validators.py", "symbols": ["NumericValidator"]},
        ],
    },

    # ============ B-LLM 判定类指标 ============
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "faithfulness",
        "title": "忠实度：陈述级判据",
        "page_file": "b01-faithfulness.md",
        "boundary": "负责把答案拆成陈述并逐条判定是否被上下文支持；"
                    "不负责相关性（b02）、不负责事实正确性（b04）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/faithfulness/metric.py",
             "symbols": ["Faithfulness"]},
            {"path": "src/ragas/metrics/_faithfulness.py",
             "symbols": ["Faithfulness", "StatementGeneratorPrompt", "NLIStatementPrompt"]},
        ],
    },
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "answer-relevance",
        "title": "答案相关性与正确性",
        "page_file": "b02-answer-relevance.md",
        "boundary": "负责答案是否切题、以及答案与标准答案的正确性比对；"
                    "不负责忠实度（b01）、不负责上下文质量（b03）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/answer_relevancy/metric.py",
             "symbols": ["AnswerRelevancy"]},
            {"path": "src/ragas/metrics/collections/answer_correctness/metric.py",
             "symbols": ["AnswerCorrectness"]},
            {"path": "src/ragas/metrics/_answer_relevance.py",
             "symbols": ["ResponseRelevancePrompt", "ResponseRelevancy"]},
        ],
    },
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "context-quality",
        "title": "上下文精确率与召回率",
        "page_file": "b03-context-quality.md",
        "boundary": "负责检索侧的上下文质量（精确率、利用率、召回率）；"
                    "不负责答案侧（b01/b02）、不负责非 LLM 指标（C 组）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/context_precision/metric.py",
             "symbols": ["ContextPrecisionWithReference", "ContextPrecisionWithoutReference",
                         "ContextUtilization"]},
            {"path": "src/ragas/metrics/collections/context_recall/metric.py",
             "symbols": ["ContextRecall"]},
            {"path": "src/ragas/metrics/collections/context_relevance/metric.py",
             "symbols": ["ContextRelevance"]},
        ],
    },
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "factual-correctness",
        "title": "事实正确性与声索分解",
        "page_file": "b04-factual-correctness.md",
        "boundary": "负责把答案与参考答案拆成声索、按交并比计算事实正确性；"
                    "不负责忠实度（b01）、不负责主题一致（b05）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/factual_correctness/metric.py",
             "symbols": ["FactualCorrectness"]},
            {"path": "src/ragas/metrics/_factual_correctness.py",
             "symbols": ["ClaimDecompositionPrompt", "DecompositionType"]},
        ],
    },
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "topic-and-summary",
        "title": "主题一致性与摘要质量",
        "page_file": "b05-topic-and-summary.md",
        "boundary": "负责主题一致性判定与摘要质量评测；"
                    "不负责事实正确性（b04）、不负责 Agent 侧指标（b06）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/topic_adherence/metric.py",
             "symbols": ["TopicAdherence"]},
            {"path": "src/ragas/metrics/collections/summary_score/metric.py",
             "symbols": ["SummaryScore"]},
        ],
    },
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "agent-metrics",
        "title": "Agent 与会话指标",
        "page_file": "b06-agent-metrics.md",
        "boundary": "负责目标达成度与工具调用正确性；"
                    "不负责摘要（b05）、不负责评分规则（b07）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/agent_goal_accuracy/metric.py",
             "symbols": ["AgentGoalAccuracyWithReference", "AgentGoalAccuracyWithoutReference"]},
            {"path": "src/ragas/metrics/collections/tool_call_accuracy/metric.py",
             "symbols": ["ToolCallAccuracy"]},
        ],
    },
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "rubrics",
        "title": "评分规则类指标",
        "page_file": "b07-rubrics.md",
        "boundary": "负责领域规则、实例规则与简单判据三类「带标准答案的评分」；"
                    "不负责预置指标（b01-b06）、不负责多模态（b08）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/domain_specific_rubrics/metric.py",
             "symbols": ["DomainSpecificRubrics"]},
            {"path": "src/ragas/metrics/collections/instance_specific_rubrics/metric.py",
             "symbols": ["InstanceSpecificRubrics"]},
            {"path": "src/ragas/metrics/_simple_criteria.py",
             "symbols": ["SimpleCriteriaScore", "SingleTurnSimpleCriteriaPrompt"]},
        ],
    },
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "multimodal-metrics",
        "title": "多模态指标",
        "page_file": "b08-multimodal-metrics.md",
        "boundary": "负责图像参与时的忠实度与相关性判定；"
                    "不负责文本指标（b01-b07）、不负责噪声敏感度（b09）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/multi_modal_faithfulness/metric.py",
             "symbols": ["MultiModalFaithfulness"]},
            {"path": "src/ragas/metrics/collections/multi_modal_relevance/metric.py",
             "symbols": ["MultiModalRelevance"]},
        ],
    },
    {
        "feature_group": "B-LLM 判定类指标",
        "feature": "noise-sensitive",
        "title": "噪声敏感度与引用片段",
        "page_file": "b09-noise-sensitive.md",
        "boundary": "负责上下文噪声对答案的影响度量、以及引用片段的抽取与判定；"
                    "不负责上下文精确率（b03）、不负责多模态（b08）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/noise_sensitivity/metric.py",
             "symbols": ["NoiseSensitivity"]},
            {"path": "src/ragas/metrics/collections/quoted_spans/metric.py",
             "symbols": ["QuotedSpansAlignment"]},
        ],
    },

    # ============ C-非 LLM 指标 ============
    {
        "feature_group": "C-非 LLM 指标",
        "feature": "string-metrics",
        "title": "字符串类指标",
        "page_file": "c01-string-metrics.md",
        "boundary": "负责编辑距离、精确匹配、BLEU/ROUGE/CHRF 等不依赖模型的相似度；"
                    "不负责语义相似度（c02）、不负责 LLM 判定指标（B 组）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/_string.py",
             "symbols": ["DistanceMeasure", "ExactMatch", "NonLLMStringSimilarity"]},
            {"path": "src/ragas/metrics/collections/_bleu_score.py", "symbols": ["BleuScore"]},
            {"path": "src/ragas/metrics/collections/_rouge_score.py", "symbols": ["RougeScore"]},
            {"path": "src/ragas/metrics/collections/chrf_score/metric.py",
             "symbols": ["CHRFScore"]},
        ],
    },
    {
        "feature_group": "C-非 LLM 指标",
        "feature": "semantic-metrics",
        "title": "语义相似与结构化比对",
        "page_file": "c02-semantic-metrics.md",
        "boundary": "负责嵌入相似度、表格比对与 SQL 语义等价三类非生成式判定；"
                    "不负责字符串指标（c01）、不负责 LLM 判定（B 组）。",
        "refs": [
            {"path": "src/ragas/metrics/collections/_semantic_similarity.py",
             "symbols": ["SemanticSimilarity"]},
            {"path": "src/ragas/metrics/collections/datacompy_score/metric.py",
             "symbols": ["DataCompyScore"]},
            {"path": "src/ragas/metrics/collections/sql_semantic_equivalence/metric.py",
             "symbols": ["SQLSemanticEquivalence"]},
        ],
    },

    # ============ D-数据集与执行 ============
    {
        "feature_group": "D-数据集与执行",
        "feature": "dataset-schema",
        "title": "样本与数据集结构",
        "page_file": "d01-dataset-schema.md",
        "boundary": "负责评测样本（单轮/多轮）与数据集容器的字段契约；"
                    "不负责执行编排（d02）、不负责持久化后端（i05）。",
        "refs": [
            {"path": "src/ragas/dataset_schema.py",
             "symbols": ["BaseSample", "SingleTurnSample", "MultiTurnSample"]},
            {"path": "src/ragas/dataset_schema.py",
             "symbols": ["RagasDataset", "EvaluationDataset", "EvaluationResult"]},
        ],
    },
    {
        "feature_group": "D-数据集与执行",
        "feature": "evaluation-run",
        "title": "评测执行与并发",
        "page_file": "d02-evaluation-run.md",
        "boundary": "负责一次评测的编排（指标分派、并发执行、结果汇总）；"
                    "不负责数据集结构（d01）、不负责重试配置（d03）。",
        "refs": [
            {"path": "src/ragas/evaluation.py", "symbols": ["evaluate", "aevaluate"]},
            {"path": "src/ragas/executor.py", "symbols": ["Executor", "run_async_batch"]},
        ],
    },
    {
        "feature_group": "D-数据集与执行",
        "feature": "run-config",
        "title": "运行配置与重试",
        "page_file": "d03-run-config.md",
        "boundary": "负责运行期配置的挂载与同步/异步重试包装；"
                    "不负责评测编排（d02）、不负责实验版本化（d04）。",
        "refs": [
            {"path": "src/ragas/run_config.py", "symbols": ["RunConfig", "add_retry"]},
            {"path": "src/ragas/run_config.py", "symbols": ["add_async_retry"]},
        ],
    },
    {
        "feature_group": "D-数据集与执行",
        "feature": "experiment",
        "title": "实验与表格容器",
        "page_file": "d04-experiment.md",
        "boundary": "负责实验的版本化包装与评测表格容器；"
                    "不负责评测编排（d02）、不负责测试集合成（G 组）。",
        "refs": [
            {"path": "src/ragas/experiment.py",
             "symbols": ["Experiment", "version_experiment", "experiment"]},
            {"path": "src/ragas/dataset.py", "symbols": ["DataTable", "Dataset"]},
        ],
    },

    # ============ E-提示层 ============
    {
        "feature_group": "E-提示层",
        "feature": "prompt-base",
        "title": "提示契约",
        "page_file": "e01-prompt-base.md",
        "boundary": "负责提示的抽象契约与输入/输出形态声明；"
                    "不负责 pydantic 提示（e02）、不负责少样例外挂（e03）。",
        "refs": [
            {"path": "src/ragas/prompt/base.py",
             "symbols": ["BasePrompt", "StringPrompt", "StringIO", "BoolIO"]},
        ],
    },
    {
        "feature_group": "E-提示层",
        "feature": "pydantic-prompt",
        "title": "结构化输出提示",
        "page_file": "e02-pydantic-prompt.md",
        "boundary": "负责把提示输出约束成 pydantic 模型、以及解析失败时的修复与重试；"
                    "不负责基础契约（e01）、不负责少样例外挂（e03）。",
        "refs": [
            {"path": "src/ragas/prompt/pydantic_prompt.py",
             "symbols": ["PydanticPrompt", "RagasOutputParser", "FixOutputFormat"]},
            {"path": "src/ragas/prompt/pydantic_prompt.py",
             "symbols": ["OutputStringAndPrompt"]},
        ],
    },
    {
        "feature_group": "E-提示层",
        "feature": "dynamic-few-shot",
        "title": "动态少样本",
        "page_file": "e03-dynamic-few-shot.md",
        "boundary": "负责按相似度动态挑选示例注入提示；"
                    "不负责提示契约（e01）、不负责结构化输出（e02）。",
        "refs": [
            {"path": "src/ragas/prompt/dynamic_few_shot.py",
             "symbols": ["DynamicFewShotPrompt", "SimpleExampleStore"]},
            {"path": "src/ragas/prompt/dynamic_few_shot.py",
             "symbols": ["SimpleInMemoryExampleStore"]},
        ],
    },

    # ============ F-模型接入 ============
    {
        "feature_group": "F-模型接入",
        "feature": "llm-wrappers",
        "title": "模型包装与工厂",
        "page_file": "f01-llm-wrappers.md",
        "boundary": "负责对各家模型客户端的统一包装与按名构造；"
                    "不负责嵌入（f02）、不负责提示（E 组）。",
        "refs": [
            {"path": "src/ragas/llms/base.py",
             "symbols": ["BaseRagasLLM", "LangchainLLMWrapper", "LlamaIndexLLMWrapper"]},
            {"path": "src/ragas/llms/base.py", "symbols": ["llm_factory"]},
        ],
    },
    {
        "feature_group": "F-模型接入",
        "feature": "embedding-wrappers",
        "title": "嵌入包装与工厂",
        "page_file": "f02-embedding-wrappers.md",
        "boundary": "负责嵌入模型的统一包装与按名构造（含旧式调用兼容）；"
                    "不负责对话模型（f01）、不负责非 LLM 指标（C 组）。",
        "refs": [
            {"path": "src/ragas/embeddings/base.py",
             "symbols": ["BaseRagasEmbeddings", "LangchainEmbeddingsWrapper",
                         "HuggingfaceEmbeddings"]},
            {"path": "src/ragas/embeddings/base.py", "symbols": ["embedding_factory"]},
        ],
    },

    # ============ G-测试集合成 ============
    {
        "feature_group": "G-测试集合成",
        "feature": "knowledge-graph",
        "title": "文档知识图谱",
        "page_file": "g01-knowledge-graph.md",
        "boundary": "负责把文档组织成节点/关系的知识图谱并支持多跳遍历；"
                    "不负责角色（g02）、不负责生成器（g04）。",
        "refs": [
            {"path": "src/ragas/testset/graph.py",
             "symbols": ["KnowledgeGraph", "Node", "Relationship", "NodeType"]},
        ],
    },
    {
        "feature_group": "G-测试集合成",
        "feature": "persona",
        "title": "问题角色生成",
        "page_file": "g02-persona.md",
        "boundary": "负责从知识图谱生成「谁会问什么问题」的角色画像；"
                    "不负责图谱（g01）、不负责合成器（g03）。",
        "refs": [
            {"path": "src/ragas/testset/persona.py",
             "symbols": ["generate_personas_from_kg", "Persona", "PersonaList"]},
        ],
    },
    {
        "feature_group": "G-测试集合成",
        "feature": "synthesizer-base",
        "title": "合成器契约与场景",
        "page_file": "g03-synthesizer-base.md",
        "boundary": "负责单跳/多跳合成器的抽象契约与场景结构；"
                    "不负责图谱（g01）、不负责总装（g04）。",
        "refs": [
            {"path": "src/ragas/testset/synthesizers/base.py",
             "symbols": ["BaseSynthesizer", "BaseScenario", "QueryLength", "QueryStyle"]},
            {"path": "src/ragas/testset/synthesizers/single_hop/base.py",
             "symbols": ["SingleHopScenario", "SingleHopQuerySynthesizer"]},
            {"path": "src/ragas/testset/synthesizers/multi_hop/base.py",
             "symbols": ["MultiHopScenario", "MultiHopQuerySynthesizer"]},
        ],
    },
    {
        "feature_group": "G-测试集合成",
        "feature": "testset-generator",
        "title": "测试集总装",
        "page_file": "g04-testset-generator.md",
        "boundary": "负责把知识图谱、角色与合成器组装成一次测试集生成流程；"
                    "不负责合成器内部（g03）、不负责优化器（H 组）。",
        "refs": [
            {"path": "src/ragas/testset/synthesizers/generate.py",
             "symbols": ["TestsetGenerator"]},
            {"path": "src/ragas/testset/synthesizers/testset_schema.py",
             "symbols": ["TestsetSample", "Testset"]},
        ],
    },

    # ============ H-优化器 ============
    {
        "feature_group": "H-优化器",
        "feature": "genetic-optimizer",
        "title": "提示遗传优化",
        "page_file": "h01-genetic-optimizer.md",
        "boundary": "负责用遗传算法迭代改进提示（交叉、变异、反馈）；"
                    "不负责 DSPy 优化（h02）、不负责提示契约（E 组）。",
        "refs": [
            {"path": "src/ragas/optimizers/genetic.py", "symbols": ["GeneticOptimizer"]},
            {"path": "src/ragas/optimizers/genetic.py",
             "symbols": ["CrossOverPrompt", "FeedbackMutationPrompt"]},
        ],
    },
    {
        "feature_group": "H-优化器",
        "feature": "dspy-optimizer",
        "title": "DSPy 编译优化",
        "page_file": "h02-dspy-optimizer.md",
        "boundary": "负责把 ragas 提示接到 DSPy 的优化器上；"
                    "不负责遗传优化（h01）、不负责提示契约（E 组）。",
        "refs": [
            {"path": "src/ragas/optimizers/base.py", "symbols": ["Optimizer"]},
            {"path": "src/ragas/optimizers/dspy_optimizer.py", "symbols": ["DSPyOptimizer"]},
        ],
    },

    # ============ I-运行时支撑 ============
    {
        "feature_group": "I-运行时支撑",
        "feature": "cache",
        "title": "结果缓存",
        "page_file": "i01-cache.md",
        "boundary": "负责评测结果的缓存键构造与磁盘缓存后端；"
                    "不负责成本统计（i02）、不负责并发（i04）。",
        "refs": [
            {"path": "src/ragas/cache.py",
             "symbols": ["CacheInterface", "DiskCacheBackend", "cacher"]},
            {"path": "src/ragas/cache.py", "symbols": ["_make_pydantic_picklable"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑",
        "feature": "cost",
        "title": "token 用量与成本回调",
        "page_file": "i02-cost.md",
        "boundary": "负责按提供方折算 token 用量并累计成本；"
                    "不负责缓存（i01）、不负责追踪（i05）。",
        "refs": [
            {"path": "src/ragas/cost.py", "symbols": ["TokenUsage", "CostCallbackHandler"]},
            {"path": "src/ragas/cost.py", "symbols": ["get_token_usage_for_openai"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑",
        "feature": "tokenizers",
        "title": "分词器适配",
        "page_file": "i03-tokenizers.md",
        "boundary": "负责分词器的统一入口与 tiktoken/HuggingFace 两种实现；"
                    "不负责成本（i02）、不负责异步（i04）。",
        "refs": [
            {"path": "src/ragas/tokenizers.py",
             "symbols": ["BaseTokenizer", "TiktokenWrapper", "HuggingFaceTokenizer"]},
            {"path": "src/ragas/tokenizers.py", "symbols": ["get_tokenizer"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑",
        "feature": "async-utils",
        "title": "异步工具与追踪",
        "page_file": "i04-async-utils.md",
        "boundary": "负责事件循环检测、并发批量执行与追踪回调；"
                    "不负责执行编排（d02）、不负责数据集后端（i05）。",
        "refs": [
            {"path": "src/ragas/async_utils.py",
             "symbols": ["is_event_loop_running", "run_async_tasks", "as_completed"]},
            {"path": "src/ragas/callbacks.py",
             "symbols": ["RagasTracer", "MetricTrace", "parse_run_traces"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑",
        "feature": "backends",
        "title": "数据集后端",
        "page_file": "i05-backends.md",
        "boundary": "负责评测数据集的外部存储后端与注册表；"
                    "不负责缓存（i01）、不负责数据集结构（d01）。",
        "refs": [
            {"path": "src/ragas/backends/base.py", "symbols": ["BaseBackend"]},
            {"path": "src/ragas/backends/registry.py",
             "symbols": ["BackendRegistry", "get_registry", "register_backend"]},
        ],
    },
]
