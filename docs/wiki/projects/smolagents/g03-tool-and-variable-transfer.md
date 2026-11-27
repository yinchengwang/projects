---
project: smolagents
feature: tool-and-variable-transfer
title: 工具与变量跨进程传递
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/remote_executors.py
    symbols: [send_tools, send_variables, install_packages]
---

# 工具与变量跨进程传递

## 一句话职责

把本地定义的工具与变量送进远端沙箱：工具靠「源码 + 依赖 + 显式记账」三件套，变量靠「安全序列化 + 按开关生成的反序列化器 + 注入顶层命名空间」，两者都以代码文本为载体跨越进程边界。

## 机制是什么

三个方法构成一条准备链，顺序固定：

1. `install_packages(additional_imports)`——装运行期依赖，返回装成功的包名列表。
2. `send_tools(tools)`——打补丁、装工具声明的依赖、送工具定义源码。
3. `send_variables(variables)`——把变量序列化后注入远端命名空间。

前两步在构造期（子类 `__init__` 里调 `install_packages`）或首次使用时发生；第三步在每次运行前按需调用。

三者共享一个核心约束：**能跨过去的只有文本**。工具靠 `inspect.getsource`（或对象自带的 `__source__`）转成源码，变量靠 `SafeSerializer` 转成带前缀的字符串。没有任何对象引用、没有 pickle 之外的对象传输机制。

## 怎么实现的

**工具依赖的三个来源。** `send_tools` 里的依赖集合是这么算出来的：

```python
{ pkg for tool in tools.values() for pkg in tool.to_dict()["requirements"] if pkg not in self.installed_packages + ["smolagents"] }
```

所以依赖来自**每个工具自己声明的 `requirements`**，而不是扫描源码里的 import。这解释了 C 组工具页里 `to_dict` 为什么要带 `requirements` 字段——它为的就是这一层的远程传输。

`smolagents` 被固定排除：远端环境不需要这个包本身，因为工具基类的源码会被单独送出，不依赖远端装包。

**「已装」的记账闭环。** `installed_packages` 是实例上的列表，初值来自构造期的 `install_packages(additional_imports)` 返回值。基类 `install_packages` 的实现是：非空则执行 `!pip install ...`，然后**无条件返回传入的列表**。

所以「是否装成功」并不真的被检查——`!pip install` 的输出被记进日志，但返回值总是输入。这意味着若某个包安装失败，它仍会被记入 `installed_packages`，后续 `send_tools` 不会再尝试。这是一个「信任 pip 会成功」的假设，失败后果是远端执行时 `ImportError`。

`BlaxelExecutor` 覆盖了这个方法并做了退出码检查（失败返回空列表），所以它反而会在下次重试——两条路径在失败记账上正好相反。

**`PIL` 到 `pillow` 的硬编码映射。** 依赖集合里若出现 `"PIL"`，会被换成 `"pillow"`。原因是工具作者写的是**导入名**（`from PIL import Image`），而 pip 要的是**发行名**。这类不一致在 Python 生态里无法自动推导（`opencv-python` 与 `cv2` 同理），只能逐个硬编码。

代码里只有这一条特例，说明它是在实际使用中撞出来的，而不是系统性解决。

**工具定义代码的生成。** 实际送出去的内容来自 `get_tools_definition_code(tools)`（在 `tools.py`，不在本页范围）。它产出的是每个工具的类定义源码，由 `Tool.to_dict()` 与工具系统的源码导出机制配合完成。所以远端拿到的是「可以 `exec` 的类定义文本」，执行后就得到可调用的工具对象。

`send_tools` 拿到代码后调 `run_code_raise_errors(code)` 执行，并把 `code_output.logs` 打出来。注意它**不检查 `output` 或失败标志**——工具定义代码执行失败的唯一表现是后续调用时找不到工具。

**`send_variables` 的三段代码。** 它构造的不是数据，而是一段程序：

```python
<反序列化器源码>
vars_dict = _deserialize(<序列化字符串字面量>)
locals().update(vars_dict)
```

第一段由 `SafeSerializer.get_deserializer_code(allow_pickle)` 生成，第二段把序列化结果作为 `repr` 字面量嵌入，第三段注入命名空间。

`locals().update(...)` 在模块顶层有效——`exec` 在全局命名空间执行时 `locals()` 就是那个全局字典。所以变量确实进了命名空间，后续每次执行都能看到它们。这也是远程执行器能像 REPL 一样工作的原因。

**反序列化器的按需生成。** `get_deserializer_code` 是这一层最巧的设计：它用 `inspect.getsource(SafeSerializer.from_json_safe)` 拿到**本地的真实实现源码**，做两次文本处理（去掉 `@staticmethod` 装饰器行、把 `SafeSerializer.from_json_safe` 换成 `_from_json_safe`），拼进一个完整的模块文本。

然后按 `allow_pickle` 拼 pickle 分支：为真时两条分支都是 `import pickle; return pickle.loads(...)`，为假时两条都是 `raise SerializationError("Pickle data rejected: allow_pickle=False")`。

关键在于：**`allow_pickle=False` 时，送出去的代码里根本不存在 pickle 调用**。这不依赖远端遵守约定。

