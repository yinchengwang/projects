---
project: mcp-python-sdk
feature: elicitation
title: 诱导请求与结果形态
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/elicitation.py
    symbols: [AcceptedElicitation, DeclinedElicitation, render_elicitation_schema]
  - path: src/mcp/server/elicitation.py
    symbols: [CancelledElicitation, AcceptedUrlElicitation]
---

# 诱导请求与结果形态

## 一句话职责

让一个执行到一半的工具向客户端要一个答案：表单模式下把 Pydantic 模型渲染成**规范允许的极简 schema** 并在回程严格校验，URL 模式下只交换「用户愿不愿意去那个页面」这一个事实——而四种结果都被建模成可穷尽匹配的类型。

## 机制是什么

两种模式，各有一组结果类型：

| 模式 | 入口 | 结果类型 |
|---|---|---|
| 表单（form） | `elicit_with_validation(session, message, schema)` | `AcceptedElicitation[Model]` / `DeclinedElicitation` / `CancelledElicitation` |
| URL（url） | `elicit_url(session, message, url, elicitation_id)` | `AcceptedUrlElicitation` / `DeclinedElicitation` / `CancelledElicitation` |

两种模式共用后两个失败形态（拒绝、取消），差别只在「接受」：表单模式的接受携带数据（`data: ElicitSchemaModelT`），URL 模式的接受**不携带任何内容**——文档写得很明确：`"accept"` 只表示用户同意打开那个 URL，**不表示他们完成了对面的操作**。

`ElicitationResult` / `UrlElicitationResult` 是两个联合类型别名（前者用 `TypeAliasType` 以便携带类型参数）。

`render_elicitation_schema(schema)` 是把 Pydantic 模型转成规范可接受的 `requested_schema` 的入口。

## 怎么实现的

**四种结果形态的边界是「谁做了决定」。** `accept` 是「用户提交了表单 / 用户同意跳转」，`decline` 是「用户说了不」，`cancel` 是「用户把这个提示关掉了、没做选择」。文档特意强调了第三条与第二条的区别：`cancel` 是**没有选择**，不是拒绝。三者都**不是错误**——诱导文档里明说「拒绝不是错误，工具自己决定 decline 意味着什么，然后正常回答模型」。

而「无法提问」是第四种、也是唯一的错误情形：客户端没注册回调就没有声明 `elicitation` 能力，于是服务端的请求会以协议错误失败（文档里的实测输出是 `Elicitation not supported`）。这**不是** `decline`——工具拿到的是异常而不是拒绝结果。

**`AcceptedElicitation` 用 `Generic` 而不是在联合别名里重复声明。** `ElicitationResult` 用 `TypeAliasType(..., type_params=(ElicitSchemaModelT,))` 定义，因此 `ElicitationResult[MyModel]` 能工作。这一点很实用：工具函数把返回注解写成 `ElicitationResult[Confirm]` 时，`match result` 的每个分支都能拿到正确的 `data` 类型，而 `result.data` 在 `accept` 之外根本不存在——文档用「类型检查器会强制你先判断 `action`」来描述这个效果。

**「接受但没有内容」是一个必须显式处理的畸形情形。** `elicit_with_validation` 在 `action == "accept"` 且 `result.content is None` 时抛 `ValueError("Received an accepted elicitation with no content")`。这条分支存在是因为线格式允许 `accept` 不带 `content`，而模型的 `data` 字段是必填的——若不拦，构造 `AcceptedElicitation` 会以一条 pydantic 校验错误失败，错误信息会指向模型而不能说明真正的原因。

**回程校验做两次，且两次目的不同。** 先用 `schema.model_validate(result.content)` 校验并得到类型化数据，捕获 `ValidationError` 后转成 `ValueError`（文案：内容不匹配请求的 schema）。第二步是 pydantic 自身的构造校验。转成 `ValueError` 的意义在于：这是一个**来自外部的、语义明确的失败**，与模型定义有 bug 的情况区分开。文档举了反例：客户端给 `bool` 字段发 `"maybe"`，于是调用抛 `ValueError`、工具失败、业务代码里的 `if` 从未执行——「答案不会污染你的预订」。

**渲染方向的核心难点是规范比 Pydantic 窄得多。** 诱导 schema 只允许**扁平的原语字段**：`str`、`int`、`float`、`bool`，或者字符串的 `Literal`（变成 `enum`）。而 Pydantic 能表达的远多于此。于是代码用两步把差距关掉：

其一，**自定义 JSON Schema 生成器** `_ElicitationJsonSchema`，重写两个钩子：

