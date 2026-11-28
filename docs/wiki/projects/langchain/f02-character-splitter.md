---
project: langchain
feature: character-splitter
title: 字符与递归字符分块
page_type: feature
schema_version: 1
refs:
  - path: libs/text-splitters/langchain_text_splitters/character.py
    symbols: [CharacterTextSplitter, RecursiveCharacterTextSplitter]
  - path: libs/text-splitters/langchain_text_splitters/base.py
    symbols: [TextSplitter]
---

# 字符与递归字符分块

## 一句话职责

两个分块器：`CharacterTextSplitter` 是**单分隔符切分**（按一个字符/正则切开再合并），`RecursiveCharacterTextSplitter` 是**多分隔符按优先级递归降级**——先用最粗的分隔符（空行）、不够就换更细的（换行、空格），最后退到按字符切；它还带**按语言选择的预设分隔符表**，让代码文件按类/函数/控制流边界切。

## 机制是什么

| 类 | 切法 | 分隔符来源 |
|---|---|---|
| `CharacterTextSplitter` | 一次 `re.split` + 合并 | 一个 `separator`（可正则） |
| `RecursiveCharacterTextSplitter` | 逐级递归，每次换更细的分隔符 | 一个 `separators` 列表，默认 `["\n\n", "\n", " ", ""]` |
| `MarkdownTextSplitter`（在另一页） | 用户 `Language.MARKDOWN` 的预设 | 语言预设表 |

而两者都复用了基类 `TextSplitter` 的 `_merge_splits`——也就是说「切」各不同，「合」统一。

## 怎么实现的

### 一、`CharacterTextSplitter`：四步与一个「不要重复插入分隔符」的判断

**`split_text` 的实现分四步，每步的注释是作者自己编号写的。**

1. **决定匹配模式**：`separator if is_separator_regex else re.escape(separator)`。
2. **初次切分**：`_split_text_with_regex(text, sep_pattern, keep_separator=...)`。
3. **识别零宽前后视断言**：判据是分隔符是否以 `(?=`、`(?<!`、`(?<=`、`(?!)` 这四个前缀之一开头。
4. **决定合并时用什么分隔符**：

```
merge_sep = ""
if not (self._keep_separator or is_lookaround):
    merge_sep = self._separator
```

**第 3 步这个判断是本页最细的一处，它解决的是一个真实的正确性问题。** 若分隔符是 `(?=...)` 这样的零宽断言，`re.split` **不会从文本里删掉任何字符**——切出来的片段拼回去还是原文。此时若合并时再插一次 `separator` 字面量，就会往文本里塞入字符串 `"(?=...)"`，产生一段本来不存在的垃圾。

而第 4 步的另一个条件 `self._keep_separator` 说的是另一件事：**若切分时已经把分隔符留在片段里**（`keep_separator` 为真），合并时就不能再加一次。

两个条件合并成「只要分隔符已经在片段里、或它根本不是可插入的字面量，就不要再插」。这是一个典型的「同一个参数在切分与合并两处必须语义一致」的约束。

**`_split_text_with_regex` 用「给分隔符加括号」把分隔符保留在结果里。** 这是 `re.split` 的一个特性：

```
re.split(f"({separator})", text)
```

带捕获组的 `re.split` **会把分隔符也作为结果的一部分返回**。于是结果列表是「片段、分隔符、片段、分隔符、…」交替排列。

然后为了支持 `keep_separator` 的三态，代码把交替列表重新配对：

```
splits = ([splits_[i] + splits_[i + 1] for i in range(0, len(splits_) - 1, 2)])
    if keep_separator == "end"
    else ([splits_[i] + splits_[i + 1] for i in range(1, len(splits_), 2)])
```

- `"end"`：从下标 0 起步、步长 2，取「片段 + 后面的分隔符」——分隔符放在**块尾**。
- `"start"`（或 `True`）：从下标 1 起步，取「分隔符 + 后面的片段」——分隔符放在**块首**。

**但这两行之外还有两段补丁，它们处理边界情形。**

```
if len(splits_) % 2 == 0: splits += splits_[-1:]
splits = ([*splits, splits_[-1]]) if keep_separator == "end" else ([splits_[0], *splits])
```

第一段：若 `re.split` 的结果长度是**偶数**，说明文本末尾没有分隔符——此时最后一段是孤立片段，要单独追加。

第二段：把**第一段**（`"start"` 时）或**最后一段**（`"end"` 时）补回去。理由很直接：

