---
project: autogen
feature: function-tool
title: 函数工具与参数校验
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/tools/_function_tool.py
    symbols: [FunctionTool, run]
---

# 函数工具与参数校验

## 一句话职责

把一个普通 Python 函数包装成工具：构造期用签名推断参数模型，调用期把模型给的字典参数填回函数实参，并处理「同步还是异步」「支持不支持取消」这两种正交差异。

## 机制是什么

`FunctionTool` 把 `BaseTool[BaseModel, BaseModel]` 具体化，关键决策全在 `__init__` 里一次做完：

- 用 `get_typed_signature(func)` 拿到求值后的签名；
- 用 `args_base_model_from_signature(func_name + "args", signature)` 动态造一个 pydantic 模型当参数类型；
- 判断 `"cancellation_token" in signature.parameters` 得到 `_has_cancellation_support`；
- 返回类型直接取 `self._signature.return_annotation`（`BaseTool` 构造时再剥 `Annotated`）。

于是「函数 → 工具」的转换是无损的：函数名、描述、参数类型、返回类型全部保留，`schema` 属性由参数模型自动推出。

函数名有个小坑：`name or func.func.__name__ if isinstance(func, functools.partial) else name or func.__name__`。Python 的三元表达式优先级让这句读起来别扭但行为正确——对 `functools.partial` 取 `func.func.__name__`，否则取 `func.__name__`，且显式 `name` 始终优先。

## 怎么实现的

**`run` 的实参装配。** 先遍历 `self._signature.parameters`，对每个名字 `if hasattr(args, name)` 就 `kwargs[name] = getattr(args, name)`。之所以用 `hasattr` 而不是直接取，是因为参数模型里不包含 `cancellation_token`（`args_base_model_from_signature` 主动跳过了它），而签名里有——用 `hasattr` 过滤比显式排除名单更稳。

**四个分支。** 两个维度交叉出四条路径：

| | 支持取消 | 不支持取消 |
|---|---|---|
| 协程函数 | `await func(**kwargs, cancellation_token=token)` | `await func(**kwargs)` |
| 普通函数 | `run_in_executor(None, partial(func, **kwargs, cancellation_token=token))`，**不 link future** | `run_in_executor(None, partial(func, **kwargs))`，再 `token.link_future(future)` |

注意「同步 + 支持取消」这一格：它把 token 作为关键字参数传进线程，但**没有 `link_future`**，于是取消请求不会中断正在跑的线程——函数必须自己轮询 `token.is_cancelled()`。而「同步 + 不支持取消」这一格反而 link 了 future，取消会尝试 `future.cancel()`（对已开始的 executor 任务通常无效，只是让 `await` 提前抛 `CancelledError`）。这两格的差异是本实现里最容易误判的地方。

**阻塞调用一律进线程池。** 同步函数走 `asyncio.get_event_loop().run_in_executor(None, ...)`，即默认 `ThreadPoolExecutor`。这是为了让同步函数不阻塞事件循环——考虑到整个 actor 运行时共用一条事件循环，这一点是必须的，不是优化。

**配置往返是一次源码往返。** `_to_config` 产出 `FunctionToolConfig`：`source_code`（`dedent(to_code(self._func))`，即 `inspect.getsource` 去掉装饰器与公共缩进）、`name`、`description`、`global_imports`、`has_cancellation_support`。**函数体以源码文本形式进入配置**，而不是引用路径。

`_from_config` 的还原过程分三拍：先发一条醒目的安全警告（`UserWarning`，文案明说「会执行代码，只加载可信来源」）；再逐条 `exec(import_to_str(import_stmt), exec_globals)` 建立导入环境，`ModuleNotFoundError`/`ImportError`/其他异常被分别包成更具体的异常；最后 `exec(config.source_code, exec_globals)`，用 `config.source_code.split("def ")[1].split("(")[0]` 从源码文本里**字符串切出函数名**，再 `exec_globals[func_name]` 取函数并断言 `callable`。

注意还原时**没有把 `has_cancellation_support` 用上**——它只是被记录在配置里，加载时新的 `FunctionTool` 会重新从签名判断。所以这个字段是描述性的，不参与行为。

## 关键取舍

**源码事实：** `_from_config` 用 `split("def ")` 从源码文本里抓函数名。这是一种脆弱的解析：源码里若在函数定义前出现字符串 `"def "`（注释、docstring 里的示例），抓到的就是错的。之所以能勉强工作，是因为 `to_code` 已剥掉装饰器，源码首行必然是 `def <name>(...`。

