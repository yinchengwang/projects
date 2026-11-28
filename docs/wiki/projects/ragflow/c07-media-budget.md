---
project: ragflow
feature: media-budget
title: 媒体预算与落盘
page_type: feature
schema_version: 1
refs:
  - path: internal/parser/media/budget.go
    symbols: [Budget, Reservation, NewBudget]
  - path: internal/parser/media/sink.go
    symbols: [Sink, NewSink]
  - path: internal/parser/media/limits.go
    symbols: [Limits, LoadLimits]
---

# 媒体预算与落盘

## 一句话职责

给一次解析运行套上三道上限（单图字节、总字节、总张数），按「先准入、后计数、读出来再结算」的流程让解析器**知道该不该下载这张图片**，并把通过的图片写进运行期缓存拿到一个可后取的引用。

## 机制是什么

这里的核心困难是**字节预算必须在不读数据的情况下判断**。而它不可能——真实的图片大小只有下载/解码之后才知道。

所以这套设计用了**两阶段记账**：

| 阶段 | 已知什么 | 能判断什么 |
|---|---|---|
| `Reserve(declared)` | 声明大小（或 `UnknownSize`） | 若声明超限 → 直接拒，**且不占张数** |
| `Commit(actual)` | 真实大小 | 若真实超总预算 → 拒，**但张数已占** |

**「声明超限不占张数」是刻意的**：一个 100 MB 的图被拒后，不该让剩下的 1023 张少一张。而「真实超限占张数」也是刻意的——见下。

**第二个机制是「两个走法都要存在，因为它们对应两种真实的解析器形态」：**

- **`Reserve`（稳定来源）**：同一个 `source` 标识的图片只会被处理一次（`Sink` 里有 `refs` map 做记忆化）。所以「先查声明大小」能省掉重复下载。
- **`ReserveOccurrence`（无来源）**：Office / Markdown 的图片是**顺序流过**的——没有稳定标识可以记住。所以必须**先占张数再判字节**，否则「字节超限被拒」会让后面的文本不处理。

**这个区别就是两个方法各自的注释所表达的**：

> `Reserve` checks declared bytes before reserving a stable source; `ReserveOccurrence` **preserves sequential extraction semantics by consuming the count slot first.**

而 `ReserveOccurrence` 的注释进一步说明：

> The occurrence consumes one item before its actual size is known, matching sequential Office and Markdown extraction where **byte-limit omissions must not stop later text**.

**「字节超限不能让后续文本停」** —— 因为 Office/Markdown 是顺序流：一张图被拒后必须继续处理下一个元素。而若「字节被拒」不占张数，那么一个含 5000 张超大图的 docx 会让循环一直继续到把所有图都试一遍。

**这是一处深刻的取舍**：占张数意味着「失败的尝试也消耗配额」，而这恰好是这个场景想要的——**它把「重复失败」也纳入预算**。

**第三个机制是 `UnknownSize` 的双向传播。**

```go
const (
	// UnknownSize defers the byte check until Reservation.Commit.
	UnknownSize Size = -1
)
```

而 `Budget.bytesExhausted` 这个字段的存在，是为了解决一个具体问题：

> `bytesExhausted` records that an unknown-size payload was rejected by the total-byte limit. **Later unknown-size sources are rejected before their bytes are read**, so a run cannot download past the byte budget.

**「否则后续的未知大小来源都会被下载到超预算为止」** —— 所以一旦「未知大小 + 总预算耗尽」发生过，就把整类来源提前拒掉。

**而它只在「单图不超限但总预算超」时置位**：

```go
if !r.budget.admitBytes(r.item, actual) {
	// An unknown-size payload that fits the per-image limit but not the
	// run budget spends the budget: later sources of unknown size would
	// only be downloaded to fail the same check. A per-image oversize
	// says nothing about the remaining run budget, so it does not set
	// the flag.
	if r.unknown && int64(actual) <= r.budget.limits.ImageMaxBytes {
		r.budget.bytesExhausted = true
	}
	return false
}
```

**「单图超限说明不了剩余预算，所以不置位」** —— 这是一处精确的因果判断。一个 50 MB 的图被单图上限拒掉，不代表总预算没了；而一个 10 MB 的图（单图允许）被总预算拒掉，说明总预算确实耗尽了。

