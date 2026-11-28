---
project: ragflow
feature: metadata-filter
title: 元数据过滤条件生成
page_type: feature
schema_version: 1
refs:
  - path: internal/service/metadata_filter.go
    symbols: [ApplyMetaFilter, GenMetaFilter, MetaFilterResult]
  - path: internal/service/metadata_filter.go
    symbols: [MetadataConditionToDocIDs]
---

# 元数据过滤条件生成

## 一句话职责

把用户的自然语言问题交给 LLM，让它**自己产出 `{key, op, value}` 三元组**和一个 `and`/`or` 逻辑，再用这些条件在文档元数据的倒排表上筛出「这次检索允许看的文档集合」；而整条链路的关键不在生成，在**生成失败或筛不出东西时如何退化为「不过滤」而不是「返回零结果」**。

## 机制是什么

元数据过滤是一个「自然语言 → 结构化查询」的转换，而这套实现要处理一个根本性的不对称：

**过滤失败有两种方向，而它们的严重性完全不同。**

- **过宽**（该过滤但没过滤）：返回的文档多于用户想要的，检索结果里有噪声，但**答案通常还是对的**。
- **过窄**（不该过滤却过滤成零结果）：返回零个文档，模型没有上下文，**答案一定错**。

所以整套设计的默认方向是「宁可不过滤」。这个方向不是靠一个布尔开关表达的，而是靠**一个三种返回值的约定**（`nil` / `[]string{NoMatchDocIDSentinel}` / 其它），而这个约定在 `ApplyMetaDataFilter` 的 docstring 里被写得非常明确：

```
// The return value carries three distinct outcomes:
//
//   - nil -- no metadata narrowing: the generated filter produced no
//     conditions, or conditions that selected nothing. Callers keep their own
//     document scope and search it unfiltered; a filter that fails to narrow is
//     recoverable, one that narrows to the wrong documents is not.
//   - []string{NoMatchDocIDSentinel} -- manual conditions that match no document.
//     Only manual mode produces the sentinel, and it means zero results on
//     purpose: the user asked for those exact conditions.
//   - any other slice -- the documents the filter selected.
//
// nil must never be turned into the sentinel: that would answer "the metadata
// was unusable" with "no documents exist", which is the one outcome the user
// did not ask for.
```

**三种返回值的语义区别是「谁的意图」。** `nil` 是「系统没能理解」——退回不过滤；哨兵是「用户明确要求这些条件」——零结果是正确答案；其它是正常筛出。

第二个机制是**三级模式**（`auto` / `semi_auto` / `manual`），而它们的区别精确地是「谁来决定过滤条件」：

| 模式 | 条件由谁产生 | 失败怎么办 |
|---|---|---|
| `auto` | LLM 从全部元数据键里自由选 | 返回 `nil`（不过滤） |
| `semi_auto` | LLM 只能从用户勾选的键里选 | 返回 `nil`（不过滤） |
| `manual` | 用户直接给定条件 | 返回哨兵（零结果） |

第三个机制是**过滤可以在两个地方执行**：推下到文档引擎（ES/Infinity 的元数据索引），或退回内存里算。而这两条的语义差异用一个 `nil` / 空切片的分界来表达（下面细讲）。

## 怎么实现的

**`GenMetaFilter` 的第一步是「把元数据的键值结构压成「键 → 值列表」」。**

```go
func GenMetaFilter(ctx context.Context, chatModel *modelModule.ChatModel, metaData common.MetaData, question string, constraints map[string]string, descriptions map[string]string) (*MetaFilterResult, error) {
    if chatModel == nil {
        return nil, fmt.Errorf("chat model is nil")
    }

    if len(metaData) == 0 {
        return &MetaFilterResult{Conditions: []MetaFilterCondition{}, Logic: "and"}, nil
    }

    // Build metadata structure for prompt
    metaDataStructure := make(map[string][]string)
    for key, values := range metaData {
        keys := make([]string, 0, len(values))
        for k := range values {
            keys = append(keys, k)
        }
        metaDataStructure[key] = keys
    }

    metaDataJSON, _ := json.Marshal(metaDataStructure)
```

**`common.MetaData` 的形状是「键 → 值 → 文档 id 列表」**（一个倒排索引），而提示词只需要「键 → 值」——**丢掉了文档 id**，因为提示词的目的是让模型知道「有哪些值可选」，不是知道「哪些文档匹配」。

**注意这里没有排序**：`for k := range values` 直接收集 map 的键。于是**同一份元数据在同一进程内多次调用会生成不同的提示词顺序**（Go 的 map 遍历顺序是随机的）。若这条链路上有任何缓存（按提示词哈希），命中率会随机波动。与 `kbPrompt` 里那个 `sort.Strings` 的处理对照，这里是**漏了排序的一处**。

**空元数据时返回「空条件 + and」，而不是 `nil` 或错误。**

```go
if len(metaData) == 0 {
    return &MetaFilterResult{Conditions: []MetaFilterCondition{}, Logic: "and"}, nil
}
```

