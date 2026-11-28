---
project: goose
feature: gateway-pairing
title: 网关配对与白名单
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/gateway/pairing.rs
    symbols: [PairingStore, StoredPairing, StoredPendingCodes]
  - path: crates/goose/src/gateway/manager.rs
    symbols: [GatewayManager, secret_key_for, allowed_user_ids_from_platform_config]
---

# 网关配对与白名单

## 一句话职责

把「一个外部平台上的陌生人能否操作这台机器上的 agent」变成两阶段握手：先发一个短期有效的一次性配对码，兑换后成为长期配对记录；同时用一份历史遗留码的吊销流程，防止旧版本留下的明文码被继续使用。

## 机制是什么

`PairingStore` 持有一个 `RwLock<HashMap<PlatformUser, PairingState>>`——内存里的配对表。`PairingState` 是个枚举（`Unpaired` / `Paired { session_id, paired_at }`）。

持久化分两条完全不同的路径，这是这一页的核心结构：

- **配对记录**存进普通配置参数 `gateway_pairings`，序列化成 `Vec<StoredPairing>`。
- **待定配对码**存进**密钥库** `gateway_pending_codes`，序列化成 `StoredPendingCodes`。

`StoredPairing` 有四个字段：`platform`、`user_id`、`display_name`、`state`。

`StoredPendingCodes` 有 `codes: Vec<StoredPendingCode>`、`legacy_import_complete: bool`、`imported_legacy_codes: Vec<String>` 三部分；每个 `StoredPendingCode` 是 `code`、`gateway_type`、`expires_at` 三字段。

## 怎么实现的

**为什么配对记录能进普通配置、配对码必须进密钥库。** 这个不对称不是随意的：配对记录只说明「这个人已经通过验证」，它本身不是凭据——即使泄漏，攻击者也不能凭它冒充（他仍需要平台侧的身份）。而**待定配对码就是凭据**：谁拿到那 6 位码就能兑换成一次配对。所以它必须进密钥库。

`PENDING_CODES_CONFIG_KEY` 与 `PENDING_CODES_SECRET_KEY` 两个常量**字符串相同**（都是 `gateway_pending_codes`），但用途不同：前者用来查普通配置里是否还有旧数据，后者用来写密钥库。同名是有意的，这样「查旧位置」与「写新位置」用同一个名字，便于迁移。

**配对码的生成。** `generate_code()` 用 `rand` 从 `"ABCDEFGHJKLMNPQRSTUVWXYZ23456789"` 里取 6 个字符。

这个字符集刻意排除了四组容易混淆的字符：`I`、`O`（与 `1`、`0` 形近）、`0`、`1`（本身）。所以是 32 个字符、6 位——约 10 亿种组合。对一个 5 分钟有效、且需要消耗一次有效兑换的码来说，这个空间足够。

有效期在 `GatewayManager::generate_pairing_code` 里给出：`Utc::now().timestamp() + 300`，即 5 分钟。

**待定码的读改写都是原子的。** `store_pending_code` 与 `consume_pending_code` 都不直接读再写，而是走 `Config::update_secret(key, |stored| ...)`——一个「读-改-写在一个原子操作里」的接口，闭包返回 `SecretUpdate::Write(值, 结果)` 或 `Unchanged(结果)`。

`consume_pending_code` 的逻辑最值得看：它在闭包里先判 `legacy_import_complete`，然后吊销已导入的旧码，再按 `position` 找目标码：

- **找不到**：若前面有改动就 `Write(state, None)`，否则 `Unchanged(None)`。
- **找到**：`state.codes.remove(position)` 后 `.filter(|pending| now <= pending.expires_at)` 判过期，`.map(|pending| pending.gateway_type)` 取出类型——**无论如何都 `Write`**。

所以「过期码」也会被**移除**并返回 `None`。这是正确的：过期的码不该继续占着列表，也不该被重试。而「未找到」可能只是「码不对」，不该写盘（除非迁移用到了）。

返回值的类型 `Option<String>` 是 `gateway_type` 而不是布尔——因为兑换成功后需要知道「这个码是给哪种网关用的」。

**迁移的三重防护。** `migrate_pending_codes` 是这一页最独特的部分。它做的事：

