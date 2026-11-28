---
project: goose
feature: session-runtime
title: 会话运行时与附件
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/agents/session_runtime.rs
    symbols: [SessionRuntime, Attachment]
  - path: crates/goose/src/agents/provider_manager.rs
    symbols: [ProviderManager, SharedProviders, SessionSlot]
---

# 会话运行时与附件

## 一句话职责

`SessionRuntime` 保存所有**活的**（live）会话的内存状态——哪些连接把它打开着、它是否常驻、它的运行、确认与待投递的 steer；`ProviderManager` 则按会话持有 provider 实例，并区分「每会话独占的 provider」与「可共享的 provider」两条路径。

## 机制是什么

会话记录存在 SQLite（见 a01），但「正在被使用」这件事只能在内存里表达，因为它的内容是：

| 状态 | 类型 | 含义 |
|---|---|---|
| 常驻集 | LRU 缓存 | 最多 `max_sessions` 个会话同时「驻留」，超出就淘汰 |
| 附件 | `HashMap<会话, IndexMap<连接, Attachment>>` | 哪些客户端连接打开了这个会话，按 **attach 顺序**排 |
| steer 队列 | `HashMap<会话, SteerQueue>` | 用户在一轮进行中插话，消息排在这里 |
| 生命周期锁 | `HashMap<会话, Arc<Mutex<()>>>` | 让「附件/释放」互斥 |
| 确认协调器 | `ToolConfirmationCoordinator` | 工具调用等待用户批准的等待点 |
| 运行注册表 | `ActiveRunRegistry` | 该会话是否有正在跑的 agent 轮次或 live 语音交互 |

`SessionRuntime` 自己的 docstring 只有一句，但把范围钉死了：「一个活会话除了它的记录之外 goose 还保留的一切」。

「附件」（`Attachment`）是这一层的核心概念，而且排序不是随意的：

```
pub struct Attachment {
    pub client: ClientContext,
    pub session_names: Option<mpsc::UnboundedSender<SessionNameUpdate>>,
}
```

`attachments` 用的是 `IndexMap<u64, Attachment>`，注释写明「每个会话按 attach 顺序；**最后一个是该会话当前对话的对象**」。于是同一个会话被多个客户端同时打开时（桌面端两个窗口、或桌面 + CLI），谁在最新，出站消息就发给谁。`attach` 里的 `shift_remove` 是关键：

```
connections.shift_remove(&connection);
connections.insert(connection, attachment);
```

先按连接 id 删掉旧条目再插入，于是「同一连接重新附加」会把它移到末尾——`IndexMap` 的插入顺序因此等价于「最近附加优先」。若用普通 `HashMap`，就无法定义「最后一个是哪个」。

`detach` 的返回值说明了同一件事：

```
pub(super) fn detach(&self, session_id: &str, connection: u64) -> Option<Attachment> {
    ...
    let latest = connections.last().map(|(_, attachment)| attachment.clone());
    if latest.is_none() { attachments.remove(session_id); }
    latest
}
```

返回值是「现在该跟谁说话」，而不是「被移除的那个」。无人附加时返回 `None` 并把条目整个删掉——于是 `is_open()` 与「附件表里有没有这个键」等价，不需要额外的标志位。

## 怎么实现的

**常驻集是一个 LRU，但淘汰有三条豁免。**

```
resident: Mutex<lru::LruCache<String, ()>>,
max_sessions: max_sessions.max(1),
```

缓存的值是 `()`——它只关心顺序，不关心内容。`touch` 把会话提到最近使用并返回该释放的会话：

```
pub(super) fn touch(&self, session_id: &str) -> Vec<String> {
    let mut resident = self.resident.lock()...;
    resident.put(session_id.to_string(), ());
    self.over_limit(&mut resident, Some(session_id))
}
```

`over_limit` 从 LRU 的尾部（最久未用）向前遍历，过滤掉两类：`keep`（刚被 touch 的那个，绝不被自己淘汰）与 `is_busy` 的会话。第三个豁免写在 docstring 里：「当其它每个会话都忙时，上限会被超出，直到一轮结束或另一个会话被触碰」。也就是说上限是**软**的——忙的会话不可能被淘汰，所以宁可变超。

`is_busy` 的定义把两件事合并：

```
fn is_busy(&self, session_id: &str) -> bool {
    self.confirmations.has_active_turn(session_id) || self.runs.is_active(session_id)
}
```

