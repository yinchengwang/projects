---
project: langchain
feature: callback-handlers
title: 内置处理器与流式输出
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/callbacks/streaming_stdout.py
    symbols: [StreamingStdOutCallbackHandler]
  - path: libs/core/langchain_core/callbacks/usage.py
    symbols: [UsageMetadataCallbackHandler]
  - path: libs/core/langchain_core/callbacks/file.py
    symbols: [FileCallbackHandler]
---

# 内置处理器与流式输出

## 一句话职责

这三个 handler 覆盖了最基础的三类需求——**把 token 实时打到屏幕上**、**把多模型的 token 用量累加起来**、**把整个运行过程写进一个文件**——而它们各自示范了一种「handler 该怎么写才对」的范式。

## 机制是什么

| handler | 触发的事件 | 状态 | 关键约束 |
|---|---|---|---|
| `StreamingStdOutCallbackHandler` | 只有 `on_llm_new_token` | 无 | 需要模型支持流式 |
| `UsageMetadataCallbackHandler` | 只有 `on_llm_end` | 一个按模型名累加的字典 + 一把锁 | 依赖 `AIMessage.usage_metadata` |
| `FileCallbackHandler` | 链/agent/工具的开始与结束 | 一个打开的文件 | 推荐用 `with` 语句 |

三者都是 `BaseCallbackHandler` 的子类，都只实现自己关心的事件，其余事件**留空或根本不定义**。

## 怎么实现的

### 一、流式：只写那一个方法

**`StreamingStdOutCallbackHandler` 的核心只有两行。**

```
sys.stdout.write(str(token))
sys.stdout.flush()
```

`flush()` 是必需的：没有它，输出会在缓冲区里攒到一定量才刷出，**流式就变成了一批一批的块**。这行代码是整个 handler 的存在意义所在。

**`str(token)` 这一层转换对应一个真实的类型变化。** 签名是 `token: str | list[str | dict[str, Any]]`——也就是说新式模型可能把「一个 token」表达成**内容块列表**而不是字符串。`str()` 让两种形态都能写出，代价是列表形态会被打印成 Python 的 repr。

这个处理是妥协而非最优：一个内容块列表里可能混着文本块与图片块，`str()` 会把它们一起打印出来。但对「往终端流式输出」这个用途来说，能把东西打出来比格式化得漂亮更重要。

**它显式地把 `on_llm_start`、`on_chat_model_start`、`on_llm_end`、`on_llm_error`、`on_chain_*`、`on_tool_*`、`on_agent_action` 全部定义成空方法。** 这些方法什么都不做，只保留了 `**kwargs`。

这个选择值得记：**在基类里它们本来就是可选的**（未实现即等同于不关心），显式写一堆空方法并不改变行为。但它改变了一件事——`__init__` 之后这些属性一定存在，于是 `getattr(handler, event_name)` 不会失配。也就是说这些空方法主要是**为可读性与调试服务的**：打开这个文件就能看到「这个 handler 关心哪些事件」，不必对照基类去推断。

**它的文档带一句警告**：`!!! warning "Only works with LLMs that support streaming."`。这句必须存在，因为一个不支持流式的模型**不会报错**——它只会在结束时一次性给出结果，而 `on_llm_new_token` 从不被调用，屏幕上什么都不出现。

### 二、用量：一把锁与一次 `add_usage`

**`UsageMetadataCallbackHandler` 的状态只有一个字典与一把锁。**

```
self._lock = threading.Lock()
self.usage_metadata: dict[str, UsageMetadata] = {}
```

字典的键是**模型名**、值是累计的 `UsageMetadata`。也就是说同一个 handler 可以跨多个模型、多次调用累加，输出是一张「每个模型花了多少 token」的表。

**锁的必要性来自并发**。异步路径里 handler 是 `gather` 并发执行的（见回调管理器那页），而一个同步 handler 在异步路径里会被丢进线程池——两个线程同时 `on_llm_end` 就是真实情形。所以「读-改-写」这三步必须在锁里：

```
with self._lock:
    if model_name not in self.usage_metadata:
        self.usage_metadata[model_name] = usage_metadata
    else:
        self.usage_metadata[model_name] = add_usage(...)
```

