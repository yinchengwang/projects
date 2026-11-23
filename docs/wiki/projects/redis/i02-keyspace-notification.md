---
project: redis
feature: keyspace-notification
page_type: feature
schema_version: 1
refs:
  - path: src/notify.c
    symbols: [notifyKeyspaceEvent]
---

# 键空间通知

## 一句话职责

`notifyKeyspaceEvent` 是键空间通知唯一的对外入口：它把「某库某键发生了某事件」翻译成 Pub/Sub 频道名与消息体，先无条件分发给 module 订阅者，再按 `server.notify_keyspace_events` 位图判断该类事件是否开启，开启且存在订阅者时向 `__keyspace@<db>__:<key>` 与 `__keyevent@<db>__:<event>` 两类频道投递。真正的投递动作全部委托给 `src/pubsub.c` 的 `pubsubPublishMessage`，本函数不维护任何订阅关系，也不参与键的增删。

## 机制是什么

键空间通知是「事件源在键写入语义里埋点、消费者用普通 Pub/Sub 订阅」的旁路机制。整个机制只依赖三样东西：

- **一张 32 位以内的类位图**：`server.notify_keyspace_events`，声明类型是 `int`，注释写明它是各 `NOTIFY_...` 标志的 xor。标志本体定义在 `src/server.h`，位号散布在 `NOTIFY_KEYSPACE (1<<0)` 到 `NOTIFY_ARRAY (1<<23)` 之间（`NOTIFY_LOADED (1<<12)` 与 `NOTIFY_KEY_TRIMMED (1<<17)` 占的是只给 module 用的号），`ENABLE_GCRA` 编译时再加 `NOTIFY_RATE_LIMIT (1<<24)`。
- **两类频道命名约定**：`__keyspace@<db>__:<key>` 承载事件名，`__keyevent@<db>__:<event>` 承载键名。通知就是 `PUBLISH` 一次这两类频道，因此可以被 `PSUBSCRIBE '__key*__:*'` 这类模式订阅命中。
- **一套配置字符到标志的映射**：`notify-keyspace-events` 用单字符串选类，`keyspaceEventsStringToFlags` 把字符翻成位，`keyspaceEventsFlagsToString` 反过来。

因此通知本身没有独立的消息队列、没有持久化、没有跨节点广播：`notifyKeyspaceEvent` 返回值是 `void`，投递结果（`pubsubPublishMessage` 返回的接收者数）被直接丢弃，语义与 Pub/Sub 一致——断开订阅期间的事件永久丢失。集群下每个节点只为自己那片键空间产生事件，客户端要收全量必须逐节点订阅。

## 怎么实现的

### 一、入口分层：两个公开 API，一个静态实现

`src/notify.c` 里的调用链是三层的：

1. `notifyKeyspaceEvent(int type, const char *event, robj *key, int dbid)` 只做一件事：转调 `notifyKeyspaceEventImpl(type, event, key, dbid, NULL, 0)`。
2. `notifyKeyspaceEventWithSubkeys(int type, const char *event, robj *key, int dbid, robj **subkeys, int count)` 同样只转调 `notifyKeyspaceEventImpl`，只是把 subkeys 数组与数量带进去。
3. `notifyKeyspaceEventImpl` 是静态函数，所有判断与投递都在这里。

源码注释明确写出这套分层的目的：`notifyKeyspaceEvent` 是与旧版本兼容的键级 API，`notifyKeyspaceEventWithSubkeys` 是带子键信息的扩展版，后者在 `subkeys == NULL && count == 0` 时行为与前者的完全一致。任务模块调用的 `RM_NotifyKeyspaceEvent` / `RM_NotifyKeyspaceEventWithSubkeys`（`src/module.c`）就是直接转调这两个函数，并把 `dbid` 换成 `ctx->client->db->id`。

### 二、`notifyKeyspaceEventImpl` 的执行顺序

函数体按固定次序往下走，任何一步的提前返回都意味着后面的频道一个都发不出去：

