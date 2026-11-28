---
project: ragflow
feature: parser-component
title: 解析组件与文件族分派
page_type: feature
schema_version: 1
refs:
  - path: internal/ingestion/component/parser.go
    symbols: [ParserComponent]
  - path: internal/ingestion/component/parser_dispatch.go
    symbols: [ParserFileFamily, parserSetupConfigurer]
---

# 解析组件与文件族分派

## 一句话职责

把上游传来的**二进制**变成结构化的条目列表：先按若干线索判定「这是哪个文件族」，再按族分派到具体解析器（本地 DeepDOC、远程 OCR、语音转写、VLM 增强），最后把产物统一成 JSON 条目 + 文件元数据两种输出。

## 机制是什么

解析是所有 RAG 流水线里**输入形态最杂**的一步：PDF、DOCX、XLSX、PPT、Markdown、HTML、EPUB、CSV、图片、音频、视频、纯文本。每种的解析方式完全不同，而且同一族还能有多个后端（本地 vs 远程 OCR vs VLM）。

于是这个组件要同时处理三件事：

1. **族判定**。`fileTypeFromInputs` 从三个线索依次尝试——上游显式给的 `file_type`、文件名、文件描述符里的名字——都没有就降级成 `FileTypeOTHER`（纯文本页模式）。
2. **族 → 配置的映射**。配置不是按「扩展名」而是按**族名**组织的（`pdf`、`docx`、`slides`、`spreadsheet`、`text&code`、`image`、`audio`……）。所以 `resolveParserFamily` 把扩展名折叠成族名，`familyToExt` 做反向。
3. **分派链**。媒体类（PDF 视觉增强、视频、图片、音频）各自有一条独立的尝试路径，只有前面都没接手才落到「普通解析」。

第三条的实现方式值得注意：它用了**一串 `handled` 布尔**而不是嵌套 if。每一段返回 `(result, handled, err)`，下一段只在 `!handled` 时执行。这样「谁处理了这个文件」是显式的，而且新增一种媒体只需在最前面插一段。

**这是一处与 `switch` 不同的多路分派**：`switch` 要求判据是单一值，而这里每段都可能基于「文件类型 + 配置 + 可用服务」三重条件才决定是否接手。

## 怎么实现的

### 构造：把所有 DSL 参数摊平成族配置

```go
func NewParserComponent(params map[string]any) (runtime.Component, error) {
	s := defaultSetups()
	if params == nil {
		return &ParserComponent{setups: s}, nil
	}
	params = schema.FlattenLegacyParserSetups(params)
	...
	for k, raw := range params {
		if k == "outputs" || k == "allowed_output_format" || k == "enable_vision_enhancement" || k == "vlm" {
			continue
		}
		ftCfg, ok := raw.(map[string]any)
		if !ok {
			continue
		}
		if _, exists := s[k]; !exists {
			s[k] = schema.ParserSetup{}
		}
		for fk, fv := range ftCfg {
			s[k][fk] = cloneParserSetupValue(fv)
		}
	}
	schema.ApplyParserGlobalOptions(params, s)
```

六处：

1. **`defaultSetups()` 先铺默认值，再被覆盖** —— 所以「没配的族」用默认配置，而不是「没配置就报错」。这是「零配置也能跑」的设计。

2. **`FlattenLegacyParserSetups` 处理历史格式**：注释说旧版前端把各族配置**嵌在 `setups` 键下**，这里把它们**上提到顶层**。所以一次格式迁移在读取侧做成了兼容——老画布不用重新保存也能跑。

3. **四个被跳过的键**（`outputs` / `allowed_output_format` / `enable_vision_enhancement` / `vlm`）—— 它们是**全局设置而不是族配置**。跳过它们是因为后面那个循环把「所有 map 类型的值」都当族配置，若不跳过会把 `vlm` 当成一个文件族。

