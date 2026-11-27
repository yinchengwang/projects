---
project: openhands
feature: default-backend
title: 默认后端与锁定后端
page_type: feature
schema_version: 1
refs:
  - path: src/api/backend-registry/default-backend.ts
    symbols: [SEEDED_DEFAULT_BACKEND_ID, makeDefaultLocalBackend]
  - path: src/api/backend-registry/default-backend.ts
    symbols: [LOCKED_CLOUD_BACKEND_ID, makeLockedCloudBackend]
---

# 默认后端与锁定后端

## 一句话职责

两个工厂各自造一条**从部署环境推导出来的**后端记录：`makeDefaultLocalBackend` 从启动器注入的 host 与 session key 造一条本地后端（首次加载时作为种子写入），`makeLockedCloudBackend` 在被锁死到单一 OpenHands Cloud 主机的部署里造那条唯一的云端后端——两条路径在「锁定模式」下互斥。

## 机制是什么

这个模块解决的问题是**「用户第一次打开时，后端清单里该有什么」。** 答案是「取决于这次部署是怎么发的」，而不是「取决于用户在界面上填了什么」。于是它不去问用户，而是读注入的环境：启动器（CLI 或静态服务器）会在构建或服务时把后端地址与 session key 塞进前端。

两种部署形态决定了两个工厂，而且它们**在锁定模式下互相排斥**：

| 部署形态 | 判据 | 结果 |
|---|---|---|
| 被锁死到单一 Cloud 主机 | `getLockedCloudHost()` 返回非空 | 只有 `locked-cloud` 一条，且 `kind: "cloud"` + `authMode: "cookie"` |
| 普通自托管 | `getLockedCloudHost()` 返回 null | 若 host 与 apiKey 都可用，则种一条 `default-local` |

两个工厂都返回 `Backend | null`，`null` 的含义是**「这次部署没给出足够信息，让 onboarding 去问用户」**——不是错误。这个区分很重要：`null` 不等于失败，它是一次正常的「该问人了」。

两个稳定的 id 常量是这套机制的骨架：

```
export const SEEDED_DEFAULT_BACKEND_ID = "default-local";
export const LOCKED_CLOUD_BACKEND_ID = "locked-cloud";
```

它们的注释强调「stable id」——因为别处的行为靠 id 识别：`storage` 会按 `SEEDED_DEFAULT_BACKEND_ID` 去找那条要同步 apiKey 的记录；`url-selection` 会按 `LOCKED_CLOUD_BACKEND_ID` 判断「是否有被锁定的云端后端」。

## 怎么实现的

**`makeDefaultLocalBackend` 的第一行就是一条硬性的排除。**

```
export function makeDefaultLocalBackend(): Backend | null {
  // Locked-to-Cloud deployments must never auto-seed a Local backend — see the docblock above.
  if (getLockedCloudHost()) return null;

  const host = getAgentServerBaseUrl();
  const apiKey = getAgentServerSessionApiKey();

  if (!host || !apiKey) return null;

  return { id: SEEDED_DEFAULT_BACKEND_ID, name: DEFAULT_LOCAL_BACKEND_NAME, host, apiKey, kind: "local" };
}
```

docstring 把「为什么锁定模式必须拒绝种本地后端」写成了一段完整的故障描述：

> 在锁定模式下用户只能对着配置好的 Cloud URL 认证，所以从一份烘焙/注入的 session key 种一个本地后端，会**短路首次运行的 onboarding 关卡**，并把用户困在 Manage Backends 恢复弹窗里、挂着一个连不上的本地条目。

也就是说：如果这里不返回 `null`，那么在锁定部署里，清单会先出现一条「看起来可用」的本地后端，于是 onboarding 认为「已经有后端了」而跳过，但那条本地后端根本没配认证——用户最终看到的是一个连不上的条目加一个恢复弹窗。这个故障的症状（「弹窗而不是 onboarding」）与原因（「多了一条种子后端」）之间的距离很远，所以这段注释值得逐字读。

条件是两个都得有：`host` 与 `apiKey`。`getAgentServerBaseUrl()` 会退回 `window.location.origin`（注释见 `agent-server-config`），所以 host 几乎总是有值；真正决定成败的是 apiKey。而 apiKey 有两个来源（`getBakedSessionApiKey` 的 docstring 写得很细）：构建期烘焙的 `VITE_SESSION_API_KEY`，与服务时注入的 `window.__AGENT_CANVAS_SESSION_API_KEY__`。后者的注释直接说明了缺了它会怎样：「发布的二进制无法构造一个默认本地后端，注册表保持为空，用户看到 Manage Backends 弹窗而不是 onboarding」。

**`makeLockedCloudBackend` 有两个前置条件，第二个是隐式的。**

```
export function makeLockedCloudBackend(): Backend | null {
  if (!getLockedCloudHost()) return null;

  const host = getCookieAuthCloudHost();
  if (!host) return null;

  return { id: LOCKED_CLOUD_BACKEND_ID, name: LOCKED_CLOUD_BACKEND_NAME, host, apiKey: "", kind: "cloud", authMode: "cookie" };
}
```

第一个是「有锁定主机」。第二个更微妙：`getCookieAuthCloudHost()` 只在**当前页面的 origin 与被锁定的 host 相同时**才返回 origin，否则返回 `null`。所以如果部署被锁到 `https://app.all-hands.dev`，而用户从别的域打开页面，这个工厂返回 `null`——不会造出一条「指向一个 cookie 不会生效的主机」的后端。

这段逻辑的意义在于 cookie 是**同源**凭据：`authMode: "cookie"` 的可行性完全取决于「页面就在那个域上」。把它检查一遍再构造，比构造出来然后在探活时失败要早得多。

