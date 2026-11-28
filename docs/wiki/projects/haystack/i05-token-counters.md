---
project: haystack
feature: token-counters
title: token 计数与评测结果聚合
page_type: feature
schema_version: 1
refs:
  - path: haystack/token_counters/tiktoken_counter.py
    symbols: [TiktokenCounter]
  - path: haystack/token_counters/approximate_counter.py
    symbols: [ApproximateTokenCounter]
  - path: haystack/token_counters/utils.py
    symbols: [_rendered_conversation, _rendered_tools]
  - path: haystack/evaluation/eval_run_result.py
    symbols: [EvaluationRunResult]
---

# token 计数与评测结果聚合

## 一句话职责

两件都在「把结构化数据变成一个数」：前者把一串消息（含工具调用、图像、文件）**渲染成一段可测量的纯文本**再数 token，三个实现沿「精度 ↔ 成本」排成一条轴（字符比估算 → 本地真实编码 → 调 provider 的计数 API）；后者把「若干个评测器的逐条分数」对齐成一张表，并按位置或按指定列把两次运行的结果并排比较。

## 机制是什么

**计数侧**有三层：

1. `TokenCounter`（一个 `Protocol`，在 `token_counters/types/protocol.py`）——只要求 `count(messages, tools=None) -> int` 与 `to_dict`，而 `from_dict` 有默认实现（`default_from_dict`）。docstring 说明了它的契约：「实现 `to_dict` 让计数器的设置能挺过序列化。默认的 `from_dict` 把它们原样传回构造函数，这对简单值足够；当 `to_dict` 发出的是必须先重建的东西（例如 `Secret` 或嵌套组件）时覆盖它」。

2. **三个实现**，精度与成本沿一条轴排列：`ApproximateTokenCounter`（`len(text) / chars_per_token`，默认 4.0）→ `TiktokenCounter`（`tiktoken` 的字节对编码，默认 `o200k_base`）→ `OpenAITokenCounter`（调 OpenAI 的输入 token 计数 API）。

3. 一层共享的渲染工具：`_rendered_conversation` / `_rendered_tools` / `_non_text_tokens` / `_non_text_placeholder`。

前两个实现的 `count` 结构完全一样：

```python
if not messages and not tools: return 0
text_tokens = <把 _rendered_conversation(messages) + _rendered_tools(tools) 数成 token>
return text_tokens + _non_text_tokens(messages, tokens_per_image=..., tokens_per_file=...)
```

所以「渲染后计数的文字部分」与「按件计价的非文字部分」是两笔相加，而**唯一不同的只有中间那一行**。第三个实现（`OpenAITokenCounter`）连这个骨架都不套——它把整批输入交给 provider 去数。

**聚合侧**只有一个类 `EvaluationRunResult`，三个报告方法（`aggregated_report` / `detailed_report` / `comparative_detailed_report`）与三种输出格式（`json` / `csv` / `df`）。

## 怎么实现的

**「渲染」而不是「序列化」是计数侧最关键的设计判断，而理由在 `_render_message` 的 docstring 里。**

它说的是「渲染成一行或多行纯文本」，而**不是**序列化成 JSON。为什么？因为 token 计数要模拟的是「provider 实际看到的东西」——而 provider 看到的既不是 Python 对象也不是严格 JSON，而是一段按某种模板拼起来的文本。所以这里用 `[role] content` 这种人类可读的标记来近似。

`_render_message` 有四个分支：

**第一支：工具结果消息单独处理。**

```python
if results := message.tool_call_results:
    return "\n".join(
        f"[tool:{result.origin.tool_name}"
        f"{' id=' + result.origin.id if include_tool_call_ids and result.origin.id else ''}"
        f"{' (error)' if result.error else ''}] "
        f"{_tool_result_text(result.result, placeholder=placeholder)}"
        for result in results
    )
```

所以一条工具结果消息渲染成 `[tool:name] 结果` 或 `[tool:name id=xxx (error)] 结果`。注释解释了为什么要单独一支：「一条工具结果消息只携带 `tool_call_results`，所以它被单独渲染并用产生它的工具名标注」。

