---
project: llama-index
feature: settings
title: 全局设置与解析注册
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/settings.py
    symbols: [_Settings]
  - path: llama-index-core/llama_index/core/node_parser/loading.py
    symbols: [load_parser]
---

# 全局设置与解析注册

## 一句话职责

用一个模块级单例把「默认模型、默认嵌入、默认切分器、默认 token 计数器」收成一处懒初始化配置——每个值第一次被读时才去解析，而且**每次读都会把当前的 callback manager 追加上去**；同时用一张「类名 → 类」的静态表把切分器的反序列化收成一个白名单。

## 机制是什么

`_Settings` 是一个 `@dataclass`，八个字段全是私有的 `Optional` 加下划线前缀：

```python
_llm、_embed_model、_callback_manager、_tokenizer、
_node_parser、_prompt_helper、_chat_prompt_helper、_transformations
```

而模块末尾一行 `Settings = _Settings()` ——**单例**。所以「全局默认值」在这个框架里就是一个模块级对象，不是环境变量、不是配置文件、不是依赖注入容器。

对外暴露的是成对 property：`llm`、`embed_model`、`callback_manager`、`tokenizer`、`node_parser`、`prompt_helper`、`chat_prompt_helper`、`transformations`，另加一批**转发型 property**：`chunk_size`、`chunk_overlap`、`num_output`、`context_window`（以及它们的 `chat_` 版本）、`text_splitter`、`pydantic_program_mode`。

`load_parser(data: dict) -> NodeParser` 只有 43 行的文件（含导入），核心是一张 `all_node_parsers: Dict[str, Type[NodeParser]]` 九项表，加一个三分支函数。

## 怎么实现的

**懒初始化是「读时解析」，而每个 getter 都做同一件额外的事。** 看 `llm` 的 getter：

```python
if self._llm is None:
    self._llm = resolve_llm("default")
if self._callback_manager is not None:
    self._llm.callback_manager = self._callback_manager
return self._llm
```

所以**每次读 `Settings.llm` 都会把当前的 callback manager 写进 LLM 实例**，而不只是第一次。`embed_model` 与 `node_parser` 的 getter 也是同一段。

这个「每次读都追加」的选择解决了一个真实的时序问题：`Settings.callback_manager` 可能在任何时刻被设置（第一次被读时它自己也懒创建成空 `CallbackManager()`）。若只在首次解析时传递，那么「先设 llm、后设 callback_manager」这个顺序就会让 callback manager 永远不生效。每次读都传，则两个方向的顺序都能收敛。

代价是「读一个属性有写副作用」——`Settings.llm` 不是一个纯 getter。这在并发场景下是无保护的写（没有锁），但因为单例被设计成在启动期配置完就不再改，实际风险有限。

**setter 都做「解析」而不是「赋值」。** `llm` 的 setter 是 `self._llm = resolve_llm(llm)` ——所以 `Settings.llm = "gpt-4"` 传一个字符串也能成立，`resolve_llm` 会把字符串解析成实际的 LLM 实例。`embed_model` 同理（`resolve_embed_model`）。

所以这两个属性接受「宽类型」：`llm` 的 setter 参数是 `LLMType`（可含字符串），`embed_model` 的参数是 `EmbedType`。而 getter 返回的是确定的实例类型。这是一个**在边界上做归一**的设计：调用方可以偷懒传字符串，框架负责解析。

而 `node_parser` 的 setter 不做解析（直接赋值），因为 `NodeParser` 没有对应的 `resolve_*` 函数——这也解释了 `load_parser` 为什么存在：它是那个解析路径，只是它收的是 `dict` 而不是类型联合。

**默认值全是「最简实现」。** `Settings.llm` 未设时 `resolve_llm("default")`；`Settings.embed_model` 未设时 `resolve_embed_model("default")`；`Settings.node_parser` 未设时 `SentenceSplitter()`；`Settings.callback_manager` 未设时 `CallbackManager()`；`Settings.transformations` 未设时 `[self.node_parser]`。

`transformations` 那一条特别能说明设计的意图：**默认的转换链就是「一次切分」**。所以「我没配置任何处理管线」这个状态在框架里等价于「只有切分器」。

**转发型 property 是「把设置对象的字段挂到单例上」的一层便利。** 例如 `chunk_size`：

```python
@property
def chunk_size(self) -> int:
    if hasattr(self.node_parser, "chunk_size"):
        return self.node_parser.chunk_size
    else:
        raise ValueError("Configured node parser does not have chunk size.")
```

注意它先通过 `self.node_parser` 拿到（或懒创建）一个解析器，再看有没有那个属性。所以 `Settings.chunk_size = 512` 实际上是在改「当前 node parser 的 chunk_size」。而若当前 parser 是 `HierarchicalNodeParser` 这种没有 `chunk_size` 的，就报 `ValueError("Configured node parser does not have chunk size.")`。