**源码事实：** 配置里存的是**函数源码**，因此 `_func` 是闭包、依赖模块级私有变量、或定义在类方法里时，dump 出来的源码无法独立执行。构造函数不禁止这类函数，失败要到 load 时才暴露（`ValueError("Could not compile and load function")`）。

**源码事实：** `global_imports` 的默认值是可变对象 `[]`（`Sequence[Import] = []`）。因为只被读、不被改，实践中安全，但仍是可变默认参数的用法。

**源码事实：** 「同步 + 取消」分支不 `link_future`。这不是遗漏，而是因为 `link_future` 只会取消 asyncio 层的 Future，无法中断已在执行的线程；真正的取消必须由函数自己检查 token。代价是这条路径上取消是**协作式**的，函数不配合就一直跑到结束。

**源码事实：** 四个分支里只有「同步 + 不支持取消」会 `link_future`——看似反直觉，实则因为这一格没有任何其他取消入口（函数拿不到 token），link 是唯一能让 `await` 提前退出的手段。

**架构推论：** 参数校验发生在 `BaseTool.run_json` 里的 `model_validate`，而 `run` 只负责搬运属性。所以 `FunctionTool` 从不自己做校验，它把「校验」与「执行」彻底分开，且校验模型与 schema 来自同一个 `args_model`——这是 f01 里说的「两套并行的类型转换共享同一来源」的具体落点。

**架构推论：** 函数体作为源码进配置，等于把「工具」变成了**可自包含迁移**的单元：一份配置带着实现走，不需要目标环境有同名模块。这对 Studio 这类「导出再导入」的场景是必需的，代价是配置即代码执行，安全边界只能靠「只加载可信来源」这句警告。

**架构推论：** `run` 直接把返回值原样返回，类型不做校验（类 docstring 明说「验证工具输出类型是用户的责任」）。对比 `BaseStreamTool.run_json_stream` 会 `isinstance` 校验末尾元素——**流式有校验、非流式没有**，这是两处不一致。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/tools/_function_tool.py` | `FunctionTool` | 函数包装成具体工具类 |
| `python/packages/autogen-core/src/autogen_core/tools/_function_tool.py` | `FunctionTool.__init__` | 签名推断参数模型、判定取消支持、确定函数名 |
| `python/packages/autogen-core/src/autogen_core/tools/_function_tool.py` | `FunctionTool.run` | 四条分支：协程/同步 × 取消/不取消 |
| `python/packages/autogen-core/src/autogen_core/tools/_function_tool.py` | `FunctionTool._to_config` | 把函数源码、导入、描述打成配置 |
| `python/packages/autogen-core/src/autogen_core/tools/_function_tool.py` | `FunctionTool._from_config` | 执行导入与源码，重建工具，带安全警告 |
| `python/packages/autogen-core/src/autogen_core/tools/_function_tool.py` | `FunctionToolConfig` | 五个字段：源码、名字、描述、导入、取消支持标记 |

## 参考

- [AutoGen 源码：_function_tool.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/tools/_function_tool.py)
- [AutoGen 文档：Tools](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/tools.html)
- [Python 文档：asyncio 事件循环与 run_in_executor](https://docs.python.org/3/library/asyncio-eventloop.html)

## 与主核 / AI 的关系

主核里最接近的类比是「注册 UDF / 回调」：把用户代码挂进引擎，引擎负责在不同上下文中调用它（有锁、无锁、并行 worker）。差别在于主核的回调是**已编译、已链接**的，引擎知道它的签名与副作用；`FunctionTool` 面对的是运行期才拿到注解、且可能带闭包的 Python 函数，所以它把签名内省放在构造期、把校验放在调用期，用 pydantic 这一层补齐缺失的类型信息。

对 AI 工程最要紧的一条是线程模型。整个单线程运行时的「并发」靠协程交错，任何同步工具都会占住线程池里的一个线程；线程池默认上限是 `min(32, cpu+4)`，于是**同时挂起多个同步工具调用就会排队**，而排队时间会被记在工具的 span 里，看起来像「工具变慢」。写成 `async def` 并真正 `await` I/O 是唯一能拿到并发的方式。

第二条是取消语义。很多工具会在取消时收到 token 却继续跑完——工程师往往以为取消是抢占式的。实际语义是：协程分支靠 `await` 抛出，同步分支靠函数自己轮询。设计「长跑工具」时必须明确选哪条，否则上层超时后线程还在空转。

第三条是配置往返的边界。把函数源码 dump 进配置意味着「工具可迁移」，但只在函数是**模块级、无闭包、依赖已声明在 `global_imports` 里**时成立。跨环境传递工具配置前先做一次 `dump` 再 `load` 的自检，比在远端踩 `Could not compile and load function` 便宜得多。
