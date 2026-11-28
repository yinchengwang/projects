---
project: dify
feature: tool-catalogue
title: 工具候选与能力查询
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/generator/tool_catalogue.py
    symbols: [ToolCatalogueEntry, ToolCapabilityQuery, ToolCandidateSelection]
  - path: api/core/workflow/generator/tool_catalogue.py
    symbols: [LegacyToolFallbackSelection]
---

# 工具候选与能力查询

## 一句话职责

在「装了几百个工具」与「上下文只有几万 token」之间做取舍：把完整清单与提示文本**分开保存**——完整清单供校验与节点填充使用，提示文本只发给经过筛选的一小部分。

## 机制是什么

这一层要解决的是**目录规模与上下文预算的冲突**，而它的解法建立在一个关键区分上：

**同一个工具清单要服务三个不同消费者，而三者需要的规模不同。**

| 消费者 | 需要多少 | 原因 |
|---|---|---|
| 规划提示 | 一小部分（≤16） | 提示里塞满工具会挤掉指令 |
| 节点填充 | 完整 | 生成工具节点要精确的身份字段 |
| 结构校验 | 完整 | 否则「装了但没进提示」的工具会被当成幻觉 |

所以模块 docstring 把这条分工写死了：「`build_tool_catalogue` still returns the COMPLETE installed inventory because the validator and node hydrator must never mistake an installed tool beyond the prompt cap for a missing one. `format_tool_catalogue` alone truncates the sorted inventory to `_MAX_PROMPT_TOOLS` lines.」

**这条「完整清单 vs 提示文本」的分离是全模块最重要的设计**：一旦有人为了省 token 在 `build_tool_catalogue` 里截断，校验层就会开始误判——一个真实存在但排在末尾的工具会被判成「模型幻觉」。这种 bug 极难定位，因为错误信息指向的是模型而不是截断。

在上面这个区分之下，提示文本有三档规模策略：

- **直接注入**（工具数 ≤ 24）：全给。
- **路由筛选**（工具数 > 24）：先用一次小调用提取能力查询，再按查询打分选 ≤16 个候选。
- **旧式兜底**（路由失败或查询没命中）：按排序取前 80 个。

## 怎么实现的

**枚举清单时容忍单点失败。**

`build_tool_catalogue` 遍历租户的内置 provider，对每个 provider 再遍历它的工具。两层各有一个 `try/except`：列出 provider 的工具失败就跳过这个 provider，描述单个工具失败就跳过这个工具。两处注释写的理由相同——「one bad provider must not break the whole generator」。

这里有一处**刻意的类型收敛**，注释解释得相当细：工具 provider 领域里「内置」与「插件」是两类（`builtin` vs `plugin`），但工作流工具节点的 `provider_type` 把两者**都归为 `builtin`**。注释说明理由：这样与 `ToolTransformService` 和前端集合类型契约一致，「leaking `plugin` here makes generated nodes invisible to the canvas' installed-tool collection」。

也就是说**生成器输出的节点必须与前端认识的分类对齐**，否则画布上找不到这个节点。这条约束不在任何类型里，只能靠注释传递。而代码里插件 provider 仍单独记录了 `plugin_id` 与 `plugin_unique_identifier`，于是「分类是 builtin、但带着 plugin 身份」这个组合是合法且有意义的。

**键的选择有理由。** `installed_tool_keys` 用 `(provider_name, tool_name)` 而不是 `(provider_id, ...)`，docstring 解释了原因：生成提示要求把 provider 的目录名同时写进 `data.provider_id` 与 `data.provider_name`，所以两者同值。**用哪个都能工作，选一个并在文档里说明另一个同值**，这是避免下游反复犹豫的做法。

**提示文本的格式设计针对一个具体歧义。**

`format_tool_catalogue` 每行形如「`- provider/tool (标签) [provider_id="..."; tool_name="..."] — 描述`」。中间那段 JSON 引号字段看起来冗余，注释解释了原因：「provider names for plugin tools commonly contain slashes themselves (for example `langgenius/google/google`). Explicit JSON-quoted fields remove the ambiguous 'split on the last slash' guess.」

**这是一个真实的解析歧义**：`a/b/c` 里哪个是 provider、哪个是 tool？靠斜杠数量猜会错。所以显式给出两个字段名，让模型不必猜。代价是每行变长，而收益是**消除一个模型必然踩中的歧义**。

