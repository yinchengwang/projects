---
project: autogen
feature: code-executor
title: 代码执行器与依赖声明
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/code_executor/_base.py
    symbols: [CodeExecutor, CodeBlock, CodeResult]
  - path: python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py
    symbols: [FunctionWithRequirements, with_requirements, build_python_functions_file]
---

# 代码执行器与依赖声明

## 一句话职责

给出「把代码块放进某个环境里跑、拿回退出码与输出」的最小契约，并提供一套把函数及其包依赖/导入依赖打包成可注入脚本的机制，使生成式代码能安全地调用宿主预先批准的函数。

## 机制是什么

`CodeExecutor` 的契约只有三个方法加一个动作：`execute_code_blocks(code_blocks, cancellation_token)` 执行一批代码块返回一个 `CodeResult`，`start`/`stop`/`restart` 管理环境生命周期。加上 `__aenter__`/`__aexit__` 转调启停，就构成全部。

数据模型都是 `dataclass` 而非 pydantic 模型：`CodeBlock(code, language)` 与 `CodeResult(exit_code, output)`。**一批代码块只回一个结果**——没有逐块的退出码，也没有结构化的中间状态，只有一个整体 `output` 字符串。这个取舍决定了它只能服务「连续脚本」语义，不能做步骤级诊断。

真正的特色在另一半：`_func_with_reqs.py` 里的依赖声明体系。`FunctionWithRequirements` 把一个可调用对象连同 `python_packages`（pip 包名列表）和 `global_imports`（导入语句列表）包在一起；`with_requirements(...)` 是它的装饰器形态；`build_python_functions_file(funcs)` 把一批这样的函数**拼成一个 Python 源文件**，供执行器在沙箱里 import。

`Import` 类型是 `Union[str, ImportFromModule, Alias]` 三种形态的联合，分别对应 `import x`、`from m import a, b`、`import x as y`。

## 怎么实现的

**执行器契约的边界。** 三个抽象方法加一个 `restart`——`restart` 的 docstring 说它「在智能体重置时被调用」，但基类没有任何地方调它。所以「重启」是留给上层的约定，实现必须提供但不保证被调用。生命周期用 `__aenter__`/`__aexit__` 做语法糖，`__aexit__` 返回 `None`（不吞异常）。

**`FunctionWithRequirements` 的两个形态。** 一个是持有 `Callable` 的 `FunctionWithRequirements`，`__call__` 直接转调 `self.func`；另一个是持有**源码字符串**的 `FunctionWithRequirementsStr`，它在 `__init__` 里用 `importlib` 的 `_StringLoader`（`SourceLoader` 子类）加 `spec_from_loader` 把字符串编译成模块、`exec_module` 执行、再用 `inspect.getmembers(module, inspect.isfunction)` 取函数；**必须恰好一个函数**，否则 `ValueError("The string must contain exactly one function")`。它的 `__call__` 直接 `NotImplementedError`——字符串形态只能被导出，不能被本地调用。

**导入语句的文本化。** `_import_to_str` 三支：字符串写 `import {x}`，`Alias` 写 `import {name} as {alias}`，`ImportFromModule` 把 `imports` 里每项按「字符串原样 / `Alias` 写成 `name as alias`」拼成逗号列表，写成 `from {module} import ...`。`build_python_functions_file` 先把所有函数（两种带依赖的形态）的 `global_imports` 收进一个 `set`——`ImportFromModule` 与 `Alias` 都是 `frozen=True` 的 dataclass，所以可哈希，集合去重才成立——拼成开头一段 import，再逐个追加函数源码。

**源码抽取。** `_to_code` 对 `FunctionWithRequirementsStr` 直接返回 `func.func`（原始字符串）；其余走 `inspect.getsource`，若源码以 `@` 开头则切掉第一行（去装饰器）。所以装饰器被剥掉是对的——`@with_requirements(...)` 在目标环境里没有定义，留着会导致 SyntaxError 之外更糟的 NameError。

**`with_requirements` 的实现。** 它返回一个 wrapper，把目标函数包成 `FunctionWithRequirements(python_packages=..., global_imports=..., func=func)`，再 `functools.update_wrapper` 把 `__name__`、`__doc__` 等拷过去。所以装饰后得到的**不是一个函数而是一个可调用对象**，`inspect.signature` 之类还能工作，但 `inspect.isfunction` 会失败。

**辅助输出。** `to_stub(func)` 生成函数签名加 docstring 的桩：`def name(签名):` 加缩进的 docstring 加 `    ...`。`to_code` / `import_to_str` 只是 `_to_code` / `_import_to_str` 的公开别名。`FunctionWithRequirements.from_callable` 与 `FunctionWithRequirements.from_str` 是两个便捷构造器，后者返回的是 `FunctionWithRequirementsStr`——**返回类型与类的类型不同**，这个不对称在类型标注里明确写出来了。

**向后兼容的小动作。** `ImportFromModule` 是 `frozen=True` dataclass 却自定义了 `__init__`：用 `object.__setattr__` 绕过 frozen，并把 `list` 形式的 `imports` 转成 `tuple`。这样既保持了可哈希，又允许调用方传列表。

## 关键取舍

