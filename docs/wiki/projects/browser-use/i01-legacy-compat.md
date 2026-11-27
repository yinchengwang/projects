---
project: browser-use
feature: legacy-compat
title: 0.x 兼容层与已移除名称
page_type: feature
schema_version: 1
refs:
  - path: browser_use/legacy.py
    symbols: [removed_name, removed_module, install]
  - path: browser_use/tools.py
    symbols: [RemovedField, _no_removed_fields]
---

# 0.x 兼容层与已移除名称

## 一句话职责

把「升级报错」做成产品设计：0.x 的每个模块路径与每个被移除的名字都不再给出 `ImportError: cannot import name` 或 `No module named`，而是给出**一条指名 1.0 答案的迁移指引**——包括 `from browser_use import dom` 这种模块路径、`llm.openai_gpt_4o` 这种动态属性、以及 `AgentOutput` 这种「模块还在但类型没了」的中间地带；同时 `RemovedField` 在构造期拦截已移除的字段。

## 机制是什么

五个入口，覆盖 0.x 名称的四种存在形态：

| 0.x 形态 | 拦截点 | 抛什么 |
|---|---|---|
| 整个模块消失（`browser_use.dom`、`browser_use.actor`） | `sys.meta_path` 末端的 finder | `ModuleNotFoundError`（带迁移文案） |
| 模块还在、名字没了（`agent.views.AgentOutput`） | 模块的 `__getattr__` = `removed_names(__name__)` | `ImportError` 或 `AttributeError`（按调用形态） |
| 子模块被当名字导入（`from browser_use.browser import session`） | `__getattr__` 里额外的 `_removed_submodule` 探测 | `ModuleNotFoundError` |
| 动态生成的属性（`llm.openai_gpt_4o`） | `__getattr__` 里的正则匹配 | `ImportError` 或 `AttributeError` |
| 构造参数（`Agent(browser_profile=...)`） | `RemovedField` 描述符 | `TypeError`（在浏览器启动之前） |

`legacy.py` 的模块 docstring 只有两句，但把分工写清了：**模块用一个放在 `sys.meta_path` 末端的 finder，于是它只看到没有任何真实模块应答的路径；而 1.0 保留的模块里的名字，由该模块的 `__getattr__` 调 `removed_name()`**。

## 怎么实现的

**两个数据表 + 一个匹配器，不用 if 链。** `_MODULES` 是「0.x 模块路径 → 1.0 答案」（例如 `browser_use.dom` → `the agent drives the page with Python and raw CDP in its REPL; there is no DOM, Page or Element API.`），`_NAMES` 是「模块 → {名字 → 答案}」，`BROWSER_MEMBERS` 是第三个：0.x `BrowserSession`/`BrowserContext`/`Browser` 上所有 1.0 的 `Browser` 没有的公开成员（约 110 个）。

这三张表的价值在于**它们是从 0.x 的各 tag 里抄出来的清单**，而不是靠回忆挑几个常见的。`BROWSER_MEMBERS` 上方的注释写明了来源：**0.1 到 0.13 的每一个公开方法与属性（从 tags 取的）**。也就是说这份清单的完备性有据可查。

**文案是「答案」而不是「道歉」。** 每个 value 都是一句可执行的话，例如 `_BROWSER = 'from browser_use import Browser; pass options to Browser(...) (BrowserSession and BrowserProfile are gone).'`、`_LOOP = 'the 0.x loop internals are gone: the model writes Python cells, result.messages holds the transcript.'`。这是本页最值得学的部分：**报错文本的成败在于它是否给出下一步动作**，而不在于它是否解释了原因。

**`removed_name` 只在「from-import」场景抛 `ImportError`，其余抛 `AttributeError`。** 这个区分写在 docstring 里，理由非常具体：

- 一个 from-import 里抛 `AttributeError` 会被读成一句干巴巴的 `cannot import name`，于是**精心写的迁移文案就丢了**；
- 而其余场景（`getattr`、`hasattr`、属性访问）抛 `ImportError` 会让 `hasattr()` 与 `getattr(module, name, default)` **无法捕获**——这些内建只吞 `AttributeError`。docstring 举了实际后果：`getattr(browser_use, 'BrowserSession', None)` 这种**特征探测**会直接抛出而不是走 fallback，于是那些「有就用、没有就降级」的代码全崩。

