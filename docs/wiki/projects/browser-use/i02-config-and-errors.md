---
project: browser-use
feature: config-and-errors
title: 配置加载与错误类型
page_type: feature
schema_version: 1
refs:
  - path: browser_use/config.py
    symbols: [Config, load_env]
  - path: browser_use/exceptions.py
    symbols: [MissingAPIKeyError, BrowserConfigError, BrowserConnectionLost]
---

# 配置加载与错误类型

## 一句话职责

两件基础设施：`Config` 用一个**每次访问都读环境变量**的属性集合给全包提供设置（外加 `.env` 的加载时机约束），而 `exceptions.py` 把三类失败——缺密钥、浏览器配置无效、连接丢失——做成**有名有姓、能区分责任、且自带修复指引**的类型。

## 机制是什么

| 层 | 内容 |
|---|---|
| 设置读取 | `Config`（模块级单例 `CONFIG`）+ `load_env()` + 三个环境探测（docker / container） |
| 错误类型 | `MissingAPIKeyError(RuntimeError)`、`BrowserConfigError(ValueError)`、`BrowserConnectionLost(ConnectionError)` |
| 错误共用工具 | `removed()`、`tagged()`、`keys_url()`、`missing_key_text()` |

`MissingAPIKeyError` 还带两个「枚举型」字段 `key_gate` 与 `key_reason`——它们的用途写在 docstring 里：**遥测会发这两个枚举，绝不发密钥本身**。

## 怎么实现的

**`Config` 的类 docstring 只有一句，但它决定了整个读取策略：每次访问都读环境，所以测试与运行中途的 `os.environ` 变更都会被尊重。** 这也解释了为什么它是三个 `@property` 而不是在 `__init__` 里缓存字段——`ANONYMIZED_TELEMETRY` 之类的开关必须在「用户关掉之后立刻生效」，而 `browser-use telemetry disable` 是在同一个进程里改环境变量的。

**`ANONYMIZED_TELEMETRY` 的解析里藏着一条安全取向的注释。** 实现是 `value[0] in 'ty1'`，旁边注释：**`'' in 'ty1'` 是 `True`：一个空值会让遥测留在开启状态**。也就是说 `value != ''` 那个前置判断是必要的——没有它，一个写成 `ANONYMIZED_TELEMETRY=`（空值）的环境会让 `value[0]` 抛 `IndexError` 而不是「关闭」。而这里选择的方向是：**空值 = 默认（开启）**，因为默认值本就是 `'true'`。这个选择本身可以商榷，但重要的是它被写成了注释而不是留给读者猜。

**`BROWSER_USE_CONFIG_DIR` 是一个会**副作用地创建目录**的 property。** 它在返回前 `mkdir(parents=True, exist_ok=True)`，且路径先 `expanduser().resolve()`。把它放在 property 上有代价（读一个设置会碰磁盘），但收益是**任何拿到这个目录的调用方都不必再自己创建**。优先级是 `BROWSER_USE_CONFIG_DIR` 环境变量 → `XDG_CONFIG_HOME/browseruse` → `~/.config/browseruse`。这条链让 Linux 上遵守 XDG 约定，同时给了显式覆盖点。

**`load_env()` 的关键不是「读 .env」而是「谁调用它」。** docstring 三句都是约束：

1. **从工作目录（或其父目录）读 `.env` 进 `os.environ`，如 0.x 在 import 时做的那样；已设的变量优先。**
2. **由 Agent 与 Browser 模块调用，于是凭据能从 `.env` 解析出来。**
3. **REPL worker 从不调用它。**

第三条是与 `c03-secret-redaction` 里环境白名单直接配套的：worker 的环境是白名单过滤后的，若它自己去读 `.env`，那个过滤就被绕过了。所以「不调用」是一条安全约束而不是性能选择。而 `find_dotenv(usecwd=True)` 的 `usecwd` 参数保证从当前工作目录搜，而不是从 `dotenv` 模块所在位置。

**`is_running_in_docker` 与 `is_in_container` 是两个不同粒度的探测，注释解释了后者为何存在。** `is_running_in_docker` 只看 `/.dockerenv` 与 `/proc/1/cgroup` 里的 `docker` 字样。`is_in_container` 的 docstring 直接批评前者：**只看 Docker 会漏掉 podman 与每一个 Kubernetes pod**。于是它多加三种信号：`KUBERNETES_SERVICE_HOST`（注释：**每个 pod 里都设**）、`container` 环境变量（注释：**podman、systemd-nspawn 会这么说**）、以及 `/run/.containerenv`（podman）。

