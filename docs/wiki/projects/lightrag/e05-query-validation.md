---
project: lightrag
feature: query-validation
title: 查询校验
page_type: feature
schema_version: 1
refs:
  - path: lightrag/query_validation.py
    symbols: [validate_query_not_empty, validate_rag_query, QueryValidationError]
  - path: lightrag/query_validation.py
    symbols: [rag_query_weight, meets_min_rag_query_weight, _is_wide_character]
---

# 查询校验

## 一句话职责

用两个不等价的拒绝理由守住查询入口：**「什么都没有」**（任何路径都不允许）与**「短到不可能有检索意义」**（只针对 RAG 检索路径）——而后者用「东亚字符计 2、其余计 1」的加权长度判定，让中文两个字与英文三个字母等价。

## 机制是什么

整个模块只有 136 行，而它的重量几乎全在一段注释里：**为什么宽字符表要按整个 Unicode 区块列举，而不是按已分配的码点。**

两个拒绝理由是两种不同的契约：

| 异常 | 条件 | 适用范围 | 语义 |
|---|---|---|---|
| `EmptyQueryError` | `strip()` 后为空 | **所有路径**，含 `bypass` 与 `/api/generate` | 「你什么都没说」 |
| `RAGQueryTooShortError` | 加权长度 < 3 | 只有 RAG 检索模式 | 「你说的不足以检索」 |

**`bypass` 与 `/api/generate` 豁免的是后者而不是前者。** 它们的文档字符串把这件事说了两遍：

> 这适用于每条路径，`bypass` 与 `/api/generate` 也包括在内：**空提示对 LLM 没有意义**，转发它只会换来一个 provider 侧错误或一个对虚无的幻觉答案。

**这个区分是必要的**：`bypass` 意味着「不走检索，直接把查询交给模型」，所以「太短」对它不成立（模型可以回答「你好」）；但「空」对它同样不成立（空提示无意义）。

## 怎么实现的

### 一、权重函数：把 2 赋给东亚字符的理由

**常量只有一行，而它的注释给出了量化依据。**

```
MIN_RAG_QUERY_WEIGHT = 3

# East Asian characters count as two English-equivalent ones: a two-character
# CJK word carries about as much retrieval signal as a three-letter English one.
```

**推一下边界**：三个英文字符 = 3 ≥ 3 通过；两个 CJK 字符 = 4 ≥ 3 通过；一个 CJK 字符 = 2 < 3 拒绝。**于是最小值就是「3 个英文」或「2 个汉字」。**

**为什么 2 而不是 3？** 注释说「两个汉字的词与三个字母的英文词携带的检索信号大致相当」。这是一个经验判断，但它有一个可验证的推论：**中文的双字词（如「北京」「利润」）是语义最小单位**，单字往往不够（「京」单独检索会命中一堆无关内容）。

**`rag_query_weight` 与 `meets_min_rag_query_weight` 是同一个计算的两种形态。**

```
def rag_query_weight(query: str) -> int:
    return sum(2 if _is_wide_character(char) else 1 for char in query.strip())

def meets_min_rag_query_weight(query: str) -> bool:
    weight = 0
    for character in query.strip():
        weight += 2 if _is_wide_character(character) else 1
        if weight >= MIN_RAG_QUERY_WEIGHT:
            return True
    return False
```

**两个函数的文档把「为什么有两个」说得很清楚：**

> 当只有与 `MIN_RAG_QUERY_WEIGHT` 的比较重要时，优先用 `meets_min_rag_query_weight`：`rag_query_weight` 会遍历整个字符串，**在事件循环上最多是 64 KiB 的同步工作**。

而另一个函数给出了实测数字：

> 它一达到阈值就短路，所以一个 64 KiB 的查询只花两次迭代而不是 65,536 次。完整的 `rag_query_weight` 遍历给**每个最大尺寸的 RAG 请求**增加了约 69 ms 的事件循环阻塞 CPU。

**「约 69 ms」是一个被测量过的数字**，而且它说的是「每个最大尺寸请求」。**这是一条「用一次测量把一个看似无害的实现换成另一个」的记录**——如果只读代码，`sum(...)` 与手写循环的差别看起来无关紧要。

