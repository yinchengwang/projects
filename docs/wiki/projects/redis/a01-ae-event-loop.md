---
project: redis
feature: ae-event-loop
page_type: feature
schema_version: 1
refs:
  - path: src/ae.c
    symbols: [aeMain, aeProcessEvents, aeWait]
  - path: src/ae.c
    symbols: [aeCreateFileEvent, aeDeleteFileEvent]
---

# 事件循环与文件事件分派

## 一句话职责

Redis 用自带的极小事件库把「哪些文件描述符可读或可写」变成一轮轮回调，单线程主循环因此只做两件事：等事件、分派事件。

## 机制是什么

`src/ae.c` 里的 ae 是 Redis 自带的事件驱动库，只认识两类事件：**文件事件**（按 fd 记录可读/可写掩码与回调指针）与**时间事件**（到期才触发的定时回调）。多路复用后端在编译期按性能顺序固定，`ae.c` 顶部的条件 include 链是 `HAVE_EVPORT` → `HAVE_EPOLL` → `HAVE_KQUEUE` → `HAVE_SELECT`，运行时不再探测。

服务端主循环是 `aeMain`：`while (!eventLoop->stop)` 死循环，每轮调用 `aeProcessEvents`，参数固定为 `AE_ALL_EVENTS | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP`——既处理文件事件也处理时间事件，并且每轮都触发睡前与睡后两个钩子。

注册侧只有两个入口：`aeCreateFileEvent` 按 fd 登记掩码与回调，`aeDeleteFileEvent` 摘掉某类事件。本页只讲注册、轮询与分派本身，不涉及客户端协议解析、命令语义与后台线程调度。

## 怎么实现的

**注册。** `aeCreateFileEvent` 先检查 fd 是否超出 `setsize`，超出则置 `errno = ERANGE` 并返回 `AE_ERR`。若 fd 超出现有 `events`/`fired` 数组长度，就按「新长度取 `max(旧长度×2, fd+1)`、且不超过 `setsize`」扩容，新增槽位的掩码初始化为 `AE_NONE`。随后交给后端 `aeApiAddEvent`（epoll 下按该 fd 是否已有事件选 `EPOLL_CTL_ADD` 或 `EPOLL_CTL_MOD`，并把旧掩码合并进本次请求），再把回调按位写进 `fe->rfileProc` / `fe->wfileProc`，最后抬高 `maxfd`。

`aeDeleteFileEvent` 在 fd 越界、或该槽位本来就是 `AE_NONE` 时直接返回；若待删掩码含 `AE_WRITABLE`，源码会顺带把 `AE_BARRIER` 一并清掉（注释写明：移除可写时总要移除 barrier）。删完若这个 fd 恰是 `maxfd` 且掩码归零，就向前回扫找到下一个仍有事件的 fd 作为新的 `maxfd`。

**一轮循环。** `aeProcessEvents` 的 flags 决定这一轮做什么：`AE_FILE_EVENTS` 处理文件事件、`AE_TIME_EVENTS` 处理时间事件、`AE_DONT_WAIT` 不阻塞、`AE_CALL_BEFORE_SLEEP` 与 `AE_CALL_AFTER_SLEEP` 决定是否调钩子；两个事件标记都没给时函数立刻返回 0。真正进入阻塞等待的条件是「有 fd 被注册（`maxfd != -1`）」或「要处理时间事件且不要求立即返回」。

超时这样定：参数 flags 或 `eventLoop->flags` 带 `AE_DONT_WAIT` 就用零超时；否则若要处理时间事件，用 `usUntilEarliestTimer` 算出离最近到期定时器还有多久作为超时；两者都不满足则给后端传 `NULL`，即无限等待。源码注释专门解释了「即使没有文件事件要处理，只要还要处理时间事件也照样要调 `aeApiPoll`」——好让线程睡到下一个真正的期限，而不是空转轮询。`beforesleep` 钩子在计算超时**之前**调用，注释指出 `eventLoop->flags` 可能在钩子里被改掉，因此必须在它之后再判断 `AE_DONT_WAIT`，而参数 flags 优先级最高：参数一旦带 `AE_DONT_WAIT`，`eventLoop->flags` 设成什么都忽略。

