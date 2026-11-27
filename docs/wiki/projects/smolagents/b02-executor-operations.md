---
project: smolagents
feature: executor-operations
title: 解释器支持的语句与表达式
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/local_python_executor.py
    symbols: [evaluate_while, evaluate_lambda, evaluate_class_def]
  - path: src/smolagents/local_python_executor.py
    symbols: [evaluate_augassign, create_function, evaluate_boolop]
---

# 解释器支持的语句与表达式

## 一句话职责

`evaluate_ast` 是一张 43 个分支的显式分派表：它把「Python 语言」裁剪成一个已知子集，每种语法结构都有对应的 `evaluate_*` 实现，没写进来的语法一律抛「is not supported」——**能力边界就是这张表的长度**。

## 机制是什么

`evaluate_ast` 是一个按 `isinstance(expression, ast.X)` 逐条比对的巨型 `if/elif` 链，末尾是 `raise InterpreterError(f"{...} is not supported.")`。没有注册表、没有装饰器、没有 `getattr(self, f"evaluate_{type}")` 之类的动态分派——加一种语法就是加一个分支。

支持面按 AST 节点分类如下：

| 类别 | 支持的节点 |
|---|---|
| 赋值 | `Assign`、`AnnAssign`、`AugAssign` |
| 表达式 | `Constant`、`Name`、`BinOp`、`UnaryOp`、`BoolOp`、`Compare`、`IfExp`、`Attribute`、`Subscript`、`Slice`、`Starred`、`Tuple`、`List`、`Dict`、`Set`、`JoinedStr`/`FormattedValue`（f-string）、`Lambda` |
| 推导式 | `ListComp`、`SetComp`、`DictComp`、`GeneratorExp` |
| 控制流 | `If`、`For`、`While`、`Break`、`Continue`、`Return`、`Pass` |
| 定义 | `FunctionDef`、`ClassDef` |
| 异常 | `Try`、`Raise`、`Assert` |
| 其它语句 | `Expr`、`With`、`Delete`、`Import`、`ImportFrom` |
| 兼容 | `ast.Index`（老版本 Python 的下标包装，存在时透传） |

**明确不支持的**：`match` 模式匹配、海象运算符 `:=`（`NamedExpr`）、`yield`/`await`/`async def`/`async for`/`async with`、`global`/`nonlocal`、`except*`（`TryStar`）、类型别名语句。这些节点在文件里连字符串都没出现过，落在末端的通用报错上。

## 怎么实现的

**先看门，再看分支。** `evaluate_ast` 开头做两件事：检查 `state["_operations_count"]["counter"] >= MAX_OPERATIONS`（1000 万次）就抛错，然后自增。计数器存在 state 里，但**每次 `evaluate_python_code` 开头都会把它重置成 `{"counter": 0}`**（与 `_print_outputs` 一起重置），所以预算是「每次代码执行一份」，不跨调用累积——注释里给出的用途是「可能是代码里有死循环，也可能只是算得太多」。

这一点很容易读错：计数器住在 `state` 这个跨调用持久化的字典里，看起来像是累积的，但入口处那行重置把它变成了一次性的。`MAX_WHILE_ITERATIONS`（100 万）则是 `evaluate_while` 里的局部变量，同样是每次进入该 while 循环重新计数。

**控制流靠自定义异常实现，而不是返回值。** 三个类：`BreakException`、`ContinueException`、`ReturnException`（后者带 `value`）。`evaluate_ast` 遇到 `ast.Break` 直接 `raise BreakException()`，遇到 `ast.Continue` 抛对应异常，遇到 `ast.Return` 抛 `ReturnException(值)`。循环与函数体在合适的层级捕获它们：

- `evaluate_for`：`try` 包住循环体，`BreakException` 直接 `return result`，`ContinueException` 只 `break` 内层 `for node in body`；
- `evaluate_while`：同上，但 `iterations > MAX_WHILE_ITERATIONS`（100 万）时抛错，且函数恒返回 `None`；
- `create_function`：`try` 包住函数体，捕到 `ReturnException` 就把 `e.value` 当返回值。

这套做法与 CPython 的字节码跳转不同：这里用异常把控制流交回到正确的层级，所以「嵌套循环里 break 只会退出当前循环」是自然成立的——只要每一层循环各自捕获。

