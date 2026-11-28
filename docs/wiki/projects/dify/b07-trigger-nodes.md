---
project: dify
feature: trigger-nodes
title: 触发器节点
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/nodes/trigger_webhook/node.py
    symbols: [TriggerWebhookNode]
  - path: api/core/workflow/nodes/trigger_webhook/entities.py
    symbols: [WebhookParameter, WebhookBodyParameter]
  - path: api/core/workflow/nodes/trigger_schedule/trigger_schedule_node.py
    symbols: [TriggerScheduleNode]
  - path: api/core/workflow/nodes/trigger_plugin/trigger_event_node.py
    symbols: [TriggerEventNode]
---

# 触发器节点

## 一句话职责

三种「工作流可以被谁唤醒」的入口节点：webhook 由外部 HTTP 调用触发、schedule 由定时器触发、plugin 由插件事件触发。三者本身几乎不做事——它们只是把「触发时注入到变量池里的数据」转成下游能用的输出。

## 机制是什么

触发器节点是**唯一一类「执行时机在用户点击之前」的节点**。b02 讲过 `is_start_node_type` 把开始节点、数据源节点与三类触发器都算作入口；这一篇讲这三类触发器各自长什么样。

关键认知是：**触发器节点自己不接收 HTTP 请求、不跑定时器、不订阅插件事件**。真正的接收发生在别处——webhook 的请求由 trigger controller 解析后注入变量池，定时由调度侧负责，插件事件由插件守护进程推过来。节点执行时，数据**已经在变量池里了**，节点的职责只是「取出来、按配置整形、写成输出」。

这个分工的理由在 webhook 节点的 `_run` docstring 里写得最清楚：「Like the start node, this simply takes the webhook data from the variable pool and makes it available to downstream nodes. The actual webhook handling happens in the trigger controller.」

所以三个节点的代码极短（webhook 最长，因为它要做类型整形；schedule 与 plugin 各只有几十行），而它们的复杂度在**别处的三件东西**上：`NodeExecutionType.ROOT` 的执行类型、`get_default_config` 提供的画布初值、以及 entities 里那套参数类型白名单。

## 怎么实现的

**三个节点都把 `execution_type` 声明成 `ROOT`。** 这个声明告诉引擎：这是图的根，没有前驱，可以被直接启动。它与 b02 的 `is_start_node_type` 是同一件事的两个层面——前者给执行器看，后者给注册表/校验看。

**`get_default_config` 是画布新建节点时的初值。** 三个都实现了，内容都是「一份能跑通的最小配置」：webhook 默认 GET + JSON + 异步 + 30 秒超时 + 状态码 200；schedule 默认可视化模式 + 每天 + 中午 12 点 + UTC；plugin 全是空字符串。这个函数的意义是**让画布拖出一个节点时它是合法的**，而不是等用户填完才能保存。

**webhook：唯一需要真整形的节点。**

`_run` 做三件事：取本节点前缀下的所有变量（`get_by_prefix(self.id)`）作为输入；按配置抽取输出；把系统变量也拼进输出（键形如 `系统变量节点 ID.变量名`）。

输出抽取 `_extract_configured_outputs` 是全组最复杂的逻辑，按配置的三类字段分别处理：

- **headers**：先把实际收到的 header 全转小写建一个索引，然后按配置里的名字依次取。取值时先试原名、再试下划线/连字符互换的别名、最后试小写索引。**输出键用 `_to_sanitized` 把连字符换成下划线**，因为变量名不能带连字符。
- **query params**：直接查 `query_params` 字典，**不做大小写或形态归一**。
- **body**：按 `content_type` 先分两大类。文本类型下整个 body 被当成一个字符串塞给**每一个**配置的 body 参数（`continue` 掉后续处理）；二进制类型下把整个 raw body 交给文件构造，成功就是文件变量，失败就把原始字典当输出。JSON 与表单类型则按参数类型分派：文件类型从 `files` 字典里取并尝试构造文件变量，其余直接从 body 字典按名取。

