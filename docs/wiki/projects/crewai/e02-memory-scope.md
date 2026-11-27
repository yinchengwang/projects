---
project: crewai
feature: memory-scope
title: 记忆作用域与隔离
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/memory/memory_scope.py
    symbols: [MemoryScope, MemorySlice, subscope]
---

# 记忆作用域与隔离

## 一句话职责

`MemoryScope` 与 `MemorySlice` 是同一份记忆的两个受限视图：前者把全部操作钉在一棵子树下，后者把若干棵子树拼成一个只读窗口——它们都不复制数据，只改变「操作落在哪个路径上」。

## 机制是什么

记忆的存储模型是**路径树**：每条记录有一个 `scope` 字段，形如 `/company/team/user`。所有检索与删除都按 `scope_prefix` 前缀过滤。视图层不引入新的隔离维度，只是把「前缀」这件事抽出来：

- `MemoryScope(root_path)`：所有读写都被钳在 `root_path` 及其子路径内。调用方传的相对 scope 会被拼在根下面，传 `/` 或 `None` 表示「就是这个根」。
- `MemorySlice(scopes, categories, read_only=True)`：跨多棵子树的窗口。检索会**逐个子树分别查**再合并去重排序；写入默认是 no-op（因为「往哪个子树写」不可推断，必须显式给 scope）。

两者都带 `memory_kind` 判别字段（`"scope"` / `"slice"`），与 `Memory`（`"memory"`）组成一个判别联合。这是为了让 `Flow.memory` 这类字段能接受任意一种视图并正确地反序列化——旧配置里的字典没有这个字段，所以还有一个前置修补函数（`_ensure_memory_kind`）按「有 `scopes` 键 → slice」「有 `root_path` 键 → scope」「否则 memory」来补。

## 怎么实现的

**运行时依赖不进字段。** 两个视图都把 `Memory` 放在私有属性 `_memory` 上，而不是 Pydantic 字段。理由写在 `bind` 的 docstring 里：`Memory` 无法被序列化，所以检查点里只能保存视图的路径配置，恢复后再把活着的 `Memory` 绑回去。`_require_memory()` 在未绑定时抛一个明确的错误（提示调用 `.bind(memory)`），而不是后续某个操作莫名失败。

**构造期归一是在 `mode="wrap"` 校验器里做的。** 之所以用 `wrap` 而不是 `before`/`after`：需要先从输入字典里把 `memory` 键摘出来（它不是字段），再让标准校验跑完，最后把摘出来的对象挂到实例的私有属性上。`MemoryScope` 还顺手归一了 `root_path`：去掉尾斜杠、必要时补前导斜杠，得到内部 `_root`。`MemorySlice` 则把每个 scope 的尾斜杠去掉、空串变 `/`。

**路径拼接是视图的唯一实质逻辑。** `_scope_path(scope)` 的规则：空或 `/` 返回根；否则去掉尾斜杠、补前导斜杠，再拼到根后面。当根本身为空（根是 `/`）时直接返回子路径。这个函数被 `remember`、`remember_many`、`list_scopes`、`info`、`tree`、`list_categories`、`reset` 全部复用；`recall`、`forget`、`list_categories` 写的是 `self._scope_path(scope) if scope else (self._root or "/")` 这个等价变形——因为 `_scope_path` 对空输入本来就返回根，两种写法结果相同，只是把「默认检索整个子树」这个意图写在了调用点。这个「默认范围是整棵子树」的语义对读、删、统计三种操作一致。

**`subscope` 只改路径，不新建上下文。** 它把当前根与新段的拼起来，然后直接拿同一个 `_memory` 构造一个新的 `MemoryScope`。空段（全是斜杠）时退化成「根相同的副本」。所以「下钻」是零成本的，且父视图与子视图共享同一个后端与同一个写入队列。

**`MemorySlice.recall` 是合并去重的多路检索。** 它循环每个 scope 调底层 `recall`，但请求量放大 `_RECALL_OVERSAMPLE_FACTOR`（2 倍）——源码注释解释了原因：后续要做跨子树合并与去重，不多取就会因为去重后不足数而少给结果。合并后按 `score` 降序排，用记录 id 去重，取前 `limit` 条。分类过滤支持「slice 级默认分类」：调用方没传 `categories` 就用视图自己的。

**`MemorySlice` 的聚合操作会做算术。** `info` 把各子树的记录数**相加**、分类取并集、最旧/最新时间取最小/最大、子作用域取并集去重；`list_categories` 把同名分类的计数相加；`list_scopes` 拼出完整路径后去重排序。这些聚合让「视图」在统计意义上像一个独立作用域，尽管底层是若干不相连的子树。

