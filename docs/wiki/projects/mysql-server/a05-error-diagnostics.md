---
project: mysql-server
feature: error-diagnostics
page_type: feature
schema_version: 1
refs:
  - path: sql/derror.cc
    symbols: [init_errmessage, mysql_errno_to_builtin]
---

# 错误码与诊断信息

## 一句话职责

把「一个数字错误码」翻译成「一条能发给客户端、也能写进错误日志的诊断信息」：服务端按错误码所属的号段找到对应消息，套用格式化参数，再交给诊断区（Diagnostics area）与协议层发出去。

## 机制是什么

MySQL 的错误诊断由两套并行的编号体系支撑：

- **客户端号段**——错误码落在 `ER_SERVER_RANGE_START`（源码注释写明其值为 error_code 10000）以下，消息文本来自 `messages_to_clients.txt` 生成的编译期内建表。
- **错误日志号段**——错误码从 `ER_SERVER_RANGE_START` 起，消息文本来自 `messages_to_error_log.txt`。

`sql/mysqld.cc` 的 `my_message_sql()` 里有一条硬断言 `assert(error < ER_SERVER_RANGE_START);`，注释解释：给客户端的消息与写错误日志的消息现在分处不同数字区间，若一条消息要同时发给客户端和写进错误日志，必须在两个区间各加一次（一次 `my_error()`，一次 `LogErr()`）。`SIGNAL` 语句是这条规则的例外——源码注释明确说 `SIGNAL` 目前允许设置任意错误码，但把错误日志号段的消息通过 `SIGNAL` 抛出来不会写入该日志。

**诊断信息的三层承载：**

1. **消息表**——错误码到消息模板的映射。运行时从磁盘的 `errmsg.sys` 读入（`sql/derror.cc` 的 `MY_LOCALE_ERRMSGS::read_texts()`），读失败则回退到编译期内建的英文消息。
2. **诊断区**——`THD` 上的 `Diagnostics_area`，记录 `mysql_errno`、消息文本与 `returned_sqlstate`。
3. **协议包**——`sql/protocol_classic.cc` 的 `net_send_error_packet()` 把错误码、SQLSTATE 与消息文本拼成 ERR 包发给客户端。

**边界（与相邻机制的分界）：**

- 不负责错误码的定义表本身（`share/errmsg-utf8.txt` 及其生成产物），那是构建期的事。
- 不负责 SQLSTATE 的具体文案生成，也不负责 SQLSTATE 与消息之间的映射规则——`sql/sql_class.cc` 的 `THD::raise_condition()` 在 `sqlstate == nullptr` 时调 `mysql_errno_to_sqlstate(sql_errno)` 取得 SQLSTATE，属于 C 组。
- 不负责错误信息如何被拼进协议包的具体字节布局（`net_send_error_packet()` 属 A 组协议层）。

## 怎么实现的

### 号段到数组下标的映射：`mysql_errno_to_builtin()`

`sql/derror.cc` 的 `mysql_errno_to_builtin()` 是「逻辑错误码 → 内建消息数组下标」的唯一换算入口。函数体是一个遍历 `NUM_SECTIONS` 个号段的循环：

```cpp
for (i = 0; i < NUM_SECTIONS; i++) {
  if (temp_errno >= errmsg_section_start[i] &&
      temp_errno < (errmsg_section_start[i] + errmsg_section_size[i]))
    return mysql_errno - errmsg_section_start[i] + offset;
  offset += errmsg_section_size[i];
}
return -1; /* General error */
```

要点：

- 命中条件是**闭开区间** `[errmsg_section_start[i], errmsg_section_start[i] + errmsg_section_size[i])`，即号段起点含、终点不含。
- 返回值是**全局数组下标**，由「减去本段起点」加上「前面所有段的累计长度 `offset`」得到。这正是注释里说的「错误消息在数组里顺序存放，但逻辑上按号段组织」——物理顺序与逻辑号段不一致，映射必须靠 `offset` 累加。
- 找不到时返回 `-1`，注释标注为 `General error`。调用方据此判定该错误码没有对应消息。
- `MY_LOCALE_ERRMSGS::lookup()` 用的是同一套号段遍历逻辑，只是把下标换成取 `errmsgs[...]` 字符串；查不到时返回字面量 `"Invalid error code"`。

