---
project: mcp-python-sdk
feature: skills
title: 技能包与资源校验
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/shared/skills.py
    symbols: [Skill, SkillResource, validate_directory_result]
  - path: src/mcp/server/skills.py
    symbols: [Skills]
---

# 技能包与资源校验

## 一句话职责

把「一个技能」建模成一个 **URI 根下的一组文件清单**：URI 决定技能名、清单里每一项都必须落在同一个目录内且不重复、`SKILL.md` 必须在其中；服务端侧只提供协议管道与一致性校验，**完全不碰文件系统**。

## 机制是什么

三层分工：

| 层 | 位置 | 内容 |
|---|---|---|
| 线格式类型 | `shared/skills.py` | `SkillResource`、`Skill`、三对 params/result、三个 `Request` 类 |
| 一致性校验 | `shared/skills.py` | `skill_name_from_uri`、`_validate_resource_uri_in_skill`、`validate_directory_result` |
| 服务端管道 | `server/skills.py` | `Skills` 扩展：三个 `MethodBinding`、错误契约、缓存字段的版本门 |

扩展标识 `io.modelcontextprotocol/skills`，三个方法：`skills/list`、`skills/get`、`resources/directory/read`（最后一个是可选能力）。

## 怎么实现的

**「技能」是一个 URI，不是一份配置。** `skill_name_from_uri` 从 `SKILL.md` 的 URI 里取出名字：要求是绝对 URI、path 以 `/SKILL.md` 结尾、且**不能带 query 或 fragment**。名字取自路径的最后一段；若没有目录段（`skill://<name>/SKILL.md` 这种无组织前缀的裸形态），则退回到 **authority（hostname）**。这条映射不是随意的约定——docstring 引的是 SEP-2640 的 Resource Mapping 小节：路径最后一段等于技能的 `name`，裸形态下那一段就是 authority。

**`Skill._check_conformance` 把一串 SEP-2640 规则压成一个构造期检查。** 它按顺序做四件事：

1. 校验 frontmatter 的 `name`：必须是 `[a-z0-9]+(-[a-z0-9]+)*` 形式、长度 1–64。
2. 校验 `name` **与 URI 推出的名字一致**。这条把「清单说自己是 A、URI 指向 B」这种自相矛盾直接拒掉。
3. 校验 `description` 长度在 1–1024。
4. 若非 `"dynamic"`，逐个资源：URI 必须在技能根目录内、不能重复、且**技能的 `SKILL.md` 必须自己也在清单里**。

第 4 步的最后一条值得单独说：`resources` 不含 `SKILL.md` 是一个很容易犯又不明显的问题——客户端拿到清单后可能只按清单取文件，若主文件不在其中，它就永远读不到技能本体。

**目录归属的判定是「路径前缀 + 逐段查穿越」。** `_validate_resource_uri_in_skill` 检查六件事：绝对 URI、无 query/fragment、不以 `/` 结尾（目录不是文件）、scheme 与 netloc 与技能一致、path 要么等于 `SKILL.md` 自身的 path 要么以 `<root>/` 开头、且任何一段都不是 `.` 或 `..`。

这与 `g02-path-security` 那套字符串级检查同源但更窄：这里不需要处理符号链接（URI 不是文件系统路径），但必须处理 **scheme/netloc 相同而 path 逃逸**的情形，以及「以 `/` 结尾」这种把目录当文件声明的错误。

**`"dynamic"` 是清单的第三种形态，是一个明确的逃生口。** `SkillResources = list[SkillResource] | Literal["dynamic"]`。当 `resources == "dynamic"` 时 `_check_conformance` **提前返回**，不校验资源列表。docstring 说明它的用途是对应 SEP-2640 的 Resources 小节——一个清单太大或按需生成的技能可以说「资源要另取」，于是它不能声明具体条目。

**两个上限被明确标为 SHOULD NOT 而不是硬上限。** `MAX_RESOURCES_PER_SKILL = 512`（含 `SKILL.md`）与 `MAX_TOTAL_SIZE = 16 MiB`。注释写得很清楚：**规范要求宿主必须支持「至多 512 项」，并允许支持更多，所以 `Skill` 不会拒绝超限的清单**。这两个常量因此是「帮助调用方自检的参考值」，而不是校验的一部分——这个区分很重要，否则一个合规但偏大的技能包会被框架无故拒绝。

**`SkillResource` 的形状规则是内在的，所以放在字段级。** `digest` 用 `AfterValidator` 检查 `sha256:` + 64 位小写十六进制；`size` 用 `Field(ge=0)`。docstring 把它们与 `Skill` 的规则分开描述：这些是「形状」（谁都不会想接受一个畸形摘要），而 `Skill` 的那些是「SEP-2640 一致性」（依赖多个字段之间的关系）。

**`ListSkillsResult` 在 `Skill` 自校验之上再加一条：不得有两个条目共享 `uri`。** 这是一个「结果级」不变式，因为它需要跨条目比较，单个 `Skill` 无法判断。同类思路出现在 `validate_directory_result` 里（去重也是跨条目）。

