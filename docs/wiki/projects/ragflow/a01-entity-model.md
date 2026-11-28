---
project: ragflow
feature: entity-model
title: 实体模型与状态枚举
page_type: feature
schema_version: 1
refs:
  - path: internal/entity/dataset.go
    symbols: [Knowledgebase, ParserType, TaskStatus, PipelineTaskType]
  - path: internal/entity/document.go
    symbols: [Document, DocumentListItem, IngestionStatusWith]
  - path: internal/entity/chat.go
    symbols: [Chat, ChatSession]
---

# 实体模型与状态枚举

## 一句话职责

`internal/entity` 里的每个结构体都同时扮演两个角色：**数据库 schema 的声明**（GORM tag 决定列名、类型、索引）与**状态机的词汇表**（同文件里的字符串枚举决定合法取值），而这两者的耦合方式——tag 里钉的类型、枚举里写的值、以及服务层额外补的默认值——决定了整个服务层怎么读、怎么写、怎么判断一条记录走到了哪一步。

## 机制是什么

这一层是「一次请求进门后最先碰到的层」，它的形态由一个约束决定：**Go 服务和遗留 Python API 共享同一套 MySQL/PostgreSQL schema**（见仓库的 AGENTS.md，双方都不是权威）。

共享同一套表带来三个具体后果，每一个都在代码里留下了痕迹：

1. **列必须宽容地反序列化。** Python 写过 `parser_config`，Go 也要读；而 Python 的 Peewee 曾把 JSON 对象**编码成 JSON 字符串**再存一遍，于是同一列的合法内容有两种深度。`JSONMap.Scan` 因此要先直接解析、失败再脱一层。
2. **时间戳有两套列。** `BaseModel` 同时有 `create_time`（int64 毫秒）与 `create_date`（`time.Time`），四个字段覆盖两个时间点。注释直说这是为了匹配 Peewee 的 `null=True`。
3. **默认值不能交给 GORM。** `Chat` 的数值字段**刻意一个 `default:` tag 都不加**，理由写在注释里（下面细讲）——因为 API 契约允许显式传 0，而 GORM 的 `default:` 会在 Create 时把显式的 0 覆盖成列默认值。

第二个决定性约束是**枚举是 Go 的、不是数据库的**。MySQL 里这些列全是 `varchar`，合法取值靠代码里的 `const` 块维护。于是「状态机」这件事在这套代码里没有单一权威：`TaskStatus` 有六个值，`common` 包里有一套 `CREATED/SCHEDULED/RUNNING/...` 常量，`ParserType` 的十四个值又与实际生效的解析器注册表**不一致**。这三套词表互不引用，靠人维护同步。

## 怎么实现的

**一个文件里挤了六个枚举，且三种命名约定。**

```
type Status string
const (
    StatusValid   Status = "1"
    StatusInvalid Status = "0"
)

type TenantPermission string
const (
    TenantPermissionMe   TenantPermission = "me"
    TenantPermissionTeam TenantPermission = "team"
)

type ParserType string
const (
    ParserTypePresentation ParserType = "presentation"
    ParserTypeGeneral      ParserType = "general"
    ParserTypeNaive        ParserType = "naive"
    ParserTypeKG           ParserType = "knowledge_graph"
    // ... 共 14 个
)

type TaskStatus string
const (
    TaskStatusUnstart  TaskStatus = "0"
    TaskStatusRunning  TaskStatus = "1"
    TaskStatusCancel   TaskStatus = "2"
    TaskStatusDone     TaskStatus = "3"
    TaskStatusFail     TaskStatus = "4"
    TaskStatusSchedule TaskStatus = "5"
)

type PipelineTaskType string
const (
    PipelineTaskTypeParse    PipelineTaskType = "Parse"
    PipelineTaskTypeDownload PipelineTaskType = "Download"
    PipelineTaskTypeRAPTOR   PipelineTaskType = "RAPTOR"
    PipelineTaskTypeWiki     PipelineTaskType = "Wiki"
    // ... 共 7 个
)

type FileSource string
const (
    FileSourceLocal         FileSource = ""
    FileSourceKnowledgebase FileSource = "knowledgebase"
    FileSourceS3            FileSource = "s3"
)
```

四处值得逐个看：

**第一，`Status` 用 `"1"`/`"0"` 而不是 `"valid"`/`"invalid"`。** 这是一个**软删除标记**（`Knowledgebase.Status` 是 `*string`，列上 `default:'1'`）。用单字符数字的好处是列宽 `size:1` 最小、历史数据兼容；代价是**读代码时必须记住 `"1"` 是「活着」**，而几乎所有其它语言的习惯是 `1`/`0` 表示布尔真/假——这里恰好也是真/假，但 `"1"` = 有效、`"0"` = 已删，与「1 为真」的直觉一致，属于运气好的情况。

**第二，`TaskStatus` 的数字**不是**生命周期顺序。**

```
Unstart="0"  Running="1"  Cancel="2"  Done="3"  Fail="4"  Schedule="5"
```

- `Cancel`（"2"）夹在 `Running` 与 `Done` 中间，但它是**终态**；
- `Schedule`（"5"）数值最大，但它在语义上是 `Running` **之前**的状态（已排队未开始）。

