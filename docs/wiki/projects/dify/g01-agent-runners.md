---
project: dify
feature: agent-runners
title: Agent 执行器族
page_type: feature
schema_version: 1
refs:
  - path: api/core/agent/base_agent_runner.py
    symbols: [BaseAgentRunner]
  - path: api/core/agent/fc_agent_runner.py
    symbols: [FunctionCallAgentRunner]
  - path: api/core/agent/cot_agent_runner.py
    symbols: [CotAgentRunner]
---

# Agent 执行器族

## 一句话职责

两种把「模型想调工具」变成「真的有工具被调用」的实现：一种靠模型原生的函数调用（`FunctionCallAgentRunner`），一种靠**文本协议解析**（`CotAgentRunner`，ReAct 的 Thought/Action/Observation 三段式）——两者共享同一个基类提供的工具准备、思考落库、历史组织。

## 机制是什么

同一件事的两种协议：

| 维度 | `FunctionCallAgentRunner` | `CotAgentRunner` |
|---|---|---|
| 动作来源 | `LLMResultChunk.delta.message.tool_calls` | 从流式文本里正则式解析 `Action:` / `Action Input:` |
| 工具怎么交给模型 | 结构化 schema（`tools=prompt_messages_tools`） | 写进提示词文本（`tools=[]`） |
| 模型要求 | 必须支持 function calling | 任何模型（含不支持工具调用的） |
| 终止信号 | 没有 tool call 即结束 | 动作名等于 `final answer` |
| 迭代上限语义 | 到上限就**清空工具列表** | 到上限就**清空工具列表**，且若还想要工具则抛错 |

`CotAgentRunner` 是 `ABC`——它把 `_organize_prompt_messages` 声明成抽象方法，具体的提示词拼装交给每种模型系列的子类（不同模型对 ReAct 格式的遵从度不同）。所以「CoT 执行器」实际是一族，不是一类。

**这两种执行器代表的是「有 function calling 就用原生，没有就用文本协议」这条降级链。** 而这套机制正被 `dify-agent` 那个独立包取代（见 g02/g03），所以这一页描述的是一段**正在退役但仍在服务**的实现。

## 怎么实现的

### 共用部分：`BaseAgentRunner`

**构造期做五件事，其中三件有副作用。**

1. `organize_agent_history(session, prompt_messages)` —— 在 `__init__` 里就组织历史消息；
2. 建 `DifyAgentCallbackHandler` 与 `DatasetIndexToolCallbackHandler`（后者要 `queue_manager`、`app_id`、`message_id`）；
3. `DatasetRetrieverTool.get_dataset_tools(...)` 造出知识库检索工具——**知识库在这里被当成「一类工具」注入**，而不是特殊通道；
4. 查 `MessageAgentThought` 表数出已有思考条数 `agent_thought_count`（用于续写时编号连续）；
5. **`session.close()`** —— 构造函数主动关掉传进来的 session。

第 5 条很扎眼：一个构造函数关闭调用方的 session，意味着**调用方必须传一个可以关的 session，且之后不能再用它**。这与「谁打开谁关闭」的常规约定相反，但在这里是有意的：构造完就要去调模型（长耗时），提前释放连接。

**模型能力探测决定三个行为开关。**

```
model_schema = llm_model.get_model_schema(...)
features = model_schema.features or []
self.stream_tool_call = ModelFeature.STREAM_TOOL_CALL in features
self.vision_enabled   = ModelFeature.VISION in features
self.files = application_generate_entity.files if self.vision_enabled else []
```

**不支持视觉的模型连文件都不传**（`self.files` 直接置空而不是传了让模型报错）。而 `stream_tool_call` 决定 FC 执行器调模型时 `stream=` 传什么——支持流式工具调用就流式，否则一次性拿完整结果再解析。

**工具准备 `_init_prompt_tools` 的容错策略是「静默跳过」。**

