---
project: dify
feature: model-provider
title: 模型提供方服务
page_type: feature
schema_version: 1
refs:
  - path: api/services/model_provider/service.py
    symbols: [ModelProviderService]
---

# 模型提供方服务

## 一句话职责

`ModelProviderService` 是「工作区里有哪些模型可用」这件事的门面：它把每一次请求都转成一次新的 `ProviderManager`（由插件层提供），并额外承担三件插件层不该知道的事——**列表接口绝不返回明文凭据**、**默认模型解析失败时仍能展示已保存的配置**、以及**只读取配额行是否存在而不读余额**。

## 机制是什么

模型提供方的数据分散在五张表里（`Provider`、`ProviderCredential`、`ProviderModel`、`ProviderModelCredential`、`TenantPreferredModelProvider`），另有一张 `TenantDefaultModel` 存「默认用哪个」。而它们全部要通过插件层（`ProviderManager`）聚合，因为「某个 provider 下有哪些模型」的真相在插件声明里，不在数据库里。

于是这个服务有两套数据来源，而它按场景分工：

- **要模型/凭据的完整能力时** → 走 `_get_provider_manager(tenant_id).get_configurations(tenant_id)`；
- **只要「有没有、叫什么」这类元信息时** → 直接查库（`_load_provider_summary_states`）。

`_load_provider_summary_states` 的 docstring 把这个分工写清了：

> Load workspace configuration metadata, including free quota row presence, without reading secrets.

**「including free quota row presence, without reading secrets」** ——两个限定都很具体：

- **只看「配额行是否存在」**，不去读余额（余额要调计费服务）；
- **不读密钥**，因为列表页不需要解密，而解密是这条路径上最贵的操作（与 h04 里 MCP 侧 `include_sensitive` 开关是同一类考虑）。

而「每次都新建 manager」是这个服务的另一半特征：

```python
@staticmethod
def _get_provider_manager(tenant_id: str) -> ProviderManager:
    return create_plugin_provider_manager(tenant_id=tenant_id)
```

**没有缓存、没有字段、没有构造器** ——整个类没有一个 `__init__`，所有方法都是 `@staticmethod` 或 `self` 上不存任何状态的方法。**「无状态门面」的收益是线程安全、无需生命周期管理；代价是每次调用都重建一次 manager，也就是每次都要重新拉一遍插件配置。**

## 怎么实现的

**默认模型解析有两个入口，区别只在「谁来管事务」。**

```python
def get_default_model_selection(
    self, tenant_id: str, model_type: ModelType, *, session: Session
) -> tuple[str, str] | None:
    """Resolve a workspace default without creating or committing a default-model record."""
    return self._get_saved_default_model_selection(tenant_id, model_type, session=session) or (
        self._discover_default_model_selection(tenant_id, model_type)
    )

def resolve_default_model_selection(
    self, tenant_id: str, model_type: ModelType, *, session_factory: sessionmaker[Session]
) -> tuple[str, str] | None:
    """Read the saved choice in a short session, then discover a model if no choice is saved."""
    with session_factory() as session:
        saved_model = self._get_saved_default_model_selection(tenant_id, model_type, session=session)
    return saved_model or self._discover_default_model_selection(tenant_id, model_type)
```

**四处：**

1. **两条 docstring 都在强调「不写库」** ——`without creating or committing a default-model record` / `Read the saved choice in a short session`。**「解析」与「保存」被刻意分开**：查询默认模型不该顺手把发现结果落库，否则一个只读接口会产生写副作用。
2. **`resolve_` 版本把读库限制在一个短 session 里，且 `with` 块**只包住读**，发现逻辑在块外** ——于是「网络/插件调用」不发生在一个已开启的事务里。这是 h04 里 `validate_server_url_standalone` 的同一条原则（网络 I/O 必须在事务外），只是用在了更小的尺度上。
3. **两个版本的表达式结构相同**（`saved or discover`），差异只在 session 的获取方式。**之所以留两份而不是一个**，是因为调用方有的已经持有一个 session（不能自己开新的），有的只有一个 `sessionmaker`。
4. **`or` 的短路语义在这里恰好正确** ——`None` 才继续发现，而 `("", "")` 这样的空元组不会被误判（它不会被返回，因为 `_get_saved_default_model_selection` 只在拿到行时才构造元组）。

