---
project: openai-agents
feature: sqlite-session
title: SQLite 会话存储与并发安全
page_type: feature
schema_version: 1
refs:
  - path: src/agents/memory/sqlite_session.py
    symbols: [SQLiteSession]
---

# SQLite 会话存储与并发安全

## 一句话职责

用 SQLite 为每个 `session_id` 保存一串 JSON 输入项，并按运行环境选择共享内存连接或文件库线程本地连接，以锁、事务回滚与写操作取消屏障保证异步接口不丢写、不暴露半提交状态。

## 机制是什么

`SQLiteSession` 实现 `SessionABC`。构造参数包括会话 id、数据库路径（默认 `:memory:`）、可配置的两张表名与可选 SessionSettings。默认内存库只活到进程退出；传文件路径则可以跨实例、跨进程复用历史。

数据模型分两张表：session 表保存 session_id 与创建/更新时间；message 表保存自增 id、session_id、序列化的 `message_data` 与时间戳，并以外键级联删除。消息按数据库 id 排序，因此无需为每项额外生成应用层时间顺序。

## 怎么实现的

**连接所有权按库类型分叉。** 内存库只创建一个 `check_same_thread=False` 的共享连接，因 SQLite `:memory:` 的不同连接各有独立数据库；文件库则初始化 schema 后按线程维护连接。每个连接还被记录 owner thread，关闭时可以处理已退出线程留下的连接。

**锁粒度。** 内存库使用实例级 `RLock`，文件库则通过类级字典按 `Path.expanduser().resolve()` 共享同一路径的锁，并用引用计数在最后一个 session 释放时删除锁对象。`_locked_connection` 在锁内取得连接并执行整个同步数据库操作，避免同一实例/同一文件的线程并发操作交错。

**WAL 与跨进程写。** `_configure_connection` 设置 busy timeout 并启用 WAL。若 `PRAGMA journal_mode=WAL` 暂时遇到 locked，它在 busy timeout 截止前以短睡眠重试。WAL 提升读写并行，但库文件初始化仍需处理多个进程同时建库时的短暂锁竞争。

**异步接口的实际执行。** SQLite 操作放进 `asyncio.to_thread`，避免阻塞事件循环；写操作外包 `await_session_mutation`。该 helper 保证调用者取消时不会把正在执行的写任务取消掉，写完成后才向上抛取消信号。异常路径 `_write_connection` 尝试 rollback；rollback 本身失败则使连接失效并从连接集合清除。

**追加与读取。** `_insert_items` 先 `INSERT OR IGNORE` 创建 session 行，再用 `executemany` 插入每个 JSON item，最后更新 `updated_at`。`get_items(limit)` 解析有效 limit，按 id 倒序读尾部 N 项，再反转成时间正序交给调用者。JSON 解码失败时，损坏数据不会被伪造为有效输入项。

**pop 的校验事务。** `_pop_item_with_validation` 在同一写连接事务内删除最新一行并 `RETURNING message_data`；空则提交并返回 None。JSON 损坏的尾项提交删除后继续找前一项。若传入 validate 回调，回调在工作线程中同步执行；校验失败则上下文管理器 rollback，删除回滚，行 id 与位置保留。

**关闭。** `close()` 将 session 标记 closed，在锁内关闭登记连接和 quarantined 连接；内存共享连接也纳入关闭。某连接关闭失败不阻止其它连接关闭，失败连接被隔离并保留错误，最后再报告首个错误。之后即使 `add_items([])` 这样的空写也会先检查 closed 并抛 RuntimeError。

## 关键取舍

**源码事实：** `:memory:` 使用单共享连接，文件库使用线程本地连接；这是 SQLite 内存库按连接隔离的语义直接决定的，不是简单的性能选择。

**源码事实：** SQLite 文件路径锁只在当前进程内共享。跨进程协调依赖 SQLite 自己的文件锁、busy timeout 与 WAL，而不是 Python 锁。

**源码事实：** add/pop/clear 的操作被 `asyncio.to_thread` 执行，数据库动作本身同步；异步只是避免堵塞事件循环。`await_session_mutation` 不提供 timeout、transaction 或 mutual exclusion，分别由连接锁、SQLite 事务与底层忙等待负责。

**源码事实：** `pop_item` 的删除与可选校验在同一事务中。校验异常可回滚删除；而 JSON 本身无法解析的损坏项则提交删除并继续向前找，二者的失败语义不同。

**架构推论：** 内存与文件两条连接策略解决了一个看似矛盾的目标：内存库要跨线程共享同一份状态，文件库又要避免多线程争用同一个 sqlite connection。把策略封装在构造期分支里，调用方仍只面对同一 Session API。

**架构推论：** 取消屏障强调「取消不等于回滚」。持久化层若在后台线程写入，上层 task 已取消并不意味着线程安全地停在了事务边界；等待操作 settle 后再报告取消，能阻止调用方立刻重试造成重复追加。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/memory/sqlite_session.py` | `SQLiteSession.__init__` | 配置表名、连接策略与 session 设置 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession._acquire_file_lock` | 同进程同文件路径共享重入锁 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession._configure_connection` | busy timeout 与 WAL 初始化 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession._create_schema_for_connection` | session/message 表及索引 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession._write_connection` | 异常回滚并在失败时隔离连接 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession.get_items` | 尾部限制查询并按时间顺序返回 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession.add_items` | 线程池追加 JSON 项并提交 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession._pop_item_with_validation` | 事务内删除、校验与回滚 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession.clear_session` | 级联清除指定 session 历史 |
| `src/agents/memory/sqlite_session.py` | `SQLiteSession.close` | 关闭登记连接并释放文件锁引用 |

## 参考

- [openai-agents-python 源码：memory/sqlite_session.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/memory/sqlite_session.py)
- [SQLite 文档：Write-Ahead Logging](https://www.sqlite.org/wal.html)
- [Python 文档：sqlite3 连接与线程](https://docs.python.org/3/library/sqlite3.html)

## 与主核 / AI 的关系

主核中的 WAL 与事务层也依赖「先记录、再提交，失败回滚」，SQLiteSession 把该原则应用到 agent 输入历史，但不维护数据库内核级的恢复日志；持久性由 SQLite 自己承担。应用层的可见边界是每条 message 行与 session id。

与 AutoGen 的会话历史抽象相比，SQLiteSession 将序列化格式、表结构、并发锁与连接生命周期都收在一个实现里；协议层只暴露历史操作。这样换存储后端不改 Runner，但 schema 与 SQL 是实现私有的，不能把数据库文件当成跨版本稳定的公开格式。

对 AI 工程最实用的提醒是异步 API 背后可能仍有同步 I/O。把数据库调用移出事件循环是必要但不充分：还得处理线程连接归属、取消期间写入是否继续、rollback 失败如何处置。这里的 to_thread + 锁 + 写事务回滚是一组整体，而不是可随意拆开的几个优化。