**于是「按 status 排序」得到的是一个没有意义的顺序**：已取消的排在已完成之前，已排队的排在最末。任何想用 `ORDER BY status` 表示「进行中的优先」的地方都必须显式列清单，不能用范围比较。这是一处「枚举值被当成字符串存，但被误以为有数值序」的典型陷阱。

**第三，三个枚举三种大小写。** `ParserType` 全小写、`TaskStatus` 是数字字符串、`PipelineTaskType` 是**首字母大写**（`"Parse"`、`"GraphRAG"`、`"RAPTOR"`）。前两个可以比较时不区分大小写，第三个不行。而 `PipelineTaskType` 的值直接落进 `pipeline_operation_log.task_type` 列并被 `WHERE task_type = ?` 过滤，所以**大小写是数据的一部分**，改一次就与历史行不匹配。

**第四，`FileSourceLocal = ""` —— 一个空字符串作为合法成员。**

```
FileSourceLocal FileSource = ""
```

它与「未设置」在 Go 层无法区分（零值就是 `""`），但与 `"knowledgebase"`/`"s3"` 可区分。`Document.SourceType` 的列默认值也是 `default:local` —— **注意这里默认值是字符串 `local`，而枚举成员的值是空串**。两者不一致：数据库会给新行填 `local`，而 Go 代码里的「本地」是 `""`。任何按 `SourceType == FileSourceLocal` 做判断的路径，对从库里读回来的行都会不成立。

**`ParserType` 的十四个值与实际生效的解析器集合对不上，这是这一页最重要的一处。**

`ParserType` 列出 14 个：`presentation / laws / manual / paper / book / qa / table / general / naive / picture / one / audio / email / knowledge_graph`。

而真正决定「哪些 parser_id 合法」的是 `ingestion/pipeline` 里的**内建模板注册表**——它从 `template/ingestion_pipeline_*.json` 嵌入文件加载，`builtinParserOrder` 里列的键是：`general / qa / manual / table / paper / book / laws / presentation / picture / one / audio / email / outline / summary / enriched`。

把两批对照，差异很清楚：

| 只在 `ParserType` 里 | 只在注册表里 |
|---|---|
| `naive` | `outline` |
| `knowledge_graph` | `summary` |
| | `enriched` |

而这三处的处理方式各不相同：

```
func canonicalDatasetParserID(parserID string) (string, error) {
    if parserID == "knowledge_graph" {
        return parserID, nil
    }
    registry, err := pipelinepkg.DefaultRegistry()
    ...
    template, ok := registry.Get(parserID)
    if ok {
        return template.ParserID, nil
    }
    return "", parserIDError()
}
```

- **`knowledge_graph` 被硬编码为「无需注册表校验」的特例** ——它是 `ParserType` 的成员，却不在模板注册表里，所以在校验函数开头单独放行。
- **`naive` 靠别名解析**：

```
func builtinAliases() map[string]string {
    return map[string]string{
        "naive": "general",
    }
}
```

注释说明了原因：「`naive` 是历史默认 parser，它的行为现在住在 `general` 模板里，所以存着 `parser_id="naive"` 的数据集在查表时会解析到 `general`」。别名**不出现在 `List()`/`Refs()`** 里，于是前端只看得到规范模板。
- **`outline`/`summary`/`enriched` 在 `ParserType` 里根本没有成员** ——想用它们必须写字面量字符串。

**结论是：`ParserType` 已经不是权威了。** 它是一份「历史上出现过的 parser_id 的名字清单」，而权威是那个从 JSON 模板加载的注册表。保留它的唯一作用是给 Go 代码一个免字符串的引用方式，而它既不完备（缺三个）也不精确（多两个）。**一个「看起来是权威」的枚举与真正的权威并存，是这个包最容易误导读者的一处。**

`parserIDError()` 还做了一件很实用的事——把合法值列进错误消息：

```
refs := registry.Refs()
switch len(refs) {
case 0:
    return errors.New("invalid parser_id")
case 1:
    return fmt.Errorf("input should be '%s'", refs[0])
default:
    return fmt.Errorf("input should be %s or '%s'", quoteList(refs[:len(refs)-1]), refs[len(refs)-1])
}
```

**它没有维护一张硬编码的合法值表，而是现查注册表再拼消息** ——所以新增一个 JSON 模板，错误消息自动更新。这是「错误消息的内容由权威数据源生成」的正确做法。

**`Knowledgebase` 的 14 组「任务 id + 完成时间」是这套实体里最显眼的结构。**

```
GraphragTaskID       *string    / GraphragTaskFinishAt       *time.Time
RaptorTaskID         *string    / RaptorTaskFinishAt         *time.Time
MindmapTaskID        *string    / MindmapTaskFinishAt        *time.Time
WikiTaskID           *string    / WikiTaskFinishAt           *time.Time
SkillTaskID          *string    / SkillTaskFinishAt          *time.Time
StructureGraphTaskID *string    / StructureGraphTaskFinishAt *time.Time
StructureMindmapTaskID *string  / StructureMindmapTaskFinishAt *time.Time
TimelineTaskID       *string    / TimelineTaskFinishAt       *time.Time
SessionGraphTaskID   *string    / SessionGraphTaskFinishAt   *time.Time
SessionEssenceTaskID *string    / SessionEssenceTaskFinishAt *time.Time
```