## 怎么实现的

### `Limits`：三个上限，两个来自环境变量

```go
const (
	defaultImageMaxBytes        = 32 << 20     // 32 MB
	defaultMediaMaxImagesPerRun = 1024
	defaultMediaMaxBytesPerRun  = int64(1 << 30) // 1 GB
)
```

```go
func ImageMaxBytes() int {
	if n := common.GetEnvInt(common.EnvMediaImageMaxBytes, 0); n > 0 && n <= int(^uint(0)>>1)/4*3 {
		return n
	}
	return defaultImageMaxBytes
}
```

**四处**：

1. **32 MB / 1024 张 / 1 GB 是一组「够用但不失控」的值** —— 而它们的关系是「1024 × 32 MB = 32 GB ≫ 1 GB」，所以**总字节预算实际上是主要约束**，张数上限只在「大量很小的图」时起作用。

2. **`n <= int(^uint(0)>>1)/4*3` 是一处溢出防护** —— `^uint(0)>>1` 是 `MaxInt`，而 `/4*3` 得到 `MaxInt` 的 3/4。

   **为什么是 3/4？** 因为 `ImageMaxBytes()` 返回 `int` 而被转成 `int64` 使用，后来还会与 `MaxBytesPerRun` 做减法。留 1/4 的余量让后续的加法（`totalBytes += actual`）不溢出 int64。

   **这是一处「用范围收缩换安全」的做法**，而注释没解释 3/4 的来源。

3. **环境变量为 0 或负数时不覆盖** —— 所以「设 0」等于「用默认」，而**没有「设为无限」的表达**。

4. **`MediaMaxBytesPerRun()` 返回 `int64(n)`** —— 而 `GetEnvInt` 返回 `int`，所以**在 32 位平台上上限是 `MaxInt32`**（约 2 GB）。所以「把总预算设成 10 GB」在 32 位平台会失败。

   而 `ImageMaxBytes()` 那个 3/4 判断在 32 位平台上会限制到约 1.6 GB——**远大于默认的 32 MB**，所以实践上无影响。

**`LoadLimits` 是「一次快照」：**

```go
// LoadLimits reads the configured media limits once for a parser run.
func LoadLimits() Limits {
	return Limits{
		ImageMaxBytes:   int64(ImageMaxBytes()),
		MaxImagesPerRun: MediaMaxImagesPerRun(),
		MaxBytesPerRun:  MediaMaxBytesPerRun(),
	}
}
```

**「一次」很重要** —— 因为三个 getter 每次都读环境变量。若在运行中途改变环境变量，`LoadLimits` 的快照保证一次运行内的上限一致。**这是一个「配置读取要快照」的实例**（与 b01 里 `datasetLanguage` 在构造时求值同一手法）。

### `Reserve`：先字节后张数，而顺序是有意的

```go
func (b *Budget) Reserve(declared Size) (*Reservation, Admission) {
	if declared != UnknownSize && declared < 0 {
		return nil, RejectedInvalid
	}
	if declared == UnknownSize && b.bytesExhausted {
		b.noteByteRejection(declared)
		return nil, RejectedByBytes
	}
	if declared != UnknownSize && !b.canAdmitBytes(declared) {
		b.noteByteRejection(declared)
		return nil, RejectedByBytes
	}
	if b.countExhausted || b.itemsUsed >= b.limits.MaxImagesPerRun {
		b.countExhausted = true
		return nil, RejectedByCount
	}
	item := &budgetItem{}
	b.itemsUsed++
	return &Reservation{budget: b, item: item, unknown: declared == UnknownSize}, Admitted
}
```

**六处**：

1. **`declared < 0 && != UnknownSize` 是非法** —— 所以 `-2`/`-5` 都被拒为 `RejectedInvalid`，只有恰好 `-1` 是「未知」。

   **这是一处「哨兵值占用负数区间」的后果**：所有负数都被当作非法，除了那个特定值。

2. **`bytesExhausted` 的检查在最前** —— 且它也 `noteByteRejection(declared)`。**注意此时 `declared == UnknownSize`（即 -1），而 `noteByteRejection(-1)` 里 `int64(-1) > ImageMaxBytes` 为假，所以计进 `totalOverflows`。**

   **这意味着「因预算耗尽被提前拒的未知大小来源」会被算作「达到预算后省略的图片」** —— 语义上是对的。

