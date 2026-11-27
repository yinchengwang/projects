---
project: openai-agents
feature: multi-provider
title: 前缀路由与多 provider 解析
page_type: feature
schema_version: 1
refs:
  - path: src/agents/models/multi_provider.py
    symbols: [MultiProvider, MultiProviderMap]
---

# 前缀路由与多 provider 解析

## 一句话职责

用一个「模型名前缀」出发把名字路由到不同的 provider：显式映射优先，其次两个内建的 fallback 前缀，最后才看 OpenAI 与未知前缀两种模式——并允许调用方把 `openai/xxx` 这类歧义前缀解释成「别名」或「字面模型 id」。

## 机制是什么

`MultiProvider` 是 `ModelProvider` 的实现，它自己不当工厂的终点，而是当**分派器**：拿到模型名字符串，切出前缀，决定交给哪个子 provider。它持有一个 `provider_map`（`MultiProviderMap`，可空）、一个总是存在的 `openai_provider`、以及一个惰性填充的 `_fallback_providers` 字典。

`MultiProviderMap` 是极简的「前缀 → provider」字典包装：`has_prefix`、`get_provider`、`add_provider`、`remove_provider`，以及 `get_mapping`/`set_mapping` 一对整体读写。`get_mapping` 返回的是拷贝，注释写得很清楚——避免调用方持着内部字典。

路由的优先级在 `_resolve_prefixed_model` 里定死，共四级：显式映射 → `litellm`/`any-llm` 内建 fallback → `openai` 前缀 → 未知前缀。这个顺序是有理由的：注释说「显式 provider_map 是最不令人意外的路由机制，所以它总是优先于内建的 OpenAI 别名与未知前缀回退行为」。

两个模式参数决定了边界行为：`openai_prefix_mode`（`"alias"` 或 `"model_id"`）与 `unknown_prefix_mode`（`"error"` 或 `"model_id"`）。两个都在构造时用静态方法校验，非法值直接 `UserError`。

## 怎么实现的

**前缀切分。** `_get_prefix_and_model_name` 只切一刀：`model_name.split("/", 1)`。所以 `litellm/openai/gpt-4.1` 切出前缀 `litellm` 与模型 `openai/gpt-4.1`——嵌套前缀不会被递归解析，剩下的部分原样交给下游 provider。这是「前缀只吃一层」的明确选择。

**无前缀与空名。** `get_model` 先判 `model_name is None` 直接转发给 OpenAI provider（同 `get_model(None)`），再判无前缀则转发 `stripped_model_name`。源码注释解释了为什么无前缀总是走 OpenAI：那个 provider 自己可以通过 `base_url` 指向任何 OpenAI 兼容端点。所以「用别的后端」不需要加前缀，只需要配端点。

**显式映射优先。** `_resolve_prefixed_model` 第一步查 `provider_map.get_provider(prefix)`，命中就直接返回该 provider 与**剥掉前缀后的**模型名。注意它返回的是 `stripped_model_name` 而不是原名——显式映射的语义是「这个前缀是我的 provider，剩下的名字是它要的模型名」。

**内建 fallback 的两个前缀。** `litellm` 与 `any-llm` 命中时走 `_get_fallback_provider`，它先查缓存字典，没有才 `_create_fallback_provider` 并缓存。后者的实现是**延迟导入**：`from ..extensions.models.litellm_provider import LitellmProvider`——这是必要的，因为 litellm 与 any-llm 都是可选依赖，顶层导入会让不装它们的人连 `import agents` 都失败。其余前缀在此处 `UserError("Unknown prefix")`。

**`openai` 前缀的两义性。** 这是整个模块存在的核心理由。docstring 把场景写得很具体：`openai/gpt-4.1` 到底意味着「路由到 OpenAI provider 并使用模型 `gpt-4.1`」还是「把字面模型 id `openai/gpt-4.1` 发给配置好的 OpenAI 兼容端点」？`alias` 模式选前者（剥前缀），`model_id` 模式选后者（保留原名）。两者都交给同一个 `openai_provider`，差别只在传给它的字符串。

**未知前缀的两种处理。** `unknown_prefix_mode == "model_id"` 时，任何未识别前缀都原样交给 OpenAI provider——这是为 `openrouter/openai/gpt-4o` 这类「兼容端点把整串当模型 id」的场景准备的。`"error"` 模式保持历史行为，`UserError`。

**关闭的纵深。** `aclose` 收集三类 provider：`openai_provider`、`provider_map` 里的所有值、`_fallback_providers` 里的所有值。然后按 `id()` 去重（防止同一个 provider 被注册到多个前缀），跳过 `self`（防止递归），逐个 `await aclose()`。异常处理是「记录第一个错误、跑完所有 provider、再抛第一个」——并且 `CancelledError` 直接重抛，不参与「先跑完再抛」的逻辑。

这个「收集—去重—全关—抛首个错误」的顺序有明确意图：一个 provider 关闭失败不该让其余 provider 泄漏。这与 Responses 侧的「先清理再分类」是同一种资源管理取向：**保证全部释放，再报告失败**。

## 关键取舍

**源码事实：** 显式映射返回剥前缀后的名字，而 `openai`/未知前缀在 `model_id` 模式下返回原名。所以「剥不剥前缀」是每个分支各自决定的，不是统一规则。这解释了为什么要用 `_resolve_prefixed_model` 返回 `(provider, name)` 元组，而不是只返回 provider。

