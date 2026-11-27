---
project: mcp-python-sdk
feature: dependency-resolution
title: 请求期依赖解析
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/mcpserver/resolve.py
    symbols: [Resolve, Elicit, Sample, ListRoots]
  - path: src/mcp/server/mcpserver/resolve.py
    symbols: [find_resolved_parameters, returns_input_required]
---

# 请求期依赖解析

## 一句话职责

`Annotated[T, Resolve(fn)]` 让工具参数在**每次调用时**由一个函数现算出来，而不是由客户端传入；如果那个函数需要问客户端（表单、采样、roots），它就返回一个请求标记，框架把它变成一次协议级往返——多轮之间靠 `request_state` 记住已经问过的答案。

## 机制是什么

这套机制解决的是「工具参数来自哪里」的第三种可能。前两种是：客户端传（常规参数）、框架注入（`Context`，见 d03）。第三种是**服务端按需解析**——例如「当前用户是谁」由服务端从 token 推出、「这个仓库的默认分支是什么」要查一次 API。这类值的共同点是：不该由模型猜、也不该由客户端传。

四种标记构成全部语义：

| 标记 | 出现在 | 含义 |
|---|---|---|
| `Resolve(fn)` | 参数注解的元数据 | 这个参数由 `fn` 算出 |
| `Elicit(message, schema)` | 解析器的返回值 | 请客户端填一张表单 |
| `Sample(messages, ...)` | 解析器的返回值 | 请客户端用自己的模型采样 |
| `ListRoots()` | 解析器的返回值 | 请客户端给出 roots |

前三个是「提问」，最后一个是「问根目录」。它们都是**解析器的返回值**而不是异常——解析器正常返回一个标记，框架负责把它变成一次往返。

一次调用只有**一条** `input_required` 通道（`ctx.input_responses` + `ctx.request_state`），所以这套机制与「工具体自己返回 `InputRequiredResult`」互斥，注册期就拒绝两者并用。

## 怎么实现的

**注册期的三件事：找参数、验返回、建计划。**

`find_resolved_parameters(fn)` 扫签名，只看**顶层**的 `Annotated`：`get_origin(annotation) is not Annotated` 时，若注解里某处埋了 `Resolve`（`_contains_resolve` 递归判断）就**抛 `InvalidSignature`**，而不是静默忽略。错误文案给出正确写法。这是刻意的：被包在联合类型里的 `Resolve` 会静默失效，作者会以为注入了、实际没注入。

找到后返回 `{参数名: (Resolve, wants_union)}`，其中 `wants_union` 由 `_wants_union(type_arg)` 判定——如果注解的类型是 `ElicitationResult` 的成员（说明作者想拿完整的 accept/decline/cancel 结果而不是解包后的数据），就置真。`_wants_union` 要处理三种写法：可下标的 `ElicitationResult[T]` 别名、裸 `ElicitationResult`、以及显式写出的 `AcceptedElicitation[T] | ...` 联合。它的实现靠不断取 `__value__`（PEP 695 的 `TypeAliasType` 把目标放在这个属性上）。

`returns_input_required(fn)` 判断返回注解里是否有 `InputRequiredResult` 分支，`_has_input_required_arm` 会穿透 `Annotated`、类型别名（`__value__`）、联合。它捕获 `NameError` 并返回 `False`：`TYPE_CHECKING` 里才导入的别名在运行期取 `__value__` 会炸，那种情况下「这个检查看不到该分支」，而 `Tool.run` 里的运行时兜底还能接住。

`build_resolver_plans` 是注册期最重的一步：**静态分析以工具参数为根的解析器 DAG**。它递归 `analyze(fn, stack)`：

