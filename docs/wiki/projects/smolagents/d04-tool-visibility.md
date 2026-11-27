---
project: smolagents
feature: tool-visibility
title: 工具暴露与描述注入
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/tools.py
    symbols: [get_tools_definition_code, add_description]
  - path: src/smolagents/models.py
    symbols: [get_tool_json_schema]
---

# 工具暴露与描述注入

## 一句话职责

同一个工具要在三个完全不同的地方「被看见」——提示词文本、远程进程的命名空间、以及模型的 tool calling 接口——这三处各有各的格式要求，`to_code_prompt` / `get_tools_definition_code` / `get_tool_json_schema` 就是三条互不相同的投递通道。

## 机制是什么

工具的可调用性依赖三个条件同时成立，任何一个断了，模型就调不到它：

| 通道 | 谁消费 | 载体 | 实现 |
|---|---|---|---|
| 提示词 | 模型（读） | 模板里的 Jinja 调用 | `to_code_prompt` / `to_tool_calling_prompt` |
| 远程命名空间 | 远程执行器里的 Python | 可 `exec` 的源码字符串 | `get_tools_definition_code` |
| 接口 schema | 供应商 API | `tools` 参数里的 JSON | `get_tool_json_schema` |

三条通道的数据都来自同一份 `inputs` / `output_type` / `description`，但**转译规则各不相同**。这不是重复实现，而是三种媒介的约束不同：提示词是给人（模型）读的自然语言环境，远程命名空间要求可执行，API 要求 JSON Schema 的严格子集。

## 怎么实现的

**第一条：提示词。** 由 `prompts/*.yaml` 里的 Jinja 表达式决定，见 c01：代码式智能体用 `{{ tool.to_code_prompt() }}`，工具调用式用 `{{ tool.to_tool_calling_prompt() }}`。这里没有额外逻辑——**工具只是被模板拉进文本**。

值得一提的是模板拿到的变量是 `{"tools": self.tools, ...}`，即**名字到工具的字典**，所以循环写法是 `{% for tool in tools.values() %}`。`MultiStepAgent` 里另有 `tools_and_managed_agents` 属性把工具与受管子智能体拼成一个列表，供 tool calling 通道使用（见下）。

**第二条：远程命名空间。** `get_tools_definition_code(tools)` 产出一段完整可执行的 Python，结构是三段拼起来：

1. **基础导入**：`"\n".join(f"import {module}" for module in BASE_BUILTIN_MODULES)`，即把 `collections`、`datetime`、`itertools`、`math`、`queue`、`random`、`re`、`stat`、`statistics`、`time`、`unicodedata` 全部 import 进来。这样工具方法里用到这些模块时不必自己再导入——正好与 `MethodChecker` 的白名单对齐（见 d02）：静态检查允许的名字，运行时都真的存在。
2. **一个最小 `Tool` 桩**：手写一段字符串，定义 `class Tool:`，只有 `__call__`（转发给 `forward`）与空 `forward`。远程端不需要真的 smolagents 包，只需要这个转发语义。
3. **每个工具的类源码 + 实例绑定**：对每个工具先 `validate_tool_attributes(tool.__class__, check_imports=False)`（校验点见 d02），再 `instance_to_source(tool, base_cls=Tool)` 生成类定义，然后**把 `from smolagents.tools import Tool` 这一行替换成空串**（因为桩已经提供了 `Tool`），最后补一行 `\n\n{tool.name} = {tool.__class__.__name__}()`。

第 3 步最后一行的意义：远程端拿到的不只是类，还有**已经按工具名绑定好的实例**。所以模型生成的代码里 `web_search(query=...)` 能直接工作，不需要先实例化。注意左值是 `tool.name`（工具声明名）而非类名——名字可以合法地与类名不同。

这个函数由 `RemotePythonExecutor.send_tools` 调用，前置步骤是：先给 `final_answer` 打补丁（`_patch_final_answer_with_exception`，让远程端的最终答案通过异常传出），再用 `tool.to_dict()["requirements"]` 汇总所有依赖并 `pip install`（其中 `PIL` 被改写成 `pillow`——导入名与包名不一致的已知一例），最后才注入源码。