4. **`if !ok { continue }` 静默跳过非 map 值** —— 所以一个类型错误的族配置被忽略而不是报错。**这与 `vlm` / `enable_vision_enhancement` 的处理相反**（那两个非对应类型就 `return nil, errors.New(...)`）。

   **这个不对称有理由**：全局设置配错会让功能静默失效（VLM 没模型、语言不生效），所以必须报错；族配置配错只影响那一族，而默认值仍在。

5. **`cloneParserSetupValue` 深拷贝** —— 注释没解释，但理由能推出来：`params` 来自画布 DSL，可能在别处被复用；不拷贝会让两个组件实例共享嵌套 map，一个改另一个也跟着变。

6. **`Check()` 在构造末尾调用** —— 注释说「Errors here surface as canvas compile failures so a malformed param is caught at build time rather than mid-run」。**这是「配置错误尽早暴露」的实例**：构建期（编译画布时）报错比运行时（处理文档时）报错好得多，因为前者用户当场能改。

### `Invoke` 的六段：每段都可独立失败

**第一段：解码二进制，并把运行级元数据从 Globals 拉回来。**

```go
ctx, binary, err := readParserBinary(ctx, db, inputs)
...
if tid := globals.GlobalOrInput(ctx, inputs, "tenant_id", ""); tid != "" {
	inputs["tenant_id"] = tid
}
if lang := globals.GlobalOrInput(ctx, inputs, "lang", ""); lang != "" {
	inputs["lang"] = lang
}
```

**这两条注释把「为什么要拉回来」说得很具体：**

> The File component upstream does not emit tenant_id; the pipeline runner seeds it into `CanvasState.Globals`, and the Parser must pull it back into the local `inputs` map for the dispatch functions.

> File emits no `lang`, and the language consumers below — vision enhancement and the media dispatch branches — read the local `inputs` map, so without this the KB language never reaches them and **the prompt language silently falls back to English**.

**第二句是本页最值得记的一处**：语言丢失的后果不是报错，而是**静默回退到英语**。所以「把 Globals 里的值复制进 inputs」这个看起来多余的步骤，实际是防止一个静默的质量退化。

**第二段：族判定与解析器选择，而「请求的族」与「实际选中的族」可能不同。**

```go
requestedType := fileTypeFromInputs(inputs)
fileParser, fileTypeExt, selectionErr := selectParserForRequest(requestedType, binary, setups)
if selectionErr == nil && fileTypeExt != requestedType {
	warning := fmt.Sprintf("file %q: requested format %s does not match detected format %s; continuing with the %s parser", ...)
	common.Warn(warning)
	reportParserWarnings(ctx, []string{warning})
}
```

三处：

1. **`selectParserForRequest` 把 `configuredParserMethod(setups, fileType)` 传给 `parser.GetParserWithMethod`** —— 所以「配置里 pin 了哪个方法」会覆盖「扩展名默认用哪个解析器」。这是「运营可强制指定后端」的入口。

2. **`fileTypeExt != requestedType` 时只警告不报错** —— 因为检测出的实际格式更可信（`GetParserWithMethod` 内部会嗅探内容）。所以「声明的格式不对」是提示，不是错误。

3. **`reportParserWarnings` 把警告同时送进日志与进度通道** —— 所以前端能看到「文件格式与声明不符」。

**第三段：进度窗口被 `NewSplit` 切成两段。**

```go
parsePagesOn := pdfPageProgressSupported(fileTypeExt, setups)
enhanceOn := c.enableVisionEnhancement && !parser.HasParseMethod(fileTypeExt, configuredParserMethod(setups, fileTypeExt))
split := progress.NewSplit(parsePagesOn, enhanceOn)
pageWin := split.Take(parsePagesOn)
visionWin := split.Take(enhanceOn)
```

四处：

1. **`enhanceOn` 的第二项是「没有注册远程方法」** —— 即：如果配了远程 OCR（MinerU/PaddleOCR/Docling），就不再做 VLM 增强（因为远程后端已经产出结构化结果了）。

