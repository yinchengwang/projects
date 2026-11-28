---
project: ragflow
feature: runtime-common
title: 公共常量、Agent 事件与分词器
page_type: feature
schema_version: 1
refs:
  - path: internal/common/constants.go
    symbols: [DialogStatus, IsTerminalTaskStatus]
  - path: internal/common/agent_event.go
    symbols: [AgentEvent, AgentEventSnapshot]
  - path: internal/tokenizer/tokenizer.go
    symbols: [Tokenizer, New, Tokenize]
---

# 公共常量、Agent 事件与分词器

## 一句话职责

三块被全仓反复引用的运行时基础设施：`common` 用字符串常量与判定函数统一任务状态与对话状态的字面值；`AgentEvent` 定义一次节点执行的生命周期记录，`AgentEventSnapshot` 负责把任意值压成**有界且脱敏**的 JSON；`tokenizer` 用对象池封装 cgo 分词器，并提供 BPE 计数与截断。

## 机制是什么

三块东西看似无关，共同点是**它们都是「被到处 import 的底座」**，因此每一处设计取舍都会放大到全仓：

| 块 | 解决什么 | 关键约束 |
|---|---|---|
| `common` 常量 | 状态字面值散落在各处的隐患 | 必须能被字符串直接比较，且与 Python 侧共享 |
| `AgentEvent` 快照 | 事件要落盘/上报，但不能带敏感数据与超大载荷 | 有界、脱敏、不改动输入 |
| `tokenizer` | cgo 分词器开销大且不并发安全 | 池化、可伸缩、可关闭 |

**状态常量是「两态划分」而不是枚举。** `IsTerminalTaskStatus` 认 `COMPLETED` / `STOPPED` / `FAILED`；`IsActiveTaskStatus` 认 `CREATED` / `SCHEDULED` / `RUNNING` / `STOPPING`。七个状态恰好被两个函数分完，**没有第三个类别，也没有遗漏**。这比「用枚举 + switch 穷举」轻，也更适合跨语言共享（Python 侧写的是同样的字符串）。

`DialogStatus` 是 `string` 的类型别名，只有一个用法：把 `dialog.status` 从裸字符串提升成有名字的类型。它的值常量是 `StatusDialogValid = "1"`，注释明确写着「镜像 Python 的 `StatusEnum.VALID.value`」「所有 chatbot/agentbot 授权路径必须用这个常量而不是字面量」。

**快照函数有两个返回值。** `AgentEventSnapshot(value) (string, bool)` 返回 JSON 文本与「是否发生截断」。截断信息必须对外可见，否则使用者无法判断「这个 payload 是完整的还是被砍过的」。

**分词器对外是「值类型 + 方法」。** `Tokenizer` 只有一个 `lang` 字段，`New(lang)` 创建，各方法（`Tokenize` 等）在每次调用时从池里借一个分析器、设置语言、执行、归还。**它不持有分析器**，所以是廉价可复制的。

## 怎么实现的

### 状态常量与两个划分函数

七个状态常量是裸 `const`（`CREATED`、`SCHEDULED`、`RUNNING`、`COMPLETED`、`FAILED`、`STOPPED`、`STOPPING`），**没有包装成类型**。这与其他项目的做法相反，但有其理由：这些值要与 Python 侧、数据库字符串列、CLI 输出共享，用裸字符串最省转换。

`IsTerminalTaskStatus` 与 `IsActiveTaskStatus` 都是 `switch` + `default: false`，而不是「非 A 即 B」的取反。**两个函数各自枚举，而不是互相取反**——这允许存在既不活跃也非终态的中间状态（当前没有，但结构上允许）。

`UNSTART` 是一个特殊值，注释写明它是「派生显示值，不是持久化的任务状态」：文档没有活跃摄取任务时，列表接口报 `UNSTART`。它被包含在 `ValidRunStatuses` 里（过滤参数可接受），但**不在** `IsActiveTaskStatus` 也不在 `IsTerminalTaskStatus` 里——因为它根本不是任务状态。