**十对列，全部用同一形状**：一个可空的任务 id 指针 + 一个可空的完成时间指针。注释说明它是在镜像 Python 侧的 `_INDEX_TYPE_TO_TASK_ID_FIELD`。

三点评价值得写下来：

1. **「有没有跑过」与「什么时候跑完」用一个指针对表达** ——`ID != nil` 表示有过一次任务，`FinishAt != nil` 表示它完成了。这比一个 `status` 列更省（不用枚举），也更脆弱（「跑失败」与「没跑」无法区分——都只有 id 没有完成时间）。
2. **十对列平铺在一张表上，而不是一张 task 表加一个类型列。** 代价是 `Knowledgebase` 宽度 50+ 列、每新增一种产物就要加两列并改迁移；收益是「列出知识库时一次查询就拿到全部产物状态」，不用 join。
3. **`PipelineTaskType` 的七个值（`Parse`/`Download`/`RAPTOR`/`GraphRAG`/`Mindmap`/`Memory`/`Wiki`）与这十对列是两套不同的「产物枚举」** ——前者在 `pipeline_operation_log.task_type` 里、后者在 `knowledgebase` 的列上，而它们**不是同一个集合**（`Skill`/`StructureGraph`/`StructureMindmap`/`Timeline`/`SessionGraph`/`SessionEssence` 六种产物在 `PipelineTaskType` 里没有成员，`Parse`/`Download`/`Memory` 则在十对列里没有专属列）。这种「产物种类」定义分散在两处且不同步，是这套实体最需要读者自己拼起来的部分。

**`Version` 列有一段很明确的冗余说明。**

```
// Version is the number of the latest dataset version. It is redundant with
// MAX(dataset_version.version) and kept so lists and CAS checks do not need
// an aggregate. It advances only through the version recorder.
Version int64 `gorm:"column:version;not null;default:0" json:"version"`
```

**「冗余于 `MAX(dataset_version.version)`，保留是为了列表与 CAS 检查不必做聚合」** ——这是一个有意为之的物化字段（denormalization）。而「它只通过版本记录器前进」这条约束是**唯一保证它不漂移的机制**；没有别的代码路径能安全地改它。

`Knowledgebase` 还有两处「不是列」的字段：

```
OperatorPermission int32 `gorm:"-" json:"operator_permission,omitempty"`
```

**`gorm:"-"` 让 GORM 忽略这个字段** ——它不是数据库列，是请求期算出来塞进去的（当前用户对这个知识库有什么权限）。同样的写法在 `Chat` 里也有一个 `OperatorPermission`。

**`ToMap` 与结构体字段的不对称，是这一页另一个具体发现。**

`ToMap` 序列化 16 个固定键，外加按非空条件补 9 个键。而结构体里有 14 组任务 id/时间——**`ToMap` 只输出了 3 组**（GraphRAG、RAPTOR、Mindmap）：

```
if kb.GraphragTaskID != nil { result["graphrag_task_id"] = *kb.GraphragTaskID }
if kb.RaptorTaskID != nil { result["raptor_task_id"] = *kb.RaptorTaskID }
if kb.MindmapTaskID != nil { result["mindmap_task_id"] = *kb.MindmapTaskID }
```

**Wiki / Skill / StructureGraph / StructureMindmap / Timeline / SessionGraph / SessionEssence 七种产物的任务状态，`ToMap` 一个都不输出。** 也没有输出 `status`、`user_id`、`tenant_embd_id`、`version`、`wiki_task_id` 等。

对照 `KnowledgebaseListItem` 就更清楚了：列表结构里有 `PipelineVersionID`、`TenantEmbdID`，而 `ToMap` 都没有。**于是「同一个知识库」在不同接口上返回的字段集不同，而这没有任何地方声明谁该有什么。** 从代码看不出是有意裁剪还是漏了——`ToMap` 更像是「随着字段增加而没跟上」的那一方。

**一个有意思的派生字段：`ToMap` 输出的是「关键词权重」，而数据库存的是「向量权重」。**

```
"similarity_threshold":       kb.SimilarityThreshold,
"keywords_similarity_weight": 1 - kb.VectorSimilarityWeight,
```

**库里一个数、API 上另一个数，且后者是前者的补数。** 命名也换了（`vector_similarity_weight` → `keywords_similarity_weight`）。这解释了 `KnowledgebaseListItem` 里那个 `gorm:"-"`… 不，那个是 `json:"-"`：

```
VectorSimilarityWeight float64 `json:"-"`
```

**列表结构里向量权重根本不输出**，而详情/`ToMap` 输出它的补数。三处（库列名、`ToMap` 键名、列表结构）对同一个概念用了三种处理。宁可输出补数也不输出原值的理由大概是 Python 时代的 API 兼容——这是一个典型的「历史契约压倒了直白」的例子。

