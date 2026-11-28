---
project: ragflow
feature: blob-storage
title: 对象存储后端
page_type: feature
schema_version: 1
refs:
  - path: internal/storage/memory.go
    symbols: [MemoryStorage, NewMemoryStorage]
  - path: internal/storage/minio.go
    symbols: [MinioStorage, NewMinioStorage]
  - path: internal/storage/oss.go
    symbols: [OSSStorage, NewOSSStorage]
  - path: internal/storage/remove.go
    symbols: [RemoveBlob]
---

# 对象存储后端

## 一句话职责

用一个**十六个方法的 `Storage` 接口**接上五种对象存储后端（MinIO、S3、OSS、GCS、内存测试替身），而全包的 `doc.go` 主动声明了一件反直觉的事：**各后端的键折叠规则是刻意不统一的**，`resolveBucketAndPath` 不许「统一」。

## 机制是什么

**`doc.go` 是这一页最重要的文件，而它几乎全在讲「不要统一什么」。** 它给出的路径折叠契约如下：

| | minio | s3 | oss |
|---|---|---|---|
| `bucket=="" prefix==""` | `fnm` | `fnm` | `fnm` |
| `bucket=="" prefix!=""` | `prefix/fnm` | `prefix/bucket/fnm` | `prefix/fnm` |
| `bucket!="" prefix==""` | `bucket/fnm` | `bucket/fnm` | `fnm` |
| `bucket!="" prefix!=""` | `prefix/bucket/fnm` | `prefix/bucket/fnm` | `prefix/fnm` |

**而紧接的一句是：**

> 这张表就是契约，**不要「统一」它**。

**理由是历史：** 迁移前的 Python 连接器（`rag/utils/minio_conn.py` 与 `rag/utils/s3_conn.py`，均已在 #20289 移除）折叠键的方式不同——**minio 只在单桶模式插入逻辑桶作为路径段，而 s3 只要配了前缀就插入**。而 Go 移植**保留了各家语义**，好让迁移前写入的键在迁移后仍能解析。

**两个后果被显式写出来：**

> - **切换 `STORAGE_ENGINE` 不迁移已有对象键**；同一个 `(bucket, fnm)` 参数在不同后端上解析成不同的键。**把引擎当作每次部署固定。**
> - 在共享桶模式 OSS 无法把删除限定到一个逻辑桶，所以 `RemoveBucket` / `RemoveEmptyBucket` **拒绝执行而不是猜**。

**而「健康检查」也是刻意不统一的：**

> `MinioStorage.Health` 发 `HEAD /minio/health/live` 并要 HTTP 200，**不发 S3 桶请求**。`s3Base` 的健康检查**上传一个探针对象**到 `<bucket 或 "health-check-bucket">/<prefix>/txtxtxtxt1`，与迁移前的 Python S3 连接器一致。**差别是有意的**：写探针也验证写权限，但它**要求**写权限，于是**只读凭证会让 MinIO 风格与 S3 风格的部署报告不同的健康状态。调用方不能把一个引擎的 `false` 与另一个引擎的 `false` 当作可比较。**

**这段话把「不统一」的边界划得极清楚：**

| 维度 | 是否允许统一 | 判据 |
|---|---|---|
| 键折叠 | **否** | 历史数据的存在性 |
| 健康检查语义 | **否** | 它衡量的是不同东西（活性 vs 读写权限） |
| 重试/移动/NotFound 判定 | **是** | 纯逻辑，没有历史包袱 |

**而「是」的那一栏被抽到 `storage_common.go`，`doc.go` 的布局说明把它点出来了：**

> `s3base.go` 放 aws-sdk-go-v2 后端的共享实现（`s3.go` = AWS S3、`oss.go` = 阿里云 OSS）；`storage_common.go` 放**后端无关的重试/移动辅助**，被所有后端使用，**包括 `minio.go`（minio-go SDK）、`gcs.go`（Google SDK）与 `memory.go`（测试替身）**。

**于是这个包有四个抽象层：**

| 层 | 内容 | 适用 |
|---|---|---|
| `Storage` 接口 | 十六个方法 | 全部 |
| `storage_common` | `IsNotFound` / `IsBucketNotFound` / `retryObjectOp` / `moveWithRollback` / `sleepOrAbort` | 全部 |
| `s3Base` | S3 兼容后端的十四个方法 | s3 / oss |
| `MemoryStorage` | 一份完整独立实现 | 测试 |

## 怎么实现的

### 一、`Storage` 接口：十六个方法里有两对「判存在 vs 判错误」

**接口里有四组方法，而其中一组反映了一个反复出现的需求。**

```
ObjExist(ctx, bucket, fnm, tenantID ...string) bool
ObjectExists(ctx, bucket, fnm) (bool, error)
BucketExists(ctx, bucket string) bool
BucketExistsWithError(ctx, bucket string) (bool, error)
```

**四个方法、两对语义：**

