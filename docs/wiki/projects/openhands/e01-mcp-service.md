---
project: openhands
feature: mcp-service
title: MCP 服务器配置与测试
page_type: feature
schema_version: 1
refs:
  - path: src/api/mcp-service/mcp-service.api.ts
    symbols: [McpService, toMcpServer, buildMcpTestRequest]
  - path: src/api/mcp-service/mcp-service.api.ts
    symbols: [redactMcpTestResponse, finalizeMcpTestResponse, getMcpTestTimeout]
---

# MCP 服务器配置与测试

## 一句话职责

`McpService` 是 MCP 设置的执行层：把界面上的 `MCPServerConfig` 翻译成后端能接受的请求形状、按后端与认证方式决定超时、发起「测试连接」或「OAuth 授权」、再把回来的结果做**出口脱敏与失败归类**。

## 机制是什么

这个模块的复杂度几乎全部来自「同一次测试请求有四种可能的走法」：

| 后端 | 服务器类型 | 走法 |
|---|---|---|
| 本地 agent-server | stdio / sse / http | `MCPClient.testServer`，凭据回填后直接探测 |
| 云端 | sse / http | app server 的 `POST /api/v1/mcp/test`，凭据由服务端按 `name` 恢复 |
| 云端 | stdio | **短路为合成成功**，不探测 |
| 任一 | OAuth | 独立的「启动 job + 轮询状态」流程，带浏览器弹窗 |

四种走法各有各的坑，而代码把这些差异压在同一组辅助函数上——`toMcpServer`、`buildMcpTestRequest`、`getMcpTestTimeout`、`finalizeMcpTestResponse`——让四个入口共享同一套翻译与收尾逻辑。

## 怎么实现的

**`toMcpServer` 是形状翻译器，核心手法是「空则省略」。**

```ts
if (server.type === "stdio") {
  return {
    type: "stdio",
    command: server.command!,
    ...(server.args?.length && { args: server.args }),
    ...(server.env && Object.keys(server.env).length > 0 && { env: server.env }),
  };
}
return {
  type: server.type === "sse" ? "sse" : "http",
  url: server.url!,
  ...(server.headers && Object.keys(server.headers).length > 0 && { headers: server.headers }),
  ...(server.auth ? { auth: server.auth } : {}),
};
```

三处值得看：

- **`server.type === "sse" ? "sse" : "http"`**：界面上的类型是 `stdio | sse | shttp`，而线上只认 `stdio | sse | http`。`shttp`（streamable HTTP）在线上叫 `http`。这一处映射是整个函数里唯一真正「翻译」而非转抄的地方，也是名字差异最容易被漏掉的地方。
- **空数组与空对象要省略而不是传空**：`server.args?.length && ...` 与 `Object.keys(server.env).length > 0 && ...` 两处都在检查非空。传 `args: []` 或 `env: {}` 与不传在语义上未必等价——服务端的 schema 可能对两者区别对待（例如把「显式空 env」理解为「清空继承的环境」）。省略是更安全的表达。
- **`command!` 与 `url!` 的非空断言**：这说明类型层面的 `MCPServerConfig` 允许这两个字段缺席（因为它是三种传输的联合），但编码上这里假定「到这一步已经校验过」。断言即「信任上游校验」，也是这一页唯一放下运行期守卫的地方——而它恰好落在最不该出事的位置（发给后端的必填字段）。

**`getMcpTestTimeout` 只有两行，但包含一条业务规则。**

```ts
function getMcpTestTimeout(server: MCPServerConfig): number | undefined {
  if (server.auth?.strategy !== "oauth2") return server.timeout;
  return OAUTH_MCP_TEST_TIMEOUT_SECONDS;
}
```

规则是：**非 OAuth 用用户配置的超时，OAuth 一律用固定的 120 秒**。

为什么 OAuth 要覆盖用户配置？因为 OAuth 探测的耗时完全不由「网络快慢」决定——它要等真人在浏览器里完成登录授权。用户配的 10 秒超时会让每一次 OAuth 测试都必然失败。所以这里把「用户意图」让位给「流程本身的时长下限」。

