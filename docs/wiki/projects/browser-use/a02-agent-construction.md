---
project: browser-use
feature: agent-construction
title: Agent 构造期校验与参数透传
page_type: feature
schema_version: 1
refs:
  - path: browser_use/agent/service.py
    symbols: [_check_task_arg, _check_task, __getattr__]
  - path: browser_use/agent/service.py
    symbols: [workspace, add_new_task]
---

# Agent 构造期校验与参数透传

## 一句话职责

`Agent.__init__` 把「能不能跑」的判断全部提前到构造期——类型、取值域、互斥参数、以及 0.x 遗留参数的去向——让一个必然失败的配置在**起浏览器之前**就炸掉；`__getattr__` 则接管「访问已移除属性」这类调用，给出 1.0 的替代品而不是一句 `AttributeError`。

## 机制是什么

这一层的核心是一个判断：**哪些错误值得在构造期抛，哪些只该警告。**

源码里的分法很明确，四档：

| 档位 | 手段 | 典型例子 |
|---|---|---|
| 硬拒 | `TypeError` / `ValueError` | `task` 非字符串或空白、`reasoning` 拼错、`browser` 与 `browser_session` 同时给 |
| 告警后继续 | `logger.warning` | 短于 4 字符的敏感值、`DEFAULT_LLM` 环境变量 |
| 静默忽略 | 从 `kwargs` 里 pop 掉 | `IGNORED` 集合里的 11 个 0.x 调参 |
| 抛错但带替代方案 | 自定义消息 | `REMOVED` 那张表里的所有参数与属性 |

判据写在注释里：只有 **「不可能改变一次 run 返回什么」**的 0.x 参数才进 `IGNORED`（注释原话：「warn and run（0.x 脚本经常设它们），其余一律抛」）。而 `use_vision=False` 被从这个集合里单独豁免——它**会**改变行为（截图不再进模型），所以保留。

参数校验的总体取向是「宁可拒绝，也不要跑一次错的」，理由写在多处：「一个拼写错误会启动（并计费）一个浏览器，然后在第一次模型请求上 400」。

## 怎么实现的

**`_check_task_arg` 只有五行，但拦的是最常见的浪费。**

```
def _check_task_arg(task: Any) -> None:
    """Before anything starts: None, '' or a number started (and billed) a browser, then failed."""
    if not isinstance(task, str):
        raise TypeError(f'task must be a str, not {type(task).__name__}')
    if not task.strip():
        raise ValueError('task is empty: say what the agent should do')
```

docstring 直接记录了这条校验存在的理由：历史上 `None`、空串或一个数字都会「启动并计费一个浏览器，然后失败」。两个分支分得也准：类型错是 `TypeError`，内容空是 `ValueError`。

`add_new_task` 复用同一个函数：

```
def add_new_task(self, task: str) -> None:
    """The next run() continues this conversation with a new task."""
    _check_task_arg(task)
    self.task, self._continue = task, True
    self._tasks.append(task)
```

三个动作：校验、把 `_continue` 置真（下一次 `run()` 续接会话）、把任务记进 `_tasks`（遥测用，注释在 `_tasks` 定义处说明「遥测永不发送它们的文本」）。

**`_check_task` 是另一类检查：只警告，不拒绝。**

```
def _check_task(self) -> None:
    if self.browser.use_cloud and (m := LOCAL_TARGET.search(self.task)):
        logger.warning(
            f'The task mentions {m.group()!r}; a cloud browser cannot reach your localhost or private network. '
            "If that is the target, use a local browser: Agent(task, browser=Browser('chromium'))."
        )
```

注释写着「a hint, never a refusal」。判定依据是 `LOCAL_TARGET` 那个正则，它同时覆盖 `localhost`、`127.x`、`0.0.0.0`、RFC1918 三段（`10.x`、`192.168.x`、`172.16..31.x`）、`[::1]` 与裸 `::1`（用前后断言避免匹配到长串里的冒号）、以及 mDNS 的 `.local`。

用正则扫任务文本当然会误报（用户可能只是在报告里提到 `192.168.1.1`），但这正是「警告而非拒绝」的位置——误报的代价是一条日志，漏报的代价是一次跑到一半才发现端点不可达的云端 run。

**`__init__` 的校验顺序本身就是信息。**

前几行处理 0.x 别名与忽略参数：

```
judge = kwargs.pop('use_judge', judge)
judge_context = kwargs.pop('ground_truth', judge_context)
self.use_vision = kwargs.get('use_vision') is not False
for name in [n for n in kwargs if n in IGNORED]:
    hint = REMOVED[name]
    if (name, kwargs.pop(name)) != ('use_vision', False):
        logger.warning(f'Agent({name}=...) is ignored in browser-use 1.0' + (f': {hint}' if hint else '.'))
if kwargs:
    raise _unsupported(kwargs, 'Agent')
```

