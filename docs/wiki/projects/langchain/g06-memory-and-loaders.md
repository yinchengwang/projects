---
project: langchain
feature: memory-and-loaders
title: 会话内存与文档加载
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/document_loaders/base.py
    symbols: [BaseLoader, BaseBlobParser]
  - path: libs/core/langchain_core/document_loaders/blob_loaders.py
    symbols: [BlobLoader, yield_blobs]
  - path: libs/core/langchain_core/runnables/history.py
    symbols: [BaseChatMessageHistory]
---

# 会话内存与文档加载

## 一句话职责

把「内容从哪儿来」与「对话记住什么」都做成**惰性优先**的契约：加载器只要求实现 `lazy_load` 生成器、解析器只要求 `lazy_parse`、blob 加载器只要求 `yield_blobs`；而会话历史则由一个 `Runnable` 绑定层在调用前后自动读写，用 `configurable` 里的键去查对应的历史对象。

## 机制是什么

三个契约层次分明：

- `BlobLoader` 只有一个抽象方法 `yield_blobs() -> Iterator[Blob]`——「从存储系统按某个准则加载原始内容，并把它作为 blob 流**惰性地**返回」。
- `BaseBlobParser` 只有一个抽象方法 `lazy_parse(blob) -> Iterator[Document]`——把 blob 变成文档。
- `BaseLoader` 是 `ABC`，但它**没有一个 `abstractmethod`**（类上带 `# noqa: B024` 正是为此）。它提供 `load` / `aload` / `load_and_split` / `lazy_load` / `alazy_load` 五个方法，靠「子类覆盖了谁」做双向委托。

`BaseChatMessageHistory` 在本页只作为**被查询的目标**出现：`RunnableWithMessageHistory` 从 `config["configurable"]["message_history"]` 取它，再调 `messages` / `add_messages`（异步侧 `aget_messages` / `aadd_messages`）。

## 怎么实现的

**`BaseLoader` 的核心是一条注释写明了「未来会收紧」。** `lazy_load` 上方的注释：

```python
# Attention: This method will be upgraded into an abstractmethod once it's
#            implemented in all the existing subclasses.
```

所以现在 `lazy_load` 不是抽象方法，而是「默认实现 + 报错」：

```python
if type(self).load != BaseLoader.load:
    return iter(self.load())
msg = f"{self.__class__.__name__} does not implement lazy_load()"
raise NotImplementedError(msg)
```

它与 g03 的 `add_texts` 用的是**同一套手法**：比对方法身份判断「子类是否覆盖了另一个方法」。这里比对的是 `type(self).load != BaseLoader.load`——若子类覆盖了 eagem 的 `load`，那 `lazy_load` 就退化成 `iter(self.load())`。

所以「惰性」在这套抽象里是**推荐而非强制**：一个只实现 `load` 的老式加载器仍能用，但它的 `lazy_load` 会先把全部文档装进内存再转成迭代器。而类 docstring 说的是「实现应当用生成器实现惰性加载方法以免一次性把所有文档载入内存」——这是给实现者的指引，不是机器强制的。

**`load` 与 `aload` 都被明确标为「便利方法，不该被覆盖」。** 源码上有两处硬话：

- `load` 上方：`# Sub-classes should not implement this method directly. Instead, they should implement the lazy load method.`（子类不应直接实现此方法，应实现惰性加载方法）
- 类 docstring：`load` 只是为了用户便利而提供，不应被覆盖。

但代码本身允许覆盖（因为 `lazy_load` 的判断正是为了兼容那些覆盖了 `load` 的子类）。所以这是一条**靠约定维持**的规矩：文档说别改，代码给改了的人留了一条退路，同时把退路当成「未实现惰性」的信号。

**`alazy_load` 是唯一一处真正做异步让步的实现。** 它的写法值得逐步看：

```python
iterator = await run_in_executor(None, self.lazy_load)
done = object()
while True:
    doc = await run_in_executor(None, next, iterator, done)
    if doc is done:
        break
    yield doc
```

三步：先在线程池里**构造**迭代器（因为构造可能触发 I/O，例如打开文件），然后每取一条都在线程池里调 `next`，最后用一个哨兵 `object()` 代替 `StopIteration`。

