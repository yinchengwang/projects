---
project: redis
feature: command-table
page_type: feature
schema_version: 1
refs:
  - path: src/server.c
    symbols: [populateCommandTable, lookupCommand, commandCheckArity]
---

# 命令表与命令元数据

## 一句话职责

Redis 把所有命令的定义编译成一张静态表，启动时一次性装配进两张字典，之后按命令名 O(1) 查出实现函数与元数据，并在执行前用 arity 规则挡住参数个数不对的调用。

## 机制是什么

命令定义本体是 `src/commands.def` 里的静态数组 `redisCommandTable[]`，源码注释写明它「auto generated from the json files in the commands folder」——即由 `src/commands/` 下的命令 JSON 生成，每条记录用 `MAKE_CMD` 宏填好命令名、简介、复杂度、首次出现的版本、实现函数指针、arity、命令标志位、ACL 类别、键规格等字段。

`populateCommandTable` 在启动时遍历这张静态表，把每条命令装进运行期的字典。`lookupCommand` 是按参数向量查命令的入口，负责区分「顶层命令」与「容器命令的子命令」两级。`commandCheckArity` 是执行前的元数据校验：命令声明的参数个数与实际收到的对不上就拒掉。

本页只讲命令表的构建、查找与元数据校验；命令被调用之后的执行编排与统计记账是另一页的事，各命令的具体语义也不在本页。

## 怎么实现的

**装配。** `populateCommandTable` 从 `redisCommandTable` 的第一项开始向后遍历，以 `declared_name == NULL` 作为数组结束标志。对每条命令做四件事：把 `declared_name` 复制成一份 sds 存进 `c->fullname`；调 `populateCommandStructure` 做递归装配，失败就跳过这条命令；把命令分别 `dictAdd` 进 `server.commands` 和 `server.orig_commands`；最后 `serverAssert(retval1 == DICT_OK && retval2 == DICT_OK)`——两个字典都要求添加成功，命令名重复会直接 panic 而不是静默覆盖。

**递归装配。** `populateCommandStructure` 对每条命令逐项填充运行时字段：

- **模式过滤。** 带 `CMD_SENTINEL` 标记的命令在哨兵模式下不注册，带 `CMD_ONLY_SENTINEL` 的只在哨兵模式注册；不满足条件时返回 `C_ERR`，该命令不会进字典。
- **ACL 类别推导。** `setImplicitACLCategories` 按标志位补类别：`CMD_WRITE` 补 `ACL_CATEGORY_WRITE`；`CMD_READONLY` 补 `ACL_CATEGORY_READ`（但显式排除 `ACL_CATEGORY_SCRIPTING` 的脚本命令）；`CMD_ADMIN` 补 `ADMIN` 与 `DANGEROUS`；`CMD_PUBSUB` 补 `PUBSUB`；`CMD_FAST` 补 `FAST`；`CMD_BLOCKING` 补 `BLOCKING`。最后一条规则写在注释里：「If it's not @fast is @slow in this binary world」——凡是没被标成 `@fast` 的，一律补上 `ACL_CATEGORY_SLOW`。
- **延迟直方图懒分配。** `c->latency_histogram` 置 `NULL`，注释说明只有命令第一次被执行时才分配内存。
- **旧版键规格。** `populateCommandLegacyRangeSpec` 维护旧的 `(first, last, step)` 键规格，源码注释说明它只为继续支持 `COMMAND INFO` 与 `COMMAND GETKEYS` 的回复格式而保留。
- **ACL 命令 ID。** `c->id = ACLGetCommandID(c->fullname)`，分配一个运行期递增的命令 ID，供 ACL 位图判权使用。
- **子命令。** 若命令带 `subcommands` 数组，逐个递归装配子命令：`sub->fullname` 由 `catSubCommandFullname` 拼成 `"父名|子名"` 的形式，然后 `commandAddSubcommand` 把它塞进父命令的 `subcommands_dict`（键是子命令短名），并给子命令分配自己的 ACL ID。

