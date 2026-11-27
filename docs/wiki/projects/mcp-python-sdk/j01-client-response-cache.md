---
project: mcp-python-sdk
feature: client-response-cache
title: 客户端响应缓存
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/client/caching.py
    symbols: [ClientResponseCache, CacheConfig, CacheKey]
  - path: src/mcp/client/caching.py
    symbols: [InMemoryResponseCacheStore, ResponseCacheStore]
---

# 客户端响应缓存

## 一句话职责

按服务端给的 `ttlMs`/`cacheScope` 提示缓存可缓存的响应，并把「缓存键」设计成**分臂（arm）的 JSON 字段元组**：每个键同时绑定授权分区、协议时代与响应作用域，于是私有条目不会跨用户串、不同协议时代的条目不会互服、公有条目在默认配置下也不会跨分区。

## 机制是什么

四个概念：

| 类型 | 角色 |
|---|---|
| `CacheKey` | 缓存身份：`(method, params_key, partition)` 三元组，`partition` 是**统筹器算出的臂标识**，对 store 不透明 |
| `CacheEntry` | 一个缓存值 + `scope`（`public`/`private`）+ `expires_at`（epoch 秒，`None` 表示永不新鲜） |
| `ResponseCacheStore` | 存储协议：`get`/`set`/`delete`/`clear` |
| `CacheConfig` | 客户端级配置：store、`partition`、`target_id`、`default_ttl_ms`、`clock`、`share_public` |

`ClientResponseCache` 是统筹器：算键、判时代、解 TTL 与作用域、处理驱逐竞态、把 store 的异常降级成 miss。

「臂」（arm）是这套设计的核心词：同一个逻辑键有两个臂——`private` 臂与 `public` 臂，各自是独立的 `CacheKey`。

## 怎么实现的

**缓存键不能拍平成字符串，这条写进了 `CacheKey` 的文档。** 它的 docstring 只有一句实质内容：**作为字段元组比较，绝不要拍平成字符串（碰撞风险）**。原因是三元组里两个字段来自外部（`method` 是协议方法名、`params_key` 可能是 URI），拼接后 `("a", "b")` 与 `("ab", "")` 会撞在一起。

**`_arm` 用 JSON 数组而不是拼接，理由与上一条同源。** 注释：**用 JSON 数组，于是精心构造的 `arm_id`/`partition` 值不能在字段边界上撞车**。`json.dumps([...])` 保证了分隔与转义，字符串拼接做不到。

**臂标识包含四段，每段都对一类错误。** `_arm(scope)` 构造：

| 段 | 作用 |
|---|---|
| `scope`（`public`/`private`） | 两个臂物理分离 |
| 协商出的协议版本 | **时代隔离**：不同协议时代的条目内容不同（被过筛的字段、头部过滤），绝不互服 |
| `arm_id` | 具体缓存实例身份 |
| `partition` | 授权上下文；**只在 `private` 臂、或 `share_public` 为假时的 `public` 臂才加入** |

第四段的两个条件是本页最需要记住的设计：`private` 臂永远带 `partition`；`public` 臂只有在**不共享**时才带。于是默认配置下 `public` 臂也按分区隔离，`share_public=True` 才让公有臂跨分区共享。

**`share_public` 的警告必须原文理解。** 它的文档写的是：**这会信任服务器对每个共享该 store 的 principal 的 `"public"` 分类——一个标错的响应就会跨租户泄漏**。而且它补了一句关键的约束：**只能在构造期设置；per-call 的 `cache_mode` 永远不能扩大共享范围**。也就是说「放宽共享」是一个不可被单次调用覆盖的安全决定。

**`partition` 的取值来源被明确规定。** 文档：**从已验证的凭据派生——绝不要用请求提供的数据或服务器 URL**；并且它在 `Client` 生命周期内固定，principal 变了就新建 `Client`。这是一个「不许动态」的约束：若允许中途变，缓存会在分区之间漂移，而漂移的缓存就是泄漏。

**`CacheConfig` 的三条构造期校验各有理由。** 自定义 store 必须显式给 `partition`（否则共享 store 上没有隔离）；`target_id` 不许是空字符串（`None` 表示「省略」，`""` 是无效值而不是省略）；`default_ttl_ms` 不许为负。

