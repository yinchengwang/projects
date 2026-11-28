---
project: ragflow
feature: model-provider
title: 模型提供方与模型工厂
page_type: feature
schema_version: 1
refs:
  - path: internal/service/model_service.go
    symbols: [ModelProviderService, CheckConnectionModelInfo]
  - path: internal/service/model_factory.go
    symbols: [ModelFactory, ResolvedModelTarget, NewChatModel]
  - path: internal/service/model_factory.go
    symbols: [NewEmbeddingModel]
---

# 模型提供方与模型工厂

## 一句话职责

`ModelProviderService` 管的是**「这个租户能把哪些模型接进来」**——创建提供方、创建实例、存凭据、验连通性、把每个模型的可用性状态写回库；`ModelFactory` 管的是**「这一次调用到底用哪个模型实例」**——把 `gp-` 分组引用或 `name@instance@provider` 复合名解析成 driver + 凭据 + 上下文长度，再构造出带类型的 `ChatModel` / `EmbeddingModel`。**两层之间只通过数据库里的三张表（provider / instance / model）相连。**

## 机制是什么

这套设计要解决的核心问题是：**一次请求给定的只是「模型引用」这一个字符串，而把它变成可用实例需要穿过五层信息。**

```
modelRef 字符串
  ↓ 解析
租户 → 模型行（或分组）→ provider 行 → instance 行 → 目录里的模型元数据
  ↓ 合成
driver（怎么连）+ apiConfig（连哪、用什么凭据）+ Model（上下文多长、支不支持工具）
  ↓ 构造
ChatModel / EmbeddingModel / RerankModel / ASRModel / ...
```

**「提供方」与「模型实例」是两层，这个分层不是命名上的**：一个 provider（如 `openai`）可以有多份 instance（不同 API key、不同 base_url、不同区域），而每份 instance 下挂着若干 model 行。于是同一个模型名在不同 instance 上是**不同的连接**，而 `ResolvedModelTarget` 的注释专门解释了 `InstanceName` 为什么必须暴露：

> Two candidates routinely share provider and model and differ only here — the endpoint and credentials come from the instance — so it is what distinguishes them to a human.

第二个机制是**权限与配置校验嵌在解析路径里**。`ModelFactory.resolveModelIdentity` 不是「先查数据再查权限」，而是**边解析边校验**：查 provider 时校验租户归属、查 instance 时校验 provider 归属与启用状态、查 model 时校验类型位图是否覆盖请求的类型。任何一步不过就返回同一个哨兵错误 `errModelConfigUnavailable`（哨兵的意义见下）。

## 怎么实现的

**`ModelProviderService` 持七个 DAO，一个都不直接用 `dao.DB` 之外的依赖。**

```go
type ModelProviderService struct {
    modelProviderDAO *dao.TenantModelProviderDAO
    modelInstanceDAO *dao.TenantModelInstanceDAO
    modelDAO         *dao.TenantModelDAO
    tenantDAO        *dao.TenantDAO
    userTenantDAO    *dao.UserTenantDAO
    modelGroupDAO    *dao.TenantModelGroupDAO
    modelGroupMapDAO *dao.TenantModelGroupMapDAO
}
```

而 `ModelFactory` 只持四个（少了 `tenantDAO` 与两个 group DAO）——**它把分组交给 `pickGroupModel`（企业版实现，社区版 stub 拒绝 `gp-` 引用）**。这个差异说明「分组」是企业管理能力，而模型解析本身是共享的。

**`errModelConfigUnavailable` 是一个「不可重试」的哨兵错误。**

```go
// errModelConfigUnavailable marks model configuration failures that cannot be
// repaired by retrying the same task.
var errModelConfigUnavailable = errors.New("model configuration unavailable")
```

**注释的用词是关键：「cannot be repaired by retrying the same task」。** 这决定了一个上层行为——摄取任务遇到这个错误必须**直接失败而不是重试**，因为重试同一个任务不会让缺失的模型配置出现。对比之下，网络错误是可重试的。

而它被用在 `resolveModelIdentity` 里的**每一种失败**上，且都带 `%w` 包装：

```go
if tenantID == "" {
    return nil, fmt.Errorf("%w: tenant id is required", errModelConfigUnavailable)
}
...
return nil, fmt.Errorf("%w: model %q cannot be used as %s model", errModelConfigUnavailable, modelRef, requestedType.String())
```

**「同一个哨兵 + 不同的消息」这个组合让上层可以只做一次 `errors.Is` 判断，同时保留了定位信息。** 32 处错误返回全用同一个哨兵，意味着**「配置问题」在这套代码里是一个类别，不是一个具体原因**。

代价是：一个拼错的 modelRef 与一个被禁用的 instance 从上层看是同一类错误。想区分必须解析消息文本——而消息文本是给人读的。

**`resolveModelIdentity` 承担了全部解析与校验，且它的分支密度非常高。**

