---
project: dify
feature: node-runtime
title: 节点运行时装配与变量池
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/node_runtime.py
    symbols: [DifyPreparedLLM, DifyRetrieverAttachmentLoader]
  - path: api/core/workflow/system_variables.py
    symbols: [SystemVariableKey, build_system_variables]
  - path: api/core/workflow/file_reference.py
    symbols: [FileReference, build_file_reference]
  - path: api/core/workflow/nodes/datasource/datasource_node.py
    symbols: [DatasourceNode]
---

# 节点运行时装配与变量池

## 一句话职责

`node_runtime.py` 是本仓实现 `graphon` 运行期协议的唯一集中点：把模型实例、文件工厂、检索附件、工具运行时、人工介入仓储这些「dify 的持久化与插件能力」逐个适配成引擎能用的接口；另外两个文件负责变量池的骨架——系统变量键与规范文件引用格式。

## 机制是什么

b01 讲过本仓与引擎的分界线：产品关切留在这边，图语义在那边。这一页讲的就是那条线上的**具体接口**。

引擎为了让节点保持纯净，把每一类外部依赖都声明成一个 `Protocol`：给模型一个 `LLMProtocol`，给文件一个 `FileReferenceFactoryProtocol`，给工具一个 `ToolNodeRuntimeProtocol`。**引擎只认这些协议，本仓负责实现它们。** 于是 `node_runtime.py` 成了一组适配器的集合，每个类都实现一个协议，每个类都把「dify 怎么做」翻译成「引擎要什么」。

这个文件有 1100 行，但它的复杂度不在算法上，而在**它要同时满足两套约束**：引擎的协议形状，与 dify 的计费、审核、多租户、插件守护进程、文件权限。

第二件事是变量池。系统变量是**每个节点都能读的全局上下文**（query、conversation_id、workflow_run_id、文档 ID 等），它有两个非常具体的性质：

其一，**它的键名是接口**——节点、前端、DSL 里都用字符串键引用它，所以键名不可随手改。

其二，**它必须在节点构造前就绪**。带记忆的 LLM 节点在**构造期**就要读 conversation_id，构造完再注入就晚了。于是有了一套「preload」机制：在构造节点之前，按节点类型预判它需要哪些系统变量并提前加载。

## 怎么实现的

**模型适配器：一个注入的特性位。**

`DifyPreparedLLM` 实现 `LLMProtocol`，作用是把完整的 `ModelInstance`（含凭据、配额、审核、模型类型实例）藏起来，只暴露引擎需要的七个成员。

其中 `get_model_schema` 做了一件不寻常的事：**如果模型 schema 的 `features` 里没有 `STRUCTURED_OUTPUT`，就补上**，返回一个 `model_copy`。注释解释了这个「撒谎」的理由：「Dify can supply structured output through a schema prompt without native support. Copy the schema so provider metadata still selects the correct path.」

也就是说，**引擎用 `features` 判断能不能做结构化输出，而 dify 在部分模型上是用「提示里塞 schema」的方式实现同一能力的**。如果不补这一位，引擎会拒绝走结构化输出路径，而 dify 明明能做到。补上之后，两个键的语义差异（原生支持 vs 提示模拟）就被抹平了。

这是整页最值得记住的一处：**当一个能力可以用两种方式实现时，能力位就变成了「谁在实现」的信息载体，而它通常只表达了「是否可用」。**

`DifyPreparedPollingLLM` 紧接着**覆盖了这个注入**，直接返回原始 schema。理由同样写在注释里：「Polling passes the schema directly to the plugin and has no prompt fallback.」——轮询路径把 schema 直接交给插件，没有提示层可以塞。**同一能力在两条路径上的实现方式不同，因此能力位也不能相同。** 这个子类的 `get_model_schema` 是父类行为的例外，读代码时容易漏。

**轮询适配器：配额与审核的时序。**

`DifyPreparedPollingLLM` 的复杂度全在「什么时候扣钱、什么时候审核」这一个问题上。它的三段逻辑值得逐步看：

`start_llm_polling` 的顺序是：先 `finalize_llm_polling()`（收尾上一个任务）、再**审核输入**、再申请配额预留、最后提交任务。**审核在建任务与预留配额之前**——被拒的输入不建任务也不占额度。