**注意 `lookup()` 与 `mysql_errno_to_builtin()` 的失败语义不同**：前者返回字符串 `"Invalid error code"`，后者返回 `-1`。`sql/derror.cc` 的 `error_message_fetch()` 走的是后者——它先算 `int i = mysql_errno_to_builtin(mysql_errno);`，只有 `i >= 0` 才取 `sqlstate_map[i].text`，否则返回 `nullptr`。

### 内建消息表的对齐方式

`error_message_fetch()` 与 `read_texts()` 的兜底分支都写成：

```cpp
server_error *sqlstate_map = &error_names_array[1];
```

`error_names_array[1]` 这个「跳过第 0 项」的下标偏移，是为了让数组下标与 `mysql_errno_to_builtin()` 返回的全局下标对齐——`mysql_errno_to_builtin()` 的返回值是从 0 起的连续数组下标，而 `error_names_array` 的第 0 项被占用。

### 启动时装载：`init_errmessage()`

`sql/derror.cc` 的 `init_errmessage()` 做三件事：

1. `my_default_lc_messages->errmsgs->read_texts()`——从磁盘读消息文件。
2. `if (!my_default_lc_messages->errmsgs->is_loaded()) return true;`——读不到消息就算致命错误，注释写明「Fatal error, not able to allocate memory」。
3. **逐号段注册消息查表函数**：对 `NUM_SECTIONS` 个号段逐个调 `my_error_register(error_message_for_client, errmsg_section_start[i], errmsg_section_start[i] + errmsg_section_size[i] - 1)`。注册区间的右端是**闭区间**（`- 1`），与 `mysql_errno_to_builtin()` 的闭开区间判断不同——前者是注册语义，后者是查表语义。

注册用的回调是 `error_message_for_client`，也就是说：**`my_error()` 系列取消息时走的是「面向客户端」的那条取消息路径**。

`init_errmessage()` 的唯一调用方是 `sql/mysqld.cc` 的服务启动流程——`init_errmessage()` 返回非 0 时启动直接 `return 1` 失败。

### 三条取消息路径

`sql/derror.cc` 提供了三个入口，差别在于「用哪种语言」和「给谁用」：

- **`error_message_for_client(int)`**——面向客户端。函数体只有三行：若 `current_thd` 非空则 `ER_THD_NONCONST(current_thd, mysql_errno)`（用会话 `lc_messages` 指定的语言），否则回退到 `error_message_fetch(mysql_errno)`。这是**唯一处理会话语言覆盖**的入口，也是 `init_errmessage()` 注册给 `my_error()` 的回调。
- **`error_message_for_error_log(int)`**——面向服务端错误日志。函数体是 `return error_message_fetch(mysql_errno);`，**不考虑会话语言**，只用配置的默认语言。
- **`error_message_fetch(int)`**（文件内 `static`）——公共底座。若 `my_default_lc_messages` 非空且 `errmsgs->is_loaded()` 为真，返回 `ER_DEFAULT_NONCONST(mysql_errno)`（默认语言）；否则走内建表 `sqlstate_map[i].text`；两者都拿不到则返回 `nullptr`。源码注释写明返回 `nullptr`「可能意味着调用方有 bug」。

对应地还有四个便捷包装：`ER_DEFAULT` 与 `ER_THD`（`#ifndef CHECK_ERRMSG_FORMAT` 包裹的编译期常量版本）、`ER_DEFAULT_NONCONST` 与 `ER_THD_NONCONST`（运行期版本）。`ER_THD` 与 `ER_THD_NONCONST` 都取 `thd->variables.lc_messages->errmsgs->lookup(...)`。

### 磁盘消息文件的格式：`MY_LOCALE_ERRMSGS::read_texts()`

`read_texts()` 从 `lc_messages_dir` 下读 `errmsg.sys`（文件名由文件内 `static const char *ERRMSG_FILE = "errmsg.sys";` 给出）。函数体的关键步骤与判断：

