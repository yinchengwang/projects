---
project: haystack
feature: conditional-router
title: 条件路由
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/routers/conditional_router.py
    symbols: [ConditionalRouter, Route, RouteConditionException]
  - path: haystack/components/routers/conditional_router.py
    symbols: [NoRouteSelectedException]
---

# 条件路由

## 一句话职责

把一组「**Jinja 表达式 + 输出模板 + 输出名 + 输出类型**」的四元组当作路由表，按顺序求值每个 `condition`，**第一次为真的那条胜出**，求值它的 `output` 模板并只往那一个命名 socket 写值。

## 机制是什么

一条路由是四个字段的组合，而它们分别回答四个不同的问题：

| 字段 | 回答 | 形态 |
|---|---|---|
| `condition` | **走不走这条** | Jinja 表达式，渲染后 `literal_eval` 成布尔 |
| `output` | **带什么过去** | Jinja 表达式（`output_passthrough=True` 时是裸变量名） |
| `output_name` | **从哪个 socket 出去** | 字符串（成为组件的输出 socket 名） |
| `output_type` | **出去的是什么类型** | 类型对象，可选地在运行期被校验 |
| `output_passthrough` | 可选，绕过 Jinja | 布尔 |

**三个字段都支持写成列表** —— 一条路由可以一次写多个输出（长度必须三者一致）。于是一个「多路条件分支」在管道图上表现为「一个组件、N 个输出 socket」，而不是 N 个组件。

**三类失败必须分清，而这是理解这个组件的关键：**

| 情形 | 结果 |
|---|---|
| 某条 `condition` 为假 | **正常**：`continue` 去看下一条 |
| `condition` 表达式本身求值出错 | `RouteConditionException` |
| 所有 `condition` 都为假 | `NoRouteSelectedException` |

**第一行最容易被误读。** 「条件为假」不是错误——它是路由的**正常工作方式**：一张路由表里绝大多数条件本来就是假的，真的那个才胜出。异常只在「没法判断真假」与「没有任何一条为真」这两种情况下抛出。

而这两者的性质完全不同：**`RouteConditionException` 是输入或模板有问题**（变量缺失、类型不可比较、语法错），**`NoRouteSelectedException` 是路由表覆盖不全**（少写了一个兜底分支）。前者是 bug，后者是设计缺口——但它们都必须抛，因为沉默地「什么都不输出」会让下游永远等不到输入。

## 怎么实现的

### 一、构造期：从模板里反推 socket 类型

**`ConditionalRouter` 不需要调用方声明输入有哪些变量——它从模板里抽。**

```
input_types: set[str] = set()  # let's just store the name, type will always be Any
for route in routes:
    outputs = route["output"] if isinstance(route["output"], list) else [route["output"]]
    if output_passthrough:
        route_input_names = self._extract_variables(self._env, [route["condition"]])
        route_input_names.update(outputs)
    else:
        route_input_names = self._extract_variables(self._env, [route["condition"]] + outputs)
    input_types.update(route_input_names)
```

**`condition` 与 `output` 都是模板，两者的变量都是输入。** 而 passthrough 分支单独处理——因为它的 `output` 是**变量名本身**而不是模板，所以要直接 `update` 进输入集合（否则 `{{ foo }}` 与 `foo` 的抽取结果不同）。

**变量抽取用的是 Jinja 的 AST 而不是正则。**

```
jinja2_ast = env.parse(template)
template_variables = meta.find_undeclared_variables(jinja2_ast)
assigned_variables = _collect_assigned_variables(jinja2_ast)
return assigned_variables, template_variables
```

于是 `_extract_variables` 里做的是集合差：

```
variables.update(template_variables - assigned_variables)
```

**`assigned_variables` 是模板内 `{% set %}` 赋的值**——它们不是外部输入。若不减掉，一个用了 `{% set x = 1 %}` 的模板会让组件多出一个叫 `x` 的输入 socket，而调用方根本不知道该传什么。

**而 `find_undeclared_variables` 只返回真正的自由变量** —— 它已经排除了 `{% for %}` 的循环变量等局部名。于是这套抽取在「从模板反推接口」这件事上是正确的，而不是凑出来的。

**可选变量从必填集合里被减掉。**

