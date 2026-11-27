---
project: metagpt
feature: sop-actions
title: SOP 动作序列：需求到代码
page_type: feature
schema_version: 1
refs:
  - path: metagpt/actions/analyze_requirements.py
    symbols: [AnalyzeRequirementsRestrictions]
  - path: metagpt/actions/design_api.py
    symbols: [WriteDesign]
  - path: metagpt/actions/project_management.py
    symbols: [WriteTasks]
  - path: metagpt/actions/write_code.py
    symbols: [WriteCode]
  - path: metagpt/actions/run_code.py
    symbols: [RunCode]
---

# SOP 动作序列：需求到代码

## 一句话职责

这五个动作组成一条从自然语言需求走到可运行代码的流水线，每一环的产物不是字符串而是**一个落盘的结构化文档**，下一环靠文件路径而非上下文接续。

## 机制是什么

链条是：`AnalyzeRequirementsRestrictions`（抽约束）→ `WriteDesign`（系统设计）→ `WriteTasks`（任务清单）→ `WriteCode`（逐文件写码）→ `RunCode`（跑测试）。

它们共享三条约定：

1. **产物一律落盘**。设计存成 `system_design.json`，任务清单存成 `project_schedule.json`，代码写进 `src/`，测试结果存成 `test_outputs/*.json`。动作之间传的是 `Document`（带 `filename` 与 `root_path`）与文件名，而不是文档全文。
2. **每个动作都有两类入口**：`run(with_messages=...)` 走「角色/消息驱动」路径，从消息的 `instruct_content` 里取上游产物；不给 `with_messages` 就走 `_execute_api(...)` 的「直接调用」路径，参数是纯字符串与路径。两条路径最后都汇到同一个私有方法。
3. **提示都用 `ActionNode` 树填**，而不是手写模板。所以每次填充都会顺带产出 `instruct_content`，即一份已按字段解析好的 pydantic 对象——这是「产物是结构化文档」在实际代码里的落点。

## 怎么实现的

**需求约束抽取是最朴素的一个。** `AnalyzeRequirementsRestrictions` 不建节点树，纯 `llm.aask(prompt)` 后返回原文。它的提示由三段常量拼成：三个 few-shot 例子、抽取指令、输出格式。指令里三条要求写得很具体——语言跟随需求、只抽约束不抽详细需求、识别编程语言且缺省给 HTML/CSS/JS。输出格式是三行带方括号前缀的文本（`[User Restrictions]`、`[Language Restrictions]`、`[Programming Language]`），不是 JSON。

这份产物没有结构化解析，下游靠文本再解析。所以它是全链条里唯一「产出自由文本」的动作。

**WriteDesign 的两条重要分支：新建与增量。** 走消息路径时，它从 `with_messages[-1].instruct_content` 里取 `project_path` 建出 `ProjectRepo`，然后拿到两份变更清单：`changed_prd_filenames` 与 `system_design` 目录下的变更文件。对每一份待更新的 PRD，`_update_system_design` 做「有旧设计就 `_merge`、没有就 `_new_system_design`」的判定：

- 新建：`DESIGN_API_NODE.fill(...)`，把整棵树填一遍。
- 合并：用 `NEW_REQ_TEMPLATE` 拼出 `### Legacy Content` 加 `### New Requirements`，交给 `REFINED_DESIGN_NODE`。这套「精炼节点」的字段名都带 `Refined` 前缀（`Refined File list`、`Refined Implementation Approach` 等），与新建节点的字段一一对应但说明不同。

这里有一个刻意的顺序：变更清单**有意排除当前正在处理的文件**（`changed_system_designs` 里跳过已在 `changed_files` 中的），先处理 PRD 变更、再处理设计自身的变更。代码注释说明了动机——等所有设计文件处理完再发消息，给后续步骤留全局优化的空间。

**设计产物不是一份文档，是四份。** `_update_system_design` 在保存 JSON 之后立刻做三件额外的事：从 `Data structures and interfaces` 字段抽出 mermaid 类图存到 `DATA_API_DESIGN_FILE_REPO`；从 `Program call flow` 字段抽出时序图存到 `SEQ_FLOW_FILE_REPO`；再存一份 PDF。两个 `_save_*` 方法都用 `m.get(新字段) or m.get(Refined字段)` 的写法，所以新建与增量两条路径产出的字段名不同也能取到值。