`text_splitter` 与 `node_parser` 是**同一存储的两个名字**（`text_splitter` 的 getter 就是 `return self.node_parser`，setter 就是 `self.node_parser = text_splitter`）。所以「切分器」这个概念在这一层有两个名字，它们完全等价。这是历史遗留的别名——`TextSplitter` 是早期名字，`NodeParser` 是后来的。

`pydantic_program_mode` 没有自己的存储，而是 `return self.llm.pydantic_program_mode`。所以它是**把 LLM 实例上的一个字段提升到全局设置层**。好处是「用户可以设 `Settings.pydantic_program_mode = ...` 而不必拿到底层 LLM 对象」；代价是这个设置与具体的 LLM 实例绑定，换 LLM 就重置了。

**`prompt_helper` 有一个「按 LLM 元数据构造」的分支，且顺序关键。** 实现：

```python
if self._llm is not None and self._prompt_helper is None:
    self._prompt_helper = PromptHelper.from_llm_metadata(self._llm.metadata)
elif self._prompt_helper is None:
    self._prompt_helper = PromptHelper()
return self._prompt_helper
```

第一个分支用的是 `self._llm`（**下划线字段**）而不是 `self.llm`（property）。所以它**不会**触发 LLM 的懒解析——若用户没设过 LLM，`self._llm` 是 `None`，就走第二个分支给一个裸 `PromptHelper()`。

这个差别的实际后果是：「只读了 `Settings.prompt_helper` 不会顺带把默认 LLM 解析出来」。而反过来，若用户已设 `Settings.llm = "gpt-4"`，则 prompt helper 会从该模型的 metadata 里取到上下文窗口与输出 token 数。

`chat_prompt_helper` 多一个条件 `and is_chat_model(self._llm)` ——只有当前模型是聊天模型时才从它构造聊天版 helper。这解释得通：`ChatPromptHelper` 需要知道聊天消息的结构（例如 system/assistant/user 的分别），而补全模型没有这个概念。

**`context_window` / `num_output` 是两层转发。** `Settings.context_window` → `self.prompt_helper.context_window` → 那个 helper 内部的字段。而 `Settings.chat_context_window` → `self.chat_prompt_helper.context_window`。

所以「上下文窗口」在这套设置里有两个值：补全版与聊天版。这个分裂是必要的——两类模型的提示构造方式不同（补全版把所有东西拼成一段文本，聊天版保留消息边界），于是「能塞多少上下文」的估算也不同。

**`tokenizer` 的 setter 有一段对 `transformers` 的特判。** 实现：

```python
try:
    from transformers import PreTrainedTokenizerBase  # pants: no-infer-dep
    if isinstance(tokenizer, PreTrainedTokenizerBase):
        from functools import partial
        tokenizer = partial(tokenizer.encode, add_special_tokens=False)
except ImportError:
    pass
set_global_tokenizer(tokenizer)
```

所以「用户传入 HuggingFace 的 tokenizer 对象」会被自动包成 `partial(tokenizer.encode, add_special_tokens=False)` —— 转成「字符串进、token id 列表出」的函数。

`add_special_tokens=False` 这个参数很关键：token 计数要的是「这段文本本身占多少 token」，不该把 `[CLS]` / `[SEP]` 这类分隔符算进去——否则每段都会多算两三个，累积误差明显。

`except ImportError: pass` 让「没装 transformers」时优雅降级：用户传一个普通可调用对象照样能用，只是不会被自动包装。而注释 `# pants: no-infer-dep` 说明这个导入是「可选依赖，不该被静态推断为必需」。

注意 `_tokenizer` 字段在这里**没有被使用**——setter 写的是 `set_global_tokenizer(tokenizer)`（改动的是 `llama_index.core.global_tokenizer`），而 getter 读的也是那个全局变量而不是 `self._tokenizer`。所以 `_tokenizer` 是一个**没被用到的字段**（遗留）。getter 的注释「TODO: deprecated?」也印证了这块正在向「模块级全局变量」迁移。

**`global_handler` 是唯一一个「setter 签名与 getter 完全不同」的属性。** getter 无参返回 `Optional[BaseCallbackHandler]`；setter 是 `def global_handler(self, eval_mode: str, **eval_params: Any)` ——收一个模式名加可变参数，然后调 `set_global_handler(eval_mode, **eval_params)`。

所以 `Settings.global_handler = "wandb"` 这种写法其实是不成立的（Python 的 property setter 只接受一个值），必须写成把两个参数打包的调用。这让它更像一个「挂在属性位置上的命令」而不是一个可存取的字段。`# TODO: deprecated?` 的注释也说明作者自己对这个形态不确定。

