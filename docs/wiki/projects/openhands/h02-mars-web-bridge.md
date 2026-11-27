---
project: openhands
feature: mars-web-bridge
title: Mars Web 桥接与 RPC
page_type: feature
schema_version: 1
refs:
  - path: src/api/mars/mars-web-bridge.ts
    symbols: [RpcEnvelope, rpc, createMarsWebBridge]
  - path: src/api/mars/mars-web-bridge.ts
    symbols: [MarsWebBridgeError, getMarsBridgeHeaders, probeMarsWebBridge]
---

# Mars Web 桥接与 RPC

## 一句话职责

在浏览器里造一个**与桌面版 `window.marsBridge` 形状完全相同的对象**：每个方法变成一次 `POST /mars/rpc/<method>`，参数放进 JSON；服务器拿着 DigitalOcean 令牌拨隧道，页面只拿到一个**同源、带路径前缀**的地址（`/mars/sessions/<id>`）——于是托管智能体的全部界面代码一行都不用改。

## 机制是什么

与 `h01` 的分工是本页的起点：

| | `h01`（隧道侧） | 本页（桥接侧） |
|---|---|---|
| 环境 | Electron 主进程 / Web 服务器 | 浏览器页面 |
| 内容 | 凭据存储、隧道拨通、会话生命周期、失败归因 | 把 `MarsBridge` 的每个方法转成一次 HTTP 调用 |
| 令牌 | **持有**（`safeStorage` / 钥匙串 / 加密文件） | **从不持有** |
| 产出 | 回环 `127.0.0.1:<port>` | 同源 `/mars/sessions/<id>` |

四个部件：

| 部件 | 作用 |
|---|---|
| `RpcEnvelope<T>` | 线格式：`{ result?, error?: { message?, status? } }` |
| `rpc<T>` | 一次调用的完整流程：POST → 解信封 → 按 HTTP 状态判失败 |
| `createMarsWebBridge` | 把 `MarsBridge` 的 18 个方法全部映射成 `call(method)` |
| `MarsWebBridgeError` | 带 `status` 的失败类型，让调用方能按状态分支 |

## 怎么实现的

**模块 docstring 把「为什么不直接打」讲完了。** 三段：

1. 桌面版从 Electron preload 拿到桥；**浏览器从 Agent Canvas 服务器拿到**，那个服务器托管同一个主进程桥，并在 `/mars/rpc/<method>` 下同源暴露它。
2. **这里每个方法就是一次携带调用参数的 POST，于是托管智能体界面的其余部分原样运行：它只跟 `MarsBridge` 接口说话。**
3. 连接解析成这个 origin 上的一个**带路径前缀**的后端（`/mars/sessions/<id>`），由服务器代理到它为这个会话持有的端口转发隧道。**DigitalOcean 令牌只在服务器上用于拨那条隧道，绝不到达页面。**

第 2 条是本页存在的全部理由：**接口一致，所以消费方无需分支**。这也解释了为什么 `createMarsWebBridge` 的实现几乎是机械的——18 个方法都写成 `call('methodName')`，唯一的例外是 `signInWithOAuth`。

**`signInWithOAuth` 是唯一手写的成员，且它直接抛。** 代码与注释：

```ts
signInWithOAuth: async () => {
  throw new MarsWebBridgeError(
    "OAuth sign-in is only available in the desktop app; use a personal access token.",
    400,
  );
},
```

注释说明了原因：**Web 服务器永不提供 OAuth（没有可信的回环重定向）；它返回的 auth state 如此说明，界面会因此隐藏那个按钮**。

这里有一个值得注意的双保险：**界面是通过 `authState.canUseOAuth` 隐藏按钮的**（`h01` 里那个字段的注释：`False when this build has no registered OAuth client; PAT still works`），而这个抛出只是兜底。两层都在，是因为「界面隐藏」依赖一次可能失败的 `getAuthState`，而直接调用的路径仍可能被走到。

