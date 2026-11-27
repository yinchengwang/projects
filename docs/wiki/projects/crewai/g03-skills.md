---
project: crewai
feature: skills
title: 技能发现与加载
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/skills/loader.py
    symbols: [discover_skills, load_skill, format_skill_context]
  - path: lib/crewai/src/crewai/skills/tool.py
    symbols: [LoadSkillTool, create_skill_loader_tool]
---

# 技能发现与加载

## 一句话职责

把磁盘上的 `SKILL.md` 目录变成可分三级「渐进披露」的技能：默认只读元数据（名字加描述），真正需要时才把正文注入提示，必要时再列资源清单——并用一个 `load_skill` 工具让模型自己决定何时升级。

## 机制是什么

技能是目录 + 一个 `SKILL.md`。`SKILL.md` 由 YAML frontmatter 与正文组成，frontmatter 里有 `name`（1–64 字符、小写字母数字加连字符）、`description`（1–1024 字符）、可选的 `license`、`compatibility`、`metadata`、`allowed-tools`。

渐进披露做成三个整数等级，定义在一个 `Literal[1, 2, 3]` 的类型别名上：

- `METADATA = 1`：只加载 frontmatter。
- `INSTRUCTIONS = 2`：加载完整正文。
- `RESOURCES = 3`：再把 `scripts/`、`references/`、`assets/` 三个目录的文件清单编目。

升级靠 `Skill.with_disclosure_level` 产生**新对象**，而不是原地改——`Skill` 是不可变的，每次升级都是「同一份 frontmatter + 更多内容」的新实例。等级只升不降，`activate_skill` 与 `load_skill_resources` 都有「已达该级则原样返回」的幂等判断。

为什么值得这样做？因为技能正文可能很长（源码里对超过 50,000 字符的正文专门发 warning，提示会吃掉上下文窗口）。默认全部注入等于把 prompt 撑爆；只注入元数据又让模型不知道正文里写了什么。三级披露是这两者之间的折中：**先让模型知道有哪些技能、它能做什么，再由模型按需索取具体步骤**。

## 怎么实现的

**发现。** `discover_skills(search_path, source)` 先检查路径存在且是目录，否则 `FileNotFoundError`。遍历 `sorted(search_path.iterdir())`，只处理目录、且目录里必须有 `SKILL.md`。每个成功加载的调 `load_skill_metadata`（只解析 frontmatter，并校验目录名与技能名一致，正文超长时 warning），追加进结果。失败的不中断循环——记 `_logger.warning` 并继续。若传了 `source`，整个过程中发三类事件：`SkillDiscoveryStartedEvent`（带搜索路径）、每个技能的 `SkillLoadedEvent`（带路径与披露等级）、失败时的 `SkillLoadFailedEvent`，最后 `SkillDiscoveryCompletedEvent`（带找到数量与名字列表）。

**分派的四种输入。** `load_skill(skill, source, activate=True)` 按类型分支：

- `Skill` 实例：原样返回单元素列表，**保持它自己的披露等级**。
- `Path`：`discover_skills` 后，`activate` 为真则逐个 `activate_skill`。
- 以 `@` 开头的字符串：走 `resolve_registry_ref`（注册表引用，`activate` 参数透传）。
- 以 `---` 开头的字符串：当成**内联 SKILL.md**，直接 `parse_frontmatter` 拆出 frontmatter 与正文，构造一个 `disclosure_level=INSTRUCTIONS`、`path=Path(".")` 的 `Skill`。注意这条路径不受 `activate` 影响，因为它本来就是全文。
- 其余字符串：当作路径，同 `Path` 分支。
- 其他类型：`TypeError`。

**去重。** `load_skills` 用字典按 `dedup_key` 保序去重。默认键是技能名；若输入是 `@` 引用，键变成 `f"{org}/{name}"`——注释说明理由：不同组织可以发布同名的技能，仅按名字去重会误删。

