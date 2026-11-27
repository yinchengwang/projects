---
project: browser-use
feature: history-and-result
title: 历史列表与结果提取
page_type: feature
schema_version: 1
refs:
  - path: browser_use/agent/views.py
    symbols: [AgentHistoryList, final_result, is_successful]
  - path: browser_use/agent/views.py
    symbols: [Usage, load_from_file, structured_output]
---

# 历史列表与结果提取

## 一句话职责

`AgentHistoryList` 是 `Agent.run()` 的返回值：一个扁平的 dataclass，把所有「这次运行发生了什么」摊成字段，再在上面挂一层**刻意保留的 0.x 方法名**——凡是 1.0 无法忠实实现的旧方法，都在被调用时抛出指名替代方案的错误。

## 机制是什么

这个类要同时满足两件冲突的事：

- **1.0 的结构变了**。0.x 里每一步是一个 action，`history` 是一串 action 记录；1.0 里每一步是一个 Python 单元格，动作可能在同一格里发生几十次。所以「按步取 action 列表」在 1.0 里没有对应物。
- **0.x 的调用代码还在**。用户写的是 `result.final_result()`、`result.is_successful()`、`result.usage.total_tokens`。

它的解法是**字段全部用 1.0 的形状，方法名全部保留 0.x 的**，中间用三层机制接：

| 机制 | 用途 |
|---|---|
| 真实现 | `final_result` / `structured_output` / `is_successful` / `errors` / `number_of_steps` 等能忠实实现的方法 |
| `REMOVED` + `ALTERNATIVES` 表 | `__getattr__` 拦下无对应物的方法名，报出 1.0 的做法 |
| `__len__` / `__bool__` / `__repr__` 覆写 | 三处「协议方法」的显式处理 |

字段本身的设计密度很高，几乎每个都有注释解释取舍。

## 怎么实现的

**`status` 的四种取值与两种「没交付」的区别。** 注释写得很细：

> `completed` | `timeout` | `max_steps` | `error`. A run that reaches max_steps or the timeout gets one forced delivery turn and still returns `completed`（success=False when the agent reports it unfinished）; `max_steps` and `timeout` mean nothing was delivered: read `partial`.

即四个字符串分两组：

- `completed` 表示**有东西交付**。哪怕撞上了步数上限或超时，也会先给一次「强制交付」的机会，只要交出来了状态仍是 `completed`，只是 `success=False`（若模型自己承认没做完）。
- `max_steps` / `timeout` 表示**什么都没交付**，此时应该去读 `partial`。
- `error` 是异常终止。

这个区分让调用方只需判一个字段就知道该读 `text` 还是 `partial`，而不必组合多个条件。

**`cost_usd` 的边界被明确划了出来。** 注释写「model + web search charges only（None when a model's price is unknown）; a cloud browser and its proxy are billed apart: `GET /api/v4/browsers/{browser.id}` has `browserCost` and `proxyCost`」。即这个字段**不是总成本**，而是 API 侧能算出来的那部分；云端浏览器与代理是另一条计费线，要靠另一个接口查。`None` 的含义是「模型价格未知」，而不是「免费」。

这是一个很值得记的设计：把「部分成本」诚实地命名成 `cost_usd` 并在注释里划清边界，比叫 `total_cost` 却只含一部分要好。

**`success` 的三种取值承载了真实的语义差异。** `success: bool | None`：

- `True` / `False` 来自模型的 `finish(..., success=...)`。
- `None` 表示**模型没有给判定**——可能是没走 `finish`、可能是用纯文本答的、也可能给了 `finish` 但没带 `success`。

`__post_init__` 里有一条硬规则：`if self.status != 'completed': self.success = None`。即**没完成就没有判定可言**，无论模型说过什么。这避免了「运行崩溃但 `success=True`」这种自相矛盾的状态。

`is_successful()` 的 docstring 把 0.x 的差异讲得很清楚：

> The agent's verdict: True / False from `finish(..., success)`; None when there is no verdict…（0.x `done` defaulted it to True）. For the 0.x default use `is_done() and success is not False`.

也就是说 0.x 里「做完了就算成功」，1.0 里「成功」是模型的**主动声明**。要给旧行为，用 `is_done() and success is not False` 这个表达式——注意是 `is not False` 而不是 `is True`，这样 `None` 也算成功。这行注释实际上给出了一段兼容代码，比单纯说「不兼容」有用得多。

**`structured_output` 的判据绕了一圈。** 实现是一行：

