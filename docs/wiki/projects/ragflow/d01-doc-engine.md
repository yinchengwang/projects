---
project: ragflow
feature: doc-engine
title: 文档引擎抽象
page_type: feature
schema_version: 1
refs:
  - path: internal/engine/engine.go
    symbols: [EngineType, DocEngine, MessageQueue]
---

# 文档引擎抽象

## 一句话职责

用一个**三十二个方法的 `DocEngine` 接口**把七种物理后端（Elasticsearch、OpenSearch、Infinity、OceanBase/SeekDB、SereneDB、Vastbase）收进同一个契约，并把「引擎家族」这个概念用两个谓词函数显式编码——因为这套接口的真实难点不是方法多，而是**同一个语义在不同后端上有不同的实现可能性**。

## 机制是什么

`DocEngine` 的方法可以按用途分成五组，而分组方式本身说明了 ragflow 把什么算作「文档引擎」：

| 组 | 方法数 | 用途 |
|---|---|---|
| **块存储** | 8 | `CreateChunkStore` / `InsertChunks` / `UpdateChunks` / `DeleteChunks` / `Search` / `GetChunk` / `DropChunkStore` / `ChunkStoreExists` |
| **文档元数据存储** | 8 | `CreateMetadataStore` / `InsertMetadata` / `UpdateMetadata` / `DeleteMetadata` / `DeleteMetadataKeys` / `DropMetadataStore` / `MetadataStoreExists` / `SearchMetadata` |
| **自由文档索引** | 3 | `IndexDocument` / `DeleteDocument` / `BulkIndex` |
| **结果集后处理** | 6 | `GetFields` / `GetAggregation` / `GetHighlight` / `GetChunkIDs` / `KNNScores` / `GetScores` |
| **能力与运维** | 5 | `RunSQL` / `Ping` / `Close` / `GetType` / `SupportsPageRank` |

**前两组是「真后端操作」，而第三到第五组暴露了这个抽象的一个根本妥协。**

`GetFields` / `GetAggregation` / `GetHighlight` / `KNNScores` / `GetScores` 这几个方法**不访问任何后端** —— 它们是对已经取回的结果集做内存加工。放在接口里的原因是**这些加工逻辑与后端的返回格式绑定**：ES 的 highlight 结构、Infinity 的 `SCORE` 列、两者的聚合结果形状都不同，所以「怎么从这份结果里抽出字段」必须由后端自己回答。

**这是一个「把格式知识封在后端里」的选择**，代价是 `DocEngine` 不再是一个纯 I/O 接口 —— 它一半是驱动器、一半是结果解析器。

**七种引擎实际上只有四个实现谱系：**

| 谱系 | 成员 | 本质 |
|---|---|---|
| Lucene 系 | `elasticsearch`、`opensearch` | 两个几乎同构的 HTTP JSON 后端 |
| 列式 SQL + 向量索引 | `infinity` | 自带 HNSW + 全文索引的独立引擎 |
| SQL 系（OceanBase 家族） | `oceanbase`、`seekdb` | 共享同一套 SQL 实现 |
| SQL 系（独立） | `serenedb`、`vastbase` | 各自独立 |

**而第一个谱系的两个成员是这一页最重要的观察点：** 两个后端各有近三千行的 `chunk.go`，函数清单几乎逐条对应，连 `SearchResponse` 结构体的注释都一字不差。它们之所以不合并，`esoscommon` 包的注释给了直接答案：

> `SanitizeString` 之前在这两个引擎里逐字节重复；现在两边都调这份共享实现，**这样两个后端就不会漂移**。

**也就是说：共享的是「能共享的」，剩下的是「故意不同的」。**

## 怎么实现的

### 一、`EngineType`：七个值，两组谓词

```
const (
	EngineElasticsearch EngineType = "elasticsearch"
	EngineOpenSearch    EngineType = "opensearch"
	EngineInfinity      EngineType = "infinity"
	EngineOceanBase     EngineType = "oceanbase"
	EngineSeekDB        EngineType = "seekdb"
	EngineSereneDB      EngineType = "serenedb"
	EngineVastbase      EngineType = "vastbase"
)
```

**七个值全部是字符串，而它们被两个谓词函数重新分组。**

```
func IsOceanBaseFamily(engineName string) bool {
	return engineName == string(EngineOceanBase) || engineName == string(EngineSeekDB)
}
```

