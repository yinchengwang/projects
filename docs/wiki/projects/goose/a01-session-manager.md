---
project: goose
feature: session-manager
title: 会话管理与用量聚合
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/session/session_manager.rs
    symbols: [SessionManager, Session, SessionType]
  - path: crates/goose/src/session/session_manager.rs
    symbols: [SessionUpdateBuilder, SessionUsageTotals, SessionInsights]
---

# 会话管理与用量聚合

## 一句话职责

`SessionManager` 把一个 SQLite 库（`sessions.db`，schema 版本 18）包成会话仓库：一条 `Session` 是一行记录加上一段 `Conversation`，所有写操作都走 `SessionUpdateBuilder` 的「只改指定列」补丁式更新，用量则另有 `usage_ledger` 台账按差值逐笔记录。

## 机制是什么

这里是两套存储，职责不同：

- **`sessions` 表**：会话本身的快照列——身份、工作目录、类型、时间戳、扩展状态、provider 与模型、累计用量与累计成本等。一行就是一个会话的「当前状态」。
- **`usage_ledger` 表**：用量的**逐笔台账**。它不存总量，只存每次调用相对于「会话累计到当时为止」的增量。

为什么要把总量与台账分开？因为总量会被覆盖（每次更新都写最新累计值），而「这次到底花了多少」只能从差值里恢复。台账那一行是**差值算出来的**，不是调用方传进来的：

```
MAX(COALESCE(s.accumulated_total_tokens, 0) - l.total_sum, 0)
```

即「会话当前累计」减「同一会话已入账之和」，并用 `MAX(…, 0)` 夹住——一个累计值被回退（例如重试时对话被重置）不会产生负的用量。

成本那一列还多一层判断：「累计成本为空 或 不大于已入账之和时，记 `NULL` 而不是 0」。`NULL` 表示「这次没产生可归因的成本增量」，与「增量为 0」在语义上不同，而 `0` 会在下游聚合里被当成真实值。

## 怎么实现的

**`Session` 是平铺的一行，但字段来源分三类。**

```
pub struct Session {
    pub id: String,
    pub working_dir: PathBuf,
    #[serde(alias = "description")]
    pub name: String,
    #[serde(default)]
    pub user_set_name: bool,
    #[serde(default)]
    pub session_type: SessionType,
    ...
    pub conversation: Option<Conversation>,
    pub message_count: usize,
    ...
    pub usage: Usage,
    pub accumulated_usage: Usage,
    pub accumulated_cost: Option<f64>,
    ...
}
```

三类各有各的读法：

- **身份与归属**：`id`、`name`（带 `alias = "description"`，说明这个字段曾经叫别的名字，旧存档要能读进来）、`user_set_name`、`session_type`、`created_at` / `updated_at`、`project_id`、`parent_session_id`。
- **运行配置**：`working_dir`、`extension_data`、`provider_name`、`model_config`、`goose_mode`、`recipe` 与 `user_recipe_values`、`system_prompt_override` / `system_prompt_extras`、`container`。
- **账目**：`usage`（本轮）、`accumulated_usage`（会话累计）、`accumulated_cost`。

`conversation` 是可选字段，因为它不总是被读出来——`get_session(id, include_messages: bool)` 用这个布尔控制是否附带消息。docstring 层面的意义很直接：列表页要几百条会话的标题，不需要几百段对话。

`SessionType` 是七个变体的枚举，且**默认值是 `User`**：

```
pub enum SessionType {
    #[default] User,
    Scheduled,
    SubAgent,
    Hidden,
    Terminal,
    Gateway,
    Acp,
}
```

它同时挂了 `strum::Display` / `strum::EnumString`，且 `serde(rename_all = "snake_case")` + `strum(serialize_all = "snake_case")`。两套转换都指向同一个字符串形态，于是「存进 SQL 的类型名」与「序列化成 JSON 的类型名」一致——这一点是 `get_insights` 能做 `session_type IN (...)` 过滤的前提，因为它要拿 `t.to_string()` 作为绑定参数。