```go
func (s *ModelFactory) resolveModelIdentity(ctx context.Context, requestedType entity.ModelType, modelRef string) (*modelIdentity, error) {
    if s == nil || s.modelDAO == nil || s.modelProviderDAO == nil || s.modelInstanceDAO == nil {
        return nil, fmt.Errorf("%w: model factory is not initialized", errModelConfigUnavailable)
    }
    if modelRef == "" {
        return nil, fmt.Errorf("%w: model ref is required", errModelConfigUnavailable)
    }
    tenantID := strings.TrimSpace(permission.TenantIDFromContext(ctx))
    userID := strings.TrimSpace(permission.UserIDFromContext(ctx))
    if tenantID == "" {
        return nil, fmt.Errorf("%w: tenant id is required", errModelConfigUnavailable)
    }
    ...
```

**第一个检查是「工厂未初始化」——四个字段逐个判空。** 这看起来是防御性冗余（构造函数总会填它们），但它保护的是**零值 `ModelFactory{}` 被误用的场景**（例如某个调用方自己 `&ModelFactory{}` 而没走 `NewModelFactory`）。这类检查在 Go 里是常见的「零值安全」防御。

**第二个检查是 `modelRef == ""`，而 `tenantID == ""` 也被单独检查** ——两条都是「上下文里没有租户」，但分别对应「没传模型」与「没传身份」。注释没解释为什么不合并。

**「先判分组、再判单行」是两套完全不同的解析路径。**

```go
if parsedGroupID, ok := common.ParseGroupModelRef(modelRef); ok {
    // A gp-<group_id> reference resolves through the group's failover
    // picker instead of a single saved row. Enterprise-only; the community
    // stub rejects the reference. The group is carried into the resolved
    // target so usage records can tell group-mediated calls from direct
    // model calls.
    groupID = parsedGroupID
    model, err = s.pickGroupModel(ctx, tenantID, groupID, requestedType)
    ...
} else {
    model, err = s.modelDAO.GetByID(ctx, dao.DB, modelRef)
    if err != nil && !errors.Is(err, gorm.ErrRecordNotFound) {
        return nil, err
    }
}
```

**注意 `gp-` 路径与单行路径的错误处理不同**：分组路径把错误直接返回（`if err != nil { return nil, err }`），而单行路径**容忍 `ErrRecordNotFound`** 并继续往下——因为「库里没有这一行」可能意味着这是一个**复合名**（`name@instance@provider`），要由后面的分支去解析目录模型。

**这个「找不到行不是错误」的选择，是整段解析最关键的语义**：它让「目录里有、租户没存」的内建模型也能被解析。而代价是**一个完全拼错的 modelRef 会走到复合名解析，得到一条「no saved model row or valid composite name」而不是「找不到模型」**。

**复合名解析走一个专门的分段函数，且它对 2 段的歧义做了显式处理。**

```go
func splitRightAnchoredModelName(compositeName string) (modelName, instanceName, providerName string) {
    parts := strings.Split(compositeName, "@")
    switch len(parts) {
    case 3:
        return parts[0], parts[1], parts[2]
    case 2:
        // The 2-segment form "model@X" is ambiguous: X could be a provider
        // suffix (only "Builtin" is recognised by the TEI / Builtin
        // short-circuits that consume this helper) or part of the model
        // name itself (e.g. a quantization tag like "q8_0" in
        // "text-embedding-nomic-embed-text-v1.5@q8_0"). Treat the last
        // token as a provider only when it actually is one; otherwise
        // the whole string is the bare model name and the caller falls
        // through to its non-short-circuit path.
        if parts[1] == "Builtin" {
            return parts[0], "default", parts[1]
        }
        return compositeName, "", ""
    case 1:
        return parts[0], "", ""
    }
    n := len(parts)
    return strings.Join(parts[:n-2], "@"), parts[n-2], parts[n-1]
}
```

**四处：**

1. **三段直接切分**（`model@instance@provider`）。
2. **两段有歧义** ——`X` 可能是 provider 后缀，也可能是模型名的一部分（举的例子很具体：`text-embedding-nomic-embed-text-v1.5@q8_0` 里的 `q8_0` 是量化标签）。**只有 `"Builtin"` 被硬编码认定为 provider**，其它一律当成「整个字符串是模型名」。
3. **一段就是裸名。**
4. **超过三段时从右往左锚定**（`strings.Join(parts[:n-2], "@")`）——因为模型名本身可能含 `@`。

**`case 2` 的硬编码 `"Builtin"` 是一个真实的局限**：任何其它 provider 的两段形式（`gpt-4@openai`）都会被当成「模型名叫 `gpt-4@openai`」。而注释承认了这一点（「only 'Builtin' is recognised by the TEI / Builtin short-circuits that consume this helper」）——**这个函数只为少数短路路径服务，不是通用解析器**。真正的复合名解析是 `common.ParseCompositeModelName`（在 `resolveModelIdentity` 里用的是它，不是这个函数）。

**于是同一个概念有两个解析函数、两套规则**，而它们的名字都叫「复合名」。这是这一页最容易绊倒读者的一处。

**`ResolveInfo` 有一条硬编码的内建 OCR 短路。**

