---
project: browser-use
feature: accessibility-tree
title: 无障碍树观测与交互原语
page_type: feature
schema_version: 1
refs:
  - path: browser_use/ax.py
    symbols: [AxHelpers, state, settle, drain_events]
  - path: browser_use/ax.py
    symbols: [click, type, _describe, _capture]
---

# 无障碍树观测与交互原语

## 一句话职责

`AxHelpers` 把 Chrome 的无障碍树压成一份「一页纸、带编号、按页面顺序」的文本状态，并提供按那个编号点击/输入的交互原语——**模型只看到文本，按 `[id]` 操作**，不必读 DOM、不必算坐标。

## 机制是什么

这套设计要回答一个很具体的问题：模型怎么「看见」页面？

常见做法有三条：截图（多模态，成本高且不可检索）、DOM 序列化（体积巨大、噪音多）、以及无障碍树（Chrome 已经为屏幕阅读器算好的语义结构）。这里选第三条，理由是可以从实现读出来：

- AX 树里每个可交互节点带 `backendDOMNodeId`，**这个 id 可以直接喂给 CDP**，不需要再走 `DOM.querySelector`。
- 树里只有语义角色与名字，没有 class、样式、脚本——天然是「给读者看的」摘要。
- 它按页面顺序排列，所以文本顺序即阅读顺序。

整个模块可以分成四层：

| 层 | 职责 |
|---|---|
| 事件观察 | 后台线程持续 `drain_events`，跟踪在飞请求、导航失败、JS 对话框 |
| 稳定等待 | `settle()` 用「文档已解析 + 无新请求 + DOM 静默」代替固定 sleep |
| 状态打印 | `state()` 遍历 AX 树、编号、压缩、按预算截断 |
| 交互原语 | `click()` / `type()` 按 id 找到未被遮挡的坐标再发 CDP 输入事件 |

## 怎么实现的

**后台事件线程是「凭什么能不用 sleep」的前提。** `__init__` 里起一个 daemon 线程跑 `_watch` → `_pump`（25 毫秒一轮，异常退避 500 毫秒）。`_pump` 从守护进程拿事件，逐个交给 `_observe`，同时把事件塞进 `deque(maxlen=500)` 供 `drain_events()` 取走。

`_observe` 做四件事：

- **`Page.javascriptDialogOpening`**：直接 `Page.handleJavaScriptDialog(accept=True, promptText=defaultPrompt)` 关闭它，并把文本记进 `dialogs`。注释解释了为什么必须在这里做：「An open alert/confirm blocks the page and every CDP call on it」。这是与 worker 那侧的 `dialog_open` 配套的——这里主动关掉，那里则把卡住的调用翻译成解释。
- **`Target.detachedFromTarget`**：清掉该 session 的三张表条目，避免泄漏。
- **`Network.requestWillBeSent` / `loadingFailed` 里的 `Document` 类型**：前者清掉导航错误（新文档开始加载），后者记下错误文本。注释说明用途：「Chrome then shows `chrome-error://chromewebdata/`, often with an empty accessibility tree that says nothing about why」。这就是后面 `state()` 能写出「页面没加载成功：某错误」的依据。
- **普通请求的在飞记账**：只记非长连接类型（排除 `WebSocket`/`EventSource`/`Media`/`Ping`/`Manifest`），按 `requestId` 记开始时间，完成/失败时移除。

**`settle()` 是这个模块最精巧的一段。** 它的 docstring 就是设计说明：「Event-based wait, never a fixed sleep: document parsed, no fresh in-flight requests（older than 1.5 s are treated as long-poll/analytics）, and no DOM mutation for `quiet_ms`. Bounded by `cap_ms`.」

三层条件缺一不可，因为它们各自覆盖一类「看起来静止但还没好」：

| 条件 | 靠什么判 | 挡住的误判 |
|---|---|---|
| 文档已解析 | `PROBE_JS` 的 `ready` 状态 | 页面还在加载就开跑 |
| 无新鲜在飞请求 | `inflight()` 计数 | 数据还没回来 |
| DOM 静默 `quiet_ms` | 探针的 `idle` 值 | 还在渲染/动画中 |