`aeApiPoll` 返回后，若本轮不要文件事件就把事件数清零；随后调 `aftersleep` 钩子，再进入分派循环。

**分派。** 每个就绪事件按 fd 取回 `fe`，再用 `fe->mask & mask &` 过滤一次——前一个回调可能已经摘掉了这次触发的事件。默认先调可读再调可写，注释给的理由是：这样有机会在处理完查询后立刻把回复发出去。若 `fe->mask` 带 `AE_BARRIER` 则反转成先可写后可读，注释给的理由是让应用能在 `beforeSleep` 里先把东西落盘再回客户端。可读与可写回调是同一个函数指针时只调用一次。需要点明的是，Redis 自身注册文件事件时并未设置 `AE_BARRIER`（模块 API 的 `eventLoopToAeMask` 只映射可读/可写），所以这条反转分支在服务端当前是未被触发的。

**时间事件。** 时间事件在本轮所有文件事件回调**之后**才处理（`processTimeEvents`）。定时器回调的返回值决定下一次到期时间，返回 `AE_NOMORE` 则该定时器被删除。

**`aeWait`。** 这是给「同步等一个 fd」用的旁路接口：不走事件循环，直接用 `poll(2)` 等一个 fd 可读或可写并返回掩码，其中 `POLLERR` 与 `POLLHUP` 都被算作可写。Redis 服务端自己不用它，调用方在 `src/socket.c`（等可写）、`src/asyncio.c`、`src/redis-cli.c` 等处。

## 关键取舍

**源码事实：** 所有文件事件回调在同一条线程上串行执行，命令处理天然不需要锁。**源码事实：** 时间事件是一条无序链表，找最近到期的定时器是 O(N)——`usUntilEarliestTimer` 的注释写明「Redis 目前不需要优化」，并列出有序插入与改用跳表两种可选方案。**源码事实：** 后端在编译期固定，运行时不再探测可用接口。

**架构推论：** 把「注册与分派」和「每轮前后该做什么」拆成 `beforeSleep`/`aftersleep` 两个钩子，是让 ae 库保持业务无关的关键——IO 线程各自持有一个 `aeEventLoop` 并注册了自己的钩子（`src/iothread.c`），同一套库被复用到多线程场景。**架构推论：** 超时由「最近到期定时器」而非固定轮询间隔决定，使空闲时事件循环能睡满；代价是每轮都要遍历一次定时器链表。**架构推论：** 若改成多线程各自跑事件循环，命令执行的串行性就没了——这正是 Redis 选择单线程主循环、只让 I/O 线程做读写的原因。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ae.c` | `aeMain` | 单线程主循环，反复调用 `aeProcessEvents` |
| `src/ae.c` | `aeProcessEvents` | 一轮的事件处理：算超时、跑钩子、分派文件事件、跑时间事件 |
| `src/ae.c` | `aeWait` | 旁路接口，用 `poll` 同步等单个 fd |
| `src/ae.c` | `aeCreateFileEvent` | 注册文件事件，必要时扩容事件数组 |
| `src/ae.c` | `aeDeleteFileEvent` | 摘除文件事件并维护 `maxfd` |

## 参考

- [Redis 源码：ae.c](https://github.com/redis/redis/blob/unstable/src/ae.c)
- [Redis 源码：ae_epoll.c](https://github.com/redis/redis/blob/unstable/src/ae_epoll.c)
- [epoll(7) 手册页](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [Redis 文档：诊断延迟问题](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/latency/)

## 与主核 / AI 的关系

ae 的「注册—等待—分派」三段式可以作为主核 I/O 与后台任务层的事件循环参照，但主核是多写者引擎，Redis 的单线程串行保证不能照搬：主核必须自行定义回调之间的互斥与可见性边界。AI 推理请求在事件循环里只是一次可读事件，回调耗时被拉长时暴露的是事件循环延迟问题，Redis 靠把读写下放给 I/O 线程来缓解，主核若引入长耗时回调需要同类的分层。
