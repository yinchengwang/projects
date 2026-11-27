---
project: browser-use
page_type: overview
display_name: Browser Use
repo: https://github.com/browser-use/browser-use
license: MIT
language: Python
stars: 121704
stars_as_of: 2026-11-22
refs:
  - path: browser_use/agent/service.py
    symbols: [Agent]
  - path: browser_use/agent/loop.py
    symbols: [BrowserLoop, python]
  - path: browser_use/repl/session.py
    symbols: [Repl, worker_env]
  - path: browser_use/ax.py
    symbols: [AxHelpers]
  - path: browser_use/browser/browser.py
    symbols: [Browser]
  - path: browser_use/domains.py
    symbols: [DomainRules, Guard]
---
# Browser Use

## 一句话职责

浏览器自动化 agent：把「操作浏览器」这件事交给模型写的 Python 代码，代码在一个常驻 REPL 子进程里执行，进程里既有 browser-harness 辅助函数也有裸 CDP。

## 机制是什么

1.0 与 0.x 有根本差别，这一点必须先说清：**0.x 每步从 DOM 快照里挑一个预定义动作，1.0 让模型直接写 Python**。

这个转变把浏览器 agent 的设计问题重新排列了一遍。不再是「如何把页面表征成动作空间」（那需要元素编号、动作 schema、每步一次模型往返），而是「如何给模型一个能操作浏览器的 Python 环境，并约束它别出事」。于是重心从「动作定义」移到「执行环境」：

- **REPL 会话**：父进程起一个独立子进程当解释器，通过 127.0.0.1 回连 + 随机令牌自证；单元格超时杀掉整个进程组再重启。
- **能力注入**：子进程里预装 `browser-harness` 的辅助函数与裸 CDP 通道，模型可以直接 `await page.click(...)` 也可以自己发 CDP 命令。
- **约束而非裁剪**：不像 smolagents 用 AST 解释器限制语法，这里语言能力完整，靠**环境变量白名单 + 敏感值脱敏 + 域名规则 + 独立守卫进程**来降低影响面，并在文档里明确「不是沙箱」。
- **0.x API 保留**：`Agent`、`Browser`、`Tools`、`run()`、`final_result()` 仍是公开面；访问已移除的 0.x 名称会抛出带迁移指引的错误。

本文库按经审阅的机制单元组织：**9 个机制领域、共 25 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 9 个领域按下面的顺序排列：A Agent 入口与生命周期 → B 循环、投递与预算 → C REPL 会话与工作进程 → D 动作、观测与结果 → E 浏览器后端 → F 域名与网络策略 → G MCP 与 CLI 接入 → H 遥测与录制 → I 兼容、配置与自评。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`browser_use/` 包全部子目录。

## 机制导航（25 篇 / 9 组）

### A-Agent 入口与生命周期（2 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-agent-entry.md`](a01-agent-entry.md) | Agent 门面与运行生命周期 |
| [`a02-agent-construction.md`](a02-agent-construction.md) | Agent 构造期校验与参数透传 |

### B-循环、投递与预算（4 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-agent-loop.md`](b01-agent-loop.md) | 循环骨架与 python 工具 |
| [`b02-delivery-guards.md`](b02-delivery-guards.md) | 投递校验与预算 |
| [`b03-compaction-and-images.md`](b03-compaction-and-images.md) | 上下文压缩与图像裁剪 |
| [`b04-streaming-recording.md`](b04-streaming-recording.md) | 响应流改造与请求录制 |

### C-REPL 会话与工作进程（3 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-repl-session.md`](c01-repl-session.md) | REPL 父进程侧与会话管理 |
| [`c02-repl-worker.md`](c02-repl-worker.md) | 工作进程与单元执行 |
| [`c03-secret-redaction.md`](c03-secret-redaction.md) | 敏感值脱敏与环境白名单 |

### D-动作、观测与结果（3 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-tools-and-actions.md`](d01-tools-and-actions.md) | 工具注册与动作结果 |
| [`d02-accessibility-tree.md`](d02-accessibility-tree.md) | 无障碍树观测与交互原语 |
| [`d03-history-and-result.md`](d03-history-and-result.md) | 历史列表与结果提取 |

### E-浏览器后端（3 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-browser-backends.md`](e01-browser-backends.md) | 浏览器后端选择与连接 |
| [`e02-local-browser.md`](e02-local-browser.md) | 本地浏览器进程与用户目录 |
| [`e03-downloads-and-proxy.md`](e03-downloads-and-proxy.md) | 下载与代理轮换 |

### F-域名与网络策略（3 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-domain-rules.md`](f01-domain-rules.md) | 域名规则与守卫进程 |
| [`f02-network-and-url.md`](f02-network-and-url.md) | URL 规范化与 TLS 请求 |
| [`f03-web-search.md`](f03-web-search.md) | 网页搜索接入 |

### G-MCP 与 CLI 接入（2 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-cli-mcp-server.md`](g01-cli-mcp-server.md) | CLI MCP 服务端 |
| [`g02-skills-packaging.md`](g02-skills-packaging.md) | 技能包与安装 |

