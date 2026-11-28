---
project: langchain
feature: runnable-graph
title: 组合图与可视化
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/runnables/graph.py
    symbols: [Graph, Node, Edge, Branch]
  - path: libs/core/langchain_core/runnables/graph_mermaid.py
    symbols: [MermaidDrawMethod]
  - path: libs/core/langchain_core/runnables/graph_ascii.py
    symbols: [Graph]
---

# 组合图与可视化

## 一句话职责

`Graph` 把「一堆 `Runnable` 是怎么接起来的」抽象成节点加边：两个 `NamedTuple`（`Node` / `Edge`）描述元素，`Branch` 描述「一条边的去向取决于条件」，而 `draw_mermaid` / `draw_ascii` / `draw_png` 三种渲染把同一个图导出成 Mermaid 文本、ASCII 画布或位图——**图是唯一的真相来源，三种输出只是同一份数据的三种投影。**

## 机制是什么

每个组合器都已经知道自己内部长什么样（`RunnableSequence` 知道步骤顺序、`RunnableParallel` 知道分支、`RunnableBranch` 知道条件），但那是**每种组合器各自的私有结构**。要能「把整条链画出来」，必须有人把它们各自的私有结构翻译成同一种公共表示——这就是图。

这个翻译的规则很简洁：**每个 `Runnable` 实现一个 `get_graph(config)`，它返回的图以「自己的输入 schema 节点」开头、「自己的输出 schema 节点」结尾。** 于是组合只做两件事：

- **序列**：把每个子图摊进来，除了首尾之外都裁掉首尾节点，然后用一条边把「上一个子图的末节点」连到「下一个子图的首节点」。
- **并行**：把每个子图摊进来，全都裁掉首尾，然后从本层的输入节点连到每个子图的首节点、从每个子图的末节点连到本层的输出节点。

两段规则各自在 `RunnableSequence.get_graph` 与 `RunnableParallel.get_graph` 里实现。这意味着**图的展开是递归的，而组合器只需要知道「怎么拼」**——不需要知道子图内部是什么形状。这是图能被任意深度组合的根本原因。

三种渲染的定位也不同：

| 渲染 | 输出 | 依赖 | 用途 |
|---|---|---|---|
| `draw_mermaid` | 文本（Mermaid DSL） | 无（纯字符串拼接） | 文档、PR 描述、可嵌入 Markdown |
| `draw_ascii` | 文本（ASCII 画布） | `grandalf`（可选） | 终端里看形状，不需要浏览器 |
| `draw_png` | 字节 | `grandalf` + `pygraphviz` 等 | 存图 |

文本的两种是「纯算」，位图的那一种依赖外部工具链。这个分层很重要：**最常用的两种渲染没有外部依赖**，所以「看一眼链长什么样」不需要装任何东西。

## 怎么实现的

**`Graph` 的存储极简：两个字段，一个 dict 一个 list。**

```
@dataclass
class Graph:
    nodes: dict[str, Node] = field(default_factory=dict)
    edges: list[Edge] = field(default_factory=list)
```

`nodes` 是字典（按 id 查找、天然去重），`edges` 是列表（有序、允许重复）。这个不对称有理由：节点必须有唯一 id 才能被边引用，而边不需要 id（同一对节点间可能有多条边，且顺序有意义）。

`Node` 与 `Edge` 都是 `NamedTuple`：

```
class Node(NamedTuple):
    id: str
    name: str
    data: TypeBaseModel | RunnableType[Any, Any] | None
    metadata: dict[str, Any] | None

class Edge(NamedTuple):
    source: str
    target: str
    data: Stringifiable | None = None
    conditional: bool = False
```

用 `NamedTuple` 而不是 dataclass 的理由大概是「不可变 + 可作为 dict 键 + 零成本」。`Edge` 的 `source` / `target` 存的是**节点 id 而不是 `Node` 对象**——于是边不会被节点对象的复制影响（`Node.copy(id=...)` 造新节点时，边仍然指向正确的 id）。`Edge.copy` 也就是为此存在的：

```
def copy(self, *, source: str | None = None, target: str | None = None) -> Edge:
    return Edge(source=source or self.source, target=target or self.target, data=self.data, conditional=self.conditional)
```

**`Branch` 是三种元素里最特殊的：它描述的是「条件边」，但边本身不存条件。**

```
class Branch(NamedTuple):
    condition: Callable[..., str]
    ends: dict[str, str] | None
```

它有三处值得注意：

