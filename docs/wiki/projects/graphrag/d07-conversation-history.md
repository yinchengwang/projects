---
project: graphrag
feature: conversation-history
title: 对话历史与问题生成
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/query/context_builder/conversation_history.py
    symbols: [ConversationHistory]
  - path: packages/graphrag/graphrag/query/context_builder/rate_relevancy.py
    symbols: [rate_relevancy]
  - path: packages/graphrag/graphrag/query/question_gen/local_gen.py
    symbols: [LocalQuestionGen]
---

# 对话历史与问题生成

## 一句话职责

把扁平的「轮次列表」压成「问答对」，再按**最近 N 轮**与 **token 预算**筛出能进上下文的那几轮；`rate_relevancy` 提供一个 0-5 的相关度打分原语（供 d06 的社区筛选使用）；`LocalQuestionGen` 则把同一个 local 上下文反过来用于**生成**问题而不是回答问题。

## 机制是什么

这一页的三件东西表面上属于「多轮对话」，实际各自解决不同问题：

**1. `ConversationHistory` 的核心是「问答对」这个聚合单位。**

原始输入是扁平的 `[{role, content}, ...]`，而「带进上下文的单位」不该是单个 turn——因为「用户问了什么」与「助手答了什么」必须一起出现才有意义。所以 `to_qa_turns()` 做一次聚合：**一个 USER turn 开启一个新的 `QATurn`，后续的 ASSISTANT turn 全部挂到它下面**。

这个聚合规则隐含一个假设：**对话总是「用户先说」**。若历史以助手消息开头（例如从某个中间状态接续），那条消息会被丢弃（因为 `current_qa_turn` 还是 `None`）。

**2. `rate_relevancy` 是一个独立的打分原语，而不是「历史筛选器」。**

它在仓库里**只有一个调用方**（d06 的 `DynamicCommunitySelection`），用来给社区报告打分。所以「用相关度筛历史」这件事**实际上没有实现**——历史筛选走的是「最近 N 轮 + token 预算」的简单截断。

这个差别值得写清楚，因为名字容易让人以为是「历史按相关度打分」。

**3. `LocalQuestionGen` 是「反向使用上下文」。**

`LocalSearch` 用上下文回答一个问题；`LocalQuestionGen` 用上下文**提出** N 个问题。两者共用同一个 `LocalContextBuilder` 与同一套数据，只是 prompt 与输出形态不同（`LocalSearch` 返回字符串，它返回 `list[str]`——按换行切分）。

这三件事被放在同一页，是因为它们共享一个前提：**上下文里除了「事实」，还可以放「对话」与「指令」**。

## 怎么实现的

### `ConversationHistory.to_qa_turns`：一次聚合，两个隐含假设

```python
def to_qa_turns(self) -> list[QATurn]:
    qa_turns = list[QATurn]()
    current_qa_turn = None
    for turn in self.turns:
        if turn.role == ConversationRole.USER:
            if current_qa_turn:
                qa_turns.append(current_qa_turn)
            current_qa_turn = QATurn(user_query=turn, assistant_answers=[])
        else:
            if current_qa_turn:
                current_qa_turn.assistant_answers.append(turn)  # type: ignore
    if current_qa_turn:
        qa_turns.append(current_qa_turn)
    return qa_turns
```

五处：

1. **`else` 分支同时处理 ASSISTANT 与 SYSTEM** —— 所以**系统消息被当作助手回答**。`ConversationRole` 有三个值，而这里只判了「是不是 USER」。

   这意味着一段以 `system` 开头的历史，那条系统消息会被挂到**前一个** `QATurn` 的 `assistant_answers` 里；若无前一个，则被丢弃。

2. **`turn.role == ConversationRole.USER` 用枚举比较** —— 而 `ConversationRole` 继承 `str`，所以 `"user" == ConversationRole.USER` 也为真。这是 `str`/`Enum` 混继承的常见用法。

3. **`if current_qa_turn:` 判真值** —— `QATurn` 是 dataclass，没有 `__bool__`，所以对象恒为真。当 `None` 时为假。所以这个守卫就是「有没有前一个 QA 对」。