`_finalize_polling_result` 处理终态：`RUNNING` 原样返回；`SUCCEEDED` 时**先提交配额、再审核输出**——这个顺序是关键。注释写明了后果：「Rejected successful output is never delivered, but already consumed quota remains committed.」——**输出被审核拒掉，钱照扣**，因为模型确实被调用了。反过来若先审核再提交，被拒时会走释放路径，平台就白跑了模型。

失败时释放配额；异常时无论哪一步出错都用「安全释放」兜底并重新抛出。

`finalize_llm_polling` 自己把预留置空再释放，所以它是幂等的——这句很重要，因为它在四五个地方被调用：正常收尾、启动前的清理、以及两条异常路径。

`finalize_llm_polling` 还出现在 `DifyLLMNode` 的 `finally` 里（见 b03）：那个 47 行的子类存在的唯一目的就是保证这个方法一定被调用。**两页在这里接上了**——b03 讲节点侧为什么需要它，这里讲它实现了什么。

**工具运行时：把工作流状态藏进不透明句柄。**

`DifyToolNodeRuntime` 实现 `ToolNodeRuntimeProtocol`。引擎只要求 `get_runtime` 返回一个 `ToolRuntimeHandle`，并且把它当**不透明令牌**传回来。dify 就利用这个「不透明」：句柄的 `raw` 字段里装的不是工具本身，而是一个 `_WorkflowToolRuntimeBinding`，额外携带三样引擎不需要但 dify 需要的东西——conversation_id、父级追踪上下文、追踪会话 ID。

这个手法的收益是**不必让引擎知道工作流语义**，代价是**类型不安全**：`raw` 是 `Any`，所以读取时写了一个兜底：若不是预期类型，就把 `raw` 当作工具本身包一层。这条兜底意味着「有人可能会塞别的进来」，而代码选择容忍而不是报错。

`get_runtime` 里还有一段只在「工作流即工具」时执行：从系统变量取外层运行 ID，与节点执行 ID 组成 `ParentTraceContext`。**这是跨工作流调用的追踪链**——被调工作流的节点要知道自己是哪次外层调用的产物。

`invoke` 里有三次 `hasattr` 检查（`set_parent_trace_context` / `clear_parent_trace_context` / `set_trace_session_id`），因为不同工具类型支持不同的追踪能力。**用 `hasattr` 而不是接口，说明追踪能力没有统一抽象**，这是为兼容已有的多种工具类型而做的让步。

`get_usage` 有一个宽松取值：`latest_usage` 可能是 `LLMUsage` 也可能是 dict，两种都接受。工具侧的用量记录没有统一类型。

**文件与附件：权限的两级检查。**

`DifyRetrieverAttachmentLoader` 的 `load` 开头是两道检查：先 `is_retriever_segment_access_granted(segment_id)`（上下文级的访问授权，由外层授予），再是可选的外部检查器。**任一不通过就返回空列表，不报错**——「无权读」与「没有附件」在返回值上不可区分。

通过后查 `SegmentAttachmentBinding` 与 `UploadFile` 的联表，然后**先批量授予上传文件访问权**（`grant_upload_file_access`），再逐个构造文件引用。

构造时硬编码了 `type: FileType.IMAGE`。这是一个明显的窄化：**附件被一律当作图片**。如果附件实际是 PDF 或音频，类型就是错的，而下游可能据此选择处理方式。

**系统变量：键是接口，值要归一。**

`SystemVariableKey` 是 `StrEnum`，十六个键。这里有一个与 b07 同类的陷阱：成员名 `WORKFLOW_EXECUTION_ID` 对应的**值是 `"workflow_run_id"`**。所以代码里引用它时用成员名，而变量池里的键、DSL 里的引用都是 `workflow_run_id`。两者不一致，且没有任何编译期检查。

`_normalize_system_variable_values` 做三件归一：

- 把 `workflow_execution_id` 这个**别名字段**折成 `workflow_run_id`（只在新键不存在时）；
- **丢弃所有值为 `None` 的项**——所以系统变量表里没有「值为空」的键，只有「键不存在」；
- 强制补一个 `files: []`。

最后那条让 `files` 成为**唯一必定存在的系统变量**，即使没有任何文件。这解释了为什么节点里可以放心地对 `files` 取值而不判空。

`build_system_variables` 把归一后的字典转成变量对象列表，每个都带 `(系统变量节点 ID, 键名)` 的 selector。`default_system_variables()` 则用一个新的 uuid 当 `workflow_run_id`——**用于那些没有真实运行的场合**（例如单节点调试、裸节点试跑）。