```
for tool in self.app_config.agent.tools or []:
    try:
        prompt_tool, tool_entity = self._convert_tool_to_prompt_message_tool(tool)
    except Exception:
        # api tool may be deleted
        continue
```

注释说明了唯一预期原因：「api tool may be deleted」。后果是**任何工具准备失败都会让这个工具从列表里消失**，模型不知道自己少了一个工具。这是「配置里引用了已删除的 API 工具」这个真实场景的妥协——宁可少一个工具，也不要整个 Agent 起不来。

工具转换 `_convert_tool_to_prompt_message_tool` 还会顺手把 `DifyRunContext` 塞进工具的运行时：

```
tool_entity.runtime.dify_run_context = DifyRunContext(
    tenant_id=..., app_id=..., user_id=...,
    user_from=(UserFrom.ACCOUNT if invoke_from.runs_as_account() else UserFrom.END_USER),
    invoke_from=invoke_from,
)
```

`user_from` 由 `invoke_from.runs_as_account()` 推导——**调用来源决定「这是账号在跑还是终端用户在跑」**，而这个区分会影响工具内部的权限判断。

知识库工具走另一条转换 `_convert_dataset_retriever_tool_to_prompt_message_tool`，它的参数 schema **全部声明成 `string`**：

```
parameter_type = "string"
prompt_tool.parameters["properties"][parameter.name] = {"type": parameter_type, ...}
```

知识库工具的实际参数（查询文本、分数阈值等）都是标量，所以统一成 string 是可以的——但这也意味着**类型信息在这一步丢失**，模型看到的是「一切都是字符串」。

**`update_prompt_message_tool` 每轮都会刷新工具的参数 schema。**

```
prompt_tool.parameters = tool.get_llm_parameters_json_schema()
```

两个执行器都在每轮结束前对所有工具做一遍。原因是**工具参数可能随运行时状态变化**（例如某些参数只在特定配置下出现），所以每轮重新取一份，而不是在初始化时定死。

### `FunctionCallAgentRunner`

**主循环的骨架是「while 有工具调用且未超步数」，而超步数不是立刻失败，而是「先摘掉工具再试最后一次」。**

```
max_iteration_steps = min(app_config.agent.max_iteration, 99) + 1
while function_call_state and iteration_step <= max_iteration_steps:
    function_call_state = False
    if iteration_step == max_iteration_steps:
        prompt_messages_tools = []   # 最后一次：不给工具
```

`+1` 意味着实际循环次数比用户配置多一次——**那一次是「禁止调工具的收尾轮」**。于是当 `max_iteration=3` 时，模型有 3 次机会调工具，第 4 次只能回答。

而用户配置的迭代数被 `min(..., 99)` 夹住：**上限 99 是硬编码的**，防止配置写个 10000 把预算吃光。

**超步数判定在工具执行之后：**

```
if iteration_step == max_iteration_steps and tool_calls:
    raise AgentMaxIterationError(app_config.agent.max_iteration)
```

也就是说，如果最后一轮（本该收尾）模型仍然坚持要调工具，就抛 `AgentMaxIterationError`。**这是「最后一次机会没抓住」的显式失败**，而不是静默返回一个半成品答案。

**模型调用前显式释放数据库连接：**

```
session.commit()
session.close()
chunks = model_instance.invoke_llm(...)
```

注释写的是「Release any setup/tool transaction before waiting on the provider stream.」——**不要在等一个可能几十秒的流式响应时占着数据库连接**。

**流式与非流式两条路各自处理，但产出形状收敛。** 流式路径逐 chunk：`check_tool_calls` 判断有没有工具调用、`extract_tool_calls` 取出 `(id, name, args)` 三元组、累积文本到 `response`、累积用量、`yield chunk` 直通给上层。非流式路径拿完整 `LLMResult`，做同样三件事，然后把结果**包成一个单 chunk** yield 出去：

```
yield LLMResultChunk(..., delta=LLMResultChunkDelta(index=0, message=result.message, usage=result.usage))
```

