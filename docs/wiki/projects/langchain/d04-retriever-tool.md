---
project: langchain
feature: retriever-tool
title: 检索器与渲染
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/retrievers.py
    symbols: [BaseRetriever, RetrieverInput]
  - path: libs/core/langchain_core/tools/render.py
    symbols: [render_text_description]
---

# 检索器与渲染

## 一句话职责

`BaseRetriever` 把「查文档」这件事统一成 `str → list[Document]` 的 Runnable 契约，并用一套签名字节探测让「旧式没有 `run_manager` 参数」「只实现了同步版本」这两种存量实现也能正常工作；`render_text_description` 则是把工具集渲染成纯文本，供那些只接受字符串提示的模型使用。

## 机制是什么

这页有两块内容，它们解决的是同一类问题的两端：**如何让能力被不同形态的消费者使用**。

第一块是检索器的**接口收窄**。检索器的输入输出类型被钉死成两个别名：

```python
RetrieverInput = str
RetrieverOutput = list[Document]
RetrieverLike = Runnable[RetrieverInput, RetrieverOutput]
RetrieverOutputLike = Runnable[Any, RetrieverOutput]
```

即「输入是字符串查询、输出是文档列表」。这个收窄让所有检索器（向量库、BM25、Web 搜索…）都能互换——调用方不必知道具体是哪种。

但它带来一个兼容问题：这个基类已经存在多年，早期实现的方法签名不带 `run_manager` 参数。要支持它们，就必须**在运行时不假定签名形状**。

第二块是**渲染**。有些模型（尤其是早期的补全模型）只有「一段文本进、一段文本出」，没有结构化的函数调用能力。它们要通过文本了解有哪些工具可用，所以需要把 `BaseTool` 列表降级成人类可读的文本。

## 怎么实现的

**`__init_subclass__` 在类创建时探测签名，并自动补一个异步垫片。**

```python
@override
def __init_subclass__(cls, **kwargs: Any) -> None:
    super().__init_subclass__(**kwargs)
    parameters = _signature_parameters(cls._get_relevant_documents)
    cls._new_arg_supported = parameters.get("run_manager") is not None
    if (
        not cls._new_arg_supported
        and cls._aget_relevant_documents == BaseRetriever._aget_relevant_documents
    ):
        # we need to tolerate no run_manager in _aget_relevant_documents signature
        async def _aget_relevant_documents(self: Self, query: str) -> list[Document]:
            return await run_in_executor(None, self._get_relevant_documents, query)
        cls._aget_relevant_documents = _aget_relevant_documents

    # If a V1 retriever broke the interface and expects additional arguments
    cls._expects_other_args = (
        len(set(parameters.keys()) - {"self", "query", "run_manager"}) > 0
    )
```

这段做了三件事，每一件都在处理一类历史包袱：

1. **`_new_arg_supported = parameters.get("run_manager") is not None`**：探测子类的 `_get_relevant_documents` 是否接受 `run_manager`。这是一个**逐类计算一次的布尔位**，之后每次调用都靠它决定怎么传参。

2. **自动补异步实现**。条件是「同步版本不接受 `run_manager`」且「子类没有覆写异步版本」（`cls._aget_relevant_documents == BaseRetriever._aget_relevant_documents` 这个比较是**判断是否仍是基类方法**——即未覆写）。

   补出来的垫片只做一件事：把同步方法丢进线程池。注释解释了为什么不能复用基类的异步默认实现：「we need to tolerate no `run_manager` in `_aget_relevant_documents` signature」——基类的异步默认实现会传 `run_manager`，而那类旧实现不接受它，会抛 `TypeError`。

   注意垫片里对 `_get_relevant_documents` 的调用**没有传 `run_manager`**，并带 `# type: ignore[call-arg]`——因为那个省略正是这段代码的意图。

3. **`_expects_other_args`**：判断子类的签名里是否有 `self`/`query`/`run_manager` 之外的参数。含义是「这个实现的接口越界了，它期望额外参数」。注释直说「If a V1 retriever broke the interface and expects additional arguments」。

