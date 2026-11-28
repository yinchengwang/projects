---
project: ragflow
feature: infinity-engine
title: Infinity 引擎与块分页排序
page_type: feature
schema_version: 1
refs:
  - path: internal/engine/infinity/chunk.go
    symbols: [CreateChunkStore, InsertChunks, Search, DeleteChunks]
---

# Infinity 引擎与块分页排序

## 一句话职责

一个四千行的块存储实现，把「每数据集一张表」这个 SQL 式模型接上向量与全文检索，并在**三处**做了 ES/OS 不需要做的事：建表时把分析器烧进索引、写入时按内容哈希去重后先删后插、分页时用 `_row_id` + 权重值的条件更新做页码权重的乐观并发控制。

## 机制是什么

Infinity 与 Lucene 系的根本差别是**存储模型**：ES/OS 是「文档进索引」，Infinity 是「行进表」。于是「块存储」这件事在两边的形态完全不同：

| | ES / OpenSearch | Infinity |
|---|---|---|
| 索引粒度 | 一个 tenant 一个索引，块按 `kb_id` 过滤 | **一个数据集一张表**（`{baseName}_{datasetID}`） |
| 表结构 | 动态映射（mapping 可随时加字段） | **映射文件在 `conf/` 里，建表时消费** |
| 全文分析器 | 客户端分析，只存词项 | **服务端分析器，创建索引时固定** |
| 唯一性 | `_id` 天然唯一 | **没有 id 唯一约束** |
| 分页 | `from`/`size` + `search_after` | `LIMIT`/`OFFSET` + 显式 `Sort` |

**第一行「一个数据集一张表」是整个实现的地基。** 它带来两个连锁后果：

1. **表名由 `baseName` + `datasetID` 拼出来** —— 于是「跨数据集检索」在 Infinity 上是「跨多张表分别查再合并」，而 ES 上是一次多索引查询。
2. **表结构是静态的** —— 于是「加字段」需要 `AddColumns`，而「未声明的字段」在写入时必须被丢弃。**这解释了 `dropUndeclaredFields` 这个函数的存在必要性。**

**而「分析器固定」这一条引出了本页最重要的一条约束：** 表已存在时，`CreateChunkStore` 会保留原有索引——**连带保留它建表时用的那个分析器**。于是「改数据集语言」这个操作在 Infinity 上不可能生效。

## 怎么实现的

### 一、`CreateChunkStore`：三层 mapping 选择 + 三种索引

**入口只做一件事：拿数据库连接，然后转给 `createChunkStoreWithDB`。**

**第一层选择是表名。**

```
func buildChunkTableName(baseName, datasetID string) string {
	if datasetID == "skill" {
		return baseName
	}
	return fmt.Sprintf("%s_%s", baseName, datasetID)
}
```

**「skill 用 `baseName` 本身」是一个特例** —— 因为技能索引不按数据集分表。

**第二层选择是映射文件。**

```
func chunkMappingFileName(baseName, datasetID, configured string) string {
	switch {
	case datasetID == "skill":
		return "skill_infinity_mapping.json"
	case isMemoryIndexName(baseName):
		return "message_infinity_mapping.json"
	default:
		return configured
	}
```

**而它前面有一段注释记录了一次真实故障：**

> Memory 消息表需要 `message_infinity_mapping.json`：**它们的行带 `user_id`、`message_id`……而块映射不声明这些列，所以用块映射建表会让每次插入都以 3013 失败。**

**「3013」这个错误码在 Infinity 里几乎等同于「SQL 层不接受这个请求」**，而它在本页反复出现（未声明字段、`_score` 投影、`similarity()` 限制、`status` 列不存在）——**这是一个「Schema 严格」与「接口宽松」之间的持续摩擦点。**

**第三层是「表已存在时怎么办」，而这里的逻辑最讲究。**

```
if exists {
	table, err = db.GetTable(tableName)
	colExists, err := e.columnExists(table, vectorColName)
	if !colExists {
		addColSchema := infinity.TableSchema{
			&infinity.ColumnDefinition{
				Name:     vectorColName,
				DataType: fmt.Sprintf("vector,%d,float", vecSize),
			},
		}
		if _, err := table.AddColumns(addColSchema); err != nil {
			return fmt.Errorf("failed to add vector column %s: %w", vectorColName, err)
		}
	}
	if parserID == "table" {
		chunkDataExists, checkErr := e.columnExists(table, "chunk_data")
		if err := e.ensureChunkDataColumn(table, chunkDataExists); err != nil {
			return err
		}
	}
}
```

**「加向量列而不是改向量列」是本页最关键的设计决定。**

**向量列名是 `q_<dim>_vec`**（`common.VectorColumnName(vecSize)`），**于是换嵌入模型（维度变化）时得到的是一个新的列名，而不是同一个列的宽度变化。** 而列名一换，旧列与旧索引就还在——查询时指定新列名即可。

**这解释了向量索引的命名：**

```
vectorIndexName := fmt.Sprintf("q_%d_vec_idx", vecSize)
_, err = table.CreateIndex(
	vectorIndexName,
	infinity.NewIndexInfo(vectorColName, infinity.IndexTypeHnsw, map[string]string{
		"M":               "16",
		"ef_construction": "50",
		"metric":          "cosine",
		"encode":          "lvq",
	}),
	infinity.ConflictTypeIgnore,
	"",
)
```

**「把维度写进索引名，避免嵌入模型变化时的冲突」** —— 注释原话。而 `ConflictTypeIgnore` 让「索引已存在」不报错。

**四个 HNSW 参数值得记：**

| 参数 | 值 | 含义 |
|---|---|---|
| `M` | 16 | 每个节点的邻居数 |
| `ef_construction` | 50 | 建图时的候选池 |
| `metric` | cosine | 度量 |
| `encode` | **lvq** | 向量量化 |

**`encode=lvq`（locally-adaptive vector quantization）是一个明确的取舍：用少量精度换内存与速度。** 对一个「块向量」场景（数量大、单条查询容忍近似）是合适的；而对精度敏感的场景它会是隐患。**这一行是硬编码的，没有通过配置暴露。**

**然后是全文索引，而这里的双重循环是「分析器烧进索引」的实现。**

