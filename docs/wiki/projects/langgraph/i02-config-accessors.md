---
project: langgraph
feature: config-accessors
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/config.py
    symbols: [get_config, get_store, get_stream_writer]
---

# 配置、存储与流写入器取用

## 一句话职责

三个无参函数让节点体在不改签名的前提下拿到当前运行的配置、长期存储与自定义流写入器，靠的是 langchain-core 的 child runnable config contextvar。

## 机制是什么

`get_config()` 从 `var_child_runnable_config` 里读当前配置。这个 contextvar 是 langchain-core 的机制：Runnable 在进入 `invoke`/`stream` 时把它设好，子调用继承它。拿不到就抛 `RuntimeError("Called get_config outside of a runnable context")`——不是返回空字典，因为「没有上下文」和「上下文是空的」是两件事。

`get_store()` 与 `get_stream_writer()` 都是在 `get_config()` 之上再走一层：从 `config["configurable"][CONFIG_KEY_RUNTIME]` 取出 Runtime，再分别读 `.store` 与 `.stream_writer`。所以这两个函数其实是 Runtime 的语法糖，和 i01 是同一套机制的两个入口。

## 怎么实现的

**Python 3.10 的兜底。** `get_config()` 开头有一段只在 3.10 下执行的检查：如果当前有 event loop 就跑一个 `asyncio.current_task()`，若存在任务就抛 `RuntimeError("Python 3.11 or later required to use this in an async context")`。原因是 3.11 之前 `asyncio.create_task` 不复制 contextvar，异步路径下取不到配置。糟糕的是这段代码用 `try/except RuntimeError: pass` 包着——自己抛的 `RuntimeError` 会被自己的 except 吞掉。也就是说这段检查实际上从不生效，是个已知的死代码。3.11+ 下整段被 `sys.version_info` 短路掉。

**`get_config` 的实现只有三行。** 拿 contextvar，有就返回，没有就抛。`if var_config := ...` 这种写法同时完成取值与真值判断——空字典也会走 else 分支，但因为配置永远至少含 `configurable`，实际不会误判。

**`get_store` 的文档比代码长得多。** 它的 docstring 用两个完整示例分别演示 StateGraph 与 functional API 的用法，并各自附上 `pycon` 输出。函数体是一行：`get_config()[CONF][CONFIG_KEY_RUNTIME].store`。这种「文档重、代码轻」的比例说明它的价值在于「让用户知道什么时候能用」，而不是实现复杂度。

它明确警告：Python 3.11 以下在异步路径下 `get_store()` 不可用，因为它依赖 contextvar 传播。同一个限制也写在 `get_stream_writer()` 的 docstring 里。

**`get_stream_writer` 同样是一行。** 取 Runtime 的 `stream_writer`。这个 writer 在没人请求 `custom` 流时是 `_no_op_stream_writer`（一个什么都不做的函数，定义在本文件与 `runtime.py` 各有一份），而不是 `None`。所以节点可以无条件调用它，不必判断是否启用。

**为什么不是 `Optional`。** 这是刻意的：`StreamWriter` 的类型就是 `Callable[[Any], None]`，注入时永远是可调用对象。若返回 `None`，每个调用点都得写 `if writer:`，而绝大多数调用点在编译期就知道自己要不要写。用 no-op 换掉了这层判断。

**`_no_op_stream_writer` 有两份定义。** `config.py` 一份，`runtime.py` 一份。这是模块间不能互相导入时的重复（`runtime.py` 导入 `config.get_config`，反向导入会成环）。两份都是同名同签名的空函数，语义上等价。

## 关键取舍

**源码事实：** 三个函数全部无参。没有「传一个 config 进来」的版本，也没有「传 schema 做类型收窄」的版本（`get_runtime` 倒是接受一个可选的 `context_schema` 用于类型提示）。这意味着取配置这件事只能发生在执行上下文内部——单测里直接调节点函数会拿到 `RuntimeError` 而不是静默的空值。

**源码事实：** `get_store` 与 `get_stream_writer` 直接索引 `[CONF][CONFIG_KEY_RUNTIME]`，没有 `KeyError` 的兜底转换。所以「在 runnable 上下文里但没有 runtime」会抛一个裸 `KeyError`，而不是一个解释性的错误。这条路径只有在手工构造配置时才会走到。

**源码事实：** 3.10 的异步检查是坏的（自己抛的异常被自己吞）。这不算设计取舍，但是读代码时必须有的事实认知：不能指望它拦住 3.10 下的异步误用。

**架构推论：** 三个访问器共用一条取数链（contextvar → config → configurable → runtime → 字段），因此「可用性」这件事只有一处判定：是否在 runnable 上下文里。这使节点体可以放心调用而不做防御式判断，代价是脱离上下文就必然失败——而这正是想要的：一个在上下文外被调用的节点本来就是 bug。

**架构推论：** 把 store 与 writer 都塞进 Runtime 而不是各自一个 contextvar，减少了要维护的传播通道数量。多一个 contextvar 就多一份「哪些异步路径会丢」的风险矩阵；共用一条链，只需保证这一条。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/config.py` | `get_config` | 从 contextvar 取当前 RunnableConfig，缺上下文即抛 |
| `libs/langgraph/langgraph/config.py` | `get_store` | 经配置取 Runtime，返回其中的长期存储 |
| `libs/langgraph/langgraph/config.py` | `get_stream_writer` | 经配置取 Runtime，返回自定义流写入函数 |
| `libs/langgraph/langgraph/config.py` | `_no_op_stream_writer` | 未启用 custom 流时的空实现 |
| `libs/langgraph/langgraph/runtime.py` | `get_runtime` | 同一取数链上取整个 Runtime |
| `libs/langgraph/langgraph/runtime.py` | `Runtime.stream_writer` | 写入函数的承载字段，默认为 no-op |

## 参考

- [LangGraph 源码：config.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/config.py)
- [LangGraph 源码：runtime.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/runtime.py)
- [LangChain 文档：RunnableConfig 与配置传播](https://python.langchain.com/docs/concepts/runnables/)

## 与主核 / AI 的关系

「靠上下文隐式传递、而不是显式参数」与主核里的线程局部执行上下文（当前事务、当前连接）是同一种做法。主核这么做是为了不必给每个内部函数增加参数，这里的原因相同：节点签名是用户写的，框架不能要求他们多接三个参数。

代价也相同：隐式上下文在跨线程、跨协程、跨进程边界时会丢。主核对这一点用显式 xid 传参来兜底；LangGraph 的兜底是 Python 3.11 的 contextvar 自动传播，以及 `Token` 式的显式复位。

对 AI 侧的实际用法：工具与节点里想往流里写进度就用 `get_stream_writer()`，想读长期记忆就用 `get_store()`。两者都不要缓存到全局变量里——它们绑定的是当前这次运行，缓存会导致下一次运行读到上一次的 store。