这条规则还带出一个全局一致性：`OAUTH_MCP_TEST_TIMEOUT_SECONDS = 120` 与 `MAX_CLOUD_MCP_TEST_TIMEOUT_SECONDS = 120` 是同一个数值但两个常量。注释分别写了来由——前者是「OAuth 探测的预算」，后者是「app server 的 `POST /api/v1/mcp/test` 能接受的**上限**」。分开定义是对的：它们的理由不同，将来若云端上限变了，OAuth 预算不该跟着变。

云端分支还用 `Math.min(timeout, MAX_CLOUD_MCP_TEST_TIMEOUT_SECONDS)` 夹了一次（在 `testRemoteServerViaCloud` 里）。这是一道**本地不可能发现的失败**：超过上限的请求会被服务端直接拒，而前端若不夹，用户会看到一个与超时无关的报错。

**`buildMcpTestRequest` 把「翻译 + 回填 + 校验 + 超时」四件事收在一处。** 它是本地路径的请求构造器：

```ts
const validation = getCredentialValidationForServer(server);
const substituted = await substituteRedactedMcpCredentials(server);
const serverSpec = toMcpServer(substituted);
const timeout = getMcpTestTimeout(server);
return {
  request: {
    server: serverSpec,
    ...(server.name ? { name: server.name } : {}),
    ...(timeout !== undefined ? { timeout } : {}),
    ...(validation ? { tool_call: validation.toolCall } : {}),
  },
  substituted,
};
```

四处细节：

1. **顺序是先回填再翻译**：`toMcpServer(substituted)`。这很关键——若先翻译后回填，回填逻辑就得再理解一遍线上形状。e02 已经讲清了 `substituteRedactedMcpCredentials` 的作用；这里要记的是它**在形状翻译之前**发生。
2. **返回 `substituted` 而不只是 `request`**：调用方拿它做出口脱敏的第二个来源。注释在 `testServer` 里说明了这一点：「Redact against both config forms: `server` may hold fresh plaintext input, `substituted` the stored（encrypted）round-trip values」。
3. **`tool_call` 来自 `validation`**：即「这个服务器在目录里有对应的凭据验证配方」时，探测请求要**额外调用一个只读工具**。这让探测能发现「连上了但凭据不对」——单纯连上不代表认证有效。
4. **`timeout` 用 `!== undefined` 判断而不是 `timeout && ...`**：`0` 是合法的超时值吗？在 `MCPServerConfig` 里大概不是，但用 `!== undefined` 保证了「显式 0」也会被传出去而不是被 falsy 判断吞掉。这是与 `toMcpServer` 里空值省略相反的一个选择——因为 `timeout: 0` 与「不传 timeout」在语义上确实不同。

**两个脱敏函数分工明确。** `redactMcpTestResponse` 只处理文本：

```ts
if (!result.ok) return { ...result, error: redactMcpSecrets(result.error, ...redactionSources) };
if (result.tool_result) return { ...result, tool_result: { ...result.tool_result, text: redactMcpSecrets(result.tool_result.text, ...redactionSources) } };
return result;
```

它覆盖两个出口：失败时的 `error` 与成功时的 `tool_result.text`。两者都是「服务端原文回流到界面」的通道。注意它**不改 `tools` 列表里的名字**——工具名是元数据，不是用户数据。

`finalizeMcpTestResponse` 在脱敏之上再叠一层**语义归类**：

```ts
const redacted = redactMcpTestResponse(result, redactionSources);
if (redacted.ok && validation && redacted.tool_result &&
    redacted.tools.includes(validation.toolCall.name)) {
  const credentialError = validation.interpret(redacted.tool_result);
  if (credentialError) return { ok: false, error: credentialError, error_kind: "credentials" };
}
return redacted;
```

关键在那第三个合取条件：**探测工具必须真的出现在 `tools` 里**才算数。docstring 把这条的理由写得很清楚：

> The interpretation only runs when the probe tool is actually advertised in `tools` — a server variant that doesn't expose the tool（e.g. a hosted alternative to the stdio server the spec was written for）must degrade to a connectivity-only success, not be misreported as bad credentials.