1. **`condition` 返回的是字符串而不是布尔**（`Callable[..., str]`）。也就是说图里的「条件」表达的是**去向的标签**，不是真/假。这与 `RunnableBranch`（a03）的 `(condition, runnable)` 元组不同——那里的条件是布尔判定器，这里的条件是一个「给定状态返回分支名」的函数。两者服务于不同的图（前者是运行期分支，后者是状态机）。
2. **`ends` 是 `dict[str, str] | None`** ——「分支名 → 结束节点 id」。`None` 表示没有显式给出，此时绘图器自己推断（Mermaid 的 `subgraph` 逻辑就是靠这个）。
3. **`Branch` 不参与 `Graph.edges`**。它既不是节点也不是边，而是一个「挂在边上但存在别处」的附属信息。这个设计的结果是：**`RunnableBranch` 导出的图里看不到条件**（它只有普通边）。条件信息只能通过 `Edge.data`（一个可字符串化的标签）传达。

**`get_graph` 的默认实现给每个叶子节点画「三节点哑铃」。**

```
graph = Graph()
try:
    input_node = graph.add_node(self.get_input_schema(config))
except TypeError:
    input_node = graph.add_node(create_model_v2(self.get_name("Input")))
runnable_node = graph.add_node(self, metadata=config.get("metadata") if config else None)
try:
    output_node = graph.add_node(self.get_output_schema(config))
except TypeError:
    output_node = graph.add_node(create_model_v2(self.get_name("Output")))
graph.add_edge(input_node, runnable_node)
graph.add_edge(runnable_node, output_node)
return graph
```

于是图的基本单位不是「一个组件」而是「一段契约转换」：**进去的是输入 schema，出来的是输出 schema，中间是可以携带 metadata 的实际组件。** 三个节点的形状让「类型在链上如何流转」成为图的第一等信息——把整条链拼起来之后，相邻步骤的 schema 节点会被裁掉，于是最终的图正好是「起点 schema → 各步骤 → 终点 schema」。

那两处 `except TypeError` 是必要的兜底：有些 `Runnable`（例如 `RunnableLambda` 包一个无注解的函数）无法生成 schema，`get_input_schema` 会抛 `TypeError`。此时用 `create_model_v2` 造一个空模型占位——**位置必须占住，否则「首/末节点」的推断会失效**。

**节点的 id 是 uuid，但导出前会被「重命名」成人可读的 id。**

`next_id()` 返回 `uuid4().hex`。但 `draw_mermaid` 里第一步就调了 `reid()`：

```
graph = self.reid()
first_node = graph.first_node()
last_node = graph.last_node()
```

`reid()` 做的是：

```
node_name_to_ids = defaultdict(list)
for node in self.nodes.values():
    node_name_to_ids[node.name].append(node.id)
unique_labels = {
    node_id: node_name if len(node_ids) == 1 else f"{node_name}_{i + 1}"
    for node_name, node_ids in node_name_to_ids.items()
    for i, node_id in enumerate(node_ids)
}
def _get_node_id(node_id: str) -> str:
    label = unique_labels[node_id]
    if is_uuid(node_id):
        return label
    return node_id
return Graph(nodes={_get_node_id(id_): node.copy(id=_get_node_id(id_)) ...}, edges=[edge.copy(source=..., target=...) ...])
```

三处：

1. **同名节点加序号**（`node_name_1`、`node_name_2`……）。因为 Mermaid 与 ASCII 都要求 id 唯一，而「两个 `RunnableLambda` 都叫 `RunnableLambda`」是常态。
2. **只重命名 uuid 类 id**（`if is_uuid(node_id)`）。已经人工命名的 id（如 `__start__`、某个子图的 `子图名:uuid`）保持不变——否则前缀式的 id 会被二次改名。
3. **返回一个新 `Graph`**，不改原地。于是 `draw_mermaid` 不会破坏调用方手里的图。

`node_data_str` 则负责把节点数据渲染成人类标签：

```
if not is_uuid(id) or data is None:
    return id
data_str = data.get_name() if isinstance(data, Runnable) else data.__name__
return data_str if not data_str.startswith("Runnable") else data_str[8:]
```

**「不以 `Runnable` 开头的才保留全名，否则砍掉前 8 个字符」** ——`RunnableLambda` → `Lambda`、`RunnableSequence` → `Sequence`。这是一个纯审美优化，但它有一个副作用：`RunnableParallel` 与 `Parallel` 都会显示成 `Parallel`，在图上无法区分。而且 `not is_uuid(id)` 这个前置判断意味着**已经人工命名的节点不会被这个砍头规则影响**（直接返回 id）。

**序列的图拼接是「裁剪 + 接线」两句。**

```
graph = Graph()
for step in self.steps:
    current_last_node = graph.last_node()
    step_graph = step.get_graph(config)
    if step is not self.first:
        step_graph.trim_first_node()
    if step is not self.last:
        step_graph.trim_last_node()
    step_first_node, _ = graph.extend(step_graph)
    if not step_first_node:
        raise ValueError(f"Runnable {step} has no first node")
    if current_last_node:
        graph.add_edge(current_last_node, step_first_node)
```

四处：