1. 用 `get_param_source_values` 读**普通配置**位置上的旧数据（复数形式的 `_source_values` 意味着它同时覆盖环境变量与配置文件的旧键）。
2. 若为空就返回 `Ok(())`。
3. 否则：把旧的码记进 `imported_legacy_codes` 并把对应的现行码**吊销**（`revoke_imported_codes`），标 `legacy_import_complete = true`，写回密钥库。
4. 然后**返回一个错误**，消息要求用户「停止旧的 Goose 进程、从配置文件里删除 `GATEWAY_PENDING_CODES` 与 `gateway_pending_codes`、重试并生成新的配对码」。

第 4 步的返回错误是关键设计：迁移不是静默完成的，而是**故意失败**。理由可以从消息内容推出来——如果旧进程还在跑，它会继续读旧位置的码；新版即使把这些码吊销了，旧进程也会重新写回去。所以光吊销不够，必须让人介入把旧数据清掉。这是一次「必须惊动用户才能收敛」的迁移。

`StoredPendingCodesValue` 那个 untagged 枚举是兼容层：既能读新结构（带两个额外字段的对象），也能读旧结构（裸的 `Vec<StoredPendingCode>` 数组）。`into_state` 把旧的数组形态包装成新结构并置 `legacy_import_complete: false`——这个 `false` 就是「还没迁移过」的标记，于是下次任何读写都会触发一次完整迁移。

`revoke_imported_codes` 的实现是 `retain`：把 `codes` 里所有出现在 `imported_legacy_codes` 中的码删掉，返回是否真删了。所以一旦某个码被标记为「旧版遗留」，它永远不会被兑换——即使攻击者从旧位置读到了它。

`StoredPendingCodesValue::default()` 是显式实现的（返回 `State(StoredPendingCodes::default())`），因为 derive 出来的 `Default` 对一个 untagged 枚举会选第一个变体，显式实现让意图明确。

**配对记录走内存加全量落盘。** `set`、`remove`、`remove_all_for_platform` 都在写锁内改 map，然后 `save_pairings_to_config(&pairings)` 把整个 map 序列化成 `Vec<StoredPairing>` 写进配置。

所以「配对」与「取消配对」都是全量重写。规模上是「这台机器配过几个用户」，通常是个位数到几十，全量写完全可接受。

`load_pairings_from_config` 在构造时读一次，失败时 `unwrap_or_default()` 得到空表——配置坏了等于「没有任何配对」。这个宽容方向偏安全：宁可让所有人都要重新配对，也不要在数据可疑时放行。

**`get` 的默认值表达语义。** `pairings.get(user).cloned().unwrap_or(PairingState::Unpaired)`——「没记录」就是「未配对」。这个默认值让所有查询点都不必处理 `Option`，且默认方向是安全的那一侧。

**`list_paired_users` 只列已配对。** 它遍历 map，只把 `PairingState::Paired { .. }` 的条目收进结果（连同 `session_id` 与 `paired_at`），且只收 `user.platform == gateway_type` 的。所以「待定」与「未配对」都不出现在列表里——一个用于展示「谁在用这台机器」的接口，不该把「正在申请的人」混进去。

**平台级批量清理。** `remove_all_for_platform(platform)` 用 `retain` 删掉某平台的全部记录，返回删除数量。调用点很明确：`GatewayManager::remove_gateway` 在移除网关时调它——注释那条日志写「failed to clear pairings on remove」，且失败只是 warn 不阻断。

为什么移除网关要连带清配对？因为配对记录里带着 `session_id`（goose 侧会话标识）。网关没了，那些会话对该平台用户再无意义，留着既占空间又可能在将来误用。

**`GatewayManager` 的三字段。** `gateways: RwLock<HashMap<String, GatewayInstance>>`、`pairing_store: Arc<PairingStore>`、`services: Arc<StateMachineServices>`。构造时先建 `PairingStore::new()?`——所以配对表的加载失败会让整个 manager 建不起来。

**`secret_key_for` 是密钥命名空间。** 一行：`format!("gateway_platform_config_{}", gateway_type)`。所以每种网关（Discord、Slack 之类）的**平台配置**（含 bot token 之类）各存一份密钥。

它被用在两处：`load_saved_configs` 时 `get_secret` 读取，失败就**跳过这个网关**并记 warn（注释：「skipping gateway with missing platform config secret」）；`save_config` 时 `set_secret` 写入。

所以「普通配置里存 `gateway_configs`（无密钥：类型、最大会话数、白名单），密钥库里存 `gateway_platform_config_<type>`（有密钥）」——与 g04 的 `api_key_env` 手法完全同源。这是产品内一致的原则：**配置存引用与元数据，密钥存密钥**。