即同样的目录条目可能对应两种服务器：本地 stdio 版（有那个只读工具）与托管版（没有）。若无条件跑 `interpret`，托管版会被判成「凭据错误」——而它其实只是没有那个工具。**先确认工具存在，再解释工具的结果**，这个顺序避免了把「不存在」误读为「失败」。

另一个细节是 `redacted.tool_result` 被用于 `interpret` 而不是原始 `result.tool_result`——但 `interpret` 只看 `is_error` 之类的结构性字段，脱敏后的文本对它没有影响。这里用脱敏版是「保持一致」的选择，不是必需。

**云端 stdio 短路是这段代码里最反直觉的一处，而注释把理由写全了。**

```ts
if (server.type === "stdio") {
  return { ok: true, tools: [] };
}
```

> A stdio server spawns inside the cloud sandbox, which isn't reachable from the browser before the user starts a conversation, so it cannot be probed from the settings page. Short-circuit with a synthetic success so saving/installing proceeds; any real failure surfaces inside the conversation runtime instead.（Throwing here would block the install flow entirely — see the cloud regression test.）

也就是说这是一个**刻意的谎**：报告「成功、零工具」，而不是报告「无法探测」。理由有三层：

1. 物理上不可能——stdio 服务只在沙箱里存在，浏览器在下一次会话启动前够不到它。
2. 若抛错，安装流程被完全阻断——用户永远装不上云端 stdio 服务器。
3. 真实失败会在会话运行时暴露——不是不报，是延迟报。

代价是「测试连接」按钮在云端 stdio 上会给出一个假的绿灯。这是**可用性与诚实性之间的取舍**，而代码选择了可用性并把代价写在注释里。这类决定的正确性完全取决于「用户是否知道这次测试没做事」——注释的存在说明作者清楚这一点。

注意这个合成成功连 `tool_call` 都不带——`tools: []` 会让 `finalizeMcpTestResponse` 里的 `redacted.tools.includes(validation.toolCall.name)` 判为假，于是验证逻辑被跳过，结果就是最朴素的「连接成功」。这条链是自洽的。

**云端远程探测自己重写一遍请求，而不是复用 `buildMcpTestRequest`。** `testRemoteServerViaCloud` 里：

```ts
const authHeaders = server.auth ? headersFromMcpAuth({ ...server.auth }) : null;
const headers = { ...server.headers, ...authHeaders };
const name = server.id || server.name;
```

docstring 解释了为什么不能复用：

> Unchanged（redacted）credentials are not substituted here: the app server restores them from the stored server of the same settings key, which is why the key is sent as `name`。`auth` is flattened to headers the same way cloud saves persist it so that restoration matches; an `auth` that cannot be flattened（e.g. OAuth without tokens）is passed through and the app server answers with a structured failure.

三件事：

1. **云端不做前端回填**——回填由服务端做，它按 `name`（实际是 `server.id || server.name`）找到已存的那条。这把 e02 讲的替换通道限定在本地路径上。
2. **`auth` 被拍平成 headers，且必须与云端保存时的拍平方式一致**——否则服务端按保存时的形状找不回原值。注释里「the same way cloud saves persist it so that restoration matches」这句是全段的核心：脱敏回填靠「同形」匹配。
3. **拍平失败就原样传 `auth`**——`...(server.auth && authHeaders === null && { auth: server.auth })`。即「我拍不平，交给你判」，服务端会给出结构化失败而不是错误地按 headers 恢复。

注意 `headers` 的展开顺序：`{ ...server.headers, ...authHeaders }`。auth 拍平出的头**覆盖**同名的显式头——这是有意的，因为 auth 才是权威来源。

`testServer` 的本地分支还有一处收尾值得记：

```ts
} finally {
  client.close();
}
```

`MCPClient` 是每调用一次就新建、用完立刻关。这与 `authorizeOAuth` 里的 `transport.close()` 是同一纪律——探测客户端的生命周期严格限定在一次调用内，不留池化。