**注意两个函数都 `strip()`，而 `rag_query_weight` 的文档补了一句关于内部空白的说明**：

> 每个东亚字符计 2；其它每个 Unicode 码点计 1。**内部空白仍然是查询的一部分**，与之前的字符长度契约一致。

**即只有「外围」空白被剥掉，「内部」空白计入权重**（算作 1）。这与旧实现的行为一致——**而「与旧契约一致」是这类改造里最重要的约束**，否则前端/后端的门禁会不同步。

### 二、宽字符表：一段关于「表会腐烂」的注释

**这是全模块最有分量的部分。核心论点是：按整个 Unicode 区块列举，而不是按已分配的码点。**

```
_WIDE_CODEPOINT_RANGES = (
    (0x1100, 0x11FF),  # Hangul Jamo
    (0x2E80, 0x33FF),  # CJK Radicals, Kangxi, Symbols, Kana, Bopomofo, Hangul
    (0x3400, 0x4DBF),  # CJK Unified Ideographs Extension A
    (0x4E00, 0x9FFF),  # CJK Unified Ideographs
    (0xA960, 0xA97F),  # Hangul Jamo Extended-A
    (0xAC00, 0xD7FF),  # Hangul Syllables and Hangul Jamo Extended-B
    (0xF900, 0xFAFF),  # CJK Compatibility Ideographs
    (0xFE30, 0xFE4F),  # CJK Compatibility Forms
    (0xFF00, 0xFFEE),  # Halfwidth and Fullwidth Forms
    (0x16FE0, 0x16FFF),  # Ideographic Symbols and Punctuation
    (0x1AFF0, 0x1B2FF),  # Kana Extended-B/Supplement/Extended-A, Small Kana, Nushu
    (0x20000, 0x3FFFD),  # Planes 2 and 3 — every CJK extension, present and future
)
```

**第一条理由：这样的表不会腐烂。**

> **它是不会腐烂的。** `U+20000–U+3FFFD` 是整个平面 2 与平面 3，Unicode 把它们分配给 CJK 表意文字，所以从扩展 B 到 J 以及将来每一个都被覆盖。**一个按已分配字符枚举的表每次 Unicode 发布都需要编辑——而且要在两种语言里——并在有人去编辑之前静默少算真实存在的字符。**

**「在两种语言里」指的是这份 Python 实现与前端 TypeScript 实现各一份。** 而这句「静默少算」命中了真正的风险：漏掉一个码点不会报错，只会让一个含该字符的查询被判得太短。

**第二条理由：它放弃的精度在这里一文不值。**

> 它放弃的精度在这里不值一分钱。这些范围内的标点、填充符和未分配码点权重为 2，于是两个这样的字符会通过一个它们不该通过的下限。**代价是一次什么都找不到的检索。没人会打「〾〿」来问事情，而三字符下限是对「用户到底有没有问任何东西」的粗略启发式，不是一个正确性边界。**

**这段话把「这是一个启发式」讲透了**，而它的论据分两支：

1. **误判的代价不对称**：错误放行 → 一次空检索（用户看到「没找到」，然后改写查询）；错误拒绝 → 用户被拦在门外（更糟）。所以宁可放宽。
2. **触发条件不现实**：没人会只用标点查询。

**第三条理由是「按契约裁剪，而不是按宽度表扩张」——这是全模块最有设计智慧的一句。**

> 这些区块是**书写中文、日文、韩文**的那些，这正是这个下限的契约在**两份 API 指南与全部十一个 locale 字符串**里说的。东亚且宽但书写其它语言的文字系统——西夏文、契丹文、彝文——不在内。它们曾经短暂在内，起因是**从一张宽度表上读到「东亚宽字符」而不是从契约上读**，而唯一的后果是一份「西夏文被不一致地覆盖」的报告。**扩张到每个宽字符区块会让那张表变大来回答那份报告，并招来下一份；与契约对齐则通过收缩来回答它。**

