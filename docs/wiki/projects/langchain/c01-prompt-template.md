---
project: langchain
feature: prompt-template
title: 提示模板与消息占位
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/prompts/chat.py
    symbols: [ChatPromptTemplate, MessagesPlaceholder, BaseStringMessagePromptTemplate]
  - path: libs/core/langchain_core/prompts/few_shot.py
    symbols: [FewShotPromptTemplate, FewShotChatMessagePromptTemplate]
---

# 提示模板与消息占位

## 一句话职责

`ChatPromptTemplate` 把「一串消息模板」当成一个可调用的提示：它负责把各种宽松的写法（字符串、元组、消息对象、占位符、嵌套模板）统一成消息模板集合，在格式化时收集变量、替换值、并把结果摊平成消息列表。

## 机制是什么

这里的核心矛盾是**写法太多而语义只有一个**。用户想表达「这一条是 human 说的，内容模板是 `{question}`」，而源码支持至少五种写法：

1. `BaseMessagePromptTemplate` 实例
2. `BaseMessage` 实例
3. 二元组 `("human", "{question}")`
4. 二元组 `(HumanMessage, "{question}")`（第一个元素是类）
5. 裸字符串 `"{question}"`（默认当作 human）

五种写法全部归一到同一组对象，而**归一化的关键约束是「变量要从哪来」**：前两种自带模板（变量在模板里），中间两种要现造模板，最后一种要补一个默认角色。

第二个矛盾是**变量集合的推导**。一个提示模板的 `input_variables` 不是用户声明的，而是从所有消息模板里**推导**出来的。这就带来几个必须处理的情形：某个变量已经在 `partial_variables` 里填过（不该再算输入）、某个占位符是 `optional`（不该强制提供）、同一个变量在多处出现（只算一次）。

## 怎么实现的

**第一步：把五种写法归一成三种对象。**

```python
def _convert_to_message_template(message, template_format="f-string"):
    if isinstance(message, (BaseMessagePromptTemplate, BaseChatPromptTemplate)):
        message_ = message
    elif isinstance(message, BaseMessage):
        message_ = message
    elif isinstance(message, str):
        message_ = _create_template_from_message_type("human", message, template_format=template_format)
    elif isinstance(message, (tuple, dict)):
        ...
```

前两支持原样接受——因为已经是「模板」或「消息」，无需再造。后两支持需要现场构造。

**裸字符串默认补成 human**。这是一个便利性选择：写 `ChatPromptTemplate.from_messages(["hello"])` 得到一条 human 消息。默认角色选 human 而不是 system 是因为**未标注角色的文本几乎总是用户要说的话**，而系统提示通常会被显式写出来。

**二元组的第一个元素有三种可能，三种可能对应三条路径。**

```python
if isinstance(message_type_str, str):
    message_ = _create_template_from_message_type(message_type_str, template, template_format=template_format)
elif hasattr(message_type_str, "model_fields") and "type" in message_type_str.model_fields:
    message_type = message_type_str.model_fields["type"].default
    message_ = _create_template_from_message_type(message_type, template, template_format=template_format)
else:
    message_ = message_type_str(prompt=PromptTemplate.from_template(cast("str", template), template_format=template_format))
```

三条路径：

1. **字符串**（`"human"` / `"ai"` / `"system"` / `"placeholder"`）→ 走按名字分派的工厂。
2. **消息类**（`HumanMessage` 等）→ **从类上读判别位**：`message_type_str.model_fields["type"].default` 得到 `"human"`，再走同一个工厂。这一步很巧妙——它不需要一张「类 → 角色名」的映射表，而是**问了类自己的默认值**。所以任何继承 `BaseMessage` 且声明了 `type` 默认值的类都能用，包括用户自定义的。
3. **其他**（例如 `BaseMessagePromptTemplate` 的子类）→ 假定它是模板类，直接 `message_type_str(prompt=PromptTemplate...)` 构造。

第三条路径是一个「鸭子类型兜底」：只要它能接受 `prompt=` 参数就成。若不能，会在构造时抛 `TypeError` 而不是在这里被明确拒绝——所以这条路径的错误信息不如前两条友好。

**字典写法与二元组等价，但键名被严格限定。**

```python
if isinstance(message, dict):
    if set(message.keys()) != {"content", "role"}:
        msg = "Expected dict to have exact keys 'role' and 'content'." f" Got: {message}"
        raise ValueError(msg)
    message_type_str = message["role"]
    template = message["content"]
```

