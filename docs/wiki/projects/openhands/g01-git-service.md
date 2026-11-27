---
project: openhands
feature: git-service
title: Git 服务与云/本地分支
page_type: feature
schema_version: 1
refs:
  - path: src/api/git-service/git-service.api.ts
    symbols: [safeProvider, isCloudActive, normalizeRepositoryPage]
  - path: src/api/git-service/git-service.api.ts
    symbols: [EMPTY_REPOSITORY_PAGE, searchAgentServerRepositories, GitService]
---

# Git 服务与云/本地分支

## 一句话职责

同一个「列出仓库、列出分支、列出安装」的界面背后有两套数据来源：**云后端**走 App API 的 Git 集成（带 installation id、GitHub App 授权），**本地/agent-server 后端**走运行时自己的 `GitClient`——而这层负责在两者之间分派，并在任何一侧给出「不能查」时返回一个**形状完好的空页**而不是抛错。

## 机制是什么

| 部件 | 作用 |
|---|---|
| `isCloudActive()` | 读当前活跃后端的 `kind`，决定分派方向 |
| `isInvalidProvider` | 在分派之前先挡掉「没有 provider」的调用 |
| `safeProvider` | 一个**无校验**的类型断言，把 `string` 变成 `Provider` |
| `normalizeRepositoryPage` | 把 agent-server 返回的页归一成前端 `RepositoryPage` |
| `EMPTY_REPOSITORY_PAGE` / `EMPTY_BRANCH_PAGE` / `EMPTY_INSTALLATION_PAGE` | 三个形状完好的空返回值 |
| `GitService` | 七个静态方法，每个都按同一套「先挡 provider、再看后端、再分派」的顺序 |

## 怎么实现的

**三个常量的存在说明「空」是一个正常返回值而不是异常路径。** `EMPTY_REPOSITORY_PAGE` 是 `{ items: [], next_page_id: null }`，三个页类型各自一个。这些常量被用在**六处**不同的早退分支：provider 无效、非云后端却要查安装、安装索引越界、以及几个「只有云能查」的方法。

选择「返回空页」而不是「抛错」的直接后果是调用方不必区分「查不到」与「不该查」——一个下拉框拿到空列表就显示「没有可选项」。这是一个用户体验上的判断：**在这种输入型数据上，空比错误更好用**。

**`isInvalidProvider` 挡的是三种值，其中两种是字符串化的 `undefined`/`null`。** 注释说明了要防的具体后果：**会产出 `?provider=undefined` 这样的无效 API 请求**。也就是说这个函数不只是防 `null`——它防的是上游把 `undefined` 拼进了 URL。`!provider || provider === "undefined" || provider === "null"` 这三个判断合起来才覆盖实际出现过的坏值。

**`safeProvider` 的注释与实现之间的落差值得记住。** 它只有一行：`(value: string): Provider => value as Provider`——**一个赤裸的类型断言，没有任何运行期检查**。名字里的 `safe` 指的是「绕过类型系统的检查」，而不是「安全」。它被用在所有需要 `Provider` 的位置上，因为分派之后的值来自上游字符串，而 `Provider` 是一个联合类型。

把这与 `isInvalidProvider` 对照就清楚了：**这里的安全策略是「在入口挡掉空值，之后一律当合法」**。这是前端与外部 API 打交道时的常见取舍——穷尽枚举校验的代价是每次加一个 provider 就要改一处，而运行期的错误会在第一次请求上暴露。

**`normalizeRepositoryPage` 做三件事，第三件是一个「缺省即省略」的细节。** 它把每个 repo 的 `git_provider` 过一遍 `safeProvider`（把 SDK 的 `string` 收窄成前端的联合类型）、把 `next_page_id` 用 `?? null` 兜住、并且**只在 `missing_token` 存在时才写这个键**（`...(page.missing_token !== undefined ? { missing_token: page.missing_token } : {})`）。

第三件事的理由可以从字段名推出来：`missing_token` 表达的是「这个 provider 缺授权」。若它被无条件写成 `undefined`，一个读 `{...repo, missing_token}` 的调用方会看到这个键**存在但值为 undefined**，而在对象展开与 `JSON.stringify` 语义下这两者不同。所以这里用条件展开保持「有就是有、没有就没有」。

**只有 agent-server 路径需要归一化，云路径不需要。** 云路径的 `searchCloudRepositories` 直接返回前端 `RepositoryPage`，而 agent-server 的 `GitClient` 返回的是 SDK 类型，所以 `searchAgentServerRepositories` 先 `page as RepositoryPage` 强转，再过归一化。

这个不对称说明一件事：**「归一化」发生在 SDK 边界上，而不是在服务逻辑里**。也就是说这层并不试图统一两套后端的形状，只把其中一套补到另一套的样子。

**七个方法的分派规则可以归成三档，这个分层本身就是设计。**

| 档 | 方法 | 非云后端时 |
|---|---|---|
| 两套都有 | `searchGitRepositories`、`retrieveUserGitRepositories` | 走 agent-server 的 `GitClient` |
| 只有云有 | `getRepositoryDefaultBranch`、`getRepositoryBranches`、`searchRepositoryBranches`、`getUserInstallations`、`retrieveInstallationRepositories` | 返回对应的空页 / `null` |

第二档的判据是**「这个概念是不是云 API 才有的」**：installations（GitHub App 安装）在本地 agent-server 上不存在，而仓库与分支在云上走 App API、在本地走运行时。所以本地后端只能列仓库（且是运行时缓存里的那些），不能列 installation，也不能查某个仓库的分支列表。

`getRepositoryDefaultBranch` 的注释把它的用途写明了：**给那些列表项里没有 `main_branch` 的仓库取默认分支；未知时为 `null`**。这也是为什么它返回 `null` 而不是空页——它要的是单个值，不是列表。