2. **`progress.NewSplit` 把「总体进度」按阶段权重切成子窗口** —— 所以解析页与视觉增强各占一段进度，而不是两者都报 0→1。**这是「多阶段任务的总进度不能倒退」的标准解法。**

3. **`parsePagesOn` 委托给 `parser.ReportsPageProgress(method)`** —— 注释说明了为什么不在这里硬编码：

> The decision is delegated to the parser package so the set of page-reporting backends has a single source of truth and this shared file never hard-codes method names.

**「后端清单只有一处」是关键**：新增一个会报页进度的后端，只需在 parser 包里登记，所有调用点自动生效。

4. **`pageWin` / `visionWin` 分别传给不同阶段** —— 所以解析阶段只推进它那一半。

**第四段：媒体分派链，四个 `handled` 布尔串起来。**

```go
dispatched, handledVision, visionErr := maybeDispatchPDFVision(...)
var handledMedia bool
if !handledVision {
	dispatched, handledMedia, visionErr = maybeDispatchVideo(...)
}
var handledImage bool
if !handledVision && !handledMedia {
	dispatched, handledImage, visionErr = maybeDispatchImage(...)
}
var handledAudio bool
if !handledVision && !handledMedia && !handledImage {
	dispatched, handledAudio, visionErr = maybeDispatchAudio(...)
}
if !handledVision && !handledMedia && !handledImage && !handledAudio {
	// 普通解析
}
```

五处：

1. **条件越来越长的 `if`** —— 表面看是冗余（`handledVision` 为真时后面都不会执行），但因为每个分支都要重新判断全部前置标记，所以不能简化成 `else if`。**这是「谁接手了」这个状态必须显式携带的代价。**

2. **每个分支都必须判 `visionErr`** —— 而且都 `return nil, visionErr`。所以媒体分派的失败是**硬失败**，不像「格式不符」那样降级。

3. **`maybeDispatchImage` 多传两个参数**（`c.enableVisionEnhancement` 与 `c.vision`）—— 因为图片是唯一「同一后端内还能再增强」的族：OCR 出文本后可以用 VLM 重写。注释：「Image dispatch: OCR with independently controlled VLM enhancement」。

4. **注释里的「Mirrors _video()」/「Mirrors chunk()」** —— 这些指的是 Python 版的对标函数。所以这一层是 Python 实现的 Go 移植，注释保留了溯源信息。

5. **只有全部 `handled` 都为假才走普通解析，而在普通解析里还有一个额外判断**：

```go
if selectionErr != nil && fileTypeExt != utility.FileTypeOTHER {
	return nil, fmt.Errorf("parser: resolve %q: %w", fileTypeExt, selectionErr)
}
```

   **即：如果族能识别但解析器选不出来，硬失败**（不是降级成纯文本）。这与「未知族降级」形成对比。

**第五段：VLM 增强在普通解析之后、对已知族统一做一次。**

```go
if enhanceOn && !handledMedia && !handledImage && !handledAudio && dispatched.Err == nil && ctx.Err() == nil {
	dispatched, _, enhanceErr = maybeDispatchVisionEnhancement(ctx, db, fileTypeExt, dispatched, inputs, c.vision, visionWin)
	...
} else {
	dispatched = stripBackendDescribedResult(dispatched)
}
```

三处：

1. **条件是「解析成功且未取消」** —— 所以解析失败时不做增强（没有内容可增强）。

2. **`!handledMedia && !handledImage && !handledAudio` 三个都要判** —— 因为视频/图片/音频自己已经处理过视觉，再做一次会重复。

3. **`stripBackendDescribedResult` 在 else 分支里** —— 注释：「still strip the transient backend-description marker so it never reaches the chunker」。**这个标记是后端用来「描述自己产出了什么」的临时字段**，不该进入分块器。所以两条路（增强 / 不增强）都要剥，只是位置不同。

**第六段：计费在解析成功之后。**

