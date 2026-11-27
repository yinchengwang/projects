---
project: metagpt
feature: tool-registry
title: 工具注册表与 schema 生成
page_type: feature
schema_version: 1
refs:
  - path: metagpt/tools/tool_registry.py
    symbols: [ToolRegistry, register_tool, make_schema]
  - path: metagpt/tools/tool_registry.py
    symbols: [register_tools_from_path, get_tools_by_tag]
---

# 工具注册表与 schema 生成

## 一句话职责

工具注册表把一个 Python 函数或类登记成「带名称、路径、源码、schema、标签」的 `Tool` 记录，让 LLM 能在提示词里看到工具说明，也让运行期能按名字或标签把工具取出来。

## 机制是什么

注册是一个**双向喂料**的过程：函数与类的签名、docstring 是给模型看的说明书，而源码本身是可被执行的实现。注册表把两者绑在一起：

- `Tool.name` 是工具名（默认取类名或函数名）；
- `Tool.path` 是它所在文件的相对路径（模型按这个路径 import）；
- `Tool.schemas` 是从 docstring 提炼出的结构化说明（描述、签名、参数说明、方法表）；
- `Tool.code` 是源码文本；
- `Tool.tags` 是分类标签。

两张表：`tools` 按名字索引，`tools_by_tags` 是「标签 → {名字: Tool}」的两层字典。任何一次注册都会同时写这两张表。

`ToolRegistry` 是 pydantic 模型，因此注册结果可以序列化；模块末尾创建 `TOOL_REGISTRY` 单例供全局使用。

## 怎么实现的

**`ToolRegistry.register_tool` 的五个步骤。**

1. `if self.has_tool(tool_name): return`——同名工具只登记一次，重复注册静默跳过（不更新、不报错）。这意味着「改了实现再注册」不会生效。
2. `schema_path` 缺省为 `TOOL_SCHEMA_PATH / f"{tool_name}.yml"`。注意这只是**算出路径**，注册流程并不写这个文件；源码注释提示它「可用于校验」。
3. 没给 `schemas` 就调 `make_schema(tool_source_object, include_functions, schema_path)` 现生成。`make_schema` 内部调 `convert_code_to_tool_schema`，并把任何异常吞成「返回空 schema + 记一条 error 日志」。
4. schema 为空则直接返回——**没有 schema 的工具不登记**。这是硬门槛：docstring 解析不出来就意味着模型看不到它，登记了也没用。
5. `schemas["tool_path"] = tool_path` 把代码路径写回 schema，再尝试 `ToolSchema(**schemas)` 校验。校验失败被 `except: pass` 吞掉，注释说「不遵循格式但仍会被使用」。`ToolSchema` 本身只有一个必填的 `description` 字段，所以这个校验只拦「连描述都没有」的 schema。

最后构造 `Tool` 并写入两张表，可选打印 verbose 日志。

**装饰器把「文件路径」与「源码」自动补齐。** 模块级 `register_tool(tags, schema_path, **kwargs)` 返回类装饰器，它做三件事：

- `inspect.getfile(cls)` 取定义文件。若路径里含 `"metagpt"`，就用 `"metagpt" + 最后一个 "metagpt" 之后的部分` 归一——注释解释了原因：实际路径可能是 `../metagpt/metagpt/tools/...`，而需要的只是 `metagpt/tools/...`。
- `inspect.getsource(cls)` 取源码，异常的（例如交互式定义的类）用 `contextlib.suppress(OSError)` 吞掉，留空字符串。
- 以 `cls.__name__` 为工具名调 `TOOL_REGISTRY.register_tool`。

**`validate_tool_names` 是能力的统一入口。** 它接受一个字符串列表，每一项可以是三种东西，用「先看文件、再看名字、再看标签」的顺序判定：

- 是文件或目录 → 调 `register_tools_from_path` 现场注册，把结果并入；
- 是已注册的工具名（取 `:` 前的部分）→ 支持 `Editor:read,write` 这种「只取类工具的部分方法」写法：把原工具的 schema 深拷贝一份，`methods` 只保留指定且存在的方法，不存在的只记 warning；
- 是已注册的标签 → 把该标签下所有工具并入；
- 都不是 → 记 warning 跳过。

返回的是 `dict[name, Tool]`，调用方拿到的已经是「可用的工具集合」。这个方法被工具推荐器与 `ToolRecommender` 的字段校验共用，因此「用户配了哪些工具」与「运行时能调哪些工具」走同一套解析。

