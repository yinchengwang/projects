---
project: ragflow
feature: mcp
title: MCP 服务端与连接器
page_type: feature
schema_version: 1
refs:
  - path: internal/mcp/connector.go
    symbols: [ServiceConnector, NewServiceConnector, Retrieval]
  - path: internal/mcp/http.go
    symbols: [Handler, NewHandler]
---

# MCP 服务端与连接器

## 一句话职责

把 RAGFlow 的三个能力（检索、列知识库、列对话）暴露成 MCP 工具，并同时提供两种传输（SSE 与 Streamable HTTP）：`ServiceConnector` 负责把工具调用接到进程内服务层，`NewHandler` 负责认证、会话归属与协议接线。

## 机制是什么

MCP（Model Context Protocol）是「让模型调用外部能力」的协议。RAGFlow 侧要做的只有两件事：

**一，把能力描述成工具。** 一个工具要告诉客户端「我叫什么、收什么参数、干什么用」。这三个工具的 schema 来自嵌入的 `tools.json`，而**工具描述的后半段是每个请求现拼的**——把当前调用者可见的知识库与对话列表拼进 `description`。这一点决定了服务端不能共享工具列表，必须逐请求生成（`CacheScope = "private"`）。

**二，把协议接进 HTTP。** MCP 有两种传输：老式的 SSE（客户端 GET 建流、再 POST 发消息）与新式的 Streamable HTTP（同一个端点既收又发）。两种都支持，可各自开关。

`Connector` 接口是这两件事之间的分界：工具层只认三个方法，不关心数据从哪来；`ServiceConnector` 是它的实现，用**函数字段**接入服务层，以避免 `mcp` 包直接 import `service`（否则会形成循环依赖或把整个服务层拖进这个薄包）。

## 怎么实现的

**`ServiceConnector` 的三个函数字段就是它的全部依赖。** `listDatasets`、`listChats`、`retrieval` 三个函数类型与原方法签名一致，`NewServiceConnector` 只是把它们存起来。注释明确解释了动机：抽象成函数参数，使本包不直接依赖服务层。**这是 Go 里绕开循环依赖的常规手法**——把依赖倒成函数值，而不是 import 接口。

**`ListDatasets` 有一个特殊的 `pageSize == -1` 分支。** 这个值表示「把全部页都取回来」：

- 从第 1 页开始循环，每页固定请求 100 条；
- 累加到 `data`，每轮检查三个退出条件：**本页为空**、**累计数已达 total**（且 `total > 0`）、**ctx 已取消**（取消返回错误而不是静默截断）；
- 否则继续下一页。

其他 `pageSize` 时只取一页，且 `min(pageSize, 100)` **把单页上限压在 100**。这是服务端强制的上限，客户端传更大值也不会拿到更多。

这个 `-1` 分支的存在是因为工具描述需要「全部可见知识库的名字」——描述里漏掉一个库，模型就永远不知道它存在。而分页是 REST 侧的既定契约，所以只能在这里循环补齐。

**`listText` 是「一行一个 JSON 对象」的极简序列化。** 每个条目只保留 `id`、`name`、`description` 三个字段，`json.Marshal` 后按换行拼接。对话（chats）允许没有 `description`，缺了就填空串；知识库则要求该字段存在（缺失时 `description` 会是 `nil`，Marshal 后成 `null`）。

这个格式的选择有实际后果：**它是给模型读的文本，不是给程序解析的 JSON**（整体不是合法 JSON 数组）。所以换行分隔、单行 JSON 是「既让模型容易读、又保留结构」的折中。

**`Retrieval` 是纯转发。** 一行 `return c.retrieval(ctx, req)`，把所有参数选择留给服务层。`RetrievalRequest` 结构体带 JSON tag，字段覆盖数据集 ID 列表、文档 ID 列表、问题、分页、相似度阈值、向量权重、top-k、重排模型与是否关键词检索——**它与 REST 侧的检索参数一一对应**，因为 MCP 工具复用的就是同一套服务。

**`NewHandler` 装配的是「认证壳 + 双传输 mux」。** 它先建一个**脱离请求生命周期**的 `lifetime` context（`context.WithCancel(context.Background())`），`Handler.Close` 就是取消它。然后：

- 建 Streamable HTTP handler：`Stateless: true`（不保留会话）、`JSONResponse` 可选、请求体上限 1 MiB、**`PropagateRequestCancellation: true`**（客户端断开时取消工具执行）。每次请求都用 `newServer(r.Context(), connector())` **新建一个 server 实例**——这与「工具描述逐请求生成」是同一件事的两面。
- 建 `mux`，按选项注册 `/mcp` 与 `/mcp/`（Streamable），以及 `GET /sse` + `POST /messages/`（SSE）。

