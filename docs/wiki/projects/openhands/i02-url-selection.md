---
project: openhands
feature: url-selection
title: 后端选择的 URL 参数
page_type: feature
schema_version: 1
refs:
  - path: src/api/backend-registry/url-selection.ts
    symbols: [BACKEND_QUERY_PARAM, withBackendSelectionParams, readBackendSelectionFromUrl]
  - path: src/api/backend-registry/url-selection.ts
    symbols: [ORG_QUERY_PARAM, readLockedCloudOrgFromUrl]
---

# 后端选择的 URL 参数

## 一句话职责

同一个会话 id 在不同后端上指向不同的东西，而「当前后端」是**按标签页**存的——于是一个用 cmd/ctrl 或中键打开的新标签页会从错误的存储回退值启动、把会话解析到错的后端并显示「会话不存在」。这个模块让每个应用内链接**自己携带后端身份**，新标签页据此固定到正确的后端。

## 机制是什么

两个查询参数 + 四个读取/写入函数：

| 参数 / 函数 | 作用 |
|---|---|
| `BACKEND_QUERY_PARAM = 'backend'` | 后端 id |
| `ORG_QUERY_PARAM = 'org'` | 云组织 id（仅云后端需要） |
| `withBackendSelectionParams(path, active)` | 给应用内路径追加这两个参数 |
| `readBackendSelectionFromUrl(backends, search)` | 从 URL 读选择，只认**已注册**的后端 |
| `readLockedCloudOrgFromUrl(backends, search)` | 从云 web app 回链里读 org，绑定到锁定云后端 |
| `removeOrgFromCurrentUrl()` / `currentLocationSearch()` | 从地址栏删 org 参数 / 读当前查询串 |

## 怎么实现的

**那 14 行模块注释是这一页的核心，它把 bug 的成因写成了一个可复述的机制。** 逐句拆：

> 当前后端是**按标签页**的：`readStoredActiveBackend()` 优先 `sessionStorage`，只回退到 `localStorage`。

> 一个用 cmd/ctrl-click 或中键打开的标签页**不会可靠地继承 opener 的 `sessionStorage`**，所以它可能从 `localStorage` 回退值启动——而那个值持有的是**任意标签页里最后选中的那个后端**，不是我们刚才看的那个侧栏所属的那个。

> 新标签页于是用错误的后端解析会话 id，显示「conversation not found」。

> **把身份带在 URL 里让链接自描述**，于是无论存储里是什么，新标签页都能固定到正确的后端。

这段话解释了一个常被误判的现象：**「会话不存在」的错误其实是「后端选错了」**。因为会话 id 只在创建它的那个后端上有效，跨后端查询会得到一个干净的空结果或 404，而界面给出的是「不存在」——一个把「查错了地方」说成「东西不存在」的错误消息。

**这套设计的立论基础是「存储的作用域不可靠」，而不是「存储有 bug」。** `sessionStorage` 的语义本来就是按标签页的，而新标签页是否继承 opener 的 `sessionStorage` 取决于浏览器实现与打开方式（`noopener`、中键、拖拽、复制链接都可能不同）。所以修法不能是「让存储更可靠」——**能可靠自描述的只有 URL**。

**`withBackendSelectionParams` 的四步拆分处理的是「路径里已经有查询与 fragment」这个常见麻烦。**

1. 先切 `#`：`hashIndex`、`fragment`、`withoutFragment`。注释：**路径上任何 fragment 都会被逐字保留，并保持在查询串之后**。
2. 再切 `?`，但**只切第一个**。注释给了具体理由：**后面的 `?` 是普通查询数据（比如一个自带查询的 `next=` 重定向），而 `split("?")` 会静默丢掉它之后的一切**。
3. `new URLSearchParams(existingSearch)` 把已有参数读进来（于是**保留原有参数**而不是丢弃），再 `set` 这两个。
4. 拼回去：`${pathname}?${params}${fragment}`。

第 2 步的注释是本页最实用的一条：**`split("?")` 与 `split("?")[1]` 这类写法会静默截断**。而用 `indexOf` 切第一次、其余当数据，才是正确的「把查询串当成不透明负载」的写法。

**`set` 而不是 `append` 让这个函数幂等。** 一个链接如果已经被加过一次参数，再加一次不会得到两个 `backend=` ——`URLSearchParams.set` 是「替换或新增」。这一点很重要：链接可能在多层组件里被各加一次。