**为什么 `OceanBase` 与 `SeekDB` 要算一个家族？** 因为它们**共享同一套 SQL 实现**。从 `InitDocEngine` 的分派能看出来：两者走的是同一个 `case`，且都通过 `ResolveOceanBaseConnection(engineType)` 拿连接配置，最后 `oceanbase.NewEngine(engineType, connectionConfig)` —— 引擎名被当成参数传进去，而不是选不同的构造函数。

**而 `IsVastbase` 单独存在，注释把为什么不能合进去说得很清楚：**

> Vastbase 是基于 PostgreSQL 协议的，并且在服务层与 OceanBase 家族共享若干行为（本地重排、select 字段里的块向量、共享表的内存生命周期），**但它是一套独立的 SQL 实现，所以保留自己的谓词而不加入 `IsOceanBaseFamily`。**

**三个层次被这段话区分开了：**

| 层次 | 判据 | 例子 |
|---|---|---|
| 共享**实现** | 同一份代码 | OceanBase + SeekDB |
| 共享**行为** | 需要同样的调用方分支，但代码不同 | Vastbase vs OceanBase 家族 |
| 共享**协议** | 同一套线协议（PG wire） | Vastbase（独立实现但仍被单独识别） |

**这是一个值得记的设计：谓词函数的名字在描述「哪些调用方分支该走同一条路」，而不在描述「哪两个后端代码相似」。** 若把 Vastbase 塞进 `IsOceanBaseFamily`，所有「共享行为」的分支会正确命中，但任何「共享实现」的分支会错。

**`Type` 是一个绕过循环依赖的适配器。**

```
func Type(docEngine DocEngine) EngineType {
	if docEngine == nil {
		return EngineType("unknown")
	}
	return EngineType(docEngine.GetType())
}
```

**注释解释了它为什么存在：**

> 这个变通是因为我们不能直接 import elasticsearch 或 infinity 包。

**也就是说：`DocEngine` 接口不能返回 `EngineType`（那会让 `engine` 包依赖各后端包来构造返回值），于是接口里只有 `GetType() string`，由这个免费函数转成枚举。** 这是一个**用一层转换换掉一次循环依赖**的典型手法。

**注意 `nil` 返回 `"unknown"` 而不是空字符串** —— 于是「未初始化」与「类型是空」可区分。

### 二、`language` 参数：三个方法都带它，而只有部分引擎在乎

**`CreateChunkStore`、`InsertChunks` 都有 `language` 参数，而接口文档把这件事写得很直白。**

```
// CreateChunkStore creates the chunk store for a dataset. language is the
// dataset language, needed by engines whose fulltext analyzer is fixed when
// the index is created; the others ignore it.
```

**「全文分析器在索引创建时就固定的那些引擎需要它，其余忽略」。**

**`Infinity` 的代码印证了这句话。** 它的 `CreateChunkStore` 把 `language` 一路传到建表逻辑，最终进入 `analyzerForLanguage`：

```
func analyzerForLanguage(analyzer, language string) string {
	key := strings.ToLower(strings.TrimSpace(language))
	if _, ok := analyzerLanguages[key]; !ok {
		return analyzer
	}
	if base, _, _ := strings.Cut(analyzer, "-"); base != ragAnalyzer {
		return analyzer
	}
	return analyzer + "-" + key
}
```

**而 `analyzerLanguages` 只有两个成员：**

```
var analyzerLanguages = map[string]struct{}{
	"czech":  {},
	"slovak": {},
}
```

**为什么只有捷克语和斯洛伐克语？注释给的理由很具体：**

> Infinity 的分析器会把它们的变音符折成 ASCII 并且跳过词干化，所以这两种语言的数据集必须通过 `rag-<language>` 来索引与查询，而不是默认的 `rag-coarse`/`rag-fine` 那一对。
>
> **它们是仅有的两种分析器与默认不同的语言**：其它任何名字要么选中一个 Snowball 词干器（english、dutch、german…），要么是空操作（中文、日文、韩文用的是词典分词而不是词干化）。**把某个词干器写在这里会让该语言的每个新建数据集都切换到它，那本身就是一个行为变更。**

**最后一句是这条设计的完整理由：这是一个极小的白名单，而它的窄是刻意的。** 加一个语言进这个 map 不是「支持一种新语言」，而是「改变该语言所有新建数据集的行为」。

**而 ES 侧则完全忽略 `language`，因为它的分析逻辑在客户端。**

