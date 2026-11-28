---
project: open-interpreter
feature: approvals
title: 审批档案与风险判定
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/protocol/src/approvals.rs
    symbols: [ResolvedPermissionProfile, EscalationPermissions, ExecPolicyAmendment]
  - path: codex-rs/protocol/src/approvals.rs
    symbols: [GuardianRiskLevel, NetworkApprovalContext, NetworkPolicyRuleAction]
---

# 审批档案与风险判定

## 一句话职责

`approvals.rs` 定义「一次审批长什么样」以及「它能被升级成什么」：`ExecPolicyAmendment` 与 `NetworkApprovalContext` 描述「用户批准时顺手能记住什么」，`EscalationPermissions` 区分「把权限合并进当前轮次」还是「整份替换」（`ResolvedPermissionProfile`），`GuardianRiskLevel` 与网络规则动作则是这套档案里的风险刻度与放行/拒绝词汇。

## 机制是什么

审批这件事在实现上有两种形态，而这一层把它们都建模了：

| 形态 | 由谁决定 | 数据形态 |
|---|---|---|
| **人审** | 用户点「允许 / 拒绝」 | `ReviewDecision` 一族 |
| **机审** | 一个分类模型（Guardian）给出风险分 | `GuardianAssessmentEvent` + `GuardianRiskLevel` |

两者最终都要落到「这条命令跑不跑」，所以它们的产出词汇被刻意对齐：人审有 `Approved` / `Denied` / `Abort`，机审有 `Allow` / `Deny` —— 而 `GuardianAssessmentOutcome` 就是 `Allow | Deny` 这个最小集合。

**批准不只是「执行这一次」。** 这是这一层最关键的一点：允许之后还有「要不要记住」这个维度，而记住的粒度有三种：

1. **本会话**：`ReviewDecision::ApprovedForSession` —— 同一会话内同类的后续请求自动批准。
2. **写回策略文件**：`ExecPolicyAmendment` —— 把这条命令的前缀固化成一条 `prefix_rule(decision="allow")`，**跨会话生效**（见 b03 的 `blocking_append_allow_prefix_rule`）。
3. **写回网络策略**：`NetworkPolicyAmendment` —— 对同一主机记一条 allow/deny。

这三种粒度的存在，决定了「审批请求」必须携带一组**提议**（proposed）：客户端拿着这些提议去渲染不同的按钮，用户选哪一个，就执行对应的动作。于是 `ExecApprovalRequestEvent` 里出现了 `proposed_execpolicy_amendment`、`proposed_network_policy_amendments`、`additional_permissions` 这些字段——**它们是「可选按钮」的数据源**，而不是审批本身的参数。

**权限升级有两种语义，而它们的区别是「合并」与「替换」。**

```
pub enum EscalationPermissions {
    /// Permissions to merge with the active turn permissions.
    AdditionalPermissionProfile(AdditionalPermissionProfile),
    /// Fully resolved permissions that should replace the active turn permissions.
    ResolvedPermissionProfile(ResolvedPermissionProfile),
}
```

两种都叫「升级」，但一个是在现有权限上**加**，一个是把现有权限**换掉**。这个区分不是形式主义：同一个「需要写某个目录」的请求，若走合并，用户原有的、更宽的限制仍然有效；若走替换，那份宽限制就被这份新的收窄了。搞混的后果是权限要么意外收紧（用户莫名其妙地被拒）要么意外放开。

`ResolvedPermissionProfile` 的定义极简，只有一个字段：

```
pub struct ResolvedPermissionProfile { pub permission_profile: PermissionProfile }
```

它是**一个单字段新类型**，而不是直接用 `PermissionProfile`。这个包装不是为了字段，而是为了在枚举里可分辨：`EscalationPermissions::AdditionalPermissionProfile(AdditionalPermissionProfile)` 与 `EscalationPermissions::ResolvedPermissionProfile(ResolvedPermissionProfile)` 若用裸类型，两者的载荷在类型上可能无法区分（或至少读起来无法区分）。用一个专门的类型名，让「这是替换用的」这件事在类型上一目了然。

`#[allow(clippy::large_enum_variant)]` 挂在 `EscalationPermissions` 上说明两个变体的尺寸差距很大——`PermissionProfile` 显然比 `AdditionalPermissionProfile` 大得多。选择「允许大变体」而不是「装箱最小化」，是因为这些值随审批请求流动、数量极少，为几十个字节引入一次堆分配不值得。

## 怎么实现的

**`ExecPolicyAmendment` 在线上是一个裸数组，而不是对象。**