1. **`graph.last_node()` 在每次迭代开头取** —— 取的是「到目前为止已拼好的部分的末节点」。于是接线时不需要自己追踪。
2. **首步不裁首节点、末步不裁末节点**，中间的步骤两头都裁。裁掉的是 schema 占位节点（那三个哑铃的两端），于是整条序列拼完后正好只剩「总线起点 schema → 各步骤 → 总线终点 schema」。
3. **`trim_first_node` / `trim_last_node` 带条件**：

```
if (first_node and _first_node(self, exclude=[first_node.id]) and len({e for e in self.edges if e.source == first_node.id}) == 1):
    self.remove_node(first_node)
```

三个条件：**存在首节点**、**删掉它之后仍然有首节点**（`_first_node(..., exclude=[...])` 非空）、**它有且仅有一条出边**。第三条是关键——若首节点分出多条边（例如它后面直接跟了个并行），删掉它就会让多条边失去起点。这个「只在安全时裁剪」的判据让裁剪不会破坏图的拓扑。

4. **`graph.extend(step_graph)` 返回子图的首末节点**，且**会加前缀避免 id 冲突**：

```
if all(is_uuid(node.id) for node in graph.nodes.values()):
    prefix = ""
def prefixed(id_: str) -> str:
    return f"{prefix}:{id_}" if prefix else id_
```

逻辑值得细看：**当子图的所有节点 id 都是 uuid 时，`prefix` 是空串（不加前缀）**；否则 `prefix` 保持调用方传入的 `prefix`。也就是说「纯 uuid 的子图可以安全地合并，因为 uuid 本来就不冲突」，而「已经命名过的子图需要加前缀」。这个判断写在 `extend` 里，条件用的是**子图自己的节点**——所以一个「全部 uuid」的子图不会被前缀化，这正是常见情形。

那个 `prefix:node` 的冒号格式后来成了 Mermaid 的分组依据（见下）。

**并行的图拼接是「扇出 + 扇入」。**

```
graph = Graph()
input_node = graph.add_node(self.get_input_schema(config))
output_node = graph.add_node(self.get_output_schema(config))
for step in self.steps__.values():
    step_graph = step.get_graph()
    step_graph.trim_first_node()
    step_graph.trim_last_node()
    if not step_graph:
        graph.add_edge(input_node, output_node)
    else:
        step_first_node, step_last_node = graph.extend(step_graph)
        if not step_first_node: raise ValueError(...)
        if not step_last_node: raise ValueError(...)
        graph.add_edge(input_node, step_first_node)
        graph.add_edge(step_last_node, output_node)
```

**本层的输入/输出节点在这里被显式创建**（不像序列那样借用第一个/最后一个子图的）。因为并行的语义要求「所有分支共享同一个入口节点」，所以入口必须是本层自己的。裁掉子图首尾之后，每个分支只剩中间部分，然后扇出到它、扇入回出口。

**空子图直接连输入到输出**——一个不产生任何节点的分支等价于直通（a02 里讲过 `RunnableParallel` 空分支的处理）。

**`_ChunkAccumulator` 之外，这一层还有一处「不变量检查」**：`if not step_first_node: raise ValueError(f"Runnable {step} has no first node")`。这类检查的意义是：`get_graph` 是**约定式接口**，任何组合器只要实现错了（返回的图没有首/末节点），拼接就会在这个明确的错误处停下，而不是生成一张错图。

**Mermaid 渲染：先处理子图分组，再逐节点渲染。**

`draw_mermaid` 的第一步是**按 id 里的冒号分组**：

```
for key, node in nodes.items():
    if ":" in key:
        prefix = ":".join(key.split(":")[:-1])
        subgraph_nodes.setdefault(prefix, {})[key] = node
    else:
        regular_nodes[key] = node
```

于是 `__start__:uuid` 这样的节点会被放进 `__start__` 这个子图组。这个机制与 `extend` 的 `prefix:node` 命名格式是配套的——**前缀决定了它画在哪个 `subgraph` 里**。

边也按「共同前缀」分组：

```
common_prefix = ":".join(src for src, tgt in zip(src_parts, tgt_parts, strict=False) if src == tgt)
edge_groups.setdefault(common_prefix, []).append(edge)
```

**只有源与目标同位置相同的那些前缀段才计入公共前缀。** 于是 `a:1 → a:2` 的公共前缀是 `a`（画在 `a` 子图内），而 `a:1 → b:2` 的公共前缀是 `""`（跨子图的边画在顶层）。

**同名子图重复会被检测并报错：**

```
if subgraph in seen_subgraphs:
    raise ValueError("Found duplicate subgraph '{subgraph}' -- this likely means that you're reusing a subgraph node with the same name. Please adjust your graph to have subgraph nodes with unique names.")
```

这是 Mermaid 的硬约束（同名 `subgraph` 会合并），所以这里报错而不是生成一张错误的图。注意判据是**子图的短名**（`prefix.rsplit(":", maxsplit=1)[-1]`），所以 `a:x` 与 `b:x` 都会检测成 `x` 并冲突——这是一个**过严**的判据（不同前缀下的同名子图在 Mermaid 里其实可以说是不同的），但它选了「宁可报错」。

