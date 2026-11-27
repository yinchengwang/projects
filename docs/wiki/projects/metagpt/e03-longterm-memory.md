---
project: metagpt
feature: longterm-memory
title: 长期记忆与向量检索
page_type: feature
schema_version: 1
refs:
  - path: metagpt/memory/longterm_memory.py
    symbols: [LongTermMemory]
  - path: metagpt/memory/role_zero_memory.py
    symbols: [RoleZeroLongTermMemory]
---

# 长期记忆与向量检索

## 一句话职责

长期记忆给「只增不减」的基础 `Memory` 加了一层外迁与召回：超过短期容量后，把滑出窗口的旧消息写进向量库；取回上下文时，用当前需求去向量库捞相关旧消息，与最近的短期记忆合并返回。

## 机制是什么

这套机制有两个实现，走的是两条完全不同的技术路线，但解决同一个问题：

| 实现 | 存储后端 | 外迁时机 | 召回触发 | 召回结果 |
|---|---|---|---|---|
| `LongTermMemory` | FAISS（经 `MemoryStorage`） | 每次 `add` 都尝试 | `find_news` 时逐个做相似度过滤 | 只用于**剔除**重复新闻 |
| `RoleZeroLongTermMemory` | Chroma（经 `SimpleEngine`） | `add` 且短期已超容量 | `get` 满足三个条件时 | 与短期记忆**合并**返回 |

第一条路线是「去重」：长期库里存的是「见过的东西」，作用是判断一条新消息是不是旧的，因此它只参与过滤，不产生额外的上下文。第二条路线是「补充」：长期库里存的是「已经滑出窗口的历史」，作用是把它捞回来当上下文，因此召回的条目会被前置拼进结果。

## 怎么实现的

**`LongTermMemory` 的三处覆写。** 它继承 `Memory`，只改三个方法，其余全部沿用：

- `add`：先调父类入短期库，再对 `rc.watch` 里的每个动作检查 `message.cause_by == action`，命中且不在恢复状态（`msg_from_recover` 为假）时写进 `memory_storage`。因此只有「角色关心的那几类动作」的产出入长期库，用户直发消息不入。`msg_from_recover` 只覆盖 `recover_memory` 里的一小段同步代码，用于避免恢复过程本身触发二次写入。
- `find_news`：先拿父类的短期新闻，若 `memory_storage` 尚未初始化就直接返回；否则对每条候选做 `search_similar`，只有**一条相似结果都没有**的才留下。所以这一步的输出是「短期没见过、长期也没见过」的消息。
- `clear`：父类清空之外再 `memory_storage.clean()`，把磁盘目录整个删掉。而 `delete` 只调父类，源码在注释里留了 `TODO`——单条删除不会同步长期库，这是明确的未完成项。

**`MemoryStorage` 的初始化是惰性的、由目录内容决定。** `recover_memory` 把角色数据目录算出来并建好，然后看目录里有没有 `default__vector_store.json`：有就从已有索引加载（`SimpleEngine.from_index`），没有就新建一个空引擎。所以「首次运行」与「续跑」的分界就是这一个文件是否存在。它在两种情况下都构造 FAISS 索引与 FAISS 检索器，并把同一个 embedding 对象传进去。

**相似度过滤的方向需要特别注意。** `search_similar` 对检索结果的条件是 `item.score < self.threshold`（阈值 0.1）才收进结果。FAISS 检索返回的 score 是相似度、越大越像，按这个条件保留的是**最不相似**的那些。无论原作者意图是「低于阈值才算够像」还是「按距离理解」，这段代码与其注释「filter the result which score is smaller than the threshold」「filter similar memories」在语义上并不自洽。读这份源码时必须知道这一点：`find_news` 的过滤行为依赖这个方向，不要据注释推断。

另外 `search_similar` 声明了 `k=4` 参数但完全没用它——检索走的是引擎自己的 `similarity_top_k`。

**`RoleZeroLongTermMemory` 的容量与门控。** 它有四个可配参数：短期容量 `memory_k`（默认 200）、召回条数 `similarity_top_k`（默认 5）、是否用 LLM 重排 `use_llm_ranker`、以及持久化目录与集合名。三个门控条件决定「这一次要不要碰长期库」：

- 写：`count() > memory_k` 才外迁。
- 读：`k != 0` **且** 最后一条消息来自用户需求 **且** `count() > memory_k`，三个条件全真才召回。

读的门控是合取而非析取，所以最苛刻的一条是「最后一条必须是用户需求」——它由 `_is_last_message_from_user_requirement` 判断，条件是「是用户消息」并且（`cause_by` 是 `UserRequirement` 或 `sent_from` 是团队负责人）。只有这种情况才值得花一次向量检索。

**外迁挪的是窗口外的那一条。** `_get_longterm_memory_item` 取下标 `-(memory_k + 1)`，即「刚好滑出最近 k 条窗口的那一条」，用 `get_by_position` 容错取回，包成 `LongTermMemoryItem`。注意 `add` 每次只挪一条，因此短期库的规模会稳定在 `memory_k + 1` 附近波动，而不是一次性批量归档。

