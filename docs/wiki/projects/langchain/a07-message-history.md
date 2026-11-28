---
project: langchain
feature: message-history
title: 会话历史绑定
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/runnables/history.py
    symbols: [RunnableWithMessageHistory]
  - path: libs/core/langchain_core/messages/base.py
    symbols: [BaseMessage, BaseMessageChunk]
---

# 会话历史绑定

## 一句话职责

`RunnableWithMessageHistory` 把一个「无状态」的管道包装成「有状态对话」：进入时按 config 里的会话键取回该会话的历史，把它拼进输入（或放进输入的某个键）；结束后用一个 `on_end` 监听器把本轮的用户消息与模型回复**追加**回历史——读取与写回分别挂在链的两端，中间那条链本身完全不知道自己有历史。

## 机制是什么

这件事的难点不在「读写」，而在三处结构约束：

1. **历史实例从哪来。** 用户给的是一个工厂函数 `get_session_history`，它的参数来自 `config["configurable"]`（默认只有一个 `session_id`）。于是「哪个会话」这件事**只能通过 config 传递**，没有别的通道。
2. **历史怎么进到输入里。** 有两种形状：整条对话就是输入（`list[BaseMessage]`），或者历史只是输入字典里的**一个键**（配合 `MessagesPlaceholder`）。后者要靠「往输入字典里赋一个键」这个动作——正好是 a06 里 `RunnableAssign` 的活。
3. **历史怎么被写回。** 写回发生在**链结束之后**，而那时手里只剩一个 tracing 的 `Run` 对象，没有调用参数。于是必须从 `run.inputs` / `run.outputs` 反序列化出消息来。

第三点带来一个隐蔽的坑：如果历史是**前置拼进输入**的（第 2 种形状的前半），那么 `run.inputs` 里就含着全部历史消息；写回时若不剔除，每条消息都会被重复追加一次。源码用「按历史长度切片」解决这个问题——这一点是这一页最值得细看的地方。

还有一件需要先说明的事：这个类已经被标记废弃。`__init__` 的第一句就是

```
warn_deprecated(
    since="1.3.3",
    message=("RunnableWithMessageHistory is deprecated. Use LangGraph's built-in persistence instead."),
    removal="2.0.0",
)
```

也就是说它是一个「保留可用但要退出历史舞台」的设计。它的机制仍然值得读——因为它把「历史」这件事的所有边界情形都暴露了一遍，而这些情形正是后继实现（LangGraph 的持久化）必须处理的东西。

## 怎么实现的

**`__init__` 装配的不是「一个包装器」，而是一条内部链。**

```
history_chain: Runnable[Any, Any] = RunnableLambda(self._enter_history, self._aenter_history).with_config(run_name="load_history")
messages_key = history_messages_key or input_messages_key
if messages_key:
    history_chain = RunnablePassthrough.assign(**{messages_key: history_chain}).with_config(run_name="insert_history")

runnable_sync = runnable.with_listeners(on_end=self._exit_history)
runnable_async = runnable.with_alisteners(on_end=self._aexit_history)

def _call_runnable_sync(_input: Any) -> Runnable[Any, Any]:
    return runnable_sync

async def _call_runnable_async(_input: Any) -> Runnable[Any, Any]:
    return runnable_async

bound = (
    history_chain
    | RunnableLambda(_call_runnable_sync, _call_runnable_async).with_config(run_name="check_sync_or_async")
).with_config(run_name="RunnableWithMessageHistory")

super().__init__(get_session_history=..., bound=bound, ..., **kwargs)
self._history_chain = history_chain
```

这段装配里有四个决定：

**第一，`history_chain` 有两种形态。** 没有 `messages_key` 时它是一个裸的 `RunnableLambda(self._enter_history)`——于是输入被直接替换成「历史 + 本轮输入」的完整消息列表（对应形状 1）。有 `messages_key` 时它被包成 `RunnablePassthrough.assign(**{messages_key: history_chain})`——于是原字典保留，只在指定的键上放历史（对应形状 2）。**同一个 `_enter_history` 函数在两种形态下的入参不同**（前者收到原始输入，后者收到原始输入并返回历史），这是靠 `assign` 的并行语义自动适配的。