「正在等用户确认」与「有活跃运行」都算忙。注释补了一句更细的：「一个 Live 会话可以没有轮次在跑，而一次提示在它的轮次开始之前就注册了自己的运行」——两个判据不全等价，因为它们的登录时机不同。

**淘汰与「再次被选中」之间的竞态由 `keep_resident` 兜住。**

```
pub(super) fn keep_resident(&self, session_id: &str) -> bool {
    let mut resident = self.resident.lock()...;
    if resident.contains(session_id) { return true; }
    if self.is_busy(session_id) {
        resident.put(session_id.to_string(), ());
        resident.demote(session_id);
        return true;
    }
    false
}
```

docstring 说清了它防的是什么：「被选为淘汰候选之后又被触碰过，或者在忙（那就回到常驻集）」。`demote` 把重生的条目放到 LRU 尾部——**放回常驻集但排到最后**，意思是「它保住了，但不因为这次保住就获得最优先的地位」。这个细节决定了下一次超限时它仍是首选。

**生命周期锁与它的自我清理。**

```
pub(super) async fn lock_lifetime(&self, session_id: &str) -> LifetimeGuard<'_> {
    let lock = self.lifetimes.lock()...entry(session_id.to_string()).or_default().clone();
    LifetimeGuard { runtime: self, session_id: session_id.to_string(), _held: lock.lock_owned().await }
}
```

docstring：「在会话被附加或释放期间持有，于是一次释放永远不会清掉一个在它运行期间被附加的会话」。换句话说，`attach` 与 `detach` 之间不能插进一次「把整个会话丢掉」。

`LifetimeGuard` 的 `Drop` 做了一件不常见的事——**在被丢弃时检查是否还有别人在等这把锁**：

```
if lifetimes.get(&self.session_id).is_some_and(|lock| Arc::strong_count(lock) == 2) {
    lifetimes.remove(&self.session_id);
}
```

`Arc::strong_count == 2` 的含义是「只有这张表 + 我手里这一份」。此时可以安全地把条目从表里摘掉，否则表会随会话数无限增长。这个实现把「引用计数当活性检测用」，代价是数字 `2` 是一个需要注释的魔数——而源码确实给了注释：「表的这份引用与我们的这份：没有别人在等」。

**三条清理路径，作用范围不同。**

- `discard_pending_steers`：只删 steer 队列。
- `forget`：全删——常驻集、附件、steer、确认、以及**取消该会话的运行**（`runs.cancel_agent_run`）。
- `detach` 到无人附加时：只删附件条目。

`forget` 的 docstring 是「丢掉为该会话保留的一切并取消它的运行」。注意它调的是 `cancel_agent_run`（会触发取消令牌），而不是从注册表里移除——后者在 `ActiveRunRegistry::remove_agent_run` 里，由运行自身在结束时调用。这个分工保证了「删除会话」与「运行正常结束」两种结束方式都走同一套取消通道。

**`ProviderManager` 的两条路径由 `session_bound()` 分流。**

```
pub async fn provider_for(&self, session: &Session) -> Result<Arc<dyn Provider>> {
    let name = provider_name_for(session)?;
    let slot = self.slot(&session.id);
    let mut slot = slot.lock().await;
    if let Some(provider) = slot.as_ref().filter(|p| p.get_name() == name) {
        return Ok(provider.clone());
    }
    let entry = crate::providers::get_from_registry(&name).await?;
    if !entry.session_bound() {
        let provider = self.shared(&name, &entry).await?;
        *slot = None;
        return Ok(provider);
    }
    ...  // 每会话构造
}
```

逐层读：

1. **先查会话槽位**，且要校验 `get_name() == name`。这个校验不是多余的：会话可以在运行时换 provider，槽里可能还是旧的那个。
2. **`session_bound()` 为假** → 走共享表，并且**把会话槽清空**（`*slot = None`）。清空是必要的，否则这个会话会拖着一个并不属于它的实例，后面 `pinned()` 就会返回错的东西。
3. **为真** → 构造新实例，并把 `working_dir`、扩展状态、model config、goose mode 都灌进去。

第 3 支里有一段值得单独看的容错：

```
if let Err(e) = provider.apply_model_selection(&model_config).await {
    warn!("Failed to apply model selection to provider: {e}");
}
```

`apply_model_selection` 失败只警告，不失败；而紧接着的 `update_mode` 失败则转成错误：

```
.map_err(|e| anyhow!("Failed to propagate mode to provider: {e}"))?;
```

两者区别对待的依据大概是：模型选择是一个「偏好」——provider 可能不支持某个具体模型名，但它仍能工作；而 goose mode 决定审批/自治行为，传不进去就是错的。这处不对称是源码里的真实判断，值得记下来。

