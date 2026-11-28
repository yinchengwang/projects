---
project: dify
feature: agent-layers
title: Agent 能力分层
page_type: feature
schema_version: 1
refs:
  - path: dify-agent/src/dify_agent/layers/__init__.py
    symbols: [__all__]
  - path: dify-agent/src/dify_agent/runtime/modules.py
    symbols: [load_modules, create_modules, REGISTRY]
---

# Agent 能力分层

## 一句话职责

把 Agent 的每一项能力（模型、工具、知识库、提示词、输出契约、shell、配置、历史）做成一个**同构的层模块**：每层导出 `Config`、`State` 和一个原生组件（`Capability` 或 `Toolset`），由 `REGISTRY` 按名字登记，运行时按请求里的组合清单逐层装配。

## 机制是什么

十二个层目录，全部遵循同一个三件套契约：

| 层 | 模块 | 导出组件 | 提供什么 |
|---|---|---|---|
| `llm` | `dify_plugin/llm_layer` | `Capability` | 模型装配（含 daemon 侧用量统计） |
| `tools` | `dify_plugin/tools_layer` | `Toolset` | 插件工具 |
| `core_tools` | `dify_core_tools/layer` | `Toolset` | 核心内置工具（走 HTTP 调 API） |
| `knowledge` | `knowledge/layer` | `Capability` + 内部 `_KnowledgeTools` | 知识库检索 |
| `knowledge_fs` | `knowledge_fs/layer` | `Capability` | 知识文件系统检索 |
| `config` | `config/layer` | `Capability` | 运行期的配置读写工具 |
| `runtime` | `runtime/layer` | `Capability` | 运行时后端（工作区） |
| `shell` | `shell/layer` | `Capability` | shell 会话与命令执行 |
| `prompt` | `prompt/layer` | `Toolset` | 提示词片段 → 用户内容 |
| `user_prompt` | `user_prompt/layer` | `Capability` | 用户输入（含图片与文件下载） |
| `output` | `output/output_layer` | `Capability` | 输出契约（JSON Schema 校验） |
| `read_media` | `read_media/layer` | `Capability` | 读取媒体文件 |
| `history` | `layers/history` | `Capability` | 历史消息的读写 |

`layers/__init__.py` 只有三行，`__all__` 是空列表：

```
"""Dify modules and client-safe configuration DTOs."""

__all__: list[str] = []
```

**这是个刻意的空导出。** 所有层都靠各自模块路径 import（`from dify_agent.layers.prompt import layer as prompt`），包根不重导出任何东西。理由在 `modules.py` 的 docstring 里：「The registry is server-owned; HTTP callers never supply import paths or live objects.」——**一旦包根提供聚合导出，就会诱使人用 `getattr` 之类的方式按名字取实现**，而这个注册表必须是静态、可审计的。

## 怎么实现的

**`REGISTRY` 是一张「名字 → 模块对象」的静态字典，18 个键映射到 13 个模块。**

```
REGISTRY = {
    "agent_soul_prompt": prompt,
    "workflow_node_job_prompt": prompt,
    "workflow_user_prompt": prompt,
    "agent_app_user_prompt": user_prompt,
    "prompt": prompt,
    "user_prompt": user_prompt,
    ...
}
```

**同一个模块被挂多个名字**，这是这套设计的第一个要点：`prompt` 模块同时承担 `agent_soul_prompt`、`workflow_node_job_prompt`、`workflow_user_prompt`、`prompt` 四个角色。**所以「层」的单位不是「代码模块」，而是「组合里的一个槽位」**——同一份实现可以在不同宿主里以不同名字出现，各自有独立的 Config/State。

这个区分很实用：`agent_soul_prompt`（Agent 应用的人格提示）与 `workflow_user_prompt`（工作流节点的用户提示）语义不同、状态内容不同，但装配逻辑可以复用。

**`_REFERENCES` 声明层间引用，且引用目标是模块对象而不是名字。**