三个不显然的细节：

- **1.5 秒以上视为长轮询/埋点**：`sum(now() - t < 1500 for t in ...)`。否则任何有 websocket 心跳或分析脚本的页面都会永远「忙」。
- **`since` 参数的语义**：`probe['idle'] = min(probe['idle'], now() - (since or start))`，注释说「quiet must be observed after this action began」。即「静默」必须发生**在动作之后**——否则动作前的静默会被误算成动作已生效。
- **导航需要更长**：`if probe['ready'] == 'loading': limit = max(cap, 3000)`。注释「navigations need longer than in-page updates」——同样的 `cap_ms` 对「页内更新」合适，对整页导航太短。
- **`is_context_loss(e)` 是唯一被容忍的异常**：注释「Navigation in progress: wait for the new document. Anything else ends the wait, never the action」——导航期间 `Runtime.evaluate` 会因为执行上下文销毁而失败，这是预期内的；其他异常结束等待但**不结束动作**。

返回值固定是 `{'why', 'pending', 'ms', 'probe'}`，`why` 三取一：`quiet`（正常）、`cap`（超时）、`error`。这个结构被 `state()` 的头部直接使用（`loading: N`）以及 `_change()` 用来判断变化。

**`state()` 是核心，也是最长的函数。** 它的流程：

1. `since = self.acted_at if self.acted else 0`——注释「A look right after an action continues that action's quiet window instead of starting a new one」。即模型主动 `state()` 时不该重新等一轮。
2. `self.acted = False`——注释「a look after the last action replaces the automatic print」。避免重复打印。
3. `settle(cap_ms=3000, since=since)`。
4. **四个并行取数**（`ThreadPoolExecutor(8)`，注释「Pi's Promise.all」）：`_capture()`（AX 树）、`_info()`（url/title）、`_visible_text()`、`_file_inputs()`。两处优化：若 settle 的探针里已经有 `url`/`title` 就跳过 `_info`（`info = probe or info_job.result()`）；若探针说没有文件输入（`probe.get('files') is False`）就跳过 `_file_inputs`。
5. 逐 frame 遍历，构造行列表。
6. 补上「隐藏的文件输入」，拼头部，按预算截断，`well_formed` 后 `log` 并返回 `Printed`。

`_capture()` 有 21 次重试（`for i in range(21)`），只在 `is_context_loss(e)` 时重试，间隔 50 毫秒。主 frame 的树直接取，子 frame 用 `self._pool.map(frame, children)` **并行**取，且每个子 frame 的失败被吞掉返回空节点列表——注释「other frames fail and are skipped」。这是必要的容错：跨进程 iframe 的 AX 树常常取不到。

**遍历的规则可以归成「三类节点」。**

- **`StaticText`**：不发新行，而是累积进 `text` 缓冲。两个筛选：`t not in inside`（不在父级的描述文本里重复）与 `visible is None or t in visible`（若拿到了可见文本，就必须真的可见）。后一条挡的是「AX 树里有但视觉上不存在」的文本（隐藏的 aria、屏外内容）。
- **`heading`**：把缓冲 flush 成一行 `## 名字`，然后在 `inside=fold(name)` 的上下文里递归——这样标题下的正文不会重复打印标题。
- **可交互节点**：判据是 `role in CONTROLS or focusable`，且必须有 `backendDOMNodeId`。

`focusable` 的判定写得很有用意：

```python
focusable = prop(n, 'focusable') and role != 'RootWebArea' and (name or val(n.get('value')) or prop(n, 'editable'))
```

注释解释了后半个合取：「Focusable wrappers（tabindex=-1 dialogs, `<main>`）are only controls if they carry a name, value or editing」——否则满页的容器 div 都会变成「控件」。

`BLOCKS`（`paragraph`、`listitem`、`row`、`LabelText`）是分段点：走到它们就 flush 一次文本缓冲，但自身不发控件行。

**每个控件行由六段拼成**（任一为空就省略）：新 id 的 `*` 前缀、`[id] role`、`"名字"`（截 150 字符）、`= 值`（截 100、加引号）、状态词、`options:` 列表。