**`Document` 的核心是 `IngestionStatusWith`，它把「文档状态」变成一个需要推理的函数。**

```
type Document struct {
    ...
    Status            *string    `gorm:"column:status;size:1;index;default:'1'" json:"status,omitempty"`
    RestoredVersion   *int64     `gorm:"column:restored_version" json:"restored_version,omitempty"`
    RestoredFromParse bool       `gorm:"-" json:"-"`
    BaseModel
}
```

注意 **`Document` 里没有「摄取状态」列** ——文档的摄取状态不存在文档表上，而在 `ingestion_task` 表里（`IngestionTask.Status`）。于是「列表里那一列 `ingestion_status`」必须现算：

```
func (d *Document) IngestionStatusWith(taskStatus string) string {
    if d == nil {
        return common.UNSTART
    }
    switch taskStatus {
    case common.CREATED, common.SCHEDULED, common.RUNNING, common.STOPPING:
        return taskStatus
    }
    if d.RestoredVersion != nil {
        if d.ChunkNum > 0 || d.RestoredFromParse {
            return common.COMPLETED
        }
        return common.UNSTART
    }
    if taskStatus == "" {
        return common.UNSTART
    }
    return taskStatus
}
```

**这段的判断顺序就是它的全部语义，而这个顺序是刻意的：**

1. **先判「跑着的」四种状态**（`CREATED`/`SCHEDULED`/`RUNNING`/`STOPPING`）——只要任务在飞，任务状态就赢，不管内容是从哪来的。
2. **再判「恢复过的文档」**（`RestoredVersion != nil`）——此时任务状态**不可信**，因为恢复不跑流水线，任务仍描述上一次跑到哪。所以由**内容**决定终态：`ChunkNum > 0` 或 `RestoredFromParse` 算完成，否则算未开始。
3. **最后**，非恢复文档按任务状态，空则算未开始。

docstring 把这条讲清了：

> It is the run's status, except for content a restore put back: a restore does not run the pipeline, so the task still describes whatever ran last, while the restored content is the version's. For those documents - the ones carrying `RestoredVersion` - the content decides the terminal state: chunks came back means completed, none came back means unstarted. A run in flight still wins, and a document no restore touched returns exactly the value it always did, so no ingestion outcome is re-read from the content counters.

**注意最后那句「a document no restore touched returns exactly the value it always did」** ——这是一条**向后兼容承诺**：新加的恢复逻辑不能改变普通文档的行为。这类承诺写进 docstring 而不是靠测试，说明作者知道这段逻辑最容易被后续改动误伤。

**它还有一个 SQL 双胞胎，docstring 明确点名：**

> `buildDocumentListIngestionStatusExpression` (dao/document.go) is the same rule in SQL, for the list that filters and sorts on it in the database.

**同一套三支判断在 Go 里一遍、在 SQL 里一遍。** 这是「列表要能在数据库里过滤和排序，所以状态必须是可下推的表达式」的直接后果——如果只在 Go 里算，就没法 `WHERE ingestion_status = 'COMPLETED'`。

两处实现必须逐支对齐，而代码里没有任何机制保证。docstring 的交叉引用是唯一的防线，而且它指向的是**同一条规则的另一种语言实现**，这类同步最难靠评审发现。

`Document` 里另一处值得看的是 `RestoredVersion` 的长注释，它把「这是标记而不是状态」说得非常具体：

> It is a marker on its own, not a status: with `ChunkNum` it answers what a restore produced - a value with `ChunkNum > 0` means the version's chunks came back, a value with `ChunkNum == 0` means the version held none (or the replay left the document without any). The run outcome stays on the ingestion task, which a restore deliberately does not touch.

**「一个字段 + 另一个字段的组合才能回答一个问题」** ——`RestoredVersion` 单独只能回答「恢复过」，配上 `ChunkNum` 才能回答「恢复出了东西没有」。而 `RestoredFromParse`（`gorm:"-"`，不是列）是为了让「恢复出的历史里记录了一次没有块的解析」这种情况也能表达——**它不落盘，由读端在填充 `RestoredVersion` 时一并设置**。

**`DocumentListItem` 是同一个行的第二种形状，而它多了连接出来的字段。**

```
type DocumentListItem struct {
    ...
    PipelineName    *string `gorm:"column:pipeline_name" json:"pipeline_name,omitempty"`
    Nickname        *string `gorm:"column:nickname" json:"nickname,omitempty"`
    IngestionStatus *string `gorm:"column:ingestion_status" json:"ingestion_status,omitempty"`
    PipelineLogID  *string `gorm:"column:pipeline_log_id" json:"-"`
    Status         *string `gorm:"column:status" json:"status,omitempty"`
    CreateTime     *int64  `gorm:"column:create_time" json:"create_time,omitempty"`
    ...
}
```

四处可对照：