```
_REFERENCES = {
    llm: {"execution_context": execution_context},
    plugin_tools: {"execution_context": execution_context, "shell": shell},
    knowledge_fs: {"execution_context": execution_context, "shell": shell},
    shell: {"runtime": runtime, "execution_context": execution_context},
    config: {"shell": shell},
    read_media: {"execution_context": execution_context, "llm": llm, "shell": shell},
}
```

键是模块对象、值里也是模块对象。**用对象而不是字符串做键，是因为模块对象才有唯一性**——若用名字，`prompt` 那四个别名会让「引用的是哪个」变得含糊。而校验时用的判断是：

```
if target not in layers or registry[target] is not expected_module:
    raise ValueError(f"Module {name} reference {field} must name a configured {expected_module.__name__} module")
```

**`is not` 身份比较**，所以引用必须指向同一份模块对象，而不是「同名即可」。**这就把「用哪个别名」这件事精确地钉死了：`llm` 层的 `execution_context` 字段只能指向 `execution_context` 模块，指向一个恰好也叫这名字但其实是别的模块的槽位会被拒。**

**`load_modules` 是整整一段「在 I/O 之前把所有能校验的都校验掉」的实现。**

六道检查按顺序：

1. **层名不得重复**：`if len(set(names)) != len(names): raise ValueError("Module names must be unique")`；
2. **必须有 `llm`**：`if "llm" not in names: raise ValueError("Missing required 'llm' module")`；
3. **快照里的层名必须在注册表里**：`unknown = state.keys() - registry.keys()`，有未知就报错。docstring 写的是「Retained unknown state names are rejected, while known removed slots are omitted from the new snapshot」——**未知的会被拒，已知但本轮不再组合的会被安静丢弃**；
4. **每一层必须能解析到模块**，否则 `Unknown registered module`；
5. **每层必须恰好导出一个 `Capability` 或一个 `Toolset`**：`if has_capability == has_toolset: raise TypeError(...)` —— 两个都有或都没有都不行；
6. **导出的东西必须是原生 Pydantic AI 类型**：`if not issubclass(implementation, expected): raise TypeError(...)`。

第 5 与第 6 条合起来实现了一个强契约：**层只能用原生组件，不能自造抽象**。注释里那句「must use native Pydantic AI components」就是这条。

**每层都要过两遍 pydantic 校验，一遍进一遍出：**

```
cfg = config_type.model_validate(spec.config)
restored = state_type.model_validate(state.get(spec.name, {}))
configs[spec.name] = cfg
layers[spec.name] = {"config": cfg.model_dump(mode="json"), "state": restored.model_dump(mode="json")}
```

**注意 `state.get(spec.name, {})`**：缺状态时用空字典去校验。这意味着每个 `State` 模型的**所有字段必须有默认值**，否则「第一次运行」就会校验失败。这是一条隐含约定，写在各层 `State` 的定义里而不是校验代码里。

**校验完配置后，还有一轮「按模块特殊处理」的检查。**

```
for name, cfg in configs.items():
    module = registry[name]
    for field, expected_module in _REFERENCES.get(module, {}).items():
        target = layers[name]["config"][field]
        if target is None:
            continue
        if target not in layers or registry[target] is not expected_module:
            raise ValueError(...)
    if module is runtime and services.runtime_backend_profile is None:
        raise ValueError("Runtime module requires a configured runtime backend")
    if module is knowledge_fs and services.knowledge_session_store is None:
        raise ValueError("KnowledgeFS module requires the server-owned shared session store")
    if module is plugin_tools:
        for tool in cfg.tools:
            plugin_tools._validate_required_hidden_parameters(tool, tool.parameters)
    if module is knowledge:
        target = cfg.execution_context
        knowledge._build_caller_context(configs[target])
```

三种检查性质不同：