### H-遥测与录制（3 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-product-telemetry.md`](h01-product-telemetry.md) | 产品遥测与离线投递 |
| [`h02-run-recorder.md`](h02-run-recorder.md) | 运行录制与步骤归集 |
| [`h03-feedback-and-auth-events.md`](h03-feedback-and-auth-events.md) | 反馈与登录事件 |

### I-兼容、配置与自评（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-legacy-compat.md`](i01-legacy-compat.md) | 0.x 兼容层与已移除名称 |
| [`i02-config-and-errors.md`](i02-config-and-errors.md) | 配置加载与错误类型 |
| [`i03-self-judging.md`](i03-self-judging.md) | 运行后自评 |

## 关键取舍

**源码事实：** REPL 会话通过「子进程回连 127.0.0.1 临时端口 + 随机令牌自证」建立通道，而不是继承文件描述符。这让它在 Windows、macOS、Linux 上行为一致。

**源码事实：** 敏感值脱敏模块的文档明确写「Not a sandbox」，并逐条列出挡不住的路径：工作进程仍能读父进程环境与 `.env`、模型代码能读到敏感值本身、能写未脱敏的文件、截图与 `js()` 读取会暴露已输入的值。

**源码事实：** 子进程拿到的环境是「白名单 + 前缀白名单 + 模式兜底」三层过滤，键名匹配 `KEY|TOKEN|SECRET|PASSW|CREDENTIAL|AUTH` 的一律剔除。

**源码事实：** 访问已移除的 0.x 名称时抛出的错误里带「1.0 的等价写法」。连 `import browser_use.dom`（0.x 的模块路径）都会得到迁移指引。这是把升级体验当产品设计。

**架构推论：** 「代码即动作」在浏览器场景的收益比在其他场景更明显。网页操作本质上是多步组合（打开、等待、定位、输入、点击、断言），用 JSON 动作表达需要每步一次模型往返；用 Python 表达可以一次完成一段流程，中间结果留在变量里不进提示词。

**架构推论：** 代价是执行环境成为核心复杂度。这个项目 47 个非测试 Python 文件里，`repl/` 与 `browser/` 占了大部分——都是为了把「模型写的代码要能安全地碰到浏览器」这件事做稳。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `browser_use/agent/service.py` | `Agent` | 0.x 公开门面：组装模型与浏览器、生命周期 |
| `browser_use/agent/loop.py` | `BrowserLoop` | 循环主体：python 工具、投递、压缩 |
| `browser_use/repl/session.py` | `Repl` | 父进程侧会话：起停工作进程、外发调用 |
| `browser_use/repl/worker.py` | `_run` | 工作进程：编译执行单元格 |
| `browser_use/ax.py` | `AxHelpers` | 无障碍树观测与点击输入 |
| `browser_use/browser/browser.py` | `Browser` | 后端选择与连接 |
| `browser_use/browser/local.py` | `LocalBrowser` | 本地 Chromium/Chrome 进程管理 |
| `browser_use/domains.py` | `DomainRules` | 域名允许/阻断规则 |
| `browser_use/mcp/cli_mcp.py` | `CLIMCPServer` | 以 MCP 工具形式暴露浏览器能力 |
| `browser_use/legacy.py` | `removed_name` | 0.x 名称的迁移错误 |

## 参考

- [browser-use 源码仓库](https://github.com/browser-use/browser-use)
- [browser-use 文档](https://docs.browser-use.com/)
- [browser-harness 仓库（REPL 辅助函数来源）](https://github.com/browser-use/browser-harness)
- [browser-use 仓库：MIGRATING.md 与信任模型](https://github.com/browser-use/browser-use/blob/main/MIGRATING.md)

## 与主核 / AI 的关系

browser-use 与主核（多模态数据库内核）在「如何约束一个通用语言的执行」这一点上可以三向对照：

**三种约束策略。** smolagents 选**裁剪语言**（AST 解释器只实现白名单语法，保证「做不到」）；browser-use 选**限制环境**（语言完整，靠环境白名单与脱敏降低影响，保证「不容易」）；主核选**编译与类型化**（SQL 被编译成算子树，保证「只能表达可优化的操作」）。三者给出的保证强度递减，表达力递增。

**REPL 与查询执行。** 常驻 REPL 的「变量在单元格之间保留」对应主核里「查询计划里中间结果可以留在算子树节点上」。两者都在解决同一件事：不必把中间结果序列化到外部再读回来。差别是 REPL 的中间结果活在 Python 堆里，主核的活在元组流里。

**跨运行的持久注入点。** `BH_AGENT_WORKSPACE` 那条注释揭示的风险——用户全局配置目录被写入后，后续运行都会读取——在主核里对应的是「扩展目录、配置文件一旦被篡改，后续启动就执行恶意代码」。防法一致：把可写范围限制在工作区内，而不是扫描内容找特征。

在 2B 波次里，browser-use 是「工具即代码」路线在垂直场景（浏览器）上的完整实现。它比 smolagents 更能说明这条路线的真实成本：真正难的不是让模型生成代码，而是给代码一个**既能操作真实世界、又不至于失控**的运行环境。