**白名单的三态与三个来源。** `allowed_user_ids_from_platform_config(platform_config)` 从 `platform_config` 这个 JSON 值里取 `allowed_user_ids`：

- 取不到（不是数组）→ `None`；取到但过滤后为空 → 也 `None`（末尾 `.filter(|ids| !ids.is_empty())`）。所以「空数组」等价于「没有白名单」。
- 数组的每一项：字符串直接取，数字 `as_u64()` 后转成字符串。所以 Discord 那种数字 id 与 Slack 那种字符串 id 都能表达。
- 非字符串非数字的项被 `filter_map` 悄悄跳过。

而 `save_config` 把它**抽出来单独存**进 `SavedGatewayEntry.allowed_user_ids`，`load_saved_configs` 时再把它塞回 `platform_config["allowed_user_ids"]`。

为什么要把白名单从 `platform_config` 里抽出来单独存一份？因为 `platform_config` 整体在密钥库里。若白名单只存在密钥里，那么「查看这个网关允许哪些人」这个动作就要去读密钥库——而白名单不含秘密。抽出来存进普通配置，让「谁能用」这件事成为可读、可审计的公开信息，而 token 仍留在密钥里。

于是白名单有三个来源，按优先级：

1. `saved_allowed_user_ids(gateway_type)`——从 `SavedGatewayEntry` 里取，落盘的运营配置。
2. `platform_config["allowed_user_ids"]`——网关运行时的完整配置。
3. 若两者都没有 → `None`，即**不设白名单**。

注意 `load_saved_configs` 的顺序是「先读密钥里的 platform_config，再把落盘的白名单覆盖进去」（`platform_config["allowed_user_ids"] = json!(ids)`）。所以落盘的那份是**权威**——它覆盖了密钥里可能残留的旧值。这保证了「UI 改白名单」与「实际生效白名单」一致。

而 `None` 的含义要小心：「没有白名单」在配对语义里意味着**所有人都可以走配对流程**，通过配对的人就能用。所以白名单是**额外的收紧**，不是必要条件。

**取消配对按「已配对才返回真」。** `unpair_user` 先 `get`，`matches!(state, PairingState::Paired { .. })` 为真才 `remove` 并返回 `Ok(true)`；否则 `Ok(false)` 且**不动**记录。所以对一个「待定」用户调取消配对不会误删什么。

**状态快照。** `status` 对每个运行中的网关收集 `list_paired_users` 并映射成 `PairedUserInfo`（platform、user_id、display_name、session_id、paired_at）。`list_paired_users` 出错时 `unwrap_or_default()`——所以读配对表失败只是让状态里少几个人，不会让整个状态查询失败。

## 关键取舍

**源码事实：** 配对记录进普通配置，待定配对码进密钥库。两个常量名相同（`gateway_pending_codes`），一个用于查旧位置、一个用于写新位置。

**源码事实：** 配对码字符集排除了 `I`、`O`、`0`、`1`，共 32 字符、6 位。有效期 300 秒。

**源码事实：** 待定码的存取都走原子 `update_secret`。消费时无论命中与否（除了纯未找到且无迁移改动）都会写盘，过期码会被移除。

**源码事实：** 旧位置残留的码会被吊销并导致**迁移故意返回错误**，要求用户手工清理旧配置。光吊销不够，因为旧进程会写回去。

**源码事实：** `allowed_user_ids_from_platform_config` 把空数组与缺失都归为 `None`（即不设白名单）。

**源码事实：** 落盘的白名单在加载时会覆盖 `platform_config` 里的同名字段，所以落盘的版本是权威。

**源码事实：** `unpair_user` 只对「已配对」状态返回 `true`，对待定状态不做任何事。

**架构推论：** 「配对记录可公开、配对码必须保密」这个不对称，是这套机制的安全基石。若把配对码也放普通配置，那任何能读到配置文件的人都能完成配对——而配对的意义正是「只有能看到我屏幕上那个码的人才是我」。这条界线不是「敏感度分级」，而是「这个值本身是不是一次性的授权凭证」。

**架构推论：** 迁移故意失败这个做法，反映的是「无法单方面收敛的迁移」。绝大多数迁移是新代码读旧数据、改写成新形状、静默完成。但这里存在**另一个写入者**（旧进程），它在旧位置上持续写。新版无法阻止它，所以唯一出路是让人杀掉旧进程并清理旧数据。识别「是否存在并发的写入者」是判断「迁移能否静默完成」的关键问题。