四个细节：`use_judge` 与 `ground_truth` 是**静默改名**（不告警，因为语义没变）；`use_vision` 的值先被读出来存成 `self.use_vision`，再参与「是否告警」的判定——`(name, value) != ('use_vision', False)` 这个元组比较的意思是「除非恰好是 `use_vision=False`，否则都告警」；`IGNORED` 里所有键都 pop 掉，`REMOVED` 里剩下的键则留给最后那个 `if kwargs: raise`。

`_unsupported` 有三条分支，第一条是关于 `max_steps` 的特判：

```
def _unsupported(kwargs, where):
    name = next(iter(kwargs))
    if name == 'max_steps':
        return TypeError(f'{where}(max_steps=...) is not an option: pass it to agent.run(max_steps=...).')
    if name in REMOVED:
        return TypeError(removed(f'{where}({name}=...)', REMOVED[name]))
    return TypeError(f'{where}({name}=...) is not a browser-use 1.0 option; MIGRATING.md lists ...')
```

三条消息的质量递增：`max_steps` 给出确切的新位置、`REMOVED` 里有的给出映射的替代品、完全未知的指向文档。而且它**一次只报一个**（`next(iter(kwargs))`）——这比一次报十个更符合「修一个再跑一次」的实际节奏，也避免了消息里塞一张表。

然后是类型与取值域的连串检查，每条都带一个「否则会发生什么」的理由：

```
_check_task_arg(task)
if not isinstance(web_search, bool): raise TypeError(...)
if not isinstance(highlight_actions, bool): raise TypeError(...)
if not isinstance(judge, bool): raise TypeError(...)
if judge_context is not None and not isinstance(judge_context, str): raise TypeError(...)
if max_context_chars is not None and (type(max_context_chars) is not int or max_context_chars < 1): raise ValueError(...)
if llm is None and os.environ.get('DEFAULT_LLM'): logger.warning('DEFAULT_LLM is set but browser-use 1.0 does not read it: this run uses bu-4.0. ...')
if reasoning not in REASONING: raise ValueError("reasoning must be 'low', 'medium', 'high' or None, not ...")
if browser and browser_session: raise ValueError('Pass browser=... or browser_session=..., not both')
browser = browser or browser_session or Browser()
if not isinstance(browser, Browser): raise TypeError(...)
tools = tools or controller
if tools is not None and not isinstance(tools, Tools): raise TypeError(...)
if sensitive_data is not None and not isinstance(sensitive_data, dict): raise TypeError(...)
```

`max_context_chars` 用的是 `type(x) is not int` 而不是 `isinstance`——又是 `bool` 那条陷阱（`True` 是 `int`）。`reasoning not in REASONING` 后面注释说明理由：「一个拼写错误会启动（并计费）一个浏览器，然后在第一次模型请求上 400」。

`browser` 与 `browser_session` 的互斥检查放在「取默认值」之前，所以「两个都给」与「都不给」两条路径是分开的：前者报错，后者造一个默认 `Browser()`（注释说明默认是 Browser Use 云端浏览器）。

**`output_model_schema` 的校验是一处「提前一次 pydantic 报错」。**

```
self.output_model_schema = output_model_schema or str
try:  # a JSON-schema dict or an instance failed only in run(), with a raw pydantic SchemaError
    TypeAdapter(self.output_model_schema).json_schema()
except Exception as e:
    raise TypeError(
        'output_model_schema must be a type (a Pydantic model, list[Model], Literal[...]), not '
        f'{output_model_schema!r:.100}. For a JSON-schema dict use pydantic_ai.StructuredDict(schema).'
    ) from e
```

注释点明动机：一个 JSON-schema 字典或一个实例**本来只会在 `run()` 里失败**，并且是裸的 `SchemaError`。现在改成构造期抛 `TypeError`，同时给出正确做法（`StructuredDict`）。`!r:.100` 截断的是 `repr`——防止一个巨大的模型实例把错误消息撑爆。

**敏感值的两个构造期动作。**

```
short = sorted({n for n, v, _ in scoped_secrets(sensitive_data) if 0 < len(v) < SHORT_SECRET})  # raises on non-str
if short:
    logger.warning(f'sensitive_data {", ".join(short)}: values under {SHORT_SECRET} characters are redacted only where they stand alone, ...')
```

