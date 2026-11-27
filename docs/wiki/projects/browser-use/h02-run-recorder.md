---
project: browser-use
feature: run-recorder
title: 运行录制与步骤归集
page_type: feature
schema_version: 1
refs:
  - path: browser_use/telemetry/recorder.py
    symbols: [RunRecorder, build, Step]
  - path: browser_use/telemetry/recorder.py
    symbols: [_timings, _hosts, _struggle]
---

# 运行录制与步骤归集

## 一句话职责

在 agent 路径上用 **O(1) 的内存更新**记下一次运行的每一步（模型耗时、首字延迟、token、单元格、helper、导航、错误），然后在遥测线程里一次性把记录折叠成事件的字段——包括一组「这个 agent 挣扎在哪」的聚合指标。

## 机制是什么

两个类 + 一组折叠函数：

| 成员 | 作用 |
|---|---|
| `Step` | 一步：一次模型响应 + 它跑的单元格。用 `__slots__` 存 21 个字段 |
| `RunRecorder` | 一次运行的记录器，六个 hook：`attempt_start`/`attempt_ok`/`attempt_failed`（模型请求）、`cell`（python）、`retry`、`compaction`、`phase`、`delivery` |
| `build(stats, total_ms, limit, top_n)` | 在遥测线程里把记录折叠成 v3 事件字段 |
| `_timings` / `_hosts` / `_struggle` / `_arrays` | 四类聚合：按名字、按主机、按挣扎模式、按步序列 |

模块 docstring 定下了基调：**每个 hook 都在 agent 路径上跑，且都是 O(1) 的内存更新（一次 `perf_counter_ns`、一次 append、一次 add）；`build()` 在运行之后的遥测线程里一次性把记录变成事件字段**。

## 怎么实现的

**「hook 是 O(1)、折叠在事后」是整篇最重要的分工。** 这条分工决定了两件事：agent 路径上不能有遍历或序列化；而所有昂贵的工作（排序、`most_common`、压缩成数组）都发生在 `build`。`build` 里几乎没有循环开销的顾虑——它可以随意排序。

**`Step` 用 `__slots__` 是必要的，不是风格。** 一次运行可能有 200 步，每步 21 个字段；普通对象每实例多一个 `__dict__`，而这里的对象数量与步数线性相关。模块 docstring 里的「O(1)」承诺在内存分配上也要成立。

**「步」的定义是「一次模型响应」而不是「一次工具调用」。** `attempt_ok` 里创建 `Step` 并把 `response.usage` 的 `input_tokens`/`output_tokens`/`cache_read_tokens`/`reasoning_tokens` 抄进去，同时记 `finish_reason`。而单元格（cell）是**追加到最后一个 step 上**的：`s.cells += 1`、`s.cell_ms += ...`。于是「一步里跑了几个单元格」是一个计数，而不是多个 Step。这个归集方式与 agent 循环的形状一致：每次模型响应之后可能连续执行多次 `python`。

**`_pending_ev` 与 `_failed` 是两个跨 hook 的累积器。** `EV_*` 是一组位标记（server/client compaction、retried、delivery、captcha、blocked、cut、new page、same code、restart、nav failed）。它们不全是「当前 hook 就能决定」的：`EV_RETRIED` 要等下一次 `attempt_ok` 才知道「上次失败过」，`EV_CLIENT_COMPACTION` 要在 compaction 之后的下一个 step 上体现。所以用 `_pending_ev` 累积、在 `attempt_ok` 里并入 `ev` 再清零。

**`Step.code` 存的是代码的哈希，且从不发送。** 字段注释写得很明确：`hash of the step's cell code; compared, never sent`。它只用于 `_struggle` 里检测「连续两步写了完全一样（或逐步累积后一样）的代码」——那是一个强信号，说明 agent 卡在同一个尝试上。哈希在 `cell` 里逐步累积：`s.code = hash(code) if s.code is None else hash((s.code, code))`，于是「一步里的多个单元格合起来」也有一个指纹。

这是一个**在隐私与可观测性之间的精确切分**：需要的是「是否重复」这个布尔结论，而不是代码内容。存哈希就够，且哈希永远不会离开进程。

**`Step.kind()` 用优先级链分类，且第一优先是「交付」。** 顺序是：`deliver`（有 `finish`/`finish_from_repl` 工具调用，**或根本没有工具调用**——因为一次纯文本回复也是在尝试交付）→ `captcha` → `nav` → `interact` → `search` → `files` → `wait` → `read` → `compute`/`none`。