**「扩张会让表变大来回答一份报告，并招来下一份；与契约对齐则通过收缩来回答它」** —— 这是把「范围蔓延」与「契约对齐」的差别说成了一个成本论证。而它给出的根因也很具体：**从一张宽度表上读需求，而不是从契约上读**。

**第四条：范围必须落在区块边界上，而提前结束是一个真实发生过的 bug。**

> 因此范围结束在**区块**边界上。提前结束一个是真正的 bug，而且已经发生过：从一个更早的按字符表里带过来的 `U+115F` 把**谚文字母（Hangul Jamo）切成两半**，于是韩文的中间与末尾字母权重变成了 1。

**这是一个精确的复盘**：`U+115F` 是谚文字母（Hangul Jamo）；从旧表继承来的 `U+115F` 只覆盖到了 `U+115F`（`0x1100–0x115F`），而它在区块中的位置是 `0x1100–0x11FF` 的中间。于是 `0x1160–0x11FF` 的中间/末尾字母被算作 1，韩文查询的权重被低估。**「提前结束一个范围」这个错误的后果是「一半的字符被算错」，而不是「不生效」——所以它很难被发现。**

**第五条：与前端同步由一个测试强制。**

```
# Keep these ranges in sync with ``lightrag_webui/src/utils/queryValidation.ts``;
# ``test_the_frontend_range_table_matches_this_one`` enforces it.
```

**「有一个测试强制它」是关键**——否则「保持同步」只是一句会腐烂的注释，而那恰恰是这个表试图避免的问题。

### 三、`_is_wide_character`：一个求和而不是二分

```
def _is_wide_character(character: str) -> bool:
    codepoint = ord(character)
    return any(start <= codepoint <= end for start, end in _WIDE_CODEPOINT_RANGES)
```

**12 个区间、线性扫描。** 对于「3 个字符就够」这个用途，二分查找是过度优化——而且 `meets_min_rag_query_weight` 已经在最外层短路了，所以实际最多扫 2 个字符 × 12 个区间 = 24 次比较。

**这是一个正确的「不为性能做多余结构」的判断**：函数调用频率高，但每次只做极少的工作，所以线性扫描优于引入排序与二分（后者会带来「表必须保持有序」的额外不变量）。

### 四、两个校验函数：先归一，再判定

**`validate_query_not_empty` 是所有路径的闸门。**

```
def validate_query_not_empty(query: str) -> str:
    normalized = query.strip()
    if not normalized:
        raise EmptyQueryError("Query must not be empty.")
    return normalized
```

**它返回归一后的字符串而不是布尔** —— 于是调用方拿到的是「干净的查询」，不必再 strip 一次。**这是一个「校验顺带归一」的接口设计**，好处是「归一化只有一处」，坏处是调用方必须用返回值（丢弃它就会用未归一的原始输入）。

**而 `validate_rag_query` 是「先通用、再专用」。**

```
def validate_rag_query(query: str) -> str:
    normalized = validate_query_not_empty(query)
    if not meets_min_rag_query_weight(normalized):
        raise RAGQueryTooShortError(_TOO_SHORT_MESSAGE)
    return normalized
```

**复用而不是重复 `strip()`** —— 于是两个校验只可能有一处归一逻辑。而 `meets_min_rag_query_weight` 内部又 `strip()` 了一次（对已归一的字符串是幂等的），这是一个小冗余但保证任一函数被单独调用时行为一致。

**而错误消息把规则完整写出来，且给了不变量与例外。**

```
_TOO_SHORT_MESSAGE = (
    "RAG query is too short. Enter at least 3 English characters or an "
    "equivalent combination where each Chinese, Japanese or Korean character "
    "counts as 2."
)
```

**三种语言都被点名**（Chinese、Japanese、Korean）—— 这与宽字符表的契约一致（只覆盖 CJK，不含西夏文等）。**错误消息与实现用的是同一个契约，这是「消息即文档」的正确做法。**

### 五、异常层级：一个基类与两个子类