```
for _, fieldName := range schema.Keys {
	fieldInfo := schema.Fields[fieldName]
	if fieldInfo.Type != "varchar" || fieldInfo.Analyzer == nil {
		continue
	}
	analyzers := []string{}
	switch a := fieldInfo.Analyzer.(type) {
	case string:        analyzers = []string{a}
	case []interface{}: for _, v := range a { if s, ok := v.(string); ok { analyzers = append(analyzers, s) } }
	}
	for _, analyzer := range analyzers {
		indexNameFt := fulltextIndexName(fieldName, analyzer)
		_, err = table.CreateIndex(
			indexNameFt,
			infinity.NewIndexInfo(fieldName, infinity.IndexTypeFullText, map[string]string{"ANALYZER": analyzerForLanguage(analyzer, language)}),
			infinity.ConflictTypeIgnore, "",
		)
	}
}
```

**三个要点：**

1. **只对 `varchar` 且有 `Analyzer` 声明的字段建全文索引** —— 而 `Analyzer` 可以是字符串或字符串数组，**于是同一个字段可以有多个分析器**（例如 `rag-coarse` 与 `rag-fine` 各一个）。
2. **索引名 `ft_<field>_<analyzer>`**，且用 `indexNamePartRe`（`[^a-zA-Z0-9]` → `_`）清洗。**注释说明了为什么查询时不需要再提分析器：**

> 查询与过滤显式引用索引名（见 `convertMatchingField`），所以名字在每种语言下都相同；**数据集语言只改变它背后的分析器。**

3. **`analyzerForLanguage(analyzer, language)` 才是 `language` 参数唯一真正落地的地方。**

**而这一段的注释是全文件最重要的一句：**

> 上面的 `CreateTable` 是 `ConflictTypeIgnore`，所以这**可能是一张已存在的表**：它的索引保持原样，**连带它创建时用的那个分析器**。

**「索引保持原样 ⇒ 分析器保持原样」这个推论把两件事绑在一起了。** 于是「给已存在的数据集换语言」在 Infinity 上**不是「不管用」，而是「不可能」** —— 因为查询时的分析器由索引决定，而索引不能被重建（`ConflictTypeIgnore` 会让重建请求变成空操作）。

**最后是二级索引，只处理 `index_type.type == "secondary"` 的字段。**

```
if indexTypeStr == "secondary" {
	indexNameSec := fmt.Sprintf("sec_%s", fieldName)
	_, err = table.CreateIndex(indexNameSec, infinity.NewIndexInfo(fieldName, infinity.IndexTypeSecondary, params), infinity.ConflictTypeIgnore, "")
}
```

**`params["cardinality"]` 被透传** —— 这是 Infinity 对二级索引的可选提示。

### 二、`InsertChunks`：五步，而每一步都在补偿「没有唯一约束」

**第一步：表不存在就自己建。**

```
table, err := db.GetTable(tableName)
if err != nil {
	errMsg := strings.ToLower(err.Error())
	if !strings.Contains(errMsg, "not found") && !strings.Contains(errMsg, "doesn't exist") {
		return nil, fmt.Errorf("failed to get table %s: %w", tableName, err)
	}
	// Infer vector size from chunks
	vectorSize := 0
	vectorPattern := regexp.MustCompile(`q_(\d+)_vec`)
	for _, chunk := range chunks { for key := range chunk { if m := vectorPattern.FindStringSubmatch(key); len(m) >= 2 { vectorSize, _ = strconv.Atoi(m[1]); break } } if vectorSize > 0 { break } }
	if vectorSize == 0 { return nil, fmt.Errorf("cannot infer vector size from chunks") }
	if err := e.createChunkStoreWithDB(db, baseName, datasetID, vectorSize, parserIDFromChunks(chunks), language); err != nil { ... }
```

**「从块里反推向量维度」是一个依赖命名约定的技巧** —— 它成立的前提正是上一步「列名带维度」的设计。**两处设计咬合在一起了。**

**而它带着一条注释：**

> 这条是摄取管线**实际走的路径**，所以数据集语言必须在这里到达。

**「`CreateChunkStore` 不是实际入口」这一点很重要** —— 大部分表是第一次写入时才被创建的，于是 `language` 必须在两条路径上都能传到建表点。

**`parserIDFromChunks` 用「块里有没有 `chunk_data`」来判断是表格解析器产出的：**

```
func parserIDFromChunks(chunks []map[string]interface{}) string {
	for _, chunk := range chunks {
		if chunkData, ok := chunk["chunk_data"].(map[string]interface{}); ok && chunkData != nil {
			return "table"
		}
	}
```

**「用内容特征反推解析器」是一个务实的做法**，而它的可靠前提是「只有表格解析器会写这个字段」。

**第二步：从 `ShowColumns` 里读回真实的列与嵌入列。**

```
re := regexp.MustCompile(`Embedding\([a-z]+,(\d+)\)`)
...
matches := re.FindStringSubmatch(colType)
if len(matches) >= 2 { size, _ := strconv.Atoi(matches[1]); embeddingCols = append(embeddingCols, [2]interface{}{colName, size}) }
```

**注意列的类型串是 `Embedding(<type>,<dim>)`** —— 与建表时写的 `vector,%d,float` 不同，**这是 Infinity 读回来的格式。**

**而 `chunk_data` 列的补偿也在这里做，且带一条很实在的注释：**

> 已存在的表是通过 `GetTable` 打开的，永远走不到 `createChunkStoreWithDB`。**尽力而为，像 Python 的 `ensure_columns` —— 一个仍然缺失的列会在下面的插入里暴露出来。**

**「尽力而为」是关键：这里失败了不返回错误，只 `Warn`。** 因为下一步的插入会给出真正的错误 —— **与其在这里猜原因，不如让下游报准确的错。**

**第三步：逐块投影，而 `kb_id` 由引擎强制写入。**

```
for i, chunk := range chunks {
	if memoryIndex {
		insertChunks[i] = shapeMemoryMessageRow(chunk, embeddingCols)
		continue
	}
	insertChunks[i] = transformChunkFields(chunk, embeddingCols)
	// kb_id is owned by the engine at the write boundary (mirrors ES
	// chunk.go InsertChunks). The ingestion producer no longer stamps it,
	// so the producer value (if any) is intentionally overridden here.
	insertChunks[i]["kb_id"] = datasetID
}
```

