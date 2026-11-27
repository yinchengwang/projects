---
project: pydantic-ai
feature: capability-composition
title: 能力组合与排序
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/capabilities/combined.py
    symbols: [CombinedCapability]
  - path: pydantic_ai_slim/pydantic_ai/capabilities/abstract.py
    symbols: [leaf_capabilities, _combine_duplicate_capabilities]
---

# 能力组合与排序

## 一句话职责

`CombinedCapability` 把多个能力编织成一条中间件链：它先按声明的位置约束拓扑排序，再把树摊平成一条派发序列；它同时是「同一个 `id` 的两个能力如何变成一」的裁决者。

## 机制是什么

组合层解决三件事，它们互相牵扯：

1. **顺序**。能力之间是中间件语义——列表里第一个是最外层，包裹其他所有层。每个能力可以用 `get_ordering()` 声明 `position`（固定在最外或最内层）、`wraps`（我包住谁）、`wrapped_by`（谁包住我）、`requires`（这些类型必须在场，但不定序）。
2. **摊平**。嵌套的 `CombinedCapability` 默认只是一层分组，它的子能力会被摊进外层，作为兄弟参与同一次排序。例外是「延迟组」：带 `id` 且 `defer_loading=True` 的组合保持为一个整体。
3. **同 id 合并**。同一个 `id` 出现两次意味着「同一个能力被配置了两次」，由该能力的 `combine` 决定怎么变成一；但这条规则只在**同一层内**成立，跨层是覆盖而不是合并。

## 怎么实现的

**两张列表，不是一张。** `CombinedCapability` 内部维护两个序列：`capabilities` 是树（用于遍历与重建），`_dispatch` 是排序后的派发序列。绝大多数情况下两者内容一致；只有在存在延迟组时它们才分叉——`_dispatch` 里延迟组的子能力会以 `_GroupMember` 的形式逐个出现、各自参与排序，而 `capabilities` 保持用户给的顺序，因为「任何整支的顺序都无法匹配这种交错派发顺序，而按重新排过序的分支重建会改变平局如何排序」。

**摊平发生在 `__post_init__` 的末尾。** `__normalize_capabilities` 遍历 `capabilities`，对每个分支调 `_combination_roots(cap)`：普通的分组直接展开成它的子项，延迟组原样保留。展开是必需的——否则一个子能力分布 `outermost` 与 `innermost` 两个层的嵌套分组，会迫使 `_effective_ordering` 把这两层合并成一个位置，从而抛「Conflicting positions」。

**排序用标准库的拓扑排序。** `sort_capabilities` 建一个 `TopologicalSorter`，按原始顺序 `add` 每个下标（这一步确立了「无约束时的平局顺序 = 用户给的顺序」），再按三种约束加边：

- 层级：`outermost` 的成员必须在所有非成员之前；`innermost` 的成员必须在所有非成员之后。
- `wraps=[X]`：我 → X（我先）。
- `wrapped_by=[X]`：X → 我。

引用既可以是类型（用 `issubclass` 匹配该子树里任一叶子），也可以是实例（用 `is` 同一性匹配）。源码在 `CapabilityOrdering` 的注释里点明了实例引用的陷阱：`for_run` 若返回新实例，对原实例的引用就再也匹配不上，所以有按运行时隔离状态的能力应当用类型引用。

失败有两条：成环抛「Circular ordering constraints among capabilities」；`requires` 里声明的类型若一个都没出现，抛「X requires Y but it was not found among the capabilities」。

**排序是子树级的，不是对象级的。** 加边前先对每个分支求 `_effective_ordering`：把该子树里所有叶子声明的位置与引用合并成一份。因此「一个包装器 + 它包住的能力」参与排序时作为一个整体。合并时若两个叶子声明了不同的 `position`，直接抛错并提示「把每个层包进自己的能力，或把叶子暴露成兄弟」。

**派发方向是刻意不对称的。** 看 `_dispatch` 的消费方式就能读出中间件语义：