3. **字节检查在张数检查**之前 —— 而注释明确「A declared size rejected by a byte limit does not consume a count slot」。**所以「先字节后张数」的顺序就是为了实现这条。**

   若反过来，一个超大图会先占掉张数再被拒。

4. **`b.countExhausted` 一旦为真就永久为真** —— 而它同时用作「已发过警告」的标记（`Warnings()` 里判它）。所以它既是状态又是标志。

5. **`ath itemsUsed >= MaxImagesPerRun` 时才 `countExhausted = true`** —— 所以「刚好用满」时置位。这与 `bytesExhausted` 的置位时机不同（那个是在**被拒之后**）。

6. **`item` 是一个指针（`*budgetItem`）** —— 所以 `Reservation` 与 `Budget` 共享它。而 `budgetItem` 有两个字段：`included`（最终是否纳入）与 `charged`（是否已记账）。

   **`charged` 的作用是「幂等」**：`admitBytes` 里 `if item.charged { return false }`，所以同一个 item 第二次调用不会重复记账。

### `ReserveOccurrence`：顺序反过来

```go
func (b *Budget) ReserveOccurrence(declared Size) (*Reservation, Admission) {
	if declared != UnknownSize && declared < 0 {
		return nil, RejectedInvalid
	}
	if b.countExhausted || b.itemsUsed >= b.limits.MaxImagesPerRun {
		b.countExhausted = true
		return nil, RejectedByCount
	}
	item := &budgetItem{}
	b.itemsUsed++
	if declared != UnknownSize && !b.admitBytes(item, declared) {
		return nil, RejectedByBytes
	}
	return &Reservation{budget: b, item: item}, Admitted
}
```

**五处**：

1. **张数先占，然后才判字节** —— 与 `Reserve` 正序相反。而这是刻意的（见机制）。

2. **`b.itemsUsed++` 在字节判断之前** —— 所以即使字节被拒，张数已被消耗。

3. **`admitBytes(item, declared)` 会置 `item.charged`** —— 所以 `Reservation.Commit` 后续会因 `charged` 返回 false。

   **注意此时返回的是 `nil, RejectedByBytes`**（没返回 reservation）—— 所以那个 `charged` 标记实际上不会再用。**这里 `item` 只是被丢弃的中间物。**

4. **`Reservation` 的 `unknown` 字段没设** —— 保持零值 `false`。**所以走 `ReserveOccurrence` 的路径永远不会置 `bytesExhausted`。**

   这与注释一致：`bytesExhausted` 的语义是「未知大小的**稳定来源**被提前拒」——而无来源的 occurrence 没有「提前拒」这个概念（每次都是新的一次流）。

   **不过这也意味着「5000 张未知大小的 Office 图」会一直尝试到张数耗尽的 1024 张**，而总字节预算由 `Commit` 逐张拒。**这就是那 1024 张上限的实际作用。**

5. **`b.bytesExhausted` 完全不被检查** —— 所以 `ReserveOccurrence` 不受它影响。

### `Commit`：幂等且区分两种超限

```go
func (r *Reservation) Commit(actual Size) bool {
	if r == nil || r.budget == nil || r.item == nil {
		return false
	}
	if r.item.included {
		return true
	}
	if r.item.charged || actual < 0 {
		return false
	}
	if !r.budget.admitBytes(r.item, actual) {
		if r.unknown && int64(actual) <= r.budget.limits.ImageMaxBytes {
			r.budget.bytesExhausted = true
		}
		return false
	}
	r.item.included = true
	r.budget.totalBytes += int64(actual)
	return true
}
```

**五处**：

1. **四层守卫**（nil 检查、`included` 早返、`charged`、`actual < 0`）—— 所以它「幂等 for an included item and keeps byte-rejected reservations rejected」（注释语）。

2. **`r.item.included` 为真时返回 true** —— 所以重复 Commit 成功不报错，也不重复记账。**这是幂等的一半。**

3. **`r.item.charged` 为真时返回 false** —— 所以「已字节拒绝」的 Reservation 永久拒绝。**这是幂等的另一半。**

   **两半的组合让 Commit 可以被安全地重试** —— 而这对「先记账后写缓存」的流程是必需的（若写缓存失败想重试）。

