---
project: goose
feature: permission-store
title: 权限记录与判定缓存
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/permission/permission_store.rs
    symbols: [ToolPermissionStore, ToolPermissionRecord]
  - path: crates/goose/src/permission/permission_inspector.rs
    symbols: [PermissionInspector, cache_non_readonly_decision]
---

# 权限记录与判定缓存

## 一句话职责

把「用户批准了某个工具调用」这件事**持久化**下来（含过期时间与参数上下文哈希），以及把只读判定的结论**缓存**下来——但只缓存否定的那一半。两件事一起做，是为了让下一次同样的调用不必再问用户。

## 机制是什么

先分清两块完全不同的持久化：

| 位置 | 存什么 | 生命周期 | 键 |
|---|---|---|---|
| `ToolPermissionStore`（磁盘 JSON） | 每一次批准/拒绝的记录 | 跨进程，可带过期 | `工具名:参数哈希` |
| `PermissionManager` 的 smart_approve 缓存 | 「这个工具被判为非只读」 | 进程内 | 工具名 |

前者是审计与回溯的基础设施，后者是一个纯优化项。它们共享同一个判断依据（「这次调用是不是只读」），但落点不同：一个是「用户说过什么」，另一个是「模型说过什么」。

这个模块的核心矛盾是**粒度**。批准一个工具调用时，到底该放行多少？

- 放行太窄（只放行这一次具体参数）→ 用户每次都要重新点。
- 放行太宽（按工具名整个放行）→ 一次批准等于永久授权，风险过高。

`ToolPermissionRecord` 的字段设计就是这个矛盾的答案：它同时记下了**工具名**、**参数上下文哈希**、**可读上下文**、**是否允许**、**时间戳**与**过期时间**。也就是说它选择的是「按工具名 + 参数哈希」这一档，并用「可选过期」让调用方还能再收窄到「一段时间内有效」。

## 怎么实现的

**记录的形状。** `ToolPermissionRecord` 里有三个字段值得逐个看：

```rust
context_hash: String,       // Hash of the tool's arguments/context to differentiate similar calls
readable_context: Option<String>,  // #[serde(skip_serializing_if = "Option::is_none")]
timestamp: i64,
expiry: Option<i64>,
```

- **`context_hash`** 的注释直接说明用途：「differentiate similar calls」——同名工具带不同参数是不同的请求。这是把「放行粒度」从工具级降到调用级的实现。
- **`readable_context`** 是给**人**读的（`tool_request.to_readable_string()`），并且**为 `None` 时不写进 JSON**。这个 `skip_serializing_if` 不是省字节，而是避免在文件里留下大量 `"readable_context": null`——这份文件是给人看与排查用的，噪音越少越好。
- **`expiry: Option<i64>`** 用可选而不是哨兵值（比如 `0` 或 `i64::MAX`）。可选意味着「永不过期」与「到某刻过期」是两种不同的表达，而不是一个特殊数字。后文会看到这个选择如何简化了所有过期判断。

**存储的整体形状与版本位。**

```rust
pub struct ToolPermissionStore {
    permissions: HashMap<String, Vec<ToolPermissionRecord>>,
    version: u32, // For future schema migrations
    #[serde(skip)] permissions_dir: PathBuf,
}
```

三处设计：

1. **`Vec<ToolPermissionRecord>` 而不是单个记录**。同一个键可以有多条记录——按时间追加。于是「最近一次怎么说」就是取数组最后一条。这为「同一个调用被反复批准/拒绝」保留了历史。
2. **`version: 1`** 带注释「For future schema migrations」。文件里显式带版本号，将来字段变了就能据此判断要不要迁移。这是本地 JSON 存储里成本极低但价值很高的一手。
3. **`permissions_dir` 用 `#[serde(skip)]`**。它是一个运行期派生的路径，不该进文件；`load` 时读取完再手动回填：

```rust
permissions.permissions_dir = store.permissions_dir;
```

这一行是必需的——`serde_json::from_reader` 反序列化出来的结构里 `permissions_dir` 是 `PathBuf::default()`（空路径），若不回填，后续 `save` 会写到空路径上。

**原子写入用「临时文件 + rename」。**

```rust
let path = self.permissions_dir.join("tool_permissions.json");
let temp_path = path.with_extension("tmp");
std::fs::write(&temp_path, &content)?;
std::fs::rename(temp_path, path)?;
```

