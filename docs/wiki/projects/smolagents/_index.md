---
project: smolagents
page_type: overview
display_name: smolagents
repo: https://github.com/huggingface/smolagents
license: Apache-2.0
language: Python
stars: 30475
stars_as_of: 2026-11-22
refs:
  - path: src/smolagents/agents.py
    symbols: [MultiStepAgent, CodeAgent, ToolCallingAgent]
  - path: src/smolagents/local_python_executor.py
    symbols: [LocalPythonExecutor, evaluate_python_code]
  - path: src/smolagents/tools.py
    symbols: [BaseTool, Tool]
  - path: src/smolagents/models.py
    symbols: [Model, ChatMessage]
  - path: src/smolagents/memory.py
    symbols: [AgentMemory, ActionStep]
  - path: src/smolagents/remote_executors.py
    symbols: [RemotePythonExecutor]
---
# smolagents

## 一句话职责

Hugging Face 的极简 agent 框架：核心主张是 **以代码为动作**——智能体不输出 JSON 形式的工具调用，而是写一段 Python 代码，由受控解释器执行。

## 机制是什么

框架只有一个包、18 个模块，代码量刻意压得很小。它要回答的问题很集中：**怎么让模型用一个动作表达多个工具调用？**

答案是让它写代码。代码天然能组合、能循环、能保存中间结果；JSON 工具调用一次只能调一个工具，要串联就得回模型一趟。代价是必须执行模型生成的任意代码，于是安全问题从「参数校验」升级成「执行环境隔离」。

框架因此有两套执行器：**本地解释器**（AST 遍历求值，裁剪内置函数与导入，源码自己声明不是安全沙箱）与**远程执行器**（E2B、Docker、Modal 等真隔离环境）。选哪一个，决定了你能跑多不可信的代码。

智能体有两种形态：`CodeAgent` 走代码路径，`ToolCallingAgent` 走传统 JSON 工具调用路径。两者共用同一个多步主循环（`MultiStepAgent`），差异只在「动作怎么表达、怎么解析」。

其余是支撑机制：**工具**同时提供代码提示与工具调用提示两种描述；**记忆**用五类步骤记录一次运行；**模型层**封装 API 与本地推理；**监控**统计 token 与耗时。

本文库按经审阅的机制单元组织：**9 个机制领域、共 30 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 9 个领域按下面的顺序排列：A 智能体骨架 → B 代码执行沙箱 → C 工具定义 → D 工具启用与校验 → E 模型层 → F 记忆与步骤 → G 远程执行 → H MCP 与序列化 → I 错误与可观测。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`src/smolagents/` 全部模块（这个框架足够小，可以完整覆盖）。它刻意把第三方工具与模型包装放在 Hub 上而非仓内，所以本清单不含「集成适配」类页面。

## 机制导航（30 篇 / 9 组）

### A-智能体骨架（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-multi-step-agent.md`](a01-multi-step-agent.md) | 多步智能体主循环 |
| [`a02-tool-calling-agent.md`](a02-tool-calling-agent.md) | 工具调用式智能体 |
| [`a03-code-agent.md`](a03-code-agent.md) | 代码式智能体 |
| [`a04-managed-agents.md`](a04-managed-agents.md) | 被管理的子智能体 |

### B-代码执行沙箱（3 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-local-python-executor.md`](b01-local-python-executor.md) | 本地 Python 解释器与能力裁剪 |
| [`b02-executor-operations.md`](b02-executor-operations.md) | 解释器支持的语句与表达式 |
| [`b03-final-answer-mechanism.md`](b03-final-answer-mechanism.md) | 最终答案的表达与提取 |

### C-工具定义（4 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-base-tool.md`](c01-base-tool.md) | 工具基类与两种提示形态 |
| [`c02-tool-source-handling.md`](c02-tool-source-handling.md) | 工具源码与序列化往返 |
| [`c03-tool-collection.md`](c03-tool-collection.md) | 工具集合与流水线工具 |
| [`c04-default-tools.md`](c04-default-tools.md) | 内置工具清单 |

### D-工具启用与校验（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-argument-validation.md`](d01-argument-validation.md) | 工具参数校验 |
| [`d02-tool-code-validation.md`](d02-tool-code-validation.md) | 工具源码的静态校验 |
| [`d03-json-schema-from-signature.md`](d03-json-schema-from-signature.md) | 从签名与文档生成 JSON schema |
| [`d04-tool-visibility.md`](d04-tool-visibility.md) | 工具暴露与描述注入 |

### E-模型层（4 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-model-base.md`](e01-model-base.md) | 模型基类与消息协议 |
| [`e02-api-models.md`](e02-api-models.md) | API 模型实现 |
| [`e03-local-inference.md`](e03-local-inference.md) | 本地推理模型 |
| [`e04-streaming-aggregation.md`](e04-streaming-aggregation.md) | 流式增量与聚合 |

### F-记忆与步骤（3 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-memory-steps.md`](f01-memory-steps.md) | 记忆步骤类型与消息转换 |
| [`f02-agent-memory.md`](f02-agent-memory.md) | 记忆容器与重放 |
| [`f03-callbacks.md`](f03-callbacks.md) | 步骤回调注册 |