**第二，同步与异步的分派发生在运行期。** 那个 `check_sync_or_async` 的 `RunnableLambda` 收到 `_input` 后什么都不做，只是**返回**同步版或异步版的 `Runnable`。返回一个 `Runnable` 作为「值」，让它在整条链里被当作下一步——于是「调用方走哪条路径」决定了用哪一个带 listener 的版本。这是一个很巧的写法：它把「同步/异步需要在装配期决定」这个矛盾推迟到了运行期，因为装配期不知道调用方会用哪个。

**第三，写回靠 listener 而不是靠包装 `invoke`。** `runnable.with_listeners(on_end=self._exit_history)` 给内层挂了一个「结束回调」，于是无论调用方走 `invoke` / `stream` / `batch` 的哪一条，只要那次 run 正常结束就会触发写回。若改用「覆盖 `invoke` 后手动写回」，就得为九种调用方式各写一遍。

**第四，`bound` 是那条链，而 `self._history_chain` 保留的是裸的加载链。** 后者只用于 `OutputType`：

```
@property
def OutputType(self) -> type[Output]:
    return self._history_chain.OutputType
```

这里有一处很微妙的选择：`OutputType` 取自 `_history_chain` 而不是 `bound`。`bound` 的末尾是那个「返回 Runnable 的 lambda」，它的输出类型是 `Runnable` 而不是业务输出；而 `_history_chain` 的输出才是「历史消息」或「带历史的字典」。这个不一致说明类型声明在这个类里已经不完全准确——但它是可用的近似。

**读取：`_enter_history` 只做三件事。**

```
def _enter_history(self, value: Any, config: RunnableConfig) -> list[BaseMessage]:
    hist: BaseChatMessageHistory = config["configurable"]["message_history"]
    messages = hist.messages.copy()
    if not self.history_messages_key:
        input_val = (value if not self.input_messages_key else value[self.input_messages_key])
        messages += self._get_input_messages(input_val)
    return messages
```

1. **实例从 `config["configurable"]["message_history"]` 拿**，且用 `[...]` 而不是 `.get` —— 缺这个键就是 `KeyError`。这看起来粗暴，但它是正确的：这个键由 `_merge_configs` 保证存在（见下），若不存在说明有代码路径绕过了包装器，此时报 `KeyError` 比静默用空历史好。
2. **`hist.messages.copy()`** 复制一份，因为接下来要 `+=` 追加。不复制就会**直接把本轮输入写进历史**——那是写回的职责，提前做会让失败的一轮也污染历史。
3. **`if not self.history_messages_key`** 才把输入拼进来。有 `history_messages_key` 时历史会单独放进那个键，当前输入留在 `input_messages_key` 里，两者各就各位。

`_aenter_history` 是异步镜像，唯一实质差别是 `await hist.aget_messages()`（异步历史存储可能真的要 I/O）。

**写回：`_exit_history` 是这一页最需要读懂的一段。**

```
def _exit_history(self, run: Run, config: RunnableConfig) -> None:
    hist: BaseChatMessageHistory = config["configurable"]["message_history"]

    inputs = load(run.inputs, allowed_objects="messages")
    input_messages = self._get_input_messages(inputs)
    if not self.history_messages_key:
        historic_messages = config["configurable"]["message_history"].messages
        input_messages = input_messages[len(historic_messages) :]

    output_val = load(run.outputs, allowed_objects="messages")
    output_messages = self._get_output_messages(output_val)
    hist.add_messages(input_messages + output_messages)
```

四步，每步都有理由：

**`load(..., allowed_objects="messages")`** —— `run.inputs` 是 tracing 层存下来的**已序列化**数据（dict 形态），不是原始 Python 对象。要还原成 `BaseMessage` 必须走反序列化，而 `allowed_objects="messages"` 是一个**安全边界**：只允许还原消息类型，不允许还原任意 class（否则一份被篡改的 trace 就能构造任意对象）。这个参数的存在说明 `load` 本身是有危险性的反序列化入口。