`apiKey: ""` 是刻意的：cookie 模式没有 key 可用，空串在 `Backend` 的类型里合法，而「有没有 key」的判断处处都要先看 `authMode !== "cookie"`（探活函数里就这么写的）。

`host` 取的是 `window.location.origin`，而不是 `getLockedCloudHost()` 的返回值——两者在被允许的情况下是同一个值（`getCookieAuthCloudHost` 内部就做了 `isSameCloudHost` 比较然后返回 origin）。用 origin 而不是配置值，保证存的这条后端与当前页面的来源一致。

**两个工厂之间还有一条不成文的对称。**

`DEFAULT_LOCAL_BACKEND_NAME = "Local"`、`LOCKED_CLOUD_BACKEND_NAME = "OpenHands Cloud"`。名字是给人看的初始值，用户之后可以改（`makeDefaultLocalBackend` 的 docstring 明说「种下之后这条后端就是普通条目——用户可以改名、改它的 host/api key，或者像任何其它后端一样删掉」）。

这句 docstring 定义了种子的**性质**：它不是特权条目，只是首次写入的初值。这一点与「锁定云端」形成对照——`locked-cloud` 虽然也没有特权代码，但它每次都在 `readStoredBackends` 里被重新写回（见 b02 页），实质上不可删除。

**种子只在「缺失」时发生，这条判断在 `storage` 里。**

本文件只负责「造什么」。什么时候造由 `readStoredBackends` 决定：`raw === null`（首次安装）或解析后有效条目为空时，才调这两个工厂之一，且结果为空就返回空数组。于是「用户主动删除了所有后端」与「首次安装」会得到同样的结果——都重新种一条。这个行为在 b02 页展开。

## 关键取舍

**源码事实：** `makeDefaultLocalBackend` 的第一行在锁定模式下直接返回 `null`，docstring 记录了不这么做的后果：短路 onboarding 关卡、把用户困在 Manage Backends 恢复弹窗。

**源码事实：** 本地种子要求 `host` 与 `apiKey` 两者都有；apiKey 有两个注入来源（构建期 env 与服务时 window 全局），缺了后者会让发布二进制无法构造默认后端。

**源码事实：** `makeLockedCloudBackend` 要求页面 origin 与被锁主机相同（经 `getCookieAuthCloudHost` 检查），因为 cookie 是同源凭据；它用 `window.location.origin` 而非配置值作 host。

**源码事实：** 锁定云端后端是 `kind: "cloud"` + `authMode: "cookie"` + `apiKey: ""`，所以任何「有没有 key」的判断都必须先排除 cookie 模式。

**源码事实：** 两个 id 都是 stable id，被 `storage`（同步本地种子 apiKey）与 `url-selection`（判断有无锁定云端后端）按 id 识别。

**架构推论：** 返回 `null` 表示「让 onboarding 去问用户」而不是「出错了」，这个约定让「部署没配全」与「用户没填全」走同一条路：需求收集界面。若用异常表达，调用方就要区分「可恢复的缺失」与「真的配置错误」两件事。

**架构推论：** 「种子」与「锁定」是这个模块里两种完全不同的默认：种子是一次性初值，之后完全普通；锁定是每次读都重建的强制约束。同一份清单里同时存在两种语义，是靠 `readStoredBackends` 的分支顺序来保证的——锁定检查在最前，一旦命中就不再走种子逻辑。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/backend-registry/default-backend.ts` | `SEEDED_DEFAULT_BACKEND_ID` | 本地种子后端的稳定 id，被存储同步逻辑按 id 识别 |
| `src/api/backend-registry/default-backend.ts` | `makeDefaultLocalBackend` | 从注入的 host + session key 造本地种子；锁定模式下拒绝 |
| `src/api/backend-registry/default-backend.ts` | `LOCKED_CLOUD_BACKEND_ID` | 锁定云端后端的稳定 id，被 URL 选择逻辑按 id 识别 |
| `src/api/backend-registry/default-backend.ts` | `makeLockedCloudBackend` | 造唯一云端后端；要求页面 origin 与被锁主机相同 |

## 参考

- [OpenHands 源码：backend-registry/default-backend.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/backend-registry/default-backend.ts)
- [OpenHands 文档：自托管与 Cloud](https://docs.openhands.dev/openhands/usage/cloud)
- [OWASP：基于 Cookie 的会话管理](https://cheatsheetseries.owasp.org/cheatsheets/Session_Management_Cheat_Sheet.html)
- [Vite 文档：环境变量与 `import.meta.env`](https://vite.dev/guide/env-and-mode)

## 与主核 / AI 的关系

「部署形态决定初始配置」与主核里「编译期开关决定默认参数」是同一类设计：主核用构建选项决定页大小、是否启用某索引，这里用注入的环境决定初始后端清单。共同点是**默认值不该由用户猜，而该由发版方式决定**。

对 AI 工程的实际意义集中在两条注释上。第一，`makeDefaultLocalBackend` 那段「锁定模式下必须拒绝」的注释，是一份完整的因果链：多了一条种子后端 → onboarding 认为已有后端而跳过 → 用户面对一个连不上的条目 → 落在恢复弹窗而不是正常流程。这类「一个看似无害的默认值改变了整条流程的判定」的故障，只有把因果写下来才不会被后人当成冗余判断删掉。

第二，`getCookieAuthCloudHost` 的存在说明**凭据的可用性有前提条件**，而前提条件应该在构造时检查而不是在使用时失败。cookie 只在同源生效，所以「造一条 cookie 后端」的唯一正确时机是页面就在那个源上。这类「把前置条件检查提前到构造点」的做法，与主核里「连接字符串在建立连接池时就验证」是同一种纪律。
