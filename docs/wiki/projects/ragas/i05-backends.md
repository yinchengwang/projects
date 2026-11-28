---
project: ragas
feature: backends
title: 数据集后端
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/backends/base.py
    symbols: [BaseBackend]
  - path: src/ragas/backends/registry.py
    symbols: [BackendRegistry, get_registry, register_backend]
---

# 数据集后端

## 一句话职责

把「数据集与实验存在哪」抽象成六个方法的接口，再用一个**单例注册表**按名字解引用实现类——实现通过 setuptools entry point 自动发现，所以「装一个带后端的包」就等于「多一个可用后端」，不需要改 ragas 的代码。

## 机制是什么

两层：

| 层 | 载体 | 职责 |
|---|---|---|
| 契约 | `BaseBackend` | 六个抽象方法：`load_dataset`/`save_dataset`/`list_datasets` 与 `load_experiment`/`save_experiment`/`list_experiments` |
| 发现 | `BackendRegistry` | 单例；`dict` 风格访问；按 entry point 组 `ragas.backends` 自动加载 |

契约的设计要点是**数据集与实验用完全相同的六个方法、但分开存储**。`BaseBackend` 的类 docstring 把这条写成硬要求：「Handle datasets and experiments with same interface but separate storage」——所以「实验」不是「带元数据的数据集」，而是**同类对象的另一个命名空间**。这在使用侧体现为 `Dataset` 与 `Experiment` 两个类共用同一套后端。

数据形态被钉死成 `List[Dict[str, Any]]`：读返回字典列表、写接收字典列表。空集合必须返回 `[]` 而不是 `None`（docstring 连写三遍），缺失必须抛 `FileNotFoundError`。这两条把「不存在」与「存在但为空」分开——是这套接口里最容易做错的语义。

## 怎么实现的

**实现通过 entry point 自动注册，配置写在 `pyproject.toml`。** 组名是 `ragas.backends`，内置四条：

```
"local/csv"  → ragas.backends.local_csv:LocalCSVBackend
"local/jsonl" → ragas.backends.local_jsonl:LocalJSONLBackend
"inmemory"   → ragas.backends.inmemory:InMemoryBackend
"gdrive"     → ragas.backends.gdrive_backend:GDriveBackend
```

于是「名字里的斜杠」（`local/csv`）不是路径语义，只是一个字符串标识；使用侧 `Dataset("my_data", "local/csv", root_dir="./data")` 会把 `root_dir` 传给构造函数。

**注册表是单例，且靠 `__new__` 而不是模块级对象去重。** `_instance` 类变量 + `__new__` 覆写；`_backends`/`_aliases`/`_discovered` 也都是**类变量**（不是实例变量），所以即使有人绕过单例直接 `BackendRegistry()`，看到的也是同一份注册表。模块底部再建 `BACKEND_REGISTRY = BackendRegistry()` 并暴露 `get_registry()`。

**发现是惰性的，且用一个 `_discovered` 布尔做一次性的闸门。** 每个读接口（`__getitem__`、`__contains__`、`__iter__`、`__len__`、`keys`、`values`、`items`、`list_all_names`、`list_backend_info`）开头都是同一段：

```
if not self._discovered:
    self.discover_backends()
```

`discover_backends()` 再判一次 `_discovered`，调 `_discover_backends()`，置真，打一条 info 日志（「Discovered N backends from entry points.」）。

**`_discover_backends` 兼容两代 `importlib.metadata` API。** `metadata.entry_points()` 在 3.10+ 返回带 `.select()` 的对象，3.9 返回 dict；代码用 `hasattr(entry_points_result, "select")` 分流，取 `group="ragas.backends"` 的条目。**单个 entry point 加载失败只记 warning 并继续**（`self[entry_point.name] = entry_point.load()` 被 try/except 包住）；整个 `entry_points()` 调用失败也只记 debug 日志。所以「一个坏插件不会拖垮整个注册表」——但也意味着**装坏的插件在运行期表现为「后端不见了」而不是报错**。

**注册与校验在 `__setitem__` 收口。** `_validate_name` 要求非空字符串（**只查非空**，不查格式，尽管 docstring 建议用字母数字/连字符/下划线）；`_validate_backend_class` 用 `issubclass(..., BaseBackend)` 校验继承，不通过抛 `TypeError`。合法就写进 `_backends`。

**别名是独立的一层映射，指向主名。** `_aliases: Dict[str, str]`；`_resolve_name` 先查别名再回落原名；`__getitem__` 用解析后的名字查。`register_aliases` 校验「主名必须已注册」（否则 `KeyError`）、跳过非法别名（warning）、已存在且未 `overwrite` 时跳过（warning）。