4. **开头的助手消息被丢弃** —— 因为 `current_qa_turn` 是 `None` 时 `else` 分支的 `if` 不成立，什么也不做。

5. **末尾 `if current_qa_turn: qa_turns.append(...)`** —— 处理「最后一轮还没被下一条 USER 触发」的情形。

**注意 `assistant_answers=[]`（空列表）而不是 `None`** —— 初始化为空列表，所以 `turn.assistant_answers` 恒为列表（除非外部改成 `None`）。而 `build_context` 里会主动造 `assistant_answers=None` 的 QATurn：

```python
if include_user_turns_only:
    qa_turns = [
        QATurn(user_query=qa_turn.user_query, assistant_answers=None)
        for qa_turn in qa_turns
    ]
```

**所以 `assistant_answers` 的两种空值语义不同**：`[]`（没有回答）与 `None`（不关心回答）。而 `get_answer_text` 对两者都返回 `None`（因为 `if self.assistant_answers:` 对空列表为假）。

### `build_context`：三段筛选，而顺序决定了语义

```python
qa_turns = self.to_qa_turns()
if include_user_turns_only:
    qa_turns = [QATurn(user_query=qa_turn.user_query, assistant_answers=None) for qa_turn in qa_turns]
if recency_bias:
    qa_turns = qa_turns[::-1]
if max_qa_turns and len(qa_turns) > max_qa_turns:
    qa_turns = qa_turns[:max_qa_turns]
```

四处：

1. **`include_user_turns_only` 先裁剪内容** —— 它把 `assistant_answers` 置 `None`，于是只留问句。

   **所以 local search 的历史上下文只有用户问句**（d02 里 `conversation_history_user_turns_only=True` 硬编码）。而 global search 默认也是 `True`（d03 的 `build_context` 参数默认值）。

   **这是一处一致的设计**：不要让模型看到自己之前的回答——因为那些回答可能错，而重放错误答案会强化错误。

2. **`recency_bias` 反转列表** —— 默认 `True`，所以反转后「最新的在前」。

   而 d02 的 local search 与 d03 的 global search **都显式传 `recency_bias=False`**。所以实际使用时**没有反转**（最旧的在前）。

   **这是一处参数默认值与唯一两个调用方都不一致的情况**：默认 `True`，实际都传 `False`。所以那段反转代码在生产路径上从不执行。

3. **`max_qa_turns` 截断在反转之后** —— 所以：
   - `recency_bias=True`：取反转后的前 N 个 = **最近的 N 轮**。
   - `recency_bias=False`：取前 N 个 = **最早的 N 轮**。

   **「传 `False` 就变成取最早的 N 轮」几乎肯定不是调用方想要的。** local/global 都传 `False`，意味着它们拿到的是**最早**的 5 轮，而不是最近的 5 轮。

   **这是一处真实的语义陷阱**：`recency_bias` 这个参数名暗示「是否偏向最近的」，而实际作用是「选择最近还是最早」。若要「最近 N 轮且不反转顺序」，需要反转两次或改实现。

4. **`if max_qa_turns and ...`** —— `max_qa_turns=None` 或 0 时**不截断**（`and` 短路）。所以「不限轮数」的写法是传 `None` 或 `0`。

**然后是预算循环，而它的写法与其它渲染器都不同：**

```python
turn_list = []
current_context_df = pd.DataFrame()
for turn in qa_turns:
    turn_list.append({
        "turn": ConversationRole.USER.__str__(),
        "content": turn.user_query.content,
    })
    if turn.assistant_answers:
        turn_list.append({
            "turn": ConversationRole.ASSISTANT.__str__(),
            "content": turn.get_answer_text(),
        })

    context_df = pd.DataFrame(turn_list)
    context_text = header + context_df.to_csv(sep=column_delimiter, index=False)
    if tokenizer.num_tokens(context_text) > max_context_tokens:
        break

    current_context_df = context_df
context_text = header + current_context_df.to_csv(sep=column_delimiter, index=False)
```

六处：