```
mandatory_input_types = input_types - set(self.optional_variables)
component.set_input_types(self, **dict.fromkeys(mandatory_input_types, Any))
for optional_var_name in self.optional_variables:
    component.set_input_type(self, name=optional_var_name, type=Any, default=None)
```

**两步：先设必填（无默认值），再逐个设可选（`default=None`）。** 而 `default=None` 正是「缺值时可路由」的机制来源——`a03`（组件就绪判定）那页讲的「必填 socket 必须有值」在这里被绕过了：**可选变量在最底层就是一个有默认值的 socket**，于是上游不给值它也有值（`None`）。

**而「声明了但没用」的可选变量会被警告。**

```
unused_optional_vars = set(self.optional_variables) - input_types
if unused_optional_vars:
    logger.warning("The following optional variables are specified but not used in any route: {unused_optional_vars}. Check if there's a typo in variable names.", ...)
```

**只警告不报错** —— 因为「多声明一个可选变量」是无害的（只是多一个永远为 `None` 的输入）。但消息里那句「Check if there's a typo」把实际最常见的原因点出来了。

**所有输入 socket 的类型都是 `Any`**，注释写明了原因：模板里的变量没有类型信息可推。**这是一个诚实的让步** —— 从 `{{ streams|length > 2 }}` 能推出 `streams` 是可 `len()` 的东西，但推不出是 `list[int]`。而输出的类型是显式声明的，所以输出 socket 有真实类型：

```
component.set_output_types(self, **output_types)
```

**于是这个组件在管道图上的形状是「输入全 `Any`、输出有类型」** —— 与手写组件恰好相反（后者通常是输入有类型、输出靠装饰器声明）。

### 二、`_validate_routes`：四类静态检查

**第一类：必填字段。**

```
mandatory_fields = {"condition", "output", "output_type", "output_name"}
if not mandatory_fields.issubset(keys):
    raise ValueError(f"Route must contain 'condition', 'output', 'output_type' and 'output_name' fields: {route}")
```

`output_passthrough` 不在其中——它是 `NotRequired`。

**第二类：三个列表等长。**

```
if not len(outputs) == len(output_types) == len(output_names):
    raise ValueError(f"Route output, output_type and output_name must have same length: {route}")
```

**这是「一条路由多输出」的唯一约束。** 三者靠 `zip(..., strict=True)` 配对，长度不符就必须在构造期拦住，否则运行期的 `strict=True` 会抛一个不带路由信息的 `ValueError`。

**第三类：`condition` 必须是合法模板，而错误消息分两支。**

```
if not self._validate_template(self._env, route["condition"]):
    condition_value = route["condition"]
    if not isinstance(condition_value, str):
        raise ValueError(f"Invalid template for condition: {condition_value!r} (type: {type(condition_value).__name__}). Condition must be a string representing a valid Jinja2 template. For example, use {str(condition_value)!r} instead of {condition_value!r}.")
    raise ValueError(f"Invalid template for condition: {condition_value}")
```

**「不是字符串」这一支的消息写得非常好**：它不但说了类型，还**直接给出修法**——`use '{{ x > 1 }}' instead of 5`。这是一个把 `5` 当成条件传进去的常见错误（用户以为能直接传布尔或数值）。

而 `_validate_template` 自己先判 `isinstance(template_text, str)` 再 `env.parse`：

```
if not isinstance(template_text, str): return False
try: env.parse(template_text); return True
except TemplateSyntaxError: return False
```

**先判类型再 `parse`** —— 因为 `env.parse` 对非字符串的行为不保证是抛 `TemplateSyntaxError`。

**第四类：`output` 只在非 passthrough 时被当作模板校验。**

```
output_passthrough = route.get("output_passthrough", False)
if not output_passthrough:
    for output in outputs:
        if not self._validate_template(self._env, output): ...
```

**passthrough 时 `output` 是变量名**，一个叫 `my-data` 的变量名不是合法模板，校验它会把正确用法判成错。

### 三、`run`：两个 try 与一个关键的 `isinstance(e, ValueError)` 分支

**主循环按顺序求值，第一个为真的胜出。**

```
for route in self.routes:
    try:
        t = self._env.from_string(route["condition"])
        rendered = t.render(**kwargs)
        if not self._unsafe:
            rendered = ast.literal_eval(rendered)
    except Exception as e:
        msg = f"Error evaluating condition for route '{route}': {e}"
        raise RouteConditionException(msg) from e
    if not rendered:
        continue
    ...
```

