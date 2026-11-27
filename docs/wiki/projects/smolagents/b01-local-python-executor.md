---
project: smolagents
feature: local-python-executor
title: 本地 Python 解释器与能力裁剪
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/local_python_executor.py
    symbols: [LocalPythonExecutor, evaluate_attribute, check_import_authorized]
  - path: src/smolagents/local_python_executor.py
    symbols: [InterpreterError, ExecutionTimeoutError, timeout]
---

# 本地 Python 解释器与能力裁剪

## 一句话职责

这个模块用 AST 遍历实现了一个「只能做被允许的事」的 Python 子集解释器：它不执行编译后的字节码，而是逐个节点求值，因此每个语法结构、每个内置函数、每个导入都可以单独放行或拒绝。

## 机制是什么

smolagents 的 CodeAgent 让模型生成 Python 代码作为动作。要跑这些代码又不把整台机器交出去，有两条路：开一个真沙箱（进程、容器、微虚机），或者在同一个进程里用一个受控解释器。

本地解释器选的是后者。它**不是安全沙箱**——源码的类文档字符串直接写明这一点，并建议需要隔离不可信代码时改用远程执行器。它防的是「模型写了不该写的东西」这类错误与误用，不是「恶意代码」。

它的做法是把 `ast.parse` 出来的语法树逐节点手工求值。标准解释器里「执行字节码」这一步被替换成「按节点类型分派到 `evaluate_*` 函数」。凡是没写对应 `evaluate_*` 的语法结构，就自然不可用。

## 怎么实现的

**三道能力裁剪。**

第一道是**内置函数白名单**。`BASE_PYTHON_TOOLS` 是一张显式的名字到对象映射：`print`、`range`、`int`、`len`、`sorted`、`zip` 这些基础函数，加上 `math` 里的常见函数。注意 `print` 被替换成 `custom_print`（一个直接 `return None` 的空实现）——真正的输出走另一条路径：代码执行时 `state["_print_outputs"]` 是一个 `PrintContainer`，由解释器自己收集，最后截断成日志字符串。所以模型看到的是「打印出来的文本」，但代码里调 `print` 不会真的写标准输出。

第二道是**导入授权**。`check_import_authorized` 把授权列表建成一棵树（`build_import_tree`），然后对要导入的模块名按点号逐段下走：某一层出现 `*` 就整体放行，某一层不存在该段就拒绝。这比单纯的字符串前缀匹配更精确——授权 `os.path` 不等于授权 `os.pathlib`，也不等于授权 `os`。

额外一层在构造期：`_check_authorized_imports_are_installed` 用 `find_spec` 检查每个授权模块是否真的装了，缺一个就在构造时报 `InterpreterError`。这把「运行到一半才发现模块不存在」提前成了启动期失败。

第三道是**dunder 属性禁止**。`evaluate_attribute` 先检查属性名是否 `__x__` 形态，是就直接抛错。这挡住的是 `().__class__.__bases__[0].__subclasses__()` 这类从普通对象爬回类型系统的经典逃逸路径。同名防护还有一个 `nodunder_getattr`，用于 `getattr` 函数路径。

**求值主循环。** `evaluate_python_code` 先 `ast.parse`（语法错误包装成带行号与脱字符的 `InterpreterError`），然后逐条语句调 `evaluate_ast`，把结果累积到 `state`。所以**变量在多次执行之间是保留的**——`state` 是执行器实例上的字段，每次 `__call__` 复用同一个 dict。

**静态工具与自定义工具的分工。** 两者都进 `static_tools` 供代码调用，但语义不同：源码文档明确写「`static_tools` 不可被代码覆盖，任何对它们名字的赋值都会报错；`custom_tools` 可以被覆盖」。这个区分在收尾时体现——`send_tools` 把工具与内置函数合并成新的 `static_tools`，覆盖上一层。

**最终答案的异常通道。** 如果工具表里有 `final_answer`，`evaluate_python_code` 会把它包一层：调 `final_answer(...)` 时先执行原函数拿到值，再抛 `FinalAnswerException(value)`。这个异常继承 `BaseException` 而不是 `Exception`，于是不会被普通的 `except Exception` 吞掉，主循环能确定地识别「这是最终答案」而不是「代码出错了」。

