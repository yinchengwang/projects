---
project: crewai
feature: crew-object
title: Crew 对象与配置校验
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/crew.py
    symbols: [Crew, check_config, validate_tasks]
---

# Crew 对象与配置校验

## 一句话职责

`Crew` 是一个 Pydantic 模型，它把「智能体列表 + 任务列表 + 流程类型」一次性验成一个可执行的配置：字段层面只保证类型，真正的语义约束全部压在挂在其上的十几个 `model_validator(mode="after")` 里。

## 机制是什么

`Crew` 继承 `BaseModel`，同时混入 `FlowTrackable`（让它能挂进 Flow 的追踪体系）。它的字段分成三类：

一类是**用户声明**的输入面：`tasks`、`agents`、`process`、`manager_llm`、`manager_agent`、`memory`、`config`、`cache`、`max_rpm`、`planning`、`knowledge_sources`、`skills`、`security_config` 等。这些字段的默认值刻意宽松——`process` 默认 `sequential`，`memory` 默认 `False`，`cache` 默认 `False`（源码注释明确写了这是 opt-in，因为缓存对实时数据与有副作用的工具是危险的）。

二类是**运行时私有状态**，用 `PrivateAttr` 而不是字段：`_logger`、`_rpm_controller`、`_file_handler`、`_cache_handler`、`_memory`、`_task_output_handler`（一个 `TaskOutputStorageHandler`，负责把每次任务输出落库）、`_inputs`、`_logging_color`、`_kickoff_event_id`、以及两个成对的派发标志 `_execution_start_dispatched` / `_execution_end_dispatched`。私有状态不进序列化，因此不会污染检查点与 `model_dump`。

三类是**检查点指针**：`execution_context`、`checkpoint_inputs`、`checkpoint_train`、`checkpoint_kickoff_event_id`。它们是从检查点恢复后用来告知「这次不是从零开始」的锚点，`copy()` 里被显式排除，原因写在排除集合的注释里——带过去会让副本误以为自己要续跑，从而每个任务都被跳过。

`id` 字段是 `UUID4` 且 `frozen=True`，还额外挂了一个 `_deny_user_set_id` 校验器：除非验证上下文里带 `from_checkpoint`，否则用户显式传 `id` 直接抛 `PydanticCustomError`。这是把「标识必须由框架分配」当成硬约束。

## 怎么实现的

**校验链的顺序是有意义的。** `Crew` 上挂着按声明顺序执行的 `after` 校验器：`set_private_attrs` → `create_crew_memory` → `create_crew_knowledge` → `check_manager_llm` → `check_config` → `validate_tasks` → 六个针对异步与条件任务的校验器。因为 `after` 校验器能拿到已构造的实例，后面的校验器可以直接读前面校验器填好的私有状态。

- `set_private_attrs` 做纯装配：补 `CacheHandler`、装 `EventListener` 与 `TraceCollectionListener`、按 `verbose` 建 `Logger`、有 `output_log_file` 才建 `FileHandler`、按 `max_rpm` 建 `RPMController`，并把字符串形式的 `function_calling_llm` 用 `create_llm` 实体化。
- `create_crew_memory` 处理 `memory` 的三态：`True` 时按 `sanitize_scope_name(name)` 造出 `/crew/<name>` 形式的根作用域，再 `Memory(embedder=..., root_scope=..., llm=...)`；已是 `Memory` / `MemoryScope` / `MemorySlice` 实例时原样收下并**不动** `root_scope`（用户配置优先）；假值则置 `None`。`_memory_llm()` 的回退顺序是 `chat_llm` → 第一个带 `llm` 的智能体的模型。
- `create_crew_knowledge` 只在 `knowledge_sources` 全为 `BaseKnowledgeSource` 时建 `Knowledge` 并 `add_sources()`，异常被吞成一条 warning——知识库构建失败不该阻断团队构造。

**`check_config` 是配置面的总闸。** 如果 `config`、`tasks`、`agents` 三样全空，抛 `missing_keys`；有 `config` 就转给 `_setup_from_config`；随后遍历 `agents`，给每个智能体塞团队的 `_cache_handler`（当 `cache=True`）与 `_rpm_controller`（当 `max_rpm` 已设）。这一段解释了为什么团队级配置能覆盖智能体的默认行为——它是在构造期把团队的共享对象直接注入智能体。

`_setup_from_config` 要求 `config` 里同时有 `agents` 与 `tasks` 两个键，缺一个抛 `missing_keys_in_config`；然后 `process` 可被配置覆盖，`agents` 逐个 `Agent(**agent)` 实例化，`tasks` 走 `_create_task`。`_create_task` 的写法值得注意：它用 `next(agt for agt in self.agents if agt.role == task_config["agent"])` 按角色名把任务绑到智能体，然后 `del task_config["agent"]` 再构造——也就是说配置文件里用**角色字符串**而不是对象引用表达归属，这对 YAML 友好，代价是角色名必须唯一且拼写精确（`next` 配生成器，找不到就是 `StopIteration`）。

**`validate_tasks` 只管顺序流程。** 它只在 `process == Process.sequential` 时逐任务检查 `task.agent is not None`，缺了就抛 `missing_agent_in_task` 并把任务描述带进错误信息。层级流程不需要这条：任务可以由管理智能体临时指派，所以任务不绑智能体是合法的。这一处不对称是「流程类型决定约束强度」的最直接证据。