而 `MarsWebBridgeError` 的 `status` 被填成 `400`——一个**合成的**状态码，因为这次失败根本没发生 HTTP 往返。

**`rpc` 的错误处理顺序是「先解析信封，再看 HTTP 状态」。** 三步：

1. `fetch`，body 是 `JSON.stringify({ args })`——注意**参数被包进一个 `args` 数组**，而不是展开。
2. **尝试解析 JSON** 进 `envelope`；`catch` 里什么都不做。注释：**一个非 JSON 的 body（边缘错误页）掉到状态行上**。
3. `if (!response.ok)` 抛 `MarsWebBridgeError(envelope.error?.message ?? '<status> <statusText>', envelope.error?.status ?? response.status)`。

第 2 步的空 `catch` 是本页最容易被误读的一处：它看起来像「吞掉错误」，实际上是**为第 3 步准备回退**。如果 body 不是 JSON（例如一个反向代理返回了 HTML 错误页），`envelope` 保持 `{}`，于是错误消息退到 `"502 Bad Gateway"` 这样的状态行。也就是说**解析失败不是错误处理失败，而是错误消息的降级路径**。

第 3 步的两个 `??` 都体现了同一个设计：**信封里的信息优先，HTTP 状态是兜底**。`error.status` 与 `response.status` 可以不同——服务器可以回 200 但信封里报一个业务错误（虽然 `!response.ok` 就不会进这个分支），也可以回 502 但信封里给一个更精确的 401。

**注意成功路径没有检查 `envelope.error`。** 也就是说**一个 HTTP 200 但信封里带 `error` 的响应当作成功**，`result` 是 `undefined`。这不是疏忽，而是协议约定：**失败必须用非 2xx 状态码表达**。把约定放在状态码上而不是信封里，让「成功/失败」只有一个判据。

**`getMarsBridgeHeaders` 的三级优先级是本页最重要的安全逻辑。** 它的 docstring 直接编号列出来了：

1. 有 **baked**（烘焙进构建的）会话密钥就用它（例如默认启动器）。
2. 否则退回**用户为这个 origin 在 API key 界面输入的密钥**（存在后端注册表里的 local backend 上）。
3. **绝不附加密钥**：若 `baseUrl` 指向另一个 host（外来后端），或存储的密钥属于另一个 host。

而实现的第一行就是这条排除：`if (baseUrl && !isServedOriginHost(baseUrl)) return {}`。

**第 2 级的「活跃后端优先」有一个具体理由。** 代码先试 `getEffectiveLocalBackend()`，若它持有服务 origin 的密钥就用它；否则才在已注册后端里找。注释写得很好：

> **优先用活跃后端当它持有服务 origin 的密钥，这样一个为同一 origin 保存的旧密钥不会遮住正在用的那个。**

也就是说同一个 origin 可能存了两个密钥（用户先输错过一次、后来又输对），而「哪个是正在用的」不能靠「哪个先注册」判断，必须靠「活跃后端当前带的是哪个」。这是一个把「当前状态」而非「历史顺序」当成真值源的正确做法。

**`holdsServedOriginKey` 的三条件判据里有一个容易被忽略的细节。** 它要求 `kind === 'local'` **且** `isServedOriginHost(host)` **且** `apiKey.trim().length > 0`。第三个条件用 `trim()` 而不是直接判空，说明**一个只含空白的密钥被当作没有密钥**——否则它会被发出去，服务器再以 401 拒绝，把一个「未配置」的问题变成一次认证失败。

**`probeMarsWebBridge` 的第二个检查是「`{ ok: true }` 而不是 200」。** 注释：

> **一个对每个路径都回 `index.html` 的服务器也是 200；只有桥会说 `{ ok: true }`。**

这条极其具体。任何 SPA 静态服务器（包括这个项目自己的 `scripts/static-server.mjs`）在收到未知路径时都会回 `index.html`（为了客户端路由），于是 `GET /mars/health` 会得到一个 200 和一个 HTML body。只看状态码，**每一个部署都会被误判成「托管了 MARS 桥」**，于是界面会显示一个点了就失败的按钮。