| 方法 | 返回 | 「存储不可达」时 |
|---|---|---|
| `ObjExist` | `bool` | 返回 `false` —— **与「不存在」不可区分** |
| `ObjectExists` | `(bool, error)` | 返回 `(false, err)` |
| `BucketExists` | `bool` | 返回 `false` |
| `BucketExistsWithError` | `(bool, error)` | 返回 `(false, err)` |

**而注释直接把这件事说明了：**

> `ObjectExists` 把「对象缺失」与「存储错误」区分开。

**为什么两个版本都要留？** 因为**旧调用方习惯单布尔**（迁移前 Python 的连接器就是这么返回的），而**新的删除/清理路径必须能区分**（否则「网络抖动」会被当成「桶不存在」从而跳过清理）。

**`doc.go` 里对这一点有一句直接的话：**

> `IsBucketNotFound` 报告桶缺失而**不匹配对象级 404**，**这样桶删除就不会把缺失的对象误当缺失的桶**。GCS 的对象迭代器对这种情况返回裸 `googleapi` 404，而对象句柄把它们的 404 包在类型化哨兵里。

**「对象级 404 与桶级 404 的区别」是一个必须显式处理的坑** —— 而两个云后端各有自己的表达方式。

**而 `tenantID ...string` 是可变参数** —— 它出现在几乎所有方法上，但**在 s3base 与 oss 的实现里完全没被使用**。

**这是一个「签名保留但语义已变」的参数**，与 d01 那页 `InitConsumer(subject)` 完全同类：**某个后端（大概是 OpenDAL 或某个多租户后端）曾用它，而当前实现不需要。** 而 `StorageType` 里仍有 `StorageOpenDAL` 这个成员，尽管 `initStorage` 的 `switch` 里没有它。

### 二、`MemoryStorage`：一份完整、正确、且带 `Inspect` 的测试替身

**它的方法数比接口还多一个 —— 多了 `Inspect`。**

```
// Inspect returns a stable snapshot of all (bucket, key, size) entries
// currently held by the backend. Intended for test diagnostics and
// cleanup assertions. The slice is freshly allocated and safe to mutate.
func (m *MemoryStorage) Inspect() []MemoryEntry
```

**「用于测试诊断与清理断言」** —— 于是测试可以断言「这个操作之后存储里应该剩什么」，而不是只知道「Get 通了」。

**而它的数据模型是两层 map + 一把读写锁。**

```
type MemoryStorage struct {
	mu      sync.RWMutex
	objects map[string]map[string][]byte
}
```

**`map[bucket]map[key][]byte` 而不是 `map[(bucket,key)]`** —— 因为 `RemoveBucket` 与 `ListObjects` 是桶级操作。

**四个值得记的实现细节：**

**其一，`Put` 与 `Get` 都做防御性拷贝。**

```
cp := make([]byte, len(binary))
copy(cp, binary)
bucketMap[fnm] = cp
```
```
out := make([]byte, len(data))
copy(out, data)
return out, nil
```

**「存进去的字节是防御性拷贝」** —— 否则调用方之后改自己的切片会污染「存储里的数据」，而这在测试里会变成极难查的串味。

**其二，`Copy` 是唯一一个**分两段加锁**的方法。**

```
m.mu.RLock()
srcBucketMap, ok := m.objects[srcBucket]
...
cp := make([]byte, len(data))
copy(cp, data)
m.mu.RUnlock()

m.mu.Lock()
defer m.mu.Unlock()
destBucketMap, ok := m.objects[destBucket]
if !ok { destBucketMap = make(map[string][]byte); m.objects[destBucket] = destBucketMap }
destBucketMap[destPath] = cp
return true
```

**「先读锁取出并拷贝源数据，释放后再写锁写目标」** —— 这是为了避免**在持有写锁时做长拷贝**，也避免锁升级（Go 的 `RWMutex` 不支持从读锁升级到写锁，那样会死锁）。

**其三，`Move` 把「回滚」委托给共享辅助。**

```
return moveWithRollback(ctx, srcBucket, srcPath, destBucket, destPath,
	func() bool { return m.Copy(ctx, srcBucket, srcPath, destBucket, destPath) },
	func(rctx context.Context) error { return m.Remove(rctx, srcBucket, srcPath) },
	func(rctx context.Context) error { return m.Remove(rctx, destBucket, destPath) },
)
```

**三个闭包：拷贝、删源、删目标（回滚用）。** 于是「移动到一半失败」的处理在五个后端上是同一份代码。

**其四，`Remove` 与 `RemoveBucket` 对「不存在」是幂等的。**

```
bucketMap, ok := m.objects[bucket]
if !ok { return nil }   // Remove
...
delete(m.objects, bucket)  // RemoveBucket
return nil
```

**而 `RemoveEmptyBucket` 会报错：**

```
if len(m.objects[bucket]) != 0 {
	return fmt.Errorf("bucket %s is not empty", bucket)
}
```

**「删非空桶要报错」与「删不存在的桶静默成功」两者都是正确的** —— 前者防止误删数据，后者是幂等性。

**而 `GetPresignedURL` 返回一个假 URL：**

```
return fmt.Sprintf("memory://%s/%s?exp=%d", bucket, fnm, exp), nil
```