**`_discover_default_model_selection` 的注释说「失败返回 None」，而它确实吞掉了所有异常。**

```python
def _discover_default_model_selection(self, tenant_id: str, model_type: ModelType) -> tuple[str, str] | None:
    """Return the first active provider model, or None if discovery fails or finds none."""
    try:
        configurations = self._get_provider_manager(tenant_id).get_configurations(tenant_id)
        available_models = configurations.get_models(model_type=model_type, only_active=True)
    except Exception:
        logger.warning("Could not resolve available default model, tenant_id: %s", tenant_id, exc_info=True)
        return None
    if not available_models:
        return None

    first_model = available_models[0]
    return first_model.provider.provider, first_model.model
```

**三处：**

1. **`except Exception` + `logger.warning(..., exc_info=True)`** ——保留了栈，但没有把异常上抛。**理由是「发现默认模型」是一个尽力而为的动作**：没有插件可用时，调用方应当拿到 `None` 而不是一个 500。
2. **「取第一个」没有任何排序保证** ——`available_models[0]` 的顺序由插件的配置顺序决定。**所以「默认模型」在用户没设置之前是不确定的**，而这一点没有注释说明。
3. **`only_active=True`** ——只考虑已启用的模型，与 `get_models_by_model_type` 一致。

**`initialize_default_llm` 是一个完整的「双检 + 唯一约束兜底」实现。**

```python
def initialize_default_llm(self, tenant_id: str) -> None:
    """Save the first active LLM during onboarding without replacing a configured default."""
    saved_default = select(TenantDefaultModel.id).where(
        TenantDefaultModel.tenant_id == tenant_id,
        TenantDefaultModel.model_type == ModelType.LLM,
    )
    with session_factory.create_session() as session:
        if session.scalar(saved_default) is not None:
            return

    configurations = self._get_provider_manager(tenant_id).get_configurations(tenant_id)
    available_models = configurations.get_models(model_type=ModelType.LLM, only_active=True)
    if not available_models:
        logger.info("No active LLM available for default model initialization, tenant_id: %s", tenant_id)
        return

    model = available_models[0]
    try:
        with session_factory.create_session() as session, session.begin():
            if session.scalar(saved_default) is not None:
                return
            session.add(
                TenantDefaultModel(
                    tenant_id=tenant_id,
                    model_type=ModelType.LLM,
                    provider_name=model.provider.provider,
                    model_name=model.model,
                )
            )
    except IntegrityError:
        # A concurrent initializer or user selection may have saved the default first.
        with session_factory.create_session() as session:
            if session.scalar(saved_default) is None:
                raise
        return
```

**六处：**

1. **方法名里 `initialize` 而不 `set`** ——docstring 说「without replacing a configured default」，即**幂等且不覆盖**。这与 h01 里 `create_empty_dataset` 的「先查后插」是同一个意图，但这里做得更完整。
2. **第一次检查在事务外，第二次在 `session.begin()` 内** ——这是标准的双检。**注意第二次检查用的是同一个 `saved_default` 语句对象**，复用了它的构造。
3. **最外层那次检查的价值是省掉 `_get_provider_manager`** ——一个已经初始化过的工作区（绝大多数）不会触发插件加载。**这是一个「用一次查询换一次插件加载」的优化**，很值。
4. **`except IntegrityError` 的分支是「冲突时确认是否真的冲突了」**：

```python
except IntegrityError:
    with session_factory.create_session() as session:
        if session.scalar(saved_default) is None:
            raise
    return
```

**「唯一约束冲突后重查一遍：如果现在还是查不到，说明冲突不是这条默认模型导致的，重新抛出。」** 这个模式比「一律忽略 IntegrityError」正确得多——**后者会把真正的约束冲突（比如别的唯一键撞了）当成幂等成功而静默吞掉。**

5. **`session.begin()` 与 `create_session()` 一起用** ——显式事务边界。
6. **末尾 `logger.info` 打印 provider 与 model** ——初始化结果可观测。**注意它只在成功路径上打印，而「并发已存在」的路径是静默 return**（这时日志里看不到「另一个人已经建了」，而这也合理：那不是失败）。

**`get_default_model_of_model_type` 的降级是「用残缺的 provider 元信息展示已保存的配置」。**