**第二支：文本。**

```python
if texts := message.texts:
    lines.append(f"[{role}] " + "\n".join(texts))
```

注意 `message.texts` 是复数——一条消息可能有多个文本块，它们被换行连接后挂在同一个 `[role]` 前缀下面。

**第三支：工具调用。**

```python
for call in message.tool_calls:
    arguments = json.dumps(call.arguments, default=str, sort_keys=True)
    call_id = f" id={call.id}" if include_tool_call_ids and call.id else ""
    lines.append(f"[{role} -> tool_call{call_id}] {call.tool_name}({arguments})")
```

`sort_keys=True` 让参数序列化确定（同样的字典总得到同样的串），`default=str` 让非 JSON 原生类型不至于抛错。

**第四支：图像与文件。**

```python
non_text: list[ChatMessageContentT] = [*message.images, *message.files]
for content in non_text:
    lines.append(f"[{role}] {placeholder(content)}")
```

注释点明了必要性：「图像与文件也消耗 token，所以它们需要一个替身而不是被跳过」。

最后一行的兜底也值得记：

```python
return "\n".join(lines) if lines else f"[{role}] <no content>"
```

**一条什么都没带的消息也会产出一行**（`[role] <no content>`），而不是空字符串。所以它至少贡献角色标记的 token——这与真实 provider 的行为一致（每条消息都有 role 包装）。

**`reasoning` 内容被刻意排除。** `_render_message` 的 docstring：「推理内容被**故意**排除：provider 在轮次之间丢弃它，所以它不是被测量上下文的一部分」。

这一条很实在：若把 reasoning 算进 token，那计数会系统性偏高，而偏高的方向恰好导致「你以为快超限了其实没有」——把一个优化动作（裁剪上下文）变成了过度裁剪。

**`placeholder` 是可注入的，而默认值的选择有理由。**

```python
_PlaceholderFn = Callable[[ChatMessageContentT], str]

def _non_text_placeholder(content: ChatMessageContentT) -> str:
    if isinstance(content, ImageContent): return "<image>"
    if isinstance(content, FileContent): return f"<file: {content.filename or 'unnamed'}>"
    return f"<{type(content).__name__}>"
```

三支：图像给 `<image>`，文件给 `<file: 名字>`（无名时 `unnamed`），其他类型给 `<类型名>`。

而 docstring 解释了为什么它是参数：「默认值短且稳定，这正是计数器需要的——因为**替身自身的长度就是被测量的东西**。一个为让模型阅读而渲染的调用方可以传一个描述内容的替身」。

这是这一层最精确的一处设计：**同一个渲染函数服务两种完全不同的目的**，而两种目的对「替身该多长」的要求相反。计数器要短（否则撑大计数），给模型读的要详尽（否则模型看不懂）。所以把替身做成参数，而不是为两种用途写两份渲染逻辑。

**`_rendered_tools` 把工具 schema 当 JSON 块渲染。**

```python
if not tools: return ""
return json.dumps([tool.tool_spec for tool in flatten_tools_or_toolsets(tools)], default=str, sort_keys=True)
```

注意它先 `flatten_tools_or_toolsets` ——所以 `Toolset` 会被摊平。而 docstring 说这是「替身，代表 provider 在消息之外一起发的东西」——因为真实的 provider 确实会把工具 schema 单独发一段。

`sort_keys=True` 同样是为了确定性：同一组工具不管 dict 插入顺序如何，渲染出的字符串一样，计数就稳定。

**`_non_text_tokens` 要钻进工具结果里找图像与文件。**

```python
for message in messages:
    images += len(message.images)
    files += len(message.files)
    for result in message.tool_call_results:
        if isinstance(result.result, str): continue
        images += sum(isinstance(block, ImageContent) for block in result.result)
        files += sum(isinstance(block, FileContent) for block in result.result)
```

所以两条路径：消息自己带的 `images` / `files`，以及**工具返回结果里嵌的**块。注释说明了后者：「一个工具返回的图像与文件嵌在工具结果里面」。

`sum(isinstance(...) for block in ...)` 利用了布尔是整数——数出 True 的个数。而 `if isinstance(result.result, str): continue` 是必需的：字符串没有块可遍历。

