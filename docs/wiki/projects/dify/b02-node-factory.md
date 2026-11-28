---
project: dify
feature: node-factory
title: 节点工厂与类型注册
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/node_factory.py
    symbols: [DifyNodeFactory, resolve_workflow_node_class, register_nodes]
  - path: api/core/workflow/node_factory.py
    symbols: [_LazyNodeTypeClassesMapping, is_start_node_type]
---

# 节点工厂与类型注册

## 一句话职责

把「节点类型 + 版本」解析成一个具体的节点类：节点实现分散在外部图引擎包与本仓两处，工厂负责把两边合起来、按版本选类、并回答「哪些节点能当工作流入口」。

## 机制是什么

节点的注册方式是一个**自动发现 + 手动导入**的混合体，而这个混合体是被依赖方向逼出来的。

外部包 `graphon` 里有 `graphon.nodes.*`（内置节点），本仓有 `core.workflow.nodes.*`（工作流专属节点），另有 `services.knowledge_fs.knowledge_retrieval_v2`。三者都要注册进同一个注册表，而注册动作是**模块导入的副作用**（`Node` 基类在 `__init_subclass__` 里自助注册）。

`register_nodes` 就是那个把副作用触发一遍的函数：

```python
def register_nodes() -> None:
    """Import production node modules so they self-register with ``Node``."""
    _import_node_package("graphon.nodes")
    _import_node_package("core.workflow.nodes")
    _import_node_package("services.knowledge_fs.knowledge_retrieval_v2")
```

而 `_import_node_package` 用 `pkgutil.walk_packages` 把包下**每个模块**都 import 一遍。这意味着**注册不是声明式的，而是「目录里有什么就注册什么」**——加一个节点文件不需要改任何注册表，只要放进目录。

代价写在 `get_node_type_classes_mapping` 的 docstring 里：「Keeping this import side effect here avoids reintroducing registry bootstrapping into lower-level graph primitives.」——**注册的启动逻辑必须待在会用到它的最上层（工作流层），不能下沉到图原语层**，否则底层每次导入都会触发一遍全包遍历。

## 怎么实现的

**三层缓存共同保护那次昂贵的遍历。**

`register_nodes` 被 `@lru_cache(maxsize=1)` 修饰，所以「遍历三个包」这件事只发生一次。`get_node_type_classes_mapping` 每次调用都先 `register_nodes()`（命中缓存即返回），再取 `Node.get_node_type_classes_mapping()`。

**`resolve_workflow_node_class` 是版本解析的核心，而它里面有一处历史包袱的显式兜底：**

```python
# Historical Agent nodes used version=2 for their tool-parameter format.
# Only the explicit kind marker identifies the newer Dify Agent node.
if (
    node_data is not None
    and node_type == BuiltinNodeTypes.AGENT
    and node_version == "2"
    and not is_dify_agent_node_data(node_data)
):
    node_version = "1"
```

**`version="2"` 有两种完全不同的含义**：旧 Agent 节点的「工具参数格式 v2」，和新 Dify Agent 节点的「节点实现 v2」。同号不同义，只能靠 `node_data` 里有没有那个显式 kind 标记来区分。

这段代码的存在本身就是一份考古记录：**版本号被复用，而旧数据不能迁移**（已经存进数据库的工作流 DSL 不会因为代码改了而变），所以只能在解析时按数据形状猜。**这类兜底无法删除，只能一直带着**——除非做一次数据迁移把旧版本号改掉，而那又要处理在线运行的旧工作流。

**版本解析的降级策略是「精确匹配优先，否则退到 latest」：**

```python
latest_node_class = node_mapping.get(LATEST_VERSION)
matched_node_class = node_mapping.get(node_version)
node_class = matched_node_class or latest_node_class
```

`LATEST_VERSION = "latest"` 是一个**哨兵字符串**，节点类注册时可以显式声明自己是 latest。这个设计的后果是：**存量的老工作流（写着 `version="1"`）如果对应类被删了，会静默用上 latest**，而不是报错。对「删掉一个旧版本实现」这种变更，行为是「老工作流按新版本跑」——**这可能对，也可能静默改变行为**。

真正该报错的只有「连 latest 都没有」，那时才 `raise ValueError`。

