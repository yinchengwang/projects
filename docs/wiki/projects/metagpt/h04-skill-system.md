---
project: metagpt
feature: skill-system
title: 技能声明与技能动作
page_type: feature
schema_version: 1
refs:
  - path: metagpt/learn/skill_loader.py
    symbols: [Skill, SkillsDeclaration, Components]
  - path: metagpt/actions/skill_action.py
    symbols: [SkillAction, ArgumentsParingAction]
  - path: metagpt/management/skill_manager.py
    symbols: [SkillManager]
---

# 技能声明与技能动作

## 一句话职责

技能分两层：YAML 里声明的「技能说明书」描述一个函数叫什么、收什么参数、有哪些示例；两个动作类负责把一句自然语言先解析成参数、再按名字叫到那个函数；`SkillManager` 则是更早的、基于向量检索的技能池实现，与前者共存于同一份代码里。

## 机制是什么

技能在 MetaGPT 里有两条互不相同的血脉，读代码时必须先分清：

**血脉一：声明式技能（当前主路径）。** 技能写在 YAML 文件里，用 `SkillsDeclaration` 加载。文件结构是 `skillapi` 版本标识 + `entities` 映射（实体名 → 该实体拥有的技能列表）+ 可选的 `components`。每个 `Skill` 声明名称、描述、`x-prerequisite` 前置条件、参数字典（名称 → 类型与说明）、以及**示例列表**（`ask` / `answer` 成对）。运行时由两个动作落地：`ArgumentsParingAction` 把用户口语转成参数，`SkillAction` 拿参数去调真实函数。

**血脉二：动作即技能（旧路径）。** `SkillManager` 里有一行 `Skill = Action`——它把 `Action` 直接当技能类型，技能池就是一个以技能名为键的字典，外加一个 Chroma 向量库用于按描述检索。这条路径没有 YAML、没有示例、没有参数解析。

两套机制共用「技能」这个词但语义不同：前者把技能当作**规格**，后者把技能当作**可执行动作**。

## 怎么实现的

**YAML 侧的数据模型。** `SkillsDeclaration` 有三个字段：`skillapi`（必填）、`entities`（必填，`Dict[str, Entity]`）、`components`（可选，类型是 `Components`）。而 `Components` 是一个**空类**——`class Components(BaseModel): pass`，没有任何字段。它的存在只是为了给「组件」这个概念留一个位置，当前不具备承载信息的能力。

`Entity` 有 `name` 与 `skills` 两个字段；`Skill` 字段更细：`name`、`description`、`id`、以别名 `x-prerequisite` 接收的 `x_prerequisite`、`parameters`（`Dict[str, Parameter]`）、`examples`（`List[Example]`，必填）、`returns`（`Returns`，必填）。

`Parameter` 只有 `type` 与可选的 `description`。`Skill.arguments` 属性把参数压成 `{名称: 说明}`，缺说明时填空字符串——这个属性是给提示词用的，因为它只保留自然语言，丢掉类型。

**加载与查找。** `SkillsDeclaration.load` 在未给路径时默认读 `docs/.well-known/skills.yaml`（相对包根的上级目录），用 `yaml.safe_load` 解析后直接 `SkillsDeclaration(**data)` 构造，不做 schema 校验以外的检查。`get_skill(name, entity_name="Assistant")` 在实体内线性查找同名技能，实体不存在或技能不存在都返回 `None`。

`get_skill_list(entity_name, context)` 的返回值容易被误读：它返回 `{技能描述: 技能名}`——**键是描述**。而且它只返回「被显式启用的」技能：从 `context.kwargs.agent_skills` 取启用列表（每项被强制解析成含 `name` 字段的对象），再筛出 `s.name in names` 的那些。没有启用列表就返回空字典。因此「声明了技能」不等于「可以用技能」，中间隔着一次显式启用。

