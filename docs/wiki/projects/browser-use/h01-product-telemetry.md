---
project: browser-use
feature: product-telemetry
title: 产品遥测与离线投递
page_type: feature
schema_version: 1
refs:
  - path: browser_use/telemetry/service.py
    symbols: [ProductTelemetry, capture, telemetry]
  - path: browser_use/telemetry/service.py
    symbols: [get_or_create_device_id, _send_outbox]
---

# 产品遥测与离线投递

## 一句话职责

把「本包自己发出的使用统计」做成**永不阻塞、永不失败、永不丢事件**的一条后台通道：agent 路径上只做一次 dict 更新与一次 `queue.put`，建事件、gzip、POST、重试、离线落盘全在后台线程与退出时的独立进程里完成。

## 机制是什么

`ProductTelemetry` 是每进程一个的实例（经 `telemetry()` 取得），核心是「三个不变量 + 三条兜底路径」：

| 不变量 | 实现方式 |
|---|---|
| agent 路径永不等待网络或磁盘 | `run_started` / `capture_run` 只改字典与入队 |
| 事件永不因为进程短命而丢 | 退出时把队列写盘，交给一个独立 sender 进程 |
| 一次发送失败不影响运行 | 失败进 outbox，下次进程发；异常一律吞掉 |

支撑结构：`Job`（一个待发事件，含 `build(steps, keys)` 回调与 `consent` 标志）、`_outbox`（离线队列）、`inflight` 标记目录（崩溃检测）。

## 怎么实现的

**模块 docstring 就是完整的设计说明，值得逐句读。** 它先说清了不变量：**agent 路径上没有任何东西等网络或磁盘，`run_started()` 与 `capture_run()` 是一次字典更新加一次 `queue.put`**。然后是后台线程的工作：**建事件、gzip、POST（3 秒超时、一次重试）**；失败进 outbox 由下个进程发。然后是退出：**仍在队列里的事件交给一个 detached sender 进程，于是一次性脚本既不等待网络也不丢它的那个事件**。

最后一段讲关闭状态：**`ANONYMIZED_TELEMETRY=false` 或 `browser-use telemetry disable` 之后没有线程、没有网络、没有文件**；只有用户自己发的反馈（`Job.consent`）会发出去，而且**用本进程的 id（不写 `device_id` 文件）、发一次或丢弃（不用 outbox）**。这一句把「关闭」的语义定得很死：不是「尽量少发」，而是「除了用户主动要求的那条，什么都不做」。

**`Job.build` 是一个「分档降级」的回调而不是静态属性。** 它的签名是 `build(steps_kept, keys_per_map)`，`_event` 里按 `LEVELS = ((200, 50), (100, 50), (50, 20), (0, 20))` 逐档尝试，直到 `json.dumps` 后的字节数不超过 `MAX_BYTES = 64 KiB`。这是一个很实用的策略：与其截断 JSON（会产生非法结构），不如让**事件的构造方**按「保留多少步、每个映射保留几个键」重新产出小一点的版本。

它还顺手记录三笔自观测指标：`tel_build_us`（构造耗时微秒）、`tel_bytes`（最终字节数）、`tel_prev_send_fail`（上次发送失败计数）。注释特别说明这些只在 `props` 里已经有 `tel_build_us` 时才加——也就是说某些事件选择不上报自观测。

**`_send` 的重试与出箱判定有一串精确条件。** 流程是：`_online()` 且 `_post()` 成功则 `ok`；否则**睡 1 秒再试一次**。然后：

- `ok` → `send_failures = 0`；
- 不 ok → `send_failures += 1`，并且**只有两个条件都成立才写出箱**：`job.run_id not in self._handed`（退出时已交给 sender，不能再写，否则发两次）且 `self.enabled`（关闭状态下不写盘）。

注释把这两条写在了代码旁边：`not after the exit sender took it (sent twice); not with usage stats off (its caller says it was not sent)`。

**`_online()` 与 TLS 上下文的构造顺序被一次 segfault 修出来。** 它的 docstring 说明：**退出后返回 `False`**，其余时候**在锁里**构造 TLS 上下文。理由是：**如果让 `urlopen` 自己建，证书加载会在 OpenSSL 里跑，而退出流程正在释放 OpenSSL——一个 CLI 调用在它的线程刚开始第一次发送时退出就会 segfault（macOS 上离线调用中占 0.6%）**。

这条 bug 的现象很典型：只在 process 退出与后台线程首次发送的竞态窗口里出现，且只在部分平台。修法是把「构造上下文」这个动作搬到锁内、并在退出后直接拒绝发送。

