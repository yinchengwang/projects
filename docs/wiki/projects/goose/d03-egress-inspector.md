---
project: goose
feature: egress-inspector
title: 出口方向审查
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/security/egress_inspector.rs
    symbols: [EgressInspector, EgressDirection, EgressDestination]
  - path: crates/goose/src/security/egress_inspector.rs
    symbols: [detect_direction, extract_destinations, extract_text_for_inspection]
---

# 出口方向审查

## 一句话职责

检查 shell 命令与网络工具调用会把数据送到哪里、方向是出还是进，然后把每个目标地记进结构化日志——但它的动作恒为 `Allow`，`confidence` 恒为 `0.0`。它是一个**观测器**，不是拦截器。

## 机制是什么

这是 goose 检查链里最特殊的一个审查器，值得先把它的定位说清：

| 审查器 | 会不会拦 | 生产什么 |
|---|---|---|
| 安全（d01） | 会（`RequireApproval`） | 恶意判定与可引用的 finding |
| 权限（c01/c02） | 会（`Deny` / `RequireApproval`） | 批准与缓存 |
| 对手（d04） | 会（`Deny`） | 模型复核结论 |
| **出口（本页）** | **不会** | **目标地址、域名、类别、方向** |

它的价值全部在「记下来」这件事上：谁在往哪儿发数据。这类信息在事后排查时极有价值（「这个 agent 到底把东西发到哪些域名了」），而在事中拦截的价值有限——因为合法的开发操作本来就要访问网络（`git push`、`npm publish`、`docker push` 都是出站）。

所以它做三件技术活：

1. **从命令文本里抽目标地**（九类模式）
2. **判断方向**（出 / 进 / 未知）
3. **选出该审查的文本**（不同工具的参数键不同）

## 怎么实现的

**只审两类工具，且两者用不同的参数键。**

```rust
fn is_shell_tool(name: &str) -> bool {
    matches!(name, "shell" | "bash" | "execute_command" | "run_command" | "terminal")
        || name.ends_with("__shell") || name.ends_with("__bash") || name.ends_with("__terminal")
}
fn is_web_tool(name: &str) -> bool {
    matches!(name, "web_fetch" | "fetch" | "browser_navigate" | "http_request")
        || name.ends_with("__web_fetch") || name.ends_with("__fetch") || name.ends_with("__browser_navigate")
}
```

两个函数都用了「精确名单 + 后缀匹配」的组合。后缀那半句是为了覆盖 MCP 工具常见的 `extension__tool` 命名（例如 `developer__shell`）——同一类工具在不同扩展里前缀不同，但尾部约定相对稳定。

注意两者的**不对称**：shell 的后缀集合包含 `__terminal`，web 的包含 `__browser_navigate`；而精确名单里 shell 有五个名字、web 有四个。这不是穷举，而是「目前认识的形状」——认不出的工具会被静默跳过（`continue`）。

**该审的文本由工具类别决定键。** `extract_text_for_inspection` 只有一行核心逻辑：

```rust
let keys: &[&str] = if is_web { &["url", "uri", "endpoint"] } else { &["command", "cmd", "script", "input"] };
keys.iter().find_map(|k| args.get(*k).and_then(|v| v.as_str()).map(|s| s.to_string()))
```

即 web 工具看 `url` / `uri` / `endpoint`，shell 看 `command` / `cmd` / `script` / `input`，按顺序取**第一个字符串**。

这四种 shell 键名与多种工具实现习惯对应；多个候选是按「最可能」排序的。只取一个键意味着「一条命令里若同时有 `command` 与 `script`，只看 `command`」——在此用途下可接受（这里要的是观测，不是穷尽）。

**九类目标地抽取，每类都有自己的形状。** `extract_destinations` 逐类跑正则，全部用 `OnceLock<Regex>` 惰性编译（而不是 `LazyLock`），这是为了在每个函数内就地持有而不引入模块级静态。

