# -*- coding: utf-8 -*-
"""Redis 机制清单（P4 首批三库之二）。

盘点方法：先按 `src/` 的目录骨架导航，再对候选机制逐个读源文件的函数定义
（`dump_redis_defs.py` 抽取顶层定义，再用词边界匹配确认符号确实在该文件里），
最后按机制簇归并成组。**篇数不是按文件数折算**：`src/` 有 131 个 `.c`，
本清单是逐条盘点后合并/拆分/移组得到的 59 个机制单元。

refs 里的每个 `(path, symbol)` 都已用
`python gen_manifest.py --project redis --spec-module redis_mechanism_specs --verify`
对固定 checkout 校验通过（见 P4 批次 0 记录）。

源码锚点：`redis@d4dee5668f12d3680c8a9b96e0a9cc77caa91b21`
"""

GROUP_ORDER = [
    "A-事件循环与命令分派",
    "B-网络与客户端",
    "C-键空间与过期",
    "D-对象与编码",
    "E-底层数据结构",
    "F-数据类型",
    "G-持久化",
    "H-复制与高可用",
    "I-阻塞与通知",
    "J-事务脚本与模块",
    "K-安全",
    "L-特殊数据类型",
    "M-启动运维与后台任务",
]

MECHANISM_SPECS = [
    # ============ A 事件循环与命令分派 ============
    {
        "feature_group": "A-事件循环与命令分派",
        "feature": "ae-event-loop",
        "title": "事件循环与文件事件分派",
        "page_file": "a01-ae-event-loop.md",
        "boundary": "负责文件事件（可读/可写）与时间事件的注册、轮询与分派，"
                    "以及单线程主循环的推进方式；不负责客户端协议解析（B 组）、"
                    "不负责命令语义（F 组）、不负责后台线程的调度（M 组的 bio）。",
        "refs": [
            {"path": "src/ae.c", "symbols": ["aeMain", "aeProcessEvents", "aeWait"]},
            {"path": "src/ae.c", "symbols": ["aeCreateFileEvent", "aeDeleteFileEvent"]},
        ],
    },
    {
        "feature_group": "A-事件循环与命令分派",
        "feature": "beforesleep-cycle",
        "title": "每轮事件循环前后的固定动作",
        "page_file": "a02-beforesleep-cycle.md",
        "boundary": "负责一轮事件循环开始前/结束后必须做的事（把待发回复写出去、"
                    "处理阻塞客户端的唤醒、刷新持久化缓冲）；不负责定时任务（a03 serverCron）、"
                    "不负责写回本身的协议细节（b04）。",
        "refs": [
            {"path": "src/server.c", "symbols": ["beforeSleep", "afterSleep"]},
        ],
    },
    {
        "feature_group": "A-事件循环与命令分派",
        "feature": "servercron-timers",
        "title": "serverCron 定时任务",
        "page_file": "a03-servercron-timers.md",
        "boundary": "负责按固定频率触发的周期性工作（过期扫描入口、客户端定期维护、"
                    "状态持久化统计等）；不负责事件循环本身（a01），不负责具体的过期或"
                    "淘汰算法（c04/c05 负责算法内部）。",
        "refs": [
            {"path": "src/server.c", "symbols": ["serverCron"]},
        ],
    },
    {
        "feature_group": "A-事件循环与命令分派",
        "feature": "command-table",
        "title": "命令表与命令元数据",
        "page_file": "a04-command-table.md",
        "boundary": "负责命令表的构建（把命令定义填入查找表）、按名/按子命令查找、"
                    "以及 arity 等元数据校验；不负责命令的执行与统计（a05），"
                    "不负责各命令的具体语义（F/L 组）。",
        "refs": [
            {"path": "src/server.c", "symbols": ["populateCommandTable", "lookupCommand", "commandCheckArity"]},
        ],
    },
    {
        "feature_group": "A-事件循环与命令分派",
        "feature": "command-dispatch-call",
        "title": "命令执行与传播登记",
        "page_file": "a05-command-dispatch-call.md",
        "boundary": "负责一次命令调用的执行编排：预检、调用命令实现、耗时/统计记账、"
                    "以及把写命令登记到 AOF 与复制传播缓冲；不负责命令表查找（a04）、"
                    "不负责协议解析与回复缓冲（B 组）、不负责 AOF 文件写入（g02）。",
        "refs": [
            {"path": "src/server.c", "symbols": ["call", "processCommand", "alsoPropagate"]},
        ],
    },
    {
        "feature_group": "A-事件循环与命令分派",
        "feature": "client-input-parsing",
        "title": "客户端输入解析",
        "page_file": "a06-client-input-parsing.md",
        "boundary": "负责把 socket 上读到的字节解析成命令行（多行批量协议与内联命令两种形态），"
                    "并在解析完成后交给命令分派；不负责客户端对象的创建/释放（b01）、"
                    "不负责回复编码（b02）、不负责命令语义。",
        "refs": [
            {"path": "src/networking.c", "symbols": ["readQueryFromClient", "processInputBuffer"]},
            {"path": "src/networking.c", "symbols": ["processMultibulkBuffer", "processInlineBuffer"]},
        ],
    },

    # ============ B 网络与客户端 ============
    {
        "feature_group": "B-网络与客户端",
        "feature": "client-lifecycle",
        "title": "客户端对象生命周期",
        "page_file": "b01-client-lifecycle.md",
        "boundary": "负责客户端对象的创建、复位与释放（含连接级资源与查询缓冲的持有）；"
                    "不负责协议解析（a06）、不负责输出缓冲上限（b03）、"
                    "不负责阻塞状态机（i01）。",
        "refs": [
            {"path": "src/networking.c", "symbols": ["createClient", "freeClient", "resetClient"]},
        ],
    },
    {
        "feature_group": "B-网络与客户端",
        "feature": "reply-buffers",
        "title": "回复编码与缓冲",
        "page_file": "b02-reply-buffers.md",
        "boundary": "负责把结果编码成协议回复并写入客户端输出缓冲（整数、数组、批量串、错误、空值）；"
                    "不负责实际发送到 socket（b04）、不负责缓冲上限与关闭策略（b03）。",
        "refs": [
            {"path": "src/networking.c",
             "symbols": ["addReplyBulkCBuffer", "addReplyErrorObject", "addReplyLongLong",
                          "addReplyArrayLen", "addReplyNull"]},
        ],
    },
    {
        "feature_group": "B-网络与客户端",
        "feature": "output-buffer-limits",
        "title": "输出缓冲上限与超限处置",
        "page_file": "b03-output-buffer-limits.md",
        "boundary": "负责按客户端类型（普通/主从/pubsub）判定输出缓冲是否超限，"
                    "以及超限时关闭客户端的路径；不负责回复编码（b02）、"
                    "不负责写 socket 的成功/失败处理（b04）。",
        "refs": [
            {"path": "src/networking.c",
             "symbols": ["checkClientOutputBufferLimits", "closeClientOnOutputBufferLimitReached"]},
        ],
    },
    {
        "feature_group": "B-网络与客户端",
        "feature": "socket-writeback",
        "title": "回复写回 socket",
        "page_file": "b04-socket-writeback.md",
        "boundary": "负责把输出缓冲里的回复写到 socket，处理部分写、注册可写事件、"
                    "以及写失败后的断开；不负责回复编码（b02），"
                    "不负责输出缓冲上限判定（b03）。",
        "refs": [
            {"path": "src/networking.c", "symbols": ["writeToClient", "sendReplyToClient"]},
        ],
    },
    {
        "feature_group": "B-网络与客户端",
        "feature": "clients-cron-maintenance",
        "title": "客户端定期维护",
        "page_file": "b05-clients-cron-maintenance.md",
        "boundary": "负责周期性缩减过长/过空的查询缓冲与输出缓冲、统计占用最大的客户端；"
                    "不负责客户端创建与释放（b01），不负责主动淘汰（b06）。",
        "refs": [
            {"path": "src/server.c",
             "symbols": ["clientsCron", "clientsCronResizeQueryBuffer", "clientsCronResizeOutputBuffer"]},
        ],
    },
    {
        "feature_group": "B-网络与客户端",
        "feature": "client-eviction",
        "title": "客户端按内存驱逐",
        "page_file": "b06-client-eviction.md",
        "boundary": "负责在内存压力下按占用挑选并断开客户端（客户端级淘汰）；"
                    "不负责键级淘汰（c05 的 maxmemory 淘汰），"
                    "不负责输出缓冲上限的逐客户端判定（b03）。",
        "refs": [
            {"path": "src/server.c", "symbols": ["clientEvictionAllowed"]},
        ],
    },

    # ============ C 键空间与过期 ============
    {
        "feature_group": "C-键空间与过期",
        "feature": "keyspace-read-lookup",
        "title": "键的读侧查找",
        "page_file": "c01-keyspace-read-lookup.md",
        "boundary": "负责读路径上按键查找值、过期检查与命中/未命中统计；"
                    "不负责写侧插入与覆盖（c02）、不负责过期算法本身（c04）、"
                    "不负责数据库选号与多库切换。",
        "refs": [
            {"path": "src/db.c", "symbols": ["lookupKeyRead", "lookupKeyReadWithFlags", "lookupKeyWrite"]},
        ],
    },
    {
        "feature_group": "C-键空间与过期",
        "feature": "keyspace-write-delete",
        "title": "键的写侧插入与删除",
        "page_file": "c02-keyspace-write-delete.md",
        "boundary": "负责新增键、覆盖键与删除（含同步删除与延迟删除的选择路径）；"
                    "不负责读侧查找与过期判定（c01）、不负责对象编码转换（d02）、"
                    "不负责键空间通知的投递（i02）。",
        "refs": [
            {"path": "src/db.c", "symbols": ["dbAdd", "setKey", "dbDelete", "dbSyncDelete"]},
        ],
    },
    {
        "feature_group": "C-键空间与过期",
        "feature": "expiration-metadata",
        "title": "过期时间元数据",
        "page_file": "c03-expiration-metadata.md",
        "boundary": "负责记录与读取键的过期时间（挂在键空间上的绝对时间戳表示）；"
                    "不负责主动扫描过期键（c04）、不负责内存淘汰（c05）。",
        "refs": [
            {"path": "src/db.c", "symbols": ["setExpire"]},
        ],
    },
    {
        "feature_group": "C-键空间与过期",
        "feature": "active-expire",
        "title": "主动过期扫描",
        "page_file": "c04-active-expire.md",
        "boundary": "负责按时间配额分批扫描并删除已过期键的算法（采样、轮转、时间预算）；"
                    "不负责过期元数据的写入（c03）、不负责内存淘汰（c05）。",
        "refs": [
            {"path": "src/expire.c", "symbols": ["activeExpireCycle"]},
        ],
    },
    {
        "feature_group": "C-键空间与过期",
        "feature": "memory-eviction",
        "title": "内存淘汰",
        "page_file": "c05-memory-eviction.md",
        "boundary": "负责 maxmemory 压力下按键挑选候选并删除的淘汰策略流程；"
                    "不负责过期扫描（c04）、不负责客户端级驱逐（b06）、"
                    "不负责内存分配器本身（e07）。",
        "refs": [
            {"path": "src/evict.c", "symbols": ["performEvictions"]},
        ],
    },
    {
        "feature_group": "C-键空间与过期",
        "feature": "keyspace-scan",
        "title": "键空间游标扫描",
        "page_file": "c06-keyspace-scan.md",
        "boundary": "负责 SCAN 类命令的游标推进与结果裁剪语义（含多编码下的遍历跳过策略）；"
                    "不负责哈希表渐进 rehash 本身（e01）、不负责键匹配之后的命令语义。",
        "refs": [
            {"path": "src/db.c", "symbols": ["scanGenericCommand"]},
        ],
    },

    # ============ D 对象与编码 ============
    {
        "feature_group": "D-对象与编码",
        "feature": "robj-refcount",
        "title": "对象与引用计数",
        "page_file": "d01-robj-refcount.md",
        "boundary": "负责值对象的引用计数增减与共享对象的持有约定；"
                    "不负责按类型释放底层结构（d03）、不负责编码选择（d02）。",
        "refs": [
            {"path": "src/object.c", "symbols": ["incrRefCount", "decrRefCount", "createStringObject"]},
        ],
    },
    {
        "feature_group": "D-对象与编码",
        "feature": "object-encoding-transition",
        "title": "对象编码与转换",
        "page_file": "d02-object-encoding-transition.md",
        "boundary": "负责在对象创建/修改时决定使用哪种内部编码，并在读写需要时把编码解码成"
                    "可直接操作的形态（如需要时把紧凑编码解出来）；不负责各类型的增删语义（F 组）、"
                    "不负责底层结构的实现（E 组）。",
        "refs": [
            {"path": "src/object.c", "symbols": ["tryObjectEncoding", "getDecodedObject", "dupStringObject"]},
        ],
    },
    {
        "feature_group": "D-对象与编码",
        "feature": "object-free-by-type",
        "title": "按类型释放值对象",
        "page_file": "d03-object-free-by-type.md",
        "boundary": "负责按对象的实际类型与编码调用对应的底层释放函数；"
                    "不负责引用计数判定（d01）、不负责底层结构的内存管理（e07）。",
        "refs": [
            {"path": "src/object.c",
             "symbols": ["freeStringObject", "freeListObject", "freeHashObject",
                          "freeSetObject", "freeZsetObject"]},
        ],
    },
    {
        "feature_group": "D-对象与编码",
        "feature": "intset-encoding",
        "title": "整数集合编码",
        "page_file": "d04-intset-encoding.md",
        "boundary": "负责整数集合的插入与查找（含宽度升级），以及集合类型在整数集合与"
                    "通用编码之间的切换判据；不负责哈希表编码的集合（F 组的 set-type 负责语义，"
                    "d02 负责编码选择）。",
        "refs": [
            {"path": "src/intset.c", "symbols": ["intsetAdd", "intsetSearch"]},
            {"path": "src/t_set.c", "symbols": ["setTypeMaybeConvert"]},
        ],
    },
    {
        "feature_group": "D-对象与编码",
        "feature": "ziplist-legacy-encoding",
        "title": "旧版紧凑列表编码",
        "page_file": "d05-ziplist-legacy-encoding.md",
        "boundary": "负责旧紧凑编码的定位/遍历语义（供兼容读取与升级路径使用）；"
                    "不负责当前主用的 listpack（e05），不负责列表/哈希/有序集合的"
                    "上层命令语义（F 组）。",
        "refs": [
            {"path": "src/ziplist.c", "symbols": ["ziplistIndex"]},
        ],
    },

    # ============ E 底层数据结构 ============
    {
        "feature_group": "E-底层数据结构",
        "feature": "dict-hashtable",
        "title": "哈希表与渐进 rehash",
        "page_file": "e01-dict-hashtable.md",
        "boundary": "负责哈希表的插入/查找与渐进式 rehash 的推进；"
                    "不负责为遍历提供稳定游标（e02）、不负责键空间语义（C 组）。",
        "refs": [
            {"path": "src/dict.c", "symbols": ["dictAdd", "dictFind", "dictRehash"]},
        ],
    },
    {
        "feature_group": "E-底层数据结构",
        "feature": "dict-scan-iteration",
        "title": "哈希表的反向遍历与安全游标",
        "page_file": "e02-dict-scan-iteration.md",
        "boundary": "负责在重哈希状态下仍能安全推进的游标扫描，以及取迭代器逐桶遍历、"
                    "删键时不解引用失效节点的路径；不负责 rehash 的推进策略（e01）。",
        "refs": [
            {"path": "src/dict.c", "symbols": ["dictScan", "dictGetIterator", "dictNext", "dictUnlink"]},
        ],
    },
    {
        "feature_group": "E-底层数据结构",
        "feature": "sds-string",
        "title": "动态字符串",
        "page_file": "e03-sds-string.md",
        "boundary": "负责带长度头的动态字符串的分配、扩容、追加与释放冗余空间；"
                    "不负责对象层引用计数（d01）、不负责总内存分配器（e07）。",
        "refs": [
            {"path": "src/sds.c", "symbols": ["sdsnewlen", "sdsMakeRoomFor", "sdsRemoveFreeSpace", "sdscatlen"]},
        ],
    },
    {
        "feature_group": "E-底层数据结构",
        "feature": "rax-radix-tree",
        "title": "压缩前缀树",
        "page_file": "e04-rax-radix-tree.md",
        "boundary": "负责按字节前缀组织的有序树插入与查找/遍历（用于流条目索引等场景）；"
                    "不负责跳表（f06）、不负责有序集合的命令语义（f05）。",
        "refs": [
            {"path": "src/rax.c", "symbols": ["raxInsert", "raxFind", "raxSeek", "raxNext", "raxNew"]},
        ],
    },
    {
        "feature_group": "E-底层数据结构",
        "feature": "listpack",
        "title": "紧凑列表 listpack",
        "page_file": "e05-listpack.md",
        "boundary": "负责 listpack 的追加/读取/插入/替换/删除与元素定位；"
                    "不负责把多个 listpack 串起来的链表（e06）、不负责旧 ziplist 兼容（d05）。",
        "refs": [
            {"path": "src/listpack.c", "symbols": ["lpAppend", "lpGet", "lpGetValue", "lpDelete", "lpReplace", "lpInsert"]},
        ],
    },
    {
        "feature_group": "E-底层数据结构",
        "feature": "quicklist",
        "title": "quicklist 分片链表",
        "page_file": "e06-quicklist.md",
        "boundary": "负责把多个紧凑节点组织成双向链表，并在节点内做插入/替换/删除与节点拆分合并；"
                    "不负责单节点内部的紧凑编码（e05），不负责列表命令语义（f02）。",
        "refs": [
            {"path": "src/quicklist.c",
             "symbols": ["quicklistPush", "quicklistCreate", "quicklistDelIndex", "quicklistReplaceAtIndex"]},
        ],
    },
    {
        "feature_group": "E-底层数据结构",
        "feature": "memory-allocator",
        "title": "内存分配与用量统计",
        "page_file": "e07-memory-allocator.md",
        "boundary": "负责内存申请的封装（分配/释放/realloc）、进程用量统计与 RSS/私有脏页探测；"
                    "不负责淘汰策略（c05）、不负责各结构的布局（e03/e05/e06）。",
        "refs": [
            {"path": "src/zmalloc.c",
             "symbols": ["zmalloc", "zfree", "zrealloc", "zmalloc_used_memory",
                          "zmalloc_get_rss", "zmalloc_get_private_dirty", "zstrdup"]},
        ],
    },

    # ============ F 数据类型 ============
    {
        "feature_group": "F-数据类型",
        "feature": "string-type",
        "title": "字符串类型",
        "page_file": "f01-string-type.md",
        "boundary": "负责字符串的读写命令语义（设置/读取/取子串）；"
                    "不负责位操作（l01）、不负责对象编码选择（d02）。",
        "refs": [
            {"path": "src/t_string.c", "symbols": ["setGenericCommand", "getGenericCommand", "getrangeCommand"]},
        ],
    },
    {
        "feature_group": "F-数据类型",
        "feature": "list-type",
        "title": "列表类型",
        "page_file": "f02-list-type.md",
        "boundary": "负责列表的推入/弹出命令语义与在需要时把紧凑编码升级为通用编码；"
                    "不负责底层链表结构（e06）、不负责阻塞客户端的解除（i01）。",
        "refs": [
            {"path": "src/t_list.c", "symbols": ["listTypePush", "listTypePop", "pushGenericCommand", "listTypeTryConversion"]},
        ],
    },
    {
        "feature_group": "F-数据类型",
        "feature": "hash-type",
        "title": "哈希类型",
        "page_file": "f03-hash-type.md",
        "boundary": "负责哈希字段的读写/删除/长度与字段迭代，以及紧凑编码下的转换触发；"
                    "不负责通用哈希表实现（e01）、不负责键空间读写（C 组）。",
        "refs": [
            {"path": "src/t_hash.c",
             "symbols": ["hashTypeSet", "hashTypeGetValue", "hashTypeDelete",
                          "hashTypeLength", "hashTypeTryConversion", "hashTypeInitIterator"]},
        ],
    },
    {
        "feature_group": "F-数据类型",
        "feature": "set-type",
        "title": "集合类型",
        "page_file": "f04-set-type.md",
        "boundary": "负责集合的增删/判存/基数与随机取元素；不负责整数集合的宽度升级细节（d04）、"
                    "不负责哈希表实现（e01）、不负责有序集合（f05）。",
        "refs": [
            {"path": "src/t_set.c",
             "symbols": ["setTypeAdd", "setTypeRemove", "setTypeIsMember", "setTypeSize",
                          "setTypeMaybeConvert", "setTypeRandomElement"]},
        ],
    },
    {
        "feature_group": "F-数据类型",
        "feature": "zset-type",
        "title": "有序集合类型",
        "page_file": "f05-zset-type.md",
        "boundary": "负责成员分值与名次/取值的读写语义，以及编码切换判据；"
                    "不负责跳表实现（f06）、不负责通用哈希表（e01）。",
        "refs": [
            {"path": "src/t_zset.c", "symbols": ["zsetAdd", "zsetScore", "zsetDel", "zsetRank", "zsetTypeMaybeConvert"]},
        ],
    },
    {
        "feature_group": "F-数据类型",
        "feature": "skiplist",
        "title": "跳表",
        "page_file": "f06-skiplist.md",
        "boundary": "负责跳表的插入与层级维护（作为有序集合在元素较多时的索引结构）；"
                    "不负责有序集合命令语义（f05）、不负责压缩前缀树（e04）。",
        "refs": [
            {"path": "src/t_zset.c", "symbols": ["zslInsert"]},
        ],
    },
    {
        "feature_group": "F-数据类型",
        "feature": "stream-type",
        "title": "流类型",
        "page_file": "f07-stream-type.md",
        "boundary": "负责流的条目追加、迭代读取、按 ID 裁剪与删除，以及 ID 比较语义；"
                    "不负责压缩前缀树实现（e04）、不负责消费者组状态机（f08）。",
        "refs": [
            {"path": "src/t_stream.c",
             "symbols": ["streamAppendItem", "streamIteratorStart", "streamIteratorGetID",
                          "streamTrim", "streamDeleteItem", "streamCompareID"]},
        ],
    },
    {
        "feature_group": "F-数据类型",
        "feature": "stream-consumer-groups",
        "title": "流的消费者组",
        "page_file": "f08-stream-consumer-groups.md",
        "boundary": "负责消费者组的组内游标、待确认条目的转移与确认语义；"
                    "不负责条目本身如何写入与裁剪（f07）。",
        "refs": [
            {"path": "src/t_stream.c", "symbols": ["streamCreateCG"]},
        ],
    },

    # ============ G 持久化 ============
    {
        "feature_group": "G-持久化",
        "feature": "rdb-snapshot",
        "title": "RDB 快照格式与落盘",
        "page_file": "g01-rdb-snapshot.md",
        "boundary": "负责把整个数据集序列化成快照并读回，含各编码的写出/读入分支；"
                    "不负责写命令的追加日志（g02）、不负责复制全量同步的传输协议（h01）。",
        "refs": [
            {"path": "src/rdb.c", "symbols": ["rdbSave", "rdbLoad"]},
        ],
    },
    {
        "feature_group": "G-持久化",
        "feature": "aof-append",
        "title": "AOF 追加日志",
        "page_file": "g02-aof-append.md",
        "boundary": "负责把写命令追加进 AOF 缓冲并落盘的分支；"
                    "不负责命令传播的登记点（a05）、不负责快照（g01）。",
        "refs": [
            {"path": "src/aof.c", "symbols": ["aofWrite", "feedAppendOnlyFile"]},
        ],
    },

    # ============ H 复制与高可用 ============
    {
        "feature_group": "H-复制与高可用",
        "feature": "replication-feed",
        "title": "主从复制传播",
        "page_file": "h01-replication-feed.md",
        "boundary": "负责把写命令传播给从库、维护从库连接与偏移推进、以及从库侧发起复制的入口；"
                    "不负责 AOF 写入（g02）、不负责不落盘的全量/增量同步格式细节（g01 的"
                    "快照写出供其复用）、不负责集群分片（h02）。",
        "refs": [
            {"path": "src/replication.c", "symbols": ["replicationFeedSlaves", "replicaofCommand"]},
        ],
    },
    {
        "feature_group": "H-复制与高可用",
        "feature": "cluster-bus",
        "title": "集群总线与槽位",
        "page_file": "h02-cluster-bus.md",
        "boundary": "负责集群模式下的节点握手、集群总线报文处理与槽位归属维护；"
                    "不负责单机主从复制（h01）、不负责哨兵（h03）。",
        "refs": [
            {"path": "src/cluster_legacy.c", "symbols": ["clusterInit", "clusterProcessPacket"]},
        ],
    },
    {
        "feature_group": "H-复制与高可用",
        "feature": "sentinel",
        "title": "哨兵监控与故障转移",
        "page_file": "h03-sentinel.md",
        "boundary": "负责哨兵对主从实例的周期探测、主观下线判定与故障转移流程；"
                    "不负责数据面复制（h01）、不负责集群槽位（h02）。",
        "refs": [
            {"path": "src/sentinel.c",
             "symbols": ["sentinelHandleDictOfRedisInstances", "sentinelSendPeriodicCommands",
                          "sentinelCheckSubjectivelyDown"]},
        ],
    },

    # ============ I 阻塞与通知 ============
    {
        "feature_group": "I-阻塞与通知",
        "feature": "blocked-clients",
        "title": "阻塞客户端与唤醒",
        "page_file": "i01-blocked-clients.md",
        "boundary": "负责把客户端置为阻塞等待（如列表/流的阻塞读）、登记等待的键，"
                    "以及条件满足时解除阻塞并重放命令；不负责键的写入语义（c02/F 组）、"
                    "不负责键空间通知（i02）。",
        "refs": [
            {"path": "src/blocked.c", "symbols": ["blockForKeys", "handleClientsBlockedOnKeys", "unblockClient"]},
        ],
    },
    {
        "feature_group": "I-阻塞与通知",
        "feature": "keyspace-notification",
        "title": "键空间通知",
        "page_file": "i02-keyspace-notification.md",
        "boundary": "负责把键的变更事件按配置转换成通知消息投递给订阅者；"
                    "不负责订阅通道的维护（j02 的 pubsub）、不负责键的增删本身（c02）。",
        "refs": [
            {"path": "src/notify.c", "symbols": ["notifyKeyspaceEvent"]},
        ],
    },

    # ============ J 事务脚本与模块 ============
    {
        "feature_group": "J-事务脚本与模块",
        "feature": "multi-exec",
        "title": "事务与命令排队",
        "page_file": "j01-multi-exec.md",
        "boundary": "负责把命令排入事务队列、以及 EXEC 时按队列顺序依次执行；"
                    "不负责命令本身的原子性保证（由单线程事件循环提供，a01）、"
                    "不负责脚本执行（j03）。",
        "refs": [
            {"path": "src/multi.c", "symbols": ["multiCommand", "execCommand"]},
        ],
    },
    {
        "feature_group": "J-事务脚本与模块",
        "feature": "pubsub",
        "title": "发布订阅通道",
        "page_file": "j02-pubsub.md",
        "boundary": "负责订阅通道的维护与消息投递（频道/模式两类）；"
                    "不负责键空间通知的事件生成（i02），不负责集群下的跨节点扩散（h02）。",
        "refs": [
            {"path": "src/pubsub.c", "symbols": ["pubsubPublishMessage"]},
        ],
    },
    {
        "feature_group": "J-事务脚本与模块",
        "feature": "scripting-eval",
        "title": "脚本执行",
        "page_file": "j03-scripting-eval.md",
        "boundary": "负责脚本命令的执行编排（解释器生命周期、脚本缓存、错误包装）；"
                    "不负责函数库（j04）、不负责事务排队（j01）。",
        "refs": [
            {"path": "src/eval.c", "symbols": ["evalGenericCommand", "scriptCommand"]},
        ],
    },
    {
        "feature_group": "J-事务脚本与模块",
        "feature": "functions-library",
        "title": "函数库",
        "page_file": "j04-functions-library.md",
        "boundary": "负责以库为单位加载/注册可调用函数并暴露调用入口；"
                    "不负责临时脚本执行（j03）。",
        "refs": [
            {"path": "src/functions.c",
             "symbols": ["functionsCreateWithLibraryCtx", "fcallCommand", "functionLoadCommand"]},
        ],
    },
    {
        "feature_group": "J-事务脚本与模块",
        "feature": "module-api",
        "title": "模块系统",
        "page_file": "j05-module-api.md",
        "boundary": "负责模块系统的初始化与模块注册骨架；不负责脚本与函数库（j03/j04）、"
                    "不负责各模块自己定义的数据类型语义。",
        "refs": [
            {"path": "src/module.c", "symbols": ["moduleInitModulesSystem"]},
        ],
    },
    {
        "feature_group": "J-事务脚本与模块",
        "feature": "config-runtime",
        "title": "运行时配置与配置重写",
        "page_file": "j06-config-runtime.md",
        "boundary": "负责运行时改配置的校验与生效、以及把内存中的配置重写回配置文件；"
                    "不负责各参数被哪些子系统消费（散落在 C/F/G 组）。",
        "refs": [
            {"path": "src/config.c", "symbols": ["configSetCommand", "rewriteConfig"]},
        ],
    },

    # ============ K 安全 ============
    {
        "feature_group": "K-安全",
        "feature": "acl",
        "title": "访问控制列表",
        "page_file": "k01-acl.md",
        "boundary": "负责用户的权限规则维护（命令/键/通道的分类授权）与一次命令的权限判定；"
                    "不负责认证密码学（k02）、不负责 TLS 通道加密（k03）。",
        "refs": [
            {"path": "src/acl.c",
             "symbols": ["ACLInit", "ACLSetUser", "ACLCheckAllUserCommandPerm", "ACLAuthenticateUser"]},
        ],
    },
    {
        "feature_group": "K-安全",
        "feature": "tls-transport",
        "title": "TLS 传输",
        "page_file": "k02-tls-transport.md",
        "boundary": "负责 TLS 上下文的初始化与连接加密的装配；不负责用户权限（k01）。",
        "refs": [
            {"path": "src/tls.c", "symbols": ["tlsInit"]},
        ],
    },

    # ============ L 特殊数据类型 ============
    {
        "feature_group": "L-特殊数据类型",
        "feature": "bitops",
        "title": "位与位域操作",
        "page_file": "l01-bitops.md",
        "boundary": "负责按位/按位域的读写与位计数、位定位命令语义；"
                    "不负责字符串类型的普通读写（f01）。",
        "refs": [
            {"path": "src/bitops.c",
             "symbols": ["setbitCommand", "getbitCommand", "bitcountCommand", "bitposCommand", "bitfieldGeneric"]},
        ],
    },
    {
        "feature_group": "L-特殊数据类型",
        "feature": "hyperloglog",
        "title": "基数估算",
        "page_file": "l02-hyperloglog.md",
        "boundary": "负责基数估算数据结构的更新与估算读取；不负责集合类型的精确计数（f04）。",
        "refs": [
            {"path": "src/hyperloglog.c", "symbols": ["hllAdd", "hllCount", "pfaddCommand"]},
        ],
    },
    {
        "feature_group": "L-特殊数据类型",
        "feature": "geo-index",
        "title": "地理索引与半径查询",
        "page_file": "l03-geo-index.md",
        "boundary": "负责地理坐标的编码存储与按半径/范围的查询流程；"
                    "不负责有序集合本身的分值与名次语义（f05）。",
        "refs": [
            {"path": "src/geo.c", "symbols": ["geoaddCommand", "georadiusGeneric"]},
        ],
    },
    {
        "feature_group": "L-特殊数据类型",
        "feature": "sort-command",
        "title": "SORT 命令",
        "page_file": "l04-sort-command.md",
        "boundary": "负责 SORT/SORT_RO 的取值、排序与输出流程（含外部键与 limiting）；"
                    "不负责列表/集合/有序集合的存储语义（F 组）。",
        "refs": [
            {"path": "src/sort.c", "symbols": ["sortCommand", "sortCommandGeneric"]},
        ],
    },
    {
        "feature_group": "L-特殊数据类型",
        "feature": "active-defrag",
        "title": "主动碎片整理",
        "page_file": "l05-active-defrag.md",
        "boundary": "负责按时间配额主动迁移对象以降低碎片；不负责分配器本身（e07）、"
                    "不负责内存淘汰（c05）。",
        "refs": [
            {"path": "src/defrag.c", "symbols": ["activeDefragCycle"]},
        ],
    },

    # ============ M 启动运维与后台任务 ============
    {
        "feature_group": "M-启动运维与后台任务",
        "feature": "server-bootstrap",
        "title": "服务端启动装配",
        "page_file": "m01-server-bootstrap.md",
        "boundary": "负责进程启动时把配置默认值、监听、数据结构与事件循环装配起来的顺序；"
                    "不负责运行期的定时任务（a03）、不负责关停流程（m03）。",
        "refs": [
            {"path": "src/server.c", "symbols": ["initServerConfig", "initServer"]},
        ],
    },
    {
        "feature_group": "M-启动运维与后台任务",
        "feature": "bio-background-jobs",
        "title": "后台 I/O 线程任务",
        "page_file": "m02-bio-background-jobs.md",
        "boundary": "负责把慢 I/O 任务交给后台线程并向主线程通知完成的机制；"
                    "不负责事件循环（a01）、不负责 AOF/RDB 的格式（G 组）。",
        "refs": [
            {"path": "src/bio.c", "symbols": ["bioSubmitJob"]},
        ],
    },
    {
        "feature_group": "M-启动运维与后台任务",
        "feature": "shutdown-sequence",
        "title": "关停流程",
        "page_file": "m03-shutdown-sequence.md",
        "boundary": "负责关停前的收尾顺序（落盘、通知、资源释放）；不负责启动装配（m01）、"
                    "不负责崩溃恢复后如何加载数据（g01 的读入路径）。",
        "refs": [
            {"path": "src/server.c", "symbols": ["prepareForShutdown"]},
        ],
    },
    {
        "feature_group": "M-启动运维与后台任务",
        "feature": "slowlog",
        "title": "慢查询日志",
        "page_file": "m04-slowlog.md",
        "boundary": "负责按耗时阈值把命令记录进慢日志并维护其环形列表；"
                    "不负责命令执行的耗时记账点（a05）、不负责延迟监控样本。",
        "refs": [
            {"path": "src/slowlog.c", "symbols": ["slowlogPushEntryIfNeeded"]},
        ],
    },
    {
        "feature_group": "M-启动运维与后台任务",
        "feature": "latency-monitor",
        "title": "延迟监控与命令重写",
        "page_file": "m05-latency-monitor.md",
        "boundary": "负责把一个客户端请求的命令行按语义重写成可重新执行的命令向量（供传播、"
                    "AOF 与复制使用）；不负责慢日志（m04）、不负责传播登记点（a05）。",
        "refs": [
            {"path": "src/networking.c", "symbols": ["rewriteClientCommandVector"]},
        ],
    },
]
