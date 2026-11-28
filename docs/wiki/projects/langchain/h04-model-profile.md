---
project: langchain
feature: model-profile
title: 模型档案与能力声明
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/language_models/model_profile.py
    symbols: [ModelProfile, ModelProfileRegistry]
  - path: libs/core/langchain_core/language_models/_compat_bridge.py
    symbols: [finalize_tool_call_chunk, _accumulate_usage]
---

# 模型档案与能力声明

## 一句话职责

用一个 `total=False` 的 `TypedDict` 声明「这个模型有什么能力」，允许任意字段缺失、允许额外字段（但额外字段会触发一条指向上游版本的警告）；它挂在 `BaseChatModel.profile` 上，由 provider 子类实现一个解析方法去填——失败被静默吞掉，因为这是可选信息。

## 机制是什么

`ModelProfile(TypedDict, total=False)` 的 docstring 把来源与状态写全了：数据「来自 [models.dev](https://github.com/sst/models.dev)，辅以额外字段，并由 `langchain-model-profiles` 包（通过它的 `langchain-profiles` CLI）生成」。

而且带了 `!!! warning "Beta feature"`：「字段与格式可能变化。这是一个 `total=False` 的 `TypedDict`，所以任何字段都可能缺失——**用 `.get()` 保护访问**」。

类体里还有一行 `__pydantic_config__ = ConfigDict(extra="allow")`——所以它在 Pydantic 语境下也允许额外键。

字段按注释分成四组：

- **模型元数据**：`name`、`status`（`'active'` / `'deprecated'` 之类）、`release_date`、`last_updated`（都是 ISO 8601）、`open_weights`（权重是否公开）。
- **输入约束**：`max_input_tokens`（上下文窗口）、`text_inputs`、`image_inputs`、以及 `TODO: add more detail about formats?`。
- 中间还有一组能力（工具调用、结构化输出等）。
- **其他能力**：`attachment`（是否支持文件附件）、`temperature`（**是否支持 temperature 参数**）。

最后这一条很有意思：`temperature: bool` 声明的不是「有什么」，而是「**能不能给某个参数**」。所以这个档案不只是能力清单，还包含了「参数合法性」信息。

`ModelProfileRegistry = dict[str, ModelProfile]` ——一行类型别名，docstring 说「把模型标识或名字映射到其 `ModelProfile` 的注册表」。

## 怎么实现的

**「档案缺失」被当成正常状态，而不是错误。** `BaseChatModel` 上有两个 `model_validator(mode="after")`，顺序很关键。

第一个 `_set_model_profile`：

```python
if self.profile is None:
    with contextlib.suppress(Exception):
        self.profile = self._resolve_model_profile()
```

那句 `contextlib.suppress(Exception)` 带注释：「抑制 partner 覆盖的报错（例如缺少 profile 文件、导入坏掉），使**模型构造永远不会因为一个可选字段而失败**」。

这是一个明确的价值排序：宁可拿不到档案，也不能因为档案拿不到而构造不出模型。档案是增强信息（用来自动配参数、决定是否启用某些能力），缺了它模型仍然能用。若让它成为硬依赖，一个 provider 包打包漏了数据文件就会导致「这个模型的任何调用都失败」——代价与收益完全不成比例。

`_resolve_model_profile` 的默认实现返回 `None`，docstring 把「为什么子类必须自己覆盖」讲透了：

「在子类里覆盖这个方法而不是 `_set_model_profile`。基类验证器会自动调用它并处理赋值。**这避免了把 partner 代码耦合到 Pydantic 验证器机制上**。每个 partner 需要自己的覆盖，因为事情会因 partner 而异，例如标识模型的属性（`model`、`model_name`、`model_id`、`deployment_name`）与 partner 本地的 `_get_default_model_profile` 函数（各自从自己那份档案数据里读）」。

两点值得拆：

第一，「避免耦合到 Pydantic 验证器机制」是一个真实的收益：若 partner 要覆盖 `_set_model_profile`，它就得知道「`mode="after"` 的验证器在什么时机跑、返回值要怎么写、覆盖后会替换掉基类验证器」。让 partner 只实现一个普通方法（输入无、输出 `ModelProfile | None`），复杂度全留在基类。

而那句 docstring 里还有一层警告：「用一个新的 `@model_validator` 覆盖这个验证器会**替换**基类验证器（Pydantic v2 行为），绕过标准解析路径。**普通的方法覆盖不会阻止基类验证器运行**」。所以「覆盖验证器」与「覆盖普通方法」在 Pydantic v2 里的效果完全不同——这是很容易踩的坑，被写进了文档。

第二，`model` / `model_name` / `model_id` / `deployment_name` 这四种属性名说明生态里的模型标识字段并不统一。所以「读哪个字段来查档案」这件事无法在基类做——这正是 `_resolve_model_profile` 必须由每个 partner 覆盖的实质原因。

方法里还有一条 `TODO`：「考虑在 `BaseChatModel` 上加一个 `_model_identifier` 属性来标准化 partner 如何标识模型，那可能让这里有一个调用共享档案加载机制的默认实现」。所以这是一个**已知的、有明确解法但还没做的**抽象欠债。

**第二个验证器的顺序依赖被写成注释。** `_check_profile_keys` 上方：

```python
# NOTE: _check_profile_keys must be defined AFTER _set_model_profile.
# Pydantic v2 runs mode="after" validators in definition order.
```

所以「先填档案、再检查档案的键」这个顺序**完全依赖类体里的定义顺序**。这不是类型系统能保证的，也不是显式声明的依赖——只能靠注释提醒后来者别把两个方法的顺序调换。这类「靠代码书写顺序表达依赖」的设计很脆弱，但 Pydantic v2 的模型验证器没有别的排序机制。

`_check_profile_keys` 的实现带两重守卫：

```python
if self.profile and isinstance(self.profile, dict):
    _warn_unknown_profile_keys(self.profile)
```

`self.profile` 的真值判定处理空字典；`isinstance(..., dict)` 的注释说：「`ModelProfile` 是 `TypedDict`（永远是 dict），但**防止 partner 覆盖返回意外类型**」。所以这是一个防御性的类型检查——因为档案来自 partner 代码，而 partner 可能返回任何东西（例如某个 Pydantic 模型实例）。

**`_warn_unknown_profile_keys` 是一次「版本错配探测」。** 实现五步：

1. `if not isinstance(profile, dict): return`（`# type: ignore[unreachable]`）。
2. `declared = frozenset(get_type_hints(ModelProfile).keys())`。
3. `except (TypeError, NameError)` → `logger.debug(...)` 并 `return`。注释解释两种异常：「`get_type_hints` 在对不可解析的前向引用上抛 `NameError`，在注解求值成非类型对象时抛 `TypeError`」。
4. `extra = sorted(set(profile) - declared)`。
5. `if extra: warnings.warn(...)`。

第 5 步的警告文案是本页最有信息量的一处：

```
f"Unrecognized keys in model profile: {extra}. "
f"This may indicate a version mismatch between langchain-core "
f"and your provider package. Consider upgrading langchain-core."
```

所以它不把「额外键」当成「你写错了字段名」，而是当成「**版本错配**」——provider 包按新格式产出了新字段，而你装的 `langchain-core` 还不认识它们。给出的补救是「升级 `langchain-core`」，方向很具体。

这个诊断的价值在于：`ModelProfile` 是 `total=False` 的 `TypedDict`，所以**额外键在类型层面完全合法**（运行时就是普通字典）。没有这条警告，一个装着旧 core 的用户会得到「新字段被静默忽略」——档案看起来正常、某些能力却悄悄不生效。而这条警告把静默失败变成一条可见的提示。

`frozenset(get_type_hints(...).keys())` 而不是静态列出字段名，是为了「声明在哪、检查就用哪」——加字段只需改类体一处。

第 3 步那个 `logger.debug` 而不是 `warn`：如果 `get_type_hints` 本身失败（前向引用解析不了），那是库内部的问题而非用户的问题，且此时无法判断额外键，所以静默降级到「不检查」比误报好。

**`_compat_bridge` 的两个符号是档案生态的另一半。** 两者的文档都写得很重，值得逐条读。

`finalize_tool_call_chunk` 的职责是「把累积的 tool-chunk 参数解析成 finalized 块」，而 docstring 明确它「**在 compat bridge 的 `_finalize_block` 与 `ChatModelStream` 的流末清扫之间共享**」。所以这是一个被两条路径复用的规范化函数——架构上意味着「一块工具调用在流结束时该长什么样」只有一份定义。

它的参数列表值得一提，因为每个都承载一个「什么都没记住就得靠参数传」的事实：`raw_args`（累积的 partial JSON 字符串）、`id_`（跨块收集的 id）、`name`、`extras`（provider 专有字段）、`finalized_type`（`"tool_call"` 或 `"server_tool_call"`）。

`raw_args` 的文档说「`None` 或空按 `{}` 处理」，`extras` 的文档说「调用方负责已经把不想传播的键丢掉（**尤其是 `type`、`id`、`name`、`args` 与客户端 `tool_call` 上的 `index`**）」。所以这个函数的契约里有一条隐含要求：**去重责任在调用方**。它自己不做过滤，因为「哪些键该丢」取决于 finalized 类型。

返回值三种：`ToolCall`、`ServerToolCall`、或 `InvalidToolCall`——最后那个仅在「`raw_args` 非空但不是合法 JSON」时出现。而 `InvalidToolCall` 里 `args` 存的是**原始字符串**而不是解析结果，docstring 的措辞是「让下游消费者仍能内省这个畸形载荷」。所以坏数据不被丢弃，而是换个形状保留下来——把「解析失败」本身变成一种可观察的结果。

异常捕获是 `except (json.JSONDecodeError, TypeError)`。加 `TypeError` 的理由与 h03 的 `parse_tool_call` 一致：`json.loads(None)` 抛的是 `TypeError` 而不是 `JSONDecodeError`。

`_accumulate_usage` 的职责是「跨块累加用量并合并明细字典」。docstring 说：「`delta` 是一个块的 `usage_metadata`；`current` 是运行中的总计。**两边都按字面键读写，使有类型的形状端到端保留——不绕道 `dict[str, Any]`**」。

实现是：`new = current if current is not None else {}`（注意这是**同一对象的引用**，不是拷贝），然后对 `input_tokens` / `output_tokens` / `total_tokens` 三个键各做 `if ... in delta: new[key] = new.get(key, 0) + delta[key]`，最后对两个 details 子字典做 `merged.update(delta_details)`。

两处设计值得记：

第一，三个 token 计数都用 `if "x" in delta` 而不是 `if delta.get("x")` ——所以**显式给 0 也会被计入**（加 0，无影响），但「缺这个键」不会被当成 0。这个区别在累加里其实无关紧要，但它表达的是「按字面键读写」这个原则：不去假设缺失等于零。

第二，`_isolate_usage` 存在的原因正好补全了第一点。它的 docstring：「为事件拷贝用量，使消费者无法修改源消息。**重放路径（`message_to_events`）喂的是活的 `msg.usage_metadata`，所以发出的事件不能与它的字典共享**：拷顶层加嵌套的两个 details 字典来解除别名。流式累加器已经拥有它构造的字典，所以那条路径上这个拷贝是无害的空操作」。

所以数据流有两条：流式路径里 `_accumulate_usage` 自己构造字典（拥有所有权，可以直接发出去），重放路径里用量来自一个已存在的消息对象（发出去就会被消费者改到源对象上）。`_isolate_usage` 是给后者用的「解除别名」。

而 `_accumulate_usage` 的 `new = current if current is not None else {}` 不拷贝，**恰恰是依赖了「拥有所有权」这个前提**——它在原地累加调用方传进来的字典。两条路径的所有权约定不同，所以需要 `_isolate_usage` 那一步显式解除。这是一对很典型的「所有权约定在本层看不见，只能靠函数注释」的例子。

**档案与这两个函数的联系在于「能力声明的落点在哪里」。** `profile` 声明的是「模型支持什么」，而 `finalize_tool_call_chunk` / `_accumulate_usage` 处理的是「模型实际产出了什么」。前者是事前声明，后者是事后观测——两者共同构成「对模型行为的建模」。档案里的 `structured_output` / `tool_calling` 之类字段，理论上可以用来决定「要不要走工具调用路径」；而 `usage` 累加是档案里「这个模型报告用量吗」的经验来源。

## 关键取舍

**源码事实：** `ModelProfile` 是 `total=False` 的 `TypedDict`，任何字段可缺失，文档要求用 `.get()` 访问。

**源码事实：** `_set_model_profile` 用 `contextlib.suppress(Exception)` 吞掉解析失败，注释明确「模型构造永远不会因为一个可选字段而失败」。

**源码事实：** `_resolve_model_profile` 必须由每个 partner 覆盖，因为标识模型的属性名各不相同（`model` / `model_name` / `model_id` / `deployment_name`）。

**源码事实：** 覆盖 `_set_model_profile` 这个验证器会替换基类验证器，而普通方法覆盖不会——两者效果相反，文档专门指出。

**源码事实：** `_check_profile_keys` 必须在 `_set_model_profile` 之后定义，因为 Pydantic v2 按定义顺序运行 `mode="after"` 验证器。

**源码事实：** 额外键的警告把原因归结为「core 与 provider 包的版本错配」并建议升级 core。

**源码事实：** `finalize_tool_call_chunk` 由 compat bridge 与流末清扫共享，且要求调用方先丢掉不想传播的键。

**源码事实：** `InvalidToolCall` 保留原始 args 字符串而不是丢弃坏载荷。

**源码事实：** `_accumulate_usage` 原地累加（不拷贝），而 `_isolate_usage` 为重放路径解除别名。

**架构推论：** 「档案缺失不致命」这个选择，本质上是把「增强信息」与「必需信息」分开对待。`profile` 用来做智能决策（自动选参数、判断能力），但没有它模型照样能跑。所以失败处理的方向必须是「降级继续」而不是「拒绝构造」——这条判断几乎适用于所有「provider 提供的元数据」。

**架构推论：** 「额外键 = 版本错配」这个诊断方向很值得学。多数库对未知字段的处理是静默忽略（因为 `total=False` 本来就允许任意键），结果是「装了旧版 core 的用户看不到新能力，且不知道原因」。把未知字段解释成版本信号，并给出「升级 core」的建议，是把一类静默失效变成一条可读提示的最省办法。

**架构推论：** 「靠类体书写顺序表达验证器依赖」是一个结构性脆弱点。`# NOTE: ... must be defined AFTER ...` 这条注释是唯一的保护。任何靠顺序表达依赖的设计，都该至少有这么一条注释——因为它会在重构（例如按字母排序方法、或用工具自动整理代码）时被无声打破。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/language_models/model_profile.py` | `ModelProfile` | `total=False` 的能力档案，四组字段 |
| `libs/core/langchain_core/language_models/model_profile.py` | `ModelProfile.__pydantic_config__` | `extra="allow"`，允许额外键 |
| `libs/core/langchain_core/language_models/model_profile.py` | `ModelProfile.temperature` | 声明「能否给这个参数」而非「有什么能力」 |
| `libs/core/langchain_core/language_models/model_profile.py` | `ModelProfileRegistry` | 模型标识到档案的映射类型别名 |
| `libs/core/langchain_core/language_models/model_profile.py` | `_warn_unknown_profile_keys` | 未知键视为版本错配并建议升级 core |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._resolve_model_profile` | 由 partner 覆盖，避免耦合 Pydantic 机制 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._set_model_profile` | 吞异常填充档案，构造不因可选字段失败 |
| `libs/core/langchain_core/language_models/chat_models.py` | `BaseChatModel._check_profile_keys` | 定义顺序依赖的键检查 |
| `libs/core/langchain_core/language_models/_compat_bridge.py` | `finalize_tool_call_chunk` | 两条路径共享的工具调用块规范化 |
| `libs/core/langchain_core/language_models/_compat_bridge.py` | `_accumulate_usage` | 按字面键累加，原地更新 |
| `libs/core/langchain_core/language_models/_compat_bridge.py` | `_isolate_usage` | 重放路径解除字典别名 |

## 参考

- [langchain 源码：language_models/model_profile.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/language_models/model_profile.py)
- [langchain 文档：模型档案](https://docs.langchain.com/oss/python/langchain/models#model-profiles)
- [models.dev：模型元数据源](https://github.com/sst/models.dev)

## 与主核 / AI 的关系

主核里最接近的是「对象元数据里的可选统计信息」：表的行数估计、索引的基数估计、列值的分布直方图——查询优化器用它们做决策，但它们缺失时查询仍然能跑（只是计划可能更差）。`ModelProfile` 与那些统计信息在定位上一致：**用于决策的增强信息，不是数据本身**。

而这个类比还能往前推一步：主核的统计信息是**观测出来的**（采样、计数），这里的档案是**声明出来的**（provider 包里写死的）。声明的好处是零成本、可跨实例复用；坏处是会与实际不符——预训练时的文档说支持某能力，实际版本可能不支持。`_warn_unknown_profile_keys` 那条版本错配警告，处理的就是这种「声明与实际不同步」的一个具体表现。

与 h01（聊天模型契约）的连接：`profile: ModelProfile | None` 是 `BaseChatModel` 的字段（h01 的源码落点里能看到它），两个验证器也挂在那里。所以 h01 是契约的骨架，这一页是「骨架上的一个可选器官」。

与 h03（工具绑定）的连接：档案里关于工具调用、结构化输出的布尔字段，正是 h03 那些转换函数「要不要走这条路径」的依据；而 `finalize_tool_call_chunk` 处理的是「走了之后拿到了什么」。

对 AI 工程最实用的一条是「未知字段当作版本信号」。给一个「允许任意键」的类型加一条「出现未知键就警告版本错配」，成本只有一次 `get_type_hints` 加一次集合差，收益是把一类静默失效变成可读提示。判据是：**这个类型是跨包共享的吗？** 是的话，未知键几乎总意味着两边的版本不同步。

另一条是 `_resolve_model_profile` 那个「让 partner 只实现普通方法」的设计。当基类需要「子类提供一个值，但值的获取时机由基类控制」时，让子类实现一个无参方法比让它覆盖验证器要好得多——后者把整套机制细节（验证时机、返回值语义、覆盖会替换基类）暴露给了子类，而那恰恰是最容易写错的部分。

最后一条是 `_isolate_usage` 揭示的所有权约定。同一个值，在一条路径上是「本层构造的、可以给出去」，在另一条路径上是「别人的、给出去会被改」。而函数签名看不出来这个差别，只能靠注释。任何「返回不含拷贝的复合结构」的 API 都有这个隐患——判据是：**这个值可能是调用方或某个长期存活对象的内部状态吗？** 是的话，在边界上拷一次比事后排查「为什么我的用量被改了」便宜得多。
