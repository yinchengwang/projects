---
project: ragas
feature: cache
title: 结果缓存
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/cache.py
    symbols: [CacheInterface, DiskCacheBackend, cacher]
  - path: src/ragas/cache.py
    symbols: [_make_pydantic_picklable]
---

# 结果缓存

## 一句话职责

用一个装饰器把「函数调用 → 结果」按内容哈希落盘：不传后端就原样返回原函数，传了就按「函数名 + 归一化后的参数」算键，命中即返回、未命中则执行并把结果写成**可 pickle**的形式。

## 机制是什么

三层结构，每层都很薄：

| 层 | 载体 | 职责 |
|---|---|---|
| 契约 | `CacheInterface` | 三个方法：`get(key)`、`set(key, value)`、`has_key(key)` |
| 实现 | `DiskCacheBackend` | 基于 `diskcache` 的落盘实现，目录默认 `.cache` |
| 接入 | `cacher(cache_backend)` | 装饰器：无后端则透传；有则包同步/异步两版包装 |

它被用在**模型调用的最外层**：`BaseRagasLLM.__post_init__` 里若设了 `cache`，就把 `generate_text` / `agenerate_text` 就地替换成包装后的版本；`llms/base.py` 里另有一处对 `generate`/`agenerate` 做同样的事；embedding 包装器则把 `embed_text`、`embed_query`、`embed_documents`、`predict` 等逐个包起来。所以缓存的粒度是「一次完整的模型/嵌入调用」，不是一个指标或一次评测。

`CacheInterface` 还实现了 `__get_pydantic_core_schema__`（返回 `is_instance_schema`）——这让它可以作为 pydantic 字段类型（`DSPyOptimizer.cache` 就是），否则 pydantic 会对抽象类报错。

## 怎么实现的

**装饰器在「无后端」时就返回原函数，不是包一层空壳。** `cacher(None)` 直接 `return func`，所以不配缓存的路径上没有任何额外开销，也不改变函数的签名与身份。

**同步与异步分两个包装函数，靠 `inspect.iscoroutinefunction` 决定用哪个。** 两个包装体只差一个 `await`，其余完全一致：算键 → `has_key` 命中即 `get` → 否则执行 → `_make_pydantic_picklable(result)` → `set` → **返回原 result 而不是 picklable 版**。也就是说写进缓存的是副本，调用方拿到的仍是活对象。

**键的生成是一条三步流水线。** `_generate_cache_key(func, args, kwargs)`：

1. 过滤 kwargs 里 `EXCLUDE_KEYS`（当前只有 `callbacks`）——回调对象每次不同，进键就永远不命中；
2. `_make_hashable` 把参数递归归一成可排序结构；
3. `json.dumps(key_data, sort_keys=True, default=str)` 后 SHA256。

第 3 步的两个参数各有用途：`sort_keys=True` 让同样内容不同插入顺序的 dict 得到同一个键（我实测 `_make_hashable` 已经用 `tuple(sorted(...))` 保证这点，`sort_keys` 是二次保险）；`default=str` 兜住所有不可序列化对象。

**`_make_hashable` 的四条规则与一处隐患。**

- `tuple`/`list` → 递归成 tuple；
- `dict` → `tuple(sorted((k, _make_hashable(v)) for ...))`；
- `set` → `tuple(sorted(_make_hashable(e) ...))`；
- `BaseModel` → 先 `model_dump()` 再递归。

**隐患在 `sorted`**：对混合键类型的 dict（如 `{'a': 1, 2: 'b'}`），Python 3 会抛 `TypeError: '<' not supported between instances of 'int' and 'str'`。我按同样逻辑实测确认了这个失败。真实场景里 `model_dump()` 出来的键都是字符串，所以常规路径不会触发；但只要有一个自定义对象把非 str 键塞进了 kwargs 的嵌套结构，缓存就会在算键时崩掉。

