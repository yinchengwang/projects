---
project: pydantic-ai
feature: tool-search
title: 工具搜索与延迟发现
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_tool_search.py
    symbols: [ToolSearchCallPart, ToolSearchReturnPart, ToolSearchArgs]
  - path: pydantic_ai_slim/pydantic_ai/toolsets/deferred_loading.py
    symbols: [DeferredLoadingToolset]
---

# 工具搜索与延迟发现

## 一句话职责

工具搜索把「工具太多、塞不进上下文」的问题变成一次检索：标了 `defer_loading=True` 的工具初始不出现在模型面前，模型用 `search_tools` 按关键词找到它们，找到的那几个才在下一轮上线——搜索本身跨供应商，有原生与服务端本地两条路径。

## 机制是什么

三层结构：

1. **标记**。`DeferredLoadingToolset` 把 `defer_loading=True` 打到工具定义上（全部，或指定的 `tool_names` 集合）。这是一个纯投影：底层工具集不变，只改定义。`Tool(defer_loading=True)` 是单工具级的同类标记。
2. **检索**。`ToolSearchToolset` 包住内层工具集，在 `get_tools` 里把「延迟的工具」收进 corpus，并额外挂一个名为 `search_tools` 的函数工具。模型调用它得到 `ToolSearchReturnContent`：`discovered_tools` 列表（只带名字）。
3. **揭示**。搜索返回的名字构成本轮的「已揭示」集合，下一轮请求时这些工具的完整定义才上线。搜索返回只带名字是有意的——完整定义（描述、参数 schema）在下一轮请求里提供。

跨供应商的可移植性靠一个统一的参数形状：`ToolSearchArgs` 只有 `queries: list[str]`，各适配器在解析时把自家线格式归一成它、在发出时再重建。三种典型映射写在 docstring 里：Anthropic 的 BM25/regex 是「单元素列表放查询串」；OpenAI 服务端执行的 `tool_search` 是「模型选中的工具路径列表」；OpenAI 客户端执行或本地兜底是「单元素列表放关键词」。

## 怎么实现的

**两条路径：原生与服务端本地。** `ToolSearchToolset.get_tools` 里先按 `tool_def.defer_loading` 分出 deferred 与 visible，再按 `self.tools`（`ToolSelector`）把匹配到的非输出、非框架类工具也强制标成 deferred（`replace(tool_def, defer_loading=True)`）。

接着做一件关键的分流：对每个 deferred 工具问 `is_gated_by_deferred_capability(ctx, tool_def)`。

- **受能力门控的**：仍然延迟，但**不**进 corpus。理由写在注释里：这类工具要靠 `load_capability` 加载它的能力才能出现，绝不能靠查询被搜到，否则模型会以为「问出来就能用」，而能力的钩子与指令却没被激活。
- **不受门控的**：进 corpus，并在 `tool_def` 上打 `with_native=_TOOL_SEARCH_BUILTIN_ID`。注释特意澄清语义：`with_native` 只表示「属于 corpus」，不表示别的。

若最终没有 deferred 工具，直接返回 `all_tools`——一个搜索工具都没有的场景不值得多一个 `search_tools` 槽位。

还有一道名称保护：内层若有工具恰好叫 `search_tools`（`_SEARCH_TOOLS_NAME`），直接抛 `UserError` 并提示改名。这是硬拒绝而非静默覆盖，因为静默会悄悄改掉用户的工具行为。

**`search_tools` 何时发出，条件写得很细。** 只有当 `enable_fallback` 为真且 corpus 非空时才挂上去，并带 `unless_native='tool_search'`——于是「供应商支持原生内置时，适配器把它从线上丢掉」。注释解释了两个不显然的理由：

- 保留它在工具集里跨发现步存在，是为了**保住提示词缓存**：一旦所有工具都被发现就把它撤掉，会立刻让请求前缀失效。
- 当没有任何可搜工具时（例如延迟工具全被能力门控），不要发出：它只能永远回答「没找到」，白占一个槽位与缓存前缀字节。
- `enable_fallback=False`（具名原生策略 `'bm25'` / `'regex'`）时干脆不发：既没有本地算法可兜底，发出还会注册一个「幽灵兜底」从而抑制「不支持的 builtin」报错，并在支持它的供应商上留下一个多余的函数工具。