**没有 `backend.id` 时直接返回原路径。** `if (!backend.id) return path;`——一个未注册/未命名的后端不加参数。这是一个「宁可少信息也不要错信息」的取向：加一个空的 `backend=` 会让读取方以空串去匹配，而我下面会看到读取方对空值有明确处理。

**`orgId` 只在有值时加。** `if (orgId) params.set(ORG_QUERY_PARAM, orgId)`——本地后端没有 org，于是这个参数根本不出现。这是「缺省即省略」的又一例（与 `g01` 的 `missing_token`、`g02` 的 `diff` 是同一手法）。

**`readBackendSelectionFromUrl` 的「只认已注册」是本页的安全取向。** 注释：

> 从当前 URL 读一个后端选择，**只在它指名一个已注册后端时保留它**。一个不认识的 id（来自另一台机器的链接，或一个已经被移除的后端）被忽略，于是调用方回退到存储里的选择。

而实现里 `if (!backends.some((b) => b.id === backendId)) return null;`。

这条判断挡住的是一类真实情形：**链接可能来自另一台机器、另一个账号，或者指向一个已经被删除的后端**。此时 URL 里的 id 无法解析——如果硬用它，调用方会进入「后端不存在」的空转状态；忽略它则退回「上次用的那个」，至少是可用状态。

`readLockedCloudOrgFromUrl` 同理：`if (!backends.some((b) => b.id === LOCKED_CLOUD_BACKEND_ID)) return null;`。

**`readLockedCloudOrgFromUrl` 的判据里有一条反向条件。** `if (params.has(BACKEND_QUERY_PARAM)) return null;`——**带 `backend=` 就不归它管**，交给 `readBackendSelectionFromUrl`。

它的 docstring 描述的是另一个方向的链接：

> 读云自己的 web app 在链接回来时交回的 org。它的「Back to App」链接只带 `?org=<id>`——这个参数正是 Canvas 放进它指向云 Settings 的链接里的——**因为锁定于那个云主机的 Canvas 只有一个后端**。其他情形（没有 org、还指名了后端、或没有锁定云后端）都留给其他读取器。

这段说明了两个方向的对称：Canvas → 云（带 `org`），云 → Canvas（带回同一个 `org`）。而「锁定云后端只有一个」这个事实让回链不需要 `backend=`。

**「锁定云后端」在这里是一个常量身份。** 它的 id 是 `LOCKED_CLOUD_BACKEND_ID`（来自 `default-backend`），于是在回链场景里后端不需要被指名——它只能是那个。这也解释了为什么要有两个读取器：**两种链接的语义不同**（一个说「到这个后端」，一个说「到这个 org 的云」）。

**`removeOrgFromCurrentUrl` 用 `replaceState` 而不是 `pushState`。** 它解析当前 `href`，若没有 `org` 就直接返回；有就删掉并用 `replaceState` **重写整个 URL**（`` `${url.pathname}${url.search}${url.hash}` ``）。

用 `replaceState` 意味着**不新增历史记录**——参数被消费掉之后，地址栏干净且后退按钮不会回到「带 org」的那个状态。这是一个一次性消费参数的正确处理方式。

它还考虑了两个边界：`typeof window === "undefined"` 直接返回（SSR / 测试环境），以及删参数用 `url.searchParams.delete` 而不是字符串拼接（避免留下 `?` 或 `&&`）。

**`currentLocationSearch()` 是一个 4 行的适配函数。** 它只做「在浏览器里读 `window.location.search`，否则空串」。这个存在的理由是把**非浏览器的环境变量化**收进一个地方——于是所有调用方都不必自己判断 `typeof window`。

**这两个 try/catch 包 `new URLSearchParams(search)` 是防「非法查询串」。** `URLSearchParams` 对多数输入都能容忍，但对孤立的代理对（lone surrogate）会抛 `URIError`。两个读取函数都把它捕获并返回 `null`——意味着**一个畸形的 URL 被当成「没有选择」**，回退到存储值。

这个兜底方向是对的：URL 是外部输入（可以被粘帖、被别人构造），而选择逻辑不该因为它的畸形而崩。

## 关键取舍