**第三条：接口 schema。** `get_tool_json_schema(tool)` 返回标准 OpenAI 外形：

```
{"type": "function", "function": {"name", "description", "parameters": {"type": "object", "properties", "required"}}}
```

它先 `deepcopy(tool.inputs)`（避免污染工具定义——这一步是必须的，因为后面要**就地改写**），然后对每个参数做三件事：

- **`any` 改写成 `string`**。JSON Schema 里没有 `any`，所以 `output_type = "any"` 或某个参数声明为 `any` 时，线上必须给一个具体类型。这是三条通道里唯一的**语义妥协**：提示词里可以说 `-> any`，接口上只能说 `string`。
- **必填由 `nullable` 反推**：没有 `nullable` 或它为假 → 进 `required` 列表。
- **`anyOf` 展平**：若参数 schema 里有 `anyOf`（联合类型的产物，见 d03），就遍历子 schema，跳过 `type == "null"` 的（同时给参数置 `nullable = True`），把 `any` 子项换成 `string`，收集其余类型名；最后 `type` 写成列表（多于一个类型时）或单值；若某个子项带 `enum` 就提到顶层；然后 `value.pop("anyOf")`。

这段展平逻辑等价于把 `Optional[X] | Y` 重新压成供应商能接受的「类型列表 + nullable + enum」三件套。注释里说「assuming there is only one enum in anyOf」——这是一个明确的简化假设。

它的调用点在 `_prepare_completion_kwargs` 里：`if tools_to_call_from: completion_kwargs["tools"] = [get_tool_json_schema(tool) for tool in tools_to_call_from]`，紧接着 `tool_choice` 也只在有工具时才传。`ToolCallingAgent` 传进来的是 `self.tools_and_managed_agents`（工具 + 受管子智能体），所以**受管子智能体在被调用时也是以「一个函数工具」的形式出现在接口上的**。

**`add_description` 是一个未被使用的辅助。** 它是个装饰器工厂：`inner(func)` 里给函数对象挂 `func.description = description` 与 `func.name = func.__name__`，然后返回原函数。整个仓库里它只有定义、没有调用点。它的存在意义大概是为早期的「函数级工具」留兼容口（给函数贴上 `description`/`name` 属性，再交给 `@tool` 或某种加载逻辑读取）。当前 `@tool` 走的是 `get_json_schema` 从 docstring 推导，完全不需要这两个属性。**读源码时容易误以为它是主路径的一部分——实际上不是**。

**三条通道的信息损失各不相同。** 把它们并排看能看出设计取向：

| 信息 | 提示词 | 远程源码 | 接口 schema |
|---|---|---|---|
| `any` 类型 | 保留 | 保留（源码不改） | 降级为 `string` |
| `output_schema` | 完整写入 `Returns:` 段 | 作为类属性导出 | **不带**（接口只需参数 schema） |
| 参数描述 | 逐条列出 | 作为类属性随 `inputs` 导出 | 保留在 `properties` 里 |
| `nullable` | 只在类型里体现 | 随 `inputs` 导出 | 转成「是否进 `required`」 |
| `enum` | 只作为文本 | 随 `inputs` 导出 | 提为 `enum` 关键字 |

## 关键取舍

**源码事实：** 工具的暴露有三条独立通道，各有各的转译实现，共享同一份 `inputs` / `output_type` / `description`。

**源码事实：** 远程注入的源码里包含一个手写的最小 `Tool` 桩，且会把工具源码里 `from smolagents.tools import Tool` 这一行删掉——远程端不需要真的安装 smolagents。

**源码事实：** 注入时会给每个工具补一行 `{tool.name} = {类名}()`，使工具在远程端以**同名实例**出现，模型代码可直接调用。

**源码事实：** `get_tool_json_schema` 把 `any` 改写成 `string`，并展平 `anyOf`（跳过 null、提 enum 到顶层、`pop` 掉 `anyOf`）。它先 `deepcopy` 再就地改写。

**源码事实：** `add_description` 在整个仓库里没有调用点，是一条死路径。