```go
func (f *ModelFactory) ResolveInfo(ctx context.Context, modelType entity.ModelType, modelRef string) (*modelModule.ModelInfo, error) {
    if modelType == entity.ModelTypeOCR {
        modelName, instanceName, providerName := splitRightAnchoredModelName(modelRef)
        if modelName == "deepdoc" && instanceName == "default" && providerName == "infiniflow" {
            if err := f.authorize(ctx); err != nil {
                return nil, err
            }
            return &modelModule.ModelInfo{
                Name:         modelName,
                ProviderName: providerName,
                InstanceName: instanceName,
                ModelTypes:   []string{entity.ModelTypeOCR.String()},
            }, nil
        }
    }
    ...
    if name, ok := builtinEmbeddingName(modelRef); ok && modelType == entity.ModelTypeEmbedding {
        info, _ := dao.GetModelProviderManager().GetModelByName("Builtin", name)
        return modelInfoFromTarget(&modelTarget{...ProviderName: "Builtin"...}), nil
    }
```

**两条短路都跳过数据库查询，只做 `authorize(ctx)`**：

- **`deepdoc@default@infiniflow` 作为 OCR 模型** ——三家字符串精确匹配才命中。它没有 provider 行、没有 instance 行，纯粹是一个「进程内 DeepDoc」的别名。
- **`builtinEmbeddingName(modelRef)`** ——内建嵌入模型（如 TEI）也走短路，从目录直接拿元数据。注意这里**忽略了 `GetModelByName` 的错误**（`info, _ :=`），若目录里没有这个名字，`info` 是 nil，而 `modelInfoFromTarget` 能容忍 nil（返回一个只有名字与类型的 info）。

**这是「内建能力伪装成普通模型」的设计**：DeepDoc OCR 与会话、嵌入、rerank 共用同一个 modelRef 概念，于是调用方不需要知道「这个模型是本地的还是远程的」。代价是**这层伪装只在这两个短路分支里成立**——任何其它路径（例如 `ResolveChatTarget`）拿到 `deepdoc@default@infiniflow` 会走真正的数据库解析并失败。

**模型类型是一个位图，而位图的展开顺序是硬编码的。**

```go
func modelTypesFromBitmask(modelType entity.ModelType) []entity.ModelType {
    var modelTypes []entity.ModelType
    for _, candidate := range []entity.ModelType{
        entity.ModelTypeChat,
        entity.ModelTypeEmbedding,
        entity.ModelTypeSpeech2Text,
        entity.ModelTypeImage2Text,
        entity.ModelTypeRerank,
        entity.ModelTypeTTS,
        entity.ModelTypeOCR,
        entity.ModelTypeDecision,
        entity.ModelTypeScoring,
    } {
        if modelType.Has(candidate) {
            modelTypes = append(modelTypes, candidate)
        }
    }
    return modelTypes
}
```

**一个模型可以同时是多种类型**（`tenant_model.model_type` 是一个位掩码），而展开顺序由这个字面量列表决定。这让「一个模型行可以服务 chat 与 vision」成为可能——而 `verifyProviderModel` 里 `case "chat", "vision"` 合并处理正好呼应。

**代价是新增一种模型类型要改三处**：位掩码常量、这个展开列表、`verifyProviderModel` 的 switch。

**`modelInfoWithTenantExtra` 把「目录元数据 + 租户覆盖」合成一个 Model，而它的合并规则很具体。**

```go
func modelInfoWithTenantExtra(modelInfo *modelModule.Model, modelEntity *entity.TenantModel) (*modelModule.Model, error) {
    if modelEntity == nil { return modelInfo, nil }

    model := modelModule.Model{Name: modelEntity.ModelName}
    if modelInfo != nil {
        model = *modelInfo
        model.ModelTypes = append([]string(nil), modelInfo.ModelTypes...)
        model.Dimensions = append([]int(nil), modelInfo.Dimensions...)
        model.Alias = append([]string(nil), modelInfo.Alias...)
        if modelInfo.ModelTypeMap != nil {
            model.ModelTypeMap = make(map[string]bool, len(modelInfo.ModelTypeMap))
            for modelType, enabled := range modelInfo.ModelTypeMap {
                model.ModelTypeMap[modelType] = enabled
            }
        }
        ...
```

**逐字段深拷贝的原因是「防止两个租户共享同一份目录对象后被互相改写」。** 目录模型来自 `GetModelProviderManager()`（进程级单例），若直接 `model = *modelInfo` 后再改切片/映射，会污染全局。注释没写这一点，但这几行 `append([]T(nil), ...)` 的意图很清楚。

**而后半部分的优先级是「租户行优先，目录兜底」：**

```go
if modelEntity.ModelType != 0 {
    setModelInfoTypes(&model, entity.ModelType(modelEntity.ModelType))
    if documentParseSupported {
        model.ModelTypeMap["doc_parse"] = true
    }
}
...
if modelEntity.ModelType == 0 && len(extra.ModelTypes) > 0 {
    model.ModelTypes = append([]string(nil), extra.ModelTypes...)
    ...
}
```

**两处 `documentParseSupported` 的「抢救」值得注意**：租户行给了类型位掩码时，目录里的 `doc_parse` 标记会**被覆盖掉**（因为 `setModelInfoTypes` 重建了整个 `ModelTypeMap`）。于是代码先存下 `documentParseSupported`，覆盖后再塞回去。