**`ArgumentsParingAction` 拼的是三段式提示词。** 它的 `prompt` 属性按顺序拼：技能名 + 「function parameters description」，逐参数列出 `parameter k: 说明`；然后是「Examples」段，把每个示例拼成「若你要做 X，就返回 Y」；最后一段是任务指令，要求模型「参照函数描述，按示例格式，把 `{self.ask}` 转成函数参数」。system prompt 是「你是一个函数解析器，能把口语转成函数参数」。

`run` 把响应交给 `parse_arguments` 解析，然后把结果同时放进 `self.args` 与返回消息的 `instruct_content`——也就是说参数既是动作的副作用，也是消息的结构化载荷。

**`parse_arguments` 用 Python 语法解析模型输出。** 它先找出 `技能名(` 这个前缀与最后一个 `)`，截出中间部分，然后拼成 `fake_expression = f"dict({args_txt})"` 交给 `ast.parse(mode="eval")`，遍历 `body.keywords` 逐个 `ast.literal_eval`。

这个做法的巧妙之处是用 `dict(...)` 包一层，让 `a=1, b="x"` 这种「函数调用参数表」直接成为合法的字典构造语法，从而复用 Python 自带的字面量解析而不用自己写 parser。代价是它的容错为零：模型若输出位置参数、goto 或注释，`ast.parse` 抛 `SyntaxError`；`literal_eval` 只接受字面量，参数值里出现变量名或函数调用也会抛。前缀没找到或右括号缺失时返回 `None` 并只记日志，不抛错。

**`SkillAction` 是执行端。** 它持有 `skill` 与 `args`，`run` 里先对 `kwargs` 做一次深拷贝，然后把与 `args` 同名的键从 options 里弹掉——避免同一个参数既来自解析结果又来自调用方而重复传参（`**options` 与 `**args` 撞名会抛 `TypeError`，所以必须先去重）。

真正调用在 `find_and_call_function`：`importlib.import_module("metagpt.learn")`，然后 `getattr(module, function_name)`，再 `await function(**args, **kwargs)`。所以**技能的实际实现必须是由 `metagpt.learn` 包导出的顶层函数**——`learn/__init__.py` 目前只导出 `text_to_image`、`text_to_speech`、`google_search` 三个，因此能被这样调用的技能名也只有这三类。

异常处理分两层：模块名或属性不存在时记 error 日志并抛 `ValueError(f"{function_name} not found")`；`SkillAction.run` 捕获所有异常并把错误文本包成 assistant 消息返回，让上层能继续跑而不是整条链路崩掉。

**`SkillManager` 是旧路径的完整形态。** 构造时建一个 `ChromaStore("skill_manager")`（内存集合），`_skills` 字典存动作对象。`add_skill` 双写：字典按技能名存，Chroma 里按 `skill.desc` 存文档、`{"name", "desc"}` 作元数据、技能名作 id。`del_skill` 双删。`retrieve_skill` 走向量检索后取 `["ids"][0]`——注意返回的是 **id 字符串列表**（实际是技能名），不是技能对象；`retrieve_skill_scored` 则返回原始检索结果。`generate_skill_desc` 只是把提示词文件读出来打日志，没有真正生成描述，函数体到 `logger.info` 就结束了。

## 关键取舍

**源码事实：** `Components` 是空模型。它占位但无字段，因此 YAML 里写 `components` 的任何内容都会被 pydantic 忽略（除非配置了额外字段策略），也没有任何代码读取它。

**源码事实：** `get_skill_list` 用「描述」当返回字典的键，用「名称」当值。这让调用方需要「按描述挑技能、按名字调用」，也意味着两条技能描述相同会互相覆盖。

**源码事实：** 技能必须显式启用才会出现在 `get_skill_list` 里；未启用时即使 YAML 里声明了也返回空字典。