**两张字典的分工。** `server.commands` 与 `server.orig_commands` 都在 `initServerConfig` 里用 `dictCreate(&commandTableDictType)` 建好后立刻 `populateCommandTable()` 填满。源码注释写明：`server.commands` 会被 `redis.conf` 的 `rename-command` 指令改名，`server.orig_commands` 则「will be unaffected by rename-command statements in redis.conf」。查不到命令时的回退路径是 `lookupCommandOrOriginal`——先查 `server.commands`，查不到再查 `server.orig_commands`；它的注释点名了真实用途：「This is used by functions rewriting the argument vector such as `rewriteClientCommandVector()` in order to set client->cmd pointer correctly even if the command was renamed」，真实调用点在 `src/networking.c`。

**按名查找。** `lookupCommand` 本身只是 `lookupCommandLogic(server.commands, argv, argc, 0)` 的薄封装。`lookupCommandLogic` 的判定顺序是：

1. `base_cmd = dictFetchValue(commands, argv[0]->ptr)` 取顶层命令。
2. `has_subcommands = base_cmd && base_cmd->subcommands_dict`。
3. 若 `argc == 1` 或该命令没有子命令，直接返回 `base_cmd`。源码注释提醒：此时 `base_cmd->proc` 可能是 `NULL`——像 `CONFIG`、`CLIENT`、`CLUSTER` 这类容器命令在 `src/commands.def` 里就是 `proc` 为 `NULL`、`arity` 为 `-2`、只挂一串子命令。
4. 否则（即 `argc > 1` 且有子命令）返回 `lookupSubcommand(base_cmd, argv[1]->ptr)`。源码注释写明限制：「Currently we support just one level of subcommands」。

`lookupCommandLogic` 还带一个 `strict` 参数，源码注释解释了它的语义分界：「`strict` should be used every time we want to look up a command name (e.g. in COMMAND INFO) rather than to find the command a user requested to execute」。`strict` 为真时参数个数必须精确（顶层命令要求 `argc == 1`，子命令要求 `argc == 2`），否则返回 `NULL`。`lookupCommand` 传 `strict = 0`；按字符串查的 `lookupCommandBySdsLogic` 传 `strict = 1`，它先按 `|` 拆分名字（`sdssplitlen`），拆出来的段数小于 1 或大于 2 都直接返回 `NULL`。

**arity 校验。** `commandCheckArity` 的判定条件只有一行：`(cmd->arity > 0 && cmd->arity != argc) || (argc < -cmd->arity)`。语义因此是两种——`arity` 为正表示**精确**个数，为负表示**至少** `|arity|` 个。参数个数包含命令名本身：`src/server.h` 的 `struct redisCommand` 注释写明「it is possible to use -N to say >= N」，而 `src/commands.def` 里的实际取值可见 `GET` 是 2（命令名加一个键）、`INCR` 是 2、`SET` 是 -3（`SET key value` 最少三个）、`DEL` 是 -2、`PING` 是 -1。校验失败时错误信息是 `"wrong number of arguments for '%s' command"`，`%s` 取的是 `cmd->fullname`，所以子命令的报错里会出现 `父名|子名` 的形式。

**查不到命令时的退化路径。** 名字不存在时报错由 `commandCheckExistence` 负责，它把两种情况分开：若 `argv[0]` 本身是个容器命令（`isContainerCommandBySds` 为真），说明是子命令写错了，提示 `"missing subcommand. Try %s HELP."` 或 `"unknown subcommand '%.128s'. Try %s HELP."`；否则提示 `"unknown command '%.128s'"`，并在 `argc >= 2` 时把后续参数拼在后面（拼到 128 字节为止）。函数末尾用 `sdsmapchars(*err, "\r\n", "  ", 2)` 把换行换成空格，源码注释说明原因：参数来自用户，可能含任意字符，不处理会输出非法协议。

