---
project: browser-use
feature: tools-and-actions
title: 工具注册与动作结果
page_type: feature
schema_version: 1
refs:
  - path: browser_use/tools.py
    symbols: [Tools, action, ActionResult]
  - path: browser_use/tools.py
    symbols: [_check_done_results, _without_0x_defaults]
---

# 工具注册与动作结果

## 一句话职责

`Tools` 是模型除 Python 之外的第二条调用通道：用 `@tools.action('描述')` 把普通函数登记成 Pydantic AI 工具，让模型可以按名字调用自定义动作；而 `ActionResult` 与一整套 `REMOVED_FIELDS` 表则专门负责**把 0.x 的写法在启动前翻译成清晰的错误**，而不是让它在一次计费运行中途炸掉。

## 机制是什么

这个模块有三层，用途差别很大：

| 层 | 面向谁 | 手段 |
|---|---|---|
| 注册与校验 | 1.0 使用者 | `@tools.action(...)` + 六道注册期检查 |
| 0.x 兼容拒绝 | 0.x 迁移者 | 四张 `*_REMOVED` 映射表 + `removed()` 文案 |
| 返回值形状 | 0.x 的 action | `ActionResult` 模型 + 两个 `mode='before'` 的宽容处理 |

第一层的目标是「**在注册时就把将来必然失败的写法拒掉**」。这一动机在 `_check_done_results` 的 docstring 里写得最清楚：一个在源码里返回 `ActionResult(is_done=True, ...)` 的动作，「can only fail when it runs, after the browser started」——即要等到浏览器已经起来（云端浏览器已开始计费）才炸。所以要在注册期用 AST 静态查出来。

第二层是一种**迁移工程**：把「已经不存在的东西」变成有用的报错。四张表分别是 `Tools(...)` 的参数、`Tools` 的方法、`@tools.action(...)` 的参数、以及 `ActionResult` 的字段——每一行的值是「1.0 里该怎么做」。

## 怎么实现的

**`action` 装饰器的六道检查，顺序即优先级。** 每一道都对应一个真实误用：

1. **`callable(description)`**：`@tools.action` 不带括号时，`description` 收到的是函数本身。错误文案直接说「Use `@tools.action('what the action does')` with parentheses; `@tools.action` alone registers nothing」——因为不带括号时装饰器返回的是 `decorator` 函数，原函数被替换掉，等于什么都没注册。
2. **`domains` / `allowed_domains`**：0.x 的「动作级域名限制」在 1.0 里没有对应物，替代方案是 `Browser(allowed_domains=...)` 限制整个浏览器。这里直接把参数名报出来并给方向。
3. **其他 kwargs**：查 `ACTION_REMOVED` 表，命中则给对应建议（`page_filter`、`requires_browser`、`terminates_sequence`），未命中则说「不是 1.0 选项，见 MIGRATING.md」。
4. **`INJECTED` 参数检测**：0.x 会把 `browser_session`、`context`、`cdp_client`、`page_url` 等九个名字注入动作。1.0 的动作**只拿到模型传的参数**，所以函数签名里出现这些名字就是错的。文案说得更狠：「the model drives the browser itself in Python」——即这类需求在 1.0 里压根不该由动作承担。
5. **`RESERVED` 与 `TOOL_NAME`**：`RESERVED = {'python', 'finish', 'finish_from_repl'}` 是三个内置工具名，重名会覆盖内置工具，所以直接 `ValueError`。`TOOL_NAME = re.compile(r'[a-zA-Z0-9_-]{1,64}')` 的注释给了具体依据：「what OpenAI, Anthropic and Google accept」；并说明为什么要自己查——「a lambda（`'<lambda>'`）failed only at the provider, with a 400」。把一个供应商端的 400 提前成本地 `ValueError`，是这类校验的典型价值。
6. **`_check_done_results(fn)`**：AST 静态扫描，见下。

检查完了才真正登记。登记时有两处兼容处理：

- **`param_model` 回填**：0.x 允许 `def action(params, ...)` 配 `param_model=Model`，1.0 支持这个形态的方式是「把第一个参数的注解改写成 `param_model`」（`fn.__annotations__[params[0]] = param_model`）。注释写明「it wins」——显式传的 `param_model` 覆盖原有注解。
- **同名替换**：`self.functions = [t for t in self.functions if t.name != fn.__name__]` 之后再 append。注释解释了必要性：「0.x kept actions by name: registering one again（a notebook cell run twice）replaces it」。在 notebook 里反复执行同一个 cell 会重复注册，若不去重就会有两个同名工具传给供应商。