1. **它不嵌 `BaseModel`，而是把四个时间字段平铺** ——因为这是「查询结果的行」，不是「表模型」，不需要 GORM 的时间钩子（`BeforeCreate`/`BeforeUpdate`）。
2. **`PipelineVersionID` 的注释解释了它为什么必须出现在列表里**：「Returned by the list so the reopened change-parser dialog can show the saved pin instead of re-adopting the latest version.」 ——**列表返回一个字段，是为了让前端某个对话框能显示「已钉住的版本」**。这是一条很具体的需求，也说明了「同一份数据在列表与详情里出现的理由可以完全不同」。
3. **`IngestionStatus` 与 `Status` 两个字段并存** ——一个是摄取状态（由 `IngestionStatusWith` 或 SQL 表达式算出），一个是软删除标记。两个名字里的 `Status` 含义完全不同，读代码时极易混淆。
4. **`PipelineLogID` 是 `json:"-"`** ——列存在、能读进来，但不出现在 JSON 里。它只在服务内部用来定位日志行。

**`Chat` 的那段 GORM 注释是这一页最「实战」的内容。**

```
// NOTE: No `default:` GORM tags here. The service layer (chat.go Create)
// supplies sensible defaults (0.1 / 0.3 / 6 / 64 / 1024 / "1") when a field is
// omitted, and honors an explicitly provided zero value. A GORM `default:`
// tag would force GORM to overwrite an explicit zero (e.g.
// similarity_threshold=0) with the column default during Create, breaking
// the API contract that permits 0.
//
// TopK carries no `type:` tag either: the MySQL driver derives an integer
// column from the Go field's width (int64 -> bigint) and ignores the tag, so
// declaring int only made the manual migration and AutoMigrate rewrite the
// column in turn on every startup.
SimilarityThreshold    float64   `gorm:"column:similarity_threshold" json:"similarity_threshold"`
TopN                   int64     `gorm:"column:top_n" json:"top_n"`
```

**两段注释记录了两个具体的 GORM 陷阱，都值得直接抄进任何用 GORM 的项目：**

**陷阱一：`default:` tag 会让「显式传零值」失效。** GORM 的 `default:` 不只是 DDL 层的默认，它在 Create 时会把「Go 零值」当成「未提供」并替换成列默认值。于是 `similarity_threshold: 0`（用户明确要求 0）会被写成 0.1。**解法是把默认值的职责交给服务层，让「有没有传」与「传了什么值」在请求结构里可区分（指针或 `omitempty`）。**

**陷阱二：`type:` tag 在 MySQL 驱动上被忽略，但会引发迁移抖动。** 声明 `type:int` 时驱动按 Go 字段宽度推导（`int64` → `bigint`）而忽略 tag，于是手工迁移与 `AutoMigrate` 会**每次启动都把列改回去**——因为两边的期望不同。解法是干脆不写 tag，让驱动推导成为唯一真相。

**`Chat` 的模型引用有一处「防重命名」的设计。**

```
// TenantDecisionID mirrors TenantLLMID: the resolved row id behind the
// requested decision_id, so a later rename of the model value does not
// silently point this chat at a different model.
TenantDecisionID *string `gorm:"column:tenant_decision_id;size:35;index" json:"tenant_decision_id,omitempty"`
DecisionID       *string `gorm:"column:decision_id;size:256" json:"decision_id,omitempty"`
```

**每对「用户填的名字 + 解析出的行 id」这是第二种冗余存储**（`LLMID`/`TenantLLMID`、`RerankID`/`TenantRerankID`、`DecisionID`/`TenantDecisionID`）。存行 id 的理由写得很直接：**模型值被改名之后，这个对话不会静默指向另一个模型**。

而 `DecisionID` 的注释说明了空值的语义：「empty means 'inherit the tenant default', which is what almost every chat wants」。于是「继承」用空值表达，不用哨兵值。

**`ChatSession` 的 `Message` 是唯一一个「不落在这张表里」的字段。**

```
type ChatSession struct {
    ID       string            `gorm:"column:id;primaryKey;size:32" json:"id"`
    DialogID string            `gorm:"column:dialog_id;size:32;not null;index" json:"dialog_id"`
    Name     *string           `gorm:"column:name;size:255;index" json:"name,omitempty"`
    Message  json.RawMessage   `gorm:"-" json:"message,omitempty"`
    Source   ChatSessionSource `gorm:"column:source;size:32;not null;default:normal" json:"source"`
    BaseModel
}
```

注释很短但准确：「Messages are hydrated from the ordered message table.」——**`Message` 是 `gorm:"-"` 的，它不映射列**，而是查询时从有序消息表拼装进来。于是「会话」这个概念在这套 schema 里是**拆开存的**（会话元数据一张表、消息另一张有序表），而这个结构体是拼装后的视图。

`ChatSessionSource` 只有两个值（`normal` / `chat_channel`），且是**列上唯一带 `default:` 的枚举**（`default:normal`）——与 `Chat` 的数值字段相反，这里用默认值是安全的，因为它不是可由用户显式设成零值的字段。

**`BaseModel` 的时间字段要读一段注释才明白为什么是四列。**

```
// All time fields are nullable to match Python Peewee model (null=True)
type BaseModel struct {
    CreateTime *int64     `gorm:"column:create_time;index" json:"create_time,omitempty"`
    CreateDate *time.Time `gorm:"column:create_date;index" json:"create_date,omitempty"`
    UpdateTime *int64     `gorm:"column:update_time;index" json:"update_time,omitempty"`
    UpdateDate *time.Time `gorm:"column:update_date;index" json:"update_date,omitempty"`
}
```

