---
project: metagpt
feature: action-node
title: ActionNode 与结构化输出
page_type: feature
schema_version: 1
refs:
  - path: metagpt/actions/action_node.py
    symbols: [ActionNode, ReviewMode, FillMode]
  - path: metagpt/actions/action_outcls_registry.py
    symbols: [register_action_outcls]
---

# ActionNode 与结构化输出

## 一句话职责

ActionNode 把「让模型按固定字段填一张表」这件事做成节点树：树形声明字段与说明，编译成提示，再用动态生成的 pydantic 类把模型输出强行解析成结构化对象。

## 机制是什么

一个 `ActionNode` 有四个声明字段和两个产出字段。声明部分是 `key`（字段名）、`expected_type`（期望类型）、`instruction`（这条字段要填什么）、`example`（示例值）；产出部分是 `content`（模型原文）和 `instruct_content`（解析后的 pydantic 实例）。

节点可以挂子节点，形成一棵树。`get_mapping(mode)` 决定了「这次填充看的是自己还是孩子」：`"children"` 只看子节点，`"root"` 只看自己，`"auto"` 有孩子就看孩子、没孩子看自己。这个 mode 参数贯穿编译、填充、评审、修复四条路径。

三个枚举刻画了这条流水线的可调项：`FillMode`（`code_fill`/`xml_fill`/`single_fill`，即三种绕开结构化解析的填充方式）、`ReviewMode`（`human`/`auto`）、`ReviseMode`（`human`/`human_review`/`auto`）。

## 怎么实现的

**编译：三段拼一张提示。** `compile` 直接处理 `schema == "raw"` 的特例——只有 context、语言约束和 instruction，不生成例。其余情况走 `SIMPLE_TEMPLATE`，四个槽位分别是 context、格式示例、字段说明、约束。

字段说明永远用 markdown 编译（`compile_instruction(schema="markdown")`），格式示例才按调用方要求选 json 或 markdown。这个非对称是源码注释里明确写了原因的：json 形式的 instruction 会带来格式问题，注释举的例子是 `"Project name": "web_2048  # 项目名称使用下划线"`——把说明混进 JSON 值里，模型容易当成真值输出。

`compile_example` 里那段注释更细：不能对 example 用 f-string，因为转成 str 后 `json.dumps` 会再套一层引号，示例里的 list 变成字符串 `"['main.py']"`。所以它把 example 原样交给 `compile_to`。

**动态建类：把树翻译成 pydantic 模型。** `_get_children_mapping` 递归生成 `{字段名: (类型, Field(default=example, description=instruction))}`，嵌套子节点则生成嵌套 dict。`create_model_class` 把这些映射交给 `create_model`，同时注一个 `mode="before"` 的校验器 `check_fields`：

- 用 `is_optional_type` 判断哪些字段可缺省（`Optional[X]` 即 `Union[X, None]` 且只有两个参数）。非可选的字段若缺失就 `raise ValueError(f"Missing fields: ...")`。
- 多余字段只 `logger.warning`，不报错。

这个不对称很重要：缺字段是硬错误，多字段是软告警。多给的字段通常无害（模型把说明文字也当字段名了），而缺字段会让下游拿到 `None`。

**为什么需要注册表。** `create_model` 每次调用都返回**不同的类对象**，即使类名与字段定义完全相同。于是同一个节点被填两次会得到两个互不相等的类型，`isinstance` 判断和相等比较全部失效。`register_action_outcls` 用一个字符串键缓存：把调用参数拼成 `"<调用者类名>_<class_name>_<排序后的 mapping>"`，并做 `typing.List → list`、`typing.Dict → dict` 的归一（否则 `List[str]` 与 `list[str]` 会算出两个键，但其实是同一个模型）。dict 参数先 `sorted` 再拼，保证键与插入顺序无关。

**解析：两条路。** `_aask_v1` 带 `tenacity` 重试（指数退避 1 到 20 秒，最多 6 次）——因为解析失败在这个设计里是常态而非异常。

按 schema 分岔：`json` 走 `llm_output_postprocess`，把模型的 JSON Schema 交给后处理器并要求 `req_key=f"[/{TAG}]"`，即先截出 `[CONTENT]...[/CONTENT]` 段再解析；其余（markdown）走 `OutputParser.parse_data_with_mapping`，按 `## ` 切块、逐块尝试剥掉代码围栏，再按 mapping 里的类型决定要不要当文件列表解析。

两条路都返回 dict，最后 `output_class(**parsed_data)` 交给 pydantic 校验。所以「解析」和「类型校验」是分开的两步：解析器只负责把文本变 dict，类型正确性完全由动态模型负责。

**三种绕行填充。** `fill` 在走进结构化路径之前先看 `mode`：

- `code_fill`：直接把 context 当提示，模型输出过 `sanitize` 抽出代码块，塞进单字段模型。
- `xml_fill`：先用 `xml_compile` 在 context 后追加「每个字段一个 `<tag>content</tag>`」的格式要求，模型输出后用正则逐字段抠。抠出来的值按 `field_types` 手工转型，失败就有默认值（`int` 失败给 0，`list` 失败给 `[]`）。
- `single_fill`：最简，模型输出整段作为唯一字段的值。