即**必须恰好是这两个键**。多于或少于都报错。用 `set(...) != {...}` 而不是「包含」判断，是因为要挡住拼写错误（`{"role": ..., "content": ..., "extra": ...}`）与缺失。严格在这里是对的：字典没有位置信息，若允许额外键就无法判断多余的键该不该被当成模板数据。

**占位符 `"placeholder"` 特殊在于它不是「造一个消息」而是「造一个接收消息的洞」。**

```python
elif message_type == "placeholder":
    if isinstance(template, str):
        if template[0] != "{" or template[-1] != "}":
            msg = f"Invalid placeholder template: {template}. Expected a variable name surrounded by curly braces."
            raise ValueError(msg)
        var_name = template[1:-1]
        message = MessagesPlaceholder(variable_name=var_name, optional=True)
```

注意两个细节：

- **模板必须是 `{...}` 包起来的一个变量名**，代码只检查首尾是不是花括号。这是一种**宽松校验**：`"{a}b}"` 会通过检查但 `var_name` 变成 `"a}b"`。真正的合法性由后面格式化时的变量收集决定。
- **通过 `"placeholder"` 写法构造的占位符默认 `optional=True`**。而直接 `MessagesPlaceholder("history")` 构造时默认 `optional=False`。**同一个类，两条入口给出了相反的默认值**。这个不一致值得记：它意味着「用 `("placeholder", "{history}")` 元组写法」与「用 `MessagesPlaceholder("history")`」的行为不同——前者缺变量返回空列表，后者抛 `KeyError`。源码没有注释说明这个选择，但从便利性看，元组写法是简写形态，故取更宽松的默认。

**列表写法（二元组）在占位符情形下还给出一个「变量名 + 是否可选」的形态。**

```python
try:
    var_name_wrapped, is_optional = template
except ValueError as e:
    msg = (...)
    raise ValueError(msg) from e
```

即 `(("placeholder", ["{history}", True]))` 这种写法。`is_optional` 必须是 `bool`，变量名必须是 `{...}` 形状。这段校验比字符串形态严格（区分了两种类型），因为它承载了「可选性」这一额外信息。

**`MessagesPlaceholder.format_messages` 是占位符的实际行为，而它的三分支决定了「缺变量时怎么办」。**

```python
value = (
    kwargs.get(self.variable_name, [])
    if self.optional
    else kwargs[self.variable_name]
)
if not isinstance(value, list):
    msg = f"variable {self.variable_name} should be a list of base messages, got {value} of type {type(value)}"
    raise ValueError(msg)
value = convert_to_messages(value)
if self.n_messages:
    value = value[-self.n_messages :]
return value
```

四处：

- **`optional` 决定用 `kwargs.get(..., [])` 还是 `kwargs[...]`**。前者缺失时给空列表，后者缺失时抛 `KeyError`。文档把这条说得极其清楚：「If `False` then a named argument with name `variable_name` must be passed in, **even if the value is an empty list**」。即非可选形态下，连「我要传空」都必须显式写出来。
- **必须是 `list`**。传字符串或元组都报错。这个严格要求来自占位符的语义：它要放**多条**消息，所以必须是列表。
- **`convert_to_messages(value)` 做元素级转换**。所以列表里可以放元组（`("human", "hi")`）或 dict，不必都是 `BaseMessage`。这一点让占位符能接住「刚查出来的会话历史」这类原始数据。
- **`n_messages` 取**最后 N 条**（`value[-n:]`）**。这是「只保留最近几条历史」的实现。取尾部而非头部，因为对话历史里最近的部分最相关。注意它作用在转换后的消息列表上，所以计数单位是消息条数而非 token 数。

`n_messages` 的类型是 `PositiveInt` 而不是 `int`——即**不允许 0 或负数**。这个约束由 pydantic 强制执行，比运行期检查 `if self.n_messages and ...` 更早、更清晰。而 `if self.n_messages:`（真值判断）在此处安全，因为 `PositiveInt` 排除了 0。

**变量的推导是 `ChatPromptTemplate` 的核心校验，它比「收集」复杂得多。**