**两个计数器的差别只有一行，但它们对「结果的可信度」的态度不同。**

`TiktokenCounter` 的类 docstring 主动列出了两条限制：

- 「**它是纯文本的**，所以图像与文件得到的是扁平的 `tokens_per_image` / `tokens_per_file` 估算而不是真实计数。」
- 「**它是 OpenAI 的编码器**。其他 provider 的分词方式不同，所以预期计数在它们身上会漂移。」

再加一条关于特殊 token：「消息或工具内容里的字面特殊 token 串（例如 `<|endoftext|>`）被当作普通文本编码，所以它们被计数而不是抛错」。

第三条对应实现里的 `self._encoder.encode_ordinary(...)` ——用的是 `encode_ordinary` 而不是 `encode`。`tiktoken` 的 `encode` 遇到未在 `allowed_special` 里的特殊 token 串会抛错，而 `encode_ordinary` 把一切当普通文本。这个选择避免了「用户消息里恰好含 `<|endoftext|>` 就崩」这种荒谬情况。

`TiktokenCounter` 的编码器是**懒加载**的：

```python
def warm_up(self) -> None:
    if self._encoder is not None: return
    self._encoder = tiktoken.get_encoding(self.encoding)
```

docstring 说明它「加载编码器，若词表未缓存则下载它」。所以第一次 `count` 可能有网络开销——而 `warm_up` 可以被外部提前调用（这与 `Tool` / `Hook` 的生命周期约定一致）。而幂等守卫是必须的：docstring 里没写「必须幂等」，但 `if self._encoder is not None: return` 自己实现了。

`ApproximateTokenCounter` 的校验只有一条：`chars_per_token <= 0` 报错。而它没有任何懒加载——纯算术。

**第三个实现 `OpenAITokenCounter` 走的是一条完全不同的路：它不发本地计数，而是把输入发给 OpenAI 的 API 数。**

类 docstring 把差别说明白了：「不同于本地 token 计数器，这个计数器把输入发给 OpenAI 的 `POST /v1/responses/input_tokens` 端点。返回的计数**包含针对消息与工具 schema 的模型专属格式化，以及受支持的非文本内容如图像与文件**」。

所以三者的取舍是一条清晰的三角：

| 实现 | 精度 | 成本 | 适用范围 |
|---|---|---|---|
| `ApproximateTokenCounter` | 粗（字符比） | 零（纯算术） | 任意 provider，只要不要求精确 |
| `TiktokenCounter` | 中（真实编码，但无消息模板开销） | 一次词表加载（可能下载） | 任意 provider，误差可接受 |
| `OpenAITokenCounter` | 最高（含 provider 侧模板与非文本内容） | **每次 `count` 一次 HTTP 调用** | 仅 OpenAI，且要求精确 |

`ApproximateTokenCounter` 的误差来自「4 字符 ≈ 1 token」这个比例——对英文大致成立，对中文与代码会明显偏离。而 `TiktokenCounter` 的误差来自它**只数渲染出的文本**：真实 provider 还会加消息边界标记、工具 schema 包装等开销，那部分它完全不知道。`OpenAITokenCounter` 之所以最准，正是因为它把「模板开销」也交给了 provider 自己算。

而 `OpenAITokenCounter` 的实现细节也印证了这个定位——它**不复用渲染工具**，而是走 responses API 的格式转换：

```python
openai_input: list[dict[str, Any]] = []
for message in messages:
    openai_input.extend(_convert_chat_message_to_responses_api_format(message=message))

request: dict[str, Any] = {"model": self.model, "input": openai_input}
if tools:
    request["tools"] = [{"type": "function", **tool.tool_spec} for tool in flatten_tools_or_toolsets(tools=tools)]

response = client.responses.input_tokens.count(**request)
return response.input_tokens
```

所以三个计数器里，前两个共享渲染层（因为它们都要自己「猜」provider 看到的文本），第三个完全绕过它（因为 provider 知道得比任何本地渲染都准）。**这是「精度越接近 provider，本地渲染的作用越小」的直接体现**。