1. `serverAssert(sdsEncodedObject(key))`：键对象必须是 `OBJ_ENCODING_RAW` 或 `OBJ_ENCODING_EMBSTR`（`sdsEncodedObject` 宏在 `src/server.h` 定义），因为后续要直接读 `key->ptr` 拼进频道名。
2. `moduleNotifyKeyspaceEvent(type, event, key, dbid, subkeys, count)`：**先于配置检查**。源码注释写明这一步「绕过通知配置」，由 module 引擎自己按订阅者登记的掩码过滤。所以即使 `notify-keyspace-events` 是空串，module 回调照收。
3. `if (!(server.notify_keyspace_events & type)) return;`：调用点传进来的 `type` 是单一类标志（如 `NOTIFY_STRING`），类没开就直接结束。
4. 订阅者短路：`dictSize(server.pubsub_patterns) == 0 && kvstoreSize(server.pubsub_channels) == 0` 时返回。`kvstoreSize` 返回的是 `kvstore->key_count`，即频道表中的键总数。源码注释说明这一步是为了「没有任何订阅者时省掉剩余工作」。
5. `createStringObject(event, strlen(event))` 造出 `eventobj`；`ll2string(buf, sizeof(buf), dbid)` 把库号写进 24 字节的 `buf`，得到 `len`。
6. 按位图逐级投递（见下一节），最后 `decrRefCount(eventobj)`。

### 三、键级两级频道

- `server.notify_keyspace_events & NOTIFY_KEYSPACE` 为真时：频道由 `sdsnewlen("__keyspace@", 11)` + 库号 + `sdscatlen(chan, "__:", 3)` + `key->ptr` 拼成，包成 `robj` 后 `pubsubPublishMessage(chanobj, eventobj, 0)`，即消息体是**事件名**。
- `server.notify_keyspace_events & NOTIFY_KEYEVENT` 为真时：频道换成 `sdsnewlen("__keyevent@", 11)` + 库号 + `"__:"` + 事件名，消息体是**键对象本身**。

`eventobj` 只造一次、两级频道共用，函数末尾统一释放一次，所以两级的「事件名」是同一个对象而不是拷贝。每次投递后立刻 `decrRefCount(chanobj)`，频道名对象用完即弃。

这两个前缀 `"__keyspace@"` 与 `"__keyevent@"` 都是 11 字节，`"__:"` 是 3 字节；库号字符串由 `ll2string` 生成，缓冲为 24 字节。`pubsubPublishMessage` 的第三个实参写死为 `0`，含义是走全局频道类型（`pubSubType`）而非分片频道类型（`pubSubShardType`），所以键空间通知能被模式订阅匹配到。

只配 `K` 或只配 `E` 就只投递对应一级；两个都没配时，第 6 步的两个 `if` 都是假，函数在 `decrRefCount` 后返回，一条消息也不发——这与 `redis.conf` 注释「不指定至少一个 K 或 E，就不会投递任何事件」是同一件事。

### 四、子键级四级频道

子键级分支整体被 `if (subkeys != NULL && count > 0)` 包裹，四个频道各自独立判定：

- `NOTIFY_SUBKEYSPACE`（`S`）：频道 `sdsnewlen("__subkeyspace@", 14)` + 库号 + `"__:"` + 键名，消息体是 `事件名 + "|" + 子键载荷`。前置条件是 `!strchr(event, '|')`，源码注释说明事件名里带 `|` 会与分隔符冲突，因此跳过。
- `NOTIFY_SUBKEYEVENT`（`T`）：频道 `sdsnewlen("__subkeyevent@", 14)` + 库号 + `"__:"` + 事件名，消息体是 `键长:键名 + "|" + 子键载荷`，键长由 `ll2string(keylenbuf, sizeof(keylenbuf), keylen)`（缓冲 32 字节）生成。这一级没有事件名过滤。
- `NOTIFY_SUBKEYSPACEITEM`（`I`）：对 `subkeys` 数组**逐元素循环 `count` 次**，每次拼 `sdsnewlen("__subkeyspaceitem@", 18)` + 库号 + `"__:"` + 键名 + `"\n"` + 子键名，消息体是 `eventobj`。前置条件是 `memchr(key->ptr, '\n', sdslen(key->ptr)) == NULL`，键名含换行就整级跳过。
- `NOTIFY_SUBKEYSPACEEVENT`（`V`）：频道 `sdsnewlen("__subkeyspaceevent@", 19)` + 库号 + `"__:"` + 事件名 + `"|"` + 键名，消息体只有子键载荷。前置条件同样是事件名不含 `|`。