**默认检索：关键词重叠，倒排索引 + 自定义排序键。** `_run_keywords_search` 的实现值得细看：

- 分词用 `[a-z0-9]+`（小写后），对查询与索引项同用一套，所以匹配是大小写不敏感且**有词边界**的——`me` 能匹配 `get_me`，但匹配不到 `comment` 内部的子串。这条注释直接给了例子。
- 倒排表按 `<name, description>` 元组缓存（`_keyword_corpus` / `_keyword_postings`），corpus 变了才重建。这是为多次搜索复用索引。
- 排序键是三元组 `(已发现?, -score, index)`：**「未发现的优先」是主键**，相关度只是次键，corpus 顺序做最后裁决。注释说明了为什么：在 `max_results` 裁剪时，一个已发现的工具绝不能挤掉未发现的匹配，它只能填剩下的空位。
- `max_results` 为负数时保留切片语义（从末尾裁掉若干），所以先对所有匹配排名再切片。
- 全部零分时返回 `_empty_return()`，即 `{'discovered_tools': [], 'message': _NO_MATCHES_MESSAGE}`——注释说明这条消息会经由 JSON 序列化的返回值送到模型，让它不要用同样的关键词重试；需要线上文本的适配器（Anthropic 的自定义可调用空结果路径）也从这里读。

**自定义检索：`search_fn`。** 它收到 `(ctx, queries, corpus)`，可同步可异步；返回的名字列表会被按 `max_results` 截断，且**逐个校验**是否真在 corpus 里（`tool_defs_by_name.get(name)` 为 `None` 就跳过）。这条校验是必要的：用户函数可能凭印象返回不存在的名字，静默忽略比让不存在的工具「被揭示」安全。

`_SearchTool` 这个 `ToolsetTool` 子类除了携带 corpus，还带 `discovered_tool_names` —— 一个在**构建搜索工具那一刻**取的 `ctx.discovered_tool_names` 快照。注释解释了为什么要「已发现」而不是「已揭示」：它是原始的历史证据，不查任何每请求的线上状态；在 corpus 内部两者恰好重合（能力门控的工具被排除在外，所以被历史命名的 corpus 工具也是可用的），但排序只需要更弱的那个事实——「模型以前见过这个工具」——就能把它排在新鲜匹配之后。

**搜索参数 schema 从哪来。** 有一段很讲究的实现：`_search_tools_signature` 只是一个**签名来源，从不被调用**（函数体 `raise NotImplementedError`）。它被包成 `Tool(...)` 以复用与其他工具完全相同的 schema 生成路径，而不是手写 TypedDict + `TypeAdapter`。参数是 `queries: list[str]` 以对齐跨供应商的 `ToolSearchArgs` 形状。

自定义 `parameter_description` 时走 `_build_search_args_schema`：用 `deepcopy` 改已有 schema 里的 `queries.description`，而不是重新构造带闭包变量的签名。注释给了原因：`from __future__ import annotations` 会把类型表达式变成字符串，闭包里捕获的描述在 `Tool` 事后重新求值字符串时是解析不到的。函数带 `@cache` 按描述记忆化，所以只有首次用到某个覆盖值才付构建成本。

**发现历史是「重放证据」而不是「存状态」。** `parse_discovered_tools` / `discovered_tool_names_in_order` 从消息历史里重建「已经发现了哪些工具」，三个证据源：

- 请求侧的 `ToolAvailabilityDeltaPart.tools_added`；
- 请求侧的 `ToolSearchReturnPart.discovered_tools`；
- 兼容旧历史：`ToolReturnPart` 且 `tool_name == 'search_tools'` 时读 `metadata['discovered_tools']`（用 TypedDict 校验，校验失败就跳过——这样不会把用户自定义的同名工具误判为发现记录）。

响应侧则读 `NativeToolSearchReturnPart.discovered_tools`（服务端原生搜索的结果在响应里）。

`discovered_tool_names_in_order` 返回**首次出现顺序**的元组，注释说明原因：线上工具段的字节稳定性（`byte-stable provider tool segments`）。两者都只扫描 `post_compaction_window`，即「模型真正还能看见的那段历史」——这是同一个边界定义被复用。

