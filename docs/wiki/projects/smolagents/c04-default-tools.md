---
project: smolagents
feature: default-tools
title: 内置工具清单
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/default_tools.py
    symbols: [PythonInterpreterTool, FinalAnswerTool, WebSearchTool]
  - path: src/smolagents/default_tools.py
    symbols: [VisitWebpageTool, UserInputTool, ApiWebSearchTool]
---

# 内置工具清单

## 一句话职责

内置工具是框架随包附带的几个现成 `Tool` 实现，它们的作用与其说是「提供能力」，不如说是**把外部依赖的边界画清楚**：哪些是纯本地计算，哪些需要联网、需要 API key、需要额外 pip 包，各自在什么时机才付出这份代价。

## 机制是什么

清单可分成四类，分类依据不是领域，而是**依赖的形状**：

- **零依赖本地工具**：`FinalAnswerTool`（原样返回答案）、`UserInputTool`（阻塞读标准输入）、`PythonInterpreterTool`（委托给本地受控解释器）。
- **可选依赖的联网工具**：`DuckDuckGoSearchTool`（`ddgs`）、`WebSearchTool`（抓 HTML/RSS，只要 `requests`）、`VisitWebpageTool`（`requests` + `markdownify`）、`WikipediaSearchTool`（`wikipedia-api`）、`ApiWebSearchTool`（`requests` + 环境变量里的 key）。
- **需要模型与 GPU 的工具**：`SpeechToTextTool`，`PipelineTool` 的子类，要 `transformers` + `torch` + `accelerate`。
- **映射表**：`TOOL_MAPPING` 只收了三个（`python_interpreter` / `web_search` / `visit_webpage`），供 `Agent(add_base_tools=True)` 使用。

注意 `TOOL_MAPPING` 的键是**工具名**而非类名，且它是「名字 → 类」的字典——因为 `add_base_tools` 的语义是「按名字装一批」，而不是「装这个类」。

## 怎么实现的

**依赖延迟到构造或调用，且错误文案带可执行的修复指令。** 这是这批工具最一致的模式：

- `DuckDuckGoSearchTool.__init__` 里 `from ddgs import DDGS`，`ImportError` 时抛「你必须安装 `ddgs`：例如 `pip install ddgs`」。
- `VisitWebpageTool.forward` 里才 import `requests` 与 `markdownify`，`ImportError` 时把两个包名都写进文案。
- `WikipediaSearchTool.__init__` import `wikipediaapi`，缺包时给 `pip install wikipedia-api`（注意包名与模块名不同，文案用的是 pip 名）。
- `SpeechToTextTool` 通过 `PipelineTool.__init__` 的 `_is_package_available("accelerate")` / `("torch")` 检查，缺了提示 `pip install 'smolagents[transformers]'`。

独有的一处是 `ApiWebSearchTool`：它不需要额外 pip 包，但需要 `BRAVE_API_KEY`（或自定义 `api_key_name`），`self.api_key = api_key or os.getenv(self.api_key_name)`——**缺失时不在构造期报错**，而是在 `forward` 里由 `requests` 收到 401/403 后 `raise_for_status()` 抛出。这是一处不一致：其他工具在构造期就把缺失依赖说清楚，这个把问题推迟到了第一次调用。

**`PythonInterpreterTool` 是唯一带作用域语义的内置工具。** 它的 `forward` 里每次都新建 `state = {}`，然后调 `evaluate_python_code(code, state=state, static_tools=BASE_PYTHON_TOOLS, authorized_imports=..., timeout_seconds=...)`，返回值是 `(result, is_final_answer)` 的**第一个元素**（注释点明了第二元素是什么），最后拼成 `Stdout:\n{state['_print_outputs']}\nOutput: {output}`。

两个细节值得记：

- **状态不保留**。`state = {}` 每次新建，所以上一次调用的变量在这次不可见。它在 `inputs["code"]` 的 description 里明确写了这一点：「这段代码里用到的所有变量都必须在这同一段里定义，否则会报错」。这是刻意的——这个工具给 `ToolCallingAgent` 用（它没有共享解释器），而不是给 `CodeAgent` 用。
- **授权导入被写进描述**。`__init__` 里算出 `self.authorized_imports`（`BASE_BUILTIN_MODULES` 与用户传入的并集），然后把这份清单**嵌进 `inputs["code"]["description"]`**，于是模型在提示词里就能看到「这段代码只能导入这些库」。这是把能力边界同时表达成「运行时限制」与「提示词文本」的一个例子。