**这两条探测结果直接决定了 `invoke` 里的三次分派。**

```python
kwargs_ = kwargs if self._expects_other_args else {}
if self._new_arg_supported:
    result = self._get_relevant_documents(input, run_manager=run_manager, **kwargs_)
else:
    result = self._get_relevant_documents(input, **kwargs_)
```

三种组合：

| `_new_arg_supported` | `_expects_other_args` | 行为 |
|---|---|---|
| `True` | `False` | 传 `run_manager`，丢弃所有额外 kwargs |
| `True` | `True` | 传 `run_manager` + 额外 kwargs |
| `False` | 任意 | 不传 `run_manager`，kwargs 视 `_expects_other_args` 而定 |

关键在 `kwargs_ = kwargs if self._expects_other_args else {}` 这一行：**默认丢弃所有调用方传的额外关键字参数**。这个方向是保守的——若把 `kwargs` 无条件传下去，一个标准签名的实现会收到它不认识的参数并抛 `TypeError`。所以「只有明确探测到它会接受额外参数时才传」。

这个设计有一个副作用值得记：**同一个 `invoke(**kwargs)` 调用，在不同检索器上行为不同**。对一个标准检索器，多传的参数被静默吞掉；对一个越界的老检索器，它们会被接住。所以 kwargs 的语义取决于子类——这类不一致在源码里只能靠这两行分派维持。

**回调生命周期是手写的三段式，而不是复用 Runnable 的默认流程。**

```python
run_manager = callback_manager.on_retriever_start(None, input, name=..., run_id=kwargs.pop("run_id", None))
try:
    ... # 调用实现
except Exception as e:
    run_manager.on_retriever_error(e)
    raise
else:
    run_manager.on_retriever_end(result)
    return result
```

四处：

1. **`on_retriever_start(None, input, ...)`**：第一个参数是 `Serialized`，传 `None`——即这个事件不携带序列化后的对象信息。这与 LLM 事件不同（那里通常带序列化信息），因为检索器往往没有需要序列化的标识。
2. **`run_id=kwargs.pop("run_id", None)`**：用 `pop` 而不是 `get`。所以 `run_id` **从 kwargs 里被移走**——它不该再传给检索器实现。这是一个「先取走自己用的，剩下的留给下游」的处理。
3. **`except` 里先 `on_retriever_error(e)` 再 `raise`**。顺序不可反：先通知回调，再让异常继续传播。若反了，异常会先炸掉调用栈，回调就来不及记录。
4. **`else` 分支而非 `try` 末尾**：`on_retriever_end` 只在成功时调用。这是 Python 的 try/except/else 语义——`else` 里的异常不被同一个 `except` 捕获。所以若 `on_retriever_end` 自己抛错，不会被误报成「检索器出错」。

`ainvoke` 是完全对称的版本，只是把每一步都加上 `await`：

```python
run_manager = await callback_manager.on_retriever_start(None, input, ...)
try:
    ...  # await self._aget_relevant_documents(...)
except Exception as e:
    await run_manager.on_retriever_error(e)
    raise
else:
    await run_manager.on_retriever_end(result)
    return result
```

**两处都相同的一个细节：`on_retriever_end` 只传 `result`**，不给它 `run_id` 或其他信息。所以这次追踪的「结束」事件完全靠 `run_manager` 记住自己属于哪个 run。

**`_get_ls_params` 做了一次名字规范化，为的是让追踪记录里的名字干净。**

```python
def _get_ls_params(self, **_kwargs: Any) -> LangSmithRetrieverParams:
    default_retriever_name = self.get_name()
    if default_retriever_name.startswith("Retriever"):
        default_retriever_name = default_retriever_name[9:]
    elif default_retriever_name.endswith("Retriever"):
        default_retriever_name = default_retriever_name[:-9]
    default_retriever_name = default_retriever_name.lower()
    return LangSmithRetrieverParams(ls_retriever_name=default_retriever_name)
```