**查表本身的省流优化。** `isCommandReusable` 是一个 `static inline` 判断：当上一条命令不为空、没有子命令、且名字与本次 `argv` 匹配时，可以直接复用上次的查表结果，不必再查字典。`processCommand` 与 `preprocessCommand` 都用了它。

## 关键取舍

**源码事实：** 命令表是静态数组加两张字典，运行时只有查表，没有解析或分派开销；子命令只支持一级，两级以上的名字拆分会被 `lookupCommandBySdsLogic` 直接判为非法。**源码事实：** `server.commands` 与 `server.orig_commands` 并存，前者会被 `rename-command` 改名，后者保持原样，靠 `lookupCommandOrOriginal` 在查不到时回退。**源码事实：** ACL 类别、ACL 命令 ID、legacy 键规格都在装配期一次性算好；只有延迟直方图是懒分配的。**源码事实：** 命令名在两个字典里重复添加，用 `serverAssert` 断言成功——重复命令名是启动期错误而不是运行期错误。

**架构推论：** 把命令元数据做成「编译期生成的静态表 + 启动期一次装配」，换来运行期零解析开销与 O(1) 查表；代价是新增或修改命令必须重新生成 `src/commands.def`，且启动时要遍历整张表。**架构推论：** `strict` 与非 `strict` 两套查找语义分开，把「精确地查一个命令名」和「查用户实际请求执行的东西」区分开，避免 `COMMAND INFO` 之类的元数据查询因为参数个数不同而查错对象。**架构推论：** 保留 legacy `(first, last, step)` 键规格是向后兼容成本——新旧两套键描述并存，代价是每条命令多一份元数据，换来 `COMMAND INFO` 旧回复格式不必破坏。**架构推论：** 两张字典是运维改名能力与「改名后仍要能正确识别命令」之间的折中：只留一张，改名后重写参数向量的路径就会失去命令身份；留两张则每次回退查找多一次 `dictFetchValue`。**架构推论：** arity 用一个整数同时表达「精确」与「至少」两种约束，是用符号位省字段的典型做法，可读性换来的是排查时必须先看符号才能判断语义。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/server.c` | `populateCommandTable` | 遍历静态命令表，装配后写入 `server.commands` 与 `server.orig_commands` 两张字典 |
| `src/server.c` | `lookupCommand` | 按参数向量查命令，区分顶层命令与一级子命令 |
| `src/server.c` | `commandCheckArity` | 校验实际参数个数是否符合命令声明的 arity 规则 |

## 参考

- [Redis 源码：server.c](https://github.com/redis/redis/blob/unstable/src/server.c) —— 命令表装配、查找与 arity 校验的实现。
- [Redis 源码：commands.def](https://github.com/redis/redis/blob/unstable/src/commands.def) —— 自动生成的命令定义表，含每条命令的 arity、标志位与 ACL 类别。
- [Redis 命令：COMMAND](https://redis.io/docs/latest/commands/command/) —— 命令元数据的对外查询入口。
- [Redis 命令：COMMAND INFO](https://redis.io/docs/latest/commands/command-info/) —— 查询单个或多个命令的完整元数据。
- [Redis 开发文档：键规格](https://redis.io/docs/latest/develop/reference/key-specs/) —— 键规格（key specs）的语义与新旧两套描述的关系。

## 与主核 / AI 的关系

「静态表 + 启动装配 + O(1) 查表」是可迁移的元数据模式：主核若把算子、计划节点、权限规则也做成启动期装配的静态表，就能获得同样的运行期零开销。但主核是多写者引擎，命令表的读写需要并发保护，Redis 的单线程假设在这里不能照搬。arity 校验的思路可直接借用到主核的语句参数校验上——用「精确个数 / 至少 N 个」两种语义覆盖绝大多数算子，比逐个算子写判断更省事。AI 侧若把 Redis 当向量缓存，命令表决定了 AI 侧只能调用 Redis 已注册的命令，新增能力必须走 Redis 的命令注册路径或模块 API，不能靠旁路写入。
