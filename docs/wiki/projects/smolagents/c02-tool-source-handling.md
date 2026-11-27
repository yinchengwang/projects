---
project: smolagents
feature: tool-source-handling
title: 工具源码与序列化往返
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/tools.py
    symbols: [to_dict, from_dict, from_code]
  - path: src/smolagents/tools.py
    symbols: [from_hub, from_langchain]
---

# 工具源码与序列化往返

## 一句话职责

工具的唯一持久形态是 **Python 源码**：`to_dict` 把一个工具实例反向生成为可直接 `exec` 的类定义代码（顺带算出依赖清单），`from_dict` / `from_code` / `from_hub` 再把它加载回来——因为没有 schema 能承载工具的行为，只有源码能。

## 机制是什么

多数框架序列化工具时序列化的是**元数据**（名字、描述、参数 schema），执行时靠注册表找回到进程里那个函数。smolagents 走另一条路：序列化**行为本身**。

原因是它的使用场景里工具必须跨进程。远程执行器（E2B、Docker、Modal 等）在另一个进程/容器里跑代码，那边没有用户的 Python 对象，只有送过去的源码。于是工具必须能被写成源码、发过去、在那边重建。这也解释了为什么 `to_dict` 返回的字典里 `code` 是主角，`requirements` 是从代码里反推出来的。

往返有三个入口，对应三种来源：

- `to_dict` / `from_dict`：源码字符串与字典之间的对称转换。
- `from_code`：直接从一段源码字符串构造实例（`from_dict` 内部就调它）。
- `from_hub`：从 Hub 上的 Space 仓库下载 `tool.py` 再走 `from_code`；`from_langchain` 则是把外部对象就地包一层，压根不产生源码。

## 怎么实现的

**`to_dict` 有两条完全不同的生成路径，按工具怎么被创建分。**

第一条是 `@tool` 装饰器创建的（内部类名恰好是 `SimpleTool`）。这种情况只需要拿 `forward` 一个函数的源码，所以做法是：

1. `get_source(self.forward)`，去掉 `@tool` 装饰器行，`ast.parse` 后交给 `MethodChecker(set())` 检查（类属性集为空，因为此时还没有类体）。有错就抛 `ValueError`，逐条列出 `MethodChecker` 的 errors。这是一处**序列化前的预检**：能存就说明代码自包含。
2. 拼一个最小的类骨架：`class SimpleTool(Tool)` 加 `name` / `description` / `inputs` / `output_type`（有 `output_schema` 也带上），全部用 `json.dumps` / `repr` 字面量化。
3. 把 `forward` 源码改名（`self.name` → `"forward"`）并用正则 `def forward\(((?!self)[^)]*)\)` 补上 `self` 参数，再缩进四空格贴进类体。

第二条是「子类化 `Tool`」创建的。这时要生成整个类的源码，于是先 `validate_tool_attributes(self.__class__)` 把它过一遍静态校验（见 d02），再用 `instance_to_source(self, base_cls=Tool)` 生成。`instance_to_source` 的规则是：只输出**与基类不同**的类属性与方法（用 `co_code` 比对字节码判断方法是否被覆写），多行字符串用 `"""` 包裹并转义内部三引号。这条「只写差异」的策略是必要的——否则每个工具的源码都会拖着一份 `Tool` 的全部实现。

有一条硬拒绝：`SpaceToolWrapper` / `LangChainToolWrapper` / `GradioToolWrapper` 三种包装器**不允许**导出，错误文案直接说「会制造错误」。它们的 `forward` 是外部对象的方法或闭包，拿不到有意义的源码。

**依赖清单是反推出来的。** `get_imports(tool_code)` 扫源码里的 import 语句，减去 `sys.stdlib_module_names`，再并上 `smolagents` 本身。所以 `requirements` 不是作者声明的，是**从生成的代码里读出来的**——这保证了「代码与依赖清单永远同步」，代价是动态导入（`importlib.import_module("x")`）不会被发现。

`to_dict` 的返回结构就是三（或四）个键：`name`、`code`、`requirements`（已排序），有 `output_schema` 时再加一个。

**`from_code` 是往返闭合的那一步。** 实现只有几行，但每行都有理由：

