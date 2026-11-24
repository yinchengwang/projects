---
project: mysql-server
feature: binlog-dump-source
page_type: feature
schema_version: 1
refs:
  - path: sql/rpl_source.cc
    symbols: [mysql_binlog_send, com_binlog_dump]
---

# binlog 发送端

## 一句话职责

`sql/rpl_source.cc` 里的 binlog 发送端负责**把源库（source）的 binlog 内容按复制协议推送给从库**：解析从库发来的 dump 请求（文件坐标或 GTID 集合）、定位游标，然后把事件流持续写回网络连接。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责把 binlog 内容按复制协议推送给从库：游标定位、事件读取与网络发送；**不负责复制关系的管理（M02）**、**不负责 GTID 的挑选（M03）**。

## 机制是什么

### 一次 dump 是「一次请求 + 一条长连接」

源库的 binlog 发送是**客户端驱动**的：不是源库主动推，而是从库发一条 `COM_BINLOG_DUMP` / `COM_BINLOG_DUMP_GTID` 命令，源库在该连接上持续回传事件，直到连接断开为止。这条命令的处理入口在 `sql/rpl_source.cc` 的 `com_binlog_dump` 与 `com_binlog_dump_gtid`。

协议 payload 由 `sql/rpl_source.cc` 里 `page_protocol_com_binlog_dump` 的文档表格给出，字段与 `com_binlog_dump` 里的读取顺序一一对应：

| 字段 | 宽度 | `com_binlog_dump` 中的读取语句 |
|---|---|---|
| status | 1 字节 | 命令字本身，值为 `0x12`（`COM_BINLOG_DUMP`） |
| binlog-pos | 4 字节 | `READ_INT(pos, 4)` |
| flags | 2 字节 | `READ_INT(flags, 2)` |
| server-id | 4 字节 | `READ_INT(thd->server_id, 4)` |
| binlog-filename | 变长 | `thd->mem_strdup(packet + 10)` |

源码注释明确说明了 flags 的取值：`can right now has one possible value: ::BINLOG_DUMP_NON_BLOCK`。同时注释解释了为什么 binlog-pos 只读 4 字节：「4 bytes is too little, but changing the protocol would break compatibility. This has been fixed in the new protocol. @see com_binlog_dump_gtid().」——新协议 `com_binlog_dump_gtid` 里位置改读 8 字节（`READ_INT(pos, 8)`）。

### 两种协议：坐标式与 GTID 式

`com_binlog_dump` 与 `com_binlog_dump_gtid` 的差别只在**请求里带什么定位信息**：

- `com_binlog_dump`（坐标式）：带文件名 + 4 字节文件内偏移 `pos`，GTID 集合传 `nullptr`；
- `com_binlog_dump_gtid`（GTID 式）：带文件名 + 8 字节 `pos` + 从库已执行的 GTID 集合（`slave_gtid_executed`），从库把集合用 `add_gtid_encoding()` 编进包体。

GTID 集合的解码在 `com_binlog_dump_gtid` 里：先 `READ_INT(data_size, 4)` 拿到集合字节数，再 `CHECK_PACKET_SIZE(data_size)` 做边界检查，然后 `slave_gtid_executed.add_gtid_encoding(packet_position, data_size)`；成功返回 `RETURN_STATUS_OK` 之外的值就直接 `return true`（结束命令处理）。解码成功后 `slave_gtid_executed.to_string(&gtid_string)` 把集合转成可读字符串，用于通用日志与调试打印。

**边界提示：** GTID 的**挑选**（决定从库里真正要发哪些 GTID）属于 M03，不在本页范围；本页只负责把这个集合从协议里解出来并原样交给下游发送器。

### dump 线程是一种受管的资源

源库对 dump 线程做了**资源限流**。`sql/rpl_source.cc` 里有一个静态单例资源：

```
resource_blocker::Resource &get_dump_thread_resource() {
  static resource_blocker::Resource dump_thread_resource;
  return dump_thread_resource;
}
```

每个 dump 线程要占用一份该资源：`thd->rpl_thd_ctx.dump_thread_user = resource_blocker::User(get_dump_thread_resource());`。拿不到时的处理在 `check_and_report_dump_thread_blocked`：

