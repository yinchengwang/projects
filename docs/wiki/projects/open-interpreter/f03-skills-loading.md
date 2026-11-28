---
project: open-interpreter
feature: skills-loading
title: 技能加载与 frontmatter 解析
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/skills/src/loading.rs
    symbols: [SkillRootLoader, LoadedSkills, SkillRootSnapshotCache]
  - path: codex-rs/skills/src/parser.rs
    symbols: [parse_skill_frontmatter_metadata, MAX_NAME_LEN, SkillParseError]
---

# 技能加载与 frontmatter 解析

## 一句话职责

两件事分开做：**加载**只定义接口与数据结构（谁提供技能根、快照缓存归谁所有、结果怎么组织），**解析**负责从一份 `SKILL.md` 里取出并校验元数据——而且它要能容下第三方写坏的 YAML，因为真实的技能市场里那些文件确实写坏了。

## 机制是什么

| 文件 | 职责 | 行数 |
|---|---|---|
| `loading.rs` | 只有类型与 trait：**没有一行文件系统访问** | 119 |
| `parser.rs` | 纯函数式的 frontmatter 解析 + 一处容错修复 | 225 |

`loading.rs` 里六个公开项：`SkillError`、`LoadedSkillRoot`、`LoadedSkills`、`SkillRootSnapshotCache`（trait）、`SkillRootSnapshots`（句柄）、`SkillRootLoadRequest`、`SkillRootLoader`（trait）+ 一个 `SkillLoadFuture` 类型别名。

`parser.rs` 里两个结构（`SkillFrontmatter` 与 `SkillFrontmatterMetadata`，私有）、一个结果类型 `ParsedSkillFrontmatter`、一个错误枚举、一个入口函数。

## 怎么实现的

### 加载层：接口与所有权分离

**`loading.rs` 只有 119 行，且其中没有一行是 IO。** 这个事实本身就是设计声明：**「怎么找文件」被完全从这个 crate 里剥离出去**，本 crate 只定义契约。

四条契约：

1. `SkillRootLoader<Root>::load_roots(&self, request) -> SkillLoadFuture<'_, LoadedSkills>`——`Root` 是泛型参数，于是调用方可以用自己的根标识类型（路径、id、枚举都行）。
2. `SkillRootSnapshotCache<Root>: get(&self, &Root) -> Option<LoadedSkillRoot>` 与 `insert(&self, Root, LoadedSkillRoot)`——缓存由实现方拥有。
3. `SkillRootLoadRequest<Root>` 三个字段：`roots: Vec<Root>`、`restriction_product: Option<Product>`、`snapshots: Option<SkillRootSnapshots<Root>>`。
4. `SkillRootSnapshots<Root>` 是对 `Arc<dyn SkillRootSnapshotCache<Root>>` 的一层薄包装。

**`SkillRootCMSnapshotCache` 的文档注释是本页最值得记的一段。** 原文：

> 缓存已解析的根，但不把缓存的所有权指派给某个 loader。实现方拥有存储并决定其生命周期；loader 可以复用已有快照，也可以发布一个新加载的快照，而**不必了解调用方更大的状态**。

也就是说这里的抽象目标是**解耦两件事**：「解析技能需要 IO 与缓存」与「缓存该活多久」。一个 CLI 进程可能希望缓存进程内有效；一个 app-server 可能希望缓存跨请求有效并可按配置失效。若缓存由 loader 拥有，这两种生命周期就无法同时支持。

**`SkillRootSnapshots` 的 `PartialEq` / `Hash` 用 `Arc::ptr_eq` 与指针哈希，这条注释解决了它的用途。** 原文：**相等与哈希使用缓存分配的标识，于是调用方可以把这个句柄放进自己的缓存键里，而不必检视或取得其内容的所有权**。

也就是说这个句柄的价值在于**可以当键用**。上层要缓存「这份配置 + 这批根 → 这批技能」的结果，而缓存键里必须包含「用的是哪一份快照」；如果比较是内容级的（比较全部技能），键会极大且计算昂贵。用指针身份则既是常量时间，也**保证「同一份快照的两次使用」被认为是同一个输入**。