```python
@model_validator(mode="before")
@classmethod
def validate_input_variables(cls, values):
    messages = values["messages"]
    input_vars: set[str] = set()
    optional_variables = set()
    input_types: dict[str, Any] = values.get("input_types", {})
    for message in messages:
        if isinstance(message, (BaseMessagePromptTemplate, BaseChatPromptTemplate)):
            input_vars.update(message.input_variables)
        if isinstance(message, MessagesPlaceholder):
            if "partial_variables" not in values:
                values["partial_variables"] = {}
            if message.optional and message.variable_name not in values["partial_variables"]:
                values["partial_variables"][message.variable_name] = []
                optional_variables.add(message.variable_name)
            if message.variable_name not in input_types:
                input_types[message.variable_name] = list[AnyMessage]
    if "partial_variables" in values:
        input_vars -= set(values["partial_variables"])
    if optional_variables:
        input_vars -= optional_variables
    ...
```

这段逻辑有六件事，逐条看：

1. **递归收集**：`BaseChatPromptTemplate` 也是 `input_variables` 的来源——所以模板可以嵌套模板，内层的变量会冒泡到外层。
2. **占位符的变量特殊处理**：它不进 `input_vars`（因为它不是从 `input_variables` 拿的），而是走下面两条。
3. **可选占位符被填成 `partial_variables[name] = []`**。这是一个很实在的机制：把「可选」实现成「预先用空列表填好」。于是格式化时变量永远存在（就是那个空列表），`MessagesPlaceholder` 的 `optional` 分支甚至不必生效。这也解释了为什么可选变量必须记住（`optional_variables.add`）——否则它会被算成「已满足的输入」而不出现在任何地方。
4. **占位符的 `input_types` 被设成 `list[AnyMessage]`**。这是给「类型检查 / schema 生成」用的信息：这个变量期望的是消息列表而不是字符串。
5. **`input_vars -= set(values["partial_variables"])`**：已经用 partial 填过的变量不算输入。这条让 `prompt.partial(question="fixed")` 之后 `input_variables` 自动收缩。
6. **`input_vars -= optional_variables`**：可选的不算必填输入。

最后一段是**严格校验与自动推导的分叉**：

```python
if "input_variables" in values and values.get("validate_template"):
    if input_vars != set(values["input_variables"]):
        msg = "Got mismatched input_variables. " f"Expected: {input_vars}. " f"Got: {values['input_variables']}"
        raise ValueError(msg)
else:
    values["input_variables"] = sorted(input_vars)
```

- **用户显式给了 `input_variables` 且开了 `validate_template`** → 必须与推导结果**完全相等**（不是包含关系）。报错信息同时打出两侧集合，便于对比。
- **否则** → 用推导结果覆盖用户的声明，且 `sorted(...)` 保证**顺序确定**。排序是为了让「同一份模板得出同一个 `input_variables` 列表」——这对提示的可复现性与快照测试都很重要。

`validate_template` 默认 `False`，所以常规路径是「自动推导并覆盖」。即**用户手写的 `input_variables` 默认会被忽略**——除非显式开启校验。这个默认值的选择避免了「手写清单与模板漂移」这一类常见问题，代价是用户无法通过手写清单来缩小变量集（只能靠 `partial`）。

**`format_messages` 只做一件事：把每条模板的产出拼接起来，但三种输入的处理不同。**

```python
kwargs = self._merge_partial_and_user_variables(**kwargs)
result = []
for message_template in self.messages:
    if isinstance(message_template, BaseMessage):
        result.extend([message_template])
    elif isinstance(message_template, (BaseMessagePromptTemplate, BaseChatPromptTemplate)):
        message = message_template.format_messages(**kwargs)
        result.extend(message)
    else:
        msg = f"Unexpected input: {message_template}"
        raise ValueError(msg)
return result
```

三处：

- **`_merge_partial_and_user_variables` 先合并 partial**。所以 partial 的优先级低于调用时传的值吗？取决于实现——但关键点是**合并发生在格式化之前**，于是所有模板看到同一份完整变量集。
- **`BaseMessage` 直接 append（用 `extend([...])` 的写法）**，因为它已经是成品，不需要格式化。
- **用 `extend` 而不是 `append`**：一条模板可能产出**多条**消息（例如 `FewShotChatMessagePromptTemplate` 产出 N 条，或一个模板类自己返回列表）。所以统一用 `extend`，让「一进多出」成为合法情形。

`aformat_messages` 是完全对称的异步版本，行为一致——只是把 `format_messages` 换成 `await ... .aformat_messages(...)`。这类「同步与异步完全平行」的实现是必要的：提示格式化可能涉及异步 I/O（占位符要走检索器去取历史）。

