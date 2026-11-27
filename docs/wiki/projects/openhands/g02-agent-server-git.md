---
project: openhands
feature: agent-server-git
title: 运行时侧 Git 与路径换算
page_type: feature
schema_version: 1
refs:
  - path: src/api/git-service/agent-server-git-service.api.ts
    symbols: [AgentServerGitChange, AgentServerGitCommit, toAbsoluteRuntimePath]
  - path: src/api/git-service/agent-server-git-service.api.ts
    symbols: [getFromRuntime, isEndpointMissingError, AgentServerGitService]
---

# 运行时侧 Git 与路径换算

## 一句话职责

查一个**正在运行的沙箱**里的改动、提交与差异：本地模式下直接用 SDK 的类型化方法打运行时，云端模式额外走 App API 的专用代理；而这两种路径共用一个必须做的事——**把界面传来的相对路径换算成运行时要求的绝对路径**，否则云端会因为重复加前缀而 404。

## 机制是什么

| 部件 | 作用 |
|---|---|
| `AgentServerGitChange` / `AgentServerGitCommit` / `AgentServerGitCommitsPage` | 运行时的蛇形命名线格式（`short_sha`、`has_more`） |
| `toAbsoluteRuntimePath` | 相对路径 → 绝对路径的唯一换算点 |
| `getFromRuntime` | 通用 GET，直打运行时并用 `HttpClient` 保留 `.status` |
| `isEndpointMissingError` | 404 判定，`null` 而不是异常 |
| `AgentServerGitService` | 四个方法：改动、提交列表、单提交改动、差异 |

与 `g01` 的分工是清楚的：那一套面向**云后端的仓库与分支发现**（回答「用户能开哪些仓库」），这一套面向**已经跑起来的会话**（回答「这个沙箱里改了什么」）。两者共用 `getActiveBackend()` 但对接的接口完全不同。

## 怎么实现的

**路径换算是一段真实事故的记录。** `toAbsoluteRuntimePath` 的注释写得很具体：

> 云运行时的 `/api/git/{changes,diff}` 端点会给相对路径**前置** `/workspace/`（于是一个像 `workspace/project` 的相对参数会变成 `/workspace/workspace/project` 并 404）。`getGitPath` 默认返回本地 agent-server 的相对约定；在发给运行时之前归一成绝对路径。

也就是说：**本地运行时接受相对路径（且自己不动它），云运行时接受相对路径但会自作主张加前缀**。同一个 API 表面，两种行为。于是唯一的解法是在客户端就把相对路径变成绝对路径——加一个前导 `/` 就够，因为它本身已经以 `workspace/...` 开头。

**`toAbsoluteRuntimePath` 是幂等的，这是它能在五处调用的原因。** `path.startsWith("/") ? path : '/' + path`。云路径（走代理）与本地路径（走 SDK）都调它，而调用两次也不会变成 `//workspace/...`。这一点很重要：`getFromRuntime` 内部也调它，而几个调用方在调 `getFromRuntime` 之前已经调过。

**`getFromRuntime` 存在的原因是「类型化包装器挡住了两个需要的东西」。** 它的注释说明了两个具体缺口：

1. **类型化的 `gitChanges`/`gitDiff` 包装器不知道较新的端点与参数**——所以要走原始 `client.get`。
2. **它们会重新包装错误，丢掉 `.status`**——而 `404` 状态正是「这个服务器太旧、没有 commits 端点」这个特性探测的依据。

第 2 条是本页最值得记的一条：**错误被重新包装后，特性探测就失效了**。这不是「错误处理不好」，而是「抽象层吞掉了调用方需要的判据」。

**`isEndpointMissingError` 认两种错误形态，注释说了为什么。** 它同时检查 `isSdkHttpStatusError(error, 404)` 与 `(isAxiosError(error) && error.response?.status === 404)`。注释：

> 提交端点是较新的，比一些已部署的 agent server 新。一个 404（路由不存在）意味着「服务器太旧」——调用方**隐藏提交区域**而不是露出一条错误。本地调用与云代理调用（经共享的 TypeScript 客户端）**都抛 SDK 的 `HttpError`**；axios 检查被保留作为 axios 形状错误的**安全网**。

「隐藏而不是报错」是一个正确的产品判断：一个可选的面板功能，在旧服务器上应该消失，而不是让整个会话页面报错。

