---
project: openhands
page_type: overview
display_name: OpenHands Agent Canvas
repo: https://github.com/OpenHands/OpenHands
license: MIT
language: TypeScript
stars: 93984
stars_as_of: 2026-11-22
refs:
  - path: src/api/backend-registry/types.ts
    symbols: [Backend, BackendSelection]
  - path: src/api/backend-registry/active-store.ts
    symbols: [getActiveBackend, computeSnapshot]
  - path: src/api/mcp-service/mcp-redacted-credentials.ts
    symbols: [substituteRedactedMcpCredentials]
  - path: src/api/conversation-service/agent-server-conversation-service.api.ts
    symbols: [normalizeWorkspace]
  - path: src/api/mars/mars-tunnel-backend.ts
    symbols: [MarsConnection, MarsTransport]
  - path: src/stores/conversation-store.ts
    symbols: [ConversationState, ConversationActions]
---
# OpenHands Agent Canvas

## 一句话职责

自托管的「编程 agent 开发者控制中心」：把 OpenHands、Claude Code、Codex、Gemini、Pi、OpenCode 等多个 agent 后端统一到一套控制界面，并支持本地、Docker、VM、云等多种运行位置。

## 机制是什么

这个项目**不是 agent 运行时**。它是最外层的前端控制面：一层 React/TypeScript 应用，负责把多个 agent 后端抽象成一个可切换的「后端」概念，然后在这个统一抽象之上做会话、事件、MCP、Git、自动化等一切界面操作。

理解它需要先理解仓库的职责边界。源码的 AGENTS.md 把四个仓的归属写得很明确：

| 仓库 | 负责什么 |
|---|---|
| `OpenHands/OpenHands`（本项目） | Agent Canvas UI、前端状态、后端选择、前端服务集成、本地栈编排 |
| `OpenHands/software-agent-sdk` | Python SDK、Agent Server、canonical REST/WebSocket API |
| `OpenHands/extensions` | 可复用的技能、自动化、插件 |
| `OpenHands/automation` | 自动化的定义、调度、webhook 生命周期 |

依赖方向是「Agent Server 契约 → TypeScript client → Agent Canvas」。因此本仓的价值集中在**控制面**：它自己不实现 agent 循环、不跑工具、不管沙箱，而是把「多个后端、多种 agent、多种会话形态、本地与云」这些维度织成一套可用的操作界面。

本文库按经审阅的机制单元组织：**9 个机制领域、共 23 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元。React 组件（`components/` 下 755 个文件）与样式属于表现层，不建页。

## 怎么实现的

下表的 9 个领域按下面的顺序排列：A 后端注册表与选择 → B 后端健康与存储 → C ACP 智能体接入 → D 会话与事件 → E MCP 集成 → F 自动化与工具目录 → G 工作区与 Git → H 云隧道与桥接 → I 状态容器与路由。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`src/api/`（各后端服务客户端）、`src/stores/`（状态容器）、`src/hooks/` 中的状态派生部分。

## 机制导航（23 篇 / 9 组）

### A-后端注册表与选择（3 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-backend-model.md`](a01-backend-model.md) | 后端类型模型 |
| [`a02-active-backend.md`](a02-active-backend.md) | 当前后端的选定与回退 |
| [`a03-default-backend.md`](a03-default-backend.md) | 默认后端与锁定后端 |

### B-后端健康与存储（2 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-backend-health.md`](b01-backend-health.md) | 后端健康状态跟踪 |
| [`b02-backend-storage.md`](b02-backend-storage.md) | 后端清单的本地持久化 |

### C-ACP 智能体接入（2 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-acp-auth-probe.md`](c01-acp-auth-probe.md) | ACP 智能体认证探测 |
| [`c02-agent-profiles.md`](c02-agent-profiles.md) | 智能体档案与当前档案 |

### D-会话与事件（3 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-conversation-service.md`](d01-conversation-service.md) | 会话服务与响应归一化 |
| [`d02-conversation-types.md`](d02-conversation-types.md) | 会话类型与启动任务 |
| [`d03-event-service.md`](d03-event-service.md) | 事件检索与确认响应 |

### E-MCP 集成（3 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-mcp-service.md`](e01-mcp-service.md) | MCP 服务器配置与测试 |
| [`e02-mcp-redacted-credentials.md`](e02-mcp-redacted-credentials.md) | MCP 凭据脱敏与服务端替换 |
| [`e03-mcp-health.md`](e03-mcp-health.md) | MCP 健康探测与重授权 |

### F-自动化与工具目录（2 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-automation-service.md`](f01-automation-service.md) | 自动化与自定义 Webhook |
| [`f02-tool-catalog.md`](f02-tool-catalog.md) | 工具目录校验 |

### G-工作区与 Git（3 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-git-service.md`](g01-git-service.md) | Git 服务与云/本地分支 |
| [`g02-agent-server-git.md`](g02-agent-server-git.md) | 运行时侧 Git 与路径换算 |
| [`g03-bash-service.md`](g03-bash-service.md) | Bash 输出分页与检索 |

### H-云隧道与桥接（2 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-mars-tunnel.md`](h01-mars-tunnel.md) | Mars 隧道与沙箱代理 |
| [`h02-mars-web-bridge.md`](h02-mars-web-bridge.md) | Mars Web 桥接与 RPC |

