---
project: smolagents
feature: local-inference
title: 本地推理模型
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/models.py
    symbols: [VLLMModel, MLXModel]
---

# 本地推理模型

## 一句话职责

把「模型就在本进程里」这件事接入同一套消息协议：复用基类的参数合并与角色转换，但把调用换成直接驱动推理引擎，并在引擎能力不足时自己补上停止词与用量统计。

## 机制是什么

`VLLMModel` 与 `MLXModel` 都继承 `Model`（不是 `ApiModel`），因此**没有限流器和重试器**——本地推理不存在「服务端限流」这个概念。两者都在构造期惰性检查依赖包，缺包时抛出带安装命令的错误（`pip install 'smolagents[vllm]'` / `smolagents[mlx-lm]`），然后加载模型与 tokenizer。

两者的共同套路是：把 `_prepare_completion_kwargs` 的产物拆开，**只取自己需要的部分**，其余参数直接丢弃。所以基类那套为 HTTP 请求设计的参数（`tool_choice`、`response_format` 的嵌套形状）在本地路径上会被单独处理或忽略。

两者的共同差异点有两个：**是否支持结构化输出**、**是否支持视觉输入**。`VLLMModel` 支持前者（借助 vLLM 的 `StructuredOutputsParams`）但不支持视觉（构造时写死 `_is_vlm = False`）；`MLXModel` 两者都不支持（`generate` 里若传 `response_format` 直接 `ValueError`，构造时 `flatten_messages_as_text=True`）。

## 怎么实现的

**VLLM 的提示构造与采样。** `generate` 先用 `_prepare_completion_kwargs` 拼参数，其中显式传 `flatten_messages_as_text=(not self._is_vlm)`。因为 `_is_vlm` 恒为 `False`，所以实际上总是扁平化文本。

接着把「结构化输出」翻译成 vLLM 的私有形状：

```python
StructuredOutputsParams(json=response_format["json_schema"]["schema"]) if response_format else None
```

注意这里取的是 `response_format["json_schema"]["schema"]`——即**假定调用方传的是 OpenAI 那种嵌套形状**，再从中剥出裸 schema 交给 vLLM。所以上游的 `response_format` 契约是 OpenAI 形状，转换责任在这一层。

然后从 kwargs 里 `pop` 出 `messages`、`stop`、`tools`、`tool_choice`（最后一个直接丢弃，因为 vLLM 的采样参数里没有 tool_choice 概念），把 `messages` 与 `tools` 一起交给 `tokenizer.apply_chat_template`，产出**一个纯字符串 prompt**。所以工具定义是被 chat template 渲染进提示里的，不是通过 API 参数传递。

随后构造 `SamplingParams`：`n`、`temperature`（默认 0.0）、`max_tokens`（默认 2048）、`stop`、`structured_outputs`。这三个默认值（贪心、2048）与 HTTP 路径的默认值不同，是本地推理的常见配置。

用量统计靠**长度**：`input_tokens=len(out[0].prompt_token_ids)`、`output_tokens=len(out[0].outputs[0].token_ids)`。这是精确值，因为 tokenizer 就在手上——比 HTTP 路径依赖服务端回报可靠。

**VLLM 的停止词兜底。** `if stop_sequences is not None and not self.supports_stop_parameter: output_text = remove_content_after_stop_sequences(...)`。但 `SamplingParams` 里已经传了 `stop=prepared_stop_sequences`——所以这是两层保险：服务端截断为主，客户端截断为辅。之所以需要后者，是因为 vLLM 的 stop 实现与「模型是否支持 stop 参数」无关，而 `supports_stop_parameter` 是按模型名正则判断的，对本地模型通常返回 `True`（名字不匹配那些云端模式），于是这段兜底大多不生效。

**VLLM 的清理专门方法。** `cleanup()` 做四步：`destroy_model_parallel()`、删掉 `self.model.llm_engine.model_executor.driver_worker`（注释给了 vLLM issue 链接作为出处）、`gc.collect()`、`destroy_distributed_environment()` 加 `torch.cuda.empty_cache()`。

这个方法的存在本身就说明了本地推理的一个真实问题：**模型占用的显存不会因为对象被垃圾回收就释放**。分布式并行状态、CUDA 缓存都需要显式销毁，否则同一进程里连续加载第二个模型会 OOM。API 模型没有对应方法，因为它的资源在远端。

**MLX 的流式循环。** `MLXModel.generate` 表面上是非流式，内部却用 `stream_generate` 逐 token 累积：

```python
for response in self.stream_generate(self.model, self.tokenizer, prompt=prompt_ids, **completion_kwargs):
    output_tokens += 1
    text += response.text
    if any((stop_index := text.rfind(stop)) != -1 for stop in stops):
        text = text[:stop_index]
        break
```

这个循环同时完成了三件事：累积文本、**计数输出 token**（每轮加一，因为 MLX 每轮吐一个 token）、检查停止词。

停止检查用的是 `text.rfind(stop)` 加海象运算符——找每个停止词在累积文本里的**最后出现位置**，任一找到就截断并 break。用 `rfind` 而不是 `find` 的原因在于：累积文本可能已经包含停止词的一部分（例如停止词是 `END`，当前 text 是 `...EN`），下一轮拼上 `D` 才完整；`rfind` 总能找到最新出现的那个完整停止词。而 `find` 会找到更早的位置，导致过度截断。

**MLX 的用量与不支持项。** 输入 token 用 `len(prompt_ids)`，输出用循环计数。`response_format` 非空时直接 `ValueError("MLX does not support structured outputs.")`——不是静默忽略，而是明确拒绝。这在「能力差异」的处理上比 VLLM 更硬：VLLM 有替代实现就翻译，MLX 没有就报错。

