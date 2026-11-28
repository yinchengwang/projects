---
project: llama-index
feature: auto-retriever
title: 自动检索与选择器
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/base/base_auto_retriever.py
    symbols: [BaseAutoRetriever]
  - path: llama-index-core/llama_index/core/base/base_selector.py
    symbols: [BaseSelector, SingleSelection, MultiSelection]
---

# 自动检索与选择器

## 一句话职责

`BaseAutoRetriever` 把「检索」拆成**两阶段**：先让 LLM 产出一个「检索规格」（spec），再按规格造一个真正的检索器去执行；`BaseSelector` 是「从若干候选里挑一个/几个」的通用抽象，输出带序号的 `SingleSelection` / `MultiSelection`。

## 机制是什么

这两者是同一件事的两半：**让模型参与决定「怎么查」而不是「查什么」**。

`BaseAutoRetriever` 处理的是**参数推断**：用户只给一句自然语言，而向量检索需要 `query`、`filters`、`top_k`。让 LLM 从自然语言里推出这三个参数，比让用户手写 `MetadataFilters` 更自然。

`BaseSelector` 处理的是**目标选择**：面对若干检索器（或查询引擎），先决定「该问哪一个」。它的输入是候选的元数据（`ToolMetadata.description`），输出是序号 + 理由。

两者共享一个模式：**LLM 的产出不是最终结果，而是「下一步做什么」的指令**。所以它们都在「结构化输出 → 校验/兜底 → 构造执行者」这条链上。

## 怎么实现的

**`BaseAutoRetriever` 只有五个方法，其中一个方法体只有三行。**

```python
class BaseAutoRetriever(BaseRetriever):
    @abstractmethod
    def generate_retrieval_spec(self, query_bundle: QueryBundle, **kwargs: Any) -> BaseModel: ...

    @abstractmethod
    async def agenerate_retrieval_spec(self, query_bundle: QueryBundle, **kwargs: Any) -> BaseModel: ...

    @abstractmethod
    def _build_retriever_from_spec(self, retrieval_spec: BaseModel) -> Tuple[BaseRetriever, QueryBundle]: ...

    def _retrieve(self, query_bundle: QueryBundle) -> List[NodeWithScore]:
        retrieval_spec = self.generate_retrieval_spec(query_bundle)
        retriever, new_query_bundle = self._build_retriever_from_spec(retrieval_spec)
        return retriever.retrieve(new_query_bundle)

    async def _aretrieve(self, query_bundle: QueryBundle) -> List[NodeWithScore]:
        retrieval_spec = await self.agenerate_retrieval_spec(query_bundle)
        retriever, new_query_bundle = self._build_retriever_from_spec(retrieval_spec)
        return await retriever.aretrieve(new_query_bundle)
```

四处：

1. **三个抽象方法全是「必须由子类实现」，但两个是同步/异步对，第三个只有同步**。`_build_retriever_from_spec` **没有异步版**——而它做的事情（造一个检索器对象）本来也不该是异步的。这个设计是对的：把「可能有 I/O 的」（生成 spec）与「纯内存的」（造检索器）分开。

2. **返回值是 `Tuple[BaseRetriever, QueryBundle]`** —— 即「造出的检索器」+「改写后的查询」。后者是关键：spec 里可能推出了一个**新的查询串**（例如用户问「2023 年的财报」，spec 把 query 变成「财报」而把 `year=2023` 放进 filters）。所以「查询」与「过滤条件」从一句话里被**分解**开了。

3. **`_aretrieve` 里 `await self.agenerate_retrieval_spec(...)` 但 `_build_retriever_from_spec` 没有 `await`**——async 方法里调同步方法。这是对的（那个方法纯 CPU），但也是「async 函数里混同步调用」的一个正确示例（对比 d01 里 `_aretrieve` 的默认实现是阻塞的）。

4. **`spec` 的类型是 `BaseModel` 而不是具体类型**。所以基类完全不关心 spec 长什么样——「规格」这个概念被留成了完全开放的。子类（如 `VectorIndexAutoRetriever`）用 `VectorStoreQuerySpec`。

   代价是：**`_build_retriever_from_spec` 的签名只能写 `BaseModel`**，而实际实现要 `cast` 成具体类型（源码里 `VectorIndexAutoRetriever` 的实现带 `# type: ignore`）。所以这个抽象在类型上是不闭合的。

