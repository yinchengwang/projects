---
project: smolagents
feature: tool-code-validation
title: 工具源码的静态校验
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/tool_validation.py
    symbols: [MethodChecker, validate_tool_attributes]
  - path: src/smolagents/tools.py
    symbols: [validate_after_init]
---

# 工具源码的静态校验

## 一句话职责

在工具源码被序列化、发到另一个进程、在那里 `exec` 之前，用两个 AST 访问器先证明它是**自包含的**：类体只放字面量、`__init__` 全部有默认值、方法里的每个名字都能追溯到参数/类属性/导入/内置——这套检查是「源码即工具」这条路线唯一的静态防线。

## 机制是什么

smolagents 的工具要被写成源码送到远程执行器（见 c02）。一旦送出，那个进程只有源码，没有用户的模块、没有闭包、没有局部环境。所以「这段代码在别处能不能独立跑起来」必须在发出前回答。

`tool_validation.py` 用两个 `ast.NodeVisitor` 子类来回答，作用域不同：

| 访问器 | 作用域 | 查什么 |
|---|---|---|
| `ClassLevelChecker`（藏在 `validate_tool_attributes` 内部） | 整个类体 | 类属性是否只含字面量、`name` 是否常量字符串且合法、`__init__` 参数是否都有字面量默认值 |
| `MethodChecker` | 单个方法 | 方法体内的每个名字（读与调用）是否可追溯；`self.xxx` 属性访问是否越界 |

它们都在**纯静态**层面工作：读源码字符串（`get_source(cls)`），不 import、不执行、不看运行时值。

## 怎么实现的

**`validate_tool_attributes` 的三段结构。**

第一段：拿源码并确认第一个语句是 `ClassDef`（否则 `ValueError("Source code must define a class")`），交给 `ClassLevelChecker` 遍历。

`ClassLevelChecker` 有两个关键开关：`in_method` 与几个名字集合。

- `visit_FunctionDef` 里，若函数名是 `__init__` 就先跑 `_check_init_function_parameters`；然后置 `in_method = True` 再 `generic_visit`，出来后恢复原值。这样 `visit_Assign` 能区分「类体里的赋值」与「方法体里的赋值」。
- `visit_Assign` 只在**不在方法内**时工作：把目标名收进 `class_attributes`；再用 `ast.walk(node.value)` 检查右值——**只要出现任何不是 `Constant` / `Dict` / `List` / `Set` 的节点**，就把这个属性记进 `complex_attributes`。注意这个判据是「子树上出现了非字面量节点」，所以 `x = foo()`、`x = [bar()]`、`x = a.b` 都算复杂。
- 对 `name` 属性有额外的三重检查：必须是 `ast.Constant`、值必须是 `str`、且必须通过 `is_valid_name`。三条错误文案分别写清「必须是常量 / 必须是字符串 / 必须是合法标识符且非保留字」。
- `_check_init_function_parameters` 用 `zip_longest(reversed(args), reversed(defaults))` 从尾部对齐参数与默认值（Python 的默认值就是从右往左对应的）。没有默认值且不是 `self` 的参数记进 `non_defaults`；有默认值但不是字面量的记进 `non_literal_defaults`。

第二段：把上面四类问题各自汇成一条错误消息，拼进 `errors` 列表。四类分别是 `invalid_attributes`（逐条）、`complex_attributes`（一句 + 逗号分隔的名字列表）、`non_defaults`（一句 + 名单）、`non_literal_defaults`（一句 + 名单）。

第三段：对类体里**每个** `FunctionDef` 各造一个新 `MethodChecker`，把 `class_level_checker.class_attributes` 传进去作为「合法自由名」的一部分，跑完把它的 `errors` 逐条加上 `- {方法名}: ` 前缀汇入 `errors`。最后若有错，一次性抛 `ValueError("Tool validation failed for {类名}:\n" + 逐行)`。

**`MethodChecker` 的核心是「名字可追溯」这一个判据。** `visit_Name` 只在 `ctx` 是 `Load`（读）时工作，检查名字是否落在八类合法来源里：