三者都跳过编译与结构化解析，因为它们的产出形态单一（一段代码、一组标签、一整段文本），用 JSON 反而更脆。

**递归注入。** `set_llm` / `set_context` 走 `set_recursive`，把同一个值写进自己和全部子孙节点。这样 `fill` 只需在根节点上设一次，子树都能用。

**评审与修复是「先圈定再重建模型」。** `auto_review` 只让模型为「不匹配的字段」给意见，所以它不知道哪些字段会出问题。做法是先拿全部 key 去模型输出里找 `"key":` 这个字面串，命中的进 `include_keys`；再用 `exclude` 参数重建一个只含这些字段的模型去解析。没命中的字段自然被排除，模型也就不必为它们编造值。

`auto_revise` 反着来：先拿到评审意见，`exclude` 掉没有意见的字段，用剩下的字段编译示例与说明，再填一次，最后 `update_instruct_content` 把结果并回原对象。

## 关键取舍

**源码事实：** 提示里明确要求「Continue in the same language as input」与「把输出包在 `[CONTENT]` 里，其它什么都不要」。语言约束与格式约束被写成两个常量拼进每个提示，不靠模型自觉。

**源码事实：** 缺字段报错、多字段只告警。这让「模型少给了必填项」立即失败并触发重试，而「模型多给了点东西」不至于让整次调用白费。

**源码事实：** 类型转换在 xml_fill 里用 `eval`（`list`/`dict` 两种），失败静默退到空容器。这是已知的粗糙处：`eval` 对不可信输入不安全，但由于输入来自自己构造的 XML 标签内，风险被限制在模型输出范围内。

**架构推论：** 「动态 pydantic 模型 + 注册表缓存」这一组合，使「字段结构」既是提示的一部分又是校验的一部分，且两者永远同步——因为都从同一个 mapping 派生。代价是节点树的形状只能在运行期确定，静态类型检查器看不到这些字段。

**架构推论：** 解析失败用重试而不是「尽力修」来解决。tenacity 的 6 次重试意味着一次格式跑偏会连带消耗 6 次模型调用；这是用成本换成功率，也解释了为什么 `fill` 上还挂了 `exp_cache` ——同样的 (req, llm, schema, mode) 命中经验池就不再花这次钱。

**架构推论：** `schema="raw"` 是唯一的非结构化路径，且它连 example 都不生成。因此「raw 节点」本质上是「把 instruction 当普通提示用」，`instruct_content` 为 `None`，后续 review/revise 直接不可用（`assert self.instruct_content` 会挡住）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/actions/action_node.py` | `ActionNode` | 节点树：声明字段、编译提示、填充与评审 |
| `metagpt/actions/action_node.py` | `ActionNode.compile` | 拼装 context/example/instruction/constraint 四段提示 |
| `metagpt/actions/action_node.py` | `ActionNode.create_model_class` | 按 mapping 动态建 pydantic 模型并注入必填校验 |
| `metagpt/actions/action_node.py` | `ActionNode.is_optional_type` | 识别 `Optional[X]` 以决定字段是否可缺省 |
| `metagpt/actions/action_node.py` | `ActionNode._aask_v1` | 带退避重试的提问与双路解析 |
| `metagpt/actions/action_node.py` | `ActionNode.fill` | 三种绕行模式与 simple/complex 两种填充策略 |
| `metagpt/actions/action_node.py` | `ActionNode.auto_review` | 按输出里实际出现的 key 重建子集模型 |
| `metagpt/actions/action_node.py` | `ActionNode.auto_revise` | 按评审意见圈定字段后重填并并回 |
| `metagpt/actions/action_node.py` | `FillMode` | 代码块、XML 标签、整段文本三种绕行填充 |
| `metagpt/actions/action_node.py` | `ReviewMode` | 人工评审与自动评审的二选一 |
| `metagpt/actions/action_outcls_registry.py` | `register_action_outcls` | 按参数拼字符串键缓存动态类，保证同定义同类 |

## 参考

- [MetaGPT 源码：actions/action_node.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/action_node.py)
- [MetaGPT 源码：actions/action_outcls_registry.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/action_outcls_registry.py)
- [Pydantic 文档：动态模型创建](https://docs.pydantic.dev/latest/concepts/models/#dynamic-model-creation)

## 与主核 / AI 的关系

「先声明字段、再由声明同时生成提示与校验器」与主核里「表定义同时生成存储布局与解析代码」是同一个套路：单一来源派生出多个视图，避免两边漂移。主核的派生是 DDL 到 C 结构体，这里的派生是节点树到 pydantic 模型，都是编译期/装配期一次成型、运行期零判断。

对 agent 工程最实用的一条是「缺字段硬错、多字段软警」这条不对称规则。它意味着提示里可以大胆列举字段说明，不必担心模型把说明当字段名输出；也意味着一旦某个字段在提示里写得含糊，模型漏填会直接失败并重试 6 次——报错会很快暴露，而不是悄悄产出半个对象。写新的 SOP 动作时若发现某字段经常被漏，先查 instruction 是否足够具体，而不是去加默认值。