```python
structured = self.output_schema is not str if self.output_schema is not None else not isinstance(self.output, str)
```

读法：若声明了 `output_schema`，则「结构化」等价于「schema 不是 `str`」；若没有声明（`None`，意味着「未知」），则退化为「输出不是字符串」。

这解决了 `Literal[...]` 与 `StrEnum` 这类**结构化输出但类型就是字符串**的边界情况。docstring 明确写「A `Literal` or `StrEnum` schema delivers a `str`: still structured」。若按「是不是字符串」判断，这两种会被误判成非结构化。

`output_schema` 字段的注释也说明了 `None` 的双重含义：「`Agent(output_model_schema=...)`（`str` when none）; `None`: unknown, judged by the output」。即 `Agent` 传了 `output_model_schema` 就用它，没传时字段是字符串 `str`（表示「显式无结构化」），而 `None` 表示「不知道，要靠输出反推」。

**`__repr__` 的取舍有实测数字。** 注释：「Without `.messages`: the transcript（screenshots included）made `print(result)` about 565 KB.」所以 `__repr__` 里只放 `messages=<N messages>`，且 `text` 超过 500 字符就截断并附上原始长度。这是一个很典型的「打印友好」设计点——一个 dataclass 的默认 `repr` 在装了截图之后会变成几百 KB 的噪音。

**三个协议方法的覆写各自解决一个坑。**

- **`__len__` 抛 `TypeError`**：0.x 里「一条历史一项」，1.0 里没有这个概念。若返回 `number_of_steps` 会让 `len(result)` 静默给出误导性数字，所以直接报错并指向 `number_of_steps()`。这条注释只有一句「0.x: one history item per step; there are none here」。
- **`__bool__` 必须覆写**：注释写「without it, truth testing would call `__len__`」。Python 的真值测试在无 `__bool__` 时会调 `__len__`，而 `__len__` 已经抛错了——于是 `if result:` 会炸。返回常量 `True` 修掉这个连锁。
- **`load_from_file` 是 `classmethod`**：注释「A 0.x classmethod: called on the class, `__getattr__` never runs」。这是 Python 数据模型的一个细节——类属性查找不走实例的 `__getattr__`，所以如果只在 `__getattr__` 里拦 `load_from_file`，`AgentHistoryList.load_from_file(...)` 这种 0.x 写法会得到 `AttributeError: type object has no attribute`（一条无用信息）而不是带建议的迁移错误。所以必须显式定义一个会抛 `AttributeError` 的类方法。

**`REMOVED` 与 `ALTERNATIVES` 的分工。** `REMOVED` 是一组**没有**直接替代的方法名（`urls`、`screenshots`、`action_history`、`model_thoughts`、`save_to_file` 等），它们的错误文案统一落到 `HISTORY_ALTERNATIVE`：

> the agent runs Python cells, not step actions: use `final_result()`, `structured_output`, `errors()`, `.messages`（the full transcript）or the files the agent saved in `agent.workspace`.

`ALTERNATIVES` 只给**有**直接替代的几个：`model_dump` → `dataclasses.asdict(result)`、`model_dump_json` → `final_result()`、`total_input_tokens` / `input_token_usage` → `result.usage['input']`、`history` → `result.messages` + `number_of_steps()`、`is_judged` / `is_validated` → `result.judgement`（未判定时为 `None`）。

`__getattr__` 只有三行，但分流清楚：

```python
if name in REMOVED:
    raise AttributeError(removed(f'AgentHistoryList.{name}', ALTERNATIVES.get(name, HISTORY_ALTERNATIVE)))
raise AttributeError(f'{type(self).__name__!r} object has no attribute {name!r}')
```

注意 `model_dump` 同时在 `REMOVED` 与 `ALTERNATIVES` 里——`REMOVED` 决定「要不要报迁移错」，`ALTERNATIVES` 决定「报什么」。两个集合职责分离，所以一个名字可以只改文案而不改分类。

`errors()` 与 `has_errors()` 的语义也被 1.0 的循环结构改了。docstring：

> The error that ended the run, if any. Unlike 0.x（one entry per step）, cell errors the agent recovered from are not listed: they are in `.messages`（the python tool results）.

即 `errors()` 只返回**终止运行的那个错误**，最多一项。单元格内部被模型修好的错误不算——它们在对话记录里。这是一个重要的语义收窄：0.x 的 `errors()` 是「每一步的错误清单」，1.0 的是「致命错误」。若不改，调用方会以为运行有多个失败。