即先写完整的 `tool_permissions.tmp`，再原子改名覆盖目标。动机是崩溃与并发安全的交集：直接写目标文件若中途崩溃，会留下一个**截断的 JSON**，下次 `load` 会 `serde_json` 解析失败并整个丢掉所有历史批准记录（等于把所有授权重置成「每次都要问」）。用 rename 则要么是旧文件、要么是新文件。

`create_dir_all` 在写之前调，因为 `permissions/` 目录可能首次不存在。

**查权限：取最后一条未过期的记录。**

```rust
pub fn check_permission(&self, tool_request: &ToolRequest) -> Option<bool> {
    let context_hash = self.hash_tool_context(tool_request);
    let key = format!("{}:{}", tool_call.name, context_hash);
    self.permissions.get(&key).and_then(|records| {
        records.iter()
            .rfind(|record| record.expiry.is_none_or(|exp| exp > Utc::now().timestamp()))
            .map(|record| record.allowed)
    })
}
```

三个细节：

- **`rfind`**：从后往前找，所以拿到的是**最新的**那条有效记录。这与「追加写入」的存储形态配合——数组尾部是最新。
- **`expiry.is_none_or(|exp| exp > now)`**：`None` 视为永不过期，`Some` 才判时间。`is_none_or` 这个组合子让「可选过期」在判断处只有一行，不需要 `match`。这是前面「用可选而不是哨兵」的直接收益。
- **过期记录不会被删除，只是查不到**。清理是另一个动作（`cleanup_expired`），在 `load` 时统一做。

返回类型是 `Option<bool>` 而不是 `bool`：三种状态分别是「没有记录」「记录为允许」「记录为拒绝」。若压成 `bool`，调用方就无法区分「用户从没表过态」与「用户明确拒绝过」——而后者往往不该再问，前者必须问。

**写权限：追加一条记录、立即落盘。**

```rust
let record = ToolPermissionRecord {
    tool_name: tool_call.name.to_string().clone(),
    allowed,
    context_hash,
    readable_context: Some(tool_request.to_readable_string()),
    timestamp: Utc::now().timestamp(),
    expiry: expiry_duration.map(|d| Utc::now().timestamp() + d.as_secs() as i64),
};
self.permissions.entry(key).or_default().push(record);
self.save()?;
```

`expiry_duration: Option<Duration>` 直接映射成 `Option<i64>` 的时间戳——**「有没有过期」与「过期多久」用同一个 `Option` 表达**，不引入第三个状态。`or_default()` 让首次写入该键时自动建空向量。

注意 `record_permission` 签名是 `&mut self` 且每次都 `save()`。即**每次批准都是一次磁盘写**。这在交互频率上是可以接受的（用户批准一次就是一次），但它把「持久化」与「记录」绑在一起，没有批量写的余地。

**上下文哈希用 blake3，输入是 JSON 化的 arguments。**

```rust
fn hash_tool_context(&self, tool_request: &ToolRequest) -> String {
    let mut hasher = Hasher::new();
    hasher.update(
        serde_json::to_string(&tool_request.tool_call.as_ref().unwrap().arguments)
            .unwrap_or_default().as_bytes(),
    );
    hasher.finalize().to_hex().to_string()
}
```

三处：

- **先 JSON 化再哈希**，而不是哈希 `Debug` 输出或逐字段哈希。JSON 是参数的规范表示，`serde_json::to_string` 对同样的 `Value` 会给出同样的字符串（`HashMap` 底层用的是 `BTreeMap`（`serde_json` 默认按 `preserve_order` 关闭，键顺序稳定）），所以哈希可复现。
- **序列化失败时用空串**（`unwrap_or_default()`）。后果是「所有序列化失败的不同参数哈希到同一个值」——这是一个理论上的碰撞，但序列化 `serde_json::Value` 几乎不失败，所以这里选的是「简单」而不是「严格」。
- **`tool_call.as_ref().unwrap()`**：这个函数假定 `tool_call` 是 `Ok`。它是私有函数，调用方（`check_permission` / `record_permission`）也各自 `unwrap()`——即**畸形的工具调用在这层是未被防御的**。Rust 里这会 panic。结合 c01 里「畸形请求一律跳过」的做法，这里的差异值得注意：判官路径防了，持久化路径没有。

**`cleanup_expired` 的两层 retain 与「只在真删了东西时才写盘」。**

