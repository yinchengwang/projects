---
project: redis
feature: geo-index
page_type: feature
schema_version: 1
refs:
  - path: src/geo.c
    symbols: [geoaddCommand, georadiusGeneric]
---

# 地理索引与半径查询

## 一句话职责

`src/geo.c` 负责把经纬度编码成有序集合的 score，并按圆形或矩形范围查询：`geoaddCommand` 把坐标编成 52 位 geohash 整数，作为分值交给 `zaddCommand` 落库；`georadiusGeneric` 用覆盖盒加九宫格邻居捞出候选，再逐个算真实距离过滤；不负责有序集合本身的分值与名次语义（f05）。

## 机制是什么

地理索引不是新数据类型，就是一个普通有序集合，只是把 score 约定成经纬度的交织编码。`geohash.h` 的 `GEO_STEP_MAX` 为 26，注释写明「26*2 = 52 bits」：`geohashEncodeWGS84` 在 26 步上把纬度与经度各取 26 位、用 `interleave64` 交错成一个 52 位整数，`geohashAlign52Bits` 再左移 `52 - hash.step*2` 位对齐到 52 位整数区间；因为 `GEO_STEP_MAX` 就是 26，这次的移位量为 0，编码值直接就是 zset 的 score。允许的坐标范围由 `geohash.h` 的四个常量定死：`GEO_LAT_MIN` 为 -85.05112878、`GEO_LAT_MAX` 为 85.05112878、`GEO_LONG_MIN` 为 -180、`GEO_LONG_MAX` 为 180，注释说明取值来自 EPSG:900913 / EPSG:3785 / OSGEO:41001，所以两极不能编码。

读回坐标拿到的是格心而不是原始入参：`decodeGeohash` 用 `GEO_STEP_MAX` 步做 `geohashDecodeToLongLatWGS84`，`geohashDecodeAreaToLongLat` 取格子经纬度区间的中点并夹到 `GEO_LONG_MIN` / `GEO_LONG_MAX` / `GEO_LAT_MIN` / `GEO_LAT_MAX`。这也是 GEOADD 存入 52 位、GEOPOS 回读双精度但精度封顶在 52 位格子的原因。

查询之所以可行，是因为 zset 按 score 有序：一个 geohash 盒子覆盖的 score 是连续区间，`scoresOfGeoHashBox` 把盒子算成 `[align52Bits(hash), align52Bits(hash.bits+1))` 两条边界，一次区间扫描就能只捞出落在这个盒子里的成员，不需要全表扫。查询形状由 `geohash.h` 的 `GeoShape` 表达：`CIRCULAR_TYPE` 为 1 时用 `t.radius`，`RECTANGLE_TYPE` 为 2 时用 `t.r.width` 与 `t.r.height`，`xy[0]` 是经度、`xy[1]` 是纬度，`conversion` 是「米 → 用户单位」的换算系数。

## 怎么实现的

**写入路径：`geoaddCommand` 只做编码，落库全交给 ZADD。** 从 `longidx = 2` 开始逐个吃掉 `nx` / `xx` / `ch` 选项（`ch` 在源码里是空分支，注释写「Handle in zaddCommand」，因为 CH 语义由 ZADD 实现），遇到第一个非选项参数就停。校验只有一条：`(c->argc - longidx) % 3` 非零或 `xx && nx` 同时成立，就回 `shared.syntaxerr`。随后 `elements = (c->argc - longidx) / 3`，新向量长度 `argc = longidx + elements*2`，恰好对上 ZADD 的 `key [CH] [NX|XX] score ele ...` 形态；`argv[0]` 用 `createRawStringObject("zadd",4)` 现造，选项位原样搬过来并 `incrRefCount`。每个三元组走 `extractLongLatOrReply`：先用 `getDoubleFromObjectOrReply` 读两个数，再检查 `xy[0]` 落在 `GEO_LONG_MIN`..`GEO_LONG_MAX`、`xy[1]` 落在 `GEO_LAT_MIN`..`GEO_LAT_MAX`，越界就回 `invalid longitude,latitude pair`；任一条失败会把已建的 argv 逐个 `decrRefCount` 再 `zfree` 返回。编码成功后 score 由 `createStringObjectFromLongLongWithSds(bits)` 生成，成员对象直接复用原参数。最后 `replaceClientCommandVector` 换掉 `c->argv` 并调用 `zaddCommand`，所以 NX/XX/CH 语义、listpack 与 B+ 树的编码选择、`server.dirty` 计数、`zadd` 键空间通知全部由 `zaddGenericCommand` 负责：新建键时 `zsetTypeCreate` 按 `zset-max-listpack-entries`（`src/config.c` 默认 128）与 `zset-max-listpack-value`（默认 64）决定初始编码。

