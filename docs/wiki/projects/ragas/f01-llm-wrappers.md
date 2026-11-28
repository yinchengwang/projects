---
project: ragas
feature: llm-wrappers
title: 模型包装与工厂
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/llms/base.py
    symbols: [BaseRagasLLM, LangchainLLMWrapper, LlamaIndexLLMWrapper]
  - path: src/ragas/llms/base.py
    symbols: [llm_factory]
---

# 模型包装与工厂

## 一句话职责

这一层把「框架各异的模型客户端」收成两个互不兼容的接口——旧式 `BaseRagasLLM` 只谈文本与 `LLMResult`，新式 `InstructorBaseRagasLLM` 直接收一个 pydantic 模型并返回它的实例——而 `llm_factory` 是造后者的唯一入口。

## 机制是什么

同一个文件里并排着两代抽象，它们不是版本演进而是一个**能力分界**：

| | `BaseRagasLLM` | `InstructorBaseRagasLLM` |
|---|---|---|
| 输入 | `PromptValue`（一段文本） | `prompt: str` + `response_model` |
| 输出 | `LLMResult`（若干 `Generation` 文本） | 校验过的 pydantic 实例 |
| 多采样 | 有 `n` 参数 | 没有（一次一个） |
| 格式保证 | 无，调用方自己解析 | 由 instructor 与服务端保证 |
| 实现 | `LangchainLLMWrapper`、`LlamaIndexLLMWrapper` | `InstructorLLM`（`llm_factory` 造出来的就是它） |

`BaseRagasLLM` 是 `@dataclass` + `ABC`，带三个字段：`run_config`（重试与超时）、`multiple_completion_supported`、`cache`。三个抽象方法：`generate_text`、`agenerate_text`、`is_finished`。它自己只提供一个具体实现 `generate`——异步版的包装器。

`InstructorBaseRagasLLM` 是纯 `ABC`，只有两个抽象方法 `generate` 与 `agenerate`，签名完全一样（都是「提示文本 + 响应模型 → 模型实例」），区别只在是否 `await`。

## 怎么实现的

**旧式的 `generate` 不是抽象方法，这一点很关键。** 它做了三件事：温度缺省（`get_temperature(n)`，`n > 1` 给 0.3、否则 0.01——采样多次才需要随机性）、套 `add_async_retry` 重试、然后 `if not self.is_finished(result): raise LLMDidNotFinishException()`。

`is_finished` 因此是旧式接口的**关键契约**：它把「模型是不是被截断了」这件事变成每个包装器必须实现的判断。`LangchainLLMWrapper.is_finished` 从 `generation_info["finish_reason"]` 或 `ChatGeneration.message.response_metadata` 里的 `finish_reason` / `stop_reason` 读出原因，接受的集合是 `stop`/`STOP`/`MAX_TOKENS`/`eos_token`（`stop_reason` 还接受 `end_turn`）；读不到任何原因时**默认 `True`**（注释承认这条路径「less reliable」）。`LlamaIndexLLMWrapper.is_finished` 干脆恒返回 `True`——LlamaIndex 不暴露可比对的结束原因，于是那层保护形同虚设。

**两个旧式包装器都已被标记弃用。** 两者的 `__init__` 各自发一条 `DeprecationWarning`，文案都是「用 `llm_factory` 替代」，并给出完整迁移示例。但类 docstring 里的 `.. deprecated::` 指令块被注释掉了（留着 `# TODO: Revisit deprecation warning`）——也就是说**告警走运行期、文档标记走注释**，这是当前实现的真实状态。

**多采样的两种实现路径。** langchain 原生模型分两类：`is_multiple_completion_supported` 认的（OpenAI / Azure / Vertex 系）能一次要 `n` 个，走 `generate_prompt(prompts=[prompt], n=n)`；其余不支持 `n`，于是把同一个 prompt **复制 n 份**去批量调用，然后把 `generations` 从「n 个批次各一个」压成「一个批次 n 个」，让结果形状看起来像多采样。async 版同理，但改成设置 `self.langchain_llm.n = n`。两条路径都在调用前后**临时改温度再改回去**，并在 `bypass_temperature` 时跳过（o1 系不支持温度）。

**`set_run_config` 不只是存配置。** `LangchainLLMWrapper.set_run_config` 若发现底层是 OpenAI 系客户端，会把 `run_config.timeout` 写进客户端的 `request_timeout`，并且**把 `run_config.exception_types` 改成 `RateLimitError`**——即重试只针对限流异常。这是「配置设置顺带改了重试语义」的一个实例。

**`llm_factory` 是造新式 LLM 的唯一入口，且强制要求 client。** 代码里两条硬校验：`client is None` 抛 `ValueError` 且文案直接给出迁移步骤（写明「Text-only mode has been removed」）；`model` 为空也抛。这说明**旧式的纯文本工厂已经被移除**，不是弃用——`llm_factory` 只产出结构化 LLM。