最后构造的是 Pydantic AI 的 `Tool`：`Tool(fn, takes_ctx=False, name=fn.__name__, description=description or None, max_retries=TOOL_RETRIES)`。注意 `takes_ctx=False`——与上面第 4 道检查同一条原则的落地。

`TOOL_RETRIES = 1_000_000` 这个数字很反常，注释给了完整理由：

> bu-pi returns the error and goes on; `max_steps` bounds it. A budget of 1 ended the run on two bad python calls in a row（a truncated cell, a JSON slip）.

即重试预算实际上是「不设限」，靠 `max_steps` 兜住。理由是重试计数是**按工具**还是**按全局**会改变语义：若一个动作的两次失败就终止整个运行，那么模型修 bug 的正常流程会被误杀。这是一条从实际失败里调出来的参数。

**`_check_done_results` 的 AST 判定。** 它 `ast.parse(textwrap.dedent(inspect.getsource(fn)))`，拿不到源码就返回（注释点名「REPL, compiled, or a lambda's partial line」——这些场景无法静态分析，运行期仍会抛）。

然后 `ast.walk` 找所有 `ActionResult(...)` 调用，逐个关键字判两类：

- `done = kw.arg == 'is_done' and not _empty_literal(kw.value)`
- `listed = kw.arg in ('attachments', 'images') and not _empty_literal(kw.value)`

`kw.arg is None`（`ActionResult(**fields)`）直接 `continue`——展开写法看不出键，不能误判。

`_empty_literal` 是一个小函数，判 `None`/`False`/`0`/`''`/空容器这些「写了等于没写」的字面量。注释在 `listed` 那行解释了它的作用：「an empty list is the 0.x default spelled out: `ActionResult` drops it at runtime, so it runs」。也就是说 `ActionResult(attachments=[])` 是合法的（运行时会被丢掉），只有真正带内容的才拦。

**`ActionResult` 的宽容处理是这一页最值得读的部分。** 它保留 `extracted_content` / `error` 两个字段（外加 `extra='allow'`，所以额外字段照收），然后有两个 `mode='before'` 的加工步骤。

第一步是 `_without_0x_defaults`：**把 0.x 里「写了但不改变行为」的默认值删掉**。注释举了例子：「`ActionResult(extracted_content=..., is_done=False, success=True)`」——在 0.x 里 `success` 只在 `is_done=True` 时才被统计，所以这个组合完全等价于什么都不写。如果直接按「出现 `is_done` 就报错」处理，一次**计费运行**会因为一个无意义的默认值而终止。所以规则是：

```python
if data.get('is_done'):
    return data                                    # 真的是 is_done=True：交给下一步报错
return {k: v for k, v in data.items()
        if not (k in REMOVED_FIELDS and (k in ('is_done', 'success') or not v))}
```

读法：`is_done` 为假时，删掉所有「属于 REMOVED_FIELDS 且（是 `is_done`/`success` 这两个布尔语义字段，或者值为假）」的键。于是 `is_done=False` 与 `success=True` 都被抹掉（前者是布尔语义字段不看值，后者值为真但是布尔语义字段），`attachments=[]` 被抹掉（值为假），而 `attachments=['a.png']` 保留（属于 REMOVED_FIELDS 且值不假）→ 下一步报错。

第二步才是硬拒绝：查 `REMOVED_FIELDS`，命中就 `raise RemovedField(removed(...))`。`RemovedField` 继承 `TypeError`，docstring 说明了这个选择：「It ends the run（never handed to the model as a tool error）, so the developer sees it」——即这个错误**不给模型看**，直接终止运行让开发者看到。这与工具执行期的错误策略正好相反（那里会给模型看）。

`REMOVED_FIELDS` 四条的替代方案写得很具体，值得照抄其表达方式：

