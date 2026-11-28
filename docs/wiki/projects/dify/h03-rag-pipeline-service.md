---
project: dify
feature: rag-pipeline-service
title: RAG 流水线服务
page_type: feature
schema_version: 1
refs:
  - path: api/services/rag_pipeline/rag_pipeline.py
    symbols: [RagPipelineService]
  - path: api/services/rag_pipeline/rag_pipeline_dsl_service.py
    symbols: [RagPipelineDslService, CURRENT_DSL_VERSION]
---

# RAG 流水线服务

## 一句话职责

`RagPipelineService` 把「一条 RAG 流水线」当作**可编辑的工作流应用**来管理——草稿与已发布版本、单节点调试、数据源、模板；`RagPipelineDslService` 负责它的**序列化边界**：把 YAML 导入成一个 `Pipeline` + `Dataset` + `Workflow` 三件套，或反向导出，并在导入时处理版本兼容、依赖检查与 ID 加密。

## 机制是什么

RAG 流水线与普通工作流应用的关键差别是：**它自带一个数据集**。

一条流水线里有「知识索引节点」，那个节点声明了分块结构、索引技术、嵌入模型、检索参数——而这些参数最终要落成一个 `Dataset` 行（因为摄取流水线、向量库集合、权限都挂在 `Dataset` 上）。于是「流水线」与「数据集」必须**成对创建、成对更新、成对删除**，而它们分属两张表。

这带来两个贯穿全页的约束：

**第一，草稿态是唯一可编辑的地方，而已发布态是快照。** `Workflow` 表用 `version` 字段区分：`"draft"` 是一个固定值，已发布版本则是**时间戳字符串**：

```python
version=str(datetime.now(UTC).replace(tzinfo=None))
```

**「版本号就是发布时间」**——这让「列出历史版本」天然有序，且不需要额外的自增序号。代价是**同一秒内两次发布得到同一个版本号**，而代码没有防这一点（没有唯一约束的迹象）。

**第二，DSL 是跨租户共享的交换格式，所以 ID 不能裸传。** 导出的 YAML 里若含 `dataset_ids`，那是本租户的库 id；别的租户导入时会指向不存在的库。所以导出时加密、导入时解密：

```python
def _generate_aes_key(self, tenant_id: str) -> bytes:
    """Generate AES key based on tenant_id"""
    return hashlib.sha256(tenant_id.encode()).digest()

def encrypt_dataset_id(self, dataset_id: str, tenant_id: str) -> str:
    """Encrypt dataset_id using AES-CBC mode"""
    key = self._generate_aes_key(tenant_id)
    iv = key[:16]
    cipher = AES.new(key, AES.MODE_CBC, iv)
    ct_bytes = cipher.encrypt(pad(dataset_id.encode(), AES.block_size))
    return base64.b64encode(ct_bytes).decode()
```

**这是「用租户 id 派生密钥」的加密，而 IV 就是密钥的前 16 字节** ——所以它**既不是保密性保护也不是完整性保护，只是「让 id 在别的租户里解不开」**。这个定位很重要：它的目的不是防攻击者（租户 id 不是秘密），而是防「导入到别的租户时静默指向错误资源」。

## 怎么实现的

**`RagPipelineService` 的构造器把两个仓库注入进来，而它自己只持一个 session。**

```python
def __init__(self, session: Session, session_maker: sessionmaker | None = None):
    """Initialize RagPipelineService with repository dependencies."""
    self._session = session
    if session_maker is None:
        session_maker = sessionmaker(bind=db.engine, expire_on_commit=False)
    self._node_execution_service_repo = DifyAPIRepositoryFactory.create_api_workflow_node_execution_repository(
        session_maker
    )
    self._workflow_run_repo = DifyAPIRepositoryFactory.create_api_workflow_run_repository(session_maker)
```

**四处：**

1. **`session_maker` 可注入但默认自建** ——测试可传假的，生产不传。
2. **`expire_on_commit=False`** ——提交后对象仍可读属性。这一层到处在 `commit()` 之后还用 `workflow.id`、`pipeline.id`，若默认 `True` 就会在提交后触发一次隐式刷新（或报 detached 错）。
3. **仓库由工厂创建而不是直接 new** ——见 h05（仓储页）里 `DifyAPIRepositoryFactory` 的作用。
4. **它同时持有「一个 session」和「一个 session_maker」** ——前者给业务读写，后者给需要独立事务的路径（仓库、`DraftVariableSaver`）。**两者并存是这一层最容易出错的地方**：同一个操作里，部分写入走 `self._session`、部分走新 session，事务边界不统一。

**`sync_draft_workflow` 用 `unique_hash` 做乐观并发控制。**

