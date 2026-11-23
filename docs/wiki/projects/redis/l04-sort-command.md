---
project: redis
feature: sort-command
page_type: feature
schema_version: 1
refs:
  - path: src/sort.c
    symbols: [sortCommand, sortCommandGeneric]
---

# SORT 命令

## 一句话职责

`src/sort.c` 负责 SORT / SORT_RO 的完整流程：把 SQL 风格的选项解析成「排序方式 + 限制区间 + 输出操作」三组状态，把待排元素装进 `redisSortObject` 向量，按数值或 ALPHA 规则排序，再按 LIMIT 截断后直接回包或 STORE 成一个列表；不负责列表 / 集合 / 有序集合自身的存储与编码语义（F 组）。

## 机制是什么

SORT 不是一种新数据类型，而是一条跨类型的读流程：`sortCommandGeneric` 里 `lookupKeyRead` 拿到键后只接受 `OBJ_SET`、`OBJ_LIST`、`OBJ_ZSET` 三种类型，其余一律回 `shared.wrongtypeerr`。它对每种类型都先把成员**复制**进一个独立的向量，再在这个向量上排序，完全不改原键的编码与顺序。

承载元素的类型是 `server.h` 里的 `redisSortObject`：一个 `robj *obj` 加一个联合 `union { double score; robj *cmpobj; } u`。同一个槽在数值模式下用 `u.score` 预存分数，在 ALPHA 模式下改用 `u.cmpobj` 存解码后的比较对象——两条路径共用同一份向量，靠 `server.sort_alpha` 区分怎么读这个联合。第二个辅助结构是 `redisSortOperation`，只装 `type` 与 `pattern`，用于把每个 GET 选项挂成链表节点；`type` 的唯一取值是 `server.h` 定义的 `SORT_OP_GET`（值为 0），源码在两处非 GET 分支里用 `serverAssertWithInfo` 断言这一点。

排序参数没有放在向量里，而是写进全局 `server` 结构：`sort_desc`、`sort_alpha`、`sort_bypattern`、`sort_store` 四个 int 字段。`src/sort.c` 里 `sortCompare` 上方的注释给了理由：带额外参数的 `qsort_r` 不是标准实现而是 BSD 专有，因此只能用无参的 `qsort` 比较器，参数只能走全局。

两个入口函数只差一个只读标志：`sortCommand` 调 `sortCommandGeneric(c,0)`，`sortroCommand` 调 `sortCommandGeneric(c,1)`。`src/commands.def` 把 sort 注册成 arity `-2`、带 `CMD_WRITE|CMD_DENYOOM`，把 sort_ro 注册成 `CMD_READONLY`、自 `7.0.0` 起存在。

## 怎么实现的

**1. 先做一次 ACL 能力探测。** 函数开头调 `ACLUserCheckCmdWithUnrestrictedKeyAccess(c->user, c->cmd, c->argv, c->argc, CMD_KEY_ACCESS)`，把结果存进局部 `user_has_full_key_access`。`src/acl.c` 里这个函数的注释写明语义：除了命令本身需要的权限，还要求用户对整个键空间具备给定 flag 的访问（例如 `~*` 或 allkeys），用户为 `NULL` 时直接返回 1。

**2. 选项解析。** 循环从 `j = 2` 扫到 `argc`，每轮先算 `leftargs = c->argc-j-1` 做参数个数守卫：`LIMIT` 需要 `leftargs >= 2` 并用两次 `getLongFromObjectOrReply` 读 `limit_start` / `limit_count`，`STORE` / `BY` / `GET` 各需要 `leftargs >= 1`。`ASC` 置 `desc = 0`、`DESC` 置 `desc = 1`、`ALPHA` 置 `alpha = 1`。`STORE` 分支还多一个前置条件 `readonly == 0`——SORT_RO 里 `STORE` 根本不被识别，直接落到最后的 `shared.syntaxerr`。任何无法识别的参数都 `addReplyErrorObject(c, shared.syntaxerr)` 并置 `syntax_error`；解析结束后只要 `syntax_error` 非零就 `listRelease(operations)` 并返回，不做任何查键。

