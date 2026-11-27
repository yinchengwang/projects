---
project: crewai
feature: llm-factory
title: 模型创建与环境回退
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/utilities/llm_utils.py
    symbols: [create_llm, create_llm_like, _llm_via_environment_or_fallback]
---

# 模型创建与环境回退

## 一句话职责

把用户给的四种形态（模型对象、模型名字符串、配置字典、任意鸭子类型对象）统一建成一个模型实例；什么都没给时按环境变量回退；还提供一条「按已有模型的样子换一个模型」的搬运规则。

## 机制是什么

`create_llm` 是一个四分支的类型分派：

- 已是 `LLM` 或 `BaseLLM` → 原样返回，不做任何重建。
- 字符串 → `LLM(model=...)`，失败记日志并重抛。
- 字典 → 从 `model`/`model_name`/`deployment_name` 三个键里取一个作为模型名（都缺则 `ValueError`），把另两个弹掉，其余键原样传进 `LLM(**params)`。
- `None` → `_llm_via_environment_or_fallback()`。
- 其他对象 → 用 `getattr` 依次探 `model`、`model_name`、`deployment_name`，都没有就 `str(obj)`；再探 `temperature`、`max_tokens`、`logprobs`、`timeout`、`api_key`、`base_url`、`api_base` 七个属性，组一个 `LLM(...)`。

所以这个函数的实质是「不管你给我什么，我都尽量弄出一个 LLM」。

`create_llm_like(model, base)` 是另一件事：给一个新模型名，按 `base` 的配置把它建出来。核心是两条设置清单——`GENERATION_SETTINGS`（19 项，温度、top_p、max_tokens、stop、seed、timeout、惩罚项、logprobs、reasoning_effort、callbacks、stream 等）**跨 provider 搬运**；`PROVIDER_SETTINGS`（40 项左右，凭据、端点、客户端构造、provider 私有设置）**只在同一 provider 内搬运**。

## 怎么实现的

**环境回退的顺序。** `_llm_via_environment_or_fallback` 先定模型名：`MODEL` → `MODEL_NAME` → `OPENAI_MODEL_NAME` → `DEFAULT_LLM_MODEL`（四级回退）。然后声明一整排 `None` 的参数占位（温度、max_tokens、logprobs、timeout、api_key、api_version、惩罚项、top_p、n、stop、logit_bias、response_format、seed、top_logprobs、callbacks），再单独处理 base_url 与 api_base 的三级回退：`BASE_URL` → `OPENAI_API_BASE` → `OPENAI_BASE_URL`；`api_base` 则是 `API_BASE` → `AZURE_API_BASE`；两者**互相补齐**——只有一个有值时同步到另一个。

接着组 `llm_params` 字典，按 provider 从 `ENV_VARS` 里查该 provider 的环境变量清单：每个条目若有 `key_name` 且不在三个 AWS 排除项里，就把它在环境里的值经 `_normalize_key_name` 归一成 litellm 认识的名字塞进去；若条目是带 `default: True` 的字典，则把它的其它键值**当作默认参数**填入。

最后 `{k: v for k, v in llm_params.items() if v is not None}` 剔掉 None，再 `LLM(**llm_params)`。异常记日志并重抛。

**`_normalize_key_name` 的模糊匹配。** 它遍历 `LITELLM_PARAMS` 清单，只要某个参数名**是**环境变量名（转小写后）的子串就返回它。所以 `AWS_SECRET_ACCESS_KEY` 能被映射到 `aws_secret_access_key` 这类参数——靠的是子串而非精确相等。

**搬运规则的实现。** `create_llm_like` 全程包在 `building_mapped_model()` 上下文里，最后 `mark_mapped(...)` 标记产物。它先 `LLM._resolve_route(model, _configured_settings(base, _ENDPOINT_NAMES))` 解析目标路由——**用声明的端点参与解析**，因为一个未知的 `openai/` 模型到底去哪，取决于声明的 endpoint（可能是自建的兼容端点）。

然后 `carried = _configured_settings(base, GENERATION_SETTINGS)` 收生成参数；`if _same_provider(base, route): carried.update(_configured_settings(base, PROVIDER_SETTINGS))` 才收凭据与端点。最后 `_build_like(model, carried, type(base), through_litellm)`。