**WriteTasks 同样是增量优先。** 它的逻辑与 WriteDesign 高度对称：取 `changed_system_design_filenames` 与 tasks 目录的变更，逐个 `_update_tasks`；有旧任务文档就 `_merge`（`REFINED_PM_NODE`），没有就 `_run_new_tasks`（`PM_NODE`）。

额外一步是 `_update_requirements`：从任务文档里读 `Required packages`，与已有的 `requirements.txt` 求并集后写回。这一步让「任务分解」这个动作顺手承担了依赖清单的维护——因为包依赖在 PM_NODE 的字段里本来就要填。

返回值是一个 `AIMessage`，其 `instruct_content` 是把上游 `input_args` 全量 `model_dump()` 之后再补三个字段（变更任务文件名、变更 API 规格、依赖文件名）。这是一种**累加式上下文**：每一环往下传的结构体都在前面基础上增补，而不是替换。

**WriteCode 是唯一不落盘的动作。** 它的 `run` 返回 `CodingContext`，由角色（Engineer）负责 `repo.srcs.save(...)` 写入。这个分工是有意的：一个任务可能对应多个文件的编码尝试，写盘时机应由角色控制而非动作。

它的提示装配值得细看。基础 `PROMPT_TEMPLATE` 有六个语境界：Design、Task、Legacy Code、Debug logs、Bug Feedback logs，加一份 `Format example`。指令部分是七条硬性要求，其中「只实现这一个文件」「必须是完整代码」「必须遵循 Data structures and interfaces，不许改设计」「不许留 TODO」都是防止模型偷工减料的具体约束。

两次模型调用：`write_code` 带 `RetryPolicy` 式的退避重试（最多 6 次），内部用 `CodeParser.parse_code` 从输出里抽代码块——抽不出会抛异常，从而触发重试。

**上下文靠 get_codes 拼，不靠消息历史。** `get_codes` 有两个模式。普通模式下，它从任务文档的 `Task list` 字段拿到目标文件名列表，逐个从 `srcs` 读出已有代码拼成代码块——**并排除当前要写的那个文件**。增量模式下，它遍历 `srcs` 的全部文件，对当前文件做特殊处理：若它不是 `main.py`，就去旧的 workspace 取旧代码放在最前面并标注「要重写的文件」；若是 `main.py`，直接跳过。注释解释了跳过 `main.py` 的原因——保持主文件聚焦，避免把无关键塞进提示。

这段逻辑解释了为什么 `WriteCode` 不需要完整消息历史：任务清单里已经写了要哪些文件，代码在磁盘上，它只需要读文件。

**RunCode 把「跑」和「判」合成一步。** 它有两种模式：`script` 起子进程，`text` 用 `exec` 直接在内存里跑。script 模式下 `run_script` 会先拼 `PYTHONPATH`（把工作目录与额外路径串进去），然后调 `_install_dependencies` 装 `requirements.txt` 与 `pytest`，再用 `subprocess.Popen` 起进程，10 秒超时后 `kill`。

拿到 stdout/stderr 之后，它把代码、测试代码、命令、输出拼进 `PROMPT_TEMPLATE` 交给模型，要求模型只回答四段：错误原因、要重写哪一个文件、`PASS`/`FAIL`、以及 `Send To`（`NoOne`/`Engineer`/`QaEngineer`）。最后包成 `RunCodeResult(summary, stdout, stderr)`。

截断策略是有区别的：stdout 截到 500 字符，stderr 截到 10000 字符。理由是 stdout 通常只是打印、价值低，stderr 里的堆栈才是定位依据。

`Send To` 这一字段是这条链的回边：它决定错误该退回给写码的角色还是测试的角色，从而形成「写—测—修」的循环。

## 关键取舍

**源码事实：** 五个动作里四个把产物落盘并把路径放进消息，只有 `WriteCode` 返回内存对象、由角色落盘。这不是不一致，而是职责划分：文档类产物是 SOP 的一等公民（可增量合并、可依赖追踪），代码文件则必须由持有 repo 的角色统一写入并声明依赖。