**符号名 ≠ 语义**的地方在这里最密集：`I` 的载荷是事件、子键在频道名里；`V` 的载荷是子键、键与事件在频道名里；`S` 与 `T` 才把子键放进载荷。四级的频道前缀长度在源码里逐个写了字面量（14、14、18、19），不是靠 `strlen` 推出来的。

子键载荷统一由静态函数 `catSubkeysPayload(sds dst, robj **subkeys, int count)` 生成，格式是 `<len>:<subkey>[,<len>:<subkey>...]`，逗号只在 `i > 0` 时补，长度用 `ll2string(lenbuf, sizeof(lenbuf), subkeylen)`（缓冲 32 字节）。每个子键对象都要先过 `serverAssert(sdsEncodedObject(subkeys[i]))`。`dst` 传 `NULL` 时函数自己 `sdsempty()`，`V` 级正是这样调用的。

### 五、模块侧调度与重入保护

`moduleNotifyKeyspaceEvent` 在 `src/module.c`：

1. `listLength(moduleKeyspaceSubscribers) == 0` 直接返回，没有 module 订阅者时只剩一次函数调用开销。
2. `enterExecutionUnit(0, 0)`：源码注释写明的意图是「让 notify_callback 里派生的命令与原命令一起被包进同一个 MULTI/EXEC 传播」，只增 `execution_nesting`、不刷新缓存时间。
3. `server.in_keyspace_notification++`（字段声明为 `uint8_t`，注释说明它是计数器，嵌套通知能干净地嵌套）。
4. `type &= ~(NOTIFY_KEYEVENT | NOTIFY_KEYSPACE | NOTIFY_SUBKEYSPACE | NOTIFY_SUBKEYEVENT | NOTIFY_SUBKEYSPACEITEM | NOTIFY_SUBKEYSPACEEVENT)`：把「频道形态」相关的位从掩码里剔掉，只留事件类位供订阅者匹配。
5. 逐订阅者：`sub->event_mask & type` 命中，且 `sub->active == 0` 或模块带 `REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS` 选项；带 subkey 回调且登记了 `REDISMODULE_NOTIFY_FLAG_SUBKEYS_REQUIRED` 的订阅者，在 `subkeys == NULL || count == 0` 时被 `continue` 跳过。
6. 回调前 `sub->active = 1`（防自触发）、`server.allow_access_expired++`、`server.allow_access_trimmed++`，调用 `notify_callback` 或 `notify_callback_with_subkeys`，回调后三个值都还原。
7. 收尾 `server.in_keyspace_notification--` 与 `exitExecutionUnit()`。

两个缓存掩码由 `moduleUpdateKeyspaceSubscribersTypes` 维护：遍历订阅者列表得到 `moduleKeyspaceSubscribersTypes` 与 `moduleKeyspaceSubscribersWithSubkeysTypes`，被 `moduleHasSubscribersForKeyspaceEvent` / `moduleHasSubscribersForKeyspaceEventWithSubkeys` 用 `&` 判定。

### 六、谁来决定要不要收集子键：`isSubkeyNotifyEnabled`

子键级信息是有成本的（要额外分配数组、遍历字段名），因此 `src/notify.c` 提供 `isSubkeyNotifyEnabled(int type)` 做前置判定，函数体按三条依次短路：

1. `moduleHasSubscribersForKeyspaceEventWithSubkeys(type)` 为真则返回 1——有 module 要 subkey，与 Pub/Sub 配置无关；
2. 否则 `dictSize(server.pubsub_patterns) == 0 && kvstoreSize(server.pubsub_channels) == 0` 为真则返回 0——没有任何 Pub/Sub 订阅者；
3. 否则返回 `(server.notify_keyspace_events & type) && (server.notify_keyspace_events & (NOTIFY_SUBKEYSPACE | NOTIFY_SUBKEYEVENT | NOTIFY_SUBKEYSPACEITEM | NOTIFY_SUBKEYSPACEEVENT))`，即「该类事件开启」且「至少开了一种子键频道形态」。