描述被裁到 120 字符（超出就截断加省略号），换行被压成空格。这两个处理都是为了让「一个工具一行」这个结构稳定——一旦描述里带换行，模型就可能把续行当成新工具。

**排序后截断而不是任意截断。** `build_tool_catalogue` 返回前按 `(provider_name, tool_name)` 排序，`format_tool_catalogue` 只取前 N 个。所以排序不只为了好看——**它决定了哪些工具会被截掉**。这是一个隐式后果：改排序等于改截断结果，而截断结果会影响生成质量。

**路由阶段：从能力要到候选，中间是一套加权打分。**

`select_tool_candidates` 的处理顺序值得逐步看：

1. **固定项**（pinned）：请求文本里**显式提到**完整标识符的工具，加上 refine 图里已存在的工具。这两类永不被数量上限挤掉。
2. **逐查询排名**：对每个查询（最多取前 5 个）调 `_rank_tools_for_query`，只保留「有信心」的结果，每个查询最多取 3 个。
3. **合并去重**：同一工具被多个查询命中时**取最高分**，不是累加。
4. **全局上限与多样性**：按分数降序填充，**每个 provider 最多 4 个**，总共最多 16 个。

打分公式是：`短语加分 + 名称词重叠×5 + 标签词重叠×3 + 描述词重叠×1 + 模糊相似度`。短语加分是分层的：查询整串出现在「provider/tool 标识」里给 8、出现在标签里给 6、出现在描述里给 2。

「有信心」的判据是：**任意一类词有重叠**，或**模糊相似度达到 0.72**。所以一个工具即使一个词都没对上，只要名字与查询串足够像（比如拼写变体）仍算有信心。

**排序键是三层**：分数降序、provider 名、tool 名。第三层存在的意义是**确定性**——同分时按名字排，保证同样的输入永远得到同样的候选集。

**未命中的查询会被收集起来，触发旧式兜底。** `unmatched_queries` 记录那些排名后一个「有信心」结果都没有的能力描述。只要非空，`runner` 就放弃路由结果、改走 `select_legacy_fallback_selection`。注释里日志会记下 `reason="unmatched_query"` 与完整的选择统计。

这个选择的逻辑是：**只要有一个查询没匹配上，就不能相信这次路由的完整性**。宁可退回「按排序取前 80」这种粗但全的策略，也不要给 planner 一个缺了关键工具的候选集。

**旧式兜底把「固定项优先」写进了上限计算。**

`select_legacy_fallback_selection` 先算出 pinned 集合，然后 **`regular_limit = 80 - len(pinned)`**，即固定项先占额度，剩下的名额给常规工具按排序填。

所以当显式提到的工具很多时，常规工具的名额会被压缩到 0，而总条数**可能超过 80**（`entries` 是 pinned + regular[:regular_limit]，若 pinned 本身超过 80 就超了）。`overflow_count` 属性专门表达这个情形：「Count intentional pinned overflow beyond the legacy prompt cap」——**超出上限是有意的，因为固定项不该被上限拒绝**。

`LegacyToolFallbackSelection` 之所以同时带 `omitted_entries` 与 `limit`，是为了让日志能说清「哪些工具被丢了」。`runner` 里的日志函数确实把它们列了出来（截断到 20 个再加「+N」）。**「丢了什么」比「丢了多少」更有诊断价值**，这个数据结构就是为此设计的。

**打分里的文本归一做了四件事。**

`_normalize_search_text`：先做 Unicode NFKC 归一（全半角统一），再在驼峰边界插空格，然后把下划线与连字符换成空格，最后用小写化。所以 `getWeather`、`get_weather`、`Get-Weather` 会归一到同一组词。

`_search_tokens` 在此基础上切词、丢掉长度 ≤1 的词、并丢掉一个停用词表（含 `tool`/`using`/`with` 等 14 个）。**停用词表里有 `tool` 本身**——因为几乎每个查询里都会写「用某个工具」，这个词无区分度。

`_fuzzy_ratio` 用 `SequenceMatcher`，且对两侧都截到 160 字符。截断的理由是性能：序列相似度是平方复杂度，长描述会让排名变慢。代价是**只有前缀参与模糊匹配**——两条描述后半段相似但前半段不同的工具拿不到模糊分。

参数 `autojunk=False` 也是一个具体修正：默认的「自动垃圾」启发式会把在序列中出现频率超过 1% 的元素当噪音，这对自然语言有效但对短标识符有害。