**`load_parser` 是一个三分支函数加一张静态白名单表。** 实现：

```python
if isinstance(data, NodeParser):
    return data
parser_name = data.get("class_name")
if parser_name is None:
    raise ValueError("Parser loading requires a class_name")
if parser_name not in all_node_parsers:
    raise ValueError(f"Invalid parser name: {parser_name}")
else:
    return all_node_parsers[parser_name].from_dict(data)
```

四步值得逐一记：

1. **`isinstance(data, NodeParser)` 就原样返回**——所以这个函数接受「已经是解析器」的输入，是一个「归一化器」而不只是「反序列化器」。
2. **缺 `class_name` 报错**，消息是 `"Parser loading requires a class_name"`。
3. **不在白名单里报错**，消息带上了具体名字。
4. **命中就调该类的 `from_dict(data)`** ——把整个 dict（含 `class_name`）都传进去。

第 4 步把 `class_name` 也传给 `from_dict` 是一个细节：子类的 `from_dict` 需要容忍这个多余字段（或把它当初始化参数）。`BaseComponent` 那套机制通常允许它。

**`all_node_parsers` 的键是 `Class.class_name()` 而不是硬编码字符串。** 每一项都写成 `SentenceSplitter.class_name(): SentenceSplitter` ——所以键与类**在编译期就对齐**，不会出现「字符串写错但类对」的情况。而类名本身由 `class_name()` 给出（通常是类名，也可能被子类覆盖）。

这张表只登记了**九个**解析器：`HTMLNodeParser`、`JSONNodeParser`、`MarkdownNodeParser`、`SimpleFileNodeParser`、`HierarchicalNodeParser`、`CodeSplitter`、`SentenceSplitter`、`TokenTextSplitter`、`SentenceWindowNodeParser`。

而 `llama_index/core/node_parser/` 下的实际类远不止这些（还有 `token_splitter`、`semantic_splitter`、`structured_parser`、`interface` 等）。所以这张表**不是「所有解析器」**，而是「可以从序列化数据重建的那些」。这个限制体现为：一个不在表里的解析器被序列化后，反序列化会失败并报 `"Invalid parser name: xxx"`。

用白名单而不是「按名字动态 import」的理由是安全与确定性：动态 import 等于「让数据决定实例化什么类」，这正是序列化页里那套允许清单要防的。而这里的白名单是**编译期固定的 import**，所以无论数据里写什么名字，能实例化的只有这九类。

## 关键取舍

**源码事实：** `Settings` 是 `_Settings()` 的模块级单例，八个字段全私有且 `Optional`。

**源码事实：** `llm` / `embed_model` / `node_parser` 的 getter 每次读都会把当前 `_callback_manager` 写进实例（不只是首次）。

**源码事实：** `llm` 与 `embed_model` 的 setter 走 `resolve_*` 解析（接受字符串），`node_parser` 的 setter 直接赋值。

**源码事实：** `prompt_helper` getter 用 `self._llm`（私有字段）而非 `self.llm`，所以读它不会触发 LLM 懒解析。

**源码事实：** `chat_prompt_helper` 额外要求 `is_chat_model(self._llm)` 才从 metadata 构造。

**源码事实：** `text_splitter` 与 `node_parser` 是同一存储的两个名字。

**源码事实：** `pydantic_program_mode` 无自己的存储，转发到 `self.llm` 的字段。

**源码事实：** `chunk_size` / `chunk_overlap` / `context_window` / `num_output` 都是「转发到 node_parser 或 prompt_helper」，缺失时报「配置的解析器没有该属性」。

**源码事实：** `tokenizer` 的 setter 对 `PreTrainedTokenizerBase` 自动包成 `partial(encode, add_special_tokens=False)`，`transformers` 缺失时静默跳过；`_tokenizer` 字段实际未被使用。

**源码事实：** `global_handler` 的 getter 无参、setter 收 `(eval_mode, **eval_params)`，形态不一致。

**源码事实：** `load_parser` 先判「已是解析器」直接返回，再按 `class_name` 查九项白名单，查不到报错。

**架构推论：** 「懒初始化单例」这种全局配置形态，解决的是「框架层无法要求用户显式注入所有依赖」这个现实。一个 RAG 框架有几十个组件，若每个都需要显式传入 LLM 与嵌入模型，那入门门槛会高到没人愿意用。单例让「什么都不配也能跑」成立——代价是**隐式依赖**：任何一个组件内部读 `Settings.llm` 时，实际用的是全局那份，而在多租户/多配置场景里这就成了问题。