- **路径解析**：`convert_dirname(lang_path, language, NullS)` 后 `my_load_path(lang_path, lang_path, lc_messages_dir)`。
- **两级回退**：第一次按 `lang_path` 拼出的路径开文件失败时，再按 `lc_messages_dir` 本身拼一次（对应 5.5 之前的 `--language` 语义）。第二次仍失败则 `LogErr(ERROR_LEVEL, ER_ERRMSG_CANT_FIND_FILE, name)` 并跳到 `open_err`。成功走第二级但走了第一级失败路径时，会额外 `LogErr(WARNING_LEVEL, ER_ERRMSG_LOADING_55_STYLE, lc_messages_dir)`。
- **文件头校验**：读 32 字节头，要求 `head[0] == 254 && head[1] == 254 && head[2] == 3 && head[3] == 1 && head[4] == 1`，否则跳 `read_err`。这是固定的 5 字节魔数序列 `FE FE 03 01 01`。
- **长度字段**：头里 `uint4korr(head + 6)` 取文本区长度，`uint4korr(head + 10)` 取消息条数 `no_of_errmsgs`。
- **条数下限检查**：`if (no_of_errmsgs < error_messages)` 则报 `ER_ERRMSG_MISSING_IN_FILE` 并跳 `open_err`。其中 `error_messages` 是 `NUM_SECTIONS` 个号段长度之和，在函数开头由循环累加得到——这是「文件里的条数不得少于编译期号段总数」的硬约束。
- **内存布局**：`my_malloc(key_memory_errmsgs_server, length + no_of_errmsgs * sizeof(char *), MYF(0))` 一次性分配「指针区 + 文本区」。文本区起点 `start_of_errmsgs = (uchar *)(errmsgs + no_of_errmsgs)`。
- **偏移先落文本区再复制**：函数先读 `no_of_errmsgs * 4` 字节的偏移表到 `start_of_errmsgs`（文本区），注释解释「不能直接读进 Section1，因为不同处理器架构上指针大小不同」；随后再把 4 字节偏移逐个展开成指针写回 `errmsgs[i]`。最后才把 `length` 字节的文本读进 `start_of_errmsgs`。
- **失败释放**：`read_err_init:` 分支先 `my_free(errmsgs)` 并把 `errmsgs` 置 `nullptr`，注释说明此时「已经丢掉了原本有效的配置，只剩半套消息，必须释放掉然后落到内建兜底」。

### 内建兜底：`open_err:` 分支

`read_texts()` 的 `open_err:` 分支是回退到编译期内建消息的地方：

```cpp
if (!errmsgs) {
  if ((errmsgs = static_cast<const char **>(
           my_malloc(key_memory_errmsgs_server,
                     error_messages * sizeof(char *), MYF(0))))) {
    server_error *sqlstate_map = &error_names_array[1];
    for (uint i = 0; i < error_messages; ++i)
      errmsgs[i] = sqlstate_map[i].text;
  }
}
```

要点：

- 回退是**逐条赋值**：`errmsgs[i] = sqlstate_map[i].text`，取的是内建表的英文原文。
- 分配量是 `error_messages * sizeof(char *)`——**只分配指针区，不分配文本区**。源码注释解释了原因：指针与消息作为一整块连续内存分配和释放，这样关服时仍能被正确回收。
- 即使文件读取失败，这个分支返回的仍是 `true`（失败），函数注释写明「If we can't read the messagefile then it's panic- we can't continue」。**注意这里有个关键语义**：`read_texts()` 返回 `true`，但 `init_errmessage()` 紧接着检查的是 `is_loaded()` 而不是 `read_texts()` 的返回值——内建兜底填好了 `errmsgs` 指针数组，因此 `is_loaded()` 为真，`init_errmessage()` 继续往下走并返回 `false`（成功）。**回退由 `init_errmessage()` 通过 `is_loaded()` 间接完成，不是由 `read_texts()` 的返回值决定。**

### 卸载：`deinit_errmessage()`

`deinit_errmessage()` 对 `NUM_SECTIONS` 个号段逐个调 `my_error_unregister(errmsg_section_start[i], errmsg_section_start[i] + errmsg_section_size[i] - 1)`，把注册区间原样撤销。调用方是 `sql/mysqld.cc` 的关服流程，位置在 `deinit_errmessage();  // finish server errs`。