七种类型不是装饰：`SubAgent`、`Hidden`、`Terminal`、`Gateway`、`Acp` 都会被某些查询排除或包含，而 `get_insights` 只统计 `User` 与 `Scheduled` 两种。也就是说「用户会当作自己的会话」这个判断，是用类型枚举表达的策略，而不是一个布尔字段。

**`SessionUpdateBuilder` 用 `Option<Option<T>>` 表达三种意图。**

```
pub struct SessionUpdateBuilder<'a> {
    session_manager: &'a SessionManager,
    session_id: String,
    name: Option<String>,
    ...
    accumulated_cost: Option<Option<f64>>,
    schedule_id: Option<Option<String>>,
    recipe: Option<Option<Recipe>>,
    provider_name: Option<Option<String>>,
    model_config: Option<Option<ModelConfig>>,
    archived_at: Option<Option<DateTime<Utc>>>,
    ...
}
```

这里有两层可选性，各层含义不同：

| 外层 | 内层 | 含义 |
|---|---|---|
| `None` | — | 这次更新不动这一列 |
| `Some(None)` | 空 | 把这一列置为 SQL NULL |
| `Some(Some(v))` | 有值 | 把这一列设为 v |

单层 `Option<T>` 无法区分前两者，于是「清空某个字段」与「不改某个字段」会混成一个值。这个模式在字段层面重复了七次，每次都是一个「可能被清空的列」。

它的 builder 方法也按意图分了叉。命名那一组有两个入口，而不是一个：

```
pub fn user_provided_name(mut self, name: impl Into<String>) -> Self {
    let name = name.into().trim().to_string();
    if !name.is_empty() {
        self.name = Some(name);
        self.user_set_name = Some(true);
    }
    self
}

pub fn system_generated_name(mut self, name: impl Into<String>) -> Self {
    ...
    self.user_set_name = Some(false);
    ...
}
```

两个方法都顺手设置了 `user_set_name`，而且是相反的值。这叫「把不变量写进 setter」：一旦名字来源变化，`user_set_name` 必然跟着对，调用方不可能设错。空名字被**静默跳过**（`if !name.is_empty()`）也是同一类处理——一个空白名字会毁掉列表页，而 builder 不该让它落地。

`apply()` 只有一行，把自身交给内部实现：

```
pub async fn apply(self) -> Result<()> { self.session_manager.apply_update_inner(self).await }
```

`SessionUpdateBuilder` 的所有字段都是私有的，构造只能从 `SessionManager::update(id)` 出来，`apply` 是唯一出口——这保证了「补丁」这个语义不会被绕过。

**两个辅助函数把「结算视图」的组装从存储层挪出来。**

```
pub fn token_state_from_session_and_totals(session: &Session, totals: &SessionUsageTotals) -> TokenState {
    TokenState {
        input_tokens: session.usage.input_tokens.unwrap_or(0),
        ...
        accumulated_input_tokens: totals.accumulated_usage.input_tokens.unwrap_or(0),
        ...
        accumulated_cost: totals.accumulated_cost,
    }
}
```

读法很值得注意：**本轮字段来自 `session.usage`，累计字段来自 `totals`**。也就是说「本轮」与「累计」两个来源不同——本轮就在会话记录里，累计要从台账重新聚合。旁边还有一个 `impl From<&Session> for TokenState`，它一律取 `session` 里的字段（本轮与 `session.accumulated_usage`）——这是「没有台账时的降级视图」。

两个函数并存，回答的是两个不同的问题：那个 `From` 实现回答「这条记录里写着什么」，而 `token_state_from_session_and_totals` 回答「加上台账后到底花了多少」。前者是快照，后者是核算。

`unwrap_or(0)` 也是刻意的：`Usage` 的每个字段都是 `Option`，因为不同的 provider 报的字段不同。缺失视为 0 而不是错误——一个不报缓存 token 的 provider 不该让整个统计失败。