| 类别 | 识别 | domain 怎么来 |
|---|---|---|
| `url` | `https?|ftp` 后面跟到空白/引号/分隔符 | 从 URL 里解析 host |
| `git_remote` | `git@host:path` | 直接取 `git@` 之后到冒号 |
| `s3_bucket` | `s3://bucket/path` | 拼成 `{bucket}.s3.amazonaws.com` |
| `gcs_bucket` | `gs://bucket/path` | 拼成 `{bucket}.storage.googleapis.com` |
| `scp_target` | `scp`/`rsync` 后到 `host:` | 直接取 host |
| `ssh_target` | `ssh` 后跳过选项与参数，取 host | 直接取 host |
| `docker_registry` | `docker push|login` 后的目标 | 取目标里第一个 `/` 之前 |
| `generic_network` | `fetch|nc|ncat|netcat|ftp|sftp|socat|httpie|xh` 后跟域名 | 取那个域名 |
| `package_publish` | 行首或 `;`/`&`/`|`/换行后的 `npm publish` / `cargo publish` | 固定为 `registry.npmjs.org` / `crates.io` |

三处值得细看：

**S3 与 GCS 把「bucket 名」翻译成「域名」。** `s3://my-bucket/path` 的真实目标是 `my-bucket.s3.amazonaws.com`。这一步不是形式主义——`domain` 字段的语义是「数据到达的主机」，而 bucket 名不是主机名。把两者对齐后才能与 `url` 类的 domain 放在同一个列表里比较。

**`scp` / `rsync` 的正则依赖「目标在最后」。** 它的模式是 `(?:scp|rsync)\s+.*?(?:\S+@)?([a-zA-Z0-9][\w.-]+):`——非贪婪匹配到第一个 `host:`。这在 `scp file host:/path` 上正确，但也会误捕 `scp -P 2222: ...` 这类不常见写法。这里选的是「覆盖主流用法」，而方向判定那一侧会再做一次基于「最后一个参数含冒号」的独立判断。

**`ssh` 的正则专门跳过选项。**

```rust
r"ssh\s+(?:-\w+\s+\S+\s+)*(?:\S+@)?([a-zA-Z0-9][\w.-]+)"
```

`(?:-\w+\s+\S+\s+)*` 这一段吃掉「选项 + 选项值」的成对出现（`-p 2222`、`-i keyfile`），这样 `ssh -p 2222 user@host` 里捕获的是 `host` 而不是 `2222`。然后还有一个额外守卫：

```rust
if !host.starts_with('-') { ... }
```

这是防「选项只有一个词」（如 `-v`）时正则把它当成 host 捕出来。

**`generic_network` 有一处跨类去重，且是这一段的精巧点。**

```rust
let already_seen: HashSet<String> = destinations.iter().map(|d| d.domain.to_lowercase()).collect();
for cap in generic_net_cmd_re.captures_iter(command) {
    if !already_seen.contains(&domain) { ... push generic_network ... }
}
```

即：**前面几类已经识别到的域名，不要再以 `generic_network` 重复记一遍**。`fetch https://api.example.com` 会先被 `url` 类捕获，再被 `generic_network` 类看到——若不去重，日志里同一目标会出现两条，一条精确（`url`）一条笼统（`generic_network`）。

注意它比对的是 `domain.to_lowercase()` 而收集侧也是 `to_lowercase()`，所以大小写差异不会漏掉去重。而**它自己的 `domain` 没有小写化就 push 了**——这是一处细微的不一致：去重集合是低层规范化的，但写入的值保持了原样。这不影响功能（去重已经生效），但会让日志里出现域名大小写不统一的记录。

**两条 publish 模式特意要求「行首或命令分隔符之后」。**

```rust
r"(?:^|[;&|]\s*|\n)\s*npm\s+publish(?:\s|$)"
```

这个前缀不是多余的：若只写 `npm publish`，那么 `echo "don't run npm publish"` 或注释里提到它都会命中。而在 shell 命令里，真正的调用必然出现在行首、`;` 之后、`&` 之后、`|`（管道）之后或换行之后。把「命令位置」写进正则，是这个模式集里唯一一处做了**语法层**（而非纯文本层）判断的地方。

**方向判定：先查明确命令，再看护栏，最后看 fallback。** `detect_direction` 的顺序值得逐段读，因为它决定了同名工具在不同用法下的归类：

