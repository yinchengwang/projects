---
project: langchain
feature: chat-model-contract
title: 聊天模型契约
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/language_models/chat_models.py
    symbols: [BaseChatModel, SimpleChatModel, _ChatModelBinding]
  - path: libs/core/langchain_core/language_models/llms.py
    symbols: [BaseLLM, LLM]
---

# 聊天模型契约

## 一句话职责

把「模型」抽象成一份**必选两个、其余全是可选钩子**的契约：只要实现 `_generate` 与 `_llm_type` 就能跑，流式、异步、批处理、工具绑定全都有默认路径——而「要不要真的流式」由四个否决位与三个肯定位共同判定，不是由有没有实现 `_stream` 决定。

## 机制是什么

`BaseChatModel(BaseLanguageModel[AIMessage], ABC)` 的类 docstring 里有**三张表**，这个结构本身就是对这一层最好的说明：

1. **命令式方法表**：`invoke` / `ainvoke` / `stream` / `astream` / `astream_events` / `batch` / `abatch` / `batch_as_completed` / `abatch_as_completed`，输入输出与「默认行为是什么」一栏写得极准——例如 `ainvoke` 那一行写「默认在异步执行器里跑 `invoke`」、`stream` 那一行写「默认产出一份 `invoke` 的结果」。
2. **声明式方法表**：`bind_tools` / `with_structured_output` / `with_retry` / `with_fallbacks` / `configurable_fields` / `configurable_alternatives` —— 全都是「用这个模型创建另一个 `Runnable`」。
3. **自定义实现表**：必需项 `_generate` 与 `_llm_type`（后者注明「用于唯一标识模型类型，用于日志」），可选项 `_identifying_params`（「为追踪目的表达模型参数化」）、`_stream`、`_agenerate`、`_astream`。

所以这个契约的形状是：**两个必选，四个可选钩子，其余靠基类默认实现**。

同族的 `BaseLLM(BaseLanguageModel[str], ABC)` 是「只进字符串出字符串」的旧式契约，`_generate` 标了 `@abstractmethod`，输入是 `list[str]` 而不是消息列表。

`SimpleChatModel` 是给老代码用的桥：它把 `_generate` 实现成「调 `_call` 拿一个字符串、包成 `AIMessage`」，于是子类只需实现 `_call`。

## 怎么实现的

**`SimpleChatModel` 的 docstring 直接说它是兼容层。** 原文是一条 `!!! note`：「这个实现主要**在这里是为了向后兼容**。新的实现请直接使用 `BaseChatModel`」。

它的 `_generate` 只有三行：

```python
output_str = self._call(messages, stop=stop, run_manager=run_manager, **kwargs)
message = AIMessage(content=output_str)
generation = ChatGeneration(message=message)
return ChatResult(generations=[generation])
```

注意它**丢掉了** `_call` 可能返回的元信息：只取 `output_str` 做 `content`，`ChatGeneration` 与 `ChatResult` 都用最朴素的构造。所以用 `SimpleChatModel` 的模型在追踪里看不到 token 用量、看不到 `response_metadata`、也拿不到多候选。这是「用便利换能力」的直接代价，也正是「新实现请直接用 `BaseChatModel`」这句建议的实质理由。

`SimpleChatModel` 仍然把 `_call` 标成 `@abstractmethod`（docstring 就三个字：「更简单的接口」）。而 `_agenerate` 走 `run_in_executor` 包 `_generate`——所以老式模型能直接获得异步能力。

**`BaseChatModel` 的四个可选钩子形成一张降级矩阵。** 从源码可以读出四条降级线：

- 不实现 `_agenerate` → 基类用 `run_in_executor(None, self._generate, ...)`，同步方法丢线程池。
- 不实现 `_stream` → 基类 `raise NotImplementedError`，但**上层 `stream` 会捕获并退回 `invoke`**（docstring 明写「`stream` 默认产出一份 `invoke` 的结果」）。
- 不实现 `_astream` → 基类用哨兵桥接同步 `_stream`（与 g06 的 `alazy_load` 完全同一手法：`iterator = await run_in_executor(None, self._stream, ...)`、`done = object()`、`next(iterator, done)`）。
- 不实现 `_identifying_params` → 只是少了一份追踪元信息（它是 `Optional`）。