**TTL 的解析有两条规则，第二条容易被忽略。** `_resolve` 的做法：

1. 只有当**协商版本是现代**且结果**显式设过** `ttl_ms` 时才用它；否则退回 `default_ttl_ms`。
2. 最终值被 `min(ttl_ms, MAX_TTL_MS)` 截断，上限 24 小时。

第一条的理由写在注释里：**旧对端也能在线上放 `ttlMs`/`cacheScope` 键，所以线字段存在不是「对方是现代版本」的信号——提示只在现代时代才计数**。这是一个很重要的判断：不能用「出现了新字段」推断对端版本，必须用协商结果。

第二条还有一个细节注释：显式 `ttlMs: 0` **保持为 0**（不会退回默认值），而负数在上游已不可构造，所以只有上限生效。

**作用域解析默认落到私有。** `scope = "public" if modern and result.cache_scope == "public" else "private"`。也就是说：非现代时代一律私有；现代时代也要服务端明确说 `public` 才算。这与 `CacheableResult` 自身默认 `private` 的设计取向一致。

**读路径有两次查找，且第二次有一条防串臂检查。** `read` 先查 private 臂；未命中再查 public 臂，但**若查到的条目 `scope != "public"` 就丢弃**。注释说明了两个理由：一是「作用域翻转后，过期的私有条目不能遮住新鲜的公有条目」，二是「绝不从共享臂引出一个被服务端标为 private 的条目」。第二条是安全底线——不论条目怎么跑到那个臂里的，都不许从共享臂返回私有内容。

**读路径的异常边界包住了整个复制动作。** 注释：**形状不对的条目最晚会在复制时抛**，所以边界要覆盖整条读路径（含 `model_copy`），不能只包 `store.get`。任何读路径失败都变成 miss，`_warn_store_failure("get")` 记一次警告。

**写路径的顺序是为了「一个键永不由两个臂同时应答」。** `write` 的逻辑值得逐步看：

1. 若 `_generation_moved(...)`（该键在取数期间被驱逐过），直接返回，不写。
2. 解析 `ttl_ms` 与 `scope`，算出两个臂的键。
3. `ttl_ms <= 0`：不缓存。但若 `mode == "refresh"`，还要**清理删除两臂**——注释说明：重新取数已经取代了那个热条目，取消操作不能让它继续被服务。
4. 确定 `own`（由解析出的 scope 决定）与 `opposite`；**先删对侧臂**。注释：先删对侧，一个失败的删除可以在 set 之前中止，**于是永远不会有两个臂同时应答同一个键**。若对侧删除失败，连 own 臂也尽力删掉，退化成全 miss。
5. `set` 成功与否：失败则清理删除 own 臂（因为取数已取代了原有的 own 条目，而失败的 set 把它留在了原地）。
6. `finally` 里**再查一次** `_generation_moved`：驱逐可能在 set 提交期间发生（**甚至当这个 await 被取消时**），所以每次退出都要重查，而且这个删除**必须完成**，否则挂起的取消会让已被驱逐的条目复活。

第 6 步是本页最精妙的一处：它把「取消」与「并发驱逐」两个独立问题的交叉点显式处理了。

**驱逐的竞态用「代数（generation）」而不是锁。** `capture(method, params_key)` 在取数**之前**登记并返回当前代数；`write` 拿这个代数与当前值比较。`evict_key` **先自增代数再删除**，注释：**先 bump，一个在途的取数就无法把已驱逐的条目写回去**。而 `_generation_moved` 里有一个刻意的失手方向：**被 FIFO 挤掉的键会 fail open（接受这个共租户竞态）而不是丢弃取数结果**。

**代数表有上限，溢出策略是 FIFO 而不是永不限制。** `_GENERATION_MAP_CAP = 4096`，注释：到上限就丢最老的键的竞态守卫。另有一条注释解释了「未登记的键跳过 bump 但不跳过删除」：**URI 不能让这张表无限增长**，而持久 store 里可能存在未被 capture 的条目，所以删除必须照做。