- `nullable_schema` 直接把 `T | None` **展平成 `T`**。原因在 docstring 里：规范的 `PrimitiveSchemaDefinition` 既不接受 `anyOf` 也不接受 null 类型；一个可选字段的正确表达方式是**把它从 `required` 里拿掉**，而 pydantic 对任何有默认值的字段本来就这么做。
- `default_schema` 在默认值恰为 `None` 时**把 `default` 键删掉**。否则会生成 `"default": null`，同样不是规范接受的形状。

这两个钩子加在一起说明一件事：「可选」在规范里的表达方式与在 JSON Schema 里的表达方式不同，必须主动翻译而不是透传。

其二，**渲染后逐字段回验**。`_validate_rendered_properties` 把每个 `properties` 项喂给 `PrimitiveSchemaDefinition` 的 `TypeAdapter`，校验失败就抛 `TypeError`，消息里带字段名、渲染结果与规范名。docstring 说明了它是兜住「渲染器放过去了但规范不接受」的那一类：裸 `list[str]`（没有 enum）、多原语联合、嵌套模型。

两层是互补的：第一层处理**已知的形态差异**，第二层处理**没预料到的形态**。文档里那条实测错误信息就是这个兜底在起作用——把 `Address` 嵌进模型，报的是 `Elicitation schema field 'address' rendered as {'$ref': '#/$defs/Address'}, which is not a valid PrimitiveSchemaDefinition`。

**`PrimitiveSchemaDefinition` 是「内部表面包」里的真值来源。** 文件里有一句注释值得记住：它从 `mcp_types._v2025_11_25` 导入这个类型，作为**校验的单一真值源**，而不是自己手写一份允许列表。这样规范演化时校验会跟着走，不会出现「框架以为合法、客户端按规范渲染失败」的分裂。

**URL 模式的入口没有 schema 参数，因为不该有数据。** `elicit_url` 的签名只有 `message`、`url`、`elicitation_id`，返回的 `UrlElicitationResult` 里 `AcceptedUrlElicitation` 只有一个 `action` 字段。文档列出了适用场景并给了统一判据：**需要敏感凭据、OAuth 授权流、支付与订阅，以及任何「数据不该经过 LLM 上下文」的交互**。

**URL 模式的完成信号是另一个方法，不在返回值里。** docstring 明确：响应只表示用户是否同意跳转，真正的交互在带外发生；当诱导完成时，**服务端应当发一条 `ElicitCompleteNotification`** 告诉客户端。诱导文档把这件事说得更透：客户端拿到那个通知才知道可以停止显示「等待支付…」，否则只能猜。这是一个重要的接口形状选择——「已完成」是一个新事件而不是一次返回，因为完成的时刻与请求的时刻无关。

**`related_request_id` 把诱导挂回触发它的请求。** 两个函数的这个参数都是可选的，直接透传给 `session.elicit_form` / `session.elicit_url`。它的作用与订阅流里的同名参数一致：让客户端/服务端能把「这条出站请求」与「刚才那条入站请求」关联起来，做进度、取消与追踪。

**URL 模式的分支里有一个 `else: raise`。** 表单模式用「不是 accept、不是 decline 就当中 cancel」的宽松写法，URL 模式则把三个分支写全并加一个 `# pragma: no cover` 的兜底 `ValueError`。两种写法的差异有原因：URL 模式的 `action` 来自协议联合类型，穷尽写出三个合法值后剩下的是「协议给了意料之外的动作」，值得显式报错而不是静默归到 cancel。

**这套工具只在有反向通道的连接上工作。** `session.elicit_form` / `session.elicit_url` 的文档都注明会抛 `NoBackChannelError`，因为它们是**服务端向客户端**发起的请求。诱导文档给出了完整的时代边界：这条通道只存在于**经典握手的会话（规范 2025-11-25 或更早）**；在 `2026-07-28` 连接上不存在服务端发起的请求，这些调用会失败。那条文档同时给出了跨版本的替代路径——用 resolver（参数上挂 `Resolve(fn)`，返回 `Elicit(...)`），由框架在两种连接上分别适配。

## 关键取舍

**源码事实：** 「接受但不带内容」被显式拦成 `ValueError`，而不是让 pydantic 的必填字段校验去报错。错误消息指向真实原因。

**源码事实：** 回程校验失败被转成 `ValueError`（而非 `ValidationError`），把「外部数据不合规」与「模型定义有问题」分开。

**源码事实：** 可选字段在渲染时被展平成非可空类型，并由「不出现在 `required`」表达可选性；`default: null` 被主动删除。这是两种 schema 体系之间的翻译，不是透传。