### 从错误码到客户端：`my_message_sql()` → `raise_condition()` → `net_send_error_packet()`

诊断信息的实际流向是一条四段链：

**第一段——`my_message_sql()`（`sql/mysqld.cc`）。** 这是 mysys 层所有全局错误消息的汇聚点，源码注释写明「All global error messages are sent here where the first one is stored for the client」。函数体的关键动作：

- `if (error == 0)` 时把错误码改成 `ER_UNKNOWN_ERROR`（注释说调用 `my_error()` 时 error 为 0 是 bug，这是「至少阻止新的滥用」的兜底）。
- `if (thd)` 时调 `thd->raise_condition(error, nullptr, Sql_condition::SL_ERROR, str, MyFlags & ME_FATALERROR)`。
- `assert(error < ER_SERVER_RANGE_START);`——号段断言。
- `MyFlags & ME_ERRORLOG` 分支处理「既给客户端又写错误日志」的遗留行为：`EE_OUTOFMEMORY` 或 `HA_ERR_OUT_OF_MEM` 被改写成 `ER_SERVER_OUT_OF_RESOURCES`；`error <= HA_ERR_LAST` 的被改写成 `ER_SERVER_HANDLER_ERROR`；其余若仍小于 `ER_SERVER_RANGE_START` 则用 `LogErr(ER_ERROR_INFO_FROM_DA)`。

`my_message_sql` 也是 `error_handler_hook` 的默认值——`sql/mysqld.cc` 的 `error_handler_hook = my_message_sql;`。

**第二段——`THD::raise_condition()`（`sql/sql_class.cc`）。** 函数体做的事：

- `assert(sql_errno != 0)`，release 构建下若为 0 则改成 `ER_UNKNOWN_ERROR`。
- `if (msg == nullptr) msg = ER_THD_NONCONST(this, sql_errno);`——**没给消息就按会话语言现取**。
- `if (sqlstate == nullptr) sqlstate = mysql_errno_to_sqlstate(sql_errno);`——**没给 SQLSTATE 就按错误码换算**。
- `MYSQL_LOG_ERROR(sql_errno, PSI_ERROR_OPERATION_RAISED)`。
- `handle_condition()` 先给内部错误处理器机会；被处理掉就直接 `return nullptr`。
- 级别为 `SL_ERROR` 时：调 `mysql_audit_notify(this, AUDIT_EVENT(MYSQL_AUDIT_GENERAL_ERROR), sql_errno, msg, strlen(msg))`；置 `is_slave_error = true`（注释：用于捕捉复制期间的查询错误）；若 `!da->is_error()` 则 `da->set_error_status(sql_errno, msg, sqlstate)`。
- 最后 `da->push_warning(this, sql_errno, sqlstate, level, msg)` 压入诊断区条件栈；但**致命 OOM 时跳过**——判断条件是 `is_fatal_error() && (sql_errno == EE_OUTOFMEMORY || sql_errno == ER_OUTOFMEMORY || sql_errno == ER_STD_BAD_ALLOC_ERROR)`，注释解释致命 OOM 时压栈需要分配内存，本身可能失败。

`THD::raise_error()`、`raise_error_printf()`、`raise_warning()`、`raise_warning_printf()`、`raise_note()`、`raise_note_printf()` 这六个便捷函数都走 `ER_THD_NONCONST(this, sql_errno)` 取消息模板；带 `_printf` 的版本用 `vsnprintf(ebuff, sizeof(ebuff), format, args)` 填进 `char ebuff[MYSQL_ERRMSG_SIZE]`。`raise_note()` 与 `raise_note_printf()` 额外检查 `variables.option_bits & OPTION_SQL_NOTES`。

**第三段——诊断区。** `da->set_error_status(sql_errno, msg, sqlstate)` 把三要素存进 `Diagnostics_area`。`sql/sql_class.cc` 的 `Thd_mem_cnt::set_thd_error_status()` 是同一套写法的另一个调用方：`m_thd->get_stmt_da()->set_error_status(m_da.mysql_errno(), m_da.message_text(), m_da.returned_sqlstate())`，外层用 `set_overwrite_status(true)` / `false` 包裹。