**`VectorIndexAutoRetriever` 是唯一的实现，而它的 spec 生成有两处值得记。**

```python
def generate_retrieval_spec(self, query_bundle: QueryBundle, **kwargs: Any) -> BaseModel:
    info_str = self._vector_store_info.model_dump_json(indent=4)
    schema_str = VectorStoreQuerySpec.model_json_schema()
    output = self._llm.predict(self._prompt, schema_str=schema_str, info_str=info_str, query_str=query_bundle.query_str)
    return self._parse_generated_spec(output, query_bundle)
```

三处：

1. **每次调用都重新算 `info_str` 与 `schema_str`**。前者是 `VectorStoreInfo` 的 JSON（元数据字段的说明 + 内容描述），后者是 `VectorStoreQuerySpec` 的 JSON Schema。**两者都是常量**（`self._vector_store_info` 在构造时固定），却每次序列化一遍。

   这是一个可优化的点：把两个字符串在构造时算好即可。但源码没做——大概因为 `predict` 会缓存提示（同一模板 + 同一变量会命中 LLM 缓存，若供应商支持）。

2. **`schema_str` 被塞进提示**——即让模型看到「输出应该长什么样的 JSON Schema」。这是「用 schema 引导结构化输出」的通用做法。

3. **`info_str` 里含 `content_info`（内容描述）与 `metadata_info`（可用元数据字段的说明）**。所以模型知道「这个库里有关于财报的内容，且 `year`、`company` 两个字段可以过滤」。这是**把索引的能力描述喂给模型**——自动检索能工作的前提。

**`_parse_generated_spec` 的兜底策略值得逐字读。**

```python
def _parse_generated_spec(self, output: str, query_bundle: QueryBundle) -> BaseModel:
    try:
        structured_output = cast(StructuredOutput, self._output_parser.parse(output))
        query_spec = cast(VectorStoreQuerySpec, structured_output.parsed_output)
    except OutputParserException:
        _logger.warning("Failed to parse query spec, using defaults as fallback.")
        query_spec = VectorStoreQuerySpec(query=query_bundle.query_str, filters=[], top_k=None)

    return query_spec
```

三处：

1. **只捕获 `OutputParserException`**。所以解析器的其他异常（如 `KeyError`、`ValidationError`）会传播——尽管它们同样意味着「模型输出不合规」。这个捕获范围是**按异常类型**而不是「按语义」定的，说明解析器的契约是「解析失败一律抛 `OutputParserException`」。

2. **兜底 spec 是「用原始查询 + 无过滤 + 不指定 top_k」**。这是一个很正确的降级：即使 LLM 输出完全不可解析，检索仍然按「普通向量检索」进行。**功能不是消失而是退化成默认行为**。

3. **兜底路径没有上报到调用方**（只有一条 `_logger.warning`）。所以用户不知道 LLM 这一步失败了——他看到的是「检索结果不太准」。

    这是一个可辩的选择：自动检索的降级是「退回到非自动」，所以功能仍可用。但静默降级让「LLM 抽参数的能力差」这个问题不可观测——而这恰恰是用户需要知道的事（可能该换模型或改提示）。

**`_build_retriever_from_spec` 是自动检索的核心，而它的四步都在处理「模型给的值可能不合理」。**

```python
new_query_bundle = self._get_query_bundle(spec.query)

# define similarity_top_k
if spec.query or self._empty_query_top_k is None:
    similarity_top_k = self._similarity_top_k
else:
    similarity_top_k = self._empty_query_top_k

if spec.top_k is not None:
    similarity_top_k = min(spec.top_k, self._max_top_k, similarity_top_k)

# avoid passing empty filters to retriever
if len(spec.filters) + len(self._extra_filters.filters) == 0:
    filters = None
else:
    filters = MetadataFilters(filters=[*spec.filters, *self._extra_filters.filters])
```

四个决策：

