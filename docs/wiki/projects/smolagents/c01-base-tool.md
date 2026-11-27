---
project: smolagents
feature: base-tool
title: 工具基类与两种提示形态
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/tools.py
    symbols: [BaseTool, Tool, to_code_prompt, to_tool_calling_prompt]
---

# 工具基类与两种提示形态

## 一句话职责

`Tool` 是「一个可被智能体调用的能力」的统一表示：它把元数据（名字、描述、输入/输出类型）声明为类属性，把行为收进 `forward`，并在构造期用一次严格自检保证声明与实现一致；同一个对象能渲染成两种完全不同的提示形态，因为 smolagents 有两种智能体。

## 机制是什么

工具在 smolagents 里不是「函数 + JSON schema」，而是**类**。这条选择决定了后面一切：

- **声明式元数据**：`name`、`description`、`inputs`、`output_type` 都是类属性，用户可以只写这四个 + `forward` 就得到一个合法工具。
- **`inputs` 自带受限类型体系**：不是任意 JSON schema，而是 `AUTHORIZED_TYPES` 里的十个字符串（`string` / `boolean` / `integer` / `number` / `image` / `audio` / `array` / `object` / `any` / `null`）。每个输入是 `{"type": ..., "description": ...}`，还可选 `nullable`。
- **两种渲染**：`CodeAgent` 把工具当 Python 函数用，需要的是一段可读的函数签名 + docstring；`ToolCallingAgent` 走模型的 tool calling 接口，需要的是名字 + 描述 + 输入字典。这两个形态分别由 `to_code_prompt` 与 `to_tool_calling_prompt` 产出，由提示词模板直接调用。

`BaseTool` 只约定两件事：有 `name`，以及可调用。`Tool` 在其上加全部脚手架。`agents.py` 里 `_setup_tools` 的断言就是对着 `BaseTool` 做的——所以第三方可以从头实现一个不自检的工具，但绝大多数人走 `Tool`。

## 怎么实现的

**构造期自检靠装饰器拦截 `__init__`。** `Tool.__init_subclass__` 在**每个子类**定义时调用 `validate_after_init(cls)`，把该子类的 `__init__` 换成一个「先跑原 `__init__`，再跑 `self.validate_arguments()`」的包装。注意这是逐子类包装：继承链上每一层都会被包一次，所以实例化最末子类时会一路校验上去（幂等，因为校验只读属性）。

`validate_arguments` 做的事按顺序：

1. 四个必需属性存在且类型正确（`description`/`name` 为 `str`，`inputs` 为 `dict`，`output_type` 为 `str`）；`output_schema` 可选但必须是 `dict`。
2. `name` 必须是合法 Python 标识符且不是保留字（`is_valid_name`）——这条约束的理由在 CodeAgent：工具在生成的代码里是**函数名**，不合法就写不出可跑的代码。
3. 每个 input 必须同时有 `type` 与 `description`；`type` 可以是字符串或字符串列表，逐个核在 `AUTHORIZED_TYPES` 里；`output_type` 同理。
4. **`forward` 的形参必须与 `inputs` 的键完全一致**（`actual_keys != expected_keys` 就报错，错误文案明确要求「`self` 之后依次对齐 `inputs` 的键」）。
5. `_convert_type_hints_to_json_schema(self.forward, error_on_missing_type_hints=False)` 再核一遍：每个 `inputs` 的键都要出现在函数签名的 JSON schema 里；`nullable` 必须两边同时声明——只在 `inputs` 里写 `nullable` 或只在类型注解里写 `Optional[...]` 都报错。

第 4、5 条可以被 `skip_forward_signature_validation = True` 整体跳过。源码注释点名了三类需要跳过的：`PipelineTool`、`SpaceToolWrapper`、`LangChainToolWrapper`——它们的 `forward` 是 `*args, **kwargs` 或直接赋成外部对象的方法，无法按固定签名对齐。

**`__call__` 与 `forward` 的分工。** 外部一律调 `__call__`，它做三件事再交给 `forward`：