所以实现是：先按 `_removed_name` 得到 `ImportError`（因为文案构造在那里统一），再判断「这次是不是 from-import」，若不是就把它包装成 `AttributeError(str(error), name=name, obj=sys.modules.get(module))`——**文案完全相同，只是类型换掉**。

**判断「是不是 from-import」靠字节码与栈帧，这是全篇最硬的一处。** `_from_import()` 做两步：

1. **栈帧回溯**：从 `sys._getframe(1)` 开始，跳过 `removed_name` 与 `__getattr__` 这两个函数帧（用 `frame.f_code.co_name` 判断，并要求 `frame.f_globals['__name__']` 以 `browser_use` 开头，避免误跳别人的同名函数）。
2. 判定：若栈帧是 `importlib._bootstrap` 的 `_handle_fromlist`，**是** from-import（这是 `from package import name` 走的路径）；否则检查**当前指令是不是 `IMPORT_FROM`**（`frame.f_code.co_code[frame.f_lasti] == _IMPORT_FROM`，其中 `_IMPORT_FROM = opmap['IMPORT_FROM']`）——这是 `from module import name` 走的 `IMPORT_FROM` 指令。

这段代码脆弱吗？是的——它依赖 CPython 的字节码布局与 importlib 的内部函数名。但它是**唯一能区分两种调用形态的手段**，因为 Python 没有给 `__getattr__` 传「调用上下文」。而收益很实：迁移文案在 where 场景都能到用户眼前，同时不破坏特征探测。

**`_removed_submodule` 处理的是「子模块被当名字导入」这个陷阱。** 注释说明了机制：**`from browser_use.browser import session` 时 from-import 会先问包的 `__getattr__`，再去导入子模块；而 finder 对缺失子模块抛的 `ModuleNotFoundError` 会被吞掉，变成一句 `cannot import name 'session'`**。

所以这里主动探测：若 `fullname` 在 `_MODULES` 里（说明它真的是 0.x 模块），且 `PathFinder.find_spec` 找不到真实模块，就返回迁移文案（由 `removed_module` 生成）。反过来，若 1.0 在该前缀下**有真实模块**（注释里举了 `browser_use.tools`），就返回 `None` 让 import 系统正常加载。

**`removed_module` 用「最长前缀匹配」。** 它把路径按 `.` 切开，从最长到最短逐个查 `_MODULES`。于是 `browser_use.browser.watchdogs.screenshot_watchdog` 会命中 `browser_use.browser.watchdogs`（而不是退到 `browser_use.browser`），而注释写明「**longest match wins**」。这个顺序保证「越具体的答案越优先」。

**`_MODULES` 里有一批用 `dict.fromkeys` 展开的批量条目，且注释标了版本。** 例如 0.6.0 的十一个 `*_watchdog` 子模块、0.1.0 的 `browser_use.browser.service`。这些注释说明清单不是一次写成的，而是**按版本积累的**——每次有人从某个老版本升级失败，就往表里加一条。

**`_LLM_INSTANCE` 的正则覆盖了「0.x 按属性访问就建模型实例」的行为。** 注释：**`llm.openai_gpt_4o`、`llm.google_gemini_2_5_flash`……**。正则 `(openai|azure|google|anthropic|mistral|cerebras|bu)_\w+|codestral|pixtral_large|get_llm_by_name` 匹配这些形态，再按前缀查 `_LLM_INSTANCE_ANSWER` 给出对应答案（例如 `openai` → `ChatOpenAI('<model>'), e.g. ChatOpenAI('gpt-4o') for openai_gpt_4o.`）。

这是一个「名字空间即协议」的例子：0.x 把模型目录编码进了属性名，于是迁移指引需要**从属性名反推出类与新名字**，而不是笼统地说「用 ChatOpenAI」。

**`_modules()` 是惰性构建的，注释给了理由。** `_MODULES: dict = {}` 上方写着：**首次使用时构建：它会 import `browser_use`**。而那行 import 是 `_llm_modules()` 里的 `from browser_use import _LLM_REMOVED`——一个循环依赖，只能等到有人真的查询 0.x 名字时才解析。这也是为什么 `removed_name` 内部对 `browser_use.exceptions` 的 import 也写成注释解释的形式：**`this module is imported from modules exceptions imports`**。