1. **`similarity_top_k` 的基准值取决于「查询是否为空」**。这是一个很实际的设计：当 spec 推出了「纯靠元数据过滤」时（`query=""`），方案是「取更多的结果」（`empty_query_top_k` 默认 10 而非 `similarity_top_k` 的默认 5）。理由清楚——纯过滤没有相关性排序，所以多取一些。

   而条件是 `spec.query or self._empty_query_top_k is None` —— 即「有查询串 **或** 没有配 `empty_query_top_k`」时用 `similarity_top_k`。注意这里 `spec.query` 用**真值判断**（空串走 else）——这是正确的，因为「空查询」的判据就是空串。

2. **`min(spec.top_k, self._max_top_k, similarity_top_k)`** —— **三重 clamp**。模型给的 `top_k` 被压到不超过「全局上限」也不超过「本次基准」。这个设计是必要的（模型可能说「返回 1000 条」），而两个上限各有来源：`max_top_k` 是部署配置，`similarity_top_k` 是本次检索的默认。

   注意 `min` 的三个参数里 `similarity_top_k` 已经是基准值（可能来自 `_empty_query_top_k`）。所以模型的 `top_k` 只能**降低**而不能提高。

3. **空过滤器不传给检索器**（`filters = None`）。理由在注释里：「avoid passing empty filters to retriever」——因为一个空 `MetadataFilters` 在某些向量库上会被当成「必须有匹配的元数据」，导致返回空结果。这是一个**后端兼容性补丁**。

4. **`[*spec.filters, *self._extra_filters.filters]`** —— 模型的过滤器与构造时传入的额外过滤器**合并**（而非后者覆盖前者）。所以「部署者强制加的范围限制」无法被模型的输出绕过。这是一个**安全方向正确的设计**：`extra_filters` 是约束，不是默认值。

   而 `extra_filters` 的 `OR` 条件在构造时就被拒绝了（见下），所以合并后的语义是 AND——这是可靠的前提。

**构造时对 `extra_filters` 有一处明确的拒绝。**

```python
if extra_filters is not None and extra_filters.condition == FilterCondition.OR:
    raise ValueError("extra_filters cannot be OR condition")
self._extra_filters = extra_filters or MetadataFilters(filters=[])
```

即**不允许 `extra_filters` 用 OR**。理由在前面那步合并能看出来：`MetadataFilters(filters=[*spec.filters, *extra])` 会把所有过滤器拍进同一个列表，而该列表有一个单一的 `condition`。若 `extra` 是 OR 而 spec 的是 AND，合并后的语义就既不是 AND 也不是 OR。

所以这是一个**「语义无法正确合并，因此拒绝输入」**的处理。方向正确，但它只拒绝了 `OR`，没有拒绝「spec 的 condition 与 extra 的不同」——而后者同样会造成语义错误。不过 `spec.filters` 的类型是 `List[MetadataFilter]`（不是一个 `MetadataFilters`），所以它没有 condition（隐含 AND）。于是只有 `extra` 的 condition 需要限制。这个分析让那条拒绝显得恰好充分。

**`_get_query_bundle` 是「空查询 + 默认向量」的桥。**

```python
def _get_query_bundle(self, query: str) -> QueryBundle:
    if not query and self._default_empty_query_vector is not None:
        return QueryBundle(query_str="", embedding=self._default_empty_query_vector)
    else:
        return QueryBundle(query_str=query)
```

即：**spec 推出空查询时，若配了 `default_empty_query_vector`，就用它当查询向量**。这让「纯元数据过滤」在「必须给向量」的后端上也成立——给一个固定的占位向量（通常全零或均值），配合 filters 做过滤。

这是一个很实用的技巧，而它的代价是：向量检索会用一个**与查询无关的固定向量**去算相似度，所以「相似度排序」完全是噪声，只有 filters 在起作用。这正是 `empty_query_top_k` 默认更大的原因（排序不可用，只能多取）。

**`BaseSelector` 的两个子模型是本篇最值得细看的部分。**