1. `_BUILTIN_NAMES`——`set(vars(builtins))`，模块加载时算一次；
2. `BASE_BUILTIN_MODULES`——`collections`、`datetime`、`itertools`、`math`、`queue`、`random`、`re`、`stat`、`statistics`、`time`、`unicodedata`；
3. 函数参数名（`arg_names`，含 `*args` / `**kwargs`）；
4. 字面量 `"self"`；
5. 类属性名（由外部传入的 `class_attributes`）；
6. `import` 的名字（`imports`，键是 asname 或原名）；
7. `from ... import` 的名字（`from_imports`，值是 `(module, name)` 元组）；
8. 已赋值名（`assigned_names`）与已定义类名（`defined_classes`）；
9. 以及硬编码的 `typing_names = {"Any"}`。

都不在就追加 `Name '{x}' is undefined.`。

为了让「已赋值」这条判据不漏，它必须把 Python 里**所有会绑定名字的语法结构**都实现一遍，这正是这个访问器一半代码量的来源：`visit_Assign`（含元组/列表解包）、`visit_AnnAssign`、`visit_For`（含元组解包）、`visit_With`（`optional_vars`）、`visit_ExceptHandler`（`except ... as e` 的 `e`）、以及 `_handle_comprehension_generators` 被 `ListComp` / `DictComp` / `SetComp` 三者共用（生成器表达式未单独处理）。

`visit_Attribute` 有一处刻意的**短路**：如果属性访问的对象是名为 `self` 的 `Name`，就**不**继续 `generic_visit`。这挡住了「`self.foo` 里的 `foo` 被当成自由名」的误报。反过来说，`self.` 之后的属性名本身**不受检查**——所以「方法里用了 `self.xxx` 而 `xxx` 在 `__init__` 里才赋值」这种情况检查不出来。

`visit_Call` 与 `visit_Name` 的判据几乎重复（都在查同一个白名单），只是 `visit_Call` 期望 `node.func` 是 `ast.Name`（即 `foo(...)` 这种直接调用），检查完仍会 `generic_visit`。这段重复是显式写出来的，理由大概是 `visit_Name` 只覆盖 `Load` 上下文，而调用还有别的边界情况。

`check_imports` 参数被一路传递并存在 `self.check_imports` 上，但**在方法体内没有任何分支读取它**。也就是说目前 `MethodChecker` 的类文档说「contains no local imports」这条意图并未真正落在实现里——本地导入不会被拒绝。这一点从侧面说明 `get_tools_definition_code` 为什么要显式传 `check_imports=False`：反正它不影响行为，传 False 只是把意图写在调用点上。

**`validate_after_init` 是这套检查的挂载点。** 它把子类的 `__init__` 换成一个包装：先跑原 `__init__`，再跑 `self.validate_arguments()`（即 c01 里那五道声明侧检查）。注意它挂在**每个子类**上（由 `Tool.__init_subclass__` 触发），所以继承链上每一层都会被包一次。

**三类调用点，用途不同。**

- `Tool.to_dict`：子类化路径上调 `validate_tool_attributes(self.__class__)`（默认 `check_imports=True`）。**序列化前的门禁**——源码要能被别人跑起来，先证明它自包含。
- `Tool.to_dict` 的 `@tool` 路径：不调 `validate_tool_attributes`，而是对 `forward` 源码单独 `ast.parse` 后用 `MethodChecker(set())` 检查（类属性集为空，因为此刻还没有类体）。**同一道门禁的轻量版**。
- `get_tools_definition_code`：调 `validate_tool_attributes(tool.__class__, check_imports=False)`。这是**送给远程执行器前**的检查，也是注入源码的入口。

三处都指向同一个时间点：**源码即将离开本进程**。

**检查与生成共享同一个「差异导出」视角。** `get_tools_definition_code` 生成的不是单个工具，而是一整段以 `class Tool:` 打头的桩代码（只含 `__call__` 转发到 `forward`）加各工具类定义，最后每个工具再补一行 `{tool.name} = {类名}()` 实例化。所以远程端拿到的不只是源码，而是「一个最小 `Tool` 基类 + 若干子类 + 实例绑定」。这和 `get_imports` 反推出的 `requirements` 一起，构成远程端重建工具所需的全部材料。

## 关键取舍

**源码事实：** 校验是纯静态的，读源码字符串，不 import 不执行。所有判据都来自 AST 结构。

**源码事实：** 类属性只允许 `Constant` / `Dict` / `List` / `Set` 子树；任何一处出现其他节点就整条属性判为「复杂」。

**源码事实：** `__init__` 的每个参数都必须有默认值，且默认值必须是字面量。注释给了理由：init 时选定的参数无法被追溯，因而无法重建源码，所以重要的参数必须声明成类属性。

