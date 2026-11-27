---
project: pydantic-ai
feature: model-providers
title: 提供方与模型档案
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/providers/__init__.py
    symbols: [Provider, infer_provider_class, infer_provider]
  - path: pydantic_ai_slim/pydantic_ai/profiles/__init__.py
    symbols: [ModelProfile, merge_profile]
---

# 提供方与模型档案

## 一句话职责

把「谁来提供 API 客户端」与「这个模型有什么固有特性」拆成两层：`Provider` 负责认证、端点、客户端生命周期，`ModelProfile` 是一张纯数据的能力表，让同一份模型代码在不同供应商、不同模型上都能自动适配而不必写 `isinstance` 分支。

## 机制是什么

`Provider[InterfaceClient]` 是一个泛型 ABC，三个抽象成员 `name`、`base_url`、`client`，外加一堆有默认实现的协作属性：

- `canonical_system`：这个 provider 背后真正的服务（默认等于 `name`）。
- `compatible_systems`：它还能验证哪些别的系统的无状态加密数据（默认空集，且是**单向**的）。
- `model_id_namespace`：模型 ID 的限定命名空间（默认 `name`）。
- `shares_server_state_with(provider_url)`：能否查回在该端点创建的服务端状态。

`ModelProfile` 是一个 `total=False` 的 `TypedDict`：字段全部可选，缺省即「用文档化的默认值」，默认值在各访问点应用。子类（`OpenAIModelProfile`、`AnthropicModelProfile` 等）追加供应商特有的键，跨类合并靠 dict 展开。

`merge_profile(base, *overrides)` 是唯一的合并入口：按顺序展开，后者覆盖前者，`None` 视为空，且**每个输入先做旧键名翻译**再展开。

## 怎么实现的

**`infer_provider_class` 是一张 `match` 表。** 输入是 provider 名字符串，输出是类。它先剥掉 `gateway/` 前缀（网关路由到上游 provider 是另一层关注点），然后按名字分支返回对应类。`infer_provider(provider)` 是它的实例化版本：`provider_class()`，对 `gateway/` 前缀则走 `gateway_provider(upstream)`。两个函数分开的价值在于：需要类（比如做 `isinstance` 或子类化）与需要实例的地方各取所需。

**`name` 不只是标签，是历史的一部分。** `Provider.name` 的文档警告写得很重：这个值会流进每条 `ModelMessage.provider_name`，而思考块识别与原生工具识别在加载历史时会检查它，所以**静默改掉一个已发布 provider 的 `name` 会让用旧值记录的历史无法重放**。这一条把「重命名」从无害重构变成了兼容性破坏。

**`canonical_system` 与 `name` 为什么要分开。** 因为它们回答不同问题。`name` 是「这个 provider 实例叫什么」（可被子类改名），`canonical_system` 是「它后面真正的服务是谁」。需要按服务判断的地方（比如某个原生工具接受哪些模型 ID）读后者，于是「只改了认证方式、打同一个 API」的子类不会丢失与母体的数据共享。`_canonical_system_follows_client` 这个类变量处理一个特例：当 `canonical_system` 取决于实例化时用的客户端而不是名字（`google` 走 Gemini 还是 Google Cloud、`openai` 走 OpenAI 还是 Azure）时置真，此时响应必须总是记录 canonical system，否则新历史里同名两系统就分不清。

**两种回传判定的分工是这套设计最需要记住的一点。** `accepts_stateless_artifacts` 按 **canonical system** 判：那些靠 API 自己的密钥与格式校验、不需要查库的数据（加密推理、思考签名、redacted thinking、加密的服务端工具结果、OpenAI 压缩项）在系统匹配或列在 `compatible_systems` 里时可回传。`accepts_server_state` 按 **endpoint** 判：那些 API 存起来按 ID 查的东西（`previous_response_id`、item ID、会话、代码执行容器）要求 `provider_name` 相同**且** provider 认可 `response.provider_url`。两条路分开的直接后果：同一个 API 的另一个端点产出的响应，加密推理仍能回传，但 ID 不能。这对网关与代理场景是必要的区分。

**`shares_server_state_with` 的默认实现只比 URL。** 归一化后比较（忽略 scheme/host 大小写、默认端口、尾部斜杠）。文档给出覆盖条件：两个不同 URL 到达同一份服务端状态（例如代理别名）时重写。这条默认实现的保守性是对的——宁可少回传 ID，也不要跨端点查一个不存在的对象。

**`compatible_systems` 是单向的。** 文档明确：把系统 X 列进来意味着**把 X 的数据发给这个 provider**，而不是反过来。这个方向性很重要，因为它决定了加密数据能否被正确验签；把方向搞反会导致运行期 400 而不是编译期错误。

**`ModelProfile` 的「缺省即默认」约定。** 全部字段可选的代价是「默认值分散在各访问点」，收益是 profile 可以只写与默认不同的部分。这个选择的实际影响：读 profile 必须用带默认值的 `get`，不能假设键存在。源码里大量 `self.profile.get('supports_cache', False)` 就是这个约定的体现。

**`merge_profile` 先翻译再展开。** 步骤是「对每个输入单独跑 `_translate_legacy_profile_keys`，然后按顺序 dict 展开」。这个顺序不可交换：若先展开再翻译，一个 override 里的旧键名可能与 base 里的新键名共存，翻译时反而让 base 的值赢，违背「override 覆盖」的直觉。