**「`kb_id` 由引擎在写入边界拥有，生产者不再盖章，所以生产者的值被有意覆盖」** —— 这是一条**归属权声明**：字段的真相来自参数而不来自载荷。

**紧接着是那个静态 schema 的必然补偿：**

```
if err := e.dropUndeclaredFields(tableName, insertChunks, columns); err != nil {
	return nil, err
}
```

**「丢弃未声明的字段」在 ES 里不需要（动态映射会接受），在 Infinity 里必须做**（否则 3013）。

**第四步：按内容哈希 id 去重。**

```
insertChunks = dedupeByChunkID(insertChunks)
```

**注释把整件事讲清楚了：**

> 块 id 是**内容寻址**的，所以一个批次里可能重复同一个 id。**Elasticsearch 每个 `_id` 只保留一行；Infinity 没有 id 唯一性**，所以折叠成每个 id 一行、**后者胜**。跨批次的重复不需要处理：下面的删除会覆盖它们。

**「内容寻址 ⇒ 批次内重复」这个因果关系值得单独说：** 若两个块的内容相同，它们的 id 就相同——**而同一文档里内容相同的两个块是完全合法的。**

**第五步：先删后插。**

```
if len(insertChunks) > 0 {
	idList := make([]string, len(insertChunks))
	for i, chunk := range insertChunks {
		// is a UUID produced by the document ingestion path
		// (uuid.NewString), not user input. We single-quote it
		// for Infinity SQL; UUIDs cannot contain single quotes
		// by construction (RFC 4122 §3).
		// codeql[go/unsafe-quoting] False positive: chunk["id"]
		idList[i] = fmt.Sprintf("'%v'", chunk["id"])
	}
	filter := fmt.Sprintf("id IN (%s)", strings.Join(idList, ", "))
	delResp, delErr := table.Delete(filter)
	if delErr != nil { common.Warn(...) } else { common.Info(fmt.Sprintf("Deleted %d existing rows", delResp.DeletedRows)) }
}
```

**「先删后插」是 Infinity 上实现 upsert 的唯一路径**（没有 `ON CONFLICT` 可用）。

**而删除失败只 `Warn` 不中止** —— 因为「没有旧行」与「删失败」对插入来说后果相同（后者会得到重复行，但不致命）。**这个宽容是有意的，且它的代价是可能出现重复行。**

**`codeql` 那两行注释是「安全告警的正当性辩护」**：拼进 SQL 的是一个 UUID，而 UUID 按 RFC 4122 不含单引号，所以单引号包裹是安全的。

**插入本身按 `infinityMaxInsertRows = 8192` 分批，而错误处理带连接回收：**

```
for start := 0; start < len(insertChunks); start += infinityMaxInsertRows {
	batch := insertChunks[start:min(start+infinityMaxInsertRows, len(insertChunks))]
	if _, err = table.Insert(batch); err != nil {
		common.Warn("Infinity insert failed", zap.String("tableName", tableName), zap.Int("rows", len(batch)), zap.Strings("rowFields", rowFieldNames(batch)), zap.Error(err))
		if isConnectionLevelError(err) { e.client.dropConnectionFor(db, "InsertChunks") }
		return nil, fmt.Errorf("failed to insert chunks to dataset: %w", err)
	}
}
```

**`rowFieldNames(batch)` 在失败时把**这一批的字段名**打出来** —— 这是针对 3013 的诊断：**「列不存在」这个错误只有配上「我提交了哪些列」才有用。**

**而 `isConnectionLevelError` + `dropConnectionFor` 是一个明确的区分：** 若错误说明连接已坏，把这个连接从池里踢掉（否则下一次复用它还会失败）；若是 schema 错误则保留。

**最后一行很值得看：**

```
return []string{}, nil
```

**它返回一个**空切片**而不是插入的 id 列表。** 与 ES 那边对比（ES 的 `insertChunks` 返回 `ids`）这是一处语义差异 —— 而调用方若依赖「返回的 id 列表」就会在 Infinity 上拿到空。**这是一个真实的行为不对称，虽然对本页的机制解释无害，但它是「同接口不同实现」的又一实例。**

### 三、`AdjustChunkPagerank`：三重重试 + 条件更新 + 读回校验

**这是 Infinity 独有能力的实现，也是全文件并发控制最讲究的一段。**

**第一层：进程内分片锁。**

```
func pagerankAdjustLock(key string) *sync.Mutex {
	hash := fnv.New32a()
	_, _ = hash.Write([]byte(key))
	return &pagerankAdjustLocks[hash.Sum32()%pagerankAdjustLockCount]
}
```

**256 个 `sync.Mutex` 组成的固定数组，按 `tableName+":"+chunkID` 的 FNV-32a 哈希取模选一个。**

**这是一个「用哈希分片把锁的竞争范围缩小」的标准手法**，代价是不同 key 可能碰撞到同一把锁（`256` 远小于可能的块数）。**碰撞只影响并发度，不影响正确性** —— 因为锁的作用是「减少同一块的并发冲突」，而不是隔离不同块。

```
lock := pagerankAdjustLock(tableName + ":" + chunkID)
lock.Lock()
defer lock.Unlock()
```

**第二层：读-改-写的循环，带 `pagerankAdjustRetryCount = 2` 次重试（共 3 次尝试）。**

```
for attempt := 0; attempt <= pagerankAdjustRetryCount; attempt++ {
	if err := ctx.Err(); err != nil { return err }
	result, err := table.Output([]string{common.PAGERANK_FLD, "row_id()"}).Filter(filter).ToResult()
	if err != nil { lastErr = err; continue }
	rowID, ok := firstQueryInt64(qr.Data, "ROW_ID", "row_id()", "row_id")
	if !ok { return fmt.Errorf("%w: %s", types.ErrDocumentNotFound, chunkID) }
	currentWeight := 0.0
	if currentValue, ok := firstQueryValue(qr.Data, common.PAGERANK_FLD); ok { if weight, ok := coerceToFloat(currentValue); ok { currentWeight = weight } }
	nextWeight := currentWeight + delta
	if nextWeight < minWeight { nextWeight = minWeight }
	if nextWeight > maxWeight { nextWeight = maxWeight }
	if floatsEqual(currentWeight, nextWeight) { return nil }
	...
}
```