代价也很实在：它的 `warm_up` 要建一个 `OpenAI` 客户端（含 `api_key` 解析、超时、重试、HTTP 客户端配置），`close` 要释放它，而 `count` 的延迟取决于网络。所以它适合「在一次运行前算一次预算」，不适合放进热循环。

注意它的 `count` 里那句防御：

```python
client = self.client
if client is None:
    raise RuntimeError("The OpenAI client was not initialized.")
```

`warm_up()` 里已经建过客户端，所以理论上不会为 `None`。这句是给类型检查器用的（`self.client: OpenAI | None`），但它顺带也是一个诚实的断言：如果 `warm_up` 被重写成不建客户端，这里会立刻炸而不是让 `None.count(...)` 抛一个更难读的 `AttributeError`。

**聚合侧的构造期检查是「四条交叉一致性」。**

```python
self.inputs = deepcopy(inputs)
self.results = deepcopy(results)

if len(inputs) == 0: raise ValueError("No inputs provided.")
if len({len(lst) for lst in inputs.values()}) != 1: raise ValueError("Lengths of the inputs should be the same.")
expected_len = len(next(iter(inputs.values())))

for metric, outputs in results.items():
    if "score" not in outputs: raise ValueError(f"Aggregate score missing for {metric}.")
    if "individual_scores" not in outputs: raise ValueError(f"Individual scores missing for {metric}.")
    if len(outputs["individual_scores"]) != expected_len:
        raise ValueError(f"Length of individual scores for '{metric}' should be the same as the inputs. Got {len(outputs['individual_scores'])} but expected {expected_len}.")
```

四道：输入非空、所有输入列表等长、每个指标都有 `score` 与 `individual_scores`、逐条分数长度等于输入行数。

**第四道是这一页的核心不变量**：它保证「每个指标的第 i 个分数对应第 i 行输入」。没有这条，后面所有报告都会错位——而错位的表看起来仍然正常（数字都在，只是配错了行）。

而 `deepcopy` 两处是防御外部修改：创建的 `EvaluationRunResult` 是快照，之后调用方改原字典不影响它。

**`detailed_report` 里有一段「类型一致性」的处理，值得单独看。**

```python
combined_data = {col: self.inputs[col] for col in self.inputs}

scores_columns = list(self.results.keys())
for col in scores_columns:
    col_values = self.results[col]["individual_scores"]
    if any(isinstance(v, float) for v in col_values):
        col_values = [float(v) for v in col_values]
    combined_data[col] = col_values
```

逻辑是：**若这一列里任何一个值是 float，就把整列都转成 float**。

为什么要这样做？因为 i02 那页的指标里，`single_hit` 的逐条分数是整数形式的 `float` 或 `int`，而 mixed 类型列写进 CSV / DataFrame 时会被推断成 object 列，排序和比较都会出问题。统一成 float 让整列有确定类型。

而 `any(isinstance(v, float) for v in col_values)` 的判据是「有任何一个是 float」而不是「全是 float」——所以一个 `[1, 0, 0.5]` 的列会被全部转成 `[1.0, 0.0, 0.5]`。这正确：它保证了类型统一。

但它有个缺口：**若整列都是 int 而 `nan` 出现**（`float("nan")` 是 float，所以会被 `any` 命中），那没问题。而若整列都是 int 且没有 float，则保持 int——这也对。所以这个判据实际上覆盖了要紧的情况。

**`comparative_detailed_report` 是这一页最复杂的方法，而它的复杂度全在「两次运行的行怎么配对」。**

两种配对不对等：

**按位置配对**（`_positional_rows`）：

```python
rows = [(i, i) for i in range(min(len_a, len_b))]
rows.extend((i, None) for i in range(len_b, len_a))
rows.extend((None, j) for j in range(len_a, len_b))
```

所以「共同的前 min 行按位配对，多出来的记为 `None`」。而长度不等时**先记一条警告**，且警告文案给出了修法：「传 `align_on`，用一个能标识每行的列来配对」。

**按列配对**（`_aligned_rows`）：