```rust
self.permissions.retain(|_, records| {
    records.retain(|record| record.expiry.is_none_or(|exp| exp > now));
    changed = changed || records.is_empty();
    !records.is_empty()
});
if changed { self.save()?; }
```

外层删掉「所有记录都过期」的键，内层删掉过期的记录。然后有个小但正确的细节：`changed` 只在**某个键变成空**时才置真——也就是说「某个键里删掉了一条但仍剩别的记录」不会触发写盘。

这带来一个可接受的后果：**部分过期的记录会留在内存里直到下次 `save`**。而功能上无碍，因为 `check_permission` 用 `rfind` 配过期判断，查的时候本来就会跳过它们。所以这里选的是「少写一次盘」而不是「文件绝对干净」——判据是「这份文件只影响加载时的初始状态，不影响查权限的正确性」。

`load()` 里在反序列化之后立即 `cleanup_expired()`，也就是**过期清理发生在进程启动时**。运行期不清理，只在查的时候过滤。

**`PermissionInspector` 把判定结果翻译成审批动作，而它的分支顺序就是策略。** `inspect` 面对每个请求按五步走：

| 顺序 | 条件 | 动作 |
|---|---|---|
| 0 | `GooseMode::Auto` | 全部 `Allow` |
| 1 | 用户显式权限（`get_user_permission`） | `AlwaysAllow`→Allow、`NeverAllow`→Deny、`AskBefore`→RequireApproval |
| 2 | `SmartApprove` 且工具带只读注解 | `Allow` |
| 3 | 工具是扩展管理工具 | `RequireApproval(带理由)` |
| 4 | `SmartApprove` 且缓存为「无」或「旧式允许」 | **推迟给 LLM 只读判定** |
| 5 | 其余 | `RequireApproval` |

这个顺序里有两条信息量最大的设计：

- **用户显式权限压过一切缓存**（第 1 步在 2/4 之前）。有测试直接叫 `smart_approve_preserves_user_permission_over_legacy_cache`：即使用户设了 `AskBefore` 而缓存里有个旧的 `AlwaysAllow`，结果仍是「需要批准」，且缓存不被改写。也就是说**缓存不能推翻用户的显式意志**。
- **第 4 步把「缓存为 `AlwaysAllow`」也当作「无缓存」重新判断**。这是因为早期版本可能缓存了「允许」而现在的策略变了，于是它被当作**过期的信任**，要重新判。对应测试 `smart_approve_rejudges_legacy_cached_allow`：结果从 Allow 变成 RequireApproval，并把缓存改写为 `AskBefore`。

也就是说 smart_approve 缓存**只用来加速「需要批准」这个结论**——一个已经被判为非只读的工具，下次直接问用户，不必再花一次模型调用。这正是 c02 里 `cache_non_readonly_decision` 做的事。

**`cache_non_readonly_decision` 只缓存否定的那一半，而这是整个缓存策略的核心。**

```rust
fn cache_non_readonly_decision(permission_manager: &PermissionManager, candidate: &ToolRequest, is_readonly: bool) {
    if is_readonly { return; }
    if let Ok(tool_call) = &candidate.tool_call {
        permission_manager.update_smart_approve_permission(&tool_call.name, PermissionLevel::AskBefore);
    }
}
```

三处值得说清：

1. **`is_readonly == true` 直接返回**。只读判定**不进缓存**——因为「这次调用是只读的」是关于**这一次具体参数**的结论，不能提升成「这个工具总是只读」。而且只读放行本身不需要加速：下次遇到同样的调用，判官会再判一次，成本可接受。
2. **非只读的结论按工具名缓存为 `AskBefore`**。这个结论的性质是「不确定/需要人」，它**可以**提升到工具级——因为「这个工具可能写东西」是个保守判断，即使误判也只会让用户多点一次。
3. 有一条测试叫 `smart_approve_only_caches_negative_name_wide_decisions`，它断言：`is_readonly = true` 时缓存仍是 `None`，`is_readonly = false` 时缓存变成 `Some(AskBefore)`。名字里的 `negative` 与 `name_wide` 把这两条都写清楚了。

**这正是「缓存只会让系统更保守」的实现**：缓存里唯一可能出现的值就是 `AskBefore`，而这个值的效果是「下次直接问用户」。所以缓存不可能造成静默放行，最坏的后果只是多做一次提问。

**只读集合与写缓存的分工，源代码里有一句直接交代。**

```rust
// readonly_tools is per-agent to avoid concurrent session clobbering; write-annotated
// tools are cached globally via PermissionManager.
```