**`evaluate_while` 与 `evaluate_for` 对 break 的返回值处理不一致。** `while` 捕到 `BreakException` 后 `return None`；`for` 捕到后 `return result`（此前累积的最后非空值）。读代码时这个不对称容易被忽略。

**函数的语义有三处与 CPython 不同，都写在 `create_function` 里。**

1. **作用域是「调用时刻的 state 副本」**：`func_state = state.copy()` 在 `new_func` 内部执行，因此函数体可以看见**定义之后才出现的变量**（只要调用时已在 state 里），这与 Python 的闭包按定义位置绑定不同；
2. **默认值在每次调用时求值**：`default_values = [evaluate_ast(d, state, ...) for d in func_def.args.defaults]` 也在 `new_func` 内部，所以 `def f(x=[]):` 这类可变默认值在这里**不会**跨调用共享——正好躲开了 Python 的经典陷阱，但语义已经变了；
3. **函数体结束时返回最后一条语句的值**：`for stmt in func_def.body: result = evaluate_ast(...)`，没有 `return` 就把最后一条语句的求值结果返回。`__init__` 是特例，强制返回 `None`。

`self`/`__class__` 是被专门注入的：若第一个参数名是 `self` 且确实有实参，就把 `func_state["self"]` 与 `func_state["__class__"]` 设上——这是让类方法里的 `super()` 能工作的前提。

函数对象上还挂了三个属性：`__ast__`（原始 AST）、`__source__`（`ast.unparse` 出的源码）、`__name__`。源码保留是为了工具序列化时能拿回定义。

**`evaluate_lambda` 与函数定义共享同一套作用域规则**，但更简：只看 `args`，`new_state = state.copy()`，`zip` 上参数值后求值函数体。没有默认值、没有 `*args`/`**kwargs` 处理。

**类定义需要手工模拟元类协议。** `evaluate_class_def` 的步骤：

1. 求值所有基类；
2. 取元类——遍历基类，第一个 `type(base) is not type` 的元类的类胜出，否则用 `type`；
3. 若元类有 `__prepare__` 就用它拿类字典（注释点名 Enum 的 `_EnumDict`），否则用普通 dict；
4. 逐条处理类体，**只允许五种形态**：`FunctionDef`（存进类字典）、`AnnAssign`（写 `__annotations__`，有值则同时赋值）、`Assign`（写类字典或 `setattr`）、`Pass`、以及「第一条语句且是字符串常量」的 docstring（写 `__doc__`）；
5. 其它任何语句抛 `Unsupported statement in class body`；
6. 最后 `metaclass(name, tuple(bases), class_dict)` 造类并写回 state。

其中 `AnnAssign` 还细分了三种 target（`Name`、`Attribute`、`Subscript`），`Subscript` 这种形态注释举例是 `dict[key]: int`。

**增量赋值实现成「取值 → 就地运算 → 写回」。** `evaluate_augassign` 先 `get_current_value(target)`（支持 `Name`/`Subscript`/`Attribute`/`Tuple`/`List` 五种 target），再按运算符分支就地修改，最后 `set_value` 写回并返回新值。两处细节偏离 CPython：

- 未定义的名字当 0：`state.get(target.id, 0)`——所以 `x += 1` 对未定义的 `x` 会得到 `1` 而不是 `NameError`；
- 列表加法有类型检查：`current_value` 是 list 而右值不是 list 时抛「Cannot add non-list value … to a list」，这条检查在 `+=` 里有、在普通 `+` 里没有。

**赋值会挡住对工具名的覆盖。** `set_value` 对 `ast.Name` 的第一件事是 `if target.id in static_tools: raise InterpreterError("Cannot assign to name '…': doing this would erase the existing tool!")`。这就是「工具不可被代码覆盖」这条规则的唯一实现点。解包赋值另有一整套 CPython 对齐逻辑：`list(value)` 转序列（不可迭代就报 cannot unpack）、支持单个 starred target 吸收余量、多重 starred 报错、数量不足/过多分别报错。

**布尔运算短路并返回操作数本身，不是布尔值。** `evaluate_boolop` 用两个 lambda 区分短路条件：`and` 用 `lambda x: not x`（遇假即返回该假值）、`or` 用 `lambda x: bool(x)`（遇真即返回该真值），全都没短路就返回最后一个值。这与 Python 语义一致，也意味着 `a or b` 的结果类型取决于 `a`。