`through_litellm` 的判断是 `type(base) is LLM and base.is_litellm`——若原模型走的是 LiteLLM，新模型也走 LiteLLM，因为「回调与额外 kwargs 是为那个环境写的」。

**`_build_like` 的目标选择与过滤。** 目标类取 `route.native_class or LLM`；若 `through_litellm` 则强制成 `LLM` 并把 `is_litellm` 塞进 carried。然后按目标类的 `model_fields` 白名单过滤参数——**目标类声明了该字段才带过去**（`is_litellm` 例外）。若目标类与原类不同，踢掉 `additional_params`（那是原类自己的额外 kwargs）。再调 `_carry_cap_under_the_targets_name` 把输出 token 上限改成目标类用的名字，最后若目标是 anthropic 且带了 `temperature` 就踢掉 `top_p`——因为当前 Claude 模型不接受两者同时出现。

**`_same_provider` 的三层判断。** 目标路由有原生类时：类型恰好等于目标类 → 若该类同时服务多个 provider（只有 `OpenAICompatibleCompletion` 是这样），还要 provider 字符串也相等；否则类型是 `LLM` 且 `LLM._get_native_provider(base.provider)` 得到目标类 → 同上还要 provider 相等。目标路由没有原生类时 → `type(base) is LLM and route.provider == base.provider`。

所以「同 provider」的判定在 `OpenAICompatibleCompletion` 这里破例为「还要比 provider 字符串」，原因写在注释里：一个类服务所有 OpenAI 兼容 provider（OpenRouter、DeepSeek、Ollama、vLLM…），每个都有自己的端点与密钥。

**`_configured_settings` 与 `_derived_from_model`。** 前者按名字清单从 `base` 上取值，跳过 `None` 与空容器；后者判定某设置是不是「provider 从模型推出来的」而不是调用方给的——`_MODEL_DERIVED` 只列了 `max_tokens`（注释说 Anthropic 会把 `max_tokens` 设成模型的输出上限）。所以推导出来的值不搬运。

**Azure 部署路径的剥离。** `_AZURE_DEPLOYMENT_PATH` 是 `"/openai/deployments/"`，`create_llm_from_kwargs_like` 里会把 endpoint 在这个位置切开，只保留资源根——因为 Azure 在构造时把声明的模型名烘进了端点，新模型必须用自己的部署。

**角色覆盖。** `overlay_llm_for(role, declared)` 在 `llm_overlay` 生效时决定某角色该用哪个模型：角色键优先（且用调用方原始声明构造，而不是用已被模型键映射过的版本），否则按模型键匹配，否则用原模型。

## 关键取舍

**源码事实：** `create_llm` 对已是 `LLM`/`BaseLLM` 的输入**原样返回，不做任何补全**。所以传一个 `BaseLLM` 子类实例与传一个模型名字符串，走的是两条完全不同的路径，前者不经过任何工厂逻辑。

**源码事实：** 字典分支只认 `model`/`model_name`/`deployment_name` 三个键名。写 `{"model_id": "gpt-4o"}` 会因找不到模型名而 `ValueError`，而不是静默用默认模型。

**源码事实：** 鸭子类型分支会 `str(obj)` 兜底当模型名。所以传一个随便的对象也能构造出一个「模型名是对象 repr」的 `LLM`，直到真正调用时才失败。

**源码事实：** `PROVIDER_SETTINGS` 只在同 provider 时搬运，注释解释了必要性：跨 provider 搬运会把 OpenAI 的密钥与代理 URL 送给 Anthropic。这是安全边界，不是优化。

**源码事实：** `additional_params` 只在同类之间搬运。理由写在函数注释里：「那是类自己的额外 kwargs」。跨类搬运会让目标类收到一堆它不认识的键。

**源码事实：** 「目标的字段类型拒绝某个设置时，警告而不是抛错」。docstring 明说理由：换模型发生在 kickoff 内部，不该因为一个参数的形状不兼容就让整次运行失败。

**架构推论：** 这套工厂的复杂度集中在「把一个模型的配置搬到另一个模型上」这件事上，而不是「建一个模型」。这说明真正的难点不是构造，而是**语义对齐**：哪些设置跨 provider 有意义（温度、stop），哪些只属于特定 provider（密钥、端点、安全设置、工具声明），哪些是 provider 从模型推出来的（输出上限）。三条清单（`GENERATION_SETTINGS`、`PROVIDER_SETTINGS`、`_MODEL_DERIVED`）就是这份语义地图。