**编目。** `build_skill_catalog(skills)` 只收「还没到 INSTRUCTIONS」的技能（已全文加载的会在每次执行时渲染，不必再进目录）。重名的技能用父目录名限定成 `dir/name`，且对限定后仍冲突的加数字后缀（`-2`、`-3`…）。所以目录里每个键都是唯一可寻址的。

**注入格式。** `format_skill_context(skill, label=None)` 产出 XML 包裹的块：`<skill name="...">` 加描述加正文，`RESOURCES` 级再加一段 `### Available Resources` 列出每个目录下的文件，最后 `</skill>`。只到 METADATA 级时只有名字与描述。注释点明用 XML 标签包起来的原因：**可以当作系统提示里的稳定缓存锚点**。

**运行期加载器。** `LoadSkillTool` 是个 `BaseTool`：`name` 固定 `load_skill`，`args_schema` 是只有一个必填 `skill_name` 字段的模型，另有三个 `exclude=True` 的字段（`catalog`、`source`、`task`）不参与序列化。`_run` 从 `catalog` 里按名字取技能：取不到就返回一段「技能不存在，可用的是……」的文本（**返回文案而不是抛异常**，模型能自我纠正）；取到就 `activate_skill` 升到 INSTRUCTIONS、发 `SkillUsedEvent`、返回 `format_skill_context(activated, label=skill_name)`。

**名字冲突的处理。** `resolve_loader_tool_name(reserved_names)` 检查默认名 `load_skill` 是否已被 agent 的既有工具占用，占用就从 `load_skill_2` 开始找空位。注释解释了必要性：**若既有工具已占用这个名字，模型就永远到不了加载器，而技能目录却还在指向它**——那是一条静默失效的引用。

**创建条件。** `create_skill_loader_tool` 先 `build_skill_catalog(skills or [])`；目录为空（即所有技能都已是 INSTRUCTIONS 级）就返回 `None`，不注入任何工具。这条判断让「全部技能已全文加载」的场景不额外多出一个无用的工具。

**正文超长只警告不拒绝。** `load_skill_metadata` 与 `load_skill_instructions` 都对 `len(body) > 50_000` 发 warning，措辞是「大正文可能在注入提示时消耗大量上下文窗口」。没有硬性上限，因为技能作者可能真的需要长文档。

## 关键取舍

**源码事实：** 权限、等级、内容都是「构造新对象」，所以同一技能可以在一个 agent 里处于 METADATA 级、在另一个里处于 INSTRUCTIONS 级，互不影响。这是渐进披露能按执行隔离的前提。

**源码事实：** `discover_skills` 只往下一层看（`iterdir` 后检查子目录），不递归搜索嵌套的技能目录。所以技能必须直接放在搜索路径下。

**源码事实：** 内联 SKILL.md 分支的 `path` 被设成 `Path(".")`，于是它的 `scripts_dir` 等属性指向当前目录。这对纯文本技能无害，但若内联内容引用了资源文件，路径会是错的。

**源码事实：** 技能名与目录名必须一致（由 `validate_directory_name` 强制），所以「目录叫 foo、frontmatter 里写 bar」会被拒。这避免了「按目录名找技能」与「按 frontmatter 名寻址」两套命名并存。

**源码事实：** 加载器在技能不存在时返回文本而不是 `ToolFailure`。语义上这是「参数给错了」而不是「工具执行失败」，所以它落在 f05 的失败分类体系之外——模型会读到提示并重试，不需要策略层介入。

**架构推论：** 三级披露把「上下文预算」变成模型可控的资源。默认成本是「每个技能一行描述」，模型按需支付更多。这是与「把全部文档塞进 system prompt」相反的策略，也是长技能库（几十上百个技能）能工作的唯一方式——否则光技能目录就吃光窗口。

**架构推论：** 目录键唯一化（`dir/name`、`-2` 后缀）解决的是「同名技能来自不同来源」的寻址问题。只要目录键唯一，加载器就能准确取到某一份；而渲染时用 `label` 覆盖显示名，则让模型看到的名字与目录键一致，避免「我按目录看到的键去加载，结果加载到另一份」的错配。