**比较运算支持链式，并在首次判假时提前返回。** `evaluate_condition` 遍历 `ops`/`comparators` 配对，支持 `== != < <= > >= is is not in not in` 十种；`current_result is False` 直接返回 `False`（注意用的是 `is False`，所以返回 numpy 布尔之类不会被误判），否则逐项 `and` 累积，`left` 滚到 `right`。

**名字查找带「拼写纠错」。** `evaluate_name` 的优先级是 `state` → `static_tools`（经 `safer_func` 包一层）→ `custom_tools` → `ERRORS`（内建异常类，这样代码里能 `raise ValueError(...)`）。都没命中时，用 `difflib.get_close_matches(name.id, list(state.keys()))` 找最近的键，找到就**返回那个值**而不是抛错。这是一个有争议的容错：模型把变量名拼错一个字，代码会静默用另一个变量继续跑。相较之下 `evaluate_subscript` 的纠错温和得多——只在报错信息里附一句「Maybe you meant one of these indexes instead」。

**函数调用的门禁有三道。** `evaluate_call` 在真正调用前检查：

1. 被调对象若是内建函数且不在 `static_tools` 里 → 抛「Invoking a builtin function that has not been explicitly added as a tool is not allowed」。注释写在条件里：`(inspect.getmodule(func) == builtins) and inspect.isbuiltin(func) and (func not in static_tools.values())`——所以白名单是「同一函数对象出现在 static_tools 的某个值里」，用户拿别名包一层并不能绕过；
2. 名字是 dunder 且不在 `static_tools` 且不在 `ALLOWED_DUNDER_METHODS`（`__init__`/`__str__`/`__repr__`）→ 抛「Forbidden call to dunder function」；
3. `super` 与 `print` 被特判：`super` 支持 0/1/2 参数三种形态（0 参数时依赖 state 里的 `__class__` 与 `self`），`print` 完全不进 Python 的 print——它把参数用空格拼起来写进 `state["_print_outputs"]` 再返回 `None`。

参数展开支持 `*args` 与 `**kwargs`（后者要求是 dict，否则报 cannot unpack non-dict）；`func` 可以是 `Call`/`Lambda`/`Attribute`/`Name`/`Subscript` 五种形态，其它形态直接报「This is not a correct function」。

**推导式的实现分两套，生成器表达式那套有多子句缺陷。** `_evaluate_comprehensions` 是一个递归生成器：接收生成器子句列表，逐个求值 `iter`、对每个值 `state.copy()` 后绑定 target、检查所有 `if` 子句、再递归下一层，最内层 `yield evaluate_element(state)`。`evaluate_listcomp`、`evaluate_setcomp`、`evaluate_dictcomp` 都调它，只是外面套 `list()`/`set()`/`dict()`。**每层都用 state 副本**，所以推导式里的循环变量不会泄漏到外层。

但 `evaluate_generatorexp` 没走这条路——它自己写了一个 `for gen in genexp.generators:` 的平铺循环，对每个子句各自独立遍历并 `yield` 元素，而不是像前者那样把多个子句做笛卡尔展开。后果是**多子句的生成器表达式会报错而不是给出结果**：`list((x, y) for x in [1, 2] for y in [10, 20])` 在求值第二子句产生的元素时，`new_state` 里只有 `y` 没有 `x`（因为第一个子句的绑定只存在于它自己的那轮 `new_state`），于是抛「The variable `x` is not defined」；单子句的 `(x * 2 for x in ...)` 正常。用同样代码实测确认：`[(x, y) for x in [1,2] for y in [10,20]]` 得到完整的四元素笛卡尔积，而生成器版本报错。这是本文件里一处明确的实现不对称，属于「语法被列进支持表、但语义只覆盖了常见子集」的情形。

**`try` 的语义基本对齐，但有一处退化。** `evaluate_try` 逐条跑 body，捕 `Exception`（**不是** `BaseException`，所以 `FinalAnswerException` 这类不会进来），遍历 handlers：`handler.type is None` 视为通配，否则求值异常类型并 `isinstance` 判断；命中则把异常绑定到 `handler.name`（若有）并跑 handler 体；没命中就 `raise e` 重抛。`orelse` 在无异常时跑，`finalbody` 恒跑。退化点是 `raise e` 会让 traceback 从当前位置起算，原始抛出点信息丢失。