三处：

1. **`[9:]` 与 `[:-9]`**：`"Retriever"` 恰好 9 个字符。所以 `"RetrieverFoo"` → `"Foo"`，`"FooRetriever"` → `"Foo"`。用字面长度做切片，不写常量——耦合但直接。
2. **`elif` 而非两个独立 `if`**：一个以 `Retriever` 开头又以 `Retriever` 结尾的名字（如 `"RetrieverRetriever"`）只会被剥一次。若两个都做，会剥成空串。
3. **`startswith` 优先于 `endswith`**：名字若是 `"Retriever"` 本身（正好 9 字符），`startswith` 成立，切片后是空串；`endswith` 也成立但会被 `elif` 跳过。这是一个边界情况，结果为空名字。

这个规范化的目的是让 `ls_retriever_name` 成为「检索器的语义名」而不是类名——`FooRetriever` 与 `RetrieverFoo` 都归类为 `foo`。这让追踪平台上的聚合有意义。

注意 `LangSmithRetrieverParams` 是一个 `total=False` 的 TypedDict，除 `ls_retriever_name` 外还有 `ls_vector_store_provider`、`ls_embedding_provider`、`ls_embedding_model` 三个可选键——这些是子类可以补充的信息（向量库提供商、嵌入提供商与模型），基类只填名字。

**`inheritable_metadata` 用了两次字典合并，合并顺序决定了谁赢。**

```python
inheritable_metadata = {
    **(config.get("metadata") or {}),
    **self._get_ls_params(**kwargs),
}
```

先展开调用方给的 metadata，再展开 `_get_ls_params` 的产出。**后者覆盖前者**——即框架算出的 `ls_retriever_name` 不能被调用方通过 config 覆盖。这个顺序是刻意的：它保证了追踪字段的可信度。

而 `CallbackManager.configure(...)` 的参数把这个 metadata 分成两档：

```python
callback_manager = CallbackManager.configure(
    config.get("callbacks"),
    None,
    verbose=kwargs.get("verbose", False),
    inheritable_tags=config.get("tags"),
    local_tags=self.tags,
    inheritable_metadata=inheritable_metadata,
    local_metadata=self.metadata,
)
```

`inheritable_*` 会传给子 run，`local_*` 只属于本次运行。检索器自己的 `tags`/`metadata` 是 local（只属于这一层），而来自 config 的是 inheritable（会向下传播）。

**`render_text_description` 的渲染是「有签名用签名、没签名用名字」，而这两条路径差别不小。**

```python
def render_text_description(tools: list[BaseTool]) -> str:
    descriptions = []
    for tool in tools:
        if hasattr(tool, "func") and tool.func:
            sig = signature(tool.func)
            description = f"{tool.name}{sig} - {tool.description}"
        else:
            description = f"{tool.name} - {tool.description}"
        descriptions.append(description)
    return "\n".join(descriptions)
```

三处：

1. **`hasattr(tool, "func") and tool.func`**：这要求工具**对象上有 `func` 属性且非空**。`StructuredTool` 与 `Tool` 都有 `func` 字段，所以通常成立；但若一个工具只有 `coroutine`（纯异步实现），`func` 是 `None`，就走无签名分支。
2. **`signature(tool.func)` 渲染成 `(query: str)` 这样的形状**，直接拼在工具名后面。所以输出形如 `search(query: str) - 这是搜索工具`。这一步让模型**能看到参数名与类型**——即使没有结构化的函数调用支持。
3. **没有签名时降级成 `name - description`**，即丢掉参数信息。这是一个真实的损失：纯异步工具在这个渲染函数下面，模型看不到参数。

文档给的输出示例是 `search: This tool is used for search`——注意示例用的是**冒号**分隔，而实现用的是 **` - ` 分隔**。文档与实现不一致。这是值得指出的：照文档写提示词的人会得到与预期不同的格式。