- 该函数先判断 `if (rpl_user) return true;`——拿到资源即成功；
- 拿不到时遍历 `rpl_user.block_reasons()`，把**所有**阻塞原因用一个空格拼成 `all_msgs`（用 `first` 标志控制分隔符），再 `my_message(ER_SOURCE_FATAL_ERROR_READING_BINLOG, all_msgs.c_str(), MYF(0))` 一次性回报给客户端，最后返回 false。

`com_binlog_dump` / `com_binlog_dump_gtid` 里的调用顺序是「**先占资源、再解析包**」：如果 `thd->rpl_thd_ctx.dump_thread_user` 为空就先 `resource_blocker::User(get_dump_thread_resource())` 去占，然后 `check_and_report_dump_thread_blocked()` 判定，失败直接 `return false`。注释解释了「if we first registered a replica, user will already be initialized and using a resource」——即 `register_replica` 已经占过资源的线程，`com_binlog_dump` 不重复占。

### 权限与「僵尸线程」清理

两条命令入口都做同样的两件事：

1. **权限检查**：`if (check_global_access(thd, REPL_SLAVE_ACL)) return false;`——需要 `REPL_SLAVE_ACL` 全局权限。
2. **清理僵尸 dump 线程**：`kill_zombie_dump_threads(thd);`

`kill_zombie_dump_threads` 处理的是「从库断线重连后，旧 dump 线程还在 `mysql_cond_wait` 等 binlog 更新、导致同一从库出现两条 dump 线程」的问题。源码注释把背景写得很清楚：从库停止后若源库空闲，旧的 dump 线程会一直等到有查询写入 binlog 才结束；因此从库重连发 `COM_BINLOG_DUMP` 时，源库要杀掉同 UUID / 同 server_id 的旧线程。

判定逻辑在回调类 `Find_zombie_dump_thread::operator()` 里：

- 只看**别的**线程（`thd != cur_thd`）且命令是 `COM_BINLOG_DUMP` 或 `COM_BINLOG_DUMP_GTID`；
- 用 `get_replica_uuid(thd, &tmp_uuid)` 取该线程的 UUID（`get_replica_uuid` 从用户变量 `replica_uuid` / `slave_uuid` 中取，取不到返回 nullptr）；
- 若当前线程有 UUID，则用 `strncmp(m_replica_uuid.c_ptr(), tmp_uuid.c_ptr(), UUID_LENGTH)` 比对（长度按 `UUID_LENGTH` 常量截断）；
- 若当前线程**没有** UUID（老版本从库），则退化为「server_id 相同且对方也没有 UUID」：`is_zombie_thread = ((thd->server_id == cur_thd->server_id) && !tmp_uuid.length());`

命中后的处置是：不调 `kill_one_thread()`（注释说那样「will be slow because it will iterate through the list again」），而是直接 `tmp_ptr->duplicate_slave_id = true;` 再 `tmp_ptr->awake(THD::KILL_QUERY);`。

`kill_zombie_dump_threads` 本身还有一个前置守卫：`if (replica_uuid.length() == 0 && thd->server_id == 0) return;`——既没有 UUID 又没有 server_id 时不处理（注释说明真实从库不会为 0，而 `mysqlbinlog` 工具发 `COM_BINLOG_DUMP` 时会是 0）。

## 怎么实现的

### 第一步：`com_binlog_dump` 解析坐标式请求

函数体顺序：

1. `thd->status_var.com_other++;` 计入命令统计；`thd->enable_slow_log = opt_log_slow_admin_statements;` 决定是否写慢日志；
2. `check_global_access(thd, REPL_SLAVE_ACL)` 失败则 `return false`；
3. 占 dump 线程资源（见上），`check_and_report_dump_thread_blocked` 失败则 `return false`；
4. `READ_INT(pos, 4); READ_INT(flags, 2); READ_INT(thd->server_id, 4);` 依次读位置、标志、从库 server_id；
5. `kill_zombie_dump_threads(thd);`；
6. `query_logger.general_log_print(thd, thd->get_command(), "Log: '%s'  Pos: %ld", packet + 10, (long)pos);` 写通用日志；
7. `mysql_binlog_send(thd, thd->mem_strdup(packet + 10), (my_off_t)pos, nullptr, flags);` —— 交给发送器，GTID 集合传 `nullptr`；
8. `unregister_replica(thd, true, true /*need_lock_slave_list=true*/);` 摘掉本线程的从库登记；
9. `return true;`，注释写明「fake COM_QUIT -- if we get here, the thread needs to terminate」。