**从路径批量注册。** `register_tools_from_path` 分文件与目录两种情况：文件直接处理，目录用 `os.walk` 遍历所有文件逐个处理。`register_tools_from_file` 有三条过滤：文件名必须以 `.py` 结尾、不能是 `setup.py`、不能以 `test` 开头。然后读全文、用 AST 解析出 schema 字典（见代码转 schema 那一页），对每个工具把 `code` 字段从 schema 里 `pop` 出来单独存进 `Tool.code`，再注册。

这个 `pop` 很关键：`code` 是给运行期用的源码文本，不该留在给模型看的 schema 里，否则提示词会把整段实现塞进去。

**标签查询。** `get_tools_by_tag(key)` 返回 `self.tools_by_tags.get(key, {})`——未命中返回空字典而不是 `None`。`has_tool_tag` 判标签存在，`get_tool_tags` 返回全部标签。这三个方法构不成「原子」的一组：`has_tool_tag` 为真时标签下至少有零个工具是可能的（若注册时 `tags=[]` 则不会建标签，所以实际上只有「注册时带了标签」才会出现键，每个键至少有一个工具）。

## 关键取舍

**源码事实：** 同名工具重复注册直接跳过，不更新 schema 也不更新源码。这使注册表是「首次写入生效」的，热更新需要手动改 `TOOL_REGISTRY.tools`。

**源码事实：** schema 生成失败时工具**不注册**，而不是带着空 schema 注册。这让「模型看不到的工具」彻底不存在，避免调用时才发现没有说明。

**源码事实：** `ToolSchema` 校验失败被静默吞掉（`except: pass` 且日志被注释掉）。因此「注册成功」不等于「schema 合规」，只有 `description` 缺失才会被这个校验发现，而它恰好是唯一必填字段。

**源码事实：** 路径归一是按「最后一个 `metagpt` 之后的部分」拼接，遇到路径里出现多次 `metagpt` 的嵌套结构依然正确，但遇到不含 `metagpt` 的路径（例如用户自己目录下的工具）就原样保留绝对路径。

**架构推论：** 把「文件路径 + 源码」一起登记，使工具不必被注册表 import 就能被使用——模型按 `path` 自己 import。这避免了「注册时执行用户代码」的风险，代价是同一份代码在进程里可能被 import 两次（一次为取源码与签名，一次为真正调用）。

**架构推论：** 标签是纯人工分类，与函数语义无关。因此「按标签召回」的质量完全取决于标注质量。这也是后来加 BM25 / 向量召回（工具推荐那一页）的动因：标签太粗，无法表达「这个工具大概能干什么」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/tools/tool_registry.py` | `ToolRegistry` | 名字表与标签表两张索引 |
| `metagpt/tools/tool_registry.py` | `ToolRegistry.register_tool` | 重复跳过、schema 硬门槛、双表写入 |
| `metagpt/tools/tool_registry.py` | `register_tool` | 类装饰器：自动补齐路径与源码 |
| `metagpt/tools/tool_registry.py` | `make_schema` | 生成 schema 并把异常吞成空 schema |
| `metagpt/tools/tool_registry.py` | `validate_tool_names` | 名字 / 标签 / 路径三合一的解析入口 |
| `metagpt/tools/tool_registry.py` | `register_tools_from_path` | 文件或目录递归注册 |
| `metagpt/tools/tool_registry.py` | `register_tools_from_file` | AST 解析并按工具拆分出源码 |
| `metagpt/tools/tool_registry.py` | `get_tools_by_tag` | 标签查询，未命中返回空字典 |
| `metagpt/tools/tool_data_type.py` | `Tool` | 工具记录：name/path/schemas/code/tags |

## 参考

- [MetaGPT 源码：tools/tool_registry.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/tools/tool_registry.py)
- [MetaGPT 文档：工具与工具注册](https://docs.deepwisdom.ai/main/en/guide/tutorials/customize_tool.html)
- [MetaGPT 源码：tools/tool_data_type.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/tools/tool_data_type.py)

## 与主核 / AI 的关系

「同时登记可执行实现与可展示说明」与主核里「表既有物理布局又有目录元信息」是同一种双面性：主核的表需要目录项（给优化器与用户看）与物理布局（给存储引擎用），两者必须一致；这里需要 schema（给模型看）与源码路径（给运行期用），两者由同一次注册保证配对。差别在主核的一致性由 DDL 保证，这里靠「注册时一次性抓取」。

对 agent 工程而言，这一页解释了工具集为什么能「按需组合」：`validate_tool_names` 让配置里可以混写工具名、标签、甚至一个目录路径，解析结果统一是 `Tool` 字典。也因此，把工具目录写给 agent 就等于把那个目录下所有非测试的 `.py` 里所有公开函数与类都注册成工具——便利，但暴露面完全由目录内容决定。

