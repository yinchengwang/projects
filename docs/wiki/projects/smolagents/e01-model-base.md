---
project: smolagents
feature: model-base
title: 模型基类与消息协议
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/models.py
    symbols: [Model, ChatMessage, get_tool_call_from_text]
  - path: src/smolagents/models.py
    symbols: [remove_content_after_stop_sequences, get_clean_message_list]
---

# 模型基类与消息协议

## 一句话职责

定义框架内部唯一的消息形状 `ChatMessage`，以及所有模型实现共享的「消息清洗 → 参数合并 → 调用」三段流程，让不同厂商的 API 差异全部收敛在 `generate` 一个方法里。

## 机制是什么

`ChatMessage` 是个五字段的 dataclass：`role`（五值枚举）、`content`（字符串或内容块列表）、`tool_calls`、`raw`（厂商原始响应）、`token_usage`。

`MessageRole` 有五个值：`user`、`assistant`、`system`、`tool-call`、`tool-response`。后两个不是任何厂商协议里的角色——它们是框架自造的中间角色，用来表示「模型要求调用工具」与「工具返回了结果」这两类语义。之所以自造，是因为不同厂商对这两件事的表达完全不同：有的用独立的 tool 角色，有的塞进 user 消息，有的用 assistant 消息里的结构化字段。框架统一成两个自定义角色，再由各模型实现按需转换。

`Model` 是基类，`generate` 是唯一必须实现的方法。基类提供四件共享能力：`_prepare_completion_kwargs`（把消息与各种参数合成一次调用的 kwargs）、`parse_tool_calls`（把文本形式的调用解析成结构化）、`to_dict`/`from_dict`（配置往返）、`supports_stop_parameter`（该模型是否支持 stop 参数）。

## 怎么实现的

**消息清洗与合并。** `get_clean_message_list` 是每次调用前的必经步骤，做四件事：

1. **角色转换**。传入 `role_conversions` 映射，默认是模块级的 `tool_role_conversions`（把 `tool-call` 映射成 `assistant`、`tool-response` 映射成 `user`）。所以框架内部的两个自造角色在发给厂商前会被压回标准角色。
2. **图片编码**。内容块里 `type == "image"` 的元素，按 `convert_images_to_image_urls` 决定是编成 base64 字段还是 `image_url` 形状。若 `flatten_messages_as_text` 为真则直接断言失败——纯文本模式下不能带图。
3. **相邻同角色合并**。若当前消息与输出列表最后一条角色相同，就合并内容而不是新增一条。文本与文本之间用换行拼接，块与块之间若是相邻文本块也合并。这个合并是必须的：很多厂商不接受连续两条同角色消息。
4. **深拷贝入参**。函数开头 `deepcopy(message_list)`，注释写「避免修改原列表」。因为后续要改 `element` 与 `message.role`，不拷贝会污染调用方的记忆对象。

**参数合并的三层优先级。** `_prepare_completion_kwargs` 的 docstring 直接列了优先级：`self.kwargs`（模型默认）> 显式传入的 kwargs > 具体参数（stop、response_format 等）。实现顺序正好相反：先放具体参数，再 `update(kwargs)`，最后遍历 `self.kwargs` 逐个覆盖。

`self.kwargs` 里有个特殊哨兵 `REMOVE_PARAMETER`：值为它时执行 `completion_kwargs.pop(name, None)` 而不是赋值。这让「模型级默认里显式禁用某个参数」成为可能——比如某个部署不接受 `temperature`，可以在构造时传 `temperature=REMOVE_PARAMETER` 把它从最终请求里删掉，而不是传 `None`（`None` 会被发出去）。

**stop 参数的能力探测。** `supports_stop_parameter` 是个 property，转到模块函数 `supports_stop_parameter(model_id)`。后者先用正则匹配两组：OpenAI 的 `o3`（除 `o3-mini`）、`o4`、`gpt-5` 系列，以及 `grok-` 系列。命中就返回 `False`。