`DELIVER`、`INTERACT`、`WAITS`、`FILES` 四个 frozenset 就把「哪些 helper 属于哪类行为」编码出来了，且 `INTERACT` 里同时收了 browser-harness 的原名（`click_at_xy`、`type_text`）与 `bu.*` 形式（`bu.click`、`bu.type`）——因为 ultrafast 模式走的是后者。

**`RunRecorder.__init__` 的 `known` 集合解决了「只报我们或开发者选的名字」。** `self.known = DELIVER | {'python'} | tools`，而 `attempt_ok` 里对 `ToolCallPart` 的处理是 `s.tools.append(p.tool_name if p.tool_name in self.known else 'invalid')`。模块 docstring 的对应句是：**只记 browser-use、Python 或已安装包选定的名字**。于是模型编造的工具名统一压成 `invalid`，不会把任意字符串带进遥测。

**`_model_name` 是同一原则在模型名上的应用，且带一个更细的判定。** 只有在两种情况下才承认响应的 `model_name`：**走网关**（名字由网关注入，可信），或**名字以配置模型的最后一段路径开头**。否则返回 `'other'`。docstring 明说被排除的对象：**一个自定义端点可以编成任意本地路径或文本**。`MODEL_NAME` 正则（`[A-Za-z0-9._:-]{1,64}`）是第一道闸，`_code` 里的 `CODE` 正则（`[a-z0-9_.]{1,40}`）对 provider 错误码做同样的事，不合格一律 `'other'`。

**`cell` 里对 `r` 的解读会把 REPL 的统计折进三个层次。** `stats.helper_t` 是 `{helper: [calls, errors, ns_total, ns_max]}`，于是同一次遍历同时累加：所有 helper 的总耗时、其中属于 `WAITS` 的部分、是否发生过 `INTERACT`、是否调过 `checkpoint`。`stats.cdp_t` 单独汇总成 `cdp_ns`。三笔分别进 `ms['helpers']`、`ms['waits']`、`ms['cdp']`，同时在 step 上记 `wait_ms` 与 `cdp_ms`。

这样「总耗时」与「按步耗时」都有，而**总耗时是跨步求和的精确值**（`ms` 累加的是 ns），按步是毫秒取整——一个刻意的精度分配：总和要精确，明细可以粗。

**`first_nav` 与 `first_ok_ms` 是两个「首个有效动作」指标。** 前者记第一次发生导航的 `(step, ms)`；后者的条件是 `not r.get('error') and (nav or interacted)`，即**一次不报错且有实质动作（导航或交互）**的单元格。注释没有多写，但从名字能看出这两个指标的用途：衡量「agent 多久开始真的动浏览器」以及「多久开始有效地动」。

**`_hosts` 处理了「某步没看到 URL」的情形。** docstring：**一个没看到 URL 的 step 归到前一步的 host 上**。实现是先 `s.host = current = s.host or current`（**直接把省缺值回写到 Step 上**），再累计七元组 `[steps, err_steps, cell_ms, captchas, blocks, navs, failed navs]`。

回写到 `Step` 上这个细节有意义：后续 `_struggle` 的 host streak 判定读的也是这个字段，如果不回写，它会在空 host 的步上断掉连续性。

**`_struggle` 是这一页最有产品意义的函数。** 它在 O(steps) 里算出十一个「挣扎指标」：

| 指标 | 含义 |
|---|---|
| `max_err_streak` | 最长连续报错步数 |
| `err_streaks_3` | 出现过的「连续 3 步以上报错」次数 |
| `streak_err` / `streak_kind` / `streak_helper` | 最长那段里最常见的错误名、行为类别、首个 helper |
| `repeat_fail_steps` | 与上一步「同一错误 + 同一组 helper」的步数 |
| `repeat_code_steps` | 与上一步代码指纹相同的步数 |
| `max_host_streak` | 在同一 host 上连续停留的最长步数 |
| `last_progress_step` / `wasted_steps` | 最后一次有进展的步序号，及其之后的步数 |

**「有进展」的判定是这里最讲究的一处。** 条件是 `not s.err and (new or s.checkpoint or delivered)`，其中 `new = EV_NEW_PAGE and not EV_NAV_FAILED`——**一个错误页不算进展**。而 `delivered` 有一个注释解释的特殊情形：

> 一个最终的文本回复也是交付（`finish_via text`）：live 运行在 bu-fast 上 11 次里有 6 次是这样交付的，而每一次都把它最后一个新页面之后的每一步算成了浪费

也就是说早先的版本只认 `finish`/`finish_from_repl` 工具调用，于是「靠纯文本回复交付」的运行会被算出大量虚假的「浪费步数」。修法是把「最后一步且无工具调用」也算交付。