1. **每轮都重建整个 DataFrame 并重新 `to_csv`** —— 因为要判断「加上这一轮后总量是多少」。这是 O(n²) 的渲染，与 d06 的渲染器（增量累加 token）不同。

   **d06 的做法是「累加 token 数」，这里是「重新渲染再数」**。后者更准确（因为 `to_csv` 可能改变转义），但更慢。

2. **`break` 之后用 `current_context_df`（上一轮的）** —— 所以「放不下的那一轮被整个丢掉」，不是部分保留。

3. **`current_context_df` 初值是空 `DataFrame`** —— 若第一轮就超预算，`break` 后 `context_text = header + 空df.to_csv(...)`。而空 DataFrame 的 `to_csv` 返回 `"\n"`（只有列名的空表会返回空串）。

   **所以「连一轮都放不下」时返回的是 `header + "\n"`** —— 一个只有标题行的空表。而调用方（d02）判 `if conversation_history_context.strip() != ""`，此时它非空（有标题行），于是**会把一个空表当作有效历史**并扣掉它的 token。

4. **`ConversationRole.USER.__str__()`** —— 调实例的 `__str__`（不是类）。所以得到 `"user"`。**这是很绕的写法**：直接写 `"user"` 或用 `ConversationRole.USER.value` 都更清楚。

5. **`if turn.assistant_answers:` 判真值** —— `None` 与 `[]` 都为假，所以 `include_user_turns_only=True` 时不会追加助手行。

6. **表头只有两列 `turn`/`content`** —— 没有「第几轮」的列。所以模型看到的是 `user|问题` 与 `assistant|回答` 交替的行，靠顺序理解。

**早期返回的键名不一致：**

```python
if len(qa_turns) == 0 or not qa_turns:
    return ("", {context_name: pd.DataFrame()})
```

**注意是 `{context_name: ...}`**（原始大小写 `"Conversation History"`），而正常返回是 `{context_name.lower(): ...}`（`"conversation history"`）。

**这是一处真实的键名不一致**：空历史时字典键是 `"Conversation History"`，非空时是 `"conversation history"`。

而 d02 的 local search 用 `final_context_data = conversation_history_context_data`（直接赋值）——若拿到的是空历史，`final_context_data` 会有大写键的 `DataFrame`，而后续 `{**final_context_data, **community_context_data}` 会保留它。所以**最终 `context_records` 里可能出现两种大小写的键**。

（d02 里还有 `if conversation_history_context.strip() != ""` 的守卫，空历史时不会赋值——所以实际触发需要「历史非空但产出了空文本」，这在 `max_context_tokens` 极小时可能。）

**`len(qa_turns) == 0 or not qa_turns` 是冗余的** —— 两个条件等价（列表的 `not` 就是 `len == 0`）。

### `get_user_turns`：逆序与截断

```python
def get_user_turns(self, max_user_turns: int | None = 1) -> list[str]:
    """Get the last user turns in the conversation history."""
    user_turns = []
    for turn in self.turns[::-1]:
        if turn.role == ConversationRole.USER:
            user_turns.append(turn.content)
            if max_user_turns and len(user_turns) >= max_user_turns:
                break
    return user_turns
```

三处：

1. **`self.turns[::-1]` 逆序遍历** —— 从最新往后。所以返回的列表是**逆时间序**（最新的在前）。

2. **默认 `max_user_turns=1`** —— 只取最近一条。而 d02 的 local search 传的是 `conversation_history_max_turns`（默认 5）。

3. **返回顺序是「最新在前」** —— 而 d02 把它拼进 query：

```python
pre_user_questions = "\n".join(conversation_history.get_user_turns(conversation_history_max_turns))
query = f"{query}\n{pre_user_questions}"
```

   **所以 `query` 的结构是「当前问题 \n 最近的历史问题 \n 更早的历史问题...」** —— 历史部分逆序。

   这大概不是有意的，但因为历史只是给检索提供上下文词，逆序影响不大。**不过它对生成有影响**：模型看到的 user message 里历史是倒着排的。

### `rate_relevancy`：0-5 打分 + 众数投票