这个函数的存在说明「stop 不是所有模型都支持」是必须显式处理的差异。`_prepare_completion_kwargs` 里因此写成 `if stop_sequences is not None and self.supports_stop_parameter`——不支持就不发这个参数。

**stop 的客户端兜底。** 但即便不发，语义仍要实现：模型可能越过停止词继续生成。所以 `remove_content_after_stop_sequences` 在客户端做一次截断——对每个停止词 `split` 后取第一段。注释解释了两处边界：`content` 可能为 `None`（纯工具调用响应），此时直接返回。

注意它的实现是**逐个停止词依次截断**，而不是找最早位置。因为每轮截断后剩下的部分很短，逐次 `split` 与找最小位置在这类场景下结果相同，代码更简单。

**文本调用解析。** `get_tool_call_from_text(text, tool_name_key, tool_arguments_key)` 服务那些「不返回结构化 tool_calls、只在文本里写 JSON」的模型。它先 `parse_json_blob` 提取 JSON，再按可配置的键名取工具名（缺键时报错并把实际有的键列出来），取参数（若是字符串则再尝试解析成 JSON），最后造一个带 `uuid4` id 的 `ChatMessageToolCall`。

两个键名做成 `Model` 的构造参数（默认 `"name"` 与 `"arguments"`），是因为不同模型的提示模板里这两个字段叫法不同。`MLXModel` 的 docstring 就写「这个键通常能在模型的 chat template 里找到」。

**`parse_tool_calls` 的补全。** 基类方法先强行把 `role` 改成 `assistant`（注释：「需要时覆盖角色」），再判：若没有结构化 `tool_calls`，就断言 `content` 非空并从文本解析；最后逐个把 `arguments` 从字符串解析成 dict。

所以「文本协议」与「原生函数调用」在消费侧是同一种形状——上层不必知道模型是哪种。

**`ChatMessage` 往返的两处特殊处理。** `model_dump_json` 走 `get_dict_from_nested_dataclasses(self, ignore_key="raw")`——**丢掉 `raw`**。`raw` 是厂商原始响应对象，通常不可 JSON 序列化且体积大，记忆持久化时不应包含它。

`__post_init__` 里对 `tool_calls` 逐个调 `_coerce_tool_call`，把 dict 或 pydantic 模型统一成 `ChatMessageToolCall`。这让「从 API 直接拿到的 tool_calls」与「从 JSON 反序列化的 tool_calls」在对象层同形。

**配置往返的凭据处理。** `to_dict` 把 `self.kwargs` 与 `model_id` 摊开，再补九个常见属性（temperature、max_tokens、provider、api_base 等，用 `hasattr` 判断避免强制要求）。然后对 `token` 与 `api_key` 两个名字：**不导出，只打一行提示**要求手动导出。

这是明确的取舍：配置导出常被用于序列化模型定义（例如生成可分享的 agent 配置），若静默带上密钥就是泄漏。

## 关键取舍

**源码事实：** `tool-call` 与 `tool-response` 是两个框架自造的角色，任何厂商都不认。它们在 `get_clean_message_list` 里通过 `role_conversions` 压回标准角色，所以「内部语义」与「线上协议」之间隔着一层显式映射。

**源码事实：** `REMOVE_PARAMETER` 与「传 `None`」语义不同：前者把键从请求里删掉，后者把 `null` 发出去。两者在多数 API 上行为不同。

**源码事实：** `supports_stop_parameter` 是**模型名正则匹配**，不是从 API 能力查询。所以新模型发布时若名字不匹配任何已知模式，会默认被当作支持 stop——这是保守方向相反的默认（乐观假设）。

**源码事实：** `remove_content_after_stop_sequences` 只在 `supports_stop_parameter` 为假时被调用，是「服务端不支持就客户端补」的兜底，不替代服务端截断。

**源码事实：** `ChatMessage.model_dump_json` 排除 `raw`，但 `dict()` 不排除。所以「给外部看的序列化」与「内部调试用的字典」是两个不同精度。