**这个返回值在 `ApplyMetaDataFilter` 里的处理是：`runMetadataFilter([])` → 因为 `len(conditions) > 0` 为假，跳过推下 → `ApplyMetaFilter(metaData, [], "and")` → 因为 `len(filters) == 0` 返回 `[]string{}`** → `constrainDocIDs(baseDocIDs, [])` → 返回 `[]string{}` → `len(docIDs) == 0` → **`return nil`（不过滤）。

**所以「没有元数据」这条路径最终也归到「不过滤」，符合设计意图。** 但它绕了一个大圈才到那里——从「空条件」到「空 doc id」再到「识别为空」再到 `nil`。这条链路的每一环都是对的，但它让「空元数据 = 不过滤」这件事依赖了四个函数的实现细节。

**提示词来自一个嵌入式模板，而渲染器手写了「伪 Jinja 条件块」的处理。**

```go
func renderMetaFilterTemplate(currentDate, metadataKeys, question, constraints, descriptions string) (string, error) {
    templateContent, err := (prompts.EmbeddedPromptLoader{}).Load("meta_filter")
    if err != nil { return "", err }

    // Resolve the conditional blocks before substituting any value, so text
    // coming from the question or a dataset owner's description is never
    // parsed as template syntax.
    result := templateContent
    for _, block := range []struct {
        re   *regexp.Regexp
        keep bool
    }{
        {metaFilterConstraintsBlock, constraints != ""},
        {metaFilterDescriptionsBlock, descriptions != ""},
    } {
        if block.keep {
            result = block.re.ReplaceAllString(result, "$1")
        } else {
            result = block.re.ReplaceAllString(result, "")
        }
    }

    // A single pass: a substituted value is not scanned for further placeholders.
    result = strings.NewReplacer(
        "{{ current_date }}", currentDate,
        "{{ metadata_keys }}", metadataKeys,
        "{{ user_question }}", question,
        "{{ constraints }}", constraints,
        "{{ metadata_descriptions }}", descriptions,
    ).Replace(result)

    result = extraNewlinesRe.ReplaceAllString(result, "\n\n")
    return strings.TrimSpace(result), nil
}
```

**四处设计点，每一处都在处理一个具体的注入风险或格式问题：**

**第一，条件块先解、值后替换。** 注释写明了理由：「so text coming from the question or a dataset owner's description is never parsed as template syntax」。**如果先替换 `{{ user_question }}`，用户问题里若含 `{% if ... %}` 就会被当成模板语法处理** ——这是一个真实的注入面，而修法是把「结构解析」与「值替换」分成两个阶段。

**第二，`strings.NewReplacer(...).Replace(result)` 是单趟替换。** 注释：「A single pass: a substituted value is not scanned for further placeholders」。**这是与 `strings.ReplaceAll` 链式调用的关键差别**：链式替换会让「第一个替换的结果再被后续规则扫描」，于是「用户问题里写了 `{{ metadata_keys }}`」会被二次展开；`Replacer` 一次扫描所有模式，替换结果不再被扫描。

**这是同一个注入面的第二道防御**，与第一道互补。

**第三，`metaFilterIfBlock` 的正则把标签后的换行也吃掉了。**

```go
func metaFilterIfBlock(name string) *regexp.Regexp {
    return regexp.MustCompile(`(?s)\{%\s*if\s+` + name + `\s*%\}\n?(.*?)\{%\s*endif\s*%\}\n?`)
}
```