```python
workflow = self.get_draft_workflow(pipeline=pipeline)

if workflow and workflow.unique_hash != unique_hash:
    raise WorkflowHashNotEqualError()

if not workflow:
    workflow = Workflow(
        tenant_id=pipeline.tenant_id,
        app_id=pipeline.id,
        _features="{}",
        type=WorkflowType.RAG_PIPELINE,
        version="draft",
        graph=json.dumps(graph),
        created_by=account.id,
        _environment_variables=environment_variables,
        _conversation_variables=conversation_variables,
        _rag_pipeline_variables=rag_pipeline_variables,
    )
    self._session.add(workflow)
    self._session.flush()
    pipeline.workflow_id = workflow.id
else:
    workflow.graph = json.dumps(graph)
    workflow.updated_by = account.id
    workflow.updated_at = datetime.now(UTC).replace(tzinfo=None)
    ...
self._session.commit()
```

**四处：**

1. **`unique_hash` 是客户端带上来的「我以为的当前版本」**，与库里的不一致就拒绝。这是**画布型产品的必需品**：用户在浏览器里编辑了十分钟，期间别人也改过，直接保存会覆盖。报错让前端提示「有人改过，请刷新」。
2. **`if not workflow` 时 `pipeline.workflow_id = workflow.id`** ——但赋值用的是 `flush()` 之后拿到的 id，而**这行赋值没有显式 `session.add(pipeline)`**。它依赖「`pipeline` 是同一个 session 里的持久对象」这个前提。若传进来的 `pipeline` 来自别的 session，这行会静默失效。
3. **`_features="{}"` 与 `_environment_variables=...` 用下划线前缀的构造参数** ——说明这些是 SQLAlchemy 的「私有映射属性」（`_xxx` 映射到列 `xxx`，同时提供带校验的 property）。这在这套代码里是一种一致的惯用法。
4. **末尾有一行被注释掉的事件派发**：

```python
# trigger  workflow events TODO
# app_draft_workflow_was_synced.send(pipeline, synced_draft_workflow=workflow)
```

**「TODO + 注释掉的事件」说明「草稿同步应当发事件」这件事被识别了但没做。** 后果是：任何依赖「草稿变更」通知的功能（比如草稿自动保存的统计、协作冲突提示）都没有数据源。

**`publish_workflow` 是「快照 + 反向依赖 + 副作用写回」。**

```python
validate_llm_environment_model_references(
    graph=draft_workflow.graph_dict,
    environment_variables=draft_workflow.environment_variables,
)

workflow = Workflow.new(
    tenant_id=pipeline.tenant_id,
    app_id=pipeline.id,
    type=draft_workflow.type,
    version=str(datetime.now(UTC).replace(tzinfo=None)),
    graph=draft_workflow.graph,
    ...
)
session.add(workflow)

graph = workflow.graph_dict
nodes = graph.get("nodes", [])
# dataset_service imports RagPipelineService, so defer this reverse dependency.
from services.knowledge.dataset_service import DatasetService

for node in nodes:
    if node.get("data", {}).get("type") == "knowledge-index":
        knowledge_configuration = node.get("data", {})
        knowledge_configuration = KnowledgeConfiguration.model_validate(knowledge_configuration)

        # update dataset
        dataset = get_pipeline_dataset(pipeline, session=session)
        if not dataset:
            raise ValueError("Dataset not found")
        DatasetService.update_rag_pipeline_dataset_settings(...)
```

**五处：**

1. **发布前先校验 LLM 环境变量引用** ——因为环境变量的解析发生在运行时，若引用悬空，发布出去的流水线会在第一次执行时失败。**在发布点校验而不是在保存点校验，是一个「延迟到最后一个安全点」的选择**：草稿允许引用未定义变量（用户还在编辑），发布不允许。
2. **`version` 用时间戳字符串**（前面已讲）。
3. **那行注释说明了循环依赖的处理方式**：

```python
# dataset_service imports RagPipelineService, so defer this reverse dependency.
from services.knowledge.dataset_service import DatasetService
```

**`dataset_service` 顶部 import 了 `RagPipelineService`，所以这里必须在函数内 import 反向依赖** ——这是 Python 里处理循环依赖的标准手法，而注释把它写清了。**代价是 import 错误从「启动时」推迟到「发布时」**：如果 `dataset_service` 里有语法错误，只有发布流水线才会炸。这与 b02 讲过的「自动发现把错误推迟到运行期」是同一类问题。

4. **`Workflow.new(...)` 是类方法而非构造器** ——大概率负责把不带下划线的参数转成私有映射属性。
5. **发布时同步数据集设置**，且传入 `has_published=pipeline.is_published` ——**注意这个值是在「已经 add 了新版本但还没 commit」的时刻读的**，所以 `is_published` 反映的是**发布前**的状态。这个时序是刻意的：`update_rag_pipeline_dataset_settings` 需要知道「这是首次发布还是再次发布」，因为两者的校验规则不同（首次发布允许改分块结构，再次发布不允许）。**但它的正确性依赖「`pipeline.is_published` 在这行之前没有被更新」** ——而代码里确实没有更新它。这是一处隐式的时序契约。