所以「实现 `_generate` 就能用」这句话是准确的：其余三层都能从它推导出来。而推导链条是有方向的——**异步从同步来、流式从非流式来**，绝不反向。

**`_streaming_disabled` 是四个否决位。** 这个方法 docstring 明写「这些会覆盖任何肯定触发（已挂接 handler、`stream=True` 等）」：

```python
if self.disable_streaming is True: return True
if self.disable_streaming == "tool_calling" and kwargs.get("tools"): return True
if "stream" in kwargs and not kwargs["stream"]: return True
return ("streaming" in self.model_fields_set and getattr(self, "streaming", None) is False)
```

四个位分别是：实例级硬关；「只在带工具时关」；调用级 `stream=False`；实例级 `streaming=False`。

第 2 位 `disable_streaming="tool_calling"` 的字段 docstring 把它讲透了：「若为 `'tool_calling'`，只有在模型被带上 `tools` 关键字参数调用时才绕过流式。换句话说，LangChain 只在提供了 tools 参数时自动切换到非流式行为（`invoke`）。**这提供了两个世界里最好的东西**」。

这条的理由在真实后端上很具体：不少模型的「带工具 + 流式」组合实现有问题（工具调用的增量片段拼不起来），而「带工具但不流式」是可靠的。用 `"tool_calling"` 就精确地把「有毛病的那条路径」关掉，其余保持流式。

最后一行那三个条件的与，读起来有点绕：`"streaming" in self.model_fields_set` 表示「这个实例显式设过 `streaming` 字段」（不是类默认值），`getattr(self, "streaming", None) is False` 表示值是 `False`。两者都要成立才算否决。用 `model_fields_set` 而不是直接读值，是为了区分「用户显式关」与「类里就没这个字段」——后者不该被当成关。

**`_should_stream` 是「能否 + 愿不愿」。** 它先算两件事：

```python
sync_not_implemented = type(self)._stream == BaseChatModel._stream
async_not_implemented = type(self)._astream == BaseChatModel._astream
```

这是与 g03 / g06 同源的「比对方法身份」手法，但这里比对的是**基类自身的方法对象**，所以语义是「子类有没有覆盖它」，比 `type(self).load != BaseLoader.load` 更直接。

然后两道能力检查：

- 同步路径：`if (not async_api) and sync_not_implemented: return False`
- 异步路径：`if async_api and async_not_implemented and sync_not_implemented: return False`

第二条的两个条件都是「未实现」——因为异步可以从同步桥接过来（`_astream` 的默认实现就是桥接 `_stream`）。所以只要同步那层有实现，异步路径也能流。这个判断写得很精确，注释也点了：「注意，因为异步回退到同步，这里要检查两者」。

能力通过之后是 `if self._streaming_disabled(**kwargs): return False`（否决位优先），再往下是三个肯定位：

1. `kwargs.get("stream")` 为真。
2. `"streaming" in self.model_fields_set` 且 `getattr(self, "streaming", None) is True`。
3. 已挂接的 handler 里有任何一个 `_StreamingCallbackHandler` 的实例：`any(isinstance(h, _StreamingCallbackHandler) for h in handlers)`。

第 3 位是最不显然的一条：**「挂了流式回调 handler」本身就是要求流式的信号**。理由是「只有流式时才能收到增量回调」，所以一个挂了流式 handler 的调用若走非流式路径，那个 handler 就是死的。把它当成肯定触发，等于「按需求推断意图」。

**`_should_use_protocol_streaming` 是另一条独立的判定。** docstring 说清了两件事：「它在 `_generate_with_cache` / `_agenerate_with_cache` 内部、**run manager 打开之后**与 `_should_stream` 并行运行；当某个 handler 声明自己是 `_V2StreamingCallbackHandler` 时，它压过 v1 流式分支。它是与 `_should_stream` **并列**的而不是委托——v1 与 v2 的肯定触发是**不相交**的」。

所以这套判定有两个互斥的「协议版本」：

- v1：靠 `_StreamingCallbackHandler`、`stream=True`、实例 `streaming=True` 触发。
- v2：靠 `_V2StreamingCallbackHandler` 触发。

而 v2 还要求「在请求的那一侧有事件来源」：