工厂本身不做 provider 分支，而是交给适配器：`adapter="auto"` 时调 `auto_detect_adapter(client, provider)`，然后 `get_adapter(adapter).create_llm(client, model, provider, cache=..., mode=..., **kwargs)`。异常处理有个细节：`ValueError` 若含「Unknown adapter」字样直接透传（那是配置错误，不该被包装成初始化失败），其余异常统一包成「Failed to initialize {provider} client with {adapter} adapter」并附上原错误。

**instructor 的接入方式是「把客户端 patch 成会说话的函数」。** 两条函数分工：`_get_instructor_client` 按 provider 走 `instructor.from_openai` / `from_anthropic` / `from_genai` / `from_gemini` / `from_litellm` / `from_perplexity`，其余落到 `_patch_client_for_provider`。后者不看 provider 名而**看客户端形状**：有 `chat.completions.create` 就用 `from_openai`，有 `messages.create` 就用 `Instructor` / `AsyncInstructor` 构造，两者都没有就抛错。这让 DeepSeek、Groq、Mistral 这类「OpenAI 兼容但非 OpenAI」的服务能直接工作。

这里有一处刻意的模式选择：默认 `instructor.Mode.JSON` 而不是 `TOOLS`。注释给了原因——OpenAI 的 function calling（TOOLS 模式）对 pydantic 里的 `Dict` 类型有 bug，会返回空对象 `{}` 而不是结构化数据，并挂了一条 issue 编号。相反，Google 与 Perplexity 只有在调用方**显式**给了 mode 时才转发，否则用 instructor 自己为该 provider 选的默认——因为这两家没有 OpenAI 那个 `Dict` 问题，它们的默认更合适。

**新式实现里最费篇幅的是参数映射。** `InstructorLLM._map_provider_params` 按 provider 分派到三套映射：

- **Google**：把 `temperature`/`max_tokens`/`top_p`/`top_k` 收进一个 `generation_config` 字典，其中 `max_tokens` 改名为 `max_output_tokens`，其余参数留在顶层。
- **OpenAI / Azure**：先判模型是不是「推理模型」（o 系列 `o1`–`o9`、`gpt-5` 到 `gpt-19`、`codex-mini`，用前缀模式匹配以便覆盖未来版本）。命中则把 `max_tokens` 改名成 `max_completion_tokens`、把 `temperature` **强制设为 1.0**、并删掉 `top_p`——因为这几个模型只接受这些取值。还处理了 Bedrock 给模型 ID 加 `openai.` 前缀的情况：调用仍用原 ID，但检测用剥掉前缀后的名字。
- **Anthropic**：只转发调用方**显式设过**的 `temperature` 与 `top_p`。判断依据是 `_explicit_model_args`——它在构造时由 `model_args.model_fields_set | kwargs` 记下，与「默认值」区分开。理由写在注释里：当前 Claude 模型拒绝这两个参数，或只接受自己的默认值。

**系统提示被从模型参数里摘出来。** `InstructorLLM.__init__` 把 `system_prompt` 从 `model_args` 里 `pop` 掉，单独存成属性，调用时拼成消息列表的第一条。这样它不会作为采样参数被送进 API。

**同步/异步的双向桥接。** `InstructorLLM` 在构造时用 `_check_client_async` 判定客户端类型，把这个结果存成 `is_async`。之后：

- `generate`：异步客户端则把 `agenerate` 的协程丢进 `_run_async_in_current_loop`；同步客户端直接 `_generate_sync`。
- `agenerate`：同步客户端则 `await asyncio.to_thread(self._generate_sync, ...)`；异步客户端才真正 `await` 客户端调用。

于是 `_generate_sync` 成了**同步执行的唯一真相**，两种客户端、两个方向都收敛到它或它的异步镜像。

**`_check_client_async` 的检测方式多得异常。** 它依次判：类名是否 `AsyncInstructor`；包装器的 `.client` 属性是否为协程函数；`chat.completions.create` 是否协程；Google 的 `create` 是否协程；最后**遍历 `create_fn.__closure__` 里的每个 cell**，看有没有异步客户端或 `acompletion`。最后这条是兜底：instructor 把底层客户端藏在闭包里时不暴露属性。配套的 `_run_async_in_current_loop` 处理「事件循环已在跑」（Jupyter）的情形——起一个新线程、在新线程里建新循环跑协程，再把结果或异常搬回主线程。

## 关键取舍

**源码事实：** 两代接口并存且互不继承。旧式有 `n`、有 `is_finished`、返回 `LLMResult`；新式没有多采样、没有截断检测、返回模型实例。它们是能力分界，不是同一接口的两个版本。

**源码事实：** `llm_factory` 已移除纯文本模式（`client` 缺失即报错并给迁移指引），只产出 `InstructorBaseRagasLLM`。

**源码事实：** 默认 instructor mode 是 `Mode.JSON`，原因是 OpenAI 的 TOOLS 模式对 `Dict` 类型注解有 bug；Google/Perplexity 在未显式指定时沿用 instructor 自己的默认。

