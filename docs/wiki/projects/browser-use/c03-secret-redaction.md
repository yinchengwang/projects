---
project: browser-use
feature: secret-redaction
title: 敏感值脱敏与环境白名单
page_type: feature
schema_version: 1
refs:
  - path: browser_use/repl/session.py
    symbols: [scoped_secrets, redact, redact_forms, secret_forms]
  - path: browser_use/repl/session.py
    symbols: [worker_env, clip]
---

# 敏感值脱敏与环境白名单

## 一句话职责

模型写的 Python 在独立子进程里执行，而子进程不该拿到父进程的全部环境变量；同时用户提供的敏感值（密码、令牌）必须能被打进页面，却不能出现在回流给模型的文本与文件里。这两件事分别由环境白名单与脱敏通道完成。

## 机制是什么

browser-use 的 1.0 把「动作」交给模型写的 Python 执行。这带来一个与其他框架不同的信任问题：**模型代码就在本机进程里跑，它既不是被裁剪的语言子集（像 smolagents 的 AST 解释器），也不是隔离沙箱**。

模块文档把这一点写得非常直接：环境白名单「只把密钥挡在它自己的环境之外」，而以用户身份运行的工作进程「仍然能读到父进程的环境（`ps -E`、`/proc/<pid>/environ`）和 `.env` 文件」。同样，敏感值虽然从回流文本里被替换，但「模型写的 Python 可以读到这些值、可以写未脱敏的文件、截图或对某个字段做 `js()` 读取也会暴露已输入的值」。

所以这套机制的目标不是「防住恶意模型」，而是**降低意外泄漏面**：不让密钥出现在环境变量里被无意读取，不让敏感值出现在日志、单元格文件、检查点与回流文本里被无意带到模型上下文。

## 怎么实现的

**环境白名单 `worker_env`。** 三层过滤从父进程环境里挑出可传递的键：

1. **白名单集合**：路径类（`PATH`、`HOME`、`APPDATA`、`TEMP`）、系统类（`SYSTEMROOT`、`COMSPEC`）、终端与显示（`TERM`、`DISPLAY`、`WAYLAND_DISPLAY`）、Python 相关（`PYTHONPATH`、`VIRTUAL_ENV`）、以及代理与 CA（`HTTP_PROXY`、`SSL_CERT_FILE`、`REQUESTS_CA_BUNDLE`）。代理与 CA 特意保留，注释说明理由是「单元格里的 `http_get()` 与 requests/httpx 需要它们」。
2. **前缀白名单**：`LC_`（本地化）、`BU_` 与 `BH_`（browser-harness 自己的变量，如 `BU_NAME`、`BH_RUNTIME_DIR`）。
3. **显式拒绝 + 模式拒绝**：即使命中白名单，键名匹配 `KEY|TOKEN|SECRET|PASSW|CREDENTIAL|AUTH` 的也一律剔除。这条放在最后，所以是兜底。

启动时还会**覆盖若干变量**来收紧行为。其中三处值得单独看：

- `BH_REQUIRE_EXISTING_DAEMON='1'` 与 `BU_CDP_WS='ws://127.0.0.1:9/...'`：注释解释理由是「一个朴素调用绝不能连到用户自己的 Chrome——CLI 调用只能复用本次运行的守护进程，或者失败；单元格启动的守护进程会去拨一个死端口，而不是发现本地 Chrome」。
- `BH_AGENT_WORKSPACE` 指向工作区内的目录：注释写明「`browser-harness` 导入时会执行 `agent_helpers.py` 并读它的 domain-skills，绝不能是用户全局那个——被提示注入的一次运行可以给后续运行埋东西」。
- `BU_MODEL_CODE='1'`：注释说明模型的 `feedback()` 调用不算用户同意。

**敏感值作用域 `scoped_secrets`。** 接受 0.x 的 `sensitive_data` 形态或 `(name, value[, domain])` 元组，统一成 `(名字, 值, 域名模式或 None)` 三元组。没有域名模式的值「在任何地方都能输入」；带域名的「只在 URL 匹配的页面输入」。这把「值能不能被用」与「值在哪里能被用」分开表达。

**脱敏两条路径。** `secret_forms` 生成一个值的多种可辨识形态（含 HTML 转义形态、base64 核心），`redact_forms` 按这些形态做替换。`redact` 是字符串级入口。

一个细节：`SHORT_SECRET = 4` —— 长度小于 4 的值**只在独立出现时**被替换，不在更长的单词或数字内部替换。否则一个三字符的值会把正文里所有巧合的子串都打上马赛克。