```go
if tenantID, _ := getString(inputs, "tenant_id"); tenantID != "" && docID != "" && fileTypeExt == utility.FileTypePDF {
	if pdfSetup, ok := c.setups["pdf"]; ok {
		if method, _ := pdfSetup["parse_method"].(string); strings.Contains(strings.ToUpper(method), "DEEPDOC") {
			if billingService := billing.Get(); billingService != nil && billingService.Enabled() {
				if pageCount := parserPageCount(dispatched.File); pageCount > 0 {
					if chargeErr := billingService.ChargeParsedDocument(ctx, tenantID, docID, runKey, pageCount, true); chargeErr != nil {
						return nil, chargeErr
					}
				}
			}
		}
	}
}
```

**注释说明了语义**：「DeepDOC PDFs are metered per page after a successful parse. A shortage fails the run, matching the hold contract」。

四处：

1. **只对 DeepDOC PDF 计费** —— 因为那是**本地**解析（有自有算力成本），远程 OCR 是用户自己的服务。

2. **按页计费**（`parserPageCount(dispatched.File)`），不是按文件。所以一个 500 页 PDF 与 5 页 PDF 收费不同。

3. **计费失败让整个运行失败** —— 注释说「matching the hold contract」，即「欠费即停」。**这是一处「计费在关键路径上」的设计**，与「观测失败只记日志」相反。

4. **`billing.Get()` 可能为 nil 且 `Enabled()` 可能为假** —— 所以自托管部署（无计费）完全跳过。

**输出：既给 JSON 也给存储引用。**

```go
out["file_type"] = string(fileTypeExt)
if docID != "" { out["doc_id"] = docID }
if bucket, _ := getString(inputs, "bucket"); bucket != "" { out["bucket"] = bucket }
if path, _ := getString(inputs, "path"); path != "" { out["path"] = path }
globals.PublishGlobals(ctx, out)
```

三处：

1. **转发 `doc_id`/`bucket`/`path` 的理由是「让下游按需重新取源文件」**：

> Forward the storage references so a downstream chunker can re-acquire the source PDF and crop section images on demand, instead of carrying the binary across the component boundary.

   **「不把二进制跨组件传递」是一条重要的架构决定**：二进制可能几十 MB，而组件之间传的是 map。所以解析产物里只留引用，需要时再读。

2. **`PublishGlobals` 存在的原因是运行时不转发隐式字段**：

> The Go runtime forwards only this explicit output to the next node, so shared fields must live in Globals.

   **即：组件之间只传「显式 output」，所以想共享的东西必须显式放进 Globals。** 这解释了 `Invoke` 开头为什么要从 Globals 拉 `tenant_id` / `lang`——它们是别的组件 publish 的。

3. **`logParserOutput` 里记了 `normalized_from`** —— 由 `resolveParserNormalizationSource` 决定「原始产物来自 JSON 还是 Markdown」。因为 Markdown 只是内部表示，对外统一成 JSON。

### `dispatchParse`：绝不返回部分结果

```go
// The function NEVER returns a partial result. On error the result
// is the zero value (Err != nil, no JSON and no File). Callers detect the
// success/failure boundary with ParseResult.Produced().
func dispatchParse(ctx context.Context, p parser.ParseResultProducer, fileType utility.FileType, filename string, data []byte, setups map[string]schema.ParserSetup, window progress.Window) parser.ParseResult {
	if fileType == utility.FileTypeOTHER {
		return parser.ParseResult{}
	}
	...
	res := p.ParseWithResult(ctx, filename, data)
	if res.Err != nil {
		return parser.ParseResult{Err: fmt.Errorf("parser: %q: %w", fileType, res.Err)}
	}
	if parseMethod != "" {
		if res.File == nil { res.File = map[string]any{} }
		res.File["parse_method"] = parseMethod
	}
	return res
}
```

五处：

1. **「绝不返回部分结果」是显式契约** —— 失败时返回**零值 + Err**（既无 JSON 也无 File）。所以调用方不能「拿一半用一半」。

   **这条契约的价值在于简化下游**：分块器不需要判「解析产出了一部分是不是够用」。