`get_system_text` 的实现值得看一眼：它先取段，再用 `getattr(segment, "text", None)` 并检查是不是字符串。**不是字符串就返回 `None`，而不是 `str()` 强转**。所以一个数字类型的系统变量用 `get_system_text` 取会得到 `None`——这是刻意的类型严格，防止把 `123` 悄悄变成 `"123"` 后被当作 ID 用。

**preload：构造期的时序问题。**

`get_node_creation_preload_selectors` 只对三种节点返回 selector：LLM、问题分类器、参数提取器——且**必须带 `memory` 配置**。返回的唯一 selectors 是 `conversation_id`。

理由不难推：带记忆的节点在构造期就要拿会话 ID 去初始化记忆。其它节点不需要构造期变量，所以不 preload。

`preload_node_creation_variables` 做四件事：校验 selector 长度至少 2（否则抛错）、去重、**跳过变量池里已存在的**、然后批量加载并写回池。跳过已存在这一步避免了重复加载，也让「调用方已经填好」的情形不必再查一次。

它有一个小陷阱：写回池时用的是 `loaded_selector[:2]`，即**只取前两段**。而校验时允许更长的 selector。所以一个三段 selector 会被加载但只按前段写回——嵌套路径的信息在这里被截断。

**变量池的隐式补全。**

`inject_default_system_variable_mappings` 只对「LLM 节点且带 memory」生效，补一个 `节点 ID.#sys.query#` 到 `#sys.query#` 的映射。

注释说得很清楚：「Add workflow-owned implicit sys mappings that graphon should not know about.」——**这条映射是 dify 的约定，引擎不该知道**，所以由 dify 在变量加载前补进去。这与 b01 的「产品关切在入口层」是同一条原则的微观版本。

**规范文件引用：严格与宽松的显式分裂。**

`file_reference.py` 的模块 docstring 是全库少见的好文档，它把设计意图讲透了：规范格式是 `dify-file-ref:<base64url-json>`，内部含 `record_id`，可选 `storage_key`。然后明确声明**同时提供严格与宽松两套辅助函数**：`is_canonical_file_reference` 是严格校验器，`parse_file_reference` 与 `resolve_file_record_id` 保持宽松以便读历史数据。最后一句是硬约束：「Callers enforcing canonical-only behavior must not use the permissive helpers as validators.」

`parse_file_reference` 的宽松体现在三种兜底，一律「把整个输入当成 record_id」：不是前缀开头、base64+JSON 解析失败、payload 不是对象或 `record_id` 不合法。

`is_canonical_file_reference` 的实现很巧：`reference.startswith(prefix) and parsed.record_id != reference`。**用「解析出的 record_id 与原串不同」来证明解析真的成功了**——因为所有兜底路径都返回原串，所以不等就意味着走了正常路径。一个判等就替代了「再解析一次并检查错误」。

这个技巧有效但脆弱：它依赖「兜底必定返回原串」这一实现细节。若将来有人加一条兜底返回别的东西，这个校验就静默失效。

**`DatasourceNode`：运行时装配的消费者。**

放在这一页是因为它集中用到了上述能力：系统变量（`DATASOURCE_TYPE`、`DATASOURCE_INFO`）、文件引用解析、以及注入的凭据获取函数。

它的 `_run` 是四个分支的 `match`：在线文档与在线网盘都转给 `DatasourceManager.stream_node_events`（流式产出事件）；网站抓取只回显信息；本地文件解析引用后取上传文件、**同时写进变量池并放进输出**。

注意本地文件那一支做了两件事：`variable_pool.add([self._node_id, "file"], file_info)` 与 `outputs={"file": file_info, ...}`。**它既写变量池又写输出**，因为下游既可能用 selector 也可能用输出变量名引用它。

还有一处副作用值得记：`datasource_info["icon"] = ...` 就地修改了从系统变量取出的字典，然后再把它当 `parameters_for_log`。所以**日志里的参数是被加工过的**，不是原始注入值。这与 b07 里 `_webhook_raw` 的处境完全一样。

异常处理只捕获两类（插件守护进程错误与自己的错误类型），其余上抛给引擎。

## 关键取舍

**源码事实：** `DifyPreparedLLM.get_model_schema` 在缺少 `STRUCTURED_OUTPUT` 时补上该特性位，注释说明 dify 能用 schema 提示模拟该能力；`DifyPreparedPollingLLM` 覆盖此方法**不做**注入，因为轮询路径把 schema 直接交给插件、没有提示回退。