- `stack` 里有当前 key → 抛「Resolver has a cyclic dependency」。这是环路检测，靠递归栈而不是图着色。
- 已在 `plans` 里 → 直接返回（记忆化，同一解析器只分析一次）。
- 否则给一个 `wire_key`：基串是 `_state_key(fn)`（`module:qualname`），若同一个基串已出现过就加 `#1`、`#2` 后缀。注释解释了为什么不能用 `id()`：`request_state` 要在无状态 HTTP 下跨 worker 恢复，所以键必须是**由注册信息确定的**；而闭包或同一方法的多个实例会共享 `module:qualname`，于是用后缀确定性地区分。
- 逐参数分类成三种 `_ParamPlan`：`context`（注解是 `Context` 子类）、`resolve`（有 `Resolve` 标记，同时把被依赖的 `fn` 收进 `nested` 递归分析）、`by_name`（参数名在 `tool_arg_names` 里，即直接取工具的同名参数）。
- 三者都不是 → 抛 `InvalidSignature` 并列出三种合法形态。
- `_check_elicit_return` 检查返回注解里的「提问分支」数量：超过一个就报错，文案是「一个解析器只问一个问题——拆成多个解析器」。

**`tool_arg_names` 必须用别名而不是字段名。** `Tool.from_function` 里算的是 `{field.alias or name for ...}`，注释给了理由：`model_dump_one_level` 按别名输出，解析器「按名取参数」时用的键必须与那一刻真实存在的键一致。

**调用期的执行是一个「先全部遍历、再决定是否返回」的过程。** `resolve_arguments` 先构造一个 `_Resolution` 承载本次调用共享的状态：

- `input_required`：由 `_uses_input_required(context.protocol_version)` 决定，即协议版本 ≥ `2026-07-28` 才走批量往返路径，旧版本仍走同步 `ctx.elicit()`。
- `answers`：`context.input_responses`（仅 `input_required` 时取）。
- `state` / `asked`：从 `context.request_state` 解码出的历史结果与「上一轮问过的题目摘要」。
- `cache`：本次调用内的去重表，键是解析器身份（`_resolver_key`）。
- `persist` / `pending`：本轮要写进下一轮 `request_state` 的记录，与本轮要问的问题。

然后对每个 `Resolve` 参数调 `_resolve(marker.fn, res)`。`_Pending` 异常表示「需要客户端输入」，捕获后 `continue`——**继续处理下一个参数**，这样多个参数需要的提问能被**批量收集**到同一轮。全部走完后，若 `res.pending` 非空，就把 `pending` 与各题的摘要打包成 `InputRequiredResult` 返回；否则返回注入值字典。

`_resolve` 的细节：

- 先查 `cache`，命中直接返回（同一次调用里两个参数依赖同一个解析器，只跑一次）。
- 若 `wire_key` 已在 `pending` 里，抛 `_Pending`——**另一个消费者本轮已经问过这个问题，不重复跑也不再问一次**。
- 按 `_ParamPlan` 逐个填参数：`context` 直接给 `res.context`；`by_name` 从 `tool_args` 取；`resolve` 递归。关键在递归那段：捕获 `_Pending` 时只置 `dep_pending = True` 并 `continue`，**继续访问其余依赖**，注释写明目的是让独立的、需要输入的依赖都被收集进 `pending`。全部走完后若 `dep_pending` 才抛 `_Pending`。
- 调用形式按 `is_async_callable` 分流，同步走 `anyio.to_thread.run_sync`。
- 结果是标记 → `_fulfil`；否则 `_accepted(result)` 直接包成接受态。注释点明：解析器可以返回任意类型（`Elicit[T]` 的 schema 只约束提问，不约束解析器返回值），普通返回值**不写进 `request_state`**，下一轮会重跑。

**`_fulfil` 是两条传输路径的分叉点。**

旧协议（`input_required` 为假）走同步回信道：先 `_require_capability` 检查客户端能力（且只在 `session.can_send_request` 时检查，否则让真正发送时报告失败），`Elicit` 直接 `await context.elicit(...)` 并把 `ValueError` 转成 `ToolError`，其余两类走 `session.send_request(_render_request(marker), _result_type(marker), ...)`。