另外 `SECRET_PLACEHOLDER` 同时匹配 0.x 的 `<secret>name</secret>` 与 1.0 的 `<secret:name>` 两种占位形态——这是为兼容旧对话记录里已存在的占位符。

**`clip` 与输出上限。** 单个单元格的输出在**工作进程里**就被截断（`MAX_OUTPUT_CHARS = 12_000`），注释说明理由是「一次巨大的 print 永远不该跨过通道」。这是把限流放在产生侧而不是消费侧。

## 关键取舍

**源码事实：** 模块文档明确写「Not a sandbox」，并逐条列出这套机制挡不住的路径：工作进程能读父进程环境与 `.env`、模型代码能读到敏感值本身、能写未脱敏的文件、截图与 `js()` 读取会暴露已输入的值。这是把安全边界交代清楚，而不是给出虚假保证。

**源码事实：** 环境过滤是「白名单 + 前缀 + 模式兜底」三层，且模式匹配放在最后，所以即使未来往白名单里加了带 `TOKEN` 的键也会被剔除。

**源码事实：** 短于 4 字符的敏感值只在独立出现时替换，避免过度脱敏把正常文本打碎。

**源码事实：** 输出截断发生在工作进程侧（`MAX_OUTPUT_CHARS`），不是父进程收到后再裁。

**架构推论：** 与 smolagents 的对比最能说明设计取向差异。smolagents 选择**裁剪语言**（AST 解释器只实现白名单语法），因此能保证「代码做不到某些事」；browser-use 选择**不裁剪语言、只限制环境与回流**，因此模型能写任意 Python，但密钥不进环境、敏感值不出现在回流里。前者的保证更强但兼容性更差（新语法默认不可用），后者的表达力完整但安全依赖「不泄漏」而非「做不到」。

**架构推论：** `BH_AGENT_WORKSPACE` 那条注释揭示了 agent 工具链特有的一类风险：**跨运行的持久注入点**。用户全局配置目录里的文件会被下一次运行读取，于是一次提示注入可以让后续所有运行都带上恶意辅助代码。防法是「把工作区私有化」，而不是「检测恶意内容」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/repl/session.py` | `worker_env` | 三层过滤的环境白名单构造 |
| `browser_use/repl/session.py` | `scoped_secrets` | 敏感值归一为（名字、值、域名模式） |
| `browser_use/repl/session.py` | `redact` / `redact_forms` | 按形态替换敏感值 |
| `browser_use/repl/session.py` | `secret_forms` | 生成一个值的多种可辨识形态 |
| `browser_use/repl/session.py` | `clip` | 单单元格输出上限 |
| `browser_use/repl/session.py` | `check_loop` | Windows 上需 Proactor 事件循环的前置检查 |

## 参考

- [browser-use 源码：repl/session.py](https://github.com/browser-use/browser-use/blob/main/browser_use/repl/session.py)
- [browser-use 文档：敏感数据](https://docs.browser-use.com/customize/sensitive-data)
- [browser-use 仓库：MIGRATING.md 的信任模型一节](https://github.com/browser-use/browser-use/blob/main/MIGRATING.md)
- [browser-harness 仓库](https://github.com/browser-use/browser-harness)

## 与主核 / AI 的关系

这套「环境白名单 + 值级脱敏」与主核里的凭据处理同构。主核也要在连接外部存储时使用密码与密钥，同样不能让它们出现在日志、错误消息、审计记录里。差别在威胁模型：主核的代码是可信的，所以只需防「意外泄漏」；browser-use 的代码由模型生成，属于半可信，所以还要防「主动读取」——而它在文档里承认这一步防不住。

**与 smolagents 的取舍对照值得记住**：裁剪语言 vs 限制环境。两者都叫「安全」，但给出的保证完全不同：前者是「做不到」，后者是「不容易」。工程上要按场景选：跑完全不可信代码必须上真隔离（进程/容器/微虚机）；跑模型生成的代码、且能接受「它会读到密钥但大概率不会外发」这个残余风险，才可以用限制环境这条路。

`BH_AGENT_WORKSPACE` 那条注释还提示了一类主核里也有对应的问题：**持久化配置目录是被信任的输入**。主核的配置文件、扩展目录一旦被写入，后续启动都会执行其中的内容。防法与这里一致——把可写范围限制在工作区内，而不是扫描内容找恶意特征。