**架构推论：** 环境回退这段代码的形态（先声明一整排 `None` 变量、再逐个从环境里填、最后剔掉 None）说明它的目标是「环境里有什么就用什么」，代价是**默认值与「没设置」不可区分**。一个显式设成 `0` 的温度会与「未设置」走同一条路；一个显式设成空列表的 stop 也会被剔掉。

**架构推论：** 路由解析要用端点参与（`LLM._resolve_route(model, _configured_settings(base, _ENDPOINT_NAMES))`），揭示了「模型名到 provider 的映射不是纯函数」——同一个 `openai/xxx` 可能指向官方端点，也可能指向自建兼容端点。这是自建部署普遍存在之后必然出现的复杂度。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `create_llm` | 四分支类型分派，统一建出模型实例 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `_llm_via_environment_or_fallback` | 环境变量四级回退与 provider 专用变量注入 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `_normalize_key_name` | 环境变量名到 litellm 参数名的子串映射 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `create_llm_like` | 按已有模型配置新模型，含跨 provider 搬运规则 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `GENERATION_SETTINGS` | 跨 provider 可搬运的生成参数清单 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `PROVIDER_SETTINGS` | 仅同 provider 搬运的凭据与端点清单 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `_build_like` | 按目标类字段白名单过滤并修正参数名 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `_same_provider` | 三层判定，含多 provider 类的破例 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `_serves_several_providers` | 判定某类是否服务多个 provider |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `_carry_cap_under_the_targets_name` | 输出 token 上限按目标类命名规则改名 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `_derived_from_model` | 排除 provider 从模型推导出的值 |
| `lib/crewai/src/crewai/utilities/llm_utils.py` | `overlay_llm_for` | 角色键优先的模型覆盖解析 |

## 参考

- [CrewAI 源码：utilities/llm_utils.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/utilities/llm_utils.py)
- [CrewAI 文档：Agents 与模型配置](https://docs.crewai.com/en/concepts/agents)
- [LiteLLM 文档：Provider 参数](https://docs.litellm.ai/docs/providers)

## 与主核 / AI 的关系

主核里最接近的类比是「按连接串发明连接对象」：连接串可以只给数据库名走默认配置，也可以带上用户、主机、参数覆盖一切；主机与凭据不能跨不同的服务复用，而查询级参数（超时、隔离级别）可以。这里的 `GENERATION_SETTINGS` 与 `PROVIDER_SETTINGS` 划的正是这条线——**跨边界只搬语义一致的，不搬环境相关的**。

与 AutoGen 相比，AutoGen 根本不提供「按已有模型配置新模型」这件事：`ChatCompletionClient` 各实现自己解析自己的参数，要换模型就重新构造一个客户端。CrewAI 的 `create_llm_like` 存在是因为它有 `llm_overlay` 这类「在不改用户代码的前提下替换模型」的需求——一次替换必须尽量保留调用方原本的意图，否则「只是换了个模型」会变成「行为全变了」。

对 AI 工程最实用的一条是那份搬运语义地图。任何做「模型降级/升级/AB 测试」的系统都会遇到同一问题：原模型上调好的温度、stop 序列、max_tokens 该怎么带到新模型上？答案是分三类处理——语义一致的直接搬，环境相关的只在同类间搬，provider 自动推导的丢弃。把这三类写成显式清单（而不是散落在条件分支里），是这套实现最值得借鉴的部分。

另一条是那条 `top_p` 与 `temperature` 互斥的特例。它是「provider 有个真实约束，而通用搬运逻辑不知道」的典型：如果不加这条判断，换到 Claude 会得到一个 API 报错。这类特例只能靠逐 provider 积累，所以任何跨 provider 的配置搬运都要预留「目标侧修正」的钩子——这里的 `_carry_cap_under_the_targets_name` 与 anthropic 判断就是那个钩子的位置。

最后是 Azure 部署路径的剥离。它提醒我们「端点」不总是纯粹的基地址：有些 provider 把资源名与模型名编码进 URL。搬运端点时必须知道哪一段是可复用的（资源根）、哪一段必须重算（部署名），否则新模型会指向旧部署。