它调用了 `scoped_secrets`（c03 页讲过），后者的作用之一是**归一化并顺带校验**——注释写「raises on non-str」，即非字符串的值在这里就会抛。所以这一行同时干了三件事：归一化、类型校验、以及找出过短的值来警告。

`self.sensitive_data = dict(sensitive_data or {})` 是拷贝而不是引用——避免调用方之后修改字典影响到 agent。

**`mode` 与 `reasoning` 的默认值推导。**

```
fast = gateway_alias(self.llm) in ('bu-fast', 'bu-ultrafast')
self.mode = check_mode(mode if mode is not None else 'ultrafast' if fast else 'default')
```

注释给了实测理由：「`bu-fast` 与 `bu-ultrafast` 在 `ultrafast` 模式下评测（Google Flights：default 模式 0/3，ultrafast 3/3）」。也就是说模型别名决定了默认提示集，而这个默认是被 bench 数据钉住的。

**`workspace` 属性是「延迟创建、不自动删除」。**

```
@property
def workspace(self) -> Path:
    """Default: a new temp dir, made on first use (an Agent that never runs leaves none) and kept after the run
    (the result's files, screenshots, cell output): the caller deletes it (MIGRATING.md). Not deleted on close():
    delivered files and links in the result point into it."""
    if self._workspace is None:
        self._workspace = Path(tempfile.mkdtemp(prefix='browser-use-')).absolute()
    return self._workspace
```

三条语义都在 docstring 里：**延迟创建**（从没跑过的 Agent 不留目录）、**跑完保留**（结果里的文件与链接指向它，所以不能删）、**`close()` 也不删**（同上）。

`__init__` 里对应的那行注释解释了另一个细节：

```
# As given, made absolute, symlinks kept: on macOS resolve() turned tempfile's /var/... into /private/var/..., and
# result.downloads[0].relative_to(workspace) raised ValueError. Default: made on first use (workspace).
self._workspace = Path(workspace).expanduser().absolute() if workspace else None
```

用 `.absolute()` 而**不用** `.resolve()`：macOS 上 `resolve()` 会把 `tempfile` 给的 `/var/...` 变成 `/private/var/...`，于是后续 `result.downloads[0].relative_to(workspace)` 抛 `ValueError`（两者成了不同的前缀）。这是一个只有真跑过才发现的坑，而被写进了注释。

**`__getattr__` 是「移除名称」的第三个入口。**

```
def __getattr__(self, name: str) -> Any:
    if name in REMOVED_METHODS:
        raise AttributeError(removed(f'Agent.{name}()', REMOVED_METHODS[name]))
    if name in REMOVED_ATTRS:
        raise AttributeError(removed(f'Agent.{name}', REMOVED_ATTRS[name]))
    raise AttributeError(f'{type(self).__name__!r} object has no attribute {name!r}')
```

方法名与属性名分两张表，因为报错文案要区分 `Agent.step()` 与 `Agent.state`。`REMOVED_METHODS` 里有一批「没有 1.0 替代」的项映射到空串（`resume`、`detect_variables`），此时 `removed(...)` 大概会退回通用文案。

注意 `__getattr__` 只在正常查找失败后被调用——所以 `self._repl`、`self.task` 这些真字段不受影响。这条也是它能安全存在的全部原因。

模块级还有一层同名机制，在文件末尾：

```
if not TYPE_CHECKING:  # pyright would accept any name imported from here
    from browser_use.legacy import removed_names
    __getattr__ = removed_names(__name__)  # a 0.x name raises the removed error naming its 1.0 answer
```

即模块级的 `from browser_use.agent.service import AgentConfig` 之类也会得到同样的引导。`if not TYPE_CHECKING` 的理由写在注释里：pyright 会接受从这里导入的任意名字，因此不能让它看到这个定义。

**`_config` 是构造期的另一面：它把「能上报什么」也做了白名单。**

虽然它服务遥测，但它体现了同一种纪律：`_config` 的 docstring 写着「一次 run 的配置字段：计数、布尔、枚举与名字，**绝不是值或路径**」。于是里面有 `sensitive_data_count`、`sensitive_data_domain_scoped` 这样的聚合，有 `browser_exe` 这种「把可执行文件路径归成 edge/chromium/chrome/other 四类」，以及 `cdp_url: cdp_category(host(browser.cdp_url))`。同一个「不让原始值外流」的思路，在构造期就已经落地为可上报字段的设计。

## 关键取舍

**源码事实：** 校验分四档：硬拒、告警后继续、静默忽略（pop 掉）、抛错并给替代方案。只有「不可能改变 run 结果」的 0.x 参数进忽略集合，`use_vision=False` 因此被豁免。