**同一时刻存两份：一份 int64 毫秒、一份 `time.Time`。** 理由是「匹配 Peewee 的 `null=True`」——也就是 Python 侧建的表就是这么定义的，Go 不能改。

代价很具体：**两列可能在同一次写入里取到不同时刻**。`autoModelTime()` 用 `time.Now().Local()` 同时给出毫秒与截断到秒的时间，所以写入时是一致的；但若有人只改 `update_time`（很多 `Update()` 调用只传一个列名），两列就会不同步。而 `BaseModel` 的 `BeforeUpdate` 钩子会同时写两列，这让同步在「走 ORM」的路径上成立。

**`DBTimeMillis` 解决的是「多实例时钟不一致」。**

```
func DBTimeMillis(db *gorm.DB) (int64, error) {
    ...
    switch name := db.Dialector.Name(); name {
    case "mysql":
        expr = "CAST(UNIX_TIMESTAMP(NOW(3)) * 1000 AS SIGNED)"
    case "postgres":
        // clock_timestamp() rather than now(): now() is the transaction start,
        // and two statements of one history write would then share a moment even
        // when they are seconds apart.
        expr = "CAST(EXTRACT(EPOCH FROM clock_timestamp()) * 1000 AS BIGINT)"
    case "sqlite":
        expr = "CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER)"
    default:
        return 0, fmt.Errorf("no database clock expression for dialect %q", name)
    }
}
```

**注释把问题的形态讲得很清楚**：两个应用实例对「现在」的看法不同，快实例给 chunk 版本打的戳会看起来比慢实例晚打的 dataset 版本更新，于是恢复会重放错的那一半历史。**读数据库的时钟把所有写者放到一条时间线上。**

三处值得记的细节：

1. **Postgres 用 `clock_timestamp()` 而不是 `now()`。** 理由写在注释里：`now()` 是**事务开始时刻**，一次历史写入里的两条语句会因此共享同一时刻，即使它们相隔若干秒。
2. **三种方言三种表达式，未知方言返回错误而不是猜。** 注释说明这是为了让调用方**退回本地时钟**而不是让写入失败——「graceful degradation 的决策权在调用方」。
3. **`julianday('now') - 2440587.5`** 是 Unix epoch 的儒略日偏移，`86400000` 是每天的毫秒数。这个式子对 SQLite 是必需的（它没有 `UNIX_TIMESTAMP`）。

**`JSONMap.Scan` 的「先直接解析、失败再脱一层」是共享 schema 的直接证据。**

```
// Python/legacy rows sometimes stored a JSON object as a JSON-encoded
// string ("{...}"). Try parsing directly first, then unwrap one level.
// Decode into fresh maps: Unmarshal into a reused non-nil map keeps
// stale keys from a previous Scan.
var fresh JSONMap
if err := json.Unmarshal([]byte(raw), &fresh); err == nil {
    *j = fresh
    return nil
}
var quoted string
if err := json.Unmarshal([]byte(raw), &quoted); err != nil {
    return json.Unmarshal([]byte(raw), j)
}
```

四处：

1. **「Python/legacy rows sometimes stored a JSON object as a JSON-encoded string」** ——同一个列有两种合法深度，读端必须都认。这是双栈共享 schema 的代价，而且不会随时间消失（历史行一直在）。
2. **注释第二句解释了「解到新 map」的理由**：往已非 nil 的 map 上 `Unmarshal` 会**保留上一次 Scan 的旧键** ——因为 `Unmarshal` 是增量覆盖而非替换。这是一个非常容易漏的细节，且症状是「某些行读出来带有上一行的字段」。
3. **空白串被当 nil**（`strings.TrimSpace(raw) == ""`）——覆盖「列里存了空串」这种情况。
4. **`GormDataType()` 返回 `"longtext"`** ——它决定 GORM 建列时用什么类型。用 `longtext` 而不是 MySQL 的 `json` 类型，是因为要与 Python 侧的表定义一致（Peewee 用的就是 text）。

## 关键取舍

**源码事实：** `internal/entity/dataset.go` 一个文件里有六个枚举（`Status` / `TenantPermission` / `ParserType` / `TaskStatus` / `PipelineTaskType` / `FileSource`），且三种命名约定：小写词、数字字符串、首字母大写。

**源码事实：** `TaskStatus` 的六个值是 `"0"`–`"5"`，其中 `Cancel="2"` 是终态却排在 `Done="3"` 之前，`Schedule="5"` 是前置状态却数值最大——**枚举值的数值序不等于生命周期序**。

**源码事实：** `ParserType` 的 14 个值与内建模板注册表（`builtinParserOrder` 列出的 15 个）不一致：`ParserType` 独有 `naive` / `knowledge_graph`，注册表独有 `outline` / `summary` / `enriched`。`knowledge_graph` 在校验函数里被硬编码放行，`naive` 靠 `builtinAliases` 映射到 `general`。