**`_post` 里有一处 macOS fork 特例。** 注释：**fork 出来的子进程在 macOS 上会在系统代理查询（`_scproxy`、CoreFoundation）里 segfault，所以只用环境变量里的代理（`HTTPS_PROXY`、`NO_PROXY`）构建 opener**。判定条件同时看平台与 `os.getpid() != _IMPORT_PID`。这是一个极窄但真实的平台缺陷，写进注释才不会被后人「简化」掉。

**崩溃检测用「标记文件 + pid 存活 + 时间预算」三件套。** `run_started` 把 run 记进 `_active` 并入队一个 `('mark', run_id)`；线程每 `MARKER_REFRESH_S = 30` 秒对**所有**活动 run 重写一次标记（`queue.get` 超时就刷）。标记内容含 `pid`、`started`、`updated`、`progress()` 与 `props()`，写入用「临时文件 + `os.replace`」保证原子。

**`_send_crashed` 的判定条件写得很讲究。** 它遍历 `inflight/*.json`，**跳过两种**：`pid == os.getpid()`（本进程的，还在跑），以及 `_alive(pid) 且 age < timeout_s + CRASH_GRACE_S`（进程还活着且没超过它自己的超时预算加 15 分钟宽限）。其余一律按 **`status='crashed'`** 上报，并把 `steps` 与 `ms_total` 从标记里补出来。

这个设计的价值在于：**崩溃的运行不是「没有事件」，而是一条 status 不同的事件**。而「多老的标记才算崩」用 `timeout_s` 而不是固定值，因为不同运行的预算不同。

**标记与发送的竞态用「重命名占用」解决。** `_claim(path, kind)` 把 `x.json` 改成 `x.<pid>.crashed`（或 `.sending`），另一次进程启动时若看到 `*.crashed` / `*.sending` 里那个 pid 已死，就改回原名重新处理。这是**无锁的跨进程互斥**：重命名在 POSIX 与 Windows 上都是原子的，而「谁的名字里有活着的 pid」就是谁在占用。

**`_outbox` 同时负责三件清理。** 一是把死掉的 sender 留下的 `.sending` 认领换回 `.gz`（注释：**a sender that died mid-send … left its claim, and nothing else would ever send it**）；二是删掉超过 `OUTBOX_DAYS = 7` 天的文件；三是按 mtime 排序返回。上限由 `_to_outbox` 控制：`OUTBOX_FILES = 20`、`OUTBOX_BYTES = 512 KiB`。

**出箱本身也遵守「离线就让剩下的等」。** `_send_outbox` 在第一次 POST 失败时把认领换回原名、`send_failures += len(files) - i`、然后 `return`——不继续尝试剩余文件。这是一个正确的退让：一个已经证明离线的进程继续逐个试只会拖长时间。

**`_at_exit` 是整篇最复杂的一段，值得拆开。** 它做：

1. **pid 检查**：fork 出的子进程不执行退出逻辑（父进程会做）。
2. 用 `acquire(timeout=0.5)` 尝试拿锁并**置 `_exiting = True`** 再放开。注释解释了这条：**线程会为一个重命名或 TLS 构造持锁约 10 毫秒**，所以要给一个短的等待；而 `_exiting` 一置，线程的 `_claim` 与 `_online` 都会改为「不动作」。
3. **把 `_pending` 全部记进 `_handed`**——这是「交给 sender 了，不要再写出箱」的依据。
4. **即使队列为空也要跑**，因为「一个在事件之前就被杀掉的运行仍要把 outbox 递出去」。这点很细：如果只在有队列项时才启动 sender，一个刚启动就退出的进程会让前面积压的离线事件继续等下去。
5. 对每个 job：`enabled` 且能写出箱就写出箱；否则写**临时文件**（`_once_file`，前缀带 `!` 标记）；都失败则从 `_handed` 里撤掉（留给线程发）。
6. 把 backlog 一起交给 sender。
7. 用 `subprocess.Popen([sys.executable, '-I', '-c', EXIT_SENDER, url, *files])` 起独立进程，Windows 上加 `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW`，POSIX 上 `start_new_session=True`。
8. Popen 失败时**删掉所有 `!` 临时文件**，因为「a temp file would stay forever, with the feedback's text in it」。

第 8 步是隐私考虑而非整洁考虑：临时文件里装着用户主动发的反馈正文，一个没被删掉的文件会一直躺在系统临时目录里。

**`EXIT_SENDER` 是一段内联 Python 源码，不是另一个模块。** 它被 `subprocess` 以 `-I`（隔离模式）执行，做四件事：优先用 truststore 建 TLS 上下文（与主进程一致，含 `SSL_CERT_FILE`/`SSL_CERT_DIR`）；对每个路径**先重命名占用**（`os.replace` 到 `.{pid}.sending`）；POST 成功后删掉；失败则**恢复原名**（普通文件）或删掉（`!` 前缀的一次性文件）。