**这是一个很典型的「上层覆盖把下层的一个特殊位冲掉」的问题**，而修法是「局部补救」而不是「合并语义」。它只在 `doc_parse` 这一个键上成立——如果目录里还有别的非类型键，同样会被冲掉，而代码没有处理。

**`ResolvedModelTarget` 是一条**刻意的**逃生舱口，且注释明确警告不要用于公开 API。**

```go
// ResolvedModelTarget exposes a resolved model's provider details — driver, model name,
// base URL and credentials. Unlike ResolveInfo it deliberately DOES expose the driver and
// credentials, so it is an internal escape hatch for paths that must hand the provider
// endpoint and token to an external system (the mode 8 Codex agent) instead of building an
// in-process model. Keep it out of public APIs.
type ResolvedModelTarget struct {
    ProviderName string
    ModelName    string
    InstanceName string
    Driver       modelModule.ModelDriver
    BaseURL      string
    APIKey       string
}
```

**「Unlike `ResolveInfo` it deliberately DOES expose the driver and credentials」** ——这是一个清晰的分界线：**常规路径返回 `ModelInfo`（无凭据），只有必须把 endpoint 与 token 交给外部系统的路径（Codex agent）才用 `ResolvedModelTarget`。**

而 `ResolveInfo` 的注释也对称地写了它的承诺：

> ResolveDefaultInfo returns metadata for the operation tenant's default model without exposing its provider driver or API credentials.

**两条注释共同构成一条安全边界：凭据只能通过一个被明确标注为「别放进公开 API」的类型流出。** 这个做法值得直接抄——**把敏感数据的出口做成一个名字带警告的类型，比在每个调用点检查「有没有泄漏凭据」更可靠。**

**`ModelReference` 是反向的：从 info 拼出一个引用字符串。**

```go
func ModelReference(info *modelModule.ModelInfo) string {
    if info == nil { return "" }
    if id := strings.TrimSpace(info.ID); id != "" { return id }
    name := strings.TrimSpace(info.Name)
    if name == "" { return "" }
    if provider := strings.TrimSpace(info.ProviderName); provider != "" {
        instance := strings.TrimSpace(info.InstanceName)
        if instance == "" { return name + "@" + provider }
        return name + "@" + instance + "@" + provider
    }
    return name
}
```

**优先返回 id，只有没有 id（内建/目录模型）才拼复合名。** 与 `splitRightAnchoredModelName` 对照，这里的 2 段形式是 `name@provider`，而那个函数把 2 段解释为「可能只是模型名」——**两个方向不对称**：`ModelReference` 会产出 `name@provider`（2 段），而这种形式喂回解析器时（除 `Builtin` 外）会被当成裸模型名。

**这是一处真实的往返不闭合**：`ModelReference(info)` 的输出不一定是能被 `splitRightAnchoredModelName` 正确解析的输入。

**`CheckConnection` 是「拿未落库的凭据去验连通性」，而它的分支就是这个功能的全部难点。**

```go
type CheckConnectionRequest struct {
    APIKey              json.RawMessage            `json:"api_key"`
    ClearAPIKey         bool                       `json:"clear_api_key"`
    UseSavedCredentials bool                       `json:"use_saved_credentials"`
    APIKeyConfig        json.RawMessage            `json:"api_key_config"`
    Region              string                     `json:"region"`
    BaseURL             string                     `json:"base_url"`
    InstanceID          string                     `json:"instance_id"`
    ModelInfo           []CheckConnectionModelInfo `json:"model_info"`
}
```

**八个字段覆盖了三种语义完全不同的调用**：

1. **纯新增**：只给凭据（`api_key` / `region` / `base_url`），不指定 `instance_id` —— 验一遍能不能连。
2. **验已有实例**：给 `instance_id` + `use_saved_credentials` —— 用库里存的凭据验。
3. **验已有实例但换凭据**：给 `instance_id` 和新的 `api_key` —— 验新凭据。

`CheckConnection` 开头就是把它们归一成「有 `savedInstance` 与否 + 有 `useSavedCredentials` 与否」四象限：

```go
if savedInstance != nil {
    ...
    if useSavedCredentials {
        if clearAPIKey || apiKey != "" {
            return common.CodeBadRequest, errors.New("saved credentials cannot be combined with submitted credentials")
        }
        if instanceAccess.Source != permission.AccessSourceOwner && (region != "" || baseURL != "" || hasSavedProviderAPIKeyConfig(req.APIKeyConfig)) {
            code, normalizedErr := permissionresponse.NormalizeHidden(permission.ErrPermissionDenied)
            return code, normalizedErr
        }
        apiKey = savedInstance.APIKey
        ...
    } else {
        if hasSavedProviderAPIKeyConfig(req.APIKeyConfig) {
            return common.CodeBadRequest, errors.New("api_key_config requires saved credentials")
        }
        ...
    }
} else if useSavedCredentials {
    return common.CodeBadRequest, errors.New("saved credentials require an existing model instance")
} else if hasSavedProviderAPIKeyConfig(req.APIKeyConfig) {
    return common.CodeBadRequest, errors.New("api_key_config requires saved credentials")
}
```