这是一个把「缓存键的身份 vs 值」这件事做对的实例：句柄身份用于键，内容用于判断是否需要重载。

**`SkillRootSnapshots::Debug` 用 `finish_non_exhaustive()`。** 即打印成 `SkillRootSnapshots` 而不展开内部。理由与上面的身份语义一致：内部是 `dyn` trait 对象，任何展开都不稳定且无意义。

**`LoadedSkillRoot` 有一个字段值得单独看：`skill_discovery_path_by_path: Arc<HashMap<AbsolutePathBuf, AbsolutePathBuf>>`。**

它是一个**从技能文件路径到「发现路径」的映射**，且用 `Arc` 共享。这个映射的存在说明：一个技能在磁盘上的真实位置与「它在目录结构里被发现的位置」可能不是一回事——例如一个技能目录里的符号链接、或一个插件目录里的转发。而 `selection.rs`（下一节）要用这个映射做「按路径匹配技能」的正则化。

用 `Arc` 而非 `HashMap` 是必要的：`LoadedSkillRoot` 会被 `SkillRootSnapshotCache` 复制给多个调用方，而这份映射可能很大。

**`LoadedSkills` 是 `Default + PartialEq` 的两字段结构（`skills`、`errors`），`LoadedSkillRoot` 却多了 `root`、`is_agent_plugin` 与上面那个映射。** 也就是说有两级聚合：

- **每个根**一份 `LoadedSkillRoot`（含自己的错误列表）；
- **整批根**一份 `LoadedSkills`（合并所有技能与所有错误）。

而 `is_agent_plugin` 只在 per-root 层存在——因为「某个根是不是 agent 插件」是根级属性，聚合到整批就没意义了。

**`SkillError` 是 `path + message` 的两字段结构，没有错误码。** 这是一个刻意的朴素选择：技能加载的错误大多是「这个文件读不了 / 解析不了 / 校验不过」，调用方的处理方式统一是「跳过并告诉用户」，于是不需要类型化的错误。

**`SkillLoadFuture` 类型别名的存在是为了 trait 的对象安全性。** 定义是 `Pin<Box<dyn Future<Output = T> + Send + 'a>>`，而 `SkillRootLoader::load_roots` 返回 `SkillLoadFuture<'_, LoadedSkills>`。

若直接写 `async fn`，这个 trait 就不是对象安全的（无法 `dyn`）。而 `Arc<dyn SkillRootLoader<Root>>` 这种用法在缓存场景里几乎是必须的。所以这里显式地写成 boxed future——与那个项目 `AGENTS.md` 里「避免 `async_fn_in_trait`，显式写出 future 契约」的规则一致。

### 解析层：容错但要收紧

**`SkillParseError` 有四个变体，覆盖四种失败阶段。**

| 变体 | 含义 |
|---|---|
| `MissingFrontmatter` | 没有 `---` 分隔的前置块 |
| `InvalidYaml(serde_yaml::Error)` | YAML 解析失败（带源错误） |
| `MissingField(&'static str)` | 必填字段缺失 |
| `InvalidField { field, reason }` | 字段存在但不合法 |

`MissingField` 与 `InvalidField` 用 `&'static str` 作字段名——**字段名只能是编译期已知的那几个**，不是任意字符串。这避免了「错误消息里出现用户可控文本」，与前面项目里「诊断字段名要过白名单」是同一条纪律的轻量版本。

**`extract_frontmatter` 的实现有三个细节。**

1. 首行必须 `trim() == "---"`——**允许前后空白**，于是 `  ---` 也算。
2. 逐行读到结束分隔符，期间的**内容行**收集起来；结束分隔符同样用 `trim() == "---"` 匹配。
3. 若**收集到的内容为空**或**没找到结束分隔符**，返回 `None`。

第 3 点的「内容为空也算缺 frontmatter」值得注意：一份只有 `---\n---\n` 的文件会被报成 `MissingFrontmatter`，而不是「解析出一个空对象然后报 `MissingField(description)`」。前者更接近用户的实际情况（他们根本没写元数据）。