```python
keys_a = _row_keys(self.inputs[align_on])
keys_b = _row_keys(other.inputs[align_on])
position_b = {key: j for j, key in enumerate(keys_b)}
keys_a_set = set(keys_a)

rows = [(i, position_b.get(key)) for i, key in enumerate(keys_a)]
only_b = [j for j, key in enumerate(keys_b) if key not in keys_a_set]
rows.extend((None, j) for j in only_b)
```

而 `_row_keys` 解决了一个真实的问题：

```python
def _row_keys(values: list[Any]) -> list[tuple[Any, int]]:
    """Build a key for each row from its value and how many times that value appeared before."""
    seen: Counter = Counter()
    keys = []
    for value in values:
        keys.append((value, seen[value]))
        seen[value] += 1
    return keys
```

所以每一行的键是 `(值, 该值第几次出现)`。这让**重复值按出现顺序配对**——若两个运行都问了同一个问题两次，第一次对第一次、第二次对第二次，而不是都配到第一个。

没有这个机制的话，用 `{value: index}` 建映射会让重复值全部指向最后一次出现，配对完全错乱。

而 `_aligned_rows` 还有一层 `try/except TypeError`：

```python
except TypeError as error:
    raise ValueError(f"The values of the '{align_on}' column can't be used to match rows. Use a column with hashable values, such as strings or numbers.") from error
```

因为 `Counter` / `dict` / `set` 都要求值可哈希。若用户传了一个「值本身是列表」的列（例如检索到的文档列表），就会在这里得到一条带修法的错误。

**配对后的列名规则有三条，而它们都为了「不丢信息」。**

第一条：两个运行同名时改名。

```python
if name_a == name_b:
    name_a, name_b = f"{self.run_name}_first", f"{other.run_name}_second"
    logger.warning(...)
```

注释：「保留两个运行的列，而不是让第二个运行覆盖第一个」。若不改名，`combined_results[f"{name_a}_{key}"]` 与 `combined_results[f"{name_b}_{key}"]` 会撞同一个键——后写的覆盖先写的，于是第一个运行的数据静默消失。

第二条：共享输入列只出现一次。

```python
if key in ignore:
    values_b = detailed_b.get(key)
    combined_results[key] = [values[i] if i is not None else (values_b[j] if values_b is not None and j is not None else None) for i, j in rows]
```

注释：「共享输入列只显示一次；只存在于另一个运行的行取它的值」。所以「问题文本」这类两边相同的列不会重复成 `run_a_question` / `run_b_question`，而是合成一列——且只存在于 B 的行用 B 的值填。

第三条：非共享列加运行名前缀。

```python
else:
    combined_results[f"{name_a}_{key}"] = [values[i] if i is not None else None for i, _ in rows]
...
for key, values in detailed_b.items():
    if key not in ignore:
        combined_results[f"{name_b}_{key}"] = [values[j] if j is not None else None for _, j in rows]
```

所以分数列一定带前缀（`runA_score` / `runB_score`），而输入列默认都进 `ignore`（`keep_columns is None` 时 `ignore = list(self.inputs.keys())`，即全部输入列都不加前缀）。

`keep_columns` 的语义是反过来的：**它列出「要在名字里保留（即当作分数列处理）的列」**。所以传 `keep_columns=["question"]` 会让 `question` 变成 `runA_question` / `runB_question`。这个参数名的意图是「这些列我关心两次运行的差异」，但读起来容易与「只保留这些列」混淆。

**`_handle_output` 是三种格式的统一出口，而它的错误处理哲学不一致。**

```python
if output_format == "json": return data
if output_format == "df":
    pandas_import.check()
    return DataFrame(data)
if output_format == "csv":
    if not csv_file: raise ValueError("A file path must be provided in 'csv_file' parameter to save the CSV output.")
    return EvaluationRunResult._write_to_csv(csv_file, data)
raise ValueError(f"Invalid output format '{output_format}' provided. Choose from 'json', 'csv', or 'df'.")
```

`json` / `df` 失败就抛（`pandas_import.check()` 会在没装 pandas 时抛带安装提示的错误）；`csv` 则交给 `_write_to_csv` ——而那个函数**把所有异常转成字符串返回**：