**OAuth 流程是另一条完全独立的路径，最值得看的是弹窗的生命周期管理。** `authorizeOAuth` 开头：

```ts
let popup: Window | null = window.open("about:blank", "_blank");
```

注释：「Opened synchronously inside the click handler so popup blockers allow it; every failure path below closes it again.」这是一条浏览器端的硬约束：`window.open` 只有**在处理用户手势的同步栈里**才不会被拦截。而 OAuth 授权 URL 要等 `transport.start` 返回才知道，那已经是 `await` 之后了——所以只能先开一个 `about:blank` 的占位窗，拿到 URL 再改它的 `location`。

于是整个流程的每一步都必须考虑「这个弹窗现在该怎么办」：

- `start` 失败或没有 `job_id` → `popup?.close()` 并返回失败。
- **`start` 成功但没有 `authorization_url`** → 也关掉窗口，并置 `popup = null`。注释说明了这个分支：探测已在存储的令牌上完成（仍有效或已刷新），不需要用户同意。**这是整个流程里最容易被忽略的一支**：OAuth 的「不需要授权」是一种成功，不能留下一个空白窗口。
- 轮询到终态 → `popup?.close()` 再返回。
- 循环末尾若无终态且仍有 `authorization_url` → 设置弹窗的 `location.href` 让用户去授权。

注意那两段轮询的差异：先是 20 次 × 250 毫秒的快轮询，条件是 `!status.callback_ready` 才 `break`；然后才是 120 次 × 1 秒的慢轮询。快轮询的意义是**在跳转之前确认服务端是否已经就绪**——`callback_ready` 表示服务端已经把回调用路准备好，此时才可以安全地把用户送到授权页；否则用户可能完成授权而回调还没就绪。这是「先把接收端准备好，再让用户去授权」的正确顺序，而它被编码成两个节奏不同的循环。

失败时的文案区分也很有信息量：

```ts
if (start.authorization_url) return { ok: false, error: "OAuth authorization was not completed", error_kind: "authorization" };
return { ok: false, error: "OAuth authorization timed out", error_kind: "timeout" };
```

即**有授权 URL = 人没完成登录**（`authorization`）；**没有 = 服务端在存储令牌上探测超时**（`timeout`）。同一段代码的两种超时，含义完全不同，错误类型也不同。

`oauthStatusToTestResponse` 里还有一处反过来的判定：

```ts
if (status.error_kind === "timeout" && status.authorization_url) {
  return { ok: false, error: status.error || "OAuth authorization was not completed", error_kind: "authorization" };
}
```

注释解释了：后端的探测有自己的预算，若它已经下发过授权 URL，那这个预算耗在**等人登录**上，而不是等服务器。

**`createOAuthTransport` 是本地与云端的统一接口。** `McpOAuthTransport`（`start` / `status` / `close`）把两条传输的差异收在一个小协议上：

- 云端：`start` 走 `startCloudMcpOAuth`，`status` 走 `getCloudMcpOAuthStatus`，`close` 是空函数（云端没有客户端要关）。
- 本地：`start` / `status` 走 `MCPClient` 的 OAuth 接口，`close` 关客户端。

云端的 `start` 里有一句注释值得记：`auth` 保持 `oauth2` 形态而不是拍平成 headers，理由是「so the app server runs the OAuth flow rather than flattening it to headers」。这与前面 `testRemoteServerViaCloud` 刻意拍平形成对照——**普通探测要拍平（为了与保存时的形状匹配），OAuth 探测不能拍平（否则服务端会当成静态 auth，不会走 OAuth 流程）**。同一个字段在两条流程上要求相反的形状，这个差异只能靠注释传递。

**`getMcpProbeOptions` 与 `getMcpOAuthCallbackBaseUrl` 解决的是「浏览器够不够得到」。** OAuth 探测需要一个**本地** agent-server，因为流程里要发 IPC 与回调。所以云端后端下，它会去注册过的后端列表里找一个可用的本地后端，找不到就抛「OAuth authorization requires a reachable local backend」。