**SSE 的会话登记有一个刻意的锁范围。** `GET /sse` 里：

1. 用 `rand.Text()` 生成会话 ID，构造 `SSEServerTransport`，端点写成 `/messages/?session_id=<id>`。
2. 从 ctx 取 `owner`（当前用户）。
3. 设置 `text/event-stream` 与 `no-cache` 头。
4. **持锁执行 `Connect`**。注释写明了理由：`Connect` 在返回前就会把端点发布出去，所以「客户端立刻发来的第一个 POST」必须等到会话已登记才能找到。若不持锁，存在一个「端点已可用但 map 里还没有」的窗口，那个 POST 会拿到 404。
5. 成功后写入 `sessions[id]`，放锁。
6. `defer` 里删除会话并 `session.Close()`；再用 `context.AfterFunc` 在请求 ctx 结束时关连接；最后 `session.Wait()` 阻塞到会话结束。

**`POST /messages/` 做两道检查。** 第一道是内容类型必须是 `application/json`（否则 415）。第二道是**会话归属**：先按 `session_id`（兼容 `sessionid`）查 map，再比较 `session.owner` 与当前用户 ID——不匹配统一返回 404 而不是 403。**用 404 而不是 403 是有意的：不泄露「这个会话存在但不属于你」。**

**认证壳 `protected` 是四段串联检查，顺序很重要。**

1. **localhost 保护先于认证。** 若请求到达的本地地址是回环（`http.LocalAddrContextKey`），则 `Host` 头必须是 `localhost` 或回环 IP，否则 403。注释说明这条对两种传输都生效——因为自定义 SSE 传输不经过 SDK 的 handler，SDK 自带的那层保护在这里会漏。这是防 DNS rebinding 的「Host 头」防护。
2. **拒绝跨源浏览器的访问。** 只要有 `Origin` 头，就必须「host 与 `r.Host` 相同」且 scheme 为 http/https，否则 403。这一条把「浏览器页面直接调本地 MCP」挡掉，命令行客户端因为不带 `Origin` 不受影响。
3. **把请求 ctx 与 handler 生命周期绑定。** `context.AfterFunc(lifetime, stop)` —— `Handler.Close()` 之后，所有在途请求被取消。
4. **认证并注入身份。** `resolve(ctx, Credential(r.Header))` 返回 userID；出错或空则 401。成功则 `permission.WithIdentity(ctx, userID, permission.TenantIDFromContext(ctx))` 写进 ctx，再交给内层 mux。

**`Credential` 只接受三种已知写法。** 优先 `Authorization: Bearer <token>`（大小写不敏感的前缀匹配，token 去空白后非空才收），否则依次尝试 `api_key`、`X-API-Key`、`Api-Key` 三个头。**不接受的 scheme 直接忽略而不是报错**——返回空串让上层统一判 401。

**工具注册与中间件是「每请求重建」的另一半。** `newServer` 从嵌入的 `toolsJSON` 反序列化三个工具定义，按固定下标（0 检索、1 列库、2 列对话）挂上 handler。中间件做三件事：

1. **给每个调用加 60 秒超时**，并与「请求级 context」（即 `newServer` 收到的那个 ctx）联动：`context.AfterFunc(requestContext, cancel)`，请求结束就取消。
2. **`tools/list` 逐请求拼描述**：取全部知识库（`ListDatasets(ctx, 1, -1, ...)`）与前 30 个对话（`ListChats(ctx, 1, 30, ...)`），把两份文本分别拼到工具描述后面（对话拼到第 3 个工具，其余拼第 1、2 个），并对每个工具做**浅拷贝**（`copy := *tool`）以避免污染共享的 `tools` 切片。结果设 `CacheScope = "private"`。
3. **`tools/call` 校验工具名**：不在三个已知名字里就返回一个 `IsError = true` 的文本结果，而不是返回协议错误。

**开源版的能力降级写在一个构建标签文件里。** `stub.go` 带 `//go:build !ee`，把企业版才有的 `CodexTickets` 与 `NewCodexHandler` 提供成「永远关闭 / 永远 404」的最小实现。注释直说了社区版没有 mode 8，所以类型不带状态、每个操作都拒绝。这使共享代码可以无条件调用它们，而 OSS 构建行为正确。

## 关键取舍

**源码事实：** 工具描述逐请求生成、`CacheScope` 设为 `private`、Streamable HTTP 用 `Stateless: true` 且每请求新建 server。**这三处是同一个决定的三面：不能跨请求复用任何含调用者可见资源的东西。**

**源码事实：** `ListDatasets(pageSize=-1)` 逐页拉全量（每页 100），正数 `pageSize` 被 `min(pageSize, 100)` 压到最多 100。

