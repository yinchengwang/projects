---
project: openhands
feature: active-backend
title: 当前后端的选定与回退
page_type: feature
schema_version: 1
refs:
  - path: src/api/backend-registry/active-store.ts
    symbols: [getActiveBackend, getEffectiveLocalBackend, getRegisteredBackends, getSnapshot]
  - path: src/api/backend-registry/active-store.ts
    symbols: [computeSnapshot, pickFallbackBackend, readInitialSelection]
---

# 当前后端的选定与回退

## 一句话职责

`active-store` 是一个模块级单例：它持有「已注册的后端清单 + 用户的显式选择 + 解析出的生效后端」这一份快照，并保证任何时刻都存在一个可用的生效后端——即使选择已失效、清单为空、或者根本没有注册过任何后端。

## 机制是什么

这个模块要回答的问题是**「现在用哪个后端」，而它的难点在于「选择可能无效」。** 选择是持久化在存储里的（`BackendSelection` 只有一个 id），而后端清单会被用户改动——删掉、改名、加新的。于是「选择的 id 在清单里找不到」是一个常态，不是异常。

于是这里用一个三态的快照来表达：

```
interface Snapshot {
  backends: Backend[];
  selection: BackendSelection | null;
  active: ResolvedActiveBackend;
}
```

- `backends`：当前注册的清单；
- `selection`：用户的**显式**选择，可能为 `null`（从未选择过），也可能指向一个已不存在的 id；
- `active`：**解析结果**，永远非空。

关键在于第三项是 `ResolvedActiveBackend` 而不是可空的。为此引入了一个哨兵：

```
export const NO_BACKEND_ID = "no-backend";

export const NO_BACKEND: Backend = {
  id: NO_BACKEND_ID,
  name: "No Backend Available",
  host: "",
  apiKey: "",
  kind: "local",
};
```

它的注释把两条约束写死了：「绝不能被持久化」，以及「调用方在解释 `kind`、`host`、`apiKey` 之前必须先检查 `isNoBackend()`」。也就是说这个对象的存在是为了让类型层面没有可选性，但它的字段是**无意义的**——`kind: "local"` 不是事实，只是为了让 `getEffectiveLocalBackend` 能编译通过（那里另有 `!isNoBackend(active)` 判断）。

这是一种「用哨兵消除 null」的标准手法，但代价被写清在注释里：任何读字段的地方都必须先判身份。

## 怎么实现的

**模块初始化就在跑一次完整解析。**

```
const initialBackends = readStoredBackends();
let snapshot: Snapshot = computeSnapshot(initialBackends, readInitialSelection(initialBackends));
```

顺序是「先读清单，再读选择」——因为 `readInitialSelection` 需要清单来做校验（「URL 里钉的后端 id 必须是已注册的」）。

**`readInitialSelection` 有三级优先级，每一级都有明确的理由。**

```
function readInitialSelection(backends: Backend[]): BackendSelection | null {
  const search = currentLocationSearch();
  const fromUrl = readBackendSelectionFromUrl(backends, search);
  if (fromUrl) { writeStoredActiveBackend(fromUrl); return fromUrl; }
  const handedBack = readLockedCloudOrgFromUrl(backends, search);
  if (handedBack) { writeStoredActiveBackend(handedBack); removeOrgFromCurrentUrl(); return handedBack; }
  return readStoredActiveBackend();
}
```

1. **URL 里的 `?backend=`** 优先。docstring 交代了这是一条真实故障的修复：「在新标签页打开的链接（cmd/ctrl-click、中键、『在新标签页打开』）应当落在拥有该会话的那个后端上，即使新标签页以空的 `sessionStorage` 起步、否则会采用 `localStorage` 里的回退」。
2. **云应用交接的 `?org=`**。注释：「云自己的 web 应用会指名它当时选中的组织。尊重它一次，然后丢掉，于是一次刷新会保留在这里之后选定的任何组织」。第二句是关键——它一次性使用（消费后 `removeOrgFromCurrentUrl()`），否则刷新会把用户在此处的选择一直压回去。
3. **存储里的选择**（`readStoredActiveBackend()`，sessionStorage 优先）。