```python
has_native_sync = getattr(cls, "_stream_chat_model_events", None) is not None
has_native_async = getattr(cls, "_astream_chat_model_events", None) is not None
overrides_sync = cls._stream is not BaseChatModel._stream
overrides_async = cls._astream is not BaseChatModel._astream
has_sync_source = has_native_sync or overrides_sync
has_async_source = has_native_async or overrides_async
has_source = (has_sync_source or has_async_source) if async_api else has_sync_source
```

所以 v2 的事件来源有两种：**原生钩子**（`_stream_chat_model_events` / `_astream_chat_model_events`，名字存在即算）或**旧 `_stream` 的兼容桥**。而同步路径只认同步来源（`has_sync_source`），异步路径接受任一（`has_async_source` 或 `has_sync_source`）——同样是「异步可以回退到同步」。

第 3 步 `return not self._streaming_disabled(**kwargs)` —— v2 只受否决位约束，**不需要三个肯定位中的任何一个**，因为「挂了 v2 handler」本身就是它唯一的肯定触发（前面已经检查过了）。

**`_ChatModelBinding` 是一个「只为类型」存在的子类。** docstring 说得不再清楚：「`RunnableBinding` 保留聊天模型类型的 v3 重载。由 `BaseChatModel.bind` 返回，这样被绑定 runnable 的 `stream_events(version="v3")` / `astream_events(version="v3")` 调用方能拿到有类型的 `ChatModelStream` / `AsyncChatModelStream` 而无需 `cast`。**运行时它是一个普通 `RunnableBinding`；这个子类纯为给类型检查器更具体的表面**」。

它做了三件事：

1. `lc_id()` 重写为 `[*cls.get_lc_namespace(), "RunnableBinding"]` ——docstring 解释：「序列化时作为 `RunnableBinding`。运行时这个类与 `RunnableBinding` 行为等价；保持序列化 id 稳定意味着现有快照与 load 映射继续工作而不必注册新条目」。
2. 给 `stream_events` 与 `astream_events` 各写两份 `@overload`，`version="v3"` 那份返回 `ChatModelStream` / `AsyncChatModelStream`。
3. 实现体转发到 `super()`。

第 1 条是这一层最值得学的一处：**类型层面的细化不能带来序列化层面的新类型**。若 `lc_id` 默认给出 `_ChatModelBinding`，那么任何序列化过的 bind 结果都会在反序列化时找不到映射（`SERIALIZABLE_MAPPING` 里没有这个条目），于是所有旧快照都会坏。把 `lc_id` 钉回 `RunnableBinding`，就让「类型上更精确」变成零成本的纯静态改进。

`bind()` 的 docstring 也解释了为什么需要这个覆盖：「覆盖 `Runnable.bind` 使结果保留聊天模型专属的 `stream_events` / `astream_events` 重载。没有这个覆盖，`model.bind(...).stream_events(version="v3")` 会被类型化成 `Iterator[Any]`、`await model.bind(...).astream_events(version="v3")` 化成 `Any`，迫使调用方 `cast`」。

而「这个窄化在后续的 `bind` / `with_config` 调用中也能保留」这一条，靠的是 `RunnableBinding.bind` 用 `self.__class__(...)` 构造结果——所以子类身份在链式调用中被传递下去。这是一个很少见但很漂亮的细节：**类型信息沿链式调用传播，靠的是「构造时用 self.__class__」这个约定**。

**`asdict` 与废弃的 `dict`。** `asdict()` 实现是 `dict(self._identifying_params)` 加一个 `_type` 键。`dict()` 被 `@deprecated("1.4.2", alternative="asdict", removal="2.0.0")` 标为废弃并转发。两者并存的原因很实在：`_identifying_params` 是子类可能覆盖的属性，而有些老子类覆盖的是 `dict()`。所以还有第三个方法 `_dict_for_compat()`：「返回聊天模型字典**同时保留已废弃的覆盖**」，实现是 `with suppress_langchain_deprecation_warning(): return self.dict()`。

这是三重结构的典型：新名字（`asdict`）、旧名字（`dict`，带废弃警告）、以及「内部要用旧名字但不想刷警告」的兼容通道。没有第三个，任何内部调用都会触发用户可见的废弃警告。

**`_convert_input` 在两种契约里都做三形态归一。** `BaseLLM` 与 `BaseChatModel` 各有一份同名方法，逻辑形状一致：`PromptValue` 原样返回；`str` 包成 `StringPromptValue`；`Sequence` 用 `convert_to_messages` 转成 `ChatPromptValue`。其余抛 `ValueError` 并列出三种合法类型。