```python
class SingleSelection(BaseModel):
    index: int
    reason: str

class MultiSelection(BaseModel):
    selections: List[SingleSelection]

    @property
    def ind(self) -> int:
        if len(self.selections) != 1:
            raise ValueError(f"There are {len(self.selections)} selections, please use .inds.")
        return self.selections[0].index

    @property
    def reason(self) -> str: ...
    @property
    def inds(self) -> List[int]: return [x.index for x in self.selections]
    @property
    def reasons(self) -> List[str]: return [x.reason for x in self.selections]
```

四处：

1. **`MultiSelection` 是唯一的「结果类型」，而 `SelectorResult = MultiSelection`**（注释说「separate name for clarity and to not confuse function calling model」）。所以**「单选」是「多选的特例」**——一个只有一个元素的 `MultiSelection`。

   这个选择的价值：调用方只需处理一种类型；`SelectorResult` 这个别名让它对「函数调用模型」看起来更像一个独立概念。

2. **`ind` / `reason` 是「恰好一个」的便捷访问器**，且**不满足时抛 `ValueError`**（而不是返回第一个）。这是好的设计：它把「我以为只选了一个」这个假设显式化了。错误消息还指出了正确用法（`please use .inds`）。

   而调用方必须知道这个陷阱：`RouterRetriever` 里在「多选」分支用 `result.inds`，在「单选」分支用 `result.ind`——而它靠 `len(result.inds) > 1` 分派。所以**一个元素时走单选路径**，不会触发 `ValueError`。

3. **`inds` 与 `reasons` 是纯派生属性**（每次访问都新建列表）。所以调用两次会有两个列表对象——但内容相同。

4. **`index` 是 0-based**（因为 `_structured_output_to_selector_result` 里做了 `answer.choice - 1`）。所以所有选择器的输出**统一成 0-based**，而模型看到的是 1-based（`_build_choices_text` 里 `f"({ind + 1})"`）。

   这个约定的方向是对的：**面向人的一侧用 1-based，面向程序的一侧用 0-based**，转换点在一处。但也意味着**没有边界检查**——模型给 `choice=11` 时 `index=10`，而后续 `self._retrievers[10]` 会 `IndexError`（`RouterRetriever` 没检查）。

**`BaseSelector` 本身很短，但它的 `select`/`aselect` 有归一化的职责。**

```python
def select(self, choices: Sequence[MetadataType], query: QueryType) -> SelectorResult:
    metadatas = [_wrap_choice(choice) for choice in choices]
    query_bundle = _wrap_query(query)
    return self._select(choices=metadatas, query=query_bundle)
```

四处的归一化：

1. **`MetadataType = Union[str, ToolMetadata]`** —— 候选可以是字符串或 `ToolMetadata`。`_wrap_choice` 把字符串包成 `ToolMetadata(description=choice)`。所以「给一串描述文字」也能当候选。

2. **`QueryType = Union[str, QueryBundle]`** —— 查询同样两态。`_wrap_query` 把字符串包成 `QueryBundle(query_str=query)`。

3. **两个 `_wrap_*` 都 `raise ValueError(f"Unexpected type: {type(choice)}")`** 兜底。所以类型不匹配时错误信息带实际类型。

4. **`select` 与 `aselect` 都做归一化，然后调 `_select` / `_aselect`**。所以归一化在基类、执行在子类——这是标准的分层。

**`aselect` 在基类是抽象的，与 d03（langchain 的结构化工具）不同。**

```python
@abstractmethod
def _select(self, choices: Sequence[ToolMetadata], query: QueryBundle) -> SelectorResult:
    pass

@abstractmethod
async def _aselect(self, choices: Sequence[ToolMetadata], query: QueryBundle) -> SelectorResult:
    pass
```

**两者都是 `@abstractmethod`**——所以子类必须同时实现同步与异步。这与 d01 的 `BaseRetriever`（异步是可选）相反。

这是一个重要的设计差别：**选择器强制要求异步实现**。理由大概是因为「选择」总是涉及一次 LLM 调用（有 I/O），所以异步不是可选的优化而是必需的能力。而 `BaseRetriever` 允许只有同步实现（因为有些检索器纯内存计算）。

注意两个方法体都是 `pass`——抽象方法的标准写法。但 `_select` 的 `pass` 与 `...` 的区别在于：`...` 是 `Ellipsis` 表达式（通常用于类型存根），`pass` 是空语句。混用两者在本项目里很常见（`base_auto_retriever.py` 用 `...`，这里用 `pass`）。