**源码事实：** 参数解析依赖模型输出严格的 `技能名(k=v, ...)` 形式，用 `dict()` 包一层走后 Python 字面量解析。任何位置参数、变量引用、嵌套结构都会失败，且失败时 `parse_arguments` 返回 `None` 而不是抛错，`args` 变成 `None` 后 `SkillAction` 会以空参数调用。

**源码事实：** 技能实现被硬绑定到 `metagpt.learn` 包的顶层导出。因此「新增一个技能」不仅要在 YAML 里声明，还必须在该包的 `__init__` 里导出同名函数，否则调用时报「not found」。

**架构推论：** 两条血脉并存说明这套机制处在迁移中途：声明式的规格化路线（YAML + 示例 + 参数）已经具备完整链路，而检索式的技能池（`SkillManager`）保留了「按描述找技能」的能力但缺少参数与示例。二者没有共用数据结构，`SkillManager` 里的 `Skill` 与 YAML 里的 `Skill` 是两个完全不同的东西。

**架构推论：** 把「参数解析」与「函数调用」拆成两个独立动作，使「只解析不执行」成为可能——这对需要人工确认参数的流程很重要。代价是两者之间的契约是 `self.args` 这个裸字典，没有任何校验：`ArgumentsParingAction` 不保证参数名与技能声明一致，`SkillAction` 也不检查缺参数。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/learn/skill_loader.py` | `Skill` | 技能声明：参数、示例、返回与前条件 |
| `metagpt/learn/skill_loader.py` | `Skill.arguments` | 参数名到说明的映射，供提示词使用 |
| `metagpt/learn/skill_loader.py` | `SkillsDeclaration` | YAML 根结构：skillapi / entities / components |
| `metagpt/learn/skill_loader.py` | `SkillsDeclaration.get_skill_list` | 只返回显式启用的技能，键为描述 |
| `metagpt/learn/skill_loader.py` | `Components` | 空占位模型，当前无语义 |
| `metagpt/actions/skill_action.py` | `ArgumentsParingAction` | 三段式提示词把口语转成参数 |
| `metagpt/actions/skill_action.py` | `ArgumentsParingAction.parse_arguments` | `dict(...)` 包装 + `ast.literal_eval` 解析 |
| `metagpt/actions/skill_action.py` | `SkillAction` | 去重同名参数后调用技能实现 |
| `metagpt/actions/skill_action.py` | `SkillAction.find_and_call_function` | 从 `metagpt.learn` 取顶层函数并 await |
| `metagpt/management/skill_manager.py` | `SkillManager` | 旧路径：技能池 + Chroma 描述检索 |
| `metagpt/management/skill_manager.py` | `SkillManager.add_skill` | 字典与向量库双写 |

## 参考

- [MetaGPT 源码：learn/skill_loader.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/learn/skill_loader.py)
- [MetaGPT 源码：actions/skill_action.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/skill_action.py)
- [MetaGPT 源码：management/skill_manager.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/management/skill_manager.py)
- [Python 文档：ast.literal_eval](https://docs.python.org/3/library/ast.html#ast.literal_eval)

## 与主核 / AI 的关系

「声明在数据文件里、执行靠一个极窄的解析器」与主核里「参数化查询先解析再绑定」是同一种分工：主核把 SQL 文本解析成计划、再按计划读页，解析与执行彻底分离，因此可以在两者之间插入校验与优化。这里也做了分离，但中间那道契约是个裸字典，既没有参数名校验也没有类型校验——这正是它比主核脆弱的地方。

对 agent 工程而言，这一页解释了「技能」为什么看起来像工具却不是工具：工具靠 schema 让模型直接生成调用（结构化输出），技能靠示例让模型把口语改写成 `name(k=v)` 这种字符串再解析回来。前者省一次解析但要求模型遵守 JSON 模式，后者对模型更宽容（纯文本即可）但在参数复杂时极易解析失败。若技能参数超过两三个或含嵌套结构，`dict()` 加字面量解析这条路会频繁出错，此时应改用工具那套结构化 schema。