4. **`actual < 0` 也拒绝** —— 所以 `Commit(UnknownSize)` 是错的用法（用 `-1` 当未知会走到这里被拒）。

   **这是一处 API 陷阱**：`Size` 类型的哨兵是 `-1`，而 `Commit` 把 `-1` 当非法。所以「未知大小」只能出现在 `Reserve`/`ReserveOccurrence` 的参数里，不能出现在 `Commit` 的参数里。**类型系统没有表达这个区别。**

5. **`totalBytes` 只在成功时累加，而 `admitBytes` 内部不累加** —— 所以记账点集中在 Commit 里，而 `Reserve` 阶段不预扣。**这是「声明大小不预扣」的一致设计**：否则 `Reserve(32MB)` 后再 `Commit(1MB)` 会多扣 31 MB。

   **但这也意味着「同时多个 Reservation 在飞」时总预算会被超额** —— 每个都只看到当前的 `totalBytes`。而 `Sink` 是串行的（注释：「one serial parser invocation」），所以不会发生。

### `noteByteRejection`：两种超限分开计数

```go
func (b *Budget) noteByteRejection(size Size) {
	if int64(size) > b.limits.ImageMaxBytes {
		b.oversizedImages++
		return
	}
	b.totalOverflows++
}
```

**三处**：

1. **两个计数器**：单图超限（`oversizedImages`）与总预算超限（`totalOverflows`）。
2. **`return` 让单图超限不计入 totalOverflows** —— 所以两者互斥。
3. **`size == UnknownSize` 时 `int64(-1) > ImageMaxBytes` 为假** → 计进 `totalOverflows`。**这是前面提到的那条语义。**

**而 `Warnings()` 把三个计数器翻译成人话：**

```go
if b.oversizedImages > 0 {
	warnings = append(warnings, fmt.Sprintf(
		"omitted %d embedded image payload(s) larger than the %d-byte per-image limit",
		b.oversizedImages, b.limits.ImageMaxBytes))
}
if b.totalOverflows > 0 {
	warnings = append(warnings, fmt.Sprintf(
		"omitted %d embedded image payload(s) after reaching the %d-byte run image budget",
		b.totalOverflows, b.limits.MaxBytesPerRun))
}
if b.countExhausted {
	warnings = append(warnings, fmt.Sprintf(
		"stopped extracting embedded images after the %d-item run limit",
		b.limits.MaxImagesPerRun))
}
```

**三处**：

1. **三条警告的措辞区分了三种终止方式**：`omitted ... larger than`（单图超限）/ `omitted ... after reaching`（预算耗尽）/ `stopped extracting ... after`（张数耗尽）。**「omitted」与「stopped」的差别很重要**：前者是「这张被跳过，后面继续」，后者是「整类都停了」。

2. **警告里带上了具体的上限值** —— 所以用户知道该改哪个环境变量。

3. **`Warnings()` 不去重、不排序** —— 三条按固定顺序追加。而它可能被调用多次（`Sink.Warnings` 每次重新生成）——**所以它不是「只报一次」的机制**，与 `countExhausted` 作为标志的用法不同。

### `Sink.Resolve`：先查声明、后读数据、再结算、最后落盘

```go
if ref, seen := s.refs[source]; seen {
	return ref, nil
}
if s.cache == nil {
	return nil, fmt.Errorf("%s media cache is not configured", s.format)
}
s.refs[source] = nil
reservation, admission := s.budget.Reserve(declared)
if admission != Admitted {
	return nil, nil
}
mime, data, ok := load()
if !ok {
	return nil, nil
}
```

**六处**：

1. **记忆化用 `s.refs[source]`，而 `seen` 为真时返回缓存的 ref（**可能是 nil**）** —— 所以「已被拒绝的来源」也会被记住（`nil` 值）。

   **注释说明了这一点**：「Repeated sources reuse both accepted references and recorded omissions」。

   **这是一处重要的语义**：一个被拒的来源不会在第二次引用时重试。而 `delete(s.refs, source)` 在两种情形下会撤销记忆（取消 / 缓存写失败）。

2. **`s.refs[source] = nil` 在 `Reserve` **之前**设置** —— 所以「正在处理中」的标记也用它。若过程中 panic，这个 nil 会留下来，后续同来源会被当作「已拒」。

