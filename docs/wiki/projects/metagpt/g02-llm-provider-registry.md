---
project: metagpt
feature: llm-provider-registry
title: 提供方注册与选择
page_type: feature
schema_version: 1
refs:
  - path: metagpt/provider/llm_provider_registry.py
    symbols: [LLMProviderRegistry]
---

# 提供方注册与选择

## 一句话职责

注册表把「配置里写的提供方枚举值」映射到「该由哪个类来实例化」，并在实例化后补一层微调：把配置里显式设定的 `use_system_prompt` 覆盖到实例默认值上。

## 机制是什么

全仓有二十多个 `BaseLLM` 子类，每个负责一家厂商。运行期只需要回答一个问题：拿到一份 `LLMConfig`，该 `new` 哪个类？

答案是两级查表：配置里的 `api_type` 是 `LLMType` 枚举，注册表的 `providers` 字典以该枚举为键、以类为值。查不到就直接 `KeyError`——没有兜底、没有默认实现。

注册不是手工调用的，而是在模块导入时由装饰器完成：每个 provider 文件在类定义上方写 `@register_provider(...)`，装饰器把类写进全局注册表。因此「有哪些提供方可用」等价于「哪些模块被导入过」。

## 怎么实现的

**注册表的三个成员。** `LLMProviderRegistry` 只有一个字典 `providers` 和两个方法：`register(key, provider_cls)` 写入、`get_provider(enum)` 读取。`get_provider` 用的是 `self.providers[enum]` 而不是 `.get()`，所以未知枚举抛 `KeyError` 而不是返回 `None`。

**装饰器支持一对多。** `register_provider(keys)` 接受单个枚举或枚举列表，返回一个类装饰器：遍历 keys 逐个 `register`，最后原样返回类。一对多映射在这里被大量使用——`OpenAILLM` 一个类同时注册给 `OPENAI`、`FIREWORKS`、`OPEN_LLM`、`MOONSHOT`、`MISTRAL`、`YI`、`OPEN_ROUTER`、`DEEPSEEK`、`SILICONFLOW`、`OPENROUTER`、`LLAMA_API` 十一个枚举值。这是「同一套 OpenAI 兼容协议、只是 base_url 与模型名不同」这一现实的直接映射。

**模块级单例。** 文件末尾创建 `LLM_REGISTRY = LLMProviderRegistry()`，它就是这个进程里唯一的注册表。装饰器闭包捕获的也是这个名字，因此所有 provider 模块共享同一张表。

**`create_llm_instance` 的三步。** 这是注册表对外的唯一使用入口：

1. `llm = LLM_REGISTRY.get_provider(config.api_type)(config)`——按枚举取出类并用同一份配置实例化。注意这里把配置对象直接当构造参数传，各子类的 `__init__` 自己去读需要的字段。
2. 若 `llm.use_system_prompt` 为真而 `config.use_system_prompt` 为假，就把实例上的值改成假。源码注释解释了原因：o1 这类模型不应带 system 消息，但 OpenAI provider 的默认 `use_system_prompt` 是 `True`，所以要在实例化之后用配置里的显式值压过去。
3. 返回实例。

这个覆盖是**单向**的：只能把真改成假，不能把假改成真。也就是说「某个 provider 默认不带 system prompt，但配置想让它带」这件事做不到。方向性来自条件的写法（`llm.use_system_prompt and not config.use_system_prompt`），不是笔误——它服务的场景是「默认开、按模型关」。

**与上层门面的关系。** 业务代码几乎不直接碰注册表，而是调 `metagpt.llm.LLM(config=None, context=None)`：给了配置就走 `ctx.llm_with_cost_manager_from_llm_config(config)`，没给就走 `ctx.llm()` 取上下文里的默认实例。上下文内部最终也走到 `create_llm_instance`。因此注册表是「机制」，`LLM()` 是「门面」，上下文是「带缓存的持有者」。

`LLMType` 枚举有一处值得注意的实现：它定义了 `__missing__(self, key): return self.OPENAI`。这个方法是「按名取值」失败时的钩子（例如 `LLMType["unknown"]`），**不影响**注册表按枚举值查字典的路径。所以「未知提供方回落成 OpenAI」这个意图在运行期的主路径上并不生效——真正会抛 `KeyError`。

