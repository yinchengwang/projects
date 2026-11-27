---
project: metagpt
feature: code-to-tool-schema
title: 代码转工具 schema 的 AST 分析
page_type: feature
schema_version: 1
refs:
  - path: metagpt/tools/tool_convert.py
    symbols: [convert_code_to_tool_schema, convert_code_to_tool_schema_ast, CodeVisitor]
---

# 代码转工具 schema 的 AST 分析

## 一句话职责

把一段 Python 代码变成「模型能读懂的说明书」有两种手段：手里已有活对象时靠反射（`inspect`），只有一段源码文本时靠 AST 遍历（`CodeVisitor`），两者输出的 schema 结构一致。

## 机制是什么

schema 是工具与模型之间的唯一契约，形状固定成四种键：

| 键 | 含义 | 用途 |
|---|---|---|
| `type` | `function` / `async_function` / `class` | 告诉模型这是函数还是类 |
| `description` | docstring 的「Args:」之前那部分 | 给模型判断能不能用 |
| `signature` | 参数表与返回标注的文本形式 | 告诉模型怎么调 |
| `parameters` | docstring 里「Args:」之后的部分 | 参数语义的补充说明 |

类工具多一层 `methods`，键是方法名、值是上述四键结构；从 AST 路径产出的类与函数还会多一个 `code` 键，存原始源码片段（`register_tools_from_file` 会把这个键 `pop` 出去单独存，不留在 schema 里）。

两条路径的**输入不同**：`convert_code_to_tool_schema(obj)` 吃活对象，能拿到真实的函数签名；`convert_code_to_tool_schema_ast(code)` 吃字符串，只能从源码文本重建签名。前者精度高但要求代码已被 import，后者不执行代码但重写的签名逻辑必须自己维护。

## 怎么实现的

**反射路径的两个分支。** `convert_code_to_tool_schema` 用 `inspect.isclass` / `inspect.isfunction` 分流：

- 类是 `{"type": "class", "description": ..., "methods": {}}`，然后 `inspect.getmembers(obj, inspect.isfunction)` 遍历成员，跳过以 `_` 开头的（**`__init__` 例外，被保留**），若调用方给了 `include` 白名单则不在其中的也跳过，每个方法调 `function_docstring_to_schema`。
- 函数直接走 `function_docstring_to_schema`。

注意 `getmembers` 只认 `inspect.isfunction`，因此 `@staticmethod` 与 `@classmethod` 在这个筛选下行为不同：静态方法在 Python 3 里取出来仍是普通函数，会命中；类方法取出来是绑定方法，不是 `isfunction`，会被漏掉。

**类方法的 docstring 要沿 MRO 往上找。** `get_class_method_docstring` 遍历 `cls.__mro__`，在每个基类的 `__dict__` 里找同名方法并返回其 `__doc__`。这样「子类没覆写但继承了方法」时也能拿到注释。注意它只在**找到同名方法**时返回 docstring，若同名方法没有 docstring 就继续往下一个基类找——也就是说它会返回「最近一个有注释的同名实现」的注释，可能与实际执行的方法不是同一个。

**`function_docstring_to_schema` 的核心是 `inspect.signature`。** 它把签名 `str()` 化直接当 `signature` 字段，把 docstring 交给 `GoogleDocstringParser.parse` 切成整体描述与参数描述，再按 `inspect.iscoroutinefunction` 决定 `type` 是 `async_function` 还是 `function`。

`GoogleDocstringParser` 的实现极简：把连续空白压成单空格、去掉首尾空白，然后以 `"Args:"` 为界二分。没有 `Args:` 就整段当描述、参数部分为空。因此「Google 风格但用 `Arguments:` 或 `Params:`」的注释会被整段当成描述，参数说明丢失。

**AST 路径靠 `CodeVisitor` 三处 visit。** `convert_code_to_tool_schema_ast` 把源码 `ast.parse` 后交给 visitor，返回 `visitor.get_tool_schemas()`，即 `{name: schema}` 字典。

- `visit_ClassDef`：先取类 docstring 作描述（`ast.get_docstring` 已被 `remove_spaces` 处理），再遍历类体的直接子节点，只处理 `FunctionDef` 与 `AsyncFunctionDef`，同样跳过 `_` 开头（`__init__` 例外）。**只扫直接子节点**，不递归进嵌套类，也不会看到从基类继承来的方法。最后把类的源码片段存进 `code`。
- `visit_FunctionDef` / `visit_AsyncFunctionDef` 都转到 `_visit_function`：跳过名字以 `_` 开头的（这里**没有 `__init__` 例外**，因为模块级函数不存在这个概念），取出 schema 并附上源码片段。

**AST 侧的签名重建。** `_get_function_signature` 用一个确定的算法拼字符串：遍历 `node.args.args`，每项先取参数名，有注解就 `ast.unparse` 后拼 `: 注解`，在默认值表里就再拼 ` = 默认值`；最后有返回注解就拼 ` -> 注解`。