**架构推论：** 「每次读都重新追加 callback manager」这个做法揭示了一个深层矛盾：`Settings.llm` 既是一个「配置槽」（应该是纯读写），又是一个「依赖注入点」（需要把全局副作用传导进去）。两者合并到同一个属性上，就必然产生「读有副作用」。真正的解法是显式的 `configure()` 阶段（把依赖一次性灌进去），但那会失去懒初始化的便利。

**架构推论：** `load_parser` 的白名单是「可控反序列化」的最小实现。它没有用 `importlib` 按名字动态加载，而是把九个类静态导入后按 `class_name()` 索引。这带来一个具体的收益：**无论传入什么字符串，能实例化的类集合都不变**。这与序列化那一类机制的「允许清单」是同一条原则的两种实现——一种是「运行时检查类路径是否在清单内」，一种是「清单本身就是唯一的入口」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/settings.py` | `_Settings` | 八字段懒初始化配置容器 |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.llm` | 读时解析默认模型并追写 callback manager |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.embed_model` | 同上，setter 走 `resolve_embed_model` |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.node_parser` | 默认 `SentenceSplitter()`，setter 直接赋值 |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.prompt_helper` | 用私有 `_llm` 判断，读它不触发 LLM 解析 |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.chat_prompt_helper` | 额外要求当前模型是聊天模型 |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.chunk_size` | 转发到 node_parser，缺失属性时报错 |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.text_splitter` | `node_parser` 的历史别名 |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.tokenizer` | 自动包装 HF tokenizer，`add_special_tokens=False` |
| `llama-index-core/llama_index/core/settings.py` | `_Settings.transformations` | 默认就是「一次切分」 |
| `llama-index-core/llama_index/core/settings.py` | `Settings` | 模块级单例实例 |
| `llama-index-core/llama_index/core/node_parser/loading.py` | `all_node_parsers` | 九项「类名 → 类」静态白名单 |
| `llama-index-core/llama_index/core/node_parser/loading.py` | `load_parser` | 已是解析器直接返回，否则按白名单反序列化 |

## 参考

- [llama-index 源码：settings.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/settings.py)
- [llama-index 源码：node_parser/loading.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/loading.py)
- [llama-index 文档：全局设置](https://docs.llamaindex.ai/en/stable/module_guides/supporting_modules/settings/)

## 与主核 / AI 的关系

主核里最接近的是「全局配置与默认值体系」：一个数据库实例有一批 GUC 参数（工作内存、缓冲区大小、默认隔离级别），未设置时取内置默认值。`Settings` 与那套参数体系的职责相同，连「懒生效」这个特征都像——某些参数只在首次使用时才真正解析。

差别在于**可变性与作用域**。主核的参数是「实例级、可事务性修改、有明确的取值范围与校验」，而这里的 `Settings` 是「进程级、随时可改、几乎无校验」。所以多实例场景下主核可以给每个连接不同的参数，而这里只能全局换掉——换的时候会影响所有正在跑的组件。

与 h01（存储上下文）的对照很有意思：`StorageContext` 是**显式传入**的（每个索引各自持一份），而 `Settings` 是**隐式读取**的（任何组件都能跨界取用）。同一个框架里两种依赖传递风格并存，说明它没有统一的 DI 方案，而是按组件年龄分成两代。

与本页第二部分的对照：`load_parser` 那张白名单表，与 h03 的 `PropertyGraphStore` 能力位、`MetadataFilters.legacy_filters` 的明确报错，都是同一种态度——**能力边界要显式**。前者靠「只有这九个类」限制，后者靠「不支持就抛」，都在避免「静默降级」。

对 AI 工程最实用的一条是「识别懒初始化单例的隐式耦合」。把 `Settings.llm` 换成另一个模型时，所有读它的组件会在下一次读时看到新值——包括那些已经拿到旧引用的组件内部逻辑。这在单用户脚本里没问题，在服务里会表现为「改配置后行为部分改变」。若要把这类框架用在多租户场景，正确做法是**给每个请求显式构造自己的 Settings 副本**（或直接用显式注入），而不是依赖那个模块级对象。

另一条是「转发型 property 要有明确的失败消息」。`chunk_size` 在「当前解析器没有这个属性」时报的是 `"Configured node parser does not have chunk size."` ——它说的是「配置的解析器没有这个能力」，而不是 `AttributeError`。这让人能立刻明白「不是漏配了值，而是配错了组件类型」。

最后一条是 `load_parser` 的「先归一化再反序列化」结构。它把两种输入（已构造的对象、待构造的 dict）收在一个入口上，让调用方不必先判断类型。这个模式的成本只有一行 `isinstance`，收益是消除了「上层到处写 if 判断」的重复。