- `is_done` → 「the model ends the run with `finish`.」
- `images` → 「return text; the agent takes screenshots itself（`screenshot()`）」，并说明理由：「kept as an extra field it reached the model as JSON text: base64 image data in the conversation, never an image」。这是一条很有价值的观察——0.x 的 `images` 字段在 1.0 里会退化成对话里的 base64 文本。
- `success` → 「the model may give a verdict in `finish(..., success=...)`; `result.success` holds it（`None` when omitted）」
- `attachments` → 「save files in the workspace; files the agent wrote there are delivered.」

第三步（不报错）是 `IGNORED_FIELDS`：`include_in_memory` 与 `include_extracted_content_only_once` 这两个字段**只警告一次并删掉，不抛错**。注释给了两条理由：

> an action runs after the browser started（and a cloud browser is billed）, and 1.0 keeps every result in the conversation anyway

即「1.0 里每个结果都会留在对话里，所以这两个开关的行为恰好都是「全留」」——语义上等价，只是用户的意图无法表达。而报错的代价是终止一次已经计费的运行，不值得。`_warned` 是模块级集合，保证「一次运行只警告一次」——注释说「an action returns it on every call」，若每次都警告会刷屏。

**`Tools` 的 0.x 兼容面。** 它是一个几乎空的类，但三处细节都是为了让旧代码走到正确错误：

- `__init__` 只接受空 kwargs。非空时先查 `TOOLS_REMOVED`，命中给建议，未命中给通用提示。注意它的实现是先取 `next(iter(kwargs))` 报**第一个**——一次只报一个，避免错误信息过长。
- `registry` 属性返回 `self`，因为 0.x 写 `@controller.registry.action(...)`。注释就一行说明这个别名的作用。
- `__getattr__` 查 `TOOLS_METHODS_REMOVED`，把 `act`、`exclude_action`、`get_output_model`、`use_structured_output_action`、`set_coordinate_clicking` 五个已移除方法变成带建议的 `AttributeError`。

**模块级的两处「接线」也值得看。**

```python
__path__: list[str] = []
```

注释解释：0.x 的 `browser_use.tools` 是一个**包**（有 `tools.service`、`tools.registry.views` 等子模块），所以旧代码会写 `from browser_use.tools.service import ...`。空 `__path__` 让 Python 认为这个模块是包，从而让那些 import 走到 0.x 的 import hook（`browser_use/legacy.py`）而不是停在「not a package」。这是一个很少见的技巧：**用一个空列表改变模块的类型**，让错误路径能接管。

```python
if not TYPE_CHECKING:
    from browser_use.legacy import removed_names
    __getattr__ = removed_names(__name__)
```

即 `from browser_use.tools import registry`（0.x 的用法）会走 `__getattr__`，由 `removed_names` 产出指向 `Tools` 的错误提示。`if not TYPE_CHECKING` 的注释说明了为什么要包起来：「pyright would accept any name imported from here」——若不包，类型检查器会认为从这里能 import 任何名字，从而吞掉真实的拼写错误。

## 关键取舍

**源码事实：** `@tools.action` 在注册期做六道检查，其中「源码里返回 `ActionResult(is_done=True)`」用 AST 静态扫描，理由是这种写法只会在浏览器已经启动（云端已计费）之后才失败。

**源码事实：** 动作参数里出现 `INJECTED` 里的九个名字就直接报错；构造出的 Pydantic AI 工具是 `takes_ctx=False`。

**源码事实：** 工具名先按 `[a-zA-Z0-9_-]{1,64}` 自校验，避免等到供应商返回 400。

**源码事实：** `TOOL_RETRIES` 实际上是「不设限」，由 `max_steps` 兜住，因为按工具计的两次失败会误杀正常的修 bug 流程。

**源码事实：** `ActionResult` 先抹掉「0.x 里等价于没写」的默认值（`is_done=False`、`success=True`、空列表），再对真正有内容的已移除字段抛 `RemovedField`；`RemovedField` 继承 `TypeError` 且**不交给模型**。

**源码事实：** `include_in_memory` / `include_extracted_content_only_once` 只警告一次并静默删除，因为报错会终止一次已计费的运行，而 1.0 的行为恰好与这两个开关的意图一致。

**源码事实：** 模块用空 `__path__` 把自己伪装成包，让 0.x 的子模块 import 能落到兼容层的 hook 上。