**3. BY 的两条路。** 若 BY 的 pattern 里 `strchr(...,'*')` 为 NULL，说明是常量 BY，直接置 `dontsort = 1`（既不查权重键也不排序）。若有 `*`，依次过三道闸：集群模式下 `patternHashSlot(sortby->ptr, sdslen(sortby->ptr))` 必须等于 `getKeySlot(c->argv[1]->ptr)`，否则回「BY option of SORT denied in Cluster mode when keys formed by the pattern may be in different slots.」；非集群但 `server.cluster_compatibility_sample_ratio` 非 0 时，再用 `SHOULD_CLUSTER_COMPATIBILITY_SAMPLE()` 抽样比对，不等就 `server.stat_cluster_incompatible_ops++`（该配置在 `src/config.c` 注册为取值范围 0–100 的整数，默认 `0`，因此默认不做这项记账）；最后若 `!user_has_full_key_access` 则回「BY option of SORT denied due to insufficient ACL permissions.」。`src/cluster.c` 的 `patternHashSlot` 遇到 `*`、`?`、`[` 或转义符 `\` 立刻返回 -1，因此集群模式下任何带通配符的 BY 都会因为 -1 不等于真实 slot 而被拒。

**4. GET 同构但更严。** `'#'` 跳过 slot 检查（源码注释说明它代表键自身），其余 pattern 在集群模式下同样要求 `patternHashSlot` 等于排序键的 slot；`cluster_compatibility_sample_ratio` 抽样记账的逻辑与 BY 一致。ACL 检查在 GET 这里是**无条件**的（BY 那条只写在有 `*` 的分支里）。每个 GET 都 `listAddNodeTail(operations, createSortOperation(SORT_OP_GET, c->argv[j+1]))` 并 `getop++`。

**5. 查键与缺失键降级。** `lookupKeyRead` 之后，类型不在三种之列就回 wrongtype。再补一条特例：类型是 `OBJ_ZSET` 且 `zsetLength(sortval) == 0` 时把 `sortval` 置 NULL，源码注释给的场景是模块在 ZPOP 掉最后一个成员后触发的键空间通知里、键本身尚未删除之前对该有序集合执行 SORT。缺失键的分支是 `sortval = createQuicklistObject(server.list_max_listpack_size, server.list_compress_depth)`——一个空 quicklist，于是后面的装载循环空转、`vectorlen` 为 0、输出空数组。这两个参数在 `src/config.c` 里是 `list-max-listpack-size`（默认 `-2`）和 `list-compress-depth`（默认 `0`）。

**6. 集合的无序兜底。** 若 `dontsort` 且类型是 `OBJ_SET` 且（有 `storekey` 或客户端带 `CLIENT_SCRIPT`），则强制 `dontsort = 0`、`alpha = 1`、`sortby = NULL`。源码注释的理由是集合没有原生顺序，不排序就无法在脚本与复制中保证一致结果；list 与 zset 保留原生顺序所以不需要这条。

**7. 数元素。** `switch(sortval->type)` 给出 `vectorlen`：list 用 `listTypeLength`，zset 用 `zsetLength`（`src/t_zset.c` 里它按 LISTPACK / SKIPLIST / BTREE 三种编码分别取长度），set 先看 `setTypeHasExpireSupport`——`server.h` 里这个 inline 判 `OBJ_ENCODING_HT` 的 dict type 是否为 `setDictTypeWithExpire`、或编码是否为 `OBJ_ENCODING_LISTPACK_EX`；可过期集合必须用 `SET_ITER_SKIP_EXPIRED` 迭代器**数一遍**，因为逻辑已过期的成员不参与排序。

**8. LIMIT 归一化。** `start = min(max(limit_start, 0), vectorlen)`，`limit_count = min(max(limit_count, -1), vectorlen)`，`end = (limit_count < 0) ? vectorlen-1 : start+limit_count-1`。源码注释点明上界取 `vectorlen` 是为了避免整数溢出。`start >= vectorlen` 时把 `start` 压到 `vectorlen-1`、`end` 压到 `vectorlen-2`，形成一个空区间。

**9. 装载向量（五个分支）。** 全部用 `zmalloc(sizeof(redisSortObject)*vectorlen)`。
- list 且 `dontsort`：这是唯一带 LIMIT 预剪枝的分支。`listTypeInitIterator` 的起点由 `desc ? (listTypeLength(sortval) - start - 1) : start` 决定，方向由 `desc ? LIST_HEAD : LIST_TAIL` 决定（`LIST_HEAD` 为 0、`LIST_TAIL` 为 1）；装完 `end -= start; start = 0`，让后续输出代码仍按「从 0 开始的区间」工作。
- list 全量：从位置 0 沿 `LIST_TAIL` 方向走完整条。
- set：`setTypeNext(&si, &str, &len, &llele)` 拿成员，交给 `createSortElement(str,len,llele,alpha && !sortby)`；该函数在成员是整数编码且「需要按字符串比较」时用 `ll2string` 一次性格式化（`LONG_STR_SIZE` 在 `src/util.h` 定义为 21），避免每次比较都重新格式化。`server.memory_tracking_enabled` 为真时还会在装载前后各取一次 `kvobjAllocSize` 并 `updateSlotAllocSize`。
- zset 且 `dontsort`：listpack 编码用 `lpSeek(lp, 2*start)`（降序用 `-2-(2*start)`）直接定位成员节点，因为每个成员在 listpack 里占两个相邻项；BTREE 编码用 `zbtreeIteratorSeekRank` 定点 rank 后 `zbtreeIteratorNext(...,desc,...)`；SKIPLIST 编码降序从 `zsl->tail` 起步、升序从 `zsl->header->level[0].forward` 起步，起点非 0 时用 `zslGetElementByRank(zsl, zsetLength(sortval)-start)`（降序，名次从 1 起算）或 `zslGetElementByRank(zsl, start+1)`（升序）。装完同样 `end -= start; start = 0`。
- zset 全量：BTREE 用 `zbtreeIteratorStart` + `zbtreeIteratorNext` 顺序取；SKIPLIST 走 `((zset*)sortval->ptr)->dict` 的 `dictInitIterator` / `dictNext` 遍历，再从节点 `zslGetNodeElement` 取成员。

装载结束有 `serverAssertWithInfo(c,sortval,j == vectorlen)`。

**10. 填分数或比较对象。** `!dontsort` 时逐元素处理：有 `sortby` 就 `lookupKeyByPattern(c->db,sortby,vector[j].obj)` 取权重键，取不到就 `continue`（该槽分数留在装载时写入的 0）；无 `sortby` 就用元素自己。`alpha` 且有 `sortby` 时 `vector[j].u.cmpobj = getDecodedObject(byval)`；否则按编码分派：sds 编码走 `string2d`，返回 0 就把 `int_conversion_error` 置 1（`src/util.c` 里 `string2d` 的实现要求整个字符串被消费完、拒绝 NaN、拒绝溢出到 `HUGE_VAL`），`OBJ_ENCODING_INT` 直接 `(long)byval->ptr`。`sortby` 取来的对象在这一轮末尾 `decrRefCount`。

**11. 排序。** 先把四个开关写进 `server`，再做选择：`sortby && (start != 0 || end != vectorlen-1)` 时用 `pqsort(vector,vectorlen,sizeof(redisSortObject),sortCompare,start,end)` 只排区间，否则 `qsort` 全量。`src/pqsort.c` 的 `pqsort` 只是把向量里的 `[lrange, rrange]` 换算成字节区间后调用内部 `_pqsort`，它会在递归里裁掉与目标区间不相交的分段。

`sortCompare` 的规则分四支：数值模式下比 `u.score`，相等时用 `compareStringObjects` 按字节序兜底（源码注释说这样 SORT 的结果是确定的）；`alpha` 且有 `sort_bypattern` 时比 `u.cmpobj`，两个都非空才直接 `compareStringObjects` 或 `strcoll`，有一边为空时以空为更小、两边都空为相等；`alpha` 无 `bypattern` 时比 `obj`，`sort_store` 为真用 `compareStringObjects`（`src/object.c` 里它是 `REDIS_COMPARE_BINARY` 的包装，走 `memcmp` 再比长度），否则用 `collateStringObjects`（`REDIS_COMPARE_COLL` 包装，走 `strcoll`）。最后一行 `return server.sort_desc ? -cmp : cmp;`。

**12. 输出或 STORE。** `outputlen = getop ? getop*(end-start+1) : end-start+1`。若 `int_conversion_error` 非零，直接回「One or more scores can't be converted into double」——注意这一步发生在排序**之后**，分数不合法时排序其实已经跑完，只是结果被错误回复取代。无 `storekey` 时 `addReplyArrayLen(outputlen)` 后逐元素回复：没有 GET 就 `addReplyBulk(vector[j].obj)`，有 GET 就把 `operations` 链表整体 `listRewind` 走一遍，每个操作再 `lookupKeyByPattern`，取不到回 `addReplyNull`，取到就 bulk 回复并 `decrRefCount`。有 `storekey` 时先建 `createQuicklistObject(server.list_max_listpack_size, server.list_compress_depth)`，逐元素 `listTypePush(sobj, ..., LIST_TAIL)`，GET 取不到时压一个空串（`createStringObject("",0)`）而不是 NULL；`outputlen` 非 0 就 `listTypeTryConversion(sobj, LIST_CONV_AUTO, NULL, NULL)` 再 `setKey(c, c->db, storekey, &sobj, 0)`（所有权转移给 DB，`sobj` 置 NULL）、发 `NOTIFY_LIST` 的 `"sortstore"` 键空间事件、`server.dirty += outputlen`，最后回 `addReplyLongLong(outputlen)`；`outputlen` 为 0 时改为 `dbDelete(c->db, storekey)`，删除成功才 `keyModified`、发 `"del"` 事件、`server.dirty++`，同样回 0。

**13. 清理。** 逐元素 `decrRefCount(vector[j].obj)`、`decrRefCount(sortval)`、`listRelease(operations)`，`alpha` 为真时再逐元素 `decrRefCount(vector[j].u.cmpobj)`（只对非空指针），最后 `zfree(vector)`。注意 cmpobj 的释放放在 `obj` 之后独立的一轮循环里。

**14. 取键规则集中在 `lookupKeyByPattern`。** 它承载 BY 与 GET 的全部语义，三条规则按源码顺序生效：pattern 恰为 `#`（`spat[0]=='#' && spat[1]=='\0'`）时 `incrRefCount(subst)` 并原样返回，这就是 `SORT key GET #` 取元素自身的实现；pattern 里找不到 `*` 时返回 NULL（源码注释：固定键的 GET 没有意义）；找到 `*` 后再看 `*` 右侧有没有 `"->"` 且箭头后非空，有就把箭头左边拼成 key、右边当 hash field——拼出的键必须 `kv->type == OBJ_HASH`，用 `hashTypeGetValueObject(db, kv, fieldobj->ptr, HFE_LAZY_EXPIRE, &val, NULL, &isHashDeleted)` 取值（`HFE_LAZY_EXPIRE` 在 `server.h` 定义为 0，含义是「删除过期字段，若为最后一个字段则连键一起删」），返回值 `isHashDeleted` 为真就 `goto noobj`；没有箭头时拼出的键必须 `OBJ_STRING` 并 `incrRefCount`。所有返回对象都被注释要求「引用计数已加一」，调用方负责减一。拼接用的是栈上 `char subbuf[LONG_STR_SIZE]` 与 `ll2string`，整数编码的替换对象不必分配 sds。