真实调用方在类型实现文件里：`src/t_hash.c` 的 `hashTypeExpire`（用 `FIELDS_STACK_SIZE`，值为 64，做栈上向量）、`src/t_hash.c` 的 `himportSetCommand`（栈上模板容量 `HASH_TMPL_STACK_ENTRIES`，值为 128，超了转堆）、`src/t_set.c` 的 `setTypeExpire`（`SME_STACK_SIZE`，值为 64）。判定为假时它们不建子键数组，但仍调用 `notifyKeyspaceEventWithSubkeys`，把 `subkeys` 传 `NULL`、`count` 传 `0`；真正兜底的是 `notifyKeyspaceEventImpl` 的 `subkeys != NULL && count > 0` 条件，此时只剩键级两级频道。这就是本机制的降级路径：**子键级收集关闭 → 调用方传 NULL/0 → 只剩 K/E 两级**。

### 七、配置层：字符 ↔ 位图

`src/config.c` 把 `notify-keyspace-events` 注册成特殊配置：

- `createSpecialConfig("notify-keyspace-events", NULL, MODIFIABLE_CONFIG, setConfigNotifyKeyspaceEventsOption, getConfigNotifyKeyspaceEventsOption, rewriteConfigNotifyKeyspaceEventsOption, NULL)`，即运行时可改（`MODIFIABLE_CONFIG`）。
- setter 的错误分支有两条：`argc != 1` 时置 `*err = "wrong number of arguments"`；`keyspaceEventsStringToFlags(argv[0])` 返回 `-1` 时置 `*err = "Invalid event class character. Use 'Ag$lshzxeKEtmdnocaSTIV'."`（`ENABLE_GCRA` 下该串多一个 `r`，变成 `'Ag$lshzxeKEtmdnocraSTIV'`）。注意错误串里没有列出 `b`，而 `keyspaceEventsStringToFlags` 的 `case 'b'` 确实接受 `NOTIFY_BITMAP`——两者不一致是源码现状。
- `setConfigNotifyKeyspaceEventsOption` 校验通过后直接把结果写回 `server.notify_keyspace_events = flags`。
- `rewriteConfigNormalizeFileArgs` 里同一组函数也被用来校验配置文件中的写法，`flags == -1` 时返回 `NULL`。
- `rewriteConfigGetDefaultValue` 对该配置返回 `sdsnew("")`，即默认值就是空串；`initServerConfig` 里也把 `server.notify_keyspace_events` 置 0；`redis.conf` 随包给的是 `notify-keyspace-events ""`。

`keyspaceEventsStringToFlags` 的映射是逐字符 `switch`：`A → NOTIFY_ALL`、`g/$/l/s/h/z/x/e/K/E/t/m/d/b/a/n/o/c/S/T/I/V` 各有对应位，`r` 仅在 `ENABLE_GCRA` 下存在，其余字符一律 `return -1`。`A` 是打包别名而非独立位：`NOTIFY_ALL` 的展开式是 `g|$|l|s|h|z|x|e|t|d|b|a` 共 12 个类，`m`、`n`、`o`、`c` 被源码注释明确标注为「故意排除」。

`keyspaceEventsFlagsToString` 是反向：只在 `(flags & NOTIFY_ALL) == NOTIFY_ALL`（全部 12 个类都置位）时才输出单个 `"A"`，否则逐类输出字符；随后 **无条件**按序追加 `n`、`o`、`c`、`K`、`E`、`m`、`S`、`T`、`I`、`V`。因此 `A` 的吸收范围仅限那 12 个类，`K`/`E`/子键四级通道名与 `m/n/o/c` 永远单独出现。

### 八、事件从哪来

`notifyKeyspaceEvent` 的调用点分布在类型实现与键空间层，事件名与类标志是调用点硬编码的：