**finder 放在 `sys.meta_path` 的**末端**。** `install()` 用 `sys.meta_path.append(...)`。`Removed0xModules.find_spec` 只在 `fullname` 以 `browser_use.` 开头且 `removed_module` 有答案时才抛；否则返回 `None`。类 docstring 补了一句兼容性考虑：**抛的是 `ModuleNotFoundError`（一种 `ImportError`），所以 `except ImportError` 的 fallback 仍然有效**。

**在末端追加而不是插入开头**，意味着真实模块永远优先——它只兜住「没有真实模块应答」的那部分。这与 `_removed_submodule` 里的 `PathFinder.find_spec` 探测是同一个原则的两个实现位置。

**`removed_names(module)` 是一个返回 `__getattr__` 的工厂。** 它被多个模块用（`utils.py`、`skills/__init__.py`、`telemetry/service.py` 末尾都能看到 `__getattr__ = removed_names(__name__)`）。这些模块的共同写法都带 `if not TYPE_CHECKING:` 与一句注释 `pyright would accept any name imported from here`——也就是说**这个动态钩子必须对类型检查器隐藏**，否则任何名字都能通过类型检查。

**`_NAMES` 里有几处答案是空串（`''`）。** 例如 `browser_use.agent` 的 `BetaAgentError`、`browser_use.agent.tests`、`browser_use.sandbox`。空答案意味着 `removed()` 会生成一条只声明「已移除」而没有后续动作的文案。这也是诚实的：**有些名字确实没有对应物**，编一个答案比说「无」更糟。

**`BROWSER_MEMBERS` 是独立的第三张表而不是合并进 `_NAMES`**，因为它的用途不同：它不是按模块查名字，而是被 `browser.py` 作为「1.0 的 `Browser` 没有的成员清单」用于运行期拦截（`browser.py REMOVED_METHODS names the common ones` 这句注释说明了它与另一个表的配合）。

### `RemovedField`：构造期拦截

**`RemovedField` 是一个 `TypeError` 子类而不是描述符。** 它由 `ActionResult` 的 `model_validator(mode='before')`——即 `_no_removed_fields`——在**解析入参时**抛出。docstring 一句话说清了它的定位：**一个 0.x 的 `ActionResult` 字段在 1.0 被去掉；它终止这次运行（绝不会当作工具错误交给模型），于是开发者会看到它**。

也就是说这里有两层设计：字段在模型上不存在（`model_config = ConfigDict(extra='allow')` 让未知字段被接受而不是报校验错），但**一旦出现就抛一个终止运行的异常**，而不是静默吞掉或变成工具错误被模型自己「处理」。

**被移除字段分两档，理由完全不同。** `REMOVED_FIELDS` 是硬拒的四个：

| 0.x 字段 | 1.0 答案 |
|---|---|
| `is_done` | 模型用 `finish` 结束运行 |
| `images` | 返回文本；agent 自己截图 |
| `success` | 模型可在 `finish(..., success=...)` 给判定，结果在 `result.success` |
| `attachments` | 文件写进工作区，agent 写的文件会被交付 |

其中 `images` 那条的注释给出了硬拒的具体原因：**留着它作为额外字段会让它作为 JSON 文本到达模型——对话里出现 base64 图片数据，而从来不是一个图片**。这是「静默接受一个字段」会造成实质性错误的典型例子。

`IGNORED_FIELDS` 是两个只警告不抛的：`include_in_memory` 与 `include_extracted_content_only_once`，答案都是 `every action result stays in the conversation.`。为什么这两档不同？注释写得很好：**一个 action 是在浏览器启动之后才跑的（而云浏览器已经在计费），而 1.0 反正会把每个结果留在对话里**。

也就是说「拒」与「警告后忽略」的分界不是「重要性」而是**「这个字段是否会让用户看到错误的结果」**：`images` 会让 base64 进对话（错误结果），而 `include_in_memory` 只是不再有任何作用（无害）。