**`SessionUsageTotals` 与 `SessionInsights` 是两个粒度的聚合。**

```
pub struct SessionUsageTotals { pub accumulated_usage: Usage, pub accumulated_cost: Option<f64> }

pub struct SessionInsights { pub total_sessions: usize, pub total_tokens: i64 }
```

前者是**单会话**的累计（从台账求和），后者是**全局**的概览。后者更值得注意的是它统计的口径：

```
SELECT COUNT(*) as total_sessions,
       COALESCE(SUM(COALESCE(accumulated_total_tokens, total_tokens, 0)), 0) as total_tokens
FROM sessions WHERE session_type IN ({})
```

三层 `COALESCE` 的次序是有意义的：优先用**累计总量**，缺失则退回**本轮总量**，都没有就是 0。前两层是列级的回退，第三层是求和函数的空集回退（`SUM` 在零行时返回 `NULL`）。

而类型过滤由调用方给：`SessionManager::get_insights()` 固定传 `&[SessionType::User, SessionType::Scheduled]`。于是「统计」这个动作天然排除了子智能体、隐藏会话、ACP 会话——它们的 token 属于某个主会话的开销，重复计入会让总数虚高。

`types.is_empty()` 的早期返回（总数 0）不是防御性代码，而是必要代码：空列表会生成 `IN` 的空括号集合，SQL 直接语法错误。用 `types.iter().map(|_| "?").join(", ")` 生成占位符再逐个 `bind`，也是为了避免把类型名拼进 SQL——不过这里 `session_type` 的值来自枚举的 `to_string()`，本来就不可能注入。

**查询是分批的。**文件里有一个 `const SESSION_COUNT_BATCH_SIZE: usize = 900;`。900 这个数字是 SQLite 的 999 个参数上限减掉一些余量——凡是要按 id 集合查会话的地方（`get_session` 的批量版、按 id 列表取消息），都会按这个尺寸切段。它说明 `SessionManager` 的接口有意支持「一次查几十上百个 id」，而不是只做单条查询。

**数据库位置是进程级的单例。**

```
static SESSION_STORAGE: LazyLock<Arc<SessionStorage>> =
    LazyLock::new(|| Arc::new(SessionStorage::new(Paths::data_dir())));

pub fn instance() -> Self { ... }
```

`SessionManager::new(data_dir)` 允许测试与嵌入场景用临时目录，而 `instance()` 是应用内共享的那一个。`SessionManager::storage()` 暴露 `&Arc<SessionStorage>`，说明上层有时需要绕过门面直接用存储层（比如需要更细的查询）。

**`record_usage_metrics` 的事务形态。**

```
let mut tx = pool.begin_with("BEGIN IMMEDIATE").await?;
```

`BEGIN IMMEDIATE` 而不是默认的延迟事务：这条语句要读「已入账之和」再写新行，两步之间若有别的写入插进来就会算错差值。SQLite 的 `IMMEDIATE` 在事务开始时就取写锁，把「读-算-写」变成一个不可分割的动作。这是「差值是算出来的」这条设计对事务的硬要求。

## 关键取舍

**源码事实：** 用量分两张表：`sessions` 里存累计快照，`usage_ledger` 里按**差值**记逐笔增量；成本列在「累计为空或不大于已入账之和」时记 `NULL` 而非 0，`NULL` 与 0 语义不同。

**源码事实：** 差值写入用 `BEGIN IMMEDIATE`，因为「读已入账之和」与「写新行」必须不可分割。

**源码事实：** `SessionUpdateBuilder` 在七个可清空字段上用 `Option<Option<T>>` 表达「不动 / 置空 / 设值」三态；builder 字段全部私有，`apply()` 是唯一出口。

**源码事实：** 命名有两个入口（`user_provided_name` / `system_generated_name`），各自把 `user_set_name` 设成相反值；空白名字被静默跳过。