`getMcpOAuthCallbackBaseUrl` 的注释把问题说得很清楚：agent-server 会把 OAuth 供应商的重定向发到自己名下的一个路由，「without it, the redirect goes to a loopback port on the agent-server host, which a browser outside that container or machine cannot reach」。所以在容器化或远程部署下，必须把「浏览器视角的基址」显式传过去，否则用户登录完成后会跳到一个他访问不到的地址。

实现上它用 `new URL(host || "/", window.location.href)` 解析——即以当前页面为基准解析相对地址。最后 `replace(/\/+$/, "")` 去掉尾部斜杠，因为后面的拼接假定没有尾斜杠。

**`createMcpProbeClient` 的超时比 OAuth 预算多 5 秒**：`OAUTH_MCP_TEST_TIMEOUT_SECONDS * 1000 + 5000`。这 5 秒是给「轮询最后一次状态」留的余量——服务端的探测预算用满后，客户端还要再问一次结果。若两者相等，最后一次状态查询会与客户端的超时同时到达，结果不可预测。

## 关键取舍

**源码事实：** `toMcpServer` 把界面的 `shttp` 映射成线上的 `http`，并在 `args` / `env` / `headers` 为空时省略字段而非传空值。

**源码事实：** OAuth 探测无视用户配置的超时，固定用 120 秒，因为耗时由真人操作决定；云端测试还要额外用 `Math.min` 夹到 app server 的 120 秒上限。

**源码事实：** `buildMcpTestRequest` 先回填凭据再翻译形状，并把回填后的配置一并返回，供出口脱敏当第二个来源。

**源码事实：** `finalizeMcpTestResponse` 只在探测工具**确实出现在 `tools` 列表**时才跑凭据语义解释，避免把「托管版没有该工具」误判为「凭据错误」。

**源码事实：** 云端 stdio 服务器短路为合成成功 `{ok: true, tools: []}`，理由是浏览器在会话启动前够不到沙箱，且报错会彻底阻断安装流程。

**源码事实：** 云端远程探测不做前端回填（由服务端按 `name` 恢复），且 `auth` 必须拍平成 headers、与云端保存时的形状一致，回填才匹配；拍不平就原样传 `auth` 让服务端结构化失败。

**源码事实：** OAuth 的弹窗必须在点击的同步栈里 `window.open("about:blank")`，之后才改 `location`；且「没有 authorization_url」也是一种成功，此时必须关掉弹窗。

**源码事实：** OAuth 有两段节奏不同的轮询：先 20 次 × 250 毫秒等 `callback_ready`，再 120 次 × 1 秒等终态——先在服务端就绪再让用户去授权。

**源码事实：** 超时文案按「有没有下发过授权 URL」区分 `authorization` 与 `timeout`；同一个 `error_kind: "timeout"` 在带 URL 时被重写为 `authorization`。

**架构推论：** 这个模块是「一个概念、多种部署」的典型代价清单。同一个「测试连接」在四种组合下有四套形状要求：本地要回填凭据、云端要交名字让服务端回填、云端 stdio 根本不能测、OAuth 不能拍平 auth。把这些差异都放在同一组辅助函数里，至少保证了「形状翻译」与「出口脱敏」是共享的；但差异本身无法消除，只能靠注释逐条交代。

**架构推论：** 「探测工具必须出现在 tools 里才解释结果」这条，是**避免把缺失当失败**的通用模式。系统里凡是「我有针对某能力的判断逻辑」的地方，都要先确认那个能力真的存在，再判断它的结果。否则代理、变体、降级实现都会被误判。