第三步是必需的：`run_in_executor` 无法把 `StopIteration` 传出来（在异步生成器里抛 `StopIteration` 会变成 `RuntimeError`），所以用「传入一个哨兵作为默认值」让 `next` 永不抛异常，靠比较哨兵判断结束。

这是这一页最实用的一处实现细节：**任何「把同步迭代器桥接成异步生成器」的代码都得这么写**，否则会在最后一条数据处炸掉。

**`load_and_split` 带 `danger` 级警告。** docstring 里是 `!!! danger` + 「**不要覆盖这个方法**。**应当被视为已废弃**！」。实现是：若没给 `text_splitter`，先检查 `_HAS_TEXT_SPLITTERS`（模块顶部用 try/except 导入 `RecursiveCharacterTextSplitter` 设的标志），没装就抛 `ImportError` 并给出 `pip install -U langchain-text-splitters`；否则用默认的 `RecursiveCharacterTextSplitter()`；然后 `self.load()` 后 `split_documents`。

`_HAS_TEXT_SPLITTERS` 这个标志存在的原因很直白：`langchain_text_splitters` 是**独立的包**，`langchain_core` 不该硬依赖它。所以默认参数（`RecursiveCharacterTextSplitter`）是一个「可选依赖提供的默认值」——这在 Python 里没有干净的写法，只能用 try/except 加运行时检查。

`TextSplitter` 的类型标注在 `TYPE_CHECKING` 里导入（仅用于注解），而实际使用只在 `load_and_split` 内部。所以「装了就能用、没装也能 import 这个模块」这两件事同时成立。

**类 `ABC` 但无抽象方法这件事被 lint 压制而不是重构。** `class BaseLoader(ABC):  # noqa: B024` —— `B024` 是「ABC 但没有抽象方法」的检查。这里的选择是保留 `ABC` 继承（为了让子类有明确的「这是抽象基类」语义）并压制警告，而不是去掉 `ABC`。这是一个诚实的选择：从形态上它确实不是抽象的（所有方法都有实现），但从**语义**上它是（子类必须覆盖其中一个）。

**`BaseBlobParser.parse` 的定位与 `BaseLoader.load` 完全同构。** 抽象方法是 `lazy_parse`；`parse` 是 `list(self.lazy_parse(blob))`，docstring 写「这是给交互式开发环境用的便利方法。生产应用应当偏向 `lazy_parse`。子类一般不应覆盖这个 parse 方法」。

所以三个契约里两个（loader 与 parser）都是「惰性方法是真接口、急切方法是便利包装」，而 `BlobLoader` 干脆只提供惰性一种——没有 `load_blobs` 这样的急切版本。这个不对称说明 blob 这一层更晚、更克制：它没必要给「一次性拿全部 blob」提供便利，因为 blob 往往是大对象（文件内容），急切拿到内存本身就是危险操作。

**`blob_loaders.py` 只有 38 行，且做了一次再导出。** 模块 doc 说目标：「促进内容加载与内容解析代码的解耦。此外，内容加载代码默认应当提供惰性加载接口」。

而 `Blob` 与 `PathLike` 是从 `langchain_core.documents.base` **再导出**的，带注释「Re-export `Blob` and `PathLike` for backwards compatibility」，并在 `__all__` 里列出。所以这个模块的当前位置（`document_loaders/`）是历史演化的结果——定义在 `documents/base.py`，但对外暴露在加载器命名空间。这类再导出是 Python 生态常见的兼容手段：类的真实位置变了，但旧的 import 路径继续可用。

**`RunnableWithMessageHistory` 是一个「用组合实现的包装器」。** 它继承 `RunnableBindingBase`，但真正的行为几乎全部来自构造函数里拼出来的那条链。`__init__` 做五件事：

1. `warn_deprecated(since="1.3.3", message="RunnableWithMessageHistory is deprecated. Use LangGraph's built-in persistence instead.", removal="2.0.0")` ——**构造即警告**。
2. 造 `history_chain = RunnableLambda(self._enter_history, self._aenter_history).with_config(run_name="load_history")`。
3. 若 `messages_key = history_messages_key or input_messages_key` 存在，则 `history_chain = RunnablePassthrough.assign(**{messages_key: history_chain}).with_config(run_name="insert_history")`。
4. 造 sync/async 两份监听器包装：`runnable.with_listeners(on_end=self._exit_history)` / `with_alisteners(on_end=self._aexit_history)`。
5. 用 `history_chain | RunnableLambda(_call_runnable_sync, _call_runnable_async)` 拼成整条 `bound`，run_name 为 `"RunnableWithMessageHistory"`。