2. **`Produced()` 是成功边界** —— 而 `Err` 只是失败原因。两者分开是因为「有内容但带警告」是可能的（`Warnings` 字段）。

3. **`FileTypeOTHER` 时直接返回零值** —— 注释：「We return no result here so the caller routes to that path」。所以「纯文本页模式」的判定不在这个函数里，而是由「返回零值」这个信号驱动。

4. **`parse_method` 被写回文件元数据** —— 注释：「so callers can tell the difference between 'explicit OCR' and 'default DeepDOC' without re-reading setups」。**这是把「配置」变成「事实」写进产物**，否则下游拿不到「这次到底用了哪个后端」。

5. **`lib_type` 不再传** —— 注释：「the dispatcher picks a single backend per family and the Go constructors mirror that」。所以「每个族一个后端」这个约束让原本的 `lib_type` 参数消失了。

**IO 追踪是按「是否远程」决定要不要开**：

```go
if method := strings.ToLower(strings.TrimSpace(parseMethod)); method != "" && method != "deepdoc" && method != "plain_text" && !parser.HasParseMethod(fileType, method) {
	finishParserIO = agentruntime.TrackIO(ctx, "Parser", "remote parser", "backend="+method)
} else {
	finishParserIO = func() {}
}
defer finishParserIO()
```

**注意条件是 `!parser.HasParseMethod(fileType, method)`** —— 即「这个方法既不是默认的、也**没有注册**」时才当作远程 IO 追踪。

**这个条件很绕**：`HasParseMethod` 为真说明是已注册方法（可能本地也可能远程），所以不追踪；为假才是「未注册的第三方后端」。

### `ParserFileFamily`：给外部调用方的唯一入口

```go
// ParserFileFamily normalises a free-form file-type/extension hint to the
// family identifier used as the key into a Parser component's
// setups (e.g. "pdf", "docx", "slides", "text&code"). It is the exported
// entry point for callers outside this package (e.g. the ingestion task
// executor that injects the debug page cap into override_params) that need
// to build the canonical ParserConfig[cpnID][family]["pages"] shape.
func ParserFileFamily(ext string) string {
	fileType := utility.GetFileType("file." + ext)
	if fileType == utility.FileTypeOTHER {
		fileType = familyToExt(ext)
	}
	if fileType == utility.FileTypeOTHER {
		return ""
	}
	return resolveParserFamily(fileType)
}
```

四处：

1. **两段降级**：先当扩展名查（`"file." + ext`），不行再当族名查。所以 `ParserFileFamily("md")` 与 `ParserFileFamily("markdown")` 都返回 `"markdown"`。

2. **两次都失败返回 `""`** —— 空串表示「无法判定」，而不是 `"other"`。**这是有意的**：调用方要能区分「判定为 other 族」与「判不出来」。

3. **它的存在理由是「外部调用方要构造 `ParserConfig[cpnID][family]["pages"]`」** —— 即 b01 里注入调试页数上限那条路径。**所以这是一个「为一个具体调用点导出的函数」**，而函数本身是纯映射。

4. **`resolveParserFamily` 的注释是一句很好的纪律**：「A type name must not be reinterpreted as another filename extension」。即——**族名与扩展名是两个命名空间，混用会让 `xls` 被当成 `xlsx`**。

   而 `fileTypeFromInputs` 里那段注释正好与之对应：

   > Direct extension match first — handles exact hints like "xls", "ppt", "doc", "docx", etc. This must run before the family look-up so that legacy binary extensions aren't collapsed to OOXML types (e.g. "xls" → XLSX).

   **所以「扩展名优先、族名兜底」的顺序是为了保住老二进制格式的区分**。若反过来，`xls` 会先被 `familyToExt("spreadsheet")` 之类的路径吃掉，或者被嵌到 `xlsx`。

### `parserSetupConfigurer`：可选能力探测