**架构推论：** 这个模块的主要工作量不在功能（注册一个函数只需十几行），而在**迁移语义**。四张映射表的价值在于：把「AttributeError / TypeError / 供应商 400」这三种不可读的失败，统一成「你写的是 X，1.0 里请用 Y」。一次计费运行中途失败的代价远高于启动期一个明确的报错，这是所有这类表存在的经济理由。

**架构推论：** `RemovedField` 继承 `TypeError` 而不是某个自定义基类，是一个务实的选择：升到 `TypeError` 意味着现存的 `except TypeError` 能接住它，也意味着它在语义上确实属于「类型用错了」。但它与普通 `TypeError` 的差别在**投递对象**：普通工具错误会给模型看，这个不给。

**架构推论：** 「抹掉等价于没写的默认值」这条规则揭示了兼容层设计的一个要点：**判断标准不应是「字段名是否已移除」，而应是「这次使用是否改变了行为」**。按前者判会误杀大量无害代码，按后者判才既安全又不添麻烦。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/tools.py` | `Tools` | 动作注册表；0.x `Controller` 的别名 |
| `browser_use/tools.py` | `Tools.action` | 装饰器：六道注册期检查、同名替换、`param_model` 回填 |
| `browser_use/tools.py` | `ActionResult` | 0.x 返回类型；两个 `mode='before'` 的宽容与拒绝 |
| `browser_use/tools.py` | `_without_0x_defaults` | 抹掉等价于没写的 0.x 默认值，避免终止计费运行 |
| `browser_use/tools.py` | `_check_done_results` | AST 静态扫描 `ActionResult(is_done=True)` 等写法 |
| `browser_use/tools.py` | `_empty_literal` | 判定「写了等于没写」的字面量 |
| `browser_use/tools.py` | `RemovedField` | 继承 `TypeError`，不交给模型的硬拒绝 |
| `browser_use/tools.py` | `TOOLS_REMOVED` / `TOOLS_METHODS_REMOVED` / `ACTION_REMOVED` | 三张「旧写法 → 新做法」映射表 |
| `browser_use/tools.py` | `RESERVED` / `INJECTED` / `TOOL_NAME` | 内置名、0.x 注入参数名、供应商可接受的名字形状 |
| `browser_use/tools.py` | `TOOL_RETRIES` | 事实上的「不限重试」，由 `max_steps` 兜住 |

## 参考

- [browser-use 源码：tools.py](https://github.com/browser-use/browser-use/blob/main/browser_use/tools.py)
- [browser-use 仓库：MIGRATING.md](https://github.com/browser-use/browser-use/blob/main/MIGRATING.md)
- [Pydantic AI 文档：工具](https://ai.pydantic.dev/tools/)
- [OpenAI 文档：function calling 的函数名约束](https://platform.openai.com/docs/guides/function-calling)

## 与主核 / AI 的关系

「在启动前把必然失败的配置拒掉」与主核里的**目录校验**是同一种工程手段：`CREATE INDEX` 时若引用了不存在的列，主核会在 DDL 阶段报错，而不是等到第一次查询才发现。两边都遵循同一条经济学：**错误发现得越早，代价越小**——主核里是避免一次全表扫描才发现，这里避免一次已经计费的运行。

四张映射表的形态在主核里也有对应物：升级大版本时被移除的 GUC 参数、被改名的系统视图，主核通常的做法是「保留一个能读但报错/警告的壳」。差别在主核的壳往往要活很久（因为生态大），而这里把壳做得很薄——只负责产出错误文本，不模拟任何行为。这个选择适合「行为已经根本改变」的重写：0.x 的 action 有内置动作与注入参数，1.0 的模型自己在 Python 里动手，两边的动作概念已经不是同一个东西，任何行为模拟都会制造误解。

对 AI 工程的实际意义在于三点。第一，**重试预算不要按工具计**：这个仓库用了一个近乎无限的数字并把约束交给步数预算，避免「模型连犯两次格式错误就被判死」。第二，**供应商侧的约束要前置到本地**：工具名的字符集与长度、名字不能是 `<lambda>`，这些都是供应商 400 的常见成因，本地校验一次就能省掉一整轮往返。第三，**兼容层要区分「等价于没写」与「真的改了行为」**：前者静默抹掉，后者才报错，否则升级体验会变成一片红。