**源码事实：** Anthropic 的 `temperature`/`top_p` 只在调用方显式设过时才转发；OpenAI 推理模型的 `temperature` 被强制成 1.0、`top_p` 被删掉。

**源码事实：** `LangchainLLMWrapper.set_run_config` 会改写 `run_config.exception_types` 为 `RateLimitError`，把「配置」与「重试语义」绑在一起。

**源码事实：** `LlamaIndexLLMWrapper.is_finished` 恒 `True`，所以那条截断保护在这一路不起作用。

**架构推论：** 把「结构化保证」从客户端挪到服务端（instructor + `response_model`）后，客户端的失败处理只剩一层——异常，不再有「解析成功但字段不对」这种中间态。这就是为什么新式接口没有 `is_finished`：它不需要判断「模型是不是说完了」，因为拿到的已经是校验过的对象。

**架构推论：** 参数映射之所以要按 provider 手写三套，是因为「统一接口」这个目标在参数层无法真正达成——各家对 `max_tokens` 的命名、对 `temperature` 的可选性、对推理模型取值的限制都不同。把这些差异收在一处（`_map_provider_params`）而不是散在调用点，是让接口看起来统一的前提。

**架构推论：** `_explicit_model_args` 这个「区分显式与默认」的机制，本质是在补 pydantic 默认值带来的信息损失——有默认值的字段无法从值本身判断「调用方是否设过」。凡是「默认值恰好是 provider 不接受的取值」的场景，都需要这一层记账。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/llms/base.py` | `BaseRagasLLM` | 旧式接口：文本进、`LLMResult` 出，附 `is_finished` 契约 |
| `src/ragas/llms/base.py` | `BaseRagasLLM.generate` | 温度缺省、重试包装、截断检测后抛 `LLMDidNotFinishException` |
| `src/ragas/llms/base.py` | `LangchainLLMWrapper` | langchain 适配：`n` 支持判定与复制 prompt 的降级路径 |
| `src/ragas/llms/base.py` | `LangchainLLMWrapper.is_finished` | 从 `finish_reason`/`stop_reason` 判定截断，读不到即默认通过 |
| `src/ragas/llms/base.py` | `LlamaIndexLLMWrapper` | LlamaIndex 适配：参数裁剪与恒真的截断判定 |
| `src/ragas/llms/base.py` | `llm_factory` | 唯一的新式工厂：强制 client、适配器自动选择 |
| `src/ragas/llms/base.py` | `InstructorBaseRagasLLM` | 新式接口：提示 + 响应模型 → 模型实例 |
| `src/ragas/llms/base.py` | `InstructorLLM` | 新式实现：参数映射、系统提示、正反向桥接 |
| `src/ragas/llms/base.py` | `InstructorLLM._map_provider_params` | 三套 provider 参数映射的分派 |
| `src/ragas/llms/base.py` | `_get_instructor_client` | 按 provider 把客户端 patch 成 instructor 形态 |
| `src/ragas/llms/base.py` | `_patch_client_for_provider` | 按客户端形状（而非 provider 名）选择 patch 方法 |
| `src/ragas/llms/base.py` | `_check_client_async` | 多形态的异步能力探测，含闭包遍历 |
| `src/ragas/llms/base.py` | `is_multiple_completion_supported` | 认哪些 langchain 模型支持一次要 n 个 |

## 参考

- [ragas 源码：llms/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/llms/base.py)
- [ragas 文档：使用自定义 LLM](https://docs.ragas.io/en/stable/howtos/customizations/custom_llm/)
- [Instructor 文档：结构化输出与 provider 适配](https://python.useinstructor.com/)
- [LangChain 文档：BaseLanguageModel 接口](https://python.langchain.com/docs/concepts/chat_models/)

## 与主核 / AI 的关系

两代接口的分野，与主核里「文本协议」到「二进制协议」的迁移是同一种工程现象：新接口把**校验前移到对端**，于是本端的解析与重试设施全部可以删掉。主核省的是解析 CPU 与容错分支，这里省的是「解析失败 → 修格式提示 → 再解析」那一整条链路（见 E 组第二页）。

对 agent 工程最实用的四条。其一：选 `llm_factory` 造出的新式 LLM，能同时拿到结构化输出与服务端保障，且避开旧式包装器的弃用告警。其二：默认 `Mode.JSON` 不是随便选的——若你的响应模型字段里有 `Dict`，用 TOOLS 模式会稳定地拿到空对象。其三：OpenAI 推理模型的 `temperature` 会被强制成 1.0、`top_p` 被删——这意味着在 o 系列上你**无法通过调温度来控制确定性**，需要改用别的办法（例如限制输出模型、或多次采样取一致）。其四：`is_finished` 只在旧式路径存在，且 llama-index 那一路恒真——如果你依赖「截断即失败」这个保护，必须走 OpenAI 系 langchain 客户端，或自己解析 `finish_reason`。