**架构推论：** 云端 stdio 的合成成功是「谎报以保可用」的极端案例，值得作为反例记住：它把不可测伪装成可测且通过。留下注释是对的，但更彻底的做法是把结果标成第三种状态（例如 `unverified`），让界面显示「云端无法预检」而不是绿灯。这份实现选择的是前者——因为下游的 `ExtendedMCPTestResponse` 只有 `ok` 二值。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/mcp-service/mcp-service.api.ts` | `McpService` | 测试、云端测试、OAuth 授权、传输选择的统一入口 |
| `src/api/mcp-service/mcp-service.api.ts` | `toMcpServer` | 界面形状 → 线上形状，`shttp`→`http`，空字段省略 |
| `src/api/mcp-service/mcp-service.api.ts` | `buildMcpTestRequest` | 回填 → 翻译 → 超时 → `tool_call`，返回请求与回填后配置 |
| `src/api/mcp-service/mcp-service.api.ts` | `getMcpTestTimeout` | OAuth 固定 120 秒，其余用用户配置 |
| `src/api/mcp-service/mcp-service.api.ts` | `redactMcpTestResponse` | 出口脱敏，覆盖 `error` 与 `tool_result.text` |
| `src/api/mcp-service/mcp-service.api.ts` | `finalizeMcpTestResponse` | 脱敏后按凭据配方解释，仅在工具存在时生效 |
| `src/api/mcp-service/mcp-service.api.ts` | `McpService.testServer` | 三路分派：云端 stdio 短路、云端远程、本地 |
| `src/api/mcp-service/mcp-service.api.ts` | `testRemoteServerViaCloud` | 云端远程探测，auth 拍平与 `name` 传递 |
| `src/api/mcp-service/mcp-service.api.ts` | `McpService.authorizeOAuth` | 弹窗生命周期、两段轮询、失败分类 |
| `src/api/mcp-service/mcp-service.api.ts` | `oauthStatusToTestResponse` | 状态 → 测试结果的映射，含超时重写 |
| `src/api/mcp-service/mcp-service.api.ts` | `createOAuthTransport` | 云端/本地 OAuth 传输的统一协议 |
| `src/api/mcp-service/mcp-service.api.ts` | `getMcpProbeOptions` / `getMcpOAuthCallbackBaseUrl` | 找可达的本地后端；给浏览器视角的回调基址 |

## 参考

- [OpenHands 源码：mcp-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/mcp-service/mcp-service.api.ts)
- [MCP 规范：授权](https://modelcontextprotocol.io/specification/2025-06-18/basic/authorization)
- [OpenHands 文档：MCP](https://docs.openhands.dev/openhands/usage/mcp)
- [MDN：window.open 与弹窗拦截](https://developer.mozilla.org/en-US/docs/Web/API/Window/open)

## 与主核 / AI 的关系

「同一能力在多种部署组合下形状不同」与主核里的**连接参数协商**同构：主核连本地 socket 与连远程 TCP 时，认证方式、超时来源、错误分类都不同，而上层查询接口必须一致。这份实现的处理手法值得对照——它把差异收敛在少数几个辅助函数里（形状翻译、超时选择、出口脱敏），让四个入口共享；主核通常用「连接字符串 + 参数覆盖」做同一件事。

「先确认能力存在，再解释结果」这条与主核里的**能力探测**同理：在 PostgreSQL 里用某个函数前先查 `pg_proc`，在新版本特性上先查 `server_version`。若跳过探测直接调用，旧版本会把「不支持」表现为语法错误，而调用方会误判成「执行失败」。这里的 `redacted.tools.includes(validation.toolCall.name)` 就是同一种探测。

OAuth 弹窗那段与主核的**两阶段提交**有一个共同点：都在「外部参与者必须就位」之后才继续。这里先在服务端等 `callback_ready` 再把用户送去授权，正如主核先在协调者落盘 prepare 再让参与者提交。顺序错了，外部参与者的成果会因为接收端没准备好而丢失——在这里表现为用户登录完成后回不来，在白屏上卡住。

对 AI 工程的实际意义在于：MCP 是 agent 获取外部能力的主渠道，而 MCP 的配置界面是用户与「不可控外部服务」之间唯一的缓冲层。这份实现给出的三条经验可直接用：**OAuth 耗时由真人决定，所以超时不能由用户配置**；**出口脱敏与入站回填必须成对存在**（e02 讲回填，这里讲出口）；以及**把认证形状的一致性写进代码**（云端拍平 auth 是为了能与保存时匹配，OAuth 不拍平是为了让服务端走 OAuth 流程而不是当成静态头）。最后一条最容易漏——同一个 `auth` 字段在两条流程上要求相反的形状，只能靠注释把理由钉住。