**`LLMSingleSelector` 与 `LLMMultiSelector` 的构造校验相同，而它们的 `_select` 差别在于「要不要限制输出数量」。**

```python
if self._prompt.output_parser is None:
    raise ValueError("Prompt should have output parser.")
```

两个选择器都有这一句。**「提示必须带输出解析器」** 是这个类的核心约束：选择器的返回值必须是结构化的 `SelectorResult`，所以它依赖提示自带的解析器把 LLM 文本转成结构。

这是一个把「解析」**绑在提示上**的设计。对比 d02（langchain 的输出解析）里解析器是独立对象——两种都可行，但这里的好处是「提示与它的解析器不能配错」（它们是一个整体）。代价是「换解析器必须换提示」。

`LLMMultiSelector` 的 `_select`：

```python
context_list = _build_choices_text(choices)
max_outputs = self._max_outputs or len(choices)

prediction = self._llm.predict(
    prompt=self._prompt,
    num_choices=len(choices),
    max_outputs=max_outputs,
    context_list=context_list,
```

三处：

1. **`max_outputs` 默认等于候选数**——即「不限制」。所以多选器默认可以选全部。
2. **提示里同时有 `num_choices` 与 `max_outputs`**。前者是「一共有几个候选」（让模型知道编号范围），后者是「最多选几个」。**这两个数是模型能给出合法序号的关键约束**。
3. **`context_list` 由 `_build_choices_text` 生成**：每个候选渲染成 `(N) description`，`\n\n` 分隔，并把 `description` 的换行折叠成空格（`" ".join(choice.description.splitlines())`）。折叠是必要的——否则多行描述会让编号与内容错位，模型难以对齐。

**`RouterRetriever` 是 `BaseSelector` 的消费者，而它的两处细节暴露了「多选」的合并问题。**

```python
if len(result.inds) > 1:
    retrieved_results = {}
    for i, engine_ind in enumerate(result.inds):
        logger.info(f"Selecting retriever {engine_ind}: {result.reasons[i]}.")
        selected_retriever = self._retrievers[engine_ind]
        cur_results = selected_retriever.retrieve(query_bundle)
        retrieved_results.update({n.node.node_id: n for n in cur_results})
else:
    try:
        selected_retriever = self._retrievers[result.ind]
        ...
    except ValueError as e:
        raise ValueError("Failed to select retriever") from e
    cur_results = selected_retriever.retrieve(query_bundle)
    retrieved_results = {n.node.node_id: n for n in cur_results}
```

四处：

1. **`retrieved_results` 用 `dict[node_id, NodeWithScore]` 当累加器**——所以**去重按 node_id，后到的覆盖先到的**。多选时，若两个检索器返回同一个节点，后者（按 `result.inds` 顺序的后者）的分数会赢。所以**分数由「检索器顺序」决定**——而顺序由 LLM 的 `selections` 列表顺序决定。

   注意这与 d01 的 `_handle_recursive_retrieval` 去重策略相反（那里保留**首次**出现）。两处都是去重，方向相反。

2. **`result.ind` 的 `ValueError` 被 catch 并重抛成 `"Failed to select retriever"`**。两种情况会走到这里：`selections` 为空（`len(result.inds) == 0` → 走 else 分支 → `ind` 抛 `ValueError`「There are 0 selections」），或恰好一个（正常）。所以**「LLM 一个都没选」被包装成「选择失败」**——这比返回空结果好（静默返回空列表会让人以为「没有相关文档」）。

   但**它丢失了原始错误消息**（没有从 `e` 里取文本，只用了 `from e` 保留链）。所以日志里看不到「There are 0 selections」这句关键信息。

3. **`self._retrievers[engine_ind]` 无边界检查**。`engine_ind` 来自 `result.inds`，而它来自 LLM 的序号减一。所以**模型给出超范围序号会 `IndexError`**（不是 `ValueError`，所以不会被那个 catch 接住）。

   这是本篇里最具体的崩溃点，而它的形态与 d01/d02 里「`choice_idxs` 无边界检查」完全一致。**三处同错**说明这是这个项目里的一类系统性缺失：**凡是从 LLM 输出派生的索引，都没有范围校验**。