这是本页最值得记的一处：**一个看起来完全无害的累加器，在并发分发下必须有锁**。而锁的粒度取在整个分支上，不是只保护赋值——因为「先查再写」本身就是竞态。

**`add_usage` 是 `langchain_core.messages.ai` 里的函数，而不是本文件手写的。** 也就是说「怎么把两份用量相加」是一个共享的定义，handler 只是调用方。这避免了「某个 handler 加漏了 cache token」这类不一致。

**取的路径绕了三层 `isinstance` / `try`，因为用量可能根本不存在。**

```
try: generation = response.generations[0][0]
except IndexError: generation = None
...
if isinstance(generation, ChatGeneration):
    try:
        message = generation.message
        if isinstance(message, AIMessage):
            usage_metadata = message.usage_metadata
            model_name = message.response_metadata.get("model_name")
    except AttributeError: pass
```

四道守卫：`generations` 可能是空列表、`generation` 可能不是 `ChatGeneration`（可能是老式 `Generation`）、`generation.message` 可能不存在、`message` 可能不是 `AIMessage`。

**其中第二与第四道是同一个方向：用量信息只存在于新式聊天模型的 `AIMessage` 上。** 而 `usage_metadata` 本身的注释写着「langchain-core >= 0.2.2」——所以第四道守卫实际上是在挡「更老的版本或非标准实现」。

**最关键的一步是不报错也不猜。** 若 `usage_metadata` 或 `model_name` 任一为空，这个事件被**静默跳过**：

```
if usage_metadata and model_name:
    ...
```

两个条件同时要求是有理由的：没有 `usage_metadata` 就无从累加；**没有 `model_name` 就无法选择累加到哪个键**。而这里没有「用默认键兜底」——因为那会把不同模型的用量混进同一个桶，得到一张看似完整实则错误的表。

**`__repr__` 被重写成 `str(self.usage_metadata)`。** 于是 `print(cb)` 直接给出那张表。这是一个小的可用性选择：这个 handler 的唯一产出就是这个字典，让 `repr` 直接呈现它比标准的对象表示有用得多。

**`get_usage_metadata_callback` 是一个上下文管理器，且它的 `finally` 里有一个具体的 bug 修复注释。**

```
finally:
    # Always clear the context var, including when the with-block raises.
    # Without finally, post-block model calls keep accumulating into cb
    # (see #38989).
    usage_metadata_callback_var.reset(token)
```

注释点明两件事：**用 `finally` 而不是正常路径**，因为 with 块抛异常时若不复位，那个 `ContextVar` 会一直指向这个 handler；后果是**退出 with 块之后的模型调用仍然会累加进来**，于是下一段代码的用量被污染。

这是一个「上下文变量必须在 `finally` 里复位」的标准案例，而且它附带了一个真实 issue 编号——说明这条曾经是 bug。

**它在构造时 `register_configure_hook(usage_metadata_callback_var, inheritable=True)`。** `inheritable=True` 意味着子运行会继承这个 contextvar，所以嵌套的链调用里每一层都会把用量汇报给同一个 handler。

而注意一个细节：**这个 `ContextVar` 是在函数体内部创建的**，不是模块级常量。于是每次调用 `get_usage_metadata_callback` 都会得到一个新的 `ContextVar` 对象。好处是多次调用彼此隔离；代价是无法在别处按名字取到同一个变量。

### 三、文件：上下文管理器是推荐路径，直接实例化被废弃

**`FileCallbackHandler` 是本页唯一一个有真实资源管理问题的 handler，而它的实现承认了两个时代。**

文档把两种用法并列写出：

| 用法 | 生命周期 | 状态 |
|---|---|---|
| `with FileCallbackHandler(...) as handler:` | `__enter__` / `__exit__` 管住 | **推荐** |
| `handler = FileCallbackHandler(...)` | 靠 `__del__` 或显式 `close()` | **已废弃** |

**文件在 `__init__` 里就被打开了**（`Path(self.filename).open(self.mode, encoding="utf-8")`），所以 `__enter__` 只做一件事：把一个标志位置真。