**`is_start_node_type` 用 `frozenset` 定义了「什么能当入口」：**

```python
_START_NODE_TYPES: frozenset[NodeType] = frozenset(
    (BuiltinNodeTypes.START, BuiltinNodeTypes.DATASOURCE, *TRIGGER_NODE_TYPES)
)
```

**三种都能当入口**：普通开始节点、数据源节点、三类触发器。这是产品层面的判断——工作流可以由用户点击启动，也可以由数据源变更或外部事件启动。把这个集合收进一个函数，意味着「入口」这个概念**在别处不用再重复判断**。

`get_default_root_node_id` 的 docstring 说明了它为什么在这个文件里：「This workflow-layer helper depends on start-node semantics defined by `is_start_node_type`, so it intentionally lives next to the node registry instead of in the raw `graphon.entities.graph_config` schema module.」——**依赖同一个语义的两个函数放在一起，即使一个看起来属于「图配置模块」**。这是正确的聚合判据：按「共享什么知识」而不是按「像什么名词」分组。

它还跳过 `type == "custom-note"` 的节点——**画布上的便签不是节点**，但在 DSL 里占一个位置。这类「非执行元素混在执行列表里」是画布型产品的通病。

**`_LazyNodeTypeClassesMapping` 是个 `MutableMapping`，同时带缓存、删除集与覆盖集**：`_cached_snapshot`/`_cached_version` 做快照缓存、`_deleted` 记录被删掉的类型、`_overrides` 记录被替换的映射。

**「删除集」与「覆盖集」并存说明它要支持测试期的临时替换**：把某个节点换成 mock 再换回来。用一个 version 号驱动缓存失效，是这里最省事的正确做法。

`DifyNodeFactory` 是那个把这些能力包起来的门面，`DifyGraphInitContext` 是冻结的 dataclass（`frozen=True, slots=True`）承载图初始化值——**`slots=True` 说明作者在意实例内存**，这对每个工作流都建一份的上下文对象是合理的。

## 关键取舍

**源码事实：** 节点注册靠 `pkgutil.walk_packages` 遍历三个包，导入即注册；`register_nodes` 被 `lru_cache(maxsize=1)` 缓存。

**源码事实：** `get_node_type_classes_mapping` 的 docstring 说明「导入副作用必须留在工作流层，不能下沉到图原语层」。

**源码事实：** `resolve_workflow_node_class` 里有显式兜底：`node_type == AGENT` 且 `version == "2"` 且无 kind 标记时，把版本改判成 `"1"`，注释说明旧 Agent 节点用 version=2 表示工具参数格式。

**源码事实：** 版本解析为「精确匹配 → 退到 `LATEST_VERSION`（哨兵 `"latest"`）→ 都没有才报错」。

**源码事实：** `_START_NODE_TYPES` 包含 `START`、`DATASOURCE` 与全部 `TRIGGER_NODE_TYPES`；`get_default_root_node_id` 跳过 `custom-note`。

**源码事实：** `_LazyNodeTypeClassesMapping` 同时持缓存快照、版本号、删除集与覆盖集。

**架构推论：** 「目录里有什么就注册什么」对开发者最友好（加文件即生效），对**运行时可预测性最差**：一个包内的 import 顺序错了、某个模块 import 失败，就可能让某类节点在运行期才「不存在」。而这类失败发生在「用户打开一个老工作流」时，不在启动时。**自动发现的代价总是推迟错误暴露**。

**架构推论：** 「精确匹配失败退到 latest」是**用静默替换换部署便利**。它的安全版应该是「退到 latest 但记一条 warning」——否则「删掉一个旧版本」这个操作在监控上完全无声。判断标准很简单：**这个降级会不会改变已有数据的语义？** 会，就该告警。