**三个要点：**

1. **`ast.literal_eval(rendered)`** —— 模板渲染出来的永远是**字符串**。而 `"{{ streams|length > 2 }}"` 渲染成 `"True"` 或 `"False"`，需要转成真正的布尔。`literal_eval` 只接受字面量，**不执行代码**——这是它相对 `eval` 的安全边界。
2. **整个求值块单独一个 `try`**，注释写明：「Separate try so all condition errors, including ValueError, raise RouteConditionException.」也就是说**`literal_eval` 的 `ValueError` 也被包进来** —— 一个渲染成 `"abc"` 的条件表达式会变成 `RouteConditionException` 而不是裸 `ValueError`。
3. **`if not rendered: continue`** —— 这就是「条件为假是正常流程」的实现。注意它用的是**真值测试**而不是 `is True`，于是 `0`、`""`、`[]`、`None` 都算假。

**输出求值块里有 passthrough 的直取分支。**

```
if output_passthrough:
    if output not in kwargs:
        raise ValueError(f"Variable '{output}' not found in inputs for passthrough route '{output_name}'. Ensure '{output}' is passed as an input to the router.")
    output_value = kwargs[output]
else:
    t_output = self._env.from_string(output)
    output_value = t_output.render(**kwargs)
    with contextlib.suppress(Exception):
        if not self._unsafe and output_type is not str:
            output_value = ast.literal_eval(output_value)
```

**passthrough 的价值在这一行体现：`output_value = kwargs[output]` 是原对象引用。** 没有渲染、没有字符串化、没有 `literal_eval` —— 于是一个 dataclass、Pydantic 模型、`Document` 都能原样穿过路由，而不会被 Jinja 的字符串化毁掉。

**这也解释了它的名字为什么叫 `output_passthrough`（直通）而不是「变量模式」。**

**而非 passthrough 分支里那个 `output_type is not str` 的条件有一段长注释，值得完整读。**

> 我们在「输出已经是字符串」的情况下抑制异常，否则我们会尝试求值并失败。这是必需的，因为输出可能是不同的字面量结构。这不支持任何用户类型。**当声明的 `output_type` 是 `str` 时我们跳过字面量求值**，这样渲染出的字符串若恰好是合法的 Python 字面量（比如 `"1,000"` → `(1, 0)`、`"42"` → `42`、`"None"` → `None`）就会原样返回而不是被强制成另一个类型，那会违反声明的 `output_type`。

**这三个例子精确说明了不加这个条件的后果：**

| 渲染结果 | `literal_eval` 后 | 后果 |
|---|---|---|
| `"1,000"` | `(1, 0)` | 字符串变成一个元组 |
| `"42"` | `42` | 字符串变成整数 |
| `"None"` | `None` | 字符串变成 `None` |

**一个声明 `output_type=str` 的路由，若把用户输入 `"42"` 交出去，下游会收到整数 `42`。** 而这个错误只在「内容恰好长得像字面量」时发生——最典型的就是用户输入。

**所以这个条件不是优化，是正确性修复。** 而它的写法是「按声明的输出类型决定要不要做类型转换」——一个由**契约驱动**的判断。

### 四、真正的错误来源：`isinstance(e, ValueError)` 这一支

**输出求值块的 `except` 里有一个不寻常的分支。**

```
except Exception as e:
    # If this was a type-validation failure or missing passthrough variable, let it propagate
    if isinstance(e, ValueError):
        raise
    msg = f"Error evaluating condition for route '{route}': {e}"
    raise RouteConditionException(msg) from e
```

**它按异常类型分流：`ValueError` 直接向上抛（保留原消息），其它异常被包成 `RouteConditionException`。**

需要直抛的是两处自己 `raise` 的 `ValueError`：

- passthrough 变量缺失：`f"Variable '{output}' not found in inputs for passthrough route '{output_name}'."`
- 类型校验失败：`f"Route '{output_name}' type doesn't match expected type"`

**这两条消息都是精确的、带上下文的**（哪个路由名、哪个变量）。若被包进 `RouteConditionException`，它们会被拼进一个说「condition」的错误消息里——**信息会被误标到错误的阶段**。

