---
project: haystack
feature: component-checks
title: 组件就绪判定
page_type: feature
schema_version: 1
refs:
  - path: haystack/core/pipeline/component_checks.py
    symbols: [can_component_run, are_all_sockets_ready, has_any_trigger]
  - path: haystack/core/pipeline/component_checks.py
    symbols: [has_lazy_variadic_socket_received_all_inputs, can_not_receive_inputs_from_pipeline, has_user_input]
---

# 组件就绪判定

## 一句话职责

在一个数据流管道里判断「这个组件此刻能不能跑」——它必须同时满足「必填输入都到齐」与「有一个触发理由」，两个条件都成立才返回真。

## 机制是什么

haystack 的管道是一张组件 DAG，运行循环每一轮都在问同一个问题：哪些组件可以跑了？这个问题看起来简单，实际要处理四类边界：

1. **必填 vs 可选输入**。必填 socket 必须都有值，可选 socket 没值也能跑。
2. **可变参数 socket**。一个 socket 可能接多个上游，什么时候算「齐了」分两种情况：贪婪型（来一个就跑）与懒惰型（等所有上游都跑完）。
3. **触发**。光有输入不等于该跑——组件可能已经跑过了，输入被重复填充。所以还要判断「有没有新的理由让它再跑一次」。
4. **上游明确没有产出**。上游跑了但某个输出没产出，这与「上游还没跑」是两件事，必须能区分。

这四条合起来构成了判定层的全部复杂度。它被拆成两个独立入口 `are_all_sockets_ready`（输入是否齐）与 `has_any_trigger`（是否该跑），再由 `can_component_run` 组合。

## 怎么实现的

**`can_component_run` 只是两个判断的与。** 它调 `are_all_sockets_ready(..., only_check_mandatory=True)` 与 `has_any_trigger`，返回二者的逻辑与。拆开的理由很实际：**这两种失败需要不同的诊断**——输入没齐说明上游还没跑完（正常等待），没有触发说明这个组件已经跑过且没有新输入（该停）。合成一个布尔就无法区分。

**`are_all_sockets_ready` 的检查范围受 `only_check_mandatory` 控制。** 只检查必填时，取所有 `is_mandatory` 的 socket；否则取「必填 or 有上游」的 socket。第二种口径的意义是：**可选但接了上游的 socket 也要等**，否则会在上游还没跑完时提前开跑，拿到的就是半份输入。

判定本身用两个集合：`filled_sockets` 与 `expected_sockets`，最后比较相等。填充条件是两个分支的或——`has_socket_received_all_inputs` 为真，**或者**「是懒惰可变 socket 且收到了任意输入」。第二个分支是给懒惰可变 socket 的宽容：它不要求全部上游都到齐，只要至少有一个就允许开跑。

**`has_socket_received_all_inputs` 按 socket 种类三分支。** 顺序很重要：

- 没有输入 → 直接假。
- **贪婪可变**且至少一个输入不是「无产出」标记 → 真。
- **懒惰可变**且所有期望上游都产出了 → 真。
- 否则（非可变）→ 只看第一个输入是不是「无产出」标记。

非可变分支只看 `socket_inputs[0]`，因为非可变 socket 只接一个上游，列表里最多一项。

**「无产出」用哨兵类而不是 `None`。** `_NoOutputProduced` 是一个空类，注释把理由写得很明确：**必须用 `isinstance` 判断，绝不能用 `==`**——因为 `==` 会调用被检查值的 `__eq__`，而像 DataFrame 这样的对象比较结果不是布尔。它实现了 `__eq__`/`__hash__` 让自身实例可互换，还实现了 `to_dict`/`from_dict`，理由是**它会随管道输入进入快照**，所以必须可序列化。

这个哨兵解决了第四类边界：`None` 无法区分「上游明确输出 None」与「上游没输出这个 socket」；空列表也无法区分「没接到上游」与「接到了但没值」。

**`has_any_trigger` 有三个触发来源。** 前置组件给了输入、用户从管道外部给了输入、或者这个组件根本不可能从管道里收到输入（纯起点）且管道被调用过。后两个都带 `component["visits"] == 0` 条件——**只在首次执行时成立**。

三个来源都受限的理由，文档字符串逐条列了：组件执行前会消费输入（删除），所以前置输入只能触发一次；外部输入只在首次执行时算触发；`Pipeline.run` 同理。**这就是「触发只能发生一次」的实现方式**——不是靠一个「已运行」标志，而是靠「输入被消费 + visits 计数」两个天然事实。

**`visits` 计数存在组件元数据里。** 它不是组件的字段，而是管道运行期在 `component` 字典上维护的计数。这让判定函数保持无状态：给定组件元数据与输入，结果完全确定。

**几个辅助判定的语义差异值得记。** `any_socket_value_from_predecessor_received` 要求「值不是无产出标记 **且** sender 不为 None」——两个条件同时成立才算「前置给了值」，sender 为 None 表示来自管道外。`has_user_input` 则相反，只找 sender 为 None 的项。`can_not_receive_inputs_from_pipeline` 要求**所有** socket 的 senders 都为空。