**三个要点：**

1. **读的是 `row_id()` 而不只是权重** —— 因为下一步的更新条件要用它。
2. **`firstQueryInt64` 试三个键名**（`ROW_ID`、`row_id()`、`row_id`）—— **同一个东西在 Infinity 的三种写法下会以三种名字出现**，这是「接口宽松」的又一处摩擦。
3. **`floatsEqual(current, next)` 直接返回成功** —— 当钳位让结果等于当前值时不做写操作。**这既省一次写，也避免把 `next <= 0` 的情形误判为「需要写 0」。**

**第三层：条件更新，而条件里带上了旧的权重值。**

```
updateFilter := fmt.Sprintf("_row_id = %d AND %s = %s", rowID, common.PAGERANK_FLD, formatFilterFloat(currentWeight))
if _, err := table.Update(updateFilter, map[string]interface{}{common.PAGERANK_FLD: nextWeight}); err != nil {
	lastErr = err
	continue
}
```

**`_row_id = <id> AND pagerank_fea = <旧值>` 是一次 CAS** —— 若期间有别人改了权重，这条更新会匹配 0 行（而不是覆盖别人的结果），于是下一轮重读重算。

**这与 f01 那页 `SQLiteCache._delete_if_unchanged` 是同一种手法，也与 ES/OS 那两处把 `retry_on_conflict=3` 交给服务端的做法是同一种意图。** 三种实现、一个目标：**并发累加不能丢更新。**

**而 Infinity 没有 `retry_on_conflict` 可用，所以必须自己实现 CAS + 自己重试。**

**第四层：读回校验。**

```
verifyResult, err := table.Output([]string{common.PAGERANK_FLD}).Filter(filter).ToResult()
...
if currentValue, ok := firstQueryValue(verifyQR.Data, common.PAGERANK_FLD); ok {
	if actualWeight, ok := coerceToFloat(currentValue); ok && floatsEqual(actualWeight, nextWeight) { return nil }
}
lastErr = fmt.Errorf("pagerank update conflict")
```

**「更新返回成功」不够 —— 必须再读一次确认写进去的确实是 `nextWeight`。**

**为什么？** 因为 `table.Update` 的成功可能意味着「匹配 0 行」也可能意味着「写成功了但被并发覆盖」。**读回校验把这三种情况压成两种：值对 → 成功；值不对 → 冲突，重试。**

**而三次尝试都失败后的错误是：**

```
return fmt.Errorf("failed to adjust chunk pagerank: %w", lastErr)
```

**`lastErr` 可能是任一层的错误（读失败、写失败、或「冲突」）** —— 于是上层看到的是「最后一次的原因」。

**最后一点值得记：`SupportsPageRank()` 在这个引擎上返回 `false`，而 `AdjustChunkPagerank` 存在且被认真实现。**

**接口文档把那个方法定义为「引擎是否支持**数据集级** pagerank**」，所以 `false` 表达的是「没有数据集级的 pagerank 聚合」，而不是「不能调单个块的权重」。** 但两个具名概念（`SupportsPageRank` 与 `AdjustChunkPagerank`）在同一个接口里出现而取相反的默认，是一个会让调用方犹豫的地方 —— **能力标志与方法存在性不对齐时，标志必须靠注释来解释它的确切范围。**

### 四、`DeleteChunks`：与 `UpdateChunks` 共享同一道「不许全域」的闸

**两个方法都有这段几乎逐字相同的守卫，而 `UpdateChunks` 那份的注释最完整：**

```
filter := buildFilterFromCondition(condition, clmns)
if len(condition) > 0 && (filter == "" || filter == "1=1") {
	// Every condition key was dropped (blank value, empty list, unknown
	// column). table.Update("1=1", ...) would rewrite every row of the
	// dataset, so refuse instead — mirrors DeleteChunks/DeleteMetadata.
	return fmt.Errorf("INFINITY update aborted: non-empty condition yielded unconstrained filter on table %s", tableName)
}
```

**它防的是一个具体的灾难：** 调用方给了条件，但条件里每个键都被丢弃（空值、空列表、未知列），于是 `buildFilterFromCondition` 返回 `""` 或 `"1=1"`。**而 `table.Update("1=1", ...)` 会重写整个数据集的每一行。**

**判据是「条件非空但结果无约束」** —— 而不是「条件为空」。**后者是合法用法**（无条件的 update 可能是有意的），**前者一定是翻译失败。**

**这是一条值得抄的规则：当「把 A 翻译成 B」这一步可能悄悄产出「不受限的 B」时，必须检查并拒绝。** 静默降级成全表操作是所有过滤层里最危险的一类缺陷。

**而 `DeleteChunks` 的表存在性处理是宽容的：**

```
table, err := db.GetTable(tableName)
if err != nil {
	common.Warn(fmt.Sprintf("Table %s does not exist, skipping delete", tableName))
	return 0, nil
}
```

**「表不存在 → 删掉 0 行，成功」** —— 这与「幂等删除」的语义一致（目标状态已达成）。

**而 `UpdateChunks` 对同一个情况也返回 `nil`，注释说这是为了对齐 Python：「镜像 Python 的 `docStoreConn`，它在表不存在时静默返回 False」。**

**「对齐旧实现的行为而不对齐它的错误处理」是一个值得留意的模式** —— `DeleteChunks` 的 `return 0, nil` 与 `UpdateChunks` 的 `return nil` 都在复刻「静默成功」，而不是借机改成报错。

### 五、`Search`：一条查询要过五道适配

**Infinity 的 `Search` 与 ES 那份的骨架相同，但每一段都多一层转换。它们可以按顺序列出来：**

**适配一：`available_int` 的默认过滤是「三段式推断」。**

```
if availInt, ok := req.Filter["available_int"]; ok {
	filterParts = append(filterParts, fmt.Sprintf("available_int=%v", availInt))
} else if status, ok := req.Filter["status"]; ok {
	filterParts = append(filterParts, fmt.Sprintf("status='%s'", status))
} else if shouldDefaultAvailableFilter(req, isSkillIndex) {
	filterParts = append(filterParts, "available_int=1")
}
```

