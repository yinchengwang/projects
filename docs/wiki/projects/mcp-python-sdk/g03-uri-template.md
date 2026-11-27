---
project: mcp-python-sdk
feature: uri-template
title: URI 模板展开与匹配
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/shared/uri_template.py
    symbols: [UriTemplate, _Expression, InvalidUriTemplate]
---

# URI 模板展开与匹配

## 一句话职责

实现 RFC 6570 的双向能力：**展开**（模板 + 变量 → URI，规范有定义）与**匹配**（URI → 变量，规范明确不管）。匹配一侧是 MCP 服务端把 `resources/read` 路由到处理器的关键，而它的做法是不回溯的两端扫描。

## 机制是什么

`UriTemplate.parse(template)` 产出不可变对象，对外两个主要方法 `expand(variables)` 与 `match(uri)`，另有 `variables` / `variable_names` / `query_variable_names` 三个只读属性。

内部表示分三层：

| 层 | 结构 | 用途 |
|---|---|---|
| 语法层 | `_parts: list[str \| _Expression]` | 字面量与 `{...}` 表达式按位置交错，供 `expand` 原序走一遍 |
| 扁平层 | `_prefix` / `_greedy` / `_suffix`（`_Lit` 与 `_Cap` 原子序列） | 把运算符的前缀与分隔符降级成字面量，扫描只处理两种原子 |
| 查询层 | `_query_variables: list[Variable]` | 尾部 `{?...}`/`{&...}` 从扫描里切出去，单独按查询串处理 |

`_Expression` 是一个 `{...}` 表达式（一个运算符 + 若干变量），`Variable` 是 `name` + `operator` + `explode`。`InvalidUriTemplate` 是唯一的解析错误，带 `template` 与可选的 `position` 便于定位。

## 怎么实现的

**支持范围是「Level 1–3 全部 + Level 4 的 explode」，且明确列出不支持的东西。** 模块 docstring 写得很清楚：Level 4 的前缀修饰符 `{var:N}` 与查询 explode `{?var*}` 不支持。这比「声称支持 RFC 6570」诚实得多——列出缺口，调用方才知道哪些模板会在解析期被拒。

**匹配是自研的，因为规范把这件事推给了正则。** docstring 引了 RFC 6570 §1.4：匹配语义明确交给各语言的 regex。而正则在这里不可接受，因为模板由用户提供，一条恶意模板足以产生灾难性回溯。于是实现选了**两端扫描且永不回溯**，复杂度 O(n·v)（n 是 URI 长度，v 是变量数）。docstring 还给了一句关键的结论：现实模板 v < 10，所以实际上就是线性；**不存在能产生超多项式时间的输入**。这是把「ReDoS 不可能」变成结构性保证，而不是靠限制输入。

**「至多一个多段变量」是整个匹配算法可以成立的前提。** `_is_greedy` 判定规则是：`explode` 为真，或运算符是 `+` / `#`。docstring 解释了为什么限一个：两个这样的变量在一个模板里**本质歧义**（多出来的那一段该归谁？），所以在解析期就拒。

同时被拒的还有「两个变量相邻且中间没有字面量」，包括「变量紧贴多段变量」。原因同样直接：扫描没有锚点可以确定边界。这里有一个很漂亮的细则：**会自带前导字符的运算符，其前导字符本身就充当了那个锚点**。所以 `{+path}{.ext}` 与 `{a}{.b}` 合法（`.` 运算符自己发 `.`），而 `{+path}{ext}` 与 `{a}{b}` 不合法。文档把这句写成了对照句，说明它是刻意的语义而不是实现限制。

**懒/贪的分配规则被精确定义。** 多段变量**之前**的有界变量按懒匹配（找后续字面量的首次出现），**之后**的按贪匹配（找前一字面量的末次出现）；没有多段变量时全程贪匹配，与正则语义一致。这解释了 `_scan_prefix` 用 `find`（最小跨度）、`_scan_suffix` 用 `rfind`（最小跨度但从右往左看，等价于模板顺序里靠前的变量更贪）。