前两级都会**先写回存储**再返回。这不是冗余：URL 参数在后续同标签内导航时会消失（注释：「后续的同标签导航会丢掉查询参数」），所以把「从 URL 学到的事实」立刻固化，导航之后仍然一致。

**`computeSnapshot` 的核心是一条 fall-through。**

```
function computeSnapshot(backends, selection): Snapshot {
  let activeBackend: Backend | null = null;
  let activeOrgId: string | null = null;

  if (selection) {
    const found = backends.find((b) => b.id === selection.backendId);
    if (found) { activeBackend = found; activeOrgId = selection.orgId ?? null; }
    // 选择指向已删除的后端时落到下面的未选择分支；orgId 也一并丢弃，
    // 因为它只在一个具体 cloud 后端的语境里有意义。
  }

  // @spec BM-003 — Fallback on active backend removal
  if (!activeBackend) { activeBackend = pickFallbackBackend(backends); activeOrgId = null; }

  return { backends, selection, active: { backend: activeBackend, orgId: activeOrgId } };
}
```

两处都值得记：

- **解析失败时 `orgId` 被清空**，理由写在注释里：org 只在特定 cloud 后端的语境里有意义。若保留它，就会得到「一个本地后端的 orgId」这种无意义组合。
- **`selection` 本身被原样保留在快照里**，即使它是失效的。也就是说 `getActiveSelection()` 可能返回一个指向不存在后端的选择——Docstring 层面这是「用户的意图」，而 `active` 是「现实」。两者的差异正是 UI 需要的信息（例如「你选的云端后端已被删除」这种提示）。

`// @spec BM-003 — Fallback on active backend removal` 这个标记说明这条行为是被规格追踪的。仓库里还有 `BM-001`（自动切换到新连接的后端）、`BM-002`（切换后端保持在同一页面）、`BM-004`（主题感知的连接反馈），说明后端管理这块被人为编号过。

**`pickFallbackBackend` 的三级阶梯是这一页最值得细读的地方。**

```
function pickFallbackBackend(backends: Backend[]): Backend {
  const healthyLocalBackend = backends.find(
    (backend) => backend.kind === "local" && getBackendHealthEntry(backend.id)?.disabled !== true,
  );
  if (healthyLocalBackend) return healthyLocalBackend;

  const localBackend = backends.find((backend) => backend.kind === "local");
  return localBackend ?? backends[0] ?? NO_BACKEND;
}
```

docstring 把「为什么不能简单地取 `backends[0]`」讲透了：

> 大多数 GUI 说的是本地 agent-server 协议，所以一个排在索引 0 的 cloud 或死掉的本地后端会让 `getEffectiveLocalBackend()` 返回 null，从而使每个本地协议调用都抛「No backend is configured」——即使清单里更靠后有一个健康的本地后端。

所以优先级是：**健康（未被禁用）的本地后端 → 任意本地后端 → 清单第一项 → 哨兵**。

注意「健康」的判据是 `disabled !== true`，即「连续失败次数已到上限」的反面；它不是「探测成功」。这个选择是保守的：新的、从未探测过的后端（`getBackendHealthEntry` 返回 `null`）**算健康**，所以刚添加的后端会成为回退目标。若判据换成「必须探测成功」，那么首屏（还没有任何探测结果）就永远选不出后端。

这个依赖方向也有代价：`active-store` 在文件顶部 `import { getBackendHealthEntry } from "./health-store"`，于是「活动后端的选择」依赖「健康状态」——一个选择逻辑因为一个通信协议的限制而不得不读健康状态。反过来，`health-store` 不依赖 `active-store`，所以这条边是单向的、没有环。