**`Usage` 是 `dict` 子类而非 dataclass。** 它继承 `dict`，键是 `input` / `output` / `cache_read` / `cache_write` / `requests`，而 0.x 的 `UsageSummary` 属性名（`total_tokens` 等）通过 `__getattr__` 拦下来，报出对应的 1.0 表达式。七条映射里有几条值得看：

- `total_cost` → `result.cost_usd.`
- `total_tokens` → `result.usage['input'] + result.usage['output'].`
- `by_model` → `one model runs the whole task: result.usage.`

最后这条揭示了一个结构简化：0.x 一次运行可能换模型（用于不同步骤），所以用量按模型分组；1.0 一次运行一个模型，分组没有意义。文案把这件事直接说出来，而不是报个 `AttributeError` 让用户猜。

注意映射的值是**带句点的字符串**（`"result.usage['input']."`），拼接进 `removed()` 生成一句完整的话。`total_cost` 那条甚至指向一个属性而不是表达式——因为成本不在 `usage` 里。

`__post_init__` 里 `self.usage = Usage(self.usage)` 把传进来的普通 dict 转成 `Usage`，于是属性访问拦截对任何来源都生效。

**`partial` 与 `downloads` 是两个「副作用出口」。** `partial` 的注释：「last `checkpoint(..., partial=True)` when unfinished」——即模型在运行中途若用 `checkpoint(..., partial=True)` 存过兜底结果，未完成时这里就有值。这解释了为什么 worker 的 `checkpoint` 要支持 `partial` 参数并写 `state/partial.json`：它是**跨进程传递的**，最终落到这个字段。

`downloads` 的类型是 `list[Path]`，注释：「files this run downloaded, in `agent.workspace/downloads`」。用 `Path` 而不是 URL 或记录，说明这里关心的是「文件在哪儿」，这与 `Browser.list_downloads` 的记录形态不同。

**`judgement` 是一个可选的前向引用。** 类型是 `Judgement | None`，而 `Judgement` 只在 `TYPE_CHECKING` 里导入——所以这是字符串注解（文件顶部有 `from __future__ import annotations`）。注释写「the judge's verdict, labels and gotchas（`Agent(judge=True)`, the default）; None when off, not run or failed」。三种 `None` 的原因被明确列出，因为调用方需要区分「没开」与「开了但失败」。

**模块级的两处接线与 tools.py 同源。** `ActionResult` 从 `browser_use.tools` 导入并放进 `__all__`，注释「0.x imported it from here」——即 0.x 的用户写 `from browser_use.agent.views import ActionResult`，1.0 要保住这个 import 路径。末尾的 `__getattr__ = removed_names(__name__)` 同样用 `if not TYPE_CHECKING` 包住，理由与 tools.py 一致（「pyright would accept any name imported from here」）。

## 关键取舍

**源码事实：** `status` 有四种取值，其中 `completed` 表示「有交付」（即使撞上步数上限或超时也先强制交付一次），`max_steps` / `timeout` 表示「无交付，读 `partial`」。

**源码事实：** `cost_usd` 只含模型与网页搜索费用，云端浏览器与代理另行计费；`None` 表示模型价格未知而非免费。

**源码事实：** `__post_init__` 在 `status != 'completed'` 时强制把 `success` 置 `None`，杜绝「崩溃但成功」的矛盾状态。

**源码事实：** `structured_output` 在声明了 `output_schema` 时按「schema 是不是 `str`」判断，避免把 `Literal` / `StrEnum` 这类「结构化但输出是字符串」的情况误判。

**源码事实：** `__len__` 抛 `TypeError`，因此必须覆写 `__bool__`，否则 `if result:` 会被连带炸掉。

**源码事实：** `load_from_file` 显式定义为会抛错的 `classmethod`，因为类属性查找不走实例的 `__getattr__`。

**源码事实：** `errors()` 只返回结束运行的那个错误，恢复过的单元格错误留在 `.messages` 里。

**架构推论：** 这个类的复杂度几乎全部来自「保留旧方法名但不保留旧语义」。它给出的解法是**把差异写成一句可执行的话**：不是「不支持」，而是「用 `result.messages` 与 `number_of_steps()`」；不是「行为变了」，而是「0.x 的默认值等价于 `is_done() and success is not False`」。这类文案比抛错本身更有价值，因为迁移的人真正需要的是下一步。