3. **`s.budget.Reserve(declared)` 的返回值不判 `err`**（因为 `Reserve` 只返回两个值）—— 而 `admission != Admitted` 时返回 `(nil, nil)`：**`nil` error 表示「限额跳过，不是错误」**。

4. **`load()` 返回 `(mime, data, ok)`** —— `ok == false` 表示「读不出来」。此时也返回 `(nil, nil)`，而 `refs[source]` 已是 nil，**所以这次失败被记忆下来了。**

   **但注释说「A false load result records a source omission; the caller reports its format-specific warning」** —— 所以「读不出来」的警告由调用方负责，而 `Sink` 只记录省略。

5. **`s.cache == nil` 时报错而不是跳过** —— 注释在 `CacheFromContext` 上说「Parser paths that emit references must fail rather than silently drop images when it is absent」。**「静默丢图」比「报错」更糟**。

6. **`if err := s.ctx.Err(); err != nil` 在最早处检查** —— 所以取消会优先返回取消错误。

**转码那一段有一处顺序上的细节：**

```go
if int64(len(data)) <= s.budget.limits.ImageMaxBytes {
	if outMIME, outData, transcoded, err := TranscodeImage(s.ctx, mime, data); transcoded {
		if err != nil {
			if ctxErr := s.ctx.Err(); ctxErr != nil {
				delete(s.refs, source)
				return nil, ctxErr
			}
			s.transcodeFailures++
		} else if outMIME != "" && len(outData) > 0 {
			mime, data = outMIME, outData
		}
	}
}
```

**四处**：

1. **转码前先判「原数据是否超单图上限」** —— 注释在测试名里说得很直接：「`TestSinkResolveSkipsTranscodeForOversizedInput`」。**超限的图不浪费 CPU 去转码。**

2. **转码失败不是错误** —— 只 `transcodeFailures++`，然后**保留原始格式**。而警告措辞是「kept N image(s) in original format after transcode failure」。**这是「降级而非失败」的一致策略。**

3. **但「转码期间 ctx 被取消」是错误** —— 且它 `delete(s.refs, source)` 撤销记忆。**所以取消不污染记忆化**（可以让重试重新尝试）。

4. **`outMIME != "" && len(outData) > 0` 才替换** —— 所以「转码成功但输出空」会保留原数据。

**而 `Commit` 用的是**转码后**的 `len(data)`** —— 因为转码可能改变大小（PNG → JPEG 会显著变小）。**所以预算是按「最终落盘的字节」算的，这更准确。**

### `Sink` 的三类失败与警告

```go
func (s *Sink) Warnings() []string {
	warnings := s.budget.Warnings()
	if s.limitSkips > 0 {
		warnings = append(warnings, fmt.Sprintf("%s parser skipped %d image(s) after the run media cache limit", s.format, s.limitSkips))
	}
	if s.transcodeFailures > 0 {
		warnings = append(warnings, fmt.Sprintf("%s parser kept %d image(s) in original format after transcode failure", s.format, s.transcodeFailures))
	}
	return warnings
}
```

**三处**：

1. **`format` 前缀** —— 所以警告能定位到「哪个解析器」。**这是唯一标识调用方的字段。**

2. **`limitSkips` 与 `Budget.Warnings()` 的三种是不同层**：`Budget` 管「准入阶段」的拒绝，而 `limitSkips` 管「缓存写入阶段」的拒绝（`ErrMediaLimitExceeded`）。

   **两层限额的来源不同**：`Budget` 用的是 `Limits`（快照的配置），而 `Cache.Put` 用的是缓存自己的运行期配额。所以**同一张图可能通过 Budget 但被 Cache 拒**。

3. **`transcodeFailures` 是「非致命降级」的计数** —— 与 `limitSkips` 同为「跳过了但不失败」。

**而 `Cache` 的错误分类是明确的：**

```go
var ErrMediaNotFound = errors.New("media: reference not found")
var ErrMediaLimitExceeded = errors.New("media: run media limit exceeded")
```

**两处**：

1. **`ErrMediaNotFound` 的注释强调区分**：「Cache transport, scope, size and digest failures use other errors and **must be returned to the task rather than treated as a missing image**」。

   **「缺失」与「读失败」必须区分** —— 因为前者可能是正常的（图片被清理了），后者是故障。