- **引用检查**（`_REFERENCES`）是通用的，所有层共享一套逻辑；
- **服务依赖检查**（`runtime` 要后端、`knowledge_fs` 要共享会话存储）是**按模块用 `is` 判断硬编码的**——只有两个特例，还没抽象成「模块声明自己需要什么服务」；
- **配置内部一致性检查**（插件工具的隐藏参数、知识库的 caller context）是**调用模块自己的私有函数**完成的。

最后一行 `knowledge._build_caller_context(configs[target])` 很有意思：**它不保存返回值，只是为了「提前失败」而调用一次**。也就是说这个函数在校验阶段被当作「纯校验器」用，真正的构建在执行阶段再做一次。

**`create_modules` 只有一行列表推导，但有两个要点。**

```
return {
    name: (registry[name].Capability(name) if hasattr(registry[name], "Capability") else registry[name].Toolset(name))
    for name in deps.layers
}
```

1. **每个组件都用 `name` 构造**（不是模块名）——于是 `prompt` 的四个别名会造出四个独立实例，各自持有自己的 `State`；
2. docstring 是「Construct fresh native instances for this run; no instance is restored.」——**不复用任何实例**，与前文 g02 里「每次运行创建全新组件」那条约束一致。

**`snapshot_modules` 是反向：只留 State，丢掉 Config。**

```
return SessionSnapshot(layers={
    name: registry[name].State.model_validate(entry["state"]).model_dump(mode="json")
    for name, entry in deps.layers.items()
})
```

docstring：「Validate and copy final state after resource cleanup, excluding all Config.」——**Config 不进快照**。理由很实际：Config 是「这次运行怎么配置的」，属于请求的一部分；而 State 是「这次运行之后需要记住什么」，属于下一步的输入。**把两者都存进快照，会让「换配置重放」变得不可能。**

**注意 `snapshot_modules` 只遍历 `deps.layers`**（本轮组合里的层），所以上一轮存在但这一轮不在组合里的 State 会**自然消失**——这与 `load_modules` 里「known removed slots are omitted from the new snapshot」那条 docstring 完全对应。

**层与层之间的关系靠 `Deps` 传，不靠全局变量。** `Deps` 里装 `layers`（配置与状态的 JSON 字典）、`services`、`run_id`，还有一个特殊字段 `tool_call_limit_store`——**它是运行期才挂上去的**（g02 里那句 `deps.tool_call_limit_store = self._tool_call_limit_store`），因为配额存储属于「这次运行」，不属于「这次请求」。

**为什么层必须是 `Capability`/`Toolset` 而不能是随便什么类？** 因为 `create_agent` 只接受这两类：

```
agent = create_agent(
    model,
    toolsets=[module for module in modules.values() if isinstance(module, AbstractToolset)],
    capabilities=capabilities,
    output_type=output_type,
)
```

装配端靠 `isinstance` 筛，所以「层导出什么类型」直接决定「它被当成工具集还是能力」。**`Capability` 能挂钩子（改提示、拦工具调用、管生命周期），`Toolset` 只提供工具**——这个二选一决定了层的表达能力上限。

**十二个层的代码量差了两个数量级，这本身说明抽象的位置选得对不对。**

按层文件的实际行数量级：`shell/layer.py` 近八百行（含 `ShellSession` 完整会话管理）、`dify_plugin/tools_layer.py` 六百余行（含文件参数归一、blob 合并、错误文本）、`knowledge/layer.py` 五百余行（含检索、rerank、metadata 过滤、eager 检索）、`output/output_layer.py` 三百五十行（含 JSON Schema 引用内联与校验）、`config/layer.py` 三百四十余行；而 `prompt/layer.py` 只有三十一行、`history.py` 十五行代码量级、`runtime/layer.py` 四十行、`dify_plugin/llm_layer.py` 四十二行。

**这说明「层」这个抽象只统一了「接口形状」，没有统一「内部复杂度」**。短层几乎只是把配置与状态搬运一遍；长层在层内部实现了一整套子系统。