```python
try:
    result = self._get_provider_manager(tenant_id).get_default_model(tenant_id=tenant_id, model_type=model_type_enum)
    return DefaultModelResponse(...) if result else None
except Exception as e:
    logger.debug("get_default_model_of_model_type error: %s", e)

# Provider metadata is optional for displaying the saved configuration.
with session_factory.create_session() as session:
    saved_model = session.execute(
        select(TenantDefaultModel.model_name, TenantDefaultModel.provider_name).where(...)
    ).one_or_none()

if saved_model is None:
    return None

return DefaultModelResponse(
    model=saved_model.model_name,
    model_type=model_type_enum,
    provider=SimpleProviderEntityResponse(
        tenant_id=tenant_id,
        provider=saved_model.provider_name,
        label=I18nObject(en_US=saved_model.provider_name, zh_Hans=saved_model.provider_name),
        supported_model_types=[],
    ),
)
```

**五处：**

1. **异常只记 `logger.debug`** ——比 `_discover_default_model_selection` 的 `warning` 更安静，因为**这里有降级路径，异常不是终点**。
2. **降级路径造了一个「假 provider」**：`label` 直接用 provider 名（中英文相同），`supported_model_types=[]` 空列表。**注释解释了为什么可以这么做：「Provider metadata is optional for displaying the saved configuration.」** ——展示配置只需要名字，不需要图标、标签、支持的类型列表。
3. **这意味着一部分响应是「不完整的但有用」的** ——前端能显示「默认模型是 gpt-4o，提供方是 openai」，但显示不出 openai 的图标。**「优雅降级到部分信息」比「整个接口 500」好，代价是消费方必须容忍空列表。**
4. **`only_active` 无关**——这条路径直接查 `TenantDefaultModel`，不看模型是否仍启用。**所以一个已被禁用的模型仍会被报成「默认模型」**，而它在推理时会失败。这是一个真实的语义缺口：**「保存的默认」与「可用的默认」在这条降级路径上被合并了。**
5. **`if result else None` 在 try 内部** ——所以「manager 返回 None」与「manager 抛异常」走了两条不同的路：前者直接返回 `None`（不降级查库），后者才降级。**这个区别是对的**：manager 说「没有默认模型」是权威答案，而抛异常说明它不可用。

**`get_provider_list` 里有一段醒目的凭据剥离代码，注释用「IMPORTANT」开头。**

```python
# IMPORTANT: Never expose decrypted credentials in the provider list API.
# Sanitize custom model configurations by dropping the credentials payload.
sanitized_model_config = []
if models:
    from core.entities.provider_entities import CustomModelConfiguration  # local import to avoid cycles

    for model in models:
        sanitized_model_config.append(
            CustomModelConfiguration(
                model=model.model,
                model_type=model.model_type,
                credentials=None,  # strip secrets from list view
                current_credential_id=model.current_credential_id,
                current_credential_name=model.current_credential_name,
                available_model_credentials=model.available_model_credentials,
                unadded_to_model_list=model.unadded_to_model_list,
            )
        )
```

**五处：**

1. **不是「不返回某个字段」，而是逐字段重建一个对象并把 `credentials` 设为 `None`** ——这个写法比 `del` 或 `copy` + 置空更明确：**它强迫作者逐字段列出「哪些要保留」，于是新加字段默认不会被带上**。这是一个「白名单而非黑名单」的实现。
2. **代价是每加一个字段都要回来补**，否则该字段在列表接口里消失（而不是像黑名单那样泄漏）。
3. **`current_credential_id` / `current_credential_name` 被保留了** ——它们是元信息而不是密钥。**「credential 的名字」能展示，「credential 的值」不能**，这个区分在字段级被做对了。
4. **`from core.entities.provider_entities import CustomModelConfiguration  # local import to avoid cycles`** ——函数内 import 是为了打破循环依赖。**与 h03 里 `publish_workflow` 的那句 `# dataset_service imports RagPipelineService, so defer this reverse dependency.` 是同一手法。**
5. **`if models:` 包住整个循环** ——空列表时跳过，也顺带跳过了那次 import。

**`get_provider_summary_list` 的读顺序被注释解释了，而它揭示了一处缓存失效逻辑。**