2. **`ErrMediaLimitExceeded` 是唯一被 `Sink` 吞掉的错误** —— 其他都返回给调用方。

**而 `ValidatePayload` 做三重校验：**

```go
func ValidatePayload(data []byte, ref entity.MediaRef) ([]byte, error) {
	if len(data) > ImageMaxBytes() { ... }
	sum := sha256.Sum256(data)
	if got := hex.EncodeToString(sum[:]); got != ref.SHA256 { ... }
	if ref.Size > 0 && int64(len(data)) != ref.Size { ... }
	return data, nil
}
```

**三处**：

1. **大小上限、摘要、长度三重校验** —— 所以「缓存被换过内容」或「引用的摘要是假的」都能被发现。

2. **`ref.Size > 0` 才校验长度** —— 所以「未知大小」（0）时不判。

3. **它读的是 `ImageMaxBytes()` 而**不是**快照的 `Limits`** —— **这是一处不一致**：`Budget` 用快照，而这里每次都读环境变量。若运行中环境变量被改，两者会不同。

## 关键取舍

**源码事实：** 三道上限的默认值是 32 MB / 1024 张 / 1 GB（`32 << 20` / `1024` / `1 << 30`）。因为 1024 × 32 MB = 32 GB ≫ 1 GB，所以**总字节预算是主要约束，张数上限只在「大量小图」时生效**。

**源码事实：** 三个上限都从环境变量读，`0` 或负数表示「不覆盖」（**没有「设为无限」的表达**）；`ImageMaxBytes` 额外把值限制在 `MaxInt` 的 3/4 以内（为后续 int64 运算留余量），而注释没解释 3/4 的来源。

**源码事实：** `LoadLimits` 是「一次运行一次快照」，因为三个 getter 每次都读环境变量——快照保证一次运行内上限一致。

**源码事实：** 两种准入路径存在是为了对应两种解析器形态：`Reserve`（稳定来源，可记忆化，**先判字节后占张数**——「A declared size rejected by a byte limit does not consume a count slot」）与 `ReserveOccurrence`（无来源的顺序流，**先占张数后判字节**——「byte-limit omissions must not stop later text」）。

**源码事实：** 「失败的尝试也消耗张数」这个取舍在顺序流场景是想要的：否则 5000 张超大图会全部被试一遍。

**源码事实：** `UnknownSize` 是 `Size(-1)`，而**所有其他负数都是 `RejectedInvalid`**——所以哨兵占用了负区间，只有恰好 `-1` 是「未知」。

**源码事实：** `Commit(UnknownSize)` 会因 `actual < 0` 被拒——**所以 `-1` 只能出现在 `Reserve`/`ReserveOccurrence` 的参数里，不能出现在 `Commit` 里；类型系统没有表达这个区别。**

**源码事实：** `bytesExhausted` 只在「未知大小 **且** 单图不超限 **但** 总预算超」时置位——因为「A per-image oversize says nothing about the remaining run budget」。它的作用是「后续未知大小来源在读字节之前就被拒」，避免「一直下载到超预算」。

**源码事实：** `ReserveOccurrence` 不检查也不置 `bytesExhausted`（`Reservation.unknown` 保持 false），所以那条路径没有「提前拒」的概念。

**源码事实：** `Commit` 是双向幂等：已纳入的重复 Commit 返回 true 且不重复记账；已字节拒绝的永久返回 false。`charged` 标记保证 `admitBytes` 不重复记账。

**源码事实：** `totalBytes` 只在 `Commit` 成功时累加，`Reserve` **不预扣**——所以「声明大小」不占用预算。代价是「多个 Reservation 并发在飞时会超额」，但 `Sink` 是串行的（「one serial parser invocation」），所以不会发生。

**源码事实：** `noteByteRejection` 把超限分两类计数（`oversizedImages` 单图超限 / `totalOverflows` 总预算超限），互斥；`size == UnknownSize` 时 `int64(-1) > ImageMaxBytes` 为假，所以「因预算耗尽被提前拒的未知来源」计入 `totalOverflows`。