- 惰性 `setup()`：`is_initialized` 为假时先调。`Tool.setup` 默认只置位，`PipelineTool` 覆写成真正加载模型。这就是「重操作在首次使用时才做」的落点。
- **单 dict 参数解包**：若只传了一个位置参数且它是 dict，并且它的所有键都在 `self.inputs` 里，就把它当关键字参数。这让「模型按 tool calling 习惯传一个 JSON 对象」与「代码里按位置传」都能落到同一个 `forward` 上。
- 可选的 `sanitize_inputs_outputs`：开启时过 `handle_agent_input_types` / `handle_agent_output_types`，把 `image`/`audio` 这类模态在原始值与 `AgentImage`/`AgentAudio` 之间转换。只有 gradio 演示路径会打开它。

**`to_code_prompt`：把工具渲染成一个假的函数定义。** 输出形如：

```
def <name>(<参数名>: <类型>, ...) -> <output_type>:
    """<description>

    Args:
        <参数名>: <描述>

    Returns:
        ...
    """
```

文本构造里有几处非显然的处理：

- **`output_schema` 存在时，返回类型改写成 `dict`**，并在描述里追加一句「这个工具返回结构化输出，用 JSON schema 直接取字段，不需要 print」——注释说明这是为了照顾小模型（点了 Mistral Small、Gemma 3）。同时把 schema 以四空格缩进写进 `Returns:` 段。
- 类型用的是 `inputs` 里声明的**字符串**（`string`、`integer`…），不是 Python 类型名。对模型来说是「这是字符串值」，对解释器来说它只是提示文本，不参与类型检查。
- 整个 docstring 用 `""""""` 包起来再整体缩进四空格，所以它能被塞进代码块里。

**`to_tool_calling_prompt`：一行式。** 返回 `name: description\n    Takes inputs: {inputs}\n    Returns an output of type: {output_type}`。直接把 `inputs` 这个 dict 的 `repr` 印出来——因为对 tool calling 而言精确 schema 由接口层另给（见 d03、d04），提示里的这份只是让人（模型）读懂。

**两种形态的真正开关在模板。** `prompts/code_agent.yaml` 与 `prompts/structured_code_agent.yaml` 里是 `{{ tool.to_code_prompt() }}`，`prompts/toolcalling_agent.yaml` 里是 `{{ tool.to_tool_calling_prompt() }}`。也就是说渲染方式不是工具自己决定的，而是由「哪种智能体在拼提示词」决定。这正是同一个 `Tool` 对象能被两种智能体共用的原因。

**`PipelineTool` 是这条抽象的一次压力测试。** 它继承 `Tool`，但把调用拆成三段：`encode`（预处理）→ `forward`（模型前向）→ `decode`（后处理），并覆写 `__call__` 在自己的控制下按顺序跑这三段，中间还插了 `send_to_device` 把 tensor 搬到设备上、结束后搬回 CPU。`setup` 里做真正的重量级加载：`pre_processor_class.from_pretrained` / `model_class.from_pretrained`，再挑设备（有 `device_map` 就用它的第一个值，否则用 `accelerate.PartialState().default_device`）。它的构造期检查是「装了 accelerate 与 torch 吗」，缺了直接 `ModuleNotFoundError` 并给出 pip extra 名称。

## 关键取舍

**源码事实：** 工具是类而非函数，且 `__init_subclass__` 用 `validate_after_init` 包装每个子类的 `__init__`。这使「漏写 `inputs` 或 `forward` 签名不一致」在**实例化时**就失败，而不是等到第一次调用。

**源码事实：** `name` 必须是合法 Python 标识符且非保留字。这条约束只对 CodeAgent 有意义，但对所有工具强制——因为同一种工具要能在两种智能体间复用。

**源码事实：** `inputs` 的类型是十个受限字符串，不是任意 JSON schema。想表达联合类型只能写成字符串列表，想表达可空只能写 `nullable: True`。

**源码事实：** `nullable` 必须两边同时声明（`inputs` 里与类型注解里），只写一边会被构造期检查拒绝。这是把「文档与实现不一致」变成硬错误的一处设计。

