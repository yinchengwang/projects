---
project: openhands
feature: backend-model
title: 后端类型模型
page_type: feature
schema_version: 1
refs:
  - path: src/api/backend-registry/types.ts
    symbols: [BackendKind, BackendAuthMode, Backend, BackendSelection]
  - path: src/api/backend-registry/types.ts
    symbols: [ResolvedActiveBackend]
---

# 后端类型模型

## 一句话职责

`Backend` 是一个**主机（host）**的描述，不是一个 agent：它记录「这台 agent-server 在哪、怎么认证、是什么种类」，而「跑哪种 agent」是另一个维度（`AgentKind`，取值 `openhands` / `acp`）由档案层决定；`BackendSelection` 与 `ResolvedActiveBackend` 则分别表达「用户选了谁」与「解析后实际生效的是谁」。

## 机制是什么

先解决一处极易混淆的命名。OpenHands Agent Canvas 要接的 agent 有 OpenHands 原生、Claude Code、Codex、Gemini CLI、Pi、OpenCode 等多个，但 `BackendKind` 只有两个值：

```
export type BackendKind = "local" | "cloud";
```

也就是说这个注册表里的「后端」指的是**agent-server 的部署位置**，不是 agent 的种类：

- `local`：跑在用户自己机器（或本地栈）上的 agent-server，协议是本地 agent-server 那一套；
- `cloud`：OpenHands Cloud 的 app-server。

「哪种 agent 跑在这台 agent-server 上」是会话/档案层的事（`agent_settings.agent_kind` 与 agent profile），与后端是两个正交的轴。这个区分在后续每一页都会用到：`getEffectiveLocalBackend` 之所以存在，就是因为大量 GUI 服务只认本地 agent-server 协议，与「后端接线到了哪种 agent」无关。

第二个轴是认证方式：

```
export type BackendAuthMode = "api-key" | "cookie";
```

`api-key` 是常态；`cookie` 专门给「部署被锁死到单一 OpenHands Cloud 主机」的场景——此时认证靠浏览器同源 cookie，所以 `Backend.apiKey` 是空串而 `authMode` 是 `cookie`。这个字段可选，缺失等价于 `api-key`（校验函数接受 `undefined`）。

## 怎么实现的

**`Backend` 的七个字段可以分成三组。**

```
export interface Backend {
  id: string;
  name: string;
  host: string;
  apiKey: string;
  kind: BackendKind;
  authMode?: BackendAuthMode;
  connectionRevision?: number;
  marsSessionId?: string;
  marsConfigId?: string;
}
```

- **身份组**：`id`（稳定标识，用户可改名字但不该改 id）、`name`（显示名，用户可自由改）。
- **接线组**：`host`、`apiKey`、`kind`、`authMode`。这四项决定「能不能连上、以什么身份连上」。
- **派生组**：`connectionRevision`、`marsSessionId`、`marsConfigId`。

身份与接线的分离体现在注释里：id 是给程序用的（选择、缓存键、存储），name 是给人看的——所以改名不该让任何数据失效，而改 `host` / `apiKey` 必须让缓存失效。

**`connectionRevision` 是整个前端状态层的失效锚点。**

它的注释只有一句但信息量很大：

> Changes whenever connection credentials change, invalidating keyed data.

「凭证变化时递增，使所有按连接分键的数据失效」。它被大量查询键引用（设置缓存、会话历史、扩展、遥测、云组织、预算等），所以它的作用是把「后端身份」这个概念从「`id` 字符串」升级成「`id` + 凭证版本」。只按 `id` 分键的话，用户改了 API key 之后，旧的缓存命中会给出用旧凭据取回的数据——一个静默的错误答案。

递增发生在写路径上（`active-backend-context` 的 `updateBackend`）：`hostChanged` 或 `apiKeyChanged` 为真时取 `(b.connectionRevision ?? 0) + 1`，否则保留原值；同时还会调 `resetBackendHealth(id)` 清掉健康计数。这说明「凭证变了 ⟹ 旧的健康结论也作废」是同一条判断的第二个后果。

它的可选性也有含义：手写的、从旧版本存储读出的、或隧道派生出来的条目没有这个字段，所以所有消费点都写 `?? 0` ——缺失等价于第 0 版。

**`marsSessionId` / `marsConfigId` 是「宿主种类不变、来源可辨认」的补丁。**

这两条注释把 DigitalOcean MARS 的场景讲清楚了：

> `host` 指向一个 DigitalOcean MARS 会话时设置——即端口转发隧道（或 Web 服务器为它做的代理）的回环端。远端是一个普通的 agent-server，所以后端仍然是 `kind: "local"`；这两个字段只是让 Managed Agents 界面与连接生命周期能识别它。

关键那句话是「远端是一个普通的 agent-server，所以仍然是 local」。从协议角度，MARS 会话和普通本地后端没有区别，所以它不该有自己的 `kind`；但从生命周期角度它必须可辨认——隧道的关闭、会话的空闲暂停、计费都挂在它身上。解法是「加可选标记字段，不加新种类」。

`marsConfigId` 更弱：只记「这个会话是从哪个 MARS agent 配置启动的，当已知时」。它是溯源信息，不参与任何行为分支。

**`BackendSelection` 与 `ResolvedActiveBackend` 是两个不同的东西。**

