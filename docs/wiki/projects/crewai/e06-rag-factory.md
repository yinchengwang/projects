---
project: crewai
feature: rag-factory
title: RAG 存储工厂
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/rag/factory.py
    symbols: [create_client, register_rag_client_factory]
---

# RAG 存储工厂

## 一句话职责

`create_client` 按配置里的 `provider` 造出向量库客户端：内置 `chromadb` 与 `qdrant` 两条懒加载分支，外部可以通过 `register_rag_client_factory` 按 provider 名插一个新工厂，或覆盖内置实现。

## 机制是什么

这一层只解决一个问题：**给定一份配置，交出可用的客户端，并且不要为了「可能用到」而导入重量级依赖**。

选择键是 `config.provider`。分发有三条路，顺序固定：

1. 查登记表 `_factories`——按 provider 名找用户登记的工厂，命中就直接调用它；
2. 内置分支：`chromadb`、`qdrant`；
3. 都没有——抛 `ValueError("Unsupported provider: ...")`。

登记优先于内置，这是刻意的：源码注释解释「为一个内置 provider 登记的工厂会覆盖该 provider 的内置实现」。

## 怎么实现的

**两条内置分支的导入方式不同，原因也不同。** `chromadb` 用 `importlib.import_module("crewai.rag.chromadb.factory")`，源码注释只写了一句：「导入这个包会在 chromadb 缺失时抛出安装提示」——也就是说，缺失依赖的提示是由 `crewai.rag.chromadb` 包自身在导入时产生的，工厂层不必再判断。`qdrant` 则走 `require("crewai.rag.qdrant.factory", purpose="The 'qdrant' provider")`，由通用的可选依赖工装来把 `ImportError` 转成带「哪个功能需要它」的明确错误。

两条分支的返回类型都被 `cast` 成预先定义的协议（`ChromaFactoryModule` / `QdrantFactoryModule`），协议里只声明 `create_client(self, config)` 一个方法。这是静态类型层面的一种妥协：真实模块无法在运行期被约束成协议，但 `cast` 让调用点拿到正确的返回类型，同时避免在顶层导入 `ChromaDBClient` / `QdrantClient`。

**为什么按 provider 建注册表，而不是像记忆/知识/流程那样「单默认 setter」。** 源码在 `RagClientFactory` 的注释里给了理由：`create_client` 本来就是在 `config.provider` 上分派的，所以这一层最自然的接缝是「每个 provider 一个工厂」，而不是「整体替换一个默认值」。这也解释了为什么这个模块同时提供 `register` 与 `unregister` 两个函数——按名字注册、按名字移除，可以逐个 provider 地增删，副作用局限在单个 provider 上。而记忆、知识、流程三处用的是「进程级单默认 + 传 None 复位」，因为它们的选择维度是「要不要替换默认后端」，不是「哪个 provider」。

**登记是进程级的、无并发保护的。** `_factories` 是一个普通 dict，`register` 直接赋值、`unregister` 用 `pop(provider, None)`（不存在时静默）。docstring 明确说「预期在启动时一次性设置」。这不是疏忽而是定位：配一次、之后只读。若运行期反复注册同一个 provider，最后一个生效，没有任何版本或引用计数概念。

**客户端的真实构造在各自工厂里。** `chromadb` 的工厂先建目录（`os.makedirs(persist_dir, exist_ok=True)`），然后在 `lock(f"chromadb:{persist_dir}")` 里创建 `PersistentClient`，最后用同一把锁名构造 `ChromaDBClient`——也就是说「打开数据库」这一步被串行化，防止多线程同时初始化同一个持久目录。`qdrant` 的工厂更复杂，因为它要处理本地模式的两个硬约束：本地存储目录只能被一个进程打开，以及 SQLite 后端不是线程安全的。它的解法是「同一路径共享同一个底层客户端 + 每客户端一把 RLock + 用 pid 后缀隔离默认目录 + fork 检测」。这些都在 d 组之外的 provider 实现里，属于本章的下游。

**配置基类只带四个通用旋钮。** `BaseRagConfig` 是个 frozen 的 Pydantic dataclass：`provider`（`init=False`，由子类定值）、`embedding_function`、`limit`（5）、`score_threshold`（0.6）、`batch_size`（100）。三个数值默认值就是「检索取几条、相似度门槛、批量写多大」的全局默认，各 provider 可以覆盖。

**类型层面刻意回避具体配置类。** `rag/config/types.py` 在 `TYPE_CHECKING` 下把 `RagConfigType` 定义为具体配置的联合，运行期则一律退成 `BaseRagConfig`。源码注释解释了原因：若运行期就写具体类名，`crewai` 一被导入就会连带导入每个 provider 后端（例如没安装或没使用的 chromadb）。这是「零成本抽象」的直接体现——类型精度让位于导入开销。

**默认 provider 与懒初始化。** 常量里 `DEFAULT_RAG_PROVIDER = "qdrant"`。而 `get_rag_config()` 在未配置时会造一个默认配置：本地 Qdrant + 默认嵌入器（OpenAI 的 `text-embedding-3-small`，经 `QdrantEmbeddingAdapter` 包一层）。所以「不配任何东西直接用」的实际行为是「本地 Qdrant + OpenAI 嵌入」，这意味着首次使用需要一个 `OPENAI_API_KEY`——也正是 `KnowledgeStorage` 那边会因维度不匹配而给出重置提示的场景。