**清理删除必须完成但有界。** `_cleanup_delete` 用 `anyio.move_on_after(timeout, shield=True)` 包住多个删除。注释给两条理由：**shield 让挂起的取消不能跳过删除**（否则一个臂会继续服务已驱逐的条目），**有界让一个卡住的 store 删除不能无取消地拖住客户端拆解**。超时会记一条警告并说明条目会随 TTL 自然过期。`_STORE_CLEANUP_TIMEOUT = 5` 秒，注释说它镜像了 dispatcher 的最终写界定。

**通知驱动的驱逐是「最终生效」而不是同步。** `evict_for_notification` 把四类通知映射到要失效的键：`tools/list`、`prompts/list`、`resources/list` **加** `resources/templates/list`（注释：模板枚举的是同一片会变的资源空间）、以及 `resources/read` 的**特定 uri**。docstring 明说驱逐是通过 spawn 的任务分发的，**代数自增把写回竞态关掉，但在途的读可能短暂返回旧条目**。

**store 的失败一律降级，且告警按操作种类合并。** `_warn_store_failure` 的注释写得很细：**每种操作每次故障潮只警告一次，且只有同类操作成功才重新武装**，所以一个健康的 delete 不会给坏掉的 set 重新武装。这个设计的目的是不让一个坏 store 把日志刷爆，同时又不至于一直静默。

**默认 store 是纯同步方法体，所以不会有撕裂写。** `InMemoryResponseCacheStore` 的 docstring 说明：方法体是同步的，所以并发任务永远观察不到撕裂写。它的 LRU 实现借用 dict 的插入顺序：`get` 用**弹出再插回**把键移到末尾，`set` 先 pop 再插入，超上限就删第一个（最久未用）。`max_entries=0` 关闭上限——`if self._max_entries and ...` 这个写法让 0 成为「不限」而不是「立刻全删」。

**`ResponseCacheStore` 的协议文档把责任边界写全了。** 四条值得记住：每个 `Client` 从**单个事件循环**调用它的 store；**每操作的原子性是实现者的责任**；**操作可以抛异常，SDK 降级为 miss 而不是让调用失败**；序列化的 store 必须把 `value` 还原成结果模型对象，**形状不对的条目算 miss、绝不算错误**。最后还有一条实现提示：**一次查找可能发起两次顺序的 `get`（先私有臂、再公有臂）**。

**per-call 的三种模式各管一半。** `CacheMode` 定义：`"use"` 既服务也存储；`"refresh"` **只存储不服务**；`"bypass"` 完全跳过缓存。这个划分让「强制刷新」不必先清缓存——`refresh` 会写入新值并取代旧值，同时（按 `write` 的第 3 步）在 `ttl<=0` 时清掉旧条目。

**`MAX_TTL_MS` 是硬上限。** 24 小时。服务端给的 `ttlMs` 再大也会被截断——这防的是一个服务端用「永不过期」把客户端的缓存变成不可控的内存/陈旧数据源。

## 关键取舍

**源码事实：** 缓存键是字段元组 + JSON 数组编码的臂标识，两者都刻意避免字符串拼接，理由是碰撞。

**源码事实：** 臂标识包含「协商版本」，于是不同协议时代的条目永不互服——同一逻辑键在升降级后会自然 miss。

**源码事实：** `partition` 只在私有臂、或未开启 `share_public` 的公有臂里参与；`share_public` 只能在构造期设置，per-call 模式无法扩大共享。

**源码事实：** TTL 提示只在协商版本为现代且结果显式设过该字段时生效；线字段存在不是版本信号。显式 `ttlMs: 0` 保持 0，上限 24 小时。

**源码事实：** 作用域默认私有；从公有臂读到的非 `public` 条目被丢弃，即使它真的在那里。

**源码事实：** 写路径先删对侧臂再写自己，保证一个键永不由两臂同时应答；任一步失败都退化为全 miss。

**源码事实：** 驱逐竞态靠代数自增处理（先 bump 再删），`write` 在 `finally` 里重查，且清理删除被 shield 且有界。

**源码事实：** 代数表有 4096 上限并 FIFO 淘汰；被淘汰的键 fail open 而不丢取数结果。

**源码事实：** 通知驱逐是异步分发的，因此在途读可能短暂返回旧值；`resources/list` 变化同时失效模板列表。

**源码事实：** store 的任何异常都降级为 miss；告警按操作种类去重且按同类成功重新武装。