**架构推论：** 把消息归一成一套内部形状，代价是每个模型实现都要写「内部形状 → 厂商形状」的转换；收益是上层（agent、记忆、流式聚合）只认一种形状。若不做这层归一，每接入一个厂商就要在 agent 主循环里加一个分支——那才是真正的成本爆炸点。

**架构推论：** 「自造角色 + 转换表」比「要求厂商角色一致」现实得多。厂商角色集合不收敛（OpenAI 有 `tool`，Anthropic 用 content block，本地模型模板各异），只能由框架定义一个超集再往下压。

**架构推论：** `supports_stop_parameter` 用正则而非配置，反映了一个现实：模型能力元数据没有统一来源。代价是新模型的支持情况要靠改代码维护；好处是不依赖任何外部服务就能判断。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/models.py` | `Model` | 模型基类：参数合并、调用解析、配置往返 |
| `src/smolagents/models.py` | `Model._prepare_completion_kwargs` | 三层优先级合并参数，支持删除哨兵 |
| `src/smolagents/models.py` | `Model.parse_tool_calls` | 无结构化调用时从文本补全并解析参数 |
| `src/smolagents/models.py` | `Model.supports_stop_parameter` | 按模型名正则判断是否支持 stop |
| `src/smolagents/models.py` | `Model.to_dict` | 导出配置，跳过 `token` 与 `api_key` |
| `src/smolagents/models.py` | `ChatMessage` | 五字段内部消息，含厂商原始响应与用量 |
| `src/smolagents/models.py` | `ChatMessage.model_dump_json` | 序列化时排除 `raw` |
| `src/smolagents/models.py` | `MessageRole` | 五值角色，含两个框架自造值 |
| `src/smolagents/models.py` | `get_tool_call_from_text` | 按可配置键名从文本提取工具调用 |
| `src/smolagents/models.py` | `get_clean_message_list` | 角色转换、图片编码、相邻同角色合并 |
| `src/smolagents/models.py` | `remove_content_after_stop_sequences` | 服务端不支持 stop 时的客户端截断 |
| `src/smolagents/models.py` | `REMOVE_PARAMETER` | 从请求参数中删除某键的哨兵值 |

## 参考

- [smolagents 源码：models.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/models.py)
- [smolagents 文档：Models](https://huggingface.co/docs/smolagents/main/en/reference/models)
- [OpenAI 文档：Chat Completions 消息角色](https://platform.openai.com/docs/api-reference/chat)

## 与主核 / AI 的关系

主核里最接近的是「客户端协议的消息归一」：同一条查询在文本协议与二进制协议里形状不同，解析后落到同一棵语法树。这里的 `ChatMessage` 就是那棵语法树，`get_clean_message_list` 是出方向的反向转换。差别在于主核的两端都由同一方实现、可共享语法；这里的光谱两端是不同厂商的 HTTP API，没有共同子集可用。

与 CrewAI 的模型层相比，CrewAI 把消息直接交给各 provider SDK 的原生类型（`LLMMessage`），转换发生在 provider 适配器内部；smolagents 在框架层定义了自己的 `ChatMessage` 并强制所有实现转换。前者适配成本低但上层要处理多种消息类型，后者上层统一但要写更多转换代码。这页的两个自造角色正是后一种选择的直接产物。

对 AI 工程最实用的一条是 `REMOVE_PARAMETER` 这个哨兵。多模型统一配置时，最常见的冲突是「某个模型的部署不接受某个参数」。传 `None` 往往不解决问题——很多 API 把 `null` 当作显式值而不是「未设置」，于是报错或产生意外行为。提供一个「删除这个键」的显式语义，比让用户在 `None` 与「不传」之间猜要可靠得多。

另一条是 `raw` 的双重处理：内部 `dict()` 保留、序列化排除。任何「对象带一个厂商原始响应」的设计都要做这个区分，否则记忆持久化会带上不可反序列化的 SDK 对象，而调试时又看不到原始详情。一句 `ignore_key` 就同时解决了两个问题。