`DefaultCancelRunStatuses` 只有 `SCHEDULED` 与 `RUNNING` 两个值，注释解释 `UNSTART` 被排除的理由：「没有任务可取消」。

常量文件里还有一处值得单独看的注释：`ParentChunkOrderBase = 1 << 30`。它把 chunk 的排序值域一分为二——源块在 `[0, 1<<30)`，父块从 `1<<30` 起。注释详细说明了为什么必须有这条分界：Elasticsearch 上父块的排序字段缺失会被当作最大值，天然排最后；但 Infinity 里「缺失」不存在，未写入的整数列回落到默认 0，于是父块与第一个源块并列，分页切片时顺序每页都不同。**这是「用一个足够大的偏移量把两类行隔开」的经典手法，而它被逼出来的原因是一个引擎的默认值语义。**

### AgentEvent 与有界快照

`AgentEvent` 有 22 个字段，覆盖一次节点执行的全维信息：标识（`EventID`、`Sequence`、`RunID`、`ExecutionID`、`ParentExecutionID`）、归属（`TenantID`、`UserID`、`AgentID`、`AgentVersion`、`SessionID`、`MessageID`）、节点（`NodeID`、`NodeName`、`NodeType`、`Attempt`）、结果（`Status`、`DurationMS`、`ErrorCode`、`ErrorMessage`）、载荷（`InputsJSON`、`OutputsJSON`、`PayloadTruncated`）与时间（`EventAt`）。

`Sequence` 是 `uint64`，`Attempt` 是 `uint32`，`DurationMS` 是 `int64`——**按语义选宽度而不是一律 int**。

`AgentEventSnapshot` 是本页最复杂的一段。它的目标是「任意值 → 有界、脱敏、合法 JSON」，实现靠一个递归闭包 `copyValue(value, depth)`，配三组配额：

1. **节点配额 `remaining = 256`**：每次递归减一，耗尽即标截断。
2. **深度上限 `depth > 8`**：超过即标截断。
3. **字节配额 `remainingBytes = 12000`**：字符串按长度扣减，耗尽即标截断。

三种配额都在同一处判：`remaining < 0 || depth > 8 || remainingBytes <= 0` → 返回 `"[truncated]"` 并置标志。

**按类型分派的处理策略各不相同。** 标量（`nil` 与各类数字/布尔）原样返回——注意数字类型列了 11 种，包括 `float32` 与各种 `uint`。

`string` 有三条支路：

- 以 `data:` 开头 → 返回 `"[binary omitted]"` 并标截断（内联二进制）。
- **长度不超过 16 KiB 且去空白后以 `{` 或 `[` 开头** → 尝试 `json.Unmarshal`，成功则把解析结果递归处理。注释说明动机：HTTP 节点把响应体以字符串返回，包括 token 端点返回的 JSON——**所以要先把「JSON 字符串」解成结构，才能让下面的键名脱敏生效**。若跳过这步，一段含 `"api_key":"xxx"` 的 JSON 文本会整段通过，只有正则脱敏能拦。
- 其余按 `min(4096, remainingBytes)` 截断，用 `strings.ToValidUTF8` 修掉被切开的 UTF-8，扣减字节配额，最后过 `RedactAgentEventText`。

`map[string]map[string]any` 是一个专门的快速分支（容量上限 64，只保留原值不做递归）。`map[string]any` 是最常走的路径，它做四件事：

- 键名超过 256 字节就截断并标标志；键名长度也扣字节配额。
- **键名以 `__` 开头或等于 `state` 的直接跳过**——注释在外层说明「不保留图内部状态」。
- 容量上限 64，超了标截断并跳出。
- 敏感键（`isSensitiveAgentEventKey`）直接替换成 `redactedLogValue`，不递归。

切片类（`[]map[string]any`、`[]any`、`[]string`）统一上限 64 项。