而其它异常（比如 Jinja 渲染输出模板时的 `UndefinedError`）被包成 `RouteConditionException`。

**这里有一个值得记的不准确**：那个包装消息写的是 `"Error evaluating condition for route '{route}'"`，但它出现在**输出模板求值**的 `except` 里。所以当一个输出模板出错时，用户看到的错误说「求值 condition 时出错」——**指向了错误的字段**。而 `{route}` 是整个路由字典，通常很长。

这是一个真实的诊断缺陷：**消息模板被从上面的 `try` 复制下来，但语义已经变了**。正确的措辞应当是「Error evaluating output for route ...」。它对功能没有影响（异常类型与传播路径都对），但对排查有实实在在的影响。

### 五、类型校验：`_output_matches_type` 的递归

**`validate_output_type=True` 时才启用，而它的实现是按泛型参数递归。**

```
if expected_type is Any: return True
origin = get_origin(expected_type); args = get_args(expected_type)
if origin is None: return isinstance(value, expected_type)
```

**`origin is None` 就是「裸类型」**（`int`、`str`、`list`）——直接 `isinstance`。而带参数的泛型（`list[int]`、`dict[str, int]`、`int | str`）走后面的分支。

**序列分支有一个针对「字符串也是序列」的专门排除。**

```
if isinstance(origin, type) and issubclass(origin, Sequence):
    if isinstance(value, (str, bytes)): return False
    if not isinstance(value, Sequence): return False
    if not value: return True
    return all(self._output_matches_type(item, args[0]) for item in value)
```

**`str` 与 `bytes` 是 `Sequence`，但它们声明的 `output_type` 不是序列类型** —— 于是 `list[int]` 收到 `"abc"` 必须判假。这个检查是必需的，否则字符串会被逐字符校验，而字符是字符串，`list[str]` 会误通过。

**而「空序列/空映射直接为真」是一条刻意的放宽**：`args[0]` 对空集合无从检验，此时「没有元素违反类型」是对的。同样的处理在 Mapping 分支里重复：

```
if isinstance(origin, type) and issubclass(origin, Mapping):
    if not isinstance(value, Mapping): return False
    if not value: return True
    key_type, value_type = args
    return all(self._output_matches_type(k, key_type) and self._output_matches_type(v, value_type) for k, v in value.items())
```

**`key_type, value_type = args` 这是一个未加校验的解包** —— 一个裸 `Mapping`（无参数）会让 `get_args` 返回空元组，于是这里抛 `ValueError: not enough values to unpack`。而那个异常不在 `except Exception` 之外——它会被 `isinstance(e, ValueError)` 判为真并**直接向上抛**，于是用户看到一个「不是足够的值用于解包」的错误。这是这类泛型递归里常见的边界泄漏。

**Union 分支是递归的或。**

```
if _is_union_type(origin):
    return any(self._output_matches_type(value, arg) for arg in args)
return False
```

**最后的 `return False` 是「无法识别的泛型一律判假」** —— 于是 `Literal[...]`、`Callable[...]` 之类会校验失败。这是一个保守的选择：宁可在启用校验时拒绝，也不假装通过。

**这个函数带了 `# noqa: PLR0911`（分支过多）** —— 六个 `return`，是它复杂度的度量。

### 六、`unsafe` 与沙箱：两个环境

**构造时按 `unsafe` 选 Jinja 环境。**

```
if self._unsafe:
    logger.warning("Unsafe mode is enabled. This allows execution of arbitrary code in the Jinja template. Use this only if you trust the source of the template.")
self._env = NativeEnvironment() if self._unsafe else HaystackSandboxedEnvironment()
self._env.filters.update(self.custom_filters)
```

**两个差别：**

1. **`HaystackSandboxedEnvironment` 限制可访问的属性与调用**（Jinja 的 sandbox 实现），默认防住「通过模板访问 `__class__` 之类」的路径。
2. **`NativeEnvironment` 保留原生类型** —— 它是 Jinja 的一个环境，渲染结果不强制成字符串。

第 2 点解释了一个细节：`ast.literal_eval` 只在不 unsafe 时被使用。因为：

```
if not self._unsafe:
    rendered = ast.literal_eval(rendered)
```

**`NativeEnvironment` 渲染出的已经是原生对象**（`True` 而不是 `"True"`），再 `literal_eval` 一个布尔会抛 `ValueError`。所以两个 `not self._unsafe` 条件不是「安全开关」，而是**两个环境的行为差异适配**。