- 新建键：`src/db.c` 的 `dbAddInternal` 在 `signalKeyAsReady` 之后发 `NOTIFY_NEW, "new"`。
- 覆盖与类型变化：`src/db.c` 的 `setKeyByLink` 发 `NOTIFY_OVERWRITTEN, "overwritten"`；仅当 `oldtype != newtype` 才再发 `NOTIFY_TYPE_CHANGED, "type_changed"`。
- 未命中：`src/db.c` 的 `lookupKey` 在未找到且未带 `LOOKUP_NONOTIFY`/`LOOKUP_WRITE` 时发 `NOTIFY_KEY_MISS, "keymiss"`。
- 过期/淘汰删键：`src/db.c` 的静态函数 `deleteKeyAndPropagate` 按 `notify_type` 二选一发 `NOTIFY_EXPIRED, "expired"` 或 `NOTIFY_EVICTED, "evicted"`，并顺带把延迟监控事件名选成 `"expire-del"` / `"evict-del"`。它的两个包装是 `deleteExpiredKeyAndPropagate`（调用方为 `src/expire.c` 的 `activeExpireCycleTryExpire` 与 `src/db.c` 的 `expireIfNeeded` 删除路径）和 `deleteEvictedKeyAndPropagate`（调用方为 `src/evict.c` 的 `performEvictions`）。
- 字符串：`src/t_string.c` 的 `setGenericCommand` 发 `NOTIFY_STRING, "set"`，带过期时间时再发 `NOTIFY_GENERIC, "expire"`；写入的过期时间已过时的分支只发 `NOTIFY_GENERIC, "del"`。`setCommand` 只负责解析参数并转调它。
- 哈希：`src/t_hash.c` 的 `hsetCommand` 发 `notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hset", ...)`，子键取 `argv[2 + i * 2]` 即全部字段名；`hsetnxCommand` 只带一个字段；`hashTypeExpire` 批量发 `"hexpired"`，字段全空后再发 `"del"`。
- 集合：`src/t_set.c` 的 `setTypeExpire` 同理发 `"sexpired"`，空集合再发 `"del"`。
- 仅 module 可见的两类：`NOTIFY_LOADED`（`src/rdb.c` 载入键时）与 `NOTIFY_KEY_TRIMMED`（`src/cluster_asm.c` 槽迁移裁剪时）通过 `moduleNotifyKeyspaceEvent` 直接分发，不经过 `notifyKeyspaceEvent`，因为它们的位不在 `notify-keyspace-events` 可表达的字符集里。

### 九、通知回调期间的时间线约束

源码注释与代码给出了三条硬约束：

- **回调是同步的**：`moduleNotifyKeyspaceEvent` 里 `enterExecutionUnit` 后当次就调用订阅者，没有队列。
- **不能在回调里改键空间**：`module.c` 的 `RM_AddPostNotificationJobForKey` 在 `server.in_keyspace_notification == 0` 时直接 `return REDISMODULE_ERR` 并 `serverLog(LL_WARNING, ...)`，注释说明这个 API 只能在键空间通知处理函数内部调用，用来把「写操作」推迟到安全时刻。
- **不能嵌套发通知**：`keyedPostNotifBlocksNotification` 在 `server.firing_keyed_post_notif_jobs` 为真时拒绝 `RM_NotifyKeyspaceEvent` / `RM_NotifyKeyspaceEventWithSubkeys`，返回 `REDISMODULE_ERR` 并只告警一次（`keyedPostNotifNotifyWarned` 只由 `firePostExecutionUnitJobs` 与 `firePerKeyJobsBetweenSubcommands` 重置）。

`src/server.h` 里的 `KSN_INVALIDATE_KVOBJ(o)` 展开成 `do { (o) = NULL; } while (0)`，注释说明它是为 RediSearch 这类在通知回调里改键元数据的模块准备的：通知之后本地 kvobj 指针失效，必须置空以防后续访问。`src/t_hash.c`、`src/db.c`、`src/expire.c`、`src/cluster.c` 在通知后都跟着这行宏，这也是调用点附近常出现 `kv` 重新赋值的原因。

## 关键取舍