```
git push / git remote add        -> Outbound
git clone / pull / fetch         -> Inbound
gh repo create / fork            -> Outbound
curl 带 -X POST/PUT/PATCH、--data、-d、-F、--form、--upload-file、-T  -> Outbound
wget 带 --post-data / --post-file / --body-data / --body-file        -> Outbound
npm/cargo publish、pip/twine upload、gem push  -> Outbound
docker push -> Outbound / docker pull -> Inbound
scp/rsync: 最后一个参数含冒号 -> Outbound，否则 Inbound
curl/wget（什么都没带） -> Inbound
其余 -> Unknown
```

几处设计判断：

- **`git remote add` 被算作 Outbound**。加一个远端本身不发数据，但它是「将来要往外发」的意图声明。归到出站是把「意图」而非「动作」记进日志——从排查视角看，「这个 agent 配置了一个外部远端」比「它还没推」更有意义。
- **`curl` 的正则同时认长选项与短选项**：`-X POST`、`--data`、`-d`、`-F`、`-T` 都在里面。这是为了同时覆盖 curl 的几种上传写法（表单、原始数据、上传文件）。
- **`scp`/`rsync` 的方向靠「最后一个参数有没有冒号」判断**。因为在 `scp local remote:path` 里远端在最后，而 `scp remote:path local` 里远端在最前。只判最后一个参数能把最主流的两种写法分开，但对三个以上参数（如 `scp a b: c`）不保证正确。
- **`curl`/`wget` 兜底为 Inbound**。这是最后一条正向规则：「没带上传参数的网络请求默认算下载」。这个默认值合理（绝大多数 `curl` 用法是取数据），但它也可能把 `curl -O url` 之类的语义搞反——不过这里的方向只进日志，不影响动作。
- **兜底是 `Unknown` 而不是 `Inbound`**。即没有认出任何网络命令的 shell 命令，方向记 `unknown`，而不是猜一个。

**「去重是按 destination 而不是 domain」这一点在两个层面出现。**

外层（`inspect` 里）：

```rust
let destinations: Vec<_> = extract_destinations(&text).into_iter()
    .filter(|d| seen_destinations.insert(d.destination.clone()))
    .collect();
```

`seen_destinations` 是**整批请求**（`tool_requests` 循环之外）定义的，所以同一批里两个命令访问同一目标只会记一次。去重键是 `destination`（完整的目标字符串，如整个 URL），而不是 domain——因为 `https://a.com/x` 与 `https://a.com/y` 是访问了同一个主机的不同资源，保留两条能看到更多细节。

内层是前面说的 `generic_network` 与精确类之间的 domain 级去重。**两层去重的粒度不同**（外层按完整目标、内层按域名），这是有意为之：内层要防止同一个目标被两种模式重复识别；外层要防止不同请求重复报同一目标。

**动作恒为 Allow、confidence 恒为 0.0——这两个「常数」各有一层含义。**

```rust
results.push(InspectionResult {
    tool_request_id: tool_request.id.clone(),
    action: InspectionAction::Allow,
    reason: format!("Egress destinations detected: {}", ...),
    confidence: 0.0,
    inspector_name: self.name().to_string(),
    finding_id: None,
});
```

- **`Allow`** 是因为它不该拦。
- **`confidence: 0.0`** 对它而言不是「不确定」而是「这不是一个判定」。它没有做出恶意/安全的判断，所以置信度应该是零。若填 1.0，那么上游若按置信度排序或加权，这个观测结果会被当成强证据。

而 `reason` 里带着目标列表，这是它真正的产出——即便动作是 Allow，理由也把「发到哪儿了」写进检查链的结果。这样 UI 或日志层仍能展示这条信息，而不是只能去翻遥测。

**日志字段是这一页的核心产出。** 每个目标一条：

```rust
tracing::info!(
    security.event_type = "egress",
    security.action = "LOG",
    security.threat_type = "data_exfiltration",
    network.destination = dest.destination.as_str(),
    network.domain = dest.domain.as_str(),
    network.egress_kind = dest.kind.as_str(),
    network.direction = direction.as_str(),
    tool.name = name,
    "network egress detected"
);
```