**服务端只做管道，不碰文件系统。** 模块 docstring 把这条写得很直接：**它不发现、不读取、不哈希任何技能；服务端作者提供处理器来回答三个方法，具体目录怎么存由作者决定；底层的 `skill://` 文件内容通过服务器普通的资源注册 API 提供**。示例代码里 `list_skills` / `get_skill` 都只是普通 async 函数。

**三个方法的必要性是被规范绑定的，因此写成两种构造参数形态。** `list_skills` 与 `get_skill` 是必填关键字参数，`read_directory` 可选。docstring 给了规范依据：**SEP-2640 要求服务端必须回答前两个，无论技能是否出现在列表里**。而后者的存在直接决定能力广告：`settings()` 返回 `{"directoryRead": True}` 或空 dict。

**错误契约把「调用方错」与「服务端错」严格分开。** docstring 三条：处理器主动 `raise MCPError` 返回特定错误（例如 `get_skill` 对不服务的 URI 抛 `INVALID_PARAMS`）；而**不符合 SEP-2640 的结果被这里捕获并报 `INTERNAL_ERROR`**——因为那是服务端故障，不是调用方参数错。这个区分决定了客户端/监控该怀疑谁。

**每个处理器都在返回前重新校验一遍出站载荷。** 注释解释得很清楚：`ListSkillsResult` / `Skill` 在构造时就自校验，所以**构造**不合规清单的处理器会在这里抛 `ValidationError`；但模型是可变的，所以**出站前再 `model_validate(result.model_dump())` 一次**，挡住「构造后又被改动」的情形。这是「构造期校验 + 出站前复核」的双保险，成本很低（一次 round-trip 序列化）而覆盖面很大。

**`skills/get` 还多校验一条「返回的技能就是被请求的那个」。** `result.skill.uri != params.uri` 也报 `INTERNAL_ERROR`。这条防的是处理器在路由上出错——请求 A 却返回 B，客户端会以为拿到了 A 的内容。

**入站 URI 的形状错误走 `INVALID_PARAMS`，且错误消息直接透传。** `_require_skill_md_uri` / `_require_directory_uri` 把 `ValueError` 转成 `MCPError(INVALID_PARAMS, str(exc))`。这里把原始消息给调用方是有意的：URI 形状是调用方给的东西，告诉它哪里错了没有信息泄漏风险。

**缓存字段的版本门是本页最微妙的一处。** 三个结果类型都继承 `CacheableResult`，因而带 `ttlMs` / `cacheScope`（SEP-2549）。但这些是**扩展方法**，所以——`_finalize_cacheable` 的 docstring 点明——**runner 的按版本表面筛子不会跑在它们的结果上**，没有别的东西会在旧连接上剥掉这两个字段。于是这里手工做：若协议版本不在 `MODERN_PROTOCOL_VERSIONS` 里，就 `model_dump` 后 `pop` 掉 `ttlMs` 与 `cacheScope`。

这条是「扩展方法不受核心版本门保护」这个事实的直接后果，也是 `shared/skills.py` 里 `ListSkillsResult` / `GetSkillResult` / `ReadDirectoryResult` 三处 docstring 都在提醒的同一件事：**直接为旧连接构造这些结果的调用方必须自己省略这两个字段**。

**`_finalize_cacheable` 还顺带说明了默认值的安全性。** `CacheableResult` 默认 `cache_scope="private"`，注释理由：**把用户特定的条目挡在共享缓存之外**。技能清单可能因用户而异（权限、订阅），默认私有是正确的安全侧取值。

**`validate_directory_result` 的自我限定写得很清楚。** docstring 说它「只检查包含关系与形状——每个列出的资源都是 `uri` 的直接子项且 `uri` 唯一」，并主动声明**它无法确认清单是穷尽的，因为它没有对目录内容的独立视野**。这是把「校验器能证明什么」讲明白的范例：不夸大，也不假装能校验它无法知道的事。

**「直接子项」的判定用 `removeprefix` 而不是分割。** 先算 `prefix = parent_path.rstrip('/') + '/'`（根目录则用 `'/'`），再 `relative = child.path.removeprefix(prefix)`；若 `relative` 与原文相同（没剥掉）、为空、含 `/`、或是 `.`/`..`，就都不是直接子项。用 `removeprefix` 一次判定，比先分割再接回去更不容易出错。

## 关键取舍

**源码事实：** 技能的身份由 URI 决定，frontmatter 的 `name` 必须与 URI 推出的名字一致，否则构造期拒。

**源码事实：** 非 `"dynamic"` 时清单必须包含技能自身的 `SKILL.md`、不得重复、每项必须在技能根目录内且不含穿越段。

**源码事实：** 512 项与 16 MiB 是 SHOULD NOT 参考阈值，**不是**校验：超限清单被接受，因为规范要求宿主支持至多 512 并要求允许更多。