**查询路径：`georadiusGeneric` 一个函数吃六个命令。** 五个位标志定义在 `src/geo.c` 顶部：`RADIUS_COORDS` 是 `1<<0`、`RADIUS_MEMBER` 是 `1<<1`、`RADIUS_NOSTORE` 是 `1<<2`、`GEOSEARCH` 是 `1<<3`、`GEOSEARCHSTORE` 是 `1<<4`。先 `lookupKeyRead` 再 `checkType(c, zobj, OBJ_ZSET)`，然后按 flag 定 `base_args`：坐标形式为 6（key x y radius unit 之后才是选项），按成员形式为 5，GEOSEARCH 为 2，GEOSEARCHSTORE 为 3 并把 `srcKeyIndex` 传成 2（`geosearchstoreCommand` 就是这么调的），同时 `storekey = c->argv[1]`。有一处特判：`(flags & RADIUS_MEMBER) && !zobj` 时仍把 `base_args` 设成 5 继续解析参数，最后才按 `storekey` 是否存在决定回 `czero` 还是 `emptyarray`——注释写明了理由是「要先知道 STORE 标志才能决定回复形态」。source key 不存在时若带 storekey，先 `dbDelete` 目标键、`keyModified`、发 `del` 通知并 `server.dirty++`，然后回 `shared.czero`。

**选项解析的硬约束。** `withdist` / `withhash` / `withcoord` / `any` / `asc` / `desc` / `count` / `store` / `storedist` / `frommember` / `fromlonlat` / `byradius` / `bybox` 逐 token 顺手匹配，其余一律 `shared.syntaxerr`。COUNT 读到的值 `<= 0` 直接回 `COUNT must be > 0`；`any` 没有配 COUNT 回 `the ANY argument requires COUNT argument`；`storekey` 与 `withdist` / `withhash` / `withcoords` 任一同时存在就回「不兼容」错误；GEOSEARCH 必须恰好有 `FROMMEMBER` 或 `FROMLONLAT` 之一、恰好有 `BYRADIUS` 或 `BYBOX` 之一，否则报 `exactly one of ...`。还有一个形状判定：`count != 0 && sort == SORT_NONE && !any` 时把 `sort` 强制成 `SORT_ASC`，源码注释给的理由是「不带排序的 COUNT 没意义，必须排完才能返回最近的 N 个」。

**覆盖盒计算：`geohashCalculateAreasByShapeWGS84`。** 先 `geohashBoundingBox` 用 `EARTH_RADIUS_IN_METERS`（`src/geohash_helper.c` 里为 6372797.560856）把形状折成经纬度包围盒：圆形取 `t.radius`、矩形取 `width/2` 与 `height/2`，纬度增量 `lat_delta = rad_deg(height/EARTH_RADIUS_IN_METERS)`，经度增量在南北两侧分别用 `latitude+lat_delta` 与 `latitude-lat_delta` 的余弦算（因为高纬弧长更短），南半球则改用 bottom 增量填 `bounds[0]` / `bounds[2]`。之后把形状压成一个标量半径：圆形直接是 `t.radius`，矩形是 `sqrt((w/2)*(w/2)+(h/2)*(h/2))`（到角的距离），再乘 `conversion`。步长由 `geohashEstimateStepsByRadius` 估：`range_meters == 0` 直接返回 26；否则 `step` 从 1 起、`range_meters` 每次翻倍直到不小于 `MERCATOR_MAX`（该常量在 `src/geohash_helper.c` 里为 20037726.37）；随后 `step -= 2`，纬度绝对值大于 66 再减 1、大于 80 再减 1，最后夹到 1 到 26。拿到 `steps` 后 `geohashEncode` + `geohashNeighbors` + `geohashDecode`，再做两轮修正：第一轮看边缘盒是否真能盖住包围盒（`north.latitude.max < max_lat`、`south.latitude.min > min_lat`、`east.longitude.max < max_lon`、`west.longitude.min > min_lon` 任一成立）且 `steps > 1`，就把 `steps--` 重算一遍；第二轮在 `steps >= 2` 时用 `GZERO` 把已经落在包围盒外的邻居直接清零（例如 `area.longitude.min < min_lon` 时清 `west`、`south_west`、`north_west`），被清零的盒子在采集阶段会被 `HASHISZERO` 跳过。