**「确定性、非网络的 URL 字符串供测试用」** —— 于是「签 URL」这个能力在测试里也有一条路径，虽然它不真签名。

**一个细节：`Put` / `Remove` / `RemoveBucket` / `RemoveEmptyBucket` / `Copy` / `Move` / `ObjectExists` / `BucketExistsWithError` 都检查了 `ctx.Err()`，而 `Get` / `ObjExist` / `ListObjects` / `GetPresignedURL` / `BucketExists` **没有**。**

**这是一个不一致** —— 大概是「写操作检查、读操作省一次判断」的有意简化，但它让「取消是否能中止一个读」变成不可预期的。

### 三、`RemoveBlob`：一个 51 行的函数，解决了「请求已取消但对象已写出」

**这是 refs 里唯一的免费函数，而它的注释把问题陈述得极清楚。**

```go
// RemoveBlob deletes the object a write already detached from its row, retrying
// on a context that survives the originating request.
//
// It deliberately does NOT use the request context: a cancelled request must not
// leak the blob it already wrote, nor orphan a blob whose row was deleted. The
// retries are bounded by a timeout so a wedged storage SDK cannot block the
// caller forever, and a cancellation of the cleanup context itself is terminal
// rather than retried.
func RemoveBlob(ctx context.Context, impl Storage, bucket, object string) error {
	newCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 30*time.Second)
	defer cancel()

	var lastErr error
	for attempt := 0; attempt < 3; attempt++ {
		if err := impl.Remove(newCtx, bucket, object); err != nil {
			lastErr = err
			// Treat cancellation of the *new* cleanup ctx as terminal.
			if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
				break
			}
			time.Sleep(time.Duration(attempt+1) * 100 * time.Millisecond)
			continue
		}
		return nil
	}
	return lastErr
}
```

**四个决定，每一个都对应一种失败模式：**

**第一，`context.WithoutCancel(ctx)`。**

**Go 1.21 引入的这个函数返回一个「不随父 ctx 取消」的派生 ctx，同时保留所有值。** 于是「请求已经取消」不再阻止清理。

**注释里的两个场景说明了它的必要性：**
- **「已取消的请求不能泄漏它已经写出的 blob」** —— 写成功了，但紧接着的响应构造被取消，此时若用请求 ctx 删不掉。
- **「也不能让一个行已被删除的 blob 变成孤儿」** —— 元数据行删了但对象还在，是**反向**的泄漏。

**第二，`WithTimeout(..., 30*time.Second)`。**

**「重试被超时限制住，这样卡住的存储 SDK 不能永远阻塞调用方」。**

**注意这里的选择：`WithoutCancel` 解决「被取消」，`WithTimeout` 又加回了「有界」。** 两者组合出的语义正是想要的：**不因外部取消而放弃，但不能无限期。**

**第三，三重重试 + 线性退避 `(attempt+1) * 100ms`（100ms、200ms）。**

**这是最简单的退避** —— 没有指数、没有抖动。**对「清理」这个场景是合适的：它不在关键路径上（调用方已经拿到了结果或错误），而总耗时上限是 30 秒。**

**第四，`errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded)` 时 `break`。**

**「把**新**清理 ctx 的取消当成终止而不是重试。」**

**这一行非常关键：** 若 `Remove` 返回的错误是「清理 ctx 自己超时」，重试没有意义（`newCtx` 已死）。**而若把这种情况也当作可重试，三次都会立刻失败，等于白等两次 100/200ms —— 更糟的是它掩盖了「已超时」这个真实原因。**

**而函数最后的返回值是 `lastErr`** —— 注意**可能是 `nil`**：若第一次就成功则 `return nil`（提前返回）。**但若循环跑完三次都失败，`lastErr` 是最后一次的错误。**

**而有一个极端的边界：若 `impl.Remove` 三次都返回 nil 之外的错误且都在 100/200ms 后重试，总共只花 300ms —— 远小于 30 秒的超时。** 于是那两个数字的关系是「超时是大上限，三次重试是小上限」。**两层界限都有必要**：超时防「单次调用卡死」，重试次数防「快速连败」。

**这个函数没有出现在 `s3base` 或任何具体后端里 —— 它是调用方（更高层）用的工具。** 也就是说「删除一个已 detached 的对象」是**业务层的动作**，而存储包只提供工具。

### 四、`moveWithRollback`：五后端共享的「移动到一半」

```
func moveWithRollback(ctx context.Context, srcBucket, srcPath, destBucket, destPath string,
	copyFn func() bool, removeSrc, removeDest func(context.Context) error) bool {
	if !copyFn() {
		return false
	}
	if err := removeSrc(ctx); err != nil {
		common.Warn("Failed to remove source object after copy", ...)
		rollbackCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 30*time.Second)
		defer cancel()
		if rbErr := removeDest(rollbackCtx); rbErr != nil {
			common.Warn("Failed to roll back copied destination object", ...)
		}
		return false
	}
	return true
}
```

**注释说明了它的契约：**