**第二个隐患在「不认识的类型原样返回」。** 对既不是前四类也不是可哈希基本类型的对象，`_make_hashable` 走 `else: return o`，于是这个对象一路进到 `json.dumps(default=str)`，最终被 `str(obj)` 序列化。而 Python 默认的 `object.__repr__` **包含内存地址**（形如 `<Foo object at 0x...>`）。我实测同一进程内同一对象两次算键一致、两个不同实例得到不同键——也就是说这类参数会让缓存**在进程内有效、跨进程必然失效**。对「跑一次评测、重启再跑」这个最常见场景，缓存等于没生效。

**`_make_pydantic_picklable` 解决的是一类具体的破坏。** docstring 写明：instructor 库在结构化输出生成过程中会动态创建新的类对象，这些类的身份有问题，导致无法 pickle。函数的做法是：若对象是 `BaseModel`，从 `obj.__class__.__module__` 取模块对象，再按 `__name__` 从模块里取「真正的类」，**若它存在且与当前类不是同一个对象**，就用 `actual_class(**obj.model_dump())` 重建一份；否则原样返回。

所以它修的不是「pydantic 不能 pickle」——正常 pydantic 模型可以——而是「instructor 造出来的冒牌类不能 pickle」。判别标准是 `actual_class is not obj_class` 这个身份比较，很直接。

**`DiskCacheBackend` 是纯转发，加两处细节。** `__init__` 里延迟 `from diskcache import Cache`，缺包时抛带安装命令的 `ImportError`；`__del__` 里 `self.cache.close()` 释放句柄。`has_key` 用的是 `key in self.cache`（diskcache 的 `__contains__`），不是先 `get` 再判 None——避免把「缓存里存的确实是 None」误当成未命中。

**接入点是「构造时替换实例属性」，不是类级装饰。** `BaseRagasLLM.__post_init__` 里 `self.generate_text = cacher(...)(self.generate_text)`——注意此时 `self.generate_text` 是**绑定方法**，包出来的 wrapper 通过 `functools.wraps` 保留了 `__wrapped__`，所以 `inspect.signature` 仍能取到原始签名（我实测包装后签名为 `(p, n=1)`）。副作用是：一旦包装，缓存键里的 `func.__qualname__` 就是原方法的限定名（`X.generate_text`），与实例无关——**同一个类的两个实例共享同一键空间**，若它们的构造函数参数不同（不同模型、不同温度），会互相命中对方的缓存。

## 关键取舍

**源码事实：** `cacher(None)` 直接返回原函数，不包装。所以「不开缓存」在运行时零成本。

**源码事实：** `EXCLUDE_KEYS` 只含 `callbacks`，且只在顶层 kwargs 过滤，不递归。

**源码事实：** `_make_hashable` 对 dict 用 `tuple(sorted(...))`，混合键类型会抛 `TypeError`（实测确认）。

**源码事实：** 未知类型经 `default=str` 序列化，而默认 `repr` 含内存地址，导致这类参数的缓存跨进程失效（实测：同对象两次同键，两实例不同键）。

**源码事实：** `_make_pydantic_picklable` 只在「模块里能找到同名真类且与当前类不同一个」时才重建，其余原样返回。

**源码事实：** 缓存键只含 `func.__qualname__`，不含实例身份或实例状态。同一 LLM 类的不同实例共享键空间。

**源码事实：** 写缓存时存的是 `_make_pydantic_picklable(result)`，而返回给调用方的是原 result。

**架构推论：** 缓存键的设计本质是**「什么算同一次调用」这个判定**。这里选的是「函数名 + 入参内容」，把「谁调用」「调用时什么配置」排除在外。对纯函数这是对的；对「带状态的对象上的方法」就不成立——模型的温度、provider、system prompt 若不在入参里，就不会进键。这是装饰器式缓存在有状态对象上最常见的失配。