两个函数都用了 `@cache` / `try/except`，理由一致：探测本身碰文件系统，答案在一个进程里不会变。而 `is_running_in_docker` 用 `@cache`、`is_in_container` 没加（因为它内部调用了前者），所以 `is_in_container` 的重复代价只有两次 `getenv`。

**错误类型的选择是按「谁能处理」定的。** `MissingAPIKeyError` 继承 `RuntimeError`（配置/环境问题）；`BrowserConfigError` 继承 `ValueError`（**给的选项名不存在**，属于调用方参数错）；`BrowserConnectionLost` 继承 `ConnectionError`（传输层问题）。三个父类对应三种责任方：环境、调用方、运行时。这个选择让上层的 `except` 能按语义捕获，而不必认识具体的类名。

**`BrowserConfigError` 的 docstring 把「报错时机」也写进了契约。** 原文：**一个什么也不命名的 Browser 选项（一个 Chrome 或云 profile），或者没有可写的 browser-harness 目录，在浏览器启动时被发现，在任何浏览器跑起来或被计费之前：`Agent.run()` 像抛其它配置错误那样抛它**。

其中两个信息很关键：一是**发现时机是「启动时、任何浏览器跑起来之前」**，这与 `i01` 里 `RemovedField` 的设计目标完全一致（都要赶在计费之前）；二是**它不是 `run()` 的返回值而是异常**——与 0.x 里「provider 失败通过 `result.error` 表达」形成对照，配置错是异常而同一次运行内的模型/网络故障是状态。

**`BrowserConnectionLost` 的默认消息就带上了「为什么」。** 构造函数默认 `why='the browser-harness daemon is not running'`，消息前缀 `browser connection lost: `。给一个默认原因而不是空消息，是一个小而实际的取向：大多数情况下原因就是那个，而自定义原因可以是别的（崩溃、被杀）。

**`MissingAPIKeyError` 把「哪种情况」与「缺什么」分成两个维度。** 构造参数是 `what`（缺哪个能力，例如 `Browser Use Cloud`）、`reason`（可选的具体原因）、`gate`（`default_model` / `gateway_model` / `web_search` / `cloud_browser`）、`key_reason`（`missing` / `corrupt_store` / `unwritable_store` / `bad_chars`）。

docstring 明确了后两者的用途：**`key_gate` 与 `key_reason` 是遥测会发的枚举，绝不发密钥本身**。这是把「错误对象」同时当作「遥测数据源」的做法——一类信息只定义一次，报错与统计都用它。而与 `h03` 的 `library_auth` 通道对接时，`key_gate`/`key_reason` 就是那两个字段。

**有 `reason` 时消息完全不同，这是一处刻意的分支。** `reason` 非空时消息是 `f'{what} needs a Browser Use API key. {reason}'` 并**直接返回**；否则才走 `missing_key_text()` 那条「四行修复指引」。理由很明显：**如果原因是「密钥存储损坏」或「密钥无法发送」，那么「去注册一个新密钥」这个指引是错的**——需要的是修存储。

**`summary` 属性是「错误消息要出门，所以必须先脱敏」的实现。** docstring：**第一行，给遥测用。密钥存储的错误会带上存储路径，而 `scrub` 会把 HOME 之外的路径保留（`BH_AUTH_PATH=/srv/<customer>/auth.json`）：所以那两种原因只说缺了什么能力与原因**。

也就是 `corrupt_store` / `unwritable_store` 时 `summary` 返回一个**不含路径的固定格式**，其余情况才用 `str(self).splitlines()[0]`。这条与 `h03` 里「绝不发密钥、id、项目、URL、state、code」是同一套边界，只是执行位置在错误类型上。

**`missing_key_text` 的「最多四行」是有结构的。** docstring：**缺密钥的修复法，最多 4 行：先登录（步数最少），再环境变量，最后无密钥的用法**。并且有一个 notebook 分支：**在 notebook 里第一行是那个从单元格登录的单次调用（没有终端、没有重启）**——`browser_use.login()`。判定方式是 `'ipykernel' in sys.modules`（可被 `notebook` 参数覆盖）。

这个分支的存在理由值得注意：在 Jupyter 里，`browser-use login` 是一个 shell 命令、且登录后要重启进程才能读到环境变量——两条都不成立。所以 notebook 必须给一个**在同一个进程里就能完成**的路径。