**`parse_skill_frontmatter_metadata` 的签名带一个 `default_name: impl FnOnce() -> String`。** 也就是说**名字可以从外部兜底**——frontmatter 里没写 `name` 时调这个闭包。这个设计让「名字从哪来」成为调用方的决定（通常来自目录名），而不必在解析器里猜。

而兜底前的处理是 `parsed.name.as_deref().map(sanitize_single_line).filter(|v| !v.is_empty()).unwrap_or_else(default_name)`——**先规整空白、再过滤空串、最后才兜底**。于是「`name: "   "`」这种情况会走兜底而不是得到一个空白名字。

**`sanitize_single_line` 把任意空白折成单空格。** 实现是 `raw.split_whitespace().collect::<Vec<_>>().join(" ")`。于是名字里不可能有换行、制表符、连续空格。

这条对名字是必要的：技能名要出现在 `$skill-name` 这样的提及语法里，而提及解析器只认特定字符（字母、数字、`_`、`-`、`:`）。一个带空格的技能名将永远无法通过提及被选中。规整改不了「名字里有非法字符」的问题，但至少消灭了空白这一类。

而 `short_description` 用同样的规整但**不需要兜底**（`filter(|v| !v.is_empty())` 后仍是 `Option`）。

**`MAX_NAME_LEN = 64`，且校验用的是 `value.chars().count()`。** 于是长度是**字符数**而不是字节数——一个 64 个汉字的名字通过，而一个 64 个字符的 ASCII 名字也通过。选择字符数是对的：限制的目的是「名字要能被展示和被键入」，与字节长度无关。

**校验顺序是「先长度、后描述非空」**：

```
validate_len(&name, MAX_NAME_LEN, "name")?;
if description.is_empty() { return Err(MissingField("description")) }
```

而这个顺序意味着**一个名字超长且描述缺失的文件报的是名字错误**。这是合理的：越靠前的字段越可能是用户首先要处理的。

**`description` 缺失时给 `MissingField`，而不是 `InvalidField`。** 虽然实现上它是「空串」而不是「字段不存在」（因为 `description` 有 `serde(default)`，缺失时得到 `None` → `unwrap_or_default()` → 空串）。也就是说**「缺失」与「写成空串」被合并成同一个错误**，这正是 `SkillParseError::MissingField` 那一条的语义。

这个合并是有意的：对调用方来说两者一样，都是「这份技能没有可用的描述」。

### YAML 修复：一行一行地只修出问题的那几个标量

**`repair_frontmatter_scalar_fields` 是这一页最复杂的函数（80 行），它的存在理由写在注释里。** 原文：

> 有些第三方技能用了散文式的写法，例如 `description: Build for AWS: ECS` 或 `argument-hint: <duration: e.g. 7d>`。**保持这个修复按行进行，这样其它无效 YAML 仍然会暴露出来**。

这两句说明了两件事：

1. **问题真实存在**：`description: Build for AWS: ECS` 在 YAML 里是**非法的**——未加引号的标量里出现 `: ` 会被当成映射键的分隔符。而写技能的人只是想写一句人话。
2. **修复的边界是刻意收窄的**：不做通用的 YAML 宽松解析（那等于放弃校验），只对「像这种写法」的行加引号。

**修复的判定是一串条件，逐个都值得看。** 对每一行：

- 先算缩进；**若在块标量内部**（前一行的值以 `|` 或 `>` 开头，且当前行缩进大于它），跳过——块标量里的 `:` 完全合法。
- 用 `split_once(':')` 拆键值；**键为空则跳过**（例如 `- item` 这类行）。
- **值必须以空白开头**（`value.chars().next().is_none_or(char::is_whitespace)` 的反面）——即 `key:value` 这种没有空格的写法被跳过，因为那不是「散文式描述」而是紧凑写法（或别的语法）。
- 取值的起始空白与被修剪后的标量，并**在标量里找注释起点**：一个 `#` 且它前面是空白（或它是第一个字符）。
- **标量首字符是 `|` 或 `>` 时**记录「进入块标量」并跳过。
- **首字符是 `'` 或 `"` 时跳过**——已经加过引号。
- 判断标量里**是否存在 `:` 后接空白的分隔符**；若是，或者首字符是 `[`/`{`/`@`/`` ` `` 且该标量本身解析不成 YAML 值，则**需要修复**。
- 修复方式是：`format!("{key}:{leading_whitespace}'{quoted}'{comment}")`，其中 `quoted_scalar` 是把内部的 `'` 双写成 `''`。