### G-远程执行（3 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-remote-executor-contract.md`](g01-remote-executor-contract.md) | 远程执行器契约 |
| [`g02-executor-backends.md`](g02-executor-backends.md) | 远程执行后端实现 |
| [`g03-tool-and-variable-transfer.md`](g03-tool-and-variable-transfer.md) | 工具与变量跨进程传递 |

### H-MCP 与序列化（2 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-mcp-client.md`](h01-mcp-client.md) | MCP 客户端与工具接入 |
| [`h02-safe-serialization.md`](h02-safe-serialization.md) | 安全序列化 |

### I-错误与可观测（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-error-taxonomy.md`](i01-error-taxonomy.md) | 错误分类与重试 |
| [`i02-parsing-and-truncation.md`](i02-parsing-and-truncation.md) | 解析工具与内容截断 |
| [`i03-monitoring.md`](i03-monitoring.md) | 监控、用量与日志 |

## 关键取舍

**源码事实：** 本地解释器的类文档字符串明确写「这不是安全沙箱」（not a security sandbox），并建议需要隔离不可信代码时改用远程执行器。它的能力裁剪（内置白名单、树形导入授权、dunder 禁止）减少的是误用面，不是建立信任边界。

**源码事实：** 超时机制用线程池实现而非 signal，源码注释坦白了原因（跨平台、任意线程可用）与代价（超时后线程无法强杀，会在后台跑完）。也就是说超时只保证调用方不被阻塞，不保证副作用停止。

**源码事实：** `final_answer` 通过抛 `BaseException` 子类短路执行，因此不会被用户代码里的 `try/except Exception` 吞掉。

**源码事实：** 工具的静态校验（`tool_validation.py`）会检查工具源码里是否有禁止的语句、属性与导入。这是框架在「执行前」多加的一道，其他同类框架通常没有。

**架构推论：** 「以代码为动作」本质上是一次权衡：用一个动作表达组合逻辑，换取对执行环境的强依赖。JSON 工具调用的框架（openai-agents、autogen）不需要沙箱，因为它们不执行任意代码；smolagents 必须提供执行器抽象，因为它的动作就是代码。

**架构推论：** 框架小到可以完整读完，这是它有意的设计取向——不追求功能覆盖，而是把「代码即动作」这一个主张做透，其他能力（新工具、新模型）交给 Hub。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/smolagents/agents.py` | `MultiStepAgent` | 多步主循环：规划、行动、观察、收尾 |
| `src/smolagents/agents.py` | `CodeAgent` | 代码式智能体 |
| `src/smolagents/agents.py` | `ToolCallingAgent` | JSON 工具调用式智能体 |
| `src/smolagents/local_python_executor.py` | `LocalPythonExecutor` | 本地受控解释器 |
| `src/smolagents/local_python_executor.py` | `evaluate_python_code` | AST 逐节点求值主函数 |
| `src/smolagents/tools.py` | `Tool` | 工具定义与双提示形态 |
| `src/smolagents/tool_validation.py` | `MethodChecker` | 工具源码静态校验 |
| `src/smolagents/models.py` | `Model` | 模型基类与消息协议 |
| `src/smolagents/memory.py` | `AgentMemory` | 记忆容器与重放 |
| `src/smolagents/remote_executors.py` | `RemotePythonExecutor` | 远程隔离执行契约 |

## 参考

- [smolagents 源码仓库](https://github.com/huggingface/smolagents)
- [smolagents 文档：智能体介绍](https://huggingface.co/docs/smolagents/conceptual_guides/intro_agents)
- [smolagents 文档：安全执行代码](https://huggingface.co/docs/smolagents/tutorials/secure_code_execution)
- [smolagents 文档：工具](https://huggingface.co/docs/smolagents/tutorials/tools)

## 与主核 / AI 的关系

smolagents 与主核（多模态数据库内核）的交集在「受控执行」这条线上最清楚：

**AST 解释器 vs 表达式求值器。** 主核要把 SQL 表达式编译成可求值的算子树，且不允许用户表达式触达任意宿主函数；smolagents 要把 Python 源码变成可求值的节点树，且只允许触达白名单里的对象。两者都在做同一件事——把一个通用语言的表达力裁剪成受控子集。差别在目标：主核裁剪是为语义可预测与可优化，smolagents 裁剪是为限制模型生成代码的影响面。

**超时 ≠ 停止。** 本地解释器的超时用线程池实现，源码注释坦白「超时后线程无法强杀，会在后台跑完」。主核里对应的问题是「查询超时后后台仍在扫表」。两者都必须在文档里说清超时保证的是什么，否则上层资源配额会被后台残留任务悄悄突破。

**两套执行器 vs 两套后端。** 本地解释器与远程执行器的分层，与主核里「内置引擎 + 外部存储适配」是同构的：前者方便但边界弱，后者隔离强但成本高。区别在于 smolagents 把「选哪个」这件事显式交给用户，而不是自动降级。

对 2B 波次（工具使用与软件工程 Agent）的整体判断：smolagents 是「工具即代码」这条路线的最小实现。它的价值不在功能，而在把这条路线所需的最低机制集合展示得很干净——解释器、工具双描述、远程执行契约、静态校验。要看懂其他代码式 agent（open-interpreter、openhands 的 shell 工具）的设计，先读它比读大框架更快。