`storage/perfschema/pfs.cc` 读诊断区时分三种状态：`DA_EOF` 时 `memcpy(pfs->m_sqlstate, "00000", SQLSTATE_LENGTH)`——**成功时 SQLSTATE 固定为 `00000`**；`DA_ERROR` 时 `pfs->m_sql_errno = da->mysql_errno();` 且 `memcpy(pfs->m_sqlstate, da->returned_sqlstate(), SQLSTATE_LENGTH);`，并 `pfs->m_error_count++`。

**第四段——协议层（`sql/protocol_classic.cc`）。** `net_send_error(THD *thd, uint sql_errno, const char *err)` 做三件事：

- `thd->get_stmt_da()->set_overwrite_status(true);`——注释说这是「即使已经有 OK 或 EOF 也能推出一个错误」的唯一情形。
- `thd->server_status &= ~SERVER_MORE_RESULTS_EXISTS;`——中止多结果集。
- `error = net_send_error_packet(thd, sql_errno, err, mysql_errno_to_sqlstate(sql_errno));`

`net_send_error_packet()` 的包布局（源码注释给出）：`buff[]: sql_errno:2 + ('#':1 + SQLSTATE_LENGTH:5) + MYSQL_ERRMSG_SIZE:512`。实际写入是 `int2store(buff, sql_errno)` 写 2 字节错误码；`client_capabilities & CLIENT_PROTOCOL_41` 为真时，`buff[2] = '#'`（注释：第一个 `#` 是为了协议向后兼容）再 `my_stpcpy(buff + 3, sqlstate)` 拷 5 字节 SQLSTATE；随后 `convert_error_message(converted_err, sizeof(converted_err), character_set_results, err, strlen(err), system_charset_info, &error)` 把消息转成 `character_set_results` 指定的字符集，`strmake(pos, converted_err, MYSQL_ERRMSG_SIZE - 1)` 写入并保证 NUL 结尾；最后 `net_write_command(net, uchar{255}, ...)` 发出（`255` 是 ERR 包标记）。

`net_send_error_packet()` 里还有一条连接级边界：`if (net->vio == nullptr)` 时，若处于 bootstrap 阶段则 `my_message_local(ERROR_LEVEL, EE_NET_SEND_ERROR_IN_BOOTSTRAP, sql_errno, err)` 打到 stderr，随后 `return false`。

注意 `net_send_error()` 有两个重载：THD 版本与 NET 版本。NET 版本用于「新建连接过程中的初始握手」，传 `global_system_variables.character_set_results` 作为目标字符集，且不走 `set_overwrite_status` 那一套。

### 存储引擎错误码的另一条注册路径

存储引擎的错误码有自己的号段与消息表，注册者是 `sql/handler.cc` 的 `ha_init_errors()`：

- `my_malloc(key_memory_errmsgs_handler, HA_ERR_ERRORS * sizeof(char *), MYF(MY_WME | MY_ZEROFILL))` 分配指针数组，用 `MY_ZEROFILL` 避免留下未初始化空洞。
- `SETMSG` 宏展开为 `handler_errmsgs[(nr)-HA_ERR_FIRST] = (msg)`，把 `HA_ERR_*` 号映射到消息。号不是消息的：`SETMSG(HA_ERR_FOUND_DUPP_KEY, ER_DEFAULT(ER_DUP_KEY))`、`SETMSG(HA_ERR_CRASHED, ER_DEFAULT(ER_NOT_KEYFILE))`、`SETMSG(HA_ERR_TOO_BIG_ROW, "Too big row")`——**一部分映射到客户端消息表里的 `ER_*`，一部分是就地写死的英文字面串**。
- 注册：`return my_error_register(get_handler_errmsg, HA_ERR_FIRST, HA_ERR_LAST);`

这是与 `init_errmessage()` **并行**的第二套注册：前者注册服务端 `ER_*` 号段并用 `error_message_for_client` 作回调，后者注册 `HA_ERR_FIRST..HA_ERR_LAST` 区间并用 `get_handler_errmsg` 作回调。卸载由 `sql/handler.cc` 的 `my_error_unregister(HA_ERR_FIRST, HA_ERR_LAST)` 完成。