包体越界的统一处理：`READ` / `READ_INT` / `CHECK_PACKET_SIZE` 三个宏都是先检查 `packet_bytes_todo < BYTES` 再跳到 `error_malformed_packet`，那里 `my_error(ER_MALFORMED_PACKET, MYF(0))` 并 `return true`。

`READ_STRING` 宏的判断是 `if (BUFFER_SIZE <= BYTES) goto error_malformed_packet;`，即缓冲区必须严格大于要写入的长度（留一个字节给结尾的 `'\0'`）。

### 第二步：`com_binlog_dump_gtid` 解析 GTID 式请求

读取顺序与坐标式不同：

1. `READ_INT(flags, 2); READ_INT(thd->server_id, 4); READ_INT(name_size, 4);`
2. `READ_STRING(name, name_size, sizeof(name));` —— `name` 是 `char name[FN_REFLEN + 1]`，`sizeof(name)` 即 `FN_REFLEN + 1`；`READ_STRING` 会额外把 `name[name_size]` 置为 `'\0'`；
3. `READ_INT(pos, 8);` —— 8 字节位置，正是新协议相对旧协议的扩展点；
4. `READ_INT(data_size, 4); CHECK_PACKET_SIZE(data_size);`
5. `slave_gtid_executed.add_gtid_encoding(packet_position, data_size)`，返回非 `RETURN_STATUS_OK` 就 `return true`；
6. `slave_gtid_executed.to_string(&gtid_string);` 生成可读字符串，供 `DBUG_PRINT` 与通用日志使用；随后 `my_free(gtid_string)` 释放；
7. `kill_zombie_dump_threads(thd);`、写通用日志（格式串是 `"Log: '%s' Pos: %" PRIu64 " GTIDs: '%s'"`）；
8. `mysql_binlog_send(thd, name, (my_off_t)pos, &slave_gtid_executed, flags);` —— GTID 集合非空地传下去；
9. `unregister_replica(thd, true, true /*need_lock_slave_list=true*/);` 然后 `return true;`。

`com_binlog_dump_gtid` 里的 `Sid_map sid_map(nullptr /*no sid_lock ...*/)` 与 `Gtid_set slave_gtid_executed(&sid_map)` 都是**局部对象**，注释写明「this is a completely local object」，所以不带 sid 锁。

### 第三步：`mysql_binlog_send` 是真正的发送入口

`mysql_binlog_send` 的函数体极短：

```
void mysql_binlog_send(THD *thd, char *log_ident, my_off_t pos,
                       Gtid_set *slave_gtid_executed, uint32 flags) {
  Binlog_sender sender(thd, log_ident, pos, slave_gtid_executed, flags);

  sender.run();
}
```

它只做两件事：**用五个参数构造 `Binlog_sender`**（头文件 `sql/rpl_binlog_sender.h`），然后调 `sender.run()`。也就是说，事件读取、游标推进、网络发送的细节都在 `Binlog_sender` 里，而 `mysql_binlog_send` 是 `sql/rpl_source.cc` 与发送器之间的唯一交接点。

`sql/rpl_source.cc` 里 `mysql_binlog_send` 有**两个调用方**：`com_binlog_dump` 与 `com_binlog_dump_gtid`。

### 第四步：从库登记表 `slave_list`

`sql/rpl_source.cc` 维护一张全局从库登记表：

```
malloc_unordered_map<uint32, unique_ptr_my_free<REPLICA_INFO>> slave_list{
    key_memory_REPLICA_INFO};
```

键是 `server_id`（`uint32`），值是 `REPLICA_INFO`。登记入口是 `register_replica`：