**这是一个容易误读的地方**：`unsafe=True` 同时开启了「不沙箱」与「不做 `literal_eval`」两件事，而后者与安全无关。

**而 `custom_filters` 是往环境里注入自定义过滤器** —— 于是条件表达式可以写成 `{{ text|my_norm == 'foo' }}`。这是一个「让用户扩展条件语言」的钩子，且它注入的是 `self._env`（实例级），不影响全局。

**`NativeEnvironment` 还带来一个隐含的行为：`output` 模板可以直接产出非字符串对象。** 于是一个 `{{ my_list }}` 在 unsafe 模式下会得到真正的列表，而 `output_type=list[int]` 是它的自然声明。

## 关键取舍

**源码事实：** 一条路由是 `(condition, output, output_name, output_type)` 四元组，后三者可写成等长列表以支持一条路由多输出；`output_passthrough` 是唯一的可选字段。

**源码事实：** 三类情形严格区分——条件为假走 `continue`（正常流程）、条件表达式求值出错抛 `RouteConditionException`、全部为假抛 `NoRouteSelectedException`。**「条件为假」不抛异常。**

**源码事实：** 输入的 socket 集合由**模板反推**：用 Jinja AST 的 `find_undeclared_variables` 减去 `{% set %}` 赋值的变量；`output_passthrough` 的 `output` 是裸变量名，直接计入输入。

**源码事实：** 所有输入 socket 类型都是 `Any`（模板推不出类型），而输出 socket 用声明的 `output_type`——组件在管道图上呈现「输入无类型、输出有类型」的少见形态。

**源码事实：** 可选变量通过**给 socket 一个 `default=None`** 实现「缺值也能跑」，而不是在 `run` 里判 `kwargs.get`；声明了却没用到的可选变量会警告并提示「检查是否有拼写错误」。

**源码事实：** 构造期四类静态校验：必填字段齐全、三个列表等长、`condition` 必须是合法模板、`output` 仅在非 passthrough 时校验。`condition` 不是字符串时，错误消息直接给出修法（`use '{{ ... }}' instead of 5`）。

**源码事实：** `condition` 求值后必须 `ast.literal_eval`（渲染结果永远是字符串），且该步的 `ValueError` 也被包成 `RouteConditionException`——注释写明这是刻意的。

**源码事实：** `output` 的字面量求值**按声明的类型决定要不要做**：`output_type is str` 时跳过。理由是 `"1,000"` 会变成 `(1, 0)`、`"42"` 变成 `42`、`"None"` 变成 `None`——那会违反声明的 `output_type`，而用户输入恰好长得像字面量是最常见的情形。

**源码事实：** `output_passthrough` 直接 `kwargs[output]` 取原对象，不经 Jinja——这是复杂类型（dataclass、Pydantic 模型）能穿过路由的唯一方式。

**源码事实：** 输出求值块的 `except` 按异常类型分流：`ValueError` 直接上抛（保住「变量缺失」与「类型不匹配」两条精确消息），其它包成 `RouteConditionException`。

**源码事实（缺陷）：** 该 `except` 里的包装消息写的是 `"Error evaluating condition for route ..."`，但它位于**输出模板**求值的异常路径上——消息把出错的字段误标成 `condition`，且 `{route}` 是整个字典（通常很长）。

**源码事实：** `_output_matches_type` 按泛型参数递归：裸类型用 `isinstance`、序列排除 `str`/`bytes`、空序列与空映射直接为真、`Union` 递归取或、无法识别的泛型一律判假。其中 `key_type, value_type = args` 对无参数 `Mapping` 会抛解包错误，且因为它是 `ValueError` 会被直接上抛。

**源码事实：** `unsafe=True` 同时开启两件事：换用 `NativeEnvironment`（不沙箱）与跳过 `literal_eval`——后者与安全无关，而是因为 `NativeEnvironment` 渲染出的已是原生对象，再 `literal_eval` 会抛错。

**架构推论：** 这个组件把「路由」表达成了**数据而不是代码**——一张路由表就是一个可以序列化、可以热替换、可以跨配置复用的字面量结构。代价是它必须自己实现「从模板反推接口」与「把字符串渲染收敛回类型」这两件事，而这两件事恰好是这套实现里最容易出细节缺陷的地方（`literal_eval` 的三处、泛型递归的边界）。

