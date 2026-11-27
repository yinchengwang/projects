---
project: crewai
feature: flow-expressions
title: Flow 表达式与模板渲染
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/flow/expressions.py
    symbols: [Expression, evaluate, render_template]
---

# Flow 表达式与模板渲染

## 一句话职责

`Expression` 给声明式流程提供两种取值能力：把整段文本当一条 CEL 表达式求值（`evaluate`），或在字符串里插值 `${...}`（`render_template`），并在加载期做不带求值的根标识符校验。

## 机制是什么

声明式流程定义里到处需要「从流程数据取值」：动作的 `with` 映射、`crew` 的 `inputs`、`agent` 的 `input`、`each` 的 `in` 与每步的 `if`。取值语言选了 CEL（Common Expression Language），而不是 Python `eval` 或格式化字符串。

两种用法有明确的类型规则：

- **整段一条表达式**：结果保留原类型。写 `domains: "${state.domains}"` 得到的是 list，不是字符串。
- **文本混合插值**：结果是字符串。非文本值按 JSON 序列化嵌入，`null` 变成空串。
- 这条规则的判据是「解析后的片段里只有一个表达式片段，且所有字面片段都是空白」。

此外 `now()` 是一个注册进 CEL 环境的函数，返回当前 UTC 时间戳，且**在整个运行内冻结**——同一个 kickoff 里多次求值拿到同一时刻。

## 怎么实现的

**模板切分靠词法器而非字符串查找。** `_parse_template_segments` 找 `${` 后调 `_marker_end`：它把 celpy 的 CEL 词法器拿来，从 `${` 之后开始逐 token 走，遇到 `{` 深度加一、`}` 深度减一，深度归零处的 token 起始位置就是闭合大括号。用词法器而不是 `find("}")` 的原因是表达式里可能含大括号（映射字面量、嵌套调用），简单查找会提前截断。词法失败或被耗尽时抛 `ExpressionError`，报「未闭合或非法」。解析结果被 `lru_cache(maxsize=256)` 缓存——同一模板字符串在循环流程里只解析一次。

**两种求值路径共用一套上下文。** `from_flow` 是构造标准上下文的入口：`state` 取流程状态的序列化副本，`outputs` 取方法输出按名索引的映射（`_outputs.outputs_by_name`，若某方法带人工反馈结果，优先用反馈对象而不是原始输出），额外的 `local_context` 里除 `state` / `outputs` 之外的键经 `to_serializable(max_depth=0)` 后并入——`each` 动作就是靠这个把当前 `item` 传进去的。`now` 取流程实例上的 `_cel_now`（kickoff 时设置，恢复时重设）。

**求值过程是「编译 → 绑定函数 → 求值 → JSON 往返」。** `_evaluate_cel` 建 CEL 环境（带 `now` 的函数注解）、用环境编译表达式、把 Python 上下文经 `json_to_cel` 转成 CEL 值后求值，然后检查返回值里是否藏着 `CELEvalError`——CEL 支持错误值传播，错误可能被包在列表或字典里返回而不抛出，所以 `_find_cel_eval_error` 递归扫描容器，找到就抛。最后一步 `json.loads(json.dumps(result, cls=CELJSONEncoder))` 把 CEL 类型（时间戳、时长等）规范成纯 JSON 值。任何异常都被包成 `ExpressionError`，附上原始表达式文本。

**校验不求值。** `validate_expression` 只做三件事：把表达式编译成语法树、收集根标识符、与允许集合比对。收集逻辑（`_collect_root_identifiers`）遍历 celpy 的解析树，识别几种节点：`ident` 是根标识符；`ident_arg` 是调用，只看参数部分；`member_dot_arg` 是「接收者.宏(...)」形态，要先算接收者的根，再把宏的局部变量加入局部根集合后处理参数。宏局部变量的判定（`_receiver_macro_local_roots`）只对 `all` / `exists` / `exists_one` / `filter` / `map` 五个接收者宏生效——`items.all(x, ...)` 里的 `x` 不算根。允许集合之外出现任何根就报错，错误文案会同时列出允许集合与非法集合，并给出示例写法。

`validate_template` 是对复合值的递归：字符串走模板切分并对每个表达式片段调 `validate_expression`（多个片段时错误位置带序号），字典按键递归、列表按下标递归，错误路径会拼成 `with.key` / `with[0]` 这种可定位的形式。

**类型保持的判定写在一处。** `_render_template_string` 先切分、取出全部表达式片段；没有表达式就原样返回；只有一个表达式片段且所有字面片段 `strip()` 后为空时，直接返回该表达式的求值结果（保留类型）；否则拼接——字面片段原样、表达式结果经 `_stringify_cel_value`（字符串原样，其他类型 JSON 序列化并 `ensure_ascii=False`），`None` 渲染成空串。