**「显式给了 `available_int` 就用它；给了 `status` 就用 `status`；都没给就默认 `available_int=1`」。**

**而 `shouldDefaultAvailableFilter` 只是两个否定：**

```
func shouldDefaultAvailableFilter(req *types.SearchRequest, isSkillIndex bool) bool {
	return !isSkillIndex && !req.IncludeUnavailable
}
```

**「默认只检索可用块」这个行为是被 `IncludeUnavailable` 显式关闭的** —— 于是「要不要看禁用块」是一个显式选择而不是遗漏。

**适配二：`_score` 投影必须先剥掉再加回去。**

```
outputColumns = stripScorePseudoColumns(outputColumns)
if hasTextMatch || hasVectorMatch {
	if hasTextMatch { outputColumns = append(outputColumns, "score()") }
	if hasVectorMatch && !hasTextMatch { outputColumns = append(outputColumns, "similarity()") }
	...
}
```

**注释把原因讲得很清楚：**

> Infinity 把 `_score` 投影绑定到 `SCORE()`，而它只允许在 MATCH TEXT/TENSOR 或 Fusion 时使用 —— 否则 3013。**Elasticsearch 对任何形状都容忍 `_score`，所以调用方无条件传它**（导航的 cluster 下降与 navScan 都这么做）：**丢掉他们的、再加回这个形状唯一接受的那一种。** `calculateScores()` 无论如何都会为读者物化 `_score`。

**而 `similarity()` 的限制更窄：**

> `similarity()` 只被 Infinity 在**只有 MATCH VECTOR** 时允许。当文本与向量匹配同时存在（带 Fusion 的混合检索），只有 `score()` 有效 —— **Fusion 产生一个统一的 `SCORE` 列。**

**「调用方无条件传 → 后端各自修正」是一个接口不对称的典型形态**：`SearchRequest` 里 `SelectFields` 是通用的，而「能不能选 `_score`」取决于后端与查询形状。

**适配三：`row_id()` 无条件补上。**

```
if !slices.Contains(outputColumns, "row_id") && !slices.Contains(outputColumns, "row_id()") {
	outputColumns = append(outputColumns, "row_id()")
}
```

**`row_id` 在 Infinity 里是隐式主键，而它被无条件取回** —— 因为「按 id 更新」需要它（`AdjustChunkPagerank` 用的就是它）。

**适配四：部分通配投影必须按表展开。**

```
wildcardSelect := hasPartialWildcard(outputColumns)
...
if wildcardSelect { tableOutputColumns = expandSelectWildcards(tableOutputColumns, tableColumns) }
```

**注释说明了两种后端的差异：**

> **Infinity 把 `q_*_vec` 解析成算术表达式 `q_` * `_vec` 并绑定失败（3013），而 Elasticsearch 会在 `_source` 里展开这个模式。嵌入维度因表而异，所以这件事必须按表决定。** 裸 `*` 不动它：Infinity 支持它。

**「`q_*_vec` 被当成乘法」是一个只有列式 SQL 引擎才会犯的错** —— 而补偿是「先读这张表的真实列，再在本地展开通配」。

**适配五：记忆索引的排序要在本地合成。**

```
mergeMemoryOrder := isMemoryIndex && !hasTextMatch && !hasVectorMatch && orderBy != nil && len(orderBy.Fields) > 0
...
if mergeMemoryOrder {
	table = table.Limit(pageSize + offset)
} else {
	table = table.Limit(pageSize)
	if offset > 0 { table = table.Offset(offset) }
}
...
if mergeMemoryOrder {
	sortChunksByFields(allResults, orderBy)
	allResults = allResults[min(offset, len(allResults)):]
}
```

**「记忆索引在合并多张表之后再排序」** —— 因为单表内的 `Sort` 无法保证跨表的全局顺序。**代价是每张表都要多取 `pageSize + offset` 行。**

**而 `Status` 之外还有一处「按数量上限截断」：**

```
if len(allResults) > pageSize {
	allResults = allResults[:pageSize]
}
```

**在跨表合并之后裁到 `pageSize`。**

**除此之外有三处值得单独记的工程细节：**

**其一：错误不再被吞掉。**

```
// queryErr keeps the first per-table failure. A failed table query used to be
// swallowed (warn + continue), which turned schema/query mismatches into
// silently empty results — a partial-wildcard projection ("q_*_vec", 3013)
// made the knowledge-compile reader see no products, so the dataset navigation
// tree silently never materialized. Failures stay tolerable while another
// table answered, but surface when nothing did.
```

**「从『静默返回空结果』改成『有别的表答了才容忍、全都失败就报错』」** —— 这段注释记录了一个真实的、由「吞错误」造成的下游故障（导航树永远不出现）。

**最后的判定是：**

```
if len(allResults) == 0 && totalHits == 0 && queryErr != nil {
	return nil, fmt.Errorf("infinity search failed on all tables: %w", queryErr)
}
```

**「零结果 + 零总数 + 有错误」三者同时成立才算失败。** 若 `totalHits > 0` 说明有些表答了，此时空结果是可信的。

**其二：`ef` 有一个带实测数据的下限。**

```
// defaultDenseEF floors the HNSW candidate pool ("ef") for Infinity dense
// search. Chunk tables are HNSW-indexed (see createChunkStoreWithDB), and
// Infinity defaults ef to topN when the caller passes none, which degrades
// to near-greedy descent on large graphs and silently loses recall: on a
// 60k-row HNSW graph top-10 self-hit was 54% with the default and 94.5%
// at ef=200 (#22161).
const defaultDenseEF = 200
```

**「6 万行的 HNSW 图上，top-10 自命中率默认是 54%，ef=200 时 94.5%」** —— 这是全页最硬的一个数字，而它解释了为什么必须有一个下限而不能相信服务端默认。

**而 `resolveDenseEF` 还有一条约束：**

> `ef` 永远不低于请求的 `topN`（**比 `topN` 小的 `ef` 不可能返回 `topN` 行**）。