**这与 f03 里 `PluginModelBackwardsInvocation.invoke_llm` 是同一个手法**：把非流式伪装成单块流，让消费方只写一种逻辑。

`json.dumps` 工具参数的地方有一处兜底：

```
try:
    tool_call_inputs = json.dumps({...}, ensure_ascii=False)
except TypeError:
    tool_call_inputs = json.dumps({...})   # force ASCII for non-serializable objects
```

`ensure_ascii=False` 保留中文可读，但遇到不可序列化对象会抛 `TypeError`；兜底版本去掉 `ensure_ascii=False` —— **注释说「force ASCII to handle non-serializable objects」，但这个兜底并不能解决不可序列化问题**（`ensure_ascii` 只影响非 ASCII 字符的转义，与对象可序列化性无关），它实际只在「含中文且含不可序列化对象」时同样失败。这是一处**看似兜底实则无效**的写法。

**工具执行的四种结果被统一成四字段字典。**

```
{"tool_call_id": ..., "tool_call_name": ..., "tool_response": ..., "meta": ...}
```

找不到工具时**不抛异常**，而是构造 `"there is not a tool named {name}"` 作为 `tool_response`，并把 meta 标成 `error_instance`。**这条错误文本会被作为 Observation 喂回模型**，于是模型有机会改用一个存在的工具——这是「把错误变成上下文」的具体应用。

工具真正执行时包在 `with use_credit_usage_metadata({"app_type": CreditUsageAppType.AGENT})` 里，执行完 `session.commit()` + `session.close()`，再把产生的文件逐个 `QueueMessageFileEvent` 推给前端。

**知识库检索的图片被单独处理。** `_build_dataset_tool_image_contents` 从工具响应里抽图片内容，若有就额外追加一条 `UserPromptMessage`（名字是 `_KNOWLEDGE_RETRIEVAL_PROMPT_NAME`），内容为「图片 + 查询或响应文本」。**这是为「多模态知识库检索」打的补丁**：工具响应是文本（含图片引用），但模型需要真的看到图片。

**思考落库分两次，这是这一页最值得注意的设计。** 每一轮先 `create_agent_thought` 拿一个 id，然后：

1. 模型响应后立刻 `save_agent_thought(tool_name=..., tool_input=..., thought=response, answer=response, ...)`；
2. 工具执行完再 `save_agent_thought(tool_name="", tool_input="", thought="", observation={...}, answer="", messages_ids=...)` —— **同一个 id 第二次写入，补上 Observation**。

于是数据库中「一次思考」由两次写入拼成：先有想法与工具调用，后有观察结果。前端通过 `QueueAgentThoughtEvent` 在每次写入后推送，因此**用户看到的是「思考中 → 调工具中 → 观察到了」的渐进过程**。

**`final_answer += response + "\n"` 是累加而不是覆盖。** 所以返回的最终答案是**所有轮次的模型输出拼接**，不是最后一轮。这一点在只看返回值的调用方看来很反直觉——它拿到的是完整思考过程，而不只是结论。

### `CotAgentRunner`

**它的核心是一个流式解析器：`CotAgentOutputParser.handle_react_stream_output(chunks, usage_dict)`。** 它把模型吐出的**纯文本**流转换成两种事件：解析出的 `Action` 对象、或普通文本块。执行器在循环里区分处理：

- 是 `Action` → 记进 `scratchpad.action`/`action_str`，并把 `json.dumps(chunk.model_dump())` 累进 `agent_response`；
- 是文本 → 累进 `agent_response` 与 `thought`，同时 `yield` 一个只含这段文本的 `LLMResultChunk` 直通给前端。

**所以「思考过程」是边流边推给用户的，而「动作」被拦下来不推。** 这解释了为什么 CoT 模式下用户能看到模型的大段推理文本，而 FC 模式下只能看到工具调用名。

**停止序列是动态加的：**

```
if "Observation" not in model_conf.stop:
    if model_conf.provider not in self._ignore_observation_providers:
        model_conf.stop.append("Observation")
```