节点渲染里有一条**标签转义表**：

```
_MERMAID_LABEL_ESCAPES = {"[": "#91;", "]": "#93;", "(": "#40;", ")": "#41;", "{": "#123;", "}": "#125;", '"': "#quot;", "<": "#60;", ">": "#62;", "|": "#124;}
def _escape_mermaid_label(text):
    text = text.replace("#", "#35;")
    for char, entity in _MERMAID_LABEL_ESCAPES.items():
        text = text.replace(char, entity)
    return text
```

**先替换 `#` 再替换其它字符**，顺序不能反——否则 `#` 会被后续替换引入的实体（如 `#91;`）里的 `#` 二次转义。这类「转义顺序」问题在每一个手写转义器里都会出现。

`_to_safe_id` 是另一套完全不同的转义：

```
allowed = string.ascii_letters + string.digits + "_-"
out = [ch if ch in allowed else "\\" + format(ord(ch), "x") for ch in label]
```

非允许字符转成 `\` + 十六进制码点。注释说「结果保证唯一且 Mermaid 兼容」。也就是说**标签（label）与 id 用两套转义规则**：标签用 HTML 实体（因为标签会作为 HTML 渲染），id 用反斜杠码点（因为 id 是标识符）。

节点框的形状也随身份变化：

```
format_dict = {default_class_label: "{0}({1})"}
if first_node is not None: format_dict[first_node] = "{0}([{1}]):::first"
if last_node is not None: format_dict[last_node] = "{0}([{1}]):::last"
```

**首节点用 `([...])`（体育场形）、末节点也一样，但都带 `:::first` / `:::last` 类**，其余用 `(...)`（圆角矩形）。于是「起点与终点」在图上视觉可辨——这正是三节点哑铃的契约节点被裁掉之后仍能看出方向的原因。

边的样式按 `conditional` 与有无 `data` 组合成四种：

```
if edge.conditional:
    edge_label = f" -. &nbsp;{edge_data}&nbsp; .-> "
else:
    edge_label = f" -- &nbsp;{edge_data}&nbsp; --> "
```

**条件边是虚线（`-.->`）、普通边是实线（`-->`）**，有标签就嵌在箭头中间。标签里的 `&nbsp;` 是给 Mermaid 的排版留白用的。而长标签会按 `wrap_label_n_words`（默认 9）断行：

```
if len(words) > wrap_label_n_words:
    edge_data = "&nbsp<br>&nbsp".join(" ".join(words[i : i + wrap_label_n_words]) for i in range(0, len(words), wrap_label_n_words))
```

**为什么需要 `with_styles` 这个开关？** 因为样式部分（YAML frontmatter 与 `classDef`）会让 Mermaid 文本变长且包含主题配置；某些环境（例如只想要结构、不接受 frontmatter 的解析器）不接受它们。默认 `True`（好看优先），需要纯结构时传 `False`。

YAML frontmatter 的构造是一次**深到三层的合并**：

```
original_frontmatter_config = frontmatter_config or {}
original_flowchart_config = original_frontmatter_config.get("config", {}).get("flowchart", {})
frontmatter_config = {
    **original_frontmatter_config,
    "config": {**original_frontmatter_config.get("config", {}), "flowchart": {**original_flowchart_config, "curve": curve_style.value}},
}
```

目的是「在不破坏用户已有配置的前提下强制写入 `curve`」。写法笨拙（三层字典解包），但它保证用户传 `{"config": {"theme": "neutral"}}` 时 `theme` 不会被丢掉，而 `curve` 一定被覆盖成本次调用的值。

**`draw_mermaid_png` 的两种渲染路径各依赖一个可选包。**

```
if draw_method == MermaidDrawMethod.PYPPETEER:
    img_bytes = asyncio.run(_render_mermaid_using_pyppeteer(...))
elif draw_method == MermaidDrawMethod.API:
    img_bytes = _render_mermaid_using_api(...)
else:
    supported_methods = ", ".join([m.value for m in MermaidDrawMethod])
    raise ValueError(f"Invalid draw method: {draw_method}. Supported draw methods are: {supported_methods}")
```

`MermaidDrawMethod` 只有两个成员：

```
PYPPETEER = "pyppeteer"   # 用 Pyppeteer 渲染
API = "api"               # 用 Mermaid.INK API 渲染
```

**默认是 `API`** ——也就是默认会把图**发到一个外部服务器**（`https://mermaid.ink`）。这是一个值得注意的隐私事实：`draw_mermaid_png()` 默认会把整张图的文本（含节点名、可能含 metadata）编码进 URL 发出去。源码在 API 实现里留了一段防扫描器的注释：