**架构推论：** 版本号被复用（`version="2"` 两义）是**不可逆的技术债**。它的根因是把「节点实现的版本」与「节点参数的格式版本」压进同一个字段。修法是加独立的 `kind` 字段（它已经加了，但只用于区分新老 Agent），而旧数据仍要靠形状猜。**这是「字段复用」的经典代价：一次将就，永久兼容**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/node_factory.py` | `register_nodes` | 遍历三个包触发注册（lru_cache 缓存） |
| `api/core/workflow/node_factory.py` | `get_node_type_classes_mapping` | 只读快照入口 |
| `api/core/workflow/node_factory.py` | `resolve_workflow_node_class` | 类型 + 版本 → 具体类 |
| `api/core/workflow/node_factory.py` | `is_start_node_type` | 入口节点语义 |
| `api/core/workflow/node_factory.py` | `DifyNodeFactory` | 节点构造门面 |
| `api/core/workflow/node_factory.py` | `_LazyNodeTypeClassesMapping` | 带缓存与覆盖的可变映射 |
| `api/core/workflow/node_factory.py` | `DifyGraphInitContext` | 冻结的图初始化上下文 |
| `api/core/workflow/node_factory.py` | `get_default_root_node_id` | 从 DSL 找入口节点 |
| `api/core/workflow/node_factory.py` | `LATEST_VERSION` | `"latest"` 哨兵常量 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [graphon：dify 外置的图执行内核](https://github.com/langgenius/graphon)
- [dify 文档：工作流节点](https://docs.dify.ai/guides/workflow/node)
- [dify 源码：core/workflow/node_factory.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/node_factory.py)

## 与主核 / AI 的关系

节点工厂与主核（多模态数据库内核）的交集在**「把类型名解析成实现」这个多态核心上，而且两者的失败模式几乎逐条对应**。

**导入即注册 vs 数据库的算子注册。** 数据库把 `+`/`-`/`=` 之类算子注册到类型系统（PostgreSQL 的 `pg_operator`、`pg_proc`）。两边都是「名字 → 实现」的映射。差别是数据库的表是**持久化的元数据**（改注册要写系统表、有版本、可查询），dify 的注册是**进程内的导入副作用**（重启就重建）。**持久化注册的好处是可审计、可跨版本对比；导入副作用的好处是零配置。** 数据库选了前者因为它要做跨版本升级与备份恢复，dify 选后者因为它每次部署都是全量镜像。

**版本复用 vs 数据库的类型 OID 不可复用。** 数据库的 OID 一旦分配就不再复用，即使对象被删——正是为了避免「同一个号指两个东西」。dify 的 `version="2"` 两义就是这个问题的实证：同号两义没法自动消歧，只能靠数据形状猜。**数据库行业用「永不复用」这条硬规则避开的坑，应用层往往要自己踩一遍。**

**静默退到 latest vs 数据库的类型转换隐式规则。** 数据库有「隐式类型转换」的等级（`pg_cast` 的 castcontext 分 implicit/assignment/explicit）。implicit 的转换会静默发生，也是历史上出问题最多的地方，所以 PG 把大多数转换降级成 assignment。**「精确匹配失败退到 latest」等价于一条 implicit 转换规则**，而它比类型转换更危险——转换至少保语义，换实现不保。

**`_START_NODE_TYPES` vs 数据库的表访问方法（AM）。** 数据库为每种存储访问方法声明它支持什么操作（`amhandler` 注册 `scan`/`insert`/`update`），不是所有 AM 都支持全部操作。dify 的「哪些节点能当入口」是同一类「能力声明」。差别是数据库把能力声明做成注册表的一部分（查得到），dify 做成一个 `frozenset`（查得到但不可扩展）——**加一种新入口类型要改代码，不能靠插件**。

**跳过 `custom-note` vs 数据库的系统列。** 数据库的系统列（`ctid`/`xmin`）物理存在于行里但不参与用户查询。`custom-note` 是「DSL 里有、执行时不参与」的同类东西。**两者都要在遍历时显式跳过**，而「显式跳过」比「过滤掉」脆弱：新增一种非执行元素时，得记得在每个遍历点都加。

对 agent 工程最直接的价值有四条。其一：**自动发现的注册方式会把错误推迟到运行期**，加节点很方便但「节点突然不存在」的故障只在用户打开老工作流时暴露。其二：**版本号不要复用**——一旦两个含义共用一个字段，就只能靠数据形状猜，而且这笔债还不掉。其三：**任何「匹配不到就退到最新」的降级都要告警**，否则「删旧版本」这个操作在监控上完全无声却改变了运行行为。其四：**按共享知识聚合模块，而不是按名词**——`get_default_root_node_id` 该和 `is_start_node_type` 放一起，因为它们共享「什么算入口」这一个判断。