**`input_messages[len(historic_messages):]`** —— 这是那个「重复写回」的补丁。当 `history_messages_key` 为空时，`_enter_history` 把历史**前置**拼进了输入，于是 `run.inputs` 是「历史 + 本轮用户消息」。若原样全部写回，历史里就多了一份历史。截断的依据是**当前历史的长度**——此刻 `hist.messages` 还没被追加，所以它的长度正好等于「被前置进去的那一段」。

这个办法能成立，倚赖一个很强的时序假设：**从 `_enter_history` 读取到 `_exit_history` 写入之间，这个历史实例没有别人动过。** 若并发两个请求共享同一会话，第一个请求的写回会改变长度，第二个请求进来时读到的是「更长」的历史，但它的 `run.inputs` 里前置的是「较短」的那一段——两个长度对不上，截断就会错位（少截或多截）。这是一个真实的并发缺陷，而它不是靠加锁解决的，而是靠「每个会话同一时刻只有一个请求」这个用法假设。

**`input_messages + output_messages`** 一次性追加两类消息：本轮的用户消息与本轮的模型回复。次序是用户在前——这与真实对话的时间顺序一致。

**`_get_input_messages` 把四种输入形态归一成消息列表。**

```
if isinstance(input_val, dict):
    if self.input_messages_key: key = self.input_messages_key
    elif len(input_val) == 1: key = next(iter(input_val.keys()))
    else: key = "input"
    input_val = input_val[key]
```

字典形态的取键顺序是三级回退：**显式配置的键 → 唯一的键（只有一个时可猜） → 硬编码 `"input"`**。第二级是关键——「字典只有一个键，那它就是要找的那个」这条推断在绝大多数简单链里成立，于是用户不必配 `input_messages_key`。第三级是最后的兜底：多个键且没配，就假定叫 `input`；若也没有，`input_val[key]` 会 `KeyError`。

然后三支类型转换：

```
if isinstance(input_val, str): return [HumanMessage(content=input_val)]
if isinstance(input_val, BaseMessage): return [input_val]
if isinstance(input_val, (list, tuple)): ...
```

**裸字符串被当成 `HumanMessage`。** 这条转换是这个类最方便的地方：`chain_with_history.invoke("hi", config=...)` 不需要构造消息对象。它也是**唯一一处「类型即语义」的推断**——内容本身没有任何标记说它是人类的，靠的是「用户传了个字符串」。

**`_get_output_messages` 多一个专门为「直接包装 chat model」准备的特例。**

```
if isinstance(output_val, dict):
    if self.output_messages_key: key = self.output_messages_key
    elif len(output_val) == 1: key = next(iter(output_val.keys()))
    else: key = "output"
    if key not in output_val and "generations" in output_val:
        output_val = output_val["generations"][0][0]["message"]
    else:
        output_val = output_val[key]
```

那个 `generations` 分支的注释写着「如果你直接包装的是一个 chat model，输出其实是个奇怪的 generations 对象」。也就是说旧版 chat model 的返回形状是 `{"generations": [[{"message": AIMessage, ...}]]}`，两层列表嵌套，真实消息埋在 `[0][0]["message"]`。这个特例是一个**历史包袱**：新版的 chat model 直接返回 `AIMessage`，但这个类的兼容代码仍然保留着对老形状的探测。

注意探测条件很松：只要目标 key 不在字典里、而字典里有 `generations`，就走特例。于是「输出字典恰好有个叫 `generations` 的键」会被误判——但那种情况在实践中几乎不存在。

`str` → `AIMessage`（对应输入侧的 `str` → `HumanMessage`）；`BaseMessage` → 单元素；`list` / `tuple` → 序列。

**`_merge_configs` 是唯一一个「在配置合并期做副作用」的地方。**