`render_text_description_and_args` 是加参数版本：

```python
tool_strings = []
for tool in tools:
    args_schema = str(tool.args)
    ...
    tool_strings.append(f"{description}, args: {args_schema}")
return "\n".join(tool_strings)
```

它多做了两件事：**渲染 `tool.args`**（`BaseTool.args` 返回参数的 JSON Schema 字典，`str()` 一下变成 Python 字典字面量），并把它追加成 `, args: {...}`。

注意 `str(dict)` 的产物是 Python 字面量语法（`{'query': {'type': 'string'}}`）而不是 JSON（`{"query": {"type": "string"}}`）。文档示例里写的是 JSON 双引号形式——**又一处文档与实现不一致**。

这个差别不只是美观：单引号形式不是合法 JSON，模型若尝试按 JSON 复制可能出错。不过对「给模型看的说明文字」而言，Python 字典语法也足够可读。

**这两个渲染函数没有处理 `tool.args` 为空的情形。**

若一个工具没有 `args_schema` 且是 `StructuredTool`，`tool.args` 会是 `{'properties': {}, ...}` 之类的产物——渲染成 `args: {'properties': {}}`，对模型无意义。而 `Tool` 的 `args` 是硬编码的 `{"tool_input": {"type": "string"}}`（d03 讲过），所以单输入工具在文本渲染里会显示一个名为 `tool_input` 的字符串参数——这个名字不在任何函数签名里，却会出现在提示里。

**检索器与工具之间的关系是「包装」而不是「继承」。**

`tools/retriever.py` 里的 `create_retriever_tool` 是这条关系的实体：它接收一个 `BaseRetriever`，返回一个 `StructuredTool`。而它的输入 schema 是**另一个** `RetrieverInput`——一个 Pydantic 模型：

```python
class RetrieverInput(BaseModel):
    query: str = Field(description="query to look up in retriever")
```

注意这里有一个**同名不同物**的撞名：`retrievers.py` 里 `RetrieverInput = str`（类型别名），`tools/retriever.py` 里 `RetrieverInput` 是一个 `BaseModel` 类。两者语义相反——一个是「输入就是字符串」，另一个是「输入是带 `query` 字段的对象」。之所以要后者，是因为工具的参数必须是对象（d02 讲过 `Runnable` 必须 object schema 才能当工具）。

包装过程本身很短：

```python
def func(query: str, callbacks: Callbacks = None) -> str | tuple[str, list[Document]]:
    docs = retriever.invoke(query, config={"callbacks": callbacks})
    content = document_separator.join(format_document(doc, document_prompt_) for doc in docs)
    if response_format == "content_and_artifact":
        return (content, docs)
    return content
```

四处：

1. **`callbacks: Callbacks = None`** 是显式声明的参数。这正是 d03 讲的「`_run` 只在签名里有 `callbacks` 时才注入」——这里声明了，所以回调会被注入并传给检索器。
2. **`document_separator` 默认 `"\n\n"`**：把多条文档拼成一段文本。这个默认值让文档之间有清晰的分隔。
3. **`document_prompt` 默认 `PromptTemplate.from_template("{page_content}")`**：即只取正文。可以替换成包含元数据的模板（如「来源：{source}\n{page_content}」），这就是「渲染」的自由度所在。
4. **`response_format == "content_and_artifact"` 时返回二元组**：内容是拼好的字符串，artifact 是原始文档列表。这个选项让上层能同时拿到「给模型看的文本」与「给程序用的结构化数据」——这正是 g01 那种「内容与元数据分离」的模式。

注意 artifact 里的文档**没有被渲染**，是原始的 `list[Document]`。所以模型只看文本，而后续代码能拿到完整对象。

**渲染的两条路径对应两种模型能力。**

| 模型能力 | 用哪个 |
|---|---|
| 支持函数调用 / 工具协议 | 结构化 schema（d02 的 `ToolDescription`） |
| 只支持文本进文本出 | `render_text_description` 塞进系统提示 |

