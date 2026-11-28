---
project: ragas
feature: tokenizers
title: 分词器适配
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/tokenizers.py
    symbols: [BaseTokenizer, TiktokenWrapper, HuggingFaceTokenizer]
  - path: src/ragas/tokenizers.py
    symbols: [get_tokenizer]
---

# 分词器适配

## 一句话职责

把「把文本切成 token」这件小事抽象成三方法接口（`encode` / `decode` / `count_tokens`），并提供两个实现（tiktoken 与 HuggingFace）加一个延迟加载的默认实例——因为真正的成本不在接口，而在**首次创建分词器时要下载并解析词表**。

## 机制是什么

| 层 | 载体 | 特点 |
|---|---|---|
| 契约 | `BaseTokenizer` | 抽象 `encode`/`decode`，具体方法 `count_tokens` = `len(encode(text))` |
| 实现一 | `TiktokenWrapper` | 包装 OpenAI 的 tiktoken，支持「预设 encoding / 按模型名 / 按 encoding 名 / 默认 `o200k_base`」四种入口 |
| 实现二 | `HuggingFaceTokenizer` | 包装 transformers 的 `AutoTokenizer`，需要 `transformers` 可选依赖 |
| 默认值 | `_LazyTokenizer` + `get_default_tokenizer()` | 全局单例，首次属性访问才真正创建 |
| 工厂 | `get_tokenizer(type, model_name, encoding_name)` | 按字符串选实现，参数校验在此收口 |

**`_LazyTokenizer` 的存在原因写得很直白**：源码注释是「Lazy initialization to avoid network calls at import time」。tiktoken 的 `get_encoding` 首次调用会去拉词表文件，若在模块导入时就建实例，`import ragas` 会变成一次隐式网络请求——这在离线环境、CI、以及「只是想读个版本号」的场景里都是不可接受的。

## 怎么实现的

**接口设计上只有两个真抽象方法。** `count_tokens` 是 `BaseTokenizer` 里的**具体方法**（`return len(self.encode(text))`），子类可覆写但不是必须。所以「数 token」这件事默认就是「编码后数长度」，只有需要特殊语义时才覆写。

**`TiktokenWrapper` 的四段优先级是明确的。** `__init__` 按顺序判断：传了 `encoding` 对象就用它；传了 `model_name` 就 `encoding_for_model(model_name)`；传了 `encoding_name` 就 `get_encoding(encoding_name)`；都没有则默认 `o200k_base`。这四条互斥且不可组合——**同时传 `model_name` 与 `encoding_name` 时后者被静默忽略**（`elif` 链），没有任何警告。

两个方法上的细节：`encode` 传 `disallowed_special=()`，即**允许文本里出现 `<|endoftext|>` 这类特殊 token 字面量而不报错**。tiktoken 默认会把它们当作「不允许的特殊 token」抛异常；评测场景里用户文本可能真的包含这类字符串，关掉检查才不至于把正常数据当异常。`decode` 直接透传。

**`HuggingFaceTokenizer` 有两个必填中的必填。** `tokenizer` 与 `model_name` 至少给一个，否则 `ValueError("Either tokenizer or model_name must be provided")`。若给 `model_name` 才在**方法内部** `from transformers import AutoTokenizer`——即 transformers 是可选依赖，只有走这条路才需要它；缺包时抛带安装指引的 `ImportError`。

`encode` 传 `add_special_tokens=False`，`decode` 传 `skip_special_tokens=True`。这两个默认值的选择与「数 token 用于长度预算」的用途一致：不该把 `[CLS]`/`</s>` 算进内容长度。

**`get_default_tokenizer()` 是进程级单例，靠模块级全局变量。** 首次调用时 `_default_tokenizer = TiktokenWrapper(encoding_name="o200k_base")`，后续直接返回缓存。没有锁——多线程首次并发调用可能各建一个（后写的赢），但因为结果等价，实际无害。

**`_LazyTokenizer` 是「替身对象」，四种转发方式叠加。** 它继承 `BaseTokenizer`（docstring 说明这是为了通过静态类型检查），然后：
- `__getattr__` 把**任何**未知属性转发给真分词器（所以 `tokenizer.encoding` 也能穿透）；
- `encode`/`decode`/`count_tokens` 各自显式转发一次。

显式转发是必要的：`__getattr__` 只在常规查找失败后才触发，而 `encode`/`decode` 定义在 `BaseTokenizer` 上，正常情况下会命中基类的抽象方法而不是走 `__getattr__`。所以这三个方法必须各写一行，否则调用会落到基类的 `pass` 上返回 `None`。

**模块底部有一个模块级常量 `DEFAULT_TOKENIZER: BaseTokenizer = _LazyTokenizer()`。** 它是「向后兼容」的入口：`testset/transforms/base.py` 里 `tokenizer: BaseTokenizer = field(default_factory=lambda: DEFAULT_TOKENIZER)` —— 用 `default_factory` 而不是直接赋值，是为了不在 dataclass 定义时就共享同一个可变默认值（这里虽然对象本身是延迟代理、不持有状态，但形状上仍然是正确的做法）。

顺带一个可以确定的事实：`BaseTokenizer` 与三个实现**没有**在 `src/` 里被发现的其他调用点使用了 `count_tokens`；实际被用的是 `testset/transforms` 里的 `encode`/`decode`（切块用），以及 `utils.py` 里 `num_tokens_from_string(string, encoding_name, tokenizer=None)` —— 后者若传了 `tokenizer` 就走 `tokenizer.count_tokens`，否则直接 `tiktoken.get_encoding(encoding_name)`。也就是说**这条抽象并没有覆盖全部计数路径**，`utils` 里还有一条绕过它的直连实现，且默认 encoding 是 `cl100k_base`，与默认分词器的 `o200k_base` 不同。