**源码事实：** SSE 会话在 `Connect` 期间持锁登记，理由是「端点先发布、会话后登记」的窗口会让快速客户端拿到 404。

**源码事实：** SSE 消息 POST 的归属不匹配返回 404 而非 403；`Credential` 只认 Bearer 与 `api_key` / `X-API-Key` / `Api-Key`，其他 scheme 静默忽略。

**源码事实：** localhost Host 头校验与 Origin 校验都在认证之前，且作用于两种传输——因为自定义 SSE 不经过 SDK handler。

**源码事实：** 未知工具名返回 `IsError` 文本结果而不是协议级错误；工具调用的超时是 60 秒。

**架构推论：** 「依赖倒成函数值」让 `mcp` 包保持薄：它不 import `service`，因此也不受服务层演化影响。代价是接线点在包外——`NewServiceConnector` 的三个函数必须由上层提供，参数一改就要同步改接线代码，编译器在这里帮不上忙。

**架构推论：** 两种传输并存且有各自的取舍。SSE 需要服务端保存会话（有状态），因此必须解决「会话归属」与「登记窗口」；Streamable HTTP 无状态，每请求独立，天然不需要会话表。**新式传输的「无状态」让一半的并发问题消失**，这也解释了为什么它是默认推荐。

**架构推论：** 把「工具列表」做成逐请求生成，意味着 `tools/list` 会**触发两次数据查询**（列库 + 列对话）。这是一个隐性成本：客户端刷新工具列表的频率越高，这两次查询越频繁。若不这么做，模型就看不到调用者自己的资源——**正确性与成本在这里是直接对立的**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/mcp/connector.go` | `ServiceConnector` | 三个函数字段构成的服务边界实现 |
| `internal/mcp/connector.go` | `NewServiceConnector` | 注入服务层函数，避免包间直接依赖 |
| `internal/mcp/connector.go` | `ServiceConnector.ListDatasets` | `-1` 全量分页；正数被压到上限 100 |
| `internal/mcp/connector.go` | `ServiceConnector.Retrieval` | 纯转发到服务层检索 |
| `internal/mcp/connector.go` | `listText` | 一行一个 JSON 对象的文本序列化 |
| `internal/mcp/server.go` | `Connector` | 工具层唯一依赖的三方法接口 |
| `internal/mcp/server.go` | `RetrievalRequest` | 与 REST 检索参数一一对应的请求体 |
| `internal/mcp/server.go` | `newServer` | 每请求新建 server，注册三工具与中间件 |
| `internal/mcp/server.go` | `tools/list` 中间件分支 | 逐请求拼描述并设 `CacheScope=private` |
| `internal/mcp/http.go` | `Handler` | 持有内层 handler 与取消函数 |
| `internal/mcp/http.go` | `NewHandler` | 认证壳 + 双传输注册 |
| `internal/mcp/http.go` | `Credential` | 三种受支持凭据头的提取 |
| `internal/mcp/http.go` | `GET /sse` | 持锁登记会话，owner 绑定 |
| `internal/mcp/http.go` | `POST /messages/`（`ServeHTTP` 路由） | 内容类型与会话归属双检 |
| `internal/mcp/stub.go` | `NewCodexHandler` | 社区版：路由存在但恒 404 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：mcp/http.go](https://github.com/infiniflow/ragflow/blob/main/internal/mcp/http.go)
- [MCP 规范：传输层](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
- [MCP 规范：工具](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

MCP 的「工具描述含调用者可见资源」与主核里「查询计划依赖调用者的权限」是同一件事：**同一条语句对不同用户有不同结果，所以不能缓存共享**。主核为此在计划缓存里把权限当成缓存键的一部分；MCP 更直接——干脆不缓存，每请求重建并把结果标成 `private`。

`ServiceConnector` 用函数值注入依赖，与主核里「存储引擎通过回调注入 I/O」同源：都是为了让上层不依赖具体实现。差别是主核的回调是稳定的低层原语（读页/写页），而这里的回调是三个业务方法——**业务接口比 I/O 原语更易变，所以这种解耦的代价更高**。

SSE 会话归属返回 404 而非 403，是安全工程里的标准做法（不泄露资源存在性），与主核在权限不足时对某些对象返回「不存在」而非「禁止访问」出于同一考虑。

对 agent 工程的实际含义有四条。其一，`tools/list` 会打两次数据库查询，客户端不该高频刷新工具列表。其二，SSE 传输是有状态的，必须粘性路由；Streamable HTTP 无状态，可以在任意实例间负载均衡——**多副本部署该关掉 SSE**。其三，MCP 的凭据只认 Bearer 与三个 API key 头，用其他自定义 scheme 会得到 401 且没有提示。其四，工具调用的 60 秒超时是硬编码的，长检索任务需要拆分或在客户端侧容忍超时。