```go
type parserSetupConfigurer interface {
	ConfigureFromSetup(setup map[string]any)
}

type parserLanguageConfigurer interface {
	SetLanguage(lang string)
}

func configureParserFromSetups(p any, fileType utility.FileType, setups map[string]schema.ParserSetup) {
	cfg, ok := p.(parserSetupConfigurer)
	if !ok {
		return
	}
	family := resolveParserFamily(fileType)
	setup, ok := setups[family]
	if !ok {
		return
	}
	cfg.ConfigureFromSetup(map[string]any(setup))
}
```

四处：

1. **两个单方法接口，用于类型断言** —— Go 里「可选能力」的标准做法。所以「解析器是否接受族配置」由它自己声明（实现了就接受）。

2. **`p any` 而不是 `parser.ParseResultProducer`** —— 因为断言目标不是「解析器」而是「配置消费者」。用 `any` 让任何类型都能被探测。

3. **`configureParserLanguage` 里 `if lang == "" { return }` 在最前** —— 所以空语言不会调用 `SetLanguage("")`，避免把「未指定」当成「显式设为空」。

4. **两处「找不到就静默返回」** —— 族配置缺失不是错误（默认值已在 `defaultSetups` 里）。

## 关键取舍

**源码事实：** `NewParserComponent` 先铺 `defaultSetups()` 再覆盖；`FlattenLegacyParserSetups` 把旧画布的嵌套 `setups` 上提，兼容历史格式。

**源码事实：** 构造器跳过四个**全局**键（`outputs` / `allowed_output_format` / `enable_vision_enhancement` / `vlm`），其余 map 类型值当作族配置；族配置类型不对时**静默跳过**，而 `vlm` / `enable_vision_enhancement` 类型不对时**返回错误**——因为全局设置配错会静默失效，族配置配错有默认值兜底。

**源码事实：** 族配置值用 `cloneParserSetupValue` 深拷贝，避免两个组件实例共享嵌套 map。

**源码事实：** `Check()` 在构造末尾调用，让畸形参数变成**画布编译期**错误而不是运行期错误。

**源码事实：** `Invoke` 开头从 `CanvasState.Globals` 把 `tenant_id` 与 `lang` 拉回本地 `inputs`——因为 File 组件不产出这两个，而下游视觉增强与媒体分派只读本地 map；注释明确指出语言丢失的后果是「prompt language silently falls back to English」（静默质量退化，不是报错）。

**源码事实：** 族判定与解析器选择可能给出不同的类型（`fileTypeExt != requestedType`），此时只警告并继续用检测出的类型；`configuredParserMethod` 能让模板 pin 的后端覆盖扩展名默认后端。

**源码事实：** `parser.ReportsPageProgress(method)` 是「哪些后端会报页进度」的唯一判断处，本文件不硬编码方法名；`progress.NewSplit` 把总进度按「解析页 / 视觉增强」两段切分，避免多阶段一起报 0→1。

**源码事实：** 媒体分派是一条「`handled` 布尔链」：PDF 视觉 → 视频 → 图片 → 音频 → 普通解析，每段只在全部前驱为假时执行，且每段错误都硬失败。图片是唯一「同一后端内还能再增强」的族。

**源码事实：** 普通解析路径上，若「族能识别但解析器选不出来」则硬失败；只有 `FileTypeOTHER` 才降级到纯文本页模式。

**源码事实：** VLM 增强在普通解析成功后做一次，条件包含「解析成功且未取消」；两条路径（增强 / 不增强）都要 `stripBackendDescribedResult` 剥掉后端的临时描述标记。

**源码事实：** 计费只对 `DEEPDOC` PDF 按**页数**计，解析成功后执行，且欠费让整个运行失败（「matching the hold contract」）；`billing.Get()` 为 nil 或未启用时完全跳过。

**源码事实：** 输出转发 `doc_id` / `bucket` / `path`，让下游分块器按需重新取源文件切图，**不把二进制跨组件传递**。

**源码事实：** `globals.PublishGlobals` 存在是因为「运行时不转发隐式字段，只转发显式 output」，所以跨组件共享的数据必须显式放进 Globals。