**`getEffectiveLocalBackend` 是一句很短的判断，但它的 docstring 是全文件最长的之一。**

```
export function getEffectiveLocalBackend(): Backend | null {
  const active = snapshot.active.backend;
  if (active.kind === "local" && !isNoBackend(active)) return active;
  return null;
}
```

docstring：「选用于**本地 agent-server 协议**调用的后端。GUI 的大多数服务（设置读写、会话 CRUD、skills/MCP/secrets 等）说的是本地 agent-server 的协议，它们在 cloud 主机上会失败。**只有活动后端才有资格：一个 cloud 选择不得借用另一个已注册的本地后端。**」

最后一句是这条设计的精髓。一个「聪明」的实现可能会说「当前选的是 cloud，但本地协议调用需要本地后端，那我去找一个本地后端来用」——那会造成静默的双后端并行：用户在 cloud 界面里操作，而设置被悄悄写到本地后端。这里选择返回 `null`，让调用方显式失败。

**写路径有两条，各自维护不同的不变量。**

`setActiveSelection`：

```
export function setActiveSelection(selection: BackendSelection | null): void {
  writeStoredActiveBackend(selection);
  snapshot = computeSnapshot(snapshot.backends, selection);
  notify();
}
```

先落存储再算快照。选择可以设成指向不存在后端吗？可以——`computeSnapshot` 会回退，但 `selection` 会被记住。这允许「用户选了一个后端，但它随后被删掉了」这条时间线自然工作。

`setRegisteredBackends`：

```
export function setRegisteredBackends(backends: Backend[]): void {
  writeStoredBackends(backends);
  let nextSelection = snapshot.selection;
  if (nextSelection && !backends.some((b) => b.id === nextSelection!.backendId)) {
    nextSelection = null;
    writeStoredActiveBackend(null);
  }
  snapshot = computeSnapshot(backends, nextSelection);
  notify();
}
```

它多做一件事：**如果新清单里没有当前选择指向的后端，就把选择清空并写回存储。**这与 `computeSnapshot` 的容忍（失效选择被保留在快照里）看似矛盾，其实分工清楚——`setRegisteredBackends` 是「用户在编辑清单」这条路径，此时把失效的选择清掉是对的；而 `computeSnapshot` 只是解析，它不擅自动用户的选择。若清单是从存储读来的初始状态且选择已失效，`computeSnapshot` 的容忍避免了「一启动就写存储」。

**快照是不可变的，订阅者靠替换感知变化。**

所有写路径都是 `snapshot = computeSnapshot(...); notify()`——整个对象被替换，不是原地改。于是 `React.useSyncExternalStore(subscribeActiveBackend, getSnapshot)` 能正确工作：`getSnapshot` 返回同一个引用直到下次替换，满足「快照必须按引用稳定」这条 React 契约。

`subscribeActiveBackend` 返回一个退订函数（`listeners.delete`），这是 `useSyncExternalStore` 要求的标准形态。`__resetActiveStoreForTests` 的存在说明这套单例在测试里需要显式重置——模块级状态的通病。

**自动切换的一条规格。**虽然不在本文件，但它的存在解释了 `setActiveSelection` 的调用来源：`active-backend-context` 里 `// @spec BM-001 — Auto-switch to newly connected backend`。也就是说「添加并成功连接一个后端之后自动选中它」是上层的行为，`active-store` 只提供原语。

## 关键取舍

**源码事实：** `active` 是非空的 `ResolvedActiveBackend`，靠 `NO_BACKEND` 哨兵消除可空性；该哨兵的注释明确要求「绝不可持久化」且「读字段前必须先 `isNoBackend()`」。

**源码事实：** 初始选择的优先级是「URL 的 `?backend=` → 云交接的 `?org=` → 存储」，前两级都会先写回存储再返回，因为同标签内的后续导航会丢掉查询参数。