默认值表的构造是 `dict(zip(参数名列表[-len(defaults):], defaults))`——用「参数列表末尾与默认值列表对齐」的方式配平，这是 Python 语法本身的规则（有默认值的参数必须在末尾）。因此这个配对是正确的。

**AST 侧有三处与反射侧不等价。** 一，只处理 `node.args.args`，**忽略 `*args`、`**kwargs` 与仅关键字参数**（`kwonlyargs`）。二，不处理 `decorator_list`，所以静态方法与类方法在 AST 看来与普通方法相同。三，签名里不带类型的外部前缀（`ast.unparse` 输出的是源码里的字面形式，例如 `List[str]` 而不是 `typing.List[str]` 求值后的对象）。反射侧的 `str(inspect.signature(...))` 输出的是解析后的注解 `repr`，两者文本不同。

**`code` 键的位置差异。** AST 路径给类和函数都加了 `code`（`ast.get_source_segment`），反射路径不加。这个差异被上层吸收：`register_tools_from_file` 从 schema 里 `pop("code")` 出来放进 `Tool.code`；而装饰器路径直接把活对象交给 `inspect.getsource` 取源码。两条路最终都填满 `Tool.code`，但填充者不同。

## 关键取舍

**源码事实：** docstring 解析用 `"Args:"` 做单一切分点，且只支持这一种段落名。参数说明的文字原样保留（含 `Args:` 前缀），不做结构化拆解——模型看到的是「一段自然语言」而不是「参数名 → 说明」的字典。

**源码事实：** 类方法在反射路径上会被漏掉（`getmembers` 只匹配 `isfunction`），但在 AST 路径上不会（只看函数定义节点）。同一份代码走两条路会得到不同的 `methods` 集合。

**源码事实：** `get_class_method_docstring` 取的是「MRO 上第一个有注释的同名实现」，若子类覆写了方法但无注释，会返回基类的注释——描述与实现可能不符。

**源码事实：** AST 签名重建忽略仅关键字参数与可变参数。这类函数的 `signature` 字段会漏掉参数，模型据此调用就会出错。

**架构推论：** 保留两条路径而不合并，是因为它们的代价结构相反：反射需要 import（可能触发副作用、可能缺依赖），AST 不需要执行代码（可用于扫描用户上传的目录）。`register_tools_from_path` 选 AST 正是为了避免 import 任意文件。

**架构推论：** schema 里同时有 `signature`（文本）与 `parameters`（自然语言），没有真正的结构化参数表（名称、类型、是否必填）。这让 schema 保持「给模型读」的定位，也意味着任何需要程序化使用参数信息的地方（例如自动生成 JSON Schema 做函数调用）都必须自己再解析一遍。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/tools/tool_convert.py` | `convert_code_to_tool_schema` | 反射路径：活对象到 schema，类/函数分流 |
| `metagpt/tools/tool_convert.py` | `convert_code_to_tool_schema_ast` | AST 路径入口：解析源码并收集 schema |
| `metagpt/tools/tool_convert.py` | `CodeVisitor` | 遍历类与函数定义，产出 `{name: schema}` |
| `metagpt/tools/tool_convert.py` | `CodeVisitor.visit_ClassDef` | 只扫直接子节点的方法并附源码片段 |
| `metagpt/tools/tool_convert.py` | `CodeVisitor._get_function_signature` | 由 AST 重建参数表与返回标注 |
| `metagpt/tools/tool_convert.py` | `CodeVisitor._get_function_schemas` | 函数 docstring 与签名的合并 |
| `metagpt/tools/tool_convert.py` | `function_docstring_to_schema` | 反射侧的函数 schema 组装 |
| `metagpt/tools/tool_convert.py` | `get_class_method_docstring` | 沿 MRO 找方法注释 |
| `metagpt/utils/parse_docstring.py` | `GoogleDocstringParser` | 以 `Args:` 为界切分描述与参数 |

## 参考

- [MetaGPT 源码：tools/tool_convert.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/tools/tool_convert.py)
- [Python 文档：ast 模块与 NodeVisitor](https://docs.python.org/3/library/ast.html)
- [MetaGPT 文档：自定义工具](https://docs.deepwisdom.ai/main/en/guide/tutorials/customize_tool.html)

## 与主核 / AI 的关系

「同一份代码用两种手段提取元数据，输出同一结构」与主核里「DDL 解析」与「运行时目录缓存」是同一种双源问题：主核在建表时解析 SQL 文本得到表定义，在打开已存在的库时从目录读回同一份定义，两者必须产生等价结果。这里的两条路径没有强一致约束（没有 schema 版本号、没有校验），因此同一份代码经两条路得到不同 schema 是可接受的。

对 agent 工程的实际含义：写工具时 docstring 的格式不是风格问题而是功能问题。`Args:` 这个标记是唯一的分段依据，写错段落名就等于没写参数说明；同理，若函数的必需参数藏在仅关键字参数里，从文件路径注册的工具会得到一份缺参数的签名，模型据此生成的调用会失败。这也解释了为什么「给模型的工具描述」值得单独写、而不是靠注释自动抽取。