**`restore_published_workflow_to_draft` 的 docstring 把「共性抽走、个性留下」说得很清楚。**

```python
"""Restore a published pipeline workflow snapshot into the draft workflow.

Pipelines reuse the shared draft-restore field copy helper, but still own
the pipeline-specific flush/link step that wires a newly created draft
back onto ``pipeline.workflow_id``. The source version remains locked
through snapshot-field copy and commit.
"""
```

三句各自对应一件事：

1. **字段复制走共享 helper**（`apply_published_workflow_snapshot_to_draft`）——与工作流应用共用；
2. **「flush + 把新草稿挂回 `pipeline.workflow_id`」是流水线独有的** ——因为流水线的草稿与数据集绑定，多一步关联；
3. **「源版本在复制与提交期间保持锁定」** ——这是并发安全声明：恢复期间别一个发布不能把源版本换掉。

而实现里确实用 `is_new_draft` 分流：

```python
draft_workflow, is_new_draft = apply_published_workflow_snapshot_to_draft(...)

if is_new_draft:
    self._session.add(draft_workflow)
    self._session.flush()
    pipeline.workflow_id = draft_workflow.id

self._session.commit()
```

**共享 helper 返回一个 `(对象, 是否新建)` 的二元组，把「要不要 add」的决定权留给调用方** ——这是一个好形状：helper 不持 session，只做纯字段搬运。

**`run_draft_workflow_node` 是「单节点调试」，而它的事务边界最碎。**

```python
start_at = time.perf_counter()
node_config = draft_workflow.get_node_config_by_id(node_id)

eclosing_node_type_and_id = draft_workflow.get_enclosing_node_type_and_id(node_config)
if eclosing_node_type_and_id:
    _, enclosing_node_id = eclosing_node_type_and_id
else:
    enclosing_node_id = None

workflow_node_execution = self._handle_node_run_result(
    getter=lambda: WorkflowEntry.single_step_run(
        execution_driver=WorkflowRunAgg.run,
        workflow=draft_workflow,
        node_id=node_id,
        user_inputs=user_inputs,
        user_id=account.id,
        variable_pool=_build_seeded_variable_pool(
            build_bootstrap_variables(
                system_variables=default_system_variables(),
                environment_variables=draft_workflow.environment_variables,
            )
        ),
        variable_loader=DraftVarLoader(
            engine=db.engine,
            app_id=pipeline.id,
            tenant_id=pipeline.tenant_id,
            user_id=account.id,
        ),
    ),
    start_at=start_at,
    tenant_id=pipeline.tenant_id,
    node_id=node_id,
)
```

**五处：**

1. **`getter=lambda: ...`** ——把执行包成 thunk 传给 `_handle_node_run_result`，后者负责计时与异常包装。**「传 thunk 而不是传结果」让耗时统计能覆盖执行本身。**
2. **`enclosing_node_id` 是为「迭代/循环体里的节点」准备的** ——单步调试跑一个循环体内的节点时，变量要存回那个循环的迭代上下文，而不是顶层。这解释了后面 `DraftVariableSaver(enclosing_node_id=...)`。
3. **`variable_pool` 由「系统变量 + 环境变量」种子化** ——单步调试没有上游节点输出，所以只种这两类。**这意味着「调试一个依赖上游输出的节点」必须由用户手工填 `user_inputs`**，否则变量缺失。
4. **`DraftVarLoader` 独立于 session** ——它收 `db.engine`，自己建连接。因为变量可能在执行过程中被异步写入。
5. **`time.perf_counter()` 而不是 `time.time()`** ——与 h02 的 `monotonic()` 同理，用于测耗时。

而后续的三段写入把事务边界暴露得很清楚：

```python
repository = DifyCoreRepositoryFactory.create_workflow_node_execution_repository(
    session_factory=db.engine, ...
)
repository.save(workflow_node_execution)
workflow_node_execution_db_model = self._node_execution_service_repo.get_execution_by_id(workflow_node_execution.id)

with sessionmaker(bind=db.engine).begin() as session:
    draft_var_saver = DraftVariableSaver(session=session, ...)
    draft_var_saver.save(process_data=..., outputs=...)

if isinstance(workflow_node_execution_db_model, WorkflowNodeExecutionModel):
    enqueue_draft_node_execution_trace(...)
```

**三个独立事务**：保存执行记录、保存草稿变量、入队 trace。**它们各自 `commit`（或由 `repository.save` 内部 commit）** ——没有共同事务。

**这是一个刻意的取舍**：调试是「尽力记录」的场景，一次写入失败不该让整个调试失败。代价是可能出现「执行记录有、变量没存」的中间状态——而这时下一次调试会因变量缺失而行为不同。

`isinstance` 那一层判断也值得一提：**trace 只在拿到 ORM 模型时才入队**，因为 trace 需要模型的字段（不是 service 层的 dataclass）。

**`run_datasource_workflow_node` 返回一个 `Generator`，而它的参数校验用一个 `match` 表达式。**