```
#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq, JsonSchema, TS)]
#[serde(transparent)]
#[ts(type = "Array<string>")]
pub struct ExecPolicyAmendment { pub command: Vec<String> }
```

`#[serde(transparent)]` 让它在 JSON 里序列化成 `["git", "push"]` 而不是 `{"command": [...]}`；`#[ts(type = "Array<string>")]` 让生成的 TypeScript 类型也是 `string[]`。两处必须同时声明，因为 serde 与 ts-rs 是两套独立的生成器——只改一处就会出现「Rust 侧是数组、TS 侧是对象」的不一致。

这个选择的实际好处：`ExecPolicyAmendment` 与「前缀」这个概念在语义上就是同一个东西（一串 token），去掉包装层让 wire 更简单，也让「前缀 → 规则行」的转换少一层解包。代价是将来若想加一个字段（比如「这个改动的来源」），就必须放弃 `transparent` 并破坏线上形状。

结构体上的注释把用途写全了：

> The `command` tokens form the prefix that would be added as an execpolicy `prefix_rule(..., decision="allow")`, letting the agent bypass approval for commands that start with this token sequence.

注意「the agent」这个词——这份提议是给**智能体**看的（它可以在被拒后建议用户「要不要允许这一类命令」），而不只是给界面用的。

`new` 与 `command()` 两个方法加上 `From<Vec<String>>`，让它既能显式构造也能从 Vec 自然转换。

**网络侧的档案是两个小类型加一个协议枚举。**

```
pub struct NetworkApprovalContext { pub host: String, pub protocol: NetworkApprovalProtocol }
pub enum NetworkPolicyRuleAction { Allow, Deny }
```

`NetworkApprovalContext` 只带「主机 + 协议」，不带端口、路径、方法——也就是说批准的对象是**「对某主机的某类协议」**这个粒度，而不是具体的 URL。这与 b01 里网络规则的主机归一化一致：规则本身就是按主机建立的，审批也按主机粒度记忆。

`NetworkApprovalProtocol` 有兼容细节：

```
pub enum NetworkApprovalProtocol {
    // TODO(viyatb): Add websocket protocol variants when managed proxy policy
    // decisions expose websocket traffic as a distinct approval context.
    Http,
    #[serde(alias = "https_connect", alias = "http-connect")]
    Https,
    Socks5Tcp,
    Socks5Udp,
}
```

`Https` 有两个 serde 别名（`https_connect`、`http-connect`），与 b01 里 `NetworkRuleProtocol::parse` 接受的三种拼法对齐。前者是 wire 层的向后兼容，后者是策略文件里的书写宽容——同一个概念在两条入口上都要宽容。

那条 TODO 注释也值得记：它明确说「等托管代理策略开始把 websocket 流量作为独立审批语境时再加变体」。这是一个**有意的缺口**：目前 websocket 与 https 共用审批语境，作者知道这是个近似。

**`GuardianRiskLevel` 四档，且序列化成小写。**

```
#[serde(rename_all = "lowercase")]
pub enum GuardianRiskLevel { Low, Medium, High, Critical }
```

四档而不是三档，且 `Critical` 与 `High` 分开——说明这套分级的用途不只是「拦不拦」，还包括「怎么呈现」。`GuardianAssessmentEvent` 里 `risk_level: Option<GuardianRiskLevel>` 的注释是「粗糙的风险标签。评估进行中时省略」——也就是说它是**结果字段**，评估未完成时不存在，而不是一个默认值。

同一文件里还有 `GuardianUserAuthorization`（`Unknown` / `Low` / `Medium` / `High`）——另一个四档枚举，含义是「对话记录有多直接地授权了这次操作」。这两个维度是**正交**的：一个操作可以「风险高」但「用户明确要求过」（于是允许），也可以「风险低」但「用户从未提及」（于是要问）。把它们分成两个枚举而不是合成一个分数，让「为什么需要这次审批」可解释。

`GuardianReviewReason` 把「为什么要审」列成十一种，且最后带 `#[serde(other)] Unknown`：

```
pub enum GuardianReviewReason {
    Policy, FreshRequired, MissingScore, StaleScore, InvalidScore,
    IncompatibleCompaction, ElevatedRisk, ScoringFailure, AuthorizationChanged,
    #[serde(other)] Unknown,
}
```

`#[serde(other)]` 让**未知的**取值反序列化成 `Unknown` 而不是解析失败。这是进化兼容的标准做法：发送方（可能是更老的客户端或更新的服务端）加了新原因，接收方仍能处理整个事件，只是把原因归为未知。注释也说明 `review_reason` 在「更老客户端记录的事件里」缺失，所以字段是 `Option` + `default`。