**四处值得逐个记：**

**第一，「保存的凭据」与「提交的凭据」不能混用** ——`clearAPIKey || apiKey != ""` 都算「提交了凭据」。这防止了一个危险的组合：「用旧 key 连，但同时声明要清空 key」。

**第二，非 owner 不能改 region / baseURL / apiKeyConfig，但可以点「用保存的凭据验一遍」。**

```go
if instanceAccess.Source != permission.AccessSourceOwner && (region != "" || baseURL != "" || hasSavedProviderAPIKeyConfig(req.APIKeyConfig)) {
    ... ErrPermissionDenied
}
```

**这条权限判据的分界很精确**：「只验证现有配置」是读操作，任何有 update 权限的人都能做；「改变连接目标」是写操作，只有 owner 能做。而它用 `instanceAccess.Source` 判断而不是「租户归属」——因为共享实例的含义就是「别人建的、你能用」。

**第三，`region` 与 `baseURL` 在未提交时从**实例的 `extra`** 里回填。**

```go
extra := make(map[string]interface{})
if savedInstance.Extra != "" {
    if err := json.Unmarshal([]byte(savedInstance.Extra), &extra); err != nil {
        return common.CodeServerError, err
    }
}
...
if region == "" { region, _ = extra["region"].(string) }
if baseURL == "" { baseURL, _ = extra["base_url"].(string) }
```

**`extra` 是一个 JSON 字符串列，用 `map[string]interface{}` 解开再类型断言取两个键。** 于是「实例的连接参数」没有 schema——键名靠约定，类型靠断言，缺失靠 `""`。

**第四，`region == ""` 最终归一成 `"default"`。**

```go
if region == "" { region = "default" }
```

**与 `newModelDriverForBaseURL` 里那段呼应对照：**

```go
baseURLByRegion := map[string]string{ region: baseURL }
if region == "" { baseURLByRegion["default"] = baseURL }
```

**两处都在处理「region 为空」但方式不同**：`CheckConnection` 提前归一成 `"default"`，而 `newModelDriverForBaseURL` **两个都塞**（空键 + `default` 键）。后者是为了「无论驱动按哪个键查，都能拿到 baseURL 」。**这是同一个问题的两种防御，出现在两个函数里，说明「region 的默认值该是什么」没有单一真相。**

**本地 provider 必须给 `base_url`，这是一条硬校验。**

```go
driver := providerInfo.ModelDriver
if strings.EqualFold(providerInfo.Class, "local") {
    if baseURL == "" {
        return common.CodeDataError, fmt.Errorf("base_url is required for local provider %s", providerName)
    }
    var err error
    driver, err = newModelDriverForBaseURL(driver, providerName, region, baseURL)
    if err != nil {
        return common.CodeServerError, err
    }
}
```

**`providerInfo.Class == "local"` 意味着「这个 provider 没有公网 endpoint，必须由部署方给地址」**（Ollama、Xinference、LocalAI 这类）。于是「有没有 base_url」从「可选」变成「必需」。

**注意错误码用的是 `CodeDataError` 而不是 `CodeBadRequest`** ——语义上的区分：坏请求是「你格式错了」，数据错误是「你缺了必要的一项」。

**`driver.NewInstance(baseURLByRegion)` 是「按 region 造一份 driver 变体」的机制，而不支持自定义 base_url 的 provider 返回 nil。**

```go
newDriver := driver.NewInstance(baseURLByRegion)
if newDriver == nil {
    return nil, fmt.Errorf("provider %s does not support custom base_url", providerName)
}
```

**「`NewInstance` 返回 nil 表示不支持」是一个接口约定，靠错误消息解释。** 它让「支持自定义 base_url」变成 provider 驱动的可选能力，而不是配置校验能提前判断的属性——**所以这条约束只能在使用时发现**。

**`verifyProviderModel` 是为了「验证」而真的调用每一种模态。**

```go
switch mtLower {
case "chat", "vision":
    msg := []modelModule.Message{{Role: "user", Content: "Hi"}}
    chatModel := modelModule.NewChatModel(driver, &modelName, apiConfig)
    _, err = chatModel.ChatWithMessages(ctx, msg, nil, nil)
case "embedding":
    if model.MaxDimension != nil && model.MaxBatchSize != nil {
        requestedDimension := *model.MaxDimension
        if len(model.Dimensions) > 0 { requestedDimension = model.Dimensions[0] }
        err = validateEmbeddingModel(model, requestedDimension, 1)
    }
    if err == nil {
        maxTokens := 0
        if model.MaxTokens != nil { maxTokens = *model.MaxTokens }
        embeddingModel := modelModule.NewEmbeddingModel(driver, &modelName, apiConfig, maxTokens)
        _, err = embeddingModel.Embed(ctx, modelModule.EmbedRequest{Texts: []string{"test"}}, nil, nil)
    }
case "rerank":
    rerankRequest := modelModule.RerankRequest{Query: "test", Documents: []string{"test"}}
    rerankModel := modelModule.NewRerankModel(driver, &modelName, apiConfig, 0)
    _, err = rerankModel.Rerank(ctx, rerankRequest, &modelModule.RerankConfig{}, nil)
case "tts":
    content := "hello"
    ttsModel := modelModule.NewTTSModel(driver, &modelName, apiConfig)
    _, err = ttsModel.Speech(ctx, &content, nil, nil)
case "asr":
    err = verifyASRModel(ctx, driver, modelName, apiConfig)
case "ocr":
    ...
```