**两端扫描的执行顺序有讲究。** `_scan` 在多段变量存在时**先扫后缀**：`_scan_suffix(..., anchored=False)` 得到 `suffix_start`，然后 `_scan_prefix(..., suffix_start)` 以前者的位置为天花板扫前缀，中间的缝隙就是多段变量的值。注释解释了为什么这个顺序是对的：解析器已保证非空后缀以字面量开头，所以 `rfind` 推出的锚点不依赖前缀扫到多远。代码还留了一行显式守卫（`suffix_start < prefix_end` 返回 `None`）而不是 `assert`，注释说明理由：**万一将来出现回归，应表现为「不匹配」而不是抛异常**。

**没有多段变量时，整条原子序列当作后缀锚定扫描。** `anchored=True` 带来一条特例：`atoms[0]` 必须在位置 0 匹配，而不是它最右出现的位置。注释给了真实反例：模板 `prefix-{id}` 对 URI `prefix-prefix-123`，用 `rfind` 会落到第二个 `prefix` 上，把 `id` 解成 `123` 而丢掉前面一段。

**`ifemp` 处理的是 `;` 运算符的一个特例。** RFC 6570 §A 规定具名变量展开为空值时，`;` 只发 `;name` 而 `?`/`&` 发 `name=`。这把「空值」变成两种线形态，所以扫描必须两种都接受。`_Cap.ifemp` 字段与两处专用分支就是为此存在的。`_scan_prefix` 里有一个顺序细节值得记住：**先检查后续字面量是否就地开始（值空），再检查 `=`**，注释说明否则「本身以 `=` 开头的字面量会被误当成 ifemp 分隔符」。

**查询尾部是另一套语义，且切换条件很严格。** `_split_query_tail` 只在三个条件同时满足时才把尾部 `{?...}`/`{&...}` 切出去：

1. 模板尾部是连续的一段 `?`/`&` 表达式；
2. 这一段**第一个**必须是 `{?...}` —— 因为只有 `?` 运算符会发出可被 `partition("?")` 找到的 `?`；单独的 `{&page}` 发的是 `&`，切不开；
3. 路径部分既不含字面 `?`/`#`，也不含 `{?...}`/`{#...}` 表达式——否则 `partition("#")` 再 `partition("?")` 会把路径扫描期望看到的字符剥掉。

任一条不满足就退回严格扫描。这套条件写得很细，是因为「宽松查询匹配」与「严格路径扫描」共享同一段字符串，混用会产生静默错误而不是报错。

**宽松查询匹配有三个自由度的理由被写明了。** `query_variable_names` 的 docstring 说这些变量「在一个 URI 里缺失也算匹配，缺失的名字只是不在结果里」，于是**下游函数的默认值能生效**。`match` 的例子演示了顺序无关、部分给出、多余参数被忽略三种情形。

**查询解析不用 `parse_qs`，理由是 `+` 语义。** `_parse_query` 的 docstring 说明：`urllib.parse.parse_qs` 按 form-urlencoding 把 `+` 当空格，而 RFC 3986 里 `+` 是字面子分隔符，只有 `%20` 编码空格；RFC 6570 与 MCP 资源 URI 走的是后者。

**查询解析**不解码参数名**，这是一条安全措施。** 同一段 docstring 给了理由：RFC 6570 展开从不编码变量名，所以合法匹配里名字总是字面形式；若解码名字，`%74oken=evil&token=real` 就能靠「先到先得」用假名遮住真的 `token`。同时它保留重复键的第一个值，把没有 `=` 的对当空值。

**保留展开会放行已存在的 pct-triplet。** `_encode` 在 `allow_reserved` 时不是简单 `quote(safe=_RESERVED)`，而是先按 `%XX` 切分：三元组原样透传，缝隙之间做 quote。docstring 说明这是 RFC 6570 §3.2.3 的要求，同时补一句：**没有跟两个十六进制位的裸 `%` 仍会被编成 `%25`**。这个区分让「已经是编码形态的路径」不会被二次编码。