`parse_discovered_tools` 的注释里有段很坦诚的权衡说明：每个 `CompactionPart` 在它所在位置重置派生状态；判据要够 provider-agnostic，因为**多算**会阻止重新发现或声称一个其实不可见的 schema 可见，而**少算**一次只会多允许一次冗余但幂等的搜索。后来可用性成了执行门槛，少算也会**拒绝**调用，所以复合窗口那套追踪就是为那个场景补的。

**原生与本地两条路径的互转。** `_tool_search.py` 提供了成套的合成函数：

- `synthesize_local_from_native_call` / `..._return`：把服务端原生部件翻成本地形状，保留 `tool_call_id`、`content`、`metadata`，丢掉 `provider_*`。
- `synthesize_local_tool_search_messages`：把历史里的原生部件翻成本地消息。它维持一条不显然的**消息形状差异**：原生服务端搜索是**一条**响应同时带调用与内联结果；本地的函数式执行是「响应(调用) + 请求(结果)」两条消息。所以它以每个 `NativeToolSearchReturnPart` 为**切分边界**：之前的部分（文本、调用）成一条响应，结果成一条请求，之后的部分（下游工具调用、更多文本）另起一条响应。注释给的例子很具体：原生一轮 `[Text, SearchCall, SearchReturn, ToolCall(weather)]` 会翻成四条消息，weather 调用落在搜索返回之后的独立响应上，与「非原生供应商上模型会分两轮发出」的形状一致。
- 切分时的元数据纪律：`_split_response(first=True)` 保留原响应的身份级元数据（provider response id、usage），后续切分把 `provider_response_id` 清空、`usage` 归零，避免下游重复计费或把一次 API 调用当成两次。
- `pair_returns=True` 是给一类脏历史的修补：已存的 OpenAI 历史可能给匿名搜索输出的结果用输出项 id（`tso_…`）而不是它调用方的 id，按 id 配对工具结果的 API 会拒绝。于是「结果不指任何已翻译调用，而本响应里还有未被回答的调用」时，取最老的那个未回答调用的 id。注释同时保留了另一条情况：指更早响应调用的结果保持自己的 id——Anthropic 可能在调用之后的那条响应里才送达搜索结果。
- `repair_dangling_translated_calls`：翻译后若有调用没结果，补一个合成的「被打断」返回。它复用 `messages.py` 的私有修复辅助，保证「与 agent 自己会补的返回一致」；尾随的响应不动，因为那是「恢复运行时的活动前沿」。

**揭示的安全检查在别处。** `_tool_execution` 里 `_reject_unloaded_capability_reveals` 拒绝「用 `ToolReturn.tools` 揭示一个未被加载的能力所拥有的工具」，报错要求用 `load_capability` 加载该能力，以便它的指令与钩子一起激活。它按**可用性**而非「是否已加载」判定——注释说明了一个实际的坑：一个常驻能力永远不会被 `load_capability`，按 `loaded_capability_ids` 判会拒绝它自己的搜索门控工具，并给出一个开发者无法执行的加载步骤。`load_capability` 自身通过 `activating_capability_id` 豁免它正在激活的那个能力，同时仍对每个其他能力的工具受同一规则约束，所以它不能夹带私货。

## 关键取舍

**源码事实：** 延迟工具分两类——受能力门控的永不进 corpus（只能靠加载能力揭示），不受门控的才可搜索。这条区分决定「模型能否靠提问拿到它」。

**源码事实：** `search_tools` 在 corpus 已搜空时仍留在工具集里，为的是不破坏提示词缓存前缀；但 corpus 一开始就空时不发出，免得白占槽位。

**源码事实：** 搜索返回只带工具名，完整定义在下一轮请求里给。这让返回体小，也把「谁负责揭示」明确地交给下一轮。

**源码事实：** 分词是有词边界的（`me` 不匹配 `comment` 内部），且「未发现优先」是排序主键、相关度只是次键。

**源码事实：** 发现历史从消息历史重放，且只扫压缩窗口；旧历史走 `metadata['discovered_tools']` 兼容路径并做 TypedDict 校验。

**源码事实：** 原生路径与本地路径的消息形状不同，翻译时以「原生返回部件」为切分边界，并刻意把后续切分的 usage 归零。

**架构推论：** 「延迟 + 搜索」这套机制的本质是把工具清单变成一个大对象存储、只在需要时把少数条目读进上下文。代价是多了一轮交互（搜索 → 揭示），收益是工具数量可以远超上下文窗口。这也是为什么排序主键选「未发现优先」而不是纯相关度——搜索是「发现」而非「检索已知」。