**架构推论：** 用 XML 标签包裹并在注释里点明「稳定缓存锚点」，说明这套实现考虑了 prompt caching：同一段技能文本若在多次请求中位置与内容都不变，就能命中缓存。这要求注入的技能顺序稳定——`sorted` 遍历与保序去重都是为此服务的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/skills/loader.py` | `discover_skills` | 扫描目录，逐技能加载元数据并发事件 |
| `lib/crewai/src/crewai/skills/loader.py` | `load_skill` | 四种输入的归一化分派，含内联 SKILL.md |
| `lib/crewai/src/crewai/skills/loader.py` | `load_skills` | 按名字或组织限定去重合并 |
| `lib/crewai/src/crewai/skills/loader.py` | `build_skill_catalog` | 只收未全文加载的技能，键唯一化 |
| `lib/crewai/src/crewai/skills/loader.py` | `activate_skill` | 升级到 INSTRUCTIONS，幂等 |
| `lib/crewai/src/crewai/skills/loader.py` | `format_skill_context` | 按等级渲染 XML 包裹的技能块 |
| `lib/crewai/src/crewai/skills/tool.py` | `LoadSkillTool` | 运行期按名字加载技能的 `BaseTool` |
| `lib/crewai/src/crewai/skills/tool.py` | `LoadSkillTool._run` | 查目录、升级、发事件、返回渲染块 |
| `lib/crewai/src/crewai/skills/tool.py` | `create_skill_loader_tool` | 仅当存在未全文加载技能时才建工具 |
| `lib/crewai/src/crewai/skills/tool.py` | `resolve_loader_tool_name` | 避开与既有工具重名导致加载器不可达 |
| `lib/crewai/src/crewai/skills/models.py` | `Skill.with_disclosure_level` | 不可变升级，产生新实例 |
| `lib/crewai/src/crewai/skills/parser.py` | `load_skill_metadata` | 只解析 frontmatter 的 METADATA 级加载 |

## 参考

- [CrewAI 源码：skills/loader.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/skills/loader.py)
- [CrewAI 源码：skills/tool.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/skills/tool.py)
- [CrewAI 文档：Skills](https://docs.crewai.com/en/skills)

## 与主核 / AI 的关系

主核里最接近的是「按需加载的过程定义」：函数体存在系统目录里，只有真正被调用时才编译成执行计划。技能的三级披露与这条路径同构——目录里先登记元数据，执行时再取正文。差别在于主核的加载由查询计划确定，不可协商；这里由模型自己决定要不要索取，是**把加载时机交给被服务方**。

与 AutoGen 相比，AutoGen 没有等价机制。它的工具清单是 `list_tools` 一次给全，工具描述随 schema 一起进请求；要给模型一大段操作手册，只能塞进 system message 或做成工具返回值。CrewAI 的渐进披露恰好补了这个空档：技能既是「知识」又有「可寻址的加载入口」，而且成本是分段支付的。

对 AI 工程最实用的一条是「元数据默认、正文按需」这个预算模型。当一个 agent 需要指导文档、风格规范、领域手册这类长文本时，全量注入会让每轮请求都背上同样的 token 成本；改成元数据 + 加加载工具，平均成本降到「一次描述 + 偶尔全文」。代价是多一轮往返，以及模型可能不去加载那份它其实需要的技能——所以描述写得准不准，直接决定这套机制有没有用。

另一条是 `resolve_loader_tool_name` 那个不起眼的重名规避。它防的是一类特别难查的故障：加载器被别的工具遮住，技能目录里的名字全都指向一个不存在的入口，表现是「模型看到有技能却从不调用」。这类「同名遮蔽」在工具数量增长后必然出现，把规避逻辑做进创建路径比靠命名约定可靠。

最后是「正文超长只警告」。这是把判断权留给使用者的选择：框架提示成本，但不替用户拒绝。对需要长文档的领域（法规、协议）这是对的；但配合渐进披露使用时要注意，一个 50K 字符的技能一旦被加载，就会在后续每轮请求里都占位，直到上下文被压缩。