五处值得看：

1. **`security.action = "LOG"`**：动作空间里除了 Allow/Deny 还有 LOG，这让「记录了但没有判定」在日志层面可见，与那些会拦的事件区分开。
2. **`security.threat_type = "data_exfiltration"`**：注意这是一条**固定值**。也就是说一个 `git clone`（Inbound）也会被标成 `data_exfiltration` 威胁类型。这是一个用词上的粗糙处——威胁类型字段本该与方向配合，但这里不区分。下游若按 `threat_type` 聚合，会把所有出站与入站都算成数据外泄。
3. **`network.destination` 与 `network.domain` 两字段并存**：前者是完整目标（含路径），后者是主机。这样既能按域名聚合，也能看到具体路径。
4. **`network.egress_kind`**：九类之一的字符串，让「这是 `git_remote` 还是 `docker_registry`」可被查询。
5. **每个目标一条日志**而不是每个请求一条：`for dest in &destinations` 在外层循环内。所以一个带三个 URL 的命令会产生三条日志，它们的 `tool.name` 相同、`network.*` 不同。这种形态便于按目标做检索。

另外注意 `EgressDirection::as_str()` 把三个枚举值转成 `"outbound"` / `"inbound"` / `"unknown"` 小写字符串。用一个方法而不是 `Debug` 格式化，说明这个字符串是**作为数据被消费的**（进日志、进索引），不是给人调试看的。

## 关键取舍

**源码事实：** `EgressInspector` 的动作恒为 `Allow`、`confidence` 恒为 `0.0`，它的产出是 `reason` 里的目标列表与结构化日志，而不是判定。

**源码事实：** 只审 shell 类与 web 类工具，两类用不同的参数键（shell 看 `command`/`cmd`/`script`/`input`，web 看 `url`/`uri`/`endpoint`），只取第一个命中的字符串。

**源码事实：** 九类目标抽取中，S3 与 GCS 会把 bucket 名翻译成规范域名；`generic_network` 会跳过前几类已识别的域名（大小写不敏感）；`npm`/`cargo` publish 要求出现在行首或命令分隔符之后。

**源码事实：** `ssh` 的正则专门跳过「选项 + 选项值」的成对出现，并对捕获到的 host 再加一条「不能以 `-` 开头」的守卫。

**源码事实：** 方向判定按固定顺序：明确的 git/docker/publish 命令先归类；`curl`/`wget` 带上传参数算 Outbound；`scp`/`rsync` 以「最后一个参数含冒号」判定；`curl`/`wget` 无上传参数兜底为 Inbound；其余为 Unknown。

**源码事实：** 去重有两层：跨请求按完整 `destination` 去重（`seen_destinations` 在循环外），同类之间按 `domain` 去重（`generic_network` 跳过已识别域名）。

**源码事实：** 日志固定写 `security.threat_type = "data_exfiltration"`、`security.action = "LOG"`，每个目标一条，字段含完整目标、域名、类别与方向。

**架构推论：** 「只观测不拦截」是一个成熟的定位选择，而不是未完成的实现。它的判据是：这类行为的合法版本与恶意版本在数据层面无法区分——`git push` 与 `curl --data @secrets` 都是出站，靠命令文本无法稳定判定意图。于是把它做成观测器，价值在于**事后可查**；如果强行拦截，误报会把正常的开发流程全部堵死。

**架构推论：** 九类目标抽取的实质是「把 shell 命令里的目标地语法**降级成一个扁平列表**」。原始命令里目标的语义各不相同（远端仓库、对象存储、包注册中心），而汇报层只想知道「数据会到哪个主机」。这个降级需要逐类翻译（bucket → 域名、registry 目标 → 主机），是本模块最费力的部分。

**架构推论：** 方向判定用的是**命令级的关键词与参数形状**，而不是解析命令后看语义。这是务实的选择：完整解析 shell 语法（含管道、重定向、子命令、变量展开）成本极高且难以覆盖所有 shell，而关键词判定能覆盖绝大多数真实用法。代价是它会被误导——`echo "git push"` 会被判成 Outbound。这里能接受，因为方向只进日志。