1. 造一个空模块 `types.ModuleType("dynamic_tool")`，`exec(tool_code, module.__dict__)`。
2. 用 `inspect.getmembers(module, inspect.isclass)` 找第一个 `issubclass(obj, Tool)` 且不是 `Tool` 自身的类。
3. 找不到就 `ValueError("No Tool subclass found in the code.")`。
4. 两个补救：若 `tool_class.inputs` 不是 dict（可能是源码里写成字符串），用 `ast.literal_eval` 还原；`output_schema` 同理。
5. `return tool_class(**kwargs)`。

第 4 步值得单说：它说明这批代码曾经（或可能）把 `inputs` 存成字符串形式，加载时靠 `literal_eval` 转回来。用 `literal_eval` 而非 `eval` 是有意的——前者只能解析字面量，不能执行任意代码。这是这条 `exec` 链上唯一的收紧点。

注意 `exec` 本身没有沙箱：`from_code` 会真的执行传入的源码。所以 `from_hub` 强制要求 `trust_remote_code=True`，否则直接 `ValueError`，并在 docstring 里用醒目提示写「加载工具就是下载并本地执行它，务必像对待 pip 包一样先审阅」。`from_langchain` / `from_space` 不设这道门槛，因为它们接收的是**已经构造好的对象**（`langchain_tool` / Space 客户端），源码从未跨进程——信任判断发生了在调用方拿到那个对象之前。

**`from_dict` 是「字典 → 实例」的薄壳。** 校验 `code` 键存在，调 `from_code`，再把 `output_schema` 补回去（它不在源码里，因为生成的类骨架只在 `@tool` 路径上带它）。

**Hub 路径多了两步 IO。** `from_hub` 用 `hf_hub_download(repo_id, "tool.py", repo_type="space", ...)` 取文件，读成文本再 `Tool.from_code`。它把 kwargs 分两类：`cache_dir` / `revision` / `subfolder` / `local_files_only` / `force_download` / `proxies` 等给下载层，其余传给工具构造函数。

**反方向还有 `save` / `push_to_hub`。** `save` 写三个文件：`{tool_file_name}.py`（即 `to_dict()["code"]`）、`app.py`（一个调 `launch_gradio_demo` 的极小启动脚本）、`requirements.txt`（`to_dict()["requirements"]` 换行拼起来）。`push_to_hub` 把它们作为三个 `CommitOperationAdd` 提交到 `repo_type="space"` 的仓库，并打上 `smolagents` 与 `tool` 两个 tag。

**`from_langchain` 是唯一不产生源码的入口，代价也随之出现。** 它构造一个内联的 `LangChainToolWrapper`：名字取 `langchain_tool.name.lower()`，`inputs` 直接抄 `langchain_tool.args`（顺手删掉 `title` 键、把 `description` 清空），`output_type` 硬编码 `"string"`，`forward` 里把位置参数按下标塞进字典再调 `langchain_tool.run`。它设了 `skip_forward_signature_validation = True`，因为 `forward` 是 `*args, **kwargs`。这个包装器**无法导出**（见上文硬拒绝），所以「包进来用」可以，「存下来复用」不行。

看 `forward` 那段能发现一处会静默出错的地方：循环 `for index, argument in enumerate(args)` 里，每次都用 `next(iter(self.inputs))` 取键——也就是**永远取第一个输入名**，而不是与 `index` 对应的那个。所以传两个以上位置参数时，它们会被依次覆盖到同一个键上，后面的覆盖前面的。

## 关键取舍

**源码事实：** 工具序列化的产物是可直接 `exec` 的类定义源码，不是元数据。`requirements` 由源码反推，不靠声明。

**源码事实：** `from_code` 用 `exec` 执行传入代码，无沙箱。`trust_remote_code=True` 这道门槛只设在「源码要从远端下载」的入口上（`from_hub`、`ToolCollection.from_hub`、`ToolCollection.from_mcp`），`from_code` / `from_dict` 本身不检查——调用方传入的源码被视为已被信任。

**源码事实：** 两个补救点用 `ast.literal_eval` 而非 `eval`：`inputs` 与 `output_schema` 若是字符串形态，按字面量还原。

**源码事实：** `@tool` 装饰器路径与子类路径生成源码的方式完全不同——前者抠出 `forward` 函数体拼骨架，后者靠 `instance_to_source` 导出差异。前者还对 `forward` 源码先跑 `MethodChecker` 预检。