**模块属性拦截。** `crewai.rag` 包把自己替换成一个自定义 `ModuleType`：对 `rag` 赋值 `config` 会转发给 `set_rag_config`，访问未定义属性时尝试按子模块导入。这让 `import crewai.rag as rag; rag.config = ChromaDBConfig()` 这种写法成立，同时保持「用到哪个 provider 才导入哪个」。代价是模块对象的语义被改写，任何 `getattr(rag, ...)` 的行为都可能是「尝试导入」而不是「取属性」。

## 关键取舍

**源码事实：** 登记优先于内置，且登记没有校验——一个 typo 的 provider 名会被安静地注册进字典，之后 `create_client` 拿到那个字符串时既不会匹配内置也不会报「拼错了」，只会报「Unsupported provider」。

**源码事实：** 两条内置分支的缺失依赖报错走不同机制（包内提示 vs `require` 工装）。这不一致来自历史：chromadb 是早期默认，qdrant 是后加且设为新默认，于是后者用了更完善的工装。

**源码事实：** `register` / `unregister` 无锁、无幂等语义（后者对不存在的名字静默）。定位是「启动时配置一次」。

**源码事实：** 运行期的类型全部塌成 `BaseRagConfig`，为的是避免一 import `crewai` 就拖进所有 provider 后端。这让「安装了 12 个向量库」的环境与「只装 1 个」的环境启动成本一致。

**架构推论：** 这一层的接缝选在「provider 名」而不是「客户端接口」，所以扩展方式是「新增一个 provider 名 + 一个工厂函数」，而不是「实现一个接口并注册到容器」。好处是零抽象、零继承要求；代价是没有任何机制保证新 provider 的客户端满足 `BaseClient` 的行为契约，只能靠类型标注与调用方自觉。

**架构推论：** 默认 provider 是 qdrant，但 `get_rag_config` 造的是**本地** Qdrant（带 path 选项与 pid 隔离）。这选择反映了一个判断：默认应当是「能跑起来且不依赖外部服务」，代价是本地模式的并发与 fork 限制必须由工厂自己兜住——这正是 qdrant 工厂里那段 `_LockedQdrantClient` 与 pid 后缀代码的由来。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/rag/factory.py` | `create_client` | 按 `config.provider` 分发：登记表 → 内置 chromadb → 内置 qdrant → 报错 |
| `lib/crewai/src/crewai/rag/factory.py` | `register_rag_client_factory` | 按 provider 名登记工厂，可覆盖内置实现 |
| `lib/crewai/src/crewai/rag/factory.py` | `unregister_rag_client_factory` | 按 provider 名移除，不存在时静默 |
| `lib/crewai/src/crewai/rag/factory.py` | `RagClientFactory` | 工厂签名：配置进、客户端出 |
| `lib/crewai/src/crewai/rag/config/base.py` | `BaseRagConfig` | 四处通用配置：嵌入器、条数、分数门槛、批量大小 |
| `lib/crewai/src/crewai/rag/config/types.py` | `RagConfigType` | 类型精度与导入开销的折中：运行期统一为基类 |
| `lib/crewai/src/crewai/rag/config/constants.py` | `DEFAULT_RAG_PROVIDER` | 默认 provider 选择 |
| `lib/crewai/src/crewai/rag/chromadb/factory.py` | `create_client` | 内置分支一：建目录、锁内打开持久客户端 |
| `lib/crewai/src/crewai/rag/qdrant/factory.py` | `create_client` | 内置分支二：本地单进程约束、线程锁、fork 隔离 |

## 参考

- [CrewAI 源码：rag/factory.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/rag/factory.py)
- [CrewAI 源码：rag/config/utils.py（配置与懒初始化）](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/rag/config/utils.py)
- [Chroma 文档：持久化客户端](https://docs.trychroma.com/docs/run-chroma/persistent-client)

## 与主核 / AI 的关系

这就是主核里「存储引擎的分层注册 + 按需加载」的一个业务侧对应物。主核的存储层用函数指针表把表访问方法绑定到具体实现，运行期按配置选一套；这里用 provider 名到工厂的字典做同一件事。两边的共同取舍都是「把选择延迟到配置」，并且都要求实现方满足一组行为契约（主核是 `db_storage_*` 方法集，这里是 `BaseClient`）。

差别在约束强度。主核的实现在编译/链接期就被约束在某套接口上，缺一个方法链接就失败；这里只有在真正调用到那个方法时才会发现缺了什么。这也解释了为什么 `cast` 与 `Protocol` 在这里扮演了重要角色——类型检查是唯一的事前约束。

另一处对照是本地模式的并发处理。qdrant 工厂发现「本地存储目录单进程、SQLite 非线程安全」之后，选择「同路径共享一个客户端 + 全局 RLock + 默认目录加 pid 后缀」。这与主核里对页缓存的加锁思路同构：都不允许多套运行时同时操作同一份本地存储，而是收敛成「一份存储一套锁」。区别在主核能在多进程下靠文件锁协作，而这里只能退化成「一个进程一个目录」——因为底层库本身不支持多进程。

对 AI 工程的实际意义在于：向量库选型的真正成本不在 API，而在「本地模式的边界条件」。这套代码里最长的两段注释都在讲这个：本地 Qdrant 一个目录只能一个进程打开，fork 出来的子进程继承父进程的连接后必须报错而不是静默写入，默认目录要加 pid 后缀才能在多 worker 下不互相撞车。这些坑与「用哪个向量库做检索」毫无关系，却决定了同一份代码在 `gunicorn --workers 4` 下能不能跑起来。