```python
# Read bindings first: remote-debug identity changes invalidate provider metadata
# before the provider cache is consulted.
bindings = PluginService.list_model_provider_bindings(tenant_id)
provider_entities = PluginService.fetch_plugin_model_providers(tenant_id=tenant_id)
states = self._load_provider_summary_states(tenant_id)
```

**「先读 bindings」** ——因为**远程调试的标识变化会让 provider 元数据失效，而这个失效必须在查缓存之前发生**。也就是说，`list_model_provider_bindings` 的副作用**包括让缓存作废**。

**这是一个隐式的时序依赖**：三个调用看起来可以任意排序（都是读），但实际第一个会改缓存状态。**它没有写在函数签名上，只写在这条注释里。** 任何重构这三个调用的顺序都会引入「读到旧元数据」的 bug，而症状是「插件改了但界面没变」。

**`_load_provider_summary_states` 用四条查询 + 一次内存聚合来避免逐 provider 查询。**

```python
states: defaultdict[str, _ProviderSummaryState] = defaultdict(_ProviderSummaryState)
```

**用 `defaultdict` 让「第一次遇到某个 provider」自动建状态**，于是后面的赋值不需要 `if name not in states`。

而四条查询各有明确的作用：

1. **`provider_rows`**：`Provider` 左连 `ProviderCredential`（`and_(credential.id == provider.credential_id, credential.tenant_id == tenant_id)`），且 `where` 里用 `or_` 限定「自定义 provider **或** 系统 provider 且配额类型为 FREE」。**这个 `or_` 就是「配额行是否存在」的判定来源** ——只有系统 provider 的 FREE 配额行会被计入，于是「有没有免费额度」不需要调计费服务。
2. **`credential_rows`**：全部凭据，按 `created_at desc, id desc` 排序。**第二排序键 `id` 保证同刻创建的凭据顺序确定**（与 h06 里 `_parent_first` 的 `(start_time, span_id)` 是同一种考虑）。
3. **`custom_model_rows`**：`ProviderModel` 与 `ProviderModelCredential` 的 **`union`**（不是 `union_all`）——所以同一个 provider 名在两张表里都出现时只算一次。
4. **`preferred_provider_rows`**：`TenantPreferredModelProvider` 的偏好类型。

**聚合循环里有一处「规范化名 vs 原始名」的判据，它处理的是「同一个 provider 存了两种写法」。**

```python
for provider in provider_rows:
    provider_name = str(ModelProviderID(provider.provider_name))
    state = states[provider_name]
    if provider.provider_type == ProviderType.SYSTEM:
        state.has_free_quota = True
    elif not state.has_custom_provider or provider.provider_name == provider_name:
        state.has_custom_provider = True
        state.current_credential_id = provider.credential_id
        ...
```

**`provider.provider_name == provider_name`** ——这里 `provider_name` 是**规范化之后**的名字（`str(ModelProviderID(...))`），而左边是原始值。**所以这个比较的含义是「这一行的原始名恰好就是规范名」**，即「规范形式那一行」。条件 `not state.has_custom_provider or <是规范行>` 的语义是：**第一次遇到就收，或者遇到规范行就覆盖**。

**效果是「规范行优先于非规范行」** ——与下面偏好提供方那段完全同构：

```python
preferred_provider_priorities: dict[str, bool] = {}
for preferred_provider in preferred_provider_rows:
    provider_name = str(ModelProviderID(preferred_provider.provider_name))
    is_canonical_row = preferred_provider.provider_name == provider_name
    if provider_name in preferred_provider_priorities and not is_canonical_row:
        continue
    preferred_provider_priorities[provider_name] = is_canonical_row
    states[provider_name].preferred_provider_type = preferred_provider.preferred_provider_type
```

**偏好段把「是否规范行」显式命名成 `is_canonical_row`**，比上面那段的内联比较可读得多——**同一规则两处实现，一处起了名一处没起。**

**而两段都在解决同一个历史问题：`Provider.provider_name` 可能带组织前缀（如 `langgenius/openai`）也可能不带，而 `ModelProviderID` 归一成同一个键。** 于是「同一个 provider 有两行」是常态，必须挑一行作为权威。

**`_select_binding` 的优先级是「远程调试优先于已安装插件」。**