```
def _merge_configs(self, *configs):
    config = super()._merge_configs(*configs)
    expected_keys = [field_spec.id for field_spec in self.history_factory_config]
    configurable = config.get("configurable", {})
    missing_keys = set(expected_keys) - set(configurable.keys())
    parameter_names = _get_parameter_names(self.get_session_history)

    if missing_keys and parameter_names:
        ... raise ValueError(带示例的提示) ...

    if len(expected_keys) == 1:
        if parameter_names:
            message_history = self.get_session_history(configurable[expected_keys[0]])
        else:
            if not config: config["configurable"] = {}
            message_history = self.get_session_history()
    else:
        if set(expected_keys) != set(parameter_names):
            ... raise ValueError ...
        message_history = self.get_session_history(**{key: configurable[key] for key in expected_keys})

    config["configurable"]["message_history"] = message_history
    return config
```

这段的分支结构是「工厂函数的**参数个数** × 配置项**个数**」的四象限：

| `expected_keys` | `parameter_names` | 行为 |
|---|---|---|
| 1 个 | 有 | 把该配置值按**位置**传给工厂 |
| 1 个 | 无（工厂不收参数） | 无参调用，并确保 `config["configurable"]` 存在 |
| 多个 | 名字集合相等 | 按**关键字**逐个传 |
| 多个 | 名字集合不等 | `ValueError`（名字对不上） |

**先用 `_get_parameter_names`（基于 `inspect.signature`）探测工厂的参数名**，再决定按位置还是按名字传。这是一个「让用户工厂函数可以自由选择签名」的适配层：`def get_by_session_id(session_id: str)` 与 `def get_session_history(*, user_id, thread_id)` 都能用。

不过这里有明显的粗糙处：当 `expected_keys` 为 1 且工厂无参数时，代码写的是 `if not config: config["configurable"] = {}` —— 但 `config` 此时已经是 `super()._merge_configs(...)` 的产物，必然非空（`ensure_config` 保证了默认键）。所以这一步实际上是**死代码**。紧随其后的 `config["configurable"]["message_history"] = message_history` 会覆盖任何已有的同名键——也就是说**用户不能自己在 configurable 里塞一个 `message_history`**，它总是被工厂的结果替换。

**最重要的缺失键检查带一个可执行的修复提示。**

```
example_input = {self.input_messages_key: "foo"}
example_configurable = dict.fromkeys(missing_keys, "[your-value-here]")
example_config = {"configurable": example_configurable}
msg = (f"Missing keys {sorted(missing_keys)} in config['configurable'] "
       f"Expected keys are {sorted(expected_keys)}."
       f"When using via .invoke() or .stream(), pass in a config; "
       f"e.g., chain.invoke({example_input}, {example_config})")
```

它**构造出一段可以直接抄的示例调用**（含具体缺失的键名与占位值）。这类错误消息在「必须通过 config 传参」的 API 里价值很高，因为用户最容易的失误就是忘了传 `config`。

注意条件是 `if missing_keys and parameter_names`：**若工厂不收参数，缺键也无所谓**（工厂会用某种全局默认，例如固定读一个存储）。这个短路让「无参工厂」这种用法不必传 config。

**`config_specs` 把工厂需要的键暴露成「可配置字段规格」。**

```
@property
def config_specs(self) -> list[ConfigurableFieldSpec]:
    return get_unique_config_specs(super().config_specs + list(self.history_factory_config))
```

若用户没给 `history_factory_config`，构造期会补一份默认的：

```
config_specs = [ConfigurableFieldSpec(id="session_id", annotation=str, name="Session ID",
    description="Unique identifier for a session.", default="", is_shared=True)]
```

`is_shared=True` 这个标志的意思是「这个值在并行分支之间共享」——于是前端/调用方只需提供一次 `session_id`，所有分支用同一个。默认值的空串 `""` 与 a05 里 `RunnableConfigurableAlternatives` 的 `default_key` 风格一致：一个「没设」的可比较值。

注意 `super().config_specs` 先出现（父类 = `RunnableBindingBase`，它会收集内层那条链的 specs），再加会话键。于是「这个可配置对象的全部可调项」= 「内层管道的可调项」+「会话工厂需要的键」。

**输入 schema 按三种输入形状给出三种结构。**