## 关键取舍

**源码事实：** 排序参数走全局 `server` 字段而不是比较器上下文，`src/sort.c` 的注释直接归因于「`qsort_r` 不是标准，只能靠全局结构传参」，同一条命令因此不可重入地共享 `sort_desc` / `sort_alpha` / `sort_bypattern` / `sort_store` 四个开关。

**源码事实：** `sort_store` 不是「只决定输出位置」，它还换掉了比较函数：有 STORE 时 ALPHA 比较走 `compareStringObjects` 的字节序，无 STORE 时走 `collateStringObjects` 的 `strcoll`。因此同一批数据在 `SORT key ALPHA` 与 `SORT key ALPHA STORE dst` 下给出的次序取决于两个函数各自的结果：`strcoll` 跟随当前 locale 的 collation，`memcmp` 按字节序，两者只在 C locale 下等价。

**源码事实：** `dontsort` 是纯优化路径：BY 常量时不查权重键、不填分数、不排序，list 与 zset 直接按原生顺序装载并按 LIMIT 只取所需区间（list 用 `listTypeInitIterator` 的起点偏移，listpack zset 用 `lpSeek` 定点，skiplist zset 用 `zslGetElementByRank`）。代价是这条路径上 `BY` 的 argument 虽然被解析保存，却完全不参与结果。