```python
datasource_parameters = datasource_node_data.get("datasource_parameters", {})
for key, value in datasource_parameters.items():
    param_value = value.get("value")

    match param_value:
        case None | "" | [] | {}:
            variables_map[key] = param_value
```

**「空值用四个字面量一起匹配」这个写法在 Python 里是必需的** ——因为 `match` 用的是**相等比较**而不是真值判断，于是「空串、空列表、空字典、None」四种「空」必须逐项列出。这行代码同时说明了一件事：**这里要识别的是「空」，而不是「假」**（`0` 与 `False` 不该被当成空）。

**`RagPipelineDslService` 的 docstring 是一条清晰的契约声明。**

```python
class RagPipelineDslService:
    """Import, export, and inspect RAG pipeline DSL using the caller-owned session.

    Callers pass a plain ``Session`` (not wrapped in ``.begin()``) and are responsible for calling
    ``session.commit()`` on success or ``session.rollback()`` on failure.  Methods here only flush
    when generated IDs are needed mid-operation; they never commit or rollback.
    """
```

**「传 plain Session、调用方负责 commit/rollback、内部只 flush」** ——这是一个**明确的事务所有权声明**，而它与 `RagPipelineService` 的行为正好相反（后者到处 `self._session.commit()`）。

**同一个功能域里两个服务的事务策略不同**，这不是疏漏而是分层的必然结果：`RagPipelineService` 是「面向控制器」的门面（自己收尾事务更省事），`RagPipelineDslService` 是「可被别的服务复用的组件」（不能替调用方决定何时提交）。**但代价是使用者必须记住哪个是哪个。**

**`import_rag_pipeline` 的返回类型是「结果对象而不是异常」，而这一点贯穿全函数。**

```python
if not yaml_url:
    return RagPipelineImportInfo(
        id=import_id, status=ImportStatus.FAILED,
        error="yaml_url is required when import_mode is yaml-url",
    )
...
if dsl_content_size(raw_content) > DSL_MAX_SIZE:
    return RagPipelineImportInfo(
        id=import_id, status=ImportStatus.FAILED,
        error="File size exceeds the limit of 10MB",
    )
```

**所有可预期的失败都返回 `status=FAILED` 的结果对象，只有 `import_mode` 非法才 `raise ValueError`。**

**这个分界线值得推敲**：

- **「请求参数不合法」（`import_mode` 拼错）→ 抛异常** ——因为那是调用方的编程错误；
- **「请求内容不合法」（URL 拿不到、内容太大、YAML 格式错）→ 返回失败对象** ——因为那是用户输入的问题，需要以「导入失败 + 原因」呈现给用户。

**这是「错误分类」的一个清晰实例：程序员错误抛异常，用户错误返回状态。** 而它的可观测后果是：前者产生 500，后者产生 200 + 失败状态。

**版本兼容用「主版本不匹配就挂起等确认」来表达。**

```python
if not data.get("version"):
    data["version"] = "0.1.0"
if not data.get("kind") or data.get("kind") != "rag_pipeline":
    data["kind"] = "rag_pipeline"

imported_version = data.get("version", "0.1.0")
if not isinstance(imported_version, str):
    raise ValueError(f"Invalid version type, expected str, got {type(imported_version)}")
status = check_version_compatibility(imported_version, CURRENT_DSL_VERSION)
```

**四处：**

1. **缺失 `version` / `kind` 时直接补上，而不是报错** ——宽度优先，让「手写的最小 YAML」也能导入。
2. **`kind` 不匹配时也直接覆盖**（`!= "rag_pipeline"` → 设为 `rag_pipeline`）。这与「严格校验」相反：**把一个 `kind: app` 的文件当流水线导入会被静默改写**，而不是拒绝。
3. **版本类型必须真是 `str`**，否则 `raise` ——注意这里用的是「抛异常」，与上面的「返回失败对象」不一致。**同一函数里两种错误风格并存。**
4. **`CURRENT_DSL_VERSION = "0.1.0"`，与代码里硬编码的 `"0.1.0"` 字面量重复出现三次** ——`data["version"] = "0.1.0"`、`data.get("version", "0.1.0")`。**这些地方没有引用常量**，所以「升级 DSL 版本」时要改四处而容易漏。

**主版本不匹配时的挂起机制是「把待确认数据放 Redis，10 分钟过期」。**

```python
if status == ImportStatus.PENDING:
    tenant_id = account.current_tenant_id
    if tenant_id is None:
        raise ValueError("Current tenant is not set")
    pending_data = RagPipelinePendingData(
        tenant_id=tenant_id,
        account_id=account.id,
        import_mode=import_mode,
        yaml_content=content,
        pipeline_id=pipeline_id,
    )
    redis_client.setex(
        f"{IMPORT_INFO_REDIS_KEY_PREFIX}{import_id}",
        IMPORT_INFO_REDIS_EXPIRY,
        pending_data.model_dump_json(),
    )

    return RagPipelineImportInfo(
        id=import_id, status=status, pipeline_id=pipeline_id,
        imported_dsl_version=imported_version,
    )
```

