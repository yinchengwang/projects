---
project: smolagents
feature: remote-executor-contract
title: 远程执行器契约
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/remote_executors.py
    symbols: [RemotePythonExecutor, run_code_raise_errors, _patch_final_answer_with_exception]
---

# 远程执行器契约

## 一句话职责

定义「真隔离」的统一接口：一套模板方法把「装依赖、送工具、送变量、跑代码、识别最终答案」串成固定流程，子类只需实现「在一个沙箱里跑一段代码并交出输出」这一件事。

## 机制是什么

`RemotePythonExecutor` 继承 `PythonExecutor`，是本地解释器的对位物。b01 那页已经写明本地解释器「不是安全沙箱」，这一层就是被指向的那个安全边界。

基类定义的生命周期是：

- 构造：记下 `additional_imports`、`logger`、`allow_pickle`，并把 `installed_packages` 置空，打一行「Initializing executor, hold on...」。
- `install_packages(additional_imports)`：默认实现就是往远端执行一条 `!pip install ...`，然后把装过的包返回（用于去重）。
- `send_tools(tools)`：先给 `final_answer` 打补丁，再装工具声明的依赖，最后把工具定义代码送进远端。
- `send_variables(variables)`：把变量序列化成带前缀的字符串，连同反序列化代码一起送进远端并 `locals().update(...)`。
- `__call__(code_action)`：直接转发到 `run_code_raise_errors`。

唯一必须实现的是 `run_code_raise_errors(code) -> CodeOutput`。所以「远程」的具体形态（E2B 沙箱、Docker 容器、Modal、Blaxel）完全由子类决定，而「怎么把准备工作送过去」是基类固定的。

`allow_pickle` 是个贯穿全层的安全开关，默认 `False`。docstring 在每一层都重复了同一段警告：pickle 反序列化可以执行任意代码，只在完全信任执行环境、且需要兼容自定义类型时才打开。

## 怎么实现的

**最终答案的异常通道（最精巧的一处）。** 远端的代码要能表达「我给出最终答案了」，而这不能靠返回值——因为代码的最后一行未必是答案，且必须能穿透任何用户写的 `try/except`。做法与本地解释器一致：抛异常。

但远程场景有一个本地没有的约束：`FinalAnswerTool` 的实现代码要先被**送到远端**才能运行，而送过去的是**源码文本**。所以 `_patch_final_answer_with_exception` 做了一次「类改造」：

1. 动态建一个继承原类的子类 `_FinalAnswerTool`。
2. 给它写一个新的 `forward`，内部定义了完整的 `_to_json_safe` 与 `_serialize_with_fallback`，最后 `raise FinalAnswerException(序列化后的值)`。
3. 把原 `forward` 改名成 `_forward` 挂在子类上，并把它的源码里 `def forward(` 替换成 `def _forward(` 后存进 `__source__`。
4. 把新 `forward` 的源码存进 `__source__`，并把闭包变量 `allow_pickle_setting` **文本替换成字面量**。
5. `final_answer_tool.__class__ = _FinalAnswerTool`——直接把实例的类换掉。

第 4 步的文本替换是关键：注释说明「源提取无法保留闭包」，所以 `ALLOW_PICKLE = allow_pickle_setting` 这一行必须在送出去之前被替换成 `ALLOW_PICKLE = False` 这样的字面量，否则远端拿到的源码里引用了一个不存在的变量。

第 3、4 步的 `__source__` 设置也不是装饰：工具系统的源码导出走 `get_source()`，它会**优先取 `obj.__source__`**（见 `utils.get_source` 的实现），取不到才走 `inspect.getsource`。所以这里是主动控制「送出去的源码长什么样」。

**序列化逻辑为什么要内联。** 注释写得很明确：这段序列化代码被内联进 `forward`，因为「这个方法本身的源码会被提取并送到远端环境，那里不存在外部引用」。所以本该调用 `SafeSerializer` 的地方，全部展开成局部函数。远端不需要有 `smolagents` 包，只要这段源码自足。

**返回值的前缀协议。** 序列化产物一律带前缀：`"safe:" + json.dumps(...)` 或 `"pickle:" + base64(pickle.dumps(...))`。接收侧 `_deserialize_final_answer` 按前缀分派：