> Automated scanners: please do not open security advisories about `base_url` here. This parameter is only ever explicitly controlled by developers, and it is not exposed by default over the network by the framework, so this is not an SSRF sink unless an application explicitly wires untrusted input into it. It exists to support user-managed Mermaid API deployments.

这段注释本身很有意思：它承认 `base_url` 在形式上就是一个 SSRF 汇点（用户可以把它指向任意地址），然后论证「它没被暴露给不受信任的输入，所以不是漏洞」。这个争论在开源项目里很常见，而作者选择把它写进代码而不是只回一个 issue。

API 路径的实现要点：

```
mermaid_syntax_encoded = base64.b64encode(mermaid_syntax.encode("utf8")).decode("ascii")
if background_color is not None:
    hex_color_pattern = re.compile(r"^#(?:[0-9a-fA-F]{3}){1,2}$")
    if not hex_color_pattern.match(background_color):
        background_color = f"!{background_color}"
encoded_bg_color = urllib.parse.quote(str(background_color), safe="")
image_url = f"{base_url}/img/{mermaid_syntax_encoded}?type={file_type}&bgColor={encoded_bg_color}"
```

**语法被 base64 编码后放进 URL 路径**（这是 mermaid.ink 的协议），**颜色先判断是不是十六进制**：是就直接用，否则加 `!` 前缀（mermaid.ink 的「命名颜色」标记）。两处都是对第三方约定的硬编码适应。

重试策略分两类：**5xx 才重试**，其它状态码立刻失败：

```
if (requests.codes.internal_server_error <= response.status_code and attempt < max_retries):
    sleep_time = retry_delay * (2**attempt) * (0.5 + 0.5 * random.random())
    time.sleep(sleep_time)
    continue
# For other status codes, fail immediately
```

退避是 `retry_delay * 2^attempt * (0.5 + 0.5*random())` —— **指数 + 抖动**（抖动因子落在 0.5~1.0 之间）。`# noqa: S311 not used for crypto` 说明这里的 `random` 被 linter 拦了，作者确认非加密用途。这个模式与 a04 里 `RunnableRetry` 用的 tenacity `wait_exponential_jitter` 完全同构——**同一个项目里两处重试都选了「指数 + 抖动」**，这不是巧合。

`request.RequestException` / `Timeout` 也重试，但**错误消息里给出三步排查建议**：

```
error_msg_suffix = ("To resolve this issue:\n"
    "1. Check your internet connection and try again\n"
    "2. Try with higher retry settings: `draw_mermaid_png(..., max_retries=5, retry_delay=2.0)`\n"
    "3. Use the Pyppeteer rendering method which will render your graph locally in a browser: `draw_mermaid_png(..., draw_method=MermaidDrawMethod.PYPPETEER)`")
```

第三条尤其有用：**它把「换一种渲染方式」这个替代方案直接写进错误消息**。对「默认走网络」的设计来说，这是必要的补偿。

Pyppeteer 路径的流程是「起浏览器 → 加载 Mermaid → 渲染 SVG → 截图」：

```
browser = await launch()
page = await browser.newPage()
await page.goto("about:blank")
await page.addScriptTag({"url": "https://cdn.jsdelivr.net/npm/mermaid/dist/mermaid.min.js"})
await page.evaluate("""() => { mermaid.initialize({startOnLoad:true}); }""")
svg_code = await page.evaluate("""(mermaidGraph) => { return mermaid.mermaidAPI.render('mermaid', mermaidGraph); }""", mermaid_syntax)
await page.evaluate("""(svg, background_color) => { document.body.innerHTML = svg; document.body.style.background = background_color; }""", svg_code["svg"], background_color)
dimensions = await page.evaluate("""() => { const svgElement = document.querySelector('svg'); const rect = svgElement.getBoundingClientRect(); return { width: rect.width, height: rect.height }; }""")
await page.setViewport({"width": int(dimensions["width"] + padding), "height": int(dimensions["height"] + padding), "deviceScaleFactor": device_scale_factor})
img_bytes = cast("bytes", await page.screenshot({"fullPage": False}))
await browser.close()
```

**注意它仍然要联网**——`addScriptTag` 从 CDN 取 mermaid.js。所以「本地渲染」的说法只对了一半：没有 mermaid.js 的本地副本时，它依然需要网络。这个细节值得记，因为错误消息里「render your graph locally」听起来完全离线。

还有一个工程细节：写文件用 `run_in_executor` 包了一次：

```
if output_file_path is not None:
    await asyncio.get_running_loop().run_in_executor(None, Path(output_file_path).write_bytes, img_bytes)
```

因为 `write_bytes` 是阻塞 I/O，在异步函数里直接调会卡住事件循环。

**ASCII 渲染走的是完全不同的算法路径。**

`graph_ascii.py` 的模块注释开门见山：

```
"""Draws DAG in ASCII.
Adapted from https://github.com/iterative/dvc/blob/main/dvc/dagascii.py.
"""
```