```python
except PermissionError: return f"Error: Permission denied when writing to {csv_file}"
except OSError as e: return f"Error writing to {csv_file}: {str(e)}"
except Exception as e: return f"Error: {str(e)}"
```

所以三种格式的失败方式不同：两种抛异常，一种返回错误字符串。而返回类型标注是 `Union[str, "DataFrame", dict[str, list[Any]]]` ——**调用方无法从类型上区分「成功写的消息」与「错误消息」，两者都是 `str`**。

这是一个真实的接口瑕疵：`aggregated_report(output_format="csv")` 成功时返回 `"Data successfully written to x.csv"`，失败时返回 `"Error: ..."`。要判断成功与否只能做字符串前缀匹配。相比之下，`json` 与 `df` 分支的失败是异常，调用方可以用 `try/except` 统一处理。**同一组三种格式，错误处理应该统一**。

而 `_write_to_csv` 里那三层捕获的顺序也是对的：`PermissionError` 是 `OSError` 的子类，所以必须先捕它（否则会被第二个分支吞掉，消息变成笼统的「写文件出错」而不是明确的「权限被拒」）。

## 关键取舍

**源码事实：** 前两个计数器共享 `_rendered_conversation` + `_rendered_tools` + `_non_text_tokens`，`count` 只差「文本部分怎么数」这一行。

**源码事实：** `OpenAITokenCounter` **完全不复用渲染层**，而是把消息转成 responses API 格式后发给 `POST /v1/responses/input_tokens`，由 provider 算出含模板开销与非文本内容的精确值——每次 `count` 一次 HTTP 调用。

**源码事实：** `TokenCounter` 是 `Protocol` 而非基类，只要求 `count` 与 `to_dict`；`from_dict` 有默认实现（`default_from_dict`），docstring 说明「当 `to_dict` 发出的是必须先重建的东西（`Secret`、嵌套组件）时才覆盖它」。

**源码事实：** `OpenAITokenCounter.count` 在建过客户端之后仍断言 `client is not None` 并抛 `RuntimeError`——一半给类型检查器，一半是「若 `warm_up` 被改成不建客户端就立刻炸」的诚实断言。

**源码事实：** 消息被**渲染成带角色标记的纯文本**（`[role] content`）而不是序列化成 JSON，因为要模拟 provider 实际看到的形状。

**源码事实：** `reasoning` 内容被**故意排除**在渲染之外，因为 provider 在轮次间丢弃它。

**源码事实：** `placeholder` 是可注入的回调，因为「计数用」需要短替身而「给模型读」需要详尽替身——两种需求相反。

**源码事实：** `TiktokenCounter` 用 `encode_ordinary` 而不是 `encode`，所以字面特殊 token 串被当普通文本而不是抛错。

**源码事实：** `TiktokenCounter.warm_up` 懒加载且幂等；`ApproximateTokenCounter` 只校验 `chars_per_token > 0`。

**源码事实：** `_non_text_tokens` 会钻进工具结果里找嵌套的图像与文件块，因为工具返回的附件也在消息里。

**源码事实：** `EvaluationRunResult.__init__` 用 `deepcopy` 两处，并交叉校验「逐条分数长度 == 输入行数」这条核心不变量。

**源码事实：** `detailed_report` 里「有一个 float 就把整列转 float」，避免 CSV/DataFrame 出现混合类型列。

**源码事实：** `_row_keys` 用「值 + 该值第几次出现」作键，所以重复值按出现顺序配对。

**源码事实：** 两个运行同名时列名加 `_first` / `_second` 后缀，避免第二个运行覆盖第一个。

**源码事实：** `keep_columns` 的语义是「这些列按分数列处理（加运行名前缀）」，而不是「只保留这些列」。

**源码事实：** `csv` 输出失败返回错误字符串，而 `json` / `df` 失败抛异常——同一组三格式的错误处理不统一，调用方无法从类型上区分「成功消息」与「错误消息」。