所以整条流水线是：**读历史（并可能插入到输入的某个键）→ 调真正的 runnable → 结束时写历史**。第 3 步的 `RunnablePassthrough.assign` 是「把读到的历史塞进输入字典的某个键」，只有在「输入是 dict 且需要分开的历史键」时才用。

步骤 2 与 4 的分工是关键：读历史是**流水线的一步**（同步执行），写历史是**结束回调**（`on_end`）。这意味着「读」失败会让整个调用失败，而「写」失败不会（回调在 run 结束后执行）。这个不对称在实践中有意义——历史写入出错不该让已经完成的模型调用白费。

**`_enter_history` 与 `_exit_history` 的不对称值得逐行对照。** 读侧：

```python
messages = hist.messages.copy()
if not self.history_messages_key:
    input_val = value if not self.input_messages_key else value[self.input_messages_key]
    messages += self._get_input_messages(input_val)
return messages
```

`hist.messages.copy()` ——先拷贝再追加，不修改原列表。若无 `history_messages_key`（即「历史与本次输入合在一起」模式），就把本次输入的消息追加到历史后面返回。所以「拼接」发生在读侧。

写侧：

```python
inputs = load(run.inputs, allowed_objects="messages")
input_messages = self._get_input_messages(inputs)
if not self.history_messages_key:
    historic_messages = config["configurable"]["message_history"].messages
    input_messages = input_messages[len(historic_messages):]
output_val = load(run.outputs, allowed_objects="messages")
output_messages = self._get_output_messages(output_val)
hist.add_messages(input_messages + output_messages)
```

四步。第 3 步那个切片是这一层的核心技巧：因为读侧把历史**拼在了输入前面**，所以 `run.inputs` 里记录的是「历史 + 本次输入」的完整列表。写侧若直接把它全部写回去，历史会被**复制一遍**。所以用 `len(historic_messages)` 切掉前缀，只保留本次新增的那几条。

注释写得很直白：「若历史消息被前置到了输入消息里，把它们移除以免往历史里加重复消息」。

这个「读侧拼接、写侧切除」的配对是这类自动历史机制的标准解法，但它的脆弱处也很明显：切片依赖「历史长度在调用期间没变」。若在 run 期间有另一个请求往同一会话写了消息，切片的偏移就错了，会漏掉或多写几条。异步版同样有这个窗口（`await hist.aget_messages()` 之后才切）。这是一个**已知未处理**的竞态，它正是「用 LangGraph 的持久化替代」这条废弃建议的现实理由之一。

`load(run.inputs, allowed_objects="messages")` 这个调用也很关键：它在反序列化 run 的输入时**限定只允许还原 messages 类型**（与 g04 的 `load(..., allowed_objects=[Document])` 同一机制，见 i01）。所以调用链里如果有人塞了别的类型，这里会拒绝。

**`_merge_configs` 是「缺键时报带例子的错」。** 它先 `super()._merge_configs(*configs)`，然后算 `missing_keys = set(expected_keys) - set(configurable.keys())`。若缺键且 `_get_parameter_names(self.get_session_history)` 非空，就构造一段包含**可复制粘贴例子**的错误：

```python
msg = (
    f"Missing keys {sorted(missing_keys)} in config['configurable'] "
    f"Expected keys are {sorted(expected_keys)}."
    f"When using via .invoke() or .stream(), pass in a config; "
    f"e.g., chain.invoke({example_input}, {example_config})"
)
```

`example_input = {self.input_messages_key: "foo"}`、`example_configurable = dict.fromkeys(missing_keys, "[your-value-here]")`。所以错误消息里带着一个形如 `chain.invoke({'chat_history': 'foo'}, {'configurable': {'session_id': '[your-value-here]'}})` 的示例。

这是这一层最值得学的一处：**「缺什么」和「怎么补」都写在错误消息里**。用户不需要去查文档就能改对。

注意第三个 `f` 字符串前少了空格（`f"Expected keys are {sorted(expected_keys)}."` 与 `f"When using via ..."` 会拼成 `...keys are [..].When using...`），这是一个小的格式瑕疵：两个句子之间没有空格。功能上无害、可读性上略差。