**架构推论：** 三条通道并存是「一种工具、多种智能体、多种执行环境」的必然结果。每新增一种消费方（新的智能体范式、新的执行器、新的供应商），就要新增或修改一条通道，且要重新回答「这份信息在这里怎么表示」。`any` → `string` 的降级就是这类转译必然会产生的语义流失，它无法被消除，只能被记录。

**架构推论：** 远程注入选择「源码 + 同名实例绑定」而不是「RPC 描述」，意味着远程端拿到的是**行为**而非**契约**。好处是模型代码里可以直接把工具当函数用（与 `to_code_prompt` 承诺的形态一致）；坏处是每条通道都要单独维护自包含性（因此 d02 的静态校验在这一条通道上是必需的）。

**架构推论：** `add_description` 的残留说明了这类框架的一个真实状态：函数级工具与类级工具两个时代并存过，清理不彻底。这类遗留不会造成 bug，但会误导读者（也包括读代码的模型），因此值得在文档层面写明「它不是主路径」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/tools.py` | `get_tools_definition_code` | 生成远程注入源码：基础导入 + `Tool` 桩 + 各工具类 + 同名实例绑定 |
| `src/smolagents/tools.py` | `add_description` | 给函数挂 `description`/`name` 属性的装饰器，当前无调用点 |
| `src/smolagents/tools.py` | `to_code_prompt` / `to_tool_calling_prompt` | 提示词通道的两种渲染（详见 c01） |
| `src/smolagents/models.py` | `get_tool_json_schema` | 工具 → OpenAI 风格 function schema，含 `any` 降级与 `anyOf` 展平 |
| `src/smolagents/models.py` | `_prepare_completion_kwargs` | 注入点：`tools_to_call_from` 非空时才传 `tools` 与 `tool_choice` |
| `src/smolagents/agents.py` | `tools_and_managed_agents` | 把工具与受管子智能体合并成接口层的一个列表 |
| `src/smolagents/remote_executors.py` | `send_tools` | 远程通道的编排：补丁 final_answer → 装依赖 → 注入源码 |
| `src/smolagents/remote_executors.py` | `_patch_final_answer_with_exception` | 让远程端的最终答案通过异常传出 |

## 参考

- [smolagents 源码：tools.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/tools.py)
- [smolagents 源码：models.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/models.py)
- [smolagents 文档：工具如何呈现给模型](https://huggingface.co/docs/smolagents/tutorials/tools)
- [OpenAI 文档：function calling](https://platform.openai.com/docs/guides/function-calling)

## 与主核 / AI 的关系

「同一份定义，多种消费方，各自转译」这件事在主核里对应的是**协议层与执行层的分野**：一份表定义既要给客户端协议描述（列名与类型）、又要给执行器实际布局（页格式、编码），两者不能共用同一个表示，因为协议要稳定可读、布局要紧凑高效。smolagents 的三条通道性质相同：提示词要自然语言化、远程源码要可执行、接口 schema 要符合供应商子集。

信息损失的位置也很像。主核里 `numeric` 到客户端可能被描述得更宽泛，内部却按精确小数存储；这里 `any` 在提示词与源码里保留，在接口上被降级成 `string`。**只要能选出「哪一侧是权威表示」，这类损失就是可控的**——smolagents 的权威表示是 `inputs`，另三条通道都是它的投影。

`add_description` 这条死路径值得作为反面参照：主核里也有类似的废弃接口，留着不删的理由通常是「怕外部还在用」。但在一份要被人和模型共同阅读的代码库里，**未使用的公开函数就是一份错误的说明书**。smolagents 把它留着，代价是每个读 `tools.py` 的人都要花时间判断它是否在主路径上。

对 AI 工程的实际意义在于：工具「能被调用」不是一件事，而是三件事同时成立。实践中出问题最多的往往不是 schema 写错，而是三条通道不同步——例如工具加了新参数但提示词模板没更新（模型不知道有这参数）、或者依赖没进 `requirements`（远程端 import 失败）。这套实现给出的可借鉴做法是：**让三条通道都从同一份声明派生，并且把派生动作放在尽可能靠后的统一位置**（模板、`send_tools`、`_prepare_completion_kwargs` 各一个收敛点），这样新增一个字段时只需要改声明与三处投影，而不是散落在各工具里。