**架构推论：** 把发现状态做成「历史证据的重放」而不是「存在运行状态里的集合」，使这套机制与压缩、恢复、跨供应商切换全都兼容：状态永远能从历史里重建。代价是每次都要扫一遍窗口，且证据源有三种（增量部件、类型化内容、旧元数据），必须逐个兼容。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_tool_search.py` | `ToolSearchArgs` | 跨供应商统一的搜索参数形状（只有 `queries`） |
| `pydantic_ai_slim/pydantic_ai/_tool_search.py` | `ToolSearchCallPart` | 本地执行路径的搜索调用部件，带 `queries` 访问器 |
| `pydantic_ai_slim/pydantic_ai/_tool_search.py` | `ToolSearchReturnPart` | 本地执行路径的搜索返回部件，带 `discovered_tools` / `message` |
| `pydantic_ai_slim/pydantic_ai/_tool_search.py` | `synthesize_local_tool_search_messages` | 原生 → 本地历史的翻译与切分边界 |
| `pydantic_ai_slim/pydantic_ai/_tool_search.py` | `repair_dangling_translated_calls` | 补齐翻译后无结果的调用 |
| `pydantic_ai_slim/pydantic_ai/toolsets/deferred_loading.py` | `DeferredLoadingToolset` | 把 `defer_loading=True` 投影到定义上 |
| `pydantic_ai_slim/pydantic_ai/toolsets/_tool_search.py` | `ToolSearchToolset.get_tools` | 分流可搜索 / 能力门控，按条件挂 `search_tools` |
| `pydantic_ai_slim/pydantic_ai/toolsets/_tool_search.py` | `keywords_search` / `_run_keywords_search` | 倒排索引 + 未发现优先排序 |
| `pydantic_ai_slim/pydantic_ai/toolsets/_tool_search.py` | `parse_discovered_tools` / `discovered_tool_names_in_order` | 从历史重放发现证据 |
| `pydantic_ai_slim/pydantic_ai/toolsets/_tool_search.py` | `is_gated_by_deferred_capability` | 判定工具是否只能靠加载能力揭示 |

## 参考

- [pydantic-ai 源码：_tool_search.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_tool_search.py)
- [pydantic-ai 源码：toolsets/_tool_search.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/toolsets/_tool_search.py)
- [Pydantic AI 文档：工具搜索](https://ai.pydantic.dev/tools-toolsets/tools-advanced/#tool-search)
- [Anthropic 文档：工具搜索（tool search tool）](https://docs.anthropic.com/en/docs/build-with-claude/tool-use/tool-search-tool)

## 与主核 / AI 的关系

「延迟加载 + 按需求索」这套思路，对应主核里的**按需读页**：都假设工作集远大于可容纳空间，于是把「什么算在内存里」变成一次显式的取用决定。主核靠访问模式与预读策略决定读哪些页；这里靠模型自己发起的搜索决定揭示哪些工具，判据是可读的名字与描述而非访问频率。

两者都有一个共同的难点：**知道什么已经在内存里**。主核有明确的页状态；这里的状态只能从历史重放，因为消息历史是唯一持久的事实来源。这也是为什么这套实现要维护三种证据源并兼容旧格式——状态不在手边，只能从痕迹推断。主核用的是真状态，所以不需要这些兼容；这里的代价换来了「压缩、恢复到别的供应商、跨进程续跑」都能工作。

排序键那条「未发现优先」也很可对照：主核的缓存替换策略里，命中即更新的项会被视为「热」；这里恰恰相反，**已经发现的工具**被排到后面，因为搜索的目的是发现而不是复用。这是同一个「优先级」概念在不同目标下的相反选择。

对 AI 工程的实际意义在于：工具规模一旦超过几十个，把全量 schema 塞进上下文既贵又降低模型准确率。这套实现给出了可落地的三条做法——用标记（`defer_loading`）而不是删除来控制可见性、搜索只回名字、揭示靠下一轮。同时它暴露了真实工程里必须处理的三类麻烦：能力门控的工具不能靠搜索揭示（否则绕过权限）、原生与服务端两种路径的历史形状不同、以及「已发现」必须能从历史重建而不是存在内存里。
