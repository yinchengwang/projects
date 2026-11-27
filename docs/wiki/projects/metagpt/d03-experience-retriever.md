---
project: metagpt
feature: experience-retriever
title: 经验检索器与策略注入
page_type: feature
schema_version: 1
refs:
  - path: metagpt/strategy/experience_retriever.py
    symbols: [ExpRetriever, TRDExpRetriever, SimpleExpRetriever]
---

# 经验检索器与策略注入

## 一句话职责

经验检索器把「该照着什么样子做」的事前示例从角色记忆里选出来，塞进系统提示的 examples 槽位——它的接口只收一段上下文、只返回一段文本，不碰任何状态。

## 机制是什么

`ExpRetriever` 是一个 pydantic `BaseModel`，只有一个方法 `retrieve(context: str = "") -> str`。基类实现直接 `raise NotImplementedError`。

六个子类，全部是「返回一段常量字符串」，差别只在返回哪一段：

| 子类 | 返回内容 | 谁在用 |
|---|---|---|
| `DummyExpRetriever` | 空串 | `RoleZero` 默认 |
| `SimpleExpRetriever` | `TL_EXAMPLE`（团队领导者的七组示例） | `TeamLeader` |
| `KeywordExpRetriever` | 按关键词从三个示例里挑 | `DataAnalyst` |
| `TRDExpRetriever` | 一份完整的 TRD 工作流示例 | 未在角色里使用 |
| `TRDAllExpRetriever` | 更长的 TRD 两段式示例 | 未在角色里使用 |
| `TRDToolExpRetriever` | 精简版（单个命令） | 未在角色里使用 |

也就是说：**除了 `KeywordExpRetriever`，其余检索器都不做任何检索**，`context` 参数被忽略。名字叫 retriever，行为是「返回预置的 few-shot 段落」。

`KeywordExpRetriever` 是唯一真正看输入的：它在 `context.lower()` 里找 `deploy` / `issue` / `https:` / `http:` / `search` / `click` 这些关键词，命中就返回对应的示例块，都不命中返回空串。

## 怎么实现的

**注入点在角色层的提示装配。** `RoleZero._think` 里第一条就是 `example = self._retrieve_experience()`，然后把它作为 `example` 参数塞进 `self.system_prompt.format(...)`。所以经验是**系统提示的一部分**，不是对话历史的一部分——它每一轮都被重新注入，而不是在历史里累积。

`_retrieve_experience` 的默认实现做四步：从 `self.rc.memory.get(self.memory_k)` 取最近 200 条消息、逐个 `str()`、用空行拼成一段文本、交给 `self.experience_retriever.retrieve(context=context)`。所以「上下文」是「最近的对话历史压成的一个大字符串」。

这个默认实现被 docstring 标为「Can be overwritten in subclasses」，而 `Engineer2` 确实覆写了——它直接 `return ENGINEER_EXAMPLE`，跳过了取记忆与调检索器这两步，用一份写死在代码里的工程示例。

**检索器是角色的字段，可以在构造时替换。** 三个角色各声明了自己的默认值：

- `RoleZero`：`experience_retriever: Annotated[ExpRetriever, Field(exclude=True)] = DummyExpRetriever()`
- `TeamLeader`：改成 `SimpleExpRetriever()`
- `DataAnalyst`：改成 `KeywordExpRetriever()`

`Field(exclude=True)` 让检索器不进序列化——它是代码里的策略对象，不是数据。`Annotated[ExpRetriever, ...]` 把字段类型声明成基类，所以构造时传任意子类实例都合法，这是「策略可替换」的类型层面落点。

