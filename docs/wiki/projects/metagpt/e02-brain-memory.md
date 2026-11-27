---
project: metagpt
feature: brain-memory
title: 脑记忆与短期上下文压缩
page_type: feature
schema_version: 1
refs:
  - path: metagpt/memory/brain_memory.py
    symbols: [BrainMemory, Config]
---

# 脑记忆与短期上下文压缩

## 一句话职责

`BrainMemory` 是给多轮对话场景准备的一层会话级记忆：它把历史消息与增量知识分开保存、用 Redis 做跨请求缓存、并在历史超长时把「更早的对话」折叠成一段摘要，只保留最近若干字符的原文。

## 机制是什么

一个长时间对话的进程模型是这样的：用户每说一句，服务端加载这段会话的历史、拼提示词、调模型、把回答写回历史。历史无限增长，于是必须截断，但直接截断会丢掉「很久以前说过什么」这件事。`BrainMemory` 的答案是两级表示：

- `history`：最近若干条消息的原文，按字符预算裁剪。
- `historical_summary`：更早内容的压缩摘要。

两者拼起来构成完整上下文。另有 `knowledge` 字段存与当前请求相关的增量知识（例如检索到的片段），它与对话历史分开维护，取用时用 `get_knowledge` 拼成一段文本。

`last_history_id` 是这套机制的另一个支点。它记录已入库的最大消息 id，`add_history` 据此丢弃「比已入库更旧」的消息。

## 怎么实现的

**两个入口方法的角色改写。** `add_talk` 把消息 `role` 强制改成 `user` 再入库，`add_answer` 改成 `assistant`。也就是说 caller 不需要关心消息来自哪儿，只要选对入口。两者都会置 `is_dirty = True`。

**`add_history` 的单调过滤。** 若消息带 `id`，就比较它与 `last_history_id`：`to_int(msg.id, 0) <= to_int(self.last_history_id, -1)` 时直接返回。默认值选取很讲究——消息 id 转不成整数时按 `0` 处理，`last_history_id` 转不成时按 `-1` 处理。因此「非数字 id 的消息」与「空 `last_history_id`」相比总是通过；而严格递增的数字 id 会被强制单调。这条规则的用途是应对「并发写入或重放导致同一批消息被提交两次」。

`to_int` 用裸 `except` 吞掉所有异常，非法输入一律降级成默认值——这是刻意宽松，不走抛出路线。

**`is_dirty` 是唯一的落盘闸门。** `dumps` 一进来就 `if not self.is_dirty: return`，不做序列化、不碰 Redis。反之只有真正改过历史才写。写完置回 `False`。

**`loads` 与 `dumps` 的键。** 键由 `to_redis_key(prefix, user_id, chat_id)` 拼成 `prefix:user_id:chat_id` 三段。`loads` 在键为空时直接返回一个全新的空 `BrainMemory`——而不是报错。`loads` 从 Redis 取到值后 `parse_raw` 反序列化，并把 `is_dirty` 强制置 `False`：刚从盘上读回来的东西不该被当作脏数据。

`dumps` 里还有一个 `cacheable` 开关：为假时不写 Redis，但仍然清 `is_dirty`。这表示「本次运行不要缓存这个会话」，与「没有改动」是两回事。

**`set_history_summary` 的双分支。** 若传入摘要与当前摘要相同：只在 `is_dirty` 时补一次 `dumps`（把可能未落盘的尾部写出去），然后返回。若不同：更新摘要、**清空 `history`**、立即 `dumps`。清空是必须的——摘要取代了原文，若两者并存，上下文里同一件事会出现两遍。

**两种摘要策略的分叉。** `summarize` 一进门就按 llm 类型分流：

- `MetaGPTLLM` 走 `_metagpt_summarize`：**不调模型**，纯本地裁剪。它从最新消息往前累加字符数，累到 `max_words` 为止，超出预算的那条被截断到剩余额度，然后反转回正序、覆盖 `self.history`、落盘，最后返回 `to_metagpt_history_format` 的 JSON 字符串。这条路径本质是「滑动窗口」，之所以挂在 `summary` 名下，是为了让接口对两种后端一致。
- 其他 llm 走 `_openai_summarize`：先把 `historical_summary` 与全部 `history` 拼成一个长文本，若 `limit` 给定且文本短于 limit 就直接原样返回（不摘要），否则调 `_summarize` 做真正的递归摘要，成功后 `set_history_summary` 落盘。

**`_summarize` 的分而治之。** 文本超过 `max_token_count` 时切成若干滑窗，每窗各摘要一份，把摘要们拼接后作为新文本再判长度，循环最多 100 轮（`max_count` 是防死循环的保险），直到拼一次就能收进 `max_token_count`。每轮把 `max_words` 按窗口数均摊（`int(max_words / len(text_windows)) + 1`，上限 100），所以越深的轮次单份摘要越短。

`split_texts` 的滑动窗口有一个 `padding_size`：窗口大小 `window_size`、步长 `window_size - padding_size`，因此相邻窗口重叠 `padding_size` 个字符。注释举了 `window_size=3, padding_size=1` 的例子，重叠是为了避免把句子从中间切断导致语义丢失。