**源码事实：** 轮询适配器的顺序是「审核输入 → 预留配额 → 建任务」，终态时「提交配额 → 审核输出」。输出被拒时配额**不退还**，注释明确写下这个后果。

**源码事实：** `finalize_llm_polling` 先置空再释放，可重复调用；在启动、正常收尾、两条异常路径上都出现。

**源码事实：** `ToolRuntimeHandle.raw` 里装的是 `_WorkflowToolRuntimeBinding`（工具 + 会话 ID + 父追踪上下文 + 追踪会话 ID），引擎把它当不透明令牌；读取时有「当作裸工具」的兜底分支。

**源码事实：** 工作流追踪上下文只在「工作流即工具」的 provider 上构造，由外层运行 ID 与节点执行 ID 组成；追踪能力通过三次 `hasattr` 探测而非统一接口。

**源码事实：** `DifyRetrieverAttachmentLoader.load` 有两级权限检查，任一失败返回空列表；构造出的文件类型硬编码为 `FileType.IMAGE`；构造前先批量授予上传文件访问权。

**源码事实：** `SystemVariableKey.WORKFLOW_EXECUTION_ID` 的值是 `"workflow_run_id"`；归一函数会折叠 `workflow_execution_id` 别名、丢弃 `None` 值、并强制补 `files: []`。

**源码事实：** `get_system_text` 只在段的 `text` 是字符串时返回，否则 `None`，不做强制转换。

**源码事实：** 构造期预加载只对 LLM、问题分类器、参数提取器且带 memory 的节点生效，只预加载 `conversation_id`；写回池时截取 selector 前两段。

**源码事实：** `is_canonical_file_reference` 用「解析出的 record_id 与原串不等」来判定解析成功，依赖所有兜底路径都返回原串。

**源码事实：** `DatasourceNode` 就地给 `datasource_info` 补 `icon` 后再当作输入日志；本地文件分支既写变量池又写输出；只捕获两类异常。

**架构推论：** 「用 `Protocol` 逐项适配」与「把适配器集中在一个文件」是配套的两个决定。前者让节点不依赖基础设施，后者让**所有跨边界的东西都在一处可见**。1100 行确实长，但它换来的是「想知道 dify 与引擎之间有什么，读这一个文件就够」——**边界的价值在于可枚举**。

**架构推论：** 「能力位注入」是一个必须显式记录的技术债。它让引擎的判定与 dify 的实际能力对齐，但也意味着**任何依赖 `features` 做优化的引擎逻辑都可能在 dify 这里得到乐观结论**。轮询路径不注入，正因为那条路径没有回退手段——**同一个能力位在两条路径上语义不同，这本身就是设计裂缝**。

**架构推论：** 「先审核输入、再申请配额」与「先提交配额、再审核输出」这一对顺序，反映的是**成本归属的判定**：输入被拒时模型未被调用，成本为零；输出被拒时模型已被调用，成本已发生。这类顺序不是随意排的，它编码了「谁承担失败的代价」。

**架构推论：** 「严格与宽松成对提供，并在文档里划清用途」是一个正面的范例。危险不在于宽松函数存在，而在于宽松函数被误当校验器——所以模块 docstring 直接禁止这种用法。**能容忍历史数据与能校验新数据，是两件必须分开命名的事。**