- `"start"` 配对从下标 1 开始，于是**下标 0 的那一段（第一个分隔符之前的内容）被漏掉了**，必须显式放回最前面。
- `"end"` 配对到 `len-1` 为止，于是**最后一个元素（最后一个分隔符之后的内容）被漏掉了**，必须放到最后。

这两行是「用列表切片重组时最容易漏掉头部/尾部」的典型修正，而作者把它们与主配对逻辑并列写出，正是为了让读者能看到边界在哪里。

**`if separator` 为空时 `splits = list(text)`——按单字符切。** 这个兜底让「分隔符为空」有确定语义：切到最小单位。而它也是递归切分器最后一层退到的那条路径（默认分隔符列表的末尾是空串）。

**返回前 `[s for s in splits if s]` 过滤掉空串。** `re.split` 在文本以分隔符开头/结尾/有连续分隔符时会产生空片段，这些都该丢。

### 二、`RecursiveCharacterTextSplitter`：选分隔符与递归

**`_split_text(text, separators)` 的第一步是「从优先级列表里找第一个在文本里出现过的分隔符」。**

```
separator = separators[-1]
new_separators = []
for i, s_ in enumerate(separators):
    separator_ = s_ if self._is_separator_regex else re.escape(s_)
    if not s_:
        separator = s_                    # 空分隔符：直接采用，不往下找
        break
    if re.search(separator_, text):
        separator = s_
        new_separators = separators[i + 1:]
        break
```

三条退出路径：

1. **遇到空字符串分隔符** → 立即采用它（按字符切），**不再继续找**。空串作为「最后的兜底」必须放在列表末尾，而它的存在会让这个循环提前结束——否则 `re.search("", text)` 永远为真，逻辑上会选到它但语义上应当作为终局。
2. **找到第一个真实出现的分隔符** → 采用它，并**记住它后面的那些**（`new_separators`）作为下一层的候选。
3. **一个都没找到** → `separator` 保持为 `separators[-1]`（列表最后一项），`new_separators` 为空。

第三种情形的后果是：所有分隔符都不在文本里，于是按最后一个（通常是空串）切分；同时 `new_separators` 为空意味着**不再递归**。这是一个「如果连最后的分隔符都没有，就不要再试」的终止条件。

**「记住后面的候选」是递归能逐级降级的关键。** 每一层只用「当前这层分隔符之后」的那些，于是层级是**严格递减的**：`"\n\n"` 用过之后下一层只剩 `["\n", " ", ""]`；`"\n"` 之后再只剩 `[" ", ""]`。这保证了不会重复尝试已经失败的分隔符。

**切完之后是「逐个片段判长度、够长就递归」的循环。**

```
good_splits = []
separator_ = "" if self._keep_separator else separator
for s in splits:
    if self._length_function(s) < self._chunk_size:
        good_splits.append(s)
    else:
        if good_splits:
            merged_text = self._merge_splits(good_splits, separator_); final_chunks.extend(merged_text); good_splits = []
        if not new_separators:
            final_chunks.append(s)
        else:
            other_info = self._split_text(s, new_separators); final_chunks.extend(other_info)
if good_splits:
    merged_text = self._merge_splits(good_splits, separator_); final_chunks.extend(merged_text)
```

四个要点：

1. **判定用 `<` 而不是 `<=`**：恰好等于 `chunk_size` 的片段会被当成「太长」进入递归分支。这是个 off-by-one 的选择，保守方向。
2. **遇到长片段时先把积累的短片段合并产出**（`if good_splits:` 那一段），然后清空。如果不先产出，长片段的递归结果会与前面的短片段混在一起，顺序被打乱。
3. **没有下一层分隔符时，长片段原样保留**（`final_chunks.append(s)`）——**不切了**。这就是本页最重要的取舍：一个无法再切细的超长片段会被当成一个块输出，导致实际的块超过 `chunk_size`。基类 `_merge_splits` 里那个 `logger.warning` 会因此报警。
4. **`separator_` 在 `keep_separator` 为真时被清空。** 因为此时片段自身已经带着分隔符，合并时再加一次就会重复。

而 `keep_separator` 的默认值在这里变了：基类默认 `False`，而 `RecursiveCharacterTextSplitter` 默认 `True`。这个改动是对的——递归切分是靠分隔符（`"\nclass "` 这类）识别边界的，若默认把它们丢掉，「按类定义切」的效果会打折。