**四个方法的路径选择不同，这个差异写在类注释里。** 类注释说明了两件事实：本地模式下运行时就在 `127.0.0.1:18000`、浏览器能直连；云端模式下运行时在 `*.prod-runtime.all-hands.dev`，**其 CORS 允许列表（`OH_ALLOW_CORS_ORIGINS`，在 saas-deploy 里设成 Canvas 的 origin）允许浏览器直连**，并用会话的 `X-Session-API-Key` 认证。

然后关键的一句：**改动与差异的列表端点另外还有专用的云 App-API 代理（`GET /api/v1/app-conversations/{id}/git/{changes,diff}`），由服务端自己解析；下面的提交端点则直打运行时**。

于是在同一套方法里，**两条路径的选择规则不一样**：

| 方法 | 云端走哪 | 本地走哪 |
|---|---|---|
| `getGitChanges` | App API 代理 | SDK `gitChanges` |
| `getGitChangeDiff`（无 commit） | App API 代理 | SDK `gitDiff` |
| `getGitCommits` | 直打运行时 `getFromRuntime` | 同 |
| `getCommitChanges` | 直打运行时 `getFromRuntime` | 同 |
| `getGitChangeDiff`（带 commit） | 直打运行时 `getFromRuntime` | 同 |

这个分法有一个解释得通的逻辑：**改动与差异的「当前」版本有云代理（因为它需要服务端解析会话 → 运行时），而提交相关的较新端点没有代理，只能直打**。而带 `commit` 的差异属于提交族，所以走直打。

**两处注释留下了同一个坑的记录：「不要传 `ref`」。** `getGitChanges` 里有：

> 不传 `ref`：让服务器自己判基线（origin branch / merge-base），于是 agent **已经提交过的**改动仍然显示。`ref: "HEAD"` 会在每次 `git commit` 之后变空——而且上面的云代理分支本来就不传 `ref`。

`getGitChangeDiff` 重复了这条：**因为基线必须与改动列表用的是同一个**。

这是这一页最重要的功能细节：改动列表与差异都基于「origin branch / merge-base」而不是 HEAD，否则 agent 一提交，界面上的改动就全部消失。而两处必须用同一个基线，否则「列表点进去看到的差异」与「列表里的状态」会不一致。

**`getGitChangeDiff` 有两条几乎重复的分支，差异只在参数。** 带 `commit` 时调 `getFromRuntime` 并额外传 `commit`，注释解释了它为什么不能走代理：**两侧都来自服务器上的 git 对象，于是这次提交删掉的文件仍能渲染**。无 `commit` 时走云代理或 SDK。

「删掉的文件仍能渲染」这句说明了差异数据的来源：`modified` 与 `original` 两份内容都是从 git 对象里取出来的。而工作区改动（未提交）的「原文件」可能是从索引或基线取，删掉的文件就没有工作区副本了。

**响应归一化的形状在三处重复，且都带一个可选 `diff` 字段。** `getGitChangeDiff` 的两个分支返回的都是：

```
{ modified: diff?.modified ?? "", original: diff?.original ?? "", ...(diff?.diff ? { diff: diff.diff } : {}) }
```

三件事：`modified`/`original` 缺省为空串（保证前端拿到的总是字符串）、`diff` **只在非空时加**（与 `g01` 里 `missing_token` 的条件保留是同一手法）、以及一个 `as GitChangeDiff` 的断言（因为 `diff` 是额外字段）。

**状态映射统一走 `mapAnyGitStatusToClientStatus`。** 三处映射改动的地方都用它，并且都有一个相同形状的强制转换：`String(change.status) as Parameters<typeof mapAnyGitStatusToClientStatus>[0]`。这意味着**运行时的状态字符串是宽类型，而映射器期望一个具体的字面量联合**，所以就地断言。映射器本身抽在 `utils/git-status-mapper` 里，说明「git 状态词表 → 前端状态」是一个需要独立维护的转换表。

**两处「响应不是数组」的检查用了同一句文案。** `Invalid response from runtime - runtime may be unavailable`。这句话同时是诊断提示：形状错误在这些场景下最可能的原因是**运行时不可用**（一个 502/503 之类被代理转成了空对象），而不是数据内容变了。

**`getGitChanges` 在云代理路径上也做路径换算，只是换算在 URL 参数里。** `params.set("path", toAbsoluteRuntimePath(path))` 然后拼进查询串。于是**换算点有两处形式**：URL 参数（代理路径）与 `getFromRuntime` 里的 `params: { ...params, path: toAbsoluteRuntimePath(params.path) }`（直打路径）。这是同一个规则的两处落地，也是为什么换算函数必须幂等。