```
class QueryValidationError(ValueError):
    """Base class for rejections of user-supplied query text."""

class EmptyQueryError(QueryValidationError):
    """Raised when query text is empty or only whitespace. ..."""

class RAGQueryTooShortError(QueryValidationError):
    """Raised when query text is too short for meaningful RAG retrieval."""
```

**基类继承 `ValueError`** —— 于是所有捕获 `ValueError` 的上层代码（宽泛的输入校验）自动接住它们，不必知道这个模块存在。而需要精细区分的地方可以捕 `EmptyQueryError`。

**两个子类的意义在于「调用方要不要区分」**：HTTP 层可能给 `EmptyQueryError` 返回 400（格式错误）、给 `RAGQueryTooShortError` 返回也是 400 但带不同的提示文案；而 `bypass` 路径只调用前者，所以后者的异常在那里根本不可能出现。

**基类的文档说「用户提供的查询文本的拒绝」** —— 注意是「拒绝」而不是「错误」。这个措辞把「用户输入不合法」与「系统出故障」划开了：**这两类异常都是正常的业务结果，不应该触发告警。**

## 关键取舍

**源码事实：** 两个拒绝理由对应两个异常：`EmptyQueryError`（任何路径，含 `bypass` 与 `/api/generate`）与 `RAGQueryTooShortError`（仅 RAG 检索模式）。前者文档写明「空提示对 LLM 没有意义，转发只会换来 provider 错误或对虚无的幻觉答案」。

**源码事实：** `MIN_RAG_QUERY_WEIGHT = 3`，东亚字符计 2。推得下限 = 「3 个英文字符」或「2 个 CJK 字符」。注释给的依据是「两个 CJK 字与三个英文字母携带的检索信号大致相当」。

**源码事实：** 同一计算有两种形态：`rag_query_weight`（完整遍历）与 `meets_min_rag_query_weight`（短路）。后者存在的理由是实测：完整遍历给「每个最大尺寸（64 KiB）RAG 请求」增加约 69 ms 的事件循环阻塞 CPU；短路后同一查询只花两次迭代。

**源码事实：** 两个函数都只 `strip()` 外围空白，内部空白计入权重（算 1），文档注明这是「与之前的字符长度契约一致」。

**源码事实：** 宽字符表按**整个 Unicode 区块**列举（12 个区间），而不是按已分配码点。理由有三：不会腐烂（`U+20000–U+3FFFD` 覆盖平面 2 与 3 的全部 CJK 扩展，含将来的）；放弃的精度无价值（范围内标点/填充符权重为 2，误放行的代价只是一次空检索）；「三字符下限是启发式不是正确性边界」。

**源码事实：** 表只覆盖书写中日韩的文字系统，西夏文/契丹文/彝文被明确排除。它们曾经在内，起因是「从宽度表读需求而不是从契约读」；处理方式是**收缩到契约**而不是扩张到所有宽区块，注释的理由是「扩张会让表变大来回答一份报告，并招来下一份」。

**源码事实：** 范围必须结束在区块边界上。注释记录了一个真实 bug：从旧表继承的 `U+115F` 把谚文字母区块切成两半，使韩文的中间/末尾字母权重为 1。

**源码事实：** 与前端的同步（`lightrag_webui/src/utils/queryValidation.ts`）由测试 `test_the_frontend_range_table_matches_this_one` 强制，而不只靠注释。

**源码事实：** `_is_wide_character` 用 12 区间的线性扫描而非二分——因为外层已短路（最多扫 2 个字符），引入排序会让「表必须有序」变成新的不变量。

**源码事实：** `validate_query_not_empty` 返回归一后的字符串而非布尔，`validate_rag_query` 复用它（于是归一逻辑只有一处）。

**源码事实：** 异常层级是 `QueryValidationError(ValueError)` 加两个子类，于是宽泛的 `except ValueError` 自动接住，而需要区分时能区分。

**架构推论：** 这个模块展示了**「契约优先于覆盖」**的完整推理链：需求写在 API 指南与 locale 字符串里（中日韩），于是实现只覆盖中日韩；当有人报告「西夏文覆盖不一致」时，正确反应是检查契约而不是扩表——因为扩表看起来在修问题，实际是在改契约，且会招来下一份报告（「为什么彝文覆盖了而傈僳文没有」）。