## 关键取舍

**源码事实：** 注册表的键是枚举成员，不是字符串。因此「配置里写 `api_type: openai`」能工作，是因为 `LLMConfig` 用 pydantic 把它解析成了 `LLMType.OPENAI`；绕过 pydantic 直接给字符串会查不到。

**源码事实：** `get_provider` 用下标而非 `.get()`，未知提供方抛 `KeyError`。这比「回落成默认提供方」更难排错，但也不会让配置写错时静默用错模型。

**源码事实：** 注册发生在模块导入时。因此「某个 provider 不可用」有两种成因：模块没被导入，或模块导入失败（例如缺该厂商 SDK）。后者通常是惰性导入（`from pymilvus import ...` 放在方法体内）之外的顶层 import 失败，会让整个文件注册不上。

**源码事实：** `use_system_prompt` 的覆盖是单向的，且发生在实例化之后、返回之前。因此每个被 `create_llm_instance` 造出来的实例都带着这次修正，而直接用类构造的实例不会——两条构造路径的行为不一致。

**架构推论：** 一个类注册给多个枚举，意味着「提供方」这个概念的粒度是「协议族」而不是「厂商」。这解释了为什么 `LLMType` 里既有 `OPEN_ROUTER` 又有 `OPENROUTER`（两个枚举值、同一个类），也解释了新增一家 OpenAI 兼容厂商只需要加一个枚举值而不必写新类。

**架构推论：** 注册表是模块级单例、注册靠 import 副作用。这让「加载哪些 provider」由 import 图决定，而不是由配置决定。好处是零配置即用；代价是无法在运行期卸载或替换某个 provider，也无法在同一个进程里对同一枚举用两套实现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/provider/llm_provider_registry.py` | `LLMProviderRegistry` | 枚举到 provider 类的映射表 |
| `metagpt/provider/llm_provider_registry.py` | `LLMProviderRegistry.register` | 写入映射，后注册覆盖先注册 |
| `metagpt/provider/llm_provider_registry.py` | `LLMProviderRegistry.get_provider` | 按枚举取值，未知即 KeyError |
| `metagpt/provider/llm_provider_registry.py` | `register_provider` | 类装饰器，支持一次注册多个枚举 |
| `metagpt/provider/llm_provider_registry.py` | `create_llm_instance` | 实例化 + `use_system_prompt` 单向修正 |
| `metagpt/provider/llm_provider_registry.py` | `LLM_REGISTRY` | 模块级单例注册表 |
| `metagpt/provider/openai_api.py` | `OpenAILLM` | 一个类注册给十一枚举的典型例子 |
| `metagpt/llm.py` | `LLM` | 业务侧门面，转交上下文取实例 |

## 参考

- [MetaGPT 源码：provider/llm_provider_registry.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/provider/llm_provider_registry.py)
- [MetaGPT 文档：配置 LLM](https://docs.deepwisdom.ai/main/en/guide/tutorials/customize_llm.html)
- [MetaGPT 源码：configs/llm_config.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/configs/llm_config.py)

## 与主核 / AI 的关系

「注册表 + 装饰器 + 模块导入副作用」与主核里「存储引擎按类型注册、由启动期注册表决定能打开哪些格式」是同一种模式。差别在兜底策略：主核遇到未知格式通常报明确错误并列出支持列表；这里直接 `KeyError`，把「有哪些可用」这个问题留给读代码的人。另一处差别是主核的注册表可以被配置裁剪（只编译需要的引擎），这里的注册完全由 import 决定。

对 agent 工程的含义：切换模型提供方是「改一个枚举值」的成本，但前提是那个 provider 模块已经被导入过。因此在插件化或多租户部署里，若要支持「运行期按用户配置切换厂商」，必须保证所有可能的 provider 模块都已被导入——这正是上下文与 `LLM()` 门面存在的实际价值：它把「取实例」这件事收敛到一处，方便统一预热。