**「真的发一次最小请求」是验证连通性的唯一可靠方式**，而每类模型的「最小请求」都不同：

- chat/vision 发一句 `Hi`；
- embedding 先用 `validateEmbeddingModel` 校验维度/批大小是否在模型允许范围内，再嵌一个 `"test"`；
- rerank 查 `"test"` 与文档 `"test"`；
- tts 念一句 `hello`；
- asr 与 ocr 需要**真实媒体**——所以它们用了 `generateTestWAV()` 与 `minimalPNG()` 两个内置的极小样本。

**`generateTestWAV()` 与 `minimalPNG()` 是「为了让 ASR/OCR 能被验证」而造的常量** ——因为这两种模态无法像 chat 那样发一个字符串。这是一个很实在的细节：**验证逻辑的可行性取决于能不能构造一个合法的最小输入**，而代码为此内嵌了两个二进制生成器。

**`passedTypes` 让「同一类模型只验一个就够了」。**

```go
for _, model := range modelsToVerify {
    modelName := model.Name
    anyPassed := false
    for _, modelType := range model.ModelTypes {
        mtLower := strings.ToLower(modelType)
        // If a model type we've already verified successfully, skip.
        if passedTypes[mtLower] {
            continue
        }
        ...
```

**注释直说了规则**：某一类已经验过且通过，就跳过后续同类。于是「提供方下有 20 个 chat 模型」只会花一次调用。

**但这有一个必须指出的后果**：**只有第一个 chat 模型会被真正验证，其余 19 个的可用性从未被检查，却仍会拿到 `"passed"` 状态**（因为 `updateModelVerifyResults` 是按 `modelVerifyResult` 里的结果写库的）。于是「连通性检查」在目录很大的 provider 上退化成「抽样检查」——而库里写的是「全部通过」。

**`unverifiableTypes` 与 `errSet` 是同一条流水线上的两种降级。**

```go
var errs []error
errSet := make(map[string]bool)
passedTypes := make(map[string]bool)
unverifiableTypes := make(map[string]bool)
```

**三种集合分别表示「通过」「无法验证」「失败」**，而 `unverifiableTypes` 存在意味着「有些类型**根本没法验**」（例如某些 provider 不实现某个模态的探测）——这类既不算通过也不算失败。

**`errSet` 用来对错误去重**：同一个错误消息只进 `errs` 一次，避免「20 个模型都返回同一个 401」时刷出 20 条同样的错误。

**`CheckConnection` 的结果会被写回库，但写失败不致命。**

```go
if instanceID != "" && len(modelVerifyResult) > 0 {
    if dbErr := m.updateModelVerifyResults(ctx, providerName, instanceID, modelVerifyResult); dbErr != nil {
        if permissionresponse.IsPermissionError(dbErr) {
            code, normalizedErr := permissionresponse.NormalizeHidden(dbErr)
            return code, normalizedErr
        }
        common.Logger.Error("failed to persist model verify results", zap.Error(dbErr))
    }
}

if verifyErr != nil {
    return common.CodeServerError, verifyErr
}
```

**这里的顺序很重要**：先写状态、再返回验证结果。于是即使验证失败（`verifyErr != nil`），**每个模型的通过/失败状态已经落库**——调用方能看到「哪些模型连不上」。

而**权限错误与普通数据库错误被区别对待**：权限错误要向外返回（那是安全问题，不能吞），普通写库失败只记日志（因为验证本身已经完成了，丢一次状态不值得让整个请求失败）。

## 关键取舍

**源码事实：** `ModelProviderService` 持七个 DAO（含两个 group DAO + tenantDAO），`ModelFactory` 只持四个——分组解析（`pickGroupModel`）在企业版实现、社区版 stub 拒绝 `gp-` 引用，因此工厂不需要 group DAO。

**源码事实：** `errModelConfigUnavailable` 是「不可通过重试同一任务修复」的哨兵错误，`resolveModelIdentity` 里 32 处失败全部用它加 `%w` 包装；上层只需一次 `errors.Is` 即可分类，代价是「拼错的 modelRef」与「被禁用的 instance」在类型上无法区分。

**源码事实：** `resolveModelIdentity` 的解析顺序是「`gp-` 分组引用 → 单行 id 查询（容忍 `ErrRecordNotFound`）→ 复合名 `name@instance@provider` → 目录兜底」；单行查询容忍未找到是必需的，因为「目录里有、租户没存」的内建模型要靠复合名路径解析。