- 先 `check_access(thd, REPL_SLAVE_ACL, any_db, nullptr, nullptr, false, false)`，失败 `return 1`；
- 占 dump 线程资源，`check_and_report_dump_thread_blocked` 失败 `return 1`；
- `my_malloc(key_memory_REPLICA_INFO, sizeof(REPLICA_INFO), MYF(MY_WME))` 分配 `REPLICA_INFO`，分配失败 `return 1`；
- 边界检查 `if (p + 4 > p_end) { my_error(ER_MALFORMED_PACKET, MYF(0)); return 1; }` 之后读 `thd->server_id = si->server_id = uint4korr(p);`；
- 用 `get_object` 宏依次读 `report-host` / `report-user` / `report-password`，宏内部用 `net_field_length_ll(&p)` 取长度、越界时 `errmsg = msg` 跳 `err`；
- 端口读 2 字节（`uint2korr(p)`），然后 `p += 4` 跳过「fake rpl_recovery_rank」字段——注释写明这是为了兼容带 BUG#13963 补丁的老版本服务器（见 BUG#49259）；
- `if (!(si->master_id = uint4korr(p))) si->master_id = server_id;` —— 源库 id 为 0 时回填为本机 `server_id`；
- `si->thd_id = thd->thread_id();`，并用 `get_replica_uuid(thd, &replica_uuid)` 尝试取从库 UUID 填 `si->valid_replica_uuid` / `si->replica_uuid`；
- 登记时先 `mysql_mutex_lock(&LOCK_replica_list);`，调用 `unregister_replica(thd, false, false /*need_lock_slave_list=false*/)` 清掉同 id 的旧记录（此时已持锁，所以第二个参数传 false 走 `mysql_mutex_assert_owner(&LOCK_replica_list)` 分支），再 `slave_list.emplace(si->server_id, std::move(si))`，`res` 取 `emplace` 的 `second`（即「插入是否成功」），最后解锁返回 `res`。

`unregister_replica(thd, only_mine, need_lock_slave_list)` 的语义：`only_mine` 为真时只删 `it->second->thd_id == thd->thread_id()` 的记录（即只摘自己）；`need_lock_slave_list` 为真时自己加解锁 `LOCK_replica_list`，为假时断言已持锁。

`show_replicas`（对应 `SHOW REPLICAS` / `SHOW SLAVE HOSTS`）在 `LOCK_replica_list` 保护下遍历 `slave_list`，输出列依次为 `Server_Id`、`Host`、`Port`、`Source_Id`、`Replica_UUID`；当 `opt_show_replica_auth_info` 为真时额外输出 `User` 与 `Password` 两列。

### 全局开关

`sql/rpl_source.cc` 顶部定义了两个与 dump 直接相关的全局量：

```
int max_binlog_dump_events = 0;  // unlimited
bool opt_sporadic_binlog_dump_fail = false;
```

`max_binlog_dump_events` 的初值是 `0`，源码注释直接写明含义是 `unlimited`——即默认不限制单次 dump 推送的事件条数；`0` 是「不限制」这一语义的取值。`opt_sporadic_binlog_dump_fail` 初值 `false`，是一个用于偶发失败注入的开关位。

## 关键取舍

### 取舍一：协议向后兼容优先于协议整齐

**放弃的是**「把 `COM_BINLOG_DUMP` 的位置字段统一改成 8 字节」的做法。**换来的是** 老版本从库能继续连新版本源库。

代价就写在 `com_binlog_dump` 的注释里：位置仍然只读 4 字节，源码明说「4 bytes is too little, but changing the protocol would break compatibility」。GTID 式协议因此另起一条命令 `COM_BINLOG_DUMP_GTID`，而不是在原命令上扩字段。

**换成 XXX 方案会怎样：** 若在原命令上直接扩字段，所有老从库会在解析时读到错位的字段，升级路径会被堵死。

### 取舍二：dump 线程是**受限资源**，不是来者不拒

**放弃的是**「每个 `COM_BINLOG_DUMP` 请求都无条件开一条线程」。**换来的是** 源库对并发 dump 数量有可控上界。

代价是连接会被拒：`resource_blocker::User(get_dump_thread_resource())` 拿不到资源时，`check_and_report_dump_thread_blocked` 把**全部**阻塞原因拼成一条消息，用 `ER_SOURCE_FATAL_ERROR_READING_BINLOG` 回报客户端。这个「一次把所有原因都报出来」的做法本身也是取舍：牺牲一点报错简洁度，换取运维一次就能看到全部阻塞点。

**换成 XXX 方案会怎样：** 若不限流，大量从库重连会在源库上堆积阻塞在 `mysql_cond_wait` 的线程，源库的线程数与内存都会被拖垮。

### 取舍三：僵尸线程用「自己动手杀」代替通用 kill 路径