> 拷贝对象，然后删源。**若删源失败，在派生自 `context.WithoutCancel` 的 30 秒 detach 上下文下尽力而为地删掉目标** —— 这样清理能在「删源期间父 ctx 被取消」时存活 —— **然后移动报告失败。**

**三个决定：**

1. **`copyFn` 返回 `bool` 而不是 `error`** —— 因为 `Copy` 在接口里就是 `bool`（所有后端都这样）。**于是「拷贝为什么失败」在这个层已经被丢掉了**（各后端在 `Copy` 内部打了日志）。
2. **回滚用 `WithoutCancel` + 30 秒** —— 与 `RemoveBlob` 完全同一套手法。
3. **回滚失败只 `Warn`** —— 于是「移动失败但目标残留」这个中间状态**不会报错**，只留一条日志。**这是一个必须有人来清理的残留**，而代码承认了它。

**而三个闭包参数的签名不同：`copyFn` 无 ctx（因为它已经捕获了），两个 remove 都要 ctx。** 这是为了让回滚能换 ctx。

### 五、`retryObjectOp`：把「取消优先」写成契约

```
func retryObjectOp(ctx context.Context, attempts int, onRetry func(), op func() error) error {
	var lastErr error
	for i := 0; i < attempts; i++ {
		if err := ctx.Err(); err != nil { return err }
		if err := op(); err != nil {
			lastErr = err
			if cerr := ctx.Err(); cerr != nil { return cerr }
			if onRetry != nil { onRetry() }
			if serr := sleepOrAbort(ctx, time.Second); serr != nil { return serr }
			continue
		}
		return nil
	}
	return lastErr
}
```

**注释给了它的核心契约：**

> 在尝试之间睡一秒并在每次睡眠前调 `onRetry`（例如客户端重连）。首次成功返回 `nil`，尝试耗尽返回最后一个错误。**被取消的 context 在尝试前短路并在退避睡眠中胜出，所以取消总能及时暴露，而不是付一个睡眠窗口的代价 —— 这是 `s3_cancel_test.go` 钉住的契约。**

**「取消在退避睡眠中胜出」由 `sleepOrAbort` 保证：**

```
func sleepOrAbort(ctx context.Context, d time.Duration) error {
	if err := ctx.Err(); err != nil { return err }
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-ctx.Done(): return ctx.Err()
	case <-timer.C:    return nil
	}
}
```

**「可中断的 sleep」** —— 而它是 `select` 而不是 `time.Sleep`，这是唯一正确的写法。

**而「取消优先于重试」需要**三次**检查：循环开头、`op` 失败后、以及睡眠中。** 缺任一处都会让取消被延迟一整秒。

**`onRetry` 的用途在 s3base 里是「重建客户端」：**

```
return retryObjectOp(ctx, 2, func() { b.reconnect(ctx) }, func() error { ... })
```

**「先重连再重试」** —— 因为 aws-sdk 的客户端在长连接失败后可能处于坏状态。**而 `reconnect` 被设计成一个闭包注入到 `s3Base`（见下），正是因为「重建客户端」是各厂商唯一的差异项之一。**

### 六、`s3Base`：五个注入点

**`s3Base` 的注释列出了它要求后端注入的**五件**东西：**

```
//   - client:     a getter returning the CURRENT client, so reconnect swaps
//     are visible to shared methods without stale handles;
//   - resolve:    the bucket/key folding semantics (see doc.go for the
//     deliberately non-uniform per-vendor rules);
//   - makeBucket: bucket creation (AWS pins a LocationConstraint per region,
//     OSS does not);
//   - reconnect:  rebuild the client after a failed attempt;
//   - typeName:   the Type() identifier.
```

**四个要点：**

**第一，`client` 是一个**函数**而不是字段。**

```
client func() *s3.Client
```

**注释的理由极重要：「这样 reconnector 的替换对共享方法可见，不会有陈旧句柄」。**

**若存的是 `*s3.Client` 值，则 `reconnect` 重建客户端后共享方法仍用旧值 —— 于是「重连」等于没做。** 用 getter 让每次调用都取当前客户端。**这是一个「通过共享一个间接层」修复「替换不可见」的标准手法。**

**而 `OSSStorage` 的实现印证了这一点：**

```
storage.s3Base = s3Base{
	client:     func() *s3.Client { return storage.client },
	...
}
```

**它返回**外层的** `storage.client`，于是 `reconnect` 换掉那个字段后所有共享方法都看到新值。

**第二，`resolve` 是被注入的折叠规则** —— 这是 `doc.go` 那张表落地的机制。

**第三，`makeBucket` 的差异被注释点出来了：**

> bucket creation (**AWS pins a LocationConstraint per region, OSS does not**)

**「AWS 要为每个区域钉一个位置约束」** —— 这是 S3 的 API 细节：非 `us-east-1` 的桶创建必须显式给 `LocationConstraint`，否则报错。

**第四，`reconnect` 只在 `Put` / `Get` / `GetPresignedURL` 里被用作 `onRetry`**，而它**重建整个客户端。**