**`FewShotPromptTemplate` 与 `FewShotChatMessagePromptTemplate` 共用同一个 mixin，而 mixin 里那条校验是**互斥**的。**

```python
examples = values.get("examples")
example_selector = values.get("example_selector")
if examples and example_selector:
    msg = "Only one of 'examples' and 'example_selector' should be provided"
    raise ValueError(msg)
if examples is None and example_selector is None:
    msg = "One of 'examples' and 'example_selector' should be provided"
    raise ValueError(msg)
```

即**恰好给一个**。这个互斥有两个实际理由：`_get_examples` 的分支依赖它（两个都给了就不知道走哪条）；且「静态示例」与「动态选择」在语义上不可混用。

注意第一支用 `if examples and`（真值判断）而第二支用 `if examples is None and`（身份判断）。这个不一致有意义：**空列表 `examples=[]` 在第一支里不算「给了」**（真值假），但在第二支里也**不算「没给」**（不是 `None`）。于是 `examples=[]` 加 `example_selector=None` 会触发第二条错误——即「给了空列表等于没给」。而 `examples=[]` 与 `example_selector=X` 同时出现时，第一支不触发（空列表是假），第二支也不触发（两者都不是 `None`），于是**构造成功**并走 selector 分支。

这个边界行为有点绕，但从「示例为空就是没有示例」的角度是合理的。

**`_get_examples` 的三支与两个版本（同步/异步）对应。**

```python
def _get_examples(self, **kwargs):
    if self.examples is not None:
        return self.examples
    if self.example_selector is not None:
        return self.example_selector.select_examples(kwkwargs)
    raise ValueError(...)
```

注意 `is not None` 而不是真值判断：所以 `examples=[]` 在这里会返回空列表（而不是跌到 selector 分支）。这与上面校验的第二支一致。

而 `_aget_examples` 只把 `select_examples` 换成 `await aselect_examples(...)`。这是「sync/async 成对」的又一实例。

**`FewShotPromptTemplate.format` 的组装顺序，以及一次「额外过滤」。**

```python
kwargs = self._merge_partial_and_user_variables(**kwargs)
examples = self._get_examples(**kwargs)
examples = [{k: e[k] for k in self.example_prompt.input_variables} for e in examples]
example_strings = [self.example_prompt.format(**example) for example in examples]
pieces = [self.prefix, *example_strings, self.suffix]
template = self.example_separator.join([piece for piece in pieces if piece])
return DEFAULT_FORMATTER_MAPPING[self.template_format](template, **kwargs)
```

四处：

1. **`{k: e[k] for k in self.example_prompt.input_variables}` 是投影**：每个示例字典里可能有很多键，但只有 `example_prompt` 需要的那些会被取用。直接 `**example` 会把多余键传给模板，可能触发 `extra` 校验失败。所以这里显式收窄。
2. **`example_separator.join([piece for piece in pieces if piece])`**。默认分隔符是 `"\n\n"`。注意那个**空值过滤**：`prefix` 默认 `""`、`suffix` 必须给但可能为空，`example_strings` 可能为空列表。若不滤掉空串，`"\n\n".join(["", "x", ""])` 会产生前后两个多余的空行。
3. **示例在模板化之前先被字符串化，然后整段再走一次格式化**。这是一个**两阶段格式化**：先 `example_prompt.format(**example)` 把每个示例变成一段文本，拼成完整模板文本，再 `DEFAULT_FORMATTER_MAPPING[template_format](template, **kwargs)` 用用户输入填外层的变量。
4. **`kwargs` 在第二阶段被整体传入**。这意味着**示例里没被用到的变量也会出现在第二阶段的格式化上下文里**——若它们不出现在模板文本中就无害，但若模板文本里恰好有同名占位符，就会被填。

第 2 阶段的这个设计有一个真实后果：`get_template_variables(self.prefix + self.suffix, ...)` 只在 `prefix + suffix` 上收集变量（见 `template_is_valid` 校验），所以**示例文本里的 `{...}` 不会被当成模板变量**——除非它们保留到了那一段。而 `example_prompt.format` 已经把示例里的变量填掉了，所以通常不会有残留。

**`FewShotChatMessagePromptTemplate` 的产出是消息列表而非字符串，所以组装方式完全不同。**

```python
examples = self._get_examples(**kwargs)
examples = [{k: e[k] for k in self.example_prompt.input_variables} for e in examples]
return [
    message
    for example in examples
    for message in self.example_prompt.format_messages(**example)
]
```