**源码事实：** `forward` 签名校验可以按类关闭（`skip_forward_signature_validation`），关闭者恰好是三类「包装外部接口」的工具——它们的签名由外部决定，无法对齐 `inputs`。

**架构推论：** 把「两种提示形态」做成 `Tool` 上的两个方法、由模板选择调用，避免了「为两种智能体各准备一套工具类型」。代价是每个工具类都带着一个它可能永远用不到的渲染方法，且新增一种智能体形态就要给 `Tool` 加一个方法。

**架构推论：** `__call__` 里的「单 dict 解包」是一处实用主义妥协：它让「代码里 `tool(x=1)`」「代码里 `tool({"x": 1})`」「tool calling 层传 dict」三种调用方式归一。但也意味着如果一个工具只接受一个 dict 类型的参数，且该 dict 的键恰好与工具输入键同名，就会歧义——源码里没做进一步区分，靠 `all(key in self.inputs ...)` 这一个判据。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/tools.py` | `BaseTool` | 最小契约：有 `name`、可调用 |
| `src/smolagents/tools.py` | `Tool` | 工具基类：元数据声明、构造期自检、惰性 setup、调用归一 |
| `src/smolagents/tools.py` | `Tool.validate_arguments` | 五步自检：属性类型、名字合法性、inputs 形状、签名对齐、nullable 一致 |
| `src/smolagents/tools.py` | `Tool.__call__` | 惰性 setup + 单 dict 解包 + 可选模态转换后转发 `forward` |
| `src/smolagents/tools.py` | `Tool.setup` | 惰性初始化钩子，默认只置位 |
| `src/smolagents/tools.py` | `to_code_prompt` | 渲染成带 docstring 的函数定义，供代码式智能体 |
| `src/smolagents/tools.py` | `to_tool_calling_prompt` | 渲染成单行描述，供工具调用式智能体 |
| `src/smolagents/tools.py` | `validate_after_init` | 子类 `__init__` 包装器，把自检挂到实例化时刻 |
| `src/smolagents/tools.py` | `PipelineTool` | 三段式（encode/forward/decode）工具，`setup` 里真加载模型 |

## 参考

- [smolagents 源码：tools.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/tools.py)
- [smolagents 文档：工具总览](https://huggingface.co/docs/smolagents/tutorials/tools)
- [smolagents 文档：如何创建工具](https://huggingface.co/docs/smolagents/tutorials/tools#how-to-create-a-tool)
- [Python 文档：`__init_subclass__`](https://docs.python.org/3/reference/datamodel.html#object.__init_subclass__)

## 与主核 / AI 的关系

「把外部能力的契约做成一张声明表，再在构造期校验声明与实现是否一致」这套思路，与主核里的**系统目录**是同一类设计：主核的表定义声明列名与类型，执行层据此校验与规划；这里的类属性声明工具的名字与输入输出，框架据此渲染提示词与校验参数。两边的核心价值都在「一份声明，多处消费」——差别在主核的声明要支撑查询优化（因此类型必须精确、可比较），这里的声明要支撑自然语言理解（因此描述文本与类型同等重要）。

`nullable` 必须两边同时声明这条检查值得对照主核里的一致性检查（例如索引定义与表定义）。两者的动机完全相同：**允许声明与实现各说一套，等价于允许一个静默的 bug 长期存在**。把它们变成启动期/构造期错误，是因为这类不一致一旦上线就很难从行为上定位。

对 AI 工程的实际意义在于两点。第一，**工具应当是类而不是裸函数**这个选择带来的收益是可测试性：`validate_arguments` 可以在 CI 里对每个工具类跑一遍，把「忘了写描述」「签名与 inputs 不匹配」这类问题挡在运行之前。第二，**两种提示形态共存**说明了框架设计里一个常被忽视的成本：一旦支持两种智能体范式，工具的「对外表示」就不再唯一，任何新增的工具元数据都必须回答「它在两种形态里怎么显示」。smolagents 的答案是让模板决定，这是一个低耦合的解法，但它把负担转移给了每一个新增的提示形态。