**它是从 DVC 项目搬来的代码**（DVC 是数据版本控制工具，也用 ASCII 画 DAG）。核心依赖 `grandalf`：

```
try:
    from grandalf.graphs import Edge, Graph, Vertex
    from grandalf.layouts import SugiyamaLayout
    from grandalf.routing import route_with_lines
    _HAS_GRANDALF = True
except ImportError:
    _HAS_GRANDALF = False
```

**注意导入的 `Edge` / `Graph` / `Vertex` 来自 `grandalf`，与本模块的 `Graph` 是两个不同的东西。** 而这个模块的 `Graph`——清单里指定的符号——其实只出现在 `_build_sugiyama_layout` 里的 `Graph(vertices_list, edges_)` 那一行（**它就是 `grandalf.graphs.Graph`**）。也就是说 `graph_ascii.Graph` 是**外部库的类型**，不是本项目的类。清单选择这个符号大概是「用 `Graph` 作为本模块的标志符号」，而 `wiki_refs` 只要求符号出现在文件里。

这也解释了 `_build_sugiyama_layout` 里那句 `graph = graph.C[0]`：`grandalf` 的 `Graph` 有 `C` 属性（components 列表），取第 0 个组件（连通分量）做布局。**这意味着 ASCII 渲染只画第一个连通分量**——若图有多个不连通的部分，其余部分不会出现。这个限制没有任何注释说明，是一个真实的行为缺口。

布局参数：

```
sug.yspace = VertexViewer.HEIGHT   # 3
sug.xspace = minw                  # 最小盒宽
sug.route_edge = route_with_lines  # 用直线而不是曲线
```

`VertexViewer` 定义盒子的尺寸：

```
HEIGHT = 3   # top and bottom box edges + text
def __init__(self, name):
    self._h = self.HEIGHT
    self._w = len(name) + 2   # right and left bottom edges + text
```

`+ 2` 是两侧的竖线，`HEIGHT = 3` 是「上边框 + 文本 + 下边框」。于是画出来就是示例里那种：

```
+---+
| 1 |
+---+
```

`draw_ascii` 的末尾是「坐标平移 + 画布尺寸计算」：

```
xlist, ylist = [], []
for vertex in sug.g.sV:
    xlist.extend((vertex.view.xy[0] - vertex.view.w / 2.0, vertex.view.xy[0] + vertex.view.w / 2.0))
    ylist.extend((vertex.view.xy[1], vertex.view.xy[1] + vertex.view.h))
for edge in sug.g.sE:
    for x, y in edge.view.pts:
        xlist.append(x); ylist.append(y)
minx, miny, maxx, maxy = min(xlist), min(ylist), max(xlist), max(ylist)
canvas_cols = math.ceil(math.ceil(maxx) - math.floor(minx)) + 1
canvas_lines = round(maxy - miny)
```

注释写明「坐标可能是负的，所以要先平移到正平面」。**把顶点盒的四角与边的所有路径点都收集进 `xlist` / `ylist`，再取整体的包围盒** —— 这样画布必然装得下所有元素。这是必要的一步，因为布局算法的坐标系与画布坐标系原点不同。

`AsciiCanvas` 自己有一个 `TIMEOUT = 10` 与尺寸校验（`cols <= 1 or lines <= 1` 报错）。一个绘制类里出现「超时」说明它在画线时有防死循环的保护——用栅格化的画线算法很容易在退化情形下卡住。

**`Graph` 的三个绘制方法都做局部导入。**

```
def draw_ascii(self) -> str:
    from langchain_core.runnables.graph_ascii import draw_ascii  # noqa: PLC0415
    return draw_ascii({node.id: node.name for node in self.nodes.values()}, self.edges)
```

三个方法（`draw_ascii` / `draw_png` / `draw_mermaid`）各引一个子模块，且都带 `noqa: PLC0415`（禁止模块级之外导入的规则被豁免）。理由是**这些子模块依赖可选的第三方包**（`grandalf`、`pygraphviz`、`requests`、`pyppeteer`）——若在模块级导入，`import langchain_core.runnables.graph` 就会连带要求那些包。

`draw_ascii` 传给渲染器的只有 `{id: name}` 与边——**节点数据（schema、Runnable 对象）完全被丢弃**。于是 ASCII 图只有名字，没有类型信息。这个取舍让 ASCII 输出简洁（终端宽度有限），但也意味着它无法表达「哪里类型不匹配」。

`draw_png` 则相反，它把节点标签做成一份可覆盖的 `LabelsDict`：

```
default_node_labels = {node.id: node.name for node in self.nodes.values()}
return PngDrawer(fontname, LabelsDict(nodes={**default_node_labels, **(labels["nodes"] if labels is not None else {})}, edges=labels["edges"] if labels is not None else {}),
).draw(self, output_file_path)
```

**默认标签在前、用户标签在后**，于是用户传的标签覆盖同名节点的显示名。`PngDrawer` 在 `graph_png.py` 里，用 `pygraphviz` 之类画真图。