**采集：`membersOfAllNeighbors` 固定查九个盒子。** 邻居数组是 `GeoHashBits neighbors[9]`，下标 0 是自身，1 到 8 是 `north`、`south`、`east`、`west`、`north_east`、`north_west`、`south_east`、`south_west`；`HASHISZERO` 的跳过，与「上一个已处理的邻居」`bits` 和 `step` 完全相同的也跳过（注释说明半径到 5000 公里级时相邻盒会重合，跳过是为了避免重复元素），每次进盒前先看 `ga->used && limit && ga->used >= limit`，满足就 `break`。单盒由 `membersOfGeoHashBox` 转给 `scoresOfGeoHashBox` 再进 `geoGetPointsInRange`。

**区间扫描：`geoGetPointsInRange` 按编码分三支。** 构造的 `zrangespec` 是 `minex = 0`、`maxex = 1`，即「min 含、max 不含」，注释写作 `min <= val < max`。`OBJ_ENCODING_LISTPACK` 用 `zzlFirstInRange` 找起点再 `zzlNext` 推进；`OBJ_ENCODING_SKIPLIST` 用 `zslNthInRange(zsl, &range, 0, NULL)` 找起点再走 `ln->level[0].forward`；`OBJ_ENCODING_BTREE` 用 `zbtreeIteratorSeekScore` 定位再 `zbtreeIteratorNext` 逐项读。三支的退出条件一致：`zslValueLteMax` 判越界就 `break`，命中就 `geoArrayAppend` 记进数组，`ga->used >= limit` 也 `break`。起点为空（`zzlFirstInRange` / `zslNthInRange` / `zbtreeIteratorSeekScore` 返回失败）就直接回 0。

**精确过滤：`geoWithinShape`。** 先把 score 解码回经纬度，再按形状分两路：圆形调 `geohashGetDistanceIfInRadiusWGS84`，半径传 `shape->t.radius * shape->conversion`，内部是 `geohashGetDistance` 的 haversine 加上「距离大于半径就返回 0」；矩形调 `geohashGetDistanceIfInRectangle`，先算纬度方向距离 `geohashGetLatDistance`（源码注释说它比经度距离便宜，所以先判）与 `height_m/2` 比较，再算同纬度上的经度距离 `geohashGetDistance(x2, y2, x1, y2)` 与 `width_m/2` 比较，两个都不超才写 `*distance`。`geohashGetDistance` 里有一个短路：两经度差为 0 时 `sin((lon2r-lon1r)/2)` 得 0，直接走 `geohashGetLatDistance`，省掉三角运算。

**排序与回复。** `result_length = ga->used`，`returned_items` 取 `count` 与 `result_length` 的较小值（`count == 0` 时即全部）。需要排序时，若 `returned_items == result_length` 就 `qsort` 整个数组，否则 `pqsort(ga->array, result_length, sizeof(geoPoint), cb, 0, returned_items-1)` 只把前 N 个排好（`pqsort` 在 `src/pqsort.c` 里以 `lrange` / `rrange` 为界剪枝，注释写「只处理目标区间」）。比较器 `sort_gp_asc` 用 `gpa->dist > gpb->dist` 三分支返回，因为 double 差值不能直接塞进 int。回包时先把 `gp->dist /= shape.conversion` 换回用户单位；`option_length` 累加 `withdist` / `withcoords` / `withhash`，非零时每个结果发成嵌套数组，长度是 `option_length + 1`；成员用 `addReplyBulkSds` 发出后立刻把 `gp->member` 置 NULL 交给回复缓冲区；WITHDIST 走 `addReplyDoubleDistance`，它用 `fixedpoint_d2string` 取 4 位小数（注释给的理由是「5.2145 meters away」比 15 位小数好看）；WITHHASH 回 `gp->score`；WITHCOORD 回解码出的经纬度。