`_ignore_observation_providers = ["wenxin"]` —— **文心一言被排除在外**：它的 API 不接受某些停止序列，或者加上会让生成异常。这是一份**按 provider 名硬编码的例外清单**，未来每遇到一个不兼容的 provider 都要往这里加一条。

**ReAct 的解析没有 action 时直接当最终答案返回。**

```
if not scratchpad.action:
    final_answer = ""
else:
    if scratchpad.action.action_name.lower() == "final answer":
        ...  # dict/str/其它三种形态分别取值
```

两条兜底都值得记：**解析失败（没有 action）时 `final_answer = ""`** —— 静默返回空串，而不是报错或重试；**`final answer` 的输入接受三种形态**（dict → JSON 序列化、str → 原样、其它 → `f"{...}"`），因为模型有时把答案写在 `Action Input` 里、有时干脆把整段结论写在 `Thought` 里。

**超步数的判定比 FC 宽松：**

```
if iteration_step == max_iteration_steps and scratchpad.action:
    if scratchpad.action.action_name.lower() != "final answer":
        raise AgentMaxIterationError(...)
```

**最后一轮如果动作是 `final answer`，不算超步数。** 而 FC 那边同一位置的判断是 `and tool_calls`——没有这个豁免，因为 FC 的最终答案不是「一次工具调用」。

**`_handle_invoke_action` 与 FC 的工具调用有四处差异：**

1. 参数是 `str` 时先 `json.loads`，且用 `contextlib.suppress(json.JSONDecodeError)` —— **解析失败就保持字符串原样**，不报错；
2. 找不到工具时返回 `(answer, ToolInvokeMeta.error_instance(answer))` 二元组，**没有 `meta` 字典**（FC 那边是四字段字典）；
3. 不接收 `message_id`/`conversation_id`/`app_id`（FC 传了这四个）；
4. 返回 `(tool_invoke_response, tool_invoke_meta)` 而 FC 返回三元组（多了 `message_files`）——但 CoT 其实也在内部处理了文件推送，只是不向上返回。

**这两个执行器的接口形状不一致，而它们共享基类。** 基类只统一了「准备工具 / 落库思考 / 组织历史」，没有统一「一轮怎么跑」。这也是为什么 `run` 的签名都不同：FC 是 `run(session, message, query, **kwargs)`，CoT 是 `run(session, message, query, inputs)` —— **`inputs` 是必填位置参数，用于填充提示词模板里的 `{{key}}` 占位符**（`_fill_in_inputs_from_external_data_tools` 做字符串替换）。

**`_fill_in_inputs_from_external_data_tools` 用裸字符串替换：**

```
instruction = instruction.replace(f"{{{{{key}}}}}", str(value))
```

`{{{{` 与 `}}}}` 在 f-string 里转义成 `{{` 与 `}}`，也就是匹配 `{{key}}` 形式的占位符。整段包在 `try/except Exception: continue` 里——**替换失败就跳过这个变量**，提示词里留着未替换的 `{{key}}`。这类「容错到静默」的处理让模板错误很难发现。

## 关键取舍

**源码事实：** 两个执行器共享 `BaseAgentRunner`（工具准备、思考落库、历史组织、模型能力探测），但 `run` 的签名与返回形状不同：FC 返回 `Generator[LLMResultChunk]`，CoT 返回无类型参数 `Generator` 且多一个必填 `inputs`。

**源码事实：** 迭代上限 `min(max_iteration, 99) + 1`，第 `max_iteration_steps` 轮清空工具列表作为「收尾轮」；FC 在该轮仍要调工具则抛 `AgentMaxIterationError`，CoT 额外豁免 `final answer` 动作。

**源码事实：** 模型调用前显式 `session.commit()` + `session.close()`；`BaseAgentRunner.__init__` 结尾也 `session.close()`。

**源码事实：** 工具准备失败（`_init_prompt_tools`）被 `except Exception: continue` 静默跳过，注释给的预期原因是「api tool may be deleted」。