**递归的终止条件实际上是两个，而不是一个。**

- 分隔符列表耗尽（`new_separators` 空）→ 直接输出。
- 片段长度已小于 `chunk_size` → 进入 `good_splits`，最后被合并。

第 2 条是常态退出，第 1 条是失败退出。**递归深度上限由分隔符列表长度决定**——默认列表长度 4，所以最多 4 层。

### 三、语言预设：二十八种语言的分隔符表

**`get_separators_for_language` 是一大段 `if` 链，每个语言返回一个列表。**

以 C/C++ 为例（两者共用一份表）：

```
"\nclass "                      # 类定义
"\nvoid " / "\nint " / "\nfloat " / "\ndouble "   # 函数定义
"\nif " / "\nfor " / "\nwhile " / "\nswitch " / "\ncase "   # 控制流
"\n\n" / "\n" / " " / ""        # 普通行、空行、空格、字符
```

**这个排序的意图很清楚：越「语义完整」的边界越靠前。** 「类定义前」比「空行」重要得多——因为一段类定义跟着它的方法才是完整上下文，而按空行切会把它们拆散。

而每条注释都标了类别（`Split along class definitions` / `Split along function definitions` / `Split along control flow statements` / `Split by the normal type of lines`），于是这份列表的构造规则在代码里就可见了。

**`from_language` 是一个 `classmethod`，它强制 `is_separator_regex=True`。**

```
separators = cls.get_separators_for_language(language)
return cls(separators=separators, is_separator_regex=True, **kwargs)
```

这个强制是必要的：**语言预设里有些分隔符就是正则**。最典型的是 VisualBasic6：

```
vis = r"(?:Public|Private|Friend|Global|Static)\s+"
rf"\n(?!End\s){vis}?Sub\s+"
```

`(?!End\s)` 是一个**否定前视断言**——`\nEnd Sub` 不该被当成「Sub 定义的开头」。这类「排除结束标记」的判断只能用正则表达。

**VisualBasic6 那份表说明了为什么语言预设必须支持正则：** 语言的关键字边界往往需要「前面是换行、后面是关键字、但要排除某些已排除形态」这种三段条件，而纯字面串表达不了。而其余语言（C、Go、Java…）的预设都是纯字面串——它们**在 `is_separator_regex=True` 下也能正常工作**，因为它们的串里没有正则元字符（`re.escape` 未生效时直接当正则用也匹配字面）。

这是一个「同一套参数既能喂字面串也能喂正则」的宽容设计，代价是**若某个语言的字面分隔符里含正则元字符（比如 `#` 在无 escape 时不是元字符，但 `.` 是），行为就会错**。

**末尾有两个不同的报错，且它们区分了「实现了但还没写」与「根本不存在」。**

```
if language in Language._value2member_map_: raise ValueError(f"Language {language} is not implemented yet!")
raise ValueError(f"Language {language} is not supported! Please choose from {list(Language)}")
```

第一条针对**枚举里有但还没写预设**的语言（「not implemented yet」），第二条针对根本不是 `Language` 的值（「not supported」并列出全部候选）。这个二分对使用者的价值很直接：前者意味着「等更新」，后者意味着「你拼错了」。

**默认分隔符列表的末尾那个空串是这条链的基石。** 它保证递归总有一层可以退：`["\n\n", "\n", " ", ""]` 的最后一层是「按字符切」，而任何文本都可以被切成单字符。

## 关键取舍

**源码事实：** `CharacterTextSplitter` 在「分隔符是零宽前后视断言」或「`keep_separator` 为真」时不重新插入分隔符——前者因为零宽断言不从文本删字符，插入会产生垃圾；后者因为分隔符已在片段里。

**源码事实：** `_split_text_with_regex` 用带捕获组的 `re.split` 保留分隔符，再用两段补丁补回被切片漏掉的头部（`start` 模式）或尾部（`end` 模式）。

**源码事实：** 递归切分每层只保留「当前分隔符之后」的候选，于是层级严格递减，不会重复试已失败的分隔符。

**源码事实：** 递归遇到长片段时**先合并并产出已积累的短片段**，再递归——否则顺序会被打乱。

**源码事实：** 没有下一层分隔符时，超长片段**原样输出为一个块**，于是实际块可能超过 `chunk_size`（基类会 `warn`）。