**架构推论：** 「渲染而不是序列化」这个判断决定了整个计数层的形状。序列化要精确可逆（JSON 保留每个字段），而计数要的是「接近 provider 实际看到的 token 流」——那是一个有损的、带模板的文本。选择渲染之后，一系列具体决定都变得自然：加角色标记、排除 reasoning、非文本内容用短替身。若反过来用 JSON 序列化，计数会系统性偏高（多了一堆引号、括号、字段名）。

**架构推论：** 三个计数器构成一条完整的「精度 ↔ 成本」轴，而它们的存在说明**没有一种计数方式在三种意义上都对**（准、快、跨 provider）。`ApproximateTokenCounter` 快且跨 provider 但粗；`TiktokenCounter` 准一些但只对 OpenAI 编码准、对其他 provider 漂移；`OpenAITokenCounter` 最准但绑 OpenAI 且每次要一次网络调用。关键在于**本地渲染层存在的理由正是「你无法问 provider」**——一旦能问，那层就是多余的。这也解释了为什么第三个实现完全绕开了 `_rendered_conversation`。

**架构推论：** 「替身长度就是被测量的东西」这句 docstring 揭示了计数类工具的一个固有偏差源。任何「用占位符代替不可测量内容」的做法，都在用「占位符自身的长度」冒充「真实内容的长度」。所以这类估算的正确性从来不是绝对的，而是「**相对于你设定的参数**」——这也是为什么 `tokens_per_image` / `tokens_per_file` 是必须显式提供的参数，而不是硬编码的常量。