**`get_title` 复用摘要。** 它先 `summarize(max_words=500)` 拿一份摘要，再把「把上面的摘要翻成不超过 N 词的语言 X 标题」作为指令发一次模型调用。`MetaGPTLLM` 分支则直接取 `history[0].content` 当标题——没有历史时返回 `"New"`。

**`is_related` / `rewrite` 也是一分为二。** `MetaGPTLLM` 下分别返回 `False` 与原文（即不做判断、不做改写），其他后端下各发一次带固定 system prompt 的调用。`is_related` 靠响应里是否含 `"TRUE"` 判定，大小写敏感、不做解析。

**上下文拼装的两个属性。** `is_history_available` 是 `history or historical_summary` 的布尔化。`history_text` 先放摘要、再拼 `history[:-1]` 的全部内容——**故意排除最后一条**，因为最后一条通常是当前待回答的问题，会由调用方单独放进提示词。它还容忍元素是 dict 的形态（反序列化后可能出现），用 `Message(**m)` 重新解析。

**`exists` 从后往前找。** 逐条比较 `content` 全等，找到即返回真。倒序遍历是个小优化：最近的消息更可能命中。

## 关键取舍

**源码事实：** `BrainMemory` 的 `llm` 字段标了 `exclude=True`，不参与序列化。模型客户端是运行期资源，不该随会话状态往返 Redis。

**源码事实：** `MetaGPTLLM` 分支的 `summarize` 不产生任何摘要文本，只是把 `history` 裁剪到字符预算并返回 JSON。同一方法名在两种后端下做的是不同的事（裁剪 vs 压缩），接口一致但语义不等价。这是为了让上层不强依赖具体后端。

**源码事实：** `_openai_summarize` 在摘要失败时抛 `ValueError(f"text too long:{text_length}")`，而 `_summarize` 的 100 轮循环耗尽时返回空字符串——两个失败路径的兜底不同，前者抛、后者静默返回空。空字符串会被 `if summary:` 判假，于是继续走到抛错分支。

**源码事实：** `add_history` 用整数比较做单调过滤，对非数字 id 完全放行。因此「id 是 uuid 的会话」享受不到这层保护，重复提交的旧消息会被原样追加。

**架构推论：** 摘要与原文的切换是原子的：`set_history_summary` 要么只补落盘（摘要未变），要么同时更新摘要并清空原文并落盘。不存在「摘要已更新但原文还在」的中间态，因此下游拼上下文时不会重复。代价是摘要一旦生成，原文就不可恢复。

**架构推论：** 整个类的持久化边界是 `is_dirty` 一个布尔。任何绕过 `add_talk`/`add_answer`/`add_history` 直接改 `history` 的代码，若忘了置 `is_dirty`，改动就不会落盘；反之任何无意义的置位都会触发一次全量 `model_dump_json`。这是「用单一脏标记换实现简洁」的典型代价。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/memory/brain_memory.py` | `BrainMemory` | 会话级记忆：原文窗口 + 摘要 + 增量知识 |
| `metagpt/memory/brain_memory.py` | `BrainMemory.add_history` | 按 id 单调过滤后追加，置脏 |
| `metagpt/memory/brain_memory.py` | `BrainMemory.dumps` | 脏标记门控的 Redis 写入 |
| `metagpt/memory/brain_memory.py` | `BrainMemory.loads` | 按会话键读取并反序列化，强制清脏 |
| `metagpt/memory/brain_memory.py` | `BrainMemory.set_history_summary` | 摘要更新的原子切换：清原文并立即落盘 |
| `metagpt/memory/brain_memory.py` | `BrainMemory.summarize` | 按后端分叉：本地裁剪或模型压缩 |
| `metagpt/memory/brain_memory.py` | `BrainMemory._summarize` | 滑窗 + 递归摘要，100 轮保险 |
| `metagpt/memory/brain_memory.py` | `BrainMemory.split_texts` | 带重叠的定长滑窗切分 |
| `metagpt/memory/brain_memory.py` | `BrainMemory.history_text` | 摘要加历史（排除最后一条）的拼装 |
| `metagpt/memory/brain_memory.py` | `Config` | pydantic 内部配置：允许任意类型、默认注入全局配置 |

## 参考

- [MetaGPT 源码：memory/brain_memory.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/memory/brain_memory.py)
- [MetaGPT 源码：utils/redis.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/utils/redis.py)
- [LangChain 文档：对话摘要记忆](https://python.langchain.com/docs/versions/migrating_memory/long_term_memory_agent/)

## 与主核 / AI 的关系

「原文窗口 + 摘要」与主核的「热数据页 + 冷数据归档」是同一种空间换时间的取舍：主核把最近访问的页留在缓冲池，把冷页交给更慢的介质；这里把最近的对话留成原文，把更早的折叠成摘要。两者的分界都由一个可调阈值决定（主核是缓冲池大小，这里是字符预算），两者都必须回答「归档后能否无损还原」——主核答能（有页副本），这里答不能。

对 agent 工程的实际含义是明确的：`BrainMemory` 适合「服务端多轮客服」这种会话，因为它的持久化单元就是一次会话（`user_id:chat_id`）。若把它用于长任务 agent，摘要会不断吞掉早期决策依据，且一旦摘要生成原文即不可恢复——需要可回溯的场景应改用向量化的长期记忆。