**最短的那层值得完整看一遍，它把三件套契约压到了极限。** `prompt/layer.py` 的 `State` 是一个**只有 `extra="forbid"` 配置、零字段**的模型：

```
class State(BaseModel):
    model_config = ConfigDict(extra="forbid")
```

零字段却仍然要声明，因为装配器要求每层都有 `State` 类型可校验。而 `Config` 有三个字段 `prefix`/`suffix`/`user`，`Toolset.__init__` 把槽位名存成 `self.name` 并传给父类的 `id`，再用 `instructions=self._render_instructions` 挂一个**惰性渲染函数**——它直到模型真正开始运行才被调用，届时才去 `deps.layers[self.name]["config"]` 取配置。

这是「配置在装配期校验、在运行期渲染」的具体形态：**校验用一份 pydantic 模型，渲染再 `model_validate` 一次**。两次校验听起来冗余，但它让 `Toolset` 不持有配置对象，只持有槽位名——**组件因此是无状态的，状态全在 `Deps` 里**。

`build_user_content` 是这条思路的延伸：它返回 `list[UserContent]`，被 g02 的装配段收集成用户输入。**一个 Toolset 同时承担「渲染 instructions」与「贡献用户内容」两件事**，因为这两个钩子都长在 `FunctionToolset` 上。

**`history.py` 是全仓最能体现「当前指令从不存储」这条约束的地方。** 它 43 行代码里有一行是：

```
persistent = [replace(m, instructions=None) if isinstance(m, ModelRequest) else m for m in messages]
```

**每个 `ModelRequest` 的 `instructions` 都被置空后才落盘。** 理由在模块 docstring：「Conversation persistence in JSON state; run instructions remain transient.」——指令属于「这次运行怎么配的」，与 g03 里「Config 不进快照」是同一个判断，只是这里更细：**连混在消息里的 instructions 也要剥掉**。

`save_messages` 还有一段针对中断的历史修复：

```
if interrupted and persistent:
    last = persistent[-1]
    if isinstance(last, ModelResponse) and last.state == "complete" and last.tool_calls:
        persistent[-1] = replace(last, state="interrupted")
```

docstring 解释得很清楚：「Pydantic AI records interruption on the in-progress request when a complete response's tool calls were not followed by a model request. Older captured histories may instead leave the response itself complete, so mark that trailing tool-call response interrupted for native history repair.」——**新版框架把中断记在「未完成的 request」上，老版历史可能把带 tool_calls 的 response 记成 complete**，于是要把末尾那条改标成 `interrupted`，否则下次运行会认为那些工具调用已经完成。

## 关键取舍

**源码事实：** 所有层遵循 `Config` + `State` + 恰好一个（`Capability` | `Toolset`）的三件套；`load_modules` 校验「不能两个都有也不能都没有」且必须是原生 `AbstractCapability`/`AbstractToolset` 子类。

**源码事实：** `REGISTRY` 里 18 个名字对应 13 个模块，`prompt` 与 `user_prompt` 各承担多个槽位名；组件实例用槽位名构造，因此同一模块的多个槽位互相独立。

**源码事实：** `_REFERENCES` 用模块对象做键和值，校验用 `registry[target] is not expected_module` 的身份比较。

**源码事实：** `State` 缺失时用空字典去 `model_validate`，因此每个 `State` 的所有字段必须有默认值。

**源码事实：** 服务依赖检查（`runtime` 要后端、`knowledge_fs` 要共享会话存储）是按模块 `is` 判断硬编码的两个特例。

**源码事实：** `knowledge._build_caller_context` 在校验阶段被调用一次（丢弃返回值）只为提前失败，执行阶段会再调一次。

**源码事实：** `snapshot_modules` 只遍历 `deps.layers`，只存 State 不存 Config；上一轮有、这一轮没组合的层会从快照里消失。