`*` 前缀的含义来自 `previous = self.last`：**上一次打印里没出现过的 id**。注释写「marks new ids and names targets in action lines」——这让模型能看出「这一步新出现了哪些元素」，是判断动作效果的廉价信号。

`shown` 的推导有一处实现细节：**选择框、文件按钮、可编辑 generic 会被改名**成 `select` / `file` / `textbox`。`select` 的那个判断来自「A native `<select>` keeps its options in a popup child: summarize them instead of listing each」——先找 `MenuListPopup` 子节点，收集其中的 `option` 名字，只显示前 5 个（超出加 `| … N more`）。而 `file` 的判定是 `role == 'button' and value`——文件按钮在 AX 树里就是一个带值的按钮。

状态词有五个来源，其中 `disabled` 那条带了限定：「Chrome flags aria-disabled too, but only native disabled stops input: those are never focusable」，所以判据是 `prop(n, 'disabled') and not prop(n, 'focusable')`。

**标签去重是一处体贴的细节**：

```python
if lines and lines[-1].lower() == name.lower():
    lines.pop()  # its label, printed as its name
```

即若上一行（StaticText 累积出的段落）与当前控件的名字相同，就把那行删掉——否则会出现「Search」段落紧跟 `[12] textbox "Search"` 的重复。

**截断策略有明确理由**：「Over max, keep the top and the last fifth: a dialog's Done/Submit is usually its last line.」即头部取 80% 预算、尾部取 20%，中间插一行说明切了多少。这是一个基于真实页面结构的判断：对话框的确认按钮通常在最后。

**空树的解释分两种**，这也是很实用的一处：

```
(the page did not load: <net 错误>)          ← 有导航错误时
(nothing in the accessibility tree: the page is blank, or a popup, often in a
 cross-origin iframe, hides it; screenshot() to see it)   ← 否则
```

第二种文案还给出了下一步动作（`screenshot()`），而不是只说「空」。

**隐藏文件输入的补漏**：`_file_inputs()` 用 `DOM.querySelectorAll('input[type=file]')` 找所有文件输入，然后「凡是不在 `self.last` 里的」就补一行 `[id] file "名字" (hidden)` 并插到最前面（`lines[:0] = hidden`）。注释给了原因：「Styled uploaders hide their file input, which drops it from the AX tree; its id still works for uploads」。名字取 `aria-label` → `name` → `id` 依次回退。

**交互原语的核心是 `_point()`：找「可点且未被遮挡」的坐标。** 分三步尝试：

1. `DOM.scrollIntoViewIfNeeded` 之后跑 `POINT_JS` 拿到点。若 `covered` 为假就用它。
2. 若被遮挡，改用 `CENTER_JS` 居中滚动后重试。注释给了具体例子：「A fixed header or footer over a target near the viewport edge: the target counts as visible, so `scrollIntoViewIfNeeded` leaves it there（demoqa.com's footer）. Centered, it is usually clear.」
3. 仍被遮挡就**报出是谁遮挡**：`DOM.getNodeForLocation` 找到该坐标下的节点，`Accessibility.getPartialAXTree` 取它的祖先链，向上找第一个「有名字或是 dialog 角色」的节点，并用 `text()` 在它没有名字时取它的第一个 `StaticText` 子节点作为名字。

最后那句错误写得极具体：`Target covered by [id] role "名字"; nothing was clicked`。这条信息对模型有用得多——它能据此判断是不是要先关掉一个弹层。

`_act()` 是 click/type 的共同外壳，它的节奏值得看清：

```python
self.acted = True ; self.acted_at = 0        # 先置位：失败的动作也要打印状态
self._track_network()                         # 确保 Network 域已启用
before = self._probe()                        # 动作前快照
detail = body()                               # 真正动手
self.acted_at = now()                         # 动作完成时刻（供后续 settle 的 since）
settled = self.settle()
line = f'[ok] {op} {describe(id)}{detail} -> {self._change(before, settled.probe)}'
```