即**两重列表推导**：外层过示例、内层过该示例产生的消息。所以一条示例可以产出多条消息（如 `(human 问) + (ai 答)` 两条）。这正是它的类文档举的例子：「each example will become 2 messages: 1 human, and 1 AI」。

与 `FewShotPromptTemplate` 的差别：**没有 prefix/suffix/separator 参数**。因为它在 `ChatPromptTemplate.from_messages([...])` 里作为**一个成员**使用，前后消息由外层模板提供。这是一个干净的分工：字符串版负责「一整段文本的全部组装」，消息版只负责「一段示例消息」。

注意它的 `input_variables` 需要显式声明（`Field(default_factory=list)`），文档写「A list of the names of the variables the prompt template will use to pass to the `example_selector`, if provided」。即**这些变量是喂给选择器的**，不是喂给 `example_prompt` 的。这个区分很重要：`example_prompt` 的变量由它自己（`example_prompt.input_variables`），而选择器的输入变量需要单独声明。

**两个 few-shot 类都不支持序列化，且文档直说了。**

```python
@classmethod
def is_lc_serializable(cls) -> bool:
    """Return `False` as this class is not serializable."""
    return False
```

理由是它们持有 `example_selector`（可能内含向量库、嵌入模型等不可序列化对象）。源码没有注释，但这个否定是合理的保守选择——与其尝试序列化并在反序列化时失败，不如声明不支持。而 `FewShotPromptTemplate.save` 里还有一道显式检查：

```python
if self.example_selector:
    msg = "Saving an example selector is not currently supported"
    raise ValueError(msg)
```

即**静态示例可以存盘，带选择器的不行**。这条与 `is_lc_serializable = False` 看起来矛盾（后者对整个类说不），但实际上 `save` 走的是另一条路径（字典导出），所以静态示例仍有存盘能力。

## 关键取舍

**源码事实：** `_convert_to_message_template` 支持五种写法，其中裸字符串默认补成 human；二元组第一个元素若为消息类，通过 `model_fields["type"].default` 从类上读角色名，无需映射表。

**源码事实：** 字典写法要求键**恰好**是 `{"role", "content"}`，用集合相等判断而非包含判断。

**源码事实：** 通过 `("placeholder", "{name}")` 元组写法构造的占位符默认 `optional=True`，而直接 `MessagesPlaceholder("name")` 默认 `optional=False`——同一类两条入口给出相反默认。

**源码事实：** `MessagesPlaceholder.format_messages` 要求变量是 `list`，用 `convert_to_messages` 做元素级转换，`n_messages` 取**尾部** N 条，且 `PositiveInt` 排除 0 与负数。

**源码事实：** 可选占位符通过把 `partial_variables[name]` 预填为 `[]` 来实现「可选」，并在推导输入变量时把可选变量与已 partial 的变量都从必填集合里减去。

**源码事实：** `input_variables` 默认由模板推导并 `sorted` 覆盖用户声明；只有显式 `validate_template=True` 才与用户声明做**完全相等**的严格比对。

**源码事实：** `format_messages` 对 `BaseMessage` 直接收集、对模板调 `format_messages` 并 `extend`（因为一条模板可产出多条消息）；同步与异步方法完全平行。

**源码事实：** 两个 few-shot 类共用 mixin，`examples` 与 `example_selector` **恰好给一个**；`examples=[]` 在「不能同时给」分支里算「没给」，在 `_get_examples` 里却返回空列表。

**源码事实：** `FewShotPromptTemplate.format` 是两阶段格式化：先把每个示例字符串化并 join 成完整模板文本，再用用户变量整段格式化；示例字典先按 `example_prompt.input_variables` 投影，空片段在 join 前被滤掉。

**源码事实：** `FewShotChatMessagePromptTemplate` 不做 prefix/suffix/separator，只产出一段示例消息（用两重推导支持「一条示例产出多条消息」）。

**架构推论：** 「五种写法归一」这类设计的代价是**错误信息的位置分散**。字符串写法与二元组字符串写法都走 `_create_template_from_message_type`（错误信息友好），而「其他类」那条鸭子类型兜底会在构造时抛 `TypeError`。所以同一个「写错了」的用户体验取决于用了哪种写法。

**架构推论：** 「可选占位符预填空列表」是一个把**语法便利**降级成**数据约定**的手法。它让「可选的变量」在内部根本不存在第零个状态——永远是「有值」（可能为空列表）。好处是格式化阶段不必再判可选性；代价是 `input_variables` / `optional_variables` / `partial_variables` 三处都要保持一致，任一处漏了就会误报或误放行。