**架构推论：** 两套注册表并存意味着 `my_error()` 的错误码空间是「按区间查表」而非「按区间查函数」——一个错误码落在哪个区间，就用哪个区间的回调取消息。`sql/handler.cc` 里 `HA_ERR_FIRST`/`HA_ERR_LAST` 与 `sql/derror.cc` 里 `errmsg_section_start[]`/`errmsg_section_size[]` 是两套互不相干的边界定义，`init_errmessage()` 与 `ha_init_errors()` 也各自注册自己那一段。

## 关键取舍

**把「错误码→消息」的查表做成运行时注册的回调，而不是编译期直接索引。** `init_errmessage()` 逐号段调 `my_error_register()`，让 `my_error()` 在运行期按错误码所在区间找到取消息函数。好处是消息来源可替换——`errmsg.sys` 读入后是动态语言内容，存储引擎还能注册自己的一段；代价是取消息多一层间接，且每个区间的边界与回调必须成对维护。

**磁盘消息读失败时回退到编译期内建表，而不是直接启动失败。** `read_texts()` 的 `open_err:` 分支用 `error_names_array[1]` 逐条填 `errmsgs` 指针数组。好处是服务端在语言包缺失的环境里仍能启动并给出英文消息；代价是错误信息语言不受控，且 `read_texts()` 返回 `true`（失败）这个返回值与「最终确实有消息可用」并不一致，必须靠 `init_errmessage()` 改查 `is_loaded()` 才能区分。

**内建兜底只分配指针区，不分配文本区。** `error_messages * sizeof(char *)` 的大小，源码注释解释为「指针与消息作为一整块连续内存分配和释放，关服时仍能被正确回收」。这在内存占用上更省，但代价是这块内存的生命周期与 `errmsgs` 指针数组绑定——`MY_LOCALE_ERRMSGS::destroy()` 里 `my_free(errmsgs); errmsgs = nullptr;` 是成对的两步。

**错误消息的偏移表先读进文本区再展开成指针。** `read_texts()` 明确不把 4 字节偏移直接读进 `errmsgs`（Section1），注释理由是「不同处理器架构上指针大小不同」。这让同一个 `errmsg.sys` 可以在 32 位与 64 位上通用，代价是读盘时多一次内存区域复用。

**客户端号段与错误日志号段分离，并用断言强制。** `ER_SERVER_RANGE_START` 把错误码切成两半，`my_message_sql()` 里 `assert(error < ER_SERVER_RANGE_START);` 把这条纪律钉在编译期检查上。这避免了「客户端错误码被顺手写进错误日志」这类语义混淆，代价是同一条消息要给两边都得写两遍（源码注释里的要求：复制到另一个区间，并分配自己的符号，比如 `ER_DA_EXAMPLE_MESSAGE`）。

**取消息分成「客户端」「错误日志」两个入口，而不是一个。** `error_message_for_client()` 会用会话 `lc_messages` 的语言，`error_message_for_error_log()` 不会。代价是同一个错误码在客户端与错误日志里可能是两种语言的文案。

**协议层自己再兜一次字符集。** `net_send_error_packet()` 用 `convert_error_message()` 把消息从 `system_charset_info` 转到 `character_set_results`。这让错误消息即使来自按 `lc_messages` 语言取出的表，也能按会话的结果字符集发给客户端。