```
fields: dict[str, Any] = {}
if self.input_messages_key and self.history_messages_key:
    fields[self.input_messages_key] = (str | BaseMessage | Sequence[BaseMessage], ...)
elif self.input_messages_key:
    fields[self.input_messages_key] = (Sequence[BaseMessage], ...)
else:
    return create_model_v2("RunnableWithChatHistoryInput", root=(Sequence[BaseMessage], ...))
return create_model_v2("RunnableWithChatHistoryInput", field_definitions=fields)
```

三种情形对应三种用法：**两个键都配**（`input_messages_key` 收本轮输入、`history_messages_key` 收历史，历史由 assign 注入，所以 schema 里只有 `input_messages_key` 是必填）；**只配 `input_messages_key`**（历史被前置进同一个键，于是那个键的类型是消息**序列**而不是单个值）；**都没配**（整条输入就是消息序列，schema 是一个 `RootModel`）。

第二支的类型差异值得注意：`input_messages_key` 单独存在时它收的是 `Sequence[BaseMessage]`（因为历史被拼进来了），而两键并存时它只收单个 `str | BaseMessage | Sequence`（历史另占一键）。**同一个字段名在不同配置下类型不同**——这是 schema 必须按情形生成的原因。

**`BaseMessage` 是这套机制的值类型，四个字段决定了它能承载什么。**

```
content: str | list[str | dict[Any, Any]]
additional_kwargs: dict[Any, Any] = Field(default_factory=dict)
response_metadata: dict[Any, Any] = Field(default_factory=dict)
type: str
name: str | None = None
id: str | None = Field(default=None, coerce_numbers_to_str=True)
```

五点：

1. **`content` 是「字符串或块列表」这个联合**，而不是统一成列表。这个联合是全部麻烦的根源，也是兼容性的代价：早期代码只写字符串，新代码写块列表。保留联合意味着每个消费 `content` 的地方都要分两支。
2. **`additional_kwargs` 与 `response_metadata` 是两个独立的「额外字典」**，语义分工明确：前者「与消息关联的额外载荷」（工具调用等），后者「响应头、logprobs、token 数、模型名」。把它们拆开而不是合并成一个，是因为前者要参与序列化与回放（工具调用必须带回去），后者只是观测信息。
3. **`type: str` 是必填且唯一化的**（注释说明它的用途是反序列化时识别类型）。它是一个字符串而不是枚举，于是第三方可以定义自己的消息类型。
4. **`id` 的 `coerce_numbers_to_str=True`** —— 有些 provider 用整数作为消息 id，强制转换避免 pydantic 校验失败。
5. **`model_config = ConfigDict(extra="allow")`** —— 允许任意额外字段。这是「消息要能承载任何 provider 的特有字段」的必然选择，代价是拼写错误不会报错。

**`content_blocks` 属性做的是「把历史上存下来的各种 content 归一成标准块」。**

```
content = ([self.content] if isinstance(self.content, str) and self.content else self.content)
for item in content:
    if isinstance(item, str): blocks.append({"type": "text", "text": item})
    elif isinstance(item, dict):
        item_type = item.get("type")
        if item_type not in types.KNOWN_BLOCK_TYPES: blocks.append({"type": "non_standard", "value": item})
        else:
            if "source_type" in item: blocks.append({"type": "non_standard", "value": item}); continue
            blocks.append(cast("types.ContentBlock", item))

for parsing_step in [_convert_v0_multimodal_input_to_v1, _convert_to_v1_from_chat_completions_input,
                     _convert_to_v1_from_anthropic_input, _convert_to_v1_from_genai_input,
                     _convert_to_v1_from_converse_input]:
    blocks = parsing_step(blocks)
return blocks
```

**两阶段：先分类（standard / non_standard），再逐个 provider 的转换器依次尝试解包。** 五个转换器按顺序串起来——每个只处理自己认识的非标准块，不认识的原样传下去。最后剩下的仍是 `non_standard`。