**这一整套条件的共同目标是「最小侵入」**：只在能确定「这一行是个裸标量且它因为有冒号而非法」时才加引号。任何不确定的情形（块标量、已引号、紧凑写法、无法解析的 flow 结构）都不动。

**注释的位置保留得不平常。** 修复后是 `key:{空白}'值'{注释}`——也就是**原来的空白宽度与注释文本都被保住**。若简单地把整行替换成 `key: '值'`，用户文件里的对齐与注释就全丢了，而这是一个自动修复工具不该做的事。

**修复失败时返回的是原始错误，而不是修复后的错误。** 实现是：

```
Some(repaired) => serde_yaml::from_str(&repaired).map_err(|_| original_error),
None => Err(original_error),
```

**`.map_err(|_| original_error)` 这一句把修复后的解析错误丢弃、换回原始错误。** 这个选择意味着：修复没有救回来时，用户看到的是「原文件为什么错」，而不是「我改过之后为什么还错」。后者会让人以为自己的文件有更深的问题。

这条细节是自动修复类工具的通用原则：**当修复不起作用时，诊断信息必须指向用户的实际输入**。

**修复只可能发生一次，且不会级联。** 函数返回 `Option<String>`，`changed` 标志决定是 `Some` 还是 `None`。调用处只在首次解析失败时试一次修复。所以「修一次还不行就报错」，不会反复尝试。

**`block_scalar_indent` 的维护是这个函数里唯一的状态机。** 它用 `Option<usize>` 记住块标量的起始缩进，遇到空行或缩进更大的行就继续跳过，否则清空。

逻辑上正确的边界是：**块标量在遇到缩进小于等于起始缩进的非空行时结束**。而实现是「`line.trim().is_empty()` 或 `indent > block_indent` 才 continue」，所以一个缩进等于 `block_indent` 的非空行会结束块标量——符合 YAML 的实际规则（块标量的内容必须比键更深缩进）。

## 关键取舍

**源码事实：** `loading.rs` 整份文件没有 IO，只有数据结构与两个 trait——文件系统访问、解析、优先级、product 过滤、快照复用全部被推到实现方。

**源码事实：** 缓存 trait 不拥有存储，于是同一个 loader 可以在「进程内缓存」与「跨请求缓存」两种生命周期下工作。

**源码事实：** `SkillRootSnapshots` 的相等与哈希用 `Arc::ptr_eq` / 指针哈希，目的是让它**能当缓存键用**——既常量时间，又保证同一份快照被认作同一个输入。

**源码事实：** `LoadedSkillRoot.skill_discovery_path_by_path` 是「技能真实路径 → 发现路径」的映射，用 `Arc` 共享；它的消费方是后续的按路径匹配。

**源码事实：** `SkillLoadFuture` 是 hand-written boxed future 类型别名，因为 `SkillRootLoader` 要能 `dyn`（对象安全）。

**源码事实：** `SkillParseError` 的字段名参数是 `&'static str`，错误消息里不出现用户可控文本。

**源码事实：** frontmatter 首尾分隔符都允许前后空白；内容为空也报 `MissingFrontmatter`（而不是「解析出空对象再报缺字段」）。

**源码事实：** 名字有外部兜底闭包，且兜底前先规整空白、过滤空串——所以「`name: "   "`」走兜底而不是得到空白名字。

**源码事实：** 名字长度限制按**字符**（`chars().count()`）计，64 上限；描述缺失与写成空串被合并成同一个 `MissingField`。