**放弃的是** `kill_one_thread()` 这条通用终止路径。**换来的是** 重连时的清理耗时可控。

源码注释写明了原因：`kill_one_thread()`「will be slow because it will iterate through the list again」，所以 `kill_zombie_dump_threads` 里改为先 `Global_THD_manager::get_instance()->find_thd(&find_zombie_dump_thread)` 拿到 `THD_ptr`，再 `tmp_ptr->duplicate_slave_id = true;` 与 `tmp_ptr->awake(THD::KILL_QUERY);`。

**换成 XXX 方案会怎样：** 每次从库重连都要多遍历一遍全局 THD 列表，重连风暴时这项开销会被放大。

### 取舍四：UUID 优先、server_id 兜底的僵尸判定

**放弃的是**「只用 server_id 判定同一个从库」。**换来的是** 新版从库（5.6 以后带 UUID）能被精确区分。

代价是判定逻辑分叉：`Find_zombie_dump_thread::operator()` 里，有 UUID 时按 `UUID_LENGTH` 长度截断比较 UUID；没有 UUID 时退化为「server_id 相同且对方也没有 UUID」。老从库路径的 `!tmp_uuid.length()` 条件意味着：**一个不带 UUID 的新客户端**不会被误判成僵尸。

**换成 XXX 方案会怎样：** 若只用 server_id，多个共用 server_id 的从库（或 `mysqlbinlog` 工具）会互相杀掉对方的 dump 线程。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/rpl_source.cc | `com_binlog_dump` | `COM_BINLOG_DUMP` 命令入口：权限检查、占 dump 资源、读 4 字节位置 / 2 字节 flags / 4 字节 server_id、清僵尸线程、通用日志、转 `mysql_binlog_send`，末尾 `unregister_replica` 并返回 true（fake COM_QUIT） |
| sql/rpl_source.cc | `com_binlog_dump_gtid` | `COM_BINLOG_DUMP_GTID` 命令入口：读 2 字节 flags / 4 字节 server_id / 4 字节 name_size / 文件名 / 8 字节位置 / 4 字节 GTID 集合长度，用 `add_gtid_encoding` 解码从库 GTID 集合，再转 `mysql_binlog_send` |
| sql/rpl_source.cc | `mysql_binlog_send` | 发送交接点：构造 `Binlog_sender(thd, log_ident, pos, slave_gtid_executed, flags)` 并调 `sender.run()`；调用方为 `com_binlog_dump` 与 `com_binlog_dump_gtid` |
| sql/rpl_source.cc | `check_and_report_dump_thread_blocked` | dump 资源获取结果判定：成功返回 true；失败把所有 `block_reasons()` 用空格拼接后用 `ER_SOURCE_FATAL_ERROR_READING_BINLOG` 回报 |
| sql/rpl_source.cc | `get_dump_thread_resource` | 返回 dump 线程受限资源的静态单例 `resource_blocker::Resource` |
| sql/rpl_source.cc | `kill_zombie_dump_threads` | 重连时清理旧 dump 线程：UUID 或 server_id 为 0 时直接返回，否则用 `Find_zombie_dump_thread` 找到旧线程并 `awake(THD::KILL_QUERY)` |
| sql/rpl_source.cc | `Find_zombie_dump_thread` | 僵尸线程查找回调：只看命令为 `COM_BINLOG_DUMP` / `COM_BINLOG_DUMP_GTID` 的其他线程，有 UUID 按 `UUID_LENGTH` 比对，否则按 server_id 相同且对方无 UUID |
| sql/rpl_source.cc | `get_replica_uuid` | 从用户变量 `replica_uuid` / `slave_uuid` 取从库 UUID，取不到返回 nullptr；取值时持 `thd->LOCK_thd_data` |
| sql/rpl_source.cc | `get_user_var_from_alternatives` | 在 `replica_uuid` 与 `slave_uuid` 两个用户变量名之间择一返回 `user_var_entry` |
| sql/rpl_source.cc | `register_replica` | 从库登记入口：权限检查、占 dump 资源、读 server_id / host / user / password / port、跳过 4 字节 fake rpl_recovery_rank、回填 master_id、登记到 `slave_list` |
| sql/rpl_source.cc | `unregister_replica` | 从 `slave_list` 摘除记录；`only_mine` 为真时只摘本线程，`need_lock_slave_list` 为真时自行加解锁 `LOCK_replica_list` |
| sql/rpl_source.cc | `show_replicas` | `SHOW REPLICAS` / `SHOW SLAVE HOSTS` 实现：持 `LOCK_replica_list` 遍历 `slave_list`，输出 `Server_Id` / `Host` / `Port` / `Source_Id` / `Replica_UUID` |
| sql/rpl_source.cc | `slave_list` | 全局从库登记表，`malloc_unordered_map<uint32, unique_ptr_my_free<REPLICA_INFO>>`，键为 server_id |
| sql/rpl_source.cc | `CHECK_PACKET_SIZE` | 宏：剩余字节不足 BYTES 时跳 `error_malformed_packet` |
| sql/rpl_source.cc | `READ_INT` | 宏：先做 `CHECK_PACKET_SIZE` 再按 `uint##BYTES##korr` 解码并推进读取位置 |
| sql/rpl_source.cc | `READ_STRING` | 宏：要求 `BUFFER_SIZE > BYTES`，拷贝后补 `'\0'` 结尾 |
| sql/rpl_source.cc | `get_object` | 宏：按 `net_field_length_ll` 长度读变长字符串，越界时设 `errmsg` 并跳 `err` |
| sql/rpl_source.cc | `max_binlog_dump_events` | 全局量，初值 `0`，源码注释为 `unlimited`（不限制单次 dump 的事件条数） |
| sql/rpl_source.cc | `opt_sporadic_binlog_dump_fail` | 全局开关，初值 `false`，用于偶发 dump 失败注入 |