**源码事实：** 找不到工具时不抛异常，而是把「there is not a tool named X」作为 Observation 喂回模型。

**源码事实：** 同一轮内 `save_agent_thought` 被调用两次（先 thought/answer，后 observation），共用同一个 `agent_thought_id`。

**源码事实：** `final_answer` 在 FC 里是 `+=` 累加所有轮次的响应。

**源码事实：** CoT 的停止序列按 provider 硬编码例外（`_ignore_observation_providers = ["wenxin"]`）；无 action 时静默返回空答案；`Action Input` 的参数 JSON 解析失败时保持字符串。

**源码事实：** FC 里 `json.dumps(..., ensure_ascii=False)` 的 `TypeError` 兜底只是去掉 `ensure_ascii` 参数，并不能解决不可序列化对象。

**架构推论：** 「两种执行器」存在的原因是**能力差异而非偏好**：有些部署环境里的模型不支持原生工具调用，而产品的卖点又不能只服务一部分模型。于是文本协议必须存在，而它的复杂度全压在「从自由文本里可靠地抠出动作」这件事上（那个解析器是独立类，说明它足够重）。

**架构推论：** 「最后一次不给工具」是这类循环的通用收尾手法：它把「无法继续调工具」变成模型可感知的约束，从而逼出一个答案；而如果模型仍然坚持（FC 路线），就显式失败。**比「到步数直接截断」好得多**，因为截断会返回一个半成品且无人知晓。

**架构推论：** 「错误作为 Observation 喂回」是让 Agent 自纠的核心机制，但它的前提是**错误消息要面向模型写**。「there is not a tool named X」之所以可用，是因为模型能据此换工具；若换成一段堆栈，模型只会重复同样的调用。