**源码事实：** `dispatchParse` 的契约是「绝不返回部分结果」——失败时为「零值 + Err」（无 JSON 无 File），成功边界由 `ParseResult.Produced()` 判定；`FileTypeOTHER` 直接返回零值让调用方走纯文本页分支。

**源码事实：** `dispatchParse` 把 `parse_method` 写回 `res.File`，让下游能区分「显式 OCR」与「默认 DeepDOC」而不必重读配置；`lib_type` 参数已不存在，因为「每族单后端」。

**源码事实：** IO 追踪只在「方法非默认且未注册」时开启（`!parser.HasParseMethod`），即只追踪未注册的第三方后端。

**源码事实：** `ParserFileFamily` 先按扩展名查、再按族名查，两段都失败返回 `""`（空串而非 `"other"`），让调用方能区分「判定为 other」与「判不出来」；它是为「外部构造 `ParserConfig[cpnID][family]["pages"]`」而导出的。

**源码事实：** `resolveParserFamily` 的注释明确「族名不得被重新解释为另一个文件扩展名」；`fileTypeFromInputs` 因此「先精确扩展名匹配、后族名兜底」，否则 `xls` 会被折叠成 `xlsx`。

**源码事实：** `configureParserFromSetups` / `configureParserLanguage` 用单方法接口 + 类型断言做**可选能力探测**；参数类型是 `any` 而非 `ParseResultProducer`；空语言不调用 `SetLanguage("")`。

**架构推论：** 「`handled` 布尔链」这个结构是这类多路分派的正确形态，而它比 `switch` 更适合本场景的原因在于**每段的条件不是同一个判据**：是否接手取决于「文件族 + 配置 + 可用服务」三者。`switch` 只能分派单一值。代价是条件表达式越来越长，且必须显式传递每个标记——换来的是「谁处理了这个文件」在代码里完全可见。

**架构推论：** 「配置与事实分离」这条纪律在 `parse_method` 写回产物里体现得很清楚。配置是「希望用哪个后端」，产物里的是「实际用了哪个」。若下游只能重读配置，那么配置在运行中被改过、或者族名解析出错时，下游会拿到错误的后端标识。**判据是「这个值在运行中可能偏离配置吗」——可能就写进产物。**

