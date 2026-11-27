---
project: openhands
feature: backend-health
title: 后端健康状态跟踪
page_type: feature
schema_version: 1
refs:
  - path: src/api/backend-registry/health-store.ts
    symbols: [getHealthSnapshot, getBackendHealthEntry, subscribeBackendHealth]
  - path: src/api/backend-registry/health-store.ts
    symbols: [recordBackendFailure, recordBackendSuccess, dropBackendHealth]
---

# 后端健康状态跟踪

## 一句话职责

`health-store` 为每个后端维护一条**连续失败计数**，到第五次就把该后端标记为 `disabled`，从而停止后台探活——而且这个状态落在 `localStorage` 里，所以刷新页面不会偷偷给一个已知连不上的后端重新上弦。

## 机制是什么

这里要解决的不是「后端通不通」（那是一次探活的结果），而是**「还要不要继续问它」**。两者是不同的问题：

- 通不通是瞬时的，每次探活都能重算；
- 要不要问是**跨会话的策略**——一个已经失败五次的云端主机，在用户修正配置之前不该再被轮询。

后者必须持久化，否则每次刷新都会重新开始五次尝试，界面上就是无穷无尽的红灯闪烁与请求噪音。于是健康状态被拆成两层：

| 层 | 文件 | 内容 |
|---|---|---|
| 存储层 | `health-storage` | `localStorage` 读写、条目结构、上限与校验、错误消息截断 |
| 存储管理器 | `health-store`（本页） | 内存中的 map、写入即持久化、变更通知、失败/成功/丢弃三种转移 |

本页只讲 `health-store`，但它的每个行为都以存储层的常量为前提：`MAX_CONSECUTIVE_FAILURES = 5` 与 `MAX_ERROR_MESSAGE_LENGTH = 500`。

条目结构四个字段，各有明确用途：

```
interface BackendHealthEntry {
  consecutiveFailures: number;
  lastError: string | null;
  lastFailureAt: number | null;
  disabled: boolean;
}
```

`disabled` 是冗余的吗？不是——它是**派生值的快照**。可以由 `consecutiveFailures >= MAX` 算出来，但把它存下来让「读」不需要知道上限，也让存储层的校验能把「一个被篡改成 `-1` 的计数」拒掉（那条注释我稍后会讲）。

## 怎么实现的

**一个内存 map + 一个 listener 集合，写入即持久化。**

```
let healthMap: BackendHealthMap = readStoredHealth();
const listeners = new Set<Listener>();

function commit(next: BackendHealthMap): void {
  healthMap = next;
  writeStoredHealth(next);
  notify();
}
```

`commit` 是唯一的写入口：换内存引用、写存储、通知。三件事的顺序固定——先换内存（读取者立刻看到新值），再写存储（持久化），再通知（UI 重渲染）。所有变更函数都以不可变方式构造新 map（`{ ...healthMap, [id]: nextEntry }`），所以 `getHealthSnapshot` 返回的引用在两次变更之间稳定，满足 `useSyncExternalStore` 的要求。

模块初始化即 `readStoredHealth()`——也就是页面一加载就把上一会话的失败计数读回来。这一步是整套机制能跨刷新生效的关键。

**`recordBackendFailure` 是这一页的核心，它有四行值得逐句读。**

```
export function recordBackendFailure(id: string, error: unknown): void {
  const prev = healthMap[id];
  const consecutiveFailures = Math.min((prev?.consecutiveFailures ?? 0) + 1, MAX_CONSECUTIVE_FAILURES);
  const disabled = prev?.disabled === true || consecutiveFailures >= MAX_CONSECUTIVE_FAILURES;

  const nextEntry: BackendHealthEntry = {
    consecutiveFailures,
    lastError: truncateErrorMessage(error),
    lastFailureAt: Date.now(),
    disabled,
  };

  commit({ ...healthMap, [id]: nextEntry });
}
```

1. **计数被 `Math.min` 夹在上限。** 这防止计数无限增长——否则 `localStorage` 里会存一个越来越大的数字，而它其实已经饱和。存储层的校验也依赖这一条（只接受 `0..MAX` 的整数）。
2. **`disabled` 有「粘性」**：`prev?.disabled === true || ...`。一旦被禁用，即使计数因某种原因回落到别处，也不会自己解开。这是对的——禁用只能由用户改配置或成功探活来解除。
3. `lastError` 走 `truncateErrorMessage`，来源是 `getUserFacingConnectionErrorMessage(error)`，缺失时给 `"Unknown error"`，超 500 字符截断并加省略号。所以存下来的**不是原始错误**，而是一句面向用户的话。
4. `lastFailureAt` 用 `Date.now()`，而 `consecutiveFailures` 由上限夹住——一个记录「失败了几次」，一个记录「最后一次是什么时候」。

docstring 点明了这个状态的用途：「达到 `MAX_CONSECUTIVE_FAILURES` 后条目被标记 `disabled: true`，轮询 hook 读它来停止发射探针——**包括在全新页面加载时，因为状态住在 localStorage 里**」。那句「包括在全新页面加载时」就是它必须持久化的全部理由。