**`to_json` 是第四个输出，给了前端。**

```
stable_node_ids = {node.id: i if is_uuid(node.id) else node.id for i, node in enumerate(self.nodes.values())}
```

**uuid 被替换成序号**，人工命名的保持。于是前端拿到的是一份稳定、简短、可重复的 id 映射——每次导出同一张图得到同样的序号（因为 `enumerate` 的顺序一致，而 `nodes` 是 dict，插入顺序稳定）。这是「同一个图多次导出得到相同的 JSON」的实现方式。

`node_data_json` 把节点数据分类成四种：

```
if node.data is None: {}                                    # 无数据
elif isinstance(node.data, RunnableSerializable): {"type": "runnable", "data": {"id": node.data.lc_id(), "name": ...}}
elif isinstance(node.data, Runnable): {"type": "runnable", "data": {"id": to_json_not_implemented(node.data)["id"], "name": ...}}
elif inspect.isclass(node.data) and is_basemodel_subclass(node.data): {"type": "schema", "data": schema 或名字}
else: {"type": "unknown", "data": name}
```

四类里有一条清晰的降级链：**可序列化的 `Runnable` 给真 id → 不可序列化的仍给一个「未实现」占位 id → schema 类给 JSON Schema（若 `with_schemas`）或名字 → 其余归 unknown**。而 `_IgnoreUnserializable` 这个 schema 生成器参数是为了让「schema 里有无法序列化的字段」不导致整个导出失败。

`metadata` 最后被无条件附上（`if node.metadata is not None: json["metadata"] = node.metadata`）。这与 `get_graph` 默认实现里 `metadata=config.get("metadata") if config else None` 是配套的：**图节点携带的是「构造这个图时那份 config 的 metadata」**，于是导出的 JSON 里能看出「这个图是在什么配置下导出的」。

## 关键取舍

**源码事实：** `Graph` 只有两个字段（`nodes: dict[str, Node]`、`edges: list[Edge]`）；`Node` / `Edge` / `Branch` 都是 `NamedTuple`，边存的是节点 **id** 而非 `Node` 对象。

**源码事实：** `Branch.condition` 的类型是 `Callable[..., str]`（返回分支标签而非布尔），且 `Branch` 不进 `edges`——所以 `RunnableBranch` 导出的图里看不到条件。

**源码事实：** `get_graph` 的默认实现给每个叶子画「输入 schema → 组件 → 输出 schema」三节点哑铃，schema 生成失败时用 `create_model_v2` 造空模型占位。

**源码事实：** 序列拼接用「首步不裁首、末步不裁末、中间两头裁」+ 「上一个末节点连到下一个首节点」；`trim_first_node` / `trim_last_node` 只在「删掉后仍有首/末节点」且「该节点只有一条相关边」时才真的删除。

**源码事实：** `Graph.extend` 只在子图不是「全部 uuid id」时才给节点加 `prefix:` 前缀，于是纯 uuid 子图可无冲突合并。

**源码事实：** `reid()` 只重命名 uuid 类 id（同名加 `_1`/`_2` 序号），返回新 `Graph`；`node_data_str` 对以 `Runnable` 开头的名字砍掉前 8 个字符。

**源码事实：** Mermaid 渲染按 id 里的冒号切子图、按「源与目标同位置的相同前缀」分组边；同名子图会 `ValueError`；标签转义先处理 `#` 再处理其它字符；节点 id 用 `\` + 十六进制码点转义，与标签的 HTML 实体转义是两套规则。

**源码事实：** 首/末节点用 `([...])` 体育场形并带 `:::first` / `:::last` 类；条件边是 `-.->` 虚线、普通边是 `-->` 实线；长标签按 `wrap_label_n_words`（默认 9）插 `&nbsp<br>&nbsp`。

**源码事实：** `MermaidDrawMethod` 只有 `PYPPETEER` 与 `API` 两成员，**默认 `API`**（把整张图 base64 编码后发到 `https://mermaid.ink`）；源码内有防自动扫描器的注释解释 `base_url` 不构成 SSRF 汇点的前提。

**源码事实：** API 路径的 402/其它非 5xx 立刻失败、5xx 才重试，退避是 `retry_delay * 2^attempt * (0.5 + 0.5*random())`；错误消息给出三步排查建议，第三步是「改用 Pyppeteer 本地渲染」。

**源码事实：** Pyppeteer 路径仍需从 CDN 加载 mermaid.js，因此「本地渲染」并不完全离线；写文件走 `run_in_executor`。

**源码事实：** `graph_ascii` 改编自 DVC 的 `dagascii.py`，依赖可选包 `grandalf`；`VertexViewer.HEIGHT = 3`、宽度为 `len(name) + 2`；布局取 `graph.C[0]`（只画第一个连通分量）；画布尺寸由顶点盒四角与边路径点的整体包围盒算出。