**`{+var}` 不编码 `?` 与 `#`，但扫描会在那里停。** docstring 的最后一段交代了这个不对称：值里含字面 `?` 或 `#` 时 **expand 正常、match 回不来**。原因是 `_STOP_CHARS['+']` 是 `"?#"`，扫描到此为止，于是 `{+path}{?q}` 能把路径与查询分开。这是一个刻意的取舍：保 round-trip 就会失去「路径与查询可分离」的能力，而后者对 MCP 更重要。

**`_STOP_CHARS` 是每个运算符的扫描停止集。** 这张表把正则的字符类边界翻译成了「遇到这些就停」，因此不需要回溯。表里有注释解释每项语义：简单展开 `""` 停 `/? #&,`（一切结构性字符都被百分号编码了，所以见到就说明值结束）；`.` 停下一个 `.`；`/` 停下一个 `/`；`;` 停 `;/?#`；`?`/`&` 停 `&#`；`#` 什么都不停（fragment 是 URI 尾巴）。

**解析期还有两条结构校验。** `_check_duplicate_variables` 拒绝同名变量重复出现——理由是 RFC 6570 要求重复变量展开为同值，匹配就得做反向引用，代价可能指数级；与其静默只保留最后一次捕获，不如在解析期拒。`_check_single_query_expression` 拒绝两个 `{?...}`——因为 `?` 运算符自带前导 `?`，两条会产出两个 `?`，按 RFC 3986 §3.4 是畸形；正确写法是 `{?a,b}` 或 `{?a}{&b}`。

**规模限制是显式的三档。** `DEFAULT_MAX_TEMPLATE_LENGTH`（8192）、`DEFAULT_MAX_VARIABLES`（256）、`DEFAULT_MAX_URI_LENGTH`（65536）。`max_variables` 的文档解释了一个容易漏的绕过方式：**数变量而不是数表达式**，因为一条 `{v0,v1,...,vN}` 可以把任意多变量塞进一个表达式计数里。超长 URI 在 `match` 里直接返回 `None` 而不扫描。

**explode 的匹配要求 span 以分隔符开头。** `_extract_greedy` 的注释说明：`{/a*}` 展开成 `/x/y` 而不是 `x/y`，而扫描不消费分隔符本身，所以非空 span 的第一个字符必须是分隔符。它还处理了「后续空段是合法值」的情形：`{/path*}` 对 `["a","","c"]` 展开成 `/a//c`，所以 `split(sep)[1:]` 保留空段（只有第 0 段因必为空被丢）。具名 explode 还要逐段校验前缀（`name=` 或裸 `name` 表示空值），不合就 `None`。

## 关键取舍

**源码事实：** 匹配语义不在 RFC 内，实现选了不回溯的两端扫描（O(n·v)），并把「无超多项式输入」写成了设计结论，而不是靠限制输入长度。

**源码事实：** 一个模板至多一个多段变量；两个变量相邻（含紧贴多段变量）在解析期被拒。自带前导字符的运算符提供锚点，所以 `{a}{.b}` 合法、`{a}{b}` 不合法。

**源码事实：** 多段变量之前懒匹配、之后贪匹配；无多段变量时全程贪匹配并与正则语义一致。

**源码事实：** 无多段变量时首原子锚定在位置 0，不用 `rfind`。这是为了修 `prefix-{id}` 对 `prefix-prefix-123` 的错解。

**源码事实：** 保留展开会原样放行已有的 `%XX` 三元组，但裸 `%` 仍编码。避免二次编码。

**源码事实：** `{+var}` 不编码 `?`/`#`，但 `match` 在那里停止扫描，于是含字面 `?`/`#` 的值无法 round-trip。

**源码事实：** 宽松查询匹配只在三个条件同时成立时启用；否则退回严格扫描。条件之严是因为两条路径共享同一段字符串。

**源码事实：** 查询参数名不解码，防止 `%74oken=evil` 靠先到先得遮住 `token`。

**源码事实：** 变量名重复被解析期拒绝，因为反引用匹配代价可能指数级。

**源码事实：** 规模限制按变量数而非表达式数计，堵住单表达式塞入海量变量的绕过。