**五处：**

1. **整个 YAML 原文被存进 Redis**（不是引用、不是路径）——所以确认时不需要重新下载。代价是**大 YAML 会占 Redis 内存**，而 10MB 上限意味着单条最多 10MB。
2. **`RagPipelinePendingData` 继承 `PendingImportOwner`**，后者提供 `is_accessible_by`：

```python
pending_data = RagPipelinePendingData.model_validate_json(pending_data)
if not pending_data.is_accessible_by(
    tenant_id=account.current_tenant_id,
    account_id=account.id,
):
    return RagPipelineImportInfo(
        id=import_id, status=ImportStatus.FAILED,
        error="Import information expired or does not exist",
    )
```

**「过期」与「不属于你」返回同一个错误消息** ——这是一个安全考虑：**不泄漏「这个 import_id 存在但你无权」**。这类「把两种失败合并成一条消息」的做法，判据是「泄漏存在性是否有害」。

3. **`isinstance(pending_data, str | bytes)` 的判断** ——Python 3.10 起 `isinstance` 接受联合类型，这行等价于「不是 str 也不是 bytes」，用于防御 Redis 客户端返回意外类型。
4. **`IMPORT_INFO_REDIS_EXPIRY = 10 * 60`** ——10 分钟。这是一个产品判断：用户看到「版本不兼容，是否继续？」后要在 10 分钟内点确认。
5. **`confirm_import` 的 `dataset_created` 字段带 `Field(default=False, exclude=True)`**：

```python
# Internal creation outcome for post-commit authorization; never part of the DSL response.
dataset_created: bool = Field(default=False, exclude=True)
```

**「内部字段用 `exclude=True` 排除出序列化」** ——注释说明了理由：「for post-commit authorization」。即控制器需要在提交后决定「要不要给新建的库授权」，而这个信息不能出现在返回给前端的 JSON 里。**用 Pydantic 的 `exclude` 而不是另建一个返回类型，是一个省事但有风险的做法**：任何用 `model_dump()` 之外的方式读取该对象的地方都可能泄出去。

**`confirm_import` 里那段「找知识索引节点 → 建/改数据集」的逻辑与 `import_rag_pipeline` 几乎完全重复。**

对比两处的代码：`import_rag_pipeline` 里从 `for node in nodes:` 开始到 `dataset_id = dataset.id` 的那一大段，与 `confirm_import` 里的同一段，结构一致但**细节不同**：

- 前者的名字用 `generate_incremental_name(names, name)` 去重，后者直接用 `name`；
- 前者有「已发布流水线不允许改分块结构」的校验，后者没有；
- 前者在 `dataset_created` 时先查全部库名，后者不查。

**这种「相似但不同」的重复是最危险的一类**：改一处容易忘记另一处，而差异本身又像是刻意的。从代码看不出哪个是「正确版本」。

**`_extract_dependencies_from_workflow` 是「导出前把环境变量解析成实际模型」。**

```python
referenced_llm_nodes = [
    node.get("data", {})
    for node in graph.get("nodes", [])
    if node.get("data", {}).get("type") == BuiltinNodeTypes.LLM
    and should_resolve_llm_model_selector(node.get("data", {}).get("model_selector"))
]
environment_variables = (
    {variable.name: variable for variable in workflow.environment_variables} if referenced_llm_nodes else {}
)
for node_data in referenced_llm_nodes:
    try:
        selector = parse_llm_model_selector(node_data["model_selector"])
        variable = environment_variables.get(selector[1])
        if not isinstance(variable, LLMEnvironmentVariable):
            raise ValueError(
                f"LLM environment variable '{selector[1]}' was not found or is not an LLM variable"
            )
        node_data["model"] = resolve_llm_model_config(
            node_model=ModelConfig.model_validate(node_data.get("model", {})),
            variable_name=selector[1],
            variable_value=variable.value,
        ).model_dump(mode="json")
    except ValueError as exc:
        logger.warning(
            "Skipping unresolved LLM environment model while extracting dependencies for selector %r: %s",
            node_data.get("model_selector"),
            exc,
        )
```

**五处：**

1. **只处理 `should_resolve_llm_model_selector(...)` 为真的节点** ——这是一个预判，避免对所有 LLM 节点都尝试解析。
2. **`environment_variables` 只在有引用节点时才构建** ——一个微优化，但对「几百个变量的工作流」是有意义的。
3. **解析成功后把结果写回 `node_data["model"]`** ——**注意这是原地改图**，而后面 `_extract_dependencies_from_workflow_graph(graph)` 依赖这个改动来提取模型依赖。
4. **解析失败只 `logger.warning`** ——导出继续，该节点保留原来的 `model_selector` 与旧 `model`。**这会让「导出的 DSL 里模型依赖不完整」**，从而「导入到别的租户后少了这个模型的依赖声明」。这是一个**静默的降级**，而它的注释（"Skipping unresolved LLM environment model"）至少留下了线索。
5. **`except ValueError` 只接住 ValueError** ——若 `parse_llm_model_selector` 抛别的异常（比如 `KeyError`/`IndexError`），整个导出会失败。**这是一个窄了点的捕获范围**，而它可能是刻意的（别的异常是真 bug）。