即：**只读注解集合放在 `PermissionInspector` 实例上的 `RwLock<HashSet<String>>`，非只读判定放在全局的 `PermissionManager`**。理由是并发——把只读集合放进全局，多个会话同时更新它就会互相覆盖。

`apply_tool_annotations` 每次收到工具列表就**整体替换**这个集合（`*self.readonly_tools.write().unwrap() = readonly_annotated`），而不是增量合并。这是一处正确的选择：工具列表每次是全量的，若合并则已移除的工具会永远留在只读集合里。

它同时调 `self.permission_manager.apply_tool_annotations(tools)`——让全局管理器也知道注解。两条路径并行存放同一份信息的两个投影，职责不同：一个供本实例的快速判定，一个供跨会话的写缓存决策。

**`process_inspection_results` 的三段结构值得记。** 它把检查链的结果重新装配成 `PermissionCheckResult`：

1. **先按权限审查器自己的结果做基线**（`inspector_name == "permission"` 的结果）。逐个请求匹配 `tool_request_id`，`Allow`/`Deny`/`RequireApproval` 分别进三个桶。
2. **找不到权限结果时默认「需要批准」**，注释写「default to needs approval for safety」。
3. **再把其他审查器（安全、出口、对手）的结果当覆盖应用**——通过 `apply_inspection_results_to_permissions` 重新装配。

第 2 步与第 3 步合起来的语义是：**权限是基线，安全类是覆盖**。权限层说「这个工具用户允许了」，但安全层说「这次调用是恶意的」，最终仍应拦。d01 里讲的 `is_malicious && should_ask_user` 合取就是这条覆盖的落点。

还有一处实现细节：`permission_results` 只保留 `inspector_name == "permission"` 的那些，而 `remaining_requests` 是过滤后的待检列表。如果某个请求既没有权限结果也没有其他结果，它落到「需要批准」——这是**任何一处失效都偏向保守**的又一处证据。

`confidence: 1.0` 且注释「Permission decisions are definitive」——权限判定的置信度恒为 1。这与安全审查器要给置信度形成对照：权限是一个确定的事实（用户设了什么），安全是一个概率判断。

## 关键取舍

**源码事实：** 记录的键是 `工具名:参数哈希`，参数哈希是 `arguments` 的 JSON 字符串的 blake3，因此同名不同参是不同记录；数组按时间追加，查询用 `rfind` 取最新未过期项。

**源码事实：** `expiry` 用 `Option<i64>` 而不是哨兵值，`check_permission` 用 `is_none_or(|exp| exp > now)` 一行完成判断。

**源码事实：** 存储文件带 `version: 1` 为将来迁移留位；`permissions_dir` 用 `#[serde(skip)]` 且 `load` 后手动回填，否则 `save` 会写到空路径。

**源码事实：** 写入用「临时文件 + rename」保证原子性，避免崩溃留下截断 JSON 导致历史批准全部丢失。

**源码事实：** `cleanup_expired` 只在某个键被清空时才写盘，因此部分过期的记录可能留在内存里；这不影响正确性，因为查询时会按过期时间过滤。

**源码事实：** smart_approve 缓存**只缓存非只读结论**（写为 `AskBefore`），只读判定不进缓存；缓存中唯一可能的值使系统更保守，不可能是静默放行。

**源码事实：** 用户显式权限压过 smart_approve 缓存；缓存中的 `AlwaysAllow` 被当作「需重新判定」而不是权威允许。

**源码事实：** 只读注解集合是每个 `PermissionInspector` 实例的私有状态（避免并发会话互相覆盖），非只读判定则走全局 `PermissionManager`。

**架构推论：** 这套缓存的设计原则可以概括成一句：**缓存可以让人多做一次确认，不能让人少做一次确认**。所有具体实现都是从这句话推出来的——只缓存 `AskBefore`、把旧的 `AlwaysAllow` 当作无效、用户显式意志优先于缓存。这类「单向安全」的设计比「精确缓存」更容易做对，也更不容易在版本演进中退化。

**架构推论：** 用 `Option<bool>` 而不是 `bool` 返回，是「三态优于二值」的经典应用。用户从没表过态的调用必须问，明确拒绝过的可以记住不再问，明确允许的放行——这三种情形对交互的影响完全不同，压成一个布尔必然丢掉一种。