**召回是「前置 + 时间序」。** `get` 先用父类拿最近 k 条，再判定是否召回；召回时以「最后一条消息的内容」作为查询串（`_build_longterm_memory_query`），调 `rag_engine.retrieve`，从每个节点的 `metadata["obj"]` 取回 `LongTermMemoryItem`，按 `created_at` 排序，最后拼成 `related + memories`——长期记忆排在短期之前，符合「更旧的内容在前」的阅读顺序。

**RAG 引擎是懒加载的。** `rag_engine` 属性在第一次访问时才 `_resolve_rag_engine`，后者在方法体内部 import `metagpt.rag`，导入失败就抛一个带「需要安装 rag 模块」提示的 `ImportError`。这样「不用长期记忆的部署」不会被 rag 依赖拖死，代价是配置错误要等到第一次真正读写时才暴露。

**异常兜底的分层。** `_add_to_longterm_memory` 与 `_fetch_longterm_memories` 都挂了 `handle_exception`，前者失败只记日志、后者失败返回空列表。也就是说长期记忆的故障被完全隔离在「少一点历史」这个后果上，不会打断主流程。

## 关键取舍

**源码事实：** `LongTermMemory` 的长库只收 `rc.watch` 里那几类动作的消息，`RoleZeroLongTermMemory` 收的是所有滑出窗口的消息。两者的筛选维度不同：前者按「来源动作」筛，后者按「时间新旧」筛。

**源码事实：** `delete` 在 `LongTermMemory` 里没有同步长期库（源码有 TODO），所以「删除一条消息」在长期库里仍然命中，`find_news` 会继续把它当已见。删除语义在这里是不完整的。

**源码事实：** `search_similar` 的阈值比较方向与注释相悖（保留低分而非高分），且参数 `k` 未被使用。这是一处需要调用方自知的行为，不是可由文档推断的语义。

**源码事实：** `RoleZeroLongTermMemory` 的读写门控都是 `count() > memory_k`，但写只在 `add` 时判、读还在 `get` 时判，且读多了「最后一条是用户需求」和 `k != 0` 两个条件。因此同一个状态在「写已开启、读未开启」之间可以长期共存。

**架构推论：** 两条路线对「长期记忆」的定义不同导致接口不同：`LongTermMemory` 把长期库当作**判重集合**，因此不需要召回接口，只覆写 `find_news`；`RoleZeroLongTermMemory` 把它当作**扩容历史**，因此必须覆写 `get`。想判断某个角色用的是哪种记忆，看它覆写了哪个方法比看类名更准。

**架构推论：** 外迁以「一条」为粒度，且必须靠短期库仍保留原文才能挪（`get_by_position` 取的是当前 `storage` 里的条目）。因此若有人在外部直接清空短期库，长期库就失去了继续外迁的数据源，两边会脱节。整个设计假设「短期库是唯一真相，长期库是它的延伸」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/memory/longterm_memory.py` | `LongTermMemory` | 覆写 add/find_news/clear 的长期记忆 |
| `metagpt/memory/memory_storage.py` | `MemoryStorage.recover_memory` | 按目录内索引文件存在性决定加载或新建 |
| `metagpt/memory/memory_storage.py` | `MemoryStorage.search_similar` | 相似度检索与阈值过滤 |
| `metagpt/memory/longterm_memory.py` | `LongTermMemory.find_news` | 短期新闻再用长期库剔重 |
| `metagpt/memory/role_zero_memory.py` | `RoleZeroLongTermMemory` | 容量门控 + RAG 召回的长期记忆 |
| `metagpt/memory/role_zero_memory.py` | `RoleZeroLongTermMemory.add` | 超容量后外迁滑出窗口的那一条 |
| `metagpt/memory/role_zero_memory.py` | `RoleZeroLongTermMemory.get` | 三条件门控召回并前置拼接 |
| `metagpt/memory/role_zero_memory.py` | `_build_longterm_memory_query` | 以最后一条消息内容作为召回查询 |

## 参考

- [MetaGPT 源码：memory/longterm_memory.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/memory/longterm_memory.py)
- [MetaGPT 源码：memory/role_zero_memory.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/memory/role_zero_memory.py)
- [MetaGPT 文档：RAG 模块](https://docs.deepwisdom.ai/main/en/guide/tutorials/rag.html)

## 与主核 / AI 的关系

「短期库是唯一真相、长期库只是它的延伸」与主核里「缓冲池是页的唯一在内存副本、持久层是它的延伸」是同构的：主核的脏页最终必须写回，读缺失时再从持久层取；这里的短期库决定写什么出去，取不足时再从长期库捞。两者的差别在于一致性保证——主核有 WAL 与页校验，这里没有任何校验，外迁失败只记一条日志，长库与短库可以长期不一致。

对 agent 工程的含义：长期记忆的成本是有形状的。`RoleZeroLongTermMemory` 每次召回都要一次向量检索（若开 `use_llm_ranker` 还多一次模型调用），而读门控把这次开销限制在「用户刚提出问题」这一种时机；外迁每次只挪一条，避免了批量归档的抖动。设计成这个形状，说明它服务的是「单角色长任务」，而不是「多角色高并发」。