**源码事实：** `MethodChecker.check_imports` 被存储但从未被读取——「禁止本地导入」这条类文档声明目前没有落实到代码里。

**源码事实：** `visit_Attribute` 对 `self.xxx` 短路，因此 `self.` 之后的属性名不受检查。这意味着「`__init__` 里赋值的实例属性」与「方法里用的实例属性」之间的一致性不在检查范围内。

**架构推论：** 这套校验的目标不是安全，而是**可移植性**。它检查的是「这段代码在陌生进程里能不能找到它引用的每个名字」，而不是「这段代码会不会做坏事」。这也是为什么白名单里有 `os` 之外的一批标准库模块、却完全没有「危险函数」黑名单——黑名单是本地解释器（b01）的职责，静态校验只管自包含。

**架构推论：** 把检查挂在「即将离开进程」的三个点上，而不是在工具定义时就强制，是因为本地使用时并不需要源码可导出（`from_langchain` 这类包装器就明确不可导出）。检查点跟着**使用场景**走，而不是跟着类型走——这让「本地能跑但不适合分发」的工具合法存在。

**架构推论：** 「名字可追溯」这个单一判据很优雅，但它的代价是必须穷举 Python 所有绑定名字的语法。少了任何一处（例如生成器表达式、`match` 语句的模式绑定、`global` 声明），就会把合法代码误判为 undefined。这是一份会随语言演进而腐化的检查，源码里对 `match` 与 genexp 的缺失正是这种腐化的体现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/tool_validation.py` | `MethodChecker` | 方法级访问器：名字可追溯性 + `self.` 短路 |
| `src/smolagents/tool_validation.py` | `validate_tool_attributes` | 类级入口：类属性字面量、`name` 合法性、`__init__` 默认值、逐方法检查 |
| `src/smolagents/tool_validation.py` | `ClassLevelChecker` | 类体访问器，用 `in_method` 区分类体赋值与方法体赋值 |
| `src/smolagents/tool_validation.py` | `_check_init_function_parameters` | 从尾部对齐参数与默认值，收集无默认值/非字面量默认值 |
| `src/smolagents/tool_validation.py` | `_handle_comprehension_generators` | 三种推导式共用的生成器变量收集 |
| `src/smolagents/tools.py` | `validate_after_init` | 把声明侧自检挂到每个子类 `__init__` |
| `src/smolagents/tools.py` | `get_tools_definition_code` | 远程执行前的校验与源码注入入口 |
| `src/smolagents/utils.py` | `instance_to_source` | 只导出与基类不同的属性与方法，与方法校验互补 |

## 参考

- [smolagents 源码：tool_validation.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/tool_validation.py)
- [smolagents 文档：工具的自定义校验](https://huggingface.co/docs/smolagents/tutorials/tools)
- [Python 文档：ast 模块](https://docs.python.org/3/library/ast.html)
- [Python 文档：NodeVisitor](https://docs.python.org/3/library/ast.html#ast.NodeVisitor)

## 与主核 / AI 的关系

这套「静态证明自包含」的做法，与主核里的**依赖方向门禁**（`tools/check_deps.sh` 那类）是同一种工程手段：都在编译/运行之前用一个可穷举的规则集，证明某个单元不会引用它不该引用的东西。差别在判据的严格程度——主核的门禁按层与模块名判，规则很少、结论可靠；这里的门禁按「名字能否追溯到八类来源」判，规则更细但更难穷举完整，因此会有误报与漏报。

值得对照的是「类属性只许字面量」这条。主核里同样有这样的约束：配置文件只允许标量、不允许表达式，否则「配置」就变成了需要求值的程序。理由完全一致——**要被别的进程读取的东西，不能依赖当前进程的运行状态**。这条原则在 smolagents 里表现为 `complex_attributes` 检查，在数据库里表现为「目录表只存常量」。

对 AI 工程的实际意义在于：一旦工具的持久形态是源码，静态检查就不可避免，而且它必然是**保守的**——宁可拒绝一些能跑的代码，也不要放过一些会崩的代码。这套实现给出的三条具体做法都可直接借用：只允许字面量类属性、`__init__` 全部给字面量默认值（把可配置项提升为类属性）、以及「名字可追溯」检查。同时它也暴露了一个必须警惕的问题：这类基于 AST 的白名单检查会随 Python 版本演进而失效（新增语法会带来新的名字绑定形式），所以它需要与语言版本绑定维护，而不能一次写完就不管。