```python
messages_builder = (
    CompletionMessagesBuilder()
    .add_system_message(rate_query.format(description=description, question=query))
    .add_user_message(query)
)

for _ in range(num_repeats):
    async with semaphore if semaphore is not None else nullcontext():
        model_response = await model.completion_async(
            messages=messages_builder.build(), response_format_json_object=True, **model_params,
        )
        response = await gather_completion_response_async(model_response)
    try:
        _, parsed_response = try_parse_json_object(response)
        ratings.append(parsed_response["rating"])
    except KeyError:
        logger.warning("Error parsing json response, defaulting to rating 1")
        ratings.append(1)
    ...
options, counts = np.unique(ratings, return_counts=True)
rating = int(options[np.argmax(counts)])
```

四处（这一页补 d06 未展开的两点）：

1. **`RATE_QUERY` 实际说的是 0 到 5**，而 `rate_relevancy` 的 docstring 说「on a scale of 0 to 10」。**文档与 prompt 不一致**。

   而 `DynamicCommunitySelection` 的 `threshold` 默认是 1。所以「0-5 量表 + 阈值 1」意味着「只要不是 0 分就展开」——**阈值非常宽松**。

2. **`messages_builder` 复用** —— 循环外构造一次，循环内多次 `build()`。所以多次评分（`num_repeats > 1`）时 prompt 完全相同。

   **所以投票投的是「模型自身的随机性」**，不是「同一问题的不同表述」。若 `temperature=0`，多次调用结果相同，投票毫无意义。

3. **`try_parse_json_object` 的失败信号是 `{}`**，而 `parsed_response["rating"]` 对空字典抛 `KeyError` —— 所以异常路径确实能捕获「解析失败」。

4. **众数用 `np.unique` 后 `argmax`** —— `options` 是排序后的唯一值，`argmax(counts)` 在平票时返回**第一个**，即**最小值**。

   所以评分不确定时倾向给**低分**（不展开）。这与「解析失败默认 1」（倾向展开）方向相反——但两者处理的是不同情形。

### `LocalQuestionGen`：两个几乎完全相同的方法

```python
async def agenerate(self, question_history, context_data, question_count, **kwargs) -> QuestionResult:
    ...
async def generate(self, question_history, context_data, question_count, **kwargs) -> QuestionResult:
    ...
```

**两个方法的实现逐行相同**，唯一差别是两处 `logger.exception` 的消息文本（"Exception in generating question" vs "Exception in generating questions"——少个 s）。

**而两者都是 `async def`** —— 所以 `generate` 与 `agenerate` 在调用方式上完全一样。而基类 `BaseQuestionGen` 把两者都声明为 `@abstractmethod async def`。

**这是一处纯粹的历史遗留**：某次重构把同步方法改成了异步，但保留了旧名字。**代价是每个子类要写两份，且两者可能漂移**（这里已经漂移了——一条日志少个 s）。

**`question_history` 的处理值得看：**

```python
if len(question_history) == 0:
    question_text = ""
    conversation_history = None
else:
    question_text = question_history[-1]
    history = [{"role": "user", "content": query} for query in question_history[:-1]]
    conversation_history = ConversationHistory.from_list(history)
```

四处：

1. **最后一条当「当前问题」，其余当历史** —— 所以 `question_history` 的语义是「问题序列」，而生成的是「下一个问题」。

2. **历史全是 `role: "user"`** —— 因为这里只有问题，没有答案。所以 `from_list` 建出的历史里 `assistant_answers` 都是 `[]`。

3. **`conversation_history = None`（不是空对象）** —— 空历史时传 `None`，而 `build_context` 里判 `if conversation_history:` —— **空对象也是真值**（`ConversationHistory` 没定义 `__bool__`/`__len__`），所以必须传 `None` 才算「无历史」。

   **这是一处必须靠调用方传对值的设计**：若传一个 `ConversationHistory()`（`turns=[]`），`build_context` 会认为「有历史」，然后 `to_qa_turns()` 返回 `[]`，最后走到那个键名不一致的早退分支。

4. **`question_text` 为空串时** —— `context_builder.build_context(query="")`。而 `map_query_to_entities` 会用空串去检索向量库。**这是一处未防御的路径**（对比 d05 的 basic search 有 `if query != ""` 守卫）。

**`build_context` 的调用对 `kwargs` 与 params 的合并顺序与 d02 一致：**