**第五，`typeName` 只是 `Type()` 的返回值。**

**而注释还说明了哪些方法**不**进 `s3base`：**

> 结构与厂商差异较大的方法 —— `connect`、`RemoveBucket`、`RemoveEmptyBucket` 与带版本的对象删除辅助 —— **留在具体后端，覆盖基类本来会提供的任何东西。**

**这划出了一条清晰的界：能共享的是「逐个对象的操作」，不能共享的是「桶级操作与连接建立」。** 而原因可以从 `OSSStorage.RemoveBucket` 的实现看出来：

```
if o.bucket != "" {
	return fmt.Errorf("cannot remove logical OSS bucket %s: shared bucket mode does not isolate objects by logical bucket", bucket)
}
```

**「共享桶模式下逻辑桶没有物理隔离，所以拒绝删除而不是静默清空共享前缀」。**

**这是本页最讲究的一处业务逻辑。** 在共享桶模式下，`bucket` 参数只是一个路径段（`prefix/fnm`），于是「删桶」若真的执行就会删掉**整片共享前缀**。**拒绝执行是唯一正确的选择** —— 而错误消息把原因说清楚了。

**而 `s3Base.Health` 的写探针在共享桶模式下会有副作用：** 它会**创建一个名为 `health-check-bucket` 的桶**（若配置里没给桶名）。

```
bucket := b.bucket
if bucket == "" {
	bucket = "health-check-bucket"
}
```

**「健康检查会建桶」是一个相当强的副作用** —— 而注释（在 `doc.go`）说这与迁移前 Python 连接器一致。

### 七、`MinioStorage`：路径折叠的四处分支与它自己的锁

**`resolveBucketAndPath` 是四段 if/else，正是 `doc.go` 表格的 minio 列。**

```
actualBucket := bucket
if m.bucket != "" { actualBucket = m.bucket }
actualPath := fnm
if m.bucket != "" {
	if m.prefixPath != "" { actualPath = fmt.Sprintf("%s/%s/%s", m.prefixPath, bucket, fnm) }
	else                  { actualPath = fmt.Sprintf("%s/%s", bucket, fnm) }
} else if m.prefixPath != "" {
	actualPath = fmt.Sprintf("%s/%s", m.prefixPath, fnm)
}
return actualBucket, actualPath
```

**注意 minio 是唯一一个「配了桶名就把逻辑桶当路径段」的后端** —— 而 OSS 是唯一一个「配了桶名就不管逻辑桶」的后端。**`doc.go` 说这些差别都来自各厂商的 Python 连接器。**

**而 `Health` 是唯一不用写探针的：**

```
// Health checks endpoint liveness with a read-only probe, not bucket permissions.
func (m *MinioStorage) Health(ctx context.Context) bool {
	probeCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	...
	healthURL := m.client.EndpointURL()
	healthURL.Path = "/minio/health/live"
	req, err := http.NewRequestWithContext(probeCtx, http.MethodHead, healthURL.String(), nil)
	...
	return probeCtx.Err() == nil && resp.StatusCode == http.StatusOK
}
```

**「用只读探针检查端点活性，不检查桶权限」** —— 与 `s3Base` 的写探针形成 `doc.go` 里那段对照。

**而 `healthClient` 是一个**单独的** http.Client，带 `CheckRedirect` 返回 `http.ErrUseLastResponse`：**

```
m.healthClient = &http.Client{
	Transport: transport,
	CheckRedirect: func(_ *http.Request, _ []*http.Request) error { return http.ErrUseLastResponse },
}
```

**「不跟随重定向」** —— 于是 302 不会变成 200，健康检查不会被一个重定向页骗过。

**而 `Put` 里有一个「只有 minio 需要」的建桶逻辑：**

```
if m.bucket == "" {
	exists, err := m.client.BucketExists(ctx, bucket)
	...
	if !exists { if err := m.client.MakeBucket(ctx, bucket, minio.MakeBucketOptions{}); err != nil { ... } }
}
```

**「配了默认桶就不检查/不建桶」** —— 因为此时桶是部署时给定的，不该由代码建。

**而 `Put` 的重试参数是 `retryObjectOp(ctx, 3, nil, ...)`（三次、无 `onRetry`），而 `Get` 是 `(ctx, 2, nil, ...)`（两次）。**

**「写三次、读两次」是一个不对等的选择** —— 写更重要（丢了要重传），读可以失败让上层重试整次请求。

**而 `Remove` 的错误处理把「不存在」压成成功：**

```
code := minio.ToErrorResponse(err).Code
if code == "NoSuchKey" || code == "NoSuchBucket" { return nil }
```

**「按错误码字符串判断」** —— 与 `s3Base` 用 `isS3NotFound(err)` 是同一种意图的两种实现（minio 的 SDK 有结构化错误码，aws-sdk 那边要靠匹配错误类型与 HTTP 状态）。

### 八、统一判定的两处集中

**`storage_common.go` 把「什么是缺失」这件事收成两个函数，而它们是这一页唯一真正跨所有后端的东西。**