所以判据是「能解析出 JSON 且 `ok === true`」。这也解释了 `rpc` 里那个空 `catch` 的同源关切：**这个系统的部署形态里存在大量「200 但不是你以为的那个东西」**。

**探针的 1.5 秒超时是功能可用性判断的一部分。** `PROBE_TIMEOUT_MS = 1500`，用 `AbortController` + `setTimeout` 实现，`finally` 里清定时器。这个超时要短，因为它发生在**首次渲染之前**——`installMarsWebBridge` 的 docstring 说明了这条时序约束：**在 app 渲染之前解析完成，因为 `getMarsBridge()` 是同步读的**。

**`installMarsWebBridge` 的三个早退条件各有一类场景。** 代码：

```ts
if (typeof window === "undefined" || window.marsBridge) return false;
if (!(await probeMarsWebBridge())) return false;
window.marsBridge = createMarsWebBridge();
return true;
```

第一行的两个判断对应用两种环境：**没有 `window`（SSR 或测试）**、**已经有桥（Electron 的 preload 先装了）**。docstring 对这种情形说得很清楚：**在 Electron 里是 no-op（preload 已经提供了桥），服务器不提供 MARS 时也是 no-op，于是托管智能体界面像以前一样保持隐藏**。

第二行是「服务器没有桥」——这是最常见的情形（绝大多数部署不开 MARS）。返回 `false` 而不是抛错，让调用方不必 try/catch。

**「隐藏」是默认，不是失败。** 三处 docstring 与注释反复强调同一个结果：没有桥 → 界面隐藏。`h01` 的模块 docstring 甚至给了调用方的正确姿势：**对 `getMarsBridge()` 做门禁，而不是靠在每次调用上捕获失败**。也就是说「没有 MARS」是一种**产品状态**（这个部署不支持），而不是一种**异常**。

**`baseUrl` 的默认值是空串，语义是「页面自己的 origin」。** `createMarsWebBridge` 与 `probeMarsWebBridge` 的参数注释都写了：**Agent Canvas 服务器所在的 origin（与路径前缀）；空表示页面的自身 origin**。

允许传 `baseUrl` 说明这个桥可以被挂在一个子路径下（或者页面与服务器不同源但被显式配置），而默认的空串覆盖绝大多数情形。

**`fetchImpl` 也是参数，默认是 `(...params) => fetch(...params)`。** 这个包装看起来多余（为什么不直接给 `fetch`？），但它让 `fetch` 作为**全局值**在调用时才被读取，而不是在模块加载时绑定。这对测试注入与「某些环境下 fetch 后到」都必要。

## 关键取舍

**源码事实：** 走 HTTP RPC 而不是在页面上实现隧道，因为 harness-api 不发 CORS 头、隧道需要 Node 进程、令牌必须留在页面之外。三条各自都足以否掉「页面直连」。

**源码事实：** 18 个方法机械映射，只有 `signInWithOAuth` 手写并直接抛——因为服务器侧没有可信的回环重定向。界面另有一层 `canUseOAuth` 隐藏，抛出是兜底。

**源码事实：** 解析信封失败时静默，让错误消息降级到 HTTP 状态行；失败必须由非 2xx 表达，200 带 `error` 视为成功。

**源码事实：** 请求头有三级优先级，两级「不附加」：外来 host 不附加、存储密钥属于别的 host 不附加。活跃后端优先于注册顺序，避免旧密钥遮住正在用的那个。

**源码事实：** 空白密钥视同没有密钥，避免把「未配置」变成一次认证失败。

**源码事实：** 健康探测要求 `{ ok: true }` 而非 200，因为 SPA 静态服务器对未知路径会回 `index.html`。