**架构推论：** `readable_context` 与哈希并存，揭示了持久化记录里「给机器用的键」与「给人看的说明」应当分开。若只存哈希，排查时无法知道当时到底批准了什么；若只存可读文本，键会太长且不稳定。两者并存是最省事也最实用的组合。

**架构推论：** 一处值得指出的不一致：`check_permission` 与 `record_permission` 都对 `tool_call` 做 `unwrap()`，而判官路径（c01）对畸形请求是明确跳过的。也就是说一条畸形的工具调用若走到这里会 panic，而不是退化成「需要批准」。这是一个可以被修正的边界——把 `unwrap` 换成 `?` 或前置过滤，纳入与判官一致的保守语义。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/permission/permission_store.rs` | `ToolPermissionStore` | 磁盘持久化：加载、保存、查询、记录、过期清理 |
| `crates/goose/src/permission/permission_store.rs` | `ToolPermissionRecord` | 单条记录：工具名、允许位、上下文哈希、可读上下文、时间、可选过期 |
| `crates/goose/src/permission/permission_store.rs` | `check_permission` | 按 `工具名:哈希` 查最新未过期记录，三态返回 |
| `crates/goose/src/permission/permission_store.rs` | `record_permission` | 追加记录并立即原子落盘 |
| `crates/goose/src/permission/permission_store.rs` | `hash_tool_context` | 参数 JSON 的 blake3 哈希 |
| `crates/goose/src/permission/permission_store.rs` | `cleanup_expired` | 两层 `retain`，只在键被清空时写盘 |
| `crates/goose/src/permission/permission_inspector.rs` | `PermissionInspector` | 权限审查器：五步分支把状态翻译成审批动作 |
| `crates/goose/src/permission/permission_inspector.rs` | `cache_non_readonly_decision` | 只缓存非只读结论为 `AskBefore` |
| `crates/goose/src/permission/permission_inspector.rs` | `apply_tool_annotations` | 整体替换本实例的只读集合，并同步全局管理器 |
| `crates/goose/src/permission/permission_inspector.rs` | `process_inspection_results` | 权限结果为基线、其他审查器结果为覆盖 |

## 参考

- [goose 源码：permission/permission_store.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/permission/permission_store.rs)
- [goose 源码：permission/permission_inspector.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/permission/permission_inspector.rs)
- [goose 文档：权限模式](https://block.github.io/goose/docs/guides/goose-permissions)
- [Rust 文档：std::fs::rename](https://doc.rust-lang.org/std/fs/fn.rename.html)

## 与主核 / AI 的关系

「持久化一份带过期时间的授权记录，并以内容哈希为键」与主核里的**权限缓存与 prepared statement 缓存**结构同源：主核缓存一条执行计划时，键是「规范化后的语句文本 + 参数类型 + 相关对象版本号」；这里键是「工具名 + 参数哈希」。两边都面对同一个问题——**只按名字缓存会导致不同参数串用同一个结论**，所以必须把参数（或语句文本）纳入键。

「临时文件 + rename」在主核里对应的是**WAL 与原子重命名**：主核写关键元数据文件时也采用「写临时文件、fsync、rename」这一套，理由完全相同——避免崩溃留下半截文件。而这里那个特别具体的动机（「截断的 JSON 会让下次加载失败并丢掉所有授权记录，等于把所有授权重置」）也非常主核：一个格式损坏的配置文件能让整个系统退回到最保守的默认。

「缓存只能让人多做确认，不能让人少做」这条原则，与主核里**缓存失效的默认方向**一致：主核缓存失效时宁可重算一次，也不愿给出一个可能过期的结果。这里的对应做法是把缓存唯一可能的值设成 `AskBefore`，把旧的「允许」当作无效——效果就是「缓存永远站在保守的一侧」。

对 AI 工程的实际意义在于：agent 的用户批准是一个需要被持久化的**决策**，而不只是一次 UI 交互。这份实现给出的三条经验可直接用：**键里必须包含参数而不只是工具名**（否则「读 A 文件」的批准会变成「读任意文件」的批准）、**过期用可选而不是哨兵值**（让「永不过期」与「到某刻过期」成为两种表达，判断处只需一行）、以及**缓存只缓存否定结论**。最后一条尤其值得记：在安全判定里，一条「允许」的缓存若错了就是静默放行，而一条「需要确认」的缓存即使错了也只是多问一次——两者的失败代价不对称，缓存策略就该按这个不对称来设计。