其中 `"source_type" in item` 那条特判很具体：**v0 的块与 v1 的块可能用同一个 `type` 值**，但 v0 一定带 `source_type`。于是「有 `source_type` 就不是 v1 块」这条判据把两代格式区分开。这类「用存在性而非版本号做判别」的手法在无版本号的历史数据上是唯一可行的。

`text` 属性则简单得多：只抽 `type == "text"` 的块（或裸字符串），`"".join` 起来。**其它块类型被静默忽略**——注释写明「only extracts blocks with `type: 'text'`; other block types are ignored」。于是 `msg.text` 在一个只含图片的消息上是空串而不是报错。

返回类型 `TextAccessor` 是一个 `str` 的子类，同时支持属性与调用两种取法：

```
def __call__(self) -> str:
    warn_deprecated(since="1.0.0", message="Calling .text() as a method is deprecated. Use .text as a property instead ...", removal="2.0.0")
    return str(self)
```

`message.text` 拿到一个字符串（`str` 子类），`message.text()` 也能调用并返回同一个字符串，只是发一条废弃警告。**用「类型技巧」把一个 method 到 property 的破坏性迁移做成兼容**——这是本仓库里最典型的一处迁移手法。

**`BaseMessageChunk` 与 `merge_content` 提供「分块相加得到完整消息」。**

```
def __add__(self, other):
    if isinstance(other, BaseMessageChunk):
        return self.__class__(
            id=self.id, type=self.type,
            content=merge_content(self.content, other.content),
            additional_kwargs=merge_dicts(self.additional_kwargs, other.additional_kwargs),
            response_metadata=merge_dicts(self.response_metadata, other.response_metadata),
        )
```

三点：

1. **`id=self.id` 取第一个 chunk 的 id**，而不是新生成——分块的 id 与完整消息的 id 应当一致。
2. **`self.__class__(...)`** 保住子类（`AIMessageChunk` 相加得 `AIMessageChunk`），于是链式相加仍然类型正确。
3. **三个字段各用各的合并器**：`content` 用 `merge_content`（因为类型是联合），两个字典用 `merge_dicts`。字典的合并是递归的（`merge_dicts` 来自 `utils/_merge`，处理嵌套），因为 `response_metadata` 里可能嵌着 usage 之类的子字典。

`merge_content` 自己处理四种组合，其中最细的一条：

```
if isinstance(merged, list):
    # The branches below append to it, which would change the first message.
    merged = merged.copy()
```

**先复制列表再追加**，否则「把 chunk 相加」会顺手改掉第一个 chunk 的 content —— 一个典型的隐式共享陷阱。注释把后果说得很明白。

另一个细节：

```
elif merged and isinstance(merged[-1], str):
    merged[-1] += content
elif content == "":
    pass
```

**「列表 + 字符串」时拼到最后一个元素**（而不是新加一个元素），并且**空字符串是 no-op**（而不是把一个 `""` 推进列表）。这两条让「文本流式吐块」的典型场景得到自然结果：多个 `str` 块拼成一个文本块，而不是一堆碎片。

`__add__` 的报错也值得记：

```
msg = ('unsupported operand type(s) for +: "' f"{self.__class__.__name__}" f'" and "{other.__class__.__name__}"')
```

它模仿了 Python 内建运算符错误的格式，但填的是自己的类名——于是「消息 chunk 与不兼容类型相加」的错误看起来与普通 `+` 错误一致。

**历史写入的一定是完整消息，不是 chunk。** 这一点由 `_get_output_messages` 保证：它接收的 `output_val` 是链的最终输出（已由 `_ChunkAccumulator` 合并过，见 a01），因此拿到的是 `AIMessage` 而不是 `AIMessageChunk`。若真的把一个 chunk 存进历史，下一次读取时 `hist.messages` 里就会有一个「半条消息」——它的 `content` 只有部分文本。这条约束是隐式的，没有代码检查。

## 关键取舍

**源码事实：** `RunnableWithMessageHistory` 已在 1.3.3 标记废弃、计划 2.0.0 移除，`__init__` 首句就发 `warn_deprecated` 并建议改用 LangGraph 的持久化。