所以「模型的输入可以是字符串、消息序列、或者已渲染的 prompt」这件事，是在这一层统一掉的。这也是为什么 `invoke` 的签名能写成 `LanguageModelInput` 这么宽。

**`BaseLLM._serialized` 是 `functools.cached_property` 包 `dumpd`。** 注释解释了为什么可以 `cast`：「在这种情况下 self 总是 `Serializable` 对象，因此结果保证是 dict，因为 `dumpd` 用默认回调，它用 `obj.to_json`，而 `to_json` 总是返回 `TypedDict` 子类」。

这里有两条信息：一是序列化表示被缓存（每次追踪不必重算）；二是类型断言的依据被写进注释——不是「我觉得它应该是 dict」，而是「调用链保证它总是 TypedDict 子类」。这类注释比 `# type: ignore` 有价值得多，因为它给出了可检验的理由。

**`BaseLLM._stream` 的 docstring 写了「不实现会怎样」**：「这个方法应当被支持流式的子类覆盖。**若不实现，`stream` 调用的默认行为会回退到模型的非流式版本，并把输出作为一个单块返回**」。

所以 `_stream` 的默认实现 `raise NotImplementedError` 并不是终点——上层会捕获它并退回。这与 `VectorStore.delete` 的 `bool | None`、g05 的 `acompress_documents` 是三种不同的「未实现」表达：那里是返回值语义、那里是默认桥接、这里是抛异常但被上层兜住。第三种最不直观（要读上层代码才知道异常会被捕获），所以这一层专门用 docstring 把它写明。

## 关键取舍

**源码事实：** 自定义实现只需 `_generate` 与 `_llm_type`；`_identifying_params`、`_stream`、`_agenerate`、`_astream` 都是可选，且由基类给出降级路径。

**源码事实：** `disable_streaming` 三取值（`True` / `"tool_calling"` / `False`），`"tool_calling"` 只在传了 `tools` 时关流式，字段 doc 称这是「两个世界里最好的东西」。

**源码事实：** `_streaming_disabled` 的四个否决位会覆盖任何肯定触发。

**源码事实：** `_should_stream` 用 `type(self)._stream == BaseChatModel._stream` 判断是否覆盖，异步侧要同时查两层。

**源码事实：** 挂了 `_StreamingCallbackHandler` 本身就是「要求流式」的肯定位。

**源码事实：** `_should_use_protocol_streaming` 与 `_should_stream` 并列而非委托，v1 与 v2 的肯定触发不相交。

**源码事实：** `_ChatModelBinding.lc_id` 钉回 `RunnableBinding`，序列化不因类型细化而新增条目。

**源码事实：** `bind()` 的窄化靠 `RunnableBinding.bind` 用 `self.__class__(...)` 构造而沿链传递。

**源码事实：** `SimpleChatModel._generate` 丢弃 `_call` 之外的一切元信息。

**架构推论：** 「少数必选 + 可降级钩子」这个形态的根本原因，是模型后端的实现难度差异极大。一个本地小模型可能只能同步跑一次；一个商业 API 可能原生支持流式与异步。若两者都要求实现全套，「能跑的最小实现」反而成了少数派。所以契约把「流式」做成可推导而非必选——代价是「有没有真的流式」变成运行时的判定问题，也就诞生了那套否决位与肯定位。

**架构推论：** 「否决优先于肯定」这个顺序，是把**安全阀**放在**意图**之前。用户可能挂了流式 handler（表达意图），但同时也设了 `disable_streaming=True`（表达约束）；约束优先。任何「有默认行为 + 可覆盖开关」的系统都该这么排——否则开关就形同虚设。