**源码事实：** `Warnings()` 三种措辞区分三种终止方式（`omitted ... larger than` / `omitted ... after reaching` / `stopped extracting ... after`），且都带上具体的上限值；它不去重、不排序，可被多次调用重复生成（与 `countExhausted` 作为「已报过」标志的用法不同）。

**源码事实：** `Sink.refs` 的 `nil` 值同时表示「已拒绝」与「处理中」；`Resolve` 在 `Reserve` 之前就写 `nil`，所以 panic 会让该来源永久被判为已拒。而 `delete(s.refs, source)` 只在「取消」与「缓存写失败」两处发生——**所以这两种失败可以被重试，而「限额」与「读不出」不会。**

**源码事实：** `Sink.Resolve` 的错误分类：`admission != Admitted` 与 `load` 失败都返回 `(nil, nil)`（**限额跳过不是错误**）；`cache == nil` 是错误（「Parser paths that emit references must fail rather than silently drop images」）；`ErrMediaLimitExceeded` 被吞成 `limitSkips`；其余缓存错误返回给调用方。

**源码事实：** 转码在「原数据超单图上限」时被跳过（省 CPU）；转码失败只计数并保留原始格式（`transcodeFailures`）；但「转码期间 ctx 取消」是错误且会撤销记忆化。

**源码事实：** `Commit` 用的是**转码后**的 `len(data)`——所以预算按「最终落盘的字节」算（PNG → JPEG 会显著变小）。

**源码事实：** 两层限额来源不同：`Budget` 用 `LoadLimits()` 的快照，而 `Cache.Put` 用它自己的运行期配额——**同一张图可能通过 Budget 但被 Cache 拒**，因此 `limitSkips` 与 `Budget.Warnings()` 是不同层的计数。

**源码事实：** `ValidatePayload` 做大小/摘要/长度三重校验，而它读的是**实时的 `ImageMaxBytes()`** 而非快照的 `Limits`——与 `Budget` 的快照语义不一致。

**源码事实：** `ErrMediaNotFound` 与「缓存读失败」必须区分（「must be returned to the task rather than treated as a missing image」），因为前者可能正常、后者是故障。

**架构推论：** 「声明大小不预扣、真实大小才记账」这个两阶段设计是这类「未知实际大小时要限流」问题的标准解。它的代价是「并发在飞时会超额」——而这份实现用「串行」来回避，而不是用「预留」。**判据是「并发度是否可控」**：串行则不需要预留，并发则必须预留（否则上限只是软约束）。

**架构推论：** 两条准入路径的存在揭示了一条容易被忽略的事实：**「无稳定标识的顺序流」与「有稳定标识的可记忆化来源」需要不同的限流顺序**。前者必须「先占配额再判大小」（否则失败不消耗配额，写循环会无限重试），后者必须「先判大小再占配额」（否则一次超大就白白吃掉一张配额）。**判据是「失败是否会导致同一位置被反复尝试」**。