**架构推论：** `input_variables` 默认自动推导并覆盖用户声明，是「默认反对手写清单」的选择。手写清单的经典问题是**与模板漂移**（改了模板忘了改清单），而这种漂移在格式化时才以 `KeyError` 或「多余的变量」形式暴露。默认推导消灭了这个问题，代价是失去了「用清单缩小变量集」的能力——那只能靠 `partial`。

**架构推论：** 两阶段格式化（先示例后整体）在 `FewShotPromptTemplate` 里是必需的（因为它最终要产出一段文本），但它带来一个隐式约束：**示例文本不能包含会被第二阶段误当占位符的花括号**。源码没有防护这一点——若某个示例值里恰好有 `{something}`，第二阶段会尝试替换它。这是一个可被利用的注入面（示例数据若来自外部，就能影响外层模板）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/prompts/chat.py` | `ChatPromptTemplate` | 消息模板集合，负责变量推导与格式化分派 |
| `libs/core/langchain_core/prompts/chat.py` | `ChatPromptTemplate.validate_input_variables` | 变量收集、可选/partial 减法、严格或自动覆盖 |
| `libs/core/langchain_core/prompts/chat.py` | `ChatPromptTemplate.from_messages` | 宽松写法的入口 |
| `libs/core/langchain_core/prompts/chat.py` | `MessagesPlaceholder` | 接收消息列表的占位符，支持 `optional` 与 `n_messages` |
| `libs/core/langchain_core/prompts/chat.py` | `BaseStringMessagePromptTemplate` | 字符串模板 + 消息类的桥，`from_template` 工厂 |
| `libs/core/langchain_core/prompts/chat.py` | `_create_template_from_message_type` | 按消息类型名分派，含 `placeholder` 的两种写法 |
| `libs/core/langchain_core/prompts/chat.py` | `_convert_to_message_template` | 五种写法的归一逻辑 |
| `libs/core/langchain_core/prompts/few_shot.py` | `_FewShotPromptTemplateMixin` | 共享的 `examples`/`example_selector` 互斥校验与取值 |
| `libs/core/langchain_core/prompts/few_shot.py` | `FewShotPromptTemplate` | 字符串版：prefix + 示例 + suffix 的两阶段组装 |
| `libs/core/langchain_core/prompts/few_shot.py` | `FewShotChatMessagePromptTemplate` | 消息版：只产出一段示例消息，不做前后缀 |

## 参考

- [langchain 源码：prompts/chat.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/prompts/chat.py)
- [langchain 源码：prompts/few_shot.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/prompts/few_shot.py)
- [langchain 文档：提示模板](https://docs.langchain.com/oss/python/langchain/prompts)
- [Pydantic 文档：模型校验器](https://docs.pydantic.dev/latest/concepts/validators/)

## 与主核 / AI 的关系

「多种宽松写法归一到一个结构」与主核里的**语法糖归一**同源。主核接受 `SERIAL`、`IDENTITY`、`DEFAULT nextval(...)` 三种写法来表达「自增列」，内部统一成同一套序列依赖。归一的价值在于后续只需处理一种形状；代价在于**错误必须在归一之前报**，否则用户会看到一个与他的写法无关的错误。

「变量从模板推导而非声明」与主核里的**依赖分析**是同一件事：主核对一条查询要推导出「它引用了哪些表、哪些列」，而不是让用户声明。推导是可靠的（因为从语法树出发），声明的可靠性取决于人。默认推导 + 可选的严格比对，正是主核对待「用户 hint」的方式——hint 会被采纳但不作为真相来源。

「可选占位符预填空列表」这种「把可选性降级成默认值」的手法，与主核里**可空列的默认值**有相同的取舍结构：它消除了一个状态分支，但要求所有读取方都接受「空与缺失等价」。而这类约定一旦有一处漏判，就会出现「本该报错的地方悄悄通过」。

对 AI 工程的实际意义在于：提示模板是 agent 与模型之间唯一的输入契约，它的表达力直接决定「能不能把上下文组织成想要的样子」。这份实现给出的三条经验可直接用：**归一宽松写法时把错误挡在归一之前**、**变量从模板推导而不是让用户手写清单**、以及**把「可选」实现成默认值而不是一个额外状态**（但要保证所有消费点都同意这个等价）。