**架构推论：** `_row_keys` 那个「值 + 出现次数」的复合键，是「对重复值做有序配对」的标准解法。任何「按某列 join 两个数据集」的实现都会遇到重复值，而朴素的「值 → 索引」映射会把所有重复值指向最后一个（或第一个）。加上出现次数之后，配对变成确定的且保序。判据很清楚：**要配对的列是否可能重复？** 会的话，键里必须带序号。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/token_counters/tiktoken_counter.py` | `TiktokenCounter` | 真实编码器计数，docstring 自陈两条限制 |
| `haystack/token_counters/tiktoken_counter.py` | `TiktokenCounter.warm_up` | 懒加载编码器，幂等 |
| `haystack/token_counters/tiktoken_counter.py` | `TiktokenCounter.count` | `encode_ordinary` 加非文本估算 |
| `haystack/token_counters/approximate_counter.py` | `ApproximateTokenCounter` | 字符比例估算，只校验比例为正 |
| `haystack/token_counters/openai_counter.py` | `OpenAITokenCounter` | 走 provider 计数 API，最准但每次一次 HTTP |
| `haystack/token_counters/openai_counter.py` | `OpenAITokenCounter.close` | 释放 OpenAI 客户端与底层 HTTP 资源 |
| `haystack/token_counters/types/protocol.py` | `TokenCounter` | `Protocol`：只要求 `count` 与 `to_dict` |
| `haystack/token_counters/__init__.py` | `_import_structure` | 三个计数器按需懒加载，避免顶层拖入重依赖 |
| `haystack/token_counters/utils.py` | `_rendered_conversation` | 整段对话渲染成一块纯文本 |
| `haystack/token_counters/utils.py` | `_rendered_tools` | 工具 schema 渲染成 JSON 块 |
| `haystack/token_counters/utils.py` | `_render_message` | 四分支渲染，故意排除 reasoning |
| `haystack/token_counters/utils.py` | `_non_text_placeholder` | 三种非文本内容的短替身 |
| `haystack/token_counters/utils.py` | `_non_text_tokens` | 按件计价，含工具结果里嵌套的块 |
| `haystack/token_counters/utils.py` | `_tool_result_text` | 工具结果拼成单串，非文本部分用替身 |
| `haystack/evaluation/eval_run_result.py` | `EvaluationRunResult` | 输入与逐条分数的对齐容器 |
| `haystack/evaluation/eval_run_result.py` | `EvaluationRunResult.__init__` | `deepcopy` 与四道交叉一致性校验 |
| `haystack/evaluation/eval_run_result.py` | `aggregated_report` | 每个指标一个汇总分数的两列表 |
| `haystack/evaluation/eval_run_result.py` | `detailed_report` | 输入列加分数列，float 类型统一 |
| `haystack/evaluation/eval_run_result.py` | `comparative_detailed_report` | 两次运行的并排比较 |
| `haystack/evaluation/eval_run_result.py` | `_positional_rows` | 按位置配对，长度不等时告警 |
| `haystack/evaluation/eval_run_result.py` | `_aligned_rows` | 按指定列配对，含不可哈希值的错误提示 |
| `haystack/evaluation/eval_run_result.py` | `_row_keys` | 「值 + 出现次数」的复合键 |
| `haystack/evaluation/eval_run_result.py` | `_handle_output` | 三格式出口，错误处理不统一 |
| `haystack/evaluation/eval_run_result.py` | `_write_to_csv` | 三层异常捕获转成消息字符串 |

## 参考

- [haystack 源码：token_counters/utils.py](https://github.com/deepset-ai/haystack/blob/main/haystack/token_counters/utils.py)
- [haystack 源码：evaluation/eval_run_result.py](https://github.com/deepset-ai/haystack/blob/main/haystack/evaluation/eval_run_result.py)
- [haystack 文档：token 计数](https://docs.haystack.deepset.ai/docs/token-counters)
- [tiktoken：OpenAI 的字节对编码器](https://github.com/openai/tiktoken)

## 与主核 / AI 的关系

主核里最接近的是「行大小估算（row size estimate）」：一行数据的字节数不精确等于各列宽度之和（有对齐、有空值位图、有变长偏移），所以优化器用一个带常数项的估算公式。这一页的 token 计数就是同一类问题——**精确测量不可能或太贵，所以用「结构化的部分精确算 + 难算的部分按件估算」**。

差别在于**估算误差的方向与用途**。主核的行大小估算偏大是安全的（多分配一点内存）；token 计数的偏差没有安全方向——偏大会导致过度裁剪上下文，偏小会导致超限报错。所以这一页的 docstring 花了大量篇幅说明「哪里会漂移」，而不是只给一个数字。

与本组 i02（检索指标）的连接最直接：`EvaluationRunResult` 消费的 `individual_scores` 正是那一页（以及 i01）的输出。所以 `__init__` 里那条「逐条分数长度 == 输入行数」的校验，实际上是在这一层守住一个跨组件的契约——任何违反它的评测器都会在这里被挡住而不是产出一张错位的表。

与 i01（LLM 评测器）的 `nan` 约定呼应：`detailed_report` 里那个「有 float 就整列转 float」的处理，正是为了让含 `nan` 的列与不含的列都能安全进 CSV / DataFrame。若不做统一，`nan` 与整数混在一列会让 pandas 推断成 object。

对 AI 工程最实用的一条是「数 token 要渲染成 provider 看到的形状」。直接用 `len(json.dumps(messages))` 估算会系统性偏高（多出大量结构字符），而直接用 `sum(len(m.content))` 会偏低（漏掉角色标记与工具 schema）。渲染成 `[role] content` 这种近似形状，误差最小且方向可控。

另一条是「选计数器要按用途而不是按精度」。本地 `TiktokenCounter` 的误差来自「不知道 provider 的消息模板开销」，所以它适合**同 provider 内的相对比较**（这个 prompt 比那个长多少）——比例误差会部分抵消。而 `OpenAITokenCounter` 适合**绝对阈值判断**（还剩多少预算、会不会超限），因为只有它知道模板开销。用近似值做绝对判断，误差会直接变成「明明没超却报错」或「悄悄超了」。

再一条是「非文本内容必须显式定价」。图像、文件、音频在文本 tokenizer 下无法测量，所以任何计数都只能给一个固定件数。这意味着**这个参数必须由使用者按自己的实际情况设置**——默认值（小图 85、文件 1000）只是一个起点，传大图时不改就会严重低估。

最后一条是 `_row_keys` 的复合键。任何「两次运行结果对比」的功能都会遇到「按某列配对」的需求，而那一列很可能有重复值（同一个问题问两次、同一个文档出现在两行）。用「值 + 出现次数」作键是让配对确定且保序的最简办法，比「加一个显式行号列」对用户更友好。