**导出的凭据剥离是三处显式的 `pop`。**

```python
# filter credential id from tool node
if not include_secret and data_type == BuiltinNodeTypes.TOOL:
    node_data.pop("credential_id", None)
# filter credential id from agent node
if not include_secret and data_type == BuiltinNodeTypes.AGENT:
    for tool in node_data.get("agent_parameters", {}).get("tools", {}).get("value", []):
        tool.pop("credential_id", None)
```

**三处：**

1. **`include_secret` 是显式参数，默认 `False`** ——「默认不导出凭据」是正确的默认方向。
2. **两条路径分别处理「工具节点自己的凭据」与「Agent 节点里每个工具的凭据」** ——后者嵌套在 `agent_parameters.tools.value` 这条三层路径里，说明**凭据可能藏在任意深度**，只能按节点类型逐个点名。
3. **`pop(..., None)` 而不是 `del`** ——容忍字段不存在。

**这里暴露的是「凭据位置不是结构化声明」这件事**：如果节点定义里有一个统一的 `secret_fields` 元数据，导出时就不需要按类型枚举。当前的写法意味着**每加一种带凭据的节点类型，都要回来加一条 `pop`**。

**`export_rag_pipeline_dsl` 里 `workflow.to_dict(include_secret=include_secret)` 与上面三处 `pop` 并存**，说明「剥离凭据」这件事分两层做：一层在模型序列化里，一层在导出时补刀。**两层都做是防御性的，但也说明第一层并不完备。**

**`check_dependencies` 与 `get_leaked_dependencies` 把「依赖」做成了一个可查询的状态。**

```python
redis_key = f"{CHECK_DEPENDENCIES_REDIS_KEY_PREFIX}{pipeline.id}"
dependencies = redis_client.get(redis_key)
if not dependencies:
    return CheckDependenciesResult()
```

**依赖检查的结果被暂存在 Redis（按 pipeline id 索引）**，于是「导入时发现的依赖」与「用户点检查时的依赖」是同一条数据。**缺失时返回空结果而不是报错** ——即「没有待检查的依赖」与「检查过了、没有缺失」不可区分。

**`create_rag_pipeline_dataset` 里有一处 `current_user` 的隐式依赖。**

```python
account = cast(Account, current_user)
rag_pipeline_import_info: RagPipelineImportInfo = self.import_rag_pipeline(
    account=account, ...
)
```

**`current_user` 没有作为参数传入，而是从模块级（Flask 的上下文局部变量）取的** ——这与同一文件的 `RagPipelineDslService.__init__(self, session: Session)` 风格矛盾。**一个显式声明「事务由调用方管」的服务类，却在方法里读全局当前用户**，是一个真实的耦合：这个类在 Flask 请求上下文之外不能正确工作。

## 关键取舍

**源码事实：** 已发布版本号是**时间戳字符串**（`str(datetime.now(UTC).replace(tzinfo=None))`），草稿固定为 `"draft"`；同一秒内两次发布会得到同一版本号，代码未见去重。

**源码事实：** `sync_draft_workflow` 用客户端传入的 `unique_hash` 做乐观并发控制，不一致抛 `WorkflowHashNotEqualError`；新建草稿后 `flush()` 取 id 并赋给 `pipeline.workflow_id`（未显式 `session.add(pipeline)`，依赖它已在同一 session）。末尾有一行注释掉的 `app_draft_workflow_was_synced.send(...)` 带 `TODO`。

**源码事实：** `publish_workflow` 在创建新版本前调用 `validate_llm_environment_model_references`（草稿允许悬空引用、发布不允许）；用函数内 import 打破 `dataset_service` ↔ `rag_pipeline` 的循环依赖，注释写明原因；发布时对 `knowledge-index` 节点调用 `DatasetService.update_rag_pipeline_dataset_settings`，传入的 `has_published=pipeline.is_published` 是**发布前**的状态（首次/再次发布规则不同）。

**源码事实：** `restore_published_workflow_to_draft` 复用共享 helper `apply_published_workflow_snapshot_to_draft` 做字段复制（返回 `(对象, 是否新建)`），自己只负责「新建时 add + flush + 挂回 `pipeline.workflow_id`」；docstring 声明源版本在复制与提交期间保持锁定。

**源码事实：** `run_draft_workflow_node` 把执行包成 `getter=lambda: ...` 传给 `_handle_node_run_result` 以便计时；变量池只种「系统变量 + 环境变量」（无上游输出，需用户手工填 `user_inputs`）；`enclosing_node_id` 用于把循环体内节点的变量存回正确的迭代上下文。写入分三个独立事务（执行记录、草稿变量、trace 入队），无共同事务。