**架构推论：** 一处值得指出的不一致：`threat_type` 恒为 `data_exfiltration`，与 `direction` 字段可能相矛盾（Inbound 的 `git clone` 也被标成数据外泄）。更准确的做法是按方向分档（`inbound` 时标 `data_ingress` 或不标威胁类型），或者干脆不设这个字段而只用 `egress_kind` + `direction`。当前形态会让按 `threat_type` 聚合的下游统计偏大。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/security/egress_inspector.rs` | `EgressInspector` | 出口观测审查器，动作恒 Allow |
| `crates/goose/src/security/egress_inspector.rs` | `EgressDirection` | `Outbound` / `Inbound` / `Unknown` 三态与 `as_str` |
| `crates/goose/src/security/egress_inspector.rs` | `EgressDestination` | 目标地：类别、完整目标、域名 |
| `crates/goose/src/security/egress_inspector.rs` | `extract_destinations` | 九类正则抽取与 S3/GCS/publish 的规范化 |
| `crates/goose/src/security/egress_inspector.rs` | `detect_direction` | 按固定顺序的方向判定 |
| `crates/goose/src/security/egress_inspector.rs` | `extract_text_for_inspection` | 按工具类别选择参数键并取第一个字符串 |
| `crates/goose/src/security/egress_inspector.rs` | `extract_domain_from_url` | 手写 URL 主机解析，含 IPv6 方括号与 userinfo |
| `crates/goose/src/security/egress_inspector.rs` | `is_shell_tool` / `is_web_tool` | 工具名前缀与后缀匹配 |
| `crates/goose/src/security/egress_inspector.rs` | `inspect` | 逐请求抽取、去重、记日志、产出 Allow 结果 |

## 参考

- [goose 源码：security/egress_inspector.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/security/egress_inspector.rs)
- [goose 文档：安全](https://block.github.io/goose/docs/guides/security/)
- [OWASP：LLM10 无界消耗与数据外泄](https://genai.owasp.org/llmrisk/llm102025-unbounded-consumption/)
- [MITRE ATT&CK：通过 C2 通道外泄数据](https://attack.mitre.org/techniques/T1041/)

## 与主核 / AI 的关系

「只观测、不拦截，把结构化事实写进审计流」与主核里的**审计日志（如 `pgaudit`）**是同一种定位。主核可以记录每条语句访问了哪些表、哪个用户、什么时间，但不因此拒绝执行——因为「访问了敏感表」未必是错误（管理员本来就要看）。审计的价值在事后追责与异常发现，而拦截要靠策略（权限、行级安全）。这里的 `Allow` + `confidence: 0.0` 正是同一种分工。

「把多种目标的语法降级成一个统一的扁平表示」在主核里对应的是**路径与对象名的规范化**：`s3://bucket/key` 与 `my-bucket.s3.amazonaws.com/key` 是同一个位置的两种写法，必须归一才能比较。本页里 S3/GCS bucket 翻译成域名，就是把这个归一化做在检测侧。

「完整解析 shell 语法太贵，改用关键词判定」这条取舍，与主核里**用启发式而非精确证明做规划**是同类的：主核在估算选择率时会用抽样与直方图（近似），而不是精确扫描全表（正确但不可行）。两者都选择了「够用的近似」，并接受已知的误差——区别在于主核的误差会体现在性能上，这里的误差只体现在日志准确性上。

对 AI 工程的实际意义在于：数据外泄是 agent 类系统最现实的威胁之一，而**它的合法版本与攻击版本共享同一批命令**。这份实现给出的两条经验可直接用：**把「记录」与「拦截」做成两件事**（记录可以很详尽、很激进；拦截必须很保守）、以及**目标的规范化要在检测侧做完**（bucket 与域名、registry 与主机必须归到同一个维度才能聚合）。第三点也可以借鉴：去重要分层——同一次请求内按目标精确去重，同一批请求间按完整目标去重，避免日志被同一个 URL 刷屏。