| 钩子族 | 遍历方向 | 结果 |
|---|---|---|
| `before_*` | 正序 | 最外层先跑 |
| `after_*` | 逆序 | 最内层先跑 |
| `wrap_*` | 逆序建链 | 正序执行，最外层包住全部 |
| `on_*_error` | 逆序 | 最内层的错误处理先看到，可替换错误继续往上传 |

`on_*_error` 有一条特殊规则：`RetryModelRequest` 被单独 `raise` 出去，注释解释了原因——它是控制流而不是替换错误，某个能力要求重试已经代表整条链作答，再把它当错误传给外层，会让外层错误地「从重试里恢复」或重新包一层。

**延迟能力在派发时按可用性过滤。** `_ctx_for_active_cap` 在能力是延迟且当前未激活时返回 `None`，所有钩子循环都据此 `continue`。注意被跳过的是钩子，不是注册——延迟组本身仍在注册表里，靠 `id` 被 `load_capability` 找到。

**同 id 合并的三种局面。** `_combine_duplicate_capabilities` 按「层」区分：

- **同层内**：是「同一配置说了两遍」，交给 `combine`。默认实现 `merge_capability_fields` 逐字段合并——`None` 读作「未声明」，只有一方声明的值存活；两边都是映射/集合/序列则求并集（后出现者赢同名键，序列按首次出现的顺序去重）；其余取后值。`None` 读作未声明是这里最关键的一行：它使得 `WebSearch(allowed_domains=[...])` 与裸的 `WebSearch()` 组合后，白名单不被静默放宽。
- **跨层**：后者**整体覆盖**前者，`combine` 根本不参与。注释给的例子很直接：`agent.run(capabilities=[WebSearch(allowed_domains=[...])])` 是在说「这次运行只准访问这些域」，把它与 agent 层的白名单合并恰好会放宽这个限制。
- **`id=None`**：不碰，运行时自己区分匿名能力。

两条硬拒绝凌驾于上述规则：同一个 `id` 被**不同类**的能力占用一律报错（没有哪个类能定义另一类的合并语义）；能力若没有在类体里声明默认 `id`（`_declares_default_id` 只看类属性），则用户传的同一个 `id` 被视为「他本来想区分开的两件事」，走 `_repeated_id_message` 报「Ids identify one capability within a run, so give each a distinct `id`」而不是合并。

**改子项有专用通道。** `_rebound` 是唯一受支持的替换方式：`replace()` 会重跑 `__post_init__`，而重建 `_instruction_sources` 恰恰会丢掉「子容器自己覆写了 `get_instructions`」这一事实（摊平后那个容器已经不在了）。`_rebound` 做浅拷贝并显式带着替换表，按「每次出现」而非「每个对象」消费决定——同一个对象在列表里出现两次时，两次的决定可能不同。

**绑定分两阶段。** `bind_capabilities_tier` 让 `Agent.__init__` 先绑定非 `innermost` 层的能力，等这些能力贡献的工具集都出现在 `agent.toolsets` 上之后，再绑定 `innermost` 层。理由写在 docstring 里：耐久性能力的 `for_agent` 要包裹 agent 的工具集，必须先看见全部工具集。延迟组的子项各自按自己的层分别绑定。

## 关键取舍

**源码事实：** 排序的平局顺序就是用户给的顺序。`TopologicalSorter` 按插入顺序做平局裁决，源码明确这么用。于是「调整能力列表顺序」是一个真实的语义操作，而不是风格选择。

**源码事实：** `_dispatch` 与 `capabilities` 在无延迟组时是同一份内容，有延迟组时才分叉。这是为了同时满足「延迟组整体注册/整体加载」与「子能力按自己的位置参与排序」两个互相矛盾的需求。

**源码事实：** 跨层覆盖不咨询 `combine`。这意味着「限制型能力」在运行层可以完整替换 agent 层的同名能力，而不是与之求并集。

**源码事实：** `merge_capability_fields` 对未声明为 dataclass 字段的实例属性直接报错而不是忽略。理由写得很清楚：`replace_no_init` 会复制最后一个实例，那种属性会静默取最后一值，违背「只有一方声明的值存活」这条承诺，所以宁可拒绝。