**架构推论：** `ER_DEFAULT` 与 `ER_THD` 这两个宏被 `#ifndef CHECK_ERRMSG_FORMAT` 包住，编译期版本与运行期版本（`ER_DEFAULT_NONCONST` / `ER_THD_NONCONST`）并存。从 `ER_THD` 取 `thd->variables.lc_messages->errmsgs->lookup(...)` 这一实现可以看出，会话级 `lc_messages` 可以在连接建立后被改变，因此错误消息模板不能是编译期常量——这解释了为何需要运行期版本存在。但 `#ifndef CHECK_ERRMSG_FORMAT` 这个宏在本地检出的文件中没有定义点与使用点，**无法从本次检出的源码确认**是哪套构建流程启用它。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/derror.cc` | `init_errmessage` | 启动时装载：读消息文件、检查 `is_loaded()`、逐号段 `my_error_register(error_message_for_client, ...)` 把客户端消息注册给 `my_error()` |
| `sql/derror.cc` | `mysql_errno_to_builtin` | 逻辑错误码到内建消息数组全局下标的换算：遍历 `NUM_SECTIONS` 个号段，闭开区间命中后用「减段起点 + 累加 offset」求下标，未命中返回 `-1` |
| `sql/derror.cc` | `MY_LOCALE_ERRMSGS::read_texts` | 读 `errmsg.sys`：校验 5 字节头魔数 `FE FE 03 01 01`、检查消息条数不少于号段总数、分配指针区加文本区、先读偏移表再展开成指针 |
| `sql/derror.cc` | `MY_LOCALE_ERRMSGS::lookup` | 按同一套号段遍历取消息字符串，未命中返回 `"Invalid error code"` |
| `sql/derror.cc` | `error_message_fetch` | 取消息底座：默认语言已加载则取默认语言，否则取 `error_names_array[1]` 内建表，都不可得返回 `nullptr` |
| `sql/derror.cc` | `error_message_for_client` | 面向客户端的取消息入口：有 `current_thd` 时用会话 `lc_messages` 语言，否则回退到 `error_message_fetch`；被注册为 `my_error()` 的回调 |
| `sql/derror.cc` | `error_message_for_error_log` | 面向错误日志的取消息入口：不考虑会话语言，直接走 `error_message_fetch` |
| `sql/derror.cc` | `deinit_errmessage` | 关服时逐号段 `my_error_unregister(...)` 撤销注册 |
| `sql/derror.cc` | `ER_DEFAULT` / `ER_THD` / `ER_DEFAULT_NONCONST` / `ER_THD_NONCONST` | 四个取消息便捷包装：前两个为编译期常量版（`#ifndef CHECK_ERRMSG_FORMAT` 包裹），后两个为运行期版；`ER_THD` 系取 `thd->variables.lc_messages` |
| `sql/mysqld.cc` | `my_message_sql` | 全局错误消息汇聚点：error 为 0 改 `ER_UNKNOWN_ERROR`、`raise_condition()` 上报、`assert(error < ER_SERVER_RANGE_START)` 断言号段、`ME_ERRORLOG` 分支改写错误码写日志 |
| `sql/mysqld.cc` | `init_errmessage` 调用点 | 启动流程中 `if (init_errmessage()) return 1;`，紧随其后是 `init_client_errs()` |
| `sql/mysqld.cc` | `deinit_errmessage` 调用点 | 关服流程中的 `deinit_errmessage();  // finish server errs` |
| `sql/sql_class.cc` | `THD::raise_condition` | 错误进入诊断区的主干：补消息、补 SQLSTATE、审计通知、`da->set_error_status()`、致命 OOM 时跳过压栈 |
| `sql/sql_class.cc` | `THD::raise_error` / `raise_error_printf` / `raise_warning` / `raise_warning_printf` / `raise_note` / `raise_note_printf` | 六个便捷上报函数：用 `ER_THD_NONCONST` 取模板，`_printf` 版用 `vsnprintf` 填 `MYSQL_ERRMSG_SIZE` 缓冲 |
| `sql/protocol_classic.cc` | `net_send_error` | 面向 THD 的错误发送：`set_overwrite_status(true)`、清 `SERVER_MORE_RESULTS_EXISTS`、转调 `net_send_error_packet` 并附 `mysql_errno_to_sqlstate` |
| `sql/protocol_classic.cc` | `net_send_error_packet` | ERR 包拼装：2 字节错误码、`'#'` 加 5 字节 SQLSTATE、消息按 `character_set_results` 转换后写入 `MYSQL_ERRMSG_SIZE` 缓冲，`255` 标记发出 |
| `sql/handler.cc` | `ha_init_errors` | 存储引擎错误码的并行注册：`SETMSG` 宏把 `HA_ERR_*` 映射到消息（部分取自 `ER_DEFAULT(ER_*)`，部分为就地写死的英文字面串），再 `my_error_register(get_handler_errmsg, HA_ERR_FIRST, HA_ERR_LAST)` |
| `sql/log.cc` | 错误日志取消息调用点 | `error_message_for_error_log(ER_LOG_SLOW_CANNOT_OPEN)` 与 `error_message_for_error_log(ER_LOG_GENERAL_CANNOT_OPEN)` 取消息拼进 `LogEvent().lookup(...)`；另有 `LOG_ITEM_LOG_LOOKUP` 类型把错误码换成消息文本，取不到时用 `"invalid error code"` |
| `storage/perfschema/pfs.cc` | 诊断区状态读取 | `DA_EOF` 时 SQLSTATE 置 `"00000"`；`DA_ERROR` 时读 `da->mysql_errno()` 与 `da->returned_sqlstate()` 并递增错误计数 |