**源码事实：** 三种外部包装器（Space / LangChain / Gradio）禁止导出，错误文案明确写「会产生错误」。

**架构推论：** 用源码作为序列化格式，等于承认「工具的行为无法用数据描述」。收益是跨进程、跨机器、跨容器都能精确复现；代价是**序列化即执行**，所以安全模型只能靠信任门槛（`trust_remote_code`）而不是靠沙箱或验证。

**架构推论：** `to_dict` 的两条路径暴露了一个真实约束：`@tool` 装饰器创建的类是被动态拼出来的（`SimpleTool` 只存在于运行时），它的 `__module__` 与源码位置都不可靠，所以只能从 `forward` 这个原始函数反推。这也解释了为什么这条路径需要 `add_self_argument` 这样的补丁——`forward` 源自一个不像方法的普通函数，必须补上 `self` 才能变成方法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/tools.py` | `Tool.to_dict` | 生成 `{name, code, requirements[, output_schema]}`，两条源码生成路径 |
| `src/smolagents/tools.py` | `Tool.from_dict` | 读 `code` 走 `from_code`，补回 `output_schema` |
| `src/smolagents/tools.py` | `Tool.from_code` | `exec` 源码到临时模块，找 `Tool` 子类并实例化，字面量还原补救 |
| `src/smolagents/tools.py` | `Tool.from_hub` | 下载 Space 的 `tool.py` 后走 `from_code`，强制 `trust_remote_code` |
| `src/smolagents/tools.py` | `Tool.from_langchain` | 内联包装 LangChain 工具，跳过签名校验，不可导出 |
| `src/smolagents/tools.py` | `Tool.save` / `push_to_hub` | 落盘三个文件 / 提交到 Hub Space |
| `src/smolagents/tools.py` | `add_self_argument` | 给 `forward` 源码补 `self` 参数的正则补丁 |
| `src/smolagents/tool_validation.py` | `MethodChecker` | 序列化前的自包含性预检（`@tool` 路径） |
| `src/smolagents/utils.py` | `instance_to_source` | 导出实例所属类中与基类不同的属性与方法 |

## 参考

- [smolagents 源码：tools.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/tools.py)
- [smolagents 文档：把工具分享到 Hub](https://huggingface.co/docs/smolagents/tutorials/tools#share-your-tool-on-the-hub)
- [smolagents 文档：从 Hub 导入工具](https://huggingface.co/docs/smolagents/tutorials/tools#import-a-tool-from-the-hub)
- [Python 文档：`ast.literal_eval`](https://docs.python.org/3/library/ast.html#ast.literal_eval)

## 与主核 / AI 的关系

「把可执行的定义当作数据存下来」这件事，主核里对应的是**存储过程 / 视图定义**：两者都需要把一个可执行体持久化，且都需要在恢复时能重建出等价行为。差别在验证：主核保存的是 SQL 文本，重建时由解析器重新解析并绑定到目录，任何引用失效会在创建/解析期暴露；smolagents 保存的是 Python 源码，重建时直接 `exec`，因此**只能靠信任而不是靠验证**。这是两种语言在「可安全重新解析」上的根本差异，不是设计取舍。

`requirements` 由源码反推这一点，与主核里「视图依赖表」的推导同源：都是从定义文本中提取外部引用，以便在装载前准备好运行环境。但主核依赖图是精确的（解析器知道每个名字指向哪个对象），这里只能做到模块级别——`get_imports` 认不出一个未 import 却通过全局可用的名字。

对 AI 工程的实际意义在于：**工具分发的难点不是格式，而是信任**。这套实现给出的解法是把源码当作唯一格式，并把信任决策压缩成一个显式布尔开关（`trust_remote_code`），再在文档里用两处醒目警告重复说明。这比假装「有校验所以安全」更诚实。另一个可借鉴的实践是 `to_dict` 里的序列化前预检（`MethodChecker` + `validate_tool_attributes`）：**能存就说明自包含**，把「存下来才发现跑不起来」提前到保存时刻。最后，把不可导出的类型显式列成黑名单并给出错误文案，比让它们导出后在加载时报奇怪的 `NameError` 要好得多。