**源码事实：** `litellm` 与 `any-llm` 的 fallback 是**内建**的，不需要 `provider_map` 就生效。所以 `provider_map` 不是「必需配置」而是「覆盖与扩展」——它在优先级上高于内建 fallback，所以在映射里放 `litellm` 可以替换掉默认的 LiteLLM provider。

**源码事实：** `MultiProviderMap` 没有「未知键」的保护：`remove_provider` 直接 `del`，键不存在就 `KeyError`；`get_provider` 返回 `None` 而不是抛错。两个方法的失败模式不一致，调用方需要分别处理。

**源码事实：** `aclose` 里 `if provider is self: continue` 这行防的是「有人把 `MultiProvider` 自己注册进了 `provider_map`」，否则会无限递归。这是一个真实存在的误用路径。

**架构推论：** 前缀路由是一种**把命名空间塞进字符串**的设计。它的好处是「换后端不改代码」——同一个 `Agent(model="litellm/openai/gpt-4.1")` 换个前缀就跑在另一条链路上。坏处是模型名不再是不透明标识：它同时承载路由信息与真实模型名，任何一处解析错就会打到错的端点。

**架构推论：** 那两个 `mode` 参数的存在说明前缀语法本身有歧义，而歧义无法靠约定消除。`openai_prefix_mode` 与 `unknown_prefix_mode` 各给两个值，实际上是在承认「同一份配置在两种部署环境里有不同含义」——官方端点用 alias，自建兼容端点用 model_id。把这个选择做成构造参数，比猜要诚实。

**架构推论：** 延迟导入 fallback provider 是可选依赖的标准处理，但这里还有一层：`_create_fallback_provider` 在 `else` 分支里 `UserError("Unknown prefix")`，意味着「前缀存在但对应的可选包没装」会表现为导入错误而非友好提示。这是可改进点，但不影响当前机制的正确性。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/models/multi_provider.py` | `MultiProvider` | 按前缀分派到子 provider 的路由器 |
| `src/agents/models/multi_provider.py` | `MultiProviderMap` | 前缀到 provider 的字典包装，返回拷贝 |
| `src/agents/models/multi_provider.py` | `MultiProvider.get_model` | 无前缀直通 OpenAI，有前缀走解析 |
| `src/agents/models/multi_provider.py` | `MultiProvider._get_prefix_and_model_name` | 只切一层的前缀切分 |
| `src/agents/models/multi_provider.py` | `MultiProvider._resolve_prefixed_model` | 四级优先级与两种前缀模式 |
| `src/agents/models/multi_provider.py` | `MultiProvider._create_fallback_provider` | 延迟导入 litellm / any-llm |
| `src/agents/models/multi_provider.py` | `MultiProvider._get_fallback_provider` | fallback provider 惰性缓存 |
| `src/agents/models/multi_provider.py` | `MultiProvider._validate_openai_prefix_mode` | 模式参数构造期校验 |
| `src/agents/models/multi_provider.py` | `MultiProvider.aclose` | 收集、去重、全关、抛首个错误 |

## 参考

- [openai-agents-python 源码：models/multi_provider.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/models/multi_provider.py)
- [Agents SDK 文档：Models 与多 provider](https://openai.github.io/openai-agents-python/models/)
- [LiteLLM 文档：支持的 provider 列表](https://docs.litellm.ai/docs/providers)

## 与主核 / AI 的关系

主核里最接近的是「连接串 scheme 路由」：`postgresql://`、`file:`、`redis://` 这类前缀决定用哪个后端适配器，前缀之后的部分交给该适配器解释。这里的路由结构与之一致，但也有实质差别——主核的 scheme 是封闭集合，写错就报错；这里多了一个 `unknown_prefix_mode="model_id"` 的逃生口，让未知前缀能原样传下去。多出这条是因为 OpenAI 兼容端点普遍接受「把命名空间塞进模型 id」这种用法。

与 AutoGen 的对比更能说明差异：AutoGen 的模型解析靠 `ComponentModel` 的 provider 字符串（一个完整的点号路径），走 import 加白名单校验；这里靠模型名里的一段前缀，走映射表加内建 fallback。前者是**配置级路由**（配置里写清用哪个类），后者是**字符串级路由**（名字里暗示用哪个 provider）。CrewAI 的做法介于两者之间：它有 `create_llm_like` 这类「按已有模型配置新模型」的工厂，也认 `openai/`、`litellm/` 前缀。

对 AI 工程最实用的一条是那两个 mode 参数的思路。任何「前缀既是路由也是名字一部分」的设计都会遇到同样的歧义，而歧义不能靠文档解决——必须让用户在构造时表达选择。默认值取历史行为（`alias` / `error`），新行为作为显式开关，这个顺序保证了向后兼容。

另一条是 `aclose` 里那套「收集—去重—全关—再抛首个错误」。容器类对象持有多个子对象时的关闭逻辑，最容易写成「遍历到第一个失败就返回」，导致后面的资源泄漏。正确做法是如这里的代码：先跑完全部、记录第一个错误、最后抛出。代价是调用方拿不到全部错误（只拿第一个），但资源不泄漏比错误完整更重要。