**源码事实：** `TokenState` 有两个构造路径：一个从会话记录取全部字段，一个「本轮取会话、累计取台账」。前者是快照视图，后者是核算视图。

**源码事实：** `get_insights` 只统计 `User` 与 `Scheduled` 两类，且 `session_type` 走 `to_string()` 绑定参数；`types` 为空时直接返回零而不是发出空 `IN` 查询。

**源码事实：** `SESSION_COUNT_BATCH_SIZE = 900` 是 SQLite 参数上限下的批量尺寸。

**架构推论：** 「快照列 + 台账」是同一份事实的两种表示，各有各的用途：快照让读一条会话变成一次单行查询（列表页需要），台账让「每一笔花了多少」可恢复（成本报表需要）。只有快照则历史不可追，只有台账则每次读都要聚合。

**架构推论：** builder 的私有字段 + 单一出口，使「补丁式更新」成为可保证的语义。若允许构造出任意字段组合，一次更新可能意外覆盖掉未指定的列，而那种错误在测试里极难发现——它只在「两个功能先后改同一条记录」时才暴露。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/session/session_manager.rs` | `SessionManager` | 会话仓库门面：建/读/改/删、导出导入、名称生成、用量记录 |
| `crates/goose/src/session/session_manager.rs` | `Session` | 一条会话记录：身份、运行配置、账目三组字段 |
| `crates/goose/src/session/session_manager.rs` | `SessionType` | 七种会话类型，默认 `User`；`get_insights` 按它过滤 |
| `crates/goose/src/session/session_manager.rs` | `SessionUpdateBuilder` | 补丁式更新器，`Option<Option<T>>` 三态 + 私有字段 + 单一 `apply` |
| `crates/goose/src/session/session_manager.rs` | `SessionUsageTotals` | 单会话累计（来自台账求和） |
| `crates/goose/src/session/session_manager.rs` | `SessionInsights` | 全局概览：会话数与三层 `COALESCE` 的 token 总数 |
| `crates/goose/src/session/session_manager.rs` | `token_state_from_session_and_totals` | 核算视图：本轮取会话、累计取台账 |

## 参考

- [goose 源码：session/session_manager.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/session/session_manager.rs)
- [goose 文档：会话与历史](https://block.github.io/goose/docs/guides/sessions/)
- [SQLite 文档：事务与 `BEGIN IMMEDIATE`](https://www.sqlite.org/lang_transaction.html)
- [sqlx 文档：SQLite 与查询绑定](https://docs.rs/sqlx/latest/sqlx/sqlite/index.html)

## 与主核 / AI 的关系

「快照列 + 台账」这个双表示，与主核里「表的当前行 + WAL/undo 日志」是同构的：快照让读一条记录变成一次寻址，日志让「发生了什么」可重放、可审计。主核的日志是引擎内部的，这里把台账做成了第一等的业务表——因为「这次花了多少钱」本身就是产品要展示的数据，不是实现细节。

对 AI 工程的实际意义有三条。

第一，**用差值而不是累加计数来记账**。`record_usage_metrics` 不接收「本次用量」，而是从「当前累计 - 已入账之和」算出来。这样调用方不可能记错：它只负责把累计值刷新，入账是幂等可推的。反过来若让调用方传增量，一次重试或一次重复调用就会双记，而事后无法分辨。

第二，**`NULL` 与 `0` 必须分开**。成本列用 `NULL` 表示「这次没有可归因的成本增量」，用 `0` 表示「真的是零」。合并两者会让聚合出现两种错误：把「不知道」当成「没花钱」，或把「没花钱」当成「数据缺失」。

第三，**`Option<Option<T>>` 是「可能被清空的列」的正确编码**。只要一个更新接口既支持「不指定」又支持「置空」，单层 `Option` 就不够用。这个模式看起来很笨重，但它把一个静默的数据损坏问题变成了一处类型层面的显式表达。