`_change()` 的三分判定：`doc` 或 `url` 变了 → `navigated to <url>`（若该 URL 是错误页则附上 net 错误）；`n`（节点数）变了 → `page changed`；否则 `no change`。这让每次动作都自带一句结果摘要，模型不必再调 `state()` 才知道发生了什么。

`_click_at()` 有一处「去掉一步反而更好」的决定：**不发 `mouseMoved`**，直接 `mousePressed` + `mouseReleased`。注释给了实测理由：「browser-harness tabs stay in the background, where Chrome answers `mouseMoved` only after ~5 s（measured, headless Chrome）. Press and release move the pointer anyway.」

**`type()` 覆盖了五种表单形态。** 按 `KIND_JS` 拿标签与类型后分派：

- `SELECT`：走 `SELECT_JS` 按选项文本选择。
- 日期类 `INPUT`（`date`/`time`/`datetime-local`/`month`/`week`）：走 `DATE_JS` 直接设值——这类控件用键盘输入不可靠。
- 其余：先 `_point(id, editable_cover=True)` 拿点、点击，然后处理「组合框把焦点抢到自己覆盖层里」的情况（`MOVED_JS` 检测，注释「type there, not into the hidden original」）。接着 `Ctrl+A`（`modifiers: 2` + `commands: ['selectAll']`）选中原文本，`text` 为空就发 `Backspace`，否则 `insertText` 打前 n-1 个字符、**最后一个字符用真实按键事件**发出。注释解释了为什么：「Datepickers and autocompletes react to key events, which `insertText` never sends: type the last character as a key.」——即用一个字符的按键事件触发那些监听 `keydown` 的组件，同时避免逐字符发送的开销。

**密码字段的显示脱敏**：

```python
secret = kind['tag'] == 'INPUT' and kind['type'] == 'password'
shown = (lambda v: f'<{len(v)} characters, hidden>') if secret else (lambda v: quote(clip(v, 60)))
```

注释写得很实在：「A password field's value never goes into the action line: it lands in the trace, logs and the judge's prompt」。即这一行的文本会流经追踪、日志与自评提示词，所以长度可以透露、内容不行。

## 关键取舍

**源码事实：** 观测走无障碍树而非截图或 DOM 序列化，编号直接用 `backendDOMNodeId`，因此 id 可被 CDP 直接消费。

**源码事实：** `settle()` 用「文档已解析 + 无新鲜在飞请求（超过 1.5 秒视为长轮询）+ DOM 静默 `quiet_ms`」三条件代替固定等待，且 `since` 保证静默发生在动作之后。

**源码事实：** 后台线程主动 `accept` 掉 JS 对话框，因为一个打开的 alert/confirm 会阻塞该页面上所有 CDP 调用。

**源码事实：** 遍历只把 `role in CONTROLS` 或「focusable 且带名字/值/可编辑」的节点当控件，`StaticText` 累积成段而非逐条成行，标题成为 `## 名字` 且屏蔽其子树的重复文本。

**源码事实：** 上次打印里没出现过的 id 加 `*` 前缀；`<select>` 的选项被摘要成前 5 个；隐藏的文件输入被单独补在最前面。

**源码事实：** 超预算时保留头部 80% 与尾部 20%，理由是「对话框的 Done/Submit 通常是最后一行」。

**源码事实：** `_point()` 三次尝试（原位 → 居中 → 报出遮挡者），失败时的错误信息包含遮挡元素的 id、角色与名字。

**源码事实：** 点击不发 `mouseMoved`，因为后台标签页的 `mouseMoved` 要等约 5 秒。

**源码事实：** 输入时最后一个字符用真实按键事件而非 `insertText`，以触发监听按键的日期选择器与自动补全。

**源码事实：** 密码字段在动作行里只显示字符数。

**架构推论：** 选无障碍树作为「眼睛」，本质上是接受了一个**已经被语义压缩过的视图**。好处是 token 经济与可读性；代价是信息损失——AX 树不告诉你元素长什么样、在哪儿，也不保证跨站 iframe 的内容可得。这解释了为什么 `_capture` 要吞掉子 frame 失败，以及为什么空树时的提示要引导模型去看截图。