**删除有两条路径，语义不同。** `__delitem__` 先看是不是别名——是就只删别名；否则要求主名存在（不在就 `KeyError`），删主名**并连带删掉所有指向它的别名**。这个「级联清理」是必要的，否则会留下悬空别名指向不存在的后端。

**唯一的实例化入口是 `create_backend`。** `backend_class = self[backend_type]` 后 `return backend_class(**kwargs)`——不做参数校验、不缓存实例。所以每次调用都新建一个后端对象；对文件型后端这无所谓（无状态），对需要连接的实现则意味着「每次拿 Dataset 都重连」——除非调用方自己持有实例并直接传进去（接口同时支持字符串与实例，见 `Dataset._resolve_backend`）。

**使用侧有两条入口，解析逻辑在 `Dataset._resolve_backend`。** 传字符串时取 `registry[backend]` 拿类、再 `backend_class(**kwargs)`；把 `KeyError` 翻成 `ValueError`（附带可用后端列表与「装个插件」的提示），把构造期的 `TypeError` 翻成「检查必填参数」的 `TypeError`，其它异常翻成 `RuntimeError`。传实例时只用 `isinstance(backend, BaseBackend)` 校验。**三条异常翻译是这套设计里对用户最友好的部分**：字符串写错、参数给错、构造失败三种情况的提示完全不同。

**内置实现各自代表一种取舍，值得对照：**

| 实现 | 存储 | 特点 |
|---|---|---|
| `InMemoryBackend` | 两个 dict | 读写都 `deepcopy`，实例间完全隔离；`data_model` 被忽略 |
| `LocalCSVBackend` | `root_dir/datasets/*.csv` | 用 `csv.DictReader/DictWriter`，**所有值退化成字符串**；空数据写成空文件 |
| `LocalJSONLBackend` | 同上目录结构 | 逐行 JSON，额外做了 datetime 的序列化/反序列化 |
| `GDriveBackend` | Google Sheets | 可选依赖，`try/except ImportError` 后定义**桩类**（`type("Request", (), {})`、`build` 抛 `ImportError`） |

`InMemoryBackend` 的 `deepcopy` 是「防手滑」的显式成本：`load` 返回副本，调用方改了不会污染存储；`save` 也存副本，调用方后续改原数据不会悄悄改到存储里。两个方向都做了，是这类内存实现里少见的完整做法。

`LocalCSVBackend` 的字段名取自 `data[0].keys()`——**第一行的键决定表头**，后续行多出或缺少字段都不会报错（`DictWriter` 默认容忍）。写空数据时只创建一个空文件，于是「存在但为空」与「不存在」在文件系统层面可区分（前者文件存在但无表头，`DictReader` 返回空列表）。

`GDriveBackend` 的桩类模式是为了**类型检查能过**：模块级 `try/except ImportError` 里若什么都不定义，后面 `class GDriveBackend(BaseBackend)` 里用到 `Credentials` 等名字会 NameError（只在运行期）。定义桩类后类体可以正常构造，真正调用时由 `build(*args, **kwargs)` 抛 `ImportError`。`backends/__init__.py` 也据此设 `GDRIVE_AVAILABLE` 并只在可用时把 `GDriveBackend` 加进 `__all__`。

**注意 `backends/__init__.py` 的 try/except 判的是「能不能 import」而不是 `GDRIVE_AVAILABLE`。** `gdrive_backend.py` 内部已经捕获了 ImportError 并设 `GDRIVE_AVAILABLE=False`，所以那个模块**总是能 import 成功**；`__init__.py` 外层那个 try 永远走成功分支，`GDriveBackend` 因此总是被加入 `__all__`——即使 Google 的依赖一个都没装。

## 关键取舍

**源码事实：** 六个方法的契约把「数据集」与「实验」定义成同接口、分开存储的两个命名空间，而不是「实验 = 带元数据的数据集」。

**源码事实：** 空集合返回 `[]`（三处 docstring 都强调）、缺失抛 `FileNotFoundError`——「空」与「不存在」必须可区分。

**源码事实：** 实现通过 `pyproject.toml` 的 `[project.entry-points."ragas.backends"]` 注册，名字含斜杠（`local/csv`）只是标识符。

**源码事实：** `BackendRegistry` 用 `__new__` + 类变量做单例；所有读接口都靠 `_discovered` 惰性触发发现；单个 entry point 加载失败只记 warning。

**源码事实：** `__delitem__` 删主名时会级联删除指向它的全部别名；删别名只删别名本身。

**源码事实：** `_validate_name` 只校验「非空字符串」，不校验格式。

**源码事实：** `create_backend` 每次新建实例，不缓存。

**源码事实：** `GDriveBackend` 用桩类保证「缺依赖也能 import」，但 `backends/__init__.py` 的外层 try/except 因此总是成功，`GDriveBackend` 总在 `__all__` 里。