**超时。** `timeout` 装饰器用单线程 `ThreadPoolExecutor` + `future.result(timeout=...)`。源码注释坦白了它的局限：Python 无法强制杀死线程，所以超时后调用方拿到 `ExecutionTimeoutError` 继续走，但那个线程会在后台一直跑到自然结束。这是「跨平台 + 线程安全」换来的代价——基于 `signal` 的方案更彻底，但只能在主线程用且不支持 Windows。

## 关键取舍

**源码事实：** 类文档字符串明确写「这不是安全沙箱」（not a security sandbox），并指向远程执行器。所以三道路由的作用是**减少误用面**，不是建立信任边界。

**源码事实：** `timeout` 的实现用线程池而非 `signal`，注释说明理由（跨平台、任意线程可用），并明确记录「超时后线程无法被强杀，会在后台继续」。这意味着超时只保证调用方不被阻塞，不保证副作用停止。

**源码事实：** `final_answer` 通过抛 `BaseException` 子类来短路执行，而非常规返回。这样即使用户代码里有 `try/except Exception`，最终答案也不会被吞。

**源码事实：** 授权导入用树形逐段匹配而非字符串前缀，`*` 只在某一层生效。构造期还会用 `find_spec` 预检模块是否安装。

**架构推论：** 用 AST 解释器替代真沙箱，换来的是「可以精确到单个语法节点地控制能力」，代价是「必须自己实现每种语句的语义」。这个模块有 1796 行，其中绝大多数是 `evaluate_*` 系列——每支持一种语法就多一份实现与维护成本。它同时意味着**新语法（含 Python 版本升级带来的新节点）默认不可用**，这是一条保守但安全的默认。

**架构推论：** 「变量在多次调用间保留」使代码式智能体可以像 REPL 一样分步工作：第一步导入并读取数据，第二步基于同一个变量继续算。这是 CodeAgent 相对 JSON 工具调用的真实优势——不必把中间结果序列化到消息里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/local_python_executor.py` | `LocalPythonExecutor` | 本地执行器主体：状态、工具、授权与调用入口 |
| `src/smolagents/local_python_executor.py` | `evaluate_python_code` | 求值主函数：解析、逐句求值、最终答案通道、超时包装 |
| `src/smolagents/local_python_executor.py` | `evaluate_attribute` | 属性访问，含 dunder 禁止 |
| `src/smolagents/local_python_executor.py` | `check_import_authorized` | 树形逐段模块授权判定 |
| `src/smolagents/local_python_executor.py` | `timeout` | 线程池超时装饰器及其已知局限 |
| `src/smolagents/local_python_executor.py` | `InterpreterError` | 所有解释器侧失败的统一错误类型 |
| `src/smolagents/local_python_executor.py` | `ExecutionTimeoutError` | 超时专用错误 |
| `src/smolagents/local_python_executor.py` | `PrintContainer` | print 输出收集与截断载体 |

## 参考

- [smolagents 源码：local_python_executor.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/local_python_executor.py)
- [smolagents 文档：安全执行代码](https://huggingface.co/docs/smolagents/tutorials/secure_code_execution)
- [smolagents 文档：Code agents](https://huggingface.co/docs/smolagents/conceptual_guides/intro_agents)
- [Python 文档：ast 模块](https://docs.python.org/3/library/ast.html)

## 与主核 / AI 的关系

这套「AST 遍历求值 + 白名单裁剪」与主核里「表达式求值器」是同一种结构：主核要把 SQL 表达式编译成可求值的算子树，且不允许用户表达式触达任意宿主函数；这里要把 Python 源码变成可求值的节点树，且只允许触达白名单里的对象。两者都在做同一件事：**把一个通用语言的表达力裁剪成一个受控子集**。

差别在于目标不同。主核裁剪是为了「保证语义可预测、可优化、可移植」，这里裁剪是为了「限制模型生成的代码能造成多大影响」。前者必须覆盖完整语义（否则用户写不了查询），后者可以有意留缺口（不支持就去掉）。

`timeout` 那段注释值得所有做「带超时的用户代码执行」的人看一遍：它诚实地记录了「超时 ≠ 停止」这个常被忽略的事实。主核里对应的问题是「查询超时后后台仍在扫表」——两者都必须在文档层面说清「超时保证的是什么」，否则上层的资源配额会被后台残留任务悄悄突破。

对 agent 工程的实际结论：**本地解释器不是安全边界**。要跑不可信代码就必须上进程/容器/微虚机隔离（smolagents 用 `RemotePythonExecutor` 系列表达这个边界）。把本地解释器当成安全措施，是这类框架最常见的使用误判。