```python
@staticmethod
def _select_binding(
    current_binding: PluginModelProviderBinding | None,
    candidate_binding: PluginModelProviderBinding,
) -> PluginModelProviderBinding:
    """Prefer a remote-debug runtime when one shadows an installed plugin."""
    if current_binding is None:
        return candidate_binding
    if (
        candidate_binding.source == PluginInstallationSource.Remote
        and current_binding.source != PluginInstallationSource.Remote
    ):
        return candidate_binding
    return current_binding
```

**三处：**

1. **这是一个「reduce 用的比较函数」** ——签名是 `(当前, 候选) -> 新的当前`，适合 `functools.reduce` 或循环累积。**把「选哪个」的规则抽成一个纯函数，让累积逻辑不必知道规则。**
2. **规则只有一条：Remote 胜过非 Remote。** 两个都是 Remote 或两个都不是时保留先来的（`return current_binding`）——**于是「先来」的顺序决定了同类之间的胜者，而顺序来自上游的列表**。
3. **「remote-debug 遮蔽已安装插件」是一个调试便利** ——开发者在本地跑插件时，应当看到本地版本而不是已发布的。**这也是为什么 `get_provider_summary_list` 要先读 bindings**：这个遮蔽关系会影响后续所有的元数据。

**`_get_preferred_provider_type` 是一棵四级回退树，而它的默认分支值得记住。**

```python
@staticmethod
def _get_preferred_provider_type(
    state: _ProviderSummaryState,
    *,
    custom_present: bool,
    system_enabled: bool,
) -> ProviderType:
    if state.preferred_provider_type is not None:
        return state.preferred_provider_type
    if dify_config.DEPLOYMENT_EDITION == DeploymentEdition.CLOUD and system_enabled:
        return ProviderType.SYSTEM
    if custom_present:
        return ProviderType.CUSTOM
    if system_enabled:
        return ProviderType.SYSTEM
    return ProviderType.CUSTOM
```

**四处：**

1. **用户显式设置优先**（`state.preferred_provider_type is not None`）。
2. **云版 + 系统 provider 可用 → 系统优先** ——这是一个**商业策略**：托管版默认用平台提供的配额（因为它要计费）。
3. **自托管版：自定义优先于系统**（第 3、4 条）——即在私有部署里，用户自己配的凭据优先。
4. **最后的兜底是 `CUSTOM`** ——**即使 `system_enabled=False` 且 `custom_present=False` 也返回 `CUSTOM`**。这是一个「没有可用选项时仍然给一个确定值」的选择，而 `CUSTOM` 在这里的实际含义是「你该自己配一个」。**注释没有解释这一点，而它决定了空配置工作区的 UI 会高亮「自定义」标签。**

**`get_models_by_model_type` 的分组循环里有一处「先建桶再过滤」的顺序。**

```python
provider_models: dict[str, list[ModelWithProviderEntity]] = {}
for model in models:
    if model.provider.provider not in provider_models:
        provider_models[model.provider.provider] = []

    if model.deprecated:
        continue

    provider_models[model.provider.provider].append(model)
```

**空桶在 `deprecated` 检查之前就建好了**，于是「某 provider 下所有模型都废弃」会留下一个空列表。而后面有这个兜底：

```python
for provider, models in provider_models.items():
    if not models:
        continue
```

**「建空桶 → 后面跳过空桶」是一个两步的过滤** ——功能正确，但多了一次无用的字典写入。**若把建桶移到 `deprecated` 检查之后**，空桶就不会产生，`if not models: continue` 也可以去掉。

**而 `status=CustomConfigurationStatus.ACTIVE` 是硬编码的。**

```python
providers_with_models.append(
    ProviderWithModelsResponse(
        ...
        status=CustomConfigurationStatus.ACTIVE,
        ...
    )
)
```

**所有出现在这个列表里的 provider 都被标成 `ACTIVE`** ——因为能走到这里说明它有可用模型（`only_active=True` 且非 deprecated）。**于是这个字段在响应里没有信息量**，但它对前端是必需的（同一形状也在别处使用，那里可能是别的值）。

**`get_model_parameter_rules` 的 docstring 说明了它为什么要复用同一条路径。**