**`searchGitRepositories` 与 `retrieveUserGitRepositories` 的差别只在 query 与 limit 默认值。** 前者 `limit = 100`、传 `query || undefined`；后者 `limit = 30`、不传 query。而两者在云路径上调用的是**同一个** `searchCloudRepositories`。

这是「搜索型列表」与「浏览型列表」的区分落在参数上，而不是落在两个不同的接口上。`limit` 的差异（100 vs 30）反映的是「用户主动搜」时愿意一次取更多，「默认打开面板」时取少一点。

**`query || undefined` 这个写法是必要的而不是风格。** 空字符串 `''` 在 HTTP 查询里会变成 `?query=`，多数 API 会把它当成「有个空的查询条件」。转成 `undefined` 后参数被省略，语义回到「没有查询」。相同处理在每个方法里都出现。

**`retrieveInstallationRepositories` 是唯一需要索引数组的方法。** 它接 `installationIndex` 与 `installations: string[]`，先取 `installations[installationIndex]`，取不到就返回空页。把「索引 → id」的解析放在服务层而不是调用方，好处是越界与缺 key 两种情况都落进同一个「返回空页」的路径。

**`searchAgentServerRepositories` 是一个独立的模块级函数而不是 `GitService` 的静态方法。** 它是两个「两套都有」的方法共用的私有实现，被抽到类外。这个细节说明 `GitService` 的职责被限定成**分派 + 归一化**，真正的本地实现不属于它的公共面。

## 关键取舍

**源码事实：** 「不能查」一律返回形状完好的空页（三个常量），而不是抛错。空是一个正常返回值。

**源码事实：** `isInvalidProvider` 除了 `null`/空串还专门挡字符串 `"undefined"` 与 `"null"`，因为它们会产出 `?provider=undefined` 这样的无效请求。

**源码事实：** `safeProvider` 是纯类型断言，运行期不校验。安全策略是「入口挡空值、之后一律当合法」，代价是新 provider 靠首次请求暴露。

**源码事实：** `normalizeRepositoryPage` 只在 `missing_token` 存在时写这个键，保持「有就是有、没有就没有」。

**源码事实：** 归一化只发生在 SDK 边界（agent-server 路径），云路径直接产出前端类型——这层不试图统一两套形状，只把一套补到另一套。

**源码事实：** 方法分两档：仓库/分支查询两套都有，installation 与分支列表只有云有。

**架构推论：** 这层的真正工作是**把「后端能力差异」收敛成一个统一的、总是可用的接口**。代价是有些方法在本地后端上永远返回空——调用方无从区分「这里确实没有」与「这个后端不支持查」。这是一个显式的取舍：在 UI 上两者的表现相同（显示没有可选项），而在日志/诊断上会丢失信息。

替代方案是让每个方法返回 `{items, unsupported?: true}`，调用方就能显示「本地后端不支持安装列表」。这在此项目里没做，说明作者判断这种区分对用户体验没有价值（用户看到的是同一个下拉框），而多出来的类型负担会扩散到所有调用点。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/git-service/git-service.api.ts` | `GitService` | 七个静态查询方法，统一的分派顺序 |
| `src/api/git-service/git-service.api.ts` | `isCloudActive` | 读活跃后端的 `kind` 决定分派 |
| `src/api/git-service/git-service.api.ts` | `isInvalidProvider` | 挡 `null`/空串/字符串化的 `undefined` 与 `null` |
| `src/api/git-service/git-service.api.ts` | `safeProvider` | 无校验的类型断言，把 `string` 收窄成 `Provider` |
| `src/api/git-service/git-service.api.ts` | `normalizeRepositoryPage` | SDK 页 → 前端页，含 `missing_token` 的条件保留 |
| `src/api/git-service/git-service.api.ts` | `searchAgentServerRepositories` | 本地后端的仓库查询实现（类外私有函数） |
| `src/api/git-service/git-service.api.ts` | `EMPTY_REPOSITORY_PAGE` / `EMPTY_BRANCH_PAGE` / `EMPTY_INSTALLATION_PAGE` | 三个「不能查」时的空返回值 |

## 参考

- [OpenHands 源码：git-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/git-service/git-service.api.ts)
- [OpenHands 文档：Git 集成](https://docs.openhands.dev/openhands/usage/github)
- [GitHub 文档：GitHub App 安装](https://docs.github.com/en/apps/using-github-apps)
- [TypeScript 文档：类型断言](https://www.typescriptlang.org/docs/handbook/2/everyday-types.html#type-assertions)

## 与主核 / AI 的关系

这层与主核里「同一查询在本地存储与远程存储上走不同实现」是同构问题：主核的 fdw（外部数据包装器）同样要按目标能力分派，并且同样面对「这个远程不支持某操作」该怎么表达——是报错，还是返回空结果集。数据库通常选择报错（`FEATURE_NOT_SUPPORTED`），因为查询计划需要知道真相；而前端 UI 选择返回空，因为它只需要一个可渲染的结果。

对 agent 工程的直接价值有三条。其一，**能力差异要收敛在一个地方**。把「哪些操作只有云有」写进一个服务层的分派表，比让每个调用点自己判断 `backend.kind` 要可靠得多，因为后者会漂移。其二，**「不能查」与「查不到」在 UI 层可以合并，在日志层不该合并**。这里合并了 UI 表现，代价是排查时说不清原因。其三，**类型断言要集中且命名诚实**。`safeProvider` 这个名字里的 `safe` 容易被误读成「安全校验」，实际上它是「绕过校验」——把它限定在入口挡过之后的位置使用，是这套写法唯一能成立的前提。