**这段注释本身是这一页最有价值的东西**：一个聚合指标的定义错了一个边界，会让 6/11 的运行报告出完全错误的结论，而错误方向恰好是「看起来更糟」。这种偏差如果不修，就会一路传到产品决策里。

**`build` 里的步数保留策略是「首尾保留、中间丢弃」。** `head = limit // 4`，于是 200 步上限下保留**前 50 与后 150**。理由是明显的：开头（启动、首次导航）与结尾（交付、失败）比中段有信息量。被丢弃的数量记在 `tel_steps_dropped` 里，让下游知道这不是全量。

**`build` 的时间字段分两类：已知项与余项。** `ms_total` 是总时长；`ms_*` 逐项列出（preflight、model、ttft、cells、helpers、waits、cdp、compaction、retry_sleep、delivery、shutdown）；然后 `ms_other = max(0, total_ms - sum(已知项))`。明确算出一个「其它」余项，比让下游自己减要好：**下游无法知道该减哪些项**，而这些项会随版本增减。

**`ms_browser_start` 可以为 `None` 而 `ms_repl_start` 缺省为 0。** 前者的 `ms.get('browser_start')` 没有默认值，因为「浏览器是否被启动过」这件事本身有信息量（附着到用户的 Chrome 时就没有这个阶段）；后者用 `ms.get('repl_start', 0)`，因为 REPL 总是要起。

**各聚合映射都有 top-N 截断，且截断值不同。** `response_models` 取前 5、`nav_errors` 取 `min(top_n, 20)`、`host_stats` 取 `min(top_n, 30)`、`helper_stats` / `cdp_stats` 取 `top_n`。这不是随手写的数字：不同映射的基数不同（模型最多几个、主机可能几十个、helper 十几类），截断值反映的是「这个维度有多少个值是有意义的」。

**`_timings` 的排序依据是总耗时而不是调用次数。** docstring：**返回 `{name: [calls, errors, ms total, ms max]}`，按总耗时取前 n 个**。这是一个正确的选择：调用 1000 次但每次 1 微秒的 helper 不值得关注，调用 3 次每次 2 秒的才值得。

**`_struggle` 里 `streaks3` 的收尾处理容易漏。** 循环结束后还有一句 `streaks3 += streak >= 3`，因为「一直失败到最后」的那段不会被循环内的 `else` 分支计数。这类「循环内的结束条件」是聚合代码里最常见的 off-by-one 来源。

**`common()` 的小工具筛掉空值再取众数。** `values = [v for v in values if v]` 之后才 `most_common(1)`。若不筛，空字符串会成为一个候选，可能把真正的众数挤掉。

**`CURRENT` 这个 ContextVar 解决了「没有 RunContext 的代码怎么记时」。** 定义处注释：**运行自己的记录器，供没有 `RunContext` 的代码使用（`StreamedRequest`：到第一个事件的耗时）。每个运行在自己的任务里设置它。** 而 `first_event()` 的实现是 `if self._ttft < 0: self._ttft = now() - self._att`——用 `-1` 作「尚未发生」的哨兵，且只记第一次。

**`where` 字段记的是「异常发生在哪个阶段」。** `attempt_start` 置 `'model'`、`cell_start` 置 `'cell'`、`attempt_ok` 与 `cell` 结束时清空。字段注释：**一个结束了运行的异常来自哪里**。这个信息让「失败在模型请求还是单元格」可以被区分——两者的排查方向完全不同。

**容量上限在多处出现且各不相同。** `model_fails` 30 条、`compactions` 20 条、`s.helpers` 6 个、`s.err` 与 helper 名截断到 `MAX_NAME`。这些都是「防一条畸形事件撑爆 64 KiB」的具体措施，与 `h01` 的分档降级是同一套思路的两层。

## 关键取舍

**源码事实：** 所有 hook 都是 O(1) 的内存更新，折叠（排序、众数、压缩数组）全部推后到遥测线程的 `build`。

**源码事实：** `Step` 用 `__slots__`；一步 = 一次模型响应，单元格以计数形式累加在这一步上。

**源码事实：** 步骤代码只存哈希且从不发送，仅用于相邻步比较以检测「重复同一尝试」。

**源码事实：** 工具名与模型名都过「已知/可信」过滤，未知一律压成 `invalid` / `other`。模型名只在网关或前缀匹配时才承认，因为自定义端点可以编造任意名字。

**源码事实：** 位标记分两批：本 hook 能定的直接 OR，跨 hook 的用 `_pending_ev` 累积到下一个 step。