```
func IsNotFound(err error) bool {
	if errors.Is(err, ErrMemoryNotFound) || errors.Is(err, gcs.ErrObjectNotExist) || IsBucketNotFound(err) || isS3NotFound(err) {
		return true
	}
	var minioErr minio.ErrorResponse
	if errors.As(err, &minioErr) {
		return minioErr.Code == "NoSuchKey" || minioErr.Code == "NoSuchBucket" || minioErr.Code == "NotFound"
	}
	return false
}
```

**它要同时识别五种来源：** 内存后端的哨兵、GCS 的对象哨兵、桶缺失（递归调用 `IsBucketNotFound`）、S3 的错误、以及 minio 的结构化码。

**而 `IsBucketNotFound` 的存在理由在注释里：**

> 报告桶缺失而**不匹配对象级 404**，**这样桶删除就不会把缺失的对象误当缺失的桶**。GCS 的对象迭代器对这种情况返回裸 `googleapi` 404，而对象句柄把它们的 404 包在类型化哨兵里。

**而它对 `googleapi.Error` 的判断是**兜底的最后一行**：**

```
var apiErr *googleapi.Error
return errors.As(err, &apiErr) && apiErr.Code == http.StatusNotFound
```

**「在 GCS 上这是唯一能识别『桶不存在』的路径」** —— 因为桶迭代器不返回类型化哨兵。

**注意 `IsNotFound` 里调用了 `IsBucketNotFound`** —— 于是「桶缺失」也算「对象缺失」。**这个包含关系是有意的**（对「取对象」来说，桶没了与对象没了都是「取不到」），但它让两个函数不能独立使用：**判桶缺失必须用 `IsBucketNotFound`，判对象缺失可以用 `IsNotFound`。**

## 关键取舍

**源码事实：** `doc.go` 用整张表声明「路径折叠刻意不统一」，并写「这张表就是契约，不要统一它」；理由是迁移前 Python 连接器（minio 只在单桶模式插逻辑桶、s3 配了前缀就插）的语义被 Go 移植保留，好让迁移前写入的键仍能解析。

**源码事实：** `doc.go` 明确两个后果：切换 `STORAGE_ENGINE` 不迁移已有对象键（同一 `(bucket, fnm)` 在不同后端解析成不同键，引擎应视为每次部署固定）；共享桶模式下 OSS 无法把删除限定到逻辑桶，所以 `RemoveBucket` / `RemoveEmptyBucket` 拒绝执行。

**源码事实：** 健康检查也刻意不统一：MinIO 发 `HEAD /minio/health/live`（只读探针、不查桶权限）；`s3Base` 上传探针对象 `<bucket 或 "health-check-bucket">/<prefix>/txtxtxtxt1`（也验证写权限，但**要求**写权限）。`doc.go` 明写「调用方不能把一个引擎的 `false` 与另一个引擎的 `false` 当作可比较」。

**源码事实：** `Storage` 有 16 个方法，其中四组是「判存在」的两对：`ObjExist`（`bool`）对 `ObjectExists`（`(bool, error)`）、`BucketExists` 对 `BucketExistsWithError` —— 后两者的注释明写「把缺失与存储错误区分开」，保留单布尔版是为旧调用方。

**源码事实：** `tenantID ...string` 出现在几乎所有方法上，但在 `s3Base` 与 `oss` 的实现里**完全未被使用**；`StorageType` 仍有 `StorageOpenDAL` 成员，而 `initStorage` 的 `switch` 里没有它。

**源码事实：** `MemoryStorage` 的 `Put` / `Get` 都做防御性拷贝（注释：「存进去的字节是防御性拷贝」）；`Copy` 分两段加锁（先 RLock 拷贝源、释放、再 Lock 写目标）以避免写锁内长拷贝与锁升级；`Move` 委托 `moveWithRollback`；`GetPresignedURL` 返回确定性的 `memory://` 伪 URL；多一个 `Inspect` 供测试断言残留。

**源码事实：** `MemoryStorage` 的写类方法（`Put` / `Remove` / `RemoveBucket` / `RemoveEmptyBucket` / `Copy` / `Move` / `ObjectExists` / `BucketExistsWithError`）都检查 `ctx.Err()`，而读类方法（`Get` / `ObjExist` / `ListObjects` / `GetPresignedURL` / `BucketExists`）**没有**。

**源码事实：** `RemoveBlob` 用 `context.WithTimeout(context.WithoutCancel(ctx), 30*time.Second)` —— `WithoutCancel` 让清理不随请求取消而放弃，`WithTimeout` 又给回一个有界上限；重试三次、线性退避 100ms/200ms；**把「新清理 ctx 自身被取消」当成终止而非重试**。

**源码事实：** `moveWithRollback` 在删源失败时用 `WithoutCancel` + 30 秒的 detach ctx 尽力删掉目标，然后**仍返回 `false`**；回滚失败只 `Warn` —— 于是「移动失败但目标残留」是一个必须由别处清理的中间状态。