**`\n?` 在两端各出现一次，注释说明了意图：「together with the newline after each tag so a dropped or unwrapped block leaves no stray blank line (Jinja's trim_blocks)」** ——这是在模仿 Jinja 的 `trim_blocks` 行为。而 `extraNewlinesRe`（`\n{3,}` → `\n\n`）是第二道格式清理。

**第四，模板加载失败时返回错误，而 `genMetaFilterPrompt` 把它降级成空提示词。**

```go
func genMetaFilterPrompt(metaDataJSON, question, constraintsJSON, descriptionsJSON, currentDate string) string {
    prompt, err := renderMetaFilterTemplate(...)
    if err != nil {
        common.Warn("Failed to render meta filter template, using fallback", zap.Error(err))
        // Fallback to empty prompt
        return ""
    }
    return prompt
}
```

**「空提示词」这个降级很危险**：它会被作为 `systemPrompt` 传给模型，于是模型收到的是一个空系统提示 + 「Generate filters:」。**模型很可能返回一些自创的 JSON 或者干脆不返回 JSON**，然后走解析失败路径，最终返回空条件 → 不过滤。

**这条链路的结果是对的（不过滤），但过程浪费了一次 LLM 调用，而日志里只有一条 warn。** 更直接的降级是在加载失败时直接返回空条件而不调模型。

**`offeredMetaKeyDescriptions` 是「semi_auto 只给勾选的键」这条约束的实现。**

```go
func offeredMetaKeyDescriptions(offeredKeys map[string][]string, descriptions map[string]string) string {
    offered := make(map[string]string, len(descriptions))
    for key, description := range descriptions {
        if _, ok := offeredKeys[key]; !ok || strings.TrimSpace(description) == "" {
            continue
        }
        if runes := []rune(description); len(runes) > metaFilterDescriptionLimit {
            description = string(runes[:metaFilterDescriptionLimit]) + "…"
        }
        offered[key] = description
    }
    if len(offered) == 0 { return "" }
    var buf bytes.Buffer
    enc := json.NewEncoder(&buf)
    enc.SetEscapeHTML(false)
    if err := enc.Encode(offered); err != nil { return "" }
    return strings.TrimSpace(buf.String())
}
```

**四处：**

1. **只保留 `offeredKeys` 里有的键** ——注释说明了理由：「a semi_auto call narrows the key set deliberately, and a description for a key the model cannot filter on is noise it may act upon」。**这是一个很有价值的判断：给模型的能力边界之外的信息，会让它试图越界。**

2. **按 rune 截断到 1024 字符。** 注释写得很实在：「the dataset config accepts 65535 characters per key. Cap it: this prompt carries no token budgeting, and a description long enough to crowd out the value space, the question or the answer costs the filter entirely」。**「这个提示词没有 token 预算」是一个诚实的设计承认**——于是唯一的保护是硬上限 1024。

3. **`json.NewEncoder` + `SetEscapeHTML(false)`** ——注释说「HTML escaping is off so a description reaches the model as written, not with `&` turned into `\u0026`」。**默认的 `json.Marshal` 会把 `&`、`<`、`>` 转成 unicode 转义**，对模型来说是噪声。

4. **空结果返回空串而不是 `"{}"`** ——让 `renderMetaFilterTemplate` 的 `descriptions != ""` 判断能把整块条件渲染剔除。

**`GenMetaFilter` 的响应清洗是三层降级。**

```go
responseStr := strings.TrimSpace(*response.Answer)
responseStr = common.StripThinkTrailing(responseStr)
responseStr = jsonFenceRE.ReplaceAllString(responseStr, "")
responseStr = strings.TrimSpace(responseStr)

var result MetaFilterResult
if err := json.Unmarshal([]byte(responseStr), &result); err != nil {
    repaired, rerr := jsonrepair.Repair(responseStr)
    if rerr != nil { repaired = responseStr }
    if err2 := json.Unmarshal([]byte(repaired), &result); err2 != nil {
        common.Warn("Failed to parse meta filter response after repair",
            zap.String("raw", responseStr[:min(len(responseStr), 200)]),
            zap.Error(err))
        return &MetaFilterResult{Conditions: []MetaFilterCondition{}, Logic: "and"}, nil
    }
}
```

**三层是：去尾部思考段 → 去 markdown 围栏 → 尝试 `jsonrepair`。**

**最后一层失败时返回「空条件」而不是错误** ——注释与日志都表明这是有意的（只打 warn，不 `return nil, err`）。于是**「模型输出无法解析」这条最常见的失败路径，与「模型说不需要过滤」在返回值上完全一样**。

**这是一处值得指出的信息丢失**：`GenMetaFilter` 的签名有 `error` 返回，但**解析失败时不使用它**。调用方无法区分「过滤失败」与「无需过滤」。而 `ApplyMetaDataFilter` 里两条路径的后果相同（都返回 `nil` = 不过滤），所以行为是对的——但可观测性没了。

**`ApplyMetaFilter` 与 `applySingleCondition` 是内存过滤的双层实现，而它们的关系值得说清。**

```go
func ApplyMetaFilter(metaData common.MetaData, filters []MetaFilterCondition, logic string) []string {
    if len(filters) == 0 { return []string{} }
    conditions := make([]common.MetaCondition, 0, len(filters))
    for _, f := range filters {
        conditions = append(conditions, convertToMetaCondition(f))
    }
    return common.MetaFilter(metaData, &common.MetaFilterInput{Conditions: conditions, Logic: logic})
}
```

**它自己不做事，只做「类型转换 + 委托」。** 注释解释了这次重构消除了什么：「This eliminates the duplicate merge logic that previously existed between `ApplyMetaFilter` and `common.MetaFilter`」——**合并逻辑（多条件的 AND/OR）曾经有两份，现在收进 `common.MetaFilter` 一份。**

而 `applySingleCondition` 是**遗留的、已经不被 `ApplyMetaFilter` 调用的单条件实现**（它的 switch 里重复了 `=` / `!=` / `contains` / `in` 等分支，且有 `compareValues` 这个只被它用到的辅助函数）。**这一段是旧实现的残骸**，而 `compareValues` 的注释还在解释「数值比较优先」这个已被 `common.MetaFilter` 接管的行为。

**`convertToMetaCondition` 的 operator 归一与 value 转换是两件不同的事。**

```go
func convertToMetaCondition(f MetaFilterCondition) common.MetaCondition {
    mc := common.MetaCondition{
        Key:      f.Key,
        Operator: common.NormalizeOperator(f.Op),
        Value:    f.Value,
    }
    switch f.Op {
    case "in", "not in":
        if values, ok := f.Value.([]interface{}); ok {
            for _, value := range values {
                if _, ok := value.(string); !ok {
                    mc.Value = nil
                    return mc
                }
            }
            return mc
        }
        strVal, ok := f.Value.(string)
        if !ok { return mc }
        parts := strings.Split(strVal, ",")
        arr := make([]interface{}, 0, len(parts))
        for _, p := range parts {
            if trimmed := strings.TrimSpace(p); trimmed != "" {
                arr = append(arr, trimmed)
            }
        }
        mc.Value = arr
    }
    return mc
}
```

**三处：**

1. **`NormalizeOperator` 把多种写法归一到一组符号**（`"=="`→`"="`、`"!="`→`"≠"`、`">="`→`"≥"`、`"is"`→`"="`）。注释里点名了一处对照：「see `common.metadata_utils.operatorMapping` for the full list」。

2. **`in` / `not in` 的数组必须**全员是字符串**，否则整个条件被置 `nil`。** 注释说明了这个选择的后果：「Arrays with non-string members become nil so the condition matches no documents」。

**「成员非字符串 → 条件匹配不到」是一个很硬的语义**：它把「模型给了一个含数字的数组」变成「这个条件筛掉一切」。对 `manual` 模式，这会产出哨兵（零结果）——**而 `manual` 是用户明确要求的模式，零结果是「正确答案」**。但对模型生成的 `auto` 模式，含数字的数组更可能是「模型把年份写成了数字」，而结果是「零文档 → 不过滤」——恰好被宽容处理了。**同一个转换在两个模式下后果不同，而这条差异没有写在任何地方。**

3. **字符串形式的 `in` 值按逗号切分**，这依赖模型按提示词写 `"a,b"` 而不是 `["a","b"]`。两种形式都支持，说明作者不确定模型会给哪种。

**`MetadataConditionToDocIDs` 是另一个入口，而它与 `ApplyMetaFilter` 的差别在「哨兵的产生条件」。**

```go
func MetadataConditionToDocIDs(metaData common.MetaData, metadataCondition map[string]interface{}) string {
    if metadataCondition == nil { return "" }
    input := common.ParseAndConvert(metadataCondition)
    if input == nil { return "" }
    filtered := common.MetaFilter(metaData, input)

    rawConditions, _ := metadataCondition["conditions"].([]interface{})
    if len(rawConditions) > 0 && len(filtered) == 0 {
        return "-999"
    }
    if len(filtered) == 0 { return "" }
    return strings.Join(filtered, ",")
}
```

**它返回的是逗号分隔的字符串，而哨兵条件比 `ApplyMetaDataFilter` 更宽**：只要「给了条件且筛不出」就返回 `-999`，**不分 `manual` 还是其它模式**。

**这是一处真实的不一致**：同一个哨兵常量 `"-999"`，在 `ApplyMetaDataFilter` 里只在 `manual` 模式产生（`auto`/`semi_auto` 筛不出时返回 `nil`），而在 `MetadataConditionToDocIDs` 里无条件产生。

**从消费端能看到这个不一致的代价**：

```go
// internal/agent/retrievalbridge/enhancer.go
// nil means the metadata could not narrow the search -- an auto/semi_auto
// filter that produced no conditions, or one refused because the value space
// did not fit the model's context. The scope the caller already asked for
// still applies; it is the only thing that does. Turning this into the
// no-match sentinel would send "-999" down as the document scope and return
// zero chunks, which is not what the filter said.
if docIDs == nil {
    return baseDocIDs, nil
}
return docIDs, nil
```

**这段注释把「为什么不能把 nil 变成哨兵」讲得极清楚** ——而它需要存在，正是因为那个区分只靠调用点自己维护。

**`ApplyMetaDataFilter` 的核心是那个 `runMetadataFilter` 闭包，以及它维护的 `nil` vs 空切片约定。**

```go
// The nil-vs-empty-slice convention (matching Python):
//   nil        -> push-down was not viable / errored -> fall back to in-memory
//   []string{} -> push-down succeeded but found 0 matching docs -> definitive,
//                 do NOT fall back to in-memory (empty result is authoritative)
runMetadataFilter := func(conditions []MetaFilterCondition, logic string) []string {
    if len(conditions) > 0 && len(kbIDs) > 0 {
        docEngine := engine.Get()
        if docEngine != nil {
            condMaps := make([]map[string]interface{}, len(conditions))
            for i, c := range conditions {
                condMaps[i] = map[string]interface{}{
                    "key": c.Key, "op": c.Op, "value": c.Value,
                }
            }
            pushdownIDs := docEngine.FilterDocIdsByMetaPushdown(ctx, dao.DB, kbIDs, condMaps, logic)
            if pushdownIDs != nil {
                return pushdownIDs
            }
        }
    }
    return ApplyMetaFilter(metaData, conditions, logic)
}
```

**这个 `nil` / 空切片区分是整页最精妙的一处**：它用「Go 切片的可空性」区分两种语义——

- **`nil`** = 推下不可行（引擎不存在、没给 kbIDs、或推下报错）→ **退回内存过滤**；
- **`[]string{}`（非 nil 空切片）** = 推下**成功**但没有匹配 → **这就是最终答案，不要退回内存**。

**为什么这个区分必要？** 因为「推下报错」与「推下成功但零匹配」在返回值上都是「零个 id」。若不区分，报错会被当成「零匹配」，于是用户得到零结果——而正确的处理是「退回内存算一遍」。

**而这条约定依赖被调用方严格遵守「成功但零匹配时必须返回非 nil 空切片」** ——一个 `return nil` 与 `return []string{}` 的差别，在本包里决定了「过滤是否生效」。这是 Go 里 `nil` 切片与空切片可区分这一特性的正面用法，但它也是**最容易在重构中被抹平的约定**（例如有人写 `return []string{}` 统一了所有分支，推下失败就会静默变成零结果）。

**三种模式的分支里，`auto` 与 `semi_auto` 的失败处理完全一样，而 `semi_auto` 多了一层「键收窄」。**

```go
case "semi_auto":
    selectedKeys := []string{}
    constraints := make(map[string]string)

    if semiAuto, ok := metaDataFilter["semi_auto"].([]interface{}); ok {
        for _, item := range semiAuto {
            switch v := item.(type) {
            case string:
                selectedKeys = append(selectedKeys, v)
            case map[string]interface{}:
                if key, ok := v["key"].(string); ok {
                    selectedKeys = append(selectedKeys, key)
                    if op, ok := v["op"].(string); ok {
                        constraints[key] = op
                    }
                }
            }
        }
    }

    if len(selectedKeys) > 0 {
        filteredMeta := make(common.MetaData)
        for _, key := range selectedKeys {
            if val, exists := metaData[key]; exists {
                filteredMeta[key] = val
            }
        }

        if len(filteredMeta) > 0 {
            filters, err := GenMetaFilter(ctx, chatModel, filteredMeta, question, constraints, loadMetaKeyDescriptions(ctx, kbIDs))
            ...
        }
    }
```

**六处的可读性问题：**

1. **`semi_auto` 的两种元素形式**（纯字符串键 或 `{key, op}` 对象）——**后者携带一个「操作符约束」**（`constraints[key] = op`），它被传给 `GenMetaFilter` 并渲染进提示词。于是「用户勾选了一个键并要求用 `=` 比较」这件事通过提示词约束模型。
2. **`selectedKeys` 可能重复**（两种形式都能 push），于是 `filteredMeta` 的构建循环会重复赋值同一个键（无害但不整洁）。
3. **`len(selectedKeys) > 0` 但 `len(filteredMeta) == 0` 时（勾选的键在元数据里都不存在），**整个 `if` 块不满足，**控制流掉到函数末尾的 `return baseDocIDs`** ——即「不过滤」而不是「零结果」。这是「失败方向偏向不过滤」的又一处体现。
4. **`constraints` 只在**键存在**时才生效**（因为它被传给收窄后的 `filteredMeta`），于是「约束了一个不存在的键」静默无效。
5. **`auto` 与 `semi_auto` 的 `len(docIDs) == 0 → return nil` 是同一个结论的两种路径**（空条件 or 筛不出），注释明确写了：「No conditions, or conditions that selected nothing: the model was not able to narrow the search, so it stays unnarrowed」。
6. **`semi_auto` 分支末尾没有 `default`/`return`** ——若 `selectedKeys` 为空，代码不 `return`，落到函数末尾的 `return baseDocIDs`。这让「semi_auto 但没勾选任何键」= 不过滤。

**`manual` 模式的 `manualValueResolver` 是一个变参回调，而它的形状暴露了一处设计妥协。**

```go
func ApplyMetaDataFilter(
    ctx context.Context,
    ...
    manualValueResolver ...ManualValueResolver,
) []string {
```

**用变参而不是普通可选参数**（`nil` 或一个空函数），是为了在既有调用点不改签名的情况下加这个能力。而使用处的写法也很防御：

```go
if len(manualValueResolver) > 0 && manualValueResolver[0] != nil {
    resolver := manualValueResolver[0]
```

**「变参 + 只取第一个 + 判非 nil」这三个条件加起来，等价于「可选参数」的 Go 方言。** 这是一处**为向后兼容付出的表达成本**——而仓库的 AGENTS.md 明确要求「Do not add or preserve deprecated Go APIs just to ease migration inside the repo」，所以这个妥协与项目自己的规范相抵。

**`manual` 模式的哨兵产生条件与其它模式的分界线很清楚。**

```go
filteredIDs := runMetadataFilter(conditions, logic)
docIDs := constrainDocIDs(baseDocIDs, filteredIDs)
if len(manualFilters) > 0 && len(docIDs) == 0 {
    // The user named these conditions, so no match is the answer.
    return []string{NoMatchDocIDSentinel}
}
return docIDs
```

**「The user named these conditions, so no match is the answer」** ——这一句把这个模式的全部逻辑说完了。而它对「空 docIDs」不返回 `nil`，因为**用户手写的条件筛不出东西，说明他的数据里确实没有符合条件的文档，这是一个有效的答案**。

**`constrainDocIDs` 是「取交集」的规范实现。**

```go
func constrainDocIDs(baseDocIDs, filteredDocIDs []string) []string {
    filteredDocIDs = common.Deduplicate(filteredDocIDs)
    if len(baseDocIDs) == 0 { return filteredDocIDs }
    if len(filteredDocIDs) == 0 { return []string{} }

    filteredSet := make(map[string]struct{}, len(filteredDocIDs))
    for _, docID := range filteredDocIDs { filteredSet[docID] = struct{}{} }
    result := make([]string, 0, min(len(baseDocIDs), len(filteredSet)))
    seen := make(map[string]struct{}, len(baseDocIDs))
    for _, docID := range baseDocIDs {
        if _, allowed := filteredSet[docID]; !allowed { continue }
        if _, exists := seen[docID]; exists { continue }
        seen[docID] = struct{}{}
        result = append(result, docID)
    }
    return result
}
```

**三处：**

1. **`baseDocIDs` 为空时直接用 `filteredDocIDs`** ——「没有既有的范围」意味着「元数据筛出的就是全部范围」。
2. **`filteredDocIDs` 为空时返回 `[]string{}`（非 nil）** ——这是一个刻意的选择：调用方会把它当成「筛出零个」而不是「没筛」。**在这里不返回 nil 正是与哨兵语义配套的**（`auto`/`semi_auto` 拿到非 nil 空切片后会在 `len(docIDs) == 0` 处转成 `nil`）。
3. **遍历 `baseDocIDs` 而不是 `filteredDocIDs`** ——于是**输出顺序保持调用方原有顺序**，而不是元数据筛出的顺序。这对检索很重要：若 `baseDocIDs` 是「按权限算出的可读文档」且有序，交集后仍有序。

**`chatMetadataDocIDs` 是把它接到对话流水线的薄包装，而它的降级点有三处。**

```go
func (s *ChatPipelineService) chatMetadataDocIDs(ctx context.Context, chat *entity.Chat, question string, model *modelModule.ChatModel, docIDs, kbIDs []string) []string {
    if chat.MetaDataFilter == nil || len(*chat.MetaDataFilter) == 0 || len(kbIDs) == 0 || question == "" {
        return docIDs
    }
    var meta common.MetaData
    var err error
    if s.MetadataSvc != nil {
        meta, err = s.MetadataSvc.GetFlattedMetaByKBs(ctx, kbIDs)
    }
    if err != nil {
        common.Warn("loadMetaData failed; skipping meta_data_filter", zap.Error(err))
        return docIDs
    }
    if filtered := ApplyMetaDataFilter(ctx, *chat.MetaDataFilter, meta, question, model, docIDs, kbIDs); filtered != nil {
        common.Debug("meta_data_filter applied", zap.Int("filtered_count", len(filtered)), zap.Int("pre_filter_count", len(docIDs)))
        return filtered
    }
    return docIDs
}
```

**三处降级都是「返回原范围」**：

1. 没有配置 / 没有 kb / 没有问题 → 原范围；
2. 元数据读不出来 → 原范围（打 warn）；
3. 过滤返回 `nil` → 原范围。

**而 `s.MetadataSvc != nil` 的判断有一个微妙之处**：若 `MetadataSvc` 为 nil，`meta` 保持零值（`nil`），`err` 也保持 `nil`，于是**不会走 warn 分支，直接进 `ApplyMetaDataFilter` 并传一个 nil metaData**。它会走到 `GenMetaFilter` 的 `len(metaData) == 0` → 空条件 → 不过滤。**结果是对的，但「服务未装配」这件事没有任何日志**。

**`loadMetaKeyDescriptions` 是一个可被测试替换的包级变量。**

```go
// Best effort, and only called for the LLM-driven methods, so manual filters
// never pay for the dataset reads. A dataset without a config contributes
// nothing, and the first non-empty description for a key wins when several
// datasets are searched together. A variable so tests can stub the database.
var loadMetaKeyDescriptions = func(ctx context.Context, kbIDs []string) map[string]string {
    if len(kbIDs) == 0 || dao.DB == nil { return nil }
    fields, err := NewMetadataService().DeclaredMetadataFields(ctx, kbIDs)
    if err != nil {
        common.Warn("Metadata key descriptions could not be read", ...)
        return nil
    }
    return metaKeyDescriptions(fields)
}
```

**「A variable so tests can stub the database」** ——这是一个显式的可测试性设计：把函数声明为变量，测试可以替换它。注释还说明了两条策略：「only called for the LLM-driven methods, so manual filters never pay for the dataset reads」（`manual` 不查库）与「the first non-empty description for a key wins when several datasets are searched together」（多库合并取第一个）。

**而后者的实现在 `metaKeyDescriptions` 里：**

```go
out := make(map[string]string, len(fields))
for _, field := range fields {
    description := strings.TrimSpace(field.Description)
    if description == "" { continue }
    if _, seen := out[field.Key]; !seen {
        out[field.Key] = description
    }
}
```

**「第一个非空胜出」靠 `if _, seen := out[field.Key]; !seen`。** 而「第一个」取决于 `DeclaredMetadataFields` 返回的顺序——若它不保证顺序，那么「哪个库的描述胜出」是不确定的。**这是一处语义依赖上游顺序、而代码没有声明的地方。**

## 关键取舍

**源码事实：** `ApplyMetaDataFilter` 的返回值有且仅有三种语义：`nil` = 没有收窄（调用方保留自己的范围、不过滤检索）；`[]string{NoMatchDocIDSentinel}`（值 `"-999"`）= 用户明确要求的条件零匹配；其它切片 = 选中的文档。docstring 明确禁止把 `nil` 转成哨兵：「that would answer 'the metadata was unusable' with 'no documents exist'」。

**源码事实：** 只有 `manual` 模式会产生哨兵；`auto` / `semi_auto` 在「生成空条件」或「条件筛不出」时都返回 `nil`（不过滤）。而 `MetadataConditionToDocIDs` 是另一个入口，它在「给了条件且筛不出」时**无条件**返回 `"-999"`，与 `ApplyMetaDataFilter` 的 `manual` 专属规则不一致。

**源码事实：** `runMetadataFilter` 用 `nil` / 非 nil 空切片区分「推下不可行/报错 → 退回内存」与「推下成功且零匹配 → 结论确定，不回退」。注释称之为「matching Python」的约定。

**源码事实：** `GenMetaFilter` 在元数据为空时返回「空条件 + and」；模型为 nil 时返回错误；LLM 调用失败返回错误；但**响应解析失败（含 `jsonrepair` 后仍失败）时返回空条件而 `error` 为 nil**，只打 warn。于是「解析失败」与「模型判断无需过滤」在上层不可区分。

**源码事实：** 响应清洗是三层：去尾部思考段（`StripThinkTrailing`）→ 去 markdown 围栏（`jsonFenceRE`）→ `jsonrepair.Repair`。第三层失败时日志只保留前 200 字符原文。

**源码事实：** 提示词模板的渲染先解「伪 Jinja 条件块」再替换值，注释说明这是为了让用户问题与数据集描述里的模板语法不被解析；值替换用 `strings.NewReplacer`（单趟，替换结果不再被扫描），与链式 `ReplaceAll` 的二次展开风险相对。

**源码事实：** `metaFilterIfBlock` 的正则把标签后的换行也匹配进去（注释：模仿 Jinja 的 `trim_blocks`），并用 `extraNewlinesRe`（`\n{3,}` → `\n\n`）兜底清理。

**源码事实：** `offeredMetaKeyDescriptions` 只保留「本次被提供的键」的描述（注释：semi_auto 有意收窄键集，给模型能力边界外的信息会让它越界）；按 rune 截断到 1024 字符（注释承认「this prompt carries no token budgeting」而单键描述上限是 65535）；`json.Encoder` 关掉 HTML 转义以免 `&` 变成 `\u0026`。

**源码事实：** `convertToMetaCondition` 对 `in` / `not in` 的数组要求「全员为字符串」，否则整个条件值置 `nil` 从而匹配不到任何文档；字符串形式按逗号切分。该转换在 `manual` 模式下的后果是零结果（哨兵），在 `auto` 模式下是「零文档 → 不过滤」——同一转换在两个模式下后果不同。

**源码事实：** `ApplyMetaFilter` 只做「类型转换 + 委托给 `common.MetaFilter`」，注释说明这次重构消除了它与 `common.MetaFilter` 里重复的多条件合并逻辑；`applySingleCondition` 与 `compareValues` 是遗留的单条件实现，已不被 `ApplyMetaFilter` 调用。

**源码事实：** `constrainDocIDs` 遍历 `baseDocIDs` 保持调用方原顺序；`baseDocIDs` 为空时直接用筛出结果；筛出为空时返回**非 nil** 空切片（与哨兵语义配套，`auto`/`semi_auto` 会在上层转成 `nil`）。

**源码事实：** `semi_auto` 支持两种元素形式（纯字符串键、带 `op` 的对象），后者把操作符作为 `constraints` 传进提示词；若勾选的键在元数据里都不存在，控制流落到函数末尾 `return baseDocIDs`（不过滤）。

**源码事实：** `manualValueResolver` 用**变参**实现可选参数（调用点只需传 0 或 1 个），使用处需同时判断 `len(...) > 0` 与 `[0] != nil`。

**源码事实：** `chatMetadataDocIDs` 有三处降级都返回原范围（未配置/无 kb/无问题、元数据读取失败并打 warn、`ApplyMetaDataFilter` 返回 nil）；当 `s.MetadataSvc == nil` 时不会打任何日志，直接以 nil 元数据进入过滤并最终归为不过滤。

**源码事实：** `loadMetaKeyDescriptions` 是包级变量（「A variable so tests can stub the database」），只在 LLM 驱动的模式下调用（`manual` 不查库）；多库合并时「第一个非空描述胜出」，而「第一个」取决于 `DeclaredMetadataFields` 的返回顺序。

**架构推论：** 用「`nil` / 哨兵 / 正常切片」三值表达「谁该负责」是一个**被文档救活的脆弱设计**。它在正确性上很干净（三种语义各有专属产生条件），但它依赖：每个产生点严格区分、每个消费点绝不混淆、以及 `nil` 与「非 nil 空切片」在另一处又承担了第二个三层语义。**同一套代码里有两次「靠可空性传语义」，一处是返回值三元组，一处是推下/内存的分界** —— 这是把语义压进值域做过了头。

**架构推论：** 「解析失败返回空条件而不返回错误」这个选择让系统更鲁棒（不会因为模型抽风而让整个问答失败），但它把「可观测性」抵押掉了。**更合适的形状是「返回空条件 + 一个已被降级的错误标记」**：让调用方仍能按旧语义继续（不过滤），同时让监控能统计「过滤生成失败率」这个指标。当前的写法让这条失败率在指标上完全不可见。

**架构推论：** 把「过滤推下到存储引擎」与「内存过滤」两条路径并存，收益是「大库不用把元数据全读进内存」，代价是**两条路径的语义必须严格一致**，而这只能靠约定。真正危险的不是实现出错，而是**两条路径的边界条件不同**（例如内存路径对 `in` 的空数组处理与推下路径的 SQL 生成不同）。这类双路径设计应当有一条「对同一份数据两条路径结果必须相同」的对照测试，而这里只有 docstring 里的约定。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/service/metadata_filter.go` | `MetaFilterResult` / `MetaFilterCondition` | LLM 输出的条件集合与逻辑（`Conditions` + `Logic`） |
| `internal/service/metadata_filter.go` | `GenMetaFilter` | 组装提示词、调 LLM、三层清洗、`jsonrepair` 修 JSON |
| `internal/service/metadata_filter.go` | `ApplyMetaFilter` | 类型转换 + 委托 `common.MetaFilter`（唯一的合并实现） |
| `internal/service/metadata_filter.go` | `MetadataConditionToDocIDs` | 另一入口：返回逗号分隔 id 串，筛不出时无条件给 `-999` |
| `internal/service/metadata_filter.go` | `ApplyMetaDataFilter` | 三种模式的分派 + 推下/内存双路径 + 三值返回约定 |
| `internal/service/metadata_filter.go` | `constrainDocIDs` | 与原范围取交集并保持原顺序 |
| `internal/service/metadata_filter.go` | `convertToMetaCondition` | 操作符归一 + `in`/`not in` 的值形态转换 |
| `internal/service/metadata_filter.go` | `NoMatchDocIDSentinel` | 零结果哨兵 `"-999"` |
| `internal/service/metadata_filter.go` | `renderMetaFilterTemplate` / `metaFilterIfBlock` | 伪 Jinja 条件块解析 + 单趟值替换（防模板注入） |
| `internal/service/metadata_filter.go` | `offeredMetaKeyDescriptions` | 只给被提供的键的描述、按 rune 截断 1024、关 HTML 转义 |
| `internal/service/metadata_filter.go` | `loadMetaKeyDescriptions` / `metaKeyDescriptions` | 从数据集元数据配置读键描述（可测试替换的包级变量） |
| `internal/service/metadata_filter.go` | `chatMetadataDocIDs` | 接到对话流水线；三处降级都返回原范围 |
| `internal/agent/retrievalbridge/enhancer.go` | （`nil` 处理） | 消费端：`nil` 必须保留调用方范围，绝不能转成哨兵 |
| `internal/handler/dify_retrieval_handler.go` | （哨兵检查） | 消费端：出现哨兵时禁用知识图谱增强 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：internal/service/metadata_filter.go](https://github.com/infiniflow/ragflow/blob/main/internal/service/metadata_filter.go)
- [jsonrepair 库](https://github.com/kaptinlin/jsonrepair)
- [Elasticsearch 文档：term-level queries](https://www.elastic.co/guide/en/elasticsearch/reference/current/term-level-queries.html)

## 与主核 / AI 的关系

元数据过滤在主核（多模态数据库内核）里对应的是**「把用户意图翻译成可下推的过滤条件」**——查询优化器做的事。四条经验。

**第一，把「失败方向」做成设计而不是默认。** 这里整套约定都在回答「过滤失败时该宽还是该窄」，答案是「宽」（返回 `nil` 不过滤），并且只有用户手写的条件才允许「窄」（哨兵零结果）。主核里做谓词下推时同理：**一个无法下推的谓词必须退化成「在内存里再筛一遍」，绝不能退化成「返回空集」**。判断准则：任何「优化失败」的路径都必须等价于「不优化」，而不是等价于「结果为空」。

**第二，用可空性传语义（`nil` vs 非 nil 空切片）能work，但必须在一个地方写清全部语义。** 这里有两处这样用：返回值三值约定、推下失败与推下零匹配。**这类约定的成本全部落在「下一个改代码的人」身上**：一个 `return nil` 改写成 `return []string{}` 就把「回退内存」变成了「零结果」。主核里凡是靠 `NULL` 与「空值」区分语义的地方，都应当在类型或枚举上显式化（三态枚举），而不是依赖值的可空性。

**第三，LLM 生成的结构化查询必须假设它是脏的，并把修复过程记录下来。** 这里三层清洗（去思考段、去围栏、`jsonrepair`）+ 解析失败退化为空条件，是一条正确的宽容路径。但**「解析失败」与「无需过滤」在上层不可区分**，导致这条失败率无法被度量。主核里若引入任何 LLM 生成的东西（自然语言转谓词、转 SQL、转索引建议），都必须把「生成失败」单独计数——**否则质量退化会以「结果变差」而不是「指标报警」的形式出现**。

**第四，模板渲染要防「值被当作语法解析」。** 这里把条件块解析与值替换分成两个阶段，并用单趟 `NewReplacer` 防止替换结果被二次扫描。主核里凡是把用户输入拼进可执行模板（SQL 模板、配置模板、日志格式串）时同理：**结构解析必须在值替换之前完成，且值替换必须是单趟**。这两条加起来才挡住「用户输入里的控制语法被执行」这一类问题，缺任一条都留有缺口。