## 与主核 / AI 的关系

主核（`engine/db`）的错误码体系是自持的：内部错误码用 `E_*` 前缀，公开层映射为 `DB_E_*`，错误信息在 `common/` 层生成。对比 MySQL 的做法，有三点差异值得记录：

**架构推论：** 主核没有「客户端号段 / 错误日志号段」这条分界线。它把错误码映射成 `DB_E_*` 时，映射关系是编译期固定的代码，不是运行时注册的回调表。若主核要支持「用一种语言写错误信息、按客户端语言输出」，就必须引入类似 `MY_LOCALE_ERRMSGS` 的语言资源层，并把「取消息」做成可替换的函数指针，而不是在错误处理点直接拼字符串。

**架构推论：** 主核不区分「给调用方的诊断」与「给运维看的日志」两套消息来源。MySQL 用 `ER_SERVER_RANGE_START` 把两者切成两个号段、并用 `my_message_sql()` 里的断言强制纪律，是因为它同时面向客户端协议与运维人员两条完全不同的读者。主核是嵌入式内核、没有协议层，这个分界对它没有直接价值；但如果主核将来要接入远程协议或服务化，同样的分界会出现。

**架构推论：** 主核的错误信息在产生点就地构造字符串，而 MySQL 是「错误码 + 格式化参数」分离：消息模板存在表里，错误点只传参数（如 `my_error(ER_GET_ERRNO, MYF(0), error, "InnoDB error")`）。这种分离让同一个错误码在不同语言下复用同一份参数列表，也让消息表可以被离线翻译工具处理。

对 AI 场景而言，MySQL 的错误诊断链给出了「可被机器解析的失败契约」的一个具体样本：错误码（`mysql_errno`）、SQLSTATE、消息文本三要素在诊断区里同时存在，AI 代理可以直接读 `da->mysql_errno()` 与 `da->returned_sqlstate()` 判断失败类型，而不必解析人类可读的中文/英文消息。

同时有两处「机器不好处理」的地方值得 AI 层注意：其一，`sql/sql_class.cc` 的 `THD::raise_condition()` 在 `sqlstate == nullptr` 时才调 `mysql_errno_to_sqlstate(sql_errno)` 换算 SQLSTATE——**SQLSTATE 是从错误码派生的，不是独立给定的**，因此 AI 代理读到 SQLSTATE 时应知道它是派生值。其二，`sql/derror.cc` 的 `mysql_errno_to_builtin()` 返回 `-1` 与 `MY_LOCALE_ERRMSGS::lookup()` 返回 `"Invalid error code"` 是**两种不同的失败表示**，前者是整数哨兵、后者是字符串哨兵，两处调用方的判空方式不同。

## 参考

- [MySQL 源码：`sql/derror.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/derror.cc)
- [MySQL 源码：`sql/sql_class.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_class.cc)
- [MySQL 源码：`sql/protocol_classic.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/protocol_classic.cc)
- [MySQL 源码：`sql/handler.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/handler.cc)
- [MySQL 源码：`sql/mysqld.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/mysqld.cc)
- [MySQL 参考手册：错误消息参考（Server Error Message Reference）](https://dev.mysql.com/doc/refman/8.0/en/server-error-message-reference.html)
- [MySQL 参考手册：客户端与服务端通信协议（`COM_` 命令）](https://dev.mysql.com/doc/refman/8.0/en/client-server-protocol.html)
- [MySQL 参考手册：InnoDB 错误消息（InnoDB Error Messages）](https://dev.mysql.com/doc/refman/8.0/en/innodb-error-messages.html)