**源码事实：** `retryObjectOp` 在**三处**检查取消：循环开头、`op` 失败后、以及 `sleepOrAbort` 的 `select` 里；注释明确「取消在退避睡眠中胜出」，并说这条契约由 `s3_cancel_test.go` 钉住。

**源码事实：** `s3Base` 要求注入五件东西，其中 `client` 是**函数**而非字段 —— 注释理由「这样 reconnect 的替换对共享方法可见，不会有陈旧句柄」；`OSSStorage` 的实现 `func() *s3.Client { return storage.client }` 印证了这一点。

**源码事实：** `s3Base` 只提供「逐对象操作」，而 `connect`、`RemoveBucket`、`RemoveEmptyBucket` 与带版本的对象删除辅助留在具体后端，注释说明理由是「结构上与厂商差异较大」。

**源码事实：** `OSSStorage.RemoveBucket` / `RemoveEmptyBucket` 在共享桶模式下直接**报错拒绝**，理由「共享桶模式不按逻辑桶隔离对象」；而 `s3Base.Health` 在未配桶名时会**建一个 `health-check-bucket`**（写探针的副作用）。

**源码事实：** `MinioStorage.Health` 用独立 `healthClient`，`CheckRedirect` 返回 `http.ErrUseLastResponse`（不跟随重定向，避免 302 被当成 200）；`Put` 只在**未配默认桶**时才 `MakeBucket`；`Put` 重试 3 次、`Get` 重试 2 次且两者都不注入 `onRetry`。

**源码事实：** `MinioStorage.Remove` 用 `minio.ToErrorResponse(err).Code` 匹配 `"NoSuchKey"` / `"NoSuchBucket"` 并返回 `nil` —— 与 `s3Base` 用 `isS3NotFound(err)` 是同一意图的两种实现。

**源码事实：** `IsNotFound` 同时识别 `ErrMemoryNotFound`、`gcs.ErrObjectNotExist`、`IsBucketNotFound`（递归）、`isS3NotFound`、以及 minio 的结构化码；`IsBucketNotFound` 的注释明确它「不匹配对象级 404」，因为「桶删除会把缺失对象误当缺失桶」，而 GCS 的桶迭代器只给裸 `googleapi` 404、对象句柄才有类型化哨兵。

**架构推论：** 「刻意不统一」这件事被写进 `doc.go` 而不是散落在各处，是这个包最值得学的一点。**它的判据不是审美而是「已存在的数据」** —— 键折叠规则一旦改变，迁移前写入的对象就永远取不到，而这类错误在部署之后才暴露、且不可回滚。**把不可统一的契约写成一页文档、并用一张表把每一格都列出来，比在每个实现里加一句注释要可靠得多**，因为它给出了「改这一格会发生什么」的完整视图。

**架构推论：** `RemoveBlob` 是这个包里唯一真正「面向业务」的函数，而它解决的是一类普遍问题：**副作用已经发生、但触发它的上下文已经失效。** `context.WithoutCancel` 是 Go 1.21 才有的正规解法，而在此之前这类清理要么用 `context.Background()`（丢掉所有值）、要么放弃清理。**「取消不该阻止补偿」与「补偿不该无限期」这两个要求同时成立，才需要 `WithoutCancel` + `WithTimeout` 这对组合。**