**STORE 分支。** 有 `storekey` 时不进回复循环，而是 `zsetTypeCreate(returned_items, 0)` 建目标 zset，逐项 `zsetAdd` 写入，score 用 `storedist ? gp->dist : gp->score`，并 `serverAssert(out_flags & ZADD_OUT_ADDED)` 断言每个成员都是新增；写成功后 `setKey`、按 flag 发 `geosearchstore` 或 `georadiusstore` 通知、`server.dirty += returned_items`；`returned_items` 为 0 就 `dbDelete` 目标键并 `keyModified`、发 `del` 通知、`server.dirty++`，最后回 `returned_items` 这个整数。

**其余命令。** `geohashCommand` 用 base32 字母表 `0123456789bcdefghjkmnpqrstuvwxyz` 重新编码：先 `decodeGeohash` 解出坐标，再用 `-180..180` / `-90..90` 的 `GeoHashRange` 以 step 26 编码，输出 11 个字符，第 11 个字符（下标 10）固定取 `geoalphabet[0]`，注释写明「我们只有 52 位，为了兼容按 0 假定」。`geoposCommand` 缺失成员回 `addReplyNullArray`，命中就回 `[经度, 纬度]` 两元素数组。`geodistCommand` 在 `c->argc == 5` 时用 `extractUnitOrReply` 取单位、`c->argc > 5` 直接报语法错，然后要求两个成员都能取到 score，任一取不到就回 null；距离是 `geohashGetDistance(...) / to_meter`。单位换算由 `extractUnitOrReply` 给：`m` 为 1、`km` 为 1000、`ft` 为 0.3048、`mi` 为 1609.34，其余回 `unsupported unit provided. please use M, KM, FT, MI` 并返回 -1。`extractDistanceOrReply` 在半径小于 0 时回 `radius cannot be negative`，`extractBoxOrReply` 在宽或高小于 0 时回 `height or width cannot be negative`。

**入口包装。** `georadiusCommand` 传 `RADIUS_COORDS`；`georadiusbymemberCommand` 传 `RADIUS_MEMBER`；`georadiusroCommand` 传 `RADIUS_COORDS|RADIUS_NOSTORE`；`georadiusbymemberroCommand` 传 `RADIUS_MEMBER|RADIUS_NOSTORE`；`geosearchCommand` 传 `GEOSEARCH`；`geosearchstoreCommand` 传 `srcKeyIndex` 为 2、标志为 `GEOSEARCH|GEOSEARCHSTORE`。`RADIUS_NOSTORE` 的作用是让 STORE / STOREDIST 的解析分支直接落到 `else` 报语法错，所以 `_RO` 变体在只读调用方也能安全执行。

## 关键取舍

**源码事实：** 精度被 `GEO_STEP_MAX` 锁死为 26 步即 52 位，写入时一步到位，之后没有提高精度的接口；读回走 `geohashDecodeAreaToLongLat` 的格心，所以 GEOPOS / GEODIST 用的是格心坐标，位置误差只由那 52 位决定。

**源码事实：** 查询是两段式，先用估算步长的盒子粗筛、再用 haversine 精筛。步长估算 `geohashEstimateStepsByRadius` 只做了「纬度绝对值大于 66 减 1、大于 80 再减 1」两档放宽，其余纬度差异不处理；正确性靠 `geohashCalculateAreasByShapeWGS84` 的边缘检查和 `GZERO` 兜底，宁可盒大也不漏，多出来的候选由 `geoWithinShape` 丢掉。

**源码事实：** `COUNT` 搭配 `ANY` 时 `limit` 直接传 `count`，`membersOfAllNeighbors` 与 `geoGetPointsInRange` 一达到就 `break`，所以结果既不排序也不保证是全局最近的 N 个；不搭 `ANY` 时源码强制 `SORT_ASC` 并用 `pqsort` 只排前 N 个，用部分排序换掉全量排序。