## 与主核 / AI 的关系

- 对**主核（存储引擎）**而言：dump 发送端读的是 binlog 文件，而 binlog 文件的写入由 L03 的缓存与刷盘机制负责。发送端的 `Binlog_sender` 从 `sql/rpl_source.cc` 侧只拿到「文件名 + 位置 + GTID 集合 + flags」四元组，具体读文件与推进游标在 `Binlog_sender` 里。**源库的写路径与发送路径由 binlog 文件这一持久介质解耦**，因此发送端不直接触碰 InnoDB 的任何结构。
- 对 **AI / 数据同步链路**而言：`COM_BINLOG_DUMP` 本身就是 CDC 的经典接入协议（`mysqlbinlog --read-from-remote-server` 走的就是这条命令）。`kill_zombie_dump_threads` 的存在保证了断线重连后不会堆积多条流，这对「以 binlog 为输入源的增量同步消费者」是必要条件。
- **架构推论：** 因为 `com_binlog_dump` 的 `flags` 目前只有一个取值 `BINLOG_DUMP_NON_BLOCK`，而 `com_binlog_dump_gtid` 把从库已执行 GTID 集合原样下传给 `Binlog_sender`，所以「跳过从库已执行的 GTID」这一决策的**输入**是在本页产生的，但**执行**发生在 `Binlog_sender` 内——源码在本页并未直接陈述由谁做挑选，这与本机制「不负责 GTID 的挑选（M03）」的边界一致。

## 参考

- [MySQL 参考手册：复制协议与命令（Replication Protocol / COM_BINLOG_DUMP）](https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_replication.html) —— `COM_BINLOG_DUMP` 的 payload 字段定义，与 `sql/rpl_source.cc` 中 `page_protocol_com_binlog_dump` 文档表格对应。
- [MySQL 参考手册：二进制日志（Binary Log）](https://dev.mysql.com/doc/refman/8.0/en/binary-log.html) —— 二进制日志整体语义，用于确认「源库按协议推送 binlog 给从库」这一背景。
- [MySQL 参考手册：服务器系统变量（`log_slow_admin_statements`）](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html) —— `opt_log_slow_admin_statements` 对应的系统变量官方说明。
- [MySQL Server 源码：`sql/rpl_source.cc`（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_source.cc) —— 本页两个 refs 符号（`mysql_binlog_send` / `com_binlog_dump`）及 `com_binlog_dump_gtid` / `kill_zombie_dump_threads` / `register_replica` 的真实实现。
- [MySQL Server 源码：`sql/rpl_binlog_sender.h`（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_binlog_sender.h) —— `Binlog_sender` 的声明，`mysql_binlog_send` 构造并调用它的目标类。