**架构推论：** 「按整个区块列举」这个决定本身是一个**关于维护成本的判断**：精确的表在某个时刻更准确，但需要随每次 Unicode 发布更新（且在两处语言里）；而区块级的表永远不需要更新，代价是在某些范围内过度覆盖。**当一个判定的精度要求是「启发式」级别时，选择「不需要维护」比选择「更准确」长期更正确。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/query_validation.py` | `MIN_RAG_QUERY_WEIGHT` | 阈值 3（= 3 英文或 2 CJK） |
| `lightrag/query_validation.py` | `_WIDE_CODEPOINT_RANGES` | 12 个 Unicode 区块，按区块而非按码点 |
| `lightrag/query_validation.py` | `_is_wide_character` | 线性扫描 12 区间 |
| `lightrag/query_validation.py` | `rag_query_weight` | 完整加权长度（配 69 ms 的实测说明） |
| `lightrag/query_validation.py` | `meets_min_rag_query_weight` | 短路版本，达到阈值即返回 |
| `lightrag/query_validation.py` | `validate_query_not_empty` | 归一 + 空判定，返回归一后的字符串 |
| `lightrag/query_validation.py` | `validate_rag_query` | 复用空判定后再判加权长度 |
| `lightrag/query_validation.py` | `QueryValidationError` | 基类，继承 `ValueError` |
| `lightrag/query_validation.py` | `EmptyQueryError` / `RAGQueryTooShortError` | 两个不等价的拒绝理由 |
| `lightrag/query_validation.py` | `_TOO_SHORT_MESSAGE` | 消息点名中日韩三语，与实现同契约 |

## 参考

- [lightrag 源码：query_validation.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/query_validation.py)
- [Unicode 文档：CJK 统一表意文字区块](https://www.unicode.org/charts/)
- [Python 文档：unicodedata 与码点](https://docs.python.org/3/library/unicodedata.html)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

输入校验与主核的**参数校验层**职责相同：在进入昂贵的执行路径之前，用最便宜的手段拒掉不可能成功的请求。而两者的判据性质完全不同。

**主核的校验几乎全是「结构性的」**：类型对不对、长度是否越界、外键是否存在。这些都是可判定的，所以拒绝是明确的、无争议的。而这里的判据是「这句话够不够构成一个查询」——**它本质上是语义判断，却只能用长度来近似**。于是整个模块的努力方向不是「判得更准」，而是「**让一个不准的判据不至于错得太离谱**」。

**「用某个可测量的量当代理指标」在数据库里也有对应**：查询优化器用行数估算当「这次扫描代价」的代理，而估算可能完全错（选择性估计是数据库里最著名的难题之一）。两者的应对方式相同——**承认它是启发式，把边界设在不会造成灾难的地方，而不是试图让它精确**。数据库的做法是「估算错也只是慢」，这里的做法是「判错也只是多一次或空一次检索」。

**宽字符表在「两种语言里各一份」这件事与主核的跨端一致性要求同构。** 数据库的校验规则（标识符长度、保留字、排序规则）也要在服务端与客户端驱动里保持一致，而不同步的后果是「同一个查询在 CLI 能跑、在驱动里报错」。**用测试强制同步（而不是注释提醒）是唯一可靠的做法**——这与主核里「用 CI 校验协议版本一致」是同一个思路。

对 agent 工程的直接价值有三条。其一，**误判的代价不对称时，宁可放宽**。错误放行一次空检索远好于错误拒绝一个真实提问，所以宽字符表选区块级而非码点级。其二，**用测量替换猜测，并且把数字写下来**。`69 ms` 这个数字解释了为什么需要两个几乎相同的函数——没有它，后来者会把短路版本当成冗余删掉。其三，**契约优先于覆盖范围**。当覆盖缺口被报告时，先检查契约说在范围内的是哪些——扩表看起来在修问题，实际在改契约，且会持续招来新的报告。