```
return max(ef, topN)
```

**其三：`minimum_should_match` 是从三种类型读出来的。**

```
if v, ok := matchText.ExtraOptions["minimum_should_match"]; ok {
	switch x := v.(type) {
	case float64: minMatch = x
	case int:     minMatch = float64(x)
	case string:  s := strings.TrimSuffix(x, "%"); if pct, err := strconv.Atoi(s); err == nil { minMatch = float64(pct) / 100 }
	}
}
```

**「`float64`、`int`、带 `%` 的字符串都接受」** —— 又是同一类摩擦，而这里的补偿是「把百分比字符串归一成小数」。

**而默认值是 `0.0`：**

> 镜像 `rag/utils/infinity_conn.py`，它读 `matchExpr.extra_options.get("minimum_should_match", 0.0)` —— 对英文（非中文）路径，Go 的 `Question()` 构造器不发 `minimum_should_match`，所以默认 0.0 用以匹配 Python 对英文查询实际生效的 0% 阈值。

**「英文查询的默认阈值是 0%」** —— 也就是说英文路径上「一个词都不必命中」也算匹配。**这是一个会显著影响召回率的默认，而它来自对旧实现的对齐而不是一个独立决定。**

**而 Fusion 的归一化默认是 `atan`（与 OS 的 `min_max` 不同）：**

```
fusionParams := map[string]interface{}{"normalize": "atan"}
if fusionExpr.FusionParams != nil { for k, v := range fusionExpr.FusionParams { fusionParams[k] = v } }
```

**「调用方给的参数覆盖默认，默认是 `atan`」** —— 两种后端的默认归一化方式不同，于是**同一个查询在 ES 系与 Infinity 上不仅分数不同，连「怎么把两个分数合成一个」的数学都不同。** 这解释了 d02 那页为什么说「语义不一致但出口一致」。

### 六、`SearchByRegexp`：RE2 之所以是 RE2

**它是一个**刻意与 `SearchRequest` 分开的类型**，而 `types` 包里对这件事有说明：**

> 它与 `SearchRequest`（携带文本/向量/融合匹配表达式）**故意解耦**，因为正则匹配是一种独立的检索模式，没有向量或文本的相关性打分。

**Infinity 这一份的实现依赖两件事：**

```
// The pattern goes down as regex(content, ...), which the content column's
// sparse gram index narrows by the pattern's mandatory literals; Infinity
// evaluates it with RE2, so the pattern is used verbatim (no ".*(...).*" wrapper)
// and "(?i)" carries case folding.
```

**「内容列的稀疏 gram 索引会按模式里的必选字面量收窄范围」** —— 这是正则能在大表上跑得动的原因。**而「RE2 直接吃原样模式，不加 `.*(...).*` 包裹」** 说明语法上不需要为了「子串匹配」而改写模式。

**两个限制常量：**

```
regexpSearchDefaultLimit = 30      // 与 ES 引擎一致，让 grep_chunks 在后端间返回同一页
regexpSearchReturnAllLimit = 10000 // list_chunks 深读的天花板
```

**「与 ES 一致」这条注释是在承诺跨后端行为一致** —— 而 `RegexpSearchRequest.ReturnAll` 的存在说明这个上限需要被显式关闭。

## 关键取舍

**源码事实：** 一个数据集一张表（`{baseName}_{datasetID}`），`datasetID == "skill"` 时表名就是 `baseName`；映射文件三选一（`skill_infinity_mapping.json` / `message_infinity_mapping.json` / 配置值），memory 表用错映射会让每次插入 3013。

**源码事实：** 向量列名 `q_<dim>_vec`、索引名 `q_<dim>_vec_idx` —— **把维度烧进名字**，于是换嵌入模型得到的是一个新列而非同列的宽度变化，旧列与旧索引原地保留；建索引用 `ConflictTypeIgnore`，HNSW 参数硬编码 `M=16` / `ef_construction=50` / `metric=cosine` / **`encode=lvq`**。

**源码事实：** 全文索引名 `ft_<field>_<analyzer>`（非字母数字字符换成 `_`），`ANALYZER` 参数经 `analyzerForLanguage` 处理；**注释明确：`CreateTable` 是 `ConflictTypeIgnore`，所以表可能存在，它的索引（连带建表时的分析器）保持原样** —— 于是「给已存在数据集换语言」在 Infinity 上不可能生效。

**源码事实：** 二级索引名 `sec_<field>`，只对 `index_type.type == "secondary"` 的字段建，`cardinality` 透传。

**源码事实：** `InsertChunks` 在表不存在时**从块里的 `q_<dim>_vec` 键反推向量维度**再建表，并用 `parserIDFromChunks`（看有没有 `chunk_data`）判断解析器；注释点明「这是摄取管线实际走的路径，所以数据集语言必须在这里到达」。

**源码事实：** 已存在的表走 `GetTable`、永远到不了 `createChunkStoreWithDB`，所以 `chunk_data` 列要在 `InsertChunks` 里单独补偿，且**尽力而为**（失败只 `Warn`）——理由是「仍然缺失的列会在下面的插入里暴露」。

**源码事实：** `kb_id` 由引擎在写入边界强制写成 `datasetID`，注释说明「生产者不再盖章，所以生产者的值被有意覆盖」。

**源码事实：** 写入前 `dropUndeclaredFields` 丢弃表里没有的字段；**ES 靠动态映射接受，Infinity 必须丢**（否则 3013）。

**源码事实：** 块 id 是**内容寻址**的，所以同一批次可能出现重复 id；ES 每个 `_id` 只留一行而 **Infinity 没有唯一约束**，于是 `dedupeByChunkID` 折叠成每 id 一行、后者胜；跨批次的重复由「先删后插」覆盖。

**源码事实：** 插入是「先 `id IN (...)` 删除、再分批插入」，删除失败只 `Warn` 不中止（代价是可能出现重复行）；插入按 `infinityMaxInsertRows = 8192` 分批，失败时把**这一批的字段名**打进日志（为诊断 3013），并在 `isConnectionLevelError` 时 `dropConnectionFor`。

**源码事实：** `InsertChunks` 成功时返回**空切片** `[]string{}` 而非插入的 id —— 与 ES 的返回不同，是一处真实的行为不对称。