`_get_normalized` 会**同时尝试连字符与下划线两个方向**（有连字符就试下划线版，反之亦然），所以 `X-Trace-Id` 与 `X_Trace_Id` 两种写法都能命中。

**文件构造是一个独立方法 `generate_file_var`。** 它从文件字典里取 `reference` 或 `related_id` 解析出记录 ID，然后按 `transfer_method` 把 ID 写到**不同字段名**上：本地文件与远程 URL 写 `upload_file_id`、工具文件写 `tool_file_id`、数据源文件写 `datasource_file_id`。这一步是必要的，因为文件工厂按传输方式读不同的字段。构造失败只记 error 日志并返回 `None`，调用方再把原始字典当输出。

注意这个方法**就地修改传入的字典**（`file["upload_file_id"] = file_id`），而传进来的 `file` 是 `webhook_data` 里的对象引用。所以它会污染 `_webhook_raw` 那份"原始"数据——最后那行 `outputs["_webhook_raw"] = webhook_data` 拿到的已经是被改过的字典。**「原始数据」实际上不原始。**

**最后一件事值得单独看：输出里塞了 `_webhook_raw`。** 注释说是「for debugging/advanced use」。这个键不在任何参数配置里，是硬编码的，所以它绕过了「按配置抽取」的原则——**任何 webhook 都会带一个全量原始数据输出**。好处是调试方便；坏处是这个输出会进变量池，下游可以选它，于是「节点输出集合」不再完全由配置决定，而它可能含敏感字段。

**参数类型的白名单是三层的。**

`WebhookParameter`（headers 与 query 共用）允许「字符串、数字、布尔」；但 header 的校验函数与 query 的校验函数用的是不同集合——**header 只允许字符串**，query 允许三种。第三层是 body：允许标量与四种数组加文件。

而 `WebhookParameter` 自己的 `validate_type` 用的是**并集**（header 允许集 ∪ query 允许集）。所以校验实际上做了两遍：构造参数时按并集过一遍，然后在 `WebhookData` 的 `headers`/`params` 校验函数里再按各自的窄集合过一遍。**并集那层对 headers 是松的，真正的约束在容器层。**

**method 有前后两个方向的归一。** `normalize_method` 把输入的字符串转小写，支持大小写混着写；`Method` 是 `StrEnum`，值是全小写。所以 `GET`、`Get`、`get` 都能进。

**`SyncMode` 里有一个容易看错的枚举。** 类里只定义了 `SYNC = "async"`——成员名叫 SYNC 但值是 `"async"`，注释写「only support」。而 `get_default_config` 里的 `async_mode` 是 `True`。也就是说**「同步模式」这个字段的实际含义是「异步触发」**，命名与语义相反。这个枚举在节点代码里没有被引用，只作为文档存在。

**schedule：几乎什么都不做。**

`_run` 与 plugin 完全同构：取本节点前缀变量 + 系统变量，合并成输出，全部成功返回。**唯一差别是它没有 metadata**。

节点的配置（`mode` 是可视化还是 cron、频率、时区）在运行期**完全不被使用**——这些字段只影响调度侧的解析，节点执行时数据已经在变量池里。所以这个节点的代码里看不到任何时间计算。

`get_default_config` 给的初值是「可视化模式 + 每天 + `12:00 AM` + `on_minute: 0` + 周日 + 每月 1 号 + UTC」。这些初值互相矛盾（每天触发却给了星期与日期配置），但它们是给 UI 用的：切换频率时不用重填。

**plugin：多一个元数据。**

`_run` 除了同样的取值合并，还构造一份 `TRIGGER_INFO` 元数据（provider_id、event_name、plugin_unique_identifier），并重写了 `populate_start_event` 把 `provider_id` 挂到起始事件上。

元数据的用途是**在日志/追踪里标记这条执行是被哪个插件事件触发的**。这是三个触发器里唯一一个「节点自己关心触发来源」的——因为插件事件的来源不像 webhook 那样有明确的 HTTP 请求可查。