**变量序列化的前缀。** `SafeSerializer.dumps` 产出 `"safe:" + json` 或 `"pickle:" + base64`。`allow_pickle=False` 时只有前者可能产生——`to_json_safe` 遇到无法安全序列化的类型会抛 `SerializationError`，不会降级。

`allow_pickle=True` 时先试安全序列化，失败才降级到 pickle，且降级前发一条 `FutureWarning` 说明「这是安全风险，未来版本会移除」。

**`repr` 嵌入的安全边界。** 序列化字符串被 `repr()` 后嵌入源码。这是安全的：Python 的字符串 `repr` 会正确转义引号、反斜杠与非打印字符，所以外部内容不会改变代码结构。这里没有使用 f-string 拼引号这类危险做法。

## 关键取舍

**源码事实：** 依赖来自工具的 `requirements` 声明，不是源码扫描。所以一个工具若在运行时 `import` 了未声明的包，远端会 `ImportError`。

**源码事实：** 基类 `install_packages` 无条件把输入当作已装返回，不检查 `!pip install` 是否成功。所以安装失败是静默的，只留日志。

**源码事实：** `BlaxelExecutor` 的覆盖版会在失败时返回空列表，导致下次重装——与基类行为相反。

**源码事实：** `PIL` → `pillow` 是唯一的硬编码导入名到包名映射。

**源码事实：** `allow_pickle=False` 时，反序列化器源码里不含 pickle 代码路径，安全策略是结构性的而非约定性的。

**源码事实：** 变量注入用 `locals().update(...)`，只在顶层命名空间有效。若远端执行被包在一个函数里，这个更新不会影响外层。

**架构推论：** 「工具靠源码、变量靠序列化」是两种不同性质的信息流。工具是**行为**，必须以可执行代码的形式过去（因为闭包与对象引用无法传输）；变量是**数据**，可以序列化。这也解释了为什么工具传输要求「源码可提取」——用 `__source__` 支持动态定义的类（见 `utils.get_source`），正是为了解决「`inspect.getsource` 对某些对象不可用」这个限制。

**架构推论：** 把 `allow_pickle` 烘进生成的代码，而不是作为运行时参数传过去，是「不可信环境下的开关传递」的正确做法。传参数意味着远端可以在解析前改掉它；生成代码意味着「没有那条路径」。任何安全开关要跨信任边界时，这个模式都适用。

**架构推论：** `installed_packages` 这个列表承担的是「幂等记账」而不是「真实状态」。它对同一次会话内的重复发送有效（避免反复 pip install），但对跨会话、或安装失败的情况无保护。要更可靠，需要让 `install_packages` 真的验证导入可用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/remote_executors.py` | `send_tools` | 打补丁、算依赖、送工具定义源码 |
| `src/smolagents/remote_executors.py` | `send_variables` | 生成反序列化器并注入顶层命名空间 |
| `src/smolagents/remote_executors.py` | `install_packages` | 基类用 `!pip install` 装依赖并记账 |
| `src/smolagents/serialization.py` | `SafeSerializer.dumps` | 按 `allow_pickle` 产出带前缀的序列化字符串 |
| `src/smolagents/serialization.py` | `SafeSerializer.get_deserializer_code` | 从真实实现生成远端可用的反序列化器源码 |
| `src/smolagents/serialization.py` | `SafeSerializer.get_safe_serializer_code` | 生成完整的序列化器类源码，供远端使用 |
| `src/smolagents/utils.py` | `get_source` | 优先取 `__source__` 的源码提取 |

## 参考

- [smolagents 源码：remote_executors.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/remote_executors.py)
- [smolagents 源码：serialization.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/serialization.py)
- [smolagents 文档：安全执行代码](https://huggingface.co/docs/smolagents/tutorials/secure_code_execution)

## 与主核 / AI 的关系

主核里最接近的是「计划序列化与跨节点传输」：并行执行要把计划片段送到工作进程，那里只能传数据不能传函数指针，所以必须把「做什么」编码成可重建的描述。这里 `send_tools` 做的是同一件事，但送的不是计划描述而是**完整的类定义源码**——因为 Python 的工具是带方法的对象，无法用声明式描述重建。

与 CrewAI 的 MCP 原生工具（crewai 的 g02 页）对照，两种「让工具在别处可用」的路径截然不同：CrewAI 那边的工具**始终在本地进程**执行，远端只是被调用的服务；smolagents 这里的工具**本身被搬到了远端**。前者的安全边界在「服务端能做什么」，后者的安全边界在「沙箱能否被逃逸」。

对 AI 工程最实用的一条是「安全开关烘进生成的代码」。这是 `send_variables` 那套设计的精髓：与其传一个 `allow_pickle` 参数让远端自己判断，不如按参数生成两份不同的代码，让不安全的分支在文本层面就不存在。任何「跨信任边界的配置项」都可以用这个思路——配置不再是数据，而是代码生成的选择器。

另一条是那个 `PIL` → `pillow` 的映射。它提醒我们「依赖声明」和「实际导入」在 Python 里不是同一套名字，而远程传输必须以 pip 能识别的名字出现。做跨进程工具传输时，最好在工具定义层面就强制声明「pip 名」而不是「导入名」，或者维护一张显式映射表——硬编码单个特例的写法在第三个同类问题出现时就会开始失控。