**`GuardianAssessmentEvent` 是「一次机审的生命周期」。**

它的字段分成四组：

| 组 | 字段 |
|---|---|
| 身份 | `id`、`target_item_id`、`turn_id` |
| 时间 | `started_at_ms`、`completed_at_ms` |
| 结论 | `status`、`risk_level`、`user_authorization`、`rationale`、`decision_source` |
| 被审对象 | `action: GuardianAssessmentAction` |

`status` 五态（`InProgress` / `Approved` / `Denied` / `TimedOut` / `Aborted`）说明机审是**异步的**：它可能超时、可能被中止。这正是「机审」与「人审」在实现上的根本差别——人可以无限期地看，模型不能。

`GuardianAssessmentAction` 是一个带 `tag = "type"` 的七个变体枚举，覆盖它能审的所有动作：

```
Command { source, command, cwd }
Execve { source, program, argv, cwd }
WriteStdin { approval_id, process_id, stdin, cwd }
ApplyPatch { cwd, files }
NetworkAccess { target, host, protocol, port }
McpToolCall { server, tool_name, connector_id, connector_name, tool_title }
RequestPermissions { reason, permissions }
```

它比「命令审批」宽得多——这也印证了「审批不只是 shell 命令」这件事：写 stdin 到已有进程、打补丁、网络访问、调 MCP 工具、申请权限，全都在同一个模型下被审。`WriteStdin` 那段注释特别说明了 `cwd` 的语义：「这是该终端的**启动目录**，不是它的当前工作目录」——一个容易搞错的细节，因为终端可以 `cd` 过。

**`ExecApprovalRequestEvent` 用两个「有效值」方法把「显式给」与「按旧逻辑推导」分开。**

```
impl ExecApprovalRequestEvent {
    pub fn effective_approval_id(&self) -> String {
        self.approval_id.clone().unwrap_or_else(|| self.call_id.clone())
    }
}
```

注释写明了 `approval_id` 何时存在：「缺失时，审批针对命令条目本身（`call_id`）；存在时用于子命令审批（通过 execve 拦截）与 stdin 写入」。也就是说审批的粒度**可以是整条命令，也可以是某一次子命令调用**——后者靠 execve 拦截得到，前者是常态。

```
pub fn effective_available_decisions(&self) -> Vec<ReviewDecision> {
    match &self.available_decisions {
        Some(decisions) => decisions.clone(),
        None => Self::default_available_decisions(
            self.network_approval_context.as_ref(),
            self.proposed_execpolicy_amendment.as_ref(),
            self.proposed_network_policy_amendments.as_deref(),
            self.additional_permissions.as_ref(),
        ),
    }
}
```

注释：「`available_decisions` 是新字段，老发送方可能不填，所以不在时回退到旧逻辑」。这是**双向兼容**的写法：新发送方显式给出决策集合（可以完全自定义），老发送方不给则用推导规则。接收方对两种情况都能正确渲染。

**推导规则分三支，每支的按钮集合不同。**

```
pub fn default_available_decisions(
    network_approval_context: Option<&NetworkApprovalContext>,
    proposed_execpolicy_amendment: Option<&ExecPolicyAmendment>,
    proposed_network_policy_amendments: Option<&[NetworkPolicyAmendment]>,
    additional_permissions: Option<&AdditionalPermissionProfile>,
) -> Vec<ReviewDecision> {
```

- **有网络语境** → `[Approved, ApprovedForSession, (NetworkPolicyAmendment 若提议里有 Allow), Abort]`。注意它先推 `Approved` 与 `ApprovedForSession`，再**条件性**追加「记住这个主机」，最后 `Abort`。
- **有额外权限** → `[Approved, Abort]`。这一支最短，因为「申请更多权限」没有「记住」这种语义——权限升级要么这次给，要么不给。
- **其余（普通命令）** → `[Approved, (ApprovedExecpolicyAmendment 若有提议), Abort]`。

三支的共同点是：**总是以 `Approved` 开头、以 `Abort` 结尾**，中间的「记住」项才是差异。这个顺序是有意义的——它是界面上按钮的顺序，而「允许」在最前、「中止」在最后是常见布局。

一个细节：网络那一支推的是 `ApprovedForSession`（会话级记住），而命令那一支推的是 `ApprovedExecpolicyAmendment`（写回策略文件）。两者都是「下次不用问」，但**作用域不同**：网络是会话级的内存缓存，命令是持久化的文件改动。为什么不对称？大概因为网络访问的会话级缓存在 b03 的 `compiled_network_domains` 里已有支撑（每个会话可以各自编译一份），而把网络规则写进文件的风险更高（它会影响所有未来的会话）。