**给 LLM 看的规则文本与代码同源。** 模块里有一组 `FLOW_TEMPLATE_EXPRESSION_RULES` 常量，逐条写清「用 `${...}` 读数据」「`state` 是输入、`outputs.<step>` 是已完成方法结果」「不要在映射串里用 CEL 的 `+` 拼字符串」「单个表达式保留类型」「混合文本时非文本值变 JSON」「`now()` 是本次运行冻结的 UTC 时间」，并拼成 `FLOW_TEMPLATE_EXPRESSION_CONTRACT` 供字段描述复用（`flow_template_expression_description`）。YAML / JSON 两种格式还各带一组示例。

## 关键取舍

**源码事实：** 选 CEL 而不是 Python 求值，直接后果是声明式定义不需要能执行任意代码就能取值。`script` 动作是唯一的例外，且它单独用环境变量开关（`CREWAI_ALLOW_FLOW_SCRIPT_EXECUTION`）把守——两条路径的安全等级被显式分开。

**源码事实：** `now()` 在一次运行内冻结，但恢复（resume）时会重新取一次。源码注释解释了这个不对称：流程可能在等人工反馈上挂好几天，恢复后的表达式必须看到「今天」。

**源码事实：** 类型保持只认「单个表达式且无有效字面」这一种情形。`"${state.n} items"` 一定得到字符串，哪怕 `state.n` 是数字。这条规则简单、可预测，但意味着想让数字保持数字就不能加单位或前后缀。

**源码事实：** 根校验只到根一级。`state.foo` 里的 `foo` 是否真是状态字段，静态不可知——状态可以是 `dict`，字段集运行期才能确定。所以校验的目标被明确限定在「别把变量名写错成另一个根」，而不是「字段是否存在」。

**源码事实：** 错误值是 CEL 的一种正常返回值形态，不会自动抛异常。因此必须显式扫描容器，否则「表达式出错了但流程继续用错误值往下跑」会成为静默故障。

**架构推论：** 把「校验」与「求值」拆成两个方法而不是一个带 `dry_run` 标志的方法，使加载期校验不必构造运行上下文。这决定了定义可以在没有状态实例的情况下被校验——`FlowDefinition` 的三项模型校验正是这么用的。

**架构推论：** 宏局部变量的处理只覆盖五个接收者宏。这是一个已知的、被写的白名单，而非通用作用域分析。新增宏语法时需要同步这个集合，否则会把宏的绑定变量误报成未知根。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/flow/expressions.py` | `Expression` | 表达式/模板的统一入口，含上下文构造与四种校验/求值方法 |
| `lib/crewai/src/crewai/flow/expressions.py` | `Expression.evaluate` | 整段当作一条 CEL 表达式求值 |
| `lib/crewai/src/crewai/flow/expressions.py` | `Expression.render_template` | 对嵌套值做 `${...}` 插值渲染，单表达式保留类型 |
| `lib/crewai/src/crewai/flow/expressions.py` | `_parse_template_segments` | 借助 CEL 词法器切分模板，结果带缓存 |
| `lib/crewai/src/crewai/flow/expressions.py` | `validate_expression` / `validate_template` | 编译但不求值的根标识符白名单校验 |
| `lib/crewai/src/crewai/flow/expressions.py` | `_collect_root_identifiers` | 解析树遍历，排除接收者宏的局部绑定 |
| `lib/crewai/src/crewai/flow/expressions.py` | `_evaluate_cel` | 编译、绑函数、求值、错误值扫描、CEL→JSON 归一 |

## 参考

- [CrewAI 源码：expressions.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/flow/expressions.py)
- [CEL 语言规范](https://github.com/google/cel-spec/blob/master/doc/langdef.md)
- [celpy 项目主页](https://github.com/cloud-custodian/cel-python)

## 与主核 / AI 的关系

这套「加载期做根校验、运行期才真求值、静态检查保守」的分工，与主核里 SQL 的处理是同一条线：解析期能确定的东西（语法、可引用的范围）在解析期查死，不能确定的（值、动态结构）留到执行期，执行期出错要能定位到具体表达式。

但两边的失败策略不同。主核遇到非法引用直接拒绝执行计划；这里 `ExpressionError` 在运行期抛出后会沿方法执行路径上抛，最终变成 `MethodExecutionFailedEvent` 并让这次 kickoff 失败——也就是「一个取值表达式写错，整条流程终止」。这个取舍是合理的：表达式算不出值，后续方法没有输入，继续跑只会产生更离谱的结果。

值得对照的是 `each` 动作的上下文注入方式。它给子步骤额外开了一组根（`item`），用局部上下文而非修改 `state` 来传递当前项——这与主核里相关子查询的外部列引用属于同一类问题：内层作用域需要看见外层的绑定。两边的解法也相似：不是把值塞进全局状态，而是把「作用域」显式建模并传给求值器。

对 AI 工程的实际意义在于：让模型只写「取值表达式」而不写代码，是提升声明式编排可验证性的关键一步。代价是表达式语言必须有明确的类型规则与作用域规则，否则模型写出的东西在加载期过、运行期炸。这套机制的做法是把规则同时写进代码和给模型看的契约文本（`FLOW_TEMPLATE_EXPRESSION_RULES`），让「能做什么」在提示词层面和实现层面一致。