```
def __enter__(self):
    self._file_opened_in_context = True
    return self
```

**`_file_opened_in_context` 这个标志的用途在 `_write` 里揭晓。**

```
global _GLOBAL_DEPRECATION_WARNED
if not self._file_opened_in_context and not _GLOBAL_DEPRECATION_WARNED:
    warn_deprecated(since="0.3.67", pending=True, message=(...))
    _GLOBAL_DEPRECATION_WARNED = True
```

两个条件：没在上下文里用，以及**全局只警告一次**。

那个模块级 `_GLOBAL_DEPRECATION_WARNED` 表明作者考虑过「一个循环里创建几十个 handler 会刷几十条警告」。这是废弃警告应有的做法——**它应当提示一次，而不是淹没日志**。

而 `pending=True` 说明这是「将来会变成错误」的预告，不是当前的错误。

**`_write` 在文件不可用时抛 `RuntimeError` 而不是静默丢弃。**

```
if not hasattr(self, "file") or self.file is None or self.file.closed:
    raise RuntimeError("File is not open. Use FileCallbackHandler as a context manager.")
```

三条件守卫（属性不存在 / 为 `None` / 已关闭）是因为 `__del__` 可能在 `__init__` 失败后也被调用，此时 `self.file` 根本没被赋值。

错误消息直接给出做法（"Use ... as a context manager"）。这比「file is closed」有用得多。

**`close()` 是幂等的，且它自己也做了三重检查。**

```
if hasattr(self, "file") and self.file and not self.file.closed:
    self.file.close()
```

于是 `__exit__` 与 `__del__` 都调它也不会重复关闭——这在 Python 里是必要的，因为 `__del__` 的调用时机不确定，`with` 块结束时文件已经被关掉了，`__del__` 之后还会再调一次。

**`on_chain_start` 的链名解析是一条三级回退。**

```
kwargs.get("name") or serialized.get("name", serialized.get("id", ["<unknown>"])[-1]) or "<unknown>"
```

优先用事件参数里的 `name`，其次用序列化描述里的 `name`，再其次用 `id` 列表的**最后一个元素**（这是类名），最后兜底 `"<unknown>"`。

**那个 `id[-1]` 是 langchain 序列化格式的约定**：`id` 是一个从模块路径到类名的列表，最后一个才是类名。同样的三级回退在 `StdOutCallbackHandler` 里也出现，且写法几乎相同——两个 handler 都用了这套，说明它是这个代码库里的既有模式，而不是一处随意的选择。

**写文件的默认模式是追加 `"a"`，且显式 `encoding="utf-8"`。** 追加是有道理的：一次运行写一个 trace 文件，多次运行不该覆盖历史。而 `encoding` 显式指定避免了平台默认编码带来的差异（Windows 上默认可能是 cp1252，写中文会出错）。

## 关键取舍

**源码事实：** `StreamingStdOutCallbackHandler` 的核心只有 `write` + `flush` 两步，`flush` 是流式可用的前提；token 类型是 `str | list[str | dict]`，用 `str()` 统一写出。

**源码事实：** 它把十几个事件方法显式定义为空方法，不改变行为，只让「关心什么」在文件内可见。

**源码事实：** `UsageMetadataCallbackHandler` 用一把 `threading.Lock` 保护「查-加-写」三步，因为异步分发会让它被并发调用。

**源码事实：** 累加用共享的 `add_usage` 而不是本文件手写；`usage_metadata` 或 `model_name` 任一缺失就静默跳过，**不用默认键兜底**。

**源码事实：** `get_usage_metadata_callback` 的 `ContextVar` 是函数内创建的，且必须返回 `inheritable=True`；`finally` 里的 `reset` 附带了「不复位会让退出后的调用继续累加」这个 bug 的 issue 编号。

**源码事实：** `FileCallbackHandler` 在 `__init__` 就打开文件，用 `_file_opened_in_context` 标志区分「上下文用法」与「已废弃的直接实例化」。

**源码事实：** 废弃警告由模块级 `_GLOBAL_DEPRECATION_WARNED` 控制为**全局只提示一次**，且 `pending=True`（预告而非错误）。