**警告只打一次，因为一个 action 每次调用都会返回它。** 代码里 `_warned` 是类级 `set`，同名第二次就不再打印——这个细节避免了一个被频繁调用的 action 把日志刷满。

**`_without_0x_defaults` 处理的是「0.x 习惯写全默认值」这个真实迁移痛点。** docstring：**0.x 的 action 常常把默认值写出来：`ActionResult(extracted_content=..., is_done=False, success=True)`。在 0.x 里这些什么都不改变（`success` 只有配 `is_done=True` 才算），所以它们会白白终止一次已经计费的运行**。

实现规则很精确：若 `is_done` 为真就原样返回（那是真的要结束）；否则**过滤掉 `is_done`、`success`，以及任何假值的已移除字段**。于是 `is_done=False` 与 `success=True` 这两个「无意义的写法」被静默剔除，而 `is_done=True` 仍然照实拒绝。这是一个把「用户实际意图」与「字面写法」分开处理的范例。

**构造期拦截之外的第三层：注册期检查。** `_check_done_results` 的 docstring 说明了它的位置：**一个源码里返回 `ActionResult(is_done=True, ...)` 的 action（0.x 结束运行的方式）只会在运行时失败，而那已经是浏览器启动之后；所以在注册时就直接拒绝它**。实现是 `ast.parse` 该函数的源码并遍历 `ast.Call`，找 `ActionResult` 的调用与其 `is_done` 关键字。

拿不到源码时（REPL 里定义的、编译过的、或 lambda 被 `partial` 包住）就跳过——注释列了这些情形。于是运行期的 `RemovedField` 仍是兜底。

**这条「在注册期而不是运行期失败」的原则与 `AGENTS.md` 的纪律一致**：*0.x options 1.0 cannot honor raise a `TypeError` naming the option before any browser starts.*。两者都是同一条判断：**报错越早，代价越小**——因为浏览器启动可能意味着云资源已经开始计费。

## 关键取舍

**源码事实：** 0.x 模块路径与名字被两张表覆盖，模块表用 `sys.meta_path` 末端 finder，名字表用各模块的 `__getattr__` 工厂。清单来源是 0.x 各 tag 的实际 API，不是挑选出来的常见项。

**源码事实：** 迁移文案是一句可执行的答案，而不是一句道歉；确实没有对应物的名字给空答案而不是编造。

**源码事实：** 同一份文案在 from-import 场景抛 `ImportError`、其余场景抛 `AttributeError`，因为 `hasattr`/`getattr(..., default)` 只吞后者，而特征探测必须能降级。

**源码事实：** 区分两种调用形态靠栈帧回溯 + `IMPORT_FROM` 指令判定（含 `importlib._bootstrap._handle_fromlist` 这个特例）。这是唯一可用的手段，也是本页最脆弱的一处。

**源码事实：** `_removed_submodule` 主动探测「子模块被当名字导入」并把被吞掉的 `ModuleNotFoundError` 换成迁移文案；若该前缀下 1.0 有真实模块则让正常导入进行。

**源码事实：** `removed_module` 最长前缀匹配；finder 追加在 `sys.meta_path` **末端**，真实模块永远优先。

**源码事实：** 模块表的构建是惰性的，因为它需要 `from browser_use import _LLM_REMOVED`——一个循环依赖。

**源码事实：** `RemovedField` 在解析 `ActionResult` 入参时抛 `TypeError`，绝不当作工具错误交给模型；被移除字段分「硬拒」与「警告后忽略」两档，分界是「是否会让用户看到错误结果」而非重要性。

**源码事实：** `_without_0x_defaults` 静默剔除 0.x 习惯写出的无意义写法（`is_done=False`、`success=True`），但 `is_done=True` 照实拒绝——把字面写法与实际意图分开。

**源码事实：** `_check_done_results` 在**注册期**用 `ast` 检查一个 action 是否返回 `is_done=True`，避免失败发生在浏览器已启动、云资源已计费之后；拿不到源码时运行期兜底。

**源码事实：** 仓库纪律要求这类选项报 `TypeError` 且与 `MIGRATING.md` 保持同步——这张表是文档与代码的共同产物。