### I-状态容器与路由（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-conversation-store.md`](i01-conversation-store.md) | 会话状态容器 |
| [`i02-url-selection.md`](i02-url-selection.md) | 后端选择的 URL 参数 |
| [`i03-agent-state.md`](i03-agent-state.md) | 智能体执行状态的派生 |

## 关键取舍

**源码事实：** 仓库的 AGENTS.md 用一张表划清四个仓的职责边界，并明确「不要在本仓重新实现 Agent Server 的端点或契约」。这把「控制面」与「运行时」的分工变成一条硬纪律。

**源码事实：** MCP 凭据走「服务端返回脱敏值 → 前端按需回填」的两条路径，且注释写明「持久化从不调用替换辅助函数；稀疏补丁省略未变的密钥」。前端从不持有明文凭据。

**源码事实：** 会话服务的响应要经过一批归一化函数逐个字段处理（缺字段给默认、类型不对给空值、路径绝对化后校验在目录内）。前端把后端返回的 JSON 当作不可信输入。

**源码事实：** 后端健康状态、后端清单、URL 参数三条来源共同决定「当前用哪个后端」，且有明确的回退规则。

**架构推论：** 「控制面与运行时分离」是这个项目最有价值的机制。同一个界面要能驱动多个 agent 实现（自家 SDK、Claude Code、Codex 等），唯一可行的做法是把「后端」抽象成统一的协议端点，界面只跟协议说话。这也解释了为什么 ACP（Agent Client Protocol）在这里出现——它正是让第三方 agent 能挂进来的标准。

**架构推论：** 代价是所有能力都被压到「协议能表达的那一层」。任何后端独有的特性要么被抽象掉，要么只能在界面层做能力探测后做差异化展示（这正是 ACP 认证探测那篇的主题）。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/api/backend-registry/types.ts` | `Backend` | 后端类型模型 |
| `src/api/backend-registry/active-store.ts` | `getActiveBackend` | 当前后端选定与回退 |
| `src/api/backend-registry/health-store.ts` | `getHealthSnapshot` | 后端健康跟踪 |
| `src/api/backend-registry/storage.ts` | `readStoredBackends` | 后端清单本地持久化 |
| `src/api/acp-service/acp-service.api.ts` | `ACP_AUTH_PROBES` | ACP 智能体认证探测 |
| `src/api/conversation-service/agent-server-conversation-service.api.ts` | `normalizeWorkspace` | 会话响应归一化 |
| `src/api/mcp-service/mcp-redacted-credentials.ts` | `substituteRedactedMcpCredentials` | 凭据脱敏与回填 |
| `src/api/mars/mars-tunnel-backend.ts` | `MarsTransport` | 云隧道传输 |
| `src/api/mars/mars-web-bridge.ts` | `createMarsWebBridge` | 浏览器侧 RPC 桥接 |
| `src/stores/conversation-store.ts` | `ConversationState` | 前端会话状态容器 |

## 参考

- [OpenHands 源码仓库](https://github.com/OpenHands/OpenHands)
- [OpenHands 文档：Agent Canvas](https://docs.openhands.dev/openhands/usage/agent-canvas)
- [OpenHands 文档：ACP 智能体](https://docs.openhands.dev/openhands/usage/agent-canvas/acp-agents)
- [Agent Client Protocol](https://agentclientprotocol.com/)
- [MCP 规范](https://modelcontextprotocol.io/specification/2025-06-18)

## 与主核 / AI 的关系

OpenHands Agent Canvas 与主核（多模态数据库内核）的关系不在「实现相似」，而在「**控制面与执行面的分层**」这一架构问题上：

**三条边界与主核的对应。** 主核里也有类似的三层：SQL 协议层（对外契约）、执行引擎（真正干活）、客户端工具（连过来用）。OpenHands 把这三层分在不同仓库里并写进纪律；主核把三层放在同一个仓但用目录与依赖方向分隔（`tools/check_deps.sh` 强制 L0–L4 分层）。两种做法的目标一致：**让「对外契约」不被「实现细节」污染**。

**不可信响应的归一化。** 会话服务那批 `normalize*` 函数与主核里「解析客户端发来的 SQL 文本要逐段校验」是同一种防御姿态。差别在于主核的不可信输入是 SQL 文本，这里的不可信输入是 HTTP JSON；共同点是**都在边界处做「形状校验 + 缺省填充 + 拒绝不合规值」**，而不是相信上游说自己返回了什么。

**凭据只在需要时存在。** MCP 凭据的「脱敏展示 + 按需回填」与主核里「连接凭据解密到局部变量、连上即弃」是同一条原则的两种实现。这条原则的价值在于：即使前端/进程内存被读取，也拿不到长期有效的明文。

对 2B 波次的整体判断：这是 13 个项目里唯一一个**不以「agent 怎么跑」为机制**的项目。它说明了一件重要的事——当 agent 生态里实现百花齐放时，真正被复用的是「控制面协议」，而不是任何一个 agent 循环。想理解 agent 工程的实际架构重心，这个项目比任何一个框架都更直接。