**源码事实：** `CodeResult` 只有一个 `exit_code` 与一个 `output`，对应一整批代码块。所以「第 3 块失败了」无法从结果里看出来，除非自己往输出里打标记。

**源码事实：** `FunctionWithRequirementsStr.__call__` 抛 `NotImplementedError`，但它的 `compiled_func` 是真实编译出的函数。也就是说能力存在，只是不通过 `__call__` 暴露——设计上把「可本地调用」与「仅可导出」明确分开。

**源码事实：** `build_python_functions_file` 用 `set` 去重导入，而 `ImportFromModule` 的去重粒度是**整个 from 语句**。`from pandas import DataFrame` 与 `from pandas import concat` 是两条不同的语句，不会被合并成一条——生成的文件里会出现两行 from pandas。

**源码事实：** `_to_code` 的装饰器剥离靠「源码是否以 `@` 开头」。多层装饰器时只切掉第一行，剩下的 `@` 装饰器仍会留在输出里，导致生成的脚本引用不存在的装饰器。

**源码事实：** `python_packages` 只是**声明**，`build_python_functions_file` 完全不使用它。装包的责任在执行器实现（`autogen-ext` 里的 Docker/本地执行器会读它并 pip install）。核心包只保证这份声明随函数一起被传递。

**架构推论：** 为什么核心包只定义契约、不提供实现？因为「在哪儿跑」是部署决策，涉及到容器、权限、网络隔离，任何默认实现都会有安全争议。核心包给出的是**形状**：一批代码块 + 取消令牌 → 一个退出码与输出；以及**如何把宿主函数塞进去**的统一格式。这种把危险实现推给扩展包的分层，是 Autogen 里少见的、刻意的不完整。

**架构推论：** 声明式依赖解决了「生成代码要调用宿主准备好的函数」这个具体问题：模型不需要知道 `load_data` 的内部实现，只要 `from <functions_module> import load_data` 即可；而执行器负责生成那个 module 并把依赖装好。这让「允许模型调用哪些函数」成为一个显式白名单——生成代码能 import 的东西就是已授权的那些，而不是整个宿主进程。

**架构推论：** `global_imports` 用 dataclass 而非字符串，是为了让「导入」成为**可去重、可比较、可序列化**的值。若直接存 `"from pandas import DataFrame"` 这样的字符串，去重就得靠字符串相等，而 `Alias` 之类的别名信息也会丢失。代价是每次都要重新渲染成文本。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/code_executor/_base.py` | `CodeExecutor` | 执行器抽象：执行、启停、重启、上下文管理 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_base.py` | `CodeExecutor.execute_code_blocks` | 一批代码块换一个结果 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_base.py` | `CodeBlock` | 代码文本加语言标记 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_base.py` | `CodeResult` | 退出码加合并输出 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `FunctionWithRequirements` | 函数加包依赖与导入依赖，可直接调用 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `FunctionWithRequirementsStr` | 源码字符串形态，仅编译与导出 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `with_requirements` | 装饰器形态，产出可调用对象 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `ImportFromModule` | from 导入的值对象，frozen 可哈希 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `Alias` | 导入别名值对象 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `build_python_functions_file` | 汇总导入并拼接函数源码成单文件 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `_to_code` | 抽取源码并剥掉首行装饰器 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `to_stub` | 生成签名加 docstring 的桩 |
| `python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py` | `_StringLoader` | 把字符串当源码模块加载的 loader |

## 参考

- [AutoGen 源码：code_executor/_base.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/code_executor/_base.py)
- [AutoGen 源码：code_executor/_func_with_reqs.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py)
- [AutoGen 文档：Command Line Code Executors](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/command-line-code-executors.html)

## 与主核 / AI 的关系

主核执行的是**声明式**语句：查询被解析成执行计划，每一步的输入输出由算子契约固定，失败有明确错误码与位置。代码执行器执行的是**命令式**脚本，无法预知副作用，因此它把一切收敛到「退出码 + 输出」两个观察点，并鼓励把危险动作推到隔离环境（这正是扩展包里 Docker 执行器存在的理由）。理解这条差别，才能判断什么时候该用代码执行（需要表达力、可容忍不可预测性），什么时候该用结构化工具（需要可审计、可回滚）。

对 AI 工程来说，`with_requirements` 解决的是一个非常具体的问题：**模型生成的代码要调用宿主的能力，但不能随便 import**。声明依赖后，执行器只需把批准的函数与它们的依赖装进沙箱，生成代码用固定模块名 import。这是把「能力授权」落实到语法层面的做法——比在提示里写「只能调用这些函数」可靠得多，因为越权的 import 会直接失败。

第二条实践要点是那次「装饰器剥离」。`inspect.getsource` 会带上 `@with_requirements(...)`，而目标环境里没有这个装饰器，直接执行会 NameError。剥掉首行装饰器是必要的修补，但它只处理一层——多层装饰或注释夹在装饰器与 `def` 之间时会残留。写带依赖的函数时保持「单层装饰器、紧跟 def」是最安全的形态。

最后一条：`CodeResult` 是整批的结果，所以生成多块代码时，正确做法是让每块在末尾打印可识别的标记，或干脆一次只跑一块。指望从合并输出里恢复逐块状态，会在出错的场景下拿到无法定位的文本。