4. **`_aretrieve` 里 `asyncio.gather(*tasks)` 并行**。注意同步版是**串行**（`for` 循环逐个调），而异步版并行。所以「多选时同步与异步的时序不同」——同步版若第一个检索器很慢，整体就慢；异步版取最慢的那个。

**`RouterRetriever._get_prompt_modules` 有一句注释揭示了它的取舍。**

```python
def _get_prompt_modules(self) -> PromptMixinType:
    """Get prompt sub-modules."""
    # NOTE: don't include tools for now
    return {"selector": self._selector}
```

即「只暴露选择器的提示，不暴露检索器工具们的提示」。理由是「for now」（暂时）——即这是一处有意留下的不完整。后果是**通过 `RouterRetriever` 做提示的集中管理时，候选检索器的提示无法通过这条路径访问**。

**`get_selector_from_llm` 的「优先 Pydantic 选择器」揭示了一个能力探测机制。**

```python
if is_multi:
    try:
        selector = PydanticMultiSelector.from_defaults(llm=llm)
    except ValueError:
        selector = LLMMultiSelector.from_defaults(llm=llm)
else:
    try:
        selector = PydanticSingleSelector.from_defaults(llm=llm)
    except ValueError:
        selector = LLMSingleSelector.from_defaults(llm=llm)
```

即**先试「函数调用/Pydantic 结构化输出」版，`ValueError` 则退回「纯文本解析」版**。`ValueError` 的含义大概是「这个模型不支持函数调用」。

这个探测方式是**用异常做能力协商**。它的优点是不必查询模型能力表；缺点是**把「模型不支持」与「构造过程出了别的错」混在一起**（任何 `ValueError` 都会导致降级）。更严的做法是显式查 `llm.metadata.is_function_calling_model`——而这个字段在 `LLMMetadata` 里确实存在（b01 里提过 `LLMMetadata` 有 `is_function_calling_model`）。

## 关键取舍

**源码事实：** `BaseAutoRetriever` 的三个抽象方法是「同步生成 spec / 异步生成 spec / 同步造检索器」，`_build_retriever_from_spec` **无异步版**，因此 `_aretrieve` 是「await 生成 + 同步造 + await 检索」。

**源码事实：** spec 的返回类型是裸 `BaseModel`，子类实现在需要 `cast` 并在签名上带 `# type: ignore`——抽象在类型上不闭合。

**源码事实：** `generate_retrieval_spec` 每次调用都重新序列化 `vector_store_info` 与 `VectorStoreQuerySpec` 的 JSON Schema（两者构造时已固定），把它们塞进提示（`schema_str` / `info_str`）。

**源码事实：** `_parse_generated_spec` 只捕获 `OutputParserException`，兜底成「原始查询 + 无过滤 + 无 top_k」的 spec，并且只有一条 `_logger.warning`——降级静默，调用方无从得知 LLM 这一步失败。

**源码事实：** `_build_retriever_from_spec` 的四步都偏向「压低模型的自由度」：空查询时用 `empty_query_top_k`、`top_k` 被 `min(spec.top_k, max_top_k, similarity_top_k)` 三重 clamp（只能降不能升）、空过滤器传 `None`（避免后端误判）、模型的过滤器与 `extra_filters` **合并**而非覆盖。

**源码事实：** 构造时拒绝 `extra_filters.condition == OR`，因为合并后所有过滤器共用单一 condition；而 `spec.filters` 是 `List[MetadataFilter]`（隐含 AND），所以只有 `extra` 需要限制。

**源码事实：** `_get_query_bundle` 在「spec 查询为空且有 `default_empty_query_vector`」时用固定向量当查询——使纯元数据过滤在「必须给向量」的后端上可行，但相似度排序随即变成噪声（这正是 `empty_query_top_k` 默认 10 而非 5 的原因）。

**源码事实：** `MultiSelection` 是唯一结果类型（`SelectorResult` 是它的别名），单选是多选的一个元素；`ind`/`reason` 在「不是恰好一个」时抛 `ValueError` 并指引改用 `inds`/`reasons`。