## 关键取舍

**源码事实：** 云端运行时会给自己前置 `/workspace/`，本地不会，所以客户端必须先把相对路径换成绝对路径，且换算函数幂等以便在多层调用中重复使用。

**源码事实：** 走原始 `HttpClient` 而非类型化包装器，因为后者（1）不认识较新的端点与参数，（2）会重新包装错误从而丢掉 `.status`。

**源码事实：** 404 被当作「服务器太旧」的特性判据，调用方隐藏提交区域而不报错；判据同时认 SDK `HttpError` 与 axios 形状。

**源码事实：** 改动与差异走云代理（服务端解析），提交族端点直打运行时；带 `commit` 的差异因为要读 git 对象（删掉的文件仍可渲染）也直打。

**源码事实：** 改动用「origin branch / merge-base」而非 `HEAD` 作基线，否则每次提交后界面改动会清空；差异必须用同一基线。

**源码事实：** `modified`/`original` 缺省为空串，`diff` 只在非空时加入——与 `g01` 里 `missing_token` 的条件保留一致。

**架构推论：** 这一层的复杂度不在业务逻辑，而在**同一个 REST 表面在不同部署下行为不同**。相对/绝对路径、有无代理、错误形态、端点是否存在——四类差异各自需要一处适配，而这些适配互相独立（不能合成一个「环境适配层」），因为它们发生的位置不同：路径要在构造请求前改，代理选择要在知道 `kind` 之后做，错误形态要在 `catch` 里判，端点存在性要在拿到 404 后决定。

这四条里最隐蔽的是**端点存在性**：它把「旧服务器」变成了一种正常状态（隐藏面板），而这需要错误对象保留 `status`——也就是需要绕过那个会重包错误的抽象层。这类「抽象层的便利与诊断需求相冲突」的情形，在任何分层的系统里都会反复出现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/git-service/agent-server-git-service.api.ts` | `AgentServerGitService` | 四个方法：改动、提交列表、单提交改动、差异 |
| `src/api/git-service/agent-server-git-service.api.ts` | `toAbsoluteRuntimePath` | 幂等的相对→绝对换算，防云端重复前缀 |
| `src/api/git-service/agent-server-git-service.api.ts` | `getFromRuntime` | 直打运行时的通用 GET，保留 `.status` |
| `src/api/git-service/agent-server-git-service.api.ts` | `isEndpointMissingError` | 404 特性判据，认 SDK 与 axios 两种错误形状 |
| `src/api/git-service/agent-server-git-service.api.ts` | `AgentServerGitChange` | 运行时的改动词表线格式（`status`、`path`） |
| `src/api/git-service/agent-server-git-service.api.ts` | `AgentServerGitCommit` / `AgentServerGitCommitsPage` | 蛇形命名的提交线格式与分页字段 |
| `src/api/git-service/agent-server-git-service.api.ts` | `mapAnyGitStatusToClientStatus`（引用） | git 状态词表到前端状态的转换 |

## 参考

- [OpenHands 源码：agent-server-git-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/git-service/agent-server-git-service.api.ts)
- [OpenHands 文档：Git 与工作区](https://docs.openhands.dev/openhands/usage/github)
- [Git 文档：merge-base](https://git-scm.com/docs/git-merge-base)
- [MDN：CORS 预检与允许来源](https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS)

## 与主核 / AI 的关系

这一层与主核里「同一份数据在工作区缓存与远端读取之间分派」是同类问题。数据库的差异在于：它把「远端不支持」表达成 SQL 错误码，而这类前端把「旧服务器没有这个端点」表达成**功能消失**。两种都是合理的产品决策，但都不约而同地需要「能区分 404 是因为没有这条路由，还是因为没有这个对象」——而很多 HTTP 客户端封装恰好把 `status` 丢了。

对 agent 工程的直接价值有三条。其一，**路径约定的差异要在客户端统一，且换算函数要幂等**。一个 API 在一处加前缀、在另一处不加，是分布式系统里最难发现的 bug 类型之一——因为它只在某个部署形态下出现。其二，**特性探测需要保留原始错误状态码**。只要有一层把错误重包成自己的类型，404 探测就会失效，而失效的表现是「旧部署上整个页面报错」而不是「某个面板消失」。其三，**改动的基线要显式选**。用 `HEAD` 做基线看起来很自然，但它会让「提交之后」的界面变空——这类「自然但错」的默认值需要靠注释才能守住，否则下一个人会把它改回去。