```
// language is the dataset language, which only engines whose fulltext analyzer
// is fixed at index creation need; Elasticsearch analyses client-side and
// ignores it.
```

**「ES 在客户端分析」—— 它拿到的 `chunks` 里已经有 `content_ltks`（tokenize 后的词项）这类字段，全文索引只是把现成的词项存进去，不需要服务端分析器。** 于是同一个接口参数在两个谱系上是「必需」与「无意义」的差别。

**这是一个「接口必须容纳后端能力差异」的实例**：参数在签名里存在，但它的语义强度取决于后端。

**而 `Infinity` 的 `InsertChunks` 里有一行注释点出了更麻烦的一点：**

```
// Create table. This is the path the ingestion pipeline actually takes,
// so the dataset language has to reach it here.
```

**也就是说：`CreateChunkStore` 不是摄取管线的实际入口** —— 表通常在第一次写入时才被创建（`InsertChunks` 发现表不存在就自己建）。**于是 `language` 必须在两条路径上都能传到建表点，否则「语言只在显式建表时生效」。

### 三、`MessageQueue`：与 `DocEngine` 并列但在同一个文件里

**它也在 `engine.go` 里，而它与 `DocEngine` 毫无关系 —— 它管的是任务消息。**

方法可以分三段：

**第一段：任务队列。**
```
Init() error
Type() string
InitConsumer(subject string) error
PublishTask(subject string, payload []byte) error
PullMessages(ctx, messageCount int) ([]common.TaskHandle, error)
PullMessage(ctx) (common.TaskHandle, error)
ListMessages(messageType string, pending bool) ([]map[string]string, error)
ShowMessageQueue() (map[string]string, error)
CheckStatus() string
```

**`InitConsumer` 的注释记录了一次接口演化：**

> `subject` 参数是从单通道契约保留下来的，但**被忽略**：引擎按版本专属的优先级通道各建一个持久消费者 —— **企业版定义自己的通道集合，社区版用单通道。**

**这是一个「签名保留但语义已变」的参数。** 它与 `_get_method` 那类问题同源：**改签名会破坏调用方，于是旧参数留下来变成噪音。** 而注释承担了「它为什么还在」的解释职责。

**第二段：知识编译的消费者面。**
```
InitKnowledgeCompileStream() error
InitKnowledgeCompileConsumer() error
PublishKnowledgeCompile(subject string, payload []byte) error
FetchKnowledgeCompileMessages(n int) ([]common.RawMessage, error)
```

**第三段：租约（lease），这是最有意思的一段。**
```
InitKnowledgeCompileLeases() error
AcquireKnowledgeCompileLease(key, holder string, ttl time.Duration) (uint64, bool, error)
HeartbeatKnowledgeCompileLease(key, holder string, ttl, revision uint64) (uint64, bool, error)
ReleaseKnowledgeCompileLease(key, holder string, revision uint64) error
```

**三个参数揭示了完整的租约协议：**

| 参数 | 作用 |
|---|---|
| `key` | 租约标识（大概是被编译的数据集） |
| `holder` | 谁持有（节点标识） |
| `ttl` | 租约时长 |
| `revision` | **带版本的 CAS 令牌** |

**`Acquire` 返回 `(uint64, bool, error)`：`revision` + 「是否抢到」+ 错误。`Heartbeat` 也返回 `(uint64, bool, error)`：新 revision + 「是否续成」+ 错误。**

**而 `Release` 要求传 `revision`** —— 于是「释放」是一次**带版本检查的删除**：若别人已经在你持有期间抢走了租约（revision 变了），你的释放会失败而不是把别人的租约删掉。

**这是 f01 那页 `SQLiteCache._delete_if_unchanged` 的同一种手法**：把「删除」变成「仅当它仍是我持有的那一份」。**区别在于这里用显式 revision 而不比值内容。**

**而 `SubscribeNotify` 上有一条原则性的注释。**

```
// SubscribeNotify returns a channel of dataset ids pushed by
// PublishKnowledgeCompile on the notify.kc.workers subject (Option E
// *11.4: NATS as a wake-up, MySQL as the scheduling system of record).
```

**「NATS 当闹钟、MySQL 当调度真相」** —— 这是一条很重要的架构定位：**消息队列不承载真相，只承载唤醒。** 于是「推送丢了」不会导致任务丢失（定期轮询会兜住），而实现也允许返回 `(nil, nil)` 表示「推送不可用」。