**架构推论：** 这一层把「升级」当成一个需要设计的**用户旅程**而不是一个需要容忍的兼容期。常见的两种做法都不如它：一是保留旧名（永远清不掉技术债，且旧名会悄悄做错事）；二是直接删（用户看到 `cannot import name 'AgentOutput'`，只能去搜文档）。这里的第三种做法是**用报错本身作为文档的投递渠道**——每条报错都是一条定制的迁移指引，且投递点精确到用户的调用形态。

代价也很真实：需要三张靠人工维护的表、一段读字节码的脆弱代码、以及每个模块末尾的 `__getattr__` 钩子。但这些成本是**一次性的、可测的**，而收益分布在每一次升级上。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/legacy.py` | `removed_name` | 名字级拦截入口，按调用形态选 `ImportError` 或 `AttributeError` |
| `browser_use/legacy.py` | `removed_module` | 模块级拦截，最长前缀匹配返回迁移文案 |
| `browser_use/legacy.py` | `install` | 把 finder 追加到 `sys.meta_path` 末端 |
| `browser_use/legacy.py` | `Removed0xModules` | finder 本体，只处理无真实模块应答的 `browser_use.*` 路径 |
| `browser_use/legacy.py` | `removed_names` | 生成模块 `__getattr__` 的工厂 |
| `browser_use/legacy.py` | `_from_import` | 栈帧 + `IMPORT_FROM` 判定调用形态 |
| `browser_use/legacy.py` | `_removed_submodule` | 子模块被当名字导入时的主动探测 |
| `browser_use/legacy.py` | `_MODULES` / `_NAMES` | 两张答案表，按 0.x 版本积累 |
| `browser_use/legacy.py` | `_LLM_INSTANCE` | 0.x 动态模型属性的正则与按前缀答案 |
| `browser_use/legacy.py` | `BROWSER_MEMBERS` | 0.x `Browser(Profile/Context/Session)` 上已无的约 110 个成员 |
| `browser_use/legacy.py` | `_llm_modules` | 各 provider 子模块到新类名或移除文案的映射 |
| `browser_use/tools.py` | `RemovedField` | 已移除字段的 `TypeError` 子类，在解析 `ActionResult` 入参时抛出 |
| `browser_use/tools.py` | `_no_removed_fields` | `before` 校验器：硬拒硬移除字段、警告后剔除忽略字段 |
| `browser_use/tools.py` | `REMOVED_FIELDS` / `IGNORED_FIELDS` | 两档字段及其 1.0 答案 |
| `browser_use/tools.py` | `_without_0x_defaults` | 剔除 0.x 写全的默认值，保留真正的 `is_done=True` |
| `browser_use/tools.py` | `_check_done_results` | 注册期用 `ast` 拦下会以 `is_done=True` 结束运行的 action |

## 参考

- [browser-use 源码：legacy.py](https://github.com/browser-use/browser-use/blob/main/browser_use/legacy.py)
- [browser-use 仓库：MIGRATING.md](https://github.com/browser-use/browser-use/blob/main/MIGRATING.md)
- [Python 文档：sys.meta_path 与导入协议](https://docs.python.org/3/reference/import.html#finders-and-loaders)
- [Python 文档：模块的 `__getattr__`](https://peps.python.org/pep-0562/)

## 与主核 / AI 的关系

这套兼容层与主核里「废弃配置项与废弃语法的处理」是同一类问题，但方向相反。数据库通常保留旧语法若干版本并打 warning（因为 SQL 文本无法给用户手把手指引），而这里是**直接拒绝并给出替代方案**——适用于「API 层面改名/重构」这种无法同时支持两套的场景。

对 agent 工程的直接价值有三条。其一，**报错是最有效的文档投递点**。用户不会去读迁移指南，但一定会看到报错；把迁移指引塞进报错，等于让文档在需要的时刻自动出现。其二，**报错类型要按调用形态选**。同一个「没有这个属性」在 `from x import y` 与该被忽略的探测之间，语义完全不同——一个该响亮地失败，一个该安静地降级。其三，**迁移表要与文档同步维护**。`RemovedField` 与 `MIGRATING.md` 的同步要求被写进了仓库纪律，因为一旦漂移，用户拿到的指引就是错的——而错误指引比没有指引更伤信任。