```python
result = self.context_builder.build_context(
    query=question_text,
    conversation_history=conversation_history,
    **kwargs,
    **self.context_builder_params,
)
```

**所以构造时的参数覆盖调用时的**（与 d02 相同的陷阱）。

**而 `context_data` 参数允许跳过上下文构建：**

```python
if context_data is None:
    result = self.context_builder.build_context(...)
    context_data = result.context_chunks
    context_records = result.context_records
else:
    context_records = {"context_data": context_data}
```

**这是「让调用方复用一份已构建的上下文」的能力** —— 例如对同一批文档生成多个问题时不必重复检索。而 `context_records` 在跳过路径上被造成 `{"context_data": ...}`（一个不同的形状）。

**prompt 与输出：**

```python
system_prompt = self.system_prompt.format(context_data=context_data, question_count=question_count)
...
return QuestionResult(
    response=response.split("\n"),
    context_data={"question_context": question_text, **context_records},
    completion_time=...,
    llm_calls=1,
    prompt_tokens=self.tokenizer.num_tokens(system_prompt),
)
```

五处：

1. **`response.split("\n")`** —— 所以「问题列表」是从模型输出按换行切出来的。**没有任何格式校验**：模型若输出编号列表（`1. ...`），切出来会带编号；若输出一段话，切出来是一条。

   **这依赖 prompt 明确要求「每行一个问题」**（`QUESTION_SYSTEM_PROMPT` 里应该有这个约束），但没有代码层的保障。

2. **`{"question_context": question_text, **context_records}`** —— 把「生成时用的最后一个问题」也放进 `context_data`。而 `context_records` 是 `dict[str, DataFrame]`，所以最终是一个混合形状的字典（一个 `str` + 若干 `DataFrame`）。

   **这破坏了 `context_data` 的类型一致性**（`SearchResult.context_data` 声明的是 `str | list[DataFrame] | dict[str, DataFrame]`）。而 `QuestionResult.context_data` 的类型是 `str | dict[str, Any]` —— **它没有跟着更新**。

3. **`prompt_tokens=self.tokenizer.num_tokens(system_prompt)`** —— 只算 system prompt，不含 user message（`question_text`）。与 d02/d05 相同的偏差。

4. **`llm_calls=1`** —— 但 `build_context` 也可能调 LLM（若上下文构建器做动态选择）。那些调用被漏计。

5. **异常路径也返回 `llm_calls=1`** —— 与 d02 相同的问题。

## 关键取舍

**源码事实：** `to_qa_turns` 用一个 USER turn 开启新 `QATurn`，其它 role 全部挂为 `assistant_answers`——所以 **SYSTEM 消息被当助手回答**，且**开头的助手消息被丢弃**（`current_qa_turn` 为 `None`）。

**源码事实：** `build_context` 的三段顺序是「裁剪内容（`include_user_turns_only` 置 `assistant_answers=None`）→ 可选反转（`recency_bias`）→ 截断（`max_qa_turns`）」。所以 `recency_bias=True` 取最近 N 轮，`False` 取**最早** N 轮。

**源码事实：** `recency_bias` 默认 `True`，但**唯一的两个调用方（local search、global search）都显式传 `False`**——所以反转代码在生产路径上从不执行，而它们实际拿到的是最早的 5 轮而非最近的 5 轮。

**源码事实：** 预算判断每轮**重建整个 DataFrame 并重新 `to_csv`** 再数 token（O(n²) 渲染），而不是像 d06 的渲染器那样增量累加；超预算 `break` 后丢弃整轮。

**源码事实：** 「连一轮都放不下」时返回 `header + 空df.to_csv()`（标题行加空串），**非空字符串**。所以调用方 `if ...strip() != ""` 的守卫会通过，把这个空表当有效历史并扣掉 token。

**源码事实：** 早退分支返回 `{context_name: ...}`（原始大小写），正常返回 `{context_name.lower(): ...}`——**两处键名大小写不一致**。且早退条件是 `len(qa_turns) == 0 or not qa_turns`（两个等价条件）。