**默认分支是「拒绝」而不是「尽力转换」。** 遇到未列出的类型（函数、通道、自定义结构体）返回 `"[non-JSON value omitted]"` 并标截断。这让快照函数不会因为某个奇怪类型而 panic 或序列化失败。

最后 `json.Marshal` 一次，若序列化失败或结果超过 16 KiB，返回固定的 `{"truncated":true}` 并置截断。

**脱敏走两层，正则 + 键名。** `RedactAgentEventText` 用三条正则：

- `agentEventAuthorization`：把 `Bearer xxx` / `Basic xxx` 替换成 `$1 [REDACTED]`（保留 scheme 名）。
- `agentEventCredentials`：匹配 `api_key|password|secret|authorization|access_token|refresh_token|credential` 后面跟 `=` 或 `:` 的值，替换成 `${1}[REDACTED]`。
- `agentEventURLs`：对每个 URL **解析后清空 `User`、`RawQuery`、`ForceQuery`、`Fragment`**，只保留 scheme + host + path。注释点明理由：签名下载链接的查询串本身就是 bearer 凭据，**不论参数名是什么**。这比逐个匹配 `?token=` 之类的参数名可靠得多。

`isSensitiveAgentEventKey` 先做键名归一（去掉 `-`、`_`、`.`，转小写），再判：复用 `isSensitiveLogKey`，或后缀是 `apikey` / `token`，或等于 `proxyauthorization` / `cookie` / `setcookie`，或包含 `credential` / `password` / `secret`。**归一化是关键**：`api-key`、`api_key`、`API.Key` 都会变成 `apikey`。

### 分词器池

`analyzerPool` 是这一层的核心，字段分成三组：

- **状态**：`initialized`（受 `stateMu` 保护）、`baseAnalyzer`（模板）、`instances`（缓冲 channel，容量 `MaxSize`）。
- **并发**：`stateMu`（RWMutex）、`currentSize`（atomic）、`borrowers` / `wg`（两个 WaitGroup）。
- **生命周期**：`stopCh` + `stopOnce`。

注释把锁的设计讲得很清楚：**`acquire` / `release` / `shrink` 整个函数体持读锁，`Close` 用写锁翻转状态**。于是「检查 initialized + 操作 channel」成为一个不可分的动作，`Close` 一旦拿到写锁，就不会再有读者进来。`currentSize` 特意用 atomic 放在这对锁之外，让热路径的读-改-写保持廉价。

**`Init` 用双重检查 + 全局 atomic 指针。** `initMu` 串行化，`globalPool` 是 `atomic.Pointer[analyzerPool]`。已有的池且已初始化就直接返回（幂等）。默认值：`MinSize = 2×CPU`、`MaxSize = 16×CPU`、空闲超时 5 分钟、获取超时 10 秒；`MinSize > MaxSize` 时把 `MinSize` 压到 `MaxSize`。

**预热的顺序体现了一个重要约定**：先建 `baseAnalyzer` 并 `Load()`，再 `Copy()` 出 `MinSize` 个实例，全部成功后**才**置 `initialized = true`，再启动收缩 goroutine，最后 `globalPool.Store(p)`。注释写明「只发布完全构造好的池」，且「先起收缩循环再发布，避免观察到没有收缩器的池」。中途失败走 `discardUnpublished`——它自己关掉已建的实例与模板，因为此时 `initialized` 仍是 `false`，`Close` 的幂等守卫会直接返回而不释放任何东西。

**`acquire` 是快路径 + 扩容 + 等待三段。** 先非阻塞取；取不到就 `atomic.Load` 当前大小，若小于上限则 `CompareAndSwap` 抢占名额并 `createInstance`（失败要 `Add(-1)` 回退）；CAS 失败说明别的 goroutine 也在扩，直接落到等待段。等待段 `select` 三选一：拿到实例、`stopCh` 关闭（返回 `errPoolClosed`）、超时（返回带 `current_size` 与 `max` 的错误消息）。**成功取到实例后 `borrowers.Add(1)`**，这是为了让 `Close` 能等所有借出的实例归还。