**源码事实：** `FileSourceLocal = ""`（空串是合法成员），而 `Document.SourceType` 的列默认值是 `default:local`——库里写的 `local` 与 Go 侧的 `""` 不相等。

**源码事实：** `Knowledgebase` 有十组「任务 id + 完成时间」可空指针对（Graphrag / Raptor / Mindmap / Wiki / Skill / StructureGraph / StructureMindmap / Timeline / SessionGraph / SessionEssence）；`ID != nil` 表示跑过，`FinishAt != nil` 表示跑完了，因此「失败」与「没跑」在数据上不可区分。

**源码事实：** `PipelineTaskType` 的七个值与那十组列**不是同一个集合**——`Skill`/`StructureGraph`/`StructureMindmap`/`Timeline`/`SessionGraph`/`SessionEssence` 在枚举里没有成员，`Parse`/`Download`/`Memory` 在十组列里没有专属列。

**源码事实：** `Knowledgebase.Version` 冗余于 `MAX(dataset_version.version)`，保留目的是让列表与 CAS 检查免于聚合，且「只通过版本记录器前进」。

**源码事实：** `Knowledgebase.ToMap` 只输出 3 组任务状态（GraphRAG / RAPTOR / Mindmap），另外 7 种产物的任务状态以及 `status` / `user_id` / `tenant_embd_id` / `version` 都不输出；`ToMap` 输出 `keywords_similarity_weight = 1 - VectorSimilarityWeight`（补数、换名），而 `KnowledgebaseListItem` 对同一字段用 `json:"-"` 完全不输出。

**源码事实：** `Document` **没有摄取状态列**；摄取状态活在 `ingestion_task.status`，列表里的那一列由 `IngestionStatusWith`（Go）与 `buildDocumentListIngestionStatusExpression`（SQL）各算一遍，docstring 显式交叉引用两者。

**源码事实：** `IngestionStatusWith` 的判断顺序是「在飞的四种状态优先 → 恢复过的文档由内容（`ChunkNum` 与 `RestoredFromParse`）决定终态 → 其余按任务状态、空则未开始」，docstring 承诺「非恢复文档的行为与以前完全一致」。

**源码事实：** `Document.RestoredFromParse` 是 `gorm:"-"`（不是列），由读端在填充 `RestoredVersion` 时一并设置；`ChatSession.Message` 同为 `gorm:"-"`，由有序消息表在查询时拼装。

**源码事实：** `Chat` 的数值字段**刻意不带 `default:` tag**：服务层补默认值且尊重显式零值，因为 GORM 的 `default:` 会在 Create 时用列默认覆盖显式 0；`TopK` 也刻意不带 `type:` tag，因为 MySQL 驱动按 Go 字段宽度推导并忽略 tag，声明 `int` 只会让手工迁移与 `AutoMigrate` 每次启动互相改写列。

**源码事实：** 模型引用的三对冗余（`LLMID`/`TenantLLMID`、`RerankID`/`TenantRerankID`、`DecisionID`/`TenantDecisionID`）都存「用户填的值 + 解析出的行 id」，目的是模型被改名后对话不会静默指向另一个模型。

**源码事实：** `BaseModel` 用四列表达两个时间点（`create_time`/`create_date`、`update_time`/`update_date`），注释说是为匹配 Peewee 的 `null=True`。`DBTimeMillis` 从数据库读时钟（MySQL `NOW(3)`、Postgres `clock_timestamp()`、SQLite 儒略日），未知方言返回错误让调用方退回本地时钟。

**源码事实：** `JSONMap.Scan` 先直接解析、失败再脱一层字符串，以兼容「Python 把 JSON 对象编码成 JSON 字符串」的历史行；且刻意解到新 map，因为往非 nil map 上 `Unmarshal` 会保留旧键。

**架构推论：** `ParserType` 与注册表并存，是「枚举曾经是权威、后来权威搬到数据文件、枚举没被删」的典型残留。它的危害不是错误，而是**误导**：读到 `ParserTypeKG` 的人会以为 `knowledge_graph` 是一个受支持的解析器，实际上它只在校验函数的一个特例分支里被放行。**当权威搬到可配置数据源（JSON 模板、DB 表、配置文件）时，原来的枚举必须要么删掉、要么明确降级为「历史值清单」并停止被新代码引用。**

**架构推论：** 「任务 id + 完成时间」这种二元组表达状态，在「失败」与「未开始」之间留了一个空洞。补法有两种：加一个 status 列（三态齐全但多一列），或让失败也写完成时间（语义被污染）。这套代码选了不加，代价是「产物生成失败」这件事在知识库表上看不出来——只能去 `pipeline_operation_log` 查。**凡是用「可空字段的有无」表达状态的 schema，都必须逐项检查「缺失」到底有几种含义。**