**源码事实：** 修法的立论是 `sessionStorage` 的作用域语义不可靠（新标签页是否继承取决于打开方式），所以只能让链接自描述，而不是试图让存储更可靠。

**源码事实：** 写路径只切**第一个** `?`，因为后面的 `?` 是普通查询数据；`split("?")` 会静默丢数据。

**源码事实：** 写路径用 `set` 而非 `append`，所以重复调用幂等；fragment 被逐字保留并放在查询之后；没有后端 id 时原样返回路径。

**源码事实：** 读路径只保留指名**已注册**后端的选择，不认识的 id 被忽略并回退到存储——因为链接可能来自另一台机器或指向已删除的后端。

**源码事实：** 两个读取器的语义不同：`readBackendSelectionFromUrl` 处理「到这个后端」，`readLockedCloudOrgFromUrl` 处理「到这个 org 的云」，后者遇到 `backend=` 就主动退出。

**源码事实：** 消费 `org` 参数用 `replaceState`，所以不污染历史；删除用 `searchParams.delete` 而不是字符串拼接。

**源码事实：** 畸形查询串（孤立代理对）被捕获并当成「没有选择」，URL 作为外部输入不让选择逻辑崩。

**架构推论：** 这一页是**「隐式状态 vs 显式参数」**这条老问题的一个完整案例。隐式状态（存储里的「当前后端」）在单标签页里够用，但它跨上下文时不可靠；显式参数解决了跨上下文的问题，代价是**每个链接都要被改造成自描述**——也就是一个 `withBackendSelectionParams` 必须在所有生成应用内链接的地方被调用。

这个代价正是这类修法的实际难点：**遗忘一处就会有一个链接在某些情况下打开错的东西**。而这段代码没有试图「兜住」这种遗漏（它做不到），所以它的价值在于那段 14 行注释——**把成因写清楚，让后来加链接的人知道为什么必须调它**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/backend-registry/url-selection.ts` | `BACKEND_QUERY_PARAM` / `ORG_QUERY_PARAM` | 两个自描述参数的名字 |
| `src/api/backend-registry/url-selection.ts` | `withBackendSelectionParams` | 追加后端身份，保留 fragment 与已有参数，幂等 |
| `src/api/backend-registry/url-selection.ts` | `readBackendSelectionFromUrl` | 读选择，只认已注册后端，畸形串返回 `null` |
| `src/api/backend-registry/url-selection.ts` | `readLockedCloudOrgFromUrl` | 云回链的 org 读取，遇 `backend=` 主动退出 |
| `src/api/backend-registry/url-selection.ts` | `removeOrgFromCurrentUrl` | 用 `replaceState` 一次性消费 org 参数 |
| `src/api/backend-registry/url-selection.ts` | `currentLocationSearch` | 把「非浏览器环境」的差异收进一处 |

## 参考

- [OpenHands 源码：src/api/backend-registry/url-selection.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/backend-registry/url-selection.ts)
- [MDN：URLSearchParams](https://developer.mozilla.org/en-US/docs/Web/API/URLSearchParams)
- [MDN：History.replaceState](https://developer.mozilla.org/en-US/docs/Web/API/History/replaceState)
- [MDN：sessionStorage 与 localStorage 的作用域](https://developer.mozilla.org/en-US/docs/Web/API/Web_Storage_API)

## 与主核 / AI 的关系

这个问题与主核里「连接池绑定到哪个数据库」属于同一类：**一个标识在不同上下文里指向不同对象**。数据库用 `search_path` 与会话状态解决它，跨会话时则必须显式限定（`db.schema.table`）。这里的 `backend=` 参数就是那个显式限定符——把「隐式上下文」变成「显式限定」。

对 agent 工程的直接价值有三条。其一，**「找不到」经常是「查错了地方」**。把会话 id 拿到错的后端上解析，得到的不是一个明确的「后端不匹配」错误，而是「不存在」——这类错误消息会把排查引向完全错误的方向。其二，**修跨上下文的状态问题时，要让数据自描述而不是让存储更可靠**。`sessionStorage` 的继承行为由浏览器与打开方式决定，不是应用能控制的；URL 是可以控制的。其三，**URL 改动要遵守两个细节**：只切第一个 `?`（后面的属于负载）、用 `set` 而非 `append`（幂等）。这两条各自对应一类「本地测不出来、线上偶发」的 bug。