`all_socket_predecessors_executed` 与 `has_lazy_variadic_socket_received_all_inputs` 看起来相似但不同：前者比较「期望的 sender 集合」与「输入里的 sender 集合」，**不管值是不是无产出标记**；后者则**排除无产出标记**。这个差别决定了两者用途不同——前者回答「上游都跑过了吗」，后者回答「上游都给出值了吗」。

## 关键取舍

**源码事实：** 就绪判定被拆成「输入齐」与「有触发」两个入口，由 `can_component_run` 取与。拆开的理由是两种失败的诊断含义不同。

**源码事实：** 「上游明确无产出」用哨兵类 `_NoOutputProduced` 表示，注释明确要求用 `isinstance` 而非 `==` 判断，因为被检查值可能是 DataFrame 这类 `__eq__` 不返回布尔的类型。它还必须可序列化（`to_dict`/`from_dict`），因为它会进管道快照。

**源码事实：** 可变 socket 分贪婪与懒惰两种。贪婪的「有一个输入就算齐」，懒惰的要「所有期望上游都产出」。`are_all_sockets_ready` 对懒惰可变 socket 额外放宽了「任意输入即可跑」。

**源码事实：** 三个触发来源中有两个带 `component["visits"] == 0` 条件，即只在首次执行时生效。

**源码事实：** `all_socket_predecessors_executed` 不排除无产出标记，而 `has_lazy_variadic_socket_received_all_inputs` 排除——两者语义不同，前者问「跑过没」，后者问「给出值没」。

**架构推论：** 用「输入被消费 + visits 计数」表达「触发只能一次」，而不是显式的已运行标志，好处是判定函数保持纯函数（无状态、结果可预测），坏处是**正确性依赖运行循环必须真的消费输入**。这是把不变量放在「数据流自然性质」上的做法——省了状态，但把约束分散到了调用方。

**架构推论：** 哨兵类的存在说明**数据流系统里「缺失」不是一种值而是一种状态**。用 `None` 当缺失会在「用户真的传了 None」时错判。这是所有数据流框架共有的问题，解法只有两种：哨兵对象，或者带 tag 的包装（Option/Result）。haystack 选了前者，代价是每个消费点都要记得用 `isinstance` 判而不是真值判。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/core/pipeline/component_checks.py` | `can_component_run` | 组合「输入齐」与「有触发」 |
| `haystack/core/pipeline/component_checks.py` | `are_all_sockets_ready` | 输入齐备判定，含必填/有上游两种口径 |
| `haystack/core/pipeline/component_checks.py` | `has_any_trigger` | 三个触发来源的判定 |
| `haystack/core/pipeline/component_checks.py` | `has_socket_received_all_inputs` | 按 socket 种类三分支 |
| `haystack/core/pipeline/component_checks.py` | `_NoOutputProduced` | 「上游明确无产出」的哨兵 |
| `haystack/core/pipeline/component_checks.py` | `can_not_receive_inputs_from_pipeline` | 纯起点组件判定 |
| `haystack/core/pipeline/component_checks.py` | `all_socket_predecessors_executed` | 上游是否都跑过（不排除无产出） |
| `haystack/core/pipeline/component_checks.py` | `has_user_input` | 管道外输入判定 |

## 参考

- [haystack 源码仓库](https://github.com/deepset-ai/haystack)
- [haystack 文档：管道](https://docs.haystack.deepset.ai/docs/pipelines)
- [haystack 文档：组件](https://docs.haystack.deepset.ai/docs/components)
- [haystack 源码：component_checks.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/pipeline/component_checks.py)

## 与主核 / AI 的关系

就绪判定与主核（多模态数据库内核）的**查询算子调度**是同一类问题：给定一张算子图与当前可用的中间结果，决定哪些算子可以执行。

**两种调度哲学。** 主核（以及大多数数据库）走**拉取式**：从根算子开始，向下要数据，算子在被要时才执行。haystack 走**推送式**：上游算完把值推进下游 socket，然后判断下游是否就绪。拉取式的就绪判定隐含在递归调用里（不需要显式判定函数）；推送式必须显式判定，因为「值到了」与「该算了」不是同一件事。代价就是要写这一整套 `component_checks`。

**「无产出」的表示。** 数据库里对应的是 NULL 与「列不存在」的区分——SQL 三值逻辑正是为此存在的。haystack 用哨兵对象表达「上游没给这个 socket」，与 SQL 的 NULL 在动机上完全一致：**必须能区分「没有值」与「值是空/未知」**。差别是 SQL 把它做进了类型系统（三值逻辑、`IS NULL`），haystack 用一个普通类加 `isinstance` 约定。

**可变 socket 与数据库的 variadic 算子。** 贪婪（来一个就跑）对应流式聚合的「增量输出」，懒惰（等全部）对应阻塞式聚合（sort、hash join 的 build 端）。数据库里这个选择由算子语义决定，haystack 里由 socket 声明决定。

**用输入消费表达「只触发一次」，与数据库的游标语义同构**：算子每次被调用都推进一次输入游标，读完就没了。差别是数据库把这状态放在执行器内部（`PlanState`），haystack 放在 socket 的输入列表里（执行前删除）。两者都是「不靠标志位，靠数据自然性质」的思路。