**源码事实：** 三个区间扫描分支由 `zobj->encoding` 决定，geo 层对编码本身不设任何要求，只依赖「按 score 有序、能给闭开区间扫一遍」这一条。`membersOfAllNeighbors` 里对重复盒的判重只与 `last_processed` 这一个下标比较，不是与所有已处理盒比较。

**架构推论：** GEOADD 改写成 ZADD 再转发，等于让 geo 集合完整继承有序集合的一切：编码阈值、NX/XX/CH 语义、`server.dirty` 计数、键空间通知、RDB 与 AOF 的序列化路径。代价是 geo 只是 score 的一种编码约定而不是独立类型——任何 ZADD / ZINCRBY 都能往同一个键里塞进无法解码成坐标的 score，那时 `decodeGeohash` 失败，GEOPOS、GEODIST、GEOHASH 都只能回 null。

**架构推论：** 传播面同理：`geoaddCommand` 用 `replaceClientCommandVector` 换掉 `c->argv` 后调 `zaddCommand`，`call()` 收尾按 `c->argv` 走 `alsoPropagateEx`，于是副本与 AOF 看到的是改写后的 ZADD 而不是 GEOADD；`zaddGenericCommand` 里的 `server.dirty += (added+updated)` 正是让这次传播被判定为有数据变更的依据。