新协议（`input_required` 为真）做四件事，顺序很讲究：

1. `_render_request(marker)` 渲染成线上的请求形状，`_request_digest(request)` 算出摘要。
2. `_restore_outcome(res, key, marker, q)` **先查历史结果**。这个位置是刻意的：注释说「只有在工具体决定要问之后才查」，所以 `request_state` 里的记录**永远不能替代解析器自己的计算**——它只能满足一次已经决定要问的提问。历史结果优先于客户端重发的答案。
3. 取 `res.answers.get(key)`，并校验摘要：`res.asked.get(key) != q` 时**丢弃这个答案**并打日志「the question changed since it was asked」，然后重新提问。这条防的是重新部署改了问题措辞后，旧答案被错配。
4. 逐类型处理答案：非 `Elicit` 的按 `_result_type` 校验；`Elicit` 的按 action 分支，`accept` 时用 `marker.schema.model_validate(answer.content)` 校验，并把**客户端发来的原始 wire content**（不是模型对象）写进 `persist`——注释说这样下一轮恢复时重新校验的是同样的字节。

**`request_state` 的编码有三处防御。** `_encode_state` 用 `compact_json` 而不是 `model_dump_json`；`_decode_state` 用 stdlib `json.loads` 再 `model_validate`，注释解释了原因：`_encode_state` 可能产出被转义的孤立代理字符，Pydantic 的 JSON 解析器会拒绝。解码失败或版本号不匹配一律当作「没有进度」——注释说明这是把异常当成「操作者自己集群内的漂移（例如滚动升级）」，而不是攻击，因为字符串本身已由边界认证。

`_STATE_VERSION` 是 3，注释记录了 v3 的变更：把已记录与挂起的结果**钉在 ASCII 规范化的题目渲染上**。

**恢复时的一个细节值得单独记。** `_restore_outcome` 把原始解码条目**原样**放进 `res.persist`，注释解释：若后续还有其他解析器待解，下一轮的 `request_state` 由 `res.persist` 构造，所以早先的答案必须留在里面、字节一致地留着，否则会被丢掉并重问。

## 关键取舍

**源码事实：** 一次调用只有一条 `input_required` 通道，所以 `Resolve(...)` 与「工具体返回 `InputRequiredResult`」互斥，注册期抛 `InvalidSignature`，运行期还有 `RuntimeError` 兜底。

**源码事实：** `Resolve` 只在顶层 `Annotated` 生效；埋在联合里会被报错而不是静默忽略。

**源码事实：** 解析器的 wire key 由 `module:qualname` 加上确定性后缀构成，不含 `id()`，因为 `request_state` 要在无状态 HTTP 的任意 worker 上恢复。

**源码事实：** 多轮之间，`request_state` 的记录只用来满足「工具体已经决定要问的」提问，不能替代解析器计算本身；解析器的普通返回值不持久化，下一轮重跑。

**源码事实：** 答案与它对应的题目摘要绑定（`asked[key] != q` 就丢弃重问），防的是重新部署后改题导致错配。

**源码事实：** `_decode_state` 解不开或版本不符时按「没有进度」处理，理由是字符串已由边界认证，失败只可能来自集群内漂移。

**架构推论：** 「静态建计划、运行期只走计划」这个分工是这套机制能收敛的关键。环路、非法参数、错误的多提问返回注解全在注册期暴露；运行期只剩「按计划递归 + 收集待问」。若把参数分类也推迟到运行期，第一次调用才报签名错误，排查成本会高得多。