- **源码事实：** module 分发放在配置检查之前，所以 module 订阅者不受 `notify-keyspace-events` 影响；代价是即使运维把配置置空，module 回调仍会收到每个事件。
- **源码事实：** 「没有订阅者就跳过」用的是 `dictSize(server.pubsub_patterns) == 0 && kvstoreSize(server.pubsub_channels) == 0` 两个全局计数，而不是逐频道查询；代价是只要还有任意一个频道订阅者（哪怕是无关频道），事件仍会完整走完频道名拼接与投递流程。
- **源码事实：** `A` 只是打包别名，`keyspaceEventsFlagsToString` 用 `(flags & NOTIFY_ALL) == NOTIFY_ALL` 做全等判断才输出 `A`；`K`/`E`/子键通道与 `m/n/o/c` 永远单独输出。这是「配置回显」与「语义等价」分离的直接结果。
- **源码事实：** 子键级四级频道把「键/事件/子键」分别塞进频道名或载荷，并用长度前缀编码子键，因此二进制安全的子键名也能表达；代价是订阅者必须按各级不同的解析规则处理载荷，且事件名含 `|`、键名含 `\n` 时对应级别被静默跳过。
- **源码事实：** 子键数组由调用方在建，`isSubkeyNotifyEnabled` 是唯一开关；关闭时调用方仍照常调用 `notifyKeyspaceEventWithSubkeys` 并传 `NULL`/`0`，键级两级频道不受影响。
- **源码事实：** 通知的执行顺序是「先 module 回调，后 Pub/Sub 投递」。源码注释明确承认 module 回调可以重分配 kvobj，这也是 `KSN_INVALIDATE_KVOBJ` 存在的理由。
- **架构推论：** 把通知实现成「拼频道名 + 调用 `pubsubPublishMessage`」而不是独立的消息总线，使通知免费继承 Pub/Sub 的模式匹配、集群分片规则与 ACL 归属；代价是通知也继承了 Pub/Sub 的 at-most-once 语义，订阅断开之间的事件不可回放。
- **架构推论：** 事件类位图与频道形态位（`K`/`E`/`S`/`T`/`I`/`V`）混在同一个 `int` 里，用「先 `& type` 判类、再 `& NOTIFY_KEYSPACE` 判形态」的两段式解析；这是用一张位图同时表达「哪些类」与「发哪几级频道」的紧凑做法，可读性代价是 `moduleNotifyKeyspaceEvent` 必须显式剔掉形态位才能正确匹配订阅者。
- **架构推论：** module 分发绕过配置，说明 module 被视为「内核内消费者」而非「外部订阅者」：它的订阅关系在模块加载期登记、由 `moduleKeyspaceSubscribersTypes` 缓存掩码加速，与运行时可变、依赖字符串解析的 `notify_keyspace_events` 属于两套生命周期。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/notify.c` | `notifyKeyspaceEvent` | 键级公开入口，转调 `notifyKeyspaceEventImpl` 并传 `NULL`/`0` |
| `src/notify.c` | `notifyKeyspaceEventWithSubkeys` | 带子键的公开入口，同一静态实现 |
| `src/notify.c` | `notifyKeyspaceEventImpl` | 全部判定与投递：module 分发 → 类位图检查 → 订阅者短路 → K/E → S/T/I/V |
| `src/notify.c` | `catSubkeysPayload` | 生成 `<len>:<subkey>[,...]` 子键载荷 |
| `src/notify.c` | `keyspaceEventsStringToFlags` | 配置字符 → 位图，非法字符返回 `-1` |
| `src/notify.c` | `keyspaceEventsFlagsToString` | 位图 → 配置字符，`A` 需全等命中 |
| `src/notify.c` | `isSubkeyNotifyEnabled` | 判定是否需要收集子键信息 |
| `src/server.h` | `NOTIFY_*` 宏 / `NOTIFY_ALL` | 类位与 `A` 别名的真实定义（`1<<0` 至 `1<<24`） |
| `src/server.h` | `KSN_INVALIDATE_KVOBJ` | 通知后失效本地 kvobj 指针 |
| `src/config.c` | `setConfigNotifyKeyspaceEventsOption` | 配置 setter，两条错误分支 |
| `src/config.c` | `getConfigNotifyKeyspaceEventsOption` | 配置 getter，回显为字符串 |
| `src/config.c` | `rewriteConfigNotifyKeyspaceEventsOption` | 配置重写，`force` 取决于位图是否非 0 |
| `src/config.c` | `createSpecialConfig` 调用点 | 注册 `notify-keyspace-events` 为 `MODIFIABLE_CONFIG` |
| `src/module.c` | `moduleNotifyKeyspaceEvent` | module 分发器，含执行单元与重入保护 |
| `src/module.c` | `moduleUpdateKeyspaceSubscribersTypes` | 维护两个订阅掩码缓存 |
| `src/module.c` | `moduleHasSubscribersForKeyspaceEventWithSubkeys` | 供 `isSubkeyNotifyEnabled` 使用 |
| `src/module.c` | `RM_NotifyKeyspaceEvent` | 暴露给 module 的通知 API |
| `src/module.c` | `RM_AddPostNotificationJobForKey` | 通知回调内延迟写的安全 API，靠 `in_keyspace_notification` 判定 |
| `src/pubsub.c` | `pubsubPublishMessage` | 实际投递，第三参固定 0 走全局频道类型 |
| `src/pubsub.c` | `pubsubPublishMessageInternal` | 频道集合投递 + 模式匹配投递 |
| `src/db.c` | `dbAddInternal` | 发 `new` 事件 |
| `src/db.c` | `setKeyByLink` | 发 `overwritten` 与 `type_changed` |
| `src/db.c` | `deleteKeyAndPropagate` | 发 `expired` / `evicted`，两个包装为 `deleteExpiredKeyAndPropagate` / `deleteEvictedKeyAndPropagate` |
| `src/db.c` | `lookupKey` | 未命中发 `keymiss` |
| `src/t_hash.c` | `hashTypeExpire` | 批量 `hexpired`，子键收集受 `isSubkeyNotifyEnabled` 控制 |
| `src/t_set.c` | `setTypeExpire` | 批量 `sexpired`，同上 |
| `src/t_string.c` | `setGenericCommand` | `set` / `expire` / `del` 三事件的顺序落点 |

## 与主核 / AI 的关系

对主核（`engine/db/`）而言，这套机制最值得照搬的是**「事件源与消费方解耦、且开关是一张位图」**这一点：写入路径只负责在合适的时机调用一个 `notify(type, event, key, dbid)`，具体发不发、发哪一级、有没有人收，全部在通知函数内一次判定。主核若要把变更暴露给索引刷新、缓存失效或流式查询，可以照抄这条单入口约定，把「是否有人订阅」的快路径短路也一并抄过来——`notifyKeyspaceEventImpl` 在没有任何订阅者时连字符串都不拼，这是热路径上的关键优化。

**架构推论：** 位图同时承载「事件类」与「投递形态」的做法对主核并不合适。Redis 这里受限于一个 `int` 和一套字符串配置语法；主核可以拆成两张表（事件类 → 是否产生、形态 → 发到哪条通道），省掉 `moduleNotifyKeyspaceEvent` 那种「先剔位再匹配」的隐式约定。隐式约定是这类机制最容易在扩展时出错的地方。

**架构推论：** 子键级通知的设计给 AI 场景提供了直接参照：向量索引关心的往往不是「某个键变了」，而是「某个键的第 N 个字段/元素变了」。Redis 用长度前缀子键载荷 + 四级频道，把「按键订阅」「按事件订阅」「按键+子键订阅」「按事件+键订阅」拆成四条独立通道，代价是四套解析规则；主核若做增量索引刷新，应把这四条通道的语义在接口层写死，而不是靠载荷格式隐式区分。

**架构推论：** 通知回调里禁止写键空间、并提供 `RM_AddPostNotificationJobForKey` 推迟写，说明 Redis 把「事件通知」定位成**只读观察**。主核若要支持「收到变更后更新索引」，就必须自带等价的后置作业队列，否则会在回调里递归触发通知或破坏一致性。

## 参考

- [Redis 文档：Keyspace notifications](https://redis.io/docs/latest/develop/pubsub/keyspace-notifications/) —— `K`/`E` 两级频道、配置字符表、事件产生时机与集群下逐节点订阅的口径。
- [Redis 文档：Subkey notifications](https://redis.io/docs/latest/develop/pubsub/subkeyspace-notifications/) —— `S`/`T`/`I`/`V` 四级子键频道、长度前缀载荷格式与三条解析歧义保护规则。
- [Redis 源码：src/notify.c](https://github.com/redis/redis/blob/unstable/src/notify.c) —— 本页 `notifyKeyspaceEventImpl` 全部频道拼接、`catSubkeysPayload` 与两个配置互转函数的真实定义位置。
- [Redis 源码：src/module.c](https://github.com/redis/redis/blob/unstable/src/module.c) —— `moduleNotifyKeyspaceEvent` 分发器、订阅掩码缓存与 `RM_AddPostNotificationJobForKey` 的实现。
- [Redis 源码：src/server.h](https://github.com/redis/redis/blob/unstable/src/server.h) —— `NOTIFY_*` 位定义与 `NOTIFY_ALL` 展开式、`KSN_INVALIDATE_KVOBJ`。