**架构推论：** 「条件为假是正常流程」这个设计与 `RouteConditionException` 的存在共同定义了一种**三态判定**：真 / 假 / 判不出来。把后两者混为一谈（比如条件求值异常时直接 `continue`）会让「变量名拼错」表现为「路由静默不命中」——那是最难排查的一类问题。**能区分「假」与「错」的判定器，才配得上做控制流。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/routers/conditional_router.py` | `ConditionalRouter` | 按顺序求值路由表，首个为真者胜出 |
| `haystack/components/routers/conditional_router.py` | `Route` | 四元组 TypedDict，`output_passthrough` 可选 |
| `haystack/components/routers/conditional_router.py` | `NoRouteSelectedException` | 所有条件都为假（无兜底）时抛 |
| `haystack/components/routers/conditional_router.py` | `RouteConditionException` | 条件表达式求值/解析出错时抛 |
| `haystack/components/routers/conditional_router.py` | `_validate_routes` | 必填字段、三列表等长、模板合法性 |
| `haystack/components/routers/conditional_router.py` | `_extract_variables` | 模板变量反推输入，减去 `{% set %}` 赋值 |
| `haystack/components/routers/conditional_router.py` | `run` 的条件块 | 独立 `try` + `literal_eval` + `continue` |
| `haystack/components/routers/conditional_router.py` | `run` 的输出块 | passthrough 直取 / 模板渲染 + 按 `output_type` 决定求值 |
| `haystack/components/routers/conditional_router.py` | `run` 的 `except` | `ValueError` 上抛、其余包成条件异常（消息字段误标） |
| `haystack/components/routers/conditional_router.py` | `_output_matches_type` | 泛型递归校验，序列排除 `str`/`bytes` |
| `haystack/utils/jinja2_extensions.py` | `_extract_template_variables_and_assignments` | AST 级变量抽取，区分自由变量与 `{% set %}` |

## 参考

- [haystack 源码：routers/conditional_router.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/routers/conditional_router.py)
- [haystack 文档：条件路由](https://docs.haystack.deepset.ai/docs/conditionalrouter)
- [Jinja2 文档：沙箱与 NativeEnvironment](https://jinja.palletsprojects.com/en/stable/sandbox/)
- [haystack 文档：路由](https://docs.haystack.deepset.ai/docs/routers)

## 与主核 / AI 的关系

条件路由与主核里的**谓词下推**是同一件事的两种尺度：两者都把「判断条件」表达成可传递、可检查的结构（这里是 Jinja 表达式，那里是 SQL 谓词），然后交给一个执行器去求值并决定走哪条分支。

**而三态判定在主核里有精确对应：SQL 的三值逻辑。** `WHERE` 里的谓词可能为 `TRUE`、`FALSE` 或 `NULL`——第三态不是错误，而是「无法判定」，且它在 `AND`/`OR` 里的传播规则被严格定义。条件路由的「真 / 假 / 判不出来」是同一种区分，差别是 SQL 把第三态做进了类型系统与求值规则，而这里用一个专门的异常来表达它。**两者都拒绝把「判不出来」压成「假」，因为那会让错误变成静默的行为差异。**

**「路由表是数据」这一点在主核里对应执行计划的可存储性**：数据库的执行计划能被序列化、能被缓存、能被 `EXPLAIN` 出来。路由表能被 `to_dict` / `from_dict` 往返，因此同样「可存、可查、可替换」——这是把控制流当数据对待共同换来的好处。

对 agent 工程的直接价值有三条。其一，**「条件为假」与「条件出错」必须分开**。混在一起，一个变量名拼错就表现为「这条路由永远不命中」，而管道不报错、下游永远等不到输入，排查成本极高。其二，**`output_type` 声明的字符串必须真的返回字符串**。`"42"` 被转成整数 `42` 这类错误只在「用户输入恰好长得像字面量」时出现，而它破坏的是下游对类型的信任。其三，**复杂对象要有一条不经模板的通道**。`output_passthrough` 那行 `kwargs[output]` 是 dataclass、Pydantic 模型穿过路由的唯一安全路径——任何经过 Jinja 渲染的路径都会把它们字符串化。