**架构推论：** `SystemVariableKey` 的成员名与值不一致（`WORKFLOW_EXECUTION_ID` → `workflow_run_id`）与 b07 的 `SyncMode.SYNC = "async"` 是同一类问题。`StrEnum` 的设计本意是让成员名与值保持一致以免歧义，一旦两者不同，**读代码的人与读数据的人会得出相反理解**，而且没有任何机制会提示。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/node_runtime.py` | `DifyPreparedLLM` | 模型适配：隐藏实例细节、注入结构化输出能力位 |
| `api/core/workflow/node_runtime.py` | `DifyPreparedPollingLLM` | 轮询适配：配额预留/提交/释放与输入输出审核时序 |
| `api/core/workflow/node_runtime.py` | `DifyPreparedPollingLLM._finalize_polling_result` | 终态结算：先提交配额再审核输出 |
| `api/core/workflow/node_runtime.py` | `DifyFileReferenceFactory` | 文件工厂适配，注入租户与访问控制 |
| `api/core/workflow/node_runtime.py` | `DifyRetrieverAttachmentLoader` | 检索附件加载，两级权限检查 |
| `api/core/workflow/node_runtime.py` | `DifyToolNodeRuntime` | 工具运行时适配，句柄内藏工作流状态 |
| `api/core/workflow/node_runtime.py` | `DifyToolNodeRuntime.get_runtime` | 解析工具并构造追踪上下文 |
| `api/core/workflow/node_runtime.py` | `DifyHumanInputNodeRuntime` | 人工介入运行时，按调用来源裁剪投递方式 |
| `api/core/workflow/system_variables.py` | `SystemVariableKey` | 十六个系统变量键，成员名与值存在不一致 |
| `api/core/workflow/system_variables.py` | `build_system_variables` | 归一后转成带 selector 的变量对象 |
| `api/core/workflow/system_variables.py` | `default_system_variables` | 无真实运行时生成默认系统变量 |
| `api/core/workflow/system_variables.py` | `get_system_text` | 严格取文本，非字符串返回 `None` |
| `api/core/workflow/system_variables.py` | `get_node_creation_preload_selectors` | 构造期预加载的节点类型白名单 |
| `api/core/workflow/system_variables.py` | `preload_node_creation_variables` | 去重、跳过已存在、截断写回 |
| `api/core/workflow/system_variables.py` | `inject_default_system_variable_mappings` | 补 memory LLM 的隐式 query 映射 |
| `api/core/workflow/file_reference.py` | `FileReference` | 文件引用的冻结值对象 |
| `api/core/workflow/file_reference.py` | `build_file_reference` | 生成 `dify-file-ref:` 前缀的规范串 |
| `api/core/workflow/file_reference.py` | `parse_file_reference` | 宽松解析，三种兜底都回落到原串 |
| `api/core/workflow/file_reference.py` | `is_canonical_file_reference` | 严格校验，靠「解析值不等于原串」判成功 |
| `api/core/workflow/nodes/datasource/datasource_node.py` | `DatasourceNode` | 数据源节点，四类 provider 分派 |
| `api/core/workflow/nodes/datasource/datasource_node.py` | `DatasourceNode._run` | 系统变量取值、本地文件写池与输出 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [graphon：dify 外置的图执行内核](https://github.com/langgenius/graphon)
- [dify 源码：core/workflow/node_runtime.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/node_runtime.py)
- [dify 文档：工作流变量与系统变量](https://docs.dify.ai/guides/workflow/variables)

## 与主核 / AI 的关系

**「适配层集中在一个文件」与主核的 VFD/存储管理器分层同构。** 数据库不会让每个访问方法各自处理文件描述符、缓冲、页锁，这些都在存储管理器里统一做；dify 不让每个节点各自处理租户、配额、文件权限，这些都在运行时层统一做。**判据是「这个关切会不会被所有上层都遇到」**——会，就必须收拢，否则它会散落成 N 份不一致的实现。

**「能力位被注入」与主核的类型系统宽松化类似。** 数据库允许给自定义类型声明「支持排序」「支持哈希」这类标志，而这些标志可能由用户实现而非原生支持。一旦某个能力是「模拟实现」的，依赖该标志做优化的路径就会得到不同代价。**声明能力与真实能力之间的缝隙，是所有可扩展系统的固有风险面。**

**「审核输入先于建任务、审核输出后于扣费」与主核的两阶段提交同构。** 2PC 的 `PREPARE` 阶段先确认所有参与者可提交，`COMMIT` 才落定。dify 的顺序同理：不可逆的动作（扣费、提交）必须放在所有可能否决的检查之后。**顺序本身就是协议的一部分，改动顺序等于改动语义。**

**「严格与宽松成对」与主核的 `regproc` 兼容读取同类。** 数据库读系统目录时对历史格式保持宽容，但新写入必须符合当前格式。两者都把「读宽松、写严格」写成显式规则，而不是让宽容悄悄渗透到校验路径。**写路径的严格性是数据质量唯一可控的地方。**

对 agent 工程最实用的五条。其一：**结构化输出能力位是被注入的**，不要以为模型原生支持；走轮询路径时该位不注入，行为可能不同。其二：**审核拒绝的输出照样扣配额**，成本模型要按「模型被调用」而不是「结果被采用」估算。其三：**附件加载会做两级权限检查且失败静默返回空**，排查「附件没出来」要先看授权。其四：**系统变量的成员名与值可能不一致**（`WORKFLOW_EXECUTION_ID` 对应 `workflow_run_id`），写 DSL 与读数据库时都要按值来。其五：**权限与文件类型都是硬编码的**（附件一律当图片），不要依赖它做类型分派。