**这一点在 `MessageQueue` 的三段里是一致的：任务队列承载真相（`PullMessages` 是拉取式的），而 notify 通道只是加速。**

### 四、工厂：`once` + `switch` + 一次 tokenizer 耦合

**`InitDocEngine` 用 `once.Do` 包住整个初始化，而里面有一行横向耦合值得单独看。**

```
once.Do(func() {
	globalConfig := server.GetConfig()
	engineType = globalConfig.DocEngineType()
	tokenizer.SetEngineType(engineType)
	var err error
	switch engineType {
	case "elasticsearch": globalEngine, err = elasticsearch.NewEngine(ctx, globalConfig.GetElasticsearchConfig())
	case "opensearch":    globalEngine, err = opensearch.NewEngine(ctx, globalConfig.GetOpenSearchConfig())
	case "infinity":      globalEngine, err = infinity.NewEngine(ctx, globalConfig.GetInfinityConfig())
	case "oceanbase", "seekdb":
		connectionConfig, resolveErr := globalConfig.ResolveOceanBaseConnection(engineType)
		if resolveErr != nil { err = resolveErr } else { globalEngine, err = oceanbase.NewEngine(engineType, connectionConfig) }
	case "serenedb":      globalEngine, err = serenedb.NewEngine(globalConfig.GetSereneDBConfig())
	case "vastbase":      globalEngine, err = vastbase.NewEngine(globalConfig.GetVastbaseConfig())
	default:              err = fmt.Errorf("unsupported doc engine type: %s", engineType)
	}
	if err != nil { initErr = fmt.Errorf("failed to create doc engine: %w", err); return }
	registerChunkHistory(globalEngine)
	registerMetadataHistory(globalEngine)
})
```

**四个要点：**

**第一，`tokenizer.SetEngineType(engineType)` 在创建引擎之前调用。**

**这是横向耦合**：分词器要按引擎调整行为（不同后端对「词项」的表示不同），而引擎本身还不知道自己是谁。**执行顺序因此变成隐式约定** —— 若把这一行移到 `switch` 之后，分词器的早期调用（若存在）就会用错模式。

**第二，`initErr` 在闭包外声明、闭包内赋值。**

**这是 `sync.Once` 的经典陷阱处理方式**：`once.Do` 无法返回值，于是错误要通过闭包变量传出。而在 `once.Do` 之后的调用者一定看到这个错误（因为第一次执行已经完成）。**代价是错误被缓存** —— 第二次调用 `InitDocEngine` 会立即返回同一个 `initErr`，不会重试。

**这也是 `sync.Once` 的正确语义**：初始化只做一次，成败都定。

**第三，`registerChunkHistory` / `registerMetadataHistory` 在社区版是空操作。**

```
//go:build !ee
func registerChunkHistory(_ DocEngine) {}
```

**两个函数用 build tag `!ee` 做版本隔离** —— 企业版提供块与元数据的历史版本功能。**这是一个「接口在社区版保留、实现在企业版」「的钩子模式**，且它**不是通过运行时配置而通过编译期分派**。

**第四，`default` 分支的报错包含引擎类型名** —— 与其余几个包的工厂一致。

**而 `MessageQueue` 的初始化**用另一个模式：它没有 `once`，因为 `SetMessageQueueEngine` 是一个测试缝。

```
// SetMessageQueueEngine installs the global message-queue engine. It exists
// primarily as a test seam so callers can drive Start() without a real server
// config; production code uses InitMessageQueue.
```

**「测试缝」这个词直接把设计意图说出来了** —— 一个可在生产代码路径之外替换全局单例的 setter。**这与 `Factory.SetStorage`（见 d04 那页）是同一个手法。**

**而 `InitMessageQueue` 目前只支持 `nats`，且空值报错。**

```
case "":
	return fmt.Errorf("message queue type is empty")
```

**「空值单独报错」是一个小但正确的细节** —— 否则「未配置」会落到 `default` 的「不支持的类型」，误导排查方向。

## 关键取舍

**源码事实：** `DocEngine` 有约三十二个方法，分五组：块存储 8、文档元数据 8、自由文档索引 3、结果集后处理 6、能力与运维 5。

**源码事实：** `GetFields` / `GetAggregation` / `GetHighlight` / `KNNScores` / `GetScores` **不访问后端**，是对已取回结果集的内存加工；放在接口里是因为「怎么从这份结果里抽出字段」与后端返回格式绑定（ES 的 highlight 结构、Infinity 的 `SCORE` 列各不相同）。