**源码事实：** `RagPipelineDslService` 的类 docstring 声明「调用方负责 commit/rollback，本类只 flush，从不提交」——与 `RagPipelineService` 到处 `self._session.commit()` 的策略相反。

**源码事实：** `import_rag_pipeline` 对「可预期的用户输入错误」一律返回 `status=FAILED` 的结果对象（URL 失败、超限、YAML 非法、缺 `rag_pipeline`），只有 `import_mode` 非法与「`version` 类型不是 str」抛 `ValueError`。

**源码事实：** DSL 导入时缺失 `version` / `kind` 会被直接补成 `"0.1.0"` / `"rag_pipeline"`，且 `kind` 值不匹配时也会被**静默覆盖**；`"0.1.0"` 字面量在三处重复而 `CURRENT_DSL_VERSION` 常量未被引用。

**源码事实：** 主版本不兼容时把整份 YAML 与归属信息写进 Redis（键前缀 `app_import_info:`，TTL 10 分钟），返回 `PENDING`；`confirm_import` 通过 `PendingImportOwner.is_accessible_by` 校验租户与账号，**「过期」与「无权」返回同一条错误消息**（不泄漏存在性）；确认成功后删除 Redis 键。

**源码事实：** `RagPipelineImportInfo.dataset_created` 用 `Field(default=False, exclude=True)` 排除出序列化，注释说明用途是「post-commit authorization」。

**源码事实：** `import_rag_pipeline` 与 `confirm_import` 里「遍历知识索引节点 → 建/改数据集」的两段逻辑结构重复但细节不同（前者用 `generate_incremental_name` 去重并校验已发布流水线的分块结构，后者不查名字、不校验结构）。

**源码事实：** 数据集 id 用「租户 id 的 SHA-256 作 AES 密钥、密钥前 16 字节作 IV」的 AES-CBC 加密后 base64 导出，导入侧解密失败返回 `None`；加密目的是让 id 在别的租户无法解出，而非保密或完整性保护。

**源码事实：** 导出时凭据剥离分两层：`workflow.to_dict(include_secret=...)` 与三处按节点类型的 `pop("credential_id", None)`（工具节点自身、Agent 节点内 `agent_parameters.tools.value` 里每个工具），后者说明凭据位置没有结构化声明。

**源码事实：** `_extract_dependencies_from_workflow` 在导出前把 LLM 节点的环境变量引用解析成实际模型并**原地写回 `node_data["model"]`**，供后续依赖提取使用；解析失败只 `logger.warning` 并跳过，于是导出的依赖声明可能不完整。

**源码事实：** 依赖检查结果按 `pipeline.id` 暂存在 Redis（前缀 `app_check_dependencies:`），缺失时返回空 `CheckDependenciesResult()`，使「无待检查依赖」与「已检查无缺失」不可区分。

**源码事实：** `create_rag_pipeline_dataset` 从模块级 `current_user` 取当前账号（`cast(Account, current_user)`），未作为参数传入。

**架构推论：** 「草稿可编辑、发布即快照」是画布型产品的标准形状，而它把「乐观并发」的责任推给了客户端（`unique_hash`）。这个设计的前提是**前端确实持有完整状态**；若客户端是脚本或无状态调用方，「带 hash」就变成了一种必须遵守的约定而不是协议保证。**判断这类设计是否安全，看「不带 hash 会发生什么」**：这里 `unique_hash=None` 时若库里已有草稿会直接抛错（`workflow.unique_hash != None`），所以是安全的。

**架构推论：** 「同一功能域两个服务的事务策略相反」是分层的必然结果，但必须靠文档明确（这里 `RagPipelineDslService` 的 docstring 做到了）。**风险在调用方**：一个拿 `RagPipelineDslService` 当门面用的新代码会忘记 commit，于是数据静默丢失（因为它只 `flush`）。这类契约应当在**类型层面**表达（比如要求传入一个已开启事务的 session 类型），而 Python 里只能靠文档。

**架构推论：** 「凭据位置靠按类型枚举 `pop`」与「路由按类型注册」是同一类问题：**只要凭据/敏感字段的位置不是数据的一部分，每加一种载体就要回来补一次**。正确形状是让节点定义声明「哪些字段是秘密」的元数据，导出时统一遍历。当前实现的双层防御（序列化层 + 导出层）说明作者知道单层不够，但没有把语义抬到数据层。