**三个「清除」函数当前实现相同，但刻意保留为三个。**

```
export function recordBackendSuccess(id: string): void {
  if (!(id in healthMap)) return;
  const { [id]: _removed, ...rest } = healthMap;
  commit(rest);
}

export function resetBackendHealth(id: string): void { ...同上... }

export function dropBackendHealth(id: string): void { ...同上... }
```

三个函数都做「删掉那个键、若无该键则什么都不做」。`resetBackendHealth` 的 docstring 极其诚实：

> 用户更新某后端的配置之后为它重新上弦轮询。**今天与 `recordBackendSuccess` 完全一致，但保留为独立函数是为了让调用点的语义读起来清楚。**

这是一处「用命名承载意图」的取舍。三个调用点确实语义不同：

- `recordBackendSuccess` —— 探活成功了，清掉失败记录；
- `resetBackendHealth` —— 用户改了 host/apiKey（`active-backend-context` 的 `updateBackend` 里调），让后端重新可用；
- `dropBackendHealth` —— 后端被删除了（`removeBackend` 里调）。

合并成一个函数是可行的，但调用点读起来会变成「成功时调 `clearHealth`、删除时也调 `clearHealth`」——读者必须去猜「为什么删除要调一个叫 success 的函数」。保持三个名字的成本是三份重复的四行代码，收益是每个调用点自解释。`if (!(id in healthMap)) return` 这条早期返回也很重要：没有它，每次成功探活（哪怕从未失败过）都会构造一个新 map 并触发一次持久化写入与一次全员通知。

**订阅接口是 `useSyncExternalStore` 的标准形态。**

```
export function subscribeBackendHealth(listener: Listener): () => void {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}
```

返回退订函数而不是要求调用方传同一个引用去 `unsubscribe`——这与 `active-store` 完全一致，两个 store 用了同一套形状。

**读取侧有两个粒度。**

`getHealthSnapshot()` 返回整个 map（给 `useSyncExternalStore` 做快照），`getBackendHealthEntry(id)` 返回单条或 `null`。后者被 `pickFallbackBackend` 直接调用：

```
getBackendHealthEntry(backend.id)?.disabled !== true
```

注意这里的判断：`disabled !== true` 而不是 `=== false`。于是 `null`（没有条目，从未探测过）与 `false`（探测过且没被禁用）都算健康，只有 `true` 才被排除。这条 `?? null` 与 `!== true` 的组合，是「未知不等于不健康」这个判断在代码里的具体形态。

**消费者侧如何把它变成三态。**`useBackendsHealth` 里最终产出的不是 store 里的原始条目，而是一层归一化：

```
let isConnected: boolean | null;
if (hasMissingCloudApiKey) isConnected = false;
else if (disabled) { isConnected = false; }        // 轮询停了就当成断开
else if (r.isSuccess) isConnected = true;
else if (r.isError) isConnected = false;
else isConnected = null;
```

第二支的注释解释了为什么要把 `disabled` 折进 `isConnected`：「达到上限后轮询停止——当成断开，好让既有消费者（圆点、徽标）不需要知道这些新字段就能渲染成红色」。这是「新增状态字段时保持旧消费者语义不变」的经典处理：新的 `disabled` 字段并不要求 UI 改代码，它在消费侧被翻译回旧的三态。

**在 `disabled` 之后仍有三条重新探活的路。**

`useBackendsHealth` 的 `shouldProbe` 把三种情况并列：

```
const shouldProbe =
  !hasMissingCloudApiKey &&
  (!isDisabled || probeDisabledOnce || shouldReprobeStaleCloudNetworkError);
```

- `probeDisabledOnce`：调用方显式要求（Manage Backends 弹窗打开时），让一个已恢复的后端能清掉陈旧的错误状态；
- `shouldReprobeStaleCloudNetworkError`：被禁用、是 cloud、且 `lastError` 是 CORS/网络类错误——这类错误可能只是暂时的网络问题，不代表后端真的挂了。

后一条用到了 `isCorsOrNetworkErrorMessage(entry?.lastError)`，即**把持久化的错误文案当成可判定的状态**。这也是为什么 `lastError` 存的是 `getUserFacingConnectionErrorMessage` 的结果而不是原始异常：它必须是可比较的稳定字符串常量集合（`MISSING_BACKEND_API_KEY_ERROR`、`CLOUD_BACKEND_LOGGED_OUT_ERROR`、`CLOUD_BACKEND_API_KEY_OR_NETWORK_ERROR`、`INVALID_BACKEND_API_KEY_ERROR`）。

**存储层的校验防的是自写数据。**

`health-storage` 里 `isValidEntry` 的注释：

> localStorage 是用户可写的。拒绝任何超出我们自己写入器产生的范围的东西——一个被篡改的 `-1` 永远不会到达上限，会破坏整个禁用机制；而一个巨大的值只会白白弄乱 UI。

所以校验同时检查：`consecutiveFailures` 是整数且在 `0..MAX` 之间、`lastError` 是字符串或 `null`、`lastFailureAt` 是数字或 `null`、`disabled` 是布尔。任一不合规，该条目被**逐条丢弃**（`readStoredHealth` 遍历并只保留合规项），而不是整个 map 作废。