**`release` 的行为取决于池是否还开着。** 持读锁后若 `initialized` 已为假，就**直接关掉这个实例而不是放回 channel**（channel 可能已关闭）。否则尝试非阻塞放回；池满则关掉实例并把 `currentSize` 减一——注释说「正常不该发生」。

**`Close` 是三步，顺序有讲究。**

1. **先关 `stopCh`**（在拿写锁之前）。注释解释了为什么：一旦有写者排队，RWMutex 就阻塞新读者，而某个卡在 `<-p.instances` 的 `acquire` 也持着读锁——它既无法被 `release` 喂食，又挡住写者，只能等超时。关 `stopCh` 让它立刻返回。
2. **拿写锁翻转 `initialized`**，然后放锁，再 `wg.Wait()` 等收缩循环退出。注释说明这个等待必须在锁外，因为 `shrink` 内部要拿读锁。
3. **`borrowers.Wait()`** 等所有借出的实例归还。注释给出一条关键事实：`RAGAnalyzer_Copy` 共享模板的 `trie_` / `pos_table_` / `wordnet_lemma_` 但不拥有它们，只独占自己的 OpenCC 转换器——**所以模板必须活到最后一份借用归还**。
4. **再拿写锁，关 channel、排空、关模板**。

`drainInstances` 用非阻塞 `select` 循环排空，且**对已关闭的 channel 也安全**（`ok` 为假就返回）。

**语言状态是「每借必设」。** `withAnalyzer` / `withAnalyzerResult` 在借到实例后无条件 `SetLanguage(lang)`，空字符串映射到 `defaultLanguage = "English"`。注释解释了这个设计的原因：**池化复用会留下上一个任务的语言状态**，不重设就会串。这也是为什么 `SetFineGrained` / `SetEnablePosition` 在池模式下是 no-op——它们改的是实例的持久状态，而实例是共享的。

**`Tokenize` 有两层「入口函数 + 方法」。** 包级 `Tokenize(text)` 转发到 `defaultTokenizer.Tokenize`；方法版先用 `engineType == "infinity"` 短路返回原文（注释：为对齐 Python 行为），再走池。**引擎分支在方法里而不在包级函数里**，所以两个入口行为一致。

**BPE 计数与截断是另一套，且失败方向是 panic。** `getCL100KEncoder` 用嵌入在匿名结构里的 `sync.Once` 做一次加载。`NumTokensFromString` 在编码器不可用时**panic 而不是返回 0**，注释用了很长的篇幅解释理由：返回 0 会让「所有预算都装得下」，这是 fail-open；而实际上曾发生过 Go 镜像漏带 `cl100k_base.tiktoken` 导致所有计数值变成 0、而另一条离线 C++ 分词器照常工作、差异长期未被发现的事故。Python 侧的对应实现也是在 try 之外解析编码器，所以同样会抛错——**这里选择与 Python 行为一致且更响亮**。

`TrimContentToTokenLimit` 同样 panic，且多一层处理：token 边界切出来的字符串可能断在多字节 rune 中间（一个 emoji 四字节可能跨 token），所以先 `clampToValidUTF8` 取最长合法前缀，再在循环里逐个丢尾部 rune 直到重新编码后的 token 数满足上限。注释提到这是被测试 `TestCountersSatisfyTrimProperties` 在 emoji 语料上抓到的真实缺陷。

`EncodeCL100KTokens` 返回 `(tokens, ok)`，编码器不可用时 `ok = false`，让调用方能自己降级而不是把空结果当成「零 token」。

**词频/词性查询用「一次借出取两个值」。** `GetTermFreqAndTag` 缓存到 `termLookupCache`，注释说明三条理由：每次查询都要借池并跨 cgo，而 `Weights` 对每个 token 每次检索的每个 chunk 都要查一次；结果只依赖 term 与 Init 载入的词典；两者来自同一个打包的 trie 条目，所以合并成一个 memo 而不是两个 map。**只缓存成功结果**，注释点明「Init 之前调用不会把 0 钉住」。