**源码事实：** YAML 修复是逐行、最小侵入的：只在「值是裸标量且因冒号而非法」时加引号，跳过块标量、已引号、紧凑写法、无法解析的 flow 结构；并保留原空白与注释。

**源码事实：** 修复后仍失败时返回**原始错误**，让诊断指向用户的实际输入而非修复产物。

**架构推论：** 这份代码把「加载」与「解析」彻底分开，而分开的收益在测试上最明显——`parser.rs` 是纯函数，225 行配 119 行测试；`loading.rs` 无逻辑，119 行配 79 行测试（测的是 trait 契约而非行为）。

而解析层的重心在**容错**而非校验：它检查的只有「名字非空且不超长」「描述非空」两条，其余精力全花在「让第三方写坏的 YAML 能被读出来」。这个比例是对的——技能是个开放生态，文件的作者不受控；一个「格式必须完美」的解析器会让大量真实技能无法加载。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/skills/src/loading.rs` | `SkillRootLoader` | 加载契约，`Root` 泛型 + boxed future |
| `codex-rs/skills/src/loading.rs` | `SkillLoadFuture` | 显式 boxed future 别名，保证 trait 对象安全 |
| `codex-rs/skills/src/loading.rs` | `LoadedSkills` / `LoadedSkillRoot` | 两级聚合：整批结果与 per-root 结果 |
| `codex-rs/skills/src/loading.rs` | `LoadedSkillRoot.skill_discovery_path_by_path` | 真实路径 → 发现路径的共享映射 |
| `codex-rs/skills/src/loading.rs` | `SkillRootSnapshotCache` | 缓存 trait，存储与生命周期归实现方 |
| `codex-rs/skills/src/loading.rs` | `SkillRootSnapshots` | 以 `Arc` 身份作相等与哈希的句柄 |
| `codex-rs/skills/src/loading.rs` | `SkillRootLoadRequest` | 根列表 + product 限制 + 可选快照 |
| `codex-rs/skills/src/loading.rs` | `SkillError` | 路径 + 消息的朴素错误 |
| `codex-rs/skills/src/parser.rs` | `parse_skill_frontmatter_metadata` | 唯一入口，带名字兜底闭包 |
| `codex-rs/skills/src/parser.rs` | `SkillParseError` | 四阶段错误，字段名为 `&'static str` |
| `codex-rs/skills/src/parser.rs` | `extract_frontmatter` | 提取 `---` 之间的块，空内容也视为缺失 |
| `codex-rs/skills/src/parser.rs` | `repair_frontmatter_scalar_fields` | 逐行最小侵入的 YAML 修复 |
| `codex-rs/skills/src/parser.rs` | `sanitize_single_line` | 折白，使名字可被提及语法识别 |
| `codex-rs/skills/src/parser.rs` | `MAX_NAME_LEN` | 64 字符的名字上限 |

## 参考

- [open-interpreter 源码仓库](https://github.com/OpenInterpreter/open-interpreter)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [YAML 规范](https://yaml.org/spec/1.2.2/)
- [open-interpreter 文档：技能](https://www.openinterpreter.com/docs/terminal/skills)

## 与主核 / AI 的关系

这套「接口层零 IO、实现层负责一切」与主核里「存储抽象层」是同构：数据库的访问方法层也只定义契约（读页、写页、刷盘），具体实现由 buffer manager 或直接 IO 提供。共同点是**把生命周期与策略交给调用方**，而不是在实现里定死。

对 agent 工程的直接价值有三条。其一，**缓存的「身份 vs 内容」要分开**。当缓存键需要包含「用的是哪份快照」时，用句柄身份（指针）而不是内容比较——既快，也避免两个内容相同但来源不同的快照被判为同一个。其二，**容错的边界要逐行收窄，不要全局放宽**。注释那句「保持修复按行进行，这样其它无效 YAML 仍然会暴露」是这类工具的关键：全局宽松等于放弃校验，而逐行修复能同时做到「救回真实输入」与「继续报真错」。其三，**自动修复失败时要报原始错误**。用户看到「改过之后仍然错」会去猜深层原因；看到原始错误才知道该改什么。