**`FinalAnswerTool` 是纯通道。** `name = "final_answer"`，`inputs = {"answer": {"type": "any", ...}}`，`output_type = "any"`，`forward` 就是 `return answer`。它之所以存在，是因为代码式智能体需要一个**能被调用的形式**来声明「我完成了」——本地解释器会给它包一层抛 `FinalAnswerException`（见 b01）。所以这个工具的实际价值在「提供一个约定的名字与签名」，而非逻辑。

`Agent._setup_tools` 里 `self.tools.setdefault("final_answer", FinalAnswerTool())` 这句说明了它的地位：**永远存在，且用户提供的同名工具优先**（`setdefault` 的语义）。

**`UserInputTool` 直接阻断在标准输入上。** `forward` 里 `input(f"{question} => Type your answer here:")`。这在交互式 notebook 里可用，在服务和批处理里会挂住或者读到 EOF。它没有任何超时或非交互降级——这既是简单，也是一个真实限制。

**联网工具的三种抓取策略，写法各不相同。**

- `WebSearchTool` 有 `engine` 参数（`duckduckgo` / `bing` / `exa`）。duckduckgo 分支去抓 `https://lite.duckduckgo.com/lite/` 并用一个内联的 `HTMLParser` 子类（`SimpleResultParser`）解析——它按 CSS class（`result-link` / `result-snippet` / `link-text`）捕获字段，在 `</tr>` 时把「title+description+link 齐全」的结果收进列表。bing 分支改抓 `format=rss` 的 XML 用 `ElementTree` 解析（比解析 HTML 稳）。exa 分支走官方 API，key 从 `EXA_API_KEY` 读，缺 key 抛 `ValueError`。
- `ApiWebSearchTool` 默认打 Brave 的 `/res/v1/web/search`，`extract_results` 从 `data["web"]["results"]` 里取 `title` / `url` / `description`，`format_markdown` 输出带序号的 markdown 列表，空结果返回固定文案（不抛异常）。
- `VisitWebpageTool` 用 `markdownify` 把 HTML 转 markdown，再用 `re.sub(r"\n{3,}", "\n\n", ...)` 压掉多余空行，最后按 `max_output_length`（默认 40000 字符）截断并附一句「内容已被截断」的说明。它的 `forward` 把三类异常各自转成**字符串返回**而不是抛出：超时返回「请求超时，请稍后再试」、`RequestException` 返回错误串、其他异常返回「发生了意外错误」。这是刻意的：网页抓取失败是常态，让模型看到文本比让整步崩掉更有用。

**限流用同一个模式重复了两次。** `DuckDuckGoSearchTool` 与 `ApiWebSearchTool` 都有 `rate_limit`（默认 1.0 次/秒）与 `_enforce_rate_limit`：算 `_min_interval = 1.0 / rate_limit`，`time.sleep` 补足间隔，然后更新 `_last_request_time`。`rate_limit` 为假值时直接返回。这是「客户端礼貌」而非「服务端配额」——单实例有效，多实例或跨进程无效。

**`SpeechToTextTool` 展示 `PipelineTool` 的用法。** 它设 `default_checkpoint = "openai/whisper-large-v3-turbo"`，用 `__new__` 在实例创建前就把 `pre_processor_class` / `model_class` 设到**类**上（因为 `setup` 里读的是 `self.pre_processor_class`，而这两个类需要按需 import transformers）。`encode` 先把参数包成 `AgentAudio` 再 `.to_raw()`，`forward` 调 `model.generate`，`decode` 用 `batch_decode(..., skip_special_tokens=True)[0]`。

## 关键取舍

**源码事实：** 依赖检查分散在「构造期」（`ddgs`、`wikipedia-api`、`torch`/`accelerate`）与「调用期」（`requests`、`markdownify`）两处，`ApiWebSearchTool` 的 API key 则完全没有前置检查。

**源码事实：** `PythonInterpreterTool` 每次调用新建 `state`，变量不跨调用保留；授权导入清单被写进 `inputs` 的描述文本，模型可见。

**源码事实：** `final_answer` 由 `Agent._setup_tools` 用 `setdefault` 兜底加入，用户同名工具优先。

**源码事实：** `VisitWebpageTool` 把超时与请求失败转成字符串返回，不抛异常；`DuckDuckGoSearchTool` 与 `WebSearchTool` 在零结果时抛 `Exception`。

**源码事实：** `TOOL_MAPPING` 只含三个工具（解释器、DuckDuckGo 搜索、访问网页），且键是工具名。