**共享 provider 按「配置代次」失效。**

```
async fn shared(&self, name: &str, entry: &ProviderEntry) -> Result<Arc<dyn Provider>> {
    let generation = Config::global().generation();
    let mut shared = self.shared.lock().await;
    if let Some((cached_generation, provider)) = shared.get(name) {
        if *cached_generation == generation { return Ok(provider.clone()); }
    }
    let provider = entry.create(Vec::new()).await?;
    shared.insert(name.to_string(), (generation, provider.clone()));
    Ok(provider)
}
```

`SharedProviders` 的类型就是 `HashMap<String, (u64, Arc<dyn Provider>)>`——名字 + 代次 + 实例。于是「配置改了」不需要显式通知缓存失效：代次一变，下次取用自然重建。这是一条极省事的失效机制，代价是每次取共享 provider 都要读一次 `Config::global().generation()`。

共享实例用 `entry.create(Vec::new())` 构造，即**不传扩展列表**。这与 `session_bound` 那条路径传入 `extensions` 形成对照：共享实例不属于任何一个会话，所以不能带某个会话的扩展上下文。

**`pinned` 与 `set_provider` 是「读」与「写」的两端。**

```
pub async fn pinned(&self, session: &Session) -> Option<Arc<dyn Provider>> {
    let name = provider_name_for(session).ok()?;
    self.slot(&session.id).lock().await.clone().filter(|provider| provider.get_name() == name)
}
```

docstring 只有一句：「已经在为这个会话服务的 provider，如果有的话；**永不构造新的**」。用途是「问一句现状」——例如界面要显示当前模型，不该因为去问一句就新建一个 provider。

`set_provider` 是它的对偶：直接把槽填上。于是存在第三条路径：某些调用方自己造好 provider 再放进去，绕过 `provider_for` 的分流逻辑。

**会话槽的锁是异步的，而会话表是同步的。**

```
type SessionSlot = Arc<tokio::sync::Mutex<Option<Arc<dyn Provider>>>>;
sessions: Mutex<HashMap<String, SessionSlot>>,
```

外层用 `std::sync::Mutex`（只为保护 map 的插入/查找，极短），每个槽用 `tokio::sync::Mutex`（因为构造 provider 是异步的，同一会话的并发取用必须串行）。这个双层锁的形状是「短临界区用同步锁、长临界区用异步锁」的标准做法。

`slot()` 的注释性细节：它用 `.entry(...).or_default().clone()`，即先拿锁构造一个空槽再克隆 `Arc` 出去，**避免在持有外层 map 锁时 await**。

**provider 名与模型配置有两个回退层级。**

```
pub fn provider_name_for(session: &Session) -> Result<String> {
    match &session.provider_name {
        Some(name) => Ok(name.clone()),
        None => Config::global().get_goose_provider().map_err(|_| anyhow!("Could not configure agent: missing provider")),
    }
}
```

会话里记了就用会话的，否则用全局配置。`model_config_for` 同理：

```
pub fn model_config_for(session: &Session) -> Result<ModelConfig> {
    if let Some(model_config) = &session.model_config { return Ok(model_config.clone()); }
    let config = Config::global();
    let provider_name = provider_name_for(session)?;
    let model_name = config.get_goose_model().map_err(|_| anyhow!("Could not resolve model config: missing model"))?;
    crate::model_config::model_config_from_user_config(&provider_name, &model_name)
        .map_err(|e| anyhow!("Could not resolve model config: {e}"))
}
```

注意 `model_config_for` 在回退时**先解析 provider 名**，因为用户配置里的模型名要与 provider 一起才能变成 `ModelConfig`（同一个模型名在不同 provider 下参数不同）。三个错误消息都以 `Could not …` 开头且给出缺什么，这对「首次配置」场景很重要——它们就是用户看到的提示。

## 关键取舍

**源码事实：** 附件用 `IndexMap` 而非 `HashMap`，且 `attach` 用 `shift_remove` + `insert` 来维护「最近附加的排在最后」；`detach` 返回的是「现在该跟谁说话」而不是被移除者。

**源码事实：** 常驻上限是软的：`over_limit` 过滤掉刚被 touch 的会话与所有 busy 会话，于是「其它每个会话都忙」时会超限而不淘汰。

**源码事实：** `keep_resident` 会把被淘汰后又变忙的会话放回常驻集，但用 `demote` 放到 LRU 尾部，于是它不会因此获得更长的存活期。