**架构推论：** 「不把二进制跨组件传递」是一个值得单独记的决定。组件之间的契约是 `map[string]any`，携带几十 MB 的 `[]byte` 会让每次跨节点的 map 传递都变贵（Go 里 map 复制是浅拷贝，但序列化/日志/追踪会把二进制拖进去）。改成「传引用，用时再读」把一个 O(数据量) 的成本挪到了真正需要它的那一处（切章节图）。代价是下游必须能访问同一存储，且源文件不能在处理中途被删。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/ingestion/component/parser.go` | `ParserComponent` | 解析组件（族配置 + 视觉增强设置） |
| `internal/ingestion/component/parser.go` | `visionSettings` | 全局 VLM 设置（模型 / 提示 / 语言） |
| `internal/ingestion/component/parser.go` | `NewParserComponent` | 铺默认值 + 上提旧格式 + 深拷贝覆盖 + `Check()` |
| `internal/ingestion/component/parser.go` | `cloneParserSetupValue` | 族配置深拷贝 |
| `internal/ingestion/component/parser.go` | `Check` | 构造期校验（错误即画布编译失败） |
| `internal/ingestion/component/parser.go` | `defaultSetups` | 各族默认配置 |
| `internal/ingestion/component/parser.go` | `Invoke` | 六段主流程（解码 → 拉 Globals → 族判定 → 媒体链 → 增强 → 计费） |
| `internal/ingestion/component/parser.go` | `readParserBinary` | 从 inputs / 对象存储解析出二进制 |
| `internal/ingestion/component/parser.go` | `reportParserWarnings` | 警告同时进日志与进度通道 |
| `internal/ingestion/component/parser.go` | `stripBackendDescribedResult` | 剥掉后端的临时描述标记 |
| `internal/ingestion/component/parser.go` | `parserPageCount` | 计费用的页数 |
| `internal/ingestion/component/parser_dispatch.go` | `parserSetupConfigurer` | 可选能力接口：接受族配置 |
| `internal/ingestion/component/parser_dispatch.go` | `parserLanguageConfigurer` | 可选能力接口：接受语言 |
| `internal/ingestion/component/parser_dispatch.go` | `resolveParserFamily` | 扩展名 → 族名（配置键） |
| `internal/ingestion/component/parser_dispatch.go` | `configuredParserMethod` | 从族配置取 pin 的后端名 |
| `internal/ingestion/component/parser_dispatch.go` | `selectParserForRequest` | 带 pin 方法的解析器选择 |
| `internal/ingestion/component/parser_dispatch.go` | `configureParserFromSetups` | 类型断言注入族配置 |
| `internal/ingestion/component/parser_dispatch.go` | `dispatchParse` | 「绝不返回部分结果」的解析调用 |
| `internal/ingestion/component/parser_dispatch.go` | `pdfPageProgressSupported` | 委托 `parser.ReportsPageProgress` |
| `internal/ingestion/component/parser_dispatch.go` | `attachPDFPageProgress` | 挂页回调与 IO 追踪 |
| `internal/ingestion/component/parser_dispatch.go` | `fileTypeFromInputs` | 四段降级：类型提示 → 文件名 → 描述符 → OTHER |
| `internal/ingestion/component/parser_dispatch.go` | `familyToExt` | 族名 → 解析器类型 |
| `internal/ingestion/component/parser_dispatch.go` | `ParserFileFamily` | 对外导出的族名归一 |
| `internal/ingestion/component/parser_dispatch.go` | `buildParserOutputs` | 组装 JSON + 文件元数据输出 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：解析方法配置](https://ragflow.io/docs/dev/configure_knowledge_base)
- [DeepDOC：ragflow 的文档解析模型](https://ragflow.io/blog/deepdoc)
- [论文：版面感知的文档解析](https://arxiv.org/abs/2307.12559)

## 与主核 / AI 的关系

「文件族分派」与主核（多模态数据库内核）的**类型分派**（按 `atttypid` 选操作函数）结构相同：都是「先判定类型，再按类型找实现」。差别在**判据的可信度**：主核的类型是模式里写死的（`OID` + `pg_type` 注册表），不可能判错；这里的族是从文件名/提示猜的，**可能错**，所以才有「请求格式与实际格式不符」这条警告路径。

**这个差别的后果很实际**：主核不需要「降级到通用路径」的表达，因为不存在「类型未知」的行；这里必须有 `FileTypeOTHER` 这个兜底族，否则一个没有文件名的输入就没法处理。

第二处对照是「`handled` 布尔链」与主核的**多级解析器**（`raw_parser` → `parse_analyze` → `transformStmt`）。主核的分派是**严格分层**的：每一层只认一种输入，失败即返回错误，不会有「上一层可能也处理了」的问题。这里因为媒体分派的条件互相重叠（一个文件既是 PDF 又有视觉增强可能），才需要 `handled` 标记来表达「已处理，别再来一次」。**判据是「各分支的判据是否互斥」**：互斥则分层，重叠则标记。

第三处是「配置与事实分离」。主核里对应的是 `EXPLAIN (VERBOSE)` 输出「实际用的索引」——计划里写的是「可能用哪个」，执行后要报告「实际用了哪个」。两者都是因为**运行中的决策可能与静态配置不同**。

对 RAG 工程的实际意义在于：解析层是「异构输入」的第一道关，它的设计质量决定了后面所有环节的上限。这份实现给出的三条经验可直接用：**媒体分派要用显式标记而不是嵌套 if**（否则「谁处理了」会变成隐性知识）、**把实际用的后端写进产物**（配置不等于事实）、以及**不把二进制跨组件传**（传引用，需要时再读）。