`admitTerm` 的容量策略值得看：达到 `1<<16` 时**整体清空而不是冻结**。注释解释：冻结会让先来的条目永久占据预算，后来变热的词永远命中不了且无法恢复；清空只需重新预热少量热点。并且清空发生在插入之前，刚查到的值会存活到新一代。注释也承认这是 check-then-act，并发下会超限、旋转可能丢掉在途插入——「但缓存是有界且近似计数的，这就够了」。

## 关键取舍

**源码事实：** 七个任务状态由 `IsTerminalTaskStatus`（3 个）与 `IsActiveTaskStatus`（4 个）分完，两者各自枚举而非互相取反；`UNSTART` 是派生显示值，只出现在合法过滤集合里。

**源码事实：** `DialogStatus` 是 `string` 的类型别名，唯一常量 `StatusDialogValid = "1"`，注释要求所有 bot 授权路径用它而非字面量。

**源码事实：** `AgentEventSnapshot` 有三组配额（256 节点、深度 8、12000 字节），外层还有 4096 单串、64 项集合、16 KiB 总长三道限制；未知类型返回省略标记而非尽力转换。

**源码事实：** 快照会先把「不超过 16 KiB 且以 `{`/`[` 开头的字符串」尝试解析成 JSON 再递归，目的是让键名级脱敏能作用到内嵌 JSON。

**源码事实：** URL 脱敏清空整个查询串与 fragment，而不只是匹配特定参数名。

**源码事实：** 池的 `acquire`/`release`/`shrink` 全函数持读锁，`Close` 先关 `stopCh`（在写锁之前）再翻状态、等在途借用归零、最后关 channel 与模板。

**源码事实：** 语言在每次借用时重设（空串映射 English），`SetFineGrained` / `SetEnablePosition` 在池模式下是 no-op。

**源码事实：** cl100k 编码器缺失时 `NumTokensFromString` 与 `TrimContentToTokenLimit` 都 panic；`TrimContentToTokenLimit` 还会把结果回退到合法 UTF-8 边界并重新校验 token 数。

**架构推论：** 「状态用裸字符串 + 两个划分函数」牺牲了类型安全，换来跨语言与跨存储的零摩擦。代价是任何新增状态都必须**同时**更新两个函数与 `ValidRunStatuses`，编译器不会提醒。

**架构推论：** 快照函数的设计哲学是「宁可丢信息，不可泄密或超限」。三组配额保证了上界，脱敏两层覆盖了结构与文本，未知类型直接省略。这让「记录任意上下文」这件事变得可预测——**但所有被截断的信息都是永久丢失的**，排查问题时要意识到日志里的 payload 可能不完整。

**架构推论：** 池的锁设计体现了「先唤醒等待者、再夺锁」的顺序智慧。若先拿写锁再关 `stopCh`，卡在 channel 上的借用者会与写者互等，直到超时。**关闭路径的顺序错误不会导致死锁（因为有超时），但会让每次关闭都多花一个完整的获取超时。**