**源码事实：** 用 `ConversationRole.USER.__str__()` 取字符串（绕）；`ConversationRole` 继承 `str, Enum` 所以 `"user" == ConversationRole.USER` 为真。

**源码事实：** `get_user_turns` 逆序遍历 `self.turns[::-1]`，所以返回列表**最新在前**；默认 `max_user_turns=1`。d02 把它 `"\n".join` 拼进 query，所以 `query` 里的历史部分是**逆时间序**。

**源码事实：** `RATE_QUERY` 的 prompt 说「On a scale from 0 to 5」，而 `rate_relevancy` 的 docstring 说「0 to 10」——**文档与 prompt 不一致**；`DynamicCommunitySelection` 的 `threshold` 默认 1，所以「0-5 量表 + 阈值 1」等于「只要不是 0 分就展开」。

**源码事实：** `num_repeats > 1` 时多次调用的 prompt **完全相同**（`messages_builder` 在循环外构造），所以投票投的是模型自身随机性；`temperature=0` 时投票无意义。众数用 `np.unique` + `argmax`，平票取**较小值**（保守不展开），与「解析失败默认 1」（保守展开）方向相反但处理不同情形。

**源码事实：** `LocalQuestionGen` 的 `generate` 与 `agenerate` **逐行相同**（唯一差别是一条日志少个 `s`），且**都是 `async def`**——历史遗留，代价是两份可能漂移。

**源码事实：** `question_history` 的最后一条当「当前问题」、其余全当 `role: "user"` 的历史；空历史时传 `conversation_history = None` 而**不能传空 `ConversationHistory()`**（后者是真值，会让 `build_context` 走「有历史」分支并命中键名不一致的早退）。

**源码事实：** `question_text` 为空串时仍会 `build_context(query="")`——`map_query_to_entities` 会拿空串检索（对比 d05 有 `if query != ""` 守卫）。

**源码事实：** `context_data` 参数允许跳过上下文构建（复用已建好的），此时 `context_records = {"context_data": context_data}`（另一种形状）。

**源码事实：** 输出 `response=response.split("\n")`，**无格式校验**——依赖 prompt 约束「每行一个问题」；`context_data={"question_context": question_text, **context_records}` 把 `str` 与 `DataFrame` 混在一个字典里，破坏了 `SearchResult.context_data` 声明的类型一致性，而 `QuestionResult.context_data` 的类型标注（`str | dict[str, Any]`）没有跟着更新。

**源码事实：** `prompt_tokens` 只算 system prompt（不含 user message），`llm_calls` 硬编码 1（漏计 `build_context` 内部的 LLM 调用）——与 d02/d05 相同的偏差。

**架构推论：** 「历史按最近 N 轮截断」与「历史按相关度筛选」是两种不同的策略，而这份实现选了前者、留了后者的原语（`rate_relevancy`）却没用在同一处。**判据是「相关度信号是否值得它的成本」**：给历史打分要为每一轮调用一次 LLM，而历史的轮数本来就不多（5-10 轮），所以「全带最近 N 轮」更划算。`rate_relevancy` 用在社区报告上是合理的——因为那里的候选集可能有几千个，筛选才有价值。

**架构推论：** `recency_bias` 的默认值与唯一两个调用方都不一致，这几乎肯定是**参数语义演变留下的残迹**：原本可能是「是否反转」，后来被当成「是否偏向最近」。而 `False` 时取「最早 N 轮」这个副作用没被注意到——**因为最早的 5 轮在短对话里就是全部**，所以小规模测试无法暴露。