**架构推论：** `request_state` 的设计实际上是把「一次调用的中间状态」外化成客户端持有的不透明字符串。它换来的是**无状态服务端**——任何 worker 都能接手下一轮，不需要粘性会话或服务端缓存。代价有三条明显的：状态大小受传输限制、状态内容必须能在版本间演进（因此有 `_STATE_VERSION` 与「当作没有进度」的容错）、以及题目摘要这种防错配的额外机制。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/mcpserver/resolve.py` | `Resolve` | 参数注解里的标记：这个参数由哪个函数算 |
| `src/mcp/server/mcpserver/resolve.py` | `Elicit` / `Sample` / `ListRoots` | 解析器可返回的三种提问标记与一种 roots 标记 |
| `src/mcp/server/mcpserver/resolve.py` | `find_resolved_parameters` | 注册期找 `Annotated[_, Resolve]` 参数，埋进联合则报错 |
| `src/mcp/server/mcpserver/resolve.py` | `returns_input_required` | 判断返回注解是否含 `InputRequiredResult` 分支 |
| `src/mcp/server/mcpserver/resolve.py` | `build_resolver_plans` | 静态分析解析器 DAG：环路检测、参数三分类、确定性 wire key |
| `src/mcp/server/mcpserver/resolve.py` | `resolve_arguments` | 调用期入口：遍历全部参数、批量收集待问、返回注入值或 `InputRequiredResult` |
| `src/mcp/server/mcpserver/resolve.py` | `_resolve` | 单个解析器求值，含调用内去重与待问去重 |
| `src/mcp/server/mcpserver/resolve.py` | `_fulfil` | 标记 → 结果，新旧协议的两种传输路径 |
| `src/mcp/server/mcpserver/resolve.py` | `_request_digest` / `_restore_outcome` | 答案与题目摘要绑定，恢复历史结果 |
| `src/mcp/server/mcpserver/resolve.py` | `_decode_state` / `_encode_state` | `request_state` 的编解码与版本容错 |

## 参考

- [mcp-python-sdk 源码：mcpserver/resolve.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/resolve.py)
- [mcp-python-sdk 源码：mcpserver/tools/base.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/tools/base.py)
- [MCP 规范：引导（elicitation）](https://modelcontextprotocol.io/specification/2025-06-18/client/elicitation)
- [MCP 规范：采样（sampling）](https://modelcontextprotocol.io/specification/2025-06-18/client/sampling)

## 与主核 / AI 的关系

「参数由服务端在请求期现算」这种依赖注入，与主核里的**默认表达式**有可比之处：`DEFAULT now()` 也是「这个值不由客户端提供，而由系统在求值时刻算出」。两边都在签名/表定义里声明意图，在运行时解析。差别在失败面——主核的默认表达式只读系统状态，不会向外发起往返；这里的解析器可以问客户端，于是引入了「一次调用可能不在一轮内完成」这个主核里没有的维度。

真正值得对照的是 `request_state` 这套「把中间状态外化给客户端持有」的做法。主核的对应选择正好相反：游标、临时表、会话状态全在服务端，客户端只持有一个句柄。这么选是因为数据库假定服务端有持久状态、且客户端不可信；而 MCP 服务端要能在无状态 HTTP 后面横向扩展，只能把状态推给客户端，并因此必须做三件主核不需要做的事——**给状态加版本号**、**对状态解析失败做降级**（当作没有进度）、**把状态内容与它对应的提问摘要绑定**以防重新部署后错配。

「题目摘要」这一条尤其值得记。它是「状态外化」的必然产物：当状态的语义依赖于另一个会变化的实体（这里是被问的问题），就必须在状态里记下那个实体的指纹，否则版本漂移会让旧状态被误认为有效。主核里同类手法是「计划缓存按 schema 版本失效」。

对 AI 工程的实际意义在于：这套机制给出了「工具参数需要人工介入」的一个完整解法。实践中常见做法是在工具内部直接 `await ctx.elicit(...)`，那在单轮内可行；一旦需要**跨多轮、跨进程、批量提问**，就必须有 `Resolve` 这样的声明式抽象——因为它能让框架把「哪些参数需要问」在运行前收集齐，一次往返问完，而不是让模型触发 N 次工具调用去逐个补参数。