```python
def get_model_parameter_rules(self, tenant_id: str, provider: str, model: str) -> list[ParameterRule]:
    """
    Get LLM parameter rules using credentials from the same route as inference.
    """
    provider_configuration = configuration_for_model(
        self._get_provider_configuration(tenant_id, provider), ModelType.LLM, model
    )

    credentials = provider_configuration.get_current_credentials(model_type=ModelType.LLM, model=model)
    if not credentials:
        return []

    model_schema = provider_configuration.get_model_schema(
        model_type=ModelType.LLM, model=model, credentials=credentials
    )

    return model_schema.parameter_rules if model_schema else []
```

**四处：**

1. **「using credentials from the same route as inference」** ——**参数规则必须与推理时用同一套凭据**，否则「界面上能调的温度范围」与「实际请求时模型接受的范围」可能不一致（不同凭据可能对应不同的模型变体/区域）。
2. **`configuration_for_model(...)` 按模型解析配置** ——因为同一个 provider 下不同模型可能有不同的配置分支。
3. **`if not credentials: return []`** ——**没凭据时返回空参数列表而不是报错**。这意味着「还没配凭据」时界面能正常打开，只是没有参数可调。**这是一个正确的降级方向**（参数规则是辅助信息）。
4. **`model_schema.parameter_rules if model_schema else []`** ——第二次降级：有凭据但取不到 schema 时也返回空。

**`get_provider` 与 `_get_provider_configuration` 的分工是「一个不报错、一个报错」。**

```python
def get_provider(self, tenant_id: str, provider: str) -> ProviderResponse:
def _get_provider_configuration(self, tenant_id: str, provider: str):
    """
    Raises:
        ProviderNotFoundError: If provider doesn't exist
    """
    provider_configurations = self._get_provider_manager(tenant_id).get_configurations(tenant_id)
    provider_configuration = provider_configurations.get(provider)
    if not provider_configuration:
        raise ProviderNotFoundError(f"Provider {provider} does not exist.")
    return provider_configuration
```

**`_get_provider_configuration` 的 docstring 有完整的 `Raises:` 段** ——而全文件其他方法大多只有 `:param:` / `:return:`。**这说明「会抛 `ProviderNotFoundError`」是一个需要显式声明的契约**（因为它是这个类唯一自己抛的领域异常）。

**大量方法是纯委托，而它们的存在是为了「让控制器不直接依赖插件层」。**

```python
def enable_model(self, tenant_id: str, provider: str, model: str, model_type: str):
    provider_configuration = self._get_provider_configuration(tenant_id, provider)
    provider_configuration.enable_model(model=model, model_type=ModelType(model_type))
```

`enable_model` / `disable_model` / `switch_preferred_provider` / `update_default_model_of_model_type` 都是这种形状：**取配置 → 调一个方法**。

**这层的实际价值有三点，而它们在代码里都能看到：**

1. **统一把 `str` 参数转成枚举**（`ModelType(model_type)`、`ProviderType.value_of(...)`）——**转换点集中在这里，插件层只接收枚举**；
2. **统一把「找不到 provider」变成 `ProviderNotFoundError`**；
3. **提供了一个可以被打桩的接缝**——测试可以替换这个方法，而不需要模拟整个插件系统。

**代价是它几乎全是「转发 + 转换」，真正的逻辑都在 `ProviderManager` 里。** 判断是否值得看一条标准：**这一层有没有做插件层不该做的事？** 答案是**有三件**（凭据剥离、默认模型降级展示、免密钥的汇总查询），所以这一层不是纯粹的透传。

## 关键取舍

**源码事实：** 整个服务无状态（无 `__init__`、方法皆为静态或无状态），每次调用 `_get_provider_manager(tenant_id)` 通过 `create_plugin_provider_manager` 新建一个 `ProviderManager`，无缓存。

**源码事实：** 默认模型解析有两个入口：`get_default_model_selection`（收已开启的 `Session`）与 `resolve_default_model_selection`（收 `sessionmaker`，把读库限制在一个短 session 内，发现逻辑在 `with` 块外）；两者 docstring 都声明「不创建/不提交默认模型记录」。

**源码事实：** `_discover_default_model_selection` 吞掉所有异常（`logger.warning(..., exc_info=True)`）返回 `None`，取 `available_models[0]` 无排序保证。

**源码事实：** `initialize_default_llm` 用「事务外检查 → 插件发现 → `session.begin()` 内二次检查 → `IntegrityError` 后重查以决定是否重抛」的完整幂等模式；docstring 说明「without replacing a configured default」。