**架构推论：** `bytesExhausted` 这个字段解决的是一个**具体的性能问题而不是正确性问题**：不置位的话结果仍然正确（每张都会被 Commit 拒），但会**把所有未知大小的图都下载一遍才发现超预算**。所以这是一个「用一次状态位把 O(n) 的无效下载变成 O(1) 的提前拒绝」的优化。**判据是「无效工作的代价是否值得一个状态位」**——网络下载值得。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/parser/media/limits.go` | `Limits` | 三道上限的快照 |
| `internal/parser/media/limits.go` | `LoadLimits` | 一次运行一次读取 |
| `internal/parser/media/limits.go` | `ImageMaxBytes` | 单图上限（含 3/4 `MaxInt` 的溢出防护） |
| `internal/parser/media/limits.go` | `MediaMaxImagesPerRun` / `MediaMaxBytesPerRun` | 张数与总字节上限 |
| `internal/parser/media/limits.go` | `defaultImageMaxBytes` / `defaultMediaMaxImagesPerRun` / `defaultMediaMaxBytesPerRun` | 32 MB / 1024 / 1 GB |
| `internal/parser/media/budget.go` | `Size` / `UnknownSize` | 字节数类型与未知哨兵（-1） |
| `internal/parser/media/budget.go` | `Admission` | 四种准入结论（含三种拒绝理由） |
| `internal/parser/media/budget.go` | `Budget` | 一次运行的记账（含两个 exhausted 标志） |
| `internal/parser/media/budget.go` | `Reservation` / `budgetItem` | 准入句柄（`included` / `charged` 两态） |
| `internal/parser/media/budget.go` | `NewBudget` / `NewBudgetWithLimits` | 两种构造（快照 / 显式上限） |
| `internal/parser/media/budget.go` | `Budget.Reserve` | 稳定来源：先字节后张数 |
| `internal/parser/media/budget.go` | `Budget.ReserveOccurrence` | 无来源顺序流：先张数后字节 |
| `internal/parser/media/budget.go` | `Reservation.Commit` | 幂等结算（含 `bytesExhausted` 置位） |
| `internal/parser/media/budget.go` | `Budget.canAdmitBytes` / `admitBytes` | 单图与总预算双重判断 |
| `internal/parser/media/budget.go` | `noteByteRejection` | 两类超限分开计数 |
| `internal/parser/media/budget.go` | `Budget.Warnings` | 三条措辞区分的警告 |
| `internal/parser/media/sink.go` | `Sink` | 「准入 + 转码 + 落盘 + 记忆化」四合一 |
| `internal/parser/media/sink.go` | `NewSink` | 绑定 ctx / cache / format / budget |
| `internal/parser/media/sink.go` | `Sink.Resolve` | 主流程（含记忆化与取消撤销） |
| `internal/parser/media/sink.go` | `Sink.Warnings` | `Budget` 警告 + 两层跳过 + 转码降级 |
| `internal/parser/media/sink.go` | `Sink.Lookup` / `Sink.Skip` | 免重复校验 / 显式记录不可读来源 |
| `internal/parser/media/cache.go` | `Cache` | `Put`/`Get` 两方法接口 |
| `internal/parser/media/cache.go` | `ErrMediaNotFound` / `ErrMediaLimitExceeded` | 「缺失」与「限额」必须区分的两个哨兵 |
| `internal/parser/media/cache.go` | `WithCache` / `CacheFromContext` | 运行期缓存的 context 传递 |
| `internal/parser/media/cache.go` | `ValidatePayload` | 大小/摘要/长度三重校验 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：图片与多模态摄取](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：RAG 中的多模态文档处理](https://arxiv.org/abs/2404.13919)
- [Go 文档：context 值传递的约定](https://pkg.go.dev/context)

## 与主核 / AI 的关系

「两阶段记账（声明 → 真实）」与主核（多模态数据库内核）的**工作内存（work_mem）限额**是同一类问题：都要在「实际用量未知」时保证不超总量。而主核的解法是**运行期自适应**——排序/哈希连接在用量达到 `work_mem` 时切换到磁盘溢出（`external sort`），所以它不需要「事前声明」。

**这个差别来自两种操作的可逆性**：数据库的排序可以中途落盘继续；而这里「下载了 100 MB 图片才发现超预算」是不可逆的浪费。**判据是「超限后能否优雅降级」**：能则运行期检查，不能则必须事前准入。

第二处对照是 `ErrMediaNotFound` 与 `ErrMediaLimitExceeded` 必须区分，与主核区分**「行不存在」与「读失败」**（`SnapshotTooOld` / `SerializationFailure` 都是错误，而「没有匹配行」不是）。两者都是同一个纪律：**「没有」是一个正常的业务结果，「读不出来」是一个故障**。混淆这两者的后果是「故障被静默当成空数据」。

第三处是 `LoadLimits` 的「一次快照」与主核的**GUC 参数在事务内的固定**。主核的 `SET LOCAL` 让参数在一个事务内生效且不回滚可见性；而全局 GUC 在运行期可变但已在执行的计划不会重新读取。两者都是「配置读取要有一个明确的时机点」。**判据是「运行中途改配置是否该影响正在进行的操作」**——两份实现都答「不该」。

对 RAG 工程的实际意义在于：多模态摄取是 RAG 里唯一「用户上传的文件可以任意大」的环节，所以限额不是可选的美化而是可用性的前提。这份实现给出的三条经验可直接用：**未知大小时要两阶段记账**（声明阶段判明显超限，结算阶段判总量）、**顺序流与可记忆化来源需要相反的准入顺序**（否则要么漏配额要么白白消耗）、以及**「没有」与「读不出来」必须是不同的错误**（否则故障会被静默当成空数据）。