**裸 `raise` 不支持。** `evaluate_raise` 里若 `raise_node.exc is None` 直接抛 `InterpreterError("Re-raise is not supported without an active exception")`。所以 `except: raise` 这种常见写法在这里是解释器错误。

**`with` 是手工实现 `__enter__`/`__exit__` 协议，并正确模拟了抑制语义。** `evaluate_with` 先对所有 item 调 `__enter__` 并绑定 `optional_vars`；body 抛异常时构造 `exc_info`，然后**从最内层往外**逐层 `__exit__(*exc_info)`——任何一层返回真值就把 `exc_info` 重置为 `(None, None, None)`（对外层而言异常已被抑制），某层 `__exit__` 自己抛错则用新异常替换 `exc_info`；最后 `exc_info[1]` 非空才 `raise ... with_traceback(...)`。无异常时逐层 `__exit__(None, None, None)`。这段是全文件里对 Python 协议还原得最细的一处。

**`import` 拿到的是「安全副本」，不是真模块。** `evaluate_import` 先 `check_import_authorized` 逐段查树，通过后 `import_module` 真模块，再交给 `get_safe_module` 造一个新的 `ModuleType`，把属性**按引用**逐个拷过去、遇到子模块递归处理、用 `visited` 集合挡循环引用。所以模型拿到的是一个冒牌模块对象：属性都指向真对象（函数调用仍会真的执行），但模块的 `__dict__` 是新的，往模块上写属性不会污染真模块。`from x import *` 会优先用模块的 `__all__`，没有则取所有不以 `_` 开头的名字。

`evaluate_delete` 只支持两种 target：`del name`（不存在则报错）与 `del obj[key]`。删属性、删切片都不支持。

## 关键取舍

**源码事实：** 分派是手写 `if/elif` 链、43 个分支，没有动态注册。加一种语法必须改这个函数，因此「支持哪些语法」在一处即可读完，但也没有插件式扩展点。

**源码事实：** 操作计数器（`MAX_OPERATIONS` 1000 万）虽然存在 `state["_operations_count"]` 里，但每次 `evaluate_python_code` 入口都会重置，所以是「每次执行一份预算」，不跨调用累积。

**源码事实：** `evaluate_name` 的拼写纠错会**静默返回近似变量**，而 `evaluate_subscript` 只把近似候选写进错误消息。同一个文件里两种容错策略，风险完全不同。

**源码事实：** 函数的默认值在每次调用时重新求值，函数体看到的是调用时刻的 state 副本。这让「定义后新增的变量对函数可见」，与 Python 的闭包规则不同。

**源码事实：** `x += 1` 对未定义的 `x` 得到 `1`（`state.get(target.id, 0)`），不报 `NameError`。

**源码事实：** 裸 `raise` 不支持，`except: raise` 会是解释器错误。`try` 的异常重抛用 `raise e`，会丢掉原始 traceback。

**源码事实：** 函数调用对内建函数与 dunder 方法各有一道门禁，白名单判据是「同一函数对象在 `static_tools` 的值里」，别名包装绕不过去。

**架构推论：** 这种「显式分派表」的设计让能力成长是**可控且可审计**的：没有意外的语义，因为每个节点都必须被手工实现；也没有意外的新语法，因为 Python 版本升级带来的新节点默认落在「不支持」。代价是实现量与语言表面成正比——这个文件近 1800 行，绝大部分是逐节点的语义还原。

**架构推论：** 用异常实现控制流，使嵌套结构的作用域天然正确（每层循环自己捕自己的 break），但代价是「每个支持语句的实现者都必须记得捕对应异常」——漏捕会让 `break` 直接穿透到最外层，表现为「循环提前结束但没报错」。