**源码事实：** `Graph.draw_ascii` 只把 `{id: name}` 与边传给渲染器（丢弃节点数据与 schema）；`draw_png` 支持用 `LabelsDict` 覆盖默认标签；`to_json` 把 uuid 换成 `enumerate` 序号以便重复导出得到相同结果。

**架构推论：** 「每个 `Runnable` 自己导出子图、组合器只负责拼」是唯一能让图与执行逻辑保持同步的做法。若反过来由某个中心化组件知道每种组合器的内部结构，那么新增一个组合器就必然需要同时改那个中心——而它与组合器不在同一个文件里，几乎一定会漏。

**架构推论：** 「裁剪安全节点」这条判据（删掉后仍有首/末且只有一条边）是图变换的通用要求：任何结构简化的前提都是「简化不破坏拓扑性质」。序列拼接依赖它来避免把「多出边节点」裁掉。

**架构推论：** 默认走网络 API 是一个用便利换隐私的选择。它把「画图」这件小事变成一次外部请求，而请求内容（base64 后的整张图）包含节点名与 metadata。正确的默认值应当是不联网的那一个——这也是为什么错误消息里把「改用本地渲染」列为第三步补偿。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/runnables/graph.py` | `Graph` | 两个字段的图容器：`nodes` 字典 + `edges` 列表，含拼接/裁剪/重命名/四种导出 |
| `libs/core/langchain_core/runnables/graph.py` | `Node` | 节点：id、name、data（schema 或 `Runnable`）、metadata |
| `libs/core/langchain_core/runnables/graph.py` | `Edge` | 边：source / target 存 id、可选 label、是否条件边 |
| `libs/core/langchain_core/runnables/graph.py` | `Branch` | 条件边的附属描述：`Callable[..., str]` 的条件 + 分支名到结束节点 |
| `libs/core/langchain_core/runnables/graph.py` | `Graph.extend` / `trim_first_node` / `trim_last_node` | 子图合并（按需加前缀）与安全裁剪 |
| `libs/core/langchain_core/runnables/graph.py` | `Graph.reid` / `node_data_str` / `node_data_json` | 人可读 id 生成与节点数据的四种分类导出 |
| `libs/core/langchain_core/runnables/graph_mermaid.py` | `MermaidDrawMethod` | 两种位图渲染方式：`pyppeteer` 与 `api`（默认后者） |
| `libs/core/langchain_core/runnables/graph_mermaid.py` | `draw_mermaid` / `draw_mermaid_png` | 文本渲染（子图分组、两套转义、四种边样式）与位图渲染（两条可选依赖路径） |
| `libs/core/langchain_core/runnables/graph_ascii.py` | `Graph`（`grandalf.graphs.Graph`） | ASCII 渲染用外部库的 DAG 类型，配 `SugiyamaLayout` 布局 |
| `libs/core/langchain_core/runnables/graph_ascii.py` | `draw_ascii` / `_build_sugiyama_layout` | 布局、包围盒与画布尺寸计算 |

## 参考

- [langchain 源码：runnables/graph.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/graph.py)
- [langchain 源码：runnables/graph_mermaid.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/graph_mermaid.py)
- [Mermaid 流程图语法](https://mermaid.js.org/syntax/flowchart.html)
- [grandalf（Sugiyama 布局库）](https://github.com/bdcht/grandalf)

## 与主核 / AI 的关系

「把执行结构导出成图」在主核里对应的是**查询计划可视化**（`EXPLAIN` 的图形化形态）：把算子树/图导出来，让用户看出「这条 SQL 到底怎么跑」。这里有三条经验。

**第一，图的真相来源必须分散在各组件里，拼接则由组合器负责。** `get_graph` 是每个 `Runnable` 自己的责任，序列与并行只做拼。主核里若让某个中心模块知道所有算子的内部结构，那个模块会成为巨大的耦合点，且新增算子时必然被遗忘。

**第二，「裁剪」必须带拓扑安全判据。** `trim_first_node` 要求「删掉后仍有首节点」且「只有一条出边」。没有这两条，序列拼接会把一个扇出节点的第一个分支删掉，产生一张看起来正常但语义错误的图。主核里做「计划简化」时同理：任何节点消除都要先证明它不影响连通性与语义。

**第三，默认不联网。** 这里 `draw_mermaid_png` 默认把整张图发到外部服务，而图里含节点名与 metadata——在含敏感表名或路径的场景下，这就是一次数据外泄。主核若做「计划图上传」功能，默认值应当是本地产出，联网必须是显式选择。

另有一条更实用的：**文本渲染优先**。`draw_mermaid` 与 `draw_ascii` 都不依赖外部工具，只有 `draw_png` 需要 `pygraphviz` / `pyppeteer`。把最常用的输出做成「纯字符串拼接」意味着「看计划形状」不需要安装任何东西——而依赖越少的功能，被用到的概率越高。