**源码事实：** 「有进展」判定把错误页排除在外；且最终纯文本回复也算交付——不这样做会让 11 次 live 运行里的 6 次报出大量虚假浪费步数。

**源码事实：** 步数保留是「前 1/4 + 后 3/4」，丢弃数量显式上报。

**源码事实：** 时间字段里显式算出 `ms_other` 余项，因为下游无法知道该减哪些项。

**源码事实：** `_hosts` 把省缺 host 回写到 `Step`，否则后续步骤的 host streak 会被空值打断。

**源码事实：** `_timings` 按总耗时排序而非调用次数。

**架构推论：** 这个模块的设计可以概括成一句话：**在热路径上只允许「记下来」，不允许「理解它」**。所有需要比较、排序、统计的工作都在事后做；而为了支撑事后理解，热路径必须预先选好「记什么」（步划分、位标记、哈希、计数）。这个分法的难点不在实现而在**预判**：如果一个信号在热路径上没被记下来，事后就无法补算。所以 `Step` 的 21 个字段与十一个位标记实际上是一份「将来可能需要的信号」清单，而这份清单只能靠对业务的理解来定，不能靠通用框架生成。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/telemetry/recorder.py` | `RunRecorder` | 一次运行的记录器，六个 O(1) hook |
| `browser_use/telemetry/recorder.py` | `RunRecorder.attempt_start` / `attempt_ok` / `attempt_failed` | 模型请求三段计时，失败单独留档 |
| `browser_use/telemetry/recorder.py` | `RunRecorder.cell` | 把 REPL 的 helper/cdp 统计折进步与全局 |
| `browser_use/telemetry/recorder.py` | `RunRecorder.build` | 事后折叠成 v3 字段，含余项与首尾保留 |
| `browser_use/telemetry/recorder.py` | `RunRecorder._model_name` | 只承认网关或前缀匹配的模型名 |
| `browser_use/telemetry/recorder.py` | `Step` | 一步的 21 个字段，`__slots__` 省内存 |
| `browser_use/telemetry/recorder.py` | `Step.kind` | 按优先级把一步归类为 deliver/captcha/nav/… |
| `browser_use/telemetry/recorder.py` | `_struggle` | 十一项挣扎指标，含「错误页不算进展」 |
| `browser_use/telemetry/recorder.py` | `_hosts` | 按主机聚合七元组，省缺回写 |
| `browser_use/telemetry/recorder.py` | `_timings` | 按总耗时取前 n 的名字级聚合 |
| `browser_use/telemetry/recorder.py` | `_arrays` | 把保留的步压成列式数组 |
| `browser_use/telemetry/recorder.py` | `CURRENT` | 供无 `RunContext` 的代码取到本运行的记录器 |
| `browser_use/telemetry/recorder.py` | `EV_*` 位标记 | 跨 hook 的稀疏事件，用 `_pending_ev` 累积 |

## 参考

- [browser-use 源码：telemetry/recorder.py](https://github.com/browser-use/browser-use/blob/main/browser_use/telemetry/recorder.py)
- [browser-use 源码：telemetry/stats.py](https://github.com/browser-use/browser-use/blob/main/browser_use/telemetry/stats.py)
- [browser-use 文档：遥测与隐私](https://docs.browser-use.com/customize/telemetry)
- [Python 文档：perf_counter_ns](https://docs.python.org/3/library/time.html#time.perf_counter_ns)

## 与主核 / AI 的关系

运行录制与主核里的**慢查询诊断日志**结构相同：热路径上只累加计数与耗时，语句结束后才汇总成一条可分析记录（耗时分布、扫描行数、争用等待）。主核也有「事后无法补算」的困境——若热路径没统计某个等待事件，事后就永远不知道它发生了。

对 agent 工程的直接价值有三条。其一，**要测量「挣扎」，就必须先把行为分类**。`Step.kind()` 那套优先级链、以及 `DELIVER`/`INTERACT`/`WAITS`/`FILES` 四个集合，是「浪费步数」这个指标能算出来的前提。只记耗时与 token 是无法回答「它为什么慢」的。其二，**聚合指标的边界定义错了，比不测量更糟**。`wasted_steps` 那条注释就是一个实例：漏了「纯文本交付」这一种收尾，会让过半正常运行报出大量虚假浪费，而偏差方向让结果看起来更糟，从而误导优化方向。其三，**隐私与可观测性可以用「只留必要结论」切分**。代码只存哈希用于比较重复、工具名与模型名只承认已知集合，这些都不是妥协而是精确的切分：需要的结论留下，原始数据不留。