**架构推论：** 「同一规则在 Go 与 SQL 各写一遍」是「列表要能在数据库里过滤排序」逼出来的。它无法靠注释保证同步，只能靠**把规则写成可读的三支结构并在两处保持同样的分支顺序**。更稳的做法是把分支顺序也变成数据（例如一张 status 映射表），但那样 SQL 就得查表，复杂度上去了。当前这个取舍在「规则简单且稳定」时是合理的；一旦规则开始长，就必须改成单一实现（要么全下推，要么全在 Go 层算完再过滤）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/entity/dataset.go` | `Knowledgebase` | 知识库主表（50+ 列：配置、三个计数器、十组任务状态、版本号） |
| `internal/entity/dataset.go` | `ParserType` | 14 个历史 parser_id 值（与实际模板注册表不一致，非权威） |
| `internal/entity/dataset.go` | `TaskStatus` | 六个数字字符串任务状态（数值序 ≠ 生命周期序） |
| `internal/entity/dataset.go` | `PipelineTaskType` | 七种流水线任务类型（首字母大写，值直接落列并被 WHERE 匹配） |
| `internal/entity/dataset.go` | `Status` / `TenantPermission` / `FileSource` | 软删除标记（`"1"`/`"0"`）、权限级别、文件来源（空串为本地） |
| `internal/entity/dataset.go` | `Knowledgebase.ToMap` | 响应序列化（部分字段、任务状态只输出三种、权重输出补数） |
| `internal/entity/dataset.go` | `KnowledgebasesLanguage` | 多库查询的语言合并：全部一致才取，混合则回退空串 |
| `internal/entity/document.go` | `Document` | 文档主表；无摄取状态列，恢复相关标记（`RestoredVersion` / `RestoredFromParse`） |
| `internal/entity/document.go` | `DocumentListItem` | 列表行（带 join 字段：pipeline_name / nickname / ingestion_status） |
| `internal/entity/document.go` | `IngestionStatusWith` | 三支状态推导（在飞优先 → 恢复由内容决定 → 其余按任务状态） |
| `internal/entity/chat.go` | `Chat` | 对话配置表（映射 `dialog` 表）；无 `default:`/`type:` tag 的两段理由 |
| `internal/entity/chat.go` | `ChatSession` | 会话表（映射 `conversation`）；`Message` 由消息表拼装（`gorm:"-"`） |
| `internal/entity/chat.go` | `ChatSessionSource` | 会话来源枚举（`normal` / `chat_channel`） |
| `internal/entity/base.go` | `BaseModel` | 四列时间戳（毫秒 + `time.Time`），为兼容 Peewee |
| `internal/entity/base.go` | `DBTimeMillis` | 从数据库读时钟，统一多实例时间线 |
| `internal/entity/base.go` | `JSONMap` / `JSONSlice` | JSON 列的 `Valuer`/`Scanner`，兼容 Python 的双层编码 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [GORM 文档：模型定义与 tag](https://gorm.io/docs/models.html)
- [GORM 文档：创建钩子与默认值](https://gorm.io/docs/create.html)
- [ragflow 源码：internal/entity/dataset.go](https://github.com/infiniflow/ragflow/blob/main/internal/entity/dataset.go)

## 与主核 / AI 的关系

实体层与主核（多模态数据库内核）的交集在**「schema 由谁定义」**这个根本问题上，两者给出了不同的答案。四条经验。

**第一，当权威从代码搬到数据，旧枚举必须删或降级。** 这里 `ParserType` 曾是权威，权威搬到 `template/ingestion_pipeline_*.json` 后枚举没删，于是出现「枚举有 14 个、实际生效 15 个、两者交集只覆盖一部分」的局面。主核里同类的迁移是「格式白名单从 `#define` 搬进系统表」——**迁移完成后旧的 `#define` 必须删掉，否则代码里会长期存在两个互不一致的合法值集合，而新人读到的往往是错的那个。**

**第二，可空字段表达状态时，「缺失」的含义必须逐项枚举。** 这里用「任务 id + 完成时间」两列表达产物状态，`FinishAt == nil` 同时意味着「还在跑」「失败了」「从没跑过」三种情况。主核里元组的可见性状态、索引的构建状态都有同类风险：**凡是靠「某个字段有没有值」推断状态的地方，都要问一句「这个缺失有几种原因、是否需要区分」**。三种以上就必须加状态列，否则判断会分散到各处且互相矛盾。

**第三，同一条规则在两种语言里写两遍时，分支顺序也是契约。** `IngestionStatusWith` 与它的 SQL 双胞胎必须逐支对齐，包括「在飞优先 → 恢复由内容决定 → 其余按任务」这个顺序——**换一个顺序在多数数据上仍然正确，只在少数边界数据上出错**，所以评审极难发现。主核里做「下推到 SQL 的过滤条件」时同理：一旦有了第二份实现，就要么让它从第一份生成，要么把它纳入必有对照测试的范围。

**第四，ORM 层的「默认值」是 API 契约的隐形破坏者。** 这里 `Chat` 因为「显式传 0 必须生效」而拒绝使用 `default:` tag，把默认值的职责交给服务层；同时因为 MySQL 驱动忽略 `type:` tag 而干脆不写它，避免迁移与 `AutoMigrate` 每次启动互相改写。主核里凡是「CREATE TABLE 带默认值」的场景都有同一类问题：**DDL 默认值只应服务「列必须非空」的约束，不应承担「业务默认值」；后者必须由写入路径显式决定，否则「用户传了默认值所代表的那个值」与「用户没传」永远无法区分。**