**架构推论：** 两个协议版本（v1 / v2）用「不相交的肯定触发」来共存，而不是让新版本覆盖旧版本。这是个很克制的手法：v2 只在有人明确要求（挂了 v2 handler）时才启用，所以旧代码的行为一字不变。相比之下「默认切到新协议」会让所有依赖增量回调时序的代码悄悄改变行为。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel` | 三张表定下必选与可选的全貌 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._generate` | 唯一必选的生成方法 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._llm_type` | 必选的类型标识，用于日志 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._agenerate` | 默认异步即线程池包同步 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._astream` | 用哨兵把同步 `_stream` 桥成异步 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._streaming_disabled` | 四个否决位，覆盖一切肯定触发 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._should_stream` | 能力判定加三个肯定位 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._should_use_protocol_streaming` | v2 协议的独立判定与事件来源检查 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel.bind` | 返回 `_ChatModelBinding` 以保留类型重载 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel.asdict` | 参数字典，废弃的 `dict` 转发到此 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._dict_for_compat` | 内部使用旧入口但不刷废弃警告 |
| `libs/core/langchain_core/language_models/chat_models.py` | `_ChatModelBinding` | 纯类型子类，运行时等同 `RunnableBinding` |
| `libs/core/langchain_core/language_models/chat_models.py` | `_ChatModelBinding.lc_id` | 序列化 id 钉回 `RunnableBinding` |
| `libs/core/langchain_core/language_models/chat_models.py` | `SimpleChatModel` | 兼容层，只保留字符串输出 |
| `libs/core/langchain_core/language_models/llms.py` | `BaseLLM` | 字符串进出的旧式模型契约 |
| `libs/core/langchain_core/language_models/llms.py` | `BaseLLM._generate` | 必选方法，入参是 prompt 列表 |
| `libs/core/langchain_core/language_models/llms.py` | `BaseLLM._serialized` | 缓存的序列化表示 |
| `libs/core/langchain_core/language_models/llms.py` | `LLM` | 只要实现 `_call` 的简化基类 |
| `libs/core/langchain_core/language_models/llms.py` | `LLM._call` | 简化契约的唯一必选方法 |

## 参考

- [langchain 源码：language_models/chat_models.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/language_models/chat_models.py)
- [langchain 源码：language_models/llms.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/language_models/llms.py)
- [langchain 文档：聊天模型接口](https://python.langchain.com/docs/concepts/chat_models/)

## 与主核 / AI 的关系

主核里最接近的是「存储引擎的能力矩阵」：必选实现少数几个操作，其余（索引、压缩、并行扫描）都是可选，缺失时上层自动退回更慢但正确的路径。这一页的降级矩阵与那个模式一致，连「能力判定要看实现痕迹」这件事都像——主核看函数指针是否为空，这里看 `type(self)._stream == BaseChatModel._stream`。

差别在于**判定的复合度**。主核的能力判定是二值（实现了 / 没实现）；这里的「要不要流式」是「能力 ∧ ¬否决 ∧ 某个肯定位」的三段判断，且有两个互斥协议版本。原因在于流式不是一个纯能力问题——它同时是「后端支持」「调用方意图」「包装层约束」三者的交汇点。

与 g05（压缩器与嵌入）的分工：那边定义「文本进向量出」的极简契约；这一页定义模型契约，比它复杂一个数量级，因为模型要处理流式、异步、工具、结构化输出这一整片能力面。两页都出现了「异步默认是同步套壳」的同一手法，说明这是这一批契约定下的统一约定。

与 h02（流式投影）的衔接很直接：`_should_stream` 决定「走不走流式」，`ChatModelStream`（h02）决定「流式事件如何被投影成文本片段」。这一页是决策者，那一页是执行者。

与 h04（模型档案）的连接：`BaseChatModel.profile: ModelProfile | None` 这个字段就是 h04 那套能力声明的挂载点，字段 doc 明写「若未指定，初始化时从 provider 包自动加载」。

对 AI 工程最实用的一条是「否决位优先于肯定位」。任何「默认行为 + 用户开关」的设计都会遇到「同时存在相反信号」的情况：用户挂了流式 handler、同时设了 `disable_streaming=True`。把否决放前面，让约束永远生效，是唯一不会让开关失效的排序。

另一条是 `_ChatModelBinding` 那个「类型细化但不改序列化 id」的做法。给一个已有类型加更精确的静态类型表面时，最容易顺手给它一个新的序列化标识——而那个标识会让所有历史快照失效。判据很清楚：**只在编译期存在的东西，不该出现在运行期/持久化层的标识里**。

最后一条是「挂了流式 handler 就算要求流式」这个肯定位。它把一个隐式的需求写成了显式的判定：handler 只会在增量回调时被调用，所以挂了它就等于声明「我要增量」。这类「按依赖推断意图」的判定在多处有用，但要注意它只在「该 handler 的存在与否严格由意图决定」时才成立——若某个框架会无条件挂一个空 handler，这条判定就会让所有调用都走流式。