**架构推论：** 「字符串 + entry point」这套插件机制的标准收益是**装配与实现解耦**：ragas 不需要知道有哪些后端，第三方包自己声明。标准代价是**发现失败是静默的**：一个插件 import 时报错，注册表只是少一条，用户拿到的是「Backend 'x' not found. Available backends: [...]」——列表里没有它，但没人告诉他为什么。

**架构推论：** `BaseBackend` 把六个方法定成抽象而非默认实现，意味着**任何新后端都必须自己处理「空集合」与「缺失」两个边界**。这与「提供默认实现 + 子类覆写」相反：后者省事但容易让某个后端悄悄返回 `None`。把边界写进 docstring 三遍，是在用文档替代码兜底。

**架构推论：** 接口同时接受「字符串名」与「后端实例」，是这类存储抽象的必要双入口：字符串适合配置文件与 CLI，实例适合需要复用的昂贵连接（数据库池、云客户端）。`create_backend` 不缓存实例，正好把「要不要复用」的决定权留给调用方。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/backends/base.py` | `BaseBackend` | 六个抽象方法的契约与边界语义 |
| `src/ragas/backends/registry.py` | `BackendRegistry` | 单例注册表：注册、别名、惰性发现、dict 风格访问 |
| `src/ragas/backends/registry.py` | `BackendRegistry._discover_backends` | 两代 `entry_points` API 的兼容与单插件容错 |
| `src/ragas/backends/registry.py` | `BackendRegistry._validate_backend_class` | 强制继承 `BaseBackend` |
| `src/ragas/backends/registry.py` | `BackendRegistry.register_aliases` | 别名映射与冲突跳过 |
| `src/ragas/backends/registry.py` | `BackendRegistry.__delitem__` | 删主名时级联清理别名 |
| `src/ragas/backends/registry.py` | `BackendRegistry.create_backend` | 唯一的实例化入口，不缓存 |
| `src/ragas/backends/registry.py` | `get_registry` / `register_backend` | 全局单例访问与注册便捷函数 |
| `src/ragas/backends/registry.py` | `print_available_backends` | 以文本列出全部后端与说明 |
| `src/ragas/backends/__init__.py` | 导出与可选后端处理 | 内置后端导出 + `GDRIVE_AVAILABLE` |
| `src/ragas/backends/inmemory.py` | `InMemoryBackend` | 两个 dict，读写双向 `deepcopy` |
| `src/ragas/backends/local_csv.py` | `LocalCSVBackend` | CSV 落盘，值退化为字符串，首行决定表头 |
| `src/ragas/backends/local_jsonl.py` | `LocalJSONLBackend` | 逐行 JSON，额外做 datetime 往返 |
| `src/ragas/backends/gdrive_backend.py` | `GDriveBackend` | 可选依赖 + 桩类保证可导入 |
| `src/ragas/dataset.py` | `DataTable._resolve_backend` | 字符串/实例双入口与三类异常翻译 |
| `pyproject.toml` | `project.entry-points` | 四条内置后端的 entry point 声明（组名 `ragas.backends`） |

## 参考

- [ragas 源码：backends/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/backends/base.py)
- [ragas 源码：backends/registry.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/backends/registry.py)
- [Python 文档：importlib.metadata](https://docs.python.org/3/library/importlib.metadata.html)
- [ragas 文档：数据集](https://docs.ragas.io/en/stable/concepts/datasets/)

## 与主核 / AI 的关系

这套「接口 + entry point + 单例注册表」与主核里「存储引擎/插件式算子」的分层完全同构：接口定义边界语义，注册表负责装配，实现由外部包提供。主核用动态库 + 注册函数，Python 用 entry point——两者都在解决同一个问题：**让实现可插拔而不让核心代码知道具体实现**。

三条可直接用的结论。第一，**「空」与「不存在」必须分开**：`[]` 与 `FileNotFoundError` 的区分看似琐碎，实则决定了上层「新数据集 vs 空数据集」的分支能不能写对；文档里连写三遍说明这条最容易被实现者做错。第二，**插件加载失败要能定位**：这里只记 warning，于是「后端不见了」这个现象没有任何线索指向「插件坏了」；更好的做法是把失败信息留在注册表里，`print_available_backends` 时一并展示。第三，**可选依赖用桩类保住导入**：`GDriveBackend` 的桩类模式让模块总是可导入、错误推迟到调用时——但外层再套一层 try/except 反而让「不可用」这个事实在 `__all__` 上失真，这类「双重容错」容易互相掩盖。

最后一条判断：看一个框架的数据层是否可用于生产，看三件事——后端是否可插拔、空集与缺失是否区分、昂贵的连接能否被复用（实例入口）。三者缺一，数据层在规模化时一定会被改写。