**源码事实：** 数值模式下转换失败的判定用 `int_conversion_error` 累积，而错误回复在排序之后才发出，所以「报错」不代表「没做工作」——`qsort` 已经对全向量跑完，失败元素以 0 分参与过比较。

**源码事实：** 集群与 ACL 的限制都落在解析阶段：集群模式下 BY / GET 的 pattern 必须能推出与排序键相同的 slot（`patternHashSlot` 对通配符返回 -1），非法则连键都不查就回错；ACL 则要求 `CMD_KEY_ACCESS` 的全键空间授权，否则 BY / GET 被拒。`src/sort.c` 内只有函数开头那一次 `ACLUserCheckCmdWithUnrestrictedKeyAccess` 调用，排序键本身不在这里校验。

**源码事实：** `vectorlen` 的上界同时被用于 LIMIT 归一（`min(..., vectorlen)`）与向量分配，源码注释把前者归因为「避免输入导致的整数溢出」。

**源码事实：** 集合的成员复制在装载时就决定要不要转成字符串（`createSortElement` 的 `alpha && !sortby` 参数），因为 ALPHA 且无 BY 时比较的是元素字面量，否则每次 `strcoll` 前都要重新格式化整数。

**源码事实：** 空结果的 STORE 不是「写一个空列表」，而是 `dbDelete` 目标键；`outputlen` 为 0 时连 `setKey` 都不会被调用，发的是 `"del"` 事件而不是 `"sortstore"`。