**源码事实：** `LifetimeGuard::drop` 用 `Arc::strong_count == 2` 判断「没有别人在等这把锁」，据此从表里摘掉条目，防止生命周期锁表无限增长。

**源码事实：** `provider_for` 分两条路径取决于 `entry.session_bound()`；共享路径会把会话槽清空，避免槽里留着一个不属于该会话的实例。

**源码事实：** `apply_model_selection` 失败只警告，而 `update_mode` 失败转错误——前者是偏好，后者决定审批行为。

**源码事实：** 共享 provider 以 `(配置代次, 实例)` 缓存，代次来自 `Config::global().generation()`；代次变化即自然失效，无需显式通知。

**源码事实：** `pinned()` 永不构造新 provider，只回报槽里已有的那个，且同样校验名字。

**架构推论：** 把「活状态」与「记录」彻底分开（SQLite vs 内存），使会话能被多个进程或多次启动复用，而「谁在用」只在进程内成立。代价是「常驻」变成一个需要上限与淘汰策略的资源——而这正是 `max_sessions` 存在的理由。

**架构推论：** 「附件顺序 = 最近附加优先」是一个隐式的路由规则：会话的出站消息发给最近打开它的那个客户端。换成广播会让每个窗口都收到别人触发的通知，换成固定绑定会让新开的窗口看不到后续消息。两种都更糟。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/agents/session_runtime.rs` | `SessionRuntime` | 活会话的全部内存状态：常驻集、附件、steer、锁、确认与运行 |
| `crates/goose/src/agents/session_runtime.rs` | `Attachment` | 打开某会话的一个客户端连接，携带名称更新通道 |
| `crates/goose/src/agents/session_runtime.rs` | `touch` / `over_limit` | LRU 提升与带两条豁免的淘汰 |
| `crates/goose/src/agents/session_runtime.rs` | `keep_resident` | 二次校验：淘汰候选若又被触碰或变忙则放回并降级 |
| `crates/goose/src/agents/session_runtime.rs` | `detach` | 移除连接并返回「现在该跟谁说话」 |
| `crates/goose/src/agents/session_runtime.rs` | `forget` | 全量清理并取消该会话的运行 |
| `crates/goose/src/agents/provider_manager.rs` | `ProviderManager` | 按会话的 provider 槽 + 全局共享表 |
| `crates/goose/src/agents/provider_manager.rs` | `SharedProviders` | `名字 → (配置代次, 实例)` 的代次失效缓存 |
| `crates/goose/src/agents/provider_manager.rs` | `SessionSlot` | 每会话一个异步互斥的 provider 槽 |
| `crates/goose/src/agents/provider_manager.rs` | `provider_name_for` / `model_config_for` | 会话字段优先、全局配置回退的两层解析 |

## 参考

- [goose 源码：agents/session_runtime.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/session_runtime.rs)
- [goose 源码：agents/provider_manager.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/agents/provider_manager.rs)
- [lru crate 文档](https://docs.rs/lru/latest/lru/)
- [goose 文档：会话管理](https://block.github.io/goose/docs/guides/sessions/)

## 与主核 / AI 的关系

「常驻集 + 附件 + 每会话槽位」这套结构与主核里的「缓冲池 + 会话句柄 + 事务上下文」几乎逐项对应：缓冲池限制同时活跃的页数并做 LRU 淘汰，会话句柄记录谁在用它，事务上下文保存这次操作的状态。主核最怕的也是同一件事——**淘汰一个正在被用的对象**，所以两者都需要「忙则豁免」这条规则。

对 AI 工程的实际意义有三条。第一，**淘汰必须能识别「忙」**。这里用两个判据合并（确认中 / 有活跃运行），并额外给了「再次被触碰」的二次校验。少了任何一条，都会出现「用户在等批准，而会话被淘汰」这种极难复现的故障。

第二，**「最后一个附件」比「所有附件」更有用**。多客户端场景里，把出站消息发给「最近打开的那个」是一个既简单又符合直觉的规则，而实现它需要的只是一个有序 map 加一次 `shift_remove` + `insert`。这类「用一个数据结构的选择代替一段路由逻辑」的做法很值得借鉴。

第三，**共享资源的失效可以靠代次而非通知**。`(generation, instance)` 的模式让缓存不需要任何 invalidation 调用——只要读取代次时对比，过期的自然被替换。代价是每次取用多一次读配置，收益是「不会有谁忘记发失效通知」。这一条在主核里也能用：配置版本号一变，派生物自然作废。