**架构推论：** 白名单抽出来单独落盘，是一个「审计可见性」的设计。把「谁能用」与「用什么凭据」放在同一个密钥库里，会让权限信息变成不可查的东西——而权限信息本身应当是最容易被审计的。这个拆分让两者的可见性不同：白名单可读、token 不可读。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/gateway/pairing.rs` | `PairingStore` | 配对表的内存缓存与持久化 |
| `crates/goose/src/gateway/pairing.rs` | `StoredPairing` | 配对记录的落盘形状 |
| `crates/goose/src/gateway/pairing.rs` | `StoredPendingCodes` | 待定码集合，含迁移标记与已导入码 |
| `crates/goose/src/gateway/pairing.rs` | `PairingStore.generate_code` | 排除易混字符的 6 位码生成 |
| `crates/goose/src/gateway/pairing.rs` | `PairingStore.consume_pending_code` | 原子消费，过期码移除并返回 None |
| `crates/goose/src/gateway/pairing.rs` | `PairingStore.migrate_pending_codes` | 吊销旧位置码并故意返回错误 |
| `crates/goose/src/gateway/pairing.rs` | `StoredPendingCodes.revoke_imported_codes` | 让遗留码永远无法兑换 |
| `crates/goose/src/gateway/pairing.rs` | `PairingStore.remove_all_for_platform` | 移除网关时批量清配对 |
| `crates/goose/src/gateway/pairing.rs` | `PairingStore.list_paired_users` | 只返回已配对条目 |
| `crates/goose/src/gateway/manager.rs` | `GatewayManager` | 网关实例与配对库的持有者 |
| `crates/goose/src/gateway/manager.rs` | `secret_key_for` | `gateway_platform_config_<type>` 密钥命名空间 |
| `crates/goose/src/gateway/manager.rs` | `allowed_user_ids_from_platform_config` | 白名单提取，空归 None |
| `crates/goose/src/gateway/manager.rs` | `saved_allowed_user_ids` | 从落盘运营配置读白名单 |
| `crates/goose/src/gateway/manager.rs` | `GatewayManager.generate_pairing_code` | 生成码并设 300 秒有效期 |

## 参考

- [goose 源码：gateway/pairing.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/gateway/pairing.rs)
- [goose 源码：gateway/manager.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/gateway/manager.rs)
- [goose 文档：网关](https://block.github.io/goose/docs/guides/gateways/)

## 与主核 / AI 的关系

主核里最接近的是「客户端接入授权」：一台机器上的数据库只允许特定用户或特定来源连入，授权信息本身是可查的（在系统表里），而口令摘要或证书私钥是不可查的。这一页的两条存储路径正好对应这两个可见性层级，连「白名单要能审计」这个动机都一样。

与 smolagents 的 `SafeSerializer` 相比，两者的「严格度」来自不同的焦虑。`SafeSerializer` 担心的是**反序列化执行代码**，所以用白名单类型标记；这一页担心的是**凭据泄漏与重放**，所以用密钥库加一次性码加吊销。这也提示了一件事：安全设计没有统一答案，它取决于「你在防哪一类攻击」。

与 goose 自己的 g04（声明式提供方）对照，两者是同一套「配置存引用、密钥存密钥」原则的两个实例。g04 里是 `api_key_env` 存键名，这里 `secret_key_for` 存命名空间；g04 里密钥由用户在 UI 输入后入密钥库，这里 token 从平台配置来同样入密钥库。这种一致性本身就是设计资产——用户与开发者只需要理解一次这个模式。

对 AI 工程最实用的一条是「识别并发的写入者」这个迁移判据。做配置格式演进时，标准做法是「读到旧格式就转成新格式并写回」。但如果**旧版本的程序还在运行**（同机多进程、用户没升级、或容器里跑着两个版本），它会在旧位置继续写，你的迁移就会被反复回滚。`migrate_pending_codes` 的处理方式——吊销加故意报错要求人工介入——是唯一可靠的答案。判断标准很简单：这个数据是否可能被另一个进程或另一个版本写？

另一条是「配对码的字符集设计」。排除 `I`、`O`、`0`、`1` 是因为这个码要靠人从屏幕上抄到另一台设备上。所有「人工转录」的短码（配对码、设备码、恢复码）都该做同样的排除，否则用户会输入 `O` 而系统里是 `0`，得到「码无效」而不是「你抄错了」。这个细节看起来是产品体验，实际直接影响配对成功率。