**架构推论：** 「版本不兼容就挂起 + 用户确认」这个交互比「拒绝」或「静默升级」都好，因为它**把不可自动判断的决定交给用户**。而它的实现要点是：暂存必须带归属（谁能确认）与过期时间（不能永久占内存）；错误消息必须合并「不存在」与「无权」以免泄漏。这三条在这里都做到了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/services/rag_pipeline/rag_pipeline.py` | `RagPipelineService` | 流水线门面（草稿/发布/单步/数据源/模板） |
| `api/services/rag_pipeline/rag_pipeline.py` | `RagPipelineService.sync_draft_workflow` | 用 `unique_hash` 做乐观并发，新建或更新草稿 |
| `api/services/rag_pipeline/rag_pipeline.py` | `RagPipelineService.publish_workflow` | 生成时间戳版本快照 + 校验 LLM 环境变量 + 同步数据集设置 |
| `api/services/rag_pipeline/rag_pipeline.py` | `RagPipelineService.restore_published_workflow_to_draft` | 复用共享快照复制 helper，自己只做新建草稿的挂链 |
| `api/services/rag_pipeline/rag_pipeline.py` | `RagPipelineService.run_draft_workflow_node` | 单节点调试：种子变量池 + 三处独立事务写入 |
| `api/services/rag_pipeline/rag_pipeline.py` | `RagPipelineService.run_datasource_workflow_node` | 数据源节点执行（生成器），空值用 `match` 四分支识别 |
| `api/services/rag_pipeline/rag_pipeline.py` | `RagPipelineService.get_pipeline_templates` | 按 `HOSTED_FETCH_PIPELINE_TEMPLATES_MODE` 选检索实现，非英文空结果回退英文 |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineDslService` | DSL 导入/导出/依赖检查（调用方持事务，本类只 flush） |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineDslService.import_rag_pipeline` | 取 YAML（内容或 URL）→ 版本兼容 → 依赖提取 → 建库建管道 |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineDslService.confirm_import` | 从 Redis 取待确认 YAML、校验归属、建库并清理键 |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineDslService.export_rag_pipeline_dsl` | 组装 YAML：加密 dataset_ids、剥离凭据、附依赖 |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineDslService._append_workflow_export_data` | 草稿工作流导出与三处 credential 剥离 |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineDslService._extract_dependencies_from_workflow` | 把 LLM 环境变量引用解析成模型并原地写回图 |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineDslService.encrypt_dataset_id` / `decrypt_dataset_id` | 租户派生的 AES-CBC id 编解码 |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineDslService.check_dependencies` | 从 Redis 读依赖检查结果（缺失返回空结果） |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `CURRENT_DSL_VERSION` | 当前 DSL 版本 `"0.1.0"`（代码里另有字面量重复） |
| `api/services/rag_pipeline/rag_pipeline_dsl_service.py` | `RagPipelineImportInfo` / `RagPipelinePendingData` | 导入结果（含 `exclude=True` 的内部字段）与待确认数据 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 文档：RAG 流水线](https://docs.dify.ai/guides/knowledge-base/rag-pipeline)
- [SQLAlchemy 文档：sessionmaker 与 expire_on_commit](https://docs.sqlalchemy.org/en/20/orm/session_api.html)
- [PyCryptodome AES 文档](https://pycryptodome.readthedocs.io/en/latest/src/cipher/aes.html)

## 与主核 / AI 的关系

流水线服务与主核（多模态数据库内核）的交集在**「可编辑定义 + 不可变快照」与「跨边界序列化」**。四条经验。

**第一，可变定义与不可变快照必须分成两种存储形态。** 这里草稿是一行固定 `version="draft"` 的记录（原地改），发布是每次新增一行时间戳版本（永不改）。主核里做「表定义/索引定义」的在线变更时同理：**编辑态与生效态分开存**，生效态只追加不修改——否则「回滚到上一版本」就无从下手。而「版本号用时间戳」这个选择的代价（同一秒冲突）应当由唯一约束兜住，而不是靠时钟精度。

**第二，跨租户/跨实例交换的资源标识必须做「作用域绑定」。** 这里用租户 id 派生密钥加密数据集 id，目的不是保密而是「别的作用域解不开，于是不会指错」。主核里做备份/导出/复制时同理：**任何在文档里出现的外部标识都要能自证它属于哪个作用域**，否则导入到另一处会静默指向不存在的对象，而错误会推迟到第一次使用时才暴露。

**第三，敏感字段的位置应当是数据的一部分，而不是代码里的枚举。** 这里导出时按节点类型逐个 `pop("credential_id")`，三层嵌套（Agent 里的工具列表）也要单独处理。主核里做「脱敏导出」时同理：**让每个结构声明「哪些字段是秘密」，导出时统一遍历**，否则每加一种载体都要回来补一条，而漏掉的那一条不会有任何提示。

**第四，同一模块内两种事务策略并存时，契约必须写在类文档里，且调用方要能被工具检查。** 这里 `RagPipelineDslService` 明说「调用方负责 commit」，而同一功能域的 `RagPipelineService` 自己 commit。主核里做「核心 API 与可复用组件」的分层时同理：**组件不替调用方决定事务边界是对的，但必须让「忘记 commit 就会静默丢数据」这件事在类型或断言上可见**——否则它会以「某些操作偶尔没生效」的形式长期存在。