**源码事实：** 七种引擎实际只有四个实现谱系：Lucene 系（ES/OpenSearch）、Infinity、OceanBase 家族（oceanbase/seekdb）、独立 SQL（serenedb/vastbase）。

**源码事实：** `IsOceanBaseFamily` 只认 `oceanbase` + `seekdb`，因为两者**共享同一套 SQL 实现**（工厂里走同一个 `case`、用引擎名当参数传给同一个构造函数）。`IsVastbase` 单独存在，注释说明它与 OceanBase 家族**共享服务层行为但代码是独立实现** —— 所以不能加进那个谓词。

**源码事实：** `Type(docEngine)` 是一个绕过循环依赖的免费函数（接口只能返回 `string`，否则 `engine` 包要依赖各后端包）；`nil` 返回 `"unknown"` 而非空串。

**源码事实：** `CreateChunkStore` / `InsertChunks` 的 `language` 参数注释明写「全文分析器在索引创建时固定的引擎需要它，其余忽略」；ES 在客户端分析所以忽略，Infinity 需要。

**源码事实：** Infinity 的 `analyzerLanguages` 白名单只有 `czech` 与 `slovak`，注释理由：分析器会把它们的变音符折成 ASCII 并跳过词干化；其它语言要么选 Snowball 词干器、要么是空操作。**注释明确警告：把某个词干器写进这里会让该语言每个新建数据集切换行为** —— 加一个成员不是「支持新语言」而是「改变既有语义」。

**源码事实：** Infinity 的 `InsertChunks` 里注释点明「这是摄取管线实际走的路径，所以数据集语言必须在这里到达」——`CreateChunkStore` 不是实际入口，表常在首次写入时创建，语言必须在两条路径上都能传到建表点。

**源码事实：** `MessageQueue` 与 `DocEngine` 在同一个文件里但毫无关系；`InitConsumer(subject)` 的 `subject` 是「保留但忽略」的旧参数（企业版定义自己的优先级通道集合，社区版用单通道）。

**源码事实：** 租约协议有「带版本的 CAS」语义：`Acquire` / `Heartbeat` 返回 `(revision, ok, error)`，`Release` 要求传 `revision` —— 若别人已在你持有期间抢走租约，你的释放会失败而不是误删。

**源码事实：** `SubscribeNotify` 的注释给出架构定位：「NATS 当闹钟，MySQL 当调度真相」，实现可返回 `(nil, nil)` 表示推送不可用、调用方须退化为定期轮询。

**源码事实：** `InitDocEngine` 用 `sync.Once` + 闭包外声明的 `initErr`（`once.Do` 无法返回值），于是错误被缓存、后续调用不重试；`tokenizer.SetEngineType` 在创建引擎**之前**调用，构成隐式顺序约定。

**源码事实：** `registerChunkHistory` / `registerMetadataHistory` 用 `//go:build !ee` 在社区版留空实现 —— 编译期分派而非运行期配置的钩子。

**架构推论：** 把「结果解析」也放进驱动器接口，是这个抽象最大的性格特征。**好处**是后端格式差异被彻底封住，调用方只需要 `result.Chunks []map[string]interface{}`；**代价**是 `DocEngine` 既不是纯 I/O 接口也不是纯算法接口，任何「想在后端之外做一次统一后处理」的念头都要先问「它属于哪个层」。**当两种后端的返回格式差异足够大时，把解析放进驱动器是唯一不会漏的做法。**