所以 `render_text_description` 不是「过时的兼容代码」，而是**给另一类模型的正路**。它的存在解释了为什么 `BaseTool` 需要 `description` 字段且文档强调「Used to tell the model how/when/why to use the tool. You can provide few-shot examples as a part of the description」——对文本渲染路径来说，`description` 是模型唯一的决策依据。

## 关键取舍

**源码事实：** `RetrieverInput = str` 与 `RetrieverOutput = list[Document]` 把检索器契约钉死成「字符串进、文档列表出」，使不同检索实现可以互换。

**源码事实：** `__init_subclass__` 在类创建时探测 `_get_relevant_documents` 的签名，算出 `_new_arg_supported`（是否接受 `run_manager`）与 `_expects_other_args`（是否有额外参数），并在「不接受 `run_manager` 且未覆写异步版」时自动补一个走线程池的异步垫片。

**源码事实：** 自动生成的异步垫片调 `_get_relevant_documents` 时不传 `run_manager`（带 `# type: ignore[call-arg]`），因为省略正是它的意图——基类的异步默认实现会传，而那类旧实现会抛 `TypeError`。

**源码事实：** `invoke`/`ainvoke` 用 `kwargs_ = kwargs if self._expects_other_args else {}` 决定是否把额外 kwargs 下传，默认丢弃；`run_id` 用 `pop` 取出后不再传给实现。

**源码事实：** 回调是手写三段式：`on_retriever_start` → `try` 调用 / `except` 里 `on_retriever_error(e)` 后再 `raise` / `else` 里 `on_retriever_end(result)`。用 `else` 保证「结束通知自己抛错」不会被误当成「检索器出错」。

**源码事实：** `_get_ls_params` 从类名里剥掉前缀或后缀 `"Retriever"`（9 字符）再转小写，且用 `elif` 保证只剥一次。

**源码事实：** `inheritable_metadata` 先展开 `config` 里的 metadata 再展开 `_get_ls_params` 的结果，因此框架算出的追踪字段覆盖调用方提供的同名键。

**源码事实：** `render_text_description` 在工具对象有非空 `func` 时才渲染签名，否则降级成 `name - description`（丢失参数信息）；文档示例的格式（冒号分隔）与实现（` - ` 分隔）不一致。

**源码事实：** `render_text_description_and_args` 用 `str(tool.args)` 渲染参数，产出的是 Python 字典字面量（单引号）而非 JSON。

**源码事实：** `tools/retriever.py` 的 `create_retriever_tool` 用**同名的另一个** `RetrieverInput`（Pydantic 模型，含 `query` 字段）当工具参数 schema，因为工具参数必须是对象；`response_format="content_and_artifact"` 时返回 `(渲染文本, 原始文档列表)`。

**架构推论：** `__init_subclass__` 这套「类创建时探测签名」的手法，本质是**把运行期的鸭子类型判断前移到导入期**。代价是每个子类创建时都做一次反射（`_signature_parameters`），收益是之后每次调用只需读一个布尔位。这是一处很典型的「一次性成本换常态成本」。

**架构推论：** `_expects_other_args` 默认丢弃 kwargs 的方向是对的，但它的副作用值得警惕：同一个 `**kwargs` 在不同检索器上语义不同。更干净的做法是让所有检索器都接受 `**kwargs` 并自行决定是否使用，但那会破坏「旧实现不该被改」的前提。所以这里的选择是**用不一致换取不改存量代码**。

**架构推论：** `RetrieverInput` 的撞名（`str` 别名 vs `BaseModel` 类）是一个真实的认知债。它揭示了一个跨层的事实：**检索器契约里输入是字符串，而工具契约里输入必须是对象**。要在两者之间转换，就必须有一个「把字符串包成 `query` 字段」的适配模型。撞名让这个转换看起来像同一个概念，实际是两个。