**内容形态是 JSON 命令序列。** 所有示例块的格式一致：一段 `## example N`、一行 `User Requirement:`、一行 `Explanation:`，然后一个 ```json 代码块，里面是命令列表。命令的形状是 `{"command_name": ..., "args": {...}}`，与 `RoleZero` 实际输出的命令格式完全一致——因为示例的作用就是「教模型按什么格式输出命令」。

`TL_EXAMPLE` 的七组示例覆盖了很不同的情形：从零建计划（分解成多个 `Plan.append_task` 并按角色分工）、数据类任务不分解决（一条任务直接派给分析师）、收到上游完成消息后勾任务并派下一个、纯问答不建计划、全部完成时直接 `end`、以及「当前任务与已完成任务相同」时直接勾掉。这些不是按关键词挑的，而是**每次都全给**——`SimpleExpRetriever` 无条件返回整段。

`KeywordExpRetriever` 的三个分支也各有针对性：`deploy` 给「用 Terminal 守护进程起服务、测试、再部署」；`issue` 给「读 issue、clone、搜符号、改、提 PR」；`https`/`search` 给浏览器抓取流程（带 `click` 时给多步交互版，否则给直连 URL 的简版）。匹配是朴素的子串包含，大小写不敏感，按 `if/elif` 顺序短路。

**注意 `KeywordExpRetriever` 的签名与基类不一致。** 它多了 `exp_type: Literal["plan", "task"] = "plan"` 参数，且那段 `task` 分支被整块注释掉了（注释里留着 `SEARCH_SYMBOL_EXAMPLE` 的名字，但该常量并不存在）。这是个未完成的分支：类型上允许传 `task`，行为上仍返回 `""`。

## 关键取舍

**源码事实：** `retrieve` 只收 `context` 一个参数，不返回结构化对象，也不返回评分。这意味着「检索」的全部输出就是一段要粘进提示的文本，调用方无法知道命中了哪条经验、也无法在下游做置信度判断。

**源码事实：** 五个检索器忽略 `context`。它们实际上是「预置示例的选择器」，选择依据写死在类里（某个角色用某个类），而不是由运行时上下文决定。`KeywordExpRetriever` 是唯一的例外。

**源码事实：** `TRDExpRetriever` 家族三个类在代码里没有任何角色引用。它们是为一套 TRD（技术需求文档）工作流准备的示例库，但当前的角色体系（`RoleZero` 系）没有接入。

**架构推论：** 把经验做成「角色字段 + 提示中的 examples 槽位」而不是「记忆检索的命中结果」，带来一个直接后果：经验不会随任务进展变化。同一角色在整个项目里看到的示例是同一份，除非它换检索器或换角色。好处是可预测（示例稳定，模型行为稳定），坏处是无法针对「当前卡在一个具体错误」这种情况给出更贴切的示范。

**架构推论：** 检索器与角色记忆的耦合被放在了 `_retrieve_experience` 这个角色方法里，而不是检索器内部。检索器不知道记忆系统存在，只接收一段字符串。这使「换成向量检索」变成只需换一个 `ExpRetriever` 子类并保持接口，代价是那类实现拿不到结构化的消息对象（只有拼好的文本）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/strategy/experience_retriever.py` | `ExpRetriever` | 接口：只收上下文、只返回文本 |
| `metagpt/strategy/experience_retriever.py` | `DummyExpRetriever` | 返回空串，`RoleZero` 的默认零经验策略 |
| `metagpt/strategy/experience_retriever.py` | `SimpleExpRetriever` | 无条件返回团队领导的七组示例 |
| `metagpt/strategy/experience_retriever.py` | `KeywordExpRetriever` | 按关键词在部署/修 issue/抓取三组示例间选择 |
| `metagpt/strategy/experience_retriever.py` | `TRDExpRetriever` | 完整 TRD 工作流示例（当前未被角色引用） |
| `metagpt/strategy/experience_retriever.py` | `TRDAllExpRetriever` | 更长版 TRD 示例（同上） |
| `metagpt/strategy/experience_retriever.py` | `TRDToolExpRetriever` | 单命令精简版 TRD 示例（同上） |
| `metagpt/roles/di/role_zero.py` | `_retrieve_experience` | 取最近 200 条消息拼成文本后交给检索器 |
| `metagpt/roles/di/role_zero.py` | `_think` | 把经验注入系统提示的 examples 槽位 |
| `metagpt/roles/di/team_leader.py` | `experience_retriever` | 字段默认值改为 `SimpleExpRetriever` |
| `metagpt/roles/di/data_analyst.py` | `experience_retriever` | 字段默认值改为 `KeywordExpRetriever` |

## 参考

- [MetaGPT 源码：strategy/experience_retriever.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/strategy/experience_retriever.py)
- [MetaGPT 源码：roles/di/role_zero.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/roles/di/role_zero.py)
- [MetaGPT 文档：RoleZero 与 Data Interpreter](https://docs.deepwisdom.ai/main/en/guide/tutorials/agent_about.html)

## 与主核 / AI 的关系

「检索器只返回一段文本、不暴露命中信息」这个设计，与主核里那些「只返回结果缓冲、不返回执行计划」的封装是同一种取舍：把内部策略完全藏在接口后面，调用方只能看到最终产物。好处是替换实现零成本；坏处是无法观测。

对 agent 工程的实际经验是：few-shot 示例该放系统提示还是对话历史，是个有实质差别的选择。放在系统提示（这里的选择）意味着每轮重注入、不占历史预算、模型注意力始终有新示例；放在历史里则会随上下文压缩被裁掉。MetaGPT 选了前者，并且用「一个角色一个检索器」的方式把示例选择做成了类级配置——这比在运行时按语义相似度检索简单得多，也更容易调。真正需要动态检索时（比如按当前错误类型给不同修复示范），接口是够用的：只要写一个看 `context` 的 `ExpRetriever` 子类，把它挂到角色字段上即可。