**架构推论：** 「家族谓词」这个手法值得学。它的名字（`IsOceanBaseFamily`）读起来像在分类后端，实际在分类**调用方的分支意图**。一旦某个调用方写了 `if IsOceanBaseFamily(x) { 走共享实现 }`，注释里的「Vastbase 共享行为但非共享实现」就直接变成一次潜在的空指针或类型错误。**谓词函数的判据必须与被判定的那个分支一一对应** —— 一个谓词想同时服务多种分支就是在埋雷。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/engine/engine.go` | `EngineType` | 七个字符串枚举值 |
| `internal/engine/engine.go` | `DocEngine` | 约三十二方法的统一契约 |
| `internal/engine/engine.go` | `Type` | 绕循环依赖的类型转换适配器 |
| `internal/engine/engine.go` | `IsOceanBaseFamily` | 「共享实现」的家族谓词 |
| `internal/engine/engine.go` | `IsVastbase` | 「共享行为但独立实现」的独立谓词 |
| `internal/engine/engine.go` | `MessageQueue` | 任务队列 + 知识编译 + 租约 + notify |
| `internal/engine/global.go` | `InitDocEngine` | `sync.Once` 工厂 + tokenizer 前置耦合 |
| `internal/engine/global.go` | `InitMessageQueue` | 只支持 `nats`，空值单独报错 |
| `internal/engine/global.go` | `SetMessageQueueEngine` | 显式标注的测试缝 |
| `internal/engine/stub.go` | `registerChunkHistory` / `registerMetadataHistory` | `//go:build !ee` 的社区版空钩子 |
| `internal/engine/types/types.go` | `SearchRequest` / `SearchResult` | 跨后端的统一请求与结果 |
| `internal/engine/types/types.go` | `MatchTextExpr` / `MatchDenseExpr` / `FusionExpr` | 三种匹配表达式的形状 |
| `internal/engine/types/types.go` | `SearchMetadataRequest` / `SearchMetadataResult` | 元数据索引的请求与结果 |
| `internal/engine/types/types.go` | `OrderByExpr` / `MatchTextExpr` 等 | `Asc` / `Desc` 链式构造 |
| `internal/common/constants.go` | `PAGERANK_FLD` / `TAG_FLD` | `pagerank_fea` / `tag_feas` 两个跨后端字段名 |
| `internal/common/constants.go` | `MAX_RESULT_WINDOW` / `SearchAfterBatchSize` | 10000 与 1000 两个深分页常量 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：engine.go](https://github.com/infiniflow/ragflow/blob/main/internal/engine/engine.go)
- [Elasticsearch 文档：dense_vector 与 kNN 检索](https://www.elastic.co/guide/en/elasticsearch/reference/current/dense-vector.html)
- [Infinity 项目主页](https://github.com/infiniflow/infinity)
- [Go 文档：sync.Once](https://pkg.go.dev/sync#Once)

## 与主核 / AI 的关系

这个抽象与主核的**存储管理器（`smgr`）** 在同一层位置上，而两者面对的多样性完全不同。

**主核的 `smgr` 只有一种后端**（本地文件，加上 WAL 的两条特殊路径）。它的接口可以极薄（`read` / `write` / `nblocks` / `truncate`），因为差异只存在于「哪条 fork、哪个块号」。**而 `DocEngine` 要同时容纳「倒排索引 + 向量 + 全文分析器」与「一个 SQL 表」这两种完全不同的执行模型**，于是接口必然变厚。

**「结果解析放进接口」这个决定在数据库里有精确的对应物：访问方法（`amapi`）的 `getbitmap` / `amgettuple`。** 索引必须自己回答「怎么从它的存储格式里取出元组」，因为 B-tree 与 GiST 的格式天差地别。**ragflow 把同样的判断做到了结果集的顶层** —— 一旦后端能返回不同的 JSON 形状，解析就不能在调用方做。

**「家族谓词」在数据库里对应的是 `wal_level` / `amname` 这类字符串比较的调用点。** 数据库也常用「按名字判断某后端特性」，而它踩过的坑正是 ragflow 在注释里预防的：**谓词一旦被用于不止一种分支，它的语义就会分裂。** PostgreSQL 的 `relation->rd_tableam` 是一个函数指针表而不是名字比较，正是为了避开这个问题。

**「NATS 当闹钟、MySQL 当真相」这条原则在数据库里是「索引 vs 堆」的分工**：索引可以重建（丢了能恢复），堆是真相（丢了就没了）。**凡是「加速器」都不该承载真相** —— 这是所有分布式系统的共同纪律，而 ragflow 在一行注释里把它写清楚了。

对 agent 工程的直接价值有三条。其一，**接口该厚就厚，别为了「薄」把格式知识漏给调用方**。`GetHighlight` 若不在接口里，每个调用方都要写一遍「ES 返回的是 `map[string]interface{}` 的 highlight、Infinity 返回的是列」——那是必然出错的重复。其二，**同一语义在不同后端上的「是否必需」必须在接口注释里写清**。`language` 参数对 ES 无意义、对 Infinity 决定索引行为，而这件事只有注释能表达。其三，**谓词函数必须与其服务的那个分支一一对应**。「共享实现」与「共享行为」是两个判据，混用一个谓词迟早会错。