它还内建「一个超时就全体退让」：`offline` 一旦置真，后续文件不再尝试，`!` 文件直接删除。注释写明了这句的意义：**one timeout: the outbox's files wait for the next process**。

**`_once_file` 的注释解释了为什么一次性文件不能进 outbox。** 原文：**the outbox would keep it and send it once stats are on, without asking again**。也就是说关闭状态下的用户反馈是「这一次的同意」，不能被一个持久队列延后到「统计重新开启之后」再发出去。

**`get_or_create_device_id` 的取值顺序有三层，且注释明确排除了一种做法。** 顺序是 `BROWSER_USE_DEVICE_ID` 环境变量 → 持久化文件 → 进程内随机 UUID。注释：**an id that cannot be stored is this process's own: never one derived from the machine (MAC, hostname)**。也就是说「拿不到稳定 id」时的降级方向是「随机」，绝不能是「机器指纹」。

`_persisted_device_id` 的写入用 `os.link(tmp, path)` 做原子占位，注释：**fails if another process stored its id first: every process keeps the first one**；在不支持硬链接的文件系统上退回 `if not path.exists(): os.replace(...)`。这与 `f02` 里 `name_download` 是同一手法。

**`_own()` 处理 fork。** docstring：**一个 fork 出来的子进程（Linux 上的 multiprocessing 池）一开始没有父进程的线程、队列、运行或事件。** 注释补了一句关键的时序教训：**它在任何公共方法改状态之前被调用；更晚地重置会把刚加进去的那个运行或事件擦掉**。

**每条「永不破坏运行」的路径都有对应注释。** `run_started` / `capture_run` 的 `except Exception` 后面都写着 `- telemetry never breaks a run`；`_loop` 里写着 `- never let one event stop the thread`；`_mark` 写着 `- a read-only disk, or a props()/progress() that raises: never ends the thread`；`_at_exit` 写着 `- exit must never fail because of telemetry`。

**`_loop` 里有一行防内存驻留的赋值。** `job = None` 旁边注释：**this frame would keep the last run's job (its recorder and stats) until the next event**。也就是说局部变量在循环里会一直持有上一个 job 的引用，从而让它的 recorder 与 stats 无法释放——显式置 `None` 才能让 GC 收走。这是一类很难发现的常驻内存增长。

**`_pending` 有 100 条上限，溢出丢最旧并 `forget`。** 注释：**a sender that cannot keep up: drop the oldest**；而 `forget(oldest)` 的理由是**它的运行已经结束，留下的标记会被下一个进程报成 crashed**。这里体现了一个跨机制的耦合：丢弃事件时必须同时清理崩溃标记，否则一个「已经发过事件」的运行会被误报为崩溃。

**关闭开关有两个来源，且「每进程读一次」是性能选择。** `enabled` 是 `CONFIG.ANONYMIZED_TELEMETRY and not harness_disabled()`。`harness_disabled` 读 browser-harness 的环境变量（`BH_TELEMETRY=0`）或它的 `telemetry.json` 里 `{"disabled": true}`，**后者每进程只读一次**（`_harness_config_disabled` 缓存）。这解释了为什么允许两个来源：browser-use 与 browser-harness 是两个包，用户关掉任一个都应该生效。

**首事件时打印一条通知，内容是数据边界清单。** `_start` 里那句 `logger.info` 列出了**会发**什么（模型、步数、成本、时间、helper 名、访问的 URL **不含查询串**、错误消息、最终结果）与**不会发**什么（**绝不含 task 文本、输入文本、代码、密钥**）。把边界写在用户第一次看到的那行日志里，比藏在文档里有效。

## 关键取舍

**源码事实：** agent 路径只有「字典更新 + 入队」，其余全部在后台线程或退出时的独立进程里完成。任何异常都被吞掉并标了 `never breaks a run`。

**源码事实：** 事件按 `LEVELS` 分档重建直到不超过 64 KiB，而不是截断 JSON。构造方（recorder/stats）参与降级。

**源码事实：** 退出时即使队列为空也会起 sender，因为「事件之前就被杀掉的运行」仍需把积压的 outbox 递出去。

**源码事实：** 一次性文件（关闭状态下的用户反馈）不进 outbox，因为它代表「这一次的同意」，不该在统计重新开启后自动发出去。Popen 失败时这些文件被删除，因为里面有反馈正文。

**源码事实：** TLS 上下文在锁内、退出后拒绝发送，修的是「退出释放 OpenSSL 时后台线程首次发送」导致的 segfault。

**源码事实：** 崩溃检测用标记文件 + pid 存活 + `timeout_s + 15 分钟` 判定；跨进程互斥靠原子的重命名占用。

**源码事实：** 设备 id 无法持久化时退回「本进程随机」，明确禁止从机器特征派生。

**源码事实：** 丢弃最旧事件时必须同时清掉它的崩溃标记，否则该运行会被误报为 crashed。