**工具身份里有三处「找不到就退回另一个值」。**

`_i18n_text` 取英文标签、退回中文、再退回空串；`format_tool_catalogue` 里标签等于工具名时**不显示标签**（避免「`weather (weather)`」这种重复）；`format_tool_builder_context` 里 `tool_label` 为空时退回 `tool_name`。

三处都是「显示用的 fallback」，不影响身份字段。真正影响生成的 `provider_name` 与 `tool_name` 从不做 fallback——**身份必须精确，展示可以退化**。

**builder 上下文的参数分派是一条实质规则。**

`format_tool_builder_context` 列出每个参数时带 `form` 字段，并给出一句指令：「`form=llm -> tool_parameters; form=form -> tool_configurations`」。

这条规则解释了表单式工具的两类参数该写到哪里：模型要填的（`llm`）进 `tool_parameters`，界面配置的（`form`）进 `tool_configurations`。**如果生成器把这条搞反，节点会缺参数或把密钥写进模型可见的位置**——所以它被写成提示里的显式指令而不只是字段罗列。

**能力查询的结构很小。** `ToolCapabilityQuery` 只有两个字段：`capability`（一句能力描述）与 `keywords`（关键词列表）。`runner` 侧解析时限制了查询数 ≤5、每查询关键词 ≤8，且规范化后为空的能力描述会被丢弃；若最后一条查询都没有，会抛 schema 错误。**限制数量是为了控制打分成本**——排名是「查询 × 工具」的笛卡尔积。

## 关键取舍

**源码事实：** 完整清单与提示文本分离：`build_tool_catalogue` 返回全部工具，`format_tool_catalogue` 才做截断；docstring 明确说分离是为了不让校验把「没进提示的已装工具」当成缺失。

**源码事实：** 生成节点里插件 provider 的 `provider_type` 被归为 `builtin`，注释说明是为了与前端集合类型契约一致，否则生成的节点在画布上不可见。

**源码事实：** 提示行里显式写出 `provider_id` 与 `tool_name` 两个 JSON 引号字段，因为插件 provider 名可能自带斜杠（如 `langgenius/google/google`），靠斜杠切分会歧义。

**源码事实：** 三档规模策略：≤24 直接全给、>24 走路由、路由失败或任一查询未命中则退回前 80 的排序兜底。

**源码事实：** 固定项（请求里显式提到的 + refine 图中已存在的）永不被数量上限挤掉，旧式兜底里上限按 `80 - pinned 数` 算给常规工具，因此总数可以超过 80。

**源码事实：** 路由打分为「短语加分 + 名称×5 + 标签×3 + 描述×1 + 模糊相似度」，信心判据为「任一类词有重叠」或「模糊相似度 ≥ 0.72」；每查询最多取 3 个、每 provider 最多 4 个、总计最多 16 个。

**源码事实：** 模糊相似度比较前两侧都截到 160 字符且关闭了自动噪音启发式。

**源码事实：** 参数按 `form` 字段分派到 `tool_parameters`（llm）或 `tool_configurations`（form），这条规则写进了 builder 提示。

**架构推论：** 「完整清单与提示文本分离」是这类「模型需要看，但看不了全部」的场景的唯一正确形状。它把「减少模型输入」与「保持系统事实完整」解耦，使截断只影响生成质量、不影响正确性。**任何为了让模型少看而裁剪数据源的做法，都会把性能优化变成正确性问题。**

**架构推论：** 「任一查询未命中就整体退回兜底」是一条保守策略。它的前提是「路由不确定性不可局部评估」——你无法知道没匹配上的那句能力描述是否对应了一个关键工具。**当局部失败无法评估影响范围时，退回全局策略是正确选择**，代价是放弃了路由带来的精度。

**架构推论：** pinned 优先于上限，说明这个上限是「预算」而不是「契约」。预算的意义是控制成本，而显式指定的工具属于**用户意图**，不该被成本约束否决。区分「预算」与「契约」在配额、限流、超时等所有带数值上限的地方都适用。