**源码事实：** 探测限时 1.5 秒，因为它必须在首次渲染之前完成——`getMarsBridge()` 是同步读的。

**源码事实：** 没有桥是正常状态而非异常：三处入口都返回 `false` / 保持隐藏，调用方应门禁而不是捕获。

**架构推论：** 这一页的价值在于**用形状复制代替能力复制**。桌面版的桥有真隧道、真凭据、真 OAuth；Web 版的桥只有 HTTP 转发。两者能力不等，但**接口形状相同**，于是消费方只需写一次。代价是「不支持的能力」必须由数据说明（`canUseOAuth: false`）而不是由类型说明——一个 `MarsBridge` 的实现可以满足类型却永远抛错。

第二点值得记的是**这个仓库对自己部署形态的了解有多具体**：「SPA 会对未知路径回 index.html，所以 200 不算数」这类判断只能来自踩过的实际部署。它说明在这类多形态部署（Electron / dev 服务器 / 静态服务器 / Docker）的项目里，**「探测」比「假设」重要**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/mars/mars-web-bridge.ts` | `createMarsWebBridge` | 把 18 个 `MarsBridge` 方法映射成 HTTP RPC |
| `src/api/mars/mars-web-bridge.ts` | `rpc` | 单次调用：POST、解信封、按状态判失败 |
| `src/api/mars/mars-web-bridge.ts` | `RpcEnvelope` | `{ result?, error?: { message?, status? } }` 线格式 |
| `src/api/mars/mars-web-bridge.ts` | `MarsWebBridgeError` | 带 `status` 的失败类型，含合成的 400 |
| `src/api/mars/mars-web-bridge.ts` | `getMarsBridgeHeaders` | 三级优先级的会话密钥附加逻辑 |
| `src/api/mars/mars-web-bridge.ts` | `holdsServedOriginKey` | 「本地 + 服务 origin + 非空白密钥」三条件 |
| `src/api/mars/mars-web-bridge.ts` | `probeMarsWebBridge` | 1.5 秒健康探测，要求 `{ ok: true }` |
| `src/api/mars/mars-web-bridge.ts` | `installMarsWebBridge` | 首次渲染前安装 `window.marsBridge`，三处早退 |
| `src/api/mars/mars-web-bridge.ts` | `MARS_WEB_HEALTH_PATH` / `RPC_PREFIX` | 两条同源路由的前缀 |

## 参考

- [OpenHands 源码：src/api/mars/mars-web-bridge.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/mars/mars-web-bridge.ts)
- [OpenHands 源码：mars-tunnel-backend.ts（隧道侧）](https://github.com/OpenHands/OpenHands/blob/main/src/api/mars/mars-tunnel-backend.ts)
- [MDN：SameSite 与 HttpOnly Cookie](https://developer.mozilla.org/en-US/docs/Web/HTTP/Cookies)
- [OpenHands 文档：DigitalOcean 托管智能体](https://docs.openhands.dev/openhands/usage/cloud/digitalocean)

## 与主核 / AI 的关系

这一层的结构与主核里「客户端库把远端服务包装成本地对象」完全一致：驱动（driver）也是让调用方以为在操作本地句柄，实际每次调用都是一次网络往返。两者的共同难题是**「不支持的能力如何表达」**——数据库用能力位（`server_version_num`、`libpq` 的能力查询），这里用数据字段（`canUseOAuth`）。

对 agent 工程的直接价值有三条。其一，**接口形状一致比能力一致更重要**。桌面版有真隧道与真 OAuth，Web 版只有转发；但消费方只写一次代码，因为接口相同，不支持的能力由**返回值**说明。其二，**探测要能识别「假 200」**。SPA 静态服务器会回 `index.html`，把 200 当成功会让每个部署都误判成支持某个功能——判据必须是内容而不是状态码。其三，**「没有这个能力」是产品状态，不是错误**。让所有入口返回 `false`、界面保持隐藏，比让每个调用点 try/catch 一个「不支持」异常要干净得多。