**围绕异步与条件任务的六个校验器，逐条都是边界条件：**

1. `validate_end_with_at_most_one_async_task` 从尾部反向遍历，一遇到同步任务就 `break`，因此它约束的只是**尾部连续异步段**的长度不超过一，中间位置的异步任务不受此限。
2. `validate_must_have_non_conditional_task` 数非条件任务的数量，为 0 就报错；`tasks` 为空时提前返回（空任务是允许的，构造阶段不阻断）。
3. `validate_first_task` 只看第一个元素：首任务不能是 `ConditionalTask`。理由很直接——条件任务的判定输入是「上一个任务的输出」，首任务没有上一个。
4. `validate_async_tasks_not_async` 遍历全部任务，禁止 `ConditionalTask` 带 `async_execution`：条件任务的语义是「同步判、可能跳过」，异步化会让跳过判定与后续任务的上下文读取产生竞态。
5. `validate_async_task_cannot_include_sequential_async_tasks_in_context` 的检查是嵌套循环：某异步任务的 `context` 若引用了另一个异步任务，就从这个任务的位置向前回扫，若两个异步任务之间没有同步任务隔开就抛 `ValueError`。也就是说「异步任务可以读上下文，但上下文里的异步任务必须已被同步屏障刷完」。
6. `validate_context_no_future_tasks` 先用 `id(task)` 建索引表，再检查每个 `context` 元素的下标不大于本任务下标，禁止引用未来任务的输出。注意它用 `id()` 而不是 `==` 做键——`Task` 上 `__hash__ = object.__hash__` 且 Pydantic 的相等语义较宽，用身份比较避免了两个描述相同的任务被误判为同一节点。

**`key` 是把配置压成一个摘要。** 团队 key 是「所有智能体 key + 所有任务 key」拼接后取 md5（`usedforsecurity=False`），用于追踪 baggage 与事件关联。它是**内容指纹而非身份**：改了任一角色描述或任务描述，key 就变。

## 关键取舍

**源码事实：** 语义校验全放在 `model_validator(mode="after")` 而不在 `__init__` 里手写 if，因此所有违规都以 `PydanticCustomError` 的统一形态抛出，调用方可以按类型而非字符串区分错误。

**源码事实：** `id` 的 `_deny_user_set_id` 允许检查点恢复时注入 id，普通构造时禁止。这是「标识可继承、但不可伪造」的折中。

**源码事实：** 顺序流程强制每个任务绑定智能体，层级流程不强制。同一个 `tasks` 列表在两种流程下合法性强弱不同，这是把约束写在流程类型而非任务类型上的结果。

**源码事实：** `create_crew_knowledge` 吞掉异常只记 warning。知识库是可选的增强能力，构造期不该因为向量库不可用而整体失败。

**架构推论：** 把团队配置的合法性做成「一串独立的小校验器」而不是一个巨型 `validate()`，好处是每条规则的失败信息都能精确指向具体字段和具体任务；代价是规则之间的顺序依赖只能靠声明顺序隐式保证，读代码时必须按顺序读。

**架构推论：** `_setup_from_config` 用角色名解析任务归属，把「配置即数据」推到极致，但也意味着角色名事实上成了配置里的主键，重命名角色会静默破坏绑定关系（表现为 `StopIteration`）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/crew.py` | `Crew` | 团队模型本体：声明字段、私有运行时状态、检查点指针 |
| `lib/crewai/src/crewai/crew.py` | `check_config` | 配置总闸：三样全空报错、按 `config` 建智能体与任务、注入共享缓存与 RPM |
| `lib/crewai/src/crewai/crew.py` | `validate_tasks` | 仅顺序流程要求每个任务都绑定智能体 |
| `lib/crewai/src/crewai/crew.py` | `_setup_from_config` | 从 `config` 字典实例化智能体与任务，按角色名做绑定 |
| `lib/crewai/src/crewai/crew.py` | `create_crew_memory` | `memory` 三态处理与 `/crew/<name>` 根作用域 |
| `lib/crewai/src/crewai/crew.py` | `key` | 由智能体 key 与任务 key 拼接的 md5 内容指纹 |

## 参考

- [CrewAI 源码：crew.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/crew.py)
- [CrewAI 文档：Crews 概念](https://docs.crewai.com/en/concepts/crews)
- [CrewAI 文档：Agents 与 Crews 的配置](https://docs.crewai.com/en/concepts/agents)

## 与主核 / AI 的关系

`Crew` 扮演的角色与主核里的「连接配置 + 校验层」同构：主核在连接串与参数表上做的一堆校验（层级依赖、参数互斥、目录存在性），在这里换成了「智能体-任务绑定、异步尾段长度、条件任务位置、上下文不得指向未来」这类执行语义校验。差别在于主核校验的是资源与协议约束，这里校验的是**调度可行性**——它保证执行循环在跑起来之前不会遇到「任务没有执行者」这种结构性空洞。

对 AI 工程的实际意义在于：把这类约束尽量前移到构造期，是把 LLM 的不确定性挡在边界外的做法。模型输出不可控，但「谁执行哪个任务、哪些任务可以并行、条件任务排在哪」这些是确定性的，越早失败越省事。这也解释了为什么同一个 `tasks` 列表在顺序与层级流程下被验成不同的合法性——流程类型是这套约束的唯一开关。