**写入的两种态度。** `MemoryScope.remember` 直接转发（路径拼好），所以作用域视图是可写的。`MemorySlice.remember` 在 `read_only=True`（默认）时返回 `None`，不写任何东西；即使 `read_only=False`，也要求调用方显式给 `scope`——因为跨子树视图没有「默认写入位置」。这是一个刻意的信息缺口设计：不猜，让调用方明确。

## 关键取舍

**源码事实：** 作用域是纯前缀，不是访问控制。`MemoryScope` 能挡住「用相对路径写到别处」，但底层的 `Memory` 本身一旦被拿到就能访问全树。所以它是**减少误用**的工具，不是安全边界。真正的隔离靠 `private` / `source` 字段（见 e01 的私密过滤）。

**源码事实：** 视图不复制数据，也不缓存统计。`info` 每次都真的去后端查各子树。这让数据永远新鲜，代价是深树上的统计成本随子树数量线性增长。

**源码事实：** `MemorySlice` 默认只读。这是最保守的默认值——跨子树写入的位置歧义太大，宁可让调用方显式构造可写 slice 并显式给路径。

**源码事实：** 反序列化依赖 `memory_kind` 判别字段，同时保留一个按键名猜测的兼容修补。这说明判别字段是后加的，而旧数据（配置、检查点）必须能读。

**架构推论：** 把视图做成「共享后端 + 不同路径前缀」，使「一个团队一个记忆空间」这类需求不需要多套存储，只需要多套前缀。代价是所有隔离都发生在查询条件里，后端必须真的把 `scope_prefix` 用在索引上（前缀索引），否则前缀过滤会退化成全表扫。

**架构推论：** `subscope` 返回的是新的 `MemoryScope` 而不是修改当前对象，配合不可变的路径语义，使视图可以安全地在异步代码里传递。这个选择与「配置进字段、依赖进私有属性」是同一条设计线的两端：凡是可共享的都做成值，凡是不可共享的都放进私有绑定。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/memory/memory_scope.py` | `MemoryScope` | 单子树视图：路径归一、操作转发、`bind` 绑定运行时依赖 |
| `lib/crewai/src/crewai/memory/memory_scope.py` | `MemorySlice` | 跨子树只读窗口：多路检索合并去重、统计聚合 |
| `lib/crewai/src/crewai/memory/memory_scope.py` | `subscope` | 下钻：拼路径并共享同一个后端 |
| `lib/crewai/src/crewai/memory/memory_scope.py` | `_scope_path` | 相对路径拼接的唯一实现，被多数操作复用 |
| `lib/crewai/src/crewai/memory/memory_scope.py` | `_ensure_memory_kind` | 旧字典数据补判别字段，维持联合反序列化兼容 |
| `lib/crewai/src/crewai/memory/memory_scope.py` | `bind` / `_require_memory` | 检查点恢复后重新绑定活对象，未绑定时明确报错 |

## 参考

- [CrewAI 源码：memory_scope.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/memory/memory_scope.py)
- [CrewAI 文档：Memory 概念](https://docs.crewai.com/en/concepts/memory)
- [Pydantic 文档：判别联合](https://docs.pydantic.dev/latest/concepts/unions/#discriminated-unions)

## 与主核 / AI 的关系

这里的「路径前缀作用域」与主核里的 schema / 命名空间是同一种抽象：用一棵树把数据分区，查询时给一个前缀或全限定名来限定范围。差别在强制力——主核的 schema 是权限与命名解析的硬边界，跨 schema 引用必须写出全名；这里的前缀只是一个查询参数，任何持有底层 `Memory` 的代码都能绕过视图。

`MemorySlice` 的「多路检索 + 过采样 + 合并去重」与主核里对分区表做并行扫描再归并是同一算法：都先按分区发起检索，再统一排序去重。两边的过采样动机也一样——局部取满不等于全局取满，去重与过滤会吃掉结果，所以每一路都要多取。区别在主核的归并可以做流式（有序归并），这里的 `score` 需要全部算完才能排序。

对 AI 工程的实际意义在于：多智能体系统天然需要「谁记得什么」的边界——一支团队、一个角色、一次会话。用路径前缀实现这套边界的最大好处是零额外设施（不需要为每个边界建独立索引），最大风险是把「视图」误当「权限」。这套代码的正确读法是：视图负责组织与防误用，真正的安全边界在记录级的 `private` / `source` 过滤里，两者缺一不可。