**`default_structured_output_mode` 是 profile 默认值影响行为的典型例子。** 默认 `'tool'`，但 `Model._default_structured_output_mode` 加了一条特例：当默认是 `tool`、有输出工具、模型支持原生 schema、且强制工具调用会关闭思考时，改用 `native`。这条特例说明 profile 提供的是**默认**而不是**规则**，最终决定权在模型层。

**`model_profile` 是静态方法而 `_configured_model_profile` 是实例方法。** 静态版按模型名查表；实例版让「构造时带了 profile 的 provider」（如 `OpenAICompatibleProvider`）能返回自己配置的那份。模型层用后者，所以自定义 provider 可以按实例而非按名字配置能力。

**provider 的客户端生命周期有两层意外处理。** `_moved_to_another_event_loop` 检测「上次用的 loop 现在没在跑」，`__aenter__` 在锁内 `_refresh_http_client` 并计数。文档说明：在 `Agent` 上下文管理器里这些自动处理；不在里面时，新事件循环上的运行会拿到新客户端。这是为了适配「同一个 provider 对象被跨 loop 复用」这种在 asyncio 里本应避免但实际常见的用法。

## 关键取舍

**源码事实：** provider 的 `name` 参与历史重放。改一个已发布 provider 的名字会让旧历史无法被正确识别思考块与原生工具。这是一个**不能靠 semver 掩盖**的兼容性约束。

**源码事实：** 无状态加密数据按系统判、服务端状态 ID 按端点判。两条路分开，使「同 API 不同端点」与「不同 provider 同系统」两种情形各有正确结果。

**源码事实：** `compatible_systems` 是单向声明。方向写反会导致运行期验签失败。

**源码事实：** profile 全字段可选，默认值在访问点应用。所以「这个模型支持什么」不能只看 profile 里有什么，要看读的地方给了什么默认。

**源码事实：** `merge_profile` 先逐输入翻译旧键名再展开，顺序不可换。

**源码事实：** profile 的 `default_structured_output_mode` 只是默认，模型层可以按「强制工具调用是否关闭思考」改写它。

**架构推论：** 这套 provider/profile 分层的目的，是让「新增一个供应商」与「修正一个模型的特性」成为两件独立且都很小的事：前者写一个 `Provider` 子类（认证 + 端点 + 客户端），后者改一个 dict。代价是**信息被切成两处**——一个模型的行为由它的 provider 与 profile 共同决定，排查「为什么这个模型不支持 X」时要在两处查。这也是为什么源码里反复强调「能力标志放 profile、供应商 API 行为放 `Provider.model_profile()`」：边界不清会让这类问题无法定位。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/providers/__init__.py` | `Provider` | provider 基类：名字、端点、客户端、canonical system |
| `pydantic_ai_slim/pydantic_ai/providers/__init__.py` | `Provider.name` | 参与历史重放的 provider 标识 |
| `pydantic_ai_slim/pydantic_ai/providers/__init__.py` | `Provider.canonical_system` | 背后的真实服务，判无状态数据能否回传 |
| `pydantic_ai_slim/pydantic_ai/providers/__init__.py` | `Provider.compatible_systems` | 单向声明还接受哪些系统的数据 |
| `pydantic_ai_slim/pydantic_ai/providers/__init__.py` | `Provider.shares_server_state_with` | 按端点判服务端状态能否查回 |
| `pydantic_ai_slim/pydantic_ai/providers/__init__.py` | `infer_provider_class` | 名字到 provider 类的 match 表 |
| `pydantic_ai_slim/pydantic_ai/providers/__init__.py` | `infer_provider` | 名字到实例，含 `gateway/` 前缀转发 |
| `pydantic_ai_slim/pydantic_ai/profiles/__init__.py` | `ModelProfile` | 全可选的能力表，缺省即默认 |
| `pydantic_ai_slim/pydantic_ai/profiles/__init__.py` | `merge_profile` | 逐输入翻译旧键名后按序展开 |
| `pydantic_ai_slim/pydantic_ai/profiles/__init__.py` | `default_structured_output_mode` | 输出通道的 profile 级默认 |
| `pydantic_ai_slim/pydantic_ai/profiles/__init__.py` | `prompt_cache_outlook` | 按历史时间戳预测缓存冷热 |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `Model.accepts_server_state` | 服务端状态回传判定的消费点 |

## 参考

- [pydantic-ai 源码：providers/__init__.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/providers/__init__.py)
- [pydantic-ai 源码：profiles/__init__.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/profiles/__init__.py)
- [Pydantic AI 文档：模型档案](https://ai.pydantic.dev/api/profiles/)

## 与主核 / AI 的关系

provider 与 profile 的切分，对应主核里「介质管理器」与「表统计信息」的分工：前者管怎么读写（连接、认证、路径），后者管这份数据有什么先验特性（行数、分布、是否可并行）。主核的优化器读统计信息做决策，不直接问介质；这里的模型层读 profile 做决策，不直接问 provider 的实现细节。

对 agent 工程的直接价值有三条。其一，**接一个兼容 OpenAI 的供应商，写 provider 比改模型类划算**——只要它真的走同一套 wire 协议，能力差异应当表达为 profile 覆盖而不是新模型类。其二，**无状态加密数据与服务端 ID 的回传规则不同**，跨端点/跨网关时会出现「推理能带回去但 `previous_response_id` 不能用」这种看似矛盾的现象，这不是 bug 而是两条判定的结论。其三，**改 provider 的 `name` 是破坏性变更**。它写进了历史，任何需要长期保存消息的场景都不能随便重命名。