**三个节点共享的取值模式有一个隐含前提。** 都是 `get_by_prefix(self.id)`，也就是「变量池里所有以这个节点 ID 为前缀的变量」。这意味着注入方必须**按节点 ID 前缀写变量**，否则节点取不到任何东西。而变量池的填充发生在触发侧的控制器里，节点这边完全看不到。**这是一条跨模块的隐式契约**：前缀约定的两端分别在两个不同的子系统里。

也正因为如此，三个节点在「变量池里什么都没有」时行为一致：输出空字典、状态成功。**一个没被正确注入数据的触发器节点会静默产出一个空输出**，而不是报错。

## 关键取舍

**源码事实：** 三个触发器节点都声明 `execution_type = NodeExecutionType.ROOT`，都实现 `get_default_config` 提供画布初值。

**源码事实：** 触发器节点不接收外部请求、不跑定时器、不订阅事件；它们只从变量池按节点 ID 前缀取值。真正的接收在触发控制器/调度侧/插件侧。

**源码事实：** webhook 的 header 取值支持原名、下划线/连字符互换、以及小写索引三级回退；输出键把连字符换成下划线。

**源码事实：** 文本类型的 body 会把整段文本塞给每一个配置的 body 参数；二进制类型把整个 raw body 当文件构造。

**源码事实：** `generate_file_var` 就地修改传入字典，因此最后输出的 `_webhook_raw` 是被改写过的数据。

**源码事实：** `_webhook_raw` 是硬编码输出，不属于任何参数配置，因此所有 webhook 都会额外多一个全量原始数据输出。

**源码事实：** `WebhookParameter.validate_type` 用的是 header 与 query 允许集的并集，真正的窄约束在 `WebhookData` 的容器校验器里。

**源码事实：** `WebhookData.SyncMode.SYNC` 的值是 `"async"`，成员名与语义相反；默认配置里的 `async_mode` 为 `True`。

**源码事实：** schedule 节点的 `mode`/`frequency`/`cron_expression`/`timezone` 在节点执行期不被读取，仅由调度侧使用。

**源码事实：** plugin 节点额外输出 `TRIGGER_INFO` 元数据并在起始事件上挂 `provider_id`。

**架构推论：** 「触发器节点不做触发」这条分工是**正确的边界**，因为触发涉及进程外的东西（HTTP 服务器、调度器、插件守护进程），而节点是进程内的一次纯计算。把接收放进节点会让节点依赖服务器生命周期，图就没法离线复跑了。代价是**跨模块的前缀契约**：注入方与消费方必须约定同一套节点 ID 前缀规则，而这个约定不在任何类型里。

**架构推论：** 「变量池为空 → 空输出 + 成功」是这类节点唯一合理的选择，因为它们在测试/调试、以及非触发路径（如手动运行工作流）里也会被执行。但也正因如此，**触发器配置错位不会报错**——用户会看到一个跑完但一个字段都没有的工作流。这类静默的代价应至少配一条日志。

**架构推论：** `_webhook_raw` 的存在是「调试便利」对「输出契约」的一次让步。它把「节点输出 = 配置声明的参数」变成了「配置声明的参数 + 一份全量数据」。**任何绕过配置的输出都是潜在的敏感数据泄漏点**，尤其在多租户平台里。