**架构推论：** SORT 把「读多键」这件事塞进了一个写命令的外壳里——`sort` 在命令表带 `CMD_WRITE|CMD_DENYOOM`，而它的实际副作用只有可选的 STORE 一个键，因此 `sort_ro` 的只读性完全由 `sortCommandGeneric` 的 `readonly` 参数屏蔽 STORE 实现，没有独立的执行路径。这也意味着键空间访问的声明只能靠 `src/db.c` 的 `sortGetKeys` 与 `sortROGetKeys` 手工补：`src/commands.def` 的 SORT_Keyspecs 把 BY / GET 与 STORE 都标成 `KSPEC_BS_UNKNOWN`，注释说明键名来自被排序键的内容、且关键字可出现在参数数组任意位置。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/sort.c` | `sortCommand` | SORT 入口，以 `readonly=0` 调 `sortCommandGeneric` |
| `src/sort.c` | `sortCommandGeneric` | 命令主体：选项解析、装载向量、算分、排序、输出或 STORE |
| `src/sort.c` | `sortroCommand` | SORT_RO 入口，以 `readonly=1` 调 `sortCommandGeneric`，STORE 因此不被识别 |
| `src/sort.c` | `lookupKeyByPattern` | BY / GET 的取值规则：`#` 返回自身，无 `*` 返回 NULL，`->` 走 hash 字段 |
| `src/sort.c` | `createSortOperation` | 把一个 GET 选项包装成链表节点，type 只能是 `SORT_OP_GET` |
| `src/sort.c` | `sortCompare` | 比较器：数值比 score、ALPHA 比 cmpobj 或 obj，`sort_desc` 取反 |
| `src/sort.c` | `createSortElement` | 从原地读到的成员建 robj，可整数编码且需字符串比较时先格式化 |
| `src/pqsort.c` | `pqsort` | 只排 `[lrange, rrange]` 的部分排序，把区间换算成字节区间 |
| `src/server.h` | `redisSortObject` | 向量元素：`obj` 加 `score` / `cmpobj` 的联合 |
| `src/cluster.c` | `patternHashSlot` | 由 BY / GET 的 pattern 推断单一 slot，遇通配符或转义返回 -1 |
| `src/db.c` | `sortGetKeys` | SORT 的键提取：排序键加最后一个 STORE 键，跳过 limit / get / by 参数 |
| `src/db.c` | `sortROGetKeys` | SORT_RO 的键提取：只报排序键 |