`missing_keys and parameter_names` 这个双条件也有讲究：只有当工厂函数**有参数**时才报错。若 `get_session_history` 是无参函数，那就不需要 configurable 里的任何键，缺键是合理的。这是一个「按需校验」而不是「一律校验」。

**默认配置规格只有一个 `session_id`。** 若没传 `history_factory_config`，就用：

```python
ConfigurableFieldSpec(id="session_id", annotation=str, name="Session ID", description="Unique identifier for a session.", default="", is_shared=True)
```

`is_shared=True` 意味着这个键在整条链上共享（而不是每个组件各有一份）。`default=""` 让「没给」不立刻报错——报错留给 `_merge_configs` 里那个更友好的检查。

`config_specs` 属性把 `super().config_specs + list(self.history_factory_config)` 经 `get_unique_config_specs` 去重后返回。所以「可配置的键」= 父类的 + 历史工厂声明的，去重。这让 `RunnableWithMessageHistory` 能声明「我需要 `session_id`」这件事被框架识别到（例如 `astream_events` 或配置校验会用到 `config_specs`）。

## 关键取舍

**源码事实：** `BaseLoader` 继承 `ABC` 但没有抽象方法（`# noqa: B024`），实际抽象性由 `lazy_load` 的「未覆盖就报错」实现。

**源码事实：** `lazy_load` 用 `type(self).load != BaseLoader.load` 判断子类是否覆盖了 `load`，与 g03 的 `add_texts` 同一手法。

**源码事实：** `lazy_load` 上有注释「此方法将在所有现有子类实现后被升级为抽象方法」。

**源码事实：** `alazy_load` 用哨兵 `object()` 桥接同步迭代器，因为 `run_in_executor` 无法传出 `StopIteration`。

**源码事实：** `load_and_split` 带 `danger` 警告「不要覆盖、应视为已废弃」，默认 splitter 依赖可选包。

**源码事实：** `BlobLoader` 只提供惰性 `yield_blobs`，没有急切版本。

**源码事实：** `RunnableWithMessageHistory` 构造即 `warn_deprecated`，`removal="2.0.0"`，建议改用 LangGraph 持久化。

**源码事实：** 读历史是流水线的一步（失败则整体失败），写历史是 `on_end` 回调（失败不影响已完成调用）。

**源码事实：** 写侧用 `input_messages[len(historic_messages):]` 切掉读侧前置的历史，避免重复写入。

**源码事实：** `_merge_configs` 的错误消息里含可直接复制的 `invoke` 示例。

**架构推论：** 「惰性方法是真接口、急切方法是便利包装」这个统一形态，是针对「内容可能很大、可能很多」这一现实的直接回应。把 `lazy_` 当契约、把急切版当包装，等于强制每个实现至少想过「怎么一条一条地出」，而急切版只是 `list(...)` 一层。代价是这套抽象永远有一个「部分实现」的中间态（只实现了急切版的子类），也就永远需要那条「未来会收紧」的注释。

**架构推论：** `BaseLoader` 那句「未来会升级为抽象方法」的注释，是框架演化中一类常见的技术债标记。它的价值在于**把意图写在最靠近代码的地方**——后来者看到这条注释就知道「不要在新的子类里省略 `lazy_load`」。而代价是它无法自动执行：没有任何东西会提醒维护者在「所有子类都实现了」时去改这一行。