**源码事实：** `_authority_ceiling` 是合并规则里唯一的例外，取所有声明值中的**最低**权级，因为降低权限是限制而非配置。

**架构推论：** 把组合做成「拓扑排序 + 树摊平」而不是「链式 `wrap` 嵌套」，使顺序约束可以声明式表达（`wraps` / `wrapped_by` / `requires`），而不必让用户手工排出正确的嵌套层次。代价是排序结果可能与用户书写顺序不同，调试时必须看排序后的派发序列而不是源码里的列表。

**架构推论：** 同层合并 / 跨层覆盖这条二分，本质是把「配置」与「策略」分开：同层是同一份配置的多处陈述，跨层是上层对下层的收紧或替换。这也是为什么报错文案会区分「你传了同一个 id」与「你类里声明了默认 id」——前者是你想区分，后者是类在说「一个 agent 只有一个我」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/capabilities/combined.py` | `CombinedCapability` | 组合容器：摊平、排序、按方向派发、`_rebound` 重绑 |
| `pydantic_ai_slim/pydantic_ai/capabilities/combined.py` | `CombinedCapability.__normalize_capabilities` | 摊平嵌套分组，生成 `_dispatch`，触发排序 |
| `pydantic_ai_slim/pydantic_ai/capabilities/combined.py` | `_GroupMember` | 延迟组的子能力代理，携带组 id 与延迟标记 |
| `pydantic_ai_slim/pydantic_ai/capabilities/combined.py` | `bind_capabilities_tier` | 两阶段绑定，innermost 层最后绑定以看见全部工具集 |
| `pydantic_ai_slim/pydantic_ai/capabilities/abstract.py` | `leaf_capabilities` | 按应用顺序收集树的叶子能力 |
| `pydantic_ai_slim/pydantic_ai/capabilities/abstract.py` | `_combine_duplicate_capabilities` | 同层合并 / 跨层覆盖的裁决与三类 id 冲突拒绝 |
| `pydantic_ai_slim/pydantic_ai/capabilities/abstract.py` | `combine` | 同 id 合并的默认实现，逐字段合并 |
| `pydantic_ai_slim/pydantic_ai/capabilities/_ordering.py` | `sort_capabilities` | 拓扑排序，位置/相对/依赖三类边与失败处理 |

## 参考

- [pydantic-ai 源码：capabilities/combined.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/capabilities/combined.py)
- [pydantic-ai 源码：capabilities/_ordering.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/capabilities/_ordering.py)
- [Pydantic AI 文档：自定义能力的组合](https://ai.pydantic.dev/capabilities/custom/)
- [Python 文档：graphlib.TopologicalSorter](https://docs.python.org/3/library/graphlib.html#graphlib.TopologicalSorter)

## 与主核 / AI 的关系

这套「声明约束 + 拓扑排序 + 按方向派发」的结构，与主核里查询计划的算子树是同构的：都要把一组带相对顺序要求的单元排成一条确定序列，都要处理成环与缺失依赖。差别在约束的来源——主核的约束来自语义（哪个算子必须在下游）与代价估算，是编译器推导出来的；这里的约束由能力自己声明，是作者意图的直接表达，因此排序器不需要也不可能做代价比较。

值得对照的是「包装器与容器在排序里是一个整体」这条规则。主核里若允许算子重排，就必须先证明重排不改变语义；这里用更简单的办法绕开了这层证明——不拆开一个包装器与它包住的能力，只对顶层单元排序。代价是「一个能力想精确地插到另一个能力的某两层之间」无法表达，只能靠把那个能力拆成两个。

对 AI 工程的实际意义在于：一旦横切行为可以声明相对顺序，就能在不改任何调用方的情况下重组行为栈。但这也意味着**顺序是语义的一部分**：同一组能力换个顺序就是另一套行为，而框架明确不为「两个能力贡献同一设置」告警——注释的原话是顺序决定结果，而顺序是用户选的。调试时必须看排序后的序列，而不是自己写的列表。