**两者的能力声明差异。** `VLLMModel` 设 `self._is_vlm = False` 并注释「VLLMModel 还不支持视觉模型」；`MLXModel` 设 `self.is_vlm = False` 并注释「mlx-lm 不支持视觉模型」，同时把 `flatten_messages_as_text=True` 传给基类。所以视觉能力在两个实现里都是「明确声明不支持」而不是「未实现所以行为未定义」。

**chat template 的 kwargs 分开传。** 两者都有独立的 `apply_chat_template_kwargs` 构造参数，且 `MLXModel` 用 `setdefault("add_generation_prompt", True)` 给一个默认值。这个分开是必要的：`apply_chat_template` 的选项（是否加生成提示、是否 tokenize）与推理引擎的采样参数是两套完全不同的配置，混在 `**kwargs` 里必然冲突。

## 关键取舍

**源码事实：** 两个本地实现都不继承 `ApiModel`，因此没有限流与重试。这正确——本地推理的失败原因（OOM、模型不支持某参数）重试无益。

**源码事实：** `VLLMModel` 把 `tool_choice` 从 kwargs 里 `pop` 后**直接丢弃**，注释只说「Override the OpenAI schema for VLLM compatibility」。所以「强制模型调用工具」这个语义在 vLLM 路径上不可用，工具只能靠提示与 chat template 引导。

**源码事实：** MLX 的停止词检查用 `rfind`，即「最后出现位置」。这意味着如果模型先生成了一次 `END` 又继续生成，最终截断点会在**后面那个** `END` 上——这与服务端 stop 的「首次出现即停」不同。这是一个潜在的行为差异，但对「逐 token 累积」的场景通常无影响，因为一旦出现完整停止词就会立刻 break。

**源码事实：** `VLLMModel.cleanup()` 直接访问 `self.model.llm_engine.model_executor.driver_worker` 这个内部路径，并注释指向具体 issue。这是对第三方库内部结构的硬依赖，vLLM 重构后会失效。

**架构推论：** 本地模型必须在进程内自己算用量、自己实现停止词，因为没有任何「服务端」替它做这些。这与 API 模型的刚好相反：那边这些都从响应里读，这边要自己数 token。代价是两套语义，收益是不依赖外部服务。

**架构推论：** 依赖检查与错误消息写成「请安装某个 extra」的形式，是把可选依赖的发现时机提前到构造期。若不做，用户会在首次 `generate` 时收到 `ModuleNotFoundError: No module named 'vllm'`，从栈里看不出这是「没装可选依赖」还是「装错了版本」。

**架构推论：** 把 `apply_chat_template_kwargs` 与采样参数分开传，反映了一个更深的分层：本地推理的「提示构造」与「解码策略」是两个独立阶段，而 HTTP API 把两者合并成一个请求。这个差异是「本地 vs 远端」最本质的不同，不是实现细节。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/models.py` | `VLLMModel` | vLLM 本地推理实现 |
| `src/smolagents/models.py` | `VLLMModel.generate` | 拼提示、构造采样参数、按 token 长度算用量 |
| `src/smolagents/models.py` | `VLLMModel.cleanup` | 释放分布式并行状态与显存 |
| `src/smolagents/models.py` | `MLXModel` | Apple Silicon 本地推理实现 |
| `src/smolagents/models.py` | `MLXModel.generate` | 逐 token 流式累积、计数、停止词截断 |
| `src/smolagents/models.py` | `supports_stop_parameter` | 按模型名判断是否支持服务端 stop |
| `src/smolagents/models.py` | `remove_content_after_stop_sequences` | 客户端停止词兜底截断 |

## 参考

- [smolagents 源码：models.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/models.py)
- [vLLM 文档：SamplingParams 与结构化输出](https://docs.vllm.ai/en/latest/dev/sampling_params.html)
- [MLX 文档：mlx-lm 的 stream_generate](https://github.com/ml-explore/mlx-lm)

## 与主核 / AI 的关系

主核里最接近的是「嵌入式引擎与远端引擎」的对照：嵌入式把执行放在进程内，无需网络协议、无需序列化中间结果，但资源（内存、文件句柄）必须由自己管理并显式释放；远端引擎把这些交给服务端，自己只处理请求与响应。`VLLMModel.cleanup()` 那段显存销毁代码，与主核里「关闭嵌入式数据库时必须显式释放共享内存段」是同一类问题——**进程内资源的释放不是自动的**。

与 API 路径对照，本地推理少了两层设施（限流、重试），多了两件事（提示构造、资源清理）。这个对比本身很有信息量：网络模型的复杂度在「如何可靠地发请求」，本地模型的复杂度在「如何管理本机资源」。

对 AI 工程最实用的一条是 MLX 那个「逐 token 累积 + 每轮检查停止词」的循环结构。它揭示了一个在流式实现里常见的问题：停止词可能被拆在两个 token 里（例如 `END` 拆成 `EN` 与 `D`），所以必须对**累积后的文本**检查而不是对每个 token 检查。同时用 `rfind` 而非 `find` 避免找到更早的位置。任何手写流式截断的地方都要注意这两点。

另一条是「不支持就明确报错」。`MLXModel` 对 `response_format` 直接 `ValueError`，而 `VLLMModel` 则做了形状翻译。两者没有统一成「静默忽略」——因为静默忽略会让调用方以为结构化输出生效了，实际拿到的是自由文本，只在解析时才发现问题。把能力缺失暴露在调用点，比让它变成下游的解析失败便宜得多。