**源码事实：** 复合名有两个解析函数：`splitRightAnchoredModelName`（2 段时仅认 `"Builtin"` 为 provider，否则整串当模型名；>3 段时从右锚定）与 `common.ParseCompositeModelName`（`resolveModelIdentity` 实际用的那个）。两者规则不同。

**源码事实：** `ModelReference` 优先返回 id，否则拼 `name@provider`（2 段）或 `name@instance@provider`（3 段）——其输出喂回 `splitRightAnchoredModelName` 时（2 段且 provider 不是 `Builtin`）会被当成裸模型名，往返不闭合。

**源码事实：** `ResolveInfo` 有两条硬编码短路：`deepdoc@default@infiniflow`（三串精确匹配，仅 `authorize` 后返回，不查库）与 `builtinEmbeddingName`（从目录拿元数据且忽略错误）。

**源码事实：** 模型类型是位掩码，展开成列表的顺序由 `modelTypesFromBitmask` 里的字面量序列决定（chat / embedding / speech2text / image2text / rerank / tts / ocr / decision / scoring）；新增类型要改位掩码常量、这个列表、以及 `verifyProviderModel` 的 switch。

**源码事实：** `modelInfoWithTenantExtra` 对目录 Model 做逐字段深拷贝（切片 `append([]T(nil), ...)`、映射重建），以防改写进程级共享的目录对象；租户类型位掩码非 0 时用 `setModelInfoTypes` 重建整个 `ModelTypeMap`，因此需要先存下 `doc_parse` 标记再塞回。

**源码事实：** `ResolvedModelTarget` 刻意暴露 driver / baseURL / APIKey，注释明确说明它是「给必须把 endpoint 与 token 交给外部系统的路径（mode 8 Codex agent）」的内部逃生舱口，并要求「Keep it out of public APIs」；对应的 `ResolveInfo` / `ResolveDefaultInfo` 则不暴露凭据。

**源码事实：** `CheckConnection` 用八个字段覆盖三种语义（纯新增校验 / 用已存凭据校验 / 校验新凭据），并把它们归一成四象限；「保存的凭据」与「提交的凭据」禁止混用；非 owner 可「仅验证现有配置」但不能改 `region` / `baseURL` / `apiKeyConfig`（判据是 `instanceAccess.Source != AccessSourceOwner`）。

**源码事实：** 实例的 `extra` 是 JSON 字符串列，用 `map[string]interface{}` 解开后类型断言取 `region` / `base_url`，无 schema、缺失靠空串；`CheckConnection` 把空 `region` 归一成 `"default"`，而 `newModelDriverForBaseURL` 同时写空键与 `"default"` 键，两处对同一默认值有两种处理。

**源码事实：** `class == "local"` 的 provider 必须有 `base_url`（错误码 `CodeDataError` 而非 `CodeBadRequest`），并用 `newModelDriverForBaseURL` 造带自定义地址的 driver 变体；`driver.NewInstance` 返回 nil 表示该 provider 不支持自定义 base_url，这条约束只能在使用时发现。

**源码事实：** `verifyProviderModel` 对每种模态真的发一次最小请求（chat 发 `"Hi"`、embedding 先校验维度/批大小再嵌 `"test"`、rerank 用 `"test"`、tts 念 `"hello"`、asr 与 ocr 依赖内置的 `generateTestWAV()` / `minimalPNG()`）。`passedTypes` 让同类模型只验一个，因此目录大的 provider 上实际是抽样验证，但未验过的同类模型同样会被写成通过状态。

**源码事实：** 验证状态先落库再返回结果，因此验证失败时每个模型的状态仍可见；权限类写库错误向外返回，普通写库错误只记日志。

**架构推论：** 「一个哨兵错误 + 若干包装消息」让「配置不可用」成为一个干净的分类，但把「到底哪里不可用」降级成了字符串。**这是 Go 里 `errors.Is` 分类的常见取舍**：分类越粗，上层的策略越简单（这个不可重试），但可诊断性越差。若要两者兼顾，正确做法是定义一小组哨兵（`ErrModelNotFound` / `ErrModelDisabled` / `ErrModelWrongType`），而不是一个。

**架构推论：** 「provider / instance / model」三层是凭据与连接参数的正确归属方式：**凭据属于 instance 而不是 provider 或 model**，于是「同一 provider 的两套 key」不需要复制模型定义。而 `ResolvedModelTarget` 暴露 `InstanceName` 的理由（「两个候选常共享 provider 与 model，只在 instance 上不同」）正是这个分层在诊断层面的直接后果。

**架构推论：** 「验证连通性」与「记录可用性」是两件事，而这里它们被一次调用合并。合并的收益是「验完立刻能得到每个模型的可用状态」；代价是**验证策略（抽样，因 `passedTypes`）与记录策略（全量写库）不匹配**——记录了从未验证过的模型的结论。任何「批量探测 + 批量记录」的设计都必须回答：未探测的项记录什么。