**架构推论：** `render_text_description` 的签名渲染路径依赖 `signature(tool.func)`——这是**从实现反推对模型呈现的信息**，而不是从 `args_schema` 推导。这在方向上是可疑的：`args_schema` 才是权威的参数定义（它可能被显式改写、可能有别名或默认值），而函数签名只是它的来源之一。所以对「`args_schema` 与 `func` 签名不一致」的工具，两种渲染函数会给出不同信息。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/retrievers.py` | `RetrieverInput` / `RetrieverOutput` | `str` 与 `list[Document]` 的类型别名，钉死检索器契约 |
| `libs/core/langchain_core/retrievers.py` | `BaseRetriever` | 检索器抽象基类，持 `invoke`/`ainvoke` 的完整回调生命周期 |
| `libs/core/langchain_core/retrievers.py` | `BaseRetriever.__init_subclass__` | 创建子类时探测签名、补异步垫片、算两个分派布尔位 |
| `libs/core/langchain_core/retrievers.py` | `BaseRetriever._get_ls_params` | 类名规范化成追踪用的语义名（剥前后缀 `"Retriever"`） |
| `libs/core/langchain_core/retrievers.py` | `BaseRetriever.invoke` / `ainvoke` | 三段式回调 + 按布尔位分派 + 默认丢弃额外 kwargs |
| `libs/core/langchain_core/retrievers.py` | `LangSmithRetrieverParams` | `total=False` 的追踪字段集合 |
| `libs/core/langchain_core/tools/render.py` | `render_text_description` | 工具渲染成纯文本（有 `func` 时带签名） |
| `libs/core/langchain_core/tools/render.py` | `render_text_description_and_args` | 追加 `args: {...}`（Python 字面量形式） |
| `libs/core/langchain_core/tools/render.py` | `ToolsRenderer` | 渲染函数的类型别名 |
| `libs/core/langchain_core/tools/retriever.py` | `create_retriever_tool` | 把检索器包成 `StructuredTool`，含 `query` 字段的 `RetrieverInput` |

## 参考

- [langchain 源码：retrievers.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/retrievers.py)
- [langchain 源码：tools/render.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/tools/render.py)
- [langchain 文档：检索器](https://docs.langchain.com/oss/python/langchain/retrieval)
- [langchain 文档：如何把检索器变成工具](https://python.langchain.com/docs/how_to/tools_retrieval/)

## 与主核 / AI 的关系

「类创建时探测签名」与主核里的**插件入口点扫描**同源。主核在加载扩展时读插件声明的能力表，据此决定调用哪些钩子；本页读的是方法的签名，据此决定传哪些参数。两者的共同点是**用一次性探查换来常态调用的正确性**，也都把不一致处理成了「探测结果」而不是「运行期 try/except」。

「默认丢弃额外参数」与主核里的**参数裁剪**一致：主核在某些兼容模式下会忽略客户端传的多余设置项，而不是报错。判据相同——**兼容优先于严格**，但代价是「用户以为传了生效了」的静默失效。本页的 `_expects_other_args` 至少把这个静默变成了可探测的（`True` 时才传）。

「同一份数据两种表示（结构化 vs 文本）」在主核里对应的是**机器可读与人类可读的两种输出格式**（如 `EXPLAIN` 的计划树与 JSON 格式）。主核需要后者给工具消费、前者给人看；本页需要结构化 schema 给函数调用、文本给补全模型。判据相同——**消费方的能力决定表示的形态**，而不是「选一个更通用」的。

对 AI 工程的实际意义在于：接口一旦发布就会被大量实现继承，此后任何加参数的需求都只能靠探测而不是强制。这份实现给出的三条经验可直接用：**用类创建时的反射算出兼容位而不是每次调用都试错**、**默认不把未知参数下传**（否则旧实现会集体抛 `TypeError`）、以及**文本渲染与结构化渲染要同时维护**（前者不是遗留代码，而是另一类模型的唯一入口）。