**源码事实：** 层的代码量差两个数量级：`shell` 近八百行、`dify_plugin/tools_layer` 六百余行、`knowledge` 五百余行、`output` 三百五十行、`config` 三百四十余行；而 `prompt` 只 31 行、`history` 43 行、`runtime` 40 行、`llm_layer` 42 行。

**源码事实：** `prompt/layer.py` 的 `State` 是零字段模型（只有 `extra="forbid"`），`Toolset` 只持有槽位名、把 `instructions` 挂成惰性渲染函数，运行期再 `Config.model_validate` 一次。

**源码事实：** `history.save_messages` 在落盘前把每个 `ModelRequest` 的 `instructions` 置空；中断时若末尾是「complete 且带 tool_calls 的 `ModelResponse`」，把它重标为 `interrupted`。

**架构推论：** 「层 = 配置 + 状态 + 一个原生组件」这套统一形状，把握手成本降到极低：加一个能力 = 加一个目录 + 在 `REGISTRY` 加一行。代价是**这个统一形状对内部复杂度毫无约束**——长层在内部自建子系统，短层几乎只是搬运。统一接口解决的是「能不能装进来」，不解决「装进来之后多复杂」。

**架构推论：** 用模块对象（而非字符串）做 `_REFERENCES` 的键值，是一条被「别名」逼出来的正确做法。只要一个模块能挂多个名字，「按名字引用」就有歧义；用身份比较把歧义消掉，代价是引用关系无法序列化（它只存在于代码里，不进请求）。

**架构推论：** `State` 字段必须有默认值这条隐含约定，是「用空字典初始化」这个实现的必然推论。**这类「由实现细节决定的数据模型约束」最容易被忘记**，因为它不出现在任何接口文档里，只在某次新增字段忘了给默认值时以运行期错误暴露。

**架构推论：** 「只存 State 不存 Config」是正确的状态设计：状态是「需要记住的」，配置是「这次怎么配的」。反过来会让快照与配置耦合，导致「换配置重放」不可能。同理「移除的层从快照消失」保证快照始终精确反映当前的组合。