**源码事实：** `_write` 在文件不可用时抛 `RuntimeError` 且消息里给出正确用法；`close()` 幂等，`__del__` 与 `__exit__` 都会调它。

**源码事实：** 两个 handler 共用同一套「链名三级回退」，其中 `serialized["id"][-1]` 取类名是序列化格式的约定。

**架构推论：** 三个 handler 恰好示范了 handler 写作的三条基线：**无状态的（流式）只需要那一个方法**、**有状态的要为并发做准备**、**持有资源的要提供上下文管理器**。

而两个「带状态」的差别很有意思：`UsageMetadataCallbackHandler` 的状态是纯内存的，加锁就够了；`FileCallbackHandler` 的状态是外部资源，锁解决不了问题，必须靠生命周期管理。**这也是为什么后者是三个里唯一被废弃了某种用法的**——资源管理一旦依赖 `__del__`，就注定不可靠。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/callbacks/streaming_stdout.py` | `StreamingStdOutCallbackHandler` | 只实现 `on_llm_new_token`，write + flush |
| `libs/core/langchain_core/callbacks/usage.py` | `UsageMetadataCallbackHandler` | 按模型名累加 `usage_metadata`，带锁 |
| `libs/core/langchain_core/callbacks/usage.py` | `UsageMetadataCallbackHandler.on_llm_end` | 四道 `isinstance`/`try` 守卫，缺任一关键项就跳过 |
| `libs/core/langchain_core/callbacks/usage.py` | `get_usage_metadata_callback` | 上下文管理器，`inheritable` 的 contextvar + `finally` 复位 |
| `libs/core/langchain_core/callbacks/file.py` | `FileCallbackHandler` | 写文件的 handler，推荐上下文用法 |
| `libs/core/langchain_core/callbacks/file.py` | `FileCallbackHandler.__enter__` / `_file_opened_in_context` | 只置标志，文件在构造时就已打开 |
| `libs/core/langchain_core/callbacks/file.py` | `FileCallbackHandler._write` | 全局只警告一次的废弃提示 + 不可用时抛 `RuntimeError` |
| `libs/core/langchain_core/callbacks/file.py` | `FileCallbackHandler.close` | 幂等关闭，`__exit__` 与 `__del__` 共用 |
| `libs/core/langchain_core/callbacks/file.py` | `_GLOBAL_DEPRECATION_WARNED` | 模块级「只提示一次」开关 |
| `libs/core/langchain_core/callbacks/stdout.py` | `StdOutCallbackHandler` | 非流式版，与文件版共用链名三级回退 |

## 参考

- [langchain 源码：callbacks/streaming_stdout.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/callbacks/streaming_stdout.py)
- [langchain 源码：callbacks/usage.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/callbacks/usage.py)
- [langchain 文档：流式输出](https://python.langchain.com/docs/how_to/streaming/)
- [Python 文档：contextlib 与上下文管理器](https://docs.python.org/3/library/contextlib.html)

## 与主核 / AI 的关系

这三个 handler 与主核里的「日志器 / 统计收集器 / 审计写入器」几乎逐条对应：数据库也有一个「只往 stdout 吐进度」的轻量观察者、一个「统计每次查询的行数与耗时」的累加器、一个「把整个语句过程写进审计文件」的写入器。三者面临的资源与并发问题完全一样。

对 agent 工程的直接价值有三条。其一，**累加器在并发分发下必须有锁，且粒度要盖住「先查后写」**。`UsageMetadataCallbackHandler` 只做加法，但它仍然是竞态的——这类「看起来无害」的共享状态最容易被漏掉。其二，**缺失的关键信息要跳过，不要兜底**。缺 `model_name` 时若用默认键，会得到一张看似完整实则把用量混在一起的表；静默跳过至少让人看出「这个模型没被统计」。其三，**持有外部资源的 handler 只应有一种可靠用法，并把另一种明确废弃**。`FileCallbackHandler` 允许直接实例化（靠 `__del__` 关闭）是个历史包袱，而它的处理方式是：保留兼容、只警告一次、消息里给出正确写法——这比静默容忍或直接破坏兼容都好。