## 与主核 / AI 的关系

在本仓之外，SORT 没有跨模块的导出面：`lookupKeyByPattern`、`createSortOperation`、`sortCompare`、`createSortElement` 都只在 `src/sort.c` 内定义且未出现在任何头文件里；`server.h` 只声明了命令入口 `sortCommand` 与 `sortroCommand`，二者由 `src/commands.def` 的命令表绑定。它依赖的外部能力全部是别的机制页的责任：`zsetLength`（f05）、`setTypeHasExpireSupport` 与 `SET_ITER_SKIP_EXPIRED`（f04）、`listTypeInitIterator` / `listTypePush`（f02）、`hashTypeGetValueObject`（f03）、`compareStringObjects` / `collateStringObjects` 与 `getDecodedObject`（d01）。

**架构推论：** SORT 的形态可以直接照搬到「按外部权重排序一份 id 集合」这类需求上——它的三层结构（pattern 解析成取值规则、平行向量装分数、可切片的比较器）与主核做 ANN 候选重排时的形状一致：候选 id 列表相当于待排元素，外部权重相当于 BY pattern 指向的键，LIMIT 相当于 top-k 窗口，`pqsort` 相当于只排窗口的部分排序。区别在于 SORT 的取值规则靠 glob 拼键名，主核的权重来自列或者向量距离表，因此可以直接省掉 `lookupKeyByPattern` 这一层。

**架构推论：** SORT 的两处设计对 AI 侧检索链路有直接参考价值。其一，`dontsort` 证明「已知数据源本身有序时，整个排序阶段可以消掉，LIMIT 可以下沉成区间定位」——对应到向量索引就是「有序候选队列 + 只取窗口」；其二，`int_conversion_error` 在排序之后才报错，说明「先算分再校验」把错误报告推到了流程末端，代价是全向量白排一遍，这提示在重排链路里把分数合法性检查提前到装向量之前。

## 参考

- [Redis 源码：sort.c](https://github.com/redis/redis/blob/unstable/src/sort.c)
- [Redis 源码：pqsort.c](https://github.com/redis/redis/blob/unstable/src/pqsort.c)
- [Redis 命令：SORT](https://redis.io/docs/latest/commands/sort/)
- [Redis 命令：SORT_RO](https://redis.io/docs/latest/commands/sort_ro/)