**源码事实：** `RecursiveCharacterTextSplitter` 的 `keep_separator` 默认 `True`（基类默认 `False`），因为递归靠分隔符识别边界。

**源码事实：** 判定「片段太长」用 `<` 而非 `<=`，恰好等于 `chunk_size` 的片段会进入递归分支。

**源码事实：** 语言预设强制 `is_separator_regex=True`，因为部分预设（如 VisualBasic6 的 `\n(?!End\s)...`）必须是正则；而大部分预设是纯字面串，在正则模式下也能正常工作。

**源码事实：** 语言表的排序按「语义完整的边界优先」，每条带注释说明类别。

**源码事实：** 两个报错区分「枚举里有但未实现」与「值不合法并列出候选」。

**架构推论：** 这个递归分块器的本质是**一个优先级列表驱动的分治**：从最粗的分隔符开始切，只对「仍然太长」的片段继续切细，直到没得切为止。它的输出质量完全取决于**那张优先级表**——对自然语言是「空行 → 换行 → 空格 → 字符」，对代码是「类定义 → 函数定义 → 控制流 → 空行 → …」。

这也解释了为什么那张表要按语言分别维护二十多份：**「一个完整的语义单元在哪里结束」这件事，只有懂那种语言的人才知道**。而这份知识无法从语法规则自动推导，只能硬编码。

代价是：那张表覆盖不到的语言就退化成按「空行/换行/空格/字符」切，效果与自然语言一样。而它无法表达「按注解、按 import 组、按语言特有的结构」这类更细的边界——那需要真正的解析器（AST），而不是字符串分隔符。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/text-splitters/langchain_text_splitters/character.py` | `CharacterTextSplitter` | 单分隔符切分，四步含「不重复插入分隔符」判断 |
| `libs/text-splitters/langchain_text_splitters/character.py` | `CharacterTextSplitter.split_text` | 决定匹配模式、初切、判零宽断言、定合并分隔符 |
| `libs/text-splitters/langchain_text_splitters/character.py` | `_split_text_with_regex` | 带捕获组的 `re.split` + 两种 `keep_separator` 配对 + 两段边界补丁 |
| `libs/text-splitters/langchain_text_splitters/character.py` | `RecursiveCharacterTextSplitter` | 多分隔符递归降级 |
| `libs/text-splitters/langchain_text_splitters/character.py` | `RecursiveCharacterTextSplitter._split_text` | 选分隔符、逐片段判长、先合并再递归 |
| `libs/text-splitters/langchain_text_splitters/character.py` | `RecursiveCharacterTextSplitter.from_language` | 取语言预设并强制正则模式 |
| `libs/text-splitters/langchain_text_splitters/character.py` | `get_separators_for_language` | 二十八种语言的分隔符预设表 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `TextSplitter._merge_splits` | 两条路径共用的合并与重叠逻辑 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `TextSplitter._chunk_size` | 递归判定与合并上限的同一来源 |

## 参考

- [langchain 源码：character.py](https://github.com/langchain-ai/langchain/blob/master/libs/text-splitters/langchain_text_splitters/character.py)
- [langchain 文档：按代码语言递归切分](https://python.langchain.com/docs/how_to/code_splitter/)
- [Python 文档：re.split](https://docs.python.org/3/library/re.html#re.split)
- [langchain 文档：文本分割器概念](https://python.langchain.com/docs/concepts/text_splitters/)

## 与主核 / AI 的关系

这个递归降级策略与主核里的**索引页分裂**是同构的：B 树节点满了要分裂，分裂点先尝试「按中间键均分」，若某个键太大则退到「按字节边界」，最终退到「单条记录一页」。两者都是「按优先级从粗到细找切分点」的算法，且都有同一个失败出口——**实在切不动时就让它超限**。

对 agent 工程的直接价值有三条。其一，**零宽断言不能当字面量重新插入**。`(?=...)` 这类分隔符切完文本其实没变，若合并时按字面加回去会污染内容——这个错误很隐蔽，只在用了正则分隔符时才出现。其二，**「切不动就输出超限块」这个降级必须显式，并留下痕迹**。这里靠基类的 `warn` 报出来，让人知道某些块实际超标；若静默通过，下游的 token 超限错误会以完全无关的形式出现。其三，**「语义单元边界」是领域知识，必须逐语言维护**。通用分隔符对代码只能按空行/换行切，会把一个类和它的方法拆散；按 `\nclass ` 切才能保住上下文——这份表没有通用解，只能积累。