**源码事实：** 选择器的 `index` 统一 0-based（`answer.choice - 1`），而提示里渲染成 1-based（`(ind + 1)`）；转换只在一处，但**没有边界检查**。

**源码事实：** `BaseSelector.select`/`aselect` 负责归一化（`str → ToolMetadata`、`str → QueryBundle`），`_select`/`_aselect` **两个都是 `@abstractmethod`**（与 `BaseRetriever` 的异步可选相反）；两个 LLM 选择器都强制「提示必须带 `output_parser`」。

**源码事实：** `RouterRetriever` 多选时用 `dict[node_id, NodeWithScore]` 累加（**后到覆盖先到**，与 d01 的「保留首次」相反）；`self._retrievers[engine_ind]` 无边界检查（超范围会 `IndexError` 而非被 `ValueError` 捕获）；空选择被包装成 `"Failed to select retriever"` 且丢失原始消息；同步串行、异步 `gather` 并行。

**源码事实：** `get_selector_from_llm` 用 `try Pydantic*Selector except ValueError: 退回 LLM*Selector` 做能力协商，而 `LLMMetadata.is_function_calling_model` 这个显式字段未被使用。

**架构推论：** 「LLM 产出规格，程序按规格执行」这个两阶段结构，是「让模型参与决策」最可控的形态。它的关键性质是：**模型的输出必须先通过一个程序化的翻译层**（`_build_retriever_from_spec`），而这一层可以 clamp、可以合并、可以拒绝。所以模型无法直接控制 `top_k` 上限、无法绕过 `extra_filters`。这与「让模型直接输出检索结果」或「让模型调工具」相比，**边界更清**——但代价是多一次 LLM 调用（一次推断 + 一次检索）。

**架构推论：** 兜底降级（解析失败就退回普通检索）是一个正确的方向选择，但**静默**让它的价值减半。自动检索的能力边界（哪些查询能推出好参数）是需要观测的——否则「效果不好」会被误归因为「检索质量差」而不是「参数推断失败」。更完整的做法是把降级次数作为指标暴露出来。

**架构推论：** 「1-based 给人、0-based 给程序、转换在一处」是一个好约定，但它把**校验责任**留空了：转换点没有做范围检查，而下游直接用这个索引。这类「约定清晰但校验缺失」的组合最危险——因为约定让人放松警惕。同一个项目里三处（d01 的 `choice_idxs`、d02 的两处、本篇的 `RouterRetriever`）都是这个形态。