**源码事实：** `computeSnapshot` 在选择失效时回退，并**同时清空 `orgId`**（org 只在特定 cloud 后端的语境里成立）；但失效的 `selection` 仍被保留在快照里，作为「用户的意图」与 `active` 的现实并列。

**源码事实：** 回退阶梯是「未被禁用的本地后端 → 任意本地后端 → 清单第一项 → 哨兵」，「健康」的判据是 `disabled !== true`，因此从未探测过的后端算健康。

**源码事实：** `getEffectiveLocalBackend` 只在**活动后端**是本地时返回它；cloud 选择不会借用另一个已注册的本地后端。

**源码事实：** `setRegisteredBackends` 会在清单里没有当前选择时清空选择并写回存储；`computeSnapshot` 则不写存储。

**架构推论：** 「把用户意图（`selection`）与现实（`active`）分开存」让回退成为可解释的：UI 既能显示「你现在实际在用 X」，也能显示「你原本选的 Y 已经不在了」。如果只保留 `active`，回退就变成了一次静默替换。

**架构推论：** `getEffectiveLocalBackend` 返回 `null` 而不是「另找一个本地后端」，是把「跨后端的静默降级」明确排除。在多后端系统里，最危险的故障不是「连不上」，而是「连上了但连错了」——用户以为在改 cloud 的设置，实际写进了本地。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/backend-registry/active-store.ts` | `getActiveBackend` | 返回解析后的生效后端（永非空） |
| `src/api/backend-registry/active-store.ts` | `getEffectiveLocalBackend` | 只在活动后端是本地时返回它，否则 null |
| `src/api/backend-registry/active-store.ts` | `getRegisteredBackends` | 返回当前注册清单 |
| `src/api/backend-registry/active-store.ts` | `getSnapshot` | 整体快照，供 `useSyncExternalStore` |
| `src/api/backend-registry/active-store.ts` | `computeSnapshot` | 解析选择、失效回退、清空 orgId |
| `src/api/backend-registry/active-store.ts` | `pickFallbackBackend` | 三级回退阶梯：健康本地 → 任意本地 → 第一项 → 哨兵 |
| `src/api/backend-registry/active-store.ts` | `readInitialSelection` | 三级初始选择：URL → 云交接 org → 存储 |
| `src/api/backend-registry/active-store.ts` | `NO_BACKEND` / `isNoBackend` | 消除可空性的哨兵与其身份判定 |

## 参考

- [OpenHands 源码：backend-registry/active-store.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/backend-registry/active-store.ts)
- [React 文档：useSyncExternalStore](https://react.dev/reference/react/useSyncExternalStore)
- [OpenHands 文档：管理后端](https://docs.openhands.dev/openhands/usage/settings)
- [TanStack Query：缓存键与失效](https://tanstack.com/query/latest/docs/framework/react/guides/query-keys)

## 与主核 / AI 的关系

这套「意图与解析结果分开」的形态，在主核里对应的是「连接池里的逻辑名称」与「实际连接的物理主机」：客户端说「用 A 库」，主核解析到某台机器上的某个实例；A 库被下线时，解析失败必须显式报错，而不是悄悄换一台机器。`getEffectiveLocalBackend` 的那句「cloud 选择不得借用另一个本地后端」就是同一条纪律的客户端版本。

对 AI 工程的实际意义有两条。第一，**回退必须分级且判据要保守**。这里的三级阶梯把「本地协议兼容性」放在「清单顺序」之上，因为绝大多数调用只说本地协议；而「健康」的判据用「未被禁用」而非「探测成功」，是为了让从未探测过的后端也能被选中。这两个选择都要靠对调用方的理解才能做对——脱离「谁在调用」谈回退顺序是没有意义的。

第二，**失效的选择应当被显示而不是被抹掉**。把 `selection` 留在快照里，使得「你选的后端已经没了」这件事可以被界面说出来。如果实现成「解析失败就把选择清成 null」，用户看到的就是「后端莫名其妙换了一个」，而他永远不会知道自己原本选的是什么。