**架构推论：** 这批工具的共同价值不是「能力」，而是**示范依赖管理**。每个工具的 import 位置都经过考虑：装一次就贵的（模型）在 `setup`；只在一个方法里用的（`requests`）放方法内；需要早期失败的（`ddgs`）放构造。这套分层没有统一抽象，靠约定维持——也就意味着新增内置工具时容易走样（`ApiWebSearchTool` 的 key 检查就是走样的例子）。

**架构推论：** 「零结果抛异常」与「失败返回字符串」两种策略并存，反映了两类失败的性质不同：搜索零结果意味着「你的查询有问题」，让模型看到异常文本能促使它改查询；网页抓取失败往往与查询无关（网络、站点的反爬），返回文本让模型换个 URL 继续。这个区别没有写成文档，但从实现能读出来。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/default_tools.py` | `PythonInterpreterTool` | 委托本地解释器执行代码，每次新建状态，授权导入写进描述 |
| `src/smolagents/default_tools.py` | `FinalAnswerTool` | 纯通道工具，提供 `final_answer` 的名字与签名 |
| `src/smolagents/default_tools.py` | `UserInputTool` | 阻塞读标准输入的交互工具 |
| `src/smolagents/default_tools.py` | `DuckDuckGoSearchTool` | `ddgs` 搜索 + 客户端限流，零结果抛异常 |
| `src/smolagents/default_tools.py` | `GoogleSearchTool` | serpapi / 其他 provider 搜索，含 `filter_year` 可空参数 |
| `src/smolagents/default_tools.py` | `ApiWebSearchTool` | Brave API 搜索，key 缺失推迟到调用期暴露 |
| `src/smolagents/default_tools.py` | `WebSearchTool` | 三引擎搜索：DDG 抓 HTML、Bing 读 RSS、Exa 走 API |
| `src/smolagents/default_tools.py` | `VisitWebpageTool` | 网页转 markdown + 截断，三类异常转成文本返回 |
| `src/smolagents/default_tools.py` | `WikipediaSearchTool` | `wikipedia-api` 检索，要求显式 user_agent |
| `src/smolagents/default_tools.py` | `SpeechToTextTool` | `PipelineTool` 子类，Whisper 语音转文字 |
| `src/smolagents/default_tools.py` | `TOOL_MAPPING` | `add_base_tools` 使用的「名字 → 类」映射，仅三项 |

## 参考

- [smolagents 源码：default_tools.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/default_tools.py)
- [smolagents 文档：默认工具](https://huggingface.co/docs/smolagents/tutorials/tools#default-tools)
- [smolagents 文档：工具参考](https://huggingface.co/docs/smolagents/reference/tools)
- [Wikimedia 用户代理政策](https://foundation.wikimedia.org/wiki/Policy:Wikimedia_Foundation_User-Agent_Policy)

## 与主核 / AI 的关系

这批工具在架构上的位置，相当于主核里的**内置函数与扩展模块**：都是「随系统附带的、开箱可用的能力」，且都要回答「依赖了什么、什么时候检查」。主核的扩展函数在 `CREATE EXTENSION` 时就绑定，缺失的共享库立刻报错；这里的检查点分散在构造与调用两处，因为 Python 的导入成本差异很大（import torch 可能几秒，import requests 只要几十毫秒）。这个差异不是随意的：**检查时机应当与被检查物的获取成本成反比**。

`PythonInterpreterTool` 的「每次新建 state」与 `CodeAgent` 的「保留 state」形成对照，这恰好对应主核里会话级临时表与普通表的关系：前者随会话消失，后者持久。选择哪种取决于使用方是否需要在多次调用间累积工作——`ToolCallingAgent` 的模型每次只给一次调用，累积无从谈起。

`VisitWebpageTool` 的「把异常转成字符串」值得单独对照主核的错误处理。数据库不会把「表不存在」变成一个正常结果返回，因为那会让错误不可区分；但在 agent 场景里，**把失败变成可读文本正是正确的**——模型是唯一能对失败做出反应的执行者，它需要的不是异常类型而是「发生了什么、下次该怎么试」。这是「错误处理策略取决于谁来处理错误」的一个清晰例证。

对 AI 工程的实际意义在于：内置工具是最容易被低估的一环。真实项目里它们几乎总会被替换或扩展，因此它们的价值主要在三个方面——示范依赖该怎么延迟、示范失败该怎么表达、示范能力边界该怎么同时写进运行时与提示词（`PythonInterpreterTool` 把授权导入嵌进描述这一点，是最值得抄的细节）。