- `safe:` → `json.loads` 后走 `SafeSerializer.from_json_safe`
- `pickle:` → `allow_pickle` 为假则**抛 `SerializationError`**，为真才 `pickle.loads`
- 其他 → 抛 `SerializationError("Unknown final answer format: expected 'safe:' or 'pickle:' prefix")`

注意第三条：没有前缀的输入被**拒绝**，而不是当成 legacy pickle 处理。这比 `SafeSerializer.loads` 更严格——那个版本对无前缀输入会在 `allow_pickle=True` 时按 legacy pickle 反序列化。远程最终答案这条路径选择了「只接受显式前缀」，因为这里的输入来自一个不完全可信的环境（沙箱里的代码可以自由构造这个字符串）。

**`send_tools` 的三步顺序。** 先给 `final_answer` 打补丁，再算要装哪些包，最后送工具定义。

依赖计算是一句集合推导：遍历所有工具的 `to_dict()["requirements"]`，排除掉已经装过的（`self.installed_packages + ["smolagents"]`）。所以「已装」的记账靠 `installed_packages` 这个列表累积，而 `install_packages` 的默认实现返回输入列表，形成闭环。

里面有一个特例：`if "PIL" in packages_to_install: discard("PIL"); add("pillow")`。因为工具声明的是**导入名**（`PIL`），而 pip 需要的是**包名**（`pillow`）。这类映射在 Python 生态里很常见且无法自动推导，只能硬编码。

**`send_variables` 的两段代码。** 它不直接传值，而是构造一段程序：

```python
<SafeSerializer.get_deserializer_code(allow_pickle)>
vars_dict = _deserialize(<序列化字符串的 repr>)
locals().update(vars_dict)
```

第一行是通过 `inspect.getsource` **从本地实现生成**的反序列化器源码（`get_deserializer_code` 会取 `from_json_safe` 的源码、改名成 `_from_json_safe`、按 `allow_pickle` 拼出 pickle 分支），第二三行调用它并塞进 `locals()`。

`locals().update(...)` 在模块顶层（即 `exec` 的全局命名空间）是有效的变量注入手段。这也意味着变量只能注入到顶层命名空间——函数内部的 local 与它不是同一件事。

**`get_deserializer_code` 的按需生成。** 它按 `allow_pickle` 拼不同的分支：为真时两条 pickle 分支都是 `import pickle; return pickle.loads(...)`；为假时两条都是 `raise SerializationError("Pickle data rejected: allow_pickle=False")`。

所以「安全策略」是**烘进送出去的代码里**的，而不是靠运行时配置。远端拿到的反序列化器要么有能力解 pickle，要么根本不含 pickle 代码路径。这个设计比「传一个 flag 给远端」更安全——远端代码无法通过修改 flag 绕过。

## 关键取舍

**源码事实：** `_patch_final_answer_with_exception` 通过 `final_answer_tool.__class__ = _FinalAnswerTool` 直接改实例的类。这是 Python 允许但少见的操作，副作用是原实例的所有状态保留、方法换成新的。它依赖「原类与新类是子类关系」这个前提。

**源码事实：** 内联序列化代码里 `ALLOW_PICKLE` 是通过**文本替换源码字符串**注入的，因为闭包无法随源码传递。这依赖源码里那一行的字面形式 `ALLOW_PICKLE = allow_pickle_setting` 保持不变——任何格式化改动都会让替换失效且不会报错。

**源码事实：** `_deserialize_final_answer` 拒绝无前缀输入，而 `SafeSerializer.loads` 接受无前缀输入作为 legacy pickle。同一份代码里两条路径的严格度不同，是刻意的：最终答案来自沙箱内部，不可信程度更高。

**源码事实：** `allow_pickle=False` 时的安全策略是「送出去的代码里根本没有 pickle 分支」，而不是运行时判断。

**源码事实：** 依赖安装默认走 `!pip install` 这条 IPython 语法，所以基类的默认实现假定远端是 Jupyter 内核。子类可以覆盖（Blaxel 就覆盖成了调用进程 API）。

**架构推论：** 模板方法模式在这里的作用是「把安全相关的准备工作全部收口到基类」。如果每个后端各自实现「送工具、送变量」，那么安全策略（只用安全序列化、把 final_answer 转成异常）就会有 N 份实现、N 种可能的不一致。收口之后，新增一个后端只需实现「在沙箱里跑代码」，安全性由基类保证。