**架构推论：** 一个 `BaseAgentRunner` 只统一了「外围」，没统一「核心循环」，说明这套抽象的层次选错了位：真正的差异（动作协议）恰好在最难抽象的地方。`dify-agent` 那个独立包（g02）正是对这件事的重新回答——**把循环本身做成一个可配置的运行时**，而不是让每个执行器各写一遍。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/agent/base_agent_runner.py` | `BaseAgentRunner` | 执行器基类：构造期准备、工具转换、思考落库、历史组织 |
| `api/core/agent/base_agent_runner.py` | `BaseAgentRunner._init_prompt_tools` | 工具准备，失败静默跳过 |
| `api/core/agent/base_agent_runner.py` | `BaseAgentRunner._convert_tool_to_prompt_message_tool` | 工具 → 提示词工具，并注入 `DifyRunContext` |
| `api/core/agent/base_agent_runner.py` | `BaseAgentRunner._convert_dataset_retriever_tool_to_prompt_message_tool` | 知识库工具的参数 schema（全 string） |
| `api/core/agent/base_agent_runner.py` | `BaseAgentRunner.update_prompt_message_tool` | 每轮刷新工具参数 schema |
| `api/core/agent/base_agent_runner.py` | `BaseAgentRunner.create_agent_thought` | 申请思考 id |
| `api/core/agent/base_agent_runner.py` | `BaseAgentRunner.save_agent_thought` | 同 id 两次写入（想法 + 观察） |
| `api/core/agent/base_agent_runner.py` | `BaseAgentRunner.organize_agent_history` | 组装历史消息 |
| `api/core/agent/fc_agent_runner.py` | `FunctionCallAgentRunner` | 原生函数调用执行器 |
| `api/core/agent/fc_agent_runner.py` | `FunctionCallAgentRunner.run` | 主循环：生成、解析工具调用、执行、落库 |
| `api/core/agent/fc_agent_runner.py` | `FunctionCallAgentRunner.check_tool_calls` | 流式块里是否含工具调用 |
| `api/core/agent/fc_agent_runner.py` | `FunctionCallAgentRunner.extract_tool_calls` | 从流式块取出 `(id, name, args)` |
| `api/core/agent/fc_agent_runner.py` | `FunctionCallAgentRunner.extract_blocking_tool_calls` | 非流式响应里的同一提取 |
| `api/core/agent/fc_agent_runner.py` | `FunctionCallAgentRunner._build_dataset_tool_image_contents` | 知识库检索结果里的图片补丁 |
| `api/core/agent/cot_agent_runner.py` | `CotAgentRunner` | 文本协议（ReAct）执行器基类，`ABC` |
| `api/core/agent/cot_agent_runner.py` | `CotAgentRunner.run` | 主循环：流式解析 scratchpad、判定 final answer |
| `api/core/agent/cot_agent_runner.py` | `CotAgentRunner._handle_invoke_action` | 工具调用与参数 JSON 容错 |
| `api/core/agent/cot_agent_runner.py` | `CotAgentRunner._init_react_state` | 初始化 scratchpad 与历史 |
| `api/core/agent/cot_agent_runner.py` | `CotAgentRunner._fill_in_inputs_from_external_data_tools` | 提示词占位符替换 |
| `api/core/agent/cot_agent_runner.py` | `CotAgentRunner._format_assistant_message` | scratchpad → 提示词文本 |
| `api/core/agent/cot_agent_runner.py` | `ActionDict` | 解析产物的形状约定 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/agent/base_agent_runner.py](https://github.com/langgenius/dify/blob/main/api/core/agent/base_agent_runner.py)
- [dify 源码：core/agent/fc_agent_runner.py](https://github.com/langgenius/dify/blob/main/api/core/agent/fc_agent_runner.py)
- [dify 文档：Agent 节点](https://docs.dify.ai/guides/workflow/node/agent)

## 与主核 / AI 的关系

两种执行器与主核里「同一种语义有两种执行路径」的情况完全对应：主核的表达式求值既可走「解释执行」也可走「编译成字节码」，两条路产出同样结果但代价曲线不同。**关键判断不是「哪条更快」，而是「哪条在什么约束下不可用」**——主核里某些表达式无法编译，这里某些模型不支持原生工具调用。

**「最后一次不给工具」vs 数据库的 `LIMIT` 语义。** 数据库的 `LIMIT` 是「最多给你这么多元组」，不改变查询计划；这里的「摘掉工具」是**改变输入本身**——它把「你要继续调工具」这个选项从模型的可选空间里删掉。这是个更强的干预手段，只在「必须收敛」的场合用。**约束输入比约束输出更可靠**：后者靠模型自觉，前者是物理限制。

**同一 id 两次写入 vs 数据库的 WAL 两阶段。** 一次 Agent 思考在数据库里表现为「先写想法、后写观察」两次更新同一个主键。这不是事务内的两阶段，而是**面向用户可见性的渐进披露**：中间态必须可观测，否则用户看不到「正在调工具」。这与数据库「先写 WAL 再写页」的顺序要求逻辑不同但形式相近——**都是把一件事拆成两个可观测的时点**。

**按 provider 硬编码例外 vs 数据库的兼容性开关。** `_ignore_observation_providers = ["wenxin"]` 与数据库里那些「这个存储引擎不支持并行扫描」的硬编码判断是同一类东西：**都是把「某个实现对某个协议的不遵从」写成了代码里的特例**。这类清单只会变长，所以正确做法是尽量让它集中（这里做到了：一个类属性）并在每次新增 provider 时检查。

对 agent 工程四条结论。其一：**执行器要按「模型能力」分而不是按「产品偏好」分**，并且必须准备好文本协议这一路，否则在不支持工具调用的模型上整个功能不可用。其二：**收尾轮要摘掉工具**，让「无法继续」成为模型的物理约束，而不是靠提示词请求它停止。其三：**错误消息要面向模型写**，因为它是 Observation 的一部分，会被当作下一轮的输入。其四：**共享基类只统一外围时，说明核心循环才是真正的差异点**——那时应该考虑把循环本身参数化（`dify-agent` 的做法），而不是让每个执行器各写一遍。