**架构推论：** `generate`/`agenerate` 两份逐行相同的实现说明「异步化」是一次机械改动。这类改动的风险不在当时，而在之后：**任何一处修改只改一份就会静默分叉**。而已经发生的分叉（日志少个 `s`）恰好证明了这个风险。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/query/context_builder/conversation_history.py` | `ConversationRole` | 三种角色的 `str` 枚举 |
| `packages/graphrag/graphrag/query/context_builder/conversation_history.py` | `ConversationTurn` | 单条轮次 |
| `packages/graphrag/graphrag/query/context_builder/conversation_history.py` | `QATurn` | 问 + 多个答的聚合单位 |
| `packages/graphrag/graphrag/query/context_builder/conversation_history.py` | `ConversationHistory` | 轮次容器 |
| `packages/graphrag/graphrag/query/context_builder/conversation_history.py` | `ConversationHistory.to_qa_turns` | 扁平轮次 → 问答对（SYSTEM 当回答、开头回答被丢） |
| `packages/graphrag/graphrag/query/context_builder/conversation_history.py` | `ConversationHistory.build_context` | 裁剪 → 反转 → 截断 → O(n²) 预算判断 |
| `packages/graphrag/graphrag/query/context_builder/conversation_history.py` | `ConversationHistory.get_user_turns` | 逆序取最近 N 条问句 |
| `packages/graphrag/graphrag/query/context_builder/rate_relevancy.py` | `rate_relevancy` | 0-5 打分 + 众数投票 + `KeyError` 兜底为 1 |
| `packages/graphrag/graphrag/query/context_builder/rate_prompt.py` | `RATE_QUERY` | 打分 prompt（0-5，与 docstring 的 0-10 不一致） |
| `packages/graphrag/graphrag/query/question_gen/local_gen.py` | `LocalQuestionGen` | 用 local 上下文生成问题 |
| `packages/graphrag/graphrag/query/question_gen/local_gen.py` | `LocalQuestionGen.agenerate` / `generate` | 两份逐行相同的实现 |
| `packages/graphrag/graphrag/query/question_gen/base.py` | `BaseQuestionGen` | 问题生成的抽象基类 |
| `packages/graphrag/graphrag/query/question_gen/base.py` | `QuestionResult` | 结果信封（`response` 是 `list[str]`） |
| `packages/graphrag/graphrag/prompts/query/question_gen_system_prompt.py` | `QUESTION_SYSTEM_PROMPT` | 每行一个问题的输出约定 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：问题生成](https://microsoft.github.io/graphrag/query/question_generation/)
- [graphrag 论文（Edge et al., 2024）](https://arxiv.org/abs/2404.16130)
- [论文：多轮对话检索中的历史压缩](https://arxiv.org/abs/2305.14283)

## 与主核 / AI 的关系

「历史按最近 N 轮截断」与主核（多模态数据库内核）的**环形缓冲区**是同一类策略：容量有限时按时间丢弃最旧的。而两者的判据不同：主核丢最旧是因为**新的更可能有用**（时序数据、日志）；多轮对话里「最新 N 轮」也有同样假设，但它**在「用户先问 A、追问细节、然后回到 A」这种模式上会失效**——A 那轮可能已超出窗口。

这正是「按相关度筛选」本该解决的问题，而这份实现只留了原语没用上。**判据是「会话的引用模式是否跨窗口」**：短会话（≤5 轮）不需要，长会话或多话题会话需要。

第二处对照是 `to_qa_turns` 的「USER 开启新组」与主核的**分组聚合**（`GROUP BY` 的隐式分组）。两者都是「按某个标记切分序列」，而差别在于**主核要求标记有序且可比较，这里只要求「遇到标记就切」**。所以这里对「连续两个 USER 没有 ASSISTANT」天然容忍（前一个 QATurn 的 `assistant_answers` 为空列表），而 SQL 的分组语义里没有对应概念。

第三处是 `generate`/`agenerate` 的双份实现与主核里**同一功能的多个入口**（`ExecInsert` / `ExecInsertIndexTuples`）。数据库里这类重复通常有的是「一个给执行器、一个给工具」，有的是历史遗留。而**判据是「两个入口是否有不同的调用约束」**——这里两者都是 `async` 且签名相同，所以纯属冗余。

对 RAG 工程的实际意义在于：多轮对话是 RAG 最常见的需求，而它的实现往往轻到让人忽略陷阱。这份实现给出的三条经验可直接用：**「最近 N 轮」不等于「相关 N 轮」**（跨窗口引用会失效，需要打分或摘要）、**`recency_bias` 这类参数要检查它是否与所有调用方一致**（默认值与实际使用相反说明语义已漂移）、以及**模型的输出格式必须校验**（`split("\n")` 依赖 prompt 自觉，编号、空行、多行答案都会破坏它）。