**架构推论：** 「每个组件用槽位名构造」让同一实现能在不同槽位各自持有状态。这是必要的——`agent_soul_prompt` 与 `workflow_user_prompt` 的历史不同，不能让它们共享一个实例的状态。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `dify-agent/src/dify_agent/layers/__init__.py` | `__all__` | 刻意的空导出，阻止按名取实现 |
| `dify-agent/src/dify_agent/runtime/modules.py` | `REGISTRY` | 18 个槽位名到 13 个模块的静态映射 |
| `dify-agent/src/dify_agent/runtime/modules.py` | `_REFERENCES` | 层间引用的身份级声明 |
| `dify-agent/src/dify_agent/runtime/modules.py` | `load_modules` | I/O 前的六道校验与逐层配置/状态还原 |
| `dify-agent/src/dify_agent/runtime/modules.py` | `create_modules` | 按槽位名造全新原生组件实例 |
| `dify-agent/src/dify_agent/runtime/modules.py` | `snapshot_modules` | 清理后只快照 State |
| `dify-agent/src/dify_agent/layers/prompt/layer.py` | `Toolset` | 零状态层的极限形态：只持槽位名 + 惰性渲染 instructions |
| `dify-agent/src/dify_agent/layers/user_prompt/layer.py` | `Capability` | 用户输入（图片与文件下载） |
| `dify-agent/src/dify_agent/layers/history.py` | `save_messages` | 剥掉 instructions 并修复中断状态的历史落盘 |
| `dify-agent/src/dify_agent/layers/history.py` | `load_messages` | 从 State 读回历史 |
| `dify-agent/src/dify_agent/layers/output/output_layer.py` | `Capability` | 输出契约与 JSON Schema 引用内联 |
| `dify-agent/src/dify_agent/layers/shell/layer.py` | `Capability` | shell 会话与命令执行 |
| `dify-agent/src/dify_agent/layers/shell/layer.py` | `ShellSession` | 会话级命令执行与输出归一 |
| `dify-agent/src/dify_agent/layers/knowledge/layer.py` | `Capability` | 知识库检索能力 |
| `dify-agent/src/dify_agent/layers/knowledge/layer.py` | `_KnowledgeTools` | 检索工具集（层内部自建） |
| `dify-agent/src/dify_agent/layers/dify_plugin/tools_layer.py` | `Toolset` | 插件工具集与文件参数归一 |
| `dify-agent/src/dify_agent/layers/dify_core_tools/layer.py` | `Toolset` | 核心内置工具（HTTP 调用） |
| `dify-agent/src/dify_agent/layers/history.py` | `Capability` | 历史读写 |
| `dify-agent/src/dify_agent/layers/config/layer.py` | `Capability` | 运行期配置读写工具 |
| `dify-agent/src/dify_agent/layers/runtime/layer.py` | `Capability` | 运行时后端（最薄层之一） |
| `dify-agent/src/dify_agent/layers/execution_context/layer.py` | `Capability` | 执行上下文与追踪配置 |
| `dify-agent/src/dify_agent/runtime/context.py` | `Deps` | 层运行时依赖容器 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：dify_agent/runtime/modules.py](https://github.com/langgenius/dify/blob/main/dify-agent/src/dify_agent/runtime/modules.py)
- [Pydantic AI 文档：Capabilities](https://ai.pydantic.dev/)
- [dify 文档：Agent 节点](https://docs.dify.ai/en/cloud/use-dify/nodes/agent)

## 与主核 / AI 的关系

这套「层组合」与主核里的**扩展（extension）+ 参数（GUC）**机制是同构的：每个扩展声明自己的配置项、注册自己的钩子、持有自己的持久状态；启动时按 `shared_preload_libraries` 装哪些扩展决定能力面。差别在于**主核的扩展只能增加「钩子实现」，不能替换执行内核**；而 `dify-agent` 的层可以替换模型、提示、输出契约——因为它的内核（`pydantic_ai.Agent.run`）本身极薄。

**「`__all__` 为空」vs 数据库的系统表白名单。** 数据库只允许通过 `pg_extension` 登记过的扩展被加载，不允许「按路径点加载」。`layers/__init__.py` 的空导出是同一姿态的 Python 版：**能力面必须由一份静态清单定义，不能由运行时字符串决定**。这与 f01 里 `_handle_plugin_daemon_error` 用类名字符串分派形成对照——后者是「两侧独立发版、只能共享代码里的事实」，前者是「单仓、可以钉死清单」。

**「只快照 State」vs 数据库的 `pg_class`/`pg_attribute` 元数据。** 数据库改了配置（表结构）之后，元数据是持久化的，重启后还在；而运行时的某些配置（如 `work_mem`）不属于表定义。`dify-agent` 把这条界限画得很清楚：Config 是「一次运行怎么配」，State 是「跑完要记住什么」。**混在一起的后果是快照无法跨配置复用。**

**短层 vs 长层 vs 数据库的两种扩展。** PostgreSQL 里 `pg_stat_statements` 是薄扩展（几十行挂一个钩子），`PostGIS` 是厚扩展（自带类型系统、索引、函数库）。两者共用同一个 `CREATE EXTENSION` 入口——**统一入口不代表统一复杂度**，这是任何插件体系都要接受的事实。`dify-agent` 的十二层完全重演了这个分布。

对 agent 工程四条结论。其一：**能力的单位应该是「组合槽位」而不是「代码模块」**——同一实现挂多个槽位，让不同宿主复用装配逻辑而各自持有状态。其二：**层间引用要用对象身份校验，不要用名字**，否则别名会让引用变歧义。其三：**状态与配置必须分开存储**，只快照状态才能跨配置复用。其四：**「必须恰好导出一个 Capability 或 Toolset」这类强制契约要写在装配期**，让加层的人在跑起来之前就知道自己错了，而不是等到某次运行才崩。