**架构推论：** 「源码即载荷」是这套设计的核心约束，也是它最脆弱的地方。工具、补丁、反序列化器都以源码文本形式传输，所以任何依赖闭包、模块导入、或本地符号的实现细节都会在远端失效。这解释了为什么序列化逻辑要内联、为什么 `allow_pickle` 要文本替换——全是为了让源码在远端自足。

**架构推论：** 用异常表达最终答案，在远程场景比对本地更重要。远程执行的返回值是字符串化的结果，无法携带结构化对象；而异常可以把「值」挂在异常对象上（`FinalAnswerException(value)`），子类在 `execution.error` 里读到 `name` 与 `value` 就能还原。这是一条绕过「返回值必须是文本」限制的通道。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/remote_executors.py` | `RemotePythonExecutor` | 远程执行基类：模板方法与安全开关 |
| `src/smolagents/remote_executors.py` | `RemotePythonExecutor.run_code_raise_errors` | 子类必须实现的唯一方法 |
| `src/smolagents/remote_executors.py` | `RemotePythonExecutor.send_tools` | 打补丁、算依赖、送工具定义 |
| `src/smolagents/remote_executors.py` | `RemotePythonExecutor.send_variables` | 生成反序列化器并注入顶层命名空间 |
| `src/smolagents/remote_executors.py` | `RemotePythonExecutor.install_packages` | 默认用 `!pip install` 装依赖 |
| `src/smolagents/remote_executors.py` | `RemotePythonExecutor._patch_final_answer_with_exception` | 换类加改名，把 forward 变成抛异常 |
| `src/smolagents/remote_executors.py` | `RemotePythonExecutor._deserialize_final_answer` | 按前缀分派，无前缀直接拒绝 |
| `src/smolagents/remote_executors.py` | `FINAL_ANSWER_EXCEPTION` | 远端异常名常量，用于跨进程识别 |
| `src/smolagents/utils.py` | `get_source` | 优先取 `__source__` 的源码提取 |

## 参考

- [smolagents 源码：remote_executors.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/remote_executors.py)
- [smolagents 文档：安全执行代码](https://huggingface.co/docs/smolagents/tutorials/secure_code_execution)
- [E2B 文档：代码解释器沙箱](https://e2b.dev/docs)

## 与主核 / AI 的关系

主核里最接近的是「客户端/服务端的执行边界」：真正的工作发生在服务端进程里，客户端只负责把查询与参数送过去、把结果取回来。这里 `RemotePythonExecutor` 就是这个边界在 agent 侧的抽象——但它比数据库客户端多了一个难题：送过去的不是数据，而是**代码**。

与本地解释器（b01）对照，能清楚看到两种「限制执行」的路径差异：本地解释器靠**裁剪语言本身**（不支持某些语法就没有对应能力），远程执行器靠**隔离执行环境**（语言是完整的，但环境是沙箱）。前者的极限是「防误用」，后者的极限才是「防恶意」。b01 的文档字符串说的「需要真隔离就用远程执行器」，指的正是这条线。

与 CrewAI 的 MCP 原生工具相比（见 crewai 的 g02 页），两边的隔离手段完全不同。CrewAI 那边工具跑在**本进程**里，安全靠工具本身的实现与失败分类；smolagents 的远程执行器把整个代码执行搬进容器/微虚机。前者风险在「工具作者写了危险代码」，后者风险在「沙箱逃逸」。这也解释了为什么这边的序列化层（h02）要做成硬白名单，而 CrewAI 更关注失败分类。

对 AI 工程最实用的一条是「安全策略烘进送出去的代码」。传一个 `allow_pickle=False` 给远端，和送一段**不含 pickle 分支的**反序列化器，安全级别不同：前者依赖远端遵守约定，后者是结构上无法违反。凡是「安全相关的开关」要跨信任边界传递，都该考虑改成「按开关生成代码」而不是「把开关传过去」。

另一条是那个文本替换注入闭包常量的手法。它暴露了「源码即载荷」的真正代价：任何来自外部的值都必须变成源码里的字面量，而这只能靠字符串操作完成，既脆弱又无类型检查。若要做同类设计，更稳的方案是先把常量序列化进一个固定名字的字典（如 `_CONFIG = {...}`）再送源码，让源码只引用这个名字——这样格式调整不会破坏注入。