**源码事实：** `__init__` 装配的 `bound` 是 `history_chain | RunnableLambda(返回同步或异步 Runnable 的函数)`；写回靠 `with_listeners(on_end=self._exit_history)` / `with_alisteners`，于是九种调用方式都在 run 结束时触发写回。

**源码事实：** `history_chain` 在没有 `messages_key` 时是裸的加载 lambda，有 `messages_key` 时被 `RunnablePassthrough.assign(**{messages_key: ...})` 包住。

**源码事实：** `_enter_history` 用 `config["configurable"]["message_history"]` 且用 `[...]`（缺失即 `KeyError`），先 `hist.messages.copy()` 再追加，只在本轮输入不属于单独的 `history_messages_key` 时把它拼进返回列表。

**源码事实：** `_exit_history` 从 `run.inputs` / `run.outputs` 经 `load(..., allowed_objects="messages")` 反序列化；当历史被前置拼入输入时用 `input_messages[len(historic_messages):]` 截掉前置部分，最后 `add_messages(input_messages + output_messages)`。

**源码事实：** `_get_input_messages` 对字典按「显式键 → 唯一键 → `"input"`」三级取键，并把 `str` 转成 `HumanMessage`；`_get_output_messages` 除同构处理外还有 `generations` 两层嵌套的历史特例，并把 `str` 转成 `AIMessage`。

**源码事实：** `_merge_configs` 在配置合并期调用工厂并把结果写进 `config["configurable"]["message_history"]`（覆盖任何已有同名键）；分派按「配置键个数 × 工厂参数名」四象限；缺失键的报错里构造了可直接抄的示例调用；工厂无参数时短路缺失键检查。

**源码事实：** `config_specs` = 父类 specs + `history_factory_config`（默认补一个 `session_id`，`is_shared=True`，默认值 `""`）。

**源码事实：** 输入 schema 按三种用法给出三种结构，且「只有 `input_messages_key`」这一支的字段类型是 `Sequence[BaseMessage]`（因为历史被拼进同一键）。

**源码事实：** `BaseMessage` 的 `content` 是「字符串或块列表」联合，`id` 用 `coerce_numbers_to_str=True`，`model_config` 允许额外字段；`content_blocks` 先分类成 standard / non_standard 再依次尝试五个 provider 转换器；`text` 只收 `type == "text"` 的块并以 `TextAccessor` 返回。

**源码事实：** `TextAccessor` 是 `str` 子类，同时支持 `.text`（属性）与 `.text()`（调用并发废弃警告），把 method→property 的破坏性迁移做成兼容。

**源码事实：** `BaseMessageChunk.__add__` 沿用首个 chunk 的 `id`、用 `self.__class__` 保子类、三个字段各用对应合并器；`merge_content` 先把列表 copy 再追加（避免改到第一个 chunk），「列表 + 字符串」拼到末元素且空串为 no-op。

**架构推论：** 「读历史靠前置注入、写历史靠结束监听」这个对称结构是这类包装器的通用骨架：读发生在链前（因为它要改变输入），写发生在链后（因为它需要结果）。用监听器实现写的妙处是「不必覆盖九种调用方法」；代价是写回时只能拿到 tracing 的 `Run` 而不是原始对象，于是必须反序列化，并因此暴露了 `load` 这个安全边界。

**架构推论：** `input_messages[len(historic_messages):]` 这个截断是一个**基于长度的时间差重建**，它假定「读与写之间历史没有变化」。这个假设在并发共享会话时不成立。任何「先读全量、结束再按长度切片」的设计都有同一个脆弱点；更稳的做法是「记录本轮新增的消息」而不是「从全量里剪掉旧的」。