**源码事实：** 循环里显式 `job = None`，否则上一轮 job 的 recorder 与 stats 会被局部变量一直持有。

**源码事实：** 关闭状态有两个来源（browser-use 的 `ANONYMIZED_TELEMETRY` / CLI 与 browser-harness 的环境变量或 `telemetry.json`），后者每进程读一次。

**架构推论：** 这个模块的复杂度几乎全部来自一个约束：**遥测不得进入用户的延迟预算**。一旦接受这个约束，「发送」就必须与「产生」解耦，于是必然要处理队列上限、进程退出、离线积压、崩溃残留、fork 复制、以及跨进程互斥。这些不是过度设计——每一条都对应一类「用户会看到的现象」：脚本退出丢事件、离线时无限积压、崩溃的运行无记录、fork 后所有事件重复发、两个进程同时发同一份文件。而它把「关闭」实现得比「开启」更严格（无线程、无网络、无文件），这一点值得作为默认范式：**一个可选的采集机制，关闭时应当连痕迹都不留**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/telemetry/service.py` | `ProductTelemetry` | 每进程一个的遥测主体 |
| `browser_use/telemetry/service.py` | `ProductTelemetry.capture` / `capture_run` | 入队的两个公开入口，agent 路径上不阻塞 |
| `browser_use/telemetry/service.py` | `ProductTelemetry.run_started` | 写崩溃标记的起点，只改字典并入队 |
| `browser_use/telemetry/service.py` | `ProductTelemetry._event` | 按 `LEVELS` 分档重建到 64 KiB 以内 |
| `browser_use/telemetry/service.py` | `ProductTelemetry._send` | 一次重试、出箱条件、失败计数 |
| `browser_use/telemetry/service.py` | `ProductTelemetry._online` | 退出后拒发；TLS 上下文在锁内构造 |
| `browser_use/telemetry/service.py` | `ProductTelemetry._send_outbox` | 离线积压的重发，首次失败即退让 |
| `browser_use/telemetry/service.py` | `ProductTelemetry._claim` | 原子重命名占用，跨进程互斥 |
| `browser_use/telemetry/service.py` | `ProductTelemetry._send_crashed` | 用 pid 存活与超时预算判定崩溃 |
| `browser_use/telemetry/service.py` | `ProductTelemetry._at_exit` | 队列 + 积压交给 detached sender |
| `browser_use/telemetry/service.py` | `telemetry` | 进程级单例入口 |
| `browser_use/telemetry/service.py` | `get_or_create_device_id` | 三层取值，禁止机器指纹派生 |
| `browser_use/telemetry/service.py` | `_persisted_device_id` | 硬链接原子占位，保留最先写入的那个 id |
| `browser_use/telemetry/service.py` | `Job` | 待发事件与 `consent`（用户主动发送）标志 |
| `browser_use/telemetry/service.py` | `EXIT_SENDER` | 内联的 detached 发送程序 |
| `browser_use/telemetry/service.py` | `_once_file` | 关闭状态下的一次性反馈文件 |
| `browser_use/telemetry/service.py` | `harness_disabled` | browser-harness 侧的关闭开关，每进程读一次 |

## 参考

- [browser-use 源码：telemetry/service.py](https://github.com/browser-use/browser-use/blob/main/browser_use/telemetry/service.py)
- [browser-use 文档：遥测与隐私](https://docs.browser-use.com/customize/telemetry)
- [PostHog 文档：批量摄取 API](https://posthog.com/docs/api/capture)
- [Python 文档：atexit](https://docs.python.org/3/library/atexit.html)

## 与主核 / AI 的关系

这套设计与主核里的**后台写日志/统计收集器**面对同一组约束：主路径不能等 I/O，进程可能被杀，离线期间要能积压。数据库的处理方式是 WAL（顺序写、无锁、崩溃后重放）；这里的 `outbox` + `inflight` 标记是同一个思路在「无事务环境」下的近似实现——没有 WAL 的原子追加，于是用「临时文件 + 原子重命名 + pid 占用」凑出等价保证。

对 agent 工程的直接价值有三条。其一，**遥测的复杂度应该全部集中在「不阻塞」这个约束上**，而不是功能上。这里 600 多行代码里，几乎全部都在处理排队、退出、离线、崩溃、fork，而不是「采集什么」。其二，**「关闭」必须比「开启」更彻底**。关闭状态下无线程、无网络、无文件，唯一例外是用户主动发送的那条且不进持久队列——如果关闭后仍有后台线程或目录残留，用户就没有真正关掉它。其三，**用户主动发送的数据与自动采集的数据是两种同意**。前者不能被一个持久队列延后到「采集重新开启之后」再发；把 `consent` 做成 `Job` 上的显式字段、并让一次性文件不进 outbox，是这条界限的实现位置。