**源码事实：** `_merge` 与 `_new` 用两套节点（`Refined*` 与原始节点）。同一份文档在「首版」与「增量修订」时字段说明不同，因此不能共用一棵树。代价是字段名分叉，读取时要 `m.get(新键) or m.get(Refined键)` 兜底——设计图与时序图那两处就是这么写的。

**源码事实：** `AnalyzeRequirementsRestrictions` 是唯一的自由文本环节，它的输出格式是带方括号前缀的纯文本，没有过 pydantic 校验。整条链的其余环节都是结构化解析 + 重试。

**源码事实：** `WriteCode.get_codes` 在增量模式下对 `main.py` 做特殊跳过。这是一条针对具体项目形态的启发式规则，不是通用机制——代码注释也承认这是为了「保持主文件干净」。

**架构推论：** 「产物落盘 + 路径传递」让这条链条天然支持增量开发与断点续跑：任何一步都能从磁盘上的既有文档继续，不需要重放上游。代价是磁盘成了隐式状态机，只看动作代码无法知道当前项目进行到哪一步，必须看 `ProjectRepo` 里的目录。

**架构推论：** 每一环都以 `instruct_content` 累加的方式往下传，意味着传到最后那个结构体里包含了从需求到任务的全部中间产物。这使下游有充分上下文，也让消息体随链条增长而膨胀；`WriteTasks` 那处 `kvs = self.input_args.model_dump()` 加补字段的写法，就是这个膨胀过程的直接体现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/actions/analyze_requirements.py` | `AnalyzeRequirementsRestrictions` | 从需求里抽用户约束、语言与编程语言 |
| `metagpt/actions/design_api.py` | `WriteDesign` | 系统设计：新建用 `DESIGN_API_NODE`，增量用 `REFINED_DESIGN_NODE` |
| `metagpt/actions/design_api.py` | `WriteDesign._save_data_api_design` | 从设计字段抽 mermaid 类图并落盘 |
| `metagpt/actions/project_management.py` | `WriteTasks` | 系统设计到任务清单，并顺带维护依赖清单 |
| `metagpt/actions/project_management.py` | `WriteTasks._update_requirements` | 把 `Required packages` 并入 `requirements.txt` |
| `metagpt/actions/write_code.py` | `WriteCode` | 单文件编码，六段语境 + 七条硬约束 |
| `metagpt/actions/write_code.py` | `WriteCode.get_codes` | 按任务清单读兄弟文件、排除当前文件 |
| `metagpt/actions/run_code.py` | `RunCode` | 起子进程或内存执行，再把结果交给模型判定 |
| `metagpt/actions/run_code.py` | `RunCode.run_script` | 拼 PYTHONPATH、装依赖、限时执行 |
| `metagpt/actions/run_code.py` | `RunCode._install_dependencies` | 安装 requirements 与 pytest |

## 参考

- [MetaGPT 源码：actions/design_api.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/design_api.py)
- [MetaGPT 源码：actions/write_code.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/write_code.py)
- [MetaGPT 源码：actions/run_code.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/actions/run_code.py)
- [MetaGPT 论文：SOP 作为多智能体协作骨架](https://arxiv.org/abs/2308.00352)

## 与主核 / AI 的关系

这条链条的设计与主核里「语句执行到落盘的 WAL 协议」有一个共同点：每一步的产物先变成**可持久化、可再次读取的形态**，下一步只认这个形态，不依赖前一步的内存状态。主核靠 LSN 串起顺序，这里靠文件名与依赖集合串起顺序（`save_doc(..., dependencies={...})`）。

对 agent 工程最值得借鉴的是两点。其一，把「测试结果判定」交给模型并让它输出「该退给谁」（`Send To`），用一次 LLM 调用同时完成了错误分类与路由决策，比在代码里写一堆正则匹配异常类型更适应未知错误。其二，`WriteCode` 靠磁盘读兄弟文件而不是靠消息历史——这避免了上下文随任务数线性膨胀，代价是要求上游产物必须先落盘。这两点合起来，就是「用文件系统做 agent 的共享内存」这一取舍的具体形态。