**架构推论：** 三处展示用 fallback 与零处身份 fallback，构成一条清晰的分界：**展示可以退化，身份必须精确**。这条分界在生成系统里尤其重要，因为生成的产物会进数据库，一个模糊的身份会被持久化下来，后续所有环节都要带着它跑。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/generator/tool_catalogue.py` | `ToolCatalogueEntry` | 完整清单条目：身份、分类、参数、输出模式 |
| `api/core/workflow/generator/tool_catalogue.py` | `ToolCapabilityQuery` | 能力查询的最小结构：能力描述加关键词 |
| `api/core/workflow/generator/tool_catalogue.py` | `build_tool_catalogue` | 枚举租户全部工具，单点失败即跳过 |
| `api/core/workflow/generator/tool_catalogue.py` | `installed_tool_keys` | 校验用的身份集合，按 provider 名而非 id 建键 |
| `api/core/workflow/generator/tool_catalogue.py` | `format_tool_catalogue` | 渲染提示文本，显式写出两个身份字段 |
| `api/core/workflow/generator/tool_catalogue.py` | `ToolCandidateSelection` | 路由结果：候选、未命中查询、固定项计数 |
| `api/core/workflow/generator/tool_catalogue.py` | `select_tool_candidates` | 固定项 + 逐查询排名 + 合并 + 双上限 |
| `api/core/workflow/generator/tool_catalogue.py` | `LegacyToolFallbackSelection` | 兜底结果，含被丢弃项与超限计数 |
| `api/core/workflow/generator/tool_catalogue.py` | `select_legacy_fallback_selection` | 固定项先占额度，剩余名额给常规工具 |
| `api/core/workflow/generator/tool_catalogue.py` | `_rank_tools_for_query` | 加权打分与信心判定 |
| `api/core/workflow/generator/tool_catalogue.py` | `_normalize_search_text` | NFKC、驼峰切分、连字符与下划线归一 |
| `api/core/workflow/generator/tool_catalogue.py` | `_fuzzy_ratio` | 截到 160 字符、关闭自动噪音的序列相似度 |
| `api/core/workflow/generator/tool_catalogue.py` | `text_mentions_tool_identifier` | 带边界锚点的完整标识符匹配 |
| `api/core/workflow/generator/tool_catalogue.py` | `format_tool_builder_context` | 单工具的身份与参数契约渲染 |
| `api/core/workflow/generator/tool_catalogue.py` | `_find_current_graph_tool_keys` | 从 refine 图中提取已用工具 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：generator/tool_catalogue.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/generator/tool_catalogue.py)
- [dify 文档：工具与插件](https://docs.dify.ai/guides/tools)
- [Python difflib：序列相似度](https://docs.python.org/3/library/difflib.html)

## 与主核 / AI 的关系

**「完整数据源 + 裁剪视图」与主核的「统计信息与采样」同构。** 优化器需要知道表的全部列与索引，但估算代价时只看采样。两者都把「知道多少」与「看多少」分开：知道得全，是为了保证不会误判存在性；看得少，是为了在预算内做决定。**把这两件事混在一起，就会出现「因为没采样所以以为索引不存在」这类错误。**

**显式写出身份字段以消除斜杠歧义，与主核里用引号处理标识符同理。** SQL 里 `a.b.c` 是三段限定名，但标识符本身若含点就必须加引号。dify 的工具标识符里若含斜杠，也必须先声明哪段是 provider。**任何「用分隔符拼装复合标识」的方案，都要为「成员自身含分隔符」留出逃逸手段。**

**「预算」与「契约」的区分在原子里反复出现。** 数据库的 `work_mem` 是预算（超了可以落盘），而 `max_identifier_length` 是契约（超了直接报错）。工具目录的 80/16/24 全是预算，固定项是豁免。**把预算当成契约，会让系统在压力下做出错误的硬失败；把契约当成预算，会让非法数据溜进系统。**

**「任一未命中就整体退回」与主核的查询计划拒绝同源。** 优化器遇到无法估算的部分时，会放弃索引扫描改走顺序扫描，而不是对已知部分用索引、对未知部分瞎猜。**部分可信的部分结果比整体保守更危险**，因为你无法在结果里标出哪部分不可信。

对 agent 工程最实用的五条。其一：**永远不要在枚举阶段截断工具清单**，否则校验会把真实工具当幻觉，而错误信息会指向模型。其二：**提示里的工具标识符要显式写出字段名**，不要靠号切分——插件 provider 名常常自带斜杠。其三：**显式提到的工具要豁免数量上限**，用户意图不该被预算否决。其四：**路由只要有哪些查询没匹配上就整体退回兜底**，局部不确定性无法评估影响范围。其五：**参数的 `form` 字段决定它进 `tool_parameters` 还是 `tool_configurations`**，搞反会把界面配置项暴露给模型或让节点跑不起来。