**`get_tokenizer` 是唯一的字符串工厂，参数校验在这里收口。** `tokenizer_type == "tiktoken"` 直接透传两个可选参数；`"huggingface"` 要求 `model_name` 非空否则 `ValueError`；其它值抛 `ValueError(f"Unknown tokenizer type: ...")`。它是 `ragas/__init__.py` 导出的公开 API 之一（与 `BaseTokenizer`、`TiktokenWrapper`、`HuggingFaceTokenizer` 一起）。

## 关键取舍

**源码事实：** 默认分词器延迟创建，注释理由是避免导入时发起网络调用；`_LazyTokenizer` 继承 `BaseTokenizer` 以便通过静态类型检查，并显式转发三个方法（仅靠 `__getattr__` 会因为基类方法命中而失效）。

**源码事实：** `TiktokenWrapper.__init__` 的四段是 `if/elif` 链，同时传 `model_name` 与 `encoding_name` 时后者被静默忽略。

**源码事实：** `encode` 传 `disallowed_special=()`，允许特殊 token 字面量；HuggingFace 侧 `encode` 关 `add_special_tokens`、`decode` 关 `skip_special_tokens`。

**源码事实：** `transformers` 只在 `model_name` 路径上延迟导入，缺包抛带 `pip install transformers` 指引的 `ImportError`。

**源码事实：** `utils.num_tokens_from_string` 提供了一条绕开 `BaseTokenizer` 的直连路径，默认 encoding 为 `cl100k_base`，与默认分词器的 `o200k_base` 不一致。

**源码事实：** `get_default_tokenizer` 用无锁的模块级全局做单例。

**架构推论：** 「延迟加载」在这里不是性能优化而是**语义必需的**：`import` 一个纯计算库不该产生网络副作用。这条原则值得推广到所有「模块级默认对象」——只要构造会触发 IO（下载、连接、读大盘文件），就必须做成代理或 `default_factory`。

**架构推论：** 替身对象（lazy proxy）的坑在**方法解析顺序**：`__getattr__` 只在找不到名字时触发，所以基类上已有的方法必须逐个显式转发。这是 Python 里做 lazy proxy 最常见的错误，且症状是「返回 None 或空结果」而不是抛错——极难发现。

**架构推论：** 两个默认值不一致（默认分词器 `o200k_base`、`num_tokens_from_string` 默认 `cl100k_base`）会导致**同一个文本在两处得到不同的 token 数**。对「按 token 预算切块/截断」这类用途，这是实质性偏差：切块边界与长度上限会因走哪条路径而不同。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/tokenizers.py` | `BaseTokenizer` | 三方法契约，`count_tokens` 有默认实现 |
| `src/ragas/tokenizers.py` | `TiktokenWrapper` | tiktoken 四段优先级构造 + 允许特殊字面量的 `encode` |
| `src/ragas/tokenizers.py` | `HuggingFaceTokenizer` | transformers 可选依赖 + 特殊 token 的取舍 |
| `src/ragas/tokenizers.py` | `get_default_tokenizer` | 无锁的进程级单例创建 |
| `src/ragas/tokenizers.py` | `_LazyTokenizer` | 延迟代理，显式转发三个方法 |
| `src/ragas/tokenizers.py` | `DEFAULT_TOKENIZER` | 向后兼容的模块级实例 |
| `src/ragas/tokenizers.py` | `get_tokenizer` | 字符串工厂与参数校验 |
| `src/ragas/utils.py` | `num_tokens_from_string` | 可绕开接口的直连计数路径 |
| `src/ragas/testset/transforms/base.py` | 分词器字段 | 用 `default_factory` 取 `DEFAULT_TOKENIZER` |
| `src/ragas/__init__.py` | 导出清单 | 把四个公开符号暴露到顶层 |

## 参考

- [ragas 源码：tokenizers.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/tokenizers.py)
- [tiktoken 仓库](https://github.com/openai/tiktoken)
- [HuggingFace tokenizers 文档](https://huggingface.co/docs/tokenizers)
- [transformers 分词器文档](https://huggingface.co/docs/transformers/main_classes/tokenizer)

## 与主核 / AI 的关系

「把分词抽成接口」与主核里的「字符集/排序规则抽象」是同一类问题：**同一份数据在不同实现下有不同度量**。主核里 `utf8mb4` 与 `latin1` 决定一列能存多少字节；这里 `o200k_base` 与 `cl100k_base` 决定一段文本算多少 token。两者都必须把「用哪个」显式记录下来，否则跨实现的比较毫无意义。

三条对 AI 工程的结论。第一，**默认值不一致是隐性 bug**：同一文本在 `DEFAULT_TOKENIZER` 与 `num_tokens_from_string` 下 token 数不同，会让「按 token 切块」的边界在两处不一致；所有度量都必须能追到「用的是哪个实现」。第二，**`import` 不能有 IO 副作用**：延迟加载是必须的，而 lazy proxy 必须显式转发基类已有的方法，否则会静默返回空。第三，**特殊 token 的处理要有明确约定**：评测数据里出现 `<|endoftext|>` 字面量时，`disallowed_special=()` 让计数不失败——这个选择是对的，但要在文档里写清「计数包含什么、不包含什么」。

一条选型判断：如果一个框架的 token 计数有多条路径且默认参数不同，那么它的「上下文窗口预算」功能就不可信——先统一度量，再谈裁剪。