```
export interface BackendSelection {
  backendId: string;
  orgId?: string | null;
}

export interface ResolvedActiveBackend {
  backend: Backend;
  orgId: string | null;
}
```

`BackendSelection` 是**引用**：只有 id 与可选 org，可以安全地持久化（它不含任何凭证）。`ResolvedActiveBackend` 是**解引用后的结果**：完整的 `Backend` 对象加上归一化过的 `orgId`（`null` 而不是 `undefined`）。

这个二分是必要的，因为「选中的后端可能已经不存在」：`selection.backendId` 指向一个已被删除的后端时仍是一个合法的 `BackendSelection`（它在存储里），但无法解析出 `ResolvedActiveBackend`。于是注册表的读接口返回的是解析后的形态，而写接口收的是引用形态——两种类型各自表达一种存在性。

`orgId` 的语义也值得记：它只在 cloud 后端上有意义（本地 agent-server 没有组织概念）。所以在 `BackendSelection` 里它是可选可空，在 `ResolvedActiveBackend` 里它被强制成 `string | null`——「解析过」意味着必须给出确定的答案。

**`ResolvedActiveBackend` 的 `orgId` 与 `backend` 必须一起用。**

这不是类型能强制的东西，只能靠实现约定：任何读取 `orgId` 的地方都必须同时读 `backend.id` 并把它作为缓存键的一部分。稍后看 `active-store` 时会看到 `computeSnapshot` 里「回退时把 orgId 一并清空」这条规则，它就是这一约定的落地——一个 org 只在它所属的那个 cloud 后端处于活动状态时才有意义。

## 关键取舍

**源码事实：** `BackendKind` 只有 `local` / `cloud` 两个值。「后端」在这个仓库里指 agent-server 的部署位置，不是 agent 种类；agent 种类是 `AgentKind` 所在的另一条轴。

**源码事实：** `authMode` 可选，缺失等价于 `api-key`；`cookie` 只用于被锁死到单一 Cloud 主机的部署。

**源码事实：** `connectionRevision` 的注释明确它的作用是「使所有按连接分键的数据失效」，它可缺失且所有消费点都以 `?? 0` 归一。

**源码事实：** MARS 会话刻意不新增 `kind`，而是用两个可选标记字段（`marsSessionId` / `marsConfigId`）标示来源，因为「远端是普通 agent-server」。

**源码事实：** `BackendSelection` 是可持久化的引用（不含凭证），`ResolvedActiveBackend` 是解引用后的形态，且 `orgId` 从 `string | null | undefined` 归一成 `string | null`。

**架构推论：** 「身份（id/name）」与「接线（host/apiKey/kind/authMode）」分开，再叠一个「凭证版本」字段，是把「改名是安全的」与「换凭证是危险的」两件事表达在同一结构里。若没有 `connectionRevision`，唯一的办法是改 `id`——那会连带丢掉用户的选择、健康记录与所有会话关联。

**架构推论：** 声明式为可选字段（`authMode` / `connectionRevision` / MARS 标记）远好于新增枚举分支。每加一个 `kind` 值都要在能力判断、探活、UI 上逐个加分支；而加一个可选标记只需要在真正关心它的少数几处读一次。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/backend-registry/types.ts` | `BackendKind` | 后端种类：`local` / `cloud`（部署位置，非 agent 种类） |
| `src/api/backend-registry/types.ts` | `BackendAuthMode` | 认证方式：`api-key` / `cookie`，缺失等价于前者 |
| `src/api/backend-registry/types.ts` | `Backend` | 一条后端记录：身份、接线、派生标记三组字段 |
| `src/api/backend-registry/types.ts` | `BackendSelection` | 可持久化的引用（id + 可选 org），不含凭证 |
| `src/api/backend-registry/types.ts` | `ResolvedActiveBackend` | 解引用后的形态：完整后端 + 归一化 orgId |

## 参考

- [OpenHands 源码：backend-registry/types.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/backend-registry/types.ts)
- [OpenHands 文档：后端与 Cloud](https://docs.openhands.dev/openhands/usage/cloud)
- [软件 agent SDK（agent 种类与协议的定义方）](https://github.com/OpenHands/software-agent-sdk)
- [DigitalOcean：MARS 托管 agent](https://docs.digitalocean.com/products/)

## 与主核 / AI 的关系

「连接描述」与「业务种类」分开建模，在主核里对应的正是「数据源连接串」与「表/库名」的分离：连接串决定连到哪、以什么身份连，而「连上之后用哪个模式」是另一件事。主核里常见的错误是把两者压进一个标识符（例如把库名塞进连接串名），结果是换一个库就要新建连接。

对 AI 工程更值得抄走的是 **`connectionRevision`** 这个字段。在多后端前端里，「按连接缓存数据」几乎必然存在，而缓存键如果只取 `backendId`，那么「用户修正了一个写错的 API key」之后，界面会继续显示用旧凭据取回的数据——错得静默且难以复现。加一个「凭证版本」并让所有缓存键包含它，代价是每个键多一段，收益是把一整类静默错误变成一次必然的重新取数。

另一条是 MARS 的建模方式：**同一协议、不同生命周期，用标记字段而不是新种类**。判据很清晰——如果两个东西对协议层完全一样、只在管理面（隧道、计费、空闲暂停）上有区别，那它不该成为新的 `kind`。这条判据能让枚举保持小，而小枚举是所有能力判断能写清的前提。