**架构推论：** 一人一分值的模型决定了 geo 索引只能回答「哪些成员落在某个圆或轴对齐矩形内」，不能存额外属性、不能做多边形或空间连接；要过滤业务字段只能在客户端拿回候选后再做，要换形状就得在客户端拼多个查询。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/geo.c` | `geoaddCommand` | 解析 NX/XX/CH、校验经纬度范围、52 位编码、改写成 ZADD 并转发 |
| `src/geo.c` | `georadiusGeneric` | 六个搜索命令共用入口：flag 分派、选项解析、覆盖盒、采集、排序、回复与 STORE |
| `src/geo.c` | `georadiusCommand` / `georadiusroCommand` / `georadiusbymemberCommand` / `georadiusbymemberroCommand` / `geosearchCommand` / `geosearchstoreCommand` | 只负责传 `srcKeyIndex` 与 flag 组合的薄包装 |
| `src/geo.c` | `extractLongLatOrReply` | 读经纬度并按 `GEO_LONG_*` / `GEO_LAT_*` 四个常量卡范围 |
| `src/geo.c` | `longLatFromMember` | 取成员 score 再解码成坐标，失败回 C_ERR（GEORADIUSBYMEMBER / FROMMEMBER 用） |
| `src/geo.c` | `extractUnitOrReply` / `extractDistanceOrReply` / `extractBoxOrReply` | 单位换算（1 / 1000 / 0.3048 / 1609.34）与半径、宽高的非负校验 |
| `src/geo.c` | `decodeGeohash` | 用 `GEO_STEP_MAX` 步把 score 解码回格心坐标 |
| `src/geo.c` | `geoWithinShape` | 圆形走半径判定、矩形走半宽半高判定，命中才写 distance |
| `src/geo.c` | `geoGetPointsInRange` | 按 LISTPACK / SKIPLIST / BTREE 三支扫描闭开区间，命中即入数组并按 limit 截断 |
| `src/geo.c` | `scoresOfGeoHashBox` / `membersOfGeoHashBox` | 一个盒子折成 `[min, max)` 两个 score 边界并交给区间扫描 |
| `src/geo.c` | `membersOfAllNeighbors` | 自身加八个邻居共九个盒的顺序采集、零盒与重复盒跳过、limit 提前退出 |
| `src/geo.c` | `geoArrayCreate` / `geoArrayAppend` / `geoArrayFree` | 结果数组：首次扩容按 8 起步、之后每次翻倍，释放时逐个 `sdsfree` 成员 |
| `src/geo.c` | `sort_gp_asc` / `sort_gp_desc` | 按 `dist` 三分支比较，供 `qsort` / `pqsort` 使用 |
| `src/geo.c` | `addReplyDoubleDistance` | 距离按 4 位小数格式化后回包 |
| `src/geo.c` | `geohashCommand` / `geoposCommand` / `geodistCommand` | 单点回读：11 字符 geohash、格心经纬度、两成员距离 |
| `src/geohash_helper.c` | `geohashEstimateStepsByRadius` | 由米制半径估步长：翻倍到 `MERCATOR_MAX` 再减 2，±66 / ±80 两档放宽，夹到 1..26 |
| `src/geohash_helper.c` | `geohashBoundingBox` | 用 `EARTH_RADIUS_IN_METERS` 与纬度余弦把形状折成经纬度包围盒 |
| `src/geohash_helper.c` | `geohashCalculateAreasByShapeWGS84` | 编码中心格、算八邻居、边缘检查回退一步、`GZERO` 清无用盒 |
| `src/geohash_helper.c` | `geohashAlign52Bits` | 把 step 步的编码左移 `52 - step*2` 位对齐成 52 位 score |
| `src/geohash_helper.c` | `geohashGetDistance` / `geohashGetDistanceIfInRadius` / `geohashGetDistanceIfInRectangle` | haversine 距离、半径判定、半宽半高矩形判定 |
| `src/geohash.c` | `geohashEncode` / `geohashDecode` | 经纬度与格子之间的双向编码，越界或非法 step 返回 0 |
| `src/geohash.c` | `geohashDecodeAreaToLongLat` | 取格子区间中点并夹到坐标上下界，这是 GEOPOS 的实际取值口径 |
| `src/geohash.c` | `geohashNeighbors` / `geohash_move_x` / `geohash_move_y` | 按 `step*2` 掩码在交织位串上做经纬度方向的偏移，得到八邻居 |
| `src/t_zset.c` | `zaddCommand` / `zaddGenericCommand` | GEOADD 的真正落库方：编码选择、NX/XX/CH、dirty 与通知 |
| `src/t_zset.c` | `zsetTypeCreate` | 按 `zset-max-listpack-entries` 与 `zset-max-listpack-value` 选初始编码 |
| `src/t_zset.c` | `zsetScore` | 按编码取成员分值，GEOPOS / GEODIST / FROMMEMBER 都靠它 |
| `src/t_zset.c` | `zzlFirstInRange` | listpack 编码下找区间起点，不做有效区间就早退 |
| `src/pqsort.c` | `pqsort` | 只排 `[lrange, rrange]` 的部分快速排序，用于 COUNT 截断场景 |
| `src/util.c` | `fixedpoint_d2string` | 定点小数格式化，WITHDIST 的 4 位精度来自这里 |
| `src/db.c` | `georadiusGetKeys` | 为 key-spec 与 ACL 提取 STORE / STOREDIST 指定的目标键 |

## 与主核 / AI 的关系

本仓里 geo 层没有其他 C 侧调用方：`src/geo.c` 的辅助函数只在自身内部被调用，外部只经 `server.h` 的命令声明进入。唯一的外部接触点是 `src/db.c` 的 `georadiusGetKeys`：它在 `georadius` / `georadiusbymember` 的 key-spec 不够用时按 `store` / `storedist` 扫出目标键，注释说明重复选项按 last-wins 处理，与 `georadiusGeneric` 的解析口径保持一致；ACL 与 `getKeysFromCommandWithSpecs` 的回退路径就走这条函数。

**架构推论：** 对 AI 侧来说，这套索引能提供的是「按地理范围召回成员」而不是向量近邻检索：它没有 top-k 近似最近邻、没有多维距离度量，只有圆、轴对齐矩形和 `COUNT [ANY]` 截断这三种筛选形态，排序键只有一个标量距离。若主核要给 chunk 或文档打上地理标签并做邻近召回，可以照「zset score 存编码 + 盒粗筛 + 距离精筛」这个形态落地，把编码与距离度量换成自己的，而不必新建一种数据类型。

## 参考

- [Redis 源码：geo.c](https://github.com/redis/redis/blob/unstable/src/geo.c)
- [Redis 源码：geohash_helper.c](https://github.com/redis/redis/blob/unstable/src/geohash_helper.c)
- [Redis 文档：Geospatial 数据类型](https://redis.io/docs/latest/develop/data-types/geospatial/)
- [Redis 命令：GEOADD](https://redis.io/docs/latest/commands/geoadd/)
- [Redis 命令：GEOSEARCH](https://redis.io/docs/latest/commands/geosearch/)