**架构推论：** 「字段用新形状、方法名用旧名字」这个分裂是有意的。若把字段也做成 0.x 的形状（例如保留 `history` 列表），就要在 1.0 里维护一份假的历史，而那个历史与 `.messages` 会长期不一致。所以宁可让字段直白（12 个扁平字段），让方法名承担兼容。

**架构推论：** `__repr__` 里排除 `messages` 这条注释（「about 565 KB」）揭示了一个 agent 框架特有的问题：**运行结果对象天然携带巨量上下文**。任何日志、打印、调试都会撞上它。这里的处理是在 `repr` 里换成计数，但字段本身仍完整——需要细节的人显式去取 `.messages`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/agent/views.py` | `AgentHistoryList` | 运行结果：12 个扁平字段 + 0.x 方法名兼容层 |
| `browser_use/agent/views.py` | `final_result` | 完成时返回文本，否则 `None` |
| `browser_use/agent/views.py` | `structured_output` | 声明了 schema 时按 schema 类型判断结构化与否 |
| `browser_use/agent/views.py` | `is_successful` | 模型判定；`None` 表示没有判定，并给出 0.x 等价表达式 |
| `browser_use/agent/views.py` | `is_done` | 仅表示运行完成 |
| `browser_use/agent/views.py` | `errors` / `has_errors` | 只含终止运行的那个错误 |
| `browser_use/agent/views.py` | `__len__` / `__bool__` / `__repr__` | 三处协议方法：抛错、常量真值、去掉转录的打印形态 |
| `browser_use/agent/views.py` | `load_from_file` | 显式类方法，让类级调用也走迁移错误 |
| `browser_use/agent/views.py` | `Usage` | `dict` 子类 + 0.x 属性的替代表达式映射 |
| `browser_use/agent/views.py` | `REMOVED` / `ALTERNATIVES` / `HISTORY_ALTERNATIVE` | 无替代 / 有替代 / 统一兜底的三层文案表 |
| `browser_use/agent/views.py` | `ActionResult` | 从 `tools.py` 再导出，保住 0.x 的 import 路径 |

## 参考

- [browser-use 源码：agent/views.py](https://github.com/browser-use/browser-use/blob/main/browser_use/agent/views.py)
- [browser-use 仓库：MIGRATING.md](https://github.com/browser-use/browser-use/blob/main/MIGRATING.md)
- [Python 文档：`__getattr__` 与数据模型](https://docs.python.org/3/reference/datamodel.html#object.__getattr__)
- [Python 文档：真值测试与 `__len__`](https://docs.python.org/3/reference/datamodel.html#object.__len__)
- [Pydantic AI 文档：结果与消息](https://ai.pydantic.dev/results/)

## 与主核 / AI 的关系

「一个结果对象同时要给出判定、代价、可追溯的细节」与主核里的**查询执行结果**是同构的：一次 `EXPLAIN ANALYZE` 之后，用户既想看「成功与否」，也想看「实际行数、耗时、扫描方式」，还想看原始的详细输出。主核的处理方式是把这些分成「结果集的元信息」与「诊断输出」两部分；这里则全塞进一个 dataclass，靠 `repr` 把巨量的 `messages` 排除在外。

`status` 的四分法在主核里有类似物：`COMMIT` / `ROLLBACK` / `ABORT`，以及「事务还在但语句失败」这种中间态。共同的需求是**让调用方不必组合多个字段就能判断下一步做什么**。这里的 `completed` vs `max_steps` 的分界（有没有交付）与主核「事务提交了没」的分界属于同一类判断——它决定调用方该读哪个字段。

`is_successful()` 那段关于 0.x 默认值的注释，是最能体现兼容层成熟度的一处。它不只是说「行为变了」，而是给出等价表达式 `is_done() and success is not False`。这种做法在主核的版本升级说明里也很常见：与其说「这个 GUC 被移除了」，不如说「用这个新参数配成这个值等效」。

对 AI 工程的实际意义在于：agent 的结果对象比一般 API 的返回值难设计，因为它同时是**给人看的摘要**、**给程序用的结构**、与**给排障用的记录**。这份实现的三条经验值得直接抄：把「有没有交付」做成单一权威字段（`status`）；让「成功」成为模型的主动声明而不是推断（`success: bool | None`）；以及把巨量上下文（`.messages`）从打印路径里摘出去但保留在字段上。最后，`cost_usd` 只覆盖一部分费用这一点，必须在字段注释里写清——把不完整的成本叫成「成本」而不加限定，是这类框架最容易误导用户的地方。