**架构推论：** `SyncMode.SYNC = "async"` 这种命名反义，是**枚举成员名与持久化值分离**的典型事故。`StrEnum` 让成员名只在代码里出现，值写进数据库与 API，两者一旦分歧，读代码的人与读数据的人会得出相反结论。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/nodes/trigger_webhook/node.py` | `TriggerWebhookNode` | webhook 触发器，唯一做输出整形的触发器 |
| `api/core/workflow/nodes/trigger_webhook/node.py` | `TriggerWebhookNode._run` | 取前缀变量、抽取输出、并入系统变量 |
| `api/core/workflow/nodes/trigger_webhook/node.py` | `TriggerWebhookNode._extract_configured_outputs` | 按 headers/params/body 三类配置抽取 |
| `api/core/workflow/nodes/trigger_webhook/node.py` | `TriggerWebhookNode.generate_file_var` | 按传输方式把记录 ID 写到对应字段 |
| `api/core/workflow/nodes/trigger_webhook/entities.py` | `WebhookData` | 节点配置与三层类型白名单的容器层 |
| `api/core/workflow/nodes/trigger_webhook/entities.py` | `WebhookParameter` | header 与 query 共用，校验用并集 |
| `api/core/workflow/nodes/trigger_webhook/entities.py` | `WebhookBodyParameter` | body 参数，允许标量与四种数组加文件 |
| `api/core/workflow/nodes/trigger_webhook/entities.py` | `ContentType` | 五种内容类型，决定 body 的处理分支 |
| `api/core/workflow/nodes/trigger_schedule/trigger_schedule_node.py` | `TriggerScheduleNode` | 定时触发器，运行期不读调度配置 |
| `api/core/workflow/nodes/trigger_schedule/trigger_schedule_node.py` | `TriggerScheduleNode.get_default_config` | 可视化调度的画布初值 |
| `api/core/workflow/nodes/trigger_schedule/entities.py` | `TriggerScheduleNodeData` | 模式、频率、cron、时区配置 |
| `api/core/workflow/nodes/trigger_plugin/trigger_event_node.py` | `TriggerEventNode` | 插件事件触发器 |
| `api/core/workflow/nodes/trigger_plugin/trigger_event_node.py` | `TriggerEventNode.populate_start_event` | 把 provider_id 挂到起始事件 |
| `api/constants/trigger.py` | `TRIGGER_NODE_TYPES` | 三类触发器的类型名集合 |
| `api/constants/trigger.py` | `is_trigger_node_type` | 触发器类型判定 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：nodes/trigger_webhook/node.py](https://github.com/langgenius/dify/blob/main/api/core/workflow/nodes/trigger_webhook/node.py)
- [dify 文档：Webhook 触发器](https://docs.dify.ai/guides/workflow/trigger/webhook)
- [dify 文档：定时触发器](https://docs.dify.ai/guides/workflow/trigger/schedule)

## 与主核 / AI 的关系

**「入口节点不做接收」与主核里「连接管理器与执行器分离」同构。** 数据库的 postmaster 负责接受 TCP 连接、fork 出后端进程，执行器只处理已经建立好的会话。webhook 控制器负责解析 HTTP、注入变量池，节点只处理已就位的数据。**分工判据一致：涉及进程生命周期的部分不能进执行单元**，否则执行单元就没法被独立调度与重放。

**「前缀契约」与主核的命名空间约定同类。** 数据库里临时表、系统目录、用户对象靠 schema 前缀区分；这里靠节点 ID 前缀区分「哪个节点的输入」。两者都是**用字符串前缀替代结构化引用**，代价是约定不可校验、错了静默。数据库至少还有 `pg_namespace` 能查，这里完全没有。

**「空白输入 → 空输出成功」与主核对空结果的容忍度。** 数据库一条没有命中的 `SELECT` 返回零行而不报错，这是正确的；但「表不存在」必须报错。触发器节点的情形介于两者之间——它像「查了个空集合」，但用户可能以为配好了。**空结果与错配置在返回值上不可区分，是所有「宽松返回」设计的通病。**

**「原始数据被打码/改写」是审计场景的经典陷阱。** 主核的 WAL 记录的是逻辑变更而不是最终状态，就是为了让回放可核对；这里 `_webhook_raw` 声称是原始数据但已被就地修改。**任何声称「原始」的字段都必须真的只读**，否则依赖它做对账的人会得到错误结论。

对 agent 工程最实用的五条。其一：**触发器节点不含接收逻辑**，排查「webhook 没触发」要去 trigger controller，节点里什么都看不到。其二：**变量注入必须按节点 ID 前缀**，前缀写错会得到一个空跑成功的工作流，且没有报错。其三：**文本型 body 会把整段文本塞给每一个 body 参数**，配了多个参数时它们全是同一段内容。其四：**`_webhook_raw` 不是原始数据**，文件构造过程会就地改写它。其五：**`SyncMode.SYNC` 的值是 `"async"`**，读配置或读数据库时不要被成员名误导。