**`NO_KEY` 的文案是按 gate 定制的「没有密钥也能跑」的替代方案。** `web_search` → **关掉搜索，agent 照样能浏览**；`cloud_browser` → **`Browser('chromium')` 用本地浏览器**；默认（`NO_KEY_MODEL`）→ **用自己的模型加本地浏览器**。这三个答案让「缺密钥」不总是死路——这在一个默认走云网关的产品里是必要的。

**`keys_url` 的标记既是统计也是隐私边界。** docstring：**`KEYS_URL` 被打上「库把用户从哪送来」的标记（ref、gate、版本）：于是 dashboard 的页面浏览能算出库带来的流量。绝不含设备 id 或任何关于用户的东西；遥测关闭时不打标记**。

实现分两层：`keys_url()` 组装 query，`tagged()` 负责「遥测开启才加」。而 `tagged` 的 docstring 给出了一个更实际的理由：**关闭状态下，一个错误消息里的链接仍会告诉 cloud.browser-use.com 这次点击来自哪里**。也就是说**链接参数本身也是遥测**，必须跟着开关走。

`tagged` 还对 `telemetry().enabled` 的判断包了 `try/except`，注释：**一个不可读的设置：纯链接一样能用**。失败方向是「不加标记」。

**`removed()` 是 0.x 移除文案的唯一模板。** 它的输出结构固定：`{feature} was removed in browser-use 1.0.` + `Open an issue at {ISSUES_URL} if you need it.` + （有替代时）`In 1.0: {instead}` + `Migration guide: {MIGRATING_URL}`。

这三段的顺序有意义：**先确认发生了什么，再给出「还需要它怎么办」的出口（开 issue），然后才是替代方案**。开 issue 摆在替代方案之前是一个产品判断——它承认可能存在「1.0 真的没有等价物」的情况，而不是假定每个移除都有答案。这正是 `i01` 里那些空答案（`''`）存在的空间。

**两个模块（`config.py` 与 `exceptions.py`）末尾都有同一段兼容层钩子。** `if not TYPE_CHECKING:` 后面把 `__getattr__` 设成 `removed_names(__name__)`，注释说明「**一个 0.x 名字会抛指名其 1.0 答案的 removed 错误**」。`legacy.py` 的 `_NAMES` 表里确实为这两个模块各自列了一批名字（`config` 有十三个、`exceptions` 有 `LLMException`）。

值得一提的是 `_removed_name` 注释里那句：**`from browser_use.exceptions import removed` 在这里做而不是顶部，因为这个模块是从 exceptions import 的模块里被 import 的**。也就是说 `legacy.py` 与 `exceptions.py` 之间是一个循环，靠延迟导入解开。

## 关键取舍

**源码事实：** `Config` 每个属性都现场读环境，因此进程内改环境（如 CLI 关掉遥测）立刻生效。

**源码事实：** `ANONYMIZED_TELEMETRY` 明确把空值当默认（开启），且这条判断被写成注释而不是留给推断。

**源码事实：** `BROWSER_USE_CONFIG_DIR` 读取即创建目录，路径按 `BROWSER_USE_CONFIG_DIR` → `XDG_CONFIG_HOME` → `~/.config` 三级。

**源码事实：** `load_env()` 由 Agent 与 Browser 模块调用，**REPL worker 从不调用**——否则环境白名单会被 `.env` 绕过。

**源码事实：** `is_in_container` 是对 `is_running_in_docker` 的修正：后者只看 Docker 会漏掉 podman 与每一个 Kubernetes pod。

**源码事实：** 三个错误类型各继承不同父类（RuntimeError / ValueError / ConnectionError），对应「环境 / 调用方 / 传输」三种责任方。

**源码事实：** `BrowserConfigError` 在浏览器启动时被发现、在任何浏览器跑起来或被计费之前抛出，且是异常而不是 `result.error`。

**源码事实：** `MissingAPIKeyError` 的 `key_gate` / `key_reason` 是给遥测的枚举，绝不携带密钥；有具体 `reason` 时不给「去注册新密钥」这条指引，因为那是错的指引。

**源码事实：** `summary` 在存储类原因为避免泄漏路径而返回固定格式；`tagged` 在遥测关闭时不加任何链接参数，因为链接参数本身就是遥测。