**源码事实：** `get_default_model_of_model_type` 在 manager 抛异常时降级为「直接查 `TenantDefaultModel` 并构造一个 `label` 等于 provider 名、`supported_model_types=[]` 的假 provider」，注释说明「Provider metadata is optional for displaying the saved configuration」；manager 返回 `None` 时不降级。

**源码事实：** `get_provider_list` 以白名单方式逐字段重建 `CustomModelConfiguration` 并把 `credentials` 设为 `None`（注释以 IMPORTANT 开头说明绝不暴露明文凭据）；函数内 import 以避免循环依赖。

**源码事实：** `get_provider_summary_list` 必须先读 `PluginService.list_model_provider_bindings`（注释：remote-debug identity changes invalidate provider metadata before the provider cache is consulted），再读 provider 实体，最后读汇总状态。

**源码事实：** `_load_provider_summary_states` 用四条查询（含 `Provider` 左连 `ProviderCredential`、`ProviderModel` 与 `ProviderModelCredential` 的 `union`、凭据按 `created_at desc, id desc` 排序）+ `defaultdict` 内存聚合；「免费配额行是否存在」由 `or_(custom, and_(system, quota_type == FREE))` 判定，不读余额；不读密钥。

**源码事实：** 汇总聚合里两条「规范化名优先于非规范名」的规则：provider 行用内联比较（`provider.provider_name == provider_name`），偏好行显式命名 `is_canonical_row`；同一规则两处实现，一处有名一处无名。

**源码事实：** `_select_binding` 让 `PluginInstallationSource.Remote` 遮蔽非 Remote 的已安装插件（docstring：Prefer a remote-debug runtime when one shadows an installed plugin）；同类之间保留先到者。

**源码事实：** `_get_preferred_provider_type` 四级回退：已保存 → （云版且系统可用）系统 → 自定义存在 → 系统可用 → **兜底 `CUSTOM`**。

**源码事实：** `get_models_by_model_type` 在 `deprecated` 检查之前就建立 provider 空桶，随后用 `if not models: continue` 过滤，且 `status` 硬编码 `CustomConfigurationStatus.ACTIVE`。

**源码事实：** `get_model_parameter_rules` 复用推理路径的凭据解析（`configuration_for_model` + `get_current_credentials`），无凭据或无 schema 时都返回空列表。

**源码事实：** `_get_provider_configuration` 是唯一在 docstring 里带 `Raises:` 段的方法（`ProviderNotFoundError`）。

**架构推论：** 「无状态门面 + 每次重建 manager」让并发安全与生命周期管理变得免费，代价是**每次调用都重复一遍插件配置加载**。这在「列表页一次请求调十次服务方法」时会放大成十次加载。**判断这种设计是否合适，看有没有 `N+1` 式的调用模式**；如果有，缓存应当加在 manager 创建层（带失效策略）而不是在服务层加状态。

**架构推论：** 「优雅降级到部分信息」是列表/详情接口的正确默认，而它的风险是**把不同语义合并**。`get_default_model_of_model_type` 的降级路径不检查模型是否仍启用，于是「保存的默认」与「可用的默认」在 provider 解析失败时被混为一谈。主核里做「配置展示」与「配置生效」的分离时同理：**展示用的降级必须显式标注「这是保存值」而不是「这是生效值」**，否则用户会看到一个永远失败的默认模型却找不到原因。

**架构推论：** 「白名单式重建对象」是剥离敏感字段最可靠的手法——它让新字段默认不被带出，从而把「泄漏」变成「缺失」。代价是每加字段都要回来补，而**这个代价是可接受的**：缺失会被使用方立刻发现，泄漏不会。