**架构推论：** 「读侧拼接、写侧切除」这个配对，是自动历史管理最容易出错的地方。它把一个**有状态的操作**（历史长度）跨了两次调用——读时记在输入里、写时再算一次。任何在这之间发生的并发写入都会让切片偏移失效。这也是为什么这类机制在单线程 demo 里完美、在并发服务里会漏消息。判据很清楚：**若一个操作依赖「两次调用之间状态没变」，那它就不能用在并发环境**——除非把状态本身也纳入那次调用（这正是 LangGraph 的 checkpointer 做的事）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/document_loaders/base.py` | `BaseLoader` | ABC 但无抽象方法，靠委托实现抽象性 |
| `libs/core/langchain_core/document_loaders/base.py` | `BaseLoader.lazy_load` | 默认回退到 `load`，都不行则报错 |
| `libs/core/langchain_core/document_loaders/base.py` | `BaseLoader.alazy_load` | 用哨兵桥接同步迭代器为异步生成器 |
| `libs/core/langchain_core/document_loaders/base.py` | `BaseLoader.load_and_split` | 带 danger 警告的废弃便利方法 |
| `libs/core/langchain_core/document_loaders/base.py` | `BaseBlobParser` | 只要求 `lazy_parse` 的解析器契约 |
| `libs/core/langchain_core/document_loaders/base.py` | `BaseBlobParser.parse` | 急切包装，定位与 `load` 同构 |
| `libs/core/langchain_core/document_loaders/blob_loaders.py` | `BlobLoader` | 只有惰性方法的 blob 契约 |
| `libs/core/langchain_core/document_loaders/blob_loaders.py` | `BlobLoader.yield_blobs` | 唯一抽象方法，产出 blob 流 |
| `libs/core/langchain_core/runnables/history.py` | `RunnableWithMessageHistory` | 读历史、调 runnable、写历史的绑定层 |
| `libs/core/langchain_core/runnables/history.py` | `RunnableWithMessageHistory.__init__` | 构造即废弃警告与流水线拼装 |
| `libs/core/langchain_core/runnables/history.py` | `_enter_history` | 拷贝历史并追加本次输入 |
| `libs/core/langchain_core/runnables/history.py` | `_exit_history` | 切片去掉前置历史后写回 |
| `libs/core/langchain_core/runnables/history.py` | `_merge_configs` | 缺键报错并附可复制的调用示例 |

## 参考

- [langchain 源码：document_loaders/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/document_loaders/base.py)
- [langchain 源码：runnables/history.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/runnables/history.py)
- [langchain 文档：如何为对话添加记忆](https://python.langchain.com/docs/how_to/message_history/)

## 与主核 / AI 的关系

主核里最接近的是「游标式扫描」与「会话状态」两件事。加载器那份对应**流式读取**：不一次把全部结果装进内存，而是一条条产出。`BaseLoader` 与 `BaseBlobParser` 的分工（读原始字节 / 解析成结构）也与主核里「物理读」与「逻辑解码」的分层一致。

而 `RunnableWithMessageHistory` 对应的恰好是主核里的**会话临时状态**：连接进来时读一份状态、执行、把结果写回。差别在一致性——主核的会话状态在事务里，读与写之间有隔离保证；这里的「读历史再写历史」是两个独立操作，中间窗口内别人也能写。所以那份 deprecated 建议（改用 LangGraph 的持久化）本质上是「把会话状态搬进一个有事务语义的地方」，与主核的做法趋同。

与本组其他页的关联：`load(run.inputs, allowed_objects="messages")` 与本组 g04 的 `dump`/`load` 用的是同一套序列化/反序列化机制，其类型白名单的设计见 i01；`history_chain` 的拼装用的是 runnable 组合（本组之外的基础设施）；而 `_get_input_messages` / `_get_output_messages` 那三形态归一（str / message / dict）与 h01 的模型契约直接相关。

与 goose 的 `elide_to_budget`（上下文裁剪）对照：那边处理的是「历史太长要压」，这里处理的是「历史该在哪儿存」。两者在真实系统里必须同时存在——压完了还得存，存的地方还得能并发。

对 AI 工程最实用的一条是「同步迭代器桥接异步生成器要用哨兵」。`StopIteration` 在异步生成器内部会变成 `RuntimeError`，所以不能靠它结束循环。用 `next(iterator, sentinel)` 永不抛、再比较哨兵，是唯一干净的写法。任何把旧式生成器接入 async 代码的地方都会踩这个坑，而症状是「最后一条数据处偶发 RuntimeError」——非常难归因。

另一条是「读侧拼接、写侧切除」的教训。任何「回来时要算出差量」的机制，都要保证「差量的基准在两次之间没变」。如果做不到，就不要用「算长度差」这种办法，而应当让读侧返回一个显式的「本次新增」标记，或把整次操作纳入同一个状态版本。这里的切片在没有并发时是对的，在并发时会丢消息——而丢失的历史消息比报错更难发现。

最后一条是 `_merge_configs` 那个带示例的错误消息。配置类 API 的报错最容易写成「缺少键 X」——用户知道缺什么，但不知道怎么传。把 `chain.invoke({...}, {'configurable': {...}})` 整段拼进消息，用户就能直接复制运行。这一行的成本极低，收益是把「查文档」这一步整个省掉。