`NetworkPolicyAmendment` 本身只有两个字段：

```
pub struct NetworkPolicyAmendment { pub host: String, pub action: NetworkPolicyRuleAction }
```

而推导规则里只挑 `action == Allow` 的那条：

```
amendments.iter().find(|amendment| amendment.action == NetworkPolicyRuleAction::Allow)
```

于是**「记住并拒绝」不会出现在按钮里**。这是一个明确的取舍：批准时只提供「记住允许」，因为「记住拒绝」会让用户永久地把自己需要的主机拉黑——而这个后果比「永久允许」更难察觉（症状是「某天开始再也连不上」）。拒绝的持久化应当走显式的策略编辑，而不是审批弹窗里的一个按钮。

**`ReviewDecision` 是一份完整的回答词汇，且默认是拒绝。**

```
pub enum ReviewDecision {
    Approved,
    ApprovedExecpolicyAmendment { proposed_execpolicy_amendment: ExecPolicyAmendment },
    ApprovedForSession,
    ApprovedMcpPolicyAmendment,
    NetworkPolicyAmendment { network_policy_amendment: NetworkPolicyAmendment },
    Denied { rejection: String },
    TimedOut,
    Abort,
}
impl Default for ReviewDecision {
    fn default() -> Self {
        Self::Denied { rejection: "denied".to_string() }
    }
}
```

`Default` 是 `Denied` 而不是 `Approved`——与 `OnFailure`（goose 里默认 `Allow`）恰好相反，这里的方向是安全优先：**任何意外的默认值都是拒绝**。

`Denied` 带一个 `rejection: String`（给人看的理由），`Abort` 则是「停下，等用户下一条指令」（注释：「用户拒绝并且智能体不应做任何事，直到用户的下一条命令」）。这两个的区分很重要：`Denied` 意味着「换个方法继续」，`Abort` 意味着「整轮终止」。

`to_opaque_string` 的注释透露了一个真实约束：

> Returns an opaque version of the decision without PII. We can't use an ignored flag on `serde` because the serialization is required by some surfaces.

也就是说：既需要一个**带理由的完整序列化**（某些界面要用），又需要一个**不含敏感内容的版本**（遥测/日志用），所以只能提供两个方法而不是靠 `serde(skip)`。

## 关键取舍

**源码事实：** `EscalationPermissions` 两个变体的语义是「合并进当前轮次权限」与「整份替换当前轮次权限」，并 `#[allow(clippy::large_enum_variant)]` 选择不做装箱。

**源码事实：** `ResolvedPermissionProfile` 是单字段新类型，作用是在枚举里让「替换用的那份」在类型上可分辨。

**源码事实：** `ExecPolicyAmendment` 用 `#[serde(transparent)]` + `#[ts(type = "Array<string>")]` 在线上是裸字符串数组；两处声明必须同时改，否则 Rust 与 TS 会不一致。

**源码事实：** `NetworkApprovalProtocol` 的 `Https` 有两个 serde 别名，与策略文件解析接受的三种拼法对齐；websocket 目前与 https 共用语境，源码里有 TODO 说明这是已知近似。

**源码事实：** `GuardianRiskLevel` 四档且小写序列化；它与 `GuardianUserAuthorization` 是两个正交维度（风险高低与「用户是否明确授权」），不合成一个分数。

**源码事实：** `GuardianReviewReason` 带 `#[serde(other)] Unknown`，未知原因不导致解析失败；`review_reason` 在旧事件里缺失所以是 `Option`。

**源码事实：** `GuardianAssessmentEvent` 的 `status` 五态包含 `TimedOut` 与 `Aborted`，说明机审是异步且有界的；`risk_level` 在评估进行中被省略。

**源码事实：** `GuardianAssessmentAction` 有七个变体，覆盖命令、execve、写 stdin、打补丁、网络访问、MCP 工具调用、权限申请——审批的对象远不止 shell 命令。

**源码事实：** `effective_approval_id` 在 `approval_id` 缺失时退回 `call_id`；`effective_available_decisions` 在字段缺失时按三支规则推导（网络 / 额外权限 / 普通命令）。

**源码事实：** 推导规则里网络支只挑 `action == Allow` 的提议进按钮；命令支推 `ApprovedExecpolicyAmendment`（持久化），网络支推 `ApprovedForSession`（会话级）。

**源码事实：** `ReviewDecision` 的 `Default` 是 `Denied`；`Denied` 与 `Abort` 语义不同（换方法继续 vs 整轮终止）。