**架构推论：** 「读操作有副作用」是最难从签名上看出的依赖。这里 `list_model_provider_bindings` 会让缓存失效，而三个读调用的顺序因此不可交换。**正确形状是让失效显式**（先调一个 `invalidate_if_remote_changed()`），或者至少让名字体现副作用。仅靠注释维持的时序约束，在下一次重构时几乎必然被破坏。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/services/model_provider/service.py` | `ModelProviderService` | 模型提供方门面（无状态；每次调用新建 `ProviderManager`） |
| `api/services/model_provider/service.py` | `ModelProviderService.get_provider_list` | 提供方列表（白名单重建对象、`credentials=None` 剥离明文） |
| `api/services/model_provider/service.py` | `ModelProviderService.get_provider_summary_list` | 汇总列表（先读 bindings 以触发缓存失效；不读余额、不解密） |
| `api/services/model_provider/service.py` | `ModelProviderService._load_provider_summary_states` | 四条查询聚合出 `_ProviderSummaryState`（含免费配额行存在性） |
| `api/services/model_provider/service.py` | `ModelProviderService.get_default_model_selection` / `resolve_default_model_selection` | 默认模型解析的两个入口（事务所有权的两种形态） |
| `api/services/model_provider/service.py` | `ModelProviderService._discover_default_model_selection` | 发现首个可用模型（吞异常、取 `available_models[0]`） |
| `api/services/model_provider/service.py` | `ModelProviderService.initialize_default_llm` | 引导期落地默认 LLM（双检 + `IntegrityError` 重查） |
| `api/services/model_provider/service.py` | `ModelProviderService.get_default_model_of_model_type` | 默认模型读取（manager 失败时降级展示已保存配置） |
| `api/services/model_provider/service.py` | `ModelProviderService.get_models_by_model_type` | 按类型列模型并分组（跳过 deprecated） |
| `api/services/model_provider/service.py` | `ModelProviderService.get_model_parameter_rules` | 参数规则（复用推理路径的凭据） |
| `api/services/model_provider/service.py` | `ModelProviderService._get_provider_configuration` | 取配置或抛 `ProviderNotFoundError`（唯一带 `Raises:` 文档的方法） |
| `api/services/model_provider/service.py` | `ModelProviderService._get_preferred_provider_type` | 首选类型四级回退（兜底 `CUSTOM`） |
| `api/services/model_provider/service.py` | `ModelProviderService._select_binding` | 绑定选择（Remote 遮蔽已安装插件） |
| `api/services/model_provider/service.py` | `_ProviderSummaryState` | 汇总状态的 `slots` dataclass（八个字段） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 文档：模型提供方配置](https://docs.dify.ai/guides/model-configuration)
- [SQLAlchemy 文档：`union` 与 `outerjoin`](https://docs.sqlalchemy.org/en/20/core/selectable.html)
- [Python 文档：dataclass 与 slots](https://docs.python.org/3/library/dataclasses.html#dataclasses.dataclass)

## 与主核 / AI 的关系

模型提供方服务在主核（多模态数据库内核）里对应的是**扩展/驱动的配置管理**（哪套驱动可用、用哪个凭据、默认用哪个）。四条经验。

**第一，展示层与生效层必须分开，且降级路径要显式标注自己是哪一个。** 这里 manager 失联时降级用「保存的 provider 名」造一个假 provider 来展示配置，但不检查模型是否仍启用。主核里做「配置回显」（表空间路径、复制槽状态、索引参数）时同理：**展示值可以来自持久化配置，但必须让使用者知道「这是配的，不是现在生效的」**——否则一个永远失败的默认对象会让人查错方向。

**第二，剥离敏感字段要用白名单式重建，而不是删除字段。** 这里逐字段重建 `CustomModelConfiguration` 并置 `credentials=None`，于是新加字段默认不会被带出。主核里做「配置导出」「诊断信息收集」时同理：**黑名单式删除会随字段增加而泄漏，白名单式重建只会缺失**——而缺失会被立刻发现。

**第三，幂等的初始化要「双检 + 唯一约束兜底 + 冲突后重查」。** `initialize_default_llm` 的第三段（`IntegrityError` 后重查，查不到就重抛）是正确形状：**它区分了「幂等冲突」与「真的约束冲突」**。主核里做「首次建目录页」「首次注册扩展」时同理：**无条件吞掉 `IntegrityError` 会把真正的数据错误变成静默成功。**

**第四，读操作若有副作用，名字或结构必须让它可见。** 这里「先读 bindings 以让缓存失效」只靠一条注释维持，三个读调用的顺序不可交换。主核里凡是「读会触发失效/刷新」的路径（读系统表会重建缓存、读统计信息会采样），都该把失效拆成显式调用——**能被签名看出的依赖才能在重构后存活。**