**架构推论：** 「未知类型不报错、交给 `str()`」是典型的宽松兜底：好处是任何参数都能算出一个键、不会中断调用；代价是这些键可能不稳定或不唯一，而**失败是静默的**——用户只会观察到「缓存好像没起作用」，不会收到任何警告。更稳的做法是给未知类型加一个显式的「不可缓存」分支直接跳过缓存。

**架构推论：** `_make_pydantic_picklable` 的存在揭示了一个更广的问题：**跨库的对象身份**。第三方库动态生成的类在 pickle 时按「模块 + 限定名」查找，找不到就失败；而这类类往往只存在于内存。任何「把结果落盘」的系统都会遇到同类问题，处理手法都是同一个：**按模块名重新解析到真类，再按数据重建实例**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/cache.py` | `CacheInterface` | 三方法契约 + pydantic 类型支持 |
| `src/ragas/cache.py` | `DiskCacheBackend` | diskcache 落盘实现，含延迟导入与 `__del__` 关闭 |
| `src/ragas/cache.py` | `cacher` | 装饰器：无后端透传，有后端分同步/异步包装 |
| `src/ragas/cache.py` | `_generate_cache_key` | 过滤回调 → 归一化 → JSON → SHA256 |
| `src/ragas/cache.py` | `_make_hashable` | 四类递归归一，未知类型原样放行 |
| `src/ragas/cache.py` | `_make_pydantic_picklable` | 修复 instructor 动态类的 pickle 身份问题 |
| `src/ragas/cache.py` | `EXCLUDE_KEYS` | 顶层 kwargs 里不进键的键名 |
| `src/ragas/llms/base.py` | `BaseRagasLLM.__post_init__` | 构造期把 `generate_text`/`agenerate_text` 替换为缓存版 |
| `src/ragas/llms/litellm_llm.py` | `generate`/`agenerate` 的包装 | 同类做法在 LiteLLM 路径上的复用 |
| `src/ragas/embeddings/base.py` | `embed_text` 等的包装 | 嵌入侧把多个方法逐个包起来 |
| `src/ragas/optimizers/dspy_optimizer.py` | `DSPyOptimizer.cache` | 把 `CacheInterface` 当 pydantic 字段用 |

## 参考

- [ragas 源码：cache.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/cache.py)
- [python-diskcache 仓库](https://github.com/grantjenks/python-diskcache)
- [Python 文档：pickle](https://docs.python.org/3/library/pickle.html)
- [Python 文档：hashlib](https://docs.python.org/3/library/hashlib.html)

## 与主核 / AI 的关系

「按内容哈希缓存调用结果」与主核里的「查询结果缓存 / 页缓存」是同一种东西，判定条件也一样：**键必须完整刻画「这次结果取决于什么」**。主核里键是表版本 + 谓词 + 参数，漏掉版本号就会返回陈旧数据；这里键是函数名 + 参数，漏掉「模型配置」就会在两个不同配置的实例之间串味。

数据库侧对这件事有个更硬的约束值得借鉴：**缓存条目要能被失效**。主核有版本号/事务 id 做天然失效条件；这里没有——一旦写入磁盘，同键的结果永久有效。所以配置变更、代码变更后若不手动清目录，就会一直读到旧值。

三条对 AI 工程的结论。第一，**键的稳定性要在跨进程场景下验证**：`default=str` + 默认 `repr` 含地址，会让「重启后再跑」的缓存全部失效（实测确认），这恰好是最省钱的场景。第二，**回调类参数必须排除**：每次新建的 `callbacks` 对象进键就永不命中，`EXCLUDE_KEYS` 是对的，但只做顶层过滤意味着嵌套结构里的回调仍会进键。第三，**第三方库动态类的落盘需要身份修复**：`_make_pydantic_picklable` 的思路（按模块名找回真类、按数据重建）是通用解法，不是 ragas 特有的补丁。

最后一条判断：判断一个 agent/评测框架的缓存是否可用，看两件事——键里有没有「模型与配置」，以及未知类型是「跳过缓存」还是「猜一个键」。前者决定正确性，后者决定它是否会在你没察觉的时候什么都没做。