**源码事实：** 渲染后逐字段回验，兜住渲染器没预料到的非法形态（裸 `list[str]`、多原语联合、`$ref` 嵌套模型）。校验的真值源直接引用规范类型的 `TypeAdapter`。

**源码事实：** 拒绝与取消都不是错误，工具自己决定语义并正常回答模型；而「客户端不支持诱导」是协议错误，是异常而不是拒绝结果。

**源码事实：** URL 模式的接受不携带数据，且「完成」由独立的 complete 通知表达，而不是靠一次返回。

**源码事实：** 表单模式的 `action` 分支宽松（未知动作归 cancel），URL 模式穷尽（未知动作报错）。

**源码事实：** 两个入口都是服务端向客户端发起的请求，因此需要反向通道；经典握手会话有，`2026-07-28` 连接没有。

**架构推论：** 这套设计里最有价值的一条是**把「用户的决定」与「提问这件事能不能发生」分成两个正交维度**。四种结果形态描述前者（接受带数据 / 接受只是同意 / 拒绝 / 取消），而能力缺失、无反向通道、schema 不合规属于后者。这两类混在一起是常见错误：把「客户端不支持」当成 `decline` 会让工具以为自己被拒绝了，从而给出完全错误的用户可见行为。分开之后，工具作者必须显式回答「如果我无法提问，该怎么办」，这条要求本身就是对的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/elicitation.py` | `AcceptedElicitation` | 表单模式接受，携带已校验的类型化数据 |
| `src/mcp/server/elicitation.py` | `DeclinedElicitation` | 用户拒绝 |
| `src/mcp/server/elicitation.py` | `CancelledElicitation` | 用户关闭提示、未做选择 |
| `src/mcp/server/elicitation.py` | `AcceptedUrlElicitation` | URL 模式接受，只有动作、无内容 |
| `src/mcp/server/elicitation.py` | `ElicitationResult` / `UrlElicitationResult` | 两种模式的联合类型，前者可带类型参数 |
| `src/mcp/server/elicitation.py` | `render_elicitation_schema` | 模型 → 规范合法的 `requested_schema` |
| `src/mcp/server/elicitation.py` | `_ElicitationJsonSchema` | 把 `T \| None` 展平、删掉 `default: null` |
| `src/mcp/server/elicitation.py` | `_validate_rendered_properties` | 逐字段回验，兜住渲染器放过的非法形态 |
| `src/mcp/server/elicitation.py` | `elicit_with_validation` | 表单模式全流程：渲染、发送、空内容拦截、回程校验 |
| `src/mcp/server/elicitation.py` | `elicit_url` | URL 模式：只交换「是否同意跳转」，完成靠独立通知 |
| `src/mcp/server/session.py` | `ServerSession.elicit_form` | 发 `elicitation/create` 的 form 变体，无反向通道时抛 `NoBackChannelError` |
| `src/mcp/server/session.py` | `ServerSession.elicit_url` | 发 `elicitation/create` 的 url 变体 |

## 参考

- [mcp-python-sdk 源码：server/elicitation.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/elicitation.py)
- [mcp-python-sdk 文档：Elicitation](https://github.com/modelcontextprotocol/python-sdk/blob/main/docs/handlers/elicitation.md)
- [MCP 规范：客户端诱导](https://modelcontextprotocol.io/specification/2025-06-18/client/elicitation)
- [Pydantic 文档：自定义 JSON Schema 生成](https://docs.pydantic.dev/latest/concepts/json_schema/#customizing-the-json-schema-generation-process)

## 与主核 / AI 的关系

诱导与主核里的「交互式确认」同类：一个操作执行到一半需要人工决定，系统必须能暂停、问、然后按答案继续或放弃。数据库里对应的是 psql 的交互式提示、或运维脚本的 are-you-sure 闸门。共同点是**必须穷尽「人没回答」的所有形态**：拒绝、忽略、超时、通道不通。

对 agent 工程的直接价值有三条。其一，**「不能问」不等于「被拒绝」**。这个区分决定了工具的降级行为：如果客户端不支持诱导，正确做法是拒绝执行高风险操作，而不是把它当成用户说了不、然后静默跳过。其二，**schema 的表达力差距必须主动翻译**。把 Pydantic 模型直接当表单交给客户端，会在嵌套字段上以一条难以理解的 `$ref` 错误失败；正确做法是明确限制在扁平原语，并在渲染后校验一遍。其三，**「完成」要独立成事件**。带外流程（支付、OAuth）的完成时刻与发起时刻无关，塞进请求返回值的任何设计都会迫使调用方轮询或猜。