**架构推论：** 这个模块的定位是「把协议里没定义的那一半用可预测的代价补上」。RFC 6570 只管展开，MCP 需要反向路由；反向路由要么用正则（语义现成、代价不可控），要么自研扫描（代价可控、语义要自己写清楚）。这里选后者，并把「语义的每一条边界」都写进 docstring：歧义模板解析期拒、round-trip 的失效条件、查询宽松度、`+` 不当空格。这些都不是实现细节，而是调用方必须知道的契约——因为它们决定「哪条模板能路由到哪个 URI」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/shared/uri_template.py` | `UriTemplate` | 不可变模板对象，`expand` / `match` 的入口 |
| `src/mcp/shared/uri_template.py` | `UriTemplate.parse` | 解析 + 规模校验 + 查询尾部切分 + 贪变量分片 |
| `src/mcp/shared/uri_template.py` | `UriTemplate.match` | 两阶段匹配：尾部查询走宽松解析，其余走线性扫描 |
| `src/mcp/shared/uri_template.py` | `UriTemplate._scan` | 两端扫描：先锚定后缀再以前者为界扫前缀，缝隙归多段变量 |
| `src/mcp/shared/uri_template.py` | `_Expression` | 一个 `{...}` 表达式：运算符 + 变量列表 |
| `src/mcp/shared/uri_template.py` | `Variable` / `_is_greedy` | 变量及其「能否跨段」的判定 |
| `src/mcp/shared/uri_template.py` | `InvalidUriTemplate` | 唯一解析错误，带模板与位置 |
| `src/mcp/shared/uri_template.py` | `_OPERATOR_SPECS` / `_STOP_CHARS` | 每个运算符的展开规格与扫描停止集 |
| `src/mcp/shared/uri_template.py` | `_expand_expression` | 单表达式展开，含标量/序列、explode、`ifemp` 分支 |
| `src/mcp/shared/uri_template.py` | `_encode` | 保留展开放行 pct-triplet，其余按运算符编码 |
| `src/mcp/shared/uri_template.py` | `_scan_prefix` / `_scan_suffix` | 最小跨度的左右扫描，`ifemp` 特例在此 |
| `src/mcp/shared/uri_template.py` | `_extract_greedy` | 多段变量值解码，explode 逐段校验 |
| `src/mcp/shared/uri_template.py` | `_split_query_tail` | 判定是否可切出宽松查询尾部 |
| `src/mcp/shared/uri_template.py` | `_parse_query` | RFC 3986 语义的查询解析（`+` 不是空格、名字不解码） |
| `src/mcp/shared/uri_template.py` | `_check_duplicate_variables` / `_check_single_query_expression` | 两条结构校验，代价与畸形理由都很具体 |

## 参考

- [mcp-python-sdk 源码：shared/uri_template.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/uri_template.py)
- [RFC 6570：URI Template](https://datatracker.ietf.org/doc/html/rfc6570)
- [MCP 规范：资源与资源模板](https://modelcontextprotocol.io/specification/2025-06-18/server/resources)
- [RFC 3986：URI 通用语法](https://datatracker.ietf.org/doc/html/rfc3986)

## 与主核 / AI 的关系

URI 模板匹配与主核里「LIKE 前缀模式走索引」是同一类问题：给定一个带通配的声明，判断某个具体值是否落入其中并抽出绑定。主核的选择是「能转成索引范围就转，转不了就顺序扫」；这里的等价选择是「能线性扫就线性扫，语义有歧义就解析期拒」。

对 agent 工程的直接价值有三条。其一，**模板匹配不要用用户提供的模式去编译正则**。这是 ReDoS 的经典入口，而 agent 系统恰恰经常让模型或用户提供这类模式。这里的做法——固定代价的扫描 + 解析期拒绝歧义模板——比「加个超时」更可靠，因为它在结构上消除了可能性。其二，**歧义要早拒，不要猜**。「两个多段变量谁吃多余那段」没有正确答案，静默选一个会让路由在某些 URI 上错到难以定位。其三，**宽松匹配的适用条件必须写死**。查询参数可以乱序、可以缺、可以多，但路径不能——把这条边界落在代码里的方式（`_split_query_tail` 的三个条件）比写在文档里更可靠。