**架构推论：** 「审批请求携带提议、客户端据此渲染按钮」这个形状把「用户能选什么」下放到了发送方。好处是服务端可以按上下文决定「这次值不值得提供『记住』选项」；代价是接收方必须处理任意集合，且要做到「老发送方不填时能推出合理默认」。

**架构推论：** `Denied` 默认 + 只推「记住允许」不推「记住拒绝」，两条都指向同一个方向：审批界面的默认行为必须是最不容易造成长期损害的那一个。永久放开某条命令可以撤销（删掉策略文件里那一行），而永久拉黑某个主机在症状上更像「网络坏了」。

**架构推论：** 机审与人审共用词汇（`Allow`/`Deny` vs `Approved`/`Denied`），但保留各自的信封（`GuardianAssessmentEvent` vs `ExecApprovalRequestEvent`）。这样「谁做的决定」在数据里始终可查——如果两者合并成一种事件，事后就无法区分「用户批准了」与「模型判了低风险」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/protocol/src/approvals.rs` | `ResolvedPermissionProfile` | 单字段新类型：一份「替换用」的完整权限档案 |
| `codex-rs/protocol/src/approvals.rs` | `EscalationPermissions` | 权限升级两态：合并 vs 替换 |
| `codex-rs/protocol/src/approvals.rs` | `ExecPolicyAmendment` | 线上是裸字符串数组的命令前缀提议，可写回 `prefix_rule` |
| `codex-rs/protocol/src/approvals.rs` | `NetworkApprovalContext` | 「主机 + 协议」粒度的网络审批语境 |
| `codex-rs/protocol/src/approvals.rs` | `NetworkPolicyRuleAction` | 网络规则的 allow / deny 两态 |
| `codex-rs/protocol/src/approvals.rs` | `GuardianRiskLevel` | 四档风险标签，评估进行中省略 |
| `codex-rs/protocol/src/approvals.rs` | `GuardianAssessmentEvent` / `GuardianAssessmentAction` | 机审的生命周期与七类被审动作 |
| `codex-rs/protocol/src/approvals.rs` | `ExecApprovalRequestEvent` | 一次审批请求：语境、提议、可用决策 |
| `codex-rs/protocol/src/approvals.rs` | `effective_approval_id` / `effective_available_decisions` | 「显式优先、缺失时推导」的两个有效值 |
| `codex-rs/protocol/src/protocol.rs` | `ReviewDecision` | 八态的审批回答词汇，`Default` 为 `Denied` |

## 参考

- [open-interpreter 源码：protocol/src/approvals.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/protocol/src/approvals.rs)
- [open-interpreter 文档：沙箱与审批](https://www.openinterpreter.com/docs/terminal/sandbox)
- [serde 文档：`transparent` 与 `other`](https://serde.rs/enum-representations.html)
- [OWASP：LLM 应用中的过度代理](https://genai.owasp.org/llmrisk/llm062024-excessive-agency/)

## 与主核 / AI 的关系

「审批」在主核里对应的是「危险操作需要人工确认」这条路径：删表、改配置、执行维护脚本。这里有两个可以直接搬的结构。

第一，**「执行这一次」与「记住这个决定」是两个维度，必须分开建模**。把二者合成一个布尔（`allow` / `deny`）会丢掉最有用的一档——「这次可以，以后也这样」。这里的做法是「请求携带提议 + 回答携带作用域」：`ExecPolicyAmendment` 与 `NetworkPolicyAmendment` 分别是两个不同文件的写回，`ApprovedForSession` 是内存缓存。三档作用域各不相同，而数据形态让它们各自可辨。

第二，**权限升级要区分「合并」与「替换」**。这是最容易埋雷的一处：`EscalationPermissions` 的两个变体看起来都是「给更多权限」，但一个是在现有档案上加，一个是换掉。主核里若要做「临时提权」，同样必须问清楚是「在原有限制上追加」还是「本次换一份」——搞混会让用户以为自己仍然受原限制保护，而实际上没有。

第三，**默认值必须是拒绝**。`ReviewDecision::default()` 是 `Denied`。任何「默认放行」的设计在遇到未覆盖的分支时都会变成静默提权，而这类 bug 的发现往往在一次事故之后。

还有一条更细但很实用：**「未知枚举值不导致解析失败」**。`GuardianReviewReason` 的 `#[serde(other)] Unknown` 让新旧两端能继续对话。主核在跨版本传递状态（尤其日志与审计记录）时应该采用同一策略：宁可把无法识别的原因归为 unknown，也不要在解析阶段丢弃整条记录。