**架构推论：** `LLMSingleSelector` 与 `LLMMultiSelector` 强制「提示带输出解析器」，是把「解析」绑在「提示」上的设计。好处是二者不可配错；坏处是换模型（输出格式不同）必须换整个提示对象。对比 d02（langchain）里解析器与提示分离，两种都成立：那里提示是纯文本，解析是独立步骤；这里选择器把二者当成一体。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/base/base_auto_retriever.py` | `BaseAutoRetriever` | 两阶段检索抽象：生成 spec → 造检索器 → 执行 |
| `llama-index-core/llama_index/core/base/base_auto_retriever.py` | `BaseAutoRetriever._retrieve` / `_aretrieve` | 三行实现：生成、造、检索；异步路径混一个同步调用 |
| `llama-index-core/llama_index/core/base/base_selector.py` | `SingleSelection` | 单个选择：`index`（0-based）+ `reason` |
| `llama-index-core/llama_index/core/base/base_selector.py` | `MultiSelection` | 多选容器，`ind`/`reason` 在非单调时抛 `ValueError` |
| `llama-index-core/llama_index/core/base/base_selector.py` | `SelectorResult` | `MultiSelection` 的别名（为函数调用模型避免歧义） |
| `llama-index-core/llama_index/core/base/base_selector.py` | `BaseSelector` | 归一化入口 + 两个强制抽象实现（同步与异步） |
| `llama-index-core/llama_index/core/base/base_selector.py` | `_wrap_choice` / `_wrap_query` | `str` ↔ `ToolMetadata` / `QueryBundle` 的双态归一 |
| `llama-index-core/llama_index/core/indices/vector_store/retrievers/auto_retriever/auto_retriever.py` | `VectorIndexAutoRetriever` | 唯一实现：用 LLM 推出 `VectorStoreQuerySpec` |
| `llama-index-core/llama_index/core/indices/vector_store/retrievers/auto_retriever/auto_retriever.py` | `_parse_generated_spec` | 只捕 `OutputParserException`，兜底成默认 spec 并静默 |
| `llama-index-core/llama_index/core/indices/vector_store/retrievers/auto_retriever/auto_retriever.py` | `_build_retriever_from_spec` | 三重 clamp、空过滤器传 `None`、合并 `extra_filters` |
| `llama-index-core/llama_index/core/indices/vector_store/retrievers/auto_retriever/auto_retriever.py` | `_get_query_bundle` | 空查询 + 默认向量，让纯过滤可用 |
| `llama-index-core/llama_index/core/selectors/llm_selectors.py` | `_build_choices_text` | 1-based 编号渲染，描述换行折叠 |
| `llama-index-core/llama_index/core/selectors/llm_selectors.py` | `_structured_output_to_selector_result` | 1-based → 0-based 的唯一转换点 |
| `llama-index-core/llama_index/core/selectors/llm_selectors.py` | `LLMSingleSelector` / `LLMMultiSelector` | 强制提示带 `output_parser`；多选受 `max_outputs` 约束 |
| `llama-index-core/llama_index/core/selectors/utils.py` | `get_selector_from_llm` | 用 `try/except ValueError` 做 Pydantic vs 文本的能力协商 |
| `llama-index-core/llama_index/core/retrievers/router_retriever.py` | `RouterRetriever._retrieve` | 多选合并（后到覆盖）、无边界检查、空选择被包装成失败 |

## 参考

- [llama-index 源码：base/base_auto_retriever.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/base/base_auto_retriever.py)
- [llama-index 源码：base/base_selector.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/base/base_selector.py)
- [llama-index 文档：路由器与选择器](https://docs.llamaindex.ai/en/stable/module_guides/querying/router/)
- [llama-index 文档：向量索引自动检索](https://docs.llamaindex.ai/en/stable/examples/query_engine/vectorindex_auto_retriever/)

## 与主核 / AI 的关系

「先从自然语言推出结构化参数，再按参数执行」这件事与主核（多模态数据库内核）的**查询解析与重写**在结构上一致。主核从 SQL 文本推导出计划（谓词、连接、排序），而这里从自然语言推导出 `filters` / `top_k` / `query`。

关键差别是**可信度**。主核的解析器面对的是用户手写的、遵守语法的 SQL，所以「解析失败」意味着输入错，可以直接报错。这里面对的是 LLM 的输出，**必然**有一部分是不可解析或不合法的——所以必须有兜底。这个差别解释了为什么 `_parse_generated_spec` 选择降级而非报错：**它有你不能报告错误的理由，因为错误是被预期的。**

第二处对照是**「三重 clamp」与主核的配额机制**。主核有 `work_mem`、`statement_timeout`、`max_parallel_workers` 这些资源上限，而任何一层都不能越过它们——即使查询里写了 `SET work_mem = '10GB'`。这里的 `min(spec.top_k, max_top_k, similarity_top_k)` 是同一条纪律：**模型给的数只能往小调**。

第三处是**选择器的「无边界检查」在主核里对应「非法的对象号」**。主核的每一个对象引用（表 OID、列号、函数号）都在使用前校验存在性，因为「引用一个不存在的对象」会崩。这里 `self._retrievers[engine_ind]` 直接索引——把校验责任交给了上游的 LLM。**判据是同一个：凡是外部来源派生的引用，用之前必须校验。**

对 AI 工程的实际意义在于：「让模型决定怎么检索」是一个很实用的能力（用户不必手写过滤器），而它的工程要点全在**模型输出的不可信处理**上。这份实现给出的三条经验可直接用：**把模型的输出先经过一个程序化的翻译层**（在那里 clamp 与合并，模型就无法越权）、**降级要可用但也要可观测**（静默降级会让问题归因错位）、以及**序号必须校验范围**（1-based 转 0-based 之后是最容易漏的一处）。