**源码事实：** 缺密钥文案最多四行，且 notebook 环境下第一行换成 `browser_use.login()`——因为在 Jupyter 里 shell 命令与「重启读环境」都不成立。

**架构推论：** 这一层把「错误」当成**又要给人看、又要给机器读的东西**。人看到的是一条带修复步骤的消息（最多四行、按环境分叉、按 gate 给替代方案）；机器读的是两个枚举与一个脱敏后的 `summary`。两套消费同时存在决定了几乎每个设计：为什么 `reason` 非空时不给通用指引（因为指令错了）、为什么 `summary` 要特殊处理路径（因为路径会出门）、为什么链接参数跟着遥测开关（因为它本身是数据）。

而 `Config` 那一侧的原则相反：**能读到最新状态比省几次 `getenv` 重要**。在一个会被 CLI、MCP 服务端、REPL 父进程同时使用的库里，把设置缓存进对象属性会让「用户在运行中途关掉遥测」这类操作失效——而这类操作恰好在 `h01` 里被设计成「立刻生效、连痕迹都不留」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/config.py` | `Config` | 三个每次访问都读环境的属性 |
| `browser_use/config.py` | `Config.ANONYMIZED_TELEMETRY` | 开关解析，空值按默认 |
| `browser_use/config.py` | `Config.BROWSER_USE_CONFIG_DIR` | 三级优先级的配置目录，读取即创建 |
| `browser_use/config.py` | `load_env` | `.env` 加载与其调用者约束（worker 不调） |
| `browser_use/config.py` | `is_running_in_docker` / `is_in_container` | 容器探测的两级粒度 |
| `browser_use/config.py` | `CONFIG` | 模块级单例 |
| `browser_use/exceptions.py` | `MissingAPIKeyError` | 缺密钥错误，含给遥测的 `key_gate` / `key_reason` |
| `browser_use/exceptions.py` | `MissingAPIKeyError.summary` | 脱敏后的单行版本，存储类原因不带路径 |
| `browser_use/exceptions.py` | `missing_key_text` | 最多四行的修复指引，含 notebook 分支 |
| `browser_use/exceptions.py` | `BrowserConfigError` | 无效 Browser 选项，启动期、计费前抛出 |
| `browser_use/exceptions.py` | `BrowserConnectionLost` | 守护进程丢失，默认自带原因 |
| `browser_use/exceptions.py` | `removed` | 0.x 移除文案的唯一模板 |
| `browser_use/exceptions.py` | `keys_url` / `tagged` | 链接标记，遥测关闭时不打 |
| `browser_use/exceptions.py` | `NO_KEY` / `NO_KEY_MODEL` | 按 gate 定制的「无密钥也能跑」方案 |
| `browser_use/exceptions.py` | `ISSUES_URL` / `MIGRATING_URL` | 移除文案里的两个出口 |

## 参考

- [browser-use 源码：config.py](https://github.com/browser-use/browser-use/blob/main/browser_use/config.py)
- [browser-use 源码：exceptions.py](https://github.com/browser-use/browser-use/blob/main/browser_use/exceptions.py)
- [browser-use 文档：环境变量与配置](https://docs.browser-use.com/customize/configuration)
- [python-dotenv 文档](https://saurabh-kumar.com/python-dotenv/)
- [XDG Base Directory 规范](https://specifications.freedesktop.org/basedir-spec/latest/)

## 与主核 / AI 的关系

这一层与主核里的「配置读取 + 错误码体系」完全同构。数据库同样区分「配置错」（`FATAL`/启动期，如数据目录不可写）与「运行期错」（`ERROR`/单语句，如连接断开），并且同样要求配置错必须在做任何昂贵动作之前暴露。`BrowserConfigError` 那条「在任何浏览器跑起来或被计费之前」就是主核那句「在分配共享缓冲之前校验参数」的等价物。

对 agent 工程的直接价值有三条。其一，**缺密钥的报错要按 gate 分叉**。同一句「请设置 API_KEY」对一个想搜网页的人和一个想用云浏览器的人是不同的事后动作；笼统的指引只会让人多做无效功。其二，**有具体原因时不要给通用指引**。「密钥存储损坏」的正确修复是修存储，不是重新注册。其三，**环境变量的读取不该被缓存进对象**。一个框架可能被 CLI、服务端、子进程同时使用，且「运行中途改环境」是用户唯一可用的开关手段——缓存会让这个开关失效，而失效的表现是「关了还在发」。