**源码事实：** `AdjustChunkPagerank` 用 256 个 `sync.Mutex` 按 `tableName+":"+chunkID` 的 FNV-32a 哈希分片；重试 3 次（`pagerankAdjustRetryCount = 2`）；更新条件是 **`_row_id = <id> AND pagerank_fea = <旧值>`**（CAS），写完再**读回校验**，值不对则记「pagerank update conflict」并重试。

**源码事实：** `floatsEqual(current, next)` 时直接返回成功 —— 钳位后不变就不写。

**源码事实：** `SupportsPageRank()` 在这个引擎上返回 `false`，而 `AdjustChunkPagerank` 存在且被认真实现；接口文档把该方法定义为「数据集级 pagerank」，所以 `false` 表达的是「没有数据集级聚合」而非「不能调单个块权重」。

**源码事实：** `UpdateChunks` 与 `DeleteChunks` 共享同一道闸：`len(condition) > 0 && (filter == "" || filter == "1=1")` 时**拒绝执行**，注释理由「`table.Update("1=1", ...)` 会重写数据集的每一行」；判据是「条件非空但结果无约束」，而空条件本身是合法的。

**源码事实：** 表不存在时 `UpdateChunks` 返回 `nil`、`DeleteChunks` 返回 `(0, nil)`，注释说明这是对齐 Python `docStoreConn` 的「静默成功」。

**源码事实：** `UpdateChunks` 里 `newValue["remove"]` 是一个伪键：按 `###` 分隔符解析现有值、逐值删除、按「新值 → id 列表」聚合后再逐组更新；随后把 `docnm_kwd` / `title_tks` / `content_ltks` 等**中间词项字段**从更新里删掉（「必须与 Python `infinity_conn.py` 的删除列表一致」）。

**源码事实：** `Search` 的五道适配：`available_int` 三段式默认（显式值 → `status` → 默认 `1`，由 `IncludeUnavailable` 关闭）、`_score` 剥掉再加回（`score()` 需文本匹配、`similarity()` 只在**纯向量**时合法）、`row_id()` 无条件补、部分通配投影按表展开（「Infinity 把 `q_*_vec` 解析成算术表达式 `q_` * `_vec` 并 3013」）、记忆索引跨表排序在本地合成。

**源码事实：** 阈值从 `ExtraOptions["similarity"]`（float64）或 `["threshold"]`（string）取；`minimum_should_match` 接受 `float64` / `int` / 带 `%` 的字符串，**默认 `0.0`**，注释说明这是对齐 Python 且「Go 的 `Question()` 对英文路径不发该参数，所以英文的有效阈值是 0%」。

**源码事实：** 默认全文匹配字段是硬编码列表：块路径 `title_tks^10` / `title_sm_tks^5` / `important_kwd^30` / `important_tks^20` / `question_tks^20` / `content_ltks^2` / `content_sm_ltks`；skill 路径 `name^10` / `tags^5` / `description^3` / `content^1`。

**源码事实：** dense 与 text 同时存在时用 `filter_fulltext('<fields>', '<query>')` 把文本腿变成**向量腿的过滤器**，且对两者的单引号都做**双写转义**，注释明确 `fieldsStr` 来自固定白名单、非用户可控。

**源码事实：** Fusion 的默认归一化是 **`atan`**（OS 那边是 `min_max` + `arithmetic_mean`）。

**源码事实：** 多表查询的失败策略是「有别的表答了才容忍，全都失败就报错」，注释记录了一次真实故障：吞错误把「schema 不匹配」变成「静默空结果」，导致知识编译读者看不到产物、导航树永远不物化。

**源码事实：** `defaultDenseEF = 200`，注释给了实测：「6 万行 HNSW 图上 top-10 自命中率默认 54%、ef=200 时 94.5%」；`resolveDenseEF` 保证 `ef >= topN`（「比 topN 小的 ef 不可能返回 topN 行」）。

**源码事实：** `SearchByRegexp` 走 `regex(content, ...)`，靠内容列的稀疏 gram 索引按必选字面量收窄；Infinity 用 **RE2** 所以模式原样下发（不加 `.*(...).*` 包裹），`(?i)` 承担大小写折叠；默认上限 30（与 ES 对齐，让 `grep_chunks` 后端间返回同一页），`ReturnAll` 时上限 10000。

**架构推论：** 「一个数据集一张表」这个模型决定了后面所有的复杂度。**ES 的动态映射让「字段随便加」成为默认，而静态 schema 把「字段必须提前声明」变成一条必须被三处补偿的约束**（`dropUndeclaredFields` 丢未声明、`ensureChunkDataColumn` 补漏、`mapMemoryMessageStorageFields` 改名）。**选择静态 schema 换来的是索引可控（分析器、HNSW 参数、二级索引都能精确指定），代价是每条写入路径都要对 schema 负责。**

**架构推论：** 「维度烧进列名与索引名」是一个看似土但极其有效的方案。它把「换嵌入模型」这个破坏性变更变成了「加一列、加一个索引」的增量操作，于是回滚是免费的（旧列还在）。**代价是列会随模型迭代累积，需要清理策略 —— 而这套代码里看不到清理逻辑，只有 `AddColumns`。**