**架构推论：** 「不用固定 sleep」这条纪律贯穿整个模块：`settle` 靠事件、`_capture` 只在上下文销毁时重试、点击去掉 `mouseMoved`。这些改动都来自实测（注释里的 `~5 s`、`demoqa.com's footer`），说明**等待策略必须由观测得出，不能靠猜**。

**架构推论：** 每条动作都产出「一行摘要 + 结果判定」，而不是让模型自己再查一次。这降低了一次交互的往返次数，也把「怎么判断动作生效」这件事从模型转移到了代码——后者能比较 `doc`/`url`/`n` 三个量，比模型偶尔回头看一眼更可靠。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/ax.py` | `AxHelpers` | 无障碍树观测与按 id 交互的入口 |
| `browser_use/ax.py` | `state` | 遍历 AX 树生成编号状态文本，含截断与空树解释 |
| `browser_use/ax.py` | `settle` | 事件驱动的稳定等待（文档/网络/DOM 三条件） |
| `browser_use/ax.py` | `_capture` | 主 frame 与同进程子 frame 的 AX 树，含上下文销毁重试 |
| `browser_use/ax.py` | `_watch` / `_pump` / `_observe` | 后台事件线程：对话框、在飞请求、导航失败 |
| `browser_use/ax.py` | `drain_events` | 把已读事件交给模型 |
| `browser_use/ax.py` | `click` / `_act` | 动作外壳：快照、动手、settle、一行判定 |
| `browser_use/ax.py` | `type` | 五种表单形态的分派与末字符按键 |
| `browser_use/ax.py` | `_point` | 三次尝试找未被遮挡的可点坐标，失败时报出遮挡者 |
| `browser_use/ax.py` | `_describe` | 把 id 还原成「role "名字"」的动作行描述 |
| `browser_use/ax.py` | `_change` | `navigated to` / `page changed` / `no change` 三判 |
| `browser_use/ax.py` | `_nav_error` | 从错误页 URL 反查 net 错误 |

## 参考

- [browser-use 源码：ax.py](https://github.com/browser-use/browser-use/blob/main/browser_use/ax.py)
- [Chrome DevTools Protocol：Accessibility 域](https://chromedevtools.github.io/devtools-protocol/tot/Accessibility/)
- [W3C：无障碍树与核心概念](https://www.w3.org/TR/core-aam-1.1/)
- [browser-harness 仓库](https://github.com/browser-use/browser-harness)

## 与主核 / AI 的关系

「把一棵结构化树压成一份可读摘要，并保留稳定的引用编号」与主核里的**执行计划展示**是同一种结构：主核的 `EXPLAIN` 也是把一棵算子树压成缩进文本，节点带代价与行数估计，读者据此判断该改哪里。两边都在做同一件事——**用一份有稳定标识的文本摘要替代原始结构**。

差异在 id 的生命周期。主核的计划编号只在一份计划内有效；这里的 `backendDOMNodeId` 跨调用有效（下次 `click(12)` 仍指向同一个节点），但**跨导航就失效**。这正是 `self.last` 每次 `state()` 都重置、而 `previous` 只保留一轮的原因：id 的有效期与页面的生命周期绑定。

`settle()` 的三条件与主核里的**忙等消除**是同一类工程。数据库不会在发出写请求后 `sleep(100ms)` 再读，而是等 WAL 落盘事件；同理这里不会在点击后固定等一秒，而是等到「没在飞请求 + DOM 静默」。两者都把「等待何时结束」交给可观测信号而不是时钟。

对 AI 工程的实际意义在于：这份实现给出了「让模型操作图形界面」的一组具体答案。第一，**用无障碍树而不是截图做观测**，token 成本低一个数量级且天然可检索。第二，**编号即句柄**——把树的节点 id 直接暴露成 `[12]`，模型下一句就能引用，避免了「再描述一遍元素」的歧义。第三，**每个动作都自带结果判定**，把「是否生效」的判断从模型移到代码。第四也是最能体现工程成熟度的一条：**所有等待与超时都带实测依据**，注释里那些「约 5 秒」「demoqa 的页脚」不是逸闻，而是把一次真实故障固化成了不再复发的规则。