**架构推论：** 这个缓存的核心难点不是「存与取」，而是**三个正交维度必须同时进入键**：谁（分区）、哪个协议时代（版本）、能不能共享（作用域）。少任何一维都会产生一类静默错误：少分区会跨用户串数据；少时代会让升级后的会话读到旧形状的条目；少作用域会让标为私有的响应从共享路径漏出。而**用两个臂而不是一个带标志的臂**，是为了让「先删后写」有确定的顺序——单臂设计里 set 是覆盖，无法表达「这个键的另一个作用域副本必须消失」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/client/caching.py` | `CacheKey` | 缓存身份三元组，明确禁止拍平成字符串 |
| `src/mcp/client/caching.py` | `CacheEntry` | 值 + 作用域 + 过期时刻；`None` 表示永不新鲜 |
| `src/mcp/client/caching.py` | `CacheMode` | per-call 三态：use / refresh / bypass |
| `src/mcp/client/caching.py` | `MAX_TTL_MS` | 24 小时硬上限，服务端提示会被截断 |
| `src/mcp/client/caching.py` | `CacheConfig` | 客户端级配置与三条构造期校验 |
| `src/mcp/client/caching.py` | `CacheConfig.partition` | 从已验证凭据派生、生命周期内固定 |
| `src/mcp/client/caching.py` | `CacheConfig.share_public` | 跨分区共享公有条目的显式开关，仅构造期生效 |
| `src/mcp/client/caching.py` | `ResponseCacheStore` | 存储协议与责任边界：单循环、可抛、错形状=miss |
| `src/mcp/client/caching.py` | `InMemoryResponseCacheStore` | 默认实现：同步方法体 + dict 插入序 LRU |
| `src/mcp/client/caching.py` | `ClientResponseCache._arm` | 臂标识：JSON 数组编码 scope/版本/arm_id/分区 |
| `src/mcp/client/caching.py` | `ClientResponseCache.read` | 两臂查找、公有臂的非 public 条目丢弃、整路径异常边界 |
| `src/mcp/client/caching.py` | `ClientResponseCache.write` | 先删对侧再写自己、`finally` 重查代数、失败退化 |
| `src/mcp/client/caching.py` | `ClientResponseCache._resolve` | TTL 与作用域解析：只在现代时代吃提示，含上限截断 |
| `src/mcp/client/caching.py` | `ClientResponseCache.capture` / `_generation_moved` | 代数竞态守卫与 FIFO 淘汰 |
| `src/mcp/client/caching.py` | `ClientResponseCache._cleanup_delete` | shield + 有界的必须完成删除 |
| `src/mcp/client/caching.py` | `ClientResponseCache.evict_for_notification` | 四类通知到失效键的映射 |

## 参考

- [mcp-python-sdk 源码：client/caching.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/client/caching.py)
- [MCP 规范：可缓存结果与新鲜度提示](https://modelcontextprotocol.io/specification/2025-06-18/server/resources)
- [anyio 文档：取消与 shield](https://anyio.readthedocs.io/en/stable/cancellation.html)
- [HTTP 缓存语义：共享与私有](https://datatracker.ietf.org/doc/html/rfc9111)

## 与主核 / AI 的关系

客户端响应缓存与主核的共享缓冲区缓存面对同一组矛盾：命中率、隔离、失效。数据库要按事务隔离级别决定「这个块能不能被别的会话看到」，而这里要按授权分区决定「这个条目能不能被别的 principal 看到」——判定维度不同，但「先判隔离、再判新鲜、最后才取」的顺序是一致的。

对 agent 工程的直接价值有三条。其一，**缓存键必须把所有隔离维度显式编码**。「method + 参数」远远不够：还要有身份分区、协议版本、作用域。任何一维缺失都不会报错，只会静默串数据。其二，**服务端的 TTL 提示需要上限**。不截断就意味着服务端可以单方面把客户端的缓存变成无限期陈旧数据源。其三，**驱逐必须比写入更强势**。这里的做法是先自增代数再删除、并在写路径的 `finally` 里重查、清理删除还要 shield——三条合起来才保证「一个已失效的条目不会被在途的取数写回来」。只做其中一条，都会留下一个竞态窗口。