**架构推论：** Infinity 的 `AdjustChunkPagerank` 与 ES/OS 的那两个实现解决同一个问题，而它们的重试责任方不同：ES/OS 把 `retry_on_conflict=3` 交给服务端（服务端知道版本号），Infinity 必须自己在 SQL 层做 CAS + 读回校验 + 循环。**这揭示了「乐观并发控制该放在哪一层」的答案：能放服务端就放服务端，因为服务端有版本号而客户端只有值。** 而 Infinity 的 CAS 用**值相等**代替版本号，于是「值被改回原值」会被误判为「没人动过」—— 在浮点权重累加这个场景下这个风险可以接受（值单调变化），但它是这个方案的理论缺陷。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/engine/infinity/chunk.go` | `CreateChunkStore` | 取连接后转给 `createChunkStoreWithDB` |
| `internal/engine/infinity/chunk.go` | `createChunkStoreWithDB` | 表存在性分支、加向量列、HNSW / 全文 / 二级索引 |
| `internal/engine/infinity/chunk.go` | `buildChunkTableName` | `{baseName}_{datasetID}`，skill 用 `baseName` |
| `internal/engine/infinity/chunk.go` | `chunkMappingFileName` | 三选一映射文件（skill / memory / 配置） |
| `internal/engine/infinity/common.go` | `analyzerForLanguage` | `language` 唯一真正落地处，白名单只有 czech / slovak |
| `internal/engine/infinity/common.go` | `analyzerLanguages` / `ragAnalyzer` | 极窄白名单与注释警告 |
| `internal/engine/infinity/common.go` | `fulltextIndexName` | `ft_<field>_<analyzer>`，非字母数字转 `_` |
| `internal/engine/infinity/chunk.go` | `InsertChunks` | 反推维度建表、`kb_id` 强制、去重、先删后插、8192 分批 |
| `internal/engine/infinity/chunk.go` | `parserIDFromChunks` | 用 `chunk_data` 反推 `parser=table` |
| `internal/engine/infinity/chunk.go` | `dedupeByChunkID` | 内容寻址 id 的批内去重，后者胜 |
| `internal/engine/infinity/chunk.go` | `dropUndeclaredFields` / `declaredChunkColumns` | 静态 schema 的必然补偿 |
| `internal/engine/infinity/chunk.go` | `UpdateChunks` | 「不许全域」闸、`remove` 伪键、删中间词项字段 |
| `internal/engine/infinity/chunk.go` | `AdjustChunkPagerank` | 分片锁 + CAS + 读回校验 + 3 次重试 |
| `internal/engine/infinity/chunk.go` | `pagerankAdjustLock` | 256 个 mutex 的 FNV-32a 分片 |
| `internal/engine/infinity/chunk.go` | `DeleteChunks` | 同一道闸 + 表不存在返回 `(0, nil)` |
| `internal/engine/infinity/chunk.go` | `Search` | 五道适配、跨表合并、`queryErr` 策略 |
| `internal/engine/infinity/chunk.go` | `shouldDefaultAvailableFilter` | 两个否定构成默认行为 |
| `internal/engine/infinity/chunk.go` | `stripScorePseudoColumns` | 剥掉调用方无条件传的 `_score` |
| `internal/engine/infinity/chunk.go` | `expandSelectWildcards` / `hasPartialWildcard` | `q_*_vec` 被当乘法的补偿 |
| `internal/engine/infinity/chunk.go` | `resolveDenseEF` / `defaultDenseEF` | `ef` 下限 200 与实测依据 |
| `internal/engine/infinity/chunk.go` | `mergeMemoryOrder` 相关（`unionSortColumns` / `sortChunksByFields`） | 记忆索引跨表排序 |
| `internal/engine/infinity/chunk.go` | `SearchByRegexp` | RE2 原样模式 + 30 / 10000 两个上限 |
| `internal/engine/infinity/chunk.go` | `pagerankAdjustRetryCount` / `pagerankAdjustLockCount` / `infinityMaxInsertRows` | 2 / 256 / 8192 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：infinity/chunk.go](https://github.com/infiniflow/ragflow/blob/main/internal/engine/infinity/chunk.go)
- [Infinity 项目主页](https://github.com/infiniflow/infinity)
- [Infinity 文档：HNSW 索引与全文索引](https://infiniflow.org/docs/)
- [HNSW 论文（Malkov & Yashunin, 2018）](https://arxiv.org/abs/1603.09320)
- [RE2 语法说明](https://github.com/google/re2/wiki/Syntax)

## 与主核 / AI 的关系

Infinity 的块表与主核的**堆表（heap table）** 在结构上高度可比，而两者的差异集中在「schema 变更」这个老问题上。

**主核的堆表也是「行 + 静态 schema」**，也有 `ALTER TABLE ADD COLUMN`，也允许「加列但不重写表」（PG 12 之后的 `ADD COLUMN ... DEFAULT` 就是元数据操作）。**Infinity 的 `AddColumns` 与这个是同一类操作**，而 ragflow 用它实现了「换嵌入模型」——**把一次破坏性变更（改列宽度）转化成一次增量变更（加新列）。** 这是一个在数据库里被反复使用的技巧：**永不原地改，总是加新的。**

**「不许全域更新」这道闸，在数据库里对应的是 `UPDATE` / `DELETE` 必须有 `WHERE` 的客户端保护（如 MySQL 的 `--safe-updates`）。** 两者的原理完全相同：**过滤条件被翻译成 SQL 时可能退化成「无条件」，而「无条件」在写操作里就是灾难。** MySQL 的保护是「必须用到索引列」，ragflow 的保护是「条件非空则结果必须受限」——**后者更严，因为它连「条件里的每个键都被丢弃」这种隐蔽情形都覆盖了。**

**「用值 CAS 代替版本号」这个缺陷在数据库里是众所周知的 `ABA` 问题。** 数据库的解法是版本号（MVCC 的 `xmin`）或时间戳，而 Infinity 这里只有值。**ragflow 能接受它，因为权重是单调累加的 —— 值不会「回到原值」。** 这提醒了一条通用判据：**值的相等能否替代版本号，取决于值域是否可能回退。**

**`encode=lvq` 与主核的向量索引（`engine/db` 里的 HNSW 实现）是同一类取舍：用精度换空间。** 区别是主核那份是自研实现、参数可调，而这里是一行硬编码。**一个在「块向量」场景里合理、在「精排」场景里危险的默认值，硬编码进建表路径意味着它无法按数据集调整。**

对 agent 工程的直接价值有三条。其一，**静态 schema 的代价必须被显式补偿**。ES 让你忘了这件事，而一旦换到列式后端，每一条写入路径都要面对「字段没声明」——`dropUndeclaredFields` 与「先删后插」这两个函数不是实现细节，而是那个模型的必然产物。其二，**把维度烧进列名可以让破坏性变更免费回滚**。旧列不动、新列名带新维度，于是「换回小模型」只是改一个参数。其三，**「过滤条件翻译失败」必须被检测并拒绝**。任何「把 A 翻译成 B」的层，都要问「B 会不会退化成不受限」——因为它退化的表现是**成功执行了全表写**。