**架构推论：** 用 `str` → `HumanMessage` / `AIMessage` 的隐式转换让 API 极其好用，但它把「角色」这件事交给了位置（输入侧是人、输出侧是 AI）。在单轮链里成立，在多角色场景（工具消息、系统消息）里就必须显式构造消息对象。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/runnables/history.py` | `RunnableWithMessageHistory` | 装配「读 → 跑 → 写」三段；已标记废弃，建议改用 LangGraph 持久化 |
| `libs/core/langchain_core/runnables/history.py` | `_enter_history` / `_aenter_history` | 取历史实例、复制消息、必要时把本轮输入拼进去 |
| `libs/core/langchain_core/runnables/history.py` | `_exit_history` / `_aexit_history` | 从 `Run` 反序列化、截掉前置历史、追加本轮输入与输出 |
| `libs/core/langchain_core/runnables/history.py` | `_get_input_messages` / `_get_output_messages` | 四种输入/输出形态归一成消息列表，含 `generations` 历史特例 |
| `libs/core/langchain_core/runnables/history.py` | `_merge_configs` | 配置合并期调用工厂、校验缺失键，把实例写进 `configurable["message_history"]` |
| `libs/core/langchain_core/runnables/history.py` | `_get_parameter_names` | 用 `inspect.signature` 探测工厂参数名，决定按位置还是按关键字传 |
| `libs/core/langchain_core/messages/base.py` | `BaseMessage` | 消息值类型：content 联合、两个额外字典、可容纳任意 provider 字段 |
| `libs/core/langchain_core/messages/base.py` | `BaseMessage.content_blocks` | 把历史 content 归一成标准块，依次尝试五个 provider 转换器 |
| `libs/core/langchain_core/messages/base.py` | `TextAccessor` | `str` 子类，兼容 `.text` 属性与 `.text()` 方法两种取法 |
| `libs/core/langchain_core/messages/base.py` | `BaseMessageChunk` / `merge_content` | 分块相加得到完整消息；沿用首个 id、保子类、避免改到第一个 chunk |

## 参考

- [langchain 源码：runnables/history.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/history.py)
- [langchain 源码：messages/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/messages/base.py)
- [langchain 文档：如何为链添加消息历史](https://python.langchain.com/docs/how_to/message_history/)
- [langchain 文档：LangGraph 的持久化（推荐的替代方案）](https://langchain-ai.github.io/langgraph/concepts/persistence/)

## 与主核 / AI 的关系

「会话历史」在主核里对应的是「会话状态」或「临时表」：跨语句存活、按会话标识隔离、在事务边界上写入。这里有四条经验，其中两条是教训。

**第一，「按长度切片」去重是脆的。** `input_messages[len(historic_messages):]` 假定读与写之间历史没有变化。主核里若做「追加式日志 + 定期读取全量做比对」，会遇到同一个问题：任何并发写都会让「长度差」这个推断失效。正确做法是让写入方记录**自己新增了什么**，而不是在事后从全量里剪掉已知的部分。

**第二，「在配置合并期做副作用」是很危险的位置。** `_merge_configs` 每次合并都会调用工厂并覆盖 `message_history`。合并本该是纯函数，这里却创建/查找了一个有状态对象。后果是：任何重复合并都会重复调用工厂；用户也不能自己注入一个 `message_history`。主核里凡是「解析配置的同时建连接」的地方都有同样的隐患——解析应当只产出值，连接建立应当在明确的边界发生。

**第三，反序列化必须带白名单。** `load(..., allowed_objects="messages")` 这个参数是一个安全边界：只允许还原消息类型。主核里若从持久化的 trace / 日志里重建对象，同样必须限定可还原的类型集合，否则一份被篡改的记录就能构造任意对象。

**第四，「方法到属性的破坏性迁移」可以用类型技巧软着陆。** `TextAccessor` 是 `str` 的子类且实现 `__call__`，于是 `msg.text` 与 `msg.text()` 都可用，只有后者发废弃警告。主核的客户端协议若要从「函数调用」演进到「字段读取」，可以直接借用这个手法——它把迁移成本从「所有调用方立即改」变成「渐进改，且有一条明确的警告」。

另一条更一般的观察：这个类最终被标记废弃，理由是「改用图执行引擎的持久化」。原因是历史管理的边界情况（并发、截断、角色推断、多键会话）单靠一个包装器无法全部覆盖，而持久化引擎可以在更底层统一处理。这个演化方向对主核也有参考价值：**当一个能力需要越来越多的特例时，它大概不该继续作为一个包装器存在。**