**架构推论：** `MemoryStorage` 作为测试替身，其价值不在「能跑通」，而在**它能被断言**。`Inspect()` 让测试可以问「存储里现在剩什么」，而不是只知道「某个 key 能取到」。**一个没有内省能力的测试替身，只能验证正向路径；而清理逻辑的错误恰恰永远在「多了什么」上。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/storage/doc.go` | 包文档 | 路径折叠契约表 + 健康检查不统一声明 |
| `internal/storage/types.go` | `Storage` | 十六方法接口（含两对判存在） |
| `internal/storage/types.go` | `StorageType` / `String` | 七个后端标识（含未接线的 `StorageOpenDAL`） |
| `internal/storage/storage_common.go` | `IsNotFound` / `IsBucketNotFound` | 五后端缺失判定集中 |
| `internal/storage/storage_common.go` | `retryObjectOp` | 三处取消检查 + `onRetry` 钩子 |
| `internal/storage/storage_common.go` | `moveWithRollback` | 删源失败时 detach ctx 回滚目标 |
| `internal/storage/storage_common.go` | `sleepOrAbort` | 可中断 sleep（`select` 而非 `time.Sleep`） |
| `internal/storage/storage_factory.go` | `GetStorageFactory` / `Init` / `CloseStorage` | 单例工厂（`sync.Once`） |
| `internal/storage/storage_factory.go` | `initStorage` | `minio` / `s3` / `oss` / `gcs` 四分支 |
| `internal/storage/storage_factory.go` | `SetStorage` | 测试缝 |
| `internal/storage/memory.go` | `MemoryStorage` / `NewMemoryStorage` | 两层 map + 读写锁，完整测试替身 |
| `internal/storage/memory.go` | `Inspect` | `(bucket, key, size)` 快照供断言 |
| `internal/storage/memory.go` | `MemoryEntry` / `ErrMemoryNotFound` | 内省条目与类型化哨兵 |
| `internal/storage/minio.go` | `MinioStorage` / `NewMinioStorage` | 四段路径折叠、只读健康探针、三次/两次重试 |
| `internal/storage/minio.go` | `newMinioTransport` | TLS `InsecureSkipVerify = !cfg.Verify` |
| `internal/storage/minio.go` | `resolveBucketAndPath` | minio 列的四格 |
| `internal/storage/oss.go` | `OSSStorage` / `NewOSSStorage` | 嵌入式 `s3Base` + 共享桶模式拒绝删桶 |
| `internal/storage/oss.go` | `resolveBucketAndPath` | OSS 列的四格（配桶名时不插逻辑桶） |
| `internal/storage/oss.go` | `RemoveBucket` / `RemoveEmptyBucket` | 共享桶模式直接报错 |
| `internal/storage/s3base.go` | `s3Base` | 五注入点：`client` getter / `resolve` / `makeBucket` / `reconnect` / `typeName` |
| `internal/storage/s3base.go` | `Put` / `Get` / `GetPresignedURL` | `retryObjectOp` 的不同次数（2 / 2 / 10） |
| `internal/storage/s3base.go` | `Copy` | `httpbinding.EscapePath` 构造 `CopySource` |
| `internal/storage/s3.go` | `NewS3Storage` / `RemoveBucket` / `removeObjects` | AWS 特有：`LocationConstraint` 与分批删除 |
| `internal/storage/gcs.go` | `NewGCSStorage` / `RemoveBucket` | Google SDK 的独立实现 |
| `internal/storage/remove.go` | `RemoveBlob` | `WithoutCancel` + 30 秒 + 三次重试 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：internal/storage 目录](https://github.com/infiniflow/ragflow/tree/main/internal/storage)
- [Go 文档：context.WithoutCancel](https://pkg.go.dev/context#WithoutCancel)
- [MinIO Go SDK 文档](https://min.io/docs/minio/linux/developers/go/API.html)
- [AWS SDK for Go v2：S3 CopyObject](https://pkg.go.dev/github.com/aws/aws-sdk-go-v2/service/s3#Client.CopyObject)
- [阿里云 OSS 与 S3 兼容说明](https://help.aliyun.com/zh/oss/developer-reference/use-amazon-s3-sdks-to-access-oss)

## 与主核 / AI 的关系

对象存储与主核的**文件管理（`mdnblocksize` / 段文件 / 表空间）** 处在同一个抽象位置：都是「把字节放到某个地方，用键找回」。而两者的键语义正好相反。

**主核的键是 (表空间, 关系, fork, 块号)，语义由系统目录定义** —— 用户不能改，换了存储位置（`ALTER TABLE SET TABLESPACE`）也只是换物理路径，键不变。**而这里的键折叠规则随后端而变，同一个 `(bucket, fnm)` 在不同后端落到不同对象** —— 于是「切换存储引擎」**不是迁移而是毁灭**。

**`doc.go` 对这一点的处理方式值得学：它没有假装键是后端无关的，而是把差异写成一张表并明令不许统一。** 数据库也有类似的诚实：`pg_tablespace_location` 明确告诉你「这个表空间的物理位置是这些」，而不假装「位置无关」。**承认差异比伪装统一安全。**

**`RemoveBlob` 解决的问题在数据库里叫「崩溃后的清理」**：一个操作在提交前留下副作用（临时文件、已写的段），而发起它的会话已经断了。**数据库的解法是 WAL + 启动时的清理扫描**（`xlog` 重放后清临时文件），而 ragflow 的解法是「用一个不随请求取消的短上下文做完删除」。**后者简单得多，因为它没有「需要精确恢复」的要求 —— 删不掉对象的后果是泄漏，不是数据不一致。**

**`retryObjectOp` 的「取消优先于退避」与数据库的锁等待同源**：`LOCK` 的等待必须在 `statement_timeout` 或客户端断开时立即中止，而不是「睡完这一次再检查」。**「可中断的等待」是所有阻塞点都必须实现的语义**，而 Go 里唯一的正确写法就是 `select` 两个 channel。

**`MemoryStorage` 与主核的 `regression` 测试设施（`--temp-config` 的内存表空间、`pg_test` 的临时实例）是同一思路：** 一个**可内省**的测试替身。区别是数据库的测试替身要模拟崩溃，而这里的替身只需要模拟成功与残留。

对 agent 工程的直接价值有三条。其一，**「不可统一的差异」必须被写成文档而不是被静默统一**。键折叠、健康检查语义这类差异一旦「顺手统一」，会导致已经存在的数据取不到，而这类故障在部署后才会暴露。其二，**副作用已发生、上下文已失效时的清理要用 `context.WithoutCancel` + 超时**。用请求 ctx 会导致泄漏，用 `Background()` 会丢掉值，而「不取消但有界」是唯一正确的形态。其三，**测试替身必须有内省能力**。清理逻辑的错误永远表现为「多了一个东西」，而没有 `Inspect()` 的替身只能验证「该有的有了」，永远看不到「不该有的还在」。