**源码事实：** 服务端完全不碰文件系统：不发现、不读取、不哈希；底层内容走普通的资源注册 API。

**源码事实：** `list_skills` / `get_skill` 必填、`read_directory` 可选；后者的存在决定 `directoryRead` 能力广告。

**源码事实：** 处理器主动抛的 `MCPError` 原样返回；不合规结果被转成 `INTERNAL_ERROR`。入站 URI 形状错误是 `INVALID_PARAMS` 且消息透传。

**源码事实：** 每个处理器都在出站前重新校验一遍载荷，挡住「构造后被改动」。

**源码事实：** `skills/get` 额外校验返回的技能 URI 与请求一致。

**源码事实：** 扩展方法不受 runner 的按版本表面筛子保护，所以 `ttlMs`/`cacheScope` 必须人工按版本剥掉；`cache_scope` 默认 `private`。

**架构推论：** 这一层把「协议一致性」与「内容供给」彻底分开：一致性是**可以在构造期与出站前完全判定**的（URI 关系、名称匹配、去重、形状），而内容供给是不可以判定的（技能从哪来、怎么存）。因此校验全部集中在模型与管道里，而处理器是纯粹的注入点。这个分法的收益是**校验不可绕过**：即使处理器乱来，出站前的复核也会把它拦下；代价是每次返回多一次序列化往返——而相对于一次跨进程 RPC，这个成本可以忽略。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/shared/skills.py` | `Skill` | 技能条目，构造期执行 SEP-2640 一致性校验 |
| `src/mcp/shared/skills.py` | `Skill._check_conformance` | 名称/描述/URI 一致性/资源清单的全部关系校验 |
| `src/mcp/shared/skills.py` | `SkillResource` | 清单项：URI + `sha256:` 摘要 + 非负大小 |
| `src/mcp/shared/skills.py` | `skill_name_from_uri` | 从 `SKILL.md` URI 取技能名，裸形态退回 authority |
| `src/mcp/shared/skills.py` | `_validate_resource_uri_in_skill` | 资源必须落在技能根目录内且不含穿越段 |
| `src/mcp/shared/skills.py` | `ListSkillsResult` | 结果级去重：不得有两个技能共享 URI |
| `src/mcp/shared/skills.py` | `validate_directory_result` | 直接子项 + 去重校验，并自认无法确认穷尽性 |
| `src/mcp/shared/skills.py` | `parse_directory_uri` | 目录 URI 规范化，拒尾斜杠与 query/fragment |
| `src/mcp/shared/skills.py` | `MAX_RESOURCES_PER_SKILL` / `MAX_TOTAL_SIZE` | SHOULD NOT 参考阈值，不参与拒绝 |
| `src/mcp/server/skills.py` | `Skills` | 扩展实现：三个方法绑定与错误契约 |
| `src/mcp/server/skills.py` | `Skills.methods` | 按是否提供 `read_directory` 决定绑定集合 |
| `src/mcp/server/skills.py` | `Skills.settings` | 用 `directoryRead` 广告可选能力 |
| `src/mcp/server/skills.py` | `Skills._handle_list` / `_handle_get` | 出站前重新校验，含 get 的 URI 一致性检查 |
| `src/mcp/server/skills.py` | `_finalize_cacheable` | 扩展方法不受版本筛子保护，人工剥缓存字段 |
| `src/mcp/shared/skills.py` | `EXTENSION_ID` / `METHOD_LIST` / `METHOD_GET` / `METHOD_READ_DIRECTORY` | 扩展与三个方法的字面量 |

## 参考

- [mcp-python-sdk 源码：shared/skills.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/skills.py)
- [mcp-python-sdk 源码：server/skills.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/skills.py)
- [SEP-2640：通过 MCP 提供 Agent Skills](https://github.com/modelcontextprotocol/modelcontextprotocol/pull/2640)
- [MCP 规范：资源](https://modelcontextprotocol.io/specification/2025-06-18/server/resources)

## 与主核 / AI 的关系

技能包与主核里的「表空间目录」有结构相似性：一个具名容器下的一组对象，每个对象有名字、大小与校验值，容器有「目录读取」的需求。差别在于这里的一切都通过 URI 表达，且校验必须能在**没有任何存储访问**的前提下完成。

对 agent 工程的直接价值有三条。其一，**把一致性做成可在构造期判定的关系**。名称必须与 URI 一致、清单必须包含主文件、资源必须在同一根下——这些都能纯靠字段间关系判定，于是可以放在模型里强制，而不是靠文档约定。其二，**区分「形状规则」与「规范阈值」**。畸形的摘要必须拒，而超大的清单只该提示；把 SHOULD NOT 当 MUST NOT 会让框架拒绝规范允许的输入，这类错误一旦发布就很难收回。其三，**扩展方法要自己处理版本门**。核心方法的结果有框架的按版本筛子保护，扩展方法没有——想让旧客户端拿到干净的结果，就必须自己剥掉新字段。