`writeStoredHealth` 还有一条对称的优化：map 为空时**删掉整个键**而不是写 `"{}"`，保持存储干净。

## 关键取舍

**源码事实：** 计数被 `Math.min` 夹在 `MAX_CONSECUTIVE_FAILURES`，`disabled` 具粘性（`prev?.disabled === true ||`），所以一旦禁用不会因计数变化自己解开。

**源码事实：** 健康状态在模块初始化时就 `readStoredHealth()`，因此「全新页面加载」也带着上一会话的禁用结论；这是整套机制的主要目的。

**源码事实：** `recordBackendSuccess`、`resetBackendHealth`、`dropBackendHealth` 三者实现完全相同，保留三个名字是为了让调用点自解释（注释明确承认这一点）。

**源码事实：** 三个清除函数都以 `if (!(id in healthMap)) return` 提前返回，避免无变化的写入与全员通知。

**源码事实：** 「未知不算不健康」由 `getBackendHealthEntry(id)?.disabled !== true` 表达：没有条目（从未探测）与 `false` 等价。

**源码事实：** 消费侧把 `disabled` 折回 `isConnected: false`，使既有 UI 组件不需要知道新字段；同时保留三条重新探活的路（显式一次性重探、陈旧 CORS/网络错误自动重探、以及编辑配置后的 `resetBackendHealth`）。

**源码事实：** 持久化的 `lastError` 是一句面向用户的文案而非原始异常，因为它被当成可判定的状态来读（`isCorsOrNetworkErrorMessage`）。

**架构推论：** 「要不要继续问」与「这一次通不通」分开建模，是把一个无限重试的循环变成一个有限策略。若只有瞬时结果，失败就会无限重复；若只有持久策略，界面就得不到当前状态。两者都在这里：瞬时结果在 React Query 的 `isSuccess`/`isError` 里，持久策略在这个 store 里。

**架构推论：** 把持久层与运行时层分成两个文件（`health-storage` 与 `health-store`），使校验与迁移逻辑集中在一个不涉及 React、不涉及通知的地方。`health-store` 因此可以保持极薄：它只负责「换引用、写存储、通知」与三种转移。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/backend-registry/health-store.ts` | `getHealthSnapshot` | 整个健康 map，供 `useSyncExternalStore` |
| `src/api/backend-registry/health-store.ts` | `getBackendHealthEntry` | 单条或 `null`；`!== true` 的判据靠它 |
| `src/api/backend-registry/health-store.ts` | `subscribeBackendHealth` | 订阅变更，返回退订函数 |
| `src/api/backend-registry/health-store.ts` | `recordBackendFailure` | 计数自增（夹上限）、粘性禁用、截断错误文案 |
| `src/api/backend-registry/health-store.ts` | `recordBackendSuccess` | 清除条目，让下一次探活重新判定健康 |
| `src/api/backend-registry/health-store.ts` | `dropBackendHealth` | 后端删除时丢弃条目 |
| `src/api/backend-registry/health-store.ts` | `commit` | 唯一写入口：换引用、写存储、通知 |
| `src/api/backend-registry/health-storage.ts` | `MAX_CONSECUTIVE_FAILURES` | 禁用阈值，也是计数夹紧的上界 |
| `src/api/backend-registry/health-storage.ts` | `isValidEntry` | 逐条校验自写存储，防篡改值破坏禁用机制 |

## 参考

- [OpenHands 源码：backend-registry/health-store.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/backend-registry/health-store.ts)
- [React 文档：useSyncExternalStore](https://react.dev/reference/react/useSyncExternalStore)
- [TanStack Query：查询的启用与轮询](https://tanstack.com/query/latest/docs/framework/react/guides/query-options)
- [MDN：Web Storage API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Storage_API)

## 与主核 / AI 的关系

「失败计数 + 上限 + 粘性禁用」在主核里对应的是熔断器：一个下游节点连续失败 N 次后就停止向它发请求，直到有人干预。主核里它通常是进程内的、按连接或按节点的；这里的差别是**状态必须跨会话存活**，于是要落到 localStorage，并且必须假设它可能被篡改。

对 AI 工程的实际意义有三条。第一，**「多少次之后停止」必须是可持久化的事实，而不是运行时的猜测**。没有这一步，刷新页面就等于重置熔断，用户会看到永不停歇的红灯与后台请求噪音——这在长会话产品里是真实的资源消耗。

第二，**「未知」不等于「不健康」**。`?.disabled !== true` 这个判据让从未探测过的后端算健康，从而能被选为回退目标。反过来（要求 `disabled === false`）会让首屏无后端可用，因为那时还没有任何探测结果。

第三，**持久化的错误信息应当是稳定文案而不是原始异常**。这里存的是 `getUserFacingConnectionErrorMessage` 的输出，于是消费侧可以用字符串相等判定「这是网络类错误，值得再试一次」。如果存的是 `error.message` 原文，每次的措辞都可能不同，判定就无从做起。这一条在客户端与服务端都常见：**凡是要在最持久的地方做判断，就得先保证存下去的值是可判定的。**