**源码事实：** `_check_task_arg` 的 docstring 记录了它存在的原因：历史上 `None`、空串或数字都会「启动并计费一个浏览器，然后失败」。

**源码事实：** `_check_task` 对「任务文本提到本机地址而浏览器是云端」只警告不拒绝，注释写「a hint, never a refusal」。

**源码事实：** `bool` 被单独防了两次：`max_steps`（在 `run()` 里）与 `max_context_chars`（用 `type(x) is not int`）。

**源码事实：** `output_model_schema` 在构造期就跑一次 `TypeAdapter(...).json_schema()`，把一个「只会在 run 里以裸 `SchemaError` 失败」的错误提前成带指引的 `TypeError`。

**源码事实：** `workspace` 用 `.absolute()` 而非 `.resolve()`，因为 macOS 上 resolve 会改变 `/var` 前缀并让后续 `relative_to` 抛错。

**源码事实：** `workspace` 延迟创建、跑完保留、`close()` 也不删——因为结果里的文件与链接指向它。

**源码事实：** 已移除的名称有三个入口：构造参数（`_unsupported`）、实例属性（`__getattr__`）、模块级名称（`removed_names`，且用 `if not TYPE_CHECKING` 把它对类型检查器隐藏）。

**架构推论：** 「构造期尽量拒」的收益是可预期的（不浪费一次浏览器启动），代价是 `__init__` 变成一段六十多行的线性校验。这类代码没有抽象价值，但它的**顺序与档位**有信息价值：每一档对应一种「谁该为这个错误负责」的判断——调用方写错了（拒）、调用方可能不懂新版本（告警 + 指引）、旧代码习惯（静默）、名字已不存在（抛并给替代品）。

**架构推论：** 用 `__getattr__` 承接已移除属性，把「迁移期的一次崩溃」变成「一次带说明的崩溃」。这比在文档里写迁移表有效得多，因为错误发生在用户真正需要答案的那一刻。代价是 `__getattr__` 里的每一条都必须维护——表过期了就会给出错误的替代建议。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/agent/service.py` | `Agent.__init__` | 四档校验：硬拒、告警、忽略、抛错带替代 |
| `browser_use/agent/service.py` | `_check_task_arg` | 阻塞「必然失败的任务」在起浏览器之前 |
| `browser_use/agent/service.py` | `_check_task` | 云端浏览器 + 本机地址的提示（只警告） |
| `browser_use/agent/service.py` | `_unsupported` | 未知/已移除构造参数的三种报错文案 |
| `browser_use/agent/service.py` | `Agent.__getattr__` | 已移除属性与方法的引导式报错 |
| `browser_use/agent/service.py` | `Agent.workspace` | 延迟创建、跑后保留、不随 `close()` 删除的工作区 |
| `browser_use/agent/service.py` | `Agent.add_new_task` | 续跑任务入口，复用同一套任务校验 |

## 参考

- [browser-use 源码：agent/service.py](https://github.com/browser-use/browser-use/blob/main/browser_use/agent/service.py)
- [browser-use 仓库：MIGRATING.md](https://github.com/browser-use/browser-use/blob/main/MIGRATING.md)
- [browser-use 文档：配置](https://docs.browser-use.com/customize/configuration)
- [Pydantic 文档：TypeAdapter](https://docs.pydantic.dev/latest/concepts/type_adapter/)

## 与主核 / AI 的关系

「把能静态判断的错误全部挤到构造期」是主核里参数解析层的标准做法：一条 SQL 的语法错在解析期报，不该等到执行到一半。差别在于主核的代价是「浪费一次执行」，这里的代价是「启动一个按分钟计费的浏览器」，所以同一条纪律在这里的收益被放大了几十倍。

对 AI 工程的实际意义有两条。第一，**「忽略」是一个必须精确划定的档位**。`IGNORED` 集合的判据是「不可能改变 run 返回什么」，而 `use_vision=False` 因为会改变（截图不进模型）被豁免。这个判据比「尽量兼容」更严格也更可维护：一旦某个参数被误放进忽略集合，用户会得到一个「设了但完全没效果」的静默行为，那比直接报错难查得多。

第二，**已移除名称的处理是一种设计资源**。三个入口（构造参数、实例属性、模块级名字）覆盖了用户可能撞上旧 API 的所有方式，而每条错误消息都指向 1.0 的替代品。这比维护一份「迁移指南 + 一个通用 `TypeError`」有效得多——用户不会去读迁移指南，但一定会读异常消息。代价是这张表必须与实现同步，否则会给出错误的指引。