**架构推论：** 手工还原 `with`、元类、解包、链式比较这些「边缘语法」，说明这个解释器的目标不是「够用」，而是「让模型写的正常 Python 尽量都跑得通」。这从投入产出看是合理的：模型不会刻意用冷门语法，但会无意中用到解包、f-string、`with open(...)` 这些日常写法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/local_python_executor.py` | `evaluate_ast` | 43 分支的总分派表与每次执行重置的操作计数门 |
| `src/smolagents/local_python_executor.py` | `evaluate_while` | while 循环、100 万次上限、break 返回 `None` |
| `src/smolagents/local_python_executor.py` | `evaluate_lambda` | lambda 求值，state 副本作用域 |
| `src/smolagents/local_python_executor.py` | `create_function` | 函数对象的构造：state 副本、默认值延迟求值、self 注入 |
| `src/smolagents/local_python_executor.py` | `evaluate_class_def` | 类体白名单、元类与 `__prepare__` 模拟 |
| `src/smolagents/local_python_executor.py` | `evaluate_augassign` | 增量赋值，未定义名字当 0，就地运算 |
| `src/smolagents/local_python_executor.py` | `evaluate_boolop` | 短路并返回操作数本身 |
| `src/smolagents/local_python_executor.py` | `evaluate_condition` | 链式比较，首次判假即返回 |
| `src/smolagents/local_python_executor.py` | `evaluate_assign` / `set_value` | 赋值与解包，禁止覆盖工具名 |
| `src/smolagents/local_python_executor.py` | `evaluate_call` | 调用门禁：内建函数白名单、dunder 白名单、`super`/`print` 特判 |
| `src/smolagents/local_python_executor.py` | `evaluate_name` | 名字查找与近似匹配兜底 |
| `src/smolagents/local_python_executor.py` | `_evaluate_comprehensions` | 四种推导式共用的递归生成器 |
| `src/smolagents/local_python_executor.py` | `evaluate_try` | 异常处理、orelse、finalbody |
| `src/smolagents/local_python_executor.py` | `evaluate_raise` | 不支持裸重抛 |
| `src/smolagents/local_python_executor.py` | `evaluate_with` | 手工 `__enter__`/`__exit__` 与抑制语义 |
| `src/smolagents/local_python_executor.py` | `evaluate_import` / `get_safe_module` | 授权查树后返回模块的安全副本 |
| `src/smolagents/local_python_executor.py` | `evaluate_delete` | 仅支持删名字与删下标 |
| `src/smolagents/local_python_executor.py` | `BreakException` / `ContinueException` / `ReturnException` | 用异常承载控制流 |
| `src/smolagents/local_python_executor.py` | `MAX_OPERATIONS` / `MAX_WHILE_ITERATIONS` | 两级循环预算 |

## 参考

- [smolagents 源码：local_python_executor.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/local_python_executor.py)
- [smolagents 文档：代码执行安全](https://huggingface.co/docs/smolagents/tutorials/secure_code_execution)
- [Python 文档：ast 模块](https://docs.python.org/3/library/ast.html)

## 与主核 / AI 的关系

这张分派表与主核的「SQL 表达式/算子求值」是同一种工程形态：一个显式的节点类型到求值函数的映射，没有对应实现的节点就在解析或求值阶段被拒绝。主核里对应的是「算子注册表 + 未知算子报错」，这里因为没有注册机制而退化成一条 `if/elif` 链——功能等价，可扩展性与可读性差一档。

对写「受限语言解释器」的人，这个文件里有五条可直接搬的经验。第一，**用异常承载控制流**比自己维护跳转栈简单得多，代价是每个支持循环的实现者都要记得捕获。第二，**作用域用 `state.copy()`** 实现闭包最省事，但要明确写清「看到的是调用时刻的外层状态」，否则语义会漂。第三，**默认值延迟求值**顺手躲开了可变默认值陷阱，但这也是语义偏离，必须记录。第四，**拼写纠错要分场景**：名字查找静默纠错会让错误代码跑出错误结果，只把候选写进报错消息才是安全的做法。第五，**`with` 与元类这类语法不能省**——模型写日常 Python 一定会用到，缺了会让整段代码失败，而不是降级。

最后一条关于取舍的判断：这个解释器愿意为「对齐 CPython 的边缘语义」付出接近两千行代码，说明作者把「模型写的代码能跑」排在「实现简单」之上。反过来，如果一个系统的目标只是执行受控的表达式片段，就该砍掉类、`with`、推导式这些，让分派表短一半——**边界画在哪里，取决于你要执行的代码是人写的还是模型写的**。