**架构推论：** 「内建能力伪装成普通模型引用」是让调用面统一的好办法（DeepDoc OCR 与会话/嵌入走同一个 modelRef），但伪装的边界必须清楚：它只在被显式短路的两个入口成立，其它入口拿到同样的字符串会失败。**这类别名的正确做法是让解析函数本身识别它并在所有入口统一处理，而不是在个别入口 case-by-case 短路。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/service/model_service.go` | `ModelProviderService` | 提供方/实例/模型的运维面（七 DAO） |
| `internal/service/model_service.go` | `CheckConnectionModelInfo` | 待验证模型的描述（名字、类型、max_tokens、extra） |
| `internal/service/model_service.go` | `CheckConnectionRequest` | 八字段请求：凭据、已存凭据开关、region/baseURL、实例 id、模型清单 |
| `internal/service/model_service.go` | `CheckConnection` | 四象限归一 + 权限分界 + 驱动构造 + 验证 + 状态落库 |
| `internal/service/model_service.go` | `verifyProviderModel` | 按模态发最小请求；三类集合（通过/不可验/失败）+ 错误去重 |
| `internal/service/model_service.go` | `generateTestWAV` / `minimalPNG` | 为 ASR / OCR 构造最小合法输入 |
| `internal/service/model_service.go` | `newModelDriverForBaseURL` | 按 region 造 driver 变体；nil 表示不支持自定义 base_url |
| `internal/service/model_service.go` | `NormalizeAPIKey` / `mergeProviderAPIKey` | 凭据归一与「保留已存 / 清空 / 替换」三态合并 |
| `internal/service/model_factory.go` | `ModelFactory` | 解析模型引用、校验权限、构造强类型模型（四 DAO） |
| `internal/service/model_factory.go` | `resolveModelIdentity` | 全部分层解析与校验（分组 / 单行 / 复合名 / 目录兜底） |
| `internal/service/model_factory.go` | `ResolvedModelTarget` | 暴露 driver/凭据的内部逃生舱口（供外部系统使用） |
| `internal/service/model_factory.go` | `resolveChatTarget`（`ResolveChatTarget` / `ResolveDefaultChatTarget`） | 把引用解析成 provider 细节而不构造模型 |
| `internal/service/model_factory.go` | `NewChatModel` | 解析 → 附 usage 模板 → 构造 `ChatModel` |
| `internal/service/model_factory.go` | `NewEmbeddingModel` | 同上，额外传入 `inputTokenLimit(target)` 作为输入上限 |
| `internal/service/model_factory.go` | `modelInfoWithTenantExtra` | 目录元数据 + 租户行覆盖的深拷贝合并（含 `doc_parse` 抢救） |
| `internal/service/model_factory.go` | `splitRightAnchoredModelName` / `ModelReference` | 复合名解析（2 段歧义只认 Builtin）与反向拼接（优先 id） |
| `internal/service/model_factory.go` | `usageTemplateAttacher` / `attachUsageTemplate` | 构造期预填用量身份（企业版安装，社区版为空操作） |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：internal/service/model_factory.go](https://github.com/infiniflow/ragflow/blob/main/internal/service/model_factory.go)
- [Go 文档：`errors.Is` 与哨兵错误](https://pkg.go.dev/errors#Is)
- [OpenAI 兼容 API：models 与 embeddings](https://platform.openai.com/docs/api-reference/models)

## 与主核 / AI 的关系

模型层与主核（多模态数据库内核）的交集在**「凭据与连接参数该归谁」以及「不可用该如何分类」**。四条经验。

**第一，凭据属于「实例」这一层，不属于「提供方」或「模型」。** 这里 provider（`openai`）可以有多份 instance（不同 key / base_url / region），而模型定义只挂在 instance 下。主核里做「外部存储后端接入」（S3 / MinIO / 另一套 DB）时同理：**端点与凭据应当是可复制的「连接配置」，与「表/对象定义」解耦**。否则「同一云的两套 key」会逼出两份完全重复的模型定义。

**第二，敏感数据的出口要收敛成一个「名字带警告的类型」。** `ResolvedModelTarget` 的注释直白地说它「deliberately DOES expose driver and credentials」并「Keep it out of public APIs」，而正常路径返回的 `ModelInfo` 没有凭据。主核里做「连接串/密钥的传递」时同理：**与其在每个调用点人工检查「这里会不会打印凭据」，不如让凭据只能通过极少数、有明显名字的类型/函数流出**，并让其余 API 的返回值在类型上根本不包含凭据。

**第三，「不可重试」应当在错误类型上表达，而不是在文档里说明。** 这里 `errModelConfigUnavailable` 的存在让摄取任务可以「直接失败而不重试」。主核里区分「可重试」（超时、死锁、页争用）与「不可重试」（schema 不匹配、权限不足、配置缺失）靠的是同一个机制：**错误必须被分类，否则上层的重试策略只能是全重试或全不重试，两者都会放大故障**。

**第四，批量探测与批量记录必须配套说明「未探测项」的语义。** 这里的验证因为 `passedTypes` 而实际是抽样，但写库时把未验证的同类模型也写成通过。主核里做「批量校验页的 CRC」或「批量检查索引可用性」时同理：**跳过是为了成本，但「跳过」必须在结果里留下痕迹**（例如记录为 `unverified` 而不是 `ok`），否则「检查通过」会被读成「全部检查过」。