**架构推论：** `NumTokensFromString` 选择 panic 而不是返回 0，是「fail-loud 优于 fail-open」的明确表态。代价是一个漏带文件的部署会直接崩，收益是不会有「所有内容都被认为很短」这种静默的语义错误。**在一个会静默产生错误数据的路径上，崩溃是更好的失败模式。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/common/constants.go` | `DialogStatus` | `dialog.status` 的类型别名，避免裸字符串比较 |
| `internal/common/constants.go` | `StatusDialogValid` | 公开 bot 访问的闸门值，镜像 Python 的 `"1"` |
| `internal/common/constants.go` | `IsTerminalTaskStatus` | 终态判定：完成 / 停止 / 失败 |
| `internal/common/constants.go` | `IsActiveTaskStatus` | 活跃判定：创建 / 已排 / 运行 / 停止中 |
| `internal/common/constants.go` | `ValidRunStatuses` | 过滤参数可接受的摄取状态闭集 |
| `internal/common/constants.go` | `ParentChunkOrderBase` | 源块与父块的排序值域分界 |
| `internal/common/agent_event.go` | `AgentEvent` | 一次节点执行的生命周期记录 |
| `internal/common/agent_event.go` | `AgentEventSnapshot` | 有界、脱敏、不改输入的 JSON 快照 |
| `internal/common/agent_event.go` | `RedactAgentEventText` | 正则脱敏令牌与 URL 查询串 |
| `internal/common/agent_event.go` | `isSensitiveAgentEventKey` | 键名归一后判定是否敏感 |
| `internal/tokenizer/tokenizer.go` | `Tokenizer` | 请求级分词器值类型，只持语言 |
| `internal/tokenizer/tokenizer.go` | `New` | 构造分词器，空语言回退 English |
| `internal/tokenizer/tokenizer.go` | `Tokenize` | 分词；Infinity 引擎直接返回原文 |
| `internal/tokenizer/tokenizer.go` | `analyzerPool.acquire` | 快路径 → CAS 扩容 → 带超时等待 |
| `internal/tokenizer/tokenizer.go` | `analyzerPool.Close` | 先唤醒、再翻状态、等借用归零、最后释放 |
| `internal/tokenizer/tokenizer.go` | `NumTokensFromString` | cl100k 计数，编码器缺失时 panic |
| `internal/tokenizer/tokenizer.go` | `TrimContentToTokenLimit` | 按 token 截断并回退到合法 UTF-8 边界 |
| `internal/tokenizer/tokenizer.go` | `GetTermFreqAndTag` | 一次借出同时取词频与词性，带旋转 memo |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：common/agent_event.go](https://github.com/infiniflow/ragflow/blob/main/internal/common/agent_event.go)
- [ragflow 源码：tokenizer/tokenizer.go](https://github.com/infiniflow/ragflow/blob/main/internal/tokenizer/tokenizer.go)
- [tiktoken-go 文档：cl100k_base 编码](https://github.com/pkoukk/tiktoken-go)

## 与主核 / AI 的关系

「常量 + 划分函数」与主核里「页状态 + 可见性判定」同构：主核用「脏/干净」「已提交/未提交」这类标签决定 WAL 与刷盘行为，判定同样集中在少数几个函数里。差别是主核的状态被类型系统与索引结构双重约束，而这里的七个状态只是字符串——**约束强度换来了跨语言共享的便利**。

快照的有界与脱敏，与主核的「日志脱敏 + 记录长度上限」是同一类工程必需：日志要能长期保存、要能被不同权限的人看，就必须保证它既不含机密也不会无限增长。`ParentChunkOrderBase` 那条注释尤其值得对照主核的「用保留区间区分特殊行」——主核用系统表 ID 段、保留 OID 段来隔离内部对象，与这里用高位偏移隔开源块与父块是同一手法，**而动因同样是「某个引擎对缺失值的默认处理与另一个不同」**。

`NumTokensFromString` 的 panic 选择与主核里「校验和失败必须拒绝读取」一致：**在会静默产生错误结果的路径上，宁可失败得响亮。** 主核页校验和不匹配就报错而不是返回可能被篡改的数据；这里编码表缺失就 panic 而不是把所有计数当 0。

对 agent / RAG 工程的实际含义有四条。其一，事件快照里的 payload 可能被截断，`PayloadTruncated` 为真时不要把它当完整输入做回放。其二，日志与事件里的 URL 查询串会被清空，**排障时不能靠日志里的签名链接复现问题**。其三，分词器池的 `MaxSize` 默认是 16×CPU，并发很高时会动态扩容到上限后开始排队，`AcquireTimeout` 默认 10 秒——超时报错里带着当前大小，可据此判断是池太小还是借用没归还。其四，`cl100k_base.tiktoken` 缺失会导致进程启动即失败（`InitCL100KEncoder` 在启动时调用），这是有意的：它代替了过去「所有 token 数都是 0」的静默事故。
